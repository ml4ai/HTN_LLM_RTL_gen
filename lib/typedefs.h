#pragma once

#include <string>
#include <iostream>
#include <tuple>
#include <unordered_map>
#include <queue>
#include <map>
#include <vector>
#include <memory>
#include <iterator>
#include "kb.h"
#include "expr.h"
#include "evaluator.h"
#include <optional>
#include "util.h"
#include <boost/variant/recursive_wrapper.hpp>

//{var,type}
using Params = std::vector<std::pair<std::string, std::string>>; 
//{var,val}
using Args = std::vector<std::pair<std::string, std::string>>;
using Preconds = std::string;
using Pred = std::pair<std::string,Params>;
using Predicates = std::vector<Pred>;
struct effect {
  std::string condition;
  bool remove;
  Pred pred;
  std::unordered_map<std::string,std::unordered_set<std::string>> forall;
  //The structured form of `condition`, kept so the condition can be evaluated
  //without a solver. Null when there is none.
  expr::Ptr condition_ast;
  effect (std::string condition, bool remove, Pred pred, std::unordered_map<std::string,std::unordered_set<std::string>> forall, expr::Ptr condition_ast = nullptr) {
    this->condition = condition;
    this->remove = remove;
    this->pred = pred;
    this->forall = forall;
    this->condition_ast = condition_ast;
  }
};
using Effects = std::vector<effect>;
using task_token = std::string;
using TaskDef = std::pair<std::string, Params>;
using TaskDefs = std::unordered_map<std::string,TaskDef>;
using Objects = std::unordered_map<std::string,std::string>;
using Scorer = double (*)(KnowledgeBase&,std::vector<std::string>&);
using Scorers = std::unordered_map<std::string, Scorer>;
using Reach_Map = std::unordered_map<std::string,std::vector<std::string>>;
using Reach_Maps = std::unordered_map<std::string,Reach_Map>;
using ID = std::string;

//How a method's precondition is read. See planner_doc.md 8.16 for what each
//means, why the reading 7.4 originally proposed is not among them, and what
//each was measured to do.
//
//  Compiled  HDDL's semantics. The precondition is compiled into a
//            synthesised action ordered before the method's subtasks and
//            checked when that action is scheduled -- which in a partially
//            ordered domain can be long before the subtasks run. Opt-in:
//            --precondition_mode compiled, for HDDL conformance.
//  AtStart   The default. The condition must also hold immediately before
//            the first real action arising from the method. This is the
//            tighter reading the HDDL authors leave to "future extensions"
//            because it cannot be compiled. It is the default because on the
//            mutex encoding of transport, 45% of the plans Compiled returned
//            broke a method precondition at the method's start (8.16.1), and
//            on no shipped domain did AtStart cost a solution or a score.
//  Protected A causal link: the early check is the producer and the method's
//            first real action is the consumer. The condition must hold
//            throughout that window, so an action that does NOT arise from the
//            method may not falsify it in between. Strictly stronger than
//            AtStart, which only asks that it hold at the end of the window.
//
//There is deliberately no mode that protects the condition until the method
//*finishes*. A method's own actions routinely consume its precondition --
//transport's deliver requires (at ?p ?l1), and its own pick_up deletes it -- so
//"until the method finishes" would reject every delivery. HDDL 2.1 lets a
//modeller mark each condition at-start, at-end or overall; one planner-wide
//mode cannot make that distinction.
enum class PreconditionMode { Compiled, AtStart, Protected };

//One method's precondition, recorded when the method is decomposed so that it
//can be checked again later. It is evaluated through the synthesised action the
//loader already made for it, with the method's binding as that action's
//arguments. Only created when the mode is not Compiled, so the default path
//carries none of this.
//What a pending check tests. Fixed once the method is decomposed, so it is
//shared by every copy of the task network rather than copied with it: a
//network is copied at every step of every rollout, and copying the action name
//and argument strings of every check it had ever recorded was 6% of transport's
//runtime (planner_doc.md 8.24).
struct CheckDef {
  std::string action;        //the __mprec_ action holding the precondition
  Args args;                 //the method's binding, as that action's arguments
};

