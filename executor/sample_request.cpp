// executor_sample: generate samples of one request, for the evaluation harness
// (eval/). The request is a JSON file holding the benchmark adapter's messages:
//
//   {"system": "...", "user": "..."}
//
// or, for micro-prompting (LLM_RTL_code_generation.md 8.6.2), a conversation
// whose user turns are fixed in advance:
//
//   {"system": "...", "turns": ["...", "..."], "intermediate_max_new_tokens": N}
//
// Turn k is sent after the reply to turn k - 1, with the conversation so far;
// replies are fed back exactly as generated. The reply to the last turn is
// the sample; the earlier ones are kept beside it for the record.
//
//   executor_sample (--request REQ.json --out-dir DIR | --batch LIST.json)
//                   [--n N | --greedy]
//                   [--config CONFIG.json] [--verbose]
//
// --n N (default 20) draws N samples under the config's ladder_sampling block,
// sample i seeded base_seed + i. --greedy draws the single greedy reply (the
// greedy line), under the same frozen settings as executor_run. Writes
// DIR/sample_XX.response.txt, one per sample, and DIR/gen.json, which records
// the settings, seeds, token counts and timings, and is written last: a
// directory without gen.json is an unfinished request. A conversation also
// writes DIR/sample_XX.turns.json, every turn with its reply. Code extraction
// is the harness's, per benchmark, so nothing here parses the replies.
//
// Exit status: 0 on success, 1 on any error.

#include "executor.h"

#include <cstring>
#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

