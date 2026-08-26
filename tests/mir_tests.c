#include "sol/mir.h"
#include "sol/effects.h"
#include "sol/lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", \
                __FILE__, __LINE__, #condition); \
            ++failures; \
        } \
    } while (0)

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

static bool compile(Compilation *compilation, const char *text) {
    memset(compilation, 0, sizeof(*compilation));
    sol_tokens_init(&compilation->tokens);
    sol_diagnostics_init(&compilation->diagnostics);
    sol_syntax_tree_init(&compilation->syntax);
    sol_hir_module_init(&compilation->hir);
    sol_type_table_init(&compilation->types);
    sol_effect_table_init(&compilation->effects);
    sol_contract_table_init(&compilation->contracts);
    sol_ir_init(&compilation->ir);
    return sol_source_from_text(&compilation->source, "mir.sol", text)
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

static SolIrCallableId callable(const SolIr *ir, const char *name) {
    for (size_t index = 0; index < ir->callable_count; ++index) {
        if (strcmp(ir->callables[index].name, name) == 0) return index;
    }
    return SOL_IR_NONE;
}

static SolIrLocalId local(const SolIr *ir, SolIrDefinitionId owner,
    const char *name) {
    for (size_t index = 0; index < ir->local_count; ++index) {
        if (ir->locals[index].owner == owner
            && strcmp(ir->locals[index].name, name) == 0) return index;
    }
    return SOL_IR_NONE;
}

static bool bytes_equal(const void *left, const void *right,
    size_t count, size_t size) {
    return count == 0 || memcmp(left, right, count * size) == 0;
}

static bool mir_equal(const SolMir *left, const SolMir *right) {
    return left->callable == right->callable && left->entry == right->entry
        && left->block_count == right->block_count
        && left->instruction_count == right->instruction_count
        && left->value_count == right->value_count
        && left->parameter_value_count == right->parameter_value_count
        && left->edge_value_count == right->edge_value_count
        && left->call_argument_count == right->call_argument_count
        && left->loop_count == right->loop_count
        && left->construct_operand_count == right->construct_operand_count
        && left->temporary_count == right->temporary_count
        && bytes_equal(left->blocks, right->blocks,
            left->block_count, sizeof(*left->blocks))
        && bytes_equal(left->instructions, right->instructions,
            left->instruction_count, sizeof(*left->instructions))
        && bytes_equal(left->values, right->values,
            left->value_count, sizeof(*left->values))
        && bytes_equal(left->parameter_values, right->parameter_values,
            left->parameter_value_count, sizeof(*left->parameter_values))
        && bytes_equal(left->edge_values, right->edge_values,
            left->edge_value_count, sizeof(*left->edge_values))
        && bytes_equal(left->call_arguments, right->call_arguments,
            left->call_argument_count, sizeof(*left->call_arguments))
        && bytes_equal(left->loops, right->loops,
            left->loop_count, sizeof(*left->loops))
        && bytes_equal(left->construct_operands, right->construct_operands,
            left->construct_operand_count, sizeof(*left->construct_operands))
        && bytes_equal(left->temporaries, right->temporaries,
            left->temporary_count, sizeof(*left->temporaries));
}

static void test_initial_lowering_and_determinism(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module mir_initial\n"
        "function straight() -> Int64 { let first = 1 return first + 2 }\n"
        "function choose(flag: Bool) -> Int64 { if flag { 7 } else { 9 } }\n"
        "function assign() -> Int64 { var value = 1 value = 2 return value }\n"
        "function early(flag: Bool) -> Int64 { let first = \"first\" "
        "if flag { return 1 } else { () } let later = \"later\" return 2 }\n"
        "function both(flag: Bool) -> Int64 { "
        "if flag { return 1 } else { return 2 } }\n"
        "function ending() -> () { 1 let value = 2 }\n"
        "function fail() -> () effects { panic } "
        "{ let pending = \"pending\" panic \"boom\" }\n"));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));

    SolMir straight;
    SolMir repeated;
    sol_mir_init(&straight);
    sol_mir_init(&repeated);
    SolIrCallableId straight_id = callable(&compilation.ir, "straight");
    CHECK(sol_mir_lower_callable(&compilation.ir, straight_id, &straight,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(sol_mir_lower_callable(&compilation.ir, straight_id, &repeated,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(sol_mir_validate(&compilation.ir, &straight, NULL));
    CHECK(mir_equal(&straight, &repeated));
    CHECK(straight.block_count == 1);
    CHECK(straight.instructions[0].kind == SOL_MIR_INST_CONST_INT64);
    CHECK(straight.instructions[1].kind == SOL_MIR_INST_STORAGE_LIVE);
    CHECK(straight.instructions[2].kind == SOL_MIR_INST_STORE);
    CHECK(straight.instructions[3].kind == SOL_MIR_INST_LOAD_COPY);
    CHECK(straight.instructions[4].kind == SOL_MIR_INST_CONST_INT64);
    CHECK(straight.instructions[5].kind == SOL_MIR_INST_BINARY);
    CHECK(straight.instructions[6].kind == SOL_MIR_INST_DROP_IF_INITIALIZED);
    CHECK(straight.instructions[7].kind == SOL_MIR_INST_STORAGE_DEAD);
    CHECK(straight.blocks[0].terminator.kind == SOL_MIR_TERM_RETURN);
    sol_mir_free(&straight);
    sol_mir_free(&repeated);

    SolMir choose;
    sol_mir_init(&choose);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "choose"), &choose,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(choose.block_count == 4);
    CHECK(choose.blocks[0].terminator.kind == SOL_MIR_TERM_BRANCH);
    CHECK(choose.blocks[1].terminator.kind == SOL_MIR_TERM_GOTO);
    CHECK(choose.blocks[2].terminator.kind == SOL_MIR_TERM_GOTO);
    CHECK(choose.blocks[3].parameters.count == 1);
    CHECK(choose.blocks[3].terminator.kind == SOL_MIR_TERM_RETURN);
    CHECK(choose.blocks[1].terminator.as.go_to.block == 3);
    CHECK(choose.blocks[2].terminator.as.go_to.block == 3);
    CHECK(choose.edge_value_count == 2);
    CHECK(choose.instructions[choose.instruction_count - 2].kind
        == SOL_MIR_INST_DROP_IF_INITIALIZED);
    CHECK(choose.instructions[choose.instruction_count - 1].kind
        == SOL_MIR_INST_STORAGE_DEAD);
    sol_mir_free(&choose);

    SolMir assign;
    sol_mir_init(&assign);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "assign"), &assign,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    size_t stores = 0;
    for (size_t index = 0; index < assign.instruction_count; ++index) {
        stores += assign.instructions[index].kind == SOL_MIR_INST_STORE;
    }
    CHECK(stores == 2);
    sol_mir_free(&assign);

    SolIrCallableId early_id = callable(&compilation.ir, "early");
    SolMir early;
    sol_mir_init(&early);
    CHECK(sol_mir_lower_callable(&compilation.ir, early_id, &early,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    SolIrDefinitionId early_owner = compilation.ir.callables[early_id].owner;
    SolIrLocalId first = local(&compilation.ir, early_owner, "first");
    SolIrLocalId later = local(&compilation.ir, early_owner, "later");
    size_t two_drops = 0;
    size_t three_drops = 0;
    for (size_t block = 0; block < early.block_count; ++block) {
        if (early.blocks[block].terminator.kind != SOL_MIR_TERM_RETURN) continue;
        SolMirSlice instructions = early.blocks[block].instructions;
        SolIrLocalId drops[3];
        size_t drop_count = 0;
        for (size_t index = 0; index < instructions.count; ++index) {
            const SolMirInstruction *instruction
                = &early.instructions[instructions.offset + index];
            if (instruction->kind == SOL_MIR_INST_DROP_IF_INITIALIZED
                && drop_count < 3) drops[drop_count++] = instruction->as.local;
        }
        if (drop_count == 2) {
            ++two_drops;
            CHECK(drops[0] == first);
        } else if (drop_count == 3) {
            ++three_drops;
            CHECK(drops[0] == later && drops[1] == first);
        }
    }
    CHECK(two_drops == 1 && three_drops == 1);
    sol_mir_free(&early);

    SolMir both;
    sol_mir_init(&both);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "both"), &both,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(both.block_count == 3);
    CHECK(both.blocks[0].terminator.kind == SOL_MIR_TERM_BRANCH);
    CHECK(both.blocks[1].terminator.kind == SOL_MIR_TERM_RETURN);
    CHECK(both.blocks[2].terminator.kind == SOL_MIR_TERM_RETURN);
    sol_mir_free(&both);

    SolMir ending;
    sol_mir_init(&ending);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "ending"), &ending,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    SolMirValueId ending_value = ending.blocks[0].terminator.as.value;
    CHECK(ending.values[ending_value].type
        == compilation.ir.callables[ending.callable].result);
    const SolMirInstruction *ending_result
        = &ending.instructions[ending.values[ending_value].definition];
    CHECK(ending_result->kind == SOL_MIR_INST_EXPRESSION_RESULT);
    CHECK(ending.instructions[ending.values[ending_result->as.operand].definition]
        .kind == SOL_MIR_INST_CONST_UNIT);
    sol_mir_free(&ending);

    SolMir panic;
    sol_mir_init(&panic);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "fail"), &panic,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(panic.blocks[0].terminator.kind == SOL_MIR_TERM_PANIC);
    CHECK(panic.instructions[panic.instruction_count - 2].kind
        == SOL_MIR_INST_DROP_IF_INITIALIZED);
    CHECK(panic.instructions[panic.instruction_count - 1].kind
        == SOL_MIR_INST_STORAGE_DEAD);
    sol_mir_free(&panic);
    free_compilation(&compilation);
}

static void test_transactional_unsupported(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module mir_unsupported\n"
        "function classify(value: Bool) -> Int64 { "
        "return match value { true => 1 false => 0 } }\n"
        "function consume(value: Int64) -> Int64 { return value }\n"
        "function block_argument() -> Int64 { return consume({ 1 }) }\n"
        "function contracted(value: Int64) -> Int64 "
        "requires { value > 0 } { return value }\n"));
    SolMir mir;
    sol_mir_init(&mir);
    size_t before = compilation.diagnostics.count;
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "classify"), &mir,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(sol_mir_validate(&compilation.ir, &mir, NULL));
    CHECK(compilation.diagnostics.count == before);
    size_t pattern_tests = 0;
    for (size_t instruction = 0; instruction < mir.instruction_count;
        ++instruction) {
        pattern_tests += mir.instructions[instruction].kind
            == SOL_MIR_INST_PATTERN_TEST;
    }
    CHECK(pattern_tests == 2);
    sol_mir_free(&mir);

    sol_mir_init(&mir);
    before = compilation.diagnostics.count;
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "block_argument"), &mir,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(mir.temporary_count == 1 && mir.call_argument_count == 1);
    CHECK(compilation.diagnostics.count == before);
    sol_mir_free(&mir);

    sol_mir_init(&mir);
    before = compilation.diagnostics.count;
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "contracted"), &mir,
        &compilation.diagnostics) == SOL_MIR_LOWER_UNSUPPORTED);
    CHECK(mir.callable == SOL_IR_NONE && mir.blocks == NULL);
    CHECK(compilation.diagnostics.count == before + 1);
    sol_mir_free(&mir);

    SolIrCallableId contracted_id = callable(&compilation.ir, "contracted");
    SolIrExpressionId body = compilation.ir.callables[contracted_id].body;
    compilation.ir.callables[contracted_id].body = compilation.ir.expression_count;
    sol_mir_init(&mir);
    CHECK(sol_mir_lower_callable(&compilation.ir, contracted_id, &mir,
        &compilation.diagnostics) == SOL_MIR_LOWER_FAILED);
    CHECK(mir.callable == SOL_IR_NONE && mir.blocks == NULL
        && mir.instructions == NULL);
    sol_mir_free(&mir);
    compilation.ir.callables[contracted_id].body = body;
    free_compilation(&compilation);
}

