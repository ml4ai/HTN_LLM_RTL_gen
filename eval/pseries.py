"""The P-series: how a plan is presented to the executor
(LLM_RTL_code_generation.md 8.6.5), on the pilot problems of
rtl_designs/pilot/pilot.json.

An arm is one level of each of four factors, named p-<format>-<delivery>-
<tree>-<description>:

  format       s0    the plan's s-expressions as the planner emits them
               s1    the same with named arguments, (:state s4 :input IN ...),
                     the names being the action's parameter names
               s2    s1, plus a schema: one line per action type the plan
                     uses, saying what it means, and a legend for the objects
               nl    one natural-language sentence per step
  delivery     single  the whole plan in one prompt
               micro   one conversation: a turn per top-level part of the
                       plan, then a turn asking for the complete module
  tree         notree | tree   whether the task tree behind the plan is given
  description  desc | nodesc   whether the design description is given

4 x 2 x 2 x 2 = 32 arms. The direct-prompt baseline (description only) is the
`direct` arm of rtl_eval.py, and is not built here.

    python eval/pseries.py show p-s2-single-tree-desc rtllm-2 fsm   # one prompt
    python eval/pseries.py build     # write every arm's requests and run.json
    python eval/pseries.py generate  # run the executor on what is not done yet
    python eval/pseries.py evaluate  # rtl_eval.py evaluate and report, per arm
    python eval/pseries.py summary   # the arms and both baselines, side by side

Each takes --greedy for the greedy line, which lives beside the sampled runs,
and build, generate and evaluate take --arms or --delivery to do part of them.
--tag NAME (before the command) puts a run in eval/runs/pseries_NAME, apart
from the pilot's, which is how a changed sentence or prompt is tried.
An arm's run directory is an ordinary rtl_eval.py one (eval/runs/pseries/<arm>),
so evaluate, report, breakdown and compare all work on it.

The plans are the saved ones (rtl_designs/check_pilot.py writes and checks
them); nothing here runs the planner. Everything that turns a plan into a
prompt is in this file and in rtl_designs/rtl_domain_gloss.json, and run.json
records the hash of each.
"""

import argparse
import itertools
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import benchmarks  # noqa: E402
import passk  # noqa: E402
import rtl_eval  # noqa: E402

FORMATS = ("s0", "s1", "s2", "nl")
DELIVERIES = ("single", "micro")
TREES = ("notree", "tree")
DESCRIPTIONS = ("desc", "nodesc")
FACTORS = (("format", FORMATS), ("delivery", DELIVERIES), ("tree", TREES), ("description", DESCRIPTIONS))

MANIFEST = os.path.join(ROOT, "rtl_designs", "pilot", "pilot.json")
GLOSS = os.path.join(ROOT, "rtl_designs", "rtl_domain_gloss.json")
RUNS = {"sampled": os.path.join(HERE, "runs", "pseries"), "greedy": os.path.join(HERE, "runs", "pseries_greedy")}
BASELINES = {"sampled": {"direct": "direct", "direct_rules": "direct_rules"},
             "greedy": {"direct": "direct_greedy", "direct_rules": "direct_rules_greedy"}}

# An intermediate micro-prompt reply may run this long; the final reply has
# the executor's max_new_tokens, as every single-prompt reply does.
INTERMEDIATE_MAX_NEW_TOKENS = 512

# ---------------------------------------------------------------------------
# The words around a plan. Pre-registered: changing any of them is a new arm.

INTRO = {
    "symbolic": "A plan for this module was worked out in advance. It is a list of steps, in order. "
                "A step named analyze_... records a design decision that has already been made; "
                "a step named write_... is a piece of code to write.",
    "nl": "A plan for this module was worked out in advance. It is a list of steps, in order: "
          "the design decisions that have already been made, and the pieces of code to write.",
}
NAMED_ARGUMENTS = "Arguments are written as :name value."
SCHEMA_HEAD = "What each kind of step means:"
PLAN_HEAD = "Plan:"
TREE_HEAD = ("Task tree behind the plan. Each line is a task and the method chosen to carry it out; "
             "indented under it are its subtasks and the plan steps it led to:")
