#include "sol/mir_linkage.h"
#include "mir_linkage_internal.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SolMirLinkageTableEntry entry;
} TableCandidate;

typedef struct {
    SolMirLinkage *out;
    SolDiagnostics *diagnostics;
    SolMirLinkageBuildOutcome outcome;
    SolMirLinkageWorkMeter work;
} Builder;

typedef struct { char *data; size_t length, capacity; bool failed; } Buffer;

#ifdef SOL_MIR_PLAN_TEST_HOOKS
static _Thread_local size_t last_descriptor_work_start;
static _Thread_local size_t last_sort_work_start;

size_t sol_mir_linkage_test_last_descriptor_work_start(void) {
    return last_descriptor_work_start;
}

size_t sol_mir_linkage_test_last_sort_work_start(void) {
    return last_sort_work_start;
}
#endif

static bool report(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics != NULL) sol_diagnostics_add(diagnostics,
        "SOL-MIR-LINKAGE-001", SOL_SEVERITY_ERROR, (SolSpan){0}, message);
    return false;
}

static bool fail(Builder *builder, SolMirLinkageBuildOutcome outcome,
    const char *message) {
    builder->outcome = outcome; return report(builder->diagnostics, message);
}

static bool add_size(size_t *value, size_t amount) {
    if (amount > SIZE_MAX - *value) return false;
    *value += amount; return true;
}

static bool mul_size(size_t left, size_t right, size_t *result) {
    if (left != 0 && right > SIZE_MAX / left) return false;
    *result = left * right; return true;
}

static bool charge(Builder *builder, size_t amount) {
    if (!sol_mir_linkage_internal_work_tick(&builder->work, amount))
        return fail(builder, SOL_MIR_LINKAGE_BUILD_RESOURCE_EXHAUSTED,
            "linkage build work limit exceeded");
    return true;
}

static bool canonical_result(Builder *builder, bool result) {
    if (result) return true;
    return builder->work.exhausted
        ? fail(builder, SOL_MIR_LINKAGE_BUILD_RESOURCE_EXHAUSTED,
            "linkage build work limit exceeded")
        : false;
}

typedef int (*ItemCompare)(const void *, const void *);

static bool compare_items(Builder *builder, const unsigned char *left,
    const unsigned char *right, size_t size, ItemCompare compare, int *order) {
    if (!charge(builder, size)) return false;
    *order = compare(left, right); return true;
}

static bool swap_items(Builder *builder, unsigned char *left,
    unsigned char *right, size_t size) {
    if (left == right) return true;
    size_t work;
    if (!mul_size(size, 3, &work) || !charge(builder, work)) return false;
    for (size_t i = 0; i < size; ++i) {
        unsigned char byte = left[i]; left[i] = right[i]; right[i] = byte;
    }
    return true;
}

static bool sift_down(Builder *builder, unsigned char *base, size_t start,
    size_t count, size_t size, ItemCompare compare) {
    size_t root = start;
    while (root < count / 2) {
        size_t child = root * 2 + 1;
        if (child + 1 < count) {
            int order;
            if (!compare_items(builder, base + child * size,
                    base + (child + 1) * size, size, compare, &order))
                return false;
            if (order < 0) ++child;
        }
        int order;
        if (!compare_items(builder, base + root * size,
                base + child * size, size, compare, &order)) return false;
        if (order >= 0) return true;
        if (!swap_items(builder, base + root * size, base + child * size,
                size)) return false;
        root = child;
    }
    return true;
}

static bool sort_items(Builder *builder, void *items, size_t count,
    size_t size, ItemCompare compare) {
    if (count < 2) return true;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (last_sort_work_start == SIZE_MAX)
        last_sort_work_start = builder->work.used;
#endif
    unsigned char *base = items;
    for (size_t start = count / 2; start != 0; --start)
        if (!sift_down(builder, base, start - 1, count, size, compare))
            return false;
    for (size_t end = count; end > 1; --end) {
        if (!swap_items(builder, base, base + (end - 1) * size, size)
            || !sift_down(builder, base, 0, end - 1, size, compare))
            return false;
    }
    return true;
}

static bool limits_zero(SolMirLinkageLimits limits) {
    return limits.max_callables == 0 && limits.max_bindings == 0
        && limits.max_entry_exports == 0 && limits.max_table_entries == 0
        && limits.max_callable_values == 0 && limits.max_host_requirements == 0
        && limits.max_runtime_requirements == 0 && limits.max_owned_bytes == 0
        && limits.max_build_scratch_bytes == 0 && limits.max_build_work == 0
        && limits.max_validation_scratch_bytes == 0
        && limits.max_validation_work == 0;
}

static bool limits_complete(SolMirLinkageLimits limits) {
#define REQUIRED(name) limits.name != 0
    return REQUIRED(max_callables) && REQUIRED(max_bindings)
        && REQUIRED(max_entry_exports) && REQUIRED(max_table_entries)
        && REQUIRED(max_callable_values) && REQUIRED(max_host_requirements)
        && REQUIRED(max_runtime_requirements) && REQUIRED(max_owned_bytes)
        && REQUIRED(max_build_scratch_bytes) && REQUIRED(max_build_work)
        && REQUIRED(max_validation_scratch_bytes)
        && REQUIRED(max_validation_work);
#undef REQUIRED
}

