#include "support/test_progress.h"
#define main sol_test_body
#include "sol/mir_program.h"
#include "sol/effects.h"
#include "sol/lexer.h"
#include "sol/ownership.h"
#include "sol/package.h"

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

bool sol_mir_program_test_mir_equal(const SolMir *a, const SolMir *b);
bool sol_mir_program_test_instruction_equal(const SolMirInstruction *a,
    const SolMirInstruction *b);
bool sol_mir_program_test_terminator_equal(const SolMirTerminator *a,
    const SolMirTerminator *b);
bool sol_mir_program_test_program_equal(const SolMirProgram *a,
    const SolMirProgram *b);

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
            #condition); \
        ++failures; \
    } \
} while (0)

static void flip_scalar_byte(void *field, size_t size) {
    if (size != 0) ((unsigned char *)field)[0] ^= 1u;
}

typedef struct {
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree syntax;
    SolHirModule hir;
    SolTypeTable types;
    SolEffectTable effects;
    SolContractTable contracts;
    SolIr ir;
} Compilation;

static bool compile_text(Compilation *compilation, const char *text) {
    memset(compilation, 0, sizeof(*compilation));
    sol_tokens_init(&compilation->tokens);
    sol_diagnostics_init(&compilation->diagnostics);
    sol_syntax_tree_init(&compilation->syntax);
    sol_hir_module_init(&compilation->hir);
    sol_type_table_init(&compilation->types);
    sol_effect_table_init(&compilation->effects);
    sol_contract_table_init(&compilation->contracts);
    sol_ir_init(&compilation->ir);
    return sol_source_from_text(&compilation->source, "mir_program.sol", text)
        && sol_lex(&compilation->source, &compilation->tokens,
            &compilation->diagnostics)
        && sol_parse(&compilation->source, &compilation->tokens,
            &compilation->syntax, &compilation->diagnostics)
        && sol_hir_lower(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->diagnostics)
        && sol_type_check(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->types, &compilation->diagnostics)
        && sol_effect_check(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->diagnostics)
        && sol_contract_lower(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->contracts, &compilation->diagnostics)
        && sol_ir_lower(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->contracts, &compilation->ir,
            &compilation->diagnostics);
}

static void free_compilation(Compilation *compilation) {
    sol_ir_free(&compilation->ir);
    sol_contract_table_free(&compilation->contracts);
    sol_effect_table_free(&compilation->effects);
    sol_type_table_free(&compilation->types);
    sol_hir_module_free(&compilation->hir);
    sol_syntax_tree_free(&compilation->syntax);
    sol_tokens_free(&compilation->tokens);
    sol_source_free(&compilation->source);
    sol_diagnostics_free(&compilation->diagnostics);
}

static SolIrCallableId callable(const SolIr *ir, const char *name,
    SolIrCallableKind kind) {
    for (SolIrCallableId id = 0; id < ir->callable_count; ++id) {
        if (ir->callables[id].kind == kind
            && strcmp(ir->callables[id].name, name) == 0) return id;
    }
    return SOL_IR_NONE;
}

static bool has_template(const SolMirProgram *program, SolIrCallableId id) {
    for (size_t index = 0; index < program->template_count; ++index) {
        if (program->templates[index].callable == id) return true;
    }
    return false;
}

static bool has_import(const SolMirProgram *program, SolIrCallableId id) {
    for (size_t index = 0; index < program->import_count; ++index) {
        if (program->imports[index].callable == id) return true;
    }
    return false;
}

static SolMirProgramBuildOutcome build(const SolIr *ir,
    const SolMirProgramRoot *roots, size_t root_count,
    const SolIrCallableId *approvals, size_t approval_count,
    const SolMirProgramLimits *limits, SolMirProgram *program) {
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    SolMirProgramBuildRequest request = {
        ir, roots, root_count, approvals, approval_count, limits,
    };
    SolMirProgramBuildOutcome outcome
        = sol_mir_program_build(&request, program, &diagnostics);
    if (outcome == SOL_MIR_PROGRAM_BUILD_SUCCEEDED) {
        CHECK(sol_mir_program_validate(program, NULL));
    }
    if (outcome != SOL_MIR_PROGRAM_BUILD_SUCCEEDED
        && outcome != SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED
        && outcome != SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE
        && outcome != SOL_MIR_PROGRAM_BUILD_INVALID_ARGUMENT
        && outcome != SOL_MIR_PROGRAM_BUILD_INVALID_IR) {
        for (size_t index = 0; index < diagnostics.count; ++index) {
            fprintf(stderr, "program diagnostic: %s\n",
                diagnostics.items[index].message);
        }
    }
    sol_diagnostics_free(&diagnostics);
    return outcome;
}

static bool oracle_span_equal(SolSpan a, SolSpan b) {
    return a.start == b.start && a.end == b.end;
}

static bool oracle_mir_slice_equal(SolMirSlice a, SolMirSlice b) {
    return a.offset == b.offset && a.count == b.count;
}

static bool oracle_ir_slice_equal(SolIrSlice a, SolIrSlice b) {
    return a.offset == b.offset && a.count == b.count;
}

static bool oracle_place_equal(SolMirPlace a, SolMirPlace b) {
    return a.local == b.local && a.source_place == b.source_place;
}

static bool oracle_edge_equal(SolMirEdge a, SolMirEdge b) {
    return a.block == b.block
        && oracle_mir_slice_equal(a.arguments, b.arguments);
}

static bool oracle_call_argument_equal(SolMirCallArgument a,
    SolMirCallArgument b) {
    return a.formal == b.formal && a.access == b.access
        && a.source_expression == b.source_expression
        && a.temporary == b.temporary && a.place == b.place;
}

static bool oracle_instruction_equal(const SolMirInstruction *a,
    const SolMirInstruction *b) {
    if (a->kind != b->kind || a->block != b->block
        || a->result != b->result || a->type != b->type
        || a->source_expression != b->source_expression
        || !oracle_span_equal(a->span, b->span)) return false;
    switch (a->kind) {
        case SOL_MIR_INST_CONST_INT64:
            return a->as.integer == b->as.integer;
        case SOL_MIR_INST_CONST_BOOL:
            return a->as.boolean == b->as.boolean;
        case SOL_MIR_INST_CONST_TEXT:
        case SOL_MIR_INST_CONST_UNIT:
        case SOL_MIR_INST_HANDLER_ENTER:
        case SOL_MIR_INST_HANDLER_EXIT:
            return true;
        case SOL_MIR_INST_PARAMETER_LIVE:
        case SOL_MIR_INST_STORAGE_LIVE:
        case SOL_MIR_INST_DROP_IF_INITIALIZED:
        case SOL_MIR_INST_STORAGE_DEAD:
            return a->as.local == b->as.local;
        case SOL_MIR_INST_LOAD_COPY:
        case SOL_MIR_INST_LOAD_MOVE:
        case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED:
            return oracle_place_equal(a->as.place, b->as.place);
        case SOL_MIR_INST_LOAD_UPDATE:
            return a->as.update_load.statement == b->as.update_load.statement
                && oracle_place_equal(a->as.update_load.place,
                    b->as.update_load.place);
        case SOL_MIR_INST_STORE:
            return oracle_place_equal(a->as.store.place, b->as.store.place)
                && a->as.store.value == b->as.store.value;
        case SOL_MIR_INST_UNARY:
            return a->as.unary.operator_kind == b->as.unary.operator_kind
                && a->as.unary.operand == b->as.unary.operand;
        case SOL_MIR_INST_BINARY:
            return a->as.binary.operator_kind == b->as.binary.operator_kind
                && a->as.binary.left == b->as.binary.left
                && a->as.binary.right == b->as.binary.right;
        case SOL_MIR_INST_COMPOUND_UPDATE:
            return a->as.compound_update.statement
                    == b->as.compound_update.statement
                && a->as.compound_update.operator_kind
                    == b->as.compound_update.operator_kind
                && oracle_place_equal(a->as.compound_update.place,
                    b->as.compound_update.place)
                && a->as.compound_update.previous
                    == b->as.compound_update.previous
                && a->as.compound_update.right == b->as.compound_update.right;
        case SOL_MIR_INST_REGION_ENTER:
        case SOL_MIR_INST_REGION_EXIT:
            return a->as.region == b->as.region;
        case SOL_MIR_INST_TEMPORARY_INIT:
            return a->as.temporary_init.temporary
                    == b->as.temporary_init.temporary
                && a->as.temporary_init.value == b->as.temporary_init.value;
        case SOL_MIR_INST_TEMPORARY_DROP:
            return a->as.temporary_drop.temporary
                    == b->as.temporary_drop.temporary
                && a->as.temporary_drop.preserve_depth
                    == b->as.temporary_drop.preserve_depth;
        case SOL_MIR_INST_EXPRESSION_RESULT:
            return a->as.operand == b->as.operand;
        case SOL_MIR_INST_PATTERN_TEST:
        case SOL_MIR_INST_PATTERN_VALUE:
        case SOL_MIR_INST_MATCH_ARM:
            return a->as.pattern.match_expression
                    == b->as.pattern.match_expression
                && a->as.pattern.arm == b->as.pattern.arm
                && a->as.pattern.arm_ordinal == b->as.pattern.arm_ordinal
                && a->as.pattern.pattern == b->as.pattern.pattern
                && a->as.pattern.scrutinee == b->as.pattern.scrutinee;
        case SOL_MIR_INST_CONSTRUCT:
            return a->as.construct.kind == b->as.construct.kind
                && a->as.construct.definition == b->as.construct.definition
                && a->as.construct.variant == b->as.construct.variant
                && oracle_mir_slice_equal(a->as.construct.operands,
                    b->as.construct.operands)
                && oracle_ir_slice_equal(a->as.construct.capability_roots,
                    b->as.construct.capability_roots)
                && oracle_ir_slice_equal(a->as.construct.operation_roots,
                    b->as.construct.operation_roots);
        case SOL_MIR_INST_FUNCTION_VALUE:
            return a->as.function_callable == b->as.function_callable;
        case SOL_MIR_INST_CAPTURE_SNAPSHOT:
            return a->as.snapshot == b->as.snapshot;
        case SOL_MIR_INST_SCOPE_ENTER:
        case SOL_MIR_INST_SCOPE_EXIT:
            return a->as.scope.kind == b->as.scope.kind
                && a->as.scope.source == b->as.scope.source;
    }
    return false;
}

static bool oracle_terminator_equal(const SolMirTerminator *a,
    const SolMirTerminator *b) {
    if (a->kind != b->kind || !oracle_span_equal(a->span, b->span)) {
        return false;
    }
    switch (a->kind) {
        case SOL_MIR_TERM_GOTO:
            return oracle_edge_equal(a->as.go_to, b->as.go_to);
        case SOL_MIR_TERM_BRANCH:
            return a->as.branch.condition == b->as.branch.condition
                && oracle_edge_equal(a->as.branch.true_edge,
                    b->as.branch.true_edge)
                && oracle_edge_equal(a->as.branch.false_edge,
                    b->as.branch.false_edge);
        case SOL_MIR_TERM_RETURN:
        case SOL_MIR_TERM_PANIC:
            return a->as.value == b->as.value;
        case SOL_MIR_TERM_INVOKE:
            return a->as.invoke.source_expression
                    == b->as.invoke.source_expression
                && a->as.invoke.kind == b->as.invoke.kind
                && a->as.invoke.callable == b->as.invoke.callable
                && oracle_ir_slice_equal(a->as.invoke.type_arguments,
                    b->as.invoke.type_arguments)
                && oracle_ir_slice_equal(a->as.invoke.effects,
                    b->as.invoke.effects)
                && a->as.invoke.effect_parameter
                    == b->as.invoke.effect_parameter
                && oracle_ir_slice_equal(a->as.invoke.evidence,
                    b->as.invoke.evidence)
                && a->as.invoke.callee == b->as.invoke.callee
                && oracle_call_argument_equal(a->as.invoke.receiver,
                    b->as.invoke.receiver)
                && oracle_mir_slice_equal(a->as.invoke.arguments,
                    b->as.invoke.arguments)
                && a->as.invoke.result == b->as.invoke.result
                && oracle_edge_equal(a->as.invoke.normal_edge,
                    b->as.invoke.normal_edge)
                && oracle_edge_equal(a->as.invoke.failure_edge,
                    b->as.invoke.failure_edge);
        case SOL_MIR_TERM_RESUME_FAILURE:
            return true;
        case SOL_MIR_TERM_UNREACHABLE:
            return a->as.unreachable.statement == b->as.unreachable.statement
                && a->as.unreachable.obligation
                    == b->as.unreachable.obligation;
        case SOL_MIR_TERM_BREAK:
        case SOL_MIR_TERM_CONTINUE:
            return a->as.transfer.statement == b->as.transfer.statement
                && a->as.transfer.loop == b->as.transfer.loop
                && oracle_edge_equal(a->as.transfer.edge, b->as.transfer.edge);
        case SOL_MIR_TERM_CHECK_REFINED:
            return a->as.check_refined.source_expression
                    == b->as.check_refined.source_expression
                && a->as.check_refined.definition
                    == b->as.check_refined.definition
                && a->as.check_refined.obligation
                    == b->as.check_refined.obligation
                && a->as.check_refined.representation
                    == b->as.check_refined.representation
                && a->as.check_refined.result == b->as.check_refined.result
                && oracle_edge_equal(a->as.check_refined.normal_edge,
                    b->as.check_refined.normal_edge)
                && oracle_edge_equal(a->as.check_refined.failure_edge,
                    b->as.check_refined.failure_edge);
        case SOL_MIR_TERM_MATCH_FAILURE:
            return a->as.match_failure == b->as.match_failure;
        case SOL_MIR_TERM_PROPAGATE:
            return a->as.propagate.source_expression
                    == b->as.propagate.source_expression
                && a->as.propagate.kind == b->as.propagate.kind
                && a->as.propagate.operand == b->as.propagate.operand
                && a->as.propagate.value_result
                    == b->as.propagate.value_result
                && a->as.propagate.residual_result
                    == b->as.propagate.residual_result
                && oracle_edge_equal(a->as.propagate.value_edge,
                    b->as.propagate.value_edge)
                && oracle_edge_equal(a->as.propagate.residual_edge,
                    b->as.propagate.residual_edge);
        case SOL_MIR_TERM_CHECK_CONTRACT:
            return a->as.check_contract.obligation
                    == b->as.check_contract.obligation
                && a->as.check_contract.phase == b->as.check_contract.phase
                && a->as.check_contract.outcome == b->as.check_contract.outcome
                && a->as.check_contract.result == b->as.check_contract.result
                && oracle_edge_equal(a->as.check_contract.satisfied_edge,
                    b->as.check_contract.satisfied_edge)
                && oracle_edge_equal(a->as.check_contract.violation_edge,
                    b->as.check_contract.violation_edge)
                && oracle_edge_equal(a->as.check_contract.failure_edge,
                    b->as.check_contract.failure_edge);
        case SOL_MIR_TERM_CONTRACT_VIOLATION:
            return a->as.contract_violation == b->as.contract_violation;
        case SOL_MIR_TERM_INVALID:
            return false;
    }
    return false;
}

