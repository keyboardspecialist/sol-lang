#include "sol/typecheck.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const SolSource *source;
    const SolSyntaxTree *syntax;
    const SolHirModule *hir;
    SolTypeTable *types;
    SolDiagnostics *diagnostics;
    unsigned char *states;
    size_t depth;
    SolDefId current_definition;
    SolType expected_return;
    bool allocation_failed;
    bool depth_reported;
    bool malformed;
} SolTypeChecker;

static void sol_type_error(
    SolTypeChecker *checker,
    const char *code,
    SolSpan span,
    const char *message
);

static void sol_type_malformed(SolTypeChecker *checker) {
    if (!checker->malformed) {
        sol_type_error(
            checker,
            "SOL-INTERNAL-003",
            (SolSpan){0},
            "malformed syntax or HIR passed to type checking"
        );
    }
    checker->malformed = true;
}

static bool sol_type_span_valid(const SolSource *source, SolSpan span) {
    return source->text != NULL && span.start <= span.end && span.end <= source->length;
}

static bool sol_type_validate(SolTypeChecker *checker) {
    const SolSyntaxTree *syntax = checker->syntax;
    const SolHirModule *hir = checker->hir;
    if (syntax == NULL || hir == NULL
        || syntax->item_count > syntax->item_capacity
        || syntax->parameter_count > syntax->parameter_capacity
        || syntax->argument_count > syntax->argument_capacity
        || syntax->statement_count > syntax->statement_capacity
        || syntax->expression_count > syntax->expression_capacity
        || (syntax->item_count != 0 && syntax->items == NULL)
        || (syntax->parameter_count != 0 && syntax->parameters == NULL)
        || (syntax->argument_count != 0 && syntax->arguments == NULL)
        || (syntax->statement_count != 0 && syntax->statements == NULL)
        || (syntax->expression_count != 0 && syntax->expressions == NULL)
        || hir->definition_count != syntax->item_count
        || hir->resolution_count != syntax->expression_count
        || hir->local_count > hir->local_capacity
        || syntax->parameter_count > SIZE_MAX - syntax->argument_count
        || (hir->definition_count != 0 && hir->definitions == NULL)
        || (hir->resolution_count != 0 && hir->resolutions == NULL)
        || (hir->local_count != 0 && hir->locals == NULL)) {
        sol_type_malformed(checker);
        return false;
    }
    for (size_t index = 0; index < syntax->item_count; ++index) {
        const SolSyntaxItem *item = &syntax->items[index];
        const SolHirDefinition *definition = &hir->definitions[index];
        if (!sol_type_span_valid(checker->source, item->span)
            || !sol_type_span_valid(checker->source, item->name)
            || !sol_type_span_valid(checker->source, item->return_type)
            || (item->body != SOL_AST_NONE && item->body >= syntax->expression_count)
            || (item->first_parameter != SOL_AST_NONE
                && item->first_parameter >= syntax->parameter_count)
            || definition->syntax_item != index
            || definition->kind != item->kind
            || !sol_type_span_valid(checker->source, definition->name)) {
            sol_type_malformed(checker);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->parameter_count; ++index) {
        const SolParameter *parameter = &syntax->parameters[index];
        if (!sol_type_span_valid(checker->source, parameter->name)
            || !sol_type_span_valid(checker->source, parameter->type)
            || (parameter->next != SOL_AST_NONE && parameter->next >= syntax->parameter_count)) {
            sol_type_malformed(checker);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->argument_count; ++index) {
        const SolArgument *argument = &syntax->arguments[index];
        if (argument->value >= syntax->expression_count
            || (argument->next != SOL_AST_NONE && argument->next >= syntax->argument_count)
            || (argument->is_named && !sol_type_span_valid(checker->source, argument->name))) {
            sol_type_malformed(checker);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->statement_count; ++index) {
        const SolStatement *statement = &syntax->statements[index];
        SolExprId value = statement->kind == SOL_STATEMENT_LET
            ? statement->as.let_statement.value
            : statement->as.expression;
        if (value >= syntax->expression_count
            || (statement->next != SOL_AST_NONE && statement->next >= syntax->statement_count)) {
            sol_type_malformed(checker);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->expression_count; ++index) {
        const SolExpr *expression = &syntax->expressions[index];
        bool valid = (int)expression->kind >= 0
            && expression->kind <= SOL_EXPR_PROPAGATE
            && sol_type_span_valid(checker->source, expression->span);
        switch (expression->kind) {
            case SOL_EXPR_UNARY:
                valid = valid && expression->as.unary.operand < syntax->expression_count;
                break;
            case SOL_EXPR_BINARY:
                valid = valid
                    && expression->as.binary.left < syntax->expression_count
                    && expression->as.binary.right < syntax->expression_count;
                break;
            case SOL_EXPR_CALL:
                valid = valid
                    && expression->as.call.callee < syntax->expression_count
                    && (expression->as.call.first_argument == SOL_AST_NONE
                        || expression->as.call.first_argument < syntax->argument_count);
                break;
            case SOL_EXPR_FIELD:
                valid = valid && expression->as.field.base < syntax->expression_count;
                break;
            case SOL_EXPR_RECORD:
                valid = valid
                    && expression->as.record.type < syntax->expression_count
                    && (expression->as.record.first_field == SOL_AST_NONE
                        || expression->as.record.first_field < syntax->argument_count);
                break;
            case SOL_EXPR_IF:
                valid = valid
                    && expression->as.if_expr.condition < syntax->expression_count
                    && expression->as.if_expr.then_branch < syntax->expression_count
                    && expression->as.if_expr.else_branch < syntax->expression_count;
                break;
            case SOL_EXPR_BLOCK:
                valid = valid
                    && (expression->as.block.first_statement == SOL_AST_NONE
                        || expression->as.block.first_statement < syntax->statement_count);
                break;
            case SOL_EXPR_PROPAGATE:
                valid = valid && expression->as.propagated < syntax->expression_count;
                break;
            default:
                break;
        }
        SolResolution resolution = hir->resolutions[index];
        valid = valid
            && (int)resolution.kind >= 0
            && resolution.kind <= SOL_RESOLUTION_BUILTIN;
        if (resolution.kind == SOL_RESOLUTION_DEFINITION) {
            valid = valid && resolution.target < hir->definition_count;
        } else if (resolution.kind == SOL_RESOLUTION_LOCAL) {
            valid = valid && resolution.target < hir->local_count;
        } else if (resolution.kind == SOL_RESOLUTION_BUILTIN) {
            valid = valid && resolution.target <= SOL_BUILTIN_NONE;
        }
        if (!valid) {
            sol_type_malformed(checker);
            return false;
        }
    }
    for (size_t index = 0; index < hir->local_count; ++index) {
        const SolHirLocal *local = &hir->locals[index];
        if ((int)local->kind < 0 || local->kind > SOL_LOCAL_BINDING
            || local->owner >= hir->definition_count
            || (local->kind == SOL_LOCAL_PARAMETER
                && local->syntax_id >= syntax->parameter_count)
            || (local->kind == SOL_LOCAL_BINDING
                && local->syntax_id >= syntax->statement_count)) {
            sol_type_malformed(checker);
            return false;
        }
    }
    return true;
}

void sol_type_table_init(SolTypeTable *table) {
    memset(table, 0, sizeof(*table));
}

void sol_type_table_free(SolTypeTable *table) {
    free(table->expressions);
    free(table->locals);
    free(table->definitions);
    memset(table, 0, sizeof(*table));
}

static bool sol_type_span_equal(const SolSource *source, SolSpan span, const char *text) {
    size_t length = span.end - span.start;
    return strlen(text) == length
        && memcmp(source->text + span.start, text, length) == 0;
}

static bool sol_type_name_equal(const SolSource *source, SolSpan left, SolSpan right) {
    size_t left_length = left.end - left.start;
    size_t right_length = right.end - right.start;
    return left_length == right_length
        && memcmp(source->text + left.start, source->text + right.start, left_length) == 0;
}

static int sol_type_next_byte(const SolSource *source, SolSpan span, size_t *cursor) {
    while (*cursor < span.end) {
        unsigned char byte = (unsigned char)source->text[*cursor];
        if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n') {
            ++*cursor;
            continue;
        }
        ++*cursor;
        return (int)byte;
    }
    return -1;
}

static bool sol_type_normalized_prefix(
    const SolSource *source,
    SolSpan span,
    const char *prefix
) {
    size_t cursor = span.start;
    for (size_t index = 0; prefix[index] != '\0'; ++index) {
        if (sol_type_next_byte(source, span, &cursor) != (unsigned char)prefix[index]) {
            return false;
        }
    }
    return true;
}

static size_t sol_type_normalized_hash(const SolSource *source, SolSpan span) {
    size_t hash = (size_t)1469598103934665603ULL;
    size_t cursor = span.start;
    for (;;) {
        int byte = sol_type_next_byte(source, span, &cursor);
        if (byte < 0) {
            return hash;
        }
        hash ^= (size_t)(unsigned int)byte;
        hash *= (size_t)1099511628211ULL;
    }
}

static bool sol_type_equal(SolType left, SolType right) {
    if (left.kind == SOL_TYPE_UNKNOWN || right.kind == SOL_TYPE_UNKNOWN
        || left.kind == SOL_TYPE_ERROR || right.kind == SOL_TYPE_ERROR) {
        return true;
    }
    return left.kind == right.kind
        && ((left.kind != SOL_TYPE_NOMINAL
                && left.kind != SOL_TYPE_OPAQUE
                && left.kind != SOL_TYPE_FUNCTION)
            || left.definition == right.definition);
}

static const char *sol_type_name(SolType type) {
    switch (type.kind) {
        case SOL_TYPE_INT64: return "Int64";
        case SOL_TYPE_BOOL: return "Bool";
        case SOL_TYPE_TEXT: return "Text";
        case SOL_TYPE_UNIT: return "Unit";
        case SOL_TYPE_NOMINAL: return "nominal type";
        case SOL_TYPE_OPAQUE: return "generic type";
        case SOL_TYPE_FUNCTION: return "function";
        case SOL_TYPE_NEVER: return "Never";
        case SOL_TYPE_ERROR: return "error";
        default: return "unknown type";
    }
}

static SolType sol_type_from_span(SolTypeChecker *checker, SolSpan span) {
    if (sol_type_span_equal(checker->source, span, "Int64")) {
        return (SolType){.kind = SOL_TYPE_INT64};
    }
    if (sol_type_span_equal(checker->source, span, "Bool")) {
        return (SolType){.kind = SOL_TYPE_BOOL};
    }
    if (sol_type_span_equal(checker->source, span, "Text")) {
        return (SolType){.kind = SOL_TYPE_TEXT};
    }
    if (sol_type_span_equal(checker->source, span, "()")) {
        return (SolType){.kind = SOL_TYPE_UNIT};
    }
    for (size_t index = 0; index < checker->hir->definition_count; ++index) {
        const SolHirDefinition *definition = &checker->hir->definitions[index];
        if (definition->kind != SOL_ITEM_FUNCTION
            && sol_type_name_equal(checker->source, definition->name, span)) {
            return (SolType){.kind = SOL_TYPE_NOMINAL, .definition = index};
        }
    }
    if (sol_type_normalized_prefix(checker->source, span, "capability")) {
        for (size_t index = 0; index < checker->hir->definition_count; ++index) {
            const SolHirDefinition *definition = &checker->hir->definitions[index];
            size_t name_length = definition->name.end - definition->name.start;
            if (definition->kind == SOL_ITEM_CAPABILITY
                && span.end >= name_length
                && sol_type_name_equal(
                    checker->source,
                    definition->name,
                    (SolSpan){.start = span.end - name_length, .end = span.end}
                )) {
                return (SolType){.kind = SOL_TYPE_NOMINAL, .definition = index};
            }
        }
    }
    if (sol_type_normalized_prefix(checker->source, span, "Result<")
        || sol_type_normalized_prefix(checker->source, span, "Option<")) {
        return (SolType){
            .kind = SOL_TYPE_OPAQUE,
            .definition = sol_type_normalized_hash(checker->source, span),
        };
    }
    sol_type_error(
        checker,
        "SOL-TYPE-009",
        span,
        "unresolved declared type"
    );
    return (SolType){.kind = SOL_TYPE_ERROR};
}

static bool sol_type_allocate(SolTypeChecker *checker) {
    size_t expression_count = checker->syntax->expression_count;
    size_t local_count = checker->hir->local_count;
    size_t definition_count = checker->hir->definition_count;
    if (expression_count > SIZE_MAX / sizeof(*checker->types->expressions)
        || local_count > SIZE_MAX / sizeof(*checker->types->locals)
        || definition_count > SIZE_MAX / sizeof(*checker->types->definitions)) {
        return false;
    }
    checker->types->expressions = calloc(expression_count, sizeof(*checker->types->expressions));
    checker->types->locals = calloc(local_count, sizeof(*checker->types->locals));
    checker->types->definitions = calloc(
        definition_count,
        sizeof(*checker->types->definitions)
    );
    checker->states = calloc(expression_count, sizeof(*checker->states));
    if ((expression_count != 0
            && (checker->types->expressions == NULL || checker->states == NULL))
        || (local_count != 0 && checker->types->locals == NULL)
        || (definition_count != 0 && checker->types->definitions == NULL)) {
        return false;
    }
    checker->types->expression_count = expression_count;
    checker->types->local_count = local_count;
    checker->types->definition_count = definition_count;
    return true;
}

static void sol_type_error(
    SolTypeChecker *checker,
    const char *code,
    SolSpan span,
    const char *message
) {
    sol_diagnostics_add(
        checker->diagnostics,
        code,
        SOL_SEVERITY_ERROR,
        span,
        "%s",
        message
    );
}

static SolType sol_type_expression(SolTypeChecker *checker, SolExprId expression_id);

static SolLocalId sol_type_find_local(
    SolTypeChecker *checker,
    SolLocalKind kind,
    size_t syntax_id
) {
    for (size_t index = 0; index < checker->hir->local_count; ++index) {
        const SolHirLocal *local = &checker->hir->locals[index];
        if (local->owner == checker->current_definition
            && local->kind == kind
            && local->syntax_id == syntax_id) {
            return index;
        }
    }
    return SOL_AST_NONE;
}

static SolType sol_type_arguments(SolTypeChecker *checker, SolArgumentId argument_id) {
    SolType last = {.kind = SOL_TYPE_UNIT};
    size_t traversed = 0;
    while (argument_id != SOL_AST_NONE && traversed++ < checker->syntax->argument_count) {
        const SolArgument *argument = &checker->syntax->arguments[argument_id];
        last = sol_type_expression(checker, argument->value);
        argument_id = argument->next;
    }
    if (argument_id != SOL_AST_NONE) {
        sol_type_malformed(checker);
    }
    return last;
}

static SolType sol_type_block(SolTypeChecker *checker, const SolExpr *block) {
    SolType result = {.kind = SOL_TYPE_UNIT};
    bool terminated = false;
    SolStatementId statement_id = block->as.block.first_statement;
    size_t traversed = 0;
    while (statement_id != SOL_AST_NONE && traversed++ < checker->syntax->statement_count) {
        const SolStatement *statement = &checker->syntax->statements[statement_id];
        if (statement->kind == SOL_STATEMENT_LET) {
            SolType value = sol_type_expression(checker, statement->as.let_statement.value);
            SolLocalId local = sol_type_find_local(
                checker,
                SOL_LOCAL_BINDING,
                statement_id
            );
            if (local != SOL_AST_NONE) {
                checker->types->locals[local] = value;
            }
            if (!terminated) {
                result = value.kind == SOL_TYPE_NEVER
                    ? value
                    : (SolType){.kind = SOL_TYPE_UNIT};
                terminated = value.kind == SOL_TYPE_NEVER;
            }
        } else {
            SolType value = sol_type_expression(checker, statement->as.expression);
            if (statement->kind == SOL_STATEMENT_RETURN) {
                if (!sol_type_equal(value, checker->expected_return)) {
                    char message[160];
                    snprintf(
                        message,
                        sizeof(message),
                        "return type mismatch: expected %s, found %s",
                        sol_type_name(checker->expected_return),
                        sol_type_name(value)
                    );
                    sol_type_error(checker, "SOL-TYPE-004", statement->span, message);
                }
                if (!terminated) {
                    result = (SolType){.kind = SOL_TYPE_NEVER};
                    terminated = true;
                }
            } else if (!terminated) {
                result = value;
                terminated = value.kind == SOL_TYPE_NEVER;
            }
        }
        statement_id = statement->next;
    }
    if (statement_id != SOL_AST_NONE) {
        sol_type_malformed(checker);
    }
    return result;
}

static SolResolution sol_type_callee_resolution(
    SolTypeChecker *checker,
    SolExprId callee_id
) {
    const SolExpr *callee = &checker->syntax->expressions[callee_id];
    if (callee->kind == SOL_EXPR_PATH || callee->kind == SOL_EXPR_FIELD) {
        return checker->hir->resolutions[callee_id];
    }
    return (SolResolution){.kind = SOL_RESOLUTION_NOT_APPLICABLE};
}

static SolType sol_type_call(SolTypeChecker *checker, const SolExpr *call) {
    SolType callee_type = sol_type_expression(checker, call->as.call.callee);
    SolResolution resolution = sol_type_callee_resolution(checker, call->as.call.callee);
    SolDefId target = SOL_AST_NONE;
    if (resolution.kind == SOL_RESOLUTION_DEFINITION
        && resolution.target < checker->syntax->item_count
        && checker->syntax->items[resolution.target].kind == SOL_ITEM_FUNCTION) {
        target = resolution.target;
    } else if (callee_type.kind == SOL_TYPE_FUNCTION
        && callee_type.definition < checker->syntax->item_count) {
        target = callee_type.definition;
    }
    if (target == SOL_AST_NONE) {
        sol_type_arguments(checker, call->as.call.first_argument);
        if (resolution.kind != SOL_RESOLUTION_BUILTIN
            && callee_type.kind != SOL_TYPE_UNKNOWN
            && callee_type.kind != SOL_TYPE_ERROR) {
            sol_type_error(
                checker,
                "SOL-TYPE-010",
                checker->syntax->expressions[call->as.call.callee].span,
                "expression is not callable"
            );
        }
        return (SolType){.kind = SOL_TYPE_UNKNOWN};
    }

    const SolSyntaxItem *function = &checker->syntax->items[target];
    size_t parameter_count = 0;
    SolParameterId parameter_id = function->first_parameter;
    while (parameter_id != SOL_AST_NONE) {
        if (parameter_count++ >= checker->syntax->parameter_count) {
            sol_type_malformed(checker);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        parameter_id = checker->syntax->parameters[parameter_id].next;
    }
    SolParameterId *parameter_ids = NULL;
    bool *used = NULL;
    if (parameter_count != 0) {
        if (parameter_count > SIZE_MAX / sizeof(*parameter_ids)) {
            checker->allocation_failed = true;
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        parameter_ids = malloc(parameter_count * sizeof(*parameter_ids));
        used = calloc(parameter_count, sizeof(*used));
        if (parameter_ids == NULL || used == NULL) {
            free(parameter_ids);
            free(used);
            checker->allocation_failed = true;
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
    }
    parameter_id = function->first_parameter;
    for (size_t index = 0; index < parameter_count; ++index) {
        parameter_ids[index] = parameter_id;
        parameter_id = checker->syntax->parameters[parameter_id].next;
    }

    SolArgumentId argument_id = call->as.call.first_argument;
    size_t argument_count = 0;
    size_t positional_index = 0;
    bool seen_named = false;
    while (argument_id != SOL_AST_NONE) {
        if (argument_count++ >= checker->syntax->argument_count) {
            sol_type_malformed(checker);
            free(parameter_ids);
            free(used);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        const SolArgument *argument = &checker->syntax->arguments[argument_id];
        SolType actual = sol_type_expression(checker, argument->value);
        size_t matched = SIZE_MAX;
        if (argument->is_named) {
            seen_named = true;
            for (size_t index = 0; index < parameter_count; ++index) {
                const SolParameter *parameter = &checker->syntax->parameters[parameter_ids[index]];
                if (sol_type_name_equal(checker->source, parameter->name, argument->name)) {
                    matched = index;
                    break;
                }
            }
            if (matched == SIZE_MAX) {
                sol_type_error(
                    checker,
                    "SOL-TYPE-012",
                    argument->name,
                    "named argument does not match a parameter"
                );
            }
        } else if (seen_named) {
            sol_type_error(
                checker,
                "SOL-TYPE-012",
                checker->syntax->expressions[argument->value].span,
                "positional arguments cannot follow named arguments"
            );
        } else if (positional_index < parameter_count) {
            matched = positional_index++;
        }

        if (matched != SIZE_MAX) {
            if (used[matched]) {
                sol_type_error(
                    checker,
                    "SOL-TYPE-012",
                    argument->is_named
                        ? argument->name
                        : checker->syntax->expressions[argument->value].span,
                    "parameter is supplied more than once"
                );
            }
            used[matched] = true;
            const SolParameter *parameter = &checker->syntax->parameters[parameter_ids[matched]];
            SolType expected = sol_type_from_span(checker, parameter->type);
            if (!sol_type_equal(expected, actual)) {
                sol_type_error(
                    checker,
                    "SOL-TYPE-005",
                    checker->syntax->expressions[argument->value].span,
                    "function argument type does not match parameter type"
                );
            }
        }
        argument_id = argument->next;
    }

    bool missing = false;
    for (size_t index = 0; index < parameter_count; ++index) {
        missing = missing || !used[index];
    }
    if (missing || argument_count != parameter_count) {
        sol_type_error(
            checker,
            "SOL-TYPE-006",
            call->span,
            "function call has the wrong number of arguments"
        );
    }
    free(parameter_ids);
    free(used);
    return checker->types->definitions[target];
}

static SolType sol_type_expression(SolTypeChecker *checker, SolExprId expression_id) {
    if (expression_id >= checker->syntax->expression_count) {
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    if (checker->states[expression_id] == 2) {
        return checker->types->expressions[expression_id];
    }
    if (checker->states[expression_id] == 1) {
        sol_type_malformed(checker);
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    if (checker->depth >= 256) {
        if (!checker->depth_reported) {
            sol_type_error(
                checker,
                "SOL-TYPE-007",
                checker->syntax->expressions[expression_id].span,
                "expression structure exceeds the type-checking limit of 256"
            );
            checker->depth_reported = true;
        }
        return (SolType){.kind = SOL_TYPE_ERROR};
    }

    checker->states[expression_id] = 1;
    ++checker->depth;
    const SolExpr *expression = &checker->syntax->expressions[expression_id];
    SolType type = {.kind = SOL_TYPE_UNKNOWN};
    switch (expression->kind) {
        case SOL_EXPR_ERROR:
            type = (SolType){.kind = SOL_TYPE_ERROR};
            break;
        case SOL_EXPR_INTEGER:
            type = (SolType){.kind = SOL_TYPE_INT64};
            break;
        case SOL_EXPR_STRING:
            type = (SolType){.kind = SOL_TYPE_TEXT};
            break;
        case SOL_EXPR_BOOL:
            type = (SolType){.kind = SOL_TYPE_BOOL};
            break;
        case SOL_EXPR_UNIT:
            type = (SolType){.kind = SOL_TYPE_UNIT};
            break;
        case SOL_EXPR_PATH: {
            SolResolution resolution = checker->hir->resolutions[expression_id];
            if (resolution.kind == SOL_RESOLUTION_LOCAL
                && resolution.target < checker->types->local_count) {
                type = checker->types->locals[resolution.target];
            } else if (resolution.kind == SOL_RESOLUTION_DEFINITION
                && resolution.target < checker->types->definition_count) {
                type = checker->syntax->items[resolution.target].kind == SOL_ITEM_FUNCTION
                    ? (SolType){
                        .kind = SOL_TYPE_FUNCTION,
                        .definition = resolution.target,
                    }
                    : checker->types->definitions[resolution.target];
            }
            break;
        }
        case SOL_EXPR_UNARY: {
            SolType operand = sol_type_expression(checker, expression->as.unary.operand);
            SolType expected = expression->as.unary.operator_kind == SOL_TOKEN_BANG
                ? (SolType){.kind = SOL_TYPE_BOOL}
                : (SolType){.kind = SOL_TYPE_INT64};
            if (!sol_type_equal(operand, expected)) {
                sol_type_error(
                    checker,
                    "SOL-TYPE-002",
                    expression->span,
                    "invalid operand type for unary operator"
                );
            }
            type = expected;
            break;
        }
        case SOL_EXPR_BINARY: {
            SolType left = sol_type_expression(checker, expression->as.binary.left);
            SolType right = sol_type_expression(checker, expression->as.binary.right);
            SolTokenKind operator_kind = expression->as.binary.operator_kind;
            bool logical = operator_kind == SOL_TOKEN_AMP_AMP
                || operator_kind == SOL_TOKEN_PIPE_PIPE;
            bool equality = operator_kind == SOL_TOKEN_EQUAL_EQUAL
                || operator_kind == SOL_TOKEN_BANG_EQUAL;
            bool comparison = operator_kind == SOL_TOKEN_LESS
                || operator_kind == SOL_TOKEN_LESS_EQUAL
                || operator_kind == SOL_TOKEN_GREATER
                || operator_kind == SOL_TOKEN_GREATER_EQUAL;
            SolType expected = logical
                ? (SolType){.kind = SOL_TYPE_BOOL}
                : (SolType){.kind = SOL_TYPE_INT64};
            if ((equality && !sol_type_equal(left, right))
                || (!equality
                    && (!sol_type_equal(left, expected) || !sol_type_equal(right, expected)))) {
                sol_type_error(
                    checker,
                    "SOL-TYPE-002",
                    expression->span,
                    "invalid operand types for binary operator"
                );
            }
            type = logical || equality || comparison
                ? (SolType){.kind = SOL_TYPE_BOOL}
                : (SolType){.kind = SOL_TYPE_INT64};
            break;
        }
        case SOL_EXPR_CALL:
            type = sol_type_call(checker, expression);
            break;
        case SOL_EXPR_FIELD:
            if (checker->hir->resolutions[expression_id].kind == SOL_RESOLUTION_DEFINITION) {
                SolDefId definition = checker->hir->resolutions[expression_id].target;
                type = checker->syntax->items[definition].kind == SOL_ITEM_FUNCTION
                    ? (SolType){.kind = SOL_TYPE_FUNCTION, .definition = definition}
                    : checker->types->definitions[definition];
            } else {
                sol_type_expression(checker, expression->as.field.base);
            }
            break;
        case SOL_EXPR_RECORD: {
            SolExprId type_expression = expression->as.record.type;
            SolResolution resolution = checker->hir->resolutions[type_expression];
            sol_type_expression(checker, type_expression);
            sol_type_arguments(checker, expression->as.record.first_field);
            if (resolution.kind == SOL_RESOLUTION_DEFINITION
                && resolution.target < checker->types->definition_count
                && checker->syntax->items[resolution.target].kind == SOL_ITEM_RECORD) {
                type = (SolType){.kind = SOL_TYPE_NOMINAL, .definition = resolution.target};
            } else {
                sol_type_error(
                    checker,
                    "SOL-TYPE-011",
                    expression->span,
                    "record construction requires a record type"
                );
                type = (SolType){.kind = SOL_TYPE_ERROR};
            }
            break;
        }
        case SOL_EXPR_IF: {
            SolType condition = sol_type_expression(checker, expression->as.if_expr.condition);
            if (!sol_type_equal(condition, (SolType){.kind = SOL_TYPE_BOOL})) {
                sol_type_error(
                    checker,
                    "SOL-TYPE-003",
                    checker->syntax->expressions[expression->as.if_expr.condition].span,
                    "if condition must have type Bool"
                );
            }
            SolType then_type = sol_type_expression(checker, expression->as.if_expr.then_branch);
            SolType else_type = sol_type_expression(checker, expression->as.if_expr.else_branch);
            if (then_type.kind == SOL_TYPE_NEVER) {
                type = else_type;
            } else if (else_type.kind == SOL_TYPE_NEVER) {
                type = then_type;
            } else if (!sol_type_equal(then_type, else_type)) {
                sol_type_error(
                    checker,
                    "SOL-TYPE-008",
                    expression->span,
                    "if branches must produce the same type"
                );
                type = (SolType){.kind = SOL_TYPE_ERROR};
            } else {
                type = then_type.kind == SOL_TYPE_UNKNOWN ? else_type : then_type;
            }
            break;
        }
        case SOL_EXPR_BLOCK:
            type = sol_type_block(checker, expression);
            break;
        case SOL_EXPR_PROPAGATE:
            sol_type_expression(checker, expression->as.propagated);
            type = (SolType){.kind = SOL_TYPE_UNKNOWN};
            break;
    }
    --checker->depth;
    checker->states[expression_id] = 2;
    checker->types->expressions[expression_id] = type;
    return type;
}

bool sol_type_check(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolHirModule *hir,
    SolTypeTable *types,
    SolDiagnostics *diagnostics
) {
    if (diagnostics == NULL) {
        return false;
    }
    if (source == NULL || source->text == NULL || syntax == NULL || hir == NULL || types == NULL) {
        sol_diagnostics_add(
            diagnostics,
            "SOL-INTERNAL-003",
            SOL_SEVERITY_ERROR,
            (SolSpan){0},
            "null compiler input passed to type checking"
        );
        return false;
    }
    SolTypeChecker checker = {
        .source = source,
        .syntax = syntax,
        .hir = hir,
        .types = types,
        .diagnostics = diagnostics,
    };
    if (types->expressions != NULL || types->locals != NULL || types->definitions != NULL
        || types->expression_count != 0 || types->local_count != 0
        || types->definition_count != 0) {
        sol_type_malformed(&checker);
        return false;
    }
    if (!sol_type_validate(&checker) || !sol_type_allocate(&checker)) {
        if (!checker.malformed) {
            sol_type_malformed(&checker);
        }
        free(checker.states);
        return false;
    }

    for (size_t index = 0; index < hir->definition_count; ++index) {
        const SolSyntaxItem *item = &syntax->items[index];
        checker.current_definition = index;
        checker.types->definitions[index] = item->kind == SOL_ITEM_FUNCTION
            ? sol_type_from_span(&checker, item->return_type)
            : (SolType){.kind = SOL_TYPE_NOMINAL, .definition = index};
    }
    for (size_t index = 0; index < hir->local_count; ++index) {
        const SolHirLocal *local = &hir->locals[index];
        if (local->kind == SOL_LOCAL_PARAMETER && local->syntax_id < syntax->parameter_count) {
            checker.types->locals[index] = sol_type_from_span(
                &checker,
                syntax->parameters[local->syntax_id].type
            );
        }
    }
    for (size_t index = 0; index < syntax->item_count; ++index) {
        const SolSyntaxItem *item = &syntax->items[index];
        if (item->kind != SOL_ITEM_FUNCTION || item->body == SOL_AST_NONE) {
            continue;
        }
        checker.current_definition = index;
        checker.expected_return = checker.types->definitions[index];
        memset(checker.states, 0, syntax->expression_count * sizeof(*checker.states));
        SolType body_type = sol_type_expression(&checker, item->body);
        if (body_type.kind != SOL_TYPE_NEVER
            && !sol_type_equal(body_type, checker.expected_return)) {
            char message[160];
            snprintf(
                message,
                sizeof(message),
                "function body type mismatch: expected %s, found %s",
                sol_type_name(checker.expected_return),
                sol_type_name(body_type)
            );
            sol_type_error(&checker, "SOL-TYPE-004", item->span, message);
        }
    }

    free(checker.states);
    return !checker.allocation_failed
        && !checker.malformed
        && !diagnostics->allocation_failed;
}
