#pragma once
//Every public header of the planner, for test_linkage. Two translation units
//include this together; if any header defines a non-inline function or
//variable, the program fails to link with a duplicate symbol.
#include "cpphop/loader.h"
#include "cpphop/validate.h"
#include "cpphop/cppMCTShop.h"
#include "cpphop/grapher.h"
#include "kb.h"
#include "typedefs.h"
#include "evaluator.h"
#include "expr.h"
#include "util.h"
#include "fol/util.h"
#include "../domains/score_functions.h"
#include "test_paths.h"

//Defined in test_linkage_other.cpp: loads simple_travel there, so the planner
//below runs on data built in the other translation unit.
std::pair<DomainDef,ProblemDef> load_in_other_unit();
