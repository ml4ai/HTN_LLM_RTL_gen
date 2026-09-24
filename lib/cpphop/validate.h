#pragma once

//Checks a parsed domain and problem before anything is built from them.
//
//The loader trusts its input. A subtask naming an undeclared task, a method for
//an undeclared task, or an effect on an undeclared predicate used to crash it,
//and an undeclared predicate, object, variable or type in the wrong place made
//every rollout fail with no reason given (planner_doc.md 9.1). A language
//model writing HDDL makes exactly these mistakes, and what it needs back is a
//list of them it can act on. So this pass collects every problem it finds,
//each one naming the file and line, the construct it is in and what is wrong,
//and the loader throws them together as one HDDLError.
//
//It checks that every task, action, predicate, type, object, constant and
//variable used is declared and in scope, that every use has the declared
//number of arguments, that argument types can agree, and that a method's
//orderings name its own subtasks and do not form a cycle.

#include <algorithm>
#include <climits>
#include <tuple>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "parsing/ast.hpp"
#include "parsing/parse.hpp"

//Every problem found in a domain and problem, one per entry of problems().
//what() lists them all, one per line.
class HDDLError : public std::runtime_error {
public:
  explicit HDDLError(std::vector<std::string> problems)
    : std::runtime_error(render(problems)), problems_(std::move(problems)) {}

  std::vector<std::string> const& problems() const { return problems_; }

private:
  std::vector<std::string> problems_;

  static std::string render(std::vector<std::string> const& problems) {
    std::string s = std::to_string(problems.size()) +
                    (problems.size() == 1 ? " problem" : " problems") +
                    " in the HDDL input:";
    for (auto const& p : problems) {
      s += "\n  " + p;
    }
    return s;
  }
};

namespace hddl_check {

using namespace ast;

//Edit distance counting a swap of neighbouring letters as one edit (so
//`raod` is one from `road`), for "did you mean" suggestions. Names are short.
inline int edit_distance(std::string const& a, std::string const& b) {
  std::vector<std::vector<int>> d(a.size() + 1, std::vector<int>(b.size() + 1));
  for (size_t i = 0; i <= a.size(); i++) d[i][0] = (int)i;
  for (size_t j = 0; j <= b.size(); j++) d[0][j] = (int)j;
  for (size_t i = 1; i <= a.size(); i++) {
    for (size_t j = 1; j <= b.size(); j++) {
      d[i][j] = std::min({d[i-1][j] + 1, d[i][j-1] + 1,
                          d[i-1][j-1] + (a[i-1] == b[j-1] ? 0 : 1)});
      if (i > 1 && j > 1 && a[i-1] == b[j-2] && a[i-2] == b[j-1]) {
        d[i][j] = std::min(d[i][j], d[i-2][j-2] + 1);
      }
    }
  }
  return d[a.size()][b.size()];
}

//Candidates come as names or as map entries keyed by name.
inline std::string const& key_of(std::string const& s) { return s; }
template <class V>
std::string const& key_of(std::pair<const std::string,V> const& p) { return p.first; }

//" (did you mean X?)" for the closest candidate near enough to be a slip, or "".
template <class Names>
std::string suggest(std::string const& name, Names const& candidates,
                    std::string const& prefix = "") {
  std::string best;
  int best_d = 1 + std::max<int>(1, (int)name.size() / 3);
  //Sorted, so ties resolve the same way on every run.
  std::set<std::string> sorted;
  for (auto const& c : candidates) {
    sorted.insert(key_of(c));
  }
  for (auto const& c : sorted) {
    int d = edit_distance(name, c);
    if (d < best_d) {
      best_d = d;
      best = c;
    }
  }
  return best.empty() ? "" : " (did you mean " + prefix + best + "?)";
}


//A declared task, action or predicate: its parameters' types, in order.
using Signature = std::vector<std::string>;

class Checker {
public:

  //Types as declared: each type's direct parents. `object` is always there.
  std::unordered_map<std::string,std::vector<std::string>> parents = {{"object",{}}};
  std::unordered_map<std::string,Signature> predicates;
  std::unordered_map<std::string,Signature> tasks;
  std::unordered_map<std::string,Signature> actions;
  //Domain constants, then problem objects: name -> type.
  std::unordered_map<std::string,std::string> constants;

  //Where the construct being checked is, for the start of each message.
  struct Where {
    std::string file;
    int line = 0;
    std::string construct;  //"method m_deliver", ":init", ...
  };

