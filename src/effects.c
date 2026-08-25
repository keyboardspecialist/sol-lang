#include "sol/effects.h"

#include <stdint.h>
#include <stdlib.h>
#include "resource_internal.h"
#include <string.h>

typedef struct {
    size_t offset;
    size_t count;
} SolEffectsCalls;

typedef struct {
    SolIrCallableId *callables;
    SolEffectsCalls *calls;
    SolIrExpressionId *call_ids;
    size_t call_count;
    size_t call_capacity;
} SolEffectsReport;

static const SolIrSourceFile *sol_effects_file(const SolIr *ir, SolSpan span) {
    for (size_t index = 0; index < ir->file_count; ++index) {
        if (span.start >= ir->files[index].aggregate_start
            && span.start < ir->files[index].aggregate_end) return &ir->files[index];
    }
    return ir->file_count == 0 ? NULL : &ir->files[0];
}

static size_t sol_effects_local_offset(const SolIrSourceFile *file, size_t offset) {
    return file != NULL && offset >= file->aggregate_start
        ? offset - file->aggregate_start : offset;
}

static int sol_effects_callable_compare(
    const SolIr *ir, SolIrCallableId left_id, SolIrCallableId right_id
) {
    const SolIrCallable *left = &ir->callables[left_id];
    const SolIrCallable *right = &ir->callables[right_id];
    const SolIrSourceFile *left_file = sol_effects_file(ir, left->span);
    const SolIrSourceFile *right_file = sol_effects_file(ir, right->span);
    const char *left_path = left_file == NULL ? ir->source_path : left_file->path;
    const char *right_path = right_file == NULL ? ir->source_path : right_file->path;
    int path = strcmp(left_path, right_path);
    if (path != 0) return path;
    size_t left_start = sol_effects_local_offset(left_file, left->span.start);
    size_t right_start = sol_effects_local_offset(right_file, right->span.start);
    if (left_start != right_start) return left_start < right_start ? -1 : 1;
    if (left->span.end != right->span.end) return left->span.end < right->span.end ? -1 : 1;
    if (left->kind != right->kind) return left->kind < right->kind ? -1 : 1;
    int name = strcmp(left->name, right->name);
    if (name != 0) return name;
    return left_id < right_id ? -1 : left_id > right_id;
}

static int sol_effects_call_compare(
    const SolIr *ir, SolIrExpressionId left_id, SolIrExpressionId right_id
) {
    const SolIrExpression *left = &ir->expressions[left_id];
    const SolIrExpression *right = &ir->expressions[right_id];
    if (left->span.start != right->span.start) return left->span.start < right->span.start ? -1 : 1;
    if (left->span.end != right->span.end) return left->span.end < right->span.end ? -1 : 1;
    if (left->as.call.kind != right->as.call.kind) {
        return left->as.call.kind < right->as.call.kind ? -1 : 1;
    }
    return left_id < right_id ? -1 : left_id > right_id;
}

static bool sol_effects_append_call(SolEffectsReport *report, SolIrExpressionId id) {
    if (report->call_count == report->call_capacity) {
        size_t capacity = report->call_capacity == 0 ? 32 : report->call_capacity * 2;
        if (capacity < report->call_capacity || capacity > SIZE_MAX / sizeof(*report->call_ids)) {
            return false;
        }
        SolIrExpressionId *grown = realloc(report->call_ids, capacity * sizeof(*grown));
        if (grown == NULL) return false;
        report->call_ids = grown;
        report->call_capacity = capacity;
    }
    report->call_ids[report->call_count++] = id;
    return true;
}

static bool sol_effects_push(
    SolIrExpressionId *stack,
    unsigned char *seen,
    size_t capacity,
    size_t *count,
    SolIrExpressionId id
) {
    if (id >= capacity) return false;
    if (seen[id]) return true;
    if (*count >= capacity) return false;
    seen[id] = 1;
    stack[(*count)++] = id;
    return true;
}

