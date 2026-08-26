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
            left->loop_count, sizeof(*left->loops));
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
    CHECK(ending.instructions[ending.values[ending_value].definition].kind
        == SOL_MIR_INST_CONST_UNIT);
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
        &compilation.diagnostics) == SOL_MIR_LOWER_UNSUPPORTED);
    CHECK(mir.callable == SOL_IR_NONE && mir.blocks == NULL
        && mir.block_count == 0 && mir.instructions == NULL);
    CHECK(compilation.diagnostics.count == before + 1);
    CHECK(strcmp(compilation.diagnostics.items[before].code, "SOL-MIR-001") == 0);
    sol_mir_free(&mir);

    sol_mir_init(&mir);
    before = compilation.diagnostics.count;
    CHECK(sol_mir_lower_callable(&compilation.ir,
        callable(&compilation.ir, "block_argument"), &mir,
        &compilation.diagnostics) == SOL_MIR_LOWER_UNSUPPORTED);
    CHECK(mir.callable == SOL_IR_NONE && mir.blocks == NULL
        && mir.call_arguments == NULL);
    CHECK(compilation.diagnostics.count == before + 1);
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
                CHECK(argument->value == SOL_MIR_NONE);
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
    SolMirValueId first_owned
        = calls.call_arguments[owned_arguments.offset].value;
    SolMirValueId second_owned
        = calls.call_arguments[owned_arguments.offset + 1].value;
    calls.call_arguments[owned_arguments.offset].value = second_owned;
    calls.call_arguments[owned_arguments.offset + 1].value = first_owned;
    CHECK(!sol_mir_validate(&compilation.ir, &calls, NULL));
    calls.call_arguments[owned_arguments.offset].value = first_owned;
    calls.call_arguments[owned_arguments.offset + 1].value = second_owned;
    SolMirInstructionId first_wrapper = calls.values[first_owned].definition;
    SolMirInstructionId second_wrapper = calls.values[second_owned].definition;
    SolMirValueId first_operand = calls.instructions[first_wrapper].as.operand;
    SolMirValueId second_operand = calls.instructions[second_wrapper].as.operand;
    calls.instructions[first_wrapper].as.operand = second_operand;
    calls.instructions[second_wrapper].as.operand = first_operand;
    CHECK(!sol_mir_validate(&compilation.ir, &calls, NULL));
    calls.instructions[first_wrapper].as.operand = first_operand;
    calls.instructions[second_wrapper].as.operand = second_operand;

    SolMirTerminator *owned_term = &calls.blocks[owned_invoke].terminator;
    SolMirBlockId normal_block = owned_term->as.invoke.normal_edge.block;
    size_t parameter_offset = calls.blocks[normal_block].parameters.offset;
    calls.blocks[normal_block].parameters.offset = SIZE_MAX;
    CHECK(!sol_mir_validate(&compilation.ir, &calls, NULL));
    calls.blocks[normal_block].parameters.offset = parameter_offset;

    SolMirValueId normal_result = owned_term->as.invoke.result;
    SolMirValueId transported = calls.edge_values[
        owned_term->as.invoke.normal_edge.arguments.offset];
    SolMirValueId substitute = calls.call_arguments[owned_arguments.offset].value;
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

int main(void) {
    test_initial_lowering_and_determinism();
    test_transactional_unsupported();
    test_calls_and_remaining_local_control();
    test_validator_rejects_corruption();
    test_loop_control_lowering();
    if (failures != 0) fprintf(stderr, "%d MIR test failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
