#!/usr/bin/env python3
"""Check the counter and state-table families of rtl_domain.hddl on designs
generated here, not taken from a benchmark.

For each configuration: write the HDDL problem, compile it
(problem_compiler.py), run the planner, render the plan with
plan_to_verilog.py, and simulate the result against a Python model of the
design.

    python rtl_designs/check_counters_and_tables.py              # all of them
    python rtl_designs/check_counters_and_tables.py -j 8

Counters: six count ranges, each with and without an enable input, in all
four reset styles (async/sync x active-high/low). The ranges include one that
does not start at 0 and one that does not fill its width.

State tables: Moore machines of two to six states with random transitions and
one or two outputs, each 1 in a random, non-empty set of states, so an output
decoded from several states is covered as well as one decoded from a single
state. Reset styles are rotated.

Exits non-zero if any configuration fails to plan or fails simulation. Needs
the planner built (build/apps/planners/MCTS_planner, or --planner) and
iverilog/vvp on PATH.
"""

import argparse
import os
import random
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from plan_to_verilog import read_plan, render  # noqa: E402
import problem_compiler  # noqa: E402

DOMAIN = os.path.join(HERE, "rtl_domain.hddl")
DEFAULT_PLANNER = os.path.join(HERE, "..", "build", "apps", "planners", "MCTS_planner")
STYLES = [(True, "active_high"), (True, "active_low"),
          (False, "active_high"), (False, "active_low")]
RANGES = [(0, 15, 4), (0, 9, 4), (1, 10, 4), (0, 999, 10), (3, 6, 3), (0, 11, 4)]
MACHINES = 40


def ports(names, inputs):
    """The facts that list a module's ports in order and give their directions."""
    facts = [f"(first_port dut {names[0]})", f"(last_port dut {names[-1]})"]
    facts += [f"(next_port {a} {b})" for a, b in zip(names, names[1:])]
    facts += [f"({'input' if n in inputs else 'output'}_port {n})" for n in names]
    return facts


def reset_facts(async_, pol):
    return ["(clock_of dut clk)", "(reset_of dut rst)", f"(reset_polarity rst {pol})"] \
        + (["(reset_async rst)"] if async_ else [])


def problem_text(objects, facts):
    return ("(define (problem check) (:domain rtl_fsm)\n"
            f"  (:objects dut - module {objects})\n"
            "  (:htn :parameters () :subtasks (and (implement_module dut)))\n"
            f"  (:init {' '.join(facts)}))\n")


# ---- counters ----

def counter_configs():
    for low, high, width in RANGES:
        for enable in (True, False):
            for async_, pol in STYLES:
                yield ("counter", low, high, width, enable, async_, pol)


def counter_case(cfg, rng, cycles):
    _, low, high, width, enable, async_, pol = cfg
    names = ["clk", "rst"] + (["en"] if enable else []) + ["q"]
    facts = ports(names, {"clk", "rst", "en"}) + reset_facts(async_, pol) + [
        f"(port_width q bits{width})", "(wrap_counter dut)", "(count_output dut q)",
        f"(count_min dut n{low})", f"(count_max dut n{high})"]
    if enable:
        facts.append("(enable_of dut en)")
    tag = f"counter_{low}_{high}_{'en' if enable else 'free'}_{'async' if async_ else 'sync'}_{pol}"
    # One step a clock: (reset active, enable). The run must go round the
    # count more than once, so the enable is mostly on.
    steps = [(rng.random() < 0.02, rng.random() < 0.9) for _ in range(max(cycles, 3 * (high - low + 1)))]
    q, exp = low, []
    for rst, en in steps:
        if rst:
            q = low
        elif en or not enable:
            q = low if q == high else q + 1
        exp.append(q)
    drive = [(rst, f"en={int(en)};" if enable else "") for rst, en in steps]
    decl = "reg clk=0, rst" + (", en=0" if enable else "") + f"; wire [{width - 1}:0] q;"
    conn = ".clk(clk),.rst(rst)," + (".en(en)," if enable else "") + ".q(q)"
    return tag, problem_text(f"{' '.join(names)} - port", facts), \
        testbench(decl, conn, pol, drive, [f"q!=={e}" for e in exp])


# ---- state tables ----

def table_configs():
    for k in range(MACHINES):
        yield ("table", k) + STYLES[k % 4]


