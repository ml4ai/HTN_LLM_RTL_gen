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

//The sar3 score function (planner_doc.md 8.18). It used to count regular
//rescues only when some critical victim had been rescued too, and to divide by
//zero on a problem with no victims. It now also counts what it needs from the
//fact index instead of rebuilding the state as strings.
BOOST_AUTO_TEST_CASE(test_sar3_scorer) {
    std::ifstream fd(HTN_DOMAINS_DIR "/sar3.hddl");
    std::string dom((std::istreambuf_iterator<char>(fd)), std::istreambuf_iterator<char>());
    std::ifstream fp(HTN_DOMAINS_DIR "/sar3p1.hddl");
    std::string prob((std::istreambuf_iterator<char>(fp)), std::istreambuf_iterator<char>());
    //Two more victims: vic2 of type A, and vic3 of no type at all, whom the
    //domain's rescue effect would still mark rescued_r.
    auto at = prob.find("vic1 - victim");
    BOOST_REQUIRE(at != std::string::npos);
    prob.replace(at, 13, "vic1 vic2 vic3 - victim");
    auto [domain,problem] = load_hddl(dom, prob);

    auto kb_with = [&](std::vector<std::string> const& facts,
                       std::vector<std::string> const& drop_objects = {}) {
        auto objects = problem.objects;
        for (auto const& o : drop_objects) {
            objects.erase(o);
        }
        KnowledgeBase kb(domain.predicates, objects, domain.typetree);
        for (auto const& f : problem.initF) {
            kb.tell(f, false, false);
        }
        kb.tell("(vic_is_type_A vic2)", false, false);
        for (auto const& f : facts) {
            BOOST_REQUIRE(kb.tell(f, false, false));
        }
        kb.update_state();
        return kb;
    };
    std::vector<std::string> plan;
    //vic1 is critical (50 points); vic2 and vic3 are regular (10 each).
    auto none = kb_with({});
    BOOST_TEST(sar3(none, plan) == 0.0);
    //The bug: with no critical victim rescued this scored 0.
    auto regular_only = kb_with({"(rescued_r vic2)"});
    BOOST_TEST(sar3(regular_only, plan) == 10.0/70.0, boost::test_tools::tolerance(1e-12));
    //vic3 is of neither type A nor B, and still counts against the total, so
    //rescuing everyone scores exactly 1 rather than more.
    auto everyone = kb_with({"(rescued_c vic1)", "(rescued_r vic2)", "(rescued_r vic3)"});
    BOOST_TEST(sar3(everyone, plan) == 1.0, boost::test_tools::tolerance(1e-12));
    //No victims: nothing to rescue, and not NaN.
    auto empty = kb_with({}, {"vic1", "vic2", "vic3"});
    double score = sar3(empty, plan);
    BOOST_TEST(!std::isnan(score));
    BOOST_TEST(score == 1.0);
}

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

//Typed existential preconditions (planner_doc.md 8.27): (not (exists (?x -
//thing) ...)) next to a conjunct binding another parameter, in a problem with
//an object outside the type. The old SMT encoding read every typed exists as
//true there, so m_none was never applicable and planning failed outright.
BOOST_AUTO_TEST_CASE(test_typed_exists_preconditions) {
    auto [domain,problem] = load(HTN_DOMAINS_DIR "/exists_test.hddl",
                                 HTN_DOMAINS_DIR "/exists_test_problem.hddl");

    auto results = cppMCTShop(domain,problem,scorers["simple"],300,1,sqrt(2.0),2022);
    auto const& plan = results.t[results.end].plan;
    std::vector<std::string> steps;
    for (auto const& a : plan) {
      if (a.find("__mprec_") == std::string::npos) {
        steps.push_back(a.substr(0, a.rfind('_')));
      }
    }
    BOOST_TEST(steps.size() == 2);
    BOOST_TEST(std::count(steps.begin(), steps.end(), "(mark m1 a b)") == 1);
    BOOST_TEST(std::count(steps.begin(), steps.end(), "(mark m1 c c)") == 1);

}// end of testing typed exists

