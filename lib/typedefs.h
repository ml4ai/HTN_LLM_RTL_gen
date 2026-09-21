#pragma once

#include <string>
#include <tuple>
#include <unordered_map>
#include <queue>
#include <map>
#include <vector>
#include <iterator>
#include "kb.h"
#include <optional>
#include "util.h"
#include <boost/variant/recursive_wrapper.hpp>
#include <boost/json.hpp>

namespace json = boost::json;

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
  effect (std::string condition, bool remove, Pred pred, std::unordered_map<std::string,std::unordered_set<std::string>> forall) {
    this->condition = condition;
    this->remove = remove;
    this->pred = pred;
    this->forall = forall;
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
  std::unordered_map<int,Grounded_Task> GTs;
  //Keeps track of next newly usable ID
  int nextID = 0;

  Grounded_Task& operator[](int i) {
    return this->GTs[i];
  }

  int add_node(Grounded_Task& GT) {
    int id;
    id = this->nextID;
    this->nextID++;
    this->GTs[id] = GT;
    return id;
  }

  // gt1 -> gt2
  void add_edge(int gt1, int gt2) {
    this->GTs[gt1].outgoing.push_back(gt2);
    this->GTs[gt2].incoming.push_back(gt1);
  } 

  void remove_node(int gt) {
    for (auto &og : this->GTs[gt].outgoing) {
      this->GTs[og].incoming.erase(std::remove(GTs[og].incoming.begin(),GTs[og].incoming.end(),gt),GTs[og].incoming.end());
    }
    for (auto &ic : this->GTs[gt].incoming) {
      this->GTs[ic].outgoing.erase(std::remove(GTs[ic].outgoing.begin(),GTs[ic].outgoing.end(),gt),GTs[ic].outgoing.end());
    }
    this->GTs.erase(gt);
  }
  
  bool empty() {
    return this->GTs.empty();
  } 

  int size() {
    return this->GTs.size();
  }

};

void tag_invoke(const json::value_from_tag&, json::value& jv, Grounded_Task const& gt) {
  json::array sargs;
  for (auto const& a : gt.args) {
    json::array sa;
    sa.emplace_back(a.first);
    sa.emplace_back(a.second);
    sargs.emplace_back(sa);
  }

  json::array sincoming;
  for (auto const& i : gt.incoming) {
    sincoming.emplace_back(std::to_string(i));
  }

  json::array soutgoing;
  for (auto const& o : gt.outgoing) {
    soutgoing.emplace_back(std::to_string(o));
  }

  jv = {
      {"head", gt.head},
      {"args", sargs},
      {"incoming", sincoming},
      {"outgoing",soutgoing}
  }; 

}

Grounded_Task tag_invoke(const json::value_to_tag<Grounded_Task>&,json::value const& jv) {
  Grounded_Task gt; 
  json::object ob = jv.as_object();
  gt.head = json::value_to<std::string>(ob["head"]);

  for (auto const& a : ob["args"].as_array()) {
    std::pair<std::string,std::string> p;
    p.first = json::value_to<std::string>(a.as_array()[0]);
    p.second = json::value_to<std::string>(a.as_array()[1]);
    gt.args.push_back(p);
  }

  for (auto const& i : ob["incoming"].as_array()) {
    std::string si = json::value_to<std::string>(i);
    gt.incoming.push_back(std::stoi(si));
  }

  for (auto const& o : ob["outgoing"].as_array()) {
    std::string so = json::value_to<std::string>(o);
    gt.outgoing.push_back(std::stoi(so));
  }

  return gt;
}

void tag_invoke(const json::value_from_tag&, json::value& jv, TaskGraph const& tg) {
  std::unordered_map<std::string, Grounded_Task> sGTs;
  for (auto const& [id,n] : tg.GTs) {
    sGTs[std::to_string(id)] = n;
  }
  jv = {
      {"GTs",sGTs},
      {"nextID",std::to_string(tg.nextID)}
  };
}

TaskGraph tag_invoke(const json::value_to_tag<TaskGraph>&,json::value const& jv) {
  TaskGraph tg; 
  json::object ob = jv.as_object();
  for (auto const& [id,n] : ob["GTs"].as_object()) {
    std::string sid {id};
    tg[std::stoi(sid)] = json::value_to<Grounded_Task>(n);
  }
  std::string snextID = json::value_to<std::string>(ob["nextID"]);
  tg.nextID = std::stoi(snextID);
  return tg;
}

struct TaskNode {
  std::string task;
  std::string token;
  std::vector<int> children;
  std::vector<int> outgoing;
};

