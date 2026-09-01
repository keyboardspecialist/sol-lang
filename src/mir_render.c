#include "sol/mir.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include "resource_internal.h"
#include <string.h>

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
} MirRenderBuffer;

typedef struct {
    const SolIr *ir;
    const SolMir *mir;
    MirRenderBuffer output;
    unsigned char *expressions;
    unsigned char *statements;
    unsigned char *patterns;
    unsigned char *arms;
    unsigned char *snapshots;
    unsigned char *obligations;
} MirRenderer;

static bool render_reserve(MirRenderBuffer *buffer, size_t extra) {
    if (buffer->failed || extra > SIZE_MAX - buffer->length) {
        buffer->failed = true;
        return false;
    }
    size_t needed = buffer->length + extra;
    if (needed <= buffer->capacity) return true;
    size_t capacity = buffer->capacity == 0 ? 4096 : buffer->capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    char *grown = realloc(buffer->data, capacity);
    if (grown == NULL) {
        buffer->failed = true;
        return false;
    }
    buffer->data = grown;
    buffer->capacity = capacity;
    return true;
}

static void render_bytes(MirRenderBuffer *buffer, const char *data,
    size_t length) {
    if (!render_reserve(buffer, length)) return;
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
}

static void render_text(MirRenderBuffer *buffer, const char *text) {
    render_bytes(buffer, text, strlen(text));
}

static void render_format(MirRenderBuffer *buffer, const char *format, ...) {
    if (buffer->failed) return;
    va_list arguments;
    va_start(arguments, format);
    va_list copy;
    va_copy(copy, arguments);
    int count = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (count < 0 || (size_t)count == SIZE_MAX
        || !render_reserve(buffer, (size_t)count + 1)) {
        buffer->failed = true;
        va_end(arguments);
        return;
    }
    (void)vsnprintf(buffer->data + buffer->length,
        buffer->capacity - buffer->length, format, arguments);
    va_end(arguments);
    buffer->length += (size_t)count;
}

static void render_quoted(MirRenderBuffer *buffer, const char *text) {
    static const char hex[] = "0123456789ABCDEF";
    render_text(buffer, "\"");
    for (const unsigned char *cursor = (const unsigned char *)text;
        *cursor != 0; ++cursor) {
        unsigned char byte = *cursor;
        switch (byte) {
            case '\n': render_text(buffer, "\\n"); break;
            case '\r': render_text(buffer, "\\r"); break;
            case '\t': render_text(buffer, "\\t"); break;
            case '\b': render_text(buffer, "\\b"); break;
            case '\f': render_text(buffer, "\\f"); break;
            case '\v': render_text(buffer, "\\v"); break;
            case '\\': render_text(buffer, "\\\\"); break;
            case '"': render_text(buffer, "\\\""); break;
            default:
                if (byte >= 0x20 && byte <= 0x7e) {
                    char character = (char)byte;
                    render_bytes(buffer, &character, 1);
                } else {
                    char escape[] = {'\\', 'x', hex[byte >> 4],
                        hex[byte & 0x0f]};
                    render_bytes(buffer, escape, sizeof(escape));
                }
                break;
        }
    }
    render_text(buffer, "\"");
}

static void render_span(MirRenderBuffer *buffer, SolSpan span) {
    render_format(buffer, "%zu..%zu", span.start, span.end);
}

static void render_id(MirRenderBuffer *buffer, const char *prefix, size_t id,
    size_t none) {
    if (id == none) render_text(buffer, "none");
    else render_format(buffer, "%s%zu", prefix, id);
}

static const char *access_name(SolAccessMode access) {
    switch (access) {
        case SOL_ACCESS_OWNED: return "owned";
        case SOL_ACCESS_SHARED: return "shared";
        case SOL_ACCESS_EXCLUSIVE: return "exclusive";
    }
    return NULL;
}

static const char *authority_name(SolIrAuthorityKind kind) {
    switch (kind) {
        case SOL_IR_AUTHORITY_NONE: return "none";
        case SOL_IR_AUTHORITY_LOCAL: return "local";
        case SOL_IR_AUTHORITY_SELF: return "self";
    }
    return NULL;
}

static const char *value_kind_name(SolMirValueKind kind) {
    switch (kind) {
        case SOL_MIR_VALUE_BLOCK_PARAMETER: return "block_parameter";
        case SOL_MIR_VALUE_INSTRUCTION: return "instruction";
        case SOL_MIR_VALUE_TERMINATOR: return "terminator";
    }
    return NULL;
}

static const char *instruction_kind_name(SolMirInstructionKind kind) {
    switch (kind) {
        case SOL_MIR_INST_CONST_INT64: return "const_int64";
        case SOL_MIR_INST_CONST_BOOL: return "const_bool";
        case SOL_MIR_INST_CONST_TEXT: return "const_text";
        case SOL_MIR_INST_CONST_UNIT: return "const_unit";
        case SOL_MIR_INST_PARAMETER_LIVE: return "parameter_live";
        case SOL_MIR_INST_STORAGE_LIVE: return "storage_live";
        case SOL_MIR_INST_DROP_IF_INITIALIZED: return "drop_if_initialized";
        case SOL_MIR_INST_STORAGE_DEAD: return "storage_dead";
        case SOL_MIR_INST_LOAD_COPY: return "load_copy";
        case SOL_MIR_INST_LOAD_MOVE: return "load_move";
        case SOL_MIR_INST_LOAD_UPDATE: return "load_update";
        case SOL_MIR_INST_STORE: return "store";
        case SOL_MIR_INST_UNARY: return "unary";
        case SOL_MIR_INST_BINARY: return "binary";
        case SOL_MIR_INST_COMPOUND_UPDATE: return "compound_update";
        case SOL_MIR_INST_REGION_ENTER: return "region_enter";
        case SOL_MIR_INST_REGION_EXIT: return "region_exit";
        case SOL_MIR_INST_TEMPORARY_INIT: return "temporary_init";
        case SOL_MIR_INST_TEMPORARY_DROP: return "temporary_drop";
        case SOL_MIR_INST_EXPRESSION_RESULT: return "expression_result";
        case SOL_MIR_INST_PATTERN_TEST: return "pattern_test";
        case SOL_MIR_INST_PATTERN_VALUE: return "pattern_value";
        case SOL_MIR_INST_MATCH_ARM: return "match_arm";
        case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED:
            return "drop_place_if_initialized";
        case SOL_MIR_INST_HANDLER_ENTER: return "handler_enter";
        case SOL_MIR_INST_HANDLER_EXIT: return "handler_exit";
        case SOL_MIR_INST_CONSTRUCT: return "construct";
        case SOL_MIR_INST_CAPTURE_SNAPSHOT: return "capture_snapshot";
    }
    return NULL;
}

static const char *construct_kind_name(SolMirConstructKind kind) {
    switch (kind) {
        case SOL_MIR_CONSTRUCT_RECORD: return "record";
        case SOL_MIR_CONSTRUCT_CAPABILITY: return "capability";
        case SOL_MIR_CONSTRUCT_TUPLE: return "tuple";
        case SOL_MIR_CONSTRUCT_ENUM: return "enum";
        case SOL_MIR_CONSTRUCT_OPTION_NONE: return "option_none";
        case SOL_MIR_CONSTRUCT_OPTION_SOME: return "option_some";
        case SOL_MIR_CONSTRUCT_RESULT_OK: return "result_ok";
        case SOL_MIR_CONSTRUCT_RESULT_ERR: return "result_err";
        case SOL_MIR_CONSTRUCT_DISTINCT: return "distinct";
    }
    return NULL;
}

static const char *terminator_kind_name(SolMirTerminatorKind kind) {
    switch (kind) {
        case SOL_MIR_TERM_INVALID: return "invalid";
        case SOL_MIR_TERM_GOTO: return "goto";
        case SOL_MIR_TERM_BRANCH: return "branch";
        case SOL_MIR_TERM_RETURN: return "return";
        case SOL_MIR_TERM_PANIC: return "panic";
        case SOL_MIR_TERM_INVOKE: return "invoke";
        case SOL_MIR_TERM_RESUME_FAILURE: return "resume_failure";
        case SOL_MIR_TERM_UNREACHABLE: return "unreachable";
        case SOL_MIR_TERM_BREAK: return "break";
        case SOL_MIR_TERM_CONTINUE: return "continue";
        case SOL_MIR_TERM_CHECK_REFINED: return "check_refined";
        case SOL_MIR_TERM_MATCH_FAILURE: return "match_failure";
        case SOL_MIR_TERM_PROPAGATE: return "propagate";
        case SOL_MIR_TERM_CHECK_CONTRACT: return "check_contract";
        case SOL_MIR_TERM_CONTRACT_VIOLATION: return "contract_violation";
    }
    return NULL;
}

static const char *call_kind_name(SolIrCallKind kind) {
    switch (kind) {
        case SOL_IR_CALL_FUNCTION: return "function";
        case SOL_IR_CALL_CALLBACK: return "callback";
        case SOL_IR_CALL_CAPABILITY: return "capability";
        case SOL_IR_CALL_METHOD: return "method";
        case SOL_IR_CALL_BUILTIN_OK: return "builtin_ok";
        case SOL_IR_CALL_BUILTIN_ERR: return "builtin_err";
        case SOL_IR_CALL_BUILTIN_SOME: return "builtin_some";
        case SOL_IR_CALL_BUILTIN_NONE: return "builtin_none";
        case SOL_IR_CALL_ENUM_CONSTRUCTOR: return "enum_constructor";
        case SOL_IR_CALL_DISTINCT_CONSTRUCTOR: return "distinct_constructor";
    }
    return NULL;
}

static const char *propagation_name(SolIrPropagationKind kind) {
    switch (kind) {
        case SOL_IR_PROPAGATE_OPTION: return "option";
        case SOL_IR_PROPAGATE_RESULT: return "result";
    }
    return NULL;
}

static const char *phase_name(SolContractClauseKind kind) {
    switch (kind) {
        case SOL_CONTRACT_REQUIRES: return "requires";
        case SOL_CONTRACT_ENSURES: return "ensures";
    }
    return NULL;
}

static const char *outcome_name(SolContractOutcomeKind kind) {
    switch (kind) {
        case SOL_CONTRACT_OUTCOME_ALWAYS: return "always";
        case SOL_CONTRACT_OUTCOME_SUCCESS: return "success";
        case SOL_CONTRACT_OUTCOME_FAILURE: return "failure";
    }
    return NULL;
}