struct PendingCheck {
  std::shared_ptr<const CheckDef> def;
  bool established = false;  //the early check has passed
  bool consumed = false;     //the method's first real action has run
  int established_at = -1;   //TaskGraph::step when the early check passed
};

struct Grounded_Task {
  std::string head;
  Args args;
  std::vector<int> incoming;
  std::vector<int> outgoing;
  std::string to_string() const {
    std::string s = "("+this->head;
    for (auto const &a : args) {
      s += " "+a.second;
    }
    s += ")";
    return s;
  }
};

struct TaskGraph {
  //Tasks in ascending id order, in one contiguous vector.
  //
  //This was an unordered_map<int,Grounded_Task>. Every search node and every
  //successor in a rollout holds a TaskGraph, so what matters is the cost of a
  //copy, and copying the map allocated a bucket array plus a hash node per
  //task. Copying this is one allocation for the vector.
  //
  //The order is also now a function of the task ids and nothing else.
  //simulation and expansion collect the unconstrained tasks by iterating here
  //and then shuffle them, so iteration order feeds the random stream: under the
  //map it was libc++'s bucket layout, an implementation detail that would have
  //made the same seed plan differently when built against libstdc++.
  //
  //Sorted by construction, never by sorting: ids come from nextID, which only
  //ever increases, so every insertion is an append, and erasing preserves the
  //order of what is left.
  //
  //Each task is held through a shared pointer and copied only when written
  //(copy-on-write). A network is copied at every step of every rollout, and
  //copying every task's head, arguments and edge lists with it was 5-8% of
  //runtime (planner_doc.md 8.24). Now a copy shares every task, and a write --
  //adding or removing an edge -- detaches just the task it changes. So reads go
  //through a const reference, and writes through mut().
  using TaskPtr = std::shared_ptr<Grounded_Task>;
  std::vector<std::pair<int,TaskPtr>> GTs;
  //Keeps track of next newly usable ID
  int nextID = 0;
  //Per network, not per domain, because whether a check has been established
  //or consumed is a fact about this branch of the search. Every successor gets
  //its own copy along with the rest of the network.
  std::vector<PendingCheck> checks;
  //Real actions applied on this branch so far. Only a real action changes the
  //state -- a synthesised check has no effects and a decomposition touches only
  //the network -- so two points on a branch with the same step see the same
  //state. That lets a check skip re-evaluation when nothing has run since it
  //last passed.
  int step = 0;

  //Which checks each task carries, kept here rather than in Grounded_Task.
  //Every task in every network is copied constantly, and fields on
  //Grounded_Task made every copy larger even in Compiled mode, where they are
  //always empty: measured, that cost the default path 1-2%. This table is empty
  //in Compiled mode, so the default path pays for one empty vector per network.
  struct TaskTags {
    std::vector<int> checks;   //the method preconditions this task arises from,
                               //inherited through every enclosing method
    int establishes = -1;      //for a synthesised check task: which check it is
  };
  //Sorted by task id by construction, like GTs: tags are set immediately after
  //add_node hands out an id, and ids only increase.
  //A task's tags never change once set, so, like the tasks, they are shared
  //between copies of the network rather than copied with it (8.24).
  std::vector<std::pair<int,std::shared_ptr<const TaskTags>>> tags;

  TaskTags const* tags_of(int id) const {
    auto it = std::lower_bound(tags.begin(),tags.end(),id,
                               [](auto const& p, int k) { return p.first < k; });
    return (it != tags.end() && it->first == id) ? it->second.get() : nullptr;
  }

  std::vector<int> const& checks_of(int id) const {
    static const std::vector<int> none;
    auto const* t = this->tags_of(id);
    return t ? t->checks : none;
  }

  int establishes_of(int id) const {
    auto const* t = this->tags_of(id);
    return t ? t->establishes : -1;
  }

  void set_tags(int id, TaskTags t) {
    if (t.checks.empty() && t.establishes < 0) {
      return;
    }
    this->tags.emplace_back(id,std::make_shared<const TaskTags>(std::move(t)));
  }

  TaskPtr const* slot(int i) const {
    auto it = std::lower_bound(GTs.begin(),GTs.end(),i,
                               [](std::pair<int,TaskPtr> const& p, int k) { return p.first < k; });
    return (it != GTs.end() && it->first == i) ? &it->second : nullptr;
  }

