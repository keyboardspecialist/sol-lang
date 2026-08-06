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
    unsigned char *declared_states;
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
        || syntax->type_count > syntax->type_capacity
        || syntax->type_argument_count > syntax->type_argument_capacity
        || syntax->field_count > syntax->field_capacity
        || syntax->variant_count > syntax->variant_capacity
        || syntax->pattern_count > syntax->pattern_capacity
        || syntax->pattern_binding_count > syntax->pattern_binding_capacity
        || syntax->match_arm_count > syntax->match_arm_capacity
        || syntax->effect_count > syntax->effect_capacity
        || syntax->capability_member_count > syntax->capability_member_capacity
        || (syntax->item_count != 0 && syntax->items == NULL)
        || (syntax->parameter_count != 0 && syntax->parameters == NULL)
        || (syntax->argument_count != 0 && syntax->arguments == NULL)
        || (syntax->statement_count != 0 && syntax->statements == NULL)
        || (syntax->expression_count != 0 && syntax->expressions == NULL)
        || (syntax->type_count != 0 && syntax->types == NULL)
        || (syntax->type_argument_count != 0 && syntax->type_arguments == NULL)
        || (syntax->field_count != 0 && syntax->fields == NULL)
        || (syntax->variant_count != 0 && syntax->variants == NULL)
        || (syntax->pattern_count != 0 && syntax->patterns == NULL)
        || (syntax->pattern_binding_count != 0 && syntax->pattern_bindings == NULL)
        || (syntax->match_arm_count != 0 && syntax->match_arms == NULL)
        || (syntax->effect_count != 0 && syntax->effects == NULL)
        || (syntax->capability_member_count != 0
            && syntax->capability_members == NULL)
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
            || (item->return_type_id != SOL_AST_NONE
                && item->return_type_id >= syntax->type_count)
            || (item->first_field != SOL_AST_NONE && item->first_field >= syntax->field_count)
            || (item->first_variant != SOL_AST_NONE
                && item->first_variant >= syntax->variant_count)
            || (item->first_effect != SOL_AST_NONE
                && item->first_effect >= syntax->effect_count)
            || (item->first_member != SOL_AST_NONE
                && item->first_member >= syntax->capability_member_count)
            || definition->syntax_item != index
            || definition->kind != item->kind
            || !sol_type_span_valid(checker->source, definition->name)) {
            sol_type_malformed(checker);
            return false;
        }
        SolCapabilityMemberId member_id = item->first_member;
        size_t member_count = 0;
        while (member_id != SOL_AST_NONE) {
            if (member_id >= syntax->capability_member_count
                || member_count++ >= syntax->capability_member_count
                || syntax->capability_members[member_id].owner_item != index) {
                sol_type_malformed(checker);
                return false;
            }
            member_id = syntax->capability_members[member_id].next;
        }
    }
    for (size_t index = 0; index < syntax->parameter_count; ++index) {
        const SolParameter *parameter = &syntax->parameters[index];
        if (!sol_type_span_valid(checker->source, parameter->name)
            || !sol_type_span_valid(checker->source, parameter->type)
            || parameter->type_id >= syntax->type_count
            || (parameter->next != SOL_AST_NONE && parameter->next >= syntax->parameter_count)) {
            sol_type_malformed(checker);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->type_count; ++index) {
        const SolSyntaxType *type = &syntax->types[index];
        if ((int)type->kind < 0 || type->kind > SOL_SYNTAX_TYPE_UNIT
            || !sol_type_span_valid(checker->source, type->span)
            || !sol_type_span_valid(checker->source, type->name)
            || (type->first_argument != SOL_AST_NONE
                && type->first_argument >= syntax->type_argument_count)) {
            sol_type_malformed(checker);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->type_argument_count; ++index) {
        const SolTypeArgument *argument = &syntax->type_arguments[index];
        if (argument->type >= syntax->type_count
            || (argument->next != SOL_AST_NONE
                && argument->next >= syntax->type_argument_count)) {
            sol_type_malformed(checker);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->field_count; ++index) {
        const SolField *field = &syntax->fields[index];
        if (!sol_type_span_valid(checker->source, field->name)
            || !sol_type_span_valid(checker->source, field->span)
            || field->type >= syntax->type_count
            || (field->next != SOL_AST_NONE && field->next >= syntax->field_count)) {
            sol_type_malformed(checker);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->variant_count; ++index) {
        const SolVariant *variant = &syntax->variants[index];
        if (!sol_type_span_valid(checker->source, variant->name)
            || !sol_type_span_valid(checker->source, variant->span)
            || (variant->first_field != SOL_AST_NONE
                && variant->first_field >= syntax->field_count)
            || (variant->next != SOL_AST_NONE && variant->next >= syntax->variant_count)
            || variant->owner_item >= syntax->item_count
            || syntax->items[variant->owner_item].kind != SOL_ITEM_ENUM) {
            sol_type_malformed(checker);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->pattern_count; ++index) {
        const SolPattern *pattern = &syntax->patterns[index];
        if ((int)pattern->kind < 0 || pattern->kind > SOL_PATTERN_BOOL
            || !sol_type_span_valid(checker->source, pattern->span)
            || !sol_type_span_valid(checker->source, pattern->name)
            || (pattern->first_binding != SOL_AST_NONE
                && pattern->first_binding >= syntax->pattern_binding_count)) {
            sol_type_malformed(checker);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->pattern_binding_count; ++index) {
        const SolPatternBinding *binding = &syntax->pattern_bindings[index];
        if (!sol_type_span_valid(checker->source, binding->name)
            || (binding->next != SOL_AST_NONE
                && binding->next >= syntax->pattern_binding_count)) {
            sol_type_malformed(checker);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->match_arm_count; ++index) {
        const SolMatchArm *arm = &syntax->match_arms[index];
        if (arm->pattern >= syntax->pattern_count
            || arm->value >= syntax->expression_count
            || !sol_type_span_valid(checker->source, arm->span)
            || (arm->next != SOL_AST_NONE && arm->next >= syntax->match_arm_count)) {
            sol_type_malformed(checker);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->effect_count; ++index) {
        const SolEffect *effect = &syntax->effects[index];
        if (!sol_type_span_valid(checker->source, effect->name)
            || !sol_type_span_valid(checker->source, effect->argument)
            || !sol_type_span_valid(checker->source, effect->span)
            || (effect->next != SOL_AST_NONE && effect->next >= syntax->effect_count)
            || effect->owner_item >= syntax->item_count) {
            sol_type_malformed(checker);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->capability_member_count; ++index) {
        const SolCapabilityMember *member = &syntax->capability_members[index];
        if (!sol_type_span_valid(checker->source, member->name)
            || !sol_type_span_valid(checker->source, member->span)
            || !sol_type_span_valid(checker->source, member->return_type)
            || (member->first_parameter != SOL_AST_NONE
                && member->first_parameter >= syntax->parameter_count)
            || member->return_type_id >= syntax->type_count
            || (member->first_effect != SOL_AST_NONE
                && member->first_effect >= syntax->effect_count)
            || (member->next != SOL_AST_NONE
                && member->next >= syntax->capability_member_count)
            || member->owner_item >= syntax->item_count
            || syntax->items[member->owner_item].kind != SOL_ITEM_CAPABILITY) {
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
            case SOL_EXPR_MATCH:
                valid = valid
                    && expression->as.match_expr.scrutinee < syntax->expression_count
                    && (expression->as.match_expr.first_arm == SOL_AST_NONE
                        || expression->as.match_expr.first_arm < syntax->match_arm_count);
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
        if ((int)local->kind < 0 || local->kind > SOL_LOCAL_PATTERN
            || local->owner >= hir->definition_count
            || (local->kind == SOL_LOCAL_PARAMETER
                && local->syntax_id >= syntax->parameter_count)
            || (local->kind == SOL_LOCAL_BINDING
                && local->syntax_id >= syntax->statement_count)
            || (local->kind == SOL_LOCAL_PATTERN
                && local->syntax_id >= syntax->pattern_binding_count)) {
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
    free(table->expression_operation_origins);
    free(table->locals);
    free(table->local_capability_origins);
    free(table->local_operation_origins);
    free(table->definitions);
    free(table->declared_types);
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

static bool sol_type_equal(SolType left, SolType right) {
    if (left.kind == SOL_TYPE_UNKNOWN || right.kind == SOL_TYPE_UNKNOWN
        || left.kind == SOL_TYPE_ERROR || right.kind == SOL_TYPE_ERROR
        || left.kind == SOL_TYPE_NEVER || right.kind == SOL_TYPE_NEVER) {
        return true;
    }
    return left.kind == right.kind
        && ((left.kind != SOL_TYPE_NOMINAL
                && left.kind != SOL_TYPE_OPAQUE
                && left.kind != SOL_TYPE_FUNCTION
                && left.kind != SOL_TYPE_CAPABILITY_OPERATION
                && left.kind != SOL_TYPE_VARIANT)
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
        case SOL_TYPE_CAPABILITY_OPERATION: return "capability operation";
        case SOL_TYPE_VARIANT: return "enum constructor";
        case SOL_TYPE_NEVER: return "Never";
        case SOL_TYPE_ERROR: return "error";
        default: return "unknown type";
    }
}

static SolType sol_type_from_id(SolTypeChecker *checker, SolTypeId type_id) {
    if (type_id >= checker->syntax->type_count) {
        sol_type_malformed(checker);
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    if (checker->declared_states[type_id] == 2) {
        return checker->types->declared_types[type_id];
    }
    if (checker->declared_states[type_id] == 1) {
        sol_type_malformed(checker);
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    checker->declared_states[type_id] = 1;
    const SolSyntaxType *syntax_type = &checker->syntax->types[type_id];
    SolType type = {.kind = SOL_TYPE_ERROR};
    if (syntax_type->kind == SOL_SYNTAX_TYPE_UNIT) {
        type = (SolType){.kind = SOL_TYPE_UNIT};
    } else if (syntax_type->first_argument != SOL_AST_NONE) {
        bool result_type = sol_type_span_equal(checker->source, syntax_type->name, "Result");
        bool option_type = sol_type_span_equal(checker->source, syntax_type->name, "Option");
        size_t expected_count = result_type ? 2 : option_type ? 1 : 0;
        size_t count = 0;
        size_t hash = result_type ? (size_t)0x52534c54 : (size_t)0x4f50544e;
        bool valid = expected_count != 0;
        SolTypeArgumentId argument_id = syntax_type->first_argument;
        while (argument_id != SOL_AST_NONE) {
            if (count++ >= checker->syntax->type_argument_count) {
                sol_type_malformed(checker);
                valid = false;
                break;
            }
            const SolTypeArgument *argument = &checker->syntax->type_arguments[argument_id];
            SolType argument_type = sol_type_from_id(checker, argument->type);
            valid = valid && argument_type.kind != SOL_TYPE_ERROR;
            hash ^= (size_t)argument_type.kind + (argument_type.definition << 4);
            hash *= (size_t)1099511628211ULL;
            argument_id = argument->next;
        }
        if (!valid || count != expected_count) {
            sol_type_error(
                checker,
                "SOL-TYPE-009",
                syntax_type->span,
                "unsupported generic type or incorrect generic arity"
            );
        } else {
            type = (SolType){.kind = SOL_TYPE_OPAQUE, .definition = hash};
        }
    } else if (!syntax_type->is_capability
        && sol_type_span_equal(checker->source, syntax_type->name, "Int64")) {
        type = (SolType){.kind = SOL_TYPE_INT64};
    } else if (!syntax_type->is_capability
        && sol_type_span_equal(checker->source, syntax_type->name, "Bool")) {
        type = (SolType){.kind = SOL_TYPE_BOOL};
    } else if (!syntax_type->is_capability
        && sol_type_span_equal(checker->source, syntax_type->name, "Text")) {
        type = (SolType){.kind = SOL_TYPE_TEXT};
    } else {
        for (size_t index = 0; index < checker->hir->definition_count; ++index) {
            const SolHirDefinition *definition = &checker->hir->definitions[index];
            bool capability_matches = syntax_type->is_capability
                == (definition->kind == SOL_ITEM_CAPABILITY);
            if (definition->kind != SOL_ITEM_FUNCTION
                && capability_matches
                && sol_type_name_equal(
                    checker->source,
                    definition->name,
                    syntax_type->name
                )) {
                type = (SolType){.kind = SOL_TYPE_NOMINAL, .definition = index};
                break;
            }
        }
        if (type.kind == SOL_TYPE_ERROR) {
            sol_type_error(
                checker,
                "SOL-TYPE-009",
                syntax_type->span,
                "unresolved declared type"
            );
        }
    }
    checker->declared_states[type_id] = 2;
    checker->types->declared_types[type_id] = type;
    return type;
}

static bool sol_type_allocate(SolTypeChecker *checker) {
    size_t expression_count = checker->syntax->expression_count;
    size_t local_count = checker->hir->local_count;
    size_t definition_count = checker->hir->definition_count;
    if (expression_count > SIZE_MAX / sizeof(*checker->types->expressions)
        || local_count > SIZE_MAX / sizeof(*checker->types->locals)
        || definition_count > SIZE_MAX / sizeof(*checker->types->definitions)
        || checker->syntax->type_count > SIZE_MAX / sizeof(*checker->types->declared_types)) {
        return false;
    }
    checker->types->expressions = calloc(expression_count, sizeof(*checker->types->expressions));
    checker->types->expression_operation_origins = malloc(
        expression_count * sizeof(*checker->types->expression_operation_origins)
    );
    checker->types->locals = calloc(local_count, sizeof(*checker->types->locals));
    checker->types->local_capability_origins = malloc(
        local_count * sizeof(*checker->types->local_capability_origins)
    );
    checker->types->local_operation_origins = malloc(
        local_count * sizeof(*checker->types->local_operation_origins)
    );
    checker->types->definitions = calloc(
        definition_count,
        sizeof(*checker->types->definitions)
    );
    checker->types->declared_types = calloc(
        checker->syntax->type_count,
        sizeof(*checker->types->declared_types)
    );
    checker->states = calloc(expression_count, sizeof(*checker->states));
    checker->declared_states = calloc(
        checker->syntax->type_count,
        sizeof(*checker->declared_states)
    );
    if ((expression_count != 0
            && (checker->types->expressions == NULL
                || checker->types->expression_operation_origins == NULL
                || checker->states == NULL))
        || (local_count != 0
            && (checker->types->locals == NULL
                || checker->types->local_capability_origins == NULL
                || checker->types->local_operation_origins == NULL))
        || (definition_count != 0 && checker->types->definitions == NULL)) {
        return false;
    }
    if (checker->syntax->type_count != 0
        && (checker->types->declared_types == NULL || checker->declared_states == NULL)) {
        return false;
    }
    checker->types->expression_count = expression_count;
    checker->types->local_count = local_count;
    checker->types->definition_count = definition_count;
    checker->types->declared_type_count = checker->syntax->type_count;
    for (size_t index = 0; index < expression_count; ++index) {
        checker->types->expression_operation_origins[index] = SOL_AST_NONE;
    }
    for (size_t index = 0; index < local_count; ++index) {
        checker->types->local_capability_origins[index] = SOL_AST_NONE;
        checker->types->local_operation_origins[index] = SOL_AST_NONE;
    }
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
static SolType sol_type_variant_call(
    SolTypeChecker *checker,
    SolVariantId variant,
    const SolExpr *call
);

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

static SolParameterId sol_type_capability_origin(
    SolTypeChecker *checker,
    SolExprId expression_id
) {
    if (expression_id >= checker->syntax->expression_count) {
        sol_type_malformed(checker);
        return SOL_AST_NONE;
    }
    const SolExpr *expression = &checker->syntax->expressions[expression_id];
    SolResolution resolution = checker->hir->resolutions[expression_id];
    if (expression->kind != SOL_EXPR_PATH
        || resolution.kind != SOL_RESOLUTION_LOCAL
        || resolution.target >= checker->hir->local_count) {
        return SOL_AST_NONE;
    }
    const SolHirLocal *local = &checker->hir->locals[resolution.target];
    if (local->owner != checker->current_definition
        || resolution.target >= checker->types->local_count) {
        return SOL_AST_NONE;
    }
    return checker->types->local_capability_origins[resolution.target];
}

static SolParameterId sol_type_operation_origin(
    SolTypeChecker *checker,
    SolExprId expression_id,
    SolType type
) {
    if (type.kind != SOL_TYPE_CAPABILITY_OPERATION) return SOL_AST_NONE;
    const SolExpr *expression = &checker->syntax->expressions[expression_id];
    if (expression->kind == SOL_EXPR_FIELD) {
        return sol_type_capability_origin(checker, expression->as.field.base);
    }
    if (expression->kind == SOL_EXPR_PATH) {
        SolResolution resolution = checker->hir->resolutions[expression_id];
        if (resolution.kind == SOL_RESOLUTION_LOCAL
            && resolution.target < checker->types->local_count
            && checker->hir->locals[resolution.target].owner == checker->current_definition) {
            return checker->types->local_operation_origins[resolution.target];
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
                checker->types->local_capability_origins[local]
                    = sol_type_capability_origin(
                        checker,
                        statement->as.let_statement.value
                    );
                checker->types->local_operation_origins[local]
                    = checker->types->expression_operation_origins[
                        statement->as.let_statement.value
                    ];
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
    SolCapabilityMemberId operation = SOL_AST_NONE;
    bool invalid_operation_origin = false;
    if (resolution.kind == SOL_RESOLUTION_DEFINITION
        && resolution.target < checker->syntax->item_count
        && checker->syntax->items[resolution.target].kind == SOL_ITEM_FUNCTION) {
        target = resolution.target;
    } else if (callee_type.kind == SOL_TYPE_FUNCTION
        && callee_type.definition < checker->syntax->item_count) {
        target = callee_type.definition;
    } else if (callee_type.kind == SOL_TYPE_CAPABILITY_OPERATION
        && callee_type.definition < checker->syntax->capability_member_count) {
        operation = callee_type.definition;
    }
    if (callee_type.kind == SOL_TYPE_VARIANT) {
        return sol_type_variant_call(checker, callee_type.definition, call);
    }
    if (operation != SOL_AST_NONE) {
        const SolExpr *callee = &checker->syntax->expressions[call->as.call.callee];
        bool known_authority = checker->types->expression_operation_origins[
            call->as.call.callee
        ] != SOL_AST_NONE;
        if (!known_authority) {
            sol_type_error(
                checker,
                "SOL-TYPE-015",
                callee->span,
                "capability operation receiver has no known parameter authority"
            );
            invalid_operation_origin = true;
        }
    }
    if (target == SOL_AST_NONE && operation == SOL_AST_NONE) {
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

    SolParameterId first_parameter;
    SolType result;
    if (operation != SOL_AST_NONE) {
        const SolCapabilityMember *member = &checker->syntax->capability_members[operation];
        first_parameter = member->first_parameter;
        result = sol_type_from_id(checker, member->return_type_id);
    } else {
        first_parameter = checker->syntax->items[target].first_parameter;
        result = checker->types->definitions[target];
    }
    size_t parameter_count = 0;
    SolParameterId parameter_id = first_parameter;
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
    parameter_id = first_parameter;
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
            SolType expected = sol_type_from_id(checker, parameter->type_id);
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
    return invalid_operation_origin ? (SolType){.kind = SOL_TYPE_ERROR} : result;
}

static SolFieldId sol_type_find_field(
    SolTypeChecker *checker,
    SolDefId definition,
    SolSpan name
) {
    if (definition >= checker->syntax->item_count
        || checker->syntax->items[definition].kind != SOL_ITEM_RECORD) {
        return SOL_AST_NONE;
    }
    SolFieldId field_id = checker->syntax->items[definition].first_field;
    size_t traversed = 0;
    while (field_id != SOL_AST_NONE) {
        if (traversed++ >= checker->syntax->field_count) {
            sol_type_malformed(checker);
            return SOL_AST_NONE;
        }
        const SolField *field = &checker->syntax->fields[field_id];
        if (sol_type_name_equal(checker->source, field->name, name)) {
            return field_id;
        }
        field_id = field->next;
    }
    return SOL_AST_NONE;
}

static SolVariantId sol_type_find_variant(
    SolTypeChecker *checker,
    SolDefId definition,
    SolSpan name
) {
    if (definition >= checker->syntax->item_count
        || checker->syntax->items[definition].kind != SOL_ITEM_ENUM) {
        return SOL_AST_NONE;
    }
    SolVariantId variant_id = checker->syntax->items[definition].first_variant;
    size_t traversed = 0;
    while (variant_id != SOL_AST_NONE) {
        if (traversed++ >= checker->syntax->variant_count) {
            sol_type_malformed(checker);
            return SOL_AST_NONE;
        }
        const SolVariant *variant = &checker->syntax->variants[variant_id];
        if (sol_type_name_equal(checker->source, variant->name, name)) return variant_id;
        variant_id = variant->next;
    }
    return SOL_AST_NONE;
}

static SolCapabilityMemberId sol_type_find_capability_member(
    SolTypeChecker *checker,
    SolDefId definition,
    SolSpan name
) {
    if (definition >= checker->syntax->item_count
        || checker->syntax->items[definition].kind != SOL_ITEM_CAPABILITY) {
        return SOL_AST_NONE;
    }
    SolCapabilityMemberId member_id = checker->syntax->items[definition].first_member;
    size_t traversed = 0;
    while (member_id != SOL_AST_NONE) {
        if (traversed++ >= checker->syntax->capability_member_count) {
            sol_type_malformed(checker);
            return SOL_AST_NONE;
        }
        const SolCapabilityMember *member = &checker->syntax->capability_members[member_id];
        if (member->owner_item != definition) {
            sol_type_malformed(checker);
            return SOL_AST_NONE;
        }
        if (sol_type_name_equal(checker->source, member->name, name)) return member_id;
        member_id = member->next;
    }
    return SOL_AST_NONE;
}

static SolDefId sol_type_variant_owner(SolTypeChecker *checker, SolVariantId variant_id) {
    if (variant_id >= checker->syntax->variant_count) return SOL_AST_NONE;
    return checker->syntax->variants[variant_id].owner_item;
}

static SolType sol_type_variant_call(
    SolTypeChecker *checker,
    SolVariantId variant_id,
    const SolExpr *call
) {
    if (variant_id >= checker->syntax->variant_count) {
        sol_type_malformed(checker);
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    SolDefId owner = sol_type_variant_owner(checker, variant_id);
    if (owner == SOL_AST_NONE) return (SolType){.kind = SOL_TYPE_ERROR};
    const SolVariant *variant = &checker->syntax->variants[variant_id];
    size_t field_count = 0;
    SolFieldId field_id = variant->first_field;
    while (field_id != SOL_AST_NONE) {
        if (field_count++ >= checker->syntax->field_count) {
            sol_type_malformed(checker);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        field_id = checker->syntax->fields[field_id].next;
    }
    if (field_count > SIZE_MAX / sizeof(SolFieldId)) {
        checker->allocation_failed = true;
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    SolFieldId *field_ids = field_count == 0
        ? NULL
        : malloc(field_count * sizeof(*field_ids));
    bool *used = field_count == 0 ? NULL : calloc(field_count, sizeof(*used));
    if (field_count != 0 && (field_ids == NULL || used == NULL)) {
        free(field_ids);
        free(used);
        checker->allocation_failed = true;
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    field_id = variant->first_field;
    for (size_t index = 0; index < field_count; ++index) {
        field_ids[index] = field_id;
        field_id = checker->syntax->fields[field_id].next;
    }

    SolArgumentId argument_id = call->as.call.first_argument;
    size_t argument_count = 0;
    size_t positional = 0;
    bool seen_named = false;
    while (argument_id != SOL_AST_NONE) {
        if (argument_count++ >= checker->syntax->argument_count) {
            sol_type_malformed(checker);
            break;
        }
        const SolArgument *argument = &checker->syntax->arguments[argument_id];
        size_t matched = SIZE_MAX;
        if (argument->is_named) {
            seen_named = true;
            for (size_t index = 0; index < field_count; ++index) {
                if (sol_type_name_equal(
                    checker->source,
                    checker->syntax->fields[field_ids[index]].name,
                    argument->name
                )) {
                    matched = index;
                    break;
                }
            }
        } else if (seen_named) {
            sol_type_error(
                checker,
                "SOL-TYPE-014",
                checker->syntax->expressions[argument->value].span,
                "positional payload arguments cannot follow named arguments"
            );
        } else if (positional < field_count) {
            matched = positional++;
        }
        SolType actual = sol_type_expression(checker, argument->value);
        if (matched == SIZE_MAX) {
            sol_type_error(
                checker,
                "SOL-TYPE-014",
                argument->is_named
                    ? argument->name
                    : checker->syntax->expressions[argument->value].span,
                "enum constructor has an unknown payload argument"
            );
        } else if (used[matched]) {
            sol_type_error(
                checker,
                "SOL-TYPE-014",
                argument->is_named
                    ? argument->name
                    : checker->syntax->expressions[argument->value].span,
                "enum constructor supplies a payload field more than once"
            );
        } else {
            used[matched] = true;
            SolType expected = sol_type_from_id(
                checker,
                checker->syntax->fields[field_ids[matched]].type
            );
            if (!sol_type_equal(expected, actual)) {
                sol_type_error(
                    checker,
                    "SOL-TYPE-014",
                    checker->syntax->expressions[argument->value].span,
                    "enum constructor payload has the wrong type"
                );
            }
        }
        argument_id = argument->next;
    }
    bool missing = false;
    for (size_t index = 0; index < field_count; ++index) missing = missing || !used[index];
    if (missing || argument_count != field_count) {
        sol_type_error(
            checker,
            "SOL-TYPE-014",
            call->span,
            "enum constructor has the wrong number of payload arguments"
        );
    }
    free(field_ids);
    free(used);
    return (SolType){.kind = SOL_TYPE_NOMINAL, .definition = owner};
}

static SolType sol_type_record(SolTypeChecker *checker, const SolExpr *record) {
    SolExprId type_expression = record->as.record.type;
    SolResolution resolution = checker->hir->resolutions[type_expression];
    sol_type_expression(checker, type_expression);
    if (resolution.kind != SOL_RESOLUTION_DEFINITION
        || resolution.target >= checker->types->definition_count
        || checker->syntax->items[resolution.target].kind != SOL_ITEM_RECORD) {
        sol_type_arguments(checker, record->as.record.first_field);
        sol_type_error(
            checker,
            "SOL-TYPE-011",
            record->span,
            "record construction requires a record type"
        );
        return (SolType){.kind = SOL_TYPE_ERROR};
    }

    size_t field_count = 0;
    SolFieldId field_id = checker->syntax->items[resolution.target].first_field;
    while (field_id != SOL_AST_NONE) {
        if (field_count++ >= checker->syntax->field_count) {
            sol_type_malformed(checker);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        field_id = checker->syntax->fields[field_id].next;
    }
    if (field_count > SIZE_MAX / sizeof(SolFieldId)) {
        checker->allocation_failed = true;
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    bool *used = field_count == 0 ? NULL : calloc(field_count, sizeof(*used));
    SolFieldId *field_ids = field_count == 0
        ? NULL
        : malloc(field_count * sizeof(*field_ids));
    if (field_count != 0 && (used == NULL || field_ids == NULL)) {
        free(used);
        free(field_ids);
        checker->allocation_failed = true;
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    field_id = checker->syntax->items[resolution.target].first_field;
    for (size_t index = 0; index < field_count; ++index) {
        field_ids[index] = field_id;
        field_id = checker->syntax->fields[field_id].next;
    }

    SolArgumentId argument_id = record->as.record.first_field;
    size_t traversed = 0;
    while (argument_id != SOL_AST_NONE) {
        if (traversed++ >= checker->syntax->argument_count) {
            sol_type_malformed(checker);
            break;
        }
        const SolArgument *argument = &checker->syntax->arguments[argument_id];
        SolType actual = sol_type_expression(checker, argument->value);
        size_t matched = SIZE_MAX;
        for (size_t index = 0; index < field_count; ++index) {
            if (sol_type_name_equal(
                checker->source,
                checker->syntax->fields[field_ids[index]].name,
                argument->name
            )) {
                matched = index;
                break;
            }
        }
        if (matched == SIZE_MAX) {
            sol_type_error(
                checker,
                "SOL-TYPE-013",
                argument->name,
                "record literal contains an unknown field"
            );
        } else if (used[matched]) {
            sol_type_error(
                checker,
                "SOL-TYPE-013",
                argument->name,
                "record literal supplies a field more than once"
            );
        } else {
            used[matched] = true;
            SolType expected = sol_type_from_id(
                checker,
                checker->syntax->fields[field_ids[matched]].type
            );
            if (!sol_type_equal(expected, actual)) {
                sol_type_error(
                    checker,
                    "SOL-TYPE-013",
                    checker->syntax->expressions[argument->value].span,
                    "record field value has the wrong type"
                );
            }
        }
        argument_id = argument->next;
    }
    for (size_t index = 0; index < field_count; ++index) {
        if (!used[index]) {
            sol_type_error(
                checker,
                "SOL-TYPE-013",
                record->span,
                "record literal is missing a required field"
            );
            break;
        }
    }
    free(used);
    free(field_ids);
    return (SolType){.kind = SOL_TYPE_NOMINAL, .definition = resolution.target};
}

static SolType sol_type_match(SolTypeChecker *checker, const SolExpr *match_expression) {
    SolType scrutinee = sol_type_expression(checker, match_expression->as.match_expr.scrutinee);
    size_t case_count = scrutinee.kind == SOL_TYPE_BOOL ? 2 : 0;
    SolVariantId *variants = NULL;
    if (scrutinee.kind == SOL_TYPE_NOMINAL
        && scrutinee.definition < checker->syntax->item_count
        && checker->syntax->items[scrutinee.definition].kind == SOL_ITEM_ENUM) {
        SolVariantId variant = checker->syntax->items[scrutinee.definition].first_variant;
        while (variant != SOL_AST_NONE) {
            if (case_count++ >= checker->syntax->variant_count) {
                sol_type_malformed(checker);
                return (SolType){.kind = SOL_TYPE_ERROR};
            }
            variant = checker->syntax->variants[variant].next;
        }
        if (case_count > SIZE_MAX / sizeof(*variants)) {
            checker->allocation_failed = true;
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        variants = case_count == 0 ? NULL : malloc(case_count * sizeof(*variants));
        if (case_count != 0 && variants == NULL) {
            checker->allocation_failed = true;
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        variant = checker->syntax->items[scrutinee.definition].first_variant;
        for (size_t index = 0; index < case_count; ++index) {
            variants[index] = variant;
            variant = checker->syntax->variants[variant].next;
        }
    }
    bool *covered = case_count == 0 ? NULL : calloc(case_count, sizeof(*covered));
    if (case_count != 0 && covered == NULL) {
        free(variants);
        checker->allocation_failed = true;
        return (SolType){.kind = SOL_TYPE_ERROR};
    }

    bool wildcard = false;
    bool enum_scrutinee = scrutinee.kind == SOL_TYPE_NOMINAL
        && scrutinee.definition < checker->syntax->item_count
        && checker->syntax->items[scrutinee.definition].kind == SOL_ITEM_ENUM;
    bool open_enum = enum_scrutinee
        && checker->syntax->items[scrutinee.definition].is_open;
    bool finite_domain = scrutinee.kind == SOL_TYPE_BOOL || (enum_scrutinee && !open_enum);
    bool have_result = false;
    SolType result = {.kind = SOL_TYPE_UNKNOWN};
    SolMatchArmId arm_id = match_expression->as.match_expr.first_arm;
    size_t arm_count = 0;
    while (arm_id != SOL_AST_NONE) {
        if (arm_count++ >= checker->syntax->match_arm_count) {
            sol_type_malformed(checker);
            break;
        }
        const SolMatchArm *arm = &checker->syntax->match_arms[arm_id];
        const SolPattern *pattern = &checker->syntax->patterns[arm->pattern];
        bool complete_before_arm = case_count != 0 && !open_enum;
        for (size_t index = 0; index < case_count; ++index) {
            complete_before_arm = complete_before_arm && covered[index];
        }
        if (wildcard || complete_before_arm) {
            sol_type_error(
                checker,
                "SOL-MATCH-001",
                pattern->span,
                "match arm is unreachable because previous arms are exhaustive"
            );
        }
        SolVariantId matched_variant = SOL_AST_NONE;
        if (pattern->kind == SOL_PATTERN_WILDCARD) {
            wildcard = true;
        } else if (pattern->kind == SOL_PATTERN_BOOL) {
            if (scrutinee.kind != SOL_TYPE_BOOL) {
                sol_type_error(
                    checker,
                    "SOL-MATCH-001",
                    pattern->span,
                    "boolean pattern requires a Bool match value"
                );
            } else {
                size_t index = pattern->bool_value ? 1 : 0;
                if (covered[index]) {
                    sol_type_error(
                        checker,
                        "SOL-MATCH-001",
                        pattern->span,
                        "duplicate match case"
                    );
                }
                covered[index] = true;
            }
        } else if (scrutinee.kind == SOL_TYPE_NOMINAL
            && scrutinee.definition < checker->syntax->item_count
            && checker->syntax->items[scrutinee.definition].kind == SOL_ITEM_ENUM) {
            matched_variant = sol_type_find_variant(
                checker,
                scrutinee.definition,
                pattern->name
            );
            if (matched_variant == SOL_AST_NONE) {
                sol_type_error(
                    checker,
                    "SOL-MATCH-001",
                    pattern->name,
                    "enum has no variant with this name"
                );
            } else {
                for (size_t index = 0; index < case_count; ++index) {
                    if (variants[index] == matched_variant) {
                        if (covered[index]) {
                            sol_type_error(
                                checker,
                                "SOL-MATCH-001",
                                pattern->span,
                                "duplicate match case"
                            );
                        }
                        covered[index] = true;
                        break;
                    }
                }
            }
        } else {
            sol_type_error(
                checker,
                "SOL-MATCH-001",
                pattern->span,
                "variant pattern requires an enum match value"
            );
        }

        SolPatternBindingId binding = pattern->first_binding;
        SolFieldId payload = matched_variant == SOL_AST_NONE
            ? SOL_AST_NONE
            : checker->syntax->variants[matched_variant].first_field;
        size_t binding_count = 0;
        while (binding != SOL_AST_NONE && payload != SOL_AST_NONE) {
            if (binding_count++ >= checker->syntax->pattern_binding_count) {
                sol_type_malformed(checker);
                break;
            }
            SolLocalId local = sol_type_find_local(checker, SOL_LOCAL_PATTERN, binding);
            if (local != SOL_AST_NONE) {
                checker->types->locals[local] = sol_type_from_id(
                    checker,
                    checker->syntax->fields[payload].type
                );
            }
            binding = checker->syntax->pattern_bindings[binding].next;
            payload = checker->syntax->fields[payload].next;
        }
        if (binding != SOL_AST_NONE || payload != SOL_AST_NONE) {
            sol_type_error(
                checker,
                "SOL-MATCH-001",
                pattern->span,
                "pattern binding count does not match variant payload"
            );
        }

        SolType arm_type = sol_type_expression(checker, arm->value);
        if (!have_result || result.kind == SOL_TYPE_NEVER) {
            result = arm_type;
            have_result = true;
        } else if (arm_type.kind != SOL_TYPE_NEVER && !sol_type_equal(result, arm_type)) {
            sol_type_error(
                checker,
                "SOL-TYPE-008",
                arm->span,
                "match arms must produce the same type"
            );
            result = (SolType){.kind = SOL_TYPE_ERROR};
        }
        arm_id = arm->next;
    }
    if (!wildcard) {
        bool exhaustive = finite_domain;
        for (size_t index = 0; index < case_count; ++index) exhaustive = exhaustive && covered[index];
        if (open_enum) {
            exhaustive = false;
        }
        if (!exhaustive) {
            sol_type_error(
                checker,
                "SOL-MATCH-001",
                match_expression->span,
                "match expression is not exhaustive"
            );
        }
    }
    free(covered);
    free(variants);
    if (have_result) return result;
    return finite_domain && case_count == 0
        ? (SolType){.kind = SOL_TYPE_NEVER}
        : (SolType){.kind = SOL_TYPE_ERROR};
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
                SolType base = sol_type_expression(checker, expression->as.field.base);
                if (base.kind == SOL_TYPE_NOMINAL
                    && base.definition < checker->syntax->item_count
                    && checker->syntax->items[base.definition].kind == SOL_ITEM_RECORD) {
                    SolFieldId field = sol_type_find_field(
                        checker,
                        base.definition,
                        expression->as.field.name
                    );
                    if (field == SOL_AST_NONE) {
                        sol_type_error(
                            checker,
                            "SOL-TYPE-013",
                            expression->as.field.name,
                            "record type has no field with this name"
                        );
                        type = (SolType){.kind = SOL_TYPE_ERROR};
                    } else {
                        type = sol_type_from_id(checker, checker->syntax->fields[field].type);
                    }
                } else if (base.kind == SOL_TYPE_NOMINAL
                    && base.definition < checker->syntax->item_count
                    && checker->syntax->items[base.definition].kind == SOL_ITEM_ENUM) {
                    SolResolution base_resolution = checker->hir->resolutions[
                        expression->as.field.base
                    ];
                    if (base_resolution.kind != SOL_RESOLUTION_DEFINITION
                        || base_resolution.target != base.definition) {
                        sol_type_error(
                            checker,
                            "SOL-TYPE-014",
                            expression->span,
                            "enum constructors must be selected through the enum type"
                        );
                        type = (SolType){.kind = SOL_TYPE_ERROR};
                    } else {
                        SolVariantId variant = sol_type_find_variant(
                            checker,
                            base.definition,
                            expression->as.field.name
                        );
                        if (variant == SOL_AST_NONE) {
                            sol_type_error(
                                checker,
                                "SOL-TYPE-014",
                                expression->as.field.name,
                                "enum type has no variant with this name"
                            );
                            type = (SolType){.kind = SOL_TYPE_ERROR};
                        } else if (checker->syntax->variants[variant].first_field == SOL_AST_NONE) {
                            type = base;
                        } else {
                            type = (SolType){.kind = SOL_TYPE_VARIANT, .definition = variant};
                        }
                    }
                } else if (base.kind == SOL_TYPE_NOMINAL
                    && base.definition < checker->syntax->item_count
                    && checker->syntax->items[base.definition].kind == SOL_ITEM_CAPABILITY) {
                    SolCapabilityMemberId member = sol_type_find_capability_member(
                        checker,
                        base.definition,
                        expression->as.field.name
                    );
                    if (member == SOL_AST_NONE) {
                        sol_type_error(
                            checker,
                            "SOL-TYPE-015",
                            expression->as.field.name,
                            "capability has no operation with this name"
                        );
                        type = (SolType){.kind = SOL_TYPE_ERROR};
                    } else {
                        type = (SolType){
                            .kind = SOL_TYPE_CAPABILITY_OPERATION,
                            .definition = member,
                        };
                    }
                } else if (base.kind != SOL_TYPE_UNKNOWN && base.kind != SOL_TYPE_ERROR) {
                    sol_type_error(
                        checker,
                        "SOL-TYPE-013",
                        expression->span,
                        "field access requires a record value"
                    );
                    type = (SolType){.kind = SOL_TYPE_ERROR};
                }
            }
            break;
        case SOL_EXPR_RECORD:
            type = sol_type_record(checker, expression);
            break;
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
        case SOL_EXPR_MATCH:
            type = sol_type_match(checker, expression);
            break;
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
    checker->types->expression_operation_origins[expression_id]
        = sol_type_operation_origin(checker, expression_id, type);
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
    if (types->expressions != NULL || types->expression_operation_origins != NULL
        || types->locals != NULL || types->local_capability_origins != NULL
        || types->local_operation_origins != NULL || types->definitions != NULL
        || types->declared_types != NULL
        || types->expression_count != 0 || types->local_count != 0
        || types->definition_count != 0 || types->declared_type_count != 0) {
        sol_type_malformed(&checker);
        return false;
    }
    if (!sol_type_validate(&checker) || !sol_type_allocate(&checker)) {
        if (!checker.malformed) {
            sol_type_malformed(&checker);
        }
        free(checker.states);
        free(checker.declared_states);
        return false;
    }

    for (size_t index = 0; index < hir->definition_count; ++index) {
        const SolSyntaxItem *item = &syntax->items[index];
        checker.current_definition = index;
        checker.types->definitions[index] = item->kind == SOL_ITEM_FUNCTION
            ? sol_type_from_id(&checker, item->return_type_id)
            : (SolType){.kind = SOL_TYPE_NOMINAL, .definition = index};
        if (item->kind == SOL_ITEM_RECORD) {
            SolFieldId field_id = item->first_field;
            size_t traversed = 0;
            while (field_id != SOL_AST_NONE) {
                if (traversed++ >= syntax->field_count) {
                    sol_type_malformed(&checker);
                    break;
                }
                const SolField *field = &syntax->fields[field_id];
                sol_type_from_id(&checker, field->type);
                SolFieldId previous = item->first_field;
                size_t previous_count = 0;
                while (previous != field_id) {
                    if (previous == SOL_AST_NONE || previous_count++ >= syntax->field_count) {
                        sol_type_malformed(&checker);
                        break;
                    }
                    if (sol_type_name_equal(
                        source,
                        syntax->fields[previous].name,
                        field->name
                    )) {
                        sol_type_error(
                            &checker,
                            "SOL-TYPE-013",
                            field->name,
                            "record declares the same field more than once"
                        );
                        break;
                    }
                    previous = syntax->fields[previous].next;
                }
                field_id = field->next;
            }
        } else if (item->kind == SOL_ITEM_ENUM) {
            SolVariantId variant_id = item->first_variant;
            size_t variant_count = 0;
            while (variant_id != SOL_AST_NONE) {
                if (variant_count++ >= syntax->variant_count) {
                    sol_type_malformed(&checker);
                    break;
                }
                const SolVariant *variant = &syntax->variants[variant_id];
                SolVariantId previous = item->first_variant;
                size_t previous_count = 0;
                while (previous != variant_id) {
                    if (previous == SOL_AST_NONE
                        || previous_count++ >= syntax->variant_count) {
                        sol_type_malformed(&checker);
                        break;
                    }
                    if (sol_type_name_equal(
                        source,
                        syntax->variants[previous].name,
                        variant->name
                    )) {
                        sol_type_error(
                            &checker,
                            "SOL-TYPE-014",
                            variant->name,
                            "enum declares the same variant more than once"
                        );
                        break;
                    }
                    previous = syntax->variants[previous].next;
                }
                SolFieldId payload = variant->first_field;
                size_t payload_count = 0;
                while (payload != SOL_AST_NONE) {
                    if (payload_count++ >= syntax->field_count) {
                        sol_type_malformed(&checker);
                        break;
                    }
                    const SolField *payload_field = &syntax->fields[payload];
                    sol_type_from_id(&checker, payload_field->type);
                    SolFieldId previous_payload = variant->first_field;
                    while (previous_payload != payload) {
                        if (sol_type_name_equal(
                            source,
                            syntax->fields[previous_payload].name,
                            payload_field->name
                        )) {
                            sol_type_error(
                                &checker,
                                "SOL-TYPE-014",
                                payload_field->name,
                                "enum payload declares the same field more than once"
                            );
                            break;
                        }
                        previous_payload = syntax->fields[previous_payload].next;
                    }
                    payload = syntax->fields[payload].next;
                }
                variant_id = variant->next;
            }
        } else if (item->kind == SOL_ITEM_CAPABILITY) {
            SolCapabilityMemberId member_id = item->first_member;
            size_t member_count = 0;
            while (member_id != SOL_AST_NONE) {
                if (member_count++ >= syntax->capability_member_count) {
                    sol_type_malformed(&checker);
                    break;
                }
                const SolCapabilityMember *member = &syntax->capability_members[member_id];
                sol_type_from_id(&checker, member->return_type_id);
                SolCapabilityMemberId previous_member = item->first_member;
                size_t previous_member_count = 0;
                while (previous_member != member_id) {
                    if (previous_member == SOL_AST_NONE
                        || previous_member_count++ >= syntax->capability_member_count) {
                        sol_type_malformed(&checker);
                        break;
                    }
                    if (sol_type_name_equal(
                        source,
                        syntax->capability_members[previous_member].name,
                        member->name
                    )) {
                        sol_type_error(
                            &checker,
                            "SOL-TYPE-015",
                            member->name,
                            "capability declares the same operation more than once"
                        );
                        break;
                    }
                    previous_member = syntax->capability_members[previous_member].next;
                }
                SolParameterId parameter_id = member->first_parameter;
                size_t parameter_count = 0;
                while (parameter_id != SOL_AST_NONE) {
                    if (parameter_count++ >= syntax->parameter_count) {
                        sol_type_malformed(&checker);
                        break;
                    }
                    const SolParameter *parameter = &syntax->parameters[parameter_id];
                    sol_type_from_id(&checker, parameter->type_id);
                    SolParameterId previous_parameter = member->first_parameter;
                    size_t previous_parameter_count = 0;
                    while (previous_parameter != parameter_id) {
                        if (previous_parameter == SOL_AST_NONE
                            || previous_parameter_count++ >= syntax->parameter_count) {
                            sol_type_malformed(&checker);
                            break;
                        }
                        if (sol_type_name_equal(
                            source,
                            syntax->parameters[previous_parameter].name,
                            parameter->name
                        )) {
                            sol_type_error(
                                &checker,
                                "SOL-TYPE-015",
                                parameter->name,
                                "capability operation declares the same parameter more than once"
                            );
                            break;
                        }
                        previous_parameter = syntax->parameters[previous_parameter].next;
                    }
                    parameter_id = parameter->next;
                }
                member_id = member->next;
            }
        }
    }
    for (size_t index = 0; index < hir->local_count; ++index) {
        const SolHirLocal *local = &hir->locals[index];
        if (local->kind == SOL_LOCAL_PARAMETER && local->syntax_id < syntax->parameter_count) {
            checker.types->locals[index] = sol_type_from_id(
                &checker,
                syntax->parameters[local->syntax_id].type_id
            );
            SolType type = checker.types->locals[index];
            if (type.kind == SOL_TYPE_NOMINAL
                && type.definition < syntax->item_count
                && syntax->items[type.definition].kind == SOL_ITEM_CAPABILITY) {
                checker.types->local_capability_origins[index] = local->syntax_id;
            }
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
    unsigned char *call_callees = calloc(syntax->expression_count, sizeof(*call_callees));
    if (syntax->expression_count != 0 && call_callees == NULL) {
        checker.allocation_failed = true;
    } else {
        for (size_t index = 0; index < syntax->expression_count; ++index) {
            const SolExpr *expression = &syntax->expressions[index];
            if (expression->kind == SOL_EXPR_CALL) {
                call_callees[expression->as.call.callee] = 1;
            }
        }
        for (size_t index = 0; index < syntax->expression_count; ++index) {
            if (checker.types->expressions[index].kind != SOL_TYPE_CAPABILITY_OPERATION
                || checker.types->expression_operation_origins[index] != SOL_AST_NONE
                || call_callees[index]) {
                continue;
            }
            sol_type_error(
                &checker,
                "SOL-TYPE-015",
                syntax->expressions[index].span,
                "capability operation has no known receiver authority"
            );
        }
    }
    free(call_callees);

    free(checker.states);
    free(checker.declared_states);
    return !checker.allocation_failed
        && !checker.malformed
        && !diagnostics->allocation_failed;
}
