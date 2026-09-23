# The MCTS HTN Planner

The working document for this planner. It complements the top-level
`README.md`, which covers building and running it.

* **§1–§5 — provenance.** Which algorithms the code in `lib/` implements and
  where each piece comes from in the planning and MCTS literature, with
  pointers to the code. §4 is the running list of defects, fixed and open.
* **§6 — efficiency.** Where the time actually goes, measured.
* **§7 — causal links and timelines.** Whether POCL or timeline-based planning
  resolves the method-precondition tension of §2.3.
* **§8 — done.** Completed work, in the order it was done, with what each step
  was measured to buy.
* **§9 — remaining.** What is left, in the order to do it, with the reasoning
  behind that order.

§1–§5 cover the MCTS HTN planner (`lib/cpphop/cppMCTShop.h`) and the shared
data structures it depends on (`lib/typedefs.h`, `lib/kb.h`,
`lib/cpphop/loader.h`).

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
| Search space | `cppMCTShop.h` (`expansion`); `typedefs.h` | SHOP2 (Nau et al. 2003); HTN progression (Alford et al. 2012; Höller et al. 2020b, Alg. 2) | Höller et al.'s Alg. 2: all unconstrained primitive tasks, one unconstrained compound task (§2.3.2). **Not** SHOP2's sub(m) restriction |
| Method preconditions | `loader.h` (method loop); `typedefs.h` | HDDL compiled semantics (Höller et al. 2020a) vs. SHOP (Nau et al. 2003) | HDDL-style: compiled into a primitive action ahead of the subtasks (§2.3) |
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
  constraints over variables (Höller et al. 2020a). They stay on the method and
  are what its binding query is solved against. Because they mention only
  variables and constants, never state, their truth cannot change between
  decomposing a method and reaching its precondition check, which is why they
  are not moved into the synthesised action along with the state precondition
  (§2.3).
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
  * A **zero-arity predicate** is the degenerate case: there is nothing to
    quantify over, so it becomes a plain `Bool` constant asserted true or
    negated, and is referenced by bare name rather than as `(p)` (§4.1 note 15).
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
3. **Compound** task: **one** unconstrained compound task is chosen (the
   lowest task id), and the branching is over its methods and their bindings
   only — see §2.3.2. In `MethodDef::apply_binding`, subtasks that have no
   successor inside the method inherit the decomposed task's outgoing edges.
   The decomposed task never has incoming edges, because only unconstrained
   tasks are chosen, so no incoming edges need to be copied.

This is **progression search** over partially ordered HTN networks:

* **SHOP2** (Nau et al. 2003, Fig. 5) introduced forward decomposition on
  partially ordered networks, applying tasks in execution order so the current
  state is always known, and letting subtasks of different tasks interleave. The
  README cites SHOP2 for this reason.
* **Important difference from SHOP2.** After SHOP2 applies a method, it
  restricts the next choice to the unconstrained subtasks of *that method*
  (T₀ ← {t ∈ sub(m) : …}). This code does not. Höller et al. (2020b, §4) point
  out this exact difference. The problem-space analysis of Alford,
  Shivashankar, Kuter & Nau (2012) is the other standard reference for this
  progression formulation and its termination issues. Two consequences:
  * The space is **not fully systematic**, even under Algorithm 2: the same
    network can still be reached along different routes, so the MCTS tree can
    contain duplicate subtrees. Algorithm 3 would close that (§5).
  * The README's warning about "infinite recursive/looping tasks" matches
    Alford et al.'s (2012) observation that progression need not terminate on
    recursive domains without extra checks.
* **Method preconditions follow HDDL semantics.** HDDL the *language* does
  allow `:precondition` on a method — its grammar has `[:precondition <gd>]` in
  the method definition — and defines what it means by compilation: a fresh
  primitive task holding the precondition is "added to the method and placed
  before all other tasks in the subtask network". Höller et al. (2020a) adopt
  that as the official reading, writing that "to make the burden of supporting
  it as small as possible, we assume the compilation semantics as given above".
  So a domain author writes method preconditions normally; the planner is what
  turns them into hidden actions. That is what the loader does at load time, so
  a precondition is evaluated when its action is scheduled rather than when the
  method is decomposed. The two semantics agree for totally ordered domains; with
  interleaving they differ, because other unconstrained tasks may run between
  the decomposition and the check. This planner previously used SHOP timing —
  checking in the current state at decomposition — and §2.3.1 records what
  changing it cost.
* **What the compilation does and does not pin down.** Höller et al. (2020a)
  are candid that this semantics is loose on partially ordered problems. In a
  totally ordered domain the synthesised action runs directly before the
  method's other subtasks, so "the position where the preconditions are checked
  is fine". In a partially ordered one it "is not necessarily placed directly
  before the other subtasks, but we just know that it is placed somewhere
  before, i.e., the condition did hold at some point before the other tasks are
  executed, but may have changed meanwhile". That applies here: the synthesised
  action precedes its own method's subtasks, but tasks from elsewhere in the
  network may still interleave between the check and the subtasks it guards.
  `transport_domain`, `p18` and `problem_gather_wake_evacuate` all use
  `:ordering`, so this is the regime they are in. The tighter alternative —
  checking exactly before the first action arising from a subtask of the method
  — is one the HDDL authors explicitly leave to "future extensions", noting a
  system has to support it natively "because a compilation is not easily
  possible". `:constraints` stay on the method: they are (in)equalities
  over variables and constants only, so their truth cannot change in between.
  Unlike SHOP, the code does **not** implement SHOP's ordered if-then-else
  method lists. HDDL drops those too, and here every applicable method becomes
  a sibling branch.
* **The synthesised checks are search steps, not plan steps.** They carry an
  `artificial` flag and are left out of the emitted plan, so plan length — and
  every score function that reads it — means what it did under SHOP timing.
  They do appear in the task tree and its rendering, which is where they
  belong: they record when the check happened.

#### 2.3.1 What HDDL timing costs

Under SHOP timing a method precondition doubles as the **generator** for the
method's free variables: `(at ?p ?l1)` both tests and computes `?l1`. Under
HDDL timing it is only a **test**, so every type-consistent binding becomes a
branch and the synthesised action rejects the bad ones later. Measured on this
machine, average time for one complete DFS rollout:

| Domain | SHOP timing | HDDL timing | |
|---|---|---|---|
| `simple_travel` | 14 ms | 17 ms | 1.2× |
| `d18` | 89 ms | 155 ms | 1.7× |
| `transport` | 166 ms | 2992 ms | 18× |

The spread is the point: the cost is not uniform, it is proportional to how
much a domain leans on its method preconditions to bind free variables.
`transport` leans on it heavily, `simple_travel` and `d18` barely. A domain
written so that free variables come from task parameters or `:constraints`
pays almost nothing. This is why the default `--time_limit` is 20000 and why
`test_MCTS_planner` plans over `simple_travel` rather than `transport`.
* **Time bookkeeping.** Each primitive action advances an integer clock by one
  (`cppMCTShop.h` 184, 189). The clock is used only to line up with perceptual
  facts (§2.6).

#### 2.3.2 Algorithm 2: one compound task, not all of them

`expansion` implements Höller et al.'s (2020b) **Algorithm 2**: it branches over
every unconstrained *primitive* task, but over only a **single** unconstrained
*compound* one, chosen deterministically as the lowest task id. Their argument
is that the order in which two compound tasks are decomposed carries no
commitment to the solution — only the choice of *method* does — so branching
over which to decompose first only re-reaches the same networks by different
routes. Picking rather than branching is the move plan-space planners make when
they select a flaw to resolve.

**This is sound here only because of §2.3.** Their formalism has preconditionless
methods, and under the SHOP timing this planner used previously, a method's
applicability was a question about the current state: a compound task with no
applicable method *now* might have one after some other unconstrained action
ran, so skipping it could have stranded a branch. With preconditions compiled
into actions, what is left on a method is types and `:constraints`, neither of
which any action can change — so a compound task's set of applicable methods is
the same whenever it is decomposed.

**What it removes.** Children generated at the first few nodes:

| Domain | | Algorithm 1 | Algorithm 2 |
|---|---|---|---|
| `transport` | root | 6 | **3** |
| `d18` / `p18` | root | 4 | **2** |
| `simple_travel` | root | 2 | 2 |
| `sar3` | root | 2 | 2 |

It halves the branching exactly where several compound tasks are unconstrained
at once, and is a no-op where only one is — the totally ordered case. The saving
compounds with depth, since each avoided duplicate is the root of a subtree.

**Rollouts deliberately keep the unrestricted set.** `simulation` is not a
search over a space to be covered; it is a randomised dive that stops at the
first solution it reaches. Applying the same restriction to it was measured and
made things *worse* where it matters most: average rollout time on `transport`
went from 2826 ms to 5617 ms, though `d18` improved from 157 ms to 115 ms.
Narrowing the choices removes the lucky first paths a random dive depends on.
Since Algorithm 2 is complete, a rollout and the tree reach the same set of
terminal networks, so a value estimated over the unrestricted set still
estimates the same quantity.

**That measurement was narrower than it read, and has since been retaken.** It
was taken on the shipped `transport_problem.hddl`, and §8.7.1 later established
that this instance does not exercise the redundancy the restriction exists to
remove: its road graph is complete, so the recursive `get_to` method never
contributes an action. §8.9 retook it on instances that do exercise it, across
three restriction strategies, and **reached the same verdict** — so the
conclusion above stands, and now rests on evidence that can actually see the
effect. The switch is still in the code (`--restrict_rollouts`), default off.

**What it does not fix.** It does not reduce the time budget `transport` needs.
That is set by rollout cost (§2.3.1), which this leaves alone by design.


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

Split into what has been fixed and what is still open. Line numbers are from
the pre-trim commit and are approximate; the file and function names are
current.

### 4.1 Fixed

1. **Two non-terminating loops in the backtrack path.** In `seek_planMCTS`,
   neither loop that removed a retracted node incremented its iterator, and the
   second also erased through the iterator it was still comparing. The first
   spun forever whenever the retracted node was not its parent's first
   successor; the second spun or ran into invalidated-iterator territory
   depending on what it found. Replaced with `std::find` + `erase` and the
   erase-remove idiom.
2. **Backtracking at the root reported failure as success.** `t[v].pred` is −1
   at the root, so `t[-1]` default-constructed a node with an empty task
   network; the commit loop then exited and the planner printed "Plan found at
   depth 0" with an empty plan and an empty final state, and returned 0. This
   was reachable on the shipped domains — `sar3.hddl` at `-T 500` hit it on
   roughly 19 runs in 20 — and it made failure indistinguishable from success
   for anything scripting the planner. The root case now throws, and
   `MCTS_planner` reports it as an error with exit status 1. The same `sar3`
   configuration succeeds reliably at `-T 2000`; the failures were genuine
   budget exhaustion that the planner was concealing.
3. **Node-id collision after repeated backtracking.** New `pTree` nodes took
   their id from `t.size()`, but backtracking erases nodes, so after two
   retractions in a row `t.size()` could name a key that was still live and the
   next commit would overwrite an existing node. Ids now come from a monotonic
   counter.
4. **Out-of-bounds parameter reads on arity mismatch.** `ActionDef::apply` and
   `MethodDef::apply` walk `args` by index while indexing `this->parameters`
   and `this->task.second` with the same counter, so a task applied with more
   arguments than the definition declares read past the end of a `std::vector`.
   Both now check arity and throw a message naming the action or method.
5. **Ordering constraints could wire up the wrong task.** In
   `MethodDef::apply_binding`, an `:ordering` naming a label that is not one of
   the method's subtasks resolved through `gts`' `operator[]` to task id 0 and
   silently added an edge to an unrelated task. That is now an error. The same
   function used `operator[]` on `this->orderings`, inserting an empty entry
   into the method for every subtask label it was asked about; it now uses
   `find`.
