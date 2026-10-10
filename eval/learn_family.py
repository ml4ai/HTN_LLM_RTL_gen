#!/usr/bin/env python3
"""Leave one design family out, and have the model write its methods back.

A first, small test of whether the RTL domain can be produced rather than
written by hand (LLM_RTL_code_generation.md 6.2.2, 6.2.8, 8.6.12). One family's
methods are removed from rtl_designs/rtl_domain.hddl. The frozen model is shown
the rest of the domain, what the family's problems state, and two example
problems, and is asked for the missing methods. Its answer is tested, and a
failed answer goes back to it with the diagnostic, a fixed number of times.

    python eval/learn_family.py slice wrap_counter     # what is removed; the prompt
    python eval/learn_family.py run wrap_counter       # propose, test, repair
    python eval/learn_family.py summary

What is removed: the methods that only this family's plans use, and the tasks
left with no method. The actions stay; this is the "methods only" rung, in
which the primitives are given. Every comment is stripped from what the model
sees except the one above each action, which says what code the action stands
for, so nothing written about the removed methods is left behind.

How an answer is tested, in order, stopping at the first failure:

  format      it holds (:task ...) and (:method ...) forms and nothing else
  load        the planner loads the domain with the answer added
  plan        every training problem has a plan
  ambiguous   and the same plan under two seeds: no decision is left open
  render      the fixed templates can write the plan out
  simulate    the module they write behaves as the design should
  regression  and the other families' plans are what they were

The training problems are generated here (rtl_designs/check_*.py), never taken
from a benchmark, and their expected behaviour comes from a Python model. A
run that passes is then tried on the family's other generated problems, which
it was never told about. Nothing in this file reads a benchmark testbench.
"""

import argparse
import collections
import glob
import json
import os
import random
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RTL = os.path.join(ROOT, "rtl_designs")
sys.path.insert(0, HERE)
sys.path.insert(0, RTL)

import check_counters_and_tables as cct  # noqa: E402
import check_sequence_detectors as csd  # noqa: E402
import plan_to_verilog  # noqa: E402
import problem_compiler  # noqa: E402
import rtl_eval  # noqa: E402

DOMAIN = os.path.join(RTL, "rtl_domain.hddl")
GUIDE = os.path.join(RTL, "problem_guide.txt")
PLANNER = os.path.join(ROOT, "build", "apps", "planners", "MCTS_planner")
RUNS = os.path.join(HERE, "runs", "learn")
PLAN_SECONDS = 60
STAGES = ["format", "load", "plan", "ambiguous", "render", "simulate", "regression", "pass"]

SYSTEM = "You write HTN planning domains in HDDL. You answer with HDDL only."

# What each family is called in the prompt, the fact that marks its problems,
# and the section of the problem guide that says what its problems state.
FAMILIES = {
    "wrap_counter": {"name": "a wrap-around counter", "section": "D"},
    "table_fsm": {"name": "a Moore state machine given by a table or diagram", "section": "B"},
    "mealy_detector": {"name": "a Mealy sequence detector", "section": "A"},
}
KINDS = ["mealy_detector", "moore_detector", "table_fsm", "sequence_recogniser", "wrap_counter",
         "triangle_generator"]
KIND_NAMES = {"mealy_detector": "Mealy sequence detectors", "moore_detector": "Moore sequence detectors",
              "table_fsm": "Moore state machines given by a table", "sequence_recogniser": "sequence recognisers",
              "wrap_counter": "wrap-around counters", "triangle_generator": "triangle-wave generators"}


# ---------------------------------------------------------------------------
# HDDL text

def top_forms(text):
    """(start, end, kind, name) of each form directly inside (define ...)."""
    forms, depth, i, start = [], 0, 0, None
    while i < len(text):
        c = text[i]
        if c == ";":
            j = text.find("\n", i)
            i = len(text) if j < 0 else j
            continue
        if c == "(":
            depth += 1
            if depth == 2:
                start = i
        elif c == ")":
            if depth == 2:
                m = re.match(r"\(\s*(\S+)\s*([^\s()]*)", text[start:i + 1])
                forms.append((start, i + 1, m.group(1), m.group(2)))
            depth -= 1
        i += 1
    return forms


