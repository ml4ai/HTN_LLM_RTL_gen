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

#include <algorithm>
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
  uint32_t seed = 0;          //sampled replies only (generate_samples)
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
    return format(cfg_["generation"]["system_prompt"].get<std::string>(), user);
  }

  //The same with an explicit system prompt. Benchmarks bring their own prompt
  //templates, system message included; the template belongs to the benchmark
  //adapter, not to the executor, whose frozen settings are how it decodes.
  std::string format(std::string const& sys, std::string const& user) const {
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
    return generate(cfg_["generation"]["system_prompt"].get<std::string>(), user, on_piece);
  }

  Reply generate(std::string const& sys, std::string const& user,
                 std::function<void(std::string const&)> const& on_piece = {}) {
    json const& cx = cfg_["context"];
    int const max_new = cfg_["generation"]["max_new_tokens"];
    const llama_vocab* vocab = llama_model_get_vocab(model_);
    std::vector<llama_token> toks = tokenize(format(sys, user));
    int const n = (int)toks.size();
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
    auto t0 = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point first;
    std::vector<char> piece(256);
    llama_token t;
    llama_batch batch;
    try {
      //A prompt longer than n_batch goes in n_batch at a time; the last piece
      //is the batch the first token is sampled from. A plan with its task
      //tree can run past n_batch, which used to be refused.
      size_t const n_batch = cx["n_batch"].get<size_t>();
      size_t off = 0;
      while (toks.size() - off > n_batch) {
        if (llama_decode(ctx, llama_batch_get_one(toks.data() + off, n_batch))) {
          throw std::runtime_error("llama_decode failed on the prompt");
        }
        off += n_batch;
      }
      batch = llama_batch_get_one(toks.data() + off, toks.size() - off);
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

  //`n` samples of one request, for the pass@k ladders (the config's
  //ladder_sampling block): seeded temperature and top-p sampling, sample i
  //seeded base_seed + i. They are decoded parallel_sequences at a time as
  //separate sequences of one context: the prompt is decoded once, its memory
  //copied to every sequence, and each step then decodes one token of every
  //unfinished sequence in a single batch. Decoding on Apple Silicon is bound
  //by reading the weights, which a batch reads once for all its sequences.
  //
  //Not bit-reproducible across batch compositions: GPU kernels are not
  //batch-invariant, so the same seed in a different batch can decode
  //differently. Sampled runs promise per-seed reproducibility under a fixed
  //configuration, not more; the greedy line (generate) is the bit-exact one.
  std::vector<Reply> generate_samples(std::string const& sys, std::string const& user, int n,
                                      std::function<void(int, int)> const& on_done = {}) {
    json const& ls = cfg_.at("ladder_sampling");
    float const temperature = ls["temperature"];
    float const top_p = ls["top_p"];
    uint32_t const base_seed = ls["base_seed"];
    int const parallel = ls["parallel_sequences"];
    bool const kv_unified = ls.value("kv_unified", false);
    json const& cx = cfg_["context"];
    int const max_new = cfg_["generation"]["max_new_tokens"];
    int const n_batch = cx["n_batch"];
    const llama_vocab* vocab = llama_model_get_vocab(model_);
    std::vector<llama_token> const toks = tokenize(format(sys, user));
    int const n_prompt = (int)toks.size();

    //How many sequences fit. In a unified cache the prompt is stored once and
    //shared by every sequence (llama_memory_seq_cp shares its cells), so a
    //batch of P needs n_prompt + P * max_new cells, not P * (n_prompt +
    //max_new). Sizing it the second way wasted (P - 1) * n_prompt cells, and
    //the compute buffer grows with the cache too: on a 1,000-token VerilogEval
    //prompt (Prob147) the 32B model then needed 52.9 GB of the 53.1 GB Metal
    //allows, and ran out of GPU memory. max_context_tokens caps the cache;
    //P shrinks rather than exceed it.
    int const max_cells = ls.value("max_context_tokens", 1 << 30);
    int cap = parallel;
    if (kv_unified) {
      cap = std::min(parallel, (max_cells - n_prompt) / max_new);
    }
    else {
      cap = std::min(parallel, max_cells / (n_prompt + max_new));
    }
    if (cap < 1) {
      throw std::runtime_error("a " + std::to_string(n_prompt) + "-token prompt with max_new_tokens " +
                               std::to_string(max_new) + " does not fit max_context_tokens " +
                               std::to_string(max_cells));
    }

    std::vector<Reply> out(n);
    int done = 0;
    for (int first = 0; first < n; first += cap) {
      int const P = std::min(cap, n - first);
      int const per_seq = n_prompt + max_new;

      auto cp = llama_context_default_params();
      //Each sequence its own memory stream: the samples share only the prompt,
      //and in one unified buffer every sequence would attend over all the
      //others' cells, which grow with every step (llama.h, kv_unified).
      cp.n_seq_max = P;
      cp.kv_unified = kv_unified;
      cp.n_ctx = kv_unified ? n_prompt + P * max_new : P * per_seq;
      cp.n_batch = std::max(n_batch, P);
      cp.n_ubatch = cx["n_ubatch"];
      cp.n_threads = cx["n_threads"];
      cp.n_threads_batch = cx["n_threads_batch"];
      cp.flash_attn_type = flash_attn(cx["flash_attn"]);
      cp.type_k = kv_type(cx["type_k"]);
      cp.type_v = kv_type(cx["type_v"]);
      cp.offload_kqv = cx["offload_kqv"];
      llama_context* ctx = llama_init_from_model(model_, cp);
      if (!ctx) {
        throw std::runtime_error("could not create a context for " + std::to_string(P) +
                                 " sequences of " + std::to_string(per_seq) + " tokens");
      }
      llama_batch batch = llama_batch_init(std::max(n_batch, P), 0, P);
      std::vector<llama_sampler*> smpl(P, nullptr);
      struct Cleanup {
        llama_context* c; llama_batch* b; std::vector<llama_sampler*>* s;
        ~Cleanup() {
          for (auto* x : *s) if (x) llama_sampler_free(x);
          llama_batch_free(*b);
          llama_free(c);
        }
      } cleanup{ctx, &batch, &smpl};

      if ((int)llama_n_ctx_seq(ctx) < per_seq) {
        throw std::runtime_error("each sequence got " + std::to_string(llama_n_ctx_seq(ctx)) +
                                 " tokens of context, fewer than the " + std::to_string(per_seq) + " needed");
      }
      for (int s = 0; s < P; s++) {
        //Temperature before top-p: the order the OpenAI and Hugging Face
        //samplers use, so top-p cuts the tempered distribution.
        smpl[s] = llama_sampler_chain_init(llama_sampler_chain_default_params());
        llama_sampler_chain_add(smpl[s], llama_sampler_init_temp(temperature));
        llama_sampler_chain_add(smpl[s], llama_sampler_init_top_p(top_p, 1));
        uint32_t const seed = base_seed + (uint32_t)(first + s);
        llama_sampler_chain_add(smpl[s], llama_sampler_init_dist(seed));
        out[first + s].seed = seed;
        out[first + s].prompt_tokens = n_prompt;
        out[first + s].finish_reason = "length";
      }

      //The prompt, once, into sequence 0, in chunks of n_batch.
      auto t0 = std::chrono::steady_clock::now();
      int last_idx = 0;
      for (int i = 0; i < n_prompt; i += n_batch) {
        batch.n_tokens = 0;
        int const end = std::min(n_prompt, i + n_batch);
        for (int j = i; j < end; j++) {
          batch_add(batch, toks[j], j, 0, j == n_prompt - 1);
        }
        if (llama_decode(ctx, batch)) {
          throw std::runtime_error("llama_decode failed on the prompt");
        }
        last_idx = batch.n_tokens - 1;
      }
      llama_memory_t mem = llama_get_memory(ctx);
      for (int s = 1; s < P; s++) {
        llama_memory_seq_cp(mem, 0, s, -1, -1);
      }

      //Every sequence samples its first token from the prompt's last logits.
      std::vector<int> idx(P, last_idx);
      std::vector<bool> active(P, true);
      std::vector<llama_token> cur(P);
      std::vector<char> piece(256);
      std::chrono::steady_clock::time_point first_tok;
      int generated_total = 0;
      for (int pos = n_prompt;; pos++) {
        int live = 0;
        for (int s = 0; s < P; s++) {
          if (!active[s]) continue;
          Reply& r = out[first + s];
          llama_token t = llama_sampler_sample(smpl[s], ctx, idx[s]);
          if (llama_vocab_is_eog(vocab, t)) {
            r.finish_reason = "stop";
            active[s] = false;
            continue;
          }
          int k = llama_token_to_piece(vocab, t, piece.data(), piece.size(), 0, true);
          if (k < 0) {
            piece.resize(-k);
            k = llama_token_to_piece(vocab, t, piece.data(), piece.size(), 0, true);
          }
          r.text.append(piece.data(), k);
          r.generated_tokens++;
          generated_total++;
          cur[s] = t;
          if (r.generated_tokens >= max_new) {
            active[s] = false;
            continue;
          }
          live++;
        }
        if (pos == n_prompt) {
          first_tok = std::chrono::steady_clock::now();
        }
        if (live == 0) {
          break;
        }
        batch.n_tokens = 0;
        for (int s = 0; s < P; s++) {
          if (active[s]) {
            idx[s] = batch.n_tokens;
            batch_add(batch, cur[s], pos, s, true);
          }
        }
        if (llama_decode(ctx, batch)) {
          throw std::runtime_error("llama_decode failed while sampling");
        }
      }
      double const prompt_s = std::chrono::duration<double>(first_tok - t0).count();
      double const decode_s = seconds_since(first_tok);
      for (int s = 0; s < P; s++) {
        Reply& r = out[first + s];
        r.prompt_seconds = prompt_s;
        r.decode_seconds = decode_s;
        //Aggregate over the batch: what the batch achieved, not one sequence.
        r.decode_tok_s = decode_s > 0 ? generated_total / decode_s : 0.0;
      }
      done += P;
      if (on_done) {
        on_done(done, n);
      }
    }
    return out;
  }

  //The prompt for a whole conversation so far -- (role, text) messages -- in
  //the model's chat template, ending where the assistant's next reply begins.
  std::string format_chat(std::vector<std::pair<std::string, std::string>> const& msgs) const {
    std::vector<llama_chat_message> m;
    size_t total = 0;
    for (auto const& [role, text] : msgs) {
      m.push_back({role.c_str(), text.c_str()});
      total += text.size();
    }
    char const* tmpl = llama_model_chat_template(model_, nullptr);
    std::vector<char> buf(total + 1024 + 64 * msgs.size());
    int len = llama_chat_apply_template(tmpl, m.data(), m.size(), true, buf.data(), buf.size());
    if (len > (int)buf.size()) {
      buf.resize(len);
      len = llama_chat_apply_template(tmpl, m.data(), m.size(), true, buf.data(), buf.size());
    }
    if (len < 0) {
      throw std::runtime_error("could not apply the model's chat template");
    }
    return std::string(buf.data(), len);
  }

  //A conversation of several user turns, fixed in advance: micro-prompting
  //(LLM_RTL_code_generation.md 8.6.2). Turn k is sent after the reply to turn
  //k - 1, with every earlier turn and reply before it, and each reply is fed
  //back exactly as generated: nothing here reads one. Returns, for each of the
  //`n` samples, its reply to every turn; the last is the measured one.
  //
  //Sampled like generate_samples (sample i seeded base_seed + i, its sampler
  //carried through the turns), or one greedy conversation if `greedy`. An
  //intermediate reply may run to `intermediate_cap` tokens and the final one
  //to max_new_tokens; a reply cut off at its cap is fed back as it stands.
  //
  //The samples of a batch are sequences of one context, as in
  //generate_samples. They share the first turn's prompt and then diverge, so
  //each keeps its own record of the tokens in its cache; a new turn decodes
  //only what the conversation's prompt adds to that record. Where re-tokenizing
  //a reply gives different tokens than were sampled, the cache is cut back to
  //the common prefix and the rest decoded again.
  std::vector<std::vector<Reply>> converse(std::string const& sys,
                                           std::vector<std::string> const& turns, int n,
                                           bool greedy, int intermediate_cap,
                                           std::function<void(int, int)> const& on_done = {}) {
    if (turns.empty()) {
      throw std::runtime_error("a conversation needs at least one turn");
    }
    json const& ls = cfg_.at("ladder_sampling");
    float const temperature = ls["temperature"];
    float const top_p = ls["top_p"];
    uint32_t const base_seed = ls["base_seed"];
    int const parallel = greedy ? 1 : ls["parallel_sequences"].get<int>();
    bool const kv_unified = ls.value("kv_unified", false);
    json const& cx = cfg_["context"];
    int const max_new = cfg_["generation"]["max_new_tokens"];
    int const n_batch = cx["n_batch"];
    int const K = (int)turns.size();
    const llama_vocab* vocab = llama_model_get_vocab(model_);
    auto cap_of = [&](int t) { return t == K - 1 ? max_new : intermediate_cap; };

    std::vector<llama_token> const toks0 = tokenize(format_chat({{"system", sys}, {"user", turns[0]}}));
    int const n_prompt = (int)toks0.size();
    //What one sequence can add to the shared first prompt: every reply at its
    //cap, every later turn, the template's tokens around each, and some slack
    //for a reply that re-tokenizes longer than it was sampled.
    int per_seq_extra = 64;
    for (int t = 0; t < K; t++) {
      per_seq_extra += cap_of(t);
      if (t > 0) {
        per_seq_extra += (int)tokenize(turns[t]).size() + 16;
      }
    }
    int const max_cells = ls.value("max_context_tokens", 1 << 30);
    int cap = kv_unified ? std::min(parallel, (max_cells - n_prompt) / per_seq_extra)
                         : std::min(parallel, max_cells / (n_prompt + per_seq_extra));
    if (cap < 1) {
      throw std::runtime_error("a conversation of " + std::to_string(n_prompt + per_seq_extra) +
                               " tokens does not fit max_context_tokens " + std::to_string(max_cells));
    }

    std::vector<std::vector<Reply>> out(n, std::vector<Reply>(K));
    int done = 0;
    for (int first = 0; first < n; first += cap) {
      int const P = std::min(cap, n - first);
      int const capacity = std::max(n_batch, P);

      auto cp = llama_context_default_params();
      cp.n_seq_max = P;
      cp.kv_unified = kv_unified;
      cp.n_ctx = kv_unified ? n_prompt + P * per_seq_extra : P * (n_prompt + per_seq_extra);
      cp.n_batch = capacity;
      cp.n_ubatch = cx["n_ubatch"];
      cp.n_threads = cx["n_threads"];
      cp.n_threads_batch = cx["n_threads_batch"];
      cp.flash_attn_type = flash_attn(cx["flash_attn"]);
      cp.type_k = kv_type(cx["type_k"]);
      cp.type_v = kv_type(cx["type_v"]);
      cp.offload_kqv = cx["offload_kqv"];
      llama_context* ctx = llama_init_from_model(model_, cp);
      if (!ctx) {
        throw std::runtime_error("could not create a context for " + std::to_string(P) +
                                 " conversations of " + std::to_string(n_prompt + per_seq_extra) + " tokens");
      }
      llama_batch batch = llama_batch_init(capacity, 0, P);
      std::vector<llama_sampler*> smpl(P, nullptr);
      struct Cleanup {
        llama_context* c; llama_batch* b; std::vector<llama_sampler*>* s;
        ~Cleanup() {
          for (auto* x : *s) if (x) llama_sampler_free(x);
          llama_batch_free(*b);
          llama_free(c);
        }
      } cleanup{ctx, &batch, &smpl};

      if ((int)llama_n_ctx_seq(ctx) < n_prompt + per_seq_extra) {
        throw std::runtime_error("each sequence got " + std::to_string(llama_n_ctx_seq(ctx)) +
                                 " tokens of context, fewer than the " +
                                 std::to_string(n_prompt + per_seq_extra) + " needed");
      }
      for (int s = 0; s < P; s++) {
        smpl[s] = llama_sampler_chain_init(llama_sampler_chain_default_params());
        uint32_t seed = 0;
        if (greedy) {
          llama_sampler_chain_add(smpl[s], llama_sampler_init_greedy());
        }
        else {
          seed = base_seed + (uint32_t)(first + s);
          llama_sampler_chain_add(smpl[s], llama_sampler_init_temp(temperature));
          llama_sampler_chain_add(smpl[s], llama_sampler_init_top_p(top_p, 1));
          llama_sampler_chain_add(smpl[s], llama_sampler_init_dist(seed));
        }
        for (int t = 0; t < K; t++) {
          out[first + s][t].seed = seed;
          out[first + s][t].finish_reason = "length";
        }
      }

      llama_memory_t mem = llama_get_memory(ctx);
      //Per sequence: the tokens its cache holds, the conversation so far, and
      //the token sampled from its last logits, not yet looked at.
      std::vector<std::vector<llama_token>> cached(P);
      std::vector<std::vector<std::pair<std::string, std::string>>> history(
          P, {{"system", sys}});
      std::vector<llama_token> next(P);
      std::vector<int> idx(P, 0);
      std::vector<char> piece(256);

      for (int t = 0; t < K; t++) {
        auto t0 = std::chrono::steady_clock::now();
        for (int s = 0; s < P; s++) {
          history[s].push_back({"user", turns[t]});
        }
        if (t == 0) {
          //The first prompt is the same for all: once into sequence 0, in
          //chunks, then shared, and every sequence samples from its last logits.
          int last_idx = 0;
          for (int i = 0; i < n_prompt; i += n_batch) {
            batch.n_tokens = 0;
            int const end = std::min(n_prompt, i + n_batch);
            for (int j = i; j < end; j++) {
              batch_add(batch, toks0[j], j, 0, j == n_prompt - 1);
            }
            if (llama_decode(ctx, batch)) {
              throw std::runtime_error("llama_decode failed on the first prompt");
            }
            last_idx = batch.n_tokens - 1;
          }
          for (int s = 0; s < P; s++) {
            if (s > 0) {
              llama_memory_seq_cp(mem, 0, s, -1, -1);
            }
            cached[s] = toks0;
            next[s] = llama_sampler_sample(smpl[s], ctx, last_idx);
          }
        }
        else {
          //Each sequence decodes what its conversation's prompt adds to its
          //cache. A sequence samples as soon as the batch holding its last
          //token is decoded: the next decode overwrites the logits.
          batch.n_tokens = 0;
          std::vector<std::pair<int, int>> ends;
          auto flush = [&] {
            if (batch.n_tokens == 0) {
              return;
            }
            if (llama_decode(ctx, batch)) {
              throw std::runtime_error("llama_decode failed on turn " + std::to_string(t + 1));
            }
            for (auto const& [s, i] : ends) {
              next[s] = llama_sampler_sample(smpl[s], ctx, i);
            }
            ends.clear();
            batch.n_tokens = 0;
          };
          for (int s = 0; s < P; s++) {
            std::vector<llama_token> target = tokenize(format_chat(history[s]));
            size_t p = 0;
            while (p < cached[s].size() && p < target.size() && cached[s][p] == target[p]) {
              p++;
            }
            if (p >= target.size()) {
              p = target.size() - 1;   //always decode something: the logits are needed
            }
            if (p < cached[s].size()) {
              llama_memory_seq_rm(mem, s, (llama_pos)p, -1);
            }
            for (size_t j = p; j < target.size(); j++) {
              bool const last = j + 1 == target.size();
              batch_add(batch, target[j], (llama_pos)j, s, last);
              if (last) {
                ends.push_back({s, batch.n_tokens - 1});
              }
              if (batch.n_tokens == capacity) {
                flush();
              }
            }
            cached[s] = std::move(target);
          }
          flush();
        }
        auto first_tok = std::chrono::steady_clock::now();

        //Generate this turn's replies, one token of every unfinished sequence
        //per decode.
        int const reply_cap = cap_of(t);
        std::vector<bool> active(P, true);
        int generated_total = 0;
        for (int s = 0; s < P; s++) {
          out[first + s][t].prompt_tokens = (int)cached[s].size();
        }
        for (;;) {
          batch.n_tokens = 0;
          for (int s = 0; s < P; s++) {
            if (!active[s]) continue;
            Reply& r = out[first + s][t];
            llama_token const tok = next[s];
            if (llama_vocab_is_eog(vocab, tok)) {
              r.finish_reason = "stop";
              active[s] = false;
              continue;
            }
            int k = llama_token_to_piece(vocab, tok, piece.data(), piece.size(), 0, true);
            if (k < 0) {
              piece.resize(-k);
              k = llama_token_to_piece(vocab, tok, piece.data(), piece.size(), 0, true);
            }
            r.text.append(piece.data(), k);
            r.generated_tokens++;
            generated_total++;
            if (r.generated_tokens >= reply_cap) {
              active[s] = false;
              continue;
            }
            idx[s] = batch.n_tokens;
            batch_add(batch, tok, (llama_pos)cached[s].size(), s, true);
            cached[s].push_back(tok);
          }
          if (batch.n_tokens == 0) {
            break;
          }
          if (llama_decode(ctx, batch)) {
            throw std::runtime_error("llama_decode failed while generating turn " + std::to_string(t + 1));
          }
          for (int s = 0; s < P; s++) {
            if (active[s]) {
              next[s] = llama_sampler_sample(smpl[s], ctx, idx[s]);
            }
          }
        }
        double const prompt_s = std::chrono::duration<double>(first_tok - t0).count();
        double const decode_s = seconds_since(first_tok);
        for (int s = 0; s < P; s++) {
          Reply& r = out[first + s][t];
          r.prompt_seconds = prompt_s;
          r.decode_seconds = decode_s;
          r.decode_tok_s = decode_s > 0 ? generated_total / decode_s : 0.0;
          history[s].push_back({"assistant", r.text});
        }
      }
      done += P;
      if (on_done) {
        on_done(done, n);
      }
    }
    return out;
  }

private:
  std::vector<llama_token> tokenize(std::string const& prompt) const {
    const llama_vocab* vocab = llama_model_get_vocab(model_);
    int n = -llama_tokenize(vocab, prompt.c_str(), prompt.size(), nullptr, 0, true, true);
    std::vector<llama_token> toks(n);
    llama_tokenize(vocab, prompt.c_str(), prompt.size(), toks.data(), n, true, true);
    return toks;
  }

  static void batch_add(llama_batch& b, llama_token t, llama_pos pos, llama_seq_id s, bool logits) {
    b.token[b.n_tokens] = t;
    b.pos[b.n_tokens] = pos;
    b.n_seq_id[b.n_tokens] = 1;
    b.seq_id[b.n_tokens][0] = s;
    b.logits[b.n_tokens] = logits;
    b.n_tokens++;
  }

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