//Names shared between kinds of thing (planner_doc.md 8.28): ?s0 beside an
//object s0, ?module beside the type module, ?next beside the predicate next, a
//quantified ?idle beside the constant idle, an object walker beside the
//predicate walker and an object state beside the type state. visit's
//precondition is an imply, so it goes to Z3 in every build, and Z3 used to
//refuse it ("ambiguous constant reference ... disambiguate s0"); the direct
//evaluator, for its part, read ?s0 pinned to s0 as free. The walk has one
//plan.
BOOST_AUTO_TEST_CASE(test_names_do_not_clash) {
    auto [domain,problem] = load(HTN_DOMAINS_DIR "/name_clash_test.hddl",
                                 HTN_DOMAINS_DIR "/name_clash_test_problem.hddl");

    auto results = cppMCTShop(domain,problem,scorers["simple"],300,1,sqrt(2.0),2022);
    std::vector<std::string> steps;
    for (auto const& a : results.t[results.end].plan) {
      if (a.find("__mprec_") == std::string::npos) {
        steps.push_back(a.substr(0, a.rfind('_')));
      }
    }
    std::vector<std::string> const expected =
        {"(visit walker s0)", "(visit walker state)", "(visit walker x_0)"};
    BOOST_TEST(steps == expected, boost::test_tools::per_element());

}// end of testing shared names

//A constant in a method's :task restricts what the method decomposes: m_home
//is for (go home) only. The direct evaluator used to skip a pinned name that
//was not a variable, so m_home also took (go work) -- two seeds in six
//planned (mark home) (flag) for it. Z3, given (= home work), never did.
BOOST_AUTO_TEST_CASE(test_constant_in_method_task) {
    std::string const dom = R"(
(define (domain head_const)
  (:requirements :typing :hierarchy)
  (:types place - object)
  (:constants home work - place)
  (:predicates (been ?p - place) (special))
  (:task go :parameters (?p - place))
  (:method m_home
    :parameters ()
    :task (go home)
    :ordered-subtasks (and (t1 (mark home)) (t2 (flag))))
  (:method m_any
    :parameters (?p - place)
    :task (go ?p)
    :ordered-subtasks (and (t1 (mark ?p))))
  (:action mark :parameters (?p - place) :precondition () :effect (been ?p))
  (:action flag :parameters () :precondition () :effect (special))
))";
    std::string const prob = R"(
(define (problem head_const_p)
  (:domain head_const)
  (:objects cafe - place)
  (:htn :parameters () :ordered-subtasks (and (t1 (go work))))
  (:init)
))";
    for (int seed = 1; seed <= 6; seed++) {
        BOOST_TEST_CONTEXT("seed " << seed) {
            auto [domain,problem] = load_hddl(dom, prob);
            domain.narration = nullptr;
            auto results = cppMCTShop(domain,problem,scorers["simple"],50,1,sqrt(2.0),seed);
            std::vector<std::string> steps;
            for (auto const& a : results.t[results.end].plan) {
                steps.push_back(a.substr(0, a.rfind('_')));
            }
            std::vector<std::string> const expected = {"(mark work)"};
            BOOST_TEST(steps == expected, boost::test_tools::per_element());
        }
    }
}// end of testing a constant in a method's task

//A constant that shares a parameter's name (planner_doc.md 8.30). Argument
//lists hold names, a variable's without its '?', so ?home and the constant
//home were stored alike and grounding, which substitutes by name, replaced the
//constant with the parameter's value: decomposing (go work), the subtask
//(mark home) became (mark work), the effect (linked ?home home) wrote
//(linked work work), and a :task (pair home ?home) read both of its arguments
//as the one parameter. The loader now records which arguments are constants.
BOOST_AUTO_TEST_CASE(test_constant_named_like_a_parameter) {
    std::string const dom = R"(
(define (domain shared_names)
  (:requirements :typing :hierarchy :universal-preconditions :conditional-effects)
  (:types place - object)
  (:constants home work - place)
  (:predicates (been ?p - place) (linked ?a ?b - place) (near ?a ?b - place))
  (:task go :parameters (?home - place))
  (:task pair :parameters (?a ?b - place))
  (:method m_go
    :parameters (?home - place)
    :task (go ?home)
    :ordered-subtasks (and (t1 (mark ?home)) (t2 (mark home)) (t3 (tie ?home))
                           (t4 (pair home ?home))))
  (:method m_pair_home
    :parameters (?home - place)
    :task (pair home ?home)
    :ordered-subtasks (and (t1 (survey ?home))))
  (:action mark :parameters (?p - place) :precondition () :effect (been ?p))
  (:action tie :parameters (?home - place) :precondition () :effect (linked ?home home))
  (:action survey :parameters (?p - place) :precondition ()
    :effect (forall (?work - place) (near ?work work)))
))";
    std::string const prob = R"(
