#include "sol/ir.h"
#include "sol/ownership.h"

#include <errno.h>
#include <stdlib.h>
#include "resource_internal.h"
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
    unsigned char *pattern_states;
    size_t type_capacity;
    size_t type_id_capacity;
    size_t access_capacity;
    size_t callable_capacity;
    size_t member_capacity;
    size_t evidence_capacity;
    size_t statement_id_capacity;
    size_t arm_id_capacity;
    size_t pattern_capacity;
    size_t pattern_child_capacity;
    size_t operand_capacity;
    size_t place_capacity;
    size_t projection_capacity;
    size_t root_capacity;
    size_t cleanup_local_capacity;
    size_t effect_capacity;
    bool failed;
} SolIrLowerer;

static bool sol_ir_error(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics != NULL) {
        sol_diagnostics_add(diagnostics, "SOL-INTERNAL-006", SOL_SEVERITY_ERROR,
            (SolSpan){0}, "%s", message);
    }
    return false;
}

static bool sol_ir_indexed_error(SolDiagnostics *diagnostics, const char *message,
    size_t index, int kind) {
    char formatted[192];
    int written = snprintf(formatted, sizeof(formatted), "%s at index %zu (kind %d)",
        message, index, kind);
    return written > 0 && (size_t)written < sizeof(formatted)
        ? sol_ir_error(diagnostics, formatted) : sol_ir_error(diagnostics, message);
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
    for (size_t index = 0; index < ir->statement_count; ++index) {
        free(ir->statements[index].region_label);
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
    free(ir->accesses);
    free(ir->definitions);
    free(ir->callables);
    free(ir->members);
    free(ir->evidence);
    free(ir->locals);
    free(ir->fields);
    free(ir->variants);
    free(ir->expressions);
    free(ir->places);
    free(ir->projections);
    free(ir->statements);
    free(ir->statement_ids);
    free(ir->arms);
    free(ir->arm_ids);
    free(ir->patterns);
    free(ir->pattern_children);
    free(ir->operands);
    free(ir->roots);
    free(ir->cleanup_locals);
    free(ir->effects);
    free(ir->generic_parameters);
    free(ir->effect_parameters);
    free(ir->obligations);
    free(ir->snapshots);
    free(ir->loop_obligations);
    free(ir->unreachable_obligations);
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

static bool sol_ir_region_label_valid(const SolIr *ir,
    const SolIrStatement *statement) {
    SolSpan span = statement->region_label_span;
    if (statement->region_label == NULL || span.start >= span.end
        || span.end > ir->source_length
        || strlen(statement->region_label) != span.end - span.start
        || memcmp(statement->region_label, ir->source_bytes + span.start,
            span.end - span.start) != 0) return false;
    for (size_t index = 0; index < span.end - span.start; ++index) {
        unsigned char byte = (unsigned char)statement->region_label[index];
        bool letter = (byte >= (unsigned char)'a' && byte <= (unsigned char)'z')
            || (byte >= (unsigned char)'A' && byte <= (unsigned char)'Z')
            || byte == (unsigned char)'_';
        bool digit = byte >= (unsigned char)'0' && byte <= (unsigned char)'9';
        if (!letter && (index == 0 || !digit)) return false;
    }
    return true;
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
    if (!sol_resource_charge_arena(count)) return NULL;
    return zeroed ? calloc(count, size) : malloc(count * size);
}

static char *sol_ir_copy_span(const SolSource *source, SolSpan span) {
    if (!sol_ir_span_valid(source, span)) return NULL;
    return sol_ir_copy_text(source->text + span.start, span.end - span.start);
}

static bool sol_ir_grow(
    void **items,
    size_t *count,
    size_t *capacity,
    size_t additional,
    size_t size,
    void **first
) {
    if (additional == 0) {
        if (first != NULL) *first = NULL;
        return true;
    }
    if (size == 0 || *count > *capacity || *count > SIZE_MAX - additional
        || *count + additional > SIZE_MAX / size) return false;
    size_t old = *count;
    size_t next = old + additional;
    if (next > *capacity) {
        size_t grown_capacity = *capacity == 0 ? 8 : *capacity;
        while (grown_capacity < next) {
            if (grown_capacity > SIZE_MAX / 2) {
                grown_capacity = next;
                break;
            }
            grown_capacity *= 2;
        }
        if (grown_capacity > SIZE_MAX / size) return false;
        if (!sol_resource_charge_arena(grown_capacity - *capacity)) return false;
        void *grown = realloc(*items, grown_capacity * size);
        if (grown == NULL) return false;
        *items = grown;
        *capacity = grown_capacity;
    }
    memset((unsigned char *)*items + old * size, 0, additional * size);
    *count = next;
    if (first != NULL) *first = (unsigned char *)*items + old * size;
    return true;
}

static bool sol_ir_append_place(SolIrLowerer *lowerer, SolIrPlace place,
    SolIrPlaceId *id) {
    SolIrPlace *entry = NULL;
    if (!sol_ir_grow((void **)&lowerer->ir->places, &lowerer->ir->place_count,
        &lowerer->place_capacity, 1, sizeof(*entry), (void **)&entry)) return false;
    *entry = place;
    *id = lowerer->ir->place_count - 1;
    return true;
}

static bool sol_ir_append_projection(
    SolIrLowerer *lowerer, SolIrProjection projection
) {
    SolIrProjection *entry = NULL;
    if (!sol_ir_grow((void **)&lowerer->ir->projections,
        &lowerer->ir->projection_count, &lowerer->projection_capacity, 1,
        sizeof(*entry), (void **)&entry)) return false;
    *entry = projection;
    return true;
}

static bool sol_ir_append_type_id(SolIrLowerer *lowerer, SolIrTypeId type) {
    SolIrTypeId *entry = NULL;
    if (!sol_ir_grow((void **)&lowerer->ir->type_ids, &lowerer->ir->type_id_count,
        &lowerer->type_id_capacity, 1, sizeof(*entry), (void **)&entry)) return false;
    *entry = type;
    SolAccessMode *access = NULL;
    if (!sol_ir_grow((void **)&lowerer->ir->accesses, &lowerer->ir->access_count,
        &lowerer->access_capacity, 1, sizeof(*access), (void **)&access)) return false;
    *access = SOL_ACCESS_OWNED;
    return true;
}

static bool sol_ir_append_root(SolIrLowerer *lowerer, SolIrLocalId root) {
    SolIrLocalId *entry = NULL;
    if (!sol_ir_grow((void **)&lowerer->ir->roots, &lowerer->ir->root_count,
        &lowerer->root_capacity, 1, sizeof(*entry), (void **)&entry)) return false;
    *entry = root;
    return true;
}

static bool sol_ir_append_cleanup_local(
    SolIrLowerer *lowerer, SolIrLocalId local
) {
    SolIrLocalId *entry = NULL;
    if (!sol_ir_grow((void **)&lowerer->ir->cleanup_locals,
        &lowerer->ir->cleanup_local_count, &lowerer->cleanup_local_capacity,
        1, sizeof(*entry), (void **)&entry)) return false;
    *entry = local;
    return true;
}

static bool sol_ir_append_member(SolIrLowerer *lowerer, SolIrCallableId callable) {
    SolIrMember *entry = NULL;
    if (!sol_ir_grow((void **)&lowerer->ir->members, &lowerer->ir->member_count,
        &lowerer->member_capacity, 1, sizeof(*entry), (void **)&entry)) return false;
    entry->callable = callable;
    return true;
}

static bool sol_ir_append_statement_id(
    SolIrLowerer *lowerer, SolIrStatementId statement
) {
    SolIrStatementId *entry = NULL;
    if (!sol_ir_grow((void **)&lowerer->ir->statement_ids,
        &lowerer->ir->statement_id_count, &lowerer->statement_id_capacity,
        1, sizeof(*entry), (void **)&entry)) return false;
    *entry = statement;
    return true;
}

static bool sol_ir_append_arm_id(SolIrLowerer *lowerer, SolIrArmId arm) {
    SolIrArmId *entry = NULL;
    if (!sol_ir_grow((void **)&lowerer->ir->arm_ids,
        &lowerer->ir->arm_id_count, &lowerer->arm_id_capacity,
        1, sizeof(*entry), (void **)&entry)) return false;
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
        case SOL_ITEM_TEST: return SOL_IR_DEFINITION_TEST;
        case SOL_ITEM_TYPE:
            return item->flavor == SOL_TYPE_DECLARATION_REFINED
                ? SOL_IR_DEFINITION_REFINED : SOL_IR_DEFINITION_DISTINCT;
    }
    return SOL_IR_DEFINITION_FUNCTION;
}

static SolIrTypeId sol_ir_type(SolIrLowerer *lowerer, SolType type);
static bool sol_ir_decode_string(
    SolIrLowerer *lowerer, SolSpan span, char **output
);
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
            || existing->effects.count != candidate.effects.count
            || existing->effect_parameter != candidate.effect_parameter) continue;
        bool equal = true;
        for (size_t argument = 0; equal && argument < candidate.argument_count; ++argument) {
            equal = lowerer->ir->type_ids[existing->argument_offset + argument]
                == lowerer->ir->type_ids[candidate.argument_offset + argument];
        }
        for (size_t parameter = 0; equal && parameter < candidate.parameter_count; ++parameter) {
            equal = lowerer->ir->type_ids[existing->parameter_offset + parameter]
                    == lowerer->ir->type_ids[candidate.parameter_offset + parameter]
                && lowerer->ir->accesses[existing->parameter_access_offset + parameter]
                    == lowerer->ir->accesses[
                        candidate.parameter_access_offset + parameter];
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
        &lowerer->type_capacity, 1, sizeof(*entry), (void **)&entry)) {
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
        .effect_parameter = SOL_IR_NONE,
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
                    ? SOL_IR_TYPE_RESULT
                    : application->constructor == SOL_TYPE_CONSTRUCTOR_TUPLE
                        ? SOL_IR_TYPE_TUPLE : SOL_IR_TYPE_NOMINAL;
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
            candidate.effect_parameter = function->effect_parameter;
            candidate.parameter_offset = lowerer->ir->type_id_count;
            candidate.parameter_access_offset = lowerer->ir->access_count;
            for (size_t index = 0; index < function->parameter_count; ++index) {
                if (!sol_ir_append_type_id(lowerer, lowered_parameters[index])) {
                    free(lowered_parameters);
                    lowerer->failed = true;
                    return SOL_IR_NONE;
                }
                lowerer->ir->accesses[lowerer->ir->access_count - 1]
                    = function->accesses[index];
            }
            free(lowered_parameters);
            SolIrTypeId result = sol_ir_add_type(lowerer, candidate);
            lowerer->function_types[type.definition] = result;
            lowerer->function_states[type.definition] = 2;
            return result;
        }
        case SOL_TYPE_CAPABILITY_OPERATION: {
            if (type.definition >= lowerer->syntax->capability_member_count) {
                lowerer->failed = true;
                return SOL_IR_NONE;
            }
            const SolCapabilityMember *member
                = &lowerer->syntax->capability_members[type.definition];
            candidate.kind = SOL_IR_TYPE_FUNCTION;
            candidate.parameter_offset = lowerer->ir->type_id_count;
            candidate.parameter_access_offset = lowerer->ir->access_count;
            SolParameterId parameter = member->first_parameter;
            size_t traversed = 0;
            while (parameter != SOL_AST_NONE) {
                if (parameter >= lowerer->syntax->parameter_count
                    || traversed++ >= lowerer->syntax->parameter_count) {
                    lowerer->failed = true;
                    return SOL_IR_NONE;
                }
                const SolParameter *entry = &lowerer->syntax->parameters[parameter];
                SolIrTypeId parameter_type = sol_ir_type(
                    lowerer, lowerer->types->declared_types[entry->type_id]);
                if (parameter_type == SOL_IR_NONE
                    || !sol_ir_append_type_id(lowerer, parameter_type)) {
                    lowerer->failed = true;
                    return SOL_IR_NONE;
                }
                lowerer->ir->accesses[lowerer->ir->access_count - 1] = entry->access;
                ++candidate.parameter_count;
                parameter = entry->next;
            }
            candidate.result = sol_ir_type(lowerer,
                lowerer->types->declared_types[member->return_type_id]);
            candidate.effects = sol_ir_effect_row(lowerer,
                lowerer->effects->capability_members[type.definition].atoms,
                lowerer->effects->capability_members[type.definition].count);
            if (candidate.result == SOL_IR_NONE || lowerer->failed) return SOL_IR_NONE;
            return sol_ir_add_type(lowerer, candidate);
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
        &lowerer->effect_capacity, count, sizeof(*entries), (void **)&entries)) {
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
    SOL_IR_MAP(pattern_locals, lowerer->syntax->pattern_count);
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
    if (lowerer->syntax->pattern_count != 0) {
        lowerer->pattern_states = sol_ir_allocate(
            lowerer->syntax->pattern_count, 1, true);
        if (lowerer->pattern_states == NULL) return false;
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
    free(lowerer->pattern_states);
}

static bool sol_ir_frontend_shape_valid(SolIrLowerer *lowerer) {
    const SolSyntaxTree *syntax = lowerer->syntax;
    const SolHirModule *hir = lowerer->hir;
    const SolTypeTable *types = lowerer->types;
    const SolEffectTable *effects = lowerer->effects;
    const SolContractTable *contracts = lowerer->contracts;
    if (!sol_syntax_contracts_validate(lowerer->source, syntax)
        || !sol_type_resolution_metadata_valid(lowerer->source, syntax, types)
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
        || types->pattern_child_resolution_count != syntax->pattern_binding_count
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
            && (types->pattern_variant_resolutions == NULL
                || types->pattern_types == NULL))
        || (types->pattern_child_resolution_count != 0
            && (types->pattern_field_resolutions == NULL
                || types->pattern_tuple_ordinals == NULL))
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
        || contracts->loop_obligation_count > contracts->loop_obligation_capacity
        || contracts->unreachable_obligation_count
            > contracts->unreachable_obligation_capacity
        || (contracts->obligation_count != 0 && contracts->obligations == NULL)
        || (contracts->snapshot_count != 0 && contracts->snapshots == NULL)
        || (contracts->loop_obligation_count != 0
            && contracts->loop_obligations == NULL)
        || (contracts->unreachable_obligation_count != 0
            && contracts->unreachable_obligations == NULL)
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
        if ((function->parameter_count != 0
                && (function->parameters == NULL || function->accesses == NULL))
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
        if ((int)method->kind < 0 || method->kind > SOL_METHOD_RESOLUTION_IMPLEMENTATION
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
    for (size_t index = 0; index < contracts->loop_obligation_count; ++index) {
        const SolLoopObligation *obligation = &contracts->loop_obligations[index];
        const SolLoopFact *fact = obligation->loop_statement < types->loop_fact_count
            ? &types->loop_facts[obligation->loop_statement] : NULL;
        if (obligation->id != index
            || (int)obligation->kind < 0
            || obligation->kind > SOL_LOOP_OBLIGATION_DECREASES_STRICT
            || obligation->loop_statement >= syntax->statement_count
            || (syntax->statements[obligation->loop_statement].kind
                    != SOL_STATEMENT_LOOP
                && syntax->statements[obligation->loop_statement].kind
                    != SOL_STATEMENT_WHILE)
            || obligation->expression >= syntax->expression_count
            || fact == NULL || !fact->is_loop
            || obligation->owner != fact->owner
            || obligation->owner_member != fact->owner_member
            || obligation->owner_trait_method != fact->owner_trait_method
            || !sol_ir_span_valid(lowerer->source, obligation->span)) return false;
    }
    for (size_t index = 0; index < contracts->unreachable_obligation_count; ++index) {
        const SolUnreachableObligation *obligation
            = &contracts->unreachable_obligations[index];
        const SolUnreachableFact *fact
            = obligation->statement < types->unreachable_fact_count
                ? &types->unreachable_facts[obligation->statement] : NULL;
        if (obligation->id != index
            || obligation->statement >= syntax->statement_count
            || syntax->statements[obligation->statement].kind
                != SOL_STATEMENT_UNREACHABLE
            || obligation->proof >= syntax->expression_count
            || fact == NULL || !fact->is_unreachable
            || obligation->owner != fact->owner
            || obligation->owner_member != fact->owner_member
            || obligation->owner_trait_method != fact->owner_trait_method
            || !sol_ir_span_valid(lowerer->source, obligation->span)) return false;
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
        local->access = source->access;
        local->mutable = source->mutable;
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
            && source->syntax_id < lowerer->syntax->pattern_count
            && lowerer->syntax->patterns[source->syntax_id].kind
                == SOL_PATTERN_BINDING) {
            local->kind = SOL_IR_LOCAL_PATTERN;
            name = lowerer->syntax->patterns[source->syntax_id].name;
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
        &lowerer->callable_capacity, 1, sizeof(*entry), (void **)&entry)) return SOL_IR_NONE;
    entry->kind = kind;
    entry->owner = owner;
    entry->name = sol_ir_copy_span(lowerer->source, name);
    entry->span = span;
    entry->receiver = SOL_IR_NONE;
    entry->receiver_access = SOL_ACCESS_OWNED;
    entry->capability_source = SOL_IR_NONE;
    entry->result_authority = SOL_IR_NONE;
    if (skip_first && parameters != SOL_AST_NONE) {
        entry->receiver = sol_ir_local_for_parameter(lowerer, parameters);
        entry->receiver_access = lowerer->syntax->parameters[parameters].access;
    }
    entry->parameters = sol_ir_parameter_slice(lowerer, parameters, skip_first);
    entry->result = sol_ir_type(lowerer, result);
    entry->body = body == SOL_AST_NONE ? SOL_IR_NONE : body;
    entry->effects = sol_ir_effect_row(lowerer, effects->atoms, effects->count);
    entry->effect_parameter = effects->effect_parameter;
    if (owner < lowerer->ir->definition_count) {
        entry->generic_parameters = lowerer->ir->definitions[owner].generic_parameters;
        entry->effect_parameters = lowerer->ir->definitions[owner].effect_parameters;
        entry->capability_source = lowerer->ir->definitions[owner].capability_source;
    }
    if (entry->name == NULL || entry->result == SOL_IR_NONE || lowerer->failed) {
        return SOL_IR_NONE;
    }
    return lowerer->ir->callable_count - 1;
}

static SolIrTypeId sol_ir_callable_type(
    SolIrLowerer *lowerer, SolIrCallableId callable_id
) {
    if (callable_id >= lowerer->ir->callable_count) return SOL_IR_NONE;
    const SolIrCallable *callable = &lowerer->ir->callables[callable_id];
    SolIrType candidate = {
        .kind = SOL_IR_TYPE_FUNCTION,
        .definition = SOL_IR_NONE,
        .parameter_offset = lowerer->ir->type_id_count,
        .parameter_access_offset = lowerer->ir->access_count,
        .parameter_count = callable->parameters.count,
        .result = callable->result,
        .effects = callable->effects,
        .effect_parameter = callable->effect_parameter,
    };
    for (size_t index = 0; index < callable->parameters.count; ++index) {
        SolIrLocalId local = lowerer->ir->roots[callable->parameters.offset + index];
        if (local >= lowerer->ir->local_count
            || !sol_ir_append_type_id(lowerer, lowerer->ir->locals[local].type)) {
            lowerer->failed = true;
            return SOL_IR_NONE;
        }
        lowerer->ir->accesses[lowerer->ir->access_count - 1]
            = lowerer->ir->locals[local].access;
    }
    return sol_ir_add_type(lowerer, candidate);
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
        definition->open = source->is_open;
        definition->name = sol_ir_copy_span(lowerer->source, source->name);
        definition->span = source->span;
        definition->callable = SOL_IR_NONE;
        definition->declared_type = lowerer->types->definitions[index].kind
                == SOL_TYPE_UNKNOWN
            ? SOL_IR_NONE : sol_ir_type(lowerer, lowerer->types->definitions[index]);
        definition->representation = SOL_IR_NONE;
        definition->implementation_trait = SOL_IR_NONE;
        definition->implementation_target = SOL_IR_NONE;
        definition->capability_source = SOL_IR_NONE;
        if (source->capability_source != SOL_AST_NONE) {
            definition->capability_source = sol_ir_local_for_parameter(
                lowerer, source->capability_source);
            if (definition->capability_source == SOL_IR_NONE) return false;
        }
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
        if (source->kind == SOL_ITEM_TEST) {
            free(definition->name);
            definition->name = NULL;
            if (!sol_ir_decode_string(lowerer, source->name, &definition->name)) return false;
        }
        if (definition->name == NULL) return false;
    }
    for (size_t index = 0; index < count; ++index) {
        const SolSyntaxItem *item = &lowerer->syntax->items[index];
        if (item->kind != SOL_ITEM_FUNCTION && item->kind != SOL_ITEM_TEST) continue;
        SolIrCallableKind kind = item->kind == SOL_ITEM_TEST
            ? SOL_IR_CALLABLE_TEST : SOL_IR_CALLABLE_FUNCTION;
        SolIrCallableId callable = sol_ir_add_callable(lowerer, kind,
            index, item->name, item->span, item->first_parameter, false,
            lowerer->types->definitions[index], item->body, &lowerer->effects->functions[index]);
        if (callable == SOL_IR_NONE) return false;
        if (item->kind == SOL_ITEM_TEST) {
            free(lowerer->ir->callables[callable].name);
            lowerer->ir->callables[callable].name = NULL;
            if (!sol_ir_decode_string(
                lowerer, item->name, &lowerer->ir->callables[callable].name
            )) return false;
        }
        lowerer->definition_callables[index] = callable;
        lowerer->ir->definitions[index].callable = callable;
        lowerer->ir->callables[callable].generic_parameters
            = lowerer->ir->definitions[index].generic_parameters;
        lowerer->ir->callables[callable].effect_parameters
            = lowerer->ir->definitions[index].effect_parameters;
        if (item->result_authority_parameter != SOL_AST_NONE) {
            lowerer->ir->callables[callable].result_authority_kind
                = SOL_IR_AUTHORITY_LOCAL;
            lowerer->ir->callables[callable].result_authority
                = sol_ir_local_for_parameter(lowerer, item->result_authority_parameter);
            if (lowerer->ir->callables[callable].result_authority == SOL_IR_NONE) return false;
        }
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
        if (member->result_authority_from_self) {
            lowerer->ir->callables[callable].result_authority_kind
                = SOL_IR_AUTHORITY_SELF;
        }
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
        &lowerer->evidence_capacity, 1, sizeof(*entry), (void **)&entry)) return false;
    *entry = evidence;
    return true;
}

static SolIrSlice sol_ir_method_evidence(
    SolIrLowerer *lowerer, const SolMethodResolution *method, SolExprId receiver
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
                .binding = SOL_IR_NONE,
                .parameter = SOL_IR_NONE,
            })) {
            lowerer->failed = true;
            return slice;
        }
        slice.count = 1;
        return slice;
    }
    if (receiver >= lowerer->types->expression_count) {
        lowerer->failed = true;
        return slice;
    }
    SolType type = lowerer->types->expressions[receiver];
    if (type.kind != SOL_TYPE_PARAMETER
        || type.definition >= lowerer->syntax->type_parameter_count
        || lowerer->generic_parameters[type.definition] == SOL_IR_NONE
        || !sol_ir_append_evidence(lowerer, (SolIrDispatchEvidence){
            .trait = method->trait,
            .requirement = lowerer->method_callables[method->requirement],
            .implementation = SOL_IR_NONE,
            .method = SOL_IR_NONE,
            .type = SOL_IR_NONE,
            .binding = SOL_IR_NONE,
            .parameter = lowerer->generic_parameters[type.definition],
            .forwarded = true,
        })) {
        lowerer->failed = true;
        return slice;
    }
    slice.count = 1;
    return slice;
}

static bool sol_ir_append_bound_evidence(
    SolIrLowerer *lowerer, SolIrExpression *call
) {
    if (call->as.call.callable == SOL_IR_NONE) return true;
    const SolIrCallable *target = &lowerer->ir->callables[call->as.call.callable];
    if (target->generic_parameters.count != call->as.call.type_arguments.count) return true;
    if (call->as.call.evidence.count == 0) {
        call->as.call.evidence.offset = lowerer->ir->evidence_count;
    }
    for (size_t ordinal = 0; ordinal < target->generic_parameters.count; ++ordinal) {
        SolIrGenericParameterId parameter = target->generic_parameters.offset + ordinal;
        SolIrDefinitionId trait = lowerer->ir->generic_parameters[parameter].trait_bound;
        if (trait == SOL_IR_NONE) continue;
        SolIrTypeId type_id = lowerer->ir->type_ids[
            call->as.call.type_arguments.offset + ordinal
        ];
        if (type_id >= lowerer->ir->type_count) return false;
        const SolIrType *type = &lowerer->ir->types[type_id];
        SolIrSlice requirements = lowerer->ir->definitions[trait].members;
        for (size_t member = 0; member < requirements.count; ++member) {
            SolIrCallableId requirement
                = lowerer->ir->members[requirements.offset + member].callable;
            SolIrDispatchEvidence evidence = {
                .trait = trait,
                .requirement = requirement,
                .implementation = SOL_IR_NONE,
                .method = SOL_IR_NONE,
                .type = SOL_IR_NONE,
                .binding = parameter,
                .parameter = SOL_IR_NONE,
            };
            if (type->kind == SOL_IR_TYPE_PARAMETER) {
                evidence.forwarded = true;
                evidence.parameter = type->definition;
                if (evidence.parameter >= lowerer->ir->generic_parameter_count
                    || lowerer->ir->generic_parameters[evidence.parameter].trait_bound
                        != trait) return false;
            } else {
                for (SolIrDefinitionId definition = 0;
                    definition < lowerer->ir->definition_count; ++definition) {
                    const SolIrDefinition *implementation
                        = &lowerer->ir->definitions[definition];
                    if (implementation->kind != SOL_IR_DEFINITION_IMPLEMENTATION
                        || implementation->implementation_trait != trait
                        || implementation->implementation_target != type_id) continue;
                    for (size_t candidate = 0;
                        candidate < implementation->members.count; ++candidate) {
                        SolIrCallableId method = lowerer->ir->members[
                            implementation->members.offset + candidate
                        ].callable;
                        if (strcmp(lowerer->ir->callables[method].name,
                            lowerer->ir->callables[requirement].name) == 0) {
                            evidence.implementation = definition;
                            evidence.method = method;
                            evidence.type = type_id;
                            break;
                        }
                    }
                    if (evidence.method != SOL_IR_NONE) break;
                }
                if (evidence.method == SOL_IR_NONE) return false;
            }
            if (!sol_ir_append_evidence(lowerer, evidence)) return false;
            ++call->as.call.evidence.count;
        }
    }
    return true;
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
        &lowerer->operand_capacity, 1, sizeof(*operand), (void **)&operand)) return false;
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
        cursor = parameter;
        for (size_t index = 0; index < formal; ++index) {
            cursor = lowerer->syntax->parameters[cursor].next;
        }
        lowerer->ir->operands[lowerer->ir->operand_count - 1].access
            = lowerer->syntax->parameters[cursor].access;
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

