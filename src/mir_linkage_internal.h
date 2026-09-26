#ifndef SOL_MIR_LINKAGE_INTERNAL_H
#define SOL_MIR_LINKAGE_INTERNAL_H

#include "sol/mir_linkage.h"

typedef struct {
    size_t used;
    size_t limit;
    bool exhausted;
} SolMirLinkageWorkMeter;

bool sol_mir_linkage_internal_work_tick(
    SolMirLinkageWorkMeter *meter, size_t amount);

typedef struct {
    uint32_t state[8];
    uint64_t bits;
    uint8_t block[64];
    size_t used;
    bool valid;
    SolMirLinkageWorkMeter *work;
} SolMirLinkageSha256;

void sol_mir_linkage_internal_sha256_init(SolMirLinkageSha256 *sha);
void sol_mir_linkage_internal_sha256_write(SolMirLinkageSha256 *sha,
    const void *bytes, size_t length);
bool sol_mir_linkage_internal_sha256_finish(SolMirLinkageSha256 *sha,
    SolMirLinkageDigest *digest);

bool sol_mir_linkage_internal_instance_key(
    const SolMirLinkage *linkage, SolMirPlanInstanceId instance,
    SolMirLinkageDigest *digest, SolMirLinkageWorkMeter *work);
bool sol_mir_linkage_internal_host_key(
    const SolMirLinkage *linkage, SolMirMaterializedImportId import,
    SolMirLinkageDigest *digest, SolMirLinkageWorkMeter *work);
bool sol_mir_linkage_internal_recipe_key(
    const SolMirLinkage *linkage, SolMirRecipeId recipe,
    SolMirLinkageDigest *digest, SolMirLinkageWorkMeter *work);
bool sol_mir_linkage_internal_table_key(
    const SolMirLinkage *linkage, SolMirLinkageTargetKind kind,
    SolMirLinkageCallableId internal, SolMirLinkageHostRequirementId host,
    SolMirLinkageDigest *digest, SolMirLinkageWorkMeter *work);
bool sol_mir_linkage_internal_instance_descriptor_equal(
    const SolMirLinkage *linkage, SolMirPlanInstanceId left,
    SolMirPlanInstanceId right, bool *equal, SolMirLinkageWorkMeter *work);
bool sol_mir_linkage_internal_host_descriptor_equal(
    const SolMirLinkage *linkage, SolMirMaterializedImportId left,
    SolMirMaterializedImportId right, bool *equal,
    SolMirLinkageWorkMeter *work);
bool sol_mir_linkage_internal_recipe_descriptor_equal(
    const SolMirLinkage *linkage, SolMirRecipeId left, SolMirRecipeId right,
    bool *equal, SolMirLinkageWorkMeter *work);
bool sol_mir_linkage_internal_table_descriptor_equal(
    const SolMirLinkage *linkage, SolMirLinkageTargetKind left_kind,
    SolMirLinkageCallableId left_internal,
    SolMirLinkageHostRequirementId left_host,
    SolMirLinkageTargetKind right_kind,
    SolMirLinkageCallableId right_internal,
    SolMirLinkageHostRequirementId right_host, bool *equal,
    SolMirLinkageWorkMeter *work);
void sol_mir_linkage_internal_symbol(char namespace_kind,
    SolSemanticId semantic_id, const SolMirLinkageDigest *digest,
    SolMirLinkageSymbol *symbol);
bool sol_mir_linkage_internal_expected_usage(
    const SolMirLinkage *linkage, SolMirLinkageUsage *usage);
bool sol_mir_linkage_internal_validation_scratch(
    const SolMirLinkage *linkage, size_t *bytes);
bool sol_mir_linkage_internal_validation_requirements(
    const SolMirLinkage *linkage, size_t *work, size_t *scratch,
    SolDiagnostics *diagnostics);

#ifdef SOL_MIR_PLAN_TEST_HOOKS
void sol_mir_linkage_test_force_instance_digest_collision(bool force);
void sol_mir_linkage_test_force_host_digest_collision(bool force);
void sol_mir_linkage_test_force_runtime_digest_collision(bool force);
void sol_mir_linkage_test_force_table_digest_collision(bool force);
size_t sol_mir_linkage_test_last_descriptor_work_start(void);
size_t sol_mir_linkage_test_last_sort_work_start(void);
bool sol_mir_linkage_test_sha256(const void *bytes, size_t length,
    SolMirLinkageDigest *digest);
bool sol_mir_linkage_test_sha256_with_limit(const void *bytes, size_t length,
    size_t limit, SolMirLinkageDigest *digest, size_t *used);
#endif

#endif
