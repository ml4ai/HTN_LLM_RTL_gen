#pragma once
#include <boost/config/warning_disable.hpp>
#include <boost/spirit/home/x3.hpp>
#include <boost/spirit/home/x3/support/utility/annotate_on_success.hpp>

#include "ast.hpp"
#include "ast_adapted.hpp"
#include "parser_definitions.hpp"
#include "error_handler.hpp"

namespace parser {
    using namespace ast;

    using boost::fusion::at_c;
    using x3::lexeme, x3::lit, x3::alnum, x3::_attr, x3::_val, x3::space,
        x3::eol, x3::rule, x3::symbols;

    auto const name =
        lexeme[!lit('-') >> +(char_ - '?' - '(' - ')' - ':' - '=' - space)];


    // Rules
    rule<struct TRequirement, std::vector<Name>> const requirement =
        "requirement";
    auto const requirement_def = ':' >> name;
    BOOST_SPIRIT_DEFINE(requirement);
    struct TRequirement : x3::annotate_on_success {};

    rule<struct TPredicate, Name> const predicate = "predicate";
    auto const predicate_def = name;
    BOOST_SPIRIT_DEFINE(predicate);
    struct TPredicate : x3::annotate_on_success {};

    rule<struct TRequirements, std::vector<Name>> const requirements =
        "requirements";
    auto const requirements_def = '(' >> lit(":requirements") >> +requirement >>
                                  ')';
    BOOST_SPIRIT_DEFINE(requirements);
    struct TRequirements : x3::annotate_on_success {};

    rule<struct TConstant, Constant> const constant = "constant";
    auto const constant_def = name;
    BOOST_SPIRIT_DEFINE(constant);
    struct TConstant : x3::annotate_on_success {};

    rule<struct TVariable, Variable> const variable = "variable";
    auto const variable_def = '?' > name;
    BOOST_SPIRIT_DEFINE(variable);
    struct TVariable : x3::annotate_on_success {};

    rule<struct TPrimitiveType, PrimitiveType> const primitive_type =
        "primitive_type";
    auto const primitive_type_def = name;
    BOOST_SPIRIT_DEFINE(primitive_type);
    struct TPrimitiveType : x3::annotate_on_success {};

    rule<struct TEitherType, EitherType> const either_type = "either_type";
    auto const either_type_def = '(' >> lit("either") >> +primitive_type >> ')';
    BOOST_SPIRIT_DEFINE(either_type);
    struct TEitherType : x3::annotate_on_success {};

    rule<struct TType, Type> const type = "type";
    auto const type_def = primitive_type | either_type;
    BOOST_SPIRIT_DEFINE(type);
    struct TType : x3::annotate_on_success {};

    // Typed list of names
    rule<struct TExplicitlyTypedListNames, ExplicitlyTypedList<Name>> const
        explicitly_typed_list_names = "explicitly_typed_list_names";
    auto const explicitly_typed_list_names_def = +name >> '-' >> type;

    rule<struct TImplicitlyTypedListNames, ImplicitlyTypedList<Name>> const
        implicitly_typed_list_names = "implicitly_typed_list_names";
    auto const implicitly_typed_list_names_def = *name;

    rule<struct TTypedListNames, TypedList<Name>> const typed_list_names =
        "typed_list_names";
    auto const typed_list_names_def =
        *explicitly_typed_list_names >> -implicitly_typed_list_names;

    BOOST_SPIRIT_DEFINE(explicitly_typed_list_names,
                        implicitly_typed_list_names,
                        typed_list_names);
    struct TExplicitlyTypedListNames : x3::annotate_on_success {};
    struct TImplicitlyTypedListNames : x3::annotate_on_success {};
    struct TTypedListNames : x3::annotate_on_success {};

    // Typed list of variables
    rule<struct TExplicitlyTypedListVariables,
         ExplicitlyTypedList<Variable>> const explicitly_typed_list_variables =
        "explicitly_typed_list_variables";
    auto const explicitly_typed_list_variables_def = +variable >> '-' >> type;

