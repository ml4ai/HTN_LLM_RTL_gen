"""The translation test: can the model turn a design description into the
problem file the planner needs, with no person in between?

For each pilot problem (rtl_designs/pilot/pilot.json) the model is given the
description and a guide to the domain's facts (rtl_designs/problem_guide.txt),
and writes an HDDL problem. Two conditions:

  guide       the guide alone
  exemplars   the guide, then one worked example per design family
              (rtl_designs/exemplars/): designs written for this purpose,
              none of them a benchmark problem

A translation is scored with no judgement involved, the way
rtl_designs/check_pilot.py checks the hand-written problems: it is planned,
the plan is rendered with plan_to_verilog.py's fixed templates, and the
benchmark's own testbench runs on the result. Each attempt ends as one of

  no_problem   the reply holds no problem file that can be read
  invalid      the planner's loader rejects it
  no_plan      it loads, and the planner finds no plan
  wrong        it plans, and the rendered module fails the testbench
  pass         it plans, and the rendered module passes

Before planning, the translated file goes through the problem compiler
(rtl_designs/problem_compiler.py), which does what can be computed: it
declares numbers and widths, names a state machine's data input when only one
port can be it, and works out the state encoding.

    python eval/translate.py show exemplars rtllm-2 fsm
    python eval/translate.py build      # requests, for both conditions
    python eval/translate.py generate   # one executor process; resumes
    python eval/translate.py score      # plan, render, test; writes summary.md

Each takes --greedy for the single greedy translation. Runs are in
eval/runs/translate/<condition> and eval/runs/translate_greedy/<condition>.

This measures the translation alone. The executor is not involved after it:
whether it then writes correct Verilog from the plan is the P-series
(pseries.py).
"""

import argparse
import collections
import concurrent.futures as cf
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "rtl_designs"))

import plan_to_verilog  # noqa: E402
import problem_compiler  # noqa: E402
import rtl_eval  # noqa: E402

MANIFEST = os.path.join(ROOT, "rtl_designs", "pilot", "pilot.json")
GUIDE = os.path.join(ROOT, "rtl_designs", "problem_guide.txt")
EXEMPLARS = os.path.join(ROOT, "rtl_designs", "exemplars")
CONDITIONS = ("guide", "exemplars")
RUNS = {"sampled": os.path.join(HERE, "runs", "translate"), "greedy": os.path.join(HERE, "runs", "translate_greedy")}
OUTCOMES = ("pass", "wrong", "no_plan", "invalid", "no_problem")
SYSTEM = "You are a helpful assistant."
PLANNER_SECONDS = 180


def exemplars():
    """(description, problem) pairs, in file-name order."""
    out = []
    for name in sorted(f[:-5] for f in os.listdir(EXEMPLARS) if f.endswith(".hddl")):
        with open(os.path.join(EXEMPLARS, name + ".txt")) as f:
            text = f.read().strip()
        with open(os.path.join(EXEMPLARS, name + ".hddl")) as f:
            problem = f.read().strip()
        out.append((text, problem))
    return out


def build_prompt(condition, description):
    with open(GUIDE) as f:
        parts = [f.read().strip()]
    if condition == "exemplars":
        parts.append("EXAMPLES")
        for i, (text, problem) in enumerate(exemplars(), 1):
            parts.append(f"Example {i}. Description:\n\n{text}\n\nProblem:\n\n```\n{problem}\n```")
    parts.append("DESCRIPTION TO TRANSLATE\n\n" + description.strip())
    parts.append("Write the problem file for this description.")
    return "\n\n".join(parts)


# ---------------------------------------------------------------------------
# Reading a reply back into a problem.

def extract_problem(reply):
    """The reply's problem file as text: the last fenced block that holds a
    (define ...), else the reply from its first (define. None if neither.
    Reading it is the problem compiler's."""
    blocks = re.findall(r"```[^\n]*\n(.*?)```", reply, re.S)
    for block in reversed(blocks):
        if "(define" in block:
            return block[block.index("(define"):]
    if "(define" in reply:
        return reply[reply.index("(define"):]
    return None