static void test_calls_and_remaining_local_control(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module mir_control\n"
        "function positive(value: borrow Int64) -> Bool { return value > 0 }\n"
        "function sum(left: Int64, right: Int64) -> Int64 { return left + right }\n"
        "function replace(value: inout Int64) -> () { value = 9 }\n"
        "function foreign() -> () { region other { () } }\n"
        "function calls(flag: Bool) -> Int64 effects { panic } { "
        "var value = 1 region work { "
        "require flag || positive(value) else { panic \"required\" } "
        "value = sum(value, 2) replace(value) } "
        "region later { () } return value }\n"
        "function impossible(flag: Bool) -> Int64 { "
        "if flag && true { return 1 } else { "
        "unreachable because { flag == false } } }\n"
        "function proofs(flag: Bool) -> Int64 { if flag { "
        "unreachable because { flag } } else { "
        "unreachable because { flag == false } } }\n"));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));

    SolMir calls;
    SolMir repeated;
    sol_mir_init(&calls);
    sol_mir_init(&repeated);
    SolIrCallableId calls_id = callable(&compilation.ir, "calls");
    CHECK(sol_mir_lower_callable(&compilation.ir, calls_id, &calls,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(sol_mir_lower_callable(&compilation.ir, calls_id, &repeated,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(sol_mir_validate(&compilation.ir, &calls, NULL));
    CHECK(mir_equal(&calls, &repeated));

    size_t invokes = 0;
    size_t enters = 0;
    size_t exits = 0;
    bool saw_shared = false;
    bool saw_exclusive = false;
    bool saw_owned = false;
    SolMirBlockId first_invoke = SOL_MIR_NONE;
    SolMirBlockId owned_invoke = SOL_MIR_NONE;
    SolMirInstructionId first_region_exit = SOL_MIR_NONE;
    for (size_t index = 0; index < calls.instruction_count; ++index) {
        const SolMirInstruction *instruction = &calls.instructions[index];
        enters += instruction->kind == SOL_MIR_INST_REGION_ENTER;
        exits += instruction->kind == SOL_MIR_INST_REGION_EXIT;
        if (instruction->kind == SOL_MIR_INST_REGION_EXIT
            && first_region_exit == SOL_MIR_NONE) first_region_exit = index;
        if (instruction->kind == SOL_MIR_INST_BINARY) {
            CHECK(instruction->as.binary.operator_kind != SOL_TOKEN_AMP_AMP);
            CHECK(instruction->as.binary.operator_kind != SOL_TOKEN_PIPE_PIPE);
        }
    }
    for (size_t block = 0; block < calls.block_count; ++block) {
        const SolMirTerminator *term = &calls.blocks[block].terminator;
        if (term->kind != SOL_MIR_TERM_INVOKE) continue;
        ++invokes;
        if (first_invoke == SOL_MIR_NONE) first_invoke = block;
        CHECK(term->as.invoke.normal_edge.arguments.count == 1);
        CHECK(term->as.invoke.failure_edge.arguments.count == 0);
        CHECK(calls.blocks[term->as.invoke.failure_edge.block].terminator.kind
            == SOL_MIR_TERM_RESUME_FAILURE);
        CHECK(calls.values[term->as.invoke.result].kind
            == SOL_MIR_VALUE_TERMINATOR);
        SolMirSlice arguments = term->as.invoke.arguments;
        for (size_t index = 0; index < arguments.count; ++index) {
            const SolMirCallArgument *argument
                = &calls.call_arguments[arguments.offset + index];
            saw_shared = saw_shared || argument->access == SOL_ACCESS_SHARED;
            saw_exclusive = saw_exclusive
                || argument->access == SOL_ACCESS_EXCLUSIVE;
            saw_owned = saw_owned || argument->access == SOL_ACCESS_OWNED;
            if (argument->access != SOL_ACCESS_OWNED) {
                CHECK(argument->temporary == SOL_MIR_NONE);
                CHECK(argument->place < compilation.ir.place_count);
            }
        }
        if (arguments.count == 2
            && calls.call_arguments[arguments.offset].access == SOL_ACCESS_OWNED
            && calls.call_arguments[arguments.offset + 1].access
                == SOL_ACCESS_OWNED) owned_invoke = block;
    }
    CHECK(invokes == 3);
    CHECK(enters == 2 && exits >= 4);
    CHECK(saw_shared && saw_exclusive && saw_owned);
    CHECK(first_invoke != SOL_MIR_NONE);
    CHECK(owned_invoke != SOL_MIR_NONE);
    CHECK(first_region_exit != SOL_MIR_NONE);

    SolMirTerminator *invoke = &calls.blocks[first_invoke].terminator;
    size_t argument_offset = invoke->as.invoke.arguments.offset;
    SolAccessMode access = calls.call_arguments[argument_offset].access;
    calls.call_arguments[argument_offset].access = SOL_ACCESS_OWNED;
    CHECK(!sol_mir_validate(&compilation.ir, &calls, NULL));
    calls.call_arguments[argument_offset].access = access;
    SolMirValueKind result_kind = calls.values[invoke->as.invoke.result].kind;
    calls.values[invoke->as.invoke.result].kind = SOL_MIR_VALUE_INSTRUCTION;
    CHECK(!sol_mir_validate(&compilation.ir, &calls, NULL));
    calls.values[invoke->as.invoke.result].kind = result_kind;

    SolMirSlice owned_arguments
        = calls.blocks[owned_invoke].terminator.as.invoke.arguments;
    SolMirTemporaryId first_owned
        = calls.call_arguments[owned_arguments.offset].temporary;
    SolMirTemporaryId second_owned
        = calls.call_arguments[owned_arguments.offset + 1].temporary;
    calls.call_arguments[owned_arguments.offset].temporary = second_owned;
    calls.call_arguments[owned_arguments.offset + 1].temporary = first_owned;
    CHECK(!sol_mir_validate(&compilation.ir, &calls, NULL));
    calls.call_arguments[owned_arguments.offset].temporary = first_owned;
    calls.call_arguments[owned_arguments.offset + 1].temporary = second_owned;
    SolMirInstructionId first_wrapper = SOL_MIR_NONE;
    SolMirInstructionId second_wrapper = SOL_MIR_NONE;
    for (size_t instruction = 0; instruction < calls.instruction_count;
        ++instruction) {
        if (calls.instructions[instruction].kind
            != SOL_MIR_INST_TEMPORARY_INIT) continue;
        if (calls.instructions[instruction].as.temporary_init.temporary
            == first_owned) first_wrapper = instruction;
        if (calls.instructions[instruction].as.temporary_init.temporary
            == second_owned) second_wrapper = instruction;
    }
    CHECK(first_wrapper != SOL_MIR_NONE && second_wrapper != SOL_MIR_NONE);
    SolMirValueId first_operand
        = calls.instructions[first_wrapper].as.temporary_init.value;
    SolMirValueId second_operand
        = calls.instructions[second_wrapper].as.temporary_init.value;
    calls.instructions[first_wrapper].as.temporary_init.value = second_operand;
    calls.instructions[second_wrapper].as.temporary_init.value = first_operand;
    CHECK(!sol_mir_validate(&compilation.ir, &calls, NULL));
    calls.instructions[first_wrapper].as.temporary_init.value = first_operand;
    calls.instructions[second_wrapper].as.temporary_init.value = second_operand;

    SolMirTerminator *owned_term = &calls.blocks[owned_invoke].terminator;
    SolMirBlockId normal_block = owned_term->as.invoke.normal_edge.block;
    size_t parameter_offset = calls.blocks[normal_block].parameters.offset;
    calls.blocks[normal_block].parameters.offset = SIZE_MAX;
    CHECK(!sol_mir_validate(&compilation.ir, &calls, NULL));
    calls.blocks[normal_block].parameters.offset = parameter_offset;

    SolMirValueId normal_result = owned_term->as.invoke.result;
    SolMirValueId transported = calls.edge_values[
        owned_term->as.invoke.normal_edge.arguments.offset];
    SolMirValueId substitute = first_operand;
    calls.edge_values[owned_term->as.invoke.normal_edge.arguments.offset]
        = substitute;
    CHECK(calls.values[substitute].type == calls.values[normal_result].type);
    CHECK(!sol_mir_validate(&compilation.ir, &calls, NULL));
    calls.edge_values[owned_term->as.invoke.normal_edge.arguments.offset]
        = transported;

    SolMirBlockId cleanup_block = owned_term->as.invoke.failure_edge.block;
    SolMirSlice cleanup_instructions = calls.blocks[cleanup_block].instructions;
    SolMirInstructionId dead[2] = {SOL_MIR_NONE, SOL_MIR_NONE};
    size_t dead_count = 0;
    for (size_t index = 0; index < cleanup_instructions.count
        && dead_count < 2; ++index) {
        SolMirInstructionId id = cleanup_instructions.offset + index;
        if (calls.instructions[id].kind == SOL_MIR_INST_STORAGE_DEAD) {
            dead[dead_count++] = id;
        }
    }
    CHECK(dead_count == 2);
    SolIrLocalId first_dead = calls.instructions[dead[0]].as.local;
    SolIrLocalId second_dead = calls.instructions[dead[1]].as.local;
    calls.instructions[dead[0] - 1].as.local = second_dead;
    calls.instructions[dead[0]].as.local = second_dead;
    calls.instructions[dead[1] - 1].as.local = first_dead;
    calls.instructions[dead[1]].as.local = first_dead;
    CHECK(!sol_mir_validate(&compilation.ir, &calls, NULL));
    calls.instructions[dead[0] - 1].as.local = first_dead;
    calls.instructions[dead[0]].as.local = first_dead;
    calls.instructions[dead[1] - 1].as.local = second_dead;
    calls.instructions[dead[1]].as.local = second_dead;

    SolMirInstructionId region_enters[2] = {SOL_MIR_NONE, SOL_MIR_NONE};
    size_t region_enter_count = 0;
    for (size_t index = 0; index < calls.instruction_count
        && region_enter_count < 2; ++index) {
        if (calls.instructions[index].kind == SOL_MIR_INST_REGION_ENTER) {
            region_enters[region_enter_count++] = index;
        }
    }
    CHECK(region_enter_count == 2);
    SolIrStatementId first_region
        = calls.instructions[region_enters[0]].as.region;
    SolIrStatementId second_region
        = calls.instructions[region_enters[1]].as.region;
    SolSpan first_region_span = calls.instructions[region_enters[0]].span;
    SolSpan second_region_span = calls.instructions[region_enters[1]].span;
    for (size_t index = 0; index < calls.instruction_count; ++index) {
        if (calls.instructions[index].kind != SOL_MIR_INST_REGION_ENTER
            && calls.instructions[index].kind != SOL_MIR_INST_REGION_EXIT) {
            continue;
        }
        if (calls.instructions[index].as.region == first_region) {
            calls.instructions[index].as.region = second_region;
        } else if (calls.instructions[index].as.region == second_region) {
            calls.instructions[index].as.region = first_region;
        }
    }
    calls.instructions[region_enters[0]].span = second_region_span;
    calls.instructions[region_enters[1]].span = first_region_span;
    CHECK(!sol_mir_validate(&compilation.ir, &calls, NULL));
    for (size_t index = 0; index < calls.instruction_count; ++index) {
        if (calls.instructions[index].kind != SOL_MIR_INST_REGION_ENTER
            && calls.instructions[index].kind != SOL_MIR_INST_REGION_EXIT) {
            continue;
        }
        if (calls.instructions[index].as.region == first_region) {
            calls.instructions[index].as.region = second_region;
        } else if (calls.instructions[index].as.region == second_region) {
            calls.instructions[index].as.region = first_region;
        }
    }
    calls.instructions[region_enters[0]].span = first_region_span;
    calls.instructions[region_enters[1]].span = second_region_span;

    SolIrStatementId own_region
        = calls.instructions[first_region_exit].as.region;
    SolIrStatementId foreign_region = SOL_IR_NONE;
    for (size_t statement = 0; statement < compilation.ir.statement_count;
        ++statement) {
        if (compilation.ir.statements[statement].kind == SOL_IR_STATEMENT_REGION
            && statement != own_region) {
            foreign_region = statement;
            break;
        }
    }
    CHECK(foreign_region != SOL_IR_NONE);
    for (size_t index = 0; index < calls.instruction_count; ++index) {
        if ((calls.instructions[index].kind == SOL_MIR_INST_REGION_ENTER
                || calls.instructions[index].kind == SOL_MIR_INST_REGION_EXIT)
            && calls.instructions[index].as.region == own_region) {
            calls.instructions[index].as.region = foreign_region;
        }
    }
    CHECK(!sol_mir_validate(&compilation.ir, &calls, NULL));
    for (size_t index = 0; index < calls.instruction_count; ++index) {
        if ((calls.instructions[index].kind == SOL_MIR_INST_REGION_ENTER
                || calls.instructions[index].kind == SOL_MIR_INST_REGION_EXIT)
            && calls.instructions[index].as.region == foreign_region) {
            calls.instructions[index].as.region = own_region;
        }
    }

    calls.instructions[first_region_exit].kind = SOL_MIR_INST_REGION_ENTER;
    CHECK(!sol_mir_validate(&compilation.ir, &calls, NULL));
    calls.instructions[first_region_exit].kind = SOL_MIR_INST_REGION_EXIT;
    CHECK(sol_mir_validate(&compilation.ir, &calls, NULL));
    sol_mir_free(&calls);
    sol_mir_free(&repeated);

    SolMir impossible;
    sol_mir_init(&impossible);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "impossible"), &impossible,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    size_t unreachable = 0;
    for (size_t block = 0; block < impossible.block_count; ++block) {
        if (impossible.blocks[block].terminator.kind
            == SOL_MIR_TERM_UNREACHABLE) {
            ++unreachable;
            CHECK(impossible.blocks[block].terminator.as.unreachable.obligation
                < compilation.ir.unreachable_obligation_count);
        }
    }
    CHECK(unreachable == 1);
    CHECK(sol_mir_validate(&compilation.ir, &impossible, NULL));
    sol_mir_free(&impossible);

    SolMir proofs;
    sol_mir_init(&proofs);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "proofs"), &proofs,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    SolMirBlockId proof_blocks[2] = {SOL_MIR_NONE, SOL_MIR_NONE};
    size_t proof_count = 0;
    for (size_t block = 0; block < proofs.block_count && proof_count < 2;
        ++block) {
        if (proofs.blocks[block].terminator.kind
            == SOL_MIR_TERM_UNREACHABLE) proof_blocks[proof_count++] = block;
    }
    CHECK(proof_count == 2);
    SolIrStatementId first_statement = proofs.blocks[proof_blocks[0]]
        .terminator.as.unreachable.statement;
    size_t first_obligation = proofs.blocks[proof_blocks[0]]
        .terminator.as.unreachable.obligation;
    SolIrStatementId second_statement = proofs.blocks[proof_blocks[1]]
        .terminator.as.unreachable.statement;
    size_t second_obligation = proofs.blocks[proof_blocks[1]]
        .terminator.as.unreachable.obligation;
    SolSpan first_span = proofs.blocks[proof_blocks[0]].terminator.span;
    SolSpan second_span = proofs.blocks[proof_blocks[1]].terminator.span;
    size_t first_order = proofs.blocks[proof_blocks[0]].order;
    size_t second_order = proofs.blocks[proof_blocks[1]].order;
    proofs.blocks[proof_blocks[0]].terminator.as.unreachable.statement
        = second_statement;
    proofs.blocks[proof_blocks[0]].terminator.as.unreachable.obligation
        = second_obligation;
    proofs.blocks[proof_blocks[1]].terminator.as.unreachable.statement
        = first_statement;
    proofs.blocks[proof_blocks[1]].terminator.as.unreachable.obligation
        = first_obligation;
    proofs.blocks[proof_blocks[0]].terminator.span = second_span;
    proofs.blocks[proof_blocks[1]].terminator.span = first_span;
    proofs.blocks[proof_blocks[0]].order = second_order;
    proofs.blocks[proof_blocks[1]].order = first_order;
    CHECK(!sol_mir_validate(&compilation.ir, &proofs, NULL));
    proofs.blocks[proof_blocks[0]].terminator.as.unreachable.statement
        = first_statement;
    proofs.blocks[proof_blocks[0]].terminator.as.unreachable.obligation
        = first_obligation;
    proofs.blocks[proof_blocks[1]].terminator.as.unreachable.statement
        = second_statement;
    proofs.blocks[proof_blocks[1]].terminator.as.unreachable.obligation
        = second_obligation;
    proofs.blocks[proof_blocks[0]].terminator.span = first_span;
    proofs.blocks[proof_blocks[1]].terminator.span = second_span;
    proofs.blocks[proof_blocks[0]].order = first_order;
    proofs.blocks[proof_blocks[1]].order = second_order;
    CHECK(sol_mir_validate(&compilation.ir, &proofs, NULL));
    sol_mir_free(&proofs);

    free_compilation(&compilation);
}

static void test_validator_rejects_corruption(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module mir_invalid\n"
        "function choose(flag: Bool) -> Int64 { if flag { 1 } else { 2 } }\n"));
    SolMir mir;
    sol_mir_init(&mir);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "choose"), &mir,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(sol_mir_validate(&compilation.ir, &mir, NULL));

    SolMirBlockId target = mir.blocks[0].terminator.as.branch.true_edge.block;
    mir.blocks[0].terminator.as.branch.true_edge.block = mir.block_count;
    CHECK(!sol_mir_validate(&compilation.ir, &mir, NULL));
    mir.blocks[0].terminator.as.branch.true_edge.block = target;

    SolMirValueId condition = mir.blocks[0].terminator.as.branch.condition;
    SolIrTypeId condition_type = mir.values[condition].type;
    mir.values[condition].type = compilation.ir.callables[mir.callable].result;
    CHECK(!sol_mir_validate(&compilation.ir, &mir, NULL));
    mir.values[condition].type = condition_type;

    size_t argument_count = mir.blocks[1].terminator.as.go_to.arguments.count;
    mir.blocks[1].terminator.as.go_to.arguments.count = 0;
    CHECK(!sol_mir_validate(&compilation.ir, &mir, NULL));
    mir.blocks[1].terminator.as.go_to.arguments.count = argument_count;

    SolMirBlockId false_target
        = mir.blocks[0].terminator.as.branch.false_edge.block;
    mir.blocks[0].terminator.as.branch.false_edge.block = target;
    CHECK(!sol_mir_validate(&compilation.ir, &mir, NULL));
    mir.blocks[0].terminator.as.branch.false_edge.block = false_target;

    size_t instruction_count = mir.blocks[0].instructions.count;
    mir.blocks[0].instructions.count = 0;
    CHECK(!sol_mir_validate(&compilation.ir, &mir, NULL));
    mir.blocks[0].instructions.count = instruction_count;

    size_t entry_parameters = mir.blocks[0].parameters.count;
    mir.blocks[0].parameters.count = 1;
    CHECK(!sol_mir_validate(&compilation.ir, &mir, NULL));
    mir.blocks[0].parameters.count = entry_parameters;

    SolMirInstructionKind parameter_kind = mir.instructions[0].kind;
    mir.instructions[0].kind = SOL_MIR_INST_STORAGE_LIVE;
    CHECK(!sol_mir_validate(&compilation.ir, &mir, NULL));
    mir.instructions[0].kind = parameter_kind;

    SolMirInstructionKind final_kind
        = mir.instructions[mir.instruction_count - 1].kind;
    mir.instructions[mir.instruction_count - 1].kind
        = SOL_MIR_INST_DROP_IF_INITIALIZED;
    CHECK(!sol_mir_validate(&compilation.ir, &mir, NULL));
    mir.instructions[mir.instruction_count - 1].kind = final_kind;

    SolMirValueId result = mir.instructions[2].result;
    mir.instructions[2].result = mir.instructions[1].result;
    CHECK(!sol_mir_validate(&compilation.ir, &mir, NULL));
    mir.instructions[2].result = result;

    SolMirInstructionKind load_kind = mir.instructions[1].kind;
    mir.instructions[1].kind = SOL_MIR_INST_LOAD_MOVE;
    CHECK(!sol_mir_validate(&compilation.ir, &mir, NULL));
    mir.instructions[1].kind = load_kind;

    SolMirValueKind value_kind = mir.values[condition].kind;
    mir.values[condition].kind = (SolMirValueKind)99;
    CHECK(!sol_mir_validate(&compilation.ir, &mir, NULL));
    mir.values[condition].kind = value_kind;

    SolIrTypeId literal_type = mir.instructions[2].type;
    mir.instructions[2].type = condition_type;
    CHECK(!sol_mir_validate(&compilation.ir, &mir, NULL));
    mir.instructions[2].type = literal_type;

    size_t block_count = mir.block_count;
    size_t block_capacity = mir.block_capacity;
    mir.block_count = SIZE_MAX / sizeof(SolMirBlockId) + 1;
    mir.block_capacity = mir.block_count;
    CHECK(!sol_mir_validate(&compilation.ir, &mir, NULL));
    mir.block_count = block_count;
    mir.block_capacity = block_capacity;
    CHECK(sol_mir_validate(&compilation.ir, &mir, NULL));
    sol_mir_free(&mir);
    free_compilation(&compilation);
}