static const char *operator_name(SolTokenKind kind) {
    switch (kind) {
        case SOL_TOKEN_LESS: return "less";
        case SOL_TOKEN_GREATER: return "greater";
        case SOL_TOKEN_LESS_EQUAL: return "less_equal";
        case SOL_TOKEN_GREATER_EQUAL: return "greater_equal";
        case SOL_TOKEN_EQUAL: return "equal";
        case SOL_TOKEN_EQUAL_EQUAL: return "equal_equal";
        case SOL_TOKEN_BANG: return "bang";
        case SOL_TOKEN_BANG_EQUAL: return "bang_equal";
        case SOL_TOKEN_PLUS: return "plus";
        case SOL_TOKEN_MINUS: return "minus";
        case SOL_TOKEN_STAR: return "star";
        case SOL_TOKEN_SLASH: return "slash";
        case SOL_TOKEN_PERCENT: return "percent";
        case SOL_TOKEN_PLUS_EQUAL: return "plus_equal";
        case SOL_TOKEN_MINUS_EQUAL: return "minus_equal";
        case SOL_TOKEN_STAR_EQUAL: return "star_equal";
        case SOL_TOKEN_SLASH_EQUAL: return "slash_equal";
        case SOL_TOKEN_PERCENT_EQUAL: return "percent_equal";
        case SOL_TOKEN_AMP_AMP: return "and";
        case SOL_TOKEN_PIPE_PIPE: return "or";
        default: return NULL;
    }
}

static void render_enum(MirRenderBuffer *buffer, const char *name) {
    if (name == NULL) buffer->failed = true;
    else render_text(buffer, name);
}

static const char *callable_kind_name(SolIrCallableKind kind) {
    switch (kind) {
        case SOL_IR_CALLABLE_FUNCTION: return "function";
        case SOL_IR_CALLABLE_CAPABILITY: return "capability";
        case SOL_IR_CALLABLE_TRAIT_REQUIREMENT: return "trait_requirement";
        case SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION: return "trait_implementation";
        case SOL_IR_CALLABLE_TEST: return "test";
    }
    return NULL;
}

static const char *expression_kind_name(SolIrExpressionKind kind) {
    switch (kind) {
        case SOL_IR_EXPR_INTEGER: return "integer";
        case SOL_IR_EXPR_STRING: return "string";
        case SOL_IR_EXPR_BOOL: return "bool";
        case SOL_IR_EXPR_UNIT: return "unit";
        case SOL_IR_EXPR_PLACE: return "place";
        case SOL_IR_EXPR_DEFINITION: return "definition";
        case SOL_IR_EXPR_REFINEMENT_SELF: return "refinement_self";
        case SOL_IR_EXPR_UNARY: return "unary";
        case SOL_IR_EXPR_BINARY: return "binary";
        case SOL_IR_EXPR_CALL: return "call";
        case SOL_IR_EXPR_RECORD: return "record";
        case SOL_IR_EXPR_TUPLE: return "tuple";
        case SOL_IR_EXPR_VARIANT: return "variant";
        case SOL_IR_EXPR_IF: return "if";
        case SOL_IR_EXPR_MATCH: return "match";
        case SOL_IR_EXPR_BLOCK: return "block";
        case SOL_IR_EXPR_PROPAGATE: return "propagate";
        case SOL_IR_EXPR_HANDLE: return "handle";
        case SOL_IR_EXPR_RESULT: return "result";
        case SOL_IR_EXPR_SNAPSHOT_READ: return "snapshot_read";
        case SOL_IR_EXPR_COMPILE_TIME_HEAD: return "compile_time_head";
        case SOL_IR_EXPR_BOUND_OPERATION: return "bound_operation";
    }
    return NULL;
}

static const char *local_use_name(SolIrLocalUse use) {
    switch (use) {
        case SOL_IR_LOCAL_USE_NONE: return "none";
        case SOL_IR_LOCAL_USE_COPY: return "copy";
        case SOL_IR_LOCAL_USE_MOVE: return "move";
        case SOL_IR_LOCAL_USE_SHARED: return "shared";
        case SOL_IR_LOCAL_USE_EXCLUSIVE: return "exclusive";
        case SOL_IR_LOCAL_USE_UPDATE: return "update";
    }
    return NULL;
}

static const char *statement_kind_name(SolIrStatementKind kind) {
    switch (kind) {
        case SOL_IR_STATEMENT_LET: return "let";
        case SOL_IR_STATEMENT_DECLARE: return "declare";
        case SOL_IR_STATEMENT_ASSIGNMENT: return "assignment";
        case SOL_IR_STATEMENT_RETURN: return "return";
        case SOL_IR_STATEMENT_EXPRESSION: return "expression";
        case SOL_IR_STATEMENT_REGION: return "region";
        case SOL_IR_STATEMENT_MODIFY: return "modify";
        case SOL_IR_STATEMENT_LOOP: return "loop";
        case SOL_IR_STATEMENT_WHILE: return "while";
        case SOL_IR_STATEMENT_BREAK: return "break";
        case SOL_IR_STATEMENT_CONTINUE: return "continue";
        case SOL_IR_STATEMENT_PANIC: return "panic";
        case SOL_IR_STATEMENT_UNREACHABLE: return "unreachable";
        case SOL_IR_STATEMENT_REQUIRE: return "require";
    }
    return NULL;
}

static const char *pattern_kind_name(SolIrPatternKind kind) {
    switch (kind) {
        case SOL_IR_PATTERN_WILDCARD: return "wildcard";
        case SOL_IR_PATTERN_BOOL: return "bool";
        case SOL_IR_PATTERN_BINDING: return "binding";
        case SOL_IR_PATTERN_VARIANT: return "variant";
        case SOL_IR_PATTERN_RECORD: return "record";
        case SOL_IR_PATTERN_TUPLE: return "tuple";
    }
    return NULL;
}

static const char *contract_owner_name(SolContractOwnerKind kind) {
    switch (kind) {
        case SOL_CONTRACT_OWNER_ITEM: return "item";
        case SOL_CONTRACT_OWNER_CAPABILITY_MEMBER: return "capability_member";
        case SOL_CONTRACT_OWNER_TYPE: return "type";
    }
    return NULL;
}

static const char *loop_obligation_name(SolLoopObligationKind kind) {
    switch (kind) {
        case SOL_LOOP_OBLIGATION_INVARIANT_ENTRY: return "invariant_entry";
        case SOL_LOOP_OBLIGATION_INVARIANT_PRESERVATION:
            return "invariant_preservation";
        case SOL_LOOP_OBLIGATION_DECREASES_NONNEGATIVE:
            return "decreases_nonnegative";
        case SOL_LOOP_OBLIGATION_DECREASES_STRICT: return "decreases_strict";
    }
    return NULL;
}

static void render_type_ids(MirRenderer *renderer, SolIrSlice slice) {
    render_format(&renderer->output, "%zu[", slice.count);
    for (size_t index = 0; index < slice.count; ++index) {
        if (index != 0) render_text(&renderer->output, ",");
        render_id(&renderer->output, "ty",
            renderer->ir->type_ids[slice.offset + index], SOL_IR_NONE);
    }
    render_text(&renderer->output, "]");
}

static void render_roots(MirRenderer *renderer, SolIrSlice slice) {
    render_format(&renderer->output, "%zu[", slice.count);
    for (size_t index = 0; index < slice.count; ++index) {
        if (index != 0) render_text(&renderer->output, ",");
        SolIrLocalId local = renderer->ir->roots[slice.offset + index];
        render_id(&renderer->output, "local", local, SOL_IR_NONE);
        if (local < renderer->ir->local_count) {
            render_text(&renderer->output, "(");
            render_quoted(&renderer->output, renderer->ir->locals[local].name);
            render_text(&renderer->output, ")");
        }
    }
    render_text(&renderer->output, "]");
}

static void render_effects(MirRenderer *renderer, SolIrSlice slice) {
    render_format(&renderer->output, "%zu[", slice.count);
    for (size_t index = 0; index < slice.count; ++index) {
        if (index != 0) render_text(&renderer->output, ",");
        size_t id = slice.offset + index;
        const SolIrEffect *effect = &renderer->ir->effects[id];
        render_format(&renderer->output, "effect%zu{name=", id);
        render_quoted(&renderer->output, effect->name);
        render_text(&renderer->output, " authority_kind=");
        render_enum(&renderer->output,
            authority_name(effect->authority_kind));
        render_text(&renderer->output, " authority=");
        render_id(&renderer->output, "local", effect->authority, SOL_IR_NONE);
        render_text(&renderer->output, "}");
    }
    render_text(&renderer->output, "]");
}

static void render_evidence(MirRenderer *renderer, SolIrSlice slice) {
    render_format(&renderer->output, "%zu[", slice.count);
    for (size_t index = 0; index < slice.count; ++index) {
        if (index != 0) render_text(&renderer->output, ",");
        size_t id = slice.offset + index;
        const SolIrDispatchEvidence *evidence = &renderer->ir->evidence[id];
        render_format(&renderer->output, "evidence%zu{trait=", id);
        render_id(&renderer->output, "d", evidence->trait, SOL_IR_NONE);
        render_text(&renderer->output, " requirement=");
        render_id(&renderer->output, "c", evidence->requirement, SOL_IR_NONE);
        render_text(&renderer->output, " implementation=");
        render_id(&renderer->output, "d", evidence->implementation,
            SOL_IR_NONE);
        render_text(&renderer->output, " method=");
        render_id(&renderer->output, "c", evidence->method, SOL_IR_NONE);
        render_text(&renderer->output, " ty=");
        render_id(&renderer->output, "ty", evidence->type, SOL_IR_NONE);
        render_text(&renderer->output, " binding=");
        render_id(&renderer->output, "gp", evidence->binding, SOL_IR_NONE);
        render_text(&renderer->output, " parameter=");
        render_id(&renderer->output, "gp", evidence->parameter, SOL_IR_NONE);
        render_format(&renderer->output, " forwarded=%s}",
            evidence->forwarded ? "true" : "false");
    }
    render_text(&renderer->output, "]");
}

static void render_source_place(MirRenderer *renderer, SolIrPlaceId id) {
    MirRenderBuffer *output = &renderer->output;
    const SolIr *ir = renderer->ir;
    if (id == SOL_IR_NONE) {
        render_text(output, "none");
        return;
    }
    if (id >= ir->place_count) {
        output->failed = true;
        return;
    }
    const SolIrPlace *place = &ir->places[id];
    render_format(output, "place%zu{root=", id);
    if (place->root_kind == SOL_IR_PLACE_ROOT_LOCAL
        && place->local < ir->local_count) {
        render_format(output, "local%zu(", place->local);
        render_quoted(output, ir->locals[place->local].name);
        render_format(output, ") path=local%zu", place->local);
    } else if (place->root_kind == SOL_IR_PLACE_ROOT_TEMPORARY
        && place->temporary < ir->expression_count) {
        render_format(output, "temporary(expr%zu) path=expr%zu",
            place->temporary, place->temporary);
    } else {
        output->failed = true;
        return;
    }
    for (size_t index = 0; index < place->projections.count; ++index) {
        const SolIrProjection *projection
            = &ir->projections[place->projections.offset + index];
        if (projection->kind == SOL_IR_PROJECTION_FIELD) {
            if (projection->field >= ir->field_count) {
                output->failed = true;
                return;
            }
            render_format(output, ".field%zu(", projection->field);
            render_quoted(output, ir->fields[projection->field].name);
            render_text(output, ")");
        } else if (projection->kind == SOL_IR_PROJECTION_TUPLE_FIELD) {
            render_format(output, ".tuple%zu", projection->ordinal);
        } else if (projection->kind == SOL_IR_PROJECTION_INDEX) {
            render_format(output, ".index(expr%zu)", projection->index);
        } else if (projection->kind == SOL_IR_PROJECTION_DEREFERENCE) {
            render_text(output, ".dereference");
        } else {
            output->failed = true;
            return;
        }
    }
    render_text(output, " ty=");
    render_id(output, "ty", place->type, SOL_IR_NONE);
    render_text(output, " root_span=");
    render_span(output, place->root_span);
    render_format(output, " projections=%zu[", place->projections.count);
    for (size_t index = 0; index < place->projections.count; ++index) {
        if (index != 0) render_text(output, ",");
        size_t projection_id = place->projections.offset + index;
        const SolIrProjection *projection = &ir->projections[projection_id];
        render_format(output, "projection%zu{kind=", projection_id);
        if (projection->kind == SOL_IR_PROJECTION_FIELD) {
            render_text(output, "field field=");
            render_id(output, "field", projection->field, SOL_IR_NONE);
        } else if (projection->kind == SOL_IR_PROJECTION_TUPLE_FIELD) {
            render_format(output, "tuple_field ordinal=%zu",
                projection->ordinal);
        } else if (projection->kind == SOL_IR_PROJECTION_INDEX) {
            render_text(output, "index expression=");
            render_id(output, "expr", projection->index, SOL_IR_NONE);
        } else if (projection->kind == SOL_IR_PROJECTION_DEREFERENCE) {
            render_text(output, "dereference");
        } else {
            output->failed = true;
            return;
        }
        render_text(output, " ty=");
        render_id(output, "ty", projection->type, SOL_IR_NONE);
        render_text(output, " span=");
        render_span(output, projection->span);
        render_text(output, "}");
    }
    render_text(output, "]}");
}