(define (problem shared_names_p)
  (:domain shared_names)
  (:objects cafe - place)
  (:htn :parameters () :ordered-subtasks (and (t1 (go work))))
  (:init)
))";
    auto [domain,problem] = load_hddl(dom, prob);
    domain.narration = nullptr;

    //What the loader recorded: t2's argument and t4's first are constants.
    auto& m_go = domain.methods["go"].front();
    auto const& consts = m_go.get_constants();
    BOOST_TEST(consts.task.empty());
    BOOST_TEST_REQUIRE(consts.subtasks.contains("t2"));
    BOOST_TEST_REQUIRE(consts.subtasks.contains("t4"));
    BOOST_TEST(!consts.subtasks.contains("t1"));
    BOOST_TEST((consts.subtasks.at("t2") == ConstantArgs{true}));
    BOOST_TEST((consts.subtasks.at("t4") == ConstantArgs{true,false}));

    auto results = cppMCTShop(domain,problem,scorers["simple"],50,1,sqrt(2.0),2022);
    std::vector<std::string> steps;
    for (auto const& a : results.t[results.end].plan) {
        steps.push_back(a.substr(0, a.rfind('_')));
    }
    std::vector<std::string> const expected =
        {"(mark work)", "(mark home)", "(tie work)", "(survey work)"};
    BOOST_TEST(steps == expected, boost::test_tools::per_element());

    auto& end_state = results.t[results.end].state;
    BOOST_TEST(end_state.get_facts("been").contains("(been home)"));
    BOOST_TEST(end_state.get_facts("been").contains("(been work)"));
    //The effect's constant is home, and the forall's is work, whatever the
    //parameter and the quantified variable of those names are bound to.
    auto linked = end_state.get_facts("linked");
    BOOST_TEST(linked.size() == 1u);
    BOOST_TEST(linked.contains("(linked work home)"));
    auto near = end_state.get_facts("near");
    BOOST_TEST(near.size() == 3u);
    for (auto const* place : {"home", "work", "cafe"}) {
        BOOST_TEST(near.contains(std::string("(near ")+place+" work)"));
    }

    //(:task (pair home ?home)): the first argument must be home, and the
    //second is what ?home is bound to.
    auto& m_pair = domain.methods["pair"].front();
    BOOST_TEST((m_pair.get_constants().task == ConstantArgs{true,false}));
    KnowledgeBase kb(domain.predicates,problem.objects,domain.typetree);
    Args from_home = {{"a","home"},{"b","work"}};
    auto bound = m_pair.bindings(kb,from_home);
    BOOST_TEST_REQUIRE(bound.size() == 1u);
    BOOST_TEST(return_value("home",bound[0]) == "work");
    Args from_work = {{"a","work"},{"b","work"}};
    BOOST_TEST(m_pair.bindings(kb,from_work).empty());
    Args home_home = {{"a","home"},{"b","home"}};
    BOOST_TEST(m_pair.bindings(kb,home_home).size() == 1u);

    //A problem's :htn has parameters and objects both. With a parameter ?cafe,
    //the task (go cafe) is still for the object cafe; it used to be for
    //whatever ?cafe was bound to, and five seeds in six planned another place.
    std::string const prob_htn = R"(