def stated(facts):
    """The facts without the computed state encoding, for comparing files."""
    return [f for f in facts if f[0] not in ("state_code", "state_width")]


def reference_facts(bench, pid):
    with open(os.path.join(ROOT, "rtl_designs", "pilot", bench, pid + ".hddl")) as f:
        _, facts, _ = problem_compiler.compile_text(f.read())
    return stated(facts)


def predicate_difference(facts, reference):
    """Which predicates the translation has more or fewer facts of than the
    hand-written problem: a pointer to what went wrong, not a score."""
    a = collections.Counter(f[0] for f in facts)
    b = collections.Counter(f[0] for f in reference)
    return {p: a[p] - b[p] for p in sorted(set(a) | set(b)) if a[p] != b[p]}


def plan_translation(reply, manifest):
    """From a reply to a plan, using nothing but the reply: read the problem,
    compile it, plan it, and render the plan with the fixed templates. No
    testbench and no reference is involved, so everything this reports can be
    known at run time.

    Returns a dict with "status":
      no_problem   nothing in the reply reads as a problem file
      invalid      it reads, and is rejected before or by the planner's loader
      no_plan      it loads, and the planner finds no plan
      unrendered   it plans, and the templates cannot render the plan
      planned      it plans and renders: "steps" and "verilog" are set
    and, where they exist, "problem" (the compiled file), "facts", "computed"
    (what the compiler added) and "detail" (why it stopped)."""
    text = extract_problem(reply)
    if text is None:
        return {"status": "no_problem", "detail": "no (define ...) in the reply"}
    try:
        problem_text, facts, computed = problem_compiler.compile_text(text)
    except problem_compiler.Invalid as e:
        return {"status": "invalid", "detail": str(e), "problem": text}
    except problem_compiler.Unreadable as e:
        return {"status": "no_problem", "detail": str(e)}
    out = {"problem": problem_text, "facts": facts, "computed": computed}
    p = manifest["planner"]
    with tempfile.TemporaryDirectory() as w:
        path = os.path.join(w, "problem.hddl")
        with open(path, "w") as f:
            f.write(problem_text)
        cmd = ["perl", "-e", "alarm shift; exec @ARGV", str(PLANNER_SECONDS),
               os.path.join(ROOT, p["binary"]), "-D", os.path.join(ROOT, manifest["domain"]), "-P", path,
               "-F", p["score_fun"], "-T", str(p["time_limit_ms"]), "-r", str(p["simulations"]), "-s", str(p["seed"])]
        run = subprocess.run(cmd, capture_output=True, text=True)
    errors = [line for line in run.stderr.splitlines() if not line.startswith("warning:")]
    if run.returncode != 0 or "Plan:" not in run.stdout or "Plan found at depth 0" in run.stdout:
        message = " ".join(errors)[:300] or f"planner stopped (exit {run.returncode})"
        searching = re.search(r"no applicable decomposition|time limit|decision|rollout|no plan", message, re.I)
        timed_out = run.returncode < 0 or run.returncode == 142
        return dict(out, status="no_plan" if (searching or timed_out) else "invalid", detail=message)
    try:
        steps = plan_to_verilog.read_plan(run.stdout)
        verilog = plan_to_verilog.render(steps)
    except (SystemExit, Exception) as e:
        # The templates assume a plan from a sound problem. One from a
        # translation may lack what they need, say a port's width.
        return dict(out, status="unrendered", detail=f"the plan does not render: {type(e).__name__}: {e}")
    return dict(out, status="planned", steps=steps, verilog=verilog)


def score_reply(reply, bench, pid, ctx):
    """One translation's outcome, with what explains it: plan_translation,
    then the benchmark's testbench on the rendered module."""
    t = plan_translation(reply, ctx["manifest"])
    out = {k: t[k] for k in ("problem", "computed", "detail") if k in t}
    if "facts" in t:
        out["differs_from_reference"] = predicate_difference(stated(t["facts"]), ctx["reference"][(bench, pid)])
    if t["status"] != "planned":
        # A plan the templates cannot render is a wrong plan, as it was scored
        # before the two were told apart.
        return dict(out, outcome="wrong" if t["status"] == "unrendered" else t["status"])
    with tempfile.TemporaryDirectory() as w:
        v = os.path.join(w, "from_plan.v")
        with open(v, "w") as f:
            f.write(t["verilog"])
        passed, code, log = ctx["adapters"][bench].functional(ctx["problems"][(bench, pid)], v, w, ctx["tools"],
                                                              ctx["cfg"]["timeouts_s"])
    out["steps"] = len(t["steps"])
    if passed:
        return dict(out, outcome="pass")
    return dict(out, outcome="wrong", detail=" ".join(log.strip().splitlines()[-3:])[:300])


