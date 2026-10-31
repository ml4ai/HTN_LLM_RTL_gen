#!/usr/bin/env python3
"""Leave one design family out, and have the model build a problem's step list
one step at a time, choosing among the steps that can be taken.

The third way of asking, after learn_family.py (write the methods: nothing
loads) and learn_ground.py (write the whole step list: close, never right,
and not repaired; LLM_RTL_code_generation.md 8.6.12, 8.6.13). Here the model
never writes a step. At each turn the planner works out every step that can
be taken next, the templates work out what each would add to the module, and
the model is shown the module so far and that menu, and answers with a number.

    python eval/learn_steps.py show wrap_counter       # the first turn's prompt
    python eval/learn_steps.py run wrap_counter        # 4 lists a training problem
    python eval/learn_steps.py run wrap_counter --policy random --runs 50   # no model
    python eval/learn_steps.py summary

What the model sees is the design, in a sentence and as the problem's facts;
the facts of the design no step has used yet; the decisions recorded so far;
the Verilog so far; and the menu. It sees no HDDL. A menu entry is a step with this problem's names, what it says (the
sentence of each action it comes to, rtl_domain_gloss.json), and the lines of
Verilog it adds. A step that records a decision adds no code, so it is shown
with what it is for: the code that comes onto the menu once it is taken, when
that is cheap to work out, and always the code steps that use what it records
(needed_for), worked out from preconditions alone.

What is on the menu is everything the planner can carry out at that point,
action or task, with every choice of the problem's names of the right types,
less six kinds of entry:

  - one that changes nothing: no line of code and no new fact;
  - one that does exactly what another does, and says the same (a task is
    kept over the action it comes to, so that a list reuses the domain's
    tasks);
  - an action with an argument that no fact of its precondition mentions,
    when there are more than MAX_FREE ways to fill it in: nothing ties that
    argument down, and each way is a guess;
  - the step that ends the module, while a fact of the design that a step
    could use has not been used: the module would leave part of the design
    out. This is a rule about covering the specification, not about the
    language. Every correct plan checked uses all such facts. Nor while a
    register the code reads has no driver (undriven);
  - one that opens a block after which no code could be written, only
    decisions: a dead end, found by looking one step ahead;
  - one whose code cannot stand where it would go: the templates cannot
    write it yet (it uses a port not yet declared), it would come before
    the module header, it would end a block that is not open, it would
    end the module with a block still open, it would put an `else`
    where no `if` precedes it, it would use a name before declaring it, it
    would drive a register from a second block, or it would close a case
    statement that has no item.

So a list is valid as it is built, in its names, its types and every
precondition. What the model supplies is the order and the choice: which of
the legal steps a module of this design needs next. The --policy random arm
measures how much that is, by choosing uniformly.

A list ends when a step writes `endmodule`. It is then simulated against the
Python model of the design, as in learn_family.py, and a list that passes is
written where lift.py looks for checked lists (--from runs --runs-dir
eval/runs/steps). There is no repair: a list is built once.

The training problems are the generated ones of learn_family.py. No benchmark
testbench is read.
"""

import argparse
import collections
import difflib
import glob
import hashlib
import itertools
import json
import math
import os
import random
import re
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import learn_family as lf  # noqa: E402
import learn_ground as lg  # noqa: E402
import plan_to_verilog  # noqa: E402
import problem_compiler  # noqa: E402
import rtl_eval  # noqa: E402

RUNS = os.path.join(HERE, "runs", "steps")
GLOSS = os.path.join(ROOT, "rtl_designs", "rtl_domain_gloss.json")
MAX_FREE = 2       # ways to fill in an action some of whose arguments nothing ties down
MAX_TIED = 40      # ways to fill in any other step
MAX_TURNS = 60
MAX_TURNS_PHASED = 80  # a move to the next phase takes a turn too
MAX_LOOKAHEAD = 4  # decisions on a menu for which "makes it possible to write" is worked out
SYSTEM = "You are a careful digital designer. You answer with one number and nothing else."


