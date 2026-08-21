#include "sol/parser.h"

#include <stdint.h>
#include <stdlib.h>

typedef enum {
    SOL_EXPRESSION_CONTEXT_BODY,
    SOL_EXPRESSION_CONTEXT_REQUIRES,
    SOL_EXPRESSION_CONTEXT_ENSURES,
} SolExpressionContext;

typedef struct {
    const SolSource *source;
    const SolSyntaxTree *tree;
    unsigned char *expressions;
    unsigned char *arguments;
    unsigned char *statements;
    unsigned char *loop_invariants;
    unsigned char *arms;
    unsigned char *patterns;
    unsigned char *pattern_bindings;
    unsigned char *parameters;
    unsigned char *type_arguments;
    unsigned char *type_parameters;
    unsigned char *effect_parameters;
    unsigned char *fields;
    unsigned char *variants;
    unsigned char *effects;
    unsigned char *capability_members;
    unsigned char *trait_methods;
    unsigned char *clauses;
    unsigned char *conditions;
    size_t loop_depth;
} SolStructureValidator;

static bool sol_structure_span_valid(const SolStructureValidator *validator, SolSpan span) {
    return span.start <= span.end && span.end <= validator->source->length;
}

static bool sol_structure_name_valid(
    const SolStructureValidator *validator,
    SolSpan span
) {
    return sol_structure_span_valid(validator, span) && span.start < span.end;
}

static bool sol_structure_unary_operator_valid(SolTokenKind kind) {
    return kind == SOL_TOKEN_BANG || kind == SOL_TOKEN_MINUS;
}

static bool sol_structure_binary_operator_valid(SolTokenKind kind) {
    switch (kind) {
        case SOL_TOKEN_PIPE_PIPE:
        case SOL_TOKEN_AMP_AMP:
        case SOL_TOKEN_EQUAL_EQUAL:
        case SOL_TOKEN_BANG_EQUAL:
        case SOL_TOKEN_LESS:
        case SOL_TOKEN_LESS_EQUAL:
        case SOL_TOKEN_GREATER:
        case SOL_TOKEN_GREATER_EQUAL:
        case SOL_TOKEN_PLUS:
        case SOL_TOKEN_MINUS:
        case SOL_TOKEN_STAR:
        case SOL_TOKEN_SLASH:
        case SOL_TOKEN_PERCENT:
            return true;
        default:
            return false;
    }
}

static bool sol_structure_assignment_operator_valid(SolTokenKind kind) {
    return kind == SOL_TOKEN_EQUAL || kind == SOL_TOKEN_PLUS_EQUAL
        || kind == SOL_TOKEN_MINUS_EQUAL || kind == SOL_TOKEN_STAR_EQUAL
        || kind == SOL_TOKEN_SLASH_EQUAL || kind == SOL_TOKEN_PERCENT_EQUAL;
}

static bool sol_structure_expression(
    SolStructureValidator *validator,
    SolExprId expression_id,
    SolExpressionContext context,
    size_t depth
);

static bool sol_structure_type_arguments(
    SolStructureValidator *validator,
    SolTypeArgumentId argument,
    bool allow_access,
    size_t minimum_count,
    size_t maximum_count
) {
    size_t count = 0;
    while (argument != SOL_AST_NONE) {
        if (argument >= validator->tree->type_argument_count
            || validator->type_arguments[argument]) return false;
        validator->type_arguments[argument] = 1;
        const SolTypeArgument *current = &validator->tree->type_arguments[argument];
        if (current->type >= validator->tree->type_count
            || (int)current->access < 0 || current->access > SOL_ACCESS_EXCLUSIVE
            || (!allow_access && current->access != SOL_ACCESS_OWNED)) {
            return false;
        }
        ++count;
        argument = current->next;
    }
    return count >= minimum_count && count <= maximum_count;
}

static bool sol_structure_parameters(
    SolStructureValidator *validator,
    SolParameterId parameter
) {
    while (parameter != SOL_AST_NONE) {
        if (parameter >= validator->tree->parameter_count
            || validator->parameters[parameter]) return false;
        validator->parameters[parameter] = 1;
        const SolParameter *current = &validator->tree->parameters[parameter];
        if (!sol_structure_name_valid(validator, current->name)
            || !sol_structure_name_valid(validator, current->type)
            || current->type_id >= validator->tree->type_count
            || (int)current->access < 0 || current->access > SOL_ACCESS_EXCLUSIVE) {
            return false;
        }
        parameter = current->next;
    }
    return true;
}

static bool sol_structure_parameter_contains(
    const SolStructureValidator *validator,
    SolParameterId first,
    SolParameterId target
) {
    size_t traversed = 0;
    while (first != SOL_AST_NONE && traversed++ < validator->tree->parameter_count) {
        if (first >= validator->tree->parameter_count) return false;
        if (first == target) return true;
        first = validator->tree->parameters[first].next;
    }
    return false;
}

static bool sol_structure_type_parameters(
    SolStructureValidator *validator,
    SolTypeParameterId parameter,
    size_t owner
) {
    while (parameter != SOL_AST_NONE) {
        if (parameter >= validator->tree->type_parameter_count
            || validator->type_parameters[parameter]) return false;
        validator->type_parameters[parameter] = 1;
        const SolTypeParameter *current = &validator->tree->type_parameters[parameter];
        if (current->owner_item != owner
            || !sol_structure_name_valid(validator, current->name)
            || !sol_structure_span_valid(validator, current->bound)) return false;
        parameter = current->next;
    }
    return true;
}

static bool sol_structure_effect_parameters(
    SolStructureValidator *validator,
    SolEffectParameterId parameter,
    size_t owner
) {
    while (parameter != SOL_AST_NONE) {
        if (parameter >= validator->tree->effect_parameter_count
            || validator->effect_parameters[parameter]) return false;
        validator->effect_parameters[parameter] = 1;
        const SolEffectParameter *current = &validator->tree->effect_parameters[parameter];
        if (current->owner_item != owner
            || !sol_structure_name_valid(validator, current->name)) return false;
        parameter = current->next;
    }
    return true;
}

