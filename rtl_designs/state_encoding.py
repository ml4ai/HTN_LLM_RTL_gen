"""The state encoding of an FSM problem, as facts for rtl_domain.hddl.

A plan has to say how wide the state register is and which code each state
gets. Left to the LLM, "just enough bits" came back as three bits for ten
states in a quarter of its replies, with codes reused (LLM_RTL_code_generation.md
8.6.6). Counting is not planning either: HDDL has no arithmetic, and nothing
about the answer is a choice. So it is computed here, for any number of
states, and given to the planner as facts, which the domain threads into the
plan's steps:

    state k, in declaration order, has code k          (state_code S5 n5)
    the width is the smallest that holds the last code  (state_width m bits4)

    python rtl_designs/state_encoding.py PROBLEM.hddl ...   # write them in place

The states' order is read from the problem: the pattern chain of a sequence
detector, (initial_state m s0) then (next_prefix s t), or the listed chain,
(first_listed m s) then (listed_after s t). A problem with no states is left
alone. Running it again replaces what it wrote before.
"""

import re
import sys

MARK = "; state encoding, computed by rtl_designs/state_encoding.py"
GENERATED = re.compile(r"^\s*(\(state_code |\(state_width |n\d+( n\d+)* - value\s*$|bits\d+ - width\s*$)")


def encoding(module, states):
    """(object lines, fact lines) for `states` in declaration order."""
    width = max(1, (len(states) - 1).bit_length())
    objects = [" ".join(f"n{k}" for k in range(len(states))) + " - value", f"bits{width} - width"]
    facts = [f"(state_width {module} bits{width})"] + [f"(state_code {s} n{k})" for k, s in enumerate(states)]
    return objects, facts


def states_of(text):
    """(module, states in declaration order) of a problem, or None. Raises
    ValueError if the chain of states loops back on itself."""
    listed = re.search(r"\(first_listed (\S+) (\S+)\)", text)
    start = listed or re.search(r"\(initial_state (\S+) (\S+)\)", text)
    if not start:
        return None
    link = "listed_after" if listed else "next_prefix"
    after = dict(re.findall(r"\(%s (\S+) (\S+)\)" % link, text))
    module, s = start.group(1), start.group(2)
    states = [s]
    while states[-1] in after:
        states.append(after[states[-1]])
        if len(states) > len(after) + 1:
            raise ValueError("the states form a cycle")
    return module, states


def strip_generated(lines):
    """`lines` without what an earlier run wrote: each marker and the
    generated lines after it."""
    out, skipping = [], False
    for line in lines:
        if line.strip() == MARK:
            skipping = True
            continue
        if skipping and GENERATED.match(line):
            continue
        skipping = False
        out.append(line)
    return out


def rewrite(text):
    found = states_of(text)
    if not found:
        return text
    objects, facts = encoding(*found)
    lines = strip_generated(text.split("\n"))
    # The object block and the :init block each end at the first line that is
    # only a closing parenthesis at their indentation.
    def insert(lines, opener, new):
        i = next(k for k, l in enumerate(lines) if l.strip().startswith(opener))
        indent = len(lines[i]) - len(lines[i].lstrip())
        j = next(k for k in range(i + 1, len(lines)) if lines[k].rstrip() == " " * indent + ")")
        pad = " " * (indent + 2)
        return lines[:j] + [pad + MARK] + [pad + n for n in new] + lines[j:]
    lines = insert(lines, "(:objects", objects)
    lines = insert(lines, "(:init", facts)
    return "\n".join(lines)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for path in sys.argv[1:]:
        with open(path) as f:
            old = f.read()
        try:
            new = rewrite(old)
        except ValueError as e:
            sys.exit(f"{path}: {e}")
        if new != old:
            with open(path, "w") as f:
                f.write(new)
        found = states_of(old)
        print(f"{path}: " + (f"{len(found[1])} states" if found else "no states, unchanged"))
