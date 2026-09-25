// Benchmark harness for the MCTS HTN planner.
//
// Two measurements, for two different jobs:
//
//   --rollouts N   Runs N rollouts from the initial state at a fixed seed and
//                  reports both their timing and their scores. The timing is
//                  the performance signal: rollouts dominate the planner's
//                  runtime, so this tracks the thing optimization work moves,
//                  and it takes seconds where a full plan can take minutes.
//                  The scores are the *semantic* signal -- they are driven by
//                  the seeded RNG, not by the clock, so they are reproducible
//                  run to run. Any change to how the planner decomposes,
//                  evaluates preconditions or scores states shows up as a
//                  changed score sequence. A refactor that is meant to be
//                  behaviour-preserving must leave it byte-identical.
//
//                  Scores are only as discriminating as the score function,
//                  and several domains here use `simple`, which returns 1.0
//                  for everything. So the rollouts also report the state of
//                  the RNG afterwards. The number and order of draws depends
//                  on every shuffle and every branch the search took, so it is
//                  a far more sensitive tripwire than the scores -- sensitive
//                  enough that a change which is genuinely behaviour-preserving
//                  can still move it, if it alters how many times the engine is
//                  asked for a number. Treat a changed rng_after as a prompt to
//                  look, and the scores and plans as the verdict.
//
//   --plan         Runs the planner proper and reports wall time, the plan and
//                  a hash of the final state. This is the end-to-end check,
//                  but it is bounded by wall clock, so a slower or faster
//                  machine explores a different number of MCTS iterations and
//                  the plan may legitimately differ. Treat a changed plan here
//                  as something to investigate, not as a failure.
//
//   --iterations N Runs the planner with a fixed number of MCTS iterations per
//                  decision instead of a time budget. This is what makes the
//                  *tree* search testable. Rollouts alone do not touch
//                  expansion, selection, backprop or the commit loop -- they go
//                  through `simulation`, which deliberately does not share
//                  expansion's Algorithm 2 restriction -- so a change to any of
//                  those is invisible to --rollouts and unreliable under
//                  --plan. With iterations fixed, a whole planner run becomes a
//                  deterministic function of the seed, and its plan and final
//                  state are a semantic fingerprint of the tree search.
//
// Output is one `key=value` per line so the driver script can parse it without
// caring about formatting.

#include "../../domains/score_functions.h"
#include "cpphop/loader.h"
#include "cpphop/cppMCTShop.h"
#include <boost/program_options.hpp>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <vector>
#include <string>

namespace po = boost::program_options;

//"compiled" | "at_start" | "protected"; see PreconditionMode in typedefs.h.
static bool parse_precondition_mode(std::string const& s, PreconditionMode& out) {
  if (s == "compiled")  { out = PreconditionMode::Compiled;  return true; }
  if (s == "at_start")  { out = PreconditionMode::AtStart;   return true; }
  if (s == "protected") { out = PreconditionMode::Protected; return true; }
  return false;
}

//FNV-1a. std::hash is not required to be stable across runs or platforms, and
//a baseline saved on one machine should be comparable on another.
static std::string fnv1a(std::string const& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  std::ostringstream os;
  os << std::hex << std::setw(16) << std::setfill('0') << h;
  return os.str();
}

//Facts live in unordered containers, so they have to be sorted before they can
//be hashed or compared.
static std::string canonical_state(KnowledgeBase& kb) {
  std::vector<std::string> facts;
  for (auto const& [head,fs] : kb.get_facts()) {
    for (auto const& f : fs) {
      facts.push_back(f);
    }
  }
  std::sort(facts.begin(),facts.end());
  std::string joined;
  for (auto const& f : facts) {
    joined += f;
    joined += "\n";
  }
  return joined;
}