static bool sol_effects_collect_callable(
    const SolIr *ir, SolIrCallableId callable_id, SolEffectsReport *report
) {
    const SolIrCallable *callable = &ir->callables[callable_id];
    SolEffectsCalls *calls = &report->calls[callable_id];
    calls->offset = report->call_count;
    if (callable->body == SOL_IR_NONE) return true;
    unsigned char *seen = calloc(ir->expression_count, 1);
    SolIrExpressionId *stack = ir->expression_count == 0
        || ir->expression_count > SIZE_MAX / sizeof(*stack) ? NULL
        : malloc(ir->expression_count * sizeof(*stack));
    if ((seen == NULL || stack == NULL) && ir->expression_count != 0) {
        free(seen);
        free(stack);
        return false;
    }
    size_t stack_count = 0;
    bool valid = sol_effects_push(
        stack, seen, ir->expression_count, &stack_count, callable->body);
#define PUSH(child) \
    do { if (!sol_effects_push(stack, seen, ir->expression_count, &stack_count, (child))) valid = false; } while (0)
    while (valid && stack_count != 0) {
        SolIrExpressionId id = stack[--stack_count];
        const SolIrExpression *expression = &ir->expressions[id];
        switch (expression->kind) {
            case SOL_IR_EXPR_UNARY: PUSH(expression->as.unary.operand); break;
            case SOL_IR_EXPR_BINARY:
                PUSH(expression->as.binary.left); PUSH(expression->as.binary.right); break;
            case SOL_IR_EXPR_CALL:
                if (expression->as.call.kind <= SOL_IR_CALL_METHOD) {
                    valid = sol_effects_append_call(report, id);
                    if (!valid) break;
                    ++calls->count;
                }
                if (expression->as.call.kind == SOL_IR_CALL_CALLBACK
                    || expression->as.call.kind == SOL_IR_CALL_CAPABILITY) {
                    PUSH(expression->as.call.callee);
                } else if (expression->as.call.kind == SOL_IR_CALL_METHOD) {
                    PUSH(expression->as.call.receiver);
                }
                for (size_t index = 0; index < expression->as.call.operands.count; ++index) {
                    PUSH(ir->operands[expression->as.call.operands.offset + index].value);
                }
                break;
            case SOL_IR_EXPR_RECORD:
                for (size_t index = 0; index < expression->as.record.fields.count; ++index) {
                    PUSH(ir->operands[expression->as.record.fields.offset + index].value);
                }
                break;
            case SOL_IR_EXPR_TUPLE:
                for (size_t index = 0; index < expression->as.tuple.operands.count; ++index) {
                    PUSH(ir->operands[expression->as.tuple.operands.offset + index].value);
                }
                break;
            case SOL_IR_EXPR_PLACE: {
                const SolIrPlace *place = &ir->places[expression->as.place];
                if (place->root_kind == SOL_IR_PLACE_ROOT_TEMPORARY) {
                    PUSH(place->temporary);
                }
                break;
            }
            case SOL_IR_EXPR_BOUND_OPERATION: PUSH(expression->as.operation.receiver); break;
            case SOL_IR_EXPR_IF:
                PUSH(expression->as.if_expr.condition); PUSH(expression->as.if_expr.then_branch);
                PUSH(expression->as.if_expr.else_branch); break;
            case SOL_IR_EXPR_MATCH:
                PUSH(expression->as.match_expr.scrutinee);
                for (size_t index = 0; index < expression->as.match_expr.arms.count; ++index) {
                    SolIrArmId arm = ir->arm_ids[expression->as.match_expr.arms.offset + index];
                    if (ir->arms[arm].guard != SOL_IR_NONE) {
                        PUSH(ir->arms[arm].guard);
                    }
                    PUSH(ir->arms[arm].body);
                }
                break;
            case SOL_IR_EXPR_BLOCK:
                for (size_t index = 0;
                    index < expression->as.block.statements.count; ++index) {
                    SolIrStatementId statement
                        = ir->statement_ids[
                            expression->as.block.statements.offset + index];
                    const SolIrStatement *entry = &ir->statements[statement];
                    if (entry->kind == SOL_IR_STATEMENT_WHILE) {
                        PUSH(entry->condition);
                    }
                    if (entry->kind != SOL_IR_STATEMENT_DECLARE
                        && entry->kind != SOL_IR_STATEMENT_BREAK
                        && entry->kind != SOL_IR_STATEMENT_CONTINUE) {
                        PUSH(entry->expression);
                    }
                }
                break;
            case SOL_IR_EXPR_PROPAGATE: PUSH(expression->as.propagate.operand); break;
            case SOL_IR_EXPR_HANDLE:
                PUSH(expression->as.handler.authority); PUSH(expression->as.handler.provider);
                PUSH(expression->as.handler.body); break;
            default: break;
        }
    }
#undef PUSH
    free(seen);
    free(stack);
    for (size_t index = 1; valid && index < calls->count; ++index) {
        SolIrExpressionId value = report->call_ids[calls->offset + index];
        size_t position = index;
        while (position != 0 && sol_effects_call_compare(ir,
            report->call_ids[calls->offset + position - 1], value) > 0) {
            report->call_ids[calls->offset + position]
                = report->call_ids[calls->offset + position - 1];
            --position;
        }
        report->call_ids[calls->offset + position] = value;
    }
    return valid;
}

