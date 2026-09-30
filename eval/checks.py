"""Running Icarus Verilog and Yosys on a sample: the syntax check, compile and
simulate, synthesis, and the stuck-at-0 stub the self-test uses.

Every tool is called by the absolute path in eval_config.json. A Homebrew
Icarus 13 may be on PATH, and the evaluation is pinned to Icarus 12.
"""

import json
import os
import re
import subprocess


def run(cmd, cwd, timeout):
    """(returncode or None on timeout, combined stdout+stderr, timed_out)."""
    try:
        p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=timeout, text=True, errors="replace")
        return p.returncode, p.stdout, False
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        return None, out, True


def iverilog_cmd(tools, standard):
    return [tools["iverilog"]] + list(tools["iverilog_flags"]) + [standard]


def syntax(sample_path, workdir, tools, timeouts, standard=None):
    """The syntax check: the sample alone compiles, with no -s, so a wrong
    module name is not a syntax failure (it is an interface failure, found when
    the testbench is compiled in). Returns (ok, log)."""
    cmd = iverilog_cmd(tools, standard or tools["standard"]) + ["-o", os.devnull, sample_path]
    rc, log, timed_out = run(cmd, workdir, timeouts["compile"])
    return (rc == 0 and not timed_out), log


def compile_run(compile_cmd, sim_cmd, workdir, timeouts):
    """Compile, then simulate if that succeeded. Returns (status, log,
    timed_out): status is "compile" when compilation failed, else the
    simulator's return code (None if it timed out). The log is the compile
    log, then the simulation's output after a separator line."""
    rc, clog, ctimeout = run(compile_cmd, workdir, timeouts["compile"])
    if ctimeout or rc != 0:
        return "compile", clog, ctimeout
    rc, slog, stimeout = run(sim_cmd, workdir, timeouts["simulate"])
    return rc, clog + "\n--- simulation ---\n" + slog, stimeout


def synth(sample_path, top, workdir, tools, timeouts):
    """Yosys synthesis, reported beside the ladders. The outcome is one of
    'pass', 'frontend_error' (Yosys cannot read the file; Icarus may still
    accept it, since Yosys supports only part of SystemVerilog), 'top_missing'
    (no module named `top`), 'synth_error', or 'timeout'."""
    y = tools["yosys"]
    rc, log, t = run([y, "-q", "-p", f"read_verilog -sv {sample_path}"], workdir, timeouts["synth"])
    if t:
        return "timeout", log
    if rc != 0:
        return "frontend_error", log
    rc, log, t = run([y, "-q", "-p", f"read_verilog -sv {sample_path}; synth -top {top}"], workdir,
                     timeouts["synth"])
    if t:
        return "timeout", log
    if rc != 0:
        if re.search(r"Module `\\?%s' not found|does not exist|top module .* not found" % re.escape(top),
                     log, re.I):
            return "top_missing", log
        return "synth_error", log
    return "pass", log


def stub(golden_path, top, out_path, workdir, tools, timeouts):
    """A module with the golden's ports and every output tied to 0. A
    testbench that passes it cannot tell a working design from a dead one.

    Written from the port list alone (Yosys reads the golden and reports its
    ports), not by gutting the golden: deleting a module's cells leaves its
    plain connections, so a design that is only `assign out = in` or a
    constant came through unchanged and "passed". Parameters are not carried
    over. Returns (ok, log)."""
    js = os.path.join(workdir, "ports.json")
    script = f"read_verilog -sv {golden_path}; hierarchy -top {top}; proc; write_json {js}"
    rc, log, t = run([tools["yosys"], "-q", "-p", script], workdir, timeouts["synth"])
    if rc != 0 or t or not os.path.exists(js):
        return False, log
    with open(js) as f:
        mods = json.load(f)["modules"]
    mod = mods.get(top) or next((m for name, m in mods.items() if name.endswith(top)), None)
    if mod is None:
        return False, log + f"\nno module {top} in the JSON"
    ports, decls, assigns = [], [], []
    for name, p in mod["ports"].items():
        w = len(p["bits"])
        rng = f"[{w - 1}:0] " if w > 1 else ""
        ports.append(name)
        decls.append(f"  {p['direction']} {rng}{name};")
        if p["direction"] == "output":
            assigns.append(f"  assign {name} = {{{w}{{1'b0}}}};")
    text = f"module {top}({', '.join(ports)});\n" + "\n".join(decls + assigns) + "\nendmodule\n"
    with open(out_path, "w") as f:
        f.write(text)
    return True, log
