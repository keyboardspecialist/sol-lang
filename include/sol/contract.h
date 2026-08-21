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

typedef enum {
    SOL_LOOP_OBLIGATION_INVARIANT_ENTRY,
    SOL_LOOP_OBLIGATION_INVARIANT_PRESERVATION,
    SOL_LOOP_OBLIGATION_DECREASES_NONNEGATIVE,
    SOL_LOOP_OBLIGATION_DECREASES_STRICT,
} SolLoopObligationKind;

typedef struct {
    size_t id;
    SolLoopObligationKind kind;
    SolStatementId loop_statement;
    SolDefId owner;
    SolCapabilityMemberId owner_member;
    SolTraitMethodId owner_trait_method;
    SolExprId expression;
    SolType expression_type;
    SolSpan span;
} SolLoopObligation;

typedef struct {
    size_t id;
    SolStatementId statement;
    SolDefId owner;
    SolCapabilityMemberId owner_member;
    SolTraitMethodId owner_trait_method;
    SolExprId proof;
    SolType proof_type;
    SolSpan span;
} SolUnreachableObligation;

typedef struct {
    SolObligation *obligations;
    size_t obligation_count;
    SolSnapshot *snapshots;
    size_t snapshot_count;
    size_t snapshot_capacity;
    /* Indexed by SolExprId; SOL_AST_NONE means the expression is not old(...). */
    SolSnapshotId *expression_snapshots;
    size_t expression_count;
    SolLoopObligation *loop_obligations;
    size_t loop_obligation_count;
    size_t loop_obligation_capacity;
    SolUnreachableObligation *unreachable_obligations;
    size_t unreachable_obligation_count;
    size_t unreachable_obligation_capacity;
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
