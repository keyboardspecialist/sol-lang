#include "sol/mir_program.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    CLASS_UNSEEN,
    CLASS_PENDING,
    CLASS_TEMPLATE,
    CLASS_IMPORT,
    CLASS_REQUIREMENT,
} CallableClass;

typedef struct {
    SolIrCallableId callable;
    SolIrEvidenceId evidence;
    SolIrGenericParameterId binding;
} IncomingContext;

typedef struct {
    SolIrCallableId callable;
    SolIrCallableId incoming_target;
    SolIrEvidenceId evidence;
    SolIrExpressionId expression;
    SolSpan span;
} ForwardedUse;

typedef struct {
    SolMirProgram *program;
    SolDiagnostics *diagnostics;
    CallableClass *classes;
    IncomingContext *incoming;
    ForwardedUse *forwarded;
    size_t forwarded_count;
    size_t forwarded_capacity;
    size_t incoming_count;
    size_t incoming_capacity;
    SolMirProgramBuildOutcome outcome;
} Builder;

static bool program_empty(const SolMirProgram *program) {
    return program != NULL && program->ir == NULL && program->roots == NULL
        && program->root_count == 0 && program->approved_imports == NULL
        && program->approved_import_count == 0 && program->templates == NULL
        && program->template_count == 0 && program->imports == NULL
        && program->import_count == 0 && program->specializations == NULL
        && program->specialization_count == 0 && program->references == NULL
        && program->reference_count == 0
        && program->limits.max_callable_classifications == 0
        && program->limits.max_references == 0
        && program->limits.max_discovery_work == 0
        && program->usage.callable_classifications == 0
        && program->usage.references == 0
        && program->usage.discovery_work == 0;
}

void sol_mir_program_init(SolMirProgram *program) {
    if (program == NULL) return;
    memset(program, 0, sizeof(*program));
}

void sol_mir_program_free(SolMirProgram *program) {
    if (program == NULL) return;
    if (program->templates != NULL) {
        for (size_t index = 0; index < program->template_count; ++index) {
            sol_mir_free(&program->templates[index].mir);
        }
    }
    free(program->roots);
    free(program->approved_imports);
    free(program->templates);
    free(program->imports);
    free(program->specializations);
    free(program->references);
    sol_mir_program_init(program);
}

SolMirProgramLimits sol_mir_program_default_limits(void) {
    return (SolMirProgramLimits){4096, 65536, 1000000};
}

static void report(Builder *builder, const char *message) {
    if (builder->diagnostics == NULL) return;
    sol_diagnostics_add(builder->diagnostics, "SOL-MIR-PROGRAM-001",
        SOL_SEVERITY_ERROR, (SolSpan){0}, message);
}

static bool limits_zero(SolMirProgramLimits limits) {
    return limits.max_callable_classifications == 0 && limits.max_references == 0
        && limits.max_discovery_work == 0;
}

static bool limits_complete(SolMirProgramLimits limits) {
    return limits.max_callable_classifications != 0 && limits.max_references != 0
        && limits.max_discovery_work != 0;
}

static void *array_allocate(size_t count, size_t size) {
    if (count == 0) return NULL;
    if (size == 0 || count > SIZE_MAX / size) return NULL;
    return calloc(count, size);
}

static bool grow_array(void **items, size_t *capacity, size_t count, size_t size) {
    if (count <= *capacity) return true;
    size_t next = *capacity == 0 ? 8 : *capacity;
    while (next < count) {
        if (next > SIZE_MAX / 2) {
            next = count;
            break;
        }
        next *= 2;
    }
    if (size == 0 || next > SIZE_MAX / size) return false;
    void *grown = realloc(*items, next * size);
    if (grown == NULL) return false;
    *items = grown;
    *capacity = next;
    return true;
}

static bool charge_work(Builder *builder, size_t count) {
    SolMirProgramUsage *usage = &builder->program->usage;
    if (count > builder->program->limits.max_discovery_work
            - usage->discovery_work) {
        builder->outcome = SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED;
        report(builder, "symbolic MIR program discovery work limit exceeded");
        return false;
    }
    usage->discovery_work += count;
    return true;
}

static bool charge_classification(Builder *builder) {
    SolMirProgramUsage *usage = &builder->program->usage;
    if (usage->callable_classifications
            == builder->program->limits.max_callable_classifications) {
        builder->outcome = SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED;
        report(builder, "symbolic MIR program callable limit exceeded");
        return false;
    }
    ++usage->callable_classifications;
    return true;
}

static bool charge_sort(Builder *builder, size_t count) {
    if (count < 2) return true;
    size_t width = 1;
    while (width < count) {
        if (!charge_work(builder, count)) return false;
        if (width > SIZE_MAX / 2) break;
        width *= 2;
    }
    return true;
}

static int compare_id(const void *left, const void *right) {
    SolIrCallableId a = *(const SolIrCallableId *)left;
    SolIrCallableId b = *(const SolIrCallableId *)right;
    return a < b ? -1 : a > b;
}

static int compare_root(const void *left, const void *right) {
    const SolMirProgramRoot *a = left;
    const SolMirProgramRoot *b = right;
    if (a->callable != b->callable) return a->callable < b->callable ? -1 : 1;
    return a->kind < b->kind ? -1 : a->kind > b->kind;
}

static int compare_source(SolMirProgramSource a, SolMirProgramSource b) {
#define CMP(field) if (a.field != b.field) return a.field < b.field ? -1 : 1
    CMP(callable);
    CMP(expression);
    CMP(file);
    CMP(start);
    CMP(end);
#undef CMP
    return 0;
}

static int compare_reference(const void *left, const void *right) {
    const SolMirProgramReference *a = left;
    const SolMirProgramReference *b = right;
    int source = compare_source(a->source, b->source);
    if (source != 0) return source;
    if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
    return a->target < b->target ? -1 : a->target > b->target;
}

static int compare_specialization(const void *left, const void *right) {
    const SolMirProgramSpecialization *a = left;
    const SolMirProgramSpecialization *b = right;
#define CMP(field) if (a->field != b->field) return a->field < b->field ? -1 : 1
    CMP(trait);
    CMP(requirement);
    CMP(type);
    CMP(implementation);
    CMP(method);
#undef CMP
    return 0;
}

static int compare_template(const void *left, const void *right) {
    const SolMirProgramTemplate *a = left;
    const SolMirProgramTemplate *b = right;
    return a->callable < b->callable ? -1 : a->callable > b->callable;
}

static int compare_import(const void *left, const void *right) {
    const SolMirProgramImport *a = left;
    const SolMirProgramImport *b = right;
    return a->callable < b->callable ? -1 : a->callable > b->callable;
}

static bool source_for(Builder *builder, SolIrCallableId callable,
    SolIrExpressionId expression, SolSpan span, SolMirProgramSource *source) {
    const SolIr *ir = builder->program->ir;
    *source = (SolMirProgramSource){callable, expression, 0, span.start, span.end};
    bool found = false;
    for (size_t file = 0; file < ir->file_count; ++file) {
        if (!charge_work(builder, 1)) return false;
        const SolIrSourceFile *item = &ir->files[file];
        if (span.start >= item->aggregate_start && span.end <= item->aggregate_end) {
            source->file = file;
            source->start = span.start - item->aggregate_start;
            source->end = span.end - item->aggregate_start;
            found = true;
            break;
        }
    }
    if (found) return true;
    builder->outcome = SOL_MIR_PROGRAM_BUILD_INVALID_IR;
    report(builder, "source relation span has no owning-IR source file");
    return false;
}

