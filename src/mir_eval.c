#include "sol/mir_eval.h"
#include "sol/ownership.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct MirEval MirEval;
typedef struct MirFrame MirFrame;
typedef struct MirHandler MirHandler;

struct MirHandler {
    MirHandler *parent;
    SolIrCallableId source;
    SolIrCallableId provider_callable;
    const char *effect_name;
    void *root;
    SolInterpreterValue provider;
};

struct MirFrame {
    MirFrame *parent;
    const SolMir *mir;
    SolInterpreterValue *locals;
    bool *bound;
    bool *registered;
    void **authority_roots;
    bool *authority_known;
    SolIrLocalId *binding_order;
    size_t binding_count;
    SolInterpreterValue *values;
    bool *value_bound;
    SolInterpreterValue *temporaries;
    bool *temporary_bound;
    SolInterpreterValue *snapshots;
    bool *snapshot_bound;
    SolInterpreterValue *writebacks;
    bool *writeback_bound;
    const SolIrTypeId *type_arguments;
    size_t type_argument_count;
    SolIrGenericParameterId type_parameter_offset;
    const SolIrDispatchEvidence *evidence;
    size_t evidence_count;
    MirFrame *contract;
    bool observe_cleanup;
};

struct MirEval {
    const SolMirEvaluateRequest *request;
    SolMirEvaluateResult *result;
    MirFrame *frame;
    MirHandler *handler;
    size_t depth;
    const SolMir **callee_cache;
    SolMirCalleeStatus *callee_status;
    bool *callee_queried;
};

typedef enum { MIR_FLOW_ERROR, MIR_FLOW_VALUE } MirFlowKind;
typedef struct {
    MirFlowKind kind;
    SolInterpreterValue value;
} MirFlow;

static void value_init(SolInterpreterValue *value) {
    sol_interpreter_value_init(value);
}

static void free_values(SolInterpreterValue *values, size_t count) {
    if (values == NULL) return;
    for (size_t index = 0; index < count; ++index) {
        sol_interpreter_value_free(&values[index]);
    }
    free(values);
}

void sol_mir_evaluate_result_init(SolMirEvaluateResult *result) {
    if (result != NULL) memset(result, 0, sizeof(*result));
}

void sol_mir_evaluate_result_free(SolMirEvaluateResult *result) {
    if (result == NULL) return;
    sol_interpreter_value_free(&result->value);
    memset(result, 0, sizeof(*result));
}

static SolInterpreterLimits default_limits(void) {
    return (SolInterpreterLimits){100000, 128, 100000, 1024 * 1024, 10000};
}

static SolMirTraceEvent trace_event(SolMirTraceEventKind kind) {
    SolMirTraceEvent event;
    memset(&event, 0, sizeof(event));
    event.kind = kind;
    event.callable = SOL_IR_NONE;
    event.block = SOL_MIR_NONE;
    event.instruction = SOL_MIR_NONE;
    event.instruction_kind = SOL_MIR_TRACE_INSTRUCTION_KIND_NONE;
    event.terminator_kind = SOL_MIR_TERM_INVALID;
    event.edge_target = SOL_MIR_NONE;
    event.source_expression = SOL_IR_NONE;
    event.operation = SOL_IR_NONE;
    event.local = SOL_IR_NONE;
    event.code = SOL_INTERPRETER_OK;
    return event;
}

static void emit_trace(MirEval *eval, SolMirTraceEvent event) {
    event.ordinal = eval->result->trace_count;
    event.call_depth = eval->depth;
    if (eval->frame != NULL && eval->frame->mir != NULL) {
        event.callable = eval->frame->mir->callable;
    }
    if (eval->result->trace_count >= eval->request->trace_events) {
        eval->result->trace_truncated = true;
        return;
    }
    ++eval->result->trace_count;
    if (eval->request->trace_observer != NULL) {
        eval->request->trace_observer(eval->request->trace_context, &event);
    }
}

static void set_diagnostic(MirEval *eval, SolInterpreterCode code, SolSpan span,
    const char *format, ...) {
    SolInterpreterDiagnostic *output = &eval->result->diagnostic;
    if (output->code != SOL_INTERPRETER_OK) return;
    output->code = code;
    output->span = span;
    output->file_offset = span.start;
    const SolIr *ir = eval->request->ir;
    const char *file = ir == NULL ? NULL : ir->source_path;
    if (ir != NULL) {
        for (size_t index = 0; index < ir->file_count; ++index) {
            if (span.start >= ir->files[index].aggregate_start
                && span.start <= ir->files[index].aggregate_end) {
                file = ir->files[index].path;
                output->file_offset = span.start - ir->files[index].aggregate_start;
                break;
            }
        }
    }
    if (file != NULL) (void)snprintf(output->file, sizeof(output->file), "%s", file);
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(output->message, sizeof(output->message), format, arguments);
    va_end(arguments);
    SolMirTraceEvent event = trace_event(SOL_MIR_TRACE_FAILURE);
    event.code = code;
    event.span = span;
    emit_trace(eval, event);
}

static bool consume(MirEval *eval, size_t *used, size_t limit,
    SolInterpreterCode code, SolSpan span, const char *name) {
    if (*used == SIZE_MAX || *used >= limit) {
        set_diagnostic(eval, code, span, "%s limit exceeded", name);
        return false;
    }
    ++*used;
    return true;
}

typedef struct {
    const void **owned;
    size_t owned_count;
    size_t owned_capacity;
    size_t nodes;
    size_t bytes;
    bool allocation_failed;
    bool pointer_exceeded;
} ValueInspection;

/* Unstable raw-request safety ceiling for cycle/share inspection metadata. */
#define MIR_VALUE_INSPECTION_POINTER_LIMIT ((size_t)1048576)

static bool inspection_pointer_add(ValueInspection *inspection,
    const void *pointer) {
    if (pointer == NULL) return true;
    for (size_t index = 0; index < inspection->owned_count; ++index) {
        if (inspection->owned[index] == pointer) return false;
    }
    if (inspection->owned_count >= MIR_VALUE_INSPECTION_POINTER_LIMIT) {
        inspection->pointer_exceeded = true;
        return false;
    }
    if (inspection->owned_count == inspection->owned_capacity) {
        size_t capacity = inspection->owned_capacity == 0
            ? 16 : inspection->owned_capacity * 2;
        if (capacity < inspection->owned_capacity
            || capacity > SIZE_MAX / sizeof(*inspection->owned)) return false;
        if (capacity > MIR_VALUE_INSPECTION_POINTER_LIMIT) {
            capacity = MIR_VALUE_INSPECTION_POINTER_LIMIT;
        }
        const void **owned = realloc(inspection->owned,
            capacity * sizeof(*owned));
        if (owned == NULL) {
            inspection->allocation_failed = true;
            return false;
        }
        inspection->owned = owned;
        inspection->owned_capacity = capacity;
    }
    inspection->owned[inspection->owned_count++] = pointer;
    return true;
}

static bool inspect_value_recursive(const SolInterpreterValue *value,
    const SolInterpreterValue **ancestors, ValueInspection *inspection,
    size_t depth) {
    if (value == NULL || depth >= 256 || (int)value->kind < 0
        || value->kind > SOL_INTERPRETER_VALUE_BOUND_OPERATION) return false;
    if (inspection->nodes == SIZE_MAX) return false;
    ++inspection->nodes;
    for (size_t index = 0; index < depth; ++index) {
        if (ancestors[index] == value) return false;
    }
    ancestors[depth] = value;
    switch (value->kind) {
        case SOL_INTERPRETER_VALUE_INVALID: return false;
        case SOL_INTERPRETER_VALUE_TEXT:
            if (value->as.text.bytes == NULL
                || !inspection_pointer_add(inspection,
                    value->as.text.bytes)) return false;
            if (value->as.text.length > SIZE_MAX - inspection->bytes) return false;
            inspection->bytes += value->as.text.length;
            return true;
        case SOL_INTERPRETER_VALUE_TUPLE:
            if (value->as.aggregate.definition != SOL_IR_NONE
                || value->as.aggregate.variant != SOL_IR_NONE
                || value->as.aggregate.field_count < 2
                || value->as.aggregate.field_count > 16) return false;
            /* fall through */
        case SOL_INTERPRETER_VALUE_RECORD:
        case SOL_INTERPRETER_VALUE_ENUM:
            if ((value->as.aggregate.field_count == 0)
                    != (value->as.aggregate.fields == NULL)
                || !inspection_pointer_add(inspection,
                    value->as.aggregate.fields)) return false;
            for (size_t index = 0; index < value->as.aggregate.field_count;
                ++index) {
                if (!inspect_value_recursive(
                    &value->as.aggregate.fields[index], ancestors,
                    inspection, depth + 1)) return false;
            }
            return true;
        case SOL_INTERPRETER_VALUE_OPTION:
            if (value->as.sum.is_error
                || value->as.sum.has_value != (value->as.sum.value != NULL)) {
                return false;
            }
            return value->as.sum.value == NULL
                || (inspection_pointer_add(inspection, value->as.sum.value)
                    && inspect_value_recursive(value->as.sum.value, ancestors,
                        inspection, depth + 1));
        case SOL_INTERPRETER_VALUE_RESULT:
            return value->as.sum.has_value && value->as.sum.value != NULL
                && inspection_pointer_add(inspection, value->as.sum.value)
                && inspect_value_recursive(value->as.sum.value, ancestors,
                    inspection, depth + 1);
        case SOL_INTERPRETER_VALUE_DISTINCT:
            return value->as.distinct.value != NULL
                && inspection_pointer_add(inspection, value->as.distinct.value)
                && inspect_value_recursive(value->as.distinct.value, ancestors,
                    inspection, depth + 1);
        case SOL_INTERPRETER_VALUE_CAPABILITY:
            return value->as.capability.source == NULL
                || (inspection_pointer_add(inspection,
                        value->as.capability.source)
                    && inspect_value_recursive(value->as.capability.source,
                        ancestors, inspection, depth + 1));
        case SOL_INTERPRETER_VALUE_FUNCTION:
            return value->as.callable.receiver == NULL;
        case SOL_INTERPRETER_VALUE_BOUND_OPERATION:
            return value->as.callable.receiver != NULL
                && inspection_pointer_add(inspection,
                    value->as.callable.receiver)
                && inspect_value_recursive(value->as.callable.receiver,
                    ancestors, inspection, depth + 1);
        default: return true;
    }
}

static bool reserve_value(MirEval *eval, SolSpan span, size_t nodes, size_t bytes) {
    if (eval->result->used.value_nodes > eval->request->limits.value_nodes
        || nodes > eval->request->limits.value_nodes
            - eval->result->used.value_nodes) {
        set_diagnostic(eval, SOL_INTERPRETER_VALUE_LIMIT, span,
            "value node limit exceeded");
        return false;
    }
    if (eval->result->used.text_bytes > eval->request->limits.text_bytes
        || bytes > eval->request->limits.text_bytes
            - eval->result->used.text_bytes) {
        set_diagnostic(eval, SOL_INTERPRETER_TEXT_LIMIT, span,
            "text byte limit exceeded");
        return false;
    }
    eval->result->used.value_nodes += nodes;
    eval->result->used.text_bytes += bytes;
    return true;
}

static bool inspect_value(MirEval *eval, SolSpan span,
    const SolInterpreterValue *value, SolInterpreterCode shape_code,
    bool reserve) {
    ValueInspection inspection = {0};
    const SolInterpreterValue *ancestors[256];
    bool valid = inspect_value_recursive(value, ancestors, &inspection, 0);
    free(inspection.owned);
    if (!valid) {
        if (inspection.allocation_failed) {
            set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT, span,
                "runtime value inspection allocation failed");
        } else if (inspection.pointer_exceeded) {
            set_diagnostic(eval, SOL_INTERPRETER_VALUE_LIMIT, span,
                "runtime value inspection safety limit exceeded");
        } else {
            set_diagnostic(eval, shape_code, span,
                "runtime value shape is malformed or cyclic");
        }
        return false;
    }
    return !reserve || reserve_value(eval, span,
        inspection.nodes, inspection.bytes);
}

static bool preflight_value_shape(MirEval *eval, const SolInterpreterValue *value,
    SolInterpreterCode shape_code, SolSpan span, bool *deferred_limit) {
    size_t node_remaining = eval->result->used.value_nodes
            > eval->request->limits.value_nodes ? 0
        : eval->request->limits.value_nodes - eval->result->used.value_nodes;
    size_t text_remaining = eval->result->used.text_bytes
            > eval->request->limits.text_bytes ? 0
        : eval->request->limits.text_bytes - eval->result->used.text_bytes;
    ValueInspection inspection = {0};
    const SolInterpreterValue *ancestors[256];
    bool valid = inspect_value_recursive(value, ancestors, &inspection, 0);
    free(inspection.owned);
    if (inspection.allocation_failed) {
        set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT, span,
            "runtime value inspection allocation failed");
        return false;
    }
    if (inspection.pointer_exceeded) {
        set_diagnostic(eval, SOL_INTERPRETER_VALUE_LIMIT, span,
            "runtime value inspection safety limit exceeded");
        return false;
    }
    if (!valid) {
        set_diagnostic(eval, shape_code, span,
            "runtime value shape is malformed or cyclic");
        return false;
    }
    *deferred_limit = inspection.nodes > node_remaining
        || inspection.bytes > text_remaining;
    return true;
}

static bool clone_value(MirEval *eval, SolSpan span,
    SolInterpreterValue *destination, const SolInterpreterValue *source) {
    if (!inspect_value(eval, span, source, SOL_INTERPRETER_TYPE_INVARIANT,
        true)) return false;
    if (sol_interpreter_value_clone(destination, source)) return true;
    set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT, span,
        "runtime value allocation failed");
    return false;
}

static bool clone_internal(MirEval *eval, SolSpan span,
    SolInterpreterValue *destination, const SolInterpreterValue *source) {
    if (sol_interpreter_value_clone(destination, source)) return true;
    set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT, span,
        "internal runtime value clone failed");
    return false;
}

static bool new_node(MirEval *eval, SolSpan span) {
    return reserve_value(eval, span, 1, 0);
}

static SolIrTypeId substituted_type(MirEval *eval, SolIrTypeId id) {
    const SolIr *ir = eval->request->ir;
    for (size_t depth = 0; depth < 256 && id < ir->type_count; ++depth) {
        const SolIrType *type = &ir->types[id];
        if (type->kind != SOL_IR_TYPE_PARAMETER) return id;
        bool found = false;
        for (MirFrame *frame = eval->frame; frame != NULL; frame = frame->parent) {
            if (type->definition >= frame->type_parameter_offset
                && type->definition - frame->type_parameter_offset
                    < frame->type_argument_count) {
                id = frame->type_arguments[
                    type->definition - frame->type_parameter_offset];
                found = true;
                break;
            }
        }
        if (!found) return SOL_IR_NONE;
    }
    return SOL_IR_NONE;
}

static bool capability_valid(const SolIr *ir,
    const SolInterpreterValue *value, size_t depth) {
    if (value == NULL || value->kind != SOL_INTERPRETER_VALUE_CAPABILITY
        || value->as.capability.root == NULL
        || value->as.capability.definition >= ir->definition_count
        || depth > ir->definition_count) return false;
    const SolIrDefinition *definition
        = &ir->definitions[value->as.capability.definition];
    if (definition->kind != SOL_IR_DEFINITION_CAPABILITY) return false;
    if (definition->capability_source == SOL_IR_NONE) {
        return value->as.capability.source == NULL;
    }
    if (definition->capability_source >= ir->local_count
        || value->as.capability.source == NULL) return false;
    SolIrTypeId type = ir->locals[definition->capability_source].type;
    return type < ir->type_count && ir->types[type].kind == SOL_IR_TYPE_NOMINAL
        && value->as.capability.source->kind == SOL_INTERPRETER_VALUE_CAPABILITY
        && value->as.capability.source->as.capability.definition
            == ir->types[type].definition
        && value->as.capability.source->as.capability.root
            == value->as.capability.root
        && capability_valid(ir, value->as.capability.source, depth + 1);
}

static bool value_matches_type(MirEval *eval, const SolInterpreterValue *value,
    SolIrTypeId id, const SolInterpreterValue *self, size_t depth) {
    const SolIr *ir = eval->request->ir;
    if (value == NULL || id >= ir->type_count || depth >= 256) return false;
    const SolIrType *type = &ir->types[id];
    if (type->kind == SOL_IR_TYPE_PARAMETER) {
        SolIrTypeId substituted = substituted_type(eval, id);
        return substituted != SOL_IR_NONE && substituted != id
            && value_matches_type(eval, value, substituted, self, depth + 1);
    }
    if (type->kind == SOL_IR_TYPE_SELF) {
        if (self == NULL || value->kind != self->kind) return false;
        if (value->kind == SOL_INTERPRETER_VALUE_RECORD
            || value->kind == SOL_INTERPRETER_VALUE_ENUM) {
            return value->as.aggregate.definition == self->as.aggregate.definition;
        }
        if (value->kind == SOL_INTERPRETER_VALUE_DISTINCT) {
            return value->as.distinct.definition == self->as.distinct.definition;
        }
        return value->kind != SOL_INTERPRETER_VALUE_CAPABILITY
            || value->as.capability.definition == self->as.capability.definition;
    }
    switch (type->kind) {
        case SOL_IR_TYPE_INT64: return value->kind == SOL_INTERPRETER_VALUE_INT64;
        case SOL_IR_TYPE_BOOL: return value->kind == SOL_INTERPRETER_VALUE_BOOL;
        case SOL_IR_TYPE_TEXT: return value->kind == SOL_INTERPRETER_VALUE_TEXT;
        case SOL_IR_TYPE_UNIT: return value->kind == SOL_INTERPRETER_VALUE_UNIT;
        case SOL_IR_TYPE_NEVER: return false;
        case SOL_IR_TYPE_OPTION:
            return value->kind == SOL_INTERPRETER_VALUE_OPTION
                && type->argument_count == 1
                && (!value->as.sum.has_value || (value->as.sum.value != NULL
                    && value_matches_type(eval, value->as.sum.value,
                        ir->type_ids[type->argument_offset], self, depth + 1)));
        case SOL_IR_TYPE_RESULT:
            return value->kind == SOL_INTERPRETER_VALUE_RESULT
                && value->as.sum.has_value && value->as.sum.value != NULL
                && type->argument_count == 2
                && value_matches_type(eval, value->as.sum.value,
                    ir->type_ids[type->argument_offset
                        + (value->as.sum.is_error ? 1u : 0u)], self, depth + 1);
        case SOL_IR_TYPE_TUPLE:
            if (value->kind != SOL_INTERPRETER_VALUE_TUPLE
                || value->as.aggregate.definition != SOL_IR_NONE
                || value->as.aggregate.variant != SOL_IR_NONE
                || value->as.aggregate.field_count != type->argument_count) return false;
            for (size_t index = 0; index < type->argument_count; ++index) {
                if (!value_matches_type(eval, &value->as.aggregate.fields[index],
                    ir->type_ids[type->argument_offset + index], self,
                    depth + 1)) return false;
            }
            return true;
        case SOL_IR_TYPE_FUNCTION: {
            if (value->kind != SOL_INTERPRETER_VALUE_FUNCTION
                && value->kind != SOL_INTERPRETER_VALUE_BOUND_OPERATION) return false;
            SolIrCallableId callable = value->as.callable.callable;
            if (callable >= ir->callable_count) return false;
            if (value->kind == SOL_INTERPRETER_VALUE_BOUND_OPERATION) {
                if (!capability_valid(ir, value->as.callable.receiver, 0)
                    || ir->callables[callable].kind != SOL_IR_CALLABLE_CAPABILITY
                    || ir->callables[callable].owner
                        != value->as.callable.receiver->as.capability.definition) {
                    return false;
                }
            } else if (ir->callables[callable].kind != SOL_IR_CALLABLE_FUNCTION) {
                return false;
            }
            const SolIrCallable *target = &ir->callables[callable];
            if (target->generic_parameters.count != 0
                || target->effect_parameters.count != 0) return false;
            if (type->definition != SOL_IR_NONE) return target->owner == type->definition;
            if (target->parameters.count != type->parameter_count
                || target->result != substituted_type(eval, type->result)
                || target->effects.count != type->effects.count) return false;
            for (size_t index = 0; index < type->parameter_count; ++index) {
                SolIrLocalId local = ir->roots[target->parameters.offset + index];
                if (ir->locals[local].access
                        != ir->accesses[type->parameter_access_offset + index]
                    || ir->locals[local].type != substituted_type(eval,
                        ir->type_ids[type->parameter_offset + index])) return false;
            }
            for (size_t index = 0; index < type->effects.count; ++index) {
                const SolIrEffect *left
                    = &ir->effects[target->effects.offset + index];
                const SolIrEffect *right
                    = &ir->effects[type->effects.offset + index];
                if (left->authority_kind != right->authority_kind
                    || left->authority != right->authority
                    || strcmp(left->name, right->name) != 0) return false;
            }
            return true;
        }
        case SOL_IR_TYPE_NOMINAL: {
            if (type->definition >= ir->definition_count) return false;
            const SolIrDefinition *definition = &ir->definitions[type->definition];
            MirFrame substitution;
            memset(&substitution, 0, sizeof(substitution));
            substitution.parent = eval->frame;
            substitution.type_parameter_offset = definition->generic_parameters.offset;
            substitution.type_argument_count = type->argument_count;
            substitution.type_arguments = type->argument_count == 0 ? NULL
                : ir->type_ids + type->argument_offset;
            MirFrame *saved = eval->frame;
            eval->frame = &substitution;
            bool matches = false;
            if (definition->kind == SOL_IR_DEFINITION_CAPABILITY) {
                matches = value->kind == SOL_INTERPRETER_VALUE_CAPABILITY
                    && value->as.capability.definition == type->definition
                    && capability_valid(ir, value, 0);
            } else if (definition->kind == SOL_IR_DEFINITION_RECORD) {
                matches = value->kind == SOL_INTERPRETER_VALUE_RECORD
                    && value->as.aggregate.definition == type->definition
                    && value->as.aggregate.field_count == definition->fields.count;
                for (size_t index = 0; matches
                    && index < definition->fields.count; ++index) {
                    matches = value_matches_type(eval,
                        &value->as.aggregate.fields[index],
                        ir->fields[definition->fields.offset + index].type,
                        self, depth + 1);
                }
            } else if (definition->kind == SOL_IR_DEFINITION_ENUM) {
                matches = value->kind == SOL_INTERPRETER_VALUE_ENUM
                    && value->as.aggregate.definition == type->definition
                    && value->as.aggregate.variant < ir->variant_count
                    && ir->variants[value->as.aggregate.variant].owner
                        == type->definition;
                SolIrSlice fields = matches
                    ? ir->variants[value->as.aggregate.variant].fields
                    : (SolIrSlice){0};
                matches = matches && fields.count == value->as.aggregate.field_count;
                for (size_t index = 0; matches && index < fields.count; ++index) {
                    matches = value_matches_type(eval,
                        &value->as.aggregate.fields[index],
                        ir->fields[fields.offset + index].type,
                        self, depth + 1);
                }
            } else if (definition->kind == SOL_IR_DEFINITION_DISTINCT
                || definition->kind == SOL_IR_DEFINITION_REFINED) {
                matches = value->kind == SOL_INTERPRETER_VALUE_DISTINCT
                    && value->as.distinct.definition == type->definition
                    && value->as.distinct.value != NULL
                    && value_matches_type(eval, value->as.distinct.value,
                        definition->representation, self, depth + 1);
            }
            eval->frame = saved;
            return matches;
        }
        default: return false;
    }
}