  Grounded_Task const* find(int i) const {
    auto const* s = this->slot(i);
    return s ? s->get() : nullptr;
  }

  //Throws on an id that is not in the graph. The map's operator[] silently
  //inserted an empty task instead, which only failed later, and less clearly,
  //as "Invalid task" on its empty head.
  Grounded_Task const& operator[](int i) const {
    return this->at(i);
  }

  Grounded_Task const& at(int i) const {
    auto const* p = this->find(i);
    if (!p) {
      throw std::logic_error("TaskGraph has no task with id "+std::to_string(i)+"!");
    }
    return *p;
  }

  //The one way to get a task to write to: detaches it first if another
  //network shares it, so a write is never seen by a network it was not made in.
  Grounded_Task& mut(int i) {
    auto* s = const_cast<TaskPtr*>(this->slot(i));
    if (!s) {
      throw std::logic_error("TaskGraph has no task with id "+std::to_string(i)+"!");
    }
    if (s->use_count() > 1) {
      *s = std::make_shared<Grounded_Task>(**s);
    }
    return **s;
  }

  int add_node(Grounded_Task const& GT) {
    int id = this->nextID++;
    this->GTs.emplace_back(id,std::make_shared<Grounded_Task>(GT));
    return id;
  }

  //For a task built only to be inserted -- apply_binding builds one per
  //subtask per binding.
  int add_node(Grounded_Task&& GT) {
    int id = this->nextID++;
    this->GTs.emplace_back(id,std::make_shared<Grounded_Task>(std::move(GT)));
    return id;
  }

  // gt1 -> gt2
  void add_edge(int gt1, int gt2) {
    this->mut(gt1).outgoing.push_back(gt2);
    this->mut(gt2).incoming.push_back(gt1);
  }

  void remove_node(int gt) {
    //A shared reference keeps the task alive and unchanged while its
    //neighbours are detached and edited below.
    TaskPtr node = *this->slot(gt);
    for (int og : node->outgoing) {
      auto& inc = this->mut(og).incoming;
      inc.erase(std::remove(inc.begin(),inc.end(),gt),inc.end());
    }
    for (int ic : node->incoming) {
      auto& out = this->mut(ic).outgoing;
      out.erase(std::remove(out.begin(),out.end(),gt),out.end());
    }
    auto it = std::lower_bound(GTs.begin(),GTs.end(),gt,
                               [](std::pair<int,TaskPtr> const& p, int k) { return p.first < k; });
    this->GTs.erase(it);
    if (!this->tags.empty()) {
      auto tt = std::lower_bound(tags.begin(),tags.end(),gt,
                                 [](auto const& p, int k) { return p.first < k; });
      if (tt != tags.end() && tt->first == gt) {
        this->tags.erase(tt);
      }
    }
  }
  
  bool empty() {
    return this->GTs.empty();
  } 

  int size() {
    return this->GTs.size();
  }

};

struct TaskNode {
  std::string task;
  std::string token;
  //The method that decomposed this task, for a compound task in the committed
  //plan; empty for an action. The graph prints it (planner_doc.md 8.22).
  std::string method;
  std::vector<int> children;
  std::vector<int> outgoing;
};

using TaskTree = std::unordered_map<int,TaskNode>;

struct pNode {
    KnowledgeBase state;
    TaskGraph tasks;
    int prevTID = -1;
    std::vector<int> addedTIDs;
    //For a node made by decomposing task prevTID: the method used, recorded on
    //the task tree when this node is committed.
    std::string method;
    std::vector<int> treeRoots;
    std::vector<std::string> plan;
    int depth = 0;
    double score = 0.0;
    int sims = 0;
    int pred = -1;
    bool deadend = false;
    std::vector<int> successors = {};
    std::vector<int> unexplored = {};
};

using pTree = std::unordered_map<int,pNode>;

struct Results{
  pTree t;
  int root;
  int end;
  TaskTree tasktree;
  Results(pTree t, int root, int end, TaskTree tasktree) {
    this->t = t;
    this->root = root;
    this->end = end;
    this->tasktree = tasktree;
  }
};