int main(int argc, char* argv[]) {
  std::string dom_file, prob_file, score_fun = "simple";
  int seed = 2022, rollouts = 0, time_limit = 1000, r = 5, iterations = 0;
  int max_depth = kDefaultMaxRolloutDepth;
  int max_decisions = kDefaultMaxDecisions;
  int restrict_rollouts = 0;
  int algorithm = 2;
  std::string precondition_mode = "at_start";
  double c = sqrt(2.0);
  bool do_plan = false, show_state = false;

  try {
    po::options_description desc("Allowed options");
    desc.add_options()
      ("help,h", "produce help message")
      ("dom_file,D", po::value<std::string>(&dom_file)->required(), "domain file (required)")
      ("prob_file,P", po::value<std::string>(&prob_file)->required(), "problem file (required)")
      ("score_fun,F", po::value<std::string>(&score_fun), "score function, default = simple")
      ("seed,s", po::value<int>(&seed), "random seed, default = 2022")
      ("rollouts", po::value<int>(&rollouts), "time and score N rollouts from the initial state")
      ("plan", po::bool_switch(&do_plan), "run the planner end to end")
      ("iterations", po::value<int>(&iterations), "fixed MCTS iterations per decision instead of a time budget; makes the run deterministic")
      ("time_limit,T", po::value<int>(&time_limit), "planner budget per decision in ms, default = 1000")
      ("simulations,r", po::value<int>(&r), "rollouts per MCTS cycle, default = 5")
      ("exp_param,c", po::value<double>(&c), "exploration parameter, default = sqrt(2)")
      ("max_depth", po::value<int>(&max_depth), "depth bound for a rollout; default = 1000")
      ("max_decisions", po::value<int>(&max_decisions), "backstop on committed decisions; default = 1000")
      ("precondition_mode", po::value<std::string>(&precondition_mode), "how method preconditions are read: at_start (default), protected, or compiled (HDDL's own semantics)")
      ("algorithm", po::value<int>(&algorithm), "progression algorithm for expansion: 2 (default) branches over all unconstrained primitives plus one compound; 3 progresses no action while any compound task is unconstrained")
      ("restrict_rollouts", po::value<int>(&restrict_rollouts), "rollout candidate set: 0 = every unconstrained task (default), 1 = Algorithm 2 with expansion's lowest-id pick, 2 = Algorithm 2 with a random pick")
      ("show_state", po::bool_switch(&show_state), "also print the sorted final state")
    ;
    po::variables_map vm;
    po::store(po::parse_command_line(argc,argv,desc),vm);
    if (vm.count("help")) {
      std::cout << desc << std::endl;
      return 0;
    }
    po::notify(vm);
  }
  catch(std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }

  if (iterations > 0) {
    do_plan = true;
  }
  if (rollouts <= 0 && !do_plan) {
    std::cerr << "error: give --rollouts N, --plan, --iterations N, or a combination!\n";
    return 1;
  }

  try {
    if (!scorers.contains(score_fun)) {
      std::cerr << "error: unknown score function \"" << score_fun << "\"\n";
      return 1;
    }
    auto [domain,problem] = load(dom_file,prob_file);
    domain.set_scorer(scorers[score_fun]);
    if (!parse_precondition_mode(precondition_mode,domain.precondition_mode)) {
      std::cerr << "error: unknown --precondition_mode \"" << precondition_mode
                << "\" (expected compiled, at_start or protected)\n";
      return 1;
    }

    std::cout << "domain=" << dom_file << "\n";
    std::cout << "problem=" << prob_file << "\n";
    std::cout << "scorer=" << score_fun << "\n";
    std::cout << "seed=" << seed << "\n";
    std::cout << "precondition_mode=" << precondition_mode << "\n";

    if (rollouts > 0) {
      //Same initial node the planner builds, so the rollouts start where the
      //planner's would.
      KnowledgeBase state(domain.predicates,problem.objects,domain.typetree);
      for (auto const& f : problem.initF) {
        state.tell(f,false,false);
      }
      state.update_state();
      TaskGraph tasks;
      Grounded_Task init_t;
      init_t.head = problem.head;
      tasks.add_node(init_t);

      std::mt19937_64 g(seed);
      std::vector<double> ms;
      std::ostringstream scores;
      int failed = 0, cut = 0;
      for (int i = 0; i < rollouts; i++) {
        std::vector<std::string> plan;
        auto t0 = std::chrono::steady_clock::now();
        auto rs = simulation(plan,state,tasks,domain,g,max_depth,restrict_rollouts);
        ms.push_back(std::chrono::duration<double,std::milli>(
                       std::chrono::steady_clock::now()-t0).count());
        if (i) scores << ",";
        //"fail" and "cutoff" are reported apart because they mean different
        //things: the first is a proof that the initial network is a dead end,
        //the second only that the depth bound bit. A baseline where these two
        //swapped would be a behaviour change worth failing on.
        if (rs.status == RolloutStatus::Solved) {
          scores << std::fixed << std::setprecision(6) << rs.score;
        }
        else if (rs.status == RolloutStatus::Cutoff) {
          scores << "cutoff";
          cut++;
        }
        else {
          scores << "fail";
          failed++;
        }
      }
      //Rollout timing is skewed -- a single unlucky dive can cost several
      //times the typical one -- so report the median alongside the mean and
      //compare on the median.
      double sum = 0.0;
      for (double v : ms) sum += v;
      std::vector<double> sorted = ms;
      std::sort(sorted.begin(),sorted.end());
      double median = sorted.size() % 2
                    ? sorted[sorted.size()/2]
                    : 0.5*(sorted[sorted.size()/2 - 1] + sorted[sorted.size()/2]);
      std::cout << "rollouts=" << rollouts << "\n";
      std::cout << "rollout_failed=" << failed << "\n";
      std::cout << "rollout_cutoff=" << cut << "\n";
      std::cout << "max_depth=" << max_depth << "\n";
      std::cout << "restrict_rollouts=" << restrict_rollouts << "\n";
      //Four decimals: the fastest domains' rollouts take about ten
      //microseconds, which two decimals rounded to 0.01 or 0.00 -- a
      //"100% faster" out of nothing.
      std::cout << std::fixed << std::setprecision(4);
      std::cout << "rollout_ms_median=" << median << "\n";
      std::cout << "rollout_ms_mean=" << sum/ms.size() << "\n";
      std::cout << "rollout_ms_total=" << sum << "\n";
      std::cout << "rollout_ms_min=" << sorted.front() << "\n";
      std::cout << "rollout_ms_max=" << sorted.back() << "\n";
      //The reproducible part. Timing above will differ between machines; these
      //should not. Every score goes into the hash; the first few are printed
      //as well, so a difference can be read without the whole list, which for
      //a timing-sized run is thousands of entries.
      std::string all = scores.str();
      uint64_t h = 1469598103934665603ULL;  //FNV-1a
      for (unsigned char ch : all) {
        h = (h ^ ch) * 1099511628211ULL;
      }
      std::string head = all;
      size_t cut_at = 0;
      for (int n = 0; n < 20 && cut_at != std::string::npos; n++) {
        cut_at = head.find(',', cut_at == 0 ? 0 : cut_at + 1);
      }
      if (cut_at != std::string::npos) {
        head = head.substr(0,cut_at) + ",...";
      }
      std::ostringstream hs;
      hs << std::hex << std::setw(16) << std::setfill('0') << h;
      std::cout << "rollout_scores=" << head << "\n";
      std::cout << "rollout_scores_hash=" << hs.str() << "\n";
      std::ostringstream rng;
      rng << std::hex << std::setw(16) << std::setfill('0') << g();
      std::cout << "rng_after=" << rng.str() << "\n";
    }

    if (do_plan) {
      if (iterations > 0) {
        std::cout << "iterations=" << iterations << "\n";
      }
      else {
        std::cout << "time_limit=" << time_limit << "\n";
      }
      std::cout << "simulations=" << r << "\n";
      std::cout << "algorithm=" << algorithm << "\n";
      //The harness speaks key=value, so the planner's narration is off.
      domain.narration = nullptr;
      auto t0 = std::chrono::steady_clock::now();
      bool ok = true;
      std::string err;
      std::string plan_joined;
      size_t plan_len = 0;
      std::string state_canon;
      try {
        auto results = cppMCTShop(domain,problem,scorers[score_fun],time_limit,r,c,seed,iterations,max_depth,max_decisions,restrict_rollouts,algorithm);
        auto& end = results.t[results.end];
        plan_len = end.plan.size();
        for (size_t i = 0; i < end.plan.size(); i++) {
          if (i) plan_joined += "|";
          plan_joined += end.plan[i];
        }
        state_canon = canonical_state(end.state);
      }
      catch(std::exception& e) {
        ok = false;
        err = e.what();
      }
      double wall = std::chrono::duration<double,std::milli>(
                      std::chrono::steady_clock::now()-t0).count();
      std::cout << "plan_ok=" << (ok ? "1" : "0") << "\n";
      std::cout << std::fixed << std::setprecision(3) << "plan_wall_ms=" << wall << "\n";
      if (ok) {
        std::cout << "plan_len=" << plan_len << "\n";
        std::cout << "plan=" << plan_joined << "\n";
        std::cout << "final_state_hash=" << fnv1a(state_canon) << "\n";
        if (show_state) {
          std::istringstream ls(state_canon);
          std::string line;
          while (std::getline(ls,line)) {
            std::cout << "fact=" << line << "\n";
          }
        }
      }
      else {
        std::cout << "plan_error=" << err << "\n";
      }
    }
  }
  catch(std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  catch(...) {
    std::cerr << "error: exception of unknown type!\n";
    return 1;
  }
  return 0;
}
