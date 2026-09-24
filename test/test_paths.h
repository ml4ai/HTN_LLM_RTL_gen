#pragma once

// Where the shipped domains are, fixed at configure time.
//
// The tests used to reach them through "../../domains/...", which is relative
// to the directory the test runs in and so assumed the build directory was a
// child of the source tree. From any other build directory they compiled,
// linked, and then failed at run time with "No file ... found" -- and from the
// wrong directory test_loader exited 201, indistinguishable from a real
// assertion failure (planner_doc.md 4.1 note 17). test/CMakeLists.txt now
// passes the absolute path in.
//
// The fallback keeps a test file compilable outside CMake; it is only right
// when run from <source>/build/test, as before.
#ifndef HTN_DOMAINS_DIR
#define HTN_DOMAINS_DIR "../../domains"
#endif