  void report(Where const& w, std::string const& what) {
    std::string at = w.file;
    if (w.line > 0) {
      at += ":" + std::to_string(w.line);
    }
    //The checks run construct by construct, not top to bottom, so the report
    //is put back into source order: domain before problem, then by line.
    //:init and :goal carry no line (their nodes are not position-tagged) but
    //end a problem file; everything else without one is a header section.
    auto rank = std::find(files.begin(),files.end(),w.file);
    if (rank == files.end()) {
      rank = files.insert(files.end(),w.file);
    }
    int key = w.line > 0 ? w.line
            : (w.construct == ":init" || w.construct == ":goal") ? INT_MAX : 0;
    found.push_back({(int)(rank - files.begin()),key,
                     at + ": " + (w.construct.empty() ? "" : "in " + w.construct + ": ") + what});
  }

  std::vector<std::string> sorted_problems() const {
    auto f = found;
    std::stable_sort(f.begin(),f.end(),[](auto const& a, auto const& b) {
      return std::tie(a.file_rank,a.key) < std::tie(b.file_rank,b.key);
    });
    std::vector<std::string> out;
    for (auto const& x : f) {
      out.push_back(x.text);
    }
    return out;
  }

private:
  struct Found {
    int file_rank;
    int key;
    std::string text;
  };
  std::vector<Found> found;
  std::vector<std::string> files;

public:

  //True when a value of type a can be passed where type b is expected, or the
  //other way round. Both directions are allowed on purpose: domains pass a
  //method's `locatable` variable to a subtask's `package` parameter and rely
  //on grounding to narrow it, and that is legal. What is caught is two types
  //with no ancestor in common but `object` -- a location where a vehicle
  //belongs, which is how swapped arguments show up.
  bool compatible(std::string const& a, std::string const& b) const {
    if (a.empty() || b.empty() || a == b || a == "object" || b == "object") {
      return true;
    }
    return is_ancestor(a,b) || is_ancestor(b,a);
  }

  bool is_ancestor(std::string const& anc, std::string const& t) const {
    std::vector<std::string> todo = {t};
    std::unordered_set<std::string> seen;
    while (!todo.empty()) {
      auto x = todo.back();
      todo.pop_back();
      if (x == anc) {
        return true;
      }
      if (!seen.insert(x).second) {
        continue;
      }
      auto it = parents.find(x);
      if (it != parents.end()) {
        todo.insert(todo.end(), it->second.begin(), it->second.end());
      }
    }
    return false;
  }

  //The name of a declared type, or "" after reporting why there is none.
  std::string type_name(ast::Type const& t, Where const& w, std::string const& of) {
    if (t.get().which() != 0) {
      report(w, of + " has an (either ...) type, which this planner does not support; "
                "declare a common supertype instead");
      return "";
    }
    auto const& name = boost::get<PrimitiveType>(t);
    if (!parents.contains(name)) {
      report(w, of + " has type " + name + ", which is not declared in :types" +
                suggest(name,parents));
      return "";
    }
    return name;
  }

  //Parameters in declaration order as (name, type). Reports undeclared types
  //and repeated names; a parameter whose type is bad gets "" and is still in
  //scope, so its uses are not reported a second time.
  std::vector<std::pair<std::string,std::string>>
  parameters(TypedList<Variable> const& tl, Where const& w, std::string const& what = "parameter") {
    std::vector<std::pair<std::string,std::string>> out;
    for (auto const& e : tl.explicitly_typed_lists) {
      for (auto const& v : e.entries) {
        out.push_back({v.name, type_name(e.type,w,what + " ?" + v.name)});
      }
    }
    for (auto const& v : tl.implicitly_typed_list) {
      out.push_back({v.name,"object"});
    }
    std::unordered_set<std::string> seen;
    for (auto const& [name,type] : out) {
      if (!seen.insert(name).second) {
        report(w, what + " ?" + name + " is declared twice");
      }
    }
    return out;
  }

  static Signature types_of(std::vector<std::pair<std::string,std::string>> const& ps) {
    Signature s;
    for (auto const& p : ps) {
      s.push_back(p.second);
    }
    return s;
  }

  using Scope = std::unordered_map<std::string,std::string>;

  static Scope scope_of(std::vector<std::pair<std::string,std::string>> const& ps) {
    return Scope(ps.begin(), ps.end());
  }