class Problem:
    """One training problem with the family's methods removed, and what the
    planner and the templates say about a list of steps for it."""

    def __init__(self, family, case, workdir):
        self.family, self.case = family, case
        self.domain_text = lf.reduced_domain(family)
        os.makedirs(workdir, exist_ok=True)
        self.domain_path = os.path.join(workdir, "domain.hddl")
        with open(self.domain_path, "w") as f:
            f.write(self.domain_text)
        self.compiled, self.facts, _ = problem_compiler.compile_text(case["problem"])
        self.known = lg.signatures(self.domain_text)
        self.names = lg.objects_of(self.compiled)
        self.actions = lg.action_forms(self.domain_text)
        with open(GLOSS) as f:
            gloss = json.load(f)
        self.sentences, self.legend = gloss["actions"], gloss["legend"]
        self._plans, self._menus = {(): []}, {}
        # The kinds of fact some step records.
        self.recorded = {e[0] for _, _, effects in self.actions.values() for e in effects}
        # Actions with a parameter that no plain fact of the precondition mentions.
        self.free = set()
        for name, (params, pre, _) in self.actions.items():
            parts = pre[1:] if pre[:1] == ["and"] else ([pre] if pre else [])
            tied = {x for part in parts if all(isinstance(x, str) for x in part) for x in part[1:]}
            if set(params) - tied:
                self.free.add(name)

    def plan(self, steps):
        """The primitive plan the steps come to, or None."""
        key = tuple((n, tuple(a)) for n, a in steps)
        if key not in self._plans:
            self._plans[key] = lf.plan(self.domain_path, lg.with_steps(self.compiled, list(steps)))[0]
        return self._plans[key]

    def state(self, plan):
        facts = {tuple(f) for f in self.facts}
        for name, args in plan:
            params, _, effects = self.actions[name]
            bind = dict(zip(params, args))
            facts |= {tuple(bind.get(x, x) for x in e) for e in effects}
        return facts

    def holds(self, state, name, args):
        """Could this action's precondition hold in `state`? Only its plain
        facts and their plain negations are judged; the planner settles the
        rest. (lg.unmet does the same from the text, once; this is called
        for every way of filling in every action.)"""
        params, pre, _ = self.actions[name]
        bind = dict(zip(params, args))
        for part in pre[1:] if pre[:1] == ["and"] else ([pre] if pre else []):
            if part and all(isinstance(x, str) for x in part):
                if tuple(bind.get(x, x) for x in part) not in state:
                    return False
            elif len(part) == 2 and part[0] == "not" and all(isinstance(x, str) for x in part[1]):
                if tuple(bind.get(x, x) for x in part[1]) in state:
                    return False
        return True

    def says(self, name, args):
        params = [p[1:] for p in self.actions[name][0]]
        return self.sentences[name].format(**dict(zip(params, args)))

    def code(self, plan):
        """The Verilog the plan comes to, as lines, or None if it cannot
        stand: the templates cannot write it out at that point (a step uses
        a port that is not declared yet), it puts code before the module
        header, its blocks could not be the start of a module
        (well_formed), or it uses a name before declaring it
        (declared_before_use)."""
        try:
            lines = plan_to_verilog.render(plan).strip("\n").splitlines()
        except (SystemExit, Exception):
            return None
        if lines and not lines[0].startswith("module "):
            return None
        return lines if well_formed(lines) and declared_before_use(lines) and one_driver(lines) \
            and no_repeat(lines) else None

    def menu(self, done, look_ahead=True):
        """What can be done after `done`: a list of entries, each with its
        step, the plan it leads to, what it says and the code it adds."""
        key = (tuple((n, tuple(a)) for n, a in done), look_ahead)
        if key in self._menus:
            return self._menus[key]
        before = self.plan(done)
        state, lines = self.state(before), self.code(before) or []
        by_type = collections.defaultdict(list)
        for n, t in sorted(self.names.items()):
            by_type[t].append(n)
        candidates = []
        for name, (kind, types) in sorted(self.known.items()):
            pools = [by_type[t] if t != "object" else sorted(self.names) for t in types]
            size = 1
            for pool in pools:
                size *= len(pool)
            if size == 0 or size > 4000:
                continue
            for args in itertools.product(*pools):
                if kind == "task" or self.holds(state, name, args):
                    candidates.append((name, list(args)))
        with ThreadPoolExecutor(8) as pool:
            plans = list(pool.map(lambda c: self.plan(list(done) + [c]), candidates))
        entries, seen = [], {}
        for (name, args), after in zip(candidates, plans):
            if after is None or after[:len(before)] != before:
                continue
            added = after[len(before):]
            new_lines = self.code(after)
            if new_lines is None:
                continue
            adds = [x[2:] for x in difflib.ndiff(lines, new_lines) if x.startswith("+ ")]
            new_facts = frozenset(self.state(after) - state)
            if not adds and not new_facts:
                continue                      # changes nothing
            entry = {"step": (name, args), "kind": self.known[name][0], "plan": after, "adds": adds,
                     "facts": sorted(new_facts),
                     "says": [self.says(n, a) for n, a in added], "closes": bool(new_lines) and
                     new_lines[-1].strip() == "endmodule"}
            # The same code, the same facts and the same sentences. Two steps that
            # come to the same facts by different reasoning are both kept: their
            # sentences say different things, and only one may fit the design.
            same = (tuple(adds), new_facts, tuple(entry["says"]))
            if same in seen:                  # does what another does: keep the task
                if entry["kind"] == "task" and seen[same]["kind"] != "task":
                    entries[entries.index(seen[same])] = entry
                    seen[same] = entry
                continue
            seen[same] = entry
            entries.append(entry)
        # The module is closed only when the whole design has been dealt with:
        # every fact of it that a step could use, some step has used.
        # And every decision the list recorded that code could use, code has:
        # a detector's list recorded when its output is 1 and closed the module
        # without the step that writes it (HRL_notes.md 5.1).
        # And every register the code reads has a driver (undriven).
        entries = [e for e in entries if not e["closes"] or not (self.unused_facts(list(done) + [e["step"]])
                                                                  or self.unused_decisions(list(done) + [e["step"]])
                                                                  or undriven(self.code(e["plan"])))]
        count = collections.Counter(e["step"][0] for e in entries)
        entries = [e for e in entries if count[e["step"][0]] <= (MAX_FREE if e["step"][0] in self.free else MAX_TIED)]
        # Declarations first: no always block or assign is begun while a declaration
        # could still use a fact of the design no step has used, now or once
        # the decisions it needs are recorded. Once the
        # block is open, the declaration cannot go inside it, and the code that
        # needs it, such as a state's case, could no longer be written: opening
        # the next-state case before every state was declared was a dead end
        # (LLM_RTL_code_generation.md 8.6.18). More than syntax: Verilog allows
        # a declaration after a block, only not inside one.
        if self.declaration_pending(done, state):
            # An `assign` too: later output terms are added to it, and each
            # names a state that must be declared before it (8.6.18).
            entries = [e for e in entries if not any(x.strip().startswith(("always", "assign")) for x in e["adds"])]
        if look_ahead:
            # A step that opens a block must leave something to put in it: an
            # opener after which nothing but the block's own closing can be
            # written is a dead end, or an empty block. Decisions do not count:
            # they write nothing, and cannot make a declaration possible inside
            # a block. A block closed empty is legal Verilog that does nothing;
            # the model's state-table lists opened one first thing and closed it
            # at once, or would not close it and stopped (8.6.17).
            # Only code that goes inside the block counts: lines after the
            # opener, not an output term added to an `assign` written earlier,
            # which once let a block be opened that could only close empty.
            entries = [e for e in entries if not re.search(r"\b(begin|case)\b", " ".join(e["adds"]))
                       or any(self.fills(e, n) for n in self.menu(list(done) + [e["step"]], look_ahead=False))]
        if look_ahead:
            # A decision that code could write now stays writable: no other
            # code is offered after which it no longer could be. A detector's
            # list wrote its output's "otherwise 0" branch before the branch
            # its own decision called for, and after a plain else there is no
            # more branch (HRL_notes.md 5.1). Decisions are never held back.
            pending = set(self.unused_decisions(list(done)))

            def writes(prefix, step):
                return set(self.unused_decisions(prefix)) - set(self.unused_decisions(prefix + [step]))
            writable = set().union(*[writes(list(done), e["step"]) for e in entries]) if pending else set()

            def keeps(e):
                if not writable or not e["adds"] or writes(list(done), e["step"]):
                    return True
                after = list(done) + [e["step"]]
                still = set().union(*[writes(after, n["step"]) for n in self.menu(after, look_ahead=False)])
                return writable <= still
            entries = [e for e in entries if keeps(e)]
        entries.sort(key=lambda e: " ".join([e["step"][0]] + e["step"][1]))
        self._menus[key] = entries
        return entries

    def fills(self, opener, entry):
        """Does `entry`, taken after `opener`, put code inside the block the
        opener began: new lines after the module so far, not only the
        block's own closing?"""
        before, after = self.code(opener["plan"]) or [], self.code(entry["plan"]) or []
        new = after[len(before):]
        return after[:len(before)] == before and any(not CLOSER.match(x.strip()) for x in new)

    def declaration_pending(self, done, state):
        """Could a declaration still use a fact of the design no step has used?
        A declaring action (plan_to_verilog.DECLARES) whose precondition names
        an unused fact, with its other plain facts true now or of a kind some
        step records later: a state's parameter needs the state's transitions
        recorded first, and is pending before they are. A declaration of a
        name already declared is not pending: it can never be written, and
        counting it would bar every block for good."""
        unused = set(self.unused_facts(list(done)))
        declared = declared_names(self.code(self.plan(list(done))) or [])
        for name in plan_to_verilog.DECLARES:
            if name not in self.actions:
                continue
            params, pre, _ = self.actions[name]
            atoms = [p for p in (pre[1:] if pre[:1] == ["and"] else ([pre] if pre else []))
                     if p and all(isinstance(x, str) for x in p)]
            for a in atoms:
                for fact in unused:
                    if fact[0] != a[0] or len(fact) != len(a):
                        continue
                    bind = {}
                    if all(bind.setdefault(x, v) == v if x.startswith("?") else x == v
                           for x, v in zip(a[1:], fact[1:])):
                        for full in self._extend(bind, [b for b in atoms if b is not a], state):
                            if not {full.get(x) for x in params} & declared:
                                return True
        return False

    def unused_facts(self, done):
        """Facts of the design that an action could use and none has: the
        parts of the specification no step has dealt with yet. Each action's
        plain precondition facts, with its arguments filled in, count as
        used. Facts of a kind only methods read, such as the order states
        are listed in, are left out: no step uses them directly."""
        usable = {a[0] for _, pre, _ in self.actions.values()
                  for a in (pre[1:] if pre[:1] == ["and"] else ([pre] if pre else []))
                  if a and all(isinstance(x, str) for x in a)}
        used = set()
        for name, args in self.plan(done):
            params, pre, _ = self.actions[name]
            bind = dict(zip(params, args))
            for a in (pre[1:] if pre[:1] == ["and"] else ([pre] if pre else [])):
                if a and all(isinstance(x, str) for x in a):
                    used.add(tuple(bind.get(x, x) for x in a))
        return [f for f in map(tuple, self.facts) if f[0] in usable and f not in used]

    def unused_decisions(self, done):
        """Facts the list's own decisions recorded that code could use and no
        code has: a decision made and not yet written, such as when the
        detector's output is 1. A decision is an analyze_* action; code is a
        write_* action, and it uses a fact its plain precondition names."""
        plan = self.plan(done) or []
        wanted = {a[0] for n, (_, pre, _) in self.actions.items() if n.startswith("write_")
                  for a in (pre[1:] if pre[:1] == ["and"] else ([pre] if pre else []))
                  if a and all(isinstance(x, str) for x in a)}
        recorded, used = [], set()
        for name, args in plan:
            params, pre, effects = self.actions[name]
            bind = dict(zip(params, args))
            if name.startswith("analyze_"):
                recorded += [tuple(bind.get(x, x) for x in e) for e in effects]
            elif name.startswith("write_"):
                used |= {tuple(bind.get(x, x) for x in a)
                         for a in (pre[1:] if pre[:1] == ["and"] else ([pre] if pre else []))
                         if a and all(isinstance(x, str) for x in a)}
        return [f for f in dict.fromkeys(recorded) if f[0] in wanted and f not in used]

    def needed_for(self, done, entry):
        """What code a decision is needed for: each way a write_* action can
        use one of the facts the decision records, with its other plain
        facts either true by then or ones some step records later. Worked out
        from the preconditions alone, so it costs no planning, and it shows
        what one decision of several is for: the case for a state needs both
        of its transitions, and recording one makes nothing possible yet.
        Arguments not known yet are shown as ?."""
        state = self.state(self.plan(done)) | set(entry["facts"])
        out = []
        for name, (params, pre, _) in sorted(self.actions.items()):
            if not name.startswith("write_"):
                continue
            parts = pre[1:] if pre[:1] == ["and"] else ([pre] if pre else [])
            atoms = [p for p in parts if p and all(isinstance(x, str) for x in p)]
            if set(params) - {x for a in atoms for x in a[1:]}:
                continue                          # an argument nothing ties down
            for fact in entry["facts"]:
                for a in atoms:
                    if a[0] != fact[0] or len(a) != len(fact):
                        continue
                    bind = {}
                    if all(bind.setdefault(x, v) == v if x.startswith("?") else x == v for x, v in zip(a[1:], fact[1:])):
                        for full in self._extend(bind, [b for b in atoms if b is not a], state):
                            out.append(self.says(name, [full.get(x, "?") for x in params]))
        return list(dict.fromkeys(out))

    def _extend(self, bind, atoms, state):
        """Bindings that match the atoms to facts in `state`, leaving an atom
        unmatched only if some step records facts of its kind."""
        if not atoms:
            yield bind
            return
        a, rest = atoms[0], atoms[1:]
        found = False
        for fact in state:
            if fact[0] != a[0] or len(fact) != len(a):
                continue
            b = dict(bind)
            if all(b.setdefault(x, v) == v if x.startswith("?") else x == v for x, v in zip(a[1:], fact[1:])):
                found = True
                yield from self._extend(b, rest, state)
        if not found and a[0] in self.recorded:
            yield from self._extend(bind, rest, state)

    def opens(self, done, entry):
        """The code that a decision makes possible: the first line of each
        entry that is on the menu after it and not before."""
        now = {(e["step"][0], tuple(e["step"][1])) for e in self.menu(done)}
        out = []
        for e in self.menu(list(done) + [entry["step"]]):
            if (e["step"][0], tuple(e["step"][1])) not in now and e["adds"]:
                out.append(e["adds"][0].strip())
        return list(dict.fromkeys(out))

    def facts_text(self):
        facts = re.search(r"\(:init(.*?)\n  \)", self.compiled, re.S).group(1)
        return "\n".join(x.strip() for x in facts.splitlines() if x.strip())

    def outline_prompt(self):
        """The first turn with --phases: the model plans the work as phases,
        in its own words, before any step is taken."""
        return f"""You are going to write a Verilog module, one step at a time. Before you start, plan the work.

THE DESIGN

{self.case['what']}

The same design as facts. {self.legend}

{self.facts_text()}

YOUR TASK

Write an outline of the work: a numbered list of phases, in the order they are to be done, one short line each. A phase is a part of the work, such as a group of design decisions to record, or one part of the module to write. Use between 3 and 8 phases.

The module will then be written one phase at a time. At each step you will be shown the phase you are on, the module so far, and the steps that can be taken, and you will choose among them. So order the phases as the work must go: a decision before the code that depends on it, and anything declared before the code that uses it.

Answer with the numbered list and nothing else."""

    def prompt(self, done, outline=None, phase=None):
        before = self.plan(done)
        lines = self.code(before) or []
        decided = [self.says(n, a) for n, a in before if n.startswith("analyze_")]
        facts = self.facts_text()
        items = []
        menu = self.menu(done)
        decisions = [e for e in menu if not e["adds"]]
        for i, e in enumerate(menu, 1):
            says = " ".join(e["says"][:4]) + (" ..." if len(e["says"]) > 4 else "")
            if e["adds"]:
                adds = ("\n" + "\n".join("       " + x for x in e["adds"][:14])
                        + ("\n       ..." if len(e["adds"]) > 14 else ""))
            else:
                adds = " (no code: it records a decision)"
                # What the decision is needed for, when working that out is affordable.
                opened = self.opens(done, e) if len(decisions) <= MAX_LOOKAHEAD else []
                if opened:
                    adds += ("\n   Makes it possible to write: " + "  |  ".join(f"`{x}`" for x in opened[:4])
                             + ("  | ..." if len(opened) > 4 else ""))
                needed = self.needed_for(done, e)
                if needed:
                    adds += ("\n   Needed for: " + " ".join(needed[:3])
                             + (f" (and {len(needed) - 3} more)" if len(needed) > 3 else ""))
            items.append(f"{i}. ({' '.join([e['step'][0]] + e['step'][1])})\n   Says: {says}\n   Adds:{adds}")
        so_far = "```verilog\n" + "\n".join(lines) + "\n```" if lines else "Nothing is written yet."
        unused = self.unused_facts(done)
        unwritten = self.unused_decisions(done)
        plan_text, choose = "", ""
        if outline:
            marks = [f"{k + 1}. {text}" + ("    <- the phase you are on" if k == phase else "")
                     for k, text in enumerate(outline)]
            plan_text = "\n\nYOUR OUTLINE OF THE WORK\n\n" + "\n".join(marks)
            if phase + 1 < len(outline):
                items.append(f"{len(menu) + 1}. Phase {phase + 1} is done: go on to phase {phase + 2}, "
                             f"\"{outline[phase + 1]}\".")
                choose = (f" You are on phase {phase + 1}, \"{outline[phase]}\". Choose the next step of this "
                          f"phase or, if the phase is done, entry {len(menu) + 1} to go on to the next.")
            else:
                choose = f" You are on the last phase, \"{outline[phase]}\"."
        return f"""You are writing a Verilog module one step at a time. At each turn you are shown the module so far and every step that can be taken next, and you choose one.

THE DESIGN

{self.case['what']}

The same design as facts. {self.legend}

{facts}

{plan_text.strip() + chr(10) + chr(10) if plan_text else ''}FACTS OF THE DESIGN NO STEP HAS USED YET

{chr(10).join('(' + ' '.join(f) + ')' for f in unused) if unused else 'None: every fact a step can use has been used.'}

DECISIONS RECORDED THAT NO CODE HAS USED YET

{chr(10).join('(' + ' '.join(f) + ')' for f in unwritten) if unwritten else 'None: every decision recorded so far has been written into the code.'}

DECISIONS RECORDED SO FAR

{chr(10).join('- ' + d for d in decided) if decided else 'None yet.'}

THE MODULE SO FAR

{so_far}

WHAT CAN BE DONE NEXT

{chr(10).join(items)}

Choose the one step that should come next, so that the finished module is complete, is correct, and does exactly what the design says. Some of the steps listed belong to other kinds of design: do not choose a step this design has no use for. A step that records a decision adds no code; code that depends on a decision is not offered until the decision is recorded, so if the code you need next is not listed, look for the decision that makes it possible. The module is finished by the step that adds `endmodule`, which is offered only once every fact of the design, and every decision recorded, has been used.{choose}

Answer with the number of the step and nothing else."""


