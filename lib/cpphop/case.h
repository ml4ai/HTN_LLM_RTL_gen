#pragma once
//PDDL and HDDL names are case-insensitive: Truck_0 and truck_0 are one object.
//The parser keeps names as written, so this pass, run on the parsed domain and
//problem before validation, rewrites every use of a name to the spelling it
//was first declared with (planner_doc.md 8.23). Plans then print names as the
//domain declared them, and a domain that is already consistent is untouched.
//
//Each kind of name has its own namespace, as in PDDL: types, predicates,
//tasks and actions (one namespace, since a subtask may name either), methods,
//objects and constants, variables (scoped: a method's or action's parameters,
//a quantifier's or forall effect's variables), and a task network's subtask
//ids. A second declaration differing only in case is rewritten too, so
//validation reports it as declared twice, which in PDDL it is.

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>
#include "parsing/ast.hpp"

namespace hddl_case {

using namespace ast;

inline std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return s;
}

//lower case -> the spelling first seen.
struct Names {
  std::unordered_map<std::string,std::string> spelling;

  void declare(std::string& name) {
    auto [it,fresh] = spelling.try_emplace(lower(name), name);
    if (!fresh) {
      name = it->second;
    }
  }
  //Rewritten if declared; left alone otherwise, for validation to report.
  bool use(std::string& name) const {
    auto it = spelling.find(lower(name));
    if (it == spelling.end()) {
      return false;
    }
    name = it->second;
    return true;
  }
};

class Canonicaliser {
public:
  Names types, predicates, tasks, methods, objects;

  Canonicaliser() {
    std::string object = "object";
    types.declare(object);
  }

  void domain(Domain& d) {
    for (auto& r : d.requirements) r = lower(r);
    for (auto& group : d.types.explicitly_typed_lists) {
      for (auto& t : group.entries) types.declare(t);
      declare_type(group.type);
    }
    for (auto& t : d.types.implicitly_typed_list) types.declare(t);

    for (auto& group : d.constants.explicitly_typed_lists) {
      for (auto& c : group.entries) objects.declare(c);
      type(group.type);
    }
    for (auto& c : d.constants.implicitly_typed_list) objects.declare(c);

    for (auto& p : d.predicates) {
      predicates.declare(p.predicate);
      Names scope;
      params(p.variables, scope);
    }
    for (auto& t : d.tasks) {
      tasks.declare(t.name);
      Names scope;
      params(t.parameters, scope);
    }
    for (auto& a : d.actions) tasks.declare(a.name);
    for (auto& m : d.methods) methods.declare(m.name);

    for (auto& a : d.actions) {
      Names scope;
      params(a.parameters, scope);
      sentence(a.precondition, scope);
      effect(a.effect, scope);
    }
    for (auto& m : d.methods) {
      Names scope;
      params(m.parameters, scope);
      task_use(m.task, scope);
      sentence(m.precondition, scope);
      network(m.task_network, scope);
    }
  }

  void problem(Problem& p, Domain const& d) {
    if (lower(p.domain_name) == lower(d.name)) {
      p.domain_name = d.name;
    }
    for (auto& r : p.requirements) r = lower(r);
    for (auto& group : p.objects.explicitly_typed_lists) {
      for (auto& o : group.entries) objects.declare(o);
      type(group.type);
    }
    for (auto& o : p.objects.implicitly_typed_list) objects.declare(o);

    Names scope;
    params(p.problem_htn.parameters, scope);
    network(p.problem_htn.task_network, scope);

    for (auto& f : p.init) {
      predicate(f.predicate);
      for (auto& a : f.args) objects.use(a);
    }
    Names none;
    sentence(p.goal, none);
  }

private:
  void declare_type(ast::Type& t) {
    if (t.get().which() == 0) {
      types.declare(boost::get<PrimitiveType>(t));
    }
    else {
      type(t);
    }
  }

  void type(ast::Type& t) {
    if (t.get().which() == 0) {
      types.use(boost::get<PrimitiveType>(t));
      return;
    }
    //An unordered_set: rebuilt, since its elements cannot be edited in place.
    auto& members = boost::get<EitherType>(t);
    EitherType fixed;
    for (auto m : members) {
      types.use(m);
      fixed.insert(m);
    }
    members = std::move(fixed);
  }

  //Parameters and quantified variables are declarations in `scope`.
  void params(TypedList<Variable>& tl, Names& scope) {
    for (auto& group : tl.explicitly_typed_lists) {
      for (auto& v : group.entries) scope.declare(v.name);
      type(group.type);
    }
    for (auto& v : tl.implicitly_typed_list) scope.declare(v.name);
  }

  void term(Term& t, Names const& scope) {
    if (t.which() == 0) {
      objects.use(boost::get<Constant>(t).name);
    }
    else if (t.which() == 1) {
      scope.use(boost::get<Variable>(t).name);
    }
  }

