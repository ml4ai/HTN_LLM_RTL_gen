//The task-hierarchy graph (planner_doc.md 8.22). Plans transport with a fixed
//iteration count, so the run is reproducible, draws it as DOT and SVG, and
//checks the properties the drawing is designed to have.
#define BOOST_TEST_MODULE TestGrapher
#include <boost/test/included/unit_test.hpp>
#include <filesystem>
#include <fstream>
#include <regex>
#include "../domains/score_functions.h"
#include "cpphop/loader.h"
#include "cpphop/cppMCTShop.h"
#include "cpphop/grapher.h"
#include "test_paths.h"

namespace {
std::string slurp(std::filesystem::path const& p) {
  std::ifstream f(p);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct Planned {
  DomainDef domain;
  ProblemDef problem;
  Results results;
};

Planned plan_transport() {
  auto [domain,problem] = load(HTN_DOMAINS_DIR "/transport_domain.hddl",
                               HTN_DOMAINS_DIR "/transport_problem.hddl");
  domain.narration = nullptr;
  auto results = cppMCTShop(domain,problem,scorers["delivery_one"],1000,1,sqrt(2.0),2022,10);
  return {domain,problem,results};
}
}

BOOST_AUTO_TEST_CASE(test_task_graph) {
  auto p = plan_transport();
  auto& end = p.results.t[p.results.end];
  BOOST_TEST_REQUIRE(!end.plan.empty());

  //The committed tree records the method behind every compound task, and
  //none for an action.
  int compound = 0;
  for (auto const& [id,tn] : p.results.tasktree) {
    bool action = p.domain.actions.contains(tn.task);
    BOOST_TEST_CONTEXT(tn.token) {
      if (action) {
        BOOST_TEST(tn.method.empty());
      }
      else {
        BOOST_TEST(!tn.method.empty());
        compound++;
      }
    }
  }
  BOOST_TEST(compound > 1);

  auto dir = std::filesystem::temp_directory_path() / "htn_test_grapher";
  std::filesystem::create_directories(dir);
  auto dot = dir / "transport.dot";
  generate_graph(end.plan,end.treeRoots,p.domain,p.results.tasktree,dot.string());
  std::string g = slurp(dot);

  //The synthesised precondition checks are not drawn; the methods are.
  BOOST_TEST(g.find("__mprec_") == std::string::npos);
  BOOST_TEST(g.find("by m_deliver_ordering_0") != std::string::npos);
  //Every plan step is numbered.
  for (size_t k = 1; k <= end.plan.size(); k++) {
    BOOST_TEST_CONTEXT("step " << k) {
      BOOST_TEST(g.find("step " + std::to_string(k) + "<") != std::string::npos);
    }
  }

  //Edges by colour: red joins consecutive plan steps; blue dashed are the
  //ordering constraints, which must be transitively reduced.
  std::regex edge(R"re((n\d+) -> (n\d+)\s*\[([^\]]*)\])re");
  int red = 0;
  std::map<std::string,std::set<std::string>> blue;
  for (auto it = std::sregex_iterator(g.begin(),g.end(),edge); it != std::sregex_iterator(); ++it) {
    std::string attrs = (*it)[3];
    if (attrs.find("#dc2626") != std::string::npos) red++;
    if (attrs.find("#3b82f6") != std::string::npos) blue[(*it)[1]].insert((*it)[2]);
  }
  BOOST_TEST(red == (int)end.plan.size() - 1);
  BOOST_TEST(!blue.empty());
  for (auto const& [a,bs] : blue) {
    for (auto const& b : bs) {
      //b must not be reachable from a except by the edge itself.
      std::vector<std::string> todo;
      for (auto const& c : bs) if (c != b) todo.push_back(c);
      std::set<std::string> seen;
      bool implied = false;
      while (!todo.empty() && !implied) {
        auto x = todo.back();
        todo.pop_back();
        if (x == b) implied = true;
        if (!seen.insert(x).second) continue;
        for (auto const& y : blue[x]) todo.push_back(y);
      }
      BOOST_TEST_CONTEXT(a << " -> " << b) {
        BOOST_TEST(!implied);
      }
    }
  }

  //SVG carries a tooltip per node, naming the full task and its method.
  auto svg = dir / "transport.svg";
  generate_graph(end.plan,end.treeRoots,p.domain,p.results.tasktree,svg.string());
  std::string s = slurp(svg);
  BOOST_TEST(s.find("xlink:title=\"(deliver package_0 city_loc_0)") != std::string::npos);

  //Colouring by a type, and the two ways to ask for something impossible.
  GraphOptions by_vehicle;
  by_vehicle.colour_by = "vehicle";
  by_vehicle.objects = &p.problem.objects;
  generate_graph(end.plan,end.treeRoots,p.domain,p.results.tasktree,dot.string(),by_vehicle);
  BOOST_TEST(slurp(dot).find("colour = the vehicle involved") != std::string::npos);
  GraphOptions by_nothing;
  by_nothing.colour_by = "spaceship";
  BOOST_CHECK_THROW(generate_graph(end.plan,end.treeRoots,p.domain,p.results.tasktree,
                                   dot.string(),by_nothing), std::invalid_argument);
  BOOST_CHECK_THROW(generate_graph(end.plan,end.treeRoots,p.domain,p.results.tasktree,
                                   (dir / "transport.jpeg").string()), std::invalid_argument);
  std::filesystem::remove_all(dir);
}
