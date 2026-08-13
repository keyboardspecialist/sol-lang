#ifndef SOL_FORMATTER_H
#define SOL_FORMATTER_H

#include "sol/diagnostic.h"
#include "sol/source.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *text;
    size_t length;
    size_t capacity;
} SolFormatted;

void sol_formatted_init(SolFormatted *formatted);
void sol_formatted_free(SolFormatted *formatted);
bool sol_format_source(
    const SolSource *source,
    SolFormatted *formatted,
    SolDiagnostics *diagnostics
);

#endif
