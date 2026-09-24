#pragma once

#include "Predicate.h"
#include <vector>
#include <boost/spirit/home/x3/support/ast/position_tagged.hpp>

// update away from functions and to predicates
namespace fol {
    //Position-tagged so the parser records where each atom came from, and a
    //problem found after parsing (an undeclared predicate, a wrong arity) can
    //be reported at the atom's own line rather than its action's.
    template <class T> struct Literal : boost::spirit::x3::position_tagged {
        Predicate predicate;
        std::vector<T> args;
        bool is_negative = false;

        // operator for comparing Literals, in the context of unification
        friend bool operator==(const Literal<T>& lhs, const Literal<T>& rhs) {
            return (lhs.predicate == rhs.predicate) && (lhs.args == rhs.args);
        }

        friend std::ostream& operator<<(std::ostream& out,
                                        const Literal<T>& lit) {
            out << "Literal(";
            out << "Predicate(" << lit.predicate << ") ";
            for (int i=0; i < lit.args.size(); i++) {
                out << lit.args.at(i);
                if (i < lit.args.size() - 1) {
                    out << " ";
                }
            }
            out << ")";
            return out;
        };
    };
} // namespace fol
