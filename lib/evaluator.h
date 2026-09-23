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

//Variable name -> value, for the bindings fixed so far.
//
//A flat vector rather than a hash map, and looked up by linear scan. An Env is
//*copied* far more often than it is read -- once per candidate binding, and
//the search generates a great many of those -- so what matters is the cost of
//copying one, not the asymptotics of a lookup. A hash map copy allocates a
//bucket array plus a node per entry; this copies as a single allocation and a
//memcpy, because every variable and object name in these domains fits in
//libc++'s small-string buffer (the longest measured is 17 characters against a
//threshold of 22), so the strings carry no separate storage of their own.
//
//The scan is over a handful of entries -- a method's parameter list -- which
//is well inside the range where a linear scan beats hashing anyway.
using Env = std::vector<std::pair<std::string,std::string>>;

//The value bound to `name`, or nullptr. Returns a pointer into the Env rather
//than a copy: value_of is called per argument per atom, and a returned
//std::string would be a copy each time.
inline std::string const* env_find(Env const& env, std::string const& name) {
  for (auto const& [n,v] : env) {
    if (n == name) {
      return &v;
    }
  }
  return nullptr;
}

inline void env_set(Env& env, std::string const& name, std::string const& value) {
  for (auto& [n,v] : env) {
    if (n == name) {
      v = value;
      return;
    }
  }
  env.emplace_back(name,value);
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

//The value a term denotes under env, or nullopt when it is a variable that is
//still unbound. Constants denote themselves.
//Constants denote themselves, so the pointer is into the term. Nullptr means a
//variable that is still unbound.
inline std::string const* value_of(expr::Term const& t, Env const& env) {
  if (!t.is_variable) {
    return &t.name;
  }
  return env_find(env,t.name);
}

inline bool ground(expr::Ptr const& e, Env const& env) {
  if (!e) {
    return true;
  }
  for (auto const& a : e->args) {
    if (!value_of(a,env)) {
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

  //Resolve each argument against the incoming env ONCE rather than per tuple.
  //A negative entry means the position is free and this atom will bind it.
  //Everything below then compares integers.
  std::vector<int> want(ar,-1);
  for (size_t i = 0; i < ar; i++) {
    auto const* v = value_of(e->args[i],env);
    if (v) {
      want[i] = kb.object_id(*v);
      if (want[i] < 0) {
        return out;   //a constant no object matches: nothing can hold
      }
    }
  }

  for (size_t off = 0; off + ar <= rel.size(); off += ar) {
    bool ok = true;
    //Two passes, so that `env` is copied only for a tuple that actually
    //matches. Copying it per candidate tuple was itself a per-tuple
    //allocation, and most candidates do not match.
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
    Env next = env;
    for (size_t i = 0; i < ar; i++) {
      if (want[i] < 0) {
        env_set(next,e->args[i].name,kb.object_name(rel[off+i]));
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
      auto const* l = value_of(e->args[0],env);
      auto const* r = value_of(e->args[1],env);
      if (l && r) {
        bool same = (*l == *r);
        bool want = (e->kind == expr::Kind::Equals);
        if (same == want) {
          return std::vector<Env>{env};
        }
        return std::vector<Env>{};
      }
      if (e->kind == expr::Kind::Equals && (l || r)) {
        //One side unbound: an equality binds it.
        Env next = env;
        if (l) {
          env_set(next,e->args[1].name,*l);
        }
        else {
          env_set(next,e->args[0].name,*r);
        }
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
      //bind variables before a negation or disequality needs them.
      std::vector<expr::Ptr> ordered;
      for (auto const& c : e->children) {
        if (c && (c->kind == expr::Kind::Atom || c->kind == expr::Kind::Equals)) {
          ordered.push_back(c);
        }
      }
      for (auto const& c : e->children) {
        if (!(c && (c->kind == expr::Kind::Atom || c->kind == expr::Kind::Equals))) {
          ordered.push_back(c);
        }
      }
      std::vector<Env> envs{env};
      for (auto const& c : ordered) {
        std::vector<Env> next;
        for (auto const& cur : envs) {
          auto got = solve(kb,c,cur);
          if (!got) {
            return std::nullopt;
          }
          next.insert(next.end(),got->begin(),got->end());
        }
        envs = std::move(next);
        if (envs.empty()) {
          break;
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
        out.insert(out.end(),got->begin(),got->end());
      }
      return out;
    }

    default:
      return std::nullopt;
  }
}

//A stable key for deduplication, over the parameters the caller asked about.
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
    env_set(env,name,value);
  }

  auto solved = detail::solve(kb,e,env);
  if (!solved) {
    return std::nullopt;
  }

  std::vector<Binding> out;
  std::unordered_set<std::string> seen;
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
          next.emplace_back(name,kb.object_name(oid));
          widened.push_back(std::move(next));
        }
      }
      envs = std::move(widened);
    }
    for (auto const& full : envs) {
      Binding b;
      b.reserve(params.size());
      bool complete = true;
      for (auto const& [name,type] : params) {
        auto const* v = env_find(full,name);
        if (!v) {
          complete = false;
          break;
        }
        b.push_back({name,*v});
      }
      if (!complete) {
        //Should not happen once the widening above has run, but a partial
        //binding would be silently wrong rather than loudly wrong, so drop the
        //whole query to the fallback instead of guessing.
        return std::nullopt;
      }
      if (seen.insert(detail::key_of(b)).second) {
        out.push_back(std::move(b));
      }
    }
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

inline std::vector<Binding> solve_query(KnowledgeBase& kb,
                                        expr::Ptr const& ast,
                                        std::string const& smt,
                                        Binding const& params,
                                        Binding const& fixed,
                                        char const* what) {
  auto direct = ask(kb,ast,params,fixed);

#ifdef HTN_DIFFERENTIAL_EVAL
  if (direct) {
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
  Binding mutable_params = params;
  if (smt == "__NONE__") {
    return kb.ask("",mutable_params);
  }
  return kb.ask(smt,mutable_params);
}

} // namespace eval