static SolInterpreterValue *place_field(const SolIr *ir,
    SolInterpreterValue *base, const SolIrProjection *projection) {
    if (base == NULL) return NULL;
    if (projection->kind == SOL_IR_PROJECTION_TUPLE_FIELD) {
        return base->kind == SOL_INTERPRETER_VALUE_TUPLE
                && projection->ordinal < base->as.aggregate.field_count
            ? &base->as.aggregate.fields[projection->ordinal] : NULL;
    }
    if (projection->kind != SOL_IR_PROJECTION_FIELD
        || base->kind != SOL_INTERPRETER_VALUE_RECORD
        || base->as.aggregate.definition >= ir->definition_count) return NULL;
    SolIrSlice fields = ir->definitions[base->as.aggregate.definition].fields;
    if (projection->field < fields.offset
        || projection->field - fields.offset >= fields.count) return NULL;
    size_t ordinal = projection->field - fields.offset;
    return ordinal < base->as.aggregate.field_count
        ? &base->as.aggregate.fields[ordinal] : NULL;
}

static SolInterpreterValue *place_slot(MirEval *eval, MirFrame *frame,
    SolMirPlace place) {
    if (place.local >= eval->request->ir->local_count || !frame->bound[place.local]) {
        return NULL;
    }
    SolInterpreterValue *slot = &frame->locals[place.local];
    if (place.source_place == SOL_IR_NONE) return slot;
    const SolIrPlace *source = &eval->request->ir->places[place.source_place];
    for (size_t index = 0; index < source->projections.count; ++index) {
        slot = place_field(eval->request->ir, slot,
            &eval->request->ir->projections[source->projections.offset + index]);
        if (slot == NULL) return NULL;
    }
    return slot;
}

static bool values_equal(const SolInterpreterValue *left,
    const SolInterpreterValue *right, bool *supported) {
    if (left->kind != right->kind) return false;
    switch (left->kind) {
        case SOL_INTERPRETER_VALUE_INT64: return left->as.integer == right->as.integer;
        case SOL_INTERPRETER_VALUE_BOOL: return left->as.boolean == right->as.boolean;
        case SOL_INTERPRETER_VALUE_TEXT:
            return left->as.text.length == right->as.text.length
                && memcmp(left->as.text.bytes, right->as.text.bytes,
                    left->as.text.length) == 0;
        case SOL_INTERPRETER_VALUE_UNIT: return true;
        case SOL_INTERPRETER_VALUE_TUPLE:
        case SOL_INTERPRETER_VALUE_RECORD:
        case SOL_INTERPRETER_VALUE_ENUM:
            if (left->as.aggregate.definition != right->as.aggregate.definition
                || left->as.aggregate.variant != right->as.aggregate.variant
                || left->as.aggregate.field_count != right->as.aggregate.field_count) {
                return false;
            }
            for (size_t index = 0; index < left->as.aggregate.field_count; ++index) {
                if (!values_equal(&left->as.aggregate.fields[index],
                    &right->as.aggregate.fields[index], supported) || !*supported) {
                    return false;
                }
            }
            return true;
        case SOL_INTERPRETER_VALUE_OPTION:
        case SOL_INTERPRETER_VALUE_RESULT:
            return left->as.sum.has_value == right->as.sum.has_value
                && left->as.sum.is_error == right->as.sum.is_error
                && (!left->as.sum.has_value || values_equal(left->as.sum.value,
                    right->as.sum.value, supported));
        case SOL_INTERPRETER_VALUE_DISTINCT:
            return left->as.distinct.definition == right->as.distinct.definition
                && values_equal(left->as.distinct.value,
                    right->as.distinct.value, supported);
        default: *supported = false; return false;
    }
}

static bool integer_binary(MirEval *eval, SolTokenKind operator_kind,
    int64_t left, int64_t right, SolSpan span, SolInterpreterValue *output) {
    int64_t value = 0;
    bool boolean = false;
    bool comparison = false;
    bool valid = true;
    switch (operator_kind) {
        case SOL_TOKEN_PLUS:
            valid = !((right > 0 && left > INT64_MAX - right)
                || (right < 0 && left < INT64_MIN - right));
            if (valid) value = left + right;
            break;
        case SOL_TOKEN_MINUS:
            valid = !((right < 0 && left > INT64_MAX + right)
                || (right > 0 && left < INT64_MIN + right));
            if (valid) value = left - right;
            break;
        case SOL_TOKEN_STAR:
            if (left == 0 || right == 0) value = 0;
            else if (left == -1) valid = right != INT64_MIN,
                value = valid ? -right : 0;
            else if (right == -1) valid = left != INT64_MIN,
                value = valid ? -left : 0;
            else valid = !((left > 0 && right > 0 && left > INT64_MAX / right)
                || (left > 0 && right < 0 && right < INT64_MIN / left)
                || (left < 0 && right > 0 && left < INT64_MIN / right)
                || (left < 0 && right < 0 && left < INT64_MAX / right));
            if (valid && left != 0 && right != 0
                && left != -1 && right != -1) value = left * right;
            break;
        case SOL_TOKEN_SLASH:
        case SOL_TOKEN_PERCENT:
            if (right == 0) {
                set_diagnostic(eval, SOL_INTERPRETER_DIVISION_BY_ZERO, span,
                    "integer division by zero");
                return false;
            }
            valid = left != INT64_MIN || right != -1;
            if (valid) value = operator_kind == SOL_TOKEN_SLASH
                ? left / right : left % right;
            break;
        case SOL_TOKEN_LESS: comparison = true; boolean = left < right; break;
        case SOL_TOKEN_LESS_EQUAL: comparison = true; boolean = left <= right; break;
        case SOL_TOKEN_GREATER: comparison = true; boolean = left > right; break;
        case SOL_TOKEN_GREATER_EQUAL: comparison = true; boolean = left >= right; break;
        default: valid = false; break;
    }
    if (!valid) {
        set_diagnostic(eval, SOL_INTERPRETER_INTEGER_OVERFLOW, span,
            "checked Int64 arithmetic overflow");
        return false;
    }
    return comparison ? sol_interpreter_value_bool(output, boolean)
        : sol_interpreter_value_int64(output, value);
}

static SolTokenKind compound_operator(SolTokenKind kind) {
    switch (kind) {
        case SOL_TOKEN_PLUS_EQUAL: return SOL_TOKEN_PLUS;
        case SOL_TOKEN_MINUS_EQUAL: return SOL_TOKEN_MINUS;
        case SOL_TOKEN_STAR_EQUAL: return SOL_TOKEN_STAR;
        case SOL_TOKEN_SLASH_EQUAL: return SOL_TOKEN_SLASH;
        case SOL_TOKEN_PERCENT_EQUAL: return SOL_TOKEN_PERCENT;
        default: return kind;
    }
}

static void frame_release(MirEval *eval, MirFrame *frame, SolSpan span);

static bool frame_allocate(MirEval *eval, MirFrame *frame,
    const SolMir *mir, SolSpan span) {
    const SolIr *ir = eval->request->ir;
    memset(frame, 0, sizeof(*frame));
    frame->mir = mir;
    frame->locals = ir->local_count == 0 ? NULL
        : calloc(ir->local_count, sizeof(*frame->locals));
    frame->bound = ir->local_count == 0 ? NULL
        : calloc(ir->local_count, sizeof(*frame->bound));
    frame->registered = ir->local_count == 0 ? NULL
        : calloc(ir->local_count, sizeof(*frame->registered));
    frame->authority_roots = ir->local_count == 0 ? NULL
        : calloc(ir->local_count, sizeof(*frame->authority_roots));
    frame->authority_known = ir->local_count == 0 ? NULL
        : calloc(ir->local_count, sizeof(*frame->authority_known));
    frame->binding_order = ir->local_count == 0 ? NULL
        : malloc(ir->local_count * sizeof(*frame->binding_order));
    frame->writebacks = ir->local_count == 0 ? NULL
        : calloc(ir->local_count, sizeof(*frame->writebacks));
    frame->writeback_bound = ir->local_count == 0 ? NULL
        : calloc(ir->local_count, sizeof(*frame->writeback_bound));
    frame->values = mir->value_count == 0 ? NULL
        : calloc(mir->value_count, sizeof(*frame->values));
    frame->value_bound = mir->value_count == 0 ? NULL
        : calloc(mir->value_count, sizeof(*frame->value_bound));
    frame->temporaries = mir->temporary_count == 0 ? NULL
        : calloc(mir->temporary_count, sizeof(*frame->temporaries));
    frame->temporary_bound = mir->temporary_count == 0 ? NULL
        : calloc(mir->temporary_count, sizeof(*frame->temporary_bound));
    frame->snapshots = ir->snapshot_count == 0 ? NULL
        : calloc(ir->snapshot_count, sizeof(*frame->snapshots));
    frame->snapshot_bound = ir->snapshot_count == 0 ? NULL
        : calloc(ir->snapshot_count, sizeof(*frame->snapshot_bound));
    bool valid = (ir->local_count == 0 || (frame->locals != NULL
            && frame->bound != NULL && frame->registered != NULL
            && frame->authority_roots != NULL && frame->authority_known != NULL
            && frame->binding_order != NULL && frame->writebacks != NULL
            && frame->writeback_bound != NULL))
        && (mir->value_count == 0
            || (frame->values != NULL && frame->value_bound != NULL))
        && (mir->temporary_count == 0 || (frame->temporaries != NULL
            && frame->temporary_bound != NULL))
        && (ir->snapshot_count == 0 || (frame->snapshots != NULL
            && frame->snapshot_bound != NULL));
    if (valid) return true;
    set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT, span,
        "MIR frame allocation failed");
    frame_release(eval, frame, span);
    return false;
}

static void observe_drop(MirEval *eval, SolIrLocalId local, SolSpan span) {
    size_t ordinal = eval->result->cleanup_actions++;
    if (eval->request->cleanup_observer != NULL) {
        eval->request->cleanup_observer(eval->request->cleanup_context,
            local, ordinal);
    }
    SolMirTraceEvent event = trace_event(SOL_MIR_TRACE_DROP);
    event.local = local;
    event.span = span;
    emit_trace(eval, event);
}

static void drop_local(MirEval *eval, MirFrame *frame,
    SolIrLocalId local, SolSpan span) {
    const SolIr *ir = eval->request->ir;
    if (local >= ir->local_count || !frame->bound[local]) return;
    if (ir->locals[local].access == SOL_ACCESS_EXCLUSIVE) {
        sol_interpreter_value_free(&frame->writebacks[local]);
        frame->writebacks[local] = frame->locals[local];
        value_init(&frame->locals[local]);
        frame->writeback_bound[local] = true;
    } else {
        if (ir->locals[local].access == SOL_ACCESS_OWNED
            && frame->observe_cleanup) {
            observe_drop(eval, local, span);
        }
        sol_interpreter_value_free(&frame->locals[local]);
    }
    frame->bound[local] = false;
    frame->authority_known[local] = false;
    frame->authority_roots[local] = NULL;
}

static void frame_release(MirEval *eval, MirFrame *frame, SolSpan span) {
    while (frame->binding_count != 0) {
        SolIrLocalId local = frame->binding_order[--frame->binding_count];
        drop_local(eval, frame, local, span);
        if (local < eval->request->ir->local_count) frame->registered[local] = false;
    }
    free_values(frame->values, frame->mir == NULL ? 0 : frame->mir->value_count);
    free(frame->value_bound);
    free_values(frame->temporaries,
        frame->mir == NULL ? 0 : frame->mir->temporary_count);
    free(frame->temporary_bound);
    free_values(frame->snapshots, eval->request->ir->snapshot_count);
    free(frame->snapshot_bound);
    free_values(frame->locals, eval->request->ir->local_count);
    free(frame->bound);
    free(frame->registered);
    free(frame->authority_roots);
    free(frame->authority_known);
    free(frame->binding_order);
    free_values(frame->writebacks, eval->request->ir->local_count);
    free(frame->writeback_bound);
    memset(frame, 0, sizeof(*frame));
}

static bool contract_frame_init(MirEval *eval, MirFrame *contract,
    const MirFrame *source, SolSpan span) {
    if (!frame_allocate(eval, contract, source->mir, span)) return false;
    contract->parent = source->parent;
    contract->type_arguments = source->type_arguments;
    contract->type_argument_count = source->type_argument_count;
    contract->type_parameter_offset = source->type_parameter_offset;
    contract->evidence = source->evidence;
    contract->evidence_count = source->evidence_count;
    contract->observe_cleanup = false;
    for (size_t local = 0; local < eval->request->ir->local_count; ++local) {
        if (!source->bound[local]) continue;
        if (!clone_value(eval, span, &contract->locals[local],
            &source->locals[local])) {
            frame_release(eval, contract, span);
            return false;
        }
        contract->bound[local] = true;
        contract->registered[local] = true;
        contract->binding_order[contract->binding_count++] = local;
        contract->authority_roots[local] = source->authority_roots[local];
        contract->authority_known[local] = source->authority_known[local];
    }
    return true;
}

static bool register_local(MirEval *eval, MirFrame *frame,
    SolIrLocalId local, SolSpan span) {
    if (local >= eval->request->ir->local_count || frame->registered[local]) {
        set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT, span,
            "invalid or duplicate MIR local activation");
        return false;
    }
    frame->registered[local] = true;
    frame->binding_order[frame->binding_count++] = local;
    return true;
}

static bool set_value(MirEval *eval, MirFrame *frame, SolMirValueId id,
    const SolInterpreterValue *value, SolSpan span) {
    if (id >= frame->mir->value_count) {
        set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT, span,
            "MIR SSA result is invalid");
        return false;
    }
    if (frame->value_bound[id]) sol_interpreter_value_free(&frame->values[id]);
    if (!clone_internal(eval, span, &frame->values[id], value)) return false;
    frame->value_bound[id] = true;
    return true;
}

static SolInterpreterValue *get_value(MirEval *eval, MirFrame *frame,
    SolMirValueId id, SolSpan span) {
    if (id >= frame->mir->value_count || !frame->value_bound[id]) {
        set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT, span,
            "MIR SSA value v%zu is unavailable", id);
        return NULL;
    }
    return &frame->values[id];
}

static bool store_place(MirEval *eval, MirFrame *frame, SolMirPlace place,
    const SolInterpreterValue *value, SolSpan span) {
    if (place.local >= eval->request->ir->local_count
        || !frame->registered[place.local]) {
        set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT, span,
            "MIR assignment local is unavailable");
        return false;
    }
    SolInterpreterValue copy;
    value_init(&copy);
    if (!clone_internal(eval, span, &copy, value)) return false;
    if (place.source_place == SOL_IR_NONE
        || eval->request->ir->places[place.source_place].projections.count == 0) {
        if (frame->bound[place.local]) {
            sol_interpreter_value_free(&frame->locals[place.local]);
        }
        frame->locals[place.local] = copy;
        frame->bound[place.local] = true;
        frame->authority_known[place.local]
            = copy.kind == SOL_INTERPRETER_VALUE_CAPABILITY;
        frame->authority_roots[place.local] = frame->authority_known[place.local]
            ? copy.as.capability.root : NULL;
        return true;
    }
    SolInterpreterValue *slot = place_slot(eval, frame, place);
    if (slot == NULL) {
        sol_interpreter_value_free(&copy);
        set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT, span,
            "MIR projected assignment has a missing ancestor");
        return false;
    }
    sol_interpreter_value_free(slot);
    *slot = copy;
    return true;
}

static void drop_place(MirEval *eval, MirFrame *frame,
    SolMirPlace place, SolSpan span) {
    SolInterpreterValue *slot = place_slot(eval, frame, place);
    if (slot == NULL || slot->kind == SOL_INTERPRETER_VALUE_INVALID) return;
    if (eval->request->ir->locals[place.local].access == SOL_ACCESS_OWNED
        && frame->observe_cleanup) {
        observe_drop(eval, place.local, span);
    }
    sol_interpreter_value_free(slot);
    if (place.source_place == SOL_IR_NONE
        || eval->request->ir->places[place.source_place].projections.count == 0) {
        frame->bound[place.local] = false;
    }
}

static const SolInterpreterValue *pattern_value(const SolIr *ir,
    SolIrPatternId root, SolIrPatternId target,
    const SolInterpreterValue *value, size_t depth) {
    if (root >= ir->pattern_count || depth > 64) return NULL;
    if (root == target) return value;
    const SolIrPattern *pattern = &ir->patterns[root];
    for (size_t index = 0; index < pattern->children.count; ++index) {
        const SolIrPatternChild *child
            = &ir->pattern_children[pattern->children.offset + index];
        size_t ordinal = child->ordinal;
        if (child->field != SOL_IR_NONE) {
            if (pattern->definition >= ir->definition_count) return NULL;
            ordinal = child->field - ir->definitions[pattern->definition].fields.offset;
        }
        if ((value->kind != SOL_INTERPRETER_VALUE_TUPLE
                && value->kind != SOL_INTERPRETER_VALUE_RECORD
                && value->kind != SOL_INTERPRETER_VALUE_ENUM)
            || ordinal >= value->as.aggregate.field_count) return NULL;
        const SolInterpreterValue *found = pattern_value(ir, child->pattern,
            target, &value->as.aggregate.fields[ordinal], depth + 1);
        if (found != NULL) return found;
    }
    return NULL;
}

static int match_pattern(MirEval *eval, SolIrPatternId id,
    const SolInterpreterValue *value, size_t depth) {
    const SolIr *ir = eval->request->ir;
    if (id >= ir->pattern_count || depth > 64
        || !consume(eval, &eval->result->used.steps, eval->request->limits.steps,
            SOL_INTERPRETER_STEP_LIMIT,
            id < ir->pattern_count ? ir->patterns[id].span : (SolSpan){0},
            "MIR step")) return -1;
    const SolIrPattern *pattern = &ir->patterns[id];
    if (pattern->kind == SOL_IR_PATTERN_BOOL) {
        return value->kind == SOL_INTERPRETER_VALUE_BOOL
            && value->as.boolean == pattern->boolean;
    }
    if (pattern->kind == SOL_IR_PATTERN_BINDING
        || pattern->kind == SOL_IR_PATTERN_WILDCARD) return 1;
    if (pattern->kind == SOL_IR_PATTERN_VARIANT
        && (value->kind != SOL_INTERPRETER_VALUE_ENUM
            || value->as.aggregate.variant != pattern->variant)) return 0;
    if (pattern->kind == SOL_IR_PATTERN_RECORD
        && (value->kind != SOL_INTERPRETER_VALUE_RECORD
            || value->as.aggregate.definition != pattern->definition)) return 0;
    if (pattern->kind == SOL_IR_PATTERN_TUPLE
        && value->kind != SOL_INTERPRETER_VALUE_TUPLE) return 0;
    for (size_t index = 0; index < pattern->children.count; ++index) {
        const SolIrPatternChild *child
            = &ir->pattern_children[pattern->children.offset + index];
        size_t ordinal = child->ordinal;
        if (child->field != SOL_IR_NONE) {
            ordinal = child->field - ir->definitions[pattern->definition].fields.offset;
        }
        if (ordinal >= value->as.aggregate.field_count) return 0;
        int matched = match_pattern(eval, child->pattern,
            &value->as.aggregate.fields[ordinal], depth + 1);
        if (matched != 1) return matched;
    }
    return 1;
}