MODULE_ITEM = re.compile(r"(module|input|output|inout|reg|wire|parameter|localparam|assign|always|initial)\b")
CASE_LABEL = re.compile(r"([\w']+|default)\s*:(?!=)\s*")
CLOSER = re.compile(r"(end|endcase)\b")


def well_formed(lines):
    """Could these lines be the start of a module, as far as its structure
    goes? This is syntax only, the part a compiler would refuse whatever came
    next. Whether the module does what the design says is for the model.

      - Blocks nest. An `end` closes a `begin` and an `endcase` a `case`, and
        `endmodule` comes with every block closed.
      - A module has one header.
      - Module items (declarations, `assign`, `always`) stand only at module
        level, and statements only inside a block.
      - Inside a `case`, a statement is a case item, `label: ...`; a case
        label stands only inside a `case`.
      - An `else` needs an `if`: it may come only straight after an if-branch
        at the same depth, and not after another plain else."""
    stack = []          # open blocks: (kind, may an else follow it once it is closed?)
    seen_module = False  # a module has one header
    may_else = [False]  # at each depth, may the next statement be an else?
    for line in lines:
        text = line.strip()
        while True:                                  # closers at the start of the line
            if text.startswith("endmodule"):
                if stack:
                    return False
                text = ""
            elif re.match(r"endcase\b", text):
                if not stack or stack[-1][0] != "case":
                    return False
                may_else.pop()
                may_else[-1] = stack.pop()[1]
                text = re.sub(r"^endcase\b\s*", "", text)
                continue
            elif re.match(r"end\b", text):
                if not stack or stack[-1][0] != "begin":
                    return False
                may_else.pop()
                may_else[-1] = stack.pop()[1]
                text = re.sub(r"^end\b\s*", "", text)
                continue
            break
        if not text:
            continue
        if MODULE_ITEM.match(text):
            if stack or (text.startswith("module") and seen_module):
                return False
            seen_module = seen_module or text.startswith("module")
        elif not stack:
            return False                             # a statement outside any block
        label = CASE_LABEL.match(text) if stack and not MODULE_ITEM.match(text) else None
        rest = text[label.end():] if label else text
        is_else = re.match(r"else\b", rest) is not None
        is_if = re.match(r"(else\s+)?if\b", rest) is not None
        if label and stack[-1][0] != "case":
            return False
        if stack and stack[-1][0] == "case" and not label and not (is_else and may_else[-1]):
            return False
        if is_else and not label and not may_else[-1]:
            return False
        openers = re.findall(r"\b(begin|case)\b", rest)
        if re.search(r"\b(end|endcase)\b", rest):
            return False                             # the templates never close a block mid-line
        if openers:
            may_else[-1] = False
            for k, kind in enumerate(openers):
                stack.append((kind, is_if and k == 0))   # closing an if-branch lets an else follow
                may_else.append(False)
        else:
            may_else[-1] = is_if                     # "if (c) x <= 1;" may be followed by an else
    return True


