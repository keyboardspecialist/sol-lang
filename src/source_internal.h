#ifndef SOL_SOURCE_INTERNAL_H
#define SOL_SOURCE_INTERNAL_H

#include "sol/source.h"

typedef enum {
    SOL_SOURCE_LOAD_SUCCEEDED,
    SOL_SOURCE_LOAD_IO_FAILED,
    SOL_SOURCE_LOAD_RESOURCE_FAILED,
} SolSourceLoadOutcome;

SolSourceLoadOutcome sol_source_load_outcome(
    SolSource *source,
    const char *path,
    char *error,
    size_t error_size
);

#endif
