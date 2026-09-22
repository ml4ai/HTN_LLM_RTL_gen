#pragma once

#include "parsing/ast.hpp"
#include "util.h"
#include "expr.h"
#include "z3++.h"
#include <unordered_set>
#include <string>
#include <tuple>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <algorithm>
#include <memory>

//Type struct
struct Type {
  //Name of type
  std::string type;
  //Indexes of the types lineage. 
  std::unordered_set<int> lineage;
  //Indexes of the types children
  std::unordered_set<int> children;
};

//TypeTree struct. This keeps track of the type inheritence hierarchy
struct TypeTree {
  //Unordered map of the form {index,Type struct}
  std::unordered_map<int,Type> types;
  //Keeps track of next newly usable ID
  int nextID = 0;
  //Keeps track of usable freed IDs
  std::vector<int> freedIDs;
  //Adds type to TypeTree and returns its index. 
  int add_type(Type& type) {
    int id;
    if (!freedIDs.empty()) {
      id = freedIDs.back();
      freedIDs.pop_back();
    }
    else {
      id = nextID;
      nextID++;
    }
    types[id] = type;
    return id;
  }
  //This clears tree and adds just the root type!
  void add_root(std::string type) {
    this->types.clear();
    this->freedIDs.clear();
    this->nextID = 0;
    Type new_t;
    new_t.type = type;
    this->add_type(new_t);
  }

  void add_child(std::string type, std::string ancestor) {
    Type new_t;
    new_t.type = type;
    int a = this->find_type(ancestor); 
    new_t.lineage.insert(a);
    for (auto i : this->types[a].lineage) {
      new_t.lineage.insert(i);
    }
    int t = this->add_type(new_t);
    this->types[a].children.insert(t);  
  }

  void add_ancestor(std::string type, std::string ancestor) {
    int a = this->find_type(ancestor);
    int i = this->find_type(type);
    for (auto &[id,t] : this->types) {
      if (t.children.contains(i)) {
        t.children.erase(i); 
        t.children.insert(a);
        this->types[a].lineage.clear();
        this->types[a].lineage.insert(id);
        for (auto l : t.lineage) {
          this->types[a].lineage.insert(l);
        }
      }
    }
    this->types[a].children.insert(i);
    this->types[i].lineage.clear();
    this->types[i].lineage.insert(a);
    for (auto l : this->types[a].lineage) {
      this->types[i].lineage.insert(l);
    }
    propogate_new_lineage(i);
  }
  //Helper function for add_ancestor
  void propogate_new_lineage(int i) {
    if (this->types[i].children.empty()) {
      return;
    }
    for (auto const& c : this->types[i].children) {
      this->types[c].lineage.clear();
      this->types[c].lineage.insert(i);
      for (auto l : this->types[i].lineage) {
        this->types[c].lineage.insert(l);
      }
      propogate_new_lineage(c);
    } 
  }

  //Returns index of Type struct with name type
  int find_type(std::string type) {
    for (auto const &[i,t] : this->types) {
      if (t.type == type) {
        return i;
      }  
    }
    return -1;
  }
};

int find_var(std::vector<std::pair<std::string, std::string>> vars, std::string var) {
  for (int i = 0; i < vars.size(); i++) {
    if (vars[i].first == var) {
      return i;
    }
  }
  return -1;
}

//Add objects from the problem definition, then ungrounded predicate
//statements, and then run initialize before using the kb!
class KnowledgeBase {
    private:
      //The predicate signatures and the objects. Both are fixed for the whole
      //problem, so every KnowledgeBase derived from one holds the same content
      //-- and there is one per search node. Sharing it means a copy carries a
      //pointer instead of re-allocating the lot.
      struct Schema {
        //header,{{var1,type1},{var2,type2},...}
        std::vector<std::pair<std::string,std::vector<std::pair<std::string,std::string>>>> predicates;
        //constant, type
        std::unordered_map<std::string,std::string> objects;
      };
      std::shared_ptr<const Schema> schema;
      //The fact base: predicate head -> its argument tuples. This used to be
      //held twice, once as "(head a b)" strings and once as split tuples, with
      //the second rebuilt from the first on every update_state. Every copy of a
      //KnowledgeBase -- one per search node -- paid for both. The tuples are
      //the useful form, so they are now the only one, and the strings are
      //reconstructed on the rare paths that still want them.
      std::unordered_map<std::string, std::vector<std::vector<std::string>>> relations;

