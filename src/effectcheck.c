#include "sol/effectcheck.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const SolSource *source;
    const SolSyntaxTree *syntax;
    const SolHirModule *hir;
    const SolTypeTable *types;
    SolDiagnostics *diagnostics;
    unsigned char *visited;
    unsigned char *reported_substitutions;
    SolDefId current_function;
    size_t depth;
    bool depth_reported;
    bool malformed;
} SolEffectChecker;

static bool sol_effect_span_valid(const SolSource *source, SolSpan span) {
    return span.start <= span.end && span.end <= source->length;
}

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

static bool sol_effect_span_text_equal(
    const SolSource *source,
    SolSpan span,
    const char *text
) {
    size_t length = span.end - span.start;
    return strlen(text) == length
        && memcmp(source->text + span.start, text, length) == 0;
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

static bool sol_effect_row_contains(
    SolEffectChecker *checker,
    SolEffectId effect_id,
    const SolEffect *required,
    const SolSpan *substituted_argument
) {
    size_t traversed = 0;
    while (effect_id != SOL_AST_NONE) {
        if (effect_id >= checker->syntax->effect_count
            || traversed++ >= checker->syntax->effect_count) {
            checker->malformed = true;
            return false;
        }
        const SolEffect *effect = &checker->syntax->effects[effect_id];
        bool argument_matches = effect->has_argument == required->has_argument
            && (!effect->has_argument
                || (substituted_argument != NULL
                    ? sol_effect_span_equal(
                        checker->source,
                        effect->argument,
                        *substituted_argument
                    )
                    : sol_effect_span_equal(
                        checker->source,
                        effect->argument,
                        required->argument
                    )));
        if (!effect->is_pure
            && !required->is_pure
            && argument_matches
            && sol_effect_span_equal(checker->source, effect->name, required->name)) {
            return true;
        }
        effect_id = effect->next;
    }
    return false;
}

static bool sol_effect_direct_parameter(
    SolEffectChecker *checker,
    SolExprId expression_id,
    SolSpan *name
) {
    if (expression_id >= checker->syntax->expression_count) {
        checker->malformed = true;
        return false;
    }
    const SolExpr *expression = &checker->syntax->expressions[expression_id];
    SolResolution resolution = checker->hir->resolutions[expression_id];
    if (expression->kind != SOL_EXPR_PATH
        || resolution.kind != SOL_RESOLUTION_LOCAL
        || resolution.target >= checker->hir->local_count) {
        return false;
    }
    const SolHirLocal *local = &checker->hir->locals[resolution.target];
    if (local->kind != SOL_LOCAL_PARAMETER || local->owner != checker->current_function
        || resolution.target >= checker->types->local_count) {
        return false;
    }
    SolType type = checker->types->locals[resolution.target];
    if (type.kind != SOL_TYPE_NOMINAL
        || type.definition >= checker->syntax->item_count
        || checker->syntax->items[type.definition].kind != SOL_ITEM_CAPABILITY) {
        return false;
    }
    *name = local->name;
    return true;
}

static bool sol_effect_parameter_is_capability(
    SolEffectChecker *checker,
    SolParameterId parameter_id
) {
    const SolParameter *parameter = &checker->syntax->parameters[parameter_id];
    if (parameter->type_id >= checker->types->declared_type_count) {
        checker->malformed = true;
        return false;
    }
    SolType type = checker->types->declared_types[parameter->type_id];
    return type.kind == SOL_TYPE_NOMINAL
        && type.definition < checker->syntax->item_count
        && checker->syntax->items[type.definition].kind == SOL_ITEM_CAPABILITY;
}

static SolParameterId sol_effect_find_parameter(
    SolEffectChecker *checker,
    SolParameterId parameter_id,
    SolSpan name
) {
    size_t traversed = 0;
    while (parameter_id != SOL_AST_NONE) {
        if (parameter_id >= checker->syntax->parameter_count
            || traversed++ >= checker->syntax->parameter_count) {
            checker->malformed = true;
            return SOL_AST_NONE;
        }
        const SolParameter *parameter = &checker->syntax->parameters[parameter_id];
        if (sol_effect_span_equal(checker->source, parameter->name, name)
            && sol_effect_parameter_is_capability(checker, parameter_id)) {
            return parameter_id;
        }
        parameter_id = parameter->next;
    }
    return SOL_AST_NONE;
}

static SolExprId sol_effect_find_actual_argument(
    SolEffectChecker *checker,
    SolParameterId first_parameter,
    SolParameterId required_parameter,
    SolArgumentId argument_id
) {
    SolParameterId positional_parameter = first_parameter;
    size_t traversed = 0;
    while (argument_id != SOL_AST_NONE) {
        if (argument_id >= checker->syntax->argument_count
            || traversed++ >= checker->syntax->argument_count) {
            checker->malformed = true;
            return SOL_AST_NONE;
        }
        const SolArgument *argument = &checker->syntax->arguments[argument_id];
        SolParameterId matched = SOL_AST_NONE;
        if (argument->is_named) {
            matched = sol_effect_find_parameter(checker, first_parameter, argument->name);
        } else {
            matched = positional_parameter;
            if (positional_parameter != SOL_AST_NONE) {
                positional_parameter = checker->syntax->parameters[positional_parameter].next;
            }
        }
        if (matched == required_parameter) return argument->value;
        argument_id = argument->next;
    }
    return SOL_AST_NONE;
}

static bool sol_effect_substitute_argument(
    SolEffectChecker *checker,
    const SolExpr *call,
    const SolEffect *required,
    SolParameterId first_parameter,
    const SolSpan *self_argument,
    SolSpan *substituted_argument,
    bool *has_substitution
) {
    *has_substitution = false;
    if (!required->has_argument) return true;
    if (self_argument != NULL
        && sol_effect_span_text_equal(checker->source, required->argument, "Self")) {
        *substituted_argument = *self_argument;
        *has_substitution = true;
        return true;
    }
    SolParameterId parameter = sol_effect_find_parameter(
        checker,
        first_parameter,
        required->argument
    );
    if (parameter == SOL_AST_NONE) return !checker->malformed;
    SolExprId actual = sol_effect_find_actual_argument(
        checker,
        first_parameter,
        parameter,
        call->as.call.first_argument
    );
    if (actual == SOL_AST_NONE) {
        if (!checker->malformed) checker->malformed = true;
        return false;
    }
    if (!sol_effect_direct_parameter(checker, actual, substituted_argument)) {
        if (checker->malformed) return false;
        if (!checker->reported_substitutions[actual]) {
            sol_effect_error(
                checker,
                "SOL-EFFECT-003",
                checker->syntax->expressions[actual].span,
                "effect-bearing arguments must be direct capability parameters"
            );
            checker->reported_substitutions[actual] = 1;
        }
        return false;
    }
    *has_substitution = true;
    return true;
}

static void sol_effect_check_call(SolEffectChecker *checker, const SolExpr *call) {
    SolExprId callee_id = call->as.call.callee;
    const SolExpr *callee = &checker->syntax->expressions[callee_id];
    SolResolution resolution = checker->hir->resolutions[callee_id];
    SolEffectId effect_id = SOL_AST_NONE;
    SolParameterId first_parameter = SOL_AST_NONE;
    SolSpan receiver = {0};
    const SolSpan *receiver_pointer = NULL;
    SolType callee_type = checker->types->expressions[callee_id];
    if (callee_type.kind == SOL_TYPE_FUNCTION
        && callee_type.definition < checker->syntax->item_count
        && checker->syntax->items[callee_type.definition].kind == SOL_ITEM_FUNCTION) {
        const SolSyntaxItem *function = &checker->syntax->items[callee_type.definition];
        effect_id = function->first_effect;
        first_parameter = function->first_parameter;
    } else if (resolution.kind == SOL_RESOLUTION_DEFINITION
        && resolution.target < checker->syntax->item_count
        && checker->syntax->items[resolution.target].kind == SOL_ITEM_FUNCTION) {
        const SolSyntaxItem *function = &checker->syntax->items[resolution.target];
        effect_id = function->first_effect;
        first_parameter = function->first_parameter;
    } else if (callee_type.kind == SOL_TYPE_CAPABILITY_OPERATION
        && callee_type.definition < checker->syntax->capability_member_count
        && callee->kind == SOL_EXPR_FIELD) {
        SolCapabilityMemberId member_id = callee_type.definition;
        SolExprId receiver_id = callee->as.field.base;
        if (!sol_effect_direct_parameter(checker, receiver_id, &receiver)) {
            checker->malformed = true;
            return;
        }
        const SolCapabilityMember *member = &checker->syntax->capability_members[member_id];
        effect_id = member->first_effect;
        first_parameter = member->first_parameter;
        receiver_pointer = &receiver;
    } else {
        return;
    }

    size_t traversed = 0;
    memset(
        checker->reported_substitutions,
        0,
        checker->syntax->expression_count * sizeof(*checker->reported_substitutions)
    );
    while (effect_id != SOL_AST_NONE) {
        if (effect_id >= checker->syntax->effect_count
            || traversed++ >= checker->syntax->effect_count) {
            checker->malformed = true;
            return;
        }
        const SolEffect *required = &checker->syntax->effects[effect_id];
        if (required->is_pure) {
            effect_id = required->next;
            continue;
        }
        SolSpan substituted_argument = {0};
        bool has_substitution = false;
        bool substitution_valid = sol_effect_substitute_argument(
            checker,
            call,
            required,
            first_parameter,
            receiver_pointer,
            &substituted_argument,
            &has_substitution
        );
        if (substitution_valid
            && !sol_effect_row_contains(
                checker,
                checker->syntax->items[checker->current_function].first_effect,
                required,
                has_substitution ? &substituted_argument : NULL
            )) {
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
        if (argument_id >= checker->syntax->argument_count
            || traversed++ >= checker->syntax->argument_count) {
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

static void sol_effect_validate_row(
    SolEffectChecker *checker,
    SolEffectId first_effect,
    SolSpan owner_span,
    size_t owner_item
) {
    SolEffectId effect_id = first_effect;
    size_t count = 0;
    bool saw_pure = false;
    while (effect_id != SOL_AST_NONE) {
        if (effect_id >= checker->syntax->effect_count
            || count++ >= checker->syntax->effect_count) {
            checker->malformed = true;
            return;
        }
        const SolEffect *effect = &checker->syntax->effects[effect_id];
        if (effect->owner_item != owner_item) {
            checker->malformed = true;
            return;
        }
        if (effect->is_pure) saw_pure = true;
        if (effect->is_pure && effect->has_argument) {
            sol_effect_error(
                checker,
                "SOL-EFFECT-001",
                effect->span,
                "pure cannot have an effect argument"
            );
        }
        SolEffectId previous = first_effect;
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
            owner_span,
            "pure cannot be combined with other effects"
        );
    }
}

bool sol_effect_check(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolHirModule *hir,
    const SolTypeTable *types,
    SolDiagnostics *diagnostics
) {
    if (source == NULL || source->text == NULL || syntax == NULL || hir == NULL || types == NULL
        || diagnostics == NULL
        || syntax->item_count > syntax->item_capacity
        || syntax->expression_count > syntax->expression_capacity
        || syntax->parameter_count > syntax->parameter_capacity
        || syntax->argument_count > syntax->argument_capacity
        || syntax->statement_count > syntax->statement_capacity
        || syntax->match_arm_count > syntax->match_arm_capacity
        || syntax->effect_count > syntax->effect_capacity
        || syntax->capability_member_count > syntax->capability_member_capacity
        || (syntax->item_count != 0 && syntax->items == NULL)
        || (syntax->expression_count != 0 && syntax->expressions == NULL)
        || (syntax->parameter_count != 0 && syntax->parameters == NULL)
        || (syntax->argument_count != 0 && syntax->arguments == NULL)
        || (syntax->statement_count != 0 && syntax->statements == NULL)
        || (syntax->match_arm_count != 0 && syntax->match_arms == NULL)
        || (syntax->effect_count != 0 && syntax->effects == NULL)
        || (syntax->capability_member_count != 0 && syntax->capability_members == NULL)
        || hir->definition_count != syntax->item_count
        || hir->resolution_count != syntax->expression_count
        || hir->local_count > hir->local_capacity
        || (hir->resolution_count != 0 && hir->resolutions == NULL)
        || (hir->local_count != 0 && hir->locals == NULL)
        || types->expression_count != syntax->expression_count
        || types->local_count != hir->local_count
        || types->declared_type_count != syntax->type_count
        || (types->expression_count != 0 && types->expressions == NULL)
        || (types->local_count != 0 && types->locals == NULL)
        || (types->declared_type_count != 0 && types->declared_types == NULL)) {
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
    for (size_t index = 0; index < syntax->item_count; ++index) {
        const SolSyntaxItem *item = &syntax->items[index];
        if ((item->body != SOL_AST_NONE && item->body >= syntax->expression_count)
            || (item->first_effect != SOL_AST_NONE
                && item->first_effect >= syntax->effect_count)
            || (item->first_parameter != SOL_AST_NONE
                && item->first_parameter >= syntax->parameter_count)
            || (item->first_member != SOL_AST_NONE
                && item->first_member >= syntax->capability_member_count)) {
            sol_diagnostics_add(
                diagnostics,
                "SOL-INTERNAL-004",
                SOL_SEVERITY_ERROR,
                (SolSpan){0},
                "invalid syntax or HIR passed to effect checking"
            );
            return false;
        }
    }
    for (size_t index = 0; index < syntax->parameter_count; ++index) {
        const SolParameter *parameter = &syntax->parameters[index];
        if (!sol_effect_span_valid(source, parameter->name)
            || parameter->type_id >= syntax->type_count
            || (parameter->next != SOL_AST_NONE
                && parameter->next >= syntax->parameter_count)) {
            sol_diagnostics_add(
                diagnostics,
                "SOL-INTERNAL-004",
                SOL_SEVERITY_ERROR,
                (SolSpan){0},
                "invalid syntax or HIR passed to effect checking"
            );
            return false;
        }
    }
    for (size_t index = 0; index < syntax->argument_count; ++index) {
        const SolArgument *argument = &syntax->arguments[index];
        if (argument->value >= syntax->expression_count
            || (argument->is_named && !sol_effect_span_valid(source, argument->name))
            || (argument->next != SOL_AST_NONE
                && argument->next >= syntax->argument_count)) {
            sol_diagnostics_add(
                diagnostics,
                "SOL-INTERNAL-004",
                SOL_SEVERITY_ERROR,
                (SolSpan){0},
                "invalid syntax or HIR passed to effect checking"
            );
            return false;
        }
    }
    for (size_t index = 0; index < syntax->expression_count; ++index) {
        const SolExpr *expression = &syntax->expressions[index];
        if ((expression->kind == SOL_EXPR_CALL
                && expression->as.call.first_argument != SOL_AST_NONE
                && expression->as.call.first_argument >= syntax->argument_count)
            || (expression->kind == SOL_EXPR_RECORD
                && expression->as.record.first_field != SOL_AST_NONE
                && expression->as.record.first_field >= syntax->argument_count)) {
            sol_diagnostics_add(
                diagnostics,
                "SOL-INTERNAL-004",
                SOL_SEVERITY_ERROR,
                (SolSpan){0},
                "invalid syntax or HIR passed to effect checking"
            );
            return false;
        }
    }
    for (size_t index = 0; index < syntax->effect_count; ++index) {
        const SolEffect *effect = &syntax->effects[index];
        if (!sol_effect_span_valid(source, effect->name)
            || !sol_effect_span_valid(source, effect->argument)
            || !sol_effect_span_valid(source, effect->span)
            || (effect->next != SOL_AST_NONE && effect->next >= syntax->effect_count)
            || effect->owner_item >= syntax->item_count) {
            sol_diagnostics_add(
                diagnostics,
                "SOL-INTERNAL-004",
                SOL_SEVERITY_ERROR,
                (SolSpan){0},
                "invalid syntax or HIR passed to effect checking"
            );
            return false;
        }
    }
    for (size_t index = 0; index < syntax->capability_member_count; ++index) {
        const SolCapabilityMember *member = &syntax->capability_members[index];
        if (!sol_effect_span_valid(source, member->span)
            || (member->first_effect != SOL_AST_NONE
                && member->first_effect >= syntax->effect_count)
            || (member->first_parameter != SOL_AST_NONE
                && member->first_parameter >= syntax->parameter_count)
            || (member->next != SOL_AST_NONE
                && member->next >= syntax->capability_member_count)
            || member->owner_item >= syntax->item_count) {
            sol_diagnostics_add(
                diagnostics,
                "SOL-INTERNAL-004",
                SOL_SEVERITY_ERROR,
                (SolSpan){0},
                "invalid syntax or HIR passed to effect checking"
            );
            return false;
        }
    }
    unsigned char *visited = calloc(syntax->expression_count, sizeof(*visited));
    unsigned char *reported_substitutions = calloc(
        syntax->expression_count,
        sizeof(*reported_substitutions)
    );
    if (syntax->expression_count != 0
        && (visited == NULL || reported_substitutions == NULL)) {
        free(visited);
        free(reported_substitutions);
        return false;
    }
    SolEffectChecker checker = {
        .source = source,
        .syntax = syntax,
        .hir = hir,
        .types = types,
        .diagnostics = diagnostics,
        .visited = visited,
        .reported_substitutions = reported_substitutions,
    };
    for (size_t index = 0; index < syntax->item_count; ++index) {
        const SolSyntaxItem *item = &syntax->items[index];
        if (item->kind != SOL_ITEM_FUNCTION) continue;
        sol_effect_validate_row(&checker, item->first_effect, item->span, index);
        if (item->body != SOL_AST_NONE) {
            memset(visited, 0, syntax->expression_count * sizeof(*visited));
            checker.current_function = index;
            checker.depth = 0;
            sol_effect_expression(&checker, item->body);
        }
    }
    for (size_t index = 0; index < syntax->capability_member_count; ++index) {
        const SolCapabilityMember *member = &syntax->capability_members[index];
        sol_effect_validate_row(
            &checker,
            member->first_effect,
            member->span,
            member->owner_item
        );
    }
    free(visited);
    free(reported_substitutions);
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
