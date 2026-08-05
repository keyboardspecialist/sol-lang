#ifndef SOL_SOURCE_H
#define SOL_SOURCE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *path;
    char *text;
    size_t length;
    size_t *line_starts;
    size_t line_count;
} SolSource;

typedef struct {
    size_t start;
    size_t end;
} SolSpan;

typedef struct {
    size_t line;
    size_t column;
} SolPosition;

bool sol_source_load(SolSource *source, const char *path, char *error, size_t error_size);
bool sol_source_from_text(SolSource *source, const char *path, const char *text);
void sol_source_free(SolSource *source);
SolPosition sol_source_position(const SolSource *source, size_t offset);

#endif