      static std::string fact_string(std::string const& head,
                                     std::vector<std::string> const& args) {
        std::string f = "("+head;
        for (auto const& a : args) {
          f += " "+a;
        }
        return f+")";
      }
      //belief state in smt form, built on demand; see update_state.
      std::string smt_state;
      bool smt_fresh = false;

      //Gets all bindings for smt_statement and set of variables
      //A set of bindings is an vector of pairs of form {variable,value}. Called
      //by ask when given a collection of params.
      std::vector<std::vector<std::pair<std::string,std::string>>> 
      get_bindings(std::string F, 
                  const std::vector<std::pair<std::string, std::string>>& variables) {
        std::vector<std::vector<std::pair<std::string,std::string>>> results;
        z3::context con;
        z3::solver s(con);
        s.from_string(F.c_str());
        while (s.check() == z3::sat) {
          auto m = s.get_model();
          std::vector<std::pair<std::string, std::string>> bindings = variables;
          for (unsigned i = 0; i < m.size(); i++) {
            auto v = m[i];
            auto v_name = v.name().str();
            auto index = find_var(bindings, v_name);
            if(index != -1) {
              bindings[index].second = m.get_const_interp(v).to_string();
            }
          }
          results.push_back(bindings);
          z3::expr_vector block(con);
          for (unsigned j = 0; j < m.size(); j++) {
            auto d = m[j];
            if (d.arity() > 0) {
              continue;
            } 
            auto c = d();
            if (c.is_array() || c.get_sort().sort_kind() == Z3_UNINTERPRETED_SORT) {
              throw std::logic_error("arrays and uninterpreted sorts are not supported");
            }
            block.push_back(c != m.get_const_interp(d));
          }
          s.add(z3::mk_or(block));
        }
        return results;
      }
      
      //Parses (header arg1 arg2 ...) into {header,{arg1, arg2, ...}}
      std::pair<std::string, std::vector<std::string>> 
      parse_predicate(std::string pred) {
        std::vector<std::string> symbols = {};
        size_t pos = 0;
        std::string p = pred.substr(1,pred.size() - 2);
        std::string space_delimiter = " ";
        while ((pos = p.find(space_delimiter)) != std::string::npos) {
          symbols.push_back(p.substr(0, pos));
          p.erase(0, pos + space_delimiter.length());
        }
        if (!p.empty()) {
          symbols.push_back(p);
        }
        std::string predicate = symbols.at(0);
        symbols.erase(symbols.begin());
        return std::make_pair(predicate, symbols);
      }

      // Initializes the KBs belief state with
      // closed world assumption. Call ONCE after adding all of the needed types, objects, and
      // predicate or else all tell() and ask() calls will crash or fail!   
      void initialize(Schema& sch, TypeTree typetree) {
        for (auto const& [t1, t2] : typetree.types) {
          std::vector<std::pair<std::string, std::string>> params;
          params.push_back(std::make_pair("?o","__Object__"));
          sch.predicates.push_back(std::make_pair(t2.type,params));
        }
        for (auto const& [o1,o2] : sch.objects) {
          int t = typetree.find_type(o2);
          for (auto const& [t1,t2] : typetree.types) {
            if (in(t1,typetree.types[t].lineage) ||
                t2.type == o2) {
              //Type facts are unary: (type object).
              auto& rel = this->relations[t2.type];
              if (std::find(rel.begin(),rel.end(),std::vector<std::string>{o1}) == rel.end()) {
                rel.push_back({o1});
              }
            } 
          }
        }
      }

