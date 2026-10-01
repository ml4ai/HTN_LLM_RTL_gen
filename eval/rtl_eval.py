#!/usr/bin/env python3
"""The RTL evaluation harness (LLM_RTL_code_generation.md 6.1.3).

    rtl_eval.py selftest --bench B [--out FILE]
    rtl_eval.py generate --bench B --run-dir D (--n N | --greedy) [--problems ...] [--limit K]
    rtl_eval.py evaluate --run-dir D [--bench B] [--jobs J]
    rtl_eval.py report   --run-dir D [--bench B] [--selftest FILE]
    rtl_eval.py breakdown --run-dir D [--greedy-run G] [--bench B]
    rtl_eval.py compare  --a D1 --b D2 --bench B [--selftest FILE]

B is verilog-eval-v2 or rtllm-2. A run directory holds one arm's samples for
one or more benchmarks: D/<bench>/<problem>/request.json, the replies and
gen.json from the executor, the extracted sample_XX.v, and D/<bench>/
results.jsonl from evaluate. Every setting comes from eval_config.json and
executor/executor_config.json; nothing on the command line overrides them
except which problems to run.

Single-pass by construction: generation never sees a check's result, and the
checks run only after every reply is final (6.1, boundary case xi).
"""

import argparse
import concurrent.futures as cf
import datetime
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import benchmarks  # noqa: E402
import checks  # noqa: E402
import passk  # noqa: E402

CATEGORIES = ["pass", "abstention", "syntax_error", "interface_error", "simulation_fail", "timeout"]


def load_config(path):
    with open(path) as f:
        cfg = json.load(f)
    tools = dict(cfg["tools"])
    for t in ("iverilog", "vvp", "yosys"):
        if not os.path.exists(tools[t]):
            sys.exit(f"error: {t} not found at {tools[t]} (eval_config.json)")
    return cfg, tools


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git_head(path):
    try:
        return subprocess.run(["git", "-C", path, "rev-parse", "HEAD"], capture_output=True,
                              text=True, check=True).stdout.strip()
    except Exception:
        return None


def bench_adapter(cfg, name):
    bcfg = cfg["benchmarks"][name]
    path = os.path.expanduser(bcfg["path"])
    head = git_head(path)
    if head != bcfg["commit"]:
        sys.exit(f"error: {path} is at {head}, but eval_config.json pins {bcfg['commit']}")
    return benchmarks.adapter(name, bcfg)


def select(ad, problems, limit):
    ids = ad.problem_ids()
    if problems:
        unknown = [p for p in problems if p not in ids]
        if unknown:
            sys.exit(f"error: unknown problems {unknown}")
        ids = [p for p in ids if p in problems]
    return ids[:limit] if limit else ids


def now():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


# ---------------------------------------------------------------------------
# selftest