static void test_loop_control_lowering(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module mir_loops\n"
        "function counted(limit: Int64) -> Int64 { var n = 0 "
        "while n < limit decreases { limit - n } { let item = \"x\" "
        "n = n + 1 if n == 2 { continue } else { () } } return n }\n"
        "function transfers(flag: Bool) -> Int64 effects { diverge } { "
        "var n = 0 loop { region inner { if flag { break } else { "
        "n = n + 1 continue } } } return n }\n"
        "function nested() -> Int64 { loop { loop { break } break } return 1 }\n"
        "function condition_break() -> Int64 { while { break } {} return 6 }\n"
        "function condition_continue(flag: Bool) -> Int64 effects { diverge } { "
        "while if flag { region condition { continue } } else { false } {} "
        "return 7 }\n"
        "function trailing() -> Int64 { return 1 loop {} }\n"
        "function skipped_break() -> Int64 { loop { return 2 break } }\n"
        "function endless() -> () effects { diverge } { loop {} }\n"));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));

    SolMir counted;
    SolMir repeated;
    sol_mir_init(&counted);
    sol_mir_init(&repeated);
    SolIrCallableId counted_id = callable(&compilation.ir, "counted");
    CHECK(sol_mir_lower_callable(&compilation.ir, counted_id, &counted,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(sol_mir_lower_callable(&compilation.ir, counted_id, &repeated,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(sol_mir_validate(&compilation.ir, &counted, NULL));
    CHECK(mir_equal(&counted, &repeated));
    CHECK(counted.loop_count == 1);
    const SolMirLoop *while_loop = &counted.loops[0];
    CHECK(compilation.ir.statements[while_loop->statement].kind
        == SOL_IR_STATEMENT_WHILE);
    CHECK(while_loop->header < counted.block_count);
    CHECK(while_loop->body < counted.block_count);
    CHECK(while_loop->exit < counted.block_count);
    CHECK(while_loop->obligations.count == 2);
    bool saw_continue = false;
    bool saw_backedge = false;
    bool continue_cleaned_item = false;
    SolIrDefinitionId counted_owner = compilation.ir.callables[counted_id].owner;
    SolIrLocalId item = local(&compilation.ir, counted_owner, "item");
    for (size_t block = 0; block < counted.block_count; ++block) {
        const SolMirTerminator *term = &counted.blocks[block].terminator;
        if (term->kind == SOL_MIR_TERM_CONTINUE) {
            saw_continue = true;
            CHECK(term->as.transfer.loop == 0);
            CHECK(term->as.transfer.edge.block == while_loop->header);
            SolMirSlice instructions = counted.blocks[block].instructions;
            for (size_t index = 0; index < instructions.count; ++index) {
                const SolMirInstruction *instruction
                    = &counted.instructions[instructions.offset + index];
                continue_cleaned_item = continue_cleaned_item
                    || (instruction->kind == SOL_MIR_INST_STORAGE_DEAD
                        && instruction->as.local == item);
            }
        }
        saw_backedge = saw_backedge
            || (term->kind == SOL_MIR_TERM_GOTO
                && term->as.go_to.block == while_loop->header
                && block != counted.entry);
    }
    CHECK(saw_continue && saw_backedge && continue_cleaned_item);
    size_t obligation_count = counted.loops[0].obligations.count;
    counted.loops[0].obligations.count = 0;
    CHECK(!sol_mir_validate(&compilation.ir, &counted, NULL));
    counted.loops[0].obligations.count = obligation_count;
    SolMirBlockId backedge_target = counted.blocks[while_loop->backedge]
        .terminator.as.go_to.block;
    counted.blocks[while_loop->backedge].terminator.as.go_to.block
        = while_loop->exit;
    CHECK(!sol_mir_validate(&compilation.ir, &counted, NULL));
    counted.blocks[while_loop->backedge].terminator.as.go_to.block
        = backedge_target;
    CHECK(sol_mir_validate(&compilation.ir, &counted, NULL));
    sol_mir_free(&counted);
    sol_mir_free(&repeated);

    SolMir transfers;
    sol_mir_init(&transfers);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "transfers"), &transfers,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(transfers.loop_count == 1);
    size_t breaks = 0;
    size_t continues = 0;
    for (size_t block = 0; block < transfers.block_count; ++block) {
        const SolMirTerminator *term = &transfers.blocks[block].terminator;
        if (term->kind != SOL_MIR_TERM_BREAK
            && term->kind != SOL_MIR_TERM_CONTINUE) continue;
        breaks += term->kind == SOL_MIR_TERM_BREAK;
        continues += term->kind == SOL_MIR_TERM_CONTINUE;
        bool exited_region = false;
        SolMirSlice instructions = transfers.blocks[block].instructions;
        for (size_t index = 0; index < instructions.count; ++index) {
            exited_region = exited_region
                || transfers.instructions[instructions.offset + index].kind
                    == SOL_MIR_INST_REGION_EXIT;
        }
        CHECK(exited_region);
        CHECK(term->as.transfer.edge.block == (term->kind == SOL_MIR_TERM_BREAK
            ? transfers.loops[0].exit : transfers.loops[0].header));
    }
    CHECK(breaks == 1 && continues == 1);
    CHECK(sol_mir_validate(&compilation.ir, &transfers, NULL));
    SolMirBlockId continue_block = SOL_MIR_NONE;
    for (size_t block = 0; block < transfers.block_count; ++block) {
        if (transfers.blocks[block].terminator.kind
            == SOL_MIR_TERM_CONTINUE) {
            continue_block = block;
            break;
        }
    }
    CHECK(continue_block != SOL_MIR_NONE);
    SolMirTerminator saved_continue
        = transfers.blocks[continue_block].terminator;
    transfers.blocks[continue_block].terminator = (SolMirTerminator){
        .kind = SOL_MIR_TERM_GOTO,
        .span = saved_continue.span,
        .as.go_to = saved_continue.as.transfer.edge,
    };
    CHECK(!sol_mir_validate(&compilation.ir, &transfers, NULL));
    transfers.blocks[continue_block].terminator = saved_continue;
    SolMirBlockId saved_header = transfers.loops[0].header;
    transfers.loops[0].header = transfers.loops[0].body;
    CHECK(!sol_mir_validate(&compilation.ir, &transfers, NULL));
    transfers.loops[0].header = saved_header;
    CHECK(sol_mir_validate(&compilation.ir, &transfers, NULL));
    sol_mir_free(&transfers);

    SolMir nested;
    sol_mir_init(&nested);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "nested"), &nested,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(nested.loop_count == 2);
    CHECK(nested.loops[0].parent == SOL_MIR_NONE);
    CHECK(nested.loops[1].parent == 0);
    size_t nested_breaks[2] = {0, 0};
    for (size_t block = 0; block < nested.block_count; ++block) {
        const SolMirTerminator *term = &nested.blocks[block].terminator;
        if (term->kind != SOL_MIR_TERM_BREAK) continue;
        CHECK(term->as.transfer.loop < 2);
        ++nested_breaks[term->as.transfer.loop];
        CHECK(term->as.transfer.edge.block
            == nested.loops[term->as.transfer.loop].exit);
    }
    CHECK(nested_breaks[0] == 1 && nested_breaks[1] == 1);
    CHECK(sol_mir_validate(&compilation.ir, &nested, NULL));
    nested.loops[1].parent = SOL_MIR_NONE;
    CHECK(!sol_mir_validate(&compilation.ir, &nested, NULL));
    nested.loops[1].parent = 0;
    SolMirBlockId inner_break_block = SOL_MIR_NONE;
    for (size_t block = 0; block < nested.block_count; ++block) {
        if (nested.blocks[block].terminator.kind == SOL_MIR_TERM_BREAK
            && nested.blocks[block].terminator.as.transfer.loop == 1) {
            inner_break_block = block;
            break;
        }
    }
    CHECK(inner_break_block != SOL_MIR_NONE);
    SolMirTerminator *inner_break
        = &nested.blocks[inner_break_block].terminator;
    inner_break->as.transfer.loop = 0;
    inner_break->as.transfer.edge.block = nested.loops[0].exit;
    CHECK(!sol_mir_validate(&compilation.ir, &nested, NULL));
    inner_break->as.transfer.loop = 1;
    inner_break->as.transfer.edge.block = nested.loops[1].exit;
    CHECK(sol_mir_validate(&compilation.ir, &nested, NULL));
    sol_mir_free(&nested);

    SolMir condition;
    sol_mir_init(&condition);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "condition_break"), &condition,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(condition.loop_count == 1);
    CHECK(condition.loops[0].body == SOL_MIR_NONE);
    CHECK(condition.loops[0].exit < condition.block_count);
    CHECK(condition.blocks[condition.loops[0].header].terminator.kind
        == SOL_MIR_TERM_BREAK);
    sol_mir_free(&condition);

    SolMir condition_continue;
    sol_mir_init(&condition_continue);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "condition_continue"),
        &condition_continue,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(condition_continue.loop_count == 1);
    bool saw_condition_continue = false;
    for (size_t block = 0; block < condition_continue.block_count; ++block) {
        const SolMirTerminator *term
            = &condition_continue.blocks[block].terminator;
        if (term->kind != SOL_MIR_TERM_CONTINUE) continue;
        saw_condition_continue = true;
        CHECK(term->as.transfer.edge.block
            == condition_continue.loops[0].header);
        bool exited_region = false;
        SolMirSlice instructions
            = condition_continue.blocks[block].instructions;
        for (size_t index = 0; index < instructions.count; ++index) {
            exited_region = exited_region
                || condition_continue.instructions[
                    instructions.offset + index].kind
                    == SOL_MIR_INST_REGION_EXIT;
        }
        CHECK(exited_region);
    }
    CHECK(saw_condition_continue);
    CHECK(sol_mir_validate(&compilation.ir, &condition_continue, NULL));
    sol_mir_free(&condition_continue);

    SolMir trailing;
    sol_mir_init(&trailing);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "trailing"), &trailing,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(trailing.loop_count == 0);
    CHECK(sol_mir_validate(&compilation.ir, &trailing, NULL));
    sol_mir_free(&trailing);

    SolMir skipped_break;
    sol_mir_init(&skipped_break);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "skipped_break"), &skipped_break,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(skipped_break.loop_count == 1);
    CHECK(skipped_break.loops[0].exit == SOL_MIR_NONE);
    for (size_t block = 0; block < skipped_break.block_count; ++block) {
        CHECK(skipped_break.blocks[block].terminator.kind != SOL_MIR_TERM_BREAK);
    }
    CHECK(sol_mir_validate(&compilation.ir, &skipped_break, NULL));
    sol_mir_free(&skipped_break);

    SolMir endless;
    sol_mir_init(&endless);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "endless"), &endless,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(endless.loop_count == 1);
    CHECK(endless.loops[0].exit == SOL_MIR_NONE);
    CHECK(endless.blocks[endless.loops[0].body].terminator.kind
        == SOL_MIR_TERM_GOTO);
    CHECK(endless.blocks[endless.loops[0].body].terminator.as.go_to.block
        == endless.loops[0].header);
    CHECK(sol_mir_validate(&compilation.ir, &endless, NULL));
    size_t loop_count = endless.loop_count;
    size_t loop_capacity = endless.loop_capacity;
    endless.loop_count = SIZE_MAX / sizeof(SolMirLoop) + 1;
    endless.loop_capacity = endless.loop_count;
    CHECK(!sol_mir_validate(&compilation.ir, &endless, NULL));
    endless.loop_count = 0;
    endless.loop_capacity = loop_capacity;
    CHECK(!sol_mir_validate(&compilation.ir, &endless, NULL));
    endless.loop_count = loop_count;
    CHECK(sol_mir_validate(&compilation.ir, &endless, NULL));
    sol_mir_free(&endless);
    free_compilation(&compilation);
}