static void render_mir_place(MirRenderer *renderer, SolMirPlace place) {
    render_text(&renderer->output, "{local=");
    render_id(&renderer->output, "local", place.local, SOL_IR_NONE);
    render_text(&renderer->output, " source=");
    render_source_place(renderer, place.source_place);
    render_text(&renderer->output, "}");
}

static void render_value_slice(MirRenderer *renderer, SolMirSlice slice,
    bool parameters) {
    render_format(&renderer->output, "%zu[", slice.count);
    for (size_t index = 0; index < slice.count; ++index) {
        if (index != 0) render_text(&renderer->output, ",");
        SolMirValueId value = parameters
            ? renderer->mir->parameter_values[slice.offset + index]
            : renderer->mir->edge_values[slice.offset + index];
        render_id(&renderer->output, "v", value, SOL_MIR_NONE);
    }
    render_text(&renderer->output, "]");
}

static void render_edge(MirRenderer *renderer, SolMirEdge edge) {
    render_id(&renderer->output, "b", edge.block, SOL_MIR_NONE);
    render_text(&renderer->output, "(args=");
    render_value_slice(renderer, edge.arguments, false);
    render_text(&renderer->output, ")");
}

static void render_call_argument(MirRenderer *renderer,
    const SolMirCallArgument *argument) {
    render_text(&renderer->output, "{formal=");
    if (argument->formal == SOL_IR_NONE) render_text(&renderer->output, "none");
    else render_format(&renderer->output, "%zu", argument->formal);
    render_text(&renderer->output, " access=");
    render_enum(&renderer->output, access_name(argument->access));
    render_text(&renderer->output, " source=");
    render_id(&renderer->output, "expr", argument->source_expression,
        SOL_IR_NONE);
    render_text(&renderer->output, " temporary=");
    render_id(&renderer->output, "t", argument->temporary, SOL_MIR_NONE);
    render_text(&renderer->output, " place=");
    render_source_place(renderer, argument->place);
    render_text(&renderer->output, "}");
}

static void render_call_arguments(MirRenderer *renderer, SolMirSlice slice) {
    render_format(&renderer->output, "%zu[", slice.count);
    for (size_t index = 0; index < slice.count; ++index) {
        if (index != 0) render_text(&renderer->output, ",");
        size_t id = slice.offset + index;
        render_format(&renderer->output, "arg%zu", id);
        render_call_argument(renderer, &renderer->mir->call_arguments[id]);
    }
    render_text(&renderer->output, "]");
}

static void render_construct_operands(MirRenderer *renderer,
    SolMirSlice slice) {
    render_format(&renderer->output, "%zu[", slice.count);
    for (size_t index = 0; index < slice.count; ++index) {
        if (index != 0) render_text(&renderer->output, ",");
        size_t id = slice.offset + index;
        const SolMirConstructOperand *operand
            = &renderer->mir->construct_operands[id];
        render_format(&renderer->output, "operand%zu{formal=%zu source=", id,
            operand->formal);
        render_id(&renderer->output, "expr", operand->source_expression,
            SOL_IR_NONE);
        render_text(&renderer->output, " temporary=");
        render_id(&renderer->output, "t", operand->temporary, SOL_MIR_NONE);
        render_text(&renderer->output, "}");
    }
    render_text(&renderer->output, "]");
}

static bool mark_relation(unsigned char *marks, size_t count, size_t id,
    size_t none, bool *changed) {
    if (id == none) return true;
    if (id >= count) return false;
    if (marks[id] == 0) {
        marks[id] = 1;
        *changed = true;
    }
    return true;
}

static bool mark_expression(MirRenderer *renderer, SolIrExpressionId id,
    bool *changed) {
    return mark_relation(renderer->expressions, renderer->ir->expression_count,
        id, SOL_IR_NONE, changed);
}

static bool mark_statement(MirRenderer *renderer, SolIrStatementId id,
    bool *changed) {
    return mark_relation(renderer->statements, renderer->ir->statement_count,
        id, SOL_IR_NONE, changed);
}

static bool mark_pattern(MirRenderer *renderer, SolIrPatternId id,
    bool *changed) {
    return mark_relation(renderer->patterns, renderer->ir->pattern_count,
        id, SOL_IR_NONE, changed);
}

static bool mark_arm(MirRenderer *renderer, SolIrArmId id, bool *changed) {
    return mark_relation(renderer->arms, renderer->ir->arm_count, id,
        SOL_IR_NONE, changed);
}

static bool mark_snapshot(MirRenderer *renderer, SolIrSnapshotId id,
    bool *changed) {
    return mark_relation(renderer->snapshots, renderer->ir->snapshot_count, id,
        SOL_IR_NONE, changed);
}

static bool mark_obligation(MirRenderer *renderer, SolObligationId id,
    bool *changed) {
    if (id == UINT64_MAX || id > SIZE_MAX) return id == UINT64_MAX;
    return mark_relation(renderer->obligations, renderer->ir->obligation_count,
        (size_t)id, SIZE_MAX, changed);
}

static bool collect_direct_relations(MirRenderer *renderer) {
    const SolMir *mir = renderer->mir;
    bool changed = false;
    if (!mark_expression(renderer,
        renderer->ir->callables[mir->callable].body, &changed)) return false;
    for (size_t id = 0; id < mir->value_count; ++id) {
        if (!mark_expression(renderer, mir->values[id].source_expression,
            &changed)) return false;
    }
    for (size_t id = 0; id < mir->temporary_count; ++id) {
        if (!mark_expression(renderer, mir->temporaries[id].source_expression,
            &changed)) return false;
    }
    for (size_t id = 0; id < mir->construct_operand_count; ++id) {
        if (!mark_expression(renderer,
            mir->construct_operands[id].source_expression, &changed)) {
            return false;
        }
    }
    for (size_t id = 0; id < mir->call_argument_count; ++id) {
        if (!mark_expression(renderer, mir->call_arguments[id].source_expression,
            &changed)) return false;
    }
    for (size_t id = 0; id < mir->loop_count; ++id) {
        const SolMirLoop *loop = &mir->loops[id];
        if (!mark_statement(renderer, loop->statement, &changed)) return false;
        for (size_t index = 0; index < loop->obligations.count; ++index) {
            const SolIrLoopObligation *obligation
                = &renderer->ir->loop_obligations[
                    loop->obligations.offset + index];
            if (!mark_expression(renderer, obligation->expression, &changed)) {
                return false;
            }
        }
    }
    for (size_t id = 0; id < mir->instruction_count; ++id) {
        const SolMirInstruction *instruction = &mir->instructions[id];
        if (!mark_expression(renderer, instruction->source_expression,
            &changed)) return false;
        switch (instruction->kind) {
            case SOL_MIR_INST_LOAD_UPDATE:
                if (!mark_statement(renderer,
                    instruction->as.update_load.statement, &changed)) {
                    return false;
                }
                break;
            case SOL_MIR_INST_COMPOUND_UPDATE:
                if (!mark_statement(renderer,
                    instruction->as.compound_update.statement, &changed)) {
                    return false;
                }
                break;
            case SOL_MIR_INST_REGION_ENTER:
            case SOL_MIR_INST_REGION_EXIT:
                if (!mark_statement(renderer, instruction->as.region,
                    &changed)) return false;
                break;
            case SOL_MIR_INST_PATTERN_TEST:
            case SOL_MIR_INST_PATTERN_VALUE:
            case SOL_MIR_INST_MATCH_ARM:
                if (!mark_expression(renderer,
                        instruction->as.pattern.match_expression, &changed)
                    || !mark_arm(renderer, instruction->as.pattern.arm, &changed)
                    || !mark_pattern(renderer, instruction->as.pattern.pattern,
                        &changed)) return false;
                break;
            case SOL_MIR_INST_CAPTURE_SNAPSHOT:
                if (!mark_snapshot(renderer, instruction->as.snapshot,
                    &changed)) return false;
                break;
            default: break;
        }
    }
    for (size_t id = 0; id < mir->block_count; ++id) {
        const SolMirTerminator *term = &mir->blocks[id].terminator;
        switch (term->kind) {
            case SOL_MIR_TERM_INVOKE:
                if (!mark_expression(renderer,
                        term->as.invoke.source_expression, &changed)
                    || !mark_expression(renderer,
                        term->as.invoke.receiver.source_expression, &changed)) {
                    return false;
                }
                break;
            case SOL_MIR_TERM_UNREACHABLE: {
                if (!mark_statement(renderer, term->as.unreachable.statement,
                    &changed)) return false;
                const SolIrUnreachableObligation *obligation
                    = &renderer->ir->unreachable_obligations[
                        term->as.unreachable.obligation];
                if (!mark_expression(renderer, obligation->proof, &changed)) {
                    return false;
                }
                break;
            }
            case SOL_MIR_TERM_BREAK:
            case SOL_MIR_TERM_CONTINUE:
                if (!mark_statement(renderer, term->as.transfer.statement,
                    &changed)) return false;
                break;
            case SOL_MIR_TERM_CHECK_REFINED:
                if (!mark_expression(renderer,
                        term->as.check_refined.source_expression, &changed)
                    || !mark_obligation(renderer,
                        term->as.check_refined.obligation, &changed)) return false;
                break;
            case SOL_MIR_TERM_MATCH_FAILURE:
                if (!mark_expression(renderer, term->as.match_failure,
                    &changed)) return false;
                break;
            case SOL_MIR_TERM_PROPAGATE:
                if (!mark_expression(renderer,
                    term->as.propagate.source_expression, &changed)) return false;
                break;
            case SOL_MIR_TERM_CHECK_CONTRACT:
                if (!mark_obligation(renderer,
                    term->as.check_contract.obligation, &changed)) return false;
                break;
            case SOL_MIR_TERM_CONTRACT_VIOLATION:
                if (!mark_obligation(renderer, term->as.contract_violation,
                    &changed)) return false;
                break;
            default: break;
        }
    }
    return true;
}

