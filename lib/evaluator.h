#pragma once

#include "kb.h"
#include "expr.h"
#include <algorithm>
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

//Variable -> the object bound to it, for the bindings fixed so far.
//
//Objects are held by id, and variables by a pointer to their name. The names
//belong to the query's own expression and parameter lists, which outlive it.
//This held both as strings, so every atom hashed each bound value back to an
//id to compare it with the fact index, and every copy of an Env -- one per
//candidate binding -- copied every name and value (planner_doc.md 8.24). Names
//and ids convert once, at the start and end of a query.
//
//A flat vector looked up by linear scan: an Env is copied far more often than
//it is read, and it holds a handful of entries -- a method's parameters.
//`text` is set only for a value that names no object (object < 0), which
//can only be compared as text.
struct Slot {
  std::string const* name;
  int object;
  std::string const* text = nullptr;
};
using Env = std::vector<Slot>;

inline Slot const* env_slot(Env const& env, std::string const& name) {
  for (auto const& s : env) {
    if (*s.name == name) {
      return &s;
    }
  }
  return nullptr;
}

inline int const* env_find(Env const& env, std::string const& name) {
  auto const* s = env_slot(env,name);
  return s ? &s->object : nullptr;
}

inline void env_set(Env& env, std::string const& name, int object,
                    std::string const* text = nullptr) {
  for (auto& s : env) {
    if (*s.name == name) {
      s.object = object;
      s.text = text;
      return;
    }
  }
  env.push_back({&name,object,text});
}

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

//What a term denotes under env, as an object id; kUnbound for a variable not
//yet bound, kNoObject for a constant that names no object. A constant that
//names no object can never match a fact, as before, when it was compared as a
//string that no fact's argument equalled.
constexpr int kUnbound = -1;
constexpr int kNoObject = -2;

inline int value_of(KnowledgeBase& kb, expr::Term const& t, Env const& env) {
  if (!t.is_variable) {
    int oid = kb.object_id(t.name);
    return oid < 0 ? kNoObject : oid;
  }
  auto const* v = env_find(env,t.name);
  return v ? *v : kUnbound;
}

//The text of a bound term that names no object, for comparing two such.
inline std::string const* text_of(expr::Term const& t, Env const& env) {
  if (!t.is_variable) {
    return &t.name;
  }
  auto const* s = env_slot(env,t.name);
  return s ? s->text : nullptr;
}

inline bool ground(expr::Ptr const& e, Env const& env) {
  if (!e) {
    return true;
  }
  for (auto const& a : e->args) {
    if (a.is_variable && !env_find(env,a.name)) {
      return false;
    }
  }
  for (auto const& c : e->children) {
    if (!ground(c,env)) {
      return false;
    }
  }
  return true;
}

//Every way of extending env so that e holds. An empty result means the
//expression is false under every extension; a result containing env unchanged
//means it holds and bound nothing new.
//
//Returns nullopt if the expression turned out not to be evaluable here -- a
//negation still containing free variables, for instance, which `supported`
//cannot rule out up front because it depends on the binding order.
inline std::optional<std::vector<Env>> solve(KnowledgeBase& kb,
                                             expr::Ptr const& e,
                                             Env const& env);

inline std::optional<std::vector<Env>> solve_atom(KnowledgeBase& kb,
                                                  expr::Ptr const& e,
                                                  Env const& env) {
  std::vector<Env> out;
  int pid = kb.predicate_id(e->predicate);
  //A zero-arity predicate is a propositional atom: it holds if its relation is
  //non-empty.
  if (e->args.empty()) {
    if (pid >= 0 && !kb.relation_of(pid).empty()) {
      out.push_back(env);
    }
    return out;
  }
  if (pid < 0 || kb.arity_of(pid) != e->args.size()) {
    return out;
  }
  size_t ar = e->args.size();
  auto const& rel = kb.relation_of(pid);

  //Resolve each argument against the incoming env once rather than per tuple.
  //A negative entry means the position is free and this atom will bind it.
  std::vector<int> want(ar,kUnbound);
  for (size_t i = 0; i < ar; i++) {
    want[i] = value_of(kb,e->args[i],env);
    if (want[i] == kNoObject) {
      return out;   //a constant no object matches: nothing can hold
    }
  }

  for (size_t off = 0; off + ar <= rel.size(); off += ar) {
    bool ok = true;
    //Two passes, so that `env` is copied only for a tuple that actually
    //matches. Most candidates do not.
    for (size_t i = 0; i < ar && ok; i++) {
      if (want[i] >= 0) {
        ok = (rel[off+i] == want[i]);
      }
      else {
        //A variable repeated inside one atom -- (road ?l ?l) -- must agree
        //with its own earlier position.
        for (size_t j = 0; j < i; j++) {
          if (want[j] < 0 && e->args[j].name == e->args[i].name &&
              rel[off+j] != rel[off+i]) {
            ok = false;
          }
        }
      }
    }
    if (!ok) {
      continue;
    }
    Env next;
    next.reserve(env.size() + ar);
    next = env;
    for (size_t i = 0; i < ar; i++) {
      if (want[i] < 0) {
        env_set(next,e->args[i].name,rel[off+i]);
      }
    }
    out.push_back(std::move(next));
  }
  return out;
}