FOLLOW = "Write the module by following the plan step by step."
NO_DESCRIPTION = {
    "verilog-eval-v2": "I would like you to implement a Verilog module by following the plan below.",
    "rtllm-2": "Please act as a professional verilog designer.\n\n"
               "Implement the Verilog module that the plan below describes.",
}
MICRO_INTRO = ("The plan has {k} parts, and I will give you one part at a time. For each part, reply with "
               "what that part asks for and nothing more: the Verilog its steps describe, or a one-line "
               "acknowledgement if it only records design decisions. After the last part I will ask for "
               "the complete module.")
PART_HEAD = "Part {i} of {k} ({title}):"
PART_TREE_HEAD = "Task tree behind this part:"
MICRO_FINAL = "That was the whole plan. Now write the complete module, following the plan."
# Amended after the first greedy lines (LLM_RTL_code_generation.md 8.6.6). In a
# conversation the executor answers each part in a Markdown fence, and kept the
# fence in its final answer, inside [BEGIN]..[DONE]: 56 of 64 VerilogEval
# micro-prompt replies, none of the single-prompt ones. VerilogEval's
# extraction takes what is between the markers as it stands, so those failed to
# compile whatever the code. RTLLM's extraction reads fenced blocks, and its
# final turn is unchanged.
VE_MICRO_PLAIN = ("Put the code between the markers as plain text: no Markdown code fences, "
                  "so no line of backticks.")
RTLLM_CLOSE = "Give me the complete code."


def arm_name(fmt, delivery, tree, desc):
    return f"p-{fmt}-{delivery}-{tree}-{desc}"


def parse_arm(name):
    parts = name.split("-")
    if len(parts) != 5 or parts[0] != "p" or any(v not in levels for v, (_, levels) in zip(parts[1:], FACTORS)):
        sys.exit(f"error: {name} is not a P-series arm (p-<{'|'.join(FORMATS)}>-<{'|'.join(DELIVERIES)}>-"
                 f"<{'|'.join(TREES)}>-<{'|'.join(DESCRIPTIONS)}>)")
    return dict(zip((f for f, _ in FACTORS), parts[1:]))


def all_arms():
    return [arm_name(*c) for c in itertools.product(FORMATS, DELIVERIES, TREES, DESCRIPTIONS)]


# ---------------------------------------------------------------------------
# The domain's actions, the saved plans and their trees.

class Domain:
    """Each action's parameters (name, type) from the HDDL, and its sentence."""

    def __init__(self, hddl_path, gloss_path):
        with open(hddl_path) as f:
            text = f.read()
        self.params = {}
        for m in re.finditer(r"\(:action\s+(\w+)\s+:parameters\s+\(([^)]*)\)", text):
            params, pending = [], []
            for tok in m.group(2).split():
                if tok.startswith("?"):
                    pending.append(tok[1:])
                elif tok != "-":
                    params += [(p, tok) for p in pending]
                    pending = []
            self.params[m.group(1)] = params
        with open(gloss_path) as f:
            gloss = json.load(f)
        self.legend = gloss["legend"]
        self.sentence = gloss["actions"]
        missing = sorted(set(self.params) - set(self.sentence))
        if missing:
            sys.exit(f"error: {gloss_path} has no sentence for {missing}")


def spoken(value, type_):
    """An object as a sentence says it: one -> 1, active_low -> active-low,
    bits5 -> 5, n31 -> 31. Names (states, ports, the module) stay as they are."""
    if type_ == "bitval":
        return {"zero": "0", "one": "1"}[value]
    if type_ == "polarity":
        return value.replace("_", "-")
    if type_ in ("width", "value"):
        return re.fullmatch(r"(?:bits|n)(\d+)", value).group(1)
    return value


