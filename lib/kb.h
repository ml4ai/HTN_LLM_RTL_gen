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

  //A type whose objects are exactly those of any of `members`: what
  //(either a b) means in a parameter list. It is added under the root and
  //written into the lineage of every member and every member's subtypes, so
  //the type facts KnowledgeBase::initialize derives from lineage hold for all
  //of their objects. It is deliberately not recorded as a parent in
  //`children`: add_ancestor and propogate_new_lineage rebuild lineage from
  //children as a single chain, and would drop a member's own parent.
  void add_union(std::string const& type, std::vector<std::string> const& members,
                 std::string const& root) {
    if (this->find_type(type) != -1) {
      return;
    }
    this->add_child(type,root);
    int u = this->find_type(type);
    std::vector<int> todo;
    for (auto const& m : members) {
      int i = this->find_type(m);
      if (i != -1) {
        todo.push_back(i);
      }
    }
    while (!todo.empty()) {
      int i = todo.back();
      todo.pop_back();
      if (i == u || !this->types[i].lineage.insert(u).second) {
        continue;
      }
      for (auto c : this->types[i].children) {
        todo.push_back(c);
      }
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

        //Symbol tables. Predicate and object names are fixed for the whole
        //problem, so they are interned once here and the fact base stores
        //small integers instead of strings. Schema is shared through a
        //shared_ptr by every KnowledgeBase, so this costs one pointer per
        //copy rather than a table per search node.
        std::unordered_map<std::string,int> pred_id;   //name -> index into predicates
        std::unordered_map<std::string,int> obj_id;    //name -> dense object id
        std::vector<std::string> obj_name;             //the inverse
        std::vector<size_t> arity;                     //per predicate

        void intern() {
          pred_id.clear(); arity.clear();
          arity.reserve(predicates.size());
          for (size_t i = 0; i < predicates.size(); i++) {
            //A repeated predicate name keeps its first id; the signatures are
            //checked by arity at tell() time, as they were before.
            pred_id.emplace(predicates[i].first,(int)i);
            arity.push_back(predicates[i].second.size());
          }
          obj_id.clear(); obj_name.clear();
          obj_name.reserve(objects.size());
          for (auto const& [name,type] : objects) {
            obj_id.emplace(name,(int)obj_name.size());
            obj_name.push_back(name);
          }
        }
      };
      std::shared_ptr<const Schema> schema;
      //The fact base: predicate head -> its argument tuples. This used to be
      //held twice, once as "(head a b)" strings and once as split tuples, with
      //the second rebuilt from the first on every update_state. Every copy of a
      //KnowledgeBase -- one per search node -- paid for both. The tuples are
      //the useful form, so they are now the only one, and the strings are
      //reconstructed on the rare paths that still want them.
      //Indexed by predicate id, each entry a FLAT array of object ids: tuple k
      //of predicate p occupies [k*arity, (k+1)*arity). Flat rather than a
      //vector of tuples because the whole point is that a copy of a
      //KnowledgeBase -- one per search node, one per binding in apply_binding
      //-- should be a handful of integer buffers rather than an allocation per
      //predicate, per tuple and per argument.
      //Each predicate's buffer is shared between states and copied only when a
      //state writes to it (copy-on-write); a null entry is an empty relation.
      //
      //This is the planner's answer to "stop copying the fact base per
      //successor". A trail -- apply effects in place, undo them on the way
      //back -- was the original plan, and it does not fit: the MCTS tree holds
      //many states alive at once, so there is no single stack to unwind.
      //Sharing gets what a trail was for without the unwinding. A successor
      //shares every relation its action did not touch, and an action touches
      //one to three of a dozen or more (the type predicates alone are several).
      //Copying a KnowledgeBase is then one allocation and a refcount bump per
      //predicate, not an allocation and a copy per predicate.
      std::vector<std::shared_ptr<std::vector<int>>> relations;

      //The one way to get a relation you may write to: detaches it from every
      //other state first if it is shared, so a write can never be seen by a
      //state it was not made in.
      std::vector<int>& relation_mut(int pid) {
        auto& p = this->relations[pid];
        if (!p) {
          p = std::make_shared<std::vector<int>>();
        }
        else if (p.use_count() > 1) {
          p = std::make_shared<std::vector<int>>(*p);
        }
        return *p;
      }

      static std::string fact_string(std::string const& head,
                                     std::vector<std::string> const& args) {
        std::string f = "("+head;
        for (auto const& a : args) {
          f += " "+a;
        }
        return f+")";
      }

      //Reconstructs "(head a b)" for the cold paths -- printing, get_facts,
      //and the SMT fallback -- which are the only places that still want a
      //fact as text.
      std::string fact_string_at(int pid, size_t off) const {
        std::string f = "("+this->schema->predicates[pid].first;
        size_t a = this->schema->arity[pid];
        for (size_t i = 0; i < a; i++) {
          f += " "+this->schema->obj_name[this->relation_of(pid)[off+i]];
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
        //The type predicates were just appended above, so intern now that the
        //signature list is final.
        sch.intern();
        this->relations.assign(sch.predicates.size(),nullptr);
        for (auto const& [o1,o2] : sch.objects) {
          int t = typetree.find_type(o2);
          int oid = sch.obj_id.at(o1);
          for (auto const& [t1,t2] : typetree.types) {
            if (in(t1,typetree.types[t].lineage) ||
                t2.type == o2) {
              //Type facts are unary: (type object).
              int pid = sch.pred_id.at(t2.type);
              if (std::find(this->relation_of(pid).begin(),this->relation_of(pid).end(),oid)
                  == this->relation_of(pid).end()) {
                this->relation_mut(pid).push_back(oid);
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
            int pid0 = this->predicate_id(p.first);
            if (pid0 >= 0 && !this->relation_of(pid0).empty()) {
              this->smt_state += "(assert "+p.first+")\n";
            }
            else {
              this->smt_state += "(assert (not "+p.first+"))\n";
            }
            continue;
          }
          int pid = this->predicate_id(p.first);
          if (pid >= 0) {
            if (!this->relation_of(pid).empty()) {
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
              size_t ar = this->schema->arity[pid];
              auto const& rel = this->relation_of(pid);
              for (size_t off = 0; ar > 0 && off + ar <= rel.size(); off += ar) {
                pred_assert += "(and ";
                for (size_t j = 0; j < ar; j++) {
                  pred_assert += "(= x_"+std::to_string(j)+" "+
                                 this->schema->obj_name[rel[off+j]]+") ";
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

      //Offset of a tuple in a flat relation, or npos. Arity 0 is the
      //propositional case: the relation is either empty or holds one sentinel.
      static size_t find_tuple(std::vector<int> const& rel,
                               std::vector<int> const& tup,
                               size_t arity) {
        if (arity == 0) {
          return rel.empty() ? std::string::npos : 0;
        }
        for (size_t off = 0; off + arity <= rel.size(); off += arity) {
          bool same = true;
          for (size_t i = 0; i < arity; i++) {
            if (rel[off+i] != tup[i]) { same = false; break; }
          }
          if (same) {
            return off;
          }
        }
        return std::string::npos;
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
        int pid = this->predicate_id(pp.first);
        //Unknown predicate, or the right name at the wrong arity: rejected, as
        //before. The arity check used to be a scan over every signature.
        if (pid < 0 || this->schema->arity[pid] != pp.second.size()) {
          return false;
        }
        //Every argument must be a known object. This is stricter than the old
        //code, which stored whatever string it was handed -- but a fact over an
        //undeclared object could never be matched by a query anyway, since
        //queries range over declared objects.
        std::vector<int> tup;
        tup.reserve(pp.second.size());
        for (auto const& a : pp.second) {
          auto it = this->schema->obj_id.find(a);
          if (it == this->schema->obj_id.end()) {
            return false;
          }
          tup.push_back(it->second);
        }
        size_t a = this->schema->arity[pid];
        //Look before writing: relation_mut detaches a shared buffer, and adding
        //a fact that already holds or removing one that does not should not
        //cost a copy.
        size_t at = this->find_tuple(this->relation_of(pid),tup,a);
        if (remove) {
          if (at != std::string::npos) {
            auto& rel = this->relation_mut(pid);
            rel.erase(rel.begin()+at,rel.begin()+at+(a == 0 ? 1 : a));
          }
          if (update_state) {
            this->update_state();
          }
          return true;
        }
        if (at == std::string::npos) {
          auto& rel = this->relation_mut(pid);
          if (a == 0) {
            //A propositional atom has no arguments; one sentinel entry records
            //that it holds.
            rel.push_back(0);
          }
          else {
            rel.insert(rel.end(),tup.begin(),tup.end());
          }
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
          return this->holds(head,args);
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

      //The id-based view the direct evaluator works against; see evaluator.h.
      //Nothing here allocates.
      int predicate_id(std::string const& head) const {
        if (!this->schema) {
          return -1;
        }
        auto it = this->schema->pred_id.find(head);
        return it == this->schema->pred_id.end() ? -1 : it->second;
      }

      int object_id(std::string const& name) const {
        if (!this->schema) {
          return -1;
        }
        auto it = this->schema->obj_id.find(name);
        return it == this->schema->obj_id.end() ? -1 : it->second;
      }

      std::string const& object_name(int oid) const {
        return this->schema->obj_name[oid];
      }

      size_t arity_of(int pid) const {
        return this->schema->arity[pid];
      }

      //Flat argument tuples of one predicate: tuple k occupies
      //[k*arity, (k+1)*arity). Empty if the predicate holds of nothing.
      std::vector<int> const& relation_of(int pid) const {
        static const std::vector<int> none;
        if (pid < 0 || !this->relations[pid]) {
          return none;
        }
        return *this->relations[pid];
      }

      //Objects of a given type, as ids. Types are unary predicates asserted for
      //every object and all of its ancestors (see initialize), so a type's
      //extension is just its relation.
      //By reference: it used to return the relation by value, a copy per call,
      //and the evaluator calls it once per unbound parameter per query.
      std::vector<int> const& type_extension(std::string const& type) const {
        static const std::vector<int> none;
        int pid = this->predicate_id(type);
        if (pid < 0 || this->schema->arity[pid] != 1) {
          return none;
        }
        return this->relation_of(pid);
      }

      //What a score function needs, answered from the fact index. Score
      //functions run on every finished rollout; get_facts() rebuilds the whole
      //state as strings in hash sets, which made sar3's scorer 8.8% of that
      //domain's runtime just to count four predicates (planner_doc.md 8.18).

      //Whether the ground fact (head args...) holds. False for an unknown
      //predicate, a wrong arity or an unknown object.
      bool holds(std::string const& head, std::vector<std::string> const& args) const {
        int pid = this->predicate_id(head);
        if (pid < 0 || this->schema->arity[pid] != args.size()) {
          return false;
        }
        std::vector<int> tup;
        tup.reserve(args.size());
        for (auto const& a : args) {
          int oid = this->object_id(a);
          if (oid < 0) {
            return false;
          }
          tup.push_back(oid);
        }
        return find_tuple(this->relation_of(pid),tup,args.size()) != std::string::npos;
      }

      //How many facts of a predicate hold: the length of its buffer over its
      //arity. A type is a one-argument predicate, so this also counts a type's
      //objects. Zero for an unknown predicate.
      size_t count_facts(std::string const& head) const {
        int pid = this->predicate_id(head);
        if (pid < 0) {
          return 0;
        }
        size_t a = this->schema->arity[pid];
        size_t n = this->relation_of(pid).size();
        return a == 0 ? (n > 0 ? 1 : 0) : n / a;
      }

      //The argument lists of every fact of a predicate, as object names. For a
      //scorer that must look at which facts hold, not just how many.
      std::vector<std::vector<std::string>> facts_of(std::string const& head) const {
        std::vector<std::vector<std::string>> out;
        int pid = this->predicate_id(head);
        if (pid < 0) {
          return out;
        }
        size_t a = this->schema->arity[pid];
        auto const& rel = this->relation_of(pid);
        if (a == 0) {
          if (!rel.empty()) {
            out.emplace_back();
          }
          return out;
        }
        for (size_t off = 0; off + a <= rel.size(); off += a) {
          std::vector<std::string> args;
          args.reserve(a);
          for (size_t i = 0; i < a; i++) {
            args.push_back(this->schema->obj_name[rel[off+i]]);
          }
          out.push_back(std::move(args));
        }
        return out;
      }

      std::unordered_set<std::string> get_facts(std::string head) {
        std::unordered_set<std::string> out;
        int pid = this->predicate_id(head);
        if (pid < 0) {
          return out;
        }
        size_t a = this->schema->arity[pid];
        auto const& rel = this->relation_of(pid);
        if (a == 0) {
          if (!rel.empty()) {
            out.insert("("+head+")");
          }
          return out;
        }
        for (size_t off = 0; off + a <= rel.size(); off += a) {
          out.insert(fact_string_at(pid,off));
        }
        return out;
      } 

      void print_facts() {
        for (auto const& [head,fs] : this->get_facts()) {
          for (auto const& f : fs) {
            std::cout << f << std::endl;
          }
        }
      }

      std::unordered_map<std::string, std::unordered_set<std::string>>
      get_facts() {
        std::unordered_map<std::string, std::unordered_set<std::string>> out;
        if (!this->schema) {
          return out;
        }
        for (size_t pid = 0; pid < this->relations.size(); pid++) {
          if (this->relation_of(pid).empty()) {
            continue;
          }
          out[this->schema->predicates[pid].first] =
            this->get_facts(this->schema->predicates[pid].first);
        }
        return out;
      }
};


