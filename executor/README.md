# Local LLM executor

The executor is the LLM that writes Verilog from the planner's step-by-step
instructions. This directory holds its frozen configuration and a smoke test
that exercises it through libllama (llama.cpp's C API) in process, the way the
planner will.

| File | What it is |
|---|---|
| `executor_config.json` | Everything that can change the executor's output, pinned: model file and SHA-256, llama.cpp build, context and batch shapes, flash attention, KV cache type, sampler |
| `smoke_test.cpp` | Loads the model under that config and checks the setup end to end |
| `CMakeLists.txt` | Builds the smoke test on its own, apart from the planner |

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
