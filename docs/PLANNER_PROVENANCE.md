# Algorithmic Provenance of the MCTS HTN Planner

This note complements the top-level `README.md`. It records **which algorithms
the code in `lib/` implements** and **where each piece comes from in the
planning and MCTS literature**, with pointers to the exact code. It covers the
MCTS HTN planner (`lib/cpphop/cppMCTShop.h`) and the shared data structures it
depends on (`lib/typedefs.h`, `lib/kb.h`, `lib/cpphop/loader.h`).

Line numbers refer to the `main` branch at commit `9052868`, which predates the
trim of this repository down to the HTN planner. Files and sections describing
the hybrid planner, the DFS planner, the plan recognizers, the evaluation
harness and the perceptual pipeline have been dropped from this note along with
the code.

**Treat every line number here as approximate.** The surviving files have since
been edited, `kb.h` substantially so — it lost the time-indexed fact overlay
(§2.7) and is around 270 lines shorter than the numbers below assume. The file
and function names are current; the offsets are not.

---

## 1. Summary

In one sentence: the planner does **lifted, partial-order HTN progression
search** (the SHOP2 family, generalized as in Höller et al. 2020). It is driven
by **UCT** (Kocsis & Szepesvári 2006). Its **rollouts are complete randomized
depth-first searches that backtrack** (the same design as the lifted MCTS-JSHOP
configuration of Wichlacz et al. 2020). It runs as an **online, receding-horizon
planner**: a time-limited search per decision, then a commit. States are
represented and queried through **Z3 under a closed-world completion
encoding**, and the planner maximizes a **user-defined score** of the final
state and plan rather than minimizing cost.

| Component | Code | Closest literature | Relation |
|---|---|---|---|
| Input language | `loader.h`, `parsing/` | HDDL (Höller et al. 2020a) | Implements most of HDDL; goals ignored by the HTN planner (§2.1) |
| Initial task network → single root task + method | `loader.h` 766–833, 840 | Geier & Bercher 2011 formalism (single initial task) | Standard compilation |
| Types, conditional/`forall` effects | `kb.h` 202–219; `typedefs.h` 286–435 | PDDL typing (McDermott et al. 1998); ADL (Pednault 1989) | Standard |
| State queries, variable binding | `kb.h` 145–178, 234–455, 490–516 | Z3 (de Moura & Bjørner 2008); CWA / Clark completion (Reiter 1978; Clark 1978); SMT in planning (Gregory et al. 2012) | Engineering choice. I found no HTN planner that evaluates preconditions this way |
| Search space | `cppMCTShop.h` 151–229; `typedefs.h` 608–693 | SHOP2 (Nau et al. 2003); HTN progression (Alford et al. 2012; Höller et al. 2020b, Alg. 1) | Matches Höller et al.'s Alg. 1 (branch over all unconstrained tasks). **Not** SHOP2's sub(m) restriction |
| Method preconditions | `typedefs.h` 642–693 | SHOP semantics (Nau et al. 2003) vs. HDDL compiled semantics | SHOP-style: checked at decomposition time |
| Tree policy | `cppMCTShop.h` 17–64 | UCB1 (Auer et al. 2002); UCT (Kocsis & Szepesvári 2006) | Standard UCT, c = √2, random tie-breaking |
| Expansion | `cppMCTShop.h` 151–229, 267–312 | Coulom 2006; Browne et al. 2012 | Rollout on first visit, full child generation on second |
| Rollouts | `cppMCTShop.h` 82–149 | Wichlacz et al. 2020 (DFS roll-outs with backtracking) | Same idea; theirs is totally ordered, this one partial-order |
| Dead-end labels | `cppMCTShop.h` 30–53, 172–174, 227, 278–283 | MCTS-Solver (Winands et al. 2008); node caching in Wichlacz et al. 2020 | Only the "proven loss" half of MCTS-Solver |
| Decision rule and commit loop | `cppMCTShop.h` 244–385 | "Max child" (Chaslot et al. 2008; Browne et al. 2012); online UCT (Kocsis & Szepesvári 2006); Run-Lookahead (Ghallab, Nau & Traverso 2016); UPOM (Patra et al. 2020/2021) | Receding-horizon HTN planning |
| Objective | `typedefs.h` 42, 730–732; `domains/score_functions.h` | HTN planning with preferences (Sohrabi et al. 2009) | Arbitrary scalar utility, not cost |
| Time-indexed fact overlay | *removed* | Continual planning (Brenner & Nebel 2009); planning and acting (Ghallab et al. 2016) | Custom mechanism, since deleted (§2.7). I found no direct HTN precedent |
| Output decomposition tree | `cppMCTShop.h` 369–379; `grapher.h` | Decomposition trees (Geier & Bercher 2011; Behnke et al. 2017) | Standard artifact |

