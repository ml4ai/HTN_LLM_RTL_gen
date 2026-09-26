"""Check that rtl_domain.hddl plans correct sequence detectors for any pattern.

For each configuration (pattern, overlapping or not, reset style), this writes
the HDDL problem a translator would produce, runs the planner, renders the plan
with plan_to_verilog.py, and simulates the result with iverilog against a
brute-force Python model on random input with the pattern planted in it.

    python rtl_designs/check_sequence_detectors.py            # 134 configs
    python rtl_designs/check_sequence_detectors.py --max-len 3 --jobs 8

By default: every pattern of 1 to --max-len bits (5), plus the fsm example's
10011 (so a smaller --max-len still covers it) and five patterns of 6 to 8
bits, each with and without overlapping detection, with reset styles
rotated so that all four (async/sync x active-high/low) occur. Exits non-zero
if any configuration fails to plan or fails simulation.

Needs the planner built (build/apps/planners/MCTS_planner, or --planner) and
iverilog/vvp on PATH. --time-limit is the planner's budget per decision; the
default of 100 ms suits patterns up to 8 bits (about 32 ms per rollout there).
Running jobs in parallel slows each rollout, so raise it if a run reports
that the planner exhausted its time limit.
"""

import argparse
import itertools
import os
import random
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from plan_to_verilog import read_plan, render  # noqa: E402

DOMAIN = os.path.join(HERE, "rtl_domain.hddl")
DEFAULT_PLANNER = os.path.join(HERE, "..", "build", "apps", "planners", "MCTS_planner")
LONGER = ["10011", "101101", "110110", "0110110", "1010101", "11011011"]
STYLES = [(True, "active_high"), (True, "active_low"),
          (False, "active_high"), (False, "active_low")]


def problem(pat, overlap, async_, pol):
    """The HDDL problem for detecting pat, laid out as fsm/fsm_problem.hddl is."""
    n = len(pat)
    bit = {"0": "zero", "1": "one"}
    facts = ["(first_port fsm IN)", "(next_port IN CLK)", "(next_port CLK RST)",
             "(next_port RST MATCH)", "(last_port fsm MATCH)", "(input_port IN)",
             "(input_port CLK)", "(input_port RST)", "(output_port MATCH)",
             "(data_input_of fsm IN)", "(clock_of fsm CLK)", "(reset_of fsm RST)",
             "(detect_output_of fsm MATCH)", f"(reset_polarity RST {pol})",
             "(cleared_by_reset MATCH RST)", "(sequence_detector fsm)", "(mealy fsm)",
             "(initial_state fsm s0)", f"(last_state fsm s{n - 1})"]
    if async_:
        facts.append("(reset_async RST)")
    if overlap:
        facts.append("(overlapping fsm)")
    facts += [f"(next_prefix s{i} s{i + 1})" for i in range(n - 1)]
    facts += [f"(expects s{i} {bit[c]})" for i, c in enumerate(pat)]
    states = " ".join(f"s{i}" for i in range(n))
    return (f"(define (problem detect_{pat}) (:domain rtl_fsm)\n"
            f"  (:objects fsm - module IN CLK RST MATCH - port {states} - fsm_state)\n"
            f"  (:htn :parameters () :subtasks (and (implement_module fsm)))\n"
            f"  (:init {' '.join(facts)}))\n")


def expected(pat, overlap, bits):
    """MATCH for each input bit: 1 when the bits so far end with pat. Without
    overlapping detection, bits already used by a match cannot start another."""
    out, hist = [], ""
    for x in bits:
        hist += x
        hit = hist.endswith(pat)
        out.append(int(hit))
        if hit and not overlap:
            hist = ""
    return out


