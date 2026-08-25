#include "sol/contract.h"

#include <stdint.h>
#include <stdlib.h>
#include "resource_internal.h"
#include <string.h>

typedef struct {
    const SolSource *source;
    const SolSyntaxTree *syntax;
    const SolHirModule *hir;
    const SolTypeTable *types;
    const SolEffectTable *effects;
    SolContractTable *contracts;
    SolDiagnostics *diagnostics;
    SolObligation *obligation;
    size_t depth;
    bool malformed;
    bool allocation_failed;
    bool depth_reported;
    bool in_loop_specification;
    bool validating_guard;
} SolContractLowerer;

void sol_contract_table_init(SolContractTable *table) {
    memset(table, 0, sizeof(*table));
}

void sol_contract_table_free(SolContractTable *table) {
    free(table->obligations);
    free(table->snapshots);
    free(table->expression_snapshots);
    free(table->loop_obligations);
    free(table->unreachable_obligations);
    memset(table, 0, sizeof(*table));
}

static void sol_contract_error(
    SolContractLowerer *lowerer,
    const char *code,
    SolSpan span,
    const char *message
) {
    sol_diagnostics_add(
        lowerer->diagnostics,
        lowerer->validating_guard ? "SOL-EFFECT-010" : code,
        SOL_SEVERITY_ERROR,
        span,
        "%s",
        message
    );
}

static bool sol_contract_span_valid(const SolSource *source, SolSpan span) {
    return span.start <= span.end && span.end <= source->length;
}

static bool sol_contract_span_equal(
    const SolSource *source,
    SolSpan left,
    SolSpan right
) {
    if (!sol_contract_span_valid(source, left)
        || !sol_contract_span_valid(source, right)) return false;
    size_t left_length = left.end - left.start;
    size_t right_length = right.end - right.start;
    return left_length == right_length
        && memcmp(source->text + left.start, source->text + right.start, left_length) == 0;
}

static bool sol_contract_span_text_equal(
    const SolSource *source,
    SolSpan span,
    const char *text
) {
    size_t length = span.end - span.start;
    return sol_contract_span_valid(source, span) && strlen(text) == length
        && memcmp(source->text + span.start, text, length) == 0;
}