---

## 2. Component-by-component provenance

### 2.1 Input language and problem compilation

* **HDDL.** Domains and problems are read in HDDL (Höller et al. 2020a), the
  hierarchical extension of PDDL used in the IPC 2020/2023 HTN tracks.
  Totally ordered networks (`:ordered-subtasks`/`:ordered-tasks`) become a chain
  of ordering edges. Partially ordered networks use `:ordering`
  (`loader.h` 683–707).
* **Method `:constraints`.** HDDL limits method constraints to (in)equality
  constraints over variables (Höller et al. 2020a). The loader conjoins them
  with the method precondition (`loader.h` 669–679), so they are checked in the
  same SMT query.
* **Initial task network.** The problem's `:htn` network is turned into a
  method for a synthetic root task named after the problem (`loader.h`
  766–833), and that method is added to the domain (`loader.h` 840). The
  planner then starts from one root task (`cppMCTShop.h` 413–415). This matches
  the common formal simplification of an HTN problem with a single initial
  abstract task c_I (Geier & Bercher 2011; Höller et al. 2020b; Bercher, Alford
  & Höller 2019).
* **Goals.** Standard HTN semantics defines solutions only by task
  decomposition (Erol, Hendler & Nau 1994), and the HTN planner follows that:
  it ignores `:goal`. HDDL makes the goal optional, but when a goal is given, a
  solution must also satisfy it (Höller et al. 2020a). The README already notes
  this deviation.
* **Typing and effects.** Types become unary predicates asserted for each object
  and all of its ancestor types (`kb.h` 202–219). This is the usual compilation
  of PDDL typing (McDermott et al. 1998). Conditional effects (`when`) and
  universally quantified effects (`forall`) come from ADL (Pednault 1989) and
  are handled in `ActionDef::apply_binding` (`typedefs.h` 286–435).

### 2.2 State representation and precondition evaluation (SMT)

* The state is a set of ground facts. `update_state` compiles it into an
  SMT-LIB script (`kb.h` 234–455). The script has one finite datatype
  `__Object__` whose constructors are the problem's objects, and one
  uninterpreted Boolean function per predicate. Each function is pinned down by
  a universally quantified biconditional, `p(x̄) ⇔ ⋁_facts (x̄ = ā)`, or by
  `¬p(x̄)` when the predicate has no true facts.
  * This is the **closed-world assumption** (Reiter 1978) written as a
    **predicate completion** (Clark 1978). Each predicate is true exactly on the
    listed tuples.
* **Satisfiability and bindings.** A ground formula is checked with one Z3 call
  (`kb.h` 503–509; Z3: de Moura & Bjørner 2008). A formula with free parameters
  is solved by declaring each parameter as an `__Object__` constant constrained
  by its type. All satisfying bindings are then enumerated with **blocking
  clauses**: after each model, the solver adds "not this model" and checks
  again (`kb.h` 145–178). This is the standard AllSAT/AllSMT enumeration loop
  (e.g., Lahiri, Nieuwenhuis & Oliveras 2006).
