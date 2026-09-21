#pragma once

#include "../util.h"
#include "../typedefs.h"
#include <algorithm>
#include <any>
#include <iostream>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#include <limits>
#include <chrono>

int
selection(pTree& t,
          int v,
          double c,
          std::mt19937_64& g) {
    int original = v;
    if (!t[v].unexplored.empty()) {
      int w = t[v].unexplored.back();
      t[v].unexplored.pop_back();
      t[v].successors.push_back(w);
      return w;
    } 
    while (!t[v].successors.empty()) {
      if (t[v].deadend) {
        return v;
      }
      std::vector<int> maxes = {};
      double max = -std::numeric_limits<double>::infinity();
      for (auto const &w : t[v].successors) {
        if (!t[w].deadend) {
          double s = (t[w].score/t[w].sims) + c*sqrt(log(t[v].sims)/t[w].sims);
          if (s >= max) {
            if (s > max) {
              max = s;
              maxes.clear();
              maxes.push_back(w);
            }
            else {
              maxes.push_back(w);
            }
          }
        }
      }
      if (maxes.empty()) {
        t[v].deadend = true;
        v = original;
        continue;
      }
      v = *select_randomly(maxes.begin(), maxes.end(), g);
      if (!t[v].unexplored.empty()) {
        int w = t[v].unexplored.back();
        t[v].unexplored.pop_back();
        t[v].successors.push_back(w);
        return w;
      } 
    }
    return v;
}

void backprop(pTree& t, int n, double r, int sims) {
  do {
    //Accumulate everywhere. Overwriting at nodes with no successors, while
    //still adding to sims, made a terminal node selected k times report a mean
    //of score/k instead of score, so UCT progressively undervalued complete
    //plans it had already found.
    t[n].score += r;
    t[n].sims += sims;
    n = t[n].pred;
  }
  while (n != -1);
  return;
}

//Returns the score of the plan this rollout completed, or nullopt if the
//rollout could not reach a complete plan. An optional is used rather than a
//sentinel value so that a score function is free to return any double,
//including negative ones.
std::optional<double>
simulation(std::vector<std::string>& plan,
           KnowledgeBase state,
           TaskGraph tasks,
           DomainDef& domain,
           std::mt19937_64& g) {
  if (tasks.empty()) {
    return domain.score(state,plan);
  }
  std::vector<int> u;
  for (auto &[i,gt] : tasks.GTs) {
    if (gt.incoming.empty()) {
      if (domain.actions.contains(tasks[i].head)) {
        u.push_back(i);
      }
      else if (domain.methods.contains(tasks[i].head)) {
        u.push_back(i);
      }
      else {
        std::string message = "Invalid task ";
        message += tasks[i].head;
        message += " during simulation!";
        throw std::logic_error(message);
      }
    }
  }
  if (u.empty()) {
    return std::nullopt;
  }
  std::shuffle(u.begin(),u.end(),g);

  for (auto const& cTask : u) { 
    if (domain.actions.contains(tasks[cTask].head)) {
      auto& adef = domain.actions.at(tasks[cTask].head);
      auto act = adef.apply(state,tasks[cTask].args);
      if (!act.second.empty()) {
        auto gtasks = tasks;
        gtasks.remove_node(cTask);
        for (auto &ns : act.second) {
          ns.update_state();
          auto gplan = plan;
          //A synthesised method-precondition check is a search step but not a
          //plan step; keeping it out leaves plan length and the score
          //functions that read it meaning what they did before.
          if (!adef.is_artificial()) {
            gplan.push_back(act.first+"_"+std::to_string(cTask));
          }
          auto rs = simulation(gplan,ns,gtasks,domain,g);
          if (rs) {
            return rs;
          }
        }
      }
    }
    else {
      auto task_methods = domain.methods[tasks[cTask].head];
      std::shuffle(task_methods.begin(),task_methods.end(),g);
      for (auto &m : task_methods) {
        auto all_gts = m.apply(state,tasks[cTask].args,tasks,cTask);
        if (!all_gts.empty()) {
          std::shuffle(all_gts.begin(),all_gts.end(),g);
          for (auto &gts : all_gts) {
            auto rs = simulation(plan,state,gts.second,domain,g);
            if (rs) {
              return rs;
            }
          }
        }
      }
    }
  }
  return std::nullopt;
}

