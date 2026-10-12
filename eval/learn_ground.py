#!/usr/bin/env python3
"""Leave one design family out, and have the model do its methods' work by
hand: list the steps for one problem at a time.

The companion of learn_family.py, which asks the model for the methods
themselves and gets nothing usable (LLM_RTL_code_generation.md 8.6.12). Here
the model is asked for less, which is what 6.2.2 specifies: a ground
decomposition. For one training problem, with the family's methods removed
from the domain, it lists the steps that carry out (implement_module m). A
step is an action, or a task the domain still has methods for, with the
problem's own names as arguments and no variables. Turning checked lists into
methods is a symbolic job, done elsewhere.

    python eval/learn_ground.py show wrap_counter      # the prompt for its first problem
    python eval/learn_ground.py run wrap_counter       # propose, test, repair
    python eval/learn_ground.py summary

What the model sees is what learn_family.py shows it: the domain without the
family's methods and without comments, what a problem of that kind states,
and the problem. Only the question differs.

How a list is tested, in order, stopping at the first failure:

  format      it holds steps, one a line, and nothing else
  names       each step is a known action or task, with the right number of
              arguments, all of them names from the problem
  plan        the planner can carry the steps out in order; if not, the first
              step it cannot do is found and named
  ambiguous   and the same plan results under two seeds
  render      the fixed templates can write the plan out
  simulate    the module they write behaves as the design should

The problems are the generated training problems of learn_family.py, judged
against a Python model. No benchmark testbench is read.
"""

import argparse
import collections
import difflib
import itertools
import json
import os
import re
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import learn_family as lf  # noqa: E402
import plan_to_verilog  # noqa: E402
import problem_compiler  # noqa: E402
import rtl_eval  # noqa: E402

RUNS = os.path.join(HERE, "runs", "ground")
STAGES = ["format", "names", "plan", "ambiguous", "render", "simulate", "pass"]
SYSTEM = "You plan the steps for writing a digital design. You answer with the list of steps only."


# ---------------------------------------------------------------------------
# The domain's vocabulary, and a problem with a list of steps as its task

def typed(tokens):
    """[(name, type)] from a typed list's tokens: ?a ?b - t ?c - u."""
    out, pending, i = [], [], 0
    while i < len(tokens):
        if tokens[i] == "-":
            out += [(x, tokens[i + 1]) for x in pending]
            pending, i = [], i + 2
        else:
            pending.append(tokens[i])
            i += 1
    return out + [(x, "object") for x in pending]


def signatures(domain_text):
    """{name: (kind, the types of its parameters)} for the domain's actions
    and for its tasks that have at least one method."""
    with_methods = set(lf.method_tasks(domain_text).values())
    out = {}
    for start, end, kind, name in lf.top_forms(domain_text):
        if kind == ":action" or (kind == ":task" and name in with_methods):
            params = re.search(r":parameters\s*\(([^)]*)\)", domain_text[start:end])
            out[name] = (kind[1:], [t for _, t in typed(params.group(1).split())] if params else [])
    return out


def action_forms(domain_text):
    """{action: (parameter names, precondition, effects)}, the last two as
    nested lists. Every effect in this domain adds one fact."""
    out = {}
    for start, end, kind, name in lf.top_forms(domain_text):
        if kind != ":action":
            continue
        form = problem_compiler.parse_sexpr(domain_text[start:end])
        part = {form[i]: form[i + 1] for i in range(2, len(form) - 1, 2)}
        params = [x for x, _ in typed(part.get(":parameters", []))]
        effect = part.get(":effect", [])
        effects = effect[1:] if effect[:1] == ["and"] else ([effect] if effect else [])
        out[name] = (params, part.get(":precondition", []), effects)
    return out


def unmet(domain_text, facts, plan, name, args):
    """Which parts of an action's precondition do not hold after `plan`, as
    far as that can be told without the planner: the plain facts it needs
    that are missing, and the facts it needs absent that are there."""
    forms = action_forms(domain_text)
    state = {tuple(f) for f in facts}
    for step, step_args in plan:
        params, _, effects = forms[step]
        bind = dict(zip(params, step_args))
        state |= {tuple(bind.get(x, x) for x in e) for e in effects}
    params, pre, _ = forms[name]
    bind = dict(zip(params, args))
    parts = pre[1:] if pre[:1] == ["and"] else ([pre] if pre else [])
    out = []
    for part in parts:
        if part and all(isinstance(x, str) for x in part):
            atom = tuple(bind.get(x, x) for x in part)
            if atom not in state:
                out.append(f"it needs the fact ({' '.join(atom)}), which is not true at this point")
        elif len(part) == 2 and part[0] == "not" and all(isinstance(x, str) for x in part[1]):
            atom = tuple(bind.get(x, x) for x in part[1])
            if atom in state:
                out.append(f"it needs ({' '.join(atom)}) not to be true, and it is")
    return out