* **Lifted, SHOP-like.** Actions and methods are never grounded in advance. The
  arguments a task already carries become equalities
  (`(= ?param value)`, `typedefs.h` 466–469, 646–649). Any remaining method
  variables are bound by the query. SHOP/SHOP2 are lifted in the same way (they
  use unification against the state), whereas PANDA and most IPC HTN planners
  ground the whole problem first.
* **Relation to "planning modulo theories."** Using an SMT solver as the
  state-query engine resembles Planning Modulo Theories (Gregory, Long, Fox &
  Beck 2012), but here the solver is only a first-order query engine over a
  finite domain; no numeric or other background theories are used. I did not
  find an HTN planner that evaluates preconditions this way. It looks like a
  design choice specific to this codebase, which trades speed for expressive
  preconditions: full FOL with quantifiers and equality comes for free.

### 2.3 The decomposition search space: partial-order HTN progression

A search node holds a state, a partially ordered **task network** (`TaskGraph`:
ids plus `incoming`/`outgoing` edges, `typedefs.h` 63–104), and the plan prefix
(the actions applied so far). Successors are generated by `expansion`
(`cppMCTShop.h` 151–229):

1. Collect every **unconstrained** task, meaning a task with no incoming edge
   (`cppMCTShop.h` 156–171).
2. **Primitive** task: if its precondition holds, apply it, remove it from the
   network, and append it to the plan (178–197).
3. **Compound** task: for each method and each binding of the method's
   precondition, replace the task with the method's subtasks (199–217). In
   `MethodDef::apply_binding` (`typedefs.h` 608–640), subtasks that have no
   successor inside the method inherit the decomposed task's outgoing edges
   (628–632). The decomposed task never has incoming edges, because only
   unconstrained tasks are chosen, so no incoming edges need to be copied.

This is **progression search** over partially ordered HTN networks:

* **SHOP2** (Nau et al. 2003, Fig. 5) introduced forward decomposition on
  partially ordered networks, applying tasks in execution order so the current
  state is always known, and letting subtasks of different tasks interleave. The
  README cites SHOP2 for this reason.
* **Important difference from SHOP2.** After SHOP2 applies a method, it
  restricts the next choice to the unconstrained subtasks of *that method*
  (T₀ ← {t ∈ sub(m) : …}). This code does not. It always branches over *every*
  unconstrained task in the network. Höller et al. (2020b, §4) point out this
  exact difference. The code matches their **Algorithm 1**, which branches over
  all unconstrained primitive and compound tasks, and not their systematic
  Algorithm 3. The problem-space analysis of Alford, Shivashankar, Kuter & Nau
  (2012) is the other standard reference for this progression formulation and
  its termination issues. Two consequences:
  * The space is **non-systematic**. The same network can be reached by
    decomposing tasks in different orders, so the MCTS tree contains duplicate
    subtrees.
  * The README's warning about "infinite recursive/looping tasks" matches
    Alford et al.'s (2012) observation that progression need not terminate on
    recursive domains without extra checks.
* **Method preconditions follow SHOP semantics.** They are checked in the
  current state when the method is applied (`typedefs.h` 642–693). HDDL instead
  compiles a method precondition into a fresh primitive action placed before the
  method's subtasks (Höller et al. 2020a). The two semantics agree for totally
  ordered domains. With interleaving, they can differ: this code checks the
  precondition before other unconstrained tasks run, while HDDL checks it
  whenever the artificial action gets scheduled. Unlike SHOP, the code does
  **not** implement SHOP's ordered if-then-else method lists. HDDL drops those
  too, and here every applicable method becomes a sibling branch.
* **Time bookkeeping.** Each primitive action advances an integer clock by one
  (`cppMCTShop.h` 184, 189). The clock is used only to line up with perceptual
  facts (§2.6).

### 2.4 MCTS: tree policy, expansion, rollouts, backup

The inner loop (`cppMCTShop.h` 261–315) is textbook four-phase MCTS (see
Browne et al. 2012 for the survey and terminology). Each phase is described
below with its specifics.

