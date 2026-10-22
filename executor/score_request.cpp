// executor_score: the model's log-probability of each candidate reply to a
// request, for reading its preference over a menu's entries
// (eval/learn_steps.py). The request is a JSON file:
//
//   {"system": "...", "user": "...", "candidates": ["1", "2", ...]}
//
//   executor_score (--request REQ.json --out-dir DIR | --batch LIST.json)
//                  [--config CONFIG.json] [--verbose]
//
// A candidate's score is the log-probability of its tokens and then the end
// of the turn, after the prompt: the probability that the whole reply is that
// candidate. No sampling is involved, so the scores are deterministic on a
// given machine and build. Writes DIR/scores.json, then DIR/gen.json last: a
// directory without gen.json is an unfinished request. A batch is a JSON list
// of {"request": ..., "out_dir": ...}, with the model loaded once.
//
// Exit status: 0 on success, 1 on any error.

#include "executor.h"

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

void run_request(executor::LibLlamaExecutor& ex, executor::json const& cfg, std::string const& cfg_path,
                 std::string const& req_path, std::string const& out_dir) {
  executor::json req = executor::json::parse(read_file(req_path));
  auto candidates = req.at("candidates").get<std::vector<std::string>>();
  if (candidates.empty()) {
    throw std::runtime_error("\"candidates\" must not be empty");
  }
  fs::create_directories(out_dir);
  fs::remove(fs::path(out_dir) / "gen.json");
  auto t0 = std::chrono::steady_clock::now();
  std::vector<double> lp = ex.score(req.at("system"), req.at("user"), candidates);
  double const wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  executor::json scores = executor::json::array();
  for (size_t i = 0; i < candidates.size(); i++) {
    scores.push_back({{"candidate", candidates[i]}, {"logprob", lp[i]}});
  }
  write_file(fs::path(out_dir) / "scores.json", scores.dump(1) + "\n");
  executor::json gen = {
    {"timestamp_utc", utc_now()},
    {"mode", "score"},
    {"config", {{"path", fs::absolute(cfg_path).string()},
                {"config_version", cfg["config_version"]},
                {"sha256", executor::sha256(read_file(cfg_path))}}},
    {"model_sha256_pinned", cfg["model"]["sha256"]},
    {"llama_cpp_commit", EXECUTOR_LLAMA_COMMIT},
    {"request_sha256", executor::sha256(read_file(req_path))},
    {"candidates", candidates.size()},
    {"wall_seconds", wall},
  };
  write_file(fs::path(out_dir) / "gen.json", gen.dump(2) + "\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string req_path, out_dir, batch_path, cfg_path = EXECUTOR_DEFAULT_CONFIG;
  bool verbose = false;
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
      else if (a == "--verbose") verbose = true;
      else throw std::runtime_error("unknown argument " + a);
    }
    if (batch_path.empty() == (req_path.empty() || out_dir.empty())) {
      throw std::runtime_error("usage: executor_score (--request REQ.json --out-dir DIR | --batch LIST.json)");
    }
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
      run_request(ex, cfg, cfg_path, jobs[i].first, jobs[i].second);
    }
    return 0;
  }
  catch (std::exception const& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