def precondition(domain_text, name):
    """An action's precondition as the domain writes it, for a diagnostic."""
    for start, end, kind, n in lf.top_forms(domain_text):
        if kind == ":action" and n == name:
            body = domain_text[start:end]
            m = re.search(r":precondition\s*(.*?)\s*:effect", body, re.S)
            p = re.search(r":parameters\s*(\([^)]*\))", body)
            return (p.group(1) if p else "()"), (" ".join(m.group(1).split()) if m else "()")
    return None


def doable(domain_path, domain_text, compiled, facts, names, known, done):
    """What can be done next, after the steps in `done`: every action and
    task of the domain, with every choice of this problem's names of the
    right types, that the planner can carry out at that point. Returned as
    text for a diagnostic, or "" if `done` itself cannot be carried out.

    This is the state a decomposition is proposed in (6.2.2), put as what it
    allows. An action is first tried against the facts with unmet(), which is
    cheap, and the planner confirms the rest."""
    before, _, _ = lf.plan(domain_path, with_steps(compiled, done)) if done else ([], "", None)
    if before is None:
        return ""
    by_type = collections.defaultdict(list)
    for n, t in sorted(names.items()):
        by_type[t].append(n)
    candidates = []
    for name, (kind, types) in sorted(known.items()):
        pools = [by_type[t] if t != "object" else sorted(names) for t in types]
        size = 1
        for pool in pools:
            size *= len(pool)
        if size == 0 or size > 4000:
            continue
        for args in itertools.product(*pools):
            if kind == "task" or not unmet(domain_text, facts, before, name, list(args)):
                candidates.append((name, list(args)))
    if len(candidates) > 3000:
        return ""

    def works(step):
        return lf.plan(domain_path, with_steps(compiled, done + [step]))[0] is not None
    with ThreadPoolExecutor(8) as pool:
        ok = list(pool.map(works, candidates))
    found = collections.defaultdict(list)
    for (name, args), good in zip(candidates, ok):
        if good:
            found[name].append(args)
    if not found:
        return ""
    lines = []
    for name in sorted(found, key=lambda n: (known[n][0] != "task", n)):
        shown = [f"({' '.join([name] + a)})" for a in found[name][:4]]
        more = f" and {len(found[name]) - 4} more with other arguments" if len(found[name]) > 4 else ""
        lines.append(f"- {known[name][0]} {name}: " + " ".join(shown) + more)
    where = f"After your first {len(done)} step(s)" if done else "At the start"
    return (f"\n\n{where}, these are all the steps that can be done next. A task is listed if one of its methods "
            f"applies; an action, if its precondition holds:\n" + "\n".join(lines))


def with_steps(compiled, steps):
    """The problem with these ground steps, in order, as its task network."""
    network = "\n".join(f"      (t{i + 1} ({' '.join([name] + args)}))" for i, (name, args) in enumerate(steps))
    block = "(:htn\n    :parameters ()\n    :ordered-subtasks (and\n" + network + "\n    )\n  )"
    out, n = re.subn(r"\(:htn.*?\n  \)", lambda _: block, compiled, count=1, flags=re.S)
    if n != 1:
        sys.exit("error: the compiled problem has no (:htn ...) block where one was expected")
    return out


def objects_of(compiled):
    """{name: type} of what a step may use: the problem's objects and the
    domain's constants."""
    block = re.search(r"\(:objects(.*?)\n  \)", compiled, re.S).group(1)
    names = {"zero": "bitval", "one": "bitval", "active_high": "polarity", "active_low": "polarity"}
    for line in block.splitlines():
        if " - " in line:
            listed, type_ = line.rsplit(" - ", 1)
            names.update({n: type_.strip() for n in listed.split()})
    return names


# ---------------------------------------------------------------------------
# The prompt

