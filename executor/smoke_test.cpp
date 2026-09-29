// Smoke test for the local executor: libllama in process, under the frozen
// settings of executor_config.json.
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
//   3. with --verify-hash, the model file's SHA-256 matches (about 2 minutes);
//   4. greedy decoding of a small Verilog request, twice, each from a fresh
//      context, gives byte-identical replies (a two-sample version of the
//      determinism audit, M2 in LLM_RTL_code_generation.md);
//   5. the reply contains the requested module.
// It also reports load time and decode speed. The first decode after a load is
// slow because the memory-mapped weights are still being read from disk, so
// the speed reported is the second run's.

#include "llama.h"
#include "ggml-backend.h"
#include <nlohmann/json.hpp>
#include <CommonCrypto/CommonDigest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using json = nlohmann::json;

#ifndef EXECUTOR_LLAMA_COMMIT
#define EXECUTOR_LLAMA_COMMIT "unknown"
#endif
#ifndef EXECUTOR_DEFAULT_CONFIG
#define EXECUTOR_DEFAULT_CONFIG "executor_config.json"
#endif

namespace {

int fail(char const* what, std::string const& detail = "") {
  std::fflush(stdout);  //so the failure lands after the progress lines, not before
  std::fprintf(stderr, "FAIL: %s%s%s\n", what, detail.empty() ? "" : ": ", detail.c_str());
  return 1;
}

std::string expand_home(std::string p) {
  if (!p.empty() && p[0] == '~') {
    char const* home = std::getenv("HOME");
    p = std::string(home ? home : "") + p.substr(1);
  }
  return p;
}

std::string sha256_of(std::string const& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return "";
  }
  CC_SHA256_CTX c;
  CC_SHA256_Init(&c);
  std::vector<char> buf(1 << 24);
  while (in) {
    in.read(buf.data(), buf.size());
    CC_SHA256_Update(&c, buf.data(), (CC_LONG)in.gcount());
  }
  unsigned char d[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256_Final(d, &c);
  char hex[2 * CC_SHA256_DIGEST_LENGTH + 1];
  for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) {
    std::snprintf(hex + 2 * i, 3, "%02x", d[i]);
  }
  return hex;
}

ggml_type kv_type(std::string const& s) {
  if (s == "f16") return GGML_TYPE_F16;
  if (s == "f32") return GGML_TYPE_F32;
  if (s == "q8_0") return GGML_TYPE_Q8_0;
  throw std::runtime_error("unsupported KV cache type " + s);
}

llama_load_mode load_mode(std::string const& s) {
  if (s == "mmap") return LLAMA_LOAD_MODE_MMAP;
  if (s == "none") return LLAMA_LOAD_MODE_NONE;
  if (s == "mmap_mlock") return LLAMA_LOAD_MODE_MMAP_MLOCK;
  throw std::runtime_error("load_mode must be pinned to mmap, none or mmap_mlock, not " + s);
}

llama_lazy_mode lazy_mode(std::string const& s) {
  if (s == "off") return LLAMA_LAZY_MODE_OFF;
  if (s == "auto") return LLAMA_LAZY_MODE_AUTO;
  if (s == "on") return LLAMA_LAZY_MODE_ON;
  throw std::runtime_error("unknown lazy_mode " + s);
}

llama_flash_attn_type flash_attn(std::string const& s) {
  if (s == "enabled") return LLAMA_FLASH_ATTN_TYPE_ENABLED;
  if (s == "disabled") return LLAMA_FLASH_ATTN_TYPE_DISABLED;
  throw std::runtime_error("flash_attn must be pinned to enabled or disabled, not " + s);
}

struct Reply {
  std::string text;
  int prompt_tokens = 0;
  int generated = 0;
  double decode_tok_s = 0.0;
};