def cmd_selftest(args, cfg, tools):
    """Before any run counts: every golden reference must pass the syntax and
    functional checks, and a stuck-at-0 stub of it must fail the functional
    one. A problem whose golden fails is excluded from reports; one whose stub
    passes has a testbench too weak to trust, and is flagged."""
    ad = bench_adapter(cfg, args.bench)
    ids = select(ad, args.problems, args.limit)
    to = cfg["timeouts_s"]

    def one(pid):
        r = {"pid": pid}
        try:
            p = ad.load(pid)
        except Exception as e:
            r["load_error"] = str(e)
            return r
        with tempfile.TemporaryDirectory() as w:
            g = os.path.join(w, "golden.v")
            with open(g, "w") as f:
                f.write(p.golden)
            r["golden_syntax"], _ = checks.syntax(g, w, tools, to)
            r["golden_syntax_2005"], _ = checks.syntax(g, w, tools, to, tools["diagnostic_standard"])
            t0 = time.time()
            with tempfile.TemporaryDirectory() as w2:
                ok, code, log = ad.functional(p, g, w2, tools, to)
            r["golden_functional"], r["golden_code"] = ok, code
            r["golden_sim_s"] = round(time.time() - t0, 2)
            if not ok:
                r["golden_log_tail"] = log[-1500:]
            r["golden_synth"], _ = checks.synth(g, p.top, w, tools, to)
            s = os.path.join(w, "stub.v")
            sok, slog = checks.stub(g, p.top, s, w, tools, to)
            r["stub_built"] = sok
            if sok:
                with tempfile.TemporaryDirectory() as w3:
                    r["stub_functional"], r["stub_code"], _ = ad.functional(p, s, w3, tools, to)
            else:
                r["stub_log_tail"] = slog[-800:]
        return r

    results = []
    with cf.ThreadPoolExecutor(args.jobs) as ex:
        for r in ex.map(one, ids):
            results.append(r)
            flag = ("LOAD ERROR" if "load_error" in r else
                    "GOLDEN FAILS" if not r["golden_functional"] else
                    "weak testbench (stub passes)" if r.get("stub_functional") else "ok")
            print(f"  {r['pid']:40s} {flag}", flush=True)

    summary = {
        "problems": len(results),
        "load_errors": [r["pid"] for r in results if "load_error" in r],
        "golden_fails": [r["pid"] for r in results if "load_error" not in r and not r["golden_functional"]],
        "golden_syntax_fails": [r["pid"] for r in results if "load_error" not in r and not r["golden_syntax"]],
        "golden_not_2005": [r["pid"] for r in results if "load_error" not in r and not r["golden_syntax_2005"]],
        "golden_not_synthesizable": [r["pid"] for r in results
                                     if "load_error" not in r and r["golden_synth"] != "pass"],
        "stub_not_built": [r["pid"] for r in results if "load_error" not in r and not r.get("stub_built")],
        "weak_testbenches": [r["pid"] for r in results if r.get("stub_functional")],
    }
    out = {"bench": args.bench, "created": now(), "eval_config_sha256": sha256_file(args.config),
           "commit": cfg["benchmarks"][args.bench]["commit"], "summary": summary, "results": results}
    path = args.out or os.path.join(HERE, "runs", f"selftest_{args.bench}.json")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(out, f, indent=1)
    print(json.dumps(summary, indent=1))
    print(f"self-test written to {path}")


# ---------------------------------------------------------------------------
# generate

def cmd_generate(args, cfg, tools):
    if args.rules:
        # sv-generate --rules: VerilogEval's rules suffix on, for the rules-on
        # direct arm (LLM_RTL_code_generation.md 6.1.7). The frozen config
        # keeps sv-generate's default; run.json records the override.
        if args.bench != "verilog-eval-v2":
            sys.exit("error: --rules is VerilogEval v2's prompt option only")
        cfg["benchmarks"][args.bench] = dict(cfg["benchmarks"][args.bench], rules=True)
    ad = bench_adapter(cfg, args.bench)
    ids = select(ad, args.problems, args.limit)
    n = 1 if args.greedy else (args.n or cfg["ladder"]["n"])
    mode = "greedy" if args.greedy else "sampled"
    exe = os.path.join(ROOT, cfg["executor"]["binary"])
    ecfg = os.path.join(ROOT, cfg["executor"]["config"])
    if not os.path.exists(exe):
        sys.exit(f"error: {exe} not built (build/executor, see executor/README.md)")

    os.makedirs(args.run_dir, exist_ok=True)
    meta_path = os.path.join(args.run_dir, "run.json")
    meta = json.load(open(meta_path)) if os.path.exists(meta_path) else {
        "created": now(), "arm": args.arm, "mode": mode, "n": n, "benchmarks": {}}
    if (meta["arm"], meta["mode"], meta["n"]) != (args.arm, mode, n):
        sys.exit(f"error: {args.run_dir} holds arm={meta['arm']} mode={meta['mode']} n={meta['n']}; "
                 f"use another run directory")
    prev = meta["benchmarks"].get(args.bench)
    if prev and prev["settings"] != cfg["benchmarks"][args.bench]:
        sys.exit(f"error: {args.run_dir} holds {args.bench} generated with other settings "
                 f"({prev['settings']}); use another run directory")
    meta["benchmarks"][args.bench] = {"commit": cfg["benchmarks"][args.bench]["commit"],
                                      "settings": cfg["benchmarks"][args.bench]}
    meta["eval_config_sha256"] = sha256_file(args.config)
    meta["executor_config_sha256"] = sha256_file(ecfg)
    with open(meta_path, "w") as f:
        json.dump(meta, f, indent=1)

    t_start, done = time.time(), 0
    for i, pid in enumerate(ids):
        p = ad.load(pid)
        d = os.path.join(args.run_dir, args.bench, pid)
        os.makedirs(d, exist_ok=True)
        req = os.path.join(d, "request.json")
        body = json.dumps({"system": p.system, "user": p.user}, indent=1)
        gen = os.path.join(d, "gen.json")
        if os.path.exists(gen) and os.path.exists(req) and open(req).read() == body:
            g = json.load(open(gen))
            if g.get("n") == n and g.get("mode") == mode:
                continue  # done in an earlier invocation
        with open(req, "w") as f:
            f.write(body)
        cmd = [exe, "--request", req, "--out-dir", d, "--config", ecfg]
        cmd += ["--greedy"] if args.greedy else ["--n", str(n)]
        t0 = time.time()
        rc = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        if rc.returncode != 0:
            sys.exit(f"error: executor failed on {pid}:\n{rc.stderr[-2000:]}")
        done += 1
        per = (time.time() - t_start) / done
        left = len(ids) - i - 1
        print(f"  [{i + 1}/{len(ids)}] {pid}: {time.time() - t0:.0f} s"
              f"  (about {per * left / 60:.0f} min left)", flush=True)
    print(f"generated {done} problem(s) in {(time.time() - t_start) / 60:.1f} min; "
          f"{len(ids) - done} already present")


