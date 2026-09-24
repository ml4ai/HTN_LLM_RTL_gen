#define BOOST_TEST_MODULE TestLoader

#include <boost/test/included/unit_test.hpp>
#include "cpphop/loader.h"
#include "test_paths.h"

BOOST_AUTO_TEST_CASE(test_domain_loading) {
    // Test loading of domain definition and its components
    auto [transport_domain,transport_problem] = load(HTN_DOMAINS_DIR "/transport_domain.hddl",
                                            HTN_DOMAINS_DIR "/transport_problem.hddl");

    BOOST_TEST(transport_domain.head == "domain");

    BOOST_TEST(transport_domain.typetree.types.size() == 8);
    for (auto const& [id, t] : transport_domain.typetree.types) {
      std::cout << t.type << "->["; 
      for (auto const& c : t.children) {
        std::cout << transport_domain.typetree.types[c].type << " "; 
      }
      std::cout << "]" << std::endl;
    }

    BOOST_TEST(transport_domain.predicates.size() == 5);
    BOOST_TEST(transport_domain.predicates[2].first == "in");
    BOOST_TEST(transport_domain.predicates[2].second[0].first == "arg0");
    BOOST_TEST(transport_domain.predicates[2].second[1].first == "arg1");
    BOOST_TEST(transport_domain.predicates[2].second[1].second == "vehicle");

    // Test methods and their components (totally-ordered):
    // Test methods name:
    BOOST_TEST(transport_domain.methods["deliver"][0].get_head() == "m_deliver_ordering_0");

    // Test Methods Parameters:
    BOOST_TEST(transport_domain.methods["deliver"][0].get_parameters()[2].second == "package");

    // Test method's task to be broken down. In the abstract task, 'task' is
    // defined similar to an action. Here, it is defined as <Literal<Term>>
    auto methodtask = transport_domain.methods["deliver"][0].get_task();
    BOOST_TEST(methodtask.first == "deliver");
    BOOST_TEST(methodtask.second[0].first == "p");
    // Under HDDL timing the method itself carries no state precondition: it is
    // compiled into a synthesised primitive action ordered before the method's
    // subtasks, so that it is evaluated when that action is scheduled rather
    // than when the method is decomposed. Only :constraints, which cannot
    // change with the state, are left on the method.
    auto methodprec_f = transport_domain.methods["deliver"][0].get_preconditions();
    BOOST_TEST(methodprec_f == "__NONE__");

    // The precondition now lives on the synthesised action ...
    BOOST_TEST(transport_domain.actions.contains("__mprec_m_deliver_ordering_0"));
    BOOST_TEST(transport_domain.actions.at("__mprec_m_deliver_ordering_0").get_preconditions()
               == "(and (at p l1))");
    BOOST_TEST(transport_domain.actions.at("__mprec_m_deliver_ordering_0").is_artificial());

    // ... which is a subtask of the method, ordered ahead of every other one.
    auto prec_subtask = transport_domain.methods["deliver"][0].get_subtasks()["__mprec__"];
    BOOST_TEST(prec_subtask.first == "__mprec_m_deliver_ordering_0");
    auto prec_ord = transport_domain.methods["deliver"][0].get_orderings()["__mprec__"];
    BOOST_TEST(prec_ord.size()
               == transport_domain.methods["deliver"][0].get_subtasks().size() - 1);

    // Test Parsing Method's SubTasks (in reverse order for planner):
    BOOST_TEST(transport_domain.methods["deliver"][0].get_subtasks()["task1"].first == "load");
    BOOST_TEST(transport_domain.methods["deliver"][0].get_subtasks()["task1"].second[1].first == "l1");

    // Ordering constraints
    auto og1 = transport_domain.methods["deliver"][0].get_orderings();
    BOOST_TEST(og1["task0"][0] == "task1");

    for (auto const &[t1,o] : og1) {
      std::cout << t1 << "->["; 
      for (auto const& t2 : o) {
        std::cout << t2 << " ";
      }
      std::cout << "]" << std::endl;
    }
    // Test parsing of domain actions and their components:
    // Test parsing action names
    auto actname1 = transport_domain.actions.at("drive").get_head();
    BOOST_TEST(actname1 == "drive");

    // Test parsing action parameters
    auto actpara1 = transport_domain.actions.at("drive").get_parameters();
    BOOST_TEST(actpara1[0].second == "vehicle");
    BOOST_TEST(actpara1[2].second == "location");
    BOOST_TEST(actpara1[0].first == "v");
    BOOST_TEST(actpara1[2].first == "l2");

    // Test parsing action precondition
    auto actprec = transport_domain.actions.at("drive").get_preconditions();
    BOOST_TEST(actprec == "(and (at v l1) (road l1 l2))");

    // Test parsing action effect
    
    auto effects = transport_domain.actions.at("drive").get_effects();
    BOOST_TEST(effects[0].pred.first == "at");
    BOOST_TEST(effects[0].pred.second[0].first == "v");
    BOOST_TEST(effects[0].remove == true);

    auto no_effects = transport_domain.actions.at("noop").get_effects();
    BOOST_TEST(no_effects.size() == 0);

}// end of testing the domain

