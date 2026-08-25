#include "sol/compilation.h"

#include "sol/contract.h"
#include "sol/diagnostic.h"
#include "sol/effects.h"
#include "sol/hir.h"
#include "sol/inspection.h"
#include "sol/interpreter.h"
#include "sol/lexer.h"
#include "sol/package.h"
#include "sol/typecheck.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "resource_internal.h"
#include <string.h>

typedef enum {
    SOL_SESSION_EMPTY,
    SOL_SESSION_COMPILED,
    SOL_SESSION_FAILED,
    SOL_SESSION_IR_TAKEN,
} SolSessionState;

struct SolValidatedIr {
    SolIr ir;
};

struct SolCompilationSession {
    SolPackage package;
    SolDiagnostics diagnostics;
    SolHirModule hir;
    SolTypeTable types;
    SolEffectTable effects;
    SolContractTable contracts;
    SolValidatedIr *validated;
    SolResourceBudget resources;
    SolSessionState state;
    SolCompilationOutcome outcome;
    char error[512];
    bool frontend_live;
};

static char *sol_compilation_copy_string(const char *text) {
    size_t length = strlen(text);
    if (length == SIZE_MAX) return NULL;
    char *copy = malloc(length + 1);
    if (copy != NULL) memcpy(copy, text, length + 1);
    return copy;
}

static void sol_compilation_frontend_free(SolCompilationSession *session) {
    if (!session->frontend_live) return;
    sol_contract_table_free(&session->contracts);
    sol_effect_table_free(&session->effects);
    sol_type_table_free(&session->types);
    sol_hir_module_free(&session->hir);
    sol_diagnostics_free(&session->diagnostics);
    sol_package_free(&session->package);
    session->frontend_live = false;
}

static SolCompilationOutcome sol_compilation_finish(
    SolCompilationSession *session,
    SolCompilationOutcome outcome
) {
    sol_resource_end(&session->resources);
    session->outcome = outcome;
    session->state = outcome == SOL_COMPILATION_SUCCEEDED
        ? SOL_SESSION_COMPILED : SOL_SESSION_FAILED;
    return outcome;
}

static void sol_compilation_set_resource_error(SolCompilationSession *session) {
    const char *failure = sol_resource_failure(&session->resources);
    if (failure != NULL) {
        snprintf(session->error, sizeof(session->error), "%s", failure);
    } else if (session->error[0] == '\0') {
        snprintf(session->error, sizeof(session->error),
            "compilation stopped because the compiler ran out of memory");
    }
}

static bool sol_compilation_has_internal_diagnostic(
    const SolDiagnostics *diagnostics
) {
    static const char prefix[] = "SOL-INTERNAL-";
    for (size_t index = 0; index < diagnostics->count; ++index) {
        if (strncmp(diagnostics->items[index].code, prefix,
            sizeof(prefix) - 1) == 0) return true;
    }
    return false;
}

static SolCompilationOutcome sol_compilation_phase_outcome(
    SolCompilationSession *session,
    bool completed
) {
    if (sol_resource_failure(&session->resources) != NULL
        || session->diagnostics.allocation_failed
        || sol_compilation_has_internal_diagnostic(&session->diagnostics)) {
        return SOL_COMPILATION_RESOURCE_FAILED;
    }
    if (sol_diagnostics_has_errors(&session->diagnostics)) {
        return SOL_COMPILATION_REJECTED;
    }
    return completed ? SOL_COMPILATION_SUCCEEDED : SOL_COMPILATION_RESOURCE_FAILED;
}