def prompt(family, case):
    others = ", ".join(lf.KIND_NAMES[k] for k in lf.KINDS if k != family)
    compiled, facts, _ = problem_compiler.compile_text(case["problem"])
    module = next(f[1] for f in facts if f[0] == "first_port")
    return f"""The HDDL domain at the end of this message turns a description of a small digital design, restated as facts, into an ordered list of steps for writing its Verilog.

- A step is a primitive action. An analyze_* action records a design decision as facts. A write_* action stands for one piece of code, shown in the comment above it.
- A planner decomposes the task (implement_module ?m) with the domain's methods until only actions are left. The plan is those actions in order.
- The Verilog is then written by producing the code of each write_* step, in plan order. So the plan must contain, in the right order, every step a complete and correct module needs.

The domain has methods for these kinds of design: {others}. It has the actions for one more kind, {lf.FAMILIES[family]['name']}, but no methods for it, so the planner cannot plan the problem below. Do by hand what the missing methods would do, for this one problem: list the steps that carry out (implement_module {module}).

WHAT A PROBLEM OF THIS KIND STATES

{lf.guide_section(lf.FAMILIES[family]['section'])}

Facts named state_code and state_width, and objects named nN and bitsN, are added to a problem automatically.

RULES

- A step is one of the domain's actions, or one of its tasks that has methods, written with this problem's own names as arguments: (write_interface {module}), never (write_interface ?m). No variables, and no expressions in parentheses as arguments.
- Give the steps in the order they are to be done, one a line. Where one of the domain's tasks already does several steps in a row, use the task.
- An action can only be a step when its precondition holds at that point: every fact it needs is in the problem or was added by an earlier step.
- The steps must be everything a complete and correct module needs, and nothing more.

HOW YOUR ANSWER IS TESTED

The planner carries out your steps in order, expanding each task with the domain's methods. The Verilog written from the resulting plan is simulated and compared with what the design should do.

THE PROBLEM

{case['what']}

```hddl
{compiled.strip()}
```

THE DOMAIN

```hddl
{lf.reduced_domain(family).strip()}
```

Answer with one ``` code block that holds the steps, one a line, and nothing else."""


def repair_prompt(family, case, answer, diagnostic):
    return (prompt(family, case) + "\n\nYOUR PREVIOUS ANSWER\n\n```\n" + answer.strip() + "\n```\n\n"
            "It was tested and failed:\n\n" + diagnostic.strip() + "\n\n"
            "Write the complete corrected list of steps, as one ``` code block.")


# ---------------------------------------------------------------------------
# Testing a list of steps

def answer_steps(reply):
    """(the steps as (name, [args]), None) or (None, why not)."""
    blocks = re.findall(r"```[a-z]*\s*\n(.*?)```", reply, re.S)
    if not blocks:
        return None, "the reply holds no ``` code block"
    steps = []
    for line in blocks[-1].splitlines():
        line = line.split(";")[0].strip()
        if not line:
            continue
        # A step with or without its parentheses, and with or without a label
        # or a number in front: the model leaves the parentheses off, and a
        # line of names can be read only one way.
        m = (re.fullmatch(r"(?:\(\s*t\d+\s*|\d+[.):]\s*)?\(\s*([^\s()]+)((?:\s+[^\s()]+)*)\s*\)\s*\)?", line)
             or re.fullmatch(r"(?:\d+[.):]\s*)?([^\s()]+)((?:\s+[^\s()]+)*)", line))
        if not m:
            return None, (f"this line is not a step: {line[:120]}\nA step is a name and its arguments, "
                          f"(name argument ...), with names as arguments and nothing nested.")
        steps.append((m.group(1), m.group(2).split()))
    if not steps:
        return None, "the code block holds no steps"
    return steps, None


def show(steps):
    return "\n".join("(" + " ".join([name] + list(args)) + ")" for name, args in steps)