//One request from a fresh context, exactly as the frozen config says.
Reply generate(llama_model* model, json const& cfg, std::string const& prompt) {
  json const& cx = cfg["context"];
  const llama_vocab* vocab = llama_model_get_vocab(model);

  int n = -llama_tokenize(vocab, prompt.c_str(), prompt.size(), nullptr, 0, true, true);
  std::vector<llama_token> toks(n);
  llama_tokenize(vocab, prompt.c_str(), prompt.size(), toks.data(), n, true, true);

  auto cp = llama_context_default_params();
  cp.n_ctx = cx["n_ctx"];
  cp.n_batch = cx["n_batch"];
  cp.n_ubatch = cx["n_ubatch"];
  cp.n_seq_max = cx["n_seq_max"];
  cp.n_threads = cx["n_threads"];
  cp.n_threads_batch = cx["n_threads_batch"];
  cp.flash_attn_type = flash_attn(cx["flash_attn"]);
  cp.type_k = kv_type(cx["type_k"]);
  cp.type_v = kv_type(cx["type_v"]);
  cp.offload_kqv = cx["offload_kqv"];
  llama_context* ctx = llama_init_from_model(model, cp);
  if (!ctx) {
    throw std::runtime_error("could not create a context");
  }

  //Greedy only; see the config's sampling note.
  llama_sampler* smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
  llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

  Reply r;
  r.prompt_tokens = n;
  int const max_new = cfg["generation"]["max_new_tokens"];
  llama_batch batch = llama_batch_get_one(toks.data(), toks.size());
  std::chrono::steady_clock::time_point first;
  llama_token t;
  for (; r.generated < max_new; r.generated++) {
    if (llama_decode(ctx, batch)) {
      throw std::runtime_error("llama_decode failed");
    }
    if (r.generated == 0) {
      first = std::chrono::steady_clock::now();
    }
    t = llama_sampler_sample(smpl, ctx, -1);
    if (llama_vocab_is_eog(vocab, t)) {
      break;
    }
    char piece[256];
    int k = llama_token_to_piece(vocab, t, piece, sizeof piece, 0, true);
    r.text.append(piece, k);
    batch = llama_batch_get_one(&t, 1);
  }
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - first).count();
  //Tokens after the first: the first one's time is the prompt's.
  r.decode_tok_s = r.generated > 1 ? (r.generated - 1) / secs : 0.0;
  llama_sampler_free(smpl);
  llama_free(ctx);
  return r;
}

