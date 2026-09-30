# Local LLM executor

The executor is the LLM that writes Verilog from the planner's step-by-step
instructions. This directory holds its frozen configuration and a smoke test
that exercises it through libllama (llama.cpp's C API) in process, the way the
planner will.

| File | What it is |
|---|---|
| `executor_config.json` | Everything that can change the executor's output, pinned: model file and SHA-256, llama.cpp build, context and batch shapes, flash attention, KV cache type, sampler |
| `executor.h` | The executor itself, header-only: loads the config and model, checks the pins and the GPU, generates greedily, extracts the Verilog |
| `run_prompt.cpp` | `executor_run`: a prompt file in, the model's Verilog out |
| `sample_request.cpp` | `executor_sample`: n samples of one request, for the evaluation harness (`eval/`) |
| `smoke_test.cpp` | `executor_smoke`: checks the setup end to end |
| `CMakeLists.txt` | Builds both, from the main build or on its own |

## Running a prompt

    executor_run PROMPT.txt [-o OUT.v] [--out-dir DIR] [--config CONFIG.json] [--quiet]

The whole prompt file is the user message. The system prompt and every
generation setting come from `executor_config.json`, and there are no flags to
override them, so every run is comparable with every other. The reply streams to
stdout as it is generated; progress goes to stderr. Three files are written,
named after the prompt file (or after `-o`), in the current directory unless
`--out-dir` says otherwise:

| File | Contents |
|---|---|
| `NAME.v` | The Verilog: the last fenced ` ```verilog ` block of the reply |
| `NAME.response.txt` | The whole reply |
| `NAME.run.json` | A record of the run: config version and hash, the model's pinned hash, the llama.cpp commit, the prompt and its hash, the reply, token counts and timings |

The exit status is 0 when Verilog was extracted, and 2 when the reply had no
` ```verilog ` block. That counts as an abstention: `NAME.v` is not written, and
one left over from an earlier run is deleted so it cannot pass for this one. The
status is 1 on any error.

For example, the fsm design description, then the gold testbench (the model
used SystemVerilog's `typedef enum`, hence `-g2012`):

    ./build/executor/executor_run rtl_designs/fsm/design_description.txt --out-dir /tmp/fsm
    iverilog -g2012 -o /tmp/fsm/sim /tmp/fsm/design_description.v rtl_designs/fsm/testbench.v
    vvp /tmp/fsm/sim

That run takes about 80 s: about 4 s for the 284-token prompt, then 721 tokens
at 9.7 tokens/s. Loading the model takes about 2 s once it is in the page cache.
The Verilog it produced **fails** the testbench: its transitions return to the
initial state where the overlapping pattern should fall back to a partial
match, and it registers `MATCH` rather than driving it combinationally, as the
Mealy output the spec asks for requires. Running it again, in a new process,
gives the same bytes.

## Setup

libllama comes from conda. Use the Metal build from Anaconda's `pkgs/main`
channel; conda-forge ships only a CPU build for macOS, far too slow for a 32B
model:

    conda create -n llama -c defaults --override-channels \
        'llama.cpp=0.4.1=mps_h404e123_100' 'libllama=0.4.1=mps_h404e123_100' \
        nlohmann_json huggingface_hub

The model is the file named in `executor_config.json`, downloaded to the path
given there. Plain HTTPS is more reliable than Hugging Face's Xet transfer,
which stalled at about 50 KB/s here:

    HF_HUB_DISABLE_XET=1 hf download Qwen/Qwen2.5-Coder-32B-Instruct-GGUF \
        qwen2.5-coder-32b-instruct-q8_0.gguf \
        --revision 9d3053fce650fe1cdbdb75998c2a87add9d178ef \
        --local-dir ~/models/Qwen2.5-Coder-32B-Instruct-GGUF

## Build and run

As part of the main build, by adding the `llama` env to its prefix path (see the
top-level README). The smoke test is then `build/executor/executor_smoke`:

    cmake .. -DCMAKE_PREFIX_PATH="/opt/anaconda3/envs/htnplan;/opt/anaconda3/envs/llama"

Or on its own, from the repository root (any CMake of 3.19 or later; the
`htnplan` env has one):

    E=/opt/anaconda3/envs/llama
    cmake -S executor -B executor/build -DCMAKE_PREFIX_PATH=$E -DCMAKE_BUILD_TYPE=Release
    cmake --build executor/build
    ./executor/build/executor_smoke                 # about 15 s
    ./executor/build/executor_smoke --verify-hash   # also checks the model's SHA-256, about 1 min

Code of ours that runs the executor should link the `htn::llama` target rather
than `llama` directly. It brings libllama and the JSON reader, and defines
`EXECUTOR_LLAMA_COMMIT`, `EXECUTOR_DEFAULT_CONFIG` and `EXECUTOR_BACKEND_DIR`.
Linking it also means the pin checks below run before that code is built.

Configuring fails unless the libllama found is the build the config pins, and
unless the config's `backend_dir` is that build's `bin/`. The smoke test
checks, and exits non-zero at the first failure:

1. libllama is the pinned build;
2. a GPU backend is available;
3. with `--verify-hash`, the model file's SHA-256;
4. two greedy replies to a small Verilog request, each from a fresh context,
   are byte-identical;
5. the reply's last fenced `verilog` block contains the module asked for.

`--verbose` shows llama.cpp's own log, which is where to look when a backend
will not load or a model will not fit.

## Sampling for the pass@k ladders

`executor_sample --request REQ.json --out-dir DIR --n 20` draws 20 samples of a
request. `REQ.json` holds the benchmark adapter's messages, `{"system": ...,
"user": ...}`, because the prompt template belongs to the benchmark, not to
the executor. It writes one `sample_XX.response.txt` per sample, and `gen.json`
(settings, seeds, token counts, timings) last, so a directory without
`gen.json` is unfinished. `--greedy` draws the single greedy reply instead,
under the same frozen settings as `executor_run`. The harness in `eval/` drives
it; see `eval/README.md`.

The config's `ladder_sampling` block sets how:

- **Sampler:** temperature 0.85, then top-p 0.95, then a draw seeded
  `base_seed + i` for sample i. That is VerilogEval v2's setting for 20 samples,
  applied in the order the OpenAI and Hugging Face samplers use.
- **Batching:** the 20 samples are decoded as one batch of 20 sequences. The
  prompt is decoded once, its memory copied to every sequence, and each step
  then decodes one token of every unfinished sequence. Decoding on Apple Silicon
  is bound by reading the weights, which a batch reads once for all its
  sequences.

| Decoding (32B, M1 Max) | Tokens/s in aggregate |
|---|---|
| 1 sequence | 9.7 |
| 10 sequences, separate caches | 18.2 |
| 10 sequences, one shared cache | 24.8 |
| **20 sequences, one shared cache (the setting)** | **36.6** |

These were measured on a VerilogEval FSM problem.

A copy test confirms that the prompt reaches every sequence. At a temperature
near zero, all sequences reproduce the greedy reply. If the copy failed, the
sequences after the first would have generated without the prompt.

Sampled output is reproducible **per seed, under a fixed configuration**. It is
not bit-identical across batch sizes, because GPU kernels are not
batch-invariant. The greedy line is the bit-exact one.

## Things that are easy to get wrong

- **The Metal backend is a loadable module in the env's `bin/`, not in `lib/`.**
  A program that links libllama has to call
  `ggml_backend_load_all_from_path(<env>/bin)` before loading a model. Without
  it, llama.cpp runs the model on the CPU without saying so. The smoke test
  fails instead.
- **The first run after installing takes about 20 s longer**, while Metal
  compiles its shaders. Later runs load the model in about 2 s.
- **Settings left on `auto` are resolved by the library at load time.** The
  config pins flash attention (off) and the load mode (mmap) to what `auto`
  chose on this machine, so a library update cannot change them silently.
- **Greedy output is reproducible only on the same machine, OS and build.** GPU
  kernels are not batch-invariant, and another chip or a macOS update can change
  the output. The config records the machine it was measured on.

Measured on an M1 Max (32-core GPU, 64 GB, 400 GB/s): all 65 layers on the GPU,
prompt processing 103 tokens/s, generation 9.7 tokens/s (`llama-bench`), so a
typical 300–800-token Verilog answer takes 30–80 s.
