#pragma once

#include <iomanip>
#include <iostream>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>
#include <algorithm>
#include "parsing/ast.hpp"

// Whether an element is in an associative container. By reference: these took
// the container by value, a copy per call.
template <class Element, class AssociativeContainer>
bool in(Element const& element, AssociativeContainer const& container) {
    return container.count(element);
}

// Whether an element is in a vector.
template <class Element> bool in(Element const& element, std::vector<Element> const& v) {
    return std::find(v.begin(), v.end(), element) != v.end();
}

// select_randomly taken from
// https://stackoverflow.com/questions/6942273/how-to-get-a-random-element-from-a-c-container
template <typename Iter, typename RandomGenerator>
Iter select_randomly(Iter start, Iter end, RandomGenerator& g) {
    std::uniform_int_distribution<> dis(0, std::distance(start, end) - 1);
    std::advance(start, dis(g));
    return start;
}

// Only the overload taking a generator is kept. Two others seeded a static
// generator of their own -- once per process, from a seed or from
// random_device -- the same flaw planner_doc.md 4.1 note 14 fixed in the
// planner, left lying around unused (8.21).


// Define support for printing vectors
template <typename T>
std::ostream& operator<<(std::ostream& os, const std::vector<T>& v) {
    os << "(";
    for (size_t i = 0; i < v.size(); i++) {
        os << v.at(i);
        if (i + 1 < v.size()) {
            os << " ";
        }
    }
    os << ')';
    return os;
}

struct constraints_type : public boost::static_visitor<int> {
  int operator()(const ast::Nil&) const { return 0; }
  int operator()(const ast::Constraint&) const { return 1; }
  int operator()(const std::vector<ast::Constraint>&) const { return 2; }

};

inline int which_constraints(ast::Constraints const& c) {
  return boost::apply_visitor(constraints_type(),c);
}

struct constraint_type : public boost::static_visitor<int> {
  int operator()(const ast::Nil&) const { return 0; }
  int operator()(const ast::EqualsSentence&) const { return 1; }
  int operator()(const ast::NotEqualsSentence&) const { return 2; }

};

inline int which_constraint(ast::Constraint const& c) {
  return boost::apply_visitor(constraint_type(),c);
}

struct subtasks_type : public boost::static_visitor<int> {
  int operator()(const ast::Nil&) const { return 0; }
  int operator()(const ast::SubTask&) const { return 1; }
  int operator()(const std::vector<ast::SubTask>&) const { return 2; }

};

inline int which_subtasks(ast::SubTasks const& s) {
  return boost::apply_visitor(subtasks_type(),s);
}

struct subtask_type : public boost::static_visitor<int> {
  int operator()(const ast::MTask&) const { return 0; }
  int operator()(const ast::SubTaskWithId&) const { return 1; }

};

inline int which_subtask(ast::SubTask const& s) {
  return boost::apply_visitor(subtask_type(),s);
}

struct orderings_type : public boost::static_visitor<int> {
  int operator()(const ast::Nil&) const { return 0; }
  int operator()(const ast::Ordering&) const { return 1; }
  int operator()(const std::vector<ast::Ordering>&) const { return 2; }

};

inline int which_orderings(ast::Orderings const& os) {
  return boost::apply_visitor(orderings_type(),os);
}