def load_plan(bench, pid):
    """The saved plan as [(action, [args])], and its task tree below the
    problem's own root: the node of the top-level task."""
    base = os.path.join(ROOT, "rtl_designs", "pilot", bench, pid)
    with open(base + ".plan.txt") as f:
        steps = [line.strip()[1:-1].split() for line in f if line.strip()]
    steps = [(s[0], s[1:]) for s in steps]
    with open(base + ".tree.json") as f:
        root = json.load(f)["roots"][0]
    if len(root["children"]) != 1:
        sys.exit(f"error: {base}.tree.json: expected one top-level task")
    top = root["children"][0]
    leaves = []

    def walk(n):
        if n["action"]:
            leaves.append((n["step"], n["token"]))
        for c in n["children"]:
            walk(c)
    walk(top)
    want = [(i + 1, "(" + " ".join([name] + args) + ")") for i, (name, args) in enumerate(steps)]
    if sorted(leaves) != want:
        sys.exit(f"error: {base}: the saved plan and tree disagree; rerun rtl_designs/check_pilot.py")
    return steps, top


def step_numbers(node):
    if node["action"]:
        return [node["step"]]
    return [s for c in node["children"] for s in step_numbers(c)]


# ---------------------------------------------------------------------------
# Rendering.

def render_step(domain, step, fmt):
    name, args = step
    params = domain.params[name]
    if fmt == "s0":
        return "(" + " ".join([name] + args) + ")"
    if fmt in ("s1", "s2"):
        return "(" + " ".join([name] + [f":{p} {a}" for (p, _), a in zip(params, args)]) + ")"
    return domain.sentence[name].format(**{p: spoken(a, t) for (p, t), a in zip(params, args)})


def render_steps(domain, steps, numbers, fmt):
    return [f"{n}. {render_step(domain, steps[n - 1], fmt)}" for n in numbers]


def render_schema(domain, steps):
    """One line per action type the plan uses, in order of first use: its
    signature with named arguments, and its sentence with the parameter names
    standing where a step's values would."""
    lines, seen = [domain.legend], set()
    for name, _ in steps:
        if name in seen:
            continue
        seen.add(name)
        params = [p for p, _ in domain.params[name]]
        signature = "(" + " ".join([name] + [f":{p}" for p in params]) + ")"
        lines.append(f"{signature}: {domain.sentence[name].format(**{p: ':' + p for p in params})}")
    return lines


def step_ranges(numbers):
    """[3, 4, 5, 9] -> 'steps 3-5, 9'."""
    runs = []
    for n in numbers:
        if runs and runs[-1][1] == n - 1:
            runs[-1][1] = n
        else:
            runs.append([n, n])
    text = ", ".join(str(a) if a == b else f"{a}-{b}" for a, b in runs)
    return ("step " if len(numbers) == 1 else "steps ") + text


def render_tree(nodes, depth=0):
    """An indented outline: a compound task with the method that decomposed
    it, and under it its subtasks; actions as their plan step numbers, runs of
    them on one line. Derivation subtrees are kept whole."""
    lines, run = [], []
    pad = "  " * depth

    def flush():
        if run:
            lines.append(pad + step_ranges(run))
            run.clear()
    for n in nodes:
        if n["action"]:
            run.append(n["step"])
            continue
        flush()
        lines.append(f"{pad}{n['token']} by {n['method']}" + ("" if n["children"] else ": nothing to do"))
        lines += render_tree(n["children"], depth + 1)
    flush()
    return lines


def parts_of(top):
    """The plan cut at the top-level task's subtasks, for micro-prompting. A
    subtask that is a single action joins the part after it, or the part before
    it at the end, so that no turn carries one step alone."""
    groups, carry = [], []
    for child in top["children"]:
        if child["action"]:
            carry.append(child)
        else:
            groups.append(carry + [child])
            carry = []
    if carry:
        if groups:
            groups[-1] += carry
        else:
            groups.append(carry)
    parts = []
    for nodes in groups:
        compound = [n["token"][1:-1].split()[0] for n in nodes if not n["action"]]
        title = ", ".join(compound) if compound else nodes[0]["token"][1:-1].split()[0]
        parts.append({"title": title, "nodes": nodes, "steps": [s for n in nodes for s in step_numbers(n)]})
    return parts