static bool sol_contract_projection_name(
    const SolSource *source, SolSpan span, size_t expected
) {
    if (!sol_contract_span_valid(source, span) || span.start == span.end
        || (span.end - span.start > 1 && source->text[span.start] == '0')) return false;
    size_t value = 0;
    for (size_t index = span.start; index < span.end; ++index) {
        unsigned char byte = (unsigned char)source->text[index];
        if (byte < '0' || byte > '9') return false;
        size_t digit = (size_t)(byte - '0');
        if (value > (SIZE_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return value == expected;
}

static bool sol_contract_trait_method_reachable(
    const SolContractLowerer *lowerer,
    SolDefId owner,
    SolTraitMethodId target
) {
    if (owner >= lowerer->syntax->item_count) return false;
    SolTraitMethodId method = lowerer->syntax->items[owner].first_trait_method;
    size_t traversed = 0;
    while (method != SOL_AST_NONE) {
        if (method >= lowerer->syntax->trait_method_count
            || traversed++ >= lowerer->syntax->trait_method_count) return false;
        if (method == target) return true;
        method = lowerer->syntax->trait_methods[method].next;
    }
    return false;
}

static bool sol_contract_method_resolution_valid(
    const SolContractLowerer *lowerer,
    SolExprId expression,
    const SolMethodResolution *resolution
) {
    const SolSyntaxTree *syntax = lowerer->syntax;
    const SolTypeTable *types = lowerer->types;
    if (resolution->call != expression || expression >= syntax->expression_count
        || syntax->expressions[expression].kind != SOL_EXPR_CALL) return false;
    SolExprId callee_id = syntax->expressions[expression].as.call.callee;
    if (callee_id >= syntax->expression_count
        || syntax->expressions[callee_id].kind != SOL_EXPR_FIELD
        || callee_id >= types->expression_count
        || types->expressions[callee_id].kind != SOL_TYPE_TRAIT_METHOD
        || types->expressions[callee_id].definition != resolution->method
        || resolution->trait >= syntax->item_count
        || syntax->items[resolution->trait].kind != SOL_ITEM_TRAIT
        || resolution->requirement >= syntax->trait_method_count
        || resolution->method >= syntax->trait_method_count
        || !sol_contract_trait_method_reachable(
            lowerer, resolution->trait, resolution->requirement
        )) return false;
    SolSpan name = syntax->expressions[callee_id].as.field.name;
    const SolTraitMethod *requirement
        = &syntax->trait_methods[resolution->requirement];
    const SolTraitMethod *method = &syntax->trait_methods[resolution->method];
    if (!sol_contract_span_equal(lowerer->source, requirement->name, name)
        || !sol_contract_span_equal(lowerer->source, method->name, name)
        || !sol_contract_span_equal(lowerer->source, requirement->name, method->name)) {
        return false;
    }
    SolExprId receiver_id = syntax->expressions[callee_id].as.field.base;
    if (receiver_id >= types->expression_count) return false;
    SolType receiver = types->expressions[receiver_id];
    if (resolution->kind == SOL_METHOD_RESOLUTION_REQUIREMENT) {
        if (resolution->implementation != SOL_AST_NONE
            || resolution->method != resolution->requirement
            || receiver.kind != SOL_TYPE_PARAMETER
            || receiver.definition >= lowerer->hir->bound_resolution_count) return false;
        SolResolution bound = lowerer->hir->bound_resolutions[receiver.definition];
        return bound.kind == SOL_RESOLUTION_DEFINITION
            && bound.target == resolution->trait;
    }
    if (resolution->kind != SOL_METHOD_RESOLUTION_IMPLEMENTATION
        || resolution->implementation >= syntax->item_count
        || syntax->items[resolution->implementation].kind != SOL_ITEM_IMPLEMENTATION
        || !sol_contract_trait_method_reachable(
            lowerer, resolution->implementation, resolution->method
        ) || resolution->implementation >= lowerer->hir->trait_resolution_count
        || lowerer->hir->trait_resolutions[resolution->implementation].kind
            != SOL_RESOLUTION_DEFINITION
        || lowerer->hir->trait_resolutions[resolution->implementation].target
            != resolution->trait
        || resolution->implementation >= types->implementation_target_count) return false;
    SolType target = types->implementation_targets[resolution->implementation];
    return target.kind == receiver.kind && target.definition == receiver.definition;
}

static bool sol_contract_type_valid(
    const SolContractLowerer *lowerer,
    SolType type
) {
    if ((int)type.kind < 0 || type.kind > SOL_TYPE_TRAIT_METHOD) return false;
    switch (type.kind) {
        case SOL_TYPE_NOMINAL:
            return type.definition < lowerer->syntax->item_count
                && (lowerer->syntax->items[type.definition].kind == SOL_ITEM_RECORD
                    || lowerer->syntax->items[type.definition].kind == SOL_ITEM_ENUM
                    || lowerer->syntax->items[type.definition].kind == SOL_ITEM_TYPE
                    || lowerer->syntax->items[type.definition].kind
                        == SOL_ITEM_CAPABILITY);
        case SOL_TYPE_FUNCTION:
            return type.definition < lowerer->syntax->item_count
                && lowerer->syntax->items[type.definition].kind == SOL_ITEM_FUNCTION;
        case SOL_TYPE_APPLICATION:
            return type.definition < lowerer->types->type_application_count;
        case SOL_TYPE_FUNCTION_SIGNATURE:
            return type.definition < lowerer->types->function_type_count;
        case SOL_TYPE_CAPABILITY_OPERATION:
            return type.definition < lowerer->syntax->capability_member_count;
        case SOL_TYPE_VARIANT:
            return type.definition < lowerer->types->variant_constructor_count;
        case SOL_TYPE_PARAMETER:
            return type.definition < lowerer->syntax->type_parameter_count;
        case SOL_TYPE_SELF:
            return type.definition < lowerer->syntax->item_count;
        case SOL_TYPE_TRAIT_METHOD:
            return type.definition < lowerer->syntax->trait_method_count;
        default:
            return type.definition == 0;
    }
}

static bool sol_contract_coercion_shape_matches(
    const SolContractLowerer *lowerer,
    const SolFunctionCoercion *coercion,
    SolType actual
) {
    if (coercion->expected.kind != SOL_TYPE_FUNCTION_SIGNATURE
        || coercion->expected.definition >= lowerer->types->function_type_count) return false;
    const SolFunctionType *expected
        = &lowerer->types->function_types[coercion->expected.definition];
    SolParameterId parameter;
    SolType result;
    if (actual.kind == SOL_TYPE_FUNCTION && actual.definition < lowerer->syntax->item_count
        && lowerer->syntax->items[actual.definition].kind == SOL_ITEM_FUNCTION) {
        parameter = lowerer->syntax->items[actual.definition].first_parameter;
        result = lowerer->types->definitions[actual.definition];
    } else if (actual.kind == SOL_TYPE_CAPABILITY_OPERATION
        && actual.definition < lowerer->syntax->capability_member_count) {
        const SolCapabilityMember *member
            = &lowerer->syntax->capability_members[actual.definition];
        if (member->return_type_id >= lowerer->types->declared_type_count) return false;
        parameter = member->first_parameter;
        result = lowerer->types->declared_types[member->return_type_id];
    } else {
        return false;
    }
    size_t index = 0;
    while (parameter != SOL_AST_NONE && index < expected->parameter_count) {
        if (parameter >= lowerer->syntax->parameter_count
            || lowerer->syntax->parameters[parameter].type_id
                >= lowerer->types->declared_type_count) return false;
        SolType actual_parameter = lowerer->types->declared_types[
            lowerer->syntax->parameters[parameter].type_id
        ];
        if (actual_parameter.kind != expected->parameters[index].kind
            || actual_parameter.definition != expected->parameters[index].definition
            || lowerer->syntax->parameters[parameter].access
                != expected->accesses[index]) return false;
        parameter = lowerer->syntax->parameters[parameter].next;
        ++index;
    }
    return parameter == SOL_AST_NONE && index == expected->parameter_count
        && result.kind == expected->result.kind
        && result.definition == expected->result.definition;
}

static bool sol_contract_construction_argument_matches(
    const SolContractLowerer *lowerer,
    SolExprId expression,
    SolType expected
) {
    SolType actual = lowerer->types->expressions[expression];
    if (actual.kind == expected.kind && actual.definition == expected.definition) return true;
    for (size_t index = 0; index < lowerer->types->function_coercion_count; ++index) {
        const SolFunctionCoercion *coercion
            = &lowerer->types->function_coercions[index];
        if (coercion->expression == expression
            && coercion->expected.kind == expected.kind
            && coercion->expected.definition == expected.definition
            && sol_contract_coercion_shape_matches(lowerer, coercion, actual)) return true;
    }
    return false;
}

static bool sol_contract_validate(SolContractLowerer *lowerer) {
    const SolSyntaxTree *syntax = lowerer->syntax;
    const SolHirModule *hir = lowerer->hir;
    const SolTypeTable *types = lowerer->types;
    const SolEffectTable *effects = lowerer->effects;
    SolContractTable *contracts = lowerer->contracts;
    if (!sol_syntax_contracts_validate(lowerer->source, syntax)
        || hir->definition_count != syntax->item_count
        || hir->resolution_count != syntax->expression_count
        || hir->type_resolution_count != syntax->type_count
        || hir->local_count > hir->local_capacity
        || syntax->type_count > syntax->type_capacity
        || syntax->type_argument_count > syntax->type_argument_capacity
        || syntax->type_parameter_count > syntax->type_parameter_capacity
        || syntax->effect_parameter_count > syntax->effect_parameter_capacity
        || hir->effect_resolution_count != syntax->effect_count
        || hir->type_effect_resolution_count != syntax->type_count
        || hir->trait_resolution_count != syntax->item_count
        || hir->bound_resolution_count != syntax->type_parameter_count
        || types->expression_count != syntax->expression_count
        || types->local_count != hir->local_count
        || types->definition_count != syntax->item_count
        || types->declared_type_count != syntax->type_count
        || types->type_application_count > types->type_application_capacity
        || types->type_application_argument_count
            > types->type_application_argument_capacity
        || types->function_type_count > types->function_type_capacity
        || types->call_instantiation_count != syntax->expression_count
        || types->call_instantiation_argument_count
            > types->call_instantiation_argument_capacity
        || types->variant_constructor_count > types->variant_constructor_capacity
        || types->method_resolution_count != syntax->expression_count
        || types->member_resolution_count != syntax->expression_count
        || types->tuple_projection_count != syntax->expression_count
        || types->pattern_resolution_count != syntax->pattern_count
        || types->pattern_child_resolution_count != syntax->pattern_binding_count
        || types->argument_resolution_count != syntax->argument_count
        || types->implementation_target_count != syntax->item_count
        || types->representation_count != syntax->item_count
        || types->construction_count != syntax->expression_count
        || types->loop_fact_count != syntax->statement_count
        || types->unreachable_fact_count != syntax->statement_count
        || effects->function_count != syntax->item_count
        || effects->capability_member_count != syntax->capability_member_count
        || effects->trait_method_count != syntax->trait_method_count
        || effects->call_instantiation_count != syntax->expression_count
        || effects->call_instantiation_count > effects->call_instantiation_capacity
        || effects->call_argument_count > effects->call_argument_capacity
        || effects->call_row_count > effects->call_row_capacity
        || (hir->definition_count != 0 && hir->definitions == NULL)
        || (hir->resolution_count != 0 && hir->resolutions == NULL)
        || (hir->resolution_count != 0 && hir->expression_owners == NULL)
        || (hir->type_resolution_count != 0 && hir->type_resolutions == NULL)
        || (hir->local_count != 0 && hir->locals == NULL)
        || (syntax->type_count != 0 && syntax->types == NULL)
        || (syntax->type_argument_count != 0 && syntax->type_arguments == NULL)
        || (syntax->type_parameter_count != 0 && syntax->type_parameters == NULL)
        || (syntax->effect_parameter_count != 0 && syntax->effect_parameters == NULL)
        || (hir->effect_resolution_count != 0 && hir->effect_resolutions == NULL)
        || (hir->type_effect_resolution_count != 0
            && hir->type_effect_resolutions == NULL)
        || (hir->trait_resolution_count != 0 && hir->trait_resolutions == NULL)
        || (hir->bound_resolution_count != 0 && hir->bound_resolutions == NULL)
        || (types->expression_count != 0 && types->expressions == NULL)
        || (types->local_count != 0 && types->locals == NULL)
        || (types->definition_count != 0 && types->definitions == NULL)
        || (types->declared_type_count != 0 && types->declared_types == NULL)
        || (types->type_application_capacity != 0 && types->type_applications == NULL)
        || (types->type_application_argument_capacity != 0
            && types->type_application_arguments == NULL)
        || (types->function_type_capacity != 0 && types->function_types == NULL)
        || (types->call_instantiation_count != 0
            && types->call_instantiations == NULL)
        || (types->call_instantiation_argument_capacity != 0
            && types->call_instantiation_arguments == NULL)
        || (types->variant_constructor_capacity != 0
            && types->variant_constructors == NULL)
        || (types->method_resolution_count != 0 && types->method_resolutions == NULL)
        || (types->member_resolution_count != 0
            && (types->field_resolutions == NULL || types->variant_resolutions == NULL))
        || (types->tuple_projection_count != 0 && types->tuple_projections == NULL)
        || (types->pattern_resolution_count != 0
            && (types->pattern_variant_resolutions == NULL
                || types->pattern_types == NULL))
        || (types->pattern_child_resolution_count != 0
            && (types->pattern_field_resolutions == NULL
                || types->pattern_tuple_ordinals == NULL))
        || (types->argument_resolution_count != 0
            && types->argument_field_resolutions == NULL)
        || (types->implementation_target_count != 0
            && types->implementation_targets == NULL)
        || (types->representation_count != 0 && types->representations == NULL)
        || (types->construction_count != 0 && types->constructions == NULL)
        || (types->loop_fact_count != 0 && types->loop_facts == NULL)
        || (types->unreachable_fact_count != 0
            && types->unreachable_facts == NULL)
        || (effects->function_count != 0 && effects->functions == NULL)
        || (effects->capability_member_count != 0
            && effects->capability_members == NULL)
        || (effects->trait_method_count != 0 && effects->trait_methods == NULL)
        || (effects->call_instantiation_count != 0
            && effects->call_instantiations == NULL)
        || (effects->call_argument_capacity != 0 && effects->call_arguments == NULL)
        || (effects->call_row_capacity != 0 && effects->call_rows == NULL)
        || contracts->obligations != NULL || contracts->obligation_count != 0
        || contracts->snapshots != NULL || contracts->snapshot_count != 0
        || contracts->snapshot_capacity != 0 || contracts->expression_snapshots != NULL
        || contracts->expression_count != 0 || contracts->loop_obligations != NULL
        || contracts->loop_obligation_count != 0
        || contracts->loop_obligation_capacity != 0
        || contracts->unreachable_obligations != NULL
        || contracts->unreachable_obligation_count != 0
        || contracts->unreachable_obligation_capacity != 0) {
        return false;
    }
    for (size_t statement = 0; statement < syntax->statement_count; ++statement) {
        const SolLoopFact *fact = &types->loop_facts[statement];
        bool loop = syntax->statements[statement].kind == SOL_STATEMENT_LOOP
            || syntax->statements[statement].kind == SOL_STATEMENT_WHILE;
        bool member_owner = loop && fact->owner < syntax->item_count
            && fact->owner_member < syntax->capability_member_count
            && syntax->capability_members[fact->owner_member].owner_item == fact->owner;
        bool method_owner = loop && fact->owner < syntax->item_count
            && fact->owner_trait_method < syntax->trait_method_count
            && syntax->trait_methods[fact->owner_trait_method].owner_item == fact->owner;
        bool item_owner = loop && fact->owner < syntax->item_count
            && fact->owner_member == SOL_AST_NONE
            && fact->owner_trait_method == SOL_AST_NONE
            && (syntax->items[fact->owner].kind == SOL_ITEM_FUNCTION
                || syntax->items[fact->owner].kind == SOL_ITEM_TEST);
        SolExprId owner_body = member_owner
            ? syntax->capability_members[fact->owner_member].body
            : method_owner
                ? syntax->trait_methods[fact->owner_trait_method].body
                : item_owner ? syntax->items[fact->owner].body : SOL_AST_NONE;
        SolSpan loop_span = syntax->statements[statement].span;
        bool body_contains_loop = owner_body < syntax->expression_count
            && syntax->expressions[owner_body].span.start <= loop_span.start
            && syntax->expressions[owner_body].span.end >= loop_span.end;
        bool predicate_contains_loop = false;
        for (size_t condition = 0;
            !predicate_contains_loop && condition < syntax->contract_condition_count;
            ++condition) {
            SolExprId expression = syntax->contract_conditions[condition].expression;
            predicate_contains_loop = expression < syntax->expression_count
                && syntax->expressions[expression].span.start <= loop_span.start
                && syntax->expressions[expression].span.end >= loop_span.end;
        }
        if (fact->is_loop != loop
            || (!loop && (fact->reachable_backedge || fact->reachable_break
                    || fact->owner != 0 || fact->owner_member != 0
                    || fact->owner_trait_method != 0))
            || (loop && (fact->owner >= syntax->item_count
                    || (fact->owner_member != SOL_AST_NONE
                        && fact->owner_member >= syntax->capability_member_count)
                    || (fact->owner_trait_method != SOL_AST_NONE
                        && fact->owner_trait_method >= syntax->trait_method_count)
                    || (fact->owner_member != SOL_AST_NONE
                        && fact->owner_trait_method != SOL_AST_NONE)
                    || (!member_owner && !method_owner && !item_owner)
                    || (!body_contains_loop && !predicate_contains_loop)))) {
            return false;
        }
        const SolUnreachableFact *unreachable
            = &types->unreachable_facts[statement];
        bool is_unreachable = syntax->statements[statement].kind
            == SOL_STATEMENT_UNREACHABLE;
        bool unreachable_member = is_unreachable
            && unreachable->owner < syntax->item_count
            && unreachable->owner_member < syntax->capability_member_count
            && syntax->capability_members[unreachable->owner_member].owner_item
                == unreachable->owner;
        bool unreachable_method = is_unreachable
            && unreachable->owner < syntax->item_count
            && unreachable->owner_trait_method < syntax->trait_method_count
            && syntax->trait_methods[unreachable->owner_trait_method].owner_item
                == unreachable->owner;
        bool unreachable_item = is_unreachable
            && unreachable->owner < syntax->item_count
            && unreachable->owner_member == SOL_AST_NONE
            && unreachable->owner_trait_method == SOL_AST_NONE
            && (syntax->items[unreachable->owner].kind == SOL_ITEM_FUNCTION
                || syntax->items[unreachable->owner].kind == SOL_ITEM_TEST);
        SolExprId unreachable_body = unreachable_member
            ? syntax->capability_members[unreachable->owner_member].body
            : unreachable_method
                ? syntax->trait_methods[unreachable->owner_trait_method].body
                : unreachable_item ? syntax->items[unreachable->owner].body : SOL_AST_NONE;
        SolSpan unreachable_span = syntax->statements[statement].span;
        bool body_contains_unreachable = unreachable_body < syntax->expression_count
            && syntax->expressions[unreachable_body].span.start <= unreachable_span.start
            && syntax->expressions[unreachable_body].span.end >= unreachable_span.end;
        bool predicate_contains_unreachable = false;
        for (size_t condition = 0; !predicate_contains_unreachable
            && condition < syntax->contract_condition_count; ++condition) {
            SolExprId expression = syntax->contract_conditions[condition].expression;
            predicate_contains_unreachable = expression < syntax->expression_count
                && syntax->expressions[expression].span.start <= unreachable_span.start
                && syntax->expressions[expression].span.end >= unreachable_span.end;
        }
        if (unreachable->is_unreachable != is_unreachable
            || (!is_unreachable && (unreachable->owner != 0
                    || unreachable->owner_member != 0
                    || unreachable->owner_trait_method != 0))
            || (is_unreachable && ((!unreachable_member && !unreachable_method
                        && !unreachable_item)
                    || (unreachable->owner_member != SOL_AST_NONE
                        && unreachable->owner_trait_method != SOL_AST_NONE)
                    || (!body_contains_unreachable
                        && !predicate_contains_unreachable)))) return false;
    }
    if (!sol_type_resolution_metadata_valid(lowerer->source, syntax, types)) return false;
    for (SolExprId expression = 0; expression < types->tuple_projection_count;
        ++expression) {
        size_t ordinal = types->tuple_projections[expression];
        if (ordinal != SOL_AST_NONE
            && !sol_contract_projection_name(lowerer->source,
                syntax->expressions[expression].as.field.name, ordinal)) return false;
    }
    for (size_t index = 0; index < syntax->effect_parameter_count; ++index) {
        const SolEffectParameter *parameter = &syntax->effect_parameters[index];
        if (!sol_contract_span_valid(lowerer->source, parameter->name)
            || parameter->owner_item >= syntax->item_count
            || syntax->items[parameter->owner_item].kind != SOL_ITEM_FUNCTION
            || syntax->items[parameter->owner_item].first_effect_parameter != index
            || parameter->next != SOL_AST_NONE) return false;
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
            && sol_contract_span_equal(
                lowerer->source,
                syntax->effect_parameters[parameter].name,
                effect->name
            );
        if ((parameter_use
                && (resolution.kind != SOL_EFFECT_RESOLUTION_PARAMETER
                    || resolution.target != parameter))
            || (!parameter_use
                && (resolution.kind != SOL_EFFECT_RESOLUTION_ATOM
                    || resolution.target != SOL_AST_NONE))) return false;
    }
    for (size_t index = 0; index < syntax->type_count; ++index) {
        const SolSyntaxType *type = &syntax->types[index];
        if (type->owner_item >= syntax->item_count) return false;
        SolEffectResolution resolution = hir->type_effect_resolutions[index];
        SolEffectParameterId parameter
            = syntax->items[type->owner_item].first_effect_parameter;
        bool parameter_use = type->has_effect_tail && parameter != SOL_AST_NONE
            && sol_contract_span_equal(
                lowerer->source,
                syntax->effect_parameters[parameter].name,
                type->effect_tail
            );
        if ((parameter_use
                && (resolution.kind != SOL_EFFECT_RESOLUTION_PARAMETER
                    || resolution.target != parameter))
            || (!parameter_use
                && (resolution.kind != SOL_EFFECT_RESOLUTION_ERROR
                    || resolution.target != SOL_AST_NONE))) return false;
    }
    for (size_t index = 0; index < syntax->expression_count; ++index) {
        if (!sol_contract_type_valid(lowerer, types->expressions[index])) return false;
        SolResolution resolution = hir->resolutions[index];
        if ((int)resolution.kind < 0
            || (int)resolution.kind < 0 || resolution.kind > SOL_RESOLUTION_REFINEMENT_SELF
            || (resolution.kind == SOL_RESOLUTION_DEFINITION
                && resolution.target >= hir->definition_count)
            || (resolution.kind == SOL_RESOLUTION_LOCAL
                && resolution.target >= hir->local_count)
            || (resolution.kind == SOL_RESOLUTION_BUILTIN
                && resolution.target > SOL_BUILTIN_NONE)
            || (resolution.kind == SOL_RESOLUTION_REFINEMENT_SELF
                && (syntax->expressions[index].kind != SOL_EXPR_PATH
                    || !sol_contract_span_text_equal(
                        lowerer->source, syntax->expressions[index].as.name, "self"
                    )
                    || resolution.target >= syntax->item_count
                    || syntax->items[resolution.target].kind != SOL_ITEM_TYPE
                    || syntax->items[resolution.target].flavor
                        != SOL_TYPE_DECLARATION_REFINED
                    || hir->expression_owners[index] != resolution.target))) {
            return false;
        }
    }
    size_t application_argument_offset = 0;
    for (size_t index = 0; index < types->type_application_count; ++index) {
        const SolTypeApplication *application = &types->type_applications[index];
        const SolType *arguments = NULL;
        size_t argument_count = 0;
        size_t expected = application->constructor == SOL_TYPE_CONSTRUCTOR_OPTION
            ? 1
            : application->constructor == SOL_TYPE_CONSTRUCTOR_RESULT ? 2
            : application->constructor == SOL_TYPE_CONSTRUCTOR_TUPLE
                ? application->argument_count : 0;
        if (application->constructor == SOL_TYPE_CONSTRUCTOR_USER
            && application->definition < syntax->item_count) {
            SolTypeParameterId parameter
                = syntax->items[application->definition].first_type_parameter;
            while (parameter != SOL_AST_NONE) {
                if (parameter >= syntax->type_parameter_count
                    || expected++ >= syntax->type_parameter_count) return false;
                parameter = syntax->type_parameters[parameter].next;
            }
        }
        if (application->argument_count != expected || expected == 0
            || application->argument_offset != application_argument_offset
            || !sol_type_application_arguments(
                types,
                (SolType){SOL_TYPE_APPLICATION, index},
                &arguments,
                &argument_count
            )
            || argument_count != expected
            || (application->constructor != SOL_TYPE_CONSTRUCTOR_USER
                && application->definition != SOL_AST_NONE)
            || (application->constructor == SOL_TYPE_CONSTRUCTOR_TUPLE
                && (expected < 2 || expected > 16))
            || (application->constructor == SOL_TYPE_CONSTRUCTOR_USER
                && (application->definition >= syntax->item_count
                    || (syntax->items[application->definition].kind != SOL_ITEM_RECORD
                        && syntax->items[application->definition].kind != SOL_ITEM_ENUM
                        && syntax->items[application->definition].kind != SOL_ITEM_TYPE)))) {
            return false;
        }
        for (size_t argument = 0; argument < expected; ++argument) {
            SolType type = arguments[argument];
            if (!sol_type_exact_reference_valid(syntax, types, type)
                || (type.kind == SOL_TYPE_APPLICATION && type.definition >= index)) {
                return false;
            }
        }
        for (size_t previous = 0; previous < index; ++previous) {
            const SolTypeApplication *other = &types->type_applications[previous];
            const SolType *other_arguments = NULL;
            size_t other_count = 0;
            if (!sol_type_application_arguments(
                types,
                (SolType){SOL_TYPE_APPLICATION, previous},
                &other_arguments,
                &other_count
            )) return false;
            bool equal = application->constructor == other->constructor
                && application->definition == other->definition
                && argument_count == other_count;
            for (size_t argument = 0; equal && argument < expected; ++argument) {
                equal = arguments[argument].kind == other_arguments[argument].kind
                    && arguments[argument].definition
                        == other_arguments[argument].definition;
            }
            if (equal) return false;
        }
        application_argument_offset += expected;
    }
    if (application_argument_offset != types->type_application_argument_count) return false;
    for (SolDefId definition = 0; definition < types->representation_count; ++definition) {
        const SolSyntaxItem *item = &syntax->items[definition];
        const SolTypeRepresentation *representation = &types->representations[definition];
        if (item->kind == SOL_ITEM_TYPE) {
            if (item->representation_type >= types->declared_type_count
                || representation->flavor != item->flavor
                || representation->representation.kind
                    != types->declared_types[item->representation_type].kind
                || representation->representation.definition
                    != types->declared_types[item->representation_type].definition
                || !sol_type_exact_reference_valid(
                    syntax, types, representation->representation
                )) return false;
        } else if (representation->flavor != SOL_TYPE_DECLARATION_NONE
            || representation->representation.kind != SOL_TYPE_UNKNOWN
            || representation->representation.definition != 0) return false;
    }
    for (SolExprId expression = 0; expression < types->construction_count; ++expression) {
        const SolTypeConstruction *construction = &types->constructions[expression];
        if (construction->definition == SOL_AST_NONE) {
            if (construction->representation.kind != SOL_TYPE_UNKNOWN
                || construction->representation.definition != 0
                || construction->result.kind != SOL_TYPE_UNKNOWN
                || construction->result.definition != 0) return false;
            continue;
        }
        if (construction->definition >= syntax->item_count
            || syntax->items[construction->definition].kind != SOL_ITEM_TYPE
            || syntax->items[construction->definition].flavor
                != SOL_TYPE_DECLARATION_DISTINCT
            || syntax->expressions[expression].kind != SOL_EXPR_CALL
            || !sol_type_exact_reference_valid(syntax, types, construction->representation)
            || !sol_type_exact_reference_valid(syntax, types, construction->result)
            || construction->result.kind != types->expressions[expression].kind
            || construction->result.definition != types->expressions[expression].definition
            || sol_type_construction(types, expression) == NULL) return false;
        SolArgumentId argument = syntax->expressions[expression].as.call.first_argument;
        if (argument >= syntax->argument_count || syntax->arguments[argument].is_named
            || syntax->arguments[argument].next != SOL_AST_NONE
            || !sol_contract_construction_argument_matches(
                lowerer,
                syntax->arguments[argument].value,
                construction->representation
            )) return false;
        SolExprId callee = syntax->expressions[expression].as.call.callee;
        SolType callee_type = types->expressions[callee];
        const SolTypeApplication *callee_application
            = sol_type_application(types, callee_type);
        SolDefId callee_definition = callee_type.kind == SOL_TYPE_NOMINAL
            ? callee_type.definition
            : callee_application != NULL
                    && callee_application->constructor == SOL_TYPE_CONSTRUCTOR_USER
                ? callee_application->definition
                : SOL_AST_NONE;
        if (callee_definition != construction->definition) return false;
    }
    size_t call_argument_total = 0;
    for (size_t expression = 0; expression < types->call_instantiation_count; ++expression) {
        const SolCallInstantiation *instantiation
            = &types->call_instantiations[expression];
        if (instantiation->function == SOL_AST_NONE) {
            if (instantiation->argument_count != 0 || instantiation->argument_offset != 0) {
                return false;
            }
            continue;
        }
        if (syntax->expressions[expression].kind != SOL_EXPR_CALL
            || instantiation->function >= syntax->item_count
            || syntax->items[instantiation->function].kind != SOL_ITEM_FUNCTION) {
            return false;
        }
        size_t expected = 0;
        SolTypeParameterId parameter
            = syntax->items[instantiation->function].first_type_parameter;
        while (parameter != SOL_AST_NONE) {
            if (parameter >= syntax->type_parameter_count
                || expected++ >= syntax->type_parameter_count) return false;
            parameter = syntax->type_parameters[parameter].next;
        }
        const SolType *arguments = NULL;
        size_t argument_count = 0;
        SolExprId callee = syntax->expressions[expression].as.call.callee;
        if (expected == 0 || instantiation->argument_count != expected
            || callee >= types->expression_count
            || types->expressions[callee].kind != SOL_TYPE_FUNCTION
            || types->expressions[callee].definition != instantiation->function
            || !sol_type_call_instantiation_arguments(
                types,
                expression,
                &arguments,
                &argument_count
            )
            || argument_count != expected
            || !sol_type_call_instantiation_valid(
                lowerer->source,
                syntax,
                types,
                expression
            )) {
            return false;
        }
        if (call_argument_total > SIZE_MAX - argument_count) return false;
        call_argument_total += argument_count;
        for (size_t argument = 0; argument < argument_count; ++argument) {
            if (!sol_type_exact_reference_valid(syntax, types, arguments[argument])) {
                return false;
            }
        }
        for (size_t previous = 0; previous < expression; ++previous) {
            const SolCallInstantiation *other = &types->call_instantiations[previous];
            if (other->function == SOL_AST_NONE) continue;
            size_t left_end = instantiation->argument_offset + argument_count;
            size_t right_end = other->argument_offset + other->argument_count;
            if (instantiation->argument_offset < right_end
                && other->argument_offset < left_end) return false;
        }
    }
    if (call_argument_total != types->call_instantiation_argument_count) return false;
    for (SolExprId expression = 0; expression < types->method_resolution_count; ++expression) {
        const SolMethodResolution *method = &types->method_resolutions[expression];
        if (method->kind == SOL_METHOD_RESOLUTION_NONE) {
            if (method->call != SOL_AST_NONE || method->trait != SOL_AST_NONE
                || method->requirement != SOL_AST_NONE
                || method->implementation != SOL_AST_NONE
                || method->method != SOL_AST_NONE) return false;
            continue;
        }
        if (!sol_contract_method_resolution_valid(lowerer, expression, method)) return false;
    }
    for (size_t index = 0; index < types->variant_constructor_count; ++index) {
        const SolVariantConstructor *constructor = &types->variant_constructors[index];
        if (constructor->variant >= syntax->variant_count
            || !sol_type_exact_reference_valid(syntax, types, constructor->owner)) {
            return false;
        }
        SolDefId owner = syntax->variants[constructor->variant].owner_item;
        bool owner_matches = constructor->owner.kind == SOL_TYPE_NOMINAL
            && constructor->owner.definition == owner;
        const SolTypeApplication *application = sol_type_application(
            types,
            constructor->owner
        );
        owner_matches = owner_matches
            || (application != NULL
                && application->constructor == SOL_TYPE_CONSTRUCTOR_USER
                && application->definition == owner);
        if (!owner_matches || owner >= syntax->item_count
            || syntax->items[owner].kind != SOL_ITEM_ENUM
            || syntax->variants[constructor->variant].first_field == SOL_AST_NONE) {
            return false;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            const SolVariantConstructor *other = &types->variant_constructors[previous];
            if (other->variant == constructor->variant
                && other->owner.kind == constructor->owner.kind
                && other->owner.definition == constructor->owner.definition) return false;
        }
    }
    for (size_t index = 0; index < types->function_type_count; ++index) {
        const SolFunctionType *function = &types->function_types[index];
        if ((function->parameter_count != 0
                && (function->parameters == NULL || function->accesses == NULL))
            || (function->effects.count != 0 && function->effects.atoms == NULL)
            || !sol_type_exact_reference_valid(syntax, types, function->result)
            || (function->effect_parameter != SOL_AST_NONE
                && function->effect_parameter >= syntax->effect_parameter_count)
            || (function->result.kind == SOL_TYPE_FUNCTION_SIGNATURE
                && function->result.definition >= index)) return false;
        for (size_t parameter = 0; parameter < function->parameter_count; ++parameter) {
            SolType type = function->parameters[parameter];
            if (function->accesses[parameter] > SOL_ACCESS_EXCLUSIVE
                || !sol_type_exact_reference_valid(syntax, types, type)
                || (type.kind == SOL_TYPE_FUNCTION_SIGNATURE
                    && type.definition >= index)) {
                return false;
            }
        }
    }
    const SolType *type_arenas[] = {
        types->expressions,
        types->locals,
        types->definitions,
        types->declared_types,
    };
    const size_t type_counts[] = {
        types->expression_count,
        types->local_count,
        types->definition_count,
        types->declared_type_count,
    };
    for (size_t arena = 0; arena < sizeof(type_arenas) / sizeof(type_arenas[0]); ++arena) {
        for (size_t index = 0; index < type_counts[arena]; ++index) {
            SolType type = type_arenas[arena][index];
            bool recovery = type.kind == SOL_TYPE_UNKNOWN || type.kind == SOL_TYPE_ERROR
                || type.kind == SOL_TYPE_NEVER;
            if ((recovery && type.definition != 0)
                || (!recovery && !sol_type_exact_reference_valid(syntax, types, type))) {
                return false;
            }
        }
    }
    for (size_t index = 0; index < effects->function_count; ++index) {
        const SolEffectRow *row = &effects->functions[index];
        SolEffectParameterId expected = syntax->items[index].kind == SOL_ITEM_FUNCTION
            ? syntax->items[index].first_effect_parameter
            : SOL_AST_NONE;
        if ((row->count != 0 && row->atoms == NULL)
            || row->effect_parameter != expected
            || (syntax->items[index].kind != SOL_ITEM_FUNCTION
                && syntax->items[index].kind != SOL_ITEM_TEST && row->count != 0)) {
            return false;
        }
    }
    for (size_t index = 0; index < effects->capability_member_count; ++index) {
        if (effects->capability_members[index].count != 0
            && effects->capability_members[index].atoms == NULL) return false;
    }
    for (size_t index = 0; index < effects->trait_method_count; ++index) {
        const SolEffectRow *row = &effects->trait_methods[index];
        if ((row->count != 0 && row->atoms == NULL)
            || row->effect_parameter != SOL_AST_NONE) return false;
        for (size_t atom = 0; atom < row->count; ++atom) {
            if ((int)row->atoms[atom].argument_kind < 0
                || (int)row->atoms[atom].argument_kind < 0
                || row->atoms[atom].argument_kind > SOL_EFFECT_ATOM_STATIC_PATH
                || row->atoms[atom].parameter != SOL_AST_NONE
                || !sol_contract_span_valid(lowerer->source, row->atoms[atom].name)
                || !sol_contract_span_valid(lowerer->source, row->atoms[atom].argument)
                || !sol_contract_span_valid(lowerer->source, row->atoms[atom].span)) {
                return false;
            }
        }
    }
    size_t argument_total = 0;
    size_t row_total = 0;
    for (SolExprId expression = 0;
        expression < effects->call_instantiation_count;
        ++expression) {
        const SolEffectCallInstantiation *entry
            = &effects->call_instantiations[expression];
        if (entry->call == SOL_AST_NONE) {
            if (entry->function != SOL_AST_NONE || entry->parameter != SOL_AST_NONE
                || entry->argument_offset != 0 || entry->argument_count != 0
                || entry->row_offset != 0 || entry->row_count != 0) return false;
            continue;
        }
        const SolEffectAtom *arguments = NULL;
        const SolEffectAtom *row = NULL;
        size_t argument_count = 0;
        size_t row_count = 0;
        SolExprId callee = syntax->expressions[expression].as.call.callee;
        if (entry->call != expression
            || syntax->expressions[expression].kind != SOL_EXPR_CALL
            || entry->function >= syntax->item_count
            || syntax->items[entry->function].kind != SOL_ITEM_FUNCTION
            || syntax->items[entry->function].first_effect_parameter == SOL_AST_NONE
            || callee >= types->expression_count
            || types->expressions[callee].kind != SOL_TYPE_FUNCTION
            || types->expressions[callee].definition != entry->function
            || entry->argument_offset != argument_total
            || entry->row_offset != row_total
            || !sol_effect_call_arguments(
                effects, expression, &arguments, &argument_count
            )
            || !sol_effect_call_row(effects, expression, &row, &row_count)
            || argument_count != entry->argument_count
            || row_count != entry->row_count
            || (entry->parameter != SOL_AST_NONE
                && (entry->parameter >= syntax->effect_parameter_count
                    || syntax->effect_parameters[entry->parameter].owner_item
                        != hir->expression_owners[expression]))
            || !sol_effect_call_instantiation_valid(
                lowerer->source,
                syntax,
                hir,
                types,
                effects,
                expression
            )) return false;
        if (argument_total > SIZE_MAX - argument_count
            || row_total > SIZE_MAX - row_count) return false;
        argument_total += argument_count;
        row_total += row_count;
        for (size_t atom = 0; atom < argument_count; ++atom) {
            if (arguments[atom].argument_kind != SOL_EFFECT_ATOM_NO_ARGUMENT
                && arguments[atom].argument_kind != SOL_EFFECT_ATOM_STATIC_PATH) {
                return false;
            }
        }
        for (size_t atom = 0; atom < row_count; ++atom) {
            if ((int)row[atom].argument_kind < 0
                || (int)row[atom].argument_kind < 0
                || row[atom].argument_kind > SOL_EFFECT_ATOM_SELF) return false;
            for (size_t previous = 0; previous < atom; ++previous) {
                bool same = row[previous].argument_kind == row[atom].argument_kind
                    && row[previous].name.start == row[atom].name.start
                    && row[previous].name.end == row[atom].name.end
                    && row[previous].parameter == row[atom].parameter
                    && row[previous].argument.start == row[atom].argument.start
                    && row[previous].argument.end == row[atom].argument.end;
                if (same) return false;
            }
        }
    }
    if (argument_total != effects->call_argument_count
        || row_total != effects->call_row_count) return false;
    return true;
}

static bool sol_contract_append_snapshot(
    SolContractLowerer *lowerer,
    SolExprId old_expression,
    SolExprId operand
) {
    SolContractTable *table = lowerer->contracts;
    if (table->snapshot_count == table->snapshot_capacity) {
        size_t capacity = table->snapshot_capacity == 0
            ? 8
            : table->snapshot_capacity * 2;
        if (capacity < table->snapshot_capacity
            || capacity > SIZE_MAX / sizeof(*table->snapshots)) {
            lowerer->allocation_failed = true;
            return false;
        }
        if (!sol_resource_charge_arena(capacity - table->snapshot_capacity)) {
            lowerer->allocation_failed = true;
            return false;
        }
        SolSnapshot *grown = realloc(
            table->snapshots,
            capacity * sizeof(*table->snapshots)
        );
        if (grown == NULL) {
            lowerer->allocation_failed = true;
            return false;
        }
        table->snapshots = grown;
        table->snapshot_capacity = capacity;
    }
    SolSnapshotId id = table->snapshot_count++;
    table->snapshots[id] = (SolSnapshot){
        .id = id,
        .obligation = lowerer->obligation->id,
        .old_expression = old_expression,
        .operand = operand,
        .type = lowerer->types->expressions[old_expression],
    };
    table->expression_snapshots[old_expression] = id;
    ++lowerer->obligation->snapshot_count;
    return true;
}

static bool sol_contract_append_loop_obligation(
    SolContractLowerer *lowerer,
    SolLoopObligationKind kind,
    SolStatementId statement,
    const SolLoopFact *fact,
    SolExprId expression,
    SolSpan span
) {
    SolContractTable *table = lowerer->contracts;
    if (table->loop_obligation_count == table->loop_obligation_capacity) {
        size_t capacity = table->loop_obligation_capacity == 0
            ? 8 : table->loop_obligation_capacity * 2;
        if (capacity < table->loop_obligation_capacity
            || capacity > SIZE_MAX / sizeof(*table->loop_obligations)) {
            lowerer->allocation_failed = true;
            return false;
        }
        if (!sol_resource_charge_arena(capacity - table->loop_obligation_capacity)) {
            lowerer->allocation_failed = true;
            return false;
        }
        SolLoopObligation *grown = realloc(
            table->loop_obligations,
            capacity * sizeof(*table->loop_obligations));
        if (grown == NULL) {
            lowerer->allocation_failed = true;
            return false;
        }
        table->loop_obligations = grown;
        table->loop_obligation_capacity = capacity;
    }
    size_t id = table->loop_obligation_count++;
    table->loop_obligations[id] = (SolLoopObligation){
        .id = id,
        .kind = kind,
        .loop_statement = statement,
        .owner = fact->owner,
        .owner_member = fact->owner_member,
        .owner_trait_method = fact->owner_trait_method,
        .expression = expression,
        .expression_type = lowerer->types->expressions[expression],
        .span = span,
    };
    return true;
}

static void sol_contract_expression(
    SolContractLowerer *lowerer,
    SolExprId expression_id,
    bool in_old
);

static void sol_contract_arguments(
    SolContractLowerer *lowerer,
    SolArgumentId argument,
    bool in_old
) {
    size_t traversed = 0;
    while (argument != SOL_AST_NONE) {
        if (argument >= lowerer->syntax->argument_count
            || traversed++ >= lowerer->syntax->argument_count) {
            lowerer->malformed = true;
            return;
        }
        const SolArgument *entry = &lowerer->syntax->arguments[argument];
        sol_contract_expression(lowerer, entry->value, in_old);
        argument = entry->next;
    }
}

static bool sol_contract_call_is_pure(
    const SolContractLowerer *lowerer,
    const SolExpr *call
) {
    SolExprId callee_id = call->as.call.callee;
    SolExprId call_id = (SolExprId)(call - lowerer->syntax->expressions);
    if (sol_type_construction(lowerer->types, call_id) != NULL) return true;
    SolType callee = lowerer->types->expressions[callee_id];
    if (callee.kind == SOL_TYPE_FUNCTION
        && callee.definition < lowerer->effects->function_count) {
        if (lowerer->effects->functions[callee.definition].effect_parameter
            != SOL_AST_NONE) {
            const SolEffectCallInstantiation *entry = sol_effect_call_instantiation(
                lowerer->effects,
                (SolExprId)(call - lowerer->syntax->expressions)
            );
            return entry != NULL && entry->function == callee.definition
                && entry->parameter == SOL_AST_NONE && entry->row_count == 0;
        }
        return lowerer->effects->functions[callee.definition].count == 0;
    }
    if (callee.kind == SOL_TYPE_CAPABILITY_OPERATION
        && callee.definition < lowerer->effects->capability_member_count) {
        return lowerer->effects->capability_members[callee.definition].count == 0;
    }
    if (callee.kind == SOL_TYPE_FUNCTION_SIGNATURE
        && callee.definition < lowerer->types->function_type_count) {
        return lowerer->types->function_types[callee.definition].effect_parameter
                == SOL_AST_NONE
            && lowerer->types->function_types[callee.definition].effects.count == 0;
    }
    if (callee.kind == SOL_TYPE_TRAIT_METHOD) {
        const SolMethodResolution *method = sol_type_method_resolution(
            lowerer->types, (SolExprId)(call - lowerer->syntax->expressions)
        );
        size_t target = method != NULL
            && method->kind == SOL_METHOD_RESOLUTION_REQUIREMENT
            ? method->requirement
            : method != NULL ? method->method : SOL_AST_NONE;
        return target < lowerer->effects->trait_method_count
            && lowerer->effects->trait_methods[target].count == 0;
    }
    if (callee.kind == SOL_TYPE_VARIANT) return true;
    SolResolution resolution = lowerer->hir->resolutions[callee_id];
    return resolution.kind == SOL_RESOLUTION_BUILTIN;
}

static void sol_contract_statements(
    SolContractLowerer *lowerer,
    SolStatementId statement,
    bool in_old
) {
    size_t traversed = 0;
    while (statement != SOL_AST_NONE) {
        if (statement >= lowerer->syntax->statement_count
            || traversed++ >= lowerer->syntax->statement_count) {
            lowerer->malformed = true;
            return;
        }
        const SolStatement *entry = &lowerer->syntax->statements[statement];
        if ((int)entry->kind < 0 || entry->kind > SOL_STATEMENT_REQUIRE) {
            lowerer->malformed = true;
            return;
        }
        if ((entry->kind == SOL_STATEMENT_LOOP
                || entry->kind == SOL_STATEMENT_WHILE)
            && (entry->as.loop_statement.body
                    >= lowerer->syntax->expression_count
                || lowerer->syntax->expressions[
                    entry->as.loop_statement.body
                ].kind != SOL_EXPR_BLOCK
                || (entry->kind == SOL_STATEMENT_LOOP
                    && entry->as.loop_statement.condition != SOL_AST_NONE)
                || (entry->kind == SOL_STATEMENT_WHILE
                    && entry->as.loop_statement.condition
                        >= lowerer->syntax->expression_count))) {
            lowerer->malformed = true;
            return;
        }
        if ((entry->kind == SOL_STATEMENT_BREAK
                || entry->kind == SOL_STATEMENT_CONTINUE)
            && (entry->next != SOL_AST_NONE
                || entry->as.expression != SOL_AST_NONE)) {
            lowerer->malformed = true;
            return;
        }
        if (entry->kind == SOL_STATEMENT_RETURN) {
            sol_contract_error(
                lowerer,
                "SOL-CONTRACT-002",
                entry->span,
                "return is not allowed in a contract predicate"
            );
        }
        if (entry->kind == SOL_STATEMENT_REGION) {
            sol_contract_error(lowerer, "SOL-CONTRACT-002", entry->span,
                "region statements are not allowed in contract or refinement predicates");
        }
        if (entry->kind == SOL_STATEMENT_ASSIGNMENT) {
            sol_contract_error(lowerer, "SOL-CONTRACT-002", entry->span,
                entry->as.assignment.operator_kind == SOL_TOKEN_EQUAL
                    ? "assignment is not allowed in contract or refinement predicates"
                    : "compound assignment is not allowed in contract or refinement predicates");
            sol_contract_expression(lowerer, entry->as.assignment.target, in_old);
        }
        if (entry->kind == SOL_STATEMENT_MODIFY) {
            sol_contract_error(lowerer, "SOL-CONTRACT-002", entry->span,
                "modify is not allowed in contract or refinement predicates");
            sol_contract_expression(
                lowerer, entry->as.modify.target, in_old);
        }
        if (entry->kind == SOL_STATEMENT_VAR) {
            sol_contract_error(lowerer, "SOL-CONTRACT-002", entry->span,
                entry->as.let_statement.value == SOL_AST_NONE
                    ? "uninitialized bindings are not allowed in contract or refinement predicates"
                    : "mutable bindings are not allowed in contract or refinement predicates");
        }
        if (entry->kind == SOL_STATEMENT_LOOP
            || entry->kind == SOL_STATEMENT_WHILE
            || entry->kind == SOL_STATEMENT_BREAK
            || entry->kind == SOL_STATEMENT_CONTINUE) {
            sol_contract_error(
                lowerer,
                "SOL-CONTRACT-002",
                entry->span,
                entry->kind == SOL_STATEMENT_LOOP
                    || entry->kind == SOL_STATEMENT_WHILE
                    ? "loops are not allowed in contract or refinement predicates"
                    : "loop exits are not allowed in contract or refinement predicates"
            );
        }
        if (entry->kind == SOL_STATEMENT_PANIC
            || entry->kind == SOL_STATEMENT_UNREACHABLE
            || entry->kind == SOL_STATEMENT_REQUIRE) {
            sol_contract_error(lowerer, "SOL-CONTRACT-002", entry->span,
                "termination statements are not allowed in contract, refinement, or proof predicates");
        }
        SolExprId value = entry->kind == SOL_STATEMENT_LOOP
                || entry->kind == SOL_STATEMENT_WHILE
            ? entry->as.loop_statement.body
            : entry->kind == SOL_STATEMENT_BREAK
                    || entry->kind == SOL_STATEMENT_CONTINUE
                ? SOL_AST_NONE
            : entry->kind == SOL_STATEMENT_LET
                || entry->kind == SOL_STATEMENT_VAR
            ? entry->as.let_statement.value
            : entry->kind == SOL_STATEMENT_ASSIGNMENT
                ? entry->as.assignment.value
            : entry->kind == SOL_STATEMENT_REGION
                ? entry->as.region_statement.body
            : entry->kind == SOL_STATEMENT_MODIFY
                ? entry->as.modify.body
            : entry->kind == SOL_STATEMENT_PANIC
                ? entry->as.panic_statement.message
            : entry->kind == SOL_STATEMENT_UNREACHABLE
                ? entry->as.unreachable_statement.proof
            : entry->kind == SOL_STATEMENT_REQUIRE
                ? entry->as.require_statement.fallback_block
                : entry->as.expression;
        if (entry->kind == SOL_STATEMENT_WHILE) {
            sol_contract_expression(
                lowerer, entry->as.loop_statement.condition, in_old
            );
        } else if (entry->kind == SOL_STATEMENT_REQUIRE) {
            sol_contract_expression(
                lowerer, entry->as.require_statement.condition, in_old
            );
        }
        if (value != SOL_AST_NONE) sol_contract_expression(lowerer, value, in_old);
        statement = entry->next;
    }
}

static void sol_contract_expression(
    SolContractLowerer *lowerer,
    SolExprId expression_id,
    bool in_old
) {
    if (expression_id >= lowerer->syntax->expression_count) {
        lowerer->malformed = true;
        return;
    }
    if (lowerer->depth >= 256) {
        if (!lowerer->depth_reported) {
            sol_contract_error(
                lowerer,
                "SOL-CONTRACT-004",
                lowerer->syntax->expressions[expression_id].span,
                "contract expression exceeds the lowering limit of 256"
            );
            lowerer->depth_reported = true;
        }
        return;
    }
    ++lowerer->depth;
    const SolExpr *expression = &lowerer->syntax->expressions[expression_id];
    switch (expression->kind) {
        case SOL_EXPR_UNARY:
            sol_contract_expression(lowerer, expression->as.unary.operand, in_old);
            break;
        case SOL_EXPR_BINARY:
            sol_contract_expression(lowerer, expression->as.binary.left, in_old);
            sol_contract_expression(lowerer, expression->as.binary.right, in_old);
            break;
        case SOL_EXPR_CALL:
            if (!sol_contract_call_is_pure(lowerer, expression)) {
                sol_contract_error(
                    lowerer,
                    "SOL-CONTRACT-002",
                    expression->span,
                    "contract predicates may only call pure functions or operations"
                );
            }
            sol_contract_expression(lowerer, expression->as.call.callee, in_old);
            sol_contract_arguments(lowerer, expression->as.call.first_argument, in_old);
            break;
        case SOL_EXPR_TYPE_APPLICATION:
            sol_contract_expression(
                lowerer,
                expression->as.type_application.base,
                in_old
            );
            break;
        case SOL_EXPR_TUPLE:
            sol_contract_arguments(lowerer, expression->as.tuple.first_element, in_old);
            break;
        case SOL_EXPR_FIELD:
            sol_contract_expression(lowerer, expression->as.field.base, in_old);
            break;
        case SOL_EXPR_RECORD:
            sol_contract_expression(lowerer, expression->as.record.type, in_old);
            sol_contract_arguments(lowerer, expression->as.record.first_field, in_old);
            break;
        case SOL_EXPR_IF:
            sol_contract_expression(lowerer, expression->as.if_expr.condition, in_old);
            sol_contract_expression(lowerer, expression->as.if_expr.then_branch, in_old);
            sol_contract_expression(lowerer, expression->as.if_expr.else_branch, in_old);
            break;
        case SOL_EXPR_MATCH: {
            sol_contract_expression(lowerer, expression->as.match_expr.scrutinee, in_old);
            SolMatchArmId arm = expression->as.match_expr.first_arm;
            size_t traversed = 0;
            while (arm != SOL_AST_NONE) {
                if (arm >= lowerer->syntax->match_arm_count
                    || traversed++ >= lowerer->syntax->match_arm_count) {
                    lowerer->malformed = true;
                    break;
                }
                const SolMatchArm *entry = &lowerer->syntax->match_arms[arm];
                if (entry->guard != SOL_AST_NONE) {
                    sol_contract_expression(lowerer, entry->guard, in_old);
                }
                sol_contract_expression(
                    lowerer,
                    entry->value,
                    in_old
                );
                arm = entry->next;
            }
            break;
        }
        case SOL_EXPR_BLOCK:
            sol_contract_statements(lowerer, expression->as.block.first_statement, in_old);
            break;
        case SOL_EXPR_PROPAGATE:
            sol_contract_error(
                lowerer,
                "SOL-CONTRACT-002",
                expression->span,
                "effect propagation is not allowed in a contract predicate"
            );
            sol_contract_expression(lowerer, expression->as.propagated, in_old);
            break;
        case SOL_EXPR_HANDLE:
            sol_contract_error(
                lowerer,
                "SOL-CONTRACT-002",
                expression->span,
                "effect handlers are not allowed in a contract predicate"
            );
            sol_contract_expression(lowerer, expression->as.handle.authority, in_old);
            sol_contract_expression(lowerer, expression->as.handle.provider, in_old);
            sol_contract_expression(lowerer, expression->as.handle.body, in_old);
            break;
        case SOL_EXPR_RESULT:
            if (lowerer->validating_guard) {
                break;
            }
            if (lowerer->in_loop_specification) {
                sol_contract_error(lowerer, "SOL-CONTRACT-003", expression->span,
                    "result is unavailable in a loop specification");
            } else if (lowerer->obligation->owner_kind == SOL_CONTRACT_OWNER_TYPE) {
                sol_contract_error(
                    lowerer,
                    "SOL-CONTRACT-003",
                    expression->span,
                    "result is unavailable in a refinement predicate"
                );
            } else if (in_old) {
                sol_contract_error(
                    lowerer,
                    "SOL-CONTRACT-003",
                    expression->span,
                    "old(result) is not a valid entry-state snapshot"
                );
            }
            break;
        case SOL_EXPR_OLD:
            if (lowerer->validating_guard) {
                sol_contract_expression(lowerer, expression->as.old_expression, true);
                break;
            }
            if (lowerer->in_loop_specification) {
                sol_contract_error(lowerer, "SOL-CONTRACT-003", expression->span,
                    "old is unavailable in a loop specification");
                sol_contract_expression(lowerer, expression->as.old_expression, true);
                break;
            }
            if (lowerer->obligation->owner_kind == SOL_CONTRACT_OWNER_TYPE) {
                sol_contract_error(
                    lowerer,
                    "SOL-CONTRACT-003",
                    expression->span,
                    "old is unavailable in a refinement predicate"
                );
                sol_contract_expression(lowerer, expression->as.old_expression, true);
                break;
            }
            if (in_old) {
                sol_contract_error(
                    lowerer,
                    "SOL-CONTRACT-003",
                    expression->span,
                    "nested old expressions are not allowed"
                );
            }
            sol_contract_append_snapshot(
                lowerer,
                expression_id,
                expression->as.old_expression
            );
            sol_contract_expression(lowerer, expression->as.old_expression, true);
            break;
        default:
            break;
    }
    --lowerer->depth;
}

bool sol_contract_validate_guard(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolHirModule *hir,
    const SolTypeTable *types,
    const SolEffectTable *effects,
    SolExprId guard,
    SolDiagnostics *diagnostics
) {
    if (source == NULL || syntax == NULL || hir == NULL || types == NULL
        || effects == NULL || diagnostics == NULL || guard >= syntax->expression_count) {
        return false;
    }
    SolContractLowerer lowerer = {
        .source = source,
        .syntax = syntax,
        .hir = hir,
        .types = types,
        .effects = effects,
        .diagnostics = diagnostics,
        .validating_guard = true,
    };
    sol_contract_expression(&lowerer, guard, false);
    return !lowerer.malformed && !lowerer.allocation_failed
        && !diagnostics->allocation_failed;
}

static SolType sol_contract_owner_result(
    const SolContractLowerer *lowerer,
    const SolContractClause *clause
) {
    if (clause->owner_kind == SOL_CONTRACT_OWNER_ITEM) {
        return lowerer->types->definitions[clause->owner];
    }
    if (clause->owner_kind == SOL_CONTRACT_OWNER_TYPE) {
        return (SolType){.kind = SOL_TYPE_ERROR};
    }
    const SolCapabilityMember *member
        = &lowerer->syntax->capability_members[clause->owner];
    return lowerer->types->declared_types[member->return_type_id];
}

static SolResultBinding sol_contract_result_binding(
    const SolContractLowerer *lowerer,
    const SolContractClause *clause,
    const SolContractCondition *condition
) {
    SolResultBinding binding = {0};
    if (clause->owner_kind == SOL_CONTRACT_OWNER_TYPE
        || clause->kind != SOL_CONTRACT_ENSURES
        || condition->outcome == SOL_CONTRACT_OUTCOME_FAILURE) {
        return binding;
    }
    binding.available = true;
    binding.type = sol_contract_owner_result(lowerer, clause);
    const SolTypeApplication *application = sol_type_application(
        lowerer->types,
        binding.type
    );
    const SolType *arguments = NULL;
    size_t argument_count = 0;
    if (condition->outcome == SOL_CONTRACT_OUTCOME_SUCCESS
        && application != NULL
        && application->constructor == SOL_TYPE_CONSTRUCTOR_RESULT
        && sol_type_application_arguments(
            lowerer->types,
            binding.type,
            &arguments,
            &argument_count
        )
        && argument_count == 2) {
        binding.type = arguments[0];
    }
    return binding;
}

static bool sol_contract_effect_row_total(
    const SolContractLowerer *lowerer,
    const SolEffectRow *row
) {
    if (row == NULL) return false;
    for (size_t index = 0; index < row->count; ++index) {
        const SolEffectAtom *atom = &row->atoms[index];
        if (atom->argument_kind == SOL_EFFECT_ATOM_NO_ARGUMENT
            && sol_contract_span_text_equal(lowerer->source, atom->name, "diverge")) {
            return false;
        }
    }
    return true;
}

static bool sol_contract_loop_owner_total(
    const SolContractLowerer *lowerer,
    const SolLoopFact *fact
) {
    if (fact->owner_member != SOL_AST_NONE) {
        return sol_contract_effect_row_total(
            lowerer, &lowerer->effects->capability_members[fact->owner_member]);
    }
    if (fact->owner_trait_method != SOL_AST_NONE) {
        return sol_contract_effect_row_total(
            lowerer, &lowerer->effects->trait_methods[fact->owner_trait_method]);
    }
    return sol_contract_effect_row_total(
        lowerer, &lowerer->effects->functions[fact->owner]);
}

static int sol_contract_loop_obligation_compare(const void *left, const void *right) {
    const SolLoopObligation *a = left;
    const SolLoopObligation *b = right;
    if (a->span.start != b->span.start) return a->span.start < b->span.start ? -1 : 1;
    if (a->span.end != b->span.end) return a->span.end < b->span.end ? -1 : 1;
    if (a->loop_statement != b->loop_statement) {
        return a->loop_statement < b->loop_statement ? -1 : 1;
    }
    return (int)a->kind - (int)b->kind;
}

static void sol_contract_lower_loops(SolContractLowerer *lowerer) {
    lowerer->in_loop_specification = true;
    lowerer->obligation = NULL;
    for (SolStatementId statement = 0;
        statement < lowerer->syntax->statement_count;
        ++statement) {
        const SolStatement *loop = &lowerer->syntax->statements[statement];
        if (loop->kind != SOL_STATEMENT_LOOP && loop->kind != SOL_STATEMENT_WHILE) continue;
        bool in_predicate = false;
        for (size_t condition = 0; !in_predicate
            && condition < lowerer->syntax->contract_condition_count; ++condition) {
            SolExprId expression
                = lowerer->syntax->contract_conditions[condition].expression;
            in_predicate = expression < lowerer->syntax->expression_count
                && lowerer->syntax->expressions[expression].span.start <= loop->span.start
                && lowerer->syntax->expressions[expression].span.end >= loop->span.end;
        }
        if (in_predicate) continue;
        const SolLoopFact *fact = &lowerer->types->loop_facts[statement];
        if (fact->reachable_backedge
            && loop->as.loop_statement.decreases == SOL_AST_NONE
            && sol_contract_loop_owner_total(lowerer, fact)) {
            sol_contract_error(lowerer, "SOL-CONTRACT-006", loop->span,
                "a total callable loop with a reachable backedge requires decreases");
        }
        SolLoopInvariantId invariant = loop->as.loop_statement.first_invariant;
        size_t traversed = 0;
        while (invariant != SOL_AST_NONE) {
            if (invariant >= lowerer->syntax->loop_invariant_count
                || traversed++ >= lowerer->syntax->loop_invariant_count) {
                lowerer->malformed = true;
                break;
            }
            const SolLoopInvariant *entry
                = &lowerer->syntax->loop_invariants[invariant];
            SolSpan span = lowerer->syntax->expressions[entry->expression].span;
            lowerer->depth = 0;
            lowerer->depth_reported = false;
            sol_contract_expression(lowerer, entry->expression, false);
            sol_contract_append_loop_obligation(lowerer,
                SOL_LOOP_OBLIGATION_INVARIANT_ENTRY,
                statement, fact, entry->expression, span);
            sol_contract_append_loop_obligation(lowerer,
                SOL_LOOP_OBLIGATION_INVARIANT_PRESERVATION,
                statement, fact, entry->expression, span);
            invariant = entry->next;
        }
        SolExprId decreases = loop->as.loop_statement.decreases;
        if (decreases != SOL_AST_NONE) {
            SolSpan span = lowerer->syntax->expressions[decreases].span;
            lowerer->depth = 0;
            lowerer->depth_reported = false;
            sol_contract_expression(lowerer, decreases, false);
            sol_contract_append_loop_obligation(lowerer,
                SOL_LOOP_OBLIGATION_DECREASES_NONNEGATIVE,
                statement, fact, decreases, span);
            sol_contract_append_loop_obligation(lowerer,
                SOL_LOOP_OBLIGATION_DECREASES_STRICT,
                statement, fact, decreases, span);
        }
        if (lowerer->malformed || lowerer->allocation_failed) break;
    }
    if (!lowerer->allocation_failed && lowerer->contracts->loop_obligation_count > 1) {
        qsort(lowerer->contracts->loop_obligations,
            lowerer->contracts->loop_obligation_count,
            sizeof(*lowerer->contracts->loop_obligations),
            sol_contract_loop_obligation_compare);
        for (size_t index = 0;
            index < lowerer->contracts->loop_obligation_count;
            ++index) lowerer->contracts->loop_obligations[index].id = index;
    }
    lowerer->in_loop_specification = false;
}

static bool sol_contract_append_unreachable_obligation(
    SolContractLowerer *lowerer,
    SolStatementId statement,
    const SolUnreachableFact *fact
) {
    SolContractTable *table = lowerer->contracts;
    if (table->unreachable_obligation_count
        == table->unreachable_obligation_capacity) {
        size_t capacity = table->unreachable_obligation_capacity == 0
            ? 8 : table->unreachable_obligation_capacity * 2;
        if (capacity < table->unreachable_obligation_capacity
            || capacity > SIZE_MAX / sizeof(*table->unreachable_obligations)) {
            lowerer->allocation_failed = true;
            return false;
        }
        if (!sol_resource_charge_arena(
            capacity - table->unreachable_obligation_capacity)) {
            lowerer->allocation_failed = true;
            return false;
        }
        SolUnreachableObligation *grown = realloc(
            table->unreachable_obligations,
            capacity * sizeof(*table->unreachable_obligations));
        if (grown == NULL) {
            lowerer->allocation_failed = true;
            return false;
        }
        table->unreachable_obligations = grown;
        table->unreachable_obligation_capacity = capacity;
    }
    const SolStatement *entry = &lowerer->syntax->statements[statement];
    SolExprId proof = entry->as.unreachable_statement.proof;
    size_t id = table->unreachable_obligation_count++;
    table->unreachable_obligations[id] = (SolUnreachableObligation){
        .id = id,
        .statement = statement,
        .owner = fact->owner,
        .owner_member = fact->owner_member,
        .owner_trait_method = fact->owner_trait_method,
        .proof = proof,
        .proof_type = lowerer->types->expressions[proof],
        .span = lowerer->syntax->expressions[proof].span,
    };
    return true;
}

static int sol_contract_unreachable_obligation_compare(
    const void *left,
    const void *right
) {
    const SolUnreachableObligation *a = left;
    const SolUnreachableObligation *b = right;
    if (a->span.start != b->span.start) return a->span.start < b->span.start ? -1 : 1;
    if (a->span.end != b->span.end) return a->span.end < b->span.end ? -1 : 1;
    if (a->statement != b->statement) return a->statement < b->statement ? -1 : 1;
    return 0;
}

static void sol_contract_lower_unreachable(SolContractLowerer *lowerer) {
    lowerer->in_loop_specification = true;
    lowerer->obligation = NULL;
    for (SolStatementId statement = 0;
        statement < lowerer->syntax->statement_count; ++statement) {
        const SolStatement *entry = &lowerer->syntax->statements[statement];
        if (entry->kind != SOL_STATEMENT_UNREACHABLE) continue;
        bool in_predicate = false;
        for (size_t condition = 0; !in_predicate
            && condition < lowerer->syntax->contract_condition_count; ++condition) {
            SolExprId expression
                = lowerer->syntax->contract_conditions[condition].expression;
            in_predicate = expression < lowerer->syntax->expression_count
                && lowerer->syntax->expressions[expression].span.start
                    <= entry->span.start
                && lowerer->syntax->expressions[expression].span.end
                    >= entry->span.end;
        }
        if (in_predicate) continue;
        lowerer->depth = 0;
        lowerer->depth_reported = false;
        sol_contract_expression(
            lowerer, entry->as.unreachable_statement.proof, false);
        sol_contract_append_unreachable_obligation(
            lowerer, statement, &lowerer->types->unreachable_facts[statement]);
        if (lowerer->malformed || lowerer->allocation_failed) break;
    }
    if (!lowerer->allocation_failed
        && lowerer->contracts->unreachable_obligation_count > 1) {
        qsort(lowerer->contracts->unreachable_obligations,
            lowerer->contracts->unreachable_obligation_count,
            sizeof(*lowerer->contracts->unreachable_obligations),
            sol_contract_unreachable_obligation_compare);
        for (size_t index = 0;
            index < lowerer->contracts->unreachable_obligation_count; ++index) {
            lowerer->contracts->unreachable_obligations[index].id = index;
        }
    }
    lowerer->in_loop_specification = false;
}

bool sol_contract_lower(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolHirModule *hir,
    const SolTypeTable *types,
    const SolEffectTable *effects,
    SolContractTable *contracts,
    SolDiagnostics *diagnostics
) {
    if (diagnostics == NULL) return false;
    if (source == NULL || source->text == NULL || syntax == NULL || hir == NULL
        || types == NULL || effects == NULL || contracts == NULL) {
        sol_diagnostics_add(
            diagnostics,
            "SOL-INTERNAL-005",
            SOL_SEVERITY_ERROR,
            (SolSpan){0},
            "null compiler input passed to contract lowering"
        );
        return false;
    }
    SolContractLowerer lowerer = {
        .source = source,
        .syntax = syntax,
        .hir = hir,
        .types = types,
        .effects = effects,
        .contracts = contracts,
        .diagnostics = diagnostics,
    };
    if (!sol_contract_validate(&lowerer)) {
        sol_contract_error(
            &lowerer,
            "SOL-INTERNAL-005",
            (SolSpan){0},
            "invalid semantic input or output table passed to contract lowering"
        );
        return false;
    }
    size_t count = syntax->contract_condition_count;
    if (count > SIZE_MAX / sizeof(*contracts->obligations)
        || syntax->expression_count > SIZE_MAX / sizeof(*contracts->expression_snapshots)) {
        return false;
    }
    if (!sol_resource_charge_arena(count)
        || !sol_resource_charge_arena(syntax->expression_count)) {
        lowerer.allocation_failed = true;
        return false;
    }
    contracts->obligations = calloc(count, sizeof(*contracts->obligations));
    contracts->expression_snapshots = malloc(
        syntax->expression_count * sizeof(*contracts->expression_snapshots)
    );
    if ((count != 0 && contracts->obligations == NULL)
        || (syntax->expression_count != 0 && contracts->expression_snapshots == NULL)) {
        lowerer.allocation_failed = true;
    }
    if (!lowerer.allocation_failed) {
        contracts->obligation_count = count;
        contracts->expression_count = syntax->expression_count;
        for (size_t index = 0; index < syntax->expression_count; ++index) {
            contracts->expression_snapshots[index] = SOL_AST_NONE;
        }
        for (size_t condition_id = 0; condition_id < count; ++condition_id) {
            const SolContractCondition *condition
                = &syntax->contract_conditions[condition_id];
            if (condition->owner_clause >= syntax->contract_clause_count) {
                lowerer.malformed = true;
                break;
            }
            const SolContractClause *clause
                = &syntax->contract_clauses[condition->owner_clause];
            SolObligation *obligation = &contracts->obligations[condition_id];
            *obligation = (SolObligation){
                .id = (SolObligationId)condition_id,
                .condition = condition_id,
                .owner_kind = clause->owner_kind,
                .owner = clause->owner,
                .kind = clause->kind,
                .outcome = condition->outcome,
                .predicate = condition->expression,
                .predicate_type = types->expressions[condition->expression],
                .result = sol_contract_result_binding(&lowerer, clause, condition),
                .first_snapshot = contracts->snapshot_count,
            };
            lowerer.obligation = obligation;
            lowerer.depth = 0;
            lowerer.depth_reported = false;
            if (obligation->predicate_type.kind != SOL_TYPE_BOOL
                && obligation->predicate_type.kind != SOL_TYPE_ERROR) {
                sol_contract_error(
                    &lowerer,
                    "SOL-CONTRACT-001",
                    syntax->expressions[obligation->predicate].span,
                    "contract predicate must have type Bool"
                );
            }
            sol_contract_expression(&lowerer, obligation->predicate, false);
            if (lowerer.malformed || lowerer.allocation_failed) break;
        }
        if (!lowerer.malformed && !lowerer.allocation_failed) {
            sol_contract_lower_loops(&lowerer);
        }
        if (!lowerer.malformed && !lowerer.allocation_failed) {
            sol_contract_lower_unreachable(&lowerer);
        }
    }
    if (lowerer.allocation_failed) diagnostics->allocation_failed = true;
    if (lowerer.malformed) {
        sol_contract_error(
            &lowerer,
            "SOL-INTERNAL-005",
            (SolSpan){0},
            "malformed semantic input encountered during contract lowering"
        );
    }
    return !lowerer.malformed && !lowerer.allocation_failed
        && !diagnostics->allocation_failed;
}
