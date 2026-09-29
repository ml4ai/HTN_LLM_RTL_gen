# The local LLM executor, built on its own so the planner's build does not
# depend on llama.cpp. Point CMAKE_PREFIX_PATH at the conda env holding libllama:
#
#   E=/opt/anaconda3/envs/llama
#   cmake -S executor -B executor/build -DCMAKE_PREFIX_PATH=$E -DCMAKE_BUILD_TYPE=Release
#   cmake --build executor/build
#
# Configuring fails unless the libllama found is the build executor_config.json
# pins: outputs from another build are not comparable (LLM_RTL_code_generation.md 9.8).
cmake_minimum_required(VERSION 3.19)
project(htn_executor CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(llama REQUIRED CONFIG)
find_package(nlohmann_json 3 REQUIRED CONFIG)

set(EXECUTOR_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/executor_config.json")
file(READ "${EXECUTOR_CONFIG}" _cfg)
string(JSON _pinned_commit GET "${_cfg}" runtime llama_cpp_commit)
string(JSON _pinned_build GET "${_cfg}" runtime llama_cpp_build_number)
if(NOT LLAMA_BUILD_COMMIT STREQUAL _pinned_commit)
  message(FATAL_ERROR
    "libllama at ${llama_DIR} is build ${LLAMA_BUILD_NUMBER} (commit ${LLAMA_BUILD_COMMIT}), "
    "but executor_config.json pins build ${_pinned_build} (commit ${_pinned_commit}). "
    "Install the pinned build, or change the config deliberately and bump config_version.")
endif()
message(STATUS "libllama: build ${LLAMA_BUILD_NUMBER}, commit ${LLAMA_BUILD_COMMIT} (matches the config)")

add_executable(executor_smoke smoke_test.cpp)
target_link_libraries(executor_smoke PRIVATE llama nlohmann_json::nlohmann_json)
target_compile_options(executor_smoke PRIVATE -Wall -Wextra)
target_compile_definitions(executor_smoke PRIVATE
  EXECUTOR_LLAMA_COMMIT="${LLAMA_BUILD_COMMIT}"
  EXECUTOR_DEFAULT_CONFIG="${EXECUTOR_CONFIG}")