BOOST_AUTO_TEST_CASE(test_problem_loading) {
    // Test loading of problem definition and its components
    auto [transport_domain,transport_problem] = load(HTN_DOMAINS_DIR "/transport_domain.hddl",
                                            HTN_DOMAINS_DIR "/transport_problem.hddl");

    BOOST_TEST(transport_problem.head == "__delivery__");
    BOOST_TEST(transport_problem.domain_name == "domain");
    
    BOOST_TEST(transport_problem.initF[7] == "(road city_loc_2 city_loc_0)");

    BOOST_TEST(transport_problem.objects.size() == 9);

    BOOST_TEST(transport_problem.objects["package_0"] == "package");

    BOOST_TEST(transport_problem.initM.get_head() == ":htn");

    BOOST_TEST(transport_problem.initM.get_subtasks()["task1"].first == "deliver");
    BOOST_TEST(transport_problem.initM.get_subtasks()["task1"].second[0].first == "package_1");
}

BOOST_AUTO_TEST_CASE(test_apply) {
    // Test loading of problem definition and its components
    auto [transport_domain,transport_problem] = load(HTN_DOMAINS_DIR "/transport_domain.hddl",
                                            HTN_DOMAINS_DIR "/transport_problem.hddl");
    KnowledgeBase kb(transport_domain.predicates,transport_problem.objects,transport_domain.typetree);
    std::cout << std::endl;
    std::cout << "#ONLY OBJECT FACTS#" << std::endl; 
    kb.print_facts();
    BOOST_TEST(kb.get_facts("vehicle").contains("(vehicle truck_0)"));
    auto actions = transport_domain.actions;
    //test unmet preconditions
    Args b = {std::make_pair("v","truck_0"),std::make_pair("l1","city_loc_2"),std::make_pair("l2","city_loc_1")};
    auto no_act = actions.at("drive").apply(kb,b);
    BOOST_TEST(no_act.second.empty());

    for (auto const& f : transport_problem.initF) {
      kb.tell(f,false,false);
    }
    kb.update_state();
    std::cout << std::endl;
    std::cout << "#INITIAL FACTS#" << std::endl;
    kb.print_facts();
    BOOST_TEST(kb.get_facts("road").contains("(road city_loc_2 city_loc_1)"));

    //test action apply
    auto drive_act = actions.at("drive").apply(kb,b);
    BOOST_TEST(drive_act.first == "(drive truck_0 city_loc_2 city_loc_1)");
    BOOST_TEST(drive_act.second.size() == 1);
    std::cout << std::endl;
    std::cout << "#FACTS AFTER DRIVE#" << std::endl;
    drive_act.second[0].print_facts();
    std::cout << std::endl;
    BOOST_TEST(drive_act.second[0].get_facts("at").contains("(at truck_0 city_loc_1)"));
    BOOST_TEST(!drive_act.second[0].get_facts("at").contains("(at truck_0 city_loc_2)"));

    //test method apply
    TaskGraph tg1; 
    Grounded_Task d;
    d.head = "deliver";
    d.args = {std::make_pair("p","package_0"),std::make_pair("l2","city_loc_2")};
    int i = tg1.add_node(d);
    auto deliver_method = transport_domain.methods["deliver"][0].apply(kb,d.args,tg1,i);
    std::cout <<"#GROUNDED TASKS FOR DELIVER#" << std::endl; 
    for (auto &gts : deliver_method) {
      for (auto const &[id,gt] : gts.second.GTs) {
        std::cout << gt.to_string() << "->["; 
        for (auto &out : gt.outgoing) {
          std::cout << gts.second[out].to_string() << " ";
        }
        std::cout << "]" << std::endl;
      }
      std::cout << std::endl;
    }
    TaskGraph tg2;
    Grounded_Task init_d;
    init_d.head = "__delivery__";
    init_d.args = {};
    int j = tg2.add_node(init_d);
    auto init_method = transport_problem.initM.apply(kb,init_d.args,tg2,j);
    std::cout <<"#GROUNDED TASKS FOR DELIVER#" << std::endl; 
    for (auto &gts : init_method) {
      for (auto const &[id,gt] : gts.second.GTs) {
        std::cout << gt.to_string() << "->["; 
        for (auto &out : gt.outgoing) {
          std::cout << gts.second[out].to_string() << " ";
        }
        std::cout << "]" << std::endl;
      }
      std::cout << std::endl;
    }

}