  //The type of a term, or "" (after reporting) when it is not declared.
  std::string term_type(Term const& t, Scope const& scope, Where const& w,
                        std::string const& in) {
    if (t.which() == 0) {
      auto const& name = boost::get<Constant>(t).name;
      auto it = constants.find(name);
      if (it == constants.end()) {
        report(w, in + " uses " + name + ", which is not a declared " +
                  (constants_include_objects ? "object or constant" : "constant") +
                  suggest(name,constants));
        return "";
      }
      return it->second;
    }
    if (t.which() == 1) {
      auto const& name = boost::get<Variable>(t).name;
      auto it = scope.find(name);
      if (it == scope.end()) {
        report(w, in + " uses ?" + name + ", which is not a parameter here or bound by a quantifier" +
                  suggest(name,scope,"?"));
        return "";
      }
      return it->second;
    }
    report(w, in + " uses a function term, which this planner does not support");
    return "";
  }

  std::string show(Term const& t) {
    if (t.which() == 0) return boost::get<Constant>(t).name;
    if (t.which() == 1) return "?" + boost::get<Variable>(t).name;
    return "<function>";
  }

  template <class Args>
  std::string show(std::string const& head, Args const& args) {
    std::string s = "(" + head;
    for (auto const& a : args) {
      s += " " + show(a);
    }
    return s + ")";
  }

  std::string show(Name const& n) { return n; }

  //Arguments against a signature: count, then each argument's type.
  template <class Args, class TypeOf>
  void check_args(std::string const& kind, std::string const& head, Signature const& sig,
                  Args const& args, TypeOf type_of, Where const& w, std::string const& in) {
    if (args.size() != sig.size()) {
      report(w, in + " gives " + kind + " " + head + " " + std::to_string(args.size()) +
                (args.size() == 1 ? " argument" : " arguments") + ", but it is declared with " +
                std::to_string(sig.size()));
      return;
    }
    for (size_t i = 0; i < args.size(); i++) {
      std::string t = type_of(args[i]);
      if (!compatible(t,sig[i])) {
        report(w, in + ": argument " + std::to_string(i+1) + " (" + show(args[i]) +
                  ") has type " + t + ", but " + kind + " " + head + " expects " + sig[i] +
                  " there");
      }
    }
  }

  //Every type is also a one-argument predicate, true of the objects of that
  //type (KnowledgeBase::initialize), so (robot ?r) is a legal condition.
  //nullptr, after reporting, for a name that is neither.
  Signature const* predicate_signature(std::string const& name, size_t nargs,
                                       Where const& w, std::string const& in) {
    auto it = predicates.find(name);
    if (it != predicates.end()) {
      return &it->second;
    }
    if (parents.contains(name)) {
      if (nargs != 1) {
        report(w, in + ": " + name + " is a type, which as a predicate takes one argument");
        return nullptr;
      }
      static Signature const one_object = {"object"};
      return &one_object;
    }
    report(w, in + ": predicate " + name + " is not declared in :predicates" +
              suggest(name,predicates));
    return nullptr;
  }

  void atom(Literal<Term> const& l, Scope const& scope, Where const& w, bool in_effect) {
    std::string text = show(l.predicate,l.args);
    std::string in = (in_effect ? "effect " : "condition ") + text;
    Signature const* sig = predicate_signature(l.predicate,l.args.size(),w,in);
    if (!sig) {
      for (auto const& a : l.args) {
        term_type(a,scope,w,in);
      }
      return;
    }
    check_args("predicate",l.predicate,*sig,l.args,
               [&](Term const& a) { return term_type(a,scope,w,in); },w,in);
  }

  void sentence(Sentence const& s, Scope const& scope, Where const& w) {
    switch (s.which()) {
      case 0:
        return;
      case 1:
        atom(boost::get<Literal<Term>>(s),scope,w,false);
        return;
      case 2:
        for (auto const& c : boost::get<ConnectedSentence>(s).sentences) {
          sentence(c,scope,w);
        }
        return;
      case 3:
        sentence(boost::get<NotSentence>(s).sentence,scope,w);
        return;
      case 4: {
        auto const& i = boost::get<ImplySentence>(s);
        sentence(i.sentence1,scope,w);
        sentence(i.sentence2,scope,w);
        return;
      }
      case 5: {
        auto const& q = boost::get<QuantifiedSentence>(s);
        Scope inner = scope;
        for (auto const& [name,type] : parameters(q.variables,w,q.quantifier + " variable")) {
          inner[name] = type;
        }
        sentence(q.sentence,inner,w);
        return;
      }
      case 6: {
        auto const& e = boost::get<EqualsSentence>(s);
        term_type(e.lhs,scope,w,"(= ...)");
        term_type(e.rhs,scope,w,"(= ...)");
        return;
      }
      case 7: {
        auto const& e = boost::get<NotEqualsSentence>(s);
        term_type(e.lhs,scope,w,"(not (= ...))");
        term_type(e.rhs,scope,w,"(not (= ...))");
        return;
      }
    }
  }

