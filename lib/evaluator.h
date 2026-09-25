#pragma once

#include "kb.h"
#include "expr.h"
#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Direct evaluation of preconditions against the fact base, without a solver.
//
// The queries the planner asks are conjunctive queries with negation and
// equality over a finite, fully known set of ground facts. Sending them to Z3
// costs about 3 ms each, of which roughly 2% is spent solving and the rest on
// building a context, interning symbols and re-parsing the state as SMT-LIB
// text (planner_doc.md 6.1). Answering them here is a join over indexed
// relations.
//
// This covers the fragment the shipped domains actually use. Anything outside
// it -- quantifiers, implication, a negation whose variables are not all bound
// by the time it is reached -- returns nullopt, and the caller falls back to
// the Z3 path. The fallback is what makes this safe to land: no expressiveness
// is lost, only speed is gained, and only where the evaluator is sure.
//
// Semantics follow the SMT encoding this replaces (planner_doc.md 2.2): the
// closed-world assumption, so an atom is true exactly on the tuples listed in
// its relation, and negation is failure to prove.
namespace eval {

//Same shape as typedefs.h's Params and Args, spelled out here because this
//header sits below typedefs.h: it cannot include it without a cycle.
using Binding = std::vector<std::pair<std::string,std::string>>;

//Can this expression be evaluated directly? Checked once up front so the
//caller can decide which engine to use before doing any work.
inline bool supported(expr::Ptr const& e) {
  if (!e) {
    return true;                       //absent precondition: trivially true
  }
  switch (e->kind) {
    case expr::Kind::Atom:
    case expr::Kind::Equals:
    case expr::Kind::NotEquals:
      return true;
    case expr::Kind::Not:
    case expr::Kind::And:
    case expr::Kind::Or:
      for (auto const& c : e->children) {
        if (!supported(c)) {
          return false;
        }
      }
      return true;
    case expr::Kind::Imply:
    case expr::Kind::Forall:
    case expr::Kind::Exists:
      //Deliberately unsupported. No shipped domain uses them, and getting
      //quantifier scoping right is not worth guessing at -- Z3 already does it.
      return false;
  }
  return false;
}

namespace detail {

//A search is a depth-first walk over the expression with one binding array,
//indexed by the slots expr::number_variables gave the variables. A variable is
//set when an atom or equality binds it, the rest of the expression is searched
//through a continuation, and the variable is unset on the way back. Nothing is
//allocated per candidate binding.
//
//This replaced a breadth-first form that built, for every subexpression, the
//vector of every binding extending the incoming one, and copied a binding per
//candidate (planner_doc.md 8.25). The two produce the same bindings in the
//same order -- the breadth-first fold over a conjunction, taken level by
//level, lists its results in exactly the order a depth-first walk reaches
//them -- and the order matters: it becomes the order of successors, and so
//the random stream.
constexpr int kUnbound = -1;
constexpr int kNoObject = -2;   //a value that names no object; see Search::text

struct Search {
  KnowledgeBase& kb;
  std::vector<int> val;                   //per slot: an object id, or the above
  std::vector<std::string const*> text;   //per slot, for a kNoObject value: its text
  //The expression turned out not to be evaluable here -- a negation or a
  //disequality reached before its variables were bound. The query goes to Z3.
  bool abort = false;
  //An existence check (a negation's, or ask_any's) has its witness: unwind.
  bool halt = false;

