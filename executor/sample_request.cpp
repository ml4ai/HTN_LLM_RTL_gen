// executor_sample: generate samples of one request, for the evaluation harness
// (eval/). The request is a JSON file holding the benchmark adapter's messages:
//
//   {"system": "...", "user": "..."}
//
//   executor_sample --request REQ.json --out-dir DIR [--n N | --greedy]
//                   [--config CONFIG.json] [--verbose]
//
// --n N (default 20) draws N samples under the config's ladder_sampling block,
// sample i seeded base_seed + i. --greedy draws the single greedy reply (the
// greedy line), under the same frozen settings as executor_run. Writes
// DIR/sample_XX.response.txt, one per sample, and DIR/gen.json, which records
// the settings, seeds, token counts and timings, and is written last: a
// directory without gen.json is an unfinished request. Code extraction is the
// harness's, per benchmark, so nothing here parses the replies.
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

}  // namespace

int main(int argc, char** argv) {
  std::string req_path, out_dir, cfg_path = EXECUTOR_DEFAULT_CONFIG;
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
      else if (a == "--config") cfg_path = value();
      else if (a == "--n") n = std::stoi(value());
      else if (a == "--greedy") greedy = true;
      else if (a == "--verbose") verbose = true;
      else throw std::runtime_error("unknown argument " + a);
    }
    if (req_path.empty() || out_dir.empty()) {
      throw std::runtime_error("usage: executor_sample --request REQ.json --out-dir DIR [--n N | --greedy]");
    }
    if (greedy) {
      n = 1;
    }
    if (n < 1) {
      throw std::runtime_error("--n must be at least 1");
    }

    executor::json req = executor::json::parse(read_file(req_path));
    std::string const sys = req.at("system");
    std::string const user = req.at("user");
    fs::create_directories(out_dir);
    fs::remove(fs::path(out_dir) / "gen.json");  //unfinished until rewritten

    auto cfg = executor::load_config(cfg_path);
    executor::LibLlamaExecutor ex(cfg, verbose);

    auto t0 = std::chrono::steady_clock::now();
    std::vector<executor::Reply> replies;
    if (greedy) {
      replies.push_back(ex.generate(sys, user));
    }
    else {
      replies = ex.generate_samples(sys, user, n, [](int done, int total) {
        std::fprintf(stderr, "  %d/%d samples\n", done, total);
      });
    }
    double const wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    executor::json samples = executor::json::array();
    for (size_t i = 0; i < replies.size(); i++) {
      char name[64];
      std::snprintf(name, sizeof name, "sample_%02zu.response.txt", i);
      write_file(fs::path(out_dir) / name, replies[i].text);
      samples.push_back({{"file", name},
                         {"seed", greedy ? executor::json(nullptr) : executor::json(replies[i].seed)},
                         {"finish_reason", replies[i].finish_reason},
                         {"prompt_tokens", replies[i].prompt_tokens},
                         {"generated_tokens", replies[i].generated_tokens}});
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
    write_file(fs::path(out_dir) / "gen.json", gen.dump(2) + "\n");
    return 0;
  }
  catch (std::exception const& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
