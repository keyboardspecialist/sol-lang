#include "sol/interpreter.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { FLOW_VALUE, FLOW_RETURN, FLOW_ERROR } FlowKind;

typedef struct {
    FlowKind kind;
    SolInterpreterValue value;
} Flow;

typedef struct Frame Frame;
typedef struct Handler Handler;

struct Frame {
    Frame *parent;
    SolInterpreterValue *locals;
    bool *bound;
    bool *registered;
    void **authority_roots;
    bool *authority_known;
    SolIrLocalId *binding_order;
    size_t binding_count;
    const SolIrTypeId *type_arguments;
    size_t type_argument_count;
    SolIrGenericParameterId type_parameter_offset;
    const SolIrDispatchEvidence *evidence;
    size_t evidence_count;
};

struct Handler {
    Handler *parent;
    SolIrCallableId source;
    SolIrCallableId provider_callable;
    const char *effect_name;
    void *root;
    SolInterpreterValue provider;
};

typedef struct {
    const SolInterpreterRequest *request;
    SolInterpreterResult *result;
    Frame *frame;
    Handler *handler;
    size_t depth;
} Interpreter;

static void value_clear(SolInterpreterValue *value) {
    memset(value, 0, sizeof(*value));
}

static bool value_shape_valid_recursive(const SolInterpreterValue *value,
    const SolInterpreterValue **ancestors, size_t depth) {
    if (value == NULL || depth >= 256 || (int)value->kind < 0
        || value->kind > SOL_INTERPRETER_VALUE_BOUND_OPERATION) return false;
    for (size_t index = 0; index < depth; ++index) {
        if (ancestors[index] == value) return false;
    }
    ancestors[depth] = value;
    switch (value->kind) {
        case SOL_INTERPRETER_VALUE_INVALID: return false;
        case SOL_INTERPRETER_VALUE_TEXT: return value->as.text.bytes != NULL;
        case SOL_INTERPRETER_VALUE_RECORD:
        case SOL_INTERPRETER_VALUE_ENUM:
            if ((value->as.aggregate.field_count == 0)
                != (value->as.aggregate.fields == NULL)) return false;
            for (size_t index = 0; index < value->as.aggregate.field_count; ++index) {
                if (!value_shape_valid_recursive(&value->as.aggregate.fields[index],
                    ancestors, depth + 1)) return false;
            }
            return true;
        case SOL_INTERPRETER_VALUE_OPTION:
            if (value->as.sum.is_error
                || value->as.sum.has_value != (value->as.sum.value != NULL)) return false;
            return value->as.sum.value == NULL || value_shape_valid_recursive(
                value->as.sum.value, ancestors, depth + 1);
        case SOL_INTERPRETER_VALUE_RESULT:
            return value->as.sum.has_value && value->as.sum.value != NULL
                && value_shape_valid_recursive(value->as.sum.value,
                    ancestors, depth + 1);
        case SOL_INTERPRETER_VALUE_DISTINCT:
            return value->as.distinct.value != NULL
                && value_shape_valid_recursive(value->as.distinct.value,
                    ancestors, depth + 1);
        case SOL_INTERPRETER_VALUE_CAPABILITY:
            return value->as.capability.source == NULL
                || value_shape_valid_recursive(value->as.capability.source,
                    ancestors, depth + 1);
        case SOL_INTERPRETER_VALUE_FUNCTION:
            return value->as.callable.receiver == NULL;
        case SOL_INTERPRETER_VALUE_BOUND_OPERATION:
            return value->as.callable.receiver != NULL
                && value_shape_valid_recursive(value->as.callable.receiver,
                    ancestors, depth + 1);
        default: return true;
    }
}

static bool value_shape_valid(const SolInterpreterValue *value) {
    const SolInterpreterValue *ancestors[256];
    return value_shape_valid_recursive(value, ancestors, 0);
}

void sol_interpreter_value_init(SolInterpreterValue *value) {
    if (value != NULL) value_clear(value);
}

typedef struct {
    SolInterpreterValue **items;
    size_t count;
    size_t capacity;
} ValuePointers;

static bool value_pointer_add(ValuePointers *pointers, SolInterpreterValue *value) {
    for (size_t index = 0; index < pointers->count; ++index) {
        if (pointers->items[index] == value) return false;
    }
    if (pointers->count == pointers->capacity) {
        size_t capacity = pointers->capacity == 0 ? 16 : pointers->capacity * 2;
        if (capacity < pointers->capacity
            || capacity > SIZE_MAX / sizeof(*pointers->items)) return false;
        SolInterpreterValue **items = realloc(pointers->items,
            capacity * sizeof(*items));
        if (items == NULL) return false;
        pointers->items = items;
        pointers->capacity = capacity;
    }
    pointers->items[pointers->count++] = value;
    return true;
}

static void value_free_recursive(SolInterpreterValue *value, ValuePointers *pointers) {
    if (value == NULL || !value_pointer_add(pointers, value)) return;
    switch (value->kind) {
        case SOL_INTERPRETER_VALUE_TEXT:
            free(value->as.text.bytes);
            break;
        case SOL_INTERPRETER_VALUE_RECORD:
        case SOL_INTERPRETER_VALUE_ENUM: {
            size_t before = pointers->count;
            if (value->as.aggregate.fields != NULL) {
                for (size_t index = 0; index < value->as.aggregate.field_count; ++index) {
                    value_free_recursive(&value->as.aggregate.fields[index], pointers);
                }
            }
            if (pointers->count != before) free(value->as.aggregate.fields);
            break;
        }
        case SOL_INTERPRETER_VALUE_OPTION:
        case SOL_INTERPRETER_VALUE_RESULT:
            if (value->as.sum.value != NULL) {
                SolInterpreterValue *child = value->as.sum.value;
                size_t before = pointers->count;
                value_free_recursive(child, pointers);
                if (pointers->count != before) free(child);
            }
            break;
        case SOL_INTERPRETER_VALUE_DISTINCT:
            if (value->as.distinct.value != NULL) {
                SolInterpreterValue *child = value->as.distinct.value;
                size_t before = pointers->count;
                value_free_recursive(child, pointers);
                if (pointers->count != before) free(child);
            }
            break;
        case SOL_INTERPRETER_VALUE_CAPABILITY:
            if (value->as.capability.source != NULL) {
                SolInterpreterValue *child = value->as.capability.source;
                size_t before = pointers->count;
                value_free_recursive(child, pointers);
                if (pointers->count != before) free(child);
            }
            break;
        case SOL_INTERPRETER_VALUE_BOUND_OPERATION:
            if (value->as.callable.receiver != NULL) {
                SolInterpreterValue *child = value->as.callable.receiver;
                size_t before = pointers->count;
                value_free_recursive(child, pointers);
                if (pointers->count != before) free(child);
            }
            break;
        default: break;
    }
    value_clear(value);
}

void sol_interpreter_value_free(SolInterpreterValue *value) {
    ValuePointers pointers = {0};
    value_free_recursive(value, &pointers);
    free(pointers.items);
}

static bool clone_pointer(SolInterpreterValue **destination,
    const SolInterpreterValue *source) {
    *destination = malloc(sizeof(**destination));
    if (*destination == NULL) return false;
    sol_interpreter_value_init(*destination);
    if (!sol_interpreter_value_clone(*destination, source)) {
        free(*destination);
        *destination = NULL;
        return false;
    }
    return true;
}

bool sol_interpreter_value_clone(SolInterpreterValue *destination,
    const SolInterpreterValue *source) {
    if (destination == NULL || source == NULL || destination == source
        || !value_shape_valid(source)) return false;
    sol_interpreter_value_init(destination);
    destination->kind = source->kind;
    switch (source->kind) {
        case SOL_INTERPRETER_VALUE_INT64:
            destination->as.integer = source->as.integer;
            return true;
        case SOL_INTERPRETER_VALUE_BOOL:
            destination->as.boolean = source->as.boolean;
            return true;
        case SOL_INTERPRETER_VALUE_TEXT:
            return sol_interpreter_value_text(destination, source->as.text.bytes,
                source->as.text.length);
        case SOL_INTERPRETER_VALUE_UNIT: return true;
        case SOL_INTERPRETER_VALUE_RECORD:
        case SOL_INTERPRETER_VALUE_ENUM:
            destination->as.aggregate = source->as.aggregate;
            destination->as.aggregate.fields = NULL;
            if (source->as.aggregate.field_count != 0) {
                destination->as.aggregate.fields = calloc(source->as.aggregate.field_count,
                    sizeof(*destination->as.aggregate.fields));
                if (destination->as.aggregate.fields == NULL) goto fail;
                for (size_t index = 0; index < source->as.aggregate.field_count; ++index) {
                    if (!sol_interpreter_value_clone(
                        &destination->as.aggregate.fields[index],
                        &source->as.aggregate.fields[index])) goto fail;
                }
            }
            return true;
        case SOL_INTERPRETER_VALUE_OPTION:
        case SOL_INTERPRETER_VALUE_RESULT:
            destination->as.sum = source->as.sum;
            destination->as.sum.value = NULL;
            if (source->as.sum.value != NULL
                && !clone_pointer(&destination->as.sum.value, source->as.sum.value)) goto fail;
            return true;
        case SOL_INTERPRETER_VALUE_DISTINCT:
            destination->as.distinct.definition = source->as.distinct.definition;
            return source->as.distinct.value != NULL
                && clone_pointer(&destination->as.distinct.value, source->as.distinct.value);
        case SOL_INTERPRETER_VALUE_CAPABILITY:
            destination->as.capability = source->as.capability;
            destination->as.capability.source = NULL;
            if (source->as.capability.source != NULL
                && !clone_pointer(&destination->as.capability.source,
                    source->as.capability.source)) goto fail;
            return true;
        case SOL_INTERPRETER_VALUE_FUNCTION:
            destination->as.callable.callable = source->as.callable.callable;
            destination->as.callable.receiver = NULL;
            return true;
        case SOL_INTERPRETER_VALUE_BOUND_OPERATION:
            destination->as.callable.callable = source->as.callable.callable;
            destination->as.callable.receiver = NULL;
            return source->as.callable.receiver != NULL
                && clone_pointer(&destination->as.callable.receiver,
                    source->as.callable.receiver);
        case SOL_INTERPRETER_VALUE_INVALID: return true;
    }
fail:
    sol_interpreter_value_free(destination);
    return false;
}

bool sol_interpreter_value_int64(SolInterpreterValue *value, int64_t integer) {
    if (value == NULL) return false;
    sol_interpreter_value_init(value);
    value->kind = SOL_INTERPRETER_VALUE_INT64;
    value->as.integer = integer;
    return true;
}

bool sol_interpreter_value_bool(SolInterpreterValue *value, bool boolean) {
    if (value == NULL) return false;
    sol_interpreter_value_init(value);
    value->kind = SOL_INTERPRETER_VALUE_BOOL;
    value->as.boolean = boolean;
    return true;
}

bool sol_interpreter_value_text(SolInterpreterValue *value,
    const char *bytes, size_t length) {
    if (value == NULL || (length != 0 && bytes == NULL) || length == SIZE_MAX) return false;
    char *copy = malloc(length + 1);
    if (copy == NULL) return false;
    if (length != 0) memcpy(copy, bytes, length);
    copy[length] = '\0';
    sol_interpreter_value_init(value);
    value->kind = SOL_INTERPRETER_VALUE_TEXT;
    value->as.text.bytes = copy;
    value->as.text.length = length;
    return true;
}

bool sol_interpreter_value_unit(SolInterpreterValue *value) {
    if (value == NULL) return false;
    sol_interpreter_value_init(value);
    value->kind = SOL_INTERPRETER_VALUE_UNIT;
    return true;
}

