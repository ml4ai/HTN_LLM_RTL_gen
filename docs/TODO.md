# To do

Things deliberately deferred, with enough context to pick up cold. Ordered by
nothing in particular — see `OPTIMIZATION_PLAN.md` for the work that is
sequenced.

---

## Re-encode the recursive `goto` pattern out of the domains

**Paper.** Godet, R., Bit-Monnot, A., & Lesire-Cabaniols, C. (2024). *Redundant
Decompositions in PO HTN Domains: Goto Considered Harmful.* 7th ICAPS Workshop
on Hierarchical Planning (HPlan 2024), 36–44.
[proceedings](https://icaps24.icaps-conference.org/program/workshops/hplan/HPlanProceedings-2024.pdf)

**What it actually is.** Not a planner algorithm and not about method
preconditions — it is a **domain modelling** result. It identifies a pattern
that is extremely common in partially ordered HTN domains and makes the number
of possible decompositions explode: a recursive "get me to location X" compound
task, where several such tasks sit unordered with respect to each other in the
same network. Each one can contribute the same movement actions, so the same
plan is reachable through exponentially many decompositions.

**`transport_domain.hddl` has exactly this pattern.** `get_to` is the paper's
`goto`, with the same three methods — one hop (`m_direct`), recurse
(`m_via`), already-there (`m_noop`) — and `m_deliver_ordering_0` puts *two*
`get_to` subtasks in one network, unordered relative to each other. So this is
not hypothetical here; it is the default domain.

**The two models they propose**, both expressed in ordinary HTN with no planner
support required:

* **mutex** — constrain the decompositions so the redundant interleavings
  cannot arise.
* **(partial) task insertion** — the one to prefer. It mimics FAPE's
  distinction (Bit-Monnot et al. 2020) between *task-dependent* actions, which
  may only be introduced by decomposition, and *task-independent* ones, which
  may also be inserted freely. The movement actions become insertable; the
  `goto` task degrades to a condition that `at(t, p)` holds. They encode this
  without any task-insertion machinery by adding a recursive `free-move`
  compound task alongside a method that ends the recursion.

They report that for native PO HTN planners the task-insertion model "clearly
dominates the others". They also find large differences between planners for
the same encoding, so the right model here is an empirical question, not a
given.

**Why it is worth doing.** It is a change to `domains/*.hddl` only — no planner
code — and it attacks the search space multiplicatively, which is a different
lever from anything in `OPTIMIZATION_PLAN.md`. It is also the pattern most
likely to recur in new RTL-generation domains, so it is worth understanding
before writing many of them.

**Note on attribution.** This is sometimes mis-cited to Alford, Bercher & Aha.
That is a different paper — *Tight bounds for HTN planning with task insertion*
(IJCAI 2015) — which Godet et al. cite as the source of the task-insertion
variant they are mimicking. Alford and Bercher organise HPlan, which is
probably where the confusion comes from.

---

## Algorithm 3 (systematic progression)

`expansion` implements Höller et al.'s Algorithm 2 (see
`PLANNER_PROVENANCE.md` §2.3.2). Algorithm 3 would make the search fully
systematic. §5 of that document records the algorithm, what it would take here,
and the reason to be cautious: it postpones the state update, which is what the
rollouts and score functions read. Needs measuring, not assuming.

---

## Method preconditions under interleaving

No resolution exists in the literature (`PLANNER_PROVENANCE.md` §5.5). The
operative semantics is existential — a method precondition held at *some* point
before the method's subtasks ran, not necessarily when they run. If a domain
needs the stronger guarantee, put the condition on the action that depends on
it. Worth revisiting if HDDL 2.1's `at start` / `at end` / `overall`
qualifiers are ever standardised.