static const SolMir *provided_mir(MirEval *eval, SolIrCallableId callable,
    SolSpan span, bool preflight) {
    const SolIr *ir = eval->request->ir;
    if (callable >= ir->callable_count) {
        set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, span,
            "MIR names an out-of-range callable");
        return NULL;
    }
    const SolIrCallable *metadata = &ir->callables[callable];
    if (metadata->kind == SOL_IR_CALLABLE_CAPABILITY
        && metadata->body != SOL_IR_NONE) {
        set_diagnostic(eval, SOL_INTERPRETER_INVALID_REQUEST, span,
            "reachable bodyful capability member cannot be represented by SolMir");
        return NULL;
    }
    if (!preflight && !eval->callee_queried[callable]) {
        set_diagnostic(eval, SOL_INTERPRETER_INVALID_REQUEST, span,
            "runtime callable is outside the preflighted MIR closure");
        return NULL;
    }
    if (!eval->callee_queried[callable]) {
        if (eval->request->callee_provider == NULL) {
            set_diagnostic(eval, SOL_INTERPRETER_INVALID_REQUEST, span,
                "reachable callable has no MIR callee provider");
            return NULL;
        }
        const SolMir *mir = NULL;
        SolMirCalleeStatus status = eval->request->callee_provider(
            eval->request->callee_context, callable, &mir);
        eval->callee_queried[callable] = true;
        eval->callee_status[callable] = status;
        eval->callee_cache[callable] = mir;
        if (status == SOL_MIR_CALLEE_FOUND && mir != NULL) {
            for (size_t previous = 0; previous < ir->callable_count;
                ++previous) {
                if (previous != callable && eval->callee_queried[previous]
                    && eval->callee_status[previous] == SOL_MIR_CALLEE_FOUND
                    && eval->callee_cache[previous] == mir) {
                    eval->callee_status[callable]
                        = SOL_MIR_CALLEE_UNAVAILABLE;
                    eval->callee_cache[callable] = NULL;
                    set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, span,
                        "MIR callee provider aliased results for different callables");
                    return NULL;
                }
            }
        }
        if (status == SOL_MIR_CALLEE_FOUND
            && (mir == NULL || mir->callable != callable
                || !sol_mir_validate(ir, mir, NULL))) {
            eval->callee_status[callable] = SOL_MIR_CALLEE_UNAVAILABLE;
            eval->callee_cache[callable] = NULL;
            set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, span,
                "MIR callee provider returned invalid or mismatched MIR");
            return NULL;
        }
    }
    SolMirCalleeStatus status = eval->callee_status[callable];
    const SolMir *mir = eval->callee_cache[callable];
    if (status == SOL_MIR_CALLEE_FOUND) {
        return mir;
    }
    if (status != SOL_MIR_CALLEE_NO_BODY
        && status != SOL_MIR_CALLEE_UNAVAILABLE) {
        set_diagnostic(eval, SOL_INTERPRETER_INVALID_REQUEST, span,
            "MIR callee provider returned an unknown status");
        return NULL;
    }
    if (metadata->body != SOL_IR_NONE) {
        set_diagnostic(eval, SOL_INTERPRETER_INVALID_REQUEST, span,
            preflight ? "reachable bodyful callable is unavailable from MIR provider"
                : "bodyful callable became unavailable from MIR provider");
    } else {
        set_diagnostic(eval, SOL_INTERPRETER_INVALID_REQUEST, span,
            "bodyless callable was requested as executable MIR");
    }
    return NULL;
}

static const SolIrDispatchEvidence *resolve_evidence(MirEval *eval,
    const SolIrDispatchEvidence *candidate);
static bool preflight_callable(MirEval *eval, const SolMir *mir, bool *visited);
static bool preflight_target(MirEval *eval, SolIrCallableId callable,
    SolSpan span, bool *visited, const SolIrDispatchEvidence *evidence,
    size_t evidence_count);

static bool type_contains_callable_value(MirEval *eval, SolIrTypeId id,
    size_t depth) {
    const SolIr *ir = eval->request->ir;
    if (depth >= 256) return true;
    id = substituted_type(eval, id);
    if (id == SOL_IR_NONE || id >= ir->type_count) return true;
    const SolIrType *type = &ir->types[id];
    if (type->kind == SOL_IR_TYPE_FUNCTION
        || type->kind == SOL_IR_TYPE_PARAMETER) return true;
    if (type->kind == SOL_IR_TYPE_OPTION || type->kind == SOL_IR_TYPE_RESULT
        || type->kind == SOL_IR_TYPE_TUPLE) {
        for (size_t index = 0; index < type->argument_count; ++index) {
            if (type_contains_callable_value(eval,
                    ir->type_ids[type->argument_offset + index], depth + 1)) {
                return true;
            }
        }
        return false;
    }
    if (type->kind != SOL_IR_TYPE_NOMINAL
        || type->definition >= ir->definition_count) return false;
    const SolIrDefinition *definition = &ir->definitions[type->definition];
    MirFrame substitution;
    memset(&substitution, 0, sizeof(substitution));
    substitution.parent = eval->frame;
    substitution.type_parameter_offset = definition->generic_parameters.offset;
    substitution.type_arguments = type->argument_count == 0 ? NULL
        : ir->type_ids + type->argument_offset;
    substitution.type_argument_count = type->argument_count;
    MirFrame *saved = eval->frame;
    eval->frame = &substitution;
    bool contains = false;
    if (definition->kind == SOL_IR_DEFINITION_RECORD) {
        for (size_t index = 0; !contains && index < definition->fields.count;
            ++index) {
            contains = type_contains_callable_value(eval,
                ir->fields[definition->fields.offset + index].type, depth + 1);
        }
    } else if (definition->kind == SOL_IR_DEFINITION_ENUM) {
        for (size_t variant = 0; !contains && variant < definition->variants.count;
            ++variant) {
            SolIrSlice fields
                = ir->variants[definition->variants.offset + variant].fields;
            for (size_t field = 0; !contains && field < fields.count; ++field) {
                contains = type_contains_callable_value(eval,
                    ir->fields[fields.offset + field].type, depth + 1);
            }
        }
    } else if (definition->kind == SOL_IR_DEFINITION_DISTINCT
        || definition->kind == SOL_IR_DEFINITION_REFINED) {
        contains = type_contains_callable_value(eval,
            definition->representation, depth + 1);
    }
    eval->frame = saved;
    return contains;
}

static bool preflight_operation_result(MirEval *eval,
    SolIrCallableId callable, SolIrTypeId result_type, SolSpan span) {
    const SolIr *ir = eval->request->ir;
    if (callable >= ir->callable_count
        || ir->callables[callable].kind != SOL_IR_CALLABLE_CAPABILITY) {
        return true;
    }
    if (!type_contains_callable_value(eval, result_type, 0)) return true;
    set_diagnostic(eval, SOL_INTERPRETER_INVALID_REQUEST, span,
        "reachable capability operation may produce an unpreflighted callable value");
    return false;
}

static bool preflight_static_callback_expression(MirEval *eval,
    SolIrExpressionId id, SolSpan span, bool *visited_callables,
    size_t depth) {
    const SolIr *ir = eval->request->ir;
    if (id == SOL_IR_NONE || id >= ir->expression_count || depth >= 256) {
        return id == SOL_IR_NONE;
    }
    const SolIrExpression *expression = &ir->expressions[id];
#define STATIC_SCAN(child) do { \
        if (!preflight_static_callback_expression(eval, (child), span, \
            visited_callables, depth + 1)) return false; \
    } while (0)
    switch (expression->kind) {
        case SOL_IR_EXPR_DEFINITION:
            if (expression->as.definition < ir->definition_count) {
                SolIrCallableId callable
                    = ir->definitions[expression->as.definition].callable;
                if (callable != SOL_IR_NONE && !preflight_target(eval, callable,
                        span, visited_callables, NULL, 0)) return false;
            }
            break;
        case SOL_IR_EXPR_BOUND_OPERATION: {
            SolIrCallableId callable = expression->as.operation.callable;
            SolIrTypeId result_type = callable < ir->callable_count
                ? ir->callables[callable].result : SOL_IR_NONE;
            if (!preflight_operation_result(eval, callable, result_type, span)
                || !preflight_target(eval, callable, span, visited_callables,
                    NULL, 0)) return false;
            STATIC_SCAN(expression->as.operation.receiver);
            break;
        }
        case SOL_IR_EXPR_UNARY: STATIC_SCAN(expression->as.unary.operand); break;
        case SOL_IR_EXPR_BINARY:
            STATIC_SCAN(expression->as.binary.left);
            STATIC_SCAN(expression->as.binary.right);
            break;
        case SOL_IR_EXPR_CALL:
            if (expression->as.call.callee != SOL_IR_NONE) {
                STATIC_SCAN(expression->as.call.callee);
            }
            if (expression->as.call.receiver != SOL_IR_NONE) {
                STATIC_SCAN(expression->as.call.receiver);
            }
            for (size_t index = 0; index < expression->as.call.operands.count;
                ++index) {
                STATIC_SCAN(ir->operands[
                    expression->as.call.operands.offset + index].value);
            }
            break;
        case SOL_IR_EXPR_RECORD:
            for (size_t index = 0; index < expression->as.record.fields.count;
                ++index) {
                STATIC_SCAN(ir->operands[
                    expression->as.record.fields.offset + index].value);
            }
            break;
        case SOL_IR_EXPR_TUPLE:
            for (size_t index = 0; index < expression->as.tuple.operands.count;
                ++index) {
                STATIC_SCAN(ir->operands[
                    expression->as.tuple.operands.offset + index].value);
            }
            break;
        case SOL_IR_EXPR_IF:
            STATIC_SCAN(expression->as.if_expr.condition);
            STATIC_SCAN(expression->as.if_expr.then_branch);
            STATIC_SCAN(expression->as.if_expr.else_branch);
            break;
        case SOL_IR_EXPR_MATCH:
            STATIC_SCAN(expression->as.match_expr.scrutinee);
            for (size_t index = 0; index < expression->as.match_expr.arms.count;
                ++index) {
                const SolIrArm *arm = &ir->arms[ir->arm_ids[
                    expression->as.match_expr.arms.offset + index]];
                if (arm->guard != SOL_IR_NONE) STATIC_SCAN(arm->guard);
                STATIC_SCAN(arm->body);
            }
            break;
        case SOL_IR_EXPR_BLOCK:
            for (size_t index = 0; index < expression->as.block.statements.count;
                ++index) {
                STATIC_SCAN(ir->statements[ir->statement_ids[
                    expression->as.block.statements.offset + index]].expression);
            }
            break;
        case SOL_IR_EXPR_PROPAGATE:
            STATIC_SCAN(expression->as.propagate.operand);
            break;
        case SOL_IR_EXPR_HANDLE:
            STATIC_SCAN(expression->as.handler.provider);
            STATIC_SCAN(expression->as.handler.body);
            break;
        default: break;
    }
#undef STATIC_SCAN
    return true;
}

static bool preflight_target(MirEval *eval, SolIrCallableId callable,
    SolSpan span, bool *visited, const SolIrDispatchEvidence *evidence,
    size_t evidence_count) {
    const SolIr *ir = eval->request->ir;
    if (callable >= ir->callable_count) {
        set_diagnostic(eval, SOL_INTERPRETER_INVALID_REQUEST, span,
            "callback or MIR target callable is out of range");
        return false;
    }
    const SolIrCallable *target = &ir->callables[callable];
    for (size_t index = 0; index < evidence_count; ++index) {
        const SolIrDispatchEvidence *selected
            = resolve_evidence(eval, &evidence[index]);
        if (selected == NULL || !preflight_target(eval, selected->method,
                span, visited, NULL, 0)) {
            set_diagnostic(eval, SOL_INTERPRETER_INVALID_REQUEST, span,
                "invocation evidence has no preflightable selected method");
            return false;
        }
    }
    if (target->kind == SOL_IR_CALLABLE_CAPABILITY) {
        if (target->body != SOL_IR_NONE) {
            set_diagnostic(eval, SOL_INTERPRETER_INVALID_REQUEST, span,
                "reachable bodyful capability member cannot be represented by SolMir");
            return false;
        }
        visited[callable] = true;
        return true;
    }
    if (target->body == SOL_IR_NONE) {
        visited[callable] = true;
        return true;
    }
    if (visited[callable]) return true;
    const SolMir *callee = provided_mir(eval, callable, span, true);
    if (callee == NULL) return false;
    MirFrame scope;
    memset(&scope, 0, sizeof(scope));
    scope.parent = eval->frame;
    scope.type_parameter_offset = target->generic_parameters.offset;
    scope.type_argument_count = target->generic_parameters.count;
    scope.evidence = evidence;
    scope.evidence_count = evidence_count;
    MirFrame *saved = eval->frame;
    eval->frame = &scope;
    bool ready = preflight_callable(eval, callee, visited);
    eval->frame = saved;
    return ready;
}

static bool preflight_callable(MirEval *eval, const SolMir *mir, bool *visited) {
    const SolIr *ir = eval->request->ir;
    if (mir->callable >= ir->callable_count) return false;
    if (visited[mir->callable]) return true;
    visited[mir->callable] = true;
    for (size_t value = 0; value < mir->value_count; ++value) {
        if (!preflight_static_callback_expression(eval,
                mir->values[value].source_expression,
                mir->values[value].span, visited, 0)) return false;
    }
    for (size_t temporary = 0; temporary < mir->temporary_count; ++temporary) {
        if (!preflight_static_callback_expression(eval,
                mir->temporaries[temporary].source_expression,
                mir->temporaries[temporary].span, visited, 0)) return false;
    }
    for (size_t instruction = 0; instruction < mir->instruction_count; ++instruction) {
        const SolMirInstruction *item = &mir->instructions[instruction];
        if (item->kind != SOL_MIR_INST_HANDLER_ENTER
            || item->source_expression >= ir->expression_count) continue;
        const SolIrExpression *handler = &ir->expressions[item->source_expression];
        if (handler->kind == SOL_IR_EXPR_HANDLE
            && handler->as.handler.provider_callable < ir->callable_count
            && ir->callables[handler->as.handler.provider_callable].body != SOL_IR_NONE) {
            set_diagnostic(eval, SOL_INTERPRETER_INVALID_REQUEST, item->span,
                "reachable handler needs a bodyful capability member without SolMir");
            return false;
        }
    }
    for (size_t block = 0; block < mir->block_count; ++block) {
        const SolMirTerminator *term = &mir->blocks[block].terminator;
        if (term->kind == SOL_MIR_TERM_CHECK_REFINED) {
            const SolIrExpression *construction = &ir->expressions[
                term->as.check_refined.source_expression];
            for (size_t index = 0; construction->kind == SOL_IR_EXPR_CALL
                && index < construction->as.call.evidence.count; ++index) {
                const SolIrDispatchEvidence *candidate = &ir->evidence[
                    construction->as.call.evidence.offset + index];
                const SolIrDispatchEvidence *selected
                    = resolve_evidence(eval, candidate);
                if (selected == NULL || !preflight_target(eval,
                        selected->method, term->span, visited, NULL, 0)) {
                    set_diagnostic(eval, SOL_INTERPRETER_INVALID_REQUEST,
                        term->span,
                        "refined construction evidence is unavailable in the MIR closure");
                    return false;
                }
            }
        }
        if (term->kind != SOL_MIR_TERM_INVOKE) continue;
        if (term->as.invoke.kind == SOL_IR_CALL_CALLBACK) {
            if (term->as.invoke.result != SOL_MIR_NONE
                && term->as.invoke.result < mir->value_count
                && type_contains_callable_value(eval,
                    mir->values[term->as.invoke.result].type, 0)) {
                set_diagnostic(eval, SOL_INTERPRETER_INVALID_REQUEST,
                    term->span,
                    "callback invocation may dispatch an operation producing an unpreflighted callable value");
                return false;
            }
            SolMirTemporaryId temporary = term->as.invoke.callee;
            if (temporary < mir->temporary_count
                && !preflight_static_callback_expression(eval,
                    mir->temporaries[temporary].source_expression,
                    term->span, visited, 0)) return false;
            continue;
        }
        SolIrCallableId target = term->as.invoke.callable;
        if (term->as.invoke.kind == SOL_IR_CALL_CAPABILITY
            && target < ir->callable_count) {
            SolIrTypeId result_type = ir->callables[target].result;
            if (term->as.invoke.result != SOL_MIR_NONE
                && term->as.invoke.result < mir->value_count) {
                result_type = mir->values[term->as.invoke.result].type;
            }
            if (!preflight_operation_result(eval, target, result_type,
                    term->span)) return false;
        }
        if (target < ir->callable_count
            && ir->callables[target].kind == SOL_IR_CALLABLE_TRAIT_REQUIREMENT) {
            const SolIrDispatchEvidence *selected = NULL;
            for (size_t index = 0; index < term->as.invoke.evidence.count;
                ++index) {
                const SolIrDispatchEvidence *candidate = &ir->evidence[
                    term->as.invoke.evidence.offset + index];
                if (candidate->requirement != target) continue;
                selected = resolve_evidence(eval, candidate);
                break;
            }
            if (selected == NULL) {
                set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, term->span,
                    "trait requirement invoke has no executable evidence closure");
                return false;
            }
            if (!preflight_target(eval, selected->method, term->span, visited,
                    NULL, 0)) return false;
        } else {
            const SolIrDispatchEvidence *evidence
                = term->as.invoke.evidence.count == 0 ? NULL
                : ir->evidence + term->as.invoke.evidence.offset;
            if (!preflight_target(eval, target, term->span, visited, evidence,
                    term->as.invoke.evidence.count)) return false;
        }
    }
    return true;
}

static bool preflight_value_callbacks(MirEval *eval,
    const SolInterpreterValue *value, SolSpan span, bool *visited,
    size_t depth) {
    if (value == NULL || depth >= 256) return false;
    switch (value->kind) {
        case SOL_INTERPRETER_VALUE_FUNCTION:
            return preflight_target(eval, value->as.callable.callable, span,
                visited, NULL, 0);
        case SOL_INTERPRETER_VALUE_BOUND_OPERATION: {
            SolIrCallableId callable = value->as.callable.callable;
            if (callable >= eval->request->ir->callable_count
                || !preflight_operation_result(eval, callable,
                    eval->request->ir->callables[callable].result, span)) {
                return false;
            }
            return preflight_target(eval, callable, span, visited, NULL, 0);
        }
        case SOL_INTERPRETER_VALUE_TUPLE:
        case SOL_INTERPRETER_VALUE_RECORD:
        case SOL_INTERPRETER_VALUE_ENUM:
            for (size_t index = 0; index < value->as.aggregate.field_count;
                ++index) {
                if (!preflight_value_callbacks(eval,
                        &value->as.aggregate.fields[index], span, visited,
                        depth + 1)) return false;
            }
            return true;
        case SOL_INTERPRETER_VALUE_OPTION:
        case SOL_INTERPRETER_VALUE_RESULT:
            return value->as.sum.value == NULL
                || preflight_value_callbacks(eval, value->as.sum.value, span,
                    visited, depth + 1);
        case SOL_INTERPRETER_VALUE_DISTINCT:
            return value->as.distinct.value != NULL
                && preflight_value_callbacks(eval, value->as.distinct.value,
                    span, visited, depth + 1);
        case SOL_INTERPRETER_VALUE_CAPABILITY:
            return value->as.capability.source == NULL
                || preflight_value_callbacks(eval, value->as.capability.source,
                    span, visited, depth + 1);
        default: return true;
    }
}

static bool evaluate_obligation(MirEval *eval, SolObligationId obligation,
    const SolInterpreterValue *result, const SolInterpreterValue *self,
    bool *satisfied);
static bool evaluate_refinement_obligation(MirEval *eval,
    const SolIrExpression *construction, SolIrDefinitionId definition,
    SolObligationId obligation, const SolInterpreterValue *self,
    bool *satisfied);
static bool execute_callable(MirEval *eval, const SolMir *mir,
    const SolInterpreterValue *receiver, const SolInterpreterValue *arguments,
    size_t argument_count, const SolIrTypeId *type_arguments,
    size_t type_argument_count, const SolIrDispatchEvidence *evidence,
    size_t evidence_count, SolInterpreterValue *output,
    SolInterpreterValue *receiver_writeback,
    SolInterpreterValue *argument_writebacks, SolSpan span);

static const SolIrDispatchEvidence *resolve_parameter_evidence(MirFrame *frame,
    SolIrGenericParameterId parameter, SolIrDefinitionId trait,
    SolIrCallableId requirement) {
    if (frame == NULL) return NULL;
    for (size_t index = 0; index < frame->evidence_count; ++index) {
        const SolIrDispatchEvidence *entry = &frame->evidence[index];
        if (entry->binding != parameter || entry->trait != trait
            || entry->requirement != requirement) continue;
        return entry->forwarded ? resolve_parameter_evidence(frame->parent,
            entry->parameter, trait, requirement) : entry;
    }
    return NULL;
}

static const SolIrDispatchEvidence *resolve_evidence(MirEval *eval,
    const SolIrDispatchEvidence *candidate) {
    return candidate->forwarded ? resolve_parameter_evidence(eval->frame,
        candidate->parameter, candidate->trait, candidate->requirement)
        : candidate;
}

static bool validate_authority(MirEval *eval, const SolIrCallable *callable,
    const SolInterpreterValue *receiver, const SolInterpreterValue *result,
    SolSpan span) {
    if (callable->result_authority_kind == SOL_IR_AUTHORITY_NONE) return true;
    if (result->kind != SOL_INTERPRETER_VALUE_CAPABILITY) {
        set_diagnostic(eval, SOL_INTERPRETER_TYPE_INVARIANT, span,
            "authority-returning callable returned a non-capability");
        return false;
    }
    bool known = receiver != NULL
        && receiver->kind == SOL_INTERPRETER_VALUE_CAPABILITY;
    void *root = known ? receiver->as.capability.root : NULL;
    if (callable->result_authority_kind == SOL_IR_AUTHORITY_LOCAL) {
        SolIrLocalId local = callable->result_authority;
        known = eval->frame != NULL && local < eval->request->ir->local_count
            && eval->frame->authority_known[local];
        root = known ? eval->frame->authority_roots[local] : NULL;
    }
    if (!known || root != result->as.capability.root) {
        set_diagnostic(eval, SOL_INTERPRETER_TYPE_INVARIANT, span,
            "returned capability has the wrong authority root");
        return false;
    }
    return true;
}