static bool same_source(SolMirProgramSource a, SolMirProgramSource b) {
    return compare_source(a, b) == 0;
}

static bool add_reference(Builder *builder, SolMirProgramReferenceKind kind,
    SolIrCallableId source_callable, SolIrExpressionId expression, SolSpan span,
    SolIrCallableId target) {
    SolMirProgram *program = builder->program;
    SolMirProgramReference reference;
    memset(&reference, 0, sizeof(reference));
    reference.kind = kind;
    if (!source_for(builder, source_callable, expression, span,
            &reference.source)) return false;
    reference.target = target;
    for (size_t index = 0; index < program->reference_count; ++index) {
        if (!charge_work(builder, 1)) return false;
        SolMirProgramReference *item = &program->references[index];
        if (item->kind == kind && item->target == target
            && same_source(item->source, reference.source)) return true;
    }
    if (program->reference_count == program->limits.max_references) {
        builder->outcome = SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED;
        report(builder, "symbolic MIR program reference limit exceeded");
        return false;
    }
    if (program->reference_count == SIZE_MAX) {
        builder->outcome = SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED;
        return false;
    }
    size_t count = program->reference_count + 1;
    if (count > SIZE_MAX / sizeof(*program->references)) {
        builder->outcome = SOL_MIR_PROGRAM_BUILD_ALLOCATION_FAILED;
        return false;
    }
    void *grown = realloc(program->references, count * sizeof(*program->references));
    if (grown == NULL) {
        builder->outcome = SOL_MIR_PROGRAM_BUILD_ALLOCATION_FAILED;
        return false;
    }
    program->references = grown;
    program->references[program->reference_count++] = reference;
    program->usage.references = program->reference_count;
    return true;
}

static bool type_is_data_only(Builder *builder, SolIrTypeId id, size_t depth) {
    const SolIr *ir = builder->program->ir;
    if (!charge_work(builder, 1)) return false;
    if (id >= ir->type_count || depth >= 256 || depth > ir->type_count) {
        return false;
    }
    const SolIrType *type = &ir->types[id];
    if (type->kind == SOL_IR_TYPE_INT64 || type->kind == SOL_IR_TYPE_BOOL
        || type->kind == SOL_IR_TYPE_TEXT || type->kind == SOL_IR_TYPE_UNIT
        || type->kind == SOL_IR_TYPE_NEVER) return true;
    if (type->kind == SOL_IR_TYPE_FUNCTION || type->kind == SOL_IR_TYPE_PARAMETER
        || type->kind == SOL_IR_TYPE_SELF) return false;
    if (type->kind == SOL_IR_TYPE_OPTION || type->kind == SOL_IR_TYPE_RESULT
        || type->kind == SOL_IR_TYPE_TUPLE) {
        for (size_t index = 0; index < type->argument_count; ++index) {
            if (!type_is_data_only(builder,
                    ir->type_ids[type->argument_offset + index], depth + 1)) {
                return false;
            }
        }
        return true;
    }
    if (type->kind != SOL_IR_TYPE_NOMINAL
        || type->definition >= ir->definition_count) return false;
    const SolIrDefinition *definition = &ir->definitions[type->definition];
    if (definition->kind == SOL_IR_DEFINITION_CAPABILITY
        || definition->generic_parameters.count != 0) return false;
    bool valid = true;
    if (definition->kind == SOL_IR_DEFINITION_RECORD) {
        for (size_t index = 0; valid && index < definition->fields.count; ++index) {
            valid = type_is_data_only(builder,
                ir->fields[definition->fields.offset + index].type,
                depth + 1);
        }
    } else if (definition->kind == SOL_IR_DEFINITION_ENUM) {
        for (size_t variant = 0; valid && variant < definition->variants.count;
            ++variant) {
            SolIrSlice fields
                = ir->variants[definition->variants.offset + variant].fields;
            for (size_t field = 0; valid && field < fields.count; ++field) {
                valid = type_is_data_only(builder,
                    ir->fields[fields.offset + field].type, depth + 1);
            }
        }
    } else if (definition->kind == SOL_IR_DEFINITION_DISTINCT
        || definition->kind == SOL_IR_DEFINITION_REFINED) {
        valid = type_is_data_only(builder, definition->representation, depth + 1);
    } else {
        valid = false;
    }
    return valid;
}

static bool import_safe(Builder *builder, SolIrCallableId id) {
    const SolIr *ir = builder->program->ir;
    if (id >= ir->callable_count) return false;
    const SolIrCallable *callable = &ir->callables[id];
    if (callable->kind != SOL_IR_CALLABLE_CAPABILITY
        || callable->body != SOL_IR_NONE
        || callable->generic_parameters.count != 0
        || callable->effect_parameters.count != 0
        || callable->effect_parameter != SOL_IR_NONE) return false;
    bool valid = type_is_data_only(builder, callable->result, 0);
    for (size_t index = 0; valid && index < callable->parameters.count; ++index) {
        SolIrLocalId local = ir->roots[callable->parameters.offset + index];
        valid = ir->locals[local].access != SOL_ACCESS_EXCLUSIVE
            && type_is_data_only(builder, ir->locals[local].type, 0);
    }
    return valid;
}

static bool approved(Builder *builder, SolIrCallableId callable) {
    const SolMirProgram *program = builder->program;
    size_t low = 0;
    size_t high = program->approved_import_count;
    while (low < high) {
        if (!charge_work(builder, 1)) return false;
        size_t middle = low + (high - low) / 2;
        SolIrCallableId item = program->approved_imports[middle];
        if (item < callable) low = middle + 1;
        else high = middle;
    }
    return low < program->approved_import_count
        && program->approved_imports[low] == callable;
}

static bool enqueue(Builder *builder, SolIrCallableId callable) {
    if (callable >= builder->program->ir->callable_count) {
        builder->outcome = SOL_MIR_PROGRAM_BUILD_INVALID_IR;
        report(builder, "source relation names an out-of-range callable");
        return false;
    }
    if (builder->classes[callable] == CLASS_UNSEEN) {
        builder->classes[callable] = CLASS_PENDING;
    }
    return true;
}

static bool add_incoming(Builder *builder, SolIrCallableId callable,
    SolIrEvidenceId evidence, SolIrGenericParameterId binding) {
    for (size_t index = 0; index < builder->incoming_count; ++index) {
        if (!charge_work(builder, 1)) return false;
        const IncomingContext *item = &builder->incoming[index];
        if (item->callable == callable && item->evidence == evidence
            && item->binding == binding) return true;
    }
    if (builder->incoming_count == SIZE_MAX) return false;
    size_t count = builder->incoming_count + 1;
    if (!grow_array((void **)&builder->incoming, &builder->incoming_capacity,
            count, sizeof(*builder->incoming))) return false;
    builder->incoming[builder->incoming_count]
        = (IncomingContext){callable, evidence, binding};
    ++builder->incoming_count;
    return true;
}

