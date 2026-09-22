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
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/apps/cmake")
add_subdirectory(apps)
add_subdirectory(lib)

enable_testing()
add_subdirectory(test)