  bool stop() const { return abort || halt; }
};

//A continuation: what to do with each binding found. A non-owning reference
//to a callable, so that nested continuations do not nest types -- a template
//parameter would, once per level of the expression, without bound.
class Cont {
public:
  template <class F>
  Cont(F& f) : obj_(&f), call_([](void* o) { (*static_cast<F*>(o))(); }) {}
  void operator()() const { call_(obj_); }
private:
  void* obj_;
  void (*call_)(void*);
};

inline int value_of(Search& s, expr::Term const& t) {
  if (!t.is_variable) {
    int oid = s.kb.object_id(t.name);
    return oid < 0 ? kNoObject : oid;
  }
  return s.val[t.slot];
}

//The text of a bound term that names no object, for comparing two such.
inline std::string const* text_of(Search& s, expr::Term const& t) {
  return t.is_variable ? s.text[t.slot] : &t.name;
}

inline bool ground(Search& s, expr::Node const& e) {
  for (auto const& a : e.args) {
    if (a.is_variable && s.val[a.slot] == kUnbound) {
      return false;
    }
  }
  for (auto const& c : e.children) {
    if (c && !ground(s, *c)) {
      return false;
    }
  }
  return true;
}

inline void solve(Search& s, expr::Node const& e, Cont const& k);

inline void solve_atom(Search& s, expr::Node const& e, Cont const& k) {
  int pid = s.kb.predicate_id(e.predicate);
  auto const& rel = s.kb.relation_of(pid);
  //A zero-arity predicate is a propositional atom: it holds if its relation is
  //non-empty.
  if (e.args.empty()) {
    if (pid >= 0 && !rel.empty()) {
      k();
    }
    return;
  }
  size_t ar = e.args.size();
  if (pid < 0 || s.kb.arity_of(pid) != ar) {
    return;
  }
  //Each argument resolved once rather than per tuple; kUnbound marks a
  //position this atom will bind.
  int want_small[8];
  std::vector<int> want_large;
  int* want = want_small;
  if (ar > 8) {
    want_large.resize(ar);
    want = want_large.data();
  }
  for (size_t i = 0; i < ar; i++) {
    want[i] = value_of(s, e.args[i]);
    if (want[i] == kNoObject) {
      return;   //a value naming no object matches no fact
    }
  }
  for (size_t off = 0; off + ar <= rel.size(); off += ar) {
    bool ok = true;
    for (size_t i = 0; i < ar && ok; i++) {
      if (want[i] >= 0) {
        ok = (rel[off+i] == want[i]);
      }
      else {
        //A variable repeated inside one atom -- (road ?l ?l) -- must agree
        //with its own earlier position.
        for (size_t j = 0; j < i; j++) {
          if (want[j] < 0 && e.args[j].slot == e.args[i].slot && rel[off+j] != rel[off+i]) {
            ok = false;
          }
        }
      }
    }
    if (!ok) {
      continue;
    }
    for (size_t i = 0; i < ar; i++) {
      if (want[i] < 0) {
        s.val[e.args[i].slot] = rel[off+i];
      }
    }
    k();
    for (size_t i = 0; i < ar; i++) {
      if (want[i] < 0) {
        s.val[e.args[i].slot] = kUnbound;
      }
    }
    if (s.stop()) {
      return;
    }
  }
}

inline void solve_compare(Search& s, expr::Node const& e, Cont const& k) {
  bool equals = (e.kind == expr::Kind::Equals);
  int l = value_of(s, e.args[0]);
  int r = value_of(s, e.args[1]);
  if (l != kUnbound && r != kUnbound) {
    //Two values naming no object compare by their text.
    bool same;
    if (l == kNoObject || r == kNoObject) {
      auto const* lt = text_of(s, e.args[0]);
      auto const* rt = text_of(s, e.args[1]);
      same = (l == r) && lt && rt && *lt == *rt;
    }
    else {
      same = (l == r);
    }
    if (same == equals) {
      k();
    }
    return;
  }
  if (equals && (l != kUnbound || r != kUnbound)) {
    //One side unbound: an equality binds it.
    bool left = (l != kUnbound);
    expr::Term const& from = left ? e.args[0] : e.args[1];
    int slot = (left ? e.args[1] : e.args[0]).slot;
    s.val[slot] = left ? l : r;
    s.text[slot] = (s.val[slot] == kNoObject) ? text_of(s, from) : nullptr;
    k();
    s.val[slot] = kUnbound;
    s.text[slot] = nullptr;
    return;
  }
  //A disequality with an unbound side constrains nothing yet, and an equality
  //between two unbound variables would need a domain to range over.
  s.abort = true;
}

inline void solve_not(Search& s, expr::Node const& e, Cont const& k) {
  if (!e.children[0] || !ground(s, *e.children[0])) {
    //Negation as failure only makes sense once everything is bound.
    s.abort = true;
    return;
  }
  bool found = false;
  auto witness = [&] { found = true; s.halt = true; };
  Cont w(witness);
  solve(s, *e.children[0], w);
  s.halt = false;
  if (!s.abort && !found) {
    k();
  }
}

//Positive atoms and equalities are taken first, so that they bind variables
//before a negation or disequality needs them: `pos` runs over the children
//twice, taking in the first pass the ones that bind and in the second the
//rest, each pass in written order.
inline bool binds(expr::Ptr const& c) {
  return c && (c->kind == expr::Kind::Atom || c->kind == expr::Kind::Equals);
}

inline void solve_and(Search& s, expr::Node const& e, size_t pos, Cont const& k) {
  size_t n = e.children.size();
  while (pos < 2 * n && binds(e.children[pos % n]) != (pos < n)) {
    pos++;
  }
  if (pos == 2 * n) {
    k();
    return;
  }
  auto rest = [&] { solve_and(s, e, pos + 1, k); };
  Cont r(rest);
  auto const& c = e.children[pos % n];
  if (!c) {
    rest();
    return;
  }
  solve(s, *c, r);
}

inline void solve(Search& s, expr::Node const& e, Cont const& k) {
  switch (e.kind) {
    case expr::Kind::Atom:
      solve_atom(s, e, k);
      return;
    case expr::Kind::Equals:
    case expr::Kind::NotEquals:
      solve_compare(s, e, k);
      return;
    case expr::Kind::Not:
      solve_not(s, e, k);
      return;
    case expr::Kind::And:
      solve_and(s, e, 0, k);
      return;
    case expr::Kind::Or:
      for (auto const& c : e.children) {
        if (c) {
          solve(s, *c, k);
        }
        else {
          k();
        }
        if (s.stop()) {
          return;
        }
      }
      return;
    default:
      s.abort = true;
      return;
  }
}

//A stable key for comparing results as sets (the differential build).
inline std::string key_of(Binding const& b) {
  std::string k;
  for (auto const& [n,v] : b) {
    k += n;
    k += '=';
    k += v;
    k += ';';
  }
  return k;
}

} // namespace detail

//Which slot each parameter and each pinned argument occupies: the
//expression's own slot for a variable it mentions, a new one after them for a
//parameter it does not, and -1 for a pinned name it neither mentions nor asks
//about. An action or method computes this once, since its expression and
//parameter lists are fixed; working it out per query was a string comparison
//per parameter per slot (planner_doc.md 8.25).
struct SlotMap {
  std::vector<int> param_slot;
  std::vector<int> fixed_slot;
  int total = 0;
};

inline SlotMap slot_map(expr::Ptr const& e, Binding const& params,
                        std::vector<std::string> const& fixed_names) {
  expr::number_variables(e);
  int n = e ? e->nslots : 0;
  SlotMap m;
  std::vector<std::string const*> extra;
  auto slot_of = [&](std::string const& name) -> int {
    if (e) {
      for (int i = 0; i < n; i++) {
        if (e->slot_names[i] == name) return i;
      }
    }
    for (size_t i = 0; i < extra.size(); i++) {
      if (*extra[i] == name) return n + (int)i;
    }
    return -1;
  };
  for (auto const& [name,type] : params) {
    int slot = slot_of(name);
    if (slot < 0) {
      slot = n + (int)extra.size();
      extra.push_back(&name);
    }
    m.param_slot.push_back(slot);
  }
  for (auto const& name : fixed_names) {
    m.fixed_slot.push_back(slot_of(name));
  }
  m.total = n + (int)extra.size();
  return m;
}

namespace detail {

//Sets up a search: sizes the binding array and binds what `fixed` pins, using
//`map` if the caller has one that fits, and computing one otherwise.
inline SlotMap const& prepare(Search& s, expr::Ptr const& e, Binding const& params,
                              Binding const& fixed, SlotMap const* map, SlotMap& scratch) {
  if (!map || map->param_slot.size() != params.size() ||
      map->fixed_slot.size() != fixed.size()) {
    std::vector<std::string> names;
    names.reserve(fixed.size());
    for (auto const& f : fixed) names.push_back(f.first);
    scratch = slot_map(e,params,names);
    map = &scratch;
  }
  s.val.assign(map->total, kUnbound);
  s.text.assign(map->total, nullptr);
  for (size_t i = 0; i < fixed.size(); i++) {
    auto const& [name,value] = fixed[i];
    //A parameter "pinned" to its own name is not pinned at all. When a
    //method's subtask mentions a parameter the method does not bind,
    //MethodDef::apply_binding falls back to using the parameter's *name* as
    //its value, so the grounded task carries `room` where an object should
    //be. The string path then emits `(= room room)`, which Z3 reads as a
    //tautology and ignores, leaving the variable free to be enumerated.
    //Reproduce Z3's reading. (planner_doc.md 4.2 has the rest.)
    int slot = map->fixed_slot[i];
    if (slot < 0 || name == value) {
      continue;
    }
    //A value that names no object keeps its text: every atom then fails on
    //it, and an equality compares it by name.
    int oid = s.kb.object_id(value);
    s.val[slot] = oid < 0 ? kNoObject : oid;
    s.text[slot] = oid < 0 ? &value : nullptr;
  }
  return *map;
}

} // namespace detail

// Every binding of `params` that, extended with `fixed`, satisfies `e`.
//
// Mirrors `KnowledgeBase::ask(expr, params)`, including the part that is easy
// to overlook: a parameter the expression never mentions still has to be
// enumerated over its type's extension, because the SMT encoding declares every
// parameter as a constant constrained by its type and Z3 therefore produces a
// model for each. `fixed` supplies the parameters the caller has already
// pinned -- what the string path expresses as a prefix of `(= param value)`
// conjuncts.
//
// Returns nullopt when the expression is outside the supported fragment, in
// which case the caller should use the Z3 path.
inline std::optional<std::vector<Binding>> ask(KnowledgeBase& kb,
                                               expr::Ptr const& e,
                                               Binding const& params,
                                               Binding const& fixed,
                                               SlotMap const* map = nullptr) {
  if (!supported(e)) {
    return std::nullopt;
  }
  detail::Search s{kb, {}, {}};
  SlotMap scratch;
  auto const& prep = detail::prepare(s, e, params, fixed, map, scratch);

  //Each result as its values' object ids, with the text of one that names no
  //object. Duplicates are dropped and the first occurrence kept, in place:
  //the order of the result feeds the order of successors and so the random
  //stream. The set holds indices into `found`, hashed and compared by value.
  using Value = std::pair<int,std::string const*>;
  std::vector<std::vector<Value>> found;
  auto hash_of = [&found](size_t i) {
    size_t h = 0;
    for (auto const& [v,text] : found[i]) {
      h = h * 1000003u ^ std::hash<int>{}(v);
    }
    return h;
  };
  auto same = [&found](size_t a, size_t b) {
    if (found[a].size() != found[b].size()) return false;
    for (size_t k = 0; k < found[a].size(); k++) {
      auto const& [va,ta] = found[a][k];
      auto const& [vb,tb] = found[b][k];
      if (va != vb || (va < 0 && !(ta && tb && *ta == *tb))) return false;
    }
    return true;
  };
  std::unordered_set<size_t,decltype(hash_of),decltype(same)> seen(8,hash_of,same);

  //Each solution of the expression, then every way of giving the parameters
  //it left unbound an object of their type, the first parameter outermost.
  std::function<void(size_t)> widen = [&](size_t i) {
    if (i == params.size()) {
      std::vector<Value> b;
      b.reserve(params.size());
      for (int slot : prep.param_slot) {
        b.push_back({s.val[slot], s.text[slot]});
      }
      found.push_back(std::move(b));
      if (!seen.insert(found.size() - 1).second) {
        found.pop_back();
      }
      return;
    }
    int slot = prep.param_slot[i];
    if (s.val[slot] != detail::kUnbound) {
      widen(i + 1);
      return;
    }
    for (int oid : kb.type_extension(params[i].second)) {
      s.val[slot] = oid;
      widen(i + 1);
    }
    s.val[slot] = detail::kUnbound;
  };
  auto each = [&] { widen(0); };
  detail::Cont k(each);
  if (e) {
    detail::solve(s, *e, k);
  }
  else {
    each();
  }
  if (s.abort) {
    return std::nullopt;
  }

  std::vector<Binding> out;
  out.reserve(found.size());
  for (auto const& b : found) {
    Binding named;
    named.reserve(params.size());
    for (size_t i = 0; i < params.size(); i++) {
      auto const& [v,text] = b[i];
      named.push_back({params[i].first, v >= 0 ? kb.object_name(v) : *text});
    }
    out.push_back(std::move(named));
  }
  return out;
}

// Boolean form, mirroring `ask_any`: is there any such binding? Stops at the
// first. A solution leaves some parameters unbound, and there is a binding
// exactly when each of those has an object of its type.
inline std::optional<bool> ask_any(KnowledgeBase& kb,
                                   expr::Ptr const& e,
                                   Binding const& params,
                                   Binding const& fixed,
                                   SlotMap const* map = nullptr) {
  if (!supported(e)) {
    return std::nullopt;
  }
  detail::Search s{kb, {}, {}};
  SlotMap scratch;
  auto const& prep = detail::prepare(s, e, params, fixed, map, scratch);
  bool any = false;
  auto each = [&] {
    for (size_t i = 0; i < params.size(); i++) {
      if (s.val[prep.param_slot[i]] == detail::kUnbound &&
          kb.type_extension(params[i].second).empty()) {
        return;
      }
    }
    any = true;
    s.halt = true;
  };
  detail::Cont k(each);
  if (e) {
    detail::solve(s, *e, k);
  }
  else {
    each();
  }
  //A witness found before the search would have given up is still a witness:
  //the query holds.
  if (s.abort && !any) {
    return std::nullopt;
  }
  return any;
}

} // namespace eval

