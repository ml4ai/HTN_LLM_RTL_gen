#pragma once
// The local LLM executor: libllama in process, under the frozen settings of
// executor_config.json. Header-only; link htn::llama (executor/CMakeLists.txt),
// which supplies libllama, nlohmann_json and the EXECUTOR_* definitions.
//
//   auto cfg = executor::load_config(EXECUTOR_DEFAULT_CONFIG);
//   executor::LibLlamaExecutor ex(cfg);
//   executor::Reply r = ex.generate("Write a Verilog module ...");
//   std::string code = executor::last_verilog_block(r.text);  // "" = abstention
//
// Everything that can change the output comes from the config, and nothing
// here overrides it: a run under other settings is not comparable with runs
// under the frozen ones.

#include "llama.h"
#include "ggml-backend.h"
#include <nlohmann/json.hpp>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef EXECUTOR_LLAMA_COMMIT
#define EXECUTOR_LLAMA_COMMIT "unknown"
#endif

namespace executor {

using json = nlohmann::json;

inline std::string expand_home(std::string p) {
  if (!p.empty() && p[0] == '~') {
    char const* home = std::getenv("HOME");
    p = std::string(home ? home : "") + p.substr(1);
  }
  return p;
}

inline json load_config(std::string const& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot read config " + path);
  }
  try {
    return json::parse(in);
  }
  catch (json::exception const& e) {
    throw std::runtime_error("config " + path + " is not valid JSON: " + e.what());
  }
}

#ifdef __APPLE__
//SHA-256 of a byte string, or of a file when `is_file` (streamed: the model is 35 GB).
inline std::string sha256(std::string const& data_or_path, bool is_file = false) {
  CC_SHA256_CTX c;
  CC_SHA256_Init(&c);
  if (is_file) {
    std::ifstream in(data_or_path, std::ios::binary);
    if (!in) {
      return "";
    }
    std::vector<char> buf(1 << 24);
    while (in) {
      in.read(buf.data(), buf.size());
      CC_SHA256_Update(&c, buf.data(), (CC_LONG)in.gcount());
    }
  }
  else {
    CC_SHA256_Update(&c, data_or_path.data(), (CC_LONG)data_or_path.size());
  }
  unsigned char d[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256_Final(d, &c);
  char hex[2 * CC_SHA256_DIGEST_LENGTH + 1];
  for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) {
    std::snprintf(hex + 2 * i, 3, "%02x", d[i]);
  }
  return hex;
}
#endif

//The config's extraction rule: the last fenced ```verilog block of the reply.
//"" means there is none, which the evaluation policy scores as an abstention.
inline std::string last_verilog_block(std::string const& s) {
  std::string const open = "```verilog";
  size_t at = s.rfind(open);
  if (at == std::string::npos) {
    return "";
  }
  size_t start = s.find('\n', at);
  if (start == std::string::npos) {
    return "";
  }
  size_t end = s.find("```", start);
  if (end == std::string::npos) {
    return "";  //unterminated: the reply was cut off mid-block
  }
  return s.substr(start + 1, end - start - 1);
}

struct Reply {
  std::string text;
  std::string finish_reason;  //"stop" at an end-of-generation token, "length" at max_new_tokens
  int prompt_tokens = 0;
  int generated_tokens = 0;
  double prompt_seconds = 0.0;
  double decode_seconds = 0.0;
  double decode_tok_s = 0.0;
};