static bool collect_expression_relations(MirRenderer *renderer, size_t id,
    bool *changed) {
    const SolIr *ir = renderer->ir;
    const SolIrExpression *expression = &ir->expressions[id];
    switch (expression->kind) {
        case SOL_IR_EXPR_PLACE: {
            const SolIrPlace *place = &ir->places[expression->as.place];
            if (place->root_kind == SOL_IR_PLACE_ROOT_TEMPORARY
                && !mark_expression(renderer, place->temporary, changed)) {
                return false;
            }
            for (size_t index = 0; index < place->projections.count; ++index) {
                const SolIrProjection *projection
                    = &ir->projections[place->projections.offset + index];
                if (projection->kind == SOL_IR_PROJECTION_INDEX
                    && !mark_expression(renderer, projection->index, changed)) {
                    return false;
                }
            }
            return true;
        }
        case SOL_IR_EXPR_UNARY:
            return mark_expression(renderer, expression->as.unary.operand,
                changed);
        case SOL_IR_EXPR_BINARY:
            return mark_expression(renderer, expression->as.binary.left,
                    changed)
                && mark_expression(renderer, expression->as.binary.right,
                    changed);
        case SOL_IR_EXPR_CALL:
            if ((expression->as.call.kind == SOL_IR_CALL_CALLBACK
                    || expression->as.call.kind == SOL_IR_CALL_CAPABILITY)
                && !mark_expression(renderer, expression->as.call.callee,
                    changed)) return false;
            if (expression->as.call.kind == SOL_IR_CALL_METHOD
                && !mark_expression(renderer, expression->as.call.receiver,
                    changed)) return false;
            for (size_t index = 0; index < expression->as.call.operands.count;
                ++index) {
                if (!mark_expression(renderer, ir->operands[
                        expression->as.call.operands.offset + index].value,
                    changed)) return false;
            }
            return true;
        case SOL_IR_EXPR_RECORD:
            for (size_t index = 0; index < expression->as.record.fields.count;
                ++index) {
                if (!mark_expression(renderer, ir->operands[
                        expression->as.record.fields.offset + index].value,
                    changed)) return false;
            }
            return true;
        case SOL_IR_EXPR_TUPLE:
            for (size_t index = 0; index < expression->as.tuple.operands.count;
                ++index) {
                if (!mark_expression(renderer, ir->operands[
                        expression->as.tuple.operands.offset + index].value,
                    changed)) return false;
            }
            return true;
        case SOL_IR_EXPR_BOUND_OPERATION:
            return mark_expression(renderer, expression->as.operation.receiver,
                changed);
        case SOL_IR_EXPR_IF:
            return mark_expression(renderer, expression->as.if_expr.condition,
                    changed)
                && mark_expression(renderer,
                    expression->as.if_expr.then_branch, changed)
                && mark_expression(renderer,
                    expression->as.if_expr.else_branch, changed);
        case SOL_IR_EXPR_MATCH:
            if (!mark_expression(renderer,
                expression->as.match_expr.scrutinee, changed)) return false;
            for (size_t index = 0; index < expression->as.match_expr.arms.count;
                ++index) {
                if (!mark_arm(renderer, ir->arm_ids[
                        expression->as.match_expr.arms.offset + index],
                    changed)) return false;
            }
            return true;
        case SOL_IR_EXPR_BLOCK:
            for (size_t index = 0; index < expression->as.block.statements.count;
                ++index) {
                if (!mark_statement(renderer, ir->statement_ids[
                        expression->as.block.statements.offset + index],
                    changed)) return false;
            }
            return true;
        case SOL_IR_EXPR_PROPAGATE:
            return mark_expression(renderer, expression->as.propagate.operand,
                changed);
        case SOL_IR_EXPR_HANDLE:
            return mark_expression(renderer, expression->as.handler.authority,
                    changed)
                && mark_expression(renderer, expression->as.handler.provider,
                    changed)
                && mark_expression(renderer, expression->as.handler.body,
                    changed);
        case SOL_IR_EXPR_SNAPSHOT_READ:
            return mark_snapshot(renderer, expression->as.snapshot, changed);
        default: return true;
    }
}

static bool collect_source_relations(MirRenderer *renderer) {
    if (!collect_direct_relations(renderer)) return false;
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t id = 0; id < renderer->ir->expression_count; ++id) {
            if (renderer->expressions[id] != 0
                && !collect_expression_relations(renderer, id, &changed)) {
                return false;
            }
        }
        for (size_t id = 0; id < renderer->ir->statement_count; ++id) {
            if (renderer->statements[id] == 0) continue;
            const SolIrStatement *statement = &renderer->ir->statements[id];
            switch (statement->kind) {
                case SOL_IR_STATEMENT_LET:
                case SOL_IR_STATEMENT_RETURN:
                case SOL_IR_STATEMENT_EXPRESSION:
                case SOL_IR_STATEMENT_REGION:
                case SOL_IR_STATEMENT_PANIC:
                    if (!mark_expression(renderer, statement->expression,
                        &changed)) return false;
                    break;
                case SOL_IR_STATEMENT_ASSIGNMENT:
                case SOL_IR_STATEMENT_MODIFY:
                    if (!mark_expression(renderer, statement->target, &changed)
                        || !mark_expression(renderer, statement->expression,
                            &changed)) return false;
                    break;
                case SOL_IR_STATEMENT_WHILE:
                    if (!mark_expression(renderer, statement->condition,
                            &changed)
                        || !mark_expression(renderer, statement->expression,
                            &changed)) return false;
                    break;
                case SOL_IR_STATEMENT_LOOP:
                    if (!mark_expression(renderer, statement->expression,
                        &changed)) return false;
                    break;
                case SOL_IR_STATEMENT_REQUIRE:
                    if (!mark_expression(renderer, statement->condition,
                            &changed)
                        || !mark_expression(renderer, statement->expression,
                            &changed)) return false;
                    break;
                case SOL_IR_STATEMENT_UNREACHABLE:
                    for (size_t index = 0;
                        index < statement->unreachable_obligations.count;
                        ++index) {
                        const SolIrUnreachableObligation *obligation
                            = &renderer->ir->unreachable_obligations[
                                statement->unreachable_obligations.offset
                                    + index];
                        if (!mark_expression(renderer, obligation->proof,
                            &changed)) return false;
                    }
                    break;
                default: break;
            }
            for (size_t index = 0; index < statement->loop_obligations.count;
                ++index) {
                if (!mark_expression(renderer,
                    renderer->ir->loop_obligations[
                        statement->loop_obligations.offset + index].expression,
                    &changed)) return false;
            }
        }
        for (size_t id = 0; id < renderer->ir->pattern_count; ++id) {
            if (renderer->patterns[id] == 0) continue;
            const SolIrPattern *pattern = &renderer->ir->patterns[id];
            for (size_t index = 0; index < pattern->children.count; ++index) {
                if (!mark_pattern(renderer, renderer->ir->pattern_children[
                        pattern->children.offset + index].pattern, &changed)) {
                    return false;
                }
            }
        }
        for (size_t id = 0; id < renderer->ir->arm_count; ++id) {
            if (renderer->arms[id] == 0) continue;
            const SolIrArm *arm = &renderer->ir->arms[id];
            if (!mark_pattern(renderer, arm->pattern, &changed)
                || !mark_expression(renderer, arm->guard, &changed)
                || !mark_expression(renderer, arm->body, &changed)) return false;
        }
        for (size_t id = 0; id < renderer->ir->snapshot_count; ++id) {
            if (renderer->snapshots[id] == 0) continue;
            const SolIrSnapshot *snapshot = &renderer->ir->snapshots[id];
            if (!mark_obligation(renderer, snapshot->obligation, &changed)
                || !mark_expression(renderer, snapshot->read, &changed)
                || !mark_expression(renderer, snapshot->operand, &changed)) {
                return false;
            }
        }
        for (size_t id = 0; id < renderer->ir->obligation_count; ++id) {
            if (renderer->obligations[id] == 0) continue;
            const SolIrObligation *obligation = &renderer->ir->obligations[id];
            if (!mark_expression(renderer, obligation->predicate, &changed)) {
                return false;
            }
            for (size_t index = 0; index < obligation->snapshots.count;
                ++index) {
                if (!mark_snapshot(renderer, obligation->snapshots.offset + index,
                    &changed)) return false;
            }
        }
    }
    return true;
}

static void render_cleanup_locals(MirRenderer *renderer, SolIrSlice slice) {
    render_format(&renderer->output, "%zu[", slice.count);
    for (size_t index = 0; index < slice.count; ++index) {
        if (index != 0) render_text(&renderer->output, ",");
        render_id(&renderer->output, "local",
            renderer->ir->cleanup_locals[slice.offset + index], SOL_IR_NONE);
    }
    render_text(&renderer->output, "]");
}

static void render_ir_operands(MirRenderer *renderer, SolIrSlice slice) {
    render_format(&renderer->output, "%zu[", slice.count);
    for (size_t index = 0; index < slice.count; ++index) {
        if (index != 0) render_text(&renderer->output, ",");
        size_t id = slice.offset + index;
        const SolIrOperand *operand = &renderer->ir->operands[id];
        render_format(&renderer->output, "ir_operand%zu{formal=%zu value=", id,
            operand->formal);
        render_id(&renderer->output, "expr", operand->value, SOL_IR_NONE);
        render_text(&renderer->output, " access=");
        render_enum(&renderer->output, access_name(operand->access));
        render_text(&renderer->output, "}");
    }
    render_text(&renderer->output, "]");
}

static void render_statement_ids(MirRenderer *renderer, SolIrSlice slice) {
    render_format(&renderer->output, "%zu[", slice.count);
    for (size_t index = 0; index < slice.count; ++index) {
        if (index != 0) render_text(&renderer->output, ",");
        render_id(&renderer->output, "stmt",
            renderer->ir->statement_ids[slice.offset + index], SOL_IR_NONE);
    }
    render_text(&renderer->output, "]");
}

static void render_arm_ids(MirRenderer *renderer, SolIrSlice slice) {
    render_format(&renderer->output, "%zu[", slice.count);
    for (size_t index = 0; index < slice.count; ++index) {
        if (index != 0) render_text(&renderer->output, ",");
        render_id(&renderer->output, "arm",
            renderer->ir->arm_ids[slice.offset + index], SOL_IR_NONE);
    }
    render_text(&renderer->output, "]");
}