# ---------------------------------------------------------------------------
# Commands.

def setup(config):
    cfg, tools = rtl_eval.load_config(config)
    with open(MANIFEST) as f:
        manifest = json.load(f)
    pairs = [(e["bench"], e["pid"], e["role"]) for e in manifest["problems"]]
    adapters = {b: rtl_eval.bench_adapter(cfg, b) for b in sorted({b for b, _, _ in pairs})}
    problems = {(b, p): adapters[b].load(p) for b, p, _ in pairs}
    return {"cfg": cfg, "tools": tools, "manifest": manifest, "pairs": pairs, "adapters": adapters,
            "problems": problems, "reference": {(b, p): reference_facts(b, p) for b, p, _ in pairs},
            "config": config}


def request_dir(mode, condition, bench, pid):
    return os.path.join(RUNS[mode], condition, bench, pid)


def cmd_show(args, ctx):
    print(build_prompt(args.condition, ctx["problems"][(args.bench, args.pid)].extra["spec"]))


def cmd_build(args, ctx):
    mode = "greedy" if args.greedy else "sampled"
    n = 0
    for condition in CONDITIONS:
        for bench, pid, _ in ctx["pairs"]:
            d = request_dir(mode, condition, bench, pid)
            os.makedirs(d, exist_ok=True)
            body = json.dumps({"system": SYSTEM,
                               "user": build_prompt(condition, ctx["problems"][(bench, pid)].extra["spec"])}, indent=1)
            req = os.path.join(d, "request.json")
            if os.path.exists(req) and open(req).read() != body and os.path.exists(os.path.join(d, "gen.json")):
                sys.exit(f"error: {req} differs from what is built now, and samples were generated from it. "
                         f"Move the run aside before rebuilding.")
            with open(req, "w") as f:
                f.write(body)
            n += 1
    files = {"guide": GUIDE, "builder": os.path.abspath(__file__), "manifest": MANIFEST,
             "domain": os.path.join(ROOT, ctx["manifest"]["domain"])}
    for name in sorted(os.listdir(EXEMPLARS)):
        files["exemplar:" + name] = os.path.join(EXEMPLARS, name)
    meta = {"created": rtl_eval.now(), "mode": mode, "n": 1 if args.greedy else ctx["cfg"]["ladder"]["n"],
            "conditions": list(CONDITIONS), "sha256": {k: rtl_eval.sha256_file(v) for k, v in files.items()},
            "executor_config_sha256": rtl_eval.sha256_file(os.path.join(ROOT, ctx["cfg"]["executor"]["config"]))}
    with open(os.path.join(RUNS[mode], "run.json"), "w") as f:
        json.dump(meta, f, indent=1)
    print(f"{n} requests written under {RUNS[mode]}")


def cmd_generate(args, ctx):
    mode = "greedy" if args.greedy else "sampled"
    n = 1 if args.greedy else ctx["cfg"]["ladder"]["n"]
    jobs = []
    for condition in CONDITIONS:
        for bench, pid, _ in ctx["pairs"]:
            d = request_dir(mode, condition, bench, pid)
            req, gen = os.path.join(d, "request.json"), os.path.join(d, "gen.json")
            if not os.path.exists(req):
                sys.exit(f"error: {req} missing; run build first")
            if os.path.exists(gen):
                g = json.load(open(gen))
                if g.get("n") == n and g.get("mode") == mode and g.get("request_sha256") == rtl_eval.sha256_file(req):
                    continue
            jobs.append({"request": req, "out_dir": d})
    if not jobs:
        print("nothing to generate")
        return
    batch = os.path.join(RUNS[mode], "batch.json")
    with open(batch, "w") as f:
        json.dump(jobs, f, indent=1)
    cmd = [os.path.join(ROOT, ctx["cfg"]["executor"]["binary"]), "--batch", batch,
           "--config", os.path.join(ROOT, ctx["cfg"]["executor"]["config"])]
    cmd += ["--greedy"] if args.greedy else ["--n", str(n)]
    print(f"{len(jobs)} requests to generate ({mode})", flush=True)
    if subprocess.run(cmd, stdout=subprocess.DEVNULL).returncode != 0:
        sys.exit("error: the executor failed; see its message above. Rerun generate to resume.")


