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
        && bytes_equal(left->blocks, right->blocks,
            left->block_count, sizeof(*left->blocks))
        && bytes_equal(left->instructions, right->instructions,
            left->instruction_count, sizeof(*left->instructions))
        && bytes_equal(left->values, right->values,
            left->value_count, sizeof(*left->values))
        && bytes_equal(left->parameter_values, right->parameter_values,
            left->parameter_value_count, sizeof(*left->parameter_values))
        && bytes_equal(left->edge_values, right->edge_values,
            left->edge_value_count, sizeof(*left->edge_values));
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

int main(void) {
    test_initial_lowering_and_determinism();
    test_transactional_unsupported();
    test_validator_rejects_corruption();
    if (failures != 0) fprintf(stderr, "%d MIR test failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