static bool owner_empty(const SolMirLinkage *linkage) {
    if (linkage == NULL || linkage->operations != NULL) return false;
#define EMPTY(member, type, singular) \
    if (linkage->member != NULL || linkage->singular##_count != 0 \
        || linkage->singular##_capacity != 0) return false;
    SOL_MIR_LINKAGE_ARENAS(EMPTY)
#undef EMPTY
    return limits_zero(linkage->limits)
        && linkage->usage.callables == 0 && linkage->usage.bindings == 0
        && linkage->usage.entry_exports == 0
        && linkage->usage.table_entries == 0
        && linkage->usage.callable_values == 0
        && linkage->usage.host_requirements == 0
        && linkage->usage.runtime_requirements == 0
        && linkage->usage.owned_bytes == 0
        && linkage->usage.build_scratch_bytes == 0
        && linkage->usage.build_work == 0
        && linkage->usage.validation_scratch_bytes == 0
        && linkage->usage.validation_work == 0;
}

void sol_mir_linkage_init(SolMirLinkage *linkage) {
    if (linkage != NULL) memset(linkage, 0, sizeof(*linkage));
}

void sol_mir_linkage_free(SolMirLinkage *linkage) {
    if (linkage == NULL) return;
#define FREE(member, type, singular) free(linkage->member);
    SOL_MIR_LINKAGE_ARENAS(FREE)
#undef FREE
    sol_mir_linkage_init(linkage);
}

SolMirLinkageLimits sol_mir_linkage_default_limits(void) {
    return (SolMirLinkageLimits){
        .max_callables = 4096,
        .max_bindings = 2000000,
        .max_entry_exports = 4096,
        .max_table_entries = 2000000,
        .max_callable_values = 12000000,
        .max_host_requirements = 2000000,
        .max_runtime_requirements = 65536,
        .max_owned_bytes = 512u * 1024u * 1024u,
        .max_build_scratch_bytes = 256u * 1024u * 1024u,
        .max_build_work = 1000000000,
        .max_validation_scratch_bytes = 1536u * 1024u * 1024u,
        .max_validation_work = (size_t)4000000000ULL,
    };
}

static void *allocate(Builder *builder, size_t count, size_t size) {
    if (count == 0) return NULL;
    size_t bytes;
    if (!mul_size(count, size, &bytes)
        || !add_size(&builder->out->usage.owned_bytes, bytes)
        || builder->out->usage.owned_bytes > builder->out->limits.max_owned_bytes) {
        fail(builder, SOL_MIR_LINKAGE_BUILD_RESOURCE_EXHAUSTED,
            "linkage owned-byte limit exceeded");
        return NULL;
    }
    void *result = calloc(count, size);
    if (result == NULL) {
        builder->out->usage.owned_bytes -= bytes;
        fail(builder, SOL_MIR_LINKAGE_BUILD_ALLOCATION_FAILED,
            "linkage persistent allocation failed");
    }
    return result;
}

static int digest_compare(const SolMirLinkageDigest *left,
    const SolMirLinkageDigest *right) {
    return memcmp(left->bytes, right->bytes, SOL_MIR_LINKAGE_DIGEST_BYTES);
}

static bool digest_equal(const SolMirLinkageDigest *left,
    const SolMirLinkageDigest *right) {
    return digest_compare(left, right) == 0;
}

static int compare_callable(const void *left, const void *right) {
    const SolMirLinkageCallable *a = left, *b = right;
    return memcmp(a->symbol.bytes, b->symbol.bytes, SOL_MIR_LINKAGE_SYMBOL_LENGTH);
}

static int compare_host(const void *left, const void *right) {
    const SolMirLinkageHostRequirement *a = left, *b = right;
    int digest = digest_compare(&a->requirement_key, &b->requirement_key);
    if (digest != 0) return digest;
    if (a->semantic_id.high != b->semantic_id.high)
        return a->semantic_id.high < b->semantic_id.high ? -1 : 1;
    if (a->semantic_id.low != b->semantic_id.low)
        return a->semantic_id.low < b->semantic_id.low ? -1 : 1;
    return 0;
}

static int compare_export(const void *left, const void *right) {
    const SolMirLinkageEntryExport *a = left, *b = right;
    return memcmp(a->symbol.bytes, b->symbol.bytes, SOL_MIR_LINKAGE_SYMBOL_LENGTH);
}

static int compare_table_candidate(const void *left, const void *right) {
    const TableCandidate *a = left, *b = right;
    return digest_compare(&a->entry.identity, &b->entry.identity);
}

static int compare_runtime(const void *left, const void *right) {
    const SolMirLinkageRuntimeRequirement *a = left, *b = right;
    return digest_compare(&a->recipe_key, &b->recipe_key);
}

static SolMirLinkageCallableId callable_for_instance(Builder *builder,
    SolMirPlanInstanceId instance) {
    const SolMirLinkage *linkage = builder->out;
    for (size_t i = 0; i < linkage->callable_count; ++i) {
        if (!charge(builder, 1)) return SOL_MIR_LINKAGE_NONE;
        if (linkage->callables[i].instance == instance) return i;
    }
    return SOL_MIR_LINKAGE_NONE;
}

static SolMirLinkageHostRequirementId host_for_import(
    Builder *builder, SolMirMaterializedImportId import) {
    const SolMirLinkage *linkage = builder->out;
    for (size_t i = 0; i < linkage->host_requirement_count; ++i) {
        if (!charge(builder, 1)) return SOL_MIR_LINKAGE_NONE;
        if (linkage->host_requirements[i].import == import) return i;
    }
    return SOL_MIR_LINKAGE_NONE;
}

static bool approved(Builder *builder, const SolMirProgram *program,
    SolIrCallableId callable) {
    for (size_t i = 0; i < program->approved_import_count; ++i) {
        if (!charge(builder, 1)) return false;
        if (program->approved_imports[i] == callable) return true;
    }
    return false;
}

static bool populate_callables(Builder *builder) {
    SolMirLinkage *out = builder->out;
    const SolMirMaterialization *m = out->operations->layout->representation
        ->materialization;
    const SolIr *ir = m->plan->program->ir;
    for (size_t i = 0; i < m->image_count; ++i) {
        if (!charge(builder, 1)) return false;
        const SolMirMaterializedImage *image = &m->images[i];
        if (image->source_callable >= ir->callable_count) return false;
        const SolIrCallable *callable = &ir->callables[image->source_callable];
        if (callable->owner >= ir->definition_count) return false;
        SolMirLinkageCallable *target = &out->callables[i];
        target->instance = image->instance;
        target->semantic_id = ir->definitions[callable->owner].semantic_id;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        if (last_descriptor_work_start == SIZE_MAX)
            last_descriptor_work_start = builder->work.used;
#endif
        if (!canonical_result(builder, sol_mir_linkage_internal_instance_key(
                out, image->instance, &target->instance_key, &builder->work)))
            return false;
        if (!charge(builder, SOL_MIR_LINKAGE_SYMBOL_LENGTH)) return false;
        sol_mir_linkage_internal_symbol('i', target->semantic_id,
            &target->instance_key, &target->symbol);
    }
    if (!sort_items(builder, out->callables, out->callable_count,
            sizeof(*out->callables), compare_callable)) return false;
    for (size_t i = 0; i < out->callable_count; ++i)
        for (size_t previous = 0; previous < i; ++previous) {
            if (!charge(builder, 1)) return false;
            if (digest_equal(&out->callables[previous].instance_key,
                    &out->callables[i].instance_key)) {
                bool equal;
                if (!sol_mir_linkage_internal_instance_descriptor_equal(out,
                        out->callables[previous].instance,
                        out->callables[i].instance, &equal, &builder->work))
                    return canonical_result(builder, false);
                if (equal) return false;
                return fail(builder, SOL_MIR_LINKAGE_BUILD_SYMBOL_COLLISION,
                    "distinct callable descriptors produced the same digest");
            }
        }
    return true;
}

static bool populate_hosts(Builder *builder) {
    SolMirLinkage *out = builder->out;
    const SolMirOperations *operations = out->operations;
    const SolMirMaterialization *m = operations->layout->representation
        ->materialization;
    const SolMirProgram *program = m->plan->program;
    const SolIr *ir = program->ir;
    if (operations->import_envelope_count != m->import_count) return false;
    for (size_t i = 0; i < m->import_count; ++i) {
        if (!charge(builder, 1)) return false;
        const SolMirMaterializedImport *source = &m->imports[i];
        if (source->source_callable >= ir->callable_count) return false;
        const SolIrCallable *callable = &ir->callables[source->source_callable];
        bool is_approved = approved(builder, program, source->source_callable);
        if (builder->work.exhausted) return false;
        if (callable->owner >= ir->definition_count
            || callable->kind != SOL_IR_CALLABLE_CAPABILITY
            || callable->body != SOL_IR_NONE
            || !is_approved
            || operations->import_envelopes[i].import != i
            || !operations->import_envelopes[i].host_invoke)
            return fail(builder, SOL_MIR_LINKAGE_BUILD_UNRESOLVED_EXTERNAL,
                "external target is not an approved bodyless capability");
        SolMirLinkageHostRequirement *target = &out->host_requirements[i];
        target->import = i;
        target->semantic_id = ir->definitions[callable->owner].semantic_id;
        target->receiver = source->receiver;
        target->receiver_access = source->receiver_access;
        target->parameters = source->parameter_types;
        target->parameter_accesses = source->parameter_accesses;
        target->result = source->result; target->effects = source->effects;
        if (!canonical_result(builder, sol_mir_linkage_internal_host_key(out,
                i, &target->requirement_key, &builder->work))) return false;
        bool referenced = false;
        for (size_t binding = 0; binding < m->binding_count; ++binding) {
            if (!charge(builder, 1)) return false;
            if (m->bindings[binding].target_kind
                    == SOL_MIR_MATERIALIZED_TARGET_IMPORT
                && m->bindings[binding].import == i) referenced = true;
        }
        for (size_t plan_id = 0; plan_id < operations->callable_count; ++plan_id) {
            if (!charge(builder, 1)) return false;
            if (operations->callables[plan_id].target_kind
                    == SOL_MIR_MATERIALIZED_TARGET_IMPORT
                && operations->callables[plan_id].target_import == i)
                referenced = true;
        }
        if (!referenced) return fail(builder,
            SOL_MIR_LINKAGE_BUILD_UNRESOLVED_EXTERNAL,
            "approved host requirement is not referenced");
    }
    if (!sort_items(builder, out->host_requirements,
            out->host_requirement_count, sizeof(*out->host_requirements),
            compare_host)) return false;
    for (size_t i = 1; i < out->host_requirement_count; ++i) {
        if (!charge(builder, 1)) return false;
        if (digest_equal(&out->host_requirements[i - 1].requirement_key,
                &out->host_requirements[i].requirement_key)) {
            bool equal;
            if (!sol_mir_linkage_internal_host_descriptor_equal(out,
                    out->host_requirements[i - 1].import,
                    out->host_requirements[i].import, &equal, &builder->work))
                return canonical_result(builder, false);
            if (equal) return false;
            return fail(builder, SOL_MIR_LINKAGE_BUILD_SYMBOL_COLLISION,
                "distinct host descriptors produced the same digest");
        }
    }
    return true;
}

static bool populate_bindings(Builder *builder) {
    SolMirLinkage *out = builder->out;
    const SolMirMaterialization *m = out->operations->layout->representation
        ->materialization;
    for (size_t i = 0; i < m->binding_count; ++i) {
        if (!charge(builder, 1)) return false;
        const SolMirMaterializedBinding *source = &m->bindings[i];
        SolMirLinkageBinding target = {.binding = i,
            .internal = SOL_MIR_LINKAGE_NONE, .host = SOL_MIR_LINKAGE_NONE};
        if (source->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE) {
            target.target_kind = SOL_MIR_LINKAGE_TARGET_INTERNAL;
            target.internal = callable_for_instance(builder, source->instance);
            if (target.internal == SOL_MIR_LINKAGE_NONE) return false;
        } else if (source->target_kind == SOL_MIR_MATERIALIZED_TARGET_IMPORT) {
            target.target_kind = SOL_MIR_LINKAGE_TARGET_HOST;
            target.host = host_for_import(builder, source->import);
            if (target.host == SOL_MIR_LINKAGE_NONE) {
                if (builder->work.exhausted) return false;
                return fail(builder, SOL_MIR_LINKAGE_BUILD_UNRESOLVED_EXTERNAL,
                    "root or external binding has no valid linkage target");
            }
            if (source->kind == SOL_MIR_PLAN_DEMAND_ROOT)
                return fail(builder, SOL_MIR_LINKAGE_BUILD_UNRESOLVED_EXTERNAL,
                    "root or external binding has no valid linkage target");
        } else return fail(builder, SOL_MIR_LINKAGE_BUILD_UNRESOLVED_EXTERNAL,
            "binding has an unresolved target category");
        out->bindings[i] = target;
    }
    return true;
}

static bool entry_count(Builder *builder, const SolMirProgram *program,
    size_t *count) {
    size_t result = 0;
    for (size_t i = 0; i < program->root_count; ++i) {
        if (!charge(builder, 1)) return false;
        result += program->roots[i].kind == SOL_MIR_PROGRAM_ROOT_ENTRY;
    }
    *count = result; return true;
}

static bool populate_exports(Builder *builder) {
    SolMirLinkage *out = builder->out;
    const SolMirMaterialization *m = out->operations->layout->representation
        ->materialization;
    const SolMirProgram *program = m->plan->program;
    size_t at = 0;
    for (size_t root = 0; root < program->root_count; ++root) {
        if (!charge(builder, 1)) return false;
        if (program->roots[root].kind != SOL_MIR_PROGRAM_ROOT_ENTRY) continue;
        size_t binding = SOL_MIR_LINKAGE_NONE;
        for (size_t i = 0; i < m->binding_count; ++i) {
            if (!charge(builder, 1)) return false;
            if (m->bindings[i].kind == SOL_MIR_PLAN_DEMAND_ROOT
                && m->bindings[i].symbolic_callable
                    == program->roots[root].callable) {
                binding = i; break;
            }
        }
        if (binding == SOL_MIR_LINKAGE_NONE
            || out->bindings[binding].target_kind != SOL_MIR_LINKAGE_TARGET_INTERNAL)
            return false;
        SolMirLinkageCallableId callable = out->bindings[binding].internal;
        out->entry_exports[at] = (SolMirLinkageEntryExport){binding, callable, {{0}}};
        if (!charge(builder, SOL_MIR_LINKAGE_SYMBOL_LENGTH)) return false;
        sol_mir_linkage_internal_symbol('e', out->callables[callable].semantic_id,
            &out->callables[callable].instance_key,
            &out->entry_exports[at].symbol);
        ++at;
    }
    if (!sort_items(builder, out->entry_exports, out->entry_export_count,
            sizeof(*out->entry_exports), compare_export)) return false;
    for (size_t i = 1; i < out->entry_export_count; ++i) {
        if (!charge(builder, 1)) return false;
        if (memcmp(out->entry_exports[i - 1].symbol.bytes,
                out->entry_exports[i].symbol.bytes,
                SOL_MIR_LINKAGE_SYMBOL_LENGTH) == 0)
            return fail(builder, SOL_MIR_LINKAGE_BUILD_SYMBOL_COLLISION,
                "distinct entries produced the same export symbol");
    }
    return at == out->entry_export_count;
}

static bool callable_target(Builder *builder,
    const SolMirOperationCallablePlan *plan, SolMirLinkageTableEntry *entry) {
    const SolMirLinkage *out = builder->out;
    *entry = (SolMirLinkageTableEntry){.internal = SOL_MIR_LINKAGE_NONE,
        .host = SOL_MIR_LINKAGE_NONE};
    if (plan->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE) {
        entry->target_kind = SOL_MIR_LINKAGE_TARGET_INTERNAL;
        entry->internal = callable_for_instance(builder, plan->target_instance);
        if (entry->internal == SOL_MIR_LINKAGE_NONE) return false;
    } else if (plan->target_kind == SOL_MIR_MATERIALIZED_TARGET_IMPORT) {
        entry->target_kind = SOL_MIR_LINKAGE_TARGET_HOST;
        entry->host = host_for_import(builder, plan->target_import);
        if (entry->host == SOL_MIR_LINKAGE_NONE) return false;
    } else return false;
    return canonical_result(builder, sol_mir_linkage_internal_table_key(out,
        entry->target_kind, entry->internal, entry->host, &entry->identity,
        &builder->work));
}

static bool same_table_target(Builder *builder,
    const SolMirLinkageTableEntry *left,
    const SolMirLinkageTableEntry *right, bool *same) {
    return canonical_result(builder,
        sol_mir_linkage_internal_table_descriptor_equal(builder->out,
        left->target_kind, left->internal, left->host,
        right->target_kind, right->internal, right->host, same,
        &builder->work));
}

static bool populate_tables(Builder *builder, TableCandidate *candidates) {
    SolMirLinkage *out = builder->out;
    for (size_t i = 0; i < out->operations->callable_count; ++i) {
        if (!charge(builder, 1)
            || !callable_target(builder, &out->operations->callables[i],
                &candidates[i].entry)) return false;
    }
    if (!sort_items(builder, candidates, out->operations->callable_count,
            sizeof(*candidates), compare_table_candidate)) return false;
    size_t unique = 0;
    for (size_t i = 0; i < out->operations->callable_count; ++i) {
        if (!charge(builder, 1)) return false;
        if (i != 0 && digest_equal(&candidates[i - 1].entry.identity,
                &candidates[i].entry.identity)) {
            bool same;
            if (!same_table_target(builder, &candidates[i - 1].entry,
                    &candidates[i].entry, &same)) return false;
            if (!same)
                return fail(builder, SOL_MIR_LINKAGE_BUILD_SYMBOL_COLLISION,
                    "distinct function-table targets produced the same identity");
            continue;
        }
        ++unique;
    }
    if (unique > out->limits.max_table_entries) return fail(builder,
        SOL_MIR_LINKAGE_BUILD_RESOURCE_EXHAUSTED,
        "linkage function-table limit exceeded");
    out->table_entries = allocate(builder, unique, sizeof(*out->table_entries));
    if (unique != 0 && out->table_entries == NULL) return false;
    out->table_entry_count = out->table_entry_capacity = unique;
    size_t at = 0;
    for (size_t i = 0; i < out->operations->callable_count; ++i) {
        if (!charge(builder, 1)) return false;
        if (i != 0 && digest_equal(&candidates[i - 1].entry.identity,
                &candidates[i].entry.identity)) continue;
        out->table_entries[at++] = candidates[i].entry;
    }
    for (size_t i = 0; i < out->operations->callable_count; ++i) {
        if (!charge(builder, 1)) return false;
        SolMirLinkageTableEntry target;
        if (!callable_target(builder, &out->operations->callables[i], &target))
            return false;
        size_t table = SOL_MIR_LINKAGE_NONE;
        for (size_t q = 0; q < out->table_entry_count; ++q) {
            if (!charge(builder, 1)) return false;
            if (digest_equal(&target.identity, &out->table_entries[q].identity)) {
                bool same;
                if (!same_table_target(builder, &target, &out->table_entries[q],
                        &same)) return false;
                if (!same)
                    return fail(builder, SOL_MIR_LINKAGE_BUILD_SYMBOL_COLLISION,
                        "function-table digest lookup is ambiguous");
                table = q; break;
            }
        }
        if (table == SOL_MIR_LINKAGE_NONE) return false;
        out->callable_values[i] = (SolMirLinkageCallableValue){i, table};
    }
    return true;
}

static bool runtime_flags(Builder *builder, size_t recipe_id,
    uint32_t *result) {
    const SolMirLinkage *linkage = builder->out;
    const SolMirRepresentation *representation = linkage->operations->layout
        ->representation;
    const SolMirRecipe *recipe = &representation->recipes[recipe_id];
    uint32_t flags = 0;
    if (recipe->inhabited && !recipe->zero_sized
        && recipe->storage != SOL_MIR_STORAGE_NONE
        && recipe->storage != SOL_MIR_STORAGE_SCALAR)
        flags |= SOL_MIR_LINKAGE_RUNTIME_CREATE;
    if (recipe->copy_kind == SOL_MIR_COPY_TEXT
        || recipe->copy_kind == SOL_MIR_COPY_AGGREGATE
        || recipe->copy_kind == SOL_MIR_COPY_WRAPPER)
        flags |= SOL_MIR_LINKAGE_RUNTIME_COPY;
    if (recipe->drop_kind != SOL_MIR_DROP_NONE)
        flags |= SOL_MIR_LINKAGE_RUNTIME_DROP;
    for (size_t i = 0; i < linkage->operations->equality_node_count; ++i) {
        if (!charge(builder, 1)) return false;
        if (linkage->operations->equality_nodes[i].recipe == recipe_id
            && linkage->operations->equality_nodes[i].kind
                != SOL_MIR_OPERATION_EQUAL_SCALAR) {
            flags |= SOL_MIR_LINKAGE_RUNTIME_EQUAL; break;
        }
    }
    for (size_t i = 0; i < linkage->operations->callable_count; ++i) {
        if (!charge(builder, 1)) return false;
        if (linkage->operations->callables[i].kind
                == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION
            && linkage->operations->callables[i].function_recipe == recipe_id) {
            flags |= SOL_MIR_LINKAGE_RUNTIME_BOUND_ENVIRONMENT; break;
        }
    }
    *result = flags; return true;
}

static bool runtime_count(Builder *builder, size_t *result) {
    const SolMirRepresentation *representation = builder->out->operations->layout
        ->representation;
    size_t count = 0;
    for (size_t i = 0; i < representation->recipe_count; ++i) {
        uint32_t flags;
        if (!charge(builder, 1) || !runtime_flags(builder, i, &flags))
            return false;
        count += flags != 0;
    }
    *result = count; return true;
}

static bool populate_runtime(Builder *builder) {
    SolMirLinkage *out = builder->out;
    const SolMirRepresentation *representation = out->operations->layout
        ->representation;
    size_t at = 0;
    for (size_t i = 0; i < representation->recipe_count; ++i) {
        if (!charge(builder, 1)) return false;
        uint32_t flags;
        if (!runtime_flags(builder, i, &flags)) return false;
        if (flags == 0) continue;
        const SolMirRecipe *recipe = &representation->recipes[i];
        SolMirLinkageRuntimeRequirement *target = &out->runtime_requirements[at++];
        target->recipe = i; target->operations = flags;
        target->storage = recipe->storage; target->copy_kind = recipe->copy_kind;
        target->drop_kind = recipe->drop_kind;
        if (!canonical_result(builder, sol_mir_linkage_internal_recipe_key(out,
                i, &target->recipe_key, &builder->work))) return false;
    }
    if (!sort_items(builder, out->runtime_requirements,
            out->runtime_requirement_count, sizeof(*out->runtime_requirements),
            compare_runtime)) return false;
    for (size_t i = 1; i < out->runtime_requirement_count; ++i) {
        if (!charge(builder, 1)) return false;
        if (digest_equal(&out->runtime_requirements[i - 1].recipe_key,
                &out->runtime_requirements[i].recipe_key)) {
            bool equal;
            if (!sol_mir_linkage_internal_recipe_descriptor_equal(out,
                    out->runtime_requirements[i - 1].recipe,
                    out->runtime_requirements[i].recipe, &equal,
                    &builder->work)) return canonical_result(builder, false);
            if (equal) return false;
            return fail(builder, SOL_MIR_LINKAGE_BUILD_SYMBOL_COLLISION,
                "distinct runtime recipes produced the same identity");
        }
    }
    return at == out->runtime_requirement_count;
}

bool sol_mir_linkage_internal_validation_scratch(const SolMirLinkage *linkage,
    size_t *bytes) {
    if (linkage == NULL || linkage->operations == NULL || bytes == NULL)
        return false;
    const SolMirRepresentation *representation = linkage->operations->layout
        ->representation;
    const SolMirMaterialization *materialization
        = representation->materialization;
    size_t result, part;
    if (!mul_size(materialization->image_count, sizeof(size_t), &result)
        || !mul_size(materialization->import_count, sizeof(size_t), &part)
        || !add_size(&result, part)
        || !add_size(&result, materialization->import_count)
        || !add_size(&result, linkage->table_entry_count)
        || !add_size(&result, representation->recipe_count)) return false;
    size_t replay = 0;
#define REPLAY_SCRATCH(count, type) do { \
    if (!mul_size((count), sizeof(type), &part)) return false; \
    if (part > replay) replay = part; \
} while (0)
    REPLAY_SCRATCH(materialization->image_count, SolMirLinkageCallable);
    REPLAY_SCRATCH(materialization->import_count,
        SolMirLinkageHostRequirement);
    REPLAY_SCRATCH(linkage->entry_export_count, SolMirLinkageEntryExport);
    REPLAY_SCRATCH(linkage->operations->callable_count, TableCandidate);
    REPLAY_SCRATCH(linkage->runtime_requirement_count,
        SolMirLinkageRuntimeRequirement);
#undef REPLAY_SCRATCH
    if (replay > result) result = replay;
    *bytes = result;
    return true;
}