def strip_comments(text):
    """The text without its comments, except the comment lines directly above
    an action: they say what code the action stands for."""
    lines = text.splitlines()
    keep = [True] * len(lines)
    for i, line in enumerate(lines):
        if not line.strip().startswith(";"):
            continue
        j = i
        while j < len(lines) and lines[j].strip().startswith(";"):
            j += 1
        keep[i] = j < len(lines) and lines[j].strip().startswith("(:action")
    out = []
    for line, k in zip(lines, keep):
        if not k:
            continue
        if not line.strip().startswith(";") and ";" in line:
            line = line[:line.index(";")].rstrip()
        if line.strip() or (out and out[-1].strip()):
            out.append(line)
    return "\n".join(out) + "\n"


def method_tasks(text):
    """{method name: the task it decomposes}."""
    out = {}
    for start, end, kind, name in top_forms(text):
        if kind == ":method":
            out[name] = re.search(r":task\s*\(\s*(\S+?)[\s)]", text[start:end]).group(1)
    return out


def without(text, methods, tasks):
    """The domain text with these methods and task declarations taken out."""
    out, last = [], 0
    for start, end, kind, name in top_forms(text):
        if (kind == ":method" and name in methods) or (kind == ":task" and name in tasks):
            out.append(text[last:start])
            last = end
    return "".join(out) + text[last:]


def with_answer(reduced, forms):
    """The reduced domain with the answer's forms added before its last ')'."""
    cut = reduced.rstrip().rindex(")")
    return reduced[:cut] + "\n" + "\n\n".join("  " + f for f in forms) + "\n)\n"


# ---------------------------------------------------------------------------
# Problems: the generated ones of each family, and the probes of the others

def toggle_reset(text):
    """The same problem with the other kind of reset, async for sync."""
    m = re.search(r"\(reset_of\s+\S+\s+([^\s()]+)\)", text)
    if not m:
        return None
    fact = f"(reset_async {m.group(1)})"
    return text.replace(fact, "") if fact in text else text.replace(m.group(0), m.group(0) + " " + fact)


def kind_of(facts):
    kinds = {f[0] for f in facts}
    if "sequence_detector" in kinds:
        return "mealy_detector" if "mealy" in kinds else "moore_detector"
    return next((k for k in KINDS if k in kinds), None)


def style(async_, pol):
    return f"Reset is {'asynchronous' if async_ else 'synchronous'} and {pol.replace('_', ' ')}."


def cases(family):
    """The family's generated problems, in a fixed order: each a dict with
    its tag, a one-line description, the problem text and a testbench."""
    out = []
    if family == "wrap_counter":
        for i, cfg in enumerate(cct.counter_configs()):
            _, low, high, width, enable, async_, pol = cfg
            tag, problem, tb = cct.counter_case(cfg, random.Random(i), 300)
            what = (f"A {width}-bit counter that counts from {low} to {high} and then wraps to {low}, "
                    + ("counting only while its enable input is 1. " if enable
                       else "with no enable input: it counts on every clock edge. ")
                    + style(async_, pol) + f" Reset sets the count to {low}.")
            out.append({"tag": tag, "what": what, "problem": problem, "tb": tb})
    elif family == "table_fsm":
        for i, cfg in enumerate(cct.table_configs()):
            tag, problem, tb = cct.table_case(cfg, random.Random(1000 + i), 300)
            what = ("A Moore state machine with one input bit, given by its table of transitions. Each "
                    "output is 1 in the states listed for it. " + style(cfg[2], cfg[3]))
            out.append({"tag": tag, "what": what, "problem": problem, "tb": tb})
    elif family == "mealy_detector":
        for i, (pat, overlap, async_, pol) in enumerate(csd.configs(5)):
            rng = random.Random(i)
            bits = [rng.choice("01") for _ in range(300)]
            for k in range(0, 300 - len(pat), 23):
                bits[k:k + len(pat)] = list(pat)
            what = (f"A Mealy detector for the bit pattern {pat} on a one-bit input: the output is 1 in the "
                    f"cycle the last bit of the pattern arrives. Occurrences "
                    + ("may overlap. " if overlap else "do not overlap. ") + style(async_, pol))
            out.append({"tag": f"mealy_{pat}_{'ov' if overlap else 'no'}_{'async' if async_ else 'sync'}_{pol}",
                        "what": what, "problem": csd.problem(pat, overlap, async_, pol),
                        "tb": csd.testbench(bits, csd.expected(pat, overlap, bits), pol)})
    return out