static void render_loop_obligations(MirRenderer *renderer, SolIrSlice slice) {
    render_format(&renderer->output, "%zu[", slice.count);
    for (size_t index = 0; index < slice.count; ++index) {
        if (index != 0) render_text(&renderer->output, ",");
        size_t arena_id = slice.offset + index;
        const SolIrLoopObligation *obligation
            = &renderer->ir->loop_obligations[arena_id];
        render_format(&renderer->output,
            "loop_obligation%zu{id=%zu kind=", arena_id, obligation->id);
        render_enum(&renderer->output, loop_obligation_name(obligation->kind));
        render_text(&renderer->output, " statement=");
        render_id(&renderer->output, "stmt", obligation->loop_statement,
            SOL_IR_NONE);
        render_text(&renderer->output, " callable=");
        render_id(&renderer->output, "c", obligation->callable, SOL_IR_NONE);
        render_text(&renderer->output, " expression=");
        render_id(&renderer->output, "expr", obligation->expression,
            SOL_IR_NONE);
        render_text(&renderer->output, " expression_ty=");
        render_id(&renderer->output, "ty", obligation->expression_type,
            SOL_IR_NONE);
        render_text(&renderer->output, " span=");
        render_span(&renderer->output, obligation->span);
        render_text(&renderer->output, "}");
    }
    render_text(&renderer->output, "]");
}

static void render_unreachable_obligations(MirRenderer *renderer,
    SolIrSlice slice) {
    render_format(&renderer->output, "%zu[", slice.count);
    for (size_t index = 0; index < slice.count; ++index) {
        if (index != 0) render_text(&renderer->output, ",");
        size_t arena_id = slice.offset + index;
        const SolIrUnreachableObligation *obligation
            = &renderer->ir->unreachable_obligations[arena_id];
        render_format(&renderer->output,
            "unreachable_obligation%zu{id=%zu statement=", arena_id,
            obligation->id);
        render_id(&renderer->output, "stmt", obligation->statement,
            SOL_IR_NONE);
        render_text(&renderer->output, " callable=");
        render_id(&renderer->output, "c", obligation->callable, SOL_IR_NONE);
        render_text(&renderer->output, " proof=");
        render_id(&renderer->output, "expr", obligation->proof, SOL_IR_NONE);
        render_text(&renderer->output, " proof_ty=");
        render_id(&renderer->output, "ty", obligation->proof_type, SOL_IR_NONE);
        render_text(&renderer->output, " span=");
        render_span(&renderer->output, obligation->span);
        render_text(&renderer->output, "}");
    }
    render_text(&renderer->output, "]");
}

static void render_source_expression(MirRenderer *renderer, size_t id) {
    MirRenderBuffer *output = &renderer->output;
    const SolIrExpression *expression = &renderer->ir->expressions[id];
    render_format(output, "source_expression expr%zu kind=", id);
    render_enum(output, expression_kind_name(expression->kind));
    render_text(output, " local_use=");
    render_enum(output, local_use_name(expression->local_use));
    render_text(output, " ty=");
    render_id(output, "ty", expression->type, SOL_IR_NONE);
    render_text(output, " capability_roots=");
    render_roots(renderer, expression->capability_roots);
    render_text(output, " operation_roots=");
    render_roots(renderer, expression->operation_roots);
    render_text(output, " span=");
    render_span(output, expression->span);
    switch (expression->kind) {
        case SOL_IR_EXPR_INTEGER:
            render_format(output, " value=%" PRId64, expression->as.integer);
            break;
        case SOL_IR_EXPR_STRING:
            render_text(output, " text=");
            render_quoted(output, expression->as.string);
            break;
        case SOL_IR_EXPR_BOOL:
            render_format(output, " value=%s",
                expression->as.boolean ? "true" : "false");
            break;
        case SOL_IR_EXPR_PLACE:
            render_text(output, " place=");
            render_source_place(renderer, expression->as.place);
            break;
        case SOL_IR_EXPR_DEFINITION:
            render_text(output, " definition=");
            render_id(output, "d", expression->as.definition, SOL_IR_NONE);
            break;
        case SOL_IR_EXPR_UNARY:
            render_text(output, " operator=");
            render_enum(output, operator_name(expression->as.unary.operator_kind));
            render_text(output, " operand=");
            render_id(output, "expr", expression->as.unary.operand, SOL_IR_NONE);
            break;
        case SOL_IR_EXPR_BINARY:
            render_text(output, " left=");
            render_id(output, "expr", expression->as.binary.left, SOL_IR_NONE);
            render_text(output, " operator=");
            render_enum(output,
                operator_name(expression->as.binary.operator_kind));
            render_text(output, " right=");
            render_id(output, "expr", expression->as.binary.right, SOL_IR_NONE);
            break;
        case SOL_IR_EXPR_CALL:
            render_text(output, " call_kind=");
            render_enum(output, call_kind_name(expression->as.call.kind));
            render_text(output, " callable=");
            if (expression->as.call.kind == SOL_IR_CALL_FUNCTION
                || expression->as.call.kind == SOL_IR_CALL_CAPABILITY
                || expression->as.call.kind == SOL_IR_CALL_METHOD) {
                render_id(output, "c", expression->as.call.callable,
                    SOL_IR_NONE);
            } else render_text(output, "none");
            render_text(output, " callee=");
            if (expression->as.call.kind == SOL_IR_CALL_CALLBACK
                || expression->as.call.kind == SOL_IR_CALL_CAPABILITY) {
                render_id(output, "expr", expression->as.call.callee,
                    SOL_IR_NONE);
            } else render_text(output, "none");
            render_text(output, " receiver=");
            if (expression->as.call.kind == SOL_IR_CALL_METHOD) {
                render_id(output, "expr", expression->as.call.receiver,
                    SOL_IR_NONE);
            } else render_text(output, "none");
            render_text(output, " receiver_access=");
            if (expression->as.call.kind == SOL_IR_CALL_METHOD
                || expression->as.call.kind == SOL_IR_CALL_CAPABILITY) {
                render_enum(output,
                    access_name(expression->as.call.receiver_access));
            } else render_text(output, "none");
            render_text(output, " variant=");
            if (expression->as.call.kind == SOL_IR_CALL_ENUM_CONSTRUCTOR) {
                render_id(output, "variant", expression->as.call.variant,
                    SOL_IR_NONE);
            } else render_text(output, "none");
            render_text(output, " definition=");
            if (expression->as.call.kind == SOL_IR_CALL_ENUM_CONSTRUCTOR
                || expression->as.call.kind
                    == SOL_IR_CALL_DISTINCT_CONSTRUCTOR) {
                render_id(output, "d", expression->as.call.definition,
                    SOL_IR_NONE);
            } else render_text(output, "none");
            render_text(output, " operands=");
            render_ir_operands(renderer, expression->as.call.operands);
            render_text(output, " type_arguments=");
            render_type_ids(renderer, expression->as.call.type_arguments);
            render_text(output, " effects=");
            render_effects(renderer, expression->as.call.effects);
            render_text(output, " effect_parameter=");
            render_id(output, "ep", expression->as.call.effect_parameter,
                SOL_IR_NONE);
            render_text(output, " evidence=");
            render_evidence(renderer, expression->as.call.evidence);
            break;
        case SOL_IR_EXPR_RECORD:
            render_text(output, " definition=");
            render_id(output, "d", expression->as.record.definition,
                SOL_IR_NONE);
            render_text(output, " fields=");
            render_ir_operands(renderer, expression->as.record.fields);
            break;
        case SOL_IR_EXPR_TUPLE:
            render_text(output, " operands=");
            render_ir_operands(renderer, expression->as.tuple.operands);
            break;
        case SOL_IR_EXPR_VARIANT:
            render_text(output, " variant=");
            render_id(output, "variant", expression->as.variant.variant,
                SOL_IR_NONE);
            break;
        case SOL_IR_EXPR_BOUND_OPERATION:
            render_text(output, " receiver=");
            render_id(output, "expr", expression->as.operation.receiver,
                SOL_IR_NONE);
            render_text(output, " callable=");
            render_id(output, "c", expression->as.operation.callable,
                SOL_IR_NONE);
            break;
        case SOL_IR_EXPR_IF:
            render_text(output, " condition=");
            render_id(output, "expr", expression->as.if_expr.condition,
                SOL_IR_NONE);
            render_text(output, " then=");
            render_id(output, "expr", expression->as.if_expr.then_branch,
                SOL_IR_NONE);
            render_text(output, " else=");
            render_id(output, "expr", expression->as.if_expr.else_branch,
                SOL_IR_NONE);
            break;
        case SOL_IR_EXPR_MATCH:
            render_text(output, " scrutinee=");
            render_id(output, "expr", expression->as.match_expr.scrutinee,
                SOL_IR_NONE);
            render_text(output, " arms=");
            render_arm_ids(renderer, expression->as.match_expr.arms);
            break;
        case SOL_IR_EXPR_BLOCK:
            render_text(output, " statements=");
            render_statement_ids(renderer, expression->as.block.statements);
            render_text(output, " cleanup=");
            render_cleanup_locals(renderer, expression->as.block.cleanup);
            break;
        case SOL_IR_EXPR_PROPAGATE:
            render_text(output, " propagation_kind=");
            render_enum(output, propagation_name(expression->as.propagate.kind));
            render_text(output, " operand=");
            render_id(output, "expr", expression->as.propagate.operand,
                SOL_IR_NONE);
            render_text(output, " residual_ty=");
            render_id(output, "ty", expression->as.propagate.residual,
                SOL_IR_NONE);
            break;
        case SOL_IR_EXPR_HANDLE:
            render_text(output, " effect_name=");
            render_quoted(output, expression->as.handler.effect_name);
            render_text(output, " authority=");
            render_id(output, "expr", expression->as.handler.authority,
                SOL_IR_NONE);
            render_text(output, " provider=");
            render_id(output, "expr", expression->as.handler.provider,
                SOL_IR_NONE);
            render_text(output, " body=");
            render_id(output, "expr", expression->as.handler.body, SOL_IR_NONE);
            render_text(output, " source_callable=");
            render_id(output, "c", expression->as.handler.source, SOL_IR_NONE);
            render_text(output, " provider_callable=");
            render_id(output, "c", expression->as.handler.provider_callable,
                SOL_IR_NONE);
            render_text(output, " root=");
            render_id(output, "local", expression->as.handler.root,
                SOL_IR_NONE);
            break;
        case SOL_IR_EXPR_SNAPSHOT_READ:
            render_text(output, " snapshot=");
            render_id(output, "snapshot", expression->as.snapshot, SOL_IR_NONE);
            break;
        case SOL_IR_EXPR_REFINEMENT_SELF:
            render_text(output, " definition=");
            render_id(output, "d", expression->as.definition, SOL_IR_NONE);
            break;
        case SOL_IR_EXPR_UNIT:
        case SOL_IR_EXPR_RESULT:
        case SOL_IR_EXPR_COMPILE_TIME_HEAD:
            break;
    }
    render_text(output, "\n");
}