static SolCompilationOutcome sol_compilation_run(SolCompilationSession *session) {
    SolPackage *package = &session->package;
    SolDiagnostics *diagnostics = &session->diagnostics;
    bool completed = !sol_diagnostics_has_errors(diagnostics);

    if (completed && !package->is_directory) {
        completed = sol_hir_lower(
            &package->source, &package->syntax, &session->hir, diagnostics);
    } else if (completed) {
        SolHirFileScope *scopes = NULL;
        if (package->file_count <= SIZE_MAX / sizeof(*scopes)) {
            scopes = malloc(package->file_count * sizeof(*scopes));
        }
        if (scopes == NULL && package->file_count != 0) {
            completed = false;
        } else {
            for (size_t index = 0; index < package->file_count; ++index) {
                scopes[index] = (SolHirFileScope){
                    package->files[index].module_name,
                    package->files[index].import_start,
                    package->files[index].import_count,
                    package->files[index].item_start,
                    package->files[index].item_count,
                };
            }
            completed = sol_hir_lower_scoped(
                &package->source, &package->syntax, scopes, package->file_count,
                &session->hir, diagnostics);
            free(scopes);
        }
    }
    SolCompilationOutcome outcome = sol_compilation_phase_outcome(session, completed);
    if (outcome != SOL_COMPILATION_SUCCEEDED) return outcome;
    completed = sol_type_check(
        &package->source, &package->syntax, &session->hir, &session->types,
        diagnostics);
    outcome = sol_compilation_phase_outcome(session, completed);
    if (outcome != SOL_COMPILATION_SUCCEEDED) return outcome;
    completed = sol_effect_check(
        &package->source, &package->syntax, &session->hir, &session->types,
        &session->effects, diagnostics);
    outcome = sol_compilation_phase_outcome(session, completed);
    if (outcome != SOL_COMPILATION_SUCCEEDED) return outcome;
    completed = sol_contract_lower(
        &package->source, &package->syntax, &session->hir, &session->types,
        &session->effects, &session->contracts, diagnostics);
    outcome = sol_compilation_phase_outcome(session, completed);
    if (outcome != SOL_COMPILATION_SUCCEEDED) return outcome;
    completed = sol_ir_lower_scoped(
        &package->source, &package->syntax, &session->hir, &session->types,
        &session->effects, &session->contracts,
        package->is_directory ? package->files : NULL,
        package->is_directory ? package->file_count : 0,
        &session->validated->ir, diagnostics);
    return sol_compilation_phase_outcome(session, completed);
}

SolCompilationSession *sol_compilation_create(void) {
    SolCompilationLimits limits;
    sol_compilation_limits_default(&limits);
    return sol_compilation_create_with_limits(&limits);
}

SolCompilationSession *sol_compilation_create_with_limits(
    const SolCompilationLimits *limits
) {
    if (limits == NULL) return NULL;
    SolCompilationSession *session = calloc(1, sizeof(*session));
    if (session == NULL) return NULL;
    sol_package_init(&session->package);
    sol_diagnostics_init(&session->diagnostics);
    sol_hir_module_init(&session->hir);
    sol_type_table_init(&session->types);
    sol_effect_table_init(&session->effects);
    sol_contract_table_init(&session->contracts);
    session->frontend_live = true;
    session->state = SOL_SESSION_EMPTY;
    session->outcome = SOL_COMPILATION_INVALID_ARGUMENT;
    session->resources.limits = *limits;
    return session;
}

static bool sol_compilation_begin(SolCompilationSession *session) {
    if (session == NULL) return false;
    if (session->state != SOL_SESSION_EMPTY) {
        snprintf(session->error, sizeof(session->error),
            "compilation session has already been used");
        return false;
    }
    sol_resource_begin(&session->resources);
    session->validated = malloc(sizeof(*session->validated));
    if (session->validated == NULL) {
        sol_compilation_finish(session, SOL_COMPILATION_RESOURCE_FAILED);
        sol_compilation_set_resource_error(session);
        return false;
    }
    sol_ir_init(&session->validated->ir);
    session->error[0] = '\0';
    return true;
}