//The value bound to var, or nullptr. For the hot paths, which used
//return_value below: it returned a copy, and a "__CONST__" string to compare
//against, for every argument of every subtask grounded (8.24).
inline std::string const* bound_value(std::string const& var, Args const& args) {
  for (auto const& a : args) {
    if (var == a.first) {
      return &a.second;
    }
  }
  return nullptr;
}

inline std::string return_value(std::string const& var, Args const& args) {
  for (auto const& a : args) {
    if (var == a.first) {
      return a.second;
    }
  }
  return "__CONST__";
}

inline Pred create_predicate(std::string head, Params& params) {
  Pred pred;
  pred.first = head;
  pred.second = params;
  return pred;
}

class ActionDef {
  private:
    std::string head;
    Params parameters;
    Preconds preconditions;
    expr::Ptr precondition_ast;
    Effects effects;
    //True for the effect-free actions the loader synthesises to carry a
    //method's precondition under HDDL timing. They are real search steps, but
    //they are not part of the plan the user asked for, so they are kept out of
    //it -- otherwise they would inflate plan length and every score function
    //that looks at it.
    bool artificial = false;

    KnowledgeBase apply_binding(KnowledgeBase& kb, Args const& args) {
      KnowledgeBase new_kb = kb;
      //An effect atom's arguments: each variable's value from the first
      //binding that has it, and a constant as itself. Pointers, not copies,
      //handed to set_fact, which looks the ids up directly. This used to build
      //the fact as text for tell to parse back apart (8.24).
      std::vector<std::string const*> vals;
      auto fact_of = [&vals](Pred const& pred, Args const& first, Args const* second) {
        vals.clear();
        for (auto const& p : pred.second) {
          auto const* v = bound_value(p.first,first);
          if (!v && second) {
            v = bound_value(p.first,*second);
          }
          vals.push_back(v ? v : &p.first);
        }
        return vals;
      };
      for (auto const& e : this->effects) {
        if (e.forall.empty()) {
          if (e.condition == "__NONE__") {
            new_kb.set_fact(e.pred.first,fact_of(e.pred,args,nullptr),e.remove,false);
          }
          else {
            Args wc_fixed;
            wc_fixed.reserve(args.size());
            for (size_t i = 0; i < args.size(); i++) {
              wc_fixed.push_back({this->parameters[i].first,args[i].second});
            }
            //The SMT text only if the evaluator declines and Z3 is asked.
            auto wc = [&]() {
              if (args.empty()) {
                return e.condition;
              }
              std::string w = "(and ";
              for (size_t i = 0; i < args.size(); i++) {
                w += "(= "+this->parameters[i].first+" "+args[i].second+") ";
              }
              return w + e.condition + ")";
            };
            auto pass = eval::solve_query_lazy(new_kb,e.condition_ast,wc,
                                               this->parameters,wc_fixed,
                                               "conditional effect");
            if (!pass.empty()) {
              new_kb.set_fact(e.pred.first,fact_of(e.pred,args,nullptr),e.remove,false);
            }
          }
        }
        else {
          //Copied as before: the range is built by iterating this copy, and
          //the order it iterates in decides the order facts are added, which
          //later enumeration -- and so the random stream -- depends on.
          auto faparams = e.forall;
          if (e.condition == "__NONE__") {
            Params params;
            std::string vt = "(and";
            std::vector<expr::Ptr> vt_parts;
            for (auto const& [var,types] : faparams) {
              params.push_back({var,"__Object__"});
              for (auto const& t : types) {
                vt += " ("+t+" "+var+")";
                vt_parts.push_back(expr::make_atom(t,{{var,true}}));
              }
            }
            vt += ")";
            auto vt_ast = vt_parts.empty() ? nullptr
                                           : expr::make_connective(expr::Kind::And,vt_parts);
            auto bindings = eval::solve_query(new_kb,vt_ast,vt,params,Args{},
                                              "forall range");
            for (auto const& b : bindings) {
              new_kb.set_fact(e.pred.first,fact_of(e.pred,b,&args),e.remove,false);
            }
          }
          else {
            Params params;
            std::string vt = "(and";
            std::vector<expr::Ptr> vt_parts;
            for (auto const& [var,types] : faparams) {
              params.push_back({var,"__Object__"});
              for (auto const& t : types) {
                vt += " ("+t+" "+var+")";
                vt_parts.push_back(expr::make_atom(t,{{var,true}}));
              }
            }
            vt += ")";
            auto vt_ast = vt_parts.empty() ? nullptr
                                           : expr::make_connective(expr::Kind::And,vt_parts);

            //A quantified variable shadows an action parameter of the same
            //name, so drop those parameters and let the binding supply the
            //value instead.
            Args base_args;
            for (auto const& a : args) {
              if (!faparams.contains(a.first)) {
                base_args.push_back(a);
              }
            }
            //The condition mentions the quantified variables, so they have to
            //be declared in the query that evaluates it. Previously only the
            //action's own parameters were, and Z3 rejected the condition with
            //"unknown constant" for every binding, so this effect never fired
            //at all.
            Params condP;
            for (auto const& p : this->parameters) {
              if (!faparams.contains(p.first)) {
                condP.push_back(p);
              }
            }
            for (auto const& [var,types] : faparams) {
              condP.push_back(std::make_pair(var,"__Object__"));
            }

            auto bindings = eval::solve_query(new_kb,vt_ast,vt,params,Args{},
                                              "forall range");
            for (auto const& b : bindings) {
              //Rebuilt per binding. Appending each binding onto a shared list
              //left the previous binding's equalities in place, so from the
              //second object on the condition was self-contradictory.
              Args b_args = base_args;
              for (auto const& b_arg : b) {
                b_args.push_back(b_arg);
              }
              auto wc = [&]() {
                if (b_args.empty()) {
                  return e.condition;
                }
                std::string w = "(and ";
                for (auto const& a : b_args) {
                  w += "(= "+a.first+" "+a.second+") ";
                }
                return w + e.condition + ")";
              };
              auto pass = eval::solve_query_lazy(new_kb,e.condition_ast,wc,condP,b_args,
                                                 "forall conditional effect");
              if (!pass.empty()) {
                new_kb.set_fact(e.pred.first,fact_of(e.pred,b,&b_args),e.remove,false);
              }
            }
          }
        }
      }
      return new_kb;
    }

