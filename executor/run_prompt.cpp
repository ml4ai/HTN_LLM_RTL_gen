// executor_run: give the local LLM a prompt from a file and save the Verilog it
// writes.
//
//   executor_run PROMPT.txt [-o OUT.v] [--out-dir DIR] [--config CONFIG.json]
//                [--quiet] [--verbose]
//
// The prompt file's whole contents are the user message; the system prompt and
// every generation setting come from the frozen executor_config.json. The
// reply streams to stdout as it is generated (--quiet turns that off); progress
// goes to stderr. Three files are written, named after the prompt file unless
// -o names the Verilog:
//
//   NAME.v              the last fenced ```verilog block of the reply
//   NAME.response.txt   the whole reply
//   NAME.run.json       a record of the run: config, model hash, prompt,
//                       reply, token counts and timings
//
// Exit status: 0 when Verilog was extracted, 2 when the reply has no
// ```verilog block (an abstention; NAME.v is not written, and a stale one is
// removed), 1 on any error.

#include "executor.h"

#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace {

void usage(char const* argv0) {
  std::fprintf(stderr,
    "usage: %s PROMPT.txt [-o OUT.v] [--out-dir DIR] [--config CONFIG.json] [--quiet] [--verbose]\n",
    argv0);
}

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
  std::string prompt_arg, out_v, out_dir, cfg_path = EXECUTOR_DEFAULT_CONFIG;
  bool quiet = false, verbose = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto value = [&](char const* flag) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(std::string(flag) + " needs a value");
      }
      return argv[++i];
    };
    try {
      if (a == "-o" || a == "--output") out_v = value("-o");
      else if (a == "--out-dir") out_dir = value("--out-dir");
      else if (a == "--config") cfg_path = value("--config");
      else if (a == "--quiet") quiet = true;
      else if (a == "--verbose") verbose = true;
      else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
      else if (!a.empty() && a[0] == '-') { usage(argv[0]); return 1; }
      else if (prompt_arg.empty()) prompt_arg = a;
      else { usage(argv[0]); return 1; }
    }
    catch (std::exception const& e) {
      std::fprintf(stderr, "error: %s\n", e.what());
      return 1;
    }
  }
  if (prompt_arg.empty()) {
    usage(argv[0]);
    return 1;
  }

  try {
    fs::path const prompt_path = prompt_arg;
    std::string const user = read_file(prompt_path);
    if (user.find_first_not_of(" \t\r\n") == std::string::npos) {
      throw std::runtime_error(prompt_path.string() + " is empty");
    }

    //Where the three outputs go: next to -o if given, else in --out-dir (or the
    //current directory), all named after the prompt file.
    fs::path v_path, base;
    if (!out_v.empty()) {
      v_path = out_v;
      base = v_path.parent_path() / v_path.stem();
    }
    else {
      base = fs::path(out_dir.empty() ? "." : out_dir) / prompt_path.stem();
      v_path = base.string() + ".v";
    }
    fs::path const resp_path = base.string() + ".response.txt";
    fs::path const run_path = base.string() + ".run.json";
    if (!base.parent_path().empty()) {
      fs::create_directories(base.parent_path());
    }

    auto cfg = executor::load_config(cfg_path);
    std::fprintf(stderr, "config: %s (version %d)\n", cfg_path.c_str(), cfg["config_version"].get<int>());
    executor::LibLlamaExecutor ex(cfg, verbose);
    std::fprintf(stderr, "model: %s %s on %s, loaded in %.1f s\n",
                 cfg["model"]["name"].get<std::string>().c_str(),
                 cfg["model"]["quantization"].get<std::string>().c_str(), ex.gpu().c_str(),
                 ex.load_seconds());
    std::fprintf(stderr, "generating (greedy, up to %d tokens)...\n",
                 cfg["generation"]["max_new_tokens"].get<int>());

    auto stream = [&](std::string const& p) {
      std::cout << p << std::flush;
    };
    executor::Reply r = quiet ? ex.generate(user) : ex.generate(user, stream);
    if (!quiet) {
      std::cout << std::endl;
    }

    std::string const code = executor::last_verilog_block(r.text);
    bool const abstained = code.empty();
    write_file(resp_path, r.text);
    if (abstained) {
      //A .v left over from an earlier run would look like this run's answer.
      fs::remove(v_path);
    }
    else {
      write_file(v_path, code);
    }

    executor::json run = {
      {"timestamp_utc", utc_now()},
      {"config", {{"path", fs::absolute(cfg_path).string()},
                  {"config_version", cfg["config_version"]},
                  {"sha256", executor::sha256(read_file(cfg_path))}}},
      {"model", {{"name", cfg["model"]["name"]},
                 {"quantization", cfg["model"]["quantization"]},
                 {"sha256_pinned", cfg["model"]["sha256"]},
                 {"note", "the pinned hash from the config; the file is not re-hashed per run "
                          "(executor_smoke --verify-hash does that)"}}},
      {"llama_cpp_commit", EXECUTOR_LLAMA_COMMIT},
      {"gpu", ex.gpu()},
      {"prompt", {{"file", fs::absolute(prompt_path).string()},
                  {"sha256", executor::sha256(user)},
                  {"system", cfg["generation"]["system_prompt"]},
                  {"user", user}}},
      {"reply", {{"text", r.text},
                 {"finish_reason", r.finish_reason},
                 {"prompt_tokens", r.prompt_tokens},
                 {"generated_tokens", r.generated_tokens}}},
      {"extraction", {{"rule", cfg["generation"]["extraction"]},
                      {"abstained", abstained},
                      {"verilog_file", abstained ? executor::json(nullptr)
                                                 : executor::json(fs::absolute(v_path).string())}}},
      {"timing_s", {{"model_load", ex.load_seconds()},
                    {"prompt", r.prompt_seconds},
                    {"decode", r.decode_seconds},
                    {"decode_tok_per_s", r.decode_tok_s}}},
    };
    write_file(run_path, run.dump(2) + "\n");

    std::fprintf(stderr, "%d prompt tokens, %d generated (%s), %.1f tok/s\n", r.prompt_tokens,
                 r.generated_tokens, r.finish_reason.c_str(), r.decode_tok_s);
    std::fprintf(stderr, "reply:   %s\nrun log: %s\n", resp_path.c_str(), run_path.c_str());
    if (abstained) {
      std::fprintf(stderr, "no ```verilog block in the reply%s: nothing extracted (abstention)\n",
                   r.finish_reason == "length" ? " (it was cut off at max_new_tokens)" : "");
      return 2;
    }
    std::fprintf(stderr, "verilog: %s\n", v_path.c_str());
    return 0;
  }
  catch (std::exception const& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
