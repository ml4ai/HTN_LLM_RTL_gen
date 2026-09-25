#pragma once

#include "Term.h"

namespace fol {
    struct Function {
        std::string name;
        std::vector<Term> args = {};
        friend bool operator==(const Function& lhs, const Function& rhs) {
            return (lhs.name == rhs.name) && (lhs.args == rhs.args);
        }
        friend std::ostream& operator<<(std::ostream& out, const Function& f) {
            out << "(";
            //"(f a b)". This printed "(fa b )": no space after the name,
            //and a test of i < size that is always true.
            out << f.name;
            for (auto const& a : f.args) {
                out << " " << a;
            }
            out << ")";
            return out;
        };
    };
} // namespace fol
