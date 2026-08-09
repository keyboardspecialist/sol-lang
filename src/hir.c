#include "sol/hir.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SolSpan name;
    SolResolution resolution;
    size_t scope_depth;
} SolBinding;

typedef struct {
    const SolSource *source;
    const SolSyntaxTree *syntax;
    SolHirModule *module;
    SolDiagnostics *diagnostics;
    SolBinding *bindings;
    unsigned char *expression_states;
    size_t binding_count;
    size_t binding_capacity;
    size_t scope_depth;
    size_t traversal_depth;
    SolDefId current_definition;
    bool allocation_failed;
    bool malformed;
    bool traversal_limit_reported;
} SolResolver;

void sol_hir_module_init(SolHirModule *module) {
    memset(module, 0, sizeof(*module));
}

void sol_hir_module_free(SolHirModule *module) {
    free(module->definitions);
    free(module->locals);
    free(module->resolutions);
    memset(module, 0, sizeof(*module));
}

static bool sol_span_equal(const SolSource *source, SolSpan left, SolSpan right) {
    size_t left_length = left.end - left.start;
    size_t right_length = right.end - right.start;
    return left_length == right_length
        && memcmp(source->text + left.start, source->text + right.start, left_length) == 0;
}

static int sol_path_next_byte(const SolSource *source, SolSpan span, size_t *cursor) {
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

static bool sol_path_span_equal(const SolSource *source, SolSpan left, SolSpan right) {
    size_t left_cursor = left.start;
    size_t right_cursor = right.start;
    for (;;) {
        int left_byte = sol_path_next_byte(source, left, &left_cursor);
        int right_byte = sol_path_next_byte(source, right, &right_cursor);
        if (left_byte != right_byte) {
            return false;
        }
        if (left_byte < 0) {
            return true;
        }
    }
}

static bool sol_span_text_equal(const SolSource *source, SolSpan span, const char *text) {
    size_t length = span.end - span.start;
    return strlen(text) == length
        && memcmp(source->text + span.start, text, length) == 0;
}

static bool sol_span_valid(const SolSource *source, SolSpan span) {
    return span.start <= span.end && span.end <= source->length;
}

static void sol_resolver_malformed(SolResolver *resolver) {
    if (!resolver->malformed) {
        sol_diagnostics_add(
            resolver->diagnostics,
            "SOL-INTERNAL-002",
            SOL_SEVERITY_ERROR,
            (SolSpan){0},
            "malformed syntax AST passed to semantic lowering"
        );
    }
    resolver->malformed = true;
}

static SolEffectId sol_resolver_effect_root(
    const SolSyntaxTree *syntax,
    const SolEffect *effect
) {
    switch (effect->owner_kind) {
        case SOL_EFFECT_OWNER_ITEM:
            return effect->owner < syntax->item_count
                ? syntax->items[effect->owner].first_effect
                : SOL_AST_NONE;
        case SOL_EFFECT_OWNER_CAPABILITY_MEMBER:
            return effect->owner < syntax->capability_member_count
                ? syntax->capability_members[effect->owner].first_effect
                : SOL_AST_NONE;
        case SOL_EFFECT_OWNER_TYPE:
            return effect->owner < syntax->type_count
                ? syntax->types[effect->owner].first_effect
                : SOL_AST_NONE;
    }
    return SOL_AST_NONE;
}

static bool sol_resolver_validate(SolResolver *resolver) {
    const SolSyntaxTree *syntax = resolver->syntax;
    if (syntax->item_count > syntax->item_capacity
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
        || syntax->contract_clause_count > syntax->contract_clause_capacity
        || syntax->contract_condition_count > syntax->contract_condition_capacity
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
        || (syntax->contract_clause_count != 0 && syntax->contract_clauses == NULL)
        || (syntax->contract_condition_count != 0
            && syntax->contract_conditions == NULL)) {
        sol_resolver_malformed(resolver);
        return false;
    }
    for (size_t index = 0; index < syntax->item_count; ++index) {
        const SolSyntaxItem *item = &syntax->items[index];
        if ((int)item->kind < 0 || item->kind > SOL_ITEM_FUNCTION
            || !sol_span_valid(resolver->source, item->name)
            || !sol_span_valid(resolver->source, item->span)
            || !sol_span_valid(resolver->source, item->return_type)
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
            || (item->first_contract != SOL_AST_NONE
                && item->first_contract >= syntax->contract_clause_count)
            || (item->capability_source != SOL_AST_NONE
                && item->capability_source >= syntax->parameter_count)) {
            sol_resolver_malformed(resolver);
            return false;
        }
        if (item->first_effect != SOL_AST_NONE
            && (syntax->effects[item->first_effect].owner_kind != SOL_EFFECT_OWNER_ITEM
                || syntax->effects[item->first_effect].owner != index)) {
            sol_resolver_malformed(resolver);
            return false;
        }
        if (item->kind != SOL_ITEM_CAPABILITY && item->first_member != SOL_AST_NONE) {
            sol_resolver_malformed(resolver);
            return false;
        }
        if ((item->capability_source != SOL_AST_NONE
                && item->kind != SOL_ITEM_CAPABILITY)
            || (item->capability_source != SOL_AST_NONE
                && (syntax->parameters[item->capability_source].next != SOL_AST_NONE
                    || syntax->parameters[item->capability_source].type_id
                        >= syntax->type_count
                    || syntax->types[
                        syntax->parameters[item->capability_source].type_id
                    ].kind != SOL_SYNTAX_TYPE_PATH
                    || !syntax->types[
                        syntax->parameters[item->capability_source].type_id
                    ].is_capability
                    || syntax->types[
                        syntax->parameters[item->capability_source].type_id
                    ].first_argument != SOL_AST_NONE))) {
            sol_resolver_malformed(resolver);
            return false;
        }
        if (item->result_authority_parameter != SOL_AST_NONE) {
            SolParameterId parameter = item->first_parameter;
            size_t traversed = 0;
            while (parameter != SOL_AST_NONE
                && parameter != item->result_authority_parameter) {
                if (parameter >= syntax->parameter_count
                    || traversed++ >= syntax->parameter_count) {
                    sol_resolver_malformed(resolver);
                    return false;
                }
                parameter = syntax->parameters[parameter].next;
            }
            if (item->kind != SOL_ITEM_FUNCTION || parameter == SOL_AST_NONE
                || parameter >= syntax->parameter_count
                || item->return_type_id >= syntax->type_count
                || !syntax->types[item->return_type_id].is_capability
                || syntax->parameters[parameter].type_id >= syntax->type_count
                || !syntax->types[syntax->parameters[parameter].type_id].is_capability) {
                sol_resolver_malformed(resolver);
                return false;
            }
        }
        SolCapabilityMemberId member_id = item->first_member;
        size_t member_count = 0;
        while (member_id != SOL_AST_NONE) {
            if (member_id >= syntax->capability_member_count
                || member_count++ >= syntax->capability_member_count
                || syntax->capability_members[member_id].owner_item != index) {
                sol_resolver_malformed(resolver);
                return false;
            }
            member_id = syntax->capability_members[member_id].next;
        }
    }
    for (size_t index = 0; index < syntax->parameter_count; ++index) {
        const SolParameter *parameter = &syntax->parameters[index];
        if (!sol_span_valid(resolver->source, parameter->name)
            || !sol_span_valid(resolver->source, parameter->type)
            || parameter->type_id >= syntax->type_count
            || (parameter->next != SOL_AST_NONE && parameter->next >= syntax->parameter_count)) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->type_count; ++index) {
        const SolSyntaxType *type = &syntax->types[index];
        if ((int)type->kind < 0 || type->kind > SOL_SYNTAX_TYPE_FUNCTION
            || !sol_span_valid(resolver->source, type->span)
            || !sol_span_valid(resolver->source, type->name)
            || (type->first_argument != SOL_AST_NONE
                && type->first_argument >= syntax->type_argument_count)
            || (type->kind == SOL_SYNTAX_TYPE_FUNCTION
                && (type->return_type >= syntax->type_count
                    || (type->first_effect != SOL_AST_NONE
                        && type->first_effect >= syntax->effect_count)))) {
            sol_resolver_malformed(resolver);
            return false;
        }
        if (type->kind == SOL_SYNTAX_TYPE_FUNCTION && type->first_effect != SOL_AST_NONE
            && (syntax->effects[type->first_effect].owner_kind != SOL_EFFECT_OWNER_TYPE
                || syntax->effects[type->first_effect].owner != index)) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->type_argument_count; ++index) {
        const SolTypeArgument *argument = &syntax->type_arguments[index];
        if (argument->type >= syntax->type_count
            || (argument->next != SOL_AST_NONE
                && argument->next >= syntax->type_argument_count)) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->field_count; ++index) {
        const SolField *field = &syntax->fields[index];
        if (!sol_span_valid(resolver->source, field->name)
            || !sol_span_valid(resolver->source, field->span)
            || field->type >= syntax->type_count
            || (field->next != SOL_AST_NONE && field->next >= syntax->field_count)) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->variant_count; ++index) {
        const SolVariant *variant = &syntax->variants[index];
        if (!sol_span_valid(resolver->source, variant->name)
            || !sol_span_valid(resolver->source, variant->span)
            || (variant->first_field != SOL_AST_NONE
                && variant->first_field >= syntax->field_count)
            || (variant->next != SOL_AST_NONE && variant->next >= syntax->variant_count)
            || variant->owner_item >= syntax->item_count
            || syntax->items[variant->owner_item].kind != SOL_ITEM_ENUM) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->pattern_count; ++index) {
        const SolPattern *pattern = &syntax->patterns[index];
        if ((int)pattern->kind < 0 || pattern->kind > SOL_PATTERN_BOOL
            || !sol_span_valid(resolver->source, pattern->span)
            || !sol_span_valid(resolver->source, pattern->name)
            || (pattern->first_binding != SOL_AST_NONE
                && pattern->first_binding >= syntax->pattern_binding_count)) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->pattern_binding_count; ++index) {
        const SolPatternBinding *binding = &syntax->pattern_bindings[index];
        if (!sol_span_valid(resolver->source, binding->name)
            || (binding->next != SOL_AST_NONE
                && binding->next >= syntax->pattern_binding_count)) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->match_arm_count; ++index) {
        const SolMatchArm *arm = &syntax->match_arms[index];
        if (arm->pattern >= syntax->pattern_count
            || arm->value >= syntax->expression_count
            || !sol_span_valid(resolver->source, arm->span)
            || (arm->next != SOL_AST_NONE && arm->next >= syntax->match_arm_count)) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->effect_count; ++index) {
        const SolEffect *effect = &syntax->effects[index];
        if (!sol_span_valid(resolver->source, effect->name)
            || !sol_span_valid(resolver->source, effect->argument)
            || !sol_span_valid(resolver->source, effect->span)
            || (effect->next != SOL_AST_NONE && effect->next >= syntax->effect_count)
            || (int)effect->owner_kind < 0
            || effect->owner_kind > SOL_EFFECT_OWNER_TYPE
            || (effect->owner_kind == SOL_EFFECT_OWNER_ITEM
                && (effect->owner >= syntax->item_count
                    || syntax->items[effect->owner].kind != SOL_ITEM_FUNCTION))
            || (effect->owner_kind == SOL_EFFECT_OWNER_CAPABILITY_MEMBER
                && effect->owner >= syntax->capability_member_count)
            || (effect->owner_kind == SOL_EFFECT_OWNER_TYPE
                && (effect->owner >= syntax->type_count
                    || syntax->types[effect->owner].kind != SOL_SYNTAX_TYPE_FUNCTION))) {
            sol_resolver_malformed(resolver);
            return false;
        }
        SolEffectId linked = sol_resolver_effect_root(syntax, effect);
        size_t traversed = 0;
        bool found = false;
        while (linked != SOL_AST_NONE) {
            if (linked >= syntax->effect_count
                || traversed++ >= syntax->effect_count
                || syntax->effects[linked].owner_kind != effect->owner_kind
                || syntax->effects[linked].owner != effect->owner) {
                sol_resolver_malformed(resolver);
                return false;
            }
            if (linked == index) found = true;
            linked = syntax->effects[linked].next;
        }
        if (!found) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->capability_member_count; ++index) {
        const SolCapabilityMember *member = &syntax->capability_members[index];
        if (!sol_span_valid(resolver->source, member->name)
            || !sol_span_valid(resolver->source, member->span)
            || !sol_span_valid(resolver->source, member->return_type)
            || (member->first_parameter != SOL_AST_NONE
                && member->first_parameter >= syntax->parameter_count)
            || member->return_type_id >= syntax->type_count
            || (member->first_effect != SOL_AST_NONE
                && member->first_effect >= syntax->effect_count)
            || (member->body != SOL_AST_NONE
                && member->body >= syntax->expression_count)
            || (member->next != SOL_AST_NONE
                && member->next >= syntax->capability_member_count)
            || (member->first_contract != SOL_AST_NONE
                && member->first_contract >= syntax->contract_clause_count)
            || member->owner_item >= syntax->item_count
            || syntax->items[member->owner_item].kind != SOL_ITEM_CAPABILITY) {
            sol_resolver_malformed(resolver);
            return false;
        }
        bool derived = syntax->items[member->owner_item].capability_source != SOL_AST_NONE;
        if (derived != (member->body != SOL_AST_NONE)) {
            sol_resolver_malformed(resolver);
            return false;
        }
        if (member->result_authority_from_self
            && !syntax->types[member->return_type_id].is_capability) {
            sol_resolver_malformed(resolver);
            return false;
        }
        if (member->first_effect != SOL_AST_NONE
            && (syntax->effects[member->first_effect].owner_kind
                    != SOL_EFFECT_OWNER_CAPABILITY_MEMBER
                || syntax->effects[member->first_effect].owner != index)) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->argument_count; ++index) {
        const SolArgument *argument = &syntax->arguments[index];
        if (!sol_span_valid(resolver->source, argument->name)
            || argument->value >= syntax->expression_count
            || (argument->next != SOL_AST_NONE && argument->next >= syntax->argument_count)) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->statement_count; ++index) {
        const SolStatement *statement = &syntax->statements[index];
        if ((int)statement->kind < 0 || statement->kind > SOL_STATEMENT_EXPRESSION) {
            sol_resolver_malformed(resolver);
            return false;
        }
        SolExprId value = statement->kind == SOL_STATEMENT_LET
            ? statement->as.let_statement.value
            : statement->as.expression;
        if (!sol_span_valid(resolver->source, statement->span)
            || value >= syntax->expression_count
            || (statement->next != SOL_AST_NONE && statement->next >= syntax->statement_count)
            || (statement->kind == SOL_STATEMENT_LET
                && !sol_span_valid(resolver->source, statement->as.let_statement.name))) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->expression_count; ++index) {
        const SolExpr *expression = &syntax->expressions[index];
        bool valid = (int)expression->kind >= 0
            && expression->kind <= SOL_EXPR_OLD
            && sol_span_valid(resolver->source, expression->span);
        switch (expression->kind) {
            case SOL_EXPR_PATH:
                valid = valid && sol_span_valid(resolver->source, expression->as.name);
                break;
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
                valid = valid
                    && expression->as.field.base < syntax->expression_count
                    && sol_span_valid(resolver->source, expression->as.field.name);
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
            case SOL_EXPR_HANDLE:
                valid = valid
                    && sol_span_valid(resolver->source, expression->as.handle.effect_name)
                    && expression->as.handle.effect_name.start
                        < expression->as.handle.effect_name.end
                    && expression->as.handle.authority < syntax->expression_count
                    && expression->as.handle.provider < syntax->expression_count
                    && expression->as.handle.body < syntax->expression_count
                    && syntax->expressions[expression->as.handle.body].kind == SOL_EXPR_BLOCK;
                break;
            case SOL_EXPR_OLD:
                valid = valid && expression->as.old_expression < syntax->expression_count;
                break;
            default:
                break;
        }
        if (!valid) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    if (!sol_syntax_contracts_validate(resolver->source, syntax)) {
        sol_resolver_malformed(resolver);
        return false;
    }
    return true;
}

static bool sol_resolver_allocate(SolResolver *resolver) {
    const SolSyntaxTree *syntax = resolver->syntax;
    if (syntax->item_count > SIZE_MAX / sizeof(*resolver->module->definitions)
        || syntax->expression_count > SIZE_MAX / sizeof(*resolver->module->resolutions)) {
        return false;
    }

    resolver->module->definition_count = syntax->item_count;
    resolver->module->resolution_count = syntax->expression_count;
    resolver->module->definitions = calloc(
        syntax->item_count,
        sizeof(*resolver->module->definitions)
    );
    resolver->module->resolutions = calloc(
        syntax->expression_count,
        sizeof(*resolver->module->resolutions)
    );

    if ((syntax->item_count != 0 && resolver->module->definitions == NULL)
        || (syntax->expression_count != 0 && resolver->module->resolutions == NULL)) {
        return false;
    }
    resolver->expression_states = calloc(
        syntax->expression_count,
        sizeof(*resolver->expression_states)
    );
    if (syntax->expression_count != 0 && resolver->expression_states == NULL) {
        return false;
    }

    if (syntax->parameter_count > SIZE_MAX - syntax->statement_count) {
        return false;
    }
    size_t local_capacity = syntax->parameter_count + syntax->statement_count;
    if (local_capacity > SIZE_MAX - syntax->pattern_binding_count) {
        return false;
    }
    local_capacity += syntax->pattern_binding_count;
    if (local_capacity > SIZE_MAX / sizeof(*resolver->module->locals)
        || local_capacity > SIZE_MAX / sizeof(*resolver->bindings)) {
        return false;
    }
    resolver->module->locals = calloc(local_capacity, sizeof(*resolver->module->locals));
    resolver->bindings = calloc(local_capacity, sizeof(*resolver->bindings));
    if (local_capacity != 0
        && (resolver->module->locals == NULL || resolver->bindings == NULL)) {
        return false;
    }
    resolver->module->local_capacity = local_capacity;
    resolver->binding_capacity = local_capacity;
    return true;
}

static SolResolution sol_resolver_builtin(const SolSource *source, SolSpan name) {
    if (sol_span_text_equal(source, name, "ok")) {
        return (SolResolution){.kind = SOL_RESOLUTION_BUILTIN, .target = SOL_BUILTIN_OK};
    }
    if (sol_span_text_equal(source, name, "err")) {
        return (SolResolution){.kind = SOL_RESOLUTION_BUILTIN, .target = SOL_BUILTIN_ERR};
    }
    if (sol_span_text_equal(source, name, "some")) {
        return (SolResolution){.kind = SOL_RESOLUTION_BUILTIN, .target = SOL_BUILTIN_SOME};
    }
    if (sol_span_text_equal(source, name, "none")) {
        return (SolResolution){.kind = SOL_RESOLUTION_BUILTIN, .target = SOL_BUILTIN_NONE};
    }
    return (SolResolution){.kind = SOL_RESOLUTION_ERROR};
}

static SolResolution sol_resolver_lookup(SolResolver *resolver, SolSpan name) {
    for (size_t index = resolver->binding_count; index > 0; --index) {
        SolBinding *binding = &resolver->bindings[index - 1];
        if (sol_span_equal(resolver->source, binding->name, name)) {
            return binding->resolution;
        }
    }
    for (size_t index = 0; index < resolver->module->definition_count; ++index) {
        if (sol_path_span_equal(
            resolver->source,
            resolver->module->definitions[index].name,
            name
        )) {
            return (SolResolution){
                .kind = SOL_RESOLUTION_DEFINITION,
                .target = index,
            };
        }
    }
    return sol_resolver_builtin(resolver->source, name);
}

static SolResolution sol_resolver_lookup_definition(SolResolver *resolver, SolSpan name) {
    for (size_t index = 0; index < resolver->module->definition_count; ++index) {
        if (sol_path_span_equal(
            resolver->source,
            resolver->module->definitions[index].name,
            name
        )) {
            return (SolResolution){
                .kind = SOL_RESOLUTION_DEFINITION,
                .target = index,
            };
        }
    }
    return (SolResolution){.kind = SOL_RESOLUTION_ERROR};
}

static SolExprId sol_resolver_field_root(SolResolver *resolver, SolExprId expression_id) {
    SolExprId root = expression_id;
    while (root < resolver->syntax->expression_count
        && resolver->syntax->expressions[root].kind == SOL_EXPR_FIELD) {
        root = resolver->syntax->expressions[root].as.field.base;
    }
    return root;
}

static bool sol_resolver_add_binding(
    SolResolver *resolver,
    SolSpan name,
    SolLocalKind kind,
    size_t syntax_id
) {
    for (size_t index = resolver->binding_count; index > 0; --index) {
        SolBinding *binding = &resolver->bindings[index - 1];
        if (binding->scope_depth != resolver->scope_depth) {
            break;
        }
        if (sol_span_equal(resolver->source, binding->name, name)) {
            sol_diagnostics_add(
                resolver->diagnostics,
                "SOL-RESOLVE-003",
                SOL_SEVERITY_ERROR,
                name,
                "duplicate name in the same lexical scope"
            );
            break;
        }
    }

    if (resolver->binding_count >= resolver->binding_capacity) {
        resolver->allocation_failed = true;
        return false;
    }
    SolLocalId local = SOL_AST_NONE;
    for (size_t index = 0; index < resolver->module->local_count; ++index) {
        const SolHirLocal *candidate = &resolver->module->locals[index];
        if (candidate->owner == resolver->current_definition
            && candidate->kind == kind && candidate->syntax_id == syntax_id) {
            local = index;
            break;
        }
    }
    if (local == SOL_AST_NONE) {
        if (resolver->module->local_count >= resolver->module->local_capacity) {
            resolver->allocation_failed = true;
            return false;
        }
        local = resolver->module->local_count++;
        resolver->module->locals[local] = (SolHirLocal){
            .kind = kind,
            .name = name,
            .owner = resolver->current_definition,
            .syntax_id = syntax_id,
        };
    }
    resolver->bindings[resolver->binding_count++] = (SolBinding){
        .name = name,
        .resolution = {.kind = SOL_RESOLUTION_LOCAL, .target = local},
        .scope_depth = resolver->scope_depth,
    };
    return true;
}

static void sol_resolver_expression(SolResolver *resolver, SolExprId expression_id);

static void sol_resolver_match(SolResolver *resolver, const SolExpr *expression) {
    sol_resolver_expression(resolver, expression->as.match_expr.scrutinee);
    SolMatchArmId arm_id = expression->as.match_expr.first_arm;
    size_t arm_count = 0;
    while (arm_id != SOL_AST_NONE) {
        if (arm_count++ >= resolver->syntax->match_arm_count) {
            sol_resolver_malformed(resolver);
            return;
        }
        const SolMatchArm *arm = &resolver->syntax->match_arms[arm_id];
        const SolPattern *pattern = &resolver->syntax->patterns[arm->pattern];
        size_t binding_mark = resolver->binding_count;
        ++resolver->scope_depth;
        SolPatternBindingId binding_id = pattern->first_binding;
        size_t binding_count = 0;
        while (binding_id != SOL_AST_NONE) {
            if (binding_count++ >= resolver->syntax->pattern_binding_count) {
                sol_resolver_malformed(resolver);
                break;
            }
            const SolPatternBinding *binding = &resolver->syntax->pattern_bindings[binding_id];
            sol_resolver_add_binding(
                resolver,
                binding->name,
                SOL_LOCAL_PATTERN,
                binding_id
            );
            binding_id = binding->next;
        }
        sol_resolver_expression(resolver, arm->value);
        resolver->binding_count = binding_mark;
        --resolver->scope_depth;
        arm_id = arm->next;
    }
}

static void sol_resolver_arguments(SolResolver *resolver, SolArgumentId argument_id) {
    size_t traversed = 0;
    while (argument_id != SOL_AST_NONE) {
        if (argument_id >= resolver->syntax->argument_count
            || traversed++ >= resolver->syntax->argument_count) {
            sol_resolver_malformed(resolver);
            return;
        }
        const SolArgument *argument = &resolver->syntax->arguments[argument_id];
        sol_resolver_expression(resolver, argument->value);
        argument_id = argument->next;
    }
}

static void sol_resolver_block(SolResolver *resolver, const SolExpr *block) {
    size_t binding_mark = resolver->binding_count;
    ++resolver->scope_depth;
    SolStatementId statement_id = block->as.block.first_statement;
    size_t traversed = 0;
    while (statement_id != SOL_AST_NONE) {
        if (statement_id >= resolver->syntax->statement_count
            || traversed++ >= resolver->syntax->statement_count) {
            sol_resolver_malformed(resolver);
            break;
        }
        const SolStatement *statement = &resolver->syntax->statements[statement_id];
        if (statement->kind == SOL_STATEMENT_LET) {
            sol_resolver_expression(resolver, statement->as.let_statement.value);
            sol_resolver_add_binding(
                resolver,
                statement->as.let_statement.name,
                SOL_LOCAL_BINDING,
                statement_id
            );
        } else {
            sol_resolver_expression(resolver, statement->as.expression);
        }
        statement_id = statement->next;
    }
    resolver->binding_count = binding_mark;
    --resolver->scope_depth;
}

static void sol_resolver_expression(SolResolver *resolver, SolExprId expression_id) {
    if (expression_id == SOL_AST_NONE || expression_id >= resolver->syntax->expression_count) {
        sol_resolver_malformed(resolver);
        return;
    }
    if (resolver->expression_states[expression_id] == 1) {
        sol_resolver_malformed(resolver);
        return;
    }
    if (resolver->expression_states[expression_id] == 2) {
        return;
    }
    if (resolver->traversal_depth >= 256) {
        if (!resolver->traversal_limit_reported) {
            sol_diagnostics_add(
                resolver->diagnostics,
                "SOL-RESOLVE-004",
                SOL_SEVERITY_ERROR,
                resolver->syntax->expressions[expression_id].span,
                "expression structure exceeds the semantic traversal limit of 256"
            );
            resolver->traversal_limit_reported = true;
        }
        return;
    }
    resolver->expression_states[expression_id] = 1;
    ++resolver->traversal_depth;
    const SolExpr *expression = &resolver->syntax->expressions[expression_id];
    switch (expression->kind) {
        case SOL_EXPR_PATH: {
            SolResolution resolution = sol_resolver_lookup(resolver, expression->as.name);
            resolver->module->resolutions[expression_id] = resolution;
            if (resolution.kind == SOL_RESOLUTION_ERROR) {
                sol_diagnostics_add(
                    resolver->diagnostics,
                    "SOL-RESOLVE-002",
                    SOL_SEVERITY_ERROR,
                    expression->as.name,
                    "unresolved name"
                );
            }
            break;
        }
        case SOL_EXPR_UNARY:
            sol_resolver_expression(resolver, expression->as.unary.operand);
            break;
        case SOL_EXPR_BINARY:
            sol_resolver_expression(resolver, expression->as.binary.left);
            sol_resolver_expression(resolver, expression->as.binary.right);
            break;
        case SOL_EXPR_CALL:
            sol_resolver_expression(resolver, expression->as.call.callee);
            sol_resolver_arguments(resolver, expression->as.call.first_argument);
            break;
        case SOL_EXPR_FIELD:
            {
                SolExprId root = sol_resolver_field_root(resolver, expression_id);
                if (root < resolver->syntax->expression_count
                    && resolver->syntax->expressions[root].kind == SOL_EXPR_PATH) {
                    SolSpan base_name = resolver->syntax->expressions[root].as.name;
                    SolResolution base_resolution = sol_resolver_lookup(resolver, base_name);
                    if (base_resolution.kind == SOL_RESOLUTION_LOCAL) {
                        sol_resolver_expression(resolver, expression->as.field.base);
                        break;
                    }
                }
            }
            resolver->module->resolutions[expression_id] = sol_resolver_lookup_definition(
                resolver,
                expression->span
            );
            if (resolver->module->resolutions[expression_id].kind == SOL_RESOLUTION_ERROR) {
                resolver->module->resolutions[expression_id] = (SolResolution){
                    .kind = SOL_RESOLUTION_NOT_APPLICABLE,
                };
                sol_resolver_expression(resolver, expression->as.field.base);
            }
            break;
        case SOL_EXPR_RECORD:
            sol_resolver_expression(resolver, expression->as.record.type);
            sol_resolver_arguments(resolver, expression->as.record.first_field);
            break;
        case SOL_EXPR_IF:
            sol_resolver_expression(resolver, expression->as.if_expr.condition);
            sol_resolver_expression(resolver, expression->as.if_expr.then_branch);
            sol_resolver_expression(resolver, expression->as.if_expr.else_branch);
            break;
        case SOL_EXPR_MATCH:
            sol_resolver_match(resolver, expression);
            break;
        case SOL_EXPR_BLOCK:
            sol_resolver_block(resolver, expression);
            break;
        case SOL_EXPR_PROPAGATE:
            sol_resolver_expression(resolver, expression->as.propagated);
            break;
        case SOL_EXPR_HANDLE:
            sol_resolver_expression(resolver, expression->as.handle.authority);
            sol_resolver_expression(resolver, expression->as.handle.provider);
            sol_resolver_expression(resolver, expression->as.handle.body);
            break;
        case SOL_EXPR_RESULT:
            break;
        case SOL_EXPR_OLD:
            sol_resolver_expression(resolver, expression->as.old_expression);
            break;
        default:
            break;
    }
    --resolver->traversal_depth;
    resolver->expression_states[expression_id] = 2;
}

static void sol_resolver_collect_definitions(SolResolver *resolver) {
    for (size_t index = 0; index < resolver->syntax->item_count; ++index) {
        const SolSyntaxItem *item = &resolver->syntax->items[index];
        for (size_t previous = 0; previous < index; ++previous) {
            if (sol_path_span_equal(
                resolver->source,
                resolver->module->definitions[previous].name,
                item->name
            )) {
                sol_diagnostics_add(
                    resolver->diagnostics,
                    "SOL-RESOLVE-001",
                    SOL_SEVERITY_ERROR,
                    item->name,
                    "duplicate declaration in module"
                );
                break;
            }
        }
        resolver->module->definitions[index] = (SolHirDefinition){
            .kind = item->kind,
            .name = item->name,
            .syntax_item = index,
        };
    }
}

static void sol_resolver_bind_parameters(
    SolResolver *resolver,
    SolParameterId parameter_id
) {
    size_t traversed = 0;
    while (parameter_id != SOL_AST_NONE) {
        if (parameter_id >= resolver->syntax->parameter_count
            || traversed++ >= resolver->syntax->parameter_count) {
            sol_resolver_malformed(resolver);
            break;
        }
        const SolParameter *parameter = &resolver->syntax->parameters[parameter_id];
        sol_resolver_add_binding(
            resolver,
            parameter->name,
            SOL_LOCAL_PARAMETER,
            parameter_id
        );
        parameter_id = parameter->next;
    }
}

static void sol_resolver_contracts(
    SolResolver *resolver,
    SolContractClauseId clause_id,
    SolParameterId first_parameter
) {
    size_t clause_count = 0;
    while (clause_id != SOL_AST_NONE) {
        if (clause_id >= resolver->syntax->contract_clause_count
            || clause_count++ >= resolver->syntax->contract_clause_count) {
            sol_resolver_malformed(resolver);
            return;
        }
        const SolContractClause *clause
            = &resolver->syntax->contract_clauses[clause_id];
        SolContractConditionId condition_id = clause->first_condition;
        size_t condition_count = 0;
        while (condition_id != SOL_AST_NONE) {
            if (condition_id >= resolver->syntax->contract_condition_count
                || condition_count++ >= resolver->syntax->contract_condition_count) {
                sol_resolver_malformed(resolver);
                return;
            }
            resolver->binding_count = 0;
            resolver->scope_depth = 0;
            sol_resolver_bind_parameters(resolver, first_parameter);
            sol_resolver_expression(
                resolver,
                resolver->syntax->contract_conditions[condition_id].expression
            );
            condition_id = resolver->syntax->contract_conditions[condition_id].next;
        }
        clause_id = clause->next;
    }
    resolver->binding_count = 0;
}

static void sol_resolver_function(SolResolver *resolver, SolDefId definition) {
    const SolSyntaxItem *item = &resolver->syntax->items[definition];
    resolver->current_definition = definition;
    sol_resolver_contracts(resolver, item->first_contract, item->first_parameter);
    resolver->binding_count = 0;
    resolver->scope_depth = 0;
    sol_resolver_bind_parameters(resolver, item->first_parameter);
    if (item->body != SOL_AST_NONE) {
        sol_resolver_expression(resolver, item->body);
    }
    resolver->binding_count = 0;
}

static void sol_resolver_capability_members(SolResolver *resolver, SolDefId definition) {
    const SolSyntaxItem *item = &resolver->syntax->items[definition];
    SolCapabilityMemberId member_id = item->first_member;
    while (member_id != SOL_AST_NONE) {
        const SolCapabilityMember *member = &resolver->syntax->capability_members[member_id];
        resolver->current_definition = definition;
        sol_resolver_contracts(
            resolver,
            member->first_contract,
            member->first_parameter
        );
        resolver->binding_count = 0;
        resolver->scope_depth = 0;
        if (item->capability_source != SOL_AST_NONE) {
            sol_resolver_add_binding(
                resolver,
                resolver->syntax->parameters[item->capability_source].name,
                SOL_LOCAL_PARAMETER,
                item->capability_source
            );
        }
        sol_resolver_bind_parameters(resolver, member->first_parameter);
        if (member->body != SOL_AST_NONE) {
            sol_resolver_expression(resolver, member->body);
        }
        resolver->binding_count = 0;
        member_id = member->next;
    }
}

bool sol_hir_lower(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    SolHirModule *module,
    SolDiagnostics *diagnostics
) {
    SolResolver resolver = {
        .source = source,
        .syntax = syntax,
        .module = module,
        .diagnostics = diagnostics,
    };
    if (!sol_resolver_validate(&resolver)) {
        return false;
    }
    if (!sol_resolver_allocate(&resolver)) {
        sol_diagnostics_add(
            diagnostics,
            "SOL-INTERNAL-001",
            SOL_SEVERITY_ERROR,
            (SolSpan){0},
            "out of memory while lowering semantic HIR"
        );
        free(resolver.bindings);
        free(resolver.expression_states);
        return false;
    }

    sol_resolver_collect_definitions(&resolver);
    for (size_t index = 0; index < syntax->item_count; ++index) {
        if (syntax->items[index].kind == SOL_ITEM_FUNCTION) {
            sol_resolver_function(&resolver, index);
        } else if (syntax->items[index].kind == SOL_ITEM_CAPABILITY) {
            sol_resolver_capability_members(&resolver, index);
        }
    }

    free(resolver.bindings);
    free(resolver.expression_states);
    return !resolver.allocation_failed
        && !resolver.malformed
        && !diagnostics->allocation_failed;
}