    public:
      KnowledgeBase() {}
      KnowledgeBase(std::vector<std::pair<std::string,std::vector<std::pair<std::string,std::string>>>> predicates,
                    std::unordered_map<std::string,std::string> objects,
                    TypeTree typetree) {
        auto sch = std::make_shared<Schema>();
        sch->predicates = std::move(predicates);
        sch->objects = std::move(objects);
        //initialize both extends the signatures with one per type and asserts
        //the type facts, so it runs while the schema is still mutable.
        this->initialize(*sch,typetree);
        this->schema = std::move(sch);
        this->update_state();
      }

      //Used by tell to update the smt_state string. tell() calls this
      //automatically by default, but it can be called manually too (see tell()
      //for default settings).
      //Refreshes the derived views of the fact base. The relation index is
      //rebuilt eagerly: it is what queries are answered from. The SMT text is
      //only marked stale, because building it costs several kilobytes of
      //string that a KnowledgeBase then carries into every copy of itself --
      //and since the direct evaluator took over the query path, most states
      //are never asked anything a solver has to answer, so most of those
      //kilobytes were built, copied and discarded unread.
      void update_state() {
        //Derived index first, so it is never stale while smt_state is fresh.
        this->smt_state.clear();
        this->smt_fresh = false;
      }

    private:
      //Builds the SMT-LIB encoding of the current facts. Only reached through
      //ensure_smt, i.e. only when something actually asks Z3.
      void build_smt_state() {
        if (!this->schema) {
          return;
        }
        this->smt_state = "(declare-datatype __Object__ (";
        for (auto const& [o1,o2] : this->schema->objects) {
          this->smt_state += o1+" ";
        }
        this->smt_state += "))\n";
        for (auto const& p : this->schema->predicates) {
          //A zero-arity predicate is a propositional atom. It gets a plain Bool
          //constant and a bare assert: running it through the quantified path
          //below would emit "(forall () ...)" and "(and)", both of which Z3
          //rejects, which made any domain declaring one unusable.
          if (p.second.empty()) {
            this->smt_state += "(declare-fun "+p.first+" () Bool)\n";
            auto pf = this->relations.find(p.first);
            if (pf != this->relations.end() && !pf->second.empty()) {
              this->smt_state += "(assert "+p.first+")\n";
            }
            else {
              this->smt_state += "(assert (not "+p.first+"))\n";
            }
            continue;
          }
          if (this->relations.find(p.first) != this->relations.end()) {
            if (!this->relations[p.first].empty()) {
              this->smt_state += "(declare-fun "+p.first+" (";
              std::string pred_assert = "(assert (forall (";
              int i = 0;
              std::string var_assert = "";
              for (auto const& pars : p.second) {
                this->smt_state += "__Object__ ";
                pred_assert += "(x_"+std::to_string(i)+" __Object__) ";
                var_assert += " x_"+std::to_string(i);
                i++;
              }
              pred_assert += ") (= ("+p.first+var_assert+") (or ";
              for (auto const& tup : this->relations[p.first]) {
                pred_assert += "(and ";
                int j = 0;
                for (auto const& vals : tup) {
                  pred_assert += "(= x_"+std::to_string(j)+" "+vals+") ";
                  j++;
                }
                pred_assert += ") ";
              }
              pred_assert += "))))\n";
              this->smt_state += ") Bool)\n";
              this->smt_state += pred_assert;
            }
            else {
              this->smt_state += "(declare-fun "+p.first+" (";
              std::string pred_assert = "(assert (forall (";
              int i = 0;
              std::string var_assert = "";
              for (auto const& vars : p.second) {
                this->smt_state += "__Object__ ";
                pred_assert += "(x_"+std::to_string(i)+" __Object__) ";
                var_assert += " x_"+std::to_string(i);
                i++;
              }
              pred_assert += ") (not ("+p.first+var_assert+"))))\n";
              this->smt_state += ") Bool)\n";
              this->smt_state += pred_assert;
            }
          }
          else {
            this->smt_state += "(declare-fun "+p.first+" (";
            std::string pred_assert = "(assert (forall (";
            int i = 0;
            std::string var_assert = "";
            for (auto const& vars : p.second) {
              this->smt_state += "__Object__ ";
              pred_assert += "(x_"+std::to_string(i)+" __Object__) ";
              var_assert += " x_"+std::to_string(i);
              i++;
            }
            pred_assert += ") (not ("+p.first+var_assert+"))))\n";
            this->smt_state += ") Bool)\n";
            this->smt_state += pred_assert;
          }
        }
      }