static bool oracle_mir_equal(const SolMir *a, const SolMir *b) {
    if (a->callable != b->callable
        || !oracle_ir_slice_equal(a->generic_parameters, b->generic_parameters)
        || !oracle_ir_slice_equal(a->effect_parameters, b->effect_parameters)
        || a->entry != b->entry || a->contract_body != b->contract_body
        || a->contract_epilogue != b->contract_epilogue
        || a->block_count != b->block_count
        || a->instruction_count != b->instruction_count
        || a->value_count != b->value_count
        || a->parameter_value_count != b->parameter_value_count
        || a->edge_value_count != b->edge_value_count
        || a->call_argument_count != b->call_argument_count
        || a->loop_count != b->loop_count
        || a->construct_operand_count != b->construct_operand_count
        || a->temporary_count != b->temporary_count) return false;
    for (size_t index = 0; index < a->block_count; ++index) {
        const SolMirBlock *x = &a->blocks[index], *y = &b->blocks[index];
        if (x->id != y->id || x->order != y->order
            || !oracle_mir_slice_equal(x->parameters, y->parameters)
            || !oracle_mir_slice_equal(x->instructions, y->instructions)
            || !oracle_terminator_equal(&x->terminator, &y->terminator)
            || !oracle_span_equal(x->span, y->span)
            || x->started != y->started) return false;
    }
    for (size_t index = 0; index < a->instruction_count; ++index) {
        if (!oracle_instruction_equal(&a->instructions[index],
                &b->instructions[index])) return false;
    }
    for (size_t index = 0; index < a->value_count; ++index) {
        const SolMirValue *x = &a->values[index], *y = &b->values[index];
        if (x->kind != y->kind || x->type != y->type || x->block != y->block
            || x->definition != y->definition
            || x->source_expression != y->source_expression
            || !oracle_span_equal(x->span, y->span)) return false;
    }
    for (size_t index = 0; index < a->parameter_value_count; ++index) {
        if (a->parameter_values[index] != b->parameter_values[index]) return false;
    }
    for (size_t index = 0; index < a->edge_value_count; ++index) {
        if (a->edge_values[index] != b->edge_values[index]) return false;
    }
    for (size_t index = 0; index < a->call_argument_count; ++index) {
        if (!oracle_call_argument_equal(a->call_arguments[index],
                b->call_arguments[index])) return false;
    }
    for (size_t index = 0; index < a->loop_count; ++index) {
        const SolMirLoop *x = &a->loops[index], *y = &b->loops[index];
        if (x->statement != y->statement || x->parent != y->parent
            || x->preheader != y->preheader || x->header != y->header
            || x->condition != y->condition || x->body != y->body
            || x->backedge != y->backedge || x->exit != y->exit
            || !oracle_ir_slice_equal(x->obligations, y->obligations)
            || !oracle_span_equal(x->span, y->span)) return false;
    }
    for (size_t index = 0; index < a->construct_operand_count; ++index) {
        const SolMirConstructOperand *x = &a->construct_operands[index];
        const SolMirConstructOperand *y = &b->construct_operands[index];
        if (x->formal != y->formal
            || x->source_expression != y->source_expression
            || x->temporary != y->temporary) return false;
    }
    for (size_t index = 0; index < a->temporary_count; ++index) {
        const SolMirTemporary *x = &a->temporaries[index];
        const SolMirTemporary *y = &b->temporaries[index];
        if (x->type != y->type || x->source_expression != y->source_expression
            || !oracle_span_equal(x->span, y->span)) return false;
    }
    return true;
}

static bool oracle_source_equal(SolMirProgramSource a,
    SolMirProgramSource b) {
    return a.callable == b.callable && a.expression == b.expression
        && a.file == b.file && a.start == b.start && a.end == b.end;
}

static bool program_fields_equal(const SolMirProgram *a,
    const SolMirProgram *b) {
    if (a->ir != b->ir || a->root_count != b->root_count
        || a->approved_import_count != b->approved_import_count
        || a->template_count != b->template_count
        || a->import_count != b->import_count
        || a->specialization_count != b->specialization_count
        || a->reference_count != b->reference_count
        || a->limits.max_callable_classifications
            != b->limits.max_callable_classifications
        || a->limits.max_references != b->limits.max_references
        || a->limits.max_discovery_work != b->limits.max_discovery_work
        || a->usage.callable_classifications
            != b->usage.callable_classifications
        || a->usage.references != b->usage.references
        || a->usage.discovery_work != b->usage.discovery_work) return false;
    for (size_t index = 0; index < a->root_count; ++index) {
        if (a->roots[index].callable != b->roots[index].callable
            || a->roots[index].kind != b->roots[index].kind) return false;
    }
    for (size_t index = 0; index < a->approved_import_count; ++index) {
        if (a->approved_imports[index] != b->approved_imports[index]) return false;
    }
    for (size_t index = 0; index < a->import_count; ++index) {
        const SolMirProgramImport *x = &a->imports[index], *y = &b->imports[index];
        if (x->callable != y->callable
            || !oracle_source_equal(x->first_source, y->first_source)
            || x->source_count != y->source_count) return false;
    }
    for (size_t index = 0; index < a->specialization_count; ++index) {
        const SolMirProgramSpecialization *x = &a->specializations[index];
        const SolMirProgramSpecialization *y = &b->specializations[index];
        if (x->trait != y->trait || x->requirement != y->requirement
            || x->type != y->type || x->implementation != y->implementation
            || x->method != y->method
            || !oracle_source_equal(x->first_source, y->first_source)
            || x->source_count != y->source_count) return false;
    }
    for (size_t index = 0; index < a->reference_count; ++index) {
        const SolMirProgramReference *x = &a->references[index];
        const SolMirProgramReference *y = &b->references[index];
        if (x->kind != y->kind || !oracle_source_equal(x->source, y->source)
            || x->target != y->target) return false;
    }
    for (size_t index = 0; index < a->template_count; ++index) {
        if (a->templates[index].callable != b->templates[index].callable
            || !oracle_mir_equal(&a->templates[index].mir,
                &b->templates[index].mir)) return false;
    }
    return true;
}

/* Independently reconstruct the bounded hostile proof's work census: one tick
 * per method invoke, every candidate reference comparison, and every source
 * file range considered by matching invoke references. */
static size_t direct_method_validation_work(const SolMirProgram *program) {
    const SolIr *ir = program->ir;
    size_t work = 0;
    for (size_t template_id = 0; template_id < program->template_count; ++template_id) {
        const SolMir *mir = &program->templates[template_id].mir;
        for (size_t block = 0; block < mir->block_count; ++block) {
            const SolMirTerminator *term = &mir->blocks[block].terminator;
            if (term->kind != SOL_MIR_TERM_INVOKE
                || term->as.invoke.kind != SOL_IR_CALL_METHOD) continue;
            ++work;
            const SolIrExpression *call = term->as.invoke.source_expression
                    < ir->expression_count
                ? &ir->expressions[term->as.invoke.source_expression] : NULL;
            if (call == NULL || call->kind != SOL_IR_EXPR_CALL
                || call->as.call.evidence.count != 1
                || call->as.call.evidence.offset >= ir->evidence_count
                || ir->evidence[call->as.call.evidence.offset].forwarded
                || call->as.call.type_arguments.count != 0) continue;
            for (size_t reference = 0; reference < program->reference_count;
                ++reference) {
                ++work;
                if (program->references[reference].kind
                    != SOL_MIR_PROGRAM_REFERENCE_INVOKE) continue;
                for (size_t file = 0; file < ir->file_count; ++file) {
                    ++work;
                    const SolIrSourceFile *item = &ir->files[file];
                    if (term->span.start >= item->aggregate_start
                        && term->span.end <= item->aggregate_end) break;
                }
            }
        }
    }
    return work;
}

static size_t capability_validation_work(const SolMirProgram *program) {
    const SolIr *ir = program->ir;
    size_t work = 0;
    for (size_t template_id = 0; template_id < program->template_count; ++template_id) {
        const SolMir *mir = &program->templates[template_id].mir;
        for (size_t block = 0; block < mir->block_count; ++block) {
            const SolMirTerminator *term = &mir->blocks[block].terminator;
            if (term->kind != SOL_MIR_TERM_INVOKE
                || term->as.invoke.kind != SOL_IR_CALL_CAPABILITY) continue;
            ++work;
            const SolIrExpression *call = term->as.invoke.source_expression
                    < ir->expression_count
                ? &ir->expressions[term->as.invoke.source_expression] : NULL;
            if (call == NULL || call->kind != SOL_IR_EXPR_CALL
                || call->as.call.callee >= ir->expression_count) continue;
            const SolIrExpression *callee = &ir->expressions[call->as.call.callee];
            for (size_t reference = 0; reference < program->reference_count;
                ++reference) {
                ++work;
                SolSpan span;
                if (program->references[reference].kind
                    == SOL_MIR_PROGRAM_REFERENCE_INVOKE) span = term->span;
                else if (program->references[reference].kind
                    == SOL_MIR_PROGRAM_REFERENCE_BOUND_OPERATION) span = callee->span;
                else continue;
                for (size_t file = 0; file < ir->file_count; ++file) {
                    ++work;
                    const SolIrSourceFile *item = &ir->files[file];
                    if (span.start >= item->aggregate_start
                        && span.end <= item->aggregate_end) break;
                }
            }
        }
    }
    return work;
}

static void check_invalid_owner(SolMirProgram *program,
    SolDiagnostics *diagnostics) {
    size_t before = diagnostics->count;
    CHECK(!sol_mir_program_validate(program, diagnostics));
    CHECK(diagnostics->allocation_failed || diagnostics->count > before);
}

static bool compile_package_at(Compilation *compilation, SolPackage *package,
    const char *directory) {
    memset(compilation, 0, sizeof(*compilation));
    sol_package_init(package);
    sol_diagnostics_init(&compilation->diagnostics);
    sol_hir_module_init(&compilation->hir);
    sol_type_table_init(&compilation->types);
    sol_effect_table_init(&compilation->effects);
    sol_contract_table_init(&compilation->contracts);
    sol_ir_init(&compilation->ir);
    char error[256];
    if (!sol_package_load_directory(package, directory,
            &compilation->diagnostics, error, sizeof(error))) return false;
    SolHirFileScope *scopes = malloc(package->file_count * sizeof(*scopes));
    if (scopes == NULL) return false;
    for (size_t index = 0; index < package->file_count; ++index) {
        scopes[index] = (SolHirFileScope){
            package->files[index].module_name,
            package->files[index].import_start,
            package->files[index].import_count,
            package->files[index].item_start,
            package->files[index].item_count,
        };
    }
    bool valid = sol_hir_lower_scoped(&package->source, &package->syntax,
            scopes, package->file_count, &compilation->hir,
            &compilation->diagnostics)
        && sol_type_check(&package->source, &package->syntax, &compilation->hir,
            &compilation->types, &compilation->diagnostics)
        && sol_effect_check(&package->source, &package->syntax, &compilation->hir,
            &compilation->types, &compilation->effects,
            &compilation->diagnostics)
        && sol_contract_lower(&package->source, &package->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->contracts, &compilation->diagnostics)
        && sol_ir_lower_scoped(&package->source, &package->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->contracts, package->files, package->file_count,
            &compilation->ir, &compilation->diagnostics);
    free(scopes);
    return valid;
}

static bool compile_e6(Compilation *compilation, SolPackage *package) {
    return compile_package_at(compilation, package,
        SOL_TEST_SOURCE_DIR "/tests/conformance/e6");
}

static void free_e6(Compilation *compilation, SolPackage *package) {
    sol_ir_free(&compilation->ir);
    sol_contract_table_free(&compilation->contracts);
    sol_effect_table_free(&compilation->effects);
    sol_type_table_free(&compilation->types);
    sol_hir_module_free(&compilation->hir);
    sol_diagnostics_free(&compilation->diagnostics);
    sol_package_free(package);
}

static size_t first_scope_instruction(const SolMir *mir,
    SolMirInstructionKind kind) {
    for (size_t index = 0; index < mir->instruction_count; ++index) {
        if (mir->instructions[index].kind == kind) return index;
    }
    return SIZE_MAX;
}

static bool lower_first_scope_mir(const SolIr *ir, SolMir *mir,
    SolIrCallableId *callable_id, SolMirInstructionKind *scope_kind) {
    for (SolIrCallableId id = 0; id < ir->callable_count; ++id) {
        if (ir->callables[id].body == SOL_IR_NONE) continue;
        SolMir candidate;
        sol_mir_init(&candidate);
        SolDiagnostics diagnostics;
        sol_diagnostics_init(&diagnostics);
        SolMirLowerOutcome outcome
            = sol_mir_lower_callable(ir, id, &candidate, &diagnostics);
        sol_diagnostics_free(&diagnostics);
        if (outcome != SOL_MIR_LOWER_SUCCEEDED) {
            sol_mir_free(&candidate);
            continue;
        }
        for (size_t index = 0; index < candidate.instruction_count; ++index) {
            SolMirInstructionKind kind = candidate.instructions[index].kind;
            if (kind != SOL_MIR_INST_SCOPE_ENTER
                && kind != SOL_MIR_INST_SCOPE_EXIT) continue;
            *mir = candidate;
            *callable_id = id;
            *scope_kind = kind;
            return true;
        }
        sol_mir_free(&candidate);
    }
    return false;
}

static bool copy_mir_arena_bytes(SolMir *destination, const SolMir *source) {
#define COPY_MIR_ARENA(member, count) do { \
    if (destination->count != source->count \
        || source->count > SIZE_MAX / sizeof(*source->member)) return false; \
    if (source->count != 0) { \
        if (destination->member == NULL || source->member == NULL) return false; \
        memcpy(destination->member, source->member, \
            source->count * sizeof(*source->member)); \
    } \
} while (0)
    COPY_MIR_ARENA(blocks, block_count);
    COPY_MIR_ARENA(instructions, instruction_count);
    COPY_MIR_ARENA(values, value_count);
    COPY_MIR_ARENA(parameter_values, parameter_value_count);
    COPY_MIR_ARENA(edge_values, edge_value_count);
    COPY_MIR_ARENA(call_arguments, call_argument_count);
    COPY_MIR_ARENA(loops, loop_count);
    COPY_MIR_ARENA(construct_operands, construct_operand_count);
    COPY_MIR_ARENA(temporaries, temporary_count);
