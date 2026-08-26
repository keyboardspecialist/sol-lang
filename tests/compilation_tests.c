#include "sol/compilation.h"
#include "sol/interpreter.h"

#include <stdint.h>
#include <stdio.h>
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

static bool has_code(const SolCompilationSession *session, const char *code) {
    size_t count = sol_compilation_diagnostic_count(session);
    for (size_t index = 0; index < count; ++index) {
        SolCompilationDiagnosticView diagnostic;
        if (sol_compilation_diagnostic_at(session, index, &diagnostic)
            && strcmp(diagnostic.code, code) == 0) return true;
    }
    return false;
}

typedef struct {
    bool invoked;
    const SolIr *escaped;
} HostProbe;

typedef struct {
    size_t calls;
    char output[128];
    size_t output_length;
    int64_t argument_count;
    SolHostValue argument_value;
    SolHostValue configuration_value;
    bool fail_console;
} SafeHost;

static bool probe_host(void *context, const SolIr *ir,
    SolIrCallableId operation, void *root,
    const SolInterpreterValue *private_source,
    const SolInterpreterValue *arguments, size_t argument_count,
    SolInterpreterValue *result, SolInterpreterHostFailure *failure) {
    HostProbe *probe = context;
    probe->invoked = true;
    probe->escaped = ir;
    (void)operation;
    (void)root;
    (void)private_source;
    (void)arguments;
    (void)argument_count;
    (void)result;
    (void)failure;
    return false;
}

static bool safe_console(void *context, const SolHostValue *arguments,
    size_t argument_count, SolHostValue *result,
    SolInterpreterHostFailure *failure) {
    SafeHost *host = context;
    ++host->calls;
    if (host->fail_console) {
        static const char message[] = "console unavailable";
        memcpy(failure->bytes, message, sizeof(message) - 1);
        failure->length = sizeof(message) - 1;
        return false;
    }
    if (argument_count != 1 || arguments[0].kind != SOL_HOST_VALUE_TEXT
        || arguments[0].as.text.length > sizeof(host->output) - host->output_length) {
        return false;
    }
    memcpy(host->output + host->output_length,
        arguments[0].as.text.bytes, arguments[0].as.text.length);
    host->output_length += arguments[0].as.text.length;
    result->kind = SOL_HOST_VALUE_UNIT;
    return true;
}

static bool safe_arguments(void *context, const SolHostValue *arguments,
    size_t argument_count, SolHostValue *result,
    SolInterpreterHostFailure *failure) {
    SafeHost *host = context;
    (void)arguments;
    (void)failure;
    ++host->calls;
    if (argument_count != 0) return false;
    result->kind = SOL_HOST_VALUE_INT64;
    result->as.integer = host->argument_count;
    return true;
}

static bool safe_argument_get(void *context, const SolHostValue *arguments,
    size_t argument_count, SolHostValue *result,
    SolInterpreterHostFailure *failure) {
    SafeHost *host = context;
    (void)failure;
    ++host->calls;
    if (argument_count != 1 || arguments[0].kind != SOL_HOST_VALUE_INT64
        || arguments[0].as.integer != 0) return false;
    result->kind = SOL_HOST_VALUE_OPTION;
    result->as.sum.is_error = false;
    result->as.sum.value = &host->argument_value;
    return true;
}

static bool safe_configuration(void *context,
    const SolHostValue *arguments, size_t argument_count,
    SolHostValue *result, SolInterpreterHostFailure *failure) {
    SafeHost *host = context;
    (void)failure;
    ++host->calls;
    if (argument_count != 1 || arguments[0].kind != SOL_HOST_VALUE_TEXT) {
        return false;
    }
    result->kind = SOL_HOST_VALUE_OPTION;
    result->as.sum.is_error = false;
    result->as.sum.value = &host->configuration_value;
    return true;
}

static bool safe_wrong_result(void *context, const SolHostValue *arguments,
    size_t argument_count, SolHostValue *result,
    SolInterpreterHostFailure *failure) {
    SafeHost *host = context;
    (void)arguments;
    (void)failure;
    ++host->calls;
    if (argument_count != 0) return false;
    result->kind = SOL_HOST_VALUE_BOOL;
    result->as.boolean = true;
    return true;
}

static bool safe_alias_result(void *context, const SolHostValue *arguments,
    size_t argument_count, SolHostValue *result,
    SolInterpreterHostFailure *failure) {
    SafeHost *host = context;
    (void)failure;
    ++host->calls;
    if (argument_count != 1) return false;
    *result = arguments[0];
    return true;
}

static SolIrCallableId callable(const SolValidatedIr *validated, const char *name) {
    size_t count = sol_validated_ir_definition_count(validated);
    for (size_t index = 0; index < count; ++index) {
        SolValidatedDefinitionView definition;
        if (sol_validated_ir_definition_at(validated, index, &definition)
            && strcmp(definition.name, name) == 0) return definition.callable;
    }
    return SOL_IR_NONE;
}