def plan_preamble(domain, steps, fmt):
    """What precedes the steps: how to read them, and for s2 the schema."""
    lines = [INTRO["nl" if fmt == "nl" else "symbolic"]]
    if fmt == "s1":
        lines[0] += " " + NAMED_ARGUMENTS
    if fmt == "s2":
        lines[0] += " " + NAMED_ARGUMENTS
        lines += ["", SCHEMA_HEAD] + render_schema(domain, steps)
    return lines


def lead_of(problem, bench, with_description):
    return problem.extra["spec"].strip() if with_description else NO_DESCRIPTION[bench]


def build_request(arm, bench, problem, domain, steps, top):
    """The executor request of one arm for one problem: {"system", "user"},
    or for micro-prompting {"system", "turns", "intermediate_max_new_tokens"}.

    The frame is the benchmark's own, so that its extraction applies to the
    reply: VerilogEval's Question / [BEGIN]..[DONE] / Answer template with the
    plan where its optional rules go, and for RTLLM the description as given,
    then the plan."""
    f = parse_arm(arm)
    fmt, with_tree = f["format"], f["tree"] == "tree"
    lead = lead_of(problem, bench, f["description"] == "desc")
    preamble = plan_preamble(domain, steps, fmt)
    ve = bench == "verilog-eval-v2"
    no_explain = benchmarks.VerilogEvalV2.NO_EXPLAIN

    if f["delivery"] == "single":
        body = [lead, ""] + preamble + ["", PLAN_HEAD] + render_steps(domain, steps, range(1, len(steps) + 1), fmt)
        if with_tree:
            body += ["", TREE_HEAD] + render_tree([top])
        body += ["", FOLLOW]
        text = "\n".join(body)
        if ve:
            user = "\nQuestion:\n" + text.strip() + "\n" + no_explain + "\nAnswer:\n"
        else:
            user = text + "\n\n" + RTLLM_CLOSE
        return {"system": problem.system, "user": user}

    parts = parts_of(top)
    k = len(parts)
    turns = []
    for i, part in enumerate(parts, 1):
        body = []
        if i == 1:
            body += [lead, ""] + preamble + ["", MICRO_INTRO.format(k=k), ""]
        body += [PART_HEAD.format(i=i, k=k, title=part["title"])] + render_steps(domain, steps, part["steps"], fmt)
        if with_tree:
            body += ["", PART_TREE_HEAD] + render_tree(part["nodes"])
        text = "\n".join(body)
        turns.append(("\nQuestion:\n" + text) if (ve and i == 1) else text)
    turns.append(MICRO_FINAL + "\n" + no_explain + VE_MICRO_PLAIN + "\n\nAnswer:\n" if ve
                 else MICRO_FINAL + " " + RTLLM_CLOSE)
    return {"system": problem.system, "turns": turns, "intermediate_max_new_tokens": INTERMEDIATE_MAX_NEW_TOKENS}


# ---------------------------------------------------------------------------
# Commands.

class Pilot:
    def __init__(self, config):
        self.cfg, self.tools = rtl_eval.load_config(config)
        self.config = config
        with open(MANIFEST) as f:
            self.manifest = json.load(f)
        self.domain = Domain(os.path.join(ROOT, self.manifest["domain"]), GLOSS)
        self.adapters = {}
        self.problems = [(e["bench"], e["pid"], e["role"]) for e in self.manifest["problems"]]

    def adapter(self, bench):
        if bench not in self.adapters:
            self.adapters[bench] = rtl_eval.bench_adapter(self.cfg, bench)
        return self.adapters[bench]

    def request(self, arm, bench, pid):
        steps, top = load_plan(bench, pid)
        return build_request(arm, bench, self.adapter(bench).load(pid), self.domain, steps, top)

    def provenance(self):
        files = {"manifest": MANIFEST, "gloss": GLOSS, "prompt_builder": os.path.abspath(__file__),
                 "domain": os.path.join(ROOT, self.manifest["domain"])}
        for bench, pid, _ in self.problems:
            base = os.path.join(ROOT, "rtl_designs", "pilot", bench, pid)
            files[f"plan:{bench}/{pid}"] = base + ".plan.txt"
            files[f"tree:{bench}/{pid}"] = base + ".tree.json"
        return {k: rtl_eval.sha256_file(v) for k, v in files.items()}