    rule<struct TImplicitlyTypedListVariables,
         ImplicitlyTypedList<Variable>> const implicitly_typed_list_variables =
        "implicitly_typed_list_variables";
    auto const implicitly_typed_list_variables_def = *variable;

    rule<struct TTypedListVariables, TypedList<Variable>> const
        typed_list_variables = "typed_list_variables";
    auto const typed_list_variables_def =
        *explicitly_typed_list_variables >> -implicitly_typed_list_variables;

    BOOST_SPIRIT_DEFINE(explicitly_typed_list_variables,
                        implicitly_typed_list_variables,
                        typed_list_variables);

    struct TExplicitlyTypedListVariables : x3::annotate_on_success {};
    struct TImplicitlyTypedListVariables : x3::annotate_on_success {};
    struct TTypedListVariables : x3::annotate_on_success {};

    // Atomic formula skeleton
    rule<struct TAtomicFormulaSkeleton, AtomicFormulaSkeleton> const
        atomic_formula_skeleton = "atomic_formula_skeleton";
    auto const atomic_formula_skeleton_def =
        '(' >> name >> typed_list_variables >> ')';
    BOOST_SPIRIT_DEFINE(atomic_formula_skeleton);
    struct TAtomicFormulaSkeleton : x3::annotate_on_success {};

    // Term
    rule<struct TTerm, Term> const term = "term";
    auto const term_def = constant | variable;
    BOOST_SPIRIT_DEFINE(term);
    struct TTerm : x3::annotate_on_success {};

    // Atomic formula of terms
    rule<struct TAtomicFormulaTerms, AtomicFormula<Term>> const
        atomic_formula_terms = "atomic_formula_terms";
    auto const atomic_formula_terms_def = '(' >> predicate >> *term >> ')';
    BOOST_SPIRIT_DEFINE(atomic_formula_terms);
    struct TAtomicFormulaTerms: x3::annotate_on_success {};

    // Literals of terms
    rule<struct TLiteralTerms, Literal<Term>> const literal_terms =
                                 "literal_terms";
    auto const literal_terms_def = atomic_formula_terms;
    BOOST_SPIRIT_DEFINE(literal_terms);
    struct TLiteralTerms: x3::annotate_on_success {};

    // Atomic formula of names
    rule<struct TAtomicFormulaNames, AtomicFormula<Name>> const
        atomic_formula_names = "atomic_formula_names";
    auto const atomic_formula_names_def = '(' >> predicate >> *name >> ')';
    BOOST_SPIRIT_DEFINE(atomic_formula_names);
    struct TAtomicFormulaNames: x3::annotate_on_success {};

    // Literals of names
    rule<struct TLiteralNames, Literal<Name>> const literal_names =
                                 "literal_names";
    auto const literal_names_def = atomic_formula_names;
    BOOST_SPIRIT_DEFINE(literal_names);
    struct TLiteralNames: x3::annotate_on_success {};

    // Negative literals
    auto parse_negative_literal = [](auto& ctx) {
          _val(ctx).predicate = _attr(ctx).predicate;
          _val(ctx).args = _attr(ctx).args;
          _val(ctx).is_negative = true;
    };

    rule<struct TNegativeLiteralTerms, Literal<Term>> const negative_literal_terms = "negative_literal_terms";
    auto const negative_literal_terms_def = ('(' >> lit("not") >> literal_terms >> ')')[parse_negative_literal];
    BOOST_SPIRIT_DEFINE(negative_literal_terms);
    struct TNegativeLiteralTerms: x3::annotate_on_success {};



    // Nil
    rule<struct TNil, Nil> const nil = "nil";
    auto const nil_def = '(' >> lit(")");
    BOOST_SPIRIT_DEFINE(nil);
    struct TNil: x3::annotate_on_success {};

    rule<struct TSentence, Sentence> sentence = "sentence";

    // Connectors (and/or)
    struct connector_ : x3::symbols<std::string> {
        connector_() {
            add
                ("and", "and")
                ("or" , "or")
            ;
        }
    } connector;


