#include "sol/effectcheck.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const SolSource *source;
    const SolSyntaxTree *syntax;
    const SolHirModule *hir;
    SolDiagnostics *diagnostics;
    unsigned char *visited;
    SolDefId current_function;
    size_t depth;
    bool depth_reported;
    bool malformed;
} SolEffectChecker;

static int sol_effect_path_next_byte(const SolSource *source, SolSpan span, size_t *cursor) {
    while (*cursor < span.end) {
        unsigned char byte = (unsigned char)source->text[*cursor];
        if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n') {
            ++*cursor;
            continue;
        }
        if (byte == '/' && *cursor + 1 < span.end && source->text[*cursor + 1] == '/') {
            *cursor += 2;
            while (*cursor < span.end
                && source->text[*cursor] != '\r'
                && source->text[*cursor] != '\n') {
                ++*cursor;
            }
            continue;
        }
        if (byte == '/' && *cursor + 1 < span.end && source->text[*cursor + 1] == '*') {
            *cursor += 2;
            size_t depth = 1;
            while (*cursor < span.end && depth != 0) {
                if (*cursor + 1 < span.end
                    && source->text[*cursor] == '/'
                    && source->text[*cursor + 1] == '*') {
                    ++depth;
                    *cursor += 2;
                } else if (*cursor + 1 < span.end
                    && source->text[*cursor] == '*'
                    && source->text[*cursor + 1] == '/') {
                    --depth;
                    *cursor += 2;
                } else {
                    ++*cursor;
                }
            }
            continue;
        }
        ++*cursor;
        return (int)byte;
    }
    return -1;
}

static bool sol_effect_span_equal(const SolSource *source, SolSpan left, SolSpan right) {
    size_t left_cursor = left.start;
    size_t right_cursor = right.start;
    for (;;) {
        int left_byte = sol_effect_path_next_byte(source, left, &left_cursor);
        int right_byte = sol_effect_path_next_byte(source, right, &right_cursor);
        if (left_byte != right_byte) return false;
        if (left_byte < 0) return true;
    }
}

static bool sol_effect_equal(
    const SolSource *source,
    const SolEffect *left,
    const SolEffect *right
) {
    return left->is_pure == right->is_pure
        && left->has_argument == right->has_argument
        && sol_effect_span_equal(source, left->name, right->name)
        && (!left->has_argument
            || sol_effect_span_equal(source, left->argument, right->argument));
}