**Selection: UCT.** Line 37 computes
`Q̄(w) + c·sqrt(ln N(v) / N(w))`. That is **UCB1** (Auer, Cesa-Bianchi & Fischer
2002) applied recursively in a tree, which is **UCT** (Kocsis & Szepesvári
2006). The default `c = 1.4142 = √2` is the constant from UCB1's analysis, which
assumes rewards in [0, 1]. The bundled score functions return values in [0, 1]
(`domains/score_functions.h`). Ties are broken uniformly at random (38–55).
Nodes labeled dead ends are skipped (36). A node whose children are all dead
ends becomes a dead end itself, and selection restarts from the root (50–53).

**Expansion: delayed, then progressive.** A new node gets **rollouts only** on
its first visit (the `sims == 0` branch, 267–288). On the next visit,
`expansion` generates **all** successors at once, shuffles them, and puts them
in an `unexplored` list (220–226). Selection then promotes one unexplored child
per visit before it uses UCB among the expanded ones (23–28, 56–61). Adding one
node per iteration follows Coulom (2006). Waiting for a second visit before
expanding is the common "expansion threshold" variant (Browne et al. 2012, §3).

**Simulation: complete, randomized DFS rollouts.** `simulation`
(`cppMCTShop.h` 82–149) is not a single random playout. It is a **randomized
depth-first search with chronological backtracking**:

* It shuffles the unconstrained tasks, methods, and method bindings, then
  recurses.
* It returns the score of the **first complete plan** it finds.
* It returns −1 only after exhausting the subtree.

The closest precedent is Wichlacz, Höller, Torralba & Hoffmann (2020), which
applied MCTS to HTN planning inside PANDA (grounded, partial-order) and JSHOP
(lifted, totally ordered). For the lifted JSHOP variant they report that "each
roll-out performs a depth-first search for a solution, guaranteeing to terminate
with either a plan, or a proof that the current MCTS leaf node is a dead-end,"
and that backtracking was added because plain random roll-outs hit dead ends.
This code uses the same design in a **lifted *and* partially ordered** setting,
a combination that paper did not study. Background on randomizing
backtracking search: Gomes, Selman & Kautz (1998).

Because the DFS rollout is complete (on finite spaces), a failed rollout
*proves* the node is a dead end. That justifies marking the node dead right
after the first failure (278–283, 302–307). Each leaf gets `r` rollouts (default
5), which are summed and backed up with weight `r` (`backprop(m, n, ar, r)`).
This is the sequential analogue of **leaf parallelization** (Cazenave &
Jouandeau 2007; Chaslot, Winands & van den Herik 2008): it lowers the variance
of the leaf estimate at extra cost per iteration.

**Backup: mean.** `backprop` (66–80) adds the reward and visit count along the
path, and selection uses the mean. For *deterministic, single-agent*
optimization, the literature argues for max-style backups:

* Single-Player MCTS (Schadd et al. 2008) tracks the top score.
* Keller & Helmert (2013) propose MaxUCT for planning.
* Wichlacz et al. (2020) found a min-cost backup slightly better than the mean
  in PANDA.

Mean backup is standard UCT, but it is a known design alternative here and may
be worth an ablation.

**Dead-end labels.** Labeling nodes as proven dead ends and pruning them from
selection is the "proven loss" half of **MCTS-Solver** (Winands, Björnsson &
Saito 2008), which propagates proven game values. Wichlacz et al. (2020) use a
similar "node caching" of fully explored nodes. The code does not propagate
proven *wins*, meaning solved subtrees whose value is exact.

### 2.5 Online decision-making: time-limited search, then commit

`seek_planMCTS` (`cppMCTShop.h` 231–393) does **not** run one MCTS to the end
and read off a plan. At each step it:

1. rebuilds a fresh search tree `m` rooted at the current node (245–258),
2. runs MCTS for `time_limit` ms (259–315),
3. commits to the root child with the highest **mean** value (317–361). Browne
   et al. (2012) call this final-move rule "max child," after Chaslot et al.
   (2008),