# ---------------------------------------------------------------------------
# evaluate

def evaluate_sample(ad, p, d, idx, tools, to):
    resp_path = os.path.join(d, f"sample_{idx:02d}.response.txt")
    with open(resp_path, errors="replace") as f:
        resp = f.read()
    code = ad.extract(resp, p.top)
    r = {"pid": p.pid, "sample": idx}
    v = os.path.join(d, f"sample_{idx:02d}.v")
    if not benchmarks.has_module(code):
        if os.path.exists(v):
            os.remove(v)  # a stale extraction must not pass for this one
        r.update(category="abstention", syntax=False, functional=False)
        return r
    with open(v, "w") as f:
        f.write(code)
    with tempfile.TemporaryDirectory() as w:
        ok, log = checks.syntax(v, w, tools, to)
        r["syntax"] = ok
        if not ok:
            r.update(category="syntax_error", functional=False, syntax_log=log[-600:])
            return r
        r["syntax_2005"], _ = checks.syntax(v, w, tools, to, tools["diagnostic_standard"])
        r["synth"], _ = checks.synth(v, p.top, w, tools, to)
    with tempfile.TemporaryDirectory() as w:
        passed, bcode, log = ad.functional(p, v, w, tools, to)
    r["functional"], r["bench_code"] = passed, bcode
    if passed:
        r["category"] = "pass"
    elif bcode == "T":
        r["category"] = "timeout"
    elif bcode in ("C", "S", "e", "0", "w", "m", "p", "c"):
        # The sample compiled on its own, so a compile failure now comes from
        # binding it to the testbench.
        r["category"] = "interface_error"
    else:
        r["category"] = "simulation_fail"
    if not passed:
        r["functional_log"] = log[-600:]
    return r


def cmd_evaluate(args, cfg, tools):
    meta = json.load(open(os.path.join(args.run_dir, "run.json")))
    to = cfg["timeouts_s"]
    for bench in ([args.bench] if args.bench else meta["benchmarks"]):
        ad = bench_adapter(cfg, bench)
        bdir = os.path.join(args.run_dir, bench)
        jobs = []
        for pid in sorted(os.listdir(bdir)):
            d = os.path.join(bdir, pid)
            gen = os.path.join(d, "gen.json")
            if not os.path.exists(gen):
                print(f"  {pid}: no gen.json (generation unfinished), skipped")
                continue
            g = json.load(open(gen))
            p = ad.load(pid)
            for i, s in enumerate(g["samples"]):
                jobs.append((p, d, i, s))
        results = []
        with cf.ThreadPoolExecutor(args.jobs) as ex:
            futs = {ex.submit(evaluate_sample, ad, p, d, i, tools, to): (p, i, s) for p, d, i, s in jobs}
            for k, fut in enumerate(cf.as_completed(futs), 1):
                p, i, s = futs[fut]
                r = fut.result()
                r["finish_reason"] = s["finish_reason"]
                r["generated_tokens"] = s["generated_tokens"]
                results.append(r)
                if k % 50 == 0 or k == len(jobs):
                    print(f"  {bench}: {k}/{len(jobs)} samples checked", flush=True)
        results.sort(key=lambda r: (r["pid"], r["sample"]))
        with open(os.path.join(bdir, "results.jsonl"), "w") as f:
            for r in results:
                f.write(json.dumps(r) + "\n")
        print(f"{bench}: {len(results)} samples evaluated -> {bdir}/results.jsonl")


# ---------------------------------------------------------------------------
# report and compare

