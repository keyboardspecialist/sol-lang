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
    size_t loop_depth;
    SolDefId current_definition;
    SolDefId refinement_self_definition;
    const SolHirFileScope *scopes;
    size_t scope_count;
    SolDefId *import_targets;
    SolSpan *import_names;
    bool package_aware;
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
    free(module->expression_owners);
    free(module->type_resolutions);
    free(module->effect_resolutions);
    free(module->type_effect_resolutions);
    free(module->trait_resolutions);
    free(module->bound_resolutions);
    free(module->semantic_references);
    free(module->import_resolutions);
    free(module->file_scopes);
    free(module->item_files);
    memset(module, 0, sizeof(*module));
}

static bool sol_span_equal(const SolSource *source, SolSpan left, SolSpan right) {
    size_t left_length = left.end - left.start;
    size_t right_length = right.end - right.start;
    return left_length == right_length
        && memcmp(source->text + left.start, source->text + right.start, left_length) == 0;
}

static int sol_string_next_byte(const SolSource *source, SolSpan span, size_t *cursor) {
    if (*cursor + 1 >= span.end) return -1;
    unsigned char byte = (unsigned char)source->text[(*cursor)++];
    if (byte != '\\') return (int)byte;
    if (*cursor + 1 >= span.end) return -1;
    byte = (unsigned char)source->text[(*cursor)++];
    if (byte == 'n') return '\n';
    if (byte == 'r') return '\r';
    if (byte == 't') return '\t';
    return (int)byte;
}