bool sol_mir_linkage_internal_expected_usage(const SolMirLinkage *linkage,
    SolMirLinkageUsage *usage) {
    if (linkage == NULL || linkage->operations == NULL || usage == NULL) return false;
    SolMirLinkageUsage result = {
        .callables = linkage->callable_count,
        .bindings = linkage->binding_count,
        .entry_exports = linkage->entry_export_count,
        .table_entries = linkage->table_entry_count,
        .callable_values = linkage->callable_value_count,
        .host_requirements = linkage->host_requirement_count,
        .runtime_requirements = linkage->runtime_requirement_count,
    };
#define OWNED(member, type, singular) do { \
    size_t bytes; \
    if (!mul_size(linkage->singular##_count, sizeof(*linkage->member), &bytes) \
        || !add_size(&result.owned_bytes, bytes)) return false; \
} while (0);
    SOL_MIR_LINKAGE_ARENAS(OWNED)
#undef OWNED
    if (!mul_size(linkage->operations->callable_count,
            sizeof(TableCandidate), &result.build_scratch_bytes)) return false;
    result.build_work = 0;
    size_t local_scratch;
    if (!sol_mir_linkage_internal_validation_scratch(linkage,
            &local_scratch)) return false;
    result.validation_scratch_bytes
        = linkage->operations->usage.validation_scratch_bytes > local_scratch
        ? linkage->operations->usage.validation_scratch_bytes : local_scratch;
    result.validation_work = linkage->usage.validation_work;
    *usage = result; return true;
}