#undef COPY_MIR_ARENA
    return true;
}

static void test_canonical_representation(void) {
    Compilation compilation;
    SolPackage package;
    SolMir original, representation;
    sol_mir_init(&original);
    sol_mir_init(&representation);
    CHECK(compile_e6(&compilation, &package));
    SolIrCallableId callable_id = SOL_IR_NONE;
    SolMirInstructionKind scope_kind = SOL_MIR_INST_SCOPE_ENTER;
    bool lowered = lower_first_scope_mir(&compilation.ir, &original,
        &callable_id, &scope_kind);
    CHECK(lowered);
    if (!lowered) goto cleanup;
    CHECK(sol_mir_validate(&compilation.ir, &original, NULL));
    SolDiagnostics lower_diagnostics;
    sol_diagnostics_init(&lower_diagnostics);
    SolMirLowerOutcome duplicate_outcome = sol_mir_lower_callable(
        &compilation.ir, callable_id, &representation, &lower_diagnostics);
    sol_diagnostics_free(&lower_diagnostics);
    CHECK(duplicate_outcome == SOL_MIR_LOWER_SUCCEEDED);
    if (duplicate_outcome != SOL_MIR_LOWER_SUCCEEDED) goto cleanup;
    CHECK(sol_mir_validate(&compilation.ir, &representation, NULL));
    CHECK(original.instructions != representation.instructions
        && original.block_count == representation.block_count
        && original.instruction_count == representation.instruction_count);
    CHECK(copy_mir_arena_bytes(&representation, &original));
    CHECK(sol_mir_program_test_mir_equal(&original, &representation));

    size_t original_index = first_scope_instruction(&original, scope_kind);
    size_t representation_index
        = first_scope_instruction(&representation, scope_kind);
    CHECK(original_index != SIZE_MAX && representation_index != SIZE_MAX);
    if (original_index == SIZE_MAX || representation_index == SIZE_MAX) {
        goto cleanup;
    }
    size_t kind_offset = offsetof(SolMirInstruction, as.scope.kind);
    size_t kind_size = sizeof(((SolMirInstruction *)0)->as.scope.kind);
    size_t source_offset = offsetof(SolMirInstruction, as.scope.source);
    size_t padding_begin = kind_offset + kind_size;
    bool has_scope_padding = source_offset > padding_begin;
    size_t scope_size = sizeof(((SolMirInstruction *)0)->as.scope);
    size_t union_size = sizeof(((SolMirInstruction *)0)->as);
    bool has_inactive_union_tail = union_size > scope_size;
    SolMirInstruction *before = &original.instructions[original_index];
    SolMirInstruction *after = &representation.instructions[representation_index];
    fprintf(stderr,
        "canonical representation layout scope.kind=(%zu,%zu) "
        "scope.source=(%zu,%zu) padding=%s inactive_union_tail=%s\n",
        kind_offset, kind_size, source_offset,
        sizeof(((SolMirInstruction *)0)->as.scope.source),
        has_scope_padding ? "poisoned" : "NOT_RUN",
        has_inactive_union_tail ? "tested" : "NOT_RUN");
    if (!has_scope_padding) {
        fprintf(stderr,
            "canonical representation padding subcase NOT_RUN: "
            "no host gap between scope.kind and scope.source\n");
    }

    CHECK(before->kind == after->kind
        && before->block == after->block
        && before->result == after->result
        && before->type == after->type
        && before->source_expression == after->source_expression
        && before->span.start == after->span.start
        && before->span.end == after->span.end
        && before->as.scope.kind == after->as.scope.kind
        && before->as.scope.source == after->as.scope.source);
    unsigned char *bytes = (unsigned char *)after;
    unsigned char saved;
    if (has_scope_padding) {
        saved = bytes[padding_begin];
        bytes[padding_begin] ^= 1u;
        CHECK(before->kind == after->kind
            && before->block == after->block
            && before->result == after->result
            && before->type == after->type
            && before->source_expression == after->source_expression
            && before->span.start == after->span.start
            && before->span.end == after->span.end
            && before->as.scope.kind == after->as.scope.kind
            && before->as.scope.source == after->as.scope.source);
        CHECK(sol_mir_validate(&compilation.ir, &original, NULL));
        CHECK(sol_mir_validate(&compilation.ir, &representation, NULL));
        CHECK(sol_mir_program_test_mir_equal(&original, &representation));
        bytes[padding_begin] = saved;
    }
    CHECK(sol_mir_program_test_mir_equal(&original, &representation));
    CHECK(sol_mir_validate(&compilation.ir, &representation, NULL));

    /* These independent subcases must still run when padding is absent. */
    size_t active_union_end = offsetof(SolMirInstruction, as)
        + sizeof(after->as.scope);
    if (has_inactive_union_tail) {
        size_t inactive_offset = active_union_end;
        saved = bytes[inactive_offset];
        bytes[inactive_offset] ^= 1u;
        CHECK(before->kind == after->kind
            && before->as.scope.kind == after->as.scope.kind
            && before->as.scope.source == after->as.scope.source);
        CHECK(sol_mir_validate(&compilation.ir, &representation, NULL));
        CHECK(sol_mir_program_test_mir_equal(&original, &representation));
        bytes[inactive_offset] = saved;
    } else {
        fprintf(stderr,
            "canonical representation inactive-union subcase NOT_RUN: "
            "no inactive union tail on this host\n");
    }
    CHECK(sol_mir_program_test_mir_equal(&original, &representation));

    SolIrCallableId launch = callable(&compilation.ir, "launch",
        SOL_IR_CALLABLE_FUNCTION);
    const char *approval_names[] = {"write", "get", "count", "read"};
    SolIrCallableId approvals[4];
    for (size_t index = 0; index < 4; ++index) {
        approvals[index] = callable(&compilation.ir, approval_names[index],
            SOL_IR_CALLABLE_CAPABILITY);
        CHECK(approvals[index] != SOL_IR_NONE);
    }
    SolMirProgramRoot roots[5];
    roots[0] = (SolMirProgramRoot){launch, SOL_MIR_PROGRAM_ROOT_ENTRY};
    size_t root_count = 1;
    for (SolIrCallableId id = 0; id < compilation.ir.callable_count; ++id) {
        if (compilation.ir.callables[id].kind == SOL_IR_CALLABLE_TEST) {
            roots[root_count++] = (SolMirProgramRoot){id,
                SOL_MIR_PROGRAM_ROOT_TEST};
        }
    }
    SolMirProgram owner;
    sol_mir_program_init(&owner);
    CHECK(build(&compilation.ir, roots, root_count, approvals, 4, NULL, &owner)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(sol_mir_program_validate(&owner, NULL));
    SolMir *owned_mir = NULL;
    for (size_t index = 0; index < owner.template_count; ++index) {
        if (owner.templates[index].callable == callable_id) {
            owned_mir = &owner.templates[index].mir;
            break;
        }
    }
    CHECK(owned_mir != NULL);
    if (owned_mir != NULL) {
        size_t owned_index = first_scope_instruction(owned_mir, scope_kind);
        CHECK(owned_index != SIZE_MAX);
        if (owned_index != SIZE_MAX) {
            SolMirInstruction *owned = &owned_mir->instructions[owned_index];
            unsigned char *owned_bytes = (unsigned char *)owned;
            if (source_offset > padding_begin) {
                saved = owned_bytes[padding_begin];
                owned_bytes[padding_begin] ^= 1u;
                CHECK(sol_mir_program_validate(&owner, NULL));
                owned_bytes[padding_begin] = saved;
            }
            if (has_inactive_union_tail) {
                size_t inactive_offset = offsetof(SolMirInstruction, as)
                    + sizeof(owned->as.scope);
                saved = owned_bytes[inactive_offset];
                owned_bytes[inactive_offset] ^= 1u;
                CHECK(sol_mir_program_validate(&owner, NULL));
                owned_bytes[inactive_offset] = saved;
            }
        }
    }
    bool owner_valid_after_poison_restore
        = sol_mir_program_validate(&owner, NULL);
    CHECK(owner_valid_after_poison_restore);
    fprintf(stderr,
        "canonical representation public-owner padding=%s "
        "inactive_union_tail=%s final_validation=%s\n",
        has_scope_padding ? "tested" : "NOT_RUN",
        has_inactive_union_tail ? "tested" : "NOT_RUN",
        owner_valid_after_poison_restore ? "PASS" : "FAIL");
    sol_mir_program_free(&owner);

cleanup:
    sol_mir_free(&representation);
    sol_mir_free(&original);
    free_e6(&compilation, &package);
}

static void test_instruction_discriminants_and_fields(void) {
    SolMirInstruction left = {0}, right;
#define INST_DIFF(change) do { \
    right = left; \
    change; \
    CHECK(!sol_mir_program_test_instruction_equal(&left, &right)); \
} while (0)
#define INST_FIELD(field) INST_DIFF(flip_scalar_byte(&(field), sizeof(field)))
    left.kind = SOL_MIR_INST_SCOPE_ENTER;
    left.block = 3;
    left.result = 5;
    left.type = 7;
    left.source_expression = 9;
    left.span = (SolSpan){11, 13};
    left.as.scope = (SolMirScope){SOL_MIR_SCOPE_BLOCK, 17};
    CHECK(sol_mir_program_test_instruction_equal(&left, &left));
    INST_DIFF(right.kind = SOL_MIR_INST_SCOPE_EXIT);
    INST_FIELD(right.block);
    INST_FIELD(right.result);
    INST_FIELD(right.type);
    INST_FIELD(right.source_expression);
    INST_FIELD(right.span.start);
    INST_FIELD(right.span.end);
    INST_FIELD(right.as.scope.kind);
    INST_FIELD(right.as.scope.source);
    right = left;
    right.kind = (SolMirInstructionKind)-1;
    left.kind = (SolMirInstructionKind)-1;
    CHECK(!sol_mir_program_test_instruction_equal(&left, &right));

    /* Comparator-seam negatives only; these do not forge validated owners. */
    left = (SolMirInstruction){0};
    left.kind = SOL_MIR_INST_SCOPE_ENTER;
    left.as.scope.kind = SOL_MIR_SCOPE_INVALID;
    right = left;
    CHECK(!sol_mir_program_test_instruction_equal(&left, &right));
    left = (SolMirInstruction){0};
    left.kind = SOL_MIR_INST_CONSTRUCT;
    left.as.construct.kind = (SolMirConstructKind)-1;
    right = left;
    CHECK(!sol_mir_program_test_instruction_equal(&left, &right));

    for (int kind = SOL_MIR_INST_CONST_INT64;
        kind <= SOL_MIR_INST_FUNCTION_VALUE; ++kind) {
        left = (SolMirInstruction){0};
        left.kind = (SolMirInstructionKind)kind;
        if (left.kind == SOL_MIR_INST_SCOPE_ENTER
            || left.kind == SOL_MIR_INST_SCOPE_EXIT) {
            left.as.scope.kind = SOL_MIR_SCOPE_CALLABLE_ENVELOPE;
        }
        CHECK(sol_mir_program_test_instruction_equal(&left, &left));
    }

    left = (SolMirInstruction){0}; left.kind = SOL_MIR_INST_CONST_INT64;
    INST_FIELD(right.as.integer);
    left = (SolMirInstruction){0}; left.kind = SOL_MIR_INST_CONST_BOOL;
    INST_FIELD(right.as.boolean);
    left = (SolMirInstruction){0}; left.kind = SOL_MIR_INST_PARAMETER_LIVE;
    INST_FIELD(right.as.local);
    left.kind = SOL_MIR_INST_STORAGE_LIVE; INST_FIELD(right.as.local);
    left.kind = SOL_MIR_INST_DROP_IF_INITIALIZED; INST_FIELD(right.as.local);
    left.kind = SOL_MIR_INST_STORAGE_DEAD; INST_FIELD(right.as.local);
    left = (SolMirInstruction){0}; left.kind = SOL_MIR_INST_LOAD_COPY;
    INST_FIELD(right.as.place.local); INST_FIELD(right.as.place.source_place);
    left.kind = SOL_MIR_INST_LOAD_MOVE;
    INST_FIELD(right.as.place.local); INST_FIELD(right.as.place.source_place);
    left.kind = SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED;
    INST_FIELD(right.as.place.local); INST_FIELD(right.as.place.source_place);
    left = (SolMirInstruction){0}; left.kind = SOL_MIR_INST_LOAD_UPDATE;
    INST_FIELD(right.as.update_load.statement);
    INST_FIELD(right.as.update_load.place.local);
    INST_FIELD(right.as.update_load.place.source_place);
    left = (SolMirInstruction){0}; left.kind = SOL_MIR_INST_STORE;
    INST_FIELD(right.as.store.place.local);
    INST_FIELD(right.as.store.place.source_place);
    INST_FIELD(right.as.store.value);
    left = (SolMirInstruction){0}; left.kind = SOL_MIR_INST_UNARY;
    INST_FIELD(right.as.unary.operator_kind);
    INST_FIELD(right.as.unary.operand);
    left = (SolMirInstruction){0}; left.kind = SOL_MIR_INST_BINARY;
    INST_FIELD(right.as.binary.operator_kind);
    INST_FIELD(right.as.binary.left); INST_FIELD(right.as.binary.right);
    left = (SolMirInstruction){0}; left.kind = SOL_MIR_INST_COMPOUND_UPDATE;
    INST_FIELD(right.as.compound_update.statement);
    INST_FIELD(right.as.compound_update.operator_kind);
    INST_FIELD(right.as.compound_update.place.local);
    INST_FIELD(right.as.compound_update.place.source_place);
    INST_FIELD(right.as.compound_update.previous);
    INST_FIELD(right.as.compound_update.right);
    left = (SolMirInstruction){0}; left.kind = SOL_MIR_INST_REGION_ENTER;
    INST_FIELD(right.as.region);
    left.kind = SOL_MIR_INST_REGION_EXIT; INST_FIELD(right.as.region);
    left = (SolMirInstruction){0}; left.kind = SOL_MIR_INST_TEMPORARY_INIT;
    INST_FIELD(right.as.temporary_init.temporary);
    INST_FIELD(right.as.temporary_init.value);
    left = (SolMirInstruction){0}; left.kind = SOL_MIR_INST_TEMPORARY_DROP;
    INST_FIELD(right.as.temporary_drop.temporary);
    INST_FIELD(right.as.temporary_drop.preserve_depth);
    left = (SolMirInstruction){0}; left.kind = SOL_MIR_INST_EXPRESSION_RESULT;
    INST_FIELD(right.as.operand);
    left = (SolMirInstruction){0}; left.kind = SOL_MIR_INST_PATTERN_TEST;
    INST_FIELD(right.as.pattern.match_expression);
    INST_FIELD(right.as.pattern.arm); INST_FIELD(right.as.pattern.arm_ordinal);
    INST_FIELD(right.as.pattern.pattern); INST_FIELD(right.as.pattern.scrutinee);
    left.kind = SOL_MIR_INST_PATTERN_VALUE;
    INST_FIELD(right.as.pattern.match_expression);
    INST_FIELD(right.as.pattern.arm); INST_FIELD(right.as.pattern.arm_ordinal);
    INST_FIELD(right.as.pattern.pattern); INST_FIELD(right.as.pattern.scrutinee);
    left.kind = SOL_MIR_INST_MATCH_ARM;
    INST_FIELD(right.as.pattern.match_expression);
    INST_FIELD(right.as.pattern.arm); INST_FIELD(right.as.pattern.arm_ordinal);
    INST_FIELD(right.as.pattern.pattern); INST_FIELD(right.as.pattern.scrutinee);
    left = (SolMirInstruction){0}; left.kind = SOL_MIR_INST_CONSTRUCT;
    INST_FIELD(right.as.construct.kind);
    INST_FIELD(right.as.construct.definition);
    INST_FIELD(right.as.construct.variant);
    INST_FIELD(right.as.construct.operands.offset);
    INST_FIELD(right.as.construct.operands.count);
    INST_FIELD(right.as.construct.capability_roots.offset);
    INST_FIELD(right.as.construct.capability_roots.count);
    INST_FIELD(right.as.construct.operation_roots.offset);
    INST_FIELD(right.as.construct.operation_roots.count);
    left = (SolMirInstruction){0}; left.kind = SOL_MIR_INST_FUNCTION_VALUE;
    INST_FIELD(right.as.function_callable);
    left = (SolMirInstruction){0}; left.kind = SOL_MIR_INST_CAPTURE_SNAPSHOT;
    INST_FIELD(right.as.snapshot);
#undef INST_FIELD
#undef INST_DIFF
}

