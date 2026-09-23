#include "typedefs.h"
#include <string>
#include <algorithm>


double delivery_one(KnowledgeBase& kb,std::vector<std::string>& plan) {
  int move_count = 0;
  for (auto const& a : plan) {
    if (a.find("drive") != std::string::npos) {
      move_count++;
    }
  }
  if (kb.ask("(and (at package_0 city_loc_0) (at package_1 city_loc_2))")) {
    return 1.0*(1.0/(1.0 + move_count))*(1.0/(1 + plan.size()));
  }
  if (kb.ask("(capacity truck_0 capacity_0)")) {
    return 0.5*(1.0/(1.0 + move_count));
  }
  return 0.0;
}


//For the transport chain problems of scripts/gen_transport_chain.py, which
//vary in size, so the destinations cannot be written into the scorer the way
//delivery_one writes them. Each problem states them as static (dest ?p ?l)
//facts instead; a package is delivered when the matching (at ?p ?l) holds.
//
//Complete delivery always outranks partial delivery, and among complete plans
//fewer drives scores higher. That second part is what makes the four 8.7
//encodings comparable on plan quality: the insert model is free to emit a
//drive nothing asked for, and this is what notices.
double delivery_chain(KnowledgeBase& kb, std::vector<std::string>& plan) {
  auto facts = kb.get_facts();
  auto const& dests = facts["dest"];
  if (dests.empty()) {
    return 0.0;
  }
  double delivered = 0.0;
  for (auto const& d : dests) {
    //"(dest package_0 city_loc_3)" -> "(at package_0 city_loc_3)"
    if (facts["at"].contains("(at"+d.substr(5))) {
      delivered += 1.0;
    }
  }
  double frac = delivered/dests.size();
  if (frac < 1.0) {
    return 0.5*frac;
  }
  int drives = 0;
  for (auto const& a : plan) {
    if (a.find("drive") != std::string::npos) {
      drives++;
    }
  }
  return 0.5 + 0.5/(1.0 + drives);
}

double simple(KnowledgeBase& kb, std::vector<std::string>& plan) {
  return 1.0;
}

double travel_one(KnowledgeBase& kb, std::vector<std::string>& plan) {
  if (kb.ask("(and (loc me park) (cash me twenty))")) {
    return 1;
  }
  if (kb.ask("(loc me park)")) {
    return 0.5;
  }
  return 0.0;
}

double sar3(KnowledgeBase& kb, std::vector<std::string>& plan) {
  auto facts = kb.get_facts();
  double c_vic_count = 0.0;
  if (facts.find("vic_is_type_C") != facts.end()) {
    for (auto const& p : facts["vic_is_type_C"]) {
      c_vic_count += 1.0; 
    }
  }
  double r_vic_count = 0.0;
  if (facts.find("vic_is_type_A") != facts.end()) {
    for (auto const& p : facts["vic_is_type_A"]) {
      r_vic_count += 1.0;
    }
  }
  if (facts.find("vic_is_type_B") != facts.end()) {
    for (auto const& p : facts["vic_is_type_B"]) {
      r_vic_count += 1.0;
    }
  }
  double c_res = 0.0;
  if (facts.find("rescued_c") != facts.end()) {
    for (auto const& p : facts["rescued_c"]) {
      c_res += 1.0;
    }
  }
  double r_res = 0.0;
  if (facts.find("rescued_c") != facts.end()) {
    for (auto const& p : facts["rescued_r"]) {
      r_res += 1.0;
    }
  }
  double p_total = c_vic_count*50.0 + r_vic_count*10.0;
  double points = c_res*50.0 + r_res*10.0;
  return points/p_total;
}

Scorers scorers = Scorers({{"delivery_one", delivery_one},
                           {"delivery_chain", delivery_chain},
                           {"travel_one", travel_one},
                           {"sar3",sar3},
                           {"simple", simple}});