def table_case(cfg, rng, cycles):
    _, k, async_, pol = cfg
    n = rng.randint(2, 6)
    states = [chr(ord("A") + i) for i in range(n)]
    outs = ["z", "y"][:rng.randint(1, 2)]
    nxt = {s: (rng.choice(states), rng.choice(states)) for s in states}
    on = {o: set(rng.sample(states, rng.randint(1, n))) for o in outs}
    start = rng.choice(states)
    names = ["clk", "rst", "x"] + outs
    facts = ports(names, {"clk", "rst", "x"}) + reset_facts(async_, pol) + [
        "(table_fsm dut)", "(moore dut)", "(data_input_of dut x)", f"(initial_state dut {start})",
        f"(first_listed dut {states[0]})", f"(last_listed dut {states[-1]})"]
    facts += [f"(listed_after {a} {b})" for a, b in zip(states, states[1:])]
    for s in states:
        facts += [f"(table_next {s} zero {nxt[s][0]})", f"(table_next {s} one {nxt[s][1]})"]
    facts += [f"(asserted_in {o} {s})" for o in outs for s in states if s in on[o]]
    tag = f"table_{k:02d}_{n}states_{'async' if async_ else 'sync'}_{pol}"
    steps = [(rng.random() < 0.03, rng.randint(0, 1)) for _ in range(cycles)]
    state, checks = start, []
    for rst, x in steps:
        state = start if rst else nxt[state][x]
        checks.append(" || ".join(f"{o}!=={int(state in on[o])}" for o in outs))
    drive = [(rst, f"x={x};") for rst, x in steps]
    decl = f"reg clk=0, rst, x=0; wire {', '.join(outs)};"
    conn = ".clk(clk),.rst(rst),.x(x)," + ",".join(f".{o}({o})" for o in outs)
    return tag, problem_text(f"{' '.join(names)} - port {' '.join(states)} - fsm_state", facts), \
        testbench(decl, conn, pol, drive, checks)


def testbench(decl, conn, pol, drive, checks):
    """Inputs change on the falling edge and outputs are checked on the next
    one, after the rising edge between them has taken effect."""
    on, off = ("1", "0") if pol == "active_high" else ("0", "1")
    lines = ["`timescale 1ns/1ns", f"module tb; {decl} integer err=0;", f"dut DUT({conn});",
             "always #5 clk=~clk;", f"initial begin rst={on}; repeat (2) @(negedge clk);"]
    for (rst, inputs), check in zip(drive, checks):
        lines.append(f"rst={on if rst else off}; {inputs} @(negedge clk); if ({check}) err=err+1;")
    lines.append('if (err==0) $display("PASS"); else $display("FAIL %0d", err); $finish; end endmodule')
    return "\n".join(lines) + "\n"


def check(cfg, args, workdir, seed):
    rng = random.Random(seed)
    tag, problem, tb = (counter_case if cfg[0] == "counter" else table_case)(cfg, rng, args.cycles)
    base = os.path.join(workdir, tag)
    try:
        compiled, _, _ = problem_compiler.compile_text(problem)
    except (problem_compiler.Invalid, problem_compiler.Unreadable) as e:
        return tag, f"the problem does not compile: {e}"
    with open(base + ".hddl", "w") as f:
        f.write(compiled)
    run = subprocess.run([args.planner, "-D", DOMAIN, "-P", base + ".hddl", "-F", "simple",
                          "-i", str(args.iterations), "-r", "1", "-s", "1"],
                         capture_output=True, text=True)
    try:
        verilog = render(read_plan(run.stdout))
    except SystemExit as e:
        err = [l for l in (run.stdout + run.stderr).splitlines() if "error" in l.lower()]
        return tag, f"no plan ({e}; {' '.join(err) or 'planner exit ' + str(run.returncode)})"
    with open(base + ".v", "w") as f:
        f.write(verilog)
    with open(base + "_tb.v", "w") as f:
        f.write(tb)
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
    ap.add_argument("--iterations", "-i", type=int, default=100,
                    help="planner budget per decision, in search iterations (default 100)")
    ap.add_argument("--cycles", type=int, default=300, help="clock cycles simulated (default 300)")
    ap.add_argument("--jobs", "-j", type=int, default=4, help="configurations run at once (default 4)")
    ap.add_argument("--planner", default=DEFAULT_PLANNER, help="MCTS_planner binary")
    ap.add_argument("--keep", metavar="DIR",
                    help="write problems, Verilog and testbenches here instead of a temp dir")
    args = ap.parse_args()
    if not os.path.exists(args.planner):
        sys.exit(f"planner not found at {args.planner}; build it or pass --planner")

    cfgs = list(counter_configs()) + list(table_configs())
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
