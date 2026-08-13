#include "sol/ir.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const SolSource *source;
    const SolSyntaxTree *syntax;
    const SolHirModule *hir;
    const SolTypeTable *types;
    const SolEffectTable *effects;
    const SolContractTable *contracts;
    const SolPackageFile *files;
    size_t file_count;
    SolIr *ir;
    SolDiagnostics *diagnostics;
    SolIrTypeId *application_types;
    SolIrTypeId *function_types;
    unsigned char *application_states;
    unsigned char *function_states;
    SolIrGenericParameterId *generic_parameters;
    SolIrCallableId *definition_callables;
    SolIrCallableId *member_callables;
    SolIrCallableId *method_callables;
    SolIrLocalId *parameter_locals;
    SolIrLocalId *binding_locals;
    SolIrLocalId *pattern_locals;
    bool failed;
} SolIrLowerer;

static bool sol_ir_error(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics != NULL) {
        sol_diagnostics_add(diagnostics, "SOL-INTERNAL-006", SOL_SEVERITY_ERROR,
            (SolSpan){0}, "%s", message);
    }
    return false;
}

void sol_ir_init(SolIr *ir) {
    if (ir != NULL) memset(ir, 0, sizeof(*ir));
}

void sol_ir_free(SolIr *ir) {
    if (ir == NULL) return;
    free(ir->source_path);
    free(ir->source_bytes);
    for (size_t index = 0; index < ir->definition_count; ++index) {
        free(ir->definitions[index].name);
    }
    for (size_t index = 0; index < ir->callable_count; ++index) {
        free(ir->callables[index].name);
    }
    for (size_t index = 0; index < ir->local_count; ++index) free(ir->locals[index].name);
    for (size_t index = 0; index < ir->field_count; ++index) free(ir->fields[index].name);
    for (size_t index = 0; index < ir->variant_count; ++index) free(ir->variants[index].name);
    for (size_t index = 0; index < ir->expression_count; ++index) {
        SolIrExpression *expression = &ir->expressions[index];
        if (expression->kind == SOL_IR_EXPR_STRING) free(expression->as.string);
        if (expression->kind == SOL_IR_EXPR_HANDLE) free(expression->as.handler.effect_name);
    }
    for (size_t index = 0; index < ir->effect_count; ++index) free(ir->effects[index].name);
    for (size_t index = 0; index < ir->generic_parameter_count; ++index) {
        free(ir->generic_parameters[index].name);
    }
    for (size_t index = 0; index < ir->effect_parameter_count; ++index) {
        free(ir->effect_parameters[index].name);
    }
    free(ir->types);
    free(ir->type_ids);
    free(ir->definitions);
    free(ir->callables);
    free(ir->members);
    free(ir->evidence);
    free(ir->locals);
    free(ir->fields);
    free(ir->variants);
    free(ir->expressions);
    free(ir->statements);
    free(ir->statement_ids);
    free(ir->arms);
    free(ir->arm_ids);
    free(ir->operands);
    free(ir->roots);
    free(ir->effects);
    free(ir->generic_parameters);
    free(ir->effect_parameters);
    free(ir->obligations);
    free(ir->snapshots);
    for (size_t index = 0; index < ir->file_count; ++index) free(ir->files[index].path);
    free(ir->files);
    memset(ir, 0, sizeof(*ir));
}

static bool sol_ir_empty(const SolIr *ir) {
    if (ir == NULL) return false;
    const unsigned char *bytes = (const unsigned char *)ir;
    for (size_t index = 0; index < sizeof(*ir); ++index) {
        if (bytes[index] != 0) return false;
    }
    return true;
}

static bool sol_ir_span_valid(const SolSource *source, SolSpan span) {
    return source != NULL && source->text != NULL
        && span.start <= span.end && span.end <= source->length;
}

