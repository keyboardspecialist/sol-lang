#include "sol/diagnostic.h"
#include "sol/lexer.h"
#include "sol/parser.h"
#include "sol/source.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            ++failures; \
        } \
    } while (0)

static bool span_text_equal(const SolSource *source, SolSpan span, const char *text) {
    size_t length = span.end - span.start;
    return strlen(text) == length
        && memcmp(source->text + span.start, text, length) == 0;
}

static void check_ast_links(const SolSyntaxTree *tree) {
    for (size_t index = 0; index < tree->item_count; ++index) {
        CHECK(tree->items[index].body == SOL_AST_NONE
            || tree->items[index].body < tree->expression_count);
        CHECK(tree->items[index].first_parameter == SOL_AST_NONE
            || tree->items[index].first_parameter < tree->parameter_count);
        CHECK(tree->items[index].return_type_id == SOL_AST_NONE
            || tree->items[index].return_type_id < tree->type_count);
        CHECK(tree->items[index].first_field == SOL_AST_NONE
            || tree->items[index].first_field < tree->field_count);
        CHECK(tree->items[index].first_variant == SOL_AST_NONE
            || tree->items[index].first_variant < tree->variant_count);
        CHECK(tree->items[index].first_member == SOL_AST_NONE
            || tree->items[index].first_member < tree->capability_member_count);
        CHECK(tree->items[index].capability_source == SOL_AST_NONE
            || tree->items[index].capability_source < tree->parameter_count);
        CHECK(tree->items[index].first_contract == SOL_AST_NONE
            || tree->items[index].first_contract < tree->contract_clause_count);
        CHECK(tree->items[index].first_type_parameter == SOL_AST_NONE
            || tree->items[index].first_type_parameter < tree->type_parameter_count);
    }
    for (size_t index = 0; index < tree->expression_count; ++index) {
        const SolExpr *expression = &tree->expressions[index];
        switch (expression->kind) {
            case SOL_EXPR_UNARY:
                CHECK(expression->as.unary.operand < tree->expression_count);
                break;
            case SOL_EXPR_BINARY:
                CHECK(expression->as.binary.left < tree->expression_count);
                CHECK(expression->as.binary.right < tree->expression_count);
                break;
            case SOL_EXPR_CALL:
                CHECK(expression->as.call.callee < tree->expression_count);
                CHECK(expression->as.call.first_argument == SOL_AST_NONE
                    || expression->as.call.first_argument < tree->argument_count);
                break;
            case SOL_EXPR_TYPE_APPLICATION:
                CHECK(expression->as.type_application.base < tree->expression_count);
                CHECK(expression->as.type_application.first_argument
                    < tree->type_argument_count);
                break;
            case SOL_EXPR_FIELD:
                CHECK(expression->as.field.base < tree->expression_count);
                break;
            case SOL_EXPR_RECORD:
                CHECK(expression->as.record.type < tree->expression_count);
                CHECK(expression->as.record.first_field == SOL_AST_NONE
                    || expression->as.record.first_field < tree->argument_count);
                break;
            case SOL_EXPR_IF:
                CHECK(expression->as.if_expr.condition < tree->expression_count);
                CHECK(expression->as.if_expr.then_branch < tree->expression_count);
                CHECK(expression->as.if_expr.else_branch < tree->expression_count);
                break;
            case SOL_EXPR_MATCH:
                CHECK(expression->as.match_expr.scrutinee < tree->expression_count);
                CHECK(expression->as.match_expr.first_arm == SOL_AST_NONE
                    || expression->as.match_expr.first_arm < tree->match_arm_count);
                break;
            case SOL_EXPR_BLOCK:
                CHECK(expression->as.block.first_statement == SOL_AST_NONE
                    || expression->as.block.first_statement < tree->statement_count);
                break;
            case SOL_EXPR_PROPAGATE:
                CHECK(expression->as.propagated < tree->expression_count);
                break;
            case SOL_EXPR_HANDLE:
                CHECK(expression->as.handle.authority < tree->expression_count);
                CHECK(expression->as.handle.provider < tree->expression_count);
                CHECK(expression->as.handle.body < tree->expression_count);
                CHECK(tree->expressions[expression->as.handle.body].kind == SOL_EXPR_BLOCK);
                break;
            case SOL_EXPR_OLD:
                CHECK(expression->as.old_expression < tree->expression_count);
                break;
            default:
                break;
        }
    }
    for (size_t index = 0; index < tree->argument_count; ++index) {
        CHECK(tree->arguments[index].value < tree->expression_count);
        CHECK(tree->arguments[index].next == SOL_AST_NONE
            || tree->arguments[index].next < tree->argument_count);
    }
    for (size_t index = 0; index < tree->statement_count; ++index) {
        const SolStatement *statement = &tree->statements[index];
        CHECK(statement->next == SOL_AST_NONE || statement->next < tree->statement_count);
        if (statement->kind == SOL_STATEMENT_LET) {
            CHECK(statement->as.let_statement.value < tree->expression_count);
        } else {
            CHECK(statement->as.expression < tree->expression_count);
        }
    }
    for (size_t index = 0; index < tree->parameter_count; ++index) {
        CHECK(tree->parameters[index].type_id < tree->type_count);
        CHECK(tree->parameters[index].next == SOL_AST_NONE
            || tree->parameters[index].next < tree->parameter_count);
    }
    for (size_t index = 0; index < tree->type_count; ++index) {
        CHECK(tree->types[index].first_argument == SOL_AST_NONE
            || tree->types[index].first_argument < tree->type_argument_count);
        if (tree->types[index].kind == SOL_SYNTAX_TYPE_FUNCTION) {
            CHECK(tree->types[index].return_type < tree->type_count);
            CHECK(tree->types[index].first_effect == SOL_AST_NONE
                || tree->types[index].first_effect < tree->effect_count);
        }
    }
    for (size_t index = 0; index < tree->type_argument_count; ++index) {
        CHECK(tree->type_arguments[index].type < tree->type_count);
        CHECK(tree->type_arguments[index].next == SOL_AST_NONE
            || tree->type_arguments[index].next < tree->type_argument_count);
    }
    for (size_t index = 0; index < tree->type_parameter_count; ++index) {
        CHECK(tree->type_parameters[index].owner_item < tree->item_count);
        CHECK(tree->type_parameters[index].next == SOL_AST_NONE
            || tree->type_parameters[index].next < tree->type_parameter_count);
    }
    for (size_t index = 0; index < tree->field_count; ++index) {
        CHECK(tree->fields[index].type < tree->type_count);
        CHECK(tree->fields[index].next == SOL_AST_NONE
            || tree->fields[index].next < tree->field_count);
    }
    for (size_t index = 0; index < tree->variant_count; ++index) {
        CHECK(tree->variants[index].first_field == SOL_AST_NONE
            || tree->variants[index].first_field < tree->field_count);
        CHECK(tree->variants[index].next == SOL_AST_NONE
            || tree->variants[index].next < tree->variant_count);
    }
    for (size_t index = 0; index < tree->pattern_count; ++index) {
        CHECK(tree->patterns[index].first_binding == SOL_AST_NONE
            || tree->patterns[index].first_binding < tree->pattern_binding_count);
    }
    for (size_t index = 0; index < tree->pattern_binding_count; ++index) {
        CHECK(tree->pattern_bindings[index].next == SOL_AST_NONE
            || tree->pattern_bindings[index].next < tree->pattern_binding_count);
    }
    for (size_t index = 0; index < tree->match_arm_count; ++index) {
        CHECK(tree->match_arms[index].pattern < tree->pattern_count);
        CHECK(tree->match_arms[index].value < tree->expression_count);
        CHECK(tree->match_arms[index].next == SOL_AST_NONE
            || tree->match_arms[index].next < tree->match_arm_count);
    }
    for (size_t index = 0; index < tree->effect_count; ++index) {
        const SolEffect *effect = &tree->effects[index];
        CHECK(effect->next == SOL_AST_NONE || effect->next < tree->effect_count);
        CHECK((effect->owner_kind == SOL_EFFECT_OWNER_ITEM
                && effect->owner < tree->item_count)
            || (effect->owner_kind == SOL_EFFECT_OWNER_CAPABILITY_MEMBER
                && effect->owner < tree->capability_member_count)
            || (effect->owner_kind == SOL_EFFECT_OWNER_TYPE
                && effect->owner < tree->type_count));
    }
    for (size_t index = 0; index < tree->capability_member_count; ++index) {
        CHECK(tree->capability_members[index].first_parameter == SOL_AST_NONE
            || tree->capability_members[index].first_parameter < tree->parameter_count);
        CHECK(tree->capability_members[index].return_type_id < tree->type_count);
        CHECK(tree->capability_members[index].first_effect == SOL_AST_NONE
            || tree->capability_members[index].first_effect < tree->effect_count);
        CHECK(tree->capability_members[index].body == SOL_AST_NONE
            || tree->capability_members[index].body < tree->expression_count);
        CHECK(tree->capability_members[index].next == SOL_AST_NONE
            || tree->capability_members[index].next < tree->capability_member_count);
        CHECK(tree->capability_members[index].first_contract == SOL_AST_NONE
            || tree->capability_members[index].first_contract
                < tree->contract_clause_count);
    }
    for (size_t index = 0; index < tree->contract_clause_count; ++index) {
        CHECK(tree->contract_clauses[index].first_condition == SOL_AST_NONE
            || tree->contract_clauses[index].first_condition
                < tree->contract_condition_count);
        CHECK(tree->contract_clauses[index].next == SOL_AST_NONE
            || tree->contract_clauses[index].next < tree->contract_clause_count);
    }
    for (size_t index = 0; index < tree->contract_condition_count; ++index) {
        CHECK(tree->contract_conditions[index].expression < tree->expression_count);
        CHECK(tree->contract_conditions[index].owner_clause < tree->contract_clause_count);
        CHECK(tree->contract_conditions[index].next == SOL_AST_NONE
            || tree->contract_conditions[index].next < tree->contract_condition_count);
    }
}