SolMirLinkageBuildOutcome sol_mir_linkage_build(
    const SolMirLinkageBuildRequest *request, SolMirLinkage *output,
    SolDiagnostics *diagnostics) {
    if (request == NULL || output == NULL || request->operations == NULL
        || !owner_empty(output) || (request->limits != NULL
            && !limits_zero(*request->limits)
            && !limits_complete(*request->limits))) {
        report(diagnostics, "invalid linkage build request or destination");
        return SOL_MIR_LINKAGE_BUILD_INVALID_ARGUMENT;
    }
    if (!sol_mir_operations_validate(request->operations, diagnostics)) {
        report(diagnostics, "invalid borrowed operations owner");
        return diagnostics != NULL && diagnostics->allocation_failed
            ? SOL_MIR_LINKAGE_BUILD_ALLOCATION_FAILED
            : SOL_MIR_LINKAGE_BUILD_INVALID_OPERATIONS;
    }
    SolMirLinkage scratch; sol_mir_linkage_init(&scratch);
    scratch.operations = request->operations;
    scratch.limits = request->limits == NULL || limits_zero(*request->limits)
        ? sol_mir_linkage_default_limits() : *request->limits;
    Builder builder = {.out = &scratch, .diagnostics = diagnostics,
        .outcome = SOL_MIR_LINKAGE_BUILD_INTERNAL_FAILED,
        .work = {.limit = scratch.limits.max_build_work}};
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    last_descriptor_work_start = SIZE_MAX;
    last_sort_work_start = SIZE_MAX;
#endif
    const SolMirMaterialization *m = request->operations->layout->representation
        ->materialization;
    const SolMirProgram *program = m->plan->program;
    size_t entries, runtime;
    if (!entry_count(&builder, program, &entries)
        || !runtime_count(&builder, &runtime)) goto failed;
    if (m->image_count > scratch.limits.max_callables
        || m->binding_count > scratch.limits.max_bindings
        || entries > scratch.limits.max_entry_exports
        || request->operations->callable_count > scratch.limits.max_callable_values
        || m->import_count > scratch.limits.max_host_requirements
        || runtime > scratch.limits.max_runtime_requirements) {
        fail(&builder, SOL_MIR_LINKAGE_BUILD_RESOURCE_EXHAUSTED,
            "linkage arena limit exceeded"); goto failed;
    }
    size_t table_scratch;
    if (!mul_size(request->operations->callable_count, sizeof(TableCandidate),
            &table_scratch)
        || table_scratch > scratch.limits.max_build_scratch_bytes) {
        fail(&builder, SOL_MIR_LINKAGE_BUILD_RESOURCE_EXHAUSTED,
            "linkage build scratch limit exceeded"); goto failed;
    }
    scratch.usage.build_scratch_bytes = table_scratch;
    if (request->operations->usage.validation_scratch_bytes
            > scratch.limits.max_validation_scratch_bytes
        || request->operations->usage.validation_work
            > scratch.limits.max_validation_work) {
        fail(&builder, SOL_MIR_LINKAGE_BUILD_RESOURCE_EXHAUSTED,
            "linkage prerequisite validation resource limit exceeded");
        goto failed;
    }
#define ALLOC(member, type, singular, count_value) do { \
    scratch.member = allocate(&builder, (count_value), sizeof(*scratch.member)); \
    scratch.singular##_count = scratch.singular##_capacity = (count_value); \
    if ((count_value) != 0 && scratch.member == NULL) goto failed; \
} while (0)
    ALLOC(callables, SolMirLinkageCallable, callable, m->image_count);
    ALLOC(bindings, SolMirLinkageBinding, binding, m->binding_count);
    ALLOC(entry_exports, SolMirLinkageEntryExport, entry_export, entries);
    ALLOC(callable_values, SolMirLinkageCallableValue, callable_value,
        request->operations->callable_count);
    ALLOC(host_requirements, SolMirLinkageHostRequirement, host_requirement,
        m->import_count);
    ALLOC(runtime_requirements, SolMirLinkageRuntimeRequirement,
        runtime_requirement, runtime);