def cmd_score(args, ctx):
    mode = "greedy" if args.greedy else "sampled"
    jobs = []
    for condition in CONDITIONS:
        for bench, pid, _ in ctx["pairs"]:
            d = request_dir(mode, condition, bench, pid)
            gen = os.path.join(d, "gen.json")
            if not os.path.exists(gen):
                continue
            for i, s in enumerate(json.load(open(gen))["samples"]):
                jobs.append((condition, bench, pid, i, os.path.join(d, s["file"]), s["finish_reason"]))

    def run(job):
        condition, bench, pid, i, path, finish = job
        with open(path, errors="replace") as f:
            r = score_reply(f.read(), bench, pid, ctx)
        r.update(condition=condition, bench=bench, pid=pid, sample=i, finish_reason=finish)
        return r
    with cf.ThreadPoolExecutor(args.jobs) as ex:
        results = list(ex.map(run, jobs))
    with open(os.path.join(RUNS[mode], "results.jsonl"), "w") as f:
        for r in results:
            f.write(json.dumps(r) + "\n")

    by = collections.defaultdict(collections.Counter)
    for r in results:
        by[(r["condition"], r["bench"], r["pid"])][r["outcome"]] += 1
    md = [f"# Translation test, {mode}", "",
          "Translations of each pilot problem's description, scored by planning them, rendering the plan with "
          "fixed templates and running the benchmark's testbench. Counts are translations; `pass` is the number "
          "that yield a plan whose rendered module passes.", ""]
    for condition in CONDITIONS:
        rows = [(b, p, role, by[(condition, b, p)]) for b, p, role in ctx["pairs"] if by[(condition, b, p)]]
        if not rows:
            continue
        md += [f"## {condition}", "", "| problem | role | " + " | ".join(OUTCOMES) + " |",
               "|---|---|" + "---|" * len(OUTCOMES)]
        total = collections.Counter()
        for b, p, role, c in rows:
            md.append(f"| {p} | {role} | " + " | ".join(str(c[o]) for o in OUTCOMES) + " |")
            total.update(c)
        n = sum(total.values())
        md += [f"| **all** | | " + " | ".join(f"**{total[o]}**" for o in OUTCOMES) + " |", "",
               f"{total['pass']} of {n} translations pass ({rtl_eval.pct(total['pass'] / n)}%); "
               f"{sum(1 for _, _, _, c in rows if c['pass'] == sum(c.values()))} of {len(rows)} problems pass on "
               f"every translation, and {sum(1 for _, _, _, c in rows if c['pass'] == 0)} on none.", ""]
    with open(os.path.join(RUNS[mode], "summary.md"), "w") as f:
        f.write("\n".join(md) + "\n")
    print("\n".join(md))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--config", default=os.path.join(HERE, "eval_config.json"))
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("show")
    s.add_argument("condition", choices=CONDITIONS)
    s.add_argument("bench")
    s.add_argument("pid")
    for name in ("build", "generate", "score"):
        s = sub.add_parser(name)
        s.add_argument("--greedy", action="store_true")
        if name == "score":
            s.add_argument("--jobs", type=int, default=8)
    args = ap.parse_args()
    args.config = os.path.abspath(args.config)
    ctx = setup(args.config)
    {"show": cmd_show, "build": cmd_build, "generate": cmd_generate, "score": cmd_score}[args.cmd](args, ctx)


if __name__ == "__main__":
    main()