class LibLlamaExecutor {
public:
  //Checks the pinned build, loads the backends from the config's backend_dir,
  //refuses to go on without a GPU, and loads the model. Throws on any failure.
  explicit LibLlamaExecutor(json cfg, bool verbose = false) : cfg_(std::move(cfg)) {
    std::string const pinned = cfg_["runtime"]["llama_cpp_commit"];
    if (pinned != EXECUTOR_LLAMA_COMMIT) {
      throw std::runtime_error(std::string("libllama is not the pinned build: built against ") +
                               EXECUTOR_LLAMA_COMMIT + ", config pins " + pinned);
    }
    //Before anything reaches ggml, which logs as it loads backends. Failures
    //are still reported, by the checks here.
    if (!verbose) {
      llama_log_set([](ggml_log_level, char const*, void*) {}, nullptr);
    }
    //The Metal backend is a module in backend_dir; without it llama.cpp runs
    //the model on the CPU and says nothing.
    std::string const backend_dir = cfg_["runtime"]["backend_dir"];
    ggml_backend_load_all_from_path(backend_dir.c_str());
    for (size_t i = 0; i < ggml_backend_dev_count(); i++) {
      ggml_backend_dev_t d = ggml_backend_dev_get(i);
      if (ggml_backend_dev_type(d) == GGML_BACKEND_DEVICE_TYPE_GPU) {
        size_t free_mem = 0, total = 0;
        ggml_backend_dev_memory(d, &free_mem, &total);
        gpu_ = std::string(ggml_backend_dev_name(d)) + " (" + ggml_backend_dev_description(d) +
               ", " + std::to_string(total >> 20) + " MiB)";
        break;
      }
    }
    if (gpu_.empty()) {
      throw std::runtime_error("no GPU backend found in " + backend_dir +
                               "; the model would run on the CPU");
    }

    llama_backend_init();
    json const& ld = cfg_["load"];
    auto mp = llama_model_default_params();
    mp.n_gpu_layers = ld["n_gpu_layers"];
    mp.load_mode = load_mode(ld["load_mode"]);
    mp.lazy_mode = lazy_mode(ld["lazy_mode"]);
    std::string const path = model_path();
    auto t0 = std::chrono::steady_clock::now();
    model_ = llama_model_load_from_file(path.c_str(), mp);
    if (!model_) {
      throw std::runtime_error("could not load model " + path);
    }
    load_seconds_ = seconds_since(t0);
  }

  ~LibLlamaExecutor() {
    if (model_) {
      llama_model_free(model_);
    }
  }
  LibLlamaExecutor(LibLlamaExecutor const&) = delete;
  LibLlamaExecutor& operator=(LibLlamaExecutor const&) = delete;

  json const& config() const { return cfg_; }
  std::string const& gpu() const { return gpu_; }
  double load_seconds() const { return load_seconds_; }
  std::string model_path() const { return expand_home(cfg_["model"]["path"]); }
  uint64_t model_bytes() const { return llama_model_size(model_); }

  //The prompt exactly as the model sees it: the config's system prompt and
  //this user message, in the chat template embedded in the GGUF.
  std::string format(std::string const& user) const {
    std::string const sys = cfg_["generation"]["system_prompt"];
    llama_chat_message msgs[] = {{"system", sys.c_str()}, {"user", user.c_str()}};
    char const* tmpl = llama_model_chat_template(model_, nullptr);
    std::vector<char> buf(user.size() + sys.size() + 1024);
    int len = llama_chat_apply_template(tmpl, msgs, 2, true, buf.data(), buf.size());
    if (len > (int)buf.size()) {
      buf.resize(len);
      len = llama_chat_apply_template(tmpl, msgs, 2, true, buf.data(), buf.size());
    }
    if (len < 0) {
      throw std::runtime_error("could not apply the model's chat template");
    }
    return std::string(buf.data(), len);
  }