static SolCapabilityMemberId sol_ir_bound_member(
    const SolIrLowerer *lowerer, const SolExpr *field
) {
    if (field->kind != SOL_EXPR_FIELD
        || field->as.field.base >= lowerer->types->expression_count) return SOL_AST_NONE;
    SolType base = lowerer->types->expressions[field->as.field.base];
    if (base.kind != SOL_TYPE_NOMINAL || base.definition >= lowerer->syntax->item_count
        || lowerer->syntax->items[base.definition].kind != SOL_ITEM_CAPABILITY) {
        return SOL_AST_NONE;
    }
    SolCapabilityMemberId member
        = lowerer->syntax->items[base.definition].first_member;
    size_t traversed = 0;
    while (member != SOL_AST_NONE) {
        if (member >= lowerer->syntax->capability_member_count
            || traversed++ >= lowerer->syntax->capability_member_count) return SOL_AST_NONE;
        const SolCapabilityMember *candidate
            = &lowerer->syntax->capability_members[member];
        size_t left = candidate->name.end - candidate->name.start;
        size_t right = field->as.field.name.end - field->as.field.name.start;
        if (left == right && memcmp(lowerer->source->text + candidate->name.start,
            lowerer->source->text + field->as.field.name.start, left) == 0) return member;
        member = candidate->next;
    }
    return SOL_AST_NONE;
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

static bool sol_ir_append_instantiated_effect(
    SolIrLowerer *lowerer,
    SolIrSlice *slice,
    const char *name,
    SolIrAuthorityKind authority_kind,
    SolIrLocalId authority
) {
    SolIrEffect *entry = NULL;
    if (!sol_ir_grow((void **)&lowerer->ir->effects, &lowerer->ir->effect_count,
        &lowerer->effect_capacity, 1, sizeof(*entry), (void **)&entry)) return false;
    entry->name = sol_ir_copy_text(name, strlen(name));
    entry->authority_kind = authority_kind;
    entry->authority = authority;
    if (entry->name == NULL) return false;
    ++slice->count;
    return true;
}

static bool sol_ir_call_actual(
    const SolIrLowerer *lowerer,
    const SolIrExpression *call,
    size_t formal,
    SolIrExpressionId *actual
) {
    for (size_t index = 0; index < call->as.call.operands.count; ++index) {
        const SolIrOperand *operand
            = &lowerer->ir->operands[call->as.call.operands.offset + index];
        if (operand->formal == formal) {
            *actual = operand->value;
            return true;
        }
    }
    return false;
}

static bool sol_ir_append_effect_provenance(
    SolIrLowerer *lowerer,
    SolIrSlice *slice,
    const char *name,
    SolProvenanceId provenance_id
) {
    SolProvenance provenance;
    if (!sol_type_provenance(lowerer->types, provenance_id, &provenance)
        || provenance.count == 0) return false;
    for (size_t index = 0; index < provenance.count; ++index) {
        SolIrLocalId root = sol_ir_local_for_parameter(lowerer, provenance.roots[index]);
        if (root == SOL_IR_NONE || !sol_ir_append_instantiated_effect(lowerer, slice,
            name, SOL_IR_AUTHORITY_LOCAL, root)) return false;
    }
    return true;
}

static bool sol_ir_instantiate_call_effects(
    SolIrLowerer *lowerer, const SolExpr *source, SolIrExpression *call
) {
    SolIrSlice original = call->as.call.effects;
    if (original.count == 0) return true;
    SolIrEffect *staged = sol_ir_allocate(original.count, sizeof(*staged), false);
    if (staged == NULL) return false;
    memcpy(staged, lowerer->ir->effects + original.offset, original.count * sizeof(*staged));
    SolIrSlice instantiated = {.offset = lowerer->ir->effect_count};
    SolIrCallableId target = call->as.call.callable;
    for (size_t index = 0; index < original.count; ++index) {
        const SolIrEffect *effect = &staged[index];
        if (effect->authority_kind == SOL_IR_AUTHORITY_NONE) {
            if (!sol_ir_append_instantiated_effect(lowerer, &instantiated,
                effect->name, SOL_IR_AUTHORITY_NONE, SOL_IR_NONE)) goto failed;
            continue;
        }
        if (effect->authority_kind == SOL_IR_AUTHORITY_SELF) {
            if (source->as.call.callee >= lowerer->types->expression_count
                || !sol_ir_append_effect_provenance(lowerer, &instantiated, effect->name,
                    lowerer->types->expression_operation_origins[
                        source->as.call.callee])) goto failed;
            continue;
        }
        bool substituted = false;
        if (target != SOL_IR_NONE && target < lowerer->ir->callable_count) {
            const SolIrCallable *callable = &lowerer->ir->callables[target];
            for (size_t formal = 0; formal < callable->parameters.count; ++formal) {
                if (lowerer->ir->roots[callable->parameters.offset + formal]
                    != effect->authority) continue;
                SolIrExpressionId actual;
                if (!sol_ir_call_actual(lowerer, call, formal, &actual)
                    || actual >= lowerer->types->expression_count
                    || !sol_ir_append_effect_provenance(lowerer, &instantiated,
                        effect->name,
                        lowerer->types->expression_capability_origins[actual])) goto failed;
                substituted = true;
                break;
            }
        }
        if (!substituted && !sol_ir_append_instantiated_effect(lowerer, &instantiated,
            effect->name, SOL_IR_AUTHORITY_LOCAL, effect->authority)) goto failed;
    }
    free(staged);
    if (instantiated.count > 1) qsort(lowerer->ir->effects + instantiated.offset,
        instantiated.count, sizeof(*lowerer->ir->effects), sol_ir_effect_compare);
    size_t unique = 0;
    for (size_t index = 0; index < instantiated.count; ++index) {
        SolIrEffect *effect = &lowerer->ir->effects[instantiated.offset + index];
        if (unique != 0 && sol_ir_effect_compare(
            &lowerer->ir->effects[instantiated.offset + unique - 1], effect) == 0) {
            free(effect->name);
            continue;
        }
        if (unique != index) {
            lowerer->ir->effects[instantiated.offset + unique] = *effect;
        }
        ++unique;
    }
    instantiated.count = unique;
    lowerer->ir->effect_count = instantiated.offset + unique;
    call->as.call.effects = instantiated;
    return true;

failed:
    free(staged);
    return false;
}

static bool sol_ir_lower_call(
    SolIrLowerer *lowerer, SolExprId id, const SolExpr *source, SolIrExpression *output
) {
    output->as.call.callable = SOL_IR_NONE;
    output->as.call.receiver = SOL_IR_NONE;
    output->as.call.receiver_access = SOL_ACCESS_OWNED;
    output->as.call.variant = SOL_IR_NONE;
    output->as.call.definition = SOL_IR_NONE;
    output->as.call.effect_parameter = SOL_IR_NONE;
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
        output->as.call.callable = lowerer->method_callables[method->requirement];
        if (output->as.call.callable == SOL_IR_NONE) return false;
        const SolExpr *callee = &lowerer->syntax->expressions[source->as.call.callee];
        if (callee->kind != SOL_EXPR_FIELD) return false;
        output->as.call.receiver = callee->as.field.base;
        output->as.call.receiver_access
            = lowerer->syntax->parameters[
                lowerer->syntax->trait_methods[method->requirement].first_parameter
            ].access;
        output->as.call.evidence = sol_ir_method_evidence(
            lowerer, method, callee->as.field.base);
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
        output->as.call.effects = sol_ir_effect_row(
            lowerer, function->effects.atoms, function->effects.count);
        output->as.call.effect_parameter = function->effect_parameter;
        output->as.call.operands.offset = lowerer->ir->operand_count;
        SolArgumentId argument = source->as.call.first_argument;
        for (size_t formal = 0; formal < function->parameter_count; ++formal) {
            if (argument == SOL_AST_NONE || argument >= lowerer->syntax->argument_count
                || !sol_ir_append_operand(lowerer, formal,
                    lowerer->syntax->arguments[argument].value)) return false;
            lowerer->ir->operands[lowerer->ir->operand_count - 1].access
                = function->accesses[formal];
            ++output->as.call.operands.count;
            argument = lowerer->syntax->arguments[argument].next;
        }
        if (argument != SOL_AST_NONE) return false;
    } else if (callee_type.kind == SOL_TYPE_CAPABILITY_OPERATION) {
        if (callee_type.definition >= lowerer->syntax->capability_member_count) return false;
        output->as.call.kind = SOL_IR_CALL_CAPABILITY;
        output->as.call.receiver_access = SOL_ACCESS_SHARED;
        output->as.call.callable = lowerer->member_callables[callee_type.definition];
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
    if (!sol_ir_append_bound_evidence(lowerer, output)) return false;
    const SolEffectCallInstantiation *effect
        = sol_effect_call_instantiation(lowerer->effects, id);
    if (effect != NULL) {
        if (effect->row_offset > lowerer->effects->call_row_count
            || effect->row_count > lowerer->effects->call_row_count - effect->row_offset) {
            return false;
        }
        output->as.call.effects = sol_ir_effect_row(lowerer,
            lowerer->effects->call_rows + effect->row_offset, effect->row_count);
        output->as.call.effect_parameter = effect->parameter;
    } else if (output->as.call.callable != SOL_IR_NONE) {
        output->as.call.effects = lowerer->ir->callables[output->as.call.callable].effects;
        output->as.call.effect_parameter
            = lowerer->ir->callables[output->as.call.callable].effect_parameter;
    }
    if (output->as.call.kind <= SOL_IR_CALL_METHOD
        && !sol_ir_instantiate_call_effects(lowerer, source, output)) return false;
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
        output->as.place = SOL_IR_NONE;
        output->span = source->span;
        output->type = SOL_IR_NONE;
        SolType frontend_type = lowerer->types->expressions[id];
        bool transient_head = frontend_type.kind == SOL_TYPE_UNKNOWN
            || frontend_type.kind == SOL_TYPE_VARIANT
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
                    output->kind = SOL_IR_EXPR_PLACE;
                    if (output->type == SOL_IR_NONE) {
                        output->type = lowerer->ir->locals[resolution.target].type;
                    }
                    SolIrPlace place = {
                        .root_kind = SOL_IR_PLACE_ROOT_LOCAL,
                        .local = resolution.target,
                        .temporary = SOL_IR_NONE,
                        .type = output->type,
                        .projections = {lowerer->ir->projection_count, 0},
                        .root_span = source->span,
                    };
                    if (!sol_ir_append_place(lowerer, place, &output->as.place)) {
                        return false;
                    }
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
                {
                SolCapabilityMemberId bound_member = sol_ir_bound_member(lowerer, source);
                if (lowerer->types->field_resolutions[id] != SOL_AST_NONE
                    || lowerer->types->tuple_projections[id] != SOL_AST_NONE) {
                    size_t chain_count = 0;
                    SolExprId current = id;
                    SolExprId *chain = lowerer->syntax->expression_count == 0 ? NULL
                        : sol_ir_allocate(lowerer->syntax->expression_count,
                            sizeof(*chain), false);
                    if (chain == NULL) return false;
                    while (current < lowerer->syntax->expression_count
                        && lowerer->syntax->expressions[current].kind == SOL_EXPR_FIELD
                        && (lowerer->types->field_resolutions[current] != SOL_AST_NONE
                            || lowerer->types->tuple_projections[current] != SOL_AST_NONE)) {
                        if (chain_count >= lowerer->syntax->expression_count) {
                            free(chain);
                            return false;
                        }
                        chain[chain_count++] = current;
                        current = lowerer->syntax->expressions[current].as.field.base;
                    }
                    if (current >= lowerer->syntax->expression_count) {
                        free(chain);
                        return false;
                    }
                    SolIrPlace place = {
                        .root_kind = SOL_IR_PLACE_ROOT_TEMPORARY,
                        .local = SOL_IR_NONE,
                        .temporary = current,
                        .type = output->type,
                        .projections = {lowerer->ir->projection_count, chain_count},
                        .root_span = lowerer->syntax->expressions[current].span,
                    };
                    if (current < lowerer->syntax->expression_count
                        && lowerer->syntax->expressions[current].kind == SOL_EXPR_PATH) {
                        SolResolution root = lowerer->hir->resolutions[current];
                        if (root.kind == SOL_RESOLUTION_LOCAL
                            && root.target < lowerer->ir->local_count) {
                            place.root_kind = SOL_IR_PLACE_ROOT_LOCAL;
                            place.local = root.target;
                            place.temporary = SOL_IR_NONE;
                        }
                    }
                    for (size_t projection = chain_count; projection != 0; --projection) {
                        SolExprId field_expression = chain[projection - 1];
                        SolIrFieldId field
                            = lowerer->types->field_resolutions[field_expression];
                        size_t ordinal
                            = lowerer->types->tuple_projections[field_expression];
                        SolIrTypeId type = sol_ir_type(lowerer,
                            lowerer->types->expressions[field_expression]);
                        bool tuple = ordinal != SOL_AST_NONE;
                        if ((!tuple && field >= lowerer->syntax->field_count)
                            || (tuple && field != SOL_AST_NONE)
                            || type == SOL_IR_NONE
                            || !sol_ir_append_projection(lowerer, (SolIrProjection){
                                .kind = tuple ? SOL_IR_PROJECTION_TUPLE_FIELD
                                    : SOL_IR_PROJECTION_FIELD,
                                .type = type,
                                .field = field,
                                .ordinal = tuple ? ordinal : SOL_IR_NONE,
                                .index = SOL_IR_NONE,
                                .span = lowerer->syntax->expressions[field_expression].span,
                            })) {
                            free(chain);
                            return false;
                        }
                    }
                    free(chain);
                    output->kind = SOL_IR_EXPR_PLACE;
                    if (!sol_ir_append_place(lowerer, place, &output->as.place)) {
                        return false;
                    }
                } else if (lowerer->types->variant_resolutions[id] != SOL_AST_NONE
                    && lowerer->types->variant_resolutions[id]
                        < lowerer->syntax->variant_count
                    && lowerer->syntax->variants[
                        lowerer->types->variant_resolutions[id]
                    ].first_field == SOL_AST_NONE) {
                    output->kind = SOL_IR_EXPR_VARIANT;
                    output->as.variant.variant = lowerer->types->variant_resolutions[id];
                } else if ((frontend_type.kind == SOL_TYPE_CAPABILITY_OPERATION
                        && frontend_type.definition
                            < lowerer->syntax->capability_member_count)
                    || bound_member != SOL_AST_NONE) {
                    SolCapabilityMemberId member
                        = frontend_type.kind == SOL_TYPE_CAPABILITY_OPERATION
                        ? frontend_type.definition : bound_member;
                    output->kind = SOL_IR_EXPR_BOUND_OPERATION;
                    output->as.operation.receiver = source->as.field.base;
                    output->as.operation.callable
                        = lowerer->member_callables[member];
                    output->type = sol_ir_callable_type(
                        lowerer, output->as.operation.callable);
                    if (output->type == SOL_IR_NONE) return false;
                } else {
                    output->kind = SOL_IR_EXPR_COMPILE_TIME_HEAD;
                    if (!sol_ir_compile_time_head_used(lowerer, id)) return false;
                }
                break;
                }
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
            case SOL_EXPR_TUPLE:
                output->kind = SOL_IR_EXPR_TUPLE;
                output->as.tuple.operands = sol_ir_positional_arguments(
                    lowerer, source->as.tuple.first_element);
                break;
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
                output->as.block.statements.offset = lowerer->ir->statement_id_count;
                output->as.block.cleanup.offset = lowerer->ir->cleanup_local_count;
                SolStatementId statement = source->as.block.first_statement;
                while (statement != SOL_AST_NONE) {
                    if (statement >= lowerer->syntax->statement_count
                        || output->as.block.statements.count++
                            >= lowerer->syntax->statement_count
                        || !sol_ir_append_statement_id(lowerer, statement)) {
                        return false;
                    }
                    if (lowerer->syntax->statements[statement].kind
                            == SOL_STATEMENT_LET
                        || lowerer->syntax->statements[statement].kind
                            == SOL_STATEMENT_VAR) {
                        SolIrLocalId local = lowerer->binding_locals[statement];
                        if (local == SOL_IR_NONE
                            || !sol_ir_append_cleanup_local(lowerer, local)) return false;
                        ++output->as.block.cleanup.count;
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
                "expression %zu has no exact IR type (frontend kind %d)",
                id, (int)frontend_type.kind);
            return false;
        }
    }
    return !lowerer->failed;
}

static bool sol_ir_lower_pattern(
    SolIrLowerer *lowerer, SolPatternId source_id, SolIrPatternId *result
) {
    if (source_id >= lowerer->syntax->pattern_count
        || lowerer->pattern_states[source_id] != 0) return false;
    lowerer->pattern_states[source_id] = 1;
    const SolPattern *source = &lowerer->syntax->patterns[source_id];
    SolIrPattern *pattern = &lowerer->ir->patterns[source_id];
    switch (source->kind) {
        case SOL_PATTERN_WILDCARD: pattern->kind = SOL_IR_PATTERN_WILDCARD; break;
        case SOL_PATTERN_BOOL: pattern->kind = SOL_IR_PATTERN_BOOL; break;
        case SOL_PATTERN_BINDING: pattern->kind = SOL_IR_PATTERN_BINDING; break;
        case SOL_PATTERN_VARIANT: pattern->kind = SOL_IR_PATTERN_VARIANT; break;
        case SOL_PATTERN_RECORD: pattern->kind = SOL_IR_PATTERN_RECORD; break;
        case SOL_PATTERN_TUPLE: pattern->kind = SOL_IR_PATTERN_TUPLE; break;
        default: return false;
    }
    pattern->type = sol_ir_type(lowerer, lowerer->types->pattern_types[source_id]);
    pattern->span = source->span;
    pattern->variant = SOL_IR_NONE;
    pattern->definition = SOL_IR_NONE;
    pattern->binding = SOL_IR_NONE;
    pattern->children.offset = lowerer->ir->pattern_child_count;
    if (pattern->type == SOL_IR_NONE || !sol_ir_span_valid(lowerer->source, source->span)
        || (int)source->kind < SOL_PATTERN_WILDCARD
        || source->kind > SOL_PATTERN_TUPLE) return false;

    size_t child_count = 0;
    SolPatternBindingId child = source->first_binding;
    while (child != SOL_AST_NONE) {
        if (child >= lowerer->syntax->pattern_binding_count
            || child_count++ >= lowerer->syntax->pattern_binding_count) return false;
        child = lowerer->syntax->pattern_bindings[child].next;
    }
    bool aggregate = source->kind == SOL_PATTERN_VARIANT
        || source->kind == SOL_PATTERN_RECORD || source->kind == SOL_PATTERN_TUPLE;
    if (!aggregate && child_count != 0) return false;
    SolIrPatternChild *edges = NULL;
    if (!sol_ir_grow((void **)&lowerer->ir->pattern_children,
        &lowerer->ir->pattern_child_count, &lowerer->pattern_child_capacity,
        child_count, sizeof(*edges), (void **)&edges)) return false;
    pattern->children.count = child_count;

    const SolIrType *type = &lowerer->ir->types[pattern->type];
    if (source->kind == SOL_PATTERN_BINDING) {
        pattern->binding = lowerer->pattern_locals[source_id];
        if (pattern->binding == SOL_IR_NONE) return false;
    } else if (source->kind == SOL_PATTERN_BOOL) {
        if (type->kind != SOL_IR_TYPE_BOOL) return false;
        pattern->boolean = source->bool_value;
    } else if (source->kind == SOL_PATTERN_VARIANT) {
        pattern->variant = lowerer->types->pattern_variant_resolutions[source_id];
        if (pattern->variant >= lowerer->ir->variant_count
            || type->kind != SOL_IR_TYPE_NOMINAL
            || lowerer->ir->variants[pattern->variant].owner != type->definition
            || lowerer->ir->variants[pattern->variant].fields.count != child_count) {
            return false;
        }
    } else if (source->kind == SOL_PATTERN_RECORD) {
        if (type->kind != SOL_IR_TYPE_NOMINAL
            || type->definition >= lowerer->ir->definition_count
            || lowerer->ir->definitions[type->definition].kind
                != SOL_IR_DEFINITION_RECORD) return false;
        pattern->definition = type->definition;
    } else if (source->kind == SOL_PATTERN_TUPLE) {
        if (type->kind != SOL_IR_TYPE_TUPLE
            || type->argument_count != child_count) return false;
    }

    child = source->first_binding;
    for (size_t index = 0; index < child_count; ++index) {
        const SolPatternBinding *source_edge
            = &lowerer->syntax->pattern_bindings[child];
        size_t edge_index = pattern->children.offset + index;
        lowerer->ir->pattern_children[edge_index].field = SOL_IR_NONE;
        lowerer->ir->pattern_children[edge_index].ordinal = SOL_IR_NONE;
        if (source->kind == SOL_PATTERN_RECORD) {
            lowerer->ir->pattern_children[edge_index].field
                = lowerer->types->pattern_field_resolutions[child];
            if (lowerer->ir->pattern_children[edge_index].field
                    >= lowerer->ir->field_count
                || lowerer->ir->fields[
                    lowerer->ir->pattern_children[edge_index].field].owner
                    != pattern->definition) return false;
            for (size_t previous = 0; previous < index; ++previous) {
                if (lowerer->ir->pattern_children[
                        pattern->children.offset + previous].field
                    == lowerer->ir->pattern_children[edge_index].field) return false;
            }
        } else if (source->kind == SOL_PATTERN_TUPLE) {
            lowerer->ir->pattern_children[edge_index].ordinal
                = lowerer->types->pattern_tuple_ordinals[child];
            if (lowerer->ir->pattern_children[edge_index].ordinal != index) return false;
        } else if (source->kind == SOL_PATTERN_VARIANT) {
            lowerer->ir->pattern_children[edge_index].ordinal = index;
        }
        SolIrPatternId child_pattern = SOL_IR_NONE;
        if (!sol_ir_lower_pattern(lowerer, source_edge->pattern,
                &child_pattern)) return false;
        lowerer->ir->pattern_children[edge_index].pattern = child_pattern;
        child = source_edge->next;
    }
    lowerer->pattern_states[source_id] = 2;
    *result = source_id;
    return true;
}

static bool sol_ir_collect_pattern_bindings(
    SolIrLowerer *lowerer, SolIrPatternId id, SolIrArm *arm, size_t depth
) {
    if (id >= lowerer->ir->pattern_count || depth >= 64) return false;
    const SolIrPattern *pattern = &lowerer->ir->patterns[id];
    if (pattern->kind == SOL_IR_PATTERN_BINDING) {
        if (!sol_ir_append_root(lowerer, pattern->binding)
            || !sol_ir_append_cleanup_local(lowerer, pattern->binding)) return false;
        ++arm->bindings.count;
        ++arm->cleanup.count;
    }
    for (size_t index = 0; index < pattern->children.count; ++index) {
        if (!sol_ir_collect_pattern_bindings(lowerer,
            lowerer->ir->pattern_children[pattern->children.offset + index].pattern,
            arm, depth + 1)) return false;
    }
    return true;
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
    if (lowerer->syntax->pattern_count != 0) {
        lowerer->ir->patterns = sol_ir_allocate(lowerer->syntax->pattern_count,
            sizeof(*lowerer->ir->patterns), true);
        if (lowerer->ir->patterns == NULL) return false;
    }
    lowerer->ir->statement_count = statement_count;
    lowerer->ir->arm_count = arm_count;
    lowerer->ir->pattern_count = lowerer->syntax->pattern_count;
    for (size_t index = 0; index < statement_count; ++index) {
        const SolStatement *source = &lowerer->syntax->statements[index];
        SolIrStatement *output = &lowerer->ir->statements[index];
        output->span = source->span;
        output->local = SOL_IR_NONE;
        output->target = SOL_IR_NONE;
        output->region_label = NULL;
        output->expression = SOL_IR_NONE;
        output->condition = SOL_IR_NONE;
        output->operator_kind = SOL_TOKEN_EOF;
        if (source->kind == SOL_STATEMENT_LET || source->kind == SOL_STATEMENT_VAR) {
            bool initialized = source->as.let_statement.value != SOL_AST_NONE;
            output->kind = initialized ? SOL_IR_STATEMENT_LET
                : SOL_IR_STATEMENT_DECLARE;
            output->expression = initialized
                ? source->as.let_statement.value : SOL_IR_NONE;
            output->local = lowerer->binding_locals[index];
            if (output->local == SOL_IR_NONE) return false;
        } else if (source->kind == SOL_STATEMENT_ASSIGNMENT) {
            output->kind = SOL_IR_STATEMENT_ASSIGNMENT;
            output->target = source->as.assignment.target;
            output->expression = source->as.assignment.value;
            output->operator_kind = source->as.assignment.operator_kind;
        } else if (source->kind == SOL_STATEMENT_REGION) {
            output->kind = SOL_IR_STATEMENT_REGION;
            output->expression = source->as.region_statement.body;
            output->region_label = sol_ir_copy_span(lowerer->source,
                source->as.region_statement.label);
            output->region_label_span = source->as.region_statement.label;
            if (output->region_label == NULL) return false;
        } else if (source->kind == SOL_STATEMENT_MODIFY) {
            output->kind = SOL_IR_STATEMENT_MODIFY;
            output->target = source->as.modify.target;
            output->expression = source->as.modify.body;
        } else if (source->kind == SOL_STATEMENT_LOOP
            || source->kind == SOL_STATEMENT_WHILE) {
            output->kind = source->kind == SOL_STATEMENT_LOOP
                ? SOL_IR_STATEMENT_LOOP : SOL_IR_STATEMENT_WHILE;
            output->expression = source->as.loop_statement.body;
            output->condition = source->as.loop_statement.condition;
        } else if (source->kind == SOL_STATEMENT_BREAK
            || source->kind == SOL_STATEMENT_CONTINUE) {
            output->kind = source->kind == SOL_STATEMENT_BREAK
                ? SOL_IR_STATEMENT_BREAK : SOL_IR_STATEMENT_CONTINUE;
        } else if (source->kind == SOL_STATEMENT_PANIC) {
            output->kind = SOL_IR_STATEMENT_PANIC;
            output->expression = source->as.panic_statement.message;
        } else if (source->kind == SOL_STATEMENT_UNREACHABLE) {
            output->kind = SOL_IR_STATEMENT_UNREACHABLE;
        } else if (source->kind == SOL_STATEMENT_REQUIRE) {
            output->kind = SOL_IR_STATEMENT_REQUIRE;
            output->condition = source->as.require_statement.condition;
            output->expression = source->as.require_statement.fallback_block;
        } else {
            output->kind = source->kind == SOL_STATEMENT_RETURN
                ? SOL_IR_STATEMENT_RETURN : SOL_IR_STATEMENT_EXPRESSION;
            output->expression = source->as.expression;
        }
    }
    for (size_t index = 0; index < arm_count; ++index) {
        const SolMatchArm *source = &lowerer->syntax->match_arms[index];
        SolIrArm *output = &lowerer->ir->arms[index];
        output->span = source->span;
        output->guard = source->guard == SOL_AST_NONE ? SOL_IR_NONE : source->guard;
        output->body = source->value;
        output->bindings.offset = lowerer->ir->root_count;
        output->cleanup.offset = lowerer->ir->cleanup_local_count;
        if (!sol_ir_lower_pattern(lowerer, source->pattern, &output->pattern)
            || !sol_ir_collect_pattern_bindings(lowerer, output->pattern,
                output, 0)) return false;
        for (size_t left = 0; left < output->cleanup.count / 2; ++left) {
            size_t right = output->cleanup.count - 1 - left;
            SolIrLocalId temporary = lowerer->ir->cleanup_locals[
                output->cleanup.offset + left];
            lowerer->ir->cleanup_locals[output->cleanup.offset + left]
                = lowerer->ir->cleanup_locals[output->cleanup.offset + right];
            lowerer->ir->cleanup_locals[output->cleanup.offset + right] = temporary;
        }
    }
    for (size_t index = 0; index < lowerer->ir->pattern_count; ++index) {
        if (lowerer->pattern_states[index] != 2) return false;
    }
    return true;
}