static void test_terminator_discriminants_and_fields(void) {
    SolMirTerminator left = {0}, right;
#define TERM_DIFF(change) do { \
    right = left; \
    change; \
    CHECK(!sol_mir_program_test_terminator_equal(&left, &right)); \
} while (0)
#define TERM_FIELD(field) TERM_DIFF(flip_scalar_byte(&(field), sizeof(field)))
    left.kind = SOL_MIR_TERM_GOTO;
    left.span = (SolSpan){2, 4};
    left.as.go_to = (SolMirEdge){6, {8, 10}};
    CHECK(sol_mir_program_test_terminator_equal(&left, &left));
    TERM_DIFF(right.kind = SOL_MIR_TERM_BRANCH);
    TERM_FIELD(right.span.start); TERM_FIELD(right.span.end);
    TERM_FIELD(right.as.go_to.block);
    TERM_FIELD(right.as.go_to.arguments.offset);
    TERM_FIELD(right.as.go_to.arguments.count);

    for (int kind = SOL_MIR_TERM_GOTO;
        kind <= SOL_MIR_TERM_CONTRACT_VIOLATION; ++kind) {
        left = (SolMirTerminator){0};
        left.kind = (SolMirTerminatorKind)kind;
        CHECK(sol_mir_program_test_terminator_equal(&left, &left));
    }
    left.kind = SOL_MIR_TERM_INVALID;
    CHECK(!sol_mir_program_test_terminator_equal(&left, &left));
    left.kind = (SolMirTerminatorKind)-1;
    right = left;
    CHECK(!sol_mir_program_test_terminator_equal(&left, &right));
    left.kind = SOL_MIR_TERM_GOTO;

    left = (SolMirTerminator){0}; left.kind = SOL_MIR_TERM_BRANCH;
    TERM_FIELD(right.as.branch.condition);
    TERM_FIELD(right.as.branch.true_edge.block);
    TERM_FIELD(right.as.branch.true_edge.arguments.offset);
    TERM_FIELD(right.as.branch.true_edge.arguments.count);
    TERM_FIELD(right.as.branch.false_edge.block);
    TERM_FIELD(right.as.branch.false_edge.arguments.offset);
    TERM_FIELD(right.as.branch.false_edge.arguments.count);
    left = (SolMirTerminator){0}; left.kind = SOL_MIR_TERM_RETURN;
    TERM_FIELD(right.as.value);
    left.kind = SOL_MIR_TERM_PANIC; TERM_FIELD(right.as.value);
    left = (SolMirTerminator){0}; left.kind = SOL_MIR_TERM_INVOKE;
    TERM_FIELD(right.as.invoke.source_expression);
    TERM_FIELD(right.as.invoke.kind); TERM_FIELD(right.as.invoke.callable);
    TERM_FIELD(right.as.invoke.type_arguments.offset);
    TERM_FIELD(right.as.invoke.type_arguments.count);
    TERM_FIELD(right.as.invoke.effects.offset);
    TERM_FIELD(right.as.invoke.effects.count);
    TERM_FIELD(right.as.invoke.effect_parameter);
    TERM_FIELD(right.as.invoke.evidence.offset);
    TERM_FIELD(right.as.invoke.evidence.count);
    TERM_FIELD(right.as.invoke.callee);
    TERM_FIELD(right.as.invoke.receiver.formal);
    TERM_FIELD(right.as.invoke.receiver.access);
    TERM_FIELD(right.as.invoke.receiver.source_expression);
    TERM_FIELD(right.as.invoke.receiver.temporary);
    TERM_FIELD(right.as.invoke.receiver.place);
    TERM_FIELD(right.as.invoke.arguments.offset);
    TERM_FIELD(right.as.invoke.arguments.count);
    TERM_FIELD(right.as.invoke.result);
    TERM_FIELD(right.as.invoke.normal_edge.block);
    TERM_FIELD(right.as.invoke.normal_edge.arguments.offset);
    TERM_FIELD(right.as.invoke.normal_edge.arguments.count);
    TERM_FIELD(right.as.invoke.failure_edge.block);
    TERM_FIELD(right.as.invoke.failure_edge.arguments.offset);
    TERM_FIELD(right.as.invoke.failure_edge.arguments.count);
    left = (SolMirTerminator){0}; left.kind = SOL_MIR_TERM_UNREACHABLE;
    TERM_FIELD(right.as.unreachable.statement);
    TERM_FIELD(right.as.unreachable.obligation);
    left = (SolMirTerminator){0}; left.kind = SOL_MIR_TERM_BREAK;
    TERM_FIELD(right.as.transfer.statement); TERM_FIELD(right.as.transfer.loop);
    TERM_FIELD(right.as.transfer.edge.block);
    TERM_FIELD(right.as.transfer.edge.arguments.offset);
    TERM_FIELD(right.as.transfer.edge.arguments.count);
    left.kind = SOL_MIR_TERM_CONTINUE;
    TERM_FIELD(right.as.transfer.statement); TERM_FIELD(right.as.transfer.loop);
    TERM_FIELD(right.as.transfer.edge.block);
    TERM_FIELD(right.as.transfer.edge.arguments.offset);
    TERM_FIELD(right.as.transfer.edge.arguments.count);
    left = (SolMirTerminator){0}; left.kind = SOL_MIR_TERM_CHECK_REFINED;
    TERM_FIELD(right.as.check_refined.source_expression);
    TERM_FIELD(right.as.check_refined.definition);
    TERM_FIELD(right.as.check_refined.obligation);
    TERM_FIELD(right.as.check_refined.representation);
    TERM_FIELD(right.as.check_refined.result);
    TERM_FIELD(right.as.check_refined.normal_edge.block);
    TERM_FIELD(right.as.check_refined.normal_edge.arguments.offset);
    TERM_FIELD(right.as.check_refined.normal_edge.arguments.count);
    TERM_FIELD(right.as.check_refined.failure_edge.block);
    TERM_FIELD(right.as.check_refined.failure_edge.arguments.offset);
    TERM_FIELD(right.as.check_refined.failure_edge.arguments.count);
    left = (SolMirTerminator){0}; left.kind = SOL_MIR_TERM_MATCH_FAILURE;
    TERM_FIELD(right.as.match_failure);
    left = (SolMirTerminator){0}; left.kind = SOL_MIR_TERM_PROPAGATE;
    TERM_FIELD(right.as.propagate.source_expression);
    TERM_FIELD(right.as.propagate.kind); TERM_FIELD(right.as.propagate.operand);
    TERM_FIELD(right.as.propagate.value_result);
    TERM_FIELD(right.as.propagate.residual_result);
    TERM_FIELD(right.as.propagate.value_edge.block);
    TERM_FIELD(right.as.propagate.value_edge.arguments.offset);
    TERM_FIELD(right.as.propagate.value_edge.arguments.count);
    TERM_FIELD(right.as.propagate.residual_edge.block);
    TERM_FIELD(right.as.propagate.residual_edge.arguments.offset);
    TERM_FIELD(right.as.propagate.residual_edge.arguments.count);
    left = (SolMirTerminator){0}; left.kind = SOL_MIR_TERM_CHECK_CONTRACT;
    TERM_FIELD(right.as.check_contract.obligation);
    TERM_FIELD(right.as.check_contract.phase);
    TERM_FIELD(right.as.check_contract.outcome);
    TERM_FIELD(right.as.check_contract.result);
    TERM_FIELD(right.as.check_contract.satisfied_edge.block);
    TERM_FIELD(right.as.check_contract.satisfied_edge.arguments.offset);
    TERM_FIELD(right.as.check_contract.satisfied_edge.arguments.count);
    TERM_FIELD(right.as.check_contract.violation_edge.block);
    TERM_FIELD(right.as.check_contract.violation_edge.arguments.offset);
    TERM_FIELD(right.as.check_contract.violation_edge.arguments.count);
    TERM_FIELD(right.as.check_contract.failure_edge.block);
    TERM_FIELD(right.as.check_contract.failure_edge.arguments.offset);
    TERM_FIELD(right.as.check_contract.failure_edge.arguments.count);
    left = (SolMirTerminator){0}; left.kind = SOL_MIR_TERM_CONTRACT_VIOLATION;
    TERM_FIELD(right.as.contract_violation);
#undef TERM_FIELD
#undef TERM_DIFF
}

static void test_program_comparator_fields(void) {
    SolIr ir;
    sol_ir_init(&ir);
    SolMir mir;
    sol_mir_init(&mir);
    SolMirProgramRoot roots_left[] = {{3, SOL_MIR_PROGRAM_ROOT_ENTRY}};
    SolMirProgramRoot roots_right[] = {{3, SOL_MIR_PROGRAM_ROOT_ENTRY}};
    SolIrCallableId approvals_left[] = {5}, approvals_right[] = {5};
    SolMirProgramImport imports_left[] = {{7, {11, 13, 17, 19, 23}, 29}};
    SolMirProgramImport imports_right[] = {{7, {11, 13, 17, 19, 23}, 29}};
    SolMirProgramSpecialization specs_left[] = {
        {31, 37, 41, 43, 47, {53, 59, 61, 67, 71}, 73},
    };
    SolMirProgramSpecialization specs_right[] = {
        {31, 37, 41, 43, 47, {53, 59, 61, 67, 71}, 73},
    };
    SolMirProgramReference refs_left[] = {
        {SOL_MIR_PROGRAM_REFERENCE_INVOKE, {79, 83, 89, 97, 101}, 103},
    };
    SolMirProgramReference refs_right[] = {
        {SOL_MIR_PROGRAM_REFERENCE_INVOKE, {79, 83, 89, 97, 101}, 103},
    };
    SolMirProgramTemplate templates_left[] = {{107, {0}}};
    SolMirProgramTemplate templates_right[] = {{107, {0}}};
    templates_left[0].mir = mir;
    templates_right[0].mir = mir;
    SolMirProgram left = {
        .ir = &ir, .roots = roots_left, .root_count = 1,
        .approved_imports = approvals_left, .approved_import_count = 1,
        .templates = templates_left, .template_count = 1,
        .imports = imports_left, .import_count = 1,
        .specializations = specs_left, .specialization_count = 1,
        .references = refs_left, .reference_count = 1,
        .limits = {109, 113, 127}, .usage = {131, 137, 139},
    };
    SolMirProgram right = left;
    right.roots = roots_right;
    right.approved_imports = approvals_right;
    right.templates = templates_right;
    right.imports = imports_right;
    right.specializations = specs_right;
    right.references = refs_right;
    CHECK(sol_mir_program_test_program_equal(&left, &right));
    CHECK(program_fields_equal(&left, &right));
#define PROGRAM_CHANGE(change) do { \
    change; \
    CHECK(!sol_mir_program_test_program_equal(&left, &right)); \
    CHECK(!program_fields_equal(&left, &right)); \
} while (0)
#define PROGRAM_FIELD(left_field, right_field) do { \
    flip_scalar_byte(&(right_field), sizeof(right_field)); \
    PROGRAM_CHANGE((void)0); \
    (right_field) = (left_field); \
} while (0)
    right.ir = NULL; PROGRAM_CHANGE((void)0); right.ir = left.ir;
    PROGRAM_FIELD(left.root_count, right.root_count);
    PROGRAM_FIELD(left.approved_import_count, right.approved_import_count);
    PROGRAM_FIELD(left.template_count, right.template_count);
    PROGRAM_FIELD(left.import_count, right.import_count);
    PROGRAM_FIELD(left.specialization_count, right.specialization_count);
    PROGRAM_FIELD(left.reference_count, right.reference_count);
    PROGRAM_FIELD(left.limits.max_callable_classifications,
        right.limits.max_callable_classifications);
    PROGRAM_FIELD(left.limits.max_references, right.limits.max_references);
    PROGRAM_FIELD(left.limits.max_discovery_work, right.limits.max_discovery_work);
    PROGRAM_FIELD(left.usage.callable_classifications,
        right.usage.callable_classifications);
    PROGRAM_FIELD(left.usage.references, right.usage.references);
    PROGRAM_FIELD(left.usage.discovery_work, right.usage.discovery_work);
    PROGRAM_FIELD(roots_left[0].callable, roots_right[0].callable);
    PROGRAM_FIELD(roots_left[0].kind, roots_right[0].kind);
    PROGRAM_FIELD(approvals_left[0], approvals_right[0]);
    PROGRAM_FIELD(imports_left[0].callable, imports_right[0].callable);
    PROGRAM_FIELD(imports_left[0].first_source.callable,
        imports_right[0].first_source.callable);
    PROGRAM_FIELD(imports_left[0].first_source.expression,
        imports_right[0].first_source.expression);
    PROGRAM_FIELD(imports_left[0].first_source.file,
        imports_right[0].first_source.file);
    PROGRAM_FIELD(imports_left[0].first_source.start,
        imports_right[0].first_source.start);
    PROGRAM_FIELD(imports_left[0].first_source.end,
        imports_right[0].first_source.end);
    PROGRAM_FIELD(imports_left[0].source_count, imports_right[0].source_count);
    PROGRAM_FIELD(specs_left[0].trait, specs_right[0].trait);
    PROGRAM_FIELD(specs_left[0].requirement, specs_right[0].requirement);
    PROGRAM_FIELD(specs_left[0].type, specs_right[0].type);
    PROGRAM_FIELD(specs_left[0].implementation, specs_right[0].implementation);
    PROGRAM_FIELD(specs_left[0].method, specs_right[0].method);
    PROGRAM_FIELD(specs_left[0].first_source.callable,
        specs_right[0].first_source.callable);
    PROGRAM_FIELD(specs_left[0].first_source.expression,
        specs_right[0].first_source.expression);
    PROGRAM_FIELD(specs_left[0].first_source.file,
        specs_right[0].first_source.file);
    PROGRAM_FIELD(specs_left[0].first_source.start,
        specs_right[0].first_source.start);
    PROGRAM_FIELD(specs_left[0].first_source.end,
        specs_right[0].first_source.end);
    PROGRAM_FIELD(specs_left[0].source_count, specs_right[0].source_count);
    PROGRAM_FIELD(refs_left[0].kind, refs_right[0].kind);
    PROGRAM_FIELD(refs_left[0].source.callable, refs_right[0].source.callable);
    PROGRAM_FIELD(refs_left[0].source.expression, refs_right[0].source.expression);
    PROGRAM_FIELD(refs_left[0].source.file, refs_right[0].source.file);
    PROGRAM_FIELD(refs_left[0].source.start, refs_right[0].source.start);
    PROGRAM_FIELD(refs_left[0].source.end, refs_right[0].source.end);
    PROGRAM_FIELD(refs_left[0].target, refs_right[0].target);
    PROGRAM_FIELD(templates_left[0].callable, templates_right[0].callable);
    PROGRAM_FIELD(templates_left[0].mir.entry, templates_right[0].mir.entry);