KEYWORDS = {"module", "endmodule", "input", "output", "inout", "reg", "wire", "parameter", "localparam", "assign",
            "always", "initial", "begin", "end", "if", "else", "case", "endcase", "default", "posedge", "negedge",
            "or", "and", "not", "integer", "signed"}


def one_driver(lines):
    """Is each register assigned in one block only, by an assignment the
    language allows, and does every case statement have an item and at
    most one default? A register driven from two always blocks is two
    drivers for one signal, which synthesis refuses; an always block may
    assign only what is declared a reg; a case with no item, or with two
    defaults, is a syntax error. Like the other rules, this is the
    language, not the design."""
    owner, block, items, defaults = {}, 0, None, 0
    regs, nets = set(), set()          # declared reg, and declared but not reg
    for line in lines:
        text = line.strip()
        decl = re.match(r"(input|output|inout|reg|wire)\b(.*)", text)
        if decl:
            names = [n for n in re.findall(r"[A-Za-z_]\w*", re.sub(r"\[[^\]]*\]", " ", decl.group(2)))
                     if n not in KEYWORDS]
            if decl.group(1) == "reg" or re.match(r"output\s+reg\b", text):
                regs.update(names)
                nets.difference_update(names)
            else:
                nets.update(n for n in names if n not in regs)
            continue
        if text.startswith("always") or text.startswith("assign"):
            block += 1
        if re.match(r"case\s*\(", text):
            items, defaults = 0, 0
        elif text.startswith("endcase"):
            if items == 0:
                return False
            items = None
        elif items is not None and CASE_LABEL.match(text):
            items += 1
            if re.match(r"default\s*:", text):
                defaults += 1
                if defaults > 1:
                    return False
        targets = re.findall(r"(\w+)\s*<?=(?!=)", re.sub(r"^\s*([\w']+|default)\s*:", "", text))
        if text.startswith("assign"):
            targets = re.findall(r"assign\s+(\w+)", text)
        for t in targets:
            if t in ("parameter", "localparam") or text.startswith(("parameter", "localparam")):
                continue
            if not text.startswith("assign") and t in nets:
                return False                   # a procedural assignment to a net
            if owner.setdefault(t, block) != block:
                return False
    return True