def load_results(run_dir, bench, selftest_path):
    meta = json.load(open(os.path.join(run_dir, "run.json")))
    path = os.path.join(run_dir, bench, "results.jsonl")
    if not os.path.exists(path):
        sys.exit(f"error: {path} missing; run evaluate first")
    by_pid = {}
    for line in open(path):
        r = json.loads(line)
        by_pid.setdefault(r["pid"], []).append(r)
    excluded, flags = [], {}
    st = selftest_path or os.path.join(HERE, "runs", f"selftest_{bench}.json")
    if os.path.exists(st):
        s = json.load(open(st))["summary"]
        excluded = sorted(set(s["golden_fails"]) | set(s["load_errors"]))
        for pid in excluded:
            by_pid.pop(pid, None)
        flags = {"stub_passes": [p for p in s["weak_testbenches"] if p in by_pid],
                 "golden_not_synthesizable": [p for p in s["golden_not_synthesizable"] if p in by_pid]}
    else:
        print(f"warning: no self-test at {st}; no problems excluded", file=sys.stderr)
        st = None
    n = meta["n"]
    short = [pid for pid, rs in by_pid.items() if len(rs) != n]
    if short:
        sys.exit(f"error: problems without all {n} samples: {short[:5]}")
    return meta, by_pid, excluded, st, flags


def summarize(meta, by_pid, cfg, flags=None):
    n = meta["n"]
    ks = [k for k in cfg["ladder"]["k"] if k <= n]
    pids = sorted(by_pid)
    c_syn = [sum(r["syntax"] for r in by_pid[p]) for p in pids]
    c_func = [sum(r["functional"] for r in by_pid[p]) for p in pids]
    samples = [r for p in pids for r in by_pid[p]]
    valid = [r for r in samples if r["syntax"]]
    syn = passk.ladder(c_syn, n, ks)
    func = passk.ladder(c_func, n, ks)
    out = {
        "problems": len(pids), "n": n, "mode": meta["mode"], "arm": meta["arm"],
        "syntax_pass_at_k": syn, "functional_pass_at_k": func,
        "conditional_functional_rate": (func[1] / syn[1]) if syn.get(1) else None,
        "failure_breakdown": {c: sum(r["category"] == c for r in samples) / len(samples) for c in CATEGORIES},
        "synthesizability_of_syntax_valid": {
            o: (sum(r.get("synth") == o for r in valid) / len(valid)) if valid else None
            for o in ("pass", "frontend_error", "synth_error", "top_missing", "timeout")},
        "not_verilog_2005_of_syntax_valid": (sum(not r.get("syntax_2005", True) for r in valid) / len(valid))
        if valid else None,
        "truncated_at_max_new_tokens": sum(r["finish_reason"] == "length" for r in samples) / len(samples),
        "per_problem": {p: {"c_syn": cs, "c_func": cf_} for p, cs, cf_ in zip(pids, c_syn, c_func)},
    }
    # Robustness: the functional ladder without the problems whose testbench a
    # stuck-at-0 stub passes, where a pass may say little. Some of those are
    # designs whose correct output is constant 0, so this is a check, not a
    # replacement for the ladder above.
    weak = set((flags or {}).get("stub_passes", []))
    if weak:
        kept = [c for p, c in zip(pids, c_func) if p not in weak]
        out["functional_pass_at_k_without_stub_passing_problems"] = passk.ladder(kept, n, ks)
    return out


def pct(x):
    return "—" if x is None else f"{100 * x:.1f}"