def run_dir(mode, arm):
    return os.path.join(RUNS[mode], arm)


def selected_arms(args):
    for a in args.arms or []:
        parse_arm(a)
    arms = args.arms or all_arms()
    if getattr(args, "delivery", None):
        arms = [a for a in arms if parse_arm(a)["delivery"] == args.delivery]
    return arms


def cmd_show(args, pilot):
    req = pilot.request(args.arm, args.bench, args.pid)
    print(f"--- system ---\n{req['system']}")
    for i, turn in enumerate(req.get("turns", [req.get("user")]), 1):
        print(f"--- user, turn {i} ---\n{turn}")


def cmd_build(args, pilot):
    """Write run.json and every request.json. A request that is already there
    with other content means the arm's definition changed under a run: refuse,
    unless nothing was generated from it yet."""
    mode = "greedy" if args.greedy else "sampled"
    n = 1 if args.greedy else pilot.cfg["ladder"]["n"]
    ecfg = os.path.join(ROOT, pilot.cfg["executor"]["config"])
    written = 0
    for arm in selected_arms(args):
        d = run_dir(mode, arm)
        os.makedirs(d, exist_ok=True)
        benches = sorted({b for b, _, _ in pilot.problems})
        meta = {"created": rtl_eval.now(), "arm": arm, "mode": mode, "n": n,
                "benchmarks": {b: {"commit": pilot.cfg["benchmarks"][b]["commit"],
                                   "settings": pilot.cfg["benchmarks"][b]} for b in benches},
                "eval_config_sha256": rtl_eval.sha256_file(pilot.config),
                "executor_config_sha256": rtl_eval.sha256_file(ecfg),
                "pseries": {"factors": parse_arm(arm), "sha256": pilot.provenance(),
                            "intermediate_max_new_tokens": INTERMEDIATE_MAX_NEW_TOKENS}}
        meta_path = os.path.join(d, "run.json")
        if os.path.exists(meta_path):
            old = json.load(open(meta_path))
            meta["created"] = old["created"]
        for bench, pid, _ in pilot.problems:
            pd = os.path.join(d, bench, pid)
            os.makedirs(pd, exist_ok=True)
            body = json.dumps(pilot.request(arm, bench, pid), indent=1)
            req = os.path.join(pd, "request.json")
            if os.path.exists(req) and open(req).read() != body and os.path.exists(os.path.join(pd, "gen.json")):
                sys.exit(f"error: {req} differs from what this arm now builds, and samples were generated "
                         f"from it. Move the run aside before rebuilding.")
            with open(req, "w") as f:
                f.write(body)
            written += 1
        with open(meta_path, "w") as f:
            json.dump(meta, f, indent=1)
    print(f"{written} requests written under {RUNS[mode]}")