TRAIN = {
    # (0..9 enable, async high), (1..10 free, sync high), (0..15 free, async low), (3..6 enable, sync low)
    "wrap_counter": ["counter_0_9_en_async_active_high", "counter_1_10_free_sync_active_high",
                     "counter_0_15_free_async_active_low", "counter_3_6_en_sync_active_low"],
    "table_fsm": [0, 1, 2, 3],
    "mealy_detector": ["mealy_101_ov_", "mealy_10011_no_", "mealy_1_ov_", "mealy_0110_ov_", "mealy_11_no_"],
}


def split(family):
    """(training cases, the rest). The first two training cases are the ones
    shown in the prompt."""
    all_cases = cases(family)
    train = []
    for want in TRAIN[family]:
        train.append(all_cases[want] if isinstance(want, int)
                     else next(c for c in all_cases if c["tag"].startswith(want)))
    tags = {c["tag"] for c in train}
    return train, [c for c in all_cases if c["tag"] not in tags]


def probes():
    """Problems of every family, for finding which methods each uses and for
    checking that an answer leaves the other families' plans alone: the
    worked examples and the pilot's problem files, each with both kinds of
    reset, and a few generated ones. Planner input only; no testbench."""
    out = []
    paths = sorted(glob.glob(os.path.join(RTL, "exemplars", "*.hddl"))) \
        + sorted(glob.glob(os.path.join(RTL, "pilot", "*", "*.hddl")))
    for path in paths:
        if "fsm_hdlc" in path:      # 40 s a plan; the recogniser's worked example covers its family
            continue
        text = open(path).read()
        name = os.path.splitext(os.path.basename(path))[0]
        out.append((name, text))
        other = toggle_reset(text)
        if other:
            out.append((name + "_other_reset", other))
    for family in FAMILIES:
        train, _ = split(family)
        out += [(c["tag"], c["problem"]) for c in train]
    return out


# ---------------------------------------------------------------------------
# Planning, rendering, simulating

def plan(domain_path, problem_text, seed=1, tree=None):
    """(steps or None, the planner's message, the task tree or None)."""
    with tempfile.TemporaryDirectory() as w:
        path = os.path.join(w, "problem.hddl")
        with open(path, "w") as f:
            f.write(problem_text)
        cmd = ["perl", "-e", "alarm shift; exec @ARGV", str(PLAN_SECONDS), PLANNER, "-D", domain_path,
               "-P", path, "-F", "simple", "-i", "100", "-r", "1", "-s", str(seed)]
        tree_path = os.path.join(w, "tree.json")
        if tree:
            cmd += ["-g", "-f", tree_path]
        run = subprocess.run(cmd, capture_output=True, text=True)
        errors = [line for line in run.stderr.splitlines() if not line.startswith("warning:")]
        if run.returncode != 0 or "Plan:" not in run.stdout:
            if run.returncode < 0 or run.returncode == 142:
                return None, f"the planner did not finish in {PLAN_SECONDS} seconds", None
            return None, " ".join(errors)[:600] or f"the planner stopped (exit {run.returncode})", None
        try:
            steps = plan_to_verilog.read_plan(run.stdout)
        except SystemExit as e:
            return None, str(e), None
        return steps, "", json.load(open(tree_path)) if tree else None