static void test_bounded_value_construction(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module mir_construct\n"
        "record Empty {}\n"
        "record Box { value: Int64 }\n"
        "enum Choice { yes(value: Int64), no }\n"
        "type Meter = distinct Int64\n"
        "function empty() -> Empty { Empty {} }\n"
        "function box() -> Box { Box { value = 3 } }\n"
        "function box_after() -> Box { 1 Box { value = 3 } }\n"
        "function no() -> Choice { Choice.no }\n"
        "function yes() -> Choice { Choice.yes(4) }\n"
        "function option(flag: Bool) -> Option<Int64> { "
        "if flag { some(1) } else { none() } }\n"
        "function result(flag: Bool) -> Result<Int64, Choice> { "
        "if flag { ok(1) } else { err(Choice.no) } }\n"
        "function meter() -> Meter { Meter(4) }\n"));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));

    struct {
        const char *name;
        SolMirConstructKind kind;
        size_t operands;
    } cases[] = {
        {"empty", SOL_MIR_CONSTRUCT_RECORD, 0},
        {"box", SOL_MIR_CONSTRUCT_RECORD, 1},
        {"no", SOL_MIR_CONSTRUCT_ENUM, 0},
        {"yes", SOL_MIR_CONSTRUCT_ENUM, 1},
        {"meter", SOL_MIR_CONSTRUCT_DISTINCT, 1},
    };
    for (size_t case_index = 0;
        case_index < sizeof(cases) / sizeof(cases[0]); ++case_index) {
        SolMir mir;
        SolMir repeated;
        sol_mir_init(&mir);
        sol_mir_init(&repeated);
        SolIrCallableId id = callable(&compilation.ir, cases[case_index].name);
        CHECK(sol_mir_lower_callable(&compilation.ir, id, &mir,
            &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
        CHECK(sol_mir_lower_callable(&compilation.ir, id, &repeated,
            &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
        CHECK(sol_mir_validate(&compilation.ir, &mir, NULL));
        CHECK(mir_equal(&mir, &repeated));
        size_t constructs = 0;
        for (size_t instruction = 0; instruction < mir.instruction_count;
            ++instruction) {
            if (mir.instructions[instruction].kind != SOL_MIR_INST_CONSTRUCT) {
                continue;
            }
            ++constructs;
            CHECK(mir.instructions[instruction].as.construct.kind
                == cases[case_index].kind);
            CHECK(mir.instructions[instruction].as.construct.operands.count
                == cases[case_index].operands);
        }
        CHECK(constructs == 1);
        CHECK(mir.construct_operand_count == cases[case_index].operands);
        sol_mir_free(&mir);
        sol_mir_free(&repeated);
    }

    SolMir box_after;
    sol_mir_init(&box_after);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "box_after"), &box_after,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    SolMirInstructionId wrapper = SOL_MIR_NONE;
    SolMirValueId earlier = SOL_MIR_NONE;
    for (size_t instruction = 0; instruction < box_after.instruction_count;
        ++instruction) {
        if (box_after.instructions[instruction].kind
            == SOL_MIR_INST_CONST_INT64 && earlier == SOL_MIR_NONE) {
            earlier = box_after.instructions[instruction].result;
        }
        if (box_after.instructions[instruction].kind
            == SOL_MIR_INST_TEMPORARY_INIT) wrapper = instruction;
    }
    CHECK(wrapper != SOL_MIR_NONE && earlier != SOL_MIR_NONE);
    SolMirTemporaryId wrapped
        = box_after.instructions[wrapper].as.temporary_init.temporary;
    SolMirValueId staged_value
        = box_after.instructions[wrapper].as.temporary_init.value;
    box_after.instructions[wrapper].as.temporary_init.value = earlier;
    CHECK(!sol_mir_validate(&compilation.ir, &box_after, NULL));
    box_after.instructions[wrapper].as.temporary_init.value = staged_value;
    box_after.instructions[wrapper].as.temporary_init.temporary = SOL_MIR_NONE;
    CHECK(!sol_mir_validate(&compilation.ir, &box_after, NULL));
    box_after.instructions[wrapper].as.temporary_init.temporary = wrapped;
    SolSpan wrapper_span = box_after.instructions[wrapper].span;
    ++box_after.instructions[wrapper].span.start;
    CHECK(!sol_mir_validate(&compilation.ir, &box_after, NULL));
    box_after.instructions[wrapper].span = wrapper_span;
    CHECK(sol_mir_validate(&compilation.ir, &box_after, NULL));
    sol_mir_free(&box_after);

    SolMir option;
    sol_mir_init(&option);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "option"), &option,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    bool saw_none = false;
    bool saw_some = false;
    for (size_t instruction = 0; instruction < option.instruction_count;
        ++instruction) {
        if (option.instructions[instruction].kind != SOL_MIR_INST_CONSTRUCT) {
            continue;
        }
        saw_none = saw_none || option.instructions[instruction].as.construct.kind
            == SOL_MIR_CONSTRUCT_OPTION_NONE;
        saw_some = saw_some || option.instructions[instruction].as.construct.kind
            == SOL_MIR_CONSTRUCT_OPTION_SOME;
    }
    CHECK(saw_none && saw_some);
    CHECK(sol_mir_validate(&compilation.ir, &option, NULL));
    sol_mir_free(&option);

    SolMir result;
    sol_mir_init(&result);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "result"), &result,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    bool saw_ok = false;
    bool saw_err = false;
    bool saw_nested_enum = false;
    SolMirInstructionId first_construct = SOL_MIR_NONE;
    for (size_t instruction = 0; instruction < result.instruction_count;
        ++instruction) {
        if (result.instructions[instruction].kind != SOL_MIR_INST_CONSTRUCT) {
            continue;
        }
        if (first_construct == SOL_MIR_NONE) first_construct = instruction;
        SolMirConstructKind kind
            = result.instructions[instruction].as.construct.kind;
        saw_ok = saw_ok || kind == SOL_MIR_CONSTRUCT_RESULT_OK;
        saw_err = saw_err || kind == SOL_MIR_CONSTRUCT_RESULT_ERR;
        saw_nested_enum = saw_nested_enum || kind == SOL_MIR_CONSTRUCT_ENUM;
    }
    CHECK(saw_ok && saw_err && saw_nested_enum);
    CHECK(first_construct != SOL_MIR_NONE);
    SolMirConstructKind construct_kind
        = result.instructions[first_construct].as.construct.kind;
    result.instructions[first_construct].as.construct.kind
        = SOL_MIR_CONSTRUCT_DISTINCT;
    CHECK(!sol_mir_validate(&compilation.ir, &result, NULL));
    result.instructions[first_construct].as.construct.kind = construct_kind;
    SolMirSlice operand_slice
        = result.instructions[first_construct].as.construct.operands;
    size_t operand_count = operand_slice.count;
    result.instructions[first_construct].as.construct.operands.count
        = operand_count == 0 ? 1 : 0;
    CHECK(!sol_mir_validate(&compilation.ir, &result, NULL));
    result.instructions[first_construct].as.construct.operands.count
        = operand_count;
    size_t construct_count = result.construct_operand_count;
    size_t construct_capacity = result.construct_operand_capacity;
    result.construct_operand_count
        = SIZE_MAX / sizeof(SolMirConstructOperand) + 1;
    result.construct_operand_capacity = result.construct_operand_count;
    CHECK(!sol_mir_validate(&compilation.ir, &result, NULL));
    result.construct_operand_count = construct_count;
    result.construct_operand_capacity = construct_capacity;
    CHECK(sol_mir_validate(&compilation.ir, &result, NULL));
    sol_mir_free(&result);
    free_compilation(&compilation);

    Compilation deferred;
    CHECK(compile(&deferred,
        "module mir_construct_deferred\n"
        "record Pair { left: Int64, right: Int64 }\n"
        "record Box { value: Int64 }\n"
        "type Positive = refined Int64 where self > 0\n"
        "function pair() -> Pair { Pair { left = 1, right = 2 } }\n"
        "function tuple() -> (Int64, Bool) { (1, true) }\n"
        "function block_box() -> Box { Box { value = { 1 } } }\n"
        "function positive() -> Positive { Positive(1) }\n"));
    const char *aggregate_names[] = {
        "pair", "tuple", "block_box",
    };
    for (size_t index = 0;
        index < sizeof(aggregate_names) / sizeof(aggregate_names[0]); ++index) {
        SolMir mir;
        sol_mir_init(&mir);
        CHECK(sol_mir_lower_callable(&deferred.ir,
            callable(&deferred.ir, aggregate_names[index]), &mir,
            &deferred.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
        CHECK(sol_mir_validate(&deferred.ir, &mir, NULL));
        sol_mir_free(&mir);
    }
    SolMir refined;
    sol_mir_init(&refined);
    CHECK(sol_mir_lower_callable(&deferred.ir,
        callable(&deferred.ir, "positive"), &refined,
        &deferred.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(sol_mir_validate(&deferred.ir, &refined, NULL));
    SolMirBlockId check = SOL_MIR_NONE;
    for (size_t block = 0; block < refined.block_count; ++block) {
        if (refined.blocks[block].terminator.kind
            == SOL_MIR_TERM_CHECK_REFINED) check = block;
    }
    CHECK(check != SOL_MIR_NONE);
    SolMirTerminator *refinement = &refined.blocks[check].terminator;
    CHECK(refinement->as.check_refined.normal_edge.arguments.count == 1);
    CHECK(refinement->as.check_refined.failure_edge.arguments.count == 0);
    CHECK(refined.blocks[refinement->as.check_refined.failure_edge.block]
        .terminator.kind == SOL_MIR_TERM_RESUME_FAILURE);
    SolObligationId obligation = refinement->as.check_refined.obligation;
    refinement->as.check_refined.obligation = SOL_IR_NONE;
    CHECK(!sol_mir_validate(&deferred.ir, &refined, NULL));
    refinement->as.check_refined.obligation = obligation;
    SolMirTemporaryId representation
        = refinement->as.check_refined.representation;
    refinement->as.check_refined.representation = SOL_MIR_NONE;
    CHECK(!sol_mir_validate(&deferred.ir, &refined, NULL));
    refinement->as.check_refined.representation = representation;
    SolMirBlockId normal = refinement->as.check_refined.normal_edge.block;
    refinement->as.check_refined.normal_edge.block = SOL_MIR_NONE;
    CHECK(!sol_mir_validate(&deferred.ir, &refined, NULL));
    refinement->as.check_refined.normal_edge.block = normal;
    CHECK(sol_mir_validate(&deferred.ir, &refined, NULL));
    sol_mir_free(&refined);
    free_compilation(&deferred);
}

static void test_refined_failure_cleanup(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module mir_refined_cleanup\n"
        "record Box<T> { value: T }\n"
        "type Positive = refined Int64 where self > 0\n"
        "function make(value: Int64) -> Box<Int64> "
        "{ Box<Int64> { value } }\n"
        "function consume(box: Box<Int64>, value: Positive) -> () {}\n"
        "function checked() -> () { consume(make(1), Positive(2)) }\n"));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolMir mir;
    sol_mir_init(&mir);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "checked"), &mir,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(sol_mir_validate(&compilation.ir, &mir, NULL));
    SolMirBlockId check = SOL_MIR_NONE;
    for (size_t block = 0; block < mir.block_count; ++block) {
        if (mir.blocks[block].terminator.kind
            == SOL_MIR_TERM_CHECK_REFINED) check = block;
    }
    CHECK(check != SOL_MIR_NONE);
    SolMirBlockId failure
        = mir.blocks[check].terminator.as.check_refined.failure_edge.block;
    SolMirTemporaryId representation
        = mir.blocks[check].terminator.as.check_refined.representation;
    SolMirTemporaryId outer = SOL_MIR_NONE;
    for (size_t block = 0; block < mir.block_count; ++block) {
        const SolMirTerminator *term = &mir.blocks[block].terminator;
        if (term->kind == SOL_MIR_TERM_INVOKE
            && term->as.invoke.arguments.count == 2) {
            outer = mir.call_arguments[term->as.invoke.arguments.offset]
                .temporary;
        }
    }
    CHECK(outer != SOL_MIR_NONE);
    size_t drop_count = 0;
    SolMirTemporaryId dropped = SOL_MIR_NONE;
    SolMirSlice instructions = mir.blocks[failure].instructions;
    for (size_t index = 0; index < instructions.count; ++index) {
        const SolMirInstruction *instruction
            = &mir.instructions[instructions.offset + index];
        if (instruction->kind == SOL_MIR_INST_TEMPORARY_DROP) {
            ++drop_count;
            dropped = instruction->as.temporary_drop.temporary;
        }
    }
    CHECK(drop_count == 1 && dropped == outer && dropped != representation);
    CHECK(mir.blocks[failure].terminator.kind
        == SOL_MIR_TERM_RESUME_FAILURE);
    sol_mir_free(&mir);
    free_compilation(&compilation);
}

static void test_recursive_match_lowering(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module mir_recursive_match\n"
        "record Packet { data: (Int64, Bool) }\n"
        "function select(value: Packet, gate: Bool) -> Int64 { "
        "return match value { "
        "Packet { data = (number, true) } if gate => number "
        "_ => 0 } }\n"));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolMir mir;
    sol_mir_init(&mir);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "select"), &mir,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(sol_mir_validate(&compilation.ir, &mir, NULL));
    SolMirInstructionId pattern_value = SOL_MIR_NONE;
    bool saw_binding_drop = false;
    bool saw_match_failure = false;
    for (size_t instruction = 0; instruction < mir.instruction_count;
        ++instruction) {
        if (mir.instructions[instruction].kind == SOL_MIR_INST_PATTERN_VALUE) {
            pattern_value = instruction;
        }
        saw_binding_drop = saw_binding_drop
            || mir.instructions[instruction].kind
                == SOL_MIR_INST_DROP_IF_INITIALIZED;
    }
    for (size_t block = 0; block < mir.block_count; ++block) {
        saw_match_failure = saw_match_failure
            || mir.blocks[block].terminator.kind
                == SOL_MIR_TERM_MATCH_FAILURE;
    }
    CHECK(pattern_value != SOL_MIR_NONE);
    CHECK(saw_binding_drop && saw_match_failure);
    SolIrArmId arm = mir.instructions[pattern_value].as.pattern.arm;
    mir.instructions[pattern_value].as.pattern.arm = SOL_IR_NONE;
    CHECK(!sol_mir_validate(&compilation.ir, &mir, NULL));
    mir.instructions[pattern_value].as.pattern.arm = arm;
    CHECK(sol_mir_validate(&compilation.ir, &mir, NULL));
    sol_mir_free(&mir);
    free_compilation(&compilation);
}