  //One request, from a fresh context, decoded greedily. `on_piece`, if given,
  //receives the reply as it is generated.
  Reply generate(std::string const& user,
                 std::function<void(std::string const&)> const& on_piece = {}) {
    json const& cx = cfg_["context"];
    int const max_new = cfg_["generation"]["max_new_tokens"];
    const llama_vocab* vocab = llama_model_get_vocab(model_);
    std::string const prompt = format(user);

    int n = -llama_tokenize(vocab, prompt.c_str(), prompt.size(), nullptr, 0, true, true);
    std::vector<llama_token> toks(n);
    llama_tokenize(vocab, prompt.c_str(), prompt.size(), toks.data(), n, true, true);
    int const n_ctx = cx["n_ctx"];
    if (n + max_new > n_ctx) {
      throw std::runtime_error("prompt is " + std::to_string(n) + " tokens; with max_new_tokens " +
                               std::to_string(max_new) + " that exceeds n_ctx " +
                               std::to_string(n_ctx));
    }

    auto cp = llama_context_default_params();
    cp.n_ctx = n_ctx;
    cp.n_batch = cx["n_batch"];
    cp.n_ubatch = cx["n_ubatch"];
    cp.n_seq_max = cx["n_seq_max"];
    cp.n_threads = cx["n_threads"];
    cp.n_threads_batch = cx["n_threads_batch"];
    cp.flash_attn_type = flash_attn(cx["flash_attn"]);
    cp.type_k = kv_type(cx["type_k"]);
    cp.type_v = kv_type(cx["type_v"]);
    cp.offload_kqv = cx["offload_kqv"];
    llama_context* ctx = llama_init_from_model(model_, cp);
    if (!ctx) {
      throw std::runtime_error("could not create a context");
    }
    //Greedy only: the config's sampling note says why.
    llama_sampler* smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

    Reply r;
    r.prompt_tokens = n;
    r.finish_reason = "length";
    llama_batch batch = llama_batch_get_one(toks.data(), toks.size());
    auto t0 = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point first;
    std::vector<char> piece(256);
    llama_token t;
    try {
      for (; r.generated_tokens < max_new; r.generated_tokens++) {
        if (llama_decode(ctx, batch)) {
          throw std::runtime_error("llama_decode failed");
        }
        t = llama_sampler_sample(smpl, ctx, -1);
        //After sampling, not after llama_decode: on Metal the decode returns
        //before the GPU is done, and sampling is where the wait happens. Timed
        //after the decode, the prompt came out at 17 ms for 284 tokens.
        if (r.generated_tokens == 0) {
          first = std::chrono::steady_clock::now();
        }
        if (llama_vocab_is_eog(vocab, t)) {
          r.finish_reason = "stop";
          break;
        }
        int k = llama_token_to_piece(vocab, t, piece.data(), piece.size(), 0, true);
        if (k < 0) {
          piece.resize(-k);
          k = llama_token_to_piece(vocab, t, piece.data(), piece.size(), 0, true);
        }
        std::string p(piece.data(), k);
        r.text += p;
        if (on_piece) {
          on_piece(p);
        }
        batch = llama_batch_get_one(&t, 1);
      }
    }
    catch (...) {
      llama_sampler_free(smpl);
      llama_free(ctx);
      throw;
    }
    //The first token's time is the prompt's; decoding speed is over the rest.
    r.prompt_seconds = std::chrono::duration<double>(first - t0).count();
    r.decode_seconds = seconds_since(first);
    r.decode_tok_s = r.generated_tokens > 1 ? (r.generated_tokens - 1) / r.decode_seconds : 0.0;
    llama_sampler_free(smpl);
    llama_free(ctx);
    return r;
  }

private:
  static double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  }

  static ggml_type kv_type(std::string const& s) {
    if (s == "f16") return GGML_TYPE_F16;
    if (s == "f32") return GGML_TYPE_F32;
    if (s == "q8_0") return GGML_TYPE_Q8_0;
    throw std::runtime_error("unsupported KV cache type " + s);
  }

  static llama_flash_attn_type flash_attn(std::string const& s) {
    if (s == "enabled") return LLAMA_FLASH_ATTN_TYPE_ENABLED;
    if (s == "disabled") return LLAMA_FLASH_ATTN_TYPE_DISABLED;
    throw std::runtime_error("flash_attn must be pinned to enabled or disabled, not " + s);
  }

  static llama_load_mode load_mode(std::string const& s) {
    if (s == "mmap") return LLAMA_LOAD_MODE_MMAP;
    if (s == "none") return LLAMA_LOAD_MODE_NONE;
    if (s == "mmap_mlock") return LLAMA_LOAD_MODE_MMAP_MLOCK;
    throw std::runtime_error("load_mode must be pinned to mmap, none or mmap_mlock, not " + s);
  }

  static llama_lazy_mode lazy_mode(std::string const& s) {
    if (s == "off") return LLAMA_LAZY_MODE_OFF;
    if (s == "auto") return LLAMA_LAZY_MODE_AUTO;
    if (s == "on") return LLAMA_LAZY_MODE_ON;
    throw std::runtime_error("unknown lazy_mode " + s);
  }

  json cfg_;
  std::string gpu_;
  llama_model* model_ = nullptr;
  double load_seconds_ = 0.0;
};

}  // namespace executor