static bool sol_structure_effects(
    SolStructureValidator *validator,
    SolEffectId effect,
    SolEffectOwnerKind owner_kind,
    size_t owner
) {
    while (effect != SOL_AST_NONE) {
        if (effect >= validator->tree->effect_count || validator->effects[effect]) {
            return false;
        }
        validator->effects[effect] = 1;
        const SolEffect *current = &validator->tree->effects[effect];
        if ((int)current->owner_kind < 0 || current->owner_kind > SOL_EFFECT_OWNER_TYPE
            || current->owner_kind != owner_kind || current->owner != owner
            || !sol_structure_name_valid(validator, current->name)
            || !sol_structure_span_valid(validator, current->span)
            || (current->has_argument
                ? !sol_structure_name_valid(validator, current->argument)
                : current->argument.start != current->argument.end)) return false;
        effect = current->next;
    }
    return true;
}

static bool sol_structure_fields(
    SolStructureValidator *validator,
    SolFieldId field
) {
    while (field != SOL_AST_NONE) {
        if (field >= validator->tree->field_count || validator->fields[field]) return false;
        validator->fields[field] = 1;
        const SolField *current = &validator->tree->fields[field];
        if (!sol_structure_name_valid(validator, current->name)
            || !sol_structure_span_valid(validator, current->span)
            || current->type >= validator->tree->type_count) return false;
        field = current->next;
    }
    return true;
}

static bool sol_structure_variants(
    SolStructureValidator *validator,
    SolVariantId variant,
    size_t owner
) {
    while (variant != SOL_AST_NONE) {
        if (variant >= validator->tree->variant_count || validator->variants[variant]) {
            return false;
        }
        validator->variants[variant] = 1;
        const SolVariant *current = &validator->tree->variants[variant];
        if (current->owner_item != owner
            || !sol_structure_name_valid(validator, current->name)
            || !sol_structure_span_valid(validator, current->span)
            || !sol_structure_fields(validator, current->first_field)) return false;
        variant = current->next;
    }
    return true;
}

static bool sol_structure_arguments(
    SolStructureValidator *validator,
    SolArgumentId argument,
    SolExpressionContext context,
    size_t depth,
    bool allow_named,
    size_t minimum_count,
    size_t maximum_count
) {
    size_t count = 0;
    while (argument != SOL_AST_NONE) {
        if (argument >= validator->tree->argument_count || validator->arguments[argument]) {
            return false;
        }
        validator->arguments[argument] = 1;
        const SolArgument *current = &validator->tree->arguments[argument];
        if ((!allow_named && current->is_named)
            || (current->is_named
                && !sol_structure_name_valid(validator, current->name))
            || (!current->is_named
                && (current->name.start != 0 || current->name.end != 0))
            || !sol_structure_expression(validator, current->value, context, depth)) {
            return false;
        }
        ++count;
        argument = current->next;
    }
    return count >= minimum_count && count <= maximum_count;
}

