#ifndef SOL_INTERPRETER_H
#define SOL_INTERPRETER_H

#include "sol/ir.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SOL_INTERPRETER_VALUE_INVALID,
    SOL_INTERPRETER_VALUE_INT64,
    SOL_INTERPRETER_VALUE_BOOL,
    SOL_INTERPRETER_VALUE_TEXT,
    SOL_INTERPRETER_VALUE_UNIT,
    SOL_INTERPRETER_VALUE_TUPLE,
    SOL_INTERPRETER_VALUE_RECORD,
    SOL_INTERPRETER_VALUE_ENUM,
    SOL_INTERPRETER_VALUE_OPTION,
    SOL_INTERPRETER_VALUE_RESULT,
    SOL_INTERPRETER_VALUE_DISTINCT,
    SOL_INTERPRETER_VALUE_CAPABILITY,
    SOL_INTERPRETER_VALUE_FUNCTION,
    SOL_INTERPRETER_VALUE_BOUND_OPERATION,
} SolInterpreterValueKind;

typedef struct SolInterpreterValue SolInterpreterValue;

typedef struct {
    char *bytes;
    size_t length;
} SolInterpreterText;

typedef struct {
    SolIrDefinitionId definition;
    SolIrVariantId variant;
    SolInterpreterValue *fields;
    size_t field_count;
} SolInterpreterAggregate;

typedef struct {
    bool has_value;
    bool is_error;
    SolInterpreterValue *value;
} SolInterpreterSum;

typedef struct {
    SolIrDefinitionId definition;
    SolInterpreterValue *value;
} SolInterpreterDistinct;

typedef struct {
    SolIrDefinitionId definition;
    void *root;
    SolInterpreterValue *source;
} SolInterpreterCapability;

typedef struct {
    SolIrCallableId callable;
    SolInterpreterValue *receiver;
} SolInterpreterCallableValue;

struct SolInterpreterValue {
    SolInterpreterValueKind kind;
    union {
        int64_t integer;
        bool boolean;
        SolInterpreterText text;
        SolInterpreterAggregate aggregate;
        SolInterpreterSum sum;
        SolInterpreterDistinct distinct;
        SolInterpreterCapability capability;
        SolInterpreterCallableValue callable;
    } as;
};

typedef enum {
    SOL_INTERPRETER_CONTRACTS_IGNORE,
    SOL_INTERPRETER_CONTRACTS_CHECK,
} SolInterpreterContractPolicy;

typedef struct {
    size_t steps;
    size_t call_depth;
    size_t value_nodes;
    size_t text_bytes;
    size_t host_calls;
} SolInterpreterLimits;

/* Unstable synchronous, infallible cleanup observation hook for tests/tools. */
typedef void (*SolInterpreterCleanupObserver)(
    void *context,
    SolIrLocalId local,
    size_t ordinal
);

typedef enum {
    SOL_INTERPRETER_OK,
    SOL_INTERPRETER_INVALID_REQUEST,
    SOL_INTERPRETER_INVALID_IR,
    SOL_INTERPRETER_UNSUPPORTED_CONTRACT_POLICY,
    SOL_INTERPRETER_TYPE_INVARIANT,
    SOL_INTERPRETER_UNBOUND_OPERATION,
    SOL_INTERPRETER_HOST_ERROR,
    SOL_INTERPRETER_INTEGER_OVERFLOW,
    SOL_INTERPRETER_DIVISION_BY_ZERO,
    SOL_INTERPRETER_STEP_LIMIT,
    SOL_INTERPRETER_CALL_DEPTH_LIMIT,
    SOL_INTERPRETER_VALUE_LIMIT,
    SOL_INTERPRETER_TEXT_LIMIT,
    SOL_INTERPRETER_HOST_CALL_LIMIT,
    SOL_INTERPRETER_NO_MATCH,
    SOL_INTERPRETER_PANIC,
    SOL_INTERPRETER_REACHED_UNREACHABLE,
    SOL_INTERPRETER_REQUIRE_FALLBACK_RETURNED,
    SOL_INTERPRETER_INTERNAL_INVARIANT,
} SolInterpreterCode;