static void test_text_success_and_transfer(void) {
    static const char source[] =
        "module compilation_api\n"
        "@entry public function answer() -> Int64 effects { pure } { return 42 }\n"
        "test \"answer works\" { answer() == 42 }\n";
    SolCompilationSession *session = sol_compilation_create();
    CHECK(session != NULL);
    SolCompilationSummary summary;
    CHECK(!sol_compilation_summary(session, &summary));
    CHECK(sol_compilation_error(session) == NULL);
    CHECK(sol_compilation_compile_text(session, "memory.sol", source)
        == SOL_COMPILATION_SUCCEEDED);

    CHECK(sol_compilation_summary(session, &summary));
    CHECK(summary.file_count == 1);
    CHECK(!summary.is_directory);
    CHECK(strcmp(summary.path, "memory.sol") == 0);
    CHECK(summary.declaration_count == 2);
    CHECK(summary.hir_definition_count == 2);
    CHECK(summary.type_definition_count == 2);
    CHECK(summary.effect_function_count == 2);
    CHECK(summary.contract_obligation_count == 0);
    CHECK(summary.diagnostic_count == 0);
    CHECK(sol_compilation_diagnostic_count(session) == 0);
    SolCompilationDiagnosticView diagnostic;
    CHECK(!sol_compilation_diagnostic_at(session, 0, &diagnostic));
    FILE *inspection = tmpfile();
    CHECK(inspection != NULL);
    if (inspection != NULL) {
        CHECK(sol_compilation_inspection_render(inspection, session));
        CHECK(fclose(inspection) == 0);
    }

    CHECK(sol_compilation_compile_text(session, "other.sol", source)
        == SOL_COMPILATION_INVALID_ARGUMENT);
    SolValidatedIr *validated = NULL;
    CHECK(sol_compilation_take_ir(session, &validated) == SOL_COMPILATION_SUCCEEDED);
    CHECK(validated != NULL);
    CHECK(!sol_compilation_summary(session, &summary));
    SolValidatedIr *second = validated;
    CHECK(sol_compilation_take_ir(session, &second) == SOL_COMPILATION_INVALID_ARGUMENT);
    CHECK(second == validated);
    CHECK(sol_compilation_error(session) != NULL);
    sol_compilation_free(session);

    CHECK(sol_validated_ir_definition_count(validated) == 2);
    SolValidatedDefinitionView definition;
    CHECK(sol_validated_ir_definition_at(validated, 0, &definition));
    CHECK(definition.kind == SOL_VALIDATED_DEFINITION_FUNCTION);
    CHECK(strcmp(definition.name, "answer") == 0);
    CHECK(definition.is_entrypoint);
    CHECK(sol_validated_ir_path_at(validated, definition.span) != NULL);
    CHECK(sol_validated_ir_file_count(validated) == 1);
    SolInterpreterRequest request = {
        .ir = NULL,
        .callable = callable(validated, "answer"),
        .definition = SOL_IR_NONE,
        .contracts = SOL_INTERPRETER_CONTRACTS_IGNORE,
    };
    SolInterpreterResult result;
    CHECK(sol_validated_ir_interpret(validated, &request, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_INT64);
    CHECK(result.value.as.integer == 42);
    SolEntrypointView entrypoint;
    CHECK(sol_validated_ir_entrypoint(validated, &entrypoint));
    CHECK(entrypoint.definition == 0);
    CHECK(entrypoint.callable == request.callable);
    CHECK(entrypoint.parameter_count == 0);
    CHECK(entrypoint.result == SOL_ENTRYPOINT_RESULT_INT64);
    CHECK(strcmp(entrypoint.name, "answer") == 0);
    int status = -1;
    CHECK(sol_validated_ir_entrypoint_exit_status(validated, &result, &status));
    CHECK(status == 42);
    result.value.as.integer = 256;
    CHECK(!sol_validated_ir_entrypoint_exit_status(validated, &result, &status));
    result.value.as.integer = 42;
    result.diagnostic.code = SOL_INTERPRETER_PANIC;
    CHECK(!sol_validated_ir_entrypoint_exit_status(validated, &result, &status));
    result.diagnostic.code = SOL_INTERPRETER_OK;
    sol_interpreter_result_free(&result);

    HostProbe probe = {0};
    request.host_operation = probe_host;
    request.host_context = &probe;
    CHECK(!sol_validated_ir_interpret(validated, &request, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST);
    CHECK(strstr(result.diagnostic.message,
        "do not expose raw IR host callbacks") != NULL);
    CHECK(!probe.invoked);
    CHECK(probe.escaped == NULL);
    sol_interpreter_result_free(&result);
    request.host_operation = NULL;
    request.host_context = NULL;

    request.callable = callable(validated, "answer works");
    request.test_entry = true;
    CHECK(sol_validated_ir_interpret(validated, &request, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_BOOL);
    CHECK(result.value.as.boolean);
    sol_interpreter_result_free(&result);
    FILE *effects = tmpfile();
    CHECK(effects != NULL);
    if (effects != NULL) {
        CHECK(sol_validated_ir_effects_render(effects, validated, false));
        CHECK(fclose(effects) == 0);
    }
    sol_validated_ir_free(validated);
    CHECK(sol_validated_ir_definition_count(NULL) == 0);
    CHECK(sol_validated_ir_file_count(NULL) == 0);
    CHECK(!sol_validated_ir_definition_at(NULL, 0, &definition));
    CHECK(!sol_validated_ir_entrypoint(NULL, &entrypoint));
    SolEntrypointParameterView parameter;
    CHECK(!sol_validated_ir_entrypoint_parameter_at(NULL, 0, &parameter));
    CHECK(!sol_validated_ir_entrypoint_exit_status(NULL, &result, &status));
    CHECK(sol_validated_ir_path_at(NULL, (SolCompilationSpan){0}) == NULL);
    CHECK(!sol_validated_ir_interpret(NULL, &request, &result));
    sol_interpreter_result_free(&result);
    CHECK(!sol_validated_ir_effects_render(NULL, NULL, false));
    sol_validated_ir_free(NULL);
}

static void check_rejected(const char *source, const char *code) {
    SolCompilationSession *session = sol_compilation_create();
    CHECK(session != NULL);
    CHECK(sol_compilation_compile_text(session, "rejected.sol", source)
        == SOL_COMPILATION_REJECTED);
    SolCompilationSummary summary;
    CHECK(sol_compilation_summary(session, &summary));
    CHECK(summary.file_count == 1);
    CHECK(summary.diagnostic_count != 0);
    CHECK(has_code(session, code));
    SolCompilationDiagnosticView diagnostic;
    CHECK(sol_compilation_diagnostic_at(session, 0, &diagnostic));
    CHECK(diagnostic.code != NULL);
    CHECK(diagnostic.message != NULL);
    FILE *rendered = tmpfile();
    CHECK(rendered != NULL);
    if (rendered != NULL) {
        CHECK(sol_compilation_diagnostics_render_human(rendered, session));
        CHECK(sol_compilation_diagnostics_render_json(rendered, session));
        CHECK(!sol_compilation_inspection_render(rendered, session));
        CHECK(fclose(rendered) == 0);
    }
    SolValidatedIr *validated = NULL;
    CHECK(sol_compilation_take_ir(session, &validated)
        == SOL_COMPILATION_INVALID_ARGUMENT);
    CHECK(validated == NULL);
    SolValidatedIr *unchanged = (SolValidatedIr *)(uintptr_t)1;
    CHECK(sol_compilation_take_ir(session, &unchanged)
        == SOL_COMPILATION_INVALID_ARGUMENT);
    CHECK(unchanged == (SolValidatedIr *)(uintptr_t)1);
    sol_compilation_free(session);
}

static void test_rejections_and_failure_teardown(void) {
    check_rejected(
        "record MissingModule { value: Int64 }\n",
        "SOL-PARSE-004");
    check_rejected(
        "module type_error\n"
        "function bad() -> Int64 { return true + 1 }\n",
        "SOL-TYPE-002");
    check_rejected(
        "module effect_error\n"
        "function impure() -> Int64 effects { panic } { return 1 }\n"
        "function bad() -> Int64 effects { pure } { return impure() }\n",
        "SOL-EFFECT-002");
    check_rejected(
        "module ownership_error\n"
        "capability Clock { function read() -> Int64 effects { pure } }\n"
        "function bad(clock: capability Clock) -> Int64 effects { pure } {\n"
        "  let alias = clock\n"
        "  return clock.read()\n"
        "}\n",
        "SOL-OWNERSHIP-001");
    check_rejected(
        "module duplicate_entry\n"
        "@entry public function first() -> () effects { pure } { () }\n"
        "@entry public function second() -> Int64 effects { pure } { return 0 }\n",
        "SOL-ENTRY-002");
    check_rejected(
        "module private_entry\n"
        "@entry function run() -> () effects { pure } { () }\n",
        "SOL-ENTRY-001");
    check_rejected(
        "module generic_entry\n"
        "@entry public function run<T>() -> () effects { pure } { () }\n",
        "SOL-ENTRY-001");
    check_rejected(
        "module value_parameter\n"
        "@entry public function run(value: Int64) -> () effects { pure } { () }\n",
        "SOL-ENTRY-003");
    check_rejected(
        "module bad_result\n"
        "@entry public function run() -> Bool effects { pure } { true }\n",
        "SOL-ENTRY-003");
    check_rejected(
        "module borrowed_parameter\n"
        "capability Console {}\n"
        "@entry public function run(console: borrow capability Console) -> () "
        "effects { pure } { () }\n",
        "SOL-ENTRY-003");
    check_rejected(
        "module entry_arguments\n"
        "@entry(\"run\") public function run() -> () effects { pure } { () }\n",
        "SOL-PARSE-025");
    check_rejected(
        "module duplicate_annotation\n"
        "@entry @entry public function run() -> () effects { pure } { () }\n",
        "SOL-PARSE-026");
    check_rejected(
        "module derived_entry\n"
        "capability Root {}\n"
        "capability Derived derives_from source: capability Root {}\n"
        "@entry public function run(value: capability Derived) -> () "
        "effects { pure } { () }\n",
        "SOL-ENTRY-003");

    for (size_t index = 0; index < 8; ++index) {
        SolCompilationSession *session = sol_compilation_create();
        CHECK(session != NULL);
        CHECK(sol_compilation_compile_text(session, "bad.sol", "module bad\nfunction")
            == SOL_COMPILATION_REJECTED);
        sol_compilation_free(session);
    }
}

static void test_entrypoint_abi(void) {
    static const char source[] =
        "module entrypoint_abi\n"
        "capability Console {}\n"
        "@entry public function launch(console: capability Console) -> () "
        "effects { pure } { () }\n";
    SolCompilationSession *session = sol_compilation_create();
    CHECK(session != NULL);
    CHECK(sol_compilation_compile_text(session, "entry.sol", source)
        == SOL_COMPILATION_SUCCEEDED);
    SolValidatedIr *validated = NULL;
    CHECK(sol_compilation_take_ir(session, &validated) == SOL_COMPILATION_SUCCEEDED);
    sol_compilation_free(session);
    SolEntrypointView entrypoint;
    CHECK(sol_validated_ir_entrypoint(validated, &entrypoint));
    CHECK(entrypoint.result == SOL_ENTRYPOINT_RESULT_UNIT);
    CHECK(entrypoint.parameter_count == 1);
    SolEntrypointParameterView parameter;
    CHECK(sol_validated_ir_entrypoint_parameter_at(validated, 0, &parameter));
    CHECK(strcmp(parameter.name, "console") == 0);
    CHECK(strcmp(parameter.capability, "Console") == 0);
    CHECK(!sol_validated_ir_entrypoint_parameter_at(validated, 1, &parameter));
    SolInterpreterResult result = {0};
    result.value.kind = SOL_INTERPRETER_VALUE_UNIT;
    int status = -1;
    CHECK(sol_validated_ir_entrypoint_exit_status(validated, &result, &status));
    CHECK(status == 0);
    sol_validated_ir_free(validated);

    session = sol_compilation_create();
    CHECK(session != NULL);
    CHECK(sol_compilation_compile_text(session, "library.sol",
        "module library\nfunction value() -> Int64 { return 1 }\n")
        == SOL_COMPILATION_SUCCEEDED);
    validated = NULL;
    CHECK(sol_compilation_take_ir(session, &validated) == SOL_COMPILATION_SUCCEEDED);
    sol_compilation_free(session);
    CHECK(!sol_validated_ir_entrypoint(validated, &entrypoint));
    sol_validated_ir_free(validated);
}

static void test_safe_host_registry(void) {
    static const char source[] =
        "module safe_host\n"
        "capability Console { function write(value: Text) -> () "
        "effects { console.write<Self> } }\n"
        "capability Arguments { function count() -> Int64 "
        "effects { process.arguments.count<Self> } function get(index: Int64) "
        "-> Option<Text> effects { process.arguments.get<Self> } }\n"
        "capability Configuration { function read(key: Text) -> Option<Text> "
        "effects { configuration.read<Self> } }\n"
        "@entry public function launch(console: capability Console, "
        "arguments: capability Arguments, configuration: capability Configuration) "
        "-> Int64 effects { console.write<console> "
        "process.arguments.count<arguments> process.arguments.get<arguments> "
        "configuration.read<configuration> } {\n"
        " let configured = configuration.read(\"mode\")\n"
        " let first = arguments.get(0)\n"
        " console.write(\"ready\")\n"
        " return arguments.count()\n"
        "}\n";
    SolCompilationSession *session = sol_compilation_create();
    CHECK(session != NULL);
    CHECK(sol_compilation_compile_text(session, "host.sol", source)
        == SOL_COMPILATION_SUCCEEDED);
    SolValidatedIr *validated = NULL;
    CHECK(sol_compilation_take_ir(session, &validated) == SOL_COMPILATION_SUCCEEDED);
    sol_compilation_free(session);

    SolHostRegistry *registry = sol_host_registry_create(validated);
    CHECK(registry != NULL);
    SafeHost host = {
        .argument_count = 3,
        .argument_value = {
            .kind = SOL_HOST_VALUE_TEXT,
            .as.text = {.bytes = "first", .length = 5},
        },
        .configuration_value = {
            .kind = SOL_HOST_VALUE_TEXT,
            .as.text = {.bytes = NULL, .length = 0},
        },
    };
    CHECK(sol_host_registry_bind_root(registry, 0));
    CHECK(sol_host_registry_bind_root(registry, 1));
    CHECK(sol_host_registry_bind_root(registry, 2));
    CHECK(!sol_host_registry_bind_root(registry, 2));
    CHECK(sol_host_registry_error(registry) != NULL);
    CHECK(sol_host_registry_allow(registry, 0, "write", safe_console, &host));
    CHECK(sol_host_registry_allow(registry, 1, "count", safe_arguments, &host));
    CHECK(sol_host_registry_allow(registry, 1, "get", safe_argument_get, &host));
    CHECK(sol_host_registry_allow(registry, 2, "read", safe_configuration, &host));
    CHECK(!sol_host_registry_allow(registry, 2, "read", safe_configuration, &host));
    CHECK(sol_host_registry_error(registry) != NULL);
    SolInterpreterRequest request = {
        .contracts = SOL_INTERPRETER_CONTRACTS_IGNORE,
    };
    SolInterpreterResult result;
    CHECK(sol_validated_ir_interpret_entrypoint(validated, registry, &request, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_INT64);
    CHECK(result.value.as.integer == 3);
    CHECK(result.used.host_calls == 4);
    CHECK(host.calls == 4);
    CHECK(host.output_length == 5);
    CHECK(memcmp(host.output, "ready", 5) == 0);
    int status = -1;
    CHECK(sol_validated_ir_entrypoint_exit_status(validated, &result, &status));
    CHECK(status == 3);
    sol_interpreter_result_free(&result);

    request.limits.host_calls = 0;
    request.limits.steps = 100000;
    request.limits.call_depth = 128;
    request.limits.value_nodes = 100000;
    request.limits.text_bytes = 1048576;
    size_t calls = host.calls;
    CHECK(!sol_validated_ir_interpret_entrypoint(validated, registry, &request, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_HOST_CALL_LIMIT);
    CHECK(host.calls == calls);
    sol_interpreter_result_free(&result);
    request.limits = (SolInterpreterLimits){0};

    host.fail_console = true;
    CHECK(!sol_validated_ir_interpret_entrypoint(validated, registry, &request, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_HOST_ERROR);
    CHECK(strstr(result.diagnostic.message, "console unavailable") != NULL);
    host.fail_console = false;
    sol_interpreter_result_free(&result);

    SolHostRegistry *missing = sol_host_registry_create(validated);
    CHECK(missing != NULL);
    CHECK(sol_host_registry_bind_root(missing, 0));
    CHECK(sol_host_registry_bind_root(missing, 1));
    calls = host.calls;
    CHECK(!sol_validated_ir_interpret_entrypoint(validated, missing, &request, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST);
    CHECK(host.calls == calls);
    sol_interpreter_result_free(&result);
    CHECK(sol_host_registry_bind_root(missing, 2));
    CHECK(sol_host_registry_allow(missing, 0, "write", safe_console, &host));
    CHECK(!sol_validated_ir_interpret_entrypoint(validated, missing, &request, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST);
    CHECK(host.calls == calls);
    sol_interpreter_result_free(&result);

    SolCompilationSession *other_session = sol_compilation_create();
    CHECK(other_session != NULL);
    CHECK(sol_compilation_compile_text(other_session, "other.sol", source)
        == SOL_COMPILATION_SUCCEEDED);
    SolValidatedIr *other = NULL;
    CHECK(sol_compilation_take_ir(other_session, &other) == SOL_COMPILATION_SUCCEEDED);
    sol_compilation_free(other_session);
    CHECK(!sol_validated_ir_interpret_entrypoint(other, registry, &request, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST);
    sol_interpreter_result_free(&result);

    sol_validated_ir_free(other);
    sol_host_registry_free(missing);
    sol_host_registry_free(registry);
    sol_validated_ir_free(validated);
    sol_host_registry_free(NULL);
    CHECK(sol_host_registry_error(NULL) == NULL);
}

static void test_root_specific_allowlist(void) {
    static const char source[] =
        "module root_specific\n"
        "capability Console { function write(value: Text) -> () "
        "effects { console.write<Self> } }\n"
        "@entry public function launch(left: capability Console, "
        "right: capability Console) -> () "
        "effects { console.write<left> console.write<right> } { "
        "left.write(\"left\") right.write(\"right\") }\n";
    SolCompilationSession *session = sol_compilation_create();
    CHECK(session != NULL);
    CHECK(sol_compilation_compile_text(session, "roots.sol", source)
        == SOL_COMPILATION_SUCCEEDED);
    SolValidatedIr *validated = NULL;
    CHECK(sol_compilation_take_ir(session, &validated) == SOL_COMPILATION_SUCCEEDED);
    sol_compilation_free(session);
    SolHostRegistry *registry = sol_host_registry_create(validated);
    SafeHost left = {0};
    SafeHost right = {0};
    CHECK(registry != NULL);
    CHECK(sol_host_registry_bind_root(registry, 0));
    CHECK(sol_host_registry_bind_root(registry, 1));
    CHECK(sol_host_registry_allow(registry, 0, "write", safe_console, &left));
    CHECK(sol_host_registry_allow(registry, 1, "write", safe_console, &right));
    SolInterpreterRequest request = {
        .contracts = SOL_INTERPRETER_CONTRACTS_IGNORE,
    };
    SolInterpreterResult result;
    CHECK(sol_validated_ir_interpret_entrypoint(validated, registry, &request, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_UNIT);
    CHECK(left.calls == 1 && right.calls == 1);
    CHECK(left.output_length == 4 && memcmp(left.output, "left", 4) == 0);
    CHECK(right.output_length == 5 && memcmp(right.output, "right", 5) == 0);
    sol_interpreter_result_free(&result);
    sol_host_registry_free(registry);
    sol_validated_ir_free(validated);

    static const char alias_source[] =
        "module host_alias\n"
        "capability Echo { function echo(value: Option<Text>) -> Option<Text> "
        "effects { host.echo<Self> } }\n"
        "@entry public function launch(echo: capability Echo) -> () "
        "effects { host.echo<echo> } { let value = echo.echo(some(\"value\")) }\n";
    session = sol_compilation_create();
    CHECK(session != NULL);
    CHECK(sol_compilation_compile_text(session, "alias.sol", alias_source)
        == SOL_COMPILATION_SUCCEEDED);
    validated = NULL;
    CHECK(sol_compilation_take_ir(session, &validated) == SOL_COMPILATION_SUCCEEDED);
    sol_compilation_free(session);
    registry = sol_host_registry_create(validated);
    CHECK(registry != NULL);
    CHECK(sol_host_registry_bind_root(registry, 0));
    CHECK(sol_host_registry_allow(registry, 0, "echo", safe_alias_result, &left));
    CHECK(sol_validated_ir_interpret_entrypoint(validated, registry, &request, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_UNIT);
    sol_interpreter_result_free(&result);
    sol_host_registry_free(registry);
    sol_validated_ir_free(validated);
}

static void test_host_preflight_and_results(void) {
    static const char unsupported_source[] =
        "module unsupported_host\n"
        "capability Console { function write(value: Text) -> () "
        "effects { console.write<Self> } }\n"
        "capability Unsupported { function pair() -> (Int64, Int64) "
        "effects { unsupported.pair<Self> } }\n"
        "@entry public function launch(console: capability Console, "
        "unsupported: capability Unsupported) -> () "
        "effects { console.write<console> unsupported.pair<unsupported> } { "
        "console.write(\"must not run\") let pair = unsupported.pair() }\n";
    SolCompilationSession *session = sol_compilation_create();
    CHECK(session != NULL);
    CHECK(sol_compilation_compile_text(session, "unsupported.sol", unsupported_source)
        == SOL_COMPILATION_SUCCEEDED);
    SolValidatedIr *validated = NULL;
    CHECK(sol_compilation_take_ir(session, &validated) == SOL_COMPILATION_SUCCEEDED);
    sol_compilation_free(session);
    SolHostRegistry *registry = sol_host_registry_create(validated);
    SafeHost host = {0};
    CHECK(registry != NULL);
    CHECK(sol_host_registry_bind_root(registry, 0));
    CHECK(sol_host_registry_bind_root(registry, 1));
    CHECK(sol_host_registry_allow(registry, 0, "write", safe_console, &host));
    CHECK(!sol_host_registry_allow(registry, 1, "pair", safe_console, &host));
    SolInterpreterRequest request = {
        .contracts = SOL_INTERPRETER_CONTRACTS_IGNORE,
    };
    SolInterpreterResult result;
    CHECK(!sol_validated_ir_interpret_entrypoint(validated, registry, &request, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST);
    CHECK(host.calls == 0 && host.output_length == 0);
    sol_interpreter_result_free(&result);
    sol_host_registry_free(registry);
    sol_validated_ir_free(validated);

    static const char wrong_source[] =
        "module wrong_host_result\n"
        "capability Arguments { function count() -> Int64 "
        "effects { process.arguments.count<Self> } }\n"
        "@entry public function launch(arguments: capability Arguments) -> Int64 "
        "effects { process.arguments.count<arguments> } { return arguments.count() }\n";
    session = sol_compilation_create();
    CHECK(session != NULL);
    CHECK(sol_compilation_compile_text(session, "wrong.sol", wrong_source)
        == SOL_COMPILATION_SUCCEEDED);
    validated = NULL;
    CHECK(sol_compilation_take_ir(session, &validated) == SOL_COMPILATION_SUCCEEDED);
    sol_compilation_free(session);
    registry = sol_host_registry_create(validated);
    CHECK(registry != NULL);
    CHECK(sol_host_registry_bind_root(registry, 0));
    CHECK(sol_host_registry_allow(registry, 0, "count", safe_wrong_result, &host));
    CHECK(!sol_validated_ir_interpret_entrypoint(validated, registry, &request, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_HOST_ERROR);
    sol_interpreter_result_free(&result);
    sol_host_registry_free(registry);
    sol_validated_ir_free(validated);
}

static void test_invalid_arguments_and_state(void) {
    static const char source[] = "module valid\nfunction value() -> Int64 { return 1 }\n";
    CHECK(sol_compilation_compile_path(NULL, "x.sol")
        == SOL_COMPILATION_INVALID_ARGUMENT);
    CHECK(sol_compilation_compile_text(NULL, "x.sol", source)
        == SOL_COMPILATION_INVALID_ARGUMENT);
    CHECK(sol_compilation_compile_source(NULL, "x.sol", source, sizeof(source) - 1)
        == SOL_COMPILATION_INVALID_ARGUMENT);
    SolCompilationSummary summary;
    CHECK(!sol_compilation_summary(NULL, &summary));
    CHECK(sol_compilation_diagnostic_count(NULL) == 0);
    SolCompilationDiagnosticView diagnostic;
    CHECK(!sol_compilation_diagnostic_at(NULL, 0, &diagnostic));
    CHECK(!sol_compilation_diagnostics_render_human(NULL, NULL));
    CHECK(!sol_compilation_diagnostics_render_json(NULL, NULL));
    CHECK(!sol_compilation_inspection_render(NULL, NULL));
    CHECK(sol_compilation_error(NULL) == NULL);
    SolValidatedIr *validated = NULL;
    CHECK(sol_compilation_take_ir(NULL, &validated)
        == SOL_COMPILATION_INVALID_ARGUMENT);
    CHECK(sol_compilation_take_ir(NULL, NULL)
        == SOL_COMPILATION_INVALID_ARGUMENT);
    sol_compilation_free(NULL);

    SolCompilationSession *session = sol_compilation_create();
    CHECK(session != NULL);
    CHECK(sol_compilation_take_ir(session, NULL)
        == SOL_COMPILATION_INVALID_ARGUMENT);
    CHECK(sol_compilation_compile_text(session, NULL, source)
        == SOL_COMPILATION_INVALID_ARGUMENT);
    CHECK(sol_compilation_compile_text(session, "valid.sol", NULL)
        == SOL_COMPILATION_INVALID_ARGUMENT);
    CHECK(sol_compilation_compile_source(session, "valid.sol", NULL, 0)
        == SOL_COMPILATION_INVALID_ARGUMENT);
    CHECK(sol_compilation_compile_text(session, "valid.sol", source)
        == SOL_COMPILATION_SUCCEEDED);
    CHECK(sol_compilation_compile_path(session, SOL_TEST_SOURCE_DIR "/tests/valid.sol")
        == SOL_COMPILATION_INVALID_ARGUMENT);
    sol_compilation_free(session);
}

static void test_source_bytes_validation_and_retry(void) {
    static const char valid[] =
        "module bytes\nfunction value() -> Int64 { return 1 }\n";
    static const char embedded[] = "module bytes\0ignored";
    SolCompilationSession *session = sol_compilation_create();
    SolCompilationSummary summary;
    CHECK(session != NULL);
    CHECK(sol_compilation_compile_source(
        session, "bytes.sol", embedded, sizeof(embedded) - 1)
        == SOL_COMPILATION_INVALID_ARGUMENT);
    CHECK(!sol_compilation_summary(session, &summary));
    CHECK(sol_compilation_compile_source(
        session, "bytes.sol", valid, sizeof(valid) - 1)
        == SOL_COMPILATION_SUCCEEDED);
    sol_compilation_free(session);

    session = sol_compilation_create();
    CHECK(session != NULL);
    CHECK(sol_compilation_compile_source(session, "bytes.sol", valid, SIZE_MAX)
        == SOL_COMPILATION_INVALID_ARGUMENT);
    CHECK(sol_compilation_compile_text(session, "bytes.sol", valid)
        == SOL_COMPILATION_SUCCEEDED);
    sol_compilation_free(session);
}

static void test_paths_and_scopes(void) {
    SolCompilationSession *session = sol_compilation_create();
    CHECK(session != NULL);
    CHECK(sol_compilation_compile_path(
        session, SOL_TEST_SOURCE_DIR "/tests/valid.sol")
        == SOL_COMPILATION_SUCCEEDED);
    SolCompilationSummary summary;
    CHECK(sol_compilation_summary(session, &summary));
    CHECK(!summary.is_directory);
    CHECK(summary.file_count == 1);
    CHECK(summary.hir_file_scope_count == 0);
    sol_compilation_free(session);

    session = sol_compilation_create();
    CHECK(session != NULL);
    CHECK(sol_compilation_compile_path(
        session, SOL_TEST_SOURCE_DIR "/tests/packages/valid")
        == SOL_COMPILATION_SUCCEEDED);
    CHECK(sol_compilation_summary(session, &summary));
    CHECK(summary.is_directory);
    CHECK(summary.file_count == 7);
    CHECK(summary.hir_file_scope_count == summary.file_count);
    SolValidatedIr *validated = NULL;
    CHECK(sol_compilation_take_ir(session, &validated) == SOL_COMPILATION_SUCCEEDED);
    sol_compilation_free(session);
    CHECK(sol_validated_ir_definition_count(validated) != 0);
    CHECK(sol_validated_ir_file_count(validated) == 7);
    SolValidatedDefinitionView definition;
    CHECK(sol_validated_ir_definition_at(validated, 0, &definition));
    CHECK(sol_validated_ir_path_at(validated, definition.span) != NULL);
    sol_validated_ir_free(validated);

    session = sol_compilation_create();
    CHECK(session != NULL);
    CHECK(sol_compilation_compile_path(
        session, SOL_TEST_SOURCE_DIR "/tests/does-not-exist.sol")
        == SOL_COMPILATION_LOAD_FAILED);
    CHECK(sol_compilation_error(session) != NULL);
    CHECK(strstr(sol_compilation_error(session), "cannot inspect") != NULL);
    CHECK(sol_compilation_summary(session, &summary));
    sol_compilation_free(session);

    session = sol_compilation_create();
    CHECK(session != NULL);
    CHECK(sol_compilation_compile_path(
        session, SOL_TEST_SOURCE_DIR "/tests/out of memory.sol")
        == SOL_COMPILATION_LOAD_FAILED);
    sol_compilation_free(session);
}

static void check_resource_limit(
    SolCompilationLimits limits, const char *source, const char *message
) {
    SolCompilationSession *session = sol_compilation_create_with_limits(&limits);
    CHECK(session != NULL);
    CHECK(sol_compilation_compile_text(session, "limited.sol", source)
        == SOL_COMPILATION_RESOURCE_FAILED);
    CHECK(sol_compilation_error(session) != NULL);
    CHECK(strstr(sol_compilation_error(session), message) != NULL);
    SolValidatedIr *validated = NULL;
    CHECK(sol_compilation_take_ir(session, &validated)
        == SOL_COMPILATION_INVALID_ARGUMENT);
    CHECK(validated == NULL);
    sol_compilation_free(session);
}

static void test_resource_limits(void) {
    static const char source[] =
        "module limited\nfunction value() -> Int64 { return 1 }\n";
    SolCompilationLimits defaults;
    sol_compilation_limits_default(&defaults);
    CHECK(defaults.source_bytes_per_file != 0);
    CHECK(defaults.package_source_bytes >= defaults.source_bytes_per_file);
    CHECK(defaults.source_files != 0);
    CHECK(defaults.tokens != 0);
    CHECK(defaults.arena_entries != 0);
    CHECK(defaults.diagnostics != 0);
    CHECK(defaults.allocation_bytes != 0);
    CHECK(defaults.allocation_count != 0);
    sol_compilation_limits_default(NULL);
    CHECK(sol_compilation_create_with_limits(NULL) == NULL);

    SolCompilationLimits limits = defaults;
    limits.source_bytes_per_file = sizeof(source) - 2;
    check_resource_limit(limits, source, "source file byte limit exceeded");
    limits = defaults;
    limits.package_source_bytes = sizeof(source) - 2;
    check_resource_limit(limits, source, "package source byte limit exceeded");
    limits = defaults;
    limits.source_files = 0;
    check_resource_limit(limits, source, "source file limit exceeded");
    limits = defaults;
    limits.tokens = 1;
    check_resource_limit(limits, source, "token limit exceeded");
    limits = defaults;
    limits.arena_entries = 1;
    check_resource_limit(limits, source, "compiler arena limit exceeded");
    limits = defaults;
    limits.diagnostics = 1;
    check_resource_limit(limits, "@ @ @", "diagnostic limit exceeded");
    limits = defaults;
    limits.allocation_bytes = 1;
    check_resource_limit(limits, source, "compiler allocation byte limit exceeded");
    limits = defaults;
    limits.allocation_count = 1;
    check_resource_limit(limits, source, "compiler allocation count limit exceeded");

    limits = defaults;
    limits.directory_depth = 0;
    SolCompilationSession *session = sol_compilation_create_with_limits(&limits);
    CHECK(session != NULL);
    CHECK(sol_compilation_compile_path(
        session, SOL_TEST_SOURCE_DIR "/tests/packages/valid")
        == SOL_COMPILATION_RESOURCE_FAILED);
    CHECK(strstr(sol_compilation_error(session), "directory depth limit exceeded") != NULL);
    sol_compilation_free(session);
    limits = defaults;
    limits.directory_entries = 1;
    session = sol_compilation_create_with_limits(&limits);
    CHECK(session != NULL);
    CHECK(sol_compilation_compile_path(
        session, SOL_TEST_SOURCE_DIR "/tests/packages/valid")
        == SOL_COMPILATION_RESOURCE_FAILED);
    CHECK(strstr(sol_compilation_error(session), "directory entry limit exceeded") != NULL);
    sol_compilation_free(session);
}

int main(void) {
    test_text_success_and_transfer();
    test_rejections_and_failure_teardown();
    test_entrypoint_abi();
    test_safe_host_registry();
    test_root_specific_allowlist();
    test_host_preflight_and_results();
    test_invalid_arguments_and_state();
    test_source_bytes_validation_and_retry();
    test_paths_and_scopes();
    test_resource_limits();
    if (failures != 0) {
        fprintf(stderr, "%d compilation test%s failed\n",
            failures, failures == 1 ? "" : "s");
        return 1;
    }
    puts("compilation tests passed");
    return 0;
}
