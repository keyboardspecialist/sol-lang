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
    SolCapabilityMemberId current_member;
    SolTraitMethodId current_trait_method;
    SolType contextual_self;
    SolType expected_return;
    SolType contextual_expected;
    SolContractClauseKind contract_kind;
    SolContractOutcomeKind contract_outcome;
    SolType contract_result;
    bool in_contract;
    bool in_old;
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
static bool sol_type_span_equal(const SolSource *source, SolSpan span, const char *text);

static bool sol_type_authority_free_effect(
    const SolTypeChecker *checker,
    const SolEffect *effect
) {
    return !effect->has_argument
        && (sol_type_span_equal(checker->source, effect->name, "panic")
            || sol_type_span_equal(checker->source, effect->name, "diverge"));
}

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

static void sol_type_malformed_effect(SolTypeChecker *checker, SolSpan span) {
    if (!checker->malformed) {
        sol_type_error(
            checker,
            "SOL-INTERNAL-004",
            span,
            "malformed effect metadata passed to type checking"
        );
    }
    checker->malformed = true;
}

static bool sol_type_span_valid(const SolSource *source, SolSpan span) {
    return source->text != NULL && span.start <= span.end && span.end <= source->length;
}

static bool sol_type_validate_trait_method_links(SolTypeChecker *checker) {
    const SolSyntaxTree *syntax = checker->syntax;
    unsigned char *seen = calloc(syntax->trait_method_count, 1);
    if (syntax->trait_method_count != 0 && seen == NULL) {
        checker->allocation_failed = true;
        return false;
    }
    bool valid = true;
    for (size_t item = 0; valid && item < syntax->item_count; ++item) {
        SolItemKind kind = syntax->items[item].kind;
        SolTraitMethodId method = syntax->items[item].first_trait_method;
        if (kind != SOL_ITEM_TRAIT && kind != SOL_ITEM_IMPLEMENTATION) {
            valid = method == SOL_AST_NONE;
            continue;
        }
        size_t traversed = 0;
        while (method != SOL_AST_NONE) {
            if (method >= syntax->trait_method_count
                || traversed++ >= syntax->trait_method_count
                || seen[method] != 0
                || syntax->trait_methods[method].owner_item != item) {
                valid = false;
                break;
            }
            seen[method] = 1;
            method = syntax->trait_methods[method].next;
        }
    }
    for (size_t method = 0; valid && method < syntax->trait_method_count; ++method) {
        if (seen[method] == 0) valid = false;
    }
    free(seen);
    if (!valid) sol_type_malformed(checker);
    return valid;
}

