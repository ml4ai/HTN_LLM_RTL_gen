# tomcat-planrec
Code for the MCTS Hierarchical Task Network (HTN) planner developed for ToMCAT.

# Table of Contents
1. [Build Requirements](#build-requirements) 
2. [Installation](#installation) 
3. [HDDL Domain and Problem Definition
   Loaders](#hddl-domain-and-problem-definition-loaders)
4. [MCTS HTN Planner](#mcts-htn-planner) 
5. [Using the planner from your own code](#using-the-planner-from-your-own-code)
6. [Benchmarking](#benchmarking) 

# Build Requirements
- cmake (Minimum requirement is version 3.16, https://cmake.org/)
- Boost (Minimum requirement is version 1.79, https://www.boost.org/)
  - Only program\_options needs to be compiled; everything else used here
    (Spirit, Fusion, Variant, Optional, Boost.Test's included variant) is
    header-only
- Z3 (c++ Library) (Minimum requirement is version 4.8.17, https://github.com/Z3Prover/z3)
- Graphviz (c Library) (Tested on version 8.0.5, https://graphviz.org/)
- Tested on Apple clang version 15.0.0.15000309 (It may also work using GNU 11.4.0)

# Installation
To build, do the following:

    mkdir -p build
    cd build
    cmake ..
    make -j

After building, you can run tests with the following command from the build
directory:

    ctest

The build directory does not have to be `build` inside the source tree: the
tests, and the planner's default domain and problem, find `domains/` through an
absolute path fixed when you run `cmake`.

If you want more verbose output (e.g. if you want to show the outputs from your
`cout << ... << endl` statements), run:

    ctest -V

# HDDL Domain and Problem Definition Loaders
[Hierarchical Domain Definition Language
(HDDL)](https://staff.fnwi.uva.nl/g.behnke/papers/Hoeller2020HDDL.pdf) 
is a hierarchical extension to [Planning Domain Definition Language (PDDL)](https://en.wikipedia.org/wiki/Planning_Domain_Definition_Language). 
It allows for a hierarchical planning problems to be defined in a 
standardized human-readable syntax. 

This code-base features a loading function for parsing HDDL and loading the
parsed elements into c++ data structures. These data structures can then be
used by our planner. See our [test\_loader](test/test_loader.cpp)
script for example usage.

## Current Capabilities
- Can fully parse and load all features of HDDL aside from the
  unsupported features listed below
- Validates a domain and problem before building anything from them: every
  task, action, predicate, type, object, constant and variable used must be
  declared and in scope, with the declared number of arguments and a compatible
  type, and orderings must name the method's own subtasks without a cycle. Every
  problem found is reported at once, each with its file and line, as an
  `HDDLError`; text the grammar rejects is a `ParseError` carrying the same
  report the parser prints. `load_hddl` loads a domain and problem from strings
  rather than files. See [docs/planner\_doc.md](docs/planner_doc.md) §8.17
- `(either ...)` types are supported where a variable is typed; an object,
  constant or type has exactly one type
- `:requirements` keys are checked: an unknown key is an error, and so is a
  real PDDL requirement this planner does not implement (numeric fluents,
  durative actions, ...). What a domain uses is not compared with what it
  declares
- Some things HDDL fixes are accepted more loosely: `:requirements` and an empty
  `:parameters ()` may be left out, `:order` is accepted for `:ordering`, and
  tasks, methods and actions may come in any order
- Keywords must be lower case, although PDDL is case-insensitive
- Syntax and logic for the handling of external function calls are not
  currently supported

# MCTS HTN Planner
This [Monte Carlo Tree Search (MCTS)](https://en.wikipedia.org/wiki/Monte_Carlo_tree_search) 
Hierarchical Planner uses planning elements loaded from our HDDL domain and
problem definition loaders. The planner itself is implemented in
[cppMCTShop.h](lib/cpphop/cppMCTShop.h), and we also supply a script for
running it. Its usage is detailed below.

It uses a task decomposition method similar to
[SHOP2](https://arxiv.org/pdf/1106.4869) to generate grounded plans. However,
instead of using Depth-First Search, it runs a time-limited MCTS per each
planning decision in order to estimate the single best grounded plan according
to a user-defined score function. See
[docs/planner\_doc.md](docs/planner_doc.md) for a detailed
account of which algorithms the code implements and where each piece comes from
in the planning and MCTS literature.

## Current Capabilities
- Can run domain and problem elements loaded from our HDDL domain and problem
  definition loaders
- Can handle ordering constraints on tasks, unconstrained tasks, and performs
  task interleaving
- Method preconditions are **stricter than HDDL by default**. HDDL compiles
  each one into an effect-free primitive action ordered ahead of the method's
  subtasks, so it is checked when that action is scheduled. In a partially
  ordered domain other tasks can run in between, and the condition may no
  longer hold when the method actually starts. The planner does that
  compilation too, but by default it *also* re-checks the precondition
  immediately before the first real action arising from the method. That is the
  tighter reading the HDDL authors leave to future extensions. Choose with
  `--precondition_mode`:
  - `at_start` (default): the condition must also hold at the method's first
    real action
  - `protected`: it must hold throughout, from the check to that action; no
    other task's action may break it in between. It gives the same plans on
    every shipped domain and is cheaper where the readings differ, but is
    stricter on a condition that is broken and then restored before the method
    starts
  - `compiled`: HDDL's semantics exactly, for conformance. On a partially
    ordered domain it can return plans that break a method's precondition
    before the method starts

  The synthesised check actions are search steps, not plan steps, and are left
  out of the reported plan. See [docs/planner\_doc.md](docs/planner_doc.md)
  §8.16 for what each reading does and costs.
- We also developed a graphing function that can take the results of the
  planner and output a visual representation of the task hierarchy used to
  generate the grounded plan
- The problem definition requires a top-level task or set of partially-ordered top-level tasks
  from which to start the task decomposition process
- The task decomposition process is NOT goal directed and therefore it will
  ignore goal statements defined in problem definitions. This follows standard
  HTN semantics, where a solution is defined by decomposition alone; HDDL
  itself says a goal, when given, must also be satisfied
- The search space is not systematic: the same task network can be reached by
  decomposing tasks in different orders, so the search tree contains duplicate
  subtrees. `--algorithm 3` (in `planner_bench`) selects the systematic variant,
  which helps on some domains and hurts on others; see
  [docs/planner\_doc.md](docs/planner_doc.md) §5 and §8.10
- A domain whose decomposition can recurse without bound — a `get_to` over a
  two-way road graph, say — no longer hangs or crashes the planner: rollouts
  are depth-bounded (`--max_depth`) and the whole run is capped
  (`--max_decisions`), and the planner reports when either bound is hit. It
  still cannot *solve* such a domain, because every rollout stops at the bound
  and the search has nothing to steer by

## Running the Planner
After building, you can run: 
    
    ./apps/planners/MCTS_planner

This will run the planner on default settings, which are the same as
[test\_MCTS\_planner](test/test_MCTS_planner.cpp) 
ran by the ctest command. 

The default domain and problem definitions are the [transport
domain](domains/transport_domain.hddl) 
and a sample [transport problem](domains/transport_problem.hddl). 
The default score function is "delivery\_one" as defined in [score\_functions.h](domains/score_functions.h). 

Run with the help flag,

    ./apps/planners/MCTS_planner -h

To see what options are available including how to run the planner with
different domain and problem definitions and score functions. Score functions
must be predefined in a similar way to the "delivery\_one" function mentioned above. 
A score function runs on every finished rollout, so it should ask the
`KnowledgeBase` only for what it needs: `holds(head, args)`,
`count_facts(head)` and `facts_of(head)` answer from the fact index, and
`ask("(ground expression)")` evaluates a ground query. `get_facts()` rebuilds
the whole state as strings and is too slow to call per rollout. 

### A note on `--time_limit`

`--time_limit` (or `-T`) is the budget for **each planning decision**, and MCTS
always spends all of it, so the total run is roughly the budget times the number
of decisions. At the default of 1000 ms the transport domain above takes about
30 seconds over its 29 decisions. Far less is enough: across five seeds every
shipped domain solved reliably at 25 ms per decision, except `transport` (50 ms)
and the chain instances (about 1000 ms, because a single rollout there can take
hundreds of milliseconds). See [docs/planner\_doc.md](docs/planner_doc.md) §8.19.

Preconditions are evaluated directly against an index of the ground facts
rather than by a solver, which is what makes those numbers what they are — the
same run needed `-T 20000` and about 17 minutes before that change. See
[docs/planner\_doc.md](docs/planner_doc.md) §6 for the measurements.

If the budget is too small to evaluate even one option, the planner says so and
exits non-zero rather than reporting an empty plan.

## Using the planner from your own code

The planner is a header-only library: include `cpphop/loader.h` and
`cpphop/cppMCTShop.h` from any number of source files and link against
`tomcat`, as `test/test_linkage.cpp` does across two files.

    auto [domain,problem] = load_hddl(domain_text, problem_text);  // or load(file, file)
    domain.narration = nullptr;   // no printing; the default is standard output
    auto results = cppMCTShop(domain, problem, scorers["simple"], 100);
    auto const& plan = results.t[results.end].plan;

`load_hddl` and `load` throw `HDDLError`, listing every problem found, for
HDDL that does not validate, and `ParseError` for HDDL that does not parse.
Separate planners may run on separate threads.

## Benchmarking

`scripts/benchmark` runs every shipped domain and reports rollout timing plus a
reproducible fingerprint of what the planner did. Use it around any change that
is meant to preserve behaviour:

    scripts/benchmark --save before.json
    # ... make the change, rebuild ...
    scripts/benchmark --compare before.json

It exits non-zero if the plans, scores or final states moved, and prints timing
deltas above its measured run-to-run noise (15%) without failing on them. The
default run takes about 15 seconds; `--full` swaps the fixed-iteration runs for
the real time-limited planner and takes about 3 minutes. A baseline must come
from the same harness version, seed and mode, and `--compare` says so up front
if it does not. See [docs/planner\_doc.md](docs/planner_doc.md) §6.5 and §8.19.

Passing the `--graph` (or `-g`) flag saves a visual representation of the task
hierarchy behind the returned plan as a png. By default it is written to the
current working directory and named after the problem definition; use
`--graph_file` (or `-f`) to choose a different path.
