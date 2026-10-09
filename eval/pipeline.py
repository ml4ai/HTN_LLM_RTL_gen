"""The pipeline end to end, with a fall-back: description -> problem file ->
plan -> prompt -> Verilog, on the pilot problems.

    description --(translate, greedy)--> problem file --(compile)--> --(plan)-->
        a usable plan?  yes: the plan prompt        (NL, one prompt, with the description)
                        no:  the direct prompt       (the description alone)
    --(executor, the measured call)--> Verilog

Everything before the executor's call is deterministic, and nothing in it
sees a testbench or a reference: the translation is one greedy reply, and the
route is decided by what can be known at run time. A translation falls back
to the direct prompt when

  - the reply holds no problem file that can be read,
  - the problem compiler or the planner's loader rejects it,
  - the planner finds no plan,
  - the fixed templates cannot render the plan, or
  - the module they render does not compile.

A translation that plans to a wrong design is not caught: nothing short of a
testbench can tell. That case is what the pipeline still risks, and the
summary shows it beside the direct baseline.

    python eval/pipeline.py translate   # the greedy translations
    python eval/pipeline.py build       # route each problem; write its request
    python eval/pipeline.py generate    # the executor: n = 20, or --greedy
    python eval/pipeline.py evaluate    # rtl_eval.py evaluate and report
    python eval/pipeline.py summary     # beside the direct baseline

build, generate and evaluate take --greedy for the greedy line. The run
directories are ordinary ones, eval/runs/pipeline and
eval/runs/pipeline_greedy, so rtl_eval.py's report and compare work on them.
The translations are in eval/runs/pipeline_translation. --manifest FILE runs
other problems than the pilot's, and --tag NAME keeps that run apart.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import checks  # noqa: E402
import pseries  # noqa: E402
import rtl_eval  # noqa: E402
import translate  # noqa: E402

PLAN_ARM = "p-nl-single-notree-desc"     # the presentation carried forward from the pilot
CONDITION = "exemplars"                  # the translation prompt: the guide and the worked examples
TRANSLATION = os.path.join(HERE, "runs", "pipeline_translation")
RUNS = {"sampled": os.path.join(HERE, "runs", "pipeline"), "greedy": os.path.join(HERE, "runs", "pipeline_greedy")}
BASELINE = {"sampled": os.path.join(HERE, "runs", "direct"), "greedy": os.path.join(HERE, "runs", "direct_greedy")}
# VerilogEval's rules-on baseline: the direct prompt with its own five-rule suffix.
RULES = {"sampled": os.path.join(HERE, "runs", "direct_rules"),
         "greedy": os.path.join(HERE, "runs", "direct_rules_greedy")}


def translation_dir(bench, pid):
    return os.path.join(TRANSLATION, bench, pid)


def retag(tag):
    """Keep a run apart from the pilot's: eval/runs/pipeline_<tag> and so on."""
    global TRANSLATION
    TRANSLATION += "_" + tag
    for mode in RUNS:
        RUNS[mode] += "_" + tag


def run_executor(ctx, jobs, batch_path, greedy, n):
    if not jobs:
        print("nothing to generate")
        return
    with open(batch_path, "w") as f:
        json.dump(jobs, f, indent=1)
    cmd = [os.path.join(ROOT, ctx["cfg"]["executor"]["binary"]), "--batch", batch_path,
           "--config", os.path.join(ROOT, ctx["cfg"]["executor"]["config"])]
    cmd += ["--greedy"] if greedy else ["--n", str(n)]
    print(f"{len(jobs)} requests to generate", flush=True)
    if subprocess.run(cmd, stdout=subprocess.DEVNULL).returncode != 0:
        sys.exit("error: the executor failed; see its message above. Rerun to resume.")


def done(d, n, mode):
    """Is the request in `d` generated, for this n and mode?"""
    req, gen = os.path.join(d, "request.json"), os.path.join(d, "gen.json")
    if not os.path.exists(gen):
        return False
    g = json.load(open(gen))
    return g.get("n") == n and g.get("mode") == mode and g.get("request_sha256") == rtl_eval.sha256_file(req)


def cmd_translate(args, ctx):
    jobs = []
    for bench, pid, _ in ctx["pairs"]:
        d = translation_dir(bench, pid)
        os.makedirs(d, exist_ok=True)
        body = json.dumps({"system": translate.SYSTEM,
                           "user": translate.build_prompt(CONDITION, ctx["problems"][(bench, pid)].extra["spec"])},
                          indent=1)
        req = os.path.join(d, "request.json")
        if not os.path.exists(req) or open(req).read() != body:
            with open(req, "w") as f:
                f.write(body)
        if not done(d, 1, "greedy"):
            jobs.append({"request": req, "out_dir": d})
    run_executor(ctx, jobs, os.path.join(TRANSLATION, "batch.json"), True, 1)


