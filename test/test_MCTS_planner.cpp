#define BOOST_TEST_MODULE TestMCTSPlanner

#include <boost/test/included/unit_test.hpp>
#include "../domains/score_functions.h"
#include <math.h>
#include <stdlib.h>
#include <istream>
#include <set>
#include <fstream>
#include "cpphop/loader.h"
#include "cpphop/cppMCTShop.h"
#include "test_paths.h"

//simple_travel rather than transport. Under HDDL method-precondition timing a
//method's free variables are no longer bound by its precondition -- every
//type-consistent binding becomes a branch that the synthesised check rejects
//later -- and transport leans on exactly that: (at ?p ?l1) is what determines
//?l1. Its rollouts go from ~170ms to ~3s, which would make this test run for
//minutes. simple_travel exercises the same pipeline (methods with
//preconditions, the synthesised checks, actions, scoring) at ~17ms a rollout.
BOOST_AUTO_TEST_CASE(test_MCTS_planner) {
    auto [domain,problem] = load(HTN_DOMAINS_DIR "/simple_travel.hddl",
                                 HTN_DOMAINS_DIR "/simple_travel_problem.hddl");

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
    auto [domain,problem] = load(HTN_DOMAINS_DIR "/forall_test.hddl",
                                 HTN_DOMAINS_DIR "/forall_test_problem.hddl");

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

//(either a b) types (planner_doc.md 8.17). The loader used to throw bad_get on
//one. A parameter or quantified variable of type (either cat dog) must range
//over the objects of both types and nothing else: never the bird.
BOOST_AUTO_TEST_CASE(test_either_types) {
    std::string dom = R"(
(define (domain pets)
  (:requirements :typing :hierarchy)
  (:types cat dog bird - object)
  (:predicates (fed ?p - (either cat dog)) (counted ?p) (done))
  (:task feed_one)
  (:method m_feed
    :parameters (?p - (either cat dog))
    :task (feed_one)
    :subtasks (and (task0 (feed ?p)) (task1 (count_all))))
  (:action feed
    :parameters (?p - (either dog cat))
    :precondition (not (fed ?p))
    :effect (fed ?p))
  (:action count_all
    :parameters ()
    :effect (and (forall (?q - (either cat dog)) (counted ?q)) (done))))
)";
    std::string prob = R"(
(define (problem pets_1)
  (:domain pets)
  (:objects tom - cat rex - dog tweety - bird)
  (:htn :subtasks (feed_one))
  (:init))
)";
    std::set<std::string> fed_seen;
    for (int seed : {1, 2, 3, 4, 5, 6, 7, 8}) {
        auto [domain,problem] = load_hddl(dom, prob);
        auto results = cppMCTShop(domain,problem,scorers["simple"],20,1,sqrt(2.0),seed);
        auto& end_state = results.t[results.end].state;
        auto fed = end_state.get_facts("fed");
        BOOST_TEST_REQUIRE(fed.size() == 1u);
        fed_seen.insert(*fed.begin());
        //The quantified range: both member types, not the bird.
        auto counted = end_state.get_facts("counted");
        BOOST_TEST(counted.size() == 2u);
        BOOST_TEST(counted.contains("(counted tom)"));
        BOOST_TEST(counted.contains("(counted rex)"));
    }
    //The parameter: some seed picks each member, and none picks the bird.
    BOOST_TEST(fed_seen.contains("(fed tom)"));
    BOOST_TEST(fed_seen.contains("(fed rex)"));
    BOOST_TEST(!fed_seen.contains("(fed tweety)"));
}

