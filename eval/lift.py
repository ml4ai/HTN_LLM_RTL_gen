#!/usr/bin/env python3
"""Turn checked step lists into methods: the symbolic half of learning a
family (LLM_RTL_code_generation.md 6.2.2, 8.6.13).

learn_ground.py has the model list the steps of (implement_module m) for one
problem at a time, and checks each list by planning, rendering and simulating
it. This file generalises checked lists into HDDL methods, with no model
involved:

  1. Lists with the same sequence of step names are one case, and become one
     method.
  2. Within a case, an argument becomes a variable unless it is one of the
     domain's constants. Two argument places get the same variable when they
     hold the same name in every example.
  3. Each variable is tied to the module by facts of the problem: a fact
     pattern over variables already tied that, in every example, picks out
     exactly the name the variable stands for. Those patterns are the
     method's precondition.
  4. Two cases are told apart by a kind of fact about the module that every
     example of one has and no example of the other has. The second gets its
     absence, (not (exists ...)), in its precondition.

    python eval/lift.py wrap_counter --from hand      # from the hand-written methods' own lists
    python eval/lift.py wrap_counter --from runs      # from the lists the model got right

--from hand uses learn_ground.frontier, the decomposition the hand-written
methods give, cut where the reduced domain takes over. It is for developing
and checking the lifter: it says what the lifter can do with perfect lists.

The methods are then tested exactly as learn_family.py tests a model's: on
the training problems, on the other families' plans, and on the family's
held-out problems.

What this cannot do yet: a case whose length depends on the problem, such as
one step for every state, needs a recursive method. Lists that differ in
length are different cases here, each good only for its own length.
"""

import argparse
import collections
import glob
import itertools
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import learn_family as lf  # noqa: E402
import learn_ground as lg  # noqa: E402
import problem_compiler  # noqa: E402
import rtl_eval  # noqa: E402

CONSTANT_TYPES = {"zero": "bitval", "one": "bitval", "active_high": "polarity", "active_low": "polarity"}
CONSTANTS = set(CONSTANT_TYPES)


class CannotLift(Exception):
    pass


def example(case, steps):
    """One checked list with what the lifter needs of its problem."""
    compiled, facts, _ = problem_compiler.compile_text(case["problem"])
    types = {}
    for line in re.search(r"\(:objects(.*?)\n  \)", compiled, re.S).group(1).splitlines():
        if " - " in line:
            names, type_ = line.rsplit(" - ", 1)
            types.update({n: type_.strip() for n in names.split()})
    types.update(CONSTANT_TYPES)
    module = next(f[1] for f in facts if f[0] == "first_port")
    return {"tag": case["tag"], "steps": steps, "facts": {tuple(f) for f in facts}, "types": types,
            "module": module}


def predicate_types(domain_text):
    """{predicate: [type of each argument]}, from the domain's declarations."""
    block = next(domain_text[s:e] for s, e, kind, _ in lf.top_forms(domain_text) if kind == ":predicates")
    out = {}
    for name, params in re.findall(r"\(\s*([^\s():?]+)((?:\s+\?[^\s()]+|\s+-\s+[^\s()]+)*)\s*\)", block):
        types, pending = [], 0
        tokens = params.split()
        i = 0
        while i < len(tokens):
            if tokens[i] == "-":
                types += [tokens[i + 1]] * pending
                pending, i = 0, i + 2
            else:
                pending, i = pending + 1, i + 1
        out[name] = types + ["object"] * pending
    return out


