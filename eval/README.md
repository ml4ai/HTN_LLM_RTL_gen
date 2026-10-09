# RTL evaluation harness

Measures how often the local executor (`executor/`) writes Verilog that is
syntactically valid and functionally correct, on **VerilogEval v2**
(spec-to-RTL, 156 problems) and **RTLLM 2.0** (50 designs).

- **Two pass@k ladders from the same samples:** syntax and functional, at
  k ∈ {1, 5, 10}, computed with Chen et al.'s unbiased estimator averaged over
  problems, from n = 20 samples per problem.
- **A greedy line:** one greedy reply per problem.
- **Single-pass only.** No check's result ever reaches the model.

| File | What it is |
|---|---|
| `eval_config.json` | Every evaluation setting, pinned: tool paths and versions, flags, timeouts, benchmark commits, ladder |
| `rtl_eval.py` | The command-line driver: `selftest`, `generate`, `evaluate`, `report`, `breakdown`, `compare` |
| `pseries.py` | The P-series: the 32 ways of presenting a planner's plan to the executor, on the pilot problems (below) |
| `translate.py` | The translation test: the model writes the planner's problem file from a design description (below) |
| `pipeline.py` | The pipeline end to end, with a fall-back to the direct prompt when the translation gives no usable plan (below) |
| `benchmarks.py` | Per benchmark: problems, prompt template, code extraction, golden reference, functional test |
| `checks.py` | Icarus Verilog and Yosys: syntax, compile-and-simulate, synthesis, the stuck-at-0 stub |
| `passk.py` | The estimator, ladders, the syntax/conditional split, bootstrap, McNemar |
| `test_eval.py` | Unit tests (`python eval/test_eval.py`) |

Run outputs go to `eval/runs/`, which git ignores.

## Setup

- **The executor**, built with the `llama` env on the prefix path
  (`executor/README.md`); the harness calls `build/executor/executor_sample`.
- **Icarus Verilog 12.0** in the conda env `iverilog12`, the version VerilogEval
  v2 requires. It was built from source (`v12-branch`, commit `4fd52916`), since
  there is no conda package for Apple Silicon. The harness calls it by absolute
  path, because a Homebrew Icarus 13 may be first on `PATH`.
- **Yosys 0.60** at `/usr/local/bin/yosys`.
- **The benchmarks**, cloned outside the repo at the commits `eval_config.json`
  pins; the harness refuses any other commit:

      git clone https://github.com/NVlabs/verilog-eval ~/benchmarks/verilog-eval   # c498220d
      git clone https://github.com/hkust-zhiyao/RTLLM ~/benchmarks/RTLLM            # 51ed553d

The Python is standard library only; any Python 3.9+ works.

## Running it

    PY=/opt/anaconda3/envs/irl/bin/python
    $PY eval/rtl_eval.py selftest --bench verilog-eval-v2      # ~20 s; once per benchmark
    $PY eval/rtl_eval.py selftest --bench rtllm-2              # ~35 s
    $PY eval/rtl_eval.py generate --bench verilog-eval-v2 --run-dir eval/runs/direct   # n = 20
    $PY eval/rtl_eval.py generate --bench rtllm-2         --run-dir eval/runs/direct
    $PY eval/rtl_eval.py evaluate --run-dir eval/runs/direct
    $PY eval/rtl_eval.py report   --run-dir eval/runs/direct  # writes report.md and report.json

- **The greedy line** is a separate run directory: `generate --greedy`.
- **A subset** is `--problems P1 P2 …` or `--limit K`.
- **Interrupting is safe.** `generate` resumes, skipping every problem whose
  `gen.json` is complete for the same request.
- **Comparing two arms** (say planner vs direct) on one benchmark, paired by
  problem:

      $PY eval/rtl_eval.py compare --a eval/runs/planner --b eval/runs/direct --bench rtllm-2

  It reports the functional pass@1 difference split exactly into a syntax term
  and a conditional term, the difference at every rung of both ladders with
  bootstrap confidence intervals, and, for greedy runs, McNemar's test.