  public:
    ActionDef(std::string head,
              Params parameters,
              Preconds preconditions,
              Effects effects,
              bool artificial = false,
              expr::Ptr precondition_ast = nullptr) {
      this->head = head;
      this->parameters = parameters;
      this->preconditions = preconditions;
      this->effects = effects;
      this->artificial = artificial;
      this->precondition_ast = precondition_ast;
    }

    //The structured form of `preconditions`. Nothing evaluates it yet; it is
    //here so that the direct evaluator can.
    expr::Ptr get_precondition_ast() {
      return this->precondition_ast;
    }

    //Does the precondition hold for these arguments? The check a synthesised
    //precondition action makes, without building the successor state that
    //apply() would -- used to re-evaluate a method's precondition later.
    bool holds(KnowledgeBase& kb, Args const& args) {
      Args fixed;
      fixed.reserve(args.size());
      for (size_t i = 0; i < args.size(); i++) {
        fixed.push_back({this->parameters[i].first,args[i].second});
      }
      auto direct = eval::ask_any(kb,this->precondition_ast,this->parameters,fixed);
      if (direct) {
        return *direct;
      }
      std::string pc = "(and ";
      for (size_t i = 0; i < args.size(); i++) {
        pc += "(= "+this->parameters[i].first+" "+args[i].second+") ";
      }
      pc += this->preconditions != "__NONE__" ? this->preconditions+")" : ")";
      return kb.ask_any(pc,this->parameters);
    }

    bool is_artificial() {
      return this->artificial;
    }

    std::string get_head() {
      return this->head;
    }

    Params get_parameters() {
      return this->parameters;
    }

    Preconds get_preconditions() {
      return this->preconditions;
    }

    Effects get_effects() {
      return this->effects;
    }

