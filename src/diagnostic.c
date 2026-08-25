#include "sol/diagnostic.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include "resource_internal.h"
#include <string.h>

void sol_diagnostics_init(SolDiagnostics *diagnostics) {
    memset(diagnostics, 0, sizeof(*diagnostics));
}

void sol_diagnostics_free(SolDiagnostics *diagnostics) {
    free(diagnostics->items);
    memset(diagnostics, 0, sizeof(*diagnostics));
}

bool sol_diagnostics_add(
    SolDiagnostics *diagnostics,
    const char *code,
    SolSeverity severity,
    SolSpan span,
    const char *format,
    ...
) {
    if (!sol_resource_charge_diagnostic()) {
        diagnostics->allocation_failed = true;
        return false;
    }
    if (diagnostics->count == diagnostics->capacity) {
        if (diagnostics->capacity > SIZE_MAX / 2) {
            diagnostics->allocation_failed = true;
            return false;
        }
        size_t capacity = diagnostics->capacity == 0 ? 8 : diagnostics->capacity * 2;
        if (capacity > SIZE_MAX / sizeof(*diagnostics->items)) {
            diagnostics->allocation_failed = true;
            return false;
        }
        SolDiagnostic *items = realloc(diagnostics->items, capacity * sizeof(*items));
        if (items == NULL) {
            diagnostics->allocation_failed = true;
            return false;
        }
        diagnostics->items = items;
        diagnostics->capacity = capacity;
    }

    SolDiagnostic *diagnostic = &diagnostics->items[diagnostics->count++];
    memset(diagnostic, 0, sizeof(*diagnostic));
    snprintf(diagnostic->code, sizeof(diagnostic->code), "%s", code);
    diagnostic->severity = severity;
    diagnostic->span = span;

    va_list arguments;
    va_start(arguments, format);
    vsnprintf(diagnostic->message, sizeof(diagnostic->message), format, arguments);
    va_end(arguments);
    return true;
}

bool sol_diagnostics_has_errors(const SolDiagnostics *diagnostics) {
    if (diagnostics->allocation_failed) {
        return true;
    }
    for (size_t index = 0; index < diagnostics->count; ++index) {
        if (diagnostics->items[index].severity == SOL_SEVERITY_ERROR) {
            return true;
        }
    }
    return false;
}

static const char *sol_severity_name(SolSeverity severity) {
    return severity == SOL_SEVERITY_ERROR ? "error" : "warning";
}

void sol_diagnostics_render_human(
    FILE *stream,
    const SolSource *source,
    const SolDiagnostics *diagnostics
) {
    for (size_t index = 0; index < diagnostics->count; ++index) {
        const SolDiagnostic *diagnostic = &diagnostics->items[index];
        SolPosition position = sol_source_position(source, diagnostic->span.start);
        fprintf(
            stream,
            "%s:%zu:%zu: %s[%s]: %s\n",
            source->path,
            position.line,
            position.column,
            sol_severity_name(diagnostic->severity),
            diagnostic->code,
            diagnostic->message
        );
    }
}

static void sol_json_string(FILE *stream, const char *text) {
    fputc('"', stream);
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor != '\0'; ++cursor) {
        switch (*cursor) {
            case '"': fputs("\\\"", stream); break;
            case '\\': fputs("\\\\", stream); break;
            case '\b': fputs("\\b", stream); break;
            case '\f': fputs("\\f", stream); break;
            case '\n': fputs("\\n", stream); break;
            case '\r': fputs("\\r", stream); break;
            case '\t': fputs("\\t", stream); break;
            default:
                if (*cursor < 0x20) {
                    fprintf(stream, "\\u%04x", (unsigned int)*cursor);
                } else {
                    fputc((int)*cursor, stream);
                }
        }
    }
    fputc('"', stream);
}

void sol_diagnostics_render_json(
    FILE *stream,
    const SolSource *source,
    const SolDiagnostics *diagnostics
) {
    fputs("[", stream);
    for (size_t index = 0; index < diagnostics->count; ++index) {
        const SolDiagnostic *diagnostic = &diagnostics->items[index];
        SolPosition start = sol_source_position(source, diagnostic->span.start);
        SolPosition end = sol_source_position(source, diagnostic->span.end);
        if (index != 0) {
            fputs(",", stream);
        }
        fputs("{\"schema\":\"sol.diagnostic/1\",\"code\":", stream);
        sol_json_string(stream, diagnostic->code);
        fputs(",\"severity\":", stream);
        sol_json_string(stream, sol_severity_name(diagnostic->severity));
        fputs(",\"message\":", stream);
        sol_json_string(stream, diagnostic->message);
        fputs(",\"locations\":[{\"file\":", stream);
        sol_json_string(stream, source->path);
        fprintf(
            stream,
            ",\"start\":{\"line\":%zu,\"column\":%zu},"
            "\"end\":{\"line\":%zu,\"column\":%zu},\"role\":\"primary\"}]}",
            start.line,
            start.column,
            end.line,
            end.column
        );
    }
    fputs("]\n", stream);
}
