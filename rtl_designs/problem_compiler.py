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
  3. The sequences of a sequence recogniser are expanded into its states. The
     file says only which bit sequences are recognised, each as an object
     named for its bits, (recognises m flag seq01111110). A state is needed
     for every distinct prefix of them, and the states are related by which
     prefix extends which. That is bookkeeping on the description's own
     bits; where a state goes when its sequences break off is the planner's
     to derive (rtl_domain.hddl, family 4).
  4. The state encoding is computed (state_encoding.py): state k in
     declaration order has code k, in the smallest width that holds them all.
     Anything the file says about codes or widths of states is replaced.

Nothing here guesses. Each step either follows from the file or is left
alone: with two unassigned input ports, no data input is named. What it
cannot express it refuses: an output that would be asserted in two states is
Invalid, since the domain decodes each output from one.

    python rtl_designs/problem_compiler.py PROBLEM.hddl     # compiled, to stdout

The file is re-emitted in the layout the hand-written problems have, without
its comments. To add only the state encoding to a hand-written file and keep
its comments, use state_encoding.py.
"""

import re
import sys

import state_encoding

FSM_KINDS = ("sequence_detector", "table_fsm", "sequence_recogniser")
NAMED_TYPES = ((re.compile(r"n\d+$"), "value"), (re.compile(r"bits\d+$"), "width"),
               (re.compile(r"seq[01]+$"), "bitseq"))
BIT = {"0": "zero", "1": "one"}
# What the expansion of a recogniser's sequences writes. A file that states
# any of these for a recogniser has them replaced.
EXPANDED = ("sequence_start", "child", "no_child", "holds_on", "first_listed", "listed_after",
            "last_listed", "initial_state", "asserted_in")


def state_names(prefixes, recognised):
    """A name for the state of each prefix: short, and unlike the others.

    A state in which a sequence has just completed is named for its output,
    S_FLAG. Any other is named for how many bits are matched, M3, with a
    letter added where two prefixes of one length have to be told apart.

    The names were once the prefixes themselves, S_0111110 beside S_0111111.
    The executor mixed those up: with them it wrote fsm_hdlc correctly in 12
    replies of 20, swapping the branches of the states whose names differ in
    one digit, and it reset into the state then called S_START, which reset
    never enters (LLM_RTL_code_generation.md 8.6.7)."""
    names = {}
    by_length = {}
    for x in prefixes:
        outs = [out for out, bits in recognised if bits == x]
        if outs:
            names[x] = "S_" + "_".join(o.upper() for o in outs)
        else:
            by_length.setdefault(len(x), []).append(x)
    for length, group in by_length.items():
        for i, x in enumerate(group):
            names[x] = f"M{length}" + (chr(ord("a") + i) if len(group) > 1 else "")
    if len(set(names.values())) != len(names):
        raise Invalid("two states of the recogniser would share a name")
    return names


def expand_sequences(problem, declared, notes):
    """Step 3: the states of a sequence recogniser, from its (recognises ...)
    facts. One state per distinct prefix, listed shortest first."""
    facts = problem["facts"]
    module = problem["module"]
    recognised = [(f[2], f[3][3:]) for f in facts if f[0] == "recognises" and len(f) == 4
                  and re.fullmatch(r"seq[01]+", f[3])]
    if not recognised:
        return
    for kind in ("sequence_recogniser", "moore"):
        if [kind, module] not in facts:
            facts.append([kind, module])
            notes.append(f"added ({kind} {module}): it recognises sequences")
    dropped = [f for f in facts if f[0] in EXPANDED]
    if dropped:
        facts[:] = [f for f in facts if f[0] not in EXPANDED]
        notes.append(f"dropped {len(dropped)} state facts the file wrote: the states are derived from the sequences")
    written_states = [n for names, t in problem["objects"] if t == "fsm_state" for n in names]
    if written_states:
        problem["objects"][:] = [(n, t) for n, t in problem["objects"] if t != "fsm_state"]
        declared.difference_update(written_states)

    prefixes = sorted({bits[:k] for _, bits in recognised for k in range(len(bits) + 1)},
                      key=lambda x: (len(x), x))
    names = state_names(prefixes, recognised)
    state_name = names.__getitem__
    states = [state_name(x) for x in prefixes]
    problem["objects"].append((states, "fsm_state"))
    declared.update(states)
    facts.append(["sequence_start", module, state_name("")])
    for x in prefixes:
        for b in "01":
            if x + b in prefixes:
                facts.append(["child", state_name(x), BIT[b], state_name(x + b)])
            else:
                facts.append(["no_child", state_name(x), BIT[b]])
    facts.append(["first_listed", module, states[0]])
    facts += [["listed_after", a, b] for a, b in zip(states, states[1:])]
    facts.append(["last_listed", module, states[-1]])

    # Each output is 1 in the state where its sequence has just completed. The
    # domain decodes an output from one state, so a sequence that also
    # completes inside a longer one cannot be expressed.
    for out, bits in recognised:
        ends = [x for x in prefixes if x.endswith(bits)]
        if len(ends) != 1:
            raise Invalid(f"output {out} would be asserted in {len(ends)} states: sequence {bits} also "
                          f"completes inside another, which the domain cannot express")
        facts.append(["asserted_in", out, state_name(bits)])
    for f in [f for f in facts if f[0] == "keeps_while" and len(f) == 3]:
        ends = [bits for out, bits in recognised if out == f[1]]
        if not ends or f[2] not in BIT.values():
            raise Invalid(f"({' '.join(f)}) names no recognised output and bit value")
        for bits in ends:
            facts.append(["holds_on", state_name(bits), f[2]])

    # After reset: nothing matched, unless the file says the machine starts as
    # though a bit had just arrived.
    start = ""
    previous = [f for f in facts if f[0] == "reset_as_previous" and len(f) == 3]
    if previous:
        bit = {v: k for k, v in BIT.items()}.get(previous[0][2])
        if bit is None or bit not in prefixes:
            raise Invalid(f"({' '.join(previous[0])}): no sequence begins with that bit")
        start = bit
    facts.append(["initial_state", module, state_name(start)])
    notes.append(f"expanded {len(recognised)} sequences into {len(states)} states")



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

    # 4, first half: whatever the file says about state codes is not kept.
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

    # 3. The states of a sequence recogniser.
    expand_sequences(problem, declared, notes)

    # 4. The state encoding.
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