static void sol_effects_report_free(SolEffectsReport *report) {
    free(report->callables);
    free(report->calls);
    free(report->call_ids);
    memset(report, 0, sizeof(*report));
}

static bool sol_effects_report(const SolIr *ir, SolEffectsReport *report) {
    memset(report, 0, sizeof(*report));
    if (ir->callable_count != 0) {
        if (ir->callable_count > SIZE_MAX / sizeof(*report->callables)
            || ir->callable_count > SIZE_MAX / sizeof(*report->calls)) return false;
        report->callables = malloc(ir->callable_count * sizeof(*report->callables));
        report->calls = calloc(ir->callable_count, sizeof(*report->calls));
        if (report->callables == NULL || report->calls == NULL) {
            sol_effects_report_free(report);
            return false;
        }
    }
    for (size_t index = 0; index < ir->callable_count; ++index) {
        report->callables[index] = index;
        if (!sol_effects_collect_callable(ir, index, report)) {
            sol_effects_report_free(report);
            return false;
        }
    }
    for (size_t index = 1; index < ir->callable_count; ++index) {
        SolIrCallableId value = report->callables[index];
        size_t position = index;
        while (position != 0
            && sol_effects_callable_compare(ir, report->callables[position - 1], value) > 0) {
            report->callables[position] = report->callables[position - 1];
            --position;
        }
        report->callables[position] = value;
    }
    return true;
}

static const char *sol_effects_callable_kind(SolIrCallableKind kind) {
    switch (kind) {
        case SOL_IR_CALLABLE_FUNCTION: return "function";
        case SOL_IR_CALLABLE_CAPABILITY: return "capability";
        case SOL_IR_CALLABLE_TRAIT_REQUIREMENT: return "trait_requirement";
        case SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION: return "trait_implementation";
        case SOL_IR_CALLABLE_TEST: return "test";
    }
    return "unknown";
}

static const char *sol_effects_call_kind(SolIrCallKind kind) {
    switch (kind) {
        case SOL_IR_CALL_FUNCTION: return "function";
        case SOL_IR_CALL_CALLBACK: return "callback";
        case SOL_IR_CALL_CAPABILITY: return "capability";
        case SOL_IR_CALL_METHOD: return "method";
        default: return "constructor";
    }
}

static bool sol_effects_json_continuation(unsigned char byte) {
    return byte >= 0x80 && byte <= 0xbf;
}

