"""Plan the pilot problems (pilot/pilot.json), save each plan and its task tree,
and check that the plan is a complete set of instructions.

For each problem this compiles the problem file (problem_compiler.py: what
can be computed about it is, so the file itself holds only what the
description says), runs the planner, writes

    pilot/<bench>/<pid>.plan.txt    the plan, one action per line
    pilot/<bench>/<pid>.tree.json   the task tree behind it (the planner's -g)

and then renders the plan with plan_to_verilog.py's fixed templates and runs
the result through the benchmark's own functional check (eval/benchmarks.py).
A pass means the plan's steps, with nothing but their arguments, are enough to
write a module the benchmark accepts: no fact the code needs is missing from
the plan. The saved plans are what the P-series arms present to the executor.

    python rtl_designs/check_pilot.py            # all eight, about 20 s
    python rtl_designs/check_pilot.py --keep DIR # keep the rendered Verilog

Needs the planner built, and the evaluation harness set up (eval/README.md).
Exits non-zero on any failure.
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
sys.path.insert(0, os.path.join(ROOT, "eval"))

import plan_to_verilog  # noqa: E402
import problem_compiler  # noqa: E402
import rtl_eval  # noqa: E402


def plan(manifest, problem, tree_path):
    """Compile the problem (problem_compiler.py) and run the planner on it;
    return the planner's output, or exit with its error."""
    with open(problem) as f:
        compiled, _, _ = problem_compiler.compile_text(f.read())
    p = manifest["planner"]
    with tempfile.NamedTemporaryFile("w", suffix=".hddl", delete=False) as f:
        f.write(compiled)
    cmd = [os.path.join(ROOT, p["binary"]), "-D", os.path.join(ROOT, manifest["domain"]),
           "-P", f.name, "-F", p["score_fun"], "-T", str(p["time_limit_ms"]),
           "-r", str(p["simulations"]), "-s", str(p["seed"]), "-g", "-f", tree_path]
    run = subprocess.run(cmd, capture_output=True, text=True)
    os.unlink(f.name)
    if run.returncode != 0:
        errors = [line for line in run.stderr.splitlines() if not line.startswith("warning:")]
        sys.exit(f"planner failed on {problem}:\n" + "\n".join(errors))
    return run.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--keep", help="directory to keep the rendered Verilog in")
    ap.add_argument("--config", default=os.path.join(ROOT, "eval", "eval_config.json"))
    args = ap.parse_args()

    with open(os.path.join(HERE, "pilot", "pilot.json")) as f:
        manifest = json.load(f)
    cfg, tools = rtl_eval.load_config(args.config)
    adapters = {}
    failed = 0
    for entry in manifest["problems"]:
        bench, pid = entry["bench"], entry["pid"]
        base = os.path.join(HERE, "pilot", bench, pid)
        text = plan(manifest, base + ".hddl", base + ".tree.json")
        steps = plan_to_verilog.read_plan(text)
        with open(base + ".plan.txt", "w") as f:
            f.writelines("(" + " ".join([name] + a) + ")\n" for name, a in steps)
        verilog = plan_to_verilog.render(steps)

        ad = adapters.setdefault(bench, rtl_eval.bench_adapter(cfg, bench))
        problem = ad.load(pid)
        with tempfile.TemporaryDirectory() as w:
            v = os.path.join(w, "from_plan.v")
            with open(v, "w") as f:
                f.write(verilog)
            passed, code, log = ad.functional(problem, v, w, tools, cfg["timeouts_s"])
        if args.keep:
            os.makedirs(os.path.join(args.keep, bench), exist_ok=True)
            with open(os.path.join(args.keep, bench, pid + ".v"), "w") as f:
                f.write(verilog)
        print(f"{bench:16} {pid:24} {entry['role']:9} {len(steps):3} steps  "
              f"{'pass' if passed else 'FAIL (' + str(code) + ')'}")
        if not passed:
            failed += 1
            print("    " + "\n    ".join(log.strip().splitlines()[-6:]))
    print(f"{len(manifest['problems'])} problems, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