static void test_propagation_lowering(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module mir_propagation\n"
        "record Box<T> { value: T }\n"
        "function make(value: Int64) -> Box<Int64> "
        "{ Box<Int64> { value } }\n"
        "function consume(box: Box<Int64>, value: Int64) -> () {}\n"
        "function staged(value: Option<Int64>) -> Option<()> { "
        "consume(make(1), value?) return some(()) }\n"
        "function result(value: Result<Int64, Text>) -> Result<Bool, Text> { "
        "return ok(value? > 0) }\n"));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    const char *names[] = {"staged", "result"};
    for (size_t name = 0; name < 2; ++name) {
        SolMir mir;
        sol_mir_init(&mir);
        SolMirLowerOutcome outcome = sol_mir_lower_callable(&compilation.ir,
            callable(&compilation.ir, names[name]), &mir,
            &compilation.diagnostics);
        CHECK(outcome == SOL_MIR_LOWER_SUCCEEDED);
        if (outcome != SOL_MIR_LOWER_SUCCEEDED) {
            sol_mir_free(&mir);
            continue;
        }
        CHECK(sol_mir_validate(&compilation.ir, &mir, NULL));
        SolMirBlockId propagation = SOL_MIR_NONE;
        for (size_t block = 0; block < mir.block_count; ++block) {
            if (mir.blocks[block].terminator.kind
                == SOL_MIR_TERM_PROPAGATE) propagation = block;
        }
        CHECK(propagation != SOL_MIR_NONE);
        SolMirTerminator *term = &mir.blocks[propagation].terminator;
        CHECK(term->as.propagate.value_edge.arguments.count == 1);
        CHECK(term->as.propagate.residual_edge.arguments.count == 1);
        CHECK(mir.values[term->as.propagate.residual_result].type
            == compilation.ir.callables[mir.callable].result);
        CHECK(mir.blocks[term->as.propagate.residual_edge.block]
            .terminator.kind == SOL_MIR_TERM_RETURN);
        if (name == 0) {
            bool saw_outer_drop = false;
            SolMirSlice instructions
                = mir.blocks[term->as.propagate.residual_edge.block].instructions;
            for (size_t index = 0; index < instructions.count; ++index) {
                saw_outer_drop = saw_outer_drop
                    || mir.instructions[instructions.offset + index].kind
                        == SOL_MIR_INST_TEMPORARY_DROP;
            }
            CHECK(saw_outer_drop);
        }
        SolMirTemporaryId operand = term->as.propagate.operand;
        term->as.propagate.operand = SOL_MIR_NONE;
        CHECK(!sol_mir_validate(&compilation.ir, &mir, NULL));
        term->as.propagate.operand = operand;
        SolMirValueId residual_argument = mir.edge_values[
            term->as.propagate.residual_edge.arguments.offset];
        mir.edge_values[term->as.propagate.residual_edge.arguments.offset]
            = term->as.propagate.value_result;
        CHECK(!sol_mir_validate(&compilation.ir, &mir, NULL));
        mir.edge_values[term->as.propagate.residual_edge.arguments.offset]
            = residual_argument;
        CHECK(sol_mir_validate(&compilation.ir, &mir, NULL));
        sol_mir_free(&mir);
    }
    free_compilation(&compilation);
}

