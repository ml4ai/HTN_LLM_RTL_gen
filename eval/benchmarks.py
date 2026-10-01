"""Benchmark adapters: what a problem is, how it is prompted, how code is cut
out of a reply, what the golden reference is, and how a sample is tested.

Each adapter follows its benchmark's own harness where the benchmark defines
one, so that "functional pass" means what the benchmark means by it:

  VerilogEval v2 (NVlabs/verilog-eval, spec-to-RTL): the prompt template and
    the code extraction of scripts/sv-generate, and the pass/fail
    classification of scripts/sv-iv-analyze, ported line for line.
  RTLLM 2.0 (hkust-zhiyao/RTLLM): the design description as the prompt, and
    the pass rule of auto_run.py (the simulation prints "Pass" or "pass"),
    with Icarus Verilog in place of the proprietary VCS its Makefiles call.
"""

import os
import re
import shutil
from dataclasses import dataclass, field

from checks import compile_run, iverilog_cmd

MODULE_RE = re.compile(r"\bmodule\s+[A-Za-z_]\w*")


@dataclass
class Problem:
    bench: str
    pid: str
    top: str                      # the module name the testbench instantiates
    system: str                   # the system message
    user: str                     # the user message
    golden: str                   # golden reference source, top renamed to `top`
    extra: dict = field(default_factory=dict)


def design_class(reference, name=""):
    """FSM, sequential or combinational, read off a reference design: clocked if
    it has a posedge/negedge, an FSM if clocked with a state register driven
    through a case statement, or so named. Deterministic, so a breakdown by
    class is reproducible rather than hand-labelled."""
    clocked = bool(re.search(r"\b(posedge|negedge)\b", reference))
    named_fsm = bool(re.search(r"fsm|lemmings", name, re.I))
    stateful = bool(re.search(r"\b(state|next_state|next)\b", reference) and re.search(r"\bcase\b", reference))
    if named_fsm or (clocked and stateful):
        return "FSM"
    return "sequential" if clocked else "combinational"


def reset_style(spec):
    """The reset a spec asks for: 'synchronous' or 'asynchronous' if a sentence
    that mentions a reset says which (both, if different sentences disagree),
    else None. Read off the spec's own words, never the reference."""
    styles = set()
    for sentence in re.split(r"(?<=[.;:])\s+|\n\s*\n", spec):
        if not re.search(r"reset", sentence, re.I):
            continue
        if re.search(r"\basynchronous(ly)?\b", sentence, re.I):
            styles.add("asynchronous")
        if re.search(r"\bsynchronous(ly)?\b", sentence, re.I):
            styles.add("synchronous")
    return "both" if len(styles) > 1 else (styles.pop() if styles else None)


# A sensitivity list of two edges, one of them a clock: the shape of a flop
# with an asynchronous reset (or set), `@(posedge clk or posedge reset)`.
TWO_EDGE_RE = re.compile(r"@\s*\(\s*((?:posedge|negedge)\s+(\w+))\s*(?:or|,)\s*((?:posedge|negedge)\s+(\w+))\s*\)")
CLOCK_RE = re.compile(r"cl(oc)?k", re.I)


def uses_async_reset(code):
    return any(CLOCK_RE.search(m.group(2)) or CLOCK_RE.search(m.group(4)) for m in TWO_EDGE_RE.finditer(code))


def make_reset_synchronous(code):
    """`code` with every clock-plus-one-edge sensitivity list cut down to the
    clock edge alone, so a reset tested inside the block becomes synchronous.
    A diagnostic for the reset audit, never applied to a scored sample.
    Returns the new code and the number of lists cut."""
    cut = 0

    def keep_clock(m):
        nonlocal cut
        for edge, name in ((m.group(1), m.group(2)), (m.group(3), m.group(4))):
            if CLOCK_RE.search(name):
                cut += 1
                return f"@({edge})"
        return m.group(0)
    new = TWO_EDGE_RE.sub(keep_clock, code)
    return new, cut


def has_module(code):
    return bool(MODULE_RE.search(code or ""))


def rename_top(source, old, new):
    """Rename module `old` to `new`, as a whole word everywhere in `source`."""
    return re.sub(r"\b%s\b" % re.escape(old), new, source)


