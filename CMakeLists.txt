project(tomcat-planrec)
cmake_minimum_required(VERSION 3.16)

# Without this a plain `cmake ..` compiles with no -O flag at all. That costs
# nothing measurable while the runtime is dominated by libz3 (planner_doc.md
# 6.1.4), but it stops being free once that changes.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  message(STATUS "No build type specified, defaulting to Release")
endif()

set(CMAKE_CXX_STANDARD 20)

# Warnings on for the project's own code. Boost, Z3 and Graphviz are included as
# SYSTEM directories in each target, so what is reported is ours; it was brought
# to zero in planner_doc.md 8.21, and a new one should be read, not scrolled
# past. Unused parameters are left off: score functions and visitors take
# arguments they do not all need.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wall -Wextra -Wshadow -Wno-unused-parameter)
endif()

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/apps/cmake")
add_subdirectory(apps)
add_subdirectory(lib)

enable_testing()
add_subdirectory(test)

# The local LLM executor (executor/), built only when libllama is found. It
# lives in its own conda env, so add that env to the prefix path to get it:
#   -DCMAKE_PREFIX_PATH="/opt/anaconda3/envs/htnplan;/opt/anaconda3/envs/llama"
# The planner never depends on it, and without libllama nothing here changes.
option(HTN_WITH_LLAMA "Build the libllama executor targets (executor/) when libllama is found" ON)
if(HTN_WITH_LLAMA)
  find_package(llama CONFIG QUIET)
  find_package(nlohmann_json 3 CONFIG QUIET)
  if(llama_FOUND AND nlohmann_json_FOUND)
    add_subdirectory(executor)
  else()
    message(STATUS "libllama not found, so executor/ is not built. "
                   "Add the llama conda env to CMAKE_PREFIX_PATH to build it.")
  endif()
endif()