static bool specialization_key_equal(const SolMirProgramSpecialization *item,
    const SolIrDispatchEvidence *evidence) {
    return item->trait == evidence->trait
        && item->requirement == evidence->requirement
        && item->type == evidence->type
        && item->implementation == evidence->implementation
        && item->method == evidence->method;
}

static bool add_specialization(Builder *builder,
    const SolIrDispatchEvidence *evidence, SolIrCallableId source_callable,
    SolIrExpressionId expression, SolSpan span) {
    SolMirProgram *program = builder->program;
    SolMirProgramSource source;
    if (!source_for(builder, source_callable, expression, span, &source)) return false;
    for (size_t index = 0; index < program->specialization_count; ++index) {
        if (!charge_work(builder, 1)) return false;
        SolMirProgramSpecialization *item = &program->specializations[index];
        if (!specialization_key_equal(item, evidence)) continue;
        if (item->source_count == SIZE_MAX) return false;
        ++item->source_count;
        if (compare_source(source, item->first_source) < 0) item->first_source = source;
        return enqueue(builder, evidence->method);
    }
    if (program->specialization_count == SIZE_MAX) return false;
    size_t count = program->specialization_count + 1;
    if (count > SIZE_MAX / sizeof(*program->specializations)) return false;
    void *grown = realloc(program->specializations,
        count * sizeof(*program->specializations));
    if (grown == NULL) return false;
    program->specializations = grown;
    SolMirProgramSpecialization *item
        = &program->specializations[program->specialization_count++];
    memset(item, 0, sizeof(*item));
    item->trait = evidence->trait;
    item->requirement = evidence->requirement;
    item->type = evidence->type;
    item->implementation = evidence->implementation;
    item->method = evidence->method;
    item->first_source = source;
    item->source_count = 1;
    return enqueue(builder, evidence->method);
}

static bool propagate_forwarded(Builder *builder, SolIrCallableId callable,
    SolIrCallableId incoming_target, const SolIrDispatchEvidence *forwarded,
    SolIrExpressionId expression, SolSpan span, bool materialize,
    size_t *match_count, bool *changed) {
    const SolIr *ir = builder->program->ir;
    size_t incoming_count = builder->incoming_count;
    *match_count = 0;
    for (size_t index = 0; index < incoming_count; ++index) {
        if (!charge_work(builder, 1)) return false;
        const IncomingContext context = builder->incoming[index];
        if (context.callable != callable
            || context.binding != forwarded->parameter) continue;
        const SolIrDispatchEvidence *candidate
            = &ir->evidence[context.evidence];
        if (!candidate->forwarded && candidate->trait == forwarded->trait
            && candidate->requirement == forwarded->requirement) {
            if (materialize) {
                if (!add_specialization(builder, candidate, callable,
                        expression, span)) return false;
            } else {
                CallableClass method_class = builder->classes[candidate->method];
                if (!enqueue(builder, candidate->method)) return false;
                *changed = *changed || method_class == CLASS_UNSEEN;
                if (incoming_target != SOL_IR_NONE) {
                    size_t before = builder->incoming_count;
                    if (!add_incoming(builder, incoming_target, context.evidence,
                            forwarded->binding)) return false;
                    *changed = *changed || builder->incoming_count != before;
                }
            }
            ++*match_count;
        }
    }
    return true;
}

static bool scan_evidence(Builder *builder, SolIrCallableId source_callable,
    SolIrCallableId incoming_target, SolIrExpressionId expression, SolSpan span,
    SolIrSlice slice) {
    const SolIr *ir = builder->program->ir;
    for (size_t index = 0; index < slice.count; ++index) {
        if (!charge_work(builder, 1)) return false;
        SolIrEvidenceId id = slice.offset + index;
        const SolIrDispatchEvidence *evidence = &ir->evidence[id];
        if (evidence->forwarded) {
            bool retained = false;
            for (size_t use = 0; use < builder->forwarded_count; ++use) {
                if (!charge_work(builder, 1)) return false;
                ForwardedUse *item = &builder->forwarded[use];
                retained = retained || (item->callable == source_callable
                    && item->incoming_target == incoming_target
                    && item->evidence == id
                    && item->expression == expression
                    && item->span.start == span.start
                    && item->span.end == span.end);
            }
            if (retained) continue;
            if (builder->forwarded_count == SIZE_MAX
                || !grow_array((void **)&builder->forwarded,
                    &builder->forwarded_capacity,
                    builder->forwarded_count + 1,
                    sizeof(*builder->forwarded))) return false;
            ForwardedUse *item
                = &builder->forwarded[builder->forwarded_count++];
            item->callable = source_callable;
            item->incoming_target = incoming_target;
            item->evidence = id;
            item->expression = expression;
            item->span = span;
        } else {
            if (!add_specialization(builder, evidence, source_callable,
                    expression, span)) return false;
            if (incoming_target != SOL_IR_NONE
                && !add_incoming(builder, incoming_target, id,
                    evidence->binding)) return false;
        }
    }
    return true;
}

static bool scan_expression(Builder *builder, SolIrCallableId source_callable,
    SolIrExpressionId id, bool predicate, size_t depth);

static bool scan_statement_expression(Builder *builder,
    SolIrCallableId source_callable, const SolIrStatement *statement,
    bool predicate, size_t depth) {
    if (statement->target != SOL_IR_NONE
        && !scan_expression(builder, source_callable, statement->target,
            predicate, depth + 1)) return false;
    if (statement->condition != SOL_IR_NONE
        && !scan_expression(builder, source_callable, statement->condition,
            predicate, depth + 1)) return false;
    return statement->expression == SOL_IR_NONE
        || scan_expression(builder, source_callable, statement->expression,
            predicate, depth + 1);
}

static bool scan_static_callable(Builder *builder,
    SolIrCallableId source_callable, SolIrExpressionId id, bool predicate,
    size_t depth, bool *finite) {
    const SolIr *ir = builder->program->ir;
    *finite = false;
    if (id == SOL_IR_NONE || id >= ir->expression_count
        || depth > ir->expression_count) return true;
    if (!charge_work(builder, 1)) return false;
    const SolIrExpression *expression = &ir->expressions[id];
    if (expression->kind == SOL_IR_EXPR_DEFINITION) {
        SolIrDefinitionId definition = expression->as.definition;
        if (definition >= ir->definition_count
            || ir->definitions[definition].callable == SOL_IR_NONE) return true;
        SolIrCallableId target = ir->definitions[definition].callable;
        if (!add_reference(builder, predicate
                ? SOL_MIR_PROGRAM_REFERENCE_PREDICATE_FUNCTION_VALUE
                : SOL_MIR_PROGRAM_REFERENCE_FUNCTION_VALUE,
                source_callable, id, expression->span, target)
            || !enqueue(builder, target)) return false;
        *finite = true;
    } else if (expression->kind == SOL_IR_EXPR_BOUND_OPERATION) {
        if (!add_reference(builder, SOL_MIR_PROGRAM_REFERENCE_BOUND_OPERATION,
                source_callable, id, expression->span,
                expression->as.operation.callable)
            || !enqueue(builder, expression->as.operation.callable)) return false;
        *finite = true;
    }
    return true;
}

