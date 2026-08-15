#include "sol/interpreter.h"
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
    return sol_source_from_text(&compilation->source, "interpreter.sol", text)
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

static void free_frontend(Compilation *compilation) {
    sol_contract_table_free(&compilation->contracts);
    sol_effect_table_free(&compilation->effects);
    sol_type_table_free(&compilation->types);
    sol_hir_module_free(&compilation->hir);
    sol_syntax_tree_free(&compilation->syntax);
    sol_tokens_free(&compilation->tokens);
    sol_source_free(&compilation->source);
}

static void free_compilation(Compilation *compilation) {
    sol_ir_free(&compilation->ir);
    free_frontend(compilation);
    sol_diagnostics_free(&compilation->diagnostics);
}

static SolIrCallableId callable(const SolIr *ir, const char *name) {
    for (size_t index = 0; index < ir->callable_count; ++index) {
        if (ir->callables[index].kind == SOL_IR_CALLABLE_FUNCTION
            && strcmp(ir->callables[index].name, name) == 0) return index;
    }
    return SOL_IR_NONE;
}

static bool has_code(const Compilation *compilation, const char *code) {
    for (size_t index = 0; index < compilation->diagnostics.count; ++index) {
        if (strcmp(compilation->diagnostics.items[index].code, code) == 0) return true;
    }
    return false;
}

static bool run(const SolIr *ir, const char *name,
    const SolInterpreterValue *arguments, size_t argument_count,
    SolInterpreterContractPolicy contracts, SolInterpreterLimits limits,
    SolInterpreterHostOperation host, void *context,
    SolInterpreterResult *result) {
    SolInterpreterRequest request;
    memset(&request, 0, sizeof(request));
    request.ir = ir;
    request.callable = callable(ir, name);
    request.definition = SOL_IR_NONE;
    request.arguments = arguments;
    request.argument_count = argument_count;
    request.contracts = contracts;
    request.limits = limits;
    request.host_operation = host;
    request.host_context = context;
    return sol_interpret(&request, result);
}

typedef struct {
    const SolIr *ir;
    const char *names[32];
    size_t count;
} CleanupLog;

static void observe_cleanup(void *context, SolIrLocalId local, size_t ordinal) {
    CleanupLog *log = context;
    CHECK(ordinal == log->count);
    CHECK(local < log->ir->local_count);
    if (log->count < sizeof(log->names) / sizeof(log->names[0])
        && local < log->ir->local_count) {
        log->names[log->count++] = log->ir->locals[local].name;
    }
}

static bool run_observed(const SolIr *ir, const char *name,
    const SolInterpreterValue *arguments, size_t argument_count,
    CleanupLog *log, SolInterpreterResult *result) {
    SolInterpreterRequest request;
    memset(&request, 0, sizeof(request));
    request.ir = ir;
    request.callable = callable(ir, name);
    request.definition = SOL_IR_NONE;
    request.arguments = arguments;
    request.argument_count = argument_count;
    request.contracts = SOL_INTERPRETER_CONTRACTS_IGNORE;
    request.cleanup_observer = observe_cleanup;
    request.cleanup_context = log;
    return sol_interpret(&request, result);
}

