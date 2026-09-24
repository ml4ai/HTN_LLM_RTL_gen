#pragma once
#include "api.hpp"

#include <boost/spirit/home/x3/support/ast/position_tagged.hpp>
#include <boost/spirit/home/x3/support/utility/error_reporting.hpp>

#include <map>
#include <cctype>
#include <string>

namespace parser {
    namespace x3 = boost::spirit::x3;

    ////////////////////////////////////////////////////////////////////////////
    //  Our error handler
    ////////////////////////////////////////////////////////////////////////////
    template <typename Iterator>
    using error_handler = x3::error_handler<Iterator>;

    //x3 names what it expected by the failing rule's name when it has one,
    //and otherwise by the C++ type of the anonymous sub-parser -- hundreds of
    //characters of mangled Boost.Spirit template, useless to a person and
    //worse to a language model asked to fix its input. That type still names
    //the grammar rules inside it, as parser::T<Rule>, and the first is the
    //construct the parser was in the middle of reading. So name that, and
    //say what usually breaks one.
    inline std::string rule_words(std::string const& tag) {
        //"TTypedListVariables" -> "typed list variables"
        std::string words;
        for (size_t i = 1; i < tag.size(); i++) {
            char c = tag[i];
            if (std::isupper((unsigned char)c)) {
                if (!words.empty()) words += ' ';
                words += (char)std::tolower((unsigned char)c);
            }
            else {
                words += c;
            }
        }
        return words;
    }

    inline std::string describe_expectation(std::string const& which) {
        if (which.rfind("N5boost", 0) != 0 && which.find("boost::spirit") == std::string::npos) {
            return which;
        }
        //Itanium mangling writes parser::TTask as "6parser5TTask".
        std::string construct;
        auto at = which.find("6parser");
        if (at != std::string::npos) {
            size_t i = at + 7, n = 0;
            while (i < which.size() && std::isdigit((unsigned char)which[i])) {
                n = n * 10 + (which[i] - '0');
                i++;
            }
            if (n > 1 && i + n <= which.size() && which[i] == 'T') {
                construct = rule_words(which.substr(i, n));
            }
        }
        return "the rest of " + (construct.empty() ? std::string("this construct")
                                                   : "a " + construct) +
               " (a required keyword or part is missing, or a parenthesis is "
               "missing or extra at or before this point)";
    }

    struct ErrorHandlerBase {
        ErrorHandlerBase();

        // Declare template error handler function
        template <class Iterator, class Exception, class Context>
        x3::error_handler_result on_error(Iterator& first,
                                          Iterator const& last,
                                          Exception const& x,
                                          Context const& context);

        std::map<std::string, std::string> id_map;
    };

    ////////////////////////////////////////////////////////////////////////////
    // Implementation
    ////////////////////////////////////////////////////////////////////////////

    inline ErrorHandlerBase::ErrorHandlerBase() {
        id_map["domain"] = "Domain";
        id_map["problem"] = "Problem";
        id_map["action"] = "Action";
        id_map["task"] = "Task";
        id_map["method"] = "Method";
        id_map["requirements"] = "Requirements";
        id_map["typed_list"] = "TypedList";
        id_map["literal_terms"] = "Literal<Term>";
    }

    template <class Iterator, class Exception, class Context>
    inline x3::error_handler_result
    ErrorHandlerBase::on_error(Iterator& first,
                               Iterator const& last,
                               Exception const& x,
                               Context const& context) {
        std::string which = x.which();

        auto iter = id_map.find(which);
        if (iter != id_map.end()) {
            which = iter->second;
        }
        which = describe_expectation(which);

        std::string message = "Error! Expecting: " + which + " here:";
        auto& error_handler = x3::get<x3::error_handler_tag>(context).get();
        error_handler(x.where(), message);
        return x3::error_handler_result::fail;
    }
} // namespace parser
