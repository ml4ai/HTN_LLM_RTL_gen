cmake_minimum_required(VERSION 3.16)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")

#This adds debugger symbols for lldb during compilation when uncommented. 
#set(CMAKE_CXX_FLAGS "-ggdb")
#set(CMAKE_CXX_FLAGS "-glldb")

add_compile_definitions(BOOST_NO_CXX98_FUNCTION_BASE)

# The planner's default domain and problem are found through this absolute path,
# so running it with no arguments works from any directory, not only from
# <source>/build. The same fix as the tests; see test/test_paths.h.
get_filename_component(HTN_DOMAINS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../domains" ABSOLUTE)
add_compile_definitions(HTN_DOMAINS_DIR="${HTN_DOMAINS_DIR}")
find_package(Boost COMPONENTS program_options REQUIRED)
find_package(Graphviz REQUIRED)
find_package(Z3 REQUIRED)

add_executable(MCTS_planner MCTS_planner.cpp)
target_include_directories(MCTS_planner PRIVATE ${Boost_INCLUDE_DIR} ${GRAPHVIZ_INCLUDE_DIRS} ${Z3_CXX_INCLUDE_DIRS}
    ../../lib/)
target_link_libraries(MCTS_planner PRIVATE tomcat ${GRAPHVIZ_LIBS} ${Boost_LIBRARIES} ${Z3_LIBRARIES})
