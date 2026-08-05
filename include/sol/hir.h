#ifndef SOL_HIR_H
#define SOL_HIR_H

#include "sol/diagnostic.h"
#include "sol/parser.h"

#include <stdbool.h>
#include <stddef.h>

typedef size_t SolDefId;
typedef size_t SolLocalId;

typedef struct {
    SolItemKind kind;
    SolSpan name;
    size_t syntax_item;
} SolHirDefinition;

typedef enum {
    SOL_LOCAL_PARAMETER,
    SOL_LOCAL_BINDING,
} SolLocalKind;

typedef struct {
    SolLocalKind kind;
    SolSpan name;
    SolDefId owner;
    size_t syntax_id;
} SolHirLocal;

typedef enum {
    SOL_RESOLUTION_NOT_APPLICABLE,
    SOL_RESOLUTION_ERROR,
    SOL_RESOLUTION_DEFINITION,
    SOL_RESOLUTION_LOCAL,
    SOL_RESOLUTION_BUILTIN,
} SolResolutionKind;

typedef enum {
    SOL_BUILTIN_OK,
    SOL_BUILTIN_ERR,
    SOL_BUILTIN_SOME,
    SOL_BUILTIN_NONE,
} SolBuiltin;

typedef struct {
    SolResolutionKind kind;
    size_t target;
} SolResolution;

typedef struct {
    SolHirDefinition *definitions;
    size_t definition_count;
    SolHirLocal *locals;
    size_t local_count;
    size_t local_capacity;
    SolResolution *resolutions;
    size_t resolution_count;
} SolHirModule;

void sol_hir_module_init(SolHirModule *module);
void sol_hir_module_free(SolHirModule *module);
bool sol_hir_lower(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    SolHirModule *module,
    SolDiagnostics *diagnostics
);

#endif