static size_t sol_effects_json_utf8_length(const unsigned char *text) {
    unsigned char first = text[0];
    if (first < 0x80) return 1;
    unsigned char second = text[1];
    if (second == 0) return 0;
    if (first >= 0xc2 && first <= 0xdf) {
        return sol_effects_json_continuation(second) ? 2 : 0;
    }
    unsigned char third = text[2];
    if (third == 0 || !sol_effects_json_continuation(third)) return 0;
    if ((first == 0xe0 && second >= 0xa0 && second <= 0xbf)
        || ((first >= 0xe1 && first <= 0xec) && sol_effects_json_continuation(second))
        || (first == 0xed && second >= 0x80 && second <= 0x9f)
        || ((first == 0xee || first == 0xef) && sol_effects_json_continuation(second))) {
        return 3;
    }
    if (first < 0xf0 || first > 0xf4) return 0;
    unsigned char fourth = text[3];
    if (fourth == 0 || !sol_effects_json_continuation(fourth)) return 0;
    if ((first == 0xf0 && second >= 0x90 && second <= 0xbf)
        || ((first >= 0xf1 && first <= 0xf3) && sol_effects_json_continuation(second))
        || (first == 0xf4 && second >= 0x80 && second <= 0x8f)) return 4;
    return 0;
}

static void sol_effects_json_text(FILE *stream, const char *text) {
    const unsigned char *cursor = (const unsigned char *)text;
    while (*cursor != 0) {
        size_t utf8_length = sol_effects_json_utf8_length(cursor);
        if (*cursor >= 0x80 && utf8_length != 0) {
            (void)fwrite(cursor, 1, utf8_length, stream);
            cursor += utf8_length;
            continue;
        }
        if (*cursor == '"') fputs("\\\"", stream);
        else if (*cursor == '\\') fputs("\\\\", stream);
        else if (*cursor == '\n') fputs("\\n", stream);
        else if (*cursor == '\r') fputs("\\r", stream);
        else if (*cursor == '\t') fputs("\\t", stream);
        else if (*cursor < 0x20 || *cursor >= 0x80) {
            fprintf(stream, "\\u%04x", (unsigned int)*cursor);
        }
        else fputc((int)*cursor, stream);
        ++cursor;
    }
}

static void sol_effects_json_string(FILE *stream, const char *text) {
    fputc('"', stream);
    sol_effects_json_text(stream, text);
    fputc('"', stream);
}

static void sol_effects_json_callable_name(
    FILE *stream, const SolIr *ir, SolIrCallableId id
) {
    const SolIrCallable *callable = &ir->callables[id];
    fputc('"', stream);
    if (callable->kind != SOL_IR_CALLABLE_FUNCTION
        && callable->kind != SOL_IR_CALLABLE_TEST) {
        sol_effects_json_text(stream, ir->definitions[callable->owner].name);
        fputc('.', stream);
    }
    sol_effects_json_text(stream, callable->name);
    fputc('"', stream);
}

static void sol_effects_callable_name(FILE *stream, const SolIr *ir, SolIrCallableId id) {
    const SolIrCallable *callable = &ir->callables[id];
    const SolIrDefinition *owner = &ir->definitions[callable->owner];
    if (callable->kind == SOL_IR_CALLABLE_FUNCTION || callable->kind == SOL_IR_CALLABLE_TEST) {
        fputs(callable->name, stream);
    } else {
        fprintf(stream, "%s.%s", owner->name, callable->name);
    }
}

static SolIrCallableId sol_effects_call_target(
    const SolIr *ir, const SolIrExpression *call, bool *dynamic
) {
    *dynamic = call->as.call.kind == SOL_IR_CALL_CALLBACK;
    if (call->as.call.kind == SOL_IR_CALL_CALLBACK) return SOL_IR_NONE;
    if (call->as.call.kind == SOL_IR_CALL_METHOD && call->as.call.evidence.count != 0) {
        const SolIrDispatchEvidence *evidence = &ir->evidence[call->as.call.evidence.offset];
        if (evidence->forwarded || evidence->method == SOL_IR_NONE) {
            *dynamic = true;
            return evidence->requirement;
        }
        return evidence->method;
    }
    return call->as.call.callable;
}

static void sol_effects_human_row(
    FILE *stream, const SolIr *ir, SolIrSlice row, SolIrEffectParameterId tail
) {
    if (row.count == 0 && tail == SOL_IR_NONE) {
        fputs("pure", stream);
        return;
    }
    for (size_t index = 0; index < row.count; ++index) {
        if (index != 0) fputs(", ", stream);
        const SolIrEffect *effect = &ir->effects[row.offset + index];
        fputs(effect->name, stream);
        if (effect->authority_kind == SOL_IR_AUTHORITY_LOCAL) {
            fprintf(stream, "<%s>", ir->locals[effect->authority].name);
        } else if (effect->authority_kind == SOL_IR_AUTHORITY_SELF) {
            fputs("<Self>", stream);
        }
    }
    if (tail != SOL_IR_NONE) {
        if (row.count != 0) fputs(", ", stream);
        fputs(ir->effect_parameters[tail].name, stream);
    }
}