def test_steps(family, case, reply, workdir, tools):
    """The verdict on one list of steps for one problem."""
    os.makedirs(workdir, exist_ok=True)
    steps, why = answer_steps(reply)
    if steps is None:
        return {"stage": "format", "diagnostic": why}
    domain_text = lf.reduced_domain(family)
    domain_path = os.path.join(workdir, "domain.hddl")
    with open(domain_path, "w") as f:
        f.write(domain_text)
    with open(os.path.join(workdir, "steps.txt"), "w") as f:
        f.write(show(steps) + "\n")
    compiled, facts, _ = problem_compiler.compile_text(case["problem"])
    known, names, of_method = signatures(domain_text), objects_of(compiled), lf.method_tasks(domain_text)
    wrong, first_wrong = [], None
    for i, (name, args) in enumerate(steps, 1):
        here = f"step {i}, ({' '.join([name] + args)}):"
        if any(a.startswith("?") for a in args):
            wrong.append(f"{here} it has a variable. Use this problem's names.")
        elif name not in known:
            hint = ""
            if "m_" + name in of_method:
                hint = (f" It is the name of a method, without its m_; a step names the task a method "
                        f"decomposes, here {of_method['m_' + name]}.")
            else:
                close = difflib.get_close_matches(name, known, n=3, cutoff=0.6)
                hint = f" The nearest names the domain has are {', '.join(close)}." if close else ""
            wrong.append(f"{here} {name} is not an action of the domain, or a task of it that has a method.{hint}")
        elif len(args) != len(known[name][1]):
            wrong.append(f"{here} the {known[name][0]} {name} takes {len(known[name][1])} argument(s), "
                         f"not {len(args)}.")
        elif [a for a in args if a not in names]:
            wrong.append(f"{here} {', '.join(a for a in args if a not in names)} is not a name in this problem.")
        else:
            for j, (a, want) in enumerate(zip(args, known[name][1]), 1):
                if want not in ("object", names[a]):
                    wrong.append(f"{here} argument {j}, {a}, is a {names[a]}; the {known[name][0]} {name} takes "
                                 f"a {want} there.")
        if wrong and first_wrong is None:
            first_wrong = i - 1
    if wrong:
        return {"stage": "names", "steps": len(steps),
                "diagnostic": "\n".join(wrong[:12]) + doable(domain_path, domain_text, compiled, facts, names, known,
                                                             steps[:first_wrong])}

    plan, message, _ = lf.plan(domain_path, with_steps(compiled, steps))
    if plan is None:
        # The first step the planner cannot do: the shortest prefix with no plan.
        def works(k):
            return lf.plan(domain_path, with_steps(compiled, steps[:k]))[0] is not None
        with ThreadPoolExecutor(8) as pool:
            ok = list(pool.map(works, range(1, len(steps) + 1)))
        k = next((i for i, good in enumerate(ok) if not good), len(steps) - 1)
        name, args = steps[k]
        diagnostic = (f"The planner could do the first {k} step(s) and then could not do step {k + 1}, "
                      f"({' '.join([name] + args)}).")
        if known[name][0] == "action":
            params, pre = precondition(domain_text, name)
            before, _, _ = lf.plan(domain_path, with_steps(compiled, steps[:k])) if k else ([], "", None)
            missing = unmet(domain_text, facts, before or [], name, args)
            diagnostic += f" It is an action with parameters {params} and precondition {pre}. "
            diagnostic += ("With this step's arguments, " + "; ".join(missing) + ". Either this is the wrong action "
                           "here, or an earlier step that adds the fact is missing, or the arguments are wrong."
                           if missing else
                           "With this step's arguments, that precondition does not hold at this point.")
        else:
            diagnostic += (" It is a task, and none of its methods applies at this point with these arguments, "
                           "or a step inside it cannot be done.")
        diagnostic += doable(domain_path, domain_text, compiled, facts, names, known, steps[:k])
        return {"stage": "plan", "diagnostic": diagnostic, "steps": len(steps), "failed_step": k + 1}
    other, _, _ = lf.plan(domain_path, with_steps(compiled, steps), seed=2)
    if other != plan:
        return {"stage": "ambiguous", "steps": len(steps),
                "diagnostic": "Two runs of the planner expanded your steps into different plans."}
    try:
        verilog = plan_to_verilog.render(plan)
    except (SystemExit, Exception) as e:
        return {"stage": "render", "steps": len(steps),
                "diagnostic": (f"The planner carried out your steps, but the resulting plan cannot be written out "
                               f"as code ({type(e).__name__}: {e}). A needed step is missing or out of order. "
                               f"The plan was:\n{lf.show(plan)}")}
    wrong = lf.simulate(verilog, case["tb"], tools)
    if wrong:
        return {"stage": "simulate", "steps": len(steps),
                "diagnostic": (f"The planner carried out your steps, but the module written from the plan is "
                               f"wrong: {wrong}.\nThe plan was:\n{lf.show(plan)}\nThe module written from it:\n"
                               f"{verilog}")}
    full, _, _ = lf.plan(lf.DOMAIN, compiled)
    return {"stage": "pass", "diagnostic": "", "steps": len(steps), "plan_steps": len(plan),
            "same_plan_as_hand_written": plan == full, "tasks_used": sum(known[n][0] == "task" for n, _ in steps)}