SolCompilationOutcome sol_compilation_compile_path(
    SolCompilationSession *session,
    const char *path
) {
    if (session == NULL || path == NULL) {
        if (session != NULL) snprintf(session->error, sizeof(session->error),
            "compilation path must not be null");
        return SOL_COMPILATION_INVALID_ARGUMENT;
    }
    if (session->state != SOL_SESSION_EMPTY) {
        snprintf(session->error, sizeof(session->error),
            "compilation session has already been used");
        return SOL_COMPILATION_INVALID_ARGUMENT;
    }
    if (!sol_compilation_begin(session)) {
        return session->outcome;
    }
    if (!sol_package_load(
        &session->package, path, &session->diagnostics, session->error,
        sizeof(session->error)
    )) {
        SolCompilationOutcome outcome = sol_resource_failure(&session->resources) != NULL
            || session->diagnostics.allocation_failed
            || sol_compilation_has_internal_diagnostic(&session->diagnostics)
            ? SOL_COMPILATION_RESOURCE_FAILED : SOL_COMPILATION_LOAD_FAILED;
        if (outcome == SOL_COMPILATION_RESOURCE_FAILED) {
            sol_compilation_set_resource_error(session);
        }
        return sol_compilation_finish(session, outcome);
    }
    SolCompilationOutcome outcome = sol_compilation_run(session);
    if (outcome == SOL_COMPILATION_RESOURCE_FAILED) {
        sol_compilation_set_resource_error(session);
    }
    return sol_compilation_finish(session, outcome);
}

static bool sol_compilation_make_text_package(
    SolCompilationSession *session,
    const char *path,
    const char *text
) {
    SolPackage *package = &session->package;
    package->path = sol_compilation_copy_string(path);
    package->files = calloc(1, sizeof(*package->files));
    if (package->path == NULL || package->files == NULL) return false;
    package->file_capacity = 1;
    package->file_count = 1;
    package->files[0].path = sol_compilation_copy_string(path);
    if (package->files[0].path == NULL) return false;
    if (!sol_source_from_text(
        &package->files[0].source, package->files[0].path, text
    )) return false;
    if (!sol_source_from_text(&package->source, package->path, text)) return false;
    package->files[0].aggregate_end = package->source.length;

    SolTokens tokens;
    sol_tokens_init(&tokens);
    bool completed = sol_lex(&package->source, &tokens, &session->diagnostics);
    if (completed) completed = sol_parse(
        &package->source, &tokens, &package->syntax, &session->diagnostics);
    sol_tokens_free(&tokens);
    package->files[0].module_name = package->syntax.module_name;
    package->files[0].import_count = package->syntax.import_count;
    package->files[0].item_count = package->syntax.item_count;
    return completed;
}

SolCompilationOutcome sol_compilation_compile_text(
    SolCompilationSession *session,
    const char *path,
    const char *text
) {
    if (session == NULL || path == NULL || text == NULL) {
        if (session != NULL) snprintf(session->error, sizeof(session->error),
            "compilation path and text must not be null");
        return SOL_COMPILATION_INVALID_ARGUMENT;
    }
    return sol_compilation_compile_source(session, path, text, strlen(text));
}