      void ensure_smt() {
        if (!this->smt_fresh) {
          this->build_smt_state();
          this->smt_fresh = true;
        }
      }

    public:

      //Returns false if predicate type is not found. Only give it a single grounded
      //predicate in (header arg1 arg2 ...) form. Adding facts requires that remove = false (default) and 
      //deleting facts require that remove = true. 
      //Do not give it a (not (pred)), the "not" will be
      //handled by the parser!
      //By Default, it calls update_state(). If you are calling this in a long
      //series or loop, set update_state = false and then call update_state()
      //manually after all the tells for more optimal performance! 
      bool tell(std::string pred, bool remove = false, bool update_state = true) {
        auto pp = this->parse_predicate(pred);
        bool not_found = true;
        for (auto const& p : this->schema->predicates) {
          if (pp.first == p.first and pp.second.size() == p.second.size()) {
            not_found = false;
          }
        }
        if (not_found) {
          return false;
        }
        if (remove) {
          auto& rel = this->relations[pp.first];
          rel.erase(std::remove(rel.begin(),rel.end(),pp.second),rel.end());
          if (update_state) {
            this->update_state();
          }
          return true;
        }
        auto& rel = this->relations[pp.first];
        if (std::find(rel.begin(),rel.end(),pp.second) == rel.end()) {
          rel.push_back(pp.second);
        }
        if (update_state) {
          this->update_state();
        }
        return true;
      }

      //This adds the assert to expr, but the rest of expr must be made smt
      //compatible prior to this call. Z3 will give an error if a variable is not
      //found in params is used, which complies with HDDLs specifications for method and
      //action definitions. Params must be {{var1,type1},{var2,type2},...} 
      //This returns {{{var1,val1},{var2,val2},...},...}
      //EX: expr = (and (A ?x) (or (B ?x y) (C z)))
      //params = {{"?x", "thing"}}
      std::vector<std::vector<std::pair<std::string,std::string>>>
      ask(std::string expr,
          std::vector<std::pair<std::string, std::string>>& params) {
        this->ensure_smt();
        std::string smt_expr = this->smt_state;
        for (auto const& p : params) {
          smt_expr += "(declare-const "+p.first+" __Object__)\n";
          smt_expr += "(assert ("+p.second+" "+p.first+"))\n";
        }
        if (expr != "") {
          smt_expr += "(assert "+expr+")\n";
        }
        return get_bindings(smt_expr,params);
      }

      //Satisfiability only: does any binding of params satisfy expr? Same query
      //ask(expr,params) builds, but it stops at the first model instead of
      //enumerating all of them with blocking clauses. For a caller that only
      //needs a yes or no -- a precondition whose parameters are already pinned
      //to values by expr -- that is one solver call rather than one per model.
      bool ask_any(std::string expr,
                   std::vector<std::pair<std::string, std::string>>& params) {
        this->ensure_smt();
        std::string smt_expr = this->smt_state;
        for (auto const& p : params) {
          smt_expr += "(declare-const "+p.first+" __Object__)\n";
          smt_expr += "(assert ("+p.second+" "+p.first+"))\n";
        }
        if (expr != "") {
          smt_expr += "(assert "+expr+")\n";
        }
        z3::context con;
        z3::solver s(con);
        s.from_string(smt_expr.c_str());
        return s.check() == z3::sat;
      }