6. **Uncaught exceptions aborted the process.** `load` throws on a missing file
   or a bad extension, the planner throws when it is stuck, and `MethodDef` and
   `ActionDef` now throw on malformed input — but `main` wrapped only the
   option parsing, so all of these escaped and aborted. A missing domain file
   exited 134 (`SIGABRT`) with a raw `libc++abi` message. The planner call is
   now inside the handler and these exit 1 with a readable message. The
   `catch(...)` arm around option parsing also fell through instead of
   returning, so an unknown exception there carried on into planning.
7. **An unknown `--score_fun` segfaulted.** `scorers[score_fun]` on a missing
   key value-initialises a function pointer to null, which the planner then
   called: a typo exited 139 (`SIGSEGV`). The name is now checked up front and
   the valid names are listed.
8. **Dead code in `grapher.h`.** `build_graph_from_json` and
   `generate_graph_from_json` served the removed `apps/data_tools` graphers and
   had no remaining callers. `build_graph` also took an `Agnode_t*` parameter
   that it overwrote before any read, so every call site passed an
   uninitialised pointer by value — benign in practice, undefined behaviour by
   the standard, and a sanitiser complaint. It is a local now.

9. **Only one level of commit undo was tracked.** `prev_TID` and `prev_i`
   described only the most recent commit, so a second backtrack in a row re-ran
   the first one's removal and left the commit before it in the task tree.
   Replaced with a stack of undo records, one per committed decision.
10. **Terminal-node values decayed.** `backprop` overwrote `score` at nodes with
    no successors while still adding to `sims`, so a terminal node selected k
    times reported a mean of `score/k` instead of `score` and UCT
    progressively undervalued complete plans it had already found. It now
    accumulates everywhere.
11. **The failure sentinel collided with scores and was tested against a running
    sum.** `simulation` signalled failure by returning −1, which a score
    function returning ≤ −1 was indistinguishable from, and the caller tested
    `ar == -1.0` on the sum over `r` rollouts rather than on each result — so
    with `-r > 1` a dead-end rollout following a successful one left a sum like
    −0.5, went unnoticed, and folded its −1 into the node's score. Only a
    failure on the *first* rollout was ever caught. `simulation` now returns
    `std::optional<double>` and each rollout is tested individually.
12. **Re-expanding a dead end duplicated its children.** `selection` hands back
    a node as soon as it sees `deadend` — which only happens for the root of
    the current decision's tree, i.e. when every option has been refuted — and
    `expansion` did not check the flag, so it appended another full copy of the
    root's children and the rest of the time budget went into doing that
    repeatedly. The search now stops when selection reports the root exhausted.
13. **Universally quantified conditional effects never fired.** In
    `ActionDef::apply_binding`, the query evaluating a `forall` + `when`
    condition declared only the action's own parameters, not the quantified
    variables the condition mentions, so Z3 rejected it with "unknown constant"
    for *every* binding — this path had never worked. The same block also
    appended each binding onto a shared argument list without removing the
    previous one, so even once declared, the condition would have been
    self-contradictory from the second object on, and it erased from `args` and
    `tempP` while indexing them by a counter bounded by `args.size()`. The
    block is rewritten to build a fresh argument list per binding and to
    declare the quantified variables. `domains/forall_test.hddl` and a case in
    `test_MCTS_planner` now cover both quantified forms; no other domain in
    `domains/` uses `forall`.
14. **Seeding was process-wide.** `static std::mt19937_64 g(seed)` was
    initialised on first call, so later calls to `cppMCTShop` in the same
    process ignored their `seed`. No effect on a single run; it matters for
    harnesses that call the planner more than once.

15. **Zero-arity predicates made every query fail.** A propositional atom such
    as `(armed)`, which HDDL allows, broke the knowledge base outright:
    `update_state` emitted `(assert (forall () ...))` for it and the loader's
    `sentence_to_SMT` emitted `(armed)` as a nullary application, and Z3
    rejects both — "invalid quantifier, list of sorted variables is empty" and
    "invalid function application, arguments missing". SMT-LIB has no nullary
    application, so an atom is now declared as a plain `Bool` constant and
    referenced by bare name. Only those two sites emit SMT; the `(armed)`
    spelling is still what the knowledge base stores internally as a fact, and
    `parse_predicate` already handled it.
16. **An empty `(and)` precondition was rejected.** `:precondition (and)`, the
    conventional PDDL spelling of "no precondition", was parsed as a literal
    whose predicate is `and` and reached Z3 as a nullary `(and)`.
    `connected_sentence` already accepted zero operands and `sentence_to_SMT`
    already mapped an all-empty conjunction to `__NONE__` — the fault was
    purely alternation order in `sentence_def`, where `literal_terms` was tried
    first. A non-empty `(and ...)` was unaffected, since its nested parens do
    not parse as terms and it backtracked into `connected_sentence` anyway.

`domains/atom_test.hddl` covers both: two atoms used in a precondition, a
delete effect, an add effect and the initial state, plus an action carrying an
empty `(and)` precondition. `test_MCTS_planner` asserts the resulting state.

19. **An empty subtask network segfaulted the loader.** HDDL allows a method to
    decompose into nothing — `<subtask-defs> ::= () | ...` — and a method that
    ends a recursion is exactly what wants it. `loadDomain`'s ordered branch
    opens by indexing `sts[0]`, so `:ordered-subtasks ()` read past the end of
    an empty vector; the planner exited 139 (`SIGSEGV`) on such a domain. The
    unordered spelling `:subtasks ()` happened to be safe, since every loop in
    that branch is over `sts`. Found while building the task-insertion encoding
    of §8.7, whose `free_drive` recursion needs a base case with no subtasks.
    `domains/empty_method_test.hddl` is a loader-only fixture carrying both
    spellings, and `test_loader` was confirmed to fail against the unfixed
    loader.

### 4.2 Open

17. **The tests only run from `<source>/build`.** `test_loader`, `test_parser`
    and `test_MCTS_planner` reach their domains through the relative path
    `../../domains/...`, which assumes the build directory is a child of the
    source tree. Configuring a build anywhere else yields binaries that compile
    and link but fail at run time with "No file transport_domain.hddl found".
    Passing the domain directory in at configure time would fix it.
18. **Semantics to document.**
    * Goals are ignored by the HTN planner (§2.1).
    * The search space is non-systematic, so duplicate subtrees occur (§2.3).

Minor, not worth changing on their own but worth knowing: `expansion` shadows
the RNG `g` with a loop variable of the same name, and `MethodDef::apply`
shadows its `int i` parameter with a loop counter. Both currently resolve to
the intended object, and both would break silently if the surrounding code
moved.

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

Notes 1–8 were held to the same standard. They touch only paths that previously
crashed, looped or corrupted state, so successful search should be untouched,
and it is: the same five configurations still produce byte-identical output
against a pre-fix binary, and `ctest` stays at 4/4. All five shipped
domain/problem pairs — including `d18`/`p18`, which none of the tests cover —
still plan, which is what rules out the new arity and ordering checks firing on
valid input. The one intended behaviour change is that failures now announce
themselves: `sar3` at `-T 500` exits 1 with an error instead of printing an
empty plan, and succeeds at `-T 2000`.

Notes 10, 11 and 12 change the search itself, so byte-identical output is not
the bar for them and plans from before that commit are not reproducible after
it. What was checked instead is that plan *quality* holds up: across
`transport`, `simple_travel`, `sar3`, `d18`/`problem_gather_wake_evacuate` and
`d18`/`p18`, every configuration returns a plan of the same length as it did
before, and each still satisfies its score function. The plans themselves do
differ — on `transport` the new one delivers the two packages in the opposite
order, with the same eight actions and the same three drives, so the same
`delivery_one` score. That is the expected shape of the change: corrected UCT
values break ties differently among equally good plans.

Notes 15 and 16 carry the same risk as note 13 — neither zero-arity predicates
nor an empty `(and)` appears in any shipped domain — and note 16 changes the
grammar, which every parse goes through. `domains/atom_test.hddl` covers the
new behaviour, and the old behaviour was checked to be genuinely broken: the
preceding commit's binary exits 1 on that domain with Z3's "invalid quantifier"
error. For the parses that already worked, all five shipped domain/problem
pairs still produce byte-identical output against that same binary, and
`test_parser`'s AST assertions still hold.

Note 13 could not be checked against the shipped domains at all, since none of
them use `forall`. `domains/forall_test.hddl` exists for that: three rooms, two
dirty, an unconditional quantified effect that must alert all three and a
conditional one that must clean exactly the two. Before the fix the planner
exited with a Z3 "unknown constant" error; after it, both effects produce
exactly the expected facts, and `test_MCTS_planner` asserts it — including that
the clean room is *not* cleaned, which is what separates a working conditional
effect from one that fires unconditionally.

---

## 5. Making the search systematic

The non-systematicity noted in §2.3 and §4.2 is not inherent to progression
search; Höller et al. (2020b) give an algorithm that removes it. This section
records what that algorithm is, what it would take here, and what it would
cost. **Nothing in it has been implemented.**

### 5.1 The three algorithms

The paper gives three progression algorithms that differ only in what they
branch over when more than one task is unconstrained. Writing `UC` for the
unconstrained *compound* tasks and `UA` for the unconstrained *primitive* ones:

* **Algorithm 1** branches over every task in `UA` (apply it) *and* every task
  in `UC` × every applicable method (decompose it). This is what `expansion`
  used to do.
* **Algorithm 2** branches over every task in `UA`, but *picks a single*
  compound task from `UC` and branches only over its methods. **This is what
  `expansion` does now** (§2.3.2).
* **Algorithm 3** goes further: while `UC` is non-empty it progresses **no**
  action at all — it picks one compound task and branches over its methods.
  Only once `UC` is empty does it branch over applying the tasks in `UA`.

Algorithm 3 is the systematic one (their Theorem 3). The reasoning behind both
restrictions is the same: *the order in which two compound tasks are decomposed
implies no commitment to the solution — only the choice of method does* — so
there is nothing to branch over. Picking rather than branching is the same move
plan-space planners make when they select a flaw to resolve.

### 5.2 It depends on the switch to HDDL precondition timing

Their formalism defines a method as a bare pair `(c, tn)` of a compound task
name and a subtask network: **methods have no preconditions**, because HDDL
compiles them into actions (§2.3). Algorithm 3 relies on that. Under SHOP
timing, where a method's applicability is a question about the current state,
"never progress an action while an unconstrained compound task exists" can
strand a branch: no method of the chosen task is applicable *now*, but one would
be once some other unconstrained action has run.

So the move to HDDL timing is a prerequisite for adopting Algorithm 3 here, not
an unrelated change. Before it, this restriction would have been unsound.

### 5.3 What it would take in this code

Confined to `expansion`, which already computes the unconstrained set `u`:

1. Split `u` into compound and primitive instead of iterating it as one list.
2. If any compound task is unconstrained, pick exactly one — deterministically,
   e.g. lowest task id, so a node always makes the same choice — and generate
   children only for its methods and their bindings.
3. Otherwise generate children for the applicable primitive tasks, as now.

`simulation` should mirror it, or rollouts will explore a differently shaped
space than the tree does.

### 5.4 What it would and would not buy

**Sound and complete either way.** Their Theorems 1 and 2 cover all three
algorithms and do not rest on the assumptions below, so adopting Algorithm 3
cannot lose solutions.

**Full systematicity needs two assumptions this repository's domains break.**
Theorem 3 additionally assumes no two methods for a task have isomorphic
subtask networks (Assumption 1), and that no method has two subtasks with the
same task name (Assumption 2). Checking Assumption 2 over the shipped domains:

