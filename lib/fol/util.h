#pragma once

#include "Term.h"

// Get name of term
inline std::string name(ast::Term const& term) {
    return boost::apply_visitor([](const auto& t) { return t.name; }, term);
}