#undef ALLOC
    TableCandidate *candidates = request->operations->callable_count == 0 ? NULL
        : calloc(request->operations->callable_count, sizeof(*candidates));
    if (request->operations->callable_count != 0 && candidates == NULL) {
        fail(&builder, SOL_MIR_LINKAGE_BUILD_ALLOCATION_FAILED,
            "linkage build scratch allocation failed");
        goto failed;
    }
    if (!populate_callables(&builder) || !populate_hosts(&builder)
        || !populate_bindings(&builder) || !populate_exports(&builder)
        || !populate_tables(&builder, candidates)
        || !populate_runtime(&builder)) {
        free(candidates); goto failed;
    }
    free(candidates);
    scratch.usage.build_work = builder.work.used;
    SolMirLinkageUsage expected;
    if (!sol_mir_linkage_internal_expected_usage(&scratch, &expected)) goto failed;
    expected.build_work = builder.work.used;
    scratch.usage = expected;
    size_t validation_work, validation_scratch;
    if (!sol_mir_linkage_internal_validation_requirements(&scratch,
            &validation_work, &validation_scratch, diagnostics)) {
        SolMirLinkageBuildOutcome outcome = diagnostics != NULL
                && diagnostics->allocation_failed
            ? SOL_MIR_LINKAGE_BUILD_ALLOCATION_FAILED
            : SOL_MIR_LINKAGE_BUILD_RESOURCE_EXHAUSTED;
        fail(&builder, outcome,
            "linkage validation work limit exceeded"); goto failed;
    }
    scratch.usage.validation_work = validation_work;
    scratch.usage.validation_scratch_bytes = validation_scratch;
    if (scratch.usage.owned_bytes > scratch.limits.max_owned_bytes
        || scratch.usage.build_work > scratch.limits.max_build_work
        || scratch.usage.validation_scratch_bytes
            > scratch.limits.max_validation_scratch_bytes
        || scratch.usage.validation_work > scratch.limits.max_validation_work) {
        fail(&builder, SOL_MIR_LINKAGE_BUILD_RESOURCE_EXHAUSTED,
            "linkage measured resource limit exceeded"); goto failed;
    }
    if (!sol_mir_linkage_validate(&scratch, diagnostics)) {
        sol_mir_linkage_free(&scratch);
        return diagnostics != NULL && diagnostics->allocation_failed
            ? SOL_MIR_LINKAGE_BUILD_ALLOCATION_FAILED
            : SOL_MIR_LINKAGE_BUILD_INTERNAL_FAILED;
    }
    *output = scratch;
    return SOL_MIR_LINKAGE_BUILD_SUCCEEDED;