- **Breaking a run down by problem type** (about 10 s):

      $PY eval/rtl_eval.py breakdown --run-dir eval/runs/direct --greedy-run eval/runs/direct_greedy

  It writes `breakdown.md` and `breakdown.json` into the run directory. The
  ladders are split by design class (FSM, sequential or combinational, read off
  the reference), by spec form on VerilogEval (prose, waveform, Karnaugh map),
  and by RTLLM's own folders, and the never-solved problems are listed.

  It also runs a **reset audit**. For each spec that names a reset style, it
  counts the samples using the other style. For synchronous-reset specs, it
  re-simulates each failing sample with the asynchronous reset made synchronous,
  and reports the pass@1 that the reset style alone costs. This is a diagnostic
  only; scores never change. `--no-resim` skips the re-simulation.

**Time.** With the 32B executor, 20 samples of a problem are decoded as one
batch of 20 sequences, at about 37 tokens/s in aggregate, against 9.7 for a
single sequence. A problem takes about 10 s to 4 minutes, depending on its
longest reply. A full sampled run of both benchmarks is roughly 6–8 hours, so
run it overnight *(estimate, from the smoke runs)*. The greedy line takes about
an hour per benchmark.

## The P-series: presenting a plan

`pseries.py` builds the arms that put an HTN plan in front of the executor,
for the eight pilot problems of `rtl_designs/pilot/` (four from each
benchmark). An arm is one level of each of four factors, named
`p-<format>-<delivery>-<tree>-<description>`:

| Factor | Levels |
|---|---|
| Format | `s0` the plan's s-expressions as emitted · `s1` with named arguments · `s2` s1 plus a schema saying what each action means · `nl` one sentence per step |
| Delivery | `single` one prompt · `micro` a conversation, one turn per top-level part of the plan and a last turn asking for the module |
| Tree | `notree` · `tree` the task tree behind the plan, as an outline |
| Description | `desc` the design description, then the plan · `nodesc` the plan alone |

That is 32 arms. Each run directory is an ordinary one, `eval/runs/pseries/<arm>`
(and `eval/runs/pseries_greedy/<arm>`), so `evaluate`, `report` and `compare`
work on it.

    $PY eval/pseries.py show p-s2-single-tree-desc rtllm-2 fsm   # print one prompt
    $PY eval/pseries.py build      # write every arm's requests and run.json
    $PY eval/pseries.py generate   # run the executor on what is not done; resumes
    $PY eval/pseries.py evaluate   # evaluate and report, per arm
    $PY eval/pseries.py summary    # all arms beside the direct and rules-on baselines

- **`--greedy`** on any of them does the greedy line.
- **`--arms A B …` or `--delivery single|micro`** does part of the factorial.
- **The frame is the benchmark's own,** so its code extraction applies:
  VerilogEval's template with the plan where its optional rules go, and RTLLM's
  description followed by the plan.
- **The plans are saved ones** (`rtl_designs/check_pilot.py` writes and checks
  them); nothing here runs the planner. `run.json` records the hash of every
  file a prompt is built from.
- **Micro-prompting** follows the single-pass policy: each reply is fed back
  as generated, nothing checks one, and only the last reply is evaluated.
- **Time** (32B executor): about 7 minutes for a single-prompt request of 20
  samples on an FSM problem, and 17 for a micro-prompt one. The whole
  factorial, sampled and greedy, is about two days.

## The translation test: from a description to a problem file

`translate.py` asks the model to write the HDDL problem for each pilot problem,
given the design description and a guide to the domain's facts
(`rtl_designs/problem_guide.txt`). There are two conditions: `guide`, the guide
alone, and `exemplars`, the guide followed by one worked example per design
family (`rtl_designs/exemplars/`).

    $PY eval/translate.py show exemplars rtllm-2 fsm   # print one prompt
    $PY eval/translate.py build      # requests, for both conditions
    $PY eval/translate.py generate   # one executor process; resumes
    $PY eval/translate.py score      # plan, render, test; writes summary.md