#undef PROGRAM_FIELD
#undef PROGRAM_CHANGE
    sol_mir_free(&mir);
    sol_ir_free(&ir);
}

static void check_mir_header_and_arena_fields(const SolMir *left, SolMir *right) {
    CHECK(sol_mir_program_test_mir_equal(left, right));
#define MIR_FIELD(field) do { \
    flip_scalar_byte(&(field), sizeof(field)); \
    CHECK(!sol_mir_program_test_mir_equal(left, right)); \
    flip_scalar_byte(&(field), sizeof(field)); \
    CHECK(sol_mir_program_test_mir_equal(left, right)); \
} while (0)
    MIR_FIELD(right->callable);
    MIR_FIELD(right->generic_parameters.offset);
    MIR_FIELD(right->generic_parameters.count);
    MIR_FIELD(right->effect_parameters.offset);
    MIR_FIELD(right->effect_parameters.count);
    MIR_FIELD(right->entry);
    MIR_FIELD(right->contract_body);
    MIR_FIELD(right->contract_epilogue);
    MIR_FIELD(right->block_count);
    MIR_FIELD(right->instruction_count);
    MIR_FIELD(right->value_count);
    MIR_FIELD(right->parameter_value_count);
    MIR_FIELD(right->edge_value_count);
    MIR_FIELD(right->call_argument_count);
    MIR_FIELD(right->loop_count);
    MIR_FIELD(right->construct_operand_count);
    MIR_FIELD(right->temporary_count);
    for (size_t index = 0; index < left->block_count; ++index) {
        MIR_FIELD(right->blocks[index].id);
        MIR_FIELD(right->blocks[index].order);
        MIR_FIELD(right->blocks[index].parameters.offset);
        MIR_FIELD(right->blocks[index].parameters.count);
        MIR_FIELD(right->blocks[index].instructions.offset);
        MIR_FIELD(right->blocks[index].instructions.count);
        MIR_FIELD(right->blocks[index].terminator.kind);
        MIR_FIELD(right->blocks[index].span.start);
        MIR_FIELD(right->blocks[index].span.end);
        MIR_FIELD(right->blocks[index].started);
    }
    for (size_t index = 0; index < left->instruction_count; ++index) {
        MIR_FIELD(right->instructions[index].kind);
        MIR_FIELD(right->instructions[index].block);
        MIR_FIELD(right->instructions[index].result);
        MIR_FIELD(right->instructions[index].type);
        MIR_FIELD(right->instructions[index].source_expression);
        MIR_FIELD(right->instructions[index].span.start);
        MIR_FIELD(right->instructions[index].span.end);
    }
    for (size_t index = 0; index < left->value_count; ++index) {
        MIR_FIELD(right->values[index].kind);
        MIR_FIELD(right->values[index].type);
        MIR_FIELD(right->values[index].block);
        MIR_FIELD(right->values[index].definition);
        MIR_FIELD(right->values[index].source_expression);
        MIR_FIELD(right->values[index].span.start);
        MIR_FIELD(right->values[index].span.end);
    }
    for (size_t index = 0; index < left->parameter_value_count; ++index) {
        MIR_FIELD(right->parameter_values[index]);
    }
    for (size_t index = 0; index < left->edge_value_count; ++index) {
        MIR_FIELD(right->edge_values[index]);
    }
    for (size_t index = 0; index < left->call_argument_count; ++index) {
        MIR_FIELD(right->call_arguments[index].formal);
        MIR_FIELD(right->call_arguments[index].access);
        MIR_FIELD(right->call_arguments[index].source_expression);
        MIR_FIELD(right->call_arguments[index].temporary);
        MIR_FIELD(right->call_arguments[index].place);
    }
    for (size_t index = 0; index < left->loop_count; ++index) {
        MIR_FIELD(right->loops[index].statement);
        MIR_FIELD(right->loops[index].parent);
        MIR_FIELD(right->loops[index].preheader);
        MIR_FIELD(right->loops[index].header);
        MIR_FIELD(right->loops[index].condition);
        MIR_FIELD(right->loops[index].body);
        MIR_FIELD(right->loops[index].backedge);
        MIR_FIELD(right->loops[index].exit);
        MIR_FIELD(right->loops[index].obligations.offset);
        MIR_FIELD(right->loops[index].obligations.count);
        MIR_FIELD(right->loops[index].span.start);
        MIR_FIELD(right->loops[index].span.end);
    }
    for (size_t index = 0; index < left->construct_operand_count; ++index) {
        MIR_FIELD(right->construct_operands[index].formal);
        MIR_FIELD(right->construct_operands[index].source_expression);
        MIR_FIELD(right->construct_operands[index].temporary);
    }
    for (size_t index = 0; index < left->temporary_count; ++index) {
        MIR_FIELD(right->temporaries[index].type);
        MIR_FIELD(right->temporaries[index].source_expression);
        MIR_FIELD(right->temporaries[index].span.start);
        MIR_FIELD(right->temporaries[index].span.end);
    }
#undef MIR_FIELD
}

static void test_canonical_active_fields(void) {
    test_instruction_discriminants_and_fields();
    test_terminator_discriminants_and_fields();
    test_program_comparator_fields();

    Compilation compilation;
    SolPackage package;
    CHECK(compile_e6(&compilation, &package));
    size_t tested_callables = 0;
    for (SolIrCallableId id = 0; id < compilation.ir.callable_count; ++id) {
        if (compilation.ir.callables[id].body == SOL_IR_NONE) continue;
        SolMir left, right;
        sol_mir_init(&left);
        sol_mir_init(&right);
        SolDiagnostics diagnostics;
        sol_diagnostics_init(&diagnostics);
        SolMirLowerOutcome first = sol_mir_lower_callable(&compilation.ir, id,
            &left, &diagnostics);
        sol_diagnostics_free(&diagnostics);
        if (first == SOL_MIR_LOWER_SUCCEEDED) {
            sol_diagnostics_init(&diagnostics);
            SolMirLowerOutcome second = sol_mir_lower_callable(&compilation.ir,
                id, &right, &diagnostics);
            sol_diagnostics_free(&diagnostics);
            CHECK(second == SOL_MIR_LOWER_SUCCEEDED);
            if (second == SOL_MIR_LOWER_SUCCEEDED
                && copy_mir_arena_bytes(&right, &left)) {
                CHECK(left.instructions != right.instructions
                    && left.blocks != right.blocks);
                check_mir_header_and_arena_fields(&left, &right);
                ++tested_callables;
            } else if (second == SOL_MIR_LOWER_SUCCEEDED) {
                CHECK(false);
            }
        }
        sol_mir_free(&right);
        sol_mir_free(&left);
    }
    CHECK(tested_callables != 0);

    Compilation loop_compilation;
    const char *loop_source =
        "module mir_program\n"
        "function loop_fixture() -> Int64 effects { pure } {\n"
        "    var current = 0\n"
        "    while current < 2 decreases { 2 - current } {\n"
        "        current += 1\n"
        "    }\n"
        "    return current\n"
        "}\n";
    bool loop_compiled = compile_text(&loop_compilation, loop_source);
    CHECK(loop_compiled);
    if (loop_compiled) {
        SolIrCallableId loop_callable = callable(&loop_compilation.ir,
            "loop_fixture", SOL_IR_CALLABLE_FUNCTION);
        CHECK(loop_callable != SOL_IR_NONE);
        SolMir loop_left, loop_right;
        sol_mir_init(&loop_left);
        sol_mir_init(&loop_right);
        SolDiagnostics loop_diagnostics;
        sol_diagnostics_init(&loop_diagnostics);
        SolMirLowerOutcome loop_outcome = loop_callable == SOL_IR_NONE
            ? SOL_MIR_LOWER_FAILED
            : sol_mir_lower_callable(&loop_compilation.ir, loop_callable,
                &loop_left, &loop_diagnostics);
        sol_diagnostics_free(&loop_diagnostics);
        CHECK(loop_outcome == SOL_MIR_LOWER_SUCCEEDED);
        if (loop_outcome == SOL_MIR_LOWER_SUCCEEDED) {
            CHECK(sol_mir_validate(&loop_compilation.ir, &loop_left, NULL));
            CHECK(loop_left.loop_count != 0);
            if (loop_left.loop_count != 0) {
                const SolMirLoop *witness = &loop_left.loops[0];
                fprintf(stderr,
                    "canonical active-fields real-loop witness "
                    "callable=%zu rows=%zu obligations=(%zu,%zu) "
                    "span=(%zu,%zu)\n",
                    (size_t)loop_callable, loop_left.loop_count,
                    witness->obligations.offset, witness->obligations.count,
                    witness->span.start, witness->span.end);
                sol_diagnostics_init(&loop_diagnostics);
                SolMirLowerOutcome loop_copy_outcome = sol_mir_lower_callable(
                    &loop_compilation.ir, loop_callable, &loop_right,
                    &loop_diagnostics);
                sol_diagnostics_free(&loop_diagnostics);
                CHECK(loop_copy_outcome == SOL_MIR_LOWER_SUCCEEDED);
                bool loop_copy_valid = loop_copy_outcome
                        == SOL_MIR_LOWER_SUCCEEDED
                    && copy_mir_arena_bytes(&loop_right, &loop_left);
                CHECK(loop_copy_outcome != SOL_MIR_LOWER_SUCCEEDED
                    || loop_copy_valid);
                if (loop_copy_valid
                    && loop_right.loop_count == loop_left.loop_count) {
                    check_mir_header_and_arena_fields(&loop_left, &loop_right);
                    fprintf(stderr,
                        "canonical active-fields real-loop field mutations "
                        "completed rows=%zu fields=obligations-slice+span\n",
                        loop_left.loop_count);
                }
            }
        }
        sol_mir_free(&loop_right);
        sol_mir_free(&loop_left);
    }
    free_compilation(&loop_compilation);
    free_e6(&compilation, &package);
}