static bool preflight_predicate(MirEval *eval, SolIrExpressionId id,
    bool *visited_expressions, bool *visited_callables) {
    const SolIr *ir = eval->request->ir;
    if (id >= ir->expression_count) return false;
    if (visited_expressions[id]) return true;
    visited_expressions[id] = true;
    const SolIrExpression *expression = &ir->expressions[id];
#define SCAN(child) do { \
        if (!preflight_predicate(eval, (child), visited_expressions, \
            visited_callables)) return false; \
    } while (0)
    switch (expression->kind) {
        case SOL_IR_EXPR_DEFINITION:
            if (expression->as.definition >= ir->definition_count
                || !preflight_target(eval,
                    ir->definitions[expression->as.definition].callable,
                    expression->span, visited_callables, NULL, 0)) return false;
            break;
        case SOL_IR_EXPR_UNARY: SCAN(expression->as.unary.operand); break;
        case SOL_IR_EXPR_BINARY:
            SCAN(expression->as.binary.left);
            SCAN(expression->as.binary.right);
            break;
        case SOL_IR_EXPR_CALL: {
            if (expression->as.call.kind == SOL_IR_CALL_CAPABILITY) {
                set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, expression->span,
                    "capability operation is impure in a predicate");
                return false;
            }
            if (expression->as.call.kind == SOL_IR_CALL_FUNCTION) {
                const SolIrDispatchEvidence *evidence
                    = expression->as.call.evidence.count == 0 ? NULL
                    : ir->evidence + expression->as.call.evidence.offset;
                if (!preflight_target(eval, expression->as.call.callable,
                    expression->span, visited_callables, evidence,
                    expression->as.call.evidence.count)) return false;
            } else if (expression->as.call.kind == SOL_IR_CALL_METHOD) {
                const SolIrDispatchEvidence *selected = NULL;
                for (size_t index = 0; index < expression->as.call.evidence.count;
                    ++index) {
                    const SolIrDispatchEvidence *candidate = &ir->evidence[
                        expression->as.call.evidence.offset + index];
                    if (candidate->requirement
                            != expression->as.call.callable) continue;
                    selected = resolve_evidence(eval, candidate);
                    break;
                }
                if (selected == NULL) {
                    for (size_t index = 0; index < ir->evidence_count; ++index) {
                        const SolIrDispatchEvidence *candidate
                            = &ir->evidence[index];
                        if (!candidate->forwarded
                            && candidate->requirement
                                == expression->as.call.callable
                            && candidate->method < ir->callable_count
                            && eval->callee_queried[candidate->method]
                            && eval->callee_status[candidate->method]
                                == SOL_MIR_CALLEE_FOUND) {
                            selected = candidate;
                            break;
                        }
                    }
                }
                if (selected == NULL || !preflight_target(eval,
                        selected->method, expression->span, visited_callables,
                        NULL, 0)) {
                    set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR,
                        expression->span,
                        "trait requirement predicate call has no selected evidence");
                    return false;
                }
                SCAN(expression->as.call.receiver);
            } else if (expression->as.call.kind == SOL_IR_CALL_CALLBACK) {
                SCAN(expression->as.call.callee);
            }
            for (size_t index = 0; index < expression->as.call.operands.count;
                ++index) {
                SCAN(ir->operands[expression->as.call.operands.offset + index].value);
            }
            break;
        }
        case SOL_IR_EXPR_RECORD:
            for (size_t index = 0; index < expression->as.record.fields.count;
                ++index) SCAN(ir->operands[
                    expression->as.record.fields.offset + index].value);
            break;
        case SOL_IR_EXPR_TUPLE:
            for (size_t index = 0; index < expression->as.tuple.operands.count;
                ++index) SCAN(ir->operands[
                    expression->as.tuple.operands.offset + index].value);
            break;
        case SOL_IR_EXPR_IF:
            SCAN(expression->as.if_expr.condition);
            SCAN(expression->as.if_expr.then_branch);
            SCAN(expression->as.if_expr.else_branch);
            break;
        case SOL_IR_EXPR_MATCH:
            SCAN(expression->as.match_expr.scrutinee);
            for (size_t index = 0; index < expression->as.match_expr.arms.count;
                ++index) {
                const SolIrArm *arm = &ir->arms[ir->arm_ids[
                    expression->as.match_expr.arms.offset + index]];
                if (arm->guard != SOL_IR_NONE) SCAN(arm->guard);
                SCAN(arm->body);
            }
            break;
        case SOL_IR_EXPR_BLOCK:
            for (size_t index = 0; index < expression->as.block.statements.count;
                ++index) {
                const SolIrStatement *statement = &ir->statements[ir->statement_ids[
                    expression->as.block.statements.offset + index]];
                if (statement->kind != SOL_IR_STATEMENT_LET
                    && statement->kind != SOL_IR_STATEMENT_EXPRESSION
                    && statement->kind != SOL_IR_STATEMENT_RETURN) {
                    set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR,
                        statement->span,
                        "impure statement form reached predicate evaluator");
                    return false;
                }
                SCAN(statement->expression);
            }
            break;
        case SOL_IR_EXPR_PROPAGATE: SCAN(expression->as.propagate.operand); break;
        case SOL_IR_EXPR_HANDLE:
        case SOL_IR_EXPR_BOUND_OPERATION:
            set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, expression->span,
                "effectful form reached pure predicate evaluator");
            return false;
        case SOL_IR_EXPR_COMPILE_TIME_HEAD:
            set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, expression->span,
                "compile-time expression reached predicate evaluator");
            return false;
        default: break;
    }
#undef SCAN
    return true;
}

static const SolMir *visited_mir(MirEval *eval, SolIrCallableId callable) {
    return callable < eval->request->ir->callable_count
            && eval->callee_queried[callable]
            && eval->callee_status[callable] == SOL_MIR_CALLEE_FOUND
        ? eval->callee_cache[callable] : NULL;
}

static bool obligation_reachable(MirEval *eval,
    const SolIrObligation *obligation, const bool *visited) {
    const SolIr *ir = eval->request->ir;
    if (obligation->owner_kind == SOL_CONTRACT_OWNER_CAPABILITY_MEMBER) {
        return obligation->owner < ir->callable_count
            && visited[obligation->owner];
    }
    if (obligation->owner_kind == SOL_CONTRACT_OWNER_ITEM) {
        for (size_t callable = 0; callable < ir->callable_count; ++callable) {
            if (visited[callable]
                && ir->callables[callable].owner == obligation->owner) return true;
        }
        return false;
    }
    if (obligation->owner_kind != SOL_CONTRACT_OWNER_TYPE) return false;
    for (size_t callable = 0; callable < ir->callable_count; ++callable) {
        if (!visited[callable]) continue;
        const SolMir *mir = visited_mir(eval, callable);
        if (mir == NULL) continue;
        for (size_t block = 0; block < mir->block_count; ++block) {
            const SolMirTerminator *term = &mir->blocks[block].terminator;
            if (term->kind == SOL_MIR_TERM_CHECK_REFINED
                && term->as.check_refined.definition == obligation->owner) {
                return true;
            }
        }
    }
    return false;
}

bool sol_mir_evaluate(const SolMirEvaluateRequest *request,
    SolMirEvaluateResult *result) {
    if (result == NULL) return false;
    sol_mir_evaluate_result_init(result);
    if (request == NULL || request->ir == NULL || request->entry == NULL) {
        result->diagnostic.code = SOL_INTERPRETER_INVALID_REQUEST;
        (void)snprintf(result->diagnostic.message,
            sizeof(result->diagnostic.message),
            "MIR evaluation request, owning IR, and entry MIR are required");
        return false;
    }
    SolMirEvaluateRequest normalized = *request;
    SolInterpreterLimits zero = {0};
    if (memcmp(&normalized.limits, &zero, sizeof(zero)) == 0) {
        normalized.limits = default_limits();
    }
    MirEval eval = {.request = &normalized, .result = result};
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    bool valid_ir = sol_ir_validate(request->ir, &diagnostics);
    sol_diagnostics_free(&diagnostics);
    if (!valid_ir) {
        set_diagnostic(&eval, SOL_INTERPRETER_INVALID_IR, (SolSpan){0},
            "MIR evaluator input IR failed validation");
        return false;
    }
    if (!sol_mir_validate(request->ir, request->entry, NULL)) {
        set_diagnostic(&eval, SOL_INTERPRETER_INVALID_IR, (SolSpan){0},
            "MIR evaluator entry failed validation");
        return false;
    }
    if (request->contracts != SOL_INTERPRETER_CONTRACTS_IGNORE
        && request->contracts != SOL_INTERPRETER_CONTRACTS_CHECK) {
        set_diagnostic(&eval, SOL_INTERPRETER_UNSUPPORTED_CONTRACT_POLICY,
            (SolSpan){0}, "unknown runtime contract policy");
        return false;
    }
    SolIrCallableId callable = request->entry->callable;
    if (callable >= request->ir->callable_count
        || (request->argument_count != 0 && request->arguments == NULL)
        || (request->type_argument_count != 0 && request->type_arguments == NULL)
        || (request->evidence.count != 0 && request->evidence.items == NULL)) {
        set_diagnostic(&eval, SOL_INTERPRETER_INVALID_REQUEST, (SolSpan){0},
            "MIR evaluator argument or callable domain is malformed");
        return false;
    }
    const SolIrCallable *entry = &request->ir->callables[callable];
    if (entry->kind != SOL_IR_CALLABLE_FUNCTION
        && entry->kind != SOL_IR_CALLABLE_TEST) {
        set_diagnostic(&eval, SOL_INTERPRETER_INVALID_REQUEST, entry->span,
            "MIR evaluator entry must be a free function or test");
        return false;
    }
    if (entry->parameters.count != request->argument_count
        || entry->generic_parameters.count != request->type_argument_count) {
        set_diagnostic(&eval, SOL_INTERPRETER_INVALID_REQUEST, entry->span,
            "MIR evaluator entry argument count does not match callable metadata");
        return false;
    }
    for (size_t index = 0; index < entry->parameters.count; ++index) {
        SolIrLocalId local = request->ir->roots[entry->parameters.offset + index];
        if (request->ir->locals[local].access == SOL_ACCESS_EXCLUSIVE) {
            set_diagnostic(&eval, SOL_INTERPRETER_INVALID_REQUEST, entry->span,
                "top-level exclusive parameters require a mutable argument ABI");
            return false;
        }
    }
    bool *visited_callables = request->ir->callable_count == 0 ? NULL
        : calloc(request->ir->callable_count, sizeof(*visited_callables));
    bool *visited_expressions = request->ir->expression_count == 0 ? NULL
        : calloc(request->ir->expression_count, sizeof(*visited_expressions));
    eval.callee_cache = request->ir->callable_count == 0 ? NULL
        : calloc(request->ir->callable_count, sizeof(*eval.callee_cache));
    eval.callee_status = request->ir->callable_count == 0 ? NULL
        : calloc(request->ir->callable_count, sizeof(*eval.callee_status));
    eval.callee_queried = request->ir->callable_count == 0 ? NULL
        : calloc(request->ir->callable_count, sizeof(*eval.callee_queried));
    if ((request->ir->callable_count != 0 && visited_callables == NULL)
        || (request->ir->expression_count != 0 && visited_expressions == NULL)
        || (request->ir->callable_count != 0 && (eval.callee_cache == NULL
            || eval.callee_status == NULL || eval.callee_queried == NULL))) {
        free(visited_callables);
        free(visited_expressions);
        free(eval.callee_cache);
        free(eval.callee_status);
        free(eval.callee_queried);
        set_diagnostic(&eval, SOL_INTERPRETER_INTERNAL_INVARIANT, entry->span,
            "MIR evaluator preflight allocation failed");
        return false;
    }
    eval.callee_cache[callable] = request->entry;
    eval.callee_status[callable] = SOL_MIR_CALLEE_FOUND;
    eval.callee_queried[callable] = true;
    MirFrame preflight_scope;
    memset(&preflight_scope, 0, sizeof(preflight_scope));
    preflight_scope.type_parameter_offset = entry->generic_parameters.offset;
    preflight_scope.type_argument_count = entry->generic_parameters.count;
    preflight_scope.evidence = request->evidence.items;
    preflight_scope.evidence_count = request->evidence.count;
    eval.frame = &preflight_scope;
    bool preflight = preflight_callable(&eval, request->entry,
        visited_callables);
    for (size_t index = 0; preflight && index < request->argument_count; ++index) {
        const SolInterpreterValue *argument = &request->arguments[index];
        bool deferred = false;
        preflight = preflight_value_shape(&eval, argument,
                SOL_INTERPRETER_INVALID_REQUEST, entry->span, &deferred)
            && preflight_value_callbacks(&eval, argument, entry->span,
                visited_callables, 0);
        if (!preflight && result->diagnostic.code == SOL_INTERPRETER_OK) {
            set_diagnostic(&eval, SOL_INTERPRETER_INVALID_REQUEST, entry->span,
                "MIR evaluator argument value is malformed");
        }
    }
    bool changed = true;
    while (preflight && changed) {
        changed = false;
        size_t before = 0;
        for (size_t callable_id = 0; callable_id < request->ir->callable_count;
            ++callable_id) before += visited_callables[callable_id];
        for (size_t index = 0; preflight
            && index < request->ir->obligation_count; ++index) {
            const SolIrObligation *obligation = &request->ir->obligations[index];
            bool relevant = obligation_reachable(&eval, obligation,
                visited_callables)
                && (obligation->owner_kind == SOL_CONTRACT_OWNER_TYPE
                    || request->contracts == SOL_INTERPRETER_CONTRACTS_CHECK);
            if (relevant) preflight = preflight_predicate(&eval,
                obligation->predicate, visited_expressions, visited_callables);
        }
        size_t after = 0;
        for (size_t callable_id = 0; callable_id < request->ir->callable_count;
            ++callable_id) after += visited_callables[callable_id];
        changed = after != before;
    }
    free(visited_callables);
    free(visited_expressions);
    eval.frame = NULL;
    if (!preflight) {
        free(eval.callee_cache);
        free(eval.callee_status);
        free(eval.callee_queried);
        return false;
    }
    bool ok = execute_callable(&eval, request->entry, NULL,
        request->arguments, request->argument_count, request->type_arguments,
        request->type_argument_count, request->evidence.items,
        request->evidence.count, &result->value, NULL, NULL, entry->span);
    if (!ok && result->diagnostic.code == SOL_INTERPRETER_OK) {
        set_diagnostic(&eval, SOL_INTERPRETER_INTERNAL_INVARIANT, entry->span,
            "MIR execution failed without a specific diagnostic");
    }
    free(eval.callee_cache);
    free(eval.callee_status);
    free(eval.callee_queried);
    return ok;
}

static bool construct_value(MirEval *eval, MirFrame *frame,
    const SolMirInstruction *instruction, SolInterpreterValue *output) {
    const SolIr *ir = eval->request->ir;
    SolMirSlice slice = instruction->as.construct.operands;
    SolInterpreterValue *operands = slice.count == 0 ? NULL
        : calloc(slice.count, sizeof(*operands));
    if (slice.count != 0 && operands == NULL) {
        set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT,
            instruction->span, "MIR construction allocation failed");
        return false;
    }
    for (size_t index = 0; index < slice.count; ++index) {
        const SolMirConstructOperand *operand
            = &frame->mir->construct_operands[slice.offset + index];
        if (operand->temporary >= frame->mir->temporary_count
            || !frame->temporary_bound[operand->temporary]) {
            free_values(operands, slice.count);
            set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT,
                instruction->span, "MIR construction temporary is unavailable");
            return false;
        }
        operands[index] = frame->temporaries[operand->temporary];
        value_init(&frame->temporaries[operand->temporary]);
        frame->temporary_bound[operand->temporary] = false;
    }
    if (!new_node(eval, instruction->span)) {
        free_values(operands, slice.count);
        return false;
    }
    SolMirConstructKind kind = instruction->as.construct.kind;
    if (kind == SOL_MIR_CONSTRUCT_OPTION_NONE
        || kind == SOL_MIR_CONSTRUCT_OPTION_SOME
        || kind == SOL_MIR_CONSTRUCT_RESULT_OK
        || kind == SOL_MIR_CONSTRUCT_RESULT_ERR) {
        output->kind = kind <= SOL_MIR_CONSTRUCT_OPTION_SOME
            ? SOL_INTERPRETER_VALUE_OPTION : SOL_INTERPRETER_VALUE_RESULT;
        output->as.sum.has_value = kind != SOL_MIR_CONSTRUCT_OPTION_NONE;
        output->as.sum.is_error = kind == SOL_MIR_CONSTRUCT_RESULT_ERR;
        if (output->as.sum.has_value) {
            output->as.sum.value = malloc(sizeof(*output->as.sum.value));
            if (output->as.sum.value == NULL) goto allocation_failure;
            *output->as.sum.value = operands[0];
            value_init(&operands[0]);
        }
        free_values(operands, slice.count);
        return true;
    }
    if (kind == SOL_MIR_CONSTRUCT_DISTINCT) {
        output->kind = SOL_INTERPRETER_VALUE_DISTINCT;
        output->as.distinct.definition = instruction->as.construct.definition;
        output->as.distinct.value = malloc(sizeof(*output->as.distinct.value));
        if (output->as.distinct.value == NULL) goto allocation_failure;
        *output->as.distinct.value = operands[0];
        value_init(&operands[0]);
        free_values(operands, slice.count);
        return true;
    }
    if (kind == SOL_MIR_CONSTRUCT_CAPABILITY) {
        if (slice.count != 1
            || operands[0].kind != SOL_INTERPRETER_VALUE_CAPABILITY) {
            set_diagnostic(eval, SOL_INTERPRETER_TYPE_INVARIANT,
                instruction->span, "derived capability source is invalid");
            free_values(operands, slice.count);
            return false;
        }
        output->kind = SOL_INTERPRETER_VALUE_CAPABILITY;
        output->as.capability.definition = instruction->as.construct.definition;
        output->as.capability.root = operands[0].as.capability.root;
        output->as.capability.source = malloc(sizeof(*output->as.capability.source));
        if (output->as.capability.source == NULL) goto allocation_failure;
        *output->as.capability.source = operands[0];
        value_init(&operands[0]);
        free_values(operands, slice.count);
        return true;
    }
    size_t count = slice.count;
    SolInterpreterValue *fields = count == 0 ? NULL
        : calloc(count, sizeof(*fields));
    if (count != 0 && fields == NULL) goto allocation_failure;
    for (size_t index = 0; index < count; ++index) {
        const SolMirConstructOperand *operand
            = &frame->mir->construct_operands[slice.offset + index];
        size_t ordinal = operand->formal;
        if (kind == SOL_MIR_CONSTRUCT_RECORD) {
            ordinal -= ir->definitions[instruction->as.construct.definition]
                .fields.offset;
        } else if (kind == SOL_MIR_CONSTRUCT_ENUM) {
            ordinal -= ir->variants[instruction->as.construct.variant].fields.offset;
        }
        if (ordinal >= count) {
            free_values(fields, count);
            free_values(operands, count);
            set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT,
                instruction->span, "MIR construction formal is out of range");
            return false;
        }
        fields[ordinal] = operands[index];
        value_init(&operands[index]);
    }
    free_values(operands, count);
    output->kind = kind == SOL_MIR_CONSTRUCT_TUPLE
        ? SOL_INTERPRETER_VALUE_TUPLE
        : kind == SOL_MIR_CONSTRUCT_ENUM
        ? SOL_INTERPRETER_VALUE_ENUM : SOL_INTERPRETER_VALUE_RECORD;
    output->as.aggregate.definition = kind == SOL_MIR_CONSTRUCT_TUPLE
        ? SOL_IR_NONE : instruction->as.construct.definition;
    output->as.aggregate.variant = kind == SOL_MIR_CONSTRUCT_ENUM
        ? instruction->as.construct.variant : SOL_IR_NONE;
    output->as.aggregate.fields = fields;
    output->as.aggregate.field_count = count;
    return true;

allocation_failure:
    free_values(operands, slice.count);
    sol_interpreter_value_free(output);
    set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT,
        instruction->span, "MIR value allocation failed");
    return false;
}

static bool binary_value(MirEval *eval, SolTokenKind kind,
    const SolInterpreterValue *left, const SolInterpreterValue *right,
    SolSpan span, SolInterpreterValue *output) {
    if (!new_node(eval, span)) return false;
    if (kind == SOL_TOKEN_EQUAL_EQUAL || kind == SOL_TOKEN_BANG_EQUAL) {
        bool supported = true;
        bool equal = values_equal(left, right, &supported);
        if (!supported) {
            set_diagnostic(eval, SOL_INTERPRETER_TYPE_INVARIANT, span,
                "runtime-only values do not support structural equality");
            return false;
        }
        return sol_interpreter_value_bool(output,
            kind == SOL_TOKEN_EQUAL_EQUAL ? equal : !equal);
    }
    if (left->kind == SOL_INTERPRETER_VALUE_BOOL
        && right->kind == SOL_INTERPRETER_VALUE_BOOL
        && (kind == SOL_TOKEN_AMP_AMP || kind == SOL_TOKEN_PIPE_PIPE)) {
        return sol_interpreter_value_bool(output, kind == SOL_TOKEN_AMP_AMP
            ? left->as.boolean && right->as.boolean
            : left->as.boolean || right->as.boolean);
    }
    if (left->kind == SOL_INTERPRETER_VALUE_INT64
        && right->kind == SOL_INTERPRETER_VALUE_INT64) {
        return integer_binary(eval, kind, left->as.integer,
            right->as.integer, span, output);
    }
    set_diagnostic(eval, SOL_INTERPRETER_TYPE_INVARIANT, span,
        "binary operand runtime types are invalid");
    return false;
}