def simulate(verilog, tb, tools):
    """None if the module passes the testbench, or what went wrong."""
    with tempfile.TemporaryDirectory() as w:
        for name, text in (("dut.v", verilog), ("tb.v", tb)):
            with open(os.path.join(w, name), "w") as f:
                f.write(text)
        comp = subprocess.run([tools["iverilog"], "-o", "sim.out", "dut.v", "tb.v"], cwd=w,
                              capture_output=True, text=True)
        if comp.returncode:
            return "it does not compile: " + " ".join(comp.stderr.strip().splitlines()[:3])[:400]
        sim = subprocess.run([tools["vvp"], "sim.out"], cwd=w, capture_output=True, text=True)
        if "PASS" in sim.stdout:
            return None
        line = (sim.stdout.strip().splitlines() or ["no output"])[-1]
        m = re.match(r"FAIL (\d+)", line)
        return (f"its outputs are wrong in {m.group(1)} of the clock cycles simulated" if m
                else "the simulation did not finish: " + line[:200])


def show(steps):
    return "\n".join("(" + " ".join([name] + list(args)) + ")" for name, args in steps)


def try_case(domain_path, case, tools, seeds=(1,)):
    """One problem through the planner, the templates and the simulator.
    Returns (stage reached, detail): 'pass', or the stage that failed."""
    try:
        compiled, _, _ = problem_compiler.compile_text(case["problem"])
    except (problem_compiler.Invalid, problem_compiler.Unreadable) as e:
        sys.exit(f"error: generated problem {case['tag']} does not compile: {e}")
    steps, message, _ = plan(domain_path, compiled, seeds[0])
    if steps is None:
        loading = not re.search(r"no applicable decomposition|iteration budget|stuck|decisions without|did not finish",
                                message)
        return ("load" if loading else "plan"), message
    for seed in seeds[1:]:
        other, _, _ = plan(domain_path, compiled, seed)
        if other != steps:
            k = next((i for i, (a, b) in enumerate(zip(steps, other or [])) if a != b), min(len(steps), len(other or [])))
            a = show(steps[k:k + 1]) or "(the plan ends)"
            b = show((other or [])[k:k + 1]) or "(no plan, or the plan ends)"
            return "ambiguous", (f"two runs of the planner gave different plans, so more than one of your methods "
                                 f"applies at some point. They first differ at step {k + 1}: {a} in one and {b} "
                                 f"in the other.")
    try:
        verilog = plan_to_verilog.render(steps)
    except (SystemExit, Exception) as e:
        return "render", (f"the plan cannot be written out as code ({type(e).__name__}: {e}). The plan was:\n"
                          + show(steps))
    wrong = simulate(verilog, case["tb"], tools)
    if wrong:
        return "simulate", (f"the module written from the plan is wrong: {wrong}.\nThe plan was:\n{show(steps)}\n"
                            f"The module written from it:\n{verilog}")
    return "pass", steps


# ---------------------------------------------------------------------------
# Slicing: what a family's methods are

def used(tree):
    methods, actions = set(), set()

    def walk(node):
        if node["action"]:
            actions.add(node["token"].strip("()").split()[0])
        elif node["method"]:
            methods.add(node["method"])
        for child in node.get("children", []):
            walk(child)
    for root in tree["roots"]:
        walk(root)
    return methods, actions


def slice_domain():
    """For every probe, its family, its plan under the full domain, and the
    methods the plan uses. Cached: it is a function of the domain alone."""
    cache = os.path.join(RUNS, "slice.json")
    sha = rtl_eval.sha256_file(DOMAIN)
    if os.path.exists(cache):
        saved = json.load(open(cache))
        if saved["domain_sha256"] == sha:
            return saved
    rows = []

    def one(probe):
        name, text = probe
        compiled, facts, _ = problem_compiler.compile_text(text)
        steps, message, tree = plan(DOMAIN, compiled, tree=True)
        if steps is None:
            sys.exit(f"error: the full domain does not plan probe {name}: {message}")
        methods, actions = used(tree)
        return {"name": name, "kind": kind_of(facts), "problem": compiled, "steps": [[n, a] for n, a in steps],
                "methods": sorted(methods), "actions": sorted(actions)}
    with ThreadPoolExecutor(8) as pool:
        rows = list(pool.map(one, probes()))
    os.makedirs(RUNS, exist_ok=True)
    saved = {"domain_sha256": sha, "probes": rows}
    with open(cache, "w") as f:
        json.dump(saved, f, indent=1)
    return saved