    rule<struct TConnectedSentence, ConnectedSentence> const connected_sentence =
                                  "connected_sentence";
    auto const connected_sentence_def = '('
                               >> connector
                               >> *sentence
                               >> ')';
    BOOST_SPIRIT_DEFINE(connected_sentence);
    struct TConnectedSentence: x3::annotate_on_success {};


    rule<struct TNotSentence, NotSentence> const not_sentence =
                                  "not_sentence";
    auto const not_sentence_def = '('
                               >> lit("not")
                               >> sentence
                               >> ')';
    BOOST_SPIRIT_DEFINE(not_sentence);
    struct TNotSentence: x3::annotate_on_success {};


    rule<struct TImplySentence, ImplySentence> const imply_sentence =
                                   "imply_sentence";
    auto const imply_sentence_def = ('(' >> lit("imply"))
                                    > sentence
                                    > sentence
                                    > ')';
    BOOST_SPIRIT_DEFINE(imply_sentence);
    struct TImplySentence: x3::annotate_on_success {};

    // Quantifier (exists/forall)
    struct quantifier_ : x3::symbols<std::string> {
        quantifier_() {
            add
                ("exists", "exists")
                ("forall" , "forall")
            ;
        }
    } quantifier;

    rule<struct TQuantifiedSentence, QuantifiedSentence> const quantified_sentence =
                                   "quantified_sentence";
    auto const quantified_sentence_def = '('
                               > quantifier
                               > '('
                               > typed_list_variables
                               > ')'
                               > sentence
                               > ')';
    BOOST_SPIRIT_DEFINE(quantified_sentence);
    struct TQuantifiedSentence: x3::annotate_on_success {};

    rule<struct TEqualsSentence, EqualsSentence> const equals_sentence =
                                   "equals_sentence";
    auto const equals_sentence_def = ('(' >> lit("="))
                                 > term
                                 > term
                                 > ')';
    BOOST_SPIRIT_DEFINE(equals_sentence);
    struct TEqualsSentence : x3::annotate_on_success {};

    rule<struct TNotEqualsSentence, NotEqualsSentence> const not_equals_sentence =
                                   "not_equals_sentence";
    auto const not_equals_sentence_def = ('('
                               >> lit("not"))
                               > equals_sentence
                               > ')';
    BOOST_SPIRIT_DEFINE(not_equals_sentence);
    struct TNotEqualsSentence: x3::annotate_on_success {};


    //connected_sentence must be tried before literal_terms. Both start with
    //'(' followed by a name, and literal_terms would otherwise match an empty
    //"(and)" -- the conventional PDDL spelling of "no precondition" -- as a
    //literal whose predicate is "and", which reaches Z3 as a nullary (and) and
    //is rejected. A non-empty "(and ...)" already falls through to
    //connected_sentence because its nested parens do not parse as terms.
    //Ordering it first is safe: connector only admits "and"/"or", and a
    //predicate merely starting with those (e.g. "android") backtracks out.
    auto const sentence_def =
        nil
        | connected_sentence
        | literal_terms
        | not_sentence
        | imply_sentence
        | equals_sentence // Note: HDDL has equals sentences, but PDDL 2.1 does not.
        | quantified_sentence
        ;
    BOOST_SPIRIT_DEFINE(sentence);
    struct TSentence : x3::annotate_on_success {};

    // <p-effect>
    rule<struct TPEffect, Literal<Term>> const p_effect = "p_effect";
    auto const p_effect_def = literal_terms | negative_literal_terms;
    BOOST_SPIRIT_DEFINE(p_effect);
    struct TPEffect: x3::annotate_on_success {};

    // <cond-effect>
    rule<struct TCondEffect, CondEffect> const cond_effect = "cond_effect";
    auto const cond_effect_def = p_effect | '(' >> lit("and") >> *p_effect >> ')';
    BOOST_SPIRIT_DEFINE(cond_effect);
    struct TCondEffect: x3::annotate_on_success {};

    // <effect and <c-effect>
    rule<struct TEffect, Effect> const effect = "effect";
    rule<struct TCEffect, CEffect> const c_effect = "c_effect";