  void effect(Effect const& e, Scope const& scope, Where const& w) {
    if (e.which() == 1) {
      for (auto const& c : boost::get<AndCEffect>(e).c_effects) {
        c_effect(c,scope,w);
      }
    }
    else if (e.which() == 2) {
      c_effect(boost::get<CEffect>(e),scope,w);
    }
  }

  void c_effect(CEffect const& c, Scope const& scope, Where const& w) {
    if (c.which() == 0) {
      auto const& f = boost::get<ForallCEffect>(c);
      Scope inner = scope;
      //Untyped in this grammar; the loader infers the type from use.
      for (auto const& v : f.variables) {
        inner[v.name] = "object";
      }
      effect(f.effect,inner,w);
    }
    else if (c.which() == 1) {
      auto const& when = boost::get<WhenCEffect>(c);
      sentence(when.gd,scope,w);
      if (when.cond_effect.which() == 0) {
        atom(boost::get<PEffect>(when.cond_effect),scope,w,true);
      }
      else {
        for (auto const& p : boost::get<std::vector<PEffect>>(when.cond_effect)) {
          atom(p,scope,w,true);
        }
      }
    }
    else {
      atom(boost::get<PEffect>(c),scope,w,true);
    }
  }

  //A task or action named as a subtask, or as the task a method decomposes.
  void task_use(MTask const& t, Scope const& scope, Where w, std::string const& in,
                bool compound_only) {
    std::string text = show(t.name,t.parameters);
    Signature const* sig = nullptr;
    std::string kind = "task";
    if (tasks.contains(t.name)) {
      sig = &tasks.at(t.name);
    }
    else if (actions.contains(t.name)) {
      if (compound_only) {
        report(w, in + " " + text + " names action " + t.name +
                  "; a method decomposes a compound task (declared with :task)");
        return;
      }
      sig = &actions.at(t.name);
      kind = "action";
    }
    if (!sig) {
      std::unordered_set<std::string> names;
      for (auto const& [n,_] : tasks) names.insert(n);
      if (!compound_only) {
        for (auto const& [n,_] : actions) names.insert(n);
      }
      report(w, in + " " + text + ": " + t.name + " is not a declared " +
                (compound_only ? "task" : "task or action") + suggest(t.name,names));
      for (auto const& a : t.parameters) {
        term_type(a,scope,w,in + " " + text);
      }
      return;
    }
    check_args(kind,t.name,*sig,t.parameters,
               [&](Term const& a) { return term_type(a,scope,w,in + " " + text); },
               w,in + " " + text);
  }

