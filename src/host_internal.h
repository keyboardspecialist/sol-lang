#ifndef SOL_HOST_INTERNAL_H
#define SOL_HOST_INTERNAL_H

#include "sol/compilation.h"
#include "sol/interpreter.h"

#include "validated_ir_internal.h"

typedef struct {
    SolIrDefinitionId capability;
    bool bound;
} SolHostRoot;

typedef struct {
    size_t parameter;
    SolIrCallableId callable;
    SolHostOperation callback;
    void *context;
} SolHostGrant;

struct SolHostRegistry {
    const SolValidatedIr *validated;
    SolHostRoot *roots;
    size_t root_count;
    SolHostGrant *grants;
    size_t grant_count;
    size_t grant_capacity;
    char error[192];
};

bool sol_host_operation_supported_internal(
    const SolIr *ir,
    SolIrCallableId callable
);

#endif
