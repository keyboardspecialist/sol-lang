#include "sol/mir_eval.h"
#include "sol/effects.h"
#include "sol/lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { \
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
    SolMir *mirs;
    SolMirLowerOutcome *outcomes;
} Fixture;

static bool fixture_compile(Fixture *fixture, const char *text) {
    memset(fixture, 0, sizeof(*fixture));
    sol_tokens_init(&fixture->tokens);
    sol_diagnostics_init(&fixture->diagnostics);
    sol_syntax_tree_init(&fixture->syntax);
    sol_hir_module_init(&fixture->hir);
    sol_type_table_init(&fixture->types);
    sol_effect_table_init(&fixture->effects);
    sol_contract_table_init(&fixture->contracts);
    sol_ir_init(&fixture->ir);
    bool compiled = sol_source_from_text(&fixture->source, "mir_eval.sol", text)
        && sol_lex(&fixture->source, &fixture->tokens, &fixture->diagnostics)
        && sol_parse(&fixture->source, &fixture->tokens, &fixture->syntax,
            &fixture->diagnostics)
        && sol_hir_lower(&fixture->source, &fixture->syntax, &fixture->hir,
            &fixture->diagnostics)
        && sol_type_check(&fixture->source, &fixture->syntax, &fixture->hir,
            &fixture->types, &fixture->diagnostics)
        && sol_effect_check(&fixture->source, &fixture->syntax, &fixture->hir,
            &fixture->types, &fixture->effects, &fixture->diagnostics)
        && sol_contract_lower(&fixture->source, &fixture->syntax, &fixture->hir,
            &fixture->types, &fixture->effects, &fixture->contracts,
            &fixture->diagnostics)
        && sol_ir_lower(&fixture->source, &fixture->syntax, &fixture->hir,
            &fixture->types, &fixture->effects, &fixture->contracts,
            &fixture->ir, &fixture->diagnostics);
    if (!compiled) return false;
    fixture->mirs = calloc(fixture->ir.callable_count, sizeof(*fixture->mirs));
    fixture->outcomes = calloc(fixture->ir.callable_count,
        sizeof(*fixture->outcomes));
    if (fixture->mirs == NULL || fixture->outcomes == NULL) return false;
    for (size_t index = 0; index < fixture->ir.callable_count; ++index) {
        sol_mir_init(&fixture->mirs[index]);
        SolDiagnostics diagnostics;
        sol_diagnostics_init(&diagnostics);
        fixture->outcomes[index] = sol_mir_lower_callable(&fixture->ir, index,
            &fixture->mirs[index], &diagnostics);
        sol_diagnostics_free(&diagnostics);
    }
    return true;
}

static void fixture_free(Fixture *fixture) {
    if (fixture->mirs != NULL) {
        for (size_t index = 0; index < fixture->ir.callable_count; ++index) {
            sol_mir_free(&fixture->mirs[index]);
        }
    }
    free(fixture->mirs);
    free(fixture->outcomes);
    sol_ir_free(&fixture->ir);
    sol_contract_table_free(&fixture->contracts);
    sol_effect_table_free(&fixture->effects);
    sol_type_table_free(&fixture->types);
    sol_hir_module_free(&fixture->hir);
    sol_syntax_tree_free(&fixture->syntax);
    sol_tokens_free(&fixture->tokens);
    sol_source_free(&fixture->source);
    sol_diagnostics_free(&fixture->diagnostics);
}

static SolIrCallableId find_callable(const SolIr *ir, const char *name,
    SolIrCallableKind kind) {
    for (size_t index = 0; index < ir->callable_count; ++index) {
        if (ir->callables[index].kind == kind
            && strcmp(ir->callables[index].name, name) == 0) return index;
    }
    return SOL_IR_NONE;
}

static SolMirCalleeStatus provide(void *context, SolIrCallableId callable,
    const SolMir **mir) {
    Fixture *fixture = context;
    *mir = NULL;
    if (callable >= fixture->ir.callable_count) return SOL_MIR_CALLEE_UNAVAILABLE;
    if (fixture->outcomes[callable] == SOL_MIR_LOWER_SUCCEEDED) {
        *mir = &fixture->mirs[callable];
        return SOL_MIR_CALLEE_FOUND;
    }
    return fixture->ir.callables[callable].body == SOL_IR_NONE
        ? SOL_MIR_CALLEE_NO_BODY : SOL_MIR_CALLEE_UNAVAILABLE;
}

static SolMirCalleeStatus unavailable_provider(void *context,
    SolIrCallableId callable, const SolMir **mir);

typedef struct {
    Fixture *fixture;
    size_t counts[256];
    SolIrCallableId unavailable;
} ProviderLog;

static SolMirCalleeStatus counting_provider(void *context,
    SolIrCallableId callable, const SolMir **mir) {
    ProviderLog *log = context;
    if (callable < 256) ++log->counts[callable];
    if (callable == log->unavailable) {
        *mir = NULL;
        return SOL_MIR_CALLEE_UNAVAILABLE;
    }
    return provide(log->fixture, callable, mir);
}

static bool value_equal(const SolInterpreterValue *left,
    const SolInterpreterValue *right) {
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
                if (!value_equal(&left->as.aggregate.fields[index],
                    &right->as.aggregate.fields[index])) return false;
            }
            return true;
        case SOL_INTERPRETER_VALUE_OPTION:
        case SOL_INTERPRETER_VALUE_RESULT:
            return left->as.sum.has_value == right->as.sum.has_value
                && left->as.sum.is_error == right->as.sum.is_error
                && (!left->as.sum.has_value
                    || value_equal(left->as.sum.value, right->as.sum.value));
        case SOL_INTERPRETER_VALUE_DISTINCT:
            return left->as.distinct.definition == right->as.distinct.definition
                && value_equal(left->as.distinct.value, right->as.distinct.value);
        case SOL_INTERPRETER_VALUE_CAPABILITY:
            return left->as.capability.definition == right->as.capability.definition
                && left->as.capability.root == right->as.capability.root
                && ((left->as.capability.source == NULL
                        && right->as.capability.source == NULL)
                    || (left->as.capability.source != NULL
                        && right->as.capability.source != NULL
                        && value_equal(left->as.capability.source,
                            right->as.capability.source)));
        case SOL_INTERPRETER_VALUE_FUNCTION:
            return left->as.callable.callable == right->as.callable.callable
                && left->as.callable.receiver == NULL
                && right->as.callable.receiver == NULL;
        case SOL_INTERPRETER_VALUE_BOUND_OPERATION:
            return left->as.callable.callable == right->as.callable.callable
                && value_equal(left->as.callable.receiver,
                    right->as.callable.receiver);
        default: return false;
    }
}

typedef struct {
    SolIrLocalId locals[128];
    size_t count;
} Cleanup;

static void cleanup(void *context, SolIrLocalId local, size_t ordinal) {
    Cleanup *log = context;
    CHECK(ordinal == log->count);
    if (log->count < 128) log->locals[log->count++] = local;
}

typedef struct {
    SolMirTraceEvent events[1024];
    size_t count;
} Trace;

static void trace(void *context, const SolMirTraceEvent *event) {
    Trace *log = context;
    CHECK(event->ordinal == log->count);
    if (log->count < 1024) log->events[log->count++] = *event;
}

