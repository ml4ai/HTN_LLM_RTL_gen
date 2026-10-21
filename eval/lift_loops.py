#!/usr/bin/env python3
"""Recursive methods from checked step lists: the lifter for families whose
lists grow with the problem (LLM_RTL_code_generation.md 6.2.6, 8.6.15).

lift.py turns lists into methods when every list of a case has the same
steps. A state machine's list does not: it has a block of steps for every
state, and one for every port. This file finds those blocks and makes each
run of them a recursive task.

    python eval/lift_loops.py table_fsm --from hand     # the hand-written methods' own lists
    python eval/lift_loops.py table_fsm --from runs     # the model's passing lists (eval/runs/steps)

How it works, on the primitive plan each list comes to (the planner expands
the list's tasks, so lists that reach the same actions by different tasks
agree here):

 1. Chains. The problem's facts order some objects: (first_listed m A),
    (listed_after A B), ..., (last_listed m E) order the states, and
    first_port, next_port and last_port the ports. Any binary relation that
    forms one path, with a fact that names its first element and one that
    names its last, is a chain.
 2. Loops. A run of steps that can be grouped by consecutive elements of a
    chain, each group the steps that name one element at a fixed argument,
    is a loop. Groups may come in any order, since the lifted loop walks the
    chain in its own order, and an element may have no group. A group may
    hold a loop of its own, over another chain: for each output port, for
    each state.
 3. Variants. Groups that do the same thing are one variant: a port that is
    an input and one that is an output, a state an output is asserted in
    and one it is not. Each variant is told apart from the others by facts
    about the element, of the problem or recorded by earlier steps, which
    hold before all its groups and before none of the others'. The loop is
    simulated in the chain's own order to find what holds before each group.
    Learned from a few lists, several facts may tell variants apart, and most
    by chance ("it is the first state"). So the facts are tried in an order:
    what the group's own actions need, present or absent; for a group of no
    steps, the element's kind; the absence of what the other variants'
    actions need; then the rest, with where an element stands in a chain
    last. A fact may name "some" object, an exists: (not (exists (?p - port)
    (asserted_in ?p ?e))), no output is 1 in this state.
 4. Methods. A loop becomes a task with two methods for each variant: one
    for an element that has a next, which does the group and recurses on
    the next, and one for the last. The top-level method is the list with
    each loop replaced by a call on the chain's first element.

Before lifting, a passing list is cleaned of any block it opens and closes
with nothing in it, if what is left still passes: the model's state-table
lists open a clocked block before reading the table and close it at once.

The methods are learned from one list and then tested as lift.py tests
its methods: on every training problem, the other families' plans, and the
held-out problems. Every distinct passing list is tried in turn, longest first, and
the first whose methods pass every training problem is kept. A variant that
the list does not show is not learned.
"""

import argparse
import collections
import glob
import itertools
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import learn_family as lf  # noqa: E402
import learn_ground as lg  # noqa: E402
import learn_steps as ls  # noqa: E402
import lift  # noqa: E402
import plan_to_verilog  # noqa: E402
import problem_compiler  # noqa: E402
import rtl_eval  # noqa: E402

CONSTANTS = lift.CONSTANTS


class CannotLift(Exception):
    pass


# ---------------------------------------------------------------------------
# An example: a checked list, as the primitive plan it comes to