Each takes `--greedy`. Runs are in `eval/runs/translate/` and
`eval/runs/translate_greedy/`.

A translation is scored with no judgement involved:

1. It goes through the problem compiler (`rtl_designs/problem_compiler.py`),
   which adds what can be computed: declarations of numbers and widths, a state
   machine's data input when only one port can be it, and the state encoding.
2. The planner plans it.
3. The plan is rendered with `plan_to_verilog.py`'s fixed templates.
4. The benchmark's own testbench runs on the result.

It ends as `no_problem` (nothing readable), `invalid` (the loader rejects it),
`no_plan`, `wrong` (it plans, and the module fails) or `pass`. The first three
can be detected without a testbench; `wrong` cannot.

This measures the translation alone. The executor is not asked to write Verilog
from these plans; that is the P-series.

## The pipeline end to end

`pipeline.py` runs the whole chain on the pilot problems: a greedy translation
of the description, the problem compiler, the planner, and then the executor's
measured call.

    $PY eval/pipeline.py translate   # the greedy translations
    $PY eval/pipeline.py build       # route each problem; write its request
    $PY eval/pipeline.py generate    # the executor: n = 20, or --greedy
    $PY eval/pipeline.py evaluate
    $PY eval/pipeline.py summary     # beside the direct baseline

- **The route.** A problem whose translation yields a usable plan gets the plan
  prompt (NL, one prompt, with the description). Otherwise it gets the direct
  prompt, the description alone.
- **Which direct prompt.** For a VerilogEval problem, the one with the
  benchmark's rules suffix on (`generate --rules`), since that is the stronger
  baseline there. RTLLM has no rules option, so its problems get the plain
  direct prompt. `route.json` records which, as `prompt`.
- **When it falls back.** The reply holds no readable problem file; the
  compiler or the planner's loader rejects it; the planner finds no plan; the
  templates cannot render the plan; or the module they render does not compile.
- **What it cannot catch.** A translation that plans to a wrong design. Nothing
  short of a testbench can tell, and the routing never uses one.
- **Each problem's route** and the reason are in its `route.json` and in
  `run.json`.

The run directories, `eval/runs/pipeline` and `eval/runs/pipeline_greedy`, are
ordinary ones, so `report` and `compare` work on them.

Options go before the command:

    $PY eval/pipeline.py --manifest rtl_designs/pilot/heldout.json --tag heldout build

- **`--manifest`** runs other problems than the pilot's.
- **`--tag`** keeps a run apart, in `eval/runs/pipeline_<tag>`.
- **`--votes K`** is the agreement rule, a partial answer to the wrong
  translation that plans. The description is translated K times, sampled, and
  a plan is used only if more than half of the K translations give it.
  Otherwise the problem gets the direct prompt. Two translations agree if
  their plans are the same once each state is renamed by its code; a
  translation with no usable plan agrees with nothing. Each problem's
  `route.json` holds the ballot. It still uses no testbench, and it does not
  catch a wrong translation that most of the samples share.
- **`--reuse DIR`** (with `generate`) copies the samples of any problem whose
  request is byte for byte the one in `DIR`, in place of generating them again.
  The seeds are fixed, so they are the samples this run would produce. The
  direct baselines, rules off and on, are always looked in. Give it once per directory.

## What each check means

For every sample, in order, stopping at the first failure:

1. **Extraction**, per benchmark (below). No module means an **abstention**.
2. **Syntax:** the extracted code alone compiles in Icarus 12 with
   `-Wall -Winfloop -Wno-timescale -g2012`, the flags VerilogEval uses. There is
   no `-s`, so a wrong module name is not a syntax failure.
3. **Functional:** the benchmark's own test (below). A compile failure *here*
   happens after the module compiled alone, so it is an **interface error**, a
   module that does not bind to the testbench.

**Reported beside the ladders, never part of pass/fail:**
- synthesizability in Yosys (`read_verilog -sv; synth -top`), with Yosys front-end
  errors kept apart from synthesis errors;
- a strict Verilog-2005 check (`-g2005`), to show how often "Verilog" comes
  back as SystemVerilog;
