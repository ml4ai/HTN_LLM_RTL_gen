# RTL designs: HTN plans as step-by-step instructions

A prototype of the pipeline

    design description --(translate)--> HDDL problem --(HTN planner)--> symbolic steps --(later: NL)--> LLM writes Verilog

without the natural-language parts. The translation step is done by hand here,
and each plan step is left in symbolic form.

| File | What it is |
|---|---|
| `rtl_domain.hddl` | The domain: how to design and write six families of small sequential designs (below). It began as the Mealy sequence detector |
| `fsm/design_description.txt` | The spec (given) |
| `fsm/fsm_problem.hddl` | The spec restated as facts: the hand-done translation step |
| `fsm/fsm_plan.txt` | The plan the planner returns: 42 symbolic steps |
| `fsm/fsm_plan.svg` | The task hierarchy behind that plan (open in a browser; hover for details) |
| `plan_to_verilog.py` | A check, not a pipeline stage: one fixed template per step |
| `check_sequence_detectors.py` | Plans, renders and simulates many patterns and variants against a Python model |
| `fsm/fsm_from_plan.v` | What those templates produce from the plan. Passes `fsm/testbench.v` |
| `rtl_domain_gloss.json` | One sentence per action, used to present a plan to an LLM (`eval/pseries.py`) |
| `state_encoding.py` | Computes an FSM's state width and codes and writes them into a problem file as facts. Counting is arithmetic, not planning, so the planner is given it |
| `pilot/` | Eight benchmark problems restated as facts, with their saved plans and task trees (below) |
| `check_pilot.py` | Plans the pilot problems, renders each plan and runs the benchmark's own testbench on it |

## Running it

From the repository root, after building:

    ./build/apps/planners/MCTS_planner -D rtl_designs/rtl_domain.hddl \
        -P rtl_designs/fsm/fsm_problem.hddl -F simple -T 10 -r 1 -s 1 > plan.out
    python rtl_designs/plan_to_verilog.py plan.out > fsm.v
    iverilog -o fsm.out fsm.v rtl_designs/fsm/testbench.v && vvp fsm.out

That takes about 0.6 s. Every decision in this domain is forced: all options
but one are provably dead. The planner commits each one as soon as that is
known, so `-T` is a cap rather than a cost, and raising it costs nothing
(`docs/planner_doc.md` §8.26). It still has to fit one cycle of rollouts, at
about 2 ms each. With the default of 5 per cycle that means `-T 25` or more,
and a 2.6 s run. With less, the planner stops with "exhausted its time limit
before evaluating any option at the root". An 8-bit pattern takes about 9 ms
per rollout and plans in about 4 s at `-T 25 -r 1`.

## What the steps mean

There are two kinds of step. `analyze_*` steps are design decisions the planner
works out; they produce no code, but each is a statement an instruction could
make. `write_*` steps are pieces of code to write.

The problem gives only the pattern (`expects s_k bit`), with one state per bit.
It does **not** give the transition table. The planner derives every
transition, including the overlap handling ("support for continuous input") that
is the error-prone part of this design, using the KMP failure-function
construction written as HTN methods. The domain's header comment has the details.
For `10011`:

| Step | Meaning |
|---|---|
| `analyze_advance fsm s3 one s4` | In `s3`, IN=1 is the next pattern bit, so go to `s4` |
| `analyze_fallback_extend fsm s4 s3 one s0 s1` | The longest suffix of `1001` that is also a pattern prefix is `1`, so `s4`'s fallback is `s1` |
| `analyze_fall_back fsm s4 zero s1 s2` | In `s4`, IN=0 breaks the match: go where fallback `s1` goes on 0, which is `s2` |
| `analyze_match_overlapping fsm s4 one s1 s1` | In `s4`, IN=1 completes `10011`: assert MATCH and continue from `s1` |
| `write_state_case fsm s4 IN s2 s1` | `s4: if (IN==0) ST_nt = s2; else ST_nt = s1;` |
| `write_output_assert fsm MATCH s4 IN one` | `else if (ST_cr == s4 && IN == 1) MATCH = 1;` |

The derived machine matches `fsm/verified_fsm.v` state for state, except that
the reference has a sixth state `s5` that behaves exactly like `s1`. The plan
reuses `s1`, which is the minimal machine.

## Coverage of the domain

The domain is hand-written, and so are the problem files. A reproducible
procedure for producing both is future work; these exist to test whether a
plan helps an LLM write the code at all.

| Family | What the planner works out |
|---|---|
| Mealy sequence detector | Every transition, from the pattern alone (above) |
| Moore sequence detector | The same, plus an accepting state whose output is 1 |
| Moore FSM from a state table or diagram | Nothing beyond reading the table; the plan is the code's structure |
| Run classifier (HDLC framing) | Every transition, from the run of 1s and the rules that end one |
| Wrap-around counter with an enable | The wrap point |
| Triangle-wave generator | Where the direction turns |

All of them have one clock and a reset that is asynchronous or synchronous, of
either polarity. The FSM families have one serial input. Multi-bit inputs,
several inputs per transition and other kinds of design are not modelled.

The Mealy derivation was checked beyond `10011` with `check_sequence_detectors.py`.
It covers every pattern of 1 to 5 bits and five longer ones (6 to 8 bits), in
both overlap modes, with all four reset styles represented (134
configurations). For each one it writes the problem, plans it, renders the
plan with `plan_to_verilog.py`, and simulates the result for 300 cycles against
a brute-force Python model. All 134 pass, in about 25 s. Rerun it after
changing the domain:

    python rtl_designs/check_sequence_detectors.py               # all 134
    python rtl_designs/check_sequence_detectors.py --max-len 2 -j 8   # 24, about 6 s

It needs the planner built and `iverilog` on `PATH`, and exits non-zero on any
failure. `--keep DIR` keeps the generated problems, Verilog and testbenches.

## The pilot problems

`pilot/pilot.json` lists eight problems from RTLLM 2.0 and VerilogEval v2: two
from each that a direct prompt fails, and two it handles. Each has a problem
file under `pilot/<benchmark>/`, with the description's own words beside the
facts read off them.

    python rtl_designs/check_pilot.py

That plans all eight (15 to 58 steps, about 20 s in all) and saves each plan
(`.plan.txt`) and its task tree (`.tree.json`, from the planner's
`-g -f x.json`). It then renders each plan with `plan_to_verilog.py` and runs
the benchmark's own testbench on the result. All eight pass, so each plan
carries every fact its code needs.

It needs the evaluation harness set up (`eval/README.md`). It plans with
`-T 200`: one rollout of the longest plan does not fit the 25 ms that is
enough for `fsm`.

The saved plans are what `eval/pseries.py` presents to the LLM: in four
formats, as one prompt or a conversation, with or without the task tree and
the description.