    rule<struct TForallCEffect, ForallCEffect> const forall_c_effect = "forall_c_effect";
    auto const forall_c_effect_def = ('(' >> lit("forall")) > '(' >> typed_list_variables >> ')' >> effect > ')';
    BOOST_SPIRIT_DEFINE(forall_c_effect);
    struct TForallCEffect: x3::annotate_on_success {};

    rule<struct TAndCEffect, AndCEffect> const and_c_effect = "and_c_effect";
    auto const and_c_effect_def = ('(' >> lit("and")) > *c_effect > ')';
    BOOST_SPIRIT_DEFINE(and_c_effect);
    struct TAndCEffect: x3::annotate_on_success {};

    rule<struct TWhenCEffect, WhenCEffect> const when_c_effect = "when_c_effect";
    auto const when_c_effect_def = ('(' >> lit("when")) > sentence > cond_effect >> ')';
    BOOST_SPIRIT_DEFINE(when_c_effect);
    struct TWhenCEffect: x3::annotate_on_success {};

    auto const c_effect_def = forall_c_effect | when_c_effect | p_effect;
    auto const effect_def =
        nil
        | and_c_effect
        | c_effect;

    BOOST_SPIRIT_DEFINE(c_effect);
    BOOST_SPIRIT_DEFINE(effect);
    struct TEffect: x3::annotate_on_success {};
    struct TCEffect: x3::annotate_on_success {};


    // Typed Lists
    rule<struct TTypes, TypedList<Name>> const types = "types";
    auto const types_def = ('(' >> lit(":types"))
                               > typed_list_names
                               > ')';
    BOOST_SPIRIT_DEFINE(types);
    struct TTypes: x3::annotate_on_success {};

    rule<struct TConstants, TypedList<Name>> const constants = "constants";
    auto const constants_def = ('(' >> lit(":constants"))
                               > typed_list_names
                               > ')';
    BOOST_SPIRIT_DEFINE(constants);
    struct TConstants : x3::annotate_on_success {};

    rule<struct TPredicates, std::vector<AtomicFormulaSkeleton>> const
        predicates = "predicates";
    auto const predicates_def = ('(' >> lit(":predicates"))
                               > +atomic_formula_skeleton > ')';
    BOOST_SPIRIT_DEFINE(predicates);
    struct TPredicates: x3::annotate_on_success {};

    rule<struct TPrecondition, Sentence> const precondition = "precondition";
    auto const precondition_def = lit(":precondition")
                               > sentence;
    BOOST_SPIRIT_DEFINE(precondition);
    struct TPrecondition: x3::annotate_on_success {};

    rule<struct TParameters, TypedList<Variable>> const parameters = "parameters";
    auto const parameters_def = lit(":parameters")
                               > '('
                               > typed_list_variables
                               > ')';
    BOOST_SPIRIT_DEFINE(parameters);
    struct TParameters: x3::annotate_on_success {};

    // A task, action or method with no parameters may leave out
    // ":parameters ()" -- optional in PDDL's action definition, and what a
    // writer of HDDL reasonably expects of a task or method too.
    auto const parameters_or_none = parameters | x3::attr(TypedList<Variable>{});

    rule<struct TTask, Task> const task = "task";
    auto const task_def = name >> parameters_or_none;
    BOOST_SPIRIT_DEFINE(task);
    struct TTask: x3::annotate_on_success {};


    // Abstract Tasks
    rule<struct TAbstractTask, Task> const abstract_task = "abstract_task";
    auto const abstract_task_def = ('(' >> lit(":task")) > task >> ')';
    BOOST_SPIRIT_DEFINE(abstract_task);
    struct TAbstractTask: x3::annotate_on_success {};


    rule<struct TTaskSymbolWithTerms, MTask> const task_symbol_with_terms = "task_symbol_with_terms";
    auto const task_symbol_with_terms_def = '(' >> name >> *term >> ')';
    BOOST_SPIRIT_DEFINE(task_symbol_with_terms);
    struct TTaskSymbolWithTerms: x3::annotate_on_success {};