def undriven(lines):
    """The registers these lines read and never assign: names declared `reg`
    (or `output reg`) that appear on the right of an assignment, in a
    condition or in a `case (...)`, and are the target of no `=` or `<=`.
    An undriven register is legal Verilog and stays x: a detector's list
    read its state register in the next-state case and closed the module
    with no step that updates it, so the output could never assert
    (HRL_notes.md 5.1). More than syntax: a rule about a module being
    complete."""
    regs, read, assigned = set(), set(), set()
    for line in lines:
        text = re.sub(r"//.*", "", line).strip()
        text = re.sub(r"\d*'[bdho][0-9a-fA-FxXzZ_]+", " ", text)    # sized literals
        decl = re.match(r"(input|output|inout|reg|wire|parameter|localparam)\b(.*)", text)
        if decl:
            if decl.group(1) == "reg" or re.match(r"output\s+reg\b", text):
                body = re.sub(r"\[[^\]]*\]", " ", decl.group(2))
                regs.update(n for n in re.findall(r"[A-Za-z_]\w*", body) if n not in KEYWORDS)
            continue
        if text.startswith("module"):
            continue
        text = re.sub(r"@\s*\([^)]*\)", " ", text)                 # a sensitivity list
        text = re.sub(r"^\s*([\w']+|default)\s*:(?!=)", " ", text)  # a case label
        if text.startswith("assign"):
            targets = re.findall(r"assign\s+(\w+)", text)
        else:
            targets = re.findall(r"(\w+)\s*<?=(?!=)", text)
        assigned.update(targets)
        rest = re.sub(r"(\w+)\s*<?=(?!=)", " ", re.sub(r"assign\s+\w+\s*=", " ", text))
        read.update(n for n in re.findall(r"[A-Za-z_]\w*", rest) if n not in KEYWORDS)
    return sorted((read & regs) - assigned)


def no_repeat(lines):
    """Does no statement repeat an earlier one of the same block word for
    word? A repeated statement is legal Verilog and does nothing: a
    detector's list wrote its output's reset clause to the turn limit, and a
    counter's alternated its reset and hold lines. Openers and closers are
    not statements here: correct plans open `always @(*) begin` twice and
    close many blocks with `end`. More than syntax: a rule against code
    that does nothing."""
    blocks = [set()]
    for line in lines:
        text = re.sub(r"//.*", "", line).strip()
        if not text:
            continue
        closes = len(re.findall(r"\b(end|endcase)\b", text))
        opens = len(re.findall(r"\b(begin|case)\b", text))
        statement = text if not (CLOSER.match(text) or opens) and not MODULE_ITEM.match(text) else None
        if statement and len(blocks) > 1:
            if statement in blocks[-1]:
                return False
            blocks[-1].add(statement)
        for _ in range(closes):
            if len(blocks) > 1:
                blocks.pop()
        for _ in range(opens):
            blocks.append(set())
    return True


def empty_blocks(lines):
    """The blocks closed with nothing in them, as (begin line, end line)
    index pairs. `begin` followed at once by `end` is legal Verilog and does
    nothing. The menus do not refuse it: refused there, it sent the model's
    state-table lists down another path, and 12 of 16 into a dead end
    (LLM_RTL_code_generation.md 8.6.15). A passing list is cleaned of it
    afterwards instead (lift_loops.clean)."""
    out, pending = [], None
    for k, line in enumerate(lines):
        text = re.sub(r"//.*", "", line).strip()
        if not text:
            continue
        if pending is not None and re.match(r"end\b", text):
            out.append((pending, k))
        pending = k if re.search(r"\bbegin\s*(:\s*\w+)?$", text) else None
    return out


def declared_names(lines):
    """The names these lines declare: ports, registers, state parameters."""
    out = set()
    for line in lines:
        text = re.sub(r"//.*", "", line).strip()
        decl = re.match(r"(input|output|inout|reg|wire|parameter|localparam)\b(.*)", text)
        if decl:
            body = re.sub(r"\[[^\]]*\]", " ", decl.group(2))
            if decl.group(1) in ("parameter", "localparam"):
                out.update(re.findall(r"(\w+)\s*=", body))
            else:
                out.update(n for n in re.findall(r"[A-Za-z_]\w*", body) if n not in KEYWORDS)
    return out


def declared_before_use(lines):
    """Is every name these lines use declared on an earlier line, and none
    declared twice? A port is declared by its input or output line, a state
    by its parameter line, a register by its reg line; an output may also be
    declared a reg on a line of its own, as Verilog-2001 allows. Verilog
    refuses a use before the declaration, and a second declaration, so this
    too is syntax: it says nothing of whether the module is right."""
    declared = set()
    kinds = {}                     # name: the keyword that declared it
    for line in lines:
        text = re.sub(r"//.*", "", line).strip()
        if not text:
            continue
        text = re.sub(r"\d+'[bdho][0-9a-fA-FxXzZ_]+", " ", text)    # sized literals
        if re.match(r"module\s+\w+\s*\(.*\)\s*;", text):
            continue                                               # the port list: declared below
        decl = re.match(r"(input|output|inout|reg|wire|parameter|localparam)\b(.*)", text)
        if decl:
            body = re.sub(r"\[[^\]]*\]", " ", decl.group(2))     # widths
            if decl.group(1) in ("parameter", "localparam"):
                names = re.findall(r"(\w+)\s*=", body)
                used = set(re.findall(r"=\s*([A-Za-z_]\w*)", body))
            else:
                names = [n for n in re.findall(r"[A-Za-z_]\w*", body) if n not in KEYWORDS]
                used = set()
            if used - declared - KEYWORDS:
                return False
            for n in names:
                if n in kinds and not (decl.group(1) == "reg" and kinds[n] == "output"):
                    return False
                kinds[n] = decl.group(1)
            declared.update(names)
            continue
        if {n for n in re.findall(r"[A-Za-z_]\w*", text) if n not in KEYWORDS} - declared:
            return False                                           # a case label is a use too
    return True