def frontier(family, case):
    """The hand-written methods' own decomposition of this problem, cut where
    the reduced domain can take over: for the harness's self-test."""
    compiled, _, _ = problem_compiler.compile_text(case["problem"])
    known = signatures(lf.reduced_domain(family))
    _, _, tree = lf.plan(lf.DOMAIN, compiled, tree=True)
    removed, _ = lf.removed(family)
    out = []

    def walk(node):
        name, *args = node["token"].strip("()").split()
        if node["action"] or (name in known and not uses_removed(node)):
            out.append((name, args))
        else:
            for child in node.get("children", []):
                walk(child)

    def uses_removed(node):
        return node.get("method") in removed or any(uses_removed(c) for c in node.get("children", []))
    for child in tree["roots"][0]["children"]:
        walk(child)
    return out


# ---------------------------------------------------------------------------
# The loop

def write_request(d, user):
    os.makedirs(d, exist_ok=True)
    body = json.dumps({"system": SYSTEM, "user": user}, indent=1)
    path = os.path.join(d, "request.json")
    if not os.path.exists(path) or open(path).read() != body:
        with open(path, "w") as f:
            f.write(body)
    return path


def cmd_show(args, cfg, tools):
    train, _ = lf.split(args.family)
    text = prompt(args.family, train[args.problem])
    print(text if args.full else text[:text.index("THE DOMAIN")] + f"THE DOMAIN ...\n\n({len(text)} characters)")


def cmd_run(args, cfg, tools):
    """Round 1: one request a training problem, a sample per run. Each later
    round: every list that has not passed goes back with its diagnostic."""
    family, base = args.family, os.path.join(RUNS, args.family)
    os.makedirs(base, exist_ok=True)
    train, _ = lf.split(family)
    train = train[:args.problems] if args.problems else train
    methods, tasks = lf.removed(family)
    with open(lf.proposer_config(cfg)) as f:
        proposer = json.load(f)
    meta = {"created": rtl_eval.now(), "family": family, "runs": args.runs, "rounds": args.rounds,
            "proposer": {"config": lf.proposer_config(cfg), "sampling": proposer["ladder_sampling"]},
            "removed_methods": sorted(methods), "removed_tasks": sorted(tasks),
            "problems": [c["tag"] for c in train],
            "sha256": {"domain": rtl_eval.sha256_file(lf.DOMAIN), "guide": rtl_eval.sha256_file(lf.GUIDE),
                       "learn_ground": rtl_eval.sha256_file(os.path.abspath(__file__)),
                       "learn_family": rtl_eval.sha256_file(os.path.join(HERE, "learn_family.py")),
                       "proposer_config": rtl_eval.sha256_file(lf.proposer_config(cfg))}}
    with open(os.path.join(base, "run.json"), "w") as f:
        json.dump(meta, f, indent=1)

    keys = [(p, r) for p in range(len(train)) for r in range(args.runs)]
    answers, verdicts = {}, collections.defaultdict(list)
    jobs = []
    for p, case in enumerate(train):
        d = os.path.join(base, case["tag"], "round_1")
        request = write_request(d, prompt(family, case))
        if not lf.generated(d, args.runs):
            jobs.append({"request": request, "out_dir": d})
    lf.executor(cfg, jobs, os.path.join(base, "batch.json"), args.runs)
    for p, r in keys:
        with open(os.path.join(base, train[p]["tag"], "round_1", f"sample_{r:02d}.response.txt"),
                  errors="replace") as f:
            answers[(p, r)] = f.read()
    for rnd in range(1, args.rounds + 1):
        for p, r in keys:
            if verdicts[(p, r)] and verdicts[(p, r)][-1]["stage"] == "pass":
                continue
            work = os.path.join(base, train[p]["tag"], f"run_{r:02d}", f"round_{rnd}")
            v = test_steps(family, train[p], answers[(p, r)], work, tools)
            with open(os.path.join(work, "verdict.json"), "w") as f:
                json.dump(v, f, indent=1)
            verdicts[(p, r)].append(v)
            print(f"{train[p]['tag']} run {r:02d} round {rnd}: {v['stage']}", flush=True)
        open_keys = [k for k in keys if verdicts[k][-1]["stage"] != "pass"]
        if not open_keys or rnd == args.rounds:
            break
        jobs = []
        for p, r in open_keys:
            d = os.path.join(base, train[p]["tag"], f"run_{r:02d}", f"round_{rnd + 1}")
            steps, _ = answer_steps(answers[(p, r)])
            last = show(steps) if steps else answers[(p, r)][-2000:]
            request = write_request(d, repair_prompt(family, train[p], last, verdicts[(p, r)][-1]["diagnostic"]))
            if not lf.generated(d, 1):
                jobs.append({"request": request, "out_dir": d})
        lf.executor(cfg, jobs, os.path.join(base, "batch.json"), 1)
        for p, r in open_keys:
            d = os.path.join(base, train[p]["tag"], f"run_{r:02d}", f"round_{rnd + 1}")
            with open(os.path.join(d, "sample_00.response.txt"), errors="replace") as f:
                answers[(p, r)] = f.read()
    with open(os.path.join(base, "verdicts.json"), "w") as f:
        json.dump({f"{train[p]['tag']}/{r}": v for (p, r), v in verdicts.items()}, f, indent=1)