- how many replies were cut off at `max_new_tokens`.

**Failure breakdown categories:** abstention, syntax error, interface error,
simulation fail, timeout.

### VerilogEval v2

Everything follows its own scripts, ported line for line:

- **Prompt:** `sv-generate`'s spec-to-RTL template, with its system message,
  the `Question:` / `Answer:` frame and "Enclose your code with [BEGIN] and
  [DONE]". In-context examples are 0 and the rules are off, `sv-generate`'s
  defaults.
- **Extraction:** the code between `[BEGIN]` and `[DONE]`, else its backtick
  fallback.
- **Test:** sample, test and reference compiled together with `-s tb`,
  simulated for up to 30 s, and classified exactly as `sv-iv-analyze` does. A
  pass is its `.`: no error in the log, no `TIMEOUT`, and `Mismatches: 0`.

### RTLLM 2.0

- **Prompt:** the design description as given, under the executor's default
  system message ("You are a helpful assistant.").
- **Extraction:** the last fenced code block that defines the design's module,
  else the last block defining any module. Preferring the design's own module
  keeps a testbench the model appends from being taken for the design.
- **Test:** RTLLM's Makefile, ported from the proprietary VCS to Icarus. The
  testbench is compiled with the sample and simulated, with the design's data
  files alongside. A pass means the output contains `Pass` or `pass`
  (`auto_run.py`'s rule).
- **Time unit:** the Makefiles give VCS `-timescale=1ns/1ns`. Icarus has no such
  flag, so a file holding only `` `timescale 1ns/1ns `` is compiled first, which
  gives files without their own timescale the same default.

## The self-test, and what it found

`selftest` checks the harness against each benchmark before any run counts:

- **Every golden reference must pass** the syntax and functional checks.
  Problems whose golden fails are excluded from every report, and listed.
- **A stuck-at-0 stub must fail.** The stub is a module with the golden's ports
  and every output tied to 0. A testbench it passes cannot tell a working
  design from a dead one. Such problems are flagged, and the report adds a
  functional ladder without them.

On the pinned tools and commits:

| | VerilogEval v2 | RTLLM 2.0 |
|---|---|---|
| Golden fails (excluded) | 4: `Prob082_lfsr32`, `Prob141_count_clock`, `Prob156_review2015_fancytimer` (the testbench's own watchdog prints `TIMEOUT` even for the golden, which VerilogEval's classifier scores as a failure; the simulations report zero mismatches); `Prob099_m2014_q6c` (the testbench connects ports `Y2` and `Y4`, which the reference does not have) | 5: `asyn_fifo` and `ring_counter` (Icarus 12 does not support the testbench's `break` / whole-array assignment); `freq_divbyeven` (the testbench names an instance `checker`, a SystemVerilog keyword); `clkgenerator` and `radix2_div` (testbench races: the testbench samples on the same time step the design changes, which VCS and Icarus order differently) |
| Stub passes (flagged) | `Prob001_zero`, `Prob002_m2014_q4i` (the correct output *is* constant 0); `Prob053_m2014_q4d` | `edge_detect` (its checks combine with `&&` where `\|\|` was meant); `square_wave` (it checks only that the output is never high for more than 8 cycles) |
| Golden does not synthesize in Yosys | 6 (e.g. `Prob095`: a latch inferred in an `always_comb`) | 2 (`float_multi`, `synchronizer`) |

So the ladders cover **152 VerilogEval problems and 45 RTLLM designs**.

## Checked

- **Unit tests pass:** the estimator against the binomial definition, the
  ladder never decreasing with k, the split's identity, McNemar, and extraction
  on each benchmark's reply shapes.
- **Smoke runs** (5 problems, n = 4, and greedy) gave the expected outcomes,
  checked sample by sample. Mismatches, compile errors and passes were each
  confirmed from the logs.
- **Comparing a run with itself** gives exactly zero on every difference.
- **Resume** skips every finished problem.
- **The harness's greedy reply** to RTLLM's fsm is byte-identical to
  `executor_run`'s.