static void test_primitives_control_and_lifetime(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module runtime\n"
        "function arithmetic() -> Int64 { return (2 + 3) * 4 - 6 / 2 + 7 % 4 }\n"
        "function compare() -> Bool { return 2 < 3 && 4 >= 4 && !(1 == 2) }\n"
        "function short(flag: Bool) -> Bool { return flag && (1 / 0 == 0) }\n"
        "function choose(flag: Bool) -> Int64 { return if flag { 7 } else { 9 } }\n"
        "function nested() -> Int64 { { { return 12 } 0 } }\n"
        "function recurse(value: Int64) -> Int64 effects { diverge } "
        "{ return if value == 0 { 0 } else { recurse(value - 1) } }\n"
        "function overflow() -> Int64 { return 9223372036854775807 + 1 }\n"
        "function sub_overflow() -> Int64 { return (-9223372036854775807 - 1) - 1 }\n"
        "function mul_overflow() -> Int64 { return 9223372036854775807 * 2 }\n"
        "function neg_overflow() -> Int64 { return -(-9223372036854775807 - 1) }\n"
        "function min_div() -> Int64 { return (-9223372036854775807 - 1) / -1 }\n"
        "function min_rem() -> Int64 { return (-9223372036854775807 - 1) % -1 }\n"
        "function zero() -> Int64 { return 1 / 0 }\n"
        "function text() -> Text { return \"abcd\" }\n"));
    free_frontend(&compilation);

    SolInterpreterResult result;
    CHECK(run(&compilation.ir, "arithmetic", NULL, 0,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_INT64);
    CHECK(result.value.as.integer == 20);
    sol_interpreter_result_free(&result);

    CHECK(run(&compilation.ir, "compare", NULL, 0,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_BOOL && result.value.as.boolean);
    sol_interpreter_result_free(&result);

    SolInterpreterValue argument;
    sol_interpreter_value_bool(&argument, false);
    CHECK(run(&compilation.ir, "short", &argument, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_BOOL && !result.value.as.boolean);
    sol_interpreter_result_free(&result);
    sol_interpreter_value_free(&argument);

    CHECK(run(&compilation.ir, "nested", NULL, 0,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.value.as.integer == 12);
    sol_interpreter_result_free(&result);

    CHECK(!run(&compilation.ir, "overflow", NULL, 0,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INTEGER_OVERFLOW);
    sol_interpreter_result_free(&result);
    static const char *overflow_names[] = {
        "sub_overflow", "mul_overflow", "neg_overflow", "min_div", "min_rem",
    };
    for (size_t index = 0; index < 5; ++index) {
        CHECK(!run(&compilation.ir, overflow_names[index], NULL, 0,
            SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
            NULL, NULL, &result));
        CHECK(result.diagnostic.code == SOL_INTERPRETER_INTEGER_OVERFLOW);
        sol_interpreter_result_free(&result);
    }
    CHECK(!run(&compilation.ir, "zero", NULL, 0,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_DIVISION_BY_ZERO);
    sol_interpreter_result_free(&result);

    sol_interpreter_value_int64(&argument, 20);
    CHECK(!run(&compilation.ir, "recurse", &argument, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE,
        (SolInterpreterLimits){1000, 3, 1000, 1000, 10},
        NULL, NULL, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_CALL_DEPTH_LIMIT);
    sol_interpreter_result_free(&result);
    CHECK(!run(&compilation.ir, "recurse", &argument, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE,
        (SolInterpreterLimits){3, 100, 1000, 1000, 10},
        NULL, NULL, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_STEP_LIMIT);
    sol_interpreter_result_free(&result);
    sol_interpreter_value_free(&argument);
    CHECK(!run(&compilation.ir, "arithmetic", NULL, 0,
        SOL_INTERPRETER_CONTRACTS_IGNORE,
        (SolInterpreterLimits){100, 100, 1, 100, 10},
        NULL, NULL, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_VALUE_LIMIT);
    sol_interpreter_result_free(&result);
    CHECK(!run(&compilation.ir, "text", NULL, 0,
        SOL_INTERPRETER_CONTRACTS_IGNORE,
        (SolInterpreterLimits){100, 100, 100, 3, 10},
        NULL, NULL, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_TEXT_LIMIT);
    sol_interpreter_result_free(&result);
    for (size_t index = 0; index < compilation.ir.expression_count; ++index) {
        SolIrExpression *expression = &compilation.ir.expressions[index];
        if (expression->kind != SOL_IR_EXPR_STRING) continue;
        char *saved = expression->as.string;
        expression->as.string = NULL;
        SolDiagnostics malformed;
        sol_diagnostics_init(&malformed);
        CHECK(!sol_ir_validate(&compilation.ir, &malformed));
        sol_diagnostics_free(&malformed);
        CHECK(!run(&compilation.ir, "text", NULL, 0,
            SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
            NULL, NULL, &result));
        CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_IR);
        sol_interpreter_result_free(&result);
        expression->as.string = saved;
        break;
    }
    sol_ir_free(&compilation.ir);
    sol_diagnostics_free(&compilation.diagnostics);
}

static void test_assignment_replacement_and_failure(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module assignment_runtime\n"
        "function replaced() -> Int64 { var value = 1 value = 2 return value }\n"
        "function failed() -> Int64 { var value = 1 value = 2 value = 1 / 0 "
        "return value }\n"));
    free_frontend(&compilation);
    CleanupLog log;
    SolInterpreterResult result;
    memset(&log, 0, sizeof(log));
    log.ir = &compilation.ir;
    CHECK(run_observed(&compilation.ir, "replaced", NULL, 0, &log, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_OK);
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_INT64
        && result.value.as.integer == 2);
    CHECK(log.count == 2 && strcmp(log.names[0], "value") == 0
        && strcmp(log.names[1], "value") == 0);
    sol_interpreter_result_free(&result);

    memset(&log, 0, sizeof(log));
    log.ir = &compilation.ir;
    CHECK(!run_observed(&compilation.ir, "failed", NULL, 0, &log, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_DIVISION_BY_ZERO);
    CHECK(log.count == 2);
    sol_interpreter_result_free(&result);
    sol_ir_free(&compilation.ir);
    sol_diagnostics_free(&compilation.diagnostics);
}

static void test_data_callbacks_generics_and_traits(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module values\n"
        "record Pair { left: Int64, right: Int64 }\n"
        "record Box { value: Int64 }\n"
        "enum Choice { yes(value: Int64), no }\n"
        "enum Single { one(value: Int64) }\n"
        "enum Duo { pair(left: Int64, right: Int64) }\n"
        "type Meter = distinct Int64\n"
        "trait Increment { function increment(self: Self) -> Int64 effects { pure } }\n"
        "implementation Increment for Int64 { "
        "function increment(self: Self) -> Int64 effects { pure } { return self + 1 } }\n"
        "function pair() -> Int64 { let value = Pair { right = 8, left = 3 } "
        "return value.left + value.right }\n"
        "function box_value(value: Box) -> Int64 { return value.value }\n"
        "function single_value(value: Single) -> Int64 "
        "{ return match value { one(item) => item } }\n"
        "function choice(flag: Bool) -> Int64 { return match if flag { Choice.yes(5) } "
        "else { Choice.no } { yes(value) => value, no => 0 } }\n"
        "function enum_pair() -> Int64 { let value = Duo.pair(right = 4, left = 3) "
        "return match value { pair(left, right) => left * 10 + right } }\n"
        "function unwrap(value: Option<Int64>) -> Option<Int64> "
        "{ let item = value? return some(item + 1) }\n"
        "enum Failure { bad }\n"
        "function unwrap_result(value: Result<Int64, Failure>) -> Result<Int64, Failure> "
        "{ let item = value? return ok(item + 1) }\n"
        "function structural() -> Bool { return Pair { left = 1, right = 2 } "
        "== Pair { right = 2, left = 1 } && some(1) != none() }\n"
        "function identity(value: Int64) -> Int64 { return value }\n"
        "function apply(callback: function(Int64) -> Int64 effects { pure }, value: Int64) "
        "-> Int64 { return callback(value) }\n"
        "function callback() -> Int64 { return apply(identity, 11) }\n"
        "function generic<T>(value: T) -> T { return value }\n"
        "function generic_int() -> Int64 { return generic(17) }\n"
        "function bounded<T: Increment>(value: T) -> Int64 effects { pure } "
        "{ return value.increment() }\n"
        "function trait_value(value: Int64) -> Int64 effects { pure } "
        "{ return bounded(value) }\n"
        "function meter() -> Meter { return Meter(4) }\n"));

    SolInterpreterResult result;
    CHECK(run(&compilation.ir, "pair", NULL, 0, SOL_INTERPRETER_CONTRACTS_IGNORE,
        (SolInterpreterLimits){0}, NULL, NULL, &result));
    CHECK(result.value.as.integer == 11);
    sol_interpreter_result_free(&result);

    SolIrDefinitionId box_definition = SOL_IR_NONE;
    SolIrDefinitionId single_definition = SOL_IR_NONE;
    SolIrVariantId single_variant = SOL_IR_NONE;
    for (size_t index = 0; index < compilation.ir.definition_count; ++index) {
        if (strcmp(compilation.ir.definitions[index].name, "Box") == 0) {
            box_definition = index;
        } else if (strcmp(compilation.ir.definitions[index].name, "Single") == 0) {
            single_definition = index;
        }
    }
    for (size_t index = 0; index < compilation.ir.variant_count; ++index) {
        if (compilation.ir.variants[index].owner == single_definition) {
            single_variant = index;
        }
    }
    SolInterpreterValue malformed_aggregate;
    sol_interpreter_value_init(&malformed_aggregate);
    malformed_aggregate.kind = SOL_INTERPRETER_VALUE_RECORD;
    malformed_aggregate.as.aggregate.definition = box_definition;
    malformed_aggregate.as.aggregate.variant = SOL_IR_NONE;
    malformed_aggregate.as.aggregate.field_count = 1;
    CHECK(!run(&compilation.ir, "box_value", &malformed_aggregate, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST
        && result.diagnostic.message[0] != '\0');
    sol_interpreter_result_free(&result);
    sol_interpreter_value_free(&malformed_aggregate);
    sol_interpreter_value_init(&malformed_aggregate);
    malformed_aggregate.kind = SOL_INTERPRETER_VALUE_RECORD;
    malformed_aggregate.as.aggregate.definition = box_definition;
    malformed_aggregate.as.aggregate.variant = SOL_IR_NONE;
    malformed_aggregate.as.aggregate.fields = &malformed_aggregate;
    malformed_aggregate.as.aggregate.field_count = 1;
    CHECK(!run(&compilation.ir, "box_value", &malformed_aggregate, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST
        && result.diagnostic.message[0] != '\0');
    sol_interpreter_result_free(&result);
    sol_interpreter_value_free(&malformed_aggregate);
    sol_interpreter_value_init(&malformed_aggregate);
    malformed_aggregate.kind = SOL_INTERPRETER_VALUE_ENUM;
    malformed_aggregate.as.aggregate.definition = single_definition;
    malformed_aggregate.as.aggregate.variant = single_variant;
    malformed_aggregate.as.aggregate.field_count = 1;
    CHECK(!run(&compilation.ir, "single_value", &malformed_aggregate, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST
        && result.diagnostic.message[0] != '\0');
    sol_interpreter_result_free(&result);
    sol_interpreter_value_free(&malformed_aggregate);
    CHECK(run(&compilation.ir, "callback", NULL, 0,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.value.as.integer == 11);
    sol_interpreter_result_free(&result);
    CHECK(run(&compilation.ir, "generic_int", NULL, 0,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.value.as.integer == 17);
    sol_interpreter_result_free(&result);

    SolInterpreterValue boolean;
    sol_interpreter_value_bool(&boolean, true);
    CHECK(run(&compilation.ir, "choice", &boolean, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.value.as.integer == 5);
    sol_interpreter_result_free(&result);
    sol_interpreter_value_free(&boolean);
    CHECK(run(&compilation.ir, "enum_pair", NULL, 0,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_INT64
        && result.value.as.integer == 34);
    sol_interpreter_result_free(&result);

    SolInterpreterValue payload;
    SolInterpreterValue sum;
    sol_interpreter_value_int64(&payload, 6);
    CHECK(sol_interpreter_value_option(&sum, &payload));
    CHECK(run(&compilation.ir, "unwrap", &sum, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_OPTION
        && result.value.as.sum.has_value
        && result.value.as.sum.value->as.integer == 7);
    sol_interpreter_result_free(&result);
    sol_interpreter_value_free(&sum);
    CHECK(sol_interpreter_value_option(&sum, NULL));
    CHECK(run(&compilation.ir, "unwrap", &sum, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_OPTION
        && !result.value.as.sum.has_value);
    sol_interpreter_result_free(&result);
    sol_interpreter_value_free(&sum);
    sol_interpreter_value_free(&payload);

    sol_interpreter_value_int64(&payload, 3);
    CHECK(sol_interpreter_value_result(&sum, false, &payload));
    CHECK(run(&compilation.ir, "unwrap_result", &sum, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_RESULT
        && !result.value.as.sum.is_error
        && result.value.as.sum.value->as.integer == 4);
    sol_interpreter_result_free(&result);
    sol_interpreter_value_free(&sum);
    sol_interpreter_value_free(&payload);
    sol_interpreter_value_init(&payload);
    payload.kind = SOL_INTERPRETER_VALUE_ENUM;
    for (size_t index = 0; index < compilation.ir.variant_count; ++index) {
        if (strcmp(compilation.ir.variants[index].name, "bad") == 0) {
            payload.as.aggregate.definition = compilation.ir.variants[index].owner;
            payload.as.aggregate.variant = index;
        }
    }
    CHECK(sol_interpreter_value_result(&sum, true, &payload));
    CHECK(run(&compilation.ir, "unwrap_result", &sum, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_RESULT
        && result.value.as.sum.is_error);
    sol_interpreter_result_free(&result);
    sol_interpreter_value_free(&sum);
    sol_interpreter_value_free(&payload);

    CHECK(run(&compilation.ir, "structural", NULL, 0,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_BOOL && result.value.as.boolean);
    SolInterpreterLimits first_used = result.used;
    sol_interpreter_result_free(&result);
    CHECK(run(&compilation.ir, "structural", NULL, 0,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(memcmp(&first_used, &result.used, sizeof(first_used)) == 0);
    sol_interpreter_result_free(&result);

    SolInterpreterValue argument;
    sol_interpreter_value_int64(&argument, 9);
    CHECK(run(&compilation.ir, "trait_value", &argument, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.value.as.integer == 10);
    sol_interpreter_result_free(&result);
    sol_interpreter_value_free(&argument);

    CHECK(run(&compilation.ir, "meter", NULL, 0,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_DISTINCT);
    CHECK(result.value.as.distinct.value->as.integer == 4);
    sol_interpreter_result_free(&result);

    SolIrArm *payload_arm = NULL;
    SolIrArm *payloadless_arm = NULL;
    for (size_t index = 0; index < compilation.ir.arm_count; ++index) {
        SolIrArm *arm = &compilation.ir.arms[index];
        if (arm->kind != SOL_IR_PATTERN_VARIANT) continue;
        if (arm->bindings.count == 0) payloadless_arm = arm;
        else if (payload_arm == NULL) payload_arm = arm;
    }
    CHECK(payload_arm != NULL && payloadless_arm != NULL);
    if (payload_arm != NULL && payloadless_arm != NULL) {
        SolIrSlice saved = payload_arm->bindings;
        payload_arm->bindings.count = 0;
        SolDiagnostics malformed;
        sol_diagnostics_init(&malformed);
        CHECK(!sol_ir_validate(&compilation.ir, &malformed));
        sol_diagnostics_free(&malformed);
        sol_interpreter_value_bool(&boolean, true);
        CHECK(!run(&compilation.ir, "choice", &boolean, 1,
            SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
            NULL, NULL, &result));
        CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_IR);
        sol_interpreter_result_free(&result);
        sol_interpreter_value_free(&boolean);
        payload_arm->bindings = saved;
        SolIrLocalId local = compilation.ir.roots[saved.offset];
        SolIrTypeId saved_type = compilation.ir.locals[local].type;
        for (size_t type = 0; type < compilation.ir.type_count; ++type) {
            if (compilation.ir.types[type].kind == SOL_IR_TYPE_BOOL) {
                compilation.ir.locals[local].type = type;
                break;
            }
        }
        sol_diagnostics_init(&malformed);
        CHECK(!sol_ir_validate(&compilation.ir, &malformed));
        sol_diagnostics_free(&malformed);
        compilation.ir.locals[local].type = saved_type;
        SolIrSlice saved_payloadless = payloadless_arm->bindings;
        payloadless_arm->bindings = saved;
        sol_diagnostics_init(&malformed);
        CHECK(!sol_ir_validate(&compilation.ir, &malformed));
        sol_diagnostics_free(&malformed);
        payloadless_arm->bindings = saved_payloadless;
    }
    free_compilation(&compilation);
}

static void test_exact_generic_evidence_bindings(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module evidence_bindings\n"
        "trait Identify { function identify(self: Self) -> Int64 effects { pure } }\n"
        "implementation Identify for Int64 { function identify(self: Self) -> Int64 "
        "effects { pure } { return 11 } }\n"
        "implementation Identify for Bool { function identify(self: Self) -> Int64 "
        "effects { pure } { return 22 } }\n"
        "function second<T: Identify, U: Identify>(first: T, right: U) -> Int64 "
        "effects { pure } { return right.identify() }\n"
        "function middle<A: Identify, B: Identify>(first: A, right: B) -> Int64 "
        "effects { pure } { return second(first, right) }\n"
        "function outer<X: Identify, Y: Identify>(first: X, right: Y) -> Int64 "
        "effects { pure } { return middle(first, right) }\n"
        "function bool_callback(value: Bool) -> Int64 effects { pure } { return 100 }\n"
        "function callback_second<T: Identify, U: Identify>(first: T, right: U, "
        "callback: function(U) -> Int64 effects { pure }) -> Int64 effects { pure } "
        "{ return first.identify() + callback(right) }\n"
        "function callback_middle<A: Identify, B: Identify>(first: A, right: B, "
        "callback: function(B) -> Int64 effects { pure }) -> Int64 effects { pure } "
        "{ return callback_second(first, right, callback) }\n"
        "function direct() -> Int64 effects { pure } { return second(1, true) }\n"
        "function forwarded() -> Int64 effects { pure } { return outer(1, true) }\n"
        "function callback_forwarded() -> Int64 effects { pure } "
        "{ return callback_middle(1, true, bool_callback) }\n"));
    SolInterpreterResult result;
    CHECK(run(&compilation.ir, "direct", NULL, 0,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_INT64
        && result.value.as.integer == 22);
    sol_interpreter_result_free(&result);
    CHECK(run(&compilation.ir, "forwarded", NULL, 0,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.value.as.integer == 22);
    sol_interpreter_result_free(&result);
    CHECK(run(&compilation.ir, "callback_forwarded", NULL, 0,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.value.as.integer == 111);
    sol_interpreter_result_free(&result);

    SolIrExpression *forwarded_call = NULL;
    for (size_t index = 0; index < compilation.ir.expression_count; ++index) {
        SolIrExpression *expression = &compilation.ir.expressions[index];
        if (expression->kind != SOL_IR_EXPR_CALL
            || expression->as.call.kind != SOL_IR_CALL_FUNCTION) continue;
        for (size_t evidence = 0; evidence < expression->as.call.evidence.count;
            ++evidence) {
            if (compilation.ir.evidence[expression->as.call.evidence.offset
                + evidence].forwarded) {
                forwarded_call = expression;
                break;
            }
        }
        if (forwarded_call != NULL) break;
    }
    CHECK(forwarded_call != NULL);
    if (forwarded_call != NULL) {
        SolIrTypeId *type_id = &compilation.ir.type_ids[
            forwarded_call->as.call.type_arguments.offset
        ];
        SolIrTypeId saved = *type_id;
        *type_id = compilation.ir.type_count;
        SolDiagnostics malformed;
        sol_diagnostics_init(&malformed);
        CHECK(!sol_ir_validate(&compilation.ir, &malformed));
        sol_diagnostics_free(&malformed);
        CHECK(!run(&compilation.ir, "forwarded", NULL, 0,
            SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
            NULL, NULL, &result));
        CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_IR);
        sol_interpreter_result_free(&result);
        *type_id = saved;
    }

    SolIrCallableId second_callable = callable(&compilation.ir, "second");
    const SolIrExpression *concrete_call = NULL;
    for (size_t index = 0; index < compilation.ir.expression_count; ++index) {
        const SolIrExpression *expression = &compilation.ir.expressions[index];
        if (expression->kind == SOL_IR_EXPR_CALL
            && expression->as.call.kind == SOL_IR_CALL_FUNCTION
            && expression->as.call.callable == second_callable
            && expression->as.call.type_arguments.count == 2
            && compilation.ir.types[compilation.ir.type_ids[
                expression->as.call.type_arguments.offset]].kind == SOL_IR_TYPE_INT64
            && compilation.ir.types[compilation.ir.type_ids[
                expression->as.call.type_arguments.offset + 1]].kind == SOL_IR_TYPE_BOOL) {
            concrete_call = expression;
            break;
        }
    }
    CHECK(concrete_call != NULL);
    if (concrete_call != NULL) {
        SolIrDispatchEvidence *first_evidence = &compilation.ir.evidence[
            concrete_call->as.call.evidence.offset
        ];
        SolIrGenericParameterId saved_binding = first_evidence->binding;
        SolDiagnostics validation;
        sol_diagnostics_init(&validation);
        first_evidence->binding = SOL_IR_NONE;
        CHECK(!sol_ir_validate(&compilation.ir, &validation));
        first_evidence->binding = saved_binding;
        sol_diagnostics_free(&validation);
        SolInterpreterValue arguments[2];
        sol_interpreter_value_int64(&arguments[0], 1);
        sol_interpreter_value_bool(&arguments[1], true);
        SolInterpreterRequest request;
        memset(&request, 0, sizeof(request));
        request.ir = &compilation.ir;
        request.callable = second_callable;
        request.definition = SOL_IR_NONE;
        request.arguments = arguments;
        request.argument_count = 2;
        request.type_arguments = compilation.ir.type_ids
            + concrete_call->as.call.type_arguments.offset;
        request.type_argument_count = 2;
        request.evidence.items = compilation.ir.evidence
            + concrete_call->as.call.evidence.offset;
        request.evidence.count = concrete_call->as.call.evidence.count;
        request.contracts = SOL_INTERPRETER_CONTRACTS_IGNORE;
        CHECK(sol_interpret(&request, &result));
        CHECK(result.value.as.integer == 22);
        sol_interpreter_result_free(&result);
        SolInterpreterEvidence saved_evidence = request.evidence;
        request.evidence.count = saved_evidence.count - 1;
        CHECK(!sol_interpret(&request, &result));
        CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST);
        sol_interpreter_result_free(&result);
        request.evidence = saved_evidence;
        size_t malformed_count = request.evidence.count + 1;
        SolIrDispatchEvidence *malformed = malloc(
            malformed_count * sizeof(*malformed));
        CHECK(malformed != NULL);
        if (malformed != NULL) {
            memcpy(malformed, request.evidence.items,
                request.evidence.count * sizeof(*malformed));
            malformed[0].binding = SOL_IR_NONE;
            request.evidence.items = malformed;
            CHECK(!sol_interpret(&request, &result));
            CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST);
            sol_interpreter_result_free(&result);
            memcpy(malformed, saved_evidence.items,
                saved_evidence.count * sizeof(*malformed));
            malformed[malformed_count - 1] = malformed[0];
            request.evidence.count = malformed_count;
            CHECK(!sol_interpret(&request, &result));
            CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST);
            sol_interpreter_result_free(&result);
            free(malformed);
        }
        sol_interpreter_value_free(&arguments[0]);
        sol_interpreter_value_free(&arguments[1]);
    }
    free_compilation(&compilation);

    CHECK(!compile(&compilation,
        "module generic_function_value\n"
        "function identity<T>(value: T) -> T effects { pure } { return value }\n"
        "function apply(callback: function(Int64) -> Int64 effects { pure }) -> Int64 "
        "effects { pure } { return callback(1) }\n"
        "function invalid() -> Int64 effects { pure } { return apply(identity) }\n"));
    CHECK(has_code(&compilation, "SOL-TYPE-020"));
    free_compilation(&compilation);
}

typedef struct {
    int calls;
    bool fail;
    bool malformed_cycle;
    void *special_root;
    int64_t special_value;
} Host;

static bool host_read(void *context, const SolIr *ir,
    SolIrCallableId operation, void *root,
    const SolInterpreterValue *private_source,
    const SolInterpreterValue *arguments, size_t argument_count,
    SolInterpreterValue *result, const char **error_message) {
    Host *host = context;
    (void)root;
    (void)private_source;
    (void)arguments;
    (void)argument_count;
    ++host->calls;
    if (host->fail) {
        *error_message = "fixture host failure";
        return false;
    }
    if (host->malformed_cycle) {
        sol_interpreter_value_init(result);
        result->kind = SOL_INTERPRETER_VALUE_DISTINCT;
        result->as.distinct.definition = 0;
        result->as.distinct.value = result;
        return true;
    }
    const char *owner = ir->definitions[ir->callables[operation].owner].name;
    int64_t value = strcmp(owner, "Mock") == 0 ? 99 : 41;
    if (root == host->special_root) value = host->special_value;
    return sol_interpreter_value_int64(result, value);
}

static bool host_borrow_argument(void *context, const SolIr *ir,
    SolIrCallableId operation, void *root,
    const SolInterpreterValue *private_source,
    const SolInterpreterValue *arguments, size_t argument_count,
    SolInterpreterValue *result, const char **error_message) {
    int *calls = context;
    (void)ir;
    (void)operation;
    (void)root;
    (void)private_source;
    (void)error_message;
    ++*calls;
    if (arguments == NULL || argument_count != 1) return false;
    *result = arguments[0];
    return true;
}

static void test_borrowed_host_results(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module borrowed_host_results\n"
        "record Inner { value: Text }\n"
        "record Outer { inner: Inner }\n"
        "capability Echo { "
        "function text(value: Text) -> Text effects { pure } "
        "function nested(value: Outer) -> Outer effects { pure } }\n"
        "function echo_text(host: capability Echo, value: Text) -> Text effects { pure } "
        "{ return host.text(value) }\n"
        "function echo_nested(host: capability Echo, value: Text) -> Outer effects { pure } "
        "{ return host.nested(Outer { inner = Inner { value = value } }) }\n"));
    SolIrDefinitionId echo = SOL_IR_NONE;
    for (size_t index = 0; index < compilation.ir.definition_count; ++index) {
        if (strcmp(compilation.ir.definitions[index].name, "Echo") == 0) echo = index;
    }
    CHECK(echo != SOL_IR_NONE);
    int root;
    SolInterpreterValue arguments[2];
    CHECK(sol_interpreter_value_capability(&arguments[0], echo, &root, NULL));
    CHECK(sol_interpreter_value_text(&arguments[1], "borrowed text", 13));
    int calls = 0;
    SolInterpreterResult result;
    CHECK(run(&compilation.ir, "echo_text", arguments, 2,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        host_borrow_argument, &calls, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_TEXT
        && result.value.as.text.length == 13
        && memcmp(result.value.as.text.bytes, "borrowed text", 13) == 0);
    sol_interpreter_result_free(&result);

    CHECK(run(&compilation.ir, "echo_nested", arguments, 2,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        host_borrow_argument, &calls, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_RECORD
        && result.value.as.aggregate.field_count == 1
        && result.value.as.aggregate.fields[0].kind == SOL_INTERPRETER_VALUE_RECORD
        && result.value.as.aggregate.fields[0].as.aggregate.field_count == 1
        && result.value.as.aggregate.fields[0].as.aggregate.fields[0].kind
            == SOL_INTERPRETER_VALUE_TEXT
        && result.value.as.aggregate.fields[0].as.aggregate.fields[0].as.text.length == 13
        && memcmp(result.value.as.aggregate.fields[0].as.aggregate.fields[0].as.text.bytes,
            "borrowed text", 13) == 0);
    CHECK(calls == 2);
    sol_interpreter_result_free(&result);
    sol_interpreter_value_free(&arguments[0]);
    sol_interpreter_value_free(&arguments[1]);
    free_compilation(&compilation);
}

static void test_computed_bound_operations(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module computed_operations\n"
        "capability Clock { function offset(delta: Int64) -> Int64 effects { pure } }\n"
        "capability Other { function offset(delta: Int64) -> Int64 effects { pure } "
        "function flag(delta: Int64) -> Bool effects { pure } }\n"
        "function invoke(callback: function(Int64) -> Int64 effects { pure }, "
        "value: Int64) -> Int64 effects { pure } { return callback(value) }\n"
        "function direct_if(flag: Bool, first: capability Clock, "
        "second: capability Clock) -> Int64 effects { pure } "
        "{ return (if flag { first.offset } else { second.offset })(1) }\n"
        "function direct_match(flag: Bool, first: capability Clock, "
        "second: capability Clock) -> Int64 effects { pure } "
        "{ let operation = match flag { true => first.offset false => second.offset } "
        "return operation(1) }\n"
        "function callback_if(flag: Bool, first: capability Clock, "
        "second: capability Clock) -> Int64 effects { pure } "
        "{ return invoke(if flag { first.offset } else { second.offset }, 1) }\n"
        "function callback_match(flag: Bool, first: capability Clock, "
        "second: capability Clock) -> Int64 effects { pure } "
        "{ return invoke(match flag { true => first.offset false => second.offset }, 1) }\n"
        "function other_flag(other: capability Other) -> Bool effects { pure } "
        "{ return other.flag(1) }\n"));
    size_t exact_joins = 0;
    for (size_t index = 0; index < compilation.ir.expression_count; ++index) {
        const SolIrExpression *expression = &compilation.ir.expressions[index];
        if ((expression->kind == SOL_IR_EXPR_IF
                || expression->kind == SOL_IR_EXPR_MATCH)
            && expression->type < compilation.ir.type_count
            && compilation.ir.types[expression->type].kind == SOL_IR_TYPE_FUNCTION) {
            ++exact_joins;
        }
    }
    CHECK(exact_joins == 4);
    SolIrDefinitionId clock = SOL_IR_NONE;
    SolIrDefinitionId other = SOL_IR_NONE;
    for (size_t index = 0; index < compilation.ir.definition_count; ++index) {
        if (strcmp(compilation.ir.definitions[index].name, "Clock") == 0) clock = index;
        if (strcmp(compilation.ir.definitions[index].name, "Other") == 0) other = index;
    }
    int first_root;
    int second_root;
    SolInterpreterValue arguments[3];
    sol_interpreter_value_bool(&arguments[0], false);
    CHECK(sol_interpreter_value_capability(&arguments[1], clock, &first_root, NULL));
    CHECK(sol_interpreter_value_capability(&arguments[2], clock, &second_root, NULL));
    Host host = {.special_root = &second_root, .special_value = 77};
    static const char *names[] = {
        "direct_if", "direct_match", "callback_if", "callback_match",
    };
    SolInterpreterResult result;
    for (size_t index = 0; index < 4; ++index) {
        CHECK(run(&compilation.ir, names[index], arguments, 3,
            SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
            host_read, &host, &result));
        CHECK(result.value.kind == SOL_INTERPRETER_VALUE_INT64
            && result.value.as.integer == 77);
        sol_interpreter_result_free(&result);
    }
    SolIrExpression *clock_operation = NULL;
    SolIrExpression *other_flag = NULL;
    SolIrCallableId other_offset = SOL_IR_NONE;
    for (size_t index = 0; index < compilation.ir.callable_count; ++index) {
        if (compilation.ir.callables[index].owner == other
            && strcmp(compilation.ir.callables[index].name, "offset") == 0) {
            other_offset = index;
        }
    }
    for (size_t index = 0; index < compilation.ir.expression_count; ++index) {
        SolIrExpression *expression = &compilation.ir.expressions[index];
        if (expression->kind != SOL_IR_EXPR_BOUND_OPERATION) continue;
        SolIrDefinitionId owner
            = compilation.ir.callables[expression->as.operation.callable].owner;
        if (owner == clock && clock_operation == NULL) clock_operation = expression;
        if (owner == other
            && strcmp(compilation.ir.callables[expression->as.operation.callable].name,
                "flag") == 0) other_flag = expression;
    }
    CHECK(clock_operation != NULL && other_flag != NULL && other_offset != SOL_IR_NONE);
    if (clock_operation != NULL && other_flag != NULL && other_offset != SOL_IR_NONE) {
        SolIrCallableId saved_callable = clock_operation->as.operation.callable;
        clock_operation->as.operation.callable = other_offset;
        SolDiagnostics malformed;
        sol_diagnostics_init(&malformed);
        CHECK(!sol_ir_validate(&compilation.ir, &malformed));
        sol_diagnostics_free(&malformed);
        CHECK(!run(&compilation.ir, "direct_if", arguments, 3,
            SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
            host_read, &host, &result));
        CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_IR);
        sol_interpreter_result_free(&result);
        clock_operation->as.operation.callable = saved_callable;
        SolIrTypeId saved_type = clock_operation->type;
        clock_operation->type = other_flag->type;
        sol_diagnostics_init(&malformed);
        CHECK(!sol_ir_validate(&compilation.ir, &malformed));
        sol_diagnostics_free(&malformed);
        CHECK(!run(&compilation.ir, "direct_if", arguments, 3,
            SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
            host_read, &host, &result));
        CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_IR);
        sol_interpreter_result_free(&result);
        clock_operation->type = saved_type;

        SolInterpreterValue bypass[2];
        sol_interpreter_value_init(&bypass[0]);
        bypass[0].kind = SOL_INTERPRETER_VALUE_FUNCTION;
        bypass[0].as.callable.callable = other_offset;
        sol_interpreter_value_int64(&bypass[1], 1);
        int forged_calls = host.calls;
        CHECK(!run(&compilation.ir, "invoke", bypass, 2,
            SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
            host_read, &host, &result));
        CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST
            && result.diagnostic.message[0] != '\0');
        CHECK(host.calls == forged_calls);
        sol_interpreter_result_free(&result);
        sol_interpreter_value_free(&bypass[0]);
        sol_interpreter_value_free(&bypass[1]);
        sol_interpreter_value_init(&bypass[0]);
        bypass[0].kind = SOL_INTERPRETER_VALUE_BOUND_OPERATION;
        bypass[0].as.callable.callable = other_offset;
        bypass[0].as.callable.receiver = malloc(
            sizeof(*bypass[0].as.callable.receiver));
        CHECK(bypass[0].as.callable.receiver != NULL);
        if (bypass[0].as.callable.receiver != NULL) {
            CHECK(sol_interpreter_value_clone(bypass[0].as.callable.receiver,
                &arguments[1]));
            sol_interpreter_value_int64(&bypass[1], 1);
            int calls = host.calls;
            CHECK(!run(&compilation.ir, "invoke", bypass, 2,
                SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
                host_read, &host, &result));
            CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST);
            CHECK(host.calls == calls);
            sol_interpreter_result_free(&result);
            sol_interpreter_value_free(&bypass[1]);
        }
        sol_interpreter_value_free(&bypass[0]);
    }
    for (size_t index = 0; index < 3; ++index) {
        sol_interpreter_value_free(&arguments[index]);
    }
    free_compilation(&compilation);
}

static void test_capability_policy_and_malformed(void) {
    Compilation compilation;
    bool compiled = compile(&compilation,
        "module host\n"
        "capability Read { function read() -> Int64 effects { service.read<Self> } }\n"
        "capability Wrapped derives_from private_source: capability Read { "
        "function read() -> Int64 effects { service.read<Self> } "
        "{ return private_source.read() } }\n"
        "capability Wrong {}\n"
        "function invoke_read(source: capability Read) -> Int64 effects { service.read<source> } "
        "ensures { result == result } { let operation = source.read return operation() }\n"
        "function invoke_wrapped(source: capability Read) -> Int64 "
        "effects { service.read<source> } { let wrapper = Wrapped { private_source = source } "
        "return wrapper.read() }\n"
        "function invoke_wrapped_value(value: capability Wrapped) -> Int64 "
        "effects { service.read<value> } { return value.read() }\n");
    CHECK(compiled);
    if (!compiled) {
        sol_diagnostics_render_human(stderr, &compilation.source,
            &compilation.diagnostics);
        free_compilation(&compilation);
        return;
    }
    SolIrDefinitionId capability_definition = SOL_IR_NONE;
    for (size_t index = 0; index < compilation.ir.definition_count; ++index) {
        if (compilation.ir.definitions[index].kind == SOL_IR_DEFINITION_CAPABILITY
            && strcmp(compilation.ir.definitions[index].name, "Read") == 0) {
            capability_definition = index;
        }
    }
    int root;
    SolInterpreterValue argument;
    CHECK(sol_interpreter_value_capability(&argument, capability_definition,
        &root, NULL));
    SolInterpreterResult result;
    Host host = {0};
    CHECK(run(&compilation.ir, "invoke_read", &argument, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        host_read, &host, &result));
    CHECK(result.value.as.integer == 41 && host.calls == 1);
    sol_interpreter_result_free(&result);
    CHECK(!run(&compilation.ir, "invoke_read", &argument, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE,
        (SolInterpreterLimits){100, 100, 100, 100, 0},
        host_read, &host, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_HOST_CALL_LIMIT);
    sol_interpreter_result_free(&result);
    CHECK(run(&compilation.ir, "invoke_wrapped", &argument, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        host_read, &host, &result));
    CHECK(result.value.as.integer == 41 && host.calls == 2);
    sol_interpreter_result_free(&result);
    SolIrDefinitionId wrapped_definition = SOL_IR_NONE;
    SolIrDefinitionId wrong_definition = SOL_IR_NONE;
    for (size_t index = 0; index < compilation.ir.definition_count; ++index) {
        if (strcmp(compilation.ir.definitions[index].name, "Wrapped") == 0) {
            wrapped_definition = index;
        } else if (strcmp(compilation.ir.definitions[index].name, "Wrong") == 0) {
            wrong_definition = index;
        }
    }
    SolInterpreterValue malformed_argument;
    CHECK(sol_interpreter_value_capability(&malformed_argument,
        wrapped_definition, &root, &argument));
    CHECK(run(&compilation.ir, "invoke_wrapped_value", &malformed_argument, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        host_read, &host, &result));
    CHECK(result.value.as.integer == 41);
    sol_interpreter_result_free(&result);
    CHECK(!run(&compilation.ir, "invoke_read", &malformed_argument, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        host_read, &host, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST);
    sol_interpreter_result_free(&result);
    sol_interpreter_value_free(&malformed_argument);
    CHECK(sol_interpreter_value_capability(&malformed_argument,
        wrapped_definition, &root, NULL));
    CHECK(!run(&compilation.ir, "invoke_wrapped_value", &malformed_argument, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        host_read, &host, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST
        && result.diagnostic.message[0] != '\0');
    sol_interpreter_result_free(&result);
    sol_interpreter_value_free(&malformed_argument);
    int wrong_root;
    CHECK(sol_interpreter_value_capability(&malformed_argument,
        wrapped_definition, &wrong_root, &argument));
    CHECK(!run(&compilation.ir, "invoke_wrapped_value", &malformed_argument, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        host_read, &host, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST
        && result.diagnostic.message[0] != '\0');
    sol_interpreter_result_free(&result);
    sol_interpreter_value_free(&malformed_argument);
    SolInterpreterValue wrong_source;
    CHECK(sol_interpreter_value_capability(&wrong_source,
        wrong_definition, &root, NULL));
    CHECK(sol_interpreter_value_capability(&malformed_argument,
        wrapped_definition, &root, &wrong_source));
    CHECK(!run(&compilation.ir, "invoke_wrapped_value", &malformed_argument, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        host_read, &host, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST
        && result.diagnostic.message[0] != '\0');
    sol_interpreter_result_free(&result);
    sol_interpreter_value_free(&malformed_argument);
    sol_interpreter_value_free(&wrong_source);
    sol_interpreter_value_init(&malformed_argument);
    malformed_argument.kind = SOL_INTERPRETER_VALUE_CAPABILITY;
    malformed_argument.as.capability.definition = wrapped_definition;
    malformed_argument.as.capability.root = &root;
    malformed_argument.as.capability.source = &malformed_argument;
    CHECK(!run(&compilation.ir, "invoke_wrapped_value", &malformed_argument, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        host_read, &host, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST
        && result.diagnostic.message[0] != '\0');
    sol_interpreter_result_free(&result);
    sol_interpreter_value_free(&malformed_argument);
    host.malformed_cycle = true;
    CHECK(!run(&compilation.ir, "invoke_read", &argument, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        host_read, &host, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_TYPE_INVARIANT
        && result.diagnostic.message[0] != '\0');
    sol_interpreter_result_free(&result);
    host.malformed_cycle = false;
    CHECK(!run(&compilation.ir, "invoke_read", &argument, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_UNBOUND_OPERATION);
    sol_interpreter_result_free(&result);
    host.fail = true;
    CHECK(!run(&compilation.ir, "invoke_read", &argument, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        host_read, &host, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_HOST_ERROR);
    sol_interpreter_result_free(&result);
    CHECK(!run(&compilation.ir, "invoke_read", &argument, 1,
        SOL_INTERPRETER_CONTRACTS_CHECK, (SolInterpreterLimits){0},
        host_read, &host, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_UNSUPPORTED_CONTRACT_POLICY);
    sol_interpreter_result_free(&result);

    SolIrExpressionId saved = compilation.ir.callables[callable(
        &compilation.ir, "invoke_read")].body;
    compilation.ir.callables[callable(&compilation.ir, "invoke_read")].body
        = compilation.ir.expression_count;
    CHECK(!run(&compilation.ir, "invoke_read", &argument, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        host_read, &host, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_IR);
    sol_interpreter_result_free(&result);
    compilation.ir.callables[callable(&compilation.ir, "invoke_read")].body = saved;
    sol_interpreter_value_free(&argument);
    free_compilation(&compilation);
}

static void test_deep_handler(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module handlers\n"
        "capability Read { function read() -> Int64 effects { service.read<Self> } }\n"
        "capability Mock { function read() -> Int64 effects { pure } }\n"
        "function helper(source: borrow capability Read) -> Int64 effects { service.read<source> } "
        "{ return source.read() }\n"
        "function handled(source: capability Read, mock: capability Mock) -> Int64 "
        "effects { pure } { return handle service.read<source> with mock { helper(source) } }\n"
        "function other_root(source: capability Read, other: capability Read, "
        "mock: capability Mock) -> Int64 effects { service.read<other> } "
        "{ return handle service.read<source> with mock { helper(other) } }\n"
        "function nested(source: capability Read, outer: capability Mock, "
        "inner: capability Mock) -> Int64 effects { pure } "
        "{ return handle service.read<source> with outer { "
        "handle service.read<source> with inner { helper(source) } } }\n"));
    SolIrDefinitionId read_definition = SOL_IR_NONE;
    SolIrDefinitionId mock_definition = SOL_IR_NONE;
    for (size_t index = 0; index < compilation.ir.definition_count; ++index) {
        if (strcmp(compilation.ir.definitions[index].name, "Read") == 0) {
            read_definition = index;
        } else if (strcmp(compilation.ir.definitions[index].name, "Mock") == 0) {
            mock_definition = index;
        }
    }
    int read_root;
    int mock_root;
    SolInterpreterValue arguments[2];
    CHECK(sol_interpreter_value_capability(&arguments[0], read_definition,
        &read_root, NULL));
    CHECK(sol_interpreter_value_capability(&arguments[1], mock_definition,
        &mock_root, NULL));
    Host host = {0};
    SolInterpreterResult result;
    CHECK(run(&compilation.ir, "handled", arguments, 2,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        host_read, &host, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_INT64
        && result.value.as.integer == 99 && host.calls == 1);
    sol_interpreter_result_free(&result);
    SolIrExpression *handler_expression = NULL;
    for (size_t index = 0; index < compilation.ir.expression_count; ++index) {
        if (compilation.ir.expressions[index].kind == SOL_IR_EXPR_HANDLE) {
            handler_expression = &compilation.ir.expressions[index];
            break;
        }
    }
    CHECK(handler_expression != NULL);
    if (handler_expression != NULL) {
        SolIrCallableId saved_provider
            = handler_expression->as.handler.provider_callable;
        handler_expression->as.handler.provider_callable
            = handler_expression->as.handler.source;
        SolDiagnostics malformed;
        sol_diagnostics_init(&malformed);
        CHECK(!sol_ir_validate(&compilation.ir, &malformed));
        sol_diagnostics_free(&malformed);
        CHECK(!run(&compilation.ir, "handled", arguments, 2,
            SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
            host_read, &host, &result));
        CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_IR);
        CHECK(host.calls == 1);
        sol_interpreter_result_free(&result);
        handler_expression->as.handler.provider_callable = saved_provider;
    }
    int other_root;
    SolInterpreterValue other_arguments[3];
    CHECK(sol_interpreter_value_clone(&other_arguments[0], &arguments[0]));
    CHECK(sol_interpreter_value_capability(&other_arguments[1], read_definition,
        &other_root, NULL));
    CHECK(sol_interpreter_value_clone(&other_arguments[2], &arguments[1]));
    CHECK(run(&compilation.ir, "other_root", other_arguments, 3,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        host_read, &host, &result));
    CHECK(result.value.as.integer == 41 && host.calls == 2);
    sol_interpreter_result_free(&result);
    for (size_t index = 0; index < 3; ++index) {
        sol_interpreter_value_free(&other_arguments[index]);
    }
    int inner_root;
    SolInterpreterValue nested_arguments[3];
    CHECK(sol_interpreter_value_clone(&nested_arguments[0], &arguments[0]));
    CHECK(sol_interpreter_value_clone(&nested_arguments[1], &arguments[1]));
    CHECK(sol_interpreter_value_capability(&nested_arguments[2], mock_definition,
        &inner_root, NULL));
    host.special_root = &inner_root;
    host.special_value = 77;
    CHECK(run(&compilation.ir, "nested", nested_arguments, 3,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        host_read, &host, &result));
    CHECK(result.value.as.integer == 77 && host.calls == 3);
    sol_interpreter_result_free(&result);
    for (size_t index = 0; index < 3; ++index) {
        sol_interpreter_value_free(&nested_arguments[index]);
    }
    sol_interpreter_value_free(&arguments[0]);
    sol_interpreter_value_free(&arguments[1]);
    free_compilation(&compilation);
}

static void test_package_diagnostic_mapping(void) {
    SolPackage package;
    SolDiagnostics diagnostics;
    SolHirModule hir;
    SolTypeTable types;
    SolEffectTable effects;
    SolContractTable contracts;
    SolIr ir;
    char error[256];
    sol_package_init(&package);
    sol_diagnostics_init(&diagnostics);
    sol_hir_module_init(&hir);
    sol_type_table_init(&types);
    sol_effect_table_init(&effects);
    sol_contract_table_init(&contracts);
    sol_ir_init(&ir);
    CHECK(sol_package_load_directory(&package,
        SOL_TEST_SOURCE_DIR "/tests/packages/valid", &diagnostics,
        error, sizeof(error)));
    SolHirFileScope *scopes = package.file_count == 0 ? NULL
        : malloc(package.file_count * sizeof(*scopes));
    CHECK(package.file_count == 0 || scopes != NULL);
    for (size_t index = 0; index < package.file_count; ++index) {
        scopes[index] = (SolHirFileScope){
            package.files[index].module_name,
            package.files[index].import_start,
            package.files[index].import_count,
            package.files[index].item_start,
            package.files[index].item_count,
        };
    }
    CHECK(sol_hir_lower_scoped(&package.source, &package.syntax, scopes,
        package.file_count, &hir, &diagnostics));
    CHECK(sol_type_check(&package.source, &package.syntax, &hir, &types,
        &diagnostics));
    CHECK(sol_effect_check(&package.source, &package.syntax, &hir, &types,
        &effects, &diagnostics));
    CHECK(sol_contract_lower(&package.source, &package.syntax, &hir, &types,
        &effects, &contracts, &diagnostics));
    CHECK(sol_ir_lower_scoped(&package.source, &package.syntax, &hir, &types,
        &effects, &contracts, package.files, package.file_count, &ir,
        &diagnostics));
    free(scopes);
    sol_contract_table_free(&contracts);
    sol_effect_table_free(&effects);
    sol_type_table_free(&types);
    sol_hir_module_free(&hir);
    sol_package_free(&package);
    SolInterpreterValue argument;
    sol_interpreter_value_int64(&argument, 1);
    SolInterpreterResult result;
    CHECK(!run(&ir, "nonnegative", &argument, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE,
        (SolInterpreterLimits){100, 0, 100, 100, 10},
        NULL, NULL, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_CALL_DEPTH_LIMIT);
    sol_interpreter_value_free(&argument);
    sol_ir_free(&ir);
    CHECK(result.diagnostic.file[0] != '\0'
        && strstr(result.diagnostic.file, "rules/numbers.sol") != NULL);
    sol_interpreter_result_free(&result);
    sol_diagnostics_free(&diagnostics);
}

static void test_copy_and_move_reads(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module ownership_runtime\n"
        "capability Token {}\n"
        "function transfer(value: capability Token) -> capability Token "
        "authority { result derives_from value } { return value }\n"
        "function copied(value: Text) -> Text { let first = value return value }\n"));
    SolIrDefinitionId token = SOL_IR_NONE;
    bool saw_copy = false;
    bool saw_move = false;
    for (size_t index = 0; index < compilation.ir.definition_count; ++index) {
        if (strcmp(compilation.ir.definitions[index].name, "Token") == 0) token = index;
    }
    for (size_t index = 0; index < compilation.ir.expression_count; ++index) {
        saw_copy = saw_copy
            || compilation.ir.expressions[index].local_use == SOL_IR_LOCAL_USE_COPY;
        saw_move = saw_move
            || compilation.ir.expressions[index].local_use == SOL_IR_LOCAL_USE_MOVE;
    }
    CHECK(token != SOL_IR_NONE);
    CHECK(saw_copy && saw_move);
    free_frontend(&compilation);

    int root;
    SolInterpreterValue argument;
    CHECK(sol_interpreter_value_capability(&argument, token, &root, NULL));
    SolInterpreterResult result;
    CHECK(run(&compilation.ir, "transfer", &argument, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_CAPABILITY
        && result.value.as.capability.root == &root);
    sol_interpreter_result_free(&result);
    sol_interpreter_value_free(&argument);

    CHECK(sol_interpreter_value_text(&argument, "copy", 4));
    CHECK(run(&compilation.ir, "copied", &argument, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_TEXT
        && result.value.as.text.length == 4
        && memcmp(result.value.as.text.bytes, "copy", 4) == 0);
    sol_interpreter_result_free(&result);
    sol_interpreter_value_free(&argument);
    sol_ir_free(&compilation.ir);
    sol_diagnostics_free(&compilation.diagnostics);
}

static void test_callable_borrow_execution(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module borrow_execution\n"
        "function identity(value: borrow Text) -> Text { return value }\n"
        "function owned(value: Text) -> Text { return value }\n"
        "function apply(callback: function(borrow Text) -> Text effects { pure }, "
        "value: borrow Text) -> Text effects { pure } { return callback(value) }\n"));
    bool saw_shared = false;
    for (size_t index = 0; index < compilation.ir.local_count; ++index) {
        saw_shared = saw_shared
            || compilation.ir.locals[index].access == SOL_ACCESS_SHARED;
    }
    CHECK(saw_shared);
    SolInterpreterValue argument;
    CHECK(sol_interpreter_value_text(&argument, "borrowed", 8));
    SolInterpreterResult result;
    CHECK(run(&compilation.ir, "identity", &argument, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_TEXT
        && result.value.as.text.length == 8
        && memcmp(result.value.as.text.bytes, "borrowed", 8) == 0);
    sol_interpreter_result_free(&result);
    sol_interpreter_value_free(&argument);

    SolIrCallableId owned = SOL_IR_NONE;
    for (size_t index = 0; index < compilation.ir.callable_count; ++index) {
        if (strcmp(compilation.ir.callables[index].name, "owned") == 0) owned = index;
    }
    CHECK(owned != SOL_IR_NONE);
    SolInterpreterValue mismatch[2];
    sol_interpreter_value_init(&mismatch[0]);
    mismatch[0].kind = SOL_INTERPRETER_VALUE_FUNCTION;
    mismatch[0].as.callable.callable = owned;
    CHECK(sol_interpreter_value_text(&mismatch[1], "borrowed", 8));
    CHECK(!run(&compilation.ir, "apply", mismatch, 2,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST);
    sol_interpreter_result_free(&result);
    sol_interpreter_value_free(&mismatch[0]);
    sol_interpreter_value_free(&mismatch[1]);
    free_compilation(&compilation);
}

static void test_regions_and_deterministic_cleanup(void) {
    Compilation compilation;
    bool compiled = compile(&compilation,
        "module cleanup\n"
        "capability Token {}\n"
        "enum Payload { item(value: Text) }\n"
        "function normal() -> () { region outer { let a = \"a\" "
            "region inner { let b = \"b\" let c = \"c\" } } }\n"
        "function returned() -> Int64 { region r { let a = \"a\" return 7 } }\n"
        "function propagated() -> Option<Int64> { region r { let a = \"a\" "
            "none()? let done = () } return some(1) }\n"
        "function failed() -> Int64 { region r { let a = \"a\" "
            "let crash = 1 / 0 } return 0 }\n"
        "function moved(value: capability Token) -> capability Token "
            "authority { result derives_from value } { return value }\n"
        "function parameters(first: Text, second: Text) -> () { () }\n"
        "function borrowed(value: borrow Text) -> () { () }\n"
        "function matched(value: Payload) -> Text { "
            "return match value { item(text) => text } }\n");
    CHECK(compiled);
    if (!compiled) {
        sol_diagnostics_render_human(stderr, &compilation.source,
            &compilation.diagnostics);
        free_compilation(&compilation);
        return;
    }
    free_frontend(&compilation);
    SolInterpreterResult result;
    CleanupLog log = {.ir = &compilation.ir};
    CHECK(run_observed(&compilation.ir, "normal", NULL, 0, &log, &result));
    CHECK(log.count == 3);
    CHECK(strcmp(log.names[0], "c") == 0);
    CHECK(strcmp(log.names[1], "b") == 0);
    CHECK(strcmp(log.names[2], "a") == 0);
    CHECK(result.cleanup_actions == 3);
    sol_interpreter_result_free(&result);

    SolIrLocalId saved_cleanup = compilation.ir.cleanup_locals[0];
    compilation.ir.cleanup_locals[0] = SOL_IR_NONE;
    memset(&log, 0, sizeof(log)); log.ir = &compilation.ir;
    CHECK(!run_observed(&compilation.ir, "normal", NULL, 0, &log, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_IR);
    CHECK(log.count == 0 && result.cleanup_actions == 0);
    sol_interpreter_result_free(&result);
    compilation.ir.cleanup_locals[0] = saved_cleanup;

    memset(&log, 0, sizeof(log)); log.ir = &compilation.ir;
    CHECK(run_observed(&compilation.ir, "returned", NULL, 0, &log, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_INT64
        && result.value.as.integer == 7);
    CHECK(log.count == 1 && strcmp(log.names[0], "a") == 0);
    sol_interpreter_result_free(&result);

    memset(&log, 0, sizeof(log)); log.ir = &compilation.ir;
    CHECK(run_observed(&compilation.ir, "propagated", NULL, 0, &log, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_OPTION
        && !result.value.as.sum.has_value);
    CHECK(log.count == 1 && strcmp(log.names[0], "a") == 0);
    sol_interpreter_result_free(&result);

    memset(&log, 0, sizeof(log)); log.ir = &compilation.ir;
    CHECK(!run_observed(&compilation.ir, "failed", NULL, 0, &log, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_DIVISION_BY_ZERO);
    CHECK(log.count == 1 && strcmp(log.names[0], "a") == 0);
    sol_interpreter_result_free(&result);

    SolIrDefinitionId token = SOL_IR_NONE;
    SolIrDefinitionId payload = SOL_IR_NONE;
    SolIrVariantId item = SOL_IR_NONE;
    for (size_t index = 0; index < compilation.ir.definition_count; ++index) {
        if (strcmp(compilation.ir.definitions[index].name, "Token") == 0) token = index;
        if (strcmp(compilation.ir.definitions[index].name, "Payload") == 0) payload = index;
    }
    CHECK(payload != SOL_IR_NONE);
    if (payload != SOL_IR_NONE) item = compilation.ir.definitions[payload].variants.offset;
    int root;
    SolInterpreterValue argument;
    CHECK(sol_interpreter_value_capability(&argument, token, &root, NULL));
    memset(&log, 0, sizeof(log)); log.ir = &compilation.ir;
    CHECK(run_observed(&compilation.ir, "moved", &argument, 1, &log, &result));
    CHECK(log.count == 0 && result.cleanup_actions == 0);
    sol_interpreter_result_free(&result);
    sol_interpreter_value_free(&argument);

    SolInterpreterValue arguments[2];
    CHECK(sol_interpreter_value_text(&arguments[0], "x", 1));
    sol_interpreter_value_init(&arguments[1]);
    memset(&log, 0, sizeof(log)); log.ir = &compilation.ir;
    CHECK(!run_observed(&compilation.ir, "parameters", arguments, 2, &log, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST);
    CHECK(log.count == 0 && result.cleanup_actions == 0);
    sol_interpreter_result_free(&result);
    CHECK(sol_interpreter_value_text(&arguments[1], "y", 1));
    memset(&log, 0, sizeof(log)); log.ir = &compilation.ir;
    CHECK(run_observed(&compilation.ir, "parameters", arguments, 2, &log, &result));
    CHECK(log.count == 2 && strcmp(log.names[0], "second") == 0
        && strcmp(log.names[1], "first") == 0);
    sol_interpreter_result_free(&result);
    sol_interpreter_value_free(&arguments[0]);
    sol_interpreter_value_free(&arguments[1]);

    CHECK(sol_interpreter_value_text(&argument, "borrow", 6));
    memset(&log, 0, sizeof(log)); log.ir = &compilation.ir;
    CHECK(run_observed(&compilation.ir, "borrowed", &argument, 1, &log, &result));
    CHECK(log.count == 0);
    sol_interpreter_result_free(&result);
    sol_interpreter_value_free(&argument);

    sol_interpreter_value_init(&argument);
    argument.kind = SOL_INTERPRETER_VALUE_ENUM;
    argument.as.aggregate.definition = payload;
    argument.as.aggregate.variant = item;
    argument.as.aggregate.field_count = 1;
    argument.as.aggregate.fields = calloc(1, sizeof(*argument.as.aggregate.fields));
    CHECK(argument.as.aggregate.fields != NULL);
    if (argument.as.aggregate.fields != NULL) {
        CHECK(sol_interpreter_value_text(argument.as.aggregate.fields, "match", 5));
        memset(&log, 0, sizeof(log)); log.ir = &compilation.ir;
        CHECK(run_observed(&compilation.ir, "matched", &argument, 1, &log, &result));
        CHECK(log.count == 2 && strcmp(log.names[0], "text") == 0
            && strcmp(log.names[1], "value") == 0);
        sol_interpreter_result_free(&result);
    }
    sol_interpreter_value_free(&argument);
    sol_ir_free(&compilation.ir);
    sol_diagnostics_free(&compilation.diagnostics);

    CHECK(!compile(&compilation,
        "module escape\ncapability Token {}\n"
        "function bad(value: capability Token) -> capability Token "
        "authority { result derives_from value } "
        "{ region r { return value } }\n"));
    CHECK(has_code(&compilation, "SOL-REGION-001"));
    free_compilation(&compilation);
}

int main(void) {
    test_primitives_control_and_lifetime();
    test_data_callbacks_generics_and_traits();
    test_exact_generic_evidence_bindings();
    test_borrowed_host_results();
    test_computed_bound_operations();
    test_capability_policy_and_malformed();
    test_deep_handler();
    test_package_diagnostic_mapping();
    test_copy_and_move_reads();
    test_callable_borrow_execution();
    test_regions_and_deterministic_cleanup();
    test_assignment_replacement_and_failure();
    if (failures != 0) fprintf(stderr, "%d interpreter test failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