static void sol_effects_human_authority(
    FILE *stream, const SolIr *ir, const SolIrCallable *callable
) {
    size_t count = 0;
    if (callable->kind == SOL_IR_CALLABLE_CAPABILITY) {
        fputs("Self", stream);
        ++count;
    }
    for (size_t index = 0; index < callable->parameters.count; ++index) {
        SolIrLocalId local = ir->roots[callable->parameters.offset + index];
        if (ir->locals[local].capability_roots.count == 0) continue;
        if (count++ != 0) fputs(", ", stream);
        fputs(ir->locals[local].name, stream);
    }
    if (count == 0) fputs("none", stream);
}

static void sol_effects_json_authority(
    FILE *stream, const SolIr *ir, const SolIrCallable *callable
) {
    fputc('[', stream);
    size_t count = 0;
    if (callable->kind == SOL_IR_CALLABLE_CAPABILITY) {
        sol_effects_json_string(stream, "Self");
        ++count;
    }
    for (size_t index = 0; index < callable->parameters.count; ++index) {
        SolIrLocalId local = ir->roots[callable->parameters.offset + index];
        if (ir->locals[local].capability_roots.count == 0) continue;
        if (count++ != 0) fputc(',', stream);
        sol_effects_json_string(stream, ir->locals[local].name);
    }
    fputc(']', stream);
}

static void sol_effects_json_row(
    FILE *stream, const SolIr *ir, SolIrSlice row, SolIrEffectParameterId tail
) {
    fputc('[', stream);
    for (size_t index = 0; index < row.count; ++index) {
        if (index != 0) fputc(',', stream);
        const SolIrEffect *effect = &ir->effects[row.offset + index];
        fputs("{\"name\":", stream); sol_effects_json_string(stream, effect->name);
        fputs(",\"authority\":", stream);
        if (effect->authority_kind == SOL_IR_AUTHORITY_NONE) fputs("null", stream);
        else if (effect->authority_kind == SOL_IR_AUTHORITY_SELF) {
            fputs("{\"kind\":\"self\",\"name\":\"Self\"}", stream);
        } else {
            fputs("{\"kind\":\"local\",\"name\":", stream);
            sol_effects_json_string(stream, ir->locals[effect->authority].name);
            fputc('}', stream);
        }
        fputc('}', stream);
    }
    fputs("],\"effect_parameter\":", stream);
    if (tail == SOL_IR_NONE) fputs("null", stream);
    else sol_effects_json_string(stream, ir->effect_parameters[tail].name);
}

static void sol_effects_render_human(
    FILE *stream, const SolIr *ir, const SolEffectsReport *report
) {
    fprintf(stream, "%zu callable%s\n", ir->callable_count,
        ir->callable_count == 1 ? "" : "s");
    for (size_t order = 0; order < ir->callable_count; ++order) {
        SolIrCallableId id = report->callables[order];
        const SolIrCallable *callable = &ir->callables[id];
        const SolIrSourceFile *file = sol_effects_file(ir, callable->span);
        fprintf(stream, "\n%s ", sol_effects_callable_kind(callable->kind));
        sol_effects_callable_name(stream, ir, id);
        fprintf(stream, "  %s:%zu\n  authority: ", file == NULL ? ir->source_path : file->path,
            sol_effects_local_offset(file, callable->span.start));
        sol_effects_human_authority(stream, ir, callable);
        fputs("\n  effects: ", stream);
        sol_effects_human_row(stream, ir, callable->effects, callable->effect_parameter);
        fputc('\n', stream);
        SolEffectsCalls calls = report->calls[id];
        for (size_t index = 0; index < calls.count; ++index) {
            const SolIrExpression *call = &ir->expressions[report->call_ids[calls.offset + index]];
            bool dynamic;
            SolIrCallableId target = sol_effects_call_target(ir, call, &dynamic);
            const SolIrSourceFile *call_file = sol_effects_file(ir, call->span);
            fprintf(stream, "  call %s:%zu %s ",
                call_file == NULL ? ir->source_path : call_file->path,
                sol_effects_local_offset(call_file, call->span.start),
                sol_effects_call_kind(call->as.call.kind));
            if (target == SOL_IR_NONE) fputs("<dynamic>", stream);
            else sol_effects_callable_name(stream, ir, target);
            if (dynamic) fputs(" [dynamic]", stream);
            fputs(" effects: ", stream);
            sol_effects_human_row(stream, ir, call->as.call.effects,
                call->as.call.effect_parameter);
            fputc('\n', stream);
        }
    }
}