def lift_case(examples, name):
    """One method from the examples of one case. Returns a dict describing it."""
    steps0 = examples[0]["steps"]
    places = [(i, j) for i, (_, args) in enumerate(steps0) for j in range(len(args))]
    value = {p: tuple(e["steps"][p[0]][1][p[1]] for e in examples) for p in places}

    # Variables: one for each distinct column of values that is not a constant.
    var_of, columns = {}, {}
    module = tuple(e["module"] for e in examples)
    columns[module] = "?m"
    for p in places:
        col = value[p]
        if len(set(col)) == 1 and col[0] in CONSTANTS:
            continue
        if col not in columns:
            columns[col] = f"?v{len(columns)}"
        var_of[p] = columns[col]
    variables = {v: col for col, v in columns.items()}
    types = {}
    for v, col in variables.items():
        found = {e["types"].get(x) for e, x in zip(examples, col)}
        if len(found) != 1 or None in found:
            raise CannotLift(f"{name}: an argument is {sorted(str(t) for t in found)} in different examples")
        types[v] = found.pop()

    # Tie each variable to the module through the problem's facts.
    bound, conditions = {"?m"}, []
    while bound != set(variables):
        best = None
        for fact in sorted(examples[0]["facts"]):
            pred, args = fact[0], fact[1:]
            # Every way of reading this fact's arguments as variables or constants.
            choices = []
            for a in args:
                c = [v for v, col in variables.items() if col[0] == a]
                if a in CONSTANTS:
                    c.append(a)
                choices.append(c)
            for reading in itertools.product(*choices):
                new = [x for x in dict.fromkeys(reading) if x.startswith("?") and x not in bound]
                if not new:
                    continue
                if not all(picks_out(e, k, pred, reading, variables, bound) for k, e in enumerate(examples)):
                    continue
                # Prefer a fact about the module itself to one that goes through another
                # variable: (reset_of ?m ?r) over "the port after the clock", which is
                # true of the examples only because of how they happen to be laid out.
                candidate = (len(new), "?m" not in reading, sum(x.startswith("?") for x in reading), pred, reading)
                if best is None or candidate < best:
                    best = candidate
        if best is None:
            left = sorted(set(variables) - bound)
            raise CannotLift(f"{name}: no fact of the problem ties {', '.join(left)} "
                             f"({', '.join(variables[v][0] for v in left)} in the first example) to the module")
        pred, reading = best[-2:]
        conditions.append((pred,) + tuple(reading))
        bound |= {x for x in reading if x.startswith("?")}

    # Facts about the module alone that every example has: its kind.
    for fact in sorted(examples[0]["facts"]):
        if len(fact) == 2 and fact[1] == examples[0]["module"] and all((fact[0], e["module"]) in e["facts"]
                                                                     for e in examples):
            conditions.insert(0, (fact[0], "?m"))
    subtasks = [(n, [var_of.get((i, j), a) for j, a in enumerate(args)]) for i, (n, args) in enumerate(steps0)]
    return {"name": name, "types": types, "conditions": conditions, "subtasks": subtasks, "absent": [],
            "examples": examples}


def picks_out(e, k, pred, reading, variables, bound):
    """In example e (number k), does the pattern, with the bound variables
    filled in, match exactly one fact, and is it the one the examples hold?"""
    matches = []
    for fact in e["facts"]:
        if fact[0] != pred or len(fact) - 1 != len(reading):
            continue
        ok, seen = True, {}
        for a, r in zip(fact[1:], reading):
            if not r.startswith("?"):
                ok = a == r
            elif r in bound:
                ok = a == variables[r][k]
            else:
                ok = seen.setdefault(r, a) == a
            if not ok:
                break
        if ok:
            matches.append(seen)
    return len(matches) == 1 and all(variables[v][k] == x for v, x in matches[0].items())


def tell_apart(methods, pred_types):
    """Give each pair of cases a fact one has and the other lacks."""
    def kinds(method):
        """(predicate, place of the module) held by every example / by any."""
        every, some = None, set()
        for e in method["examples"]:
            here = {(f[0], i) for f in e["facts"] for i, a in enumerate(f[1:]) if a == e["module"]}
            every = here if every is None else every & here
            some |= here
        return every, some
    for a, b in itertools.permutations(methods, 2):
        every_a, _ = kinds(a)
        _, some_b = kinds(b)
        marks = sorted(every_a - some_b)
        every_b, _ = kinds(b)
        _, some_a = kinds(a)
        if not marks and not (every_b - some_a):
            raise CannotLift(f"{a['name']} and {b['name']}: no fact about the module tells their problems apart")
        if marks:
            pred, place = marks[0]
            if (pred, place) not in b["absent"]:
                b["absent"].append((pred, place))
    for m in methods:
        m["absent_text"] = []
        for pred, place in m["absent"]:
            types = pred_types[pred]
            args = ["?m" if i == place else f"?x{i}" for i in range(len(types))]
            others = [(x, t) for i, (x, t) in enumerate(zip(args, types)) if i != place]
            atom = f"({pred} {' '.join(args)})"
            m["absent_text"].append(f"(not (exists ({' '.join(f'{x} - {t}' for x, t in others)}) {atom}))"
                                    if others else f"(not {atom})")


