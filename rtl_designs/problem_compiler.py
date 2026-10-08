"""The problem compiler: everything about a problem file that can be computed,
done after it is written and before the planner reads it.

A problem file restates a design description as facts. Some of what the
planner needs is not in the description at all, or follows from the rest, and
asking whoever writes the file for it only gives them something to get wrong.
In the translation test (LLM_RTL_code_generation.md 8.6.7) a model wrote the
files, and 31 of its 80 failing translations had every fact a person would
have to supply: they lacked only what is listed here.

  1. Numbers and widths are declared. An object named nN is a value and one
     named bitsN a width, so a use is as good as a declaration.
  2. The data input of a state machine is named, when it is the one input
     port that is neither the clock nor the reset.
  3. The state encoding is computed (state_encoding.py): state k in
     declaration order has code k, in the smallest width that holds them all.
     Anything the file says about codes or widths of states is replaced.

Nothing here guesses. Each step either follows from the file or is left
alone: with two unassigned input ports, no data input is named.

    python rtl_designs/problem_compiler.py PROBLEM.hddl     # compiled, to stdout

The file is re-emitted in the layout the hand-written problems have, without
its comments. To add only the state encoding to a hand-written file and keep
its comments, use state_encoding.py.
"""

import re
import sys

import state_encoding

FSM_KINDS = ("sequence_detector", "table_fsm", "run_classifier")
NAMED_TYPES = ((re.compile(r"n\d+$"), "value"), (re.compile(r"bits\d+$"), "width"))


class Unreadable(ValueError):
    """The text is not a problem file: no (define ...), unbalanced, a section missing."""


class Invalid(ValueError):
    """It reads as a problem file and is wrong before the planner sees it."""


def parse_sexpr(text):
    """The first complete s-expression in `text`, as nested lists of tokens."""
    tokens = re.findall(r"\(|\)|[^\s()]+", re.sub(r";[^\n]*", "", text))
    pos = 0

    def read():
        nonlocal pos
        if pos >= len(tokens):
            raise Unreadable("unbalanced parentheses")
        tok = tokens[pos]
        pos += 1
        if tok == "(":
            out = []
            while pos < len(tokens) and tokens[pos] != ")":
                out.append(read())
            if pos >= len(tokens):
                raise Unreadable("unbalanced parentheses")
            pos += 1
            return out
        if tok == ")":
            raise Unreadable("unbalanced parentheses")
        return tok
    return read()


def read(text):
    """A problem as {"name", "module", "objects": [(names, type)], "facts": [[predicate, args...]]}."""
    tree = parse_sexpr(text)
    if not (isinstance(tree, list) and tree and tree[0] == "define"):
        raise Unreadable("not a (define ...)")
    sections = {s[0]: s for s in tree[1:] if isinstance(s, list) and s and isinstance(s[0], str)}
    for need in ("problem", ":objects", ":htn", ":init"):
        if need not in sections:
            raise Unreadable(f"no ({need} ...)")
    objects, pending = [], []
    items = sections[":objects"][1:]
    i = 0
    while i < len(items):
        if items[i] == "-" and i + 1 < len(items):
            objects.append((pending, items[i + 1]))
            pending = []
            i += 2
        else:
            if not isinstance(items[i], str):
                raise Unreadable("malformed (:objects ...)")
            pending.append(items[i])
            i += 1
    if pending:
        raise Unreadable("an object without a type")

    def find_task(node):
        if isinstance(node, list):
            if len(node) == 2 and node[0] == "implement_module" and isinstance(node[1], str):
                return node[1]
            for child in node:
                found = find_task(child)
                if found:
                    return found
        return None
    module = find_task(sections[":htn"])
    if not module:
        raise Unreadable("no (implement_module ...) task")
    facts = [f for f in sections[":init"][1:] if isinstance(f, list) and f and all(isinstance(x, str) for x in f)]
    name = sections["problem"][1] if len(sections["problem"]) > 1 else "problem"
    return {"name": name, "module": module, "objects": [(n, t) for n, t in objects if n], "facts": facts}


def emit(problem):
    lines = [f"(define (problem {problem['name']})", "  (:domain rtl_fsm)", "  (:objects"]
    lines += [f"    {' '.join(names)} - {type_}" for names, type_ in problem["objects"]]
    lines += ["  )", "  (:htn", "    :parameters ()",
              f"    :subtasks (and (implement_module {problem['module']}))", "  )", "  (:init"]
    lines += [f"    ({' '.join(f)})" for f in problem["facts"]]
    lines += ["  )", ")"]
    return "\n".join(lines) + "\n"


def complete(problem):
    """Apply the three steps to `problem` in place. Returns what was done, as
    a list of short notes."""
    notes = []
    facts = problem["facts"]
    declared = {n for names, _ in problem["objects"] for n in names}

    # 3, first half: whatever the file says about state codes is not kept.
    written = [f for f in facts if f[0] in ("state_code", "state_width")]
    if written:
        facts[:] = [f for f in facts if f[0] not in ("state_code", "state_width")]
        notes.append(f"dropped {len(written)} state-encoding facts the file wrote")

    # 1. Numbers and widths used in a fact are declared.
    for pattern, type_ in NAMED_TYPES:
        missing = sorted({a for f in facts for a in f[1:] if pattern.match(a)} - declared,
                         key=lambda x: int(re.search(r"\d+", x).group()))
        if missing:
            problem["objects"].append((missing, type_))
            declared.update(missing)
            notes.append(f"declared {' '.join(missing)} as {type_}")

    # 2. The data input of a state machine, when only one port can be it.
    kinds = {f[0] for f in facts}
    if kinds & set(FSM_KINDS) and "data_input_of" not in kinds:
        inputs = [f[1] for f in facts if f[0] == "input_port" and len(f) == 2]
        taken = {f[2] for f in facts if f[0] in ("clock_of", "reset_of") and len(f) == 3}
        free = [p for p in inputs if p not in taken]
        if len(free) == 1:
            facts.append(["data_input_of", problem["module"], free[0]])
            notes.append(f"named {free[0]} the data input: the only input that is not the clock or the reset")

    # 3. The state encoding.
    try:
        found = state_encoding.states_of(emit(problem))
    except ValueError as e:
        raise Invalid(str(e))
    if found:
        enc_objects, enc_facts = state_encoding.encoding(*found)
        for line in enc_objects:
            names, type_ = line.rsplit(" - ", 1)
            names = [n for n in names.split() if n not in declared]
            if names:
                problem["objects"].append((names, type_))
                declared.update(names)
        facts += [f[1:-1].split() for f in enc_facts]
        notes.append(f"encoded {len(found[1])} states")
    return notes


def compile_text(text):
    """(compiled problem text, its facts, notes on what was computed).
    Raises Unreadable or Invalid."""
    problem = read(text)
    notes = complete(problem)
    return emit(problem), problem["facts"], notes


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    with open(sys.argv[1]) as f:
        source = f.read()
    try:
        compiled, _, done = compile_text(source)
    except ValueError as e:
        sys.exit(f"{sys.argv[1]}: {e}")
    sys.stdout.write(compiled)
    for note in done:
        print("; " + note, file=sys.stderr)