inline std::optional<std::vector<Env>> solve(KnowledgeBase& kb,
                                             expr::Ptr const& e,
                                             Env const& env) {
  if (!e) {
    return std::vector<Env>{env};
  }
  switch (e->kind) {
    case expr::Kind::Atom:
      return solve_atom(kb,e,env);

    case expr::Kind::Equals:
    case expr::Kind::NotEquals: {
      int l = value_of(kb,e->args[0],env);
      int r = value_of(kb,e->args[1],env);
      if (l != kUnbound && r != kUnbound) {
        //Two constants naming no object compare by name, as they did when
        //everything was a string.
        bool same;
        if (l == kNoObject || r == kNoObject) {
          auto const* lt = text_of(e->args[0],env);
          auto const* rt = text_of(e->args[1],env);
          same = (l == r) && lt && rt && *lt == *rt;
        }
        else {
          same = (l == r);
        }
        bool want = (e->kind == expr::Kind::Equals);
        if (same == want) {
          return std::vector<Env>{env};
        }
        return std::vector<Env>{};
      }
      if (e->kind == expr::Kind::Equals && (l != kUnbound || r != kUnbound)) {
        //One side unbound: an equality binds it -- unless the bound side names
        //no object, which no variable can take.
        bool left = (l != kUnbound);
        int v = left ? l : r;
        std::string const* text = (v == kNoObject)
                                ? text_of(left ? e->args[0] : e->args[1],env) : nullptr;
        Env next = env;
        env_set(next,left ? e->args[1].name : e->args[0].name,v,text);
        return std::vector<Env>{next};
      }
      //A disequality between unbound variables constrains nothing yet, and
      //both sides unbound in an equality would need a domain to range over.
      return std::nullopt;
    }

    case expr::Kind::Not: {
      if (!ground(e->children[0],env)) {
        //Negation as failure only makes sense once everything is bound.
        return std::nullopt;
      }
      auto inner = solve(kb,e->children[0],env);
      if (!inner) {
        return std::nullopt;
      }
      if (inner->empty()) {
        return std::vector<Env>{env};
      }
      return std::vector<Env>{};
    }

    case expr::Kind::And: {
      //Fold left to right: each conjunct is solved under the bindings the ones
      //before it produced, which is what makes this a join rather than a
      //filtered cross product. Positive atoms are taken first so that they
      //bind variables before a negation or disequality needs them. Two passes
      //over the children in place; within each pass they keep their written
      //order.
      auto binds = [](expr::Ptr const& c) {
        return c && (c->kind == expr::Kind::Atom || c->kind == expr::Kind::Equals);
      };
      std::vector<Env> envs{env};
      for (int pass = 0; pass < 2 && !envs.empty(); pass++) {
        for (auto const& c : e->children) {
          if (binds(c) != (pass == 0)) {
            continue;
          }
          std::vector<Env> next;
          for (auto const& cur : envs) {
            auto got = solve(kb,c,cur);
            if (!got) {
              return std::nullopt;
            }
            next.insert(next.end(),std::make_move_iterator(got->begin()),
                                   std::make_move_iterator(got->end()));
          }
          envs = std::move(next);
          //Stop at the first conjunct nothing satisfies. This matters for more
          //than speed: a later negation with free variables would make solve()
          //return nullopt and send a query that is already false to Z3.
          if (envs.empty()) {
            break;
          }
        }
      }
      return envs;
    }

    case expr::Kind::Or: {
      std::vector<Env> out;
      for (auto const& c : e->children) {
        auto got = solve(kb,c,env);
        if (!got) {
          return std::nullopt;
        }
        out.insert(out.end(),std::make_move_iterator(got->begin()),
                             std::make_move_iterator(got->end()));
      }
      return out;
    }

    default:
      return std::nullopt;
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
                                               Binding const& fixed) {
  if (!supported(e)) {
    return std::nullopt;
  }
  Env env;
  env.reserve(params.size() + fixed.size());
  for (auto const& [name,value] : fixed) {
    //A parameter "pinned" to its own name is not pinned at all. When a
    //method's subtask mentions a parameter the method does not bind,
    //MethodDef::apply_binding falls back to using the parameter's *name* as
    //its value (the "__CONST__" case), so the grounded task carries
    //`room` where an object should be. The string path then emits
    //`(= room room)`, which Z3 reads as a tautology and ignores, leaving the
    //variable free to be enumerated. Binding it to the literal "room" instead
    //would silently find nothing. Reproduce Z3's reading.
    //
    //This also means neither engine can tell that case apart from a parameter
    //legitimately bound to an object of the same name -- see the note in
    //planner_doc.md 4.2.
    if (name == value) {
      continue;
    }
    //A value that names no object is kept as text: every atom then fails on
    //it, and an equality compares it by name, as when everything was a string.
    int oid = kb.object_id(value);
    if (oid < 0) {
      env_set(env,name,detail::kNoObject,&value);
    }
    else {
      env_set(env,name,oid);
    }
  }

  auto solved = detail::solve(kb,e,env);
  if (!solved) {
    return std::nullopt;
  }

  std::vector<Binding> out;
  //Duplicates dropped, first occurrence kept: the order of the result feeds
  //the order of successors and so the random stream. The set holds indices
  //into `ids`, hashed and compared by value.
  //Each value as its object id, with its text for one that names no object.
  using Value = std::pair<int,std::string const*>;
  std::vector<std::vector<Value>> ids;
  auto hash_of = [&ids](size_t i) {
    size_t h = 0;
    for (auto const& [v,text] : ids[i]) {
      h = h * 1000003u ^ std::hash<int>{}(v);
    }
    return h;
  };
  auto same = [&ids](size_t a, size_t b) {
    if (ids[a].size() != ids[b].size()) return false;
    for (size_t k = 0; k < ids[a].size(); k++) {
      auto const& [va,ta] = ids[a][k];
      auto const& [vb,tb] = ids[b][k];
      if (va != vb || (va < 0 && !(ta && tb && *ta == *tb))) return false;
    }
    return true;
  };
  std::unordered_set<size_t,decltype(hash_of),decltype(same)> seen(8,hash_of,same);
  for (auto const& base : *solved) {
    //Any parameter the expression left unbound ranges over its whole type.
    std::vector<Env> envs{base};
    for (auto const& [name,type] : params) {
      if (envs.empty()) {
        break;
      }
      if (env_find(base,name)) {
        continue;
      }
      std::vector<Env> widened;
      for (auto const& cur : envs) {
        if (env_find(cur,name)) {
          widened.push_back(cur);
          continue;
        }
        for (int oid : kb.type_extension(type)) {
          Env next = cur;
          next.push_back({&name,oid});
          widened.push_back(std::move(next));
        }
      }
      envs = std::move(widened);
    }
    for (auto const& full : envs) {
      std::vector<Value> b;
      b.reserve(params.size());
      for (auto const& [name,type] : params) {
        auto const* v = env_slot(full,name);
        if (!v) {
          //Should not happen once the widening above has run, but a partial
          //binding would be silently wrong rather than loudly wrong, so drop
          //the whole query to the fallback instead of guessing.
          return std::nullopt;
        }
        b.push_back({v->object,v->text});
      }
      ids.push_back(std::move(b));
      if (!seen.insert(ids.size() - 1).second) {
        ids.pop_back();
      }
    }
  }
  out.reserve(ids.size());
  for (auto const& b : ids) {
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

// Boolean form, mirroring `ask_any`: is there any such binding?
inline std::optional<bool> ask_any(KnowledgeBase& kb,
                                   expr::Ptr const& e,
                                   Binding const& params,
                                   Binding const& fixed) {
  auto got = ask(kb,e,params,fixed);
  if (!got) {
    return std::nullopt;
  }
  return !got->empty();
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
                                             char const* what) {
  auto direct = ask(kb,ast,params,fixed);

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