def removed(family):
    """(methods, tasks) taken out for this family: the methods only its
    probes' plans use, and the tasks that leaves with no method."""
    rows = slice_domain()["probes"]
    mine = set().union(*(r["methods"] for r in rows if r["kind"] == family))
    others = set().union(*(r["methods"] for r in rows if r["kind"] != family))
    methods = mine - others
    by_task = collections.defaultdict(set)
    for method, task in method_tasks(open(DOMAIN).read()).items():
        by_task[task].add(method)
    tasks = {t for t, ms in by_task.items() if ms <= methods}
    return methods, tasks


def reduced_domain(family):
    methods, tasks = removed(family)
    body = strip_comments(without(open(DOMAIN).read(), methods, tasks))
    return "; RTL design domain. A plan is an ordered list of primitive actions.\n" + body


def guide_section(letter):
    """What a problem of this kind states, from the problem guide."""
    text = open(GUIDE).read()
    common = text[text.index("FACTS FOR EVERY DESIGN"):text.index("THEN THE FACTS OF EXACTLY ONE KIND")]
    m = re.search(rf"^{letter}\. .*?(?=^[A-Z]\. |^RULES)", text, re.S | re.M)
    return common.strip() + "\n\nFACTS OF THIS KIND OF DESIGN\n\n" + m.group(0).strip()


def prompt(family):
    train, _ = split(family)
    others = ", ".join(KIND_NAMES[k] for k in KINDS if k != family)
    examples = []
    for case in train[:2]:
        compiled, _, _ = problem_compiler.compile_text(case["problem"])
        examples.append(f"{case['what']}\n\n```hddl\n{compiled.strip()}\n```")
    return f"""The HDDL domain at the end of this message turns a description of a small digital design, restated as facts, into an ordered list of steps for writing its Verilog.

- A step is a primitive action. An analyze_* action records a design decision as facts. A write_* action stands for one piece of code, shown in the comment above it.
- A planner decomposes the task (implement_module ?m) with the domain's methods until only actions are left. The plan is those actions in order.
- The Verilog is then written by producing the code of each write_* step, in plan order. So the plan must contain, in the right order, every step a complete and correct module needs.

The domain has methods for these kinds of design: {others}. It has the actions for one more kind, {FAMILIES[family]['name']}, but no methods for it. Write the missing methods.

WHAT A PROBLEM OF THIS KIND STATES

{guide_section(FAMILIES[family]['section'])}

Facts named state_code and state_width, and objects named nN and bitsN, are added to a problem automatically.

RULES

- Write only (:task ...) declarations, for any new compound tasks you need, and (:method ...) definitions. Do not write or change actions, predicates or types, and do not repeat anything the domain already has.
- Use the domain's existing tasks and actions wherever they fit. Follow the way its other methods are written.
- Declare every variable a method uses in its :parameters, with its type. A variable must be bound by the method's task or by its precondition.
- An action can only be a step when its own precondition holds at that point in the plan.
- For any one problem there must be exactly one plan. Where two of your methods decompose the same task, their preconditions must never both hold. Your methods must not apply to the other kinds of design.

HOW YOUR ANSWER IS TESTED

Your methods are added to the domain. The planner then plans problems like the two below, and others of the same kind. The Verilog written from each plan is simulated and compared with what the design should do.

TWO EXAMPLE PROBLEMS

{examples[0]}

{examples[1]}

THE DOMAIN

```hddl
{reduced_domain(family).strip()}
```

Answer with one ```hddl code block that holds your (:task ...) and (:method ...) forms and nothing else."""


def repair_prompt(family, answer, diagnostic):
    return (prompt(family) + "\n\nYOUR PREVIOUS ANSWER\n\n```hddl\n" + answer.strip() + "\n```\n\n"
            "It was tested and failed:\n\n" + diagnostic.strip() + "\n\n"
            "Write the complete corrected answer, as one ```hddl code block.")


# ---------------------------------------------------------------------------
# Testing an answer