failed:
    if (builder.outcome == SOL_MIR_LINKAGE_BUILD_INTERNAL_FAILED)
        report(diagnostics, "linkage construction invariant failed");
    sol_mir_linkage_free(&scratch); return builder.outcome;
}

static void format(Buffer *buffer, const char *pattern, ...) {
    if (buffer->failed) return;
    va_list args; va_start(args, pattern); va_list copy; va_copy(copy, args);
    int count = vsnprintf(NULL, 0, pattern, copy); va_end(copy);
    if (count < 0 || (size_t)count > SIZE_MAX - buffer->length - 1) {
        buffer->failed = true; va_end(args); return;
    }
    size_t needed = buffer->length + (size_t)count + 1;
    if (needed > buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 4096 : buffer->capacity;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
            capacity *= 2;
        }
        char *grown = realloc(buffer->data, capacity);
        if (grown == NULL) { buffer->failed = true; va_end(args); return; }
        buffer->data = grown; buffer->capacity = capacity;
    }
    (void)vsnprintf(buffer->data + buffer->length,
        buffer->capacity - buffer->length, pattern, args);
    va_end(args); buffer->length += (size_t)count;
}

static void format_digest(Buffer *buffer, const SolMirLinkageDigest *digest) {
    for (size_t i = 0; i < SOL_MIR_LINKAGE_DIGEST_BYTES; ++i)
        format(buffer, "%02x", digest->bytes[i]);
}