def finish(problem, done, tools, why=None):
    """The verdict on a finished, or abandoned, list."""
    v = {"steps": len(done)}
    if why:
        return dict(v, stage=why, diagnostic=why, reward=0.0)
    plan = problem.plan(done)
    lines = problem.code(plan)
    wrong = lf.simulate("\n".join(lines) + "\n", problem.case["tb"], tools)
    if wrong:
        # A graded reward beside the verdict, for training only: how close
        # the module came (learn_family.graded).
        reward, _ = lf.graded("\n".join(lines) + "\n", problem.case["tb"], tools)
        return dict(v, stage="simulate", diagnostic=wrong, plan_steps=len(plan), reward=reward)
    other = lf.plan(problem.domain_path, lg.with_steps(problem.compiled, list(done)), seed=2)[0]
    if other != plan:
        return dict(v, stage="ambiguous", diagnostic="two seeds, two plans", plan_steps=len(plan), reward=0.0)
    full, _, _ = lf.plan(lf.DOMAIN, problem.compiled)
    return dict(v, stage="pass", diagnostic="", reward=1.0, plan_steps=len(plan), same_plan_as_hand_written=plan == full,
                tasks_used=sum(problem.known[n][0] == "task" for n, _ in done))


def write_request(d, user):
    os.makedirs(d, exist_ok=True)
    body = json.dumps({"system": SYSTEM, "user": user}, indent=1)
    path = os.path.join(d, "request.json")
    if not os.path.exists(path) or open(path).read() != body:
        with open(path, "w") as f:
            f.write(body)
    return path


def outline_of(reply):
    """The phases of a numbered list, or None."""
    phases = [m.group(1).strip().rstrip(".") for m in re.finditer(r"^\s*\d+[.)]\s*(.+?)\s*$", reply, re.M)]
    return phases if 2 <= len(phases) <= 12 else None


def choice(reply, n):
    """The number the reply gives, as an index into the menu, or None."""
    m = re.search(r"\d+", reply)
    return int(m.group()) - 1 if m and 1 <= int(m.group()) <= n else None


def cmd_show(args, cfg, tools):
    train, _ = lf.split(args.family)
    problem = Problem(args.family, train[args.problem], os.path.join(RUNS, "show"))
    done = []
    for pick in args.take or []:
        done.append(problem.menu(done)[pick - 1]["step"])
    print(problem.prompt(done))
    print(f"\n({len(problem.menu(done))} entries; {len(problem.prompt(done))} characters)")


def walk(problem, steps):
    """Walk a list through the menus: is each of its steps on offer, or an
    entry that comes to the same plan? Returns (None, menu sizes) or (the
    first step that is not, its index)."""
    done, sizes = [], []
    for k, step in enumerate(steps):
        target = problem.plan(done + [step])
        if target is not None and target == problem.plan(done):
            done.append(step)                 # does nothing here, as menus leave out
            continue
        menu = problem.menu(done)
        if target is None or not any(e["plan"] == target for e in menu):
            return step, k
        sizes.append(len(menu))
        done.append(step)
    return None, sizes


def cmd_check(args, cfg, tools):
    """The menus checked against lists known to be right: the hand-written
    methods' own list for each training problem must be reachable through
    them and pass, and with --lists, so must the model's passing lists of an
    earlier run. A change to the menus that hides a step a right list needs
    shows here, before any model is run."""
    bad = 0
    for family in args.families or sorted(lf.FAMILIES):
        train, _ = lf.split(family)
        for case in train:
            problem = Problem(family, case, os.path.join(RUNS, "check", family, case["tag"]))
            lists = [("hand-written", lg.frontier(family, case))]
            if args.lists:
                for path in sorted(glob.glob(os.path.join(args.lists, family, case["tag"], "run_*", "round_*",
                                                          "verdict.json"))):
                    if json.load(open(path))["stage"] == "pass":
                        steps, _ = lg.answer_steps("```\n" + open(os.path.join(os.path.dirname(path), "steps.txt")).read()
                                                   + "```")
                        if all(lg.show(steps) != lg.show(x) for _, x in lists):
                            lists.append((path.split(os.sep)[-3], steps))
            for name, steps in lists:
                missing, at = walk(problem, steps)
                if missing:
                    bad += 1
                    print(f"{family} {case['tag']} {name}: step {at + 1} not on the menu: "
                          f"({' '.join([missing[0]] + list(missing[1]))})", flush=True)
                    continue
                verdict = finish(problem, steps, tools)
                bad += verdict["stage"] != "pass"
                print(f"{family} {case['tag']} {name}: reachable, menus {min(at)}-{max(at)}, {verdict['stage']}",
                      flush=True)
    print(f"{bad} lists not reachable or not passing")