static bool execute_instruction(MirEval *eval, MirFrame *frame,
    SolMirInstructionId id) {
    const SolIr *ir = eval->request->ir;
    const SolMirInstruction *instruction = &frame->mir->instructions[id];
    ++eval->result->executed.instructions;
    if (!consume(eval, &eval->result->used.steps, eval->request->limits.steps,
        SOL_INTERPRETER_STEP_LIMIT, instruction->span, "MIR step")) return false;
    SolMirTraceEvent event = trace_event(SOL_MIR_TRACE_INSTRUCTION);
    event.block = instruction->block;
    event.instruction = id;
    event.instruction_kind = instruction->kind;
    event.source_expression = instruction->source_expression;
    event.span = instruction->span;
    emit_trace(eval, event);
    SolInterpreterValue output;
    value_init(&output);
    bool has_output = instruction->result != SOL_MIR_NONE;
    bool ok = true;
    switch (instruction->kind) {
        case SOL_MIR_INST_CONST_INT64:
            ok = new_node(eval, instruction->span)
                && sol_interpreter_value_int64(&output, instruction->as.integer);
            break;
        case SOL_MIR_INST_CONST_BOOL:
            ok = new_node(eval, instruction->span)
                && sol_interpreter_value_bool(&output, instruction->as.boolean);
            break;
        case SOL_MIR_INST_CONST_TEXT: {
            const char *text = ir->expressions[instruction->source_expression].as.string;
            size_t length = strlen(text);
            ok = reserve_value(eval, instruction->span, 1, length)
                && sol_interpreter_value_text(&output, text, length);
            break;
        }
        case SOL_MIR_INST_CONST_UNIT:
            ok = new_node(eval, instruction->span)
                && sol_interpreter_value_unit(&output);
            break;
        case SOL_MIR_INST_PARAMETER_LIVE:
            ok = instruction->as.local < ir->local_count
                && (frame->registered[instruction->as.local]
                    || register_local(eval, frame, instruction->as.local,
                        instruction->span));
            break;
        case SOL_MIR_INST_STORAGE_LIVE:
            ok = register_local(eval, frame, instruction->as.local,
                instruction->span);
            break;
        case SOL_MIR_INST_DROP_IF_INITIALIZED:
            drop_local(eval, frame, instruction->as.local, instruction->span);
            break;
        case SOL_MIR_INST_STORAGE_DEAD:
            if (instruction->as.local < ir->local_count) {
                frame->registered[instruction->as.local] = false;
                if (frame->binding_count != 0
                    && frame->binding_order[frame->binding_count - 1]
                        == instruction->as.local) --frame->binding_count;
            }
            break;
        case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED:
            drop_place(eval, frame, instruction->as.place, instruction->span);
            break;
        case SOL_MIR_INST_LOAD_COPY:
        case SOL_MIR_INST_LOAD_MOVE:
        case SOL_MIR_INST_LOAD_UPDATE: {
            SolMirPlace place = instruction->kind == SOL_MIR_INST_LOAD_UPDATE
                ? instruction->as.update_load.place : instruction->as.place;
            SolInterpreterValue *slot = place_slot(eval, frame, place);
            if (slot == NULL || slot->kind == SOL_INTERPRETER_VALUE_INVALID) {
                set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT,
                    instruction->span, "MIR load place is unavailable");
                ok = false;
            } else if (instruction->kind == SOL_MIR_INST_LOAD_MOVE) {
                output = *slot;
                value_init(slot);
                if (place.source_place == SOL_IR_NONE
                    || ir->places[place.source_place].projections.count == 0) {
                    frame->bound[place.local] = false;
                }
            } else if (instruction->kind == SOL_MIR_INST_LOAD_UPDATE) {
                ok = clone_internal(eval, instruction->span, &output, slot);
            } else {
                ok = clone_value(eval, instruction->span, &output, slot);
            }
            break;
        }
        case SOL_MIR_INST_STORE: {
            SolInterpreterValue *value = get_value(eval, frame,
                instruction->as.store.value, instruction->span);
            ok = value != NULL && store_place(eval, frame,
                instruction->as.store.place, value, instruction->span);
            break;
        }
        case SOL_MIR_INST_UNARY: {
            SolInterpreterValue *operand = get_value(eval, frame,
                instruction->as.unary.operand, instruction->span);
            if (operand == NULL || !new_node(eval, instruction->span)) ok = false;
            else if (instruction->as.unary.operator_kind == SOL_TOKEN_BANG
                && operand->kind == SOL_INTERPRETER_VALUE_BOOL) {
                sol_interpreter_value_bool(&output, !operand->as.boolean);
            } else if (instruction->as.unary.operator_kind == SOL_TOKEN_MINUS
                && operand->kind == SOL_INTERPRETER_VALUE_INT64
                && operand->as.integer != INT64_MIN) {
                sol_interpreter_value_int64(&output, -operand->as.integer);
            } else {
                set_diagnostic(eval,
                    operand != NULL && operand->kind == SOL_INTERPRETER_VALUE_INT64
                        ? SOL_INTERPRETER_INTEGER_OVERFLOW
                        : SOL_INTERPRETER_TYPE_INVARIANT,
                    instruction->span, "invalid unary operation");
                ok = false;
            }
            break;
        }
        case SOL_MIR_INST_BINARY: {
            SolInterpreterValue *left = get_value(eval, frame,
                instruction->as.binary.left, instruction->span);
            SolInterpreterValue *right = get_value(eval, frame,
                instruction->as.binary.right, instruction->span);
            ok = left != NULL && right != NULL && binary_value(eval,
                instruction->as.binary.operator_kind, left, right,
                instruction->span, &output);
            break;
        }
        case SOL_MIR_INST_COMPOUND_UPDATE: {
            SolMirTemporaryId previous = instruction->as.compound_update.previous;
            SolInterpreterValue *right = get_value(eval, frame,
                instruction->as.compound_update.right, instruction->span);
            if (previous >= frame->mir->temporary_count
                || !frame->temporary_bound[previous] || right == NULL
                || frame->temporaries[previous].kind != SOL_INTERPRETER_VALUE_INT64
                || right->kind != SOL_INTERPRETER_VALUE_INT64) {
                set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT,
                    instruction->span, "MIR compound operands are unavailable");
                ok = false;
                break;
            }
            ok = integer_binary(eval, compound_operator(
                instruction->as.compound_update.operator_kind),
                frame->temporaries[previous].as.integer, right->as.integer,
                instruction->span, &output);
            sol_interpreter_value_free(&frame->temporaries[previous]);
            frame->temporary_bound[previous] = false;
            break;
        }
        case SOL_MIR_INST_REGION_ENTER:
        case SOL_MIR_INST_REGION_EXIT:
            break;
        case SOL_MIR_INST_TEMPORARY_INIT: {
            SolMirTemporaryId temporary = instruction->as.temporary_init.temporary;
            SolInterpreterValue *value = get_value(eval, frame,
                instruction->as.temporary_init.value, instruction->span);
            if (temporary >= frame->mir->temporary_count || value == NULL) ok = false;
            else {
                if (frame->temporary_bound[temporary]) {
                    sol_interpreter_value_free(&frame->temporaries[temporary]);
                }
                ok = clone_internal(eval, instruction->span,
                    &frame->temporaries[temporary], value);
                frame->temporary_bound[temporary] = ok;
            }
            break;
        }
        case SOL_MIR_INST_TEMPORARY_DROP: {
            SolMirTemporaryId temporary = instruction->as.temporary_drop.temporary;
            ++eval->result->executed.temporary_drops;
            SolMirTraceEvent drop = trace_event(SOL_MIR_TRACE_DROP);
            drop.instruction = id;
            drop.instruction_kind = instruction->kind;
            drop.span = instruction->span;
            emit_trace(eval, drop);
            if (temporary < frame->mir->temporary_count
                && frame->temporary_bound[temporary]) {
                sol_interpreter_value_free(&frame->temporaries[temporary]);
                frame->temporary_bound[temporary] = false;
            }
            break;
        }
        case SOL_MIR_INST_EXPRESSION_RESULT: {
            SolInterpreterValue *value = get_value(eval, frame,
                instruction->as.operand, instruction->span);
            ok = value != NULL && clone_internal(eval, instruction->span,
                &output, value);
            break;
        }
        case SOL_MIR_INST_PATTERN_TEST: {
            SolMirTemporaryId temporary = instruction->as.pattern.scrutinee;
            if (temporary >= frame->mir->temporary_count
                || !frame->temporary_bound[temporary]) ok = false;
            else {
                int matched = match_pattern(eval, instruction->as.pattern.pattern,
                    &frame->temporaries[temporary], 0);
                ok = matched >= 0
                    && sol_interpreter_value_bool(&output, matched == 1);
            }
            break;
        }
        case SOL_MIR_INST_PATTERN_VALUE: {
            SolMirTemporaryId temporary = instruction->as.pattern.scrutinee;
            const SolInterpreterValue *value = temporary < frame->mir->temporary_count
                    && frame->temporary_bound[temporary]
                ? pattern_value(ir,
                    ir->arms[instruction->as.pattern.arm].pattern,
                    instruction->as.pattern.pattern,
                    &frame->temporaries[temporary], 0) : NULL;
            ok = value != NULL && clone_value(eval, instruction->span,
                &output, value);
            break;
        }
        case SOL_MIR_INST_MATCH_ARM:
            break;
        case SOL_MIR_INST_HANDLER_ENTER: {
            const SolIrExpression *source
                = &ir->expressions[instruction->source_expression];
            SolIrLocalId authority_local = source->as.handler.root;
            const SolIrExpression *provider_expression
                = &ir->expressions[source->as.handler.provider];
            const SolIrPlace *provider_place
                = &ir->places[provider_expression->as.place];
            if (authority_local >= ir->local_count
                || provider_place->local >= ir->local_count
                || !frame->bound[authority_local]
                || !frame->bound[provider_place->local]
                || !capability_valid(ir, &frame->locals[authority_local], 0)
                || !capability_valid(ir, &frame->locals[provider_place->local], 0)) {
                set_diagnostic(eval, SOL_INTERPRETER_TYPE_INVARIANT,
                    instruction->span, "MIR handler capabilities are invalid");
                ok = false;
                break;
            }
            if (!inspect_value(eval, instruction->span,
                    &frame->locals[authority_local],
                    SOL_INTERPRETER_TYPE_INVARIANT, true)
                || !inspect_value(eval, instruction->span,
                    &frame->locals[provider_place->local],
                    SOL_INTERPRETER_TYPE_INVARIANT, true)) {
                ok = false;
                break;
            }
            MirHandler *handler = calloc(1, sizeof(*handler));
            if (handler == NULL || !clone_internal(eval, instruction->span,
                &handler->provider, &frame->locals[provider_place->local])) {
                free(handler);
                ok = false;
                break;
            }
            handler->parent = eval->handler;
            handler->source = source->as.handler.source;
            handler->provider_callable = source->as.handler.provider_callable;
            handler->effect_name = source->as.handler.effect_name;
            handler->root = frame->locals[authority_local].as.capability.root;
            eval->handler = handler;
            break;
        }
        case SOL_MIR_INST_HANDLER_EXIT:
            if (eval->handler == NULL) ok = false;
            else {
                MirHandler *handler = eval->handler;
                eval->handler = handler->parent;
                sol_interpreter_value_free(&handler->provider);
                free(handler);
            }
            break;
        case SOL_MIR_INST_CONSTRUCT:
            ok = construct_value(eval, frame, instruction, &output);
            break;
        case SOL_MIR_INST_CAPTURE_SNAPSHOT: {
            SolIrSnapshotId snapshot = instruction->as.snapshot;
            if (eval->request->contracts == SOL_INTERPRETER_CONTRACTS_IGNORE) break;
            MirFrame *contract = frame->contract;
            if (contract == NULL) {
                set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT,
                    instruction->span, "MIR contract snapshot frame is missing");
                ok = false;
                break;
            }
            const SolIrExpression *operand
                = &ir->expressions[ir->snapshots[snapshot].operand];
            const SolIrPlace *place = &ir->places[operand->as.place];
            SolMirPlace mir_place = {place->local, operand->as.place};
            SolInterpreterValue *slot = place_slot(eval, contract, mir_place);
            if (snapshot >= ir->snapshot_count || slot == NULL) ok = false;
            else {
                if (contract->snapshot_bound[snapshot]) {
                    sol_interpreter_value_free(&contract->snapshots[snapshot]);
                }
                ok = clone_value(eval, instruction->span,
                    &contract->snapshots[snapshot], slot);
                contract->snapshot_bound[snapshot] = ok;
            }
            break;
        }
    }
    if (ok && has_output) ok = set_value(eval, frame,
        instruction->result, &output, instruction->span);
    sol_interpreter_value_free(&output);
    if (!ok && eval->result->diagnostic.code == SOL_INTERPRETER_OK) {
        set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT,
            instruction->span, "MIR instruction execution failed");
    }
    return ok;
}

static MirFlow pred_error(void) {
    MirFlow flow;
    flow.kind = MIR_FLOW_ERROR;
    value_init(&flow.value);
    return flow;
}

static MirFlow pred_value(void) {
    MirFlow flow;
    flow.kind = MIR_FLOW_VALUE;
    value_init(&flow.value);
    return flow;
}

static MirFlow evaluate_predicate(MirEval *eval, SolIrExpressionId id,
    const SolInterpreterValue *result, const SolInterpreterValue *self);

static MirFlow pred_binary(MirEval *eval, const SolIrExpression *expression,
    const SolInterpreterValue *result, const SolInterpreterValue *self) {
    MirFlow left = evaluate_predicate(eval, expression->as.binary.left,
        result, self);
    if (left.kind == MIR_FLOW_ERROR) return left;
    SolTokenKind kind = expression->as.binary.operator_kind;
    if ((kind == SOL_TOKEN_AMP_AMP || kind == SOL_TOKEN_PIPE_PIPE)
        && left.value.kind == SOL_INTERPRETER_VALUE_BOOL
        && ((kind == SOL_TOKEN_AMP_AMP && !left.value.as.boolean)
            || (kind == SOL_TOKEN_PIPE_PIPE && left.value.as.boolean))) return left;
    MirFlow right = evaluate_predicate(eval, expression->as.binary.right,
        result, self);
    if (right.kind == MIR_FLOW_ERROR) {
        sol_interpreter_value_free(&left.value);
        return right;
    }
    MirFlow output = pred_value();
    if (!binary_value(eval, kind, &left.value, &right.value,
        expression->span, &output.value)) output.kind = MIR_FLOW_ERROR;
    sol_interpreter_value_free(&left.value);
    sol_interpreter_value_free(&right.value);
    return output;
}

static bool collect_pattern_leaves(const SolIr *ir, SolIrPatternId id,
    const SolInterpreterValue *value, const SolInterpreterValue **leaves,
    size_t *count, size_t capacity, size_t depth) {
    if (id >= ir->pattern_count || depth > 64) return false;
    const SolIrPattern *pattern = &ir->patterns[id];
    if (pattern->kind == SOL_IR_PATTERN_BINDING) {
        if (*count >= capacity) return false;
        leaves[(*count)++] = value;
        return true;
    }
    for (size_t index = 0; index < pattern->children.count; ++index) {
        const SolIrPatternChild *child
            = &ir->pattern_children[pattern->children.offset + index];
        size_t ordinal = child->ordinal;
        if (child->field != SOL_IR_NONE) {
            ordinal = child->field - ir->definitions[pattern->definition].fields.offset;
        }
        if (ordinal >= value->as.aggregate.field_count
            || !collect_pattern_leaves(ir, child->pattern,
                &value->as.aggregate.fields[ordinal], leaves, count,
                capacity, depth + 1)) return false;
    }
    return true;
}

static MirFlow pred_construct(MirEval *eval, const SolIrExpression *expression,
    const SolInterpreterValue *result, const SolInterpreterValue *self) {
    const SolIr *ir = eval->request->ir;
    SolIrSlice slice = expression->kind == SOL_IR_EXPR_RECORD
        ? expression->as.record.fields
        : expression->kind == SOL_IR_EXPR_TUPLE
        ? expression->as.tuple.operands : expression->as.call.operands;
    size_t count = slice.count;
    SolInterpreterValue *values = count == 0 ? NULL
        : calloc(count, sizeof(*values));
    if (count != 0 && values == NULL) return pred_error();
    for (size_t index = 0; index < count; ++index) {
        MirFlow value = evaluate_predicate(eval,
            ir->operands[slice.offset + index].value, result, self);
        if (value.kind == MIR_FLOW_ERROR) {
            free_values(values, count);
            return value;
        }
        values[index] = value.value;
    }
    if (!new_node(eval, expression->span)) {
        free_values(values, count);
        return pred_error();
    }
    MirFlow output = pred_value();
    if (expression->kind == SOL_IR_EXPR_RECORD) {
        SolIrDefinitionId definition = expression->as.record.definition;
        if (ir->definitions[definition].kind == SOL_IR_DEFINITION_CAPABILITY) {
            set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, expression->span,
                "capability construction is impure in a predicate");
            free_values(values, count);
            return pred_error();
        }
        SolInterpreterValue *fields = count == 0 ? NULL
            : calloc(count, sizeof(*fields));
        if (count != 0 && fields == NULL) goto allocation_failure;
        for (size_t index = 0; index < count; ++index) {
            size_t ordinal = ir->operands[slice.offset + index].formal
                - ir->definitions[definition].fields.offset;
            fields[ordinal] = values[index];
            value_init(&values[index]);
        }
        free_values(values, count);
        output.value.kind = SOL_INTERPRETER_VALUE_RECORD;
        output.value.as.aggregate.definition = definition;
        output.value.as.aggregate.variant = SOL_IR_NONE;
        output.value.as.aggregate.fields = fields;
        output.value.as.aggregate.field_count = count;
        return output;
    }
    if (expression->kind == SOL_IR_EXPR_TUPLE) {
        output.value.kind = SOL_INTERPRETER_VALUE_TUPLE;
        output.value.as.aggregate.definition = SOL_IR_NONE;
        output.value.as.aggregate.variant = SOL_IR_NONE;
        output.value.as.aggregate.fields = values;
        output.value.as.aggregate.field_count = count;
        return output;
    }
    SolIrCallKind kind = expression->as.call.kind;
    if (kind == SOL_IR_CALL_BUILTIN_NONE || kind == SOL_IR_CALL_BUILTIN_SOME
        || kind == SOL_IR_CALL_BUILTIN_OK || kind == SOL_IR_CALL_BUILTIN_ERR) {
        output.value.kind = kind == SOL_IR_CALL_BUILTIN_NONE
                || kind == SOL_IR_CALL_BUILTIN_SOME
            ? SOL_INTERPRETER_VALUE_OPTION : SOL_INTERPRETER_VALUE_RESULT;
        output.value.as.sum.has_value = kind != SOL_IR_CALL_BUILTIN_NONE;
        output.value.as.sum.is_error = kind == SOL_IR_CALL_BUILTIN_ERR;
        if (output.value.as.sum.has_value) {
            output.value.as.sum.value = malloc(sizeof(*output.value.as.sum.value));
            if (output.value.as.sum.value == NULL) goto allocation_failure;
            *output.value.as.sum.value = values[0];
            value_init(&values[0]);
        }
        free_values(values, count);
        return output;
    }
    if (kind == SOL_IR_CALL_ENUM_CONSTRUCTOR) {
        SolIrVariantId variant = expression->as.call.variant;
        SolInterpreterValue *fields = count == 0 ? NULL
            : calloc(count, sizeof(*fields));
        if (count != 0 && fields == NULL) goto allocation_failure;
        for (size_t index = 0; index < count; ++index) {
            size_t ordinal = ir->operands[slice.offset + index].formal
                - ir->variants[variant].fields.offset;
            fields[ordinal] = values[index];
            value_init(&values[index]);
        }
        free_values(values, count);
        output.value.kind = SOL_INTERPRETER_VALUE_ENUM;
        output.value.as.aggregate.definition = ir->variants[variant].owner;
        output.value.as.aggregate.variant = variant;
        output.value.as.aggregate.fields = fields;
        output.value.as.aggregate.field_count = count;
        return output;
    }
    if (kind == SOL_IR_CALL_DISTINCT_CONSTRUCTOR) {
        SolIrDefinitionId definition = expression->as.call.definition;
        if (ir->definitions[definition].kind == SOL_IR_DEFINITION_REFINED) {
            SolObligationId obligation = SOL_IR_NONE;
            for (size_t index = 0; index < ir->obligation_count; ++index) {
                if (ir->obligations[index].owner_kind == SOL_CONTRACT_OWNER_TYPE
                    && ir->obligations[index].owner == definition) obligation = index;
            }
            bool satisfied = false;
            if (obligation == SOL_IR_NONE || !evaluate_refinement_obligation(eval,
                expression, definition, obligation, &values[0], &satisfied)
                || !satisfied) {
                if (eval->result->diagnostic.code == SOL_INTERPRETER_OK) {
                    set_diagnostic(eval, SOL_INTERPRETER_REFINEMENT_VIOLATION,
                        expression->span,
                        "refined type predicate was not satisfied");
                }
                free_values(values, count);
                return pred_error();
            }
        }
        output.value.kind = SOL_INTERPRETER_VALUE_DISTINCT;
        output.value.as.distinct.definition = definition;
        output.value.as.distinct.value = malloc(sizeof(*output.value.as.distinct.value));
        if (output.value.as.distinct.value == NULL) goto allocation_failure;
        *output.value.as.distinct.value = values[0];
        value_init(&values[0]);
        free_values(values, count);
        return output;
    }
    free_values(values, count);
    set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, expression->span,
        "impure call form reached predicate construction evaluator");
    return pred_error();

