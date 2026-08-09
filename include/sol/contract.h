#ifndef SOL_CONTRACT_H
#define SOL_CONTRACT_H

#include "sol/diagnostic.h"
#include "sol/effectcheck.h"
#include "sol/hir.h"
#include "sol/parser.h"
#include "sol/typecheck.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t SolObligationId;
typedef size_t SolSnapshotId;

typedef struct {
    bool available;
    SolType type;
} SolResultBinding;

typedef struct {
    SolObligationId id;
    SolContractConditionId condition;
    SolContractOwnerKind owner_kind;
    size_t owner;
    SolContractClauseKind kind;
    SolContractOutcomeKind outcome;
    SolExprId predicate;
    SolType predicate_type;
    SolResultBinding result;
    SolSnapshotId first_snapshot;
    size_t snapshot_count;
} SolObligation;

typedef struct {
    SolSnapshotId id;
    SolObligationId obligation;
    SolExprId old_expression;
    SolExprId operand;
    SolType type;
} SolSnapshot;

typedef struct {
    SolObligation *obligations;
    size_t obligation_count;
    SolSnapshot *snapshots;
    size_t snapshot_count;
    size_t snapshot_capacity;
    /* Indexed by SolExprId; SOL_AST_NONE means the expression is not old(...). */
    SolSnapshotId *expression_snapshots;
    size_t expression_count;
} SolContractTable;

void sol_contract_table_init(SolContractTable *table);
void sol_contract_table_free(SolContractTable *table);

bool sol_contract_lower(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolHirModule *hir,
    const SolTypeTable *types,
    const SolEffectTable *effects,
    SolContractTable *contracts,
    SolDiagnostics *diagnostics
);

#endif