static void render_source_statement(MirRenderer *renderer, size_t id) {
    MirRenderBuffer *output = &renderer->output;
    const SolIrStatement *statement = &renderer->ir->statements[id];
    render_format(output, "source_statement stmt%zu kind=", id);
    render_enum(output, statement_kind_name(statement->kind));
    render_text(output, " span=");
    render_span(output, statement->span);
    switch (statement->kind) {
        case SOL_IR_STATEMENT_LET:
            render_text(output, " local=");
            render_id(output, "local", statement->local, SOL_IR_NONE);
            render_text(output, " expression=");
            render_id(output, "expr", statement->expression, SOL_IR_NONE);
            break;
        case SOL_IR_STATEMENT_DECLARE:
            render_text(output, " local=");
            render_id(output, "local", statement->local, SOL_IR_NONE);
            break;
        case SOL_IR_STATEMENT_ASSIGNMENT:
            render_text(output, " target=");
            render_id(output, "expr", statement->target, SOL_IR_NONE);
            render_text(output, " expression=");
            render_id(output, "expr", statement->expression, SOL_IR_NONE);
            render_text(output, " operator=");
            render_enum(output, operator_name(statement->operator_kind));
            break;
        case SOL_IR_STATEMENT_RETURN:
        case SOL_IR_STATEMENT_EXPRESSION:
        case SOL_IR_STATEMENT_PANIC:
            render_text(output, " expression=");
            render_id(output, "expr", statement->expression, SOL_IR_NONE);
            break;
        case SOL_IR_STATEMENT_REGION:
            render_text(output, " expression=");
            render_id(output, "expr", statement->expression, SOL_IR_NONE);
            render_text(output, " label=");
            render_quoted(output, statement->region_label);
            render_text(output, " label_span=");
            render_span(output, statement->region_label_span);
            break;
        case SOL_IR_STATEMENT_MODIFY:
            render_text(output, " target=");
            render_id(output, "expr", statement->target, SOL_IR_NONE);
            render_text(output, " expression=");
            render_id(output, "expr", statement->expression, SOL_IR_NONE);
            break;
        case SOL_IR_STATEMENT_LOOP:
            render_text(output, " body=");
            render_id(output, "expr", statement->expression, SOL_IR_NONE);
            render_text(output, " obligations=");
            render_loop_obligations(renderer, statement->loop_obligations);
            break;
        case SOL_IR_STATEMENT_WHILE:
            render_text(output, " condition=");
            render_id(output, "expr", statement->condition, SOL_IR_NONE);
            render_text(output, " body=");
            render_id(output, "expr", statement->expression, SOL_IR_NONE);
            render_text(output, " obligations=");
            render_loop_obligations(renderer, statement->loop_obligations);
            break;
        case SOL_IR_STATEMENT_UNREACHABLE:
            render_text(output, " obligations=");
            render_unreachable_obligations(renderer,
                statement->unreachable_obligations);
            break;
        case SOL_IR_STATEMENT_REQUIRE:
            render_text(output, " condition=");
            render_id(output, "expr", statement->condition, SOL_IR_NONE);
            render_text(output, " fallback=");
            render_id(output, "expr", statement->expression, SOL_IR_NONE);
            break;
        case SOL_IR_STATEMENT_BREAK:
        case SOL_IR_STATEMENT_CONTINUE:
            break;
    }
    render_text(output, "\n");
}

static void render_source_pattern(MirRenderer *renderer, size_t id) {
    MirRenderBuffer *output = &renderer->output;
    const SolIrPattern *pattern = &renderer->ir->patterns[id];
    render_format(output, "source_pattern pattern%zu kind=", id);
    render_enum(output, pattern_kind_name(pattern->kind));
    render_text(output, " ty=");
    render_id(output, "ty", pattern->type, SOL_IR_NONE);
    render_text(output, " span=");
    render_span(output, pattern->span);
    switch (pattern->kind) {
        case SOL_IR_PATTERN_BOOL:
            render_format(output, " value=%s",
                pattern->boolean ? "true" : "false");
            break;
        case SOL_IR_PATTERN_BINDING:
            render_text(output, " binding=");
            render_id(output, "local", pattern->binding, SOL_IR_NONE);
            break;
        case SOL_IR_PATTERN_VARIANT:
            render_text(output, " variant=");
            render_id(output, "variant", pattern->variant, SOL_IR_NONE);
            break;
        case SOL_IR_PATTERN_RECORD:
            render_text(output, " definition=");
            render_id(output, "d", pattern->definition, SOL_IR_NONE);
            break;
        case SOL_IR_PATTERN_WILDCARD:
        case SOL_IR_PATTERN_TUPLE:
            break;
    }
    render_format(output, " children=%zu[", pattern->children.count);
    for (size_t index = 0; index < pattern->children.count; ++index) {
        if (index != 0) render_text(output, ",");
        size_t child_id = pattern->children.offset + index;
        const SolIrPatternChild *child
            = &renderer->ir->pattern_children[child_id];
        render_format(output, "pattern_child%zu{field=", child_id);
        render_id(output, "field", child->field, SOL_IR_NONE);
        render_format(output, " ordinal=%zu pattern=", child->ordinal);
        render_id(output, "pattern", child->pattern, SOL_IR_NONE);
        render_text(output, "}");
    }
    render_text(output, "]\n");
}

static void render_source_arm(MirRenderer *renderer, size_t id) {
    MirRenderBuffer *output = &renderer->output;
    const SolIrArm *arm = &renderer->ir->arms[id];
    render_format(output, "source_arm arm%zu pattern=", id);
    render_id(output, "pattern", arm->pattern, SOL_IR_NONE);
    render_text(output, " guard=");
    render_id(output, "expr", arm->guard, SOL_IR_NONE);
    render_text(output, " bindings=");
    render_roots(renderer, arm->bindings);
    render_text(output, " cleanup=");
    render_cleanup_locals(renderer, arm->cleanup);
    render_text(output, " body=");
    render_id(output, "expr", arm->body, SOL_IR_NONE);
    render_text(output, " span=");
    render_span(output, arm->span);
    render_text(output, "\n");
}

static void render_source_snapshot(MirRenderer *renderer, size_t id) {
    const SolIrSnapshot *snapshot = &renderer->ir->snapshots[id];
    render_format(&renderer->output,
        "source_snapshot snapshot%zu id=snapshot%zu obligation=", id,
        snapshot->id);
    render_id(&renderer->output, "obligation", (size_t)snapshot->obligation,
        SIZE_MAX);
    render_text(&renderer->output, " read=");
    render_id(&renderer->output, "expr", snapshot->read, SOL_IR_NONE);
    render_text(&renderer->output, " operand=");
    render_id(&renderer->output, "expr", snapshot->operand, SOL_IR_NONE);
    render_text(&renderer->output, " ty=");
    render_id(&renderer->output, "ty", snapshot->type, SOL_IR_NONE);
    render_text(&renderer->output, "\n");
}

static void render_source_obligation(MirRenderer *renderer, size_t id) {
    const SolIrObligation *obligation = &renderer->ir->obligations[id];
    render_format(&renderer->output,
        "source_obligation obligation%zu id=%" PRIu64 " owner_kind=", id,
        obligation->id);
    render_enum(&renderer->output,
        contract_owner_name(obligation->owner_kind));
    render_format(&renderer->output, " owner=%zu phase=", obligation->owner);
    render_enum(&renderer->output, phase_name(obligation->kind));
    render_text(&renderer->output, " outcome=");
    render_enum(&renderer->output, outcome_name(obligation->outcome));
    render_text(&renderer->output, " predicate=");
    render_id(&renderer->output, "expr", obligation->predicate, SOL_IR_NONE);
    render_format(&renderer->output, " result_available=%s result_ty=",
        obligation->result_available ? "true" : "false");
    render_id(&renderer->output, "ty", obligation->result_type, SOL_IR_NONE);
    render_format(&renderer->output, " snapshots=%zu[",
        obligation->snapshots.count);
    for (size_t index = 0; index < obligation->snapshots.count; ++index) {
        if (index != 0) render_text(&renderer->output, ",");
        render_id(&renderer->output, "snapshot",
            obligation->snapshots.offset + index, SOL_IR_NONE);
    }
    render_text(&renderer->output, "]\n");
}

static void render_source_relations(MirRenderer *renderer) {
    for (size_t id = 0; id < renderer->ir->expression_count; ++id) {
        if (renderer->expressions[id] != 0) {
            render_source_expression(renderer, id);
        }
    }
    for (size_t id = 0; id < renderer->ir->statement_count; ++id) {
        if (renderer->statements[id] != 0) {
            render_source_statement(renderer, id);
        }
    }
    for (size_t id = 0; id < renderer->ir->pattern_count; ++id) {
        if (renderer->patterns[id] != 0) render_source_pattern(renderer, id);
    }
    for (size_t id = 0; id < renderer->ir->arm_count; ++id) {
        if (renderer->arms[id] != 0) render_source_arm(renderer, id);
    }
    for (size_t id = 0; id < renderer->ir->snapshot_count; ++id) {
        if (renderer->snapshots[id] != 0) render_source_snapshot(renderer, id);
    }
    for (size_t id = 0; id < renderer->ir->obligation_count; ++id) {
        if (renderer->obligations[id] != 0) {
            render_source_obligation(renderer, id);
        }
    }
}

static void render_callable(MirRenderer *renderer) {
    const SolMir *mir = renderer->mir;
    const SolIrCallable *callable = &renderer->ir->callables[mir->callable];
    MirRenderBuffer *output = &renderer->output;
    render_text(output, "mir callable=");
    render_id(output, "c", mir->callable, SOL_IR_NONE);
    render_text(output, " callable_kind=");
    render_enum(output, callable_kind_name(callable->kind));
    render_text(output, " owner=");
    render_id(output, "d", callable->owner, SOL_IR_NONE);
    render_text(output, " name=");
    render_quoted(output, callable->name);
    render_text(output, " callable_span=");
    render_span(output, callable->span);
    render_text(output, " parameters=");
    render_roots(renderer, callable->parameters);
    render_text(output, " result_ty=");
    render_id(output, "ty", callable->result, SOL_IR_NONE);
    render_text(output, " body=");
    render_id(output, "expr", callable->body, SOL_IR_NONE);
    render_text(output, " callable_effects=");
    render_effects(renderer, callable->effects);
    render_text(output, " callable_effect_parameter=");
    render_id(output, "ep", callable->effect_parameter, SOL_IR_NONE);
    render_text(output, " receiver=");
    render_id(output, "local", callable->receiver, SOL_IR_NONE);
    render_text(output, " receiver_access=");
    render_enum(output, access_name(callable->receiver_access));
    render_text(output, " capability_source=");
    render_id(output, "local", callable->capability_source, SOL_IR_NONE);
    render_text(output, " result_authority_kind=");
    render_enum(output, authority_name(callable->result_authority_kind));
    render_text(output, " result_authority=");
    render_id(output, "local", callable->result_authority, SOL_IR_NONE);
    render_text(output, " generic_parameters=");
    render_format(output, "%zu[", mir->generic_parameters.count);
    for (size_t index = 0; index < mir->generic_parameters.count; ++index) {
        if (index != 0) render_text(output, ",");
        render_id(output, "gp", mir->generic_parameters.offset + index,
            SOL_IR_NONE);
    }
    render_text(output, "] effect_parameters=");
    render_format(output, "%zu[", mir->effect_parameters.count);
    for (size_t index = 0; index < mir->effect_parameters.count; ++index) {
        if (index != 0) render_text(output, ",");
        render_id(output, "ep", mir->effect_parameters.offset + index,
            SOL_IR_NONE);
    }
    render_text(output, "] entry=");
    render_id(output, "b", mir->entry, SOL_MIR_NONE);
    render_text(output, " contract_body=");
    render_id(output, "b", mir->contract_body, SOL_MIR_NONE);
    render_text(output, " contract_epilogue=");
    render_id(output, "b", mir->contract_epilogue, SOL_MIR_NONE);
    render_format(output,
        " blocks=%zu instructions=%zu values=%zu temporaries=%zu loops=%zu call_arguments=%zu construct_operands=%zu\n",
        mir->block_count, mir->instruction_count, mir->value_count,
        mir->temporary_count, mir->loop_count, mir->call_argument_count,
        mir->construct_operand_count);
    for (size_t index = 0; index < mir->generic_parameters.count; ++index) {
        size_t id = mir->generic_parameters.offset + index;
        const SolIrGenericParameter *parameter
            = &renderer->ir->generic_parameters[id];
        render_format(output, "generic_parameter gp%zu owner=", id);
        render_id(output, "d", parameter->owner, SOL_IR_NONE);
        render_text(output, " name=");
        render_quoted(output, parameter->name);
        render_format(output, " ordinal=%zu trait_bound=", parameter->ordinal);
        render_id(output, "d", parameter->trait_bound, SOL_IR_NONE);
        render_text(output, "\n");
    }
    for (size_t index = 0; index < mir->effect_parameters.count; ++index) {
        size_t id = mir->effect_parameters.offset + index;
        const SolIrEffectParameter *parameter
            = &renderer->ir->effect_parameters[id];
        render_format(output, "effect_parameter ep%zu owner=", id);
        render_id(output, "d", parameter->owner, SOL_IR_NONE);
        render_text(output, " name=");
        render_quoted(output, parameter->name);
        render_format(output, " ordinal=%zu\n", parameter->ordinal);
    }
}