static bool sol_structure_statements(
    SolStructureValidator *validator,
    SolStatementId statement,
    SolExpressionContext context,
    size_t depth
) {
    while (statement != SOL_AST_NONE) {
        if (statement >= validator->tree->statement_count || validator->statements[statement]) {
            return false;
        }
        validator->statements[statement] = 1;
        const SolStatement *current = &validator->tree->statements[statement];
        if ((int)current->kind < 0 || current->kind > SOL_STATEMENT_REQUIRE
            || !sol_structure_span_valid(validator, current->span)
            || ((current->kind == SOL_STATEMENT_REGION
                    || current->kind == SOL_STATEMENT_MODIFY
                    || current->kind == SOL_STATEMENT_LOOP
                    || current->kind == SOL_STATEMENT_WHILE
                    || current->kind == SOL_STATEMENT_BREAK
                    || current->kind == SOL_STATEMENT_CONTINUE
                    || current->kind == SOL_STATEMENT_PANIC
                    || current->kind == SOL_STATEMENT_UNREACHABLE
                    || current->kind == SOL_STATEMENT_REQUIRE)
                && context != SOL_EXPRESSION_CONTEXT_BODY)) return false;
        if ((current->kind == SOL_STATEMENT_LET
                || current->kind == SOL_STATEMENT_VAR)
            && (!sol_structure_name_valid(validator, current->as.let_statement.name)
                || (current->kind == SOL_STATEMENT_LET
                    && (current->as.let_statement.value == SOL_AST_NONE
                        || current->as.let_statement.type_id != SOL_AST_NONE))
                || (current->kind == SOL_STATEMENT_VAR
                    && ((current->as.let_statement.value == SOL_AST_NONE)
                        == (current->as.let_statement.type_id == SOL_AST_NONE)))
                || (current->as.let_statement.type_id != SOL_AST_NONE
                    && current->as.let_statement.type_id >= validator->tree->type_count))) {
            return false;
        }
        SolExprId value = SOL_AST_NONE;
        switch (current->kind) {
            case SOL_STATEMENT_LET:
            case SOL_STATEMENT_VAR:
                value = current->as.let_statement.value;
                break;
            case SOL_STATEMENT_ASSIGNMENT:
                value = current->as.assignment.value;
                break;
            case SOL_STATEMENT_RETURN:
            case SOL_STATEMENT_EXPRESSION:
                value = current->as.expression;
                break;
            case SOL_STATEMENT_REGION:
                value = current->as.region_statement.body;
                break;
            case SOL_STATEMENT_MODIFY:
                value = current->as.modify.body;
                break;
            case SOL_STATEMENT_PANIC:
                value = current->as.panic_statement.message;
                break;
            case SOL_STATEMENT_UNREACHABLE:
                value = current->as.unreachable_statement.proof;
                break;
            case SOL_STATEMENT_LOOP:
            case SOL_STATEMENT_WHILE:
            case SOL_STATEMENT_BREAK:
            case SOL_STATEMENT_CONTINUE:
            case SOL_STATEMENT_REQUIRE:
                break;
        }
        if (current->kind == SOL_STATEMENT_REGION
            && (!sol_structure_span_valid(validator,
                    current->as.region_statement.label)
                || current->as.region_statement.label.start
                    == current->as.region_statement.label.end
                || value >= validator->tree->expression_count
                || validator->tree->expressions[value].kind != SOL_EXPR_BLOCK)) {
            return false;
        }
        if (current->kind == SOL_STATEMENT_ASSIGNMENT
            && (!sol_structure_assignment_operator_valid(
                    current->as.assignment.operator_kind)
                || !sol_structure_expression(validator, current->as.assignment.target,
                    context, depth))) return false;
        if (current->kind == SOL_STATEMENT_MODIFY
            && (current->as.modify.body >= validator->tree->expression_count
                || validator->tree->expressions[current->as.modify.body].kind
                    != SOL_EXPR_BLOCK
                || !sol_structure_expression(validator, current->as.modify.target,
                    context, depth))) return false;
        if (current->kind == SOL_STATEMENT_UNREACHABLE) {
            SolSpan because = current->as.unreachable_statement.because_span;
            if (!sol_structure_span_valid(validator, because)
                || because.start == because.end
                || because.start < current->span.start
                || because.end > current->span.end
                || value >= validator->tree->expression_count
                || validator->tree->expressions[value].span.start < because.start
                || validator->tree->expressions[value].span.end > because.end) {
                return false;
            }
        }
        if (current->kind == SOL_STATEMENT_REQUIRE) {
            SolExprId condition = current->as.require_statement.condition;
            SolExprId fallback = current->as.require_statement.fallback_block;
            if (!sol_structure_expression(validator, condition, context, depth)
                || fallback >= validator->tree->expression_count
                || validator->tree->expressions[fallback].kind != SOL_EXPR_BLOCK
                || !sol_structure_expression(validator, fallback, context, depth)) {
                return false;
            }
        }
        if (current->kind == SOL_STATEMENT_LOOP
            || current->kind == SOL_STATEMENT_WHILE) {
            SolExprId condition = current->as.loop_statement.condition;
            SolExprId body = current->as.loop_statement.body;
            SolLoopInvariantId invariant
                = current->as.loop_statement.first_invariant;
            SolSpan invariant_span = current->as.loop_statement.invariant_span;
            SolExprId decreases = current->as.loop_statement.decreases;
            SolSpan decreases_span = current->as.loop_statement.decreases_span;
            size_t prefix_end = condition != SOL_AST_NONE
                    && condition < validator->tree->expression_count
                ? validator->tree->expressions[condition].span.end
                : current->span.start;
            ++validator->loop_depth;
            if ((current->kind == SOL_STATEMENT_LOOP && condition != SOL_AST_NONE)
                || (current->kind == SOL_STATEMENT_WHILE
                    && !sol_structure_expression(validator, condition, context, depth))
                || ((invariant == SOL_AST_NONE)
                    != (invariant_span.start == 0 && invariant_span.end == 0))
                || ((decreases == SOL_AST_NONE)
                    != (decreases_span.start == 0 && decreases_span.end == 0))
                || (invariant != SOL_AST_NONE
                    && (!sol_structure_span_valid(validator, invariant_span)
                        || invariant_span.start == invariant_span.end
                        || invariant_span.start < prefix_end))
                || (decreases != SOL_AST_NONE
                    && (!sol_structure_span_valid(validator, decreases_span)
                        || decreases_span.start == decreases_span.end
                        || decreases_span.start < prefix_end
                        || decreases >= validator->tree->expression_count
                        || validator->tree->expressions[decreases].span.start
                            < decreases_span.start
                        || validator->tree->expressions[decreases].span.end
                            > decreases_span.end
                        || !sol_structure_expression(
                            validator, decreases, context, depth)))
                || (invariant != SOL_AST_NONE && decreases != SOL_AST_NONE
                    && invariant_span.end > decreases_span.start)
                || body >= validator->tree->expression_count
                || validator->tree->expressions[body].kind != SOL_EXPR_BLOCK) {
                --validator->loop_depth;
                return false;
            }
            size_t previous_invariant_end = invariant_span.start;
            while (invariant != SOL_AST_NONE) {
                if (invariant >= validator->tree->loop_invariant_count
                    || validator->loop_invariants[invariant]) {
                    --validator->loop_depth;
                    return false;
                }
                validator->loop_invariants[invariant] = 1;
                const SolLoopInvariant *entry
                    = &validator->tree->loop_invariants[invariant];
                if (!sol_structure_span_valid(validator, entry->span)
                    || entry->span.start < invariant_span.start
                    || entry->span.start < previous_invariant_end
                    || entry->span.end > invariant_span.end
                    || entry->expression >= validator->tree->expression_count
                    || validator->tree->expressions[entry->expression].span.start
                        != entry->span.start
                    || validator->tree->expressions[entry->expression].span.end
                        != entry->span.end
                    || !sol_structure_expression(
                        validator, entry->expression, context, depth)) {
                    --validator->loop_depth;
                    return false;
                }
                previous_invariant_end = entry->span.end;
                invariant = entry->next;
            }
            size_t body_start = validator->tree->expressions[body].span.start;
            if ((invariant_span.start != 0 && invariant_span.end > body_start)
                || (decreases_span.start != 0 && decreases_span.end > body_start)) {
                --validator->loop_depth;
                return false;
            }
            bool body_valid = sol_structure_expression(validator, body, context, depth);
            --validator->loop_depth;
            if (!body_valid) return false;
        }
        if ((current->kind == SOL_STATEMENT_BREAK
                || current->kind == SOL_STATEMENT_CONTINUE)
            && (validator->loop_depth == 0 || current->next != SOL_AST_NONE
                || current->as.expression != SOL_AST_NONE)) return false;
        if (value != SOL_AST_NONE
            && !sol_structure_expression(validator, value, context, depth)) return false;
        statement = current->next;
    }
    return true;
}