static char *sol_ir_copy_text(const char *text, size_t length) {
    if (length == SIZE_MAX) return NULL;
    char *copy = malloc(length + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static void *sol_ir_allocate(size_t count, size_t size, bool zeroed) {
    if (count == 0) return NULL;
    if (size == 0 || count > SIZE_MAX / size) return NULL;
    return zeroed ? calloc(count, size) : malloc(count * size);
}

static char *sol_ir_copy_span(const SolSource *source, SolSpan span) {
    if (!sol_ir_span_valid(source, span)) return NULL;
    return sol_ir_copy_text(source->text + span.start, span.end - span.start);
}

static bool sol_ir_grow(
    void **items, size_t *count, size_t additional, size_t size, void **first
) {
    if (additional == 0) {
        if (first != NULL) *first = NULL;
        return true;
    }
    if (size == 0 || *count > SIZE_MAX - additional
        || *count + additional > SIZE_MAX / size) return false;
    size_t old = *count;
    size_t next = old + additional;
    void *grown = realloc(*items, next * size);
    if (grown == NULL) return false;
    *items = grown;
    memset((unsigned char *)grown + old * size, 0, additional * size);
    *count = next;
    if (first != NULL) *first = (unsigned char *)grown + old * size;
    return true;
}

static bool sol_ir_append_type_id(SolIrLowerer *lowerer, SolIrTypeId type) {
    SolIrTypeId *entry = NULL;
    if (!sol_ir_grow((void **)&lowerer->ir->type_ids, &lowerer->ir->type_id_count,
        1, sizeof(*entry), (void **)&entry)) return false;
    *entry = type;
    return true;
}

static bool sol_ir_append_root(SolIrLowerer *lowerer, SolIrLocalId root) {
    SolIrLocalId *entry = NULL;
    if (!sol_ir_grow((void **)&lowerer->ir->roots, &lowerer->ir->root_count,
        1, sizeof(*entry), (void **)&entry)) return false;
    *entry = root;
    return true;
}

static bool sol_ir_append_member(SolIrLowerer *lowerer, SolIrCallableId callable) {
    SolIrMember *entry = NULL;
    if (!sol_ir_grow((void **)&lowerer->ir->members, &lowerer->ir->member_count,
        1, sizeof(*entry), (void **)&entry)) return false;
    entry->callable = callable;
    return true;
}

static bool sol_ir_append_statement_id(
    SolIrLowerer *lowerer, SolIrStatementId statement
) {
    SolIrStatementId *entry = NULL;
    if (!sol_ir_grow((void **)&lowerer->ir->statement_ids,
        &lowerer->ir->statement_id_count, 1, sizeof(*entry), (void **)&entry)) return false;
    *entry = statement;
    return true;
}

static bool sol_ir_append_arm_id(SolIrLowerer *lowerer, SolIrArmId arm) {
    SolIrArmId *entry = NULL;
    if (!sol_ir_grow((void **)&lowerer->ir->arm_ids,
        &lowerer->ir->arm_id_count, 1, sizeof(*entry), (void **)&entry)) return false;
    *entry = arm;
    return true;
}

static SolIrSlice sol_ir_provenance(SolIrLowerer *lowerer, SolProvenanceId id) {
    SolIrSlice slice = {.offset = lowerer->ir->root_count};
    if (id == SOL_PROVENANCE_NONE) return slice;
    SolProvenance provenance;
    if (!sol_type_provenance(lowerer->types, id, &provenance)) {
        lowerer->failed = true;
        return slice;
    }
    for (size_t index = 0; index < provenance.count; ++index) {
        SolParameterId parameter = provenance.roots[index];
        if (parameter >= lowerer->syntax->parameter_count
            || lowerer->parameter_locals[parameter] == SOL_IR_NONE
            || !sol_ir_append_root(lowerer, lowerer->parameter_locals[parameter])) {
            lowerer->failed = true;
            return slice;
        }
        ++slice.count;
    }
    return slice;
}

static SolIrDefinitionKind sol_ir_definition_kind(const SolSyntaxItem *item) {
    switch (item->kind) {
        case SOL_ITEM_RECORD: return SOL_IR_DEFINITION_RECORD;
        case SOL_ITEM_ENUM: return SOL_IR_DEFINITION_ENUM;
        case SOL_ITEM_CAPABILITY: return SOL_IR_DEFINITION_CAPABILITY;
        case SOL_ITEM_FUNCTION: return SOL_IR_DEFINITION_FUNCTION;
        case SOL_ITEM_TRAIT: return SOL_IR_DEFINITION_TRAIT;
        case SOL_ITEM_IMPLEMENTATION: return SOL_IR_DEFINITION_IMPLEMENTATION;
        case SOL_ITEM_TYPE:
            return item->flavor == SOL_TYPE_DECLARATION_REFINED
                ? SOL_IR_DEFINITION_REFINED : SOL_IR_DEFINITION_DISTINCT;
    }
    return SOL_IR_DEFINITION_FUNCTION;
}

static SolIrTypeId sol_ir_type(SolIrLowerer *lowerer, SolType type);
static SolIrSlice sol_ir_effect_row(
    SolIrLowerer *lowerer, const SolEffectAtom *atoms, size_t count
);
static int sol_ir_effect_compare(const void *left_pointer, const void *right_pointer);

static SolIrTypeId sol_ir_add_type(SolIrLowerer *lowerer, SolIrType candidate) {
    for (size_t index = 0; index < lowerer->ir->type_count; ++index) {
        const SolIrType *existing = &lowerer->ir->types[index];
        if (existing->kind != candidate.kind || existing->definition != candidate.definition
            || existing->argument_count != candidate.argument_count
            || existing->parameter_count != candidate.parameter_count
            || existing->result != candidate.result
            || existing->effects.count != candidate.effects.count) continue;
        bool equal = true;
        for (size_t argument = 0; equal && argument < candidate.argument_count; ++argument) {
            equal = lowerer->ir->type_ids[existing->argument_offset + argument]
                == lowerer->ir->type_ids[candidate.argument_offset + argument];
        }
        for (size_t parameter = 0; equal && parameter < candidate.parameter_count; ++parameter) {
            equal = lowerer->ir->type_ids[existing->parameter_offset + parameter]
                == lowerer->ir->type_ids[candidate.parameter_offset + parameter];
        }
        for (size_t effect = 0; equal && effect < candidate.effects.count; ++effect) {
            equal = sol_ir_effect_compare(
                &lowerer->ir->effects[existing->effects.offset + effect],
                &lowerer->ir->effects[candidate.effects.offset + effect]
            ) == 0;
        }
        if (equal) return index;
    }
    SolIrType *entry = NULL;
    if (!sol_ir_grow((void **)&lowerer->ir->types, &lowerer->ir->type_count,
        1, sizeof(*entry), (void **)&entry)) {
        lowerer->failed = true;
        return SOL_IR_NONE;
    }
    *entry = candidate;
    return lowerer->ir->type_count - 1;
}

static SolIrTypeId sol_ir_type(SolIrLowerer *lowerer, SolType type) {
    SolIrType candidate = {
        .definition = SOL_IR_NONE,
        .result = SOL_IR_NONE,
        .argument_offset = lowerer->ir->type_id_count,
        .parameter_offset = lowerer->ir->type_id_count,
    };
    switch (type.kind) {
        case SOL_TYPE_INT64: candidate.kind = SOL_IR_TYPE_INT64; break;
        case SOL_TYPE_BOOL: candidate.kind = SOL_IR_TYPE_BOOL; break;
        case SOL_TYPE_TEXT: candidate.kind = SOL_IR_TYPE_TEXT; break;
        case SOL_TYPE_UNIT: candidate.kind = SOL_IR_TYPE_UNIT; break;
        case SOL_TYPE_NEVER: candidate.kind = SOL_IR_TYPE_NEVER; break;
        case SOL_TYPE_NOMINAL:
        case SOL_TYPE_FUNCTION:
            if (type.definition >= lowerer->hir->definition_count) {
                lowerer->failed = true;
                return SOL_IR_NONE;
            }
            candidate.kind = type.kind == SOL_TYPE_FUNCTION
                ? SOL_IR_TYPE_FUNCTION : SOL_IR_TYPE_NOMINAL;
            candidate.definition = type.definition;
            break;
        case SOL_TYPE_PARAMETER:
            if (type.definition >= lowerer->syntax->type_parameter_count
                || lowerer->generic_parameters == NULL
                || lowerer->generic_parameters[type.definition] == SOL_IR_NONE) {
                lowerer->failed = true;
                return SOL_IR_NONE;
            }
            candidate.kind = SOL_IR_TYPE_PARAMETER;
            candidate.definition = lowerer->generic_parameters[type.definition];
            break;
        case SOL_TYPE_SELF:
            candidate.kind = SOL_IR_TYPE_SELF;
            candidate.definition = type.definition;
            break;
        case SOL_TYPE_APPLICATION: {
            if (type.definition >= lowerer->types->type_application_count
                || lowerer->application_types == NULL) {
                lowerer->failed = true;
                return SOL_IR_NONE;
            }
            if (lowerer->application_types[type.definition] != SOL_IR_NONE) {
                return lowerer->application_types[type.definition];
            }
            if (lowerer->application_states[type.definition] == 1) {
                lowerer->failed = true;
                return SOL_IR_NONE;
            }
            lowerer->application_states[type.definition] = 1;
            const SolTypeApplication *application
                = &lowerer->types->type_applications[type.definition];
            const SolType *arguments = NULL;
            size_t count = 0;
            if (!sol_type_application_arguments(
                lowerer->types, type, &arguments, &count
            )) {
                lowerer->failed = true;
                return SOL_IR_NONE;
            }
            candidate.kind = application->constructor == SOL_TYPE_CONSTRUCTOR_OPTION
                ? SOL_IR_TYPE_OPTION
                : application->constructor == SOL_TYPE_CONSTRUCTOR_RESULT
                    ? SOL_IR_TYPE_RESULT : SOL_IR_TYPE_NOMINAL;
            candidate.definition = application->constructor == SOL_TYPE_CONSTRUCTOR_USER
                ? application->definition : SOL_IR_NONE;
            candidate.argument_count = count;
            SolIrTypeId *lowered_arguments = count == 0
                ? NULL : sol_ir_allocate(count, sizeof(*lowered_arguments), false);
            if (count != 0 && lowered_arguments == NULL) {
                lowerer->failed = true;
                return SOL_IR_NONE;
            }
            for (size_t index = 0; index < count; ++index) {
                lowered_arguments[index] = sol_ir_type(lowerer, arguments[index]);
                if (lowered_arguments[index] == SOL_IR_NONE) {
                    free(lowered_arguments);
                    lowerer->failed = true;
                    return SOL_IR_NONE;
                }
            }
            candidate.argument_offset = lowerer->ir->type_id_count;
            for (size_t index = 0; index < count; ++index) {
                if (!sol_ir_append_type_id(lowerer, lowered_arguments[index])) {
                    free(lowered_arguments);
                    lowerer->failed = true;
                    return SOL_IR_NONE;
                }
            }
            free(lowered_arguments);
            SolIrTypeId result = sol_ir_add_type(lowerer, candidate);
            lowerer->application_types[type.definition] = result;
            lowerer->application_states[type.definition] = 2;
            return result;
        }
        case SOL_TYPE_FUNCTION_SIGNATURE: {
            if (type.definition >= lowerer->types->function_type_count
                || lowerer->function_types == NULL) {
                lowerer->failed = true;
                return SOL_IR_NONE;
            }
            if (lowerer->function_types[type.definition] != SOL_IR_NONE) {
                return lowerer->function_types[type.definition];
            }
            if (lowerer->function_states[type.definition] == 1) {
                lowerer->failed = true;
                return SOL_IR_NONE;
            }
            lowerer->function_states[type.definition] = 1;
            const SolFunctionType *function = &lowerer->types->function_types[type.definition];
            candidate.kind = SOL_IR_TYPE_FUNCTION;
            candidate.parameter_count = function->parameter_count;
            SolIrTypeId *lowered_parameters = function->parameter_count == 0
                ? NULL : sol_ir_allocate(function->parameter_count,
                    sizeof(*lowered_parameters), false);
            if (function->parameter_count != 0 && lowered_parameters == NULL) {
                lowerer->failed = true;
                return SOL_IR_NONE;
            }
            for (size_t index = 0; index < function->parameter_count; ++index) {
                lowered_parameters[index] = sol_ir_type(lowerer, function->parameters[index]);
                if (lowered_parameters[index] == SOL_IR_NONE) {
                    free(lowered_parameters);
                    lowerer->failed = true;
                    return SOL_IR_NONE;
                }
            }
            candidate.result = sol_ir_type(lowerer, function->result);
            candidate.effects = sol_ir_effect_row(
                lowerer, function->effects.atoms, function->effects.count
            );
            candidate.parameter_offset = lowerer->ir->type_id_count;
            for (size_t index = 0; index < function->parameter_count; ++index) {
                if (!sol_ir_append_type_id(lowerer, lowered_parameters[index])) {
                    free(lowered_parameters);
                    lowerer->failed = true;
                    return SOL_IR_NONE;
                }
            }
            free(lowered_parameters);
            SolIrTypeId result = sol_ir_add_type(lowerer, candidate);
            lowerer->function_types[type.definition] = result;
            lowerer->function_states[type.definition] = 2;
            return result;
        }
        default:
            lowerer->failed = true;
            return SOL_IR_NONE;
    }
    return sol_ir_add_type(lowerer, candidate);
}

static SolIrLocalId sol_ir_local_for_parameter(
    const SolIrLowerer *lowerer, SolParameterId parameter
) {
    return parameter < lowerer->syntax->parameter_count
        ? lowerer->parameter_locals[parameter] : SOL_IR_NONE;
}

static int sol_ir_effect_compare(const void *left_pointer, const void *right_pointer) {
    const SolIrEffect *left = left_pointer;
    const SolIrEffect *right = right_pointer;
    int result = strcmp(left->name, right->name);
    if (result != 0) return result;
    if (left->authority_kind != right->authority_kind) {
        return left->authority_kind < right->authority_kind ? -1 : 1;
    }
    return left->authority < right->authority ? -1 : left->authority > right->authority;
}

static SolIrSlice sol_ir_effect_row(
    SolIrLowerer *lowerer, const SolEffectAtom *atoms, size_t count
) {
    SolIrSlice slice = {.offset = lowerer->ir->effect_count, .count = count};
    if (count == 0) return slice;
    SolIrEffect *entries = NULL;
    if (!sol_ir_grow((void **)&lowerer->ir->effects, &lowerer->ir->effect_count,
        count, sizeof(*entries), (void **)&entries)) {
        lowerer->failed = true;
        return slice;
    }
    for (size_t index = 0; index < count; ++index) {
        const SolEffectAtom *atom = &atoms[index];
        if (!sol_ir_span_valid(lowerer->source, atom->name)
            || !sol_ir_span_valid(lowerer->source, atom->argument)) {
            lowerer->failed = true;
            return slice;
        }
        entries[index].name = sol_ir_copy_span(lowerer->source, atom->name);
        entries[index].authority = SOL_IR_NONE;
        if (entries[index].name == NULL) {
            lowerer->failed = true;
            return slice;
        }
        if (atom->argument_kind == SOL_EFFECT_ATOM_PARAMETER) {
            entries[index].authority_kind = SOL_IR_AUTHORITY_LOCAL;
            entries[index].authority = sol_ir_local_for_parameter(lowerer, atom->parameter);
            if (entries[index].authority == SOL_IR_NONE) lowerer->failed = true;
        } else if (atom->argument_kind == SOL_EFFECT_ATOM_SELF) {
            entries[index].authority_kind = SOL_IR_AUTHORITY_SELF;
        } else if (atom->argument_kind != SOL_EFFECT_ATOM_NO_ARGUMENT) {
            lowerer->failed = true;
        }
    }
    qsort(entries, count, sizeof(*entries), sol_ir_effect_compare);
    for (size_t index = 1; index < count; ++index) {
        if (sol_ir_effect_compare(&entries[index - 1], &entries[index]) >= 0) {
            lowerer->failed = true;
        }
    }
    return slice;
}

static SolIrSlice sol_ir_parameter_slice(
    SolIrLowerer *lowerer, SolParameterId parameter, bool skip_first
) {
    SolIrSlice slice = {.offset = lowerer->ir->root_count};
    if (skip_first && parameter != SOL_AST_NONE) {
        parameter = lowerer->syntax->parameters[parameter].next;
    }
    size_t traversed = 0;
    while (parameter != SOL_AST_NONE) {
        if (parameter >= lowerer->syntax->parameter_count
            || traversed++ >= lowerer->syntax->parameter_count
            || lowerer->parameter_locals[parameter] == SOL_IR_NONE
            || !sol_ir_append_root(lowerer, lowerer->parameter_locals[parameter])) {
            lowerer->failed = true;
            break;
        }
        ++slice.count;
        parameter = lowerer->syntax->parameters[parameter].next;
    }
    return slice;
}

static bool sol_ir_allocate_maps(SolIrLowerer *lowerer) {
#define SOL_IR_MAP(field, count) \
    do { \
        if ((count) != 0) { \
            lowerer->field = sol_ir_allocate((count), sizeof(*lowerer->field), false); \
            if (lowerer->field == NULL) return false; \
            for (size_t index = 0; index < (count); ++index) lowerer->field[index] = SOL_IR_NONE; \
        } \
    } while (0)
    SOL_IR_MAP(application_types, lowerer->types->type_application_count);
    SOL_IR_MAP(function_types, lowerer->types->function_type_count);
    SOL_IR_MAP(generic_parameters, lowerer->syntax->type_parameter_count);
    SOL_IR_MAP(definition_callables, lowerer->syntax->item_count);
    SOL_IR_MAP(member_callables, lowerer->syntax->capability_member_count);
    SOL_IR_MAP(method_callables, lowerer->syntax->trait_method_count);
    SOL_IR_MAP(parameter_locals, lowerer->syntax->parameter_count);
    SOL_IR_MAP(binding_locals, lowerer->syntax->statement_count);
    SOL_IR_MAP(pattern_locals, lowerer->syntax->pattern_binding_count);
#undef SOL_IR_MAP
    if (lowerer->types->type_application_count != 0) {
        lowerer->application_states = sol_ir_allocate(
            lowerer->types->type_application_count, 1, true);
        if (lowerer->application_states == NULL) return false;
    }
    if (lowerer->types->function_type_count != 0) {
        lowerer->function_states = sol_ir_allocate(
            lowerer->types->function_type_count, 1, true);
        if (lowerer->function_states == NULL) return false;
    }
    return true;
}

static void sol_ir_free_maps(SolIrLowerer *lowerer) {
    free(lowerer->application_types);
    free(lowerer->function_types);
    free(lowerer->application_states);
    free(lowerer->function_states);
    free(lowerer->generic_parameters);
    free(lowerer->definition_callables);
    free(lowerer->member_callables);
    free(lowerer->method_callables);
    free(lowerer->parameter_locals);
    free(lowerer->binding_locals);
    free(lowerer->pattern_locals);
}

static bool sol_ir_frontend_shape_valid(SolIrLowerer *lowerer) {
    const SolSyntaxTree *syntax = lowerer->syntax;
    const SolHirModule *hir = lowerer->hir;
    const SolTypeTable *types = lowerer->types;
    const SolEffectTable *effects = lowerer->effects;
    const SolContractTable *contracts = lowerer->contracts;
    if (!sol_syntax_contracts_validate(lowerer->source, syntax)
        || !sol_type_resolution_metadata_valid(syntax, types)
        || hir->definition_count != syntax->item_count
        || hir->resolution_count != syntax->expression_count
        || hir->trait_resolution_count != syntax->item_count
        || hir->bound_resolution_count != syntax->type_parameter_count
        || hir->local_count > hir->local_capacity
        || (hir->definition_count != 0 && hir->definitions == NULL)
        || (hir->resolution_count != 0
            && (hir->resolutions == NULL || hir->expression_owners == NULL))
        || (hir->trait_resolution_count != 0 && hir->trait_resolutions == NULL)
        || (hir->bound_resolution_count != 0 && hir->bound_resolutions == NULL)
        || (hir->local_count != 0 && hir->locals == NULL)
        || types->expression_count != syntax->expression_count
        || types->local_count != hir->local_count
        || types->definition_count != syntax->item_count
        || types->declared_type_count != syntax->type_count
        || types->representation_count != syntax->item_count
        || types->implementation_target_count != syntax->item_count
        || types->handler_count != syntax->expression_count
        || types->call_instantiation_count != syntax->expression_count
        || types->method_resolution_count != syntax->expression_count
        || types->member_resolution_count != syntax->expression_count
        || types->pattern_resolution_count != syntax->pattern_count
        || types->argument_resolution_count != syntax->argument_count
        || types->construction_count != syntax->expression_count
        || types->type_application_count > types->type_application_capacity
        || types->type_application_argument_count > types->type_application_argument_capacity
        || types->function_type_count > types->function_type_capacity
        || types->provenance_count > types->provenance_capacity
        || types->provenance_root_count > types->provenance_root_capacity
        || types->call_instantiation_argument_count
            > types->call_instantiation_argument_capacity
        || (types->expression_count != 0
            && (types->expressions == NULL
                || types->expression_capability_origins == NULL
                || types->expression_operation_origins == NULL
                || types->handlers == NULL || types->call_instantiations == NULL
                || types->method_resolutions == NULL || types->field_resolutions == NULL
                || types->variant_resolutions == NULL || types->constructions == NULL))
        || (types->local_count != 0 && (types->locals == NULL
                || types->local_capability_origins == NULL
                || types->local_operation_origins == NULL))
        || (types->definition_count != 0 && types->definitions == NULL)
        || (types->declared_type_count != 0 && types->declared_types == NULL)
        || (types->representation_count != 0 && types->representations == NULL)
        || (types->implementation_target_count != 0
            && types->implementation_targets == NULL)
        || (types->pattern_resolution_count != 0
            && types->pattern_variant_resolutions == NULL)
        || (types->argument_resolution_count != 0
            && types->argument_field_resolutions == NULL)
        || (types->type_application_count != 0 && types->type_applications == NULL)
        || (types->type_application_argument_count != 0
            && types->type_application_arguments == NULL)
        || (types->function_type_count != 0 && types->function_types == NULL)
        || (types->provenance_count != 0 && types->provenances == NULL)
        || (types->provenance_root_count != 0 && types->provenance_roots == NULL)
        || effects->function_count != syntax->item_count
        || effects->capability_member_count != syntax->capability_member_count
        || effects->trait_method_count != syntax->trait_method_count
        || effects->call_instantiation_count != syntax->expression_count
        || effects->call_instantiation_count > effects->call_instantiation_capacity
        || effects->call_argument_count > effects->call_argument_capacity
        || effects->call_row_count > effects->call_row_capacity
        || (effects->function_count != 0 && effects->functions == NULL)
        || (effects->capability_member_count != 0 && effects->capability_members == NULL)
        || (effects->trait_method_count != 0 && effects->trait_methods == NULL)
        || (effects->call_instantiation_count != 0
            && effects->call_instantiations == NULL)
        || (effects->call_argument_count != 0 && effects->call_arguments == NULL)
        || (effects->call_row_count != 0 && effects->call_rows == NULL)
        || contracts->expression_count != syntax->expression_count
        || contracts->snapshot_count > contracts->snapshot_capacity
        || (contracts->obligation_count != 0 && contracts->obligations == NULL)
        || (contracts->snapshot_count != 0 && contracts->snapshots == NULL)
        || (contracts->expression_count != 0 && contracts->expression_snapshots == NULL)) {
        return false;
    }
    for (size_t index = 0; index < types->type_application_count; ++index) {
        const SolTypeApplication *application = &types->type_applications[index];
        if (application->argument_offset > types->type_application_argument_count
            || application->argument_count > types->type_application_argument_count
                - application->argument_offset) return false;
    }
    for (size_t index = 0; index < types->function_type_count; ++index) {
        const SolFunctionType *function = &types->function_types[index];
        if ((function->parameter_count != 0 && function->parameters == NULL)
            || (function->effects.count != 0 && function->effects.atoms == NULL)) return false;
        for (size_t atom = 0; atom < function->effects.count; ++atom) {
            if (!sol_ir_span_valid(lowerer->source, function->effects.atoms[atom].name)
                || !sol_ir_span_valid(
                    lowerer->source, function->effects.atoms[atom].argument
                )) return false;
        }
    }
    for (size_t index = 0; index < types->provenance_count; ++index) {
        const SolProvenanceSet *set = &types->provenances[index];
        if (set->root_offset > types->provenance_root_count
            || set->root_count > types->provenance_root_count - set->root_offset) return false;
    }
    for (size_t index = 0; index < types->call_instantiation_count; ++index) {
        const SolCallInstantiation *call = &types->call_instantiations[index];
        if (call->function != SOL_AST_NONE
            && (call->function >= syntax->item_count
                || call->argument_offset > types->call_instantiation_argument_count
                || call->argument_count > types->call_instantiation_argument_count
                    - call->argument_offset)) return false;
    }
    for (size_t index = 0; index < types->method_resolution_count; ++index) {
        const SolMethodResolution *method = &types->method_resolutions[index];
        if (method->kind > SOL_METHOD_RESOLUTION_IMPLEMENTATION
            || (method->kind != SOL_METHOD_RESOLUTION_NONE
                && (method->call != index || method->trait >= syntax->item_count
                    || method->requirement >= syntax->trait_method_count
                    || method->method >= syntax->trait_method_count
                    || (method->kind == SOL_METHOD_RESOLUTION_IMPLEMENTATION
                        && method->implementation >= syntax->item_count)))) return false;
    }
    for (size_t index = 0; index < types->handler_count; ++index) {
        const SolHandler *handler = &types->handlers[index];
        bool is_handler = syntax->expressions[index].kind == SOL_EXPR_HANDLE;
        if (is_handler != (handler->source_member != SOL_AST_NONE)) return false;
        if (is_handler && (handler->source_member >= syntax->capability_member_count
                || handler->provider_member >= syntax->capability_member_count
                || handler->root >= syntax->parameter_count)) return false;
    }
    for (size_t index = 0; index < types->representation_count; ++index) {
        if (syntax->items[index].kind == SOL_ITEM_TYPE
            && types->representations[index].flavor == SOL_TYPE_DECLARATION_NONE) return false;
    }
    const SolEffectRow *rows[] = {
        effects->functions, effects->capability_members, effects->trait_methods,
    };
    const size_t counts[] = {
        effects->function_count, effects->capability_member_count,
        effects->trait_method_count,
    };
    for (size_t table = 0; table < 3; ++table) {
        for (size_t row = 0; row < counts[table]; ++row) {
            if (rows[table][row].count != 0 && rows[table][row].atoms == NULL) return false;
            for (size_t atom = 0; atom < rows[table][row].count; ++atom) {
                if (!sol_ir_span_valid(lowerer->source, rows[table][row].atoms[atom].name)
                    || !sol_ir_span_valid(
                        lowerer->source, rows[table][row].atoms[atom].argument
                    )) return false;
            }
        }
    }
    for (size_t index = 0; index < effects->call_instantiation_count; ++index) {
        const SolEffectCallInstantiation *call = &effects->call_instantiations[index];
        if (call->call != SOL_AST_NONE
            && (call->call != index || call->function >= syntax->item_count
                || call->argument_offset > effects->call_argument_count
                || call->argument_count > effects->call_argument_count - call->argument_offset
                || call->row_offset > effects->call_row_count
                || call->row_count > effects->call_row_count - call->row_offset)) return false;
    }
    for (size_t index = 0; index < contracts->obligation_count; ++index) {
        const SolObligation *obligation = &contracts->obligations[index];
        if (obligation->id != index || obligation->predicate >= syntax->expression_count
            || obligation->first_snapshot > contracts->snapshot_count
            || obligation->snapshot_count > contracts->snapshot_count
                - obligation->first_snapshot) return false;
    }
    for (size_t index = 0; index < contracts->snapshot_count; ++index) {
        const SolSnapshot *snapshot = &contracts->snapshots[index];
        if (snapshot->id != index || snapshot->obligation >= contracts->obligation_count
            || snapshot->old_expression >= syntax->expression_count
            || snapshot->operand >= syntax->expression_count) return false;
    }
    return true;
}

static bool sol_ir_lower_locals(SolIrLowerer *lowerer) {
    size_t count = lowerer->hir->local_count;
    if (count != 0) {
        lowerer->ir->locals = sol_ir_allocate(count, sizeof(*lowerer->ir->locals), true);
        if (lowerer->ir->locals == NULL) return false;
    }
    lowerer->ir->local_count = count;
    for (size_t index = 0; index < count; ++index) {
        const SolHirLocal *source = &lowerer->hir->locals[index];
        SolIrLocal *local = &lowerer->ir->locals[index];
        local->owner = source->owner;
        SolSpan name = {0};
        if (source->kind == SOL_LOCAL_PARAMETER
            && source->syntax_id < lowerer->syntax->parameter_count) {
            local->kind = SOL_IR_LOCAL_PARAMETER;
            name = lowerer->syntax->parameters[source->syntax_id].name;
            lowerer->parameter_locals[source->syntax_id] = index;
        } else if (source->kind == SOL_LOCAL_BINDING
            && source->syntax_id < lowerer->syntax->statement_count) {
            local->kind = SOL_IR_LOCAL_BINDING;
            name = lowerer->syntax->statements[source->syntax_id].as.let_statement.name;
            lowerer->binding_locals[source->syntax_id] = index;
        } else if (source->kind == SOL_LOCAL_PATTERN
            && source->syntax_id < lowerer->syntax->pattern_binding_count) {
            local->kind = SOL_IR_LOCAL_PATTERN;
            name = lowerer->syntax->pattern_bindings[source->syntax_id].name;
            lowerer->pattern_locals[source->syntax_id] = index;
        } else {
            return false;
        }
        local->name = sol_ir_copy_span(lowerer->source, name);
        if (local->name == NULL) return false;
    }
    for (size_t index = 0; index < count; ++index) {
        SolIrLocal *local = &lowerer->ir->locals[index];
        local->type = sol_ir_type(lowerer, lowerer->types->locals[index]);
        local->capability_roots = sol_ir_provenance(
            lowerer, lowerer->types->local_capability_origins[index]
        );
        local->operation_roots = sol_ir_provenance(
            lowerer, lowerer->types->local_operation_origins[index]
        );
        if (local->name == NULL || local->type == SOL_IR_NONE) return false;
    }
    return !lowerer->failed;
}

static bool sol_ir_lower_fields_variants(SolIrLowerer *lowerer) {
    size_t field_count = lowerer->syntax->field_count;
    size_t variant_count = lowerer->syntax->variant_count;
    if (field_count != 0) {
        lowerer->ir->fields = sol_ir_allocate(field_count, sizeof(*lowerer->ir->fields), true);
        if (lowerer->ir->fields == NULL) return false;
    }
    if (variant_count != 0) {
        lowerer->ir->variants = sol_ir_allocate(variant_count, sizeof(*lowerer->ir->variants), true);
        if (lowerer->ir->variants == NULL) return false;
    }
    lowerer->ir->field_count = field_count;
    lowerer->ir->variant_count = variant_count;
    for (size_t index = 0; index < variant_count; ++index) {
        const SolVariant *source = &lowerer->syntax->variants[index];
        SolIrVariant *variant = &lowerer->ir->variants[index];
        variant->owner = source->owner_item;
        variant->name = sol_ir_copy_span(lowerer->source, source->name);
        variant->fields.offset = source->first_field == SOL_AST_NONE
            ? field_count : source->first_field;
        SolFieldId field = source->first_field;
        while (field != SOL_AST_NONE) {
            if (field >= field_count || variant->fields.count++ >= field_count) return false;
            field = lowerer->syntax->fields[field].next;
        }
        if (variant->name == NULL) return false;
    }
    for (size_t definition = 0; definition < lowerer->syntax->item_count; ++definition) {
        const SolSyntaxItem *item = &lowerer->syntax->items[definition];
        SolFieldId field = item->kind == SOL_ITEM_RECORD ? item->first_field : SOL_AST_NONE;
        size_t traversed = 0;
        while (field != SOL_AST_NONE) {
            if (field >= field_count || traversed++ >= field_count) return false;
            lowerer->ir->fields[field].owner = definition;
            field = lowerer->syntax->fields[field].next;
        }
    }
    for (size_t variant = 0; variant < variant_count; ++variant) {
        SolFieldId field = lowerer->syntax->variants[variant].first_field;
        size_t traversed = 0;
        while (field != SOL_AST_NONE) {
            if (field >= field_count || traversed++ >= field_count) return false;
            lowerer->ir->fields[field].owner = lowerer->syntax->variants[variant].owner_item;
            field = lowerer->syntax->fields[field].next;
        }
    }
    for (size_t index = 0; index < field_count; ++index) {
        lowerer->ir->fields[index].name = sol_ir_copy_span(
            lowerer->source, lowerer->syntax->fields[index].name
        );
        lowerer->ir->fields[index].type = sol_ir_type(
            lowerer, lowerer->types->declared_types[lowerer->syntax->fields[index].type]
        );
        if (lowerer->ir->fields[index].name == NULL
            || lowerer->ir->fields[index].type == SOL_IR_NONE) return false;
    }
    return !lowerer->failed;
}

static SolIrCallableId sol_ir_add_callable(
    SolIrLowerer *lowerer,
    SolIrCallableKind kind,
    SolIrDefinitionId owner,
    SolSpan name,
    SolSpan span,
    SolParameterId parameters,
    bool skip_first,
    SolType result,
    SolExprId body,
    const SolEffectRow *effects
) {
    SolIrCallable *entry = NULL;
    if (!sol_ir_grow((void **)&lowerer->ir->callables, &lowerer->ir->callable_count,
        1, sizeof(*entry), (void **)&entry)) return SOL_IR_NONE;
    entry->kind = kind;
    entry->owner = owner;
    entry->name = sol_ir_copy_span(lowerer->source, name);
    entry->span = span;
    entry->parameters = sol_ir_parameter_slice(lowerer, parameters, skip_first);
    entry->result = sol_ir_type(lowerer, result);
    entry->body = body == SOL_AST_NONE ? SOL_IR_NONE : body;
    entry->effects = sol_ir_effect_row(lowerer, effects->atoms, effects->count);
    if (owner < lowerer->ir->definition_count) {
        entry->generic_parameters = lowerer->ir->definitions[owner].generic_parameters;
        entry->effect_parameters = lowerer->ir->definitions[owner].effect_parameters;
    }
    if (entry->name == NULL || entry->result == SOL_IR_NONE || lowerer->failed) {
        return SOL_IR_NONE;
    }
    return lowerer->ir->callable_count - 1;
}

static bool sol_ir_lower_generic_metadata(SolIrLowerer *lowerer) {
    size_t definition_count = lowerer->syntax->item_count;
    if (lowerer->syntax->type_parameter_count != 0) {
        lowerer->ir->generic_parameters = sol_ir_allocate(
            lowerer->syntax->type_parameter_count,
            sizeof(*lowerer->ir->generic_parameters), true);
        if (lowerer->ir->generic_parameters == NULL) return false;
    }
    lowerer->ir->generic_parameter_count = lowerer->syntax->type_parameter_count;
    for (size_t index = 0; index < lowerer->syntax->type_parameter_count; ++index) {
        const SolTypeParameter *source = &lowerer->syntax->type_parameters[index];
        if (source->owner_item >= definition_count) return false;
        SolIrGenericParameter *parameter = &lowerer->ir->generic_parameters[index];
        parameter->owner = source->owner_item;
        parameter->name = sol_ir_copy_span(lowerer->source, source->name);
        parameter->trait_bound = SOL_IR_NONE;
        SolResolution bound = lowerer->hir->bound_resolutions[index];
        if (bound.kind == SOL_RESOLUTION_DEFINITION) parameter->trait_bound = bound.target;
        size_t ordinal = 0;
        SolTypeParameterId current
            = lowerer->syntax->items[source->owner_item].first_type_parameter;
        while (current != index) {
            if (current == SOL_AST_NONE || current >= lowerer->syntax->type_parameter_count
                || ordinal++ >= lowerer->syntax->type_parameter_count) return false;
            current = lowerer->syntax->type_parameters[current].next;
        }
        parameter->ordinal = ordinal;
        lowerer->generic_parameters[index] = index;
        if (parameter->name == NULL) return false;
    }
    if (lowerer->syntax->effect_parameter_count != 0) {
        lowerer->ir->effect_parameters = sol_ir_allocate(
            lowerer->syntax->effect_parameter_count,
            sizeof(*lowerer->ir->effect_parameters), true);
        if (lowerer->ir->effect_parameters == NULL) return false;
    }
    lowerer->ir->effect_parameter_count = lowerer->syntax->effect_parameter_count;
    for (size_t index = 0; index < lowerer->syntax->effect_parameter_count; ++index) {
        const SolEffectParameter *source = &lowerer->syntax->effect_parameters[index];
        if (source->owner_item >= definition_count) return false;
        lowerer->ir->effect_parameters[index] = (SolIrEffectParameter){
            .owner = source->owner_item,
            .name = sol_ir_copy_span(lowerer->source, source->name),
        };
        size_t ordinal = 0;
        SolEffectParameterId current
            = lowerer->syntax->items[source->owner_item].first_effect_parameter;
        while (current != index) {
            if (current == SOL_AST_NONE || current >= lowerer->syntax->effect_parameter_count
                || ordinal++ >= lowerer->syntax->effect_parameter_count) return false;
            current = lowerer->syntax->effect_parameters[current].next;
        }
        lowerer->ir->effect_parameters[index].ordinal = ordinal;
        if (lowerer->ir->effect_parameters[index].name == NULL) return false;
    }
    return true;
}

static bool sol_ir_lower_declarations(SolIrLowerer *lowerer) {
    size_t count = lowerer->syntax->item_count;
    if (count != 0) {
        lowerer->ir->definitions = sol_ir_allocate(count,
            sizeof(*lowerer->ir->definitions), true);
        if (lowerer->ir->definitions == NULL) return false;
    }
    lowerer->ir->definition_count = count;
    for (size_t index = 0; index < count; ++index) {
        const SolSyntaxItem *source = &lowerer->syntax->items[index];
        SolIrDefinition *definition = &lowerer->ir->definitions[index];
        definition->kind = sol_ir_definition_kind(source);
        definition->semantic_id = lowerer->hir->definitions[index].semantic_id;
        definition->name = sol_ir_copy_span(lowerer->source, source->name);
        definition->span = source->span;
        definition->callable = SOL_IR_NONE;
        definition->declared_type = lowerer->types->definitions[index].kind
                == SOL_TYPE_UNKNOWN
            ? SOL_IR_NONE : sol_ir_type(lowerer, lowerer->types->definitions[index]);
        definition->representation = SOL_IR_NONE;
        definition->implementation_trait = SOL_IR_NONE;
        definition->implementation_target = SOL_IR_NONE;
        definition->fields.offset = source->first_field == SOL_AST_NONE
            ? lowerer->syntax->field_count : source->first_field;
        definition->variants.offset = source->first_variant == SOL_AST_NONE
            ? lowerer->syntax->variant_count : source->first_variant;
        definition->members.offset = 0;
        definition->generic_parameters.offset = source->first_type_parameter == SOL_AST_NONE
            ? lowerer->ir->generic_parameter_count : source->first_type_parameter;
        definition->effect_parameters.offset = source->first_effect_parameter == SOL_AST_NONE
            ? lowerer->ir->effect_parameter_count : source->first_effect_parameter;
        SolTypeParameterId generic = source->first_type_parameter;
        while (generic != SOL_AST_NONE) {
            if (generic >= lowerer->syntax->type_parameter_count
                || definition->generic_parameters.count++
                    >= lowerer->syntax->type_parameter_count) return false;
            generic = lowerer->syntax->type_parameters[generic].next;
        }
        SolEffectParameterId effect_parameter = source->first_effect_parameter;
        while (effect_parameter != SOL_AST_NONE) {
            if (effect_parameter >= lowerer->syntax->effect_parameter_count
                || definition->effect_parameters.count++
                    >= lowerer->syntax->effect_parameter_count) return false;
            effect_parameter = lowerer->syntax->effect_parameters[effect_parameter].next;
        }
        SolFieldId field = source->first_field;
        while (field != SOL_AST_NONE) {
            if (field >= lowerer->syntax->field_count
                || definition->fields.count++ >= lowerer->syntax->field_count) return false;
            field = lowerer->syntax->fields[field].next;
        }
        SolVariantId variant = source->first_variant;
        while (variant != SOL_AST_NONE) {
            if (variant >= lowerer->syntax->variant_count
                || definition->variants.count++ >= lowerer->syntax->variant_count) return false;
            variant = lowerer->syntax->variants[variant].next;
        }
        if (source->kind == SOL_ITEM_TYPE) {
            definition->representation = sol_ir_type(
                lowerer, lowerer->types->representations[index].representation
            );
        }
        if (source->kind == SOL_ITEM_IMPLEMENTATION) {
            SolResolution trait = lowerer->hir->trait_resolutions[index];
            if (trait.kind != SOL_RESOLUTION_DEFINITION
                || trait.target >= lowerer->syntax->item_count) return false;
            definition->implementation_trait = trait.target;
            definition->implementation_target = sol_ir_type(
                lowerer, lowerer->types->implementation_targets[index]
            );
        } else {
            definition->implementation_trait = SOL_IR_NONE;
            definition->implementation_target = SOL_IR_NONE;
        }
        if (definition->name == NULL) return false;
    }
    for (size_t index = 0; index < count; ++index) {
        const SolSyntaxItem *item = &lowerer->syntax->items[index];
        if (item->kind != SOL_ITEM_FUNCTION) continue;
        SolIrCallableId callable = sol_ir_add_callable(lowerer, SOL_IR_CALLABLE_FUNCTION,
            index, item->name, item->span, item->first_parameter, false,
            lowerer->types->definitions[index], item->body, &lowerer->effects->functions[index]);
        if (callable == SOL_IR_NONE) return false;
        lowerer->definition_callables[index] = callable;
        lowerer->ir->definitions[index].callable = callable;
        lowerer->ir->callables[callable].generic_parameters
            = lowerer->ir->definitions[index].generic_parameters;
        lowerer->ir->callables[callable].effect_parameters
            = lowerer->ir->definitions[index].effect_parameters;
    }
    for (size_t index = 0; index < lowerer->syntax->capability_member_count; ++index) {
        const SolCapabilityMember *member = &lowerer->syntax->capability_members[index];
        SolIrCallableId callable = sol_ir_add_callable(lowerer, SOL_IR_CALLABLE_CAPABILITY,
            member->owner_item, member->name, member->span, member->first_parameter, false,
            lowerer->types->declared_types[member->return_type_id], member->body,
            &lowerer->effects->capability_members[index]);
        SolIrDefinition *definition = &lowerer->ir->definitions[member->owner_item];
        if (definition->members.count == 0) definition->members.offset = lowerer->ir->member_count;
        if (callable == SOL_IR_NONE || !sol_ir_append_member(lowerer, callable)) return false;
        lowerer->member_callables[index] = callable;
        ++definition->members.count;
    }
    for (size_t index = 0; index < lowerer->syntax->trait_method_count; ++index) {
        const SolTraitMethod *method = &lowerer->syntax->trait_methods[index];
        SolIrCallableKind kind = lowerer->syntax->items[method->owner_item].kind
                == SOL_ITEM_IMPLEMENTATION
            ? SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION : SOL_IR_CALLABLE_TRAIT_REQUIREMENT;
        SolIrCallableId callable = sol_ir_add_callable(lowerer, kind, method->owner_item,
            method->name, method->span, method->first_parameter, true,
            lowerer->types->declared_types[method->return_type_id], method->body,
            &lowerer->effects->trait_methods[index]);
        SolIrDefinition *definition = &lowerer->ir->definitions[method->owner_item];
        if (definition->members.count == 0) definition->members.offset = lowerer->ir->member_count;
        if (callable == SOL_IR_NONE || !sol_ir_append_member(lowerer, callable)) return false;
        lowerer->method_callables[index] = callable;
        ++definition->members.count;
    }
    return !lowerer->failed;
}

static bool sol_ir_append_evidence(
    SolIrLowerer *lowerer, SolIrDispatchEvidence evidence
) {
    SolIrDispatchEvidence *entry = NULL;
    if (!sol_ir_grow((void **)&lowerer->ir->evidence, &lowerer->ir->evidence_count,
        1, sizeof(*entry), (void **)&entry)) return false;
    *entry = evidence;
    return true;
}

static SolIrSlice sol_ir_method_evidence(
    SolIrLowerer *lowerer, const SolMethodResolution *method
) {
    SolIrSlice slice = {.offset = lowerer->ir->evidence_count};
    if (method->kind == SOL_METHOD_RESOLUTION_IMPLEMENTATION) {
        SolIrTypeId target = method->implementation < lowerer->types->implementation_target_count
            ? sol_ir_type(lowerer,
                lowerer->types->implementation_targets[method->implementation])
            : SOL_IR_NONE;
        if (target == SOL_IR_NONE || !sol_ir_append_evidence(lowerer,
            (SolIrDispatchEvidence){
                .trait = method->trait,
                .requirement = lowerer->method_callables[method->requirement],
                .implementation = method->implementation,
                .method = lowerer->method_callables[method->method],
                .type = target,
            })) {
            lowerer->failed = true;
            return slice;
        }
        slice.count = 1;
        return slice;
    }
    for (SolDefId definition = 0; definition < lowerer->syntax->item_count; ++definition) {
        if (lowerer->syntax->items[definition].kind != SOL_ITEM_IMPLEMENTATION
            || lowerer->hir->trait_resolutions[definition].kind
                != SOL_RESOLUTION_DEFINITION
            || lowerer->hir->trait_resolutions[definition].target != method->trait) continue;
        SolTraitMethodId implementation_method = lowerer->syntax->items[
            definition
        ].first_trait_method;
        while (implementation_method != SOL_AST_NONE) {
            if (implementation_method >= lowerer->syntax->trait_method_count) {
                lowerer->failed = true;
                return slice;
            }
            const SolTraitMethod *candidate
                = &lowerer->syntax->trait_methods[implementation_method];
            const SolTraitMethod *requirement
                = &lowerer->syntax->trait_methods[method->requirement];
            size_t left = candidate->name.end - candidate->name.start;
            size_t right = requirement->name.end - requirement->name.start;
            if (left == right && memcmp(lowerer->source->text + candidate->name.start,
                lowerer->source->text + requirement->name.start, left) == 0) break;
            implementation_method = candidate->next;
        }
        if (implementation_method == SOL_AST_NONE) continue;
        SolIrTypeId target = sol_ir_type(
            lowerer, lowerer->types->implementation_targets[definition]
        );
        if (target == SOL_IR_NONE || !sol_ir_append_evidence(lowerer,
            (SolIrDispatchEvidence){
                .trait = method->trait,
                .requirement = lowerer->method_callables[method->requirement],
                .implementation = definition,
                .method = lowerer->method_callables[implementation_method],
                .type = target,
            })) {
            lowerer->failed = true;
            return slice;
        }
        ++slice.count;
    }
    if (slice.count == 0) lowerer->failed = true;
    return slice;
}

static SolResolution sol_ir_callee_resolution(
    const SolIrLowerer *lowerer, SolExprId expression
) {
    while (expression < lowerer->syntax->expression_count
        && lowerer->syntax->expressions[expression].kind == SOL_EXPR_TYPE_APPLICATION) {
        expression = lowerer->syntax->expressions[expression].as.type_application.base;
    }
    if (expression >= lowerer->hir->resolution_count) {
        return (SolResolution){SOL_RESOLUTION_ERROR, SOL_AST_NONE};
    }
    return lowerer->hir->resolutions[expression];
}

static bool sol_ir_append_operand(
    SolIrLowerer *lowerer, size_t formal, SolIrExpressionId value
) {
    SolIrOperand *operand = NULL;
    if (!sol_ir_grow((void **)&lowerer->ir->operands, &lowerer->ir->operand_count,
        1, sizeof(*operand), (void **)&operand)) return false;
    *operand = (SolIrOperand){.formal = formal, .value = value};
    return true;
}

static int sol_ir_operand_compare(const void *left_pointer, const void *right_pointer) {
    const SolIrOperand *left = left_pointer;
    const SolIrOperand *right = right_pointer;
    return left->formal < right->formal ? -1 : left->formal > right->formal;
}

static SolIrSlice sol_ir_order_arguments(
    SolIrLowerer *lowerer, SolArgumentId argument, SolParameterId parameter
) {
    SolIrSlice slice = {.offset = lowerer->ir->operand_count};
    size_t formal_count = 0;
    SolParameterId cursor = parameter;
    while (cursor != SOL_AST_NONE) {
        if (cursor >= lowerer->syntax->parameter_count
            || formal_count++ >= lowerer->syntax->parameter_count) {
            lowerer->failed = true;
            return slice;
        }
        cursor = lowerer->syntax->parameters[cursor].next;
    }
    size_t positional = 0;
    size_t traversed = 0;
    while (argument != SOL_AST_NONE) {
        if (argument >= lowerer->syntax->argument_count
            || traversed++ >= lowerer->syntax->argument_count) {
            lowerer->failed = true;
            return slice;
        }
        const SolArgument *actual = &lowerer->syntax->arguments[argument];
        size_t formal = SOL_IR_NONE;
        if (!actual->is_named) {
            formal = positional++;
        } else {
            if (!sol_ir_span_valid(lowerer->source, actual->name)) {
                lowerer->failed = true;
                return slice;
            }
            cursor = parameter;
            for (size_t index = 0; index < formal_count; ++index) {
                const SolParameter *candidate = &lowerer->syntax->parameters[cursor];
                if (!sol_ir_span_valid(lowerer->source, candidate->name)) {
                    lowerer->failed = true;
                    return slice;
                }
                size_t left = candidate->name.end - candidate->name.start;
                size_t right = actual->name.end - actual->name.start;
                if (left == right && memcmp(lowerer->source->text + candidate->name.start,
                    lowerer->source->text + actual->name.start, left) == 0) {
                    formal = index;
                    break;
                }
                cursor = candidate->next;
            }
        }
        if (formal >= formal_count
            || !sol_ir_append_operand(lowerer, formal, actual->value)) {
            lowerer->failed = true;
            return slice;
        }
        ++slice.count;
        argument = actual->next;
    }
    if (slice.count != formal_count) lowerer->failed = true;
    if (slice.count != 0) {
        qsort(lowerer->ir->operands + slice.offset, slice.count,
            sizeof(*lowerer->ir->operands), sol_ir_operand_compare);
        for (size_t index = 0; index < slice.count; ++index) {
            if (lowerer->ir->operands[slice.offset + index].formal != index) {
                lowerer->failed = true;
            }
        }
    }
    return slice;
}

static SolIrSlice sol_ir_field_arguments(
    SolIrLowerer *lowerer, SolArgumentId argument
) {
    SolIrSlice slice = {.offset = lowerer->ir->operand_count};
    size_t traversed = 0;
    while (argument != SOL_AST_NONE) {
        if (argument >= lowerer->syntax->argument_count
            || traversed++ >= lowerer->syntax->argument_count) {
            lowerer->failed = true;
            return slice;
        }
        SolFieldId field = lowerer->types->argument_field_resolutions[argument];
        if (field >= lowerer->syntax->field_count
            || !sol_ir_append_operand(
                lowerer, field, lowerer->syntax->arguments[argument].value
            )) {
            lowerer->failed = true;
            return slice;
        }
        ++slice.count;
        argument = lowerer->syntax->arguments[argument].next;
    }
    if (slice.count != 0) {
        qsort(lowerer->ir->operands + slice.offset, slice.count,
            sizeof(*lowerer->ir->operands), sol_ir_operand_compare);
    }
    return slice;
}

static SolIrSlice sol_ir_positional_arguments(
    SolIrLowerer *lowerer, SolArgumentId argument
) {
    SolIrSlice slice = {.offset = lowerer->ir->operand_count};
    size_t traversed = 0;
    while (argument != SOL_AST_NONE) {
        if (argument >= lowerer->syntax->argument_count
            || traversed >= lowerer->syntax->argument_count
            || !sol_ir_append_operand(lowerer, traversed,
                lowerer->syntax->arguments[argument].value)) {
            lowerer->failed = true;
            return slice;
        }
        ++traversed;
        ++slice.count;
        argument = lowerer->syntax->arguments[argument].next;
    }
    return slice;
}

static bool sol_ir_builtin_head_is_callee(
    const SolIrLowerer *lowerer, SolExprId expression
) {
    for (size_t index = 0; index < lowerer->syntax->expression_count; ++index) {
        const SolExpr *candidate = &lowerer->syntax->expressions[index];
        if (candidate->kind == SOL_EXPR_CALL && candidate->as.call.callee == expression) {
            return true;
        }
    }
    return false;
}

static bool sol_ir_compile_time_head_used(
    const SolIrLowerer *lowerer, SolExprId expression
) {
    for (size_t index = 0; index < lowerer->syntax->expression_count; ++index) {
        const SolExpr *candidate = &lowerer->syntax->expressions[index];
        if ((candidate->kind == SOL_EXPR_CALL && candidate->as.call.callee == expression)
            || (candidate->kind == SOL_EXPR_RECORD
                && candidate->as.record.type == expression)
            || (candidate->kind == SOL_EXPR_TYPE_APPLICATION
                && candidate->as.type_application.base == expression)
            || (candidate->kind == SOL_EXPR_FIELD
                && candidate->as.field.base == expression)) return true;
    }
    return false;
}

static bool sol_ir_decode_string(
    SolIrLowerer *lowerer, SolSpan span, char **output
) {
    if (!sol_ir_span_valid(lowerer->source, span) || span.end - span.start < 2
        || lowerer->source->text[span.start] != '"'
        || lowerer->source->text[span.end - 1] != '"') return false;
    size_t capacity = span.end - span.start - 1;
    char *decoded = sol_ir_allocate(capacity, 1, false);
    if (decoded == NULL) return false;
    size_t count = 0;
    for (size_t index = span.start + 1; index + 1 < span.end; ++index) {
        char value = lowerer->source->text[index];
        if (value == '\\' && index + 2 < span.end) {
            value = lowerer->source->text[++index];
            if (value == 'n') value = '\n';
            else if (value == 'r') value = '\r';
            else if (value == 't') value = '\t';
        }
        decoded[count++] = value;
    }
    decoded[count] = '\0';
    *output = decoded;
    return true;
}

static bool sol_ir_lower_call(
    SolIrLowerer *lowerer, SolExprId id, const SolExpr *source, SolIrExpression *output
) {
    output->as.call.callable = SOL_IR_NONE;
    output->as.call.receiver = SOL_IR_NONE;
    output->as.call.variant = SOL_IR_NONE;
    output->as.call.definition = SOL_IR_NONE;
    output->as.call.callee = source->as.call.callee;
    SolResolution resolution = sol_ir_callee_resolution(lowerer, source->as.call.callee);
    const SolMethodResolution *method = &lowerer->types->method_resolutions[id];
    const SolTypeConstruction *construction = &lowerer->types->constructions[id];
    SolType callee_type = lowerer->types->expressions[source->as.call.callee];
    if (resolution.kind == SOL_RESOLUTION_BUILTIN) {
        output->as.call.kind = resolution.target == SOL_BUILTIN_OK
            ? SOL_IR_CALL_BUILTIN_OK : resolution.target == SOL_BUILTIN_ERR
                ? SOL_IR_CALL_BUILTIN_ERR : resolution.target == SOL_BUILTIN_SOME
                    ? SOL_IR_CALL_BUILTIN_SOME : SOL_IR_CALL_BUILTIN_NONE;
        output->as.call.operands = sol_ir_positional_arguments(
            lowerer, source->as.call.first_argument
        );
    } else if (method->kind != SOL_METHOD_RESOLUTION_NONE) {
        if (method->method >= lowerer->syntax->trait_method_count
            || method->requirement >= lowerer->syntax->trait_method_count) {
            return false;
        }
        output->as.call.kind = SOL_IR_CALL_METHOD;
        output->as.call.callable = lowerer->method_callables[method->method];
        if (output->as.call.callable == SOL_IR_NONE) return false;
        const SolExpr *callee = &lowerer->syntax->expressions[source->as.call.callee];
        if (callee->kind != SOL_EXPR_FIELD) return false;
        output->as.call.receiver = callee->as.field.base;
        output->as.call.evidence = sol_ir_method_evidence(lowerer, method);
        SolParameterId parameter
            = lowerer->syntax->trait_methods[method->requirement].first_parameter;
        if (parameter != SOL_AST_NONE) parameter = lowerer->syntax->parameters[parameter].next;
        output->as.call.operands = sol_ir_order_arguments(
            lowerer, source->as.call.first_argument, parameter
        );
    } else if (construction->definition != SOL_AST_NONE) {
        output->as.call.kind = SOL_IR_CALL_DISTINCT_CONSTRUCTOR;
        output->as.call.definition = construction->definition;
        output->as.call.operands = sol_ir_positional_arguments(
            lowerer, source->as.call.first_argument
        );
    } else if (callee_type.kind == SOL_TYPE_VARIANT) {
        const SolVariantConstructor *variant = sol_type_variant_constructor(
            lowerer->types, callee_type
        );
        if (variant == NULL || variant->variant >= lowerer->syntax->variant_count) return false;
        output->as.call.kind = SOL_IR_CALL_ENUM_CONSTRUCTOR;
        output->as.call.variant = variant->variant;
        output->as.call.definition = lowerer->syntax->variants[variant->variant].owner_item;
        output->as.call.operands = sol_ir_field_arguments(
            lowerer, source->as.call.first_argument
        );
    } else if (callee_type.kind == SOL_TYPE_FUNCTION_SIGNATURE) {
        output->as.call.kind = SOL_IR_CALL_CALLBACK;
        const SolFunctionType *function = &lowerer->types->function_types[callee_type.definition];
        output->as.call.operands.offset = lowerer->ir->operand_count;
        SolArgumentId argument = source->as.call.first_argument;
        for (size_t formal = 0; formal < function->parameter_count; ++formal) {
            if (argument == SOL_AST_NONE || argument >= lowerer->syntax->argument_count
                || !sol_ir_append_operand(lowerer, formal,
                    lowerer->syntax->arguments[argument].value)) return false;
            ++output->as.call.operands.count;
            argument = lowerer->syntax->arguments[argument].next;
        }
        if (argument != SOL_AST_NONE) return false;
    } else if (callee_type.kind == SOL_TYPE_CAPABILITY_OPERATION) {
        if (callee_type.definition >= lowerer->syntax->capability_member_count) return false;
        output->as.call.kind = SOL_IR_CALL_CAPABILITY;
        output->as.call.callable = lowerer->member_callables[callee_type.definition];
        const SolExpr *callee = &lowerer->syntax->expressions[source->as.call.callee];
        if (callee->kind != SOL_EXPR_FIELD) return false;
        output->as.call.receiver = callee->as.field.base;
        output->as.call.operands = sol_ir_order_arguments(lowerer,
            source->as.call.first_argument,
            lowerer->syntax->capability_members[callee_type.definition].first_parameter);
    } else {
        SolDefId function = resolution.kind == SOL_RESOLUTION_DEFINITION
            ? resolution.target : callee_type.definition;
        if (function >= lowerer->syntax->item_count
            || lowerer->syntax->items[function].kind != SOL_ITEM_FUNCTION) return false;
        output->as.call.kind = SOL_IR_CALL_FUNCTION;
        output->as.call.callable = lowerer->definition_callables[function];
        output->as.call.operands = sol_ir_order_arguments(lowerer,
            source->as.call.first_argument, lowerer->syntax->items[function].first_parameter);
    }
    const SolCallInstantiation *instantiation
        = &lowerer->types->call_instantiations[id];
    if (instantiation->function != SOL_AST_NONE) {
        if (instantiation->argument_offset
                > lowerer->types->call_instantiation_argument_count
            || instantiation->argument_count
                > lowerer->types->call_instantiation_argument_count
                    - instantiation->argument_offset) return false;
        SolIrTypeId *arguments = instantiation->argument_count == 0 ? NULL
            : sol_ir_allocate(instantiation->argument_count, sizeof(*arguments), false);
        if (instantiation->argument_count != 0 && arguments == NULL) return false;
        for (size_t index = 0; index < instantiation->argument_count; ++index) {
            arguments[index] = sol_ir_type(lowerer,
                lowerer->types->call_instantiation_arguments[
                    instantiation->argument_offset + index
                ]);
            if (arguments[index] == SOL_IR_NONE) {
                free(arguments);
                return false;
            }
        }
        output->as.call.type_arguments.offset = lowerer->ir->type_id_count;
        for (size_t index = 0; index < instantiation->argument_count; ++index) {
            if (!sol_ir_append_type_id(lowerer, arguments[index])) {
                free(arguments);
                return false;
            }
            ++output->as.call.type_arguments.count;
        }
        free(arguments);
    } else {
        output->as.call.type_arguments.offset = lowerer->ir->type_id_count;
    }
    const SolEffectCallInstantiation *effect
        = sol_effect_call_instantiation(lowerer->effects, id);
    if (effect != NULL) {
        if (effect->row_offset > lowerer->effects->call_row_count
            || effect->row_count > lowerer->effects->call_row_count - effect->row_offset) {
            return false;
        }
        output->as.call.effects = sol_ir_effect_row(lowerer,
            lowerer->effects->call_rows + effect->row_offset, effect->row_count);
    } else if (output->as.call.callable != SOL_IR_NONE) {
        output->as.call.effects = lowerer->ir->callables[output->as.call.callable].effects;
    }
    if (lowerer->failed) {
        sol_diagnostics_add(lowerer->diagnostics, "SOL-INTERNAL-006",
            SOL_SEVERITY_ERROR, source->span,
            "call %zu produced invalid semantic metadata", id);
    }
    return !lowerer->failed;
}

static bool sol_ir_lower_expressions(SolIrLowerer *lowerer) {
    size_t count = lowerer->syntax->expression_count;
    if (count != 0) {
        lowerer->ir->expressions = sol_ir_allocate(count,
            sizeof(*lowerer->ir->expressions), true);
        if (lowerer->ir->expressions == NULL) return false;
    }
    lowerer->ir->expression_count = count;
    for (size_t id = 0; id < count; ++id) {
        const SolExpr *source = &lowerer->syntax->expressions[id];
        SolIrExpression *output = &lowerer->ir->expressions[id];
        output->span = source->span;
        output->type = SOL_IR_NONE;
        SolType frontend_type = lowerer->types->expressions[id];
        bool transient_head = frontend_type.kind == SOL_TYPE_UNKNOWN
            || frontend_type.kind == SOL_TYPE_VARIANT
            || frontend_type.kind == SOL_TYPE_CAPABILITY_OPERATION
            || frontend_type.kind == SOL_TYPE_TRAIT_METHOD;
        if (!transient_head) {
            output->type = sol_ir_type(lowerer, lowerer->types->expressions[id]);
        }
        output->capability_roots = sol_ir_provenance(
            lowerer, lowerer->types->expression_capability_origins[id]
        );
        output->operation_roots = sol_ir_provenance(
            lowerer, lowerer->types->expression_operation_origins[id]
        );
        switch (source->kind) {
            case SOL_EXPR_INTEGER: {
                output->kind = SOL_IR_EXPR_INTEGER;
                char *text = sol_ir_copy_span(lowerer->source, source->span);
                if (text == NULL) return false;
                errno = 0;
                char *end = NULL;
                long long value = strtoll(text, &end, 10);
                bool valid = errno == 0 && end != text && *end == '\0';
                free(text);
                if (!valid) return false;
                output->as.integer = (int64_t)value;
                break;
            }
            case SOL_EXPR_STRING:
                output->kind = SOL_IR_EXPR_STRING;
                if (!sol_ir_decode_string(lowerer, source->span, &output->as.string)) return false;
                break;
            case SOL_EXPR_BOOL:
                output->kind = SOL_IR_EXPR_BOOL;
                output->as.boolean = source->as.bool_value;
                break;
            case SOL_EXPR_UNIT: output->kind = SOL_IR_EXPR_UNIT; break;
            case SOL_EXPR_PATH: {
                SolResolution resolution = lowerer->hir->resolutions[id];
                if (resolution.kind == SOL_RESOLUTION_LOCAL) {
                    if (resolution.target >= lowerer->ir->local_count) return false;
                    output->kind = SOL_IR_EXPR_LOCAL;
                    output->as.local = resolution.target;
                } else if (resolution.kind == SOL_RESOLUTION_REFINEMENT_SELF) {
                    output->kind = SOL_IR_EXPR_REFINEMENT_SELF;
                    output->as.definition = resolution.target;
                } else if (resolution.kind == SOL_RESOLUTION_DEFINITION) {
                    output->kind = SOL_IR_EXPR_DEFINITION;
                    output->as.definition = resolution.target;
                } else if (resolution.kind == SOL_RESOLUTION_BUILTIN
                    && sol_ir_builtin_head_is_callee(lowerer, id)) {
                    output->kind = SOL_IR_EXPR_COMPILE_TIME_HEAD;
                } else {
                    sol_diagnostics_add(lowerer->diagnostics, "SOL-INTERNAL-006",
                        SOL_SEVERITY_ERROR, source->span,
                        "unsupported path expression %zu resolution %d",
                        id, (int)resolution.kind);
                    return false;
                }
                break;
            }
            case SOL_EXPR_UNARY:
                output->kind = SOL_IR_EXPR_UNARY;
                output->as.unary.operator_kind = source->as.unary.operator_kind;
                output->as.unary.operand = source->as.unary.operand;
                break;
            case SOL_EXPR_BINARY:
                output->kind = SOL_IR_EXPR_BINARY;
                output->as.binary.left = source->as.binary.left;
                output->as.binary.operator_kind = source->as.binary.operator_kind;
                output->as.binary.right = source->as.binary.right;
                break;
            case SOL_EXPR_CALL:
                output->kind = SOL_IR_EXPR_CALL;
                if (!sol_ir_lower_call(lowerer, id, source, output)) {
                    sol_diagnostics_add(lowerer->diagnostics, "SOL-INTERNAL-006",
                        SOL_SEVERITY_ERROR, source->span,
                        "failed to lower call expression %zu", id);
                    return false;
                }
                break;
            case SOL_EXPR_TYPE_APPLICATION:
                output->kind = SOL_IR_EXPR_COMPILE_TIME_HEAD;
                if (!sol_ir_compile_time_head_used(lowerer, id)
                    || sol_ir_callee_resolution(lowerer, id).kind == SOL_RESOLUTION_BUILTIN) {
                    return false;
                }
                break;
            case SOL_EXPR_FIELD:
                if (lowerer->types->field_resolutions[id] != SOL_AST_NONE) {
                    SolFieldId field = lowerer->types->field_resolutions[id];
                    if (field >= lowerer->syntax->field_count) return false;
                    output->kind = SOL_IR_EXPR_FIELD;
                    output->as.field.base = source->as.field.base;
                    output->as.field.field = field;
                } else if (lowerer->types->variant_resolutions[id] != SOL_AST_NONE
                    && lowerer->types->variant_resolutions[id]
                        < lowerer->syntax->variant_count
                    && lowerer->syntax->variants[
                        lowerer->types->variant_resolutions[id]
                    ].first_field == SOL_AST_NONE) {
                    output->kind = SOL_IR_EXPR_VARIANT;
                    output->as.variant.variant = lowerer->types->variant_resolutions[id];
                } else {
                    output->kind = SOL_IR_EXPR_COMPILE_TIME_HEAD;
                    if (!sol_ir_compile_time_head_used(lowerer, id)) return false;
                }
                break;
            case SOL_EXPR_RECORD: {
                SolType type = lowerer->types->expressions[id];
                SolIrTypeId ir_type = sol_ir_type(lowerer, type);
                if (ir_type == SOL_IR_NONE) return false;
                output->kind = SOL_IR_EXPR_RECORD;
                output->as.record.definition = lowerer->ir->types[ir_type].definition;
                output->as.record.fields
                    = output->as.record.definition < lowerer->syntax->item_count
                        && lowerer->syntax->items[output->as.record.definition].kind
                            == SOL_ITEM_CAPABILITY
                    ? sol_ir_positional_arguments(lowerer, source->as.record.first_field)
                    : sol_ir_field_arguments(lowerer, source->as.record.first_field);
                break;
            }
            case SOL_EXPR_IF:
                output->kind = SOL_IR_EXPR_IF;
                output->as.if_expr.condition = source->as.if_expr.condition;
                output->as.if_expr.then_branch = source->as.if_expr.then_branch;
                output->as.if_expr.else_branch = source->as.if_expr.else_branch;
                break;
            case SOL_EXPR_MATCH: {
                output->kind = SOL_IR_EXPR_MATCH;
                output->as.match_expr.scrutinee = source->as.match_expr.scrutinee;
                output->as.match_expr.arms.offset = lowerer->ir->arm_id_count;
                SolMatchArmId arm = source->as.match_expr.first_arm;
                while (arm != SOL_AST_NONE) {
                    if (arm >= lowerer->syntax->match_arm_count
                        || output->as.match_expr.arms.count++
                            >= lowerer->syntax->match_arm_count
                        || !sol_ir_append_arm_id(lowerer, arm)) return false;
                    arm = lowerer->syntax->match_arms[arm].next;
                }
                break;
            }
            case SOL_EXPR_BLOCK: {
                output->kind = SOL_IR_EXPR_BLOCK;
                output->as.block.offset = lowerer->ir->statement_id_count;
                SolStatementId statement = source->as.block.first_statement;
                while (statement != SOL_AST_NONE) {
                    if (statement >= lowerer->syntax->statement_count
                        || output->as.block.count++ >= lowerer->syntax->statement_count
                        || !sol_ir_append_statement_id(lowerer, statement)) {
                        return false;
                    }
                    statement = lowerer->syntax->statements[statement].next;
                }
                break;
            }
            case SOL_EXPR_PROPAGATE: {
                output->kind = SOL_IR_EXPR_PROPAGATE;
                output->as.propagate.operand = source->as.propagated;
                SolType operand = lowerer->types->expressions[source->as.propagated];
                const SolTypeApplication *application
                    = sol_type_application(lowerer->types, operand);
                const SolType *arguments = NULL;
                size_t argument_count = 0;
                if (application == NULL
                    || (application->constructor != SOL_TYPE_CONSTRUCTOR_OPTION
                        && application->constructor != SOL_TYPE_CONSTRUCTOR_RESULT)
                    || !sol_type_application_arguments(lowerer->types, operand,
                        &arguments, &argument_count)) return false;
                output->as.propagate.kind
                    = application->constructor == SOL_TYPE_CONSTRUCTOR_OPTION
                    ? SOL_IR_PROPAGATE_OPTION : SOL_IR_PROPAGATE_RESULT;
                output->as.propagate.residual = application->constructor
                        == SOL_TYPE_CONSTRUCTOR_RESULT
                    ? sol_ir_type(lowerer, arguments[1]) : SOL_IR_NONE;
                break;
            }
            case SOL_EXPR_HANDLE: {
                output->kind = SOL_IR_EXPR_HANDLE;
                const SolHandler *handler = &lowerer->types->handlers[id];
                if (handler->source_member >= lowerer->syntax->capability_member_count
                    || handler->provider_member >= lowerer->syntax->capability_member_count
                    || handler->root >= lowerer->syntax->parameter_count) return false;
                output->as.handler.effect_name = sol_ir_copy_span(
                    lowerer->source, source->as.handle.effect_name
                );
                output->as.handler.authority = source->as.handle.authority;
                output->as.handler.provider = source->as.handle.provider;
                output->as.handler.body = source->as.handle.body;
                output->as.handler.source = lowerer->member_callables[handler->source_member];
                output->as.handler.provider_callable
                    = lowerer->member_callables[handler->provider_member];
                output->as.handler.root = lowerer->parameter_locals[handler->root];
                if (output->as.handler.effect_name == NULL
                    || output->as.handler.root == SOL_IR_NONE) return false;
                break;
            }
            case SOL_EXPR_RESULT: output->kind = SOL_IR_EXPR_RESULT; break;
            case SOL_EXPR_OLD: {
                SolSnapshotId snapshot = lowerer->contracts->expression_snapshots[id];
                if (snapshot >= lowerer->contracts->snapshot_count) return false;
                output->kind = SOL_IR_EXPR_SNAPSHOT_READ;
                output->as.snapshot = snapshot;
                break;
            }
            case SOL_EXPR_ERROR: return false;
        }
        if (output->kind != SOL_IR_EXPR_COMPILE_TIME_HEAD && output->type == SOL_IR_NONE) {
            sol_diagnostics_add(lowerer->diagnostics, "SOL-INTERNAL-006",
                SOL_SEVERITY_ERROR, source->span,
                "expression %zu has no exact IR type", id);
            return false;
        }
    }
    return !lowerer->failed;
}

static bool sol_ir_lower_statements_arms(SolIrLowerer *lowerer) {
    size_t statement_count = lowerer->syntax->statement_count;
    size_t arm_count = lowerer->syntax->match_arm_count;
    if (statement_count != 0) {
        lowerer->ir->statements = sol_ir_allocate(statement_count,
            sizeof(*lowerer->ir->statements), true);
        if (lowerer->ir->statements == NULL) return false;
    }
    if (arm_count != 0) {
        lowerer->ir->arms = sol_ir_allocate(arm_count, sizeof(*lowerer->ir->arms), true);
        if (lowerer->ir->arms == NULL) return false;
    }
    lowerer->ir->statement_count = statement_count;
    lowerer->ir->arm_count = arm_count;
    for (size_t index = 0; index < statement_count; ++index) {
        const SolStatement *source = &lowerer->syntax->statements[index];
        SolIrStatement *output = &lowerer->ir->statements[index];
        output->span = source->span;
        output->local = SOL_IR_NONE;
        if (source->kind == SOL_STATEMENT_LET) {
            output->kind = SOL_IR_STATEMENT_LET;
            output->expression = source->as.let_statement.value;
            output->local = lowerer->binding_locals[index];
            if (output->local == SOL_IR_NONE) return false;
        } else {
            output->kind = source->kind == SOL_STATEMENT_RETURN
                ? SOL_IR_STATEMENT_RETURN : SOL_IR_STATEMENT_EXPRESSION;
            output->expression = source->as.expression;
        }
    }
    for (size_t index = 0; index < arm_count; ++index) {
        const SolMatchArm *source = &lowerer->syntax->match_arms[index];
        const SolPattern *pattern = &lowerer->syntax->patterns[source->pattern];
        SolIrArm *output = &lowerer->ir->arms[index];
        output->span = source->span;
        output->value = source->value;
        output->variant = SOL_IR_NONE;
        output->bindings.offset = lowerer->ir->root_count;
        if (pattern->kind == SOL_PATTERN_WILDCARD) {
            output->kind = SOL_IR_PATTERN_WILDCARD;
        } else if (pattern->kind == SOL_PATTERN_BOOL) {
            output->kind = SOL_IR_PATTERN_BOOL;
            output->boolean = pattern->bool_value;
        } else {
            output->kind = SOL_IR_PATTERN_VARIANT;
            output->variant = lowerer->types->pattern_variant_resolutions[source->pattern];
            if (output->variant >= lowerer->syntax->variant_count) return false;
            SolPatternBindingId binding = pattern->first_binding;
            while (binding != SOL_AST_NONE) {
                if (binding >= lowerer->syntax->pattern_binding_count
                    || lowerer->pattern_locals[binding] == SOL_IR_NONE
                    || !sol_ir_append_root(lowerer, lowerer->pattern_locals[binding])) {
                    return false;
                }
                ++output->bindings.count;
                binding = lowerer->syntax->pattern_bindings[binding].next;
            }
            if (output->bindings.count
                != lowerer->ir->variants[output->variant].fields.count) return false;
        }
    }
    return true;
}

static bool sol_ir_lower_contracts(SolIrLowerer *lowerer) {
    size_t obligation_count = lowerer->contracts->obligation_count;
    size_t snapshot_count = lowerer->contracts->snapshot_count;
    if (obligation_count != 0) {
        lowerer->ir->obligations = sol_ir_allocate(obligation_count,
            sizeof(*lowerer->ir->obligations), true);
        if (lowerer->ir->obligations == NULL) return false;
    }
    if (snapshot_count != 0) {
        lowerer->ir->snapshots = sol_ir_allocate(snapshot_count,
            sizeof(*lowerer->ir->snapshots), true);
        if (lowerer->ir->snapshots == NULL) return false;
    }
    lowerer->ir->obligation_count = obligation_count;
    lowerer->ir->snapshot_count = snapshot_count;
    for (size_t index = 0; index < obligation_count; ++index) {
        const SolObligation *source = &lowerer->contracts->obligations[index];
        SolIrObligation *output = &lowerer->ir->obligations[index];
        if (source->id != (SolObligationId)index
            || source->predicate >= lowerer->syntax->expression_count
            || source->first_snapshot > snapshot_count
            || source->snapshot_count > snapshot_count - source->first_snapshot) return false;
        output->id = source->id;
        output->owner_kind = source->owner_kind;
        output->owner = source->owner;
        if (source->owner_kind == SOL_CONTRACT_OWNER_CAPABILITY_MEMBER) {
            if (source->owner >= lowerer->syntax->capability_member_count) return false;
            output->owner = lowerer->member_callables[source->owner];
        } else if (source->owner_kind != SOL_CONTRACT_OWNER_ITEM
            && source->owner_kind != SOL_CONTRACT_OWNER_TYPE) {
            return false;
        }
        output->kind = source->kind;
        output->outcome = source->outcome;
        output->predicate = source->predicate;
        output->result_available = source->result.available;
        output->result_type = source->result.available
            ? sol_ir_type(lowerer, source->result.type) : SOL_IR_NONE;
        output->snapshots = (SolIrSlice){
            .offset = source->first_snapshot,
            .count = source->snapshot_count,
        };
    }
    for (size_t index = 0; index < snapshot_count; ++index) {
        const SolSnapshot *source = &lowerer->contracts->snapshots[index];
        SolIrSnapshot *output = &lowerer->ir->snapshots[index];
        if (source->id != index || source->old_expression >= lowerer->syntax->expression_count
            || source->operand >= lowerer->syntax->expression_count
            || source->obligation >= obligation_count) return false;
        output->id = source->id;
        output->obligation = source->obligation;
        output->read = source->old_expression;
        output->operand = source->operand;
        output->type = sol_ir_type(lowerer, source->type);
        if (output->type == SOL_IR_NONE) return false;
    }
    return !lowerer->failed;
}

static bool sol_ir_lower_files(SolIrLowerer *lowerer) {
    size_t count = lowerer->file_count == 0 ? 1 : lowerer->file_count;
    lowerer->ir->files = sol_ir_allocate(count, sizeof(*lowerer->ir->files), true);
    if (lowerer->ir->files == NULL) return false;
    lowerer->ir->file_count = count;
    if (lowerer->file_count == 0) {
        const char *path = lowerer->source->path == NULL ? "<unknown>" : lowerer->source->path;
        lowerer->ir->files[0].path = sol_ir_copy_text(path, strlen(path));
        lowerer->ir->files[0].aggregate_end = lowerer->source->length;
        return lowerer->ir->files[0].path != NULL;
    }
    size_t previous = 0;
    for (size_t index = 0; index < count; ++index) {
        const SolPackageFile *source = &lowerer->files[index];
        if (source->path == NULL || source->aggregate_start < previous
            || source->aggregate_start > source->aggregate_end
            || source->aggregate_end > lowerer->source->length) return false;
        lowerer->ir->files[index].path = sol_ir_copy_text(source->path, strlen(source->path));
        lowerer->ir->files[index].aggregate_start = source->aggregate_start;
        lowerer->ir->files[index].aggregate_end = source->aggregate_end;
        if (lowerer->ir->files[index].path == NULL) return false;
        previous = source->aggregate_end;
    }
    return true;
}

static bool sol_ir_slice_valid(SolIrSlice slice, size_t count) {
    return slice.offset <= count && slice.count <= count - slice.offset;
}

bool sol_ir_validate(const SolIr *ir, SolDiagnostics *diagnostics) {
    if (ir == NULL || ir->source_bytes == NULL || ir->source_path == NULL
        || (ir->type_count != 0 && ir->types == NULL)
        || (ir->type_id_count != 0 && ir->type_ids == NULL)
        || (ir->definition_count != 0 && ir->definitions == NULL)
        || (ir->generic_parameter_count != 0 && ir->generic_parameters == NULL)
        || (ir->effect_parameter_count != 0 && ir->effect_parameters == NULL)
        || (ir->callable_count != 0 && ir->callables == NULL)
        || (ir->member_count != 0 && ir->members == NULL)
        || (ir->evidence_count != 0 && ir->evidence == NULL)
        || (ir->local_count != 0 && ir->locals == NULL)
        || (ir->field_count != 0 && ir->fields == NULL)
        || (ir->variant_count != 0 && ir->variants == NULL)
        || (ir->expression_count != 0 && ir->expressions == NULL)
        || (ir->statement_count != 0 && ir->statements == NULL)
        || (ir->statement_id_count != 0 && ir->statement_ids == NULL)
        || (ir->arm_count != 0 && ir->arms == NULL)
        || (ir->arm_id_count != 0 && ir->arm_ids == NULL)
        || (ir->operand_count != 0 && ir->operands == NULL)
        || (ir->root_count != 0 && ir->roots == NULL)
        || (ir->effect_count != 0 && ir->effects == NULL)
        || (ir->obligation_count != 0 && ir->obligations == NULL)
        || (ir->snapshot_count != 0 && ir->snapshots == NULL)) {
        return sol_ir_error(diagnostics, "malformed canonical IR ownership or counts");
    }
    for (size_t index = 0; index < ir->type_count; ++index) {
        const SolIrType *type = &ir->types[index];
        if (type->kind > SOL_IR_TYPE_SELF
            || !sol_ir_slice_valid((SolIrSlice){type->argument_offset,
                type->argument_count}, ir->type_id_count)
            || !sol_ir_slice_valid((SolIrSlice){type->parameter_offset,
                type->parameter_count}, ir->type_id_count)
            || ((type->kind == SOL_IR_TYPE_NOMINAL || (type->kind == SOL_IR_TYPE_FUNCTION
                    && type->definition != SOL_IR_NONE))
                && type->definition >= ir->definition_count)
            || (type->result != SOL_IR_NONE && type->result >= ir->type_count)
            || !sol_ir_slice_valid(type->effects, ir->effect_count)) {
            return sol_ir_error(diagnostics, "malformed canonical IR type");
        }
        for (size_t argument = 0; argument < type->argument_count; ++argument) {
            if (ir->type_ids[type->argument_offset + argument] >= ir->type_count) {
                return sol_ir_error(diagnostics, "IR type argument is out of range");
            }
        }
        for (size_t parameter = 0; parameter < type->parameter_count; ++parameter) {
            if (ir->type_ids[type->parameter_offset + parameter] >= ir->type_count) {
                return sol_ir_error(diagnostics, "IR function parameter type is out of range");
            }
        }
        if (type->kind == SOL_IR_TYPE_PARAMETER
            && type->definition >= ir->generic_parameter_count) {
            return sol_ir_error(diagnostics, "IR type parameter is out of range");
        }
    }
    for (size_t index = 0; index < ir->definition_count; ++index) {
        const SolIrDefinition *definition = &ir->definitions[index];
        if (definition->name == NULL || definition->kind > SOL_IR_DEFINITION_IMPLEMENTATION
            || (definition->declared_type != SOL_IR_NONE
                && definition->declared_type >= ir->type_count)
            || (definition->callable != SOL_IR_NONE
                && definition->callable >= ir->callable_count)
            || (definition->representation != SOL_IR_NONE
                && definition->representation >= ir->type_count)
            || (definition->kind == SOL_IR_DEFINITION_IMPLEMENTATION
                && (definition->implementation_trait >= ir->definition_count
                    || definition->implementation_target >= ir->type_count))
            || (definition->kind != SOL_IR_DEFINITION_IMPLEMENTATION
                && (definition->implementation_trait != SOL_IR_NONE
                    || definition->implementation_target != SOL_IR_NONE))
            || !sol_ir_slice_valid(definition->fields, ir->field_count)
            || !sol_ir_slice_valid(definition->variants, ir->variant_count)
            || !sol_ir_slice_valid(definition->members, ir->member_count)
            || !sol_ir_slice_valid(definition->generic_parameters,
                ir->generic_parameter_count)
            || !sol_ir_slice_valid(definition->effect_parameters,
                ir->effect_parameter_count)) {
            return sol_ir_error(diagnostics, "malformed canonical IR declaration");
        }
        for (size_t member = 0; member < definition->members.count; ++member) {
            SolIrCallableId callable
                = ir->members[definition->members.offset + member].callable;
            if (callable >= ir->callable_count || ir->callables[callable].owner != index) {
                return sol_ir_error(diagnostics, "IR member has wrong callable owner");
            }
        }
        for (size_t field = 0; field < definition->fields.count; ++field) {
            if (ir->fields[definition->fields.offset + field].owner != index) {
                return sol_ir_error(diagnostics, "IR record field has the wrong owner");
            }
        }
        for (size_t parameter = 0; parameter < definition->generic_parameters.count;
            ++parameter) {
            size_t id = definition->generic_parameters.offset + parameter;
            if (ir->generic_parameters[id].owner != index
                || ir->generic_parameters[id].ordinal != parameter) {
                return sol_ir_error(diagnostics,
                    "IR definition generic parameter slice is malformed");
            }
        }
        for (size_t parameter = 0; parameter < definition->effect_parameters.count;
            ++parameter) {
            size_t id = definition->effect_parameters.offset + parameter;
            if (ir->effect_parameters[id].owner != index
                || ir->effect_parameters[id].ordinal != parameter) {
                return sol_ir_error(diagnostics,
                    "IR definition effect parameter slice is malformed");
            }
        }
    }
    for (size_t index = 0; index < ir->evidence_count; ++index) {
        const SolIrDispatchEvidence *evidence = &ir->evidence[index];
        if (evidence->trait >= ir->definition_count
            || evidence->requirement >= ir->callable_count
            || evidence->implementation >= ir->definition_count
            || evidence->method >= ir->callable_count || evidence->type >= ir->type_count
            || ir->definitions[evidence->trait].kind != SOL_IR_DEFINITION_TRAIT
            || ir->definitions[evidence->implementation].kind
                != SOL_IR_DEFINITION_IMPLEMENTATION
            || ir->definitions[evidence->implementation].implementation_trait
                != evidence->trait
            || ir->definitions[evidence->implementation].implementation_target
                != evidence->type
            || ir->callables[evidence->requirement].kind
                != SOL_IR_CALLABLE_TRAIT_REQUIREMENT
            || ir->callables[evidence->method].kind
                != SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION
            || ir->callables[evidence->method].owner != evidence->implementation) {
            return sol_ir_error(diagnostics, "malformed IR dispatch evidence");
        }
    }
    for (size_t index = 0; index < ir->callable_count; ++index) {
        const SolIrCallable *callable = &ir->callables[index];
        if (callable->name == NULL || callable->kind > SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION
            || callable->owner >= ir->definition_count || callable->result >= ir->type_count
            || (callable->body != SOL_IR_NONE && callable->body >= ir->expression_count)
            || !sol_ir_slice_valid(callable->parameters, ir->root_count)
            || !sol_ir_slice_valid(callable->effects, ir->effect_count)
            || !sol_ir_slice_valid(callable->generic_parameters,
                ir->generic_parameter_count)
            || !sol_ir_slice_valid(callable->effect_parameters,
                ir->effect_parameter_count)
            || callable->generic_parameters.offset
                != ir->definitions[callable->owner].generic_parameters.offset
            || callable->generic_parameters.count
                != ir->definitions[callable->owner].generic_parameters.count
            || callable->effect_parameters.offset
                != ir->definitions[callable->owner].effect_parameters.offset
            || callable->effect_parameters.count
                != ir->definitions[callable->owner].effect_parameters.count) {
            return sol_ir_error(diagnostics, "malformed canonical IR callable");
        }
    }
    for (size_t index = 0; index < ir->generic_parameter_count; ++index) {
        const SolIrGenericParameter *parameter = &ir->generic_parameters[index];
        if (parameter->owner >= ir->definition_count || parameter->name == NULL
            || (parameter->trait_bound != SOL_IR_NONE
                && (parameter->trait_bound >= ir->definition_count
                    || ir->definitions[parameter->trait_bound].kind
                        != SOL_IR_DEFINITION_TRAIT))) {
            return sol_ir_error(diagnostics, "malformed IR generic parameter");
        }
        SolIrSlice parameters = ir->definitions[parameter->owner].generic_parameters;
        if (parameter->ordinal >= parameters.count
            || parameters.offset + parameter->ordinal != index) {
            return sol_ir_error(diagnostics, "IR generic parameter ordinal is malformed");
        }
    }
    for (size_t index = 0; index < ir->effect_parameter_count; ++index) {
        const SolIrEffectParameter *parameter = &ir->effect_parameters[index];
        SolIrSlice parameters = parameter->owner < ir->definition_count
            ? ir->definitions[parameter->owner].effect_parameters : (SolIrSlice){0};
        if (parameter->owner >= ir->definition_count || parameter->name == NULL
            || parameter->ordinal >= parameters.count
            || parameters.offset + parameter->ordinal != index) {
            return sol_ir_error(diagnostics, "malformed IR effect parameter");
        }
    }
    for (size_t index = 0; index < ir->effect_count; ++index) {
        const SolIrEffect *effect = &ir->effects[index];
        if (effect->name == NULL || effect->authority_kind > SOL_IR_AUTHORITY_SELF
            || (effect->authority_kind == SOL_IR_AUTHORITY_LOCAL
                && effect->authority >= ir->local_count)
            || (effect->authority_kind != SOL_IR_AUTHORITY_LOCAL
                && effect->authority != SOL_IR_NONE)) {
            return sol_ir_error(diagnostics, "malformed canonical IR effect");
        }
    }
    const SolIrSlice *effect_slices = NULL;
    size_t effect_slice_count = 0;
    for (size_t type = 0; type < ir->type_count; ++type) {
        effect_slices = &ir->types[type].effects;
        effect_slice_count = 1;
        for (size_t slice = 0; slice < effect_slice_count; ++slice) {
            SolIrSlice row = effect_slices[slice];
            for (size_t atom = 1; atom < row.count; ++atom) {
                if (sol_ir_effect_compare(&ir->effects[row.offset + atom - 1],
                    &ir->effects[row.offset + atom]) >= 0) {
                    return sol_ir_error(diagnostics, "IR effect row is not canonical");
                }
            }
        }
    }
    for (size_t callable = 0; callable < ir->callable_count; ++callable) {
        SolIrSlice row = ir->callables[callable].effects;
        for (size_t atom = 1; atom < row.count; ++atom) {
            if (sol_ir_effect_compare(&ir->effects[row.offset + atom - 1],
                &ir->effects[row.offset + atom]) >= 0) {
                return sol_ir_error(diagnostics, "IR callable effect row is not canonical");
            }
        }
    }
    for (size_t index = 0; index < ir->expression_count; ++index) {
        const SolIrExpression *expression = &ir->expressions[index];
        if (expression->kind > SOL_IR_EXPR_COMPILE_TIME_HEAD
            || expression->span.start > expression->span.end
            || expression->span.end > ir->source_length
            || (expression->kind != SOL_IR_EXPR_COMPILE_TIME_HEAD
                && expression->type >= ir->type_count)
            || !sol_ir_slice_valid(expression->capability_roots, ir->root_count)
            || !sol_ir_slice_valid(expression->operation_roots, ir->root_count)) {
            return sol_ir_error(diagnostics, "malformed canonical IR expression");
        }
        if (expression->kind == SOL_IR_EXPR_CALL) {
            const SolIrSlice operands = expression->as.call.operands;
            if (expression->as.call.kind > SOL_IR_CALL_DISTINCT_CONSTRUCTOR
                || !sol_ir_slice_valid(operands, ir->operand_count)
                || !sol_ir_slice_valid(expression->as.call.type_arguments,
                    ir->type_id_count)
                || !sol_ir_slice_valid(expression->as.call.effects, ir->effect_count)) {
                return sol_ir_error(diagnostics, "malformed canonical IR call");
            }
            if (!sol_ir_slice_valid(expression->as.call.evidence, ir->evidence_count)) {
                return sol_ir_error(diagnostics, "IR call evidence is out of range");
            }
            for (size_t argument = 0;
                argument < expression->as.call.type_arguments.count;
                ++argument) {
                if (ir->type_ids[expression->as.call.type_arguments.offset + argument]
                    >= ir->type_count) {
                    return sol_ir_error(diagnostics, "IR call type argument is out of range");
                }
            }
            if (expression->as.call.kind == SOL_IR_CALL_METHOD
                && expression->as.call.evidence.count == 0) {
                return sol_ir_error(diagnostics, "IR method call has no dispatch evidence");
            }
            bool callable_call = expression->as.call.kind == SOL_IR_CALL_FUNCTION
                || expression->as.call.kind == SOL_IR_CALL_CAPABILITY
                || expression->as.call.kind == SOL_IR_CALL_METHOD;
            if ((callable_call && expression->as.call.callable >= ir->callable_count)
                || (!callable_call && expression->as.call.callable != SOL_IR_NONE)
                || (expression->as.call.kind == SOL_IR_CALL_METHOD
                    && expression->as.call.receiver >= ir->expression_count)
                || (expression->as.call.kind == SOL_IR_CALL_CAPABILITY
                    && expression->as.call.receiver >= ir->expression_count)
                || (expression->as.call.kind == SOL_IR_CALL_ENUM_CONSTRUCTOR
                    && expression->as.call.variant >= ir->variant_count)
                || (expression->as.call.kind == SOL_IR_CALL_DISTINCT_CONSTRUCTOR
                    && expression->as.call.definition >= ir->definition_count)) {
                return sol_ir_error(diagnostics, "IR call target domain is malformed");
            }
            if (expression->as.call.kind == SOL_IR_CALL_CALLBACK
                && (expression->as.call.callee >= ir->expression_count
                    || ir->expressions[expression->as.call.callee].type >= ir->type_count
                    || ir->types[ir->expressions[expression->as.call.callee].type].kind
                        != SOL_IR_TYPE_FUNCTION)) {
                return sol_ir_error(diagnostics, "IR callback callee is malformed");
            }
            size_t formal_count = 0;
            if (callable_call) {
                formal_count = ir->callables[expression->as.call.callable].parameters.count;
            } else if (expression->as.call.kind == SOL_IR_CALL_CALLBACK) {
                const SolIrType *function
                    = &ir->types[ir->expressions[expression->as.call.callee].type];
                formal_count = function->parameter_count;
            } else if (expression->as.call.kind == SOL_IR_CALL_BUILTIN_OK
                || expression->as.call.kind == SOL_IR_CALL_BUILTIN_ERR
                || expression->as.call.kind == SOL_IR_CALL_BUILTIN_SOME
                || expression->as.call.kind == SOL_IR_CALL_DISTINCT_CONSTRUCTOR) {
                formal_count = 1;
            } else if (expression->as.call.kind == SOL_IR_CALL_ENUM_CONSTRUCTOR) {
                formal_count = ir->variants[expression->as.call.variant].fields.count;
            }
            if (operands.count != formal_count) {
                return sol_ir_error(diagnostics, "IR call operand set is incomplete");
            }
            for (size_t operand = 0; operand < operands.count; ++operand) {
                size_t formal = ir->operands[operands.offset + operand].formal;
                bool enum_field = expression->as.call.kind == SOL_IR_CALL_ENUM_CONSTRUCTOR;
                SolIrSlice fields = enum_field
                    ? ir->variants[expression->as.call.variant].fields : (SolIrSlice){0};
                size_t expected = enum_field ? fields.offset + operand : operand;
                if (formal != expected) {
                    return sol_ir_error(diagnostics, "IR operand formal is out of range");
                }
                const SolIrOperand *entry = &ir->operands[operands.offset + operand];
                if (entry->value >= ir->expression_count) {
                    return sol_ir_error(diagnostics, "IR call operands are not canonical");
                }
            }
        } else if (expression->kind == SOL_IR_EXPR_VARIANT
            && (expression->as.variant.variant >= ir->variant_count
                || ir->variants[expression->as.variant.variant].fields.count != 0)) {
            return sol_ir_error(diagnostics, "malformed payloadless IR variant");
        } else if (expression->kind == SOL_IR_EXPR_FIELD
            && (expression->as.field.base >= ir->expression_count
                || expression->as.field.field >= ir->field_count)) {
            return sol_ir_error(diagnostics, "malformed canonical IR field projection");
        } else if (expression->kind == SOL_IR_EXPR_PROPAGATE
            && (expression->as.propagate.kind > SOL_IR_PROPAGATE_RESULT
                || expression->as.propagate.operand >= ir->expression_count
                || (expression->as.propagate.kind == SOL_IR_PROPAGATE_RESULT
                    && expression->as.propagate.residual >= ir->type_count))) {
            return sol_ir_error(diagnostics, "malformed canonical IR propagation");
        } else if (expression->kind == SOL_IR_EXPR_BLOCK
            && !sol_ir_slice_valid(expression->as.block, ir->statement_id_count)) {
            return sol_ir_error(diagnostics, "IR block statement slice is malformed");
        } else if (expression->kind == SOL_IR_EXPR_MATCH
            && (!sol_ir_slice_valid(expression->as.match_expr.arms, ir->arm_id_count)
                || expression->as.match_expr.scrutinee >= ir->expression_count)) {
            return sol_ir_error(diagnostics, "IR match arm slice is malformed");
        } else if (expression->kind == SOL_IR_EXPR_LOCAL
            && expression->as.local >= ir->local_count) {
            return sol_ir_error(diagnostics, "IR local expression is out of range");
        } else if ((expression->kind == SOL_IR_EXPR_DEFINITION
                || expression->kind == SOL_IR_EXPR_REFINEMENT_SELF)
            && expression->as.definition >= ir->definition_count) {
            return sol_ir_error(diagnostics, "IR definition expression is out of range");
        } else if (expression->kind == SOL_IR_EXPR_UNARY
            && expression->as.unary.operand >= ir->expression_count) {
            return sol_ir_error(diagnostics, "IR unary operand is out of range");
        } else if (expression->kind == SOL_IR_EXPR_BINARY
            && (expression->as.binary.left >= ir->expression_count
                || expression->as.binary.right >= ir->expression_count)) {
            return sol_ir_error(diagnostics, "IR binary operand is out of range");
        } else if (expression->kind == SOL_IR_EXPR_IF
            && (expression->as.if_expr.condition >= ir->expression_count
                || expression->as.if_expr.then_branch >= ir->expression_count
                || expression->as.if_expr.else_branch >= ir->expression_count)) {
            return sol_ir_error(diagnostics, "IR if child is out of range");
        } else if (expression->kind == SOL_IR_EXPR_RECORD) {
            if (expression->as.record.definition >= ir->definition_count
                || !sol_ir_slice_valid(expression->as.record.fields, ir->operand_count)) {
                return sol_ir_error(diagnostics, "IR record constructor is malformed");
            }
            const SolIrDefinition *definition
                = &ir->definitions[expression->as.record.definition];
            if (definition->kind == SOL_IR_DEFINITION_CAPABILITY) {
                SolIrSlice operands = expression->as.record.fields;
                if (operands.count != 1
                    || ir->operands[operands.offset].formal != 0
                    || ir->operands[operands.offset].value >= ir->expression_count) {
                    return sol_ir_error(diagnostics,
                        "IR capability construction source is malformed");
                }
            } else if (definition->kind == SOL_IR_DEFINITION_RECORD) {
                SolIrSlice operands = expression->as.record.fields;
                if (operands.count != definition->fields.count) {
                    return sol_ir_error(diagnostics, "IR record field set is incomplete");
                }
                for (size_t field = 0; field < operands.count; ++field) {
                    const SolIrOperand *operand = &ir->operands[operands.offset + field];
                    size_t expected = definition->fields.offset + field;
                    if (operand->value >= ir->expression_count
                        || operand->formal >= ir->field_count
                        || ir->fields[operand->formal].owner
                            != expression->as.record.definition
                        || operand->formal != expected) {
                        return sol_ir_error(diagnostics,
                            "IR record fields are not complete and canonical");
                    }
                }
            }
        } else if (expression->kind == SOL_IR_EXPR_HANDLE
            && (expression->as.handler.effect_name == NULL
                || expression->as.handler.authority >= ir->expression_count
                || expression->as.handler.provider >= ir->expression_count
                || expression->as.handler.body >= ir->expression_count
                || expression->as.handler.source >= ir->callable_count
                || expression->as.handler.provider_callable >= ir->callable_count
                || expression->as.handler.root >= ir->local_count)) {
            return sol_ir_error(diagnostics, "IR handler is malformed");
        } else if (expression->kind == SOL_IR_EXPR_SNAPSHOT_READ
            && expression->as.snapshot >= ir->snapshot_count) {
            return sol_ir_error(diagnostics, "IR snapshot read is out of range");
        }
        if (expression->kind == SOL_IR_EXPR_PROPAGATE) {
            const SolIrExpression *operand
                = &ir->expressions[expression->as.propagate.operand];
            if (operand->type >= ir->type_count) {
                return sol_ir_error(diagnostics, "IR propagation operand has no type");
            }
            const SolIrType *operand_type = &ir->types[operand->type];
            const SolIrType *result_type = &ir->types[expression->type];
            if (operand_type->argument_count == 0
                || ir->type_ids[operand_type->argument_offset] != expression->type
                || (expression->as.propagate.kind == SOL_IR_PROPAGATE_OPTION
                    && (operand_type->kind != SOL_IR_TYPE_OPTION
                        || expression->as.propagate.residual != SOL_IR_NONE))
                || (expression->as.propagate.kind == SOL_IR_PROPAGATE_RESULT
                    && (operand_type->kind != SOL_IR_TYPE_RESULT
                        || operand_type->argument_count != 2
                        || ir->type_ids[operand_type->argument_offset + 1]
                            != expression->as.propagate.residual))
                || result_type->kind == SOL_IR_TYPE_NEVER) {
                return sol_ir_error(diagnostics, "IR propagation type relation is invalid");
            }
        }
    }
    for (size_t index = 0; index < ir->statement_id_count; ++index) {
        if (ir->statement_ids[index] >= ir->statement_count) {
            return sol_ir_error(diagnostics, "IR statement ID is out of range");
        }
    }
    for (size_t index = 0; index < ir->statement_count; ++index) {
        const SolIrStatement *statement = &ir->statements[index];
        if (statement->kind > SOL_IR_STATEMENT_EXPRESSION
            || statement->span.start > statement->span.end
            || statement->span.end > ir->source_length
            || statement->expression >= ir->expression_count
            || (statement->kind == SOL_IR_STATEMENT_LET
                ? statement->local >= ir->local_count
                : statement->local != SOL_IR_NONE)) {
            return sol_ir_error(diagnostics, "malformed IR statement");
        }
    }
    for (size_t index = 0; index < ir->arm_id_count; ++index) {
        if (ir->arm_ids[index] >= ir->arm_count) {
            return sol_ir_error(diagnostics, "IR arm ID is out of range");
        }
    }
    for (size_t index = 0; index < ir->arm_count; ++index) {
        const SolIrArm *arm = &ir->arms[index];
        if (arm->kind > SOL_IR_PATTERN_VARIANT || arm->value >= ir->expression_count
            || arm->span.start > arm->span.end || arm->span.end > ir->source_length
            || !sol_ir_slice_valid(arm->bindings, ir->root_count)
            || (arm->kind == SOL_IR_PATTERN_VARIANT
                && arm->variant >= ir->variant_count)
            || (arm->kind != SOL_IR_PATTERN_VARIANT
                && (arm->variant != SOL_IR_NONE || arm->bindings.count != 0))) {
            return sol_ir_error(diagnostics, "malformed IR match arm");
        }
    }
    for (size_t index = 0; index < ir->local_count; ++index) {
        const SolIrLocal *local = &ir->locals[index];
        if (local->kind > SOL_IR_LOCAL_PATTERN || local->owner >= ir->definition_count
            || local->name == NULL || local->type >= ir->type_count
            || !sol_ir_slice_valid(local->capability_roots, ir->root_count)
            || !sol_ir_slice_valid(local->operation_roots, ir->root_count)) {
            return sol_ir_error(diagnostics, "malformed IR local");
        }
    }
    for (size_t index = 0; index < ir->root_count; ++index) {
        if (ir->roots[index] >= ir->local_count) {
            return sol_ir_error(diagnostics, "IR local root is out of range");
        }
    }
    for (size_t index = 0; index < ir->field_count; ++index) {
        if (ir->fields[index].name == NULL || ir->fields[index].owner >= ir->definition_count
            || ir->fields[index].type >= ir->type_count) {
            return sol_ir_error(diagnostics, "malformed canonical IR field");
        }
    }
    for (size_t index = 0; index < ir->variant_count; ++index) {
        if (ir->variants[index].name == NULL
            || ir->variants[index].owner >= ir->definition_count
            || !sol_ir_slice_valid(ir->variants[index].fields, ir->field_count)) {
            return sol_ir_error(diagnostics, "malformed canonical IR variant");
        }
        for (size_t field = 0; field < ir->variants[index].fields.count; ++field) {
            if (ir->fields[ir->variants[index].fields.offset + field].owner
                != ir->variants[index].owner) {
                return sol_ir_error(diagnostics, "IR variant field has the wrong owner");
            }
        }
    }
    for (size_t index = 0; index < ir->obligation_count; ++index) {
        const SolIrObligation *obligation = &ir->obligations[index];
        if (obligation->id != index || obligation->predicate >= ir->expression_count
            || !sol_ir_slice_valid(obligation->snapshots, ir->snapshot_count)
            || (obligation->result_available && obligation->result_type >= ir->type_count)
            || (obligation->owner_kind == SOL_CONTRACT_OWNER_CAPABILITY_MEMBER
                && obligation->owner >= ir->callable_count)
            || ((obligation->owner_kind == SOL_CONTRACT_OWNER_ITEM
                    || obligation->owner_kind == SOL_CONTRACT_OWNER_TYPE)
                && obligation->owner >= ir->definition_count)
            || obligation->owner_kind > SOL_CONTRACT_OWNER_TYPE) {
            return sol_ir_error(diagnostics, "malformed canonical IR obligation");
        }
    }
    for (size_t index = 0; index < ir->snapshot_count; ++index) {
        const SolIrSnapshot *snapshot = &ir->snapshots[index];
        if (snapshot->id != index || snapshot->obligation >= ir->obligation_count
            || snapshot->read >= ir->expression_count
            || snapshot->operand >= ir->expression_count || snapshot->type >= ir->type_count
            || ir->expressions[snapshot->read].kind != SOL_IR_EXPR_SNAPSHOT_READ
            || ir->expressions[snapshot->read].as.snapshot != index) {
            return sol_ir_error(diagnostics, "malformed canonical IR snapshot");
        }
    }
    if (ir->file_count == 0 || ir->files == NULL) {
        return sol_ir_error(diagnostics, "IR has no source-file map");
    }
    size_t previous_end = 0;
    for (size_t index = 0; index < ir->file_count; ++index) {
        const SolIrSourceFile *file = &ir->files[index];
        if (file->path == NULL || file->aggregate_start < previous_end
            || file->aggregate_start > file->aggregate_end
            || file->aggregate_end > ir->source_length) {
            return sol_ir_error(diagnostics, "malformed IR source-file map");
        }
        previous_end = file->aggregate_end;
    }
    return true;
}

bool sol_ir_lower_scoped(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolHirModule *hir,
    const SolTypeTable *types,
    const SolEffectTable *effects,
    const SolContractTable *contracts,
    const SolPackageFile *files,
    size_t file_count,
    SolIr *ir,
    SolDiagnostics *diagnostics
) {
    if (source == NULL || syntax == NULL || hir == NULL || types == NULL
        || effects == NULL || contracts == NULL || ir == NULL || diagnostics == NULL
        || (file_count != 0 && files == NULL)) {
        return sol_ir_error(diagnostics, "null compiler input passed to IR lowering");
    }
    if (!sol_ir_empty(ir)) return sol_ir_error(diagnostics, "IR lowering output must be empty");
    SolIr lowered;
    sol_ir_init(&lowered);
    SolIrLowerer lowerer = {
        .source = source,
        .syntax = syntax,
        .hir = hir,
        .types = types,
        .effects = effects,
        .contracts = contracts,
        .files = files,
        .file_count = file_count,
        .ir = &lowered,
        .diagnostics = diagnostics,
    };
    bool valid = sol_ir_frontend_shape_valid(&lowerer);
    if (valid) {
        const char *path = source->path == NULL ? "<unknown>" : source->path;
        lowered.source_path = sol_ir_copy_text(path, strlen(path));
        lowered.source_bytes = sol_ir_copy_text(source->text, source->length);
        lowered.source_length = source->length;
        valid = lowered.source_path != NULL && lowered.source_bytes != NULL;
    }
    if (valid && !sol_ir_allocate_maps(&lowerer)) valid = sol_ir_error(diagnostics, "IR map lowering failed");
    if (valid && !sol_ir_lower_generic_metadata(&lowerer)) valid = sol_ir_error(diagnostics, "IR generic metadata lowering failed");
    if (valid && !sol_ir_lower_locals(&lowerer)) valid = sol_ir_error(diagnostics, "IR local lowering failed");
    if (valid && !sol_ir_lower_fields_variants(&lowerer)) valid = sol_ir_error(diagnostics, "IR member lowering failed");
    if (valid && !sol_ir_lower_declarations(&lowerer)) valid = sol_ir_error(diagnostics, "IR declaration lowering failed");
    if (valid && !sol_ir_lower_expressions(&lowerer)) valid = sol_ir_error(diagnostics, "IR expression lowering failed");
    if (valid && !sol_ir_lower_statements_arms(&lowerer)) valid = sol_ir_error(diagnostics, "IR control-flow lowering failed");
    if (valid && !sol_ir_lower_contracts(&lowerer)) valid = sol_ir_error(diagnostics, "IR contract lowering failed");
    if (valid && !sol_ir_lower_files(&lowerer)) valid = sol_ir_error(diagnostics, "IR source-file lowering failed");
    if (valid && !lowerer.failed) valid = sol_ir_validate(&lowered, diagnostics);
    sol_ir_free_maps(&lowerer);
    if (!valid || lowerer.failed) {
        if (!sol_diagnostics_has_errors(diagnostics)) {
            sol_ir_error(diagnostics, "invalid frontend metadata or allocation failure in IR lowering");
        }
        sol_ir_free(&lowered);
        return false;
    }
    *ir = lowered;
    return true;
}

bool sol_ir_lower(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolHirModule *hir,
    const SolTypeTable *types,
    const SolEffectTable *effects,
    const SolContractTable *contracts,
    SolIr *ir,
    SolDiagnostics *diagnostics
) {
    return sol_ir_lower_scoped(source, syntax, hir, types, effects, contracts,
        NULL, 0, ir, diagnostics);
}