class Example:
    """A checked list, step by step: each step is an action or one of the
    domain's tasks, kept whole, so that what the domain's tasks already do
    (such as choosing a synchronous or an asynchronous register) stays theirs.
    Each step is a (name, args, index) triple; `prims[index]` is the actions
    it comes to."""

    def __init__(self, family, case, steps, workdir):
        self.problem = ls.Problem(family, case, workdir)
        self.case, self.workdir = case, workdir
        self.plan = self.problem.plan(steps)
        if self.plan is None:
            raise CannotLift(f"{case['tag']}: the list does not plan")
        steps = self.in_order(steps)
        self.steps = [(name, list(args), i) for i, (name, args) in enumerate(steps)]
        self.prims = []
        for i in range(len(steps)):
            before, after = self.problem.plan(steps[:i]), self.problem.plan(steps[:i + 1])
            self.prims.append(after[len(before):])
        self.facts = {tuple(f) for f in self.problem.facts}
        self.types = dict(lift.CONSTANT_TYPES)
        self.types.update(problem_types(self.problem.compiled))
        self.module = next(f[1] for f in self.facts if f[0] == "first_port")
        self.actions = self.problem.actions

    def in_order(self, steps):
        """The list with each run of decisions put in the order of the
        problem's chains. A decision writes no code, so a run of them can be
        reordered without changing the module; the model's lists read a
        state table in no fixed order, which would break a loop into pieces.
        The reordered list is kept only if it comes to the same module and
        the same facts."""
        facts = {tuple(f) for f in self.problem.facts}
        module = next(f[1] for f in facts if f[0] == "first_port")
        rank = {}
        for pred in sorted({f[0] for f in facts if len(f) == 3}):
            edges = {(f[1], f[2]) for f in facts if f[0] == pred and len(f) == 3 and module not in f[1:]}
            if not edges:
                continue
            nxt = dict(edges)
            heads = set(nxt) - {b for _, b in edges}
            if len(heads) == 1 and len(nxt) == len(edges):
                order = [heads.pop()]
                while order[-1] in nxt and len(order) <= len(edges):
                    order.append(nxt[order[-1]])
                for k, x in enumerate(order):
                    rank.setdefault(x, k)
        out, i = [], 0
        while i < len(steps):
            j = i
            while j < len(steps) and steps[j][0].startswith("analyze_") and not self.problem.known[steps[j][0]][0] == "task":
                j += 1
            run = steps[i:j] or [steps[i]]
            # By the chain the decision is about, then by its arguments, so that
            # every state's two transitions come in the same order.
            key = lambda st: (next((rank[a] for a in st[1] if a in rank), -1), st[0], tuple(st[1]))
            out += sorted(run, key=key) if j > i else run
            i = max(j, i + 1)
        before = self.problem.plan(steps)
        after = self.problem.plan(out)
        if after is None or self.problem.code(after) != self.problem.code(before) \
                or self.problem.state(after) != self.problem.state(before):
            return steps
        self.plan = after
        return out

    def needs(self, state, steps):
        """What the actions of these steps need, from `state` on: the plain
        facts their preconditions need present, and those they need absent,
        each as far as it holds in `state` (one an earlier action of the
        steps records is not a need of the block)."""
        present, absent = set(), set()
        for step in steps:
            for name, args in self.prims[step[2]]:
                params, pre, _ = self.actions[name]
                bind = dict(zip(params, args))
                for part in pre[1:] if pre[:1] == ["and"] else ([pre] if pre else []):
                    if part and all(isinstance(x, str) for x in part):
                        fact = tuple(bind.get(x, x) for x in part)
                        if fact in state:
                            present.add(fact)
                    elif len(part) == 2 and part[0] == "not" and all(isinstance(x, str) for x in part[1]):
                        fact = tuple(bind.get(x, x) for x in part[1])
                        if fact not in state:
                            absent.add(fact)
        return present, absent

    def after(self, state, steps):
        """The state after these steps, from `state`."""
        out = set(state)
        for step in steps:
            for name, args in self.prims[step[2]]:
                params, _, effects = self.actions[name]
                bind = dict(zip(params, args))
                out |= {tuple(bind.get(x, x) for x in e) for e in effects}
        return out


def chain_ranks(facts):
    """Each object's place in the chain the problem's facts put it in (the
    module itself left out): A 0, B 1, ... for the states."""
    module = next(f[1] for f in facts if f[0] == "first_port")
    rank = {}
    for pred in sorted({f[0] for f in facts if len(f) == 3}):
        edges = {(f[1], f[2]) for f in facts if f[0] == pred and len(f) == 3 and module not in f[1:]}
        if not edges:
            continue
        nxt = dict(edges)
        heads = set(nxt) - {b for _, b in edges}
        if len(heads) == 1 and len(nxt) == len(edges):
            order = [heads.pop()]
            while order[-1] in nxt and len(order) <= len(edges):
                order.append(nxt[order[-1]])
            for k, x in enumerate(order):
                rank.setdefault(x, k)
    return rank


