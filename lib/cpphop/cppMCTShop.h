#pragma once

#include "../util.h"
#include "../typedefs.h"
#include <algorithm>
#include <numeric>
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

//Method-precondition semantics beyond HDDL's compiled one; see PreconditionMode
//in typedefs.h and planner_doc.md 8.16. None of this runs in Compiled mode,
//where no task carries a check.
namespace mprec {

//Before a real action runs: it is the first real action of every method in
//`checks` not yet consumed, so each of those preconditions must hold now.
inline bool first_action_ok(TaskGraph const& tasks, std::vector<int> const& checks,
                            KnowledgeBase& state, DomainDef& domain) {
  for (int k : checks) {
    auto const& chk = tasks.checks[k];
    if (!chk.consumed && !domain.actions.at(chk.action).holds(state,chk.args)) {
      return false;
    }
  }
  return true;
}

//Protected only, after a real action has produced `state`: every causal link
//still open -- established, not yet consumed, and belonging to a method with
//tasks left -- that the action does not itself belong to must still hold.
inline bool links_intact(TaskGraph const& succ, std::vector<int> const& own,
                         KnowledgeBase& state, DomainDef& domain) {
  if (succ.checks.empty()) {
    return true;
  }
  std::vector<char> live(succ.checks.size(),0);
  for (auto const& [id,tg] : succ.tags) {
    for (int k : tg.checks) {
      live[k] = 1;
    }
  }
  for (size_t k = 0; k < succ.checks.size(); k++) {
    auto const& chk = succ.checks[k];
    if (!live[k] || !chk.established || chk.consumed) {
      continue;
    }
    if (std::find(own.begin(),own.end(),(int)k) != own.end()) {
      continue;
    }
    if (!domain.actions.at(chk.action).holds(state,chk.args)) {
      return false;
    }
  }
  return true;
}

//After a task has been applied and removed: record what it changed.
inline void record(TaskGraph& succ, bool real, std::vector<int> const& own, int establishes) {
  if (real) {
    for (int k : own) {
      succ.checks[k].consumed = true;
    }
  }
  else if (establishes >= 0) {
    succ.checks[establishes].established = true;
  }
}

} // namespace mprec

//Why a rollout stopped.
//
//The distinction between Refuted and Cutoff is load-bearing rather than
//cosmetic. `simulation` is a complete depth-first search, so exhausting a
//subtree without finding a plan *proves* the node is a dead end -- and that
//proof is exactly what lets `seek_planMCTS` mark the node dead and never look
//at it again (2.4). A rollout that stopped because it ran out of depth budget
//has proved nothing at all: there may be a plan just below the cut. Collapsing
//the two into one "failed" answer would prune live subtrees, so they are kept
//apart in the type and every caller has to say which one it means.
enum class RolloutStatus {
  Solved,   //reached a complete plan; `score` holds its value
  Refuted,  //exhausted the subtree -- a proof that this is a dead end
  Cutoff    //ran out of depth budget -- no information either way
};

//Deliberately has no conversion to bool. The old signature returned
//std::optional<double> and callers wrote `if (!rs)`, which is precisely the
//conflation described above; making that spelling fail to compile is the point.
struct RolloutResult {
  RolloutStatus status = RolloutStatus::Refuted;
  double score = 0.0;
};

