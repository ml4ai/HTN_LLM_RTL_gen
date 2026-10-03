#define BOOST_TEST_MODULE TestKB

#include "kb.h"
#include "typedefs.h"
#include <boost/test/included/unit_test.hpp>

using boost::unit_test::framework::master_test_suite;
using namespace std;
using namespace ast;
using namespace fol;

BOOST_AUTO_TEST_CASE(test_kb) {
    TypeTree typetree;
    std::string root = "__Object__";
    typetree.add_root(root);
    typetree.add_child("target",root);
    typetree.add_child("capacity_number",root);
    typetree.add_child("location",root);
    typetree.add_child("locatable",root);
    typetree.add_child("package","locatable");
    typetree.add_child("vehicle","locatable");

    Objects objects;
    objects["package_0"] = "package";
    objects["package_1"] = "package";
    objects["capacity_0"] = "capacity_number";
    objects["capacity_1"] = "capacity_number";
    objects["city_loc_0"] = "location";
    objects["city_loc_1"] = "location";
    objects["city_loc_2"] = "location";
    objects["truck_0"] = "vehicle";
    objects["surprise"] = "package";

    Predicates predicates;
    Args a1 = {std::make_pair("?arg0","location"),std::make_pair("?arg1","location")};
    Args a2 = {std::make_pair("?arg0","locatable"),std::make_pair("?arg1","location")};
    Args a3 = {std::make_pair("?arg0","package"),std::make_pair("?arg1","vehicle")};
    Args a4 = {std::make_pair("?arg0","vehicle"),std::make_pair("?arg1","capacity_number")};
    Args a5 = {std::make_pair("?arg0","capacity_number"),std::make_pair("?arg1","capacity_number")};
    predicates.push_back(create_predicate("road", a1));
    predicates.push_back(create_predicate("at", a2));
    predicates.push_back(create_predicate("in", a3));
    predicates.push_back(create_predicate("capacity", a4));
    predicates.push_back(create_predicate("capacity_predecessor", a5));

    KnowledgeBase kb(predicates,objects,typetree);

    BOOST_TEST(kb.tell("(capacity_predecessor capacity_0 capacity_1)",false,false));   

    BOOST_TEST(kb.tell("(capacity_predecessor capacity_0 capacity_1)",true,false));

    BOOST_TEST(kb.tell("(capacity_predecessor capacity_0 capacity_1)",false,false));
    BOOST_TEST(kb.tell("(road city_loc_0 city_loc_1)",false,false));
    BOOST_TEST(kb.tell("(road city_loc_1 city_loc_0)",false,false));
    BOOST_TEST(kb.tell("(road city_loc_1 city_loc_2)",false,false));
    BOOST_TEST(kb.tell("(road city_loc_2 city_loc_1)",false,false));
    BOOST_TEST(kb.tell("(at package_0 city_loc_1)",false,false));
    BOOST_TEST(kb.tell("(at package_1 city_loc_1)",false,false));
    BOOST_TEST(kb.tell("(at truck_0 city_loc_2)",false,false));
    BOOST_TEST(kb.tell("(capacity truck_0 capacity_1)",false,false));

    kb.update_state();

    kb.print_smt_state();

    std::string test_expr1 = "(and (at truck_0 city_loc_2) (at package_0 city_loc_1))";

    BOOST_TEST(kb.ask(test_expr1));

    std::string test_expr2 = "(and (at truck_0 city_loc_2) (or (at package_0 city_loc_1) (at package_0 city_loc_2)))";

    BOOST_TEST(kb.ask(test_expr2));

    std::string test_expr3 = "(and (at truck_0 city_loc_2) (at package_0 city_loc_2))";

    BOOST_TEST(!kb.ask(test_expr3));

    std::string test_expr4 = "(road ?c1 ?c2)";
    std::vector<std::pair<std::string,std::string>> args1 = {std::make_pair("?c1","location"),std::make_pair("?c2","location")};
    auto bindings1 = kb.ask(test_expr4,args1);
    BOOST_TEST(bindings1.size() == 4);
    for (auto const& b : bindings1) {
      for (auto const& vals : b) {
        std::cout << vals.first << " : " << vals.second << std::endl;
      }
    }

    std::vector<std::pair<std::string,std::string>> args2 = {std::make_pair("?p","package"),std::make_pair("?v","vehicle")};
    std::string test_expr5 = "(in ?p ?v)";
    auto bindings2 = kb.ask(test_expr5, args2);
    BOOST_TEST(bindings2.size() == 0);
    //for (auto const& b : test) {
    //  for (auto const& [var,val] : b) {
    //    std::cout << var << " : " << val << std::endl;
    //  }
    //}

}

