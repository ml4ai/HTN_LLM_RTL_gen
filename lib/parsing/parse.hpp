#pragma once

#include "api.hpp"
#include "config.hpp"
#include "error_handler.hpp"
#include <boost/throw_exception.hpp>
#include <exception>
#include <iostream>
#include <string>
#include <sstream>
#include <algorithm>
#include <vector>


template<class T>
auto get_parser() {
    if constexpr(std::is_same<T, ast::Literal<ast::Term>>::value) {
        return literal_terms();
    }
    else if constexpr(std::is_same<T, ast::Domain>::value) {
        return domain();
    }
    else if constexpr(std::is_same<T, ast::Problem>::value) {
        return problem();
    }
    else if constexpr(std::is_same<T, ast::Type>::value) {
        return type();
    }
    else if constexpr(std::is_same<T, ast::Sentence>::value) {
        return sentence();
    }
    else {
        BOOST_THROW_EXCEPTION(std::runtime_error("Parser not implemented for this type!"));
    }
}

//Thrown for input the grammar rejects. what() carries the whole report --
//file, line, the offending source line and a caret -- so a caller that
//catches it (a pipeline asking a language model to fix its HDDL) has
//everything a person reading stderr would have had.
struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

//The 1-based source line of every node the grammar position-tags (actions,
//methods, tasks, subtasks, orderings, ...), indexed by the node's id_first.
//The parser's own position cache holds iterators into the text, which dies
//with parse(), so the lines are worked out before it returns.
struct SourceLines {
    std::string file;
    std::vector<int> line_of_id;

    int line(boost::spirit::x3::position_tagged const& node) const {
        if (node.id_first < 0 || node.id_first >= (int)line_of_id.size()) {
            return 0;
        }
        return line_of_id[node.id_first];
    }
};

template <class T>
T parse(std::string const& storage, std::string const& file = "",
        SourceLines* lines = nullptr) {
    auto parser = get_parser<T>();
    using parser::error_handler_tag;
    namespace x3 = boost::spirit::x3;
    std::string::const_iterator iter = storage.begin();
    std::string::const_iterator end = storage.end();
    std::ostringstream report;
    parser::ErrorHandlerType error_handler(iter, end, report, file);
    T object;
    auto const error_handling_parser =
        x3::with<x3::error_handler_tag>(std::ref(error_handler))[parser];
    bool r = false;
    try {
        r = phrase_parse(iter, end, error_handling_parser, parser::skipper, object);
    }
    catch (x3::expectation_failure<std::string::const_iterator> const& x) {
        //A rule with no on_error handler lets the failure escape; report it
        //the same way the handled ones are.
        error_handler(x.where(), "Error! Expecting: " +
                                 parser::describe_expectation(x.which()) + " here:");
        r = false;
    }
    if (r && iter != end) {
        error_handler(iter, "Error! Expecting end of input here: ");
        r = false;
    }
    if (!r) {
        std::string text = report.str();
        if (text.empty()) {
            text = (file.empty() ? std::string("input") : file) +
                   ": not valid HDDL; the parser could not say where.";
        }
        BOOST_THROW_EXCEPTION(ParseError(text));
    }
    if (lines) {
        lines->file = file;
        lines->line_of_id.clear();
        auto const& positions = error_handler.get_position_cache().get_positions();
        //Not in source order (a node is tagged after its children), so look
        //each one up among the newline offsets.
        std::vector<std::ptrdiff_t> newlines;
        for (std::ptrdiff_t i = 0; i < (std::ptrdiff_t)storage.size(); i++) {
            if (storage[i] == '\n') {
                newlines.push_back(i);
            }
        }
        for (auto const& p : positions) {
            auto offset = p - storage.begin();
            int line = 1 + (int)(std::lower_bound(newlines.begin(), newlines.end(), offset)
                                 - newlines.begin());
            lines->line_of_id.push_back(line);
        }
    }
    return object;
}