def answer_forms(reply):
    """(the answer's forms, None) or (None, why it cannot be used)."""
    blocks = re.findall(r"```(?:hddl|lisp|pddl)?\s*\n(.*?)```", reply, re.S)
    if not blocks:
        return None, "the reply holds no ``` code block"
    text = "(define\n" + blocks[-1] + "\n)"
    if text.count("(") != text.count(")"):
        return None, "the parentheses in the code block do not balance; the answer may have been cut off"
    forms = top_forms(text)
    bad = sorted({kind for _, _, kind, _ in forms if kind not in (":task", ":method")})
    if bad:
        return None, f"the code block holds {', '.join(bad)} forms; only (:task ...) and (:method ...) are allowed"
    if not any(kind == ":method" for _, _, kind, _ in forms):
        return None, "the code block holds no (:method ...)"
    return [text[s:e] for s, e, _, _ in forms], None


def test_answer(family, reply, workdir, tools):
    """The verdict on one answer: {"stage", "diagnostic", ...}. Uses the
    training problems and the other families' probes only."""
    os.makedirs(workdir, exist_ok=True)
    forms, why = answer_forms(reply)
    if forms is None:
        return {"stage": "format", "diagnostic": why}
    domain_path = os.path.join(workdir, "domain.hddl")
    with open(domain_path, "w") as f:
        f.write(with_answer(reduced_domain(family), forms))
    with open(os.path.join(workdir, "answer.hddl"), "w") as f:
        f.write("\n\n".join(forms) + "\n")
    train, _ = split(family)
    with ThreadPoolExecutor(8) as pool:
        results = list(pool.map(lambda c: try_case(domain_path, c, tools, seeds=(1, 2)), train))
    order = {s: i for i, s in enumerate(STAGES)}
    worst = min(range(len(train)), key=lambda i: order[results[i][0]])
    stage, detail = results[worst]
    passed = sum(r[0] == "pass" for r in results)
    if stage != "pass":
        case = train[worst]
        compiled, _, _ = problem_compiler.compile_text(case["problem"])
        head = {"load": "The planner could not load the domain with your methods added:",
                "plan": "The planner found no plan for this problem:",
                "ambiguous": "For this problem,", "render": "For this problem,",
                "simulate": "For this problem,"}[stage]
        diagnostic = (f"{passed} of the {len(train)} test problems passed.\n\n"
                      + (f"{head} {detail}" if stage == "load"
                         else f"Problem: {case['what']}\n\n```hddl\n{compiled.strip()}\n```\n\n{head} {detail}"))
        return {"stage": stage, "diagnostic": diagnostic, "train_passed": passed, "case": case["tag"]}

    # The other families must plan as they did.
    rows = [r for r in slice_domain()["probes"] if r["kind"] != family]

    def same(row):
        steps, message, _ = plan(domain_path, row["problem"])
        return steps is not None and [[n, list(a)] for n, a in steps] == row["steps"], message
    with ThreadPoolExecutor(8) as pool:
        kept = list(pool.map(same, rows))
    broken = [(r, m) for r, (ok, m) in zip(rows, kept) if not ok]
    if broken:
        row, message = broken[0]
        diagnostic = (f"All {len(train)} test problems passed, but your methods changed the plans of "
                      f"{len(broken)} problems of the other kinds of design. For example, a problem of the kind "
                      f"'{KIND_NAMES[row['kind']]}' "
                      + (f"no longer has a plan: {message}" if message
                         else "now gets a different plan: one of your methods applies to it.")
                      + " Your methods must apply only to " + FAMILIES[family]["name"] + ".")
        return {"stage": "regression", "diagnostic": diagnostic, "train_passed": passed,
                "broken": [r["name"] for r, _ in broken]}
    return {"stage": "pass", "diagnostic": "", "train_passed": passed}