//Names in SMT text (planner_doc.md 8.28). A variable is declared to Z3 under
//its '?' (expr::smt_var) and an object is written qualified
//(expr::smt_object), so neither can be taken for the other, or for a type or
//predicate of the same name. Here ?s0 is named like an object, ?module like a
//type and ?next like a predicate; the object owns is named like a predicate,
//the object state like a type, and the object s1 like a zero-arity predicate.
//Written bare, each pair was two declarations of one name, and Z3 refused the
//query. The bindings come back under the names the caller gave, whichever way
//it spelled them.
BOOST_AUTO_TEST_CASE(test_kb_names) {
    TypeTree typetree;
    typetree.add_root("__Object__");
    typetree.add_child("module","__Object__");
    typetree.add_child("state","__Object__");

    std::unordered_map<std::string,std::string> objects;
    objects["owns"] = "module";
    objects["s0"] = "state";
    objects["s1"] = "state";
    objects["state"] = "state";
    objects["x_0"] = "state";

    std::vector<std::pair<std::string,std::string>> a0 = {};
    std::vector<std::pair<std::string,std::string>> a1 = {{"?a","state"},{"?b","state"}};
    std::vector<std::pair<std::string,std::string>> a2 = {{"?m","module"},{"?s","state"}};
    std::vector<std::pair<std::string,std::vector<std::pair<std::string,std::string>>>> predicates;
    predicates.push_back(create_predicate("next", a1));
    predicates.push_back(create_predicate("owns", a2));
    predicates.push_back(create_predicate("s1", a0));

    KnowledgeBase kb(predicates,objects,typetree);
    BOOST_TEST(kb.tell("(next s0 s1)",false,false));
    BOOST_TEST(kb.tell("(next s1 state)",false,false));
    BOOST_TEST(kb.tell("(owns owns s0)",false,false));
    BOOST_TEST(kb.tell("(s1)",false,false));
    kb.update_state();

    std::vector<std::pair<std::string,std::string>> params =
        {{"s0","state"},{"module","module"},{"next","state"}};
    auto bindings = kb.ask("(and (next ?s0 ?next) (owns ?module ?s0))",params);
    BOOST_TEST_REQUIRE(bindings.size() == 1u);
    BOOST_TEST(bindings[0][0].first == "s0");
    BOOST_TEST(bindings[0][0].second == "s0");
    BOOST_TEST(bindings[0][1].first == "module");
    BOOST_TEST(bindings[0][1].second == "owns");
    BOOST_TEST(bindings[0][2].first == "next");
    BOOST_TEST(bindings[0][2].second == "s1");

    //The variable ?s0 and the object s0 in one formula: pinned to another
    //object, ?s0 is not s0.
    std::string const s0 = expr::smt_object("s0");
    std::string const s1 = expr::smt_object("s1");
    BOOST_TEST(kb.ask("(and (= ?s0 "+s1+") (= ?s0 "+s0+"))",params).empty());
    BOOST_TEST(kb.ask_any("(and (= ?s0 "+s0+") (next ?s0 "+s1+"))",params));
    BOOST_TEST(!kb.ask_any("(and (= ?s0 "+s1+") (next ?s0 "+s1+"))",params));

    //The object s1 and the proposition s1, and the object state: the value
    //comes back as the object's name, though Z3 prints it qualified.
    std::vector<std::pair<std::string,std::string>> next_only = {{"next","state"}};
    auto after_s1 = kb.ask("(and "+expr::smt_proposition("s1")+" (next "+s1+" ?next))",next_only);
    BOOST_TEST_REQUIRE(after_s1.size() == 1u);
    BOOST_TEST(after_s1[0][0].second == "state");

    //A predicate's definition quantifies over its positions, and used to call
    //them x_0, x_1, ... With an object x_0 in a fact, the binder captured it:
    //(next x_0 s1) was defined as (and (= x_0 x_0) (= x_1 s1)), so next held
    //of every state before s1, not of x_0 alone.
    KnowledgeBase kb2(predicates,objects,typetree);
    BOOST_TEST(kb2.tell("(next x_0 s1)",false,false));
    kb2.update_state();
    std::vector<std::pair<std::string,std::string>> one = {{"?a","state"}};
    auto before_s1 = kb2.ask("(next ?a "+s1+")",one);
    BOOST_TEST_REQUIRE(before_s1.size() == 1u);
    BOOST_TEST(before_s1[0][0].first == "?a");
    BOOST_TEST(before_s1[0][0].second == "x_0");
}