//A domain built or edited in code skips the loader's validation. The planner
//must still refuse a subtask argument nothing binds, rather than ground it to
//an object named after the variable (planner_doc.md 8.17).
BOOST_AUTO_TEST_CASE(test_planner_rejects_unbound_names) {
    auto [domain,problem] = load(HTN_DOMAINS_DIR "/simple_travel.hddl",
                                 HTN_DOMAINS_DIR "/simple_travel_problem.hddl");
    auto& [task,methods] = *domain.methods.begin();
    auto m = methods.front();
    auto subtasks = m.get_subtasks();
    BOOST_REQUIRE(!subtasks.empty());
    auto& first = subtasks.begin()->second;
    BOOST_REQUIRE(!first.second.empty());
    first.second[0].first = "unbound_variable";
    methods.front() = MethodDef(m.get_head(), m.get_task(), m.get_parameters(),
                                m.get_preconditions(), subtasks, m.get_orderings(),
                                m.get_precondition_ast());
    try {
        cppMCTShop(domain,problem,scorers["travel_one"],50,1,sqrt(2.0),2022);
        BOOST_FAIL("an unbound subtask argument was planned with");
    }
    catch (std::invalid_argument const& e) {
        std::string what = e.what();
        BOOST_TEST_INFO(what);
        BOOST_TEST(what.find("passes unbound_variable to subtask") != std::string::npos);
    }
}

//Typed variables in a forall effect (planner_doc.md 8.17). The grammar used to
//accept only untyped ones. The declared type must narrow the range: over a
//predicate that takes any object, a typed forall reaches only that type's
//objects, while an untyped one reaches every object -- the robot included.
BOOST_AUTO_TEST_CASE(test_typed_forall_effects) {
    std::ifstream fd(HTN_DOMAINS_DIR "/forall_test.hddl");
    std::string dom((std::istreambuf_iterator<char>(fd)), std::istreambuf_iterator<char>());
    std::ifstream fp(HTN_DOMAINS_DIR "/forall_test_problem.hddl");
    std::string prob((std::istreambuf_iterator<char>(fp)), std::istreambuf_iterator<char>());
    auto sub = [](std::string s, std::string const& from, std::string const& to) {
        auto at = s.find(from);
        BOOST_REQUIRE(at != std::string::npos);
        return s.replace(at, from.size(), to);
    };
    dom = sub(dom, "(alerted ?l - room)", "(alerted ?l - room)\n\t\t(seen_typed ?o)\n\t\t(seen_any ?o)");
    dom = sub(dom, "(forall (?l) (alerted ?l))",
              "(forall (?l) (alerted ?l))\n\t\t\t(forall (?x - room) (seen_typed ?x))\n\t\t\t(forall (?y) (seen_any ?y))");

    auto [domain,problem] = load_hddl(dom, prob);
    auto results = cppMCTShop(domain,problem,scorers["simple"],300,1,sqrt(2.0),2022);
    auto& end_state = results.t[results.end].state;

    auto typed = end_state.get_facts("seen_typed");
    BOOST_TEST(typed.size() == 3u);
    BOOST_TEST(!typed.contains("(seen_typed bot)"));
    auto any = end_state.get_facts("seen_any");
    BOOST_TEST(any.size() == 4u);
    BOOST_TEST(any.contains("(seen_any bot)"));
}

//Zero-arity predicates (propositional atoms) and an empty "(and)" precondition.
//Atoms become plain Bool constants in SMT rather than nullary applications, and
//"(and)" means "no precondition" rather than a predicate named "and".
BOOST_AUTO_TEST_CASE(test_zero_arity_predicates) {
    auto [domain,problem] = load(HTN_DOMAINS_DIR "/atom_test.hddl",
                                 HTN_DOMAINS_DIR "/atom_test_problem.hddl");

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
    //assertions below would pass without testing anything. Compiled is set
    //explicitly -- it is no longer the default.
    int compiled_redundant = 0;
    for (int seed : seeds) {
        auto [domain,problem] = load(HTN_DOMAINS_DIR "/transport_mutex_left.hddl",
                                     HTN_DOMAINS_DIR "/transport_chain_b.hddl");
        domain.precondition_mode = PreconditionMode::Compiled;
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
            auto [domain,problem] = load(HTN_DOMAINS_DIR "/transport_mutex_left.hddl",
                                         HTN_DOMAINS_DIR "/transport_chain_b.hddl");
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
