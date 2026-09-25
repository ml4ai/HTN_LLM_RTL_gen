#pragma once

#include <fstream>
#include <iostream>
#include <string>
#include <queue>
#include <map>
#include <algorithm>
#include "parsing/api.hpp"
#include "parsing/ast.hpp"
#include "parsing/ast_adapted.hpp"
#include "parsing/parse.hpp"
#include "util.h"
#include "fol/util.h"
#include <boost/optional.hpp>
#include <boost/spirit/home/x3/support/ast/variant.hpp>
#include "kb.h"
#include "typedefs.h"
#include "validate.h"
#include "case.h"
#include <filesystem>

//The loader's workings live in this namespace, where the parser's AST names
//can be used unqualified. They used to be pulled into the global namespace of
//every file including this header, where ast::Type collided with the type
//tree's ::Type (planner_doc.md 8.20). What a caller uses is exported below.
namespace hddl_loader {

namespace fs = std::filesystem;
namespace x3 = boost::spirit::x3;
using namespace ast;
using boost::get;
using std::string, std::vector, std::unordered_set;
using Ptypes = std::unordered_map<std::string,std::vector<std::string>>;
using Tasktypes = std::unordered_map<std::string,std::vector<std::string>>;

//The declared type of argument i of a predicate or task, or __Object__ when
//there is none on record. operator[] used to insert an empty list and index
//past its end for a name with no entry: a type used as a one-argument
//predicate (legal since planner_doc.md 8.17) has none.
inline std::string const& type_at(std::unordered_map<std::string,std::vector<std::string>> const& types,
                                  std::string const& name, size_t i) {
  static std::string const object = "__Object__";
  auto it = types.find(name);
  if (it == types.end() || i >= it->second.size()) {
    return object;
  }
  return it->second[i];
}

//A term's name: a variable's without its '?', or a constant's.
inline std::string const& term_name(Term const& t) {
  return t.which() == 1 ? boost::get<Variable>(t).name : boost::get<Constant>(t).name;
}

//Every (either ...) type used in a parameter or quantifier list, collected so
//each can be added to the type tree before anything refers to it.
using EitherTypes = std::map<std::string,std::vector<std::string>>;

inline void either_types_in(ast::TypedList<ast::Variable> const& tl, EitherTypes& out) {
  for (auto const& e : tl.explicitly_typed_lists) {
    if (e.type.get().which() == 1) {
      auto const& members = boost::get<EitherType>(e.type);
      out[type_name_of(e.type)] = std::vector<std::string>(members.begin(),members.end());
    }
  }
}

inline void either_types_in(ast::Sentence const& s, EitherTypes& out) {
  switch (s.which()) {
    case 2:
      for (auto const& c : boost::get<ConnectedSentence>(s).sentences) {
        either_types_in(c,out);
      }
      break;
    case 3:
      either_types_in(boost::get<NotSentence>(s).sentence,out);
      break;
    case 4:
      either_types_in(boost::get<ImplySentence>(s).sentence1,out);
      either_types_in(boost::get<ImplySentence>(s).sentence2,out);
      break;
    case 5:
      either_types_in(boost::get<QuantifiedSentence>(s).variables,out);
      either_types_in(boost::get<QuantifiedSentence>(s).sentence,out);
      break;
  }
}

inline void either_types_in(ast::Effect const& e, EitherTypes& out);

inline void either_types_in(ast::CEffect const& c, EitherTypes& out) {
  if (c.which() == 0) {
    either_types_in(boost::get<ForallCEffect>(c).variables,out);
    either_types_in(boost::get<ForallCEffect>(c).effect,out);
  }
  else if (c.which() == 1) {
    either_types_in(boost::get<WhenCEffect>(c).gd,out);
  }
}

inline void either_types_in(ast::Effect const& e, EitherTypes& out) {
  if (e.which() == 1) {
    for (auto const& c : boost::get<AndCEffect>(e).c_effects) {
      either_types_in(c,out);
    }
  }
  else if (e.which() == 2) {
    either_types_in(boost::get<CEffect>(e),out);
  }
}

inline EitherTypes either_types_in(ast::Domain const& d) {
  EitherTypes out;
  for (auto const& p : d.predicates) either_types_in(p.variables,out);
  for (auto const& t : d.tasks) either_types_in(t.parameters,out);
  for (auto const& a : d.actions) {
    either_types_in(a.parameters,out);
    either_types_in(a.precondition,out);
    either_types_in(a.effect,out);
  }
  for (auto const& m : d.methods) {
    either_types_in(m.parameters,out);
    either_types_in(m.precondition,out);
  }
  return out;
}

inline EitherTypes either_types_in(ast::Problem const& p) {
  EitherTypes out;
  either_types_in(p.problem_htn.parameters,out);
  either_types_in(p.goal,out);
  return out;
}

inline void add_either_types(TypeTree& typetree, EitherTypes const& eithers) {
  for (auto const& [name,members] : eithers) {
    typetree.add_union(name,members,"__Object__");
  }
}

inline void get_orderings(Orderings const& orderings,std::unordered_map<std::string,std::vector<std::string>>& og) {
  if (which_orderings(orderings) == 0) {
    return;
  }
  if (which_orderings(orderings) == 1) {
    auto o = boost::get<Ordering>(orderings);
    og[o.first].push_back(o.second);
    return;
  }
  if (which_orderings(orderings) == 2) {
    auto ov = boost::get<std::vector<Ordering>>(orderings);
    for (auto const &o : ov) {
      og[o.first].push_back(o.second);
    }
    return;
  }
  return;
};

inline std::unordered_set<std::string> type_inference(Sentence const& sentence, Ptypes const& ptypes, std::string const& var) {
  std::unordered_set<std::string> types;
  types.insert("__Object__");
  if (sentence.which() == 0) {
    return types; 
  }
  if (sentence.which() == 1) {
    auto s = boost::get<Literal<Term>>(sentence);
    for (size_t i = 0; i < s.args.size(); i++) {
      if (s.args[i].which() == 1 && boost::get<Variable>(s.args[i]).name == var) {
        types.insert(type_at(ptypes,s.predicate,i));
      }
    }
    return types; 
  }
  if (sentence.which() == 2) {
    auto s = boost::get<ConnectedSentence>(sentence);
    for (auto const& t : s.sentences) {
      types.merge(type_inference(t,ptypes,var));
    }
    return types;
  }
  if (sentence.which() == 3) {
    auto s = boost::get<NotSentence>(sentence);
    types.merge(type_inference(s.sentence,ptypes,var));
    return types;
  }
  if (sentence.which() == 4) {
    auto s = boost::get<ImplySentence>(sentence);
    types.merge(type_inference(s.sentence1,ptypes,var));
    types.merge(type_inference(s.sentence2,ptypes,var));
    return types;
  }
  if (sentence.which() == 5) {
    auto s = boost::get<QuantifiedSentence>(sentence);
    //checks to see if new local scope is created for var
    for (auto const& t : s.variables.explicitly_typed_lists) {
      for (auto const& e : t.entries) {
        if (var == e.name) {
          return types;
        }
      }
    }
    for (auto const& it : s.variables.implicitly_typed_list) {
      if (var == it.name) {
        return types;
      }
    }

    types.merge(type_inference(s.sentence,ptypes,var));
    return types; 
  }
  return types;
}

inline std::string sentence_to_SMT(Sentence const& sentence, Ptypes const& ptypes) {
  if (sentence.which() == 0) {
    return "__NONE__";
  }
  if (sentence.which() == 1) {
    auto s = boost::get<Literal<Term>>(sentence);
    //A zero-arity predicate is a propositional atom, declared as a plain Bool
    //constant by KnowledgeBase::update_state. SMT-LIB has no nullary
    //application, so it has to be referenced by bare name: "(done)" is
    //rejected as a function application with its arguments missing.
    if (s.args.empty()) {
      return s.predicate;
    }
    std::string lt = "("+s.predicate;
    for (auto const& a : s.args) {
      if (a.which() == 0) {
        lt += " "+boost::get<Constant>(a).name;
      }
      else {
        lt += " "+boost::get<Variable>(a).name;
      }
    }
    return lt + ")";
  }
  if (sentence.which() == 2) {
    auto s = boost::get<ConnectedSentence>(sentence);
    std::string cs = "("+s.connector;
    bool all_none = true;
    for (auto const& t : s.sentences) {
      std::string str = sentence_to_SMT(t,ptypes);
      if (str != "__NONE__") {
        cs += " "+str;
        all_none = false;
      }
    }
    if (all_none) {
      return "__NONE__";
    }
    return cs + ")";
  }
  if (sentence.which() == 3) {
    auto s = boost::get<NotSentence>(sentence);
    std::string ns = "(not";
    std::string str = sentence_to_SMT(s.sentence,ptypes);
    if (str != "__NONE__") {
      ns += " "+str;
      return ns + ")";
    }
    return "__NONE__";
  }
  if (sentence.which() == 4) {
    auto s = boost::get<ImplySentence>(sentence);
    std::string is = "(=>";
    std::string str1 = sentence_to_SMT(s.sentence1,ptypes);
    std::string str2 = sentence_to_SMT(s.sentence2,ptypes);
    if (str1 != "__NONE__" && str2 != "__NONE__") {
      is += " "+str1;
      is += " "+str2;
      return is + ")";
    }
    return "__NONE__";
  }
  if (sentence.which() == 5) {
    auto s = boost::get<QuantifiedSentence>(sentence);
    std::string qs = "("+s.quantifier + " (";
    std::string t_imply = "(=> (and";
    if (s.variables.explicitly_typed_lists.empty() && s.variables.implicitly_typed_list.empty()) {
      return "__NONE__";
    }
    for (auto const& t : s.variables.explicitly_typed_lists) {
      std::string type = type_name_of(t.type);
      for (auto const& e : t.entries) {
        qs += "("+e.name+" __Object__) ";
        t_imply += " ("+type+" "+e.name+")";
      }
    }
    for (auto const& it : s.variables.implicitly_typed_list) {
      qs += "("+it.name+" __Object__) ";
      auto inferred_types = type_inference(s.sentence,ptypes,it.name);
      for (auto const& t : inferred_types) {
        t_imply += " ("+t+" "+it.name+")";
      }
    }
    qs += ") ";
    t_imply += ")";
    std::string str = sentence_to_SMT(s.sentence,ptypes);
    if (str != "__NONE__") {
      qs += t_imply+" "+str;
      return qs + "))";
    }
    return "__NONE__";
  }
  if (sentence.which() == 6) {
    auto s = boost::get<EqualsSentence>(sentence);
    std::string l;
    std::string r;
    if (s.lhs.which() == 0) {
      l = boost::get<Constant>(s.lhs).name;
    }
    else {
      l = boost::get<Variable>(s.lhs).name;
    }
    if (s.rhs.which() == 0) {
      r = boost::get<Constant>(s.rhs).name;
    }
    else {
      r = boost::get<Variable>(s.rhs).name;
    }
    return "(= "+l+" "+r+")";
  }
  if (sentence.which() == 7) {
    auto s = boost::get<NotEqualsSentence>(sentence);
    std::string l;
    std::string r;
    if (s.lhs.which() == 0) {
      l = boost::get<Constant>(s.lhs).name;
    }
    else {
      l = boost::get<Variable>(s.lhs).name;
    }
    if (s.rhs.which() == 0) {
      r = boost::get<Constant>(s.rhs).name;
    }
    else {
      r = boost::get<Variable>(s.rhs).name;
    }
    return "(not (= "+l+" "+r+"))";
  }
  return "__NONE__";
}

//Mirrors sentence_to_SMT above, building the structured form instead of the
//string. The two are kept in step by a round-trip assertion in test_loader:
//every precondition and effect condition in every shipped domain must render
//back to the string stored beside it.
inline expr::Ptr sentence_to_expr(Sentence const& sentence, Ptypes const& ptypes) {
  if (sentence.which() == 0) {
    return nullptr;
  }
  if (sentence.which() == 1) {
    auto s = boost::get<Literal<Term>>(sentence);
    std::vector<expr::Term> args;
    for (auto const& a : s.args) {
      if (a.which() == 0) {
        args.push_back({boost::get<Constant>(a).name,false});
      }
      else {
        args.push_back({boost::get<Variable>(a).name,true});
      }
    }
    return expr::make_atom(s.predicate,std::move(args));
  }
  if (sentence.which() == 2) {
    auto s = boost::get<ConnectedSentence>(sentence);
    std::vector<expr::Ptr> children;
    for (auto const& t : s.sentences) {
      auto c = sentence_to_expr(t,ptypes);
      if (c) {
        children.push_back(c);
      }
    }
    if (children.empty()) {
      return nullptr;
    }
    return expr::make_connective(s.connector == "or" ? expr::Kind::Or : expr::Kind::And,
                                 std::move(children));
  }
  if (sentence.which() == 3) {
    auto s = boost::get<NotSentence>(sentence);
    auto c = sentence_to_expr(s.sentence,ptypes);
    if (!c) {
      return nullptr;
    }
    return expr::make_connective(expr::Kind::Not,{c});
  }
  if (sentence.which() == 4) {
    auto s = boost::get<ImplySentence>(sentence);
    auto a = sentence_to_expr(s.sentence1,ptypes);
    auto b = sentence_to_expr(s.sentence2,ptypes);
    if (!a || !b) {
      return nullptr;
    }
    return expr::make_connective(expr::Kind::Imply,{a,b});
  }
  if (sentence.which() == 5) {
    auto s = boost::get<QuantifiedSentence>(sentence);
    if (s.variables.explicitly_typed_lists.empty() && s.variables.implicitly_typed_list.empty()) {
      return nullptr;
    }
    std::vector<expr::Bound> bound;
    for (auto const& t : s.variables.explicitly_typed_lists) {
      std::string type = type_name_of(t.type);
      for (auto const& e : t.entries) {
        bound.push_back({e.name,{type}});
      }
    }
    for (auto const& it : s.variables.implicitly_typed_list) {
      expr::Bound b;
      b.name = it.name;
      //Same call sentence_to_SMT makes, so the type order matches.
      for (auto const& t : type_inference(s.sentence,ptypes,it.name)) {
        b.types.push_back(t);
      }
      bound.push_back(std::move(b));
    }
    auto body = sentence_to_expr(s.sentence,ptypes);
    if (!body) {
      return nullptr;
    }
    return expr::make_quantified(s.quantifier,std::move(bound),body);
  }
  if (sentence.which() == 6 || sentence.which() == 7) {
    bool equals = sentence.which() == 6;
    Term lhs_t, rhs_t;
    if (equals) {
      auto s = boost::get<EqualsSentence>(sentence);
      lhs_t = s.lhs;
      rhs_t = s.rhs;
    }
    else {
      auto s = boost::get<NotEqualsSentence>(sentence);
      lhs_t = s.lhs;
      rhs_t = s.rhs;
    }
    auto conv = [](Term const& t) -> expr::Term {
      if (t.which() == 0) {
        return {boost::get<Constant>(t).name,false};
      }
      return {boost::get<Variable>(t).name,true};
    };
    return expr::make_compare(equals,conv(lhs_t),conv(rhs_t));
  }
  return nullptr;
}

inline std::unordered_set<std::string> type_inference(effect const& e, Ptypes const& ptypes, std::string const& var) {
  std::unordered_set<std::string> types = {"__Object__"};
  auto pred = e.pred;
  for (size_t i = 0; i < pred.second.size(); i++) {
    if (var == pred.second[i].first) {
      types.insert(type_at(ptypes,pred.first,i));
    }
  }
  return types;
}
//An effect's atom as the planner stores it: the predicate, and each argument
//with the declared type of its position.
inline Pred pred_of(PEffect const& p, Ptypes const& ptypes) {
  Pred pd;
  pd.first = p.predicate;
  for (size_t i = 0; i < p.args.size(); i++) {
    pd.second.emplace_back(term_name(p.args[i]),type_at(ptypes,p.predicate,i));
  }
  return pd;
}

//Forward declaration needed for decompose_effects
inline Effects decompose_ceffects(CEffect const& ceffect, Ptypes const& ptypes);

inline Effects decompose_effects(Effect const& effect, Ptypes const& ptypes) {
  Effects effects = {};
  if (effect.which() == 0) {
    return effects;
  }
  if (effect.which() == 1) {
    auto e = boost::get<AndCEffect>(effect);
    for (auto const& c : e.c_effects) {
      auto more = decompose_ceffects(c,ptypes);
      effects.insert(effects.end(),more.begin(),more.end());
    }
    return effects;
  }
  if (effect.which() == 2) {
    auto e = boost::get<CEffect>(effect);
    effects = decompose_ceffects(e,ptypes);
    return effects;
  }
  return effects;
}

inline Effects decompose_ceffects(CEffect const& ceffect, Ptypes const& ptypes) {
  Effects effects = {};
  if (ceffect.which() == 0) {
    auto e = boost::get<ForallCEffect>(ceffect);
    auto faeffects = decompose_effects(e.effect,ptypes);
    for (size_t i = 0; i < faeffects.size(); i++) {
      //An inner forall binding the same name has already set it.
      for (auto const& t : e.variables.explicitly_typed_lists) {
        std::string type = type_name_of(t.type);
        for (auto const& v : t.entries) {
          if (!faeffects[i].forall.contains(v.name)) {
            faeffects[i].forall[v.name] = {"__Object__",type};
          }
        }
      }
      //Untyped: the types of the predicate positions the variable fills.
      for (auto const& v : e.variables.implicitly_typed_list) {
        if (!faeffects[i].forall.contains(v.name)) {
          faeffects[i].forall[v.name] = type_inference(faeffects[i],ptypes,v.name);
        }
      }
    }
    return faeffects;
  }
  if (ceffect.which() == 1) {
    auto e = boost::get<WhenCEffect>(ceffect);
    if (e.cond_effect.which() == 0) {
      auto p = boost::get<PEffect>(e.cond_effect);
      auto sentSMT = sentence_to_SMT(e.gd, ptypes);
      auto sentAST = sentence_to_expr(e.gd, ptypes);
      effects.push_back(effect(sentSMT,p.is_negative,pred_of(p,ptypes),{},sentAST));
    }
    else {
      auto vp = boost::get<std::vector<PEffect>>(e.cond_effect);
      auto sentSMT = sentence_to_SMT(e.gd, ptypes);
      auto sentAST = sentence_to_expr(e.gd, ptypes);
      for (auto const& p : vp) {
        effects.push_back(effect(sentSMT,p.is_negative,pred_of(p,ptypes),{},sentAST));
      }
    }
    return effects;
  }
  if (ceffect.which() == 2) {
    auto p = boost::get<PEffect>(ceffect);
    effects.push_back(effect("__NONE__",p.is_negative,pred_of(p,ptypes),{},nullptr));
    return effects;
  }
  return effects;
}



inline std::string decompose_constraint(Constraint const& constraint) {
  if (which_constraint(constraint) == 0) {
    return "__NONE__";
  }
  if (which_constraint(constraint) == 1) {
    auto s = boost::get<EqualsSentence>(constraint);
    std::string l;
    std::string r;
    if (s.lhs.which() == 0) {
      l = boost::get<Constant>(s.lhs).name;
    }
    else {
      l = boost::get<Variable>(s.lhs).name;
    }
    if (s.rhs.which() == 0) {
      r = boost::get<Constant>(s.rhs).name;
    }
    else {
      r = boost::get<Variable>(s.rhs).name;
    }
    return "(= "+l+" "+r+")";
  }
  if (which_constraint(constraint) == 2) {
    auto s = boost::get<NotEqualsSentence>(constraint);
    std::string l;
    std::string r;
    if (s.lhs.which() == 0) {
      l = boost::get<Constant>(s.lhs).name;
    }
    else {
      l = boost::get<Variable>(s.lhs).name;
    }
    if (s.rhs.which() == 0) {
      r = boost::get<Constant>(s.rhs).name;
    }
    else {
      r = boost::get<Variable>(s.rhs).name;
    }
    return "(not (= "+l+" "+r+"))";
  }
  return "__NONE__";
}

inline std::string decompose_constraints(Constraints const& constraints) {
  std::string cs = "(and"; 
  if (which_constraints(constraints) == 0) {
    return "__NONE__";
  }
  if (which_constraints(constraints) == 1) {
    auto c = boost::get<Constraint>(constraints);
    std::string str = decompose_constraint(c);
    if (str != "__NONE__") {
      return cs+" "+decompose_constraint(c)+")";
    }
    return "__NONE__";
  }
  if (which_constraints(constraints) == 2) {
    auto cons = boost::get<std::vector<Constraint>>(constraints);
    bool all_none = true;
    for (auto const& c : cons) {
      std::string str = decompose_constraint(c);
      if (str != "__NONE__") {
        cs += " "+decompose_constraint(c); 
        all_none = false;
      }
    }
    if (all_none) {
      return "__NONE__";
    }
    return cs+")"; 
  }
  return "__NONE__";
}

//Mirrors decompose_constraint. :constraints are (in)equalities over variables
//and constants only, so this is the whole of it.
inline expr::Ptr constraint_to_expr(Constraint const& constraint) {
  int w = which_constraint(constraint);
  if (w != 1 && w != 2) {
    return nullptr;
  }
  Term lhs_t, rhs_t;
  if (w == 1) {
    auto s = boost::get<EqualsSentence>(constraint);
    lhs_t = s.lhs;
    rhs_t = s.rhs;
  }
  else {
    auto s = boost::get<NotEqualsSentence>(constraint);
    lhs_t = s.lhs;
    rhs_t = s.rhs;
  }
  auto conv = [](Term const& t) -> expr::Term {
    if (t.which() == 0) {
      return {boost::get<Constant>(t).name,false};
    }
    return {boost::get<Variable>(t).name,true};
  };
  return expr::make_compare(w == 1,conv(lhs_t),conv(rhs_t));
}

//Mirrors decompose_constraints, which always wraps in an `and`.
inline expr::Ptr constraints_to_expr(Constraints const& constraints) {
  int w = which_constraints(constraints);
  if (w == 1) {
    auto c = constraint_to_expr(boost::get<Constraint>(constraints));
    if (!c) {
      return nullptr;
    }
    return expr::make_connective(expr::Kind::And,{c});
  }
  if (w == 2) {
    std::vector<expr::Ptr> children;
    for (auto const& c : boost::get<std::vector<Constraint>>(constraints)) {
      auto e = constraint_to_expr(c);
      if (e) {
        children.push_back(e);
      }
    }
    if (children.empty()) {
      return nullptr;
    }
    return expr::make_connective(expr::Kind::And,std::move(children));
  }
  return nullptr;
}

//A subtask as the planner stores it: its task, and each argument with the
//declared type of its position.
inline TaskDef task_def_of(MTask const& st, Tasktypes const& ttypes) {
  TaskDef t;
  t.first = st.name;
  for (size_t i = 0; i < st.parameters.size(); i++) {
    t.second.emplace_back(term_name(st.parameters[i]),type_at(ttypes,st.name,i));
  }
  return t;
}

//The subtasks of a network, each with its id. One written without an id gets
//__taskN__, N counting only the unnamed ones, so orderings -- which name ids
//-- cannot refer to it.
inline std::vector<std::pair<ID,TaskDef>> get_subtasks(SubTasks const& subtasks, Tasktypes const& ttypes) {
  std::vector<SubTask> listed;
  if (which_subtasks(subtasks) == 1) {
    listed.push_back(boost::get<SubTask>(subtasks));
  }
  else if (which_subtasks(subtasks) == 2) {
    listed = boost::get<std::vector<SubTask>>(subtasks);
  }
  std::vector<std::pair<ID,TaskDef>> subs;
  int unnamed = 0;
  for (auto const& s : listed) {
    if (which_subtask(s) == 0) {
      subs.emplace_back("__task"+std::to_string(unnamed++)+"__",
                        task_def_of(boost::get<MTask>(s),ttypes));
    }
    else {
      auto const& sbtid = boost::get<SubTaskWithId>(s);
      subs.emplace_back(sbtid.id,task_def_of(sbtid.subtask,ttypes));
    }
  }
  return subs;
}

//The subtasks and orderings of a method's or the problem's task network. One
//function for both: the two used to be separate copies, and when an empty
//ordered network -- `:ordered-subtasks ()`, legal HDDL -- was found to index
//past the end of an empty list, the guard went into the method's copy only.
//A problem whose :htn was empty and ordered still crashed the loader (8.21).
inline void network_of(TaskNetwork const& tn, Tasktypes const& ttypes, TaskDefs& subtasks,
                       std::unordered_map<std::string,std::vector<std::string>>& orderings) {
  if (!tn.subtasks) {
    return;
  }
  auto sts = get_subtasks(tn.subtasks->subtasks,ttypes);
  for (auto const& st : sts) {
    subtasks[st.first] = st.second;
    orderings[st.first] = {};
  }
  bool ordered = tn.subtasks->ordering_kw == "ordered-tasks" ||
                 tn.subtasks->ordering_kw == "ordered-subtasks";
  if (ordered) {
    //Each subtask before the next, in the order written.
    for (size_t i = 1; i < sts.size(); i++) {
      orderings[sts[i-1].first].push_back(sts[i].first);
    }
  }
  else if (tn.orderings) {
    get_orderings(*tn.orderings,orderings);
  }
}

//The text of an .hddl file, or an exception saying why there is none.
inline std::string read_hddl_file(std::string const& file) {
  fs::path filePath = file;
  if (filePath.extension() != ".hddl") {
    throw fs::filesystem_error(filePath.extension().string() + " is an invalid extension, only .hddl is a valid extension!",std::error_code());
  }
  std::error_code ec;
  if (!fs::exists(filePath,ec)) {
    throw fs::filesystem_error("No file "+filePath.filename().string()+" found!",ec);
  }
  std::ifstream f(file);
  return std::string((std::istreambuf_iterator<char>(f)),
                     (std::istreambuf_iterator<char>()));
}

inline Domain dom_loader(std::string dom_file) {
  return parse<Domain>(read_hddl_file(dom_file),dom_file);
}

//A parameter list as (name, type) pairs in the order written: the explicitly
//typed groups, then any untyped names, which are __Object__.
inline Params params_of(TypedList<Variable> const& tl) {
  Params ps;
  for (auto const& group : tl.explicitly_typed_lists) {
    std::string type = type_name_of(group.type);
    for (auto const& v : group.entries) {
      ps.emplace_back(v.name,type);
    }
  }
  for (auto const& v : tl.implicitly_typed_list) {
    ps.emplace_back(v.name,"__Object__");
  }
  return ps;
}

//Parameter types alone, in order.
inline std::vector<std::string> types_of(Params const& ps) {
  std::vector<std::string> ts;
  for (auto const& p : ps) {
    ts.push_back(p.second);
  }
  return ts;
}

//Declared objects or constants: name -> type.
inline Objects objects_of(TypedList<Name> const& tl) {
  Objects os;
  for (auto const& group : tl.explicitly_typed_lists) {
    std::string type = type_name_of(group.type);
    for (auto const& n : group.entries) {
      os[n] = type;
    }
  }
  for (auto const& n : tl.implicitly_typed_list) {
    os[n] = "__Object__";
  }
  return os;
}

inline std::pair<DomainDef,std::pair<Ptypes,Tasktypes>> createDomainDef(Domain const& dom) {
  TypeTree typetree;
  typetree.add_root("__Object__");
  for (auto const& t : dom.types.explicitly_typed_lists) {
    std::string type = type_name_of(t.type); 
    if (typetree.find_type(type) == -1) {
      typetree.add_child(type,"__Object__");  
    }
    for (auto const& e : t.entries) {
      if (typetree.find_type(e) == -1) {
        typetree.add_child(e,type);
      }
      else {
        typetree.add_ancestor(e,type);
      }
    }
  }
  for (auto const& it : dom.types.implicitly_typed_list) {
    if (typetree.find_type(it) == -1) {
      typetree.add_child(it,"__Object__");
    }
  }
  add_either_types(typetree,either_types_in(dom));

  Predicates predicates;
  Ptypes ptypes;
  for (auto const& p : dom.predicates) {
    Params params = params_of(p.variables);
    ptypes[p.predicate] = types_of(params);
    predicates.emplace_back(p.predicate,std::move(params));
  }

  Objects constants = objects_of(dom.constants);

  //Argument types of every task and action, which a subtask may name.
  Tasktypes ttypes;
  for (auto const& t : dom.tasks) {
    ttypes[t.name] = types_of(params_of(t.parameters));
  }

  ActionDefs actions;
  for (auto const& a : dom.actions) {
    Params params = params_of(a.parameters);
    ttypes[a.name] = types_of(params);
    Preconds preconditions = sentence_to_SMT(a.precondition,ptypes);
    expr::Ptr precondition_ast = sentence_to_expr(a.precondition,ptypes);
    Effects effects = decompose_effects(a.effect,ptypes);
    actions.emplace(a.name, ActionDef(a.name,params,preconditions,effects,false,precondition_ast));
  }

  MethodDefs methods;
  for (auto const& m : dom.methods) {
    Params params = params_of(m.parameters);
    TaskDef task = task_def_of(m.task,ttypes);

    //HDDL timing: a method's state precondition is not checked when the method
    //is decomposed. It is compiled into a fresh effect-free primitive action
    //ordered before the method's subtasks, so it is evaluated when that action
    //is scheduled (Höller et al. 2020a). Under SHOP timing the two agree for
    //totally ordered domains, but once tasks interleave they differ: other
    //unconstrained tasks may run between the decomposition and the check.
    Preconds state_pre = sentence_to_SMT(m.precondition,ptypes);
    expr::Ptr state_pre_ast = sentence_to_expr(m.precondition,ptypes);

    //:constraints are (in)equalities over variables and constants only -- see
    //decompose_constraint -- so they never mention state and cannot change
    //between decomposition and scheduling. They stay on the method, where they
    //prune bindings, rather than multiplying branches the artificial action
    //would only reject later.
    Preconds preconditions = "__NONE__";
    expr::Ptr precondition_ast = nullptr;
    if (m.task_network.constraints) {
      std::string cs = decompose_constraints(*m.task_network.constraints);
      if (cs != "__NONE__") {
        preconditions = cs;
        precondition_ast = constraints_to_expr(*m.task_network.constraints);
      }
    }
     
    TaskDefs subtasks;
    std::unordered_map<std::string,std::vector<std::string>> orderings;
    network_of(m.task_network,ttypes,subtasks,orderings);

    if (state_pre != "__NONE__") {
      //One artificial action per method carrying a precondition, taking the
      //method's full parameter list so the binding chosen at decomposition is
      //what the check is evaluated against.
      std::string pre_action = "__mprec_"+m.name;
      int suffix = 1;
      while (actions.contains(pre_action)) {
        pre_action = "__mprec_"+m.name+"_"+std::to_string(suffix);
        suffix++;
      }
      actions.emplace(std::make_pair(pre_action,
                                     ActionDef(pre_action,params,state_pre,Effects{},true,state_pre_ast)));

      //Ordered before every other subtask. Giving it outgoing orderings also
      //keeps it from inheriting the decomposed task's outgoing edges, which
      //only the subtasks with no successor inside the method should take.
      std::string pre_id = "__mprec__";
      std::vector<std::string> before;
      for (auto const& [id,st] : subtasks) {
        before.push_back(id);
      }
      subtasks[pre_id] = std::make_pair(pre_action,params);
      orderings[pre_id] = before;
    }

    methods[m.task.name].push_back(MethodDef(m.name,task,params,preconditions,subtasks,orderings,precondition_ast));
  }
  auto DD = DomainDef(dom.name,typetree,predicates,constants,actions,methods);
  return std::make_pair(DD,std::make_pair(ptypes,ttypes));
}

//Parses, validates (validate.h) and builds a domain on its own. A problem
//needs its domain to be checked, so use load() or load_hddl() for the pair.
inline std::pair<DomainDef,std::pair<Ptypes,Tasktypes>>
loadDomain(std::string dom_file, std::vector<std::string>* warnings = nullptr) {
  SourceLines lines;
  Domain dom = parse<Domain>(read_hddl_file(dom_file),dom_file,&lines);
  canonicalise_case(dom);
  validate_hddl(dom,lines,nullptr,nullptr,warnings);
  return createDomainDef(dom);
}

inline Problem prob_loader(std::string prob_file) {
  return parse<Problem>(read_hddl_file(prob_file),prob_file);
}

inline ProblemDef createProblemDef(Problem const& prob, Ptypes const& ptypes, Tasktypes const& ttypes) {
  std::string head = "__"+prob.name+"__";
  std::string domain_name = prob.domain_name;

  Objects objects = objects_of(prob.objects);
  
  std::string m_name = prob.problem_htn.problem_class;
  TaskDef task;
  Params params;
  Preconds preconditions = "__NONE__";
  expr::Ptr precondition_ast = nullptr;
  TaskDefs subtasks;
  std::unordered_map<std::string,std::vector<std::string>> orderings;
  if (m_name == "") {
    m_name = ":c";
  }
  else {
    task.first = head;
    task.second = {};
    params = params_of(prob.problem_htn.parameters);
    //As for a method: the string for Z3, and the structured form the direct
    //evaluator reads, which this copy used to leave out.
    if (prob.problem_htn.task_network.constraints) {
      std::string cs = decompose_constraints(*prob.problem_htn.task_network.constraints);
      if (cs != "__NONE__") {
        preconditions = cs;
        precondition_ast = constraints_to_expr(*prob.problem_htn.task_network.constraints);
      }
    }
    network_of(prob.problem_htn.task_network,ttypes,subtasks,orderings);
  }
  MethodDef initM = MethodDef(m_name,task,params,preconditions,subtasks,orderings,precondition_ast);

  std::vector<std::string> initF;
  for (auto const& i : prob.init) {
    std::string pred = "("+i.predicate;
    for (auto const& a : i.args) {
      pred += " "+a;
    }
    pred += ")";
    initF.push_back(pred);
  }

  std::string goal = sentence_to_SMT(prob.goal,ptypes);
  if (goal == "__NONE__") {
    goal = "";
  }
  return ProblemDef(head,domain_name,objects,initM,initF,goal);
}

inline ProblemDef loadProblem(std::string const& prob_file, Ptypes const& ptypes, Tasktypes const& ttypes) {
  Problem prob = prob_loader(prob_file);
  return createProblemDef(prob,ptypes,ttypes);
}

//Loads a domain and problem from their text rather than from files -- the
//form a pipeline that asks a language model for HDDL has them in. The names
//are what errors are reported against. Throws ParseError for text the grammar
//rejects, and HDDLError listing every problem validate.h finds, before
//anything is built. What is legal but probably unintended -- a task nothing
//reaches, a parameter never used -- goes into `warnings` if one is given.
//Names are matched in any case and rewritten to their declared spelling first
//(case.h).
inline std::pair<DomainDef, ProblemDef> load_hddl(std::string const& dom_text,
                                           std::string const& prob_text,
                                           std::string const& dom_name = "domain",
                                           std::string const& prob_name = "problem",
                                           std::vector<std::string>* warnings = nullptr) {
  SourceLines dom_lines, prob_lines;
  Domain dom = parse<Domain>(dom_text,dom_name,&dom_lines);
  Problem prob = parse<Problem>(prob_text,prob_name,&prob_lines);
  canonicalise_case(dom,&prob);
  validate_hddl(dom,dom_lines,&prob,&prob_lines,warnings);

  auto [domDef,types] = createDomainDef(dom);
  auto probDef = createProblemDef(prob,types.first,types.second);
  add_either_types(domDef.typetree,either_types_in(prob));
  domDef.methods[probDef.initM.get_task().first].push_back(probDef.initM);
  probDef.objects.merge(domDef.constants);
  return std::make_pair(domDef,probDef);
}

inline std::pair<DomainDef, ProblemDef> load(std::string dom_file, std::string prob_file,
                                             std::vector<std::string>* warnings = nullptr) {
  return load_hddl(read_hddl_file(dom_file),read_hddl_file(prob_file),dom_file,prob_file,warnings);
}

} // namespace hddl_loader

//The loader's public interface.
using hddl_loader::load;
using hddl_loader::load_hddl;
using hddl_loader::loadDomain;
using hddl_loader::loadProblem;
using hddl_loader::createDomainDef;
using hddl_loader::createProblemDef;
using hddl_loader::dom_loader;
using hddl_loader::prob_loader;
using hddl_loader::read_hddl_file;
using hddl_loader::Ptypes;
using hddl_loader::Tasktypes;