def route(bench, pid, ctx):
    """Which prompt the executor gets for this problem, and why. Uses the
    saved translation and nothing about the problem's testbench."""
    reply_path = os.path.join(translation_dir(bench, pid), "sample_00.response.txt")
    if not os.path.exists(reply_path):
        sys.exit(f"error: no translation of {pid}; run translate first")
    with open(reply_path, errors="replace") as f:
        t = translate.plan_translation(f.read(), ctx["manifest"])
    r = {"status": t["status"], "computed": t.get("computed", []), "problem": t.get("problem")}
    if t["status"] != "planned":
        return dict(r, route="direct", reason=t.get("detail", t["status"]))
    with tempfile.TemporaryDirectory() as w:
        v = os.path.join(w, "from_plan.v")
        with open(v, "w") as f:
            f.write(t["verilog"])
        ok, log = checks.syntax(v, w, ctx["tools"], ctx["cfg"]["timeouts_s"])
    if not ok:
        return dict(r, status="uncompilable", route="direct",
                    reason="the module the templates render from the plan does not compile: "
                           + " ".join(log.strip().splitlines()[-2:])[:200])
    return dict(r, route="plan", steps=t["steps"])


def cmd_build(args, ctx):
    mode = "greedy" if args.greedy else "sampled"
    n = 1 if args.greedy else ctx["cfg"]["ladder"]["n"]
    root = RUNS[mode]
    domain = pseries.Domain(os.path.join(ROOT, ctx["manifest"]["domain"]), pseries.GLOSS)
    routes = {}
    for bench, pid, _ in ctx["pairs"]:
        problem = ctx["problems"][(bench, pid)]
        r = route(bench, pid, ctx)
        if r["route"] == "plan":
            request = pseries.build_request(PLAN_ARM, bench, problem, domain, r["steps"], None)
        else:
            request = {"system": problem.system, "user": problem.user}
        d = os.path.join(root, bench, pid)
        os.makedirs(d, exist_ok=True)
        body = json.dumps(request, indent=1)
        req = os.path.join(d, "request.json")
        if os.path.exists(req) and open(req).read() != body and os.path.exists(os.path.join(d, "gen.json")):
            sys.exit(f"error: {req} differs from what is built now, and samples were generated from it. "
                     f"Move the run aside before rebuilding.")
        with open(req, "w") as f:
            f.write(body)
        saved = dict(r, steps=["(" + " ".join([name] + a) + ")" for name, a in r["steps"]]) if "steps" in r else r
        with open(os.path.join(d, "route.json"), "w") as f:
            json.dump(saved, f, indent=1)
        routes[f"{bench}/{pid}"] = {k: saved[k] for k in ("route", "status", "reason") if k in saved}
        print(f"{bench:16} {pid:24} {r['route']:6} " + (f"{len(r['steps'])} steps" if r["route"] == "plan"
                                                       else "<- " + r["reason"][:90]))
    benches = sorted({b for b, _, _ in ctx["pairs"]})
    files = {"pipeline": os.path.abspath(__file__), "translate": os.path.join(HERE, "translate.py"),
             "pseries": os.path.join(HERE, "pseries.py"), "guide": translate.GUIDE, "gloss": pseries.GLOSS,
             "domain": os.path.join(ROOT, ctx["manifest"]["domain"]),
             "problem_compiler": os.path.join(ROOT, "rtl_designs", "problem_compiler.py")}
    meta = {"created": rtl_eval.now(), "arm": "pipeline", "mode": mode, "n": n,
            "benchmarks": {b: {"commit": ctx["cfg"]["benchmarks"][b]["commit"],
                               "settings": ctx["cfg"]["benchmarks"][b]} for b in benches},
            "eval_config_sha256": rtl_eval.sha256_file(ctx["config"]),
            "executor_config_sha256": rtl_eval.sha256_file(os.path.join(ROOT, ctx["cfg"]["executor"]["config"])),
            "pipeline": {"plan_arm": PLAN_ARM, "translation": CONDITION, "routes": routes,
                         "sha256": {k: rtl_eval.sha256_file(v) for k, v in files.items()}}}
    meta_path = os.path.join(root, "run.json")
    if os.path.exists(meta_path):
        meta["created"] = json.load(open(meta_path))["created"]
    with open(meta_path, "w") as f:
        json.dump(meta, f, indent=1)


def cmd_generate(args, ctx):
    mode = "greedy" if args.greedy else "sampled"
    n = 1 if args.greedy else ctx["cfg"]["ladder"]["n"]
    jobs = []
    for bench, pid, _ in ctx["pairs"]:
        d = os.path.join(RUNS[mode], bench, pid)
        if not os.path.exists(os.path.join(d, "request.json")):
            sys.exit(f"error: no request for {pid}; run build first")
        if not done(d, n, mode):
            jobs.append({"request": os.path.join(d, "request.json"), "out_dir": d})
    run_executor(ctx, jobs, os.path.join(RUNS[mode], "batch.json"), args.greedy, n)