//The config's extraction rule: the last fenced ```verilog block, or "" if
//there is none (an abstention).
std::string last_verilog_block(std::string const& s) {
  std::string const open = "```verilog";
  size_t at = s.rfind(open);
  if (at == std::string::npos) {
    return "";
  }
  size_t start = s.find('\n', at);
  size_t end = s.find("```", start == std::string::npos ? at + open.size() : start);
  if (start == std::string::npos || end == std::string::npos) {
    return "";
  }
  return s.substr(start + 1, end - start - 1);
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

  json cfg;
  try {
    std::ifstream in(cfg_path);
    if (!in) {
      return fail("cannot read config", cfg_path);
    }
    cfg = json::parse(in);
  }
  catch (std::exception const& e) {
    return fail("config is not valid JSON", e.what());
  }
  std::printf("config: %s (version %d)\n", cfg_path.c_str(), cfg["config_version"].get<int>());

  //1. The library is the pinned build.
  std::string const pinned = cfg["runtime"]["llama_cpp_commit"];
  if (pinned != EXECUTOR_LLAMA_COMMIT) {
    return fail("libllama is not the pinned build",
                std::string("built against ") + EXECUTOR_LLAMA_COMMIT + ", config pins " + pinned);
  }
  std::printf("libllama: build %d, commit %.8s (pinned)\n",
              cfg["runtime"]["llama_cpp_build_number"].get<int>(), pinned.c_str());

  //Before anything reaches ggml: loading the backends logs too. Failures are
  //still reported, by the checks below.
  if (!verbose) {
    llama_log_set([](ggml_log_level, char const*, void*) {}, nullptr);
  }

  //2. A GPU backend is present.
  std::string const backend_dir = cfg["runtime"]["backend_dir"];
  ggml_backend_load_all_from_path(backend_dir.c_str());
  std::string gpu;
  for (size_t i = 0; i < ggml_backend_dev_count(); i++) {
    ggml_backend_dev_t d = ggml_backend_dev_get(i);
    if (ggml_backend_dev_type(d) == GGML_BACKEND_DEVICE_TYPE_GPU) {
      size_t free = 0, total = 0;
      ggml_backend_dev_memory(d, &free, &total);
      gpu = std::string(ggml_backend_dev_name(d)) + " (" + ggml_backend_dev_description(d) + ", " +
            std::to_string(total >> 20) + " MiB)";
      break;
    }
  }
  if (gpu.empty()) {
    return fail("no GPU backend found", "looked in " + backend_dir + "; the model would run on the CPU");
  }
  std::printf("gpu: %s\n", gpu.c_str());

  //3. The model file, optionally by hash.
  json const& m = cfg["model"];
  std::string const model_path = expand_home(m["path"]);
  if (verify_hash) {
    std::printf("verifying sha256 of %s ...\n", model_path.c_str());
    std::string got = sha256_of(model_path);
    if (got != m["sha256"].get<std::string>()) {
      return fail("model sha256 mismatch", got.empty() ? "cannot read file" : got);
    }
    std::printf("sha256: ok\n");
  }

  llama_backend_init();
  auto mp = llama_model_default_params();
  mp.n_gpu_layers = cfg["load"]["n_gpu_layers"];
  try {
    mp.load_mode = load_mode(cfg["load"]["load_mode"]);
    mp.lazy_mode = lazy_mode(cfg["load"]["lazy_mode"]);
  }
  catch (std::exception const& e) {
    return fail("bad load settings in config", e.what());
  }
  auto t0 = std::chrono::steady_clock::now();
  llama_model* model = llama_model_load_from_file(model_path.c_str(), mp);
  if (!model) {
    return fail("could not load model", model_path);
  }
  double load_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("model: %s %s, %.1f GiB, loaded in %.1f s\n", m["name"].get<std::string>().c_str(),
              m["quantization"].get<std::string>().c_str(), llama_model_size(model) / 1073741824.0, load_s);

  //4. Two greedy replies from fresh contexts.
  std::string const sys = cfg["generation"]["system_prompt"];
  std::string const user =
      "Write a Verilog module named mux2 with inputs a, b, sel and output y that selects b "
      "when sel is 1 and a otherwise. Reply with only the code.";
  llama_chat_message msgs[] = {{"system", sys.c_str()}, {"user", user.c_str()}};
  std::vector<char> buf(16384);
  int len = llama_chat_apply_template(llama_model_chat_template(model, nullptr), msgs, 2, true,
                                      buf.data(), buf.size());
  if (len < 0 || len > (int)buf.size()) {
    return fail("could not apply the model's chat template");
  }
  std::string const prompt(buf.data(), len);

  Reply a, b;
  try {
    a = generate(model, cfg, prompt);
    b = generate(model, cfg, prompt);
  }
  catch (std::exception const& e) {
    return fail("generation failed", e.what());
  }
  std::printf("decode: %.1f tok/s (%d prompt tokens, %d generated)\n", b.decode_tok_s,
              b.prompt_tokens, b.generated);
  std::printf("----- reply -----\n%s\n-----------------\n", a.text.c_str());
  llama_model_free(model);

  if (a.text != b.text) {
    return fail("greedy replies differ between fresh contexts");
  }
  std::printf("determinism: byte-identical across fresh contexts\n");

  //5. The reply holds the module that was asked for.
  std::string code = last_verilog_block(a.text);
  if (code.find("module mux2") == std::string::npos || code.find("endmodule") == std::string::npos) {
    return fail("reply has no fenced verilog block containing module mux2");
  }
  std::printf("extraction: found module mux2\n");
  std::printf("PASS\n");
  return 0;
}
