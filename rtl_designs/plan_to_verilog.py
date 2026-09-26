"""Render a plan from rtl_domain.hddl into Verilog, one fixed template per action.

This is a check on the domain, not a part of the pipeline: if a plan's write_*
steps, with nothing but their arguments, are enough to produce a module that
passes the gold testbench, then the plan is a complete set of instructions. In
the intended pipeline an LLM would receive each step as a natural-language
instruction instead.

    MCTS_planner -D rtl_domain.hddl -P fsm/fsm_problem.hddl -F simple ... \
        | python plan_to_verilog.py > fsm.v

analyze_* steps produce no code. The two choices they leave to the writer are
fixed here the way the domain's comments state them: states are numbered in
the order they are declared, in binary with just enough bits, and the state
registers are called ST_cr and ST_nt.
"""

import math
import re
import sys

CUR, NXT = "ST_cr", "ST_nt"


def read_plan(text):
    """The actions listed after 'Plan:', as (name, [args]) in plan order."""
    if "Plan:" not in text:
        sys.exit("no plan in the planner output")
    found = re.search(r"Plan found at depth (\d+)", text)
    if found and found.group(1) == "0":
        sys.exit("planner reported depth 0, which means it failed")
    steps = []
    for line in text.split("Plan:", 1)[1].splitlines():
        m = re.match(r"\s*\((\S+)((?:\s+[^\s)]+)*)\)_\d+\s*$", line)
        if m:
            steps.append((m.group(1), m.group(2).split()))
    return steps


def active(rst, pol):
    return rst if pol == "active_high" else f"!{rst}"


def render(steps):
    ports = [a[1] for n, a in steps if n in ("write_input_decl", "write_output_reg_decl")]
    states = [a[1] for n, a in steps if n == "write_state_param"]
    width = max(1, math.ceil(math.log2(len(states)))) if states else 1
    code = {s: i for i, s in enumerate(states)}
    bit = {"zero": "0", "one": "1"}

    out = []
    output_branch_open = False
    for name, a in steps:
        if name.startswith("analyze_"):
            continue
        elif name == "write_module_header":
            out.append(f"module {a[0]}({', '.join(ports)});")
        elif name == "write_input_decl":
            out.append(f"input {a[1]};")
        elif name == "write_output_reg_decl":
            out.append(f"output reg {a[1]};")
        elif name == "write_state_param":
            out.append(f"parameter {a[1]} = {width}'d{code[a[1]]};")
        elif name == "write_state_regs":
            out.append(f"reg [{width - 1}:0] {CUR}, {NXT};")
        elif name in ("write_state_register_async", "write_state_register_sync"):
            _, clk, rst, pol, s0 = a
            edge = "posedge" if pol == "active_high" else "negedge"
            sens = f"posedge {clk} or {edge} {rst}" if name.endswith("async") else f"posedge {clk}"
            out += [f"always @({sens}) begin",
                    f"  if ({active(rst, pol)}) {CUR} <= {s0};",
                    f"  else {CUR} <= {NXT};",
                    "end"]
        elif name == "write_next_state_open":
            out += ["always @(*) begin", f"  case ({CUR})"]
        elif name == "write_state_case":
            _, s, inp, t0, t1 = a
            out.append(f"    {s}: if ({inp} == 1'b0) {NXT} = {t0}; else {NXT} = {t1};")
        elif name == "write_next_state_default":
            out.append(f"    default: {NXT} = {a[1]};")
        elif name == "write_next_state_close":
            out += ["  endcase", "end"]
        elif name == "write_output_open":
            out.append("always @(*) begin")
        elif name == "write_output_reset_clause":
            _, o, rst, pol = a
            out.append(f"  if ({active(rst, pol)}) {o} = 1'b0;")
            output_branch_open = True
        elif name == "write_output_assert":
            _, o, s, inp, b = a
            kw = "else if" if output_branch_open else "if"
            out.append(f"  {kw} ({CUR} == {s} && {inp} == 1'b{bit[b]}) {o} = 1'b1;")
            output_branch_open = True
        elif name == "write_output_default":
            out.append(f"  else {a[1]} = 1'b0;")
        elif name == "write_output_close":
            out.append("end")
            output_branch_open = False
        elif name == "write_module_end":
            out.append("endmodule")
        else:
            sys.exit(f"no template for action {name}")
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    text = open(sys.argv[1]).read() if len(sys.argv) > 1 else sys.stdin.read()
    sys.stdout.write(render(read_plan(text)))
