#ifndef SOL_HIR_H
#define SOL_HIR_H

#include "sol/diagnostic.h"
#include "sol/parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef size_t SolDefId;
typedef size_t SolLocalId;

typedef struct {
    uint64_t high;
    uint64_t low;
} SolSemanticId;

typedef enum {
    SOL_TYPE_RESOLUTION_ERROR,
    SOL_TYPE_RESOLUTION_BUILTIN,
    SOL_TYPE_RESOLUTION_DEFINITION,
    SOL_TYPE_RESOLUTION_PARAMETER,
    SOL_TYPE_RESOLUTION_SELF,
} SolTypeResolutionKind;

typedef enum {
    SOL_TYPE_BUILTIN_INT64,
    SOL_TYPE_BUILTIN_BOOL,
    SOL_TYPE_BUILTIN_TEXT,
    SOL_TYPE_BUILTIN_OPTION,
    SOL_TYPE_BUILTIN_RESULT,
} SolTypeBuiltin;

typedef struct {
    SolTypeResolutionKind kind;
    size_t target;
} SolTypeResolution;

typedef enum {
    SOL_EFFECT_RESOLUTION_ATOM,
    SOL_EFFECT_RESOLUTION_PARAMETER,
    SOL_EFFECT_RESOLUTION_ERROR,
} SolEffectResolutionKind;

typedef struct {
    SolEffectResolutionKind kind;
    size_t target;
} SolEffectResolution;

typedef struct {
    SolItemKind kind;
    SolSpan name;
    SolSpan stable_identity;
    SolSemanticId semantic_id;
    size_t syntax_item;
} SolHirDefinition;

typedef enum {
    SOL_SEMANTIC_REFERENCE_DECLARATION,
    SOL_SEMANTIC_REFERENCE_IMPORT,
    SOL_SEMANTIC_REFERENCE_EXPRESSION,
    SOL_SEMANTIC_REFERENCE_TYPE,
    SOL_SEMANTIC_REFERENCE_TRAIT,
    SOL_SEMANTIC_REFERENCE_BOUND,
} SolSemanticReferenceKind;

typedef struct {
    SolSemanticReferenceKind kind;
    SolSpan span;
    SolDefId target;
    SolSemanticId target_id;
} SolSemanticReference;

typedef enum {
    SOL_LOCAL_PARAMETER,
    SOL_LOCAL_BINDING,
    SOL_LOCAL_PATTERN,
} SolLocalKind;

typedef struct {
    SolLocalKind kind;
    SolSpan name;
    SolDefId owner;
    size_t syntax_id;
    SolAccessMode access;
    bool mutable;
} SolHirLocal;

typedef enum {
    SOL_RESOLUTION_NOT_APPLICABLE,
    SOL_RESOLUTION_ERROR,
    SOL_RESOLUTION_DEFINITION,
    SOL_RESOLUTION_LOCAL,
    SOL_RESOLUTION_BUILTIN,
    SOL_RESOLUTION_REFINEMENT_SELF,
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
    SolSpan module_name;
    size_t import_start;
    size_t import_count;
    size_t item_start;
    size_t item_count;
} SolHirFileScope;

typedef struct {
    SolHirDefinition *definitions;
    size_t definition_count;
    SolHirLocal *locals;
    size_t local_count;
    size_t local_capacity;
    SolResolution *resolutions;
    size_t resolution_count;
    /* Indexed by SolExprId. */
    SolDefId *expression_owners;
    /* Indexed by SolTypeId; path types have declaration-owned resolutions. */
    SolTypeResolution *type_resolutions;
    size_t type_resolution_count;
    /* Indexed by SolEffectId and SolTypeId respectively. */
    SolEffectResolution *effect_resolutions;
    size_t effect_resolution_count;
    SolEffectResolution *type_effect_resolutions;
    size_t type_effect_resolution_count;
    /* Implementation trait heads and free-function inline bounds. */
    SolResolution *trait_resolutions;
    size_t trait_resolution_count;
    SolResolution *bound_resolutions;
    size_t bound_resolution_count;
    /* Stable symbol occurrences; targets remain valid independent of arena ordering. */
    SolSemanticReference *semantic_references;
    size_t semantic_reference_count;
    size_t semantic_reference_capacity;
    /* Indexed by SolImport when package-aware. */
    SolResolution *import_resolutions;
    size_t import_resolution_count;
    /* Owned package scope metadata; item_files is indexed by syntax item. */
    SolHirFileScope *file_scopes;
    size_t file_scope_count;
    size_t *item_files;
} SolHirModule;

void sol_hir_module_init(SolHirModule *module);
void sol_hir_module_free(SolHirModule *module);
bool sol_hir_lower(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    SolHirModule *module,
    SolDiagnostics *diagnostics
);
bool sol_hir_lower_scoped(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolHirFileScope *scopes,
    size_t scope_count,
    SolHirModule *module,
    SolDiagnostics *diagnostics
);

#endif