int expansion(pTree& t,
              int n,
              DomainDef& domain,
              std::mt19937_64& g) {
    std::vector<int> u;
    for (auto const& [id,gt] : t[n].tasks.GTs) {
      if (gt.incoming.empty()) {
        if (domain.actions.contains(t[n].tasks[id].head)) {
          u.push_back(id);
        }
        else if (domain.methods.contains(t[n].tasks[id].head)) {
          u.push_back(id);
        }
        else {
          std::string message = "Invalid task ";
          message += t[n].tasks[id].head;
          message += " during simulation!";
          throw std::logic_error(message);
        }
      }
    } 
    if (u.empty()) {
      t[n].deadend = true;
      return n;
    }
    
    for (auto const& tid : u) {
      if (domain.actions.contains(t[n].tasks[tid].head)) {
        auto& adef = domain.actions.at(t[n].tasks[tid].head);
        auto act = adef.apply(t[n].state,t[n].tasks[tid].args);
        if (!act.second.empty()) {
          for (auto const& state : act.second) {
            pNode v;
            v.state = state;
            v.state.update_state();
            v.tasks = t[n].tasks;
            v.tasks.remove_node(tid);
            v.depth = t[n].depth + 1;
            v.plan = t[n].plan;
            v.treeRoots = t[n].treeRoots;
            if (!adef.is_artificial()) {
              v.plan.push_back(act.first+"_"+std::to_string(tid));
            }
            v.pred = n;
            int w = t.size();
            t[w] = v;
            t[n].unexplored.push_back(w);
          }
        }
      }
      else {
        for (auto &m : domain.methods[t[n].tasks[tid].head]) {
          auto gts = m.apply(t[n].state,t[n].tasks[tid].args,t[n].tasks,tid);
          for (auto &g : gts) { 
            pNode v;
            v.state = t[n].state;
            v.tasks = g.second;
            v.depth = t[n].depth + 1;
            v.plan = t[n].plan;
            v.addedTIDs = g.first;
            v.prevTID = tid;
            v.treeRoots = t[n].treeRoots;
            v.pred = n;
            int w = t.size();
            t[w] = v;
            t[n].unexplored.push_back(w);
          }
        }
      }
    }
    if (!t[n].unexplored.empty()) {
      std::shuffle(t[n].unexplored.begin(),t[n].unexplored.end(),g);
      int r = t[n].unexplored.back();
      t[n].successors.push_back(r);
      t[n].unexplored.pop_back();
      return r;
    }
    t[n].deadend = true;
    return n;
}