static void test_valid_declarations(void) {
    static const char source_text[] =
        "module demo.main\n"
        "edition 2027\n"
        "use core.Text\n"
        "@stable(\"demo.User.v1\")\n"
        "public record User { name: Text, }\n"
        "enum GreetError { invalid(field: Text), }\n"
        "capability Clock {\n"
        "    function now() -> Int64\n"
        "    effects { clock.read<Self> }\n"
        "}\n"
        "public function greet(user: User, clock: capability Clock) -> Result<Text, GreetError>\n"
        "effects { clock.read<clock> }\n"
        "requires { true }\n"
        "ensures { true } {\n"
        "    return ok(user.name)\n"
        "}\n";

    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "valid.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(!sol_diagnostics_has_errors(&diagnostics));
    CHECK(tree.edition == 2027);
    CHECK(tree.item_count == 4);
    CHECK(tree.capability_member_count == 1);
    CHECK(tree.items[2].first_member == 0);
    if (tree.capability_member_count == 1) {
        const SolCapabilityMember *member = &tree.capability_members[0];
        CHECK(member->owner_item == 2);
        CHECK(member->first_effect < tree.effect_count);
        CHECK(member->return_type_id < tree.type_count);
        CHECK(member->next == SOL_AST_NONE);
    }

    size_t covered = 0;
    for (size_t index = 0; index + 1 < tokens.count; ++index) {
        CHECK(tokens.items[index].span.start == covered);
        covered = tokens.items[index].span.end;
    }
    CHECK(covered == source.length);

    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_generic_syntax_and_comparison_disambiguation(void) {
    static const char source_text[] =
        "module generic_syntax\n"
        "record Box<T,> { value: T, }\n"
        "enum Either<L, R,> { left(value: L), right(value: R), }\n"
        "function identity<T,>(value: T) -> T { return value }\n"
        "function sample(a: Int64, b: Int64, c: Int64) -> Bool {\n"
        "    let box = Box<Int64> { value = identity<Int64>(1), }\n"
        "    let left = Either<Int64, Text,>.left(value = box.value)\n"
        "    return a < b && b > c\n"
        "}\n"
        "function function_head(\n"
        "    callback: function(Int64) -> Int64 effects { pure },\n"
        ") -> Int64 {\n"
        "    return identity<function(Int64) -> Int64 effects { pure }>(callback)(1)\n"
        "}\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "generic_syntax.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    if (sol_diagnostics_has_errors(&diagnostics)) {
        sol_diagnostics_render_human(stderr, &source, &diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&diagnostics));
    CHECK(tree.type_parameter_count == 4);
    size_t applications = 0;
    size_t comparisons = 0;
    for (size_t index = 0; index < tree.expression_count; ++index) {
        applications += tree.expressions[index].kind == SOL_EXPR_TYPE_APPLICATION ? 1 : 0;
        comparisons += tree.expressions[index].kind == SOL_EXPR_BINARY
            && (tree.expressions[index].as.binary.operator_kind == SOL_TOKEN_LESS
                || tree.expressions[index].as.binary.operator_kind == SOL_TOKEN_GREATER)
            ? 1
            : 0;
    }
    CHECK(applications == 4);
    CHECK(comparisons == 2);
    check_ast_links(&tree);
    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_unsupported_generic_syntax(void) {
    static const char source_text[] =
        "module unsupported_generics\n"
        "record Bounded<T: Bound> {}\n"
        "function defaulted<T = Int64>(value: T) -> T { return value }\n"
        "capability Generic<T> {}\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "unsupported_generics.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    size_t unsupported = 0;
    for (size_t index = 0; index < diagnostics.count; ++index) {
        unsupported += strcmp(diagnostics.items[index].code, "SOL-PARSE-018") == 0 ? 1 : 0;
    }
    CHECK(unsupported == 3);
    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_generic_lookahead_recovery_and_spans(void) {
    static const char valid_text[] =
        "module generic_lookahead\n"
        "function identity<T>(value: T) -> T { return value }\n"
        "function comparisons(a: Int64, b: Int64, c: Int64) -> Bool {\n"
        "    let value = identity<Option<Int64>>(some(1))\n"
        "    return a<b && b>c\n"
        "}\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "generic_lookahead.sol", valid_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(!sol_diagnostics_has_errors(&diagnostics));
    size_t applications = 0;
    size_t comparisons = 0;
    for (size_t index = 0; index < tree.expression_count; ++index) {
        const SolExpr *expression = &tree.expressions[index];
        if (expression->kind == SOL_EXPR_TYPE_APPLICATION) {
            ++applications;
            CHECK(span_text_equal(&source, expression->span, "identity<Option<Int64>>"));
        }
        comparisons += expression->kind == SOL_EXPR_BINARY
            && (expression->as.binary.operator_kind == SOL_TOKEN_LESS
                || expression->as.binary.operator_kind == SOL_TOKEN_GREATER)
            ? 1
            : 0;
    }
    CHECK(applications == 1);
    CHECK(comparisons == 2);
    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);

    static const char malformed_text[] =
        "module malformed_application\n"
        "function identity<T>(value: T) -> T { return value }\n"
        "function sample() -> Int64 { return identity<Int64(1) }\n";
    CHECK(sol_source_from_text(&source, "malformed_application.sol", malformed_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    size_t malformed = 0;
    size_t calls = 0;
    for (size_t index = 0; index < diagnostics.count; ++index) {
        malformed += strcmp(diagnostics.items[index].code, "SOL-PARSE-019") == 0 ? 1 : 0;
    }
    for (size_t index = 0; index < tree.expression_count; ++index) {
        calls += tree.expressions[index].kind == SOL_EXPR_CALL ? 1 : 0;
    }
    CHECK(malformed == 1);
    CHECK(calls == 1);
    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);

    static const char nested_bound_text[] =
        "module nested_bound\n"
        "record Bounded<T: Option<Result<Int64, Text>>> {}\n"
        "function following() -> Int64 { return 1 }\n";
    CHECK(sol_source_from_text(&source, "nested_bound.sol", nested_bound_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(tree.item_count == 2);
    CHECK(diagnostics.count == 1);
    if (diagnostics.count == 1) {
        CHECK(strcmp(diagnostics.items[0].code, "SOL-PARSE-018") == 0);
    }
    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_missing_module(void) {
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "missing.sol", "record Value { value: Int64, }\n"));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(sol_diagnostics_has_errors(&diagnostics));
    CHECK(diagnostics.count >= 1);
    if (diagnostics.count >= 1) {
        CHECK(strcmp(diagnostics.items[0].code, "SOL-PARSE-004") == 0);
    }

    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_nested_and_unterminated_comments(void) {
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    CHECK(sol_source_from_text(&source, "comments.sol", "/* outer /* inner */"));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_diagnostics_has_errors(&diagnostics));
    CHECK(diagnostics.count == 1);
    if (diagnostics.count == 1) {
        CHECK(strcmp(diagnostics.items[0].code, "SOL-LEX-003") == 0);
    }

    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_clause_order(void) {
    static const char source_text[] =
        "module ordering\n"
        "function value() -> Int64\n"
        "ensures { true }\n"
        "effects { pure } { return 1 }\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "ordering.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(sol_diagnostics_has_errors(&diagnostics));
    bool found = false;
    for (size_t index = 0; index < diagnostics.count; ++index) {
        found = found || strcmp(diagnostics.items[index].code, "SOL-PARSE-009") == 0;
    }
    CHECK(found);

    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_structured_contract_syntax(void) {
    static const char source_text[] =
        "module contracts\n"
        "capability Gate {\n"
        "    function permit(value: Int64) -> Bool\n"
        "    requires { value > 0 }\n"
        "    ensures { result == old(value) }\n"
        "}\n"
        "function check(value: Int64, ready: Bool) -> Result<Bool, Bool>\n"
        "requires {\n"
        "    value > 0, ready\n"
        "}\n"
        "ensures {\n"
        "    success => result == ready\n"
        "    failure => old(value) == value\n"
        "} { return ok(ready) }\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "contracts.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    if (sol_diagnostics_has_errors(&diagnostics)) {
        sol_diagnostics_render_human(stderr, &source, &diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&diagnostics));
    CHECK(tree.contract_clause_count == 4);
    CHECK(tree.contract_condition_count == 6);
    if (tree.item_count == 2 && tree.capability_member_count == 1
        && tree.contract_clause_count == 4 && tree.contract_condition_count == 6) {
        CHECK(tree.items[0].first_contract == SOL_AST_NONE);
        CHECK(tree.capability_members[0].first_contract == 0);
        CHECK(tree.items[1].first_contract == 2);
        CHECK(tree.contract_clauses[0].kind == SOL_CONTRACT_REQUIRES);
        CHECK(tree.contract_clauses[0].owner_kind
            == SOL_CONTRACT_OWNER_CAPABILITY_MEMBER);
        CHECK(tree.contract_clauses[0].owner == 0);
        CHECK(tree.contract_clauses[1].kind == SOL_CONTRACT_ENSURES);
        CHECK(tree.contract_clauses[2].next == 3);
        CHECK(tree.contract_clauses[2].owner_kind == SOL_CONTRACT_OWNER_ITEM);
        CHECK(tree.contract_clauses[2].owner == 1);
        CHECK(tree.contract_conditions[4].outcome == SOL_CONTRACT_OUTCOME_SUCCESS);
        CHECK(tree.contract_conditions[5].outcome == SOL_CONTRACT_OUTCOME_FAILURE);
    }
    size_t old_count = 0;
    size_t result_count = 0;
    for (size_t index = 0; index < tree.expression_count; ++index) {
        old_count += tree.expressions[index].kind == SOL_EXPR_OLD ? 1 : 0;
        result_count += tree.expressions[index].kind == SOL_EXPR_RESULT ? 1 : 0;
    }
    CHECK(old_count == 2);
    CHECK(result_count == 2);
    check_ast_links(&tree);
    CHECK(sol_syntax_contracts_validate(&source, &tree));
    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_contract_diagnostics_and_recovery(void) {
    static const char source_text[] =
        "module malformed_contracts\n"
        "function malformed(value: Int64) -> Bool\n"
        "requires {\n"
        "    old(value)\n"
        "    result\n"
        "    success => true\n"
        "    true false\n"
        "}\n"
        "requires { true }\n"
        "{ return true }\n"
        "function recovered() -> Bool { return true }\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "malformed_contracts.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(sol_diagnostics_has_errors(&diagnostics));
    CHECK(tree.item_count == 2);
    CHECK(tree.contract_clause_count == 2);
    CHECK(tree.contract_condition_count == 6);
    size_t contextual = 0;
    size_t duplicates = 0;
    for (size_t index = 0; index < diagnostics.count; ++index) {
        contextual += strcmp(diagnostics.items[index].code, "SOL-PARSE-017") == 0 ? 1 : 0;
        duplicates += strcmp(diagnostics.items[index].code, "SOL-PARSE-008") == 0 ? 1 : 0;
    }
    CHECK(contextual >= 4);
    CHECK(duplicates == 1);
    CHECK(tree.items[0].first_contract == 0);
    CHECK(tree.contract_clauses[0].next == 1);
    check_ast_links(&tree);
    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_contract_rollback(void) {
    static const char source_text[] =
        "module contract_rollback\n"
        "function broken() -> Bool requires { true }\n"
        "function recovered() -> Bool { return true }\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "contract_rollback.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(sol_diagnostics_has_errors(&diagnostics));
    CHECK(tree.item_count == 1);
    CHECK(tree.contract_clause_count == 0);
    CHECK(tree.contract_condition_count == 0);
    CHECK(tree.items[0].first_contract == SOL_AST_NONE);
    CHECK(sol_syntax_contracts_validate(&source, &tree));
    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_capability_contract_rollback(void) {
    static const char source_text[] =
        "module capability_contract_rollback\n"
        "capability Gate {\n"
        "    function broken() -> Bool requires { true } effects\n"
        "    function recovered() -> Bool requires { true }\n"
        "}\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "capability_contract_rollback.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(sol_diagnostics_has_errors(&diagnostics));
    CHECK(tree.item_count == 1);
    CHECK(tree.capability_member_count == 1);
    CHECK(tree.contract_clause_count == 1);
    CHECK(tree.contract_condition_count == 1);
    if (tree.capability_member_count == 1 && tree.contract_clause_count == 1) {
        CHECK(tree.capability_members[0].first_contract == 0);
        CHECK(tree.contract_clauses[0].owner == 0);
    }
    check_ast_links(&tree);
    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_carriage_return_positions(void) {
    SolSource source;
    CHECK(sol_source_from_text(&source, "cr.sol", "module first\rrecord Value {}\r"));
    SolPosition position = sol_source_position(&source, 13);
    CHECK(position.line == 2);
    CHECK(position.column == 1);
    sol_source_free(&source);
}

static void test_type_depth_limit(void) {
    char source_text[4096];
    size_t used = (size_t)snprintf(
        source_text,
        sizeof(source_text),
        "module depth\nrecord Deep { value: "
    );
    for (size_t index = 0; index < 300 && used + 2 < sizeof(source_text); ++index) {
        source_text[used++] = 'A';
        source_text[used++] = '<';
    }
    source_text[used++] = 'Z';
    for (size_t index = 0; index < 300 && used + 1 < sizeof(source_text); ++index) {
        source_text[used++] = '>';
    }
    source_text[used++] = ',';
    source_text[used++] = '}';
    source_text[used] = '\0';

    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "depth.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    bool found = false;
    for (size_t index = 0; index < diagnostics.count; ++index) {
        found = found || strcmp(diagnostics.items[index].code, "SOL-PARSE-010") == 0;
    }
    CHECK(found);

    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_declaration_recovery(void) {
    static const char source_text[] =
        "module recovery\n"
        "record Broken { value: Int64\n"
        "function recovered() -> Int64 { return 1 }\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "recovery.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(sol_diagnostics_has_errors(&diagnostics));
    CHECK(tree.item_count == 1);
    if (tree.item_count == 1) {
        CHECK(tree.items[0].kind == SOL_ITEM_FUNCTION);
        SolToken name = {.span = tree.items[0].name};
        CHECK(sol_token_text_equal(&source, name, "recovered"));
    }

    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_expression_ast(void) {
    static const char source_text[] =
        "module expressions\n"
        "record Pair { left: Int64, right: Int64, }\n"
        "function choose(a: Int64, b: Int64, ready: Bool) -> Result<Pair, Error>\n"
        "effects { pure } {\n"
        "    let pair = Pair { left = a + b * 2, right = b, }\n"
        "    return if ready {\n"
        "        ok(value = pair)?\n"
        "    } else {\n"
        "        Error.invalid(field = \"ready\")\n"
        "    }\n"
        "}\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "expressions.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(!sol_diagnostics_has_errors(&diagnostics));
    CHECK(tree.item_count == 2);
    check_ast_links(&tree);

    if (tree.item_count == 2) {
        SolExprId body_id = tree.items[1].body;
        CHECK(body_id != SOL_AST_NONE);
        if (body_id != SOL_AST_NONE) {
            SolExpr *body = &tree.expressions[body_id];
            CHECK(body->kind == SOL_EXPR_BLOCK);
            SolStatementId let_id = body->as.block.first_statement;
            CHECK(let_id != SOL_AST_NONE);
            if (let_id != SOL_AST_NONE) {
                SolStatement *let_statement = &tree.statements[let_id];
                CHECK(let_statement->kind == SOL_STATEMENT_LET);
                SolExpr *record = &tree.expressions[let_statement->as.let_statement.value];
                CHECK(record->kind == SOL_EXPR_RECORD);
                SolArgumentId left_field = record->as.record.first_field;
                CHECK(left_field != SOL_AST_NONE);
                if (left_field != SOL_AST_NONE) {
                    SolExpr *sum = &tree.expressions[tree.arguments[left_field].value];
                    CHECK(sum->kind == SOL_EXPR_BINARY);
                    CHECK(sum->as.binary.operator_kind == SOL_TOKEN_PLUS);
                    SolExpr *product = &tree.expressions[sum->as.binary.right];
                    CHECK(product->kind == SOL_EXPR_BINARY);
                    CHECK(product->as.binary.operator_kind == SOL_TOKEN_STAR);
                }

                SolStatementId return_id = let_statement->next;
                CHECK(return_id != SOL_AST_NONE);
                if (return_id != SOL_AST_NONE) {
                    SolStatement *return_statement = &tree.statements[return_id];
                    CHECK(return_statement->kind == SOL_STATEMENT_RETURN);
                    SolExpr *if_expression = &tree.expressions[return_statement->as.expression];
                    CHECK(if_expression->kind == SOL_EXPR_IF);
                    SolExpr *then_block = &tree.expressions[if_expression->as.if_expr.then_branch];
                    SolStatementId then_statement_id = then_block->as.block.first_statement;
                    CHECK(then_statement_id != SOL_AST_NONE);
                    if (then_statement_id != SOL_AST_NONE) {
                        SolExprId propagated_id = tree.statements[then_statement_id].as.expression;
                        CHECK(tree.expressions[propagated_id].kind == SOL_EXPR_PROPAGATE);
                    }
                }
            }
        }
    }

    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_expression_recovery(void) {
    static const char source_text[] =
        "module recovery.expression\n"
        "function broken() -> Int64 { return 1 + }\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "expression_recovery.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(sol_diagnostics_has_errors(&diagnostics));
    CHECK(tree.item_count == 1);
    check_ast_links(&tree);
    bool found = false;
    for (size_t index = 0; index < diagnostics.count; ++index) {
        found = found || strcmp(diagnostics.items[index].code, "SOL-PARSE-011") == 0;
    }
    CHECK(found);

    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_nested_record_condition(void) {
    static const char source_text[] =
        "module condition\n"
        "record Pair { left: Int64, right: Int64, }\n"
        "function compare() -> Int64 {\n"
        "    return if equal(Pair { left = 1, right = 2, }, Pair { left = 1, right = 2, }) {\n"
        "        1\n"
        "    } else {\n"
        "        0\n"
        "    }\n"
        "}\n"
        "function block_condition() -> Int64 {\n"
        "    return if { Pair { left = 1, right = 2, } } {\n"
        "        1\n"
        "    } else {\n"
        "        0\n"
        "    }\n"
        "}\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "condition.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(!sol_diagnostics_has_errors(&diagnostics));
    check_ast_links(&tree);

    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_expression_depth_limit(void) {
    char source_text[16384];
    size_t used = (size_t)snprintf(
        source_text,
        sizeof(source_text),
        "module expression_depth\nfunction deep(value: Bool) -> Int64 { return "
    );
    for (size_t index = 0; index < 300; ++index) {
        used += (size_t)snprintf(source_text + used, sizeof(source_text) - used, "if value { 1 } else ");
    }
    snprintf(source_text + used, sizeof(source_text) - used, "1 }\n");

    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "expression_depth.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(sol_diagnostics_has_errors(&diagnostics));
    bool found = false;
    for (size_t index = 0; index < diagnostics.count; ++index) {
        found = found || strcmp(diagnostics.items[index].code, "SOL-PARSE-013") == 0;
    }
    CHECK(found);
    check_ast_links(&tree);

    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_malformed_expression_delimiters(void) {
    static const char source_text[] =
        "module malformed\n"
        "function broken() -> Int64 { return call(1, }\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "malformed.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(sol_diagnostics_has_errors(&diagnostics));
    CHECK(tree.item_count == 1);
    check_ast_links(&tree);

    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_capability_member_body_rejected(void) {
    static const char source_text[] =
        "module capability_body\n"
        "capability Clock { function now() -> Int64 { return 1 } }\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "capability_body.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(sol_diagnostics_has_errors(&diagnostics));
    CHECK(tree.expression_count == 0);
    bool found = false;
    for (size_t index = 0; index < diagnostics.count; ++index) {
        found = found || strcmp(diagnostics.items[index].code, "SOL-PARSE-014") == 0;
    }
    CHECK(found);

    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_derived_capability_syntax(void) {
    static const char source_text[] =
        "module derived_capability\n"
        "capability FileSystem {\n"
        "    function read(path: Text) -> Text effects { filesystem.read<Self> }\n"
        "}\n"
        "capability ReadFileSystem derives_from source: capability FileSystem {\n"
        "    function read(path: Text) -> Text effects { filesystem.read<Self> } {\n"
        "        return source.read(path)\n"
        "    }\n"
        "}\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "derived_capability.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(!sol_diagnostics_has_errors(&diagnostics));
    CHECK(tree.item_count == 2);
    CHECK(tree.items[0].capability_source == SOL_AST_NONE);
    CHECK(tree.items[1].capability_source < tree.parameter_count);
    CHECK(tree.capability_member_count == 2);
    CHECK(tree.capability_members[0].body == SOL_AST_NONE);
    CHECK(tree.capability_members[1].body < tree.expression_count);
    check_ast_links(&tree);
    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_derived_capability_requires_member_bodies(void) {
    static const char source_text[] =
        "module malformed_derived_capability\n"
        "capability FileSystem {}\n"
        "capability ReadFileSystem derives_from source: capability FileSystem {\n"
        "    function read(path: Text) -> Text effects { pure }\n"
        "}\n"
        "function recovered() -> Int64 { return 1 }\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "malformed_derived.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(sol_diagnostics_has_errors(&diagnostics));
    bool found = false;
    for (size_t index = 0; index < diagnostics.count; ++index) {
        found = found || strcmp(diagnostics.items[index].code, "SOL-PARSE-001") == 0;
    }
    CHECK(found);
    check_ast_links(&tree);
    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_failed_function_parameter_rollback(void) {
    static const char source_text[] =
        "module parameter_recovery\n"
        "function broken(value: Int64) ->\n"
        "function recovered(actual: Int64) -> Int64 { return actual }\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "parameter_recovery.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(sol_diagnostics_has_errors(&diagnostics));
    CHECK(tree.item_count == 1);
    CHECK(tree.parameter_count == 1);
    if (tree.item_count == 1) {
        CHECK(tree.items[0].first_parameter == 0);
    }
    check_ast_links(&tree);

    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_multiline_record_return_rejected(void) {
    static const char source_text[] =
        "module multiline_record\n"
        "record Pair {}\n"
        "function make() -> Pair {\n"
        "    return Pair\n"
        "    {}\n"
        "}\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "multiline_record.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    bool found = false;
    for (size_t index = 0; index < diagnostics.count; ++index) {
        found = found || strcmp(diagnostics.items[index].code, "SOL-PARSE-015") == 0;
    }
    CHECK(found);

    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_function_types(void) {
    static const char source_text[] =
        "module function_types\n"
        "record Holder {\n"
        "    callback: function(Int64, Bool,) -> Text effects {\n"
        "        clock.read\n"
        "        network.call<Primary>\n"
        "    },\n"
        "}\n"
        "function keep(\n"
        "    callback: function(\n"
        "        function() -> Int64 effects { pure },\n"
        "    ) -> Text effects { memory.allocate },\n"
        ") -> function(\n"
        "    function() -> Int64 effects { pure },\n"
        ") -> Text effects { memory.allocate } { return callback }\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "function_types.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    if (sol_diagnostics_has_errors(&diagnostics)) {
        sol_diagnostics_render_human(stderr, &source, &diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&diagnostics));
    size_t function_type_count = 0;
    size_t type_effect_count = 0;
    for (size_t index = 0; index < tree.type_count; ++index) {
        if (tree.types[index].kind == SOL_SYNTAX_TYPE_FUNCTION) ++function_type_count;
    }
    for (size_t index = 0; index < tree.effect_count; ++index) {
        if (tree.effects[index].owner_kind == SOL_EFFECT_OWNER_TYPE) {
            ++type_effect_count;
            CHECK(tree.types[tree.effects[index].owner].kind == SOL_SYNTAX_TYPE_FUNCTION);
        }
    }
    CHECK(function_type_count == 5);
    CHECK(type_effect_count == 6);
    check_ast_links(&tree);
    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_function_type_requires_effects(void) {
    static const char source_text[] =
        "module function_type_recovery\n"
        "function broken(callback: function(Int64) -> Bool) -> Bool { return true }\n"
        "function recovered(actual: Int64) -> Int64 { return actual }\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "function_type_recovery.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(sol_diagnostics_has_errors(&diagnostics));
    CHECK(tree.item_count == 1);
    CHECK(tree.parameter_count == 1);
    check_ast_links(&tree);
    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_function_type_recovery_skips_nested_function(void) {
    static const char source_text[] =
        "module nested_function_type_recovery\n"
        "function broken(\n"
        "    value Int64,\n"
        "    callback: function(Int64) -> Bool effects { pure },\n"
        ") -> Bool { return true }\n"
        "function recovered(actual: Int64) -> Int64 { return actual }\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "nested_recovery.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(sol_diagnostics_has_errors(&diagnostics));
    CHECK(tree.item_count == 1);
    CHECK(tree.parameter_count == 1);
    CHECK(tree.type_count == 2);
    check_ast_links(&tree);
    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_handle_expression_syntax(void) {
    static const char source_text[] =
        "module handle_syntax\n"
        "capability Clock { function read() -> Int64 effects { clock.read<Self> } }\n"
        "capability TestClock { function read() -> Int64 effects { pure } }\n"
        "function sample(clock: capability Clock, provider: capability TestClock) -> Int64 {\n"
        "    return handle clock.read<clock> with provider { clock.read() }\n"
        "}\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "handle.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(!sol_diagnostics_has_errors(&diagnostics));
    check_ast_links(&tree);
    size_t handlers = 0;
    for (size_t index = 0; index < tree.expression_count; ++index) {
        if (tree.expressions[index].kind != SOL_EXPR_HANDLE) continue;
        const SolExpr *handler = &tree.expressions[index];
        CHECK(sol_token_text_equal(
            &source,
            (SolToken){.span = handler->as.handle.effect_name},
            "clock.read"
        ));
        CHECK(tree.expressions[handler->as.handle.body].kind == SOL_EXPR_BLOCK);
        ++handlers;
    }
    CHECK(handlers == 1);
    bool saw_handle = false;
    bool saw_with = false;
    for (size_t index = 0; index < tokens.count; ++index) {
        saw_handle = saw_handle || tokens.items[index].kind == SOL_TOKEN_HANDLE;
        saw_with = saw_with || tokens.items[index].kind == SOL_TOKEN_WITH;
    }
    CHECK(saw_handle);
    CHECK(saw_with);
    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_malformed_handle_expressions(void) {
    static const char *cases[] = {
        "module malformed_handle\nfunction bad(a: Int64) -> Int64 { return handle clock.read with a { 1 } }\n",
        "module malformed_handle\nfunction bad(a: Int64) -> Int64 { return handle clock.read<a> a { 1 } }\n",
        "module malformed_handle\nfunction bad(a: Int64) -> Int64 { return handle clock.read<a> with a 1 }\n",
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        SolSource source;
        SolTokens tokens;
        SolDiagnostics diagnostics;
        SolSyntaxTree tree;
        CHECK(sol_source_from_text(&source, "malformed_handle.sol", cases[index]));
        sol_tokens_init(&tokens);
        sol_diagnostics_init(&diagnostics);
        sol_syntax_tree_init(&tree);
        CHECK(sol_lex(&source, &tokens, &diagnostics));
        CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
        CHECK(sol_diagnostics_has_errors(&diagnostics));
        check_ast_links(&tree);
        sol_syntax_tree_free(&tree);
        sol_diagnostics_free(&diagnostics);
        sol_tokens_free(&tokens);
        sol_source_free(&source);
    }
}

int main(void) {
    test_valid_declarations();
    test_generic_syntax_and_comparison_disambiguation();
    test_unsupported_generic_syntax();
    test_generic_lookahead_recovery_and_spans();
    test_missing_module();
    test_nested_and_unterminated_comments();
    test_clause_order();
    test_structured_contract_syntax();
    test_contract_diagnostics_and_recovery();
    test_contract_rollback();
    test_capability_contract_rollback();
    test_carriage_return_positions();
    test_type_depth_limit();
    test_declaration_recovery();
    test_expression_ast();
    test_expression_recovery();
    test_nested_record_condition();
    test_expression_depth_limit();
    test_malformed_expression_delimiters();
    test_capability_member_body_rejected();
    test_derived_capability_syntax();
    test_derived_capability_requires_member_bodies();
    test_failed_function_parameter_rollback();
    test_multiline_record_return_rejected();
    test_function_types();
    test_function_type_requires_effects();
    test_function_type_recovery_skips_nested_function();
    test_handle_expression_syntax();
    test_malformed_handle_expressions();
    if (failures != 0) {
        fprintf(stderr, "%d frontend test failure(s)\n", failures);
        return 1;
    }
    puts("frontend tests passed");
    return 0;
}