static bool sol_structure_pattern(
    SolStructureValidator *validator,
    SolPatternId pattern_id,
    size_t depth
) {
    if (pattern_id >= validator->tree->pattern_count || depth >= 64
        || validator->patterns[pattern_id]) return false;
    validator->patterns[pattern_id] = 1;
    const SolPattern *pattern = &validator->tree->patterns[pattern_id];
    if ((int)pattern->kind < 0 || pattern->kind > SOL_PATTERN_TUPLE
        || !sol_structure_name_valid(validator, pattern->span)
        || (pattern->kind != SOL_PATTERN_BOOL && pattern->bool_value)) return false;
    bool named = pattern->kind == SOL_PATTERN_VARIANT
        || pattern->kind == SOL_PATTERN_BINDING
        || pattern->kind == SOL_PATTERN_RECORD;
    if (named) {
        if (!sol_structure_name_valid(validator, pattern->name)
            || pattern->name.start < pattern->span.start
            || pattern->name.end > pattern->span.end) return false;
    } else if (pattern->name.start != 0 || pattern->name.end != 0) {
        return false;
    }
    bool has_children = pattern->kind == SOL_PATTERN_VARIANT
        || pattern->kind == SOL_PATTERN_RECORD || pattern->kind == SOL_PATTERN_TUPLE;
    if (!has_children) return pattern->first_binding == SOL_AST_NONE;

    SolPatternBindingId binding = pattern->first_binding;
    size_t child_count = 0;
    size_t previous_end = pattern->name.end;
    while (binding != SOL_AST_NONE) {
        if (binding >= validator->tree->pattern_binding_count
            || validator->pattern_bindings[binding]) return false;
        validator->pattern_bindings[binding] = 1;
        const SolPatternBinding *current = &validator->tree->pattern_bindings[binding];
        bool record = pattern->kind == SOL_PATTERN_RECORD;
        if ((record && (!sol_structure_name_valid(validator, current->field)
                || current->field.start < pattern->name.end
                || current->field.end > pattern->span.end))
            || (!record && (current->field.start != 0 || current->field.end != 0))
            || current->pattern >= validator->tree->pattern_count
            || validator->tree->patterns[current->pattern].span.start < previous_end
            || validator->tree->patterns[current->pattern].span.end > pattern->span.end
            || !sol_structure_pattern(validator, current->pattern, depth + 1)) return false;
        previous_end = validator->tree->patterns[current->pattern].span.end;
        ++child_count;
        binding = current->next;
    }
    return pattern->kind != SOL_PATTERN_TUPLE
        || (child_count >= 2 && child_count <= 16);
}

static bool sol_structure_arms(
    SolStructureValidator *validator,
    SolMatchArmId arm,
    SolExpressionContext context,
    size_t depth
) {
    while (arm != SOL_AST_NONE) {
        if (arm >= validator->tree->match_arm_count || validator->arms[arm]) return false;
        validator->arms[arm] = 1;
        const SolMatchArm *current = &validator->tree->match_arms[arm];
        if (!sol_structure_span_valid(validator, current->span)
            || !sol_structure_pattern(validator, current->pattern, 0)
            || validator->tree->patterns[current->pattern].span.start
                != current->span.start
            || current->value >= validator->tree->expression_count
            || (current->guard != SOL_AST_NONE
                && current->guard >= validator->tree->expression_count)
            || (current->guard != SOL_AST_NONE
                && (validator->tree->expressions[current->guard].span.start
                        < validator->tree->patterns[current->pattern].span.end
                    || validator->tree->expressions[current->guard].span.end
                        > validator->tree->expressions[current->value].span.start
                    || !sol_structure_expression(
                        validator, current->guard, context, depth)))
            || validator->tree->expressions[current->value].span.end != current->span.end
            || !sol_structure_expression(validator, current->value, context, depth)) {
            return false;
        }
        arm = current->next;
    }
    return true;
}

