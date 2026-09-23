#define BOOST_TEST_MODULE TestLoader

#include <boost/test/included/unit_test.hpp>
#include "cpphop/loader.h"

BOOST_AUTO_TEST_CASE(test_domain_loading) {
    // Test loading of domain definition and its components
    auto [transport_domain,transport_problem] = load("../../domains/transport_domain.hddl",
                                            "../../domains/transport_problem.hddl");

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
    auto [transport_domain,transport_problem] = load("../../domains/transport_domain.hddl",
                                            "../../domains/transport_problem.hddl");

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
    auto [transport_domain,transport_problem] = load("../../domains/transport_domain.hddl",
                                            "../../domains/transport_problem.hddl");
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
    auto domain = loadDomain("../../domains/transport_insert.hddl").first;

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
    auto fixture = loadDomain("../../domains/empty_method_test.hddl").first;
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
      "../../domains/transport_domain.hddl",
      "../../domains/simple_travel.hddl",
      "../../domains/sar3.hddl",
      "../../domains/d18.hddl",
      "../../domains/forall_test.hddl",
      "../../domains/atom_test.hddl",
      "../../domains/transport_original.hddl",
      "../../domains/transport_common.hddl",
      "../../domains/transport_mutex.hddl",
      "../../domains/transport_mutex_left.hddl",
      "../../domains/transport_insert.hddl",
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