def build_lists(problems, train, keys, base, cfg, tools, policy="model", runs=1, phases=False, starts=None,
                scorer=None, weight=0.0):
    """Build lists by choosing from menus: one for each key (problem
    index, run, ...), from the steps in `starts` (default none), until a step
    writes endmodule or the list cannot go on. Returns the lists, their
    verdicts, and the trail, outline and phase of each.

    Who chooses (`policy`):
      model     the model's sampled reply. Lists at the same point share one
                request, and run r takes its sample r.
      random    a uniform draw.
      combined  a draw from softmax((log p + weight * score) / T): log p the
                model's probability of each entry's number (executor_score),
                score the scorer's, T the sampling temperature of the
                configuration. Weight 0 is the model alone, held to the menu.
      scorer    the entry the scorer rates highest.
    Every draw is from the list's own seeded generator."""
    done = {k: [(n, list(a)) for n, a in (starts or {}).get(k, [])] for k in keys}
    verdicts, trail = {}, collections.defaultdict(list)
    rng = {k: random.Random(1000 * k[0] + k[1] + sum(1000003 * x for x in k[2:])) for k in keys}
    outline, phase, phase_of = {}, {k: 0 for k in keys}, collections.defaultdict(list)
    if phases:
        # Turn 0: an outline a list, in the model's own words.
        jobs = []
        for p, case in enumerate(train):
            d = os.path.join(base, case["tag"], "outline")
            request = write_request(d, problems[p].outline_prompt())
            if not lf.generated(d, runs):
                jobs.append({"request": request, "out_dir": d})
        lf.executor(cfg, jobs, os.path.join(base, "batch.json"), runs)
        for p, r in keys:
            with open(os.path.join(base, train[p]["tag"], "outline", f"sample_{r:02d}.response.txt"),
                      errors="replace") as f:
                found = outline_of(f.read())
            if found:
                outline[(p, r)] = found
            else:
                verdicts[(p, r)] = finish(problems[p], [], tools, "no outline")
    for turn in range(1, (MAX_TURNS_PHASED if phases else MAX_TURNS) + 1):
        for k in keys:
            if k not in verdicts and len(done[k]) >= MAX_TURNS:
                verdicts[k] = finish(problems[k[0]], done[k], tools, "not finished")
        active = [k for k in keys if k not in verdicts]
        if not active:
            break
        groups = collections.defaultdict(list)       # lists at the same point share a request
        for k in active:
            groups[(k[0], tuple((n, tuple(a)) for n, a in done[k]), phase[k],
                    tuple(outline.get(k, ())))].append(k)
        menus = {g: problems[g[0]].menu(done[ks[0]]) for g, ks in groups.items()}
        picks = {}
        if policy == "random":
            for g, ks in groups.items():
                for k in ks:
                    n = len(menus[g])
                    picks[k] = rng[k].randrange(n) if n else None
        elif policy in ("combined", "scorer"):
            # The model's preference over each menu, one batch for the turn.
            where, jobs = {}, []
            if policy == "combined":
                for g, ks in groups.items():
                    if menus[g]:
                        d, job = score_request(base, train[g[0]]["tag"], len(done[ks[0]]) + 1,
                                               problems[g[0]].prompt(done[ks[0]]), len(menus[g]))
                        where[g] = d
                        jobs += [job] if job else []
                lf.scorer_executor(cfg, jobs, os.path.join(base, "score_batch.json"))
            for g, ks in groups.items():
                if menus[g]:
                    k0 = ks[0]
                    lp = [x["logprob"] for x in json.load(open(os.path.join(where[g], "scores.json")))] \
                        if g in where else None
                    rewards = scorer.rate(problems[g[0]], done[k0], menus[g], lp) if scorer else [0.0] * len(menus[g])
                    if policy == "combined":
                        logits = [(a + weight * b) / temperature(cfg) for a, b in zip(lp, rewards)]
                    else:
                        logits = rewards
                for k in ks:
                    if not menus[g]:
                        picks[k] = None
                    elif policy == "scorer":
                        picks[k] = max(range(len(logits)), key=lambda i: logits[i])
                    else:
                        top = max(logits)
                        w = [math.exp(x - top) for x in logits]
                        picks[k] = rng[k].choices(range(len(w)), weights=w)[0]
                    if menus[g]:
                        trail[k].append({"turn": turn, "pick": picks[k] + 1, "entries": len(menus[g])})
        else:
            jobs, where = [], {}
            for g, ks in groups.items():
                if not menus[g]:
                    continue
                k0 = ks[0]
                text = problems[g[0]].prompt(done[k0], outline.get(k0), phase[k0])
                d = os.path.join(base, train[g[0]]["tag"], f"turn_{len(done[k0]) + 1 if starts else turn:02d}",
                                 hashlib.sha256(text.encode()).hexdigest()[:12])
                request = write_request(d, text)
                where[g] = d
                if not lf.generated(d, runs):
                    jobs.append({"request": request, "out_dir": d})
            lf.executor(cfg, jobs, os.path.join(base, "batch.json"), runs)
            for g, ks in groups.items():
                for k in ks:
                    if g not in where:
                        picks[k] = None
                        continue
                    with open(os.path.join(where[g], f"sample_{k[1]:02d}.response.txt"), errors="replace") as f:
                        reply = f.read()
                    can_move = k in outline and phase[k] + 1 < len(outline[k])
                    picks[k] = choice(reply, len(menus[g]) + can_move)
                    trail[k].append({"turn": turn, "reply": reply[:80], "entries": len(menus[g])})
        for g, ks in groups.items():
            for k in ks:
                if not menus[g]:
                    verdicts[k] = finish(problems[g[0]], done[k], tools, "nothing can be done")
                elif picks[k] is None:
                    verdicts[k] = finish(problems[g[0]], done[k], tools, "no valid choice")
                elif picks[k] == len(menus[g]):        # the entry that moves to the next phase
                    phase[k] += 1
                else:
                    entry = menus[g][picks[k]]
                    done[k] = done[k] + [entry["step"]]
                    phase_of[k].append(phase[k])
                    if entry["closes"]:
                        verdicts[k] = finish(problems[g[0]], done[k], tools)
        print(f"turn {turn}: {len(active)} lists building, {len(verdicts)} finished "
              f"({sum(v['stage'] == 'pass' for v in verdicts.values())} pass)", flush=True)
    return done, verdicts, trail, outline, phase_of


def temperature(cfg):
    """The sampling temperature of the configuration the model runs under."""
    with open(lf.proposer_config(cfg)) as f:
        return json.load(f)["ladder_sampling"]["temperature"]


def score_request(base, tag, turn, text, n):
    """A request for the model's log-probability of each of the n entries'
    numbers as its reply to this menu prompt (executor_score), cached by the
    prompt. Returns its directory, and the job if it is not done yet."""
    d = os.path.join(base, tag, f"turn_{turn:02d}", hashlib.sha256(text.encode()).hexdigest()[:12] + "_scores")
    os.makedirs(d, exist_ok=True)
    request = os.path.join(d, "request.json")
    body = json.dumps({"system": SYSTEM, "user": text, "candidates": [str(i) for i in range(1, n + 1)]}, indent=1)
    if not os.path.exists(request) or open(request).read() != body:
        with open(request, "w") as f:
            f.write(body)
    return d, (None if lf.scored(d) else {"request": request, "out_dir": d})