static void test_e6_closures_and_determinism(void) {
    Compilation compilation;
    SolPackage package;
    CHECK(compile_e6(&compilation, &package));
    const SolIr *ir = &compilation.ir;
    SolIrCallableId launch = callable(ir, "launch", SOL_IR_CALLABLE_FUNCTION);
    const char *import_names[] = {"write", "get", "count", "read"};
    SolIrCallableId approvals[4];
    for (size_t index = 0; index < 4; ++index) {
        approvals[index] = callable(ir, import_names[index],
            SOL_IR_CALLABLE_CAPABILITY);
        CHECK(approvals[index] != SOL_IR_NONE);
    }
    SolMirProgramRoot roots[5];
    roots[0] = (SolMirProgramRoot){launch, SOL_MIR_PROGRAM_ROOT_ENTRY};
    size_t root_count = 1;
    for (SolIrCallableId id = 0; id < ir->callable_count; ++id) {
        if (ir->callables[id].kind == SOL_IR_CALLABLE_TEST) {
            roots[root_count++] = (SolMirProgramRoot){id,
                SOL_MIR_PROGRAM_ROOT_TEST};
        }
    }
    CHECK(root_count == 5);

    SolMirProgram entry;
    sol_mir_program_init(&entry);
    CHECK(build(ir, roots, 1, approvals, 4, NULL, &entry)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(entry.usage.discovery_work == 1166
        && capability_validation_work(&entry) == 345);
    CHECK(entry.template_count == 8);
    CHECK(entry.import_count == 4);
    CHECK(entry.specialization_count == 1);
    size_t invokes = 0;
    size_t static_function_invokes = 0;
    size_t bound_operation_invokes = 0;
    size_t authenticated_bound_references = 0;
    for (size_t index = 0; index < entry.reference_count; ++index) {
        const SolMirProgramReference *reference = &entry.references[index];
        if (reference->kind != SOL_MIR_PROGRAM_REFERENCE_INVOKE) continue;
        ++invokes;
        const SolIrExpression *call
            = &ir->expressions[reference->source.expression];
        CHECK(call->kind == SOL_IR_EXPR_CALL);
        if (call->as.call.callee < ir->expression_count) {
            SolIrExpressionKind callee_kind
                = ir->expressions[call->as.call.callee].kind;
            static_function_invokes += callee_kind == SOL_IR_EXPR_DEFINITION;
            bound_operation_invokes += callee_kind
                == SOL_IR_EXPR_BOUND_OPERATION;
            if (call->as.call.kind == SOL_IR_CALL_CAPABILITY
                && callee_kind == SOL_IR_EXPR_BOUND_OPERATION) {
                size_t matches = 0;
                for (size_t other = 0; other < entry.reference_count; ++other) {
                    const SolMirProgramReference *bound = &entry.references[other];
                    matches += bound->kind == SOL_MIR_PROGRAM_REFERENCE_BOUND_OPERATION
                        && bound->source.callable == reference->source.callable
                        && bound->source.expression == call->as.call.callee
                        && bound->target == call->as.call.callable;
                }
                CHECK(matches == 1); authenticated_bound_references += matches;
            }
        }
    }
    CHECK(invokes == 12);
    CHECK(static_function_invokes != 0);
    CHECK(bound_operation_invokes == 5);
    CHECK(authenticated_bound_references == 5 && entry.reference_count == 17);
    CHECK(entry.specializations[0].source_count != 0);
    CHECK(has_template(&entry, launch));
    for (size_t index = 0; index < 4; ++index) {
        CHECK(has_import(&entry, approvals[index]));
    }
    SolDiagnostics validation;
    sol_diagnostics_init(&validation);
    CHECK(sol_mir_program_validate(&entry, &validation));
    size_t bound_reference = SIZE_MAX;
    for (size_t i = 0; i < entry.reference_count; ++i)
        if (entry.references[i].kind == SOL_MIR_PROGRAM_REFERENCE_BOUND_OPERATION) {
            bound_reference = i; break;
        }
    CHECK(bound_reference != SIZE_MAX);
    if (bound_reference != SIZE_MAX) {
        SolMirProgramReference saved = entry.references[bound_reference];
        entry.references[bound_reference].target = SOL_IR_NONE;
        CHECK(!sol_mir_program_validate(&entry, NULL));
        entry.references[bound_reference] = saved;
        entry.references[bound_reference].source.expression = SOL_IR_NONE;
        CHECK(!sol_mir_program_validate(&entry, NULL));
        entry.references[bound_reference] = saved;
        entry.references[bound_reference].source.file = SIZE_MAX;
        CHECK(!sol_mir_program_validate(&entry, NULL));
        entry.references[bound_reference] = saved;
        ++entry.references[bound_reference].source.start;
        CHECK(!sol_mir_program_validate(&entry, NULL));
        entry.references[bound_reference] = saved;
        SolMirProgramReference displaced = entry.references[0];
        entry.references[0] = saved;
        CHECK(!sol_mir_program_validate(&entry, NULL));
        entry.references[0] = displaced;
        CHECK(sol_mir_program_validate(&entry, NULL));
    }
    SolMirProgramRootKind root_kind = entry.roots[0].kind;
    entry.roots[0].kind = SOL_MIR_PROGRAM_ROOT_TEST;
    CHECK(!sol_mir_program_validate(&entry, &validation));
    entry.roots[0].kind = root_kind;
    SolIrCallableId approved_first = entry.approved_imports[0];
    entry.approved_imports[0] = entry.approved_imports[1];
    CHECK(!sol_mir_program_validate(&entry, &validation));
    entry.approved_imports[0] = approved_first;
    SolIrCallableId imported = entry.imports[0].callable;
    entry.imports[0].callable = SOL_IR_NONE;
    CHECK(!sol_mir_program_validate(&entry, &validation));
    entry.imports[0].callable = imported;
    SolIrCallableId method = entry.specializations[0].method;
    entry.specializations[0].method = SOL_IR_NONE;
    CHECK(!sol_mir_program_validate(&entry, &validation));
    entry.specializations[0].method = method;
    SolIrExpressionId expression = entry.references[0].source.expression;
    entry.references[0].source.expression = SOL_IR_NONE;
    CHECK(!sol_mir_program_validate(&entry, &validation));
    entry.references[0].source.expression = expression;
    size_t source_file = entry.references[0].source.file;
    size_t diagnostic_count = validation.count;
    entry.references[0].source.file = SIZE_MAX;
    CHECK(!sol_mir_program_validate(&entry, &validation));
    CHECK(validation.allocation_failed || validation.count > diagnostic_count);
    entry.references[0].source.file = source_file;
    SolMirProgramReference first_reference = entry.references[0];
    entry.references[0] = entry.references[1];
    entry.references[1] = first_reference;
    CHECK(!sol_mir_program_validate(&entry, &validation));
    entry.references[1] = entry.references[0];
    entry.references[0] = first_reference;
    CHECK(sol_mir_program_validate(&entry, &validation));
    CHECK(sol_mir_program_validate(&entry, NULL));
    sol_diagnostics_free(&validation);

    SolMirProgram all;
    SolMirProgram permuted;
    sol_mir_program_init(&all);
    sol_mir_program_init(&permuted);
    CHECK(build(ir, roots, root_count, approvals, 4, NULL, &all)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    SolMirProgramRoot reverse_roots[5];
    SolIrCallableId reverse_approvals[4];
    for (size_t index = 0; index < root_count; ++index) {
        reverse_roots[index] = roots[root_count - index - 1];
    }
    for (size_t index = 0; index < 4; ++index) {
        reverse_approvals[index] = approvals[3 - index];
    }
    CHECK(build(ir, reverse_roots, root_count, reverse_approvals, 4,
        NULL, &permuted) == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(all.template_count == 14);
    CHECK(all.import_count == 4);
    CHECK(all.specialization_count == 1);
    invokes = 0;
    for (size_t index = 0; index < all.reference_count; ++index) {
        invokes += all.references[index].kind
            == SOL_MIR_PROGRAM_REFERENCE_INVOKE;
    }
    CHECK(invokes == 18);
    CHECK(all.reference_count == 23);
    CHECK(program_fields_equal(&all, &permuted));
    for (SolIrCallableId id = 0; id < ir->callable_count; ++id) {
        if (ir->callables[id].body != SOL_IR_NONE) CHECK(has_template(&all, id));
    }

    SolMirProgramLimits exact = all.usage.callable_classifications == 0
        ? sol_mir_program_default_limits()
        : (SolMirProgramLimits){all.usage.callable_classifications,
            all.usage.references, all.usage.discovery_work};
    SolMirProgram limited;
    sol_mir_program_init(&limited);
    CHECK(build(ir, roots, root_count, approvals, 4, &exact, &limited)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    sol_mir_program_free(&limited);
    SolMirProgramLimits less = exact;
    --less.max_callable_classifications;
    CHECK(build(ir, roots, root_count, approvals, 4, &less, &limited)
        == SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED);
    CHECK(limited.ir == NULL && limited.templates == NULL);
    less = exact;
    --less.max_references;
    CHECK(build(ir, roots, root_count, approvals, 4, &less, &limited)
        == SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED);
    less = exact;
    --less.max_discovery_work;
    CHECK(build(ir, roots, root_count, approvals, 4, &less, &limited)
        == SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED);

    sol_mir_program_free(&entry);
    sol_mir_program_free(&all);
    sol_mir_program_free(&permuted);
    sol_mir_program_free(&limited);
    sol_mir_program_free(&limited);
    free_e6(&compilation, &package);
}

static void test_recursion_references_and_unreachable(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module closure\n"
        "function absent() -> Int64 { return 99 }\n"
        "function leaf() -> Int64 { return 1 }\n"
        "function direct(value: Int64) -> Int64 { if value > 0 { "
        "return direct(value - 1) } else { return 0 } }\n"
        "function left(flag: Bool) -> Int64 { if flag { return right(false) } "
        "else { return leaf() + leaf() } }\n"
        "function right(flag: Bool) -> Int64 { if flag { return left(false) } "
        "else { return 0 } }\n"));
    SolIrCallableId left = callable(&compilation.ir, "left",
        SOL_IR_CALLABLE_FUNCTION);
    SolIrCallableId leaf = callable(&compilation.ir, "leaf",
        SOL_IR_CALLABLE_FUNCTION);
    SolIrCallableId absent = callable(&compilation.ir, "absent",
        SOL_IR_CALLABLE_FUNCTION);
    SolMirProgramRoot root = {left, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirProgram program;
    sol_mir_program_init(&program);
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(program.template_count == 3);
    CHECK(has_template(&program, leaf));
    CHECK(!has_template(&program, absent));
    size_t leaf_references = 0;
    for (size_t index = 0; index < program.reference_count; ++index) {
        leaf_references += program.references[index].target == leaf;
    }
    CHECK(leaf_references == 2);
    sol_mir_program_free(&program);
    root.callable = callable(&compilation.ir, "direct",
        SOL_IR_CALLABLE_FUNCTION);
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(program.template_count == 1);
    CHECK(program.reference_count == 1);
    sol_mir_program_free(&program);
    SolMirProgramLimits limits = sol_mir_program_default_limits();
    limits.max_callable_classifications = 1;
    limits.max_references = 1;
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, &limits, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    sol_mir_program_free(&program);
    root.callable = left;
    limits = sol_mir_program_default_limits();
    limits.max_callable_classifications = 2;
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, &limits, &program)
        == SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED);
    limits = sol_mir_program_default_limits();
    limits.max_references = 1;
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, &limits, &program)
        == SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED);
    limits = sol_mir_program_default_limits();
    limits.max_discovery_work = 1;
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, &limits, &program)
        == SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED);
    free_compilation(&compilation);
}