| Domain | Methods | Violations |
|---|---|---|
| `simple_travel` | 2 | none |
| `transport_domain` | 6 | `m_deliver_ordering_0` has two `get_to` subtasks |
| `sar3` | 12 | 2 methods, with 2 and 3 `location` subtasks |
| `d18` | 13 | 4 methods, up to 3 `location` subtasks |

So on three of the four, some duplicate solutions would survive. The authors
note this case is a symmetry in the *model* rather than the search revisiting a
node, and that it can be compiled away by making the repeated subtasks
distinguishable. The redundancy Algorithm 3 does remove — reaching the same
network by decomposing tasks in different orders — is present regardless.

**There is a real cost, and the authors flag it.** The argument for progression
search over plan-space search is having the current state available to compute
heuristics; Algorithm 3 postpones progression, so it postpones the state
update too. They are explicit that Algorithm 2 may therefore beat Algorithm 3
on some domains and that "only an empirical evaluation can show which algorithm
should be used in practice". That caution applies with more force here than in
their setting: this planner's node values come from rollouts over the current
state, and its score functions read that state, so delaying it delays the signal
MCTS is steering by.

**Status.** Algorithm 2 is implemented and measured (§2.3.2). Algorithm 3 is
not; it is §9.1. Going further would mean accepting the postponed state update,
which is the real question for this planner rather than a detail: node values
come from rollouts over the current state, and its score functions read that
state. Worth measuring rather than assuming the systematic one wins — and
against the chain instances of §8.7 rather than only the shipped `transport`
and `d18`, since §8.7.1 found the shipped transport instance cannot exercise
the redundancy at issue.

### 5.5 On method preconditions and interleaving: what the literature says

Separately from systematicity, §2.3 notes that HDDL's compiled method
preconditions are loose on partially ordered problems. A look for later work
resolving that found no resolution, but it did find the question sharpened.

* **Höller et al. (2020a)**, the HDDL paper, adopt the compilation and defer the
  tighter reading — checking exactly before the first action arising from a
  subtask — to "future extensions", noting a system must support it natively
  "because a compilation is not easily possible".
* **Höller & Bercher (2022)** restate the problem in the same terms two years
  later: in partially ordered planning "the decomposed task might be partially
  ordered with respect to other tasks and the subtasks might be interleaved",
  so one "cannot exactly determine the position the precondition is
  checked/needs to hold", and the definition "was chosen for HDDL due to
  practical reasons, since it is the most simple way for the planning systems".
  Their contribution is to make the resulting obligation precise from the
  verification side: because the synthesised actions are not part of a returned
  plan, a verifier "need[s] to check whether there exists a position where the
  precondition holds (in a certain range of the plan)". That existential
  reading — *holds somewhere in a window* — is the operative semantics, not
  *holds immediately before*. They also report that no other system they
  compared against supports the IPC semantics of method preconditions at all.
* **HDDL 2.1 (2023)** points at a direction rather than settling it. It is a
  position paper on temporal HDDL, and suggests that the classical
  decomposition constraints of Erol, Hendler & Nau (1994) — `before`, `after`,
  `between` — be expressed via PDDL 3.0 trajectory constraints, adding that
  "method precondition semantics (at start, at end, overall)" can be expressed
  the same way.

The shape of the resolution, then, is not to pick a better fixed convention but
to let the modeller say which one they mean: an `overall` method precondition
would have to hold throughout, `at start` at the point of decomposition. Until
something like that is standardised, a method precondition in a partially
ordered domain means only that it held at *some* point before the method's
subtasks ran. Where a domain needs more than that, the honest encoding is an
action precondition on the subtask that actually depends on it.

---

---

## 6. Efficiency: where the time goes

A measured answer to two questions: where does the time actually go, and was Z3
the right choice for the knowledge base. Everything below is from profiling and
instrumenting the current `main`, not from reading the code and guessing.

**The short version.** The planner spends **96–97% of its runtime inside Z3**,
and **about 2% of that Z3 time is spent solving anything**. The rest is
building a fresh solver context, interning symbols, and re-parsing the entire
state as SMT-LIB text — once per query. The queries themselves are trivial:
ground-fact lookups and small conjunctions over a dozen objects. Z3 was the
wrong tool for this, and the cost is not subtle.

Memory, by contrast, is fine — peak RSS is 35 MB on the heaviest shipped
domain. Do not spend effort there yet.

---

### 6.1 Evidence

#### 6.1.1 Profile (macOS `sample`, transport, self-time)

| | share of self-time |
|---|---|
| `libz3` | **64.8%** |
| `libsystem_malloc` | 19.0% |
| `libsystem_platform` (memcpy/memset/strcmp) | 9.2% |
| everything else | ~7% |

The planner's own code does not appear above the sampling threshold.

Breaking the Z3 leaves down by what they are doing:

| activity | share |
|---|---|
| allocation | 23.2% |
| `ast_manager` construction (a fresh context per query) | 14.9% |
| symbol interning / string hashing | 13.0% |
| rewriting / simplification | 7.9% |
| SMT-LIB **parsing** | 6.8% |
| **actual solving** (`smt::context`, `check_sat`) | **2.0%** |
| other | 32.2% |

#### 6.1.2 Instrumented counts

Rollouts only, from the initial state:

| | transport (3 rollouts, 8028 ms) | d18 (5 rollouts, 760 ms) |
|---|---|---|
| time in Z3 | **7688 ms (95.8%)** | **737 ms (97.0%)** |
| Z3 contexts created | 3590 | 186 |
| `ask(expr, params)` | 608 calls, 3.70 ms each | 139 calls, 4.10 ms each |
| `ask_any` (method-precondition checks) | 2979 calls, 1.82 ms each | 47 calls |
| `update_state` (builds the SMT text) | 379 calls, **8 ms total** | 102 calls, 3 ms |
| SMT text re-parsed | 1.1 MB | 0.5 MB |

Note `update_state` is *not* the problem — building the string is cheap. The
problem is that Z3 re-parses it from scratch on every query.

#### 6.1.3 The same query, two engines

"Enumerate bindings of `?l1` such that `(at package_0 ?l1)`", against the
transport initial state — 42 ground facts, the `at` relation holding 3 tuples:

| engine | per query |
|---|---|
| Z3, as the planner does it today | **3.27 ms** |
| a direct scan of the indexed relation | **0.000012 ms** |

That ratio is ~260,000×, but it is the ratio for *one step in isolation*. The
honest end-to-end bound is Amdahl's: Z3 is 96% of runtime, so removing it
entirely caps the whole-planner speedup at roughly **25×**.

#### 6.1.4 Two things that measurement ruled out

* **Memory is not a problem.** Peak RSS 35 MB for transport at `-T 30000`.
  `pNode` is 336 bytes shallow, `KnowledgeBase` 128; the whole transport state
  is 42 ground facts, about 1.3 KB of strings. There is real redundancy (§6.3)
  but it is not currently costing anything worth chasing.
* **The build is unoptimized and it does not matter — yet.** A plain
  `cmake ..`, exactly as the README instructs, compiles with
  `-std=gnu++20 -arch arm64` and **no `-O` flag**, because the project never
  sets `CMAKE_BUILD_TYPE`. Measured cost today: *none* — d18 rollouts take
  150 ms either way, because the work is inside a prebuilt `libz3`. Fix it, but
  expect the payoff only after step 3 moves work back into our own code.

---

### 6.2 Was Z3 the right call?

For the job as written, no — but the instinct behind it was sound.

Z3 buys genuine things: a declarative encoding of the closed-world assumption,
correct handling of quantifiers and equality, and all-models enumeration for
free. Writing that by hand is where bugs live. The problem is not that Z3 is
slow; it is that the code **uses it in the most expensive way available**:

1. Every query constructs a new `z3::context` — a complete AST manager.
2. The whole state is serialized to SMT-LIB text and **re-parsed per query**.
3. Each predicate is asserted as a universally quantified biconditional, so
   every query drags in quantifier machinery (`smt::mk_mam` shows in the
   profile) to answer what is really a table lookup.
4. Bindings are enumerated with blocking clauses — a solver call per model —
   even where the parameters are already pinned to values.