bool sol_interpreter_value_option(SolInterpreterValue *value,
    const SolInterpreterValue *payload) {
    if (value == NULL) return false;
    sol_interpreter_value_init(value);
    value->kind = SOL_INTERPRETER_VALUE_OPTION;
    value->as.sum.has_value = payload != NULL;
    if (payload != NULL && !clone_pointer(&value->as.sum.value, payload)) {
        sol_interpreter_value_free(value);
        return false;
    }
    return true;
}

bool sol_interpreter_value_result(SolInterpreterValue *value, bool is_error,
    const SolInterpreterValue *payload) {
    if (value == NULL || payload == NULL) return false;
    sol_interpreter_value_init(value);
    value->kind = SOL_INTERPRETER_VALUE_RESULT;
    value->as.sum.has_value = true;
    value->as.sum.is_error = is_error;
    if (!clone_pointer(&value->as.sum.value, payload)) {
        sol_interpreter_value_free(value);
        return false;
    }
    return true;
}

bool sol_interpreter_value_capability(SolInterpreterValue *value,
    SolIrDefinitionId definition, void *root,
    const SolInterpreterValue *private_source) {
    if (value == NULL || root == NULL) return false;
    sol_interpreter_value_init(value);
    value->kind = SOL_INTERPRETER_VALUE_CAPABILITY;
    value->as.capability.definition = definition;
    value->as.capability.root = root;
    if (private_source != NULL
        && !clone_pointer(&value->as.capability.source, private_source)) {
        sol_interpreter_value_free(value);
        return false;
    }
    return true;
}

void sol_interpreter_result_init(SolInterpreterResult *result) {
    if (result != NULL) memset(result, 0, sizeof(*result));
}

void sol_interpreter_result_free(SolInterpreterResult *result) {
    if (result == NULL) return;
    sol_interpreter_value_free(&result->value);
    memset(result, 0, sizeof(*result));
}

static Flow flow_new(FlowKind kind) {
    Flow flow;
    flow.kind = kind;
    sol_interpreter_value_init(&flow.value);
    return flow;
}

static void diagnostic(Interpreter *interpreter, SolInterpreterCode code,
    SolSpan span, const char *format, ...) {
    SolInterpreterDiagnostic *output = &interpreter->result->diagnostic;
    if (output->code != SOL_INTERPRETER_OK) return;
    output->code = code;
    output->span = span;
    const char *file = interpreter->request->ir != NULL
        ? interpreter->request->ir->source_path : NULL;
    output->file_offset = span.start;
    if (interpreter->request->ir != NULL) {
        const SolIr *ir = interpreter->request->ir;
        for (size_t index = 0; index < ir->file_count; ++index) {
            if (span.start >= ir->files[index].aggregate_start
                && span.start <= ir->files[index].aggregate_end) {
                file = ir->files[index].path;
                output->file_offset = span.start - ir->files[index].aggregate_start;
                break;
            }
        }
    }
    if (file != NULL) {
        (void)snprintf(output->file, sizeof(output->file), "%s", file);
    }
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(output->message, sizeof(output->message), format, arguments);
    va_end(arguments);
}

static bool consume(Interpreter *interpreter, size_t *used, size_t limit,
    SolInterpreterCode code, SolSpan span, const char *name) {
    if (*used == SIZE_MAX || *used >= limit) {
        diagnostic(interpreter, code, span, "%s limit exceeded", name);
        return false;
    }
    ++*used;
    return true;
}

static bool new_value(Interpreter *interpreter, SolSpan span) {
    return consume(interpreter, &interpreter->result->used.value_nodes,
        interpreter->request->limits.value_nodes, SOL_INTERPRETER_VALUE_LIMIT,
        span, "value node");
}

static bool value_metrics(const SolInterpreterValue *value,
    size_t *nodes, size_t *text_bytes) {
    if (*nodes == SIZE_MAX) return false;
    ++*nodes;
    switch (value->kind) {
        case SOL_INTERPRETER_VALUE_TEXT:
            if (value->as.text.length > SIZE_MAX - *text_bytes) return false;
            *text_bytes += value->as.text.length;
            break;
        case SOL_INTERPRETER_VALUE_RECORD:
        case SOL_INTERPRETER_VALUE_ENUM:
            for (size_t index = 0; index < value->as.aggregate.field_count; ++index) {
                if (!value_metrics(&value->as.aggregate.fields[index],
                    nodes, text_bytes)) return false;
            }
            break;
        case SOL_INTERPRETER_VALUE_OPTION:
        case SOL_INTERPRETER_VALUE_RESULT:
            if (value->as.sum.value != NULL
                && !value_metrics(value->as.sum.value, nodes, text_bytes)) return false;
            break;
        case SOL_INTERPRETER_VALUE_DISTINCT:
            if (value->as.distinct.value != NULL
                && !value_metrics(value->as.distinct.value, nodes, text_bytes)) return false;
            break;
        case SOL_INTERPRETER_VALUE_CAPABILITY:
            if (value->as.capability.source != NULL
                && !value_metrics(value->as.capability.source, nodes, text_bytes)) return false;
            break;
        case SOL_INTERPRETER_VALUE_BOUND_OPERATION:
            if (value->as.callable.receiver != NULL
                && !value_metrics(value->as.callable.receiver, nodes, text_bytes)) return false;
            break;
        default: break;
    }
    return true;
}

static bool reserve_clone(Interpreter *interpreter, SolSpan span,
    const SolInterpreterValue *value) {
    size_t nodes = 0;
    size_t bytes = 0;
    if (!value_shape_valid(value)) {
        diagnostic(interpreter, SOL_INTERPRETER_TYPE_INVARIANT, span,
            "runtime value shape is malformed or cyclic");
        return false;
    }
    if (!value_metrics(value, &nodes, &bytes)
        || nodes > interpreter->request->limits.value_nodes
            - interpreter->result->used.value_nodes) {
        diagnostic(interpreter, SOL_INTERPRETER_VALUE_LIMIT, span,
            "value node limit exceeded");
        return false;
    }
    if (bytes > interpreter->request->limits.text_bytes
            - interpreter->result->used.text_bytes) {
        diagnostic(interpreter, SOL_INTERPRETER_TEXT_LIMIT, span,
            "text byte limit exceeded");
        return false;
    }
    interpreter->result->used.value_nodes += nodes;
    interpreter->result->used.text_bytes += bytes;
    return true;
}

static bool clone_runtime(Interpreter *interpreter, SolSpan span,
    SolInterpreterValue *destination, const SolInterpreterValue *source) {
    if (!reserve_clone(interpreter, span, source)) return false;
    if (!sol_interpreter_value_clone(destination, source)) {
        diagnostic(interpreter, SOL_INTERPRETER_INTERNAL_INVARIANT, span,
            "runtime value allocation failed");
        return false;
    }
    return true;
}

static SolInterpreterValue *lookup_local(Interpreter *interpreter,
    SolIrLocalId local, Frame **owner) {
    for (Frame *frame = interpreter->frame; frame != NULL; frame = frame->parent) {
        if (local < interpreter->request->ir->local_count && frame->bound[local]) {
            if (owner != NULL) *owner = frame;
            return &frame->locals[local];
        }
    }
    return NULL;
}

static bool bind_local(Interpreter *interpreter, Frame *frame,
    SolIrLocalId local, const SolInterpreterValue *value, SolSpan span) {
    if (local >= interpreter->request->ir->local_count || frame->registered[local]) {
        diagnostic(interpreter, SOL_INTERPRETER_INTERNAL_INVARIANT, span,
            "invalid or duplicate local binding");
        return false;
    }
    if (!clone_runtime(interpreter, span, &frame->locals[local], value)) return false;
    frame->bound[local] = true;
    frame->registered[local] = true;
    frame->binding_order[frame->binding_count++] = local;
    if (value->kind == SOL_INTERPRETER_VALUE_CAPABILITY) {
        frame->authority_roots[local] = value->as.capability.root;
        frame->authority_known[local] = true;
    }
    return true;
}

static void cleanup_local(Interpreter *interpreter, Frame *frame,
    SolIrLocalId local) {
    if (local >= interpreter->request->ir->local_count || !frame->bound[local]) return;
    const SolIrLocal *metadata = &interpreter->request->ir->locals[local];
    if (metadata->access == SOL_ACCESS_OWNED) {
        size_t ordinal = interpreter->result->cleanup_actions++;
        if (interpreter->request->cleanup_observer != NULL) {
            interpreter->request->cleanup_observer(
                interpreter->request->cleanup_context, local, ordinal);
        }
    }
    sol_interpreter_value_free(&frame->locals[local]);
    frame->bound[local] = false;
    frame->authority_roots[local] = NULL;
    frame->authority_known[local] = false;
}

static void cleanup_slice(Interpreter *interpreter, SolIrSlice cleanup) {
    while (cleanup.count != 0) {
        --cleanup.count;
        cleanup_local(interpreter, interpreter->frame,
            interpreter->request->ir->cleanup_locals[
                cleanup.offset + cleanup.count]);
    }
}

static bool update_local(Interpreter *interpreter, SolIrLocalId local,
    SolInterpreterValue *value, SolSpan span) {
    Frame *frame = interpreter->frame;
    if (frame == NULL || local >= interpreter->request->ir->local_count
        || !frame->registered[local]) {
        diagnostic(interpreter, SOL_INTERPRETER_INTERNAL_INVARIANT, span,
            "assignment local is unavailable in the current frame");
        return false;
    }
    if (frame->bound[local]) cleanup_local(interpreter, frame, local);
    frame->locals[local] = *value;
    sol_interpreter_value_init(value);
    frame->bound[local] = true;
    frame->authority_roots[local] = NULL;
    frame->authority_known[local] = false;
    if (frame->locals[local].kind == SOL_INTERPRETER_VALUE_CAPABILITY) {
        frame->authority_roots[local] = frame->locals[local].as.capability.root;
        frame->authority_known[local] = true;
    }
    return true;
}

static SolInterpreterValue *place_field(Interpreter *interpreter,
    SolInterpreterValue *base, const SolIrProjection *projection) {
    const SolIr *ir = interpreter->request->ir;
    if (base == NULL || base->kind != SOL_INTERPRETER_VALUE_RECORD
        || base->as.aggregate.definition >= ir->definition_count) return NULL;
    SolIrSlice fields = ir->definitions[base->as.aggregate.definition].fields;
    if (projection->field < fields.offset
        || projection->field - fields.offset >= fields.count) return NULL;
    size_t ordinal = projection->field - fields.offset;
    if (ordinal >= base->as.aggregate.field_count) return NULL;
    return &base->as.aggregate.fields[ordinal];
}

static SolInterpreterValue *local_place_slot(Interpreter *interpreter,
    const SolIrPlace *place, Frame **owner) {
    SolInterpreterValue *slot = lookup_local(interpreter, place->local, owner);
    const SolIr *ir = interpreter->request->ir;
    for (size_t index = 0; slot != NULL && index < place->projections.count; ++index) {
        if (slot->kind == SOL_INTERPRETER_VALUE_INVALID) return NULL;
        slot = place_field(interpreter, slot,
            &ir->projections[place->projections.offset + index]);
    }
    return slot != NULL && slot->kind != SOL_INTERPRETER_VALUE_INVALID ? slot : NULL;
}

static const SolIrPlace *runtime_expression_place(const SolIr *ir,
    SolIrExpressionId expression) {
    if (expression >= ir->expression_count
        || ir->expressions[expression].kind != SOL_IR_EXPR_PLACE
        || ir->expressions[expression].as.place >= ir->place_count) return NULL;
    return &ir->places[ir->expressions[expression].as.place];
}

