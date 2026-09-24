#include "typedefs.h"
#include <string>
#include <algorithm>


inline double delivery_one(KnowledgeBase& kb,std::vector<std::string>& plan) {
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
inline double delivery_chain(KnowledgeBase& kb, std::vector<std::string>& plan) {
  //Asks the fact index for the dest facts and one lookup per package, rather
  //than rebuilding the whole state as strings with get_facts().
  auto dests = kb.facts_of("dest");
  if (dests.empty()) {
    return 0.0;
  }
  double delivered = 0.0;
  for (auto const& d : dests) {
    //(dest package_0 city_loc_3) is met by (at package_0 city_loc_3).
    if (kb.holds("at",d)) {
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

inline double simple(KnowledgeBase& kb, std::vector<std::string>& plan) {
  return 1.0;
}

inline double travel_one(KnowledgeBase& kb, std::vector<std::string>& plan) {
  if (kb.ask("(and (loc me park) (cash me twenty))")) {
    return 1;
  }
  if (kb.ask("(loc me park)")) {
    return 0.5;
  }
  return 0.0;
}

//The share of the available points the plan earns: 50 for each critical
//(type C) victim rescued, 10 for each regular one.
//
//"Regular" means what sar3's rescue effect means by it -- a victim that is not
//type C gets rescued_r -- so the regular total is every victim less the
//critical ones. It used to be the type A and type B victims, which let a
//victim of neither type score points it was never counted against.
//
//Two faults fixed here (planner_doc.md 8.18). Regular rescues were counted only
//when rescued_c had any fact at all, a copy-paste slip, so a plan rescuing only
//regular victims scored nothing for them. And a problem with no victims divided
//by zero; a NaN in a node's value corrupts UCT's comparisons. With nothing to
//rescue, every plan has done all there is to do.
inline double sar3(KnowledgeBase& kb, std::vector<std::string>& plan) {
  double critical = kb.count_facts("vic_is_type_C");
  double regular = std::max(0.0, (double)kb.count_facts("victim") - critical);
  double p_total = critical*50.0 + regular*10.0;
  if (p_total == 0.0) {
    return 1.0;
  }
  double points = kb.count_facts("rescued_c")*50.0 + kb.count_facts("rescued_r")*10.0;
  return points/p_total;
}

//inline: one table for the whole program, however many source files include
//this header (planner_doc.md 8.20).
inline Scorers scorers = Scorers({{"delivery_one", delivery_one},
                           {"delivery_chain", delivery_chain},
                           {"travel_one", travel_one},
                           {"sar3",sar3},
                           {"simple", simple}});