void tag_invoke(const json::value_from_tag&, json::value& jv, TaskNode const& t) {
  json::array schildren;
  for (auto const& c : t.children) {
    schildren.emplace_back(std::to_string(c));
  } 
  json::array soutgoing;
  for (auto const& o : t.outgoing) {
    soutgoing.emplace_back(std::to_string(o));
  }
  jv = {
      {"task",t.task},
      {"token",t.token},
      {"children",schildren},
      {"outgoing",soutgoing}
  };
}

using TaskTree = std::unordered_map<int,TaskNode>;

void tag_invoke(const json::value_from_tag&, json::value& jv, TaskTree const& t) {
  std::unordered_map<std::string, TaskNode> stasktree;
  for (auto const& [id,n] : t) {
    stasktree[std::to_string(id)] = n;
  }
  jv = json::value_from(stasktree);
}

struct pNode {
    KnowledgeBase state;
    TaskGraph tasks;
    int prevTID = -1;
    std::vector<int> addedTIDs;
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


std::string return_value(std::string var, Args& args) {
  for (auto const& a : args) {
    if (var == a.first) {
      return a.second;
    }
  }
  return "__CONST__";
}

Pred create_predicate(std::string head, Params& params) {
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
    Effects effects;
    //True for the effect-free actions the loader synthesises to carry a
    //method's precondition under HDDL timing. They are real search steps, but
    //they are not part of the plan the user asked for, so they are kept out of
    //it -- otherwise they would inflate plan length and every score function
    //that looks at it.
    bool artificial = false;

    KnowledgeBase apply_binding(KnowledgeBase& kb, Args& args) {
      KnowledgeBase new_kb = kb;
      for (auto const& e : this->effects) {
        auto faparams = e.forall;
        if (faparams.empty()) {
          if (e.condition == "__NONE__") {
            auto pred = e.pred;
            std::string et = "("+pred.first;
            for (auto const& p : pred.second) {
              auto val = return_value(p.first,args);
              if (val == "__CONST__") {
                et += " "+p.first;
              }
              else {
                et += " "+val;
              }
            }
            new_kb.tell(et+")",e.remove,false);
          }
          else {
            std::string wc;
            if (!args.empty()) {
              wc = "(and ";
              for (int i = 0; i < args.size(); i++) {
                wc += "(= "+this->parameters[i].first+" "+args[i].second+") ";
              }
              wc += e.condition + ")";
            }
            else {
              wc = e.condition;
            }
            auto pass = new_kb.ask(wc,this->parameters);
            if (!pass.empty()) {
              auto pred = e.pred;
              std::string et = "("+pred.first;
              for (auto const& p : pred.second) {
                auto val = return_value(p.first,args);
                if (val == "__CONST__") {
                  et += " "+p.first;
                }
                else {
                  et += " "+val;
                }
              }
              new_kb.tell(et+")",e.remove,false);
            }
          }
        }
        else {
          if (e.condition == "__NONE__") {
            Params params;
            std::string vt = "(and";
            for (auto const& [var,types] : faparams) {
              std::pair<std::string,std::string> arg;
              arg.first = var;
              arg.second = "__Object__";
              params.push_back(arg);
              for (auto const& t : types) {
                vt += " ("+t+" "+var+")";
              }
            }
            vt += ")";
            auto bindings = new_kb.ask(vt,params);
            for (auto &b : bindings) {
              auto pred = e.pred;
              std::string et = "("+pred.first;
              for (auto const& p : pred.second) {
                auto bval = return_value(p.first,b);
                if (bval == "__CONST__") {
                  auto val = return_value(p.first,args); 
                  if (val == "__CONST__") {
                    et += " "+p.first;
                  }
                  else {
                    et += " "+val;
                  }
                }
                else {
                  et += " "+bval;
                }
              }
              new_kb.tell(et+")",e.remove,false);
            }
          }
          else {
            Params params;
            std::string vt = "(and";
            for (auto const& [var,types] : faparams) {
              std::pair<std::string,std::string> arg;
              arg.first = var;
              arg.second = "__Object__";
              params.push_back(arg);
              for (auto const& t : types) {
                vt += " ("+t+" "+var+")";
              }
            }
            vt += ")";

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

            auto bindings = new_kb.ask(vt,params);
            for (auto &b : bindings) {
              //Rebuilt per binding. Appending each binding onto a shared list
              //left the previous binding's equalities in place, so from the
              //second object on the condition was self-contradictory.
              Args b_args = base_args;
              for (auto const& b_arg : b) {
                b_args.push_back(b_arg);
              }
              std::string wc;
              if (!b_args.empty()) {
                wc = "(and ";
                for (auto const& a : b_args) {
                  wc += "(= "+a.first+" "+a.second+") ";
                }
                wc += e.condition + ")";
              }
              else {
                wc = e.condition;
              }
              auto pass = new_kb.ask(wc,condP);
              if (!pass.empty()) {
                auto pred = e.pred;
                std::string et = "("+pred.first;
                for (auto const& p : pred.second) {
                  auto bval = return_value(p.first,b);
                  if (bval == "__CONST__") {
                    auto val = return_value(p.first,b_args);
                    if (val == "__CONST__") {
                      et += " "+p.first;
                    }
                    else {
                      et += " "+val;
                    }
                  }
                  else {
                    et += " "+bval;
                  }
                }
                new_kb.tell(et+")",e.remove,false);
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
              bool artificial = false) {
      this->head = head;
      this->parameters = parameters;
      this->preconditions = preconditions;
      this->effects = effects;
      this->artificial = artificial;
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

    std::pair<task_token,std::vector<KnowledgeBase>> apply(KnowledgeBase& kb, Args& args) {
      std::string pc;
      std::string token = "("+this->head;
      if (!args.empty()) {
        //args is indexed in lockstep with this->parameters, so a task invoking
        //this action with the wrong arity would read past the end of parameters.
        if (args.size() != this->parameters.size()) {
          throw std::logic_error("Action "+this->head+" applied with "+
                                 std::to_string(args.size())+" arguments but declared with "+
                                 std::to_string(this->parameters.size())+" parameters!");
        }
        pc = "(and ";
        for (int i = 0; i < args.size(); i++) {
          pc += "(= "+this->parameters[i].first+" "+args[i].second+") ";
          token += " "+args[i].second;
        }
        if (this->preconditions != "__NONE__") {
          pc += this->preconditions + ")";
        }
        else {
          pc += ")";
        }
      }
      else {
        pc = this->preconditions;
      }
      token += ")";

      std::vector<KnowledgeBase> new_states = {};

      //A synthesised method-precondition check has no effects and arrives with
      //every parameter already pinned by args, so at most one binding can
      //satisfy pc and the resulting state is just the current one. Asking for
      //satisfiability skips enumerating that single model, which matters
      //because HDDL timing puts one of these in front of every method.
      if (this->artificial) {
        if (pc == "__NONE__" || kb.ask_any(pc,this->parameters)) {
          new_states.push_back(kb);
        }
        return std::make_pair(token,new_states);
      }

      if (pc != "__NONE__") {
        if (this->parameters.empty()) {
          auto pass = kb.ask(pc);
          if (pass) {
            Args b = {};
            new_states.push_back(this->apply_binding(kb,b));
          }
        }
        else {
          auto bindings = kb.ask(pc,this->parameters);
          for (auto &b : bindings) {
            new_states.push_back(this->apply_binding(kb,b)); 
          }
        }
      }
      else {
        if (this->parameters.empty()) {
          Args b = {};
          new_states.push_back(this->apply_binding(kb,b));
        }
        else {
          auto bindings = kb.ask("",this->parameters);
          for (auto &b : bindings) {
            new_states.push_back(this->apply_binding(kb,b));
          }
        }
      }
      return std::make_pair(token,new_states);
    }

    std::vector<std::pair<Args,KnowledgeBase>> apply(KnowledgeBase& kb) {
      std::vector<std::pair<Args,KnowledgeBase>> new_states = {};
      if (this->preconditions != "__NONE__") {
        if (this->parameters.empty()) {
          auto pass = kb.ask(this->preconditions);
          if (pass) {
            Args b = {};
            std::pair<Args,KnowledgeBase> pr;
            pr.first = b;
            pr.second = this->apply_binding(kb,b);
            new_states.push_back(pr);
          }
        }
        else {
          auto bindings = kb.ask(this->preconditions,this->parameters);
          for (auto &b : bindings) {
            std::pair<Args,KnowledgeBase> pr;
            pr.first = b;
            pr.second = this->apply_binding(kb,b);
            new_states.push_back(pr); 
          }
        }
      }
      else {
        if (this->parameters.empty()) {
          Args b = {};
          std::pair<Args,KnowledgeBase> pr;
          pr.first = b;
          pr.second = this->apply_binding(kb,b);
          new_states.push_back(pr);
        }
        else {
          auto bindings = kb.ask("",this->parameters);
          for (auto &b : bindings) {
            std::pair<Args,KnowledgeBase> pr;
            pr.first = b;
            pr.second = this->apply_binding(kb,b);
            new_states.push_back(pr);
          }
        }
      }
      return new_states;
    }

};

class MethodDef {
  private:
    std::string head;
    TaskDef task; 
    Params parameters;
    Preconds preconditions;
    TaskDefs subtasks;
    std::unordered_map<std::string,std::vector<std::string>> orderings;

  public:
    MethodDef() {}
    MethodDef(std::string head, 
              TaskDef task, 
              Params parameters, 
              Preconds preconditions, 
              TaskDefs subtasks, 
              std::unordered_map<std::string,std::vector<std::string>> orderings) {
      this->head = head;
      this->task = task;
      this->parameters = parameters;
      this->preconditions = preconditions;
      this->subtasks = subtasks;
      this->orderings = orderings;
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

    std::pair<std::vector<int>,TaskGraph> apply_binding(Args& args, TaskGraph tasks, std::vector<int>& out) {
      std::unordered_map<std::string,int> gts;
      std::vector<int> addedTIDs;
      for (auto const& [id,s]: this->subtasks) {
        Grounded_Task gt;
        gt.head = s.first;
        for (auto const& pt : s.second) {
          std::pair<std::string,std::string> arg;
          arg.first = pt.first;
          std::string val = return_value(pt.first,args);
          if (val == "__CONST__") {
            arg.second = pt.first;
          }
          else {
            arg.second = val;
          }
          gt.args.push_back(arg);
        }
        gts[id] = tasks.add_node(gt);
        addedTIDs.push_back(gts[id]);
        //find, not operator[]: the latter would insert an empty ordering into
        //this->orderings for every subtask label it is asked about.
        auto ord = this->orderings.find(id);
        if (ord == this->orderings.end() || ord->second.empty()) {
          for (auto const& o : out) {
            tasks.add_edge(gts[id],o);
          }
        }
      }
      for (auto const &[t1,ot] : this->orderings) {
        for (auto const &t2 : ot) {
          //An ordering naming a label that is not one of this method's subtasks
          //would silently resolve through gts' operator[] to task id 0 and wire
          //up an edge to an unrelated task.
          auto g1 = gts.find(t1);
          auto g2 = gts.find(t2);
          if (g1 == gts.end() || g2 == gts.end()) {
            throw std::logic_error("Method "+this->head+" has an ordering constraint between "+
                                   t1+" and "+t2+", which are not both subtasks of it!");
          }
          tasks.add_edge(g1->second,g2->second);
        }
      }
      return std::make_pair(addedTIDs,tasks);
    }

    std::vector<std::pair<std::vector<int>,TaskGraph>> apply(KnowledgeBase& kb, Args& args, TaskGraph tasks, int i) {
      std::string pc;
      std::string token = "("+this->task.first;
      if (!args.empty()) {
        //args is indexed in lockstep with this->task.second, so a task invoked
        //with the wrong arity would read past the end of the task's parameters.
        if (args.size() != this->task.second.size()) {
          throw std::logic_error("Task "+this->task.first+" in method "+this->head+" applied with "+
                                 std::to_string(args.size())+" arguments but declared with "+
                                 std::to_string(this->task.second.size())+" parameters!");
        }
        pc = "(and ";
        for (int i = 0; i < args.size(); i++) {
          pc += "(= "+this->task.second[i].first+" "+args[i].second+") ";
          token += " "+args[i].second;
        }
        if (this->preconditions != "__NONE__") {
          pc += this->preconditions + ")";
        }
        else {
          pc += ")";
        }
      }
      else {
        pc = this->preconditions;
      }
      token += ")";
      std::vector<std::pair<std::vector<int>,TaskGraph>> groundings;
      std::vector<int> out = tasks[i].outgoing;
      tasks.remove_node(i);
      if (pc != "__NONE__") {
        if (this->parameters.empty()) {
          auto pass = kb.ask(pc);
          if (pass) {
            Args b = {};
            groundings.push_back(this->apply_binding(b,tasks,out));
          }
        }
        else {
          auto bindings = kb.ask(pc,this->parameters);
          for (auto &b : bindings) {
            groundings.push_back(this->apply_binding(b,tasks,out)); 
          }
        }
      }
      else {
        if (this->parameters.empty()) {
          Args b = {};
          groundings.push_back(this->apply_binding(b,tasks,out));
        }
        else {
          auto bindings = kb.ask("",this->parameters);
          for (auto &b : bindings) {
            groundings.push_back(this->apply_binding(b,tasks,out));
          }
        }
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
  void set_scorer(Scorer scorer) {
    this->scorer = scorer;
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