//`:subtasks ()` -- a method whose subtask network is empty -- is legal HDDL and
//is how the task-insertion encoding of 8.7 ends its recursion: free_drive
//decomposes into nothing and simply leaves the task network. It is worth its
//own assertion because the loader's ordered-subtask branch indexes sts[0], so
//an empty network used to read past the end of a vector, and because no
//shipped domain had one until transport_insert.hddl.
BOOST_AUTO_TEST_CASE(test_empty_subtask_network) {
    auto domain = loadDomain(HTN_DOMAINS_DIR "/transport_insert.hddl").first;

    auto& free_drive = domain.methods["free_drive"];
    BOOST_TEST(free_drive.size() == 2);

    int empty = 0, recursive = 0;
    for (auto& m : free_drive) {
        if (m.get_subtasks().empty()) {
            empty++;
            BOOST_TEST(m.get_orderings().empty());
            //An empty network carries no state precondition either, so nothing
            //was synthesised for it.
            BOOST_TEST(m.get_preconditions() == "__NONE__");
        }
        else {
            recursive++;
        }
    }
    BOOST_TEST(empty == 1);
    BOOST_TEST(recursive == 1);

    //And get_to keeps only the method that does nothing: in this encoding a
    //get_to asserts a condition rather than producing drives.
    BOOST_TEST(domain.methods["get_to"].size() == 1);

    //Both HDDL spellings of an empty network, including the one no shipped
    //domain uses. `:ordered-subtasks ()` reached the loader's ordered branch,
    //which opens by indexing sts[0] -- reading past the end of an empty
    //vector. Before the guard this exited 139 (SIGSEGV).
    auto fixture = loadDomain(HTN_DOMAINS_DIR "/empty_method_test.hddl").first;
    for (auto const& task : {"settle", "settle_ordered"}) {
        int empty = 0;
        for (auto& m : fixture.methods[task]) {
            if (m.get_subtasks().empty()) {
                empty++;
                BOOST_TEST(m.get_orderings().empty());
            }
        }
        BOOST_TEST_CONTEXT(task) {
            BOOST_TEST(empty == 1);
        }
    }
}

