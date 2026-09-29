// Smoke test for the local executor: libllama in process, under the frozen
// settings of executor_config.json, through the same code executor_run uses
// (executor.h).
//
//   executor_smoke [executor_config.json] [--verify-hash] [--verbose]
//
// --verbose passes llama.cpp's own log through, which is where to look when a
// backend fails to load or a model will not fit.
//
// Checks, in order, and exits non-zero at the first that fails:
//   1. libllama is the build the config pins (compiled in by CMake, which also
//      refuses to configure against any other build);
//   2. a GPU backend is available -- the Metal backend is a loadable module,
//      and without it llama.cpp quietly runs a 32B model on the CPU;
//   3. with --verify-hash, the model file's SHA-256 matches (about a minute);
//   4. greedy decoding of a small Verilog request, twice, each from a fresh
//      context, gives byte-identical replies (a two-sample determinism audit);
//   5. the reply contains the requested module.
// The first decode after a load is slow because the memory-mapped weights are
// still being read from disk, so the speed reported is the second run's.

#include "executor.h"

#include <cstring>

namespace {

int fail(char const* what, std::string const& detail = "") {
  std::fflush(stdout);  //so the failure lands after the progress lines, not before
  std::fprintf(stderr, "FAIL: %s%s%s\n", what, detail.empty() ? "" : ": ", detail.c_str());
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::string cfg_path = EXECUTOR_DEFAULT_CONFIG;
  bool verify_hash = false;
  bool verbose = false;
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "--verify-hash") == 0) {
      verify_hash = true;
    }
    else if (std::strcmp(argv[i], "--verbose") == 0) {
      verbose = true;
    }
    else {
      cfg_path = argv[i];
    }
  }

  executor::json cfg;
  try {
    cfg = executor::load_config(cfg_path);
  }
  catch (std::exception const& e) {
    return fail("bad config", e.what());
  }
  std::printf("config: %s (version %d)\n", cfg_path.c_str(), cfg["config_version"].get<int>());

  //3 first, when asked: no point loading a model whose file is wrong.
  if (verify_hash) {
    std::string const path = executor::expand_home(cfg["model"]["path"]);
    std::printf("verifying sha256 of %s ...\n", path.c_str());
    std::string got = executor::sha256(path, true);
    if (got != cfg["model"]["sha256"].get<std::string>()) {
      return fail("model sha256 mismatch", got.empty() ? "cannot read file" : got);
    }
    std::printf("sha256: ok\n");
  }

  //1 and 2 are the constructor's own checks.
  try {
    executor::LibLlamaExecutor ex(cfg, verbose);
    std::printf("libllama: build %d, commit %.8s (pinned)\n",
                cfg["runtime"]["llama_cpp_build_number"].get<int>(), EXECUTOR_LLAMA_COMMIT);
    std::printf("gpu: %s\n", ex.gpu().c_str());
    std::printf("model: %s %s, %.1f GiB, loaded in %.1f s\n",
                cfg["model"]["name"].get<std::string>().c_str(),
                cfg["model"]["quantization"].get<std::string>().c_str(),
                ex.model_bytes() / 1073741824.0, ex.load_seconds());

    //4. Two greedy replies from fresh contexts.
    std::string const user =
        "Write a Verilog module named mux2 with inputs a, b, sel and output y that selects b "
        "when sel is 1 and a otherwise. Reply with only the code.";
    executor::Reply a = ex.generate(user);
    executor::Reply b = ex.generate(user);
    std::printf("decode: %.1f tok/s (%d prompt tokens, %d generated)\n", b.decode_tok_s,
                b.prompt_tokens, b.generated_tokens);
    std::printf("----- reply -----\n%s\n-----------------\n", a.text.c_str());
    if (a.text != b.text) {
      return fail("greedy replies differ between fresh contexts");
    }
    std::printf("determinism: byte-identical across fresh contexts\n");

    //5. The reply holds the module that was asked for.
    std::string code = executor::last_verilog_block(a.text);
    if (code.find("module mux2") == std::string::npos || code.find("endmodule") == std::string::npos) {
      return fail("reply has no fenced verilog block containing module mux2");
    }
    std::printf("extraction: found module mux2\n");
  }
  catch (std::exception const& e) {
    return fail(e.what());
  }
  std::printf("PASS\n");
  return 0;
}