def cmd_generate(args, pilot):
    """One executor process for everything not generated yet: the model is
    loaded once. Safe to interrupt; a request is done when its gen.json is."""
    mode = "greedy" if args.greedy else "sampled"
    n = 1 if args.greedy else pilot.cfg["ladder"]["n"]
    jobs = []
    for arm in selected_arms(args):
        for bench, pid, _ in pilot.problems:
            pd = os.path.join(run_dir(mode, arm), bench, pid)
            req, gen = os.path.join(pd, "request.json"), os.path.join(pd, "gen.json")
            if not os.path.exists(req):
                sys.exit(f"error: {req} missing; run build first")
            if os.path.exists(gen):
                g = json.load(open(gen))
                if g.get("n") == n and g.get("mode") == mode and \
                        g.get("request_sha256") == rtl_eval.sha256_file(req):
                    continue
            jobs.append({"request": req, "out_dir": pd})
    if not jobs:
        print("nothing to generate")
        return
    os.makedirs(RUNS[mode], exist_ok=True)
    batch = os.path.join(RUNS[mode], "batch.json")
    with open(batch, "w") as f:
        json.dump(jobs, f, indent=1)
    exe = os.path.join(ROOT, pilot.cfg["executor"]["binary"])
    cmd = [exe, "--batch", batch, "--config", os.path.join(ROOT, pilot.cfg["executor"]["config"])]
    cmd += ["--greedy"] if args.greedy else ["--n", str(n)]
    print(f"{len(jobs)} requests to generate ({mode})", flush=True)
    rc = subprocess.run(cmd, stdout=subprocess.DEVNULL)
    if rc.returncode != 0:
        sys.exit("error: the executor failed; see its message above. Rerun generate to resume.")


def cmd_evaluate(args, pilot):
    mode = "greedy" if args.greedy else "sampled"
    py = sys.executable
    for arm in selected_arms(args):
        d = run_dir(mode, arm)
        for sub in ("evaluate", "report"):
            cmd = [py, os.path.join(HERE, "rtl_eval.py"), "--config", pilot.config, sub, "--run-dir", d]
            if sub == "evaluate":
                cmd += ["--jobs", str(args.jobs)]
            rc = subprocess.run(cmd, stdout=subprocess.DEVNULL)
            if rc.returncode != 0:
                sys.exit(f"error: rtl_eval.py {sub} failed on {d}")
        print(f"{arm}: evaluated", flush=True)


def counts(run, bench, pid):
    """(syntax passes, functional passes, samples, generated tokens over all
    turns) of one problem in a run directory, or None if it is not there."""
    path = os.path.join(run, bench, "results.jsonl")
    if not os.path.exists(path):
        return None
    rs = [r for r in map(json.loads, open(path)) if r["pid"] == pid]
    if not rs:
        return None
    tokens = None
    gen = os.path.join(run, bench, pid, "gen.json")
    if os.path.exists(gen):
        g = json.load(open(gen))
        tokens = sum(s.get("generated_tokens_all_turns", s["generated_tokens"]) for s in g["samples"]) / len(rs)
    return sum(r["syntax"] for r in rs), sum(r["functional"] for r in rs), len(rs), tokens