//The loader keeps a structured form of every precondition and effect condition
//alongside the SMT string it hands to Z3 (lib/expr.h). Nothing evaluates it
//yet -- the direct evaluator is the next step -- so the only thing worth
//asserting now is that it is faithful: rendering the structure back must
//reproduce the string stored beside it, exactly, for every action, method and
//conditional effect in every shipped domain. If those two ever disagree, the
//structure is not describing what the planner actually evaluates.
BOOST_AUTO_TEST_CASE(test_expr_ast_matches_smt) {
    std::vector<std::string> domains = {
      HTN_DOMAINS_DIR "/transport_domain.hddl",
      HTN_DOMAINS_DIR "/simple_travel.hddl",
      HTN_DOMAINS_DIR "/sar3.hddl",
      HTN_DOMAINS_DIR "/d18.hddl",
      HTN_DOMAINS_DIR "/forall_test.hddl",
      HTN_DOMAINS_DIR "/atom_test.hddl",
      HTN_DOMAINS_DIR "/transport_original.hddl",
      HTN_DOMAINS_DIR "/transport_common.hddl",
      HTN_DOMAINS_DIR "/transport_mutex.hddl",
      HTN_DOMAINS_DIR "/transport_mutex_left.hddl",
      HTN_DOMAINS_DIR "/transport_insert.hddl",
    };

    int checked = 0;
    for (auto const& d : domains) {
        auto domain = loadDomain(d).first;

        for (auto& [name,action] : domain.actions) {
            BOOST_TEST_CONTEXT(d << " action " << name) {
                BOOST_TEST(expr::to_smt(action.get_precondition_ast())
                           == action.get_preconditions());
            }
            checked++;
            for (auto const& e : action.get_effects()) {
                BOOST_TEST_CONTEXT(d << " effect of " << name) {
                    BOOST_TEST(expr::to_smt(e.condition_ast) == e.condition);
                }
                checked++;
            }
        }

        for (auto& [task,methods] : domain.methods) {
            for (auto& m : methods) {
                BOOST_TEST_CONTEXT(d << " method " << m.get_head()) {
                    BOOST_TEST(expr::to_smt(m.get_precondition_ast())
                               == m.get_preconditions());
                }
                checked++;
            }
        }
    }
    //Guard against the loop silently checking nothing.
    BOOST_TEST(checked > 100);
    std::cout << "round-tripped " << checked << " preconditions and conditions"
              << std::endl;

}// end of testing the expression IR


//planner_doc.md 9.1: the loader validates a domain and problem before building
//anything. Each case below is a one-line corruption of transport, of the kind a
//language model makes writing HDDL. Before validation the first four crashed
//the planner with no message and the rest ran with every rollout failing, or
//were silently accepted. Each must now be rejected with a message that names
//the file and line, the construct, and what is wrong.
namespace {
std::string slurp(std::string const& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)),
                       (std::istreambuf_iterator<char>()));
}

std::string const transport_dom = slurp(HTN_DOMAINS_DIR "/transport_domain.hddl");
std::string const transport_prob = slurp(HTN_DOMAINS_DIR "/transport_problem.hddl");

std::string replaced(std::string text, std::string const& from, std::string const& to) {
    auto at = text.find(from);
    BOOST_REQUIRE_MESSAGE(at != std::string::npos, "fixture text not found: " << from);
    return text.replace(at, from.size(), to);
}

//The problems validation reports, or none if the pair loads.
std::vector<std::string> problems_of(std::string const& dom, std::string const& prob) {
    try {
        load_hddl(dom, prob);
    }
    catch (HDDLError const& e) {
        return e.problems();
    }
    return {};
}

void expect_one(std::string const& dom, std::string const& prob, std::string const& expected) {
    auto ps = problems_of(dom, prob);
    BOOST_TEST_CONTEXT(expected) {
        for (auto const& p : ps) {
            BOOST_TEST_INFO("reported: " << p);
        }
        BOOST_TEST_REQUIRE(ps.size() == 1u);
        BOOST_TEST_INFO("reported: " << ps[0]);
        BOOST_TEST(ps[0].find(expected) != std::string::npos);
    }
}
}

