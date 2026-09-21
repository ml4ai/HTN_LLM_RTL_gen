#define BOOST_TEST_MODULE TestMCTSPlanner

#include <boost/test/included/unit_test.hpp>
#include "../domains/score_functions.h"
#include <math.h>
#include <stdlib.h>
#include <istream>
#include "cpphop/loader.h"
#include "cpphop/cppMCTShop.h"

BOOST_AUTO_TEST_CASE(test_MCTS_planner) {
    auto [domain,problem] = load("../../domains/transport_domain.hddl",
                                 "../../domains/transport_problem.hddl");

    auto results = cppMCTShop(domain,problem,scorers["delivery_one"],1000,1,sqrt(2.0),2022);;
    BOOST_TEST(results.t[results.end].plan.size() == 8);
    BOOST_TEST(results.t[results.end].state.get_facts("at").contains("(at package_0 city_loc_0)"));
    BOOST_TEST(results.t[results.end].state.get_facts("at").contains("(at package_1 city_loc_2)"));

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