4. records the newly added subtasks in the output task tree, then repeats from
   the chosen child (362–384).

This is **receding-horizon / online planning**. Kocsis & Szepesvári (2006)
presented UCT in exactly this interleaved way for MDPs. For hierarchical
models, the closest analogues are the **Run-Lookahead** acting procedure of
Ghallab, Nau & Traverso (2016) and **RAE + UPOM**: UPOM is a UCT-like planner
over refinement-method choices with Monte Carlo rollouts, called by the RAE
actor at each decision (Patra, Mason, Kumar, Ghallab, Traverso & Nau 2020;
Patra et al. 2021). Two consequences of the per-decision rebuild:

* Statistics from one decision are discarded at the next. There is no tree
  reuse, which is standard in game-playing MCTS.
* A commitment can lead to a dead end. The outer loop then undoes the last
  commit and searches again, up to 10 times in a row (`stuck_counter`,
  335–358). That is a bounded form of **chronological backtracking** over
  committed decisions.

The committed sequence is the *estimated best* plan under the score. Like other
MCTS planners it carries no optimality guarantee, and it is anytime only in the
per-decision time limit.

### 2.6 Objective: user-defined score

MCTS maximizes `Scorer(state, plan)` (`typedefs.h` 42, 730–732), evaluated at
the end of a complete decomposition. Two things follow:

* The HTN planner is a **utility-maximizing** planner. Classical HTN planning
  asks only for *some* decomposition, and optimal HTN planning asks for minimum
  action cost. The closest formal relative is **HTN planning with preferences**
  (HTNPLAN-P; Sohrabi, Baier & McIlraith 2009), where solution quality depends
  on the state trajectory and on how tasks were decomposed.
* The score only sees the final state and the plan string, not the
  decomposition tree. Preferences over *which methods* were used are therefore
  only expressible indirectly.

Wichlacz et al. (2020) turn plan cost into a [0, 1] reward normalized by the
incumbent solution. Here, scaling is left to the score author.

### 2.7 Time-indexed fact overlay (removed; historical)

This section is history. The mechanism it describes is **no longer in the
code**, and is recorded here only because it shaped the design of `kb.h` and
because it is the one component of this planner for which I found no HTN
precedent.

`KnowledgeBase` used to keep a second, time-indexed fact store,
`temporal_facts`, alongside the ordinary `facts` map. A perceptual agent for
the ASIST Minecraft search-and-rescue testbed pushed field-of-view percepts
onto a Redis stream; `update_temporal_facts` folded them into
`temporal_facts` between planning decisions. When the state was compiled at
time `t`, `update_state(t)` found the most recent percept timestamp ≤ `t` (the
map was ordered with `std::greater`, so `lower_bound` acted as a floor), and if
that timestamp was within 1000 units its facts were added to the predicate
completions. The planner's per-node clock, `pNode::time`, incremented once per
committed action, was what indexed into it.

The intent was to **interleave exogenous observations with planning**, in the
spirit of continual planning (Brenner & Nebel 2009) and of integrating planning
with acting and sensing (Ghallab et al. 2016).

The whole perceptual pipeline was removed when this repository was trimmed to
the HTN planner, which left the consumer side unreachable: nothing wrote to
`temporal_facts`, so it was always empty, so `update_state` always took its
no-percepts branch. That branch was a verbatim duplicate of the one guarded by
the emptiness check, so collapsing the two and deleting the overlay left the
emitted SMT identical. `temporal_facts`, the `time` parameters on `tell` and
`update_state`, and `pNode::time` are all gone; the planner no longer keeps a
clock. The removal was verified by building both versions and diffing their
output — see §4, note on verification.

### 2.8 Output: the task (decomposition) tree

Each committed method application appends children to `tasktree`
(`cppMCTShop.h` 369–379), and `grapher.h` renders the result with Graphviz. The
structure is a **decomposition tree** in the sense of Geier & Bercher (2011).
Behnke, Höller & Biundo (2017) use it as the witness in HTN plan verification.