int
seek_planMCTS(pTree& t,
              TaskTree& tasktree,
              int v,
              DomainDef& domain,
              int time_limit,
              int r,
              double c,
              std::mt19937_64& g) {
  int stuck_counter = 10;
  //One record per committed decision, so that consecutive backtracks retract
  //consecutive commits. Two scalars describing only the latest commit meant a
  //second backtrack in a row re-ran the first one's removal and left the
  //commit before it in the task tree.
  struct CommitUndo {
    int parent_TID;
    std::vector<int> added_TIDs;
  };
  std::vector<CommitUndo> undo_stack;
  //Node ids must come from a monotonic counter, not from t.size(): backtracking
  //erases nodes, after which t.size() can name a key that is still live and the
  //commit below would overwrite an existing node.
  int next_node_id = t.size();
  while (!t[v].tasks.empty()) {
    pTree m;
    pNode n_node;
    t[v].state.update_state();
    n_node.state = t[v].state;
    n_node.tasks = t[v].tasks;
    n_node.depth = t[v].depth;
    n_node.plan = t[v].plan;
    n_node.treeRoots = t[v].treeRoots;
    int w = m.size();
    m[w] = n_node;
    auto start = std::chrono::high_resolution_clock::now();
    auto stop = std::chrono::high_resolution_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(stop - start).count() < time_limit) {
      int n = selection(m,w,c,g);
      //selection only hands back a node already marked deadend when that node
      //is the root of this decision's tree, i.e. every option has been refuted.
      //Expanding it again would just append another copy of its children and
      //spend the rest of the budget doing so.
      if (m[n].deadend) {
        break;
      }
      if (m[n].tasks.empty()) {
          backprop(m,n,domain.score(m[n].state,m[n].plan),1);
      }
      else {
        if (m[n].sims == 0) {
          m[n].state.update_state();
          double ar = 0.0;
          bool bp = true;
          for (int j = 0; j < r; j++) {
            auto rs = simulation(m[n].plan,
                                 m[n].state, 
                                 m[n].tasks, 
                                 domain,
                                 g);
            //Test this rollout, not the running sum: a failure after a
            //successful rollout leaves a sum that is not -1, so it used to go
            //unnoticed and its -1 was folded into the node's score.
            if (!rs) {
              m[n].deadend = true;
              backprop(m,n,-1.0,1);
              bp = false;
              break;
            }
            ar += *rs;
          }
          if (bp) {
            backprop(m,n,ar,r);
          }
        }
        else {
          m[n].state.update_state();
          int n_p = expansion(m,n,domain,g);
          m[n_p].state.update_state();
          double ar = 0.0;
          bool bp = true;
          for (int j = 0; j < r; j++) {
            auto rs = simulation(m[n_p].plan,
                                 m[n_p].state, 
                                 m[n_p].tasks, 
                                 domain,
                                 g);
            if (!rs) {
              m[n_p].deadend = true;
              backprop(m,n_p,-1.0,1);
              bp = false;
              break;
            }
            ar += *rs;
          }
          if (bp) {
            backprop(m,n_p,ar,r);
          }
        }
      }
      stop = std::chrono::high_resolution_clock::now();
    }

    std::vector<int> arg_maxes = {};
    double max = -std::numeric_limits<double>::infinity();
    for (auto const &s : m[w].successors) {
      if (!m[s].deadend) {
        double mean = m[s].score/m[s].sims;
        if (mean >= max) {
          if (mean > max) {
            max = mean;
            arg_maxes.clear();
            arg_maxes.push_back(s);
          }
          else {
            arg_maxes.push_back(s); 
          }
        }
      }
    }

    if (arg_maxes.empty()) {
      int u = t[v].pred;
      //At the root there is nowhere to back up to. Reading t[u] here would
      //default-construct a node at key -1 whose task network is empty, and the
      //commit loop below would then exit and report an empty plan as a
      //success. Fail loudly instead.
      if (u == -1) {
        //Distinguish the two ways of getting here. With no successors at all,
        //the budget ran out before a single option was expanded, which says
        //nothing about whether a plan exists; with successors that are all
        //dead, the options were genuinely refuted.
        if (m[w].successors.empty()) {
          throw std::logic_error(
              "Planner exhausted its time limit before evaluating any option at the "
              "root. Raise --time_limit (-T) and try again!");
        }
        throw std::logic_error(
            "Planner found no applicable decomposition at the root, no plan exists "
            "within the given search budget!");
      }
      auto succ_it = std::find(t[u].successors.begin(), t[u].successors.end(), v);
      if (succ_it != t[u].successors.end()) {
        t[u].successors.erase(succ_it);
      }
      t.erase(v);
      v = u;
      if (!undo_stack.empty()) {
        auto const& undo = undo_stack.back();
        if (undo.parent_TID != -1) {
          auto& children = tasktree[undo.parent_TID].children;
          children.erase(std::remove_if(children.begin(),
                                        children.end(),
                                        [&undo](int c) { return in(c,undo.added_TIDs); }),
                         children.end());
        }
        for (auto i : undo.added_TIDs) {
          tasktree.erase(i);
        }
        undo_stack.pop_back();
      }
      stuck_counter--;
      if (stuck_counter <= 0) {
        throw std::logic_error("Planner is stuck, terminating process!");
      }
      continue;
    }
    stuck_counter = 10;

    int arg_max = *select_randomly(arg_maxes.begin(), arg_maxes.end(), g); 
    pNode k;
    k.state = m[arg_max].state;
    k.tasks = m[arg_max].tasks;
    k.plan = m[arg_max].plan;
    k.depth = t[v].depth + 1;
    k.treeRoots = m[arg_max].treeRoots;
    CommitUndo undo;
    undo.parent_TID = m[arg_max].prevTID;
    for (auto& i : m[arg_max].addedTIDs) {
      TaskNode tasknode;
      tasknode.task = k.tasks[i].head;
      tasknode.token = k.tasks[i].to_string();
      tasknode.outgoing = k.tasks[i].outgoing;
      tasktree[i] = tasknode;
      tasktree[m[arg_max].prevTID].children.push_back(i);
      undo.added_TIDs.push_back(i);
    }
    undo_stack.push_back(undo);
    k.pred = v;
    int y = next_node_id++;
    t[y] = k;
    t[v].successors.push_back(y);
    v = y;
  }
  std::cout << "Plan found at depth " << t[v].depth;
  std::cout << std::endl;
  std::cout << "Final State:" << std::endl;
  t[v].state.print_facts();
  std::cout << std::endl;
  return v;

}

Results 
cppMCTShop(DomainDef& domain,
           ProblemDef& problem,
           Scorer scorer,
           int time_limit = 1000,
           int r = 5,
           double c = 1.4142,
           int seed = 4021) {
    domain.set_scorer(scorer);
    pTree t;
    TaskTree tasktree;
    pNode root;
    root.state = KnowledgeBase(domain.predicates,problem.objects,domain.typetree);
    for (auto const& f : problem.initF) {
      root.state.tell(f,false,false);
    }
    root.state.update_state();
    Grounded_Task init_t;
    init_t.head = problem.head;
    int TID = root.tasks.add_node(init_t);
    TaskNode tasknode;
    tasknode.task = init_t.head;
    tasknode.token = init_t.to_string();
    tasktree[TID] = tasknode;
    root.treeRoots.push_back(TID);
    root.plan = {};
    root.depth = 0;
    int v = t.size();
    t[v] = root;
    std::mt19937_64 g(seed);
    std::cout << std::endl;
    std::cout << "Initial State:" << std::endl;
    t[v].state.print_facts();
    std::cout << std::endl;
    auto end = seek_planMCTS(t, tasktree, v, domain, time_limit, r, c, g);
    std::cout << "Plan:";
    for (auto const& p : t[end].plan) {
      std::cout << "\n\t " << p;
    }
    std::cout << std::endl;
    return Results(t,v,end,tasktree);
}