static bool update_place(Interpreter *interpreter, const SolIrPlace *place,
    SolInterpreterValue *value, SolSpan span) {
    if (place->projections.count == 0) {
        return update_local(interpreter, place->local, value, span);
    }
    Frame *owner = NULL;
    SolInterpreterValue *root = lookup_local(interpreter, place->local, &owner);
    SolInterpreterValue *slot = root;
    const SolIr *ir = interpreter->request->ir;
    for (size_t index = 0; slot != NULL && index < place->projections.count; ++index) {
        if (slot->kind == SOL_INTERPRETER_VALUE_INVALID) slot = NULL;
        else slot = place_field(interpreter, slot,
            &ir->projections[place->projections.offset + index]);
    }
    if (owner == NULL || slot == NULL) {
        diagnostic(interpreter, SOL_INTERPRETER_INTERNAL_INVARIANT, span,
            "assignment target has a missing strict ancestor");
        return false;
    }
    if (slot->kind != SOL_INTERPRETER_VALUE_INVALID) {
        const SolIrLocal *metadata = &ir->locals[place->local];
        if (metadata->access == SOL_ACCESS_OWNED) {
            size_t ordinal = interpreter->result->cleanup_actions++;
            if (interpreter->request->cleanup_observer != NULL) {
                interpreter->request->cleanup_observer(
                    interpreter->request->cleanup_context, place->local, ordinal);
            }
        }
        sol_interpreter_value_free(slot);
    }
    *slot = *value;
    sol_interpreter_value_init(value);
    return true;
}

static bool charge_place_update(Interpreter *interpreter, const SolIrPlace *place,
    SolSpan fallback_span) {
    if (place == NULL) return false;
    const SolIr *ir = interpreter->request->ir;
    for (size_t index = 0; index < place->projections.count; ++index) {
        SolSpan span = ir->projections[place->projections.offset + index].span;
        if (!consume(interpreter, &interpreter->result->used.steps,
            interpreter->request->limits.steps, SOL_INTERPRETER_STEP_LIMIT,
            span.end <= span.start ? fallback_span : span, "step")) return false;
    }
    return true;
}

static bool value_equal(const SolInterpreterValue *left,
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
        case SOL_INTERPRETER_VALUE_RECORD:
        case SOL_INTERPRETER_VALUE_ENUM:
            if (left->as.aggregate.definition != right->as.aggregate.definition
                || left->as.aggregate.variant != right->as.aggregate.variant
                || left->as.aggregate.field_count != right->as.aggregate.field_count) return false;
            for (size_t index = 0; index < left->as.aggregate.field_count; ++index) {
                if (!value_equal(&left->as.aggregate.fields[index],
                    &right->as.aggregate.fields[index], supported) || !*supported) return false;
            }
            return true;
        case SOL_INTERPRETER_VALUE_OPTION:
        case SOL_INTERPRETER_VALUE_RESULT:
            if (left->as.sum.has_value != right->as.sum.has_value
                || left->as.sum.is_error != right->as.sum.is_error) return false;
            return !left->as.sum.has_value || value_equal(left->as.sum.value,
                right->as.sum.value, supported);
        case SOL_INTERPRETER_VALUE_DISTINCT:
            return left->as.distinct.definition == right->as.distinct.definition
                && value_equal(left->as.distinct.value, right->as.distinct.value, supported);
        default:
            *supported = false;
            return false;
    }
}

static SolIrTypeId substituted_type(Interpreter *interpreter,
    SolIrTypeId type_id) {
    const SolIr *ir = interpreter->request->ir;
    for (size_t depth = 0; depth < 256 && type_id < ir->type_count; ++depth) {
        const SolIrType *type = &ir->types[type_id];
        if (type->kind != SOL_IR_TYPE_PARAMETER) return type_id;
        bool found = false;
        for (Frame *frame = interpreter->frame; frame != NULL; frame = frame->parent) {
            if (type->definition >= frame->type_parameter_offset
                && type->definition - frame->type_parameter_offset
                    < frame->type_argument_count) {
                type_id = frame->type_arguments[
                    type->definition - frame->type_parameter_offset
                ];
                found = true;
                break;
            }
        }
        if (!found) return SOL_IR_NONE;
    }
    return SOL_IR_NONE;
}

static bool capability_value_valid(
    const SolIr *ir, const SolInterpreterValue *value, size_t depth
) {
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
    if (definition->capability_source >= ir->local_count) return false;
    SolIrTypeId source_type = ir->locals[definition->capability_source].type;
    if (source_type >= ir->type_count
        || ir->types[source_type].kind != SOL_IR_TYPE_NOMINAL
        || value->as.capability.source == NULL
        || value->as.capability.source->kind != SOL_INTERPRETER_VALUE_CAPABILITY
        || value->as.capability.source->as.capability.definition
            != ir->types[source_type].definition
        || value->as.capability.source->as.capability.root
            != value->as.capability.root) return false;
    return capability_value_valid(ir, value->as.capability.source, depth + 1);
}

static bool bound_operation_value_valid(
    const SolIr *ir, const SolInterpreterValue *value
) {
    if (value == NULL || value->kind != SOL_INTERPRETER_VALUE_BOUND_OPERATION
        || value->as.callable.callable >= ir->callable_count
        || value->as.callable.receiver == NULL
        || !capability_value_valid(ir, value->as.callable.receiver, 0)) return false;
    const SolIrCallable *callable = &ir->callables[value->as.callable.callable];
    return callable->kind == SOL_IR_CALLABLE_CAPABILITY
        && callable->owner
            == value->as.callable.receiver->as.capability.definition;
}

static bool value_matches_type(Interpreter *interpreter,
    const SolInterpreterValue *value, SolIrTypeId type_id,
    const SolInterpreterValue *self, size_t depth) {
    const SolIr *ir = interpreter->request->ir;
    if (type_id >= ir->type_count || depth >= 256) return false;
    const SolIrType *type = &ir->types[type_id];
    if (type->kind == SOL_IR_TYPE_PARAMETER) {
        for (Frame *frame = interpreter->frame; frame != NULL; frame = frame->parent) {
            if (type->definition >= frame->type_parameter_offset
                && type->definition - frame->type_parameter_offset
                    < frame->type_argument_count) {
                return value_matches_type(interpreter, value,
                    frame->type_arguments[type->definition
                        - frame->type_parameter_offset], self, depth + 1);
            }
        }
        return false;
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
        if (value->kind == SOL_INTERPRETER_VALUE_CAPABILITY) {
            return value->as.capability.definition == self->as.capability.definition;
        }
        return value->kind != SOL_INTERPRETER_VALUE_INVALID;
    }
    switch (type->kind) {
        case SOL_IR_TYPE_INT64:
            return value->kind == SOL_INTERPRETER_VALUE_INT64;
        case SOL_IR_TYPE_BOOL:
            return value->kind == SOL_INTERPRETER_VALUE_BOOL;
        case SOL_IR_TYPE_TEXT:
            return value->kind == SOL_INTERPRETER_VALUE_TEXT;
        case SOL_IR_TYPE_UNIT:
            return value->kind == SOL_INTERPRETER_VALUE_UNIT;
        case SOL_IR_TYPE_NEVER:
            return false;
        case SOL_IR_TYPE_FUNCTION: {
            if (value->kind != SOL_INTERPRETER_VALUE_FUNCTION
                && value->kind != SOL_INTERPRETER_VALUE_BOUND_OPERATION) return false;
            SolIrCallableId callable_id = value->as.callable.callable;
            if (callable_id >= ir->callable_count) return false;
            const SolIrCallable *callable = &ir->callables[callable_id];
            if (value->kind == SOL_INTERPRETER_VALUE_FUNCTION
                && callable->kind != SOL_IR_CALLABLE_FUNCTION) return false;
            if (value->kind == SOL_INTERPRETER_VALUE_BOUND_OPERATION
                && !bound_operation_value_valid(ir, value)) return false;
            if (callable->generic_parameters.count != 0
                || callable->effect_parameters.count != 0) return false;
            if (type->definition != SOL_IR_NONE) {
                return callable->owner == type->definition;
            }
            if (callable->parameters.count != type->parameter_count
                || callable->result != substituted_type(interpreter, type->result)
                || callable->effects.count != type->effects.count) return false;
            for (size_t index = 0; index < type->parameter_count; ++index) {
                SolIrLocalId local = ir->roots[callable->parameters.offset + index];
                if (local >= ir->local_count
                    || ir->locals[local].access
                        != ir->accesses[type->parameter_access_offset + index]
                    || ir->locals[local].type != substituted_type(interpreter,
                        ir->type_ids[type->parameter_offset + index])) return false;
            }
            for (size_t index = 0; index < type->effects.count; ++index) {
                const SolIrEffect *left = &ir->effects[callable->effects.offset + index];
                const SolIrEffect *right = &ir->effects[type->effects.offset + index];
                if (left->authority_kind != right->authority_kind
                    || left->authority != right->authority
                    || strcmp(left->name, right->name) != 0) return false;
            }
            return true;
        }
        case SOL_IR_TYPE_OPTION:
            return value->kind == SOL_INTERPRETER_VALUE_OPTION
                && (!value->as.sum.has_value
                    || (value->as.sum.value != NULL && type->argument_count == 1
                        && value_matches_type(interpreter, value->as.sum.value,
                            ir->type_ids[type->argument_offset], self, depth + 1)));
        case SOL_IR_TYPE_RESULT:
            if (value->kind != SOL_INTERPRETER_VALUE_RESULT
                || !value->as.sum.has_value || value->as.sum.value == NULL
                || type->argument_count != 2) return false;
            return value_matches_type(interpreter, value->as.sum.value,
                ir->type_ids[type->argument_offset + (value->as.sum.is_error ? 1u : 0u)],
                self, depth + 1);
        case SOL_IR_TYPE_NOMINAL: {
            if (type->definition >= ir->definition_count) return false;
            const SolIrDefinition *definition = &ir->definitions[type->definition];
            if (definition->generic_parameters.count != type->argument_count) return false;
            Frame substitution;
            memset(&substitution, 0, sizeof(substitution));
            substitution.parent = interpreter->frame;
            substitution.type_parameter_offset = definition->generic_parameters.offset;
            substitution.type_argument_count = type->argument_count;
            substitution.type_arguments = type->argument_count == 0 ? NULL
                : ir->type_ids + type->argument_offset;
            interpreter->frame = &substitution;
            bool matches = false;
            if (definition->kind == SOL_IR_DEFINITION_CAPABILITY) {
                matches = value->kind == SOL_INTERPRETER_VALUE_CAPABILITY
                    && value->as.capability.definition == type->definition
                    && capability_value_valid(ir, value, 0);
            } else if (definition->kind == SOL_IR_DEFINITION_RECORD) {
                matches = value->kind == SOL_INTERPRETER_VALUE_RECORD
                    && value->as.aggregate.definition == type->definition
                    && value->as.aggregate.field_count == definition->fields.count;
                for (size_t index = 0; matches && index < definition->fields.count; ++index) {
                    matches = value_matches_type(interpreter,
                        &value->as.aggregate.fields[index],
                        ir->fields[definition->fields.offset + index].type,
                        self, depth + 1);
                }
            } else if (definition->kind == SOL_IR_DEFINITION_ENUM) {
                matches = value->kind == SOL_INTERPRETER_VALUE_ENUM
                    && value->as.aggregate.definition == type->definition
                    && value->as.aggregate.variant < ir->variant_count;
                SolIrSlice fields = matches
                    ? ir->variants[value->as.aggregate.variant].fields : (SolIrSlice){0};
                matches = matches
                    && ir->variants[value->as.aggregate.variant].owner == type->definition
                    && fields.count == value->as.aggregate.field_count;
                for (size_t index = 0; matches && index < fields.count; ++index) {
                    matches = value_matches_type(interpreter,
                        &value->as.aggregate.fields[index],
                        ir->fields[fields.offset + index].type, self, depth + 1);
                }
            } else if (definition->kind == SOL_IR_DEFINITION_DISTINCT
                || definition->kind == SOL_IR_DEFINITION_REFINED) {
                matches = value->kind == SOL_INTERPRETER_VALUE_DISTINCT
                    && value->as.distinct.definition == type->definition
                    && value->as.distinct.value != NULL
                    && value_matches_type(interpreter, value->as.distinct.value,
                        definition->representation, self, depth + 1);
            }
            interpreter->frame = substitution.parent;
            return matches;
        }
        default: return false;
    }
}