      //This adds the assert to expr, but the rest of expr must be made smt
      //compatible prior to this call.
      //This is mostly an issue for things like forall and exist, which have different
      //syntax in smt compared to hddl.
      //Only grounded statements are allowed here!
      //EX: (and (A x) (or (B x y) (C z)))
      bool ask(std::string expr) {
        //Score functions ask through this, once per rollout that reaches a
        //terminal, and their queries are ground text. Reading them into the IR
        //once and answering from the fact index avoids building the whole SMT
        //encoding and starting a solver for what is a handful of lookups. The
        //parse is cached: these strings are literals in the score function, so
        //there are only ever a few distinct ones.
        //`expr` is also the parameter name here, so the namespace needs qualifying.
        static std::unordered_map<std::string,::expr::Ptr> parsed;
        auto it = parsed.find(expr);
        if (it == parsed.end()) {
          it = parsed.emplace(expr,::expr::parse_ground(expr)).first;
        }
        auto holds = [this](std::string const& head,
                            std::vector<std::string> const& args) {
          for (auto const& tup : this->get_relation(head)) {
            if (tup == args) {
              return true;
            }
          }
          return false;
        };
        auto direct = ::expr::eval_ground(it->second,holds);

#ifdef HTN_DIFFERENTIAL_EVAL
        if (direct) {
          this->ensure_smt();
          z3::context dcon;
          z3::solver ds(dcon);
          ds.from_string((this->smt_state+"(assert "+expr+")\n").c_str());
          if (*direct != (ds.check() == z3::sat)) {
            throw std::logic_error("ground evaluator disagrees with Z3 on: "+expr);
          }
        }
#endif

        if (direct) {
          return *direct;
        }
        this->ensure_smt();
        z3::context con;
        z3::solver s(con);
        std::string smt_expr = this->smt_state+"(assert "+expr+")\n";
        s.from_string(smt_expr.c_str());
        return s.check() == z3::sat;
      }


      //prints smt_state string
      void print_smt_state() {
        this->ensure_smt();
        std::cout << this->smt_state << std::endl;
      }

      //Argument tuples of one predicate, empty if it holds of nothing. Used by
      //the direct evaluator; see evaluator.h.
      std::vector<std::vector<std::string>> const& get_relation(std::string const& head) const {
        static const std::vector<std::vector<std::string>> none;
        auto it = this->relations.find(head);
        return it == this->relations.end() ? none : it->second;
      }

      //Objects of a given type. Types are unary predicates asserted for every
      //object and all of its ancestors (see initialize), so a type's extension
      //is just its relation.
      std::vector<std::string> type_extension(std::string const& type) const {
        std::vector<std::string> objs;
        for (auto const& tup : this->get_relation(type)) {
          if (tup.size() == 1) {
            objs.push_back(tup[0]);
          }
        }
        return objs;
      }

      std::unordered_set<std::string> get_facts(std::string head) {
        std::unordered_set<std::string> out;
        for (auto const& tup : this->get_relation(head)) {
          out.insert(fact_string(head,tup));
        }
        return out;
      } 

      void print_facts() {
        for (auto const& [head,rel] : this->relations) {
          for (auto const& tup : rel) {
            std::cout << fact_string(head,tup) << std::endl;
          }
        }
      }

      std::unordered_map<std::string, std::unordered_set<std::string>>
      get_facts() {
        std::unordered_map<std::string, std::unordered_set<std::string>> out;
        for (auto const& [head,rel] : this->relations) {
          auto& set = out[head];
          for (auto const& tup : rel) {
            set.insert(fact_string(head,tup));
          }
        }
        return out;
      }
};