static void sol_effects_render_json(
    FILE *stream, const SolIr *ir, const SolEffectsReport *report
) {
    fputs("{\"schema\":\"sol.effects\",\"version\":1,\"callables\":[", stream);
    for (size_t order = 0; order < ir->callable_count; ++order) {
        if (order != 0) fputc(',', stream);
        SolIrCallableId id = report->callables[order];
        const SolIrCallable *callable = &ir->callables[id];
        const SolIrSourceFile *file = sol_effects_file(ir, callable->span);
        fputs("{\"kind\":", stream); sol_effects_json_string(stream,
            sol_effects_callable_kind(callable->kind));
        fputs(",\"name\":", stream);
        sol_effects_json_callable_name(stream, ir, id);
        fputs(",\"path\":", stream);
        sol_effects_json_string(stream, file == NULL ? ir->source_path : file->path);
        fprintf(stream, ",\"start\":%zu,\"end\":%zu,\"effects\":",
            sol_effects_local_offset(file, callable->span.start),
            sol_effects_local_offset(file, callable->span.end));
        sol_effects_json_row(stream, ir, callable->effects, callable->effect_parameter);
        fputs(",\"authority\":", stream);
        sol_effects_json_authority(stream, ir, callable);
        fputs(",\"calls\":[", stream);
        SolEffectsCalls calls = report->calls[id];
        for (size_t index = 0; index < calls.count; ++index) {
            if (index != 0) fputc(',', stream);
            const SolIrExpression *call = &ir->expressions[report->call_ids[calls.offset + index]];
            const SolIrSourceFile *call_file = sol_effects_file(ir, call->span);
            bool dynamic;
            SolIrCallableId target = sol_effects_call_target(ir, call, &dynamic);
            fputs("{\"kind\":", stream); sol_effects_json_string(stream,
                sol_effects_call_kind(call->as.call.kind));
            fputs(",\"target\":", stream);
            if (target == SOL_IR_NONE) fputs("null", stream);
            else sol_effects_json_callable_name(stream, ir, target);
            fprintf(stream, ",\"dynamic\":%s,\"path\":", dynamic ? "true" : "false");
            sol_effects_json_string(stream, call_file == NULL ? ir->source_path : call_file->path);
            fprintf(stream, ",\"start\":%zu,\"end\":%zu,\"effects\":",
                sol_effects_local_offset(call_file, call->span.start),
                sol_effects_local_offset(call_file, call->span.end));
            sol_effects_json_row(stream, ir, call->as.call.effects,
                call->as.call.effect_parameter);
            fputc('}', stream);
        }
        fputs("]}", stream);
    }
    fputs("]}\n", stream);
}

bool sol_effects_render(FILE *stream, const SolIr *ir, bool json) {
    if (stream == NULL || ir == NULL) return false;
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    bool valid = sol_ir_validate(ir, &diagnostics);
    sol_diagnostics_free(&diagnostics);
    if (!valid) return false;
    SolEffectsReport report;
    if (!sol_effects_report(ir, &report)) return false;
    if (json) sol_effects_render_json(stream, ir, &report);
    else sol_effects_render_human(stream, ir, &report);
    sol_effects_report_free(&report);
    return ferror(stream) == 0;
}