def cmd_report(args, cfg, tools):
    meta = json.load(open(os.path.join(args.run_dir, "run.json")))
    report = {"run_dir": os.path.abspath(args.run_dir), "created": now(), "run": meta, "benchmarks": {}}
    md = [f"# RTL evaluation report", "",
          f"Run `{args.run_dir}`: arm **{meta['arm']}**, {meta['mode']}, n = {meta['n']}.", ""]
    for bench in ([args.bench] if args.bench else meta["benchmarks"]):
        _, by_pid, excluded, st, flags = load_results(args.run_dir, bench, args.selftest)
        s = summarize(meta, by_pid, cfg, flags)
        s["excluded_by_selftest"] = excluded
        s["selftest"] = st
        s["selftest_flags"] = flags
        report["benchmarks"][bench] = s
        ks = sorted(s["syntax_pass_at_k"])
        md += [f"## {bench}", "",
               f"{s['problems']} problems" + (f"; excluded by the self-test (golden fails): {', '.join(excluded)}"
                                               if excluded else "") + ".", "",
               "| | " + " | ".join(f"pass@{k}" for k in ks) + " |",
               "|---|" + "---|" * len(ks),
               "| Syntax | " + " | ".join(pct(s["syntax_pass_at_k"][k]) for k in ks) + " |",
               "| Functional | " + " | ".join(pct(s["functional_pass_at_k"][k]) for k in ks) + " |"]
        if "functional_pass_at_k_without_stub_passing_problems" in s:
            md += ["| Functional, without problems a dead stub passes | " + " | ".join(
                pct(s["functional_pass_at_k_without_stub_passing_problems"][k]) for k in ks) + " |"]
        md += ["",
               f"Conditional functional rate (functional pass@1 ÷ syntax pass@1): "
               f"{pct(s['conditional_functional_rate'])}%.", "",
               "Failure breakdown, as a share of all samples: " +
               ", ".join(f"{c.replace('_', ' ')} {pct(v)}%" for c, v in s["failure_breakdown"].items()) + ".", "",
               "Synthesizability (Yosys), as a share of syntax-valid samples: " +
               ", ".join(f"{o.replace('_', ' ')} {pct(v)}%" for o, v in s["synthesizability_of_syntax_valid"].items())
               + ".", "",
               f"Not Verilog-2005 (fail `-g2005`) among syntax-valid samples: "
               f"{pct(s['not_verilog_2005_of_syntax_valid'])}%. "
               f"Replies cut off at max_new_tokens: {pct(s['truncated_at_max_new_tokens'])}%.", ""]
        if flags.get("stub_passes"):
            md += [f"Testbenches a stuck-at-0 stub passes (review; some designs are legitimately constant 0): "
                   f"{', '.join(flags['stub_passes'])}.", ""]
        if flags.get("golden_not_synthesizable"):
            md += [f"Golden references Yosys does not synthesize either: "
                   f"{', '.join(flags['golden_not_synthesizable'])}.", ""]
    with open(os.path.join(args.run_dir, "report.json"), "w") as f:
        json.dump(report, f, indent=1)
    with open(os.path.join(args.run_dir, "report.md"), "w") as f:
        f.write("\n".join(md) + "\n")
    print("\n".join(md))


