#include "../../domains/score_functions.h"
#include <math.h>
#include <stdlib.h>
#include <istream>
#include <boost/program_options.hpp>
#include "cpphop/loader.h"
#include "cpphop/cppMCTShop.h"
#include "cpphop/grapher.h"
#include <chrono>
namespace po = boost::program_options;

using namespace std;

int main(int argc, char* argv[]) {
  int time_limit = 1000;
  int r = 5;
  double c = sqrt(2.0);
  int seed = 2022;
  std::string dom_file = "../domains/transport_domain.hddl";
  std::string prob_file = "../domains/transport_problem.hddl";
  std::string score_fun = "delivery_one";
  bool graph = false;
  std::string graph_file = "";
  try {
    po::options_description desc("Allowed options");
    desc.add_options()
      ("help,h", "produce help message")
      ("time_limit,T", po::value<int>(), "Time limit (in milliseconds) allowed for each search decision (int), default = 1000. MCTS always spends the whole budget, so lower it for small domains")
      ("simulations,r", po::value<int>(), "Number of simulations per MCTS cycle (int), default = 5")
      ("exp_param,c",po::value<double>(),"The exploration parameter for the planner (double), default = sqrt(2)")
      ("dom_file,D", po::value<std::string>(),"domain file (string), default = transport_domain.hddl")
      ("prob_file,P",po::value<std::string>(),"problem file (string), default = transport_problem.hddl")
      ("score_fun,F",po::value<std::string>(),"name of score function (string), default = delivery_one")
      ("seed,s", po::value<int>(),"Random Seed (int)")
      ("graph,g",po::bool_switch()->default_value(false),"Creates a task tree graph of the returned plan and saves it as a png, default = false")
      ("graph_file,f",po::value<std::string>(), "File name for created graph (string), default = name of problem definition")
    ;

    po::variables_map vm;        
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
      std::cout << desc << std::endl;
      return 0;
    }

    if (vm.count("time_limit")) {
      time_limit = vm["time_limit"].as<int>();
    }

    if (vm.count("simulations")) {
      r = vm["simulations"].as<int>();
    }

    if (vm.count("exp_param")) {
      c = vm["exp_param"].as<double>();
    }

    if (vm.count("dom_file")) {
      dom_file = vm["dom_file"].as<std::string>();
    }

    if (vm.count("prob_file")) {
      prob_file = vm["prob_file"].as<std::string>();
    }

    if (vm.count("score_fun")) {
      score_fun = vm["score_fun"].as<std::string>();
    }

    if (vm.count("seed")) {
      seed = vm["seed"].as<int>();
    }

    if (vm.count("graph")) {
      graph = vm["graph"].as<bool>();
    }
    if (vm.count("graph_file")) {
      graph_file = vm["graph_file"].as<std::string>();
    }
  }
  catch(std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  catch(...) {
    std::cerr << "error: exception of unknown type while parsing options!\n";
    return 1;
  }

  //Loading, planning and graphing all throw on bad input (missing or malformed
  //domain and problem files, a domain the planner cannot make progress in).
  //Without this handler those escape main and the process aborts with no
  //usable message.
  try {
    if (!scorers.contains(score_fun)) {
      std::cerr << "error: unknown score function \"" << score_fun << "\". Available: ";
      bool first = true;
      for (auto const& [name,_] : scorers) {
        std::cerr << (first ? "" : ", ") << name;
        first = false;
      }
      std::cerr << std::endl;
      return 1;
    }

    auto [domain,problem] = load(dom_file,prob_file);
    if (problem.initM.get_head() != ":htn") {
      std::cout << "Problem class " << problem.initM.get_head() << " not recognized, defaulting to :htn problem class!" << std::endl;
    }

    if (graph) {
      if (graph_file == "") {
        graph_file = problem.head + ".png";
      }
      auto start = std::chrono::high_resolution_clock::now();
      auto results = cppMCTShop(domain,problem,scorers[score_fun],time_limit,r,c,seed);
      auto stop = std::chrono::high_resolution_clock::now();
      auto duration = std::chrono::duration_cast<std::chrono::microseconds>(stop - start);
      cout << "Time taken by planner: "
          << duration.count() << " microseconds" << endl;
      generate_graph(results.t[results.end].plan,
                     results.t[results.end].treeRoots,
                     domain,
                     results.tasktree,
                     graph_file);
    }
    else {
      auto start = std::chrono::high_resolution_clock::now();
      cppMCTShop(domain,problem,scorers[score_fun],time_limit,r,c,seed);
      auto stop = std::chrono::high_resolution_clock::now();
      auto duration = std::chrono::duration_cast<std::chrono::microseconds>(stop - start);
      cout << "Time taken by planner: "
          << duration.count() << " microseconds" << endl;
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
  return EXIT_SUCCESS;
}