(define (problem shared_names_htn)
  (:domain shared_names)
  (:objects cafe - place)
  (:htn :parameters (?cafe - place) :ordered-subtasks (and (t1 (go cafe))))
  (:init)
))";
    for (int seed = 1; seed <= 6; seed++) {
        BOOST_TEST_CONTEXT("seed " << seed) {
            auto [d,p] = load_hddl(dom, prob_htn);
            d.narration = nullptr;
            auto r = cppMCTShop(d,p,scorers["simple"],50,1,sqrt(2.0),seed);
            std::vector<std::string> plan;
            for (auto const& a : r.t[r.end].plan) {
                plan.push_back(a.substr(0, a.rfind('_')));
            }
            std::vector<std::string> const want =
                {"(mark cafe)", "(mark home)", "(tie cafe)", "(survey cafe)"};
            BOOST_TEST(plan == want, boost::test_tools::per_element());
        }
    }

}// end of testing a constant named like a parameter



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

//Precondition look-ahead (planner_doc.md 8.26) may drop a binding whose
//precondition fails now only when the decomposed task is the only free task,
//because then nothing can run before the check. Here it is not: consume's
//item is readied by make_ready, which is free when `use` is decomposed.
//Algorithm 3 is what makes this bite. It progresses no action while a compound
//task is free, so `use` must be decomposed before make_ready can run, with
//(ready b) still false. Algorithm 2 would also branch on running make_ready
//first, and hide a look-ahead that pruned too eagerly.
BOOST_AUTO_TEST_CASE(test_lookahead_respects_interleaving) {
    std::string dom = R"(
(define (domain lookahead_test)
  (:requirements :typing :hierarchy :method-preconditions)
  (:types item - object)
  (:predicates (spare ?i - item) (ready ?i - item) (used ?i - item))
  (:task go :parameters ())
  (:task use :parameters ())
  (:method m_go
    :parameters (?s - item)
    :task (go)
    :subtasks (and (t1 (use)) (t2 (make_ready ?s))))
  (:method m_use
    :parameters (?i - item)
    :task (use)
    :precondition (ready ?i)
    :ordered-subtasks (and (t1 (consume ?i))))
  (:action make_ready
    :parameters (?i - item)
    :precondition (spare ?i)
    :effect (ready ?i))
  (:action consume
    :parameters (?i - item)
    :precondition (ready ?i)
    :effect (used ?i)))
)";
    std::string prob = R"(
(define (problem lookahead_p)
  (:domain lookahead_test)
  (:objects a b - item)
  (:htn :parameters () :subtasks (and (go)))
  (:init (spare b)))
)";
    for (int algorithm : {2, 3}) {
        for (bool lookahead : {false, true}) {
            BOOST_TEST_CONTEXT("algorithm " << algorithm << " lookahead " << lookahead) {
                auto [domain,problem] = load_hddl(dom, prob);
                domain.narration = nullptr;
                auto results = cppMCTShop(domain,problem,scorers["simple"],0,1,sqrt(2.0),2022,
                                          10,kDefaultMaxRolloutDepth,kDefaultMaxDecisions,
                                          algorithm == 3 ? 3 : 0,algorithm,true,lookahead);
                auto& end = results.t[results.end];
                BOOST_TEST(end.state.get_facts("used").contains("(used b)"));
                BOOST_TEST(end.plan.size() == 2u);
            }
        }
    }
    //With nothing spare there is no plan, and the planner must say it proved
    //that rather than blame its time limit, which it used to whenever the
    //root had no successors -- including when the root itself was refuted.
    std::string none = prob;
    none.replace(none.find("(:init (spare b))"), 17, "(:init)");
    for (bool lookahead : {false, true}) {
        auto [domain,problem] = load_hddl(dom, none);
        domain.narration = nullptr;
        BOOST_CHECK_EXCEPTION(
            cppMCTShop(domain,problem,scorers["simple"],0,1,sqrt(2.0),2022,
                       10,kDefaultMaxRolloutDepth,kDefaultMaxDecisions,0,2,true,lookahead),
            std::logic_error,
            [](std::logic_error const& e) {
                return std::string(e.what()).find("no plan exists") != std::string::npos;
            });
    }
}