static bool sol_structure_expression(
    SolStructureValidator *validator,
    SolExprId expression_id,
    SolExpressionContext context,
    size_t depth
) {
    if (expression_id >= validator->tree->expression_count || depth >= 4096
        || validator->expressions[expression_id] != 0) {
        return false;
    }
    validator->expressions[expression_id] = 1;
    const SolExpr *expression = &validator->tree->expressions[expression_id];
    if ((int)expression->kind < 0 || expression->kind > SOL_EXPR_TYPE_APPLICATION) {
        return false;
    }
    bool valid = sol_structure_span_valid(validator, expression->span);
    switch (expression->kind) {
        case SOL_EXPR_PATH:
            valid = valid && sol_structure_name_valid(validator, expression->as.name);
            break;
        case SOL_EXPR_UNARY:
            valid = valid
                && sol_structure_unary_operator_valid(expression->as.unary.operator_kind)
                && sol_structure_expression(
                    validator,
                    expression->as.unary.operand,
                    context,
                    depth + 1
                );
            break;
        case SOL_EXPR_BINARY:
            valid = valid
                && sol_structure_binary_operator_valid(expression->as.binary.operator_kind)
                && sol_structure_expression(
                    validator,
                    expression->as.binary.left,
                    context,
                    depth + 1
                )
                && sol_structure_expression(
                    validator,
                    expression->as.binary.right,
                    context,
                    depth + 1
                );
            break;
        case SOL_EXPR_CALL:
            valid = valid
                && sol_structure_expression(
                    validator,
                    expression->as.call.callee,
                    context,
                    depth + 1
                )
                && sol_structure_arguments(
                    validator,
                    expression->as.call.first_argument,
                    context,
                    depth + 1,
                    true,
                    0,
                    SIZE_MAX
                );
            break;
        case SOL_EXPR_TYPE_APPLICATION:
            valid = valid
                && sol_structure_expression(
                    validator,
                    expression->as.type_application.base,
                    context,
                    depth + 1
                )
                && sol_structure_type_arguments(
                    validator,
                    expression->as.type_application.first_argument,
                    false,
                    1,
                    SIZE_MAX
                );
            break;
        case SOL_EXPR_FIELD:
            valid = valid
                && sol_structure_name_valid(validator, expression->as.field.name)
                && sol_structure_expression(
                    validator,
                    expression->as.field.base,
                    context,
                    depth + 1
                );
            break;
        case SOL_EXPR_TUPLE:
            valid = valid && sol_structure_arguments(
                validator,
                expression->as.tuple.first_element,
                context,
                depth + 1,
                false,
                2,
                16
            );
            break;
        case SOL_EXPR_RECORD:
            valid = valid
                && sol_structure_expression(
                    validator,
                    expression->as.record.type,
                    context,
                    depth + 1
                )
                && sol_structure_arguments(
                    validator,
                    expression->as.record.first_field,
                    context,
                    depth + 1,
                    true,
                    0,
                    SIZE_MAX
                );
            break;
        case SOL_EXPR_IF:
            valid = valid
                && sol_structure_expression(
                    validator,
                    expression->as.if_expr.condition,
                    context,
                    depth + 1
                )
                && sol_structure_expression(
                    validator,
                    expression->as.if_expr.then_branch,
                    context,
                    depth + 1
                )
                && sol_structure_expression(
                    validator,
                    expression->as.if_expr.else_branch,
                    context,
                    depth + 1
                );
            break;
        case SOL_EXPR_MATCH:
            valid = valid
                && sol_structure_expression(
                    validator,
                    expression->as.match_expr.scrutinee,
                    context,
                    depth + 1
                )
                && sol_structure_arms(
                    validator,
                    expression->as.match_expr.first_arm,
                    context,
                    depth + 1
                );
            break;
        case SOL_EXPR_BLOCK:
            valid = valid && sol_structure_statements(
                validator,
                expression->as.block.first_statement,
                context,
                depth + 1
            );
            break;
        case SOL_EXPR_PROPAGATE:
            valid = valid && sol_structure_expression(
                validator,
                expression->as.propagated,
                context,
                depth + 1
            );
            break;
        case SOL_EXPR_HANDLE:
            valid = valid
                && sol_structure_name_valid(validator, expression->as.handle.effect_name)
                && expression->as.handle.body < validator->tree->expression_count
                && validator->tree->expressions[expression->as.handle.body].kind
                    == SOL_EXPR_BLOCK
                && sol_structure_expression(
                    validator,
                    expression->as.handle.authority,
                    context,
                    depth + 1
                )
                && sol_structure_expression(
                    validator,
                    expression->as.handle.provider,
                    context,
                    depth + 1
                )
                && sol_structure_expression(
                    validator,
                    expression->as.handle.body,
                    context,
                    depth + 1
                );
            break;
        case SOL_EXPR_RESULT:
            valid = valid && context == SOL_EXPRESSION_CONTEXT_ENSURES;
            break;
        case SOL_EXPR_OLD:
            valid = valid && context == SOL_EXPRESSION_CONTEXT_ENSURES
                && sol_structure_expression(
                    validator,
                    expression->as.old_expression,
                    context,
                    depth + 1
                );
            break;
        default:
            break;
    }
    if (!valid) return false;
    validator->expressions[expression_id] = 2;
    return true;
}

static bool sol_structure_clause_list(
    SolStructureValidator *validator,
    SolContractClauseId clause,
    SolContractOwnerKind owner_kind,
    size_t owner
) {
    while (clause != SOL_AST_NONE) {
        if (clause >= validator->tree->contract_clause_count || validator->clauses[clause]) {
            return false;
        }
        validator->clauses[clause] = 1;
        const SolContractClause *current = &validator->tree->contract_clauses[clause];
        if ((int)current->kind < 0 || current->kind > SOL_CONTRACT_ENSURES
            || current->owner_kind != owner_kind || current->owner != owner
            || !sol_structure_span_valid(validator, current->span)) {
            return false;
        }
        SolExpressionContext context = current->kind == SOL_CONTRACT_REQUIRES
            ? SOL_EXPRESSION_CONTEXT_REQUIRES
            : SOL_EXPRESSION_CONTEXT_ENSURES;
        SolContractConditionId condition = current->first_condition;
        while (condition != SOL_AST_NONE) {
            if (condition >= validator->tree->contract_condition_count
                || validator->conditions[condition]) {
                return false;
            }
            validator->conditions[condition] = 1;
            const SolContractCondition *entry
                = &validator->tree->contract_conditions[condition];
            if ((int)entry->outcome < 0 || entry->outcome > SOL_CONTRACT_OUTCOME_FAILURE
                || entry->owner_clause != clause
                || (current->kind == SOL_CONTRACT_REQUIRES
                    && entry->outcome != SOL_CONTRACT_OUTCOME_ALWAYS)
                || !sol_structure_span_valid(validator, entry->span)
                || !sol_structure_expression(
                    validator,
                    entry->expression,
                    context,
                    0
                )) {
                return false;
            }
            condition = entry->next;
        }
        clause = current->next;
    }
    return true;
}