static bool scan_expression(Builder *builder, SolIrCallableId source_callable,
    SolIrExpressionId id, bool predicate, size_t depth) {
    const SolIr *ir = builder->program->ir;
    if (id == SOL_IR_NONE) return true;
    if (!charge_work(builder, 1)) return false;
    if (id >= ir->expression_count || depth > ir->expression_count) {
        builder->outcome = SOL_MIR_PROGRAM_BUILD_INVALID_IR;
        return false;
    }
    const SolIrExpression *expression = &ir->expressions[id];
#define SCAN(child) do { if (!scan_expression(builder, source_callable, (child), \
        predicate, depth + 1)) return false; } while (0)
    switch (expression->kind) {
        case SOL_IR_EXPR_DEFINITION: {
            SolIrDefinitionId definition = expression->as.definition;
            if (definition < ir->definition_count
                && ir->definitions[definition].callable != SOL_IR_NONE) {
                SolIrCallableId target = ir->definitions[definition].callable;
                if (!add_reference(builder, predicate
                        ? SOL_MIR_PROGRAM_REFERENCE_PREDICATE_FUNCTION_VALUE
                        : SOL_MIR_PROGRAM_REFERENCE_FUNCTION_VALUE,
                        source_callable, id, expression->span, target)
                    || !enqueue(builder, target)) return false;
            }
            break;
        }
        case SOL_IR_EXPR_BOUND_OPERATION:
            if (!add_reference(builder, SOL_MIR_PROGRAM_REFERENCE_BOUND_OPERATION,
                    source_callable, id, expression->span,
                    expression->as.operation.callable)
                || !enqueue(builder, expression->as.operation.callable)) return false;
            SCAN(expression->as.operation.receiver);
            break;
        case SOL_IR_EXPR_UNARY: SCAN(expression->as.unary.operand); break;
        case SOL_IR_EXPR_BINARY:
            SCAN(expression->as.binary.left);
            SCAN(expression->as.binary.right);
            break;
        case SOL_IR_EXPR_CALL: {
            const SolIrSlice evidence = expression->as.call.evidence;
            if (predicate && expression->as.call.kind == SOL_IR_CALL_CALLBACK) {
                bool finite = false;
                if (!scan_static_callable(builder, source_callable,
                        expression->as.call.callee, true, depth + 1,
                        &finite)) return false;
                if (!finite) {
                    builder->outcome
                        = SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE;
                    report(builder,
                        "predicate callback has no statically finite callable producer");
                    return false;
                }
            }
            if (predicate && (expression->as.call.kind == SOL_IR_CALL_FUNCTION
                    || expression->as.call.kind == SOL_IR_CALL_METHOD)) {
                if (!add_reference(builder, SOL_MIR_PROGRAM_REFERENCE_PREDICATE_CALL,
                        source_callable, id, expression->span,
                        expression->as.call.callable)) return false;
            }
            if (predicate && expression->as.call.kind == SOL_IR_CALL_FUNCTION) {
                if (!enqueue(builder, expression->as.call.callable)
                    || !scan_evidence(builder, source_callable,
                        expression->as.call.callable, id, expression->span,
                        evidence)) return false;
            } else if (predicate
                && expression->as.call.kind == SOL_IR_CALL_METHOD
                && !scan_evidence(builder, source_callable, SOL_IR_NONE, id,
                    expression->span, evidence)) return false;
            /* A direct invoke already retains its callee site. Only values passed
               through operands are independent static callable producers. */
            if (expression->as.call.receiver != SOL_IR_NONE) SCAN(expression->as.call.receiver);
            for (size_t index = 0; index < expression->as.call.operands.count; ++index) {
                SCAN(ir->operands[expression->as.call.operands.offset + index].value);
            }
            break;
        }
        case SOL_IR_EXPR_RECORD:
            for (size_t index = 0; index < expression->as.record.fields.count; ++index) {
                SCAN(ir->operands[expression->as.record.fields.offset + index].value);
            }
            break;
        case SOL_IR_EXPR_TUPLE:
            for (size_t index = 0; index < expression->as.tuple.operands.count; ++index) {
                SCAN(ir->operands[expression->as.tuple.operands.offset + index].value);
            }
            break;
        case SOL_IR_EXPR_IF:
            SCAN(expression->as.if_expr.condition);
            SCAN(expression->as.if_expr.then_branch);
            SCAN(expression->as.if_expr.else_branch);
            break;
        case SOL_IR_EXPR_MATCH:
            SCAN(expression->as.match_expr.scrutinee);
            for (size_t index = 0; index < expression->as.match_expr.arms.count; ++index) {
                const SolIrArm *arm
                    = &ir->arms[ir->arm_ids[expression->as.match_expr.arms.offset + index]];
                if (arm->guard != SOL_IR_NONE) SCAN(arm->guard);
                SCAN(arm->body);
            }
            break;
        case SOL_IR_EXPR_BLOCK:
            for (size_t index = 0; index < expression->as.block.statements.count; ++index) {
                const SolIrStatement *statement = &ir->statements[ir->statement_ids[
                    expression->as.block.statements.offset + index]];
                if (!scan_statement_expression(builder, source_callable, statement,
                        predicate, depth)) return false;
            }
            break;
        case SOL_IR_EXPR_PROPAGATE: SCAN(expression->as.propagate.operand); break;
        case SOL_IR_EXPR_HANDLE:
            if (!add_reference(builder, SOL_MIR_PROGRAM_REFERENCE_HANDLER_SOURCE,
                    source_callable, id, expression->span, expression->as.handler.source)
                || !enqueue(builder, expression->as.handler.source)
                || !add_reference(builder, SOL_MIR_PROGRAM_REFERENCE_HANDLER_PROVIDER,
                    source_callable, id, expression->span,
                    expression->as.handler.provider_callable)
                || !enqueue(builder, expression->as.handler.provider_callable)) return false;
            SCAN(expression->as.handler.authority);
            SCAN(expression->as.handler.provider);
            SCAN(expression->as.handler.body);
            break;
        default: break;
    }
#undef SCAN
    return true;
}

static bool scan_predicates(Builder *builder, SolIrCallableId callable,
    const SolMir *mir) {
    const SolIr *ir = builder->program->ir;
    SolIrDefinitionId owner = ir->callables[callable].owner;
    for (size_t index = 0; index < ir->obligation_count; ++index) {
        if (!charge_work(builder, 1)) return false;
        const SolIrObligation *obligation = &ir->obligations[index];
        if (obligation->owner_kind == SOL_CONTRACT_OWNER_ITEM
            && obligation->owner == owner
            && !scan_expression(builder, callable, obligation->predicate,
                true, 0)) return false;
    }
    for (size_t block = 0; block < mir->block_count; ++block) {
        const SolMirTerminator *term = &mir->blocks[block].terminator;
        if (term->kind != SOL_MIR_TERM_CHECK_REFINED) continue;
        for (size_t index = 0; index < ir->obligation_count; ++index) {
            if (!charge_work(builder, 1)) return false;
            const SolIrObligation *obligation = &ir->obligations[index];
            if (obligation->owner_kind == SOL_CONTRACT_OWNER_TYPE
                && obligation->owner == term->as.check_refined.definition
                && !scan_expression(builder, callable, obligation->predicate,
                    true, 0)) return false;
        }
        const SolIrExpression *construction
            = &ir->expressions[term->as.check_refined.source_expression];
        if (construction->kind == SOL_IR_EXPR_CALL
            && !scan_evidence(builder, callable, SOL_IR_NONE,
                term->as.check_refined.source_expression, term->span,
                construction->as.call.evidence)) return false;
    }
    return true;
}