def cmd_summary(args, pilot):
    """Every arm beside the baselines: functional passes per problem, and the
    means over the poor and the adequate problems; then each factor's levels
    averaged over the other three."""
    mode = "greedy" if args.greedy else "sampled"
    rows = {}
    for label, name in BASELINES[mode].items():
        rows[label] = os.path.join(HERE, "runs", name)
    for arm in all_arms():
        rows[arm] = run_dir(mode, arm)
    table, out = [], {"mode": mode, "problems": pilot.problems, "arms": {}}
    for label, run in rows.items():
        cells, by_role, syn, tokens = [], {"poor": [], "adequate": []}, [], []
        for bench, pid, role in pilot.problems:
            c = counts(run, bench, pid)
            if c is None:
                cells.append("—")
                continue
            cs, cf_, n, tok = c
            cells.append(f"{cf_}/{n}")
            by_role[role].append(cf_ / n)
            syn.append(cs / n)
            if tok is not None:
                tokens.append(tok)
        if not syn:
            continue
        mean = {r: (sum(v) / len(v) if v else None) for r, v in by_role.items()}
        out["arms"][label] = {"functional": dict(zip([p for _, p, _ in pilot.problems], cells)),
                              "poor": mean["poor"], "adequate": mean["adequate"],
                              "syntax": sum(syn) / len(syn),
                              "tokens_per_sample": sum(tokens) / len(tokens) if tokens else None}
        table.append((label, cells, mean, sum(syn) / len(syn), out["arms"][label]["tokens_per_sample"]))

    pids = [p for _, p, _ in pilot.problems]
    md = [f"# P-series pilot, {mode}", "",
          "Functional passes per problem (of n), the mean functional pass rate over the poor and over the "
          "adequate problems, the mean syntax pass rate, and generated tokens per sample over all turns. "
          "The baselines cover VerilogEval and RTLLM (`direct`) or VerilogEval only (`direct_rules`).", "",
          "| arm | " + " | ".join(f"{p} ({r})" for _, p, r in pilot.problems) + " | poor | adequate | syntax | tokens |",
          "|---|" + "---|" * (len(pids) + 4)]
    for label, cells, mean, syn, tok in table:
        md.append(f"| {label} | " + " | ".join(cells) + f" | {rtl_eval.pct(mean['poor'])} | "
                  f"{rtl_eval.pct(mean['adequate'])} | {rtl_eval.pct(syn)} | "
                  f"{'—' if tok is None else round(tok)} |")
    arms = {a: out["arms"][a] for a in all_arms() if a in out["arms"]}
    if arms:
        md += ["", "## Main effects", "",
               "Each factor's levels, averaged over the arms that have them (all other factors mixed).", "",
               "| factor | level | arms | poor | adequate | syntax | tokens |", "|---|---|---|---|---|---|---|"]
        out["main_effects"] = {}
        for factor, levels in FACTORS:
            for level in levels:
                sel = [v for a, v in arms.items() if parse_arm(a)[factor] == level]
                if not sel:
                    continue

                def avg(key):
                    xs = [v[key] for v in sel if v[key] is not None]
                    return sum(xs) / len(xs) if xs else None
                e = {"arms": len(sel), "poor": avg("poor"), "adequate": avg("adequate"),
                     "syntax": avg("syntax"), "tokens_per_sample": avg("tokens_per_sample")}
                out["main_effects"][f"{factor}={level}"] = e
                md.append(f"| {factor} | {level} | {e['arms']} | {rtl_eval.pct(e['poor'])} | "
                          f"{rtl_eval.pct(e['adequate'])} | {rtl_eval.pct(e['syntax'])} | "
                          f"{'—' if e['tokens_per_sample'] is None else round(e['tokens_per_sample'])} |")
    os.makedirs(RUNS[mode], exist_ok=True)
    with open(os.path.join(RUNS[mode], "summary.json"), "w") as f:
        json.dump(out, f, indent=1)
    with open(os.path.join(RUNS[mode], "summary.md"), "w") as f:
        f.write("\n".join(md) + "\n")
    print("\n".join(md))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--config", default=os.path.join(HERE, "eval_config.json"))
    ap.add_argument("--tag", help="keep this run apart from the pilot's: eval/runs/pseries_<tag> "
                                  "(and pseries_greedy_<tag>), e.g. after changing a sentence")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("show")
    s.add_argument("arm")
    s.add_argument("bench")
    s.add_argument("pid")
    for name in ("build", "generate", "evaluate", "summary"):
        s = sub.add_parser(name)
        s.add_argument("--greedy", action="store_true")
        if name != "summary":
            s.add_argument("--arms", nargs="*", help="default: all 32")
            s.add_argument("--delivery", choices=DELIVERIES, help="only the arms with this delivery")
        if name == "evaluate":
            s.add_argument("--jobs", type=int, default=8)
    args = ap.parse_args()
    args.config = os.path.abspath(args.config)
    if args.tag:
        for mode in RUNS:
            RUNS[mode] += "_" + args.tag
    pilot = Pilot(args.config)
    {"show": cmd_show, "build": cmd_build, "generate": cmd_generate,
     "evaluate": cmd_evaluate, "summary": cmd_summary}[args.cmd](args, pilot)


if __name__ == "__main__":
    main()