#define SOL_INTERPRETER_DIAGNOSTIC_FILE_CAPACITY 512

typedef struct {
    SolInterpreterCode code;
    SolSpan span;
    /* Owned, truncated source path. Empty when no source path is available. */
    char file[SOL_INTERPRETER_DIAGNOSTIC_FILE_CAPACITY];
    size_t file_offset;
    char message[192];
} SolInterpreterDiagnostic;

typedef struct {
    const SolIrDispatchEvidence *items;
    size_t count;
} SolInterpreterEvidence;

#define SOL_INTERPRETER_HOST_FAILURE_CAPACITY 191

typedef struct {
    /* Interpreter-owned text storage. length is authoritative; NUL is forbidden. */
    char bytes[SOL_INTERPRETER_HOST_FAILURE_CAPACITY];
    size_t length;
} SolInterpreterHostFailure;

/*
 * Raw trusted-interpreter callback. sol_validated_ir_interpret rejects requests
 * containing this callback because a validated handle never exposes its IR.
 * A safe application host profile is deferred to E3.
 *
 * The callback writes a borrowed value view to result. Its storage remains
 * host-owned, may alias callback inputs, and must remain valid until the
 * enclosing sol_interpret call returns. The interpreter validates and
 * deep-clones it synchronously. On failure, the callback may write bounded
 * bytes into the interpreter-owned failure buffer; length zero selects the
 * default failure message.
 */
typedef bool (*SolInterpreterHostOperation)(
    void *context,
    const SolIr *ir,
    SolIrCallableId operation,
    void *root,
    const SolInterpreterValue *private_source,
    const SolInterpreterValue *arguments,
    size_t argument_count,
    SolInterpreterValue *result,
    SolInterpreterHostFailure *failure
);

typedef struct SolInterpreterRequest {
    const SolIr *ir;
    SolIrCallableId callable;
    SolIrDefinitionId definition;
    const SolInterpreterValue *arguments;
    size_t argument_count;
    const SolIrTypeId *type_arguments;
    size_t type_argument_count;
    SolInterpreterEvidence evidence;
    SolInterpreterContractPolicy contracts;
    SolInterpreterLimits limits;
    SolInterpreterHostOperation host_operation;
    void *host_context;
    SolInterpreterCleanupObserver cleanup_observer;
    void *cleanup_context;
    /* Required to select a TEST callable; forbidden for ordinary functions. */
    bool test_entry;
} SolInterpreterRequest;

typedef struct SolInterpreterResult {
    SolInterpreterValue value;
    SolInterpreterDiagnostic diagnostic;
    SolInterpreterLimits used;
    size_t cleanup_actions;
} SolInterpreterResult;

void sol_interpreter_value_init(SolInterpreterValue *value);
void sol_interpreter_value_free(SolInterpreterValue *value);
bool sol_interpreter_value_clone(SolInterpreterValue *destination,
    const SolInterpreterValue *source);
bool sol_interpreter_value_int64(SolInterpreterValue *value, int64_t integer);
bool sol_interpreter_value_bool(SolInterpreterValue *value, bool boolean);
bool sol_interpreter_value_text(SolInterpreterValue *value,
    const char *bytes, size_t length);
bool sol_interpreter_value_unit(SolInterpreterValue *value);
bool sol_interpreter_value_option(SolInterpreterValue *value,
    const SolInterpreterValue *payload);
bool sol_interpreter_value_result(SolInterpreterValue *value, bool is_error,
    const SolInterpreterValue *payload);
bool sol_interpreter_value_capability(SolInterpreterValue *value,
    SolIrDefinitionId definition, void *root,
    const SolInterpreterValue *private_source);
void sol_interpreter_result_init(SolInterpreterResult *result);
void sol_interpreter_result_free(SolInterpreterResult *result);
bool sol_interpret(const SolInterpreterRequest *request, SolInterpreterResult *result);

#endif