def cmd_breakdown(args, cfg, tools):
    """The report split by problem category (benchmarks.py, category()): the
    design class read off the reference (FSM / sequential / combinational),
    the form of the spec, and the benchmark's own grouping."""
    meta = json.load(open(os.path.join(args.run_dir, "run.json")))
    n = meta["n"]
    ks = [k for k in cfg["ladder"]["k"] if k <= n]
    out = {"run_dir": args.run_dir, "greedy_run": args.greedy_run, "created": now(), "benchmarks": {}}
    md = ["# Failure breakdown by problem category", "",
          f"Run `{args.run_dir}` ({meta['arm']}, {meta['mode']}, n = {n})"
          + (f"; greedy line from `{args.greedy_run}`" if args.greedy_run else "") + ".", ""]
    for bench in ([args.bench] if args.bench else meta["benchmarks"]):
        ad = bench_adapter(cfg, bench)
        _, by_pid, excluded, _, _ = load_results(args.run_dir, bench, args.selftest)
        greedy = {}
        if args.greedy_run:
            _, g, _, _, _ = load_results(args.greedy_run, bench, args.selftest)
            greedy = {pid: rs[0]["functional"] for pid, rs in g.items()}
        cats = {pid: ad.category(ad.load(pid)) for pid in by_pid}
        dims = ["class", "spec"] if bench == "verilog-eval-v2" else ["class", "group", "subgroup"]
        out["benchmarks"][bench] = {"categories": cats, "by": {}}
        md += [f"## {bench}", ""]
        for dim in dims:
            groups = {}
            for pid in by_pid:
                groups.setdefault(cats[pid][dim], []).append(pid)
            rows = []
            for key, pids in sorted(groups.items(), key=lambda kv: (-len(kv[1]), kv[0])):
                samples = [r for p_ in pids for r in by_pid[p_]]
                c_syn = [sum(r["syntax"] for r in by_pid[p_]) for p_ in pids]
                c_func = [sum(r["functional"] for r in by_pid[p_]) for p_ in pids]
                syn = passk.ladder(c_syn, n, [1])[1]
                func = passk.ladder(c_func, n, ks)
                mix = {c: sum(r["category"] == c for r in samples) / len(samples) for c in CATEGORIES}
                row = {"problems": len(pids), "syntax_pass@1": syn,
                       "functional_pass_at_k": func,
                       "conditional": (func[1] / syn) if syn else None,
                       "failure_mix": mix,
                       "never_solved": sorted(p_ for p_, c in zip(pids, c_func) if c == 0)}
                if greedy:
                    row["greedy_functional"] = sum(greedy.get(p_, False) for p_ in pids) / len(pids)
                rows.append((key, row))
            out["benchmarks"][bench]["by"][dim] = dict(rows)
            md += [f"### By {dim}", "",
                   "| " + dim + " | problems | syntax @1 | " + " | ".join(f"functional @{k}" for k in ks)
                   + (" | greedy functional" if greedy else "") + " | conditional | sim fail | syntax err | interface err | never solved |",
                   "|---|---|---|" + "---|" * len(ks) + ("---|" if greedy else "") + "---|---|---|---|---|"]
            for key, r in rows:
                md.append(f"| {key} | {r['problems']} | {pct(r['syntax_pass@1'])} | "
                          + " | ".join(pct(r["functional_pass_at_k"][k]) for k in ks)
                          + (f" | {pct(r['greedy_functional'])}" if greedy else "")
                          + f" | {pct(r['conditional'])} | {pct(r['failure_mix']['simulation_fail'])}"
                          f" | {pct(r['failure_mix']['syntax_error'])} | {pct(r['failure_mix']['interface_error'])}"
                          f" | {len(r['never_solved'])} |")
            md.append("")
        never = sorted(p_ for p_ in by_pid if sum(r["functional"] for r in by_pid[p_]) == 0)
        md += [f"Never solved in {n} samples ({len(never)}): "
               + ", ".join(f"{p_} ({cats[p_]['class']})" for p_ in never) + ".", ""]
        audit, audit_md = reset_audit(ad, bench, by_pid, cats, args, cfg, tools)
        out["benchmarks"][bench]["reset_audit"] = audit
        md += audit_md
    with open(os.path.join(args.run_dir, "breakdown.json"), "w") as f:
        json.dump(out, f, indent=1)
    with open(os.path.join(args.run_dir, "breakdown.md"), "w") as f:
        f.write("\n".join(md) + "\n")
    print("\n".join(md))


def resim_synchronous(ad, p, path, tools, to):
    """Whether a failing sample passes once its asynchronous reset is made
    synchronous (benchmarks.make_reset_synchronous); None if there was nothing
    to rewrite. A diagnostic; the scored result is never changed."""
    with open(path) as f:
        new, cut = benchmarks.make_reset_synchronous(f.read())
    if not cut:
        return None
    with tempfile.TemporaryDirectory() as w:
        v = os.path.join(w, os.path.basename(path))
        with open(v, "w") as f:
            f.write(new)
        passed, _, _ = ad.functional(p, v, w, tools, to)
    return passed