static bool sol_structure_type_declaration(
    SolStructureValidator *validator,
    const SolSyntaxItem *item,
    size_t owner
) {
    if (item->representation_type >= validator->tree->type_count
        || item->body != SOL_AST_NONE || item->first_parameter != SOL_AST_NONE
        || item->return_type_id != SOL_AST_NONE || item->first_field != SOL_AST_NONE
        || item->first_variant != SOL_AST_NONE || item->is_open
        || item->first_effect != SOL_AST_NONE || item->has_effect_clause
        || item->first_member != SOL_AST_NONE
        || item->result_authority_parameter != SOL_AST_NONE
        || item->capability_source != SOL_AST_NONE
        || item->first_effect_parameter != SOL_AST_NONE
        || item->implementation_type != SOL_AST_NONE
        || item->first_trait_method != SOL_AST_NONE) {
        return false;
    }
    if (item->flavor == SOL_TYPE_DECLARATION_DISTINCT) {
        return item->first_contract == SOL_AST_NONE;
    }
    if (item->flavor != SOL_TYPE_DECLARATION_REFINED
        || item->first_contract >= validator->tree->contract_clause_count) {
        return false;
    }
    const SolContractClause *clause
        = &validator->tree->contract_clauses[item->first_contract];
    if (clause->kind != SOL_CONTRACT_REQUIRES
        || clause->owner_kind != SOL_CONTRACT_OWNER_TYPE || clause->owner != owner
        || clause->next != SOL_AST_NONE
        || clause->first_condition >= validator->tree->contract_condition_count) {
        return false;
    }
    const SolContractCondition *condition
        = &validator->tree->contract_conditions[clause->first_condition];
    return condition->outcome == SOL_CONTRACT_OUTCOME_ALWAYS
        && condition->next == SOL_AST_NONE;
}

static bool sol_structure_all_marked(const unsigned char *items, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        if (items[index] == 0) return false;
    }
    return true;
}

static bool sol_structure_types_valid(SolStructureValidator *validator) {
    for (size_t index = 0; index < validator->tree->type_count; ++index) {
        const SolSyntaxType *type = &validator->tree->types[index];
        if ((int)type->kind < 0 || type->kind > SOL_SYNTAX_TYPE_TUPLE
            || !sol_structure_name_valid(validator, type->span)
            || type->owner_item >= validator->tree->item_count
            || (type->first_argument != SOL_AST_NONE
                && type->first_argument >= validator->tree->type_argument_count)) {
            return false;
        }
        if (type->kind == SOL_SYNTAX_TYPE_PATH) {
            if (!sol_structure_name_valid(validator, type->name)
                || type->return_type != SOL_AST_NONE
                || type->first_effect != SOL_AST_NONE || type->has_effect_tail
                || !sol_structure_type_arguments(validator, type->first_argument,
                    false, 0, SIZE_MAX)) return false;
        } else if (type->kind == SOL_SYNTAX_TYPE_UNIT) {
            if (type->first_argument != SOL_AST_NONE || type->return_type != SOL_AST_NONE
                || type->first_effect != SOL_AST_NONE || type->has_effect_tail
                || type->is_capability) return false;
        } else if (type->kind == SOL_SYNTAX_TYPE_TUPLE) {
            if (type->name.start != 0 || type->name.end != 0
                || type->return_type != SOL_AST_NONE || type->first_effect != SOL_AST_NONE
                || type->has_effect_tail || type->is_capability
                || !sol_structure_type_arguments(validator, type->first_argument,
                    false, 2, 16)) return false;
        } else if (type->return_type >= validator->tree->type_count
            || (type->has_effect_tail
                && !sol_structure_name_valid(validator, type->effect_tail))
            || type->is_capability
            || !sol_structure_type_arguments(validator, type->first_argument,
                true, 0, SIZE_MAX)
            || !sol_structure_effects(validator, type->first_effect,
                SOL_EFFECT_OWNER_TYPE, index)) {
            return false;
        }
    }
    for (size_t index = 0; index < validator->tree->type_argument_count; ++index) {
        const SolTypeArgument *argument = &validator->tree->type_arguments[index];
        if (argument->type >= validator->tree->type_count
            || (argument->next != SOL_AST_NONE
                && argument->next >= validator->tree->type_argument_count)
            || (int)argument->access < 0 || argument->access > SOL_ACCESS_EXCLUSIVE) {
            return false;
        }
    }
    return true;
}

static bool sol_structure_capability_members(
    SolStructureValidator *validator,
    SolCapabilityMemberId member,
    size_t owner
) {
    while (member != SOL_AST_NONE) {
        if (member >= validator->tree->capability_member_count
            || validator->capability_members[member]) return false;
        validator->capability_members[member] = 1;
        const SolCapabilityMember *current = &validator->tree->capability_members[member];
        if (current->owner_item != owner
            || !sol_structure_name_valid(validator, current->name)
            || !sol_structure_span_valid(validator, current->span)
            || !sol_structure_name_valid(validator, current->return_type)
            || current->return_type_id >= validator->tree->type_count
            || !sol_structure_parameters(validator, current->first_parameter)
            || !sol_structure_effects(validator, current->first_effect,
                SOL_EFFECT_OWNER_CAPABILITY_MEMBER, member)
            || (current->body != SOL_AST_NONE
                && !sol_structure_expression(validator, current->body,
                    SOL_EXPRESSION_CONTEXT_BODY, 0))
            || !sol_structure_clause_list(validator, current->first_contract,
                SOL_CONTRACT_OWNER_CAPABILITY_MEMBER, member)) return false;
        member = current->next;
    }
    return true;
}

static bool sol_structure_trait_methods(
    SolStructureValidator *validator,
    SolTraitMethodId method,
    size_t owner
) {
    while (method != SOL_AST_NONE) {
        if (method >= validator->tree->trait_method_count
            || validator->trait_methods[method]) return false;
        validator->trait_methods[method] = 1;
        const SolTraitMethod *current = &validator->tree->trait_methods[method];
        if (current->owner_item != owner
            || !sol_structure_name_valid(validator, current->name)
            || !sol_structure_span_valid(validator, current->span)
            || !sol_structure_name_valid(validator, current->return_type)
            || current->return_type_id >= validator->tree->type_count
            || !sol_structure_parameters(validator, current->first_parameter)
            || !sol_structure_effects(validator, current->first_effect,
                SOL_EFFECT_OWNER_TRAIT_METHOD, method)
            || (current->body != SOL_AST_NONE
                && !sol_structure_expression(validator, current->body,
                    SOL_EXPRESSION_CONTEXT_BODY, 0))) return false;
        method = current->next;
    }
    return true;
}