static bool scan_mir(Builder *builder, const SolMir *mir) {
    const SolIr *ir = builder->program->ir;
    if (!scan_expression(builder, mir->callable,
            ir->callables[mir->callable].body, false, 0)) return false;
    for (size_t block = 0; block < mir->block_count; ++block) {
        if (!charge_work(builder, 1)) return false;
        const SolMirTerminator *term = &mir->blocks[block].terminator;
        if (term->kind != SOL_MIR_TERM_INVOKE) continue;
        if (!add_reference(builder, SOL_MIR_PROGRAM_REFERENCE_INVOKE,
                mir->callable, term->as.invoke.source_expression, term->span,
                term->as.invoke.callable)) return false;
        if (term->as.invoke.kind == SOL_IR_CALL_CALLBACK) {
            bool finite = false;
            SolMirTemporaryId temporary = term->as.invoke.callee;
            SolIrExpressionId callee = temporary < mir->temporary_count
                ? mir->temporaries[temporary].source_expression : SOL_IR_NONE;
            if (!scan_static_callable(builder, mir->callable, callee, false,
                    0, &finite)) return false;
            if (!finite) {
                builder->outcome = SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE;
                report(builder,
                    "callback invoke callee has no exact finite static producer");
                return false;
            }
            continue;
        }
        if (term->as.invoke.kind == SOL_IR_CALL_METHOD) {
            if (!enqueue(builder, term->as.invoke.callable)
                || !scan_evidence(builder, mir->callable, SOL_IR_NONE,
                    term->as.invoke.source_expression, term->span,
                    term->as.invoke.evidence)) return false;
        } else {
            if (!enqueue(builder, term->as.invoke.callable)
                || !scan_evidence(builder, mir->callable,
                    term->as.invoke.callable, term->as.invoke.source_expression,
                    term->span, term->as.invoke.evidence)) return false;
        }
    }
    return scan_predicates(builder, mir->callable, mir);
}

static bool append_import(Builder *builder, SolIrCallableId callable) {
    SolMirProgram *program = builder->program;
    SolMirProgramSource first = {SOL_IR_NONE, SOL_IR_NONE, 0, 0, 0};
    if (program->import_count == SIZE_MAX) return false;
    size_t next = program->import_count + 1;
    if (next > SIZE_MAX / sizeof(*program->imports)) return false;
    void *grown = realloc(program->imports, next * sizeof(*program->imports));
    if (grown == NULL) return false;
    program->imports = grown;
    SolMirProgramImport *item = &program->imports[program->import_count++];
    memset(item, 0, sizeof(*item));
    item->callable = callable;
    item->first_source = first;
    item->source_count = 0;
    return true;
}

static bool append_template(Builder *builder, SolIrCallableId callable,
    SolMir *mir) {
    SolMirProgram *program = builder->program;
    if (program->template_count == SIZE_MAX) return false;
    size_t count = program->template_count + 1;
    if (count > SIZE_MAX / sizeof(*program->templates)) return false;
    void *grown = realloc(program->templates, count * sizeof(*program->templates));
    if (grown == NULL) return false;
    program->templates = grown;
    SolMirProgramTemplate *item = &program->templates[program->template_count];
    memset(item, 0, sizeof(*item));
    item->callable = callable;
    item->mir = *mir;
    ++program->template_count;
    sol_mir_init(mir);
    return true;
}

static bool classify(Builder *builder, SolIrCallableId callable) {
    const SolIr *ir = builder->program->ir;
    const SolIrCallable *metadata = &ir->callables[callable];
    if (!charge_classification(builder)) return false;
    if (metadata->body == SOL_IR_NONE) {
        if (metadata->kind == SOL_IR_CALLABLE_CAPABILITY) {
            bool is_approved = approved(builder, callable);
            if (builder->outcome == SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED) {
                return false;
            }
            bool safe = is_approved && import_safe(builder, callable);
            if (builder->outcome == SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED) {
                return false;
            }
            if (!safe) {
                builder->outcome = SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE;
                report(builder, "bodyless capability target is unapproved or unsafe for the production host boundary");
                return false;
            }
            if (!append_import(builder, callable)) return false;
            builder->classes[callable] = CLASS_IMPORT;
            return true;
        }
        if (metadata->kind == SOL_IR_CALLABLE_TRAIT_REQUIREMENT) {
            builder->classes[callable] = CLASS_REQUIREMENT;
            return true;
        }
        builder->outcome = SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE;
        report(builder, "reachable bodyless callable is not an import or trait requirement");
        return false;
    }
    if (metadata->kind != SOL_IR_CALLABLE_FUNCTION
        && metadata->kind != SOL_IR_CALLABLE_TEST
        && metadata->kind != SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION) {
        builder->outcome = SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE;
        report(builder, "reachable bodyful callable is outside symbolic production MIR");
        return false;
    }
    SolMir mir;
    sol_mir_init(&mir);
    SolMirLowerOutcome lowered = sol_mir_lower_callable(ir, callable, &mir,
        builder->diagnostics);
    if (lowered == SOL_MIR_LOWER_UNSUPPORTED) {
        builder->outcome = SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE;
        sol_mir_free(&mir);
        return false;
    }
    if (lowered != SOL_MIR_LOWER_SUCCEEDED) {
        builder->outcome = builder->diagnostics->allocation_failed
            ? SOL_MIR_PROGRAM_BUILD_ALLOCATION_FAILED
            : SOL_MIR_PROGRAM_BUILD_INTERNAL_FAILED;
        sol_mir_free(&mir);
        return false;
    }
    builder->classes[callable] = CLASS_TEMPLATE;
    if (!scan_mir(builder, &mir) || !append_template(builder, callable, &mir)) {
        sol_mir_free(&mir);
        return false;
    }
    return true;
}

static bool root_valid(const SolIr *ir, SolMirProgramRoot root) {
    if (root.callable >= ir->callable_count
        || root.kind > SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE) return false;
    const SolIrCallable *callable = &ir->callables[root.callable];
    if (callable->body == SOL_IR_NONE) return false;
    if (root.kind == SOL_MIR_PROGRAM_ROOT_ENTRY) {
        return callable->kind == SOL_IR_CALLABLE_FUNCTION
            && ir->definitions[callable->owner].is_entrypoint;
    }
    if (root.kind == SOL_MIR_PROGRAM_ROOT_TEST) {
        return callable->kind == SOL_IR_CALLABLE_TEST;
    }
    return callable->kind == SOL_IR_CALLABLE_FUNCTION
        || callable->kind == SOL_IR_CALLABLE_TEST
        || callable->kind == SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION;
}

static bool root_requires_concrete_context(const SolIr *ir,
    SolMirProgramRoot root) {
    const SolIrCallable *callable = &ir->callables[root.callable];
    for (size_t index = 0; index < callable->generic_parameters.count; ++index) {
        SolIrGenericParameterId id = callable->generic_parameters.offset + index;
        if (ir->generic_parameters[id].trait_bound != SOL_IR_NONE) return true;
    }
    return false;
}