def validate(family, domain_path, tools):
    """A passing answer on the family's other generated problems, which it
    was never shown or tested on, and against the hand-written methods' plan."""
    _, rest = split(family)

    def one(case):
        stage, detail = try_case(domain_path, case, tools)
        compiled, _, _ = problem_compiler.compile_text(case["problem"])
        ours, _, _ = plan(DOMAIN, compiled)
        return stage, stage == "pass" and detail == ours
    with ThreadPoolExecutor(8) as pool:
        results = list(pool.map(one, rest))
    return {"problems": len(rest), "passed": sum(s == "pass" for s, _ in results),
            "same_plan_as_hand_written": sum(same for _, same in results),
            "failed": [c["tag"] for c, (s, _) in zip(rest, results) if s != "pass"][:10],
            "stages": dict(collections.Counter(s for s, _ in results))}


# ---------------------------------------------------------------------------
# The loop

def executor(cfg, jobs, batch_path, n):
    if not jobs:
        return
    if subprocess.run(["pgrep", "-f", "executor_sample"], capture_output=True).returncode == 0:
        sys.exit("error: an executor_sample process is already running; stop it first")
    with open(batch_path, "w") as f:
        json.dump(jobs, f, indent=1)
    cmd = [os.path.join(ROOT, cfg["executor"]["binary"]), "--batch", batch_path,
           "--config", os.path.join(ROOT, cfg["executor"]["config"]), "--n", str(n)]
    print(f"{len(jobs)} request(s) to the model, {n} sample(s) each", flush=True)
    if subprocess.run(cmd, stdout=subprocess.DEVNULL).returncode != 0:
        sys.exit("error: the executor failed; see its message above. Rerun to resume.")


def generated(d, n):
    gen = os.path.join(d, "gen.json")
    if not os.path.exists(gen):
        return False
    g = json.load(open(gen))
    return g.get("n") == n and g.get("request_sha256") == rtl_eval.sha256_file(os.path.join(d, "request.json"))


def write_request(d, user):
    os.makedirs(d, exist_ok=True)
    body = json.dumps({"system": SYSTEM, "user": user}, indent=1)
    path = os.path.join(d, "request.json")
    if not os.path.exists(path) or open(path).read() != body:
        with open(path, "w") as f:
            f.write(body)
    return path


def cmd_slice(args, cfg, tools):
    methods, tasks = removed(args.family)
    print(f"removed methods ({len(methods)}): {' '.join(sorted(methods))}")
    print(f"removed tasks ({len(tasks)}): {' '.join(sorted(tasks))}")
    train, rest = split(args.family)
    print(f"training problems: {len(train)}; held-out problems: {len(rest)}")
    text = prompt(args.family)
    print(f"prompt: {len(text)} characters")
    if args.show:
        print(text)


def cmd_run(args, cfg, tools):
    """Round 1: one request, a sample per run. Each later round: every run
    that has not passed gets its last answer back with the diagnostic."""
    family, base = args.family, os.path.join(RUNS, args.family)
    os.makedirs(base, exist_ok=True)
    methods, tasks = removed(family)
    meta = {"created": rtl_eval.now(), "family": family, "runs": args.runs, "rounds": args.rounds,
            "removed_methods": sorted(methods), "removed_tasks": sorted(tasks),
            "train": [c["tag"] for c in split(family)[0]],
            "sha256": {"domain": rtl_eval.sha256_file(DOMAIN), "guide": rtl_eval.sha256_file(GUIDE),
                       "learn_family": rtl_eval.sha256_file(os.path.abspath(__file__)),
                       "executor_config": rtl_eval.sha256_file(os.path.join(ROOT, cfg["executor"]["config"]))}}
    with open(os.path.join(base, "run.json"), "w") as f:
        json.dump(meta, f, indent=1)

    answers = {}     # run -> its latest reply
    verdicts = {}    # run -> list of verdicts, one a round
    first = os.path.join(base, "round_1")
    request = write_request(first, prompt(family))
    if not generated(first, args.runs):
        executor(cfg, [{"request": request, "out_dir": first}], os.path.join(base, "batch.json"), args.runs)
    for r in range(args.runs):
        with open(os.path.join(first, f"sample_{r:02d}.response.txt"), errors="replace") as f:
            answers[r] = f.read()
    for rnd in range(1, args.rounds + 1):
        for r in sorted(answers):
            if verdicts.get(r) and verdicts[r][-1]["stage"] == "pass":
                continue
            work = os.path.join(base, f"run_{r:02d}", f"round_{rnd}")
            v = test_answer(family, answers[r], work, tools)
            if v["stage"] == "pass":
                v["validation"] = validate(family, os.path.join(work, "domain.hddl"), tools)
            with open(os.path.join(work, "verdict.json"), "w") as f:
                json.dump(v, f, indent=1)
            verdicts.setdefault(r, []).append(v)
            print(f"run {r:02d} round {rnd}: {v['stage']}"
                  + (f"; held-out {v['validation']['passed']}/{v['validation']['problems']}" if v["stage"] == "pass"
                     else ""), flush=True)
        open_runs = [r for r in sorted(answers) if verdicts[r][-1]["stage"] != "pass"]
        if not open_runs or rnd == args.rounds:
            break
        jobs = []
        for r in open_runs:
            d = os.path.join(base, f"run_{r:02d}", f"round_{rnd + 1}")
            forms, _ = answer_forms(answers[r])
            last = "\n\n".join(forms) if forms else answers[r][-3000:]
            request = write_request(d, repair_prompt(family, last, verdicts[r][-1]["diagnostic"]))
            if not generated(d, 1):
                jobs.append({"request": request, "out_dir": d})
        executor(cfg, jobs, os.path.join(base, "batch.json"), 1)
        for r in open_runs:
            d = os.path.join(base, f"run_{r:02d}", f"round_{rnd + 1}")
            with open(os.path.join(d, "sample_00.response.txt"), errors="replace") as f:
                answers[r] = f.read()
    with open(os.path.join(base, "verdicts.json"), "w") as f:
        json.dump({str(r): v for r, v in verdicts.items()}, f, indent=1)


