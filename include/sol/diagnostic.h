#ifndef SOL_DIAGNOSTIC_H
#define SOL_DIAGNOSTIC_H

#include "sol/source.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef enum {
    SOL_SEVERITY_ERROR,
    SOL_SEVERITY_WARNING,
} SolSeverity;

typedef struct {
    char code[32];
    SolSeverity severity;
    char message[256];
    SolSpan span;
} SolDiagnostic;

typedef struct {
    SolDiagnostic *items;
    size_t count;
    size_t capacity;
    bool allocation_failed;
} SolDiagnostics;

void sol_diagnostics_init(SolDiagnostics *diagnostics);
void sol_diagnostics_free(SolDiagnostics *diagnostics);
bool sol_diagnostics_add(
    SolDiagnostics *diagnostics,
    const char *code,
    SolSeverity severity,
    SolSpan span,
    const char *format,
    ...
);
bool sol_diagnostics_has_errors(const SolDiagnostics *diagnostics);
void sol_diagnostics_render_human(
    FILE *stream,
    const SolSource *source,
    const SolDiagnostics *diagnostics
);
void sol_diagnostics_render_json(
    FILE *stream,
    const SolSource *source,
    const SolDiagnostics *diagnostics
);

#endif