def normalize(family, case, steps, workdir, tools):
    """The list with steps that commute put in the chains' order, as long
    as what results still passes. Made to declare before it opens a block,
    the model declares states one at a time, between recording transitions
    and writing output terms, in an order of its own; a lone declaration is
    tied to nothing the lifter knows (LLM_RTL_code_generation.md 8.6.18).

      1. A task that comes only to declarations becomes those declarations:
         declaring the states "from D" declares D, then E. Other tasks are
         kept whole, as the lifter keeps them.
      2. Decisions and declarations (plan_to_verilog.DECLARES) are sorted
         by the chain element they are about, decisions first. A
         declaration may also move ahead of any other step.
         Each swap must leave the list planning to the same lines of code,
         in any order.
      3. The result must pass as a list must (learn_steps.finish).

    Returns the list and how many steps moved."""
    problem = ls.Problem(family, case, workdir)
    rank = chain_ranks({tuple(f) for f in problem.facts})
    out = [(n, list(a)) for n, a in steps]
    reference = sorted(problem.code(problem.plan(out)) or [])
    k = 0
    while k < len(out):
        name = out[k][0]
        if problem.known[name][0] == "task":
            added = problem.plan(out[:k + 1])[len(problem.plan(out[:k])):]
            if added and all(n in plan_to_verilog.DECLARES for n, _ in added):
                trial = out[:k] + [(n, list(a)) for n, a in added] + out[k + 1:]
                if problem.plan(trial) == problem.plan(out):
                    out = trial
        k += 1

    def decision(st):
        return st[0].startswith("analyze_") and problem.known[st[0]][0] != "task"

    def declaration(st):
        return st[0] in plan_to_verilog.DECLARES

    def key(st):
        return (next((rank[a] for a in st[1] if a in rank), -1), not decision(st), st[0], tuple(st[1]))

    def holds(trial):
        plan = problem.plan(trial)
        lines = problem.code(plan) if plan is not None else None
        return lines is not None and sorted(lines) == reference

    moved, changed = 0, True
    while changed:
        changed = False
        for i in range(1, len(out)):
            x, y = out[i - 1], out[i]
            movable = decision(y) or declaration(y)
            if not movable:
                continue
            if (decision(x) or declaration(x)) and key(y) < key(x) or \
                    declaration(y) and not (decision(x) or declaration(x)):
                trial = out[:i - 1] + [y, x] + out[i + 1:]
                if holds(trial):
                    out, changed, moved = trial, True, moved + 1
    if moved and ls.finish(problem, out, tools)["stage"] != "pass":
        return [(n, list(a)) for n, a in steps], 0
    return out, moved


def clean(family, case, steps, workdir, tools):
    """The list less any block it opens and closes with nothing in it, as
    long as what is left still passes, as learn_steps judges a list: it
    plans, the same under two seeds, and simulates correctly. The model's
    state-table lists open a clocked block before reading the table and
    close it at once; lifted, every plan would tell the executor to write
    that block. Returns the cleaned list and the steps taken out."""
    problem = ls.Problem(family, case, workdir)
    steps, removed = list(steps), []
    while True:
        plan = problem.plan(steps)
        lines = problem.code(plan)
        # Which step writes each line: the lines a prefix of the list comes to.
        upto = [len(problem.code(problem.plan(steps[:k + 1])) or []) for k in range(len(steps))]
        writer = lambda n: next(k for k, u in enumerate(upto) if u > n)
        for begin, end in ls.empty_blocks(lines):
            a, b = writer(begin), writer(end)
            # Only steps that write nothing but the block's two lines.
            if upto[a] - (upto[a - 1] if a else 0) != 1 or upto[b] - upto[b - 1] != 1:
                continue
            trial = steps[:a] + steps[a + 1:b] + steps[b + 1:]
            if problem.plan(trial) is None:
                continue
            if ls.finish(problem, trial, tools)["stage"] == "pass":
                removed += [steps[a], steps[b]]
                steps = trial
                break
        else:
            return steps, removed


def problem_types(compiled):
    types = {}
    for line in re.search(r"\(:objects(.*?)\n  \)", compiled, re.S).group(1).splitlines():
        if " - " in line:
            names, t = line.rsplit(" - ", 1)
            types.update({n: t.strip() for n in names.split()})
    return types


# ---------------------------------------------------------------------------
# Chains

Chain = collections.namedtuple("Chain", "first next last order")


def chains(ex, others=()):
    """Every chain the problem's facts define. Its first and last facts are
    checked against `others`, the facts of other problems: a fact that
    names the first element here but not in another problem with the same
    chain, as "the clock" happens to name the first port of every generated
    problem, does not mark the chain's start."""
    out = []
    binary = collections.defaultdict(set)
    for f in ex.facts:
        if len(f) == 3 and f[1] != ex.module and ex.types.get(f[1]) == ex.types.get(f[2]):
            binary[f[0]].add((f[1], f[2]))
    for pred, edges in binary.items():
        nxt = dict(edges)
        if len(nxt) != len(edges) or len({b for _, b in edges}) != len(edges):
            continue
        heads = set(nxt) - {b for _, b in edges}
        if len(heads) != 1:
            continue
        order = [heads.pop()]
        while order[-1] in nxt and len(order) <= len(edges):
            order.append(nxt[order[-1]])
        if len(order) != len(edges) + 1:
            continue
        first = [f[0] for f in ex.facts if len(f) == 3 and f[1] == ex.module and f[2] == order[0]
                 and not any(g[0] == f[0] and g[2] != order[0] for g in ex.facts if len(g) == 3)]
        last = [f[0] for f in ex.facts if len(f) == 3 and f[1] == ex.module and f[2] == order[-1]
                and not any(g[0] == f[0] and g[2] != order[-1] for g in ex.facts if len(g) == 3)]
        first = [p for p in first if all(marks(f, pred, p, 0) for f in others)]
        last = [p for p in last if all(marks(f, pred, p, -1) for f in others)]
        if first and last:
            out.append(Chain(sorted(first)[0], pred, sorted(last)[0], order))
    return out