def cmd_summary(args, cfg, tools):
    md = ["# Leave one family out: the model writes the methods back", "",
          "| family | methods removed | runs | pass in round 1 | 2 | 3 | never | held-out problems passed "
          "(runs that pass) | same plan as hand-written | requests to the model |",
          "|---|---|---|---|---|---|---|---|---|---|"]
    out = {}
    for family in FAMILIES:
        path = os.path.join(RUNS, family, "verdicts.json")
        if not os.path.exists(path):
            continue
        verdicts = json.load(open(path))
        meta = json.load(open(os.path.join(RUNS, family, "run.json")))
        solved = collections.Counter()
        stages = collections.Counter()
        held, same, total, requests = 0, 0, 0, 1
        for r, vs in verdicts.items():
            requests += len(vs) - 1
            if vs[-1]["stage"] == "pass":
                solved[len(vs)] += 1
                held += vs[-1]["validation"]["passed"]
                same += vs[-1]["validation"]["same_plan_as_hand_written"]
                total += vs[-1]["validation"]["problems"]
            else:
                solved["never"] += 1
                stages[vs[-1]["stage"]] += 1
        md.append(f"| {family} | {len(meta['removed_methods'])} | {len(verdicts)} | {solved[1]} | {solved[2]} | "
                  f"{solved[3]} | {solved['never']}"
                  + (f" ({', '.join(f'{k} {v}' for k, v in stages.items())})" if stages else "")
                  + f" | {held} of {total} | {same} of {total} | {requests} |")
        out[family] = {"solved_in_round": dict(solved), "last_failure": dict(stages), "held_out_passed": held,
                       "held_out_total": total, "same_plan": same, "requests": requests,
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
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("slice")
    s.add_argument("family", choices=sorted(FAMILIES))
    s.add_argument("--show", action="store_true", help="print the whole prompt")
    s = sub.add_parser("run")
    s.add_argument("family", choices=sorted(FAMILIES))
    s.add_argument("--runs", type=int, default=8, help="independent runs (default 8)")
    s.add_argument("--rounds", type=int, default=3, help="answers a run may give: the first and repairs (default 3)")
    sub.add_parser("summary")
    args = ap.parse_args()
    cfg, tools = rtl_eval.load_config(os.path.abspath(args.config))
    {"slice": cmd_slice, "run": cmd_run, "summary": cmd_summary}[args.cmd](args, cfg, tools)


if __name__ == "__main__":
    main()