//One randomised depth-first dive, stopping at the first complete plan.
//
//state and tasks are taken by reference, not by value. A rollout never mutates
//either -- successor states come back fresh from ActionDef::apply, and
//MethodDef::apply takes its own copy of the network because it removes the
//decomposed task from it -- so copying them per recursion level was pure cost,
//and it was the single largest source of allocation in the planner.
//
//depth_budget bounds the recursion. Without it this search is not total: a
//domain whose decomposition can cycle -- a `get_to` that can recurse back to a
//target it already passed, say -- makes a rollout run forever rather than
//fail. Measured on a two-way road graph, the left-recursive transport encoding
//ran for 45 s without finishing where the one-way version takes 100 ms, and
//the right-recursive one exhausted the stack and exited 139. Every subtask
//counts against the budget, decompositions included, because a recursion that
//never reaches a primitive task is exactly the case being caught.
RolloutResult
simulation(std::vector<std::string>& plan,
           KnowledgeBase& state,
           TaskGraph& tasks,
           DomainDef& domain,
           std::mt19937_64& g,
           int depth_budget,
           int restrict_compound) {
  if (tasks.empty()) {
    return {RolloutStatus::Solved,domain.score(state,plan)};
  }
  if (depth_budget <= 0) {
    return {RolloutStatus::Cutoff,0.0};
  }
  //restrict_compound selects between Algorithm 1 and Algorithm 2 (§2.3.2) for
  //the rollout's candidate set. `expansion` always uses Algorithm 2: branch
  //over every unconstrained primitive task but over only ONE unconstrained
  //compound task, since the order two compound tasks are decomposed in carries
  //no commitment to the solution -- only the choice of method does.
  //
  //Completeness is unaffected either way (Holler et al.'s Theorems 1 and 2
  //cover both), which matters here beyond performance: a Refuted rollout is a
  //*proof* of a dead end, and that proof would not survive a restriction that
  //could lose solutions.
  std::vector<int> u;
  std::vector<int> compound;
  for (auto &[i,gt] : tasks.GTs) {
    if (gt.incoming.empty()) {
      if (domain.actions.contains(tasks[i].head)) {
        u.push_back(i);
      }
      else if (domain.methods.contains(tasks[i].head)) {
        if (restrict_compound != 0) {
          compound.push_back(i);
        }
        else {
          u.push_back(i);
        }
      }
      else {
        std::string message = "Invalid task ";
        message += tasks[i].head;
        message += " during simulation!";
        throw std::logic_error(message);
      }
    }
  }
  //1 = lowest task id, exactly as expansion picks it, so the rollout explores
  //the same shape of space the tree does. 2 = a uniformly random one, which
  //keeps the branching reduction but restores the per-rollout diversity a
  //randomised dive depends on.
  //3 = Algorithm 3: while any compound task is unconstrained, progress NO
  //action at all. This is the strongest of the three, and is here because it
  //is the direct test of 8.7.6's claim that interleaving is what costs --
  //Algorithm 2 only restricts which compound task is decomposed first, whereas
  //this also stops primitive actions from interleaving with pending
  //decompositions.
  if (!compound.empty()) {
    if (restrict_compound == 3) {
      u.clear();
      u.push_back(*select_randomly(compound.begin(),compound.end(),g));
    }
    else if (restrict_compound == 2) {
      u.push_back(*select_randomly(compound.begin(),compound.end(),g));
    }
    else {
      u.push_back(*std::min_element(compound.begin(),compound.end()));
    }
  }
  //Tasks remain but every one of them has an incoming edge, so the ordering
  //graph has a cycle and nothing can ever be applied. That is a genuine dead
  //end, not a budget problem.
  if (u.empty()) {
    return {RolloutStatus::Refuted,0.0};
  }
  std::shuffle(u.begin(),u.end(),g);

  //A branch that was cut short abandons the whole rollout, rather than being
  //noted while the remaining siblings are tried.
  //
  //Continuing was the obvious reading -- it keeps the rollout exhaustive
  //*within* the bound -- and it is badly wrong in practice. A rollout that
  //succeeds returns at the first plan it reaches, but one that keeps going
  //after a cutoff has to exhaust the entire depth-bounded subtree before it
  //can answer, so tightening the bound makes the planner dramatically slower
  //instead of faster. Measured: transport_chain_b plans instantly at the
  //default bound of 1000 and had not finished after 75 minutes of CPU at a
  //bound of 30. A safety limit that turns a fast failure into an unbounded
  //wait is worse than no limit at all.
  //
  //Bailing costs nothing that matters. A rollout is a randomised dive for a
  //value estimate, not a proof of anything except when it comes back Refuted
  //-- and that is preserved exactly, because Refuted is still only returned
  //when no cutoff occurred anywhere beneath this node.

  for (auto const& cTask : u) {
    if (domain.actions.contains(tasks[cTask].head)) {
      auto& adef = domain.actions.at(tasks[cTask].head);
      auto const mode = domain.precondition_mode;
      bool const real = !adef.is_artificial();
      if (mode != PreconditionMode::Compiled && real &&
          !mprec::first_action_ok(tasks,tasks.checks_of(cTask),state,domain)) {
        continue;
      }
      auto act = adef.apply(state,tasks[cTask].args);
      if (!act.second.empty()) {
        std::vector<int> const own = tasks.checks_of(cTask);
        int const establishes = tasks.establishes_of(cTask);
        auto gtasks = tasks;
        gtasks.remove_node(cTask);
        if (mode != PreconditionMode::Compiled) {
          mprec::record(gtasks,real,own,establishes);
        }
        for (auto &ns : act.second) {
          ns.update_state();
          if (mode == PreconditionMode::Protected && real &&
              !mprec::links_intact(gtasks,own,ns,domain)) {
            continue;
          }
          auto gplan = plan;
          //A synthesised method-precondition check is a search step but not a
          //plan step; keeping it out leaves plan length and the score
          //functions that read it meaning what they did before.
          if (!adef.is_artificial()) {
            gplan.push_back(act.first+"_"+std::to_string(cTask));
          }
          auto rs = simulation(gplan,ns,gtasks,domain,g,depth_budget-1,restrict_compound);
          if (rs.status == RolloutStatus::Solved) {
            return rs;
          }
          if (rs.status == RolloutStatus::Cutoff) {
            return rs;
          }
        }
      }
    }
    else {
      //Shuffle an index permutation rather than a copy of the method list:
      //copying it deep-copies every MethodDef, subtask map and ordering map.
      auto& task_methods = domain.methods[tasks[cTask].head];
      std::vector<int> order(task_methods.size());
      std::iota(order.begin(),order.end(),0);
      std::shuffle(order.begin(),order.end(),g);
      for (auto const& mi : order) {
        auto& m = task_methods[mi];
        auto all_gts = m.apply(state,tasks[cTask].args,tasks,cTask,domain.precondition_mode);
        if (!all_gts.empty()) {
          std::shuffle(all_gts.begin(),all_gts.end(),g);
          for (auto &gts : all_gts) {
            auto rs = simulation(plan,state,gts.second,domain,g,depth_budget-1,restrict_compound);
            if (rs.status == RolloutStatus::Solved) {
              return rs;
            }
            if (rs.status == RolloutStatus::Cutoff) {
              return rs;
            }
          }
        }
      }
    }
  }
  //Every branch was tried and none was cut short, so this is a proof.
  return {RolloutStatus::Refuted,0.0};
}

