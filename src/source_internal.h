#ifndef SOL_SOURCE_INTERNAL_H
#define SOL_SOURCE_INTERNAL_H

#include "sol/source.h"

#include <stdint.h>

typedef enum {
    SOL_SOURCE_LOAD_SUCCEEDED,
    SOL_SOURCE_LOAD_IO_FAILED,
    SOL_SOURCE_LOAD_RESOURCE_FAILED,
} SolSourceLoadOutcome;

typedef struct {
    uintmax_t device;
    uintmax_t inode;
} SolSourceIdentity;

SolSourceLoadOutcome sol_source_load_outcome(
    SolSource *source,
    const char *path,
    char *error,
    size_t error_size
);

SolSourceLoadOutcome sol_source_load_expected(
    SolSource *source,
    const char *path,
    const SolSourceIdentity *expected,
    char *error,
    size_t error_size
);

#endif
