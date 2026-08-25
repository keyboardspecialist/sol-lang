#ifndef SOL_RESOURCE_INTERNAL_H
#define SOL_RESOURCE_INTERNAL_H

#include "sol/compilation.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    SolCompilationLimits limits;
    size_t source_bytes;
    size_t files;
    size_t directory_entries;
    size_t tokens;
    size_t arena_entries;
    size_t diagnostics;
    size_t allocation_bytes;
    size_t allocation_count;
    const char *failure;
} SolResourceBudget;

void sol_resource_begin(SolResourceBudget *budget);
void sol_resource_end(SolResourceBudget *budget);
const char *sol_resource_failure(const SolResourceBudget *budget);
bool sol_resource_charge_source(size_t bytes);
bool sol_resource_charge_file(void);
bool sol_resource_charge_directory(size_t depth);
bool sol_resource_charge_token(void);
bool sol_resource_charge_arena(size_t entries);
bool sol_resource_charge_diagnostic(void);

void *sol_resource_malloc(size_t size);
void *sol_resource_calloc(size_t count, size_t size);
void *sol_resource_realloc(void *allocation, size_t size);

#define malloc(size) sol_resource_malloc(size)
#define calloc(count, size) sol_resource_calloc(count, size)
#define realloc(allocation, size) sol_resource_realloc(allocation, size)

#endif