static bool copy_request_arrays(Builder *builder,
    const SolMirProgramBuildRequest *request) {
    SolMirProgram *program = builder->program;
    program->roots = array_allocate(request->root_count, sizeof(*program->roots));
    if (program->roots == NULL) return false;
    for (size_t index = 0; index < request->root_count; ++index) {
        program->roots[index].callable = request->roots[index].callable;
        program->roots[index].kind = request->roots[index].kind;
    }
    qsort(program->roots, request->root_count, sizeof(*program->roots), compare_root);
    for (size_t index = 0; index < request->root_count; ++index) {
        if (!root_valid(program->ir, program->roots[index])) {
            builder->outcome = SOL_MIR_PROGRAM_BUILD_INVALID_ARGUMENT;
            report(builder, "symbolic MIR program root has the wrong category or no body");
            return false;
        }
        if (root_requires_concrete_context(program->ir, program->roots[index])) {
            builder->outcome = SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE;
            report(builder,
                "trait-bounded generic root has no invocation-chain concrete context");
            return false;
        }
        if (index != 0 && program->roots[index - 1].callable
                == program->roots[index].callable) {
            if (program->roots[index - 1].kind != program->roots[index].kind) {
                builder->outcome = SOL_MIR_PROGRAM_BUILD_INVALID_ARGUMENT;
                report(builder, "symbolic MIR program root has conflicting categories");
                return false;
            }
            continue;
        }
        program->roots[program->root_count++] = program->roots[index];
    }
    if (program->root_count != request->root_count) {
        SolMirProgramRoot *canonical = array_allocate(program->root_count,
            sizeof(*canonical));
        if (canonical == NULL) return false;
        memcpy(canonical, program->roots,
            program->root_count * sizeof(*canonical));
        free(program->roots);
        program->roots = canonical;
    }
    if (!charge_sort(builder, program->root_count)) return false;
    if (!charge_work(builder, program->root_count)) return false;
    program->approved_imports = array_allocate(request->approved_import_count,
        sizeof(*program->approved_imports));
    if (request->approved_import_count != 0 && program->approved_imports == NULL) {
        return false;
    }
    if (request->approved_import_count != 0) {
        memcpy(program->approved_imports, request->approved_imports,
            request->approved_import_count * sizeof(*program->approved_imports));
        qsort(program->approved_imports, request->approved_import_count,
            sizeof(*program->approved_imports), compare_id);
    }
    for (size_t index = 0; index < request->approved_import_count; ++index) {
        SolIrCallableId callable = program->approved_imports[index];
        if (index != 0 && program->approved_imports[index - 1] == callable) continue;
        program->approved_imports[program->approved_import_count++] = callable;
    }
    if (program->approved_import_count != request->approved_import_count) {
        SolIrCallableId *canonical = array_allocate(program->approved_import_count,
            sizeof(*canonical));
        if (canonical == NULL) return false;
        memcpy(canonical, program->approved_imports,
            program->approved_import_count * sizeof(*canonical));
        free(program->approved_imports);
        program->approved_imports = canonical;
    }
    if (!charge_sort(builder, program->approved_import_count)) return false;
    for (size_t index = 0; index < program->approved_import_count; ++index) {
        if (!charge_work(builder, 1)) return false;
        SolIrCallableId callable = program->approved_imports[index];
        if (!import_safe(builder, callable)) {
            if (builder->outcome
                == SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED) return false;
            builder->outcome = SOL_MIR_PROGRAM_BUILD_INVALID_ARGUMENT;
            report(builder, "approved import is not a safe bodyless capability member");
            return false;
        }
    }
    return true;
}

static SolMirProgramBuildOutcome build_scratch(
    const SolMirProgramBuildRequest *request, SolMirProgram *scratch,
    SolDiagnostics *diagnostics) {
    Builder builder;
    memset(&builder, 0, sizeof(builder));
    builder.program = scratch;
    builder.diagnostics = diagnostics;
    builder.outcome = SOL_MIR_PROGRAM_BUILD_ALLOCATION_FAILED;
    scratch->ir = request->ir;
    scratch->limits = request->limits == NULL || limits_zero(*request->limits)
        ? sol_mir_program_default_limits() : *request->limits;
    if (!copy_request_arrays(&builder, request)) goto done;
    builder.classes = array_allocate(scratch->ir->callable_count,
        sizeof(*builder.classes));
    if (scratch->ir->callable_count != 0 && builder.classes == NULL) goto done;
    for (size_t index = 0; index < scratch->root_count; ++index) {
        if (!enqueue(&builder, scratch->roots[index].callable)) goto done;
    }
    for (;;) {
        SolIrCallableId next = SOL_IR_NONE;
        for (SolIrCallableId callable = 0; callable < scratch->ir->callable_count;
            ++callable) {
            if (!charge_work(&builder, 1)) goto done;
            if (builder.classes[callable] == CLASS_PENDING) {
                next = callable;
                break;
            }
        }
        if (next == SOL_IR_NONE) {
            bool changed = false;
            bool unresolved = false;
            for (size_t index = 0; index < builder.forwarded_count; ++index) {
                if (!charge_work(&builder, 1)) goto done;
                ForwardedUse *use = &builder.forwarded[index];
                if (use->evidence == SOL_IR_NONE) continue;
                const SolIrDispatchEvidence *forwarded
                    = &scratch->ir->evidence[use->evidence];
                size_t matches = 0;
                if (!propagate_forwarded(&builder, use->callable,
                        use->incoming_target, forwarded, use->expression,
                        use->span, false, &matches, &changed)) goto done;
                unresolved = unresolved || matches == 0;
            }
            if (changed) continue;
            if (unresolved) {
                builder.outcome = SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE;
                report(&builder, "forwarded trait evidence has no compatible incoming concrete demand");
                goto done;
            }
            for (size_t index = 0; index < builder.forwarded_count; ++index) {
                if (!charge_work(&builder, 1)) goto done;
                ForwardedUse *use = &builder.forwarded[index];
                if (use->evidence == SOL_IR_NONE) continue;
                const SolIrDispatchEvidence *forwarded
                    = &scratch->ir->evidence[use->evidence];
                size_t matches = 0;
                bool ignored = false;
                if (!propagate_forwarded(&builder, use->callable,
                        use->incoming_target, forwarded, use->expression,
                        use->span, true, &matches, &ignored)) goto done;
                use->evidence = SOL_IR_NONE;
            }
            break;
        }
        if (!charge_work(&builder, 1) || !classify(&builder, next)) goto done;
    }
    if (!charge_sort(&builder, scratch->reference_count)) goto done;
    if (scratch->reference_count > 1) qsort(scratch->references,
        scratch->reference_count, sizeof(*scratch->references), compare_reference);
    for (size_t import = 0; import < scratch->import_count; ++import) {
        SolMirProgramImport *item = &scratch->imports[import];
        item->source_count = 0;
        for (size_t reference = 0; reference < scratch->reference_count;
            ++reference) {
            if (!charge_work(&builder, 1)) goto done;
            const SolMirProgramReference *source = &scratch->references[reference];
            if (source->target != item->callable) continue;
            if (item->source_count == 0) item->first_source = source->source;
            ++item->source_count;
        }
    }
    if (!charge_sort(&builder, scratch->template_count)
        || !charge_sort(&builder, scratch->import_count)
        || !charge_sort(&builder, scratch->specialization_count)) goto done;
    if (scratch->template_count > 1) qsort(scratch->templates,
        scratch->template_count, sizeof(*scratch->templates), compare_template);
    if (scratch->import_count > 1) qsort(scratch->imports,
        scratch->import_count, sizeof(*scratch->imports), compare_import);
    if (scratch->specialization_count > 1) qsort(scratch->specializations,
        scratch->specialization_count, sizeof(*scratch->specializations),
        compare_specialization);
    builder.outcome = SOL_MIR_PROGRAM_BUILD_SUCCEEDED;
done:
    free(builder.classes);
    free(builder.incoming);
    free(builder.forwarded);
    return builder.outcome;
}