std::string read_file(fs::path const& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    throw std::runtime_error("cannot read " + p.string());
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void write_file(fs::path const& p, std::string const& s) {
  std::ofstream out(p, std::ios::binary);
  if (!out || !(out << s)) {
    throw std::runtime_error("cannot write " + p.string());
  }
}

std::string utc_now() {
  std::time_t t = std::time(nullptr);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
  return buf;
}

//One request: generate, write the samples, and write gen.json last.
void run_request(executor::LibLlamaExecutor& ex, executor::json const& cfg,
                 std::string const& cfg_path, std::string const& req_path,
                 std::string const& out_dir, int n, bool greedy) {
  executor::json req = executor::json::parse(read_file(req_path));
  std::string const sys = req.at("system");
  bool const conversation = req.contains("turns");
  if (conversation == req.contains("user")) {
    throw std::runtime_error("a request holds either \"user\" or \"turns\"");
  }
  std::vector<std::string> turns;
  int intermediate_cap = 0;
  if (conversation) {
    turns = req.at("turns").get<std::vector<std::string>>();
    intermediate_cap = req.at("intermediate_max_new_tokens");
    if (turns.empty() || intermediate_cap < 1) {
      throw std::runtime_error("\"turns\" must be non-empty and "
                               "\"intermediate_max_new_tokens\" at least 1");
    }
  }
  fs::create_directories(out_dir);
  fs::remove(fs::path(out_dir) / "gen.json");  //unfinished until rewritten

  auto t0 = std::chrono::steady_clock::now();
  auto progress = [](int done, int total) {
    std::fprintf(stderr, "  %d/%d samples\n", done, total);
  };
  //Per sample, its reply to every turn; a plain request is one turn.
  std::vector<std::vector<executor::Reply>> replies;
  if (conversation) {
    replies = ex.converse(sys, turns, n, greedy, intermediate_cap, progress);
  }
  else if (greedy) {
    replies.push_back({ex.generate(sys, req.at("user").get<std::string>())});
  }
  else {
    for (auto& r : ex.generate_samples(sys, req.at("user").get<std::string>(), n, progress)) {
      replies.push_back({std::move(r)});
    }
  }
  double const wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  executor::json samples = executor::json::array();
  for (size_t i = 0; i < replies.size(); i++) {
    //The measured reply is the last turn's.
    executor::Reply const& last = replies[i].back();
    char name[64];
    std::snprintf(name, sizeof name, "sample_%02zu.response.txt", i);
    write_file(fs::path(out_dir) / name, last.text);
    executor::json sample = {{"file", name},
                             {"seed", greedy ? executor::json(nullptr) : executor::json(last.seed)},
                             {"finish_reason", last.finish_reason},
                             {"prompt_tokens", last.prompt_tokens},
                             {"generated_tokens", last.generated_tokens}};
    if (conversation) {
      executor::json record = executor::json::array();
      int total = 0;
      for (size_t t = 0; t < replies[i].size(); t++) {
        executor::Reply const& r = replies[i][t];
        record.push_back({{"user", turns[t]}, {"reply", r.text},
                          {"finish_reason", r.finish_reason},
                          {"prompt_tokens", r.prompt_tokens},
                          {"generated_tokens", r.generated_tokens}});
        total += r.generated_tokens;
      }
      std::snprintf(name, sizeof name, "sample_%02zu.turns.json", i);
      write_file(fs::path(out_dir) / name, record.dump(1) + "\n");
      sample["turns_file"] = name;
      sample["calls"] = replies[i].size();
      sample["generated_tokens_all_turns"] = total;
    }
    samples.push_back(sample);
  }
  executor::json gen = {
    {"timestamp_utc", utc_now()},
    {"mode", greedy ? "greedy" : "sampled"},
    {"n", n},
    {"config", {{"path", fs::absolute(cfg_path).string()},
                {"config_version", cfg["config_version"]},
                {"sha256", executor::sha256(read_file(cfg_path))}}},
    {"sampling", greedy ? cfg["sampling"] : cfg["ladder_sampling"]},
    {"model_sha256_pinned", cfg["model"]["sha256"]},
    {"llama_cpp_commit", EXECUTOR_LLAMA_COMMIT},
    {"request_sha256", executor::sha256(read_file(req_path))},
    {"wall_seconds", wall},
    {"samples", samples},
  };
  if (conversation) {
    gen["turns"] = turns.size();
    gen["intermediate_max_new_tokens"] = intermediate_cap;
  }
  write_file(fs::path(out_dir) / "gen.json", gen.dump(2) + "\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string req_path, out_dir, batch_path, cfg_path = EXECUTOR_DEFAULT_CONFIG;
  int n = 20;
  bool greedy = false, verbose = false;
  try {
    for (int i = 1; i < argc; i++) {
      std::string a = argv[i];
      auto value = [&]() -> std::string {
        if (i + 1 >= argc) throw std::runtime_error(a + " needs a value");
        return argv[++i];
      };
      if (a == "--request") req_path = value();
      else if (a == "--out-dir") out_dir = value();
      else if (a == "--batch") batch_path = value();
      else if (a == "--config") cfg_path = value();
      else if (a == "--n") n = std::stoi(value());
      else if (a == "--greedy") greedy = true;
      else if (a == "--verbose") verbose = true;
      else throw std::runtime_error("unknown argument " + a);
    }
    if (batch_path.empty() == (req_path.empty() || out_dir.empty())) {
      throw std::runtime_error("usage: executor_sample (--request REQ.json --out-dir DIR | --batch LIST.json) "
                               "[--n N | --greedy]");
    }
    if (greedy) {
      n = 1;
    }
    if (n < 1) {
      throw std::runtime_error("--n must be at least 1");
    }

    //A batch is a JSON list of {"request": ..., "out_dir": ...}: the same as
    //one invocation each, with the model loaded once.
    std::vector<std::pair<std::string, std::string>> jobs;
    if (batch_path.empty()) {
      jobs.push_back({req_path, out_dir});
    }
    else {
      for (auto const& j : executor::json::parse(read_file(batch_path))) {
        jobs.push_back({j.at("request"), j.at("out_dir")});
      }
    }

    auto cfg = executor::load_config(cfg_path);
    executor::LibLlamaExecutor ex(cfg, verbose);
    for (size_t i = 0; i < jobs.size(); i++) {
      if (!batch_path.empty()) {
        std::fprintf(stderr, "[%zu/%zu] %s\n", i + 1, jobs.size(), jobs[i].second.c_str());
      }
      run_request(ex, cfg, cfg_path, jobs[i].first, jobs[i].second, n, greedy);
    }
    return 0;
  }
  catch (std::exception const& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