static bool sol_ir_lower_contracts(SolIrLowerer *lowerer) {
    size_t obligation_count = lowerer->contracts->obligation_count;
    size_t snapshot_count = lowerer->contracts->snapshot_count;
    size_t loop_obligation_count = lowerer->contracts->loop_obligation_count;
    size_t unreachable_obligation_count
        = lowerer->contracts->unreachable_obligation_count;
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
    if (loop_obligation_count != 0) {
        lowerer->ir->loop_obligations = sol_ir_allocate(loop_obligation_count,
            sizeof(*lowerer->ir->loop_obligations), true);
        if (lowerer->ir->loop_obligations == NULL) return false;
    }
    if (unreachable_obligation_count != 0) {
        lowerer->ir->unreachable_obligations = sol_ir_allocate(
            unreachable_obligation_count,
            sizeof(*lowerer->ir->unreachable_obligations), true);
        if (lowerer->ir->unreachable_obligations == NULL) return false;
    }
    lowerer->ir->obligation_count = obligation_count;
    lowerer->ir->snapshot_count = snapshot_count;
    lowerer->ir->loop_obligation_count = loop_obligation_count;
    lowerer->ir->unreachable_obligation_count = unreachable_obligation_count;
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
    for (size_t index = 0; index < loop_obligation_count; ++index) {
        const SolLoopObligation *source
            = &lowerer->contracts->loop_obligations[index];
        SolIrLoopObligation *output = &lowerer->ir->loop_obligations[index];
        if (source->id != index || source->loop_statement >= lowerer->ir->statement_count
            || source->expression >= lowerer->ir->expression_count) return false;
        SolIrCallableId callable = source->owner_member != SOL_AST_NONE
            ? source->owner_member < lowerer->syntax->capability_member_count
                ? lowerer->member_callables[source->owner_member] : SOL_IR_NONE
            : source->owner_trait_method != SOL_AST_NONE
                ? source->owner_trait_method < lowerer->syntax->trait_method_count
                    ? lowerer->method_callables[source->owner_trait_method] : SOL_IR_NONE
                : source->owner < lowerer->syntax->item_count
                    ? lowerer->definition_callables[source->owner] : SOL_IR_NONE;
        if (callable == SOL_IR_NONE || callable >= lowerer->ir->callable_count) return false;
        SolIrStatement *loop = &lowerer->ir->statements[source->loop_statement];
        if (loop->kind != SOL_IR_STATEMENT_LOOP
            && loop->kind != SOL_IR_STATEMENT_WHILE) return false;
        if (loop->loop_obligations.count == 0) {
            loop->loop_obligations.offset = index;
        } else if (loop->loop_obligations.offset + loop->loop_obligations.count
            != index) {
            return false;
        }
        ++loop->loop_obligations.count;
        *output = (SolIrLoopObligation){
            .id = source->id,
            .kind = source->kind,
            .loop_statement = source->loop_statement,
            .callable = callable,
            .expression = source->expression,
            .expression_type = sol_ir_type(lowerer, source->expression_type),
            .span = source->span,
        };
        if (output->expression_type == SOL_IR_NONE) return false;
    }
    for (size_t index = 0; index < unreachable_obligation_count; ++index) {
        const SolUnreachableObligation *source
            = &lowerer->contracts->unreachable_obligations[index];
        if (source->id != index || source->statement >= lowerer->ir->statement_count
            || source->proof >= lowerer->ir->expression_count) return false;
        SolIrCallableId callable = source->owner_member != SOL_AST_NONE
            ? source->owner_member < lowerer->syntax->capability_member_count
                ? lowerer->member_callables[source->owner_member] : SOL_IR_NONE
            : source->owner_trait_method != SOL_AST_NONE
                ? source->owner_trait_method < lowerer->syntax->trait_method_count
                    ? lowerer->method_callables[source->owner_trait_method] : SOL_IR_NONE
                : source->owner < lowerer->syntax->item_count
                    ? lowerer->definition_callables[source->owner] : SOL_IR_NONE;
        SolIrStatement *statement = &lowerer->ir->statements[source->statement];
        if (callable == SOL_IR_NONE || callable >= lowerer->ir->callable_count
            || statement->kind != SOL_IR_STATEMENT_UNREACHABLE
            || statement->unreachable_obligations.count != 0) return false;
        statement->unreachable_obligations = (SolIrSlice){index, 1};
        lowerer->ir->unreachable_obligations[index]
            = (SolIrUnreachableObligation){
                .id = source->id,
                .statement = source->statement,
                .callable = callable,
                .proof = source->proof,
                .proof_type = sol_ir_type(lowerer, source->proof_type),
                .span = source->span,
            };
        if (lowerer->ir->unreachable_obligations[index].proof_type == SOL_IR_NONE) {
            return false;
        }
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

static bool sol_ir_access_valid(SolAccessMode access) {
    return (int)access >= 0 && access <= SOL_ACCESS_EXCLUSIVE;
}

static bool sol_ir_unary_operator_valid(SolTokenKind kind) {
    return kind == SOL_TOKEN_BANG || kind == SOL_TOKEN_MINUS;
}

static bool sol_ir_binary_operator_valid(SolTokenKind kind) {
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

static bool sol_ir_type_is(const SolIr *ir, SolIrTypeId id, SolIrTypeKind kind) {
    return id < ir->type_count && ir->types[id].kind == kind;
}

static bool sol_ir_type_matches_instantiation(
    const SolIr *ir,
    SolIrTypeId actual_id,
    SolIrTypeId formal_id,
    SolIrSlice generic_parameters,
    SolIrSlice type_arguments,
    SolIrTypeId self_type,
    size_t depth
) {
    if (actual_id >= ir->type_count || formal_id >= ir->type_count
        || depth > ir->type_count) return false;
    const SolIrType *actual = &ir->types[actual_id];
    const SolIrType *formal = &ir->types[formal_id];
    if (formal->kind == SOL_IR_TYPE_PARAMETER
        && formal->definition >= generic_parameters.offset
        && formal->definition - generic_parameters.offset < generic_parameters.count) {
        size_t ordinal = formal->definition - generic_parameters.offset;
        return ordinal < type_arguments.count
            && ir->type_ids[type_arguments.offset + ordinal] == actual_id;
    }
    if (formal->kind == SOL_IR_TYPE_SELF && self_type != SOL_IR_NONE) {
        return actual_id == self_type;
    }
    if (formal->kind == SOL_IR_TYPE_FUNCTION && formal->definition == SOL_IR_NONE
        && actual->kind == SOL_IR_TYPE_FUNCTION && actual->definition != SOL_IR_NONE) {
        if (actual->definition >= ir->definition_count
            || ir->definitions[actual->definition].callable >= ir->callable_count) {
            return false;
        }
        const SolIrCallable *callable
            = &ir->callables[ir->definitions[actual->definition].callable];
        if (callable->parameters.count != formal->parameter_count
            || (formal->effect_parameter == SOL_IR_NONE
                && callable->effects.count != formal->effects.count)) return false;
        for (size_t index = 0; index < formal->parameter_count; ++index) {
            SolIrLocalId local = ir->roots[callable->parameters.offset + index];
            if (local >= ir->local_count
                || ir->locals[local].access
                    != ir->accesses[formal->parameter_access_offset + index]
                || !sol_ir_type_matches_instantiation(ir, ir->locals[local].type,
                    ir->type_ids[formal->parameter_offset + index], generic_parameters,
                    type_arguments, self_type, depth + 1)) return false;
        }
        if (!sol_ir_type_matches_instantiation(ir, callable->result, formal->result,
            generic_parameters, type_arguments, self_type, depth + 1)) return false;
        if (formal->effect_parameter == SOL_IR_NONE) {
            for (size_t index = 0; index < formal->effects.count; ++index) {
                if (sol_ir_effect_compare(&ir->effects[callable->effects.offset + index],
                    &ir->effects[formal->effects.offset + index]) != 0) return false;
            }
        }
        return true;
    }
    if (actual->kind != formal->kind || actual->definition != formal->definition
        || actual->argument_count != formal->argument_count
        || actual->parameter_count != formal->parameter_count) return false;
    for (size_t index = 0; index < formal->argument_count; ++index) {
        if (!sol_ir_type_matches_instantiation(ir,
            ir->type_ids[actual->argument_offset + index],
            ir->type_ids[formal->argument_offset + index], generic_parameters,
            type_arguments, self_type, depth + 1)) return false;
    }
    if (formal->kind == SOL_IR_TYPE_FUNCTION) {
        if (actual->result == SOL_IR_NONE || formal->result == SOL_IR_NONE
            || (formal->effect_parameter == SOL_IR_NONE
                && (actual->effects.count != formal->effects.count
                    || actual->effect_parameter != SOL_IR_NONE))) return false;
        for (size_t index = 0; index < formal->parameter_count; ++index) {
            if (ir->accesses[actual->parameter_access_offset + index]
                    != ir->accesses[formal->parameter_access_offset + index]
                || !sol_ir_type_matches_instantiation(ir,
                    ir->type_ids[actual->parameter_offset + index],
                    ir->type_ids[formal->parameter_offset + index], generic_parameters,
                    type_arguments, self_type, depth + 1)) return false;
        }
        if (!sol_ir_type_matches_instantiation(ir, actual->result, formal->result,
            generic_parameters, type_arguments, self_type, depth + 1)) return false;
        if (formal->effect_parameter == SOL_IR_NONE) {
            for (size_t index = 0; index < formal->effects.count; ++index) {
                if (sol_ir_effect_compare(&ir->effects[actual->effects.offset + index],
                    &ir->effects[formal->effects.offset + index]) != 0) return false;
            }
        }
    }
    return true;
}

static bool sol_ir_type_assignable(
    const SolIr *ir,
    SolIrTypeId actual,
    SolIrTypeId expected,
    SolIrSlice generic_parameters,
    SolIrSlice type_arguments,
    SolIrTypeId self_type
) {
    return sol_ir_type_is(ir, actual, SOL_IR_TYPE_NEVER)
        || sol_ir_type_matches_instantiation(ir, actual, expected, generic_parameters,
            type_arguments, self_type, 0);
}

static bool sol_ir_nominal_type(
    const SolIr *ir, SolIrTypeId id, SolIrDefinitionId definition
) {
    return id < ir->type_count && ir->types[id].kind == SOL_IR_TYPE_NOMINAL
        && ir->types[id].definition == definition;
}

static bool sol_ir_root_slice_valid(const SolIr *ir, SolIrSlice slice) {
    if (!sol_ir_slice_valid(slice, ir->root_count)) return false;
    for (size_t index = 0; index < slice.count; ++index) {
        SolIrLocalId local = ir->roots[slice.offset + index];
        if (local >= ir->local_count || ir->locals[local].kind != SOL_IR_LOCAL_PARAMETER
            || ir->locals[local].type >= ir->type_count
            || ir->types[ir->locals[local].type].kind != SOL_IR_TYPE_NOMINAL
            || ir->types[ir->locals[local].type].definition >= ir->definition_count
            || ir->definitions[ir->types[ir->locals[local].type].definition].kind
                != SOL_IR_DEFINITION_CAPABILITY
            || (index != 0 && ir->roots[slice.offset + index - 1] >= local)) return false;
    }
    return true;
}

static bool sol_ir_type_shape_valid(const SolIr *ir, const SolIrType *type) {
    if ((int)type->kind < 0 || type->kind > SOL_IR_TYPE_SELF) return false;
    bool no_arguments = type->argument_count == 0;
    bool no_function_shape = type->parameter_count == 0 && type->result == SOL_IR_NONE
        && type->effects.count == 0 && type->effect_parameter == SOL_IR_NONE;
    switch (type->kind) {
        case SOL_IR_TYPE_INT64:
        case SOL_IR_TYPE_BOOL:
        case SOL_IR_TYPE_TEXT:
        case SOL_IR_TYPE_UNIT:
        case SOL_IR_TYPE_NEVER:
            return type->definition == SOL_IR_NONE && no_arguments && no_function_shape;
        case SOL_IR_TYPE_NOMINAL:
            return type->definition < ir->definition_count && no_function_shape
                && (ir->definitions[type->definition].kind == SOL_IR_DEFINITION_RECORD
                    || ir->definitions[type->definition].kind == SOL_IR_DEFINITION_ENUM
                    || ir->definitions[type->definition].kind
                        == SOL_IR_DEFINITION_DISTINCT
                    || ir->definitions[type->definition].kind
                        == SOL_IR_DEFINITION_REFINED
                    || ir->definitions[type->definition].kind
                        == SOL_IR_DEFINITION_CAPABILITY)
                && (type->argument_count == 0 || type->argument_count
                    == ir->definitions[type->definition].generic_parameters.count);
        case SOL_IR_TYPE_OPTION:
            return type->definition == SOL_IR_NONE && type->argument_count == 1
                && no_function_shape;
        case SOL_IR_TYPE_RESULT:
            return type->definition == SOL_IR_NONE && type->argument_count == 2
                && no_function_shape;
        case SOL_IR_TYPE_TUPLE:
            return type->definition == SOL_IR_NONE && type->argument_count >= 2
                && type->argument_count <= 16 && no_function_shape;
        case SOL_IR_TYPE_FUNCTION:
            if (type->definition != SOL_IR_NONE) {
                return type->definition < ir->definition_count && no_arguments
                    && no_function_shape
                    && ir->definitions[type->definition].kind
                        == SOL_IR_DEFINITION_FUNCTION;
            }
            return no_arguments && type->result < ir->type_count;
        case SOL_IR_TYPE_PARAMETER:
            return type->definition < ir->generic_parameter_count && no_arguments
                && no_function_shape;
        case SOL_IR_TYPE_SELF:
            return type->definition < ir->definition_count && no_arguments
                && no_function_shape
                && (ir->definitions[type->definition].kind == SOL_IR_DEFINITION_TRAIT
                    || ir->definitions[type->definition].kind
                        == SOL_IR_DEFINITION_IMPLEMENTATION);
    }
    return false;
}

static bool sol_ir_function_type_matches_callable(
    const SolIr *ir, SolIrTypeId type_id, SolIrCallableId callable_id
) {
    if (type_id >= ir->type_count || callable_id >= ir->callable_count) return false;
    const SolIrType *type = &ir->types[type_id];
    const SolIrCallable *callable = &ir->callables[callable_id];
    if (type->kind != SOL_IR_TYPE_FUNCTION || type->definition != SOL_IR_NONE
        || type->parameter_count != callable->parameters.count
        || type->result != callable->result
        || type->effects.count != callable->effects.count
        || type->effect_parameter != callable->effect_parameter) return false;
    for (size_t index = 0; index < type->parameter_count; ++index) {
        SolIrLocalId local = ir->roots[callable->parameters.offset + index];
        if (local >= ir->local_count
            || ir->type_ids[type->parameter_offset + index]
                != ir->locals[local].type
            || ir->accesses[type->parameter_access_offset + index]
                != ir->locals[local].access) return false;
    }
    for (size_t index = 0; index < type->effects.count; ++index) {
        if (sol_ir_effect_compare(&ir->effects[type->effects.offset + index],
            &ir->effects[callable->effects.offset + index]) != 0) return false;
    }
    return true;
}

static bool sol_ir_definition_expression_valid(
    const SolIr *ir, SolIrExpressionId expression_id
) {
    if (expression_id >= ir->expression_count) return false;
    const SolIrExpression *expression = &ir->expressions[expression_id];
    SolIrDefinitionId definition_id = expression->as.definition;
    if (definition_id >= ir->definition_count) return false;
    const SolIrDefinition *definition = &ir->definitions[definition_id];
    if (definition->kind != SOL_IR_DEFINITION_FUNCTION) return false;
    if (definition->callable >= ir->callable_count) return false;
    const SolIrCallable *callable = &ir->callables[definition->callable];
    if (callable->kind != SOL_IR_CALLABLE_FUNCTION
        || callable->owner != definition_id || expression->type >= ir->type_count) return false;
    const SolIrType *type = &ir->types[expression->type];
    if (type->kind != SOL_IR_TYPE_FUNCTION) return false;
    bool exact_type = type->definition == definition_id
        && type->argument_count == 0 && type->parameter_count == 0
        && type->result == SOL_IR_NONE && type->effects.count == 0;
    return callable->generic_parameters.count == 0
        && callable->effect_parameters.count == 0
        && (exact_type
            || (type->definition == SOL_IR_NONE
                && sol_ir_function_type_matches_callable(
                    ir, expression->type, definition->callable)));
}

static bool sol_ir_expression_capability_definition(
    const SolIr *ir, SolIrExpressionId expression, SolIrDefinitionId *definition
) {
    if (expression >= ir->expression_count) return false;
    SolIrTypeId type_id = ir->expressions[expression].type;
    if (type_id >= ir->type_count) return false;
    const SolIrType *type = &ir->types[type_id];
    if (type->kind != SOL_IR_TYPE_NOMINAL || type->definition >= ir->definition_count
        || ir->definitions[type->definition].kind != SOL_IR_DEFINITION_CAPABILITY) {
        return false;
    }
    if (definition != NULL) *definition = type->definition;
    return true;
}

static bool sol_ir_callable_effect_has_self(
    const SolIr *ir, SolIrCallableId callable, const char *name
) {
    if (callable >= ir->callable_count || name == NULL) return false;
    SolIrSlice effects = ir->callables[callable].effects;
    for (size_t index = 0; index < effects.count; ++index) {
        const SolIrEffect *effect = &ir->effects[effects.offset + index];
        if (effect->authority_kind == SOL_IR_AUTHORITY_SELF
            && strcmp(effect->name, name) == 0) return true;
    }
    return false;
}

static bool sol_ir_callable_shapes_equal(
    const SolIr *ir, SolIrCallableId left_id, SolIrCallableId right_id
) {
    if (left_id >= ir->callable_count || right_id >= ir->callable_count) return false;
    const SolIrCallable *left = &ir->callables[left_id];
    const SolIrCallable *right = &ir->callables[right_id];
    if (left->parameters.count != right->parameters.count
        || left->result != right->result
        || left->receiver_access != right->receiver_access) return false;
    for (size_t index = 0; index < left->parameters.count; ++index) {
        SolIrLocalId left_local = ir->roots[left->parameters.offset + index];
        SolIrLocalId right_local = ir->roots[right->parameters.offset + index];
        if (left_local >= ir->local_count || right_local >= ir->local_count
            || ir->locals[left_local].type != ir->locals[right_local].type) return false;
        if (ir->locals[left_local].access != ir->locals[right_local].access) return false;
    }
    return true;
}

static bool sol_ir_callable_matches_requirement(
    const SolIr *ir,
    SolIrCallableId method_id,
    SolIrCallableId requirement_id,
    SolIrTypeId self_type
) {
    if (method_id >= ir->callable_count || requirement_id >= ir->callable_count
        || self_type >= ir->type_count) return false;
    const SolIrCallable *method = &ir->callables[method_id];
    const SolIrCallable *requirement = &ir->callables[requirement_id];
    if (!sol_ir_slice_valid(method->parameters, ir->root_count)
        || !sol_ir_slice_valid(requirement->parameters, ir->root_count)
        || !sol_ir_slice_valid(method->effects, ir->effect_count)
        || !sol_ir_slice_valid(requirement->effects, ir->effect_count)
        || !sol_ir_slice_valid(method->generic_parameters,
            ir->generic_parameter_count)
        || !sol_ir_slice_valid(requirement->generic_parameters,
            ir->generic_parameter_count)
        || method->receiver >= ir->local_count
        || requirement->receiver >= ir->local_count
        || ir->locals[method->receiver].type >= ir->type_count
        || ir->locals[requirement->receiver].type >= ir->type_count
        || method->result >= ir->type_count || requirement->result >= ir->type_count
        || method->parameters.count != requirement->parameters.count
        || method->receiver_access != requirement->receiver_access
        || method->effects.count != requirement->effects.count
        || method->effect_parameter != requirement->effect_parameter
        || ir->locals[method->receiver].type != self_type
        || ir->types[ir->locals[requirement->receiver].type].kind != SOL_IR_TYPE_SELF
        || ir->types[ir->locals[requirement->receiver].type].definition
            != requirement->owner) return false;
    for (size_t index = 0; index < method->parameters.count; ++index) {
        SolIrLocalId actual = ir->roots[method->parameters.offset + index];
        SolIrLocalId formal = ir->roots[requirement->parameters.offset + index];
        if (actual >= ir->local_count || formal >= ir->local_count
            || ir->locals[actual].type >= ir->type_count
            || ir->locals[formal].type >= ir->type_count
            || ir->locals[actual].access != ir->locals[formal].access
            || !sol_ir_type_matches_instantiation(ir, ir->locals[actual].type,
                ir->locals[formal].type, requirement->generic_parameters,
                (SolIrSlice){0}, self_type, 0)) return false;
    }
    if (!sol_ir_type_matches_instantiation(ir, method->result, requirement->result,
        requirement->generic_parameters, (SolIrSlice){0}, self_type, 0)) return false;
    for (size_t index = 0; index < method->effects.count; ++index) {
        const SolIrEffect *actual = &ir->effects[method->effects.offset + index];
        const SolIrEffect *formal
            = &ir->effects[requirement->effects.offset + index];
        if (actual->name == NULL || formal->name == NULL
            || sol_ir_effect_compare(actual, formal) != 0) return false;
    }
    return true;
}

static bool sol_ir_root_slice_contains(
    const SolIr *ir, SolIrSlice slice, SolIrLocalId local
) {
    if (!sol_ir_slice_valid(slice, ir->root_count)) return false;
    for (size_t index = 0; index < slice.count; ++index) {
        if (ir->roots[slice.offset + index] == local) return true;
    }
    return false;
}

static bool sol_ir_type_is_capability(const SolIr *ir, SolIrTypeId type_id) {
    return type_id < ir->type_count && ir->types[type_id].kind == SOL_IR_TYPE_NOMINAL
        && ir->types[type_id].definition < ir->definition_count
        && ir->definitions[ir->types[type_id].definition].kind
            == SOL_IR_DEFINITION_CAPABILITY;
}

typedef struct SolIrAuthorityEnvironment SolIrAuthorityEnvironment;
struct SolIrAuthorityEnvironment {
    SolIrDefinitionId definition;
    const SolIrType *application;
    const SolIrAuthorityEnvironment *parent;
};

typedef struct {
    SolIrTypeId type;
    const SolIrAuthorityEnvironment *environment;
} SolIrAuthorityExpansion;

static bool sol_ir_authority_types_equal(
    const SolIr *ir, SolIrTypeId left_id,
    const SolIrAuthorityEnvironment *left_environment, SolIrTypeId right_id,
    const SolIrAuthorityEnvironment *right_environment, size_t depth
) {
    if (left_id >= ir->type_count || right_id >= ir->type_count || depth >= 256) {
        return false;
    }
    const SolIrType *left = &ir->types[left_id];
    const SolIrType *right = &ir->types[right_id];
    if (left->kind == SOL_IR_TYPE_PARAMETER) {
        for (const SolIrAuthorityEnvironment *entry = left_environment;
            entry != NULL; entry = entry->parent) {
            SolIrSlice parameters = ir->definitions[entry->definition].generic_parameters;
            if (left->definition < parameters.offset
                || left->definition - parameters.offset >= parameters.count) continue;
            size_t ordinal = left->definition - parameters.offset;
            return ordinal < entry->application->argument_count
                && sol_ir_authority_types_equal(ir,
                    ir->type_ids[entry->application->argument_offset + ordinal],
                    entry->parent, right_id, right_environment, depth + 1);
        }
    }
    if (right->kind == SOL_IR_TYPE_PARAMETER) {
        for (const SolIrAuthorityEnvironment *entry = right_environment;
            entry != NULL; entry = entry->parent) {
            SolIrSlice parameters = ir->definitions[entry->definition].generic_parameters;
            if (right->definition < parameters.offset
                || right->definition - parameters.offset >= parameters.count) continue;
            size_t ordinal = right->definition - parameters.offset;
            return ordinal < entry->application->argument_count
                && sol_ir_authority_types_equal(ir, left_id, left_environment,
                    ir->type_ids[entry->application->argument_offset + ordinal],
                    entry->parent, depth + 1);
        }
    }
    if (left->kind != right->kind || left->definition != right->definition
        || left->argument_count != right->argument_count) return false;
    for (size_t index = 0; index < left->argument_count; ++index) {
        if (!sol_ir_authority_types_equal(ir,
            ir->type_ids[left->argument_offset + index], left_environment,
            ir->type_ids[right->argument_offset + index], right_environment,
            depth + 1)) return false;
    }
    return true;
}

static bool sol_ir_type_may_carry_authority_inner(
    const SolIr *ir, SolIrTypeId type_id, size_t depth, bool unresolved_parameters,
    const SolIrAuthorityEnvironment *environment, SolIrAuthorityExpansion *expanded,
    size_t expanded_count
) {
    if (type_id >= ir->type_count || depth >= 256 || expanded_count >= 256) return true;
    const SolIrType *type = &ir->types[type_id];
    if (type->kind == SOL_IR_TYPE_PARAMETER) {
        for (const SolIrAuthorityEnvironment *entry = environment; entry != NULL;
            entry = entry->parent) {
            if (entry->definition >= ir->definition_count) return true;
            SolIrSlice parameters = ir->definitions[entry->definition].generic_parameters;
            if (type->definition < parameters.offset
                || type->definition - parameters.offset >= parameters.count) continue;
            size_t ordinal = type->definition - parameters.offset;
            if (entry->application == NULL
                || ordinal >= entry->application->argument_count) return true;
            return sol_ir_type_may_carry_authority_inner(ir,
                ir->type_ids[entry->application->argument_offset + ordinal],
                depth + 1, unresolved_parameters, entry->parent, expanded,
                expanded_count);
        }
        return unresolved_parameters;
    }
    if (type->kind == SOL_IR_TYPE_FUNCTION || type->kind == SOL_IR_TYPE_SELF) return true;
    if (sol_ir_type_is_capability(ir, type_id)) return true;
    if (type->kind == SOL_IR_TYPE_OPTION || type->kind == SOL_IR_TYPE_RESULT
        || type->kind == SOL_IR_TYPE_TUPLE) {
        for (size_t index = 0; index < type->argument_count; ++index) {
            if (sol_ir_type_may_carry_authority_inner(ir,
                ir->type_ids[type->argument_offset + index], depth + 1,
                unresolved_parameters, environment, expanded,
                expanded_count)) return true;
        }
    }
    if (type->kind != SOL_IR_TYPE_NOMINAL
        || type->definition >= ir->definition_count) return false;
    const SolIrDefinition *definition = &ir->definitions[type->definition];
    for (size_t index = 0; index < expanded_count; ++index) {
        if (sol_ir_authority_types_equal(ir, type_id, environment,
            expanded[index].type, expanded[index].environment, 0)) return false;
    }
    expanded[expanded_count] = (SolIrAuthorityExpansion){type_id, environment};
    SolIrAuthorityEnvironment nested = {
        .definition = type->definition,
        .application = type,
        .parent = environment,
    };
    if (definition->kind == SOL_IR_DEFINITION_RECORD) {
        for (size_t index = 0; index < definition->fields.count; ++index) {
            if (sol_ir_type_may_carry_authority_inner(ir,
                ir->fields[definition->fields.offset + index].type, depth + 1,
                unresolved_parameters, &nested, expanded,
                expanded_count + 1)) return true;
        }
    } else if (definition->kind == SOL_IR_DEFINITION_ENUM) {
        for (size_t variant = 0; variant < definition->variants.count; ++variant) {
            SolIrSlice fields = ir->variants[definition->variants.offset + variant].fields;
            for (size_t field = 0; field < fields.count; ++field) {
                if (sol_ir_type_may_carry_authority_inner(ir,
                    ir->fields[fields.offset + field].type, depth + 1,
                    unresolved_parameters, &nested, expanded,
                    expanded_count + 1)) return true;
            }
        }
    } else if ((definition->kind == SOL_IR_DEFINITION_DISTINCT
            || definition->kind == SOL_IR_DEFINITION_REFINED)
        && sol_ir_type_may_carry_authority_inner(ir, definition->representation,
            depth + 1, unresolved_parameters, &nested, expanded,
            expanded_count + 1)) return true;
    return false;
}

static bool sol_ir_type_may_carry_authority(
    const SolIr *ir, SolIrTypeId type_id, bool unresolved_parameters
) {
    SolIrAuthorityExpansion expanded[256];
    return sol_ir_type_may_carry_authority_inner(
        ir, type_id, 0, unresolved_parameters, NULL, expanded, 0);
}

static bool sol_ir_roots_equal(const SolIr *ir, SolIrSlice left, SolIrSlice right) {
    if (!sol_ir_slice_valid(left, ir->root_count)
        || !sol_ir_slice_valid(right, ir->root_count)
        || left.count != right.count) return false;
    for (size_t index = 0; index < left.count; ++index) {
        if (ir->roots[left.offset + index] != ir->roots[right.offset + index]) return false;
    }
    return true;
}

static bool sol_ir_tuple_roots_valid(const SolIr *ir, const SolIrExpression *expression,
    bool capability) {
    SolIrSlice aggregate = capability
        ? expression->capability_roots : expression->operation_roots;
    SolIrSlice operands = expression->as.tuple.operands;
    for (size_t root = 0; root < aggregate.count; ++root) {
        SolIrLocalId value = ir->roots[aggregate.offset + root];
        bool found = false;
        for (size_t index = 0; !found && index < operands.count; ++index) {
            const SolIrExpression *operand
                = &ir->expressions[ir->operands[operands.offset + index].value];
            SolIrSlice roots = capability
                ? operand->capability_roots : operand->operation_roots;
            found = sol_ir_root_slice_contains(ir, roots, value);
        }
        if (!found) return false;
    }
    for (size_t index = 0; index < operands.count; ++index) {
        const SolIrExpression *operand
            = &ir->expressions[ir->operands[operands.offset + index].value];
        SolIrSlice roots = capability
            ? operand->capability_roots : operand->operation_roots;
        for (size_t root = 0; root < roots.count; ++root) {
            if (!sol_ir_root_slice_contains(ir, aggregate,
                ir->roots[roots.offset + root])) return false;
        }
    }
    return true;
}

static const SolIrPlace *sol_ir_expression_place(
    const SolIr *ir, SolIrExpressionId expression
) {
    if (expression >= ir->expression_count
        || ir->expressions[expression].kind != SOL_IR_EXPR_PLACE
        || ir->expressions[expression].as.place >= ir->place_count) return NULL;
    return &ir->places[ir->expressions[expression].as.place];
}

static bool sol_ir_local_place(
    const SolIr *ir, SolIrExpressionId expression, SolIrLocalId *local
) {
    const SolIrPlace *place = sol_ir_expression_place(ir, expression);
    if (place == NULL || place->root_kind != SOL_IR_PLACE_ROOT_LOCAL
        || place->local >= ir->local_count) return false;
    if (local != NULL) *local = place->local;
    return true;
}

static bool sol_ir_place_types_valid(
    const SolIr *ir, SolIrExpressionId expression_id
) {
    const SolIrExpression *expression = &ir->expressions[expression_id];
    const SolIrPlace *place = sol_ir_expression_place(ir, expression_id);
    if (place == NULL || place->type >= ir->type_count
        || place->type != expression->type
        || !sol_ir_slice_valid(place->projections, ir->projection_count)
        || (int)place->root_kind < 0
        || place->root_kind > SOL_IR_PLACE_ROOT_TEMPORARY) return false;
    SolIrTypeId current = SOL_IR_NONE;
    if (place->root_kind == SOL_IR_PLACE_ROOT_LOCAL) {
        if (place->local >= ir->local_count || place->temporary != SOL_IR_NONE) return false;
        current = ir->locals[place->local].type;
    } else {
        if (place->local != SOL_IR_NONE || place->temporary >= ir->expression_count
            || place->temporary == expression_id || place->projections.count == 0
            || ir->expressions[place->temporary].kind == SOL_IR_EXPR_PLACE) return false;
        current = ir->expressions[place->temporary].type;
    }
    for (size_t index = 0; index < place->projections.count; ++index) {
        const SolIrProjection *projection
            = &ir->projections[place->projections.offset + index];
        if ((int)projection->kind < 0
            || projection->kind > SOL_IR_PROJECTION_DEREFERENCE
            || projection->type >= ir->type_count) return false;
        if (projection->kind == SOL_IR_PROJECTION_TUPLE_FIELD) {
            if (projection->field != SOL_IR_NONE || projection->index != SOL_IR_NONE
                || current >= ir->type_count) return false;
            const SolIrType *tuple = &ir->types[current];
            if (tuple->kind != SOL_IR_TYPE_TUPLE
                || projection->ordinal >= tuple->argument_count
                || projection->type
                    != ir->type_ids[tuple->argument_offset + projection->ordinal]) {
                return false;
            }
            current = projection->type;
            continue;
        }
        if (projection->kind != SOL_IR_PROJECTION_FIELD
            || projection->index != SOL_IR_NONE || projection->ordinal != SOL_IR_NONE
            || projection->field >= ir->field_count) return false;
        const SolIrField *field = &ir->fields[projection->field];
        if (field->owner >= ir->definition_count
            || ir->definitions[field->owner].kind != SOL_IR_DEFINITION_RECORD
            || !sol_ir_nominal_type(ir, current, field->owner)) return false;
        const SolIrType *base = &ir->types[current];
        const SolIrDefinition *definition = &ir->definitions[field->owner];
        if (!sol_ir_type_matches_instantiation(ir, projection->type, field->type,
            definition->generic_parameters,
            (SolIrSlice){base->argument_offset, base->argument_count}, SOL_IR_NONE, 0)) {
            return false;
        }
        current = projection->type;
    }
    return current == place->type;
}

static bool sol_ir_call_types_valid(const SolIr *ir, const SolIrExpression *expression) {
    const SolIrSlice operands = expression->as.call.operands;
    SolIrSlice generic_parameters = {0};
    SolIrSlice type_arguments = expression->as.call.type_arguments;
    SolIrTypeId self_type = SOL_IR_NONE;
    if (expression->as.call.kind == SOL_IR_CALL_FUNCTION
        || expression->as.call.kind == SOL_IR_CALL_CAPABILITY
        || expression->as.call.kind == SOL_IR_CALL_METHOD) {
        if (expression->as.call.callable >= ir->callable_count) return false;
        const SolIrCallable *callable = &ir->callables[expression->as.call.callable];
        generic_parameters = callable->generic_parameters;
        if (type_arguments.count != generic_parameters.count) return false;
        if (expression->as.call.kind == SOL_IR_CALL_METHOD) {
            if (expression->as.call.receiver >= ir->expression_count) return false;
            self_type = ir->expressions[expression->as.call.receiver].type;
        }
        if (!sol_ir_type_matches_instantiation(ir, expression->type, callable->result,
            generic_parameters, type_arguments, self_type, 0)) return false;
        for (size_t index = 0; index < operands.count; ++index) {
            SolIrLocalId local = ir->roots[callable->parameters.offset + index];
            SolIrExpressionId value = ir->operands[operands.offset + index].value;
            if (local >= ir->local_count || value >= ir->expression_count
                || !sol_ir_type_assignable(ir, ir->expressions[value].type,
                    ir->locals[local].type, generic_parameters, type_arguments,
                    self_type)) return false;
        }
        return true;
    }
    if (expression->as.call.kind == SOL_IR_CALL_CALLBACK) {
        if (type_arguments.count != 0) return false;
        if (expression->as.call.callee >= ir->expression_count) return false;
        SolIrTypeId function_id = ir->expressions[expression->as.call.callee].type;
        if (function_id >= ir->type_count) return false;
        const SolIrType *function = &ir->types[function_id];
        if (function->kind != SOL_IR_TYPE_FUNCTION || function->definition != SOL_IR_NONE
            || expression->type != function->result) return false;
        for (size_t index = 0; index < operands.count; ++index) {
            SolIrExpressionId value = ir->operands[operands.offset + index].value;
            if (value >= ir->expression_count
                || !sol_ir_type_assignable(ir, ir->expressions[value].type,
                    ir->type_ids[function->parameter_offset + index], (SolIrSlice){0},
                    (SolIrSlice){0}, SOL_IR_NONE)) return false;
        }
        return true;
    }
    const SolIrType *result = expression->type < ir->type_count
        ? &ir->types[expression->type] : NULL;
    if (result == NULL || type_arguments.count != 0) return false;
    if (expression->as.call.kind == SOL_IR_CALL_BUILTIN_NONE) {
        return result->kind == SOL_IR_TYPE_OPTION && result->argument_count == 1;
    }
    if (operands.count != 1
        && expression->as.call.kind != SOL_IR_CALL_ENUM_CONSTRUCTOR) return false;
    if (expression->as.call.kind == SOL_IR_CALL_BUILTIN_SOME
        || expression->as.call.kind == SOL_IR_CALL_BUILTIN_OK
        || expression->as.call.kind == SOL_IR_CALL_BUILTIN_ERR) {
        SolIrTypeKind expected_kind = expression->as.call.kind == SOL_IR_CALL_BUILTIN_SOME
            ? SOL_IR_TYPE_OPTION : SOL_IR_TYPE_RESULT;
        size_t ordinal = expression->as.call.kind == SOL_IR_CALL_BUILTIN_ERR ? 1 : 0;
        SolIrExpressionId value = ir->operands[operands.offset].value;
        return result->kind == expected_kind && result->argument_count > ordinal
            && value < ir->expression_count
            && sol_ir_type_assignable(ir, ir->expressions[value].type,
                ir->type_ids[result->argument_offset + ordinal], (SolIrSlice){0},
                (SolIrSlice){0}, SOL_IR_NONE);
    }
    if (expression->as.call.kind == SOL_IR_CALL_ENUM_CONSTRUCTOR) {
        if (expression->as.call.variant >= ir->variant_count) return false;
        const SolIrVariant *variant = &ir->variants[expression->as.call.variant];
        if (variant->owner >= ir->definition_count
            || !sol_ir_nominal_type(ir, expression->type, variant->owner)) return false;
        const SolIrType *nominal = &ir->types[expression->type];
        const SolIrDefinition *definition = &ir->definitions[variant->owner];
        for (size_t index = 0; index < operands.count; ++index) {
            SolIrExpressionId value = ir->operands[operands.offset + index].value;
            SolIrFieldId field = variant->fields.offset + index;
            if (value >= ir->expression_count || field >= ir->field_count
                || !sol_ir_type_assignable(ir, ir->expressions[value].type,
                    ir->fields[field].type, definition->generic_parameters,
                    (SolIrSlice){nominal->argument_offset, nominal->argument_count},
                    SOL_IR_NONE)) return false;
        }
        return true;
    }
    if (expression->as.call.kind == SOL_IR_CALL_DISTINCT_CONSTRUCTOR) {
        SolIrDefinitionId definition_id = expression->as.call.definition;
        if (definition_id >= ir->definition_count
            || ir->definitions[definition_id].kind != SOL_IR_DEFINITION_DISTINCT
            || !sol_ir_nominal_type(ir, expression->type, definition_id)) return false;
        const SolIrDefinition *definition = &ir->definitions[definition_id];
        const SolIrType *nominal = &ir->types[expression->type];
        SolIrExpressionId value = ir->operands[operands.offset].value;
        return value < ir->expression_count
            && sol_ir_type_assignable(ir, ir->expressions[value].type,
                definition->representation, definition->generic_parameters,
                (SolIrSlice){nominal->argument_offset, nominal->argument_count},
                SOL_IR_NONE);
    }
    return false;
}

static bool sol_ir_expression_types_valid(
    const SolIr *ir, SolIrExpressionId id
) {
    if (id >= ir->expression_count) return false;
    const SolIrExpression *expression = &ir->expressions[id];
    switch (expression->kind) {
        case SOL_IR_EXPR_INTEGER:
            return sol_ir_type_is(ir, expression->type, SOL_IR_TYPE_INT64)
                && expression->capability_roots.count == 0
                && expression->operation_roots.count == 0;
        case SOL_IR_EXPR_STRING:
            return sol_ir_type_is(ir, expression->type, SOL_IR_TYPE_TEXT)
                && expression->capability_roots.count == 0
                && expression->operation_roots.count == 0;
        case SOL_IR_EXPR_BOOL:
            return sol_ir_type_is(ir, expression->type, SOL_IR_TYPE_BOOL)
                && expression->capability_roots.count == 0
                && expression->operation_roots.count == 0;
        case SOL_IR_EXPR_UNIT:
            return sol_ir_type_is(ir, expression->type, SOL_IR_TYPE_UNIT)
                && expression->capability_roots.count == 0
                && expression->operation_roots.count == 0;
        case SOL_IR_EXPR_PLACE: {
            if (!sol_ir_place_types_valid(ir, id)) return false;
            const SolIrPlace *place = sol_ir_expression_place(ir, id);
            if (place->root_kind != SOL_IR_PLACE_ROOT_LOCAL) {
                return expression->local_use == SOL_IR_LOCAL_USE_NONE;
            }
            const SolIrLocal *local = &ir->locals[place->local];
            return sol_ir_roots_equal(ir, expression->capability_roots,
                    local->capability_roots)
                && sol_ir_roots_equal(ir, expression->operation_roots,
                    local->operation_roots);
        }
        case SOL_IR_EXPR_DEFINITION: {
            if (expression->as.definition >= ir->definition_count
                || expression->capability_roots.count != 0
                || expression->operation_roots.count != 0) return false;
            const SolIrDefinition *definition
                = &ir->definitions[expression->as.definition];
            if (definition->kind == SOL_IR_DEFINITION_FUNCTION) {
                return sol_ir_definition_expression_valid(ir, id)
                    || (expression->type < ir->type_count
                        && ir->types[expression->type].kind == SOL_IR_TYPE_FUNCTION
                        && ir->types[expression->type].definition
                            == expression->as.definition);
            }
            return expression->type < ir->type_count
                && (ir->types[expression->type].kind == SOL_IR_TYPE_NOMINAL
                    || ir->types[expression->type].kind == SOL_IR_TYPE_SELF)
                && ir->types[expression->type].definition == expression->as.definition;
        }
        case SOL_IR_EXPR_REFINEMENT_SELF:
            return expression->as.definition < ir->definition_count
                && ir->definitions[expression->as.definition].kind
                    == SOL_IR_DEFINITION_REFINED
                && expression->type
                    == ir->definitions[expression->as.definition].representation;
        case SOL_IR_EXPR_UNARY: {
            if (!sol_ir_unary_operator_valid(expression->as.unary.operator_kind)
                || expression->as.unary.operand >= ir->expression_count) return false;
            SolIrTypeKind kind = expression->as.unary.operator_kind == SOL_TOKEN_BANG
                ? SOL_IR_TYPE_BOOL : SOL_IR_TYPE_INT64;
            return sol_ir_type_is(ir, expression->type, kind)
                && sol_ir_type_is(ir,
                    ir->expressions[expression->as.unary.operand].type, kind);
        }
        case SOL_IR_EXPR_BINARY: {
            if (!sol_ir_binary_operator_valid(expression->as.binary.operator_kind)
                || expression->as.binary.left >= ir->expression_count
                || expression->as.binary.right >= ir->expression_count) return false;
            SolIrTypeId left = ir->expressions[expression->as.binary.left].type;
            SolIrTypeId right = ir->expressions[expression->as.binary.right].type;
            SolTokenKind operator_kind = expression->as.binary.operator_kind;
            if (operator_kind == SOL_TOKEN_AMP_AMP || operator_kind == SOL_TOKEN_PIPE_PIPE) {
                return sol_ir_type_is(ir, left, SOL_IR_TYPE_BOOL)
                    && sol_ir_type_is(ir, right, SOL_IR_TYPE_BOOL)
                    && sol_ir_type_is(ir, expression->type, SOL_IR_TYPE_BOOL);
            }
            if (operator_kind == SOL_TOKEN_EQUAL_EQUAL
                || operator_kind == SOL_TOKEN_BANG_EQUAL) {
                return left == right
                    && sol_ir_type_is(ir, expression->type, SOL_IR_TYPE_BOOL);
            }
            if (!sol_ir_type_is(ir, left, SOL_IR_TYPE_INT64)
                || !sol_ir_type_is(ir, right, SOL_IR_TYPE_INT64)) return false;
            bool comparison = operator_kind == SOL_TOKEN_LESS
                || operator_kind == SOL_TOKEN_LESS_EQUAL
                || operator_kind == SOL_TOKEN_GREATER
                || operator_kind == SOL_TOKEN_GREATER_EQUAL;
            return sol_ir_type_is(ir, expression->type,
                comparison ? SOL_IR_TYPE_BOOL : SOL_IR_TYPE_INT64);
        }
        case SOL_IR_EXPR_CALL:
            return sol_ir_call_types_valid(ir, expression);
        case SOL_IR_EXPR_RECORD: {
            SolIrDefinitionId definition_id = expression->as.record.definition;
            if (definition_id >= ir->definition_count
                || !sol_ir_nominal_type(ir, expression->type, definition_id)) return false;
            const SolIrDefinition *definition = &ir->definitions[definition_id];
            if (definition->kind != SOL_IR_DEFINITION_RECORD) {
                return definition->kind == SOL_IR_DEFINITION_CAPABILITY;
            }
            const SolIrType *nominal = &ir->types[expression->type];
            for (size_t index = 0; index < expression->as.record.fields.count; ++index) {
                const SolIrOperand *operand
                    = &ir->operands[expression->as.record.fields.offset + index];
                if (operand->value >= ir->expression_count || operand->formal >= ir->field_count
                    || !sol_ir_type_assignable(ir,
                        ir->expressions[operand->value].type,
                        ir->fields[operand->formal].type, definition->generic_parameters,
                        (SolIrSlice){nominal->argument_offset, nominal->argument_count},
                        SOL_IR_NONE)) return false;
            }
            return true;
        }
        case SOL_IR_EXPR_TUPLE: {
            if (expression->type >= ir->type_count) return false;
            const SolIrType *tuple = &ir->types[expression->type];
            if (tuple->kind != SOL_IR_TYPE_TUPLE
                || expression->as.tuple.operands.count != tuple->argument_count) return false;
            for (size_t index = 0; index < tuple->argument_count; ++index) {
                const SolIrOperand *operand
                    = &ir->operands[expression->as.tuple.operands.offset + index];
                if (operand->formal != index || operand->access != SOL_ACCESS_OWNED
                    || operand->value >= ir->expression_count
                    || !sol_ir_type_assignable(ir, ir->expressions[operand->value].type,
                        ir->type_ids[tuple->argument_offset + index],
                        (SolIrSlice){0}, (SolIrSlice){0}, SOL_IR_NONE)) return false;
            }
            return sol_ir_tuple_roots_valid(ir, expression, true)
                && sol_ir_tuple_roots_valid(ir, expression, false);
        }
        case SOL_IR_EXPR_VARIANT:
            return expression->as.variant.variant < ir->variant_count
                && ir->variants[expression->as.variant.variant].owner
                    < ir->definition_count
                && ir->definitions[ir->variants[expression->as.variant.variant].owner].kind
                    == SOL_IR_DEFINITION_ENUM
                && sol_ir_nominal_type(ir, expression->type,
                    ir->variants[expression->as.variant.variant].owner);
        case SOL_IR_EXPR_IF:
            return expression->as.if_expr.condition < ir->expression_count
                && expression->as.if_expr.then_branch < ir->expression_count
                && expression->as.if_expr.else_branch < ir->expression_count
                && sol_ir_type_is(ir,
                    ir->expressions[expression->as.if_expr.condition].type,
                    SOL_IR_TYPE_BOOL)
                && sol_ir_type_assignable(ir,
                    ir->expressions[expression->as.if_expr.then_branch].type,
                    expression->type, (SolIrSlice){0}, (SolIrSlice){0}, SOL_IR_NONE)
                && sol_ir_type_assignable(ir,
                    ir->expressions[expression->as.if_expr.else_branch].type,
                    expression->type, (SolIrSlice){0}, (SolIrSlice){0}, SOL_IR_NONE);
        case SOL_IR_EXPR_MATCH: {
            if (expression->as.match_expr.scrutinee >= ir->expression_count) return false;
            SolIrTypeId scrutinee_id
                = ir->expressions[expression->as.match_expr.scrutinee].type;
            if (scrutinee_id >= ir->type_count) return false;
            for (size_t index = 0; index < expression->as.match_expr.arms.count; ++index) {
                SolIrArmId arm_id = ir->arm_ids[expression->as.match_expr.arms.offset + index];
                if (arm_id >= ir->arm_count || ir->arms[arm_id].body >= ir->expression_count
                    || ir->arms[arm_id].pattern >= ir->pattern_count
                    || ir->patterns[ir->arms[arm_id].pattern].type != scrutinee_id
                    || (ir->arms[arm_id].guard != SOL_IR_NONE
                        && (ir->arms[arm_id].guard >= ir->expression_count
                            || !sol_ir_type_is(ir,
                                ir->expressions[ir->arms[arm_id].guard].type,
                                SOL_IR_TYPE_BOOL)))
                    || !sol_ir_type_assignable(ir, ir->expressions[ir->arms[arm_id].body].type,
                        expression->type, (SolIrSlice){0}, (SolIrSlice){0},
                        SOL_IR_NONE)) return false;
            }
            return true;
        }
        case SOL_IR_EXPR_HANDLE:
            return expression->as.handler.body < ir->expression_count
                && sol_ir_type_assignable(ir,
                    ir->expressions[expression->as.handler.body].type, expression->type,
                    (SolIrSlice){0}, (SolIrSlice){0}, SOL_IR_NONE);
        case SOL_IR_EXPR_SNAPSHOT_READ:
            return expression->as.snapshot < ir->snapshot_count
                && expression->type == ir->snapshots[expression->as.snapshot].type;
        case SOL_IR_EXPR_COMPILE_TIME_HEAD:
            return (expression->type == SOL_IR_NONE || expression->type < ir->type_count)
                && expression->local_use == SOL_IR_LOCAL_USE_NONE
                && expression->capability_roots.count == 0
                && expression->operation_roots.count == 0;
        case SOL_IR_EXPR_BLOCK:
        case SOL_IR_EXPR_PROPAGATE:
        case SOL_IR_EXPR_RESULT:
        case SOL_IR_EXPR_BOUND_OPERATION:
            return true;
    }
    return false;
}

static bool sol_ir_proof_expression_non_executable(
    const SolIr *ir,
    SolIrExpressionId id,
    SolIrCallableId callable_id,
    const unsigned char *runtime_states,
    unsigned char *proof_states,
    SolIrCallableId *proof_callables,
    SolIrCallableId *local_callables,
    bool *available,
    size_t depth
);

static bool sol_ir_executable_expression(
    const SolIr *ir,
    SolIrExpressionId id,
    SolIrDefinitionId owner,
    SolIrCallableId callable_id,
    SolIrTypeId callable_result,
    unsigned char *states,
    SolIrCallableId *statement_callables,
    SolIrCallableId *local_callables,
    bool *introduced,
    size_t loop_depth,
    bool *loop_break
) {
    if (id >= ir->expression_count || states[id] != 0) return false;
    states[id] = 1;
    const SolIrExpression *expression = &ir->expressions[id];
    SolIrSlice provenance[2] = {expression->capability_roots,
        expression->operation_roots};
    for (size_t slice = 0; slice < 2; ++slice) {
        for (size_t index = 0; index < provenance[slice].count; ++index) {
            SolIrLocalId root = ir->roots[provenance[slice].offset + index];
            if (root >= ir->local_count || ir->locals[root].owner != owner
                || callable_id >= ir->callable_count) return false;
            const SolIrCallable *callable = &ir->callables[callable_id];
            bool belongs = root == callable->receiver || root == callable->capability_source;
            for (size_t parameter = 0; !belongs
                && parameter < callable->parameters.count; ++parameter) {
                belongs = ir->roots[callable->parameters.offset + parameter] == root;
            }
            if (!belongs) return false;
        }
    }
#define SOL_IR_EXEC(child) \
    do { if (!sol_ir_executable_expression( \
        ir, (child), owner, callable_id, callable_result, states, \
        statement_callables, local_callables, introduced, \
        loop_depth, loop_break)) \
        return false; } while (0)
    switch (expression->kind) {
        case SOL_IR_EXPR_REFINEMENT_SELF:
        case SOL_IR_EXPR_RESULT:
        case SOL_IR_EXPR_SNAPSHOT_READ:
        case SOL_IR_EXPR_COMPILE_TIME_HEAD:
            return false;
        case SOL_IR_EXPR_DEFINITION: {
            if (!sol_ir_definition_expression_valid(ir, id)) return false;
            break;
        }
        case SOL_IR_EXPR_PLACE: {
            const SolIrPlace *place = sol_ir_expression_place(ir, id);
            if (place == NULL) return false;
            if (place->root_kind == SOL_IR_PLACE_ROOT_LOCAL) {
                if (place->local >= ir->local_count
                    || ir->locals[place->local].owner != owner
                    || (local_callables[place->local] != SOL_IR_NONE
                        && local_callables[place->local] != callable_id)) return false;
            } else {
                SOL_IR_EXEC(place->temporary);
            }
            break;
        }
        case SOL_IR_EXPR_BOUND_OPERATION:
            SOL_IR_EXEC(expression->as.operation.receiver);
            break;
        case SOL_IR_EXPR_UNARY:
            SOL_IR_EXEC(expression->as.unary.operand);
            break;
        case SOL_IR_EXPR_BINARY:
            SOL_IR_EXEC(expression->as.binary.left);
            SOL_IR_EXEC(expression->as.binary.right);
            break;
        case SOL_IR_EXPR_CALL:
            if (expression->as.call.effect_parameter != SOL_IR_NONE
                && ir->effect_parameters[expression->as.call.effect_parameter].owner
                    != owner) return false;
            for (size_t index = 0; index < expression->as.call.effects.count; ++index) {
                const SolIrEffect *effect
                    = &ir->effects[expression->as.call.effects.offset + index];
                if (effect->authority_kind == SOL_IR_AUTHORITY_LOCAL
                    && ir->locals[effect->authority].owner != owner) return false;
            }
            if (expression->as.call.kind == SOL_IR_CALL_CALLBACK
                || expression->as.call.kind == SOL_IR_CALL_CAPABILITY) {
                SOL_IR_EXEC(expression->as.call.callee);
            } else if (expression->as.call.kind == SOL_IR_CALL_METHOD) {
                SOL_IR_EXEC(expression->as.call.receiver);
            }
            for (size_t index = 0; index < expression->as.call.operands.count; ++index) {
                SOL_IR_EXEC(ir->operands[expression->as.call.operands.offset + index].value);
            }
            break;
        case SOL_IR_EXPR_RECORD:
            for (size_t index = 0; index < expression->as.record.fields.count; ++index) {
                SOL_IR_EXEC(ir->operands[expression->as.record.fields.offset + index].value);
            }
            break;
        case SOL_IR_EXPR_TUPLE:
            for (size_t index = 0; index < expression->as.tuple.operands.count; ++index) {
                SOL_IR_EXEC(ir->operands[expression->as.tuple.operands.offset + index].value);
            }
            break;
        case SOL_IR_EXPR_IF:
            SOL_IR_EXEC(expression->as.if_expr.condition);
            SOL_IR_EXEC(expression->as.if_expr.then_branch);
            SOL_IR_EXEC(expression->as.if_expr.else_branch);
            break;
        case SOL_IR_EXPR_MATCH:
            SOL_IR_EXEC(expression->as.match_expr.scrutinee);
            for (size_t index = 0; index < expression->as.match_expr.arms.count; ++index) {
                const SolIrArm *arm = &ir->arms[ir->arm_ids[
                    expression->as.match_expr.arms.offset + index]];
                for (size_t binding = 0; binding < arm->bindings.count; ++binding) {
                    SolIrLocalId local = ir->roots[arm->bindings.offset + binding];
                    if (local >= ir->local_count || ir->locals[local].owner != owner) {
                        return false;
                    }
                    if (local_callables[local] != SOL_IR_NONE
                        && local_callables[local] != callable_id) return false;
                    local_callables[local] = callable_id;
                    introduced[local] = true;
                }
                if (arm->guard != SOL_IR_NONE) SOL_IR_EXEC(arm->guard);
                SOL_IR_EXEC(arm->body);
                for (size_t binding = 0; binding < arm->bindings.count; ++binding) {
                    introduced[ir->roots[arm->bindings.offset + binding]] = false;
                }
            }
            break;
        case SOL_IR_EXPR_BLOCK:
            {
            bool terminated = false;
            bool computed_unit = true;
            bool computed_never = false;
            SolIrTypeId computed = SOL_IR_NONE;
            for (size_t index = 0; index < expression->as.block.statements.count; ++index) {
                SolIrStatementId statement_id = ir->statement_ids[
                    expression->as.block.statements.offset + index];
                const SolIrStatement *statement = &ir->statements[statement_id];
                if (statement_callables[statement_id] != SOL_IR_NONE
                    && statement_callables[statement_id] != callable_id) return false;
                statement_callables[statement_id] = callable_id;
                bool unreachable_loop_transfer = terminated && loop_break != NULL;
                bool saved_loop_break = unreachable_loop_transfer ? *loop_break : false;
                bool saw_break = false;
                if ((statement->kind == SOL_IR_STATEMENT_BREAK
                        || statement->kind == SOL_IR_STATEMENT_CONTINUE)
                    && loop_depth == 0) return false;
                if ((statement->kind == SOL_IR_STATEMENT_LET
                        || statement->kind == SOL_IR_STATEMENT_DECLARE)
                    && (statement->local >= ir->local_count
                        || ir->locals[statement->local].owner != owner)) return false;
                if (statement->kind == SOL_IR_STATEMENT_LET
                    || statement->kind == SOL_IR_STATEMENT_DECLARE) {
                    if (local_callables[statement->local] != SOL_IR_NONE
                        && local_callables[statement->local] != callable_id) return false;
                    local_callables[statement->local] = callable_id;
                }
                if (statement->kind == SOL_IR_STATEMENT_ASSIGNMENT) {
                    SolIrLocalId target_local = SOL_IR_NONE;
                    if (!sol_ir_local_place(ir, statement->target,
                            &target_local)
                        || !introduced[target_local]
                        || ir->locals[target_local].owner != owner) return false;
                    SOL_IR_EXEC(statement->target);
                }
                if (statement->kind == SOL_IR_STATEMENT_MODIFY) {
                    SOL_IR_EXEC(statement->target);
                }
                if (statement->kind == SOL_IR_STATEMENT_LOOP
                    || statement->kind == SOL_IR_STATEMENT_WHILE) {
                    SolIrSlice obligations = statement->loop_obligations;
                    unsigned char *proof_states = ir->expression_count == 0 ? NULL
                        : calloc(ir->expression_count, 1);
                    SolIrCallableId *proof_callables = ir->expression_count == 0 ? NULL
                        : malloc(ir->expression_count * sizeof(*proof_callables));
                    if (ir->expression_count != 0
                        && (proof_states == NULL || proof_callables == NULL)) {
                        free(proof_states);
                        free(proof_callables);
                        return false;
                    }
                    for (size_t proof = 0; proof < ir->expression_count; ++proof) {
                        proof_callables[proof] = SOL_IR_NONE;
                    }
                    for (size_t obligation = 0; obligation < obligations.count;
                        obligation += 2) {
                        const SolIrLoopObligation *entry
                            = &ir->loop_obligations[obligations.offset + obligation];
                        if (entry->callable != callable_id
                            || !sol_ir_proof_expression_non_executable(ir,
                                entry->expression, callable_id, states, proof_states,
                                proof_callables, local_callables, introduced, 0)) {
                            free(proof_states);
                            free(proof_callables);
                            return false;
                        }
                    }
                    free(proof_states);
                    free(proof_callables);
                    if (statement->kind == SOL_IR_STATEMENT_WHILE) {
                        if (!sol_ir_executable_expression(ir, statement->condition,
                            owner, callable_id, callable_result, states,
                            statement_callables, local_callables, introduced,
                            loop_depth + 1, &saw_break)) return false;
                    }
                        if (!sol_ir_executable_expression(ir, statement->expression,
                            owner, callable_id, callable_result, states,
                            statement_callables, local_callables, introduced,
                            loop_depth + 1, &saw_break)) return false;
                } else if (statement->kind == SOL_IR_STATEMENT_UNREACHABLE) {
                    SolIrSlice obligations = statement->unreachable_obligations;
                    if (obligations.count != 1) return false;
                    unsigned char *proof_states = ir->expression_count == 0 ? NULL
                        : calloc(ir->expression_count, 1);
                    SolIrCallableId *proof_callables = ir->expression_count == 0
                        ? NULL : malloc(ir->expression_count
                            * sizeof(*proof_callables));
                    if (ir->expression_count != 0
                        && (proof_states == NULL || proof_callables == NULL)) {
                        free(proof_states);
                        free(proof_callables);
                        return false;
                    }
                    for (size_t proof = 0; proof < ir->expression_count; ++proof) {
                        proof_callables[proof] = SOL_IR_NONE;
                    }
                    const SolIrUnreachableObligation *obligation
                        = &ir->unreachable_obligations[obligations.offset];
                    bool valid = obligation->callable == callable_id
                        && sol_ir_proof_expression_non_executable(ir,
                            obligation->proof, callable_id, states, proof_states,
                            proof_callables, local_callables, introduced, 0);
                    free(proof_states);
                    free(proof_callables);
                    if (!valid) return false;
                } else if (statement->kind != SOL_IR_STATEMENT_DECLARE
                    && statement->kind != SOL_IR_STATEMENT_BREAK
                    && statement->kind != SOL_IR_STATEMENT_CONTINUE) {
                    if (statement->kind == SOL_IR_STATEMENT_REQUIRE) {
                        SOL_IR_EXEC(statement->condition);
                    }
                    SOL_IR_EXEC(statement->expression);
                }
                SolIrTypeId value_type = statement->expression == SOL_IR_NONE
                    ? SOL_IR_NONE : ir->expressions[statement->expression].type;
                if (statement->kind == SOL_IR_STATEMENT_LET
                    && !sol_ir_type_assignable(ir, value_type,
                        ir->locals[statement->local].type, (SolIrSlice){0},
                        (SolIrSlice){0}, SOL_IR_NONE)) return false;
                if (statement->kind == SOL_IR_STATEMENT_LET
                    && !sol_ir_type_is(ir, value_type, SOL_IR_TYPE_NEVER)
                    && (!sol_ir_roots_equal(ir,
                            ir->expressions[statement->expression].capability_roots,
                            ir->locals[statement->local].capability_roots)
                        || !sol_ir_roots_equal(ir,
                            ir->expressions[statement->expression].operation_roots,
                            ir->locals[statement->local].operation_roots))) return false;
                if (statement->kind == SOL_IR_STATEMENT_RETURN
                    && !sol_ir_type_assignable(ir, value_type, callable_result,
                        (SolIrSlice){0}, (SolIrSlice){0}, SOL_IR_NONE)) return false;
                if (!terminated) {
                    if (statement->kind == SOL_IR_STATEMENT_RETURN
                        || statement->kind == SOL_IR_STATEMENT_BREAK
                        || statement->kind == SOL_IR_STATEMENT_CONTINUE
                        || statement->kind == SOL_IR_STATEMENT_PANIC
                        || statement->kind == SOL_IR_STATEMENT_UNREACHABLE) {
                        if ((statement->kind == SOL_IR_STATEMENT_BREAK
                                || statement->kind == SOL_IR_STATEMENT_CONTINUE)
                            && loop_depth == 0) return false;
                        if (statement->kind == SOL_IR_STATEMENT_BREAK
                            && loop_break != NULL) *loop_break = true;
                        computed = SOL_IR_NONE;
                        computed_unit = false;
                        computed_never = true;
                        terminated = true;
                    } else if (statement->kind == SOL_IR_STATEMENT_REQUIRE) {
                        computed = SOL_IR_NONE;
                        computed_unit = true;
                        computed_never = false;
                    } else if (statement->kind == SOL_IR_STATEMENT_WHILE) {
                        computed = SOL_IR_NONE;
                        computed_unit = !sol_ir_type_is(
                            ir, ir->expressions[statement->condition].type,
                            SOL_IR_TYPE_NEVER) || saw_break;
                        computed_never = !computed_unit;
                        terminated = computed_never;
                    } else if (statement->kind == SOL_IR_STATEMENT_LOOP) {
                        computed = SOL_IR_NONE;
                        computed_unit = saw_break;
                        computed_never = !saw_break;
                        terminated = !saw_break;
                    } else if (sol_ir_type_is(ir, value_type, SOL_IR_TYPE_NEVER)) {
                        computed = value_type;
                        computed_unit = false;
                        computed_never = true;
                        terminated = true;
                    } else if (statement->kind == SOL_IR_STATEMENT_EXPRESSION) {
                        computed = value_type;
                        computed_unit = false;
                    } else {
                        computed = SOL_IR_NONE;
                        computed_unit = true;
                    }
                }
                if (statement->kind == SOL_IR_STATEMENT_LET
                    || statement->kind == SOL_IR_STATEMENT_DECLARE) {
                    introduced[statement->local] = true;
                }
                if (unreachable_loop_transfer) *loop_break = saved_loop_break;
            }
            for (size_t index = 0; index < expression->as.block.statements.count; ++index) {
                const SolIrStatement *statement = &ir->statements[ir->statement_ids[
                    expression->as.block.statements.offset + index]];
                if (statement->kind == SOL_IR_STATEMENT_LET
                    || statement->kind == SOL_IR_STATEMENT_DECLARE) {
                    introduced[statement->local] = false;
                }
            }
            if ((computed_unit && !sol_ir_type_is(ir, expression->type, SOL_IR_TYPE_UNIT))
                || (computed_never
                    && !sol_ir_type_is(ir, expression->type, SOL_IR_TYPE_NEVER))
                || (!computed_unit && !computed_never && expression->type != computed)) {
                return false;
            }
            break;
            }
        case SOL_IR_EXPR_PROPAGATE:
            if (expression->as.propagate.operand >= ir->expression_count
                || callable_result >= ir->type_count) return false;
            {
            const SolIrType *result = &ir->types[callable_result];
            if ((expression->as.propagate.kind == SOL_IR_PROPAGATE_OPTION
                    && result->kind != SOL_IR_TYPE_OPTION)
                || (expression->as.propagate.kind == SOL_IR_PROPAGATE_RESULT
                    && (result->kind != SOL_IR_TYPE_RESULT
                        || result->argument_count != 2
                        || ir->type_ids[result->argument_offset + 1]
                            != expression->as.propagate.residual))) return false;
            }
            SOL_IR_EXEC(expression->as.propagate.operand);
            break;
        case SOL_IR_EXPR_HANDLE:
            SOL_IR_EXEC(expression->as.handler.authority);
            SOL_IR_EXEC(expression->as.handler.provider);
            SOL_IR_EXEC(expression->as.handler.body);
            break;
        default: break;
    }
#undef SOL_IR_EXEC
    states[id] = 2;
    return true;
}

static bool sol_ir_validate_arena_ownership(
    const SolIr *ir, SolDiagnostics *diagnostics
) {
    size_t *statement_slots = ir->statement_id_count == 0 ? NULL
        : calloc(ir->statement_id_count, sizeof(*statement_slots));
    size_t *statements = ir->statement_count == 0 ? NULL
        : calloc(ir->statement_count, sizeof(*statements));
    size_t *arm_slots = ir->arm_id_count == 0 ? NULL
        : calloc(ir->arm_id_count, sizeof(*arm_slots));
    size_t *arms = ir->arm_count == 0 ? NULL
        : calloc(ir->arm_count, sizeof(*arms));
    size_t *cleanups = ir->cleanup_local_count == 0 ? NULL
        : calloc(ir->cleanup_local_count, sizeof(*cleanups));
    size_t *places = ir->place_count == 0 ? NULL
        : calloc(ir->place_count, sizeof(*places));
    size_t *projections = ir->projection_count == 0 ? NULL
        : calloc(ir->projection_count, sizeof(*projections));
    size_t *operands = ir->operand_count == 0 ? NULL
        : calloc(ir->operand_count, sizeof(*operands));
    if ((ir->statement_id_count != 0 && statement_slots == NULL)
        || (ir->statement_count != 0 && statements == NULL)
        || (ir->arm_id_count != 0 && arm_slots == NULL)
        || (ir->arm_count != 0 && arms == NULL)
        || (ir->cleanup_local_count != 0 && cleanups == NULL)
        || (ir->place_count != 0 && places == NULL)
        || (ir->projection_count != 0 && projections == NULL)
        || (ir->operand_count != 0 && operands == NULL)) {
        free(statement_slots); free(statements); free(arm_slots); free(arms);
        free(cleanups); free(places); free(projections); free(operands);
        return sol_ir_error(diagnostics, "IR arena ownership allocation failed");
    }
    for (size_t index = 0; index < ir->expression_count; ++index) {
        const SolIrExpression *expression = &ir->expressions[index];
        if (expression->kind == SOL_IR_EXPR_PLACE) {
            if (expression->as.place >= ir->place_count) {
                free(statement_slots); free(statements); free(arm_slots); free(arms);
                free(cleanups); free(places); free(projections); free(operands);
                return sol_ir_error(diagnostics, "IR place ID is out of range");
            }
            const SolIrPlace *place = &ir->places[expression->as.place];
            ++places[expression->as.place];
            for (size_t slot = 0; slot < place->projections.count; ++slot) {
                ++projections[place->projections.offset + slot];
            }
        } else if (expression->kind == SOL_IR_EXPR_BLOCK) {
            for (size_t slot = 0; slot < expression->as.block.statements.count; ++slot) {
                size_t position = expression->as.block.statements.offset + slot;
                ++statement_slots[position];
                ++statements[ir->statement_ids[position]];
            }
            for (size_t slot = 0; slot < expression->as.block.cleanup.count; ++slot) {
                ++cleanups[expression->as.block.cleanup.offset + slot];
            }
        } else if (expression->kind == SOL_IR_EXPR_MATCH) {
            for (size_t slot = 0; slot < expression->as.match_expr.arms.count; ++slot) {
                size_t position = expression->as.match_expr.arms.offset + slot;
                ++arm_slots[position];
                ++arms[ir->arm_ids[position]];
            }
        }
        SolIrSlice operand_slice = {0};
        if (expression->kind == SOL_IR_EXPR_CALL) {
            operand_slice = expression->as.call.operands;
        } else if (expression->kind == SOL_IR_EXPR_RECORD) {
            operand_slice = expression->as.record.fields;
        } else if (expression->kind == SOL_IR_EXPR_TUPLE) {
            operand_slice = expression->as.tuple.operands;
        }
        if (!sol_ir_slice_valid(operand_slice, ir->operand_count)) {
            free(statement_slots); free(statements); free(arm_slots); free(arms);
            free(cleanups); free(places); free(projections); free(operands);
            return sol_ir_error(diagnostics, "IR operand slice is out of range");
        }
        for (size_t slot = 0; slot < operand_slice.count; ++slot) {
            ++operands[operand_slice.offset + slot];
        }
    }
    for (size_t index = 0; index < ir->arm_count; ++index) {
        for (size_t slot = 0; slot < ir->arms[index].cleanup.count; ++slot) {
            ++cleanups[ir->arms[index].cleanup.offset + slot];
        }
    }
    bool valid = true;
    for (size_t index = 0; valid && index < ir->statement_id_count; ++index) {
        valid = statement_slots[index] == 1;
    }
    for (size_t index = 0; valid && index < ir->statement_count; ++index) {
        valid = statements[index] == 1;
    }
    for (size_t index = 0; valid && index < ir->arm_id_count; ++index) {
        valid = arm_slots[index] == 1;
    }
    for (size_t index = 0; valid && index < ir->arm_count; ++index) {
        valid = arms[index] == 1;
    }
    for (size_t index = 0; valid && index < ir->cleanup_local_count; ++index) {
        valid = cleanups[index] == 1;
    }
    for (size_t index = 0; valid && index < ir->place_count; ++index) {
        valid = places[index] == 1;
    }
    for (size_t index = 0; valid && index < ir->projection_count; ++index) {
        valid = projections[index] == 1;
    }
    for (size_t index = 0; valid && index < ir->operand_count; ++index) {
        valid = operands[index] == 1;
    }
    free(statement_slots); free(statements); free(arm_slots); free(arms);
    free(cleanups); free(places); free(projections); free(operands);
    return valid || sol_ir_error(diagnostics,
        "IR statement, arm, cleanup, place, projection, or operand entry is orphaned or shared");
}

static bool sol_ir_proof_call_is_pure(
    const SolIr *ir,
    const SolIrExpression *expression
) {
    if (expression->as.call.effects.count != 0
        || expression->as.call.effect_parameter != SOL_IR_NONE) return false;
    if (expression->as.call.kind == SOL_IR_CALL_CALLBACK) {
        SolIrTypeId type = ir->expressions[expression->as.call.callee].type;
        return type < ir->type_count && ir->types[type].kind == SOL_IR_TYPE_FUNCTION
            && ir->types[type].effects.count == 0
            && ir->types[type].effect_parameter == SOL_IR_NONE;
    }
    bool callable_call = expression->as.call.kind == SOL_IR_CALL_FUNCTION
        || expression->as.call.kind == SOL_IR_CALL_CAPABILITY
        || expression->as.call.kind == SOL_IR_CALL_METHOD;
    if (!callable_call) return true;
    if (expression->as.call.callable >= ir->callable_count) return false;
    const SolIrCallable *target = &ir->callables[expression->as.call.callable];
    if (target->effects.count != 0) return false;
    if (target->effect_parameter == SOL_IR_NONE) return true;
    bool determined = false;
    for (size_t formal = 0; formal < target->parameters.count; ++formal) {
        SolIrLocalId parameter = ir->roots[target->parameters.offset + formal];
        SolIrTypeId formal_type = ir->locals[parameter].type;
        if (formal_type >= ir->type_count
            || ir->types[formal_type].kind != SOL_IR_TYPE_FUNCTION
            || ir->types[formal_type].effect_parameter
                != target->effect_parameter) continue;
        SolIrExpressionId actual = SOL_IR_NONE;
        for (size_t operand = 0; operand < expression->as.call.operands.count;
            ++operand) {
            const SolIrOperand *entry
                = &ir->operands[expression->as.call.operands.offset + operand];
            if (entry->formal == formal) actual = entry->value;
        }
        if (actual >= ir->expression_count) return false;
        SolIrTypeId actual_type = ir->expressions[actual].type;
        if (actual_type >= ir->type_count
            || ir->types[actual_type].kind != SOL_IR_TYPE_FUNCTION
            || ir->types[actual_type].effects.count != 0
            || ir->types[actual_type].effect_parameter != SOL_IR_NONE) return false;
        determined = true;
    }
    return determined;
}

static bool sol_ir_guard_expression_pure(const SolIr *ir,
    SolIrExpressionId id, unsigned char *states, size_t depth) {
    if (id >= ir->expression_count || depth >= 256 || states[id] == 1) return false;
    if (states[id] == 2) return true;
    states[id] = 1;
#define SOL_IR_GUARD(child) \
    do { if (!sol_ir_guard_expression_pure( \
        ir, (child), states, depth + 1)) return false; } while (0)
    const SolIrExpression *expression = &ir->expressions[id];
    if (expression->local_use == SOL_IR_LOCAL_USE_EXCLUSIVE
        || expression->local_use == SOL_IR_LOCAL_USE_UPDATE) return false;
    switch (expression->kind) {
        case SOL_IR_EXPR_PLACE: {
            const SolIrPlace *place = &ir->places[expression->as.place];
            if (place->root_kind == SOL_IR_PLACE_ROOT_TEMPORARY) {
                SOL_IR_GUARD(place->temporary);
            }
            for (size_t index = 0; index < place->projections.count; ++index) {
                const SolIrProjection *projection
                    = &ir->projections[place->projections.offset + index];
                if (projection->kind == SOL_IR_PROJECTION_INDEX) {
                    SOL_IR_GUARD(projection->index);
                }
            }
            break;
        }
        case SOL_IR_EXPR_UNARY:
            SOL_IR_GUARD(expression->as.unary.operand);
            break;
        case SOL_IR_EXPR_BINARY:
            SOL_IR_GUARD(expression->as.binary.left);
            SOL_IR_GUARD(expression->as.binary.right);
            break;
        case SOL_IR_EXPR_CALL:
            if (!sol_ir_proof_call_is_pure(ir, expression)) return false;
            if (expression->as.call.kind == SOL_IR_CALL_CALLBACK
                || expression->as.call.kind == SOL_IR_CALL_CAPABILITY) {
                SOL_IR_GUARD(expression->as.call.callee);
            } else if (expression->as.call.kind == SOL_IR_CALL_METHOD) {
                if (expression->as.call.receiver_access == SOL_ACCESS_EXCLUSIVE) {
                    return false;
                }
                SOL_IR_GUARD(expression->as.call.receiver);
            }
            for (size_t index = 0; index < expression->as.call.operands.count; ++index) {
                const SolIrOperand *operand
                    = &ir->operands[expression->as.call.operands.offset + index];
                if (operand->access == SOL_ACCESS_EXCLUSIVE) return false;
                SOL_IR_GUARD(operand->value);
            }
            break;
        case SOL_IR_EXPR_RECORD:
            for (size_t index = 0; index < expression->as.record.fields.count; ++index) {
                SOL_IR_GUARD(ir->operands[
                    expression->as.record.fields.offset + index].value);
            }
            break;
        case SOL_IR_EXPR_TUPLE:
            for (size_t index = 0; index < expression->as.tuple.operands.count; ++index) {
                SOL_IR_GUARD(ir->operands[
                    expression->as.tuple.operands.offset + index].value);
            }
            break;
        case SOL_IR_EXPR_BOUND_OPERATION:
            SOL_IR_GUARD(expression->as.operation.receiver);
            break;
        case SOL_IR_EXPR_IF:
            SOL_IR_GUARD(expression->as.if_expr.condition);
            SOL_IR_GUARD(expression->as.if_expr.then_branch);
            SOL_IR_GUARD(expression->as.if_expr.else_branch);
            break;
        case SOL_IR_EXPR_MATCH:
            SOL_IR_GUARD(expression->as.match_expr.scrutinee);
            for (size_t index = 0; index < expression->as.match_expr.arms.count; ++index) {
                const SolIrArm *arm = &ir->arms[ir->arm_ids[
                    expression->as.match_expr.arms.offset + index]];
                if (arm->guard != SOL_IR_NONE) SOL_IR_GUARD(arm->guard);
                SOL_IR_GUARD(arm->body);
            }
            break;
        case SOL_IR_EXPR_BLOCK:
            for (size_t index = 0; index < expression->as.block.statements.count; ++index) {
                const SolIrStatement *statement = &ir->statements[ir->statement_ids[
                    expression->as.block.statements.offset + index]];
                if (statement->kind != SOL_IR_STATEMENT_LET
                    && statement->kind != SOL_IR_STATEMENT_EXPRESSION) return false;
                SOL_IR_GUARD(statement->expression);
            }
            break;
        case SOL_IR_EXPR_PROPAGATE:
        case SOL_IR_EXPR_HANDLE:
        case SOL_IR_EXPR_RESULT:
        case SOL_IR_EXPR_SNAPSHOT_READ:
        case SOL_IR_EXPR_REFINEMENT_SELF:
        case SOL_IR_EXPR_COMPILE_TIME_HEAD:
            return false;
        default:
            break;
    }
#undef SOL_IR_GUARD
    states[id] = 2;
    return true;
}

static bool sol_ir_expression_reaches(
    const SolIr *ir,
    SolIrExpressionId id,
    SolIrExpressionId target,
    unsigned char *states,
    size_t depth
) {
    if (id == target) return true;
    if (depth >= 256) return true;
    if (id >= ir->expression_count || states[id] != 0) return false;
    states[id] = 1;
#define SOL_IR_REACHES(child) \
    do { if (sol_ir_expression_reaches( \
        ir, (child), target, states, depth + 1)) return true; } while (0)
    const SolIrExpression *expression = &ir->expressions[id];
    switch (expression->kind) {
        case SOL_IR_EXPR_PLACE: {
            const SolIrPlace *place = &ir->places[expression->as.place];
            if (place->root_kind == SOL_IR_PLACE_ROOT_TEMPORARY) {
                SOL_IR_REACHES(place->temporary);
            }
            for (size_t index = 0; index < place->projections.count; ++index) {
                const SolIrProjection *projection
                    = &ir->projections[place->projections.offset + index];
                if (projection->kind == SOL_IR_PROJECTION_INDEX) {
                    SOL_IR_REACHES(projection->index);
                }
            }
            break;
        }
        case SOL_IR_EXPR_UNARY:
            SOL_IR_REACHES(expression->as.unary.operand);
            break;
        case SOL_IR_EXPR_BINARY:
            SOL_IR_REACHES(expression->as.binary.left);
            SOL_IR_REACHES(expression->as.binary.right);
            break;
        case SOL_IR_EXPR_CALL:
            if (expression->as.call.kind == SOL_IR_CALL_CALLBACK
                || expression->as.call.kind == SOL_IR_CALL_CAPABILITY) {
                SOL_IR_REACHES(expression->as.call.callee);
            } else if (expression->as.call.kind == SOL_IR_CALL_METHOD) {
                SOL_IR_REACHES(expression->as.call.receiver);
            }
            for (size_t index = 0; index < expression->as.call.operands.count; ++index) {
                SOL_IR_REACHES(ir->operands[
                    expression->as.call.operands.offset + index].value);
            }
            break;
        case SOL_IR_EXPR_RECORD:
            for (size_t index = 0; index < expression->as.record.fields.count; ++index) {
                SOL_IR_REACHES(ir->operands[
                    expression->as.record.fields.offset + index].value);
            }
            break;
        case SOL_IR_EXPR_TUPLE:
            for (size_t index = 0; index < expression->as.tuple.operands.count; ++index) {
                SOL_IR_REACHES(ir->operands[
                    expression->as.tuple.operands.offset + index].value);
            }
            break;
        case SOL_IR_EXPR_BOUND_OPERATION:
            SOL_IR_REACHES(expression->as.operation.receiver);
            break;
        case SOL_IR_EXPR_IF:
            SOL_IR_REACHES(expression->as.if_expr.condition);
            SOL_IR_REACHES(expression->as.if_expr.then_branch);
            SOL_IR_REACHES(expression->as.if_expr.else_branch);
            break;
        case SOL_IR_EXPR_MATCH:
            SOL_IR_REACHES(expression->as.match_expr.scrutinee);
            for (size_t index = 0; index < expression->as.match_expr.arms.count; ++index) {
                SolIrArmId arm
                    = ir->arm_ids[expression->as.match_expr.arms.offset + index];
                if (ir->arms[arm].guard != SOL_IR_NONE) {
                    SOL_IR_REACHES(ir->arms[arm].guard);
                }
                SOL_IR_REACHES(ir->arms[arm].body);
            }
            break;
        case SOL_IR_EXPR_BLOCK:
            for (size_t index = 0; index < expression->as.block.statements.count; ++index) {
                const SolIrStatement *statement = &ir->statements[ir->statement_ids[
                    expression->as.block.statements.offset + index]];
                if (statement->target != SOL_IR_NONE) SOL_IR_REACHES(statement->target);
                if (statement->condition != SOL_IR_NONE) SOL_IR_REACHES(statement->condition);
                if (statement->expression != SOL_IR_NONE) SOL_IR_REACHES(statement->expression);
            }
            break;
        case SOL_IR_EXPR_PROPAGATE:
            SOL_IR_REACHES(expression->as.propagate.operand);
            break;
        case SOL_IR_EXPR_HANDLE:
            SOL_IR_REACHES(expression->as.handler.authority);
            SOL_IR_REACHES(expression->as.handler.provider);
            SOL_IR_REACHES(expression->as.handler.body);
            break;
        case SOL_IR_EXPR_SNAPSHOT_READ:
            if (expression->as.snapshot < ir->snapshot_count) {
                SOL_IR_REACHES(ir->snapshots[expression->as.snapshot].operand);
            }
            break;
        default:
            break;
    }
#undef SOL_IR_REACHES
    states[id] = 2;
    return false;
}

static bool sol_ir_proof_expression_non_executable(
    const SolIr *ir,
    SolIrExpressionId id,
    SolIrCallableId callable_id,
    const unsigned char *runtime_states,
    unsigned char *proof_states,
    SolIrCallableId *proof_callables,
    SolIrCallableId *local_callables,
    bool *available,
    size_t depth
) {
    if (id >= ir->expression_count || callable_id >= ir->callable_count
        || runtime_states[id] != 0 || depth >= 256) return false;
    if (proof_states[id] == 2) return false;
    if (proof_states[id] == 1) return false;
    proof_states[id] = 1;
    proof_callables[id] = callable_id;
#define SOL_IR_PROOF(child) \
    do { if (!sol_ir_proof_expression_non_executable( \
        ir, (child), callable_id, runtime_states, proof_states, \
        proof_callables, local_callables, available, depth + 1)) return false; } while (0)
    const SolIrExpression *expression = &ir->expressions[id];
    const SolIrCallable *callable = &ir->callables[callable_id];
    SolIrSlice provenance[2] = {expression->capability_roots,
        expression->operation_roots};
    for (size_t slice = 0; slice < 2; ++slice) {
        for (size_t index = 0; index < provenance[slice].count; ++index) {
            SolIrLocalId root = ir->roots[provenance[slice].offset + index];
            if (root >= ir->local_count || ir->locals[root].owner != callable->owner) {
                return false;
            }
            if (local_callables[root] != SOL_IR_NONE
                && local_callables[root] != callable_id) return false;
            if (available != NULL && !available[root]) return false;
        }
    }
    switch (expression->kind) {
        case SOL_IR_EXPR_PLACE: {
            const SolIrPlace *place = &ir->places[expression->as.place];
            if (place->root_kind == SOL_IR_PLACE_ROOT_LOCAL) {
                if (place->local >= ir->local_count
                    || ir->locals[place->local].owner != callable->owner
                    || (local_callables[place->local] != SOL_IR_NONE
                        && local_callables[place->local] != callable_id)
                    || (available != NULL && !available[place->local])) return false;
            } else {
                SOL_IR_PROOF(place->temporary);
            }
            for (size_t index = 0; index < place->projections.count; ++index) {
                const SolIrProjection *projection
                    = &ir->projections[place->projections.offset + index];
                if (projection->kind == SOL_IR_PROJECTION_INDEX) {
                    SOL_IR_PROOF(projection->index);
                }
            }
            break;
        }
        case SOL_IR_EXPR_UNARY:
            SOL_IR_PROOF(expression->as.unary.operand);
            break;
        case SOL_IR_EXPR_BINARY:
            SOL_IR_PROOF(expression->as.binary.left);
            SOL_IR_PROOF(expression->as.binary.right);
            break;
        case SOL_IR_EXPR_CALL:
            if (!sol_ir_proof_call_is_pure(ir, expression)) return false;
            if (expression->as.call.kind == SOL_IR_CALL_CALLBACK
                || expression->as.call.kind == SOL_IR_CALL_CAPABILITY) {
                SOL_IR_PROOF(expression->as.call.callee);
            } else if (expression->as.call.kind == SOL_IR_CALL_METHOD) {
                SOL_IR_PROOF(expression->as.call.receiver);
            }
            for (size_t index = 0; index < expression->as.call.operands.count; ++index) {
                SOL_IR_PROOF(ir->operands[
                    expression->as.call.operands.offset + index].value);
            }
            break;
        case SOL_IR_EXPR_RECORD:
            for (size_t index = 0; index < expression->as.record.fields.count; ++index) {
                SOL_IR_PROOF(ir->operands[
                    expression->as.record.fields.offset + index].value);
            }
            break;
        case SOL_IR_EXPR_TUPLE:
            for (size_t index = 0; index < expression->as.tuple.operands.count; ++index) {
                SOL_IR_PROOF(ir->operands[
                    expression->as.tuple.operands.offset + index].value);
            }
            break;
        case SOL_IR_EXPR_BOUND_OPERATION:
            SOL_IR_PROOF(expression->as.operation.receiver);
            break;
        case SOL_IR_EXPR_IF:
            SOL_IR_PROOF(expression->as.if_expr.condition);
            SOL_IR_PROOF(expression->as.if_expr.then_branch);
            SOL_IR_PROOF(expression->as.if_expr.else_branch);
            break;
        case SOL_IR_EXPR_MATCH:
            SOL_IR_PROOF(expression->as.match_expr.scrutinee);
            for (size_t index = 0; index < expression->as.match_expr.arms.count; ++index) {
                SolIrArmId arm = ir->arm_ids[
                    expression->as.match_expr.arms.offset + index];
                for (size_t binding = 0; binding < ir->arms[arm].bindings.count;
                    ++binding) {
                    SolIrLocalId local = ir->roots[
                        ir->arms[arm].bindings.offset + binding];
                    if (local >= ir->local_count
                        || ir->locals[local].owner != callable->owner
                        || (local_callables[local] != SOL_IR_NONE
                            && local_callables[local] != callable_id)) return false;
                    local_callables[local] = callable_id;
                    if (available != NULL) available[local] = true;
                }
                if (ir->arms[arm].guard != SOL_IR_NONE) {
                    SOL_IR_PROOF(ir->arms[arm].guard);
                }
                SOL_IR_PROOF(ir->arms[arm].body);
                if (available != NULL) {
                    for (size_t binding = 0; binding < ir->arms[arm].bindings.count;
                        ++binding) {
                        available[ir->roots[
                            ir->arms[arm].bindings.offset + binding]] = false;
                    }
                }
            }
            break;
        case SOL_IR_EXPR_BLOCK:
            for (size_t index = 0; index < expression->as.block.statements.count; ++index) {
                const SolIrStatement *statement = &ir->statements[ir->statement_ids[
                    expression->as.block.statements.offset + index]];
                if (statement->kind != SOL_IR_STATEMENT_LET
                    && statement->kind != SOL_IR_STATEMENT_EXPRESSION) return false;
                if (statement->kind == SOL_IR_STATEMENT_LET
                    && (statement->local >= ir->local_count
                        || ir->locals[statement->local].owner != callable->owner
                        || (local_callables[statement->local] != SOL_IR_NONE
                            && local_callables[statement->local] != callable_id))) {
                    return false;
                }
                SOL_IR_PROOF(statement->expression);
                if (statement->kind == SOL_IR_STATEMENT_LET) {
                    local_callables[statement->local] = callable_id;
                    if (available != NULL) available[statement->local] = true;
                }
            }
            if (available != NULL) {
                for (size_t index = 0; index < expression->as.block.statements.count;
                    ++index) {
                    const SolIrStatement *statement = &ir->statements[ir->statement_ids[
                        expression->as.block.statements.offset + index]];
                    if (statement->kind == SOL_IR_STATEMENT_LET) {
                        available[statement->local] = false;
                    }
                }
            }
            break;
        case SOL_IR_EXPR_PROPAGATE:
        case SOL_IR_EXPR_HANDLE:
        case SOL_IR_EXPR_RESULT:
        case SOL_IR_EXPR_SNAPSHOT_READ:
        case SOL_IR_EXPR_REFINEMENT_SELF:
        case SOL_IR_EXPR_COMPILE_TIME_HEAD:
            return false;
        default:
            break;
    }
#undef SOL_IR_PROOF
    proof_states[id] = 2;
    return true;
}

static bool sol_ir_validate_pattern_tree(const SolIr *ir, SolIrPatternId id,
    unsigned char *states, size_t *binding_index, const SolIrArm *arm,
    size_t depth) {
    if (id >= ir->pattern_count || depth >= 64 || states[id] != 0) return false;
    states[id] = 1;
    const SolIrPattern *pattern = &ir->patterns[id];
    if (pattern->kind == SOL_IR_PATTERN_BINDING) {
        if (*binding_index >= arm->bindings.count
            || ir->roots[arm->bindings.offset + *binding_index] != pattern->binding
            || ir->cleanup_locals[arm->cleanup.offset + arm->cleanup.count
                - 1 - *binding_index]
                != pattern->binding) return false;
        ++*binding_index;
    }
    if (!sol_ir_slice_valid(pattern->children, ir->pattern_child_count)) return false;
    for (size_t index = 0; index < pattern->children.count; ++index) {
        if (!sol_ir_validate_pattern_tree(ir,
            ir->pattern_children[pattern->children.offset + index].pattern,
            states, binding_index, arm, depth + 1)) return false;
    }
    states[id] = 2;
    return true;
}

static bool sol_ir_validate_pattern_ownership(
    const SolIr *ir, SolDiagnostics *diagnostics
) {
    unsigned char *states = ir->pattern_count == 0 ? NULL
        : calloc(ir->pattern_count, 1);
    size_t *local_owners = ir->local_count == 0 ? NULL
        : calloc(ir->local_count, sizeof(*local_owners));
    size_t *edge_owners = ir->pattern_child_count == 0 ? NULL
        : calloc(ir->pattern_child_count, sizeof(*edge_owners));
    if ((ir->pattern_count != 0 && states == NULL)
        || (ir->local_count != 0 && local_owners == NULL)
        || (ir->pattern_child_count != 0 && edge_owners == NULL)) {
        free(states); free(local_owners); free(edge_owners);
        return sol_ir_error(diagnostics, "IR pattern ownership allocation failed");
    }
    bool valid = true;
    for (size_t index = 0; valid && index < ir->arm_count; ++index) {
        size_t binding = 0;
        valid = sol_ir_validate_pattern_tree(ir, ir->arms[index].pattern,
            states, &binding, &ir->arms[index], 0)
            && binding == ir->arms[index].bindings.count;
    }
    for (size_t index = 0; valid && index < ir->pattern_count; ++index) {
        valid = states[index] == 2;
        for (size_t child = 0; valid
            && child < ir->patterns[index].children.count; ++child) {
            ++edge_owners[ir->patterns[index].children.offset + child];
        }
        if (valid && ir->patterns[index].kind == SOL_IR_PATTERN_BINDING
            && ir->patterns[index].binding < ir->local_count) {
            ++local_owners[ir->patterns[index].binding];
        }
    }
    for (size_t index = 0; valid && index < ir->local_count; ++index) {
        valid = ir->locals[index].kind == SOL_IR_LOCAL_PATTERN
            ? local_owners[index] == 1 : local_owners[index] == 0;
    }
    for (size_t index = 0; valid && index < ir->pattern_child_count; ++index) {
        valid = edge_owners[index] == 1;
    }
    free(states); free(local_owners); free(edge_owners);
    return valid || sol_ir_error(diagnostics,
        "IR patterns are cyclic, shared, orphaned, or have incorrect binding slices");
}

typedef struct { SolIrPatternId pattern; } SolIrMatchCell;
typedef struct {
    SolIrMatchCell *cells;
    size_t row_count;
    size_t column_count;
} SolIrMatchMatrix;
typedef struct { unsigned kind; size_t id; } SolIrMatchConstructor;

static bool sol_ir_pattern_constructor(const SolIr *ir, SolIrPatternId id,
    SolIrMatchConstructor *constructor) {
    if (id == SOL_IR_NONE) return false;
    const SolIrPattern *pattern = &ir->patterns[id];
    if (pattern->kind == SOL_IR_PATTERN_BOOL) {
        *constructor = (SolIrMatchConstructor){1, pattern->boolean ? 1 : 0};
        return true;
    }
    if (pattern->kind == SOL_IR_PATTERN_VARIANT) {
        *constructor = (SolIrMatchConstructor){2, pattern->variant};
        return true;
    }
    if (pattern->kind == SOL_IR_PATTERN_RECORD
        || pattern->kind == SOL_IR_PATTERN_TUPLE) {
        *constructor = (SolIrMatchConstructor){3, 0};
        return true;
    }
    return false;
}

static SolIrPatternId sol_ir_constructor_child(const SolIr *ir,
    SolIrPatternId id, size_t ordinal, SolIrFieldId field) {
    const SolIrPattern *pattern = &ir->patterns[id];
    for (size_t index = 0; index < pattern->children.count; ++index) {
        const SolIrPatternChild *edge
            = &ir->pattern_children[pattern->children.offset + index];
        if ((pattern->kind == SOL_IR_PATTERN_RECORD && edge->field == field)
            || (pattern->kind != SOL_IR_PATTERN_RECORD
                && edge->ordinal == ordinal)) return edge->pattern;
    }
    return SOL_IR_NONE;
}

static bool sol_ir_constructor_fields(const SolIr *ir, SolIrTypeId type_id,
    SolIrMatchConstructor constructor, SolIrTypeId **types,
    SolIrFieldId **fields, size_t *count) {
    *types = NULL; *fields = NULL; *count = 0;
    const SolIrType *type = &ir->types[type_id];
    if (constructor.kind == 1) return type->kind == SOL_IR_TYPE_BOOL;
    if (constructor.kind == 3 && type->kind == SOL_IR_TYPE_TUPLE) {
        *count = type->argument_count;
        *types = sol_ir_allocate(*count, sizeof(**types), false);
        if (*count != 0 && *types == NULL) return false;
        for (size_t index = 0; index < *count; ++index) {
            (*types)[index] = ir->type_ids[type->argument_offset + index];
        }
        return true;
    }
    if (type->kind != SOL_IR_TYPE_NOMINAL
        || type->definition >= ir->definition_count) return false;
    const SolIrDefinition *definition = &ir->definitions[type->definition];
    SolIrSlice source = {0};
    if (constructor.kind == 2 && constructor.id < ir->variant_count
        && ir->variants[constructor.id].owner == type->definition) {
        source = ir->variants[constructor.id].fields;
    } else if (constructor.kind == 3
        && definition->kind == SOL_IR_DEFINITION_RECORD) {
        source = definition->fields;
    } else {
        return false;
    }
    *count = source.count;
    *types = sol_ir_allocate(*count, sizeof(**types), false);
    *fields = sol_ir_allocate(*count, sizeof(**fields), false);
    if (*count != 0 && (*types == NULL || *fields == NULL)) {
        free(*types); free(*fields); *types = NULL; *fields = NULL;
        return false;
    }
    for (size_t index = 0; index < *count; ++index) {
        (*fields)[index] = source.offset + index;
        (*types)[index] = SOL_IR_NONE;
        for (size_t candidate = 0; candidate < ir->type_count; ++candidate) {
            if (sol_ir_type_matches_instantiation(ir, candidate,
                ir->fields[source.offset + index].type,
                definition->generic_parameters,
                (SolIrSlice){type->argument_offset, type->argument_count},
                SOL_IR_NONE, 0)) {
                (*types)[index] = candidate;
                break;
            }
        }
        if ((*types)[index] == SOL_IR_NONE) {
            free(*types); free(*fields); *types = NULL; *fields = NULL;
            return false;
        }
    }
    return true;
}

typedef enum {
    SOL_IR_INHABITATION_EMPTY,
    SOL_IR_INHABITATION_INHABITED,
    SOL_IR_INHABITATION_CYCLE,
    SOL_IR_INHABITATION_INDETERMINATE,
} SolIrInhabitation;

static SolIrInhabitation sol_ir_finite_inhabitation(const SolIr *ir,
    SolIrTypeId type_id, SolIrTypeId *stack, size_t depth, size_t *steps);

static SolIrInhabitation sol_ir_type_list_inhabitation(const SolIr *ir,
    const SolIrTypeId *types, size_t count, SolIrTypeId *stack,
    size_t depth, size_t *steps) {
    SolIrInhabitation result = SOL_IR_INHABITATION_INHABITED;
    for (size_t index = 0; index < count; ++index) {
        SolIrInhabitation child = sol_ir_finite_inhabitation(
            ir, types[index], stack, depth, steps);
        if (child == SOL_IR_INHABITATION_EMPTY) return child;
        if (child == SOL_IR_INHABITATION_INDETERMINATE) {
            result = SOL_IR_INHABITATION_INDETERMINATE;
        } else if (child == SOL_IR_INHABITATION_CYCLE
            && result == SOL_IR_INHABITATION_INHABITED) {
            result = SOL_IR_INHABITATION_CYCLE;
        }
    }
    return result;
}

static SolIrInhabitation sol_ir_constructor_inhabitation(const SolIr *ir,
    SolIrTypeId type_id, SolIrMatchConstructor constructor,
    SolIrTypeId *stack, size_t depth, size_t *steps) {
    SolIrTypeId *types = NULL;
    SolIrFieldId *fields = NULL;
    size_t count = 0;
    if (!sol_ir_constructor_fields(
        ir, type_id, constructor, &types, &fields, &count)) {
        return SOL_IR_INHABITATION_INDETERMINATE;
    }
    SolIrInhabitation result = sol_ir_type_list_inhabitation(
        ir, types, count, stack, depth, steps);
    free(types);
    free(fields);
    return result;
}

static SolIrInhabitation sol_ir_finite_inhabitation(const SolIr *ir,
    SolIrTypeId type_id, SolIrTypeId *stack, size_t depth, size_t *steps) {
    if (type_id >= ir->type_count || depth >= 64 || (*steps)++ >= 1024) {
        return SOL_IR_INHABITATION_INDETERMINATE;
    }
    const SolIrType *type = &ir->types[type_id];
    if (type->kind == SOL_IR_TYPE_NEVER) return SOL_IR_INHABITATION_EMPTY;
    if (type->kind == SOL_IR_TYPE_PARAMETER || type->kind == SOL_IR_TYPE_SELF) {
        return SOL_IR_INHABITATION_INDETERMINATE;
    }
    for (size_t index = 0; index < depth; ++index) {
        if (stack[index] == type_id) return SOL_IR_INHABITATION_CYCLE;
    }
    stack[depth] = type_id;
    if (type->kind == SOL_IR_TYPE_OPTION) return SOL_IR_INHABITATION_INHABITED;
    if (type->kind == SOL_IR_TYPE_RESULT) {
        if (!sol_ir_slice_valid((SolIrSlice){type->argument_offset,
                type->argument_count}, ir->type_id_count)
            || type->argument_count != 2) return SOL_IR_INHABITATION_INDETERMINATE;
        SolIrInhabitation left = sol_ir_finite_inhabitation(ir,
            ir->type_ids[type->argument_offset], stack, depth + 1, steps);
        SolIrInhabitation right = sol_ir_finite_inhabitation(ir,
            ir->type_ids[type->argument_offset + 1], stack, depth + 1, steps);
        if (left == SOL_IR_INHABITATION_INHABITED
            || right == SOL_IR_INHABITATION_INHABITED) {
            return SOL_IR_INHABITATION_INHABITED;
        }
        if (left == SOL_IR_INHABITATION_INDETERMINATE
            || right == SOL_IR_INHABITATION_INDETERMINATE) {
            return SOL_IR_INHABITATION_INDETERMINATE;
        }
        if (left == SOL_IR_INHABITATION_CYCLE
            || right == SOL_IR_INHABITATION_CYCLE) return SOL_IR_INHABITATION_CYCLE;
        return SOL_IR_INHABITATION_EMPTY;
    }
    if (type->kind == SOL_IR_TYPE_TUPLE) {
        if (!sol_ir_slice_valid((SolIrSlice){type->argument_offset,
                type->argument_count}, ir->type_id_count)) {
            return SOL_IR_INHABITATION_INDETERMINATE;
        }
        return sol_ir_type_list_inhabitation(ir,
            ir->type_ids + type->argument_offset, type->argument_count,
            stack, depth + 1, steps);
    }
    if (type->kind != SOL_IR_TYPE_NOMINAL
        || type->definition >= ir->definition_count) {
        return type->kind == SOL_IR_TYPE_NOMINAL
            ? SOL_IR_INHABITATION_INDETERMINATE
            : SOL_IR_INHABITATION_INHABITED;
    }
    const SolIrDefinition *definition = &ir->definitions[type->definition];
    if (definition->kind == SOL_IR_DEFINITION_RECORD) {
        return sol_ir_constructor_inhabitation(ir, type_id,
            (SolIrMatchConstructor){3, 0}, stack, depth + 1, steps);
    }
    if (definition->kind != SOL_IR_DEFINITION_ENUM || definition->open) {
        return SOL_IR_INHABITATION_INHABITED;
    }
    if (!sol_ir_slice_valid(definition->variants, ir->variant_count)) {
        return SOL_IR_INHABITATION_INDETERMINATE;
    }
    SolIrInhabitation result = SOL_IR_INHABITATION_EMPTY;
    for (size_t index = 0; index < definition->variants.count; ++index) {
        SolIrInhabitation constructor = sol_ir_constructor_inhabitation(ir,
            type_id, (SolIrMatchConstructor){2,
                definition->variants.offset + index},
            stack, depth + 1, steps);
        if (constructor == SOL_IR_INHABITATION_INHABITED) return constructor;
        if (constructor == SOL_IR_INHABITATION_INDETERMINATE) {
            result = SOL_IR_INHABITATION_INDETERMINATE;
        } else if (constructor == SOL_IR_INHABITATION_CYCLE
            && result == SOL_IR_INHABITATION_EMPTY) {
            result = SOL_IR_INHABITATION_CYCLE;
        }
    }
    return result;
}

static bool sol_ir_maybe_finitely_inhabited(const SolIr *ir, SolIrTypeId type) {
    SolIrTypeId stack[64];
    size_t steps = 0;
    SolIrInhabitation result = sol_ir_finite_inhabitation(
        ir, type, stack, 0, &steps);
    return result != SOL_IR_INHABITATION_EMPTY
        && result != SOL_IR_INHABITATION_CYCLE;
}

static bool sol_ir_match_constructor_inhabited(const SolIr *ir,
    SolIrTypeId type, SolIrMatchConstructor constructor) {
    SolIrTypeId stack[64];
    size_t steps = 0;
    SolIrInhabitation result = sol_ir_constructor_inhabitation(
        ir, type, constructor, stack, 0, &steps);
    return result != SOL_IR_INHABITATION_EMPTY
        && result != SOL_IR_INHABITATION_CYCLE;
}

static bool sol_ir_match_specialize(const SolIr *ir,
    const SolIrMatchMatrix *source, SolIrMatchConstructor constructor,
    size_t arity, const SolIrFieldId *fields, SolIrMatchMatrix *result,
    bool *overflow) {
    result->column_count = source->column_count - 1 + arity;
    result->row_count = 0;
    result->cells = NULL;
    if (source->row_count != 0
        && result->column_count > 8192 / source->row_count) {
        *overflow = true;
        return false;
    }
    size_t capacity = source->row_count * result->column_count;
    result->cells = sol_ir_allocate(capacity, sizeof(*result->cells), false);
    if (capacity != 0 && result->cells == NULL) return false;
    for (size_t row = 0; row < source->row_count; ++row) {
        const SolIrMatchCell *input
            = source->cells + row * source->column_count;
        SolIrMatchConstructor actual;
        bool wildcard = !sol_ir_pattern_constructor(ir, input[0].pattern, &actual);
        if (!wildcard && (actual.kind != constructor.kind
                || actual.id != constructor.id)) continue;
        SolIrMatchCell *output = result->column_count == 0 ? NULL
            : result->cells + result->row_count * result->column_count;
        ++result->row_count;
        for (size_t index = 0; index < arity; ++index) {
            output[index].pattern = wildcard ? SOL_IR_NONE
                : sol_ir_constructor_child(ir, input[0].pattern, index,
                    fields == NULL ? SOL_IR_NONE : fields[index]);
        }
        if (source->column_count > 1) memcpy(output + arity, input + 1,
            (source->column_count - 1) * sizeof(*output));
    }
    return true;
}

static bool sol_ir_match_useful(const SolIr *ir, const SolIrMatchMatrix *matrix,
    const SolIrMatchCell *candidate, const SolIrTypeId *types, size_t depth,
    size_t *steps, bool *overflow) {
    if (*overflow) return false;
    if (depth >= 64 || (*steps)++ >= 8192) {
        *overflow = true;
        return false;
    }
    if (matrix->column_count == 0) return matrix->row_count == 0;
    if (!sol_ir_maybe_finitely_inhabited(ir, types[0])) return false;
    bool candidate_wildcards = true;
    for (size_t column = 0; column < matrix->column_count; ++column) {
        SolIrMatchConstructor ignored;
        candidate_wildcards = candidate_wildcards
            && !sol_ir_pattern_constructor(ir, candidate[column].pattern, &ignored);
    }
    if (candidate_wildcards) {
        for (size_t row = 0; row < matrix->row_count; ++row) {
            bool row_wildcards = true;
            for (size_t column = 0; column < matrix->column_count; ++column) {
                SolIrMatchConstructor ignored;
                row_wildcards = row_wildcards && !sol_ir_pattern_constructor(ir,
                    matrix->cells[row * matrix->column_count + column].pattern,
                    &ignored);
            }
            if (row_wildcards) return false;
        }
    }
    SolIrMatchConstructor direct;
    bool has_direct = sol_ir_pattern_constructor(ir, candidate[0].pattern, &direct);
    const SolIrType *type = &ir->types[types[0]];
    bool product = type->kind == SOL_IR_TYPE_TUPLE
        || (type->kind == SOL_IR_TYPE_NOMINAL
            && type->definition < ir->definition_count
            && ir->definitions[type->definition].kind == SOL_IR_DEFINITION_RECORD);
    bool closed_enum = type->kind == SOL_IR_TYPE_NOMINAL
        && type->definition < ir->definition_count
        && ir->definitions[type->definition].kind == SOL_IR_DEFINITION_ENUM
        && !ir->definitions[type->definition].open;
    bool complete = type->kind == SOL_IR_TYPE_BOOL || product || closed_enum;
    SolIrMatchConstructor constructors[2] = {{0}};
    size_t constructor_count = 0;
    size_t enum_index = 0;
    size_t enum_count = 0;
    if (has_direct) constructors[constructor_count++] = direct;
    else if (type->kind == SOL_IR_TYPE_BOOL) {
        constructors[0] = (SolIrMatchConstructor){1, 0};
        constructors[1] = (SolIrMatchConstructor){1, 1};
        constructor_count = 2;
    } else if (product) {
        constructors[constructor_count++] = (SolIrMatchConstructor){3, 0};
    } else if (closed_enum) {
        enum_index = ir->definitions[type->definition].variants.offset;
        enum_count = ir->definitions[type->definition].variants.count;
    }
    if (has_direct || complete) {
        while (constructor_count != 0 || enum_count != 0) {
            SolIrMatchConstructor constructor;
            if (enum_count != 0) {
                constructor = (SolIrMatchConstructor){2, enum_index++};
                --enum_count;
            } else {
                constructor = constructors[--constructor_count];
            }
            if (!sol_ir_match_constructor_inhabited(
                ir, types[0], constructor)) continue;
            SolIrTypeId *field_types = NULL;
            SolIrFieldId *field_ids = NULL;
            size_t arity = 0;
            if (!sol_ir_constructor_fields(ir, types[0], constructor,
                    &field_types, &field_ids, &arity)) return false;
            size_t columns = matrix->column_count - 1 + arity;
            SolIrTypeId *specialized_types
                = sol_ir_allocate(columns, sizeof(*specialized_types), false);
            SolIrMatchCell *specialized_candidate
                = sol_ir_allocate(columns, sizeof(*specialized_candidate), false);
            SolIrMatchMatrix specialized;
            bool allocated = (columns == 0
                    || (specialized_types != NULL && specialized_candidate != NULL))
                && sol_ir_match_specialize(ir, matrix, constructor, arity,
                    field_ids, &specialized, overflow);
            if (!allocated) {
                free(field_types); free(field_ids); free(specialized_types);
                free(specialized_candidate);
                return false;
            }
            if (arity != 0) memcpy(specialized_types, field_types,
                arity * sizeof(*field_types));
            if (matrix->column_count > 1) memcpy(specialized_types + arity,
                types + 1, (matrix->column_count - 1) * sizeof(*types));
            for (size_t index = 0; index < arity; ++index) {
                specialized_candidate[index].pattern = has_direct
                    ? sol_ir_constructor_child(ir, candidate[0].pattern, index,
                        field_ids == NULL ? SOL_IR_NONE : field_ids[index])
                    : SOL_IR_NONE;
            }
            if (matrix->column_count > 1) memcpy(specialized_candidate + arity,
                candidate + 1,
                (matrix->column_count - 1) * sizeof(*candidate));
            bool useful = sol_ir_match_useful(ir, &specialized,
                specialized_candidate, specialized_types, depth + 1,
                steps, overflow);
            free(field_types); free(field_ids); free(specialized_types);
            free(specialized_candidate); free(specialized.cells);
            if (useful) return true;
        }
        return false;
    }
    SolIrMatchMatrix defaults;
    if (!sol_ir_match_specialize(ir, matrix,
        (SolIrMatchConstructor){0, 0}, 0, NULL, &defaults, overflow)) return false;
    bool useful = sol_ir_match_useful(ir, &defaults, candidate + 1,
        types + 1, depth + 1, steps, overflow);
    free(defaults.cells);
    return useful;
}

static bool sol_ir_match_coverage_valid(const SolIr *ir,
    const SolIrExpression *expression) {
    size_t arm_count = expression->as.match_expr.arms.count;
    SolIrMatchCell *rows = sol_ir_allocate(arm_count, sizeof(*rows), false);
    if (arm_count != 0 && rows == NULL) return false;
    size_t row_count = 0;
    SolIrTypeId type = ir->expressions[expression->as.match_expr.scrutinee].type;
    for (size_t index = 0; index < arm_count; ++index) {
        const SolIrArm *arm = &ir->arms[ir->arm_ids[
            expression->as.match_expr.arms.offset + index]];
        SolIrMatchMatrix matrix = {rows, row_count, 1};
        SolIrMatchCell candidate = {arm->pattern};
        size_t steps = 0;
        bool overflow = false;
        if (!sol_ir_match_useful(ir, &matrix, &candidate, &type, 0,
                &steps, &overflow) || overflow) {
            free(rows);
            return false;
        }
        if (arm->guard == SOL_IR_NONE) rows[row_count++].pattern = arm->pattern;
    }
    if (sol_ir_maybe_finitely_inhabited(ir, type)) {
        SolIrMatchMatrix matrix = {rows, row_count, 1};
        SolIrMatchCell wildcard = {SOL_IR_NONE};
        size_t steps = 0;
        bool overflow = false;
        bool missing = sol_ir_match_useful(ir, &matrix, &wildcard, &type,
            0, &steps, &overflow);
        if (missing || overflow) {
            free(rows);
            return false;
        }
    }
    free(rows);
    return true;
}

static bool sol_ir_validate_impl(const SolIr *ir, SolDiagnostics *diagnostics,
    bool validate_ownership) {
    if (ir == NULL || ir->source_bytes == NULL || ir->source_path == NULL
        || (ir->type_count != 0 && ir->types == NULL)
        || (ir->type_id_count != 0 && ir->type_ids == NULL)
        || ir->access_count != ir->type_id_count
        || (ir->access_count != 0 && ir->accesses == NULL)
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
        || (ir->place_count != 0 && ir->places == NULL)
        || (ir->projection_count != 0 && ir->projections == NULL)
        || (ir->statement_count != 0 && ir->statements == NULL)
        || (ir->statement_id_count != 0 && ir->statement_ids == NULL)
        || (ir->arm_count != 0 && ir->arms == NULL)
        || (ir->arm_id_count != 0 && ir->arm_ids == NULL)
        || (ir->pattern_count != 0 && ir->patterns == NULL)
        || (ir->pattern_child_count != 0 && ir->pattern_children == NULL)
        || ir->pattern_count > SIZE_MAX / sizeof(*ir->patterns)
        || ir->pattern_child_count > SIZE_MAX / sizeof(*ir->pattern_children)
        || (ir->operand_count != 0 && ir->operands == NULL)
        || (ir->root_count != 0 && ir->roots == NULL)
        || (ir->cleanup_local_count != 0 && ir->cleanup_locals == NULL)
        || (ir->effect_count != 0 && ir->effects == NULL)
        || (ir->obligation_count != 0 && ir->obligations == NULL)
        || (ir->snapshot_count != 0 && ir->snapshots == NULL)
        || (ir->loop_obligation_count != 0 && ir->loop_obligations == NULL)
        || (ir->unreachable_obligation_count != 0
            && ir->unreachable_obligations == NULL)) {
        return sol_ir_error(diagnostics, "malformed canonical IR ownership or counts");
    }
    for (size_t index = 0; index < ir->type_count; ++index) {
        const SolIrType *type = &ir->types[index];
        if ((int)type->kind < 0 || type->kind > SOL_IR_TYPE_SELF
            || !sol_ir_slice_valid((SolIrSlice){type->argument_offset,
                type->argument_count}, ir->type_id_count)
            || !sol_ir_slice_valid((SolIrSlice){type->parameter_offset,
                type->parameter_count}, ir->type_id_count)
            || !sol_ir_slice_valid((SolIrSlice){type->parameter_access_offset,
                type->parameter_count}, ir->access_count)
            || ((type->kind == SOL_IR_TYPE_NOMINAL || (type->kind == SOL_IR_TYPE_FUNCTION
                    && type->definition != SOL_IR_NONE))
                && type->definition >= ir->definition_count)
            || (type->result != SOL_IR_NONE && type->result >= ir->type_count)
            || !sol_ir_slice_valid(type->effects, ir->effect_count)
            || !sol_ir_type_shape_valid(ir, type)) {
            return sol_ir_indexed_error(diagnostics, "malformed canonical IR type",
                index, (int)type->kind);
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
            if (!sol_ir_access_valid(
                ir->accesses[type->parameter_access_offset + parameter])) {
                return sol_ir_error(diagnostics, "IR function parameter access is invalid");
            }
        }
        if (type->kind == SOL_IR_TYPE_PARAMETER
            && type->definition >= ir->generic_parameter_count) {
            return sol_ir_error(diagnostics, "IR type parameter is out of range");
        }
    }
    for (size_t index = 0; index < ir->access_count; ++index) {
        if (!sol_ir_access_valid(ir->accesses[index])) {
            return sol_ir_error(diagnostics, "IR access mode is out of range");
        }
    }
    for (size_t index = 0; index < ir->definition_count; ++index) {
        const SolIrDefinition *definition = &ir->definitions[index];
        if (definition->name == NULL || (int)definition->kind < 0
            || definition->kind > SOL_IR_DEFINITION_TEST
            || definition->span.start > definition->span.end
            || definition->span.end > ir->source_length
            || (definition->declared_type != SOL_IR_NONE
                && definition->declared_type >= ir->type_count)
            || (definition->callable != SOL_IR_NONE
                && definition->callable >= ir->callable_count)
            || (definition->representation != SOL_IR_NONE
                && definition->representation >= ir->type_count)
            || (definition->kind == SOL_IR_DEFINITION_IMPLEMENTATION
                && (definition->implementation_trait >= ir->definition_count
                    || definition->implementation_target >= ir->type_count))
            || (definition->kind != SOL_IR_DEFINITION_ENUM && definition->open)
            || (definition->kind != SOL_IR_DEFINITION_IMPLEMENTATION
                && (definition->implementation_trait != SOL_IR_NONE
                    || definition->implementation_target != SOL_IR_NONE))
            || !sol_ir_slice_valid(definition->fields, ir->field_count)
            || !sol_ir_slice_valid(definition->variants, ir->variant_count)
            || !sol_ir_slice_valid(definition->members, ir->member_count)
            || !sol_ir_slice_valid(definition->generic_parameters,
                ir->generic_parameter_count)
            || !sol_ir_slice_valid(definition->effect_parameters,
                ir->effect_parameter_count)
            || (definition->capability_source != SOL_IR_NONE
                && (definition->kind != SOL_IR_DEFINITION_CAPABILITY
                    || definition->capability_source >= ir->local_count
                    || ir->locals[definition->capability_source].owner != index
                    || ir->locals[definition->capability_source].kind
                        != SOL_IR_LOCAL_PARAMETER
                    || ir->locals[definition->capability_source].access != SOL_ACCESS_OWNED
                    || ir->locals[definition->capability_source].type >= ir->type_count
                    || ir->types[ir->locals[definition->capability_source].type].kind
                        != SOL_IR_TYPE_NOMINAL
                    || ir->types[ir->locals[definition->capability_source].type].definition
                        >= ir->definition_count
                    || ir->definitions[ir->types[
                        ir->locals[definition->capability_source].type
                    ].definition].kind != SOL_IR_DEFINITION_CAPABILITY))) {
            return sol_ir_error(diagnostics, "malformed canonical IR declaration");
        }
        if (definition->kind == SOL_IR_DEFINITION_TEST
            && (definition->callable >= ir->callable_count
                || ir->callables[definition->callable].kind != SOL_IR_CALLABLE_TEST
                || ir->callables[definition->callable].owner != index
                || definition->declared_type >= ir->type_count
                || ir->types[definition->declared_type].kind != SOL_IR_TYPE_BOOL
                || definition->representation != SOL_IR_NONE
                || definition->fields.count != 0 || definition->variants.count != 0
                || definition->members.count != 0
                || definition->generic_parameters.count != 0
                || definition->effect_parameters.count != 0
                || definition->capability_source != SOL_IR_NONE)) {
            return sol_ir_error(diagnostics, "malformed canonical IR test definition");
        }
        if ((definition->kind == SOL_IR_DEFINITION_FUNCTION
                && (definition->callable >= ir->callable_count
                    || ir->callables[definition->callable].kind
                        != SOL_IR_CALLABLE_FUNCTION))
            || ((definition->kind == SOL_IR_DEFINITION_FUNCTION
                    || definition->kind == SOL_IR_DEFINITION_TEST)
                != (definition->callable != SOL_IR_NONE))
            || (definition->callable != SOL_IR_NONE
                && ir->callables[definition->callable].owner != index)) {
            return sol_ir_error(diagnostics,
                "IR definition callable ownership or kind is inconsistent");
        }
        bool owns_members = definition->kind == SOL_IR_DEFINITION_CAPABILITY
            || definition->kind == SOL_IR_DEFINITION_TRAIT
            || definition->kind == SOL_IR_DEFINITION_IMPLEMENTATION;
        if (!owns_members && definition->members.count != 0) {
            return sol_ir_error(diagnostics,
                "IR non-member definition has a member slice");
        }
        if ((definition->kind != SOL_IR_DEFINITION_RECORD
                && definition->fields.count != 0)
            || (definition->kind != SOL_IR_DEFINITION_ENUM
                && definition->variants.count != 0)) {
            return sol_ir_error(diagnostics,
                "IR definition has fields or variants of the wrong kind");
        }
        for (size_t member = 0; member < definition->members.count; ++member) {
            SolIrCallableId callable
                = ir->members[definition->members.offset + member].callable;
            SolIrCallableKind expected = definition->kind
                    == SOL_IR_DEFINITION_CAPABILITY
                ? SOL_IR_CALLABLE_CAPABILITY
                : definition->kind == SOL_IR_DEFINITION_TRAIT
                    ? SOL_IR_CALLABLE_TRAIT_REQUIREMENT
                    : SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION;
            if (callable >= ir->callable_count
                || ir->callables[callable].owner != index
                || ir->callables[callable].kind != expected) {
                return sol_ir_error(diagnostics,
                    "IR member has wrong callable owner or kind");
            }
            for (size_t previous = 0; previous < member; ++previous) {
                if (ir->members[definition->members.offset + previous].callable
                    == callable) return sol_ir_error(diagnostics,
                        "IR member callable is duplicated in its owner slice");
            }
        }
        for (size_t field = 0; field < definition->fields.count; ++field) {
            if (ir->fields[definition->fields.offset + field].owner != index) {
                return sol_ir_error(diagnostics, "IR record field has the wrong owner");
            }
        }
        for (size_t variant = 0; variant < definition->variants.count; ++variant) {
            if (ir->variants[definition->variants.offset + variant].owner != index) {
                return sol_ir_error(diagnostics, "IR enum variant has the wrong owner");
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
    for (size_t member = 0; member < ir->member_count; ++member) {
        size_t owners = 0;
        for (size_t definition = 0; definition < ir->definition_count; ++definition) {
            SolIrSlice slice = ir->definitions[definition].members;
            if (member >= slice.offset && member - slice.offset < slice.count) ++owners;
        }
        if (owners != 1) return sol_ir_error(diagnostics,
            "IR member table entry is orphaned or shared between definitions");
    }
    for (size_t callable = 0; callable < ir->callable_count; ++callable) {
        SolIrCallableKind kind = ir->callables[callable].kind;
        if (kind != SOL_IR_CALLABLE_CAPABILITY
            && kind != SOL_IR_CALLABLE_TRAIT_REQUIREMENT
            && kind != SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION) continue;
        SolIrDefinitionId owner = ir->callables[callable].owner;
        if (owner >= ir->definition_count) return sol_ir_error(diagnostics,
            "IR member callable owner is out of range");
        SolIrSlice slice = ir->definitions[owner].members;
        size_t occurrences = 0;
        for (size_t member = 0; member < slice.count; ++member) {
            if (ir->members[slice.offset + member].callable == callable) ++occurrences;
        }
        if (occurrences != 1) return sol_ir_error(diagnostics,
            "IR member callable is missing or duplicated in its owner slice");
    }
    for (size_t index = 0; index < ir->definition_count; ++index) {
        if (ir->definitions[index].kind != SOL_IR_DEFINITION_CAPABILITY) continue;
        SolIrDefinitionId current = index;
        for (size_t depth = 0; depth <= ir->definition_count; ++depth) {
            SolIrLocalId source = ir->definitions[current].capability_source;
            if (source == SOL_IR_NONE) break;
            SolIrTypeId type = ir->locals[source].type;
            current = ir->types[type].definition;
            if (current == index || depth == ir->definition_count) {
                return sol_ir_error(diagnostics, "IR capability source chain is cyclic");
            }
        }
    }
    for (size_t index = 0; index < ir->evidence_count; ++index) {
        const SolIrDispatchEvidence *evidence = &ir->evidence[index];
        if (evidence->trait >= ir->definition_count
            || evidence->requirement >= ir->callable_count
            || ir->definitions[evidence->trait].kind != SOL_IR_DEFINITION_TRAIT
            || ir->callables[evidence->requirement].kind
                != SOL_IR_CALLABLE_TRAIT_REQUIREMENT
            || ir->callables[evidence->requirement].owner != evidence->trait
            || (evidence->binding != SOL_IR_NONE
                && (evidence->binding >= ir->generic_parameter_count
                    || ir->generic_parameters[evidence->binding].trait_bound
                        != evidence->trait))
            || (evidence->forwarded
                ? (evidence->parameter >= ir->generic_parameter_count
                    || ir->generic_parameters[evidence->parameter].trait_bound
                        != evidence->trait
                    || evidence->implementation != SOL_IR_NONE
                    || evidence->method != SOL_IR_NONE || evidence->type != SOL_IR_NONE)
                : (evidence->parameter != SOL_IR_NONE
                    || evidence->implementation >= ir->definition_count
                    || evidence->method >= ir->callable_count
                    || evidence->type >= ir->type_count
                    || ir->definitions[evidence->implementation].kind
                        != SOL_IR_DEFINITION_IMPLEMENTATION
                    || ir->definitions[evidence->implementation].implementation_trait
                        != evidence->trait
                    || ir->definitions[evidence->implementation].implementation_target
                        != evidence->type
                    || ir->callables[evidence->method].kind
                        != SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION
                    || ir->callables[evidence->method].owner
                        != evidence->implementation
                    || !sol_ir_callable_matches_requirement(ir, evidence->method,
                        evidence->requirement, evidence->type)))) {
            return sol_ir_error(diagnostics, "malformed IR dispatch evidence");
        }
    }
    for (size_t index = 0; index < ir->callable_count; ++index) {
        const SolIrCallable *callable = &ir->callables[index];
        if (callable->name == NULL || (int)callable->kind < 0
            || callable->kind > SOL_IR_CALLABLE_TEST
            || callable->owner >= ir->definition_count || callable->result >= ir->type_count
            || callable->span.start > callable->span.end
            || callable->span.end > ir->source_length
            || !sol_ir_access_valid(callable->receiver_access)
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
                != ir->definitions[callable->owner].effect_parameters.count
            || (callable->receiver != SOL_IR_NONE
                && (callable->receiver >= ir->local_count
                    || ir->locals[callable->receiver].owner != callable->owner
                    || (callable->kind != SOL_IR_CALLABLE_TRAIT_REQUIREMENT
                        && callable->kind != SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION)
                    || ir->locals[callable->receiver].access
                        != callable->receiver_access))
            || (callable->receiver == SOL_IR_NONE
                && callable->receiver_access != SOL_ACCESS_OWNED)
            || callable->capability_source
                != ir->definitions[callable->owner].capability_source
            || (int)callable->result_authority_kind < 0
            || callable->result_authority_kind > SOL_IR_AUTHORITY_SELF
            || (callable->result_authority_kind != SOL_IR_AUTHORITY_LOCAL
                && callable->result_authority != SOL_IR_NONE)) {
            return sol_ir_error(diagnostics, "malformed canonical IR callable");
        }
        if ((callable->result_authority_kind == SOL_IR_AUTHORITY_LOCAL
                && (callable->result_authority >= ir->local_count
                    || ir->locals[callable->result_authority].owner != callable->owner
                    || ir->locals[callable->result_authority].kind
                        != SOL_IR_LOCAL_PARAMETER
                    || !sol_ir_root_slice_contains(ir, callable->parameters,
                        callable->result_authority)
                    || !sol_ir_type_is_capability(ir,
                        ir->locals[callable->result_authority].type)
                    || !sol_ir_type_is_capability(ir, callable->result)))
            || (callable->result_authority_kind == SOL_IR_AUTHORITY_SELF
                && (callable->kind != SOL_IR_CALLABLE_CAPABILITY
                    || !sol_ir_type_is_capability(ir, callable->result)))) {
            return sol_ir_error(diagnostics, "malformed IR callable result authority");
        }
        if (callable->kind == SOL_IR_CALLABLE_TEST
            && (ir->definitions[callable->owner].kind != SOL_IR_DEFINITION_TEST
                || ir->definitions[callable->owner].callable != index
                || callable->parameters.count != 0 || callable->body == SOL_IR_NONE
                || ir->types[callable->result].kind != SOL_IR_TYPE_BOOL
                || callable->generic_parameters.count != 0
                || callable->effect_parameters.count != 0
                || callable->receiver != SOL_IR_NONE
                || callable->capability_source != SOL_IR_NONE
                || callable->result_authority_kind != SOL_IR_AUTHORITY_NONE)) {
            return sol_ir_error(diagnostics, "malformed canonical IR test callable");
        }
        SolIrDefinitionKind owner_kind = ir->definitions[callable->owner].kind;
        bool exact_owner = (callable->kind == SOL_IR_CALLABLE_FUNCTION
                && owner_kind == SOL_IR_DEFINITION_FUNCTION
                && ir->definitions[callable->owner].callable == index)
            || (callable->kind == SOL_IR_CALLABLE_TEST
                && owner_kind == SOL_IR_DEFINITION_TEST
                && ir->definitions[callable->owner].callable == index)
            || (callable->kind == SOL_IR_CALLABLE_CAPABILITY
                && owner_kind == SOL_IR_DEFINITION_CAPABILITY)
            || (callable->kind == SOL_IR_CALLABLE_TRAIT_REQUIREMENT
                && owner_kind == SOL_IR_DEFINITION_TRAIT)
            || (callable->kind == SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION
                && owner_kind == SOL_IR_DEFINITION_IMPLEMENTATION);
        if (!exact_owner) return sol_ir_error(diagnostics,
            "IR callable kind does not match its owning definition");
        for (size_t parameter = 0; parameter < callable->parameters.count;
            ++parameter) {
            SolIrLocalId local = ir->roots[callable->parameters.offset + parameter];
            if (local >= ir->local_count || ir->locals[local].owner != callable->owner
                || ir->locals[local].kind != SOL_IR_LOCAL_PARAMETER
                || local == callable->receiver || local == callable->capability_source) {
                return sol_ir_error(diagnostics,
                    "IR callable parameter binding is malformed");
            }
            for (size_t previous = 0; previous < parameter; ++previous) {
                if (ir->roots[callable->parameters.offset + previous] == local) {
                    return sol_ir_error(diagnostics,
                        "IR callable parameter binding is duplicated");
                }
            }
        }
        if (callable->receiver != SOL_IR_NONE
            && callable->receiver == callable->capability_source) {
            return sol_ir_error(diagnostics,
                "IR callable receiver bindings are duplicated");
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
    for (size_t index = 0; index < ir->local_count; ++index) {
        const SolIrLocal *local = &ir->locals[index];
        if (!sol_ir_access_valid(local->access)
            || (local->kind != SOL_IR_LOCAL_PARAMETER
                && local->access != SOL_ACCESS_OWNED)) {
            return sol_ir_error(diagnostics, "IR local access mode is malformed");
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
        if (effect->name == NULL || (int)effect->authority_kind < 0
            || effect->authority_kind > SOL_IR_AUTHORITY_SELF
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
        if (ir->types[type].effect_parameter != SOL_IR_NONE
            && ir->types[type].effect_parameter >= ir->effect_parameter_count) {
            return sol_ir_error(diagnostics, "IR type effect parameter is out of range");
        }
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
        if (ir->callables[callable].effect_parameter != SOL_IR_NONE
            && ir->callables[callable].effect_parameter >= ir->effect_parameter_count) {
            return sol_ir_error(diagnostics, "IR callable effect parameter is out of range");
        }
        if (ir->callables[callable].effect_parameter != SOL_IR_NONE
            && ir->effect_parameters[ir->callables[callable].effect_parameter].owner
                != ir->callables[callable].owner) {
            return sol_ir_error(diagnostics, "IR callable effect parameter has the wrong owner");
        }
        SolIrSlice row = ir->callables[callable].effects;
        for (size_t atom = 1; atom < row.count; ++atom) {
            if (sol_ir_effect_compare(&ir->effects[row.offset + atom - 1],
                &ir->effects[row.offset + atom]) >= 0) {
                return sol_ir_error(diagnostics, "IR callable effect row is not canonical");
            }
        }
    }
    for (size_t index = 0; index < ir->projection_count; ++index) {
        const SolIrProjection *projection = &ir->projections[index];
        if ((int)projection->kind < 0
            || projection->kind > SOL_IR_PROJECTION_DEREFERENCE
            || projection->type >= ir->type_count
            || projection->span.start > projection->span.end
            || projection->span.end > ir->source_length) {
            return sol_ir_error(diagnostics, "malformed canonical IR projection");
        }
        if (projection->kind == SOL_IR_PROJECTION_FIELD) {
            if (projection->field >= ir->field_count
                || projection->ordinal != SOL_IR_NONE
                || projection->index != SOL_IR_NONE) {
                return sol_ir_error(diagnostics,
                    "malformed canonical IR field projection");
            }
        } else if (projection->kind == SOL_IR_PROJECTION_TUPLE_FIELD) {
            if (projection->field != SOL_IR_NONE
                || projection->ordinal >= 16
                || projection->index != SOL_IR_NONE) {
                return sol_ir_error(diagnostics,
                    "malformed canonical IR tuple projection");
            }
        } else if (projection->kind == SOL_IR_PROJECTION_INDEX) {
            if (projection->field != SOL_IR_NONE
                || projection->ordinal != SOL_IR_NONE
                || projection->index >= ir->expression_count) {
                return sol_ir_error(diagnostics,
                    "malformed canonical IR index projection");
            }
            return sol_ir_error(diagnostics,
                "index projections are not canonical in the current IR");
        } else {
            if (projection->field != SOL_IR_NONE
                || projection->ordinal != SOL_IR_NONE
                || projection->index != SOL_IR_NONE) {
                return sol_ir_error(diagnostics,
                    "malformed canonical IR dereference projection");
            }
            return sol_ir_error(diagnostics,
                "dereference projections are not canonical in the current IR");
        }
    }
    for (size_t index = 0; index < ir->place_count; ++index) {
        const SolIrPlace *place = &ir->places[index];
        if ((int)place->root_kind < 0
            || place->root_kind > SOL_IR_PLACE_ROOT_TEMPORARY
            || place->type >= ir->type_count
            || !sol_ir_slice_valid(place->projections, ir->projection_count)
            || place->root_span.start > place->root_span.end
            || place->root_span.end > ir->source_length) {
            return sol_ir_error(diagnostics, "malformed canonical IR place");
        }
        if (place->root_kind == SOL_IR_PLACE_ROOT_LOCAL) {
            if (place->local >= ir->local_count || place->temporary != SOL_IR_NONE) {
                return sol_ir_error(diagnostics,
                    "malformed canonical IR local place root");
            }
        } else if (place->local != SOL_IR_NONE
            || place->temporary >= ir->expression_count
            || place->projections.count == 0
            || ir->expressions[place->temporary].kind == SOL_IR_EXPR_PLACE) {
            return sol_ir_error(diagnostics,
                "malformed canonical IR temporary place root");
        }
    }
    for (size_t index = 0; index < ir->expression_count; ++index) {
        const SolIrExpression *expression = &ir->expressions[index];
        if ((int)expression->kind < 0 || expression->kind > SOL_IR_EXPR_BOUND_OPERATION
            || (int)expression->local_use < 0
            || expression->local_use > SOL_IR_LOCAL_USE_UPDATE
            || expression->span.start > expression->span.end
            || expression->span.end > ir->source_length
            || (expression->kind != SOL_IR_EXPR_COMPILE_TIME_HEAD
                && expression->type >= ir->type_count)
            || (expression->kind == SOL_IR_EXPR_STRING
                && expression->as.string == NULL)
            || !sol_ir_root_slice_valid(ir, expression->capability_roots)
            || !sol_ir_root_slice_valid(ir, expression->operation_roots)) {
            return sol_ir_error(diagnostics, "malformed canonical IR expression");
        }
        if (expression->kind == SOL_IR_EXPR_CALL) {
            const SolIrSlice operands = expression->as.call.operands;
            if ((int)expression->as.call.kind < 0
                || expression->as.call.kind > SOL_IR_CALL_DISTINCT_CONSTRUCTOR
                || !sol_ir_slice_valid(operands, ir->operand_count)
                || !sol_ir_slice_valid(expression->as.call.type_arguments,
                    ir->type_id_count)
                || !sol_ir_slice_valid(expression->as.call.effects, ir->effect_count)) {
                return sol_ir_error(diagnostics, "malformed canonical IR call");
            }
            if (expression->as.call.effect_parameter != SOL_IR_NONE
                && expression->as.call.effect_parameter >= ir->effect_parameter_count) {
                return sol_ir_error(diagnostics, "IR call effect parameter is out of range");
            }
            for (size_t atom = 1; atom < expression->as.call.effects.count; ++atom) {
                if (sol_ir_effect_compare(&ir->effects[
                        expression->as.call.effects.offset + atom - 1],
                    &ir->effects[expression->as.call.effects.offset + atom]) >= 0) {
                    return sol_ir_error(diagnostics, "IR call effect row is not canonical");
                }
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
            if (expression->as.call.kind == SOL_IR_CALL_FUNCTION) {
                if (expression->as.call.callable >= ir->callable_count) {
                    return sol_ir_error(diagnostics,
                        "IR function call target is out of range");
                }
                const SolIrCallable *target
                    = &ir->callables[expression->as.call.callable];
                if (target->kind != SOL_IR_CALLABLE_FUNCTION) {
                    return sol_ir_error(diagnostics,
                        "IR source call target is not a free function");
                }
                if (target->generic_parameters.count
                    != expression->as.call.type_arguments.count) {
                    return sol_ir_error(diagnostics,
                        "IR function call generic argument set is incomplete");
                }
                for (size_t evidence_index = 0;
                    evidence_index < expression->as.call.evidence.count;
                    ++evidence_index) {
                    const SolIrDispatchEvidence *entry = &ir->evidence[
                        expression->as.call.evidence.offset + evidence_index
                    ];
                    if (entry->binding < target->generic_parameters.offset
                        || entry->binding - target->generic_parameters.offset
                            >= target->generic_parameters.count) {
                        return sol_ir_error(diagnostics,
                            "IR call evidence binding is outside the target callable");
                    }
                    size_t ordinal = entry->binding - target->generic_parameters.offset;
                    SolIrTypeId argument = ir->type_ids[
                        expression->as.call.type_arguments.offset + ordinal
                    ];
                    if ((!entry->forwarded && entry->type != argument)
                        || (entry->forwarded
                            && (ir->types[argument].kind != SOL_IR_TYPE_PARAMETER
                                || ir->types[argument].definition
                                    != entry->parameter))) {
                        return sol_ir_error(diagnostics,
                            "IR call evidence does not match its type argument");
                    }
                    for (size_t previous = 0; previous < evidence_index; ++previous) {
                        const SolIrDispatchEvidence *other = &ir->evidence[
                            expression->as.call.evidence.offset + previous
                        ];
                        if (other->binding == entry->binding
                            && other->requirement == entry->requirement) {
                            return sol_ir_error(diagnostics,
                                "IR call evidence binding is duplicated");
                        }
                    }
                }
                for (size_t ordinal = 0; ordinal < target->generic_parameters.count;
                    ++ordinal) {
                    SolIrGenericParameterId parameter
                        = target->generic_parameters.offset + ordinal;
                    SolIrDefinitionId trait
                        = ir->generic_parameters[parameter].trait_bound;
                    if (trait == SOL_IR_NONE) continue;
                    SolIrSlice requirements = ir->definitions[trait].members;
                    for (size_t member = 0; member < requirements.count; ++member) {
                        SolIrCallableId requirement
                            = ir->members[requirements.offset + member].callable;
                        size_t found = 0;
                        for (size_t evidence_index = 0;
                            evidence_index < expression->as.call.evidence.count;
                            ++evidence_index) {
                            const SolIrDispatchEvidence *entry = &ir->evidence[
                                expression->as.call.evidence.offset + evidence_index
                            ];
                            if (entry->binding == parameter
                                && entry->requirement == requirement) ++found;
                        }
                        if (found != 1) return sol_ir_error(diagnostics,
                            "IR call evidence binding is incomplete");
                    }
                }
            } else if (expression->as.call.kind == SOL_IR_CALL_METHOD) {
                if (expression->as.call.callable >= ir->callable_count
                    || expression->as.call.receiver >= ir->expression_count
                    || expression->as.call.receiver_access
                        != ir->callables[expression->as.call.callable].receiver_access) {
                    return sol_ir_error(diagnostics,
                        "IR method receiver access does not match its formal");
                }
                for (size_t evidence_index = 0;
                    evidence_index < expression->as.call.evidence.count;
                    ++evidence_index) {
                    if (ir->evidence[expression->as.call.evidence.offset
                        + evidence_index].binding != SOL_IR_NONE) {
                        return sol_ir_error(diagnostics,
                            "IR immediate method evidence has an invocation binding");
                    }
                }
                if (expression->as.call.evidence.count != 1) {
                    return sol_ir_error(diagnostics,
                        "IR method call does not have exact dispatch evidence");
                }
                const SolIrDispatchEvidence *entry = &ir->evidence[
                    expression->as.call.evidence.offset
                ];
                SolIrTypeId receiver_type
                    = ir->expressions[expression->as.call.receiver].type;
                if (entry->requirement != expression->as.call.callable
                    || (entry->forwarded
                        ? (receiver_type >= ir->type_count
                            || ir->types[receiver_type].kind != SOL_IR_TYPE_PARAMETER
                            || ir->types[receiver_type].definition != entry->parameter)
                        : entry->type != receiver_type)) {
                    return sol_ir_error(diagnostics,
                        "IR method evidence does not match its invocation");
                }
            }
            if ((expression->as.call.kind == SOL_IR_CALL_CAPABILITY
                    && expression->as.call.receiver_access != SOL_ACCESS_SHARED)
                || (expression->as.call.kind != SOL_IR_CALL_CAPABILITY
                    && expression->as.call.kind != SOL_IR_CALL_METHOD
                    && expression->as.call.receiver_access != SOL_ACCESS_OWNED)
                || !sol_ir_access_valid(expression->as.call.receiver_access)) {
                return sol_ir_error(diagnostics, "IR call receiver access is malformed");
            }
            bool callable_call = expression->as.call.kind == SOL_IR_CALL_FUNCTION
                || expression->as.call.kind == SOL_IR_CALL_CAPABILITY
                || expression->as.call.kind == SOL_IR_CALL_METHOD;
            if ((callable_call && expression->as.call.callable >= ir->callable_count)
                || (!callable_call && expression->as.call.callable != SOL_IR_NONE)
                || (expression->as.call.kind == SOL_IR_CALL_METHOD
                    && expression->as.call.receiver >= ir->expression_count)
                || (expression->as.call.kind == SOL_IR_CALL_ENUM_CONSTRUCTOR
                    && expression->as.call.variant >= ir->variant_count)
                || (expression->as.call.kind == SOL_IR_CALL_DISTINCT_CONSTRUCTOR
                    && expression->as.call.definition >= ir->definition_count)) {
                return sol_ir_error(diagnostics, "IR call target domain is malformed");
            }
            if (validate_ownership && expression->as.call.kind == SOL_IR_CALL_METHOD
                && expression->as.call.receiver_access != SOL_ACCESS_OWNED
                && !sol_ir_local_place(ir,
                    expression->as.call.receiver, NULL)) {
                return sol_ir_error(diagnostics,
                    "borrowed IR method receiver is not a direct local");
            }
            if (expression->as.call.kind == SOL_IR_CALL_CALLBACK
                && (expression->as.call.callee >= ir->expression_count
                    || ir->expressions[expression->as.call.callee].type >= ir->type_count
                    || ir->types[ir->expressions[expression->as.call.callee].type].kind
                        != SOL_IR_TYPE_FUNCTION)) {
                return sol_ir_error(diagnostics, "IR callback callee is malformed");
            }
            if (expression->as.call.kind == SOL_IR_CALL_CAPABILITY
                && (expression->as.call.callee >= ir->expression_count
                    || ir->expressions[expression->as.call.callee].type >= ir->type_count
                    || ir->types[ir->expressions[expression->as.call.callee].type].kind
                        != SOL_IR_TYPE_FUNCTION
                    || expression->as.call.callable >= ir->callable_count
                    || !sol_ir_function_type_matches_callable(ir,
                        ir->expressions[expression->as.call.callee].type,
                        expression->as.call.callable))) {
                return sol_ir_error(diagnostics, "IR capability callee is malformed");
            }
            if (validate_ownership && expression->as.call.kind == SOL_IR_CALL_CAPABILITY
                && ir->expressions[expression->as.call.callee].kind
                    == SOL_IR_EXPR_BOUND_OPERATION) {
                SolIrExpressionId receiver = ir->expressions[
                    expression->as.call.callee].as.operation.receiver;
                if (receiver >= ir->expression_count
                    || !sol_ir_local_place(ir, receiver, NULL)) {
                    return sol_ir_error(diagnostics,
                        "borrowed capability receiver is not a direct local");
                }
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
                SolAccessMode expected_access = SOL_ACCESS_OWNED;
                if (callable_call) {
                    SolIrLocalId local = ir->roots[ir->callables[
                        expression->as.call.callable].parameters.offset + operand];
                    expected_access = ir->locals[local].access;
                } else if (expression->as.call.kind == SOL_IR_CALL_CALLBACK) {
                    const SolIrType *function = &ir->types[ir->expressions[
                        expression->as.call.callee].type];
                    expected_access = ir->accesses[
                        function->parameter_access_offset + operand];
                }
                if (entry->access != expected_access) {
                    return sol_ir_error(diagnostics,
                        "IR call operand access does not match its formal");
                }
                if (validate_ownership && entry->access != SOL_ACCESS_OWNED
                    && !sol_ir_local_place(ir, entry->value, NULL)) {
                    return sol_ir_error(diagnostics,
                        "borrowed IR operand is not a direct local");
                }
            }
        } else if (expression->kind == SOL_IR_EXPR_VARIANT
            && (expression->as.variant.variant >= ir->variant_count
                || ir->variants[expression->as.variant.variant].fields.count != 0)) {
            return sol_ir_error(diagnostics, "malformed payloadless IR variant");
        } else if (expression->kind == SOL_IR_EXPR_BOUND_OPERATION) {
            SolIrDefinitionId receiver_definition = SOL_IR_NONE;
            if (expression->as.operation.receiver >= ir->expression_count
                || expression->as.operation.callable >= ir->callable_count
                || ir->callables[expression->as.operation.callable].kind
                    != SOL_IR_CALLABLE_CAPABILITY
                || !sol_ir_expression_capability_definition(ir,
                    expression->as.operation.receiver, &receiver_definition)
                || ir->callables[expression->as.operation.callable].owner
                    != receiver_definition
                || !sol_ir_function_type_matches_callable(ir, expression->type,
                    expression->as.operation.callable)) {
                return sol_ir_error(diagnostics, "malformed IR bound operation");
            }
            SolIrSlice roots = ir->expressions[
                expression->as.operation.receiver
            ].capability_roots;
            if (roots.count != expression->operation_roots.count) {
                return sol_ir_error(diagnostics,
                    "IR bound operation authority roots are inconsistent");
            }
            for (size_t root = 0; root < roots.count; ++root) {
                if (ir->roots[roots.offset + root]
                    != ir->roots[expression->operation_roots.offset + root]) {
                    return sol_ir_error(diagnostics,
                        "IR bound operation authority roots are inconsistent");
                }
            }
        } else if (expression->kind == SOL_IR_EXPR_PROPAGATE
            && ((int)expression->as.propagate.kind < 0
                || expression->as.propagate.kind > SOL_IR_PROPAGATE_RESULT
                || expression->as.propagate.operand >= ir->expression_count
                || (expression->as.propagate.kind == SOL_IR_PROPAGATE_RESULT
                    && expression->as.propagate.residual >= ir->type_count))) {
            return sol_ir_error(diagnostics, "malformed canonical IR propagation");
        } else if (expression->kind == SOL_IR_EXPR_BLOCK
            && (!sol_ir_slice_valid(expression->as.block.statements,
                    ir->statement_id_count)
                || !sol_ir_slice_valid(expression->as.block.cleanup,
                    ir->cleanup_local_count))) {
            return sol_ir_error(diagnostics, "IR block statement slice is malformed");
        } else if (expression->kind == SOL_IR_EXPR_MATCH
            && (!sol_ir_slice_valid(expression->as.match_expr.arms, ir->arm_id_count)
                || expression->as.match_expr.scrutinee >= ir->expression_count)) {
            return sol_ir_error(diagnostics, "IR match arm slice is malformed");
        } else if (expression->kind == SOL_IR_EXPR_DEFINITION
            && (expression->as.definition >= ir->definition_count
                || ir->definitions[expression->as.definition].kind
                    == SOL_IR_DEFINITION_TEST)) {
            return sol_ir_error(diagnostics, "IR definition expression is malformed");
        } else if (expression->kind == SOL_IR_EXPR_REFINEMENT_SELF
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
                SolIrDefinitionId source_definition = SOL_IR_NONE;
                if (definition->capability_source != SOL_IR_NONE) {
                    source_definition = ir->types[ir->locals[
                        definition->capability_source
                    ].type].definition;
                }
                if (operands.count != 1
                    || definition->capability_source == SOL_IR_NONE
                    || ir->operands[operands.offset].formal != 0
                    || ir->operands[operands.offset].access != SOL_ACCESS_OWNED
                    || ir->operands[operands.offset].value >= ir->expression_count
                    || !sol_ir_expression_capability_definition(ir,
                        ir->operands[operands.offset].value, NULL)
                    || ir->types[ir->expressions[ir->operands[
                        operands.offset
                    ].value].type].definition != source_definition) {
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
                        || operand->access != SOL_ACCESS_OWNED
                        || operand->formal >= ir->field_count
                        || ir->fields[operand->formal].owner
                            != expression->as.record.definition
                        || operand->formal != expected) {
                        return sol_ir_error(diagnostics,
                            "IR record fields are not complete and canonical");
                    }
                }
            }
        } else if (expression->kind == SOL_IR_EXPR_TUPLE) {
            SolIrSlice operands = expression->as.tuple.operands;
            if (!sol_ir_slice_valid(operands, ir->operand_count)
                || operands.count < 2 || operands.count > 16) {
                return sol_ir_error(diagnostics, "IR tuple constructor is malformed");
            }
            for (size_t ordinal = 0; ordinal < operands.count; ++ordinal) {
                const SolIrOperand *operand = &ir->operands[operands.offset + ordinal];
                if (operand->formal != ordinal || operand->access != SOL_ACCESS_OWNED
                    || operand->value >= ir->expression_count) {
                    return sol_ir_error(diagnostics,
                        "IR tuple operands are not complete and canonical");
                }
            }
        } else if (expression->kind == SOL_IR_EXPR_HANDLE) {
            SolIrDefinitionId authority_definition = SOL_IR_NONE;
            SolIrDefinitionId provider_definition = SOL_IR_NONE;
            if (expression->as.handler.effect_name == NULL
                || expression->as.handler.authority >= ir->expression_count
                || expression->as.handler.provider >= ir->expression_count
                || expression->as.handler.body >= ir->expression_count
                || expression->as.handler.source >= ir->callable_count
                || expression->as.handler.provider_callable >= ir->callable_count
                || expression->as.handler.root >= ir->local_count
                || (validate_ownership
                    && (!sol_ir_local_place(ir,
                            expression->as.handler.authority, NULL)
                        || !sol_ir_local_place(ir,
                            expression->as.handler.provider, NULL)))
                || ir->callables[expression->as.handler.source].kind
                    != SOL_IR_CALLABLE_CAPABILITY
                || ir->callables[expression->as.handler.provider_callable].kind
                    != SOL_IR_CALLABLE_CAPABILITY
                || !sol_ir_expression_capability_definition(ir,
                    expression->as.handler.authority, &authority_definition)
                || !sol_ir_expression_capability_definition(ir,
                    expression->as.handler.provider, &provider_definition)
                || ir->callables[expression->as.handler.source].owner
                    != authority_definition
                || ir->callables[expression->as.handler.provider_callable].owner
                    != provider_definition
                || !sol_ir_callable_shapes_equal(ir,
                    expression->as.handler.source,
                    expression->as.handler.provider_callable)
                || ir->locals[expression->as.handler.root].type >= ir->type_count
                || ir->types[ir->locals[expression->as.handler.root].type].kind
                    != SOL_IR_TYPE_NOMINAL
                || ir->types[ir->locals[expression->as.handler.root].type].definition
                    != authority_definition
                || !sol_ir_callable_effect_has_self(ir,
                    expression->as.handler.source,
                    expression->as.handler.effect_name)) {
                return sol_ir_error(diagnostics, "IR handler is malformed");
            }
        } else if (expression->kind == SOL_IR_EXPR_SNAPSHOT_READ
            && expression->as.snapshot >= ir->snapshot_count) {
            return sol_ir_error(diagnostics, "IR snapshot read is out of range");
        }
        if (!sol_ir_expression_types_valid(ir, index)) {
            return sol_ir_indexed_error(diagnostics,
                "IR expression operand or result type is inconsistent", index,
                expression->kind == SOL_IR_EXPR_CALL
                    ? 1000 + (int)expression->as.call.kind : (int)expression->kind);
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
        if (expression->kind == SOL_IR_EXPR_BLOCK) {
            size_t cleanup = 0;
            for (size_t item = 0; item < expression->as.block.statements.count;
                ++item) {
                SolIrStatementId statement_id = ir->statement_ids[
                    expression->as.block.statements.offset + item];
                if (statement_id >= ir->statement_count) {
                    return sol_ir_error(diagnostics,
                        "IR block statement ID is out of range");
                }
                for (size_t previous_expression = 0;
                    previous_expression < index; ++previous_expression) {
                    const SolIrExpression *previous
                        = &ir->expressions[previous_expression];
                    if (previous->kind != SOL_IR_EXPR_BLOCK) continue;
                    for (size_t previous_item = 0;
                        previous_item < previous->as.block.statements.count;
                        ++previous_item) {
                        if (ir->statement_ids[
                                previous->as.block.statements.offset + previous_item]
                            == statement_id) {
                            return sol_ir_error(diagnostics,
                                "IR statement is shared by executable blocks");
                        }
                    }
                }
                const SolIrStatement *statement = &ir->statements[statement_id];
                if ((statement->kind == SOL_IR_STATEMENT_BREAK
                        || statement->kind == SOL_IR_STATEMENT_CONTINUE)
                    && item + 1 != expression->as.block.statements.count) {
                    return sol_ir_error(diagnostics,
                        "IR loop exit is not final in its block");
                }
                if (statement->kind == SOL_IR_STATEMENT_LET
                    || statement->kind == SOL_IR_STATEMENT_DECLARE) {
                    if (cleanup >= expression->as.block.cleanup.count
                        || ir->cleanup_locals[
                            expression->as.block.cleanup.offset + cleanup]
                            != statement->local
                        || statement->local >= ir->local_count
                        || ir->locals[statement->local].kind
                            != SOL_IR_LOCAL_BINDING
                        || ir->locals[statement->local].access
                            != SOL_ACCESS_OWNED) {
                        return sol_ir_error(diagnostics,
                            "IR block cleanup metadata is invalid");
                    }
                    ++cleanup;
                }
            }
            if (cleanup != expression->as.block.cleanup.count) {
                return sol_ir_error(diagnostics,
                    "IR block cleanup does not exactly match its bindings");
            }
        }
    }
    for (size_t index = 0; index < ir->statement_id_count; ++index) {
        if (ir->statement_ids[index] >= ir->statement_count) {
            return sol_ir_error(diagnostics, "IR statement ID is out of range");
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (ir->statement_ids[previous] == ir->statement_ids[index]) {
                return sol_ir_error(diagnostics,
                    "IR statement is shared by executable blocks");
            }
        }
    }
    for (size_t index = 0; index < ir->statement_count; ++index) {
        const SolIrStatement *statement = &ir->statements[index];
        bool binding = statement->kind == SOL_IR_STATEMENT_LET
            || statement->kind == SOL_IR_STATEMENT_DECLARE;
        bool assignment = statement->kind == SOL_IR_STATEMENT_ASSIGNMENT;
        bool modify = statement->kind == SOL_IR_STATEMENT_MODIFY;
        bool loop = statement->kind == SOL_IR_STATEMENT_LOOP
            || statement->kind == SOL_IR_STATEMENT_WHILE;
        bool exit = statement->kind == SOL_IR_STATEMENT_BREAK
            || statement->kind == SOL_IR_STATEMENT_CONTINUE;
        bool has_expression = statement->kind != SOL_IR_STATEMENT_DECLARE && !exit
            && statement->kind != SOL_IR_STATEMENT_UNREACHABLE;
        bool assignment_operator = statement->operator_kind == SOL_TOKEN_EQUAL
            || statement->operator_kind == SOL_TOKEN_PLUS_EQUAL
            || statement->operator_kind == SOL_TOKEN_MINUS_EQUAL
            || statement->operator_kind == SOL_TOKEN_STAR_EQUAL
            || statement->operator_kind == SOL_TOKEN_SLASH_EQUAL
            || statement->operator_kind == SOL_TOKEN_PERCENT_EQUAL;
        if ((int)statement->kind < 0 || statement->kind > SOL_IR_STATEMENT_REQUIRE
            || statement->span.start > statement->span.end
            || statement->span.end > ir->source_length
            || (has_expression ? statement->expression >= ir->expression_count
                : statement->expression != SOL_IR_NONE)
            || (binding
                ? statement->local >= ir->local_count
                : statement->local != SOL_IR_NONE)
            || (assignment || modify
                ? statement->target >= ir->expression_count
                : statement->target != SOL_IR_NONE)
            || (assignment ? !assignment_operator
                : statement->operator_kind != SOL_TOKEN_EOF)
            || (statement->kind == SOL_IR_STATEMENT_WHILE
                    || statement->kind == SOL_IR_STATEMENT_REQUIRE
                ? statement->condition >= ir->expression_count
                    || (!sol_ir_type_is(ir,
                            ir->expressions[statement->condition].type,
                            SOL_IR_TYPE_BOOL)
                        && !sol_ir_type_is(ir,
                            ir->expressions[statement->condition].type,
                            SOL_IR_TYPE_NEVER))
                : statement->condition != SOL_IR_NONE)
            || (loop
                && (ir->expressions[statement->expression].kind
                        != SOL_IR_EXPR_BLOCK
                    || ir->expressions[statement->expression].type >= ir->type_count
                    || (ir->types[ir->expressions[statement->expression].type].kind
                            != SOL_IR_TYPE_UNIT
                        && ir->types[ir->expressions[statement->expression].type].kind
                            != SOL_IR_TYPE_NEVER)))
            || !sol_ir_slice_valid(statement->loop_obligations,
                ir->loop_obligation_count)
            || (!loop && statement->loop_obligations.count != 0)
            || !sol_ir_slice_valid(statement->unreachable_obligations,
                ir->unreachable_obligation_count)
            || (statement->kind == SOL_IR_STATEMENT_UNREACHABLE
                ? statement->unreachable_obligations.count != 1
                : statement->unreachable_obligations.count != 0)
            || (statement->kind == SOL_IR_STATEMENT_PANIC
                && !sol_ir_type_is(ir,
                    ir->expressions[statement->expression].type,
                    SOL_IR_TYPE_TEXT))
            || (statement->kind == SOL_IR_STATEMENT_REQUIRE
                && (!sol_ir_type_is(ir,
                        ir->expressions[statement->condition].type,
                        SOL_IR_TYPE_BOOL)
                    || !sol_ir_type_is(ir,
                        ir->expressions[statement->expression].type,
                        SOL_IR_TYPE_NEVER)))
            || (statement->kind == SOL_IR_STATEMENT_REGION
                ? !sol_ir_region_label_valid(ir, statement)
                    || statement->region_label_span.start
                        >= statement->region_label_span.end
                    || statement->region_label_span.start < statement->span.start
                    || statement->region_label_span.end > statement->span.end
                    || ir->expressions[statement->expression].kind != SOL_IR_EXPR_BLOCK
                    || ir->expressions[statement->expression].type >= ir->type_count
                    || (ir->types[ir->expressions[statement->expression].type].kind
                            != SOL_IR_TYPE_UNIT
                        && ir->types[ir->expressions[statement->expression].type].kind
                            != SOL_IR_TYPE_NEVER)
                : statement->region_label != NULL
                    || statement->region_label_span.start != 0
                    || statement->region_label_span.end != 0)) {
            return sol_ir_error(diagnostics, "malformed IR statement");
        }
        if (binding) {
            if (ir->locals[statement->local].kind != SOL_IR_LOCAL_BINDING) {
                return sol_ir_error(diagnostics, "IR binding target is not a binding local");
            }
            for (size_t previous = 0; previous < index; ++previous) {
                if ((ir->statements[previous].kind == SOL_IR_STATEMENT_LET
                        || ir->statements[previous].kind == SOL_IR_STATEMENT_DECLARE)
                    && ir->statements[previous].local == statement->local) {
                    return sol_ir_error(diagnostics, "IR binding is duplicated");
                }
            }
            if ((statement->kind == SOL_IR_STATEMENT_DECLARE
                    && !ir->locals[statement->local].mutable)
                || (statement->kind == SOL_IR_STATEMENT_LET
                    && statement->expression == SOL_IR_NONE)) {
                return sol_ir_error(diagnostics, "IR binding initialization is invalid");
            }
        } else if (statement->kind == SOL_IR_STATEMENT_ASSIGNMENT) {
            const SolIrExpression *target = &ir->expressions[statement->target];
            const SolIrExpression *value = &ir->expressions[statement->expression];
            SolIrLocalId local_id = SOL_IR_NONE;
            if (!sol_ir_local_place(ir, statement->target, &local_id)) {
                return sol_ir_error(diagnostics,
                    "IR assignment target is not a local field place");
            }
            const SolIrLocal *local = &ir->locals[local_id];
            const SolIrPlace *place = sol_ir_expression_place(ir, statement->target);
            bool writable = (local->kind == SOL_IR_LOCAL_BINDING
                    && local->access == SOL_ACCESS_OWNED)
                || (local->kind == SOL_IR_LOCAL_PARAMETER
                    && (local->access == SOL_ACCESS_EXCLUSIVE
                        || local->access == SOL_ACCESS_OWNED));
            bool compound = statement->operator_kind != SOL_TOKEN_EQUAL;
            if (statement->target == statement->expression
                || !sol_ir_slice_valid(local->capability_roots, ir->root_count)
                || !sol_ir_slice_valid(local->operation_roots, ir->root_count)
                || !writable
                || (place != NULL && place->projections.count == 0
                    && ir->types[value->type].kind != SOL_IR_TYPE_NEVER
                    && target->capability_roots.count == 0
                    && target->operation_roots.count == 0
                    && value->capability_roots.count == 0
                    && value->operation_roots.count == 0
                    && (sol_ir_type_may_carry_authority(ir, target->type, true)
                        || sol_ir_type_may_carry_authority(ir, value->type, true)))
                || (compound && (!sol_ir_type_is(ir, target->type,
                        SOL_IR_TYPE_INT64)
                    || (!sol_ir_type_is(ir, value->type, SOL_IR_TYPE_INT64)
                        && !sol_ir_type_is(ir, value->type, SOL_IR_TYPE_NEVER))))
                || target->kind != SOL_IR_EXPR_PLACE
                || place == NULL || target->type != place->type
                || (value->type != target->type
                    && ir->types[value->type].kind != SOL_IR_TYPE_NEVER)
                || target->capability_roots.count != local->capability_roots.count
                || target->operation_roots.count != local->operation_roots.count
                || (place->projections.count != 0
                    && (target->capability_roots.count != 0
                        || target->operation_roots.count != 0
                        || sol_ir_type_may_carry_authority(
                            ir, target->type, true)
                        || (ir->types[value->type].kind != SOL_IR_TYPE_NEVER
                            && (sol_ir_type_may_carry_authority(
                                    ir, value->type, true)
                                || value->capability_roots.count != 0
                                || value->operation_roots.count != 0))))
                || (ir->types[value->type].kind != SOL_IR_TYPE_NEVER
                    && (value->capability_roots.count
                            != local->capability_roots.count
                        || value->operation_roots.count
                            != local->operation_roots.count))) {
                return sol_ir_error(diagnostics,
                    "IR assignment target, type, or provenance is invalid");
            }
            SolIrSlice slices[4] = {target->capability_roots,
                target->operation_roots,
                ir->types[value->type].kind == SOL_IR_TYPE_NEVER
                    ? local->capability_roots : value->capability_roots,
                ir->types[value->type].kind == SOL_IR_TYPE_NEVER
                    ? local->operation_roots : value->operation_roots};
            SolIrSlice expected[4] = {local->capability_roots,
                local->operation_roots, local->capability_roots,
                local->operation_roots};
            for (size_t slice = 0; slice < 4; ++slice) {
                for (size_t root = 0; root < slices[slice].count; ++root) {
                    if (ir->roots[slices[slice].offset + root]
                        != ir->roots[expected[slice].offset + root]) {
                        return sol_ir_error(diagnostics,
                            "IR assignment provenance does not match its local");
                    }
                }
            }
        } else if (statement->kind == SOL_IR_STATEMENT_MODIFY) {
            SolIrLocalId local_id = SOL_IR_NONE;
            const SolIrPlace *place = sol_ir_expression_place(ir, statement->target);
            const SolIrExpression *body = &ir->expressions[statement->expression];
            if (!sol_ir_local_place(ir, statement->target, &local_id)
                || place == NULL || place->projections.count != 0
                || local_id >= ir->local_count
                || ir->locals[local_id].access != SOL_ACCESS_OWNED
                || !((ir->locals[local_id].kind == SOL_IR_LOCAL_BINDING
                        && !ir->locals[local_id].mutable)
                    || ir->locals[local_id].kind == SOL_IR_LOCAL_PARAMETER)
                || body->kind != SOL_IR_EXPR_BLOCK || body->type >= ir->type_count
                || (ir->types[body->type].kind != SOL_IR_TYPE_UNIT
                    && ir->types[body->type].kind != SOL_IR_TYPE_NEVER)) {
                return sol_ir_error(diagnostics, "IR modify statement is invalid");
            }
        }
    }
    for (size_t index = 0; index < ir->arm_id_count; ++index) {
        if (ir->arm_ids[index] >= ir->arm_count) {
            return sol_ir_error(diagnostics, "IR arm ID is out of range");
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (ir->arm_ids[previous] == ir->arm_ids[index]) {
                return sol_ir_error(diagnostics,
                    "IR match arm is shared by executable matches");
            }
        }
    }
    for (size_t index = 0; index < ir->pattern_count; ++index) {
        const SolIrPattern *pattern = &ir->patterns[index];
        if ((int)pattern->kind < 0 || pattern->kind > SOL_IR_PATTERN_TUPLE
            || pattern->type >= ir->type_count
            || pattern->span.start >= pattern->span.end
            || pattern->span.end > ir->source_length
            || !sol_ir_slice_valid(pattern->children, ir->pattern_child_count)) {
            return sol_ir_error(diagnostics, "malformed recursive IR pattern");
        }
        const SolIrType *type = &ir->types[pattern->type];
        bool leaf = pattern->kind == SOL_IR_PATTERN_WILDCARD
            || pattern->kind == SOL_IR_PATTERN_BOOL
            || pattern->kind == SOL_IR_PATTERN_BINDING;
        if ((leaf && pattern->children.count != 0)
            || (pattern->kind == SOL_IR_PATTERN_BOOL
                ? type->kind != SOL_IR_TYPE_BOOL
                : pattern->boolean)
            || (pattern->kind == SOL_IR_PATTERN_VARIANT
                ? (pattern->variant >= ir->variant_count
                    || type->kind != SOL_IR_TYPE_NOMINAL
                    || ir->variants[pattern->variant].owner != type->definition
                    || pattern->children.count
                        != ir->variants[pattern->variant].fields.count)
                : pattern->variant != SOL_IR_NONE)
            || (pattern->kind == SOL_IR_PATTERN_RECORD
                ? (pattern->definition >= ir->definition_count
                    || type->kind != SOL_IR_TYPE_NOMINAL
                    || type->definition != pattern->definition
                    || ir->definitions[pattern->definition].kind
                        != SOL_IR_DEFINITION_RECORD)
                : pattern->definition != SOL_IR_NONE)
            || (pattern->kind == SOL_IR_PATTERN_BINDING
                ? (pattern->binding >= ir->local_count
                    || ir->locals[pattern->binding].kind != SOL_IR_LOCAL_PATTERN
                    || ir->locals[pattern->binding].type != pattern->type
                    || ir->locals[pattern->binding].access != SOL_ACCESS_OWNED)
                : pattern->binding != SOL_IR_NONE)
            || (pattern->kind == SOL_IR_PATTERN_TUPLE
                && (type->kind != SOL_IR_TYPE_TUPLE
                    || pattern->children.count != type->argument_count))) {
            return sol_ir_error(diagnostics, "IR pattern payload metadata is invalid");
        }
        for (size_t child = 0; child < pattern->children.count; ++child) {
            const SolIrPatternChild *edge
                = &ir->pattern_children[pattern->children.offset + child];
            if (edge->pattern >= ir->pattern_count) {
                return sol_ir_error(diagnostics, "IR pattern child is out of range");
            }
            if (ir->patterns[edge->pattern].span.start < pattern->span.start
                || ir->patterns[edge->pattern].span.end > pattern->span.end) {
                return sol_ir_error(diagnostics,
                    "IR child pattern span is outside its parent");
            }
            SolIrTypeId expected = SOL_IR_NONE;
            if (pattern->kind == SOL_IR_PATTERN_TUPLE) {
                if (edge->field != SOL_IR_NONE || edge->ordinal != child) {
                    return sol_ir_error(diagnostics, "IR tuple pattern ordinal is invalid");
                }
                expected = ir->type_ids[type->argument_offset + child];
            } else if (pattern->kind == SOL_IR_PATTERN_VARIANT) {
                SolIrSlice fields = ir->variants[pattern->variant].fields;
                if (edge->field != SOL_IR_NONE || edge->ordinal != child) {
                    return sol_ir_error(diagnostics, "IR variant payload ordinal is invalid");
                }
                SolIrFieldId field = fields.offset + child;
                const SolIrDefinition *definition = &ir->definitions[type->definition];
                for (size_t candidate = 0; candidate < ir->type_count; ++candidate) {
                    if (sol_ir_type_matches_instantiation(ir, candidate,
                        ir->fields[field].type, definition->generic_parameters,
                        (SolIrSlice){type->argument_offset, type->argument_count},
                        SOL_IR_NONE, 0)) {
                        expected = candidate;
                        break;
                    }
                }
            } else if (pattern->kind == SOL_IR_PATTERN_RECORD) {
                if (edge->field >= ir->field_count || edge->ordinal != SOL_IR_NONE
                    || ir->fields[edge->field].owner != pattern->definition) {
                    return sol_ir_error(diagnostics, "IR record pattern field is invalid");
                }
                for (size_t previous = 0; previous < child; ++previous) {
                    if (ir->pattern_children[pattern->children.offset + previous].field
                        == edge->field) return sol_ir_error(diagnostics,
                            "IR record pattern field is duplicated");
                }
                const SolIrDefinition *definition
                    = &ir->definitions[pattern->definition];
                for (size_t candidate = 0; candidate < ir->type_count; ++candidate) {
                    if (sol_ir_type_matches_instantiation(ir, candidate,
                        ir->fields[edge->field].type,
                        definition->generic_parameters,
                        (SolIrSlice){type->argument_offset, type->argument_count},
                        SOL_IR_NONE, 0)) {
                        expected = candidate;
                        break;
                    }
                }
            } else {
                return sol_ir_error(diagnostics, "IR leaf pattern owns child edges");
            }
            if (expected == SOL_IR_NONE
                || ir->patterns[edge->pattern].type != expected) {
                return sol_ir_error(diagnostics, "IR pattern child type is invalid");
            }
        }
    }
    for (size_t index = 0; index < ir->arm_count; ++index) {
        const SolIrArm *arm = &ir->arms[index];
        if (arm->pattern >= ir->pattern_count || arm->body >= ir->expression_count
            || (arm->guard != SOL_IR_NONE
                && (arm->guard >= ir->expression_count
                    || !sol_ir_type_is(ir, ir->expressions[arm->guard].type,
                        SOL_IR_TYPE_BOOL)))
            || arm->span.start > arm->span.end || arm->span.end > ir->source_length
            || ir->patterns[arm->pattern].span.start < arm->span.start
            || ir->patterns[arm->pattern].span.end > arm->span.end
            || !sol_ir_slice_valid(arm->bindings, ir->root_count)
            || !sol_ir_slice_valid(arm->cleanup, ir->cleanup_local_count)
            || arm->cleanup.count != arm->bindings.count) {
            return sol_ir_error(diagnostics, "malformed IR match arm");
        }
        if (arm->guard != SOL_IR_NONE) {
            unsigned char *states = sol_ir_allocate(
                ir->expression_count, sizeof(*states), true);
            bool pure = states != NULL
                && sol_ir_guard_expression_pure(ir, arm->guard, states, 0);
            free(states);
            if (!pure) return sol_ir_error(diagnostics,
                "IR match guard is effectful or imperative");
        }
        for (size_t binding = 0; binding < arm->bindings.count; ++binding) {
            SolIrLocalId local = ir->roots[arm->bindings.offset + binding];
            if (local >= ir->local_count
                || ir->cleanup_locals[arm->cleanup.offset + arm->cleanup.count
                    - 1 - binding] != local
                || ir->locals[local].kind != SOL_IR_LOCAL_PATTERN) {
                return sol_ir_error(diagnostics, "IR match binding cleanup is invalid");
            }
            for (size_t previous = 0; previous < binding; ++previous) {
                if (ir->roots[arm->bindings.offset + previous] == local) {
                    return sol_ir_error(diagnostics, "IR match binding is duplicated");
                }
            }
        }
    }
    for (size_t index = 0; index < ir->expression_count; ++index) {
        if (ir->expressions[index].kind == SOL_IR_EXPR_MATCH
            && !sol_ir_match_coverage_valid(ir, &ir->expressions[index])) {
            return sol_ir_error(diagnostics,
                "IR match patterns are not useful, exhaustive, or within the complexity bound");
        }
    }
    for (size_t index = 0; index < ir->local_count; ++index) {
        const SolIrLocal *local = &ir->locals[index];
        if ((int)local->kind < 0 || local->kind > SOL_IR_LOCAL_PATTERN
            || local->owner >= ir->definition_count
            || local->name == NULL || local->type >= ir->type_count
            || (local->kind != SOL_IR_LOCAL_BINDING && local->mutable)
            || !sol_ir_slice_valid(local->capability_roots, ir->root_count)
            || !sol_ir_slice_valid(local->operation_roots, ir->root_count)) {
            return sol_ir_error(diagnostics, "malformed IR local");
        }
        SolIrSlice provenances[2] = {local->capability_roots, local->operation_roots};
        for (size_t provenance = 0; provenance < 2; ++provenance) {
            SolIrSlice roots = provenances[provenance];
            for (size_t root_index = 0; root_index < roots.count; ++root_index) {
                SolIrLocalId root = ir->roots[roots.offset + root_index];
                if (root >= ir->local_count) {
                    return sol_ir_error(diagnostics, "IR local provenance is malformed");
                }
                SolIrTypeId root_type = ir->locals[root].type;
                if (ir->locals[root].kind != SOL_IR_LOCAL_PARAMETER
                    || ir->locals[root].owner != local->owner
                    || root_type >= ir->type_count
                    || ir->types[root_type].kind != SOL_IR_TYPE_NOMINAL
                    || ir->types[root_type].definition >= ir->definition_count
                    || ir->definitions[ir->types[root_type].definition].kind
                        != SOL_IR_DEFINITION_CAPABILITY
                    || (root_index != 0
                        && ir->roots[roots.offset + root_index - 1] >= root)) {
                    return sol_ir_error(diagnostics, "IR local provenance is malformed");
                }
            }
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
        size_t owners = 0;
        for (size_t definition = 0; definition < ir->definition_count; ++definition) {
            SolIrSlice fields = ir->definitions[definition].fields;
            if (index >= fields.offset && index - fields.offset < fields.count) ++owners;
        }
        for (size_t variant = 0; variant < ir->variant_count; ++variant) {
            SolIrSlice fields = ir->variants[variant].fields;
            if (index >= fields.offset && index - fields.offset < fields.count) ++owners;
        }
        if (owners != 1) return sol_ir_error(diagnostics,
            "IR field is missing or duplicated in its owner slice");
    }
    for (size_t index = 0; index < ir->variant_count; ++index) {
        if (ir->variants[index].name == NULL
            || ir->variants[index].owner >= ir->definition_count
            || ir->definitions[ir->variants[index].owner].kind != SOL_IR_DEFINITION_ENUM
            || !sol_ir_slice_valid(ir->variants[index].fields, ir->field_count)) {
            return sol_ir_error(diagnostics, "malformed canonical IR variant");
        }
        for (size_t field = 0; field < ir->variants[index].fields.count; ++field) {
            if (ir->fields[ir->variants[index].fields.offset + field].owner
                != ir->variants[index].owner) {
                return sol_ir_error(diagnostics, "IR variant field has the wrong owner");
            }
        }
        size_t owners = 0;
        for (size_t definition = 0; definition < ir->definition_count; ++definition) {
            SolIrSlice variants = ir->definitions[definition].variants;
            if (index >= variants.offset && index - variants.offset < variants.count) ++owners;
        }
        if (owners != 1) return sol_ir_error(diagnostics,
            "IR variant is missing or duplicated in its owner slice");
    }
    for (size_t index = 0; index < ir->obligation_count; ++index) {
        const SolIrObligation *obligation = &ir->obligations[index];
        if (obligation->id != index || obligation->predicate >= ir->expression_count
            || (int)obligation->owner_kind < 0
            || obligation->owner_kind > SOL_CONTRACT_OWNER_TYPE
            || (int)obligation->kind < 0 || obligation->kind > SOL_CONTRACT_ENSURES
            || (int)obligation->outcome < 0
            || obligation->outcome > SOL_CONTRACT_OUTCOME_FAILURE
            || !sol_ir_slice_valid(obligation->snapshots, ir->snapshot_count)
            || (obligation->result_available && obligation->result_type >= ir->type_count)
            || (!obligation->result_available && obligation->result_type != SOL_IR_NONE)
            || !sol_ir_type_is(ir, ir->expressions[obligation->predicate].type,
                SOL_IR_TYPE_BOOL)
            || (obligation->owner_kind == SOL_CONTRACT_OWNER_CAPABILITY_MEMBER
                && obligation->owner >= ir->callable_count)
            || ((obligation->owner_kind == SOL_CONTRACT_OWNER_ITEM
                    || obligation->owner_kind == SOL_CONTRACT_OWNER_TYPE)
                && obligation->owner >= ir->definition_count)) {
            return sol_ir_error(diagnostics, "malformed canonical IR obligation");
        }
    }
    for (size_t index = 0; index < ir->snapshot_count; ++index) {
        const SolIrSnapshot *snapshot = &ir->snapshots[index];
        if (snapshot->id != index || snapshot->obligation >= ir->obligation_count
            || snapshot->read >= ir->expression_count
            || snapshot->operand >= ir->expression_count || snapshot->type >= ir->type_count
            || ir->expressions[snapshot->read].kind != SOL_IR_EXPR_SNAPSHOT_READ
            || ir->expressions[snapshot->read].as.snapshot != index
            || ir->expressions[snapshot->read].type != snapshot->type
            || ir->expressions[snapshot->operand].type != snapshot->type) {
            return sol_ir_error(diagnostics, "malformed canonical IR snapshot");
        }
    }
    for (size_t index = 0; index < ir->loop_obligation_count; ++index) {
        const SolIrLoopObligation *obligation = &ir->loop_obligations[index];
        bool invariant = obligation->kind == SOL_LOOP_OBLIGATION_INVARIANT_ENTRY
            || obligation->kind == SOL_LOOP_OBLIGATION_INVARIANT_PRESERVATION;
        if (obligation->id != index
            || (int)obligation->kind < 0
            || obligation->kind > SOL_LOOP_OBLIGATION_DECREASES_STRICT
            || obligation->loop_statement >= ir->statement_count
            || (ir->statements[obligation->loop_statement].kind
                    != SOL_IR_STATEMENT_LOOP
                && ir->statements[obligation->loop_statement].kind
                    != SOL_IR_STATEMENT_WHILE)
            || obligation->callable >= ir->callable_count
            || obligation->expression >= ir->expression_count
            || obligation->expression_type >= ir->type_count
            || obligation->expression_type
                != ir->expressions[obligation->expression].type
            || !sol_ir_type_is(ir, obligation->expression_type,
                invariant ? SOL_IR_TYPE_BOOL : SOL_IR_TYPE_INT64)
            || obligation->span.start >= obligation->span.end
            || obligation->span.end > ir->source_length
            || ir->expressions[obligation->expression].span.start
                < obligation->span.start
            || ir->expressions[obligation->expression].span.end
                > obligation->span.end) {
            return sol_ir_error(diagnostics,
                "malformed canonical IR loop obligation");
        }
        size_t owners = 0;
        for (size_t statement_id = 0; statement_id < ir->statement_count;
            ++statement_id) {
            SolIrSlice slice = ir->statements[statement_id].loop_obligations;
            if (index >= slice.offset && index - slice.offset < slice.count) {
                ++owners;
                if (statement_id != obligation->loop_statement) {
                    return sol_ir_error(diagnostics,
                        "IR loop obligation has the wrong owner slice");
                }
            }
        }
        if (owners != 1) return sol_ir_error(diagnostics,
            "IR loop obligation is missing or duplicated in its owner slice");
    }
    for (size_t index = 0; index < ir->unreachable_obligation_count; ++index) {
        const SolIrUnreachableObligation *obligation
            = &ir->unreachable_obligations[index];
        if (obligation->id != index
            || obligation->statement >= ir->statement_count
            || ir->statements[obligation->statement].kind
                != SOL_IR_STATEMENT_UNREACHABLE
            || obligation->callable >= ir->callable_count
            || obligation->proof >= ir->expression_count
            || obligation->proof_type >= ir->type_count
            || obligation->proof_type != ir->expressions[obligation->proof].type
            || !sol_ir_type_is(ir, obligation->proof_type, SOL_IR_TYPE_BOOL)
            || obligation->span.start >= obligation->span.end
            || obligation->span.end > ir->source_length
            || ir->expressions[obligation->proof].span.start < obligation->span.start
            || ir->expressions[obligation->proof].span.end > obligation->span.end) {
            return sol_ir_error(diagnostics,
                "malformed canonical IR unreachable obligation");
        }
        size_t owners = 0;
        for (size_t statement_id = 0; statement_id < ir->statement_count;
            ++statement_id) {
            SolIrSlice slice
                = ir->statements[statement_id].unreachable_obligations;
            if (index >= slice.offset && index - slice.offset < slice.count) {
                ++owners;
                if (statement_id != obligation->statement) {
                    return sol_ir_error(diagnostics,
                        "IR unreachable obligation has the wrong owner slice");
                }
            }
        }
        if (owners != 1) return sol_ir_error(diagnostics,
            "IR unreachable obligation is missing or duplicated in its owner slice");
        if (index != 0) {
            const SolIrUnreachableObligation *previous
                = &ir->unreachable_obligations[index - 1];
            if (previous->span.start > obligation->span.start
                || (previous->span.start == obligation->span.start
                    && (previous->span.end > obligation->span.end
                        || (previous->span.end == obligation->span.end
                            && previous->statement >= obligation->statement)))) {
                return sol_ir_error(diagnostics,
                    "IR unreachable obligations are not in canonical order");
            }
        }
        for (size_t contract = 0; contract < ir->obligation_count; ++contract) {
            unsigned char *contract_states = ir->expression_count == 0 ? NULL
                : calloc(ir->expression_count, 1);
            unsigned char *proof_states = ir->expression_count == 0 ? NULL
                : calloc(ir->expression_count, 1);
            if (ir->expression_count != 0
                && (contract_states == NULL || proof_states == NULL)) {
                free(contract_states);
                free(proof_states);
                return sol_ir_error(diagnostics,
                    "IR unreachable proof validation allocation failed");
            }
            bool shared = sol_ir_expression_reaches(ir,
                ir->obligations[contract].predicate, SOL_IR_NONE,
                contract_states, 0);
            for (SolIrExpressionId expression = 0;
                !shared && expression < ir->expression_count; ++expression) {
                if (contract_states[expression] == 0) continue;
                memset(proof_states, 0, ir->expression_count);
                shared = sol_ir_expression_reaches(ir, obligation->proof,
                    expression, proof_states, 0);
            }
            free(contract_states);
            free(proof_states);
            if (shared) return sol_ir_error(diagnostics,
                "IR unreachable proof expression is shared with a contract predicate");
        }
    }
    for (size_t statement_id = 0; statement_id < ir->statement_count;
        ++statement_id) {
        const SolIrStatement *statement = &ir->statements[statement_id];
        SolIrSlice slice = statement->loop_obligations;
        if (slice.count == 0) continue;
        if ((slice.count & 1u) != 0) return sol_ir_error(diagnostics,
            "IR loop obligation cardinality is not paired");
        bool saw_decreases = false;
        for (size_t offset = 0; offset < slice.count; offset += 2) {
            const SolIrLoopObligation *first
                = &ir->loop_obligations[slice.offset + offset];
            const SolIrLoopObligation *second
                = &ir->loop_obligations[slice.offset + offset + 1];
            bool invariant = first->kind == SOL_LOOP_OBLIGATION_INVARIANT_ENTRY;
            if ((!invariant
                    && first->kind != SOL_LOOP_OBLIGATION_DECREASES_NONNEGATIVE)
                || (invariant && saw_decreases)
                || second->kind != (invariant
                    ? SOL_LOOP_OBLIGATION_INVARIANT_PRESERVATION
                    : SOL_LOOP_OBLIGATION_DECREASES_STRICT)
                || first->loop_statement != statement_id
                || second->loop_statement != statement_id
                || first->callable != second->callable
                || first->expression != second->expression
                || first->expression_type != second->expression_type
                || first->span.start != second->span.start
                || first->span.end != second->span.end) {
                return sol_ir_error(diagnostics,
                    "IR loop obligation pair or order is invalid");
            }
            size_t current = slice.offset + offset;
            for (size_t previous = 0; previous < current; ++previous) {
                SolLoopObligationKind kind = ir->loop_obligations[previous].kind;
                if ((kind == SOL_LOOP_OBLIGATION_INVARIANT_ENTRY
                        || kind == SOL_LOOP_OBLIGATION_DECREASES_NONNEGATIVE)
                    && ir->loop_obligations[previous].expression
                        == first->expression) {
                    return sol_ir_error(diagnostics,
                        "IR loop obligation clause is duplicated");
                }
            }
            for (size_t obligation = 0; obligation < ir->obligation_count;
                ++obligation) {
                unsigned char *predicate_states = ir->expression_count == 0 ? NULL
                    : calloc(ir->expression_count, 1);
                unsigned char *loop_states = ir->expression_count == 0 ? NULL
                    : calloc(ir->expression_count, 1);
                if (ir->expression_count != 0
                    && (predicate_states == NULL || loop_states == NULL)) {
                    free(predicate_states);
                    free(loop_states);
                    return sol_ir_error(diagnostics,
                        "IR contract proof validation allocation failed");
                }
                bool shared = sol_ir_expression_reaches(ir,
                    ir->obligations[obligation].predicate, SOL_IR_NONE,
                    predicate_states, 0);
                for (SolIrExpressionId expression = 0;
                    !shared && expression < ir->expression_count; ++expression) {
                    if (predicate_states[expression] == 0) continue;
                    memset(loop_states, 0, ir->expression_count);
                    shared = sol_ir_expression_reaches(ir, first->expression,
                        expression, loop_states, 0);
                }
                free(predicate_states);
                free(loop_states);
                if (shared) {
                    return sol_ir_error(diagnostics,
                        "IR loop proof expression is shared with a contract predicate");
                }
            }
            if (!invariant) {
                if (saw_decreases || offset + 2 != slice.count) {
                    return sol_ir_error(diagnostics,
                        "IR loop decreases obligations are not final and unique");
                }
                saw_decreases = true;
            }
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
    unsigned char *states = ir->expression_count == 0 ? NULL
        : calloc(ir->expression_count, 1);
    SolIrCallableId *statement_callables = ir->statement_count == 0 ? NULL
        : malloc(ir->statement_count * sizeof(*statement_callables));
    SolIrCallableId *local_callables = ir->local_count == 0 ? NULL
        : malloc(ir->local_count * sizeof(*local_callables));
    bool *introduced = ir->local_count == 0 ? NULL
        : calloc(ir->local_count, sizeof(*introduced));
    if ((ir->expression_count != 0 && states == NULL)
        || (ir->statement_count != 0 && statement_callables == NULL)
        || (ir->local_count != 0 && local_callables == NULL)
        || (ir->local_count != 0 && introduced == NULL)) {
        free(states);
        free(statement_callables);
        free(local_callables);
        free(introduced);
        return sol_ir_error(diagnostics, "IR executable validation allocation failed");
    }
    for (size_t index = 0; index < ir->statement_count; ++index) {
        statement_callables[index] = SOL_IR_NONE;
    }
    for (size_t index = 0; index < ir->local_count; ++index) {
        local_callables[index] = SOL_IR_NONE;
    }
    for (size_t index = 0; index < ir->callable_count; ++index) {
        const SolIrCallable *callable = &ir->callables[index];
        for (size_t parameter = 0; parameter < callable->parameters.count; ++parameter) {
            SolIrLocalId local = ir->roots[callable->parameters.offset + parameter];
            if (local_callables[local] != SOL_IR_NONE
                && local_callables[local] != index) {
                free(states); free(statement_callables); free(local_callables);
                free(introduced);
                return sol_ir_error(diagnostics, "IR local is shared by callables");
            }
            local_callables[local] = index;
        }
        if (callable->receiver != SOL_IR_NONE) {
            if (local_callables[callable->receiver] != SOL_IR_NONE
                && local_callables[callable->receiver] != index) {
                free(states); free(statement_callables); free(local_callables);
                free(introduced);
                return sol_ir_error(diagnostics, "IR receiver is shared by callables");
            }
            local_callables[callable->receiver] = index;
        }
    }
    for (size_t pass = 0; pass < 2; ++pass) {
        if (ir->expression_count != 0) memset(states, 0, ir->expression_count);
        for (size_t index = 0; index < ir->callable_count; ++index) {
            if (ir->local_count != 0) memset(introduced, 0,
                ir->local_count * sizeof(*introduced));
            const SolIrCallable *callable = &ir->callables[index];
            for (size_t parameter = 0; parameter < callable->parameters.count;
                ++parameter) {
                introduced[ir->roots[callable->parameters.offset + parameter]] = true;
            }
            if (callable->receiver != SOL_IR_NONE) introduced[callable->receiver] = true;
            if (callable->capability_source != SOL_IR_NONE) {
                introduced[callable->capability_source] = true;
            }
            SolIrExpressionId body = callable->body;
            if (body != SOL_IR_NONE
                && (!sol_ir_type_assignable(ir, ir->expressions[body].type,
                        callable->result, (SolIrSlice){0}, (SolIrSlice){0}, SOL_IR_NONE)
                    || !sol_ir_executable_expression(ir, body, callable->owner,
                        index, callable->result, states, statement_callables,
                        local_callables, introduced, 0, NULL))) {
                free(states);
                free(statement_callables);
                free(local_callables);
                free(introduced);
                char message[192];
                int written = snprintf(message, sizeof(message),
                    "callable '%s' expression is shared, cyclic, non-runtime, or ill-typed",
                    callable->name);
                return written > 0 && (size_t)written < sizeof(message)
                    ? sol_ir_indexed_error(diagnostics, message, index, (int)callable->kind)
                    : sol_ir_error(diagnostics, "executable expression is invalid");
            }
        }
    }
    unsigned char *proof_states = ir->expression_count == 0 ? NULL
        : calloc(ir->expression_count, 1);
    SolIrCallableId *proof_callables = ir->expression_count == 0 ? NULL
        : malloc(ir->expression_count * sizeof(*proof_callables));
    if (ir->expression_count != 0
        && (proof_states == NULL || proof_callables == NULL)) {
        free(proof_states);
        free(proof_callables);
        free(states);
        free(statement_callables);
        free(local_callables);
        free(introduced);
        return sol_ir_error(diagnostics,
            "IR proof-expression validation allocation failed");
    }
    for (size_t index = 0; index < ir->expression_count; ++index) {
        proof_callables[index] = SOL_IR_NONE;
    }
    for (size_t index = 0; index < ir->loop_obligation_count; ++index) {
        const SolIrLoopObligation *obligation = &ir->loop_obligations[index];
        if (obligation->kind == SOL_LOOP_OBLIGATION_INVARIANT_PRESERVATION
            || obligation->kind == SOL_LOOP_OBLIGATION_DECREASES_STRICT) continue;
        if (obligation->callable >= ir->callable_count
            || statement_callables[obligation->loop_statement]
                != obligation->callable
            || !sol_ir_proof_expression_non_executable(ir,
                obligation->expression, obligation->callable, states,
                proof_states, proof_callables, local_callables, NULL, 0)) {
            free(proof_states);
            free(proof_callables);
            free(states);
            free(statement_callables);
            free(local_callables);
            free(introduced);
            return sol_ir_error(diagnostics,
                "IR loop proof expression is executable or shared with runtime code");
        }
    }
    for (size_t index = 0; index < ir->unreachable_obligation_count; ++index) {
        const SolIrUnreachableObligation *obligation
            = &ir->unreachable_obligations[index];
        if (obligation->callable >= ir->callable_count
            || statement_callables[obligation->statement] != obligation->callable
            || !sol_ir_proof_expression_non_executable(ir, obligation->proof,
                obligation->callable, states, proof_states, proof_callables,
                local_callables, NULL, 0)) {
            free(proof_states);
            free(proof_callables);
            free(states);
            free(statement_callables);
            free(local_callables);
            free(introduced);
            return sol_ir_error(diagnostics,
                "IR unreachable proof expression is executable, impure, or shared");
        }
    }
    free(proof_states);
    free(proof_callables);
    free(states);
    free(statement_callables);
    free(local_callables);
    free(introduced);
    if (!sol_ir_validate_pattern_ownership(ir, diagnostics)
        || !sol_ir_validate_arena_ownership(ir, diagnostics)) return false;
    return !validate_ownership || sol_ir_validate_ownership(ir, diagnostics);
}

bool sol_ir_validate(const SolIr *ir, SolDiagnostics *diagnostics) {
    return sol_ir_validate_impl(ir, diagnostics, true);
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
    if (valid && !lowerer.failed) {
        valid = sol_ir_validate_impl(&lowered, diagnostics, false);
    }
    if (valid && !lowerer.failed) valid = sol_ir_analyze_ownership(&lowered, diagnostics);
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