static void format_semantic(Buffer *buffer, SolSemanticId semantic) {
    format(buffer, "%016" PRIx64 "%016" PRIx64,
        semantic.high, semantic.low);
}

static const char *access_name(SolAccessMode access) {
    static const char *const names[] = {"owned", "shared", "exclusive"};
    return (size_t)access < sizeof(names) / sizeof(names[0])
        ? names[access] : "invalid";
}

static const char *storage_name(SolMirStorageKind storage) {
    static const char *const names[] = {"none", "scalar", "text-handle",
        "aggregate-value", "callable-handle", "capability-handle"};
    return (size_t)storage < sizeof(names) / sizeof(names[0])
        ? names[storage] : "invalid";
}

static const char *copy_name(SolMirCopyKind kind) {
    static const char *const names[] = {"trivial", "text", "aggregate",
        "wrapper", "forbidden", "unreachable"};
    return (size_t)kind < sizeof(names) / sizeof(names[0])
        ? names[kind] : "invalid";
}

static const char *drop_name(SolMirDropKind kind) {
    static const char *const names[] = {"none", "text", "aggregate",
        "callable", "capability", "wrapper"};
    return (size_t)kind < sizeof(names) / sizeof(names[0])
        ? names[kind] : "invalid";
}

static void format_target(Buffer *buffer, const SolMirLinkage *linkage,
    SolMirLinkageTargetKind kind, SolMirLinkageCallableId internal,
    SolMirLinkageHostRequirementId host) {
    if (kind == SOL_MIR_LINKAGE_TARGET_INTERNAL) {
        format(buffer, "internal:%s", linkage->callables[internal].symbol.bytes);
    } else {
        const SolMirLinkageHostRequirement *requirement
            = &linkage->host_requirements[host];
        format(buffer, "host:");
        format_semantic(buffer, requirement->semantic_id);
        format(buffer, ":");
        format_digest(buffer, &requirement->requirement_key);
    }
}