int expansion(pTree& t,
              int n,
              DomainDef& domain,
              std::mt19937_64& g,
              int algorithm) {
    //Höller et al.'s Algorithm 2: branch over every unconstrained primitive
    //task, but over only a single unconstrained compound one. Which compound
    //task is decomposed first carries no commitment -- only the choice of
    //method does -- so branching over that just re-reaches the same networks by
    //different routes. Sound here only because method applicability no longer
    //depends on the state: preconditions moved into synthesised actions (2.3),
    //leaving types and :constraints, which no action can change. The pick is
    //deterministic (lowest task id) so a node always generates the same
    //children. Note that simulation above deliberately does NOT do this -- see
    //the comment there, and 8.9 for why mirroring it there is a loss.
    //
    //algorithm == 3 is Holler et al.'s Algorithm 3, the systematic one: while
    //any compound task is unconstrained, progress NO action at all. It differs
    //from Algorithm 2 by one line below, and trades away the thing progression
    //search exists for -- having the current state in hand -- because it
    //postpones the state update. The authors are explicit that Algorithm 2 may
    //win in practice and that only measurement settles it (5.4).
    std::vector<int> u;
    std::vector<int> compound;
    for (auto const& [id,gt] : t[n].tasks.GTs) {
      if (gt.incoming.empty()) {
        if (domain.actions.contains(t[n].tasks[id].head)) {
          u.push_back(id);
        }
        else if (domain.methods.contains(t[n].tasks[id].head)) {
          compound.push_back(id);
        }
        else {
          std::string message = "Invalid task ";
          message += t[n].tasks[id].head;
          message += " during simulation!";
          throw std::logic_error(message);
        }
      }
    } 
    if (!compound.empty()) {
      //The one line that separates the two algorithms: Algorithm 3 drops the
      //unconstrained primitive tasks entirely while a compound one is waiting.
      if (algorithm == 3) {
        u.clear();
      }
      u.push_back(*std::min_element(compound.begin(),compound.end()));
    }
    if (u.empty()) {
      t[n].deadend = true;
      return n;
    }
    
    for (auto const& tid : u) {
      if (domain.actions.contains(t[n].tasks[tid].head)) {
        auto& adef = domain.actions.at(t[n].tasks[tid].head);
        auto const mode = domain.precondition_mode;
        bool const real = !adef.is_artificial();
        //The same three hooks as simulation: a real action must find every
        //unconsumed method precondition it starts still true, and under
        //Protected must not break a causal link it is foreign to.
        if (mode != PreconditionMode::Compiled && real &&
            !mprec::first_action_ok(t[n].tasks,t[n].tasks.checks_of(tid),t[n].state,domain)) {
          continue;
        }
        auto act = adef.apply(t[n].state,t[n].tasks[tid].args);
        if (!act.second.empty()) {
          std::vector<int> const own = t[n].tasks.checks_of(tid);
          int const establishes = t[n].tasks.establishes_of(tid);
          for (auto const& state : act.second) {
            pNode v;
            v.state = state;
            v.state.update_state();
            v.tasks = t[n].tasks;
            v.tasks.remove_node(tid);
            if (mode != PreconditionMode::Compiled) {
              mprec::record(v.tasks,real,own,establishes);
              if (mode == PreconditionMode::Protected && real &&
                  !mprec::links_intact(v.tasks,own,v.state,domain)) {
                continue;
              }
            }
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
          auto gts = m.apply(t[n].state,t[n].tasks[tid].args,t[n].tasks,tid,domain.precondition_mode);
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
              std::mt19937_64& g,
              int max_iterations,
              int max_depth,
              int max_decisions,
              int restrict_rollouts,
              int algorithm,
              long& cutoffs) {
  int stuck_counter = 10;
  //A global cap on committed decisions, separate from stuck_counter, which
  //resets to 10 on every successful commit and so bounds only a *run* of
  //consecutive backtracks -- never the loop as a whole.
  //
  //That was harmless while every unsuccessful rollout was a refutation: a
  //refuted node is marked dead, dead options run out, and the loop converges.
  //Cutoff deliberately does not mark anything dead, so a depth bound too tight
  //for the domain leaves UCT with no signal at all, every commit effectively
  //random, and commit/backtrack able to alternate forever. Measured before
  //this cap existed: transport_chain_b at --max_depth 20 spun with no output,
  //with the time going to seek_planMCTS rather than to any single rollout.
  //
  //The cap is deliberately far above any real plan -- the longest this planner
  //produces on the shipped domains is 16 actions -- so it is a backstop, not a
  //search parameter.
  int decisions = 0;
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
    //max_iterations > 0 replaces the wall-clock budget with a fixed number of
    //MCTS iterations. The time-limited search is the real one, but it is not
    //reproducible -- a busier machine fits fewer iterations into the same
    //budget and commits to a different plan -- so there is no way to tell a
    //refactor that changed behaviour from one that merely ran on a loaded
    //machine. Counting iterations instead makes a whole planner run a
    //deterministic function of the seed, which is what the benchmark harness
    //needs to regression-test the tree search. Nothing else should use it.
    int iterations = 0;
    while (max_iterations > 0 ? (iterations < max_iterations)
                              : (std::chrono::duration_cast<std::chrono::milliseconds>(stop - start).count() < time_limit)) {
      iterations++;
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
                                 g,
                                 max_depth,
                                 restrict_rollouts);
            //Test this rollout, not the running sum: a failure after a
            //successful rollout leaves a sum that is not -1, so it used to go
            //unnoticed and its -1 was folded into the node's score.
            if (rs.status == RolloutStatus::Refuted) {
              m[n].deadend = true;
              backprop(m,n,-1.0,1);
              bp = false;
              break;
            }
            //A cutoff is not a refutation, so the node must NOT be marked dead
            //-- see RolloutStatus. It gets the same low value, which makes UCT
            //deprioritise it without forbidding it, and the node stays
            //expandable. That is what keeps the bound from costing solutions:
            //`expansion` can still descend past the cut, and a rollout from
            //the deeper node starts again with a full budget.
            if (rs.status == RolloutStatus::Cutoff) {
              cutoffs++;
              backprop(m,n,-1.0,1);
              bp = false;
              break;
            }
            ar += rs.score;
          }
          if (bp) {
            backprop(m,n,ar,r);
          }
        }
        else {
          m[n].state.update_state();
          int n_p = expansion(m,n,domain,g,algorithm);
          m[n_p].state.update_state();
          double ar = 0.0;
          bool bp = true;
          for (int j = 0; j < r; j++) {
            auto rs = simulation(m[n_p].plan,
                                 m[n_p].state,
                                 m[n_p].tasks,
                                 domain,
                                 g,
                                 max_depth,
                                 restrict_rollouts);
            if (rs.status == RolloutStatus::Refuted) {
              m[n_p].deadend = true;
              backprop(m,n_p,-1.0,1);
              bp = false;
              break;
            }
            if (rs.status == RolloutStatus::Cutoff) {
              cutoffs++;
              backprop(m,n_p,-1.0,1);
              bp = false;
              break;
            }
            ar += rs.score;
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
    if (++decisions > max_decisions) {
      std::string message = "Planner committed "+std::to_string(max_decisions)+
        " decisions without completing a plan, and is very unlikely to be making"
        " progress.";
      if (cutoffs > 0) {
        message += " "+std::to_string(cutoffs)+" rollout(s) stopped at the depth"
          " bound of "+std::to_string(max_depth)+" rather than finishing, so the"
          " search was steering on little or no information -- raise --max_depth.";
      }
      throw std::logic_error(message);
    }

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

//The default depth bound for rollouts. Chosen against measurement rather than
//picked: the deepest rollout across every shipped domain and every chain
//instance of 8.7 is 51 (transport 30, sar3 19, d18 14, simple_travel 7, the
//chain instances 39-51), so this is roughly 20x the worst case seen. It exists
//to catch a decomposition that cycles, not to shape the search, and a domain
//that legitimately needs more should raise it rather than have it guessed
//larger here -- the planner reports when it binds, so a bound that is too
//tight announces itself instead of looking like an unsolvable problem.
constexpr int kDefaultMaxRolloutDepth = 1000;

//Backstop on committed decisions; see the comment in seek_planMCTS. Two orders
//of magnitude above the longest plan the shipped domains produce.
constexpr int kDefaultMaxDecisions = 1000;

Results
cppMCTShop(DomainDef& domain,
           ProblemDef& problem,
           Scorer scorer,
           int time_limit = 1000,
           int r = 5,
           double c = 1.4142,
           int seed = 4021,
           int max_iterations = 0,
           int max_depth = kDefaultMaxRolloutDepth,
           int max_decisions = kDefaultMaxDecisions,
           int restrict_rollouts = 0,
           int algorithm = 2) {
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
    long cutoffs = 0;
    auto end = seek_planMCTS(t, tasktree, v, domain, time_limit, r, c, g,
                             max_iterations, max_depth, max_decisions, restrict_rollouts,
                             algorithm, cutoffs);
    //A rollout that hit the bound is not evidence of anything, so if many did,
    //the search was steering on much less information than it appears to have.
    //Say so: a bound set too low for the domain otherwise looks exactly like a
    //problem the planner cannot solve.
    if (cutoffs > 0) {
      std::cout << "\nNote: " << cutoffs << " rollout(s) stopped at the depth "
                << "bound of " << max_depth << " rather than finishing."
                << "\n      Those rollouts carry no information. If the plans "
                << "this domain needs are"
                << "\n      genuinely deeper than that, raise --max_depth."
                << std::endl;
    }
    std::cout << "Plan:";
    for (auto const& p : t[end].plan) {
      std::cout << "\n\t " << p;
    }
    std::cout << std::endl;
    return Results(t,v,end,tasktree);
}
