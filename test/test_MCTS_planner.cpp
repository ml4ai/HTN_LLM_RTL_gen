#define BOOST_TEST_MODULE TestMCTSPlanner

#include <boost/test/included/unit_test.hpp>
#include "../domains/score_functions.h"
#include <math.h>
#include <stdlib.h>
#include <istream>
#include "cpphop/loader.h"
#include "cpphop/cppMCTShop.h"

//simple_travel rather than transport. Under HDDL method-precondition timing a
//method's free variables are no longer bound by its precondition -- every
//type-consistent binding becomes a branch that the synthesised check rejects
//later -- and transport leans on exactly that: (at ?p ?l1) is what determines
//?l1. Its rollouts go from ~170ms to ~3s, which would make this test run for
//minutes. simple_travel exercises the same pipeline (methods with
//preconditions, the synthesised checks, actions, scoring) at ~17ms a rollout.
BOOST_AUTO_TEST_CASE(test_MCTS_planner) {
    auto [domain,problem] = load("../../domains/simple_travel.hddl",
                                 "../../domains/simple_travel_problem.hddl");

    auto results = cppMCTShop(domain,problem,scorers["travel_one"],500,1,sqrt(2.0),2022);
    auto& end_state = results.t[results.end].state;

    //The synthesised precondition checks must not show up in the plan
    BOOST_TEST(results.t[results.end].plan.size() == 1);
    for (auto const& step : results.t[results.end].plan) {
      BOOST_TEST(step.find("__mprec_") == std::string::npos);
    }

    //travel_one scores reaching the park, which is what the plan has to achieve
    BOOST_TEST(end_state.get_facts("loc").contains("(loc me park)"));

}// end of testing the planner

//Universally quantified effects, which no other domain in domains/ uses. Both
//forms must fire for every matching object: the unconditional one for all of
//them, the conditional one for exactly those meeting the condition.
BOOST_AUTO_TEST_CASE(test_forall_effects) {
    auto [domain,problem] = load("../../domains/forall_test.hddl",
                                 "../../domains/forall_test_problem.hddl");

    auto results = cppMCTShop(domain,problem,scorers["simple"],300,1,sqrt(2.0),2022);
    auto& end_state = results.t[results.end].state;

    //(forall (?l) (alerted ?l)) over three rooms
    auto alerted = end_state.get_facts("alerted");
    BOOST_TEST(alerted.size() == 3);
    BOOST_TEST(alerted.contains("(alerted room_a)"));
    BOOST_TEST(alerted.contains("(alerted room_b)"));
    BOOST_TEST(alerted.contains("(alerted room_c)"));

    //(forall (?l) (when (dirty ?l) (clean ?l))) with room_a and room_b dirty.
    //room_c must NOT be cleaned, which is what distinguishes a working
    //conditional effect from one that fires unconditionally.
    auto clean = end_state.get_facts("clean");
    BOOST_TEST(clean.size() == 2);
    BOOST_TEST(clean.contains("(clean room_a)"));
    BOOST_TEST(clean.contains("(clean room_b)"));
    BOOST_TEST(!clean.contains("(clean room_c)"));

}// end of testing quantified effects

//Zero-arity predicates (propositional atoms) and an empty "(and)" precondition.
//Atoms become plain Bool constants in SMT rather than nullary applications, and
//"(and)" means "no precondition" rather than a predicate named "and".
BOOST_AUTO_TEST_CASE(test_zero_arity_predicates) {
    auto [domain,problem] = load("../../domains/atom_test.hddl",
                                 "../../domains/atom_test_problem.hddl");

    auto results = cppMCTShop(domain,problem,scorers["simple"],300,1,sqrt(2.0),2022);
    auto& end_state = results.t[results.end].state;

    //disarm is only applicable while (armed) holds, so finding the plan at all
    //is what shows the atom was readable as a precondition.
    BOOST_TEST(results.t[results.end].plan.size() == 2);

    //(not (armed)) must have retracted it, and (safe) must have been added
    BOOST_TEST(end_state.get_facts("armed").empty());
    BOOST_TEST(end_state.get_facts("safe").contains("(safe)"));

    //inspect carries an empty (and) precondition and must still have run
    BOOST_TEST(end_state.get_facts("checked").contains("(checked bomb_1)"));

}// end of testing zero-arity predicates



//Method-precondition semantics (planner_doc.md 8.16). Under HDDL's compiled
//reading a method's precondition is checked by a synthesised action ordered
//before its subtasks, and in a partially ordered domain other tasks can run in
//between. On the mutex encoding of transport that lets two get_to tasks both
//choose m_goto -- whose precondition is that the truck is NOT yet there -- and
//the second then runs a redundant set_mutex/noop/release after the first has
//already driven there. AtStart and Protected must never produce that.
static int count_set_mutex(Results& results) {
    int n = 0;
    for (auto const& a : results.t[results.end].plan) {
        if (a.find("set_mutex") != std::string::npos) {
            n++;
        }
    }
    return n;
}

BOOST_AUTO_TEST_CASE(test_method_precondition_modes) {
    //Seeds on which compiled mode was measured to return the redundant sequence.
    std::vector<int> seeds = {2022, 7, 99};

    //The premise first: the fixture must still exhibit the violation, or the
    //assertions below would pass without testing anything.
    int compiled_redundant = 0;
    for (int seed : seeds) {
        auto [domain,problem] = load("../../domains/transport_mutex_left.hddl",
                                     "../../domains/transport_chain_b.hddl");
        auto results = cppMCTShop(domain,problem,scorers["delivery_chain"],1000,1,sqrt(2.0),seed,6);
        if (count_set_mutex(results) > 2) {
            compiled_redundant++;
        }
    }
    BOOST_TEST_REQUIRE(compiled_redundant > 0,
        "compiled mode no longer produces a redundant get_to on these seeds; "
        "pick new ones or this test proves nothing");

    for (auto mode : {PreconditionMode::AtStart, PreconditionMode::Protected}) {
        for (int seed : seeds) {
            auto [domain,problem] = load("../../domains/transport_mutex_left.hddl",
                                         "../../domains/transport_chain_b.hddl");
            domain.precondition_mode = mode;
            auto results = cppMCTShop(domain,problem,scorers["delivery_chain"],1000,1,sqrt(2.0),seed,6);
            BOOST_TEST_CONTEXT("mode " << (int)mode << " seed " << seed) {
                //Exactly two lock acquisitions: one per get_to that actually drives.
                BOOST_TEST(count_set_mutex(results) == 2);
                //And still a complete, shortest delivery: all packages at the end
                //of the chain, in the four drives the one-way chain requires.
                auto& end_state = results.t[results.end].state;
                BOOST_TEST(end_state.get_facts("at").contains("(at package_0 city_loc_4)"));
                BOOST_TEST(end_state.get_facts("at").contains("(at package_1 city_loc_4)"));
            }
        }
    }
}// end of testing method-precondition semantics