static void test_projected_place_lowering(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module mir_projected_places\n"
        "record Pair { left: Int64, right: Int64 }\n"
        "record Nested { pair: (Pair, Int64) }\n"
        "record Box<T> { value: T }\n"
        "record Holder { left: Box<Int64>, right: Box<Int64> }\n"
        "function set(value: inout Int64) -> () { value = 7 }\n"
        "function inspect(value: borrow Int64) -> Int64 { return value }\n"
        "function set_both(left: inout Int64, right: inout Int64) -> () { "
        "left = 8 right = 9 }\n"
        "function read(value: Nested) -> Int64 { "
        "return value.pair.0.left + value.pair.1 }\n"
        "function replace() -> Int64 { var pair = Pair { left = 1, right = 2 } "
        "pair.left = 3 return pair.left }\n"
        "function shared() -> Int64 { let pair = Pair { left = 1, right = 2 } "
        "return inspect(pair.left) }\n"
        "function writeback() -> Int64 { "
        "var pair = Pair { left = 1, right = 2 } "
        "set_both(pair.left, pair.right) return pair.left + pair.right }\n"
        "function moved(value: Holder) -> Box<Int64> { return value.left }\n"));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    const char *names[] = {"read", "replace", "shared", "writeback"};
    for (size_t name = 0; name < 4; ++name) {
        SolMir mir;
        sol_mir_init(&mir);
        CHECK(sol_mir_lower_callable(&compilation.ir,
            callable(&compilation.ir, names[name]), &mir,
            &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
        CHECK(sol_mir_validate(&compilation.ir, &mir, NULL));
        bool projected_load = false;
        bool projected_drop = false;
        SolMirInstructionId load = SOL_MIR_NONE;
        SolMirInstructionId projected_store = SOL_MIR_NONE;
        SolMirValueId alternate = SOL_MIR_NONE;
        for (size_t instruction = 0; instruction < mir.instruction_count;
            ++instruction) {
            const SolMirInstruction *item = &mir.instructions[instruction];
            if (item->kind == SOL_MIR_INST_LOAD_COPY
                && item->as.place.source_place < compilation.ir.place_count
                && compilation.ir.places[item->as.place.source_place]
                    .projections.count != 0) {
                projected_load = true;
                load = instruction;
            }
            projected_drop = projected_drop
                || item->kind == SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED;
            if (item->kind == SOL_MIR_INST_STORE
                && item->as.store.place.source_place != SOL_IR_NONE) {
                projected_store = instruction;
            }
            if (item->kind == SOL_MIR_INST_CONST_INT64
                && alternate == SOL_MIR_NONE) alternate = item->result;
        }
        if (name != 2) CHECK(projected_load);
        if (name == 1) CHECK(projected_drop);
        if (name == 1 && projected_store != SOL_MIR_NONE
            && alternate != SOL_MIR_NONE) {
            SolMirValueId stored
                = mir.instructions[projected_store].as.store.value;
            CHECK(alternate != stored);
            mir.instructions[projected_store].as.store.value = alternate;
            CHECK(!sol_mir_validate(&compilation.ir, &mir, NULL));
            mir.instructions[projected_store].as.store.value = stored;
            CHECK(sol_mir_validate(&compilation.ir, &mir, NULL));
        }
        bool saw_projected_shared = false;
        SolMirTerminator *exclusive = NULL;
        for (size_t block = 0; block < mir.block_count; ++block) {
            SolMirTerminator *term = &mir.blocks[block].terminator;
            if (term->kind != SOL_MIR_TERM_INVOKE) continue;
            SolMirSlice arguments = term->as.invoke.arguments;
            if (arguments.count == 2) exclusive = term;
            for (size_t index = 0; index < arguments.count; ++index) {
                const SolMirCallArgument *argument
                    = &mir.call_arguments[arguments.offset + index];
                saw_projected_shared = saw_projected_shared
                    || (argument->access == SOL_ACCESS_SHARED
                        && compilation.ir.places[argument->place]
                            .projections.count != 0);
            }
        }
        if (name == 2) CHECK(saw_projected_shared);
        if (name == 3) {
            CHECK(exclusive != NULL);
            SolMirSlice arguments = exclusive->as.invoke.arguments;
            SolIrPlaceId second = mir.call_arguments[arguments.offset + 1].place;
            CHECK(mir.call_arguments[arguments.offset].place != second);
            mir.call_arguments[arguments.offset + 1].place
                = mir.call_arguments[arguments.offset].place;
            CHECK(!sol_mir_validate(&compilation.ir, &mir, NULL));
            mir.call_arguments[arguments.offset + 1].place = second;
            CHECK(sol_mir_validate(&compilation.ir, &mir, NULL));
        }
        if (load != SOL_MIR_NONE) {
            SolIrPlaceId place = mir.instructions[load].as.place.source_place;
            mir.instructions[load].as.place.source_place = SOL_IR_NONE;
            CHECK(!sol_mir_validate(&compilation.ir, &mir, NULL));
            mir.instructions[load].as.place.source_place = place;
            CHECK(sol_mir_validate(&compilation.ir, &mir, NULL));
        }
        sol_mir_free(&mir);
    }
    SolMir moved;
    sol_mir_init(&moved);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "moved"), &moved,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(sol_mir_validate(&compilation.ir, &moved, NULL));
    sol_mir_free(&moved);
    free_compilation(&compilation);
}

