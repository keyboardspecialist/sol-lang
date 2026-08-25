#include "sol/compilation.h"

#include <stdint.h>
#include <stdlib.h>

#include "resource_internal.h"

#undef malloc
#undef calloc
#undef realloc

static _Thread_local SolResourceBudget *sol_active_budget;

void sol_compilation_limits_default(SolCompilationLimits *limits) {
    if (limits == NULL) return;
    *limits = (SolCompilationLimits){
        .source_bytes_per_file = 8u * 1024u * 1024u,
        .package_source_bytes = 64u * 1024u * 1024u,
        .source_files = 1024,
        .directory_depth = 64,
        .directory_entries = 100000,
        .tokens = 1000000,
        .arena_entries = 2000000,
        .diagnostics = 256,
        .allocation_bytes = 512u * 1024u * 1024u,
        .allocation_count = 1000000,
    };
}

void sol_resource_begin(SolResourceBudget *budget) {
    sol_active_budget = budget;
}

void sol_resource_end(SolResourceBudget *budget) {
    if (sol_active_budget == budget) sol_active_budget = NULL;
}

const char *sol_resource_failure(const SolResourceBudget *budget) {
    return budget == NULL ? NULL : budget->failure;
}

static bool sol_resource_charge(
    size_t *used, size_t amount, size_t limit, const char *failure
) {
    if (sol_active_budget == NULL) return true;
    if (amount > SIZE_MAX - *used || *used + amount > limit) {
        if (sol_active_budget->failure == NULL) sol_active_budget->failure = failure;
        return false;
    }
    *used += amount;
    return true;
}

bool sol_resource_charge_source(size_t bytes) {
    if (sol_active_budget == NULL) return true;
    if (bytes > sol_active_budget->limits.source_bytes_per_file) {
        if (sol_active_budget->failure == NULL) {
            sol_active_budget->failure = "source file byte limit exceeded";
        }
        return false;
    }
    return sol_resource_charge(&sol_active_budget->source_bytes, bytes,
        sol_active_budget->limits.package_source_bytes,
        "package source byte limit exceeded");
}

bool sol_resource_charge_file(void) {
    if (sol_active_budget == NULL) return true;
    return sol_resource_charge(&sol_active_budget->files, 1,
        sol_active_budget->limits.source_files, "source file limit exceeded");
}

bool sol_resource_charge_directory(size_t depth) {
    if (sol_active_budget == NULL) return true;
    if (depth > sol_active_budget->limits.directory_depth) {
        if (sol_active_budget->failure == NULL) {
            sol_active_budget->failure = "directory depth limit exceeded";
        }
        return false;
    }
    return sol_resource_charge(&sol_active_budget->directory_entries, 1,
        sol_active_budget->limits.directory_entries,
        "directory entry limit exceeded");
}

bool sol_resource_charge_token(void) {
    if (sol_active_budget == NULL) return true;
    return sol_resource_charge(&sol_active_budget->tokens, 1,
        sol_active_budget->limits.tokens, "token limit exceeded");
}

bool sol_resource_charge_arena(size_t entries) {
    if (sol_active_budget == NULL) return true;
    return sol_resource_charge(&sol_active_budget->arena_entries, entries,
        sol_active_budget->limits.arena_entries, "compiler arena limit exceeded");
}

bool sol_resource_charge_diagnostic(void) {
    if (sol_active_budget == NULL) return true;
    return sol_resource_charge(&sol_active_budget->diagnostics, 1,
        sol_active_budget->limits.diagnostics, "diagnostic limit exceeded");
}

static bool sol_resource_charge_allocation(size_t size) {
    if (sol_active_budget == NULL) return true;
    if (!sol_resource_charge(&sol_active_budget->allocation_count, 1,
        sol_active_budget->limits.allocation_count,
        "compiler allocation count limit exceeded")) return false;
    return sol_resource_charge(&sol_active_budget->allocation_bytes, size,
        sol_active_budget->limits.allocation_bytes,
        "compiler allocation byte limit exceeded");
}

void *sol_resource_malloc(size_t size) {
    if (!sol_resource_charge_allocation(size)) return NULL;
    return malloc(size);
}

void *sol_resource_calloc(size_t count, size_t size) {
    if (size != 0 && count > SIZE_MAX / size) return NULL;
    if (!sol_resource_charge_allocation(count * size)) return NULL;
    return calloc(count, size);
}

void *sol_resource_realloc(void *allocation, size_t size) {
    if (!sol_resource_charge_allocation(size)) return NULL;
    return realloc(allocation, size);
}