static bool run_pair(Fixture *fixture, const char *name,
    const SolInterpreterValue *arguments, size_t argument_count,
    SolInterpreterContractPolicy contracts, SolInterpreterLimits limits,
    SolInterpreterHostOperation host, void *host_context,
    SolInterpreterResult *owning, SolMirEvaluateResult *mir,
    Cleanup *owning_cleanup, Cleanup *mir_cleanup, Trace *events,
    size_t trace_limit) {
    SolIrCallableId callable = find_callable(&fixture->ir, name,
        SOL_IR_CALLABLE_FUNCTION);
    CHECK(callable != SOL_IR_NONE);
    if (callable == SOL_IR_NONE) return false;
    SolInterpreterRequest source_request = {0};
    source_request.ir = &fixture->ir;
    source_request.callable = callable;
    source_request.definition = SOL_IR_NONE;
    source_request.arguments = arguments;
    source_request.argument_count = argument_count;
    source_request.contracts = contracts;
    source_request.limits = limits;
    source_request.host_operation = host;
    source_request.host_context = host_context;
    source_request.cleanup_observer = cleanup;
    source_request.cleanup_context = owning_cleanup;
    bool source_ok = sol_interpret(&source_request, owning);
    SolMirEvaluateRequest mir_request = {0};
    mir_request.ir = &fixture->ir;
    mir_request.entry = &fixture->mirs[callable];
    mir_request.arguments = arguments;
    mir_request.argument_count = argument_count;
    mir_request.contracts = contracts;
    mir_request.limits = limits;
    mir_request.trace_events = trace_limit;
    mir_request.host_operation = host;
    mir_request.host_context = host_context;
    mir_request.cleanup_observer = cleanup;
    mir_request.cleanup_context = mir_cleanup;
    mir_request.callee_provider = provide;
    mir_request.callee_context = fixture;
    mir_request.trace_observer = trace;
    mir_request.trace_context = events;
    bool mir_ok = sol_mir_evaluate(&mir_request, mir);
    if (source_ok != mir_ok || owning->diagnostic.code != mir->diagnostic.code) {
        fprintf(stderr, "%s: owning ok=%d code=%d message=%s; MIR ok=%d code=%d message=%s\n",
            name, source_ok, (int)owning->diagnostic.code,
            owning->diagnostic.message, mir_ok, (int)mir->diagnostic.code,
            mir->diagnostic.message);
    }
    CHECK(source_ok == mir_ok);
    CHECK(owning->diagnostic.code == mir->diagnostic.code);
    if (source_ok && mir_ok) CHECK(value_equal(&owning->value, &mir->value));
    if (!source_ok && !mir_ok) {
        CHECK(owning->diagnostic.span.start == mir->diagnostic.span.start);
        CHECK(owning->diagnostic.span.end == mir->diagnostic.span.end);
        CHECK(strcmp(owning->diagnostic.message,
            mir->diagnostic.message) == 0);
    }
    CHECK(owning_cleanup->count == mir_cleanup->count);
    if (owning_cleanup->count == mir_cleanup->count) {
        CHECK(memcmp(owning_cleanup->locals, mir_cleanup->locals,
            owning_cleanup->count * sizeof(*owning_cleanup->locals)) == 0);
    }
    return source_ok && mir_ok;
}

static void test_differential_core(void) {
    Fixture fixture;
    bool compiled = fixture_compile(&fixture,
        "module mir_eval_core\n"
        "record Pair { left: Int64, right: Int64 }\n"
        "enum Choice { yes(value: Int64), no }\n"
        "function accepts<T>(value: T) -> Bool effects { pure } { return true }\n"
        "type Meter = distinct Int64\n"
        "type Positive = refined Int64 where self > 0\n"
        "type GenericIdentity<T> = refined T where accepts<T>(self)\n"
        "type BlockPositive = refined Int64 where { let value = self value > 0 }\n"
        "type MatchPositive = refined Choice where match self { "
            "yes(value) => value > 0 no => false }\n"
        "function add(value: Int64) -> Int64 { return value + 1 }\n"
        "function recurse(value: Int64) -> Int64 effects { diverge } { "
            "return if value == 0 { 0 } else { recurse(value - 1) + 1 } }\n"
        "function core(flag: Bool) -> Int64 { var n = 0 while n < 3 "
            "decreases { 3 - n } { n += 1 } let pair = Pair { left = n, right = 4 } "
            "return if flag { add(pair.left) } else { pair.right } }\n"
        "function matching(flag: Bool) -> Int64 { return match "
            "if flag { Choice.yes(7) } else { Choice.no } "
            "{ yes(value) if value > 0 => value, _ => 0 } }\n"
        "function option(value: Option<Int64>) -> Option<Int64> { "
            "let item = value? return some(item + 1) }\n"
        "function make_positive(value: Int64) -> Positive { return Positive(value) }\n"
        "function make_generic() -> GenericIdentity<Int64> { "
            "return GenericIdentity<Int64>(4) }\n"
        "function make_block_positive() -> BlockPositive { "
            "return BlockPositive(4) }\n"
        "function make_match_positive() -> MatchPositive { "
            "return MatchPositive(Choice.yes(4)) }\n"
        "function contracted(value: Int64) -> Int64 requires { value > 0 } "
            "ensures { result == old(value) } { return value }\n"
        "function consume_text(value: Text) -> Text "
            "ensures { result == value } { let moved = value "
            "return moved }\n"
        "function affine_contract() -> Text { return consume_text(\"kept\") }\n"
        "function guard_contract() -> Int64 requires { match Choice.yes(1) { "
            "yes(value) if 1 / 0 == value => true, _ => true } } { return 1 }\n"
        "function rich_contract(value: Int64) -> Int64 requires { { "
            "let pair = Pair { left = value, right = 2 } "
            "let choice = Choice.yes(pair.left) "
            "add(value) > 1 && some(value) == some(value) "
            "&& Meter(value) == Meter(value) && match choice { "
            "yes(item) if item > 0 => true _ => false } } } "
            "ensures { result == old(value) } { return value }\n"
        "function overflow() -> Int64 { return 9223372036854775807 + 1 }\n"
        "function zero() -> Int64 { return 1 / 0 }\n"
        "function guard_error() -> Int64 { return match Choice.yes(1) { "
            "yes(value) if 1 / 0 == value => value, _ => 0 } }\n"
        "function panic_now() -> () effects { panic } { panic \"boom\" }\n"
        "function reached() -> () { unreachable because { true } }\n");
    CHECK(compiled);
    if (!compiled) {
        sol_diagnostics_render_human(stderr, &fixture.source,
            &fixture.diagnostics);
        fixture_free(&fixture);
        return;
    }
    const char *no_arguments[] = {
        "overflow", "zero", "guard_error", "panic_now", "reached"
    };
    for (size_t index = 0; index < 5; ++index) {
        SolInterpreterResult owning;
        SolMirEvaluateResult mir;
        Cleanup left = {0};
        Cleanup right = {0};
        Trace events = {0};
        (void)run_pair(&fixture, no_arguments[index], NULL, 0,
            SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
            NULL, NULL, &owning, &mir, &left, &right, &events, 1024);
        sol_interpreter_result_free(&owning);
        sol_mir_evaluate_result_free(&mir);
    }
    const char *contract_cases[] = {
        "make_generic", "make_block_positive", "make_match_positive",
        "affine_contract"
    };
    for (size_t index = 0; index < 4; ++index) {
        SolInterpreterResult owning;
        SolMirEvaluateResult mir;
        Cleanup left = {0}; Cleanup right = {0}; Trace events = {0};
        CHECK(run_pair(&fixture, contract_cases[index], NULL, 0,
            SOL_INTERPRETER_CONTRACTS_CHECK, (SolInterpreterLimits){0},
            NULL, NULL, &owning, &mir, &left, &right, &events, 1024));
        sol_interpreter_result_free(&owning);
        sol_mir_evaluate_result_free(&mir);
    }
    SolInterpreterResult guard_owning;
    SolMirEvaluateResult guard_mir;
    Cleanup guard_left = {0}; Cleanup guard_right = {0}; Trace guard_events = {0};
    CHECK(!run_pair(&fixture, "guard_contract", NULL, 0,
        SOL_INTERPRETER_CONTRACTS_CHECK, (SolInterpreterLimits){0},
        NULL, NULL, &guard_owning, &guard_mir, &guard_left, &guard_right,
        &guard_events, 1024));
    CHECK(guard_mir.diagnostic.code == SOL_INTERPRETER_DIVISION_BY_ZERO);
    sol_interpreter_result_free(&guard_owning);
    sol_mir_evaluate_result_free(&guard_mir);
    SolInterpreterValue argument;
    sol_interpreter_value_bool(&argument, true);
    const char *boolean_cases[] = {"core", "matching"};
    for (size_t index = 0; index < 2; ++index) {
        SolInterpreterResult owning;
        SolMirEvaluateResult mir;
        Cleanup left = {0}; Cleanup right = {0}; Trace events = {0};
        CHECK(run_pair(&fixture, boolean_cases[index], &argument, 1,
            SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
            NULL, NULL, &owning, &mir, &left, &right, &events, 1024));
        CHECK(mir.executed.blocks != 0 && mir.executed.instructions != 0
            && mir.executed.terminators != 0 && mir.executed.edges != 0);
        sol_interpreter_result_free(&owning);
        sol_mir_evaluate_result_free(&mir);
    }
    sol_interpreter_value_free(&argument);
    sol_interpreter_value_int64(&argument, 4);
    const char *integer_cases[] = {"recurse", "make_positive", "contracted"};
    for (size_t index = 0; index < 3; ++index) {
        SolInterpreterResult owning;
        SolMirEvaluateResult mir;
        Cleanup left = {0}; Cleanup right = {0}; Trace events = {0};
        CHECK(run_pair(&fixture, integer_cases[index], &argument, 1,
            index == 2 ? SOL_INTERPRETER_CONTRACTS_CHECK
                : SOL_INTERPRETER_CONTRACTS_IGNORE,
            (SolInterpreterLimits){0}, NULL, NULL, &owning, &mir,
            &left, &right, &events, 1024));
        sol_interpreter_result_free(&owning);
        sol_mir_evaluate_result_free(&mir);
    }
    argument.as.integer = 0;
    SolInterpreterResult owning;
    SolMirEvaluateResult mir;
    Cleanup left = {0}; Cleanup right = {0}; Trace events = {0};
    (void)run_pair(&fixture, "make_positive", &argument, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &owning, &mir, &left, &right, &events, 1024);
    sol_interpreter_result_free(&owning);
    sol_mir_evaluate_result_free(&mir);
    argument.as.integer = 4;
    memset(&left, 0, sizeof(left)); memset(&right, 0, sizeof(right));
    memset(&events, 0, sizeof(events));
    CHECK(run_pair(&fixture, "rich_contract", &argument, 1,
        SOL_INTERPRETER_CONTRACTS_CHECK, (SolInterpreterLimits){0},
        NULL, NULL, &owning, &mir, &left, &right, &events, 1024));
    sol_interpreter_result_free(&owning);
    sol_mir_evaluate_result_free(&mir);
    argument.as.integer = 0;
    memset(&left, 0, sizeof(left)); memset(&right, 0, sizeof(right));
    memset(&events, 0, sizeof(events));
    (void)run_pair(&fixture, "rich_contract", &argument, 1,
        SOL_INTERPRETER_CONTRACTS_CHECK, (SolInterpreterLimits){0},
        NULL, NULL, &owning, &mir, &left, &right, &events, 1024);
    sol_interpreter_result_free(&owning);
    sol_mir_evaluate_result_free(&mir);
    memset(&left, 0, sizeof(left)); memset(&right, 0, sizeof(right));
    memset(&events, 0, sizeof(events));
    CHECK(run_pair(&fixture, "rich_contract", &argument, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &owning, &mir, &left, &right, &events, 1024));
    sol_interpreter_result_free(&owning);
    sol_mir_evaluate_result_free(&mir);
    sol_interpreter_value_free(&argument);
    fixture_free(&fixture);
}