def cmd_run(args, cfg, tools):
    family, base = args.family, os.path.join(RUNS, args.family)
    os.makedirs(base, exist_ok=True)
    train, held = lf.split(family)
    # The family's training problems, or its generated held-out ones from
    # --offset on: those choose a chooser's weight, and test it, never seen
    # in training (HRL_notes.md 4.1).
    train = train if args.split == "train" else held[args.offset::args.stride]
    train = train[:args.problems] if args.problems else train
    problems = [Problem(family, c, os.path.join(base, c["tag"])) for c in train]
    methods, tasks = lf.removed(family)
    meta = {"created": rtl_eval.now(), "family": family, "split": args.split, "offset": args.offset,
            "stride": args.stride,
            "runs": args.runs, "policy": args.policy,
            "phases": args.phases,
            "max_free": MAX_FREE, "max_tied": MAX_TIED, "max_turns": MAX_TURNS,
            "removed_methods": sorted(methods), "removed_tasks": sorted(tasks), "problems": [c["tag"] for c in train],
            "sha256": {"domain": rtl_eval.sha256_file(lf.DOMAIN), "gloss": rtl_eval.sha256_file(GLOSS),
                       "learn_steps": rtl_eval.sha256_file(os.path.abspath(__file__)),
                       "learn_ground": rtl_eval.sha256_file(os.path.join(HERE, "learn_ground.py")),
                       "learn_family": rtl_eval.sha256_file(os.path.join(HERE, "learn_family.py"))}}
    if args.scorer:
        meta["scorer"] = {"path": os.path.abspath(args.scorer), "sha256": rtl_eval.sha256_file(args.scorer),
                          "weight": args.weight}
    if args.policy in ("model", "combined"):
        with open(lf.proposer_config(cfg)) as f:
            meta["proposer"] = {"config": lf.proposer_config(cfg), "sampling": json.load(f)["ladder_sampling"]}
        meta["sha256"]["proposer_config"] = rtl_eval.sha256_file(lf.proposer_config(cfg))
    with open(os.path.join(base, "run.json"), "w") as f:
        json.dump(meta, f, indent=1)

    keys = [(p, r) for p in range(len(train)) for r in range(args.runs)]
    chooser = None
    if args.policy == "scorer" or (args.policy == "combined" and args.weight):
        if not args.scorer:
            sys.exit("error: --policy scorer, or combined with a weight, needs --scorer")
        import scorer as scorer_module
        chooser = scorer_module.Scorer(args.scorer)
    done, verdicts, trail, outline, phase_of = build_lists(problems, train, keys, base, cfg, tools, args.policy,
                                                           args.runs, args.phases, scorer=chooser, weight=args.weight)
    for k in keys:
        if k not in verdicts:
            verdicts[k] = finish(problems[k[0]], done[k], tools, "not finished")
        # Where lift.py looks for checked lists.
        d = os.path.join(base, train[k[0]]["tag"], f"run_{k[1]:02d}", "round_1")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "steps.txt"), "w") as f:
            f.write(lg.show(done[k]) + "\n")
        with open(os.path.join(d, "verdict.json"), "w") as f:
            json.dump(dict(verdicts[k], trail=trail[k], outline=outline.get(k), phase_of_step=phase_of[k]), f,
                      indent=1)
        print(f"{train[k[0]]['tag']} run {k[1]:02d}: {verdicts[k]['stage']} after {len(done[k])} steps", flush=True)
    with open(os.path.join(base, "verdicts.json"), "w") as f:
        json.dump({f"{train[p]['tag']}/{r}": dict(v, list=lg.show(done[(p, r)]).splitlines(),
                                                   outline=outline.get((p, r)), phase_of_step=phase_of[(p, r)])
                   for (p, r), v in verdicts.items()}, f, indent=1)


def cmd_summary(args, cfg, tools):
    md = ["# Leave one family out: the model builds a step list by choosing among the steps that can be taken", "",
          "| family | policy | problems | lists | pass | same plan as the hand-written methods | "
          "problems with a passing list | how the others ended |",
          "|---|---|---|---|---|---|---|---|"]
    out = {}
    for family in lf.FAMILIES:
        path = os.path.join(RUNS, family, "verdicts.json")
        if not os.path.exists(path):
            continue
        verdicts = json.load(open(path))
        meta = json.load(open(os.path.join(RUNS, family, "run.json")))
        stages = collections.Counter(v["stage"] for v in verdicts.values())
        same = sum(v.get("same_plan_as_hand_written", False) for v in verdicts.values())
        solved = {k.split("/")[0] for k, v in verdicts.items() if v["stage"] == "pass"}
        others = ", ".join(f"{k} {v}" for k, v in stages.items() if k != "pass") or "—"
        md.append(f"| {family} | {meta['policy']} | {len(meta['problems'])} | {len(verdicts)} | {stages['pass']} | "
                  f"{same} of {stages['pass']} | {len(solved)} of {len(meta['problems'])} | {others} |")
        out[family] = {"policy": meta["policy"], "stages": dict(stages), "same_plan": same,
                       "problems_solved": sorted(solved)}
    os.makedirs(RUNS, exist_ok=True)
    with open(os.path.join(RUNS, "summary.json"), "w") as f:
        json.dump(out, f, indent=1)
    with open(os.path.join(RUNS, "summary.md"), "w") as f:
        f.write("\n".join(md) + "\n")
    print("\n".join(md))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--config", default=os.path.join(HERE, "eval_config.json"))
    ap.add_argument("--tag", help="keep this run apart: eval/runs/steps_<tag>")
    ap.add_argument("--proposer-config", help="how the model is run (default: the executor's configuration)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("show")
    s.add_argument("family", choices=sorted(lf.FAMILIES))
    s.add_argument("--problem", type=int, default=0, help="which training problem (default the first)")
    s.add_argument("--take", type=int, nargs="*", help="menu numbers to take first, to see a later turn")
    s = sub.add_parser("run")
    s.add_argument("family", choices=sorted(lf.FAMILIES))
    s.add_argument("--runs", type=int, default=4, help="lists a problem (default 4)")
    s.add_argument("--problems", type=int, help="use only the first N problems (default all)")
    s.add_argument("--split", choices=["train", "heldout"], default="train",
                   help="the family's training problems (default) or its generated held-out ones")
    s.add_argument("--offset", type=int, default=0, help="with --split heldout, start at this held-out problem")
    s.add_argument("--stride", type=int, default=1,
                   help="with --split heldout, take every Nth held-out problem from --offset (they are ordered "
                        "by size, so a stride spreads them)")
    s.add_argument("--policy", choices=["model", "random", "combined", "scorer"], default="model",
                   help="who chooses: the model's sampled reply; a uniform draw (no model is run); the model's "
                        "probabilities combined with a scorer's; or the scorer alone (see build_lists)")
    s.add_argument("--scorer", help="a scorer trained by eval/scorer.py, for --policy combined or scorer")
    s.add_argument("--weight", type=float, default=0.0, help="the scorer's weight, for --policy combined")
    s.add_argument("--phases", action="store_true",
                   help="the model first writes an outline of phases; each step is chosen within the phase it is on")
    sub.add_parser("summary")
    s = sub.add_parser("check")
    s.add_argument("families", nargs="*", help="(default every family)")
    s.add_argument("--lists", metavar="DIR", help="also walk the passing lists of this run (e.g. eval/runs/steps)")
    args = ap.parse_args()
    global RUNS
    if args.tag:
        RUNS += "_" + args.tag
    lf.PROPOSER_CONFIG = args.proposer_config
    cfg, tools = rtl_eval.load_config(os.path.abspath(args.config))
    {"show": cmd_show, "run": cmd_run, "summary": cmd_summary, "check": cmd_check}[args.cmd](args, cfg, tools)


if __name__ == "__main__":
    main()
