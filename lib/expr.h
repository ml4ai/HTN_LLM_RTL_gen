#pragma once

#include <memory>
#include <string>
#include <vector>

// A small expression IR for preconditions and effect conditions.
//
// The loader parses HDDL into a Boost.Spirit AST and then flattens every
// precondition to an SMT-LIB *string* (`sentence_to_SMT`), which is what
// `KnowledgeBase::ask` hands to Z3. That throws away the structure, so
// evaluating a precondition without a solver is impossible: there is nothing
// left to walk. This IR is that structure, kept alongside the string.
//
// It is deliberately not the parser's AST. Owning a small type here keeps the
// evaluator independent of Spirit's variant layout and of position-tracking
// state it has no use for, and lets the loader record things the SMT text
// cannot express -- most importantly, whether an argument is a variable or a
// constant, which the string spells identically and which the current engine
// only recovers from which names it declared.
//
// A null `Ptr` is the IR's "__NONE__": the sentence was absent, or every part
// of it was. `to_smt` renders that back as the literal string "__NONE__", the
// same sentinel the rest of the loader uses.
namespace expr {

enum class Kind {
  Atom,       // (at ?p ?l)      -- predicate applied to args
  Not,        // (not X)         -- one child
  And,        // (and X Y ...)   -- n children
  Or,         // (or X Y ...)    -- n children
  Imply,      // (imply X Y)     -- two children
  Equals,     // (= a b)         -- two args, no children
  NotEquals,  // (not (= a b))   -- two args, no children
  Forall,     // one child, plus the bound variables
  Exists      // one child, plus the bound variables
};

// An argument position. Variable names are stored as the parser produces them,
// which is *without* the leading '?': the grammar consumes it. Constants are
// object names.
struct Term {
  std::string name;
  bool is_variable = false;
};

// A bound variable of a quantifier, together with the type predicates that
// constrain it. Explicit types give one; an implicitly typed variable gets
// whatever `type_inference` found, and the order is preserved so that
// rendering reproduces the loader's original output exactly.
struct Bound {
  std::string name;
  std::vector<std::string> types;
};

struct Node;
using Ptr = std::shared_ptr<const Node>;

struct Node {
  Kind kind = Kind::Atom;
  std::string predicate;          // Atom
  std::vector<Term> args;         // Atom (any arity), Equals/NotEquals (two)
  std::vector<Ptr> children;      // Not(1), And/Or(n), Imply(2), Forall/Exists(1)
  std::vector<Bound> bound;       // Forall/Exists
  std::string quantifier;         // Forall/Exists: "forall" or "exists" as written
};

inline Ptr make_atom(std::string predicate, std::vector<Term> args) {
  auto n = std::make_shared<Node>();
  n->kind = Kind::Atom;
  n->predicate = std::move(predicate);
  n->args = std::move(args);
  return n;
}

inline Ptr make_compare(bool equals, Term lhs, Term rhs) {
  auto n = std::make_shared<Node>();
  n->kind = equals ? Kind::Equals : Kind::NotEquals;
  n->args = {std::move(lhs), std::move(rhs)};
  return n;
}

inline Ptr make_connective(Kind kind, std::vector<Ptr> children) {
  auto n = std::make_shared<Node>();
  n->kind = kind;
  n->children = std::move(children);
  return n;
}

inline Ptr make_quantified(std::string quantifier,
                           std::vector<Bound> bound,
                           Ptr body) {
  auto n = std::make_shared<Node>();
  n->kind = quantifier == "exists" ? Kind::Exists : Kind::Forall;
  n->quantifier = std::move(quantifier);
  n->bound = std::move(bound);
  n->children = {std::move(body)};
  return n;
}

// Renders back to the SMT-LIB text the loader would have produced. This exists
// to prove the IR is faithful: `test_loader` asserts that every precondition
// and effect condition in every shipped domain renders back to the string
// stored beside it. Once the direct evaluator replaces the SMT path there is
// nothing left for this to check, and it goes with it.
inline std::string to_smt(Ptr const& e) {
  if (!e) {
    return "__NONE__";
  }
  switch (e->kind) {
    case Kind::Atom: {
      //A zero-arity predicate is referenced by bare name; SMT-LIB has no
      //nullary application.
      if (e->args.empty()) {
        return e->predicate;
      }
      std::string s = "("+e->predicate;
      for (auto const& a : e->args) {
        s += " "+a.name;
      }
      return s+")";
    }
    case Kind::Equals:
      return "(= "+e->args[0].name+" "+e->args[1].name+")";
    case Kind::NotEquals:
      return "(not (= "+e->args[0].name+" "+e->args[1].name+"))";
    case Kind::Not: {
      std::string inner = to_smt(e->children[0]);
      if (inner == "__NONE__") {
        return "__NONE__";
      }
      return "(not "+inner+")";
    }
    case Kind::Imply: {
      std::string a = to_smt(e->children[0]);
      std::string b = to_smt(e->children[1]);
      if (a == "__NONE__" || b == "__NONE__") {
        return "__NONE__";
      }
      return "(=> "+a+" "+b+")";
    }
    case Kind::And:
    case Kind::Or: {
      std::string s = std::string("(")+(e->kind == Kind::And ? "and" : "or");
      bool any = false;
      for (auto const& c : e->children) {
        std::string inner = to_smt(c);
        if (inner != "__NONE__") {
          s += " "+inner;
          any = true;
        }
      }
      if (!any) {
        return "__NONE__";
      }
      return s+")";
    }
    case Kind::Forall:
    case Kind::Exists: {
      if (e->bound.empty()) {
        return "__NONE__";
      }
      std::string body = to_smt(e->children[0]);
      if (body == "__NONE__") {
        return "__NONE__";
      }
      //Typed quantification is desugared the way the loader desugars it: the
      //binder ranges over __Object__ and the types become an antecedent.
      std::string qs = "("+e->quantifier+" (";
      std::string t_imply = "(=> (and";
      for (auto const& b : e->bound) {
        qs += "("+b.name+" __Object__) ";
        for (auto const& t : b.types) {
          t_imply += " ("+t+" "+b.name+")";
        }
      }
      qs += ") ";
      t_imply += ")";
      return qs+t_imply+" "+body+"))";
    }
  }
  return "__NONE__";
}

} // namespace expr
