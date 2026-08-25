#ifndef SOL_COMPILATION_H
#define SOL_COMPILATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct SolCompilationSession SolCompilationSession;
typedef struct SolValidatedIr SolValidatedIr;
typedef struct SolInterpreterRequest SolInterpreterRequest;
typedef struct SolInterpreterResult SolInterpreterResult;

typedef enum {
    SOL_COMPILATION_SUCCEEDED,
    SOL_COMPILATION_REJECTED,
    SOL_COMPILATION_LOAD_FAILED,
    SOL_COMPILATION_RESOURCE_FAILED,
    SOL_COMPILATION_INVALID_ARGUMENT,
} SolCompilationOutcome;

typedef struct {
    size_t start;
    size_t end;
} SolCompilationSpan;

typedef struct {
    /* Borrowed from the session until take_ir or sol_compilation_free. */
    const char *path;
    bool is_directory;
    size_t file_count;
    size_t declaration_count;
    size_t diagnostic_count;
    size_t hir_definition_count;
    size_t hir_file_scope_count;
    size_t type_definition_count;
    size_t effect_function_count;
    size_t contract_obligation_count;
} SolCompilationSummary;

typedef enum {
    SOL_COMPILATION_DIAGNOSTIC_ERROR,
    SOL_COMPILATION_DIAGNOSTIC_WARNING,
} SolCompilationDiagnosticSeverity;

typedef struct {
    SolCompilationDiagnosticSeverity severity;
    SolCompilationSpan span;
    /* Borrowed from the session until take_ir or sol_compilation_free. */
    const char *code;
    const char *message;
} SolCompilationDiagnosticView;

typedef enum {
    SOL_VALIDATED_DEFINITION_RECORD,
    SOL_VALIDATED_DEFINITION_ENUM,
    SOL_VALIDATED_DEFINITION_DISTINCT,
    SOL_VALIDATED_DEFINITION_REFINED,
    SOL_VALIDATED_DEFINITION_CAPABILITY,
    SOL_VALIDATED_DEFINITION_FUNCTION,
    SOL_VALIDATED_DEFINITION_TRAIT,
    SOL_VALIDATED_DEFINITION_IMPLEMENTATION,
    SOL_VALIDATED_DEFINITION_TEST,
} SolValidatedDefinitionKind;

typedef struct {
    SolValidatedDefinitionKind kind;
    size_t callable;
    SolCompilationSpan span;
    /* Borrowed from the validated handle and valid until that handle is freed. */
    const char *name;
} SolValidatedDefinitionView;

SolCompilationSession *sol_compilation_create(void);
SolCompilationOutcome sol_compilation_compile_path(
    SolCompilationSession *session,
    const char *path
);
SolCompilationOutcome sol_compilation_compile_text(
    SolCompilationSession *session,
    const char *path,
    const char *text
);
/*
 * Compiles an exact byte sequence after copying it. Embedded NUL bytes and
 * lengths that cannot be NUL-terminated are rejected without consuming the
 * session. compile_text is the NUL-terminated convenience form.
 */
SolCompilationOutcome sol_compilation_compile_source(
    SolCompilationSession *session,
    const char *path,
    const void *bytes,
    size_t length
);
/* Caller owns the output value; its path is borrowed as documented above. */
bool sol_compilation_summary(
    const SolCompilationSession *session,
    SolCompilationSummary *summary
);
size_t sol_compilation_diagnostic_count(const SolCompilationSession *session);
/* Caller owns the output value; its strings are borrowed as documented above. */
bool sol_compilation_diagnostic_at(
    const SolCompilationSession *session,
    size_t index,
    SolCompilationDiagnosticView *diagnostic
);
bool sol_compilation_diagnostics_render_human(
    FILE *stream,
    const SolCompilationSession *session
);
bool sol_compilation_diagnostics_render_json(
    FILE *stream,
    const SolCompilationSession *session
);
/* Available only after a successful compile and before take_ir. */
bool sol_compilation_inspection_render(
    FILE *stream,
    const SolCompilationSession *session
);
/* Borrowed from the session and valid until sol_compilation_free. */
const char *sol_compilation_error(const SolCompilationSession *session);
/*
 * On entry, validated must be non-null and *validated must be NULL. On failure
 * caller storage is unchanged. The returned handle owns immutable validated IR.
 */
SolCompilationOutcome sol_compilation_take_ir(
    SolCompilationSession *session,
    SolValidatedIr **validated
);
void sol_compilation_free(SolCompilationSession *session);

size_t sol_validated_ir_definition_count(const SolValidatedIr *validated);
size_t sol_validated_ir_file_count(const SolValidatedIr *validated);
bool sol_validated_ir_definition_at(
    const SolValidatedIr *validated,
    size_t index,
    SolValidatedDefinitionView *definition
);
/* Returned paths are borrowed and valid until the validated handle is freed. */
const char *sol_validated_ir_path_at(
    const SolValidatedIr *validated,
    SolCompilationSpan span
);
/*
 * request->ir is ignored; the validated handle's private IR is always used.
 * Host-operation callbacks are rejected because they would expose private IR.
 */
bool sol_validated_ir_interpret(
    const SolValidatedIr *validated,
    const SolInterpreterRequest *request,
    SolInterpreterResult *result
);
bool sol_validated_ir_effects_render(
    FILE *stream,
    const SolValidatedIr *validated,
    bool json
);
void sol_validated_ir_free(SolValidatedIr *validated);

#endif