static Flow evaluate(Interpreter *interpreter, SolIrExpressionId expression_id);

static Flow invoke(Interpreter *interpreter, SolIrCallableId callable,
    const SolInterpreterValue *receiver, const SolInterpreterValue *arguments,
    size_t argument_count, const SolIrTypeId *type_arguments,
    size_t type_argument_count, const SolIrDispatchEvidence *evidence,
    size_t evidence_count, SolInterpreterValue *receiver_writeback,
    SolInterpreterValue *argument_writebacks, SolSpan span);
static bool validate_authority(Interpreter *interpreter,
    const SolIrCallable *callable, const SolInterpreterValue *receiver,
    const SolInterpreterValue *result, SolSpan span);

static void free_values(SolInterpreterValue *values, size_t count) {
    if (values == NULL) return;
    for (size_t index = 0; index < count; ++index) sol_interpreter_value_free(&values[index]);
    free(values);
}

static Flow invoke_operation(Interpreter *interpreter,
    const SolInterpreterValue *receiver, SolIrCallableId callable,
    const SolInterpreterValue *arguments, size_t argument_count,
    SolInterpreterValue *argument_writebacks, SolSpan span) {
    Flow error = flow_new(FLOW_ERROR);
    const SolIr *ir = interpreter->request->ir;
    if (callable >= ir->callable_count || !capability_value_valid(ir, receiver, 0)
        || ir->callables[callable].kind != SOL_IR_CALLABLE_CAPABILITY
        || ir->callables[callable].owner != receiver->as.capability.definition) {
        diagnostic(interpreter, SOL_INTERPRETER_TYPE_INVARIANT, span,
            "capability operation receiver and callable domains do not match");
        return error;
    }
    const SolIrCallable *operation = &ir->callables[callable];
    for (Handler *handler = interpreter->handler; handler != NULL; handler = handler->parent) {
        bool effect = false;
        for (size_t index = 0; index < operation->effects.count; ++index) {
            const SolIrEffect *atom = &interpreter->request->ir->effects[
                operation->effects.offset + index
            ];
            if (strcmp(atom->name, handler->effect_name) == 0) effect = true;
        }
        if (handler->source == callable && handler->root == receiver->as.capability.root
            && effect) {
            Handler *saved = interpreter->handler;
            interpreter->handler = handler->parent;
            Flow result = invoke_operation(interpreter, &handler->provider,
                handler->provider_callable, arguments, argument_count,
                argument_writebacks, span);
            interpreter->handler = saved;
            return result;
        }
    }
    if (operation->body != SOL_IR_NONE) {
        return invoke(interpreter, callable, receiver, arguments, argument_count,
            NULL, 0, NULL, 0, NULL, argument_writebacks, span);
    }
    for (size_t index = 0; index < operation->parameters.count; ++index) {
        SolIrLocalId local = ir->roots[operation->parameters.offset + index];
        if (ir->locals[local].access == SOL_ACCESS_EXCLUSIVE) {
            diagnostic(interpreter, SOL_INTERPRETER_UNBOUND_OPERATION, span,
                "bodyless capability operations do not support inout writeback");
            return error;
        }
    }
    if (interpreter->request->host_operation == NULL) {
        diagnostic(interpreter, SOL_INTERPRETER_UNBOUND_OPERATION, span,
            "capability operation '%s' is unbound", operation->name);
        return error;
    }
    if (!consume(interpreter, &interpreter->result->used.host_calls,
        interpreter->request->limits.host_calls, SOL_INTERPRETER_HOST_CALL_LIMIT,
        span, "host call")) return error;
    SolInterpreterValue borrowed;
    sol_interpreter_value_init(&borrowed);
    const char *message = NULL;
    bool ok = interpreter->request->host_operation(interpreter->request->host_context,
        interpreter->request->ir, callable, receiver->as.capability.root,
        receiver->as.capability.source, arguments, argument_count, &borrowed, &message);
    if (!ok) {
        diagnostic(interpreter, SOL_INTERPRETER_HOST_ERROR, span, "%s",
            message == NULL ? "host operation failed" : message);
        return error;
    }
    if (!value_shape_valid(&borrowed)
        || !value_matches_type(interpreter, &borrowed, operation->result, receiver, 0)) {
        diagnostic(interpreter, SOL_INTERPRETER_TYPE_INVARIANT, span,
            "host operation result does not match its IR type");
        return error;
    }
    Flow flow = flow_new(FLOW_VALUE);
    if (!clone_runtime(interpreter, span, &flow.value, &borrowed)) return error;
    if (!validate_authority(interpreter, operation, receiver, &flow.value, span)) {
        sol_interpreter_value_free(&flow.value);
        return error;
    }
    return flow;
}

static const SolIrDispatchEvidence *resolve_parameter_evidence(
    Frame *frame, SolIrGenericParameterId parameter,
    SolIrDefinitionId trait, SolIrCallableId requirement
) {
    if (frame == NULL) return NULL;
    for (size_t index = 0; index < frame->evidence_count; ++index) {
        const SolIrDispatchEvidence *entry = &frame->evidence[index];
        if (entry->binding != parameter || entry->trait != trait
            || entry->requirement != requirement) continue;
        if (!entry->forwarded) return entry;
        return resolve_parameter_evidence(frame->parent, entry->parameter,
            trait, requirement);
    }
    return NULL;
}