def cmd_summary(args, cfg, tools):
    md = ["# Leave one family out: the model lists the steps for one problem", "",
          "| family | problems | lists (problems x runs) | pass in round 1 | 2 | 3 | never | same plan as the "
          "hand-written methods | requests to the model |",
          "|---|---|---|---|---|---|---|---|---|"]
    out = {}
    for family in lf.FAMILIES:
        path = os.path.join(RUNS, family, "verdicts.json")
        if not os.path.exists(path):
            continue
        verdicts = json.load(open(path))
        meta = json.load(open(os.path.join(RUNS, family, "run.json")))
        solved, stages, same = collections.Counter(), collections.Counter(), 0
        requests = len(meta["problems"])
        for key, vs in verdicts.items():
            requests += len(vs) - 1
            if vs[-1]["stage"] == "pass":
                solved[len(vs)] += 1
                same += vs[-1]["same_plan_as_hand_written"]
            else:
                solved["never"] += 1
                stages[vs[-1]["stage"]] += 1
        passed = sum(v for k, v in solved.items() if k != "never")
        md.append(f"| {family} | {len(meta['problems'])} | {len(verdicts)} | {solved[1]} | {solved[2]} | {solved[3]} "
                  f"| {solved['never']}"
                  + (f" ({', '.join(f'{k} {v}' for k, v in stages.items())})" if stages else "")
                  + f" | {same} of {passed} | {requests} |")
        by_problem = collections.Counter(k.split("/")[0] for k, vs in verdicts.items() if vs[-1]["stage"] == "pass")
        out[family] = {"solved_in_round": dict(solved), "last_failure": dict(stages), "same_plan": same,
                       "requests": requests, "passed_by_problem": dict(by_problem),
                       "first_round_stages": dict(collections.Counter(vs[0]["stage"] for vs in verdicts.values()))}
    os.makedirs(RUNS, exist_ok=True)
    with open(os.path.join(RUNS, "summary.json"), "w") as f:
        json.dump(out, f, indent=1)
    with open(os.path.join(RUNS, "summary.md"), "w") as f:
        f.write("\n".join(md) + "\n")
    print("\n".join(md))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--config", default=os.path.join(HERE, "eval_config.json"))
    ap.add_argument("--tag", help="keep this run apart: eval/runs/ground_<tag>")
    ap.add_argument("--proposer-config", help="how the proposing model is run (default: the executor's "
                                              "configuration)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("show")
    s.add_argument("family", choices=sorted(lf.FAMILIES))
    s.add_argument("--problem", type=int, default=0, help="which training problem (default the first)")
    s.add_argument("--full", action="store_true", help="print the domain too")
    s = sub.add_parser("run")
    s.add_argument("family", choices=sorted(lf.FAMILIES))
    s.add_argument("--runs", type=int, default=4, help="independent lists a problem (default 4)")
    s.add_argument("--rounds", type=int, default=3, help="answers a list may take: the first and repairs (default 3)")
    s.add_argument("--problems", type=int, help="use only the first N training problems (default all)")
    sub.add_parser("summary")
    args = ap.parse_args()
    global RUNS
    if args.tag:
        RUNS += "_" + args.tag
    lf.PROPOSER_CONFIG = args.proposer_config
    cfg, tools = rtl_eval.load_config(os.path.abspath(args.config))
    {"show": cmd_show, "run": cmd_run, "summary": cmd_summary}[args.cmd](args, cfg, tools)


if __name__ == "__main__":
    main()