    std::pair<task_token,std::vector<KnowledgeBase>> apply(KnowledgeBase& kb, Args const& args) {
      //args is indexed in lockstep with this->parameters, so a task invoking
      //this action with the wrong arity would read past the end of parameters.
      if (!args.empty() && args.size() != this->parameters.size()) {
        throw std::logic_error("Action "+this->head+" applied with "+
                               std::to_string(args.size())+" arguments but declared with "+
                               std::to_string(this->parameters.size())+" parameters!");
      }
      //The SMT text of the precondition with the arguments pinned. Built only
      //if something needs it -- the Z3 fallback, or the differential build --
      //which on the default build is almost never. It used to be built on every
      //application, and it copies the whole precondition text each time.
      auto pc_of = [&]() {
        if (args.empty()) {
          return this->preconditions;
        }
        std::string pc = "(and ";
        for (size_t i = 0; i < args.size(); i++) {
          pc += "(= "+this->parameters[i].first+" "+args[i].second+") ";
        }
        return pc + (this->preconditions != "__NONE__" ? this->preconditions+")" : ")");
      };

      std::vector<KnowledgeBase> new_states = {};

      //What the `(= param value)` prefix of pc says, as an environment: the
      //direct evaluator takes the pinned parameters as bindings rather than as
      //conjuncts to re-derive.
      Args fixed;
      fixed.reserve(args.size());
      for (size_t i = 0; i < args.size(); i++) {
        fixed.push_back({this->parameters[i].first,args[i].second});
      }

      //A synthesised method-precondition check has no effects and arrives with
      //every parameter already pinned by args, so at most one binding can
      //satisfy pc and the resulting state is just the current one. Asking for
      //satisfiability skips enumerating that single model, which matters
      //because HDDL timing puts one of these in front of every method.
      //The token is empty for a synthesised check: it is a search step, never
      //a plan step, and both callers that read a token skip artificial actions
      //before doing so. Its head is also the longest string in the domain --
      //"__mprec_" plus a method name, 28 to 36 characters, past the
      //small-string buffer -- so building it was a heap allocation per check,
      //and HDDL timing puts one in front of every method.
      if (this->artificial) {
        auto direct = eval::ask_any(kb,this->precondition_ast,this->parameters,fixed);
        bool holds;
        if (direct) {
          holds = *direct;
        }
        else {
          std::string pc = pc_of();
          holds = (pc == "__NONE__" || kb.ask_any(pc,this->parameters));
        }
        if (holds) {
          new_states.push_back(kb);
        }
        return std::make_pair(task_token{},new_states);
      }

      auto bindings = eval::solve_query_lazy(kb,this->precondition_ast,pc_of,
                                             this->parameters,fixed,
                                             this->head.c_str());
      if (bindings.empty()) {
        //No successor, so nobody reads the token.
        return std::make_pair(task_token{},new_states);
      }
      for (auto &b : bindings) {
        new_states.push_back(this->apply_binding(kb,b));
      }
      std::string token = "("+this->head;
      for (auto const& a : args) {
        token += " "+a.second;
      }
      token += ")";
      return std::make_pair(token,new_states);
    }

};

class MethodDef {
  private:
    std::string head;
    TaskDef task; 
    Params parameters;
    Preconds preconditions;
    expr::Ptr precondition_ast;
    TaskDefs subtasks;
    std::unordered_map<std::string,std::vector<std::string>> orderings;

  public:
    MethodDef() {}
    MethodDef(std::string head, 
              TaskDef task, 
              Params parameters, 
              Preconds preconditions, 
              TaskDefs subtasks, 
              std::unordered_map<std::string,std::vector<std::string>> orderings,
              expr::Ptr precondition_ast = nullptr) {
      this->head = head;
      this->task = task;
      this->parameters = parameters;
      this->preconditions = preconditions;
      this->subtasks = subtasks;
      this->orderings = orderings;
      this->precondition_ast = precondition_ast;
    }

    //Structured form of `preconditions`, which for a method is its
    //:constraints (the state precondition moved into a synthesised action).
    expr::Ptr get_precondition_ast() {
      return this->precondition_ast;
    }

    std::string get_head() {
      return this->head;
    }

    TaskDef get_task() {
      return this->task;
    }

    Params get_parameters() {
      return this->parameters;
    }

    Preconds get_preconditions() {
      return this->preconditions;
    }

    TaskDefs get_subtasks() {
      return this->subtasks;
    }
    
    std::unordered_map<std::string,std::vector<std::string>> get_orderings() {
      return this->orderings;
    }