  //Subtasks, orderings and constraints of a method or of the problem's :htn.
  void task_network(TaskNetwork const& tn, Scope const& scope, Where const& w,
                    SourceLines const& lines) {
    std::vector<std::string> ids;
    if (tn.subtasks) {
      std::vector<SubTask> subs;
      auto const& st = tn.subtasks->subtasks;
      if (st.get().which() == 1) {
        subs.push_back(boost::get<SubTask>(st));
      }
      else if (st.get().which() == 2) {
        subs = boost::get<std::vector<SubTask>>(st);
      }
      for (auto const& s : subs) {
        Where sw = w;
        if (s.get().which() == 0) {
          auto const& t = boost::get<MTask>(s);
          if (lines.line(t)) sw.line = lines.line(t);
          task_use(t,scope,sw,"subtask",false);
        }
        else {
          auto const& t = boost::get<SubTaskWithId>(s);
          if (lines.line(t)) sw.line = lines.line(t);
          if (std::find(ids.begin(),ids.end(),t.id) != ids.end()) {
            report(sw, "subtask id " + t.id + " is used twice");
          }
          ids.push_back(t.id);
          task_use(t.subtask,scope,sw,"subtask " + t.id + ",",false);
        }
      }
    }
    if (tn.orderings) {
      std::vector<Ordering> os;
      auto const& o = *tn.orderings;
      if (o.get().which() == 1) {
        os.push_back(boost::get<Ordering>(o));
      }
      else if (o.get().which() == 2) {
        os = boost::get<std::vector<Ordering>>(o);
      }
      std::map<std::string,std::vector<std::string>> after;
      for (auto const& ord : os) {
        Where ow = w;
        if (lines.line(ord)) ow.line = lines.line(ord);
        bool ok = true;
        for (auto const& id : {ord.first,ord.second}) {
          if (std::find(ids.begin(),ids.end(),id) == ids.end()) {
            report(ow, "ordering (< " + ord.first + " " + ord.second + ") names " + id +
                       ", which is not one of this network's subtask ids" + suggest(id,ids));
            ok = false;
          }
        }
        if (ok) {
          after[ord.first].push_back(ord.second);
        }
      }
      //A cycle makes the network impossible to linearise: no plan can use it.
      std::map<std::string,int> state;  //0 new, 1 on the path, 2 done
      std::function<bool(std::string const&)> cyclic = [&](std::string const& id) {
        state[id] = 1;
        for (auto const& n : after[id]) {
          if (state[n] == 1 || (state[n] == 0 && cyclic(n))) {
            return true;
          }
        }
        state[id] = 2;
        return false;
      };
      for (auto const& id : ids) {
        if (state[id] == 0 && cyclic(id)) {
          report(w, "the :ordering constraints form a cycle through " + id +
                    ", so no order of the subtasks satisfies them");
          break;
        }
      }
    }
    if (tn.constraints) {
      std::vector<Constraint> cs;
      auto const& c = *tn.constraints;
      if (c.get().which() == 1) {
        cs.push_back(boost::get<Constraint>(c));
      }
      else if (c.get().which() == 2) {
        cs = boost::get<std::vector<Constraint>>(c);
      }
      for (auto const& con : cs) {
        if (con.get().which() == 1) {
          auto const& e = boost::get<EqualsSentence>(con);
          term_type(e.lhs,scope,w,"constraint");
          term_type(e.rhs,scope,w,"constraint");
        }
        else if (con.get().which() == 2) {
          auto const& e = boost::get<NotEqualsSentence>(con);
          term_type(e.lhs,scope,w,"constraint");
          term_type(e.rhs,scope,w,"constraint");
        }
      }
    }
  }

  bool constants_include_objects = false;

  void declare_types(TypedList<Name> const& types, Where const& w) {
    for (auto const& e : types.explicitly_typed_lists) {
      if (e.type.get().which() != 0) {
        report(w, "a type in :types has an (either ...) parent, which this planner does not support");
        continue;
      }
      auto const& parent = boost::get<PrimitiveType>(e.type);
      //A parent named only on the right of a `-` is declared by that use,
      //as the loader has always treated it.
      parents.try_emplace(parent);
      for (auto const& t : e.entries) {
        parents[t].push_back(parent);
      }
    }
    for (auto const& t : types.implicitly_typed_list) {
      parents.try_emplace(t);
    }
    for (auto const& [t,_] : parents) {
      if (cyclic_type(t)) {
        report(w, "type " + t + " is its own ancestor in :types");
      }
    }
  }

  bool cyclic_type(std::string const& t) const {
    for (auto const& p : parents.at(t)) {
      if (is_ancestor(t,p)) {
        return true;
      }
    }
    return false;
  }

  //name -> type, reporting undeclared types and repeats.
  void declare_objects(TypedList<Name> const& tl, Where const& w, std::string const& kind) {
    auto add = [&](std::string const& name, std::string const& type) {
      if (constants.contains(name)) {
        report(w, kind + " " + name + " is declared twice");
        return;
      }
      constants[name] = type;
    };
    for (auto const& e : tl.explicitly_typed_lists) {
      for (auto const& n : e.entries) {
        add(n, type_name(e.type,w,kind + " " + n));
      }
    }
    for (auto const& n : tl.implicitly_typed_list) {
      add(n,"object");
    }
  }