allocation_failure:
    free_values(values, count);
    set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT, expression->span,
        "predicate value allocation failed");
    return pred_error();
}

static MirFlow pred_call(MirEval *eval, const SolIrExpression *expression,
    const SolInterpreterValue *result, const SolInterpreterValue *self) {
    const SolIr *ir = eval->request->ir;
    SolIrCallKind kind = expression->as.call.kind;
    if (kind == SOL_IR_CALL_BUILTIN_NONE || kind == SOL_IR_CALL_BUILTIN_SOME
        || kind == SOL_IR_CALL_BUILTIN_OK || kind == SOL_IR_CALL_BUILTIN_ERR
        || kind == SOL_IR_CALL_ENUM_CONSTRUCTOR
        || kind == SOL_IR_CALL_DISTINCT_CONSTRUCTOR) {
        return pred_construct(eval, expression, result, self);
    }
    if (kind == SOL_IR_CALL_CAPABILITY) {
        set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, expression->span,
            "capability operation is impure in a predicate");
        return pred_error();
    }
    MirFlow receiver = pred_value();
    MirFlow callee = pred_value();
    if (kind == SOL_IR_CALL_METHOD) {
        receiver = evaluate_predicate(eval, expression->as.call.receiver,
            result, self);
        if (receiver.kind == MIR_FLOW_ERROR) return receiver;
    } else if (kind == SOL_IR_CALL_CALLBACK) {
        callee = evaluate_predicate(eval, expression->as.call.callee,
            result, self);
        if (callee.kind == MIR_FLOW_ERROR) return callee;
    }
    size_t count = expression->as.call.operands.count;
    SolInterpreterValue *arguments = count == 0 ? NULL
        : calloc(count, sizeof(*arguments));
    if (count != 0 && arguments == NULL) goto fail;
    for (size_t index = 0; index < count; ++index) {
        const SolIrOperand *operand
            = &ir->operands[expression->as.call.operands.offset + index];
        if (operand->access == SOL_ACCESS_EXCLUSIVE) {
            set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, expression->span,
                "exclusive call is impure in a predicate");
            goto fail;
        }
        MirFlow argument = evaluate_predicate(eval, operand->value, result, self);
        if (argument.kind == MIR_FLOW_ERROR) goto fail;
        arguments[index] = argument.value;
    }
    SolIrCallableId target = expression->as.call.callable;
    const SolInterpreterValue *call_receiver = NULL;
    if (kind == SOL_IR_CALL_CALLBACK) {
        if (callee.value.kind != SOL_INTERPRETER_VALUE_FUNCTION) {
            set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, expression->span,
                "predicate callback is not a pure function value");
            goto fail;
        }
        target = callee.value.as.callable.callable;
    } else if (kind == SOL_IR_CALL_METHOD) {
        call_receiver = &receiver.value;
        const SolIrDispatchEvidence *selected = NULL;
        for (size_t index = 0; index < expression->as.call.evidence.count; ++index) {
            const SolIrDispatchEvidence *candidate
                = &ir->evidence[expression->as.call.evidence.offset + index];
            if (candidate->requirement == target) {
                selected = resolve_evidence(eval, candidate);
                break;
            }
        }
        if (selected == NULL) {
            set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, expression->span,
                "predicate method evidence is unavailable");
            goto fail;
        }
        target = selected->method;
    }
    const SolMir *mir = provided_mir(eval, target, expression->span, false);
    MirFlow output = pred_error();
    if (mir != NULL) {
        value_init(&output.value);
        const SolIrTypeId *types = expression->as.call.type_arguments.count == 0
            ? NULL : ir->type_ids + expression->as.call.type_arguments.offset;
        const SolIrDispatchEvidence *evidence
            = expression->as.call.evidence.count == 0 ? NULL
            : ir->evidence + expression->as.call.evidence.offset;
        size_t type_count = expression->as.call.type_arguments.count;
        size_t evidence_count = expression->as.call.evidence.count;
        if (kind == SOL_IR_CALL_METHOD) {
            types = NULL;
            type_count = 0;
            evidence = NULL;
            evidence_count = 0;
        }
        if (execute_callable(eval, mir, call_receiver, arguments, count,
            types, type_count, evidence, evidence_count,
            &output.value, NULL, NULL,
            expression->span)) output.kind = MIR_FLOW_VALUE;
    }
    free_values(arguments, count);
    sol_interpreter_value_free(&receiver.value);
    sol_interpreter_value_free(&callee.value);
    return output;

fail:
    free_values(arguments, count);
    sol_interpreter_value_free(&receiver.value);
    sol_interpreter_value_free(&callee.value);
    return pred_error();
}

static MirFlow pred_block(MirEval *eval, const SolIrExpression *expression,
    const SolInterpreterValue *result, const SolInterpreterValue *self) {
    const SolIr *ir = eval->request->ir;
    SolIrLocalId *bound = expression->as.block.statements.count == 0 ? NULL
        : calloc(expression->as.block.statements.count, sizeof(*bound));
    if (expression->as.block.statements.count != 0 && bound == NULL) {
        return pred_error();
    }
    size_t bound_count = 0;
    MirFlow last = pred_value();
    sol_interpreter_value_unit(&last.value);
    for (size_t index = 0; index < expression->as.block.statements.count; ++index) {
        const SolIrStatement *statement = &ir->statements[ir->statement_ids[
            expression->as.block.statements.offset + index]];
        if (statement->kind != SOL_IR_STATEMENT_LET
            && statement->kind != SOL_IR_STATEMENT_EXPRESSION
            && statement->kind != SOL_IR_STATEMENT_RETURN) {
            set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, statement->span,
                "impure statement form reached predicate evaluator");
            sol_interpreter_value_free(&last.value);
            last = pred_error();
            break;
        }
        MirFlow value = evaluate_predicate(eval, statement->expression,
            result, self);
        if (value.kind == MIR_FLOW_ERROR) {
            sol_interpreter_value_free(&last.value);
            last = value;
            break;
        }
        if (statement->kind == SOL_IR_STATEMENT_LET) {
            SolIrLocalId local = statement->local;
            if (local >= ir->local_count || eval->frame->bound[local]) {
                sol_interpreter_value_free(&value.value);
                set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR,
                    statement->span, "predicate let local is already bound");
                sol_interpreter_value_free(&last.value);
                last = pred_error();
                break;
            }
            eval->frame->locals[local] = value.value;
            eval->frame->bound[local] = true;
            bound[bound_count++] = local;
            continue;
        }
        sol_interpreter_value_free(&last.value);
        last = value;
        if (statement->kind == SOL_IR_STATEMENT_RETURN) break;
    }
    while (bound_count != 0) {
        SolIrLocalId local = bound[--bound_count];
        sol_interpreter_value_free(&eval->frame->locals[local]);
        eval->frame->bound[local] = false;
    }
    free(bound);
    return last;
}

static MirFlow evaluate_predicate(MirEval *eval, SolIrExpressionId id,
    const SolInterpreterValue *result, const SolInterpreterValue *self) {
    const SolIr *ir = eval->request->ir;
    if (id >= ir->expression_count) {
        set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, (SolSpan){0},
            "predicate expression is out of range");
        return pred_error();
    }
    const SolIrExpression *expression = &ir->expressions[id];
    if (!consume(eval, &eval->result->used.steps, eval->request->limits.steps,
        SOL_INTERPRETER_STEP_LIMIT, expression->span, "predicate step")) {
        return pred_error();
    }
    MirFlow output = pred_value();
    switch (expression->kind) {
        case SOL_IR_EXPR_INTEGER:
            if (!new_node(eval, expression->span)) return pred_error();
            sol_interpreter_value_int64(&output.value, expression->as.integer);
            return output;
        case SOL_IR_EXPR_BOOL:
            if (!new_node(eval, expression->span)) return pred_error();
            sol_interpreter_value_bool(&output.value, expression->as.boolean);
            return output;
        case SOL_IR_EXPR_STRING: {
            size_t length = strlen(expression->as.string);
            if (!reserve_value(eval, expression->span, 1, length)
                || !sol_interpreter_value_text(&output.value,
                    expression->as.string, length)) return pred_error();
            return output;
        }
        case SOL_IR_EXPR_UNIT:
            if (!new_node(eval, expression->span)) return pred_error();
            sol_interpreter_value_unit(&output.value);
            return output;
        case SOL_IR_EXPR_PLACE: {
            const SolIrPlace *place = &ir->places[expression->as.place];
            if (place->root_kind == SOL_IR_PLACE_ROOT_TEMPORARY) {
                output = evaluate_predicate(eval, place->temporary, result, self);
                if (output.kind == MIR_FLOW_ERROR) return output;
                for (size_t index = 0; index < place->projections.count; ++index) {
                    SolInterpreterValue *slot = place_field(ir, &output.value,
                        &ir->projections[place->projections.offset + index]);
                    if (slot == NULL) {
                        sol_interpreter_value_free(&output.value);
                        return pred_error();
                    }
                    SolInterpreterValue selected = *slot;
                    value_init(slot);
                    sol_interpreter_value_free(&output.value);
                    output.value = selected;
                }
                return output;
            }
            if (expression->local_use == SOL_IR_LOCAL_USE_MOVE
                || place->local >= ir->local_count || !eval->frame->bound[place->local]) {
                set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, expression->span,
                    "moving or unavailable local reached predicate evaluator");
                return pred_error();
            }
            SolMirPlace mir_place = {place->local, expression->as.place};
            SolInterpreterValue *slot = place_slot(eval, eval->frame, mir_place);
            if (slot == NULL || !clone_value(eval, expression->span,
                &output.value, slot)) return pred_error();
            return output;
        }
        case SOL_IR_EXPR_DEFINITION: {
            SolIrDefinitionId definition = expression->as.definition;
            if (definition >= ir->definition_count
                || ir->definitions[definition].kind != SOL_IR_DEFINITION_FUNCTION) {
                set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, expression->span,
                    "predicate function value is invalid");
                return pred_error();
            }
            if (!new_node(eval, expression->span)) return pred_error();
            output.value.kind = SOL_INTERPRETER_VALUE_FUNCTION;
            output.value.as.callable.callable = ir->definitions[definition].callable;
            return output;
        }
        case SOL_IR_EXPR_REFINEMENT_SELF:
            if (self == NULL || !clone_value(eval, expression->span,
                &output.value, self)) {
                set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, expression->span,
                    "self is unavailable in predicate");
                return pred_error();
            }
            return output;
        case SOL_IR_EXPR_RESULT:
            if (result == NULL || !clone_value(eval, expression->span,
                &output.value, result)) {
                set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, expression->span,
                    "result is unavailable in predicate");
                return pred_error();
            }
            return output;
        case SOL_IR_EXPR_SNAPSHOT_READ: {
            SolIrSnapshotId snapshot = expression->as.snapshot;
            if (snapshot >= ir->snapshot_count
                || !eval->frame->snapshot_bound[snapshot]
                || !clone_value(eval, expression->span, &output.value,
                    &eval->frame->snapshots[snapshot])) {
                set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, expression->span,
                    "old snapshot is unavailable in predicate");
                return pred_error();
            }
            return output;
        }
        case SOL_IR_EXPR_UNARY: {
            MirFlow operand = evaluate_predicate(eval,
                expression->as.unary.operand, result, self);
            if (operand.kind == MIR_FLOW_ERROR) return operand;
            if (!new_node(eval, expression->span)) {
                sol_interpreter_value_free(&operand.value);
                return pred_error();
            }
            if (expression->as.unary.operator_kind == SOL_TOKEN_BANG
                && operand.value.kind == SOL_INTERPRETER_VALUE_BOOL) {
                sol_interpreter_value_bool(&output.value, !operand.value.as.boolean);
            } else if (expression->as.unary.operator_kind == SOL_TOKEN_MINUS
                && operand.value.kind == SOL_INTERPRETER_VALUE_INT64
                && operand.value.as.integer != INT64_MIN) {
                sol_interpreter_value_int64(&output.value,
                    -operand.value.as.integer);
            } else {
                set_diagnostic(eval, operand.value.kind == SOL_INTERPRETER_VALUE_INT64
                    ? SOL_INTERPRETER_INTEGER_OVERFLOW
                    : SOL_INTERPRETER_TYPE_INVARIANT,
                    expression->span, "invalid unary operation in predicate");
                output.kind = MIR_FLOW_ERROR;
            }
            sol_interpreter_value_free(&operand.value);
            return output;
        }
        case SOL_IR_EXPR_BINARY:
            return pred_binary(eval, expression, result, self);
        case SOL_IR_EXPR_CALL:
            return pred_call(eval, expression, result, self);
        case SOL_IR_EXPR_RECORD:
        case SOL_IR_EXPR_TUPLE:
            return pred_construct(eval, expression, result, self);
        case SOL_IR_EXPR_VARIANT:
            if (!new_node(eval, expression->span)) return pred_error();
            output.value.kind = SOL_INTERPRETER_VALUE_ENUM;
            output.value.as.aggregate.variant = expression->as.variant.variant;
            output.value.as.aggregate.definition
                = ir->variants[expression->as.variant.variant].owner;
            return output;
        case SOL_IR_EXPR_IF: {
            MirFlow condition = evaluate_predicate(eval,
                expression->as.if_expr.condition, result, self);
            if (condition.kind == MIR_FLOW_ERROR) return condition;
            if (condition.value.kind != SOL_INTERPRETER_VALUE_BOOL) {
                sol_interpreter_value_free(&condition.value);
                set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR,
                    expression->span, "predicate if condition is not Bool");
                return pred_error();
            }
            bool selected = condition.value.as.boolean;
            sol_interpreter_value_free(&condition.value);
            return evaluate_predicate(eval, selected
                ? expression->as.if_expr.then_branch
                : expression->as.if_expr.else_branch, result, self);
        }
        case SOL_IR_EXPR_MATCH: {
            MirFlow scrutinee = evaluate_predicate(eval,
                expression->as.match_expr.scrutinee, result, self);
            if (scrutinee.kind == MIR_FLOW_ERROR) return scrutinee;
            for (size_t index = 0; index < expression->as.match_expr.arms.count;
                ++index) {
                const SolIrArm *arm = &ir->arms[ir->arm_ids[
                    expression->as.match_expr.arms.offset + index]];
                int matched = match_pattern(eval, arm->pattern,
                    &scrutinee.value, 0);
                if (matched < 0) {
                    sol_interpreter_value_free(&scrutinee.value);
                    return pred_error();
                }
                if (matched == 0) continue;
                const SolInterpreterValue **leaves = arm->bindings.count == 0
                    ? NULL : calloc(arm->bindings.count, sizeof(*leaves));
                size_t leaf_count = 0;
                if (arm->bindings.count != 0 && (leaves == NULL
                    || !collect_pattern_leaves(ir, arm->pattern,
                        &scrutinee.value, leaves, &leaf_count,
                        arm->bindings.count, 0))) {
                    free(leaves);
                    break;
                }
                size_t bound = 0;
                for (; bound < arm->bindings.count; ++bound) {
                    SolIrLocalId local = ir->roots[arm->bindings.offset + bound];
                    if (eval->frame->bound[local] || !clone_value(eval, arm->span,
                        &eval->frame->locals[local], leaves[bound])) break;
                    eval->frame->bound[local] = true;
                }
                free(leaves);
                bool selected = bound == arm->bindings.count;
                bool guard_failed = false;
                if (selected && arm->guard != SOL_IR_NONE) {
                    MirFlow guard = evaluate_predicate(eval, arm->guard,
                        result, self);
                    if (guard.kind == MIR_FLOW_ERROR) guard_failed = true;
                    else if (guard.value.kind != SOL_INTERPRETER_VALUE_BOOL) {
                        set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR,
                            arm->span, "predicate match guard is not Bool");
                        guard_failed = true;
                    } else selected = guard.value.as.boolean;
                    sol_interpreter_value_free(&guard.value);
                }
                MirFlow arm_result = pred_error();
                if (selected) arm_result = evaluate_predicate(eval,
                    arm->body, result, self);
                while (bound != 0) {
                    SolIrLocalId local = ir->roots[
                        arm->bindings.offset + --bound];
                    sol_interpreter_value_free(&eval->frame->locals[local]);
                    eval->frame->bound[local] = false;
                }
                if (guard_failed) {
                    sol_interpreter_value_free(&scrutinee.value);
                    return pred_error();
                }
                if (selected) {
                    sol_interpreter_value_free(&scrutinee.value);
                    return arm_result;
                }
            }
            sol_interpreter_value_free(&scrutinee.value);
            set_diagnostic(eval, SOL_INTERPRETER_NO_MATCH, expression->span,
                "predicate match has no runtime arm");
            return pred_error();
        }
        case SOL_IR_EXPR_BLOCK:
            return pred_block(eval, expression, result, self);
        case SOL_IR_EXPR_PROPAGATE: {
            MirFlow sum = evaluate_predicate(eval,
                expression->as.propagate.operand, result, self);
            if (sum.kind == MIR_FLOW_ERROR) return sum;
            if (!sum.value.as.sum.has_value || sum.value.as.sum.is_error) {
                set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR,
                    expression->span, "residual propagation is invalid in predicate");
                sol_interpreter_value_free(&sum.value);
                return pred_error();
            }
            output.value = *sum.value.as.sum.value;
            value_init(sum.value.as.sum.value);
            sol_interpreter_value_free(&sum.value);
            return output;
        }
        case SOL_IR_EXPR_HANDLE:
        case SOL_IR_EXPR_BOUND_OPERATION:
            set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, expression->span,
                "effectful form reached pure predicate evaluator");
            return pred_error();
        case SOL_IR_EXPR_COMPILE_TIME_HEAD:
            set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, expression->span,
                "compile-time expression reached predicate evaluator");
            return pred_error();
    }
    set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, expression->span,
        "unknown predicate expression kind");
    return pred_error();
}

static bool evaluate_obligation(MirEval *eval, SolObligationId obligation,
    const SolInterpreterValue *result, const SolInterpreterValue *self,
    bool *satisfied) {
    const SolIr *ir = eval->request->ir;
    if (obligation >= ir->obligation_count) {
        set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, (SolSpan){0},
            "MIR obligation is out of range");
        return false;
    }
    MirFlow predicate = evaluate_predicate(eval,
        ir->obligations[obligation].predicate, result, self);
    if (predicate.kind == MIR_FLOW_ERROR) return false;
    if (predicate.value.kind != SOL_INTERPRETER_VALUE_BOOL) {
        sol_interpreter_value_free(&predicate.value);
        set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR,
            ir->expressions[ir->obligations[obligation].predicate].span,
            "contract or refinement predicate did not produce Bool");
        return false;
    }
    *satisfied = predicate.value.as.boolean;
    sol_interpreter_value_free(&predicate.value);
    return true;
}

static bool evaluate_refinement_obligation(MirEval *eval,
    const SolIrExpression *construction, SolIrDefinitionId definition,
    SolObligationId obligation, const SolInterpreterValue *self,
    bool *satisfied) {
    const SolIr *ir = eval->request->ir;
    if (construction == NULL || construction->type >= ir->type_count
        || definition >= ir->definition_count) {
        set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR,
            construction == NULL ? (SolSpan){0} : construction->span,
            "refined construction substitution metadata is invalid");
        return false;
    }
    const SolIrType *nominal = &ir->types[construction->type];
    const SolIrDefinition *metadata = &ir->definitions[definition];
    if (nominal->kind != SOL_IR_TYPE_NOMINAL
        || nominal->definition != definition
        || nominal->argument_count != metadata->generic_parameters.count) {
        set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, construction->span,
            "refined construction type arguments are invalid");
        return false;
    }
    MirFrame predicate;
    memset(&predicate, 0, sizeof(predicate));
    predicate.parent = eval->frame;
    predicate.mir = eval->frame == NULL ? NULL : eval->frame->mir;
    predicate.observe_cleanup = false;
    predicate.locals = ir->local_count == 0 ? NULL
        : calloc(ir->local_count, sizeof(*predicate.locals));
    predicate.bound = ir->local_count == 0 ? NULL
        : calloc(ir->local_count, sizeof(*predicate.bound));
    if (ir->local_count != 0
        && (predicate.locals == NULL || predicate.bound == NULL)) {
        set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT,
            construction->span,
            "refinement predicate local frame allocation failed");
        frame_release(eval, &predicate, construction->span);
        return false;
    }
    predicate.type_parameter_offset = metadata->generic_parameters.offset;
    predicate.type_arguments = nominal->argument_count == 0 ? NULL
        : ir->type_ids + nominal->argument_offset;
    predicate.type_argument_count = nominal->argument_count;
    if (construction->kind == SOL_IR_EXPR_CALL) {
        predicate.evidence = construction->as.call.evidence.count == 0 ? NULL
            : ir->evidence + construction->as.call.evidence.offset;
        predicate.evidence_count = construction->as.call.evidence.count;
    }
    MirFrame *saved = eval->frame;
    eval->frame = &predicate;
    bool evaluated = evaluate_obligation(eval, obligation, NULL, self,
        satisfied);
    eval->frame = saved;
    frame_release(eval, &predicate, construction->span);
    return evaluated;
}

