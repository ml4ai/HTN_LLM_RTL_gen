#!/usr/bin/env python3
"""Value estimates for competing menu entries, by rollouts: cleaner labels
for the scorer than whole lists' outcomes (HRL_notes.md §5.1).

    python eval/rollouts.py mealy_detector --runs-dir eval/runs/steps_r60_t085
    python eval/rollouts.py mealy_detector --runs-dir eval/runs/steps_r60_t085 --dry-run

From the lists of a run made with the current menus:

 1. Every state a list passed through: a training problem and the steps
    taken so far.
 2. The model's probability of each entry there (executor_score).
 3. The states where the model is unsure, its likeliest entry under
    --unsure (default 0.9), at most --states of them a problem, earliest
    first: there the choice is open, and there a scorer could change it.
 4. At each, for each of its --top likeliest entries (default 3), the list
    is finished --rollouts times (default 4) from that entry by the model,
    sampled under the configuration given (default the executor's, T 0.85,
    the evaluation temperature, HRL_notes.md §4.1), with the current menus.

An entry's value is the mean graded reward of its rollouts
(learn_family.graded: 1 for a list that passes, the balanced accuracy of a
module that simulates wrong, 0 for no module): an estimate of Q(s, a) under
the model's own policy. Graded, because where no list passes, as on the
detector family, pass rates would be 0 everywhere. The output has menu_data.py's form,
pass and fail counts now from rollouts, and each entry also carries the
model's log-probability, so scorer.py trains on it as on menu_data's.
Everything is seeded and cached by request, so a run can be resumed.
"""

import argparse
import collections
import glob
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import learn_family as lf  # noqa: E402
import learn_ground as lg  # noqa: E402
import learn_steps as ls  # noqa: E402
import menu_data  # noqa: E402
import rtl_eval  # noqa: E402

OUT = os.path.join(HERE, "runs", "rollouts")


def visited(family, runs_dir, cases):
    """{(tag, steps so far)}: every state the run's lists passed through."""
    out = set()
    for path in sorted(glob.glob(os.path.join(runs_dir, family, "*", "run_*", "round_*", "verdict.json"))):
        tag = path.split(os.sep)[-4]
        if tag not in cases:
            continue
        steps, _ = lg.answer_steps("```\n" + open(os.path.join(os.path.dirname(path), "steps.txt")).read() + "```")
        for t in range(len(steps)):
            out.add((tag, tuple((n, tuple(a)) for n, a in steps[:t])))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("family", choices=sorted(lf.FAMILIES))
    ap.add_argument("--runs-dir", required=True, help="a run made with the current menus (eval/runs/steps_TAG)")
    ap.add_argument("--unsure", type=float, default=0.9)
    ap.add_argument("--states", type=int, default=6, help="states a problem, at most")
    ap.add_argument("--top", type=int, default=3)
    ap.add_argument("--rollouts", type=int, default=4)
    ap.add_argument("--config", default=os.path.join(HERE, "eval_config.json"))
    ap.add_argument("--proposer-config", help="how the model is run (default: the executor's configuration)")
    ap.add_argument("--dry-run", action="store_true", help="choose the states and stop before the rollouts")
    args = ap.parse_args()
    lf.PROPOSER_CONFIG = args.proposer_config
    cfg, tools = rtl_eval.load_config(os.path.abspath(args.config))
    train, _ = lf.split(args.family)
    cases = {c["tag"]: i for i, c in enumerate(train)}
    base = os.path.join(OUT, args.family)
    problems = [ls.Problem(args.family, c, os.path.join(base, c["tag"])) for c in train]

    # 2. The model's probabilities at every state the lists visited.
    states = sorted(visited(args.family, args.runs_dir, cases), key=lambda s: (s[0], len(s[1])))
    info, jobs = {}, []
    for tag, done in states:
        p = problems[cases[tag]]
        done = [(n, list(a)) for n, a in done]
        menu = p.menu(done)
        if len(menu) < 2:
            continue
        d, job = ls.score_request(base, tag, len(done) + 1, p.prompt(done), len(menu))
        info[(tag, len(done), lg.show(done))] = (done, menu, d)
        jobs += [job] if job else []
    lf.scorer_executor(cfg, jobs, os.path.join(base, "score_batch.json"))

    # 3. The states where the model is unsure.
    chosen = collections.defaultdict(list)
    for key, (done, menu, d) in sorted(info.items()):
        lp = [x["logprob"] for x in json.load(open(os.path.join(d, "scores.json")))]
        if max(math.exp(x) for x in lp) < args.unsure and len(chosen[key[0]]) < args.states:
            top = sorted(range(len(lp)), key=lambda i: -lp[i])[:args.top]
            chosen[key[0]].append((done, menu, lp, top))
    n_states = sum(len(v) for v in chosen.values())
    print(f"{len(info)} states with a choice; {n_states} where the model's likeliest entry is under {args.unsure}; "
          f"{n_states * args.top * args.rollouts} rollouts to run")
    if args.dry_run:
        return

    # 4. Rollouts from each of the likeliest entries.
    keys, starts, where = [], {}, {}
    for tag, items in sorted(chosen.items()):
        p = cases[tag]
        for s, (done, menu, lp, top) in enumerate(items):
            for i in top:
                j = len(where)
                where[j] = (tag, s, i)
                for r in range(args.rollouts):
                    k = (p, r, j)
                    keys.append(k)
                    starts[k] = done + [menu[i]["step"]]
    _, verdicts, _, _, _ = ls.build_lists(problems, train, keys, base, cfg, tools, "model", args.rollouts,
                                          starts=starts)

    # The data, in menu_data's form.
    out_states = []
    for tag, items in sorted(chosen.items()):
        usable = len(problems[cases[tag]].unused_facts([]))
        for s, (done, menu, lp, top) in enumerate(items):
            entries = []
            for i in top:
                j = next(j for j, w in where.items() if w == (tag, s, i))
                passed = sum(verdicts[(cases[tag], r, j)]["stage"] == "pass" for r in range(args.rollouts))
                value = sum(verdicts[(cases[tag], r, j)].get("reward", 0.0) for r in range(args.rollouts)) / args.rollouts
                feats = menu_data.features(problems[cases[tag]], done, menu[i]["step"], usable)
                entries.append({"step": [menu[i]["step"][0], list(menu[i]["step"][1])], "pass": passed,
                                "fail": args.rollouts - passed, "value": value, "features": feats, "logprob": lp[i]})
            out_states.append({"tag": tag, "done": done, "entries": entries})
    data = {"family": args.family, "features": menu_data.FEATURES, "states": out_states,
            "source": {"runs_dir": os.path.abspath(args.runs_dir), "unsure": args.unsure, "top": args.top,
                       "rollouts": args.rollouts, "proposer_config": lf.proposer_config(cfg)},
            "lists": {"distinct": len(keys), "passing": sum(v["stage"] == "pass" for v in verdicts.values()),
                      "stages": dict(collections.Counter(v["stage"] for v in verdicts.values())), "sources": []},
            "entries_dropped_by_current_rules": 0}
    path = os.path.join(OUT, f"{args.family}.json")
    with open(path, "w") as f:
        json.dump(data, f)
    menu_data.summary(data)
    print(f"  written to {path}")


if __name__ == "__main__":
    main()
