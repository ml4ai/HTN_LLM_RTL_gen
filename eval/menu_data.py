#!/usr/bin/env python3
"""Menu states and their outcomes, from every logged step-list run: the
training data for a scorer over menu entries (HRL_notes.md §5).

    python eval/menu_data.py mealy_detector            # writes eval/runs/menu_data/mealy_detector.json
    python eval/menu_data.py table_fsm --summary       # only counts, nothing written

Every distinct list of every run of a family's training problems is pooled,
whatever the menu rules of its run (eval/runs/steps*/FAMILY). A state is a
problem and the steps taken so far; at each state the data records each
entry some list took there, how many distinct lists through it passed and
how many failed, and the entry's features. Two entries at the same state
with different pass rates make a preference pair, the better preferred.

An entry is kept only if the current menus could offer it: its code stands
under the current rules (learn_steps.Problem.code). A list's outcome is its
verdict: it passed, or it failed in any way. A list that ended because its
own run's menus ran out may have gone on under the current ones; that is
noise in the labels, and counted in the summary.
"""

import argparse
import collections
import glob
import json
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import learn_family as lf  # noqa: E402
import learn_ground as lg  # noqa: E402
import learn_steps as ls  # noqa: E402

OUT = os.path.join(HERE, "runs", "menu_data")

# Each feature is about the entry as the menu shows it, and none names a
# family, a step or a value of the design.
FEATURES = [
    "decision",        # records a decision: no code
    "task",            # one of the domain's tasks, kept whole
    "actions",         # how many actions it comes to (log)
    "lines",           # lines of code it adds (log)
    "closer_only",     # it only closes blocks
    "opens",           # it opens a block
    "ends_module",     # it writes endmodule
    "edits_earlier",   # its code changes a line written before, not only adds at the end
    "uses_unused",     # facts of the design no step has used that it uses (log)
    "unused_left",     # the fraction of the design's usable facts still unused after it
    "needed_for",      # code steps a decision it records is needed for (log)
    "new_facts",       # facts it records (log)
    "repeat",          # the same step name as the last step
    "depth",           # blocks open after it
    "progress",        # steps taken so far, over the turn limit
]


def runs_of(family):
    """(problem tag, steps, passed, stage, run directory) for every list."""
    out = []
    for path in sorted(glob.glob(os.path.join(HERE, "runs", "steps*", family, "*", "run_*", "round_*",
                                              "verdict.json"))):
        stage = json.load(open(path))["stage"]
        steps, _ = lg.answer_steps("```\n" + open(os.path.join(os.path.dirname(path), "steps.txt")).read() + "```")
        out.append((path.split(os.sep)[-4], [(n, list(a)) for n, a in steps], stage == "pass", stage,
                    path.split(os.sep)[-6]))
    return out


def depth(lines):
    d = 0
    for line in lines:
        text = line.strip()
        d += len(re.findall(r"\b(begin|case)\b", text)) - len(re.findall(r"\b(end|endcase)\b", text))
    return d


def features(problem, done, step, usable):
    """The entry `step` would be after `done`, and its features; None if it
    does not plan or its code would not stand now."""
    before, after = problem.plan(done), problem.plan(list(done) + [step])
    if before is None or after is None or after[:len(before)] != before:
        return None
    lines, new_lines = problem.code(before) or [], problem.code(after)
    if new_lines is None:
        return None
    adds = new_lines[len(lines):] if new_lines[:len(lines)] == lines else \
        [x for x in new_lines if x not in lines]
    state_before, state_after = problem.state(before), problem.state(after)
    new_facts = sorted(state_after - state_before)
    unused_before = set(problem.unused_facts(list(done)))
    unused_after = set(problem.unused_facts(list(done) + [step]))
    entry = {"step": step, "kind": problem.known[step[0]][0], "plan": after, "adds": adds, "facts": new_facts}
    decision = step[0].startswith("analyze_") and entry["kind"] != "task"
    f = {
        "decision": float(decision),
        "task": float(entry["kind"] == "task"),
        "actions": math.log1p(len(after) - len(before)),
        "lines": math.log1p(len(adds)),
        "closer_only": float(bool(adds) and all(ls.CLOSER.match(x.strip()) for x in adds)),
        "opens": float(bool(re.search(r"\b(begin|case)\b", " ".join(adds)))),
        "ends_module": float(bool(new_lines) and new_lines[-1].strip() == "endmodule"),
        "edits_earlier": float(new_lines[:len(lines)] != lines),
        "uses_unused": math.log1p(len(unused_before - unused_after)),
        "unused_left": len(unused_after) / max(1, usable),
        "needed_for": math.log1p(len(problem.needed_for(list(done), entry))) if decision else 0.0,
        "new_facts": math.log1p(len(new_facts)),
        "repeat": float(bool(done) and done[-1][0] == step[0]),
        "depth": float(depth(new_lines)),
        "progress": len(done) / ls.MAX_TURNS,
    }
    return [f[k] for k in FEATURES]