SolMirProgramBuildOutcome sol_mir_program_build(
    const SolMirProgramBuildRequest *request, SolMirProgram *program,
    SolDiagnostics *diagnostics) {
    if (request == NULL || program == NULL || diagnostics == NULL
        || !program_empty(program) || request->ir == NULL
        || request->root_count == 0 || request->roots == NULL
        || (request->approved_import_count != 0
            && request->approved_imports == NULL)
        || (request->limits != NULL && !limits_zero(*request->limits)
            && !limits_complete(*request->limits))) {
        if (diagnostics != NULL) sol_diagnostics_add(diagnostics,
            "SOL-MIR-PROGRAM-001", SOL_SEVERITY_ERROR, (SolSpan){0},
            "invalid symbolic MIR program build request or destination");
        return SOL_MIR_PROGRAM_BUILD_INVALID_ARGUMENT;
    }
    if (!sol_ir_validate(request->ir, diagnostics)) {
        return diagnostics->allocation_failed
            ? SOL_MIR_PROGRAM_BUILD_ALLOCATION_FAILED
            : SOL_MIR_PROGRAM_BUILD_INVALID_IR;
    }
    if (request->root_count > request->ir->callable_count
        || request->approved_import_count > request->ir->callable_count) {
        sol_diagnostics_add(diagnostics, "SOL-MIR-PROGRAM-001",
            SOL_SEVERITY_ERROR, (SolSpan){0},
            "symbolic MIR program request arrays exceed the callable domain");
        return SOL_MIR_PROGRAM_BUILD_INVALID_ARGUMENT;
    }
    SolMirProgram scratch;
    sol_mir_program_init(&scratch);
    SolMirProgramBuildOutcome outcome = build_scratch(request, &scratch, diagnostics);
    if (outcome == SOL_MIR_PROGRAM_BUILD_SUCCEEDED) {
        if (!sol_mir_program_validate(&scratch, diagnostics)) {
            outcome = diagnostics->allocation_failed
                ? SOL_MIR_PROGRAM_BUILD_ALLOCATION_FAILED
                : SOL_MIR_PROGRAM_BUILD_INTERNAL_FAILED;
        } else {
            *program = scratch;
            return outcome;
        }
    }
    sol_mir_program_free(&scratch);
    return outcome;
}

static bool mir_equal(const SolMir *a, const SolMir *b) {
#define BYTES(field, count) ((a->count == 0) || memcmp(a->field, b->field, \
        a->count * sizeof(*a->field)) == 0)
    return a->callable == b->callable && a->generic_parameters.offset
            == b->generic_parameters.offset
        && a->generic_parameters.count == b->generic_parameters.count
        && a->effect_parameters.offset == b->effect_parameters.offset
        && a->effect_parameters.count == b->effect_parameters.count
        && a->entry == b->entry && a->contract_body == b->contract_body
        && a->contract_epilogue == b->contract_epilogue
        && a->block_count == b->block_count
        && a->instruction_count == b->instruction_count
        && a->value_count == b->value_count
        && a->parameter_value_count == b->parameter_value_count
        && a->edge_value_count == b->edge_value_count
        && a->call_argument_count == b->call_argument_count
        && a->loop_count == b->loop_count
        && a->construct_operand_count == b->construct_operand_count
        && a->temporary_count == b->temporary_count
        && BYTES(blocks, block_count) && BYTES(instructions, instruction_count)
        && BYTES(values, value_count)
        && BYTES(parameter_values, parameter_value_count)
        && BYTES(edge_values, edge_value_count)
        && BYTES(call_arguments, call_argument_count) && BYTES(loops, loop_count)
        && BYTES(construct_operands, construct_operand_count)
        && BYTES(temporaries, temporary_count);
#undef BYTES
}

typedef struct {
    uintptr_t start;
    uintptr_t end;
} OwnedRange;

typedef struct {
    OwnedRange *items;
    size_t count;
    size_t capacity;
} OwnedRanges;

static bool owner_error(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics != NULL) {
        sol_diagnostics_add(diagnostics, "SOL-MIR-PROGRAM-002",
            SOL_SEVERITY_ERROR, (SolSpan){0}, message);
    }
    return false;
}

static bool pointer_is_canonical(size_t count, const void *pointer) {
    return (count == 0) == (pointer == NULL);
}

static bool owner_basic_headers_valid(const SolMirProgram *program,
    SolDiagnostics *diagnostics) {
    if (program == NULL || program->ir == NULL) {
        return owner_error(diagnostics, "symbolic MIR program header is null");
    }
    if (program->root_count == 0 || !limits_complete(program->limits)
        || program->reference_count > program->limits.max_references
        || program->usage.references != program->reference_count
        || program->usage.callable_classifications
            > program->limits.max_callable_classifications
        || program->usage.discovery_work > program->limits.max_discovery_work
        || !pointer_is_canonical(program->root_count, program->roots)
        || !pointer_is_canonical(program->approved_import_count,
            program->approved_imports)
        || !pointer_is_canonical(program->template_count, program->templates)
        || !pointer_is_canonical(program->import_count, program->imports)
        || !pointer_is_canonical(program->specialization_count,
            program->specializations)
        || !pointer_is_canonical(program->reference_count,
            program->references)) {
        return owner_error(diagnostics,
            "symbolic MIR program count, pointer, limit, or usage header is noncanonical");
    }
    return true;
}

static bool owner_domain_headers_valid(const SolMirProgram *program,
    SolDiagnostics *diagnostics) {
    const SolIr *ir = program->ir;
    if (program->root_count > ir->callable_count
        || program->approved_import_count > ir->callable_count
        || program->template_count > ir->callable_count
        || program->import_count > ir->callable_count
        || program->specialization_count > ir->evidence_count
        || ir->expression_count > SIZE_MAX / 7
        || (ir->expression_count == 0
            ? program->reference_count != 0
            : program->reference_count > ir->expression_count * 7)) {
        return owner_error(diagnostics,
            "symbolic MIR program count exceeds its validated owning-IR domain");
    }
    return true;
}