SolCompilationOutcome sol_compilation_compile_source(
    SolCompilationSession *session,
    const char *path,
    const void *bytes,
    size_t length
) {
    if (session == NULL || path == NULL || bytes == NULL) {
        if (session != NULL) snprintf(session->error, sizeof(session->error),
            "compilation path and source bytes must not be null");
        return SOL_COMPILATION_INVALID_ARGUMENT;
    }
    if (length == SIZE_MAX || memchr(bytes, '\0', length) != NULL) {
        snprintf(session->error, sizeof(session->error),
            length == SIZE_MAX ? "compilation source is too large"
                               : "compilation source contains an embedded NUL byte");
        return SOL_COMPILATION_INVALID_ARGUMENT;
    }
    if (session->state != SOL_SESSION_EMPTY) {
        snprintf(session->error, sizeof(session->error),
            "compilation session has already been used");
        return SOL_COMPILATION_INVALID_ARGUMENT;
    }
    if (!sol_compilation_begin(session)) {
        return session->outcome;
    }
    if (!sol_resource_charge_file() || !sol_resource_charge_source(length)) {
        sol_compilation_set_resource_error(session);
        return sol_compilation_finish(session, SOL_COMPILATION_RESOURCE_FAILED);
    }
    char *text = malloc(length + 1);
    if (text != NULL) {
        memcpy(text, bytes, length);
        text[length] = '\0';
    }
    bool completed = text != NULL
        && sol_compilation_make_text_package(session, path, text);
    free(text);
    SolCompilationOutcome outcome = sol_compilation_phase_outcome(session, completed);
    if (outcome == SOL_COMPILATION_SUCCEEDED) outcome = sol_compilation_run(session);
    if (outcome == SOL_COMPILATION_RESOURCE_FAILED) {
        sol_compilation_set_resource_error(session);
    }
    return sol_compilation_finish(session, outcome);
}

static bool sol_compilation_frontend_available(const SolCompilationSession *session) {
    return session != NULL && session->state != SOL_SESSION_EMPTY
        && session->state != SOL_SESSION_IR_TAKEN && session->frontend_live;
}

bool sol_compilation_summary(
    const SolCompilationSession *session,
    SolCompilationSummary *summary
) {
    if (!sol_compilation_frontend_available(session) || summary == NULL) return false;
    *summary = (SolCompilationSummary){
        .path = session->package.path,
        .is_directory = session->package.is_directory,
        .file_count = session->package.file_count,
        .declaration_count = session->package.syntax.item_count,
        .diagnostic_count = session->diagnostics.count,
        .hir_definition_count = session->hir.definition_count,
        .hir_file_scope_count = session->hir.file_scope_count,
        .type_definition_count = session->types.definition_count,
        .effect_function_count = session->effects.function_count,
        .contract_obligation_count = session->contracts.obligation_count,
    };
    return true;
}

size_t sol_compilation_diagnostic_count(const SolCompilationSession *session) {
    return sol_compilation_frontend_available(session)
        ? session->diagnostics.count : 0;
}

bool sol_compilation_diagnostic_at(
    const SolCompilationSession *session,
    size_t index,
    SolCompilationDiagnosticView *diagnostic
) {
    if (!sol_compilation_frontend_available(session) || diagnostic == NULL
        || index >= session->diagnostics.count) return false;
    const SolDiagnostic *entry = &session->diagnostics.items[index];
    *diagnostic = (SolCompilationDiagnosticView){
        .severity = entry->severity == SOL_SEVERITY_WARNING
            ? SOL_COMPILATION_DIAGNOSTIC_WARNING
            : SOL_COMPILATION_DIAGNOSTIC_ERROR,
        .span = {entry->span.start, entry->span.end},
        .code = entry->code,
        .message = entry->message,
    };
    return true;
}

bool sol_compilation_diagnostics_render_human(
    FILE *stream,
    const SolCompilationSession *session
) {
    if (stream == NULL || !sol_compilation_frontend_available(session)) return false;
    sol_package_diagnostics_render_human(
        stream, &session->package, &session->diagnostics);
    return ferror(stream) == 0;
}

bool sol_compilation_diagnostics_render_json(
    FILE *stream,
    const SolCompilationSession *session
) {
    if (stream == NULL || !sol_compilation_frontend_available(session)) return false;
    sol_package_diagnostics_render_json(
        stream, &session->package, &session->diagnostics);
    return ferror(stream) == 0;
}

bool sol_compilation_inspection_render(
    FILE *stream,
    const SolCompilationSession *session
) {
    return stream != NULL && session != NULL
        && session->state == SOL_SESSION_COMPILED && session->frontend_live
        && sol_inspection_render(stream, &session->package, &session->hir,
            &session->types, &session->effects, &session->contracts,
            &session->diagnostics);
}

