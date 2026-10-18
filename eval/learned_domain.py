#!/usr/bin/env python3
"""One domain with several learned families in place of their hand-written
methods, and the checks that it works as a whole (LLM_RTL_code_generation.md
8.6.16).

Each family was learned with only its own methods taken out. Put together,
the learned methods of one family must leave the other's problems alone,
and every family the domain still has by hand must plan as it did.

    python eval/learned_domain.py \\
        wrap_counter=eval/runs/steps_lift/wrap_counter_v3/answer.hddl \\
        table_fsm=eval/runs/steps_lift/table_fsm/answer.hddl \\
        --out eval/runs/steps_lift/combined

Each FAMILY=FILE names a family and the learned methods for it (an
answer.hddl that lift.py or lift_loops.py wrote). The domain is the full
hand-written one less the methods only those families use, with the learned
methods added. Checks, in order:

 1. Every training and held-out problem of each learned family plans, the
    same under two seeds, and simulates correctly.
 2. The problems of each learned family from outside the generator (the
    worked example and the pilot's problem files) plan.
 3. Every problem of the other families gets the plan the full hand-written
    domain gives it.

The domain is written to OUT/domain.hddl and the verdict to OUT/verdict.json.
"""

import argparse
import collections
import json
import os
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import learn_family as lf  # noqa: E402
import rtl_eval  # noqa: E402


def build(learned):
    """The domain text: the full domain less the methods only these
    families use, and the learned forms added."""
    methods, tasks = set(), set()
    for family in learned:
        m, t = lf.removed(family)
        methods |= m
        tasks |= t
    body = lf.strip_comments(lf.without(open(lf.DOMAIN).read(), methods, tasks))
    reduced = "; RTL design domain. A plan is an ordered list of primitive actions.\n" + body
    forms = []
    for family, path in learned.items():
        f, why = lf.answer_forms("```hddl\n" + open(path).read() + "\n```")
        if f is None:
            sys.exit(f"error: {path}: {why}")
        forms += f
    names = [lf.top_forms("(define\n" + x + "\n)")[0][3] for x in forms]
    clash = [n for n, k in collections.Counter(names).items() if k > 1]
    if clash:
        sys.exit(f"error: the learned families both define {', '.join(clash)}")
    return lf.with_answer(reduced, forms), sorted(methods), sorted(tasks)


def check(domain_path, learned, tools):
    out = {}
    with ThreadPoolExecutor(8) as pool:
        for family in learned:
            train, held = lf.split(family)
            rows = list(pool.map(lambda c: lf.try_case(domain_path, c, tools, seeds=(1, 2)), train + held))
            stages = collections.Counter(s for s, _ in rows)
            failed = [c["tag"] for c, (s, _) in zip(train + held, rows) if s != "pass"]
            out[family] = {"generated": len(rows), "passed": stages["pass"], "stages": dict(stages),
                           "failed": failed[:10]}
            print(f"{family}: {stages['pass']} of {len(rows)} training and held-out problems pass"
                  + (f"; failed: {', '.join(failed[:5])} {dict(stages)}" if failed else ""))
        tags = {c["tag"] for f in learned for c in lf.split(f)[0]}
        probes = lf.slice_domain()["probes"]
        for family in learned:
            rows = [r for r in probes if r["kind"] == family and r["name"] not in tags]
            got = list(pool.map(lambda r: lf.plan(domain_path, r["problem"])[0], rows))
            planned = sum(s is not None for s in got)
            out[family]["outside"] = {"problems": len(rows), "planned": planned,
                                      "unplanned": [r["name"] for r, s in zip(rows, got) if s is None]}
            print(f"{family}: {planned} of {len(rows)} problems from outside the generator plan")
        rows = [r for r in probes if r["kind"] not in learned]
        got = list(pool.map(lambda r: lf.plan(domain_path, r["problem"])[0], rows))
        changed = [r["name"] for r, s in zip(rows, got)
                   if s is None or [[n, list(a)] for n, a in s] != r["steps"]]
        out["other_families"] = {"problems": len(rows), "unchanged": len(rows) - len(changed),
                                 "changed": changed,
                                 "families": sorted({r["kind"] for r in rows})}
        print(f"other families ({', '.join(out['other_families']['families'])}): "
              f"{len(rows) - len(changed)} of {len(rows)} problems keep their plan"
              + (f"; changed: {', '.join(changed[:5])}" if changed else ""))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("learned", nargs="+", metavar="FAMILY=FILE")
    ap.add_argument("--out", required=True)
    ap.add_argument("--config", default=os.path.join(HERE, "eval_config.json"))
    args = ap.parse_args()
    _, tools = rtl_eval.load_config(os.path.abspath(args.config))
    learned = {}
    for item in args.learned:
        family, _, path = item.partition("=")
        if family not in lf.FAMILIES or not os.path.exists(path):
            sys.exit(f"error: {item}: expected FAMILY=FILE with a known family and an existing file")
        learned[family] = path
    text, methods, tasks = build(learned)
    os.makedirs(args.out, exist_ok=True)
    domain_path = os.path.join(args.out, "domain.hddl")
    with open(domain_path, "w") as f:
        f.write(text)
    print(f"taken out: {len(methods)} hand-written methods ({', '.join(methods)}) "
          f"and {len(tasks)} tasks ({', '.join(tasks)})")
    verdict = {"learned": learned, "removed_methods": methods, "removed_tasks": tasks,
               "domain_sha256": rtl_eval.sha256_file(domain_path)}
    verdict.update(check(domain_path, learned, tools))
    with open(os.path.join(args.out, "verdict.json"), "w") as f:
        json.dump(verdict, f, indent=1)


if __name__ == "__main__":
    main()
