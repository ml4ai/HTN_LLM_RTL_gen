#pragma once

#include <cctype>
#include <functional>
#include <memory>
#include <optional>
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

namespace expr {

// A minimal reader for the ground SMT-ish strings that `KnowledgeBase::ask`
// accepts. Score functions are written by hand against that API -- see
// domains/score_functions.h -- so they arrive as text with no structure to
// walk, and the only engine that could answer them was Z3. Parsing them once
// into the IR lets them be answered from the fact index like everything else.
//
// Deliberately narrow: `and`, `or`, `not`, `=` and atoms over constants, which
// is the whole of what those strings contain. Anything else returns null and
// the caller falls back to the solver.
inline Ptr parse_ground(std::string const& text) {
  size_t i = 0;
  //Recursive descent over the token stream. Returns null on anything
  //unexpected, which aborts the whole parse.
  std::function<Ptr()> parse = [&]() -> Ptr {
    auto skip = [&]{ while (i < text.size() && isspace((unsigned char)text[i])) i++; };
    skip();
    if (i >= text.size()) {
      return nullptr;
    }
    if (text[i] != '(') {
      //A bare symbol: a zero-arity predicate.
      size_t start = i;
      while (i < text.size() && !isspace((unsigned char)text[i]) && text[i] != '(' && text[i] != ')') i++;
      if (i == start) {
        return nullptr;
      }
      return make_atom(text.substr(start,i-start),{});
    }
    i++;                                   //consume '('
    skip();
    size_t start = i;
    while (i < text.size() && !isspace((unsigned char)text[i]) && text[i] != '(' && text[i] != ')') i++;
    std::string head = text.substr(start,i-start);
    if (head.empty()) {
      return nullptr;
    }

    if (head == "and" || head == "or" || head == "not") {
      std::vector<Ptr> kids;
      while (true) {
        skip();
        if (i >= text.size()) {
          return nullptr;
        }
        if (text[i] == ')') {
          i++;
          break;
        }
        auto k = parse();
        if (!k) {
          return nullptr;
        }
        kids.push_back(k);
      }
      if (head == "not") {
        return kids.size() == 1 ? make_connective(Kind::Not,kids) : nullptr;
      }
      if (kids.empty()) {
        return nullptr;
      }
      return make_connective(head == "and" ? Kind::And : Kind::Or,kids);
    }

    //An atom, or an equality. Arguments are constants: this reader is only
    //used for ground text.
    std::vector<Term> args;
    while (true) {
      skip();
      if (i >= text.size()) {
        return nullptr;
      }
      if (text[i] == ')') {
        i++;
        break;
      }
      if (text[i] == '(') {
        return nullptr;                    //nested term: not ground text
      }
      size_t s2 = i;
      while (i < text.size() && !isspace((unsigned char)text[i]) && text[i] != '(' && text[i] != ')') i++;
      args.push_back({text.substr(s2,i-s2),false});
    }
    if (head == "=") {
      return args.size() == 2 ? make_compare(true,args[0],args[1]) : nullptr;
    }
    return make_atom(head,std::move(args));
  };

  auto e = parse();
  if (!e) {
    return nullptr;
  }
  while (i < text.size() && isspace((unsigned char)text[i])) i++;
  return i == text.size() ? e : nullptr;   //trailing junk means we misread it
}

// Truth of a ground expression, given a way to ask whether a tuple is a fact.
// Null for anything the caller should not trust to this path.
inline std::optional<bool> eval_ground(
    Ptr const& e,
    std::function<bool(std::string const&, std::vector<std::string> const&)> const& holds) {
  if (!e) {
    return std::nullopt;
  }
  switch (e->kind) {
    case Kind::Atom: {
      std::vector<std::string> args;
      for (auto const& a : e->args) {
        if (a.is_variable) {
          return std::nullopt;
        }
        args.push_back(a.name);
      }
      return holds(e->predicate,args);
    }
    case Kind::Equals:
    case Kind::NotEquals: {
      if (e->args[0].is_variable || e->args[1].is_variable) {
        return std::nullopt;
      }
      bool same = e->args[0].name == e->args[1].name;
      return e->kind == Kind::Equals ? same : !same;
    }
    case Kind::Not: {
      auto v = eval_ground(e->children[0],holds);
      return v ? std::optional<bool>(!*v) : std::nullopt;
    }
    case Kind::And: {
      for (auto const& c : e->children) {
        auto v = eval_ground(c,holds);
        if (!v) {
          return std::nullopt;
        }
        if (!*v) {
          return false;
        }
      }
      return true;
    }
    case Kind::Or: {
      for (auto const& c : e->children) {
        auto v = eval_ground(c,holds);
        if (!v) {
          return std::nullopt;
        }
        if (*v) {
          return true;
        }
      }
      return false;
    }
    default:
      return std::nullopt;
  }
}

} // namespace expr