static const SolIrDispatchEvidence *resolve_evidence(Interpreter *interpreter,
    const SolIrDispatchEvidence *candidate) {
    if (!candidate->forwarded) return candidate;
    return resolve_parameter_evidence(interpreter->frame, candidate->parameter,
        candidate->trait, candidate->requirement);
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
        && evidence->method < ir->callable_count && evidence->type < ir->type_count
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

static bool validate_invocation_evidence(Interpreter *interpreter,
    const SolIrCallable *callable, const SolIrTypeId *type_arguments,
    size_t type_argument_count, const SolIrDispatchEvidence *evidence,
    size_t evidence_count, SolSpan span) {
    const SolIr *ir = interpreter->request->ir;
    SolInterpreterCode code = interpreter->depth == 0
        ? SOL_INTERPRETER_INVALID_REQUEST : SOL_INTERPRETER_TYPE_INVARIANT;
    if ((type_argument_count != 0 && type_arguments == NULL)
        || (evidence_count != 0 && evidence == NULL)) {
        diagnostic(interpreter, code, span,
            "generic invocation metadata is missing");
        return false;
    }
    for (size_t index = 0; index < type_argument_count; ++index) {
        if (type_arguments[index] >= ir->type_count) {
            diagnostic(interpreter, code, span,
                "generic invocation type argument is out of range");
            return false;
        }
    }
    for (size_t index = 0; index < evidence_count; ++index) {
        const SolIrDispatchEvidence *entry = &evidence[index];
        if (!evidence_domain_valid(ir, entry)
            || entry->binding < callable->generic_parameters.offset
            || entry->binding - callable->generic_parameters.offset
                >= callable->generic_parameters.count
            || ir->generic_parameters[entry->binding].trait_bound != entry->trait) {
            diagnostic(interpreter, code, span,
                "generic invocation evidence domain is invalid");
            return false;
        }
        size_t ordinal = entry->binding - callable->generic_parameters.offset;
        SolIrTypeId argument = type_arguments[ordinal];
        if (entry->forwarded) {
            Frame *caller = interpreter->frame;
            if (caller == NULL
                || entry->parameter < caller->type_parameter_offset
                || entry->parameter - caller->type_parameter_offset
                    >= caller->type_argument_count
                || ir->types[argument].kind != SOL_IR_TYPE_PARAMETER
                || ir->types[argument].definition != entry->parameter) {
                diagnostic(interpreter, code, span,
                    "forwarded invocation evidence has no matching caller parameter");
                return false;
            }
        } else if (entry->type != argument) {
            diagnostic(interpreter, code, span,
                "concrete invocation evidence has the wrong type");
            return false;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (evidence[previous].binding == entry->binding
                && evidence[previous].requirement == entry->requirement) {
                diagnostic(interpreter, code, span,
                    "generic invocation evidence binding is duplicated");
                return false;
            }
        }
    }
    for (size_t ordinal = 0; ordinal < callable->generic_parameters.count; ++ordinal) {
        SolIrGenericParameterId parameter
            = callable->generic_parameters.offset + ordinal;
        SolIrDefinitionId trait = ir->generic_parameters[parameter].trait_bound;
        if (trait == SOL_IR_NONE) continue;
        SolIrSlice requirements = ir->definitions[trait].members;
        for (size_t member = 0; member < requirements.count; ++member) {
            SolIrCallableId requirement
                = ir->members[requirements.offset + member].callable;
            size_t found = 0;
            for (size_t index = 0; index < evidence_count; ++index) {
                if (evidence[index].binding == parameter
                    && evidence[index].requirement == requirement) ++found;
            }
            if (found != 1) {
                diagnostic(interpreter, code, span,
                    "generic invocation evidence binding is incomplete");
                return false;
            }
        }
    }
    return true;
}

static bool validate_authority(Interpreter *interpreter,
    const SolIrCallable *callable, const SolInterpreterValue *receiver,
    const SolInterpreterValue *result, SolSpan span) {
    if (callable->result_authority_kind == SOL_IR_AUTHORITY_NONE) return true;
    if (result->kind != SOL_INTERPRETER_VALUE_CAPABILITY) {
        diagnostic(interpreter, SOL_INTERPRETER_TYPE_INVARIANT, span,
            "authority-returning callable returned a non-capability");
        return false;
    }
    void *authority_root = receiver != NULL
        && receiver->kind == SOL_INTERPRETER_VALUE_CAPABILITY
        ? receiver->as.capability.root : NULL;
    bool authority_known = receiver != NULL
        && receiver->kind == SOL_INTERPRETER_VALUE_CAPABILITY;
    if (callable->result_authority_kind == SOL_IR_AUTHORITY_LOCAL) {
        SolIrLocalId local = callable->result_authority;
        authority_known = interpreter->frame != NULL
            && local < interpreter->request->ir->local_count
            && interpreter->frame->authority_known[local];
        authority_root = authority_known
            ? interpreter->frame->authority_roots[local] : NULL;
    }
    if (!authority_known || authority_root != result->as.capability.root) {
        diagnostic(interpreter, SOL_INTERPRETER_TYPE_INVARIANT, span,
            "returned capability has the wrong authority root");
        return false;
    }
    return true;
}

static Flow invoke(Interpreter *interpreter, SolIrCallableId callable_id,
    const SolInterpreterValue *receiver, const SolInterpreterValue *arguments,
    size_t argument_count, const SolIrTypeId *type_arguments,
    size_t type_argument_count, const SolIrDispatchEvidence *evidence,
    size_t evidence_count, SolInterpreterValue *receiver_writeback,
    SolInterpreterValue *argument_writebacks, SolSpan span) {
    Flow error = flow_new(FLOW_ERROR);
    const SolIr *ir = interpreter->request->ir;
    if (callable_id >= ir->callable_count) {
        diagnostic(interpreter, SOL_INTERPRETER_INVALID_REQUEST, span,
            "callable is out of range");
        return error;
    }
    const SolIrCallable *callable = &ir->callables[callable_id];
    if (callable->parameters.count != argument_count
        || callable->generic_parameters.count != type_argument_count) {
        diagnostic(interpreter, SOL_INTERPRETER_TYPE_INVARIANT, span,
            "call argument count does not match callable metadata");
        return error;
    }
    if (!validate_invocation_evidence(interpreter, callable, type_arguments,
        type_argument_count, evidence, evidence_count, span)) return error;
    if (interpreter->depth >= interpreter->request->limits.call_depth) {
        diagnostic(interpreter, SOL_INTERPRETER_CALL_DEPTH_LIMIT, span,
            "call depth limit exceeded");
        return error;
    }
    Frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.parent = interpreter->frame;
    frame.type_arguments = type_arguments;
    frame.type_argument_count = type_argument_count;
    frame.type_parameter_offset = callable->generic_parameters.offset;
    frame.evidence = evidence;
    frame.evidence_count = evidence_count;
    if (ir->local_count != 0) {
        frame.locals = calloc(ir->local_count, sizeof(*frame.locals));
        frame.bound = calloc(ir->local_count, sizeof(*frame.bound));
        frame.registered = calloc(ir->local_count, sizeof(*frame.registered));
        frame.authority_roots = calloc(ir->local_count,
            sizeof(*frame.authority_roots));
        frame.authority_known = calloc(ir->local_count,
            sizeof(*frame.authority_known));
        frame.binding_order = malloc(ir->local_count * sizeof(*frame.binding_order));
        if (frame.locals == NULL || frame.bound == NULL || frame.registered == NULL
            || frame.authority_roots == NULL || frame.authority_known == NULL
            || frame.binding_order == NULL) {
            free(frame.locals);
            free(frame.bound);
            free(frame.registered);
            free(frame.authority_roots);
            free(frame.authority_known);
            free(frame.binding_order);
            diagnostic(interpreter, SOL_INTERPRETER_INTERNAL_INVARIANT, span,
                "frame allocation failed");
            return error;
        }
    }
    interpreter->frame = &frame;
    ++interpreter->depth;
    if (interpreter->depth > interpreter->result->used.call_depth) {
        interpreter->result->used.call_depth = interpreter->depth;
    }
    bool bound = true;
    if (callable->receiver != SOL_IR_NONE) {
        if (receiver == NULL) {
            diagnostic(interpreter, SOL_INTERPRETER_TYPE_INVARIANT, span,
                "callable receiver is missing");
            bound = false;
        }
    }
    if (bound && callable->capability_source != SOL_IR_NONE) {
        if (!capability_value_valid(ir, receiver, 0)
            || receiver->as.capability.source == NULL) {
            diagnostic(interpreter, SOL_INTERPRETER_TYPE_INVARIANT, span,
                "derived capability source is missing or malformed");
            bound = false;
        }
    }
    for (size_t index = 0; bound && index < argument_count; ++index) {
        SolIrLocalId local = ir->roots[callable->parameters.offset + index];
        if (!value_shape_valid(&arguments[index])
            || !value_matches_type(interpreter, &arguments[index],
                ir->locals[local].type, receiver, 0)) {
            diagnostic(interpreter, interpreter->depth == 1
                ? SOL_INTERPRETER_INVALID_REQUEST : SOL_INTERPRETER_TYPE_INVARIANT,
                span, "call argument does not match its substituted IR type");
            bound = false;
        }
    }
    if (bound && callable->receiver != SOL_IR_NONE) {
        bound = bind_local(interpreter, &frame, callable->receiver, receiver, span);
    }
    if (bound && callable->capability_source != SOL_IR_NONE) {
        bound = bind_local(interpreter, &frame, callable->capability_source,
            receiver->as.capability.source, span);
    }
    for (size_t index = 0; bound && index < argument_count; ++index) {
        SolIrLocalId local = ir->roots[callable->parameters.offset + index];
        bound = bind_local(interpreter, &frame, local, &arguments[index], span);
    }
    Flow result = error;
    if (bound && callable->body != SOL_IR_NONE) result = evaluate(interpreter, callable->body);
    else if (bound) diagnostic(interpreter, SOL_INTERPRETER_UNBOUND_OPERATION, span,
        "bodyless callable '%s' cannot execute directly", callable->name);
    if (result.kind != FLOW_ERROR
        && !value_matches_type(interpreter, &result.value, callable->result,
            receiver, 0)) {
        diagnostic(interpreter, SOL_INTERPRETER_TYPE_INVARIANT, span,
            "callable result does not match its substituted IR type");
        sol_interpreter_value_free(&result.value);
        result.kind = FLOW_ERROR;
    }
    if (result.kind != FLOW_ERROR
        && !validate_authority(interpreter, callable, receiver, &result.value, span)) {
        sol_interpreter_value_free(&result.value);
        result.kind = FLOW_ERROR;
    }
    if (result.kind != FLOW_ERROR && callable->receiver != SOL_IR_NONE
        && callable->receiver_access == SOL_ACCESS_EXCLUSIVE) {
        SolIrLocalId local = callable->receiver;
        if (receiver_writeback == NULL || !frame.bound[local]
            || !value_matches_type(interpreter, &frame.locals[local],
                ir->locals[local].type, receiver, 0)) {
            diagnostic(interpreter, SOL_INTERPRETER_TYPE_INVARIANT, span,
                "exclusive receiver did not remain complete for writeback");
            sol_interpreter_value_free(&result.value);
            result.kind = FLOW_ERROR;
        } else {
            *receiver_writeback = frame.locals[local];
            sol_interpreter_value_init(&frame.locals[local]);
            frame.bound[local] = false;
        }
    }
    for (size_t index = 0; result.kind != FLOW_ERROR && index < argument_count;
        ++index) {
        SolIrLocalId local = ir->roots[callable->parameters.offset + index];
        if (ir->locals[local].access != SOL_ACCESS_EXCLUSIVE) continue;
        if (argument_writebacks == NULL || !frame.bound[local]
            || !value_matches_type(interpreter, &frame.locals[local],
                ir->locals[local].type, receiver, 0)) {
            diagnostic(interpreter, SOL_INTERPRETER_TYPE_INVARIANT, span,
                "exclusive parameter did not remain complete for writeback");
            sol_interpreter_value_free(&result.value);
            result.kind = FLOW_ERROR;
            break;
        }
        argument_writebacks[index] = frame.locals[local];
        sol_interpreter_value_init(&frame.locals[local]);
        frame.bound[local] = false;
    }
    while (frame.binding_count != 0) {
        cleanup_local(interpreter, &frame,
            frame.binding_order[--frame.binding_count]);
    }
    free(frame.locals);
    free(frame.bound);
    free(frame.registered);
    free(frame.authority_roots);
    free(frame.authority_known);
    free(frame.binding_order);
    --interpreter->depth;
    interpreter->frame = frame.parent;
    if (result.kind == FLOW_RETURN) result.kind = FLOW_VALUE;
    return result;
}

static Flow evaluate_binary(Interpreter *interpreter,
    const SolIrExpression *expression) {
    Flow left = evaluate(interpreter, expression->as.binary.left);
    if (left.kind != FLOW_VALUE) return left;
    SolTokenKind operator_kind = expression->as.binary.operator_kind;
    if ((operator_kind == SOL_TOKEN_AMP_AMP || operator_kind == SOL_TOKEN_PIPE_PIPE)
        && left.value.kind == SOL_INTERPRETER_VALUE_BOOL
        && ((operator_kind == SOL_TOKEN_AMP_AMP && !left.value.as.boolean)
            || (operator_kind == SOL_TOKEN_PIPE_PIPE && left.value.as.boolean))) {
        return left;
    }
    Flow right = evaluate(interpreter, expression->as.binary.right);
    if (right.kind != FLOW_VALUE) {
        sol_interpreter_value_free(&left.value);
        return right;
    }
    Flow output = flow_new(FLOW_VALUE);
    bool valid = new_value(interpreter, expression->span);
    if (valid && (operator_kind == SOL_TOKEN_EQUAL_EQUAL
        || operator_kind == SOL_TOKEN_BANG_EQUAL)) {
        bool supported = true;
        bool equal = value_equal(&left.value, &right.value, &supported);
        if (!supported) {
            diagnostic(interpreter, SOL_INTERPRETER_TYPE_INVARIANT, expression->span,
                "runtime-only values do not support structural equality");
            valid = false;
        } else {
            sol_interpreter_value_bool(&output.value,
                operator_kind == SOL_TOKEN_EQUAL_EQUAL ? equal : !equal);
        }
    } else if (valid && left.value.kind == SOL_INTERPRETER_VALUE_BOOL
        && right.value.kind == SOL_INTERPRETER_VALUE_BOOL
        && (operator_kind == SOL_TOKEN_AMP_AMP || operator_kind == SOL_TOKEN_PIPE_PIPE)) {
        sol_interpreter_value_bool(&output.value, operator_kind == SOL_TOKEN_AMP_AMP
            ? left.value.as.boolean && right.value.as.boolean
            : left.value.as.boolean || right.value.as.boolean);
    } else if (valid && left.value.kind == SOL_INTERPRETER_VALUE_INT64
        && right.value.kind == SOL_INTERPRETER_VALUE_INT64) {
        int64_t a = left.value.as.integer;
        int64_t b = right.value.as.integer;
        int64_t value = 0;
        bool boolean = false;
        bool comparison = false;
        switch (operator_kind) {
            case SOL_TOKEN_PLUS:
                valid = !((b > 0 && a > INT64_MAX - b)
                    || (b < 0 && a < INT64_MIN - b));
                if (valid) value = a + b;
                break;
            case SOL_TOKEN_MINUS:
                valid = !((b < 0 && a > INT64_MAX + b)
                    || (b > 0 && a < INT64_MIN + b));
                if (valid) value = a - b;
                break;
            case SOL_TOKEN_STAR:
                if (a == 0 || b == 0) value = 0;
                else if (a == -1) valid = b != INT64_MIN, value = valid ? -b : 0;
                else if (b == -1) valid = a != INT64_MIN, value = valid ? -a : 0;
                else valid = !((a > 0 && b > 0 && a > INT64_MAX / b)
                    || (a > 0 && b < 0 && b < INT64_MIN / a)
                    || (a < 0 && b > 0 && a < INT64_MIN / b)
                    || (a < 0 && b < 0 && a < INT64_MAX / b));
                if (valid && a != 0 && b != 0 && a != -1 && b != -1) value = a * b;
                break;
            case SOL_TOKEN_SLASH:
            case SOL_TOKEN_PERCENT:
                if (b == 0) {
                    diagnostic(interpreter, SOL_INTERPRETER_DIVISION_BY_ZERO,
                        expression->span, "integer division by zero");
                    valid = false;
                } else if (a == INT64_MIN && b == -1) valid = false;
                else value = operator_kind == SOL_TOKEN_SLASH ? a / b : a % b;
                break;
            case SOL_TOKEN_LESS: comparison = true; boolean = a < b; break;
            case SOL_TOKEN_LESS_EQUAL: comparison = true; boolean = a <= b; break;
            case SOL_TOKEN_GREATER: comparison = true; boolean = a > b; break;
            case SOL_TOKEN_GREATER_EQUAL: comparison = true; boolean = a >= b; break;
            default: valid = false; break;
        }
        if (!valid && interpreter->result->diagnostic.code == SOL_INTERPRETER_OK) {
            diagnostic(interpreter, SOL_INTERPRETER_INTEGER_OVERFLOW,
                expression->span, "checked Int64 arithmetic overflow");
        }
        if (valid) {
            if (comparison) sol_interpreter_value_bool(&output.value, boolean);
            else sol_interpreter_value_int64(&output.value, value);
        }
    } else if (valid) {
        diagnostic(interpreter, SOL_INTERPRETER_TYPE_INVARIANT, expression->span,
            "binary operand runtime types are invalid");
        valid = false;
    }
    sol_interpreter_value_free(&left.value);
    sol_interpreter_value_free(&right.value);
    if (!valid) output.kind = FLOW_ERROR;
    return output;
}

static Flow evaluate_call(Interpreter *interpreter,
    const SolIrExpression *expression) {
    const SolIr *ir = interpreter->request->ir;
    Flow callee = flow_new(FLOW_VALUE);
    bool evaluate_callee = expression->as.call.kind == SOL_IR_CALL_CALLBACK
        || expression->as.call.kind == SOL_IR_CALL_CAPABILITY;
    if (evaluate_callee) {
        callee = evaluate(interpreter, expression->as.call.callee);
        if (callee.kind != FLOW_VALUE) return callee;
    }
    Flow receiver = flow_new(FLOW_VALUE);
    if (expression->as.call.kind == SOL_IR_CALL_METHOD) {
        receiver = evaluate(interpreter, expression->as.call.receiver);
        if (receiver.kind != FLOW_VALUE) {
            sol_interpreter_value_free(&callee.value);
            return receiver;
        }
    }
    size_t count = expression->as.call.operands.count;
    SolInterpreterValue *arguments = count == 0 ? NULL : calloc(count, sizeof(*arguments));
    SolInterpreterValue *writebacks = count == 0 ? NULL : calloc(count, sizeof(*writebacks));
    SolInterpreterValue receiver_writeback;
    sol_interpreter_value_init(&receiver_writeback);
    if (count != 0 && (arguments == NULL || writebacks == NULL)) {
        diagnostic(interpreter, SOL_INTERPRETER_INTERNAL_INVARIANT, expression->span,
            "argument allocation failed");
        sol_interpreter_value_free(&callee.value);
        sol_interpreter_value_free(&receiver.value);
        free(arguments);
        free(writebacks);
        return flow_new(FLOW_ERROR);
    }
    for (size_t index = 0; index < count; ++index) {
        SolIrExpressionId value = ir->operands[
            expression->as.call.operands.offset + index
        ].value;
        Flow argument = evaluate(interpreter, value);
        if (argument.kind != FLOW_VALUE) {
            free_values(arguments, index);
            free_values(writebacks, count);
            sol_interpreter_value_free(&callee.value);
            sol_interpreter_value_free(&receiver.value);
            return argument;
        }
        arguments[index] = argument.value;
    }
    Flow output = flow_new(FLOW_ERROR);
    switch (expression->as.call.kind) {
        case SOL_IR_CALL_BUILTIN_NONE:
        case SOL_IR_CALL_BUILTIN_SOME:
        case SOL_IR_CALL_BUILTIN_OK:
        case SOL_IR_CALL_BUILTIN_ERR:
            if (new_value(interpreter, expression->span)) {
                output.kind = FLOW_VALUE;
                output.value.kind = expression->as.call.kind == SOL_IR_CALL_BUILTIN_NONE
                    || expression->as.call.kind == SOL_IR_CALL_BUILTIN_SOME
                    ? SOL_INTERPRETER_VALUE_OPTION : SOL_INTERPRETER_VALUE_RESULT;
                output.value.as.sum.has_value
                    = expression->as.call.kind != SOL_IR_CALL_BUILTIN_NONE;
                output.value.as.sum.is_error
                    = expression->as.call.kind == SOL_IR_CALL_BUILTIN_ERR;
                if (output.value.as.sum.has_value) {
                    output.value.as.sum.value = malloc(sizeof(*output.value.as.sum.value));
                    if (output.value.as.sum.value == NULL) output.kind = FLOW_ERROR;
                    else {
                        *output.value.as.sum.value = arguments[0];
                        sol_interpreter_value_init(&arguments[0]);
                    }
                }
            }
            break;
        case SOL_IR_CALL_ENUM_CONSTRUCTOR:
            if (new_value(interpreter, expression->span)) {
                output.kind = FLOW_VALUE;
                output.value.kind = SOL_INTERPRETER_VALUE_ENUM;
                output.value.as.aggregate.definition = expression->as.call.definition;
                output.value.as.aggregate.variant = expression->as.call.variant;
                output.value.as.aggregate.fields = arguments;
                output.value.as.aggregate.field_count = count;
                arguments = NULL;
            }
            break;
        case SOL_IR_CALL_DISTINCT_CONSTRUCTOR:
            if (new_value(interpreter, expression->span)) {
                output.kind = FLOW_VALUE;
                output.value.kind = SOL_INTERPRETER_VALUE_DISTINCT;
                output.value.as.distinct.definition = expression->as.call.definition;
                output.value.as.distinct.value = malloc(sizeof(*output.value.as.distinct.value));
                if (output.value.as.distinct.value == NULL) output.kind = FLOW_ERROR;
                else {
                    *output.value.as.distinct.value = arguments[0];
                    sol_interpreter_value_init(&arguments[0]);
                }
            }
            break;
        case SOL_IR_CALL_CALLBACK:
            if (callee.value.kind == SOL_INTERPRETER_VALUE_FUNCTION
                && callee.value.as.callable.callable < ir->callable_count
                && ir->callables[callee.value.as.callable.callable].kind
                    == SOL_IR_CALLABLE_FUNCTION) {
                output = invoke(interpreter, callee.value.as.callable.callable, NULL,
                    arguments, count, NULL, 0, NULL, 0, NULL, writebacks,
                    expression->span);
            } else if (callee.value.kind == SOL_INTERPRETER_VALUE_BOUND_OPERATION) {
                bool exclusive = false;
                for (size_t index = 0; index < count; ++index) {
                    exclusive = exclusive || ir->operands[
                        expression->as.call.operands.offset + index].access
                            == SOL_ACCESS_EXCLUSIVE;
                }
                if (exclusive && callee.value.as.callable.callable >= ir->callable_count) {
                    diagnostic(interpreter, SOL_INTERPRETER_TYPE_INVARIANT,
                        expression->span, "capability callback metadata is invalid");
                } else output = invoke_operation(interpreter,
                    callee.value.as.callable.receiver,
                    callee.value.as.callable.callable, arguments, count,
                    writebacks, expression->span);
            } else diagnostic(interpreter, SOL_INTERPRETER_TYPE_INVARIANT,
                expression->span, "callback callee is not executable");
            break;
        case SOL_IR_CALL_CAPABILITY:
            {
            if (callee.value.kind != SOL_INTERPRETER_VALUE_BOUND_OPERATION) {
                diagnostic(interpreter, SOL_INTERPRETER_TYPE_INVARIANT,
                    expression->span, "capability callee is not a bound operation");
            } else output = invoke_operation(interpreter,
                callee.value.as.callable.receiver, callee.value.as.callable.callable,
                arguments, count, writebacks, expression->span);
            break;
            }
        case SOL_IR_CALL_METHOD: {
            const SolIrDispatchEvidence *selected = NULL;
            for (size_t index = 0; index < expression->as.call.evidence.count; ++index) {
                const SolIrDispatchEvidence *candidate = &ir->evidence[
                    expression->as.call.evidence.offset + index
                ];
                if (candidate->requirement == expression->as.call.callable) {
                    selected = resolve_evidence(interpreter, candidate);
                    break;
                }
            }
            if (selected == NULL) diagnostic(interpreter,
                SOL_INTERPRETER_TYPE_INVARIANT, expression->span,
                "method invocation evidence is unavailable");
            else output = invoke(interpreter, selected->method, &receiver.value,
                arguments, count, NULL, 0, NULL, 0,
                expression->as.call.receiver_access == SOL_ACCESS_EXCLUSIVE
                    ? &receiver_writeback : NULL,
                writebacks, expression->span);
            break;
        }
        case SOL_IR_CALL_FUNCTION:
            output = invoke(interpreter, expression->as.call.callable, NULL,
                arguments, count,
                expression->as.call.type_arguments.count == 0 ? NULL
                    : ir->type_ids + expression->as.call.type_arguments.offset,
                expression->as.call.type_arguments.count,
                expression->as.call.evidence.count == 0 ? NULL
                    : ir->evidence + expression->as.call.evidence.offset,
                expression->as.call.evidence.count, NULL, writebacks,
                expression->span);
            break;
    }
    if (output.kind != FLOW_ERROR) {
        if (receiver_writeback.kind != SOL_INTERPRETER_VALUE_INVALID) {
            const SolIrPlace *place = runtime_expression_place(ir,
                expression->as.call.receiver);
            if (!charge_place_update(interpreter, place, expression->span)) {
                sol_interpreter_value_free(&output.value);
                output = flow_new(FLOW_ERROR);
            }
        }
        for (size_t index = 0; output.kind != FLOW_ERROR && index < count; ++index) {
            if (writebacks[index].kind == SOL_INTERPRETER_VALUE_INVALID) continue;
            SolIrExpressionId actual = ir->operands[
                expression->as.call.operands.offset + index].value;
            if (!charge_place_update(interpreter, runtime_expression_place(ir, actual),
                    expression->span)) {
                sol_interpreter_value_free(&output.value);
                output = flow_new(FLOW_ERROR);
            }
        }
    }
    if (output.kind != FLOW_ERROR
        && receiver_writeback.kind != SOL_INTERPRETER_VALUE_INVALID) {
        const SolIrPlace *place = runtime_expression_place(ir,
            expression->as.call.receiver);
        if (place == NULL || !update_place(interpreter, place,
                &receiver_writeback, expression->span)) {
            sol_interpreter_value_free(&output.value);
            output = flow_new(FLOW_ERROR);
        }
    }
    for (size_t index = 0; output.kind != FLOW_ERROR && index < count; ++index) {
        if (writebacks[index].kind == SOL_INTERPRETER_VALUE_INVALID) continue;
        SolIrExpressionId actual = ir->operands[
            expression->as.call.operands.offset + index].value;
        const SolIrPlace *place = runtime_expression_place(ir, actual);
        if (place == NULL || !update_place(interpreter, place,
                &writebacks[index], expression->span)) {
            sol_interpreter_value_free(&output.value);
            output = flow_new(FLOW_ERROR);
        }
    }
    free_values(arguments, count);
    free_values(writebacks, count);
    sol_interpreter_value_free(&receiver_writeback);
    sol_interpreter_value_free(&callee.value);
    sol_interpreter_value_free(&receiver.value);
    return output;
}

static Flow evaluate_block(Interpreter *interpreter,
    const SolIrExpression *expression) {
    Flow last = flow_new(FLOW_VALUE);
    sol_interpreter_value_unit(&last.value);
    const SolIr *ir = interpreter->request->ir;
    for (size_t index = 0; index < expression->as.block.statements.count; ++index) {
        const SolIrStatement *statement = &ir->statements[ir->statement_ids[
            expression->as.block.statements.offset + index
        ]];
        Flow value = evaluate(interpreter, statement->expression);
        if (value.kind != FLOW_VALUE) {
            sol_interpreter_value_free(&last.value);
            cleanup_slice(interpreter, expression->as.block.cleanup);
            return value;
        }
        if (statement->kind == SOL_IR_STATEMENT_RETURN) {
            sol_interpreter_value_free(&last.value);
            value.kind = FLOW_RETURN;
            cleanup_slice(interpreter, expression->as.block.cleanup);
            return value;
        }
        if (statement->kind == SOL_IR_STATEMENT_LET) {
            if (!bind_local(interpreter, interpreter->frame, statement->local,
                &value.value, statement->span)) {
                sol_interpreter_value_free(&value.value);
                sol_interpreter_value_free(&last.value);
                cleanup_slice(interpreter, expression->as.block.cleanup);
                return flow_new(FLOW_ERROR);
            }
            sol_interpreter_value_free(&value.value);
        } else if (statement->kind == SOL_IR_STATEMENT_ASSIGNMENT) {
            const SolIrExpression *target = &ir->expressions[statement->target];
            const SolIrPlace *place = &ir->places[target->as.place];
            if (!charge_place_update(interpreter, place, statement->span)
                || !update_place(interpreter, place, &value.value,
                statement->span)) {
                sol_interpreter_value_free(&value.value);
                sol_interpreter_value_free(&last.value);
                cleanup_slice(interpreter, expression->as.block.cleanup);
                return flow_new(FLOW_ERROR);
            }
            sol_interpreter_value_free(&last.value);
            last = flow_new(FLOW_VALUE);
            sol_interpreter_value_unit(&last.value);
        } else {
            sol_interpreter_value_free(&last.value);
            last = value;
        }
    }
    cleanup_slice(interpreter, expression->as.block.cleanup);
    return last;
}

static Flow evaluate(Interpreter *interpreter, SolIrExpressionId expression_id) {
    const SolIr *ir = interpreter->request->ir;
    if (expression_id >= ir->expression_count) {
        diagnostic(interpreter, SOL_INTERPRETER_INTERNAL_INVARIANT, (SolSpan){0},
            "expression is out of range");
        return flow_new(FLOW_ERROR);
    }
    const SolIrExpression *expression = &ir->expressions[expression_id];
    if (!consume(interpreter, &interpreter->result->used.steps,
        interpreter->request->limits.steps, SOL_INTERPRETER_STEP_LIMIT,
        expression->span, "step")) return flow_new(FLOW_ERROR);
    Flow output = flow_new(FLOW_VALUE);
    switch (expression->kind) {
        case SOL_IR_EXPR_INTEGER:
            if (!new_value(interpreter, expression->span)) return flow_new(FLOW_ERROR);
            sol_interpreter_value_int64(&output.value, expression->as.integer);
            return output;
        case SOL_IR_EXPR_BOOL:
            if (!new_value(interpreter, expression->span)) return flow_new(FLOW_ERROR);
            sol_interpreter_value_bool(&output.value, expression->as.boolean);
            return output;
        case SOL_IR_EXPR_STRING:
            if (strlen(expression->as.string) > interpreter->request->limits.text_bytes
                    - interpreter->result->used.text_bytes) {
                diagnostic(interpreter, SOL_INTERPRETER_TEXT_LIMIT, expression->span,
                    "text byte limit exceeded");
                return flow_new(FLOW_ERROR);
            }
            if (!new_value(interpreter, expression->span)
                || !sol_interpreter_value_text(&output.value, expression->as.string,
                    strlen(expression->as.string))) return flow_new(FLOW_ERROR);
            interpreter->result->used.text_bytes += strlen(expression->as.string);
            return output;
        case SOL_IR_EXPR_UNIT:
            if (!new_value(interpreter, expression->span)) return flow_new(FLOW_ERROR);
            sol_interpreter_value_unit(&output.value);
            return output;
        case SOL_IR_EXPR_PLACE: {
            const SolIrPlace *place = &ir->places[expression->as.place];
            size_t projection_steps = place->projections.count;
            if (place->root_kind == SOL_IR_PLACE_ROOT_TEMPORARY) {
                --projection_steps;
            }
            for (size_t index = 0; index < projection_steps; ++index) {
                SolSpan step_span = index + 1 < place->projections.count
                    ? ir->projections[place->projections.offset
                        + place->projections.count - index - 2].span
                    : place->root_span;
                if (!consume(interpreter, &interpreter->result->used.steps,
                    interpreter->request->limits.steps, SOL_INTERPRETER_STEP_LIMIT,
                    step_span, "step")) return flow_new(FLOW_ERROR);
            }
            if (place->root_kind == SOL_IR_PLACE_ROOT_TEMPORARY) {
                output = evaluate(interpreter, place->temporary);
                if (output.kind != FLOW_VALUE) return output;
                for (size_t index = 0; index < place->projections.count; ++index) {
                    const SolIrProjection *projection
                        = &ir->projections[place->projections.offset + index];
                    SolInterpreterValue *slot = place_field(interpreter,
                        &output.value, projection);
                    if (slot == NULL || slot->kind == SOL_INTERPRETER_VALUE_INVALID) {
                        sol_interpreter_value_free(&output.value);
                        diagnostic(interpreter, SOL_INTERPRETER_TYPE_INVARIANT,
                            projection->span, "field projection base is invalid");
                        return flow_new(FLOW_ERROR);
                    }
                    SolInterpreterValue selected = *slot;
                    sol_interpreter_value_init(slot);
                    sol_interpreter_value_free(&output.value);
                    output.value = selected;
                }
                return output;
            } else {
                Frame *owner = NULL;
                SolInterpreterValue *value = place->projections.count == 0
                    ? lookup_local(interpreter, place->local, &owner)
                    : local_place_slot(interpreter, place, &owner);
                bool move = expression->local_use == SOL_IR_LOCAL_USE_MOVE;
                bool copied = value != NULL && !move
                    && clone_runtime(interpreter, place->root_span, &output.value, value);
                if (value != NULL && move) {
                    output.value = *value;
                    sol_interpreter_value_init(value);
                    if (place->projections.count == 0) owner->bound[place->local] = false;
                    copied = true;
                }
                if (!copied) {
                    if (value == NULL) diagnostic(interpreter,
                        SOL_INTERPRETER_INTERNAL_INVARIANT, place->root_span,
                        "local is not bound in the active frame");
                    return flow_new(FLOW_ERROR);
                }
                return output;
            }
        }
        case SOL_IR_EXPR_DEFINITION: {
            SolIrDefinitionId definition = expression->as.definition;
            if (definition >= ir->definition_count
                || ir->definitions[definition].kind != SOL_IR_DEFINITION_FUNCTION
                || ir->definitions[definition].callable >= ir->callable_count) {
                diagnostic(interpreter, SOL_INTERPRETER_TYPE_INVARIANT,
                    expression->span, "definition is not an executable function");
                return flow_new(FLOW_ERROR);
            }
            SolIrCallableId callable = ir->definitions[definition].callable;
            if (ir->callables[callable].kind != SOL_IR_CALLABLE_FUNCTION
                || ir->callables[callable].owner != definition
                || ir->callables[callable].generic_parameters.count != 0
                || ir->callables[callable].effect_parameters.count != 0) {
                diagnostic(interpreter, SOL_INTERPRETER_INVALID_IR,
                    expression->span,
                    "generic functions cannot become runtime function values");
                return flow_new(FLOW_ERROR);
            }
            if (!new_value(interpreter, expression->span)) return flow_new(FLOW_ERROR);
            output.value.kind = SOL_INTERPRETER_VALUE_FUNCTION;
            output.value.as.callable.callable = callable;
            return output;
        }
        case SOL_IR_EXPR_BOUND_OPERATION: {
            Flow receiver = evaluate(interpreter, expression->as.operation.receiver);
            if (receiver.kind != FLOW_VALUE) return receiver;
            bool capability = capability_value_valid(ir, &receiver.value, 0)
                && expression->as.operation.callable < ir->callable_count
                && ir->callables[expression->as.operation.callable].kind
                    == SOL_IR_CALLABLE_CAPABILITY
                && ir->callables[expression->as.operation.callable].owner
                    == receiver.value.as.capability.definition;
            if (!capability || !new_value(interpreter, expression->span)) {
                sol_interpreter_value_free(&receiver.value);
                if (!capability) diagnostic(
                    interpreter, SOL_INTERPRETER_TYPE_INVARIANT, expression->span,
                    "bound operation receiver is not a capability");
                return flow_new(FLOW_ERROR);
            }
            output.value.kind = SOL_INTERPRETER_VALUE_BOUND_OPERATION;
            output.value.as.callable.callable = expression->as.operation.callable;
            output.value.as.callable.receiver = malloc(sizeof(*output.value.as.callable.receiver));
            if (output.value.as.callable.receiver == NULL) {
                sol_interpreter_value_free(&receiver.value);
                return flow_new(FLOW_ERROR);
            }
            *output.value.as.callable.receiver = receiver.value;
            return output;
        }
        case SOL_IR_EXPR_UNARY: {
            Flow operand = evaluate(interpreter, expression->as.unary.operand);
            if (operand.kind != FLOW_VALUE) return operand;
            if (!new_value(interpreter, expression->span)) {
                sol_interpreter_value_free(&operand.value);
                return flow_new(FLOW_ERROR);
            }
            if (expression->as.unary.operator_kind == SOL_TOKEN_BANG
                && operand.value.kind == SOL_INTERPRETER_VALUE_BOOL) {
                sol_interpreter_value_bool(&output.value, !operand.value.as.boolean);
            } else if (expression->as.unary.operator_kind == SOL_TOKEN_MINUS
                && operand.value.kind == SOL_INTERPRETER_VALUE_INT64
                && operand.value.as.integer != INT64_MIN) {
                sol_interpreter_value_int64(&output.value, -operand.value.as.integer);
            } else {
                diagnostic(interpreter, operand.value.kind == SOL_INTERPRETER_VALUE_INT64
                    ? SOL_INTERPRETER_INTEGER_OVERFLOW : SOL_INTERPRETER_TYPE_INVARIANT,
                    expression->span, "invalid unary operation");
                output.kind = FLOW_ERROR;
            }
            sol_interpreter_value_free(&operand.value);
            return output;
        }
        case SOL_IR_EXPR_BINARY: return evaluate_binary(interpreter, expression);
        case SOL_IR_EXPR_CALL: return evaluate_call(interpreter, expression);
        case SOL_IR_EXPR_RECORD: {
            size_t count = expression->as.record.fields.count;
            SolInterpreterValue *fields = count == 0 ? NULL : calloc(count, sizeof(*fields));
            if (count != 0 && fields == NULL) return flow_new(FLOW_ERROR);
            for (size_t index = 0; index < count; ++index) {
                Flow field = evaluate(interpreter, ir->operands[
                    expression->as.record.fields.offset + index
                ].value);
                if (field.kind != FLOW_VALUE) {
                    free_values(fields, index);
                    return field;
                }
                fields[index] = field.value;
            }
            if (!new_value(interpreter, expression->span)) {
                free_values(fields, count);
                return flow_new(FLOW_ERROR);
            }
            if (ir->definitions[expression->as.record.definition].kind
                == SOL_IR_DEFINITION_CAPABILITY) {
                if (count != 1 || fields[0].kind != SOL_INTERPRETER_VALUE_CAPABILITY) {
                    free_values(fields, count);
                    diagnostic(interpreter, SOL_INTERPRETER_TYPE_INVARIANT,
                        expression->span, "capability wrapper source is invalid");
                    return flow_new(FLOW_ERROR);
                }
                output.value.kind = SOL_INTERPRETER_VALUE_CAPABILITY;
                output.value.as.capability.definition = expression->as.record.definition;
                output.value.as.capability.root = fields[0].as.capability.root;
                output.value.as.capability.source = malloc(
                    sizeof(*output.value.as.capability.source));
                if (output.value.as.capability.source == NULL) {
                    free_values(fields, count);
                    return flow_new(FLOW_ERROR);
                }
                *output.value.as.capability.source = fields[0];
                sol_interpreter_value_init(&fields[0]);
                free_values(fields, count);
            } else {
                output.value.kind = SOL_INTERPRETER_VALUE_RECORD;
                output.value.as.aggregate.definition = expression->as.record.definition;
                output.value.as.aggregate.variant = SOL_IR_NONE;
                output.value.as.aggregate.fields = fields;
                output.value.as.aggregate.field_count = count;
            }
            return output;
        }
        case SOL_IR_EXPR_VARIANT:
            if (!new_value(interpreter, expression->span)) return flow_new(FLOW_ERROR);
            output.value.kind = SOL_INTERPRETER_VALUE_ENUM;
            output.value.as.aggregate.definition
                = ir->variants[expression->as.variant.variant].owner;
            output.value.as.aggregate.variant = expression->as.variant.variant;
            return output;
        case SOL_IR_EXPR_IF: {
            Flow condition = evaluate(interpreter, expression->as.if_expr.condition);
            if (condition.kind != FLOW_VALUE) return condition;
            if (condition.value.kind != SOL_INTERPRETER_VALUE_BOOL) {
                sol_interpreter_value_free(&condition.value);
                diagnostic(interpreter, SOL_INTERPRETER_TYPE_INVARIANT,
                    expression->span, "if condition is not Bool");
                return flow_new(FLOW_ERROR);
            }
            bool choice = condition.value.as.boolean;
            sol_interpreter_value_free(&condition.value);
            return evaluate(interpreter, choice ? expression->as.if_expr.then_branch
                : expression->as.if_expr.else_branch);
        }
        case SOL_IR_EXPR_MATCH: {
            Flow scrutinee = evaluate(interpreter, expression->as.match_expr.scrutinee);
            if (scrutinee.kind != FLOW_VALUE) return scrutinee;
            for (size_t index = 0; index < expression->as.match_expr.arms.count; ++index) {
                const SolIrArm *arm = &ir->arms[ir->arm_ids[
                    expression->as.match_expr.arms.offset + index
                ]];
                bool matches = arm->kind == SOL_IR_PATTERN_WILDCARD
                    || (arm->kind == SOL_IR_PATTERN_BOOL
                        && scrutinee.value.kind == SOL_INTERPRETER_VALUE_BOOL
                        && scrutinee.value.as.boolean == arm->boolean)
                    || (arm->kind == SOL_IR_PATTERN_VARIANT
                        && scrutinee.value.kind == SOL_INTERPRETER_VALUE_ENUM
                        && scrutinee.value.as.aggregate.variant == arm->variant);
                if (!matches) continue;
                for (size_t binding = 0; binding < arm->bindings.count; ++binding) {
                    if (!bind_local(interpreter, interpreter->frame,
                        ir->roots[arm->bindings.offset + binding],
                        &scrutinee.value.as.aggregate.fields[binding], arm->span)) {
                        sol_interpreter_value_free(&scrutinee.value);
                        cleanup_slice(interpreter, arm->cleanup);
                        return flow_new(FLOW_ERROR);
                    }
                }
                sol_interpreter_value_free(&scrutinee.value);
                Flow value = evaluate(interpreter, arm->value);
                cleanup_slice(interpreter, arm->cleanup);
                return value;
            }
            sol_interpreter_value_free(&scrutinee.value);
            diagnostic(interpreter, SOL_INTERPRETER_NO_MATCH, expression->span,
                "match has no runtime arm");
            return flow_new(FLOW_ERROR);
        }
        case SOL_IR_EXPR_BLOCK: return evaluate_block(interpreter, expression);
        case SOL_IR_EXPR_PROPAGATE: {
            Flow operand = evaluate(interpreter, expression->as.propagate.operand);
            if (operand.kind != FLOW_VALUE) return operand;
            if ((operand.value.kind != SOL_INTERPRETER_VALUE_OPTION
                    && operand.value.kind != SOL_INTERPRETER_VALUE_RESULT)
                || (operand.value.kind == SOL_INTERPRETER_VALUE_OPTION
                    && expression->as.propagate.kind != SOL_IR_PROPAGATE_OPTION)
                || (operand.value.kind == SOL_INTERPRETER_VALUE_RESULT
                    && expression->as.propagate.kind != SOL_IR_PROPAGATE_RESULT)) {
                sol_interpreter_value_free(&operand.value);
                diagnostic(interpreter, SOL_INTERPRETER_TYPE_INVARIANT,
                    expression->span, "propagation operand is invalid");
                return flow_new(FLOW_ERROR);
            }
            bool residual = !operand.value.as.sum.has_value
                || operand.value.as.sum.is_error;
            if (residual) {
                operand.kind = FLOW_RETURN;
                return operand;
            }
            output.value = *operand.value.as.sum.value;
            sol_interpreter_value_init(operand.value.as.sum.value);
            sol_interpreter_value_free(&operand.value);
            return output;
        }
        case SOL_IR_EXPR_HANDLE: {
            Flow authority = evaluate(interpreter, expression->as.handler.authority);
            if (authority.kind != FLOW_VALUE) return authority;
            Flow provider = evaluate(interpreter, expression->as.handler.provider);
            if (provider.kind != FLOW_VALUE) {
                sol_interpreter_value_free(&authority.value);
                return provider;
            }
            if (!capability_value_valid(ir, &authority.value, 0)
                || !capability_value_valid(ir, &provider.value, 0)
                || expression->as.handler.source >= ir->callable_count
                || expression->as.handler.provider_callable >= ir->callable_count
                || ir->callables[expression->as.handler.source].owner
                    != authority.value.as.capability.definition
                || ir->callables[expression->as.handler.provider_callable].owner
                    != provider.value.as.capability.definition) {
                sol_interpreter_value_free(&authority.value);
                sol_interpreter_value_free(&provider.value);
                diagnostic(interpreter, SOL_INTERPRETER_TYPE_INVARIANT,
                    expression->span, "handler authority/provider domains are invalid");
                return flow_new(FLOW_ERROR);
            }
            Handler handler = {
                .parent = interpreter->handler,
                .source = expression->as.handler.source,
                .provider_callable = expression->as.handler.provider_callable,
                .effect_name = expression->as.handler.effect_name,
                .root = authority.value.as.capability.root,
                .provider = provider.value,
            };
            interpreter->handler = &handler;
            Flow body = evaluate(interpreter, expression->as.handler.body);
            interpreter->handler = handler.parent;
            sol_interpreter_value_free(&authority.value);
            sol_interpreter_value_free(&handler.provider);
            return body;
        }
        case SOL_IR_EXPR_REFINEMENT_SELF:
        case SOL_IR_EXPR_RESULT:
        case SOL_IR_EXPR_SNAPSHOT_READ:
        case SOL_IR_EXPR_COMPILE_TIME_HEAD:
            diagnostic(interpreter, SOL_INTERPRETER_INVALID_IR, expression->span,
                "contract-only or compile-time expression reached an ordinary body");
            return flow_new(FLOW_ERROR);
    }
    diagnostic(interpreter, SOL_INTERPRETER_INTERNAL_INVARIANT, expression->span,
        "unknown expression kind");
    return flow_new(FLOW_ERROR);
}

static SolInterpreterLimits default_limits(void) {
    return (SolInterpreterLimits){
        .steps = 100000,
        .call_depth = 128,
        .value_nodes = 100000,
        .text_bytes = 1024 * 1024,
        .host_calls = 10000,
    };
}

bool sol_interpret(const SolInterpreterRequest *request, SolInterpreterResult *result) {
    if (result == NULL) return false;
    sol_interpreter_result_init(result);
    if (request == NULL || request->ir == NULL) {
        result->diagnostic.code = SOL_INTERPRETER_INVALID_REQUEST;
        (void)snprintf(result->diagnostic.message, sizeof(result->diagnostic.message),
            "interpreter request and owning IR are required");
        return false;
    }
    SolInterpreterRequest normalized = *request;
    SolInterpreterLimits zero = {0};
    if (memcmp(&normalized.limits, &zero, sizeof(zero)) == 0) {
        normalized.limits = default_limits();
    }
    Interpreter interpreter = {.request = &normalized, .result = result};
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    if (!sol_ir_validate(request->ir, &diagnostics)) {
        diagnostic(&interpreter, SOL_INTERPRETER_INVALID_IR, (SolSpan){0},
            "interpreter input IR failed validation");
        sol_diagnostics_free(&diagnostics);
        return false;
    }
    sol_diagnostics_free(&diagnostics);
    if (request->contracts != SOL_INTERPRETER_CONTRACTS_IGNORE) {
        diagnostic(&interpreter, SOL_INTERPRETER_UNSUPPORTED_CONTRACT_POLICY,
            (SolSpan){0}, "runtime contract checking is not supported");
        return false;
    }
    SolIrCallableId callable = request->callable;
    bool selected_definition = callable == SOL_IR_NONE;
    if (callable == SOL_IR_NONE) {
        if (request->definition >= request->ir->definition_count) {
            diagnostic(&interpreter, SOL_INTERPRETER_INVALID_REQUEST,
                (SolSpan){0}, "request selects no valid callable or definition");
            return false;
        }
        callable = request->ir->definitions[request->definition].callable;
    }
    if (callable >= request->ir->callable_count
        || (request->argument_count != 0 && request->arguments == NULL)
        || (request->type_argument_count != 0 && request->type_arguments == NULL)
        || (request->evidence.count != 0 && request->evidence.items == NULL)) {
        diagnostic(&interpreter, SOL_INTERPRETER_INVALID_REQUEST,
            (SolSpan){0}, "request argument or callable domain is malformed");
        return false;
    }
    SolIrCallableKind kind = request->ir->callables[callable].kind;
    if (selected_definition) {
        const SolIrDefinition *definition
            = &request->ir->definitions[request->definition];
        SolIrDefinitionKind expected = request->test_entry
            ? SOL_IR_DEFINITION_TEST : SOL_IR_DEFINITION_FUNCTION;
        if (definition->kind != expected
            || request->ir->callables[callable].owner != request->definition) {
            diagnostic(&interpreter, SOL_INTERPRETER_INVALID_REQUEST,
                (SolSpan){0}, "definition does not own the requested entry kind");
            return false;
        }
    }
    if ((!request->test_entry && kind != SOL_IR_CALLABLE_FUNCTION)
        || (request->test_entry && kind != SOL_IR_CALLABLE_TEST)) {
        diagnostic(&interpreter, SOL_INTERPRETER_INVALID_REQUEST,
            (SolSpan){0}, request->test_entry
                ? "test request must select a test callable"
                : "request must select a free-function callable");
        return false;
    }
    const SolIrCallable *entry = &request->ir->callables[callable];
    for (size_t index = 0; index < entry->parameters.count; ++index) {
        SolIrLocalId local = request->ir->roots[entry->parameters.offset + index];
        if (request->ir->locals[local].access == SOL_ACCESS_EXCLUSIVE) {
            diagnostic(&interpreter, SOL_INTERPRETER_INVALID_REQUEST,
                entry->span,
                "top-level exclusive parameters require a mutable argument ABI");
            return false;
        }
    }
    Flow flow = invoke(&interpreter, callable, NULL, request->arguments,
        request->argument_count, request->type_arguments, request->type_argument_count,
        request->evidence.items, request->evidence.count, NULL, NULL,
        request->ir->callables[callable].span);
    if (flow.kind == FLOW_ERROR) {
        if (result->diagnostic.code == SOL_INTERPRETER_OK) {
            diagnostic(&interpreter, SOL_INTERPRETER_INTERNAL_INVARIANT,
                request->ir->callables[callable].span,
                "execution failed without a more specific diagnostic");
        }
        return false;
    }
    result->value = flow.value;
    return true;
}