typedef struct {
    MirFrame operation;
    MirFrame contract;
    MirFrame *caller;
    SolIrCallableId callable;
    const SolInterpreterValue *receiver;
    bool ready;
} OperationContracts;

static bool bind_contract_local(MirEval *eval, MirFrame *frame,
    SolIrLocalId local, const SolInterpreterValue *value, SolSpan span) {
    if (local >= eval->request->ir->local_count || frame->registered[local]
        || !register_local(eval, frame, local, span)
        || !clone_value(eval, span, &frame->locals[local], value)) return false;
    frame->bound[local] = true;
    if (value->kind == SOL_INTERPRETER_VALUE_CAPABILITY) {
        frame->authority_known[local] = true;
        frame->authority_roots[local] = value->as.capability.root;
    }
    return true;
}

static void release_operation_contracts(MirEval *eval,
    OperationContracts *state, SolSpan span) {
    if (!state->ready) return;
    eval->frame = state->caller;
    frame_release(eval, &state->contract, span);
    frame_release(eval, &state->operation, span);
    state->ready = false;
}

static bool prepare_operation_contracts(MirEval *eval,
    OperationContracts *state, SolIrCallableId callable,
    const SolInterpreterValue *receiver, const SolInterpreterValue *arguments,
    size_t argument_count, SolSpan span) {
    memset(state, 0, sizeof(*state));
    if (eval->request->contracts == SOL_INTERPRETER_CONTRACTS_IGNORE) return true;
    const SolIr *ir = eval->request->ir;
    const SolIrCallable *operation = &ir->callables[callable];
    state->caller = eval->frame;
    state->callable = callable;
    state->receiver = receiver;
    if (!frame_allocate(eval, &state->operation, eval->frame->mir, span)) {
        return false;
    }
    state->operation.parent = state->caller;
    state->operation.observe_cleanup = false;
    eval->frame = &state->operation;
    bool ready = operation->receiver == SOL_IR_NONE
        || bind_contract_local(eval, &state->operation, operation->receiver,
            receiver, span);
    if (ready && operation->capability_source != SOL_IR_NONE) {
        ready = receiver != NULL && receiver->as.capability.source != NULL
            && bind_contract_local(eval, &state->operation,
                operation->capability_source, receiver->as.capability.source,
                span);
    }
    for (size_t index = 0; ready && index < argument_count; ++index) {
        ready = bind_contract_local(eval, &state->operation,
            ir->roots[operation->parameters.offset + index],
            &arguments[index], span);
    }
    if (ready) ready = contract_frame_init(eval, &state->contract,
        &state->operation, span);
    state->ready = ready;
    if (!ready) {
        eval->frame = state->caller;
        frame_release(eval, &state->operation, span);
        return false;
    }
    eval->frame = &state->contract;
    for (size_t id = 0; id < ir->obligation_count; ++id) {
        const SolIrObligation *obligation = &ir->obligations[id];
        if (obligation->owner_kind != SOL_CONTRACT_OWNER_CAPABILITY_MEMBER
            || obligation->owner != callable
            || obligation->kind != SOL_CONTRACT_REQUIRES) continue;
        bool satisfied = false;
        if (!evaluate_obligation(eval, id, NULL, receiver, &satisfied)
            || !satisfied) {
            if (!satisfied && eval->result->diagnostic.code
                    == SOL_INTERPRETER_OK) {
                set_diagnostic(eval, SOL_INTERPRETER_REQUIRE_VIOLATION,
                    ir->expressions[obligation->predicate].span,
                    "requires contract was not satisfied");
            }
            release_operation_contracts(eval, state, span);
            return false;
        }
    }
    for (size_t snapshot = 0; snapshot < ir->snapshot_count; ++snapshot) {
        const SolIrSnapshot *metadata = &ir->snapshots[snapshot];
        const SolIrObligation *obligation
            = &ir->obligations[metadata->obligation];
        if (obligation->owner_kind != SOL_CONTRACT_OWNER_CAPABILITY_MEMBER
            || obligation->owner != callable
            || obligation->kind != SOL_CONTRACT_ENSURES) continue;
        MirFlow value = evaluate_predicate(eval, metadata->operand, NULL,
            receiver);
        if (value.kind == MIR_FLOW_ERROR) {
            release_operation_contracts(eval, state, span);
            return false;
        }
        state->contract.snapshots[snapshot] = value.value;
        state->contract.snapshot_bound[snapshot] = true;
    }
    eval->frame = state->caller;
    return true;
}

static bool check_operation_postconditions(MirEval *eval,
    OperationContracts *state, const SolInterpreterValue *result,
    SolSpan span) {
    if (!state->ready) return true;
    const SolIr *ir = eval->request->ir;
    eval->frame = &state->contract;
    bool valid = true;
    for (size_t id = 0; valid && id < ir->obligation_count; ++id) {
        const SolIrObligation *obligation = &ir->obligations[id];
        if (obligation->owner_kind != SOL_CONTRACT_OWNER_CAPABILITY_MEMBER
            || obligation->owner != state->callable
            || obligation->kind != SOL_CONTRACT_ENSURES) continue;
        bool success = result->kind == SOL_INTERPRETER_VALUE_RESULT
            && !result->as.sum.is_error;
        bool failure = result->kind == SOL_INTERPRETER_VALUE_RESULT
            && result->as.sum.is_error;
        if ((obligation->outcome == SOL_CONTRACT_OUTCOME_SUCCESS && !success)
            || (obligation->outcome == SOL_CONTRACT_OUTCOME_FAILURE
                && !failure)) continue;
        const SolInterpreterValue *binding = NULL;
        if (obligation->result_available) {
            binding = obligation->outcome == SOL_CONTRACT_OUTCOME_SUCCESS
                ? result->as.sum.value : result;
        }
        bool satisfied = false;
        valid = evaluate_obligation(eval, id, binding, state->receiver,
            &satisfied) && satisfied;
        if (!valid && eval->result->diagnostic.code == SOL_INTERPRETER_OK) {
            set_diagnostic(eval, SOL_INTERPRETER_ENSURE_VIOLATION,
                ir->expressions[obligation->predicate].span,
                "ensures contract was not satisfied");
        }
    }
    eval->frame = state->caller;
    if (!valid) release_operation_contracts(eval, state, span);
    return valid;
}

static bool invoke_operation(MirEval *eval, SolIrCallableId callable,
    const SolInterpreterValue *receiver, const SolInterpreterValue *arguments,
    size_t argument_count, SolInterpreterValue *result, SolSpan span) {
    const SolIr *ir = eval->request->ir;
    if (callable >= ir->callable_count || !capability_valid(ir, receiver, 0)
        || ir->callables[callable].kind != SOL_IR_CALLABLE_CAPABILITY
        || ir->callables[callable].owner != receiver->as.capability.definition) {
        set_diagnostic(eval, SOL_INTERPRETER_TYPE_INVARIANT, span,
            "capability operation receiver and callable domains do not match");
        return false;
    }
    const SolIrCallable *operation = &ir->callables[callable];
    for (MirHandler *handler = eval->handler; handler != NULL;
        handler = handler->parent) {
        bool effect = false;
        for (size_t index = 0; index < operation->effects.count; ++index) {
            effect = effect || strcmp(ir->effects[operation->effects.offset + index].name,
                handler->effect_name) == 0;
        }
        if (handler->source != callable || handler->root != receiver->as.capability.root
            || !effect) continue;
        OperationContracts contracts;
        if (!prepare_operation_contracts(eval, &contracts, callable, receiver,
            arguments, argument_count, span)) return false;
        MirHandler *saved = eval->handler;
        eval->handler = handler->parent;
        bool ok = invoke_operation(eval, handler->provider_callable,
            &handler->provider, arguments, argument_count, result, span);
        eval->handler = saved;
        if (ok) ok = check_operation_postconditions(eval, &contracts, result,
            span);
        release_operation_contracts(eval, &contracts, span);
        return ok;
    }
    if (operation->body != SOL_IR_NONE) {
        set_diagnostic(eval, SOL_INTERPRETER_INVALID_REQUEST, span,
            "reachable bodyful capability member cannot be represented by SolMir");
        return false;
    }
    for (size_t index = 0; index < operation->parameters.count; ++index) {
        SolIrLocalId local = ir->roots[operation->parameters.offset + index];
        if (ir->locals[local].access == SOL_ACCESS_EXCLUSIVE) {
            set_diagnostic(eval, SOL_INTERPRETER_UNBOUND_OPERATION, span,
                "bodyless capability operations do not support inout writeback");
            return false;
        }
    }
    OperationContracts contracts;
    if (!prepare_operation_contracts(eval, &contracts, callable, receiver,
        arguments, argument_count, span)) return false;
    if (eval->request->host_operation == NULL) {
        set_diagnostic(eval, SOL_INTERPRETER_UNBOUND_OPERATION, span,
            "capability operation '%s' is unbound", operation->name);
        release_operation_contracts(eval, &contracts, span);
        return false;
    }
    if (eval->request->host_allows != NULL
        && !eval->request->host_allows(eval->request->host_context, ir,
            callable, receiver->as.capability.root)) {
        set_diagnostic(eval, SOL_INTERPRETER_UNBOUND_OPERATION, span,
            "capability operation '%s' is not allowed for this root",
            operation->name);
        release_operation_contracts(eval, &contracts, span);
        return false;
    }
    if (!consume(eval, &eval->result->used.host_calls,
        eval->request->limits.host_calls, SOL_INTERPRETER_HOST_CALL_LIMIT,
        span, "host call")) {
        release_operation_contracts(eval, &contracts, span);
        return false;
    }
    ++eval->result->executed.host_dispatches;
    SolMirTraceEvent trace = trace_event(SOL_MIR_TRACE_HOST);
    trace.source_expression = SOL_IR_NONE;
    trace.operation = callable;
    trace.span = span;
    emit_trace(eval, trace);
    SolInterpreterValue borrowed;
    value_init(&borrowed);
    SolInterpreterHostFailure failure = {0};
    bool ok = eval->request->host_operation(eval->request->host_context, ir,
        callable, receiver->as.capability.root, receiver->as.capability.source,
        arguments, argument_count, &borrowed, &failure);
    if (!ok) {
        if (failure.length > sizeof(failure.bytes)) {
            set_diagnostic(eval, SOL_INTERPRETER_HOST_ERROR, span,
                "host operation returned an invalid failure length");
        } else if (failure.length == 0) {
            set_diagnostic(eval, SOL_INTERPRETER_HOST_ERROR, span,
                "host operation failed");
        } else if (memchr(failure.bytes, '\0', failure.length) != NULL) {
            set_diagnostic(eval, SOL_INTERPRETER_HOST_ERROR, span,
                "host operation returned invalid failure text");
        } else {
            set_diagnostic(eval, SOL_INTERPRETER_HOST_ERROR, span, "%.*s",
                (int)failure.length, failure.bytes);
        }
        release_operation_contracts(eval, &contracts, span);
        return false;
    }
    if (!inspect_value(eval, span, &borrowed,
            SOL_INTERPRETER_TYPE_INVARIANT, false)
        || !value_matches_type(eval, &borrowed, operation->result, receiver, 0)) {
        if (eval->result->diagnostic.code == SOL_INTERPRETER_OK) {
            set_diagnostic(eval, SOL_INTERPRETER_TYPE_INVARIANT, span,
                "host operation result does not match its IR type");
        }
        release_operation_contracts(eval, &contracts, span);
        return false;
    }
    if (!clone_value(eval, span, result, &borrowed)
        || !validate_authority(eval, operation, receiver, result, span)) {
        sol_interpreter_value_free(result);
        release_operation_contracts(eval, &contracts, span);
        return false;
    }
    if (!check_operation_postconditions(eval, &contracts, result, span)) {
        sol_interpreter_value_free(result);
        return false;
    }
    release_operation_contracts(eval, &contracts, span);
    return true;
}

static bool apply_edge(MirEval *eval, MirFrame *frame, const SolMirEdge *edge,
    SolSpan span) {
    size_t count = edge->arguments.count;
    SolInterpreterValue *values = count == 0 ? NULL
        : calloc(count, sizeof(*values));
    if (count != 0 && values == NULL) {
        set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT, span,
            "MIR edge allocation failed");
        return false;
    }
    for (size_t index = 0; index < count; ++index) {
        SolMirValueId source
            = frame->mir->edge_values[edge->arguments.offset + index];
        SolInterpreterValue *value = get_value(eval, frame, source, span);
        if (value == NULL || !clone_internal(eval, span, &values[index], value)) {
            free_values(values, count);
            return false;
        }
    }
    for (size_t index = 0; index < count; ++index) {
        SolMirValueId target = frame->mir->parameter_values[
            frame->mir->blocks[edge->block].parameters.offset + index];
        if (frame->value_bound[target]) {
            sol_interpreter_value_free(&frame->values[target]);
        }
        frame->values[target] = values[index];
        value_init(&values[index]);
        frame->value_bound[target] = true;
    }
    free_values(values, count);
    ++eval->result->executed.edges;
    SolMirTraceEvent event = trace_event(SOL_MIR_TRACE_EDGE);
    event.edge_target = edge->block;
    event.span = span;
    emit_trace(eval, event);
    return true;
}

static bool invoke_terminator(MirEval *eval, MirFrame *frame,
    const SolMirTerminator *term, bool *succeeded) {
    const SolIr *ir = eval->request->ir;
    size_t count = term->as.invoke.arguments.count;
    SolInterpreterValue *arguments = count == 0 ? NULL
        : calloc(count, sizeof(*arguments));
    SolInterpreterValue *writebacks = count == 0 ? NULL
        : calloc(count, sizeof(*writebacks));
    SolInterpreterValue receiver;
    SolInterpreterValue receiver_writeback;
    SolInterpreterValue callee;
    value_init(&receiver);
    value_init(&receiver_writeback);
    value_init(&callee);
    if (count != 0 && (arguments == NULL || writebacks == NULL)) goto allocation_failure;
    if (term->as.invoke.kind == SOL_IR_CALL_CALLBACK) {
        SolMirTemporaryId temporary = term->as.invoke.callee;
        if (temporary >= frame->mir->temporary_count
            || !frame->temporary_bound[temporary]) goto unavailable;
        callee = frame->temporaries[temporary];
        value_init(&frame->temporaries[temporary]);
        frame->temporary_bound[temporary] = false;
    } else if (term->as.invoke.kind == SOL_IR_CALL_METHOD
        || term->as.invoke.kind == SOL_IR_CALL_CAPABILITY) {
        const SolMirCallArgument *source = &term->as.invoke.receiver;
        if (source->access == SOL_ACCESS_OWNED) {
            if (source->temporary >= frame->mir->temporary_count
                || !frame->temporary_bound[source->temporary]) goto unavailable;
            receiver = frame->temporaries[source->temporary];
            value_init(&frame->temporaries[source->temporary]);
            frame->temporary_bound[source->temporary] = false;
        } else {
            SolMirPlace place = {ir->places[source->place].local, source->place};
            SolInterpreterValue *slot = place_slot(eval, frame, place);
            if (slot == NULL || !clone_value(eval, term->span, &receiver, slot)) {
                goto done;
            }
        }
    }
    for (size_t index = 0; index < count; ++index) {
        const SolMirCallArgument *source
            = &frame->mir->call_arguments[term->as.invoke.arguments.offset + index];
        if (source->access == SOL_ACCESS_OWNED) {
            if (source->temporary >= frame->mir->temporary_count
                || !frame->temporary_bound[source->temporary]) goto unavailable;
            arguments[index] = frame->temporaries[source->temporary];
            value_init(&frame->temporaries[source->temporary]);
            frame->temporary_bound[source->temporary] = false;
        } else {
            SolMirPlace place = {ir->places[source->place].local, source->place};
            SolInterpreterValue *slot = place_slot(eval, frame, place);
            if (slot == NULL || !clone_value(eval, term->span,
                &arguments[index], slot)) goto done;
        }
    }
    SolInterpreterValue output;
    value_init(&output);
    bool ok = false;
    if (term->as.invoke.kind == SOL_IR_CALL_CAPABILITY) {
        ok = invoke_operation(eval, term->as.invoke.callable, &receiver,
            arguments, count, &output, term->span);
    } else if (term->as.invoke.kind == SOL_IR_CALL_CALLBACK
        && callee.kind == SOL_INTERPRETER_VALUE_BOUND_OPERATION) {
        ok = invoke_operation(eval, callee.as.callable.callable,
            callee.as.callable.receiver, arguments, count, &output, term->span);
    } else {
        SolIrCallableId target = term->as.invoke.callable;
        const SolInterpreterValue *call_receiver = NULL;
        if (term->as.invoke.kind == SOL_IR_CALL_CALLBACK) {
            if (callee.kind != SOL_INTERPRETER_VALUE_FUNCTION) {
                set_diagnostic(eval, SOL_INTERPRETER_TYPE_INVARIANT, term->span,
                    "callback callee is not executable");
                goto invoked;
            }
            target = callee.as.callable.callable;
        } else if (term->as.invoke.kind == SOL_IR_CALL_METHOD) {
            call_receiver = &receiver;
            if (target < ir->callable_count
                && ir->callables[target].kind == SOL_IR_CALLABLE_TRAIT_REQUIREMENT) {
                const SolIrDispatchEvidence *selected = NULL;
                for (size_t index = 0; index < term->as.invoke.evidence.count; ++index) {
                    const SolIrDispatchEvidence *candidate = &ir->evidence[
                        term->as.invoke.evidence.offset + index];
                    if (candidate->requirement == target) {
                        selected = resolve_evidence(eval, candidate);
                        break;
                    }
                }
                if (selected == NULL) {
                    set_diagnostic(eval, SOL_INTERPRETER_TYPE_INVARIANT,
                        term->span, "method invocation evidence is unavailable");
                    goto invoked;
                }
                target = selected->method;
            }
        }
        const SolMir *callee_mir = provided_mir(eval, target, term->span,
            false);
        if (callee_mir != NULL) {
            const SolIrTypeId *types = term->as.invoke.type_arguments.count == 0
                ? NULL : ir->type_ids + term->as.invoke.type_arguments.offset;
            const SolIrDispatchEvidence *evidence
                = term->as.invoke.evidence.count == 0 ? NULL
                : ir->evidence + term->as.invoke.evidence.offset;
            size_t type_count = term->as.invoke.type_arguments.count;
            size_t evidence_count = term->as.invoke.evidence.count;
            if (term->as.invoke.kind == SOL_IR_CALL_METHOD) {
                types = NULL;
                type_count = 0;
                evidence = NULL;
                evidence_count = 0;
            }
            ok = execute_callable(eval, callee_mir, call_receiver, arguments,
                count, types, type_count, evidence, evidence_count, &output,
                term->as.invoke.receiver.access == SOL_ACCESS_EXCLUSIVE
                    ? &receiver_writeback : NULL,
                writebacks, term->span);
        }
    }
invoked:
    if (ok && term->as.invoke.result != SOL_MIR_NONE) {
        ok = set_value(eval, frame, term->as.invoke.result, &output, term->span);
    }
    sol_interpreter_value_free(&output);
    if (ok && receiver_writeback.kind != SOL_INTERPRETER_VALUE_INVALID) {
        SolIrPlaceId id = term->as.invoke.receiver.place;
        SolMirPlace place = {ir->places[id].local, id};
        drop_place(eval, frame, place, term->span);
        ok = store_place(eval, frame, place, &receiver_writeback, term->span);
    }
    for (size_t index = 0; ok && index < count; ++index) {
        if (writebacks[index].kind == SOL_INTERPRETER_VALUE_INVALID) continue;
        SolIrPlaceId id = frame->mir->call_arguments[
            term->as.invoke.arguments.offset + index].place;
        SolMirPlace place = {ir->places[id].local, id};
        drop_place(eval, frame, place, term->span);
        ok = store_place(eval, frame, place, &writebacks[index], term->span);
    }
    *succeeded = ok;
done:
    free_values(arguments, count);
    free_values(writebacks, count);
    sol_interpreter_value_free(&receiver);
    sol_interpreter_value_free(&receiver_writeback);
    sol_interpreter_value_free(&callee);
    return true;

unavailable:
    set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT, term->span,
        "MIR invoke operand is unavailable");
    goto done;
allocation_failure:
    set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT, term->span,
        "MIR invoke allocation failed");
    goto done;
}