static bool add_owned_range(OwnedRanges *ranges, const void *pointer,
    size_t count, size_t size, SolDiagnostics *diagnostics) {
    if (count == 0 || pointer == NULL || size == 0 || count > SIZE_MAX / size) {
        return owner_error(diagnostics,
            "symbolic MIR program owned allocation range is malformed");
    }
    size_t bytes = count * size;
    uintptr_t start = (uintptr_t)pointer;
    if (bytes > UINTPTR_MAX - start) {
        return owner_error(diagnostics,
            "symbolic MIR program owned allocation range overflows");
    }
    uintptr_t end = start + bytes;
    for (size_t index = 0; index < ranges->count; ++index) {
        const OwnedRange *item = &ranges->items[index];
        if (start < item->end && item->start < end) {
            return owner_error(diagnostics,
                "symbolic MIR program owned allocation ranges overlap");
        }
    }
    if (ranges->count == SIZE_MAX
        || !grow_array((void **)&ranges->items, &ranges->capacity,
            ranges->count + 1, sizeof(*ranges->items))) {
        return owner_error(diagnostics,
            "allocation failed while validating symbolic MIR program ownership");
    }
    ranges->items[ranges->count++] = (OwnedRange){start, end};
    return true;
}

static bool add_optional_owned_range(OwnedRanges *ranges, const void *pointer,
    size_t count, size_t size, SolDiagnostics *diagnostics) {
    return count == 0
        || add_owned_range(ranges, pointer, count, size, diagnostics);
}

static bool add_mir_owned_range(OwnedRanges *ranges, const void *pointer,
    size_t count, size_t capacity, size_t size, SolDiagnostics *diagnostics) {
    if (count == 0) {
        if (capacity != 0 || pointer != NULL) {
            return owner_error(diagnostics,
                "zero-count nested MIR arena is not canonically null");
        }
        return true;
    }
    if (pointer == NULL || capacity < count) {
        return owner_error(diagnostics,
            "nested MIR arena count, capacity, or pointer is noncanonical");
    }
    return add_owned_range(ranges, pointer, capacity, size, diagnostics);
}

static bool owner_memory_valid(const SolMirProgram *program,
    SolDiagnostics *diagnostics) {
    OwnedRanges ranges = {0};
#define TOP_RANGE(pointer, count) \
    if (!add_optional_owned_range(&ranges, (pointer), (count), \
            sizeof(*(pointer)), diagnostics)) goto invalid
    TOP_RANGE(program->roots, program->root_count);
    TOP_RANGE(program->approved_imports, program->approved_import_count);
    TOP_RANGE(program->templates, program->template_count);
    TOP_RANGE(program->imports, program->import_count);
    TOP_RANGE(program->specializations, program->specialization_count);
    TOP_RANGE(program->references, program->reference_count);
#undef TOP_RANGE
    for (size_t index = 0; index < program->template_count; ++index) {
        const SolMir *mir = &program->templates[index].mir;
#define MIR_RANGE(pointer, count, capacity) \
        if (!add_mir_owned_range(&ranges, (pointer), (count), (capacity), \
                sizeof(*(pointer)), diagnostics)) goto invalid
        MIR_RANGE(mir->blocks, mir->block_count, mir->block_capacity);
        MIR_RANGE(mir->instructions, mir->instruction_count,
            mir->instruction_capacity);
        MIR_RANGE(mir->values, mir->value_count, mir->value_capacity);
        MIR_RANGE(mir->parameter_values, mir->parameter_value_count,
            mir->parameter_value_capacity);
        MIR_RANGE(mir->edge_values, mir->edge_value_count,
            mir->edge_value_capacity);
        MIR_RANGE(mir->call_arguments, mir->call_argument_count,
            mir->call_argument_capacity);
        MIR_RANGE(mir->loops, mir->loop_count, mir->loop_capacity);
        MIR_RANGE(mir->construct_operands, mir->construct_operand_count,
            mir->construct_operand_capacity);
        MIR_RANGE(mir->temporaries, mir->temporary_count,
            mir->temporary_capacity);
#undef MIR_RANGE
    }
    free(ranges.items);
    return true;
invalid:
    free(ranges.items);
    return false;
}

bool sol_mir_program_validate(const SolMirProgram *program,
    SolDiagnostics *diagnostics) {
    SolDiagnostics local_diagnostics;
    bool owns_diagnostics = diagnostics == NULL;
    if (owns_diagnostics) {
        sol_diagnostics_init(&local_diagnostics);
        diagnostics = &local_diagnostics;
    }
    if (!owner_basic_headers_valid(program, diagnostics)) {
        if (owns_diagnostics) sol_diagnostics_free(&local_diagnostics);
        return false;
    }
    if (!sol_ir_validate(program->ir, diagnostics)
        || !owner_domain_headers_valid(program, diagnostics)
        || !owner_memory_valid(program, diagnostics)) {
        if (owns_diagnostics) sol_diagnostics_free(&local_diagnostics);
        return false;
    }
    for (size_t index = 0; index < program->template_count; ++index) {
        const SolMirProgramTemplate *item = &program->templates[index];
        if (item->callable >= program->ir->callable_count
            || item->mir.callable != item->callable
            || (index != 0
                && program->templates[index - 1].callable >= item->callable)) {
            owner_error(diagnostics,
                "symbolic MIR program template identity or order is noncanonical");
            if (owns_diagnostics) sol_diagnostics_free(&local_diagnostics);
            return false;
        }
        if (!sol_mir_validate(program->ir, &item->mir, diagnostics)) {
            if (owns_diagnostics) sol_diagnostics_free(&local_diagnostics);
            return false;
        }
    }
    SolMirProgramBuildRequest request = {
        program->ir, program->roots, program->root_count,
        program->approved_imports, program->approved_import_count, &program->limits,
    };
    SolMirProgram expected;
    sol_mir_program_init(&expected);
    SolMirProgramBuildOutcome outcome
        = build_scratch(&request, &expected, diagnostics);
    bool valid = outcome == SOL_MIR_PROGRAM_BUILD_SUCCEEDED
        && program->root_count == expected.root_count
        && program->approved_import_count == expected.approved_import_count
        && program->template_count == expected.template_count
        && program->import_count == expected.import_count
        && program->specialization_count == expected.specialization_count
        && program->reference_count == expected.reference_count
        && memcmp(&program->usage, &expected.usage,
            sizeof(program->usage)) == 0
        && memcmp(program->roots, expected.roots,
            program->root_count * sizeof(*program->roots)) == 0
        && (program->approved_import_count == 0
            || memcmp(program->approved_imports, expected.approved_imports,
                program->approved_import_count
                    * sizeof(*program->approved_imports)) == 0)
        && (program->import_count == 0
            || memcmp(program->imports, expected.imports,
                program->import_count * sizeof(*program->imports)) == 0)
        && (program->specialization_count == 0
            || memcmp(program->specializations, expected.specializations,
                program->specialization_count
                    * sizeof(*program->specializations)) == 0)
        && (program->reference_count == 0
            || memcmp(program->references, expected.references,
                program->reference_count * sizeof(*program->references)) == 0);
    for (size_t index = 0; valid && index < program->template_count; ++index) {
        valid = program->templates[index].callable
                == expected.templates[index].callable
            && mir_equal(&program->templates[index].mir,
                &expected.templates[index].mir);
    }
    sol_mir_program_free(&expected);
    if (!valid && diagnostics != NULL) {
        sol_diagnostics_add(diagnostics, "SOL-MIR-PROGRAM-001",
            SOL_SEVERITY_ERROR, (SolSpan){0},
            "symbolic MIR program is not the canonical authentic closure");
    }
    if (owns_diagnostics) sol_diagnostics_free(&local_diagnostics);
    return valid;
}