static bool sol_string_span_equal(const SolSource *source, SolSpan left, SolSpan right) {
    size_t left_cursor = left.start + 1;
    size_t right_cursor = right.start + 1;
    for (;;) {
        int left_byte = sol_string_next_byte(source, left, &left_cursor);
        int right_byte = sol_string_next_byte(source, right, &right_cursor);
        if (left_byte != right_byte) return false;
        if (left_byte < 0) return true;
    }
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

static bool sol_path_join_equal(
    const SolSource *source,
    SolSpan module_name,
    SolSpan declaration,
    SolSpan path
) {
    size_t module_cursor = module_name.start;
    size_t declaration_cursor = declaration.start;
    size_t path_cursor = path.start;
    int byte;
    while ((byte = sol_path_next_byte(source, module_name, &module_cursor)) >= 0) {
        if (byte != sol_path_next_byte(source, path, &path_cursor)) return false;
    }
    if (sol_path_next_byte(source, path, &path_cursor) != '.') return false;
    while ((byte = sol_path_next_byte(source, declaration, &declaration_cursor)) >= 0) {
        if (byte != sol_path_next_byte(source, path, &path_cursor)) return false;
    }
    return sol_path_next_byte(source, path, &path_cursor) < 0;
}

static bool sol_path_module_equal(
    const SolSource *source,
    SolSpan module_name,
    SolSpan path
) {
    size_t module_cursor = module_name.start;
    size_t path_cursor = path.start;
    int byte;
    while ((byte = sol_path_next_byte(source, module_name, &module_cursor)) >= 0) {
        if (byte != sol_path_next_byte(source, path, &path_cursor)) return false;
    }
    if (sol_path_next_byte(source, path, &path_cursor) != '.') return false;
    bool has_symbol = false;
    while ((byte = sol_path_next_byte(source, path, &path_cursor)) >= 0) {
        if (byte == '.') return false;
        has_symbol = true;
    }
    return has_symbol;
}

static SolSpan sol_path_final_component(const SolSource *source, SolSpan path) {
    size_t cursor = path.start;
    size_t component = path.start;
    int byte;
    while ((byte = sol_path_next_byte(source, path, &cursor)) >= 0) {
        if (byte == '.') component = cursor;
    }
    return (SolSpan){component, path.end};
}

static void sol_semantic_hash_byte(SolSemanticId *hash, unsigned char byte) {
    hash->high ^= byte;
    hash->high *= UINT64_C(1099511628211);
    hash->low ^= byte;
    hash->low *= UINT64_C(14029467366897019727);
}

static void sol_semantic_hash_text(
    SolSemanticId *hash,
    const char *text,
    size_t length
) {
    for (size_t index = 0; index < length; ++index) {
        sol_semantic_hash_byte(hash, (unsigned char)text[index]);
    }
    sol_semantic_hash_byte(hash, 0xff);
}

static void sol_semantic_hash_path(
    SolSemanticId *hash,
    const SolSource *source,
    SolSpan path
) {
    size_t cursor = path.start;
    int byte;
    while ((byte = sol_path_next_byte(source, path, &cursor)) >= 0) {
        sol_semantic_hash_byte(hash, (unsigned char)byte);
    }
    sol_semantic_hash_byte(hash, 0xff);
}

static bool sol_semantic_id_equal(SolSemanticId left, SolSemanticId right) {
    return left.high == right.high && left.low == right.low;
}

static bool sol_stable_identity_valid(const SolSource *source, SolSpan span) {
    if (span.end < span.start + 3 || source->text[span.start] != '"'
        || source->text[span.end - 1] != '"') return false;
    for (size_t index = span.start + 1; index + 1 < span.end; ++index) {
        unsigned char byte = (unsigned char)source->text[index];
        bool valid = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z')
            || (byte >= '0' && byte <= '9') || byte == '.' || byte == '_'
            || byte == '-' || byte == ':' || byte == '/';
        if (!valid) return false;
    }
    return true;
}

static SolSemanticId sol_resolver_semantic_id(
    SolResolver *resolver,
    size_t item_index
) {
    static const char version[] = "sol.semantic-id/1";
    const SolSyntaxItem *item = &resolver->syntax->items[item_index];
    SolSemanticId hash = {
        UINT64_C(14695981039346656037),
        UINT64_C(7809847782465536322),
    };
    sol_semantic_hash_text(&hash, version, sizeof(version) - 1);
    if (item->stable_identity.start != item->stable_identity.end) {
        static const char domain[] = "stable";
        sol_semantic_hash_text(&hash, domain, sizeof(domain) - 1);
        sol_semantic_hash_text(
            &hash,
            resolver->source->text + item->stable_identity.start + 1,
            item->stable_identity.end - item->stable_identity.start - 2
        );
        return hash;
    }
    static const char domain[] = "current";
    sol_semantic_hash_text(&hash, domain, sizeof(domain) - 1);
    size_t file = resolver->module->item_files[item_index];
    sol_semantic_hash_path(&hash, resolver->source, resolver->scopes[file].module_name);
    unsigned char kind = (unsigned char)item->kind;
    sol_semantic_hash_byte(&hash, kind);
    sol_semantic_hash_byte(&hash, 0xff);
    sol_semantic_hash_path(&hash, resolver->source, item->name);
    return hash;
}

static void sol_resolver_add_semantic_reference(
    SolResolver *resolver,
    SolSemanticReferenceKind kind,
    SolSpan span,
    SolDefId target
) {
    if (target >= resolver->module->definition_count
        || resolver->module->semantic_reference_count
            >= resolver->module->semantic_reference_capacity) {
        resolver->allocation_failed = true;
        return;
    }
    resolver->module->semantic_references[
        resolver->module->semantic_reference_count++
    ] = (SolSemanticReference){
        .kind = kind,
        .span = span,
        .target = target,
        .target_id = resolver->module->definitions[target].semantic_id,
    };
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

static bool sol_resolver_validate_type_graph(
    SolResolver *resolver,
    SolTypeId type_id,
    unsigned char *states,
    size_t depth
) {
    if (type_id >= resolver->syntax->type_count || depth >= 4096) return false;
    if (states[type_id] == 1) return false;
    if (states[type_id] == 2) return true;
    states[type_id] = 1;
    const SolSyntaxType *type = &resolver->syntax->types[type_id];
    SolTypeArgumentId argument = type->first_argument;
    size_t traversed = 0;
    while (argument != SOL_AST_NONE) {
        if (argument >= resolver->syntax->type_argument_count
            || traversed++ >= resolver->syntax->type_argument_count
            || !sol_resolver_validate_type_graph(
                resolver,
                resolver->syntax->type_arguments[argument].type,
                states,
                depth + 1
            )) return false;
        argument = resolver->syntax->type_arguments[argument].next;
    }
    if (type->kind == SOL_SYNTAX_TYPE_FUNCTION
        && !sol_resolver_validate_type_graph(
            resolver,
            type->return_type,
            states,
            depth + 1
        )) return false;
    states[type_id] = 2;
    return true;
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
            && syntax->contract_conditions == NULL)) {
        sol_resolver_malformed(resolver);
        return false;
    }
    for (size_t index = 0; index < syntax->item_count; ++index) {
        const SolSyntaxItem *item = &syntax->items[index];
        if ((int)item->kind < 0 || item->kind > SOL_ITEM_TEST
            || (int)item->flavor < 0
            || (int)item->flavor < 0 || item->flavor > SOL_TYPE_DECLARATION_REFINED
            || !sol_span_valid(resolver->source, item->name)
            || !sol_span_valid(resolver->source, item->span)
            || !sol_span_valid(resolver->source, item->stable_identity)
            || (item->stable_identity.start != item->stable_identity.end
                && item->stable_identity.end - item->stable_identity.start < 2)
            || !sol_span_valid(resolver->source, item->return_type)
            || !sol_span_valid(resolver->source, item->trait_name)
            || (item->body != SOL_AST_NONE && item->body >= syntax->expression_count)
            || (item->first_parameter != SOL_AST_NONE
                && item->first_parameter >= syntax->parameter_count)
            || (item->return_type_id != SOL_AST_NONE
                && item->return_type_id >= syntax->type_count)
            || (item->representation_type != SOL_AST_NONE
                && item->representation_type >= syntax->type_count)
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
                && item->first_effect_parameter >= syntax->effect_parameter_count)) {
            sol_resolver_malformed(resolver);
            return false;
        }
        if (item->kind == SOL_ITEM_TEST
            && (item->name.end - item->name.start < 2
                || resolver->source->text[item->name.start] != '"'
                || resolver->source->text[item->name.end - 1] != '"'
                || item->is_public || item->body == SOL_AST_NONE
                || item->stable_identity.start != item->stable_identity.end
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
                || item->first_trait_method != SOL_AST_NONE)) {
            sol_resolver_malformed(resolver);
            return false;
        }
        if ((item->kind == SOL_ITEM_TYPE
                && (item->flavor == SOL_TYPE_DECLARATION_NONE
                    || item->representation_type == SOL_AST_NONE))
            || (item->kind != SOL_ITEM_TYPE
                && (item->flavor != SOL_TYPE_DECLARATION_NONE
                    || item->representation_type != SOL_AST_NONE))) {
            sol_resolver_malformed(resolver);
            return false;
        }
        bool implementation = item->kind == SOL_ITEM_IMPLEMENTATION;
        if ((implementation
                && item->trait_name.start == item->trait_name.end)
            || (!implementation
                && (item->trait_name.start != 0 || item->trait_name.end != 0))) {
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
        if ((item->kind != SOL_ITEM_TRAIT && item->kind != SOL_ITEM_IMPLEMENTATION
                && item->first_trait_method != SOL_AST_NONE)
            || (item->kind == SOL_ITEM_IMPLEMENTATION
                && item->implementation_type >= syntax->type_count)
            || (item->kind != SOL_ITEM_IMPLEMENTATION
                && item->implementation_type != SOL_AST_NONE)) {
            sol_resolver_malformed(resolver);
            return false;
        }
        SolTraitMethodId trait_method = item->first_trait_method;
        size_t trait_method_count = 0;
        while (trait_method != SOL_AST_NONE) {
            if (trait_method >= syntax->trait_method_count
                || trait_method_count++ >= syntax->trait_method_count
                || syntax->trait_methods[trait_method].owner_item != index) {
                sol_resolver_malformed(resolver);
                return false;
            }
            trait_method = syntax->trait_methods[trait_method].next;
        }
        if ((item->capability_source != SOL_AST_NONE
                && item->kind != SOL_ITEM_CAPABILITY)
            || (item->capability_source != SOL_AST_NONE
                && (syntax->parameters[item->capability_source].next != SOL_AST_NONE
                    || syntax->parameters[item->capability_source].access
                        != SOL_ACCESS_OWNED
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
            || (int)parameter->access < 0 || parameter->access > SOL_ACCESS_EXCLUSIVE
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
            || type->owner_item >= syntax->item_count
            || (type->has_effect_tail
                && (type->kind != SOL_SYNTAX_TYPE_FUNCTION
                    || !sol_span_valid(resolver->source, type->effect_tail)
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
    for (size_t index = 0; index < syntax->type_parameter_count; ++index) {
        const SolTypeParameter *parameter = &syntax->type_parameters[index];
        if (!sol_span_valid(resolver->source, parameter->name)
            || !sol_span_valid(resolver->source, parameter->bound)
            || parameter->owner_item >= syntax->item_count
            || (parameter->next != SOL_AST_NONE
                && parameter->next >= syntax->type_parameter_count)) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->effect_parameter_count; ++index) {
        const SolEffectParameter *parameter = &syntax->effect_parameters[index];
        if (!sol_span_valid(resolver->source, parameter->name)
            || parameter->owner_item >= syntax->item_count
            || syntax->items[parameter->owner_item].kind != SOL_ITEM_FUNCTION
            || syntax->items[parameter->owner_item].first_effect_parameter != index
            || parameter->next != SOL_AST_NONE) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->type_argument_count; ++index) {
        const SolTypeArgument *argument = &syntax->type_arguments[index];
        if (argument->type >= syntax->type_count
            || (int)argument->access < 0 || argument->access > SOL_ACCESS_EXCLUSIVE
            || (argument->next != SOL_AST_NONE
                && argument->next >= syntax->type_argument_count)) {
            sol_resolver_malformed(resolver);
            return false;
        }
        if (argument->access != SOL_ACCESS_OWNED) {
            bool function_parameter = false;
            for (size_t type = 0; type < syntax->type_count; ++type) {
                if (syntax->types[type].kind != SOL_SYNTAX_TYPE_FUNCTION) continue;
                SolTypeArgumentId parameter = syntax->types[type].first_argument;
                size_t traversed = 0;
                while (parameter != SOL_AST_NONE
                    && traversed++ < syntax->type_argument_count) {
                    if (parameter == index) function_parameter = true;
                    parameter = syntax->type_arguments[parameter].next;
                }
            }
            if (!function_parameter) {
                sol_resolver_malformed(resolver);
                return false;
            }
        }
    }
    for (size_t target = 0; target < syntax->type_argument_count; ++target) {
        size_t references = 0;
        for (size_t type = 0; type < syntax->type_count; ++type) {
            SolTypeArgumentId argument = syntax->types[type].first_argument;
            size_t traversed = 0;
            while (argument != SOL_AST_NONE) {
                if (argument >= syntax->type_argument_count
                    || traversed++ >= syntax->type_argument_count) {
                    sol_resolver_malformed(resolver);
                    return false;
                }
                references += argument == target ? 1 : 0;
                argument = syntax->type_arguments[argument].next;
            }
        }
        for (size_t expression = 0; expression < syntax->expression_count; ++expression) {
            if (syntax->expressions[expression].kind
                != SOL_EXPR_TYPE_APPLICATION) continue;
            SolTypeArgumentId argument = syntax->expressions[
                expression
            ].as.type_application.first_argument;
            size_t traversed = 0;
            while (argument != SOL_AST_NONE) {
                if (argument >= syntax->type_argument_count
                    || traversed++ >= syntax->type_argument_count) {
                    sol_resolver_malformed(resolver);
                    return false;
                }
                references += argument == target ? 1 : 0;
                argument = syntax->type_arguments[argument].next;
            }
        }
        if (references != 1) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    for (size_t target = 0; target < syntax->type_parameter_count; ++target) {
        size_t references = 0;
        for (size_t item = 0; item < syntax->item_count; ++item) {
            SolTypeParameterId parameter = syntax->items[item].first_type_parameter;
            size_t traversed = 0;
            while (parameter != SOL_AST_NONE) {
                if (parameter >= syntax->type_parameter_count
                    || traversed++ >= syntax->type_parameter_count
                    || syntax->type_parameters[parameter].owner_item != item) {
                    sol_resolver_malformed(resolver);
                    return false;
                }
                references += parameter == target ? 1 : 0;
                parameter = syntax->type_parameters[parameter].next;
            }
        }
        if (references != 1) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    unsigned char *type_states = calloc(syntax->type_count, 1);
    if (syntax->type_count != 0 && type_states == NULL) {
        resolver->allocation_failed = true;
        return false;
    }
    bool type_graph_valid = true;
    for (SolTypeId type = 0; type_graph_valid && type < syntax->type_count; ++type) {
        type_graph_valid = sol_resolver_validate_type_graph(
            resolver,
            type,
            type_states,
            0
        );
    }
    free(type_states);
    if (!type_graph_valid) {
        sol_resolver_malformed(resolver);
        return false;
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
            || (int)effect->owner_kind < 0 || effect->owner_kind > SOL_EFFECT_OWNER_TYPE
            || (effect->owner_kind == SOL_EFFECT_OWNER_ITEM
                && (effect->owner >= syntax->item_count
                    || syntax->items[effect->owner].kind != SOL_ITEM_FUNCTION))
            || (effect->owner_kind == SOL_EFFECT_OWNER_CAPABILITY_MEMBER
                && effect->owner >= syntax->capability_member_count)
            || (effect->owner_kind == SOL_EFFECT_OWNER_TRAIT_METHOD
                && effect->owner >= syntax->trait_method_count)
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
    for (size_t index = 0; index < syntax->contract_clause_count; ++index) {
        const SolContractClause *clause = &syntax->contract_clauses[index];
        if ((int)clause->owner_kind < 0
            || (int)clause->owner_kind < 0 || clause->owner_kind > SOL_CONTRACT_OWNER_TYPE
            || (clause->owner_kind == SOL_CONTRACT_OWNER_ITEM
                && clause->owner >= syntax->item_count)
            || (clause->owner_kind == SOL_CONTRACT_OWNER_CAPABILITY_MEMBER
                && clause->owner >= syntax->capability_member_count)
            || (clause->owner_kind == SOL_CONTRACT_OWNER_TYPE
                && (clause->owner >= syntax->item_count
                    || syntax->items[clause->owner].kind != SOL_ITEM_TYPE))) {
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
    for (size_t index = 0; index < syntax->trait_method_count; ++index) {
        const SolTraitMethod *method = &syntax->trait_methods[index];
        if (!sol_span_valid(resolver->source, method->name)
            || !sol_span_valid(resolver->source, method->span)
            || !sol_span_valid(resolver->source, method->return_type)
            || method->owner_item >= syntax->item_count
            || (syntax->items[method->owner_item].kind != SOL_ITEM_TRAIT
                && syntax->items[method->owner_item].kind != SOL_ITEM_IMPLEMENTATION)
            || method->return_type_id >= syntax->type_count
            || (method->first_parameter != SOL_AST_NONE
                && method->first_parameter >= syntax->parameter_count)
            || (method->first_effect != SOL_AST_NONE
                && method->first_effect >= syntax->effect_count)
            || (method->body != SOL_AST_NONE
                && method->body >= syntax->expression_count)
            || (method->next != SOL_AST_NONE
                && method->next >= syntax->trait_method_count)
            || (method->body != SOL_AST_NONE)
                != (syntax->items[method->owner_item].kind == SOL_ITEM_IMPLEMENTATION)) {
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
        if ((int)statement->kind < 0 || statement->kind > SOL_STATEMENT_REQUIRE) {
            sol_resolver_malformed(resolver);
            return false;
        }
        SolExprId value = SOL_AST_NONE;
        switch (statement->kind) {
            case SOL_STATEMENT_LET:
            case SOL_STATEMENT_VAR:
                value = statement->as.let_statement.value;
                break;
            case SOL_STATEMENT_ASSIGNMENT:
                value = statement->as.assignment.value;
                break;
            case SOL_STATEMENT_RETURN:
            case SOL_STATEMENT_EXPRESSION:
                value = statement->as.expression;
                break;
            case SOL_STATEMENT_REGION:
                value = statement->as.region_statement.body;
                break;
            case SOL_STATEMENT_MODIFY:
                value = statement->as.modify.body;
                break;
            case SOL_STATEMENT_LOOP:
            case SOL_STATEMENT_WHILE:
                value = statement->as.loop_statement.body;
                break;
            case SOL_STATEMENT_PANIC:
                value = statement->as.panic_statement.message;
                break;
            case SOL_STATEMENT_UNREACHABLE:
                value = statement->as.unreachable_statement.proof;
                break;
            case SOL_STATEMENT_BREAK:
            case SOL_STATEMENT_CONTINUE:
                break;
            case SOL_STATEMENT_REQUIRE:
                value = statement->as.require_statement.fallback_block;
                break;
        }
        bool binding = statement->kind == SOL_STATEMENT_LET
            || statement->kind == SOL_STATEMENT_VAR;
        bool loop_exit = statement->kind == SOL_STATEMENT_BREAK
            || statement->kind == SOL_STATEMENT_CONTINUE;
        bool uninitialized = binding && value == SOL_AST_NONE;
        if (!sol_span_valid(resolver->source, statement->span)
            || (!loop_exit && !uninitialized && value >= syntax->expression_count)
            || (statement->next != SOL_AST_NONE && statement->next >= syntax->statement_count)
            || (loop_exit && statement->next != SOL_AST_NONE)
            || (loop_exit && statement->as.expression != SOL_AST_NONE)
            || (binding
                && !sol_span_valid(resolver->source, statement->as.let_statement.name))
            || (binding && (uninitialized
                    ? statement->as.let_statement.type_id >= syntax->type_count
                    : statement->as.let_statement.type_id != SOL_AST_NONE))
            || (uninitialized && statement->kind != SOL_STATEMENT_VAR)
            || (statement->kind == SOL_STATEMENT_ASSIGNMENT
                && (statement->as.assignment.target >= syntax->expression_count
                    || (statement->as.assignment.operator_kind != SOL_TOKEN_EQUAL
                        && statement->as.assignment.operator_kind
                            != SOL_TOKEN_PLUS_EQUAL
                        && statement->as.assignment.operator_kind
                            != SOL_TOKEN_MINUS_EQUAL
                        && statement->as.assignment.operator_kind
                            != SOL_TOKEN_STAR_EQUAL
                        && statement->as.assignment.operator_kind
                            != SOL_TOKEN_SLASH_EQUAL
                        && statement->as.assignment.operator_kind
                            != SOL_TOKEN_PERCENT_EQUAL)))
            || (statement->kind == SOL_STATEMENT_REGION
                && (!sol_span_valid(resolver->source,
                        statement->as.region_statement.label)
                    || statement->as.region_statement.label.start
                        == statement->as.region_statement.label.end
                    || syntax->expressions[value].kind != SOL_EXPR_BLOCK))
            || (statement->kind == SOL_STATEMENT_MODIFY
                && (statement->as.modify.target >= syntax->expression_count
                    || syntax->expressions[value].kind != SOL_EXPR_BLOCK))
            || (statement->kind == SOL_STATEMENT_UNREACHABLE
                && (!sol_span_valid(resolver->source,
                        statement->as.unreachable_statement.because_span)
                    || statement->as.unreachable_statement.because_span.start
                        == statement->as.unreachable_statement.because_span.end
                    || statement->as.unreachable_statement.because_span.start
                        < statement->span.start
                    || statement->as.unreachable_statement.because_span.end
                        > statement->span.end
                    || syntax->expressions[value].span.start
                        < statement->as.unreachable_statement.because_span.start
                    || syntax->expressions[value].span.end
                        > statement->as.unreachable_statement.because_span.end))
            || (statement->kind == SOL_STATEMENT_REQUIRE
                && (statement->as.require_statement.condition
                        >= syntax->expression_count
                    || statement->as.require_statement.fallback_block
                        >= syntax->expression_count
                    || syntax->expressions[
                        statement->as.require_statement.fallback_block].kind
                        != SOL_EXPR_BLOCK))) {
            sol_resolver_malformed(resolver);
            return false;
        }
        if ((statement->kind == SOL_STATEMENT_LOOP
                || statement->kind == SOL_STATEMENT_WHILE)
            && (value >= syntax->expression_count
                || syntax->expressions[value].kind != SOL_EXPR_BLOCK
                || (statement->kind == SOL_STATEMENT_LOOP
                    && statement->as.loop_statement.condition != SOL_AST_NONE)
                || (statement->kind == SOL_STATEMENT_WHILE
                    && statement->as.loop_statement.condition
                        >= syntax->expression_count))) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->loop_invariant_count; ++index) {
        const SolLoopInvariant *invariant = &syntax->loop_invariants[index];
        if (invariant->expression >= syntax->expression_count
            || !sol_span_valid(resolver->source, invariant->span)
            || (invariant->next != SOL_AST_NONE
                && invariant->next >= syntax->loop_invariant_count)) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->expression_count; ++index) {
        const SolExpr *expression = &syntax->expressions[index];
        bool valid = (int)expression->kind >= 0
            && expression->kind <= SOL_EXPR_TYPE_APPLICATION
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
            case SOL_EXPR_TYPE_APPLICATION:
                valid = valid
                    && expression->as.type_application.base < syntax->expression_count
                    && expression->as.type_application.first_argument
                        < syntax->type_argument_count;
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

static bool sol_resolver_validate_scopes(SolResolver *resolver) {
    const SolSyntaxTree *syntax = resolver->syntax;
    if (resolver->scope_count == 0 || resolver->scopes == NULL) {
        sol_resolver_malformed(resolver);
        return false;
    }
    size_t next_item = 0;
    size_t next_import = 0;
    for (size_t index = 0; index < resolver->scope_count; ++index) {
        const SolHirFileScope *scope = &resolver->scopes[index];
        if (!sol_span_valid(resolver->source, scope->module_name)
            || scope->module_name.start == scope->module_name.end
            || scope->item_start != next_item
            || scope->item_start > syntax->item_count
            || scope->item_count > syntax->item_count - scope->item_start
            || scope->import_start != next_import
            || scope->import_start > syntax->import_count
            || scope->import_count > syntax->import_count - scope->import_start) {
            sol_resolver_malformed(resolver);
            return false;
        }
        next_item += scope->item_count;
        next_import += scope->import_count;
    }
    size_t expected_imports = resolver->package_aware ? syntax->import_count : 0;
    if (next_item != syntax->item_count || next_import != expected_imports) {
        sol_resolver_malformed(resolver);
        return false;
    }
    if (resolver->package_aware && (syntax->import_count > syntax->import_capacity
        || (syntax->import_count != 0 && syntax->imports == NULL))) {
        sol_resolver_malformed(resolver);
        return false;
    }
    for (size_t index = 0;
        resolver->package_aware && index < syntax->import_count;
        ++index) {
        if (!sol_span_valid(resolver->source, syntax->imports[index].path)
            || syntax->imports[index].path.start == syntax->imports[index].path.end) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    return true;
}

static bool sol_resolver_allocate(SolResolver *resolver) {
    const SolSyntaxTree *syntax = resolver->syntax;
    if (syntax->item_count > SIZE_MAX / sizeof(*resolver->module->definitions)
        || syntax->expression_count > SIZE_MAX / sizeof(*resolver->module->resolutions)
        || syntax->type_count > SIZE_MAX / sizeof(*resolver->module->type_resolutions)) {
        return false;
    }
    size_t reference_capacity = syntax->item_count;
    if (syntax->import_count > SIZE_MAX - reference_capacity) return false;
    reference_capacity += syntax->import_count;
    if (syntax->expression_count > SIZE_MAX - reference_capacity) return false;
    reference_capacity += syntax->expression_count;
    if (syntax->type_count > SIZE_MAX - reference_capacity) return false;
    reference_capacity += syntax->type_count;
    if (syntax->item_count > SIZE_MAX - reference_capacity) return false;
    reference_capacity += syntax->item_count;
    if (syntax->type_parameter_count > SIZE_MAX - reference_capacity) return false;
    reference_capacity += syntax->type_parameter_count;
    if (reference_capacity > SIZE_MAX / sizeof(*resolver->module->semantic_references)) {
        return false;
    }

    resolver->module->definition_count = syntax->item_count;
    resolver->module->resolution_count = syntax->expression_count;
    resolver->module->type_resolution_count = syntax->type_count;
    resolver->module->effect_resolution_count = syntax->effect_count;
    resolver->module->type_effect_resolution_count = syntax->type_count;
    resolver->module->trait_resolution_count = syntax->item_count;
    resolver->module->bound_resolution_count = syntax->type_parameter_count;
    resolver->module->semantic_reference_capacity = reference_capacity;
    resolver->module->import_resolution_count
        = resolver->package_aware ? syntax->import_count : 0;
    resolver->module->file_scope_count = resolver->scope_count;
    resolver->module->definitions = calloc(
        syntax->item_count,
        sizeof(*resolver->module->definitions)
    );
    resolver->module->resolutions = calloc(
        syntax->expression_count,
        sizeof(*resolver->module->resolutions)
    );
    resolver->module->expression_owners = malloc(
        syntax->expression_count * sizeof(*resolver->module->expression_owners)
    );
    resolver->module->type_resolutions = calloc(
        syntax->type_count,
        sizeof(*resolver->module->type_resolutions)
    );
    resolver->module->effect_resolutions = calloc(
        syntax->effect_count,
        sizeof(*resolver->module->effect_resolutions)
    );
    resolver->module->type_effect_resolutions = malloc(
        syntax->type_count * sizeof(*resolver->module->type_effect_resolutions)
    );
    resolver->module->trait_resolutions = calloc(
        syntax->item_count, sizeof(*resolver->module->trait_resolutions)
    );
    resolver->module->bound_resolutions = calloc(
        syntax->type_parameter_count, sizeof(*resolver->module->bound_resolutions)
    );
    resolver->module->semantic_references = malloc(
        reference_capacity * sizeof(*resolver->module->semantic_references)
    );
    resolver->module->import_resolutions = calloc(
        resolver->module->import_resolution_count,
        sizeof(*resolver->module->import_resolutions)
    );
    resolver->module->file_scopes = malloc(
        resolver->scope_count * sizeof(*resolver->module->file_scopes)
    );
    resolver->module->item_files = malloc(
        syntax->item_count * sizeof(*resolver->module->item_files)
    );
    size_t import_count = resolver->package_aware ? syntax->import_count : 0;
    resolver->import_targets = malloc(import_count * sizeof(*resolver->import_targets));
    resolver->import_names = malloc(import_count * sizeof(*resolver->import_names));

    if ((syntax->item_count != 0 && resolver->module->definitions == NULL)
        || (syntax->expression_count != 0 && resolver->module->resolutions == NULL)
        || (syntax->expression_count != 0 && resolver->module->expression_owners == NULL)
        || (syntax->type_count != 0 && resolver->module->type_resolutions == NULL)
        || (syntax->effect_count != 0 && resolver->module->effect_resolutions == NULL)
        || (syntax->type_count != 0
            && resolver->module->type_effect_resolutions == NULL)
        || (syntax->item_count != 0 && resolver->module->trait_resolutions == NULL)
        || (syntax->type_parameter_count != 0
            && resolver->module->bound_resolutions == NULL)
        || (reference_capacity != 0 && resolver->module->semantic_references == NULL)
        || (resolver->module->import_resolution_count != 0
            && resolver->module->import_resolutions == NULL)
        || resolver->module->file_scopes == NULL
        || (syntax->item_count != 0 && resolver->module->item_files == NULL)
        || (import_count != 0
            && (resolver->import_targets == NULL || resolver->import_names == NULL))) {
        return false;
    }
    memcpy(
        resolver->module->file_scopes,
        resolver->scopes,
        resolver->scope_count * sizeof(*resolver->scopes)
    );
    for (size_t file = 0; file < resolver->scope_count; ++file) {
        const SolHirFileScope *scope = &resolver->scopes[file];
        for (size_t item = scope->item_start;
            item < scope->item_start + scope->item_count;
            ++item) {
            resolver->module->item_files[item] = file;
        }
    }
    for (size_t index = 0; index < import_count; ++index) {
        resolver->import_targets[index] = SOL_AST_NONE;
        resolver->import_names[index] = sol_path_final_component(
            resolver->source, syntax->imports[index].path
        );
    }
    for (size_t index = 0; index < syntax->expression_count; ++index) {
        resolver->module->expression_owners[index] = SOL_AST_NONE;
    }
    for (size_t index = 0; index < syntax->item_count; ++index) {
        resolver->module->trait_resolutions[index] = (SolResolution){
            .kind = SOL_RESOLUTION_NOT_APPLICABLE,
            .target = SOL_AST_NONE,
        };
    }
    for (size_t index = 0; index < syntax->type_parameter_count; ++index) {
        resolver->module->bound_resolutions[index] = (SolResolution){
            .kind = SOL_RESOLUTION_NOT_APPLICABLE,
            .target = SOL_AST_NONE,
        };
    }
    for (size_t index = 0; index < resolver->module->import_resolution_count; ++index) {
        resolver->module->import_resolutions[index] = (SolResolution){
            .kind = SOL_RESOLUTION_ERROR,
            .target = SOL_AST_NONE,
        };
    }
    for (size_t index = 0; index < syntax->effect_count; ++index) {
        resolver->module->effect_resolutions[index] = (SolEffectResolution){
            SOL_EFFECT_RESOLUTION_ATOM, SOL_AST_NONE
        };
    }
    for (size_t index = 0; index < syntax->type_count; ++index) {
        resolver->module->type_effect_resolutions[index] = (SolEffectResolution){
            SOL_EFFECT_RESOLUTION_ERROR, SOL_AST_NONE
        };
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

static size_t sol_resolver_owner_file(const SolResolver *resolver, SolDefId owner) {
    return owner < resolver->syntax->item_count
        ? resolver->module->item_files[owner]
        : SOL_AST_NONE;
}

static bool sol_resolver_same_module(
    const SolResolver *resolver,
    size_t left_file,
    size_t right_file
) {
    return left_file < resolver->scope_count && right_file < resolver->scope_count
        && sol_path_span_equal(
            resolver->source,
            resolver->scopes[left_file].module_name,
            resolver->scopes[right_file].module_name
        );
}

static SolResolution sol_resolver_visible_definition(
    SolResolver *resolver,
    SolDefId owner,
    SolSpan name
) {
    size_t file = sol_resolver_owner_file(resolver, owner);
    if (file == SOL_AST_NONE) return (SolResolution){SOL_RESOLUTION_ERROR, SOL_AST_NONE};
    for (size_t definition = 0;
        definition < resolver->module->definition_count;
        ++definition) {
        if (resolver->syntax->items[definition].kind == SOL_ITEM_TEST) continue;
        if (sol_resolver_same_module(
                resolver, file, resolver->module->item_files[definition]
            ) && sol_path_span_equal(
                resolver->source,
                resolver->module->definitions[definition].name,
                name
            )) {
            return (SolResolution){SOL_RESOLUTION_DEFINITION, definition};
        }
    }
    const SolHirFileScope *scope = &resolver->scopes[file];
    for (size_t offset = 0; offset < scope->import_count; ++offset) {
        size_t import = scope->import_start + offset;
        if (resolver->import_targets[import] != SOL_AST_NONE
            && sol_path_span_equal(
                resolver->source, resolver->import_names[import], name
            )) {
            return (SolResolution){
                SOL_RESOLUTION_DEFINITION, resolver->import_targets[import]
            };
        }
    }
    return (SolResolution){SOL_RESOLUTION_ERROR, SOL_AST_NONE};
}

static SolResolution sol_resolver_lookup(SolResolver *resolver, SolSpan name) {
    for (size_t index = resolver->binding_count; index > 0; --index) {
        SolBinding *binding = &resolver->bindings[index - 1];
        if (sol_span_equal(resolver->source, binding->name, name)) {
            return binding->resolution;
        }
    }
    SolResolution definition = sol_resolver_visible_definition(
        resolver, resolver->current_definition, name
    );
    if (definition.kind == SOL_RESOLUTION_DEFINITION) return definition;
    return sol_resolver_builtin(resolver->source, name);
}

static SolResolution sol_resolver_lookup_definition(SolResolver *resolver, SolSpan name) {
    return sol_resolver_visible_definition(
        resolver, resolver->current_definition, name
    );
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
            .access = kind == SOL_LOCAL_PARAMETER
                ? resolver->syntax->parameters[syntax_id].access : SOL_ACCESS_OWNED,
            .mutable = kind == SOL_LOCAL_BINDING
                && syntax_id < resolver->syntax->statement_count
                && resolver->syntax->statements[syntax_id].kind == SOL_STATEMENT_VAR,
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
        if (statement->kind == SOL_STATEMENT_LOOP
            || statement->kind == SOL_STATEMENT_WHILE) {
            ++resolver->loop_depth;
            if (statement->kind == SOL_STATEMENT_WHILE) {
                sol_resolver_expression(
                    resolver, statement->as.loop_statement.condition
                );
            }
            SolLoopInvariantId invariant
                = statement->as.loop_statement.first_invariant;
            size_t invariant_count = 0;
            while (invariant != SOL_AST_NONE) {
                if (invariant >= resolver->syntax->loop_invariant_count
                    || invariant_count++ >= resolver->syntax->loop_invariant_count) {
                    sol_resolver_malformed(resolver);
                    break;
                }
                const SolLoopInvariant *entry
                    = &resolver->syntax->loop_invariants[invariant];
                sol_resolver_expression(resolver, entry->expression);
                invariant = entry->next;
            }
            if (statement->as.loop_statement.decreases != SOL_AST_NONE) {
                sol_resolver_expression(
                    resolver, statement->as.loop_statement.decreases);
            }
            sol_resolver_expression(resolver, statement->as.loop_statement.body);
            --resolver->loop_depth;
        } else if (statement->kind == SOL_STATEMENT_BREAK
            || statement->kind == SOL_STATEMENT_CONTINUE) {
            if (resolver->loop_depth == 0) {
                sol_diagnostics_add(
                    resolver->diagnostics,
                    "SOL-RESOLVE-012",
                    SOL_SEVERITY_ERROR,
                    statement->span,
                    statement->kind == SOL_STATEMENT_BREAK
                        ? "break is only valid inside a loop"
                        : "continue is only valid inside a loop"
                );
            }
        } else if (statement->kind == SOL_STATEMENT_LET
            || statement->kind == SOL_STATEMENT_VAR) {
            if (statement->as.let_statement.value != SOL_AST_NONE) {
                sol_resolver_expression(resolver, statement->as.let_statement.value);
            }
            sol_resolver_add_binding(
                resolver,
                statement->as.let_statement.name,
                SOL_LOCAL_BINDING,
                statement_id
            );
        } else if (statement->kind == SOL_STATEMENT_ASSIGNMENT) {
            sol_resolver_expression(resolver, statement->as.assignment.target);
            sol_resolver_expression(resolver, statement->as.assignment.value);
        } else if (statement->kind == SOL_STATEMENT_REGION) {
            sol_resolver_expression(resolver, statement->as.region_statement.body);
        } else if (statement->kind == SOL_STATEMENT_MODIFY) {
            sol_resolver_expression(resolver, statement->as.modify.target);
            sol_resolver_expression(resolver, statement->as.modify.body);
        } else if (statement->kind == SOL_STATEMENT_PANIC) {
            sol_resolver_expression(resolver, statement->as.panic_statement.message);
        } else if (statement->kind == SOL_STATEMENT_UNREACHABLE) {
            sol_resolver_expression(resolver, statement->as.unreachable_statement.proof);
        } else if (statement->kind == SOL_STATEMENT_REQUIRE) {
            sol_resolver_expression(resolver, statement->as.require_statement.condition);
            sol_resolver_expression(resolver,
                statement->as.require_statement.fallback_block);
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
    if (resolver->module->expression_owners[expression_id] == SOL_AST_NONE) {
        resolver->module->expression_owners[expression_id] = resolver->current_definition;
    } else if (resolver->module->expression_owners[expression_id]
        != resolver->current_definition) {
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
            SolResolution resolution;
            if (resolver->refinement_self_definition != SOL_AST_NONE
                && sol_span_text_equal(resolver->source, expression->as.name, "self")) {
                resolution = (SolResolution){
                    SOL_RESOLUTION_REFINEMENT_SELF,
                    resolver->refinement_self_definition,
                };
            } else {
                resolution = sol_resolver_lookup(resolver, expression->as.name);
            }
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
        case SOL_EXPR_TYPE_APPLICATION:
            sol_resolver_expression(resolver, expression->as.type_application.base);
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
            bool tests = item->kind == SOL_ITEM_TEST
                && resolver->syntax->items[previous].kind == SOL_ITEM_TEST;
            bool declarations = item->kind != SOL_ITEM_TEST
                && resolver->syntax->items[previous].kind != SOL_ITEM_TEST;
            if (sol_resolver_same_module(
                    resolver,
                    resolver->module->item_files[previous],
                    resolver->module->item_files[index]
                ) && ((tests && sol_string_span_equal(
                        resolver->source,
                        resolver->module->definitions[previous].name,
                        item->name
                    )) || (declarations && sol_path_span_equal(
                        resolver->source,
                        resolver->module->definitions[previous].name,
                        item->name
                    )))) {
                sol_diagnostics_add(
                    resolver->diagnostics,
                    "SOL-RESOLVE-001",
                    SOL_SEVERITY_ERROR,
                    item->name,
                    tests ? "duplicate test label in module"
                        : "duplicate declaration in module"
                );
                break;
            }
        }
        resolver->module->definitions[index] = (SolHirDefinition){
            .kind = item->kind,
            .name = item->name,
            .stable_identity = item->stable_identity,
            .semantic_id = sol_resolver_semantic_id(resolver, index),
            .syntax_item = index,
        };
        if (item->stable_identity.start != item->stable_identity.end
            && (!item->is_public || item->kind == SOL_ITEM_IMPLEMENTATION)) {
            sol_diagnostics_add(
                resolver->diagnostics,
                "SOL-IDENTITY-001",
                SOL_SEVERITY_ERROR,
                item->stable_identity,
                "@stable is only valid on named public declarations"
            );
        } else if (item->stable_identity.start != item->stable_identity.end
            && !sol_stable_identity_valid(resolver->source, item->stable_identity)) {
            sol_diagnostics_add(
                resolver->diagnostics,
                "SOL-IDENTITY-002",
                SOL_SEVERITY_ERROR,
                item->stable_identity,
                "stable identity must be a non-empty ASCII token using letters, digits, '.', '_', '-', ':', or '/'"
            );
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (sol_semantic_id_equal(
                resolver->module->definitions[previous].semantic_id,
                resolver->module->definitions[index].semantic_id
            )) {
                sol_diagnostics_add(
                    resolver->diagnostics,
                    "SOL-IDENTITY-003",
                    SOL_SEVERITY_ERROR,
                    item->stable_identity.start != item->stable_identity.end
                        ? item->stable_identity : item->name,
                    "semantic declaration identity collides within the package"
                );
                break;
            }
        }
    }
}

static void sol_resolver_resolve_imports(SolResolver *resolver) {
    for (size_t file = 0; file < resolver->scope_count; ++file) {
        const SolHirFileScope *scope = &resolver->scopes[file];
        for (size_t offset = 0; offset < scope->import_count; ++offset) {
            size_t import = scope->import_start + offset;
            SolSpan path = resolver->syntax->imports[import].path;
            size_t target = SOL_AST_NONE;
            size_t public_count = 0;
            bool module_found = false;
            bool private_found = false;
            for (size_t target_file = 0;
                target_file < resolver->scope_count;
                ++target_file) {
                if (sol_path_module_equal(
                    resolver->source, resolver->scopes[target_file].module_name, path
                )) module_found = true;
            }
            for (size_t definition = 0;
                definition < resolver->module->definition_count;
                ++definition) {
                size_t target_file = resolver->module->item_files[definition];
                if (!sol_path_join_equal(
                        resolver->source,
                        resolver->scopes[target_file].module_name,
                        resolver->module->definitions[definition].name,
                        path
                    )) continue;
                if (!resolver->syntax->items[definition].is_public) {
                    private_found = true;
                    continue;
                }
                ++public_count;
                target = definition;
            }
            const char *code = NULL;
            const char *message = NULL;
            if (!module_found) {
                code = "SOL-RESOLVE-008";
                message = "import refers to an unknown module";
            } else if (public_count == 0 && private_found) {
                code = "SOL-RESOLVE-010";
                message = "imported declaration is private";
            } else if (public_count != 1) {
                code = "SOL-RESOLVE-009";
                message = "import refers to an unknown or ambiguous symbol";
            }
            if (code != NULL) {
                sol_diagnostics_add(
                    resolver->diagnostics, code, SOL_SEVERITY_ERROR, path, "%s", message
                );
                continue;
            }
            resolver->import_targets[import] = target;
            resolver->module->import_resolutions[import] = (SolResolution){
                SOL_RESOLUTION_DEFINITION, target
            };
            SolSpan spelling = resolver->import_names[import];
            for (size_t previous = 0; previous < offset; ++previous) {
                size_t other = scope->import_start + previous;
                if (resolver->import_targets[other] != SOL_AST_NONE
                    && sol_path_span_equal(
                        resolver->source, resolver->import_names[other], spelling
                    )) {
                    sol_diagnostics_add(
                        resolver->diagnostics, "SOL-RESOLVE-011", SOL_SEVERITY_ERROR,
                        spelling, "duplicate or ambiguous imported spelling"
                    );
                    resolver->import_targets[import] = SOL_AST_NONE;
                    resolver->import_targets[other] = SOL_AST_NONE;
                    resolver->module->import_resolutions[import] = (SolResolution){
                        SOL_RESOLUTION_ERROR, SOL_AST_NONE
                    };
                    resolver->module->import_resolutions[other] = (SolResolution){
                        SOL_RESOLUTION_ERROR, SOL_AST_NONE
                    };
                }
            }
            for (size_t definition = 0;
                definition < resolver->module->definition_count;
                ++definition) {
                if (sol_resolver_same_module(
                        resolver, file, resolver->module->item_files[definition]
                    ) && sol_path_span_equal(
                        resolver->source,
                        resolver->module->definitions[definition].name,
                        spelling
                    )) {
                    sol_diagnostics_add(
                        resolver->diagnostics, "SOL-RESOLVE-012", SOL_SEVERITY_ERROR,
                        spelling, "imported spelling collides with a module declaration"
                    );
                    resolver->import_targets[import] = SOL_AST_NONE;
                    resolver->module->import_resolutions[import] = (SolResolution){
                        SOL_RESOLUTION_ERROR, SOL_AST_NONE
                    };
                    break;
                }
            }
        }
    }
}

static void sol_resolver_collect_semantic_references(SolResolver *resolver) {
    for (SolDefId definition = 0;
        definition < resolver->module->definition_count;
        ++definition) {
        sol_resolver_add_semantic_reference(
            resolver,
            SOL_SEMANTIC_REFERENCE_DECLARATION,
            resolver->module->definitions[definition].name,
            definition
        );
    }
    for (size_t import = 0;
        import < resolver->module->import_resolution_count;
        ++import) {
        SolResolution resolution = resolver->module->import_resolutions[import];
        if (resolution.kind == SOL_RESOLUTION_DEFINITION) {
            sol_resolver_add_semantic_reference(
                resolver,
                SOL_SEMANTIC_REFERENCE_IMPORT,
                resolver->import_names[import],
                resolution.target
            );
        }
    }
    for (SolExprId expression = 0;
        expression < resolver->module->resolution_count;
        ++expression) {
        SolResolution resolution = resolver->module->resolutions[expression];
        if (resolution.kind == SOL_RESOLUTION_DEFINITION) {
            sol_resolver_add_semantic_reference(
                resolver,
                SOL_SEMANTIC_REFERENCE_EXPRESSION,
                resolver->syntax->expressions[expression].span,
                resolution.target
            );
        }
    }
    for (SolTypeId type = 0; type < resolver->module->type_resolution_count; ++type) {
        SolTypeResolution resolution = resolver->module->type_resolutions[type];
        if (resolution.kind == SOL_TYPE_RESOLUTION_DEFINITION) {
            sol_resolver_add_semantic_reference(
                resolver,
                SOL_SEMANTIC_REFERENCE_TYPE,
                resolver->syntax->types[type].name,
                resolution.target
            );
        }
    }
    for (size_t item = 0; item < resolver->module->trait_resolution_count; ++item) {
        SolResolution resolution = resolver->module->trait_resolutions[item];
        if (resolution.kind == SOL_RESOLUTION_DEFINITION) {
            sol_resolver_add_semantic_reference(
                resolver,
                SOL_SEMANTIC_REFERENCE_TRAIT,
                resolver->syntax->items[item].trait_name,
                resolution.target
            );
        }
    }
    for (size_t parameter = 0;
        parameter < resolver->module->bound_resolution_count;
        ++parameter) {
        SolResolution resolution = resolver->module->bound_resolutions[parameter];
        if (resolution.kind == SOL_RESOLUTION_DEFINITION) {
            sol_resolver_add_semantic_reference(
                resolver,
                SOL_SEMANTIC_REFERENCE_BOUND,
                resolver->syntax->type_parameters[parameter].bound,
                resolution.target
            );
        }
    }
}

static SolTypeResolution sol_resolver_builtin_type(
    const SolSource *source,
    SolSpan name
) {
    if (sol_span_text_equal(source, name, "Int64")) {
        return (SolTypeResolution){SOL_TYPE_RESOLUTION_BUILTIN, SOL_TYPE_BUILTIN_INT64};
    }
    if (sol_span_text_equal(source, name, "Bool")) {
        return (SolTypeResolution){SOL_TYPE_RESOLUTION_BUILTIN, SOL_TYPE_BUILTIN_BOOL};
    }
    if (sol_span_text_equal(source, name, "Text")) {
        return (SolTypeResolution){SOL_TYPE_RESOLUTION_BUILTIN, SOL_TYPE_BUILTIN_TEXT};
    }
    if (sol_span_text_equal(source, name, "Option")) {
        return (SolTypeResolution){SOL_TYPE_RESOLUTION_BUILTIN, SOL_TYPE_BUILTIN_OPTION};
    }
    if (sol_span_text_equal(source, name, "Result")) {
        return (SolTypeResolution){SOL_TYPE_RESOLUTION_BUILTIN, SOL_TYPE_BUILTIN_RESULT};
    }
    return (SolTypeResolution){SOL_TYPE_RESOLUTION_ERROR, SOL_AST_NONE};
}

static void sol_resolver_resolve_types(SolResolver *resolver) {
    for (size_t owner = 0; owner < resolver->syntax->item_count; ++owner) {
        SolTypeParameterId parameter = resolver->syntax->items[owner].first_type_parameter;
        size_t traversed = 0;
        while (parameter != SOL_AST_NONE) {
            if (parameter >= resolver->syntax->type_parameter_count
                || traversed++ >= resolver->syntax->type_parameter_count
                || resolver->syntax->type_parameters[parameter].owner_item != owner) {
                sol_resolver_malformed(resolver);
                return;
            }
            SolTypeParameterId previous = resolver->syntax->items[owner].first_type_parameter;
            while (previous != parameter) {
                if (sol_span_equal(
                    resolver->source,
                    resolver->syntax->type_parameters[previous].name,
                    resolver->syntax->type_parameters[parameter].name
                )) {
                    sol_diagnostics_add(
                        resolver->diagnostics,
                        "SOL-RESOLVE-005",
                        SOL_SEVERITY_ERROR,
                        resolver->syntax->type_parameters[parameter].name,
                        "duplicate generic parameter name"
                    );
                    break;
                }
                previous = resolver->syntax->type_parameters[previous].next;
            }
            parameter = resolver->syntax->type_parameters[parameter].next;
        }
        SolEffectParameterId effect_parameter
            = resolver->syntax->items[owner].first_effect_parameter;
        if (effect_parameter != SOL_AST_NONE) {
            const SolEffectParameter *effect
                = &resolver->syntax->effect_parameters[effect_parameter];
            parameter = resolver->syntax->items[owner].first_type_parameter;
            while (parameter != SOL_AST_NONE) {
                if (sol_span_equal(
                    resolver->source,
                    resolver->syntax->type_parameters[parameter].name,
                    effect->name
                )) {
                    sol_diagnostics_add(
                        resolver->diagnostics,
                        "SOL-RESOLVE-005",
                        SOL_SEVERITY_ERROR,
                        effect->name,
                        "duplicate type/effect generic parameter name"
                    );
                    break;
                }
                parameter = resolver->syntax->type_parameters[parameter].next;
            }
        }
    }
    for (size_t item = 0; item < resolver->syntax->item_count; ++item) {
        const SolSyntaxItem *entry = &resolver->syntax->items[item];
        if (entry->kind == SOL_ITEM_IMPLEMENTATION) {
            SolResolution resolution = sol_resolver_visible_definition(
                resolver, item, entry->trait_name
            );
            if (resolution.kind == SOL_RESOLUTION_DEFINITION
                && resolver->module->definitions[resolution.target].kind
                    != SOL_ITEM_TRAIT) {
                resolution = (SolResolution){SOL_RESOLUTION_ERROR, SOL_AST_NONE};
            }
            resolver->module->trait_resolutions[item] = resolution;
            if (resolution.kind == SOL_RESOLUTION_ERROR) {
                sol_diagnostics_add(
                    resolver->diagnostics, "SOL-RESOLVE-007", SOL_SEVERITY_ERROR,
                    entry->trait_name, "unresolved trait in implementation"
                );
            }
        }
    }
    for (size_t parameter = 0;
        parameter < resolver->syntax->type_parameter_count;
        ++parameter) {
        SolSpan bound = resolver->syntax->type_parameters[parameter].bound;
        if (bound.start == bound.end) continue;
        SolDefId owner = resolver->syntax->type_parameters[parameter].owner_item;
        SolResolution resolution = sol_resolver_visible_definition(
            resolver, owner, bound
        );
        if (resolution.kind == SOL_RESOLUTION_DEFINITION
            && resolver->module->definitions[resolution.target].kind != SOL_ITEM_TRAIT) {
            resolution = (SolResolution){SOL_RESOLUTION_ERROR, SOL_AST_NONE};
        }
        resolver->module->bound_resolutions[parameter] = resolution;
        if (resolution.kind == SOL_RESOLUTION_ERROR) {
            sol_diagnostics_add(
                resolver->diagnostics, "SOL-RESOLVE-007", SOL_SEVERITY_ERROR,
                bound, "unresolved trait bound"
            );
        }
    }
    for (size_t type_id = 0; type_id < resolver->syntax->type_count; ++type_id) {
        const SolSyntaxType *type = &resolver->syntax->types[type_id];
        if (type->kind != SOL_SYNTAX_TYPE_PATH) continue;
        if ((resolver->syntax->items[type->owner_item].kind == SOL_ITEM_TRAIT
                || resolver->syntax->items[type->owner_item].kind
                    == SOL_ITEM_IMPLEMENTATION)
            && sol_span_text_equal(resolver->source, type->name, "Self")) {
            resolver->module->type_resolutions[type_id] = (SolTypeResolution){
                SOL_TYPE_RESOLUTION_SELF, type->owner_item
            };
            continue;
        }
        SolTypeParameterId parameter
            = resolver->syntax->items[type->owner_item].first_type_parameter;
        size_t traversed = 0;
        while (parameter != SOL_AST_NONE) {
            if (traversed++ >= resolver->syntax->type_parameter_count) {
                sol_resolver_malformed(resolver);
                return;
            }
            if (sol_span_equal(
                resolver->source,
                resolver->syntax->type_parameters[parameter].name,
                type->name
            )) {
                resolver->module->type_resolutions[type_id] = (SolTypeResolution){
                    SOL_TYPE_RESOLUTION_PARAMETER,
                    parameter,
                };
                break;
            }
            parameter = resolver->syntax->type_parameters[parameter].next;
        }
        if (parameter != SOL_AST_NONE) continue;
        SolTypeResolution resolution = sol_resolver_builtin_type(
            resolver->source, type->name
        );
        if (resolution.kind == SOL_TYPE_RESOLUTION_ERROR) {
            SolResolution definition = sol_resolver_visible_definition(
                resolver, type->owner_item, type->name
            );
            if (definition.kind == SOL_RESOLUTION_DEFINITION) {
                SolItemKind kind = resolver->module->definitions[definition.target].kind;
                if (kind == SOL_ITEM_RECORD || kind == SOL_ITEM_ENUM
                    || kind == SOL_ITEM_TYPE
                    || kind == SOL_ITEM_CAPABILITY) {
                    resolution = (SolTypeResolution){
                        SOL_TYPE_RESOLUTION_DEFINITION, definition.target
                    };
                }
            }
        }
        resolver->module->type_resolutions[type_id] = resolution;
    }

    for (SolEffectId effect_id = 0;
        effect_id < resolver->syntax->effect_count;
        ++effect_id) {
        const SolEffect *effect = &resolver->syntax->effects[effect_id];
        if (effect->is_pure) continue;
        SolDefId owner = SOL_AST_NONE;
        if (effect->owner_kind == SOL_EFFECT_OWNER_ITEM) owner = effect->owner;
        else if (effect->owner_kind == SOL_EFFECT_OWNER_TYPE
            && effect->owner < resolver->syntax->type_count) {
            owner = resolver->syntax->types[effect->owner].owner_item;
        }
        if (owner >= resolver->syntax->item_count) continue;
        SolEffectParameterId parameter
            = resolver->syntax->items[owner].first_effect_parameter;
        if (parameter != SOL_AST_NONE && sol_span_equal(
            resolver->source,
            resolver->syntax->effect_parameters[parameter].name,
            effect->name
        )) {
            resolver->module->effect_resolutions[effect_id]
                = (SolEffectResolution){SOL_EFFECT_RESOLUTION_PARAMETER, parameter};
        }
    }
    for (SolTypeId type_id = 0; type_id < resolver->syntax->type_count; ++type_id) {
        const SolSyntaxType *type = &resolver->syntax->types[type_id];
        if (!type->has_effect_tail) continue;
        SolEffectParameterId parameter = resolver->syntax->items[
            type->owner_item
        ].first_effect_parameter;
        if (parameter != SOL_AST_NONE && sol_span_equal(
            resolver->source,
            resolver->syntax->effect_parameters[parameter].name,
            type->effect_tail
        )) {
            resolver->module->type_effect_resolutions[type_id]
                = (SolEffectResolution){SOL_EFFECT_RESOLUTION_PARAMETER, parameter};
        } else {
            sol_diagnostics_add(
                resolver->diagnostics,
                "SOL-RESOLVE-006",
                SOL_SEVERITY_ERROR,
                type->effect_tail,
                "callback effect row tail is not declared by the owning function"
            );
        }
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

static void sol_resolver_type_declaration(
    SolResolver *resolver,
    SolDefId definition
) {
    const SolSyntaxItem *item = &resolver->syntax->items[definition];
    if (item->flavor != SOL_TYPE_DECLARATION_REFINED) return;
    resolver->current_definition = definition;
    SolContractClauseId clause_id = item->first_contract;
    while (clause_id != SOL_AST_NONE) {
        const SolContractClause *clause = &resolver->syntax->contract_clauses[clause_id];
        SolContractConditionId condition_id = clause->first_condition;
        while (condition_id != SOL_AST_NONE) {
            resolver->binding_count = 0;
            resolver->scope_depth = 0;
            resolver->refinement_self_definition = definition;
            sol_resolver_expression(
                resolver,
                resolver->syntax->contract_conditions[condition_id].expression
            );
            resolver->refinement_self_definition = SOL_AST_NONE;
            condition_id = resolver->syntax->contract_conditions[condition_id].next;
        }
        clause_id = clause->next;
    }
    resolver->binding_count = 0;
}

static void sol_resolver_capability_members(SolResolver *resolver, SolDefId definition) {
    const SolSyntaxItem *item = &resolver->syntax->items[definition];
    resolver->current_definition = definition;
    resolver->binding_count = 0;
    resolver->scope_depth = 0;
    if (item->capability_source != SOL_AST_NONE) {
        sol_resolver_add_binding(resolver,
            resolver->syntax->parameters[item->capability_source].name,
            SOL_LOCAL_PARAMETER, item->capability_source);
        resolver->binding_count = 0;
    }
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

static void sol_resolver_trait_methods(SolResolver *resolver, SolDefId definition) {
    SolTraitMethodId method_id = resolver->syntax->items[definition].first_trait_method;
    while (method_id != SOL_AST_NONE) {
        const SolTraitMethod *method = &resolver->syntax->trait_methods[method_id];
        resolver->current_definition = definition;
        resolver->binding_count = 0;
        resolver->scope_depth = 0;
        sol_resolver_bind_parameters(resolver, method->first_parameter);
        if (method->body != SOL_AST_NONE) sol_resolver_expression(resolver, method->body);
        resolver->binding_count = 0;
        method_id = method->next;
    }
}

static bool sol_hir_lower_impl(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolHirFileScope *scopes,
    size_t scope_count,
    bool package_aware,
    SolHirModule *module,
    SolDiagnostics *diagnostics
) {
    SolResolver resolver = {
        .source = source,
        .syntax = syntax,
        .module = module,
        .diagnostics = diagnostics,
        .scopes = scopes,
        .scope_count = scope_count,
        .package_aware = package_aware,
        .refinement_self_definition = SOL_AST_NONE,
    };
    if (!sol_resolver_validate(&resolver)
        || !sol_resolver_validate_scopes(&resolver)) {
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
        free(resolver.import_targets);
        free(resolver.import_names);
        return false;
    }

    sol_resolver_collect_definitions(&resolver);
    if (package_aware) sol_resolver_resolve_imports(&resolver);
    sol_resolver_resolve_types(&resolver);
    for (size_t index = 0; index < syntax->item_count; ++index) {
        if (syntax->items[index].kind == SOL_ITEM_FUNCTION
            || syntax->items[index].kind == SOL_ITEM_TEST) {
            sol_resolver_function(&resolver, index);
        } else if (syntax->items[index].kind == SOL_ITEM_TYPE) {
            sol_resolver_type_declaration(&resolver, index);
        } else if (syntax->items[index].kind == SOL_ITEM_CAPABILITY) {
            sol_resolver_capability_members(&resolver, index);
        } else if (syntax->items[index].kind == SOL_ITEM_TRAIT
            || syntax->items[index].kind == SOL_ITEM_IMPLEMENTATION) {
            sol_resolver_trait_methods(&resolver, index);
        }
    }
    sol_resolver_collect_semantic_references(&resolver);

    free(resolver.bindings);
    free(resolver.expression_states);
    free(resolver.import_targets);
    free(resolver.import_names);
    if (!package_aware) {
        free(module->file_scopes);
        free(module->item_files);
        module->file_scopes = NULL;
        module->file_scope_count = 0;
        module->item_files = NULL;
    }
    return !resolver.allocation_failed
        && !resolver.malformed
        && !diagnostics->allocation_failed;
}

bool sol_hir_lower_scoped(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolHirFileScope *scopes,
    size_t scope_count,
    SolHirModule *module,
    SolDiagnostics *diagnostics
) {
    return sol_hir_lower_impl(
        source, syntax, scopes, scope_count, true, module, diagnostics
    );
}

bool sol_hir_lower(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    SolHirModule *module,
    SolDiagnostics *diagnostics
) {
    if (syntax == NULL) return false;
    SolHirFileScope scope = {
        .module_name = syntax->module_name,
        .import_start = 0,
        .import_count = 0,
        .item_start = 0,
        .item_count = syntax->item_count,
    };
    return sol_hir_lower_impl(
        source, syntax, &scope, 1, false, module, diagnostics
    );
}