static bool execute_cfg(MirEval *eval, MirFrame *frame,
    SolInterpreterValue *output) {
    const SolIr *ir = eval->request->ir;
    SolMirBlockId block_id = frame->mir->entry;
    for (;;) {
        if (block_id >= frame->mir->block_count) {
            set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT, (SolSpan){0},
                "MIR selected an invalid block");
            return false;
        }
        const SolMirBlock *block = &frame->mir->blocks[block_id];
        ++eval->result->executed.blocks;
        SolMirTraceEvent block_event = trace_event(SOL_MIR_TRACE_BLOCK);
        block_event.block = block_id;
        block_event.span = block->span;
        emit_trace(eval, block_event);
        for (size_t index = 0; index < block->instructions.count; ++index) {
            if (!execute_instruction(eval, frame,
                block->instructions.offset + index)) return false;
        }
        const SolMirTerminator *term = &block->terminator;
        ++eval->result->executed.terminators;
        if (!consume(eval, &eval->result->used.steps, eval->request->limits.steps,
            SOL_INTERPRETER_STEP_LIMIT, term->span, "MIR step")) return false;
        SolMirTraceEvent term_event = trace_event(SOL_MIR_TRACE_TERMINATOR);
        term_event.block = block_id;
        term_event.terminator_kind = term->kind;
        term_event.span = term->span;
        emit_trace(eval, term_event);
        const SolMirEdge *edge = NULL;
        switch (term->kind) {
            case SOL_MIR_TERM_GOTO: edge = &term->as.go_to; break;
            case SOL_MIR_TERM_BRANCH: {
                SolInterpreterValue *condition = get_value(eval, frame,
                    term->as.branch.condition, term->span);
                if (condition == NULL
                    || condition->kind != SOL_INTERPRETER_VALUE_BOOL) {
                    set_diagnostic(eval, SOL_INTERPRETER_TYPE_INVARIANT,
                        term->span, "MIR branch condition is not Bool");
                    return false;
                }
                edge = condition->as.boolean ? &term->as.branch.true_edge
                    : &term->as.branch.false_edge;
                break;
            }
            case SOL_MIR_TERM_RETURN: {
                SolInterpreterValue *value = get_value(eval, frame,
                    term->as.value, term->span);
                if (value == NULL || !clone_internal(eval, term->span,
                    output, value)) return false;
                SolMirTraceEvent event = trace_event(SOL_MIR_TRACE_RETURN);
                event.block = block_id;
                event.span = term->span;
                emit_trace(eval, event);
                return true;
            }
            case SOL_MIR_TERM_PANIC: {
                SolInterpreterValue *message = get_value(eval, frame,
                    term->as.value, term->span);
                if (message == NULL || message->kind != SOL_INTERPRETER_VALUE_TEXT) {
                    set_diagnostic(eval, SOL_INTERPRETER_TYPE_INVARIANT,
                        term->span, "panic message is not Text");
                } else {
                    set_diagnostic(eval, SOL_INTERPRETER_PANIC, term->span,
                        "%s", message->as.text.bytes);
                }
                return false;
            }
            case SOL_MIR_TERM_INVOKE: {
                bool succeeded = false;
                if (!invoke_terminator(eval, frame, term, &succeeded)) return false;
                edge = succeeded ? &term->as.invoke.normal_edge
                    : &term->as.invoke.failure_edge;
                break;
            }
            case SOL_MIR_TERM_RESUME_FAILURE:
                if (eval->result->diagnostic.code == SOL_INTERPRETER_OK) {
                    set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT,
                        term->span, "MIR resumed a missing failure");
                }
                return false;
            case SOL_MIR_TERM_UNREACHABLE:
                set_diagnostic(eval, SOL_INTERPRETER_REACHED_UNREACHABLE,
                    term->span, "execution reached an unreachable statement");
                return false;
            case SOL_MIR_TERM_BREAK:
            case SOL_MIR_TERM_CONTINUE:
                edge = &term->as.transfer.edge;
                break;
            case SOL_MIR_TERM_CHECK_REFINED: {
                SolMirTemporaryId temporary = term->as.check_refined.representation;
                if (temporary >= frame->mir->temporary_count
                    || !frame->temporary_bound[temporary]) {
                    set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT,
                        term->span, "MIR refinement representation is unavailable");
                    edge = &term->as.check_refined.failure_edge;
                    break;
                }
                bool satisfied = false;
                const SolIrExpression *construction
                    = &ir->expressions[term->as.check_refined.source_expression];
                bool evaluated = evaluate_refinement_obligation(eval,
                    construction, term->as.check_refined.definition,
                    term->as.check_refined.obligation,
                    &frame->temporaries[temporary], &satisfied);
                if (!evaluated || !satisfied) {
                    if (evaluated && eval->result->diagnostic.code
                            == SOL_INTERPRETER_OK) {
                        const SolIrObligation *obligation = &ir->obligations[
                            term->as.check_refined.obligation];
                        set_diagnostic(eval, SOL_INTERPRETER_REFINEMENT_VIOLATION,
                            ir->expressions[obligation->predicate].span,
                            "refined type predicate was not satisfied");
                    }
                    sol_interpreter_value_free(&frame->temporaries[temporary]);
                    frame->temporary_bound[temporary] = false;
                    edge = &term->as.check_refined.failure_edge;
                    break;
                }
                SolInterpreterValue distinct;
                value_init(&distinct);
                if (!new_node(eval, term->span)) return false;
                distinct.kind = SOL_INTERPRETER_VALUE_DISTINCT;
                distinct.as.distinct.definition = term->as.check_refined.definition;
                distinct.as.distinct.value = malloc(sizeof(*distinct.as.distinct.value));
                if (distinct.as.distinct.value == NULL) {
                    set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT,
                        term->span, "refinement result allocation failed");
                    return false;
                }
                *distinct.as.distinct.value = frame->temporaries[temporary];
                value_init(&frame->temporaries[temporary]);
                frame->temporary_bound[temporary] = false;
                bool stored = set_value(eval, frame,
                    term->as.check_refined.result, &distinct, term->span);
                sol_interpreter_value_free(&distinct);
                if (!stored) return false;
                edge = &term->as.check_refined.normal_edge;
                break;
            }
            case SOL_MIR_TERM_MATCH_FAILURE:
                set_diagnostic(eval, SOL_INTERPRETER_NO_MATCH, term->span,
                    "match has no runtime arm");
                return false;
            case SOL_MIR_TERM_PROPAGATE: {
                SolMirTemporaryId temporary = term->as.propagate.operand;
                if (temporary >= frame->mir->temporary_count
                    || !frame->temporary_bound[temporary]) {
                    set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT,
                        term->span, "MIR propagation operand is unavailable");
                    return false;
                }
                SolInterpreterValue *sum = &frame->temporaries[temporary];
                bool residual = !sum->as.sum.has_value || sum->as.sum.is_error;
                SolMirValueId result = residual
                    ? term->as.propagate.residual_result
                    : term->as.propagate.value_result;
                SolInterpreterValue transported;
                value_init(&transported);
                if (residual) {
                    transported = *sum;
                    value_init(sum);
                } else {
                    transported = *sum->as.sum.value;
                    value_init(sum->as.sum.value);
                    sol_interpreter_value_free(sum);
                }
                frame->temporary_bound[temporary] = false;
                bool stored = set_value(eval, frame, result,
                    &transported, term->span);
                sol_interpreter_value_free(&transported);
                if (!stored) return false;
                edge = residual ? &term->as.propagate.residual_edge
                    : &term->as.propagate.value_edge;
                break;
            }
            case SOL_MIR_TERM_CHECK_CONTRACT: {
                if (eval->request->contracts == SOL_INTERPRETER_CONTRACTS_IGNORE) {
                    edge = &term->as.check_contract.satisfied_edge;
                    break;
                }
                const SolIrObligation *obligation
                    = &ir->obligations[term->as.check_contract.obligation];
                const SolInterpreterValue *complete = NULL;
                const SolInterpreterValue *binding = NULL;
                if (term->as.check_contract.result != SOL_MIR_NONE) {
                    complete = get_value(eval, frame,
                        term->as.check_contract.result, term->span);
                    if (complete == NULL) return false;
                }
                bool applicable = true;
                if (obligation->outcome == SOL_CONTRACT_OUTCOME_SUCCESS) {
                    applicable = complete != NULL
                        && complete->kind == SOL_INTERPRETER_VALUE_RESULT
                        && !complete->as.sum.is_error;
                } else if (obligation->outcome == SOL_CONTRACT_OUTCOME_FAILURE) {
                    applicable = complete != NULL
                        && complete->kind == SOL_INTERPRETER_VALUE_RESULT
                        && complete->as.sum.is_error;
                }
                if (!applicable) {
                    edge = &term->as.check_contract.satisfied_edge;
                    break;
                }
                if (obligation->result_available) {
                    binding = obligation->outcome == SOL_CONTRACT_OUTCOME_SUCCESS
                        ? complete->as.sum.value : complete;
                }
                bool satisfied = false;
                MirFrame *body_frame = eval->frame;
                if (frame->contract == NULL) {
                    set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT,
                        term->span, "MIR contract frame is missing");
                    return false;
                }
                eval->frame = frame->contract;
                bool evaluated = evaluate_obligation(eval,
                    term->as.check_contract.obligation, binding, NULL,
                    &satisfied);
                eval->frame = body_frame;
                if (!evaluated) {
                    edge = &term->as.check_contract.failure_edge;
                } else {
                    edge = satisfied ? &term->as.check_contract.satisfied_edge
                        : &term->as.check_contract.violation_edge;
                }
                break;
            }
            case SOL_MIR_TERM_CONTRACT_VIOLATION: {
                const SolIrObligation *obligation
                    = &ir->obligations[term->as.contract_violation];
                set_diagnostic(eval, obligation->kind == SOL_CONTRACT_REQUIRES
                    ? SOL_INTERPRETER_REQUIRE_VIOLATION
                    : SOL_INTERPRETER_ENSURE_VIOLATION,
                    ir->expressions[obligation->predicate].span,
                    obligation->kind == SOL_CONTRACT_REQUIRES
                        ? "requires contract was not satisfied"
                        : "ensures contract was not satisfied");
                return false;
            }
            case SOL_MIR_TERM_INVALID:
                set_diagnostic(eval, SOL_INTERPRETER_INVALID_IR, term->span,
                    "MIR contains an invalid terminator");
                return false;
        }
        if (edge == NULL || !apply_edge(eval, frame, edge, term->span)) return false;
        block_id = edge->block;
    }
}

static bool evidence_domain_valid(const SolIr *ir,
    const SolIrDispatchEvidence *evidence) {
    if (evidence->trait >= ir->definition_count
        || evidence->requirement >= ir->callable_count
        || ir->definitions[evidence->trait].kind != SOL_IR_DEFINITION_TRAIT
        || ir->callables[evidence->requirement].kind
            != SOL_IR_CALLABLE_TRAIT_REQUIREMENT
        || ir->callables[evidence->requirement].owner != evidence->trait) return false;
    if (evidence->forwarded) {
        return evidence->parameter < ir->generic_parameter_count
            && ir->generic_parameters[evidence->parameter].trait_bound
                == evidence->trait
            && evidence->implementation == SOL_IR_NONE
            && evidence->method == SOL_IR_NONE && evidence->type == SOL_IR_NONE;
    }
    return evidence->parameter == SOL_IR_NONE
        && evidence->implementation < ir->definition_count
        && evidence->method < ir->callable_count
        && evidence->type < ir->type_count
        && ir->definitions[evidence->implementation].kind
            == SOL_IR_DEFINITION_IMPLEMENTATION
        && ir->definitions[evidence->implementation].implementation_trait
            == evidence->trait
        && ir->definitions[evidence->implementation].implementation_target
            == evidence->type
        && ir->callables[evidence->method].kind
            == SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION
        && ir->callables[evidence->method].owner == evidence->implementation;
}

static bool invocation_metadata_valid(MirEval *eval,
    const SolIrCallable *callable, const SolIrTypeId *types, size_t type_count,
    const SolIrDispatchEvidence *evidence, size_t evidence_count, SolSpan span) {
    const SolIr *ir = eval->request->ir;
    if (callable->generic_parameters.count != type_count
        || (type_count != 0 && types == NULL)
        || (evidence_count != 0 && evidence == NULL)) {
        set_diagnostic(eval, eval->depth == 0 ? SOL_INTERPRETER_INVALID_REQUEST
            : SOL_INTERPRETER_TYPE_INVARIANT, span,
            "generic invocation metadata is incomplete");
        return false;
    }
    for (size_t index = 0; index < type_count; ++index) {
        if (types[index] >= ir->type_count) {
            set_diagnostic(eval, eval->depth == 0
                ? SOL_INTERPRETER_INVALID_REQUEST : SOL_INTERPRETER_TYPE_INVARIANT,
                span, "generic invocation type argument is out of range");
            return false;
        }
    }
    for (size_t index = 0; index < evidence_count; ++index) {
        const SolIrDispatchEvidence *entry = &evidence[index];
        if (!evidence_domain_valid(ir, entry)
            || entry->binding < callable->generic_parameters.offset
            || evidence[index].binding - callable->generic_parameters.offset
                >= callable->generic_parameters.count
            || ir->generic_parameters[entry->binding].trait_bound
                != entry->trait) {
            set_diagnostic(eval, eval->depth == 0
                ? SOL_INTERPRETER_INVALID_REQUEST : SOL_INTERPRETER_TYPE_INVARIANT,
                span, "generic invocation evidence domain is invalid");
            return false;
        }
        size_t ordinal = entry->binding - callable->generic_parameters.offset;
        SolIrTypeId argument = types[ordinal];
        if (entry->forwarded) {
            MirFrame *caller = eval->frame;
            if (caller == NULL || entry->parameter < caller->type_parameter_offset
                || entry->parameter - caller->type_parameter_offset
                    >= caller->type_argument_count
                || ir->types[argument].kind != SOL_IR_TYPE_PARAMETER
                || ir->types[argument].definition != entry->parameter) {
                set_diagnostic(eval, eval->depth == 0
                    ? SOL_INTERPRETER_INVALID_REQUEST
                    : SOL_INTERPRETER_TYPE_INVARIANT,
                    span, "forwarded invocation evidence has no caller binding");
                return false;
            }
        } else if (entry->type != argument) {
            set_diagnostic(eval, eval->depth == 0
                ? SOL_INTERPRETER_INVALID_REQUEST : SOL_INTERPRETER_TYPE_INVARIANT,
                span, "concrete invocation evidence has the wrong type");
            return false;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (evidence[previous].binding == entry->binding
                && evidence[previous].requirement == entry->requirement) {
                set_diagnostic(eval, eval->depth == 0
                    ? SOL_INTERPRETER_INVALID_REQUEST
                    : SOL_INTERPRETER_TYPE_INVARIANT,
                    span, "generic invocation evidence binding is duplicated");
                return false;
            }
        }
    }
    for (size_t ordinal = 0; ordinal < callable->generic_parameters.count;
        ++ordinal) {
        SolIrGenericParameterId parameter
            = callable->generic_parameters.offset + ordinal;
        SolIrDefinitionId trait = ir->generic_parameters[parameter].trait_bound;
        if (trait == SOL_IR_NONE) continue;
        SolIrSlice members = ir->definitions[trait].members;
        for (size_t member = 0; member < members.count; ++member) {
            SolIrCallableId requirement
                = ir->members[members.offset + member].callable;
            size_t found = 0;
            for (size_t index = 0; index < evidence_count; ++index) {
                found += evidence[index].binding == parameter
                    && evidence[index].requirement == requirement;
            }
            if (found != 1) {
                set_diagnostic(eval, eval->depth == 0
                    ? SOL_INTERPRETER_INVALID_REQUEST
                    : SOL_INTERPRETER_TYPE_INVARIANT,
                    span, "generic invocation evidence binding is incomplete");
                return false;
            }
        }
    }
    return true;
}

static bool execute_callable(MirEval *eval, const SolMir *mir,
    const SolInterpreterValue *receiver, const SolInterpreterValue *arguments,
    size_t argument_count, const SolIrTypeId *type_arguments,
    size_t type_argument_count, const SolIrDispatchEvidence *evidence,
    size_t evidence_count, SolInterpreterValue *output,
    SolInterpreterValue *receiver_writeback,
    SolInterpreterValue *argument_writebacks, SolSpan span) {
    const SolIr *ir = eval->request->ir;
    const SolIrCallable *callable = &ir->callables[mir->callable];
    if (callable->parameters.count != argument_count
        || !invocation_metadata_valid(eval, callable, type_arguments,
            type_argument_count, evidence, evidence_count, span)) return false;
    if (eval->depth >= eval->request->limits.call_depth) {
        set_diagnostic(eval, SOL_INTERPRETER_CALL_DEPTH_LIMIT, span,
            "call depth limit exceeded");
        return false;
    }
    MirFrame frame;
    if (!frame_allocate(eval, &frame, mir, span)) return false;
    frame.parent = eval->frame;
    frame.type_arguments = type_arguments;
    frame.type_argument_count = type_argument_count;
    frame.type_parameter_offset = callable->generic_parameters.offset;
    frame.evidence = evidence;
    frame.evidence_count = evidence_count;
    frame.observe_cleanup = true;
    MirFrame *saved_frame = eval->frame;
    MirHandler *saved_handler = eval->handler;
    eval->frame = &frame;
    ++eval->depth;
    ++eval->result->executed.calls;
    if (eval->depth > eval->result->used.call_depth) {
        eval->result->used.call_depth = eval->depth;
    }
    SolMirTraceEvent call_event = trace_event(SOL_MIR_TRACE_CALL);
    call_event.callable = mir->callable;
    call_event.span = span;
    emit_trace(eval, call_event);
    bool ready = true;
    bool receiver_deferred = false;
    if (callable->receiver != SOL_IR_NONE) {
        ready = receiver != NULL && preflight_value_shape(eval, receiver,
            eval->depth == 1 ? SOL_INTERPRETER_INVALID_REQUEST
                : SOL_INTERPRETER_TYPE_INVARIANT,
            span, &receiver_deferred)
            && (receiver_deferred || value_matches_type(eval, receiver,
                ir->locals[callable->receiver].type, receiver, 0));
        if (!ready) {
            set_diagnostic(eval, eval->depth == 1
                ? SOL_INTERPRETER_INVALID_REQUEST : SOL_INTERPRETER_TYPE_INVARIANT,
                span, "call receiver does not match its substituted IR type");
        }
    }
    bool *argument_deferred = argument_count == 0 ? NULL
        : calloc(argument_count, sizeof(*argument_deferred));
    if (argument_count != 0 && argument_deferred == NULL) {
        set_diagnostic(eval, SOL_INTERPRETER_INTERNAL_INVARIANT, span,
            "call argument preflight allocation failed");
        ready = false;
    }
    for (size_t index = 0; ready && index < argument_count; ++index) {
        SolIrLocalId local = ir->roots[callable->parameters.offset + index];
        if (!preflight_value_shape(eval, &arguments[index],
                eval->depth == 1 ? SOL_INTERPRETER_INVALID_REQUEST
                    : SOL_INTERPRETER_TYPE_INVARIANT,
                span, &argument_deferred[index])
            || (!argument_deferred[index]
                && !value_matches_type(eval, &arguments[index],
                    ir->locals[local].type, receiver, 0))) {
            set_diagnostic(eval, eval->depth == 1
                ? SOL_INTERPRETER_INVALID_REQUEST : SOL_INTERPRETER_TYPE_INVARIANT,
                span, "call argument does not match its substituted IR type");
            ready = false;
        }
    }
    if (ready && callable->receiver != SOL_IR_NONE) {
        SolIrLocalId local = callable->receiver;
        ready = register_local(eval, &frame, local, span)
            && clone_value(eval, span, &frame.locals[local], receiver);
        if (ready) {
            frame.bound[local] = true;
            if (receiver->kind == SOL_INTERPRETER_VALUE_CAPABILITY) {
                frame.authority_known[local] = true;
                frame.authority_roots[local] = receiver->as.capability.root;
            }
        }
    }
    for (size_t index = 0; ready && index < argument_count; ++index) {
        SolIrLocalId local = ir->roots[callable->parameters.offset + index];
        ready = register_local(eval, &frame, local, span)
            && clone_value(eval, span, &frame.locals[local], &arguments[index]);
        if (ready) {
            frame.bound[local] = true;
            if (arguments[index].kind == SOL_INTERPRETER_VALUE_CAPABILITY) {
                frame.authority_known[local] = true;
                frame.authority_roots[local] = arguments[index].as.capability.root;
            }
        }
    }
    free(argument_deferred);
    MirFrame contract;
    bool contract_ready = false;
    if (ready && eval->request->contracts == SOL_INTERPRETER_CONTRACTS_CHECK) {
        contract_ready = contract_frame_init(eval, &contract, &frame, span);
        ready = contract_ready;
        if (ready) frame.contract = &contract;
    }
    bool ok = ready && execute_cfg(eval, &frame, output);
    if (ok && !value_matches_type(eval, output, callable->result, receiver, 0)) {
        set_diagnostic(eval, SOL_INTERPRETER_TYPE_INVARIANT, span,
            "callable result does not match its substituted IR type");
        ok = false;
    }
    if (ok && !validate_authority(eval, callable, receiver, output, span)) ok = false;
    if (ok && callable->receiver != SOL_IR_NONE
        && callable->receiver_access == SOL_ACCESS_EXCLUSIVE) {
        SolIrLocalId local = callable->receiver;
        if (receiver_writeback == NULL || !frame.writeback_bound[local]) {
            set_diagnostic(eval, SOL_INTERPRETER_TYPE_INVARIANT, span,
                "exclusive receiver did not remain complete for writeback");
            ok = false;
        } else {
            *receiver_writeback = frame.writebacks[local];
            value_init(&frame.writebacks[local]);
            frame.writeback_bound[local] = false;
        }
    }
    for (size_t index = 0; ok && index < argument_count; ++index) {
        SolIrLocalId local = ir->roots[callable->parameters.offset + index];
        if (ir->locals[local].access != SOL_ACCESS_EXCLUSIVE) continue;
        if (argument_writebacks == NULL || !frame.writeback_bound[local]) {
            set_diagnostic(eval, SOL_INTERPRETER_TYPE_INVARIANT, span,
                "exclusive parameter did not remain complete for writeback");
            ok = false;
        } else {
            argument_writebacks[index] = frame.writebacks[local];
            value_init(&frame.writebacks[local]);
            frame.writeback_bound[local] = false;
        }
    }
    while (eval->handler != saved_handler) {
        MirHandler *handler = eval->handler;
        eval->handler = handler->parent;
        sol_interpreter_value_free(&handler->provider);
        free(handler);
    }
    if (ok && eval->result->diagnostic.code != SOL_INTERPRETER_OK) ok = false;
    if (contract_ready) frame_release(eval, &contract, span);
    frame_release(eval, &frame, span);
    --eval->depth;
    eval->frame = saved_frame;
    if (!ok) sol_interpreter_value_free(output);
    return ok;
}