def top_modules(source):
    """Modules defined in `source` that no other module in it instantiates."""
    defined = re.findall(r"^\s*module\s+([A-Za-z_]\w*)", source, re.M)
    tops = []
    for m in defined:
        others = re.sub(r"^\s*module\s+%s\b" % re.escape(m), "", source, flags=re.M)
        if not re.search(r"\b%s\s*(#\s*\(|[A-Za-z_]\w*\s*\()" % re.escape(m), others):
            tops.append(m)
    return tops


# ---------------------------------------------------------------------------
# VerilogEval v2, spec-to-RTL

class VerilogEvalV2:
    name = "verilog-eval-v2"

    # scripts/sv-generate, prompts['spec-to-rtl'] and its suffixes, verbatim.
    SYSTEM = "\nYou are a Verilog RTL designer that only writes code using correct Verilog syntax.\n"
    RULES = """
Here are some additional rules and coding conventions.

 - Declare all ports and signals as logic; do not to use wire or reg.

 - For combinational logic with an always block do not explicitly specify
   the sensitivity list; instead use always @(*).

 - All sized numeric constants must have a size greater than zero
   (e.g, 0'b0 is not a valid expression).

 - An always block must read at least one signal otherwise it will never
   be executed; use an assign statement instead of an always block in
   situations where there is no need to read any signals.

 - if the module uses a synchronous reset signal, this means the reset
   signal is sampled with respect to the clock. When implementing a
   synchronous reset signal, do not include posedge reset in the
   sensitivity list of any sequential always block.
"""
    NO_EXPLAIN = """
Enclose your code with [BEGIN] and [DONE]. Only output the code snippet
and do NOT output anything else.
"""

    def __init__(self, cfg, path):
        self.cfg = cfg
        self.dir = os.path.join(path, "dataset_spec-to-rtl")
        self.scripts = os.path.join(path, "scripts")
        self.shots = int(cfg.get("in_context_examples", 0))
        self.rules = bool(cfg.get("rules", False))

    def problem_ids(self):
        with open(os.path.join(self.dir, "problems.txt")) as f:
            return [line.strip() for line in f if line.strip()]

    def user_prompt(self, spec):
        # sv-generate's assembly for task spec-to-rtl, explain off.
        full = ""
        if self.shots:
            with open(os.path.join(self.scripts, f"verilog-example-prefix_spec-to-rtl_{self.shots}-shot.txt")) as f:
                full += f.read()
        full += "\nQuestion:\n" + spec.strip() + "\n"
        if self.rules:
            full += self.RULES
        full = full.rstrip() + "\n" + self.NO_EXPLAIN
        full += "\nAnswer:\n"
        return full

    def load(self, pid):
        with open(os.path.join(self.dir, pid + "_prompt.txt")) as f:
            spec = f.read()
        with open(os.path.join(self.dir, pid + "_ref.sv")) as f:
            ref = f.read()
        return Problem(bench=self.name, pid=pid, top="TopModule", system=self.SYSTEM,
                       user=self.user_prompt(spec), golden=rename_top(ref, "RefModule", "TopModule"),
                       extra={"ref": os.path.join(self.dir, pid + "_ref.sv"),
                              "test": os.path.join(self.dir, pid + "_test.sv"),
                              "spec": spec})

    def category(self, problem):
        """The design class from the reference, and the form the spec takes:
        prose, a simulation waveform to reverse-engineer (the circuitN
        problems), or a Karnaugh map."""
        with open(problem.extra["ref"]) as f:
            ref = f.read()
        spec = problem.extra["spec"]
        form =("waveform" if re.search(r"waveform", spec, re.I) else
                "kmap" if re.search(r"karnaugh|k-map", spec, re.I) else "prose")
        cls = design_class(ref, problem.pid)
        return {"class": cls, "spec": form, "group": cls}

    @staticmethod
    def extract(content, top="TopModule"):
        """sv-generate's spec-to-RTL extraction: the code between [BEGIN] and
        [DONE], else its backtick fallback. Ported line for line."""
        lines = content.splitlines()
        backticks_count = 0
        endmodule_before_startmodule = False
        module_already_exists = False
        for line in lines:
            if line.startswith("```"):
                backticks_count += 1
            elif line.startswith("endmodule"):
                if not module_already_exists:
                    endmodule_before_startmodule = True
            elif line.startswith("module TopModule"):
                module_already_exists = True
        if endmodule_before_startmodule:
            module_already_exists = False

        found, start, end = [], False, False
        for line in lines:
            if not start:
                if line.strip() == "[BEGIN]":
                    start = True
                elif line.lstrip().startswith("[BEGIN]"):
                    found.append(line.lstrip().replace("[BEGIN]", ""))
                    start = True
            elif start and not end:
                if line.strip() == "[DONE]":
                    end = True
                elif line.rstrip().endswith("[DONE]"):
                    found.append(line.rstrip().replace("[DONE]", ""))
                    end = True
                else:
                    found.append(line)

        out = [""]
        if start and end:
            out += found
        if not start and not end:
            first_bt = second_bt = found_module = found_endmodule = False
            for line in lines:
                echo = True
                if line.strip().startswith("module TopModule"):
                    found_module = True
                if backticks_count >= 2:
                    if (not first_bt) or second_bt:
                        echo = False
                else:
                    if found_endmodule:
                        echo = False
                    if module_already_exists and not found_module:
                        echo = False
                if line.startswith("```"):
                    if not first_bt:
                        first_bt = True
                    else:
                        second_bt = True
                    echo = False
                elif line.strip().startswith("endmodule"):
                    found_endmodule = True
                if echo:
                    out.append(line)
            out.append("")
        return "\n".join(out) + "\n"

    def functional(self, problem, sample_path, workdir, tools, timeouts):
        """Compile test, reference and sample together with -s tb, simulate,
        and classify the combined log exactly as sv-iv-analyze does. The
        functional pass is its '.'."""
        sim = os.path.join(workdir, "sim")
        cmd = iverilog_cmd(tools, tools["standard"]) + ["-s", "tb", "-o", sim,
                                                        problem.extra["test"], problem.extra["ref"], sample_path]
        rc, log, timed_out = compile_run(cmd, [tools["vvp"], "-n", sim], workdir, timeouts)
        code = self.classify(log, timed_out)
        return code == ".", code, log

    @staticmethod
    def classify(log, timed_out):
        """scripts/sv-iv-analyze's passfail code for a compile-and-run log."""
        if timed_out:
            log = log + "\nTIMEOUT\n"
        passfail = "?"
        error_c = error_p = no_mismatch = False
        for line in log.splitlines():
            if "syntax error" in line:
                return "S"
            if "error" in line:
                error_c = True
            if "error: This assignment requires an explicit cast" in line:
                return "e"
            if "error: Sized numeric constant must have a size greater than zero" in line:
                return "0"
            if "warning: always_comb process has no sensitivities" in line:
                return "n"
            if "found no sensitivities so it will never trigger" in line:
                return "n"
            if "is declared here as wire" in line:
                return "w"
            if "Unknown module type" in line:
                return "m"
            if "Unable to bind wire/reg" in line:
                error_p = True
            if "Unable to bind wire/reg/memory `clk'" in line:
                return "c"
            if "TIMEOUT" in line:
                return "T"
            m = re.match(r"^Mismatches: (\d+) in \d+ samples$", line)
            if m and int(m.group(1)) == 0:
                no_mismatch = True
        if error_p:
            return "p"
        if error_c:
            return "C"
        if no_mismatch:
            return "."
        return passfail  # '?': ran, but never reported zero mismatches