static void render_temporaries(MirRenderer *renderer) {
    for (size_t id = 0; id < renderer->mir->temporary_count; ++id) {
        const SolMirTemporary *temporary = &renderer->mir->temporaries[id];
        render_format(&renderer->output, "temporary t%zu ty=", id);
        render_id(&renderer->output, "ty", temporary->type, SOL_IR_NONE);
        render_text(&renderer->output, " source=");
        render_id(&renderer->output, "expr", temporary->source_expression,
            SOL_IR_NONE);
        render_text(&renderer->output, " span=");
        render_span(&renderer->output, temporary->span);
        render_text(&renderer->output, "\n");
    }
}

static void render_loops(MirRenderer *renderer) {
    for (size_t id = 0; id < renderer->mir->loop_count; ++id) {
        const SolMirLoop *loop = &renderer->mir->loops[id];
        render_format(&renderer->output, "loop loop%zu statement=", id);
        render_id(&renderer->output, "stmt", loop->statement, SOL_IR_NONE);
        render_text(&renderer->output, " parent=");
        render_id(&renderer->output, "loop", loop->parent, SOL_MIR_NONE);
        render_text(&renderer->output, " preheader=");
        render_id(&renderer->output, "b", loop->preheader, SOL_MIR_NONE);
        render_text(&renderer->output, " header=");
        render_id(&renderer->output, "b", loop->header, SOL_MIR_NONE);
        render_text(&renderer->output, " condition=");
        render_id(&renderer->output, "b", loop->condition, SOL_MIR_NONE);
        render_text(&renderer->output, " body=");
        render_id(&renderer->output, "b", loop->body, SOL_MIR_NONE);
        render_text(&renderer->output, " backedge=");
        render_id(&renderer->output, "b", loop->backedge, SOL_MIR_NONE);
        render_text(&renderer->output, " exit=");
        render_id(&renderer->output, "b", loop->exit, SOL_MIR_NONE);
        render_text(&renderer->output, " obligations=");
        render_loop_obligations(renderer, loop->obligations);
        render_text(&renderer->output, " span=");
        render_span(&renderer->output, loop->span);
        render_text(&renderer->output, "\n");
    }
}

static void render_values(MirRenderer *renderer) {
    for (size_t id = 0; id < renderer->mir->value_count; ++id) {
        const SolMirValue *value = &renderer->mir->values[id];
        render_format(&renderer->output, "value v%zu kind=", id);
        render_enum(&renderer->output, value_kind_name(value->kind));
        render_text(&renderer->output, " ty=");
        render_id(&renderer->output, "ty", value->type, SOL_IR_NONE);
        render_text(&renderer->output, " block=");
        render_id(&renderer->output, "b", value->block, SOL_MIR_NONE);
        render_format(&renderer->output, " definition=%zu source=",
            value->definition);
        render_id(&renderer->output, "expr", value->source_expression,
            SOL_IR_NONE);
        render_text(&renderer->output, " span=");
        render_span(&renderer->output, value->span);
        render_text(&renderer->output, "\n");
    }
}

static void render_instruction(MirRenderer *renderer, size_t id) {
    MirRenderBuffer *output = &renderer->output;
    const SolMirInstruction *instruction = &renderer->mir->instructions[id];
    render_format(output, "  instruction i%zu kind=", id);
    render_enum(output, instruction_kind_name(instruction->kind));
    render_text(output, " block=");
    render_id(output, "b", instruction->block, SOL_MIR_NONE);
    render_text(output, " result=");
    render_id(output, "v", instruction->result, SOL_MIR_NONE);
    render_text(output, " ty=");
    render_id(output, "ty", instruction->type, SOL_IR_NONE);
    render_text(output, " source=");
    render_id(output, "expr", instruction->source_expression, SOL_IR_NONE);
    render_text(output, " span=");
    render_span(output, instruction->span);
    switch (instruction->kind) {
        case SOL_MIR_INST_CONST_INT64:
            render_format(output, " value=%" PRId64, instruction->as.integer);
            break;
        case SOL_MIR_INST_CONST_BOOL:
            render_format(output, " value=%s",
                instruction->as.boolean ? "true" : "false");
            break;
        case SOL_MIR_INST_CONST_TEXT:
            render_text(output, " text=");
            if (instruction->source_expression >= renderer->ir->expression_count
                || renderer->ir->expressions[instruction->source_expression].kind
                    != SOL_IR_EXPR_STRING) {
                output->failed = true;
            } else {
                render_quoted(output, renderer->ir->expressions[
                    instruction->source_expression].as.string);
            }
            break;
        case SOL_MIR_INST_PARAMETER_LIVE:
        case SOL_MIR_INST_STORAGE_LIVE:
        case SOL_MIR_INST_DROP_IF_INITIALIZED:
        case SOL_MIR_INST_STORAGE_DEAD:
            render_text(output, " local=");
            render_id(output, "local", instruction->as.local, SOL_IR_NONE);
            break;
        case SOL_MIR_INST_LOAD_COPY:
        case SOL_MIR_INST_LOAD_MOVE:
        case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED:
            render_text(output, " place=");
            render_mir_place(renderer, instruction->as.place);
            break;
        case SOL_MIR_INST_LOAD_UPDATE:
            render_text(output, " statement=");
            render_id(output, "stmt", instruction->as.update_load.statement,
                SOL_IR_NONE);
            render_text(output, " place=");
            render_mir_place(renderer, instruction->as.update_load.place);
            break;
        case SOL_MIR_INST_STORE:
            render_text(output, " place=");
            render_mir_place(renderer, instruction->as.store.place);
            render_text(output, " value=");
            render_id(output, "v", instruction->as.store.value, SOL_MIR_NONE);
            break;
        case SOL_MIR_INST_UNARY:
            render_text(output, " operator=");
            render_enum(output,
                operator_name(instruction->as.unary.operator_kind));
            render_text(output, " operand=");
            render_id(output, "v", instruction->as.unary.operand, SOL_MIR_NONE);
            break;
        case SOL_MIR_INST_BINARY:
            render_text(output, " operator=");
            render_enum(output,
                operator_name(instruction->as.binary.operator_kind));
            render_text(output, " left=");
            render_id(output, "v", instruction->as.binary.left, SOL_MIR_NONE);
            render_text(output, " right=");
            render_id(output, "v", instruction->as.binary.right, SOL_MIR_NONE);
            break;
        case SOL_MIR_INST_COMPOUND_UPDATE:
            render_text(output, " statement=");
            render_id(output, "stmt",
                instruction->as.compound_update.statement, SOL_IR_NONE);
            render_text(output, " operator=");
            render_enum(output,
                operator_name(instruction->as.compound_update.operator_kind));
            render_text(output, " place=");
            render_mir_place(renderer, instruction->as.compound_update.place);
            render_text(output, " previous=");
            render_id(output, "t", instruction->as.compound_update.previous,
                SOL_MIR_NONE);
            render_text(output, " right=");
            render_id(output, "v", instruction->as.compound_update.right,
                SOL_MIR_NONE);
            break;
        case SOL_MIR_INST_REGION_ENTER:
        case SOL_MIR_INST_REGION_EXIT:
            render_text(output, " statement=");
            render_id(output, "stmt", instruction->as.region, SOL_IR_NONE);
            break;
        case SOL_MIR_INST_TEMPORARY_INIT:
            render_text(output, " temporary=");
            render_id(output, "t", instruction->as.temporary_init.temporary,
                SOL_MIR_NONE);
            render_text(output, " value=");
            render_id(output, "v", instruction->as.temporary_init.value,
                SOL_MIR_NONE);
            break;
        case SOL_MIR_INST_TEMPORARY_DROP:
            render_text(output, " temporary=");
            render_id(output, "t", instruction->as.temporary_drop.temporary,
                SOL_MIR_NONE);
            render_format(output, " preserve_depth=%zu",
                instruction->as.temporary_drop.preserve_depth);
            break;
        case SOL_MIR_INST_EXPRESSION_RESULT:
            render_text(output, " operand=");
            render_id(output, "v", instruction->as.operand, SOL_MIR_NONE);
            break;
        case SOL_MIR_INST_PATTERN_TEST:
        case SOL_MIR_INST_PATTERN_VALUE:
        case SOL_MIR_INST_MATCH_ARM:
            render_text(output, " match_expression=");
            render_id(output, "expr",
                instruction->as.pattern.match_expression, SOL_IR_NONE);
            render_text(output, " arm=");
            render_id(output, "arm", instruction->as.pattern.arm, SOL_IR_NONE);
            render_format(output, " arm_ordinal=%zu pattern=",
                instruction->as.pattern.arm_ordinal);
            render_id(output, "pattern", instruction->as.pattern.pattern,
                SOL_IR_NONE);
            render_text(output, " scrutinee=");
            render_id(output, "t", instruction->as.pattern.scrutinee,
                SOL_MIR_NONE);
            break;
        case SOL_MIR_INST_CONSTRUCT:
            render_text(output, " construct_kind=");
            render_enum(output,
                construct_kind_name(instruction->as.construct.kind));
            render_text(output, " definition=");
            render_id(output, "d", instruction->as.construct.definition,
                SOL_IR_NONE);
            render_text(output, " variant=");
            render_id(output, "variant", instruction->as.construct.variant,
                SOL_IR_NONE);
            render_text(output, " operands=");
            render_construct_operands(renderer,
                instruction->as.construct.operands);
            render_text(output, " capability_roots=");
            render_roots(renderer, instruction->as.construct.capability_roots);
            render_text(output, " operation_roots=");
            render_roots(renderer, instruction->as.construct.operation_roots);
            break;
        case SOL_MIR_INST_CAPTURE_SNAPSHOT:
            render_text(output, " snapshot=");
            render_id(output, "snapshot", instruction->as.snapshot, SOL_IR_NONE);
            break;
        case SOL_MIR_INST_CONST_UNIT:
        case SOL_MIR_INST_HANDLER_ENTER:
        case SOL_MIR_INST_HANDLER_EXIT:
            break;
    }
    render_text(output, "\n");
}