def marks(facts, pred, mark, end):
    """In this problem's facts, does `mark` name the end of the `pred`
    chain? True, too, if the problem has no such chain."""
    edges = {(f[1], f[2]) for f in facts if f[0] == pred and len(f) == 3}
    if not edges:
        return True
    heads = {a for a, _ in edges} - {b for _, b in edges}
    tails = {b for _, b in edges} - {a for a, _ in edges}
    named = {f[2] for f in facts if f[0] == mark and len(f) == 3}
    return named == (heads if end == 0 else tails)


# ---------------------------------------------------------------------------
# Folding a plan into steps and loops

class Loop:
    def __init__(self, chain, position, groups):
        self.chain = chain              # a Chain
        self.position = position        # {step name: the argument that names the element}
        self.groups = groups            # [(element, [items])], in the plan's order
        self.name = None


def chain_position(step, chain, position):
    name, args = step[0], step[1]
    if name in position:
        p = position[name]
        return p if p < len(args) and args[p] in chain.order else None
    for p, a in enumerate(args):
        if a in chain.order:
            return p
    return None


def grow(steps, i, chain):
    """The longest run from steps[i] that groups by elements of `chain`."""
    position, groups, seen = {}, [], set()
    k = i
    while k < len(steps):
        p = chain_position(steps[k], chain, position)
        if p is None:
            break
        elem = steps[k][1][p]
        if groups and elem == groups[-1][0]:
            groups[-1][1].append(steps[k])
        elif elem in seen:
            break
        else:
            seen.add(elem)
            groups.append((elem, [steps[k]]))
        position.setdefault(steps[k][0], p)
        k += 1
    return k, position, groups


def fold(steps, all_chains, forced=(), min_groups=2):
    """Steps as a list of items: ("step", step) or ("loop", Loop). A loop
    over a chain in `forced` is taken even with one group."""
    out, i = [], 0
    while i < len(steps):
        best = None
        for chain in all_chains:
            j, position, groups = grow(steps, i, chain)
            need = 1 if chain.next in forced else min_groups
            if len(groups) >= need and (best is None or j - i > best[0] - i):
                best = (j, chain, position, groups)
        if best is None:
            out.append(("step", steps[i]))
            i += 1
            continue
        j, chain, position, groups = best
        rest = [c for c in all_chains if c.next != chain.next]
        inner = [fold(g, rest) for _, g in groups]
        # A loop that one group's steps form, every group's steps form.
        inside = {it[1].chain.next for items in inner for it in items if it[0] == "loop"}
        inner = [fold(g, rest, forced=inside) for _, g in groups]
        out.append(("loop", Loop(chain, position, [(e, items) for (e, _), items in zip(groups, inner)])))
        i = j
    return out


def flatten(items):
    for kind, x in items:
        if kind == "step":
            yield x
        else:
            for _, sub in x.groups:
                yield from flatten(sub)


def shape(items):
    """What a block does, for telling variants apart: its step names, with
    each loop by its chain."""
    return tuple(x[0] if kind == "step" else ("loop", x.chain.next) for kind, x in items)


# ---------------------------------------------------------------------------
# Lifting: variables, bindings, preconditions