    //tasks arrives by value and is always moved in by apply(), so taking it
    //this way costs nothing; the return moves it back out for the same reason.
    std::pair<std::vector<int>,TaskGraph> apply_binding(Args const& args, TaskGraph tasks, std::vector<int>& out,
                                                        std::vector<int> const& inherited = {},
                                                        PreconditionMode mode = PreconditionMode::Compiled) {
      //Subtask label -> the task id it was given. A method has a handful of
      //subtasks, so a linear scan over a vector: the unordered_map this was
      //cost a bucket array, a node per subtask and a hash per lookup, on every
      //decomposition of every rollout (8.24).
      std::vector<std::pair<std::string const*,int>> gts;
      gts.reserve(this->subtasks.size());
      auto gts_find = [&gts](std::string const& label) -> int {
        for (auto const& [l,tid] : gts) {
          if (*l == label) {
            return tid;
          }
        }
        return -1;
      };
      std::vector<int> addedTIDs;
      addedTIDs.reserve(this->subtasks.size());

      //Outside Compiled mode, record this method's precondition as a check its
      //subtasks carry. The loader compiled it into a synthesised subtask
      //labelled "__mprec__", so that subtask's action and its grounded
      //arguments are exactly what re-evaluating it needs.
      int own_check = -1;
      if (mode != PreconditionMode::Compiled) {
        auto pre = this->subtasks.find("__mprec__");
        if (pre != this->subtasks.end()) {
          auto def = std::make_shared<CheckDef>();
          def->action = pre->second.first;
          def->args.reserve(pre->second.second.size());
          for (auto const& pt : pre->second.second) {
            auto const* val = bound_value(pt.first,args);
            def->args.emplace_back(pt.first,val ? *val : pt.first);
          }
          PendingCheck chk;
          chk.def = std::move(def);
          own_check = (int)tasks.checks.size();
          tasks.checks.push_back(std::move(chk));
        }
      }

      for (auto const& [id,s]: this->subtasks) {
        Grounded_Task gt;
        gt.head = s.first;
        gt.args.reserve(s.second.size());
        for (auto const& pt : s.second) {
          auto const* val = bound_value(pt.first,args);
          gt.args.emplace_back(pt.first,val ? *val : pt.first);
        }
        int tid = tasks.add_node(std::move(gt));
        gts.emplace_back(&id,tid);
        if (mode != PreconditionMode::Compiled) {
          //Every subtask arises from this method and from every method the
          //decomposed task itself arose from.
          TaskGraph::TaskTags tg;
          tg.checks = inherited;
          if (own_check >= 0) {
            tg.checks.push_back(own_check);
          }
          if (id == "__mprec__") {
            tg.establishes = own_check;
          }
          tasks.set_tags(tid,std::move(tg));
        }
        addedTIDs.push_back(tid);
        //find, not operator[]: the latter would insert an empty ordering into
        //this->orderings for every subtask label it is asked about.
        auto ord = this->orderings.find(id);
        if (ord == this->orderings.end() || ord->second.empty()) {
          for (auto const& o : out) {
            tasks.add_edge(tid,o);
          }
        }
      }
      for (auto const &[t1,ot] : this->orderings) {
        for (auto const &t2 : ot) {
          //An ordering naming a label that is not one of this method's subtasks
          //would silently resolve through gts' operator[] to task id 0 and wire
          //up an edge to an unrelated task.
          int g1 = gts_find(t1);
          int g2 = gts_find(t2);
          if (g1 < 0 || g2 < 0) {
            throw std::logic_error("Method "+this->head+" has an ordering constraint between "+
                                   t1+" and "+t2+", which are not both subtasks of it!");
          }
          tasks.add_edge(g1,g2);
        }
      }
      //Moved, not copied: make_pair on the named locals copied the whole network.
      return std::make_pair(std::move(addedTIDs),std::move(tasks));
    }