  void domain(Domain const& d, SourceLines const& lines) {
    Where top{lines.file,0,""};

    declare_types(d.types,Where{lines.file,0,":types"});
    declare_objects(d.constants,Where{lines.file,0,":constants"},"constant");

    for (auto const& p : d.predicates) {
      Where w{lines.file,lines.line(p),"predicate " + p.predicate};
      if (predicates.contains(p.predicate)) {
        report(w, "predicate " + p.predicate + " is declared twice");
        continue;
      }
      predicates[p.predicate] = types_of(parameters(p.variables,w));
    }

    for (auto const& t : d.tasks) {
      Where w{lines.file,lines.line(t),"task " + t.name};
      if (tasks.contains(t.name)) {
        report(w, "task " + t.name + " is declared twice");
        continue;
      }
      tasks[t.name] = types_of(parameters(t.parameters,w));
    }

    //Signatures before bodies, so a method may name an action declared after it.
    std::vector<Scope> action_scopes;
    for (auto const& a : d.actions) {
      Where w{lines.file,lines.line(a),"action " + a.name};
      auto ps = parameters(a.parameters,w);
      action_scopes.push_back(scope_of(ps));
      if (actions.contains(a.name)) {
        report(w, "action " + a.name + " is declared twice");
        continue;
      }
      if (tasks.contains(a.name)) {
        report(w, a.name + " is declared both as a task and as an action");
      }
      actions[a.name] = types_of(ps);
    }

    for (size_t i = 0; i < d.actions.size(); i++) {
      auto const& a = d.actions[i];
      Where w{lines.file,lines.line(a),"action " + a.name};
      sentence(a.precondition,action_scopes[i],w);
      effect(a.effect,action_scopes[i],w);
    }

    std::unordered_set<std::string> method_names;
    for (auto const& m : d.methods) {
      Where w{lines.file,lines.line(m),"method " + m.name};
      if (!method_names.insert(m.name).second) {
        report(w, "method " + m.name + " is declared twice");
      }
      auto scope = scope_of(parameters(m.parameters,w));
      Where tw = w;
      if (lines.line(m.task)) tw.line = lines.line(m.task);
      task_use(m.task,scope,tw,":task",true);
      sentence(m.precondition,scope,w);
      task_network(m.task_network,scope,w,lines);
    }
  }

  void problem(Problem const& p, Domain const& d, SourceLines const& lines) {
    Where top{lines.file,lines.line(p),""};
    if (p.domain_name != d.name) {
      report(top, "the problem is for domain " + p.domain_name +
                  ", but the domain loaded is " + d.name);
    }
    constants_include_objects = true;
    declare_objects(p.objects,Where{lines.file,0,":objects"},"object");

    if (p.problem_htn.problem_class.empty()) {
      report(top, "the problem has no (:htn ...) block; this planner needs an initial task network");
    }
    else {
      Where w{lines.file,lines.line(p.problem_htn),":htn"};
      auto scope = scope_of(parameters(p.problem_htn.parameters,w));
      task_network(p.problem_htn.task_network,scope,w,lines);
    }

    Where iw{lines.file,0,":init"};
    for (auto const& f : p.init) {
      std::string text = show(f.predicate,f.args);
      if (f.is_negative) {
        report(iw, "fact (not " + text + "): :init lists only the facts that are true");
      }
      auto type_of = [&](Name const& n) -> std::string {
        auto it = constants.find(n);
        if (it == constants.end()) {
          report(iw, "fact " + text + " uses " + n +
                     ", which is not a declared object or constant" + suggest(n,constants));
          return "";
        }
        return it->second;
      };
      Signature const* sig = predicate_signature(f.predicate,f.args.size(),iw,"fact " + text);
      if (!sig) {
        for (auto const& a : f.args) {
          type_of(a);
        }
        continue;
      }
      check_args("predicate",f.predicate,*sig,f.args,type_of,iw,"fact " + text);
    }

    sentence(p.goal,Scope{},Where{lines.file,0,":goal"});
  }
};

} // namespace hddl_check

//Checks a domain, and a problem against it when one is given. Throws an
//HDDLError listing every problem found; returns normally if there are none.
inline void validate_hddl(ast::Domain const& d, SourceLines const& dom_lines,
                          ast::Problem const* p = nullptr,
                          SourceLines const* prob_lines = nullptr) {
  hddl_check::Checker c;
  c.domain(d,dom_lines);
  if (p) {
    c.problem(*p,d,*prob_lines);
  }
  auto problems = c.sorted_problems();
  if (!problems.empty()) {
    throw HDDLError(problems);
  }
}