class Lifter:
    def __init__(self, ex, family):
        self.ex, self.family = ex, family
        self.forms, self.tasks, self.count = [], [], 0
        self.pred_types = lift.predicate_types(open(lf.DOMAIN).read())
        self.chain_preds = set()

    def typed(self, value):
        return self.ex.types[value]

    def lift_block(self, instances, context, keep_constants=False):
        """One method body from several instances of a block that do the
        same thing. `instances` is a list of (items, state before, values),
        where values maps each context variable to its value there.
        Returns (parameters, preconditions, subtasks)."""
        first_items = instances[0][0]
        places = []                     # (item index, argument index) for plain steps
        for n, (kind, x) in enumerate(first_items):
            if kind == "step":
                places += [(n, j) for j in range(len(x[1]))]
        cols = {}
        for n, j in places:
            cols[(n, j)] = tuple(inst[0][n][1][1][j] for inst in instances)
        variables = {v: tuple(inst[2][v] for inst in instances) for v in context}
        name_of = {col: v for v, col in variables.items()}
        tentative = set()
        for p, col in cols.items():
            if col not in name_of:
                v = f"?v{len(variables)}"
                variables[v], name_of[col] = col, v
                if len(set(col)) == 1 and col[0] in CONSTANTS:
                    tentative.add(v)
        if keep_constants:
            # In a loop, a constant that is the same in every group is the
            # loop's own: "on an input of zero". Lifting it would tie it to
            # whatever fact the groups happen to share.
            for p, col in cols.items():
                if len(set(col)) == 1 and col[0] in CONSTANTS and name_of.get(col) in tentative:
                    variables.pop(name_of[col], None)
                    name_of.pop(col)
            tentative = set()
        types = {v: self.typed(col[0]) for v, col in variables.items()}
        # Loops inside: each needs its chain's first element bound, and the
        # context it runs in.
        conds = self.tie(instances, variables, set(context), tentative, types)
        subtasks = []
        for n, (kind, x) in enumerate(first_items):
            if kind == "step":
                name, args = x[0], x[1]
                subtasks.append(f"({' '.join([name] + [name_of.get(cols[(n, j)], cols[(n, j)][0]) if name_of.get(cols[(n, j)]) in variables else cols[(n, j)][0] for j in range(len(args))])})")
            else:
                loops = [inst[0][n][1] for inst in instances]
                outer = [v for v in variables if v not in ("?m", "?n")]
                task, params = self.lift_loop(loops, [(inst[1], {v: variables[v][k] for v in variables})
                                                      for k, inst in enumerate(instances)], outer, types)
                start = f"?s{len(variables)}"
                variables[start] = tuple(loop.chain.order[0] for loop in loops)
                types[start] = self.typed(loops[0].chain.order[0])
                conds.append((loops[0].chain.first, "?m", start))
                subtasks.append(f"({' '.join([task, '?m'] + params + [start])})")
        return variables, types, conds, subtasks

    def tie(self, instances, variables, bound, tentative, types):
        """Precondition facts that tie every variable to the bound ones, as
        lift.lift_case does, against each instance's own state."""
        bound, conds = set(bound) | {"?m"}, []
        while bound != set(variables):
            best = None
            for fact in sorted(instances[0][1]):
                pred, args = fact[0], fact[1:]
                choices = [[v for v, col in variables.items() if col[0] == a] + ([a] if a in CONSTANTS else [])
                           for a in args]
                for reading in itertools.product(*choices):
                    new = [x for x in dict.fromkeys(reading) if x.startswith("?") and x not in bound]
                    if not new:
                        continue
                    if not all(lift.picks_out({"facts": inst[1]}, k, pred, reading, variables, bound)
                               for k, inst in enumerate(instances)):
                        continue
                    cand = (len(new), "?e" not in reading, "?m" not in reading,
                            sum(x.startswith("?") and x not in ("?e", "?m") for x in reading), pred, reading)
                    if best is None or cand < best:
                        best = cand
            if best is None and set(variables) - bound <= tentative:
                for v in set(variables) - bound:
                    variables.pop(v)
                    types.pop(v)
                break
            if best is None:
                left = sorted(set(variables) - bound)
                raise CannotLift(f"nothing ties {', '.join(left)} "
                                 f"({', '.join(str(variables[v][0]) for v in left)}) to what is known")
            pred, reading = best[-2:]
            conds.append((pred,) + tuple(reading))
            bound |= {x for x in reading if x.startswith("?")}
        return conds

    def lift_loop(self, loops, outers, outer_vars, outer_types):
        """A recursive task for these loops, one per instance of the block
        that holds them. Returns the task and its arguments after ?m."""
        self.count += 1
        task = f"{self.family}_walk{self.count}"
        chain = loops[0].chain
        elem_type = self.typed(chain.order[0])
        params = [f"?c{k}" for k in range(len(outer_vars))]
        rename = dict(zip(outer_vars, params))
        outers = [(state, {rename.get(v, v): x for v, x in values.items() if v == "?m" or v in rename})
                  for state, values in outers]
        outer_types = {rename[v]: t for v, t in outer_types.items() if v in rename}
        # Every element of every instance, in the chain's order, with the
        # group it has (empty if none) and the state before it.
        rows = []
        for loop, (state0, values) in zip(loops, outers):
            groups = {e: self.prune(items) for e, items in loop.groups}
            state = set(state0)
            for k, elem in enumerate(loop.chain.order):
                items = groups.get(elem, [])
                vals = dict(values, **{"?e": elem})
                if k + 1 < len(loop.chain.order):
                    vals["?n"] = loop.chain.order[k + 1]
                need = self.ex.needs(state, list(flatten(items)))
                rows.append({"items": items, "state": set(state), "values": vals, "need": need,
                             "last": k + 1 == len(loop.chain.order)})
                state = self.ex.after(state, list(flatten(items)))
        variants = collections.defaultdict(list)
        for r in rows:
            variants[shape(r["items"])].append(r)
        # What tells each variant from the others, in the state before it.
        tells = {s: self.tell(rs, [r for t, others in variants.items() if t != s for r in others], params)
                 for s, rs in variants.items()}
        param_text = " ".join(f"{v} - {outer_types[v]}" for v in params)
        self.tasks.append(f"(:task {task} :parameters (?m - module {param_text} ?e - {elem_type}))".replace("  ", " "))
        for s, rs in variants.items():
            # One body for the variant, from every element that has it, at either
            # end of the chain or between; the two methods differ only in going on.
            instances = [(r["items"], r["state"], r["values"]) for r in rs]
            ctx = ["?m"] + params + ["?e"]
            if not s:
                body = ({v: tuple(r["values"][v] for r in rs) for v in ctx}, {**outer_types, "?e": elem_type}, [], [])
            else:
                body = self.lift_block(instances, ctx, keep_constants=True)
            for last in (False, True):
                variables, types, conds, subtasks = (dict(body[0]), dict(body[1]), list(body[2]), list(body[3]))
                types.setdefault("?e", elem_type)
                types["?m"] = "module"
                types.update({v: outer_types[v] for v in params})
                if not last:
                    variables["?n"] = None
                    types["?n"] = elem_type
                    conds.append((chain.next, "?e", "?n"))
                    subtasks.append(f"({' '.join([task, '?m'] + params + ['?n'])})")
                else:
                    conds.append((chain.last, "?m", "?e"))
                pre = [f"({' '.join(c)})" for c in conds] + tells[s]
                ps = " ".join(f"{v} - {types[v]}" for v in ["?m"] + params + ["?e"] + [v for v in variables
                                                                                  if v not in ["?m", "?e"] + params])
                text = "\n".join(f"      (t{k + 1} {x})" for k, x in enumerate(subtasks))
                self.forms.append(
                    f"(:method m_{task}_{len(self.forms)}\n    :parameters ({ps})\n"
                    f"    :task ({' '.join([task, '?m'] + params + ['?e'])})\n"
                    f"    :precondition (and {' '.join(pre)})\n"
                    f"    :ordered-subtasks " + (f"(and\n{text}\n    )" if subtasks else "()") + "\n  )")
        return task, list(outer_vars)

    def prune(self, items):
        """A group's items less the steps that come to no actions. A task the
        hand-written plan cuts through at that level, write_moore_output_term
        on a state the output is 0 in, does nothing there, and the group is one
        of no steps; kept, it would look like the same task where it writes
        the output's first term, and nothing could tell the two apart."""
        return [(kind, x) for kind, x in items if kind == "loop" or self.ex.prims[x[2]]]

    def tell(self, mine, others, params):
        """Literals about ?e and the context that hold before every row of
        `mine` and before none of `others`'s, as few as will do."""
        if not others:
            return []
        known = ["?m"] + params + ["?e"]

        def literals(row):
            vals = {v: row["values"][v] for v in known}
            back = collections.defaultdict(list)
            for v, x in vals.items():
                back[x].append(v)
            out = set()
            context = {x for v, x in vals.items() if v not in ("?m", "?e")}
            for fact in row["state"]:
                # A fact about the element, or one about the context alone, such
                # as "this output has been assigned", which is what tells the
                # first state an output is 1 in from the later ones.
                if row["values"]["?e"] not in fact[1:] and not (set(fact[1:]) & context):
                    continue
                # Any argument may also be "some object": (asserted_in _ ?e),
                # some output is 1 in this state, read as an exists.
                readings = [back.get(a, []) + ([a] if a in CONSTANTS else []) + ["_"] for a in fact[1:]]
                for reading in itertools.product(*readings):
                    if any(x.startswith("?") and x != "?m" for x in reading):
                        out.add((fact[0],) + reading)
            return out

        def grounded(lit, row):
            return tuple(row["values"].get(x, x) for x in lit)

        def wanted(lit, rows, k):
            # Does every row's block need this fact (k = 0) or its absence
            # (k = 1)? A literal with "_" is needed if a fact it reads is.
            def match(fact, g):
                return len(fact) == len(g) and all(b == "_" or a == b for a, b in zip(fact, g))
            return all(any(match(f, grounded(lit, r)) for f in r["need"][k]) for r in rows)

        mine_l = [literals(r) for r in mine]
        common = set.intersection(*mine_l)

        def plainness(lit):
            # Where the element stands in a chain last of all: "it is the first
            # state" told the first state an output is 1 in from the others in
            # one list only because that list's outputs both began at the first
            # state. Then a fact about the element alone, then one about the
            # element and its context, then one about the context alone. Learned
            # from one list, a fact through another variable is likelier chance.
            ctx = sum(1 for x in lit[1:] if x.startswith("?") and x not in ("?e", "?m"))
            return (lit[0] in self.chain_preds, "?e" not in lit[1:], ctx, lit.count("_"), len(lit), lit)

        # What the block's own actions need comes first: the first state an
        # output is 1 in is told from the others by what its assignment needs,
        # (asserted_in ?p ?e) and no assignment yet, not by a fact that only
        # happens to hold there. A block of no steps is told by the absence of
        # what the others' actions need. Facts present and facts absent are
        # weighed together, so that one which only happens to hold does not
        # come before the absence an action needs.
        theirs = set.union(*[literals(r) for r in others])
        cands = [(lit, True) for lit in common] + [(lit, False) for lit in theirs - set.union(*mine_l)]

        # A block that does nothing is told by the element's kind when it has
        # one, (input_port ?e), not by the absence of what the others need: a
        # port no state asserts may be an output whose states were left out of
        # the problem, and skipping it would plan a module with that output
        # undriven, where the hand-written methods find no plan.
        empty = not mine[0]["items"]

        def key(cand):
            lit, present = cand
            if present and empty and len(lit) == 2 and lit[1] == "?e":
                rank = -1
            elif present:
                rank = 0 if wanted(lit, mine, 0) else 2
            else:
                ruled = [r for r in others if lit in literals(r)]
                rank = 0 if wanted(lit, mine, 1) else 1 if wanted(lit, ruled, 0) else 2
            return (rank, not present) + plainness(lit)

        chosen, left = [], list(others)
        for lit, present in sorted(cands, key=key):
            if present and any(lit not in literals(r) for r in left):     # it rules some of the others out
                chosen.append(self.render(lit))
                left = [r for r in left if lit in literals(r)]
            elif not present and any(lit in literals(r) for r in left):
                chosen.append(f"(not {self.render(lit)})")
                left = [r for r in left if lit not in literals(r)]
            if not left:
                return chosen
        raise CannotLift(f"nothing about the element tells a block of {shape(mine[0]['items']) or 'no steps'} "
                         f"(element {', '.join(r['values']['?e'] for r in mine)}) from one of "
                         f"{shape(left[0]['items']) or 'no steps'} (element {left[0]['values']['?e']})")

    def render(self, lit):
        """A literal as HDDL, each "_" an existentially bound variable."""
        pred, args = lit[0], list(lit[1:])
        bound = []
        for k, a in enumerate(args):
            if a == "_":
                v = f"?x{k}"
                args[k] = v
                bound.append(f"{v} - {self.pred_types[pred][k]}")
        atom = f"({' '.join([pred] + args)})"
        return f"(exists ({' '.join(bound)}) {atom})" if bound else atom

    def top(self, items):
        state = set(self.ex.facts)
        variables, types, conds, subtasks = self.lift_block([(items, state, {"?m": self.ex.module})], ["?m"])
        kinds = [f for f in sorted(self.ex.facts) if len(f) == 2 and f[1] == self.ex.module]
        pre = [f"({k[0]} ?m)" for k in kinds] + [f"({' '.join(c)})" for c in conds]
        ps = " ".join(f"{v} - {types[v]}" for v in variables)
        body = "\n".join(f"      (t{k + 1} {x})" for k, x in enumerate(subtasks))
        self.forms.insert(0, f"(:method m_implement_{self.family}_learned\n    :parameters ({ps})\n"
                             f"    :task (implement_module ?m)\n    :precondition (and {' '.join(pre)})\n"
                             f"    :ordered-subtasks (and\n{body}\n    )\n  )")