const char *sol_compilation_error(const SolCompilationSession *session) {
    return session == NULL || session->error[0] == '\0' ? NULL : session->error;
}

SolCompilationOutcome sol_compilation_take_ir(
    SolCompilationSession *session,
    SolValidatedIr **validated
) {
    if (session == NULL || validated == NULL) {
        if (session != NULL) snprintf(session->error, sizeof(session->error),
            "validated IR output must not be null");
        return SOL_COMPILATION_INVALID_ARGUMENT;
    }
    if (*validated != NULL) {
        snprintf(session->error, sizeof(session->error),
            "validated IR output must be null");
        return SOL_COMPILATION_INVALID_ARGUMENT;
    }
    if (session->state != SOL_SESSION_COMPILED || session->validated == NULL) {
        snprintf(session->error, sizeof(session->error),
            "validated IR is available exactly once after successful compilation");
        return SOL_COMPILATION_INVALID_ARGUMENT;
    }
    *validated = session->validated;
    session->validated = NULL;
    sol_compilation_frontend_free(session);
    session->state = SOL_SESSION_IR_TAKEN;
    session->outcome = SOL_COMPILATION_SUCCEEDED;
    return SOL_COMPILATION_SUCCEEDED;
}

void sol_compilation_free(SolCompilationSession *session) {
    if (session == NULL) return;
    sol_compilation_frontend_free(session);
    sol_validated_ir_free(session->validated);
    free(session);
}

size_t sol_validated_ir_definition_count(const SolValidatedIr *validated) {
    return validated == NULL ? 0 : validated->ir.definition_count;
}

size_t sol_validated_ir_file_count(const SolValidatedIr *validated) {
    return validated == NULL ? 0 : validated->ir.file_count;
}

bool sol_validated_ir_definition_at(
    const SolValidatedIr *validated,
    size_t index,
    SolValidatedDefinitionView *definition
) {
    if (validated == NULL || definition == NULL
        || index >= validated->ir.definition_count) return false;
    const SolIrDefinition *entry = &validated->ir.definitions[index];
    *definition = (SolValidatedDefinitionView){
        .kind = (SolValidatedDefinitionKind)entry->kind,
        .callable = entry->callable,
        .span = {entry->span.start, entry->span.end},
        .name = entry->name,
    };
    return true;
}

const char *sol_validated_ir_path_at(
    const SolValidatedIr *validated,
    SolCompilationSpan span
) {
    if (validated == NULL) return NULL;
    const SolIr *ir = &validated->ir;
    for (size_t index = 0; index < ir->file_count; ++index) {
        if (span.start >= ir->files[index].aggregate_start
            && span.start < ir->files[index].aggregate_end) {
            return ir->files[index].path;
        }
    }
    return ir->source_path;
}

bool sol_validated_ir_interpret(
    const SolValidatedIr *validated,
    const SolInterpreterRequest *request,
    SolInterpreterResult *result
) {
    if (validated == NULL || request == NULL) {
        return sol_interpret(NULL, result);
    }
    if (result == NULL) return false;
    if (request->host_operation != NULL) {
        sol_interpreter_result_init(result);
        result->diagnostic.code = SOL_INTERPRETER_INVALID_REQUEST;
        (void)snprintf(result->diagnostic.message,
            sizeof(result->diagnostic.message),
            "validated IR handles do not expose raw IR host callbacks; a safe host profile is not yet available");
        return false;
    }
    SolInterpreterRequest private_request = *request;
    private_request.ir = &validated->ir;
    return sol_interpret(&private_request, result);
}

bool sol_validated_ir_effects_render(
    FILE *stream,
    const SolValidatedIr *validated,
    bool json
) {
    return stream != NULL && validated != NULL
        && sol_effects_render(stream, &validated->ir, json);
}

void sol_validated_ir_free(SolValidatedIr *validated) {
    if (validated == NULL) return;
    sol_ir_free(&validated->ir);
    free(validated);
}