def reset_audit(ad, bench, by_pid, cats, args, cfg, tools):
    """Does each syntax-valid sample use the reset its spec asks for
    (benchmarks.reset_style, benchmarks.uses_async_reset)? For specs that ask
    for a synchronous reset, also re-simulate the failing samples with the
    reset made synchronous, unless --no-resim: the functional pass@1 the
    reset style alone costs, by design class."""
    n = len(next(iter(by_pid.values())))
    problems = {pid: ad.load(pid) for pid in by_pid}
    style = {pid: benchmarks.reset_style(p.extra["spec"]) for pid, p in problems.items()}
    rows, jobs = {}, []
    for pid in sorted(by_pid):
        if style[pid] not in ("synchronous", "asynchronous"):
            continue
        valid = [r for r in by_pid[pid] if r["syntax"]]
        paths = {r["sample"]: os.path.join(args.run_dir, bench, pid, f"sample_{r['sample']:02d}.v") for r in valid}
        async_ = {i for i, path in paths.items() if benchmarks.uses_async_reset(open(path).read())}
        wrong = async_ if style[pid] == "synchronous" else set(paths) - async_
        rows[pid] = {"spec_reset": style[pid], "class": cats[pid]["class"],
                     "functional": sum(r["functional"] for r in by_pid[pid]),
                     "syntax_valid": len(valid), "wrong_reset_style": len(wrong)}
        if style[pid] == "synchronous" and not args.no_resim:
            rows[pid]["passes_once_synchronous"] = 0
            jobs += [(pid, paths[r["sample"]]) for r in valid if not r["functional"] and r["sample"] in async_]
    if jobs:
        with cf.ThreadPoolExecutor(args.jobs) as ex:
            for (pid, _), ok in zip(jobs, ex.map(lambda j: resim_synchronous(
                    ad, problems[j[0]], j[1], tools, cfg["timeouts_s"]), jobs)):
                rows[pid]["passes_once_synchronous"] += bool(ok)

    out = {"problems": rows}
    md = ["### Reset audit", "",
          "Specs that name a reset style, and the syntax-valid samples that use the other one: an "
          "asynchronous reset is a clock-plus-one-edge sensitivity list, `@(posedge clk or posedge reset)`.", ""]
    for want in ("synchronous", "asynchronous"):
        rs = {pid: r for pid, r in rows.items() if r["spec_reset"] == want}
        if not rs:
            continue
        valid = sum(r["syntax_valid"] for r in rs.values())
        wrong = sum(r["wrong_reset_style"] for r in rs.values())
        out[want] = {"problems": len(rs), "syntax_valid": valid, "wrong_reset_style": wrong}
        md.append(f"- **{want.capitalize()} reset asked for** ({len(rs)} problems): "
                  f"{wrong} of {valid} syntax-valid samples ({pct(wrong / valid if valid else None)}%) "
                  f"use the other style.")
    md.append("")
    sync = {pid: r for pid, r in rows.items() if r["spec_reset"] == "synchronous"}
    if sync and not args.no_resim:
        fixed = sum(r["passes_once_synchronous"] for r in sync.values())
        md += [f"Failing samples on synchronous-reset specs that pass once their reset is made synchronous: "
               f"**{fixed}**. Functional pass@1 if they had (a diagnostic; the scores above are unchanged):", "",
               "| class | problems | functional @1 | with synchronous reset | never solved | with synchronous reset |",
               "|---|---|---|---|---|---|"]
        by_class = {}
        for pid, rs in by_pid.items():
            base = sum(r["functional"] for r in rs)
            extra = rows.get(pid, {}).get("passes_once_synchronous", 0)
            for key in (cats[pid]["class"], "all"):
                c = by_class.setdefault(key, [0, 0.0, 0.0, 0, 0])
                c[0] += 1
                c[1] += base / n
                c[2] += (base + extra) / n
                c[3] += base == 0
                c[4] += base + extra == 0
        out["counterfactual_by_class"] = {}
        for key, (k, b, f, z0, z1) in sorted(by_class.items(), key=lambda kv: (kv[0] == "all", -kv[1][0])):
            out["counterfactual_by_class"][key] = {"problems": k, "functional_pass@1": b / k,
                                                   "with_synchronous_reset": f / k,
                                                   "never_solved": z0, "never_solved_with_synchronous_reset": z1}
            md.append(f"| {key} | {k} | {pct(b / k)} | {pct(f / k)} | {z0} | {z1} |")
        md += ["", "| problem | class | functional | async-reset samples | pass once synchronous |", "|---|---|---|---|---|"]
        md += [f"| {pid} | {r['class']} | {r['functional']}/{n} | {r['wrong_reset_style']}/{r['syntax_valid']} | "
               f"{r['passes_once_synchronous']} |" for pid, r in sorted(sync.items())]
        md.append("")
    return out, md