def hddl(method):
    params = " ".join(f"{v} - {t}" for v, t in method["types"].items())
    conditions = [f"({' '.join(c)})" for c in method["conditions"]] + method["absent_text"]
    subtasks = "\n".join(f"      (t{i + 1} ({' '.join([n] + args)}))" for i, (n, args) in enumerate(method["subtasks"]))
    return (f"(:method {method['name']}\n    :parameters ({params})\n    :task (implement_module ?m)\n"
            f"    :precondition (and {' '.join(conditions)})\n    :ordered-subtasks (and\n{subtasks}\n    )\n  )")


def lift(family, examples):
    """The methods for a family, as HDDL forms, from its checked lists."""
    cases = collections.defaultdict(list)
    for e in examples:
        cases[tuple(n for n, _ in e["steps"])].append(e)
    methods = [lift_case(group, f"m_implement_{family}_{k + 1}")
               for k, (_, group) in enumerate(sorted(cases.items(), key=lambda kv: kv[1][0]["tag"]))]
    tell_apart(methods, predicate_types(open(lf.DOMAIN).read()))
    return [hddl(m) for m in methods], methods


def checked_lists(family, runs_dir):
    """{problem tag: the first list of each run that passed}."""
    out = collections.defaultdict(list)
    for path in sorted(glob.glob(os.path.join(runs_dir, family, "*", "run_*", "round_*", "verdict.json"))):
        if json.load(open(path))["stage"] == "pass":
            steps, _ = lg.answer_steps("```\n" + open(os.path.join(os.path.dirname(path), "steps.txt")).read() + "```")
            out[path.split(os.sep)[-4]].append(steps)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("family", choices=sorted(lf.FAMILIES))
    ap.add_argument("--from", dest="source", choices=["hand", "runs"], default="hand")
    ap.add_argument("--runs-dir", default=lg.RUNS, help="where learn_ground.py's runs are (default eval/runs/ground)")
    ap.add_argument("--config", default=os.path.join(HERE, "eval_config.json"))
    ap.add_argument("--out", help="directory for the methods and the verdict (default: a temporary one)")
    args = ap.parse_args()
    cfg, tools = rtl_eval.load_config(os.path.abspath(args.config))
    train, _ = lf.split(args.family)
    examples = []
    if args.source == "hand":
        examples = [example(c, lg.frontier(args.family, c)) for c in train]
    else:
        lists = checked_lists(args.family, args.runs_dir)
        for c in train:
            # One list a problem: the one most runs agree on.
            if lists.get(c["tag"]):
                best = collections.Counter(lg.show(s) for s in lists[c["tag"]]).most_common(1)[0][0]
                examples.append(example(c, lg.answer_steps("```\n" + best + "\n```")[0]))
        print(f"{len(examples)} of {len(train)} training problems have a checked list")
    if not examples:
        sys.exit("nothing to lift")
    try:
        forms, methods = lift(args.family, examples)
    except CannotLift as e:
        sys.exit(f"cannot lift: {e}")
    print(f"{len(methods)} method(s) from {len(examples)} list(s): "
          + ", ".join(f"{m['name']} from {len(m['examples'])}" for m in methods))
    print("\n\n".join(forms))
    work = args.out or os.path.join(lf.RUNS + "_lift", args.family, args.source)
    verdict = lf.test_answer(args.family, "```hddl\n" + "\n\n".join(forms) + "\n```", work, tools)
    print(f"\ntraining problems and the other families: {verdict['stage']}")
    if verdict["stage"] != "pass":
        print(verdict["diagnostic"][:1500])
    else:
        v = lf.validate(args.family, os.path.join(work, "domain.hddl"), tools)
        verdict["validation"] = v
        print(f"held-out problems: {v['passed']} of {v['problems']} pass; "
              f"{v['same_plan_as_hand_written']} get the hand-written methods' plan")
        if v["failed"]:
            print("failed, for example:", ", ".join(v["failed"][:5]), v["stages"])
        # Problems of this family that the generator did not make: the worked example and
        # the pilot's problem files. Planner only; the hand-written methods' plan is the test.
        rows = [r for r in lf.slice_domain()["probes"] if r["kind"] == args.family
                and not any(r["name"] == c["tag"] for c in train)]
        same = 0
        for r in rows:
            steps, _, _ = lf.plan(os.path.join(work, "domain.hddl"), r["problem"])
            same += steps is not None and [[n, list(a)] for n, a in steps] == r["steps"]
        verdict["other_problems"] = {"problems": len(rows), "same_plan_as_hand_written": same}
        print(f"problems from outside the generator: {same} of {len(rows)} get the hand-written methods' plan")
    with open(os.path.join(work, "verdict.json"), "w") as f:
        json.dump(verdict, f, indent=1)


if __name__ == "__main__":
    main()