//A fixed iteration count makes the plan a function of the seed (planner_doc.md
//8.31). The domain leaves every decision open: each of six items may go left
//or right, and both score the same, so nothing but the random tie-break picks.
//Bounded by time, such a search commits to whatever the iterations that fit
//the budget happened to favour, and the same seed returned different plans
//from run to run. Bounded by count, two runs must agree step for step, and
//the choice must still be the seed's: over a handful of seeds, more than one
//plan appears.
BOOST_AUTO_TEST_CASE(test_fixed_iterations_repeat_an_open_choice) {
    std::string const dom = R"(
(define (domain open_choice)
  (:requirements :typing :hierarchy)
  (:types item - object)
  (:predicates (left ?i - item) (right ?i - item))
  (:task place :parameters (?i - item))
  (:method m_left
    :parameters (?i - item)
    :task (place ?i)
    :ordered-subtasks (and (t1 (put_left ?i))))
  (:method m_right
    :parameters (?i - item)
    :task (place ?i)
    :ordered-subtasks (and (t1 (put_right ?i))))
  (:action put_left :parameters (?i - item) :precondition () :effect (left ?i))
  (:action put_right :parameters (?i - item) :precondition () :effect (right ?i))
))";
    std::string const prob = R"(
(define (problem open_choice_p)
  (:domain open_choice)
  (:objects a b c d e f - item)
  (:htn :parameters ()
        :ordered-subtasks (and (t1 (place a)) (t2 (place b)) (t3 (place c))
                               (t4 (place d)) (t5 (place e)) (t6 (place f))))
  (:init)
))";
    auto plan_with = [&](int seed, int iterations) {
        auto [domain,problem] = load_hddl(dom, prob);
        domain.narration = nullptr;
        auto results = cppMCTShop(domain,problem,scorers["simple"],0,1,sqrt(2.0),seed,iterations);
        return results.t[results.end].plan;
    };
    std::set<std::vector<std::string>> distinct;
    for (int seed = 1; seed <= 8; seed++) {
        BOOST_TEST_CONTEXT("seed " << seed) {
            auto first = plan_with(seed, 20);
            BOOST_TEST(first.size() == 6u);
            for (int again = 0; again < 3; again++) {
                BOOST_TEST(plan_with(seed, 20) == first, boost::test_tools::per_element());
            }
            distinct.insert(first);
        }
    }
    BOOST_TEST(distinct.size() > 1u);

    //One iteration is the root's own rollout and expands nothing. The planner
    //must name the budget that ran out, which is not the time limit here.
    BOOST_CHECK_EXCEPTION(
        plan_with(1, 1), std::logic_error,
        [](std::logic_error const& e) {
            return std::string(e.what()).find("iteration budget") != std::string::npos;
        });
}

//The RTL demo's plan (rtl_designs/), as a regression test of both 8.26 changes
//on the domain they were made for. Every decision in it is forced and its plan
//is unique, so early commit, which may only ever skip search that could not
//change a decision, must return exactly the committed plan whether it is on
//or off. Four iterations a decision is enough only because look-ahead keeps
//the doomed bindings out of the tree: without it, a decision's few explored
//children are often all dead, and the planner backtracks until its decision
//cap stops it.
BOOST_AUTO_TEST_CASE(test_rtl_fsm_plan) {
    std::string const dir = HTN_DOMAINS_DIR "/../rtl_designs";
    std::vector<std::string> expected;
    {
        std::ifstream in(dir + "/fsm/fsm_plan.txt");
        BOOST_TEST_REQUIRE(in.good(), "cannot read " << dir << "/fsm/fsm_plan.txt");
        for (std::string line; std::getline(in, line);) {
            if (!line.empty()) {
                expected.push_back(line);
            }
        }
    }
    BOOST_TEST_REQUIRE(expected.size() == 42u);
    for (bool early : {true, false}) {
        BOOST_TEST_CONTEXT("early_commit " << early) {
            auto [domain,problem] = load(dir + "/rtl_domain.hddl", dir + "/fsm/fsm_problem.hddl");
            domain.narration = nullptr;
            auto results = cppMCTShop(domain,problem,scorers["simple"],0,1,sqrt(2.0),2022,
                                      4,kDefaultMaxRolloutDepth,kDefaultMaxDecisions,0,2,early,true);
            //Plan steps carry the id of the task they came from, "(...)_17";
            //the committed file does not.
            std::vector<std::string> plan;
            for (auto const& step : results.t[results.end].plan) {
                plan.push_back(step.substr(0, step.rfind('_')));
            }
            BOOST_TEST(plan == expected, boost::test_tools::per_element());
        }
    }
}
