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
| `rtl_eval.py` | The command-line driver: `selftest`, `generate`, `evaluate`, `report`, `compare` |
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