BOOST_AUTO_TEST_CASE(test_validation_rejects_the_ten_mistakes) {
    auto const& D = transport_dom;
    auto const& P = transport_prob;
    //Used to segfault.
    expect_one(replaced(D, "(task0 (get_to ?v ?l1))", "(task0 (get_too ?v ?l1))"), P,
               "domain:42: in method m_deliver_ordering_0: subtask task0, (get_too ?v ?l1): "
               "get_too is not a declared task or action (did you mean get_to?)");
    expect_one(replaced(D, ":task (unload ?v ?l ?p)", ":task (unlaod ?v ?l ?p)"), P,
               "unlaod is not a declared task (did you mean unload?)");
    expect_one(replaced(D, "(not (at ?v ?l1))\n", "(not (att ?v ?l1))\n"), P,
               "in action drive: effect (att ?v ?l1): predicate att is not declared in :predicates");
    expect_one(D, replaced(P, "(task0 (deliver package_0", "(task0 (delivr package_0"),
               "problem:18: in :htn: subtask task0, (delivr package_0 city_loc_0): "
               "delivr is not a declared task or action (did you mean deliver?)");
    //Used to run with every rollout failing and no reason given.
    expect_one(replaced(D, "(road ?l1 ?l2)\n                        (not", "(raod ?l1 ?l2)\n                        (not"), P,
               "predicate raod is not declared in :predicates (did you mean road?)");
    expect_one(replaced(D, "(capacity ?v ?s2)\n", "(capacity ?v)\n"), P,
               "condition (capacity ?v) gives predicate capacity 1 argument, but it is declared with 2");
    expect_one(D, replaced(P, "(at truck_0 city_loc_2)", "(at truck_1 city_loc_2)"),
               "in :init: fact (at truck_1 city_loc_2) uses truck_1, which is not a declared "
               "object or constant (did you mean truck_0?)");
    //Used to be accepted silently.
    expect_one(replaced(D, "(?v - vehicle ?l1 - location ?l2 - location)", "(?v - vehicle ?l1 - locaton ?l2 - location)"), P,
               "parameter ?l1 has type locaton, which is not declared in :types (did you mean location?)");
    expect_one(replaced(D, "(at ?p ?l1)\n", "(at ?p ?l9)\n"), P,
               "uses ?l9, which is not a parameter here or bound by a quantifier (did you mean ?l1?)");

    //The tenth is a parse error, reported by the grammar rather than by
    //validation. It used to go to stderr, with the exception saying only
    //"Parsing error!"; the report is now the exception's message.
    try {
        load_hddl(replaced(D, "(and\n\t\t\t\t(not (at ?v ?l1))", "(and\n\t\t\t\t(not (at ?v ?l1)"), P);
        BOOST_FAIL("an unbalanced parenthesis was accepted");
    }
    catch (ParseError const& e) {
        std::string what = e.what();
        BOOST_TEST_INFO(what);
        BOOST_TEST(what.find("In file domain, line 135") != std::string::npos);
        BOOST_TEST(what.find("Expecting: ')'") != std::string::npos);
    }
    //A missing parenthesis the grammar notices only at an anonymous
    //sub-parser. It used to be named by that parser's mangled C++ type.
    try {
        load_hddl(replaced(D, "(:task get_to\n\t\t:parameters (?v - vehicle ?l - location)\n\t)",
                              "(:task get_to\n\t\t:parameters (?v - vehicle ?l - location)\n\t"), P);
        BOOST_FAIL("a task missing its closing parenthesis was accepted");
    }
    catch (ParseError const& e) {
        std::string what = e.what();
        BOOST_TEST_INFO(what);
        BOOST_TEST(what.find("N5boost") == std::string::npos);
        BOOST_TEST(what.find("a parenthesis is probably missing or extra") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(test_validation_further_checks) {
    auto const& D = transport_dom;
    auto const& P = transport_prob;
    //Swapped arguments: types with nothing in common but `object`. Both
    //positions are wrong, and both are reported.
    auto swapped = problems_of(replaced(D, "(task0 (get_to ?v ?l1))", "(task0 (get_to ?l1 ?v))"), P);
    BOOST_TEST_REQUIRE(swapped.size() == 2u);
    BOOST_TEST(swapped[0].find("argument 1 (?l1) has type location, but task get_to expects vehicle there") != std::string::npos);
    BOOST_TEST(swapped[1].find("argument 2 (?v) has type vehicle, but task get_to expects location there") != std::string::npos);
    expect_one(D, replaced(P, "(capacity truck_0 capacity_2)", "(capacity capacity_2 capacity_2)"),
               "argument 1 (capacity_2) has type capacity_number, but predicate capacity expects vehicle there");
    //Orderings.
    expect_one(replaced(D, "(< task0 task1)", "(< task0 task9)"), P,
               "ordering (< task0 task9) names task9, which is not one of this network's subtask ids");
    expect_one(replaced(D, "(< task2 task3)", "(< task2 task3)\n\t\t\t(< task3 task0)"), P,
               "the :ordering constraints form a cycle");
    //Declarations.
    expect_one(replaced(D, "(:action noop", "(:action drive\n\t\t:parameters ()\n\t)\n\t(:action noop"), P,
               "action drive is declared twice");
    expect_one(replaced(D, ":task (get_to ?v ?l)\n", ":task (drive ?v ?l ?l)\n"), P,
               "names action drive; a method decomposes a compound task");
    expect_one(D, replaced(P, "(:domain  domain)", "(:domain  transport)"),
               "the problem is for domain transport, but the domain loaded is domain");

    //Every problem is reported, not just the first, and in source order.
    auto both = problems_of(replaced(replaced(D, "(at ?p ?l1)\n", "(at ?p ?l9)\n"),
                                     "(task0 (get_to ?v ?l1))", "(task0 (get_too ?v ?l1))"),
                            replaced(P, "(at truck_0 city_loc_2)", "(at truck_1 city_loc_2)"));
    BOOST_TEST_REQUIRE(both.size() == 3u);
    BOOST_TEST(both[0].rfind("domain:35:", 0) == 0);
    BOOST_TEST(both[1].rfind("domain:42:", 0) == 0);
    BOOST_TEST(both[2].rfind("problem: in :init:", 0) == 0);

    //Not problems. Every type is also a one-argument predicate true of that
    //type's objects (KnowledgeBase::initialize), so a condition may use one.
    BOOST_TEST(problems_of(replaced(D, "(at ?p ?l1)\n", "(at ?p ?l1)\n(vehicle ?v)\n"), P).empty());
    //A supertype variable passed to a subtype parameter narrows at grounding.
    BOOST_TEST(problems_of(replaced(D, "?p - package ?v - vehicle)\n\t\t:task (deliver",
                                       "?p - locatable ?v - vehicle)\n\t\t:task (deliver"), P).empty());
}

//Every shipped domain and problem pairing validates. A new check that rejects
//one of these is wrong, or has found a real mistake in it.
BOOST_AUTO_TEST_CASE(test_shipped_domains_validate) {
    std::vector<std::pair<std::string,std::string>> pairs = {
      {"transport_domain", "transport_problem"},
      {"simple_travel", "simple_travel_problem"},
      {"sar3", "sar3p1"},
      {"d18", "p18"},
      {"d18", "problem_gather_wake_evacuate"},
      {"forall_test", "forall_test_problem"},
      {"atom_test", "atom_test_problem"},
    };
    for (auto const& d : {"transport_original", "transport_common", "transport_mutex",
                          "transport_mutex_left", "transport_insert"}) {
        for (auto const& p : {"a", "b", "c", "d"}) {
            pairs.push_back({d, std::string("transport_chain_") + p});
        }
    }
    for (auto const& p : {"a", "b", "c", "d"}) {
        pairs.push_back({"transport_insert", std::string("transport_chain_") + p + "_insert"});
    }
    for (auto const& [d,p] : pairs) {
        BOOST_TEST_CONTEXT(d << " + " << p) {
            auto ps = problems_of(slurp(HTN_DOMAINS_DIR "/" + d + ".hddl"),
                                  slurp(HTN_DOMAINS_DIR "/" + p + ".hddl"));
            for (auto const& x : ps) {
                BOOST_TEST_INFO(x);
            }
            BOOST_TEST(ps.empty());
        }
    }
    BOOST_TEST(loadDomain(HTN_DOMAINS_DIR "/empty_method_test.hddl").first.head == "empty_method_test");
}