def cmd_evaluate(args, ctx):
    mode = "greedy" if args.greedy else "sampled"
    for sub in ("evaluate", "report"):
        cmd = [sys.executable, os.path.join(HERE, "rtl_eval.py"), "--config", ctx["config"], sub,
               "--run-dir", RUNS[mode]]
        if subprocess.run(cmd, stdout=subprocess.DEVNULL).returncode != 0:
            sys.exit(f"error: rtl_eval.py {sub} failed on {RUNS[mode]}")
    print(f"evaluated {RUNS[mode]}")


def cmd_summary(args, ctx):
    """Each problem's route and its passes, beside the direct baseline, for
    the sampled run and the greedy line."""
    md = ["# The pipeline end to end, with the fall-back", "",
          f"Plan prompt: `{PLAN_ARM}`. Translation: one greedy reply, the `{CONDITION}` prompt. "
          "Passes are functional passes of the executor's samples.", "",
          "| problem | role | route | pipeline, of 20 | direct, of 20 | rules on, of 20 | pipeline, greedy "
          "| direct, greedy | rules on, greedy |",
          "|---|---|---|---|---|---|---|---|---|"]
    out = {"problems": {}}
    tot = {k: {"poor": [], "adequate": []} for k in ("pipeline", "direct", "rules")}
    for bench, pid, role in ctx["pairs"]:
        rpath = os.path.join(RUNS["sampled"], bench, pid, "route.json")
        r = json.load(open(rpath)) if os.path.exists(rpath) else {"route": "—"}
        cells = {}
        for label, runs in (("pipeline", RUNS), ("direct", BASELINE), ("rules", RULES)):
            for mode in ("sampled", "greedy"):
                c = pseries.counts(runs[mode], bench, pid)
                cells[(label, mode)] = None if c is None else (c[1], c[2])
            if cells[(label, "sampled")]:
                f, n = cells[(label, "sampled")]
                tot[label][role].append(f / n)

        def show(c, greedy=False):
            if c is None:
                return "—"
            return ("pass" if c[0] else "fail") if greedy else str(c[0])
        why = "" if r["route"] != "direct" else f" ({r.get('status')})"
        md.append(f"| {pid} | {role} | {r['route']}{why} | {show(cells[('pipeline', 'sampled')])} | "
                  f"{show(cells[('direct', 'sampled')])} | {show(cells[('rules', 'sampled')])} | "
                  f"{show(cells[('pipeline', 'greedy')], True)} | {show(cells[('direct', 'greedy')], True)} | "
                  f"{show(cells[('rules', 'greedy')], True)} |")
        out["problems"][f"{bench}/{pid}"] = {"role": role, "route": r.get("route"), "status": r.get("status"),
                                             **{f"{a}_{m}": v for (a, m), v in cells.items()}}
    md.append("")
    for role in ("poor", "adequate"):
        p, d = tot["pipeline"][role], tot["direct"][role]
        if p and d:
            md.append(f"- **{role.capitalize()} problems:** pipeline {rtl_eval.pct(sum(p) / len(p))}%, "
                      f"direct {rtl_eval.pct(sum(d) / len(d))}% (functional pass@1 averaged over {len(p)} problems).")
            out[role] = {"pipeline": sum(p) / len(p), "direct": sum(d) / len(d)}
    os.makedirs(RUNS["sampled"], exist_ok=True)
    with open(os.path.join(RUNS["sampled"], "summary.json"), "w") as f:
        json.dump(out, f, indent=1)
    with open(os.path.join(RUNS["sampled"], "summary.md"), "w") as f:
        f.write("\n".join(md) + "\n")
    print("\n".join(md))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--config", default=os.path.join(HERE, "eval_config.json"))
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("translate")
    for name in ("build", "generate", "evaluate"):
        sub.add_parser(name).add_argument("--greedy", action="store_true")
    sub.add_parser("summary")
    ap.add_argument("--manifest", help="the problems to run (default: the pilot's)")
    ap.add_argument("--tag", help="keep this run apart: eval/runs/pipeline_<tag>")
    args = ap.parse_args()
    if args.tag:
        retag(args.tag)
    ctx = translate.setup(os.path.abspath(args.config), args.manifest and os.path.abspath(args.manifest))
    {"translate": cmd_translate, "build": cmd_build, "generate": cmd_generate,
     "evaluate": cmd_evaluate, "summary": cmd_summary}[args.cmd](args, ctx)


if __name__ == "__main__":
    main()