static void test_partial_move_path_state(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module mir_partial_moves\n"
        "record Box<T> { value: T }\n"
        "record Pair { left: Box<Int64>, right: Box<Int64> }\n"
        "function inspect(value: borrow Box<Int64>) -> Int64 { "
        "return value.value }\n"
        "function sibling(value: Pair) -> Box<Int64> { "
        "let moved = value.left return value.right }\n"
        "function reinitialize(value: Pair) -> Pair { var work = value "
        "let moved = work.left work.left = moved return work }\n"
        "function branch(flag: Bool, value: Pair) -> Box<Int64> { "
        "if flag { let moved = value.left } else { () } return value.right }\n"
        "function borrow_sibling(value: Pair) -> Int64 { "
        "let moved = value.left return inspect(value.right) }\n"));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    const char *names[] = {
        "sibling", "reinitialize", "branch", "borrow_sibling",
    };
    for (size_t name = 0; name < 4; ++name) {
        SolMir mir;
        SolMir repeated;
        sol_mir_init(&mir);
        sol_mir_init(&repeated);
        SolIrCallableId id = callable(&compilation.ir, names[name]);
        CHECK(sol_mir_lower_callable(&compilation.ir, id, &mir,
            &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
        CHECK(sol_mir_lower_callable(&compilation.ir, id, &repeated,
            &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
        CHECK(sol_mir_validate(&compilation.ir, &mir, NULL));
        CHECK(mir_equal(&mir, &repeated));
        bool projected_move = false;
        bool whole_cleanup = false;
        bool projected_restore = false;
        for (size_t instruction = 0; instruction < mir.instruction_count;
            ++instruction) {
            const SolMirInstruction *item = &mir.instructions[instruction];
            projected_move = projected_move
                || (item->kind == SOL_MIR_INST_LOAD_MOVE
                    && compilation.ir.places[item->as.place.source_place]
                        .projections.count != 0);
            whole_cleanup = whole_cleanup
                || item->kind == SOL_MIR_INST_DROP_IF_INITIALIZED;
            projected_restore = projected_restore
                || item->kind == SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED;
        }
        CHECK(projected_move && whole_cleanup);
        if (name == 1) CHECK(projected_restore);
        if (name == 2) {
            SolMirBlockId move_block = SOL_MIR_NONE;
            for (size_t block = 0; block < mir.block_count; ++block) {
                SolMirSlice instructions = mir.blocks[block].instructions;
                for (size_t index = 0; index < instructions.count; ++index) {
                    const SolMirInstruction *item
                        = &mir.instructions[instructions.offset + index];
                    if (item->kind == SOL_MIR_INST_LOAD_MOVE
                        && compilation.ir.places[item->as.place.source_place]
                            .projections.count != 0
                        && mir.blocks[block].terminator.kind
                            == SOL_MIR_TERM_GOTO) move_block = block;
                }
            }
            CHECK(move_block != SOL_MIR_NONE);
            SolMirBlockId target
                = mir.blocks[move_block].terminator.as.go_to.block;
            mir.blocks[move_block].terminator.as.go_to.block = move_block;
            CHECK(!sol_mir_validate(&compilation.ir, &mir, NULL));
            mir.blocks[move_block].terminator.as.go_to.block = target;
            CHECK(sol_mir_validate(&compilation.ir, &mir, NULL));
        }
        sol_mir_free(&mir);
        sol_mir_free(&repeated);
    }
    free_compilation(&compilation);
}

static void test_callback_invoke_lowering(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module mir_callbacks\n"
        "function apply(callback: function(Int64) -> Int64 effects { pure }, "
        "value: Int64) -> Int64 effects { pure } { return callback(value) }\n"
        "function borrow_apply(callback: function(borrow Int64) -> Int64 "
        "effects { pure }, value: Int64) -> Int64 effects { pure } { "
        "return callback(value) }\n"
        "function crash() -> Int64 effects { panic } { panic \"boom\" }\n"
        "function apply_failure(callback: function(Int64) -> Int64 "
        "effects { pure }) -> Int64 effects { panic } { "
        "return callback(crash()) }\n"));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    const char *names[] = {"apply", "borrow_apply", "apply_failure"};
    for (size_t name = 0; name < 3; ++name) {
        SolMir mir;
        sol_mir_init(&mir);
        CHECK(sol_mir_lower_callable(&compilation.ir,
            callable(&compilation.ir, names[name]), &mir,
            &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
        CHECK(sol_mir_validate(&compilation.ir, &mir, NULL));
        SolMirBlockId invoke = SOL_MIR_NONE;
        for (size_t block = 0; block < mir.block_count; ++block) {
            if (mir.blocks[block].terminator.kind == SOL_MIR_TERM_INVOKE
                && mir.blocks[block].terminator.as.invoke.kind
                    == SOL_IR_CALL_CALLBACK) invoke = block;
        }
        CHECK(invoke != SOL_MIR_NONE);
        SolMirTerminator *term = &mir.blocks[invoke].terminator;
        CHECK(term->as.invoke.callable == SOL_IR_NONE);
        CHECK(term->as.invoke.callee < mir.temporary_count);
        if (name == 1) {
            CHECK(mir.call_arguments[term->as.invoke.arguments.offset].access
                == SOL_ACCESS_SHARED);
        }
        if (name == 2) {
            bool dropped_callee = false;
            for (size_t instruction = 0; instruction < mir.instruction_count;
                ++instruction) {
                dropped_callee = dropped_callee
                    || (mir.instructions[instruction].kind
                            == SOL_MIR_INST_TEMPORARY_DROP
                        && mir.instructions[instruction].as.temporary_drop
                            .temporary == term->as.invoke.callee);
            }
            CHECK(dropped_callee);
        }
        SolMirTemporaryId callee = term->as.invoke.callee;
        SolMirValueId parameter = mir.parameter_values[
            mir.blocks[term->as.invoke.normal_edge.block].parameters.offset];
        SolIrExpressionId parameter_source
            = mir.values[parameter].source_expression;
        mir.values[parameter].source_expression = SOL_IR_NONE;
        CHECK(!sol_mir_validate(&compilation.ir, &mir, NULL));
        mir.values[parameter].source_expression = parameter_source;
        term->as.invoke.callee = SOL_MIR_NONE;
        CHECK(!sol_mir_validate(&compilation.ir, &mir, NULL));
        term->as.invoke.callee = callee;
        CHECK(sol_mir_validate(&compilation.ir, &mir, NULL));
        if (name == 2) {
            for (size_t block = 0; block < mir.block_count; ++block) {
                SolMirTerminator *direct = &mir.blocks[block].terminator;
                if (direct->kind != SOL_MIR_TERM_INVOKE
                    || direct->as.invoke.kind != SOL_IR_CALL_FUNCTION) {
                    continue;
                }
                direct->as.invoke.callee = 0;
                CHECK(!sol_mir_validate(&compilation.ir, &mir, NULL));
                direct->as.invoke.callee = SOL_MIR_NONE;
                CHECK(sol_mir_validate(&compilation.ir, &mir, NULL));
            }
        }
        sol_mir_free(&mir);
    }
    free_compilation(&compilation);
}

static void test_owned_temporary_cleanup(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module mir_temporaries\n"
        "record Box<T> { value: T }\n"
        "record Pair { left: Box<Int64>, right: Box<Int64> }\n"
        "function make(value: Int64) -> Box<Int64> { Box<Int64> { value } }\n"
        "function consume(first: Box<Int64>, second: Int64) -> () {}\n"
        "function consume_three(first: Box<Int64>, second: Box<Int64>, "
        "third: Int64) -> () {}\n"
        "function crash() -> Int64 effects { panic } { panic \"crash\" }\n"
        "function staged_failure() -> () effects { panic } { "
        "consume(make(1), crash()) }\n"
        "function staged_panic() -> () effects { panic } { "
        "consume(make(1), { panic \"later\" }) }\n"
        "function staged_order() -> () effects { panic } { "
        "consume_three(make(1), make(2), crash()) }\n"
        "function alias_source() -> Int64 { 1 { 2 } }\n"
        "function preserve_loop() -> () { "
        "consume(make(1), { loop { break } 2 }) }\n"
        "function pair() -> Pair { Pair { left = make(1), right = make(2) } }\n"
        "function inline_pair() -> Pair { Pair { "
        "left = Box<Int64> { value = 1 }, "
        "right = Box<Int64> { value = 2 } } }\n"
        "function tuple() -> (Box<Int64>, Box<Int64>) { "
        "(make(1), make(2)) }\n"));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));

    SolMir alias;
    sol_mir_init(&alias);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "alias_source"), &alias,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    SolMirValueId first_integer = SOL_MIR_NONE;
    SolMirInstructionId inner_alias = SOL_MIR_NONE;
    for (size_t instruction = 0; instruction < alias.instruction_count;
        ++instruction) {
        if (alias.instructions[instruction].kind == SOL_MIR_INST_CONST_INT64
            && first_integer == SOL_MIR_NONE) {
            first_integer = alias.instructions[instruction].result;
        }
        if (alias.instructions[instruction].kind
                == SOL_MIR_INST_EXPRESSION_RESULT
            && inner_alias == SOL_MIR_NONE) inner_alias = instruction;
    }
    CHECK(first_integer != SOL_MIR_NONE && inner_alias != SOL_MIR_NONE);
    SolMirValueId alias_operand = alias.instructions[inner_alias].as.operand;
    alias.instructions[inner_alias].as.operand = first_integer;
    CHECK(!sol_mir_validate(&compilation.ir, &alias, NULL));
    alias.instructions[inner_alias].as.operand = alias_operand;
    CHECK(sol_mir_validate(&compilation.ir, &alias, NULL));
    sol_mir_free(&alias);

    SolMir failure;
    sol_mir_init(&failure);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "staged_failure"), &failure,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(failure.temporary_count >= 2);
    bool saw_outer_drop = false;
    for (size_t block = 0; block < failure.block_count; ++block) {
        const SolMirTerminator *term = &failure.blocks[block].terminator;
        if (term->kind != SOL_MIR_TERM_INVOKE
            || strcmp(compilation.ir.callables[term->as.invoke.callable].name,
                "crash") != 0) continue;
        SolMirBlockId cleanup = term->as.invoke.failure_edge.block;
        SolMirSlice instructions = failure.blocks[cleanup].instructions;
        CHECK(instructions.count != 0);
        saw_outer_drop = failure.instructions[instructions.offset].kind
            == SOL_MIR_INST_TEMPORARY_DROP;
    }
    CHECK(saw_outer_drop);
    CHECK(sol_mir_validate(&compilation.ir, &failure, NULL));
    SolMirInstructionId drop = SOL_MIR_NONE;
    for (size_t instruction = 0; instruction < failure.instruction_count;
        ++instruction) {
        if (failure.instructions[instruction].kind
            == SOL_MIR_INST_TEMPORARY_DROP) {
            drop = instruction;
            break;
        }
    }
    CHECK(drop != SOL_MIR_NONE);
    SolMirTemporaryId dropped
        = failure.instructions[drop].as.temporary_drop.temporary;
    failure.instructions[drop].as.temporary_drop.temporary = SOL_MIR_NONE;
    CHECK(!sol_mir_validate(&compilation.ir, &failure, NULL));
    failure.instructions[drop].as.temporary_drop.temporary = dropped;
    CHECK(sol_mir_validate(&compilation.ir, &failure, NULL));
    size_t temporary_count = failure.temporary_count;
    size_t temporary_capacity = failure.temporary_capacity;
    failure.temporary_count = SIZE_MAX / sizeof(SolMirTemporary) + 1;
    failure.temporary_capacity = failure.temporary_count;
    CHECK(!sol_mir_validate(&compilation.ir, &failure, NULL));
    failure.temporary_count = temporary_count;
    failure.temporary_capacity = temporary_capacity;
    SolMirTemporary *temporaries = failure.temporaries;
    failure.temporaries = NULL;
    CHECK(!sol_mir_validate(&compilation.ir, &failure, NULL));
    failure.temporaries = temporaries;
    CHECK(sol_mir_validate(&compilation.ir, &failure, NULL));
    sol_mir_free(&failure);

    SolMir order;
    sol_mir_init(&order);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "staged_order"), &order,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    SolMirTemporaryId ordered_drops[2] = {SOL_MIR_NONE, SOL_MIR_NONE};
    SolMirInstructionId ordered_drop_instructions[2]
        = {SOL_MIR_NONE, SOL_MIR_NONE};
    size_t ordered_drop_count = 0;
    for (size_t block = 0; block < order.block_count; ++block) {
        const SolMirTerminator *term = &order.blocks[block].terminator;
        if (term->kind != SOL_MIR_TERM_INVOKE
            || strcmp(compilation.ir.callables[term->as.invoke.callable].name,
                "crash") != 0) continue;
        SolMirSlice instructions
            = order.blocks[term->as.invoke.failure_edge.block].instructions;
        for (size_t index = 0; index < instructions.count
            && ordered_drop_count < 2; ++index) {
            const SolMirInstruction *instruction
                = &order.instructions[instructions.offset + index];
            if (instruction->kind == SOL_MIR_INST_TEMPORARY_DROP) {
                CHECK(instruction->as.temporary_drop.preserve_depth == 0);
                ordered_drop_instructions[ordered_drop_count]
                    = instructions.offset + index;
                ordered_drops[ordered_drop_count++]
                    = instruction->as.temporary_drop.temporary;
            }
        }
    }
    CHECK(ordered_drop_count == 2);
    CHECK(ordered_drops[0] < ordered_drops[1]);
    CHECK(sol_mir_validate(&compilation.ir, &order, NULL));
    order.instructions[ordered_drop_instructions[0]]
        .as.temporary_drop.temporary = ordered_drops[1];
    order.instructions[ordered_drop_instructions[0]]
        .as.temporary_drop.preserve_depth = 1;
    order.instructions[ordered_drop_instructions[1]]
        .as.temporary_drop.temporary = ordered_drops[0];
    CHECK(!sol_mir_validate(&compilation.ir, &order, NULL));
    order.instructions[ordered_drop_instructions[0]]
        .as.temporary_drop.temporary = ordered_drops[0];
    order.instructions[ordered_drop_instructions[0]]
        .as.temporary_drop.preserve_depth = 0;
    order.instructions[ordered_drop_instructions[1]]
        .as.temporary_drop.temporary = ordered_drops[1];
    CHECK(sol_mir_validate(&compilation.ir, &order, NULL));
    sol_mir_free(&order);

    SolMir panic;
    sol_mir_init(&panic);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "staged_panic"), &panic,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    bool dropped_before_panic = false;
    for (size_t block = 0; block < panic.block_count; ++block) {
        if (panic.blocks[block].terminator.kind != SOL_MIR_TERM_PANIC) continue;
        SolMirSlice instructions = panic.blocks[block].instructions;
        for (size_t index = 0; index < instructions.count; ++index) {
            dropped_before_panic = dropped_before_panic
                || panic.instructions[instructions.offset + index].kind
                    == SOL_MIR_INST_TEMPORARY_DROP;
        }
    }
    CHECK(dropped_before_panic);
    CHECK(sol_mir_validate(&compilation.ir, &panic, NULL));
    sol_mir_free(&panic);

    SolMir loop;
    sol_mir_init(&loop);
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "preserve_loop"), &loop,
        &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
    CHECK(loop.loop_count == 1);
    for (size_t block = 0; block < loop.block_count; ++block) {
        if (loop.blocks[block].terminator.kind != SOL_MIR_TERM_BREAK) continue;
        SolMirSlice instructions = loop.blocks[block].instructions;
        for (size_t index = 0; index < instructions.count; ++index) {
            CHECK(loop.instructions[instructions.offset + index].kind
                != SOL_MIR_INST_TEMPORARY_DROP);
        }
    }
    CHECK(sol_mir_validate(&compilation.ir, &loop, NULL));
    sol_mir_free(&loop);

    const char *aggregates[] = {"pair", "inline_pair", "tuple"};
    for (size_t index = 0;
        index < sizeof(aggregates) / sizeof(aggregates[0]); ++index) {
        SolMir mir;
        SolMir repeated;
        sol_mir_init(&mir);
        sol_mir_init(&repeated);
        SolIrCallableId id = callable(&compilation.ir, aggregates[index]);
        CHECK(sol_mir_lower_callable(&compilation.ir, id, &mir,
            &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
        CHECK(sol_mir_lower_callable(&compilation.ir, id, &repeated,
            &compilation.diagnostics) == SOL_MIR_LOWER_SUCCEEDED);
        CHECK(mir_equal(&mir, &repeated));
        bool consumed_two = false;
        for (size_t instruction = 0; instruction < mir.instruction_count;
            ++instruction) {
            consumed_two = consumed_two
                || (mir.instructions[instruction].kind == SOL_MIR_INST_CONSTRUCT
                    && mir.instructions[instruction].as.construct.operands.count
                        == 2);
        }
        CHECK(consumed_two);
        CHECK(sol_mir_validate(&compilation.ir, &mir, NULL));
        if (strcmp(aggregates[index], "inline_pair") == 0) {
            SolMirInstructionId initializers[4]
                = {SOL_MIR_NONE, SOL_MIR_NONE, SOL_MIR_NONE, SOL_MIR_NONE};
            size_t initializer_count = 0;
            for (size_t instruction = 0; instruction < mir.instruction_count
                && initializer_count < 4; ++instruction) {
                if (mir.instructions[instruction].kind
                    == SOL_MIR_INST_TEMPORARY_INIT) {
                    initializers[initializer_count++] = instruction;
                }
            }
            CHECK(initializer_count == 4);
            size_t duplicate = 1;
            SolIrTypeId first_type = mir.temporaries[mir.instructions[
                initializers[0]].as.temporary_init.temporary].type;
            while (duplicate < initializer_count
                && mir.temporaries[mir.instructions[initializers[duplicate]]
                    .as.temporary_init.temporary].type != first_type) {
                ++duplicate;
            }
            CHECK(duplicate < initializer_count);
            SolMirValueId second_value = mir.instructions[initializers[duplicate]]
                .as.temporary_init.value;
            mir.instructions[initializers[duplicate]].as.temporary_init.value
                = mir.instructions[initializers[0]].as.temporary_init.value;
            CHECK(!sol_mir_validate(&compilation.ir, &mir, NULL));
            mir.instructions[initializers[duplicate]].as.temporary_init.value
                = second_value;
            CHECK(sol_mir_validate(&compilation.ir, &mir, NULL));
        }
        sol_mir_free(&mir);
        sol_mir_free(&repeated);
    }
    free_compilation(&compilation);
}

int main(void) {
    test_initial_lowering_and_determinism();
    test_transactional_unsupported();
    test_calls_and_remaining_local_control();
    test_validator_rejects_corruption();
    test_loop_control_lowering();
    test_bounded_value_construction();
    test_refined_failure_cleanup();
    test_recursive_match_lowering();
    test_propagation_lowering();
    test_projected_place_lowering();
    test_partial_move_path_state();
    test_callback_invoke_lowering();
    test_owned_temporary_cleanup();
    if (failures != 0) fprintf(stderr, "%d MIR test failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
