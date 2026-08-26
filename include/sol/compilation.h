#ifndef SOL_COMPILATION_H
#define SOL_COMPILATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct SolCompilationSession SolCompilationSession;
typedef struct SolValidatedIr SolValidatedIr;
typedef struct SolInterpreterRequest SolInterpreterRequest;
typedef struct SolInterpreterResult SolInterpreterResult;
typedef struct SolInterpreterHostFailure SolInterpreterHostFailure;
typedef struct SolHostRegistry SolHostRegistry;

#define SOL_HOST_ROOT_LIMIT 64
#define SOL_HOST_OPERATION_LIMIT 256

typedef enum {
    SOL_COMPILATION_SUCCEEDED,
    SOL_COMPILATION_REJECTED,
    SOL_COMPILATION_LOAD_FAILED,
    SOL_COMPILATION_RESOURCE_FAILED,
    SOL_COMPILATION_INVALID_ARGUMENT,
} SolCompilationOutcome;

typedef struct {
    size_t source_bytes_per_file;
    size_t package_source_bytes;
    size_t source_files;
    size_t directory_depth;
    size_t directory_entries;
    size_t tokens;
    size_t arena_entries;
    size_t diagnostics;
    size_t allocation_bytes;
    size_t allocation_count;
} SolCompilationLimits;

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
    bool is_entrypoint;
} SolValidatedDefinitionView;

typedef enum {
    SOL_ENTRYPOINT_RESULT_UNIT,
    SOL_ENTRYPOINT_RESULT_INT64,
} SolEntrypointResultKind;

typedef struct {
    size_t definition;
    size_t callable;
    size_t parameter_count;
    SolEntrypointResultKind result;
    SolCompilationSpan span;
    /* Borrowed from the validated handle and valid until that handle is freed. */
    const char *name;
} SolEntrypointView;

typedef struct {
    size_t capability_definition;
    /* Borrowed from the validated handle and valid until that handle is freed. */
    const char *name;
    const char *capability;
} SolEntrypointParameterView;

typedef enum {
    SOL_HOST_VALUE_INVALID,
    SOL_HOST_VALUE_INT64,
    SOL_HOST_VALUE_BOOL,
    SOL_HOST_VALUE_TEXT,
    SOL_HOST_VALUE_UNIT,
    SOL_HOST_VALUE_OPTION,
    SOL_HOST_VALUE_RESULT,
} SolHostValueKind;

typedef struct SolHostValue SolHostValue;

struct SolHostValue {
    SolHostValueKind kind;
    union {
        int64_t integer;
        bool boolean;
        struct {
            const char *bytes;
            size_t length;
        } text;
        struct {
            const SolHostValue *value;
            bool is_error;
        } sum;
    } as;
};

/*
 * Safe host callback for one explicitly allowed root-capability operation.
 * Argument views are borrowed for the callback duration and the result may
 * alias them. Other storage referenced by the result remains host-owned and
 * valid through hosted interpretation.
 * No IR, mutable interpreter value, callable, root, or private source is exposed.
 */
typedef bool (*SolHostOperation)(
    void *context,
    const SolHostValue *arguments,
    size_t argument_count,
    SolHostValue *result,
    SolInterpreterHostFailure *failure
);

SolCompilationSession *sol_compilation_create(void);
void sol_compilation_limits_default(SolCompilationLimits *limits);
SolCompilationSession *sol_compilation_create_with_limits(
    const SolCompilationLimits *limits
);
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
/* False means the validated program is a library with no declared entrypoint. */
bool sol_validated_ir_entrypoint(
    const SolValidatedIr *validated,
    SolEntrypointView *entrypoint
);
bool sol_validated_ir_entrypoint_parameter_at(
    const SolValidatedIr *validated,
    size_t index,
    SolEntrypointParameterView *parameter
);
/* Maps a successful entrypoint result to the exact process status ABI. */
bool sol_validated_ir_entrypoint_exit_status(
    const SolValidatedIr *validated,
    const SolInterpreterResult *result,
    int *status
);
/* The validated handle must outlive its registry. */
SolHostRegistry *sol_host_registry_create(const SolValidatedIr *validated);
void sol_host_registry_free(SolHostRegistry *registry);
/* Binds one exact entrypoint capability parameter to a fresh opaque root. */
bool sol_host_registry_bind_root(SolHostRegistry *registry, size_t parameter);
/* Allows one bodyless data-only member on that exact root. */
bool sol_host_registry_allow(
    SolHostRegistry *registry,
    size_t parameter,
    const char *operation,
    SolHostOperation callback,
    void *context
);
const char *sol_host_registry_error(const SolHostRegistry *registry);
/*
 * Preflights all declared authority before invoking the entrypoint. Only the
 * request's contracts and limits fields are read.
 */
bool sol_validated_ir_interpret_entrypoint(
    const SolValidatedIr *validated,
    const SolHostRegistry *registry,
    const SolInterpreterRequest *request,
    SolInterpreterResult *result
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
