cmake_minimum_required(VERSION 3.16)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")

add_compile_definitions(BOOST_NO_CXX98_FUNCTION_BASE)
find_package(Boost COMPONENTS program_options json REQUIRED)
find_package(Z3 REQUIRED)

add_executable(planner_bench planner_bench.cpp)
target_include_directories(planner_bench PRIVATE ${Boost_INCLUDE_DIR} ${Z3_CXX_INCLUDE_DIRS} ../../lib/)
target_link_libraries(planner_bench PRIVATE tomcat ${Boost_LIBRARIES} ${Z3_LIBRARIES})