static void test_trace_limits_and_requests(void) {
    Fixture fixture;
    CHECK(fixture_compile(&fixture,
        "module mir_eval_trace\nfunction value() -> Int64 { return 1 + 2 }\n"));
    SolIrCallableId callable = find_callable(&fixture.ir, "value",
        SOL_IR_CALLABLE_FUNCTION);
    Trace first = {0};
    SolMirEvaluateRequest request = {0};
    request.ir = &fixture.ir;
    request.entry = &fixture.mirs[callable];
    request.contracts = SOL_INTERPRETER_CONTRACTS_IGNORE;
    request.trace_events = 3;
    request.callee_provider = provide;
    request.callee_context = &fixture;
    request.trace_observer = trace;
    request.trace_context = &first;
    SolMirEvaluateResult result;
    CHECK(sol_mir_evaluate(&request, &result));
    CHECK(result.trace_count == 3 && result.trace_truncated);
    CHECK(first.count == 3);
    SolMirTraceEvent saved_events[3];
    memcpy(saved_events, first.events, sizeof(saved_events));
    CHECK(first.events[0].kind == SOL_MIR_TRACE_CALL);
    CHECK(first.events[0].block == SOL_MIR_NONE);
    CHECK(first.events[0].instruction == SOL_MIR_NONE);
    CHECK(first.events[0].instruction_kind
        == SOL_MIR_TRACE_INSTRUCTION_KIND_NONE);
    sol_mir_evaluate_result_free(&result);
    Trace repeated = {0};
    request.trace_context = &repeated;
    CHECK(sol_mir_evaluate(&request, &result));
    CHECK(repeated.count == 3
        && memcmp(saved_events, repeated.events, sizeof(saved_events)) == 0);
    sol_mir_evaluate_result_free(&result);
    request.trace_context = &first;
    request.limits = (SolInterpreterLimits){1, 100, 100, 100, 100};
    request.trace_events = 100;
    first.count = 0;
    CHECK(!sol_mir_evaluate(&request, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_STEP_LIMIT);
    sol_mir_evaluate_result_free(&result);
    request.trace_observer = NULL;
    CHECK(!sol_mir_evaluate(NULL, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST);
    sol_mir_evaluate_result_free(&result);
    SolMir saved = fixture.mirs[callable];
    fixture.mirs[callable].callable = fixture.ir.callable_count;
    CHECK(!sol_mir_evaluate(&request, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_IR);
    sol_mir_evaluate_result_free(&result);
    fixture.mirs[callable] = saved;
    fixture_free(&fixture);
}

static void test_bodyful_capability_rejection(void) {
    Fixture fixture;
    CHECK(fixture_compile(&fixture,
        "module mir_eval_capability_closure\n"
        "capability Source { function read() -> Int64 effects { pure } }\n"
        "capability Derived derives_from source: capability Source { "
            "function read() -> Int64 effects { pure } { return source.read() } }\n"
        "function call(source: capability Derived) -> Int64 effects { pure } "
            "{ return source.read() }\n"));
    SolIrDefinitionId source_definition = SOL_IR_NONE;
    SolIrDefinitionId derived_definition = SOL_IR_NONE;
    for (size_t index = 0; index < fixture.ir.definition_count; ++index) {
        if (strcmp(fixture.ir.definitions[index].name, "Source") == 0) {
            source_definition = index;
        } else if (strcmp(fixture.ir.definitions[index].name, "Derived") == 0) {
            derived_definition = index;
        }
    }
    int root;
    SolInterpreterValue source;
    SolInterpreterValue argument;
    CHECK(sol_interpreter_value_capability(&source, source_definition,
        &root, NULL));
    CHECK(sol_interpreter_value_capability(&argument, derived_definition,
        &root, &source));
    SolIrCallableId callable = find_callable(&fixture.ir, "call",
        SOL_IR_CALLABLE_FUNCTION);
    SolMirEvaluateRequest request = {0};
    request.ir = &fixture.ir;
    request.entry = &fixture.mirs[callable];
    request.arguments = &argument;
    request.argument_count = 1;
    request.contracts = SOL_INTERPRETER_CONTRACTS_IGNORE;
    request.callee_provider = provide;
    request.callee_context = &fixture;
    SolMirEvaluateResult result;
    CHECK(!sol_mir_evaluate(&request, &result));
    if (result.diagnostic.code != SOL_INTERPRETER_INVALID_REQUEST) {
        fprintf(stderr, "bodyful rejection code=%d message=%s\n",
            (int)result.diagnostic.code, result.diagnostic.message);
    }
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST);
    CHECK(strstr(result.diagnostic.message, "bodyful capability") != NULL);
    CHECK(result.executed.calls == 0);
    sol_mir_evaluate_result_free(&result);
    sol_interpreter_value_free(&argument);
    sol_interpreter_value_free(&source);
    fixture_free(&fixture);
}

typedef struct {
    SolIrCallableId operations[8];
    size_t count;
} HostLog;

static bool host_value(void *context, const SolIr *ir,
    SolIrCallableId operation, void *root,
    const SolInterpreterValue *private_source,
    const SolInterpreterValue *arguments, size_t argument_count,
    SolInterpreterValue *result, SolInterpreterHostFailure *failure) {
    HostLog *log = context;
    (void)root;
    (void)private_source;
    (void)arguments;
    (void)argument_count;
    (void)failure;
    if (log->count < 8) log->operations[log->count++] = operation;
    if (argument_count == 1
        && arguments[0].kind == SOL_INTERPRETER_VALUE_INT64) {
        return sol_interpreter_value_int64(result, arguments[0].as.integer);
    }
    const char *owner = ir->definitions[ir->callables[operation].owner].name;
    return sol_interpreter_value_int64(result,
        strcmp(owner, "Provider") == 0 ? 99 : 41);
}

static bool deny_host(void *context, const SolIr *ir,
    SolIrCallableId operation, void *root) {
    (void)context;
    (void)ir;
    (void)operation;
    (void)root;
    return false;
}

static bool malformed_host(void *context, const SolIr *ir,
    SolIrCallableId operation, void *root,
    const SolInterpreterValue *private_source,
    const SolInterpreterValue *arguments, size_t argument_count,
    SolInterpreterValue *result, SolInterpreterHostFailure *failure) {
    HostLog *log = context;
    (void)ir;
    (void)root;
    (void)private_source;
    (void)arguments;
    (void)argument_count;
    (void)failure;
    if (log->count < 8) log->operations[log->count++] = operation;
    result->kind = SOL_INTERPRETER_VALUE_OPTION;
    result->as.sum.has_value = true;
    result->as.sum.is_error = false;
    result->as.sum.value = NULL;
    return true;
}

static bool failed_host(void *context, const SolIr *ir,
    SolIrCallableId operation, void *root,
    const SolInterpreterValue *private_source,
    const SolInterpreterValue *arguments, size_t argument_count,
    SolInterpreterValue *result, SolInterpreterHostFailure *failure) {
    HostLog *log = context;
    (void)ir;
    (void)root;
    (void)private_source;
    (void)arguments;
    (void)argument_count;
    (void)result;
    if (log->count < 8) log->operations[log->count++] = operation;
    memcpy(failure->bytes, "host failed", 11);
    failure->length = 11;
    return false;
}

static void test_calls_places_methods_and_host(void) {
    Fixture fixture;
    bool compiled = fixture_compile(&fixture,
        "module mir_eval_extended\n"
        "record Pair { left: Int64, right: Int64 }\n"
        "record Box<T> { value: T }\n"
        "record Holder { left: Box<Int64>, right: Box<Int64> }\n"
        "trait Bump { function bump(self: inout Self) -> () effects { pure } }\n"
        "implementation Bump for Int64 { function bump(self: inout Self) -> () "
            "effects { pure } { self += 3 } }\n"
        "trait Read { function read(self: Self) -> Int64 effects { pure } }\n"
        "implementation Read for Int64 { function read(self: Self) -> Int64 "
            "effects { pure } { return self } }\n"
        "implementation Read for Bool { function read(self: Self) -> Int64 "
            "effects { pure } { return if self { 1 } else { 0 } } }\n"
        "function generic_read<T: Read>(value: T) -> Int64 effects { pure } "
            "{ return value.read() }\n"
        "function generic_call() -> Int64 effects { pure } "
            "{ return generic_read<Int64>(8) }\n"
        "function generic_both() -> Int64 effects { pure } "
            "{ return generic_read<Int64>(8) + generic_read<Bool>(true) }\n"
        "function increment(value: Int64) -> Int64 effects { pure } "
            "{ return value + 1 }\n"
        "function callback_entry(callback: function(Int64) -> Int64 effects { pure }, "
            "value: Int64) -> Int64 effects { pure } { return callback(value) }\n"
        "function impure_callback(value: Int64) -> Int64 effects { diverge } "
            "{ return value }\n"
        "function fail_callback() -> Int64 effects { panic } { panic \"callback boom\" }\n"
        "function panic_entry(callback: function() -> Int64 effects { panic }) "
            "-> Int64 effects { panic } { return callback() }\n"
        "record CallbackBox { callback: function(Int64) -> Int64 effects { pure } }\n"
        "function ignore_callback(box: CallbackBox) -> Int64 { return 1 }\n"
        "function set(value: inout Int64) -> () { value = 7 }\n"
        "function writeback() -> Int64 { var pair = Pair { left = 1, right = 2 } "
            "set(pair.left) pair.right.bump() return pair.left + pair.right }\n"
        "function partial(value: Holder) -> Int64 { let moved = value.left "
            "return value.right.value }\n"
        "capability Source { function read() -> Int64 "
            "effects { service.read<Self> } }\n"
        "capability Provider { function read() -> Int64 effects { pure } }\n"
        "capability CallbackHost { function load() -> "
            "function(Int64) -> Int64 effects { pure } effects { pure } }\n"
        "capability ContractHost { function echo(value: Int64) -> Int64 "
            "effects { pure } requires { true } "
            "ensures { result == 7 } }\n"
        "capability Derived derives_from source: capability Source {}\n"
        "function make_derived(source: capability Source) -> capability Derived "
            "effects { pure } { return Derived { source = source } }\n"
        "function tuple_result(flag: Bool) -> (Result<Int64, Text>, Bool) { "
            "return if flag { (ok(3), true) } else { (err(\"bad\"), false) } }\n"
        "function hosted(source: capability Source) -> Int64 "
            "effects { service.read<source> } { return source.read() }\n"
        "function contracted_hosted(host: capability ContractHost) -> Int64 "
            "effects { pure } { return host.echo(7) }\n"
        "function hosted_callback(host: capability CallbackHost) -> Int64 "
            "effects { pure } { let callback = host.load() return callback(1) }\n"
        "function invoke_factory(factory: function() -> function(Int64) -> Int64 "
            "effects { pure } effects { pure }) -> Int64 effects { pure } { "
            "let callback = factory() return callback(1) }\n"
        "function handled(source: capability Source, provider: capability Provider) "
            "-> Int64 effects { pure } { return handle service.read<source> "
            "with provider { source.read() } }\n");
    CHECK(compiled);
    if (!compiled) {
        sol_diagnostics_render_human(stderr, &fixture.source,
            &fixture.diagnostics);
        fixture_free(&fixture);
        return;
    }
    const char *plain[] = {"generic_call", "generic_both", "writeback"};
    for (size_t index = 0; index < 3; ++index) {
        SolInterpreterResult owning; SolMirEvaluateResult mir;
        Cleanup left = {0}; Cleanup right = {0}; Trace events = {0};
        CHECK(run_pair(&fixture, plain[index], NULL, 0,
            SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
            NULL, NULL, &owning, &mir, &left, &right, &events, 1024));
        sol_interpreter_result_free(&owning);
        sol_mir_evaluate_result_free(&mir);
    }
    SolInterpreterResult owning; SolMirEvaluateResult mir;
    Cleanup left = {0}; Cleanup right = {0}; Trace events = {0};
    SolIrCallableId increment = find_callable(&fixture.ir, "increment",
        SOL_IR_CALLABLE_FUNCTION);
    SolInterpreterValue callback_arguments[2];
    sol_interpreter_value_init(&callback_arguments[0]);
    callback_arguments[0].kind = SOL_INTERPRETER_VALUE_FUNCTION;
    callback_arguments[0].as.callable.callable = increment;
    sol_interpreter_value_int64(&callback_arguments[1], 10);
    memset(&left, 0, sizeof(left)); memset(&right, 0, sizeof(right));
    memset(&events, 0, sizeof(events));
    CHECK(run_pair(&fixture, "callback_entry", callback_arguments, 2,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &owning, &mir, &left, &right, &events, 1024));
    sol_interpreter_result_free(&owning);
    sol_mir_evaluate_result_free(&mir);
    sol_interpreter_value_free(&callback_arguments[0]);
    sol_interpreter_value_free(&callback_arguments[1]);

    SolIrCallableId impure = find_callable(&fixture.ir, "impure_callback",
        SOL_IR_CALLABLE_FUNCTION);
    sol_interpreter_value_init(&callback_arguments[0]);
    callback_arguments[0].kind = SOL_INTERPRETER_VALUE_FUNCTION;
    callback_arguments[0].as.callable.callable = impure;
    sol_interpreter_value_int64(&callback_arguments[1], 10);
    memset(&left, 0, sizeof(left)); memset(&right, 0, sizeof(right));
    memset(&events, 0, sizeof(events));
    CHECK(!run_pair(&fixture, "callback_entry", callback_arguments, 2,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &owning, &mir, &left, &right, &events, 1024));
    CHECK(mir.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST);
    sol_interpreter_result_free(&owning);
    sol_mir_evaluate_result_free(&mir);
    sol_interpreter_value_free(&callback_arguments[0]);
    sol_interpreter_value_free(&callback_arguments[1]);

    SolIrCallableId fail_callback = find_callable(&fixture.ir, "fail_callback",
        SOL_IR_CALLABLE_FUNCTION);
    SolInterpreterValue failing;
    sol_interpreter_value_init(&failing);
    failing.kind = SOL_INTERPRETER_VALUE_FUNCTION;
    failing.as.callable.callable = fail_callback;
    memset(&left, 0, sizeof(left)); memset(&right, 0, sizeof(right));
    memset(&events, 0, sizeof(events));
    CHECK(!run_pair(&fixture, "panic_entry", &failing, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &owning, &mir, &left, &right, &events, 1024));
    CHECK(mir.diagnostic.code == SOL_INTERPRETER_PANIC);
    sol_interpreter_result_free(&owning);
    sol_mir_evaluate_result_free(&mir);
    sol_interpreter_value_free(&failing);

    SolIrDefinitionId box = SOL_IR_NONE;
    SolIrDefinitionId holder = SOL_IR_NONE;
    SolIrDefinitionId callback_box = SOL_IR_NONE;
    SolIrDefinitionId source_definition = SOL_IR_NONE;
    SolIrDefinitionId provider_definition = SOL_IR_NONE;
    SolIrDefinitionId contract_host_definition = SOL_IR_NONE;
    SolIrDefinitionId callback_host_definition = SOL_IR_NONE;
    for (size_t index = 0; index < fixture.ir.definition_count; ++index) {
        const char *name = fixture.ir.definitions[index].name;
        if (strcmp(name, "Box") == 0) box = index;
        else if (strcmp(name, "Holder") == 0) holder = index;
        else if (strcmp(name, "CallbackBox") == 0) callback_box = index;
        else if (strcmp(name, "Source") == 0) source_definition = index;
        else if (strcmp(name, "Provider") == 0) provider_definition = index;
        else if (strcmp(name, "ContractHost") == 0) {
            contract_host_definition = index;
        } else if (strcmp(name, "CallbackHost") == 0) {
            callback_host_definition = index;
        }
    }
    SolInterpreterValue callback_field;
    sol_interpreter_value_init(&callback_field);
    callback_field.kind = SOL_INTERPRETER_VALUE_FUNCTION;
    callback_field.as.callable.callable = increment;
    SolInterpreterValue callback_box_value = {
        .kind = SOL_INTERPRETER_VALUE_RECORD,
        .as.aggregate = {callback_box, SOL_IR_NONE, &callback_field, 1},
    };
    SolIrCallableId ignore_callback = find_callable(&fixture.ir,
        "ignore_callback", SOL_IR_CALLABLE_FUNCTION);
    SolMirEvaluateRequest callback_request = {0};
    callback_request.ir = &fixture.ir;
    callback_request.entry = &fixture.mirs[ignore_callback];
    callback_request.arguments = &callback_box_value;
    callback_request.argument_count = 1;
    callback_request.contracts = SOL_INTERPRETER_CONTRACTS_IGNORE;
    callback_request.callee_provider = unavailable_provider;
    SolMirEvaluateResult callback_result;
    CHECK(!sol_mir_evaluate(&callback_request, &callback_result));
    CHECK(callback_result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST);
    CHECK(callback_result.executed.calls == 0);
    sol_mir_evaluate_result_free(&callback_result);
    SolInterpreterValue callback_host;
    int callback_host_root;
    CHECK(sol_interpreter_value_capability(&callback_host,
        callback_host_definition, &callback_host_root, NULL));
    SolIrCallableId hosted_callback = find_callable(&fixture.ir,
        "hosted_callback", SOL_IR_CALLABLE_FUNCTION);
    HostLog callback_host_log = {0};
    SolMirEvaluateRequest hosted_callback_request = {0};
    hosted_callback_request.ir = &fixture.ir;
    hosted_callback_request.entry = &fixture.mirs[hosted_callback];
    hosted_callback_request.arguments = &callback_host;
    hosted_callback_request.argument_count = 1;
    hosted_callback_request.contracts = SOL_INTERPRETER_CONTRACTS_IGNORE;
    hosted_callback_request.host_operation = host_value;
    hosted_callback_request.host_context = &callback_host_log;
    hosted_callback_request.callee_provider = provide;
    hosted_callback_request.callee_context = &fixture;
    CHECK(!sol_mir_evaluate(&hosted_callback_request, &callback_result));
    CHECK(callback_result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST);
    CHECK(callback_result.executed.calls == 0);
    CHECK(callback_host_log.count == 0);
    sol_mir_evaluate_result_free(&callback_result);
    SolIrCallableId load = find_callable(&fixture.ir, "load",
        SOL_IR_CALLABLE_CAPABILITY);
    SolInterpreterValue factory;
    sol_interpreter_value_init(&factory);
    factory.kind = SOL_INTERPRETER_VALUE_BOUND_OPERATION;
    factory.as.callable.callable = load;
    factory.as.callable.receiver = malloc(sizeof(*factory.as.callable.receiver));
    CHECK(factory.as.callable.receiver != NULL);
    if (factory.as.callable.receiver != NULL) {
        CHECK(sol_interpreter_value_clone(factory.as.callable.receiver,
            &callback_host));
        SolIrCallableId invoke_factory = find_callable(&fixture.ir,
            "invoke_factory", SOL_IR_CALLABLE_FUNCTION);
        SolMirEvaluateRequest factory_request = {0};
        factory_request.ir = &fixture.ir;
        factory_request.entry = &fixture.mirs[invoke_factory];
        factory_request.arguments = &factory;
        factory_request.argument_count = 1;
        factory_request.contracts = SOL_INTERPRETER_CONTRACTS_IGNORE;
        factory_request.host_operation = host_value;
        factory_request.host_context = &callback_host_log;
        factory_request.callee_provider = provide;
        factory_request.callee_context = &fixture;
        memset(&callback_host_log, 0, sizeof(callback_host_log));
        CHECK(!sol_mir_evaluate(&factory_request, &callback_result));
        CHECK(callback_result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST);
        CHECK(callback_result.executed.calls == 0);
        CHECK(callback_host_log.count == 0);
        sol_mir_evaluate_result_free(&callback_result);
    }
    sol_interpreter_value_free(&factory);
    sol_interpreter_value_free(&callback_host);

    SolIrCallableId bool_read = SOL_IR_NONE;
    for (size_t index = 0; index < fixture.ir.callable_count; ++index) {
        const SolIrCallable *candidate = &fixture.ir.callables[index];
        if (candidate->kind != SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION
            || candidate->owner >= fixture.ir.definition_count) continue;
        SolIrTypeId target
            = fixture.ir.definitions[candidate->owner].implementation_target;
        if (target < fixture.ir.type_count
            && fixture.ir.types[target].kind == SOL_IR_TYPE_BOOL) {
            bool_read = index;
        }
    }
    CHECK(bool_read != SOL_IR_NONE);
    SolIrCallableId generic_call = find_callable(&fixture.ir, "generic_call",
        SOL_IR_CALLABLE_FUNCTION);
    ProviderLog selected_log = {
        .fixture = &fixture,
        .unavailable = bool_read,
    };
    SolMirEvaluateRequest selected_request = {0};
    selected_request.ir = &fixture.ir;
    selected_request.entry = &fixture.mirs[generic_call];
    selected_request.contracts = SOL_INTERPRETER_CONTRACTS_IGNORE;
    selected_request.callee_provider = counting_provider;
    selected_request.callee_context = &selected_log;
    CHECK(sol_mir_evaluate(&selected_request, &callback_result));
    CHECK(callback_result.value.kind == SOL_INTERPRETER_VALUE_INT64
        && callback_result.value.as.integer == 8);
    CHECK(bool_read >= 256 || selected_log.counts[bool_read] == 0);
    sol_mir_evaluate_result_free(&callback_result);
    SolInterpreterValue holder_fields[2];
    SolInterpreterValue box_fields[2];
    SolInterpreterValue box_values[2];
    sol_interpreter_value_int64(&box_values[0], 3);
    sol_interpreter_value_int64(&box_values[1], 9);
    for (size_t index = 0; index < 2; ++index) {
        box_fields[index].kind = SOL_INTERPRETER_VALUE_RECORD;
        box_fields[index].as.aggregate.definition = box;
        box_fields[index].as.aggregate.variant = SOL_IR_NONE;
        box_fields[index].as.aggregate.fields = &box_values[index];
        box_fields[index].as.aggregate.field_count = 1;
        holder_fields[index] = box_fields[index];
    }
    SolInterpreterValue holder_value = {
        .kind = SOL_INTERPRETER_VALUE_RECORD,
        .as.aggregate = {holder, SOL_IR_NONE, holder_fields, 2},
    };
    memset(&left, 0, sizeof(left)); memset(&right, 0, sizeof(right));
    memset(&events, 0, sizeof(events));
    CHECK(run_pair(&fixture, "partial", &holder_value, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &owning, &mir, &left, &right, &events, 1024));
    sol_interpreter_result_free(&owning);
    sol_mir_evaluate_result_free(&mir);


    int source_root;
    int provider_root;
    SolInterpreterValue capabilities[2];
    CHECK(sol_interpreter_value_capability(&capabilities[0], source_definition,
        &source_root, NULL));
    CHECK(sol_interpreter_value_capability(&capabilities[1], provider_definition,
        &provider_root, NULL));
    memset(&left, 0, sizeof(left)); memset(&right, 0, sizeof(right));
    memset(&events, 0, sizeof(events));
    CHECK(run_pair(&fixture, "make_derived", capabilities, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &owning, &mir, &left, &right, &events, 1024));
    sol_interpreter_result_free(&owning);
    sol_mir_evaluate_result_free(&mir);
    SolInterpreterValue tuple_flag;
    sol_interpreter_value_bool(&tuple_flag, false);
    memset(&left, 0, sizeof(left)); memset(&right, 0, sizeof(right));
    memset(&events, 0, sizeof(events));
    CHECK(run_pair(&fixture, "tuple_result", &tuple_flag, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        NULL, NULL, &owning, &mir, &left, &right, &events, 1024));
    sol_interpreter_result_free(&owning);
    sol_mir_evaluate_result_free(&mir);
    sol_interpreter_value_free(&tuple_flag);
    HostLog host = {0};
    memset(&left, 0, sizeof(left)); memset(&right, 0, sizeof(right));
    memset(&events, 0, sizeof(events));
    CHECK(run_pair(&fixture, "hosted", capabilities, 1,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        host_value, &host, &owning, &mir, &left, &right, &events, 1024));
    CHECK(host.count == 2 && host.operations[0] == host.operations[1]);
    bool traced_operation = false;
    for (size_t index = 0; index < events.count; ++index) {
        if (events.events[index].kind != SOL_MIR_TRACE_HOST) continue;
        traced_operation = true;
        CHECK(events.events[index].operation == host.operations[1]);
    }
    CHECK(traced_operation);
    sol_interpreter_result_free(&owning);
    sol_mir_evaluate_result_free(&mir);
    SolIrCallableId hosted_callable = find_callable(&fixture.ir, "hosted",
        SOL_IR_CALLABLE_FUNCTION);
    SolMirEvaluateRequest host_request = {0};
    host_request.ir = &fixture.ir;
    host_request.entry = &fixture.mirs[hosted_callable];
    host_request.arguments = capabilities;
    host_request.argument_count = 1;
    host_request.contracts = SOL_INTERPRETER_CONTRACTS_IGNORE;
    host_request.host_operation = host_value;
    host_request.host_allows = deny_host;
    host_request.host_context = &host;
    host_request.callee_provider = provide;
    host_request.callee_context = &fixture;
    memset(&host, 0, sizeof(host));
    CHECK(!sol_mir_evaluate(&host_request, &mir));
    CHECK(mir.diagnostic.code == SOL_INTERPRETER_UNBOUND_OPERATION);
    CHECK(host.count == 0 && mir.used.host_calls == 0);
    sol_mir_evaluate_result_free(&mir);
    host_request.host_allows = NULL;
    host_request.host_operation = malformed_host;
    memset(&host, 0, sizeof(host));
    CHECK(!sol_mir_evaluate(&host_request, &mir));
    CHECK(mir.diagnostic.code == SOL_INTERPRETER_TYPE_INVARIANT);
    CHECK(host.count == 1);
    sol_mir_evaluate_result_free(&mir);
    host_request.host_operation = failed_host;
    memset(&host, 0, sizeof(host));
    CHECK(!sol_mir_evaluate(&host_request, &mir));
    CHECK(mir.diagnostic.code == SOL_INTERPRETER_HOST_ERROR);
    CHECK(strcmp(mir.diagnostic.message, "host failed") == 0);
    CHECK(host.count == 1);
    sol_mir_evaluate_result_free(&mir);
    SolInterpreterValue contract_host;
    int contract_host_root;
    CHECK(sol_interpreter_value_capability(&contract_host,
        contract_host_definition, &contract_host_root, NULL));
    memset(&host, 0, sizeof(host));
    memset(&left, 0, sizeof(left)); memset(&right, 0, sizeof(right));
    memset(&events, 0, sizeof(events));
    CHECK(run_pair(&fixture, "contracted_hosted", &contract_host, 1,
        SOL_INTERPRETER_CONTRACTS_CHECK, (SolInterpreterLimits){0},
        host_value, &host, &owning, &mir, &left, &right, &events, 1024));
    CHECK(host.count == 2 && host.operations[0] == host.operations[1]);
    sol_interpreter_result_free(&owning);
    sol_mir_evaluate_result_free(&mir);
    sol_interpreter_value_free(&contract_host);
    memset(&host, 0, sizeof(host));
    memset(&left, 0, sizeof(left)); memset(&right, 0, sizeof(right));
    memset(&events, 0, sizeof(events));
    CHECK(run_pair(&fixture, "handled", capabilities, 2,
        SOL_INTERPRETER_CONTRACTS_IGNORE, (SolInterpreterLimits){0},
        host_value, &host, &owning, &mir, &left, &right, &events, 1024));
    CHECK(host.count == 2 && host.operations[0] == host.operations[1]);
    sol_interpreter_result_free(&owning);
    sol_mir_evaluate_result_free(&mir);
    sol_interpreter_value_free(&capabilities[0]);
    sol_interpreter_value_free(&capabilities[1]);
    fixture_free(&fixture);
}

static SolMirCalleeStatus unavailable_provider(void *context,
    SolIrCallableId callable, const SolMir **mir) {
    (void)context;
    (void)callable;
    *mir = NULL;
    return SOL_MIR_CALLEE_UNAVAILABLE;
}

static SolMirCalleeStatus mismatched_provider(void *context,
    SolIrCallableId callable, const SolMir **mir) {
    Fixture *fixture = context;
    (void)callable;
    *mir = &fixture->mirs[find_callable(&fixture->ir, "caller",
        SOL_IR_CALLABLE_FUNCTION)];
    return SOL_MIR_CALLEE_FOUND;
}

typedef struct {
    Fixture *fixture;
    SolMir scratch;
} AliasingProvider;

static SolMirCalleeStatus aliasing_provider(void *context,
    SolIrCallableId callable, const SolMir **mir) {
    AliasingProvider *provider = context;
    if (callable >= provider->fixture->ir.callable_count
        || provider->fixture->outcomes[callable] != SOL_MIR_LOWER_SUCCEEDED) {
        *mir = NULL;
        return SOL_MIR_CALLEE_UNAVAILABLE;
    }
    provider->scratch = provider->fixture->mirs[callable];
    *mir = &provider->scratch;
    return SOL_MIR_CALLEE_FOUND;
}

static void test_resource_limits_and_provider_failures(void) {
    Fixture fixture;
    bool compiled = fixture_compile(&fixture,
        "module mir_eval_limits\n"
        "capability Host { function read() -> Int64 effects { host.read<Self> } }\n"
        "function leaf() -> Int64 { return 3 }\n"
        "function other() -> Int64 { return 3 }\n"
        "function caller() -> Int64 { return leaf() + other() }\n"
        "function recurse(value: Int64) -> Int64 effects { diverge } { "
            "return if value == 0 { 0 } else { recurse(value - 1) } }\n"
        "function values() -> Int64 { return 1 + 2 }\n"
        "function text() -> Text { return \"abcd\" }\n"
        "function consume(first: Text, second: Text) -> Text { return first }\n"
        "function option_identity(value: Option<Int64>) -> Option<Int64> { "
            "return value }\n"
        "function updated() -> Int64 { var value = 1 value += 2 return value }\n"
        "function patterned(value: Bool) -> Int64 { return match value { "
            "true => 1, false => 0 } }\n"
        "function hosted(host: capability Host) -> Int64 "
            "effects { host.read<host> } { return host.read() }\n"
        "function endless() -> () effects { diverge } { let item = \"x\" loop {} }\n");
    CHECK(compiled);
    if (!compiled) {
        fixture_free(&fixture);
        return;
    }
    struct LimitCase {
        const char *name;
        SolInterpreterLimits limits;
        SolInterpreterCode code;
    } cases[] = {
        {"endless", {2, 100, 100, 100, 100}, SOL_INTERPRETER_STEP_LIMIT},
        {"values", {100, 100, 1, 100, 100}, SOL_INTERPRETER_VALUE_LIMIT},
        {"text", {100, 100, 100, 3, 100}, SOL_INTERPRETER_TEXT_LIMIT},
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        SolIrCallableId callable = find_callable(&fixture.ir, cases[index].name,
            SOL_IR_CALLABLE_FUNCTION);
        Cleanup cleanup_log = {0};
        Trace events = {0};
        SolMirEvaluateRequest request = {0};
        request.ir = &fixture.ir;
        request.entry = &fixture.mirs[callable];
        request.contracts = SOL_INTERPRETER_CONTRACTS_IGNORE;
        request.limits = cases[index].limits;
        request.trace_events = 100;
        request.callee_provider = provide;
        request.callee_context = &fixture;
        request.cleanup_observer = cleanup;
        request.cleanup_context = &cleanup_log;
        request.trace_observer = trace;
        request.trace_context = &events;
        SolMirEvaluateResult result;
        CHECK(!sol_mir_evaluate(&request, &result));
        CHECK(result.diagnostic.code == cases[index].code);
        CHECK(events.count != 0
            && events.events[events.count - 1].kind == SOL_MIR_TRACE_FAILURE);
        sol_mir_evaluate_result_free(&result);
    }
    SolInterpreterValue pattern_argument;
    sol_interpreter_value_bool(&pattern_argument, true);
    const char *deterministic_names[] = {"updated", "patterned"};
    const SolInterpreterValue *deterministic_arguments[] = {
        NULL, &pattern_argument
    };
    const size_t deterministic_argument_counts[] = {0, 1};
    const size_t deterministic_cutoffs[] = {3, 3};
    for (size_t index = 0; index < 2; ++index) {
        SolIrCallableId callable = find_callable(&fixture.ir,
            deterministic_names[index], SOL_IR_CALLABLE_FUNCTION);
        SolMirEvaluateRequest request = {0};
        request.ir = &fixture.ir;
        request.entry = &fixture.mirs[callable];
        request.arguments = deterministic_arguments[index];
        request.argument_count = deterministic_argument_counts[index];
        request.contracts = SOL_INTERPRETER_CONTRACTS_IGNORE;
        request.limits = (SolInterpreterLimits){1000, 100, 1000, 1000, 100};
        request.callee_provider = provide;
        request.callee_context = &fixture;
        SolMirEvaluateResult result;
        CHECK(sol_mir_evaluate(&request, &result));
        size_t cutoff = result.used.value_nodes;
        CHECK(cutoff == deterministic_cutoffs[index]);
        sol_mir_evaluate_result_free(&result);
        request.limits.value_nodes = cutoff;
        CHECK(sol_mir_evaluate(&request, &result));
        CHECK(result.used.value_nodes == cutoff);
        sol_mir_evaluate_result_free(&result);
        request.limits.value_nodes = cutoff - 1;
        CHECK(!sol_mir_evaluate(&request, &result));
        CHECK(result.diagnostic.code == SOL_INTERPRETER_VALUE_LIMIT);
        sol_mir_evaluate_result_free(&result);
    }
    sol_interpreter_value_free(&pattern_argument);
    SolInterpreterValue recurse_argument;
    sol_interpreter_value_int64(&recurse_argument, 3);
    SolIrCallableId recurse = find_callable(&fixture.ir, "recurse",
        SOL_IR_CALLABLE_FUNCTION);
    SolMirEvaluateRequest recurse_request = {0};
    recurse_request.ir = &fixture.ir;
    recurse_request.entry = &fixture.mirs[recurse];
    recurse_request.arguments = &recurse_argument;
    recurse_request.argument_count = 1;
    recurse_request.contracts = SOL_INTERPRETER_CONTRACTS_IGNORE;
    recurse_request.limits = (SolInterpreterLimits){1000, 1, 1000, 1000, 100};
    recurse_request.callee_provider = provide;
    recurse_request.callee_context = &fixture;
    SolMirEvaluateResult result;
    CHECK(!sol_mir_evaluate(&recurse_request, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_CALL_DEPTH_LIMIT);
    sol_mir_evaluate_result_free(&result);
    sol_interpreter_value_free(&recurse_argument);

    SolInterpreterValue text_arguments[2];
    CHECK(sol_interpreter_value_text(&text_arguments[0], "a", 1));
    CHECK(sol_interpreter_value_text(&text_arguments[1], "b", 1));
    SolIrCallableId consume = find_callable(&fixture.ir, "consume",
        SOL_IR_CALLABLE_FUNCTION);
    Cleanup argument_cleanup = {0};
    SolMirEvaluateRequest argument_request = {0};
    argument_request.ir = &fixture.ir;
    argument_request.entry = &fixture.mirs[consume];
    argument_request.arguments = text_arguments;
    argument_request.argument_count = 2;
    argument_request.contracts = SOL_INTERPRETER_CONTRACTS_IGNORE;
    argument_request.limits = (SolInterpreterLimits){100, 100, 1, 100, 100};
    argument_request.callee_provider = provide;
    argument_request.callee_context = &fixture;
    argument_request.cleanup_observer = cleanup;
    argument_request.cleanup_context = &argument_cleanup;
    CHECK(!sol_mir_evaluate(&argument_request, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_VALUE_LIMIT);
    CHECK(argument_cleanup.count == 1);
    CHECK(argument_cleanup.locals[0]
        == fixture.ir.roots[fixture.ir.callables[consume].parameters.offset]);
    sol_mir_evaluate_result_free(&result);
    sol_interpreter_value_free(&text_arguments[0]);
    sol_interpreter_value_free(&text_arguments[1]);

    SolInterpreterValue cyclic;
    sol_interpreter_value_init(&cyclic);
    cyclic.kind = SOL_INTERPRETER_VALUE_OPTION;
    cyclic.as.sum.has_value = true;
    cyclic.as.sum.is_error = false;
    cyclic.as.sum.value = &cyclic;
    SolIrCallableId option_identity = find_callable(&fixture.ir,
        "option_identity", SOL_IR_CALLABLE_FUNCTION);
    SolMirEvaluateRequest malformed_request = {0};
    malformed_request.ir = &fixture.ir;
    malformed_request.entry = &fixture.mirs[option_identity];
    malformed_request.arguments = &cyclic;
    malformed_request.argument_count = 1;
    malformed_request.contracts = SOL_INTERPRETER_CONTRACTS_IGNORE;
    malformed_request.limits = (SolInterpreterLimits){100, 100, 1, 1, 100};
    malformed_request.callee_provider = provide;
    malformed_request.callee_context = &fixture;
    CHECK(!sol_mir_evaluate(&malformed_request, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST);
    CHECK(result.executed.calls == 0);
    sol_mir_evaluate_result_free(&result);

    SolIrDefinitionId host_definition = SOL_IR_NONE;
    for (size_t index = 0; index < fixture.ir.definition_count; ++index) {
        if (strcmp(fixture.ir.definitions[index].name, "Host") == 0) {
            host_definition = index;
        }
    }
    int root;
    SolInterpreterValue host_argument;
    CHECK(sol_interpreter_value_capability(&host_argument, host_definition,
        &root, NULL));
    SolIrCallableId hosted = find_callable(&fixture.ir, "hosted",
        SOL_IR_CALLABLE_FUNCTION);
    SolMirEvaluateRequest host_request = {0};
    host_request.ir = &fixture.ir;
    host_request.entry = &fixture.mirs[hosted];
    host_request.arguments = &host_argument;
    host_request.argument_count = 1;
    host_request.contracts = SOL_INTERPRETER_CONTRACTS_IGNORE;
    host_request.limits = (SolInterpreterLimits){100, 100, 100, 100, 0};
    host_request.host_operation = host_value;
    HostLog host_log = {0};
    host_request.host_context = &host_log;
    host_request.callee_provider = provide;
    host_request.callee_context = &fixture;
    CHECK(!sol_mir_evaluate(&host_request, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_HOST_CALL_LIMIT);
    CHECK(host_log.count == 0);
    sol_mir_evaluate_result_free(&result);
    sol_interpreter_value_free(&host_argument);

    SolIrCallableId caller = find_callable(&fixture.ir, "caller",
        SOL_IR_CALLABLE_FUNCTION);
    SolIrCallableId leaf = find_callable(&fixture.ir, "leaf",
        SOL_IR_CALLABLE_FUNCTION);
    SolMirEvaluateRequest provider_request = {0};
    provider_request.ir = &fixture.ir;
    provider_request.entry = &fixture.mirs[caller];
    provider_request.contracts = SOL_INTERPRETER_CONTRACTS_IGNORE;
    ProviderLog provider_log = {
        .fixture = &fixture,
        .unavailable = SOL_IR_NONE,
    };
    provider_request.callee_provider = counting_provider;
    provider_request.callee_context = &provider_log;
    CHECK(sol_mir_evaluate(&provider_request, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_INT64
        && result.value.as.integer == 6);
    CHECK(leaf >= 256 || provider_log.counts[leaf] == 1);
    sol_mir_evaluate_result_free(&result);
    AliasingProvider aliasing = {.fixture = &fixture};
    provider_request.callee_provider = aliasing_provider;
    provider_request.callee_context = &aliasing;
    CHECK(!sol_mir_evaluate(&provider_request, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_IR);
    CHECK(strstr(result.diagnostic.message, "aliased") != NULL);
    CHECK(result.executed.calls == 0);
    sol_mir_evaluate_result_free(&result);
    provider_request.callee_provider = unavailable_provider;
    provider_request.callee_context = NULL;
    CHECK(!sol_mir_evaluate(&provider_request, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST);
    CHECK(result.executed.calls == 0);
    sol_mir_evaluate_result_free(&result);
    provider_request.callee_provider = mismatched_provider;
    provider_request.callee_context = &fixture;
    CHECK(!sol_mir_evaluate(&provider_request, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_IR);
    CHECK(result.executed.calls == 0);
    sol_mir_evaluate_result_free(&result);
    fixture_free(&fixture);
}

int main(void) {
    test_differential_core();
    test_calls_places_methods_and_host();
    test_resource_limits_and_provider_failures();
    test_trace_limits_and_requests();
    test_bodyful_capability_rejection();
    if (failures != 0) fprintf(stderr, "%d MIR evaluator test failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