---

## 3. What is standard vs. what is specific to this implementation

**Standard, with direct precedents:** HDDL input; lifted forward decomposition
in execution order; partial-order progression that branches over all
unconstrained tasks; UCT with c = √2; max-child final selection;
dead-end pruning; decomposition-tree output.

**Closest single precedent:** Wichlacz et al. (2020). This codebase's MCTS work
started around 2021 (PR #46), after that paper, but whether the design came
from it is not recorded. The shared design is MCTS over HTN progression with
complete DFS rollouts. The differences:

* This code works on **lifted, partially ordered** networks. Their lifted
  system was totally ordered.
* It maximizes a **general utility** rather than minimizing cost.
* It **commits per decision** under a time budget instead of running as an
  anytime optimizer over whole plans.

**As far as I found, specific to this codebase:**

* SMT/Z3-based state queries under a completion encoding.
* The time-indexed fact overlay — since removed, but see §2.7 if the idea is
  worth reviving.

If these become part of a paper, they are the pieces to describe explicitly.

---

## 4. Implementation observations

Found while reading. Line numbers as above. These were found by reading rather
than by testing, except note 2, which has since been observed at run time.

1. **Possible infinite loops when backtracking a commit.**
   * `cppMCTShop.h` 337–342: the `for` loop never increments `it`. If `v` is
     not the *first* successor of `u`, it spins forever.
   * `cppMCTShop.h` 345–349: same problem, and `erase(it)` also invalidates
     `it`. It should be `it = erase(it)`, otherwise `++it`.
2. **Backtracking at the root — confirmed, and it silently returns an empty
   plan.** In `cppMCTShop.h` 336, `t[v].pred` is −1 at the root, so `t[-1]`
   default-constructs a node. That node has an empty task network, so the
   commit loop exits at once and the planner reports "Plan found at depth 0"
   with an empty plan and an empty final state — a *failure* printed as a
   success, with exit status 0.

   This is reachable on the shipped domains. On `sar3.hddl` / `sar3p1.hddl`
   with `-T 500` it fires on roughly 19 runs in 20; whether it fires depends on
   how many MCTS iterations fit in the wall-clock budget, so the same command
   is not reproducible run to run. Anything scripting this planner should treat
   a depth-0 result as failure rather than trusting the exit code. The
   now-removed hybrid planner guarded the root with a `u == -1` check, which is
   the fix to apply here.
3. **Only one level of commit undo is tracked.** `prev_TID` and `prev_i`
   describe only the most recent commit. After two backtracks in a row, the
   second one removes the wrong (stale) task-tree nodes.
4. **Terminal-node value decays.**
   * `backprop` overwrites `score` at nodes with no successors (68–70) but
     keeps adding to `sims`. A terminal node (empty task network) that is
     selected k times therefore has mean `score/k`, not `score`.
   * Its ancestors accumulate normally, so their means are unaffected. The
     result is that UCT systematically undervalues already-found complete
     plans. Overwriting is presumably meant for the first visit; `+=` at
     terminals would keep the mean correct.
5. **Sentinel collides with scores.**
   * Failure is signaled by −1 and success is tested with `rs > -1.0`
     (125, 140). A score function returning ≤ −1 would be read as failure.
   * `ar == -1.0` (278, 302) tests the *running sum*. That catches only a
     failed *first* rollout. This is harmless while rollouts are complete DFS,
     because a dead end fails on every rollout, but it is fragile.
   * Keeping scores in [0, 1] as UCB1 assumes, and using `std::optional`
     for failure, would remove both issues.
6. **Seeding.** `static std::mt19937_64 g(seed)` (`cppMCTShop.h` 425) is
   initialized once per process, so later calls to `cppMCTShop` with a
   different `seed` are ignored. This matters for multi-trial runs that call
   the planner more than once in a single process.
7. **Conditional `forall` effects.** In `ActionDef::apply_binding`
   (`typedefs.h` 394–397), each quantified binding is *appended* to `args`
   (a reference) without removing the previous binding. From the second
   binding on, the condition contains contradictory equalities. The effect
   therefore appears to fire for at most the first quantified object. The
   erase loop at 382–390 also indexes `tempP[i]` with a bound taken from
   `args.size()`.
8. **Semantics to document.**
   * Goals are ignored by the HTN planner (§2.1).
   * Method preconditions follow SHOP-style timing, not HDDL's (§2.3).
   * The search space is non-systematic, so duplicate subtrees occur (§2.3).

An earlier note here recorded two bugs in the time-indexed overlay's SMT
emission — a duplicate `declare-fun` when a predicate had both regular and
temporal facts, and a `lower_bound` that could dereference `end()`. Both went
away with the overlay itself (§2.7). If that mechanism is ever revived, they
are the first things to get right.

### Note on verification

Removing the overlay was checked against a build rather than by reading:

* Both versions were compiled and `ctest` run on each — `test_parser`,
  `test_kb`, `test_loader`, `test_MCTS_planner` pass before and after.
* Both binaries were run on five deterministic configurations spanning four
  domains (`transport`, `simple_travel`, `sar3`, `d18`), varying seed, time
  limit, rollout count and exploration constant. Initial state, plan and final
  state are byte-identical in every case, as is the rendered task-tree png.
* `sar3` is not usable as a regression check: it is nondeterministic on the
  *unmodified* code for the reason in note 2. Over 20 interleaved trials the
  two versions failed at the same rate (1/20 each).

The equivalence argument behind those results: `temporal_facts` had no writer,
so it was always empty, so the guard `time < 0 || temporal_facts.empty()` was
always true; and the branch it guarded was a line-for-line duplicate of the
branch in the `else`. The emitted SMT could not change.

---

## References

- Alford, R., Bercher, P., & Aha, D. W. (2015). Tight bounds for HTN planning. *ICAPS 2015*. https://dl.acm.org/doi/10.5555/3038662.3038665
- Alford, R., Shivashankar, V., Kuter, U., & Nau, D. (2012). HTN problem spaces: Structure, algorithms, termination. *SoCS 2012*. https://ojs.aaai.org/index.php/SOCS/article/view/18239
- Auer, P., Cesa-Bianchi, N., & Fischer, P. (2002). Finite-time analysis of the multiarmed bandit problem. *Machine Learning*, 47, 235–256.
- Behnke, G., Höller, D., & Biundo, S. (2017). This is a solution! (… but is it though?) – Verifying solutions of hierarchical planning problems. *ICAPS 2017*.
- Bercher, P., Alford, R., & Höller, D. (2019). A survey on hierarchical planning – One abstract idea, many concrete realizations. *IJCAI 2019*.
- Brenner, M., & Nebel, B. (2009). Continual planning and acting in dynamic multiagent environments. *JAAMAS*, 19(3), 297–331.
- Browne, C. B., et al. (2012). A survey of Monte Carlo tree search methods. *IEEE TCIAIG*, 4(1), 1–43.
- Cazenave, T., & Jouandeau, N. (2007). On the parallelization of UCT. *Computer Games Workshop 2007*.
- Chaslot, G. M. J.-B., Winands, M. H. M., & van den Herik, H. J. (2008). Parallel Monte-Carlo tree search. *CG 2008*.
- Chaslot, G. M. J.-B., Winands, M. H. M., van den Herik, H. J., Uiterwijk, J. W. H. M., & Bouzy, B. (2008). Progressive strategies for Monte-Carlo tree search. *New Mathematics and Natural Computation*, 4(3), 343–357.
- Clark, K. L. (1978). Negation as failure. In *Logic and Data Bases*, 293–322.
- Coulom, R. (2006). Efficient selectivity and backup operators in Monte-Carlo tree search. *CG 2006*.
- de Moura, L., & Bjørner, N. (2008). Z3: An efficient SMT solver. *TACAS 2008*.
- Erol, K., Hendler, J., & Nau, D. S. (1994). HTN planning: Complexity and expressivity. *AAAI 1994*.
- Geier, T., & Bercher, P. (2011). On the decidability of HTN planning with task insertion. *IJCAI 2011*.
- Ghallab, M., Nau, D., & Traverso, P. (2016). *Automated Planning and Acting*. Cambridge University Press.
- Gomes, C. P., Selman, B., & Kautz, H. (1998). Boosting combinatorial search through randomization. *AAAI 1998*.
- Gregory, P., Long, D., Fox, M., & Beck, J. C. (2012). Planning modulo theories: Extending the planning paradigm. *ICAPS 2012*. https://ojs.aaai.org/index.php/ICAPS/article/view/13505
- Höller, D., Behnke, G., Bercher, P., Biundo, S., Fiorino, H., Pellier, D., & Alford, R. (2020a). HDDL: An extension to PDDL for expressing hierarchical planning problems. *AAAI 2020*. https://staff.fnwi.uva.nl/g.behnke/papers/Hoeller2020HDDL.pdf
- Höller, D., Bercher, P., Behnke, G., & Biundo, S. (2020b). HTN planning as heuristic progression search. *JAIR*, 67, 835–880. https://jair.org/index.php/jair/article/view/11282
- Keller, T., & Helmert, M. (2013). Trial-based heuristic tree search for finite horizon MDPs. *ICAPS 2013*.
- Kocsis, L., & Szepesvári, C. (2006). Bandit based Monte-Carlo planning. *ECML 2006*.
- Lahiri, S. K., Nieuwenhuis, R., & Oliveras, A. (2006). SMT techniques for fast predicate abstraction. *CAV 2006*.
- McDermott, D., et al. (1998). PDDL – The Planning Domain Definition Language. Tech. rep. CVC TR-98-003, Yale.
- Nau, D., Au, T.-C., Ilghami, O., Kuter, U., Murdock, J. W., Wu, D., & Yaman, F. (2003). SHOP2: An HTN planning system. *JAIR*, 20, 379–404. https://arxiv.org/abs/1106.4869
- Patra, S., Mason, J., Kumar, A., Ghallab, M., Traverso, P., & Nau, D. (2020). Integrating acting, planning, and learning in hierarchical operational models. *ICAPS 2020*. https://ojs.aaai.org/index.php/ICAPS/article/view/6743
- Patra, S., Mason, J., Ghallab, M., Nau, D., & Traverso, P. (2021). Deliberative acting, planning and learning with hierarchical operational models. *Artificial Intelligence*, 299. https://arxiv.org/abs/2010.01909
- Pednault, E. P. D. (1989). ADL: Exploring the middle ground between STRIPS and the situation calculus. *KR 1989*.
- Reiter, R. (1978). On closed world data bases. In *Logic and Data Bases*, 55–76.
- Schadd, M. P. D., Winands, M. H. M., van den Herik, H. J., Chaslot, G. M. J.-B., & Uiterwijk, J. W. H. M. (2008). Single-player Monte-Carlo tree search. *CG 2008*.
- Sohrabi, S., Baier, J. A., & McIlraith, S. A. (2009). HTN planning with preferences. *IJCAI 2009*.
- Winands, M. H. M., Björnsson, Y., & Saito, J.-T. (2008). Monte-Carlo tree search solver. *CG 2008*.
- Wichlacz, J., Höller, D., Torralba, Á., & Hoffmann, J. (2020). Applying Monte-Carlo tree search in HTN planning. *SoCS 2020*. https://ojs.aaai.org/index.php/SOCS/article/view/18538 (code: https://github.com/minecraft-saar/MCTS-JSHOP)

Related but not cited above: Shao, T., Zhang, H., Cheng, K., Zhang, K., & Bie, L. (2021). The hierarchical task network planning method based on Monte Carlo tree search. *Knowledge-Based Systems*, 107067.