// --- engine selection -------------------------------------------------------
//
// One entry point for the planner: answer the query directly when the
// evaluator can, and hand it to Z3 when it cannot. Building the Z3 query is
// left to the caller, which already has the string form.
//
// Compiling with -DHTN_DIFFERENTIAL_EVAL runs *both* engines on every query the
// direct path claims and requires them to agree as sets. That is the check
// that the direct evaluator reproduces the closed-world semantics of the SMT
// encoding, which is the current definition of correctness. It is far too slow
// to ship -- it does strictly more work than the old code -- so it is a build
// flag, used to qualify a change and then turned off.
namespace eval {

inline std::string canonical(std::vector<Binding> bs) {
  std::vector<std::string> keys;
  keys.reserve(bs.size());
  for (auto const& b : bs) {
    keys.push_back(detail::key_of(b));
  }
  std::sort(keys.begin(),keys.end());
  std::string out;
  for (auto const& k : keys) {
    out += k;
    out += '|';
  }
  return out;
}

//smt_of produces the SMT-LIB text of the query, and is called only when that
//text is actually needed: when the direct evaluator declines the query, or in
//the differential build. The callers used to build it eagerly -- a `(= param
//value)` per argument followed by the whole precondition text -- on every
//action and method application, and on the default build it was almost never
//read. Taking a callable rather than the string moves that cost onto the path
//that uses it.
template <class SmtFn>
inline std::vector<Binding> solve_query_lazy(KnowledgeBase& kb,
                                             expr::Ptr const& ast,
                                             SmtFn&& smt_of,
                                             Binding const& params,
                                             Binding const& fixed,
                                             char const* what,
                                             SlotMap const* map = nullptr) {
  auto direct = ask(kb,ast,params,fixed,map);

#ifdef HTN_DIFFERENTIAL_EVAL
  if (direct) {
    std::string smt = smt_of();
    Binding mutable_params = params;
    auto reference = smt == "__NONE__" ? kb.ask("",mutable_params)
                                       : kb.ask(smt,mutable_params);
    if (canonical(*direct) != canonical(reference)) {
      throw std::logic_error(std::string("evaluator disagrees with Z3 on ")+what+
                             "\n  query: "+smt+
                             "\n  direct: "+canonical(*direct)+
                             "\n  z3:     "+canonical(reference));
    }
  }
#else
  (void)what;
#endif

  if (direct) {
    return *direct;
  }
  std::string smt = smt_of();
  Binding mutable_params = params;
  if (smt == "__NONE__") {
    return kb.ask("",mutable_params);
  }
  return kb.ask(smt,mutable_params);
}

//The eager form, for callers that have the text anyway -- the effect paths,
//which build it alongside the structured form and are cold.
inline std::vector<Binding> solve_query(KnowledgeBase& kb,
                                        expr::Ptr const& ast,
                                        std::string const& smt,
                                        Binding const& params,
                                        Binding const& fixed,
                                        char const* what) {
  return solve_query_lazy(kb,ast,[&smt]() { return smt; },params,fixed,what);
}

} // namespace eval