static void format_operations(Buffer *buffer, uint32_t operations) {
    bool separator = false;
#define FLAG(value, name) do { \
    if ((operations & (value)) != 0) { \
        format(buffer, "%s%s", separator ? "," : "", (name)); \
        separator = true; \
    } \
} while (0)
    FLAG(SOL_MIR_LINKAGE_RUNTIME_CREATE, "create");
    FLAG(SOL_MIR_LINKAGE_RUNTIME_COPY, "copy");
    FLAG(SOL_MIR_LINKAGE_RUNTIME_DROP, "drop");
    FLAG(SOL_MIR_LINKAGE_RUNTIME_EQUAL, "equal");
    FLAG(SOL_MIR_LINKAGE_RUNTIME_BOUND_ENVIRONMENT, "bound-environment");
#undef FLAG
}

bool sol_mir_linkage_render(FILE *stream, const SolMirLinkage *linkage) {
    if (stream == NULL || !sol_mir_linkage_validate(linkage, NULL)) return false;
    Buffer buffer = {0};
    format(&buffer, "mir_linkage callable=%zu binding=%zu entry=%zu table=%zu value=%zu host=%zu runtime=%zu\n",
        linkage->callable_count, linkage->binding_count,
        linkage->entry_export_count, linkage->table_entry_count,
        linkage->callable_value_count, linkage->host_requirement_count,
        linkage->runtime_requirement_count);
    for (size_t i = 0; i < linkage->callable_count; ++i) {
        const SolMirLinkageCallable *item = &linkage->callables[i];
        format(&buffer, "callable semantic=");
        format_semantic(&buffer, item->semantic_id);
        format(&buffer, " key=");
        format_digest(&buffer, &item->instance_key);
        format(&buffer, " symbol=%s\n", item->symbol.bytes);
    }
    const SolMirMaterialization *materialization = linkage->operations->layout
        ->representation->materialization;
    const SolIr *ir = materialization->plan->program->ir;
    for (size_t i = 0; i < linkage->binding_count; ++i) {
        const SolMirMaterializedBinding *source = &materialization->bindings[
            linkage->bindings[i].binding];
        const SolIrCallable *callable = &ir->callables[source->symbolic_callable];
        format(&buffer, "binding source=");
        format_semantic(&buffer, ir->definitions[callable->owner].semantic_id);
        format(&buffer, ":%s target=", callable->name);
        format_target(&buffer, linkage, linkage->bindings[i].target_kind,
            linkage->bindings[i].internal, linkage->bindings[i].host);
        format(&buffer, "\n");
    }
    for (size_t i = 0; i < linkage->entry_export_count; ++i) {
        format(&buffer, "entry symbol=%s target=%s\n",
            linkage->entry_exports[i].symbol.bytes,
            linkage->callables[linkage->entry_exports[i].callable].symbol.bytes);
    }
    for (size_t i = 0; i < linkage->host_requirement_count; ++i) {
        const SolMirLinkageHostRequirement *item = &linkage->host_requirements[i];
        format(&buffer, "host semantic=");
        format_semantic(&buffer, item->semantic_id);
        format(&buffer, " key=");
        format_digest(&buffer, &item->requirement_key);
        format(&buffer, " receiver-access=%s parameters=%zu\n",
            access_name(item->receiver_access), item->parameters.count);
    }
    for (size_t i = 0; i < linkage->table_entry_count; ++i) {
        const SolMirLinkageTableEntry *item = &linkage->table_entries[i];
        format(&buffer, "table key="); format_digest(&buffer, &item->identity);
        format(&buffer, " target=");
        format_target(&buffer, linkage, item->target_kind, item->internal,
            item->host);
        format(&buffer, "\n");
    }
    for (size_t i = 0; i < linkage->callable_value_count; ++i) {
        format(&buffer, "callable-value table=");
        format_digest(&buffer, &linkage->table_entries[
            linkage->callable_values[i].table].identity);
        format(&buffer, "\n");
    }
    for (size_t i = 0; i < linkage->runtime_requirement_count; ++i) {
        const SolMirLinkageRuntimeRequirement *item
            = &linkage->runtime_requirements[i];
        format(&buffer, "runtime key=");
        format_digest(&buffer, &item->recipe_key);
        format(&buffer, " operations="); format_operations(&buffer,
            item->operations);
        format(&buffer, " storage=%s copy=%s drop=%s\n",
            storage_name(item->storage), copy_name(item->copy_kind),
            drop_name(item->drop_kind));
    }
    bool ok = !buffer.failed && (buffer.length == 0
        || fwrite(buffer.data, buffer.length, 1, stream) == 1);
    free(buffer.data); return ok;
}