The queries being asked do not need a theorem prover. They are conjunctive
queries with negation and equality over a finite, fully known set of ground
facts: a relational/Datalog workload. That is also where the lifted-planning
literature has landed — Corrêa, Pommerening, Helmert & Francès,
*Lifted Successor Generation using Query Optimization Techniques*
([ICAPS 2020](https://icaps20.icaps-conference.org/paper88.html)) treats
precondition matching in a lifted planner explicitly as conjunctive query
evaluation and uses join-ordering techniques from databases.

**One structural note.** `loader.h` parses HDDL into a `Sentence` AST and then
flattens it to an SMT-LIB *string* (`sentence_to_SMT`); `Preconds` is a
`std::string`. The AST is thrown away. Any direct evaluator needs it kept, so
that is the hinge the whole plan turns on.

---

### 6.3 Where the wins are

Sequenced in §8 and §9. Each is independently verifiable, and the risky one comes
after its safety net.

**Benchmark harness.**

A script that runs every shipped domain/problem pair at a fixed seed and budget
and reports wall time, mean rollout time, plan length and final-state facts.
Nothing after this is verifiable without it. Every later step is judged on: no
change in plans, measurable change in time.

*The numbers in §6.1 came from throwaway probes in a scratch directory; they
should be a committed script.*

**Delete dead weight.**

`boost::json` is entirely unused: six `tag_invoke` overloads in `typedefs.h`
serialize `Grounded_Task`, `TaskGraph`, `TaskNode` and `TaskTree`, and nothing
calls `value_from` or `value_to` anywhere. They served the plan-recognizer JSON
output, removed earlier. Deleting them drops a Boost component from three
`CMakeLists.txt` files.

Also set `CMAKE_BUILD_TYPE` to `Release` when the caller does not specify
(§6.1.4) — no measurable gain today, but it stops being free to ignore after
step 3.

**Replace Z3 on the hot path — the whole game.**

Expected: most of the 96%. Do it in three moves, each shippable.

**3a. Keep the AST.** Have the loader retain the parsed `Sentence` alongside
the SMT string rather than discarding it. Pure addition, no behaviour change,
and it unblocks everything else.

**3b. Direct evaluator with Z3 as fallback.** Index ground facts as relations —
one table per predicate, keyed for the access patterns actually used. Write an
evaluator over the retained AST covering the fragment the shipped domains
need: conjunction, negation, equality/inequality, and binding enumeration for
free variables. Anything outside that fragment (nested quantifiers, `imply`)
falls through to the existing Z3 path.

The de-risking move: during a transition period, run **both** engines and
assert they agree, behind a build flag. That converts "did I re-implement the
closed-world semantics correctly" from a hope into a test, over whatever
domains are exercised. Given that the SMT encoding *is* the current definition
of correctness, differential testing is the only cheap way to be sure.

**3c. Retire the SMT path** once the evaluator has covered the shipped domains
and the RTL domains under differential testing, and `smt_state` with it.

**A cheaper alternative if step 3b looks too big:** keep Z3 but stop rebuilding
the world per query — one persistent context, expressions built through the C++
API instead of text, `push`/`pop` for the per-query part. That alone removes
the `ast_manager` construction, the parsing, and most of the interning, so
perhaps 2–4×. It is a fallback, not the destination: it leaves the quantified
encoding and the per-model solver calls in place.

**Intern symbols.**

Ground facts are stored as strings like `"(at package_0 city_loc_1)"` and
**re-parsed constantly** — `parse_predicate` splits them on every `tell` and on
every fact in every `update_state`. Intern predicate and object names to
integers once at load, and represent a fact as a small integer tuple. This
removes the string churn (the 9.2% in `memcpy`/`strcmp`), shrinks the state,
and makes relation indexing natural. Largely pointless before step 3, since Z3
wants text anyway.

**Stop copying what does not change.**

Every `KnowledgeBase` — and there is one per search node — carries its own copy
of the predicate schema and the object map, which are fixed for the whole
problem, plus a `smt_state` string of 3–5 KB. Share the immutable parts through
a pointer to one domain-level context; `smt_state` disappears with step 3c.

**Kill the deep copies in the hot path.**

Now worth doing, and not before — at present they are noise next to Z3.

* `simulation` takes `KnowledgeBase state` and `TaskGraph tasks` **by value**
  and recurses, so each level deep-copies both.
* `MethodDef::apply` and `MethodDef::apply_binding` take `TaskGraph tasks` by
  value.
* `simulation` does `auto task_methods = domain.methods[...]` to shuffle a
  copy of the method vector — shuffle an index permutation instead.
* `get_facts()`, `get_parameters()`, `get_effects()`, `get_subtasks()`,
  `get_orderings()` all return containers by value; several are called per
  node. Return `const&`.
* Longer term: a trail-based apply/undo state for rollouts instead of
  copy-per-node.

**Revisit the domains.**

Independent of all the above and possibly the largest single lever, because it
attacks the size of the search space rather than the cost per node.
`transport_domain.hddl` contains the recursive `goto` pattern that Godet et al.
show causes an exponential blow-up of redundant decompositions — see §8.7.

*Done, and it was a lever: 15× on the largest instance anything solves, from a
re-encoding that costs no planner code. But it did not answer the question it
was expected to. The shipped transport instance turns out not to exercise the
pattern at all (§8.7.1), and on instances that do, the dominant redundancy is
the interleaving of unordered top-level tasks rather than the `goto` pattern
itself (§8.7.6). That was expected to be planner work; §8.9 tried it and it
was not.*

---

### 6.4 What to expect

| work | expected | confidence |
|---|---|---|
| dead weight | 0× (hygiene) | certain |
| replace Z3 | up to ~25× | high; bounded by measurement, not estimate |
| intern symbols | small alone; enables the rest | medium |
| share immutable state | memory, little time | medium |
| deep copies | only visible after Z3 is gone | medium |
| domain re-encoding | potentially exponential | unknown until tried |

*Outcome of the last row: 15× on the largest instance any encoding solves, and
flat where the original grows — but from the mutex rewrite applied to a base
the paper does not apply it to, while the encoding the paper recommends was
3 000× worse than the one it replaces. §8.7.*

The single number worth keeping in mind: **2% of the time Z3 is given is spent
solving**. Everything else it does here is setup for a question that did not
need it.
### 6.5 The harness

*Written after §6.1–§6.4, and after the audit it grew out of.*

`apps/bench/planner_bench` measures one domain; `scripts/benchmark` drives it
over all of them and diffs against a saved baseline. It reports two kinds of
number and they are read differently: semantic fields fail a comparison,
timing fields only get printed.

It covers two code paths, and it needs both.

* **Rollouts** (`--rollouts N`) time N rollouts from the initial state. This is
  the performance signal — rollouts dominate runtime, and a few of them take
  seconds where a full plan takes minutes. It reports their scores, which are
  RNG-driven rather than clock-driven and so are reproducible.
* **A fixed-iteration plan** (`--iterations N`) runs the planner end to end but
  bounds each decision by a count of MCTS iterations rather than a wall-clock
  budget. This is what makes the **tree search** testable.

That second mode exists because of a hole found while testing the harness
itself. A deliberate change to `expansion` — picking the highest-id
unconstrained compound task instead of the lowest — was made, and the
rollout-only harness **passed it clean**. Rollouts go through `simulation`,
which deliberately does not share expansion's Algorithm 2 restriction
(§2.3.2), so they never touch `expansion`, `selection`, `backprop` or the
commit loop at all. The normal time-limited planner does touch them, but it is
not reproducible: a busier machine fits fewer iterations into the same budget
and commits to a different plan, so a changed plan cannot be distinguished from
a loaded machine. Fixing the iteration count makes a whole run a deterministic
function of the seed. With it in place the same perturbation is caught, on
`d18_gather` and `d18_p18`, with the plan diff printed.

Two caveats worth keeping in mind:

* **`transport` is excluded from the deterministic plan check.** Even *two*
  fixed iterations take about nine minutes there, for the reasons in §6.1. It
  still contributes rollout timing and scores.
* **`rng_after` is a tripwire, not a verdict.** It hashes the RNG state after
  the rollouts, so it moves if the search makes a different number of draws —
  more sensitive than the scores, and sensitive enough that a genuinely
  behaviour-preserving change can shift it. The harness reports it as a warning
  and fails only on the semantic fields.


---

### 6.6 After: the direct evaluator

§8.4 replaced Z3 on the query path with a join over indexed relations
(`lib/evaluator.h`). Median rollout time, same seeds and domains as §6.1:

| Domain | Z3 | direct | |
|---|---|---|---|
| `transport` | 2188.88 ms | **32.23 ms** | 68x |
| `sar3` | 121.34 ms | **0.98 ms** | 124x |
| `d18_gather` | 127.71 ms | **1.07 ms** | 119x |
| `d18_p18` | 71.71 ms | **0.73 ms** | 98x |
| `forall_test` | 19.85 ms | **0.06 ms** | 330x |
| `atom_test` | 5.63 ms | **0.03 ms** | 188x |
| `simple_travel` | 19.16 ms | **4.73 ms** | 4x |

The §6.1.3 estimate was that removing Z3 caps the whole-planner speedup at
about 25x, by Amdahl's law on its 96% share. Several domains beat that. The
bound was not wrong; it was computed against a workload whose *shape* the change
also altered. Answering a query in microseconds rather than milliseconds means a
rollout explores its dead ends far more cheaply, so the planner does not merely
do the same work faster — it reaches useful depth inside a budget that used to
expire first.

The practical consequence: `transport` solves at the default `-T 1000` in about
30 seconds. It needed `-T 20000` and about 17 minutes before.

`simple_travel` is the outlier at 4x, for a reason worth keeping in view: its
rollouts are dominated by recursion through a `get_to`-style task structure
rather than by query cost, so making queries nearly free does not help much.
That is the §8.7 problem, not this one.

---

## 7. Causal links and timelines

§2.3 leaves a loose end: under HDDL's compiled semantics a method precondition
in a partially ordered domain only has to have held at *some* point before the
method's subtasks run, and §5.5 found no resolution in the HTN literature. The
question is whether partial-order causal-link (POCL) planning or timeline-based
planning supplies the missing mechanism.

**They do — and not by coincidence. That is precisely the problem those
formalisms were built to solve.** The catch is paradigmatic, not technical.

### 7.1 What a causal link actually is

The tension exists because an ordering constraint is the only tool the HTN
formalism has here, and *"a happens before b"* cannot express *"p is still true
when b runs"*. POCL planning has a second tool. A causal link `α → β` records
not just that `α` produces the fluent `β` needs, but that the fluent must
**survive the interval between them**. Anything that would falsify it in that
window is a *threat*, and must be ordered out of the way.

FAPE (Bit-Monnot, Ghallab, Ingrand & Smith 2020) states the mechanism directly:

> "A causal link is created by inserting an additional persistence assertion
> `[end(β), start(α)] sv = v`, which **prevents any change on the value of the
> state variable `sv` from the end of β until the start of α**."

That is the exact guarantee HDDL's compilation cannot give. The interleaving
that breaks a compiled method precondition — some unrelated task running between
the check and the subtasks it guards — is, in POCL terms, simply a threat to a
causal link, and threat resolution is a solved problem: demote it, promote it,
or separate the variables.

### 7.2 Timelines generalise it to intervals

Timeline- and chronicle-based planning goes further. Conditions are asserted
over explicit temporal intervals against a constraint network, so "holds
throughout" is expressible directly rather than reconstructed from orderings.
FAPE treats a conflict between two assertions on the same state variable as a
flaw of the same family as a POCL threat — it describes conflicting assertions
as generalising "the notions of open-goals and threats in plan-space planning"
— and resolves them with separation constraints over temporal *and* object
variables.

This matters for the specific question because it is what HDDL 2.1's
`at start` / `at end` / `overall` qualifiers (§5.5) would need underneath them.
`overall` is a persistence assertion. A formalism with timelines gets that
qualifier essentially for free; one with only ordering constraints has to
invent it.

### 7.3 So does FAPE "solve" the tension?

It dissolves it rather than solving it. FAPE has no HDDL-style method
preconditions to be ambiguous about: a task refinement carries assertions with
explicit temporal qualification, and the constraint network enforces them. You
say when the condition must hold, and it holds then.

The honest reading is that **the tension in §2.3 is an artifact of a formalism
that cannot express interval conditions**, not a deep open problem. The
literature did not resolve it for HTN because the formalisms that needed it had
already moved to representations where it does not arise. That also explains the
shape of HDDL 2.1's proposal: it is importing interval qualifiers into HDDL.

A second, unrelated FAPE mechanism is worth noting because it comes up in §8.7:
FAPE distinguishes **task-dependent** actions, which may only be introduced by
decomposition, from **task-independent** ones, which may also be inserted
freely. That is a lever on redundant decompositions, not on precondition
timing, and it is the idea Godet et al. adapt.

### 7.4 What this planner could actually take

Adopting POCL or timelines wholesale would mean replacing the planner. This is
a **progression** planner: it searches forward through states, and its node
values come from rollouts over the current state. Causal links and timelines
are plan-space and constraint-based concepts; a planner built on them refines a
partial plan rather than advancing a state. That is a different system, not a
patch — and it would give up the thing progression search buys, which is having
the state in hand for the rollouts MCTS is steering by.

There is, however, a cheap adaptation that borrows the *idea* without the
paradigm. Call it a protected-condition set:

* when a synthesised method-precondition action fires, register the literals it
  checked as **protected**;
* keep them protected until every subtask of that method has been applied —
  `pNode::addedTIDs` already records exactly that set;
* make any action inapplicable if its delete effects would falsify a currently
  protected literal.

That is POCL threat-checking transposed into progression search, and it yields
the `overall` reading of a method precondition at roughly the cost of a set
intersection per action application.

Two caveats, both important:

1. **It is strictly stronger than HDDL.** It rejects plans that HDDL's compiled
   semantics accepts. That is the point — it is the guarantee a domain author
   usually wanted — but it means the planner would no longer be HDDL-conformant
   on partially ordered domains. It should be opt-in, per domain or per flag,
   not silent.
2. **It is a commitment, so it can prune solutions.** Protecting a literal
   forbids orderings that might have been fine because the literal got
   re-established. A full POCL planner backtracks over threat resolution;
   protection-by-pruning does not.

Neither caveat is a reason to avoid it, but both are reasons not to make it the
default without measuring. It is §9.4.

---

## 8. Done

Completed work, in the order it was done. §9 is what is left, in the order to
do it. The two are separate sections because they are read for different
reasons: this one is a record of what was measured and what it bought, and the
numbering is kept stable so that references to it from §2, §6 and §7 stay
valid.

§8.1–§8.6 came from §6 and attacked the cost of a node. §8.7 came from §4 and
§5 and attacked the number of nodes. The rule behind that sequence was: make
the thing measurable, then make it fast, then change what it means.

Cumulative effect on median rollout time, against the original Z3
implementation: `transport` 362×, `simple_travel` 479×, `sar3` 578×,
`d18_gather` 912×, `d18_p18` 478×, `forall_test` 993×, `atom_test` 563× — plus
15× again on the largest chain instance any encoding solves, from §8.7, which
is a different kind of win and is measured on different instances. §8.8 bought
no speed at all and is not meant to: it converts two crashes and two hangs into
reported failures.

---

### 8.1 Benchmark harness — **done**

`apps/bench/planner_bench` plus `scripts/benchmark`. See §6.5 for what it
measures and why it measures two different things.

    scripts/benchmark                    # ~40s, reproducible
    scripts/benchmark --save base.json   # before a change
    scripts/benchmark --compare base.json # after; exits non-zero if behaviour moved
    scripts/benchmark --full             # the real time-limited planner, ~20 min

### 8.2 Delete dead weight — **done**

`boost::json` was unused — six `tag_invoke` overloads in `typedefs.h`
serializing `Grounded_Task`, `TaskGraph`, `TaskNode` and `TaskTree`, left from
the plan-recognizer JSON output, with no caller of `value_from` or `value_to`
anywhere. `util.h` and `grapher.h` carried the include and a namespace alias
without using either. **Boost.Log was dead too**: nothing includes it, and the
only traces were a `-DBOOST_LOG_DYN_LINK` define and a `find_package` in
`test/CMakeLists.txt`.

What Boost is actually used for — Spirit, Fusion, Variant, Optional and
`boost/test/included` — is all header-only, so the one compiled component the
project needs is **program_options**, and only for the two apps. `lib/` and
`test/` now ask for headers alone.

`CMAKE_BUILD_TYPE` defaults to `Release`, so a plain `cmake ..` compiles with
`-O3 -DNDEBUG` rather than no `-O` flag at all.

**Payoff, measured: none, as predicted.** The binary no longer links
`libboost_json`, but clean build time is unchanged at 20s either way, and the
benchmark shows no timing change beyond noise — the `-O3` default included,
for the reason in §6.1.4. This was hygiene, and it is worth being clear it
bought nothing else.

### 8.3 Keep the parsed AST — **done**

`lib/expr.h` defines a small expression IR, and the loader builds it alongside
the SMT string for every action precondition, method `:constraints` and
conditional effect. Nothing evaluates it yet; §8.4 will.

It is deliberately **not** the Spirit AST. A planner-owned type keeps the
evaluator away from Spirit's variant layout and position-tracking, and lets the
loader record something the SMT text cannot express: whether an argument is a
**variable or a constant**. The string spells both as a bare name, and the
current engine only recovers the difference from which names `ask` happened to
declare. The evaluator will need it directly.

**Fidelity is asserted, not assumed.** `expr::to_smt` renders the IR back to
the loader's own string format, and `test_loader` checks that every stored
precondition and effect condition in all six domains round-trips exactly —
163 of them. The test was confirmed to fail when the renderer was deliberately
broken. `to_smt` exists only for that check and goes away with the SMT path in
§8.4.

Two things the IR must reproduce that are easy to miss: `__NONE__` propagation
(a null `Ptr`, which `and`/`or` drop and which collapses a whole node if every
part is absent), and the desugaring of typed quantification into
`(forall ((v __Object__)) (=> (and (type v)) body))`, including taking the
inferred type order from the same `type_inference` call the string path uses.

**Coverage note.** The shipped domains exercise atoms, `and`, `not`, `or`,
equality and inequality, plus `forall` in effects. None use `imply`, `exists`,
or a quantified *precondition*, so those branches of the IR are written but
unexercised. §8.4's fallback-to-Z3 covers them either way, but they are the
places to look first if something is wrong.

**Depends on:** §8.1. **Risk:** none — pure addition; the benchmark reports no
semantic change. **Payoff:** none directly.

### 8.4 Replace Z3 on the hot path — **done**

`lib/evaluator.h` answers preconditions by joining over the indexed relations
`KnowledgeBase` now keeps beside its facts. It covers the fragment the domains
use — atoms, `and`, `or`, `not`, equality, inequality, and binding enumeration
— and returns nullopt for anything else, which sends the query down the
existing Z3 path. Nothing lost, only speed gained, and only where the evaluator
is sure. Both the precondition path (`ActionDef::apply`, `MethodDef::apply`)
and the effect path (`apply_binding`: conditional effects and `forall` ranges)
go through it. See §6.6 for what it bought and §6.2 for why it was the right
shape.

**Correctness was established by differential testing, not by inspection.**
Building with `-DHTN_DIFFERENTIAL_EVAL` runs both engines on every query the
direct path claims and throws unless they agree as sets. Every domain and the
whole test suite were run that way.

It found a real disagreement, and the bug was mine. When a method's subtask
mentions a parameter the method does not bind, `MethodDef::apply_binding` falls
back to using the parameter's *name* as its value, so a grounded task can carry
`room` where an object belongs. The string path then emits `(= room room)`,
which Z3 reads as a tautology and discards, leaving the variable free to
enumerate. The evaluator had bound it to the literal `"room"` and found
nothing. Reproducing Z3's reading fixed it — and the episode is the argument
for the flag: nothing about that is visible from reading either engine alone.

**This step changes results, and that is expected.** The two engines return the
same *set* of bindings but in different orders, so the planner's shuffles land
differently and it commits to different — equally valid — plans. `sar3` now
routes a victim via `player_3` instead of `player_2`, same length, same score.
The benchmark's semantic fields therefore moved, and its baseline was retaken.
Set equality under the differential flag is the correctness claim; the
fingerprint is not.

**Still outstanding here.** The SMT path remains as the fallback, so `smt_state`
is still built on every `update_state` even though nothing reads it for the
shipped domains. Retiring it is real work — it is what `ask` consumes — and
belongs with §8.5, which changes the representation anyway.

**Depends on:** §8.3. **Risk:** was high; discharged by differential testing.
**Payoff:** 68x–330x per rollout, and the default budget back to 1000 ms.

### 8.5 Cheaper state representation — **done**

Re-profiling after §8.4 redirected this item. The premise had been string
churn from re-parsing facts; with Z3 off the query path the profile was
**51.5% malloc/free, 12% memmove/memset, 6.5% Z3**. The cost was not parsing,
it was *copying state* — there is one `KnowledgeBase` per search node, and each
carried more than it needed to. Three changes, each measured:

1. **`smt_state` is built lazily.** It is several kilobytes of SMT-LIB text
   that `update_state` produced eagerly and every copy then carried. Since the
   evaluator took over, most states are never asked anything a solver must
   answer, so most of it was built, copied and discarded unread. It is now
   materialised on demand and cached until the next update. ~30%.
2. **The predicate signatures and object map are shared.** They are fixed for
   the whole problem, so all nodes held identical copies; they now sit behind a
   `shared_ptr<const Schema>` and a copy carries a pointer. ~40% cumulative.
3. **The fact base is stored once.** It had been held twice — as
   `"(head a b)"` strings and as split argument tuples, the second rebuilt from
   the first on every `update_state`. The tuples are the useful form, so they
   are now the only one, and strings are reconstructed on the few paths that
   still want them (`get_facts`, `print_facts`).

Median rollout time, against the §6.6 numbers:

| Domain | after §8.4 | after §8.5 | | vs. the original Z3 |
|---|---|---|---|---|
| `transport` | 32.23 ms | **13.51 ms** | 2.4x | 162x |
| `sar3` | 0.98 ms | **0.26 ms** | 3.8x | 467x |
| `d18_gather` | 1.07 ms | **0.18 ms** | 5.9x | 709x |
| `d18_p18` | 0.73 ms | **0.21 ms** | 3.5x | 341x |
| `forall_test` | 0.06 ms | **0.03 ms** | 2x | 662x |
| `atom_test` | 0.03 ms | **0.01 ms** | 3x | 563x |
| `simple_travel` | 4.73 ms | **4.69 ms** | — | 4x |

Unlike §8.4 this is behaviour-preserving, and the deterministic plan
fingerprints are unchanged throughout.

**What this item did not do.** The profile was still **48.7% malloc**
afterwards. Relations hold `std::string`, so every argument of every fact is a
separate allocation, copied per node; `TaskGraph` and the plan vector carry
strings too. Interning predicates and objects to integers at load — the
original plan for this item, and the reason it was named "representation" —
is the change that removes that, and it was not done here because it reaches
much further than `kb.h`: `Grounded_Task`, `TaskGraph`, the evaluator and the
public `get_facts` API all traffic in strings. **It is now §9.2.**

`simple_travel` did not move at all here, and the explanation offered at the
time — that its rollouts were bound by recursion through a `get_to`-style task
structure — was **wrong**. §8.6 found the real cause: its score function was
asking Z3 two ground questions per rollout, which dwarfed everything else, and
routing those through the evaluator made it 117x faster. Attributing a cost
without measuring it is guesswork even when the guess is plausible.

**Depends on:** §8.4. **Risk:** low, discharged. **Payoff:** 2–6x on top of
§8.4.

### 8.6 Kill the deep copies in the hot path — **done**

The first half of this item (sharing the immutable schema, dropping
`smt_state`) landed in §8.5. For the rest, attributing allocation samples to
the nearest planner frame put **19.8% in `simulation`** alone, from its
by-value `KnowledgeBase` and `TaskGraph` parameters. Three changes:

1. **`simulation` takes state and tasks by reference.** A rollout never mutates
   either — successors come back fresh from `ActionDef::apply`, and
   `MethodDef::apply` takes its own copy because it removes the decomposed task
   from the network — so a copy per recursion level was pure cost. ~20%.
2. **The method list is shuffled by index.** `auto task_methods =
   domain.methods[...]` deep-copied every `MethodDef` with its subtask and
   ordering maps, purely in order to shuffle it. An index permutation does the
   same job.
3. **Ground queries no longer reach Z3.** Score functions ask through
   `KnowledgeBase::ask(std::string)` with hand-written ground text, which only
   a solver could answer. `expr::parse_ground` reads those strings into the IR
   once, cached, and they are answered from the fact index like everything
   else.

Median rollout time against §8.5, and against where this started:

| Domain | after §8.5 | after §8.6 | | vs. the original Z3 |
|---|---|---|---|---|
| `transport` | 13.51 ms | **6.05 ms** | 2.2x | **362x** |
| `simple_travel` | 4.69 ms | **0.04 ms** | 117x | **479x** |
| `sar3` | 0.26 ms | **0.21 ms** | 1.2x | **578x** |
| `d18_gather` | 0.18 ms | **0.14 ms** | 1.3x | **912x** |
| `d18_p18` | 0.21 ms | **0.15 ms** | 1.4x | **478x** |
| `forall_test` | 0.03 ms | **0.02 ms** | 1.5x | **993x** |
| `atom_test` | 0.01 ms | **0.01 ms** | — | **563x** |

Behaviour-preserving: every deterministic plan fingerprint is unchanged, and
the differential build agrees on every domain, the ground path included.

**Z3 no longer appears in the profile at all.** It stays linked and stays the
fallback for an expression the evaluator declines — `imply`, `exists`, a
quantified precondition, or ground text `parse_ground` will not read — but on
these domains nothing reaches it.

**What is left, and where it went.** The profile after this item is **53.5%
malloc, 30.3% our own code, 10% memcpy/memset**. The allocation is
`KnowledgeBase` and `TaskGraph` copies whose contents are `std::string`: one
per search node, one per binding in `apply_binding`. Two things would move it,
larger first, and both are now items of their own: **§9.2** (interning, which
§8.5 also left open) and **§9.3** (a trail-based state).

They are no longer next, though. §8.7 showed that on the instances that are
actually hard the binding constraint is the *number* of nodes rather than the
cost of one, so §9.1 comes first. See the §9 preamble.

**Depends on:** §8.4. **Risk:** low, discharged. **Payoff:** 1.2x–117x on top
of §8.5.

### 8.7 Re-encode the recursive `goto` pattern out of the domains — **done**

**Paper.** Godet, R., Bit-Monnot, A., & Lesire-Cabaniols, C. (2024). *Redundant
Decompositions in PO HTN Domains: Goto Considered Harmful.* 7th ICAPS Workshop
on Hierarchical Planning, 36–44.

It is a **domain modelling** result, not a planner algorithm and not about
method preconditions. It identifies a pattern common in partially ordered HTN
domains that makes the number of decompositions explode: a recursive "get me to
X" compound task, several of which sit unordered relative to each other, each
able to contribute the same movement actions — so one plan is reachable through
n^k decompositions, for n such tasks and k movement actions.

**`transport_domain.hddl` has exactly this pattern.** `get_to` is their `goto`,
with the same three methods — one hop, recurse, already-there — and
`m_deliver_ordering_0` puts *two* unordered `get_to` subtasks in one network.

They propose two re-encodings, both in ordinary HTN with no planner support: a
**mutex** model, and a **(partial) task insertion** model that mimics FAPE's
task-dependent/task-independent split (§7.3) by adding a recursive `free-move`
compound task. They report that task insertion "clearly dominates" for native
PO HTN planners.

**On this planner it does not.** It is the worst of the five encodings by more
than three orders of magnitude, and the rewrite they use as their improved
baseline is also a loss. What helps is one half of the *other* rewrite, applied
to a base they do not apply it to.

#### 8.7.1 The instances had to be built first

The shipped `transport_problem.hddl` does not exercise the pattern at all, and
that is provable from the domain text rather than merely observed. The
recursive method's precondition is

    (at ?v ?l1) ∧ (road ?l2 ?l3) ∧ ¬(road ?l1 ?l3) ∧ ¬(at ?v ?l2) ∧ ¬(at ?v ?l3)

and the instance has three locations with all six directed roads present.
`(at ?v ?l1)` pins `?l1` to where the vehicle is. `¬(road ?l1 ?l3)` then forces
`?l3 = ?l1`, since a road exists between every pair of distinct locations. But
that makes `¬(at ?v ?l3)` contradict `(at ?v ?l1)`. The conjunction is
unsatisfiable, so the method can never contribute an action here.

Measurement agrees and prices it: deleting the method leaves the same
distribution of rollout scores with no failures — nine of ten rollouts at
0.625 and one at 0.600, in both cases — while cutting rollouts from 712 nodes
to 429. (The individual scores come out in a different *order*, because
removing a method changes how many draws the RNG is asked for; the multiset is
what is meaningful.) So roughly 40% of a rollout on the shipped instance was
spent generating bindings for a method whose precondition then rejected every
one of them.

That is why §6.1's transport profile shows nothing of this, and why the
experiment needed new instances. `scripts/gen_transport_chain.py` generates
them, and the shape is load-bearing in two ways:

* **A chain**, so paths are longer than one hop and the recursion actually
  fires.
* **One-way** roads, so it terminates. This planner's rollouts are an unbounded
  depth-first search with no cycle detection or depth limit, and on a two-way
  chain the recursion can oscillate between two locations forever — in the
  left-recursive encoding as readily as the right-recursive one. With one-way
  roads every recursive step strictly advances along the chain in all five
  encodings. That constraint is a fact about this planner, not about the
  domains, and it is the first thing that would have to change to run these
  encodings on the IPC instances.

The ladder moves one knob at a time from instance `b`: packages set n (two
`get_to` subtasks each), chain length sets k.

#### 8.7.2 Results

Five encodings, five rollouts each at seed 2022. Median rollout time, and mean
search nodes visited per rollout. Nodes were counted with temporary
instrumentation in `simulation`, removed afterwards.

| | **a** 4 loc, 2 pkg | **b** 5 loc, 2 pkg | **c** = b + a package | **d** = b + a location |
|---|---|---|---|---|
| | 3 drives, 4 `get_to` | 4 drives, 4 `get_to` | 4 drives, 6 `get_to` | 5 drives, 4 `get_to` |
| `original` | 7.6 ms / 1 385 | 97.8 ms / 24 156 | — | 174.1 ms / 80 082 |
| `common` | 51.8 ms / 7 921 | 55.1 ms / 22 194 | — | 49.7 ms / 79 825 |
| `mutex` | 9.8 ms / 1 103 | 17.8 ms / 4 745 | — | 24.4 ms / 7 529 |
| **`mutex_left`** | 9.5 ms / 2 925 | **13.1 ms / 1 469** | — | **11.4 ms / 1 378** |
| `insert` | 37 702 ms / 4 646 976 | — | — | — |

An em dash is a timeout: a single run of five rollouts did not finish in 300 s.

Within each instance every encoding that finished returned the same score, so
all of them found equally short plans and the timing comparison is between
equal outcomes. `delivery_chain` rewards fewer drives precisely so that the
insert model's freedom to emit an unasked-for drive would show up here; it did
not, on the one instance insert could finish.

**Node counts track time, so this is search-space size and not cost per node.**
That is the point of the whole item: every other step in §8 made nodes cheaper,
and this one makes there be fewer of them. The one place the two disagree is
`common` at instance d, which visits the same number of nodes as `original`
(79 825 vs 80 082) while taking a third of the time. That is unexplained; with
a median over five rollouts against a mean over the same five, it may be
nothing. It does not affect any of the conclusions below, all of which rest on
differences of one to three orders of magnitude.

#### 8.7.3 The two rewrites in `common` are independent, and only measuring them separately makes the result legible

What the paper calls the `common` version of transport bundles two changes:
drop the one-hop method, and flip the recursion from left (`get_to` an
intermediate, then drive) to right (drive one hop, then `get_to` again).
Splitting them, on instance a:

| | nodes per rollout |
|---|---|
| left recursion + one-hop method (= `original`) | 1 385 |
| left recursion, one-hop method removed | *domain is incomplete — every rollout fails* |
| right recursion + one-hop method | 7 729 |
| right recursion, one-hop method removed (= `common`) | 7 921 |

**The direction of the recursion is the dominant term, and here it is a loss.**
The reason is visible in the domain text. The left-recursive method carries a
goal-directed guard: `(road ?l2 ?l3)` forces the intermediate to neighbour the
*target*, and `(not (road ?l1 ?l3))` forbids recursing at all when a direct hop
exists. Together they make the recursion walk backwards from the destination
and stop as soon as it meets the vehicle. The right-recursive form has no
equivalent — `(road ?l1 ?ln)` says only "drive somewhere" — so the search
wanders forward and discovers its mistakes late.

The second row is the reason the one-hop method cannot simply be dropped here:
`(not (road ?l1 ?l3))` presupposes it. Without it a one-hop journey has no
applicable method at all.

Godet et al. report the same split by planner rather than by encoding — Aries,
which chains backwards, prefers left recursion; PandaPi and LinearComplex,
which chain forwards, prefer right. This planner chains forwards through
*states*, so the naive expectation is that it should behave like PandaPi. It
does not, and the reason is that PandaPi has the FF heuristic to tell it which
way to wander. This planner's rollouts are uninformed. The guard in the
left-recursive method is the only guidance there is, so discarding it costs
more than the better state visibility buys.

#### 8.7.4 The mutex helps, and helps more as instances grow

The mutex rewrite makes a `get_to` take the vehicle's lock before it may emit
any drive and hold it until the vehicle arrives, so no other `get_to` can
contribute drives in the middle of one's sequence. It collapses the number of
decompositions of a given drive sequence from exponential in the number of
unordered `get_to` tasks to linear in it.

Applied to the paper's `common` base it recovers most of what right recursion
lost. **Applied to the `original` left-recursive base — which the paper does
not do, and which `transport_mutex_left.hddl` is — it is the best encoding by a
wide margin, and its cost barely moves with instance size at all:** 2 925 →
1 469 → 1 378 nodes across a, b, d, against `original`'s 1 385 → 24 156 →
80 082. Against `original` at instance d that is **15× in time and 58× in
nodes**.

The flatness makes sense once the mechanism is clear. On a one-way chain the
drive sequence itself is forced; the only thing there was to search was which
`get_to` should contribute which drive, and the lock removes exactly that.

#### 8.7.5 Task insertion is catastrophic here, and it is the insertion that does it

`insert` needed 4 646 976 nodes on the smallest instance where `original`
needed 1 385, and timed out everywhere else.

The obvious suspect was grounding width rather than the idea itself. In this
encoding the drives come from a `free_drive` task whose method has two free
location parameters, so under HDDL precondition timing (§2.3.1) it enumerates
L² bindings and the synthesised check rejects them one step later. A probe that
threads the vehicle's location through `free_drive`'s task parameters narrows
that to L:

| instance a | nodes per rollout |
|---|---|
| `insert`, L² grounding (as the paper models it) | 4 646 976 |
| `insert`, L grounding (location threaded through the task) | 1 516 457 |
| `original` | 1 385 |

So grounding width is worth 3×, and the remaining **1 100×** is not it. The
cause is insertion freedom itself. `free_drive` sits unordered against
everything, so at *every* node of the search it is a candidate, and the
hierarchy no longer says anything about which drives to emit or when. The
model buys decomposition uniqueness by giving up the guidance that made the
decompositions worth having.

That is a good trade for PandaPi and Aries, which have a heuristic and a CSP
solver respectively to replace what was given up. It is a ruinous one for a
uniformly random depth-first rollout, which has nothing.

#### 8.7.6 What the redundancy actually is here, and what is left of it

Instance c is the sharpest result in the table and it is a negative one.
Adding one package to instance b defeats **every** encoding, `mutex_left`
included, while adding one location to the same instance leaves `original`
solving it in 174 ms.

Godet et al.'s n^k does not predict that: a package takes n from 4 to 6 at
k = 4, a location takes k from 4 to 5 at n = 4, and those are comparable. So
the dominant cost in this domain is not the `goto` attribution their paper is
about. The mutex fixes the attribution redundancy, which is why `mutex_left` is
flat in chain length — and it still dies on the package knob.

**What the extra cost is was then guessed, and the guess was wrong.** This
section originally attributed it to the **interleaving of the unordered
top-level `deliver` tasks**, on the arithmetic that three unordered tasks of
four ordered subtasks each admit 12!/(4!)³ ≈ 34 650 interleavings against
8!/(4!)² = 70 for two. That reasoning is sound about interleavings and says
nothing about whether they are what costs.

§8.9 tested it directly, by restricting interleaving in the rollouts three
different ways — including Algorithm 3, which stops primitive actions
interleaving with pending decompositions at all. **Instance c times out under
every one of them, for both encodings, eight configurations in total.** If
interleaving were the binding constraint, forbidding it would have moved
something.

The honest state of it: the package knob is far more expensive than the
location knob, that is measured and reproducible, and the cause is not
decomposition-order redundancy of any kind this planner can restrict away. The
likeliest remaining explanation is that a third package simply enlarges the
space of genuinely distinct valid plans — which is not redundancy at all, and
not something a restriction can help with — but that has not been measured, and
it is recorded here as an open question rather than as a second guess.

#### 8.7.7 What to carry into the RTL domains

* **Prefer rewrites that remove choice without removing information.** That is
  the whole difference between the mutex, which is a clean win, and both the
  right-recursion flip and task insertion, which are losses. A planner with a
  heuristic can afford to trade guidance for a smaller space. This one cannot,
  and will not be able to until it has one.
* **Encoding results do not transfer between planners.** The paper's own
  conclusion says as much — "planner-independent modeling remains a
  far-fetched goal" — and this is a clean instance of it: their recommended
  encoding is this planner's worst, by 3 000×.
* **Watch the number of unordered top-level tasks before the depth of any
  recursion.** In this domain it was the more expensive knob by far.
* **Recursive tasks need a termination argument that does not depend on the
  search.** Every encoding here relies on one-way roads. Without a depth bound
  or cycle detection in `simulation`, a domain that can cycle will hang a
  rollout rather than fail it.

**Artifacts.** `domains/transport_{original,common,mutex,mutex_left,insert}.hddl`,
the instances from `scripts/gen_transport_chain.py`, and `scripts/goto_experiment`,
which reruns the grid. One encoding is in the regression harness as
`chain_mutex` so the variant domains and the `delivery_chain` scorer stay
covered; comparing the five against each other belongs in the experiment
script, not the benchmark.

**Depends on:** §8.1. **Risk:** none to the planner — the one code change was
a loader crash this turned up (§4.1 note 19). **Payoff:** 15× on the largest
instance anything solves, and flat where the original grows.

*Attribution note: this is sometimes mis-cited to Alford, Bercher & Aha. That
is the 2015 task-insertion paper Godet et al. cite as the source of the
mechanism they mimic; Alford and Bercher organise the workshop.*

### 8.8 Bound rollout depth — **done**

`simulation` was an unbounded depth-first search with no depth limit and no
cycle detection, so a domain whose decomposition can cycle did not make a
rollout fail — it made the process hang or die. Measured on a two-way road
graph, where §8.7's encodings all needed one-way roads for exactly this reason:
the left-recursive encoding ran 45 s without finishing where the one-way
version takes 100 ms, and the right-recursive one exhausted the stack and
exited 139.

**The type carries the fix.** `simulation` returned `std::optional<double>`,
where `nullopt` meant "no plan", and callers wrote `if (!rs)`. It now returns

    struct RolloutResult { RolloutStatus status; double score; };
    enum class RolloutStatus { Solved, Refuted, Cutoff };

with deliberately **no conversion to `bool`**, so the old spelling no longer
compiles and every call site has to say which failure it means. That is the
whole point: `Refuted` means the DFS exhausted the subtree, which on a finite
space is a *proof* of a dead end and is what licenses marking the node dead
(§2.4). `Cutoff` means the bound bit, and proves nothing.

**The trap is real, and was demonstrated rather than assumed.** A build that
treats `Cutoff` as `Refuted` — one added line — turns `transport_chain_b`
at `--max_depth 40` from a 14-action plan into a failure at the root, and
reports the cause as a time limit rather than as anything to do with depth.
The correct build solves it, because a cutoff leaves the node expandable:
`expansion` descends past the cut and the rollout from the deeper node starts
again with a full budget. That is what keeps the bound from costing solutions.

**A cutoff abandons the rollout immediately.** The first implementation noted
the cutoff and carried on trying sibling branches, which keeps a rollout
exhaustive *within* the bound and is badly wrong: a rollout that succeeds
returns at the first plan it reaches, but one that continues past a cutoff must
exhaust the entire depth-bounded subtree before it can answer. `chain_b` plans
in 0.9 s at the default bound and had **not finished after 75 minutes of CPU**
at a bound of 30. Bailing on the first cutoff brought that to 0.5 s, and costs
nothing that matters, because `Refuted` is still only returned when no cutoff
occurred anywhere beneath the node.

**A second backstop was needed, and the depth bound is what exposed it.**
`stuck_counter` resets to 10 on every successful commit, so it bounds a run of
consecutive backtracks and never the commit loop as a whole. That was harmless
while every unsuccessful rollout was a refutation — refuted nodes are marked
dead, dead options run out, the loop converges. `Cutoff` marks nothing dead, so
a bound too tight for the domain leaves UCT with no signal, every commit
effectively random, and commit/backtrack able to alternate without end. A
global cap on committed decisions (`--max_decisions`, default 1000, against a
longest shipped plan of 16 actions) turns that into a reported failure naming
`--max_depth`.

**The default bound is 1000, chosen against measurement.** The deepest rollout
across every shipped domain and every §8.7 chain instance is 51 — `transport`
30, `sar3` 19, `d18` 14, `simple_travel` 7, the chain instances 39–51 — so the
default sits roughly 20× above the worst case seen. It never binds on the
shipped domains, and the benchmark confirms it: no semantic field moves.

#### 8.8.1 What this does not do, and a prediction it falsified

The item as written claimed this would "unblock two-way road graphs and the IPC
instances". **That was wrong.** Cycling domains now terminate and report
instead of hanging or crashing — rollouts on the two-way chain come back
`cutoff` in 66 ms and 1.1 s for the two encodings — but every rollout cuts off,
so the search has no signal and cannot solve them. The planner grinds to the
decision cap and says so; it does not plan.

The follow-up this seemed to imply was cycle detection, and measuring killed
that too. Counting recursion steps that land on a (state, task network)
configuration already on the current DFS path: **2 548 of 137 632 nodes (1.9%)
on the two-way chain, against 8 of 50 175 (0.016%) on the one-way one.** A 120×
ratio, and still nowhere near enough — eliminating every one of them would not
dent the blowup. The two-way explosion is *branching*, not cycling: every
movement decomposition gains a second applicable direction and an uninformed
DFS has no way to prefer the right one. That is the same gap §8.7 kept running
into, and it is named under "Not on this list" rather than here.

**Depends on:** nothing. **Risk:** low; discharged by the trap demonstration
and by the benchmark. **Payoff:** turns two crashes and two hangs into reported
failures. No effect on any domain that plans today.

### 8.9 Restrict interleaving in the rollouts — **done, and rejected**

A negative result, and the item that produced it was one I wrote myself on the
strength of §8.7.6's diagnosis. Both the diagnosis and the fix it implied turn
out not to survive measurement.

`expansion` branches over every unconstrained primitive task but only one
unconstrained compound task (Algorithm 2, §2.3.2). `simulation` deliberately
does not, and rollouts are where essentially all the planner's time goes, so
giving rollouts the same restriction looked like the obvious lever — especially
after §8.7.6 concluded that interleaving of unordered top-level tasks was the
dominant cost.

It had been measured once before and rejected (§2.3.2), but that measurement
was taken on the shipped `transport_problem.hddl`, which §8.7.1 showed cannot
exercise the pattern at all. Retaking it on instances that can was the point.

**Three strategies, not one.** The restriction has a free choice in it — *which*
compound task to keep — and the answer for a tree is not obviously the answer
for a randomised dive:

* **lowest id**, exactly as `expansion` picks it, so the rollout explores the
  same shape of space the tree does;
* **random**, which keeps the branching reduction but restores the
  per-rollout diversity a randomised dive depends on;
* **Algorithm 3**, the strongest: while any compound task is unconstrained,
  progress no action at all, so primitives cannot interleave with pending
  decompositions either.

Median rollout time, same seeds, identical scores throughout — so every column
is an equal outcome and the comparison is clean:

| case | none | lowest id | random | Algorithm 3 |
|---|---|---|---|---|
| `chain_a` / original | **8.9 ms** | 28.8 | 30.5 | 7.8 |
| `chain_d` / original | **173 ms** | 302 | 168 | 374 |
| `chain_d` / mutex_left | 12.6 ms | 30.8 | **11.2** | 17.2 |
| `transport` (shipped) | **5.1 ms** | 7.4 | 5.9 | 6.0 |
| `d18_gather` | **0.15 ms** | 0.25 | 0.15 | 0.26 |
| `sar3` | 0.19 ms | 0.19 | 0.19 | — |

**No strategy is a consistent win, and the one `expansion` uses is the worst.**
Lowest-id costs up to 3.2×. The reason §2.3.2 gave originally — narrowing the
choices removes the lucky first paths a random dive depends on — survives
contact with instances that can see the effect.

**A partial vindication that still loses.** The determinism of the pick was
worth suspecting: every rollout from a node explores compounds in the same
order, so restriction costs diversity as well as branching. Random picking does
recover most of the lowest-id penalty (`chain_d`/original 302 → 168,
`chain_d`/mutex_left 31 → 11, `d18_gather` 0.25 → 0.13). It still does not beat
leaving rollouts alone, and on `chain_a`/original the restriction costs 3.2×
however the compound is chosen, so determinism was never the whole story.

#### 8.9.1 The claim this was built on, falsified

§8.7.6 attributed instance c's cost to interleaving of the unordered top-level
`deliver` tasks. The strongest possible version of this item is the direct test
of that: Algorithm 3 forbids the interleaving outright.

**Instance c times out under all four strategies, for both encodings — eight
configurations, 300 s each.** If interleaving were the binding constraint,
forbidding it would have moved something. It did not move anything.

So §8.7.6 is corrected rather than extended. The package knob really is far
more expensive than the location knob — that is measured and reproducible — but
the cause is not decomposition-order redundancy of a kind this planner can
restrict away. The likeliest remaining explanation is that a third package
enlarges the space of genuinely distinct valid plans, which is not redundancy
and not something any restriction helps with. That is a hypothesis, and it is
recorded as one; it has not been measured.

**What was kept.** `--restrict_rollouts` stays in the code with a default of 0
(unrestricted), so the question can be re-asked on the RTL domains without a
patch. §2.3.2 documents the design choice and now has executable evidence
behind it rather than a single suspect measurement. The default is
behaviour-preserving: the benchmark reports no semantic change.

**Depends on:** §8.1 and the §8.7 instances. **Risk:** none — default unchanged.
**Payoff:** none. The value here is the falsification, which removes a wrong
reason for prioritising §9.1.

---

## 9. Remaining work

Ordered by what to do next rather than by when it was thought of. Two things
reordered it, and both came out of doing §8.7. (§8.8 and §8.9 have since been
done out of this list. §8.8 changed nothing about the order; §8.9 was a
negative result that removed one of §9.1's two reasons for being first — see
there.)

* **The bottleneck moved.** §8.1–§8.6 all attacked the *cost of a node*, and
  returned 362–993× between them. §8.7 was the first item that attacked the
  *number of nodes*, and it returned 15× on instances where cost-per-node work
  had stopped helping at all. Chain instance `c` still times out past 300 s;
  halving the cost of a node does nothing for that. So search-space work comes
  first now, and the representation work that used to be next is behind it.
* **Representation work should follow the search changes, not precede them.**
  Interning reaches into `Grounded_Task`, `TaskGraph`, the evaluator and the
  public `get_facts` API, and §9.1 changes how `TaskGraph` is used.
  Doing it first means doing parts of it twice.

**This order assumes the goal is to plan on harder instances.** If the
near-term goal is instead to generate and run many *small* RTL domains, the
search space is not what hurts, and §9.2 and §9.4 should come first.

§9.1 is search space. §9.1–§9.2 are cost per node, and are what §8.5 and §8.6
left behind. §9.3 is semantics. §9.4 is hygiene and can be folded in anywhere.

---

### 9.1 Algorithm 3 (systematic progression)

`expansion` implements Algorithm 2 (§2.3.2). Algorithm 3 would make the search
fully systematic; §5 records the algorithm, what it would take, and why to be
careful — it postpones the state update, which is what the rollouts and the
score functions read. Two of the four shipped domains also break the
assumptions its systematicity theorem needs (§5.4), so it would be sound and
complete but not fully systematic on them.

**§8.9 already tried the cheap half of this, and it lost.** The same idea —
restricting how much interleaving the search branches over — applied to the
*rollouts*, where the time actually goes. Every strategy tried was slower than
leaving them alone, Algorithm 3 among them, and none made instance c solvable.

How much that transfers is a real question and should not be waved either way.
It does **not** transfer directly: rollouts are uninformed randomised dives
that depend on diverse first paths, which is exactly what a restriction takes
away, whereas the tree has UCT to supply exploration, so the mechanism that
sank §8.9 has no analogue in `expansion`. What §8.9 does establish is narrower
and still useful — that **restricting decomposition order is not what makes
instance c hard**, because forbidding interleaving outright did not move it.
That was one of the two reasons this item was near the front, and it is gone.
The other reason, systematicity for its own sake, stands.

So: still worth doing, no longer obviously first, and worth being honest that
its expected payoff is now lower than when it was written. The thing to measure
is whether removing duplicate subtrees pays for the postponed state update —
not whether it fixes instance c, which §8.9 has already answered.

**Depends on:** §8.1 and §8.7. **Risk:** medium. **Payoff:** unknown, and
lower than §8.9 was expected to make it look; measure.

### 9.2 Intern predicates, objects and task names

Left over from §8.5, which is where the reasoning is. After §8.6 the profile is
**53.5% malloc, 30.3% our own code, 10% memcpy/memset**, and the allocation is
`KnowledgeBase` and `TaskGraph` copies whose contents are `std::string`: one
per search node, one per binding in `apply_binding`. Every argument of every
fact is a separate allocation, copied per node.

Interning them to integers at load is the change that removes it, and it is the
largest remaining cost-per-node item. It was not done in §8.5 because it
reaches much further than `kb.h`: `Grounded_Task`, `TaskGraph`, the evaluator
and the public `get_facts` API all traffic in strings. That reach is also why
it belongs after §9.1, which changes how `TaskGraph` is used.

**Depends on:** §8.4; sequenced after §9.1. **Risk:** medium — wide, mechanical,
and the differential-eval flag from §8.4 does not cover it. **Payoff:** the
largest single remaining cost-per-node item.

### 9.3 Trail-based apply/undo state

Also left over from §8.6. Apply an action's effects and undo them on the way
back out, instead of copying the fact base per successor. It removes copying
rather than shrinking it, which is why it comes after §9.2 shrinks what there is
to copy.

A bigger design change than it sounds: the MCTS tree holds states, not just the
rollout, so the trail cannot simply be unwound at every level.

**Depends on:** §9.2. **Risk:** medium-high. **Payoff:** removes the copy rather
than making it cheaper.

### 9.4 Decide what a method precondition should mean here

§7.4 sets out a protected-condition set: register the literals a synthesised
precondition action checked, keep them protected until the method's subtasks
have all been applied, and make any action that would falsify one inapplicable.
POCL threat-checking transposed into progression search, at about the cost of a
set intersection per action application.

This is a **semantics decision, not an optimisation**. It is strictly stronger
than HDDL — it rejects plans HDDL accepts — so it must be opt-in, and it prunes
by commitment rather than backtracking, so it can lose solutions a full POCL
planner would find. Measure before defaulting it on.

It sits last among the substantive items for the reason §8's sequence gave:
every semantic change has to be evaluated by running the planner a great deal,
and that is cheaper after §9.1–§9.2. It is not blocked by any of them, so if the
research needs it sooner, it can move.

**Depends on:** §8.1 and §8.4. **Risk:** medium, and it changes results.

### 9.5 Loose ends

Both are recorded as open items in §4.2 and neither is worth its own work
session:

* **Item 17 — the tests only run from `<source>/build`.** `test_loader`,
  `test_parser` and `test_MCTS_planner` reach their domains through
  `../../domains/...`. From anywhere else they compile, link, and then fail at
  run time. This cost real time during §8.7: run from the wrong directory,
  `test_loader` exits 201 and looks exactly like a genuine assertion failure.
  Passing the domain directory in at configure time fixes it.
* **Item 18 — semantics to document.** Goals are ignored (§2.1); the search
  space is non-systematic, so duplicate subtrees occur (§2.3).

---

### Not on this list, deliberately

* **Memory work.** Peak RSS is 35 MB (§6.1.4). §8.6 tidied real redundancy, but
  memory is not a problem and should not be treated as one until a domain makes
  it one.
* **Goal handling.** The planner ignores `:goal` by design, per standard HTN
  semantics (§2.1). Left alone deliberately.
* **Adopting POCL or timelines wholesale.** §7.4 — that is a different planner,
  and it would give up the state-in-hand that the rollouts depend on.
* **Giving the planner a heuristic.** Named here because §8.7 kept running into
  its absence: the encodings that work for PandaPi and Aries assume a heuristic
  to replace the guidance they give up, and this planner's rollouts are
  uniformly random. That makes it the largest structural difference between
  this system and the ones the literature measures. It is a research direction
  rather than a to-do, and nothing on the list above depends on it.

---

## References

- Alford, R., Bercher, P., & Aha, D. W. (2015). Tight bounds for HTN planning. *ICAPS 2015*. https://dl.acm.org/doi/10.5555/3038662.3038665
- Alford, R., Shivashankar, V., Kuter, U., & Nau, D. (2012). HTN problem spaces: Structure, algorithms, termination. *SoCS 2012*. https://ojs.aaai.org/index.php/SOCS/article/view/18239
- Auer, P., Cesa-Bianchi, N., & Fischer, P. (2002). Finite-time analysis of the multiarmed bandit problem. *Machine Learning*, 47, 235–256.
- Behnke, G., Höller, D., & Biundo, S. (2017). This is a solution! (… but is it though?) – Verifying solutions of hierarchical planning problems. *ICAPS 2017*.
- Bercher, P., Alford, R., & Höller, D. (2019). A survey on hierarchical planning – One abstract idea, many concrete realizations. *IJCAI 2019*.
- Bit-Monnot, A., Ghallab, M., Ingrand, F., & Smith, D. E. (2020). FAPE: a constraint-based planner for generative and hierarchical temporal planning. arXiv:2010.13121. https://arxiv.org/abs/2010.13121
- Brenner, M., & Nebel, B. (2009). Continual planning and acting in dynamic multiagent environments. *JAAMAS*, 19(3), 297–331.
- Browne, C. B., et al. (2012). A survey of Monte Carlo tree search methods. *IEEE TCIAIG*, 4(1), 1–43.
- Cazenave, T., & Jouandeau, N. (2007). On the parallelization of UCT. *Computer Games Workshop 2007*.
- Chaslot, G. M. J.-B., Winands, M. H. M., & van den Herik, H. J. (2008). Parallel Monte-Carlo tree search. *CG 2008*.
- Chaslot, G. M. J.-B., Winands, M. H. M., van den Herik, H. J., Uiterwijk, J. W. H. M., & Bouzy, B. (2008). Progressive strategies for Monte-Carlo tree search. *New Mathematics and Natural Computation*, 4(3), 343–357.
- Clark, K. L. (1978). Negation as failure. In *Logic and Data Bases*, 293–322.
- Corrêa, A. B., Pommerening, F., Helmert, M., & Francès, G. (2020). Lifted successor generation using query optimization techniques. *ICAPS 2020*, 80–89. https://icaps20.icaps-conference.org/paper88.html
- Coulom, R. (2006). Efficient selectivity and backup operators in Monte-Carlo tree search. *CG 2006*.
- de Moura, L., & Bjørner, N. (2008). Z3: An efficient SMT solver. *TACAS 2008*.
- Erol, K., Hendler, J., & Nau, D. S. (1994). HTN planning: Complexity and expressivity. *AAAI 1994*.
- Geier, T., & Bercher, P. (2011). On the decidability of HTN planning with task insertion. *IJCAI 2011*.
- Ghallab, M., Nau, D., & Traverso, P. (2016). *Automated Planning and Acting*. Cambridge University Press.
- Godet, R., Bit-Monnot, A., & Lesire-Cabaniols, C. (2024). Redundant decompositions in PO HTN domains: goto considered harmful. *7th ICAPS Workshop on Hierarchical Planning (HPlan 2024)*, 36–44. https://icaps24.icaps-conference.org/program/workshops/hplan/HPlanProceedings-2024.pdf
- Gomes, C. P., Selman, B., & Kautz, H. (1998). Boosting combinatorial search through randomization. *AAAI 1998*.
- Gregory, P., Long, D., Fox, M., & Beck, J. C. (2012). Planning modulo theories: Extending the planning paradigm. *ICAPS 2012*. https://ojs.aaai.org/index.php/ICAPS/article/view/13505
- Höller, D., & Bercher, P. (2022). Compiling HTN plan verification problems into HTN planning problems. *ICAPS 2022*. https://bercher.net/publications/2022/Hoeller2022VerificationViaCompilation.pdf
- Höller, D., Behnke, G., Bercher, P., Biundo, S., Fiorino, H., Pellier, D., & Alford, R. (2020a). HDDL: An extension to PDDL for expressing hierarchical planning problems. *AAAI 2020*. https://staff.fnwi.uva.nl/g.behnke/papers/Hoeller2020HDDL.pdf
- Höller, D., Bercher, P., Behnke, G., & Biundo, S. (2020b). HTN planning as heuristic progression search. *JAIR*, 67, 835–880. https://jair.org/index.php/jair/article/view/11282
- Keller, T., & Helmert, M. (2013). Trial-based heuristic tree search for finite horizon MDPs. *ICAPS 2013*.
- Kocsis, L., & Szepesvári, C. (2006). Bandit based Monte-Carlo planning. *ECML 2006*.
- Lahiri, S. K., Nieuwenhuis, R., & Oliveras, A. (2006). SMT techniques for fast predicate abstraction. *CAV 2006*.
- McDermott, D., et al. (1998). PDDL – The Planning Domain Definition Language. Tech. rep. CVC TR-98-003, Yale.
- Nau, D., Au, T.-C., Ilghami, O., Kuter, U., Murdock, J. W., Wu, D., & Yaman, F. (2003). SHOP2: An HTN planning system. *JAIR*, 20, 379–404. https://arxiv.org/abs/1106.4869
- Patra, S., Mason, J., Ghallab, M., Nau, D., & Traverso, P. (2021). Deliberative acting, planning and learning with hierarchical operational models. *Artificial Intelligence*, 299. https://arxiv.org/abs/2010.01909
- Patra, S., Mason, J., Kumar, A., Ghallab, M., Traverso, P., & Nau, D. (2020). Integrating acting, planning, and learning in hierarchical operational models. *ICAPS 2020*. https://ojs.aaai.org/index.php/ICAPS/article/view/6743
- Pednault, E. P. D. (1989). ADL: Exploring the middle ground between STRIPS and the situation calculus. *KR 1989*.
- Pellier, D., Fiorino, H., Grand, M., Albore, A., & Bailon-Ruiz, R. (2023). HDDL 2.1: Towards defining a formalism and a semantics for temporal HTN planning. arXiv:2306.07353. https://arxiv.org/abs/2306.07353
- Reiter, R. (1978). On closed world data bases. In *Logic and Data Bases*, 55–76.
Related but not cited above: Shao, T., Zhang, H., Cheng, K., Zhang, K., & Bie, L. (2021). The hierarchical task network planning method based on Monte Carlo tree search. *Knowledge-Based Systems*, 107067.
- Schadd, M. P. D., Winands, M. H. M., van den Herik, H. J., Chaslot, G. M. J.-B., & Uiterwijk, J. W. H. M. (2008). Single-player Monte-Carlo tree search. *CG 2008*.
- Sohrabi, S., Baier, J. A., & McIlraith, S. A. (2009). HTN planning with preferences. *IJCAI 2009*.
- Wichlacz, J., Höller, D., Torralba, Á., & Hoffmann, J. (2020). Applying Monte-Carlo tree search in HTN planning. *SoCS 2020*. https://ojs.aaai.org/index.php/SOCS/article/view/18538 (code: https://github.com/minecraft-saar/MCTS-JSHOP)
- Winands, M. H. M., Björnsson, Y., & Saito, J.-T. (2008). Monte-Carlo tree search solver. *CG 2008*.