    //tasks is taken by const reference and never modified. It used to be taken
    //by value, then copied again into apply_binding per binding, and again by
    //apply_binding's return -- 2N+1 copies of the whole task network for N
    //bindings, where N is the floor, since each binding yields a successor
    //network of its own. It also means `args`, which callers pass as a
    //reference *into* this same network, can no longer be invalidated.
    std::vector<std::pair<std::vector<int>,TaskGraph>> apply(KnowledgeBase& kb, Args const& args, TaskGraph const& tasks, int i,
                                                            PreconditionMode mode = PreconditionMode::Compiled) {
      //args is indexed in lockstep with this->task.second, so a task invoked
      //with the wrong arity would read past the end of the task's parameters.
      if (!args.empty() && args.size() != this->task.second.size()) {
        throw std::logic_error("Task "+this->task.first+" in method "+this->head+" applied with "+
                               std::to_string(args.size())+" arguments but declared with "+
                               std::to_string(this->task.second.size())+" parameters!");
      }
      //As in ActionDef::apply, the SMT text is built only if the Z3 fallback
      //or the differential build asks for it. (This function also used to
      //build a "(task args...)" token on every call and never read it.)
      auto pc_of = [&]() {
        if (args.empty()) {
          return this->preconditions;
        }
        std::string pc = "(and ";
        for (size_t k = 0; k < args.size(); k++) {
          pc += "(= "+this->task.second[k].first+" "+args[k].second+") ";
        }
        return pc + (this->preconditions != "__NONE__" ? this->preconditions+")" : ")");
      };
      std::vector<std::pair<std::vector<int>,TaskGraph>> groundings;
      //As in ActionDef::apply: the `(= param value)` prefix becomes an
      //environment rather than part of the formula.
      Args fixed;
      fixed.reserve(args.size());
      for (size_t k = 0; k < args.size(); k++) {
        fixed.push_back({this->task.second[k].first,args[k].second});
      }
      auto bindings = eval::solve_query_lazy(kb,this->precondition_ast,pc_of,
                                             this->parameters,fixed,
                                             this->head.c_str());
      if (bindings.empty()) {
        return groundings;
      }
      std::vector<int> out = tasks.at(i).outgoing;
      //The checks the decomposed task carries pass to its subtasks.
      std::vector<int> inherited = tasks.checks_of(i);
      groundings.reserve(bindings.size());
      for (auto &b : bindings) {
        TaskGraph g = tasks;          //the one copy each successor needs
        g.remove_node(i);
        groundings.push_back(this->apply_binding(b,std::move(g),out,inherited,mode));
      }
      return groundings;
    }
};

using ActionDefs = std::unordered_map<std::string, ActionDef>;
//{Task,vector of the tasks Methods}
using MethodDefs = std::unordered_map<std::string, std::vector<MethodDef>>;

struct DomainDef {
  std::string head;
  TypeTree typetree;
  Predicates predicates;
  ActionDefs actions;
  MethodDefs methods;
  Objects constants;
  Scorer scorer;
  //How method preconditions are read. AtStart by default, which is stricter
  //than HDDL: it rejects plans HDDL's compiled semantics accepts. See 8.16.
  PreconditionMode precondition_mode = PreconditionMode::AtStart;
  //Where cppMCTShop narrates: the initial and final states, the plan, and any
  //note about rollouts cut off by the depth bound. Standard output by default,
  //which is what MCTS_planner shows; nullptr for none, which a program using
  //the planner as a library will usually want (planner_doc.md 8.20).
  std::ostream* narration = &std::cout;
  DomainDef(std::string head,
            TypeTree typetree,
            Predicates predicates,
            Objects constants,
            ActionDefs actions,
            MethodDefs methods) {
    this->head = head;
    this->typetree = typetree;
    this->predicates = predicates;
    this->constants = constants;
    this->actions = actions;
    this->methods = methods;
  }
  void set_scorer(Scorer s) {
    this->scorer = s;
  }


  double score(KnowledgeBase& state, std::vector<std::string>& plan) {
    return this->scorer(state,plan); 
  }

};

struct ProblemDef {
  std::string head;
  std::string domain_name;
  Objects objects;
  MethodDef initM;
  std::vector<std::string> initF;
  std::string goal;
  ProblemDef(std::string head,
             std::string domain_name,
             Objects objects,
             MethodDef initM,
             std::vector<std::string> initF,
             std::string goal = "") {
    this->head = head;
    this->domain_name = domain_name;
    this->objects = objects;
    this->initM = initM;
    this->initF = initF;
    this->goal = goal;
  } 
};