static void render_terminator(MirRenderer *renderer,
    const SolMirTerminator *term) {
    MirRenderBuffer *output = &renderer->output;
    render_text(output, "  terminator kind=");
    render_enum(output, terminator_kind_name(term->kind));
    render_text(output, " span=");
    render_span(output, term->span);
    switch (term->kind) {
        case SOL_MIR_TERM_GOTO:
            render_text(output, " edge=");
            render_edge(renderer, term->as.go_to);
            break;
        case SOL_MIR_TERM_BRANCH:
            render_text(output, " condition=");
            render_id(output, "v", term->as.branch.condition, SOL_MIR_NONE);
            render_text(output, " true_edge=");
            render_edge(renderer, term->as.branch.true_edge);
            render_text(output, " false_edge=");
            render_edge(renderer, term->as.branch.false_edge);
            break;
        case SOL_MIR_TERM_RETURN:
        case SOL_MIR_TERM_PANIC:
            render_text(output, " value=");
            render_id(output, "v", term->as.value, SOL_MIR_NONE);
            break;
        case SOL_MIR_TERM_INVOKE:
            render_text(output, " source=");
            render_id(output, "expr", term->as.invoke.source_expression,
                SOL_IR_NONE);
            render_text(output, " call_kind=");
            render_enum(output, call_kind_name(term->as.invoke.kind));
            render_text(output, " callable=");
            render_id(output, "c", term->as.invoke.callable, SOL_IR_NONE);
            render_text(output, " type_arguments=");
            render_type_ids(renderer, term->as.invoke.type_arguments);
            render_text(output, " effects=");
            render_effects(renderer, term->as.invoke.effects);
            render_text(output, " effect_parameter=");
            render_id(output, "ep", term->as.invoke.effect_parameter,
                SOL_IR_NONE);
            render_text(output, " evidence=");
            render_evidence(renderer, term->as.invoke.evidence);
            render_text(output, " callee=");
            render_id(output, "t", term->as.invoke.callee, SOL_MIR_NONE);
            render_text(output, " receiver=");
            if (term->as.invoke.kind == SOL_IR_CALL_METHOD
                || term->as.invoke.kind == SOL_IR_CALL_CAPABILITY) {
                render_call_argument(renderer, &term->as.invoke.receiver);
            } else render_text(output, "none");
            render_text(output, " arguments=");
            render_call_arguments(renderer, term->as.invoke.arguments);
            render_text(output, " result=");
            render_id(output, "v", term->as.invoke.result, SOL_MIR_NONE);
            render_text(output, " normal_edge=");
            render_edge(renderer, term->as.invoke.normal_edge);
            render_text(output, " failure_edge=");
            render_edge(renderer, term->as.invoke.failure_edge);
            break;
        case SOL_MIR_TERM_UNREACHABLE:
            render_text(output, " statement=");
            render_id(output, "stmt", term->as.unreachable.statement,
                SOL_IR_NONE);
            render_format(output, " obligation=unreachable_obligation%zu",
                term->as.unreachable.obligation);
            break;
        case SOL_MIR_TERM_BREAK:
        case SOL_MIR_TERM_CONTINUE:
            render_text(output, " statement=");
            render_id(output, "stmt", term->as.transfer.statement, SOL_IR_NONE);
            render_text(output, " loop=");
            render_id(output, "loop", term->as.transfer.loop, SOL_MIR_NONE);
            render_text(output, " edge=");
            render_edge(renderer, term->as.transfer.edge);
            break;
        case SOL_MIR_TERM_CHECK_REFINED:
            render_text(output, " source=");
            render_id(output, "expr",
                term->as.check_refined.source_expression, SOL_IR_NONE);
            render_text(output, " definition=");
            render_id(output, "d", term->as.check_refined.definition,
                SOL_IR_NONE);
            render_text(output, " obligation=");
            render_id(output, "obligation",
                (size_t)term->as.check_refined.obligation, SIZE_MAX);
            render_text(output, " representation=");
            render_id(output, "t", term->as.check_refined.representation,
                SOL_MIR_NONE);
            render_text(output, " result=");
            render_id(output, "v", term->as.check_refined.result, SOL_MIR_NONE);
            render_text(output, " normal_edge=");
            render_edge(renderer, term->as.check_refined.normal_edge);
            render_text(output, " failure_edge=");
            render_edge(renderer, term->as.check_refined.failure_edge);
            break;
        case SOL_MIR_TERM_MATCH_FAILURE:
            render_text(output, " source=");
            render_id(output, "expr", term->as.match_failure, SOL_IR_NONE);
            break;
        case SOL_MIR_TERM_PROPAGATE:
            render_text(output, " source=");
            render_id(output, "expr", term->as.propagate.source_expression,
                SOL_IR_NONE);
            render_text(output, " propagation_kind=");
            render_enum(output, propagation_name(term->as.propagate.kind));
            render_text(output, " operand=");
            render_id(output, "t", term->as.propagate.operand, SOL_MIR_NONE);
            render_text(output, " value_result=");
            render_id(output, "v", term->as.propagate.value_result,
                SOL_MIR_NONE);
            render_text(output, " residual_result=");
            render_id(output, "v", term->as.propagate.residual_result,
                SOL_MIR_NONE);
            render_text(output, " value_edge=");
            render_edge(renderer, term->as.propagate.value_edge);
            render_text(output, " residual_edge=");
            render_edge(renderer, term->as.propagate.residual_edge);
            break;
        case SOL_MIR_TERM_CHECK_CONTRACT:
            render_text(output, " obligation=");
            render_id(output, "obligation",
                (size_t)term->as.check_contract.obligation, SIZE_MAX);
            render_text(output, " phase=");
            render_enum(output, phase_name(term->as.check_contract.phase));
            render_text(output, " outcome=");
            render_enum(output, outcome_name(term->as.check_contract.outcome));
            render_text(output, " result=");
            render_id(output, "v", term->as.check_contract.result, SOL_MIR_NONE);
            render_text(output, " satisfied_edge=");
            render_edge(renderer, term->as.check_contract.satisfied_edge);
            render_text(output, " violation_edge=");
            render_edge(renderer, term->as.check_contract.violation_edge);
            render_text(output, " failure_edge=");
            render_edge(renderer, term->as.check_contract.failure_edge);
            break;
        case SOL_MIR_TERM_CONTRACT_VIOLATION:
            render_text(output, " obligation=");
            render_id(output, "obligation",
                (size_t)term->as.contract_violation, SIZE_MAX);
            break;
        case SOL_MIR_TERM_RESUME_FAILURE:
            break;
        case SOL_MIR_TERM_INVALID:
            output->failed = true;
            break;
    }
    render_text(output, "\n");
}

static void render_blocks(MirRenderer *renderer, const SolMirBlockId *order) {
    for (size_t ordinal = 0; ordinal < renderer->mir->block_count; ++ordinal) {
        SolMirBlockId id = order[ordinal];
        const SolMirBlock *block = &renderer->mir->blocks[id];
        render_format(&renderer->output, "block b%zu order=%zu parameters=",
            block->id, block->order);
        render_value_slice(renderer, block->parameters, true);
        render_format(&renderer->output, " instruction_count=%zu span=",
            block->instructions.count);
        render_span(&renderer->output, block->span);
        render_text(&renderer->output, "\n");
        for (size_t index = 0; index < block->instructions.count; ++index) {
            render_instruction(renderer, block->instructions.offset + index);
        }
        render_terminator(renderer, &block->terminator);
    }
}

static bool allocate_marks(MirRenderer *renderer) {
    const SolIr *ir = renderer->ir;
    renderer->expressions = ir->expression_count == 0 ? NULL
        : calloc(ir->expression_count, 1);
    renderer->statements = ir->statement_count == 0 ? NULL
        : calloc(ir->statement_count, 1);
    renderer->patterns = ir->pattern_count == 0 ? NULL
        : calloc(ir->pattern_count, 1);
    renderer->arms = ir->arm_count == 0 ? NULL : calloc(ir->arm_count, 1);
    renderer->snapshots = ir->snapshot_count == 0 ? NULL
        : calloc(ir->snapshot_count, 1);
    renderer->obligations = ir->obligation_count == 0 ? NULL
        : calloc(ir->obligation_count, 1);
    return (ir->expression_count == 0 || renderer->expressions != NULL)
        && (ir->statement_count == 0 || renderer->statements != NULL)
        && (ir->pattern_count == 0 || renderer->patterns != NULL)
        && (ir->arm_count == 0 || renderer->arms != NULL)
        && (ir->snapshot_count == 0 || renderer->snapshots != NULL)
        && (ir->obligation_count == 0 || renderer->obligations != NULL);
}

static void renderer_free(MirRenderer *renderer) {
    free(renderer->output.data);
    free(renderer->expressions);
    free(renderer->statements);
    free(renderer->patterns);
    free(renderer->arms);
    free(renderer->snapshots);
    free(renderer->obligations);
}

bool sol_mir_render(FILE *stream, const SolIr *ir, const SolMir *mir) {
    if (stream == NULL || ir == NULL || mir == NULL
        || !sol_mir_validate(ir, mir, NULL)
        || mir->block_count > SIZE_MAX / sizeof(SolMirBlockId)) return false;
    MirRenderer renderer = {.ir = ir, .mir = mir};
    SolMirBlockId *order = mir->block_count == 0 ? NULL
        : malloc(mir->block_count * sizeof(*order));
    if ((mir->block_count != 0 && order == NULL) || !allocate_marks(&renderer)
        || !collect_source_relations(&renderer)) {
        free(order);
        renderer_free(&renderer);
        return false;
    }
    for (size_t id = 0; id < mir->block_count; ++id) order[id] = id;
    for (size_t index = 1; index < mir->block_count; ++index) {
        SolMirBlockId id = order[index];
        size_t position = index;
        while (position != 0
            && mir->blocks[order[position - 1]].order > mir->blocks[id].order) {
            order[position] = order[position - 1];
            --position;
        }
        order[position] = id;
    }
    render_callable(&renderer);
    render_temporaries(&renderer);
    render_loops(&renderer);
    render_values(&renderer);
    render_blocks(&renderer, order);
    render_source_relations(&renderer);
    free(order);
    if (renderer.output.failed || renderer.output.length == 0
        || renderer.output.data[renderer.output.length - 1] != '\n') {
        renderer_free(&renderer);
        return false;
    }
    size_t length = renderer.output.length;
    bool written = fwrite(renderer.output.data, 1, length, stream) == length;
    renderer_free(&renderer);
    return written;
}
