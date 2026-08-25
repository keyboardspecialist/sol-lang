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
        "function answer() -> Int64 effects { pure } { return 42 }\n"
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

    for (size_t index = 0; index < 8; ++index) {
        SolCompilationSession *session = sol_compilation_create();
        CHECK(session != NULL);
        CHECK(sol_compilation_compile_text(session, "bad.sol", "module bad\nfunction")
            == SOL_COMPILATION_REJECTED);
        sol_compilation_free(session);
    }
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
