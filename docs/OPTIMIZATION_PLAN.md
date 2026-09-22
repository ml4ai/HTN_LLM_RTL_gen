# Optimization plan

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

## 1. Evidence

### 1.1 Profile (macOS `sample`, transport, self-time)

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

### 1.2 Instrumented counts

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

### 1.3 The same query, two engines

"Enumerate bindings of `?l1` such that `(at package_0 ?l1)`", against the
transport initial state — 42 ground facts, the `at` relation holding 3 tuples:

| engine | per query |
|---|---|
| Z3, as the planner does it today | **3.27 ms** |
| a direct scan of the indexed relation | **0.000012 ms** |

That ratio is ~260,000×, but it is the ratio for *one step in isolation*. The
honest end-to-end bound is Amdahl's: Z3 is 96% of runtime, so removing it
entirely caps the whole-planner speedup at roughly **25×**.

### 1.4 Two things that measurement ruled out

* **Memory is not a problem.** Peak RSS 35 MB for transport at `-T 30000`.
  `pNode` is 336 bytes shallow, `KnowledgeBase` 128; the whole transport state
  is 42 ground facts, about 1.3 KB of strings. There is real redundancy (§4)
  but it is not currently costing anything worth chasing.
* **The build is unoptimized and it does not matter — yet.** A plain
  `cmake ..`, exactly as the README instructs, compiles with
  `-std=gnu++20 -arch arm64` and **no `-O` flag**, because the project never
  sets `CMAKE_BUILD_TYPE`. Measured cost today: *none* — d18 rollouts take
  150 ms either way, because the work is inside a prebuilt `libz3`. Fix it, but
  expect the payoff only after step 3 moves work back into our own code.

---

## 2. Was Z3 the right call?

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

## 3. Plan

Ordered so that each step is independently verifiable and the risky one comes
after the safety net is in place.

### Step 1 — Benchmark harness *(prerequisite, small)*

A script that runs every shipped domain/problem pair at a fixed seed and budget
and reports wall time, mean rollout time, plan length and final-state facts.
Nothing after this is verifiable without it. Every later step is judged on: no
change in plans, measurable change in time.

*This session's numbers came from throwaway probes in the scratch directory;
they should be a committed script.*

### Step 2 — Delete dead weight *(free, zero risk)*

`boost::json` is entirely unused: six `tag_invoke` overloads in `typedefs.h`
serialize `Grounded_Task`, `TaskGraph`, `TaskNode` and `TaskTree`, and nothing
calls `value_from` or `value_to` anywhere. They served the plan-recognizer JSON
output, removed earlier. Deleting them drops a Boost component from three
`CMakeLists.txt` files.

Also set `CMAKE_BUILD_TYPE` to `Release` when the caller does not specify
(§1.4) — no measurable gain today, but it stops being free to ignore after
step 3.

### Step 3 — Replace Z3 on the hot path *(the whole game)*

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

### Step 4 — Intern symbols *(follows naturally from 3)*

Ground facts are stored as strings like `"(at package_0 city_loc_1)"` and
**re-parsed constantly** — `parse_predicate` splits them on every `tell` and on
every fact in every `update_state`. Intern predicate and object names to
integers once at load, and represent a fact as a small integer tuple. This
removes the string churn (the 9.2% in `memcpy`/`strcmp`), shrinks the state,
and makes relation indexing natural. Largely pointless before step 3, since Z3
wants text anyway.

### Step 5 — Stop copying what does not change *(memory + some time)*

Every `KnowledgeBase` — and there is one per search node — carries its own copy
of the predicate schema and the object map, which are fixed for the whole
problem, plus a `smt_state` string of 3–5 KB. Share the immutable parts through
a pointer to one domain-level context; `smt_state` disappears with step 3c.

### Step 6 — Kill the deep copies in the hot path

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

### Step 7 — Revisit the domains

Independent of all the above and possibly the largest single lever, because it
attacks the size of the search space rather than the cost per node. See
`TODO.md` — `transport_domain.hddl` contains the recursive `goto` pattern that
Godet et al. show causes an exponential blow-up of redundant decompositions.

---

## 4. What to expect

| step | expected | confidence |
|---|---|---|
| 2 — dead weight | 0× (hygiene) | certain |
| 3 — replace Z3 | up to ~25× | high; bounded by measurement, not estimate |
| 4 — intern symbols | small on its own, enables 3 and 5 | medium |
| 5 — share immutable state | memory, little time | medium |
| 6 — deep copies | only visible after 3 | medium |
| 7 — domain re-encoding | potentially exponential | unknown until tried |

The single number worth keeping in mind: **2% of the time Z3 is given is spent
solving**. Everything else it does here is setup for a question that did not
need it.
