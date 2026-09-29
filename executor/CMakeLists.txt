# The local LLM executor. Built from the top-level build when libllama is found
# there (HTN_WITH_LLAMA), or on its own:
#
#   E=/opt/anaconda3/envs/llama
#   cmake -S executor -B executor/build -DCMAKE_PREFIX_PATH=$E -DCMAKE_BUILD_TYPE=Release
#   cmake --build executor/build
#
# Configuring fails unless the libllama found is the build executor_config.json
# pins: outputs from another build are not comparable.
cmake_minimum_required(VERSION 3.19)
project(htn_executor CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  enable_testing()
endif()

# Found already when the top-level build includes this directory. llama's
# package config creates its target without checking whether it exists, so a
# second find_package fails; the variables it set (LLAMA_BUILD_COMMIT and the
# rest) are inherited from the parent scope.
if(NOT TARGET llama)
  find_package(llama REQUIRED CONFIG)
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
  find_package(nlohmann_json 3 REQUIRED CONFIG)
endif()

set(EXECUTOR_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/executor_config.json")
file(READ "${EXECUTOR_CONFIG}" _cfg)
string(JSON _pinned_commit GET "${_cfg}" runtime llama_cpp_commit)
string(JSON _pinned_build GET "${_cfg}" runtime llama_cpp_build_number)
string(JSON _backend_dir GET "${_cfg}" runtime backend_dir)
if(NOT LLAMA_BUILD_COMMIT STREQUAL _pinned_commit)
  message(FATAL_ERROR
    "libllama at ${llama_DIR} is build ${LLAMA_BUILD_NUMBER} (commit ${LLAMA_BUILD_COMMIT}), "
    "but executor_config.json pins build ${_pinned_build} (commit ${_pinned_commit}). "
    "Install the pinned build, or change the config deliberately and bump config_version.")
endif()
# The GPU backend is a module loaded at run time from the config's backend_dir,
# so that has to be the bin/ of the libllama linked here. Pointing it at
# another install would pair one build's library with another's kernels.
file(REAL_PATH "${LLAMA_BIN_DIR}" _found_bin)
file(REAL_PATH "${_backend_dir}" _config_bin)
if(NOT _found_bin STREQUAL _config_bin)
  message(FATAL_ERROR
    "executor_config.json loads backends from ${_backend_dir}, but the libllama found is "
    "installed with its backends in ${LLAMA_BIN_DIR}.")
endif()
message(STATUS "libllama: build ${LLAMA_BUILD_NUMBER}, commit ${LLAMA_BUILD_COMMIT} (matches executor_config.json)")

# What any code of ours that runs the executor links: libllama, the JSON reader
# for the config, and the pins as compile definitions. Linking this rather than
# llama directly is what keeps the pin check above in front of every such
# target.
add_library(htn_llama INTERFACE)
add_library(htn::llama ALIAS htn_llama)
target_link_libraries(htn_llama INTERFACE llama nlohmann_json::nlohmann_json)
target_compile_features(htn_llama INTERFACE cxx_std_17)
target_compile_definitions(htn_llama INTERFACE
  EXECUTOR_LLAMA_COMMIT="${LLAMA_BUILD_COMMIT}"
  EXECUTOR_DEFAULT_CONFIG="${EXECUTOR_CONFIG}"
  EXECUTOR_BACKEND_DIR="${LLAMA_BIN_DIR}")

add_executable(executor_smoke smoke_test.cpp)
target_link_libraries(executor_smoke PRIVATE htn::llama)
target_compile_options(executor_smoke PRIVATE -Wall -Wextra)

# Off by default: the smoke test loads the ~35 GB model and takes 15 s or more,
# which a routine `ctest` should not. Select it with `ctest -L llm`.
option(HTN_LLAMA_TESTS "Register the executor smoke test with ctest (loads the model)" OFF)
if(HTN_LLAMA_TESTS)
  add_test(NAME executor_smoke COMMAND executor_smoke)
  set_tests_properties(executor_smoke PROPERTIES LABELS llm TIMEOUT 600)
endif()