def cmd_compare(args, cfg, tools):
    ma, ra, exa, _, _ = load_results(args.a, args.bench, args.selftest)
    mb, rb, exb, _, _ = load_results(args.b, args.bench, args.selftest)
    if ma["n"] != mb["n"] or ma["mode"] != mb["mode"]:
        sys.exit("error: the two runs differ in n or mode")
    n = ma["n"]
    pids = sorted(set(ra) & set(rb))
    rc = cfg["report"]
    ks = [k for k in cfg["ladder"]["k"] if k <= n]

    rows = []
    for p in pids:
        row = {}
        for arm, rs in (("a", ra[p]), ("b", rb[p])):
            cs = sum(r["syntax"] for r in rs)
            cf_ = sum(r["functional"] for r in rs)
            row[arm] = (cs, cf_)
        rows.append(row)

    def terms(sample):
        syn = con = df = 0.0
        for row in sample:
            (csa, cfa), (csb, cfb) = row["a"], row["b"]
            s_a, s_b = csa / n, csb / n
            q_a = cfa / csa if csa else None
            q_b = cfb / csb if csb else None
            st, ct = passk.split(s_a, q_a, s_b, q_b)
            syn += st
            con += ct
            df += cfa / n - cfb / n
        m = len(sample)
        return df / m, syn / m, con / m

    point = terms(rows)
    ci = [passk.bootstrap_ci(rows, lambda smp, j=j: terms(smp)[j], rc["bootstrap_resamples"],
                             rc["bootstrap_seed"], rc["confidence"]) for j in range(3)]
    out = {"a": os.path.abspath(args.a), "b": os.path.abspath(args.b), "bench": args.bench,
           "problems": len(pids), "n": n,
           "functional_pass1_difference": {"point": point[0], "ci": ci[0]},
           "syntax_term": {"point": point[1], "ci": ci[1]},
           "conditional_term": {"point": point[2], "ci": ci[2]}}
    lad = {}
    for which, idx in (("syntax", 0), ("functional", 1)):
        for k in ks:
            def dk(sample, k=k, idx=idx):
                return sum(passk.pass_at_k(n, row["a"][idx], k) - passk.pass_at_k(n, row["b"][idx], k)
                           for row in sample) / len(sample)
            lad[f"{which}_pass@{k}"] = {"point": dk(rows), "ci": passk.bootstrap_ci(
                rows, dk, rc["bootstrap_resamples"], rc["bootstrap_seed"], rc["confidence"])}
    out["ladder_differences"] = lad
    if n == 1:
        for which, idx in (("syntax", 0), ("functional", 1)):
            b_ = sum(1 for row in rows if row["a"][idx] and not row["b"][idx])
            c_ = sum(1 for row in rows if row["b"][idx] and not row["a"][idx])
            out[f"mcnemar_{which}"] = {"a_only": b_, "b_only": c_, "p": passk.mcnemar_exact(b_, c_)}
    print(json.dumps(out, indent=1))
    if args.out:
        with open(args.out, "w") as f:
            json.dump(out, f, indent=1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--config", default=os.path.join(HERE, "eval_config.json"))
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("selftest")
    s.add_argument("--bench", required=True)
    s.add_argument("--problems", nargs="*")
    s.add_argument("--limit", type=int)
    s.add_argument("--jobs", type=int, default=8)
    s.add_argument("--out")

    s = sub.add_parser("generate")
    s.add_argument("--bench", required=True)
    s.add_argument("--run-dir", required=True)
    g = s.add_mutually_exclusive_group()
    g.add_argument("--n", type=int)
    g.add_argument("--greedy", action="store_true")
    s.add_argument("--problems", nargs="*")
    s.add_argument("--limit", type=int)
    s.add_argument("--arm", default="direct")
    s.add_argument("--rules", action="store_true",
                   help="VerilogEval v2 only: append sv-generate's rules suffix to the prompt")

    s = sub.add_parser("evaluate")
    s.add_argument("--run-dir", required=True)
    s.add_argument("--bench")
    s.add_argument("--jobs", type=int, default=8)

    s = sub.add_parser("report")
    s.add_argument("--run-dir", required=True)
    s.add_argument("--bench")
    s.add_argument("--selftest")

    s = sub.add_parser("breakdown")
    s.add_argument("--run-dir", required=True)
    s.add_argument("--greedy-run")
    s.add_argument("--bench")
    s.add_argument("--selftest")
    s.add_argument("--jobs", type=int, default=8)
    s.add_argument("--no-resim", action="store_true",
                   help="skip re-simulating failing samples with their reset made synchronous")

    s = sub.add_parser("compare")
    s.add_argument("--a", required=True)
    s.add_argument("--b", required=True)
    s.add_argument("--bench", required=True)
    s.add_argument("--selftest")
    s.add_argument("--out")

    args = ap.parse_args()
    # Absolute from here on: the tools run inside temporary working
    # directories, where a relative sample path names nothing, and every
    # sample then "failed" its syntax check with "No such file or directory".
    for attr in ("run_dir", "greedy_run", "a", "b", "out", "selftest"):
        if getattr(args, attr, None):
            setattr(args, attr, os.path.abspath(getattr(args, attr)))
    cfg, tools = load_config(args.config)
    {"selftest": cmd_selftest, "generate": cmd_generate, "evaluate": cmd_evaluate,
     "report": cmd_report, "breakdown": cmd_breakdown, "compare": cmd_compare}[args.cmd](args, cfg, tools)


if __name__ == "__main__":
    main()