bool sol_syntax_contracts_validate(const SolSource *source, const SolSyntaxTree *tree) {
    if (source == NULL || source->text == NULL || tree == NULL
        || tree->import_count > tree->import_capacity
        || tree->item_count > tree->item_capacity
        || tree->capability_member_count > tree->capability_member_capacity
        || tree->trait_method_count > tree->trait_method_capacity
        || tree->expression_count > tree->expression_capacity
        || tree->argument_count > tree->argument_capacity
        || tree->parameter_count > tree->parameter_capacity
        || tree->statement_count > tree->statement_capacity
        || tree->loop_invariant_count > tree->loop_invariant_capacity
        || tree->match_arm_count > tree->match_arm_capacity
        || tree->pattern_count > tree->pattern_capacity
        || tree->pattern_binding_count > tree->pattern_binding_capacity
        || tree->type_count > tree->type_capacity
        || tree->type_argument_count > tree->type_argument_capacity
        || tree->type_parameter_count > tree->type_parameter_capacity
        || tree->effect_parameter_count > tree->effect_parameter_capacity
        || tree->field_count > tree->field_capacity
        || tree->variant_count > tree->variant_capacity
        || tree->effect_count > tree->effect_capacity
        || tree->contract_clause_count > tree->contract_clause_capacity
        || tree->contract_condition_count > tree->contract_condition_capacity
        || (tree->import_count != 0 && tree->imports == NULL)
        || (tree->item_count != 0 && tree->items == NULL)
        || (tree->capability_member_count != 0 && tree->capability_members == NULL)
        || (tree->trait_method_count != 0 && tree->trait_methods == NULL)
        || (tree->expression_count != 0 && tree->expressions == NULL)
        || (tree->argument_count != 0 && tree->arguments == NULL)
        || (tree->parameter_count != 0 && tree->parameters == NULL)
        || (tree->statement_count != 0 && tree->statements == NULL)
        || (tree->loop_invariant_count != 0 && tree->loop_invariants == NULL)
        || (tree->match_arm_count != 0 && tree->match_arms == NULL)
        || (tree->pattern_count != 0 && tree->patterns == NULL)
        || (tree->pattern_binding_count != 0 && tree->pattern_bindings == NULL)
        || (tree->type_count != 0 && tree->types == NULL)
        || (tree->type_argument_count != 0 && tree->type_arguments == NULL)
        || (tree->type_parameter_count != 0 && tree->type_parameters == NULL)
        || (tree->effect_parameter_count != 0 && tree->effect_parameters == NULL)
        || (tree->field_count != 0 && tree->fields == NULL)
        || (tree->variant_count != 0 && tree->variants == NULL)
        || (tree->effect_count != 0 && tree->effects == NULL)
        || (tree->contract_clause_count != 0 && tree->contract_clauses == NULL)
        || (tree->contract_condition_count != 0 && tree->contract_conditions == NULL)) {
        return false;
    }
    SolStructureValidator validator = {.source = source, .tree = tree};
    if (!sol_structure_name_valid(&validator, tree->module_name)) return false;
    validator.expressions = calloc(tree->expression_count, 1);
    validator.arguments = calloc(tree->argument_count, 1);
    validator.statements = calloc(tree->statement_count, 1);
    validator.loop_invariants = calloc(tree->loop_invariant_count, 1);
    validator.arms = calloc(tree->match_arm_count, 1);
    validator.patterns = calloc(tree->pattern_count, 1);
    validator.pattern_bindings = calloc(tree->pattern_binding_count, 1);
    validator.parameters = calloc(tree->parameter_count, 1);
    validator.type_arguments = calloc(tree->type_argument_count, 1);
    validator.type_parameters = calloc(tree->type_parameter_count, 1);
    validator.effect_parameters = calloc(tree->effect_parameter_count, 1);
    validator.fields = calloc(tree->field_count, 1);
    validator.variants = calloc(tree->variant_count, 1);
    validator.effects = calloc(tree->effect_count, 1);
    validator.capability_members = calloc(tree->capability_member_count, 1);
    validator.trait_methods = calloc(tree->trait_method_count, 1);
    validator.clauses = calloc(tree->contract_clause_count, 1);
    validator.conditions = calloc(tree->contract_condition_count, 1);
    bool allocated = (tree->expression_count == 0 || validator.expressions != NULL)
        && (tree->argument_count == 0 || validator.arguments != NULL)
        && (tree->statement_count == 0 || validator.statements != NULL)
        && (tree->loop_invariant_count == 0 || validator.loop_invariants != NULL)
        && (tree->match_arm_count == 0 || validator.arms != NULL)
        && (tree->pattern_count == 0 || validator.patterns != NULL)
        && (tree->pattern_binding_count == 0 || validator.pattern_bindings != NULL)
        && (tree->parameter_count == 0 || validator.parameters != NULL)
        && (tree->type_argument_count == 0 || validator.type_arguments != NULL)
        && (tree->type_parameter_count == 0 || validator.type_parameters != NULL)
        && (tree->effect_parameter_count == 0 || validator.effect_parameters != NULL)
        && (tree->field_count == 0 || validator.fields != NULL)
        && (tree->variant_count == 0 || validator.variants != NULL)
        && (tree->effect_count == 0 || validator.effects != NULL)
        && (tree->capability_member_count == 0 || validator.capability_members != NULL)
        && (tree->trait_method_count == 0 || validator.trait_methods != NULL)
        && (tree->contract_clause_count == 0 || validator.clauses != NULL)
        && (tree->contract_condition_count == 0 || validator.conditions != NULL);
    bool valid = allocated && sol_structure_types_valid(&validator);
    for (size_t index = 0; valid && index < tree->import_count; ++index) {
        valid = sol_structure_span_valid(&validator, tree->imports[index].path);
    }
    for (size_t index = 0; valid && index < tree->item_count; ++index) {
        const SolSyntaxItem *item = &tree->items[index];
        if ((int)item->kind < 0 || item->kind > SOL_ITEM_TEST
            || (int)item->flavor < 0 || item->flavor > SOL_TYPE_DECLARATION_REFINED
            || !sol_structure_name_valid(&validator, item->name)
            || !sol_structure_span_valid(&validator, item->span)
            || !sol_structure_span_valid(&validator, item->stable_identity)
            || (item->return_type_id != SOL_AST_NONE
                && (item->return_type_id >= tree->type_count
                    || !sol_structure_name_valid(&validator, item->return_type)))
            || (item->return_type_id == SOL_AST_NONE
                && (item->return_type.start != 0 || item->return_type.end != 0))
            || (item->representation_type != SOL_AST_NONE
                && item->representation_type >= tree->type_count)
            || (item->implementation_type != SOL_AST_NONE
                && item->implementation_type >= tree->type_count)
            || !sol_structure_parameters(&validator, item->first_parameter)
            || !sol_structure_type_parameters(&validator,
                item->first_type_parameter, index)
            || !sol_structure_effect_parameters(&validator,
                item->first_effect_parameter, index)
            || !sol_structure_effects(&validator, item->first_effect,
                SOL_EFFECT_OWNER_ITEM, index)
            || !sol_structure_fields(&validator, item->first_field)
            || !sol_structure_variants(&validator, item->first_variant, index)
            || !sol_structure_capability_members(&validator, item->first_member, index)
            || !sol_structure_trait_methods(&validator, item->first_trait_method, index)
            || (item->capability_source != SOL_AST_NONE
                && !sol_structure_parameters(&validator, item->capability_source))
            || (item->result_authority_parameter != SOL_AST_NONE
                && (item->result_authority_parameter >= tree->parameter_count
                    || !sol_structure_parameter_contains(&validator,
                        item->first_parameter, item->result_authority_parameter)))
            || (item->kind == SOL_ITEM_IMPLEMENTATION
                ? !sol_structure_name_valid(&validator, item->trait_name)
                : item->trait_name.start != 0 || item->trait_name.end != 0)
            || (item->kind == SOL_ITEM_TYPE
                && !sol_structure_type_declaration(&validator, item, index))
            || (item->kind != SOL_ITEM_TYPE
                && (item->flavor != SOL_TYPE_DECLARATION_NONE
                    || item->representation_type != SOL_AST_NONE))
            || (item->kind != SOL_ITEM_FUNCTION && item->kind != SOL_ITEM_TYPE
                && item->kind != SOL_ITEM_TEST
                && item->first_contract != SOL_AST_NONE)
            || (item->kind == SOL_ITEM_TEST
                && (item->name.end - item->name.start < 2 || item->is_public
                    || item->stable_identity.start != item->stable_identity.end
                    || item->body == SOL_AST_NONE
                    || item->first_parameter != SOL_AST_NONE
                    || item->return_type.start != 0 || item->return_type.end != 0
                    || item->return_type_id != SOL_AST_NONE
                    || item->first_field != SOL_AST_NONE
                    || item->first_variant != SOL_AST_NONE || item->is_open
                    || item->first_effect != SOL_AST_NONE || item->has_effect_clause
                    || item->first_contract != SOL_AST_NONE
                    || item->first_member != SOL_AST_NONE
                    || item->result_authority_parameter != SOL_AST_NONE
                    || item->capability_source != SOL_AST_NONE
                    || item->first_type_parameter != SOL_AST_NONE
                    || item->first_effect_parameter != SOL_AST_NONE
                    || item->trait_name.start != 0 || item->trait_name.end != 0
                    || item->implementation_type != SOL_AST_NONE
                    || item->first_trait_method != SOL_AST_NONE))
            || (item->body != SOL_AST_NONE
                && !sol_structure_expression(
                    &validator,
                    item->body,
                    SOL_EXPRESSION_CONTEXT_BODY,
                    0
                ))
            || !sol_structure_clause_list(
                &validator,
                item->first_contract,
                item->kind == SOL_ITEM_TYPE
                    ? SOL_CONTRACT_OWNER_TYPE
                    : SOL_CONTRACT_OWNER_ITEM,
                index
            )) {
            valid = false;
        }
    }
    valid = valid
        && sol_structure_all_marked(validator.expressions, tree->expression_count)
        && sol_structure_all_marked(validator.arguments, tree->argument_count)
        && sol_structure_all_marked(validator.statements, tree->statement_count)
        && sol_structure_all_marked(
            validator.loop_invariants, tree->loop_invariant_count)
        && sol_structure_all_marked(validator.arms, tree->match_arm_count)
        && sol_structure_all_marked(validator.patterns, tree->pattern_count)
        && sol_structure_all_marked(
            validator.pattern_bindings,
            tree->pattern_binding_count
        )
        && sol_structure_all_marked(validator.parameters, tree->parameter_count)
        && sol_structure_all_marked(validator.type_arguments, tree->type_argument_count)
        && sol_structure_all_marked(validator.type_parameters, tree->type_parameter_count)
        && sol_structure_all_marked(
            validator.effect_parameters, tree->effect_parameter_count)
        && sol_structure_all_marked(validator.fields, tree->field_count)
        && sol_structure_all_marked(validator.variants, tree->variant_count)
        && sol_structure_all_marked(validator.effects, tree->effect_count)
        && sol_structure_all_marked(
            validator.capability_members, tree->capability_member_count)
        && sol_structure_all_marked(validator.trait_methods, tree->trait_method_count)
        && sol_structure_all_marked(validator.clauses, tree->contract_clause_count)
        && sol_structure_all_marked(validator.conditions, tree->contract_condition_count);
    free(validator.expressions);
    free(validator.arguments);
    free(validator.statements);
    free(validator.loop_invariants);
    free(validator.arms);
    free(validator.patterns);
    free(validator.pattern_bindings);
    free(validator.parameters);
    free(validator.type_arguments);
    free(validator.type_parameters);
    free(validator.effect_parameters);
    free(validator.fields);
    free(validator.variants);
    free(validator.effects);
    free(validator.capability_members);
    free(validator.trait_methods);
    free(validator.clauses);
    free(validator.conditions);
    return valid;
}