  //A predicate, or a type used as a one-argument predicate.
  void predicate(std::string& name) {
    if (!predicates.use(name)) {
      types.use(name);
    }
  }

  void atom(Literal<Term>& l, Names const& scope) {
    predicate(l.predicate);
    for (auto& a : l.args) term(a, scope);
  }

  void sentence(Sentence& s, Names const& scope) {
    switch (s.which()) {
      case 1:
        atom(boost::get<Literal<Term>>(s), scope);
        break;
      case 2:
        for (auto& c : boost::get<ConnectedSentence>(s).sentences) sentence(c, scope);
        break;
      case 3:
        sentence(boost::get<NotSentence>(s).sentence, scope);
        break;
      case 4:
        sentence(boost::get<ImplySentence>(s).sentence1, scope);
        sentence(boost::get<ImplySentence>(s).sentence2, scope);
        break;
      case 5: {
        auto& q = boost::get<QuantifiedSentence>(s);
        Names inner = scope;
        params(q.variables, inner);
        sentence(q.sentence, inner);
        break;
      }
      case 6:
        term(boost::get<EqualsSentence>(s).lhs, scope);
        term(boost::get<EqualsSentence>(s).rhs, scope);
        break;
      case 7:
        term(boost::get<NotEqualsSentence>(s).lhs, scope);
        term(boost::get<NotEqualsSentence>(s).rhs, scope);
        break;
    }
  }

  void effect(Effect& e, Names const& scope) {
    if (e.which() == 1) {
      for (auto& c : boost::get<AndCEffect>(e).c_effects) c_effect(c, scope);
    }
    else if (e.which() == 2) {
      c_effect(boost::get<CEffect>(e), scope);
    }
  }

  void c_effect(CEffect& c, Names const& scope) {
    if (c.which() == 0) {
      auto& f = boost::get<ForallCEffect>(c);
      Names inner = scope;
      params(f.variables, inner);
      effect(f.effect, inner);
    }
    else if (c.which() == 1) {
      auto& w = boost::get<WhenCEffect>(c);
      sentence(w.gd, scope);
      if (w.cond_effect.which() == 0) {
        atom(boost::get<PEffect>(w.cond_effect), scope);
      }
      else {
        for (auto& p : boost::get<std::vector<PEffect>>(w.cond_effect)) atom(p, scope);
      }
    }
    else {
      atom(boost::get<PEffect>(c), scope);
    }
  }

  void task_use(MTask& t, Names const& scope) {
    tasks.use(t.name);
    for (auto& a : t.parameters) term(a, scope);
  }

  void network(TaskNetwork& tn, Names const& scope) {
    Names ids;
    if (tn.subtasks) {
      auto each = [&](SubTask& s) {
        if (s.get().which() == 0) {
          task_use(boost::get<MTask>(s), scope);
        }
        else {
          auto& st = boost::get<SubTaskWithId>(s);
          ids.declare(st.id);
          task_use(st.subtask, scope);
        }
      };
      auto& sts = tn.subtasks->subtasks;
      if (sts.get().which() == 1) {
        each(boost::get<SubTask>(sts));
      }
      else if (sts.get().which() == 2) {
        for (auto& s : boost::get<std::vector<SubTask>>(sts)) each(s);
      }
    }
    if (tn.orderings) {
      auto each = [&](Ordering& o) {
        ids.use(o.first);
        ids.use(o.second);
      };
      auto& os = *tn.orderings;
      if (os.get().which() == 1) {
        each(boost::get<Ordering>(os));
      }
      else if (os.get().which() == 2) {
        for (auto& o : boost::get<std::vector<Ordering>>(os)) each(o);
      }
    }
    if (tn.constraints) {
      auto each = [&](Constraint& c) {
        if (c.get().which() == 1) {
          term(boost::get<EqualsSentence>(c).lhs, scope);
          term(boost::get<EqualsSentence>(c).rhs, scope);
        }
        else if (c.get().which() == 2) {
          term(boost::get<NotEqualsSentence>(c).lhs, scope);
          term(boost::get<NotEqualsSentence>(c).rhs, scope);
        }
      };
      auto& cs = *tn.constraints;
      if (cs.get().which() == 1) {
        each(boost::get<Constraint>(cs));
      }
      else if (cs.get().which() == 2) {
        for (auto& c : boost::get<std::vector<Constraint>>(cs)) each(c);
      }
    }
  }
};

} // namespace hddl_case

//Rewrites every name in a domain, and in a problem against it if one is
//given, to its declared spelling. Run before validation.
inline void canonicalise_case(ast::Domain& d, ast::Problem* p = nullptr) {
  hddl_case::Canonicaliser c;
  c.domain(d);
  if (p) {
    c.problem(*p, d);
  }
}