# ---------------------------------------------------------------------------
# RTLLM 2.0

class RTLLM2:
    name = "rtllm-2"

    SYSTEM = "You are a helpful assistant."
    FENCE_LANGS = {"", "verilog", "systemverilog", "sv", "v"}

    def __init__(self, cfg, path):
        self.cfg = cfg
        self.path = path
        self.designs = {}
        for root, _, files in os.walk(path):
            if "makefile" in files and ".git" not in root:
                self.designs[os.path.basename(root)] = root

    def problem_ids(self):
        return sorted(self.designs)

    def load(self, pid):
        d = self.designs[pid]
        with open(os.path.join(d, "design_description.txt")) as f:
            desc = f.read()
        m = re.search(r"Module name:\s*\n?\s*([A-Za-z_]\w*)", desc)
        if not m:
            raise ValueError(f"{pid}: no 'Module name:' in its description")
        top = m.group(1)
        # The golden: verified_<dir>.v if present, else the only verified_*.v;
        # its top module is renamed to the name the testbench instantiates.
        verified = sorted(f for f in os.listdir(d) if f.startswith("verified_") and f.endswith(".v"))
        pick = f"verified_{pid}.v" if f"verified_{pid}.v" in verified else (verified[0] if len(verified) == 1 else None)
        if pick is None:
            raise ValueError(f"{pid}: cannot tell which of {verified} is the golden reference")
        with open(os.path.join(d, pick)) as f:
            golden = f.read()
        gtops = top_modules(golden)
        if len(gtops) != 1:
            raise ValueError(f"{pid}: golden {pick} has top modules {gtops}")
        golden = rename_top(golden, gtops[0], top)
        with open(os.path.join(d, "testbench.v")) as f:
            tb = f.read()
        tbtops = top_modules(tb)
        if len(tbtops) != 1:
            raise ValueError(f"{pid}: testbench has top modules {tbtops}")
        data = [os.path.join(d, f) for f in os.listdir(d)
                if not f.endswith(".v") and f not in ("makefile", "design_description.txt")]
        return Problem(bench=self.name, pid=pid, top=top, system=self.SYSTEM, user=desc,
                       golden=golden,
                       extra={"dir": d, "testbench": os.path.join(d, "testbench.v"),
                              "tb_top": tbtops[0], "data": data, "golden_file": pick, "spec": desc})

    def category(self, problem):
        """The design class from the golden, and RTLLM's own grouping: its
        top-level folder and the folder below it (Arithmetic/Adder, ...)."""
        rel = os.path.relpath(problem.extra["dir"], self.path).split(os.sep)
        return {"class": design_class(problem.golden, problem.pid), "spec": "prose",
                "group": rel[0], "subgroup": "/".join(rel[:2])}

    @classmethod
    def extract(cls, content, top):
        """The last fenced code block that defines `top`; else the last one
        that defines any module; else, with no fences at all, the text from
        the first 'module' to the last 'endmodule'. Preferring the block that
        defines the requested module keeps a testbench the model appends from
        being taken for the design."""
        blocks, cur, lang = [], None, None
        for line in content.splitlines():
            if line.lstrip().startswith("```"):
                if cur is None:
                    lang = line.strip()[3:].strip().lower()
                    cur = []
                else:
                    blocks.append((lang, "\n".join(cur)))
                    cur = None
            elif cur is not None:
                cur.append(line)
        code_blocks = [b for l, b in blocks if l in cls.FENCE_LANGS and has_module(b)]
        defines_top = [b for b in code_blocks if re.search(r"\bmodule\s+%s\b" % re.escape(top), b)]
        if defines_top:
            return defines_top[-1] + "\n"
        if code_blocks:
            return code_blocks[-1] + "\n"
        if not blocks:
            i = content.find("module")
            j = content.rfind("endmodule")
            if i >= 0 and j > i:
                return content[i:j + len("endmodule")] + "\n"
        return ""

    def functional(self, problem, sample_path, workdir, tools, timeouts):
        """RTLLM's Makefile, on Icarus: compile the testbench with the sample
        (its data files alongside), simulate, and pass when the output
        contains "Pass" or "pass" (auto_run.py's rule) within the timeout."""
        for f in problem.extra["data"]:
            shutil.copy(f, workdir)
        tb = os.path.join(workdir, "testbench.v")
        shutil.copy(problem.extra["testbench"], tb)
        # RTLLM's Makefiles give VCS -timescale=1ns/1ns: the time unit of any
        # file that declares none. Icarus has no such flag, but a `timescale in
        # a file listed first carries over to the files after it that declare
        # none, which is the same default. Without it clkgenerator's golden,
        # which declares no timescale, fails.
        ts = os.path.join(workdir, "timescale.v")
        with open(ts, "w") as f:
            f.write("`timescale 1ns/1ns\n")
        sim = os.path.join(workdir, "sim")
        cmd = iverilog_cmd(tools, tools["standard"]) + ["-s", problem.extra["tb_top"], "-o", sim,
                                                        ts, tb, sample_path]
        rc, log, timed_out = compile_run(cmd, [tools["vvp"], "-n", sim], workdir, timeouts)
        if rc == "compile":
            return False, "C", log
        if timed_out:
            return False, "T", log
        sim_out = log.split("\n--- simulation ---\n", 1)[-1]
        passed = "Pass" in sim_out or "pass" in sim_out
        return passed, ("." if passed else "F"), log


def adapter(name, bench_cfg):
    path = os.path.expanduser(bench_cfg["path"])
    if name == VerilogEvalV2.name:
        return VerilogEvalV2(bench_cfg, path)
    if name == RTLLM2.name:
        return RTLLM2(bench_cfg, path)
    raise ValueError(f"unknown benchmark {name}")