    // Methods used to decompose abstract tasks into primitive actions
    // task as defined in Method struct != task defined in task struct
    // mtask refers to task definition found within a method:
    rule<struct TMTask, MTask> const mtask = "mtask";
    auto const mtask_def = lit(":task") > task_symbol_with_terms;
    BOOST_SPIRIT_DEFINE(mtask);
    struct TMTask: x3::annotate_on_success {};

    rule<struct TSubTaskWithId, SubTaskWithId> const subtask_with_id = "subtask_with_id";
    auto const subtask_with_id_def = '(' >> name >> task_symbol_with_terms >> ')';
    BOOST_SPIRIT_DEFINE(subtask_with_id);
    struct TSubTaskWithId: x3::annotate_on_success {};

    rule<struct TSubTask, SubTask> const subtask = "subtask";
    auto const subtask_def = task_symbol_with_terms | subtask_with_id;
    BOOST_SPIRIT_DEFINE(subtask);
    struct TSubTask: x3::annotate_on_success {};

    rule<struct TSubTasks, SubTasks> const subtasks = "subtasks";
    auto const subtasks_def = nil | subtask | '(' >> lit("and") >> +subtask >> ')';
    BOOST_SPIRIT_DEFINE(subtasks);
    struct TSubTasks: x3::annotate_on_success {};

    rule<struct TOrdering, Ordering> const ordering = "ordering";
    auto const ordering_def = '(' >> lit("<") >> name >> name >> ')'; //make sure to use lit("")

    BOOST_SPIRIT_DEFINE(ordering);
    struct TOrdering: x3::annotate_on_success {};

    rule<struct TOrderings, Orderings> const orderings = "orderings";
    auto const orderings_def = nil | ordering | '(' >> lit("and") >> +ordering >> ')';
    BOOST_SPIRIT_DEFINE(orderings);
    struct TOrderings: x3::annotate_on_success {};

    rule<struct TTaskNetworkOrderings, Orderings> const task_network_orderings = "task_network_orderings";
    // HDDL spells the keyword :order[ing]. The short form must not match the
    // start of a longer keyword.
    auto const task_network_orderings_def =
        (lit(":ordering") | (lit(":order") >> !(alnum | char_("-_")))) > orderings;
    BOOST_SPIRIT_DEFINE(task_network_orderings);
    struct TTaskNetworkOrderings: x3::annotate_on_success {};

    // Ordering keyword
    struct ordering_kw_ : x3::symbols<std::string> {
        ordering_kw_() {
            add
                ("tasks", "tasks")
                ("subtasks" , "subtasks")
                ("ordered-tasks" , "ordered-tasks")
                ("ordered-subtasks" , "ordered-subtasks")
            ;
        }
    } ordering_kw;

    rule<struct TMethodSubTasks, MethodSubTasks> const method_subtasks = "method_subtasks";
    auto const method_subtasks_def = ':' >> ordering_kw >> subtasks;
    BOOST_SPIRIT_DEFINE(method_subtasks);
    struct TMethodSubTasks : x3::annotate_on_success {};


    rule<struct TConstraint, Constraint> const constraint = "constraint";
    auto const constraint_def = nil | not_equals_sentence | equals_sentence;
    BOOST_SPIRIT_DEFINE(constraint);
    struct TConstraint : x3::annotate_on_success {};

    rule<struct TConstraints, Constraints> const constraints = "constraints";
    auto const constraints_def = nil | constraint | '(' >> lit("and") >> +constraint >> ')';
    BOOST_SPIRIT_DEFINE(constraints);
    struct TConstraints : x3::annotate_on_success {};

    rule<struct TTaskNetwork, TaskNetwork> const task_network = "task_network";
    auto const task_network_def = -method_subtasks
                               >> -task_network_orderings
                               >> -(lit(":constraints") > constraints);
    BOOST_SPIRIT_DEFINE(task_network);
    struct TTaskNetwork: x3::annotate_on_success {};

