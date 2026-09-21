# tomcat-planrec
Code for the MCTS Hierarchical Task Network (HTN) planner developed for ToMCAT.

# Table of Contents
1. [Build Requirements](#build-requirements) 
2. [Installation](#installation) 
3. [HDDL Domain and Problem Definition
   Loaders](#hddl-domain-and-problem-definition-loaders)
4. [MCTS HTN Planner](#mcts-htn-planner) 

# Build Requirements
- cmake (Minimum requirement is version 3.16, https://cmake.org/)
- Boost (Minimum requirement is version 1.79, https://www.boost.org/)
  - Specific Boost Libraries to build: filesystem, log, date\_time, chrono, program\_options, coroutine, json
- Z3 (c++ Library) (Minimum requirement is version 4.8.17, https://github.com/Z3Prover/z3)
- Graphviz (c Library) (Tested on version 8.0.5, https://graphviz.org/)
- Tested on Apple clang version 15.0.0.15000309 (It may also work using GNU 11.4.0)

# Installation
To build, do the following:

    mkdir -p build
    cd build
    cmake ..
    make -j

After building, you can run tests with the following command (assuming you are
in the `build` directory).

    ctest

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
- The "either" keyword is not currently supported
- Syntax and logic for the handling of external function calls are not
  currently supported
- Requirement checking for given requirement keys is not currently supported

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
[docs/PLANNER\_PROVENANCE.md](docs/PLANNER_PROVENANCE.md) for a detailed
account of which algorithms the code implements and where each piece comes from
in the planning and MCTS literature.

## Current Capabilities
- Can run domain and problem elements loaded from our HDDL domain and problem
  definition loaders
- Can handle ordering constraints on tasks, unconstrained tasks, and performs
  task interleaving
- We also developed a graphing function that can take the results of the
  planner and output a visual representation of the task hierarchy used to
  generate the grounded plan
- The problem definition requires a top-level task or set of partially-ordered top-level tasks
  from which to start the task decomposition process
- The task decomposition process is NOT goal directed and therefore it will
  ignore goal statements defined in problem definitions
- Shallow deadends and infinite recursive/looping tasks can cause
  the planner to "stall out"

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

Passing the `--graph` (or `-g`) flag saves a visual representation of the task
hierarchy behind the returned plan as a png. By default it is written to the
current working directory and named after the problem definition; use
`--graph_file` (or `-f`) to choose a different path.