def testbench(bits, exp, pol):
    on, off = ("1", "0") if pol == "active_high" else ("0", "1")
    lines = ["`timescale 1ns/1ns",
             "module tb; reg CLK=0, RST, IN=0; wire MATCH; integer err=0;",
             "fsm DUT(.IN(IN),.CLK(CLK),.RST(RST),.MATCH(MATCH));",
             "always #5 CLK=~CLK;",
             f"initial begin RST={on}; repeat (2) @(negedge CLK); RST={off};"]
    # Reset is released on the same negedge that drives the first bit, so the
    # first posedge out of reset consumes exactly bits[0]. MATCH is Mealy, so
    # it is checked just after IN changes, before the posedge.
    for x, e in zip(bits, exp):
        lines.append(f"IN={x}; #1 if (MATCH!=={e}) err=err+1; @(negedge CLK);")
    lines.append('if (err==0) $display("PASS"); else $display("FAIL %0d", err); '
                 "$finish; end endmodule")
    return "\n".join(lines) + "\n"


def configs(max_len):
    pats = ["".join(p) for n in range(1, max_len + 1)
            for p in itertools.product("01", repeat=n)]
    pats += [p for p in LONGER if p not in pats]
    for i, pat in enumerate(pats):
        for overlap in (True, False):
            async_, pol = STYLES[(i + (0 if overlap else 1)) % 4]
            yield pat, overlap, async_, pol


def check(cfg, args, workdir, seed):
    pat, overlap, async_, pol = cfg
    tag = f"{pat}_{'ov' if overlap else 'no'}_{'async' if async_ else 'sync'}_{pol}"
    base = os.path.join(workdir, tag)
    with open(base + ".hddl", "w") as f:
        f.write(problem(pat, overlap, async_, pol))
    run = subprocess.run([args.planner, "-D", DOMAIN, "-P", base + ".hddl", "-F", "simple",
                          "-T", str(args.time_limit), "-r", "1", "-s", "1"],
                         capture_output=True, text=True)
    try:
        verilog = render(read_plan(run.stdout))
    except SystemExit as e:
        err = [l for l in (run.stdout + run.stderr).splitlines() if "error" in l.lower()]
        return tag, f"no plan ({e}; {' '.join(err) or 'planner exit ' + str(run.returncode)})"

    rng = random.Random(seed)
    bits = [rng.choice("01") for _ in range(args.cycles)]
    for k in range(0, args.cycles - len(pat), 23):   # plant the pattern so it matches
        bits[k:k + len(pat)] = list(pat)
    with open(base + ".v", "w") as f:
        f.write(verilog)
    with open(base + "_tb.v", "w") as f:
        f.write(testbench(bits, expected(pat, overlap, bits), pol))
    comp = subprocess.run(["iverilog", "-o", base + ".out", base + ".v", base + "_tb.v"],
                          capture_output=True, text=True)
    if comp.returncode:
        return tag, "iverilog: " + comp.stderr.strip()
    sim = subprocess.run(["vvp", base + ".out"], capture_output=True, text=True)
    if "PASS" not in sim.stdout:
        return tag, "simulation: " + (sim.stdout.strip().splitlines() or ["no output"])[0]
    return tag, None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--max-len", type=int, default=5,
                    help="test every pattern up to this many bits (default 5)")
    ap.add_argument("--time-limit", "-T", type=int, default=100,
                    help="planner budget per decision in ms (default 100)")
    ap.add_argument("--cycles", type=int, default=300, help="input bits simulated (default 300)")
    ap.add_argument("--jobs", "-j", type=int, default=4, help="configurations run at once (default 4)")
    ap.add_argument("--planner", default=DEFAULT_PLANNER, help="MCTS_planner binary")
    ap.add_argument("--keep", metavar="DIR",
                    help="write problems, Verilog and testbenches here instead of a temp dir")
    args = ap.parse_args()
    if not os.path.exists(args.planner):
        sys.exit(f"planner not found at {args.planner}; build it or pass --planner")

    cfgs = list(configs(args.max_len))
    with tempfile.TemporaryDirectory() as tmp:
        workdir = args.keep or tmp
        os.makedirs(workdir, exist_ok=True)
        with ThreadPoolExecutor(args.jobs) as pool:
            results = list(pool.map(lambda ic: check(ic[1], args, workdir, seed=ic[0]),
                                    enumerate(cfgs)))
    failures = [(t, why) for t, why in results if why]
    for tag, why in failures:
        print(f"FAIL {tag}: {why}")
    print(f"{len(cfgs)} configurations, {len(failures)} failed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