static void sol_effect_error(
    SolEffectChecker *checker,
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

static bool sol_effect_function_contains(
    SolEffectChecker *checker,
    SolDefId function,
    const SolEffect *required
) {
    SolEffectId effect_id = checker->syntax->items[function].first_effect;
    size_t traversed = 0;
    while (effect_id != SOL_AST_NONE) {
        if (effect_id >= checker->syntax->effect_count
            || traversed++ >= checker->syntax->effect_count) {
            checker->malformed = true;
            return false;
        }
        const SolEffect *effect = &checker->syntax->effects[effect_id];
        if (!effect->is_pure && sol_effect_equal(checker->source, effect, required)) return true;
        effect_id = effect->next;
    }
    return false;
}

static void sol_effect_check_call(SolEffectChecker *checker, const SolExpr *call) {
    SolExprId callee_id = call->as.call.callee;
    const SolExpr *callee = &checker->syntax->expressions[callee_id];
    if (callee->kind != SOL_EXPR_PATH && callee->kind != SOL_EXPR_FIELD) return;
    SolResolution resolution = checker->hir->resolutions[callee_id];
    if (resolution.kind != SOL_RESOLUTION_DEFINITION
        || resolution.target >= checker->syntax->item_count
        || checker->syntax->items[resolution.target].kind != SOL_ITEM_FUNCTION) {
        return;
    }

    SolEffectId effect_id = checker->syntax->items[resolution.target].first_effect;
    size_t traversed = 0;
    while (effect_id != SOL_AST_NONE) {
        if (effect_id >= checker->syntax->effect_count
            || traversed++ >= checker->syntax->effect_count) {
            checker->malformed = true;
            return;
        }
        const SolEffect *required = &checker->syntax->effects[effect_id];
        if (!required->is_pure
            && !sol_effect_function_contains(checker, checker->current_function, required)) {
            sol_effect_error(
                checker,
                "SOL-EFFECT-002",
                call->span,
                "call performs an effect not declared by the caller"
            );
        }
        effect_id = required->next;
    }
}

static void sol_effect_expression(SolEffectChecker *checker, SolExprId expression_id);

static void sol_effect_arguments(SolEffectChecker *checker, SolArgumentId argument_id) {
    size_t traversed = 0;
    while (argument_id != SOL_AST_NONE) {
        if (traversed++ >= checker->syntax->argument_count) {
            checker->malformed = true;
            return;
        }
        const SolArgument *argument = &checker->syntax->arguments[argument_id];
        sol_effect_expression(checker, argument->value);
        argument_id = argument->next;
    }
}

static void sol_effect_expression(SolEffectChecker *checker, SolExprId expression_id) {
    if (expression_id >= checker->syntax->expression_count) {
        checker->malformed = true;
        return;
    }
    if (checker->visited[expression_id]) return;
    if (checker->depth >= 256) {
        if (!checker->depth_reported) {
            sol_effect_error(
                checker,
                "SOL-EFFECT-004",
                checker->syntax->expressions[expression_id].span,
                "expression structure exceeds the effect-checking limit of 256"
            );
            checker->depth_reported = true;
        }
        return;
    }
    checker->visited[expression_id] = 1;
    ++checker->depth;
    const SolExpr *expression = &checker->syntax->expressions[expression_id];
    switch (expression->kind) {
        case SOL_EXPR_UNARY:
            sol_effect_expression(checker, expression->as.unary.operand);
            break;
        case SOL_EXPR_BINARY:
            sol_effect_expression(checker, expression->as.binary.left);
            sol_effect_expression(checker, expression->as.binary.right);
            break;
        case SOL_EXPR_CALL:
            sol_effect_check_call(checker, expression);
            sol_effect_expression(checker, expression->as.call.callee);
            sol_effect_arguments(checker, expression->as.call.first_argument);
            break;
        case SOL_EXPR_FIELD:
            sol_effect_expression(checker, expression->as.field.base);
            break;
        case SOL_EXPR_RECORD:
            sol_effect_expression(checker, expression->as.record.type);
            sol_effect_arguments(checker, expression->as.record.first_field);
            break;
        case SOL_EXPR_IF:
            sol_effect_expression(checker, expression->as.if_expr.condition);
            sol_effect_expression(checker, expression->as.if_expr.then_branch);
            sol_effect_expression(checker, expression->as.if_expr.else_branch);
            break;
        case SOL_EXPR_MATCH: {
            sol_effect_expression(checker, expression->as.match_expr.scrutinee);
            SolMatchArmId arm_id = expression->as.match_expr.first_arm;
            size_t traversed = 0;
            while (arm_id != SOL_AST_NONE) {
                if (traversed++ >= checker->syntax->match_arm_count) {
                    checker->malformed = true;
                    break;
                }
                const SolMatchArm *arm = &checker->syntax->match_arms[arm_id];
                sol_effect_expression(checker, arm->value);
                arm_id = arm->next;
            }
            break;
        }
        case SOL_EXPR_BLOCK: {
            SolStatementId statement_id = expression->as.block.first_statement;
            size_t traversed = 0;
            while (statement_id != SOL_AST_NONE) {
                if (traversed++ >= checker->syntax->statement_count) {
                    checker->malformed = true;
                    break;
                }
                const SolStatement *statement = &checker->syntax->statements[statement_id];
                SolExprId value = statement->kind == SOL_STATEMENT_LET
                    ? statement->as.let_statement.value
                    : statement->as.expression;
                sol_effect_expression(checker, value);
                statement_id = statement->next;
            }
            break;
        }
        case SOL_EXPR_PROPAGATE:
            sol_effect_expression(checker, expression->as.propagated);
            break;
        default:
            break;
    }
    --checker->depth;
}

static void sol_effect_validate_row(SolEffectChecker *checker, SolDefId function) {
    SolEffectId effect_id = checker->syntax->items[function].first_effect;
    size_t count = 0;
    bool saw_pure = false;
    while (effect_id != SOL_AST_NONE) {
        if (effect_id >= checker->syntax->effect_count
            || count++ >= checker->syntax->effect_count) {
            checker->malformed = true;
            return;
        }
        const SolEffect *effect = &checker->syntax->effects[effect_id];
        if (effect->is_pure) saw_pure = true;
        if (effect->is_pure && effect->has_argument) {
            sol_effect_error(
                checker,
                "SOL-EFFECT-001",
                effect->span,
                "pure cannot have an effect argument"
            );
        }
        SolEffectId previous = checker->syntax->items[function].first_effect;
        size_t previous_count = 0;
        while (previous != effect_id) {
            if (previous >= checker->syntax->effect_count
                || previous_count++ >= checker->syntax->effect_count) {
                checker->malformed = true;
                return;
            }
            if (sol_effect_equal(
                checker->source,
                &checker->syntax->effects[previous],
                effect
            )) {
                sol_effect_error(
                    checker,
                    "SOL-EFFECT-001",
                    effect->span,
                    "duplicate effect in function row"
                );
                break;
            }
            previous = checker->syntax->effects[previous].next;
        }
        effect_id = effect->next;
    }
    if (saw_pure && count > 1) {
        sol_effect_error(
            checker,
            "SOL-EFFECT-001",
            checker->syntax->items[function].span,
            "pure cannot be combined with other effects"
        );
    }
}

bool sol_effect_check(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolHirModule *hir,
    SolDiagnostics *diagnostics
) {
    if (source == NULL || syntax == NULL || hir == NULL || diagnostics == NULL
        || hir->definition_count != syntax->item_count
        || hir->resolution_count != syntax->expression_count) {
        if (diagnostics != NULL) {
            sol_diagnostics_add(
                diagnostics,
                "SOL-INTERNAL-004",
                SOL_SEVERITY_ERROR,
                (SolSpan){0},
                "invalid syntax or HIR passed to effect checking"
            );
        }
        return false;
    }
    unsigned char *visited = calloc(syntax->expression_count, sizeof(*visited));
    if (syntax->expression_count != 0 && visited == NULL) return false;
    SolEffectChecker checker = {
        .source = source,
        .syntax = syntax,
        .hir = hir,
        .diagnostics = diagnostics,
        .visited = visited,
    };
    for (size_t index = 0; index < syntax->item_count; ++index) {
        const SolSyntaxItem *item = &syntax->items[index];
        if (item->kind != SOL_ITEM_FUNCTION) continue;
        sol_effect_validate_row(&checker, index);
        if (item->body != SOL_AST_NONE) {
            memset(visited, 0, syntax->expression_count * sizeof(*visited));
            checker.current_function = index;
            checker.depth = 0;
            sol_effect_expression(&checker, item->body);
        }
    }
    free(visited);
    if (checker.malformed) {
        sol_diagnostics_add(
            diagnostics,
            "SOL-INTERNAL-004",
            SOL_SEVERITY_ERROR,
            (SolSpan){0},
            "malformed syntax encountered during effect checking"
        );
    }
    return !checker.malformed && !diagnostics->allocation_failed;
}