def build(family, workdir):
    train, _ = lf.split(family)
    cases = {c["tag"]: c for c in train}
    distinct = {}
    for tag, steps, passed, stage, source in runs_of(family):
        if tag in cases:
            distinct.setdefault((tag, lg.show(steps)), (tag, steps, passed, stage, set()))[4].add(source)
    stats = collections.defaultdict(lambda: collections.defaultdict(lambda: [0, 0]))
    for tag, steps, passed, stage, _ in distinct.values():
        for t in range(len(steps)):
            key = (tag, tuple((n, tuple(a)) for n, a in steps[:t]))
            stats[key][(steps[t][0], tuple(steps[t][1]))][0 if passed else 1] += 1
    problems = {tag: ls.Problem(family, cases[tag], os.path.join(workdir, family, tag)) for tag in cases}
    usable = {tag: len(p.unused_facts([])) for tag, p in problems.items()}
    states, dropped = [], 0
    for (tag, done), entries in sorted(stats.items(), key=lambda kv: (kv[0][0], len(kv[0][1]))):
        done = [(n, list(a)) for n, a in done]
        rows = []
        for (name, args), (p, f) in sorted(entries.items()):
            feats = features(problems[tag], done, (name, list(args)), usable[tag])
            if feats is None:
                dropped += 1
                continue
            rows.append({"step": [name, list(args)], "pass": p, "fail": f, "features": feats})
        if rows:
            states.append({"tag": tag, "done": done, "entries": rows})
    lists = list(distinct.values())
    return {"family": family, "features": FEATURES, "states": states,
            "lists": {"distinct": len(lists), "passing": sum(x[2] for x in lists),
                      "stages": dict(collections.Counter(x[3] for x in lists)),
                      "sources": sorted({s for x in lists for s in x[4]})},
            "entries_dropped_by_current_rules": dropped}


def pairs(data):
    """(state index, better entry, worse entry) for every two entries at a
    state whose lists' pass rates differ."""
    out = []
    for i, s in enumerate(data["states"]):
        rate = [e["pass"] / (e["pass"] + e["fail"]) for e in s["entries"]]
        for a in range(len(rate)):
            for b in range(len(rate)):
                if rate[a] > rate[b]:
                    out.append((i, a, b))
    return out


def summary(data):
    branching = [s for s in data["states"] if len(s["entries"]) > 1]
    print(f"{data['family']}: {data['lists']['distinct']} distinct lists ({data['lists']['passing']} pass), "
          f"stages {data['lists']['stages']}")
    print(f"  from {len(data['lists']['sources'])} runs: {', '.join(data['lists']['sources'])}")
    print(f"  {len(data['states'])} states, {len(branching)} with more than one entry taken; "
          f"{len(pairs(data))} preference pairs; {data['entries_dropped_by_current_rules']} entries the current "
          f"rules would not offer, left out")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("family", choices=sorted(lf.FAMILIES))
    ap.add_argument("--summary", action="store_true", help="print counts only")
    args = ap.parse_args()
    data = build(args.family, os.path.join(OUT, "work"))
    summary(data)
    if not args.summary:
        os.makedirs(OUT, exist_ok=True)
        with open(os.path.join(OUT, f"{args.family}.json"), "w") as f:
            json.dump(data, f)
        print(f"  written to {os.path.join(OUT, args.family + '.json')}")


if __name__ == "__main__":
    main()