    rule<struct TMethod, Method> const method = "method";
    auto const method_def = ('(' >> lit(":method"))
                                > name
                                > parameters_or_none
                                > mtask // one task
                                >> -precondition
                                > task_network
                                > ')';
    BOOST_SPIRIT_DEFINE(method);
    struct TMethod: x3::annotate_on_success {};


    // Primitive actions

    // Note: There is a typo in section 4 of the HDDL paper
    // https://arxiv.org/pdf/1911.05499.pdf - in the specification of actions,
    // it should be ':effect' instead of ':effects' (line 44 of their listing).
    rule<struct TAction, Action> const action = "action";
    auto const action_def = ('(' >> lit(":action"))
                               > name
                               > parameters_or_none
                               >> -precondition
                               >> -(lit(":effect") >> effect)
                               > ')';
    BOOST_SPIRIT_DEFINE(action);
    struct TAction: x3::annotate_on_success {};

    // Domain definition
    rule<struct TDomain, Domain> const domain = "domain";
    // Every step is an expectation (>) rather than a sequence (>>). In C++ >>
    // binds tighter than >, so "a > b >> c >> d > e" groups b, c and d into
    // one anonymous parser, and a failure anywhere in it was reported against
    // that parser's C++ type -- a missing ')' after the domain name came out
    // as "expecting the rest of a requirements". The optional and repeated
    // parts cannot fail, so expecting them changes nothing else.
    auto const domain_def = ('(' >> lit("define")) > '(' > lit("domain")
                               > name > ')'
                               > -requirements // optional in HDDL, as in PDDL
                               > -types
                               > -constants
                               > -predicates
                               > *(abstract_task | method | action)
                               > ')';
    BOOST_SPIRIT_DEFINE(domain);
    struct TDomain : x3::annotate_on_success, ErrorHandlerBase {};


    // Problem definition
    rule<struct TObjects, TypedList<Name>> const objects = "objects";
    auto const objects_def = ('(' >> lit(":objects"))
                               > typed_list_names
                               > ')';
    BOOST_SPIRIT_DEFINE(objects);
    struct TObjects: x3::annotate_on_success {};

    // <p-init> ::= (:init <init-el>*)
    // <init-el> ::= <literal (name)>
    rule<struct TInit, Init> const init = "init";
    auto const init_def = ('(' >> lit(":init"))
                               >> *literal_names
                               >> ')';
    BOOST_SPIRIT_DEFINE(init);
    struct TInit: x3::annotate_on_success {};

    rule<struct TGoal, Sentence> const goal = "goal";
    auto const goal_def = ('(' >> lit(":goal"))
                               > sentence
                               > ')';
    BOOST_SPIRIT_DEFINE(goal);
    struct TGoal: x3::annotate_on_success {};

    // Problem class. For the first version of HDDL, only :htn is allowed.
    // Later versions might allow different classes to denote problems with
    // task insertion, etc.
    struct problem_class_ : x3::symbols<std::string> {
        problem_class_() {
            add
                (":htn" , ":htn")
            ;
        }
    } problem_class;

    rule<struct TProblemHTN, ProblemHTN> problem_htn = "problem_htn";
    auto const problem_htn_def = ('(' >> problem_class)
                               > -parameters
                               > task_network > ')';
    BOOST_SPIRIT_DEFINE(problem_htn);
    struct TProblemHTN: x3::annotate_on_success {};

    rule<struct TProblem, Problem> const problem = "problem";
    // Expectations throughout, for the reason given at domain_def.
    auto const problem_def = ('(' >> lit("define"))
                               > '(' > lit("problem") > name > ')'
                               > '(' > lit(":domain") > name > ')'
                               > -requirements
                               > -objects
                               > -problem_htn
                               > init
                               > -goal
                               > ')';
    BOOST_SPIRIT_DEFINE(problem);
    struct TProblem : x3::annotate_on_success, ErrorHandlerBase {};


} // namespace parser

parser::type_type type() { return parser::type; }
parser::literal_terms_type literal_terms() { return parser::literal_terms; }
parser::sentence_type sentence() { return parser::sentence; }
parser::domain_type domain() { return parser::domain; }
parser::problem_type problem() { return parser::problem; }
