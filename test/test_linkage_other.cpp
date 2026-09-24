#include "test_linkage_all.h"

std::pair<DomainDef,ProblemDef> load_in_other_unit() {
  return load(HTN_DOMAINS_DIR "/simple_travel.hddl",
              HTN_DOMAINS_DIR "/simple_travel_problem.hddl");
}