static void test_evidence_and_callbacks(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module evidence\n"
        "trait Value { function value(self: Self) -> Int64 effects { pure } }\n"
        "implementation Value for Int64 { function value(self: Self) -> Int64 "
        "effects { pure } { return self } }\n"
        "implementation Value for Bool { function value(self: Self) -> Int64 "
        "effects { pure } { if self { return 1 } else { return 0 } } }\n"
        "function generic<T: Value>(item: T) -> Int64 effects { pure } "
        "{ return item.value() }\n"
        "function unbounded<T>(item: T) -> T effects { pure } { return item }\n"
        "function concrete() -> Int64 effects { pure } { return generic(1) }\n"
        "function inner<U: Value>(item: U) -> Int64 effects { pure } "
        "{ return item.value() }\n"
        "function outer<T: Value>(item: T) -> Int64 effects { pure } "
        "{ return inner(item) }\n"
        "function int_nested() -> Int64 effects { pure } { return outer(1) }\n"
        "function bool_nested() -> Int64 effects { pure } { return outer(true) }\n"
        "function plain(value: Int64) -> Int64 effects { pure } { return value }\n"
        "function apply(callback: function(Int64) -> Int64 effects { pure }) "
        "-> Int64 effects { pure } { return callback(1) }\n"
        "function callback_root() -> Int64 effects { pure } { return apply(plain) }\n"));
    SolMirProgram program;
    sol_mir_program_init(&program);
    SolMirProgramRoot root = {callable(&compilation.ir, "concrete",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(program.specialization_count == 1);
    CHECK(program.template_count == 3);
    sol_mir_program_free(&program);

    SolIrCallableId generic = callable(&compilation.ir, "generic",
        SOL_IR_CALLABLE_FUNCTION);
    SolMirProgramRoot evidence_roots[] = {
        {generic, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE},
        {root.callable, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE},
    };
    SolMirProgram ordered;
    sol_mir_program_init(&ordered);
    CHECK(build(&compilation.ir, evidence_roots, 2, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    SolMirProgramRoot swapped[] = {evidence_roots[1], evidence_roots[0]};
    CHECK(build(&compilation.ir, swapped, 2, NULL, 0, NULL, &ordered)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    sol_mir_program_free(&program);
    sol_mir_program_free(&ordered);

    root.callable = callable(&compilation.ir, "unbounded",
        SOL_IR_CALLABLE_FUNCTION);
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(program.template_count == 1);
    CHECK(sol_mir_program_validate(&program, NULL));
    sol_mir_program_free(&program);

    SolMirProgramRoot nested_roots[] = {
        {callable(&compilation.ir, "int_nested", SOL_IR_CALLABLE_FUNCTION),
            SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE},
        {callable(&compilation.ir, "bool_nested", SOL_IR_CALLABLE_FUNCTION),
            SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE},
    };
    CHECK(build(&compilation.ir, nested_roots, 2, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(program.specialization_count == 2);
    CHECK(program.specializations[0].type != program.specializations[1].type);
    CHECK(program.specializations[0].method != program.specializations[1].method);
    CHECK(program.specializations[0].source_count >= 3);
    CHECK(program.specializations[1].source_count >= 3);
    CHECK(has_template(&program, callable(&compilation.ir, "outer",
        SOL_IR_CALLABLE_FUNCTION)));
    CHECK(has_template(&program, callable(&compilation.ir, "inner",
        SOL_IR_CALLABLE_FUNCTION)));
    CHECK(has_template(&program, program.specializations[0].method));
    CHECK(has_template(&program, program.specializations[1].method));
    CHECK(sol_mir_program_validate(&program, NULL));
    sol_mir_program_free(&program);

    root.callable = callable(&compilation.ir, "outer",
        SOL_IR_CALLABLE_FUNCTION);
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    SolMirProgramRoot unrelated_roots[] = {
        root,
        {callable(&compilation.ir, "concrete", SOL_IR_CALLABLE_FUNCTION),
            SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE},
    };
    CHECK(build(&compilation.ir, unrelated_roots, 2, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    SolMirProgramRoot leaking_roots[] = {root, nested_roots[0]};
    CHECK(build(&compilation.ir, leaking_roots, 2, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);

    root.callable = generic;
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    CHECK(program.ir == NULL);

    root.callable = callable(&compilation.ir, "callback_root",
        SOL_IR_CALLABLE_FUNCTION);
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    root.callable = callable(&compilation.ir, "apply", SOL_IR_CALLABLE_FUNCTION);
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    sol_mir_program_free(&program);
    free_compilation(&compilation);

    /* Method evidence is an immediate-call proof, never a callable value. */
    Compilation method_value;
    CHECK(!compile_text(&method_value,
        "module method_value\n"
        "trait Value { function value(self: borrow Self) -> Int64 effects { pure } }\n"
        "implementation Value for Int64 { function value(self: borrow Self) -> Int64 "
        "effects { pure } { return self } }\n"
        "function rejected() -> Int64 effects { pure } { let value = 1 "
        "let method = value.value return method() }\n"));
    free_compilation(&method_value);
}

static void test_callback_source_limitations(void) {
    Compilation compilation;
    CHECK(!compile_text(&compilation,
        "module conditional_callback\n"
        "function plain(value: Int64) -> Int64 effects { pure } { return value }\n"
        "function rejected(flag: Bool, callback: function(Int64) -> Int64 "
        "effects { pure }) -> Int64 effects { pure } { "
        "let selected = if flag { plain } else { callback } "
        "return selected(1) }\n"));
    free_compilation(&compilation);
    CHECK(compile_text(&compilation,
        "module mixed_callbacks\n"
        "function plain(value: Int64) -> Int64 effects { pure } { return value }\n"
        "function rejected(callback: function(Int64) -> Int64 effects { pure }) "
        "-> Int64 effects { pure } { let known = plain "
        "let first = known(1) return callback(first) }\n"));
    SolMirProgramRoot root = {callable(&compilation.ir, "rejected",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirProgram program; sol_mir_program_init(&program);
    CHECK(root.callable != SOL_IR_NONE && build(&compilation.ir, &root, 1,
        NULL, 0, NULL, &program) == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    sol_mir_program_free(&program);
    free_compilation(&compilation);
    CHECK(!compile_text(&compilation,
        "module callback_parameter_mismatch\n"
        "function plain(value: Int64) -> Int64 effects { pure } { return value }\n"
        "function bad() -> function(Text) -> Int64 effects { pure } effects { pure } { "
        "return plain }\n"));
    free_compilation(&compilation);
    CHECK(!compile_text(&compilation,
        "module callback_access_mismatch\n"
        "function plain(value: Int64) -> Int64 effects { pure } { return value }\n"
        "function bad() -> function(borrow Int64) -> Int64 effects { pure } effects { pure } { "
        "return plain }\n"));
    free_compilation(&compilation);
    CHECK(!compile_text(&compilation,
        "module callback_result_mismatch\n"
        "function plain(value: Int64) -> Int64 effects { pure } { return value }\n"
        "function bad() -> function(Int64) -> Bool effects { pure } effects { pure } { "
        "return plain }\n"));
    free_compilation(&compilation);
}

static void test_callback_producer_discovery_limits(void) {
    static const char source[] =
        "module callback_producer_limits\n"
        "record Wide { left: function() -> Int64 effects { pure }, a: Text, b: Text, "
        "c: Text, d: Text, e: Text }\n"
        "function answer() -> Int64 effects { pure } { return 42 }\n"
        "function root() -> Int64 effects { pure } { let f0 = answer let f1 = f0 "
        "let f2 = f1 let f3 = f2 let f4 = f3 let f5 = f4 let f6 = f5 let f7 = f6 "
        "let f8 = f7 let f9 = f8 let wide = Wide { left = f9, a = \"a\", b = \"b\", "
        "c = \"c\", d = \"d\", e = \"e\" } let moved = wide.left return moved() }\n";
    Compilation compilation; CHECK(compile_text(&compilation, source));
    SolMirProgramRoot root = {callable(&compilation.ir, "root",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirProgram program, exact, limited;
    sol_mir_program_init(&program); sol_mir_program_init(&exact); sol_mir_program_init(&limited);
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    if (program.ir != NULL) {
        SolMirProgramLimits limits = sol_mir_program_default_limits();
        limits.max_discovery_work = program.usage.discovery_work;
        CHECK(build(&compilation.ir, &root, 1, NULL, 0, &limits, &exact)
            == SOL_MIR_PROGRAM_BUILD_SUCCEEDED
            && exact.usage.discovery_work == limits.max_discovery_work);
        CHECK(limits.max_discovery_work != 0);
        --limits.max_discovery_work;
        CHECK(build(&compilation.ir, &root, 1, NULL, 0, &limits, &limited)
            == SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED && limited.ir == NULL);

        SolIrLocalId f0 = SOL_IR_NONE, f9 = SOL_IR_NONE, wide = SOL_IR_NONE;
        for (size_t i = 0; i < compilation.ir.local_count; ++i) {
            if (strcmp(compilation.ir.locals[i].name, "f0") == 0) f0 = i;
            if (strcmp(compilation.ir.locals[i].name, "f9") == 0) f9 = i;
            if (strcmp(compilation.ir.locals[i].name, "wide") == 0) wide = i;
        }
        size_t f0_statement = SOL_IR_NONE, f9_place = SOL_IR_NONE;
        for (size_t i = 0; i < compilation.ir.statement_count; ++i)
            if (compilation.ir.statements[i].kind == SOL_IR_STATEMENT_LET
                && compilation.ir.statements[i].local == f0) f0_statement = i;
        for (size_t i = 0; i < compilation.ir.expression_count; ++i) {
            const SolIrExpression *expression = &compilation.ir.expressions[i];
            if (expression->kind != SOL_IR_EXPR_PLACE
                || expression->as.place >= compilation.ir.place_count) continue;
            const SolIrPlace *place = &compilation.ir.places[expression->as.place];
            if (place->root_kind == SOL_IR_PLACE_ROOT_LOCAL && place->local == f9
                && place->projections.count == 0) f9_place = i;
        }
        CHECK(f0 != SOL_IR_NONE && f9 != SOL_IR_NONE && wide != SOL_IR_NONE
            && f0_statement != SOL_IR_NONE && f9_place != SOL_IR_NONE);
        if (f0_statement != SOL_IR_NONE && f9_place != SOL_IR_NONE) {
            SolMirProgram rejected; sol_mir_program_init(&rejected);
            SolIrExpressionId saved = compilation.ir.statements[f0_statement].expression;
            compilation.ir.statements[f0_statement].expression = f9_place;
            CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &rejected)
                != SOL_MIR_PROGRAM_BUILD_SUCCEEDED && rejected.ir == NULL);
            compilation.ir.statements[f0_statement].expression = saved;
            sol_mir_program_free(&rejected);
            sol_mir_program_init(&rejected);
            SolIrLocalId saved_local = compilation.ir.statements[f0_statement].local;
            compilation.ir.statements[f0_statement].local = wide;
            CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &rejected)
                != SOL_MIR_PROGRAM_BUILD_SUCCEEDED && rejected.ir == NULL);
            compilation.ir.statements[f0_statement].local = saved_local;
            sol_mir_program_free(&rejected);
        }
    }
    sol_mir_program_free(&limited); sol_mir_program_free(&exact); sol_mir_program_free(&program);
    free_compilation(&compilation);
}

static void test_import_policy_and_lifecycle(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module imports\n"
        "type Callback = distinct function() -> Int64 effects { pure }\n"
        "record Deep1 { value: Callback }\n"
        "record Deep2 { value: Deep1 }\n"
        "record Deep3 { value: Deep2 }\n"
        "record Deep4 { value: Deep3 }\n"
        "record Deep5 { value: Deep4 }\n"
        "record Deep6 { value: Deep5 }\n"
        "record Deep7 { value: Deep6 }\n"
        "record Deep8 { value: Deep7 }\n"
        "capability Host { "
        "function good(value: Text) -> Option<Text> effects { host.good<Self> } "
        "function exclusive(value: inout Int64) -> () effects { host.bad<Self> } "
        "function callback() -> Callback effects { host.callback<Self> } "
        "function deep_callback() -> Deep8 effects { host.deep<Self> } "
        "function extra() -> Int64 effects { pure } }\n"
        "capability Clock { function now() -> Int64 "
        "effects { clock.read<Self> } }\n"
        "capability Wrapped derives_from private_source: capability Clock { "
        "function now() -> Int64 effects { clock.read<Self> } "
        "{ return private_source.now() } }\n"
        "function use_wrapped(value: capability Wrapped) -> Int64 "
        "effects { clock.read<value> } { return value.now() }\n"
        "function consume(host: capability Host) -> Option<Text> "
        "effects { host.good<host> } { return host.good(\"x\") }\n"));
    const SolIr *ir = &compilation.ir;
    SolIrCallableId good = callable(ir, "good", SOL_IR_CALLABLE_CAPABILITY);
    SolMirProgramRoot root = {callable(ir, "consume", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirProgram program;
    sol_mir_program_init(&program);
    CHECK(build(ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    CHECK(program.ir == NULL);
    CHECK(build(ir, &root, 1, &good, 1, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(program.import_count == 1 && program.imports[0].callable == good);

    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    SolMirProgramBuildRequest request = {ir, &root, 1, &good, 1, NULL};
    CHECK(sol_mir_program_build(&request, &program, &diagnostics)
        == SOL_MIR_PROGRAM_BUILD_INVALID_ARGUMENT);
    sol_diagnostics_free(&diagnostics);
    sol_mir_program_free(&program);

    SolIrCallableId extra = callable(ir, "extra", SOL_IR_CALLABLE_CAPABILITY);
    SolIrCallableId extra_approvals[] = {extra, good, extra};
    CHECK(build(ir, &root, 1, extra_approvals, 3, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(program.approved_import_count == 2);
    CHECK(program.import_count == 1 && program.imports[0].callable == good);
    CHECK(sol_mir_program_validate(&program, NULL));
    SolMirProgramUsage duplicate_approval_usage = program.usage;
    sol_mir_program_free(&program);
    SolIrCallableId unique_extra_approvals[] = {good, extra};
    CHECK(build(ir, &root, 1, unique_extra_approvals, 2, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(memcmp(&program.usage, &duplicate_approval_usage,
        sizeof(program.usage)) == 0);
    CHECK(sol_mir_program_validate(&program, NULL));
    sol_mir_program_free(&program);
    SolIrCallableId permuted_duplicate_approvals[] = {good, extra, good};
    CHECK(build(ir, &root, 1, permuted_duplicate_approvals, 3, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(memcmp(&program.usage, &duplicate_approval_usage,
        sizeof(program.usage)) == 0);
    CHECK(sol_mir_program_validate(&program, NULL));
    sol_mir_program_free(&program);

    const char *invalid_names[] = {"exclusive", "callback", "deep_callback"};
    for (size_t index = 0; index < 3; ++index) {
        SolIrCallableId invalid = callable(ir, invalid_names[index],
            SOL_IR_CALLABLE_CAPABILITY);
        CHECK(build(ir, &root, 1, &invalid, 1, NULL, &program)
            == SOL_MIR_PROGRAM_BUILD_INVALID_ARGUMENT);
        CHECK(program.ir == NULL);
    }
    SolIrCallableId wrapped_now = SOL_IR_NONE;
    for (SolIrCallableId id = 0; id < ir->callable_count; ++id) {
        if (ir->callables[id].kind == SOL_IR_CALLABLE_CAPABILITY
            && ir->callables[id].body != SOL_IR_NONE
            && strcmp(ir->callables[id].name, "now") == 0) wrapped_now = id;
    }
    CHECK(wrapped_now != SOL_IR_NONE);
    CHECK(build(ir, &root, 1, &wrapped_now, 1, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_INVALID_ARGUMENT);
    SolMirProgramRoot wrapped_root = {callable(ir, "use_wrapped",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    CHECK(build(ir, &wrapped_root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    SolMirProgramLimits zero = {0};
    CHECK(build(ir, &root, 1, &good, 1, &zero, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    sol_mir_program_free(&program);
    SolMirProgramRoot duplicate_roots[] = {root, root};
    CHECK(build(ir, duplicate_roots, 2, &good, 1, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(program.root_count == 1);
    CHECK(sol_mir_program_validate(&program, NULL));
    SolMirProgramUsage duplicate_root_usage = program.usage;
    sol_mir_program_free(&program);
    CHECK(build(ir, &root, 1, &good, 1, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(memcmp(&program.usage, &duplicate_root_usage,
        sizeof(program.usage)) == 0);
    CHECK(sol_mir_program_validate(&program, NULL));
    sol_mir_program_free(&program);
    duplicate_roots[1].kind = SOL_MIR_PROGRAM_ROOT_TEST;
    CHECK(build(ir, duplicate_roots, 2, &good, 1, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_INVALID_ARGUMENT);
    SolMirProgramRoot bodyless = {good,
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    CHECK(build(ir, &bodyless, 1, &good, 1, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_INVALID_ARGUMENT);
    SolMirProgramLimits partial = {1, 0, 1};
    CHECK(build(ir, &root, 1, &good, 1, &partial, &program)
        == SOL_MIR_PROGRAM_BUILD_INVALID_ARGUMENT);
    CHECK(build(ir, &root, SIZE_MAX, &good, 1, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_INVALID_ARGUMENT);
    free_compilation(&compilation);
}

static void test_predicate_proof_and_handler_relations(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module provenance\n"
        "function predicate_dependency(value: Int64) -> Bool effects { pure } "
        "{ return value > 0 }\n"
        "function proof_dependency(value: Int64) -> Bool effects { pure } "
        "{ return value >= 0 }\n"
        "type Positive = refined Int64 where predicate_dependency(self)\n"
        "function dynamic_contract(callback: function(Int64) -> Bool "
        "effects { pure }) -> Int64 effects { pure } "
        "requires { callback(1) } { return 1 }\n"
        "function checked() -> Positive effects { pure } { "
        "while false invariant { proof_dependency(1) } decreases { 1 } {} "
        "return Positive(1) }\n"
        "capability Source { function read(value: Int64) -> Positive "
        "effects { service.read<Self> } }\n"
        "capability Provider { function read(value: Int64) -> Positive effects { pure } "
        "requires { Positive(value) == Positive(value) } "
        "ensures { result == Positive(old(value)) } { return Positive(value) } }\n"
        "function direct(provider: capability Provider) -> Positive "
        "{ return provider.read(1) }\n"
        "function handled(source: capability Source, provider: capability Provider) "
        "-> Positive { handle service.read<source> with provider { source.read(1) } }\n"));
    const SolIr *ir = &compilation.ir;
    SolIrCallableId checked = callable(ir, "checked", SOL_IR_CALLABLE_FUNCTION);
    SolIrCallableId predicate = callable(ir, "predicate_dependency",
        SOL_IR_CALLABLE_FUNCTION);
    SolIrCallableId proof = callable(ir, "proof_dependency",
        SOL_IR_CALLABLE_FUNCTION);
    SolMirProgramRoot root = {checked, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirProgram program;
    sol_mir_program_init(&program);
    CHECK(build(ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(has_template(&program, predicate));
    CHECK(!has_template(&program, proof));
    size_t predicate_references = 0;
    for (size_t index = 0; index < program.reference_count; ++index) {
        predicate_references += program.references[index].kind
            == SOL_MIR_PROGRAM_REFERENCE_PREDICATE_CALL
            && program.references[index].target == predicate;
        CHECK(program.references[index].target != proof);
    }
    CHECK(predicate_references == 1);
    sol_mir_program_free(&program);

    root.callable = callable(ir, "dynamic_contract", SOL_IR_CALLABLE_FUNCTION);
    CHECK(build(ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    CHECK(program.ir == NULL);

    SolIrCallableId source = callable(ir, "read", SOL_IR_CALLABLE_CAPABILITY);
    CHECK(ir->callables[source].body == SOL_IR_NONE);
    CHECK(ir->callables[source].receiver == SOL_IR_NONE);
    CHECK(ir->callables[source].receiver_access == SOL_ACCESS_OWNED);
    SolIrCallableId provider = SOL_IR_NONE;
    for (SolIrCallableId id = source + 1; id < ir->callable_count; ++id) {
        if (ir->callables[id].kind == SOL_IR_CALLABLE_CAPABILITY
            && strcmp(ir->callables[id].name, "read") == 0) {
            provider = id;
            break;
        }
    }
    root.callable = callable(ir, "direct", SOL_IR_CALLABLE_FUNCTION);
    CHECK(build(ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(has_template(&program, provider));
    CHECK(has_template(&program, predicate));
    CHECK(!has_import(&program, provider));
    sol_mir_program_free(&program);

    SolIrCallable *provider_metadata = &compilation.ir.callables[provider];
    SolIrLocalId saved_receiver = provider_metadata->receiver;
    provider_metadata->receiver = SOL_IR_NONE;
    CHECK(!sol_ir_validate(&compilation.ir, NULL));
    provider_metadata->receiver = saved_receiver;
    SolIrExpressionId saved_body = provider_metadata->body;
    provider_metadata->body = SOL_IR_NONE;
    CHECK(!sol_ir_validate(&compilation.ir, NULL));
    provider_metadata->body = saved_body;
    CHECK(sol_ir_validate(&compilation.ir, NULL));

    SolIrCallableId approvals[] = {source};
    root.callable = callable(ir, "handled", SOL_IR_CALLABLE_FUNCTION);
    CHECK(build(ir, &root, 1, approvals, 1, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(program.import_count == 1);
    CHECK(has_template(&program, provider));
    CHECK(has_template(&program, predicate));
    CHECK(!has_import(&program, provider));
    size_t handler_sources = 0;
    size_t handler_providers = 0;
    for (size_t index = 0; index < program.reference_count; ++index) {
        handler_sources += program.references[index].kind
            == SOL_MIR_PROGRAM_REFERENCE_HANDLER_SOURCE;
        handler_providers += program.references[index].kind
            == SOL_MIR_PROGRAM_REFERENCE_HANDLER_PROVIDER;
    }
    CHECK(handler_sources == 1);
    CHECK(handler_providers == 1);
    sol_mir_program_free(&program);
    free_compilation(&compilation);
}

static void test_derived_bodyful_capability_is_transactionally_unsupported(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module derived_bodyful\n"
        "capability Source { function read() -> Int64 effects { source.read<Self> } }\n"
        "capability Provider derives_from private_source: capability Source { "
        "function read() -> Int64 effects { pure } { return 1 } }\n"
        "function direct(provider: capability Provider) -> Int64 "
        "{ return provider.read() }\n"));
    SolMirProgram program;
    sol_mir_program_init(&program);
    SolMirProgramRoot root = {callable(&compilation.ir, "direct",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    CHECK(program.ir == NULL && program.templates == NULL && program.imports == NULL);
    free_compilation(&compilation);
}

static void test_source_backed_method_meter(void) {
    Compilation compilation;
    SolPackage package;
    CHECK(compile_package_at(&compilation, &package,
        SOL_TEST_SOURCE_DIR "/tests/conformance/p43_method_meter"));
    SolMirProgramRoot root = {callable(&compilation.ir, "launch",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_ENTRY};
    SolMirProgram program;
    sol_mir_program_init(&program);
    CHECK(root.callable != SOL_IR_NONE && build(&compilation.ir, &root, 1,
        NULL, 0, NULL, &program) == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    size_t direct_methods = 0;
    for (size_t index = 0; index < program.reference_count; ++index) {
        const SolMirProgramReference *reference = &program.references[index];
        if (reference->kind != SOL_MIR_PROGRAM_REFERENCE_INVOKE
            || reference->source.expression >= compilation.ir.expression_count) continue;
        const SolIrExpression *source
            = &compilation.ir.expressions[reference->source.expression];
        if (source->kind == SOL_IR_EXPR_CALL
            && source->as.call.kind == SOL_IR_CALL_METHOD) {
            ++direct_methods;
            CHECK(reference->source.file < compilation.ir.file_count
                && reference->source.start < reference->source.end);
        }
    }
    CHECK(direct_methods == 4 && direct_method_validation_work(&program) == 76
        && program.usage.discovery_work == 203
        && sol_mir_program_validate(&program, NULL));
    SolMirProgramLimits exact = {program.usage.callable_classifications,
        program.usage.references, program.usage.discovery_work};
    SolMirProgram repeated;
    sol_mir_program_init(&repeated);
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, &exact, &repeated)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED
        && repeated.usage.discovery_work == exact.max_discovery_work
        && sol_mir_program_validate(&repeated, NULL));
    sol_mir_program_free(&repeated);
    SolMirProgramLimits less = exact;
    --less.max_discovery_work;
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, &less, &repeated)
        == SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED
        && repeated.ir == NULL && repeated.references == NULL
        && repeated.usage.discovery_work == 0);
    sol_mir_program_free(&repeated);
    sol_mir_program_free(&program);
    free_e6(&compilation, &package);
}

static void test_source_backed_capability_meter(void) {
    Compilation compilation;
    SolPackage package;
    CHECK(compile_package_at(&compilation, &package,
        SOL_TEST_SOURCE_DIR "/tests/conformance/p43_capability_meter"));
    SolIrCallableId launch = callable(&compilation.ir, "launch",
        SOL_IR_CALLABLE_FUNCTION);
    SolIrCallableId tick = callable(&compilation.ir, "tick",
        SOL_IR_CALLABLE_CAPABILITY);
    SolMirProgramRoot root = {launch, SOL_MIR_PROGRAM_ROOT_ENTRY};
    SolMirProgram program;
    sol_mir_program_init(&program);
    CHECK(launch != SOL_IR_NONE && tick != SOL_IR_NONE
        && build(&compilation.ir, &root, 1, &tick, 1, NULL, &program)
            == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    size_t invokes = 0, bound = 0;
    for (size_t index = 0; index < program.reference_count; ++index) {
        invokes += program.references[index].kind == SOL_MIR_PROGRAM_REFERENCE_INVOKE;
        bound += program.references[index].kind
            == SOL_MIR_PROGRAM_REFERENCE_BOUND_OPERATION;
    }
    CHECK(invokes == 3 && bound == 3 && program.import_count == 1
        && capability_validation_work(&program) == 57
        && program.usage.discovery_work == 133
        && sol_mir_program_validate(&program, NULL));
    SolMirProgramLimits exact = {program.usage.callable_classifications,
        program.usage.references, program.usage.discovery_work};
    SolMirProgram repeated;
    sol_mir_program_init(&repeated);
    CHECK(build(&compilation.ir, &root, 1, &tick, 1, &exact, &repeated)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED
        && repeated.usage.discovery_work == exact.max_discovery_work
        && sol_mir_program_validate(&repeated, NULL));
    sol_mir_program_free(&repeated);
    SolMirProgramLimits less = exact;
    --less.max_discovery_work;
    CHECK(build(&compilation.ir, &root, 1, &tick, 1, &less, &repeated)
        == SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED
        && repeated.ir == NULL && repeated.references == NULL
        && repeated.usage.discovery_work == 0);
    sol_mir_program_free(&repeated);
    sol_mir_program_free(&program);
    free_e6(&compilation, &package);
}

static void test_malformed_owner(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module malformed\nfunction leaf() -> Int64 { return 1 }\n"
        "function root() -> Int64 { return leaf() }\n"));
    SolMirProgramRoot root = {callable(&compilation.ir, "root",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirProgram program;
    sol_mir_program_init(&program);
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    size_t count = program.template_count;
    program.template_count = compilation.ir.callable_count + 1;
    check_invalid_owner(&program, &diagnostics);
    program.template_count = count;
    SolMirProgramRoot *roots = program.roots;
    program.roots = NULL;
    check_invalid_owner(&program, &diagnostics);
    program.roots = roots;
    SolMirProgramTemplate *templates = program.templates;
    program.templates = NULL;
    check_invalid_owner(&program, &diagnostics);
    program.templates = templates;
    SolMirProgramSpecialization *specializations = program.specializations;
    CHECK(program.specialization_count == 0 && specializations == NULL);
    program.specializations = (SolMirProgramSpecialization *)program.roots;
    check_invalid_owner(&program, &diagnostics);
    program.specializations = specializations;

    roots = program.roots;
    program.roots = (SolMirProgramRoot *)program.templates;
    check_invalid_owner(&program, &diagnostics);
    program.roots = roots;
    SolMirProgramReference *references = program.references;
    program.references = (SolMirProgramReference *)(void *)(
        (unsigned char *)program.templates + 1);
    check_invalid_owner(&program, &diagnostics);
    program.references = references;

    if (program.template_count > 1) {
        SolIrCallableId second = program.templates[1].callable;
        program.templates[1].callable = program.templates[0].callable;
        check_invalid_owner(&program, &diagnostics);
        program.templates[1].callable = second;

        SolMir *first_mir = &program.templates[0].mir;
        SolMir *second_mir = &program.templates[1].mir;
        SolMirBlock *second_blocks = second_mir->blocks;
        size_t second_block_count = second_mir->block_count;
        size_t second_block_capacity = second_mir->block_capacity;
        second_mir->blocks = first_mir->blocks;
        second_mir->block_count = first_mir->block_count;
        second_mir->block_capacity = first_mir->block_capacity;
        check_invalid_owner(&program, &diagnostics);
        second_mir->blocks = second_blocks;
        second_mir->block_count = second_block_count;
        second_mir->block_capacity = second_block_capacity;
    }
    SolIrCallableId target = program.references[0].target;
    program.references[0].target = SOL_IR_NONE;
    check_invalid_owner(&program, &diagnostics);
    program.references[0].target = target;
    SolIrCallableId callable_id = program.templates[0].mir.callable;
    program.templates[0].mir.callable = SOL_IR_NONE;
    check_invalid_owner(&program, &diagnostics);
    program.templates[0].mir.callable = callable_id;

    SolMir *mir = &program.templates[0].mir;
    SolMirInstruction *instructions = mir->instructions;
    size_t instruction_count = mir->instruction_count;
    size_t instruction_capacity = mir->instruction_capacity;
    mir->instructions = (SolMirInstruction *)(void *)mir->blocks;
    mir->instruction_count = 1;
    mir->instruction_capacity = 1;
    check_invalid_owner(&program, &diagnostics);
    mir->instructions = instructions;
    mir->instruction_count = instruction_count;
    mir->instruction_capacity = instruction_capacity;

    SolMirValueId *edge_values = mir->edge_values;
    size_t edge_value_count = mir->edge_value_count;
    size_t edge_value_capacity = mir->edge_value_capacity;
    mir->edge_values = (SolMirValueId *)(void *)((unsigned char *)mir->values + 1);
    mir->edge_value_count = 1;
    mir->edge_value_capacity = 1;
    check_invalid_owner(&program, &diagnostics);
    mir->edge_values = edge_values;
    mir->edge_value_count = edge_value_count;
    mir->edge_value_capacity = edge_value_capacity;

    SolMirLoop *loops = mir->loops;
    size_t loop_count = mir->loop_count;
    size_t loop_capacity = mir->loop_capacity;
    CHECK(loop_count == 0 && loop_capacity == 0 && loops == NULL);
    mir->loops = (SolMirLoop *)(void *)program.roots;
    check_invalid_owner(&program, &diagnostics);
    mir->loops = loops;
    mir->loops = (SolMirLoop *)(void *)program.templates;
    mir->loop_count = 1;
    mir->loop_capacity = 1;
    check_invalid_owner(&program, &diagnostics);
    mir->loops = loops;
    mir->loop_count = loop_count;
    mir->loop_capacity = loop_capacity;

    size_t block_capacity = mir->block_capacity;
    mir->block_capacity = SIZE_MAX;
    check_invalid_owner(&program, &diagnostics);
    mir->block_capacity = block_capacity;
    CHECK(sol_mir_program_validate(&program, &diagnostics));
    sol_diagnostics_free(&diagnostics);
    sol_mir_program_free(&program);

    CHECK(compilation.ir.file_count == 1);
    size_t file_start = compilation.ir.files[0].aggregate_start;
    size_t file_end = compilation.ir.files[0].aggregate_end;
    compilation.ir.files[0].aggregate_start = compilation.ir.source_length;
    compilation.ir.files[0].aggregate_end = compilation.ir.source_length;
    CHECK(sol_ir_validate(&compilation.ir, NULL));
    sol_diagnostics_init(&diagnostics);
    SolMirProgramBuildRequest request = {
        &compilation.ir, &root, 1, NULL, 0, NULL,
    };
    CHECK(sol_mir_program_build(&request, &program, &diagnostics)
        == SOL_MIR_PROGRAM_BUILD_INVALID_IR);
    CHECK(diagnostics.count != 0 || diagnostics.allocation_failed);
    CHECK(program.ir == NULL && program.templates == NULL);
    sol_diagnostics_free(&diagnostics);
    compilation.ir.files[0].aggregate_start = file_start;
    compilation.ir.files[0].aggregate_end = file_end;
    CHECK(sol_ir_validate(&compilation.ir, NULL));
    free_compilation(&compilation);
}

int main(int argc, char **argv) {
    const char *selected = NULL;
    if (argc == 3 && !strcmp(argv[1], "--canonical-case")) {
        selected = argv[2];
    } else if (argc != 1) {
        return 2;
    }
    if (selected != NULL) {
        if (!strcmp(selected, "representation")) {
            SOL_TEST_RUN(test_canonical_representation());
        } else if (!strcmp(selected, "active-fields")) {
            SOL_TEST_RUN(test_canonical_active_fields());
        } else {
            return 2;
        }
    } else {
        SOL_TEST_RUN(test_e6_closures_and_determinism());
        SOL_TEST_RUN(test_recursion_references_and_unreachable());
        SOL_TEST_RUN(test_evidence_and_callbacks());
        SOL_TEST_RUN(test_callback_source_limitations());
        SOL_TEST_RUN(test_callback_producer_discovery_limits());
        SOL_TEST_RUN(test_import_policy_and_lifecycle());
        SOL_TEST_RUN(test_predicate_proof_and_handler_relations());
        SOL_TEST_RUN(test_derived_bodyful_capability_is_transactionally_unsupported());
        SOL_TEST_RUN(test_source_backed_method_meter());
        SOL_TEST_RUN(test_source_backed_capability_meter());
        SOL_TEST_RUN(test_malformed_owner());
        SOL_TEST_RUN(test_canonical_representation());
        SOL_TEST_RUN(test_canonical_active_fields());
    }
    if (failures != 0) {
        fprintf(stderr, "%d MIR program test(s) failed\n", failures);
        return 1;
    }
    printf("MIR program tests passed\n");
    return 0;
}
#undef main
SOL_TEST_ARGV_ENTRY("mir_program")