static int sol_type_path_next_byte(
    const SolSource *source,
    SolSpan span,
    size_t *cursor
) {
    while (*cursor < span.end) {
        unsigned char byte = (unsigned char)source->text[*cursor];
        if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n') {
            ++*cursor;
            continue;
        }
        if (byte == '/' && *cursor + 1 < span.end && source->text[*cursor + 1] == '/') {
            *cursor += 2;
            while (*cursor < span.end && source->text[*cursor] != '\r'
                && source->text[*cursor] != '\n') ++*cursor;
            continue;
        }
        if (byte == '/' && *cursor + 1 < span.end && source->text[*cursor + 1] == '*') {
            *cursor += 2;
            size_t depth = 1;
            while (*cursor < span.end && depth != 0) {
                if (*cursor + 1 < span.end && source->text[*cursor] == '/'
                    && source->text[*cursor + 1] == '*') {
                    ++depth;
                    *cursor += 2;
                } else if (*cursor + 1 < span.end && source->text[*cursor] == '*'
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

static bool sol_type_path_equal(const SolSource *source, SolSpan left, SolSpan right) {
    size_t left_cursor = left.start;
    size_t right_cursor = right.start;
    for (;;) {
        int left_byte = sol_type_path_next_byte(source, left, &left_cursor);
        int right_byte = sol_type_path_next_byte(source, right, &right_cursor);
        if (left_byte != right_byte) return false;
        if (left_byte < 0) return true;
    }
}

static SolEffectId sol_type_effect_root(
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
        case SOL_EFFECT_OWNER_TRAIT_METHOD:
            return effect->owner < syntax->trait_method_count
                ? syntax->trait_methods[effect->owner].first_effect
                : SOL_AST_NONE;
        case SOL_EFFECT_OWNER_TYPE:
            return effect->owner < syntax->type_count
                ? syntax->types[effect->owner].first_effect
                : SOL_AST_NONE;
    }
    return SOL_AST_NONE;
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
        || syntax->type_parameter_count > syntax->type_parameter_capacity
        || syntax->effect_parameter_count > syntax->effect_parameter_capacity
        || syntax->field_count > syntax->field_capacity
        || syntax->variant_count > syntax->variant_capacity
        || syntax->pattern_count > syntax->pattern_capacity
        || syntax->pattern_binding_count > syntax->pattern_binding_capacity
        || syntax->match_arm_count > syntax->match_arm_capacity
        || syntax->effect_count > syntax->effect_capacity
        || syntax->capability_member_count > syntax->capability_member_capacity
        || syntax->trait_method_count > syntax->trait_method_capacity
        || syntax->contract_clause_count > syntax->contract_clause_capacity
        || syntax->contract_condition_count > syntax->contract_condition_capacity
        || (syntax->item_count != 0 && syntax->items == NULL)
        || (syntax->parameter_count != 0 && syntax->parameters == NULL)
        || (syntax->argument_count != 0 && syntax->arguments == NULL)
        || (syntax->statement_count != 0 && syntax->statements == NULL)
        || (syntax->expression_count != 0 && syntax->expressions == NULL)
        || (syntax->type_count != 0 && syntax->types == NULL)
        || (syntax->type_argument_count != 0 && syntax->type_arguments == NULL)
        || (syntax->type_parameter_count != 0 && syntax->type_parameters == NULL)
        || (syntax->effect_parameter_count != 0 && syntax->effect_parameters == NULL)
        || (syntax->field_count != 0 && syntax->fields == NULL)
        || (syntax->variant_count != 0 && syntax->variants == NULL)
        || (syntax->pattern_count != 0 && syntax->patterns == NULL)
        || (syntax->pattern_binding_count != 0 && syntax->pattern_bindings == NULL)
        || (syntax->match_arm_count != 0 && syntax->match_arms == NULL)
        || (syntax->effect_count != 0 && syntax->effects == NULL)
        || (syntax->capability_member_count != 0
            && syntax->capability_members == NULL)
        || (syntax->trait_method_count != 0 && syntax->trait_methods == NULL)
        || (syntax->contract_clause_count != 0 && syntax->contract_clauses == NULL)
        || (syntax->contract_condition_count != 0
            && syntax->contract_conditions == NULL)
        || hir->definition_count != syntax->item_count
        || hir->resolution_count != syntax->expression_count
        || hir->type_resolution_count != syntax->type_count
        || hir->effect_resolution_count != syntax->effect_count
        || hir->type_effect_resolution_count != syntax->type_count
        || hir->trait_resolution_count != syntax->item_count
        || hir->bound_resolution_count != syntax->type_parameter_count
        || hir->local_count > hir->local_capacity
        || syntax->parameter_count > SIZE_MAX - syntax->argument_count
        || (hir->definition_count != 0 && hir->definitions == NULL)
        || (hir->resolution_count != 0 && hir->resolutions == NULL)
        || (hir->resolution_count != 0 && hir->expression_owners == NULL)
        || (hir->type_resolution_count != 0 && hir->type_resolutions == NULL)
        || (hir->effect_resolution_count != 0 && hir->effect_resolutions == NULL)
        || (hir->type_effect_resolution_count != 0
            && hir->type_effect_resolutions == NULL)
        || (hir->trait_resolution_count != 0 && hir->trait_resolutions == NULL)
        || (hir->bound_resolution_count != 0 && hir->bound_resolutions == NULL)
        || (hir->local_count != 0 && hir->locals == NULL)) {
        sol_type_malformed(checker);
        return false;
    }
    if (!sol_type_validate_trait_method_links(checker)) return false;
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
            || (item->first_contract != SOL_AST_NONE
                && item->first_contract >= syntax->contract_clause_count)
            || (item->capability_source != SOL_AST_NONE
                && item->capability_source >= syntax->parameter_count)
            || (item->first_type_parameter != SOL_AST_NONE
                && item->first_type_parameter >= syntax->type_parameter_count)
            || (item->first_effect_parameter != SOL_AST_NONE
                && item->first_effect_parameter >= syntax->effect_parameter_count)
            || (item->first_trait_method != SOL_AST_NONE
                && item->first_trait_method >= syntax->trait_method_count)
            || (item->kind == SOL_ITEM_TYPE
                && item->representation_type >= syntax->type_count)
            || (item->kind == SOL_ITEM_IMPLEMENTATION
                && item->implementation_type >= syntax->type_count)
            || definition->syntax_item != index
            || definition->kind != item->kind
            || !sol_type_span_valid(checker->source, definition->name)) {
            sol_type_malformed(checker);
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
        if (item->result_authority_parameter != SOL_AST_NONE) {
            SolParameterId parameter = item->first_parameter;
            size_t traversed = 0;
            while (parameter != SOL_AST_NONE
                && parameter != item->result_authority_parameter) {
                if (parameter >= syntax->parameter_count
                    || traversed++ >= syntax->parameter_count) {
                    sol_type_malformed(checker);
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
                sol_type_malformed(checker);
                return false;
            }
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
        if ((int)type->kind < 0 || type->kind > SOL_SYNTAX_TYPE_FUNCTION
            || !sol_type_span_valid(checker->source, type->span)
            || !sol_type_span_valid(checker->source, type->name)
            || type->owner_item >= syntax->item_count
            || (type->has_effect_tail
                && (type->kind != SOL_SYNTAX_TYPE_FUNCTION
                    || !sol_type_span_valid(checker->source, type->effect_tail)
                    || type->effect_tail.start == type->effect_tail.end
                    || type->first_effect != SOL_AST_NONE))
            || (!type->has_effect_tail
                && (type->effect_tail.start != 0 || type->effect_tail.end != 0))
            || (type->first_argument != SOL_AST_NONE
                && type->first_argument >= syntax->type_argument_count)
            || (type->kind == SOL_SYNTAX_TYPE_FUNCTION
                && (type->return_type >= syntax->type_count
                    || (type->first_effect != SOL_AST_NONE
                        && type->first_effect >= syntax->effect_count)))) {
            sol_type_malformed(checker);
            return false;
        }
        if (type->kind == SOL_SYNTAX_TYPE_FUNCTION && type->first_effect != SOL_AST_NONE
            && (syntax->effects[type->first_effect].owner_kind != SOL_EFFECT_OWNER_TYPE
                || syntax->effects[type->first_effect].owner != index)) {
            sol_type_malformed(checker);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->type_parameter_count; ++index) {
        const SolTypeParameter *parameter = &syntax->type_parameters[index];
        if (!sol_type_span_valid(checker->source, parameter->name)
            || !sol_type_span_valid(checker->source, parameter->bound)
            || parameter->owner_item >= syntax->item_count
            || (parameter->next != SOL_AST_NONE
                && parameter->next >= syntax->type_parameter_count)) {
            sol_type_malformed(checker);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->effect_parameter_count; ++index) {
        const SolEffectParameter *parameter = &syntax->effect_parameters[index];
        if (!sol_type_span_valid(checker->source, parameter->name)
            || parameter->owner_item >= syntax->item_count
            || syntax->items[parameter->owner_item].kind != SOL_ITEM_FUNCTION
            || syntax->items[parameter->owner_item].first_effect_parameter != index
            || parameter->next != SOL_AST_NONE) {
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
            || (int)effect->owner_kind < 0
            || effect->owner_kind > SOL_EFFECT_OWNER_TYPE
            || (effect->owner_kind == SOL_EFFECT_OWNER_ITEM
                && (effect->owner >= syntax->item_count
                    || syntax->items[effect->owner].kind != SOL_ITEM_FUNCTION))
            || (effect->owner_kind == SOL_EFFECT_OWNER_CAPABILITY_MEMBER
                && effect->owner >= syntax->capability_member_count)
            || (effect->owner_kind == SOL_EFFECT_OWNER_TRAIT_METHOD
                && effect->owner >= syntax->trait_method_count)
            || (effect->owner_kind == SOL_EFFECT_OWNER_TYPE
                && effect->owner >= syntax->type_count)) {
            sol_type_malformed(checker);
            return false;
        }
        bool zero_argument = effect->argument.start == 0 && effect->argument.end == 0;
        bool pure_spelling = sol_type_span_equal(checker->source, effect->name, "pure");
        if (effect->name.start == effect->name.end
            || effect->span.start != effect->name.start
            || (effect->has_argument
                ? (effect->argument.start == effect->argument.end
                    || effect->name.end >= effect->argument.start
                    || effect->argument.end >= effect->span.end)
                : (!zero_argument || effect->span.end != effect->name.end))
            || effect->is_pure != pure_spelling
            ) {
            sol_type_malformed_effect(checker, effect->span);
            return false;
        }
        SolEffectId linked = sol_type_effect_root(syntax, effect);
        size_t traversed = 0;
        bool found = false;
        while (linked != SOL_AST_NONE) {
            if (linked >= syntax->effect_count
                || traversed++ >= syntax->effect_count
                || syntax->effects[linked].owner_kind != effect->owner_kind
                || syntax->effects[linked].owner != effect->owner) {
                sol_type_malformed(checker);
                return false;
            }
            if (linked == index) found = true;
            linked = syntax->effects[linked].next;
        }
        if (!found) {
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
            || (member->body != SOL_AST_NONE
                && member->body >= syntax->expression_count)
            || (member->next != SOL_AST_NONE
                && member->next >= syntax->capability_member_count)
            || (member->first_contract != SOL_AST_NONE
                && member->first_contract >= syntax->contract_clause_count)
            || member->owner_item >= syntax->item_count
            || syntax->items[member->owner_item].kind != SOL_ITEM_CAPABILITY) {
            sol_type_malformed(checker);
            return false;
        }
        if ((syntax->items[member->owner_item].capability_source != SOL_AST_NONE)
            != (member->body != SOL_AST_NONE)) {
            sol_type_malformed(checker);
            return false;
        }
        if (member->result_authority_from_self
            && !syntax->types[member->return_type_id].is_capability) {
            sol_type_malformed(checker);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->trait_method_count; ++index) {
        const SolTraitMethod *method = &syntax->trait_methods[index];
        if (!sol_type_span_valid(checker->source, method->name)
            || !sol_type_span_valid(checker->source, method->span)
            || method->owner_item >= syntax->item_count
            || method->return_type_id >= syntax->type_count
            || (method->first_parameter != SOL_AST_NONE
                && method->first_parameter >= syntax->parameter_count)
            || (method->first_effect != SOL_AST_NONE
                && method->first_effect >= syntax->effect_count)
            || (method->body != SOL_AST_NONE
                && method->body >= syntax->expression_count)
            || (method->next != SOL_AST_NONE
                && method->next >= syntax->trait_method_count)) {
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
            && expression->kind <= SOL_EXPR_TYPE_APPLICATION
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
            case SOL_EXPR_TYPE_APPLICATION:
                valid = valid
                    && expression->as.type_application.base < syntax->expression_count
                    && expression->as.type_application.first_argument
                        < syntax->type_argument_count;
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
            case SOL_EXPR_HANDLE:
                valid = valid
                    && sol_type_span_valid(
                        checker->source,
                        expression->as.handle.effect_name
                    )
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
        SolResolution resolution = hir->resolutions[index];
        valid = valid
            && (int)resolution.kind >= 0
            && resolution.kind <= SOL_RESOLUTION_REFINEMENT_SELF;
        if (resolution.kind == SOL_RESOLUTION_DEFINITION) {
            valid = valid && resolution.target < hir->definition_count;
        } else if (resolution.kind == SOL_RESOLUTION_LOCAL) {
            valid = valid && resolution.target < hir->local_count;
        } else if (resolution.kind == SOL_RESOLUTION_BUILTIN) {
            valid = valid && resolution.target <= SOL_BUILTIN_NONE;
        } else if (resolution.kind == SOL_RESOLUTION_REFINEMENT_SELF) {
            valid = valid && resolution.target < syntax->item_count
                && syntax->items[resolution.target].kind == SOL_ITEM_TYPE
                && syntax->items[resolution.target].flavor == SOL_TYPE_DECLARATION_REFINED
                && hir->expression_owners[index] == resolution.target;
        }
        if (!valid) {
            sol_type_malformed(checker);
            return false;
        }
        if (hir->expression_owners[index] >= hir->definition_count) {
            sol_type_malformed(checker);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->type_count; ++index) {
        SolTypeResolution resolution = hir->type_resolutions[index];
        const SolSyntaxType *type = &syntax->types[index];
        if ((int)resolution.kind < 0 || resolution.kind > SOL_TYPE_RESOLUTION_SELF
            || (resolution.kind == SOL_TYPE_RESOLUTION_BUILTIN
                && resolution.target > SOL_TYPE_BUILTIN_RESULT)
            || (resolution.kind == SOL_TYPE_RESOLUTION_DEFINITION
                && resolution.target >= hir->definition_count)
            || (resolution.kind == SOL_TYPE_RESOLUTION_PARAMETER
                && resolution.target >= syntax->type_parameter_count)
            || (resolution.kind == SOL_TYPE_RESOLUTION_SELF
                && (resolution.target >= syntax->item_count
                    || (syntax->items[resolution.target].kind != SOL_ITEM_TRAIT
                        && syntax->items[resolution.target].kind
                            != SOL_ITEM_IMPLEMENTATION)))) {
            sol_type_malformed(checker);
            return false;
        }
        if (type->kind != SOL_SYNTAX_TYPE_PATH) {
            if (resolution.kind != SOL_TYPE_RESOLUTION_ERROR) {
                sol_type_malformed(checker);
                return false;
            }
            continue;
        }
        if (resolution.kind == SOL_TYPE_RESOLUTION_PARAMETER) {
            const SolTypeParameter *parameter
                = &syntax->type_parameters[resolution.target];
            if (parameter->owner_item != type->owner_item
                || !sol_type_path_equal(checker->source, parameter->name, type->name)) {
                sol_type_malformed(checker);
                return false;
            }
        } else if (resolution.kind == SOL_TYPE_RESOLUTION_DEFINITION) {
            const SolHirDefinition *definition = &hir->definitions[resolution.target];
                if ((definition->kind != SOL_ITEM_RECORD
                    && definition->kind != SOL_ITEM_ENUM
                    && definition->kind != SOL_ITEM_TYPE
                    && definition->kind != SOL_ITEM_CAPABILITY)
                || !sol_type_path_equal(checker->source, definition->name, type->name)) {
                sol_type_malformed(checker);
                return false;
            }
        } else if (resolution.kind == SOL_TYPE_RESOLUTION_BUILTIN) {
            const char *name = resolution.target == SOL_TYPE_BUILTIN_INT64
                ? "Int64"
                : resolution.target == SOL_TYPE_BUILTIN_BOOL
                    ? "Bool"
                    : resolution.target == SOL_TYPE_BUILTIN_TEXT
                        ? "Text"
                        : resolution.target == SOL_TYPE_BUILTIN_OPTION
                            ? "Option"
                            : "Result";
            if (!sol_type_span_equal(checker->source, type->name, name)) {
                sol_type_malformed(checker);
                return false;
            }
        } else if (resolution.kind == SOL_TYPE_RESOLUTION_ERROR) {
            bool resolvable = sol_type_span_equal(checker->source, type->name, "Int64")
                || sol_type_span_equal(checker->source, type->name, "Bool")
                || sol_type_span_equal(checker->source, type->name, "Text")
                || sol_type_span_equal(checker->source, type->name, "Option")
                || sol_type_span_equal(checker->source, type->name, "Result");
            SolTypeParameterId parameter
                = syntax->items[type->owner_item].first_type_parameter;
            size_t traversed = 0;
            while (!resolvable && parameter != SOL_AST_NONE) {
                if (parameter >= syntax->type_parameter_count
                    || traversed++ >= syntax->type_parameter_count) {
                    sol_type_malformed(checker);
                    return false;
                }
                resolvable = sol_type_path_equal(
                    checker->source,
                    syntax->type_parameters[parameter].name,
                    type->name
                );
                parameter = syntax->type_parameters[parameter].next;
            }
            for (size_t definition = 0;
                !resolvable && definition < hir->definition_count;
                ++definition) {
                SolItemKind kind = hir->definitions[definition].kind;
                resolvable = (kind == SOL_ITEM_RECORD || kind == SOL_ITEM_ENUM
                        || kind == SOL_ITEM_TYPE
                        || kind == SOL_ITEM_CAPABILITY)
                    && sol_type_path_equal(
                        checker->source,
                        hir->definitions[definition].name,
                        type->name
                    );
            }
            if (resolvable) {
                sol_type_malformed(checker);
                return false;
            }
        } else if (!sol_type_span_equal(checker->source, type->name, "Self")) {
            sol_type_malformed(checker);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->effect_count; ++index) {
        const SolEffect *effect = &syntax->effects[index];
        SolEffectResolution resolution = hir->effect_resolutions[index];
        SolDefId owner = SOL_AST_NONE;
        if (effect->owner_kind == SOL_EFFECT_OWNER_ITEM) {
            owner = effect->owner;
        } else if (effect->owner_kind == SOL_EFFECT_OWNER_TYPE
            && effect->owner < syntax->type_count) {
            owner = syntax->types[effect->owner].owner_item;
        }
        SolEffectParameterId parameter = owner < syntax->item_count
            ? syntax->items[owner].first_effect_parameter
            : SOL_AST_NONE;
        bool parameter_use = !effect->is_pure && parameter != SOL_AST_NONE
            && sol_type_path_equal(
                checker->source,
                syntax->effect_parameters[parameter].name,
                effect->name
            );
        if ((int)resolution.kind < 0 || resolution.kind > SOL_EFFECT_RESOLUTION_ERROR
            || (parameter_use
                && (resolution.kind != SOL_EFFECT_RESOLUTION_PARAMETER
                    || resolution.target != parameter))
            || (!parameter_use
                && (resolution.kind != SOL_EFFECT_RESOLUTION_ATOM
                    || resolution.target != SOL_AST_NONE))) {
            sol_type_malformed(checker);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->type_count; ++index) {
        const SolSyntaxType *type = &syntax->types[index];
        SolEffectResolution resolution = hir->type_effect_resolutions[index];
        SolEffectParameterId parameter = syntax->items[
            type->owner_item
        ].first_effect_parameter;
        bool parameter_use = type->has_effect_tail && parameter != SOL_AST_NONE
            && sol_type_path_equal(
                checker->source,
                syntax->effect_parameters[parameter].name,
                type->effect_tail
            );
        if ((int)resolution.kind < 0 || resolution.kind > SOL_EFFECT_RESOLUTION_ERROR
            || (parameter_use
                && (resolution.kind != SOL_EFFECT_RESOLUTION_PARAMETER
                    || resolution.target != parameter))
            || (!parameter_use
                && (resolution.kind != SOL_EFFECT_RESOLUTION_ERROR
                    || resolution.target != SOL_AST_NONE))) {
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
    for (size_t index = 0; index < hir->trait_resolution_count; ++index) {
        SolResolution resolution = hir->trait_resolutions[index];
        bool implementation = syntax->items[index].kind == SOL_ITEM_IMPLEMENTATION;
        if ((!implementation && (resolution.kind != SOL_RESOLUTION_NOT_APPLICABLE
                || resolution.target != SOL_AST_NONE))
            || (implementation && (resolution.kind != SOL_RESOLUTION_DEFINITION
                || resolution.target >= syntax->item_count
                || syntax->items[resolution.target].kind != SOL_ITEM_TRAIT))) {
            sol_type_malformed(checker);
            return false;
        }
    }
    for (size_t index = 0; index < hir->bound_resolution_count; ++index) {
        SolResolution resolution = hir->bound_resolutions[index];
        bool bounded = syntax->type_parameters[index].bound.start
            != syntax->type_parameters[index].bound.end;
        if ((!bounded && (resolution.kind != SOL_RESOLUTION_NOT_APPLICABLE
                || resolution.target != SOL_AST_NONE))
            || (bounded && (resolution.kind != SOL_RESOLUTION_DEFINITION
                || resolution.target >= syntax->item_count
                || syntax->items[resolution.target].kind != SOL_ITEM_TRAIT))) {
            sol_type_malformed(checker);
            return false;
        }
    }
    if (!sol_syntax_contracts_validate(checker->source, syntax)) {
        sol_type_malformed(checker);
        return false;
    }
    return true;
}

void sol_type_table_init(SolTypeTable *table) {
    memset(table, 0, sizeof(*table));
}

void sol_type_table_free(SolTypeTable *table) {
    free(table->expressions);
    free(table->expression_capability_origins);
    free(table->expression_operation_origins);
    free(table->locals);
    free(table->local_capability_origins);
    free(table->local_operation_origins);
    free(table->definitions);
    free(table->declared_types);
    free(table->type_applications);
    free(table->type_application_arguments);
    for (size_t index = 0; index < table->function_type_count; ++index) {
        free(table->function_types[index].parameters);
        free(table->function_types[index].effects.atoms);
    }
    free(table->function_types);
    free(table->function_coercions);
    free(table->provenances);
    free(table->provenance_roots);
    free(table->handlers);
    free(table->call_instantiations);
    free(table->call_instantiation_arguments);
    free(table->variant_constructors);
    free(table->method_resolutions);
    free(table->field_resolutions);
    free(table->variant_resolutions);
    free(table->pattern_variant_resolutions);
    free(table->argument_field_resolutions);
    free(table->implementation_targets);
    free(table->representations);
    free(table->constructions);
    memset(table, 0, sizeof(*table));
}

const SolTypeApplication *sol_type_application(
    const SolTypeTable *table,
    SolType type
) {
    if (table == NULL || type.kind != SOL_TYPE_APPLICATION
        || type.definition >= table->type_application_count) {
        return NULL;
    }
    return &table->type_applications[type.definition];
}

bool sol_type_application_arguments(
    const SolTypeTable *table,
    SolType type,
    const SolType **arguments,
    size_t *count
) {
    if (arguments != NULL) *arguments = NULL;
    if (count != NULL) *count = 0;
    const SolTypeApplication *application = sol_type_application(table, type);
    if (application == NULL || arguments == NULL || count == NULL
        || table->type_application_argument_count
            > table->type_application_argument_capacity
        || (table->type_application_argument_capacity != 0
            && table->type_application_arguments == NULL)
        || application->argument_count == 0
        || application->argument_offset > table->type_application_argument_count
        || application->argument_count > table->type_application_argument_count
            - application->argument_offset) {
        return false;
    }
    *arguments = table->type_application_arguments + application->argument_offset;
    *count = application->argument_count;
    return true;
}

bool sol_type_call_instantiation_arguments(
    const SolTypeTable *table,
    SolExprId expression,
    const SolType **arguments,
    size_t *count
) {
    if (arguments != NULL) *arguments = NULL;
    if (count != NULL) *count = 0;
    const SolCallInstantiation *instantiation = sol_type_call_instantiation(
        table,
        expression
    );
    if (instantiation == NULL || arguments == NULL || count == NULL
        || table->call_instantiation_argument_count
            > table->call_instantiation_argument_capacity
        || (table->call_instantiation_argument_capacity != 0
            && table->call_instantiation_arguments == NULL)
        || instantiation->argument_count == 0
        || instantiation->argument_offset > table->call_instantiation_argument_count
        || instantiation->argument_count > table->call_instantiation_argument_count
            - instantiation->argument_offset) {
        return false;
    }
    *arguments = table->call_instantiation_arguments + instantiation->argument_offset;
    *count = instantiation->argument_count;
    return true;
}

const SolVariantConstructor *sol_type_variant_constructor(
    const SolTypeTable *table,
    SolType type
) {
    if (table == NULL || type.kind != SOL_TYPE_VARIANT
        || table->variant_constructor_count > table->variant_constructor_capacity
        || type.definition >= table->variant_constructor_count
        || table->variant_constructors == NULL) return NULL;
    return &table->variant_constructors[type.definition];
}

const SolTypeRepresentation *sol_type_representation(
    const SolTypeTable *table,
    SolDefId definition
) {
    if (table == NULL || table->representation_count != table->definition_count
        || definition >= table->representation_count || table->representations == NULL) {
        return NULL;
    }
    const SolTypeRepresentation *representation = &table->representations[definition];
    if ((representation->flavor != SOL_TYPE_DECLARATION_DISTINCT
            && representation->flavor != SOL_TYPE_DECLARATION_REFINED)
        || (int)representation->representation.kind < 0
        || representation->representation.kind > SOL_TYPE_TRAIT_METHOD
        || representation->representation.kind == SOL_TYPE_UNKNOWN
        || representation->representation.kind == SOL_TYPE_ERROR
        || representation->representation.kind == SOL_TYPE_NEVER) return NULL;
    return representation;
}

const SolTypeConstruction *sol_type_construction(
    const SolTypeTable *table,
    SolExprId expression
) {
    if (table == NULL || table->construction_count != table->expression_count
        || expression >= table->construction_count || table->constructions == NULL) {
        return NULL;
    }
    const SolTypeConstruction *construction = &table->constructions[expression];
    const SolTypeRepresentation *representation = construction->definition == SOL_AST_NONE
        ? NULL
        : sol_type_representation(table, construction->definition);
    if (construction->definition == SOL_AST_NONE
        || construction->definition >= table->representation_count
        || representation == NULL
        || representation->flavor != SOL_TYPE_DECLARATION_DISTINCT
        || (construction->result.kind != SOL_TYPE_NOMINAL
            && construction->result.kind != SOL_TYPE_APPLICATION)
        || (int)construction->representation.kind < 0
        || construction->representation.kind > SOL_TYPE_TRAIT_METHOD
        || construction->representation.kind == SOL_TYPE_UNKNOWN
        || construction->representation.kind == SOL_TYPE_ERROR
        || construction->representation.kind == SOL_TYPE_NEVER) return NULL;
    SolDefId result_definition = construction->result.kind == SOL_TYPE_NOMINAL
        ? construction->result.definition
        : SOL_AST_NONE;
    if (construction->result.kind == SOL_TYPE_APPLICATION) {
        const SolTypeApplication *application = sol_type_application(
            table, construction->result
        );
        if (application == NULL || application->constructor != SOL_TYPE_CONSTRUCTOR_USER) {
            return NULL;
        }
        result_definition = application->definition;
    }
    if (result_definition != construction->definition) return NULL;
    return construction;
}

bool sol_type_exact_reference_valid(
    const SolSyntaxTree *syntax,
    const SolTypeTable *table,
    SolType type
) {
    if (syntax == NULL || table == NULL || (int)type.kind < 0
        || type.kind > SOL_TYPE_TRAIT_METHOD || type.kind == SOL_TYPE_UNKNOWN
        || type.kind == SOL_TYPE_ERROR || type.kind == SOL_TYPE_NEVER) return false;
    switch (type.kind) {
        case SOL_TYPE_NOMINAL:
            return type.definition < syntax->item_count
                && syntax->items != NULL
                && (syntax->items[type.definition].kind == SOL_ITEM_RECORD
                    || syntax->items[type.definition].kind == SOL_ITEM_ENUM
                    || syntax->items[type.definition].kind == SOL_ITEM_TYPE
                    || syntax->items[type.definition].kind == SOL_ITEM_CAPABILITY);
        case SOL_TYPE_APPLICATION:
            return type.definition < table->type_application_count;
        case SOL_TYPE_FUNCTION:
            return type.definition < syntax->item_count && syntax->items != NULL
                && syntax->items[type.definition].kind == SOL_ITEM_FUNCTION;
        case SOL_TYPE_FUNCTION_SIGNATURE:
            return type.definition < table->function_type_count;
        case SOL_TYPE_CAPABILITY_OPERATION:
            return type.definition < syntax->capability_member_count;
        case SOL_TYPE_VARIANT:
            return type.definition < table->variant_constructor_count;
        case SOL_TYPE_PARAMETER:
            return type.definition < syntax->type_parameter_count;
        case SOL_TYPE_SELF:
            return type.definition < syntax->item_count;
        case SOL_TYPE_TRAIT_METHOD:
            return type.definition < syntax->trait_method_count;
        default:
            return type.definition == 0;
    }
}

const SolCallInstantiation *sol_type_call_instantiation(
    const SolTypeTable *table,
    SolExprId expression
) {
    if (table == NULL || table->call_instantiation_count != table->expression_count
        || expression >= table->call_instantiation_count
        || table->call_instantiations == NULL
        || table->call_instantiations[expression].function == SOL_AST_NONE) {
        return NULL;
    }
    return &table->call_instantiations[expression];
}

const SolMethodResolution *sol_type_method_resolution(
    const SolTypeTable *table,
    SolExprId call
) {
    if (table == NULL || table->method_resolution_count != table->expression_count
        || call >= table->method_resolution_count || table->method_resolutions == NULL
        || table->method_resolutions[call].kind == SOL_METHOD_RESOLUTION_NONE) return NULL;
    return &table->method_resolutions[call];
}

bool sol_type_provenance(
    const SolTypeTable *table,
    SolProvenanceId id,
    SolProvenance *provenance
) {
    if (provenance != NULL) *provenance = (SolProvenance){0};
    if (table == NULL || provenance == NULL
        || table->provenance_count > table->provenance_capacity
        || table->provenance_root_count > table->provenance_root_capacity
        || table->provenance_root_count > SIZE_MAX / sizeof(*table->provenance_roots)
        || id >= table->provenance_count || table->provenances == NULL
        || table->provenance_roots == NULL) {
        return false;
    }
    SolProvenanceSet set = table->provenances[id];
    if (set.root_count == 0 || set.root_offset > table->provenance_root_count
        || set.root_count > table->provenance_root_count - set.root_offset) {
        return false;
    }
    provenance->roots = table->provenance_roots + set.root_offset;
    provenance->count = set.root_count;
    return true;
}

static SolDefId sol_type_metadata_nominal_definition(
    const SolTypeTable *table, SolType type
) {
    if (type.kind == SOL_TYPE_NOMINAL) return type.definition;
    const SolTypeApplication *application = sol_type_application(table, type);
    return application != NULL && application->constructor == SOL_TYPE_CONSTRUCTOR_USER
        ? application->definition : SOL_AST_NONE;
}

static bool sol_type_argument_chain_contains(
    const SolSyntaxTree *syntax, SolArgumentId first, SolArgumentId target
) {
    size_t traversed = 0;
    while (first != SOL_AST_NONE) {
        if (first >= syntax->argument_count || traversed++ >= syntax->argument_count) {
            return false;
        }
        if (first == target) return true;
        first = syntax->arguments[first].next;
    }
    return false;
}

bool sol_type_resolution_metadata_valid(
    const SolSyntaxTree *syntax,
    const SolTypeTable *table
) {
    if (syntax == NULL || table == NULL
        || syntax->item_count > syntax->item_capacity
        || syntax->argument_count > syntax->argument_capacity
        || syntax->field_count > syntax->field_capacity
        || syntax->variant_count > syntax->variant_capacity
        || syntax->pattern_count > syntax->pattern_capacity
        || syntax->match_arm_count > syntax->match_arm_capacity
        || (syntax->item_count != 0 && syntax->items == NULL)
        || table->member_resolution_count != syntax->expression_count
        || table->pattern_resolution_count != syntax->pattern_count
        || table->argument_resolution_count != syntax->argument_count
        || (table->member_resolution_count != 0
            && (table->field_resolutions == NULL || table->variant_resolutions == NULL))
        || (table->pattern_resolution_count != 0
            && table->pattern_variant_resolutions == NULL)
        || (table->argument_resolution_count != 0
            && table->argument_field_resolutions == NULL)
        || (syntax->expression_count != 0 && syntax->expressions == NULL)
        || (syntax->argument_count != 0 && syntax->arguments == NULL)
        || (syntax->field_count != 0 && syntax->fields == NULL)
        || (syntax->variant_count != 0 && syntax->variants == NULL)
        || (syntax->pattern_count != 0 && syntax->patterns == NULL)
        || (syntax->match_arm_count != 0 && syntax->match_arms == NULL)
        || table->expression_count != syntax->expression_count
        || table->declared_type_count != syntax->type_count
        || table->type_application_count > table->type_application_capacity
        || table->type_application_argument_count
            > table->type_application_argument_capacity
        || table->variant_constructor_count > table->variant_constructor_capacity
        || (table->type_application_count != 0 && table->type_applications == NULL)
        || (table->type_application_argument_count != 0
            && table->type_application_arguments == NULL)
        || (table->variant_constructor_count != 0
            && table->variant_constructors == NULL)
        || (table->expression_count != 0 && table->expressions == NULL)) return false;
    for (size_t expression = 0; expression < syntax->expression_count; ++expression) {
        SolFieldId field = table->field_resolutions[expression];
        SolVariantId variant = table->variant_resolutions[expression];
        if (field != SOL_AST_NONE) {
            if (syntax->expressions[expression].kind != SOL_EXPR_FIELD
                || field >= syntax->field_count) return false;
            SolExprId base = syntax->expressions[expression].as.field.base;
            if (base >= table->expression_count
                || syntax->fields[field].type >= table->declared_type_count
                || sol_type_metadata_nominal_definition(
                    table, table->expressions[base]
                ) == SOL_AST_NONE) return false;
            SolDefId owner = sol_type_metadata_nominal_definition(
                table, table->expressions[base]
            );
            if (owner >= syntax->item_count || syntax->items[owner].kind != SOL_ITEM_RECORD) {
                return false;
            }
            bool found = false;
            size_t traversed = 0;
            for (SolFieldId current = syntax->items[owner].first_field;
                current != SOL_AST_NONE;
                current = syntax->fields[current].next) {
                if (current >= syntax->field_count
                    || traversed++ >= syntax->field_count) return false;
                found = found || current == field;
            }
            if (!found) return false;
        }
        if (variant != SOL_AST_NONE) {
            if (syntax->expressions[expression].kind != SOL_EXPR_FIELD
                || variant >= syntax->variant_count) return false;
            SolExprId base = syntax->expressions[expression].as.field.base;
            if (base >= table->expression_count
                || syntax->variants[variant].owner_item
                    != sol_type_metadata_nominal_definition(
                        table, table->expressions[base]
                    )) return false;
        }
    }
    for (size_t argument = 0; argument < syntax->argument_count; ++argument) {
        SolFieldId field = table->argument_field_resolutions[argument];
        if (field == SOL_AST_NONE) continue;
        if (field >= syntax->field_count) return false;
        bool found_owner = false;
        for (size_t expression = 0; expression < syntax->expression_count; ++expression) {
            const SolExpr *candidate = &syntax->expressions[expression];
            SolArgumentId first = candidate->kind == SOL_EXPR_RECORD
                ? candidate->as.record.first_field
                : candidate->kind == SOL_EXPR_CALL
                    ? candidate->as.call.first_argument : SOL_AST_NONE;
            if (first == SOL_AST_NONE
                || !sol_type_argument_chain_contains(syntax, first, argument)) continue;
            SolDefId owner = SOL_AST_NONE;
            SolVariantId receiving_variant = SOL_AST_NONE;
            if (candidate->kind == SOL_EXPR_RECORD) {
                owner = sol_type_metadata_nominal_definition(
                    table, table->expressions[expression]
                );
            } else {
                SolExprId callee = candidate->as.call.callee;
                if (callee < table->expression_count
                    && table->expressions[callee].kind == SOL_TYPE_VARIANT) {
                    const SolVariantConstructor *constructor = sol_type_variant_constructor(
                        table, table->expressions[callee]
                    );
                    if (constructor != NULL && constructor->variant < syntax->variant_count) {
                        receiving_variant = constructor->variant;
                        owner = syntax->variants[constructor->variant].owner_item;
                    }
                }
            }
            if (owner == SOL_AST_NONE) continue;
            bool owner_field = false;
            if (owner < syntax->item_count && syntax->items[owner].kind == SOL_ITEM_RECORD) {
                size_t traversed = 0;
                for (SolFieldId current = syntax->items[owner].first_field;
                    current != SOL_AST_NONE;
                    current = syntax->fields[current].next) {
                    if (current >= syntax->field_count
                        || traversed++ >= syntax->field_count) return false;
                    owner_field = owner_field || current == field;
                }
            }
            if (!owner_field && receiving_variant != SOL_AST_NONE) {
                size_t traversed = 0;
                for (SolFieldId current = syntax->variants[receiving_variant].first_field;
                    current != SOL_AST_NONE;
                    current = syntax->fields[current].next) {
                    if (current >= syntax->field_count
                        || traversed++ >= syntax->field_count) return false;
                    owner_field = owner_field || current == field;
                }
            }
            found_owner = found_owner || owner_field;
        }
        if (!found_owner) return false;
    }
    for (size_t pattern = 0; pattern < syntax->pattern_count; ++pattern) {
        SolVariantId variant = table->pattern_variant_resolutions[pattern];
        if (variant == SOL_AST_NONE) continue;
        if (variant >= syntax->variant_count
            || syntax->patterns[pattern].kind != SOL_PATTERN_VARIANT) return false;
        bool found = false;
        for (size_t arm = 0; arm < syntax->match_arm_count; ++arm) {
            if (syntax->match_arms[arm].pattern != pattern) continue;
            for (size_t expression = 0; expression < syntax->expression_count; ++expression) {
                const SolExpr *candidate = &syntax->expressions[expression];
                if (candidate->kind != SOL_EXPR_MATCH) continue;
                SolMatchArmId current = candidate->as.match_expr.first_arm;
                size_t traversed = 0;
                while (current != SOL_AST_NONE) {
                    if (current >= syntax->match_arm_count
                        || traversed++ >= syntax->match_arm_count) return false;
                    if (current == arm) {
                        SolExprId scrutinee = candidate->as.match_expr.scrutinee;
                        found = scrutinee < table->expression_count
                            && syntax->variants[variant].owner_item
                                == sol_type_metadata_nominal_definition(
                                    table, table->expressions[scrutinee]
                                );
                    }
                    current = syntax->match_arms[current].next;
                }
            }
        }
        if (!found) return false;
    }
    return true;
}

static SolProvenanceId sol_type_intern_provenance(
    SolTypeChecker *checker,
    const SolParameterId *roots,
    size_t count
) {
    if (count == 0 || roots == NULL) return SOL_PROVENANCE_NONE;
    for (size_t index = 0; index < checker->types->provenance_count; ++index) {
        SolProvenance existing;
        if (!sol_type_provenance(checker->types, index, &existing)) {
            sol_type_malformed(checker);
            return SOL_PROVENANCE_NONE;
        }
        if (existing.count == count
            && memcmp(existing.roots, roots, count * sizeof(*roots)) == 0) {
            return index;
        }
    }
    SolTypeTable *table = checker->types;
    if (table->provenance_count == table->provenance_capacity) {
        size_t capacity = table->provenance_capacity == 0
            ? 8
            : table->provenance_capacity * 2;
        if (capacity < table->provenance_capacity
            || capacity > SIZE_MAX / sizeof(*table->provenances)) {
            checker->allocation_failed = true;
            return SOL_PROVENANCE_NONE;
        }
        SolProvenanceSet *grown = realloc(
            table->provenances,
            capacity * sizeof(*table->provenances)
        );
        if (grown == NULL) {
            checker->allocation_failed = true;
            return SOL_PROVENANCE_NONE;
        }
        table->provenances = grown;
        table->provenance_capacity = capacity;
    }
    if (count > SIZE_MAX - table->provenance_root_count) {
        checker->allocation_failed = true;
        return SOL_PROVENANCE_NONE;
    }
    size_t required = table->provenance_root_count + count;
    if (required > table->provenance_root_capacity) {
        size_t capacity = table->provenance_root_capacity == 0
            ? 8
            : table->provenance_root_capacity;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) {
                checker->allocation_failed = true;
                return SOL_PROVENANCE_NONE;
            }
            capacity *= 2;
        }
        if (capacity > SIZE_MAX / sizeof(*table->provenance_roots)) {
            checker->allocation_failed = true;
            return SOL_PROVENANCE_NONE;
        }
        SolParameterId *grown = realloc(
            table->provenance_roots,
            capacity * sizeof(*table->provenance_roots)
        );
        if (grown == NULL) {
            checker->allocation_failed = true;
            return SOL_PROVENANCE_NONE;
        }
        table->provenance_roots = grown;
        table->provenance_root_capacity = capacity;
    }
    SolProvenanceId id = table->provenance_count++;
    table->provenances[id] = (SolProvenanceSet){
        .root_offset = table->provenance_root_count,
        .root_count = count,
    };
    memcpy(
        table->provenance_roots + table->provenance_root_count,
        roots,
        count * sizeof(*roots)
    );
    table->provenance_root_count = required;
    return id;
}

static SolProvenanceId sol_type_singleton_provenance(
    SolTypeChecker *checker,
    SolParameterId root
) {
    return root == SOL_AST_NONE
        ? SOL_PROVENANCE_NONE
        : sol_type_intern_provenance(checker, &root, 1);
}

static bool sol_type_provenance_is_singleton(
    const SolTypeTable *table,
    SolProvenanceId id,
    SolParameterId root
) {
    SolProvenance provenance;
    return sol_type_provenance(table, id, &provenance)
        && provenance.count == 1 && provenance.roots[0] == root;
}

static SolProvenanceId sol_type_union_provenance(
    SolTypeChecker *checker,
    SolProvenanceId left_id,
    SolProvenanceId right_id
) {
    SolProvenance left;
    SolProvenance right;
    if (!sol_type_provenance(checker->types, left_id, &left)
        || !sol_type_provenance(checker->types, right_id, &right)
        || left.count > SIZE_MAX - right.count
        || left.count + right.count > SIZE_MAX / sizeof(SolParameterId)) {
        if (left_id != SOL_PROVENANCE_NONE && right_id != SOL_PROVENANCE_NONE) {
            sol_type_malformed(checker);
        }
        return SOL_PROVENANCE_NONE;
    }
    size_t capacity = left.count + right.count;
    SolParameterId *roots = malloc(capacity * sizeof(*roots));
    if (roots == NULL) {
        checker->allocation_failed = true;
        return SOL_PROVENANCE_NONE;
    }
    size_t left_index = 0;
    size_t right_index = 0;
    size_t count = 0;
    while (left_index < left.count || right_index < right.count) {
        SolParameterId root;
        if (right_index == right.count
            || (left_index < left.count && left.roots[left_index] < right.roots[right_index])) {
            root = left.roots[left_index++];
        } else if (left_index == left.count
            || right.roots[right_index] < left.roots[left_index]) {
            root = right.roots[right_index++];
        } else {
            root = left.roots[left_index++];
            ++right_index;
        }
        roots[count++] = root;
    }
    SolProvenanceId result = sol_type_intern_provenance(checker, roots, count);
    free(roots);
    return result;
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
                && left.kind != SOL_TYPE_APPLICATION
                && left.kind != SOL_TYPE_FUNCTION
                && left.kind != SOL_TYPE_FUNCTION_SIGNATURE
                && left.kind != SOL_TYPE_CAPABILITY_OPERATION
                && left.kind != SOL_TYPE_VARIANT
                && left.kind != SOL_TYPE_PARAMETER
                && left.kind != SOL_TYPE_SELF
                && left.kind != SOL_TYPE_TRAIT_METHOD)
            || left.definition == right.definition);
}

static bool sol_type_exact_equal(SolType left, SolType right) {
    return left.kind == right.kind && left.definition == right.definition;
}

static bool sol_type_effect_set_identity_equal(
    const SolEffectSet *left,
    const SolEffectSet *right
) {
    if (left->count != right->count
        || (left->count != 0 && (left->atoms == NULL || right->atoms == NULL))) return false;
    for (size_t index = 0; index < left->count; ++index) {
        const SolEffectAtom *left_atom = &left->atoms[index];
        bool found = false;
        for (size_t candidate = 0; candidate < right->count; ++candidate) {
            const SolEffectAtom *right_atom = &right->atoms[candidate];
            if (left_atom->name.start == right_atom->name.start
                && left_atom->name.end == right_atom->name.end
                && left_atom->argument.start == right_atom->argument.start
                && left_atom->argument.end == right_atom->argument.end
                && left_atom->span.start == right_atom->span.start
                && left_atom->span.end == right_atom->span.end
                && left_atom->argument_kind == right_atom->argument_kind
                && left_atom->parameter == right_atom->parameter) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

static bool sol_type_instantiation_parameter_index(
    const SolSyntaxTree *syntax,
    SolDefId owner,
    SolTypeParameterId target,
    size_t argument_count,
    size_t *result
) {
    if (owner >= syntax->item_count || syntax->items == NULL
        || syntax->type_parameters == NULL) return false;
    SolTypeParameterId parameter = syntax->items[owner].first_type_parameter;
    for (size_t index = 0; index < argument_count; ++index) {
        if (parameter == SOL_AST_NONE || parameter >= syntax->type_parameter_count) {
            return false;
        }
        if (parameter == target) {
            *result = index;
            return true;
        }
        parameter = syntax->type_parameters[parameter].next;
    }
    return false;
}

static bool sol_type_matches_instantiation(
    const SolSyntaxTree *syntax,
    const SolTypeTable *table,
    SolType pattern,
    SolType actual,
    SolDefId owner,
    const SolType *arguments,
    size_t argument_count,
    size_t depth
) {
    if (table->function_type_count == SIZE_MAX
        || table->type_application_count > SIZE_MAX - table->function_type_count - 1
        || depth > table->type_application_count + table->function_type_count + 1) {
        return false;
    }
    if (pattern.kind == SOL_TYPE_PARAMETER) {
        size_t index = 0;
        if (sol_type_instantiation_parameter_index(
            syntax,
            owner,
            pattern.definition,
            argument_count,
            &index
        )) return sol_type_exact_equal(arguments[index], actual);
    }
    if (!sol_type_exact_equal(pattern, actual)) {
        if (pattern.kind != actual.kind) return false;
        if (pattern.kind != SOL_TYPE_APPLICATION
            && pattern.kind != SOL_TYPE_FUNCTION_SIGNATURE) return false;
    }
    if (pattern.kind == SOL_TYPE_APPLICATION) {
        const SolTypeApplication *left = sol_type_application(table, pattern);
        const SolTypeApplication *right = sol_type_application(table, actual);
        const SolType *left_arguments = NULL;
        const SolType *right_arguments = NULL;
        size_t left_count = 0;
        size_t right_count = 0;
        if (left == NULL || right == NULL
            || left->constructor != right->constructor
            || left->definition != right->definition
            || !sol_type_application_arguments(
                table,
                pattern,
                &left_arguments,
                &left_count
            )
            || !sol_type_application_arguments(
                table,
                actual,
                &right_arguments,
                &right_count
            )
            || left_count != right_count) return false;
        for (size_t index = 0; index < left_count; ++index) {
            if (!sol_type_matches_instantiation(
                syntax,
                table,
                left_arguments[index],
                right_arguments[index],
                owner,
                arguments,
                argument_count,
                depth + 1
            )) return false;
        }
    } else if (pattern.kind == SOL_TYPE_FUNCTION_SIGNATURE) {
        if (pattern.definition >= table->function_type_count
            || actual.definition >= table->function_type_count
            || table->function_types == NULL) return false;
        const SolFunctionType *left = &table->function_types[pattern.definition];
        const SolFunctionType *right = &table->function_types[actual.definition];
        if (left->parameter_count != right->parameter_count
            || (left->parameter_count != 0
                && (left->parameters == NULL || right->parameters == NULL))
            || !sol_type_effect_set_identity_equal(&left->effects, &right->effects)) {
            return false;
        }
        for (size_t index = 0; index < left->parameter_count; ++index) {
            if (!sol_type_matches_instantiation(
                syntax,
                table,
                left->parameters[index],
                right->parameters[index],
                owner,
                arguments,
                argument_count,
                depth + 1
            )) return false;
        }
        if (!sol_type_matches_instantiation(
            syntax,
            table,
            left->result,
            right->result,
            owner,
            arguments,
            argument_count,
            depth + 1
        )) return false;
    }
    return true;
}

static bool sol_type_instantiated_argument_matches(
    const SolSyntaxTree *syntax,
    const SolTypeTable *table,
    SolType pattern,
    SolType actual,
    SolExprId expression,
    SolDefId owner,
    const SolType *arguments,
    size_t argument_count
) {
    if (sol_type_matches_instantiation(
        syntax,
        table,
        pattern,
        actual,
        owner,
        arguments,
        argument_count,
        0
    )) return true;
    for (size_t index = 0; index < table->function_coercion_count; ++index) {
        const SolFunctionCoercion *coercion = &table->function_coercions[index];
        if (coercion->expression == expression
            && sol_type_matches_instantiation(
                syntax,
                table,
                pattern,
                coercion->expected,
                owner,
                arguments,
                argument_count,
                0
            )) return true;
    }
    return false;
}

bool sol_type_call_instantiation_valid(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolTypeTable *table,
    SolExprId expression
) {
    const SolCallInstantiation *instantiation = sol_type_call_instantiation(
        table,
        expression
    );
    const SolType *arguments = NULL;
    size_t argument_count = 0;
    if (source == NULL || source->text == NULL || syntax == NULL || table == NULL
        || instantiation == NULL || syntax->expressions == NULL
        || (syntax->parameter_count != 0 && syntax->parameters == NULL)
        || (syntax->argument_count != 0 && syntax->arguments == NULL)
        || syntax->type_parameters == NULL
        || table->expressions == NULL
        || table->definitions == NULL || table->declared_types == NULL
        || table->function_coercion_count > table->function_coercion_capacity
        || (table->function_coercion_count != 0 && table->function_coercions == NULL)
        || instantiation->function >= syntax->item_count
        || instantiation->function >= table->definition_count
        || syntax->items == NULL
        || syntax->items[instantiation->function].kind != SOL_ITEM_FUNCTION
        || expression >= syntax->expression_count
        || syntax->expressions[expression].kind != SOL_EXPR_CALL
        || !sol_type_call_instantiation_arguments(
            table,
            expression,
            &arguments,
            &argument_count
        )) return false;
    const SolSyntaxItem *function = &syntax->items[instantiation->function];
    SolTypeParameterId type_parameter = function->first_type_parameter;
    for (size_t index = 0; index < argument_count; ++index) {
        if (type_parameter == SOL_AST_NONE
            || type_parameter >= syntax->type_parameter_count) return false;
        type_parameter = syntax->type_parameters[type_parameter].next;
    }
    if (type_parameter != SOL_AST_NONE) return false;
    if (!sol_type_matches_instantiation(
        syntax,
        table,
        table->definitions[instantiation->function],
        table->expressions[expression],
        instantiation->function,
        arguments,
        argument_count,
        0
    )) return false;

    size_t parameter_count = 0;
    SolParameterId parameter = function->first_parameter;
    while (parameter != SOL_AST_NONE) {
        if (parameter >= syntax->parameter_count
            || parameter_count++ >= syntax->parameter_count) return false;
        parameter = syntax->parameters[parameter].next;
    }
    bool *used = parameter_count == 0 ? NULL : calloc(parameter_count, sizeof(*used));
    if (parameter_count != 0 && used == NULL) return false;
    SolArgumentId argument = syntax->expressions[expression].as.call.first_argument;
    size_t positional = 0;
    size_t supplied = 0;
    bool valid = true;
    while (valid && argument != SOL_AST_NONE) {
        if (argument >= syntax->argument_count || supplied++ >= syntax->argument_count) {
            valid = false;
            break;
        }
        const SolArgument *actual_argument = &syntax->arguments[argument];
        size_t matched = SIZE_MAX;
        SolParameterId matched_parameter = SOL_AST_NONE;
        parameter = function->first_parameter;
        for (size_t index = 0; index < parameter_count; ++index) {
            if (parameter == SOL_AST_NONE || parameter >= syntax->parameter_count) {
                valid = false;
                break;
            }
            if ((!actual_argument->is_named && index == positional)
                || (actual_argument->is_named
                    && actual_argument->name.start <= actual_argument->name.end
                    && actual_argument->name.end <= source->length
                    && syntax->parameters[parameter].name.start
                        <= syntax->parameters[parameter].name.end
                    && syntax->parameters[parameter].name.end <= source->length
                    && sol_type_name_equal(
                        source,
                        actual_argument->name,
                        syntax->parameters[parameter].name
                    ))) {
                matched = index;
                matched_parameter = parameter;
                break;
            }
            parameter = syntax->parameters[parameter].next;
        }
        if (!actual_argument->is_named) ++positional;
        if (!valid || matched == SIZE_MAX || used[matched]
            || actual_argument->value >= table->expression_count
            || syntax->parameters[matched_parameter].type_id >= table->declared_type_count) {
            valid = false;
            break;
        }
        used[matched] = true;
        valid = sol_type_instantiated_argument_matches(
            syntax,
            table,
            table->declared_types[syntax->parameters[matched_parameter].type_id],
            table->expressions[actual_argument->value],
            actual_argument->value,
            instantiation->function,
            arguments,
            argument_count
        );
        argument = actual_argument->next;
    }
    if (supplied != parameter_count) valid = false;
    for (size_t index = 0; valid && index < parameter_count; ++index) {
        if (!used[index]) valid = false;
    }
    free(used);
    return valid;
}

static const char *sol_type_name(SolType type) {
    switch (type.kind) {
        case SOL_TYPE_INT64: return "Int64";
        case SOL_TYPE_BOOL: return "Bool";
        case SOL_TYPE_TEXT: return "Text";
        case SOL_TYPE_UNIT: return "Unit";
        case SOL_TYPE_NOMINAL: return "nominal type";
        case SOL_TYPE_APPLICATION: return "generic type";
        case SOL_TYPE_FUNCTION: return "function";
        case SOL_TYPE_FUNCTION_SIGNATURE: return "function type";
        case SOL_TYPE_CAPABILITY_OPERATION: return "capability operation";
        case SOL_TYPE_VARIANT: return "enum constructor";
        case SOL_TYPE_NEVER: return "Never";
        case SOL_TYPE_PARAMETER: return "type parameter";
        case SOL_TYPE_ERROR: return "error";
        default: return "unknown type";
    }
}

static int sol_type_effect_path_next_byte(
    const SolSource *source,
    SolSpan span,
    size_t *cursor
) {
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
                if (*cursor + 1 < span.end && source->text[*cursor] == '/'
                    && source->text[*cursor + 1] == '*') {
                    ++depth;
                    *cursor += 2;
                } else if (*cursor + 1 < span.end && source->text[*cursor] == '*'
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

static bool sol_type_effect_span_equal(
    const SolSource *source,
    SolSpan left,
    SolSpan right
) {
    size_t left_cursor = left.start;
    size_t right_cursor = right.start;
    for (;;) {
        int left_byte = sol_type_effect_path_next_byte(source, left, &left_cursor);
        int right_byte = sol_type_effect_path_next_byte(source, right, &right_cursor);
        if (left_byte != right_byte) return false;
        if (left_byte < 0) return true;
    }
}

static bool sol_type_effect_atom_equal(
    const SolTypeChecker *checker,
    const SolEffectAtom *left,
    const SolEffectAtom *right
) {
    return left->argument_kind == right->argument_kind
        && sol_type_effect_span_equal(checker->source, left->name, right->name)
        && (left->argument_kind != SOL_EFFECT_ATOM_STATIC_PATH
            || sol_type_effect_span_equal(
                checker->source,
                left->argument,
                right->argument
            ));
}

static bool sol_type_effect_set_equal(
    const SolTypeChecker *checker,
    const SolEffectSet *left,
    const SolEffectSet *right
) {
    if (left->count != right->count) return false;
    for (size_t left_index = 0; left_index < left->count; ++left_index) {
        bool found = false;
        for (size_t right_index = 0; right_index < right->count; ++right_index) {
            if (sol_type_effect_atom_equal(
                checker,
                &left->atoms[left_index],
                &right->atoms[right_index]
            )) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

static bool sol_type_normalize_effects(
    SolTypeChecker *checker,
    SolTypeId owner,
    SolEffectId effect_id,
    SolEffectSet *set
) {
    size_t entry_count = 0;
    bool saw_pure = false;
    while (effect_id != SOL_AST_NONE) {
        if (effect_id >= checker->syntax->effect_count
            || entry_count++ >= checker->syntax->effect_count) {
            sol_type_malformed(checker);
            return false;
        }
        const SolEffect *effect = &checker->syntax->effects[effect_id];
        if (effect->owner_kind != SOL_EFFECT_OWNER_TYPE || effect->owner != owner) {
            sol_type_malformed(checker);
            return false;
        }
        if (effect->is_pure) {
            saw_pure = true;
            if (effect->has_argument) {
                sol_type_error(
                    checker,
                    "SOL-EFFECT-001",
                    effect->span,
                    "pure cannot have an effect argument"
                );
            }
        } else if (checker->hir->effect_resolutions[effect_id].kind
            != SOL_EFFECT_RESOLUTION_PARAMETER) {
            SolEffectAtom atom = {
                .name = effect->name,
                .argument = effect->argument,
                .span = effect->span,
                .argument_kind = effect->has_argument
                    ? SOL_EFFECT_ATOM_STATIC_PATH
                    : SOL_EFFECT_ATOM_NO_ARGUMENT,
                .parameter = SOL_AST_NONE,
            };
            if (effect->has_argument) {
                sol_type_error(
                    checker,
                    "SOL-EFFECT-010",
                    effect->argument,
                    "static authority is unavailable in callback function types"
                );
            } else if (!sol_type_authority_free_effect(checker, effect)) {
                sol_type_error(
                    checker,
                    "SOL-EFFECT-010",
                    effect->span,
                    "callback effects require an explicit lexical capability authority"
                );
            }
            bool duplicate = false;
            for (size_t index = 0; index < set->count; ++index) {
                if (sol_type_effect_atom_equal(checker, &set->atoms[index], &atom)) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                sol_type_error(
                    checker,
                    "SOL-EFFECT-001",
                    effect->span,
                    "duplicate effect in function type row"
                );
            } else {
                if (set->count == SIZE_MAX / sizeof(*set->atoms)) {
                    checker->allocation_failed = true;
                    return false;
                }
                SolEffectAtom *grown = realloc(
                    set->atoms,
                    (set->count + 1) * sizeof(*set->atoms)
                );
                if (grown == NULL) {
                    checker->allocation_failed = true;
                    return false;
                }
                set->atoms = grown;
                set->atoms[set->count++] = atom;
            }
        }
        effect_id = effect->next;
    }
    if (saw_pure && entry_count > 1) {
        sol_type_error(
            checker,
            "SOL-EFFECT-001",
            checker->syntax->types[owner].span,
            "pure cannot be combined with other effects"
        );
    }
    return true;
}

static bool sol_type_function_equal(
    const SolTypeChecker *checker,
    const SolFunctionType *left,
    const SolFunctionType *right
) {
    if (left->parameter_count != right->parameter_count
        || left->effect_parameter != right->effect_parameter
        || !sol_type_exact_equal(left->result, right->result)
        || !sol_type_effect_set_equal(checker, &left->effects, &right->effects)) {
        return false;
    }
    for (size_t index = 0; index < left->parameter_count; ++index) {
        if (!sol_type_exact_equal(left->parameters[index], right->parameters[index])) {
            return false;
        }
    }
    return true;
}

static SolType sol_type_intern_function(
    SolTypeChecker *checker,
    SolFunctionType candidate
) {
    for (size_t index = 0; index < checker->types->function_type_count; ++index) {
        if (sol_type_function_equal(
            checker,
            &checker->types->function_types[index],
            &candidate
        )) {
            free(candidate.parameters);
            free(candidate.effects.atoms);
            return (SolType){.kind = SOL_TYPE_FUNCTION_SIGNATURE, .definition = index};
        }
    }
    if (checker->types->function_type_count == checker->types->function_type_capacity) {
        size_t capacity = checker->types->function_type_capacity == 0
            ? 8
            : checker->types->function_type_capacity * 2;
        if (capacity < checker->types->function_type_capacity
            || capacity > SIZE_MAX / sizeof(*checker->types->function_types)) {
            checker->allocation_failed = true;
            free(candidate.parameters);
            free(candidate.effects.atoms);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        SolFunctionType *grown = realloc(
            checker->types->function_types,
            capacity * sizeof(*checker->types->function_types)
        );
        if (grown == NULL) {
            checker->allocation_failed = true;
            free(candidate.parameters);
            free(candidate.effects.atoms);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        checker->types->function_types = grown;
        checker->types->function_type_capacity = capacity;
    }
    size_t id = checker->types->function_type_count++;
    checker->types->function_types[id] = candidate;
    return (SolType){.kind = SOL_TYPE_FUNCTION_SIGNATURE, .definition = id};
}

static SolType sol_type_intern_application(
    SolTypeChecker *checker,
    SolTypeConstructor constructor,
    SolDefId definition,
    SolType *arguments,
    size_t argument_count
) {
    if (argument_count == 0 || arguments == NULL) {
        free(arguments);
        sol_type_malformed(checker);
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    for (size_t argument = 0; argument < argument_count; ++argument) {
        SolTypeKind kind = arguments[argument].kind;
        if (kind == SOL_TYPE_UNKNOWN || kind == SOL_TYPE_ERROR || kind == SOL_TYPE_NEVER) {
            free(arguments);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
    }
    for (size_t index = 0; index < checker->types->type_application_count; ++index) {
        const SolTypeApplication *application
            = &checker->types->type_applications[index];
        if (application->constructor != constructor
            || application->definition != definition
            || application->argument_count != argument_count) {
            continue;
        }
        const SolType *existing = NULL;
        size_t existing_count = 0;
        if (!sol_type_application_arguments(
            checker->types,
            (SolType){SOL_TYPE_APPLICATION, index},
            &existing,
            &existing_count
        )) {
            free(arguments);
            sol_type_malformed(checker);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        bool equal = true;
        for (size_t argument = 0; argument < argument_count; ++argument) {
            equal = equal && sol_type_exact_equal(
                existing[argument],
                arguments[argument]
            );
        }
        if (equal) {
            free(arguments);
            return (SolType){.kind = SOL_TYPE_APPLICATION, .definition = index};
        }
    }
    if (checker->types->type_application_count
        == checker->types->type_application_capacity) {
        size_t capacity = checker->types->type_application_capacity == 0
            ? 8
            : checker->types->type_application_capacity * 2;
        if (capacity < checker->types->type_application_capacity
            || capacity > SIZE_MAX / sizeof(*checker->types->type_applications)) {
            checker->allocation_failed = true;
            free(arguments);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        SolTypeApplication *grown = realloc(
            checker->types->type_applications,
            capacity * sizeof(*checker->types->type_applications)
        );
        if (grown == NULL) {
            checker->allocation_failed = true;
            free(arguments);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        checker->types->type_applications = grown;
        checker->types->type_application_capacity = capacity;
    }
    if (argument_count > SIZE_MAX - checker->types->type_application_argument_count) {
        checker->allocation_failed = true;
        free(arguments);
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    size_t required = checker->types->type_application_argument_count + argument_count;
    if (required > checker->types->type_application_argument_capacity) {
        size_t capacity = checker->types->type_application_argument_capacity == 0
            ? 16
            : checker->types->type_application_argument_capacity;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) {
                checker->allocation_failed = true;
                free(arguments);
                return (SolType){.kind = SOL_TYPE_ERROR};
            }
            capacity *= 2;
        }
        if (capacity > SIZE_MAX / sizeof(*checker->types->type_application_arguments)) {
            checker->allocation_failed = true;
            free(arguments);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        SolType *grown = realloc(
            checker->types->type_application_arguments,
            capacity * sizeof(*checker->types->type_application_arguments)
        );
        if (grown == NULL) {
            checker->allocation_failed = true;
            free(arguments);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        checker->types->type_application_arguments = grown;
        checker->types->type_application_argument_capacity = capacity;
    }
    size_t id = checker->types->type_application_count++;
    checker->types->type_applications[id] = (SolTypeApplication){
        .constructor = constructor,
        .definition = definition,
        .argument_offset = checker->types->type_application_argument_count,
        .argument_count = argument_count,
    };
    memcpy(
        checker->types->type_application_arguments
            + checker->types->type_application_argument_count,
        arguments,
        argument_count * sizeof(*arguments)
    );
    checker->types->type_application_argument_count = required;
    free(arguments);
    return (SolType){.kind = SOL_TYPE_APPLICATION, .definition = id};
}

static size_t sol_type_parameter_count(SolTypeChecker *checker, SolDefId definition) {
    if (definition >= checker->syntax->item_count) {
        sol_type_malformed(checker);
        return 0;
    }
    size_t count = 0;
    SolTypeParameterId parameter
        = checker->syntax->items[definition].first_type_parameter;
    while (parameter != SOL_AST_NONE) {
        if (parameter >= checker->syntax->type_parameter_count
            || count++ >= checker->syntax->type_parameter_count
            || checker->syntax->type_parameters[parameter].owner_item != definition) {
            sol_type_malformed(checker);
            return 0;
        }
        parameter = checker->syntax->type_parameters[parameter].next;
    }
    return count;
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
    if (syntax_type->kind == SOL_SYNTAX_TYPE_FUNCTION) {
        size_t parameter_count = 0;
        SolTypeArgumentId argument_id = syntax_type->first_argument;
        while (argument_id != SOL_AST_NONE) {
            if (argument_id >= checker->syntax->type_argument_count
                || parameter_count++ >= checker->syntax->type_argument_count) {
                sol_type_malformed(checker);
                break;
            }
            argument_id = checker->syntax->type_arguments[argument_id].next;
        }
        SolFunctionType candidate = {.effect_parameter = SOL_AST_NONE};
        if (syntax_type->has_effect_tail) {
            SolEffectResolution resolution
                = checker->hir->type_effect_resolutions[type_id];
            if (resolution.kind != SOL_EFFECT_RESOLUTION_PARAMETER
                || resolution.target >= checker->syntax->effect_parameter_count) {
                sol_type_malformed(checker);
            } else {
                candidate.effect_parameter = resolution.target;
            }
        } else {
            SolEffectId effect = syntax_type->first_effect;
            while (effect != SOL_AST_NONE) {
                SolEffectResolution resolution = checker->hir->effect_resolutions[effect];
                if (resolution.kind == SOL_EFFECT_RESOLUTION_PARAMETER) {
                    if (candidate.effect_parameter != SOL_AST_NONE
                        && candidate.effect_parameter != resolution.target) {
                        sol_type_malformed(checker);
                    }
                    candidate.effect_parameter = resolution.target;
                }
                effect = checker->syntax->effects[effect].next;
            }
        }
        if (parameter_count != 0) {
            if (parameter_count > SIZE_MAX / sizeof(*candidate.parameters)) {
                checker->allocation_failed = true;
            } else {
                candidate.parameters = malloc(
                    parameter_count * sizeof(*candidate.parameters)
                );
                if (candidate.parameters == NULL) checker->allocation_failed = true;
            }
        }
        candidate.parameter_count = parameter_count;
        bool valid = !checker->malformed && !checker->allocation_failed;
        argument_id = syntax_type->first_argument;
        for (size_t index = 0; valid && index < parameter_count; ++index) {
            const SolTypeArgument *argument = &checker->syntax->type_arguments[argument_id];
            candidate.parameters[index] = sol_type_from_id(checker, argument->type);
            valid = candidate.parameters[index].kind != SOL_TYPE_ERROR;
            argument_id = argument->next;
        }
        if (valid) {
            candidate.result = sol_type_from_id(checker, syntax_type->return_type);
            valid = candidate.result.kind != SOL_TYPE_ERROR;
        }
        if (valid) {
            valid = sol_type_normalize_effects(
                checker,
                type_id,
                syntax_type->first_effect,
                &candidate.effects
            );
        }
        if (valid) {
            type = sol_type_intern_function(checker, candidate);
        } else {
            free(candidate.parameters);
            free(candidate.effects.atoms);
        }
    } else if (syntax_type->kind == SOL_SYNTAX_TYPE_UNIT) {
        type = (SolType){.kind = SOL_TYPE_UNIT};
    } else {
        SolTypeResolution resolution = checker->hir->type_resolutions[type_id];
        if (syntax_type->is_capability
            && (syntax_type->first_argument != SOL_AST_NONE
                || resolution.kind != SOL_TYPE_RESOLUTION_DEFINITION
                || resolution.target >= checker->syntax->item_count
                || checker->syntax->items[resolution.target].kind
                    != SOL_ITEM_CAPABILITY)) {
            sol_type_error(
                checker,
                "SOL-TYPE-009",
                syntax_type->span,
                "capability qualifies only a direct capability declaration"
            );
            checker->declared_states[type_id] = 2;
            checker->types->declared_types[type_id] = type;
            return type;
        }
        size_t expected_count = 0;
        SolTypeConstructor constructor = SOL_TYPE_CONSTRUCTOR_USER;
        SolDefId definition = SOL_AST_NONE;
        if (resolution.kind == SOL_TYPE_RESOLUTION_BUILTIN) {
            if (resolution.target == SOL_TYPE_BUILTIN_OPTION) {
                expected_count = 1;
                constructor = SOL_TYPE_CONSTRUCTOR_OPTION;
            } else if (resolution.target == SOL_TYPE_BUILTIN_RESULT) {
                expected_count = 2;
                constructor = SOL_TYPE_CONSTRUCTOR_RESULT;
            }
        } else if (resolution.kind == SOL_TYPE_RESOLUTION_DEFINITION) {
            definition = resolution.target;
            expected_count = sol_type_parameter_count(checker, definition);
        }
        if (syntax_type->first_argument != SOL_AST_NONE) {
        size_t count = 0;
        bool valid = expected_count != 0
            && resolution.kind != SOL_TYPE_RESOLUTION_PARAMETER;
        SolType *candidate_arguments = NULL;
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
            argument_id = argument->next;
        }
        if (count != 0 && count <= SIZE_MAX / sizeof(*candidate_arguments)) {
            candidate_arguments = malloc(count * sizeof(*candidate_arguments));
            if (candidate_arguments == NULL) checker->allocation_failed = true;
        }
        argument_id = syntax_type->first_argument;
        for (size_t index = 0; candidate_arguments != NULL && index < count; ++index) {
            candidate_arguments[index] = sol_type_from_id(
                checker,
                checker->syntax->type_arguments[argument_id].type
            );
            argument_id = checker->syntax->type_arguments[argument_id].next;
        }
        if (!valid || count != expected_count) {
            free(candidate_arguments);
            sol_type_error(
                checker,
                "SOL-TYPE-009",
                syntax_type->span,
                resolution.kind == SOL_TYPE_RESOLUTION_PARAMETER
                    ? "type parameters cannot be applied"
                    : "generic type application has incorrect arity or constructor"
            );
        } else if (!checker->allocation_failed) {
            type = sol_type_intern_application(
                checker,
                constructor,
                definition,
                candidate_arguments,
                count
            );
        }
        } else if (resolution.kind == SOL_TYPE_RESOLUTION_PARAMETER) {
            type = (SolType){.kind = SOL_TYPE_PARAMETER, .definition = resolution.target};
        } else if (resolution.kind == SOL_TYPE_RESOLUTION_SELF) {
            type = (SolType){.kind = SOL_TYPE_SELF, .definition = resolution.target};
        } else if (resolution.kind == SOL_TYPE_RESOLUTION_BUILTIN
            && !syntax_type->is_capability) {
            if (resolution.target == SOL_TYPE_BUILTIN_INT64) {
                type = (SolType){.kind = SOL_TYPE_INT64};
            } else if (resolution.target == SOL_TYPE_BUILTIN_BOOL) {
                type = (SolType){.kind = SOL_TYPE_BOOL};
            } else if (resolution.target == SOL_TYPE_BUILTIN_TEXT) {
                type = (SolType){.kind = SOL_TYPE_TEXT};
            } else {
                sol_type_error(
                    checker,
                    "SOL-TYPE-009",
                    syntax_type->span,
                    "generic type constructor requires explicit arguments"
                );
            }
        } else if (resolution.kind == SOL_TYPE_RESOLUTION_DEFINITION) {
            const SolSyntaxItem *item = &checker->syntax->items[resolution.target];
            bool capability_matches = syntax_type->is_capability
                == (item->kind == SOL_ITEM_CAPABILITY);
            if (!capability_matches) {
                sol_type_error(
                    checker,
                    "SOL-TYPE-009",
                    syntax_type->span,
                    "capability type qualifier does not match the declaration"
                );
            } else if (expected_count != 0) {
                sol_type_error(
                    checker,
                    "SOL-TYPE-009",
                    syntax_type->span,
                    "generic type constructor requires explicit arguments"
                );
            } else {
                type = (SolType){.kind = SOL_TYPE_NOMINAL, .definition = resolution.target};
            }
        } else {
            sol_type_error(
                checker,
                "SOL-TYPE-009",
                syntax_type->span,
                sol_type_span_equal(checker->source, syntax_type->name, "_")
                    ? "generic wildcard arguments are unsupported"
                    : "unresolved declared type"
            );
        }
    }
    checker->declared_states[type_id] = 2;
    checker->types->declared_types[type_id] = type;
    return type;
}

static bool sol_type_effect_subset(
    const SolTypeChecker *checker,
    const SolEffectSet *actual,
    const SolEffectSet *expected
) {
    for (size_t actual_index = 0; actual_index < actual->count; ++actual_index) {
        bool found = false;
        for (size_t expected_index = 0; expected_index < expected->count; ++expected_index) {
            if (sol_type_effect_atom_equal(
                checker,
                &actual->atoms[actual_index],
                &expected->atoms[expected_index]
            )) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

static SolType sol_type_substitute(
    SolTypeChecker *checker,
    SolType type,
    SolDefId owner,
    const SolType *arguments,
    size_t argument_count
) {
    if (type.kind == SOL_TYPE_PARAMETER) {
        SolTypeParameterId parameter = checker->syntax->items[owner].first_type_parameter;
        for (size_t index = 0; index < argument_count && parameter != SOL_AST_NONE; ++index) {
            if (parameter == type.definition) return arguments[index];
            parameter = checker->syntax->type_parameters[parameter].next;
        }
        return type;
    }
    if (type.kind == SOL_TYPE_APPLICATION) {
        const SolTypeApplication *stored = sol_type_application(checker->types, type);
        const SolType *stored_arguments = NULL;
        size_t count = 0;
        if (stored == NULL || !sol_type_application_arguments(
            checker->types,
            type,
            &stored_arguments,
            &count
        )) {
            sol_type_malformed(checker);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        SolTypeApplication descriptor = *stored;
        SolType *original = malloc(count * sizeof(*original));
        SolType *substituted = malloc(count * sizeof(*substituted));
        if (original == NULL || substituted == NULL) {
            free(original);
            free(substituted);
            checker->allocation_failed = true;
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        memcpy(original, stored_arguments, count * sizeof(*original));
        bool changed = false;
        for (size_t index = 0; index < count; ++index) {
            substituted[index] = sol_type_substitute(
                checker,
                original[index],
                owner,
                arguments,
                argument_count
            );
            changed = changed || !sol_type_exact_equal(
                substituted[index],
                original[index]
            );
        }
        free(original);
        if (!changed) {
            free(substituted);
            return type;
        }
        return sol_type_intern_application(
            checker,
            descriptor.constructor,
            descriptor.definition,
            substituted,
            count
        );
    }
    if (type.kind == SOL_TYPE_FUNCTION_SIGNATURE) {
        if (type.definition >= checker->types->function_type_count) {
            sol_type_malformed(checker);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        const SolFunctionType *stored = &checker->types->function_types[type.definition];
        SolFunctionType candidate = {
            .parameter_count = stored->parameter_count,
            .result = stored->result,
            .effect_parameter = stored->effect_parameter,
        };
        SolType *original = NULL;
        if (stored->parameter_count != 0) {
            original = malloc(stored->parameter_count * sizeof(*original));
            candidate.parameters = malloc(
                stored->parameter_count * sizeof(*candidate.parameters)
            );
            if (original == NULL || candidate.parameters == NULL) {
                free(original);
                free(candidate.parameters);
                checker->allocation_failed = true;
                return (SolType){.kind = SOL_TYPE_ERROR};
            }
            memcpy(
                original,
                stored->parameters,
                stored->parameter_count * sizeof(*original)
            );
        }
        SolType original_result = stored->result;
        if (stored->effects.count != 0) {
            candidate.effects.atoms = malloc(
                stored->effects.count * sizeof(*candidate.effects.atoms)
            );
            if (candidate.effects.atoms == NULL) {
                free(original);
                free(candidate.parameters);
                checker->allocation_failed = true;
                return (SolType){.kind = SOL_TYPE_ERROR};
            }
            memcpy(
                candidate.effects.atoms,
                stored->effects.atoms,
                stored->effects.count * sizeof(*candidate.effects.atoms)
            );
            candidate.effects.count = stored->effects.count;
        }
        bool changed = false;
        for (size_t index = 0; index < candidate.parameter_count; ++index) {
            candidate.parameters[index] = sol_type_substitute(
                checker,
                original[index],
                owner,
                arguments,
                argument_count
            );
            changed = changed || !sol_type_exact_equal(
                candidate.parameters[index],
                original[index]
            );
        }
        candidate.result = sol_type_substitute(
            checker,
            original_result,
            owner,
            arguments,
            argument_count
        );
        changed = changed || !sol_type_exact_equal(candidate.result, original_result);
        free(original);
        if (!changed) {
            free(candidate.parameters);
            free(candidate.effects.atoms);
            return type;
        }
        return sol_type_intern_function(checker, candidate);
    }
    return type;
}

static bool sol_type_equality_definition_supported(
    SolTypeChecker *checker,
    SolDefId definition,
    const SolType *arguments,
    size_t argument_count,
    unsigned char *active
);

static bool sol_type_equality_supported_recursive(
    SolTypeChecker *checker,
    SolType type,
    unsigned char *active
) {
    switch (type.kind) {
        case SOL_TYPE_INT64:
        case SOL_TYPE_BOOL:
        case SOL_TYPE_TEXT:
        case SOL_TYPE_UNIT:
        case SOL_TYPE_NEVER:
            return true;
        case SOL_TYPE_NOMINAL:
            return sol_type_equality_definition_supported(
                checker, type.definition, NULL, 0, active);
        case SOL_TYPE_APPLICATION: {
            const SolTypeApplication *application
                = sol_type_application(checker->types, type);
            const SolType *arguments = NULL;
            size_t argument_count = 0;
            if (application == NULL || !sol_type_application_arguments(
                checker->types, type, &arguments, &argument_count)) {
                sol_type_malformed(checker);
                return false;
            }
            if (application->constructor == SOL_TYPE_CONSTRUCTOR_OPTION
                || application->constructor == SOL_TYPE_CONSTRUCTOR_RESULT) {
                for (size_t index = 0; index < argument_count; ++index) {
                    if (!sol_type_equality_supported_recursive(
                        checker, arguments[index], active)) return false;
                }
                return true;
            }
            return sol_type_equality_definition_supported(checker,
                application->definition, arguments, argument_count, active);
        }
        case SOL_TYPE_UNKNOWN:
        case SOL_TYPE_ERROR:
        case SOL_TYPE_FUNCTION:
        case SOL_TYPE_FUNCTION_SIGNATURE:
        case SOL_TYPE_CAPABILITY_OPERATION:
        case SOL_TYPE_VARIANT:
        case SOL_TYPE_PARAMETER:
        case SOL_TYPE_SELF:
        case SOL_TYPE_TRAIT_METHOD:
            return false;
    }
    return false;
}

static bool sol_type_equality_definition_supported(
    SolTypeChecker *checker,
    SolDefId definition,
    const SolType *arguments,
    size_t argument_count,
    unsigned char *active
) {
    if (definition >= checker->syntax->item_count) {
        sol_type_malformed(checker);
        return false;
    }
    if (active[definition] != 0) return true;
    const SolSyntaxItem *item = &checker->syntax->items[definition];
    if (item->kind == SOL_ITEM_CAPABILITY || item->kind == SOL_ITEM_FUNCTION
        || item->kind == SOL_ITEM_TRAIT || item->kind == SOL_ITEM_IMPLEMENTATION) {
        return false;
    }
    active[definition] = 1;
    bool supported = true;
    if (item->kind == SOL_ITEM_RECORD) {
        SolFieldId field = item->first_field;
        size_t traversed = 0;
        while (supported && field != SOL_AST_NONE) {
            if (field >= checker->syntax->field_count
                || traversed++ >= checker->syntax->field_count) {
                sol_type_malformed(checker);
                supported = false;
                break;
            }
            SolType field_type = checker->types->declared_types[
                checker->syntax->fields[field].type
            ];
            if (argument_count != 0) field_type = sol_type_substitute(
                checker, field_type, definition, arguments, argument_count);
            supported = sol_type_equality_supported_recursive(
                checker, field_type, active);
            field = checker->syntax->fields[field].next;
        }
    } else if (item->kind == SOL_ITEM_ENUM) {
        SolVariantId variant = item->first_variant;
        size_t variants = 0;
        while (supported && variant != SOL_AST_NONE) {
            if (variant >= checker->syntax->variant_count
                || variants++ >= checker->syntax->variant_count) {
                sol_type_malformed(checker);
                supported = false;
                break;
            }
            SolFieldId field = checker->syntax->variants[variant].first_field;
            size_t fields = 0;
            while (supported && field != SOL_AST_NONE) {
                if (field >= checker->syntax->field_count
                    || fields++ >= checker->syntax->field_count) {
                    sol_type_malformed(checker);
                    supported = false;
                    break;
                }
                SolType field_type = checker->types->declared_types[
                    checker->syntax->fields[field].type
                ];
                if (argument_count != 0) field_type = sol_type_substitute(
                    checker, field_type, definition, arguments, argument_count);
                supported = sol_type_equality_supported_recursive(
                    checker, field_type, active);
                field = checker->syntax->fields[field].next;
            }
            variant = checker->syntax->variants[variant].next;
        }
    } else if (item->kind == SOL_ITEM_TYPE) {
        SolType representation = checker->types->representations[definition].representation;
        if (argument_count != 0) representation = sol_type_substitute(
            checker, representation, definition, arguments, argument_count);
        supported = sol_type_equality_supported_recursive(
            checker, representation, active);
    }
    active[definition] = 0;
    return supported;
}

static bool sol_type_equality_supported(
    SolTypeChecker *checker,
    SolType type
) {
    unsigned char *active = checker->syntax->item_count == 0 ? NULL
        : calloc(checker->syntax->item_count, 1);
    if (checker->syntax->item_count != 0 && active == NULL) {
        checker->allocation_failed = true;
        return false;
    }
    bool supported = sol_type_equality_supported_recursive(checker, type, active);
    free(active);
    return supported;
}

static bool sol_type_infer_argument(
    SolTypeChecker *checker,
    SolType pattern,
    SolType actual,
    SolDefId owner,
    SolType *arguments,
    size_t argument_count,
    bool *conflict
) {
    if (pattern.kind == SOL_TYPE_PARAMETER) {
        SolTypeParameterId parameter = checker->syntax->items[owner].first_type_parameter;
        for (size_t index = 0; index < argument_count && parameter != SOL_AST_NONE; ++index) {
            if (parameter == pattern.definition) {
                if (arguments[index].kind == SOL_TYPE_UNKNOWN) {
                    if (actual.kind == SOL_TYPE_UNKNOWN || actual.kind == SOL_TYPE_ERROR
                        || actual.kind == SOL_TYPE_NEVER) return false;
                    arguments[index] = actual;
                    return true;
                }
                if (!sol_type_exact_equal(arguments[index], actual)) *conflict = true;
                return !*conflict;
            }
            parameter = checker->syntax->type_parameters[parameter].next;
        }
        return true;
    }
    if (pattern.kind == SOL_TYPE_APPLICATION && actual.kind == SOL_TYPE_APPLICATION) {
        const SolTypeApplication *left = sol_type_application(checker->types, pattern);
        const SolTypeApplication *right = sol_type_application(checker->types, actual);
        const SolType *left_arguments = NULL;
        const SolType *right_arguments = NULL;
        size_t left_count = 0;
        size_t right_count = 0;
        if (left == NULL || right == NULL
            || !sol_type_application_arguments(
                checker->types,
                pattern,
                &left_arguments,
                &left_count
            )
            || !sol_type_application_arguments(
                checker->types,
                actual,
                &right_arguments,
                &right_count
            )) {
            sol_type_malformed(checker);
            return false;
        }
        if (left->constructor != right->constructor || left->definition != right->definition
            || left_count != right_count) {
            *conflict = true;
            return false;
        }
        for (size_t index = 0; index < left_count; ++index) {
            if (!sol_type_infer_argument(
                checker,
                left_arguments[index],
                right_arguments[index],
                owner,
                arguments,
                argument_count,
                conflict
            )) return false;
        }
    } else if (pattern.kind == SOL_TYPE_FUNCTION_SIGNATURE
        && actual.kind == SOL_TYPE_FUNCTION_SIGNATURE) {
        if (pattern.definition >= checker->types->function_type_count
            || actual.definition >= checker->types->function_type_count) {
            sol_type_malformed(checker);
            return false;
        }
        const SolFunctionType *left = &checker->types->function_types[pattern.definition];
        const SolFunctionType *right = &checker->types->function_types[actual.definition];
        if (left->parameter_count != right->parameter_count
            || (left->effect_parameter == SOL_AST_NONE
                && (right->effect_parameter != SOL_AST_NONE
                    || !sol_type_effect_set_equal(
                        checker, &left->effects, &right->effects
                    )))) {
            *conflict = true;
            return false;
        }
        for (size_t index = 0; index < left->parameter_count; ++index) {
            if (!sol_type_infer_argument(
                checker,
                left->parameters[index],
                right->parameters[index],
                owner,
                arguments,
                argument_count,
                conflict
            )) return false;
        }
        if (!sol_type_infer_argument(
            checker,
            left->result,
            right->result,
            owner,
            arguments,
            argument_count,
            conflict
        )) return false;
    }
    return true;
}

static bool sol_type_signature_shape_matches_parameters(
    SolTypeChecker *checker,
    size_t expected_id,
    SolParameterId parameter_id,
    SolType result
) {
    if (expected_id >= checker->types->function_type_count) {
        sol_type_malformed(checker);
        return false;
    }
    size_t expected_count
        = checker->types->function_types[expected_id].parameter_count;
    size_t index = 0;
    while (parameter_id != SOL_AST_NONE && index < expected_count) {
        if (parameter_id >= checker->syntax->parameter_count) {
            sol_type_malformed(checker);
            return false;
        }
        SolType parameter = sol_type_from_id(
            checker,
            checker->syntax->parameters[parameter_id].type_id
        );
        if (!sol_type_equal(
            parameter,
            checker->types->function_types[expected_id].parameters[index]
        )) return false;
        parameter_id = checker->syntax->parameters[parameter_id].next;
        ++index;
    }
    return parameter_id == SOL_AST_NONE && index == expected_count
        && sol_type_equal(
            result,
            checker->types->function_types[expected_id].result
        );
}

static bool sol_type_record_coercion(
    SolTypeChecker *checker,
    SolExprId expression,
    SolType expected
) {
    SolTypeTable *types = checker->types;
    if (types->function_coercion_count == types->function_coercion_capacity) {
        size_t capacity = types->function_coercion_capacity == 0
            ? 8
            : types->function_coercion_capacity * 2;
        if (capacity < types->function_coercion_capacity
            || capacity > SIZE_MAX / sizeof(*types->function_coercions)) {
            checker->allocation_failed = true;
            return false;
        }
        SolFunctionCoercion *grown = realloc(
            types->function_coercions,
            capacity * sizeof(*types->function_coercions)
        );
        if (grown == NULL) {
            checker->allocation_failed = true;
            return false;
        }
        types->function_coercions = grown;
        types->function_coercion_capacity = capacity;
    }
    types->function_coercions[types->function_coercion_count++] = (SolFunctionCoercion){
        .expression = expression,
        .expected = expected,
    };
    return true;
}

static bool sol_type_assignable(
    SolTypeChecker *checker,
    SolType expected,
    SolType actual,
    SolExprId expression
) {
    if (expected.kind != SOL_TYPE_FUNCTION_SIGNATURE) return sol_type_equal(expected, actual);
    if (expected.definition >= checker->types->function_type_count) {
        sol_type_malformed(checker);
        return false;
    }
    const SolFunctionType *expected_function
        = &checker->types->function_types[expected.definition];
    if (actual.kind == SOL_TYPE_FUNCTION_SIGNATURE) {
        if (actual.definition >= checker->types->function_type_count) {
            sol_type_malformed(checker);
            return false;
        }
        const SolFunctionType *actual_function
            = &checker->types->function_types[actual.definition];
        if (actual_function->parameter_count != expected_function->parameter_count
            || !sol_type_equal(actual_function->result, expected_function->result)) {
            return false;
        }
        for (size_t index = 0; index < actual_function->parameter_count; ++index) {
            if (!sol_type_equal(
                actual_function->parameters[index],
                expected_function->parameters[index]
            )) return false;
        }
        return expected_function->effect_parameter != SOL_AST_NONE
            || (actual_function->effect_parameter == SOL_AST_NONE
                && sol_type_effect_subset(
                checker,
                &actual_function->effects,
                &expected_function->effects
            ));
    }
    SolParameterId first_parameter = SOL_AST_NONE;
    SolType result = {.kind = SOL_TYPE_ERROR};
    if (actual.kind == SOL_TYPE_FUNCTION
        && actual.definition < checker->syntax->item_count) {
        if (sol_type_parameter_count(checker, actual.definition) != 0
            || checker->syntax->items[actual.definition].first_effect_parameter
                != SOL_AST_NONE) {
            sol_type_error(
                checker,
                "SOL-TYPE-020",
                checker->syntax->expressions[expression].span,
                "generic functions cannot be coerced to callbacks in this bootstrap"
            );
            return false;
        }
        first_parameter = checker->syntax->items[actual.definition].first_parameter;
        result = checker->types->definitions[actual.definition];
    } else if (actual.kind == SOL_TYPE_CAPABILITY_OPERATION
        && actual.definition < checker->syntax->capability_member_count) {
        const SolCapabilityMember *member
            = &checker->syntax->capability_members[actual.definition];
        first_parameter = member->first_parameter;
        result = sol_type_from_id(checker, member->return_type_id);
    } else {
        return sol_type_equal(expected, actual);
    }
    if (!sol_type_signature_shape_matches_parameters(
        checker,
        expected.definition,
        first_parameter,
        result
    )) return false;
    return sol_type_record_coercion(checker, expression, expected);
}

static bool sol_type_allocate(SolTypeChecker *checker) {
    size_t expression_count = checker->syntax->expression_count;
    size_t local_count = checker->hir->local_count;
    size_t definition_count = checker->hir->definition_count;
    if (expression_count > SIZE_MAX / sizeof(*checker->types->expressions)
        || expression_count > SIZE_MAX
            / sizeof(*checker->types->expression_capability_origins)
        || expression_count > SIZE_MAX
            / sizeof(*checker->types->expression_operation_origins)
        || expression_count > SIZE_MAX / sizeof(*checker->types->handlers)
        || expression_count > SIZE_MAX / sizeof(*checker->types->call_instantiations)
        || expression_count > SIZE_MAX / sizeof(*checker->types->method_resolutions)
        || expression_count > SIZE_MAX / sizeof(*checker->types->field_resolutions)
        || expression_count > SIZE_MAX / sizeof(*checker->types->variant_resolutions)
        || checker->syntax->pattern_count > SIZE_MAX
            / sizeof(*checker->types->pattern_variant_resolutions)
        || checker->syntax->argument_count > SIZE_MAX
            / sizeof(*checker->types->argument_field_resolutions)
        || expression_count > SIZE_MAX / sizeof(*checker->types->constructions)
        || local_count > SIZE_MAX / sizeof(*checker->types->locals)
        || local_count > SIZE_MAX / sizeof(*checker->types->local_capability_origins)
        || local_count > SIZE_MAX / sizeof(*checker->types->local_operation_origins)
        || definition_count > SIZE_MAX / sizeof(*checker->types->definitions)
        || definition_count > SIZE_MAX / sizeof(*checker->types->representations)
        || checker->syntax->type_count > SIZE_MAX / sizeof(*checker->types->declared_types)) {
        return false;
    }
    checker->types->expressions = calloc(expression_count, sizeof(*checker->types->expressions));
    checker->types->expression_capability_origins = malloc(
        expression_count * sizeof(*checker->types->expression_capability_origins)
    );
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
    checker->types->handlers = malloc(expression_count * sizeof(*checker->types->handlers));
    checker->types->call_instantiations = calloc(
        expression_count,
        sizeof(*checker->types->call_instantiations)
    );
    checker->types->method_resolutions = calloc(
        expression_count,
        sizeof(*checker->types->method_resolutions)
    );
    checker->types->field_resolutions = malloc(
        expression_count * sizeof(*checker->types->field_resolutions)
    );
    checker->types->variant_resolutions = malloc(
        expression_count * sizeof(*checker->types->variant_resolutions)
    );
    checker->types->pattern_variant_resolutions = malloc(
        checker->syntax->pattern_count
            * sizeof(*checker->types->pattern_variant_resolutions)
    );
    checker->types->argument_field_resolutions = malloc(
        checker->syntax->argument_count
            * sizeof(*checker->types->argument_field_resolutions)
    );
    checker->types->implementation_targets = calloc(
        definition_count,
        sizeof(*checker->types->implementation_targets)
    );
    checker->types->representations = calloc(
        definition_count,
        sizeof(*checker->types->representations)
    );
    checker->types->constructions = calloc(
        expression_count,
        sizeof(*checker->types->constructions)
    );
    checker->states = calloc(expression_count, sizeof(*checker->states));
    checker->declared_states = calloc(
        checker->syntax->type_count,
        sizeof(*checker->declared_states)
    );
    if ((expression_count != 0
            && (checker->types->expressions == NULL
                || checker->types->expression_capability_origins == NULL
                || checker->types->expression_operation_origins == NULL
                || checker->types->handlers == NULL
                || checker->types->call_instantiations == NULL
                || checker->types->method_resolutions == NULL
                || checker->types->field_resolutions == NULL
                || checker->types->variant_resolutions == NULL
                || checker->types->constructions == NULL
                || checker->states == NULL))
        || (local_count != 0
            && (checker->types->locals == NULL
                || checker->types->local_capability_origins == NULL
                || checker->types->local_operation_origins == NULL))
        || (definition_count != 0 && (checker->types->definitions == NULL
                || checker->types->implementation_targets == NULL
                || checker->types->representations == NULL))) {
        return false;
    }
    if (checker->syntax->pattern_count != 0
        && checker->types->pattern_variant_resolutions == NULL) return false;
    if (checker->syntax->argument_count != 0
        && checker->types->argument_field_resolutions == NULL) return false;
    if (checker->syntax->type_count != 0
        && (checker->types->declared_types == NULL || checker->declared_states == NULL)) {
        return false;
    }
    checker->types->expression_count = expression_count;
    checker->types->local_count = local_count;
    checker->types->definition_count = definition_count;
    checker->types->declared_type_count = checker->syntax->type_count;
    checker->types->handler_count = expression_count;
    checker->types->call_instantiation_count = expression_count;
    checker->types->method_resolution_count = expression_count;
    checker->types->member_resolution_count = expression_count;
    checker->types->pattern_resolution_count = checker->syntax->pattern_count;
    checker->types->argument_resolution_count = checker->syntax->argument_count;
    for (size_t index = 0; index < expression_count; ++index) {
        checker->types->field_resolutions[index] = SOL_AST_NONE;
        checker->types->variant_resolutions[index] = SOL_AST_NONE;
    }
    for (size_t index = 0; index < checker->syntax->pattern_count; ++index) {
        checker->types->pattern_variant_resolutions[index] = SOL_AST_NONE;
    }
    for (size_t index = 0; index < checker->syntax->argument_count; ++index) {
        checker->types->argument_field_resolutions[index] = SOL_AST_NONE;
    }
    checker->types->implementation_target_count = definition_count;
    checker->types->representation_count = definition_count;
    checker->types->construction_count = expression_count;
    for (size_t index = 0; index < expression_count; ++index) {
        checker->types->expression_capability_origins[index] = SOL_PROVENANCE_NONE;
        checker->types->expression_operation_origins[index] = SOL_PROVENANCE_NONE;
        checker->types->handlers[index] = (SolHandler){
            .source_member = SOL_AST_NONE,
            .provider_member = SOL_AST_NONE,
            .root = SOL_AST_NONE,
        };
        checker->types->call_instantiations[index].function = SOL_AST_NONE;
        checker->types->constructions[index].definition = SOL_AST_NONE;
        checker->types->method_resolutions[index] = (SolMethodResolution){
            .kind = SOL_METHOD_RESOLUTION_NONE,
            .call = SOL_AST_NONE,
            .trait = SOL_AST_NONE,
            .requirement = SOL_AST_NONE,
            .implementation = SOL_AST_NONE,
            .method = SOL_AST_NONE,
        };
    }
    for (size_t index = 0; index < local_count; ++index) {
        checker->types->local_capability_origins[index] = SOL_PROVENANCE_NONE;
        checker->types->local_operation_origins[index] = SOL_PROVENANCE_NONE;
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
static bool sol_type_contextual_builtin_expression(
    const SolTypeChecker *checker, SolExprId expression
) {
    if (expression >= checker->syntax->expression_count
        || checker->syntax->expressions[expression].kind != SOL_EXPR_CALL) return false;
    SolExprId callee = checker->syntax->expressions[expression].as.call.callee;
    while (callee < checker->syntax->expression_count
        && checker->syntax->expressions[callee].kind == SOL_EXPR_TYPE_APPLICATION) {
        callee = checker->syntax->expressions[callee].as.type_application.base;
    }
    if (callee >= checker->hir->resolution_count
        || checker->hir->resolutions[callee].kind != SOL_RESOLUTION_BUILTIN) return false;
    if (checker->hir->resolutions[callee].target != SOL_BUILTIN_SOME) return true;
    SolArgumentId argument
        = checker->syntax->expressions[expression].as.call.first_argument;
    return argument < checker->syntax->argument_count
        && checker->syntax->arguments[argument].next == SOL_AST_NONE
        && sol_type_contextual_builtin_expression(
            checker, checker->syntax->arguments[argument].value
        );
}
static SolType sol_type_expression_expected(
    SolTypeChecker *checker, SolExprId expression_id, SolType expected
) {
    SolType previous = checker->contextual_expected;
    checker->contextual_expected = expected;
    SolType result = sol_type_expression(checker, expression_id);
    checker->contextual_expected = previous;
    return result;
}
static SolType sol_type_variant_call(
    SolTypeChecker *checker,
    SolType constructor,
    const SolExpr *call
);

static SolType sol_type_intern_variant_constructor(
    SolTypeChecker *checker,
    SolVariantId variant,
    SolType owner
) {
    for (size_t index = 0; index < checker->types->variant_constructor_count; ++index) {
        const SolVariantConstructor *candidate
            = &checker->types->variant_constructors[index];
        if (candidate->variant == variant
            && sol_type_exact_equal(candidate->owner, owner)) {
            return (SolType){.kind = SOL_TYPE_VARIANT, .definition = index};
        }
    }
    if (checker->types->variant_constructor_count
        == checker->types->variant_constructor_capacity) {
        size_t capacity = checker->types->variant_constructor_capacity == 0
            ? 8
            : checker->types->variant_constructor_capacity * 2;
        if (capacity < checker->types->variant_constructor_capacity
            || capacity > SIZE_MAX / sizeof(*checker->types->variant_constructors)) {
            checker->allocation_failed = true;
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        SolVariantConstructor *grown = realloc(
            checker->types->variant_constructors,
            capacity * sizeof(*checker->types->variant_constructors)
        );
        if (grown == NULL) {
            checker->allocation_failed = true;
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        checker->types->variant_constructors = grown;
        checker->types->variant_constructor_capacity = capacity;
    }
    size_t id = checker->types->variant_constructor_count++;
    checker->types->variant_constructors[id] = (SolVariantConstructor){
        .variant = variant,
        .owner = owner,
    };
    return (SolType){.kind = SOL_TYPE_VARIANT, .definition = id};
}

static SolDefId sol_type_nominal_definition(
    SolTypeChecker *checker,
    SolType type
) {
    if (type.kind == SOL_TYPE_NOMINAL) return type.definition;
    const SolTypeApplication *application = sol_type_application(checker->types, type);
    return application != NULL && application->constructor == SOL_TYPE_CONSTRUCTOR_USER
        ? application->definition
        : SOL_AST_NONE;
}

static SolType sol_type_member_template(
    SolTypeChecker *checker,
    SolType owner_type,
    SolType template
) {
    const SolTypeApplication *application = sol_type_application(
        checker->types,
        owner_type
    );
    const SolType *stored_arguments = NULL;
    size_t argument_count = 0;
    if (application == NULL || application->constructor != SOL_TYPE_CONSTRUCTOR_USER) {
        return template;
    }
    if (!sol_type_application_arguments(
        checker->types,
        owner_type,
        &stored_arguments,
        &argument_count
    )) {
        sol_type_malformed(checker);
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    SolType *arguments = malloc(argument_count * sizeof(*arguments));
    if (arguments == NULL) {
        checker->allocation_failed = true;
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    memcpy(arguments, stored_arguments, argument_count * sizeof(*arguments));
    SolDefId definition = application->definition;
    SolType result = sol_type_substitute(
        checker,
        template,
        definition,
        arguments,
        argument_count
    );
    free(arguments);
    return result;
}

static bool sol_type_representation_has_capability(
    SolTypeChecker *checker,
    SolType type,
    size_t depth,
    SolType *expanded
) {
    if (depth >= 256) return true;
    if (type.kind == SOL_TYPE_PARAMETER) return false;
    SolDefId definition = sol_type_nominal_definition(checker, type);
    const SolTypeApplication *application = NULL;
    const SolType *arguments = NULL;
    size_t count = 0;
    if (type.kind == SOL_TYPE_APPLICATION) {
        application = sol_type_application(checker->types, type);
        if (application == NULL || !sol_type_application_arguments(
            checker->types, type, &arguments, &count
        )) return true;
        if (application->constructor != SOL_TYPE_CONSTRUCTOR_USER) {
            for (size_t index = 0; index < count; ++index) {
                if (sol_type_representation_has_capability(
                    checker, arguments[index], depth + 1, expanded
                )) return true;
            }
        }
    }
    if (definition != SOL_AST_NONE) {
        if (definition >= checker->syntax->item_count) return true;
        SolItemKind kind = checker->syntax->items[definition].kind;
        if (kind == SOL_ITEM_CAPABILITY) return true;
        if (kind != SOL_ITEM_TYPE && kind != SOL_ITEM_RECORD && kind != SOL_ITEM_ENUM) {
            return false;
        }
        for (size_t index = 0; index < depth; ++index) {
            if (sol_type_exact_equal(expanded[index], type)) return false;
        }
        expanded[depth] = type;
        if (kind == SOL_ITEM_TYPE) {
            const SolTypeRepresentation *representation = sol_type_representation(
                checker->types, definition
            );
            if (representation == NULL) return true;
            SolType next = type.kind == SOL_TYPE_APPLICATION
                ? sol_type_member_template(checker, type, representation->representation)
                : representation->representation;
            bool result = sol_type_representation_has_capability(
                checker, next, depth + 1, expanded
            );
            return result;
        }
        if (kind == SOL_ITEM_RECORD) {
            SolFieldId field = checker->syntax->items[definition].first_field;
            while (field != SOL_AST_NONE) {
                if (field >= checker->syntax->field_count) {
                    sol_type_malformed(checker);
                    return true;
                }
                SolType field_type = sol_type_member_template(
                    checker,
                    type,
                    sol_type_from_id(checker, checker->syntax->fields[field].type)
                );
                if (sol_type_representation_has_capability(
                    checker, field_type, depth + 1, expanded
                )) return true;
                field = checker->syntax->fields[field].next;
            }
        } else {
            SolVariantId variant = checker->syntax->items[definition].first_variant;
            while (variant != SOL_AST_NONE) {
                if (variant >= checker->syntax->variant_count) {
                    sol_type_malformed(checker);
                    return true;
                }
                SolFieldId field = checker->syntax->variants[variant].first_field;
                while (field != SOL_AST_NONE) {
                    if (field >= checker->syntax->field_count) {
                        sol_type_malformed(checker);
                        return true;
                    }
                    SolType field_type = sol_type_member_template(
                        checker,
                        type,
                        sol_type_from_id(checker, checker->syntax->fields[field].type)
                    );
                    if (sol_type_representation_has_capability(
                        checker, field_type, depth + 1, expanded
                    )) return true;
                    field = checker->syntax->fields[field].next;
                }
                variant = checker->syntax->variants[variant].next;
            }
        }
    } else if (type.kind == SOL_TYPE_FUNCTION_SIGNATURE) {
        if (type.definition >= checker->types->function_type_count) return true;
        const SolFunctionType *function = &checker->types->function_types[type.definition];
        if (sol_type_representation_has_capability(
            checker, function->result, depth + 1, expanded
        )) {
            return true;
        }
        for (size_t index = 0; index < function->parameter_count; ++index) {
            if (sol_type_representation_has_capability(
                checker, function->parameters[index], depth + 1, expanded
            )) return true;
        }
    }
    return false;
}

static bool sol_type_representation_reaches(
    SolTypeChecker *checker,
    SolType type,
    SolDefId target,
    size_t depth,
    unsigned char *expanded
) {
    if (depth >= 256) return true;
    SolDefId definition = sol_type_nominal_definition(checker, type);
    if (definition == target) return true;
    const SolType *arguments = NULL;
    size_t count = 0;
    if (type.kind == SOL_TYPE_APPLICATION) {
        if (!sol_type_application_arguments(checker->types, type, &arguments, &count)) {
            return true;
        }
        const SolTypeApplication *application = sol_type_application(checker->types, type);
        if (application == NULL) return true;
        if (application->constructor != SOL_TYPE_CONSTRUCTOR_USER) {
            for (size_t index = 0; index < count; ++index) {
                if (sol_type_representation_reaches(
                    checker, arguments[index], target, depth + 1, expanded
                )) return true;
            }
        }
    }
    if (definition != SOL_AST_NONE) {
        if (definition >= checker->syntax->item_count) return true;
        SolItemKind kind = checker->syntax->items[definition].kind;
        if (kind != SOL_ITEM_TYPE && kind != SOL_ITEM_RECORD && kind != SOL_ITEM_ENUM) {
            return false;
        }
        if (expanded[definition] != 0) return false;
        expanded[definition] = 1;
        if (kind == SOL_ITEM_TYPE) {
            const SolTypeRepresentation *representation = sol_type_representation(
                checker->types, definition
            );
            if (representation == NULL) return true;
            SolType next = type.kind == SOL_TYPE_APPLICATION
                ? sol_type_member_template(checker, type, representation->representation)
                : representation->representation;
            bool result = sol_type_representation_reaches(
                checker, next, target, depth + 1, expanded
            );
            expanded[definition] = 0;
            return result;
        }
        if (kind == SOL_ITEM_RECORD) {
            SolFieldId field = checker->syntax->items[definition].first_field;
            while (field != SOL_AST_NONE) {
                if (field >= checker->syntax->field_count) return true;
                SolType field_type = sol_type_member_template(
                    checker, type,
                    sol_type_from_id(checker, checker->syntax->fields[field].type)
                );
                if (sol_type_representation_reaches(
                    checker, field_type, target, depth + 1, expanded
                )) return true;
                field = checker->syntax->fields[field].next;
            }
        } else {
            SolVariantId variant = checker->syntax->items[definition].first_variant;
            while (variant != SOL_AST_NONE) {
                if (variant >= checker->syntax->variant_count) return true;
                SolFieldId field = checker->syntax->variants[variant].first_field;
                while (field != SOL_AST_NONE) {
                    if (field >= checker->syntax->field_count) return true;
                    SolType field_type = sol_type_member_template(
                        checker, type,
                        sol_type_from_id(checker, checker->syntax->fields[field].type)
                    );
                    if (sol_type_representation_reaches(
                        checker, field_type, target, depth + 1, expanded
                    )) return true;
                    field = checker->syntax->fields[field].next;
                }
                variant = checker->syntax->variants[variant].next;
            }
        }
        expanded[definition] = 0;
    }
    return false;
}

static bool sol_type_concrete_representation_has_capability(
    SolTypeChecker *checker,
    SolType type
) {
    SolType *expanded = calloc(256, sizeof(*expanded));
    if (expanded == NULL) {
        checker->allocation_failed = true;
        return true;
    }
    bool result = sol_type_representation_has_capability(checker, type, 0, expanded);
    free(expanded);
    return result;
}

static bool sol_type_application_has_unsafe_declared_representation(
    SolTypeChecker *checker,
    SolType type,
    size_t depth
) {
    if (depth >= 256) return true;
    const SolTypeApplication *application = sol_type_application(checker->types, type);
    if (application == NULL) return false;
    SolDefId definition = application->definition;
    SolTypeConstructor constructor = application->constructor;
    if (constructor == SOL_TYPE_CONSTRUCTOR_USER
        && definition < checker->syntax->item_count
        && checker->syntax->items[definition].kind == SOL_ITEM_TYPE) {
        SolType substituted = sol_type_member_template(
            checker,
            type,
            checker->types->representations[definition].representation
        );
        if (sol_type_concrete_representation_has_capability(checker, substituted)) {
            return true;
        }
    }
    const SolType *arguments = NULL;
    size_t count = 0;
    if (!sol_type_application_arguments(checker->types, type, &arguments, &count)) {
        return true;
    }
    for (size_t index = 0; index < count; ++index) {
        if (sol_type_application_has_unsafe_declared_representation(
            checker, arguments[index], depth + 1
        )) return true;
    }
    return false;
}

static SolType sol_type_replace_self(SolTypeChecker *checker, SolType type, SolType target) {
    if (type.kind == SOL_TYPE_SELF) return target;
    if (type.kind == SOL_TYPE_FUNCTION_SIGNATURE) {
        if (type.definition >= checker->types->function_type_count) {
            sol_type_malformed(checker);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        const SolFunctionType *original = &checker->types->function_types[type.definition];
        SolFunctionType candidate = {
            .parameter_count = original->parameter_count,
            .effect_parameter = original->effect_parameter,
        };
        if (candidate.parameter_count != 0) {
            candidate.parameters = malloc(
                candidate.parameter_count * sizeof(*candidate.parameters)
            );
        }
        if (original->effects.count != 0) {
            candidate.effects.atoms = malloc(
                original->effects.count * sizeof(*candidate.effects.atoms)
            );
        }
        if ((candidate.parameter_count != 0 && candidate.parameters == NULL)
            || (original->effects.count != 0 && candidate.effects.atoms == NULL)) {
            free(candidate.parameters);
            free(candidate.effects.atoms);
            checker->allocation_failed = true;
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        if (candidate.parameter_count != 0) memcpy(
            candidate.parameters, original->parameters,
            candidate.parameter_count * sizeof(*candidate.parameters)
        );
        candidate.result = original->result;
        candidate.effects.count = original->effects.count;
        if (original->effects.count != 0) memcpy(
            candidate.effects.atoms, original->effects.atoms,
            original->effects.count * sizeof(*candidate.effects.atoms)
        );
        for (size_t index = 0; index < candidate.parameter_count; ++index) {
            candidate.parameters[index] = sol_type_replace_self(
                checker, candidate.parameters[index], target
            );
        }
        candidate.result = sol_type_replace_self(checker, candidate.result, target);
        return sol_type_intern_function(checker, candidate);
    }
    if (type.kind != SOL_TYPE_APPLICATION) return type;
    const SolTypeApplication *application = sol_type_application(checker->types, type);
    const SolType *stored = NULL;
    size_t count = 0;
    if (application == NULL || !sol_type_application_arguments(
        checker->types, type, &stored, &count
    )) {
        sol_type_malformed(checker);
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    SolTypeApplication descriptor = *application;
    SolType *arguments = malloc(count * sizeof(*arguments));
    if (arguments == NULL) {
        checker->allocation_failed = true;
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    memcpy(arguments, stored, count * sizeof(*arguments));
    for (size_t index = 0; index < count; ++index) {
        arguments[index] = sol_type_replace_self(checker, arguments[index], target);
    }
    return sol_type_intern_application(
        checker, descriptor.constructor, descriptor.definition, arguments, count
    );
}

static bool sol_type_closed_target(SolTypeChecker *checker, SolType type, size_t depth) {
    if (depth >= 256) return false;
    if (type.kind == SOL_TYPE_INT64 || type.kind == SOL_TYPE_BOOL
        || type.kind == SOL_TYPE_TEXT || type.kind == SOL_TYPE_UNIT) return true;
    if (type.kind == SOL_TYPE_NOMINAL) {
        return type.definition < checker->syntax->item_count
            && (checker->syntax->items[type.definition].kind == SOL_ITEM_RECORD
                || checker->syntax->items[type.definition].kind == SOL_ITEM_ENUM
                || checker->syntax->items[type.definition].kind == SOL_ITEM_TYPE)
            && sol_type_parameter_count(checker, type.definition) == 0;
    }
    if (type.kind != SOL_TYPE_APPLICATION) return false;
    const SolTypeApplication *application = sol_type_application(checker->types, type);
    const SolType *arguments = NULL;
    size_t count = 0;
    if (application == NULL || application->constructor != SOL_TYPE_CONSTRUCTOR_USER
        || !sol_type_application_arguments(checker->types, type, &arguments, &count)) {
        return false;
    }
    for (size_t index = 0; index < count; ++index) {
        if (!sol_type_closed_target(checker, arguments[index], depth + 1)) return false;
    }
    return true;
}

static SolTraitMethodId sol_type_find_trait_method(
    SolTypeChecker *checker,
    SolDefId owner,
    SolSpan name
) {
    SolTraitMethodId method = checker->syntax->items[owner].first_trait_method;
    size_t traversed = 0;
    while (method != SOL_AST_NONE) {
        if (method >= checker->syntax->trait_method_count
            || traversed++ >= checker->syntax->trait_method_count) {
            sol_type_malformed(checker);
            return SOL_AST_NONE;
        }
        if (sol_type_name_equal(
            checker->source, checker->syntax->trait_methods[method].name, name
        )) return method;
        method = checker->syntax->trait_methods[method].next;
    }
    return SOL_AST_NONE;
}

static SolTraitMethodId sol_type_resolve_method(
    SolTypeChecker *checker,
    SolType receiver,
    SolSpan name,
    SolDefId *trait,
    SolDefId *implementation
) {
    *trait = SOL_AST_NONE;
    *implementation = SOL_AST_NONE;
    if (receiver.kind == SOL_TYPE_PARAMETER) {
        if (receiver.definition >= checker->hir->bound_resolution_count) return SOL_AST_NONE;
        SolResolution bound = checker->hir->bound_resolutions[receiver.definition];
        if (bound.kind != SOL_RESOLUTION_DEFINITION) return SOL_AST_NONE;
        *trait = bound.target;
        return sol_type_find_trait_method(checker, *trait, name);
    }
    SolTraitMethodId found = SOL_AST_NONE;
    for (SolDefId item = 0; item < checker->syntax->item_count; ++item) {
        if (checker->syntax->items[item].kind != SOL_ITEM_IMPLEMENTATION
            || item >= checker->types->implementation_target_count
            || !sol_type_exact_equal(checker->types->implementation_targets[item], receiver)) {
            continue;
        }
        SolTraitMethodId method = sol_type_find_trait_method(checker, item, name);
        if (method != SOL_AST_NONE) {
            if (found != SOL_AST_NONE) {
                sol_type_error(checker, "SOL-TYPE-021", name,
                    "method call is ambiguous across trait implementations");
                *trait = SOL_AST_NONE;
                *implementation = SOL_AST_NONE;
                return SOL_AST_NONE;
            }
            *implementation = item;
            SolResolution resolution = checker->hir->trait_resolutions[item];
            *trait = resolution.kind == SOL_RESOLUTION_DEFINITION
                ? resolution.target : SOL_AST_NONE;
            found = method;
        }
    }
    return found;
}

static bool sol_type_has_implementation(
    SolTypeChecker *checker,
    SolDefId trait,
    SolType target
) {
    if (target.kind == SOL_TYPE_PARAMETER) {
        if (target.definition >= checker->hir->bound_resolution_count) return false;
        SolResolution evidence = checker->hir->bound_resolutions[target.definition];
        return evidence.kind == SOL_RESOLUTION_DEFINITION
            && evidence.target == trait;
    }
    for (SolDefId item = 0; item < checker->syntax->item_count; ++item) {
        if (checker->syntax->items[item].kind == SOL_ITEM_IMPLEMENTATION
            && checker->hir->trait_resolutions[item].kind == SOL_RESOLUTION_DEFINITION
            && checker->hir->trait_resolutions[item].target == trait
            && sol_type_exact_equal(checker->types->implementation_targets[item], target)) {
            return true;
        }
    }
    return false;
}

static SolType sol_type_expression_application(
    SolTypeChecker *checker,
    const SolExpr *expression
) {
    SolType base = sol_type_expression(checker, expression->as.type_application.base);
    SolDefId definition = base.kind == SOL_TYPE_FUNCTION
        ? base.definition
        : sol_type_nominal_definition(checker, base);
    size_t count = 0;
    SolTypeArgumentId argument = expression->as.type_application.first_argument;
    while (argument != SOL_AST_NONE) {
        if (argument >= checker->syntax->type_argument_count
            || count++ >= checker->syntax->type_argument_count) {
            sol_type_malformed(checker);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        argument = checker->syntax->type_arguments[argument].next;
    }
    if (definition == SOL_AST_NONE || definition >= checker->syntax->item_count) {
        sol_type_error(
            checker,
            "SOL-TYPE-016",
            expression->span,
            "type arguments can only be applied to a generic declaration"
        );
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    size_t expected = sol_type_parameter_count(checker, definition);
    if (expected == 0 || count != expected) {
        sol_type_error(
            checker,
            "SOL-TYPE-016",
            expression->span,
            expected == 0
                ? "type arguments cannot be applied to a nongeneric declaration"
                : "explicit type argument count does not match the declaration"
        );
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    if (base.kind == SOL_TYPE_FUNCTION) return base;
    SolType *arguments = malloc(count * sizeof(*arguments));
    if (arguments == NULL) {
        checker->allocation_failed = true;
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    argument = expression->as.type_application.first_argument;
    bool valid = true;
    for (size_t index = 0; index < count; ++index) {
        arguments[index] = sol_type_from_id(
            checker,
            checker->syntax->type_arguments[argument].type
        );
        valid = valid && arguments[index].kind != SOL_TYPE_ERROR;
        argument = checker->syntax->type_arguments[argument].next;
    }
    if (!valid) {
        free(arguments);
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    SolType result = sol_type_intern_application(
        checker,
        SOL_TYPE_CONSTRUCTOR_USER,
        definition,
        arguments,
        count
    );
    if (result.kind == SOL_TYPE_APPLICATION
        && sol_type_application_has_unsafe_declared_representation(checker, result, 0)) {
        sol_type_error(
            checker,
            "SOL-TYPE-024",
            expression->span,
            "constructed type representation cannot contain capabilities"
        );
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    return result;
}

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

static bool sol_type_is_capability(SolTypeChecker *checker, SolType type) {
    return type.kind == SOL_TYPE_NOMINAL
        && type.definition < checker->syntax->item_count
        && checker->syntax->items[type.definition].kind == SOL_ITEM_CAPABILITY;
}

static SolProvenanceId sol_type_path_origin(
    SolTypeChecker *checker,
    SolExprId expression_id,
    const SolProvenanceId *local_origins
) {
    if (expression_id >= checker->syntax->expression_count) {
        sol_type_malformed(checker);
        return SOL_PROVENANCE_NONE;
    }
    const SolExpr *expression = &checker->syntax->expressions[expression_id];
    SolResolution resolution = checker->hir->resolutions[expression_id];
    if (expression->kind != SOL_EXPR_PATH
        || resolution.kind != SOL_RESOLUTION_LOCAL
        || resolution.target >= checker->hir->local_count) {
        return SOL_PROVENANCE_NONE;
    }
    const SolHirLocal *local = &checker->hir->locals[resolution.target];
    if (local->owner != checker->current_definition
        || resolution.target >= checker->types->local_count) {
        return SOL_PROVENANCE_NONE;
    }
    return local_origins[resolution.target];
}

static SolProvenanceId sol_type_block_origin(
    SolTypeChecker *checker,
    const SolExpr *block,
    const SolProvenanceId *expression_origins
) {
    SolProvenanceId result = SOL_PROVENANCE_NONE;
    bool terminated = false;
    SolStatementId statement_id = block->as.block.first_statement;
    size_t traversed = 0;
    while (statement_id != SOL_AST_NONE && traversed++ < checker->syntax->statement_count) {
        const SolStatement *statement = &checker->syntax->statements[statement_id];
        SolExprId value_id = statement->kind == SOL_STATEMENT_LET
            ? statement->as.let_statement.value
            : statement->as.expression;
        SolType value = checker->types->expressions[value_id];
        if (!terminated) {
            if (statement->kind == SOL_STATEMENT_EXPRESSION
                && value.kind != SOL_TYPE_NEVER
                && value.kind != SOL_TYPE_UNKNOWN
                && value.kind != SOL_TYPE_ERROR) {
                result = expression_origins[value_id];
            } else {
                result = SOL_PROVENANCE_NONE;
            }
            terminated = statement->kind == SOL_STATEMENT_RETURN
                || value.kind == SOL_TYPE_NEVER;
        }
        statement_id = statement->next;
    }
    return result;
}

static bool sol_type_join_origin(
    SolTypeChecker *checker,
    SolExprId value_id,
    const SolProvenanceId *expression_origins,
    SolProvenanceId *origin,
    bool *have_value
) {
    SolType value = checker->types->expressions[value_id];
    if (value.kind == SOL_TYPE_NEVER) return true;
    SolProvenanceId value_origin = expression_origins[value_id];
    if (value.kind == SOL_TYPE_UNKNOWN || value.kind == SOL_TYPE_ERROR
        || value_origin == SOL_PROVENANCE_NONE) {
        return false;
    }
    if (!*have_value) {
        *origin = value_origin;
        *have_value = true;
        return true;
    }
    *origin = sol_type_union_provenance(checker, *origin, value_origin);
    return *origin != SOL_PROVENANCE_NONE;
}

static SolProvenanceId sol_type_join_expression_origin(
    SolTypeChecker *checker,
    const SolExpr *expression,
    const SolProvenanceId *expression_origins
) {
    SolProvenanceId origin = SOL_PROVENANCE_NONE;
    bool have_value = false;
    if (expression->kind == SOL_EXPR_IF) {
        if (!sol_type_join_origin(
                checker,
                expression->as.if_expr.then_branch,
                expression_origins,
                &origin,
                &have_value
            )
            || !sol_type_join_origin(
                checker,
                expression->as.if_expr.else_branch,
                expression_origins,
                &origin,
                &have_value
            )) {
            return SOL_PROVENANCE_NONE;
        }
    } else {
        SolMatchArmId arm_id = expression->as.match_expr.first_arm;
        size_t traversed = 0;
        while (arm_id != SOL_AST_NONE && traversed++ < checker->syntax->match_arm_count) {
            const SolMatchArm *arm = &checker->syntax->match_arms[arm_id];
            if (!sol_type_join_origin(
                checker,
                arm->value,
                expression_origins,
                &origin,
                &have_value
            )) {
                return SOL_PROVENANCE_NONE;
            }
            arm_id = arm->next;
        }
    }
    return have_value ? origin : SOL_PROVENANCE_NONE;
}

static SolExprId sol_type_find_authority_actual(
    SolTypeChecker *checker,
    const SolExpr *call,
    SolParameterId first_parameter,
    SolParameterId required
) {
    SolParameterId positional = first_parameter;
    SolArgumentId argument = call->as.call.first_argument;
    size_t traversed = 0;
    while (argument != SOL_AST_NONE) {
        if (argument >= checker->syntax->argument_count
            || traversed++ >= checker->syntax->argument_count) {
            return SOL_AST_NONE;
        }
        const SolArgument *actual = &checker->syntax->arguments[argument];
        SolParameterId matched = SOL_AST_NONE;
        if (actual->is_named) {
            SolParameterId parameter = first_parameter;
            while (parameter != SOL_AST_NONE) {
                if (sol_type_name_equal(
                    checker->source,
                    checker->syntax->parameters[parameter].name,
                    actual->name
                )) {
                    matched = parameter;
                    break;
                }
                parameter = checker->syntax->parameters[parameter].next;
            }
        } else {
            matched = positional;
            if (positional != SOL_AST_NONE) {
                positional = checker->syntax->parameters[positional].next;
            }
        }
        if (matched == required) return actual->value;
        argument = actual->next;
    }
    return SOL_AST_NONE;
}

static SolProvenanceId sol_type_expression_origin(
    SolTypeChecker *checker,
    SolExprId expression_id,
    SolType type,
    bool capability
) {
    if (capability ? !sol_type_is_capability(checker, type)
                   : type.kind != SOL_TYPE_CAPABILITY_OPERATION) {
        return SOL_PROVENANCE_NONE;
    }
    const SolExpr *expression = &checker->syntax->expressions[expression_id];
    const SolProvenanceId *expression_origins = capability
        ? checker->types->expression_capability_origins
        : checker->types->expression_operation_origins;
    const SolProvenanceId *local_origins = capability
        ? checker->types->local_capability_origins
        : checker->types->local_operation_origins;
    if (expression->kind == SOL_EXPR_OLD) {
        return expression_origins[expression->as.old_expression];
    }
    if (capability && expression->kind == SOL_EXPR_RESULT && checker->in_contract) {
        if (checker->current_member != SOL_AST_NONE) {
            const SolCapabilityMember *member
                = &checker->syntax->capability_members[checker->current_member];
            return member->result_authority_from_self
                ? sol_type_singleton_provenance(
                    checker,
                    checker->syntax->items[member->owner_item].capability_source
                )
                : SOL_PROVENANCE_NONE;
        }
        return sol_type_singleton_provenance(
            checker,
            checker->syntax->items[
                checker->current_definition
            ].result_authority_parameter
        );
    }
    if (!capability && expression->kind == SOL_EXPR_FIELD) {
        return checker->types->expression_capability_origins[expression->as.field.base];
    }
    if (capability && expression->kind == SOL_EXPR_CALL) {
        SolExprId callee = expression->as.call.callee;
        if (callee < checker->types->expression_count) {
            SolType callee_type = checker->types->expressions[callee];
            if (callee_type.kind == SOL_TYPE_FUNCTION
                && callee_type.definition < checker->syntax->item_count) {
                const SolSyntaxItem *function
                    = &checker->syntax->items[callee_type.definition];
                if (function->result_authority_parameter != SOL_AST_NONE) {
                    SolExprId actual = sol_type_find_authority_actual(
                        checker,
                        expression,
                        function->first_parameter,
                        function->result_authority_parameter
                    );
                    if (actual < checker->types->expression_count) {
                        return checker->types->expression_capability_origins[actual];
                    }
                }
            }
            if (callee_type.kind == SOL_TYPE_CAPABILITY_OPERATION
                && callee_type.definition < checker->syntax->capability_member_count
                && checker->syntax->capability_members[
                    callee_type.definition
                ].result_authority_from_self) {
                return checker->types->expression_operation_origins[callee];
            }
        }
    }
    if (capability && expression->kind == SOL_EXPR_RECORD) {
        SolExprId type_expression = expression->as.record.type;
        SolResolution resolution = checker->hir->resolutions[type_expression];
        if (resolution.kind == SOL_RESOLUTION_DEFINITION
            && resolution.target < checker->syntax->item_count) {
            const SolSyntaxItem *item = &checker->syntax->items[resolution.target];
            if (item->kind == SOL_ITEM_CAPABILITY
                && item->capability_source != SOL_AST_NONE) {
                SolArgumentId field = expression->as.record.first_field;
                if (field < checker->syntax->argument_count) {
                    return checker->types->expression_capability_origins[
                        checker->syntax->arguments[field].value
                    ];
                }
            }
        }
    }
    if (expression->kind == SOL_EXPR_PATH) {
        return sol_type_path_origin(checker, expression_id, local_origins);
    }
    if (expression->kind == SOL_EXPR_HANDLE) {
        SolExprId body = expression->as.handle.body;
        return body < checker->types->expression_count
            ? expression_origins[body]
            : SOL_PROVENANCE_NONE;
    }
    if (expression->kind == SOL_EXPR_BLOCK) {
        return sol_type_block_origin(checker, expression, expression_origins);
    }
    if (expression->kind == SOL_EXPR_IF || expression->kind == SOL_EXPR_MATCH) {
        return sol_type_join_expression_origin(checker, expression, expression_origins);
    }
    return SOL_PROVENANCE_NONE;
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
                    = checker->types->expression_capability_origins[
                        statement->as.let_statement.value
                    ];
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
            SolType value = statement->kind == SOL_STATEMENT_RETURN
                ? sol_type_expression_expected(
                    checker, statement->as.expression, checker->expected_return
                )
                : (statement->kind == SOL_STATEMENT_EXPRESSION
                        && statement->next == SOL_AST_NONE && !terminated
                        && checker->contextual_expected.kind != SOL_TYPE_UNKNOWN)
                    ? sol_type_expression_expected(checker, statement->as.expression,
                        checker->contextual_expected)
                    : sol_type_expression(checker, statement->as.expression);
            if (statement->kind == SOL_STATEMENT_RETURN) {
                SolParameterId expected_authority = SOL_AST_NONE;
                if (checker->current_member != SOL_AST_NONE) {
                    const SolCapabilityMember *member
                        = &checker->syntax->capability_members[checker->current_member];
                    if (member->result_authority_from_self) {
                        expected_authority = checker->syntax->items[
                            member->owner_item
                        ].capability_source;
                    }
                } else {
                    expected_authority = checker->syntax->items[
                        checker->current_definition
                    ].result_authority_parameter;
                }
                if (expected_authority != SOL_AST_NONE
                    && !sol_type_provenance_is_singleton(
                        checker->types,
                        checker->types->expression_capability_origins[
                            statement->as.expression
                        ],
                        expected_authority
                    )) {
                    sol_type_error(
                        checker,
                        "SOL-AUTHORITY-001",
                        statement->span,
                        "returned capability does not derive from the declared authority source"
                    );
                }
                if (!sol_type_assignable(
                    checker,
                    checker->expected_return,
                    value,
                    statement->as.expression
                )) {
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
    while (callee->kind == SOL_EXPR_TYPE_APPLICATION) {
        callee_id = callee->as.type_application.base;
        callee = &checker->syntax->expressions[callee_id];
    }
    if (callee->kind == SOL_EXPR_PATH || callee->kind == SOL_EXPR_FIELD) {
        return checker->hir->resolutions[callee_id];
    }
    return (SolResolution){.kind = SOL_RESOLUTION_NOT_APPLICABLE};
}

static bool sol_type_record_call_instantiation(
    SolTypeChecker *checker,
    SolExprId expression,
    SolDefId function,
    const SolType *arguments,
    size_t count
) {
    SolTypeTable *table = checker->types;
    if (count == 0 || arguments == NULL
        || count > SIZE_MAX - table->call_instantiation_argument_count) {
        sol_type_malformed(checker);
        return false;
    }
    size_t required = table->call_instantiation_argument_count + count;
    if (required > table->call_instantiation_argument_capacity) {
        size_t capacity = table->call_instantiation_argument_capacity == 0
            ? 16
            : table->call_instantiation_argument_capacity;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) {
                checker->allocation_failed = true;
                return false;
            }
            capacity *= 2;
        }
        if (capacity > SIZE_MAX / sizeof(*table->call_instantiation_arguments)) {
            checker->allocation_failed = true;
            return false;
        }
        SolType *grown = realloc(
            table->call_instantiation_arguments,
            capacity * sizeof(*table->call_instantiation_arguments)
        );
        if (grown == NULL) {
            checker->allocation_failed = true;
            return false;
        }
        table->call_instantiation_arguments = grown;
        table->call_instantiation_argument_capacity = capacity;
    }
    table->call_instantiations[expression] = (SolCallInstantiation){
        .function = function,
        .argument_offset = table->call_instantiation_argument_count,
        .argument_count = count,
    };
    memcpy(
        table->call_instantiation_arguments + table->call_instantiation_argument_count,
        arguments,
        count * sizeof(*arguments)
    );
    table->call_instantiation_argument_count = required;
    return true;
}

static SolType sol_type_method_call(
    SolTypeChecker *checker,
    SolExprId call_id,
    const SolExpr *call,
    SolTraitMethodId method_id
) {
    SolExprId callee_id = call->as.call.callee;
    const SolExpr *callee = &checker->syntax->expressions[callee_id];
    if (callee->kind != SOL_EXPR_FIELD || method_id >= checker->syntax->trait_method_count) {
        sol_type_malformed(checker);
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    SolType receiver = sol_type_expression(checker, callee->as.field.base);
    SolDefId trait = SOL_AST_NONE;
    SolDefId implementation = SOL_AST_NONE;
    SolTraitMethodId selected = sol_type_resolve_method(
        checker, receiver, callee->as.field.name, &trait, &implementation
    );
    if (selected != method_id) {
        sol_type_malformed(checker);
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    SolTraitMethodId requirement_id = sol_type_find_trait_method(
        checker, trait, callee->as.field.name
    );
    if (requirement_id == SOL_AST_NONE) {
        sol_type_malformed(checker);
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    const SolTraitMethod *signature = &checker->syntax->trait_methods[requirement_id];
    SolParameterId parameter = signature->first_parameter;
    if (parameter == SOL_AST_NONE) {
        sol_type_malformed(checker);
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    SolType self_type = sol_type_replace_self(
        checker, sol_type_from_id(checker, checker->syntax->parameters[parameter].type_id), receiver
    );
    if (!sol_type_equal(self_type, receiver)) {
        sol_type_error(checker, "SOL-TYPE-021", callee->span,
            "method receiver does not match the implementation target");
    }
    parameter = checker->syntax->parameters[parameter].next;
    size_t parameter_count = 0;
    SolParameterId parameter_id = parameter;
    while (parameter_id != SOL_AST_NONE) {
        if (parameter_id >= checker->syntax->parameter_count
            || parameter_count++ >= checker->syntax->parameter_count) {
            sol_type_malformed(checker);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        parameter_id = checker->syntax->parameters[parameter_id].next;
    }
    SolParameterId *parameter_ids = parameter_count == 0
        ? NULL : malloc(parameter_count * sizeof(*parameter_ids));
    bool *used = parameter_count == 0 ? NULL : calloc(parameter_count, sizeof(*used));
    if (parameter_count != 0 && (parameter_ids == NULL || used == NULL)) {
        free(parameter_ids);
        free(used);
        checker->allocation_failed = true;
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    parameter_id = parameter;
    for (size_t index = 0; index < parameter_count; ++index) {
        parameter_ids[index] = parameter_id;
        parameter_id = checker->syntax->parameters[parameter_id].next;
    }
    SolArgumentId argument = call->as.call.first_argument;
    size_t argument_count = 0;
    size_t positional_index = 0;
    bool seen_named = false;
    while (argument != SOL_AST_NONE) {
        if (argument >= checker->syntax->argument_count
            || argument_count++ >= checker->syntax->argument_count) {
            free(parameter_ids);
            free(used);
            sol_type_malformed(checker);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        const SolArgument *actual = &checker->syntax->arguments[argument];
        size_t matched = SIZE_MAX;
        if (actual->is_named) {
            seen_named = true;
            for (size_t index = 0; index < parameter_count; ++index) {
                if (sol_type_name_equal(
                    checker->source,
                    checker->syntax->parameters[parameter_ids[index]].name,
                    actual->name
                )) {
                    matched = index;
                    break;
                }
            }
            if (matched == SIZE_MAX) {
                sol_type_error(checker, "SOL-TYPE-012", actual->name,
                    "named argument does not match a parameter");
            }
        } else if (seen_named) {
            sol_type_error(checker, "SOL-TYPE-012",
                checker->syntax->expressions[actual->value].span,
                "positional arguments cannot follow named arguments");
        } else if (positional_index < parameter_count) {
            matched = positional_index++;
        }
        SolType expected = {.kind = SOL_TYPE_UNKNOWN};
        if (matched != SIZE_MAX) {
            expected = sol_type_replace_self(
                checker,
                sol_type_from_id(checker,
                    checker->syntax->parameters[parameter_ids[matched]].type_id),
                receiver
            );
        }
        SolType actual_type = sol_type_expression_expected(
            checker, actual->value, expected
        );
        if (matched != SIZE_MAX) {
            if (used[matched]) {
                sol_type_error(checker, "SOL-TYPE-012",
                    actual->is_named ? actual->name
                        : checker->syntax->expressions[actual->value].span,
                    "parameter is supplied more than once");
            } else {
                used[matched] = true;
                if (!sol_type_assignable(checker, expected, actual_type, actual->value)) {
                    sol_type_error(checker, "SOL-TYPE-005",
                        checker->syntax->expressions[actual->value].span,
                        "method argument type does not match parameter type");
                }
            }
        }
        argument = actual->next;
    }
    bool missing = false;
    for (size_t index = 0; index < parameter_count; ++index) {
        missing = missing || !used[index];
    }
    if (missing || argument_count != parameter_count) {
        sol_type_error(checker, "SOL-TYPE-006", call->span,
            "method call has the wrong number of arguments");
    }
    free(parameter_ids);
    free(used);
    checker->types->method_resolutions[call_id] = (SolMethodResolution){
        .kind = implementation == SOL_AST_NONE
            ? SOL_METHOD_RESOLUTION_REQUIREMENT
            : SOL_METHOD_RESOLUTION_IMPLEMENTATION,
        .call = call_id,
        .trait = trait,
        .requirement = requirement_id,
        .implementation = implementation,
        .method = method_id,
    };
    return sol_type_replace_self(checker,
        sol_type_from_id(checker, signature->return_type_id), receiver);
}

static SolType sol_type_declared_constructor_call(
    SolTypeChecker *checker,
    SolExprId call_id,
    const SolExpr *call,
    SolType result,
    SolDefId definition
) {
    const SolTypeRepresentation *metadata = sol_type_representation(
        checker->types, definition
    );
    if (metadata == NULL) {
        sol_type_malformed(checker);
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    SolArgumentId argument = call->as.call.first_argument;
    size_t count = 0;
    SolType actual = {.kind = SOL_TYPE_ERROR};
    SolExprId value = SOL_AST_NONE;
    bool positional = true;
    while (argument != SOL_AST_NONE) {
        if (argument >= checker->syntax->argument_count
            || count++ >= checker->syntax->argument_count) {
            sol_type_malformed(checker);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        const SolArgument *entry = &checker->syntax->arguments[argument];
        value = entry->value;
        positional = positional && !entry->is_named;
        argument = entry->next;
    }
    if (count != 1 || !positional) {
        sol_type_error(
            checker,
            "SOL-TYPE-024",
            call->span,
            "type construction requires exactly one positional argument"
        );
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    if (metadata->flavor == SOL_TYPE_DECLARATION_REFINED) {
        sol_type_error(
            checker,
            "SOL-TYPE-024",
            call->span,
            "refined type construction is unsupported by this bootstrap"
        );
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    SolType representation = result.kind == SOL_TYPE_APPLICATION
        ? sol_type_member_template(checker, result, metadata->representation)
        : metadata->representation;
    actual = sol_type_expression_expected(checker, value, representation);
    if (!sol_type_assignable(checker, representation, actual, value)) {
        sol_type_error(
            checker,
            "SOL-TYPE-024",
            checker->syntax->expressions[value].span,
            "type constructor argument does not match its representation"
        );
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    checker->types->constructions[call_id] = (SolTypeConstruction){
        .definition = definition,
        .representation = representation,
        .result = result,
    };
    return result;
}

static SolType sol_type_builtin_constructor_call(
    SolTypeChecker *checker,
    SolBuiltin builtin,
    const SolExpr *call
) {
    SolArgumentId argument = call->as.call.first_argument;
    size_t count = 0;
    SolType value = {.kind = SOL_TYPE_UNKNOWN};
    const SolTypeApplication *expected = sol_type_application(
        checker->types, checker->contextual_expected
    );
    SolTypeConstructor constructor = builtin == SOL_BUILTIN_NONE
        || builtin == SOL_BUILTIN_SOME
        ? SOL_TYPE_CONSTRUCTOR_OPTION : SOL_TYPE_CONSTRUCTOR_RESULT;
    const SolType *expected_arguments = NULL;
    size_t expected_argument_count = 0;
    bool exact_context = expected != NULL && expected->constructor == constructor
        && sol_type_application_arguments(
            checker->types, checker->contextual_expected,
            &expected_arguments, &expected_argument_count
        )
        && expected_argument_count
            == (constructor == SOL_TYPE_CONSTRUCTOR_OPTION ? 1 : 2);
    while (argument != SOL_AST_NONE) {
        if (argument >= checker->syntax->argument_count
            || count++ >= checker->syntax->argument_count) {
            sol_type_malformed(checker);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        SolExprId value_expression = checker->syntax->arguments[argument].value;
        SolType value_expected = {.kind = SOL_TYPE_UNKNOWN};
        if (exact_context) {
            value_expected = expected_arguments[
                builtin == SOL_BUILTIN_ERR ? 1 : 0
            ];
        }
        value = sol_type_expression_expected(checker, value_expression, value_expected);
        argument = checker->syntax->arguments[argument].next;
    }
    size_t expected_count = builtin == SOL_BUILTIN_NONE ? 0 : 1;
    if (count != expected_count) {
        sol_type_error(checker, "SOL-TYPE-025", call->span,
            "builtin constructor has the wrong number of arguments");
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    if (builtin == SOL_BUILTIN_SOME && !exact_context) {
        SolType *arguments = malloc(sizeof(*arguments));
        if (arguments == NULL) {
            checker->allocation_failed = true;
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        arguments[0] = value;
        return sol_type_intern_application(
            checker, SOL_TYPE_CONSTRUCTOR_OPTION, SOL_AST_NONE, arguments, 1
        );
    }
    if (!exact_context) {
        sol_type_error(checker, "SOL-TYPE-025", call->span,
            "builtin constructor requires an exact contextual type");
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    if (builtin != SOL_BUILTIN_NONE) {
        size_t selected = builtin == SOL_BUILTIN_ERR ? 1 : 0;
        SolExprId value_expression = checker->syntax->arguments[
            call->as.call.first_argument
        ].value;
        if (!sol_type_assignable(
            checker, expected_arguments[selected], value, value_expression
        )) {
            sol_type_error(checker, "SOL-TYPE-025",
                checker->syntax->expressions[value_expression].span,
                "builtin constructor value does not match its result type");
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
    }
    return checker->contextual_expected;
}

static SolType sol_type_call(
    SolTypeChecker *checker,
    SolExprId call_id,
    const SolExpr *call
) {
    SolType callee_type = sol_type_expression(checker, call->as.call.callee);
    if (callee_type.kind == SOL_TYPE_TRAIT_METHOD) {
        return sol_type_method_call(checker, call_id, call, callee_type.definition);
    }
    SolResolution resolution = sol_type_callee_resolution(checker, call->as.call.callee);
    if (resolution.kind == SOL_RESOLUTION_BUILTIN
        && resolution.target <= SOL_BUILTIN_NONE) {
        if (checker->syntax->expressions[call->as.call.callee].kind
            == SOL_EXPR_TYPE_APPLICATION) {
            sol_type_error(checker, "SOL-TYPE-025",
                checker->syntax->expressions[call->as.call.callee].span,
                "builtin constructors do not accept explicit type applications");
            sol_type_arguments(checker, call->as.call.first_argument);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        return sol_type_builtin_constructor_call(
            checker, (SolBuiltin)resolution.target, call
        );
    }
    SolDefId declared_type = sol_type_nominal_definition(checker, callee_type);
    if (resolution.kind == SOL_RESOLUTION_DEFINITION
        && resolution.target == declared_type
        && declared_type < checker->syntax->item_count
        && checker->syntax->items[declared_type].kind == SOL_ITEM_TYPE) {
        bool generic = sol_type_parameter_count(checker, declared_type) != 0;
        bool explicit_arguments = checker->syntax->expressions[
            call->as.call.callee
        ].kind == SOL_EXPR_TYPE_APPLICATION;
        if (generic && !explicit_arguments) {
            sol_type_arguments(checker, call->as.call.first_argument);
            sol_type_error(
                checker,
                "SOL-TYPE-024",
                checker->syntax->expressions[call->as.call.callee].span,
                "generic type construction requires explicit type arguments"
            );
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        return sol_type_declared_constructor_call(
            checker, call_id, call, callee_type, declared_type
        );
    }
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
        return sol_type_variant_call(checker, callee_type, call);
    }
    if (callee_type.kind == SOL_TYPE_FUNCTION_SIGNATURE) {
        if (callee_type.definition >= checker->types->function_type_count) {
            sol_type_malformed(checker);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        const SolFunctionType *function
            = &checker->types->function_types[callee_type.definition];
        SolArgumentId argument_id = call->as.call.first_argument;
        size_t argument_count = 0;
        while (argument_id != SOL_AST_NONE) {
            if (argument_id >= checker->syntax->argument_count
                || argument_count >= checker->syntax->argument_count) {
                sol_type_malformed(checker);
                return (SolType){.kind = SOL_TYPE_ERROR};
            }
            const SolArgument *argument = &checker->syntax->arguments[argument_id];
            SolType actual = argument_count < function->parameter_count
                ? sol_type_expression_expected(
                    checker, argument->value, function->parameters[argument_count]
                )
                : sol_type_expression(checker, argument->value);
            if (argument->is_named) {
                sol_type_error(
                    checker,
                    "SOL-TYPE-012",
                    argument->name,
                    "calls through function values require positional arguments"
                );
            }
            if (argument_count < function->parameter_count
                && !sol_type_assignable(
                    checker,
                    function->parameters[argument_count],
                    actual,
                    argument->value
                )) {
                sol_type_error(
                    checker,
                    "SOL-TYPE-005",
                    checker->syntax->expressions[argument->value].span,
                    "function argument type does not match parameter type"
                );
            }
            ++argument_count;
            argument_id = argument->next;
        }
        if (argument_count != function->parameter_count) {
            sol_type_error(
                checker,
                "SOL-TYPE-006",
                call->span,
                "function call has the wrong number of arguments"
            );
        }
        return function->result;
    }
    if (operation != SOL_AST_NONE) {
        const SolExpr *callee = &checker->syntax->expressions[call->as.call.callee];
        bool known_authority = checker->types->expression_operation_origins[
            call->as.call.callee
        ] != SOL_PROVENANCE_NONE;
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
    size_t generic_count = target == SOL_AST_NONE
        ? 0
        : sol_type_parameter_count(checker, target);
    SolType *generic_arguments = generic_count == 0
        ? NULL
        : calloc(generic_count, sizeof(*generic_arguments));
    if (generic_count != 0 && generic_arguments == NULL) {
        checker->allocation_failed = true;
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    bool explicit_arguments = checker->syntax->expressions[
        call->as.call.callee
    ].kind == SOL_EXPR_TYPE_APPLICATION;
    if (explicit_arguments && generic_count != 0) {
        SolTypeArgumentId type_argument = checker->syntax->expressions[
            call->as.call.callee
        ].as.type_application.first_argument;
        for (size_t index = 0; index < generic_count; ++index) {
            if (type_argument == SOL_AST_NONE) break;
            generic_arguments[index] = sol_type_from_id(
                checker,
                checker->syntax->type_arguments[type_argument].type
            );
            type_argument = checker->syntax->type_arguments[type_argument].next;
        }
    }
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
            free(generic_arguments);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        parameter_id = checker->syntax->parameters[parameter_id].next;
    }
    SolParameterId *parameter_ids = NULL;
    bool *used = NULL;
    SolType *actual_types = NULL;
    SolExprId *actual_expressions = NULL;
    if (parameter_count != 0) {
        if (parameter_count > SIZE_MAX / sizeof(*parameter_ids)) {
            checker->allocation_failed = true;
            free(generic_arguments);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        parameter_ids = malloc(parameter_count * sizeof(*parameter_ids));
        used = calloc(parameter_count, sizeof(*used));
        actual_types = calloc(parameter_count, sizeof(*actual_types));
        actual_expressions = malloc(parameter_count * sizeof(*actual_expressions));
        if (parameter_ids == NULL || used == NULL || actual_types == NULL
            || actual_expressions == NULL) {
            free(parameter_ids);
            free(used);
            free(actual_types);
            free(actual_expressions);
            free(generic_arguments);
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
            free(actual_types);
            free(actual_expressions);
            free(generic_arguments);
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        const SolArgument *argument = &checker->syntax->arguments[argument_id];
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

        SolType contextual = {.kind = SOL_TYPE_UNKNOWN};
        if (matched != SIZE_MAX) {
            contextual = sol_type_from_id(
                checker, checker->syntax->parameters[parameter_ids[matched]].type_id
            );
            bool known_arguments = generic_count != 0;
            for (size_t index = 0; index < generic_count; ++index) {
                known_arguments = known_arguments
                    && generic_arguments[index].kind != SOL_TYPE_UNKNOWN
                    && generic_arguments[index].kind != SOL_TYPE_ERROR;
            }
            if (generic_count != 0 && (explicit_arguments || known_arguments)) {
                contextual = sol_type_substitute(checker, contextual, target,
                    generic_arguments, generic_count);
            }
        }
        SolType actual = sol_type_expression_expected(
            checker, argument->value, contextual
        );

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
            actual_types[matched] = actual;
            actual_expressions[matched] = argument->value;
            if (generic_count != 0 && !explicit_arguments) {
                bool conflict = false;
                sol_type_infer_argument(
                    checker,
                    sol_type_from_id(checker, parameter->type_id),
                    actual,
                    target,
                    generic_arguments,
                    generic_count,
                    &conflict
                );
                if (conflict) {
                    sol_type_error(
                        checker,
                        "SOL-TYPE-017",
                        checker->syntax->expressions[argument->value].span,
                        "generic argument inference produced conflicting exact types"
                    );
                }
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
    bool complete = true;
    for (size_t index = 0; index < generic_count; ++index) {
        if (generic_arguments[index].kind == SOL_TYPE_UNKNOWN
            || generic_arguments[index].kind == SOL_TYPE_ERROR
            || generic_arguments[index].kind == SOL_TYPE_NEVER) complete = false;
    }
    if (generic_count != 0 && !complete) {
        sol_type_error(
            checker,
            "SOL-TYPE-018",
            call->span,
            "generic call has an uninferred type argument"
        );
    }
    if (complete) {
        SolTypeParameterId bounded = target == SOL_AST_NONE
            ? SOL_AST_NONE : checker->syntax->items[target].first_type_parameter;
        for (size_t index = 0; index < generic_count; ++index) {
            if (bounded == SOL_AST_NONE) break;
            SolResolution bound = checker->hir->bound_resolutions[bounded];
            if (bound.kind == SOL_RESOLUTION_DEFINITION
                && !sol_type_has_implementation(
                    checker, bound.target, generic_arguments[index]
                )) {
                sol_type_error(checker, "SOL-TYPE-022", call->span,
                    "type argument does not satisfy its trait bound");
            }
            bounded = checker->syntax->type_parameters[bounded].next;
        }
        for (size_t index = 0; index < parameter_count; ++index) {
            if (!used[index]) continue;
            const SolParameter *parameter = &checker->syntax->parameters[parameter_ids[index]];
            SolType expected = sol_type_from_id(checker, parameter->type_id);
            if (generic_count != 0) {
                expected = sol_type_substitute(
                    checker,
                    expected,
                    target,
                    generic_arguments,
                    generic_count
                );
            }
            if (!sol_type_assignable(
                checker,
                expected,
                actual_types[index],
                actual_expressions[index]
            )) {
                sol_type_error(
                    checker,
                    "SOL-TYPE-005",
                    checker->syntax->expressions[actual_expressions[index]].span,
                    "function argument type does not match parameter type"
                );
            }
        }
        if (generic_count != 0) {
            result = sol_type_substitute(
                checker,
                result,
                target,
                generic_arguments,
                generic_count
            );
            if (result.kind == SOL_TYPE_APPLICATION
                && sol_type_application_has_unsafe_declared_representation(
                    checker, result, 0
                )) {
                sol_type_error(
                    checker,
                    "SOL-TYPE-024",
                    call->span,
                    "constructed type representation cannot contain capabilities"
                );
                result = (SolType){.kind = SOL_TYPE_ERROR};
            }
            sol_type_record_call_instantiation(
                checker,
                call_id,
                target,
                generic_arguments,
                generic_count
            );
        }
    }
    free(parameter_ids);
    free(used);
    free(actual_types);
    free(actual_expressions);
    free(generic_arguments);
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

static bool sol_type_member_is_effect_family(
    SolTypeChecker *checker,
    SolCapabilityMemberId member_id,
    SolSpan effect_name
) {
    if (member_id >= checker->syntax->capability_member_count) {
        sol_type_malformed(checker);
        return false;
    }
    const SolCapabilityMember *member = &checker->syntax->capability_members[member_id];
    SolEffectId effect_id = member->first_effect;
    if (effect_id == SOL_AST_NONE || effect_id >= checker->syntax->effect_count) {
        return false;
    }
    const SolEffect *effect = &checker->syntax->effects[effect_id];
    return effect->owner_kind == SOL_EFFECT_OWNER_CAPABILITY_MEMBER
        && effect->owner == member_id
        && effect->next == SOL_AST_NONE
        && !effect->is_pure
        && effect->has_argument
        && sol_type_effect_span_equal(checker->source, effect->name, effect_name)
        && sol_type_span_equal(checker->source, effect->argument, "Self");
}

static SolCapabilityMemberId sol_type_find_effect_family(
    SolTypeChecker *checker,
    SolSpan effect_name,
    size_t *matches
) {
    SolCapabilityMemberId result = SOL_AST_NONE;
    *matches = 0;
    for (size_t member_id = 0;
        member_id < checker->syntax->capability_member_count;
        ++member_id) {
        const SolCapabilityMember *member
            = &checker->syntax->capability_members[member_id];
        if (member->owner_item >= checker->syntax->item_count) {
            sol_type_malformed(checker);
            break;
        }
        if (sol_type_member_is_effect_family(checker, member_id, effect_name)) {
            result = member_id;
            ++*matches;
        }
    }
    return result;
}

static bool sol_type_member_is_pure(
    SolTypeChecker *checker,
    SolCapabilityMemberId member_id
) {
    const SolCapabilityMember *member = &checker->syntax->capability_members[member_id];
    SolEffectId effect = member->first_effect;
    size_t traversed = 0;
    while (effect != SOL_AST_NONE) {
        if (effect >= checker->syntax->effect_count
            || traversed++ >= checker->syntax->effect_count) {
            sol_type_malformed(checker);
            return false;
        }
        const SolEffect *entry = &checker->syntax->effects[effect];
        if (entry->owner_kind != SOL_EFFECT_OWNER_CAPABILITY_MEMBER
            || entry->owner != member_id || !entry->is_pure || entry->has_argument) {
            return false;
        }
        effect = entry->next;
    }
    return member->has_effect_clause;
}

static bool sol_type_member_signature_equal(
    SolTypeChecker *checker,
    SolCapabilityMemberId left_id,
    SolCapabilityMemberId right_id
) {
    const SolCapabilityMember *left = &checker->syntax->capability_members[left_id];
    const SolCapabilityMember *right = &checker->syntax->capability_members[right_id];
    SolParameterId left_parameter = left->first_parameter;
    SolParameterId right_parameter = right->first_parameter;
    size_t traversed = 0;
    while (left_parameter != SOL_AST_NONE && right_parameter != SOL_AST_NONE) {
        if (left_parameter >= checker->syntax->parameter_count
            || right_parameter >= checker->syntax->parameter_count
            || traversed++ >= checker->syntax->parameter_count) {
            sol_type_malformed(checker);
            return false;
        }
        SolType left_type = sol_type_from_id(
            checker,
            checker->syntax->parameters[left_parameter].type_id
        );
        SolType right_type = sol_type_from_id(
            checker,
            checker->syntax->parameters[right_parameter].type_id
        );
        if (!sol_type_name_equal(
                checker->source,
                checker->syntax->parameters[left_parameter].name,
                checker->syntax->parameters[right_parameter].name
            )
            || !sol_type_equal(left_type, right_type)) return false;
        left_parameter = checker->syntax->parameters[left_parameter].next;
        right_parameter = checker->syntax->parameters[right_parameter].next;
    }
    return left_parameter == SOL_AST_NONE && right_parameter == SOL_AST_NONE
        && sol_type_equal(
            sol_type_from_id(checker, left->return_type_id),
            sol_type_from_id(checker, right->return_type_id)
        );
}

static SolType sol_type_handle(
    SolTypeChecker *checker,
    SolExprId expression_id,
    const SolExpr *expression
) {
    SolType authority = sol_type_expression(checker, expression->as.handle.authority);
    SolType provider = sol_type_expression(checker, expression->as.handle.provider);
    SolType body = sol_type_expression(checker, expression->as.handle.body);
    SolProvenanceId authority_provenance = checker->types->expression_capability_origins[
        expression->as.handle.authority
    ];
    SolProvenanceId provider_provenance = checker->types->expression_capability_origins[
        expression->as.handle.provider
    ];
    SolProvenance authority_roots = {0};
    SolProvenance provider_roots = {0};
    bool known_authority = sol_type_provenance(
        checker->types,
        authority_provenance,
        &authority_roots
    );
    bool known_provider = sol_type_provenance(
        checker->types,
        provider_provenance,
        &provider_roots
    );
    bool valid = true;
    if (!sol_type_is_capability(checker, authority) || !known_authority) {
        sol_type_error(
            checker,
            "SOL-HANDLER-001",
            checker->syntax->expressions[expression->as.handle.authority].span,
            "handled effect authority must be a capability value with a known root"
        );
        valid = false;
    } else if (authority_roots.count != 1) {
        sol_type_error(
            checker,
            "SOL-HANDLER-001",
            checker->syntax->expressions[expression->as.handle.authority].span,
            "handled effect authority has mixed roots; dynamic authority matching is unsupported"
        );
        valid = false;
    }
    if (!sol_type_is_capability(checker, provider) || !known_provider) {
        sol_type_error(
            checker,
            "SOL-HANDLER-001",
            checker->syntax->expressions[expression->as.handle.provider].span,
            "effect handler provider must be a capability value with a known root"
        );
        valid = false;
    }

    size_t matches = 0;
    SolCapabilityMemberId source_member = sol_type_find_effect_family(
        checker,
        expression->as.handle.effect_name,
        &matches
    );
    if (matches != 1) {
        sol_type_error(
            checker,
            "SOL-HANDLER-001",
            expression->as.handle.effect_name,
            matches == 0
                ? "handled effect has no module-local source capability operation"
                : "handled effect family is not unique in this module"
        );
        valid = false;
    }
    SolCapabilityMemberId provider_member = SOL_AST_NONE;
    if (matches == 1) {
        const SolCapabilityMember *source
            = &checker->syntax->capability_members[source_member];
        if (!sol_type_is_capability(checker, authority)
            || authority.definition != source->owner_item) {
            sol_type_error(
                checker,
                "SOL-HANDLER-001",
                checker->syntax->expressions[expression->as.handle.authority].span,
                "handled effect authority does not expose the source operation"
            );
            valid = false;
        }
        if (sol_type_is_capability(checker, provider)) {
            provider_member = sol_type_find_capability_member(
                checker,
                provider.definition,
                source->name
            );
        }
        if (provider_member == SOL_AST_NONE
            || !sol_type_member_signature_equal(checker, source_member, provider_member)) {
            sol_type_error(
                checker,
                "SOL-HANDLER-001",
                checker->syntax->expressions[expression->as.handle.provider].span,
                "effect handler provider must expose a compatible operation"
            );
            valid = false;
        } else if (!sol_type_member_is_pure(checker, provider_member)) {
            sol_type_error(
                checker,
                "SOL-HANDLER-001",
                checker->syntax->capability_members[provider_member].span,
                "effect handler provider operation must have a pure effect row"
            );
            valid = false;
        }
    }
    if (valid) {
        checker->types->handlers[expression_id] = (SolHandler){
            .source_member = source_member,
            .provider_member = provider_member,
            .root = authority_roots.roots[0],
        };
    }
    return body;
}

static SolDefId sol_type_variant_owner(SolTypeChecker *checker, SolVariantId variant_id) {
    if (variant_id >= checker->syntax->variant_count) return SOL_AST_NONE;
    return checker->syntax->variants[variant_id].owner_item;
}

static SolType sol_type_variant_call(
    SolTypeChecker *checker,
    SolType constructor_type,
    const SolExpr *call
) {
    const SolVariantConstructor *constructor = sol_type_variant_constructor(
        checker->types,
        constructor_type
    );
    if (constructor == NULL || constructor->variant >= checker->syntax->variant_count) {
        sol_type_malformed(checker);
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    SolVariantId variant_id = constructor->variant;
    SolType owner_type = constructor->owner;
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
        if (matched == SIZE_MAX) {
            sol_type_expression(checker, argument->value);
            sol_type_error(
                checker,
                "SOL-TYPE-014",
                argument->is_named
                    ? argument->name
                    : checker->syntax->expressions[argument->value].span,
                "enum constructor has an unknown payload argument"
            );
        } else if (used[matched]) {
            sol_type_expression(checker, argument->value);
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
            checker->types->argument_field_resolutions[argument_id] = field_ids[matched];
            SolType expected = sol_type_from_id(
                checker,
                checker->syntax->fields[field_ids[matched]].type
            );
            expected = sol_type_member_template(checker, owner_type, expected);
            SolType actual = sol_type_expression_expected(
                checker, argument->value, expected
            );
            if (!sol_type_assignable(checker, expected, actual, argument->value)) {
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
    return owner_type.kind == SOL_TYPE_UNKNOWN
        ? (SolType){.kind = SOL_TYPE_NOMINAL, .definition = owner}
        : owner_type;
}

static SolType sol_type_record(SolTypeChecker *checker, const SolExpr *record) {
    SolExprId type_expression = record->as.record.type;
    SolResolution resolution = checker->hir->resolutions[type_expression];
    SolType record_type = sol_type_expression(checker, type_expression);
    SolDefId record_definition = sol_type_nominal_definition(checker, record_type);
    if (resolution.kind == SOL_RESOLUTION_DEFINITION
        && resolution.target < checker->types->definition_count
        && checker->syntax->items[resolution.target].kind == SOL_ITEM_CAPABILITY) {
        const SolSyntaxItem *wrapper = &checker->syntax->items[resolution.target];
        SolArgumentId field_id = record->as.record.first_field;
        if (wrapper->capability_source == SOL_AST_NONE) {
            sol_type_arguments(checker, field_id);
            sol_type_error(
                checker,
                "SOL-TYPE-015",
                record->span,
                "ordinary capabilities cannot be constructed"
            );
            return (SolType){.kind = SOL_TYPE_ERROR};
        }
        const SolParameter *source
            = &checker->syntax->parameters[wrapper->capability_source];
        size_t field_count = 0;
        bool valid = true;
        while (field_id != SOL_AST_NONE) {
            const SolArgument *field = &checker->syntax->arguments[field_id];
            SolType actual = sol_type_expression(checker, field->value);
            ++field_count;
            if (!field->is_named
                || !sol_type_name_equal(checker->source, field->name, source->name)) {
                sol_type_error(
                    checker,
                    "SOL-TYPE-015",
                    field->is_named ? field->name : record->span,
                    "derived capability construction only accepts its private source"
                );
                valid = false;
            } else {
                SolType expected = sol_type_from_id(checker, source->type_id);
                if (!sol_type_equal(expected, actual)) {
                    sol_type_error(
                        checker,
                        "SOL-TYPE-015",
                        checker->syntax->expressions[field->value].span,
                        "derived capability source has the wrong capability type"
                    );
                    valid = false;
                }
                if (checker->types->expression_capability_origins[field->value]
                    == SOL_PROVENANCE_NONE) {
                    sol_type_error(
                        checker,
                        "SOL-TYPE-015",
                        checker->syntax->expressions[field->value].span,
                        "derived capability source has no known root authority"
                    );
                    valid = false;
                }
            }
            field_id = field->next;
        }
        if (field_count != 1) {
            sol_type_error(
                checker,
                "SOL-TYPE-015",
                record->span,
                "derived capability construction requires exactly one source"
            );
            valid = false;
        }
        return valid
            ? (SolType){.kind = SOL_TYPE_NOMINAL, .definition = resolution.target}
            : (SolType){.kind = SOL_TYPE_ERROR};
    }
    if (record_definition >= checker->types->definition_count
        || checker->syntax->items[record_definition].kind != SOL_ITEM_RECORD) {
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
    SolFieldId field_id = checker->syntax->items[record_definition].first_field;
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
    field_id = checker->syntax->items[record_definition].first_field;
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
            sol_type_expression(checker, argument->value);
            sol_type_error(
                checker,
                "SOL-TYPE-013",
                argument->name,
                "record literal supplies a field more than once"
            );
        } else {
            used[matched] = true;
            checker->types->argument_field_resolutions[argument_id] = field_ids[matched];
            SolType expected = sol_type_from_id(
                checker,
                checker->syntax->fields[field_ids[matched]].type
            );
            expected = sol_type_member_template(checker, record_type, expected);
            SolType actual = sol_type_expression_expected(
                checker, argument->value, expected
            );
            if (!sol_type_assignable(checker, expected, actual, argument->value)) {
                sol_type_error(
                    checker,
                    "SOL-TYPE-013",
                    checker->syntax->expressions[argument->value].span,
                    "record field value has the wrong type"
                );
            }
        }
        if (matched == SIZE_MAX || (matched != SIZE_MAX && used[matched]
                && checker->types->argument_field_resolutions[argument_id] == SOL_AST_NONE)) {
            sol_type_expression(checker, argument->value);
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
    return record_type;
}

static SolType sol_type_match(SolTypeChecker *checker, const SolExpr *match_expression) {
    SolType scrutinee = sol_type_expression(checker, match_expression->as.match_expr.scrutinee);
    SolDefId scrutinee_definition = sol_type_nominal_definition(checker, scrutinee);
    size_t case_count = scrutinee.kind == SOL_TYPE_BOOL ? 2 : 0;
    SolVariantId *variants = NULL;
    if (scrutinee_definition < checker->syntax->item_count
        && checker->syntax->items[scrutinee_definition].kind == SOL_ITEM_ENUM) {
        SolVariantId variant = checker->syntax->items[scrutinee_definition].first_variant;
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
        variant = checker->syntax->items[scrutinee_definition].first_variant;
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
    bool enum_scrutinee = scrutinee_definition < checker->syntax->item_count
        && checker->syntax->items[scrutinee_definition].kind == SOL_ITEM_ENUM;
    bool open_enum = enum_scrutinee
        && checker->syntax->items[scrutinee_definition].is_open;
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
        } else if (enum_scrutinee) {
            matched_variant = sol_type_find_variant(
                checker,
                scrutinee_definition,
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
                checker->types->pattern_variant_resolutions[arm->pattern] = matched_variant;
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
                checker->types->locals[local] = sol_type_member_template(
                    checker,
                    scrutinee,
                    sol_type_from_id(checker, checker->syntax->fields[payload].type)
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

        SolType arm_type = checker->contextual_expected.kind == SOL_TYPE_UNKNOWN
            ? sol_type_expression(checker, arm->value)
            : sol_type_expression_expected(
                checker, arm->value, checker->contextual_expected
            );
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

static bool sol_type_expression_is_definition_head(
    const SolTypeChecker *checker,
    SolExprId expression,
    SolDefId definition
) {
    while (expression < checker->syntax->expression_count
        && checker->syntax->expressions[expression].kind == SOL_EXPR_TYPE_APPLICATION) {
        expression = checker->syntax->expressions[expression].as.type_application.base;
    }
    if (expression >= checker->hir->resolution_count) return false;
    SolResolution resolution = checker->hir->resolutions[expression];
    return resolution.kind == SOL_RESOLUTION_DEFINITION
        && resolution.target == definition;
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
            if (resolution.kind == SOL_RESOLUTION_REFINEMENT_SELF) {
                if (resolution.target >= checker->types->representation_count
                    || checker->syntax->items[resolution.target].kind != SOL_ITEM_TYPE) {
                    sol_type_malformed(checker);
                    type = (SolType){.kind = SOL_TYPE_ERROR};
                } else {
                    const SolTypeRepresentation *representation = sol_type_representation(
                        checker->types, resolution.target
                    );
                    if (representation == NULL) {
                        sol_type_malformed(checker);
                        type = (SolType){.kind = SOL_TYPE_ERROR};
                    } else {
                        type = representation->representation;
                    }
                }
            } else if (resolution.kind == SOL_RESOLUTION_LOCAL
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
            bool left_contextual = equality && sol_type_contextual_builtin_expression(
                checker, expression->as.binary.left
            );
            bool right_contextual = equality && sol_type_contextual_builtin_expression(
                checker, expression->as.binary.right
            );
            SolType left;
            SolType right;
            if (left_contextual && !right_contextual) {
                right = sol_type_expression(checker, expression->as.binary.right);
                left = sol_type_expression_expected(
                    checker, expression->as.binary.left, right
                );
            } else {
                left = sol_type_expression(checker, expression->as.binary.left);
                right = right_contextual && !left_contextual
                    ? sol_type_expression_expected(
                        checker, expression->as.binary.right, left
                    )
                    : sol_type_expression(checker, expression->as.binary.right);
            }
            bool equal_types = sol_type_equal(left, right);
            if ((equality && !equal_types)
                || (!equality
                    && (!sol_type_equal(left, expected) || !sol_type_equal(right, expected)))) {
                sol_type_error(
                    checker,
                    "SOL-TYPE-002",
                    expression->span,
                    "invalid operand types for binary operator"
                );
            } else if (equality && equal_types
                && !sol_type_equality_supported(checker, left)
                && !checker->allocation_failed && !checker->malformed) {
                sol_type_error(
                    checker,
                    "SOL-TYPE-027",
                    expression->span,
                    "equality is unavailable for values containing runtime-only identity"
                );
            }
            type = logical || equality || comparison
                ? (SolType){.kind = SOL_TYPE_BOOL}
                : (SolType){.kind = SOL_TYPE_INT64};
            break;
        }
        case SOL_EXPR_CALL:
            type = sol_type_call(checker, expression_id, expression);
            break;
        case SOL_EXPR_TYPE_APPLICATION:
            type = sol_type_expression_application(checker, expression);
            break;
        case SOL_EXPR_FIELD:
            if (checker->hir->resolutions[expression_id].kind == SOL_RESOLUTION_DEFINITION) {
                SolDefId definition = checker->hir->resolutions[expression_id].target;
                type = checker->syntax->items[definition].kind == SOL_ITEM_FUNCTION
                    ? (SolType){.kind = SOL_TYPE_FUNCTION, .definition = definition}
                    : checker->types->definitions[definition];
            } else {
                SolType base = sol_type_expression(checker, expression->as.field.base);
                SolDefId base_definition = sol_type_nominal_definition(checker, base);
                if (base_definition < checker->syntax->item_count
                    && checker->syntax->items[base_definition].kind == SOL_ITEM_RECORD) {
                    SolFieldId field = sol_type_find_field(
                        checker,
                        base_definition,
                        expression->as.field.name
                    );
                    if (field == SOL_AST_NONE) {
                        SolDefId trait;
                        SolDefId implementation;
                        SolTraitMethodId method = sol_type_resolve_method(
                            checker, base, expression->as.field.name,
                            &trait, &implementation
                        );
                        if (method == SOL_AST_NONE) {
                            sol_type_error(checker, "SOL-TYPE-021",
                                expression->as.field.name,
                                "no trait method candidate for receiver type");
                            type = (SolType){.kind = SOL_TYPE_ERROR};
                        } else {
                            type = (SolType){SOL_TYPE_TRAIT_METHOD, method};
                        }
                    } else {
                        checker->types->field_resolutions[expression_id] = field;
                        type = sol_type_member_template(
                            checker,
                            base,
                            sol_type_from_id(checker, checker->syntax->fields[field].type)
                        );
                    }
                } else if (base_definition < checker->syntax->item_count
                    && checker->syntax->items[base_definition].kind == SOL_ITEM_ENUM) {
                    bool enum_head = sol_type_expression_is_definition_head(
                        checker, expression->as.field.base, base_definition
                    );
                    if (!enum_head) {
                        SolDefId trait;
                        SolDefId implementation;
                        SolTraitMethodId method = sol_type_resolve_method(
                            checker, base, expression->as.field.name,
                            &trait, &implementation
                        );
                        if (method == SOL_AST_NONE) {
                            sol_type_error(checker, "SOL-TYPE-021",
                                expression->as.field.name,
                                "no trait method candidate for receiver type");
                            type = (SolType){.kind = SOL_TYPE_ERROR};
                        } else {
                            type = (SolType){SOL_TYPE_TRAIT_METHOD, method};
                        }
                        break;
                    }
                    SolVariantId variant = sol_type_find_variant(
                        checker,
                        base_definition,
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
                        checker->types->variant_resolutions[expression_id] = variant;
                        type = base;
                    } else {
                        checker->types->variant_resolutions[expression_id] = variant;
                        type = sol_type_intern_variant_constructor(
                            checker,
                            variant,
                            base
                        );
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
                    SolDefId trait;
                    SolDefId implementation;
                    SolTraitMethodId method = sol_type_resolve_method(
                        checker, base, expression->as.field.name, &trait, &implementation
                    );
                    if (method == SOL_AST_NONE) {
                        sol_type_error(checker, "SOL-TYPE-021", expression->span,
                            "no trait method candidate for receiver type");
                        type = (SolType){.kind = SOL_TYPE_ERROR};
                    } else {
                        type = (SolType){SOL_TYPE_TRAIT_METHOD, method};
                    }
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
            SolType then_type = checker->contextual_expected.kind == SOL_TYPE_UNKNOWN
                ? sol_type_expression(checker, expression->as.if_expr.then_branch)
                : sol_type_expression_expected(checker,
                    expression->as.if_expr.then_branch, checker->contextual_expected);
            SolType else_type = checker->contextual_expected.kind == SOL_TYPE_UNKNOWN
                ? sol_type_expression(checker, expression->as.if_expr.else_branch)
                : sol_type_expression_expected(checker,
                    expression->as.if_expr.else_branch, checker->contextual_expected);
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
        case SOL_EXPR_PROPAGATE: {
            SolType propagated_expected = checker->expected_return;
            SolType propagated = sol_type_expression_expected(
                checker, expression->as.propagated, propagated_expected
            );
            const SolTypeApplication *application = sol_type_application(
                checker->types, propagated
            );
            const SolTypeApplication *expected = sol_type_application(
                checker->types, checker->expected_return
            );
            const SolType *arguments = NULL;
            const SolType *expected_arguments = NULL;
            size_t argument_count = 0;
            size_t expected_count = 0;
            bool propagation_constructor = application != NULL
                && (application->constructor == SOL_TYPE_CONSTRUCTOR_OPTION
                    || application->constructor == SOL_TYPE_CONSTRUCTOR_RESULT);
            bool valid = propagation_constructor && expected != NULL
                && application->constructor == expected->constructor
                && sol_type_application_arguments(
                    checker->types, propagated, &arguments, &argument_count
                )
                && sol_type_application_arguments(
                    checker->types, checker->expected_return,
                    &expected_arguments, &expected_count
                )
                && argument_count == expected_count
                && (argument_count == 1
                    || (argument_count == 2
                        && sol_type_exact_equal(arguments[1], expected_arguments[1])));
            if (!valid) {
                sol_type_error(checker, "SOL-TYPE-026", expression->span,
                    "propagation requires matching enclosing Option or Result return type");
                type = (SolType){.kind = SOL_TYPE_ERROR};
            } else {
                type = arguments[0];
            }
            break;
        }
        case SOL_EXPR_HANDLE:
            type = sol_type_handle(checker, expression_id, expression);
            break;
        case SOL_EXPR_RESULT:
            if (!checker->in_contract || checker->contract_kind != SOL_CONTRACT_ENSURES
                || checker->contract_outcome == SOL_CONTRACT_OUTCOME_FAILURE) {
                sol_type_error(
                    checker,
                    "SOL-CONTRACT-003",
                    expression->span,
                    "result is unavailable in this contract condition"
                );
                type = (SolType){.kind = SOL_TYPE_ERROR};
            } else if (checker->in_old) {
                sol_type_error(
                    checker,
                    "SOL-CONTRACT-003",
                    expression->span,
                    "old(result) is not a valid entry-state snapshot"
                );
                type = (SolType){.kind = SOL_TYPE_ERROR};
            } else {
                type = checker->contract_result;
                const SolTypeApplication *application = sol_type_application(
                    checker->types,
                    type
                );
                const SolType *arguments = NULL;
                size_t argument_count = 0;
                if (checker->contract_outcome == SOL_CONTRACT_OUTCOME_SUCCESS
                    && application != NULL
                    && application->constructor == SOL_TYPE_CONSTRUCTOR_RESULT
                    && sol_type_application_arguments(
                        checker->types,
                        type,
                        &arguments,
                        &argument_count
                    )
                    && argument_count == 2) {
                    type = arguments[0];
                }
            }
            break;
        case SOL_EXPR_OLD: {
            if (!checker->in_contract || checker->contract_kind != SOL_CONTRACT_ENSURES) {
                sol_type_error(
                    checker,
                    "SOL-CONTRACT-003",
                    expression->span,
                    "old is only available in postconditions"
                );
            }
            if (checker->in_old) {
                sol_type_error(
                    checker,
                    "SOL-CONTRACT-003",
                    expression->span,
                    "nested old expressions are not allowed"
                );
            }
            bool previous = checker->in_old;
            checker->in_old = true;
            type = sol_type_expression(checker, expression->as.old_expression);
            checker->in_old = previous;
            break;
        }
    }
    --checker->depth;
    checker->states[expression_id] = 2;
    checker->types->expressions[expression_id] = type;
    checker->types->expression_capability_origins[expression_id]
        = sol_type_expression_origin(checker, expression_id, type, true);
    checker->types->expression_operation_origins[expression_id]
        = sol_type_expression_origin(checker, expression_id, type, false);
    return type;
}

static void sol_type_contracts(
    SolTypeChecker *checker,
    SolContractClauseId clause_id,
    SolType result
) {
    const SolTypeApplication *result_application = sol_type_application(
        checker->types,
        result
    );
    bool result_has_outcomes = result_application != NULL
        && result_application->constructor == SOL_TYPE_CONSTRUCTOR_RESULT
        && result_application->argument_count == 2;
    size_t clause_count = 0;
    while (clause_id != SOL_AST_NONE) {
        if (clause_id >= checker->syntax->contract_clause_count
            || clause_count++ >= checker->syntax->contract_clause_count) {
            sol_type_malformed(checker);
            return;
        }
        const SolContractClause *clause
            = &checker->syntax->contract_clauses[clause_id];
        SolContractConditionId condition_id = clause->first_condition;
        size_t condition_count = 0;
        while (condition_id != SOL_AST_NONE) {
            if (condition_id >= checker->syntax->contract_condition_count
                || condition_count++ >= checker->syntax->contract_condition_count) {
                sol_type_malformed(checker);
                return;
            }
            const SolContractCondition *condition
                = &checker->syntax->contract_conditions[condition_id];
            if (condition->outcome != SOL_CONTRACT_OUTCOME_ALWAYS
                && result.kind != SOL_TYPE_ERROR
                && !result_has_outcomes) {
                sol_type_error(
                    checker,
                    "SOL-CONTRACT-005",
                    condition->span,
                    "success and failure postconditions require a Result return type"
                );
            }
            checker->in_contract = true;
            checker->in_old = false;
            checker->contract_kind = clause->kind;
            checker->contract_outcome = condition->outcome;
            checker->contract_result = result;
            checker->expected_return = result;
            memset(
                checker->states,
                0,
                checker->syntax->expression_count * sizeof(*checker->states)
            );
            SolType predicate = sol_type_expression(checker, condition->expression);
            if (predicate.kind != SOL_TYPE_BOOL && predicate.kind != SOL_TYPE_ERROR) {
                sol_type_error(
                    checker,
                    "SOL-CONTRACT-001",
                    condition->span,
                    "contract predicate must have type Bool"
                );
            }
            checker->in_contract = false;
            condition_id = condition->next;
        }
        clause_id = clause->next;
    }
}

static bool sol_type_method_signature_equal(
    SolTypeChecker *checker,
    const SolTraitMethod *requirement,
    const SolTraitMethod *method,
    SolType target
) {
    SolParameterId left = requirement->first_parameter;
    SolParameterId right = method->first_parameter;
    while (left != SOL_AST_NONE && right != SOL_AST_NONE) {
        const SolParameter *left_parameter = &checker->syntax->parameters[left];
        const SolParameter *right_parameter = &checker->syntax->parameters[right];
        SolType left_type = sol_type_replace_self(
            checker, sol_type_from_id(checker, left_parameter->type_id), target
        );
        SolType right_type = sol_type_replace_self(
            checker, sol_type_from_id(checker, right_parameter->type_id), target
        );
        if (!sol_type_exact_equal(left_type, right_type)) return false;
        left = left_parameter->next;
        right = right_parameter->next;
    }
    SolType left_result = sol_type_replace_self(
        checker, sol_type_from_id(checker, requirement->return_type_id), target
    );
    SolType right_result = sol_type_replace_self(
        checker, sol_type_from_id(checker, method->return_type_id), target
    );
    return left == SOL_AST_NONE && right == SOL_AST_NONE
        && sol_type_exact_equal(left_result, right_result);
}

static void sol_type_validate_traits(SolTypeChecker *checker) {
    for (SolDefId item = 0; item < checker->syntax->item_count; ++item) {
        const SolSyntaxItem *entry = &checker->syntax->items[item];
        if (entry->kind != SOL_ITEM_TRAIT && entry->kind != SOL_ITEM_IMPLEMENTATION) continue;
        SolTraitMethodId method = entry->first_trait_method;
        while (method != SOL_AST_NONE) {
            const SolTraitMethod *current = &checker->syntax->trait_methods[method];
            SolParameterId receiver = current->first_parameter;
            bool valid_receiver = receiver != SOL_AST_NONE
                && sol_type_span_equal(
                    checker->source, checker->syntax->parameters[receiver].name, "self"
                )
                && sol_type_from_id(
                    checker, checker->syntax->parameters[receiver].type_id
                ).kind == SOL_TYPE_SELF;
            if (!valid_receiver) {
                sol_type_error(checker, "SOL-TYPE-021", current->span,
                    "trait methods require first parameter 'self: Self'");
            }
            for (SolTraitMethodId previous = entry->first_trait_method;
                previous != method;
                previous = checker->syntax->trait_methods[previous].next) {
                if (sol_type_name_equal(
                    checker->source,
                    checker->syntax->trait_methods[previous].name,
                    current->name
                )) {
                    sol_type_error(checker, "SOL-TYPE-023", current->name,
                        "method is declared more than once");
                    break;
                }
            }
            method = current->next;
        }
        if (entry->kind != SOL_ITEM_IMPLEMENTATION) continue;
        SolResolution trait_resolution = checker->hir->trait_resolutions[item];
        if (trait_resolution.kind != SOL_RESOLUTION_DEFINITION) continue;
        SolDefId trait = trait_resolution.target;
        SolType target = checker->types->implementation_targets[item];
        for (SolDefId previous = 0; previous < item; ++previous) {
            if (checker->syntax->items[previous].kind == SOL_ITEM_IMPLEMENTATION
                && checker->hir->trait_resolutions[previous].kind
                    == SOL_RESOLUTION_DEFINITION
                && checker->hir->trait_resolutions[previous].target == trait
                && sol_type_exact_equal(
                    checker->types->implementation_targets[previous], target
                )) {
                sol_type_error(checker, "SOL-TYPE-023", entry->span,
                    "duplicate coherent implementation for trait and target");
            }
        }
        SolTraitMethodId requirement = checker->syntax->items[trait].first_trait_method;
        while (requirement != SOL_AST_NONE) {
            const SolTraitMethod *required = &checker->syntax->trait_methods[requirement];
            SolTraitMethodId found = sol_type_find_trait_method(checker, item, required->name);
            if (found == SOL_AST_NONE) {
                sol_type_error(checker, "SOL-TYPE-023", entry->span,
                    "implementation is missing a required trait method");
            } else if (!sol_type_method_signature_equal(
                checker, required, &checker->syntax->trait_methods[found], target
            )) {
                sol_type_error(checker, "SOL-TYPE-023",
                    checker->syntax->trait_methods[found].span,
                    "implementation method signature does not match its requirement");
            }
            requirement = required->next;
        }
        method = entry->first_trait_method;
        while (method != SOL_AST_NONE) {
            if (sol_type_find_trait_method(
                checker, trait, checker->syntax->trait_methods[method].name
            ) == SOL_AST_NONE) {
                sol_type_error(checker, "SOL-TYPE-023",
                    checker->syntax->trait_methods[method].name,
                    "implementation declares a method not present in the trait");
            }
            method = checker->syntax->trait_methods[method].next;
        }
    }
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
        .current_member = SOL_AST_NONE,
    };
    if (types->expressions != NULL || types->expression_capability_origins != NULL
        || types->expression_operation_origins != NULL
        || types->locals != NULL || types->local_capability_origins != NULL
        || types->local_operation_origins != NULL || types->definitions != NULL
        || types->declared_types != NULL || types->function_types != NULL
        || types->type_applications != NULL || types->function_coercions != NULL
        || types->provenances != NULL || types->provenance_roots != NULL
        || types->handlers != NULL
        || types->call_instantiations != NULL
        || types->type_application_arguments != NULL
        || types->call_instantiation_arguments != NULL
        || types->variant_constructors != NULL
        || types->method_resolutions != NULL || types->field_resolutions != NULL
        || types->variant_resolutions != NULL
        || types->pattern_variant_resolutions != NULL
        || types->argument_field_resolutions != NULL
        || types->implementation_targets != NULL
        || types->representations != NULL || types->constructions != NULL
        || types->expression_count != 0 || types->local_count != 0
        || types->definition_count != 0 || types->declared_type_count != 0
        || types->type_application_count != 0 || types->type_application_capacity != 0
        || types->type_application_argument_count != 0
        || types->type_application_argument_capacity != 0
        || types->function_type_count != 0 || types->function_type_capacity != 0
        || types->function_coercion_count != 0
        || types->function_coercion_capacity != 0
        || types->provenance_count != 0 || types->provenance_capacity != 0
        || types->provenance_root_count != 0 || types->provenance_root_capacity != 0
        || types->handler_count != 0 || types->call_instantiation_count != 0
        || types->call_instantiation_argument_count != 0
        || types->call_instantiation_argument_capacity != 0
        || types->variant_constructor_count != 0
        || types->variant_constructor_capacity != 0
        || types->method_resolution_count != 0
        || types->member_resolution_count != 0
        || types->pattern_resolution_count != 0
        || types->argument_resolution_count != 0
        || types->implementation_target_count != 0
        || types->representation_count != 0 || types->construction_count != 0) {
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
            : (item->kind == SOL_ITEM_RECORD || item->kind == SOL_ITEM_ENUM
                    || item->kind == SOL_ITEM_TYPE
                    || item->kind == SOL_ITEM_CAPABILITY)
                ? (SolType){.kind = SOL_TYPE_NOMINAL, .definition = index}
                : (SolType){.kind = SOL_TYPE_UNIT};
        if (item->kind == SOL_ITEM_TYPE) {
            checker.types->representations[index] = (SolTypeRepresentation){
                .flavor = item->flavor,
                .representation = sol_type_from_id(&checker, item->representation_type),
            };
        }
        if (item->kind == SOL_ITEM_IMPLEMENTATION) {
            SolType target = sol_type_from_id(&checker, item->implementation_type);
            checker.types->implementation_targets[index] = target;
            if (!sol_type_closed_target(&checker, target, 0)) {
                sol_type_error(&checker, "SOL-TYPE-023", item->span,
                    "implementation target must be an exact closed builtin or nominal type");
            }
        }
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
            if (item->capability_source != SOL_AST_NONE) {
                sol_type_from_id(
                    &checker,
                    syntax->parameters[item->capability_source].type_id
                );
            }
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
    for (SolDefId item = 0; item < syntax->item_count; ++item) {
        if (syntax->items[item].kind != SOL_ITEM_TYPE) continue;
        SolType representation = checker.types->representations[item].representation;
        unsigned char *expanded = calloc(syntax->item_count, 1);
        if (syntax->item_count != 0 && expanded == NULL) {
            checker.allocation_failed = true;
            break;
        }
        bool reaches = sol_type_representation_reaches(
            &checker, representation, item, 0, expanded
        );
        free(expanded);
        if (reaches) {
            sol_type_error(
                &checker,
                "SOL-TYPE-024",
                syntax->items[item].name,
                "type representation cycle"
            );
        } else if (sol_type_concrete_representation_has_capability(
            &checker, representation
        )) {
            sol_type_error(
                &checker,
                "SOL-TYPE-024",
                syntax->types[syntax->items[item].representation_type].span,
                "type representations cannot contain capabilities"
            );
        }
    }
    for (SolTypeId type_id = 0; type_id < syntax->type_count; ++type_id) {
        SolType type = checker.types->declared_types[type_id];
        if (type.kind == SOL_TYPE_APPLICATION
            && sol_type_application_has_unsafe_declared_representation(
                &checker, type, 0
            )) {
            sol_type_error(
                &checker,
                "SOL-TYPE-024",
                syntax->types[type_id].span,
                "constructed type representation cannot contain capabilities"
            );
        }
    }
    sol_type_validate_traits(&checker);
    for (size_t start = 0; start < syntax->item_count; ++start) {
        if (syntax->items[start].kind != SOL_ITEM_CAPABILITY
            || syntax->items[start].capability_source == SOL_AST_NONE) {
            continue;
        }
        SolDefId current = start;
        for (size_t steps = 0; steps < syntax->item_count; ++steps) {
            const SolSyntaxItem *item = &syntax->items[current];
            if (item->capability_source == SOL_AST_NONE) break;
            SolType source_type = sol_type_from_id(
                &checker,
                syntax->parameters[item->capability_source].type_id
            );
            if (!sol_type_is_capability(&checker, source_type)) break;
            current = source_type.definition;
            if (current == start) {
                sol_type_error(
                    &checker,
                    "SOL-TYPE-015",
                    syntax->items[start].name,
                    "derived capability source cycle"
                );
                break;
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
            if (local->owner < syntax->item_count
                && syntax->items[local->owner].kind == SOL_ITEM_IMPLEMENTATION) {
                checker.types->locals[index] = sol_type_replace_self(
                    &checker,
                    checker.types->locals[index],
                    checker.types->implementation_targets[local->owner]
                );
            }
            SolType type = checker.types->locals[index];
            if (type.kind == SOL_TYPE_NOMINAL
                && type.definition < syntax->item_count
                && syntax->items[type.definition].kind == SOL_ITEM_CAPABILITY) {
                checker.types->local_capability_origins[index]
                    = sol_type_singleton_provenance(&checker, local->syntax_id);
            }
        }
    }
    for (size_t index = 0; index < syntax->item_count; ++index) {
        const SolSyntaxItem *item = &syntax->items[index];
        if (item->kind == SOL_ITEM_FUNCTION) {
            checker.current_definition = index;
            checker.current_member = SOL_AST_NONE;
            sol_type_contracts(
                &checker,
                item->first_contract,
                checker.types->definitions[index]
            );
        }
    }
    for (size_t index = 0; index < syntax->item_count; ++index) {
        const SolSyntaxItem *item = &syntax->items[index];
        if (item->kind != SOL_ITEM_TYPE || item->flavor != SOL_TYPE_DECLARATION_REFINED) {
            continue;
        }
        checker.current_definition = index;
        checker.current_member = SOL_AST_NONE;
        sol_type_contracts(
            &checker,
            item->first_contract,
            checker.types->representations[index].representation
        );
    }
    for (size_t member_id = 0; member_id < syntax->capability_member_count; ++member_id) {
        const SolCapabilityMember *member = &syntax->capability_members[member_id];
        checker.current_definition = member->owner_item;
        checker.current_member = member_id;
        sol_type_contracts(
            &checker,
            member->first_contract,
            sol_type_from_id(&checker, member->return_type_id)
        );
    }
    for (size_t method_id = 0; method_id < syntax->trait_method_count; ++method_id) {
        const SolTraitMethod *method = &syntax->trait_methods[method_id];
        if (method->body == SOL_AST_NONE) continue;
        checker.current_definition = method->owner_item;
        checker.current_member = SOL_AST_NONE;
        checker.current_trait_method = method_id;
        checker.expected_return = sol_type_replace_self(
            &checker,
            sol_type_from_id(&checker, method->return_type_id),
            checker.types->implementation_targets[method->owner_item]
        );
        memset(checker.states, 0, syntax->expression_count * sizeof(*checker.states));
        SolType body_type = sol_type_expression_expected(
            &checker, method->body, checker.expected_return
        );
        if (body_type.kind != SOL_TYPE_NEVER && !sol_type_assignable(
            &checker, checker.expected_return, body_type, method->body
        )) {
            sol_type_error(&checker, "SOL-TYPE-004", method->span,
                "implementation method body type does not match its result type");
        }
    }
    for (size_t index = 0; index < syntax->item_count; ++index) {
        const SolSyntaxItem *item = &syntax->items[index];
        if (item->kind != SOL_ITEM_FUNCTION || item->body == SOL_AST_NONE) {
            continue;
        }
        checker.current_definition = index;
        checker.current_member = SOL_AST_NONE;
        checker.expected_return = checker.types->definitions[index];
        memset(checker.states, 0, syntax->expression_count * sizeof(*checker.states));
        SolType body_type = sol_type_expression_expected(
            &checker, item->body, checker.expected_return
        );
        if (item->result_authority_parameter != SOL_AST_NONE
            && body_type.kind != SOL_TYPE_NEVER
            && !sol_type_provenance_is_singleton(
                checker.types,
                checker.types->expression_capability_origins[item->body],
                item->result_authority_parameter
            )) {
            sol_type_error(
                &checker,
                "SOL-AUTHORITY-001",
                item->span,
                "function result does not derive from the declared authority parameter"
            );
        }
        if (body_type.kind != SOL_TYPE_NEVER
            && !sol_type_assignable(
                &checker,
                checker.expected_return,
                body_type,
                item->body
            )) {
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
    for (size_t member_id = 0; member_id < syntax->capability_member_count; ++member_id) {
        const SolCapabilityMember *member = &syntax->capability_members[member_id];
        if (member->body == SOL_AST_NONE) continue;
        checker.current_definition = member->owner_item;
        checker.current_member = member_id;
        checker.expected_return = sol_type_from_id(&checker, member->return_type_id);
        memset(checker.states, 0, syntax->expression_count * sizeof(*checker.states));
        SolType body_type = sol_type_expression_expected(
            &checker, member->body, checker.expected_return
        );
        SolParameterId expected_authority = member->result_authority_from_self
            ? syntax->items[member->owner_item].capability_source
            : SOL_AST_NONE;
        if (expected_authority != SOL_AST_NONE && body_type.kind != SOL_TYPE_NEVER
            && !sol_type_provenance_is_singleton(
                checker.types,
                checker.types->expression_capability_origins[member->body],
                expected_authority
            )) {
            sol_type_error(
                &checker,
                "SOL-AUTHORITY-001",
                member->span,
                "capability member result does not derive from its source root"
            );
        }
        if (body_type.kind != SOL_TYPE_NEVER
            && !sol_type_assignable(
                &checker,
                checker.expected_return,
                body_type,
                member->body
            )) {
            sol_type_error(
                &checker,
                "SOL-TYPE-004",
                member->span,
                "capability member body type does not match its result type"
            );
        }
    }
    if (syntax->item_count != 0
        && syntax->item_count > SIZE_MAX / syntax->item_count) {
        checker.allocation_failed = true;
    } else {
        size_t graph_size = syntax->item_count * syntax->item_count;
        unsigned char *calls = calloc(graph_size, 1);
        if (graph_size != 0 && calls == NULL) {
            checker.allocation_failed = true;
        } else {
            for (SolExprId expression = 0;
                expression < syntax->expression_count;
                ++expression) {
                if (syntax->expressions[expression].kind != SOL_EXPR_CALL) continue;
                SolExprId callee = syntax->expressions[expression].as.call.callee;
                SolType callee_type = checker.types->expressions[callee];
                if (callee_type.kind != SOL_TYPE_FUNCTION
                    || callee_type.definition >= syntax->item_count) continue;
                SolDefId owner = hir->expression_owners[expression];
                if (owner < syntax->item_count) {
                    calls[owner * syntax->item_count + callee_type.definition] = 1;
                }
            }
            for (size_t through = 0; through < syntax->item_count; ++through) {
                for (size_t from = 0; from < syntax->item_count; ++from) {
                    if (!calls[from * syntax->item_count + through]) continue;
                    for (size_t to = 0; to < syntax->item_count; ++to) {
                        if (calls[through * syntax->item_count + to]) {
                            calls[from * syntax->item_count + to] = 1;
                        }
                    }
                }
            }
            for (SolDefId function = 0; function < syntax->item_count; ++function) {
                if (calls[function * syntax->item_count + function]
                    && (sol_type_parameter_count(&checker, function) != 0
                        || syntax->items[function].first_effect_parameter
                            != SOL_AST_NONE)) {
                    sol_type_error(
                        &checker,
                        "SOL-TYPE-019",
                        syntax->items[function].name,
                        "recursive generic or effect-polymorphic function calls are unsupported"
                    );
                }
            }
        }
        free(calls);
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
            if (hir->resolutions[index].kind == SOL_RESOLUTION_BUILTIN
                && !call_callees[index]) {
                sol_type_error(&checker, "SOL-TYPE-025",
                    syntax->expressions[index].span,
                    "builtin constructors can only be used as immediate calls");
            }
            if (checker.types->expressions[index].kind == SOL_TYPE_TRAIT_METHOD
                && !call_callees[index]) {
                sol_type_error(&checker, "SOL-TYPE-021",
                    syntax->expressions[index].span,
                    "trait methods can only be used as immediate dot calls");
            }
            if (checker.types->expressions[index].kind != SOL_TYPE_CAPABILITY_OPERATION
                || checker.types->expression_operation_origins[index]
                    != SOL_PROVENANCE_NONE
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

    unsigned char *applied_heads = calloc(syntax->expression_count, 1);
    if (syntax->expression_count != 0 && applied_heads == NULL) {
        checker.allocation_failed = true;
    } else {
        for (size_t index = 0; index < syntax->expression_count; ++index) {
            if (syntax->expressions[index].kind == SOL_EXPR_TYPE_APPLICATION) {
                applied_heads[syntax->expressions[index].as.type_application.base] = 1;
            }
        }
        for (size_t index = 0; index < syntax->expression_count; ++index) {
            SolType type = checker.types->expressions[index];
            if (type.kind != SOL_TYPE_NOMINAL || applied_heads[index]
                || type.definition >= syntax->item_count
                || (syntax->items[type.definition].kind != SOL_ITEM_RECORD
                    && syntax->items[type.definition].kind != SOL_ITEM_ENUM
                    && syntax->items[type.definition].kind != SOL_ITEM_TYPE)
                || sol_type_parameter_count(&checker, type.definition) == 0) continue;
            sol_type_error(
                &checker,
                "SOL-TYPE-016",
                syntax->expressions[index].span,
                "generic type constructors require explicit type arguments"
            );
        }
    }
    free(applied_heads);

    free(checker.states);
    free(checker.declared_states);
    return !checker.allocation_failed
        && !checker.malformed
        && !diagnostics->allocation_failed;
}
