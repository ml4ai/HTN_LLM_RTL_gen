//planner_doc.md 8.20: the planner is a header-only library, so a program with
//more than one source file that includes it must still link. This one has two.
//Before every function and variable its headers define was made inline, it
//failed to link with duplicate symbols. Linking at all is most of the test;
//planning across the two units checks the result is one coherent program.
#include "test_linkage_all.h"
#include <iostream>
#include <thread>
#include <latch>

int main() {
  auto [domain,problem] = load_in_other_unit();
  //A library caller: no narration on stdout.
  domain.narration = nullptr;
  auto results = cppMCTShop(domain,problem,scorers["travel_one"],50,1,sqrt(2.0),2022);
  auto const& plan = results.t[results.end].plan;
  if (plan.empty()) {
    std::cerr << "no plan across translation units" << std::endl;
    return 1;
  }
  std::cout << "planned " << plan.size() << " action(s) across two translation units" << std::endl;

  //Two planners at once, each on its own domain and problem, as a pipeline
  //might run them. Nothing they share may race: KnowledgeBase::ask's query
  //cache was a static until 8.20. Run under ThreadSanitizer, this is the test
  //of that; run normally, it checks both still plan.
  //Both loaded first, so the two searches start together and their first
  //queries overlap. ThreadSanitizer remembers only recent accesses to each
  //address, and a race whose two halves are far apart in time can go unseen.
  std::vector<std::pair<DomainDef,ProblemDef>> inputs;
  for (int k = 0; k < 2; k++) {
    //travel_one scores through KnowledgeBase::ask, so both threads use its
    //cache on every finished rollout.
    inputs.push_back(load(HTN_DOMAINS_DIR "/simple_travel.hddl",
                          HTN_DOMAINS_DIR "/simple_travel_problem.hddl"));
    inputs.back().first.narration = nullptr;
  }
  std::vector<size_t> lengths(2, 0);
  std::vector<std::thread> planners;
  for (int k = 0; k < 2; k++) {
    planners.emplace_back([k,&lengths,&inputs] {
      auto& [d,p] = inputs[k];
      auto r = cppMCTShop(d,p,scorers["travel_one"],30,1,sqrt(2.0),2022 + k);
      lengths[k] = r.t[r.end].plan.size();
    });
  }
  for (auto& t : planners) {
    t.join();
  }
  //The same, made certain to overlap: two threads released together, each
  //asking its own KnowledgeBase the same new queries, so both reach the query
  //cache's insertions at once. With the cache a shared static this is a data
  //race ThreadSanitizer reports; the planners above can miss it, because one
  //thread usually inserts a query long before the other first looks it up.
  std::latch start(2);
  std::vector<int> answered(2, 0);
  std::vector<std::thread> askers;
  for (int k = 0; k < 2; k++) {
    askers.emplace_back([k,&start,&answered,&inputs] {
      auto& [d,p] = inputs[k];
      KnowledgeBase kb(d.predicates,p.objects,d.typetree);
      for (auto const& f : p.initF) {
        kb.tell(f,false,false);
      }
      kb.update_state();
      start.arrive_and_wait();
      std::string q = "(and";
      for (int i = 0; i < 200; i++) {
        q += " (loc me home)";
        answered[k] += kb.ask(q + ")") ? 1 : 0;
      }
    });
  }
  for (auto& t : askers) {
    t.join();
  }
  if (answered[0] != 200 || answered[1] != 200) {
    std::cerr << "concurrent ground queries answered wrongly" << std::endl;
    return 1;
  }
  if (lengths[0] == 0 || lengths[1] == 0) {
    std::cerr << "a planner on a second thread found no plan" << std::endl;
    return 1;
  }
  std::cout << "planned on two threads at once" << std::endl;
  return 0;
}