def lift_family(family, examples, ex):
    """HDDL forms for the family, from one example."""
    others = [e.facts for e in examples] + [{tuple(f) for f in problem_compiler.compile_text(r["problem"])[1]}
                                            for r in lf.slice_domain()["probes"]]
    found = chains(ex, others)
    items = fold(ex.steps, found)
    lifter = Lifter(ex, family)
    lifter.chain_preds = {p for c in found for p in (c.first, c.next, c.last)}
    lifter.top(items)
    return lifter.tasks + lifter.forms, items


def describe(items, depth=0):
    for kind, x in items:
        if kind == "step":
            print("  " * depth + "(" + " ".join([x[0]] + list(x[1])) + ")")
        else:
            elems = [e for e, _ in x.groups]
            print("  " * depth + f"loop over {x.chain.next} ({len(x.chain.order)} elements, {len(elems)} with steps)")
            for e, sub in x.groups[:2]:
                print("  " * depth + f"  [{e}]")
                describe(sub, depth + 2)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("family", choices=sorted(lf.FAMILIES))
    ap.add_argument("--from", dest="source", choices=["hand", "runs"], default="hand")
    ap.add_argument("--runs-dir", default=os.path.join(HERE, "runs", "steps"))
    ap.add_argument("--config", default=os.path.join(HERE, "eval_config.json"))
    ap.add_argument("--out", help="directory for the methods and the verdict")
    args = ap.parse_args()
    cfg, tools = rtl_eval.load_config(os.path.abspath(args.config))
    train, _ = lf.split(args.family)
    work = args.out or os.path.join(HERE, "runs", "lift_loops", args.family, args.source)
    os.makedirs(work, exist_ok=True)
    examples = []
    if args.source == "hand":
        for c in train:
            examples.append(Example(args.family, c, lg.frontier(args.family, c), os.path.join(work, c["tag"])))
    else:
        lists = lift.checked_lists(args.family, args.runs_dir)
        # Every distinct passing list, most frequent first: sampled warmer, a
        # problem has several, and the most frequent need not be the regular one.
        for c in train:
            seen = set()
            for k, (text, _) in enumerate(collections.Counter(lg.show(s) for s in lists.get(c["tag"], [])).most_common()):
                steps = lg.answer_steps("```\n" + text + "\n```")[0]
                where = os.path.join(work, c["tag"] if k == 0 else f"{c['tag']}_list{k}")
                steps, removed = clean(args.family, c, steps, where, tools)
                steps, moved = normalize(args.family, c, steps, where, tools)
                if moved:
                    print(f"{c['tag']} list {k}: {moved} steps moved into the chains' order", flush=True)
                if lg.show(steps) in seen:
                    continue
                seen.add(lg.show(steps))
                if removed:
                    print(f"{c['tag']} list {k}: taken out, an empty block: " + ", ".join(
                        "(" + " ".join([n] + list(a)) + ")" for n, a in removed))
                examples.append(Example(args.family, c, steps, where))
        print(f"{len({e.case['tag'] for e in examples})} of {len(train)} training problems have a checked list; "
              f"{len(examples)} distinct lists")
    # Methods are learned from one example. Each is tried, longest first, and
    # the first whose methods pass every training problem is kept: a list
    # whose structure is irregular (one output written apart from the other)
    # gives no loop to learn from, where another problem's list may.
    verdict, attempts = None, []
    for ex in sorted(examples, key=lambda e: -len(e.plan)):
        try:
            forms, items = lift_family(args.family, examples, ex)
        except CannotLift as e:
            attempts.append(f"{os.path.basename(ex.workdir)}: cannot lift: {e}")
            print(attempts[-1])
            continue
        verdict = lf.test_answer(args.family, "```hddl\n" + "\n\n".join(forms) + "\n```", work, tools)
        attempts.append(f"{os.path.basename(ex.workdir)}: {verdict['stage']}")
        print(attempts[-1])
        with open(os.path.join(work, f"attempt_{os.path.basename(ex.workdir)}.txt"), "w") as f:
            f.write("\n\n".join(forms) + "\n\n" + verdict["stage"] + "\n" + (verdict.get("diagnostic") or ""))
        if verdict["stage"] == "pass":
            break
    if verdict is None:
        sys.exit("cannot lift from any example")
    verdict["attempts"] = attempts
    print(f"\nlifted from {ex.case['tag']} ({len(ex.steps)} steps, {len(ex.plan)} actions):")
    describe(items)
    print()
    print("\n\n".join(forms))
    print(f"\ntraining problems and the other families: {verdict['stage']}")
    if verdict["stage"] != "pass":
        print(verdict["diagnostic"][:2000])
    else:
        v = lf.validate(args.family, os.path.join(work, "domain.hddl"), tools)
        verdict["validation"] = v
        print(f"held-out problems: {v['passed']} of {v['problems']} pass; "
              f"{v['same_plan_as_hand_written']} get the hand-written methods' plan")
        if v["failed"]:
            print("failed, for example:", ", ".join(v["failed"][:5]), v["stages"])
    with open(os.path.join(work, "verdict.json"), "w") as f:
        json.dump(verdict, f, indent=1)


if __name__ == "__main__":
    main()
