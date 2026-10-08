#include "wasm_represented.h"

#include <binaryen-c.h>
#include <wasm.h>

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* P4.3 deliberately has its own module shape: Wasm32 MVP memory, no imports,
 * no start, and (only for authenticated unbound callbacks) one private table.
 * The failure packet globals remain private. */
#define P43_CODE "sol.p43.failure-code"
#define P43_SITE "sol.p43.failure-site"
#define P43_HEAP "sol.p43.heap"
#define P43_REQUESTS "sol.p43.requests"
#define P43_BYTES "sol.p43.bytes"
#define P43_MAX_REQUESTS "sol.p43.max-requests"
#define P43_MAX_BYTES "sol.p43.max-bytes"
#define P44_MAX_STEPS "sol.p44.max-steps"
#define P44_STEPS "sol.p44.steps"
#define P44_STEP "sol.p44.step"
#define P44_MAX_CALL_DEPTH "sol.p44.max-call-depth"
#define P44_CALL_DEPTH "sol.p44.call-depth"
#define P43_WRITEBACKS "sol.p43.test.writebacks"
#define P43_CLEANUP_OLD_CALLABLE "sol.p43.test.cleanup.old-callable"
#define P43_CLEANUP_MOVED_CALLABLE "sol.p43.test.cleanup.moved-callable"
#define P43_CLEANUP_TEXT_SIBLING "sol.p43.test.cleanup.text-sibling"
#define P43_CLEANUP_ROOT "sol.p43.test.cleanup.root"
#define P43_MEMORY SOL_WASM_BACKEND_MEMORY_EXPORT
#define P43_TEXT_COPY "sol.p43.text.copy"
#define P43_TEXT_EQUAL "sol.p43.text.equal"
#define P43_FIXED_ALLOC "sol.p43.fixed.alloc"
#define P43_TABLE "sol.p43.functions"
#define P44_PANIC_DETAIL_OFFSET "sol.p44.panic-detail-offset"
#define P44_PANIC_DETAIL_LENGTH "sol.p44.panic-detail-length"
#define P44_PANIC_CAPTURE "sol.p44.panic-capture"
#define P44_TEST_PACKET_RESET_SUCCESS "sol.p44.test.packet-reset-success"
#define P44_TEST_PACKET_RESET_NONPANIC "sol.p44.test.packet-reset-nonpanic"
#define P44_TRACE_OFFSET "sol.p44.trace-offset"
#define P44_TRACE_COUNT "sol.p44.trace-count"
#define P44_TRACE_OVERFLOW "sol.p44.trace-overflow"

static size_t allocation_attempts;
static size_t fail_allocation_after;
static uint64_t represented_test_max_requests = UINT64_MAX;
static uint64_t represented_test_max_bytes = UINT64_MAX;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
static size_t represented_test_max_pages;
static uint32_t represented_test_heap_base;
static size_t represented_test_qualified_certifications;
static SolWasmRepresentedTestControlTransitionStats represented_test_control_transition_stats;
static SolWasmRepresentedTestRefinementIndexStats represented_test_refinement_index_stats;
static bool represented_test_inactive_payload_probe;
static bool represented_test_callback_writeback_probe;
static bool represented_test_callable_hole_cleanup_probe;
static bool represented_test_p44_packet_reset_probe;
static bool represented_test_p44_cleanup_trace_probe;
static bool represented_test_p44_refinement_trace_probe;
#endif

typedef enum {
    REPRESENTED_BACKEND_OK,
    REPRESENTED_BACKEND_RESOURCE,
    REPRESENTED_BACKEND_ALLOCATION,
} RepresentedBackendStatus;

typedef struct {
    size_t bytes;
    bool scratch;
} RepresentedAllocation;

typedef struct {
    const SolWasmRepresentedLimits *limits;
    SolWasmRepresentedUsage *usage;
    size_t scratch_live;
    size_t owned_live;
    RepresentedBackendStatus status;
} RepresentedAccounting;

/* Binaryen and Wasmtime allocate internally.  Their private allocations are
 * outside the P4.3 allocation hook and byte quotas; only backend-owned C
 * buffers and the transferred serialized buffer are metered here. */
static RepresentedAccounting *represented_accounting;

static bool represented_add(size_t left, size_t right, size_t *result) {
    if (left > SIZE_MAX - right) return false;
    *result = left + right;
    return true;
}

static void *allocate(size_t count, size_t size) {
    size_t bytes, work, scratch, owned;
    if (size != 0 && count > SIZE_MAX / size) {
        if (represented_accounting != NULL) represented_accounting->status = REPRESENTED_BACKEND_RESOURCE;
        return NULL;
    }
    bytes = count * size;
    if (represented_accounting != NULL) {
        if (!represented_add(represented_accounting->usage->work_bytes, bytes, &work)
            || !represented_add(represented_accounting->scratch_live, bytes, &scratch)
            || !represented_add(represented_accounting->owned_live, bytes, &owned)
            || work > represented_accounting->limits->max_work_bytes
            || scratch > represented_accounting->limits->max_scratch_bytes
            || owned > represented_accounting->limits->max_owned_bytes) {
            represented_accounting->status = REPRESENTED_BACKEND_RESOURCE;
            return NULL;
        }
    }
    ++allocation_attempts;
    if (fail_allocation_after != 0 && allocation_attempts == fail_allocation_after) {
        if (represented_accounting != NULL) represented_accounting->status = REPRESENTED_BACKEND_ALLOCATION;
        return NULL;
    }
    if (bytes > SIZE_MAX - sizeof(RepresentedAllocation)) {
        if (represented_accounting != NULL) represented_accounting->status = REPRESENTED_BACKEND_RESOURCE;
        return NULL;
    }
    RepresentedAllocation *allocation = calloc(1, sizeof *allocation + bytes);
    if (allocation == NULL) {
        if (represented_accounting != NULL) represented_accounting->status = REPRESENTED_BACKEND_ALLOCATION;
        return NULL;
    }
    allocation->bytes = bytes;
    allocation->scratch = represented_accounting != NULL;
    if (represented_accounting != NULL) {
        represented_accounting->usage->work_bytes = work;
        represented_accounting->scratch_live = scratch;
        represented_accounting->owned_live = owned;
        if (scratch > represented_accounting->usage->scratch_bytes)
            represented_accounting->usage->scratch_bytes = scratch;
        if (owned > represented_accounting->usage->owned_bytes)
            represented_accounting->usage->owned_bytes = owned;
    }
    return allocation + 1;
}

static void deallocate(void *pointer) {
    if (pointer == NULL) return;
    RepresentedAllocation *allocation = (RepresentedAllocation *)pointer - 1;
    if (allocation->scratch && represented_accounting != NULL) {
        represented_accounting->scratch_live -= allocation->bytes;
        represented_accounting->owned_live -= allocation->bytes;
    }
    free(allocation);
}

static void *grow(void *pointer, size_t old_count, size_t new_count, size_t size) {
    if (new_count < old_count || (size != 0 && new_count > SIZE_MAX / size)) {
        if (represented_accounting != NULL) represented_accounting->status = REPRESENTED_BACKEND_RESOURCE;
        return NULL;
    }
    void *replacement = allocate(new_count, size);
    if (replacement == NULL) return NULL;
    if (pointer != NULL && old_count != 0) memcpy(replacement, pointer, old_count * size);
    deallocate(pointer);
    return replacement;
}

static bool represented_node_reserve(void) {
    if (represented_accounting == NULL) return true;
    if (represented_accounting->usage->generated_nodes
        >= represented_accounting->limits->max_generated_nodes) {
        represented_accounting->status = REPRESENTED_BACKEND_RESOURCE;
        return false;
    }
    ++represented_accounting->usage->generated_nodes;
    return true;
}

static BinaryenExpressionRef represented_node_finish(BinaryenExpressionRef result) {
    if (result == NULL && represented_accounting != NULL)
        --represented_accounting->usage->generated_nodes;
    return result;
}

static BinaryenExpressionRef represented_const(BinaryenModuleRef m, struct BinaryenLiteral x) {
    return represented_node_reserve() ? represented_node_finish(BinaryenConst(m, x)) : NULL;
}
static BinaryenExpressionRef represented_unary(BinaryenModuleRef m, BinaryenOp o,
    BinaryenExpressionRef x) {
    return represented_node_reserve() ? represented_node_finish(BinaryenUnary(m, o, x)) : NULL;
}
static BinaryenExpressionRef represented_binary_node(BinaryenModuleRef m, BinaryenOp o,
    BinaryenExpressionRef x, BinaryenExpressionRef y) {
    return represented_node_reserve() ? represented_node_finish(BinaryenBinary(m, o, x, y)) : NULL;
}
static BinaryenExpressionRef represented_if(BinaryenModuleRef m, BinaryenExpressionRef c,
    BinaryenExpressionRef yes, BinaryenExpressionRef no) {
    return represented_node_reserve() ? represented_node_finish(BinaryenIf(m, c, yes, no)) : NULL;
}
static BinaryenExpressionRef represented_block(BinaryenModuleRef m, const char *name,
    BinaryenExpressionRef *items, BinaryenIndex count, BinaryenType type) {
    return represented_node_reserve() ? represented_node_finish(BinaryenBlock(m, name, items, count, type)) : NULL;
}
static BinaryenExpressionRef represented_loop(BinaryenModuleRef m, const char *name,
    BinaryenExpressionRef body) {
    return represented_node_reserve() ? represented_node_finish(BinaryenLoop(m, name, body)) : NULL;
}
static BinaryenExpressionRef represented_break(BinaryenModuleRef m, const char *name,
    BinaryenExpressionRef condition, BinaryenExpressionRef value) {
    return represented_node_reserve() ? represented_node_finish(BinaryenBreak(m, name, condition, value)) : NULL;
}
static BinaryenExpressionRef represented_call(BinaryenModuleRef m, const char *target,
    BinaryenExpressionRef *operands, BinaryenIndex count, BinaryenType result) {
    return represented_node_reserve() ? represented_node_finish(BinaryenCall(m, target, operands, count, result)) : NULL;
}
static BinaryenExpressionRef represented_call_indirect(BinaryenModuleRef m, const char *table,
    BinaryenExpressionRef target, BinaryenExpressionRef *operands, BinaryenIndex count,
    BinaryenType parameters, BinaryenType results) {
    return represented_node_reserve() ? represented_node_finish(BinaryenCallIndirect(m, table,
        target, operands, count, parameters, results)) : NULL;
}
static BinaryenExpressionRef represented_local_get(BinaryenModuleRef m, BinaryenIndex index,
    BinaryenType type) {
    return represented_node_reserve() ? represented_node_finish(BinaryenLocalGet(m, index, type)) : NULL;
}
static BinaryenExpressionRef represented_local_set(BinaryenModuleRef m, BinaryenIndex index,
    BinaryenExpressionRef value) {
    return represented_node_reserve() ? represented_node_finish(BinaryenLocalSet(m, index, value)) : NULL;
}
static BinaryenExpressionRef represented_global_get(BinaryenModuleRef m, const char *name,
    BinaryenType type) {
    return represented_node_reserve() ? represented_node_finish(BinaryenGlobalGet(m, name, type)) : NULL;
}
static BinaryenExpressionRef represented_global_set(BinaryenModuleRef m, const char *name,
    BinaryenExpressionRef value) {
    return represented_node_reserve() ? represented_node_finish(BinaryenGlobalSet(m, name, value)) : NULL;
}
static BinaryenExpressionRef represented_drop(BinaryenModuleRef m, BinaryenExpressionRef value) {
    return represented_node_reserve() ? represented_node_finish(BinaryenDrop(m, value)) : NULL;
}
static BinaryenExpressionRef represented_return(BinaryenModuleRef m, BinaryenExpressionRef value) {
    return represented_node_reserve() ? represented_node_finish(BinaryenReturn(m, value)) : NULL;
}
static BinaryenExpressionRef represented_load(BinaryenModuleRef m, uint32_t bytes,
    bool signed_, uint32_t offset, uint32_t align, BinaryenType type,
    BinaryenExpressionRef pointer, const char *memory) {
    return represented_node_reserve() ? represented_node_finish(BinaryenLoad(m, bytes, signed_,
        offset, align, type, pointer, memory)) : NULL;
}
static BinaryenExpressionRef represented_store(BinaryenModuleRef m, uint32_t bytes,
    uint32_t offset, uint32_t align, BinaryenExpressionRef pointer,
    BinaryenExpressionRef value, BinaryenType type, const char *memory) {
    return represented_node_reserve() ? represented_node_finish(BinaryenStore(m, bytes, offset,
        align, pointer, value, type, memory)) : NULL;
}
static BinaryenExpressionRef represented_memory_size(BinaryenModuleRef m, const char *memory,
    bool is64) {
    return represented_node_reserve() ? represented_node_finish(BinaryenMemorySize(m, memory,
        is64)) : NULL;
}
static BinaryenExpressionRef represented_memory_grow(BinaryenModuleRef m,
    BinaryenExpressionRef delta, const char *memory, bool is64) {
    return represented_node_reserve() ? represented_node_finish(BinaryenMemoryGrow(m, delta,
        memory, is64)) : NULL;
}

#define BinaryenConst represented_const
#define BinaryenUnary represented_unary
#define BinaryenBinary represented_binary_node
#define BinaryenIf represented_if
#define BinaryenBlock represented_block
#define BinaryenLoop represented_loop
#define BinaryenBreak represented_break
#define BinaryenCall represented_call
#define BinaryenCallIndirect represented_call_indirect
#define BinaryenLocalGet represented_local_get
#define BinaryenLocalSet represented_local_set
#define BinaryenGlobalGet represented_global_get
#define BinaryenGlobalSet represented_global_set
#define BinaryenDrop represented_drop
#define BinaryenReturn represented_return
#define BinaryenLoad represented_load
#define BinaryenStore represented_store
#define BinaryenMemorySize represented_memory_size
#define BinaryenMemoryGrow represented_memory_grow

static void diagnostic(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics != NULL)
        (void)sol_diagnostics_add(diagnostics, "wasm-p43", SOL_SEVERITY_ERROR,
            (SolSpan){0, 0}, "%s", message);
}

static SolWasmRepresentedResult represented_backend_result(void) {
    return represented_accounting != NULL
        && represented_accounting->status == REPRESENTED_BACKEND_RESOURCE
        ? SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED : SOL_WASM_REPRESENTED_ALLOCATION_FAILED;
}

enum {
    /* One private wire envelope is shared by request validation, emission and
     * raw-byte validation.  Keeping it finite also bounds the validator's
     * stack-resident function-index/type-index lookup. */
    REPRESENTED_WIRE_MAX_FUNCTIONS = 256,
    REPRESENTED_WIRE_MAX_TABLE_ELEMENTS = 256,
    REPRESENTED_WIRE_MAX_PROVENANCE_RECORDS = 32768,
    /* Sum helper bodies reserve three setup expressions and one terminal
     * expression.  The remaining slots are one dispatch arm each. */
    REPRESENTED_SUM_HELPER_ITEMS = 128,
    REPRESENTED_SUM_HELPER_INITIAL_ITEMS = 3,
    REPRESENTED_SUM_HELPER_FINAL_ITEMS = 1,
    REPRESENTED_SUM_HELPER_MAX_VARIANTS = REPRESENTED_SUM_HELPER_ITEMS
        - REPRESENTED_SUM_HELPER_INITIAL_ITEMS - REPRESENTED_SUM_HELPER_FINAL_ITEMS,
};

static bool represented_transfer_output(RepresentedAccounting *accounting, size_t bytes) {
    size_t owned;
    if (!represented_add(accounting->owned_live, bytes, &owned)
        || owned > accounting->limits->max_owned_bytes) {
        accounting->status = REPRESENTED_BACKEND_RESOURCE;
        return false;
    }
    accounting->owned_live = owned;
    if (owned > accounting->usage->owned_bytes) accounting->usage->owned_bytes = owned;
    return true;
}

void sol_wasm_represented_output_init(SolWasmRepresentedOutput *output) {
    if (output == NULL) return;
    sol_wasm_backend_bytes_init(&output->bytes);
    memset(&output->usage, 0, sizeof output->usage);
}

void sol_wasm_represented_output_free(SolWasmRepresentedOutput *output) {
    if (output == NULL) return;
    sol_wasm_backend_bytes_free(&output->bytes);
    memset(&output->usage, 0, sizeof output->usage);
}

SolWasmRepresentedLimits sol_wasm_represented_default_limits(void) {
    return (SolWasmRepresentedLimits){
        .max_functions = REPRESENTED_WIRE_MAX_FUNCTIONS, .max_blocks = 4096, .max_edges = 8192,
        .max_values = 16384, .max_locals = 16384,
        .max_generated_nodes = 262144, .max_table_elements = REPRESENTED_WIRE_MAX_TABLE_ELEMENTS,
        /* Active data must fit the specified one-page initial memory. */
        .max_static_data_bytes = 60u * 1024u,
        .max_allocation_requests = UINT64_C(1048576),
        .max_allocation_bytes = UINT64_C(16) * 1024u * 1024u,
        .max_steps = UINT64_C(100000),
        .max_call_depth = 64,
        .max_provenance_records = REPRESENTED_WIRE_MAX_PROVENANCE_RECORDS,
        .max_work_bytes = 16u * 1024u * 1024u,
        .max_scratch_bytes = 16u * 1024u * 1024u,
        .max_owned_bytes = 32u * 1024u * 1024u,
        .max_output_bytes = 16u * 1024u * 1024u,
    };
}

static bool limits_complete(const SolWasmRepresentedLimits *limits) {
    return limits->max_functions != 0 && limits->max_blocks != 0
        && limits->max_edges != 0 && limits->max_values != 0 && limits->max_locals != 0
        && limits->max_generated_nodes != 0 && limits->max_table_elements != 0
        && limits->max_static_data_bytes != 0 && limits->max_allocation_requests != 0
        && limits->max_allocation_bytes != 0 && limits->max_steps != 0 && limits->max_provenance_records != 0
        && limits->max_call_depth != 0 && limits->max_call_depth <= 64
        && limits->max_work_bytes != 0 && limits->max_scratch_bytes != 0
        && limits->max_owned_bytes != 0 && limits->max_output_bytes != 0
        && limits->max_functions <= REPRESENTED_WIRE_MAX_FUNCTIONS
        && limits->max_table_elements <= REPRESENTED_WIRE_MAX_TABLE_ELEMENTS
        && limits->max_provenance_records <= REPRESENTED_WIRE_MAX_PROVENANCE_RECORDS;
}

static bool limits_zero(const SolWasmRepresentedLimits *limits) {
    return limits->max_functions == 0 && limits->max_blocks == 0
        && limits->max_edges == 0 && limits->max_values == 0 && limits->max_locals == 0
        && limits->max_generated_nodes == 0 && limits->max_table_elements == 0
        && limits->max_static_data_bytes == 0 && limits->max_allocation_requests == 0
        && limits->max_allocation_bytes == 0 && limits->max_steps == 0 && limits->max_provenance_records == 0
        && limits->max_call_depth == 0
        && limits->max_work_bytes == 0 && limits->max_scratch_bytes == 0
        && limits->max_owned_bytes == 0 && limits->max_output_bytes == 0;
}

static bool is_absolute_normalized(const char *path) {
    if (path == NULL || path[0] != '/') return false;
    if (path[1] == '\0' || path[strlen(path) - 1] == '/') return false;
    for (const char *component = path + 1; *component != '\0'; ) {
        const char *end = strchr(component, '/');
        size_t length = end == NULL ? strlen(component) : (size_t)(end - component);
        if (length == 0 || (length == 1 && component[0] == '.')
            || (length == 2 && component[0] == '.' && component[1] == '.')) return false;
        if (end == NULL) break;
        component = end + 1;
    }
    return true;
}

static bool package_relative(const char *root, const char *path,
    const char **relative) {
    size_t root_length;
    if (!is_absolute_normalized(root) || !is_absolute_normalized(path)) return false;
    root_length = strlen(root);
    if (strncmp(root, path, root_length) != 0 || path[root_length] != '/') return false;
    *relative = path + root_length + 1;
    return **relative != '\0';
}

static bool represented_scalar_recipe(const SolMirConcreteProgram *concrete,
    SolMirRecipeId recipe, bool terminal) {
    const SolMirMaterialization *materialization = &concrete->materialization;
    const SolMirLayout *layout = &concrete->layout;
    const SolMirRepresentation *representation = &concrete->representation;
    (void)materialization;
    (void)layout;
    if (recipe >= representation->recipe_count) return false;
    switch (representation->recipes[recipe].kind) {
        case SOL_MIR_RECIPE_INT64:
        case SOL_MIR_RECIPE_BOOL:
            return representation->recipes[recipe].storage == SOL_MIR_STORAGE_SCALAR;
        case SOL_MIR_RECIPE_TEXT:
            return representation->recipes[recipe].storage == SOL_MIR_STORAGE_TEXT_HANDLE;
        case SOL_MIR_RECIPE_UNIT:
            return representation->recipes[recipe].storage == SOL_MIR_STORAGE_NONE;
        case SOL_MIR_RECIPE_NEVER:
            return terminal;
        default:
            return false;
    }
}

/* C3a admits one nominal shape only: a source refined definition whose direct
 * backing is an ordinary scalar word.  The wrapper has no object, tag, handle,
 * padding, or ABI of its own.  Keep this test independent of CHECK_REFINED so
 * owner-shape validation can traverse places and values before the executable
 * check certificate is inspected. */
static bool represented_leaf_refined_recipe(const SolMirConcreteProgram *concrete,
    SolMirRecipeId recipe, SolMirRecipeId *backing) {
    const SolMirRepresentation *r = &concrete->representation;
    const SolMirLayout *layout = &concrete->layout;
    const SolIr *ir = concrete->program.ir;
    if (recipe >= r->recipe_count || recipe >= layout->type_count || ir == NULL)
        return false;
    const SolMirRecipe *item = &r->recipes[recipe];
    if (item->kind != SOL_MIR_RECIPE_REFINED || item->concrete_definition >= ir->definition_count
        || item->backing >= r->recipe_count || item->backing >= layout->type_count
        || item->storage != SOL_MIR_STORAGE_SCALAR || item->copy_kind != SOL_MIR_COPY_WRAPPER
        || item->drop_kind != SOL_MIR_DROP_WRAPPER || !item->inhabited || item->zero_sized
        || !item->is_copy || item->fields.count != 0 || item->variants.count != 0
        || item->parameters.count != 0 || item->parameter_accesses.count != 0
        || item->result != SOL_MIR_RECIPE_NONE || item->effects != SOL_MIR_RECIPE_NONE
        || item->capability_source != SOL_MIR_RECIPE_NONE) return false;
    const SolIrDefinition *definition = &ir->definitions[item->concrete_definition];
    if (definition->kind != SOL_IR_DEFINITION_REFINED || definition->open
        || definition->generic_parameters.count != 0 || definition->effect_parameters.count != 0
        || definition->fields.count != 0 || definition->variants.count != 0
        || definition->members.count != 0 || definition->representation >= ir->type_count)
        return false;
    const SolMirRecipe *physical = &r->recipes[item->backing];
    const SolIrType *source_backing = &ir->types[definition->representation];
    bool int64 = physical->kind == SOL_MIR_RECIPE_INT64
        && source_backing->kind == SOL_IR_TYPE_INT64;
    bool boolean = physical->kind == SOL_MIR_RECIPE_BOOL
        && source_backing->kind == SOL_IR_TYPE_BOOL;
    if ((!int64 && !boolean) || physical->storage != SOL_MIR_STORAGE_SCALAR
        || physical->copy_kind != SOL_MIR_COPY_TRIVIAL || physical->drop_kind != SOL_MIR_DROP_NONE
        || !physical->inhabited || physical->zero_sized || !physical->is_copy
        || source_backing->argument_count != 0 || source_backing->parameter_count != 0
        || source_backing->effects.count != 0) return false;
    const SolMirTypeLayout *outer = &layout->types[recipe];
    const SolMirTypeLayout *inner = &layout->types[item->backing];
    if (outer->recipe != recipe || inner->recipe != item->backing
        || outer->value_size != inner->value_size
        || outer->value_alignment != inner->value_alignment
        || outer->object_kind != SOL_MIR_LAYOUT_OBJECT_NONE || inner->object_kind
            != SOL_MIR_LAYOUT_OBJECT_NONE || outer->has_object || inner->has_object
        || outer->object_size != 0 || inner->object_size != 0
        || outer->object_alignment != inner->object_alignment
        || outer->tail_padding != inner->tail_padding
        || outer->tag_offset != inner->tag_offset || outer->tag_size != inner->tag_size
        || outer->payload_offset != inner->payload_offset
        || outer->payload_size != inner->payload_size
        || outer->data_handle_offset != inner->data_handle_offset
        || outer->length_offset != inner->length_offset
        || outer->target_token_offset != inner->target_token_offset
        || outer->environment_handle_offset != inner->environment_handle_offset
        || outer->root_token_offset != inner->root_token_offset
        || outer->private_source_handle_offset != inner->private_source_handle_offset)
        return false;
    if (backing != NULL) *backing = item->backing;
    return true;
}

/* A non-refined distinct is physically transparent: P2 deliberately gives it
 * exactly its backing layout, rather than an outer nominal header.  Keep the
 * unwrapping here, at the represented-closure boundary, so construction,
 * copying, equality, and logical drop all traverse one backing edge. */
static bool represented_backing_recipe(const SolMirConcreteProgram *concrete,
    SolMirRecipeId recipe, SolMirRecipeId *physical) {
    const SolMirRepresentation *r = &concrete->representation;
    size_t depth = 0;
    while (recipe < r->recipe_count && r->recipes[recipe].kind == SOL_MIR_RECIPE_DISTINCT) {
        if (++depth > r->recipe_count || r->recipes[recipe].backing >= r->recipe_count)
            return false;
        recipe = r->recipes[recipe].backing;
    }
    /* Refinements are never physically transparent through this general
     * helper. C3a admits them only at explicit whole-value leaf gates. */
    if (recipe >= r->recipe_count || r->recipes[recipe].kind == SOL_MIR_RECIPE_REFINED)
        return false;
    *physical = recipe;
    return true;
}
static bool represented_zero_width_unit_field(const SolMirConcreteProgram *concrete,
    SolMirRecipeId recipe, const SolMirFieldLayout *field);

/* Slice A2's aggregate universe is a finite P2 product tree.  A product is
 * indirect, but its children are only scalar leaves, Text handles, Unit, or
 * other products.  Recursive nominal products are deliberately outside this
 * first physical closure: a tree is enough to exercise P2's nested layouts
 * without admitting a cyclic runtime object graph. */
static bool represented_product_recipe_depth(const SolMirConcreteProgram *concrete,
    SolMirRecipeId recipe, size_t depth) {
    const SolMirRepresentation *representation = &concrete->representation;
    const SolMirLayout *layout = &concrete->layout;
    if (depth > representation->recipe_count || recipe >= representation->recipe_count
        || recipe >= layout->type_count) return false;
    const SolMirRecipe *item = &representation->recipes[recipe];
    const SolMirTypeLayout *type = &layout->types[recipe];
    if ((item->kind != SOL_MIR_RECIPE_TUPLE && item->kind != SOL_MIR_RECIPE_RECORD)
        || !item->inhabited || item->zero_sized || item->fields.count == 0
        || type->object_kind != SOL_MIR_LAYOUT_OBJECT_PRODUCT || !type->has_object
        || type->object_size == 0 || type->object_size > UINT32_MAX
        || type->object_alignment == 0 || type->object_alignment > 8
        || item->fields.offset > representation->field_count
        || item->fields.count > representation->field_count - item->fields.offset) return false;
    for (size_t i = 0; i < item->fields.count; ++i) {
        SolMirRecipeId child = representation->fields[item->fields.offset + i].type;
        if (child >= representation->recipe_count
            || (!represented_scalar_recipe(concrete, child, false)
                && !represented_product_recipe_depth(concrete, child, depth + 1))) return false;
    }
    return true;
}

static bool represented_scalar_product_recipe(const SolMirConcreteProgram *concrete,
    SolMirRecipeId recipe) {
    return represented_product_recipe_depth(concrete, recipe, 0);
}

static bool represented_sum_helper_variant_count_supported(size_t variants) {
    return variants <= REPRESENTED_SUM_HELPER_MAX_VARIANTS;
}

static bool represented_sum_recipe_depth(const SolMirConcreteProgram *concrete,
    SolMirRecipeId recipe, size_t depth) {
    const SolMirRepresentation *r = &concrete->representation;
    const SolMirLayout *layout = &concrete->layout;
    if (depth > r->recipe_count || recipe >= r->recipe_count || recipe >= layout->type_count)
        return false;
    const SolMirRecipe *item = &r->recipes[recipe];
    const SolMirTypeLayout *type = &layout->types[recipe];
    if ((item->kind != SOL_MIR_RECIPE_ENUM && item->kind != SOL_MIR_RECIPE_OPTION
            && item->kind != SOL_MIR_RECIPE_RESULT) || !item->inhabited || item->zero_sized
        || item->variants.count == 0
        || !represented_sum_helper_variant_count_supported(item->variants.count)
        || type->object_kind != SOL_MIR_LAYOUT_OBJECT_SUM
        || !type->has_object || type->object_size == 0 || type->object_size > UINT32_MAX
        || type->object_alignment == 0 || type->object_alignment > 8 || type->tag_offset != 0
        || type->tag_size != 4 || item->variants.offset > r->variant_count
        || item->variants.count > r->variant_count - item->variants.offset) return false;
    for (size_t i = 0; i < item->variants.count; ++i) {
        const SolMirRecipeVariant *variant = &r->variants[item->variants.offset + i];
        if (!variant->fields.count) continue;
        if (variant->fields.offset > r->field_count
            || variant->fields.count > r->field_count - variant->fields.offset) return false;
        for (size_t f = 0; f < variant->fields.count; ++f) {
            size_t field_id = variant->fields.offset + f;
            SolMirRecipeId child = r->fields[field_id].type;
            SolMirRecipeId physical;
            if (field_id >= layout->field_count || child >= r->recipe_count
                || (layout->fields[field_id].size == 0
                    && !represented_zero_width_unit_field(concrete, child, &layout->fields[field_id]))
                || (!layout->fields[field_id].has_storage && layout->fields[field_id].size != 0)
                || !represented_backing_recipe(concrete, child, &physical)
                || (!represented_scalar_recipe(concrete, physical, false)
                    && !represented_scalar_product_recipe(concrete, physical)
                    && !represented_sum_recipe_depth(concrete, physical, depth + 1))) return false;
        }
    }
    return true;
}

static bool represented_sum_recipe(const SolMirConcreteProgram *concrete,
    SolMirRecipeId recipe) {
    SolMirRecipeId physical;
    return represented_backing_recipe(concrete, recipe, &physical)
        && represented_sum_recipe_depth(concrete, physical, 0);
}

static bool represented_text_recipe(const SolMirConcreteProgram *concrete,
    SolMirRecipeId recipe) {
    SolMirRecipeId physical;
    return represented_backing_recipe(concrete, recipe, &physical)
        && concrete->representation.recipes[physical].kind == SOL_MIR_RECIPE_TEXT;
}

static bool represented_direct_text_handle(const SolMirConcreteProgram *concrete,
    SolMirRecipeId recipe) {
    const SolMirRepresentation *r = &concrete->representation;
    const SolMirLayout *layout = &concrete->layout;
    return recipe < r->recipe_count && recipe < layout->type_count
        && r->recipes[recipe].kind == SOL_MIR_RECIPE_TEXT && r->recipes[recipe].inhabited
        && r->recipes[recipe].storage == SOL_MIR_STORAGE_TEXT_HANDLE
        && layout->types[recipe].object_kind == SOL_MIR_LAYOUT_OBJECT_TEXT
        && layout->types[recipe].has_object && layout->types[recipe].object_size == 8
        && layout->types[recipe].object_alignment == 4;
}

static bool represented_product_recipe(const SolMirConcreteProgram *concrete,
    SolMirRecipeId recipe) {
    SolMirRecipeId physical;
    return represented_backing_recipe(concrete, recipe, &physical)
        && represented_scalar_product_recipe(concrete, physical);
}

static bool represented_indirect_recipe(const SolMirConcreteProgram *concrete,
    SolMirRecipeId recipe) {
    return represented_text_recipe(concrete, recipe)
        || represented_product_recipe(concrete, recipe)
        || represented_sum_recipe(concrete, recipe);
}

/* An unbound function value is a P2 callable header carried as an i64 Wasm
 * handle.  Bound operations deliberately remain out of this slice: their
 * second header word is an environment and belongs with C2 receivers. */
static bool represented_unbound_function_recipe(const SolMirConcreteProgram *concrete,
    SolMirRecipeId recipe) {
    const SolMirRepresentation *r = &concrete->representation;
    const SolMirLayout *layout = &concrete->layout;
    if (recipe >= r->recipe_count || recipe >= layout->type_count) return false;
    const SolMirRecipe *item = &r->recipes[recipe];
    const SolMirTypeLayout *type = &layout->types[recipe];
    return item->kind == SOL_MIR_RECIPE_FUNCTION && item->inhabited
        && item->storage == SOL_MIR_STORAGE_CALLABLE_HANDLE
        && type->object_kind == SOL_MIR_LAYOUT_OBJECT_CALLABLE && type->has_object
        && type->object_size == layout->target.pointer_size
        && type->object_alignment == layout->target.pointer_alignment
        && type->target_token_offset == 0
        && type->environment_handle_offset == SOL_MIR_LAYOUT_OFFSET_NONE;
}

/* This is deliberately not the recursive product closure above.  A callable
 * leaf is admitted only directly in a fixed product, so one projected move can
 * name one P2 field and one local hole bit.  Nested callable products, bound
 * operations, and every copy/equality route stay outside this slice. */
static bool represented_callable_product_recipe(const SolMirConcreteProgram *concrete,
    SolMirRecipeId recipe) {
    const SolMirRepresentation *r = &concrete->representation;
    const SolMirLayout *layout = &concrete->layout;
    size_t callable_count = 0, text_count = 0;
    if (recipe >= r->recipe_count || recipe >= layout->type_count
        || (r->recipes[recipe].kind != SOL_MIR_RECIPE_RECORD
            && r->recipes[recipe].kind != SOL_MIR_RECIPE_TUPLE)
        || r->recipes[recipe].copy_kind != SOL_MIR_COPY_FORBIDDEN
        || r->recipes[recipe].drop_kind != SOL_MIR_DROP_AGGREGATE
        || !r->recipes[recipe].inhabited || r->recipes[recipe].zero_sized
        || r->recipes[recipe].fields.count != 2
        || r->recipes[recipe].fields.offset > r->field_count
        || r->recipes[recipe].fields.count > r->field_count - r->recipes[recipe].fields.offset
        || layout->types[recipe].object_kind != SOL_MIR_LAYOUT_OBJECT_PRODUCT
        || !layout->types[recipe].has_object || layout->types[recipe].object_size == 0
        || layout->types[recipe].object_size > UINT32_MAX
        || layout->types[recipe].object_alignment == 0 || layout->types[recipe].object_alignment > 8)
        return false;
    for (size_t i = 0; i < r->recipes[recipe].fields.count; ++i) {
        size_t field_id = r->recipes[recipe].fields.offset + i;
        SolMirRecipeId child = r->fields[field_id].type;
        const SolMirFieldLayout *field = field_id < layout->field_count ? &layout->fields[field_id] : NULL;
        if (field == NULL || field->owner_recipe != recipe || child >= r->recipe_count
            || (field->size == 0 && !represented_zero_width_unit_field(concrete, child, field))
            || (field->size != 0 && (!field->has_storage || (field->size != 1
                && field->size != 4 && field->size != 8)))) return false;
        if (represented_unbound_function_recipe(concrete, child)) {
            if (field->size != 4 || field->alignment != 4) return false;
            if (callable_count == SIZE_MAX) return false;
            ++callable_count;
        } else if (represented_direct_text_handle(concrete, child)) {
            if (field->size != 4 || field->alignment != 4) return false;
            if (text_count == SIZE_MAX) return false;
            ++text_count;
        } else return false;
    }
    return callable_count == 1 && text_count == 1;
}

/* P2 may retain a field record for Unit at its ordinary absolute layout
 * position (including a nonzero offset), while assigning it a zero byte
 * width.  It is therefore neither a byte-addressable field nor malformed
 * padding.  Distinct<Unit> follows its single transparent backing edge. */
static bool represented_zero_width_unit_field(const SolMirConcreteProgram *concrete,
    SolMirRecipeId recipe, const SolMirFieldLayout *field) {
    SolMirRecipeId physical;
    return field != NULL && field->size == 0 && field->alignment == 1
        && represented_backing_recipe(concrete, recipe, &physical)
        && concrete->representation.recipes[physical].kind == SOL_MIR_RECIPE_UNIT;
}

static bool represented_recipe(const SolMirConcreteProgram *concrete,
    SolMirMaterializedTypeId type, bool terminal) {
    const SolMirMaterialization *materialization = &concrete->materialization;
    const SolMirLayout *layout = &concrete->layout;
    if (type >= materialization->type_count || type >= layout->type_count) return false;
    SolMirRecipeId recipe = layout->types[type].recipe;
    if (represented_leaf_refined_recipe(concrete, recipe, NULL)) return true;
    SolMirRecipeId physical;
    return represented_backing_recipe(concrete, recipe, &physical)
        && (represented_scalar_recipe(concrete, physical, terminal)
            || represented_scalar_product_recipe(concrete, physical)
            || represented_sum_recipe_depth(concrete, physical, 0)
            || represented_unbound_function_recipe(concrete, physical)
            || represented_callable_product_recipe(concrete, physical));
}

static bool whole_represented_place(const SolMirConcreteProgram *concrete,
    SolMirMaterializedPlaceId place) {
    const SolMirMaterialization *materialization = &concrete->materialization;
    if (place >= materialization->place_count) return false;
    const SolMirMaterializedPlace *item = &materialization->places[place];
    return item->projections.count == 0
        && represented_recipe(concrete, item->final_type, false);
}

static bool represented_place(const SolMirConcreteProgram *concrete,
    SolMirMaterializedPlaceId place) {
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirLayout *layout = &concrete->layout;
    if (place >= m->place_count) return false;
    const SolMirMaterializedPlace *item = &m->places[place];
    if (item->projections.count == 0) return whole_represented_place(concrete, place);
    if (item->root_type >= layout->type_count || item->final_type >= layout->type_count
        || (!represented_scalar_product_recipe(concrete, layout->types[item->root_type].recipe)
            && !represented_callable_product_recipe(concrete,
                layout->types[item->root_type].recipe))
        || item->projections.offset > m->projection_count
        || item->projections.count > m->projection_count - item->projections.offset
        || item->projections.offset > layout->projection_count
        || item->projections.count > layout->projection_count - item->projections.offset) return false;
    SolMirRecipeId current = layout->types[item->root_type].recipe;
    for (size_t i = 0; i < item->projections.count; ++i) {
        const SolMirProjectionMap *map = &layout->projections[item->projections.offset + i];
        if (map->place != place || map->base_recipe != current || map->field_layout >= layout->field_count
            || map->result_recipe >= concrete->representation.recipe_count
            || map->object_offset != layout->fields[map->field_layout].offset) return false;
        const SolMirFieldLayout *field = &layout->fields[map->field_layout];
        if (field->owner_recipe != current
            || (field->has_storage && (field->size != 1 && field->size != 4 && field->size != 8))) return false;
        current = map->result_recipe;
        if (i + 1 != item->projections.count
            && (!field->has_storage || field->size != 4
                || !represented_scalar_product_recipe(concrete, current))) return false;
    }
    if (represented_callable_product_recipe(concrete, layout->types[item->root_type].recipe))
        return item->projections.count == 1 && current == layout->types[item->final_type].recipe
            && represented_unbound_function_recipe(concrete, current);
    return current == layout->types[item->final_type].recipe
        && (represented_scalar_recipe(concrete, current, false)
            || represented_scalar_product_recipe(concrete, current));
}

static bool represented_instruction_kind_supported(SolMirInstructionKind kind) {
    switch (kind) {
        case SOL_MIR_INST_CONST_INT64: case SOL_MIR_INST_CONST_BOOL: case SOL_MIR_INST_CONST_TEXT:
        case SOL_MIR_INST_CONST_UNIT: case SOL_MIR_INST_PARAMETER_LIVE:
        case SOL_MIR_INST_STORAGE_LIVE: case SOL_MIR_INST_STORAGE_DEAD:
        case SOL_MIR_INST_DROP_IF_INITIALIZED: case SOL_MIR_INST_TEMPORARY_DROP:
        case SOL_MIR_INST_REGION_ENTER: case SOL_MIR_INST_REGION_EXIT:
        case SOL_MIR_INST_SCOPE_ENTER: case SOL_MIR_INST_SCOPE_EXIT:
        case SOL_MIR_INST_LOAD_COPY: case SOL_MIR_INST_LOAD_MOVE:
        case SOL_MIR_INST_LOAD_UPDATE: case SOL_MIR_INST_STORE:
        case SOL_MIR_INST_COMPOUND_UPDATE: case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED:
        case SOL_MIR_INST_UNARY: case SOL_MIR_INST_BINARY:
        case SOL_MIR_INST_TEMPORARY_INIT: case SOL_MIR_INST_EXPRESSION_RESULT:
        case SOL_MIR_INST_CONSTRUCT: case SOL_MIR_INST_PATTERN_TEST:
        case SOL_MIR_INST_PATTERN_VALUE: case SOL_MIR_INST_MATCH_ARM:
        case SOL_MIR_INST_FUNCTION_VALUE: case SOL_MIR_INST_CAPTURE_SNAPSHOT:
            return true;
        default: return false;
    }
}

static bool represented_instruction(const SolMirConcreteProgram *concrete,
    const SolMirMaterializedInstruction *instruction) {
    if (!represented_instruction_kind_supported(instruction->kind)) return false;
    switch (instruction->kind) {
        case SOL_MIR_INST_CONST_INT64:
        case SOL_MIR_INST_CONST_BOOL:
        case SOL_MIR_INST_CONST_TEXT:
        case SOL_MIR_INST_CONST_UNIT:
        case SOL_MIR_INST_PARAMETER_LIVE:
        case SOL_MIR_INST_STORAGE_LIVE:
        case SOL_MIR_INST_STORAGE_DEAD:
        case SOL_MIR_INST_DROP_IF_INITIALIZED:
        case SOL_MIR_INST_REGION_ENTER:
        case SOL_MIR_INST_REGION_EXIT:
        case SOL_MIR_INST_SCOPE_ENTER:
        case SOL_MIR_INST_SCOPE_EXIT:
            return true;
        case SOL_MIR_INST_LOAD_COPY:
        case SOL_MIR_INST_LOAD_MOVE:
        case SOL_MIR_INST_LOAD_UPDATE:
        case SOL_MIR_INST_STORE:
        case SOL_MIR_INST_COMPOUND_UPDATE:
        case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED:
            if (!represented_place(concrete, instruction->place)) return false;
            if (instruction->kind == SOL_MIR_INST_LOAD_COPY
                && instruction->place < concrete->materialization.place_count) {
                SolMirMaterializedTypeId type = concrete->materialization.places[
                    instruction->place].final_type;
                return type >= concrete->layout.type_count || !represented_unbound_function_recipe(concrete,
                    concrete->layout.types[type].recipe);
            }
            return true;
        case SOL_MIR_INST_CONSTRUCT:
            return true; /* Exact P3.6 constructor join is checked per image. */
        case SOL_MIR_INST_PATTERN_TEST:
        case SOL_MIR_INST_PATTERN_VALUE:
        case SOL_MIR_INST_MATCH_ARM:
            return true; /* Authenticated operation rows are checked per image. */
        case SOL_MIR_INST_UNARY:
        case SOL_MIR_INST_BINARY:
        case SOL_MIR_INST_EXPRESSION_RESULT:
            return represented_recipe(concrete, instruction->type, false);
        case SOL_MIR_INST_TEMPORARY_INIT:
        case SOL_MIR_INST_TEMPORARY_DROP:
            return instruction->temporary < concrete->materialization.temporary_count
                && represented_recipe(concrete,
                    concrete->materialization.temporaries[instruction->temporary].type, false);
        case SOL_MIR_INST_FUNCTION_VALUE:
            return instruction->type < concrete->layout.type_count
                && represented_unbound_function_recipe(concrete,
                    concrete->layout.types[instruction->type].recipe);
        case SOL_MIR_INST_CAPTURE_SNAPSHOT:
            return true; /* The exact P2/P3.6 snapshot join is checked per image. */
        default:
            return false;
    }
}

static bool represented_terminator(const SolMirMaterializedTerminator *terminator) {
    switch (terminator->kind) {
        case SOL_MIR_TERM_GOTO:
        case SOL_MIR_TERM_BRANCH:
        case SOL_MIR_TERM_BREAK:
        case SOL_MIR_TERM_CONTINUE:
        case SOL_MIR_TERM_RETURN:
        case SOL_MIR_TERM_INVOKE:
        case SOL_MIR_TERM_CHECK_CONTRACT:
        case SOL_MIR_TERM_CHECK_REFINED:
        case SOL_MIR_TERM_CONTRACT_VIOLATION:
        case SOL_MIR_TERM_PANIC:
        case SOL_MIR_TERM_MATCH_FAILURE:
        case SOL_MIR_TERM_UNREACHABLE:
        case SOL_MIR_TERM_PROPAGATE:
            return true;
        default:
            return false;
    }
}

/* A pending resume is not represented-emittable in Slice 3A.  It is admitted at
 * the owner boundary solely so the call catalog can prove that it is the
 * exact named failure continuation of one direct internal call. */
static bool represented_owner_terminator(const SolMirMaterializedTerminator *terminator,
    size_t call_count) {
    (void)call_count;
    return represented_terminator(terminator)
        || terminator->kind == SOL_MIR_TERM_RESUME_FAILURE;
}

/* Owner validation is deliberately the first dereference after request shape
 * checks.  It authenticates the exact P3.6 chain; the backend never accepts a
 * materialization or any predecessor as a substitute input. */
static bool represented_owner(const SolWasmRepresentedBuildRequest *request,
    SolWasmRepresentedUsage *usage) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    if (!sol_mir_runtime_lowered_program_validate(owner, NULL)) return false;
    const SolMirConcreteProgram *concrete = owner->conventions->concrete;
    const SolMirMaterialization *materialization = &concrete->materialization;
    const SolIr *ir = concrete->program.ir;
    if (!is_absolute_normalized(request->package_directory)
        || !sol_mir_target_descriptor_validate(&concrete->layout.target)) return false;
    SolMirTargetDescriptor wasm32 = sol_mir_target_wasm32();
    if (concrete->layout.target.pointer_size != wasm32.pointer_size
        || concrete->layout.target.pointer_alignment != wasm32.pointer_alignment
        || concrete->layout.target.int64_alignment != wasm32.int64_alignment
        || concrete->layout.target.endianness != wasm32.endianness
        || concrete->layout.target.max_object_bytes != wasm32.max_object_bytes) return false;
    for (size_t i = 0; i < ir->file_count; ++i) {
        const char *relative = NULL;
        if (!package_relative(request->package_directory, ir->files[i].path, &relative))
            return false;
    }
    if (owner->host_requirement_count != 0 || owner->handler_frame_count != 0)
        return false;
    for (size_t i = 0; i < owner->import_count; ++i) {
        const SolMirRuntimeLoweredImport *lowered = &owner->imports[i];
        if (lowered->state != SOL_MIR_RUNTIME_LOWERED_PRESENT || lowered->import_id != i
            || i >= owner->conventions->import_count) return false;
        const SolMirRuntimeImport *item = &owner->conventions->imports[i];
        if (item->recipe >= concrete->representation.recipe_count
            || !represented_recipe(concrete, item->recipe, false)) return false;
        if (concrete->representation.recipes[item->recipe].kind == SOL_MIR_RECIPE_REFINED) {
            if (!represented_leaf_refined_recipe(concrete, item->recipe, NULL)
                || item->host != SOL_MIR_LINKAGE_NONE || item->recipe_operation == 0
                || item->recipe >= owner->recipe_count) return false;
            const SolMirRuntimeLoweredRecipe *recipe = &owner->recipes[item->recipe];
            bool named = (item->kind == SOL_MIR_RUNTIME_IMPORT_RECIPE_COPY
                    && recipe->copy_import == i)
                || (item->kind == SOL_MIR_RUNTIME_IMPORT_RECIPE_DROP
                    && recipe->drop_import == i)
                || (item->kind == SOL_MIR_RUNTIME_IMPORT_RECIPE_EQUAL
                    && recipe->equal_import == i);
            if (!named || recipe->state != SOL_MIR_RUNTIME_LOWERED_PRESENT
                || recipe->recipe != item->recipe
                || recipe->create_import != SOL_MIR_RUNTIME_NONE
                || recipe->allocation != SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE
                || recipe->copy != SOL_MIR_RUNTIME_COPY_WRAPPER
                || recipe->equality != SOL_MIR_RUNTIME_EQUALITY_WRAPPER
                || recipe->ownership != SOL_MIR_RUNTIME_OWNERSHIP_WRAPPER
                || owner->imports[i].state != SOL_MIR_RUNTIME_LOWERED_PRESENT
                || owner->imports[i].import_id != i) return false;
            continue;
        }
        switch (item->kind) {
            case SOL_MIR_RUNTIME_IMPORT_RECIPE_CREATE:
            case SOL_MIR_RUNTIME_IMPORT_RECIPE_COPY:
            case SOL_MIR_RUNTIME_IMPORT_RECIPE_DROP:
            case SOL_MIR_RUNTIME_IMPORT_RECIPE_EQUAL: break;
            default: return false;
        }
    }
    for (size_t i = 0; i < materialization->local_count; ++i) {
        const SolMirMaterializedLocal *local = &materialization->locals[i];
        if ((local->kind == SOL_MIR_MATERIALIZED_LOCAL_RECEIVER
                && local->access != SOL_ACCESS_SHARED
                && local->access != SOL_ACCESS_EXCLUSIVE)
            || (local->kind != SOL_MIR_MATERIALIZED_LOCAL_RECEIVER
                && local->access != SOL_ACCESS_OWNED && local->access != SOL_ACCESS_EXCLUSIVE)
            || !represented_recipe(concrete, local->type, false)) return false;
    }
    for (size_t i = 0; i < materialization->place_count; ++i)
        if (!represented_place(concrete, i)) return false;
    for (size_t i = 0; i < materialization->temporary_count; ++i)
        if (!represented_recipe(concrete, materialization->temporaries[i].type, false)) return false;
    for (size_t i = 0; i < materialization->value_count; ++i)
        if (!represented_recipe(concrete, materialization->values[i].type, false)) return false;
    for (size_t i = 0; i < materialization->instruction_count; ++i)
        if (!represented_instruction(concrete, &materialization->instructions[i])) return false;
    for (size_t i = 0; i < materialization->block_count; ++i)
        if (!represented_owner_terminator(&materialization->blocks[i].terminator,
                owner->call_count)) return false;
    if (!represented_add(concrete->linkage.callable_count, owner->conventions->entry_count,
            &usage->functions)
        || usage->functions > UINT32_MAX) return false;
    usage->blocks = materialization->block_count;
    usage->edges = materialization->edge_count;
    usage->values = materialization->value_count;
    if (!represented_add(owner->conventions->entry_count, concrete->linkage.callable_count,
            &usage->provenance_records)
        || !represented_add(usage->provenance_records, owner->conventions->failure_site_count,
            &usage->provenance_records)
        || !represented_add(usage->provenance_records, owner->cleanup->supplemental_site_count,
            &usage->provenance_records)
        || usage->provenance_records > UINT32_MAX) return false;
    return usage->blocks <= UINT32_MAX && usage->edges <= UINT32_MAX
        && usage->values <= UINT32_MAX;
}

/* Count emitted MIR quantities by their owning ranges instead of trusting a
 * single aggregate field.  The equality checks keep the limits tied to the
 * same closure that the emitter later walks. */
static bool represented_instance_admitted(const SolMirConcreteProgram *concrete, size_t image) {
    for (size_t callable = 0; callable < concrete->linkage.callable_count; ++callable)
        if (concrete->linkage.callables[callable].instance == image) return true;
    return false;
}

static bool represented_usage_census(const SolWasmRepresentedBuildRequest *request,
    SolWasmRepresentedUsage *usage) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    size_t blocks = 0, values = 0, edges = 0;
    for (size_t i = 0; i < concrete->linkage.callable_count; ++i) {
        SolMirPlanInstanceId image_id = concrete->linkage.callables[i].instance;
        if (image_id >= m->image_count) return false;
        const SolMirMaterializedImage *image = &m->images[image_id];
        if (!represented_add(blocks, image->blocks.count, &blocks)
            || !represented_add(values, image->values.count, &values)) return false;
    }
    for (size_t i = 0; i < m->edge_count; ++i) {
        if (i >= request->program->image_edge_count
            || request->program->image_edges[i].image >= m->image_count
            || !represented_add(edges, 1, &edges)) return false;
    }
    if (blocks != m->block_count || values != m->value_count || edges != m->edge_count
        || blocks > UINT32_MAX || values > UINT32_MAX || edges > UINT32_MAX) return false;
    const SolMirOperations *operations = &concrete->operations;
    for (size_t body_id = 0; body_id < operations->predicate_body_count; ++body_id) {
        const SolMirPredicateBody *body = &operations->predicate_bodies[body_id];
        if (body->owner_kind != SOL_MIR_PREDICATE_OWNER_INSTANCE
            || !represented_instance_admitted(concrete, body->instance)) continue;
        if (body->blocks.offset > operations->predicate_block_count || body->blocks.count
            > operations->predicate_block_count - body->blocks.offset || body->values.offset
            > operations->predicate_value_count || body->values.count
            > operations->predicate_value_count - body->values.offset
            || !represented_add(blocks, body->blocks.count, &blocks)
            || !represented_add(values, body->values.count, &values)) return false;
    }
    for (size_t edge_id = 0; edge_id < operations->predicate_edge_count; ++edge_id) {
        const SolMirPredicateEdge *edge = &operations->predicate_edges[edge_id];
        if (edge->source >= operations->predicate_block_count) return false;
        const SolMirPredicateBlock *source = &operations->predicate_blocks[edge->source];
        if (source->body >= operations->predicate_body_count) return false;
        const SolMirPredicateBody *body = &operations->predicate_bodies[source->body];
        if (body->owner_kind == SOL_MIR_PREDICATE_OWNER_INSTANCE
            && represented_instance_admitted(concrete, body->instance)
            && !represented_add(edges, 1, &edges)) return false;
    }
    usage->blocks = blocks; usage->values = values; usage->edges = edges;
    return usage->blocks <= UINT32_MAX && usage->values <= UINT32_MAX
        && usage->edges <= UINT32_MAX;
}

static void put_u32(uint8_t **cursor, uint32_t value) {
    (*cursor)[0] = (uint8_t)value; (*cursor)[1] = (uint8_t)(value >> 8);
    (*cursor)[2] = (uint8_t)(value >> 16); (*cursor)[3] = (uint8_t)(value >> 24);
    *cursor += 4;
}

typedef struct RepresentedCallCatalogGraph RepresentedCallCatalogGraph;
typedef struct RepresentedQualifiedIndex RepresentedQualifiedIndex;

typedef enum {
    REPRESENTED_PROVENANCE_SOURCE_NONE,
    REPRESENTED_PROVENANCE_SOURCE_FAILURE,
    REPRESENTED_PROVENANCE_SOURCE_SUPPLEMENTAL,
} RepresentedProvenanceSource;

typedef struct {
    uint8_t tag, kind;
    const char *path, *symbol;
    size_t start, end, ordinal;
    /* These authenticate the dense source-to-canonical-record maps only; the
     * private wire schema deliberately does not serialize source IDs. */
    RepresentedProvenanceSource source_class;
    size_t source_id;
} ProvenanceRecord;

typedef struct {
    ProvenanceRecord *records;
    uint8_t *bytes;
    uint32_t *failure_record_indices;
    uint32_t *supplemental_record_indices;
    size_t count, byte_count;
    size_t failure_site_count, supplemental_site_count;
} RepresentedProvenance;

static int provenance_order(const void *left, const void *right) {
    const ProvenanceRecord *a = left, *b = right;
    unsigned a_rank = a->tag == 3
            && (a->kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_STEP
                || a->kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_STEP)
        ? 5u : a->tag;
    unsigned b_rank = b->tag == 3
            && (b->kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_STEP
                || b->kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_STEP)
        ? 5u : b->tag;
    int order = a_rank == b_rank ? 0 : a_rank < b_rank ? -1 : 1;
    if (order != 0) return order;
    order = strcmp(a->path, b->path); if (order != 0) return order;
    if (a->start != b->start) return a->start < b->start ? -1 : 1;
    if (a->end != b->end) return a->end < b->end ? -1 : 1;
    order = strcmp(a->symbol, b->symbol); if (order != 0) return order;
    if (a->ordinal != b->ordinal) return a->ordinal < b->ordinal ? -1 : 1;
    return (int)a->kind - (int)b->kind;
}

static bool provenance_source(const SolIr *ir, const char *root,
    SolMirRuntimeSource source, const char **path) {
    return source.file < ir->file_count && source.start <= UINT32_MAX
        && source.end <= UINT32_MAX && source.start <= source.end
        && package_relative(root, ir->files[source.file].path, path);
}

static bool callable_source(const SolIr *ir, const char *root, SolSpan span,
    const char **path, size_t *start, size_t *end) {
    for (size_t i = 0; i < ir->file_count; ++i) {
        const SolIrSourceFile *file = &ir->files[i];
        if (span.start >= file->aggregate_start && span.end <= file->aggregate_end
            && package_relative(root, file->path, path)) {
            *start = span.start - file->aggregate_start;
            *end = span.end - file->aggregate_start;
            return *start <= UINT32_MAX && *end <= UINT32_MAX;
        }
    }
    return false;
}

static bool represented_runtime_source(const SolIr *ir, SolSpan span,
    SolMirRuntimeSource *source) {
    if (ir == NULL || source == NULL) return false;
    size_t matches = 0;
    for (size_t i = 0; i < ir->file_count; ++i) {
        const SolIrSourceFile *file = &ir->files[i];
        if (span.start < file->aggregate_start || span.end > file->aggregate_end) continue;
        *source = (SolMirRuntimeSource){i, span.start - file->aggregate_start,
            span.end - file->aggregate_start};
        ++matches;
    }
    return matches == 1 && source->start <= source->end;
}

/* P2's PROPAGATE terminator span names its operand.  The private P4.3
 * supplemental-allocation record names the complete source operation instead:
 * include the immediately following propagation marker when it is present.
 * This normalization remains local to the serialized P4.3 provenance schema;
 * it does not alter the authenticated P2/P3 source record. */
static bool supplemental_provenance_source(const SolIr *ir,
    const SolMirRuntimeCleanupSupplementalSite *site,
    const SolMirRuntimeCleanupEvent *event, SolMirRuntimeSource *source) {
    *source = site->source;
    (void)event;
    if (source->file >= ir->file_count || source->file > SIZE_MAX - ir->files[source->file].aggregate_start)
        return false;
    size_t absolute_end = ir->files[source->file].aggregate_start + source->end;
    if (source->end > ir->files[source->file].aggregate_end - ir->files[source->file].aggregate_start
        || absolute_end >= ir->source_length || ir->source_bytes == NULL) return false;
    if (ir->source_bytes[absolute_end] == '?') ++source->end;
    return true;
}

/* An ordinary allocation and its PROPAGATE residual prerequisite can name the
 * same complete `value?` source operation.  Preserve both records (and their
 * stable order) through the schema's existing kind discriminator. */
static uint8_t supplemental_provenance_kind(const SolMirRuntimeCleanupEvent *event) {
    return event->producer == SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PROPAGATION_RESIDUAL ? 1 : 0;
}

static const char *symbol_for_image(const SolMirConcreteProgram *concrete,
    SolMirPlanInstanceId image) {
    const char *symbol = NULL;
    for (size_t i = 0; i < concrete->linkage.callable_count; ++i) {
        if (concrete->linkage.callables[i].instance != image) continue;
        if (symbol != NULL) return NULL;
        symbol = concrete->linkage.callables[i].symbol.bytes;
    }
    return symbol;
}

static const char *symbol_for_failure_site(const SolMirConcreteProgram *concrete,
    const SolMirRuntimeFailureSite *site) {
    if (site->origin_kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC
        || site->origin_kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_CALL
        || site->origin_kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_NO_MATCH
        || site->origin_kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT
        || site->origin_kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_STEP) {
        if (site->owner >= concrete->operations.predicate_body_count) return NULL;
        const SolMirPredicateBody *body = &concrete->operations.predicate_bodies[site->owner];
        if (body->owner_kind != SOL_MIR_PREDICATE_OWNER_INSTANCE) return NULL;
        return symbol_for_image(concrete, body->instance);
    }
    return symbol_for_image(concrete, site->owner);
}

static void represented_provenance_free(RepresentedProvenance *provenance) {
    if (provenance == NULL) return;
    deallocate(provenance->records);
    deallocate(provenance->bytes);
    deallocate(provenance->failure_record_indices);
    deallocate(provenance->supplemental_record_indices);
    memset(provenance, 0, sizeof *provenance);
}

/* Build the canonical schema once, retaining both the serialized section and
 * dense source-site maps for emission.  P3.1's authenticated occurrence keeps
 * distinct sites distinct even when their canonical source spans coincide. */
static bool represented_provenance_build(const SolWasmRepresentedBuildRequest *request,
    RepresentedProvenance *provenance) {
    const SolIr *ir = request->program->conventions->concrete->program.ir;
    const SolMirRuntimeConventions *conventions = request->program->conventions;
    const SolMirConcreteProgram *concrete = conventions->concrete;
    const SolMirRuntimeCleanup *cleanup = request->program->cleanup;
    size_t count = 0;
    memset(provenance, 0, sizeof *provenance);
    if (!represented_add(conventions->entry_count, concrete->linkage.callable_count, &count)
        || !represented_add(count, conventions->failure_site_count, &count)
        || !represented_add(count, cleanup->supplemental_site_count, &count)
        || count > REPRESENTED_WIRE_MAX_PROVENANCE_RECORDS || count > UINT32_MAX) {
        if (represented_accounting != NULL) represented_accounting->status = REPRESENTED_BACKEND_RESOURCE;
        return false;
    }
    provenance->failure_site_count = conventions->failure_site_count;
    provenance->supplemental_site_count = cleanup->supplemental_site_count;
    provenance->records = count == 0 ? NULL : allocate(count, sizeof *provenance->records);
    provenance->failure_record_indices = conventions->failure_site_count == 0 ? NULL
        : allocate(conventions->failure_site_count, sizeof *provenance->failure_record_indices);
    provenance->supplemental_record_indices = cleanup->supplemental_site_count == 0 ? NULL
        : allocate(cleanup->supplemental_site_count, sizeof *provenance->supplemental_record_indices);
    if ((count != 0 && provenance->records == NULL)
        || (conventions->failure_site_count != 0 && provenance->failure_record_indices == NULL)
        || (cleanup->supplemental_site_count != 0
            && provenance->supplemental_record_indices == NULL)) goto failed;

    size_t at = 0, bytes = 12;
    for (size_t i = 0; i < conventions->entry_count; ++i) {
        const SolMirRuntimeEntry *entry = &conventions->entries[i]; const char *path = NULL;
        SolMirRuntimeSource source = {entry->source.file, entry->source.start, entry->source.end};
        if (!provenance_source(ir, request->package_directory, source, &path)) goto failed;
        provenance->records[at++] = (ProvenanceRecord){1, 0, path, entry->symbol.bytes,
            source.start, source.end, 0, REPRESENTED_PROVENANCE_SOURCE_NONE, 0};
    }
    for (size_t i = 0; i < concrete->linkage.callable_count; ++i) {
        const SolMirLinkageCallable *callable = &concrete->linkage.callables[i];
        if (callable->instance >= concrete->materialization.image_count) goto failed;
        SolIrCallableId source = concrete->materialization.images[callable->instance].source_callable;
        const char *path = NULL; size_t start, end;
        if (source >= ir->callable_count || !callable_source(ir, request->package_directory,
                ir->callables[source].span, &path, &start, &end)) goto failed;
        provenance->records[at++] = (ProvenanceRecord){2, 0, path, callable->symbol.bytes, start,
            end, 0, REPRESENTED_PROVENANCE_SOURCE_NONE, 0};
    }
    for (size_t i = 0; i < conventions->failure_site_count; ++i) {
        const SolMirRuntimeFailureSite *site = &conventions->failure_sites[i];
        const char *relative = NULL;
        if (!provenance_source(ir, request->package_directory, site->source, &relative)) goto failed;
        const char *symbol = symbol_for_failure_site(concrete, site);
        if (symbol == NULL) goto failed;
        provenance->records[at++] = (ProvenanceRecord){3, (uint8_t)site->origin_kind, relative,
            symbol, site->source.start, site->source.end, site->occurrence,
            REPRESENTED_PROVENANCE_SOURCE_FAILURE, i};
    }
    for (size_t i = 0; i < cleanup->supplemental_site_count; ++i) {
        const SolMirRuntimeCleanupSupplementalSite *site = &cleanup->supplemental_sites[i];
        const char *relative = NULL; SolMirRuntimeSource source;
        if (site->event >= cleanup->event_count) goto failed;
        const SolMirRuntimeCleanupEvent *event = &cleanup->events[site->event];
        if (!supplemental_provenance_source(ir, site, event, &source)
            || !provenance_source(ir, request->package_directory, source, &relative)
            || event->owner >= concrete->materialization.image_count) goto failed;
        const char *symbol = symbol_for_image(concrete, event->owner);
        if (symbol == NULL) goto failed;
        provenance->records[at++] = (ProvenanceRecord){4, supplemental_provenance_kind(event), relative,
            symbol, source.start, source.end, 0, REPRESENTED_PROVENANCE_SOURCE_SUPPLEMENTAL, i};
    }
    if (at != count) goto failed;
    if (count != 0) qsort(provenance->records, count, sizeof *provenance->records, provenance_order);
    if (conventions->failure_site_count != 0)
        memset(provenance->failure_record_indices, 0, conventions->failure_site_count
            * sizeof *provenance->failure_record_indices);
    for (size_t i = 0; i < count; ++i) {
        ProvenanceRecord *record = &provenance->records[i];
        if (i != 0 && provenance_order(&provenance->records[i - 1], record) == 0) goto failed;
        uint32_t one_based = (uint32_t)(i + 1);
        if (record->source_class == REPRESENTED_PROVENANCE_SOURCE_FAILURE) {
            if (record->source_id >= conventions->failure_site_count
                || provenance->failure_record_indices[record->source_id] != 0) goto failed;
            provenance->failure_record_indices[record->source_id] = one_based;
        } else if (record->source_class == REPRESENTED_PROVENANCE_SOURCE_SUPPLEMENTAL) {
            if (record->source_id >= cleanup->supplemental_site_count
                || provenance->supplemental_record_indices[record->source_id] != 0) goto failed;
            provenance->supplemental_record_indices[record->source_id] = one_based;
        } else if (record->source_class != REPRESENTED_PROVENANCE_SOURCE_NONE) goto failed;
        size_t path_length = strlen(record->path), symbol_length = strlen(record->symbol);
        size_t record_bytes = 24;
        if (path_length > UINT32_MAX || symbol_length > UINT32_MAX
            || record->ordinal > UINT32_MAX
            || path_length > SIZE_MAX - record_bytes) goto failed;
        record_bytes += path_length;
        if (symbol_length > SIZE_MAX - record_bytes) goto failed;
        record_bytes += symbol_length;
        if (record_bytes > SIZE_MAX - bytes) goto failed;
        bytes += record_bytes;
    }
    for (size_t i = 0; i < conventions->failure_site_count; ++i)
        if (provenance->failure_record_indices[i] == 0) goto failed;
    for (size_t i = 0; i < cleanup->supplemental_site_count; ++i)
        if (provenance->supplemental_record_indices[i] == 0) goto failed;
    if (bytes > UINT32_MAX) {
        if (represented_accounting != NULL) represented_accounting->status = REPRESENTED_BACKEND_RESOURCE;
        goto failed;
    }
    provenance->bytes = allocate(bytes, 1);
    if (provenance->bytes == NULL) goto failed;
    uint8_t *cursor = provenance->bytes;
    memcpy(cursor, "P43P", 4); cursor += 4;
    put_u32(&cursor, 1); put_u32(&cursor, (uint32_t)count);
    for (size_t i = 0; i < count; ++i) {
        const ProvenanceRecord *record = &provenance->records[i];
        size_t path_length = strlen(record->path), symbol_length = strlen(record->symbol);
        *cursor++ = record->tag; *cursor++ = record->kind; *cursor++ = 0; *cursor++ = 0;
        put_u32(&cursor, (uint32_t)path_length); memcpy(cursor, record->path, path_length); cursor += path_length;
        put_u32(&cursor, (uint32_t)record->start); put_u32(&cursor, (uint32_t)record->end);
        put_u32(&cursor, (uint32_t)symbol_length); memcpy(cursor, record->symbol, symbol_length); cursor += symbol_length;
        put_u32(&cursor, (uint32_t)record->ordinal);
    }
    provenance->count = count;
    provenance->byte_count = bytes;
    return true;
failed:
    represented_provenance_free(provenance);
    return false;
}

typedef struct {
    BinaryenExpressionRef *items;
    size_t count, capacity;
} RepresentedNodes;

static bool represented_nodes_push(RepresentedNodes *nodes, BinaryenExpressionRef item) {
    if (item == NULL) return false;
    if (nodes->count == nodes->capacity) {
        size_t next = nodes->capacity == 0 ? 16 : nodes->capacity * 2;
        if (next < nodes->capacity || next > SIZE_MAX / sizeof *nodes->items) return false;
        BinaryenExpressionRef *items = grow(nodes->items, nodes->capacity, next,
            sizeof *items);
        if (items == NULL) return false;
        nodes->items = items; nodes->capacity = next;
    }
    nodes->items[nodes->count++] = item;
    return true;
}

/* Every source is captured before any destination is assigned.  This is used
 * both for authenticated CFG edges and the isolated backend test hook. */
static bool represented_parallel_assign(BinaryenModuleRef module, RepresentedNodes *nodes,
    BinaryenExpressionRef *sources, const BinaryenIndex *destinations,
    size_t count, size_t scratch_base) {
    for (size_t i = 0; i < count; ++i)
        if (!represented_nodes_push(nodes, BinaryenLocalSet(module,
                (BinaryenIndex)(scratch_base + i), sources[i]))) return false;
    for (size_t i = 0; i < count; ++i)
        if (!represented_nodes_push(nodes, BinaryenLocalSet(module, destinations[i],
                BinaryenLocalGet(module, (BinaryenIndex)(scratch_base + i),
                    BinaryenTypeInt64())))) return false;
    return true;
}

typedef struct {
    const SolWasmRepresentedBuildRequest *request;
    BinaryenModuleRef module;
    const SolMirMaterializedImage *image;
    const SolMirRuntimeSignature *signature;
    const RepresentedCallCatalogGraph *catalog;
    const RepresentedQualifiedIndex *qualified;
    bool calls_enabled;
    size_t image_id, parameter_count;
    size_t value_base, temporary_base, local_base, predicate_value_base, predicate_result;
    size_t predicate_value_count, snapshot_value_base, snapshot_count, staged_return;
    size_t temporary_init_base, local_init_base, local_hole_base, local_hole_count;
    size_t snapshot_init_base, scratch_base, predicate_pc, pc;
    const struct RepresentedLiteral *literals;
    size_t literal_count;
    const RepresentedProvenance *provenance;
} RepresentedFunction;

static bool represented_failure_record_index(const RepresentedProvenance *, size_t, size_t *);
static BinaryenExpressionRef mul_overflow(const RepresentedFunction *, BinaryenExpressionRef,
    BinaryenExpressionRef);

static bool represented_fixed_products_needed(const SolWasmRepresentedBuildRequest *request);
static bool represented_product_copy_needed(const SolWasmRepresentedBuildRequest *request,
    SolMirRecipeId recipe);
static bool represented_product_equal_needed(const SolWasmRepresentedBuildRequest *request,
    SolMirRecipeId recipe);
static bool represented_product_helper_name(char name[64], const char *kind,
    SolMirRecipeId recipe);
static bool represented_sum_copy_needed(const SolWasmRepresentedBuildRequest *request,
    SolMirRecipeId recipe);
static bool represented_sum_equal_needed(const SolWasmRepresentedBuildRequest *request,
    SolMirRecipeId recipe);
static bool represented_sum_helper_name(char name[64], const char *kind,
    SolMirRecipeId recipe);
static bool represented_aggregate_reaches(const SolMirConcreteProgram *concrete,
    SolMirRecipeId root, SolMirRecipeId wanted, size_t depth);
static bool represented_text_allocation_route(const SolWasmRepresentedBuildRequest *request,
    size_t instruction, size_t *supplemental);
static const SolMirOperationPatternExtraction *represented_pattern_extraction_for_instruction(
    const SolWasmRepresentedBuildRequest *request, size_t instruction);
static bool represented_propagation_plan(const SolWasmRepresentedBuildRequest *request,
    size_t image, size_t block, const SolMirOperationPropagationPlan **out);
static bool represented_propagation_pre_event(const SolWasmRepresentedBuildRequest *request,
    size_t image, size_t block, const SolMirOperationPropagationPlan *plan,
    size_t *event_out, size_t *site_out);
static bool represented_propagation_main_event(const SolWasmRepresentedBuildRequest *request,
    size_t image, size_t block, const SolMirOperationPropagationPlan *plan,
    const SolMirRuntimeCleanupTransition **value_out,
    const SolMirRuntimeCleanupTransition **residual_out);
static BinaryenExpressionRef represented_cleanup_probe_increment(BinaryenModuleRef,
    const char *);
static bool represented_cleanup_emit(const RepresentedFunction *,
    const SolMirRuntimeCleanupAction *, RepresentedNodes *);
static bool represented_cleanup_action(const SolWasmRepresentedBuildRequest *,
    const SolMirRuntimeCleanupAction *);
static bool represented_step_route(const SolWasmRepresentedBuildRequest *,
    SolMirRuntimeCleanupEventKind, size_t, size_t, size_t, size_t, size_t);

/* Static Text objects are real P2 text headers.  The handle names the header,
 * never its payload; byte zero is consequently reserved as the null handle.
 * Each CONST_TEXT owns a distinct header and payload: identical spellings are
 * intentionally not deduplicated because the physical static image is part of
 * the deterministic P4.3 resource census. */
typedef struct RepresentedLiteral {
    size_t instruction;
    const uint8_t *bytes;
    size_t length;
    uint32_t handle, data;
} RepresentedLiteral;

enum { P43_STATIC_BASE = 8, P43_FIXED_SCRATCH = 1024, P44_PANIC_DETAIL_BYTES = 192,
    P44_COPY_STEP_FAILURE = -1,
    P44_PANIC_DETAIL_MAX = P44_PANIC_DETAIL_BYTES - 1, P44_TRACE_OFFSET_IN_SCRATCH = 192,
    P44_TRACE_SLOT_BYTES = 12, P44_TRACE_CAPACITY = 64,
    P44_TRACE_BYTES = P44_TRACE_SLOT_BYTES * P44_TRACE_CAPACITY,
    P44_ENTRY_RESET_MAX_ITEMS = 7 + 2 + 2,
    P44_ENTRY_WRAPPER_MAX_ITEMS = P44_ENTRY_RESET_MAX_ITEMS + 1 + 4 + 1,
    P44_PACKET_RESET_PROBE_MAX_ITEMS = P44_ENTRY_RESET_MAX_ITEMS + 2 + 1 };

static bool represented_align8(size_t input, size_t *output) {
    if (input > SIZE_MAX - 7) return false;
    *output = (input + 7) & ~(size_t)7;
    return true;
}

/* The allocation-plan row intentionally says NONE for callable headers.  The
 * FUNCTION_VALUE instruction's authenticated supplemental event is therefore
 * the sole authority for this physical allocation. */
static const SolMirOperationCallablePlan *represented_callable_plan_for_instruction(
    const SolWasmRepresentedBuildRequest *request, size_t instruction, size_t *plan_id) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirConcreteProgram *concrete = owner->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    if (instruction >= owner->image_instruction_count || instruction >= m->instruction_count)
        return NULL;
    const SolMirRuntimeLoweredImageInstruction *row = &owner->image_instructions[instruction];
    if (row->plan >= owner->semantic_plan_count) return NULL;
    const SolMirRuntimeLoweredSemanticPlan *semantic = &owner->semantic_plans[row->plan];
    if (semantic->state != SOL_MIR_RUNTIME_LOWERED_PRESENT
        || semantic->arena != SOL_MIR_RUNTIME_LOWERED_SEMANTIC_CALLABLE
        || semantic->plan >= concrete->operations.callable_count) return NULL;
    const SolMirOperationCallablePlan *plan = &concrete->operations.callables[semantic->plan];
    if (m->instructions[instruction].kind != SOL_MIR_INST_FUNCTION_VALUE
        || plan->semantic_site >= m->semantic_site_count
        || m->semantic_sites[plan->semantic_site].producer_kind
            != SOL_MIR_MATERIALIZED_PRODUCER_INSTRUCTION
        || m->semantic_sites[plan->semantic_site].instruction != instruction) return NULL;
    if (plan_id != NULL) *plan_id = semantic->plan;
    return plan;
}

static bool represented_function_value_route(const SolWasmRepresentedBuildRequest *request,
    size_t instruction, const SolMirOperationCallablePlan **out_plan, size_t *out_table,
    size_t *out_internal) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirLinkage *linkage = &concrete->linkage;
    size_t plan_id = SOL_MIR_OPERATION_NONE;
    const SolMirOperationCallablePlan *plan = represented_callable_plan_for_instruction(request,
        instruction, &plan_id);
    if (plan == NULL || instruction >= m->instruction_count || plan->kind
            != SOL_MIR_CALLABLE_PRODUCER_EXACT_FUNCTION
        || plan->capture_kind != SOL_MIR_OPERATION_CAPTURE_NONE
        || plan->target_kind != SOL_MIR_MATERIALIZED_TARGET_INSTANCE
        || plan->target_instance >= m->image_count || plan->function_recipe >= concrete->layout.type_count
        || m->instructions[instruction].type != plan->function_recipe
        || !represented_unbound_function_recipe(concrete, plan->function_recipe)) return false;
    size_t table = SOL_MIR_LINKAGE_NONE, matches = 0;
    for (size_t i = 0; i < linkage->callable_value_count; ++i) {
        const SolMirLinkageCallableValue *value = &linkage->callable_values[i];
        if (value->callable_plan == plan_id) { table = value->table; ++matches; }
    }
    if (matches != 1 || table >= linkage->table_entry_count) return false;
    const SolMirLinkageTableEntry *entry = &linkage->table_entries[table];
    if (entry->target_kind != SOL_MIR_LINKAGE_TARGET_INTERNAL
        || entry->internal >= linkage->callable_count
        || linkage->callables[entry->internal].instance != plan->target_instance
        || entry->host != SOL_MIR_LINKAGE_NONE
        || !represented_text_allocation_route(request, instruction, &(size_t){0})) return false;
    if (out_plan != NULL) *out_plan = plan;
    if (out_table != NULL) *out_table = table;
    if (out_internal != NULL) *out_internal = entry->internal;
    return true;
}

static int represented_literal_order(const void *left, const void *right) {
    const RepresentedLiteral *a = left, *b = right;
    size_t shared = a->length < b->length ? a->length : b->length;
    int order = shared == 0 ? 0 : memcmp(a->bytes, b->bytes, shared);
    if (order != 0) return order;
    if (a->length != b->length) return a->length < b->length ? -1 : 1;
    return a->instruction < b->instruction ? -1 : a->instruction > b->instruction;
}

static void represented_put_u32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)value; bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16); bytes[3] = (uint8_t)(value >> 24);
}

/* The owner has already authenticated literal slices.  This builds a separate
 * sorted physical image and intentionally omits materializer-only trailing
 * NULs: Wasm Text payloads are owned bytes, not C strings. */
static bool represented_static_literals(const SolWasmRepresentedBuildRequest *request,
    RepresentedLiteral **out_literals, size_t *out_count, uint8_t **out_data,
    size_t *out_size, uint32_t *out_heap) {
    const SolMirMaterialization *m = &request->program->conventions->concrete->materialization;
    size_t count = 0, payload = 0;
    for (size_t i = 0; i < m->instruction_count; ++i) {
        const SolMirMaterializedInstruction *item = &m->instructions[i];
        if (item->kind != SOL_MIR_INST_CONST_TEXT) continue;
        if (!represented_add(count, 1, &count) || !represented_add(payload, item->text.count, &payload))
            return false;
    }
    RepresentedLiteral *literals = count == 0 ? NULL : allocate(count, sizeof *literals);
    if (count != 0 && literals == NULL) return false;
    size_t at = 0;
    for (size_t i = 0; i < m->instruction_count; ++i) {
        const SolMirMaterializedInstruction *item = &m->instructions[i];
        if (item->kind != SOL_MIR_INST_CONST_TEXT) continue;
        if (item->text.offset > m->literal_byte_count
            || item->text.count > m->literal_byte_count - item->text.offset) {
            deallocate(literals); return false;
        }
        literals[at++] = (RepresentedLiteral){i,
            (const uint8_t *)m->literal_bytes + item->text.offset, item->text.count, 0, 0};
    }
    qsort(literals, count, sizeof *literals, represented_literal_order);
    size_t headers, payload_start, total, heap;
    if (count > SIZE_MAX / 8 || !represented_add(count * 8, 0, &headers)
        || !represented_add(P43_STATIC_BASE, headers, &payload_start)
        || !represented_add(payload_start, payload, &total)
        || !represented_add(total, P43_FIXED_SCRATCH, &heap)
        || !represented_align8(heap, &heap) || total > UINT32_MAX || heap > UINT32_MAX) {
        deallocate(literals); return false;
    }
    uint8_t *data = total == P43_STATIC_BASE ? NULL : allocate(total - P43_STATIC_BASE, 1);
    if (total != P43_STATIC_BASE && data == NULL) { deallocate(literals); return false; }
    size_t cursor = payload_start;
    for (size_t i = 0; i < count; ++i) {
        size_t handle = P43_STATIC_BASE + i * 8;
        if (handle > UINT32_MAX || cursor > UINT32_MAX || literals[i].length > UINT32_MAX) {
            deallocate(data); deallocate(literals); return false;
        }
        literals[i].handle = (uint32_t)handle;
        literals[i].data = literals[i].length == 0 ? 0 : (uint32_t)cursor;
        represented_put_u32(data + i * 8, literals[i].data);
        represented_put_u32(data + i * 8 + 4, (uint32_t)literals[i].length);
        if (literals[i].length != 0) {
            memcpy(data + cursor - P43_STATIC_BASE, literals[i].bytes, literals[i].length);
            cursor += literals[i].length;
        }
    }
    *out_literals = literals; *out_count = count; *out_data = data;
    *out_size = total - P43_STATIC_BASE; *out_heap = (uint32_t)heap;
    return true;
}

static const RepresentedLiteral *represented_literal_for(const RepresentedFunction *function,
    size_t instruction) {
    for (size_t i = 0; i < function->literal_count; ++i)
        if (function->literals[i].instruction == instruction) return &function->literals[i];
    return NULL;
}

static bool represented_panic_detail_needed(const SolWasmRepresentedBuildRequest *request) {
    const SolMirRuntimeConventions *conventions = request->program->conventions;
    for (size_t i = 0; i < conventions->failure_site_count; ++i)
        if (conventions->failure_sites[i].origin_kind
            == SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_PANIC) return true;
    return false;
}

static bool recipe_represented(const SolMirConcreteProgram *concrete,
    SolMirRecipeId recipe, bool terminal) {
    const SolMirRepresentation *representation = &concrete->representation;
    if (recipe >= representation->recipe_count) return false;
    const SolMirRecipe *item = &representation->recipes[recipe];
    switch (item->kind) {
        case SOL_MIR_RECIPE_INT64:
        case SOL_MIR_RECIPE_BOOL:
            return item->storage == SOL_MIR_STORAGE_SCALAR;
        case SOL_MIR_RECIPE_TEXT:
            return item->storage == SOL_MIR_STORAGE_TEXT_HANDLE;
        case SOL_MIR_RECIPE_UNIT:
            return item->storage == SOL_MIR_STORAGE_NONE;
        case SOL_MIR_RECIPE_NEVER:
            return terminal;
        case SOL_MIR_RECIPE_REFINED:
            return represented_leaf_refined_recipe(concrete, recipe, NULL);
        default:
            return false;
    }
}

static bool represented_propagation_callable_recipe(const SolMirConcreteProgram *concrete,
    size_t image, SolMirRecipeId recipe, bool terminal) {
    if (recipe_represented(concrete, recipe, terminal)) return true;
    const SolMirMaterialization *m = &concrete->materialization;
    if (image >= m->image_count || !represented_sum_recipe(concrete, recipe)) return false;
    const SolMirMaterializedImage *owner = &m->images[image];
    for (size_t i = 0; i < owner->blocks.count; ++i)
        if (m->blocks[owner->blocks.offset + i].terminator.kind == SOL_MIR_TERM_PROPAGATE)
            return true;
    return false;
}

typedef struct {
    SolMirRecipeId recipe;
    SolMirRecipeId payload_recipe;
    SolMirRecipeId failure_recipe;
    size_t success_variant;
    size_t failure_variant;
    size_t success_field;
    size_t failure_field;
    uint32_t success_tag;
    uint32_t failure_tag;
} RepresentedQualifiedResult;

typedef struct {
    RepresentedQualifiedResult result;
    SolMirRecipeId recipe;
    size_t clause_count;
    bool certified;
} RepresentedQualifiedImage;

enum {
    REPRESENTED_QUALIFIED_CALLABLE_ENTRY = 1u,
    REPRESENTED_QUALIFIED_CALLABLE_DIRECT_TARGET = 2u,
    REPRESENTED_QUALIFIED_CALLABLE_CALLBACK_INOUT_TARGET = 4u,
    REPRESENTED_QUALIFIED_CALLABLE_METHOD_TARGET = 8u,
};

struct RepresentedQualifiedIndex {
    RepresentedQualifiedImage *images;
    size_t *plan_by_body;
    unsigned char *callable_flags;
    size_t image_count, body_count, callable_count, qualified_clause_count,
        refinement_plan_count;
};

typedef enum {
    REPRESENTED_QUALIFIED_INDEX_VALID,
    REPRESENTED_QUALIFIED_INDEX_INVALID,
    REPRESENTED_QUALIFIED_INDEX_RESOURCE,
    REPRESENTED_QUALIFIED_INDEX_ALLOCATION,
} RepresentedQualifiedIndexResult;

static bool represented_layout_range(uint64_t offset, uint64_t size, uint64_t object_size) {
    return offset <= object_size && size <= object_size - offset;
}

/* C2b2's source-built envelope is intentionally exact.  Variant and recipe
 * identities are discovered from the authenticated owners; only the semantic
 * Result ordinals/tags and wasm32 physical contract are fixed here. */
static bool represented_qualified_result(const SolMirConcreteProgram *concrete,
    SolMirRecipeId recipe, RepresentedQualifiedResult *result) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    ++represented_test_qualified_certifications;
#endif
    const SolMirRepresentation *r = &concrete->representation;
    const SolMirLayout *layout = &concrete->layout;
    const SolIr *ir = concrete->program.ir;
    if (result == NULL || recipe >= r->recipe_count || recipe >= layout->type_count)
        return false;
    const SolMirRecipe *sum = &r->recipes[recipe];
    const SolMirTypeLayout *sum_layout = &layout->types[recipe];
    if (sum->kind != SOL_MIR_RECIPE_RESULT || sum->concrete_definition != SOL_IR_NONE
        || sum->storage != SOL_MIR_STORAGE_AGGREGATE_VALUE || !sum->inhabited
        || sum->zero_sized || sum->copy_kind != SOL_MIR_COPY_AGGREGATE
        || sum->drop_kind != SOL_MIR_DROP_AGGREGATE || sum->variants.count != 2
        || sum->variants.offset > r->variant_count
        || sum->variants.count > r->variant_count - sum->variants.offset
        || sum_layout->recipe != recipe || sum_layout->object_kind != SOL_MIR_LAYOUT_OBJECT_SUM
        || !sum_layout->has_object || sum_layout->value_size != 4
        || sum_layout->value_alignment != 4 || sum_layout->object_size == 0
        || sum_layout->object_size > UINT32_MAX || sum_layout->object_alignment == 0
        || sum_layout->object_alignment > 8 || sum_layout->tag_offset != 0
        || sum_layout->tag_size != 4 || sum_layout->payload_offset > UINT32_MAX
        || !represented_layout_range(sum_layout->tag_offset, sum_layout->tag_size,
            sum_layout->object_size)
        || !represented_layout_range(sum_layout->payload_offset, sum_layout->payload_size,
            sum_layout->object_size)) return false;
    size_t variants[2] = {SOL_MIR_RECIPE_NONE, SOL_MIR_RECIPE_NONE};
    for (size_t i = 0; i < sum->variants.count; ++i) {
        size_t id = sum->variants.offset + i;
        const SolMirRecipeVariant *variant = &r->variants[id];
        if (variant->source_variant != SOL_IR_NONE || variant->ordinal > 1
            || variant->semantic_tag != variant->ordinal || variants[variant->ordinal]
                != SOL_MIR_RECIPE_NONE || variant->fields.count != 1
            || variant->fields.offset >= r->field_count || id >= layout->variant_count)
            return false;
        variants[variant->ordinal] = id;
    }
    if (variants[0] == SOL_MIR_RECIPE_NONE || variants[1] == SOL_MIR_RECIPE_NONE) return false;
    const SolMirRecipeVariant *success = &r->variants[variants[0]];
    const SolMirRecipeVariant *failure = &r->variants[variants[1]];
    size_t success_field = success->fields.offset, failure_field = failure->fields.offset;
    if (success_field >= layout->field_count || failure_field >= layout->field_count) return false;
    const SolMirRecipeField *success_recipe_field = &r->fields[success_field];
    const SolMirRecipeField *failure_recipe_field = &r->fields[failure_field];
    const SolMirFieldLayout *success_layout = &layout->fields[success_field];
    const SolMirFieldLayout *failure_layout = &layout->fields[failure_field];
    SolMirRecipeId payload = success_recipe_field->type;
    SolMirRecipeId error = failure_recipe_field->type;
    if (payload >= r->recipe_count || payload >= layout->type_count
        || error >= r->recipe_count || error >= layout->type_count
        || (r->recipes[payload].kind != SOL_MIR_RECIPE_INT64
            && r->recipes[payload].kind != SOL_MIR_RECIPE_BOOL)
        || r->recipes[payload].storage != SOL_MIR_STORAGE_SCALAR
        || !r->recipes[payload].is_copy || r->recipes[error].kind != SOL_MIR_RECIPE_ENUM
        || r->recipes[error].storage != SOL_MIR_STORAGE_AGGREGATE_VALUE
        || r->recipes[error].copy_kind != SOL_MIR_COPY_AGGREGATE
        || r->recipes[error].drop_kind != SOL_MIR_DROP_AGGREGATE
        || !r->recipes[error].inhabited || r->recipes[error].variants.count != 1
        || r->recipes[error].variants.offset >= r->variant_count
        || r->recipes[error].fields.count != 0 || r->recipes[error].concrete_definition >= ir->definition_count)
        return false;
    const SolIrDefinition *failure_definition = &ir->definitions[
        r->recipes[error].concrete_definition];
    size_t unit_variant = r->recipes[error].variants.offset;
    const SolMirRecipeVariant *unit = &r->variants[unit_variant];
    if (failure_definition->kind != SOL_IR_DEFINITION_ENUM || failure_definition->open
        || failure_definition->variants.count != 1 || unit->source_variant
            != failure_definition->variants.offset || unit->source_variant >= ir->variant_count
        || ir->variants[unit->source_variant].owner != r->recipes[error].concrete_definition
        || ir->variants[unit->source_variant].fields.count != 0 || unit->ordinal != 0
        || unit->semantic_tag != 0 || unit->fields.count != 0 || unit_variant >= layout->variant_count)
        return false;
    const SolMirTypeLayout *error_layout = &layout->types[error];
    const SolMirVariantLayout *unit_layout = &layout->variants[unit_variant];
    if (error_layout->recipe != error || error_layout->object_kind != SOL_MIR_LAYOUT_OBJECT_SUM
        || !error_layout->has_object || error_layout->value_size != 4
        || error_layout->value_alignment != 4 || error_layout->object_size != 4
        || error_layout->object_alignment != 4 || error_layout->tag_offset != 0
        || error_layout->tag_size != 4 || error_layout->payload_offset != 4
        || error_layout->payload_size != 0 || unit_layout->variant != unit_variant
        || unit_layout->owner_recipe != error || unit_layout->tag != unit->semantic_tag
        || !unit_layout->inhabited || !unit_layout->has_payload_storage
        || unit_layout->payload_size != 0 || unit_layout->payload_alignment != 1
        || unit_layout->tail_padding != 0) return false;
    const SolMirVariantLayout *success_variant_layout = &layout->variants[variants[0]];
    const SolMirVariantLayout *failure_variant_layout = &layout->variants[variants[1]];
    uint64_t payload_size = r->recipes[payload].kind == SOL_MIR_RECIPE_INT64 ? 8 : 1;
    uint64_t payload_alignment = r->recipes[payload].kind == SOL_MIR_RECIPE_INT64 ? 8 : 1;
    uint64_t sum_payload_size = r->recipes[payload].kind == SOL_MIR_RECIPE_INT64 ? 8 : 4;
    uint64_t sum_alignment = r->recipes[payload].kind == SOL_MIR_RECIPE_INT64 ? 8 : 4;
    const SolMirTypeLayout *payload_layout = &layout->types[payload];
    if (payload_layout->recipe != payload || payload_layout->object_kind != SOL_MIR_LAYOUT_OBJECT_NONE
        || payload_layout->has_object || payload_layout->value_size != payload_size
        || payload_layout->value_alignment != payload_alignment
        || sum_layout->object_alignment != sum_alignment
        || sum_layout->payload_offset != sum_alignment || sum_layout->payload_size != sum_payload_size
        || sum_layout->object_size != sum_alignment + sum_payload_size
        || sum_layout->tail_padding != 0
        || success_recipe_field->source_field != SOL_IR_NONE || success_recipe_field->ordinal != 0
        || failure_recipe_field->source_field != SOL_IR_NONE || failure_recipe_field->ordinal != 0
        || success_layout->field != success_field || success_layout->owner_recipe != recipe
        || success_layout->variant != variants[0] || !success_layout->has_storage
        || success_layout->offset != sum_layout->payload_offset
        || success_layout->size != payload_size || success_layout->alignment != payload_alignment
        || !represented_layout_range(success_layout->offset, success_layout->size,
            sum_layout->object_size)
        || failure_layout->field != failure_field || failure_layout->owner_recipe != recipe
        || failure_layout->variant != variants[1] || !failure_layout->has_storage
        || failure_layout->offset != sum_layout->payload_offset || failure_layout->size != 4
        || failure_layout->alignment != 4 || !represented_layout_range(failure_layout->offset,
            failure_layout->size, sum_layout->object_size)
        || success_variant_layout->variant != variants[0]
        || success_variant_layout->owner_recipe != recipe || !success_variant_layout->inhabited
        || !success_variant_layout->has_payload_storage
        || success_variant_layout->tag != success->semantic_tag
        || success_variant_layout->payload_size != payload_size
        || success_variant_layout->payload_alignment != payload_alignment
        || failure_variant_layout->variant != variants[1]
        || failure_variant_layout->owner_recipe != recipe || !failure_variant_layout->inhabited
        || !failure_variant_layout->has_payload_storage
        || failure_variant_layout->tag != failure->semantic_tag
        || failure_variant_layout->payload_size != 4
        || failure_variant_layout->payload_alignment != 4) return false;
    *result = (RepresentedQualifiedResult){recipe, payload, error, variants[0], variants[1],
        success_field, failure_field, success_variant_layout->tag, failure_variant_layout->tag};
    return true;
}

static void represented_qualified_index_free(RepresentedQualifiedIndex *index) {
    if (index == NULL) return;
    deallocate(index->images);
    deallocate(index->plan_by_body);
    deallocate(index->callable_flags);
    memset(index, 0, sizeof *index);
}

static RepresentedQualifiedIndexResult represented_qualified_index_allocation_result(void) {
    return represented_accounting != NULL
        && represented_accounting->status == REPRESENTED_BACKEND_RESOURCE
        ? REPRESENTED_QUALIFIED_INDEX_RESOURCE : REPRESENTED_QUALIFIED_INDEX_ALLOCATION;
}

/* Build one request-local certificate after accounting is active.  The first
 * pass counts qualified clauses and refinement plans without allocating.
 * Qualified-only closures preserve their historical images/body/callable
 * allocation order. A refinement-only closure owns just the body table, and a
 * closure containing neither feature allocates nothing. */
static RepresentedQualifiedIndexResult represented_qualified_index_build(
    const SolWasmRepresentedBuildRequest *request, RepresentedQualifiedIndex *index) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirOperations *o = &concrete->operations;
    const SolMirLinkage *linkage = &concrete->linkage;
    const SolMirRuntimeConventions *conventions = request->program->conventions;
    memset(index, 0, sizeof *index);
    for (size_t i = 0; i < o->predicate_body_count; ++i) {
        const SolMirPredicateBody *body = &o->predicate_bodies[i];
        if (body->owner_kind == SOL_MIR_PREDICATE_OWNER_INSTANCE
            && body->phase == SOL_CONTRACT_ENSURES
            && (body->outcome == SOL_CONTRACT_OUTCOME_SUCCESS
                || body->outcome == SOL_CONTRACT_OUTCOME_FAILURE)) {
            if (body->instance >= m->image_count || index->qualified_clause_count == SIZE_MAX)
                return REPRESENTED_QUALIFIED_INDEX_INVALID;
            ++index->qualified_clause_count;
        }
    }
    for (size_t i = 0; i < o->predicate_count; ++i)
        if (o->predicates[i].kind == SOL_MIR_OPERATION_PREDICATE_REFINEMENT) {
            if (index->refinement_plan_count == SIZE_MAX)
                return REPRESENTED_QUALIFIED_INDEX_INVALID;
            ++index->refinement_plan_count;
        }
    if (index->qualified_clause_count == 0 && index->refinement_plan_count == 0)
        return REPRESENTED_QUALIFIED_INDEX_VALID;
    index->image_count = m->image_count;
    index->body_count = o->predicate_body_count;
    index->callable_count = linkage->callable_count;
    if (index->qualified_clause_count != 0)
        index->images = m->image_count == 0 ? NULL
            : allocate(m->image_count, sizeof *index->images);
    index->plan_by_body = o->predicate_body_count == 0 ? NULL
        : allocate(o->predicate_body_count, sizeof *index->plan_by_body);
    if (index->qualified_clause_count != 0)
        index->callable_flags = linkage->callable_count == 0 ? NULL
            : allocate(linkage->callable_count, sizeof *index->callable_flags);
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (o->predicate_body_count != 0 && index->plan_by_body != NULL) {
        if (represented_test_refinement_index_stats.body_table_allocations != SIZE_MAX)
            ++represented_test_refinement_index_stats.body_table_allocations;
        represented_test_refinement_index_stats.body_table_bytes =
            o->predicate_body_count > SIZE_MAX / sizeof *index->plan_by_body ? SIZE_MAX
            : o->predicate_body_count * sizeof *index->plan_by_body;
    }
#endif
    if ((index->qualified_clause_count != 0 && m->image_count != 0 && index->images == NULL)
        || (o->predicate_body_count != 0 && index->plan_by_body == NULL)
        || (index->qualified_clause_count != 0 && linkage->callable_count != 0
            && index->callable_flags == NULL)) {
        RepresentedQualifiedIndexResult result = represented_qualified_index_allocation_result();
        represented_qualified_index_free(index);
        return result;
    }
    for (size_t i = 0; i < o->predicate_body_count; ++i)
        index->plan_by_body[i] = SOL_MIR_OPERATION_NONE;
    for (size_t i = 0; i < o->predicate_body_count; ++i) {
        const SolMirPredicateBody *body = &o->predicate_bodies[i];
        if (body->owner_kind == SOL_MIR_PREDICATE_OWNER_INSTANCE
            && body->phase == SOL_CONTRACT_ENSURES
            && (body->outcome == SOL_CONTRACT_OUTCOME_SUCCESS
                || body->outcome == SOL_CONTRACT_OUTCOME_FAILURE)) {
            if (index->images == NULL || body->instance >= index->image_count
                || index->images[body->instance].clause_count == SIZE_MAX) goto invalid;
            ++index->images[body->instance].clause_count;
        }
    }
    for (size_t i = 0; i < o->predicate_count; ++i) {
        const SolMirOperationPredicatePlan *plan = &o->predicates[i];
        if (plan->kind != SOL_MIR_OPERATION_PREDICATE_CONTRACT
            && plan->kind != SOL_MIR_OPERATION_PREDICATE_REFINEMENT) continue;
        if (plan->body >= o->predicate_body_count) goto invalid;
        if (index->plan_by_body[plan->body] != SOL_MIR_OPERATION_NONE) goto invalid;
        index->plan_by_body[plan->body] = i;
        if (plan->kind == SOL_MIR_OPERATION_PREDICATE_REFINEMENT) continue;
        const SolMirPredicateBody *body = &o->predicate_bodies[plan->body];
        bool qualified = body->owner_kind == SOL_MIR_PREDICATE_OWNER_INSTANCE
            && body->phase == SOL_CONTRACT_ENSURES
            && (body->outcome == SOL_CONTRACT_OUTCOME_SUCCESS
                || body->outcome == SOL_CONTRACT_OUTCOME_FAILURE);
        if (!qualified) continue;
        if (body->instance >= index->image_count || plan->image != body->instance
            || plan->contract_phase != body->phase || plan->contract_outcome != body->outcome)
            goto invalid;
        RepresentedQualifiedImage *row = &index->images[body->instance];
        if (!row->certified) {
            if (!represented_qualified_result(concrete, plan->result_recipe, &row->result))
                goto invalid;
            row->recipe = plan->result_recipe;
            const SolMirMaterializedImage *image = &m->images[body->instance];
            if (image->blocks.offset > m->block_count
                || image->blocks.count > m->block_count - image->blocks.offset) goto invalid;
            for (size_t b = 0; b < image->blocks.count; ++b)
                if (m->blocks[image->blocks.offset + b].terminator.kind
                        == SOL_MIR_TERM_PROPAGATE) goto invalid;
            row->certified = true;
        } else if (row->recipe != plan->result_recipe) goto invalid;
    }
    for (size_t i = 0; i < o->predicate_body_count; ++i) {
        const SolMirPredicateBody *body = &o->predicate_bodies[i];
        if (body->owner_kind == SOL_MIR_PREDICATE_OWNER_INSTANCE
            && body->phase == SOL_CONTRACT_ENSURES
            && (body->outcome == SOL_CONTRACT_OUTCOME_SUCCESS
                || body->outcome == SOL_CONTRACT_OUTCOME_FAILURE)
            && index->plan_by_body[i] == SOL_MIR_OPERATION_NONE) goto invalid;
    }
    if (index->qualified_clause_count != 0) {
        for (size_t i = 0; i < index->image_count; ++i)
            if ((index->images[i].clause_count != 0) != index->images[i].certified) goto invalid;
        for (size_t i = 0; i < conventions->entry_count; ++i) {
            if (conventions->entries[i].callable >= index->callable_count) goto invalid;
            index->callable_flags[conventions->entries[i].callable]
                |= REPRESENTED_QUALIFIED_CALLABLE_ENTRY;
        }
    }
    return REPRESENTED_QUALIFIED_INDEX_VALID;
invalid:
    represented_qualified_index_free(index);
    return REPRESENTED_QUALIFIED_INDEX_INVALID;
}

static const RepresentedQualifiedImage *represented_qualified_image(
    const RepresentedQualifiedIndex *index, size_t image, SolMirRecipeId recipe) {
    if (index == NULL || index->images == NULL || image >= index->image_count) return NULL;
    const RepresentedQualifiedImage *row = &index->images[image];
    return row->certified && row->clause_count != 0 && row->recipe == recipe ? row : NULL;
}

static const SolMirOperationPredicatePlan *represented_qualified_plan_for_body(
    const SolWasmRepresentedBuildRequest *request, const RepresentedQualifiedIndex *index,
    size_t body) {
    const SolMirOperations *o = &request->program->conventions->concrete->operations;
    if (index == NULL || index->plan_by_body == NULL || body >= index->body_count) return NULL;
    size_t plan = index->plan_by_body[body];
    return plan < o->predicate_count ? &o->predicates[plan] : NULL;
}

/* Refinement self is latency-sensitive and never falls back to an arena scan.
 * The index is the sole request-local body->plan authority for this route. */
static const SolMirOperationPredicatePlan *represented_refinement_plan_for_body(
    const SolWasmRepresentedBuildRequest *request, const RepresentedQualifiedIndex *index,
    size_t image, size_t body, size_t *plan_id) {
    const SolMirOperations *o = &request->program->conventions->concrete->operations;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (represented_test_refinement_index_stats.indexed_lookups != SIZE_MAX)
        ++represented_test_refinement_index_stats.indexed_lookups;
#endif
    if (index == NULL || index->plan_by_body == NULL || body >= index->body_count) return NULL;
    size_t id = index->plan_by_body[body];
    if (id >= o->predicate_count) return NULL;
    const SolMirOperationPredicatePlan *plan = &o->predicates[id];
    if (plan->kind != SOL_MIR_OPERATION_PREDICATE_REFINEMENT || plan->image != image
        || plan->body != body) return NULL;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (represented_test_refinement_index_stats.indexed_matches != SIZE_MAX)
        ++represented_test_refinement_index_stats.indexed_matches;
#endif
    if (plan_id != NULL) *plan_id = id;
    return plan;
}

static const SolMirOperationPredicatePlan *represented_plan_for_body(
    const SolWasmRepresentedBuildRequest *request, const RepresentedQualifiedIndex *index,
    size_t image, size_t body) {
    const SolMirOperations *o = &request->program->conventions->concrete->operations;
    if (index != NULL && index->plan_by_body != NULL) {
        const SolMirOperationPredicatePlan *plan = represented_qualified_plan_for_body(request,
            index, body);
        return plan != NULL && plan->image == image ? plan : NULL;
    }
    const SolMirOperationPredicatePlan *found = NULL;
    for (size_t i = 0; i < o->predicate_count; ++i) {
        const SolMirOperationPredicatePlan *candidate = &o->predicates[i];
        if (candidate->kind != SOL_MIR_OPERATION_PREDICATE_CONTRACT
            || candidate->image != image || candidate->body != body) continue;
        if (found != NULL) return NULL;
        found = candidate;
    }
    return found;
}

static size_t local_index(const RepresentedFunction *function,
    SolMirMaterializedLocalId local) {
    if (local < function->image->locals.offset
        || local - function->image->locals.offset >= function->image->locals.count)
        return SIZE_MAX;
    return function->local_base + local - function->image->locals.offset;
}

static size_t temporary_index(const RepresentedFunction *function,
    SolMirMaterializedTemporaryId temporary) {
    if (temporary < function->image->temporaries.offset
        || temporary - function->image->temporaries.offset >= function->image->temporaries.count)
        return SIZE_MAX;
    return function->temporary_base + temporary - function->image->temporaries.offset;
}

static size_t value_index(const RepresentedFunction *function, SolMirMaterializedValueId value) {
    if (value < function->image->values.offset
        || value - function->image->values.offset >= function->image->values.count)
        return SIZE_MAX;
    return function->value_base + value - function->image->values.offset;
}

static size_t temporary_init_index(const RepresentedFunction *function,
    SolMirMaterializedTemporaryId temporary) {
    size_t index = temporary_index(function, temporary);
    return index == SIZE_MAX ? SIZE_MAX : function->temporary_init_base
        + index - function->temporary_base;
}

static size_t local_init_index(const RepresentedFunction *function,
    SolMirMaterializedLocalId local) {
    size_t index = local_index(function, local);
    return index == SIZE_MAX ? SIZE_MAX : function->local_init_base + index - function->local_base;
}

static size_t snapshot_value_index(const RepresentedFunction *function, size_t slot) {
    return slot < function->snapshot_count ? function->snapshot_value_base + slot : SIZE_MAX;
}

static size_t snapshot_init_index(const RepresentedFunction *function, size_t slot) {
    return slot < function->snapshot_count ? function->snapshot_init_base + slot : SIZE_MAX;
}

static bool represented_callable_product_local(const RepresentedFunction *function,
    SolMirMaterializedLocalId local) {
    const SolMirConcreteProgram *concrete = function->request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    return local_index(function, local) != SIZE_MAX && local < m->local_count
        && m->locals[local].type < concrete->layout.type_count
        && represented_callable_product_recipe(concrete,
            concrete->layout.types[m->locals[local].type].recipe);
}

static size_t local_hole_index(const RepresentedFunction *function,
    SolMirMaterializedLocalId local) {
    if (function->local_hole_count == 0 || !represented_callable_product_local(function, local))
        return SIZE_MAX;
    size_t ordinal = 0;
    for (size_t i = function->image->locals.offset; i < local; ++i)
        if (represented_callable_product_local(function, i)) ++ordinal;
    return ordinal < function->local_hole_count ? function->local_hole_base + ordinal : SIZE_MAX;
}

static bool represented_same_projection(const SolMirMaterialization *, size_t, size_t);
static bool represented_callable_field_repair(const SolWasmRepresentedBuildRequest *,
    const SolMirMaterializedImage *, size_t, const SolMirFieldLayout **);
static bool represented_callable_product_whole_transfer(const SolWasmRepresentedBuildRequest *,
    const SolMirMaterializedImage *, size_t);
static bool represented_projected_pre_store_cleanup_action(const SolWasmRepresentedBuildRequest *,
    const SolMirRuntimeCleanupAction *);

typedef enum {
    REPRESENTED_CLEANUP_MARKER_INVALID,
    REPRESENTED_CLEANUP_MARKER_EVENTLESS,
    REPRESENTED_CLEANUP_MARKER_ACTION,
} RepresentedCleanupMarkerRoute;

/* Select the sole action owned by an executable cleanup marker.  This is a
 * deliberately local join through that marker's P3.6 row: neither an equal
 * action elsewhere in the cleanup arena nor a CFG successor is evidence. */
static const SolMirRuntimeCleanupAction *represented_cleanup_instruction_action(
    const RepresentedFunction *function, size_t instruction) {
    const SolMirRuntimeLoweredProgram *owner = function->request->program;
    const SolMirMaterialization *m = &owner->conventions->concrete->materialization;
    const SolMirRuntimeCleanup *cleanup = owner->cleanup;
    if (instruction >= m->instruction_count || instruction >= owner->image_instruction_count)
        return NULL;
    const SolMirMaterializedInstruction *item = &m->instructions[instruction];
    const SolMirRuntimeLoweredImageInstruction *row = &owner->image_instructions[instruction];
    size_t target = SOL_MIR_RUNTIME_NONE;
    SolMirRecipeId recipe = SOL_MIR_RECIPE_NONE;
    SolMirRuntimeCleanupActionKind kind;
    bool needs_drop_path = false;
    if (item->kind == SOL_MIR_INST_TEMPORARY_DROP) {
        if (item->temporary >= m->temporary_count) return NULL;
        kind = SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_TEMPORARY;
        target = item->temporary; recipe = m->temporaries[target].type;
    } else if (item->kind == SOL_MIR_INST_DROP_IF_INITIALIZED) {
        if (item->local >= m->local_count)
            return NULL;
        kind = (m->locals[item->local].kind == SOL_MIR_MATERIALIZED_LOCAL_PARAMETER
                || m->locals[item->local].kind == SOL_MIR_MATERIALIZED_LOCAL_RECEIVER)
            ? SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER
            : SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE;
        for (size_t i = function->image->places.offset;
             i < function->image->places.offset + function->image->places.count; ++i) {
            if (i >= m->place_count) return NULL;
            const SolMirMaterializedPlace *place = &m->places[i];
            if (place->local != item->local || place->projections.count != 0) continue;
            if (target == SOL_MIR_RUNTIME_NONE) target = i;
        }
        if (kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER) target = item->local;
        recipe = m->locals[item->local].type;
        needs_drop_path = true;
    } else if (item->kind == SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED) {
        if (item->place >= m->place_count) return NULL;
        kind = SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE;
        target = item->place;
        recipe = m->places[target].final_type;
        needs_drop_path = true;
    } else if (item->kind == SOL_MIR_INST_SCOPE_EXIT) {
        kind = SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_SCOPE;
        target = instruction; recipe = SOL_MIR_RECIPE_NONE;
    } else if (item->kind == SOL_MIR_INST_REGION_EXIT) {
        kind = SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_REGION;
        target = item->source_statement; recipe = SOL_MIR_RECIPE_NONE;
    } else return NULL;
    if (target == SOL_MIR_RUNTIME_NONE || row->state != SOL_MIR_RUNTIME_LOWERED_PRESENT
        || row->instruction != instruction || row->image != function->image_id
        || row->block != m->instructions[instruction].block || row->kind != item->kind
        || row->runtime_class != SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL
        || row->plan_family != SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP
        || row->cleanup_event >= cleanup->event_count) return NULL;
    const SolMirRuntimeCleanupEvent *event = &cleanup->events[row->cleanup_event];
    if (event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION
        || event->phase != SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
        || event->origin != SOL_MIR_RUNTIME_CLEANUP_ORIGIN_EXPLICIT
        || event->owner != row->image || event->block != row->block
        || event->operation != instruction || event->semantic_site != SOL_MIR_RUNTIME_NONE
        || event->inherited_failure_site != SOL_MIR_RUNTIME_NONE
        || event->supplemental_site != SOL_MIR_RUNTIME_NONE
        || event->producer != SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL
        || event->captures_failure_detail
        || event->capture_detail_kind != SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE
        || event->transitions.count != 1
        || event->transitions.offset > cleanup->transition_count
        || event->actions.offset > cleanup->action_count
        || event->actions.count != 1
        || event->actions.count > cleanup->action_count - event->actions.offset
        || event->transitions.count > cleanup->transition_count - event->transitions.offset)
        return NULL;
    const SolMirRuntimeCleanupTransition *transition =
        &cleanup->transitions[event->transitions.offset];
    if (transition->event != row->cleanup_event
        || transition->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL
        || transition->edge_role != SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO
        || transition->continuation != SOL_MIR_RUNTIME_NONE
        || transition->failure_source != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE
        || transition->failure_site != SOL_MIR_RUNTIME_NONE || transition->failure_mask != 0
        || transition->actions.count != 1 || transition->actions.offset != event->actions.offset
        || transition->actions.offset > cleanup->action_count
        || transition->actions.count > cleanup->action_count - transition->actions.offset)
        return NULL;
    const SolMirRuntimeCleanupAction *action = &cleanup->actions[transition->actions.offset];
    if (action->kind != kind || action->target != target || action->recipe != recipe
        || (action->flags & ~(unsigned)SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED) != 0)
        return NULL;
    if (!needs_drop_path) {
        if (action->flags != 0 || action->drop_path != SOL_MIR_RUNTIME_NONE) return NULL;
    } else {
        if (action->drop_path >= cleanup->drop_path_count) return NULL;
    }
    return action;
}

static bool represented_callable_field_move(const RepresentedFunction *function, size_t instruction) {
    const SolMirConcreteProgram *concrete = function->request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    if (instruction >= m->instruction_count || m->instructions[instruction].kind != SOL_MIR_INST_LOAD_MOVE
        || m->instructions[instruction].place >= m->place_count) return false;
    const SolMirMaterializedPlace *place = &m->places[m->instructions[instruction].place];
    return place->root_type < concrete->layout.type_count && place->final_type < concrete->layout.type_count
        && place->projections.count == 1 && represented_callable_product_recipe(concrete,
            concrete->layout.types[place->root_type].recipe) && represented_unbound_function_recipe(
                concrete, concrete->layout.types[place->final_type].recipe);
}

static bool represented_moved_callable_value(const RepresentedFunction *function,
    SolMirMaterializedValueId value, size_t depth) {
    const SolMirConcreteProgram *concrete = function->request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    size_t defining = SOL_MIR_MATERIALIZED_NONE, definitions = 0;
    if (depth > 4 || value >= m->value_count) return false;
    for (size_t i = 0; i < m->instruction_count; ++i)
        if (m->instructions[i].result == value) { defining = i; ++definitions; }
    if (definitions != 1) return false;
    if (represented_callable_field_move(function, defining)) return true;
    const SolMirMaterializedInstruction *move = &m->instructions[defining];
    if (move->kind != SOL_MIR_INST_LOAD_MOVE || move->place >= m->place_count
        || m->places[move->place].projections.count != 0
        || m->places[move->place].final_type >= concrete->layout.type_count
        || !represented_unbound_function_recipe(concrete,
            concrete->layout.types[m->places[move->place].final_type].recipe)) return false;
    SolMirMaterializedValueId source = SOL_MIR_MATERIALIZED_NONE;
    for (size_t i = 0; i < m->instruction_count; ++i) {
        const SolMirMaterializedInstruction *store = &m->instructions[i];
        if (store->kind == SOL_MIR_INST_STORE && store->place < m->place_count
            && m->places[store->place].projections.count == 0
            && m->places[store->place].local == m->places[move->place].local) {
            if (source != SOL_MIR_MATERIALIZED_NONE) return false;
            source = store->left;
        }
    }
    return source != SOL_MIR_MATERIALIZED_NONE
        && represented_moved_callable_value(function, source, depth + 1);
}

/* A moved callable is not credited when it is read from the product.  Credit it
 * only if a named cleanup action can be traced through its STORE/TEMP_INIT
 * producer to that exact projected LOAD_MOVE. */
static bool represented_moved_callable_cleanup_action(const RepresentedFunction *function,
    const SolMirRuntimeCleanupAction *action) {
    const SolMirConcreteProgram *concrete = function->request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    SolMirMaterializedValueId produced = SOL_MIR_MATERIALIZED_NONE;
    if (action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE) {
        if (action->target >= m->place_count || action->drop_path == SOL_MIR_RUNTIME_NONE
            || m->places[action->target].projections.count != 0
            || m->places[action->target].final_type >= concrete->layout.type_count
            || action->recipe != concrete->layout.types[m->places[action->target].final_type].recipe
            || !represented_unbound_function_recipe(concrete, action->recipe)) return false;
        for (size_t i = function->image->blocks.offset;
             i < function->image->blocks.offset + function->image->blocks.count; ++i) {
            if (i >= m->block_count) return false;
            const SolMirMaterializedBlock *block = &m->blocks[i];
            if (block->instructions.offset > m->instruction_count || block->instructions.count
                > m->instruction_count - block->instructions.offset) return false;
            for (size_t q = 0; q < block->instructions.count; ++q) {
                const SolMirMaterializedInstruction *instruction = &m->instructions[
                    block->instructions.offset + q];
                if (instruction->kind == SOL_MIR_INST_STORE && instruction->place == action->target) {
                    if (produced != SOL_MIR_MATERIALIZED_NONE) return false;
                    produced = instruction->left;
                }
            }
        }
    } else if (action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_TEMPORARY) {
        if (action->target >= m->temporary_count || action->drop_path != SOL_MIR_RUNTIME_NONE
            || m->temporaries[action->target].type >= concrete->layout.type_count
            || action->recipe != concrete->layout.types[m->temporaries[action->target].type].recipe
            || !represented_unbound_function_recipe(concrete, action->recipe)) return false;
        for (size_t i = function->image->blocks.offset;
             i < function->image->blocks.offset + function->image->blocks.count; ++i) {
            if (i >= m->block_count) return false;
            const SolMirMaterializedBlock *block = &m->blocks[i];
            if (block->instructions.offset > m->instruction_count || block->instructions.count
                > m->instruction_count - block->instructions.offset) return false;
            for (size_t q = 0; q < block->instructions.count; ++q) {
                const SolMirMaterializedInstruction *instruction = &m->instructions[
                    block->instructions.offset + q];
                if (instruction->kind == SOL_MIR_INST_TEMPORARY_INIT
                    && instruction->temporary == action->target) {
                    if (produced != SOL_MIR_MATERIALIZED_NONE) return false;
                    produced = instruction->left;
                }
            }
        }
    } else return false;
    if (produced == SOL_MIR_MATERIALIZED_NONE) return false;
    return represented_moved_callable_value(function, produced, 0);
}

static bool represented_moved_callable_temporary(const RepresentedFunction *function,
    SolMirMaterializedTemporaryId temporary) {
    const SolMirConcreteProgram *concrete = function->request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    if (temporary >= m->temporary_count || m->temporaries[temporary].type >= concrete->layout.type_count
        || !represented_unbound_function_recipe(concrete,
            concrete->layout.types[m->temporaries[temporary].type].recipe)) return false;
    SolMirMaterializedValueId produced = SOL_MIR_MATERIALIZED_NONE;
    for (size_t i = function->image->blocks.offset;
         i < function->image->blocks.offset + function->image->blocks.count; ++i) {
        if (i >= m->block_count) return false;
        const SolMirMaterializedBlock *block = &m->blocks[i];
        if (block->instructions.offset > m->instruction_count || block->instructions.count
            > m->instruction_count - block->instructions.offset) return false;
        for (size_t q = 0; q < block->instructions.count; ++q) {
            const SolMirMaterializedInstruction *instruction = &m->instructions[
                block->instructions.offset + q];
            if (instruction->kind == SOL_MIR_INST_TEMPORARY_INIT
                && instruction->temporary == temporary) {
                if (produced != SOL_MIR_MATERIALIZED_NONE) return false;
                produced = instruction->left;
            }
        }
    }
    if (produced == SOL_MIR_MATERIALIZED_NONE) return false;
    return represented_moved_callable_value(function, produced, 0);
}

static BinaryenExpressionRef i64_bool(BinaryenModuleRef module,
    BinaryenExpressionRef value) {
    return BinaryenUnary(module, BinaryenExtendUInt32(), value);
}

static BinaryenExpressionRef get_value(const RepresentedFunction *function,
    SolMirMaterializedValueId value) {
    size_t index = value_index(function, value);
    return index == SIZE_MAX ? NULL : BinaryenLocalGet(function->module,
        (BinaryenIndex)index, BinaryenTypeInt64());
}

/* Predicate value IDs are P2-global, while locals are per emitted image.  Keep
 * the physical layout image-local and derive the offset only from authenticated
 * instance-owned body ranges. */
static size_t represented_predicate_value_index(const RepresentedFunction *function,
    SolMirPredicateValueId value) {
    const SolMirOperations *operations = &function->request->program->conventions->concrete->operations;
    size_t base = 0;
    for (size_t i = 0; i < operations->predicate_body_count; ++i) {
        const SolMirPredicateBody *body = &operations->predicate_bodies[i];
        if (body->owner_kind != SOL_MIR_PREDICATE_OWNER_INSTANCE || body->instance != function->image_id)
            continue;
        if (body->values.offset > operations->predicate_value_count
            || body->values.count > operations->predicate_value_count - body->values.offset)
            return SIZE_MAX;
        if (value >= body->values.offset && value - body->values.offset < body->values.count)
            return function->predicate_value_base + base + value - body->values.offset;
        if (!represented_add(base, body->values.count, &base)) return SIZE_MAX;
    }
    return SIZE_MAX;
}

static BinaryenExpressionRef get_predicate_value(const RepresentedFunction *function,
    SolMirPredicateValueId value) {
    size_t index = represented_predicate_value_index(function, value);
    return index == SIZE_MAX ? NULL : BinaryenLocalGet(function->module,
        (BinaryenIndex)index, BinaryenTypeInt64());
}

static const SolMirRuntimeSignature *signature_for(const SolMirRuntimeConventions *conventions,
    SolMirLinkageCallableId callable) {
    const SolMirRuntimeSignature *result = NULL;
    for (size_t i = 0; i < conventions->signature_count; ++i) {
        const SolMirRuntimeSignature *candidate = &conventions->signatures[i];
        if (candidate->origin == SOL_MIR_RUNTIME_SIGNATURE_INTERNAL
            && candidate->internal == callable) {
            if (result != NULL) return NULL;
            result = candidate;
        }
    }
    return result;
}

static bool represented_predicate_values_for_image(const SolMirConcreteProgram *concrete,
    size_t image, size_t *count) {
    const SolMirOperations *operations = &concrete->operations;
    size_t total = 0;
    for (size_t i = 0; i < operations->predicate_body_count; ++i) {
        const SolMirPredicateBody *body = &operations->predicate_bodies[i];
        if (body->owner_kind == SOL_MIR_PREDICATE_OWNER_INSTANCE && body->instance == image
            && !represented_add(total, body->values.count, &total)) return false;
    }
    *count = total;
    return true;
}

static bool represented_snapshot_count_for_image(const SolMirConcreteProgram *concrete,
    size_t image, size_t *count) {
    size_t total = 0;
    for (size_t i = 0; i < concrete->operations.snapshot_count; ++i) {
        const SolMirOperationSnapshotPlan *plan = &concrete->operations.snapshots[i];
        if (plan->image != image) continue;
        if (total == SIZE_MAX || plan->slot != total) return false;
        ++total;
    }
    *count = total;
    return true;
}

/* Keep the resource census and the emitter on one physical-local layout:
 * params, values, temporaries, P2 locals, their init flags, edge/call scratch,
 * and the dispatch PC.  Entry wrappers have no locals but are counted as
 * functions by represented_owner. */
static bool represented_function_pattern_scratch_needed(const SolWasmRepresentedBuildRequest *request,
    const SolMirMaterializedImage *image) {
    const SolMirMaterialization *m = &request->program->conventions->concrete->materialization;
    for (size_t b = 0; b < image->blocks.count; ++b) {
        const SolMirMaterializedBlock *block = &m->blocks[image->blocks.offset + b];
        for (size_t i = 0; i < block->instructions.count; ++i) {
            size_t instruction = block->instructions.offset + i;
            if (instruction >= m->instruction_count
                || m->instructions[instruction].kind != SOL_MIR_INST_PATTERN_VALUE) continue;
            const SolMirOperationPatternExtraction *plan = represented_pattern_extraction_for_instruction(
                request, instruction);
            if (plan == NULL || plan->copy_kind != SOL_MIR_COPY_TRIVIAL) return true;
        }
    }
    return false;
}

static bool represented_function_propagation_scratch_needed(
    const SolWasmRepresentedBuildRequest *request, const SolMirMaterializedImage *image) {
    const SolMirMaterialization *m = &request->program->conventions->concrete->materialization;
    for (size_t b = 0; b < image->blocks.count; ++b) {
        size_t block = image->blocks.offset + b;
        if (block >= m->block_count) return false;
        if (m->blocks[block].terminator.kind == SOL_MIR_TERM_PROPAGATE) return true;
    }
    return false;
}

static bool represented_function_terminal_scratch_needed(
    const SolWasmRepresentedBuildRequest *request, const SolMirMaterializedImage *image) {
    const SolMirMaterialization *m = &request->program->conventions->concrete->materialization;
    for (size_t b = 0; b < image->blocks.count; ++b) {
        size_t block = image->blocks.offset + b;
        if (block >= m->block_count) return false;
        if (m->blocks[block].terminator.kind == SOL_MIR_TERM_PANIC) return true;
    }
    return false;
}

static bool represented_function_callable_hole_count(const SolWasmRepresentedBuildRequest *request,
    const SolMirMaterializedImage *image, size_t *count) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    size_t total = 0;
    for (size_t i = 0; i < image->locals.count; ++i) {
        size_t local = image->locals.offset + i;
        if (local >= m->local_count || m->locals[local].type >= concrete->layout.type_count) return false;
        if (represented_callable_product_recipe(concrete,
                concrete->layout.types[m->locals[local].type].recipe)) {
            if (total == SIZE_MAX) return false;
            ++total;
        }
    }
    *count = total;
    return true;
}

/* Parallel copies use physical i64 locals.  Predicate edges are dispatched in
 * the same callable as image edges, so their widest forwarding arity belongs
 * in the one shared scratch reservation and census. */
static bool represented_function_scratch_count(const SolWasmRepresentedBuildRequest *request,
    const SolMirMaterializedImage *image, size_t image_id, size_t *count) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirOperations *operations = &concrete->operations;
    size_t scratch = (request->program->conventions->call_count == 0
        && !represented_fixed_products_needed(request)
        && !represented_function_pattern_scratch_needed(request, image)
        && !represented_function_propagation_scratch_needed(request, image)
        && !represented_function_terminal_scratch_needed(request, image)) ? 0 : 1;
    for (size_t i = 0; i < image->blocks.count; ++i) {
        size_t block = image->blocks.offset + i;
        if (block >= m->block_count) return false;
        if (m->blocks[block].parameters.count > scratch) scratch = m->blocks[block].parameters.count;
    }
    for (size_t edge = 0; edge < operations->predicate_edge_count; ++edge) {
        const SolMirPredicateEdge *item = &operations->predicate_edges[edge];
        if (item->source >= operations->predicate_block_count) return false;
        const SolMirPredicateBlock *source = &operations->predicate_blocks[item->source];
        if (source->body >= operations->predicate_body_count) return false;
        const SolMirPredicateBody *body = &operations->predicate_bodies[source->body];
        if (body->owner_kind != SOL_MIR_PREDICATE_OWNER_INSTANCE || body->instance != image_id) continue;
        if (item->arguments.offset > operations->predicate_edge_value_count
            || item->arguments.count > operations->predicate_edge_value_count - item->arguments.offset)
            return false;
        if (item->arguments.count > scratch) scratch = item->arguments.count;
    }
    /* Byte equality stages its result before publishing the destination. */
    *count = scratch == 0 ? 1 : scratch;
    return true;
}

static bool represented_function_local_count(const SolWasmRepresentedBuildRequest *request,
    size_t callable, const SolMirRuntimeSignature *signature, size_t *count) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    if (callable >= concrete->linkage.callable_count || signature == NULL
        || concrete->linkage.callables[callable].instance >= m->image_count) return false;
    const SolMirMaterializedImage *image = &m->images[concrete->linkage.callables[callable].instance];
    size_t predicate_values = 0;
    if (!represented_predicate_values_for_image(concrete,
            concrete->linkage.callables[callable].instance, &predicate_values)) return false;
    size_t scratch = 0;
    if (!represented_function_scratch_count(request, image,
            concrete->linkage.callables[callable].instance, &scratch)) return false;
    size_t holes = 0;
    if (!represented_function_callable_hole_count(request, image, &holes)) return false;
    size_t snapshots = 0;
    if (!represented_snapshot_count_for_image(concrete,
            concrete->linkage.callables[callable].instance, &snapshots)) return false;
    size_t total = signature->slots.count;
    return represented_add(total, image->values.count, &total)
        && represented_add(total, image->temporaries.count, &total)
        && represented_add(total, image->locals.count, &total)
        && represented_add(total, predicate_values, &total)
        && represented_add(total, predicate_values != 0 ? 1 : 0, &total)
        && represented_add(total, image->temporaries.count, &total)
        && represented_add(total, image->locals.count, &total)
        && represented_add(total, holes, &total)
        && represented_add(total, snapshots, &total)
        && represented_add(total, snapshots, &total)
        && represented_add(total, snapshots != 0 ? 1 : 0, &total)
        && represented_add(total, scratch, &total)
        && represented_add(total, predicate_values != 0 ? 2 : 1, &total) && total <= UINT32_MAX
        && ((*count = total), true);
}

static bool infallible_opcode(const SolMirOperationArithmeticPlan *plan) {
    if (plan->failures != SOL_MIR_OPERATION_FAILURE_NONE || plan->compound) return false;
    switch (plan->opcode) {
        case SOL_MIR_OPERATION_BOOL_NOT:
        case SOL_MIR_OPERATION_I64_LT:
        case SOL_MIR_OPERATION_I64_LE:
        case SOL_MIR_OPERATION_I64_GT:
        case SOL_MIR_OPERATION_I64_GE:
        case SOL_MIR_OPERATION_BOOL_AND:
        case SOL_MIR_OPERATION_BOOL_OR:
        case SOL_MIR_OPERATION_VALUE_EQ:
        case SOL_MIR_OPERATION_VALUE_NE:
            return true;
        default:
            return false;
    }
}

/* Refined scalar equality is nominally authenticated even though its emitted
 * representation is one i64 word.  P2 must describe exactly one wrapper node
 * over the certified direct backing scalar; accepting an empty or unrelated
 * equality tree would silently turn cross-recipe equality into raw-bit
 * equality. */
static bool represented_refined_equality(const SolMirConcreteProgram *concrete,
    const SolMirOperationArithmeticPlan *plan) {
    const SolMirOperations *operations = &concrete->operations;
    const SolMirMaterialization *materialization = &concrete->materialization;
    SolMirRecipeId backing = SOL_MIR_RECIPE_NONE;
    if (!represented_leaf_refined_recipe(concrete, plan->operand_recipe, &backing)
        || plan->left >= materialization->value_count
        || plan->right >= materialization->value_count
        || materialization->values[plan->left].type >= concrete->layout.type_count
        || materialization->values[plan->right].type >= concrete->layout.type_count
        || concrete->layout.types[materialization->values[plan->left].type].recipe
            != plan->operand_recipe
        || concrete->layout.types[materialization->values[plan->right].type].recipe
            != plan->operand_recipe
        || plan->result_recipe >= concrete->representation.recipe_count
        || concrete->representation.recipes[plan->result_recipe].kind != SOL_MIR_RECIPE_BOOL
        || plan->equality.count != 2 || plan->equality.offset > operations->equality_node_count
        || plan->equality.count > operations->equality_node_count - plan->equality.offset)
        return false;
    const SolMirOperationEqualityNode *wrapper = &operations->equality_nodes[
        plan->equality.offset];
    const SolMirOperationEqualityNode *scalar = wrapper + 1;
    if (wrapper->recipe != plan->operand_recipe
        || wrapper->kind != SOL_MIR_OPERATION_EQUAL_WRAPPER
        || wrapper->children.count != 1
        || wrapper->children.offset >= operations->equality_child_count
        || scalar->recipe != backing || scalar->kind != SOL_MIR_OPERATION_EQUAL_SCALAR
        || scalar->children.count != 0) return false;
    const SolMirOperationEqualityChild *child = &operations->equality_children[
        wrapper->children.offset];
    return child->recipe == backing && child->field_layout == SOL_MIR_OPERATION_NONE
        && child->variant_layout == SOL_MIR_OPERATION_NONE && child->semantic_tag == 0;
}

static unsigned represented_opcode_failures(SolMirOperationOpcode opcode) {
    switch (opcode) {
        case SOL_MIR_OPERATION_I64_NEG: case SOL_MIR_OPERATION_I64_ADD:
        case SOL_MIR_OPERATION_I64_SUB: case SOL_MIR_OPERATION_I64_MUL:
            return SOL_MIR_OPERATION_FAILURE_OVERFLOW;
        case SOL_MIR_OPERATION_I64_DIV: case SOL_MIR_OPERATION_I64_REM:
            return SOL_MIR_OPERATION_FAILURE_OVERFLOW
                | SOL_MIR_OPERATION_FAILURE_DIVISION_BY_ZERO;
        default: return SOL_MIR_OPERATION_FAILURE_NONE;
    }
}

static uint32_t represented_failure_mask(unsigned failures) {
    uint32_t mask = 0;
    if ((failures & SOL_MIR_OPERATION_FAILURE_OVERFLOW) != 0)
        mask |= UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_INTEGER_OVERFLOW - 1);
    if ((failures & SOL_MIR_OPERATION_FAILURE_DIVISION_BY_ZERO) != 0)
        mask |= UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_DIVISION_BY_ZERO - 1);
    return mask;
}

static bool represented_checked_opcode(const SolMirOperationArithmeticPlan *plan) {
    return represented_opcode_failures(plan->opcode) != SOL_MIR_OPERATION_FAILURE_NONE
        && plan->failures == represented_opcode_failures(plan->opcode);
}

static bool represented_predicate_recipe(const SolMirConcreteProgram *concrete,
    SolMirRecipeId recipe, bool bool_only) {
    if (recipe >= concrete->representation.recipe_count) return false;
    const SolMirRecipe *item = &concrete->representation.recipes[recipe];
    if (bool_only) return item->kind == SOL_MIR_RECIPE_BOOL
        && item->storage == SOL_MIR_STORAGE_SCALAR;
    switch (item->kind) {
        case SOL_MIR_RECIPE_INT64:
        case SOL_MIR_RECIPE_BOOL:
            return item->storage == SOL_MIR_STORAGE_SCALAR;
        case SOL_MIR_RECIPE_UNIT:
            return item->storage == SOL_MIR_STORAGE_NONE;
        default:
            return false;
    }
}

/* C2b1 authenticates snapshot identity from source provenance all the way to
 * the P3.6 instruction row.  In particular, `plan->access == NONE` is the
 * expected whole-local shape; it is never reinterpreted as a place index. */
static const SolMirOperationSnapshotPlan *represented_snapshot_for_instruction(
    const SolWasmRepresentedBuildRequest *request, size_t instruction, size_t *plan_id) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirConcreteProgram *concrete = owner->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirOperations *o = &concrete->operations;
    const SolMirRepresentation *r = &concrete->representation;
    const SolIr *ir = concrete->program.ir;
    if (instruction >= m->instruction_count || instruction >= owner->image_instruction_count
        || m->instructions[instruction].kind != SOL_MIR_INST_CAPTURE_SNAPSHOT) return NULL;
    const SolMirMaterializedInstruction *item = &m->instructions[instruction];
    size_t found = SOL_MIR_OPERATION_NONE, matches = 0;
    for (size_t i = 0; i < o->snapshot_count; ++i)
        if (o->snapshots[i].instruction == instruction) { found = i; ++matches; }
    if (matches != 1 || found >= o->snapshot_count || item->source_snapshot >= ir->snapshot_count)
        return NULL;
    const SolMirOperationSnapshotPlan *plan = &o->snapshots[found];
    if (plan->image >= m->image_count || plan->context >= m->context_count
        || plan->local >= m->local_count || plan->provenance >= o->provenance_count
        || plan->path.count != 0 || plan->path.offset > o->path_step_count
        || plan->access != SOL_MIR_OPERATION_NONE || plan->copy_kind != SOL_MIR_COPY_TRIVIAL
        || plan->recipe >= r->recipe_count || plan->root_recipe != plan->recipe
        || item->type >= concrete->layout.type_count
        || concrete->layout.types[item->type].recipe != plan->recipe
        || item->block >= m->block_count) return NULL;
    const SolMirRecipe *recipe = &r->recipes[plan->recipe];
    if ((recipe->kind != SOL_MIR_RECIPE_INT64 && recipe->kind != SOL_MIR_RECIPE_BOOL)
        || recipe->storage != SOL_MIR_STORAGE_SCALAR || !recipe->is_copy) return NULL;
    const SolMirMaterializedImage *image = &m->images[plan->image];
    const SolMirMaterializedLocal *local = &m->locals[plan->local];
    if (image->instance != plan->image || item->block < image->blocks.offset
        || item->block - image->blocks.offset >= image->blocks.count
        || plan->local < image->locals.offset
        || plan->local - image->locals.offset >= image->locals.count
        || local->kind != SOL_MIR_MATERIALIZED_LOCAL_PARAMETER
        || local->source_local >= ir->local_count || ir->locals[local->source_local].kind
            != SOL_IR_LOCAL_PARAMETER || local->access != ir->locals[local->source_local].access
        || local->type >= concrete->layout.type_count
        || concrete->layout.types[local->type].recipe != plan->root_recipe) return NULL;
    const SolIrSnapshot *snapshot = &ir->snapshots[item->source_snapshot];
    if (snapshot->id != item->source_snapshot || snapshot->obligation >= ir->obligation_count
        || snapshot->read >= ir->expression_count || item->source_expression != snapshot->read
        || snapshot->operand >= ir->expression_count || snapshot->type != ir->locals[
            local->source_local].type || ir->expressions[snapshot->operand].kind != SOL_IR_EXPR_PLACE
        || ir->expressions[snapshot->operand].as.place >= ir->place_count) return NULL;
    const SolIrPlace *place = &ir->places[ir->expressions[snapshot->operand].as.place];
    const SolIrObligation *obligation = &ir->obligations[snapshot->obligation];
    if (place->root_kind != SOL_IR_PLACE_ROOT_LOCAL || place->local != local->source_local
        || place->projections.count != 0 || obligation->id != snapshot->obligation
        || item->source_snapshot < obligation->snapshots.offset
        || item->source_snapshot - obligation->snapshots.offset >= obligation->snapshots.count)
        return NULL;
    const SolMirPlanContext *context = &m->contexts[plan->context];
    if (context->kind != SOL_MIR_PLAN_CONTEXT_CONTRACT || context->target_kind
            != SOL_MIR_PLAN_TARGET_INSTANCE || context->instance != plan->image
        || context->obligation != snapshot->obligation) return NULL;
    const SolMirOperationProvenance *provenance = &o->provenance[plan->provenance];
    if (provenance->kind != SOL_MIR_OPERATION_PROVENANCE_SNAPSHOT
        || provenance->executable != found || provenance->source_expression != snapshot->operand
        || provenance->source_obligation != snapshot->obligation
        || provenance->source_snapshot != item->source_snapshot) return NULL;

    size_t typed = 0, overlays = 0;
    if (plan->image >= m->plan->instance_count) return NULL;
    const SolMirPlanInstance *instance = &m->plan->instances[plan->image];
    if (instance->typed_uses.offset > m->plan->typed_use_count
        || instance->typed_uses.count > m->plan->typed_use_count - instance->typed_uses.offset
        || image->overlays.offset > m->overlay_count
        || image->overlays.count > m->overlay_count - image->overlays.offset) return NULL;
    for (size_t i = 0; i < instance->typed_uses.count; ++i) {
        const SolMirPlanTypedUse *use = &m->plan->typed_uses[instance->typed_uses.offset + i];
        if (use->kind == SOL_MIR_PLAN_USE_SNAPSHOT && use->source == item->source_snapshot) {
            if (use->ordinal != 0 || use->context != plan->context
                || use->access != SOL_ACCESS_OWNED) return NULL;
            ++typed;
        }
    }
    for (size_t i = 0; i < image->overlays.count; ++i) {
        const SolMirMaterializedTypeOverlay *overlay = &m->overlays[image->overlays.offset + i];
        if (overlay->kind == SOL_MIR_PLAN_USE_SNAPSHOT
            && overlay->source == item->source_snapshot) {
            if (overlay->ordinal != 0 || overlay->context != plan->context
                || overlay->type >= concrete->layout.type_count
                || concrete->layout.types[overlay->type].recipe != plan->recipe
                || overlay->access != SOL_ACCESS_OWNED) return NULL;
            ++overlays;
        }
    }
    if (typed != 1 || overlays != 1) return NULL;

    const SolMirRuntimeLoweredImageInstruction *row = &owner->image_instructions[instruction];
    if (row->state != SOL_MIR_RUNTIME_LOWERED_PRESENT
        || row->execution != SOL_MIR_RUNTIME_LOWERED_EXECUTABLE || row->image != plan->image
        || row->instruction != instruction || row->block != item->block
        || row->kind != SOL_MIR_INST_CAPTURE_SNAPSHOT
        || row->runtime_class != SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE
        || row->plan_family != SOL_MIR_RUNTIME_LOWERED_PLAN_SNAPSHOT
        || row->facilities != SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE
        || row->plan >= owner->semantic_plan_count
        || row->cleanup_event != SOL_MIR_RUNTIME_LOWERED_NONE
        || row->failure_site != SOL_MIR_RUNTIME_NONE) return NULL;
    const SolMirRuntimeLoweredSemanticPlan *semantic = &owner->semantic_plans[row->plan];
    if (semantic->state != SOL_MIR_RUNTIME_LOWERED_PRESENT
        || semantic->arena != SOL_MIR_RUNTIME_LOWERED_SEMANTIC_SNAPSHOT
        || semantic->runtime_class != SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE
        || semantic->plan_family != SOL_MIR_RUNTIME_LOWERED_PLAN_SNAPSHOT
        || semantic->plan != found || semantic->facilities
            != (SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY | SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)
        || semantic->producer_kind != SOL_MIR_MATERIALIZED_PRODUCER_INSTRUCTION
        || semantic->producer != instruction) return NULL;
    if (plan_id != NULL) *plan_id = found;
    return plan;
}

static bool represented_snapshots_preflight(const SolWasmRepresentedBuildRequest *request) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirOperations *o = &concrete->operations;
    size_t captures = 0;
    for (size_t instruction = 0; instruction < m->instruction_count; ++instruction) {
        if (m->instructions[instruction].kind != SOL_MIR_INST_CAPTURE_SNAPSHOT) continue;
        size_t plan = SOL_MIR_OPERATION_NONE;
        if (represented_snapshot_for_instruction(request, instruction, &plan) == NULL) return false;
        ++captures;
    }
    if (captures != o->snapshot_count) return false;
    for (size_t i = 0; i < o->snapshot_count; ++i) {
        const SolMirOperationSnapshotPlan *plan = &o->snapshots[i];
        size_t expected = 0;
        for (size_t q = 0; q < i; ++q) expected += o->snapshots[q].image == plan->image;
        if (plan->slot != expected) return false;
    }
    return true;
}

static const SolMirOperationSnapshotPlan *represented_snapshot_for_predicate_input(
    const SolWasmRepresentedBuildRequest *request, size_t body_id,
    const SolMirPredicateInput *input, size_t *plan_id) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirOperations *o = &concrete->operations;
    const SolIr *ir = concrete->program.ir;
    if (body_id >= o->predicate_body_count || input == NULL
        || input->kind != SOL_MIR_PREDICATE_INPUT_SNAPSHOT
        || input->access != SOL_ACCESS_OWNED) return NULL;
    const SolMirPredicateBody *body = &o->predicate_bodies[body_id];
    if (body->owner_kind != SOL_MIR_PREDICATE_OWNER_INSTANCE || body->instance >= m->image_count
        || body->context >= m->context_count) return NULL;
    const SolMirPlanContext *context = &m->contexts[body->context];
    if (context->kind != SOL_MIR_PLAN_CONTEXT_CONTRACT || context->target_kind
            != SOL_MIR_PLAN_TARGET_INSTANCE || context->instance != body->instance
        || context->obligation >= ir->obligation_count) return NULL;
    const SolMirMaterializedImage *image = &m->images[body->instance];
    size_t base = 0; bool reached = false;
    for (size_t i = 0; i < image->contexts.count; ++i) {
        size_t id = image->contexts.offset + i;
        if (id >= m->context_count) return NULL;
        if (id == body->context) { reached = true; break; }
        if (m->contexts[id].kind != SOL_MIR_PLAN_CONTEXT_CONTRACT) continue;
        size_t prior = m->contexts[id].obligation;
        if (prior >= ir->obligation_count
            || ir->obligations[prior].snapshots.count > SIZE_MAX - base) return NULL;
        base += ir->obligations[prior].snapshots.count;
    }
    const SolIrObligation *obligation = &ir->obligations[context->obligation];
    if (!reached || input->ordinal < base || input->ordinal - base >= obligation->snapshots.count)
        return NULL;
    size_t source_snapshot = obligation->snapshots.offset + input->ordinal - base;
    if (source_snapshot >= ir->snapshot_count || ir->snapshots[source_snapshot].obligation
            != context->obligation) return NULL;
    size_t found = SOL_MIR_OPERATION_NONE, matches = 0;
    for (size_t i = 0; i < o->snapshot_count; ++i) {
        const SolMirOperationSnapshotPlan *plan = &o->snapshots[i];
        if (plan->image != body->instance || plan->context != body->context
            || plan->provenance >= o->provenance_count
            || o->provenance[plan->provenance].source_snapshot != source_snapshot) continue;
        found = i; ++matches;
    }
    if (matches != 1 || found >= o->snapshot_count) return NULL;
    const SolMirOperationSnapshotPlan *plan = &o->snapshots[found];
    size_t authenticated = SOL_MIR_OPERATION_NONE;
    if (input->recipe != plan->recipe
        || represented_snapshot_for_instruction(request, plan->instruction, &authenticated) != plan
        || authenticated != found) return NULL;
    if (plan_id != NULL) *plan_id = found;
    return plan;
}

/* C1/C2 accept owned instance callable predicates only. Requires keep their
 * entry inputs; ALWAYS ensures may read their complete scalar result, while a
 * certified SUCCESS Result ensure reads only its authenticated scalar payload.
 * Every
 * emitted predicate row is joined to its P3.6 lowering row here; unsupported
 * predicate features are a closure rejection, never a best-effort omission. */
static bool represented_predicate_body_preflight(const SolWasmRepresentedBuildRequest *request,
    const RepresentedQualifiedIndex *qualified_index, size_t image,
    const SolMirRuntimeSignature *signature, size_t body_id) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirConcreteProgram *concrete = owner->conventions->concrete;
    const SolMirOperations *o = &concrete->operations;
    if (body_id >= o->predicate_body_count || body_id >= owner->predicate_body_count) return false;
    const SolMirPredicateBody *body = &o->predicate_bodies[body_id];
    const SolMirRuntimeLoweredPredicateBody *row = &owner->predicate_bodies[body_id];
    bool refinement = body->refinement_self_recipe != SOL_MIR_RECIPE_NONE;
    size_t plan_id = SOL_MIR_RUNTIME_NONE;
    const SolMirOperationPredicatePlan *plan = refinement
        ? represented_refinement_plan_for_body(request, qualified_index, image, body_id, &plan_id)
        : represented_plan_for_body(request, qualified_index, image, body_id);
    if (!refinement && plan != NULL) plan_id = (size_t)(plan - o->predicates);
    bool requires = body->phase == SOL_CONTRACT_REQUIRES;
    bool ensures = body->phase == SOL_CONTRACT_ENSURES;
    bool qualified = ensures && (body->outcome == SOL_CONTRACT_OUTCOME_SUCCESS
        || body->outcome == SOL_CONTRACT_OUTCOME_FAILURE);
    RepresentedQualifiedResult qualified_result = {0};
    const RepresentedQualifiedImage *qualified_image = ensures
        ? represented_qualified_image(qualified_index, image, signature->result) : NULL;
    bool represented_result = qualified_image != NULL;
    if (represented_result) qualified_result = qualified_image->result;
    if (body->owner_kind != SOL_MIR_PREDICATE_OWNER_INSTANCE || body->instance != image
        || body->import != SOL_MIR_MATERIALIZED_NONE
        || (!requires && !ensures) || (refinement && (!requires
            || body->outcome != SOL_CONTRACT_OUTCOME_ALWAYS))
        || (requires && body->outcome != SOL_CONTRACT_OUTCOME_ALWAYS)
        || (ensures && body->outcome != SOL_CONTRACT_OUTCOME_ALWAYS && !qualified)
        || !represented_predicate_recipe(concrete, body->output_recipe, true)
        || (refinement ? !represented_scalar_recipe(concrete,
                body->refinement_self_recipe, false)
            : body->refinement_self_recipe != SOL_MIR_RECIPE_NONE)
        || body->blocks.count == 0 || body->blocks.offset > o->predicate_block_count
        || body->blocks.count > o->predicate_block_count - body->blocks.offset
        || body->values.offset > o->predicate_value_count
        || body->values.count > o->predicate_value_count - body->values.offset
        || body->entry < body->blocks.offset || body->entry - body->blocks.offset >= body->blocks.count
        || row->state != SOL_MIR_RUNTIME_LOWERED_PRESENT || row->body != body_id
        || row->owner_kind != body->owner_kind || row->image != image
        || row->import_id != body->import || row->context != body->context
        || row->phase != body->phase || row->blocks.offset != body->blocks.offset
        || row->blocks.count != body->blocks.count || row->entry != body->entry
        || row->outcome != body->outcome || row->output_recipe != body->output_recipe
        || row->refinement_self_recipe != body->refinement_self_recipe
        || body->inputs.offset > o->predicate_input_count
        || body->inputs.count > o->predicate_input_count - body->inputs.offset
        || plan == NULL || plan->provenance >= o->provenance_count
        || plan->block >= concrete->materialization.block_count
        || plan->context >= concrete->materialization.context_count
        || plan->contract_phase != body->phase || plan->contract_outcome != body->outcome
        || plan->context != body->context || plan->output_recipe != body->output_recipe
        || (refinement ? (plan->kind != SOL_MIR_OPERATION_PREDICATE_REFINEMENT
                || plan->input_recipe != body->refinement_self_recipe
                || plan->representation >= concrete->materialization.temporary_count
                || concrete->materialization.temporaries[plan->representation].type
                    != plan->input_recipe)
            : (plan->kind != SOL_MIR_OPERATION_PREDICATE_CONTRACT
                || plan->input_recipe != SOL_MIR_RECIPE_NONE
                || plan->representation != SOL_MIR_MATERIALIZED_NONE)))
        return false;
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirMaterializedTerminator *check = &m->blocks[plan->block].terminator;
    const SolMirPlanContext *context = &m->contexts[plan->context];
    if (check->kind != (refinement ? SOL_MIR_TERM_CHECK_REFINED : SOL_MIR_TERM_CHECK_CONTRACT)
        || check->source_obligation >= concrete->program.ir->obligation_count
        || check->source_obligation != context->obligation || check->source_obligation
            != concrete->operations.provenance[plan->provenance].source_obligation
        || check->contract_phase != body->phase || check->contract_outcome != body->outcome
        || context->kind != (refinement ? SOL_MIR_PLAN_CONTEXT_REFINEMENT
            : SOL_MIR_PLAN_CONTEXT_CONTRACT) || context->instance != image
        || context->target_kind != SOL_MIR_PLAN_TARGET_INSTANCE
        || context->obligation >= concrete->program.ir->obligation_count) return false;
    const SolIrObligation *obligation = &concrete->program.ir->obligations[context->obligation];
    if (obligation->kind != body->phase || obligation->outcome != body->outcome
        || (requires && (obligation->snapshots.count != 0 || obligation->result_available
            || check->result != SOL_MIR_MATERIALIZED_NONE || plan->result != SOL_MIR_MATERIALIZED_NONE
            || plan->result_recipe != SOL_MIR_RECIPE_NONE) && !refinement) || (ensures
            && (obligation->result_available != (body->outcome != SOL_CONTRACT_OUTCOME_FAILURE)
                || check->result >= m->value_count
                || plan->result != check->result || plan->result_recipe != m->values[check->result].type
                || plan->result_recipe != signature->result))) return false;
    if (refinement) {
        SolMirRecipeId backing = SOL_MIR_RECIPE_NONE;
        const SolMirOperationProvenance *provenance = &o->provenance[plan->provenance];
        if (!represented_leaf_refined_recipe(concrete, plan->result_recipe, &backing)
            || backing != plan->input_recipe || plan->result != check->result
            || check->result >= m->value_count || m->values[check->result].type != plan->result_recipe
            || check->representation != plan->representation
            || check->source_definition != concrete->representation.recipes[
                plan->result_recipe].concrete_definition
            || context->definition != check->source_definition
            || context->refinement_type != plan->result_recipe
            || obligation->owner_kind != SOL_CONTRACT_OWNER_TYPE
            || obligation->owner != check->source_definition
            || obligation->predicate >= concrete->program.ir->expression_count
            || obligation->snapshots.count != 0 || obligation->result_available
            || obligation->result_type != SOL_IR_NONE
            || provenance->kind != SOL_MIR_OPERATION_PROVENANCE_PREDICATE
            || provenance->executable != plan_id
            || provenance->source_expression != check->source_expression
            || provenance->source_obligation != check->source_obligation
            || provenance->source_definition != SOL_IR_NONE) return false;
    }
    if (ensures) {
        if (plan->result_recipe >= concrete->representation.recipe_count) return false;
        SolMirRecipeKind result_kind = concrete->representation.recipes[plan->result_recipe].kind;
        if ((result_kind == SOL_MIR_RECIPE_UNIT) != (signature->result_class
                == SOL_MIR_RUNTIME_RESULT_UNIT)
            || (result_kind != SOL_MIR_RECIPE_UNIT && signature->result_class
                != SOL_MIR_RUNTIME_RESULT_VALUE)
            || (qualified && !represented_result)
            || (represented_result
                ? plan->result_recipe != signature->result
                : result_kind != SOL_MIR_RECIPE_INT64 && result_kind != SOL_MIR_RECIPE_BOOL
                    && result_kind != SOL_MIR_RECIPE_UNIT)) return false;
    }
    if (plan->block >= owner->image_terminator_count)
        return false;
    const SolMirRuntimeLoweredImageTerminator *check_row = &owner->image_terminators[plan->block];
    if (check_row->state != SOL_MIR_RUNTIME_LOWERED_PRESENT || check_row->image != image
        || check_row->block != plan->block || check_row->kind != check->kind
        || check_row->execution != SOL_MIR_RUNTIME_LOWERED_EXECUTABLE
        || check_row->runtime_class != SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE
        || check_row->plan_family != SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE
        || check_row->facilities != (SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP
            | SOL_MIR_RUNTIME_LOWERED_FACILITY_EVENT
            | SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE)
        || check_row->plan >= owner->semantic_plan_count) return false;
    const SolMirRuntimeLoweredSemanticPlan *semantic = &owner->semantic_plans[check_row->plan];
    if (semantic->state != SOL_MIR_RUNTIME_LOWERED_PRESENT
        || semantic->arena != SOL_MIR_RUNTIME_LOWERED_SEMANTIC_PREDICATE
        || semantic->runtime_class != SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE
        || semantic->plan_family != SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE
        || semantic->plan != plan_id || semantic->producer_kind != SOL_MIR_MATERIALIZED_PRODUCER_PREDICATE
        || semantic->producer != body_id || semantic->facilities
            != (SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE
                | SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP)) return false;
    for (size_t other_id = 0; other_id < o->predicate_body_count; ++other_id) {
        if (other_id == body_id) continue;
        const SolMirPredicateBody *other = &o->predicate_bodies[other_id];
        if (other->owner_kind != SOL_MIR_PREDICATE_OWNER_INSTANCE || other->instance != image) continue;
        if (other->values.offset > o->predicate_value_count
            || other->values.count > o->predicate_value_count - other->values.offset
            || (body->values.offset < other->values.offset + other->values.count
                && other->values.offset < body->values.offset + body->values.count)) return false;
    }
    for (size_t value = body->values.offset; value < body->values.offset + body->values.count; ++value) {
        const SolMirPredicateValue *item = &o->predicate_values[value];
        if (!represented_predicate_recipe(concrete, item->recipe, false)) return false;
        if (item->kind == SOL_MIR_PREDICATE_VALUE_INPUT) {
            if (item->definition < body->inputs.offset || item->definition - body->inputs.offset >= body->inputs.count
                || o->predicate_inputs[item->definition].recipe != item->recipe) return false;
        } else if (item->kind == SOL_MIR_PREDICATE_VALUE_BLOCK_PARAMETER) {
            if (item->block < body->blocks.offset || item->block - body->blocks.offset >= body->blocks.count
                || item->definition >= o->predicate_blocks[item->block].parameters.count
                || o->predicate_blocks[item->block].parameters.offset + item->definition != value) return false;
        } else if (item->kind == SOL_MIR_PREDICATE_VALUE_INSTRUCTION) {
            if (item->definition >= o->predicate_instruction_count
                || o->predicate_instructions[item->definition].result != value) return false;
        } else return false;
    }
    size_t snapshot_inputs = 0, refinement_inputs = 0;
    for (size_t i = 0; i < body->inputs.count; ++i) {
        const SolMirPredicateInput *input = &o->predicate_inputs[body->inputs.offset + i];
        if (input->kind == SOL_MIR_PREDICATE_INPUT_REFINEMENT_SELF) {
            if (!refinement || input->ordinal != 0 || input->access != SOL_ACCESS_OWNED
                || input->recipe != body->refinement_self_recipe) return false;
            ++refinement_inputs;
        } else if (input->kind == SOL_MIR_PREDICATE_INPUT_PARAMETER) {
            if (input->access != SOL_ACCESS_OWNED || input->ordinal >= signature->slots.count
                || signature->slots.offset > owner->conventions->signature_slot_count
                || input->ordinal >= owner->conventions->signature_slot_count - signature->slots.offset
                || owner->conventions->signature_slots[signature->slots.offset + input->ordinal].recipe
                    != input->recipe || !represented_predicate_recipe(concrete, input->recipe, false)) return false;
        } else if (input->kind == SOL_MIR_PREDICATE_INPUT_COMPLETE_RESULT) {
            if (!ensures || qualified || input->ordinal != 0 || input->access != SOL_ACCESS_OWNED
                || input->recipe != plan->result_recipe || input->recipe >= concrete->representation.recipe_count
                || (concrete->representation.recipes[input->recipe].kind != SOL_MIR_RECIPE_INT64
                    && concrete->representation.recipes[input->recipe].kind != SOL_MIR_RECIPE_BOOL)) return false;
        } else if (input->kind == SOL_MIR_PREDICATE_INPUT_SUCCESS_RESULT) {
            if (!qualified || body->outcome != SOL_CONTRACT_OUTCOME_SUCCESS
                || input->ordinal != 0 || input->access != SOL_ACCESS_OWNED
                || input->recipe != qualified_result.payload_recipe) return false;
        } else if (input->kind == SOL_MIR_PREDICATE_INPUT_SNAPSHOT) {
            size_t snapshot_plan = SOL_MIR_OPERATION_NONE;
            if (!ensures || represented_snapshot_for_predicate_input(request, body_id,
                    input, &snapshot_plan) == NULL || snapshot_plan >= o->snapshot_count)
                return false;
            for (size_t q = body->inputs.offset; q < body->inputs.offset + i; ++q) {
                const SolMirPredicateInput *prior = &o->predicate_inputs[q];
                size_t prior_plan = SOL_MIR_OPERATION_NONE;
                if (prior->kind == SOL_MIR_PREDICATE_INPUT_SNAPSHOT
                    && represented_snapshot_for_predicate_input(request, body_id, prior,
                        &prior_plan) != NULL && prior_plan == snapshot_plan) return false;
            }
            ++snapshot_inputs;
        } else return false;
    }
    if (snapshot_inputs != obligation->snapshots.count
        || (refinement ? body->inputs.count != refinement_inputs
            : refinement_inputs != 0)) return false;
    for (size_t b = body->blocks.offset; b < body->blocks.offset + body->blocks.count; ++b) {
        const SolMirPredicateBlock *block = &o->predicate_blocks[b];
        if (block->body != body_id || b >= owner->predicate_block_count || b >= owner->predicate_terminator_count
            || owner->predicate_blocks[b].state != SOL_MIR_RUNTIME_LOWERED_PRESENT
            || owner->predicate_blocks[b].body != body_id
            || owner->predicate_terminators[b].state != SOL_MIR_RUNTIME_LOWERED_PRESENT
            || owner->predicate_terminators[b].body != body_id
            || block->parameters.offset > o->predicate_value_count
            || block->parameters.count > o->predicate_value_count - block->parameters.offset
            || block->instructions.offset > o->predicate_instruction_count
            || block->instructions.count > o->predicate_instruction_count - block->instructions.offset) return false;
        for (size_t i = 0; i < block->instructions.count; ++i) {
            size_t id = block->instructions.offset + i;
            const SolMirPredicateInstruction *in = &o->predicate_instructions[id];
            const SolMirRuntimeLoweredPredicateInstruction *lowered_instruction
                = id < owner->predicate_instruction_count
                    ? &owner->predicate_instructions[id] : NULL;
            if (id >= owner->predicate_instruction_count || in->block != b
                || in->result < body->values.offset || in->result - body->values.offset >= body->values.count
                || !represented_predicate_recipe(concrete, in->recipe, false)
                || owner->predicate_instructions[id].state != SOL_MIR_RUNTIME_LOWERED_PRESENT
                || owner->predicate_instructions[id].body != body_id
                || owner->predicate_instructions[id].block != b
                || owner->predicate_instructions[id].kind != in->kind
                || (in->kind != SOL_MIR_PREDICATE_INST_I64 && in->kind != SOL_MIR_PREDICATE_INST_BOOL
                    && in->kind != SOL_MIR_PREDICATE_INST_UNARY && in->kind != SOL_MIR_PREDICATE_INST_BINARY)
                || in->failures != represented_opcode_failures(in->opcode)
                || !represented_step_route(request,
                    SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION, body_id, b, id,
                    lowered_instruction->step_cleanup_event,
                    lowered_instruction->step_failure_site)) return false;
            const SolMirPredicateValue *result = &o->predicate_values[in->result];
            if ((in->kind == SOL_MIR_PREDICATE_INST_I64
                    && concrete->representation.recipes[in->recipe].kind != SOL_MIR_RECIPE_INT64)
                || (in->kind == SOL_MIR_PREDICATE_INST_BOOL
                    && concrete->representation.recipes[in->recipe].kind != SOL_MIR_RECIPE_BOOL)
                || result->recipe != in->recipe) return false;
            if (in->kind == SOL_MIR_PREDICATE_INST_UNARY || in->kind == SOL_MIR_PREDICATE_INST_BINARY) {
                if (in->left < body->values.offset || in->left - body->values.offset >= body->values.count
                    || (in->opcode != SOL_MIR_OPERATION_BOOL_NOT && in->opcode != SOL_MIR_OPERATION_I64_NEG
                        && (in->right < body->values.offset || in->right - body->values.offset >= body->values.count))) return false;
                const SolMirPredicateValue *left = &o->predicate_values[in->left];
                const SolMirPredicateValue *right = in->opcode == SOL_MIR_OPERATION_BOOL_NOT
                    || in->opcode == SOL_MIR_OPERATION_I64_NEG ? NULL : &o->predicate_values[in->right];
                SolMirRecipeKind left_kind = concrete->representation.recipes[left->recipe].kind;
                SolMirRecipeKind right_kind = right == NULL ? SOL_MIR_RECIPE_NEVER
                    : concrete->representation.recipes[right->recipe].kind;
                SolMirRecipeKind result_kind = concrete->representation.recipes[in->recipe].kind;
                switch (in->opcode) {
                    case SOL_MIR_OPERATION_BOOL_NOT:
                        if (left_kind != SOL_MIR_RECIPE_BOOL || result_kind != SOL_MIR_RECIPE_BOOL) return false;
                        break;
                    case SOL_MIR_OPERATION_I64_NEG:
                        if (left_kind != SOL_MIR_RECIPE_INT64 || result_kind != SOL_MIR_RECIPE_INT64) return false;
                        break;
                    case SOL_MIR_OPERATION_I64_ADD: case SOL_MIR_OPERATION_I64_SUB:
                    case SOL_MIR_OPERATION_I64_MUL: case SOL_MIR_OPERATION_I64_DIV:
                    case SOL_MIR_OPERATION_I64_REM:
                        if (left_kind != SOL_MIR_RECIPE_INT64 || right_kind != SOL_MIR_RECIPE_INT64
                            || result_kind != SOL_MIR_RECIPE_INT64) return false;
                        break;
                    case SOL_MIR_OPERATION_I64_LT: case SOL_MIR_OPERATION_I64_LE:
                    case SOL_MIR_OPERATION_I64_GT: case SOL_MIR_OPERATION_I64_GE:
                        if (left_kind != SOL_MIR_RECIPE_INT64 || right_kind != SOL_MIR_RECIPE_INT64
                            || result_kind != SOL_MIR_RECIPE_BOOL) return false;
                        break;
                    case SOL_MIR_OPERATION_BOOL_AND: case SOL_MIR_OPERATION_BOOL_OR:
                        if (left_kind != SOL_MIR_RECIPE_BOOL || right_kind != SOL_MIR_RECIPE_BOOL
                            || result_kind != SOL_MIR_RECIPE_BOOL) return false;
                        break;
                    case SOL_MIR_OPERATION_VALUE_EQ: case SOL_MIR_OPERATION_VALUE_NE:
                        if (left->recipe != right->recipe || result_kind != SOL_MIR_RECIPE_BOOL
                            || (left_kind != SOL_MIR_RECIPE_INT64 && left_kind != SOL_MIR_RECIPE_BOOL
                                && left_kind != SOL_MIR_RECIPE_UNIT)) return false;
                        break;
                    default: return false;
                }
            }
            if (in->failures != SOL_MIR_OPERATION_FAILURE_NONE) {
                const SolMirRuntimeLoweredPredicateInstruction *instruction = &owner->predicate_instructions[id];
                if (instruction->cleanup_event >= owner->cleanup->event_count
                    || instruction->failure_site >= owner->conventions->failure_site_count) return false;
                const SolMirRuntimeCleanupEvent *event = &owner->cleanup->events[instruction->cleanup_event];
                const SolMirRuntimeFailureSite *site = &owner->conventions->failure_sites[instruction->failure_site];
                if (event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION
                    || event->owner != body_id || event->block != b || event->operation != id
                    || event->inherited_failure_site != instruction->failure_site
                    || !event->captures_failure_detail
                    || event->capture_detail_kind != SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE
                    || site->origin_kind != SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC
                    || site->owner != body_id || site->block != b || site->instruction != id
                    || site->allowed_codes != represented_failure_mask(in->failures)) return false;
            }
        }
        const SolMirPredicateTerminator *term = &block->terminator;
        const SolMirRuntimeLoweredPredicateTerminator *lowered_terminator
            = &owner->predicate_terminators[b];
        if (!represented_step_route(request,
                SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR, body_id, b,
                SOL_MIR_RUNTIME_NONE, lowered_terminator->step_cleanup_event,
                lowered_terminator->step_failure_site)) return false;
        if (term->kind == SOL_MIR_PREDICATE_TERM_RETURN) {
            if (term->value < body->values.offset || term->value - body->values.offset >= body->values.count
                || o->predicate_values[term->value].recipe != body->output_recipe) return false;
        } else if (term->kind == SOL_MIR_PREDICATE_TERM_JUMP) {
            if (term->edge >= o->predicate_edge_count || o->predicate_edges[term->edge].source != b) return false;
        } else if (term->kind == SOL_MIR_PREDICATE_TERM_BRANCH) {
            if (term->condition < body->values.offset || term->condition - body->values.offset >= body->values.count
                || term->true_edge >= o->predicate_edge_count || term->false_edge >= o->predicate_edge_count
                || o->predicate_edges[term->true_edge].source != b
                || o->predicate_edges[term->false_edge].source != b) return false;
        } else return false;
        for (size_t edge_id = 0; edge_id < o->predicate_edge_count; ++edge_id) {
            const SolMirPredicateEdge *edge = &o->predicate_edges[edge_id];
            if (edge->source != b) continue;
            if (edge_id >= owner->predicate_edge_count || edge->target < body->blocks.offset
                || edge->target - body->blocks.offset >= body->blocks.count
                || edge->arguments.offset > o->predicate_edge_value_count
                || edge->arguments.count > o->predicate_edge_value_count - edge->arguments.offset
                || edge->arguments.count != o->predicate_blocks[edge->target].parameters.count
                || owner->predicate_edges[edge_id].state != SOL_MIR_RUNTIME_LOWERED_PRESENT
                || owner->predicate_edges[edge_id].body != body_id
                || owner->predicate_edges[edge_id].edge != edge_id
                || owner->predicate_edges[edge_id].source != b
                || owner->predicate_edges[edge_id].target != edge->target) return false;
            for (size_t i = 0; i < edge->arguments.count; ++i) {
                SolMirPredicateValueId source = o->predicate_edge_values[edge->arguments.offset + i];
                SolMirPredicateValueId destination = o->predicate_blocks[edge->target].parameters.offset + i;
                if (source < body->values.offset || source - body->values.offset >= body->values.count
                    || destination < body->values.offset || destination - body->values.offset >= body->values.count
                    || o->predicate_values[source].recipe != o->predicate_values[destination].recipe) return false;
            }
        }
    }
    return true;
}

typedef struct {
    const SolMirOperationPredicatePlan *plan;
    const SolMirRuntimeCleanupEvent *event;
    const SolMirRuntimeCleanupTransition *satisfied;
    const SolMirRuntimeCleanupTransition *violation;
    const SolMirRuntimeCleanupTransition *failure;
    size_t violation_site;
} RepresentedRefinementCheck;

/* Authenticate the repaired P2/P3/P3.6 three-way join.  Both a false result and
 * predicate evaluation failure use the materialized failure edge, but their
 * packet authority is deliberately distinct: code 15 belongs to the inherited
 * P3.1 site while arithmetic code 2/3 must already be pending. */
static bool represented_refinement_check(const SolWasmRepresentedBuildRequest *request,
    const RepresentedQualifiedIndex *index, size_t image, size_t block,
    RepresentedRefinementCheck *out) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirConcreteProgram *concrete = owner->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirOperations *o = &concrete->operations;
    const SolMirRuntimeCleanup *cleanup = owner->cleanup;
    if (out == NULL || block >= m->block_count || block >= owner->image_terminator_count)
        return false;
    const SolMirMaterializedTerminator *term = &m->blocks[block].terminator;
    const SolMirRuntimeLoweredImageTerminator *row = &owner->image_terminators[block];
    if (term->kind != SOL_MIR_TERM_CHECK_REFINED || row->state
            != SOL_MIR_RUNTIME_LOWERED_PRESENT || row->execution
            != SOL_MIR_RUNTIME_LOWERED_EXECUTABLE || row->image != image
        || row->block != block || row->kind != term->kind
        || row->runtime_class != SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE
        || row->plan_family != SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE
        || row->facilities != (SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP
            | SOL_MIR_RUNTIME_LOWERED_FACILITY_EVENT
            | SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE)
        || row->plan >= owner->semantic_plan_count || row->cleanup_event >= cleanup->event_count
        || row->failure_site >= owner->conventions->failure_site_count) return false;
    const SolMirRuntimeLoweredSemanticPlan *semantic = &owner->semantic_plans[row->plan];
    if (semantic->state != SOL_MIR_RUNTIME_LOWERED_PRESENT
        || semantic->arena != SOL_MIR_RUNTIME_LOWERED_SEMANTIC_PREDICATE
        || semantic->runtime_class != SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE
        || semantic->plan_family != SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE
        || semantic->facilities != (SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE
            | SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP)
        || semantic->plan >= o->predicate_count
        || semantic->producer_kind != SOL_MIR_MATERIALIZED_PRODUCER_PREDICATE)
        return false;
    const SolMirOperationPredicatePlan *plan = &o->predicates[semantic->plan];
    size_t indexed_plan = SOL_MIR_OPERATION_NONE;
    if (plan->kind != SOL_MIR_OPERATION_PREDICATE_REFINEMENT || plan->image != image
        || plan->block != block || plan->body != semantic->producer
        || plan->body >= o->predicate_body_count || plan->representation != term->representation
        || plan->result != term->result || plan->context >= m->context_count
        || term->normal_edge == term->failure_edge
        || term->normal_edge >= m->edge_count || term->failure_edge >= m->edge_count
        || m->edges[term->normal_edge].block >= m->block_count
        || m->edges[term->failure_edge].block >= m->block_count
        || m->edges[term->normal_edge].arguments.count != 1
        || m->edges[term->normal_edge].arguments.offset >= m->edge_value_count
        || m->edge_values[m->edges[term->normal_edge].arguments.offset] != term->result
        || m->edges[term->failure_edge].arguments.count != 0
        || represented_refinement_plan_for_body(request, index, image, plan->body,
            &indexed_plan) != plan || indexed_plan != semantic->plan) return false;
    const SolMirPredicateBody *body = &o->predicate_bodies[plan->body];
    const SolMirPlanContext *context = &m->contexts[plan->context];
    if (context->obligation >= concrete->program.ir->obligation_count
        || body->blocks.offset > o->predicate_block_count
        || body->blocks.count > o->predicate_block_count - body->blocks.offset) return false;
    const SolIrObligation *obligation = &concrete->program.ir->obligations[context->obligation];
    size_t return_block = SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; i < body->blocks.count; ++i) {
        size_t candidate = body->blocks.offset + i;
        if (o->predicate_blocks[candidate].body != plan->body) return false;
        if (o->predicate_blocks[candidate].terminator.kind != SOL_MIR_PREDICATE_TERM_RETURN)
            continue;
        if (return_block != SOL_MIR_RUNTIME_NONE) return false;
        return_block = candidate;
    }
    SolMirRuntimeSource expected_source, expected_event_source;
    if (return_block == SOL_MIR_RUNTIME_NONE
        || obligation->predicate >= concrete->program.ir->expression_count
        || !represented_runtime_source(concrete->program.ir,
            concrete->program.ir->expressions[obligation->predicate].span, &expected_source)
        || !represented_runtime_source(concrete->program.ir, term->span, &expected_event_source))
        return false;
    const SolMirRuntimeCleanupEvent *event = &cleanup->events[row->cleanup_event];
    const uint32_t violation_mask = UINT32_C(1)
        << (SOL_MIR_RUNTIME_FAILURE_REFINEMENT_VIOLATION - 1);
    const SolMirRuntimeFailureSite *site = &owner->conventions->failure_sites[row->failure_site];
    if (event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
        || event->phase != SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
        || event->origin != SOL_MIR_RUNTIME_CLEANUP_ORIGIN_EXPLICIT
        || event->owner != image || event->block != block
        || event->operation != SOL_MIR_RUNTIME_NONE
        || event->producer != SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL
        || event->inherited_failure_site != row->failure_site
        || event->supplemental_site != SOL_MIR_RUNTIME_NONE
        || !event->captures_failure_detail
        || event->capture_detail_kind != SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE
        || event->transitions.count != 3
        || event->transitions.offset > cleanup->transition_count
        || event->transitions.count > cleanup->transition_count - event->transitions.offset
        || site->origin_kind != SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT
        || site->owner != plan->body || site->block != return_block
        || site->instruction != SOL_MIR_RUNTIME_NONE
        || site->source.file != expected_source.file
        || site->source.start != expected_source.start || site->source.end != expected_source.end
        || event->source.file != expected_event_source.file
        || event->source.start != expected_event_source.start
        || event->source.end != expected_event_source.end || site->allowed_codes != violation_mask)
        return false;
    const SolMirRuntimeCleanupTransition *satisfied = NULL, *violation = NULL, *failure = NULL;
    for (size_t i = 0; i < event->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *candidate = &cleanup->transitions[
            event->transitions.offset + i];
        if (candidate->event != row->cleanup_event
            || candidate->actions.offset > cleanup->action_count
            || candidate->actions.count > cleanup->action_count - candidate->actions.offset
            || !candidate->primary_failure_wins) return false;
        for (size_t a = 0; a < candidate->actions.count; ++a)
            if (!represented_cleanup_action(request,
                    &cleanup->actions[candidate->actions.offset + a])) return false;
        if (candidate->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_SATISFIED) {
            if (satisfied != NULL || candidate->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL
                || candidate->continuation != term->normal_edge
                || candidate->source_edge != term->normal_edge
                || candidate->destination != m->edges[term->normal_edge].block
                || candidate->failure_source != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE
                || candidate->failure_site != SOL_MIR_RUNTIME_NONE
                || candidate->failure_mask != 0) return false;
            satisfied = candidate;
        } else if (candidate->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_VIOLATION) {
            if (violation != NULL || candidate->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
                || candidate->continuation != term->failure_edge
                || candidate->source_edge != term->failure_edge
                || candidate->destination != m->edges[term->failure_edge].block
                || candidate->failure_source
                    != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31
                || candidate->failure_site != row->failure_site
                || candidate->failure_mask != violation_mask) return false;
            violation = candidate;
        } else if (candidate->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_FAILURE) {
            if (failure != NULL || candidate->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
                || candidate->continuation != term->failure_edge
                || candidate->source_edge != term->failure_edge
                || candidate->destination != m->edges[term->failure_edge].block
                || candidate->failure_source != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING
                || candidate->failure_site != SOL_MIR_RUNTIME_NONE
                || candidate->failure_mask != 0) return false;
            failure = candidate;
        } else return false;
    }
    if (satisfied == NULL || violation == NULL || failure == NULL) return false;
    bool check_contract = false;
    if (event->actions.offset != satisfied->actions.offset
        || event->actions.count != satisfied->actions.count || satisfied->actions.count != 1
        || violation->actions.offset != event->actions.offset + event->actions.count
        || violation->actions.count != 0 || failure->actions.offset != violation->actions.offset
        || failure->actions.count != 0) return false;
    for (size_t i = 0; i < satisfied->actions.count; ++i) {
        const SolMirRuntimeCleanupAction *action = &cleanup->actions[satisfied->actions.offset + i];
        if (action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_CHECK_CONTRACT) {
            if (check_contract || action->target != term->source_obligation
                || action->flags != 0 || action->recipe != SOL_MIR_RECIPE_NONE
                || action->drop_path != SOL_MIR_RUNTIME_NONE) return false;
            check_contract = true;
        }
    }
    if (!check_contract) return false;
    *out = (RepresentedRefinementCheck){plan, event, satisfied, violation, failure,
        row->failure_site};
    return true;
}

static const SolMirOperationArithmeticPlan *arithmetic_for_instruction(
    const SolWasmRepresentedBuildRequest *request, size_t instruction) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirConcreteProgram *concrete = owner->conventions->concrete;
    if (instruction >= owner->image_instruction_count
        || instruction >= concrete->materialization.instruction_count) return NULL;
    const SolMirRuntimeLoweredImageInstruction *lowered = &owner->image_instructions[instruction];
    if (lowered->plan >= owner->semantic_plan_count) return NULL;
    const SolMirRuntimeLoweredSemanticPlan *semantic = &owner->semantic_plans[lowered->plan];
    if (semantic->state != SOL_MIR_RUNTIME_LOWERED_PRESENT
        || semantic->arena != SOL_MIR_RUNTIME_LOWERED_SEMANTIC_ARITHMETIC
        || semantic->plan >= concrete->operations.arithmetic_count) return NULL;
    const SolMirOperationArithmeticPlan *plan = &concrete->operations.arithmetic[semantic->plan];
    const SolMirMaterializedInstruction *item = &concrete->materialization.instructions[instruction];
    return plan->image == lowered->image && plan->instruction == instruction
        && plan->result == item->result ? plan : NULL;
}

static bool represented_callable_product_drop_path(const SolWasmRepresentedBuildRequest *request,
    const SolMirRuntimeCleanupAction *action) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirRuntimeCleanup *cleanup = request->program->cleanup;
    if (action->target >= m->place_count || m->places[action->target].projections.count != 0
        || m->places[action->target].final_type >= concrete->layout.type_count
        || !represented_callable_product_recipe(concrete,
            concrete->layout.types[m->places[action->target].final_type].recipe)) return true;
    if (action->drop_path >= cleanup->drop_path_count) return false;
    const SolMirRuntimeCleanupDropPath *path = &cleanup->drop_paths[action->drop_path];
    if (path->root != action->target || path->place != action->target
        || path->recipe != action->recipe || path->holes.count > 1
        || action->drop_path == SIZE_MAX || path->holes.offset != action->drop_path + 1
        || path->holes.offset > cleanup->drop_path_count
        || path->holes.count > cleanup->drop_path_count - path->holes.offset) return false;
    if (path->holes.count == 0)
        return path->liveness == SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE
            && (action->flags == 0 || action->flags == SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED);
    const SolMirRuntimeCleanupDropPath *hole = &cleanup->drop_paths[path->holes.offset];
    if (path->liveness != SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL
        || hole->root != path->root || hole->recipe != path->recipe
        || (hole->liveness != SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE
            && hole->liveness != SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL)
        || hole->place >= m->place_count || m->places[hole->place].projections.count != 1
        || m->places[hole->place].final_type >= concrete->layout.type_count
        || !represented_unbound_function_recipe(concrete,
            concrete->layout.types[m->places[hole->place].final_type].recipe)) return false;
    size_t projection = m->places[hole->place].projections.offset;
    if (projection >= concrete->layout.projection_count) return false;
    const SolMirProjectionMap *map = &concrete->layout.projections[projection];
    return map->place == hole->place && map->base_recipe == action->recipe
        && map->field_layout < concrete->layout.field_count
        && map->field_layout < concrete->representation.field_count
        && map->result_recipe == concrete->representation.fields[map->field_layout].type
        && map->object_offset == concrete->layout.fields[map->field_layout].offset;
}

static const SolMirRuntimeLoweredCleanupFailure *represented_cleanup_row(
    const SolMirRuntimeLoweredProgram *owner, SolMirRuntimeLoweredCleanupKind kind,
    size_t record) {
    size_t index = record;
    if (kind == SOL_MIR_RUNTIME_LOWERED_CLEANUP_ACTION) {
        if (record > SIZE_MAX - owner->cleanup->event_count) return NULL;
        index = owner->cleanup->event_count + record;
    } else if (kind == SOL_MIR_RUNTIME_LOWERED_CLEANUP_TRANSITION) {
        if (owner->cleanup->event_count > SIZE_MAX - owner->cleanup->action_count
            || record > SIZE_MAX - owner->cleanup->event_count - owner->cleanup->action_count)
            return NULL;
        index = owner->cleanup->event_count + owner->cleanup->action_count + record;
    } else if (kind != SOL_MIR_RUNTIME_LOWERED_CLEANUP_EVENT) return NULL;
    if (index >= owner->cleanup_failure_count) return NULL;
    const SolMirRuntimeLoweredCleanupFailure *row = &owner->cleanup_failures[index];
    return row->state == SOL_MIR_RUNTIME_LOWERED_PRESENT && row->kind == kind
        && row->record == record ? row : NULL;
}

static bool represented_cleanup_event_image(const SolWasmRepresentedBuildRequest *request,
    const SolMirRuntimeCleanupEvent *event, size_t *image) {
    const SolMirOperations *o = &request->program->conventions->concrete->operations;
    if (event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION
        || event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR) {
        *image = event->owner; return true;
    }
    if ((event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION
            || event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR)
        && event->owner < o->predicate_body_count
        && o->predicate_bodies[event->owner].owner_kind == SOL_MIR_PREDICATE_OWNER_INSTANCE) {
        *image = o->predicate_bodies[event->owner].instance; return true;
    }
    return false;
}

static bool represented_snapshot_cleanup_action(const SolWasmRepresentedBuildRequest *request,
    const SolMirRuntimeCleanupAction *action) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirRuntimeCleanup *cleanup = owner->cleanup;
    const SolMirOperations *o = &owner->conventions->concrete->operations;
    if (action < cleanup->actions || action >= cleanup->actions + cleanup->action_count)
        return false;
    size_t action_id = (size_t)(action - cleanup->actions);
    if (action->flags != 0 || action->drop_path != SOL_MIR_RUNTIME_NONE
        || action->target >= o->snapshot_count) return false;
    const SolMirOperationSnapshotPlan *plan = &o->snapshots[action->target];
    size_t authenticated = SOL_MIR_OPERATION_NONE;
    if (action->recipe != plan->recipe
        || represented_snapshot_for_instruction(request, plan->instruction, &authenticated) != plan
        || authenticated != action->target) return false;
    const SolMirRuntimeLoweredCleanupFailure *action_row = represented_cleanup_row(owner,
        SOL_MIR_RUNTIME_LOWERED_CLEANUP_ACTION, action_id);
    if (action_row == NULL || action_row->action != action_id
        || action_row->action_kind != SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT
        || action_row->action_flags != 0 || action_row->target != action->target
        || action_row->recipe != action->recipe
        || action_row->drop_path != SOL_MIR_RUNTIME_NONE) return false;
    size_t matches = 0;
    for (size_t i = 0; i < cleanup->transition_count; ++i) {
        const SolMirRuntimeCleanupTransition *transition = &cleanup->transitions[i];
        if (transition->actions.offset > cleanup->action_count
            || transition->actions.count > cleanup->action_count - transition->actions.offset
            || action_id < transition->actions.offset
            || action_id - transition->actions.offset >= transition->actions.count) continue;
        if (transition->event >= cleanup->event_count) return false;
        const SolMirRuntimeCleanupEvent *event = &cleanup->events[transition->event];
        size_t image = SOL_MIR_RUNTIME_NONE;
        if (!represented_cleanup_event_image(request, event, &image) || image != plan->image
            || event->actions.offset > cleanup->action_count
            || event->actions.count > cleanup->action_count - event->actions.offset
            || action_id < event->actions.offset
            || action_id - event->actions.offset >= event->actions.count) return false;
        const SolMirRuntimeLoweredCleanupFailure *event_row = represented_cleanup_row(owner,
            SOL_MIR_RUNTIME_LOWERED_CLEANUP_EVENT, transition->event);
        const SolMirRuntimeLoweredCleanupFailure *transition_row = represented_cleanup_row(owner,
            SOL_MIR_RUNTIME_LOWERED_CLEANUP_TRANSITION, i);
        if (event_row == NULL || event_row->event != transition->event
            || event_row->actions.offset != event->actions.offset
            || event_row->actions.count != event->actions.count
            || event_row->transitions.offset != event->transitions.offset
            || event_row->transitions.count != event->transitions.count
            || transition_row == NULL || transition_row->transition != i
            || transition_row->event != transition->event
            || transition_row->actions.offset != transition->actions.offset
            || transition_row->actions.count != transition->actions.count) return false;
        ++matches;
    }
    return matches == 1;
}

static bool represented_cleanup_action(const SolWasmRepresentedBuildRequest *request,
    const SolMirRuntimeCleanupAction *action) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    if ((action->flags & ~((unsigned)SOL_MIR_RUNTIME_CLEANUP_ACTION_NORMAL_ONLY
            | SOL_MIR_RUNTIME_CLEANUP_ACTION_FAILURE_ONLY
            | SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED)) != 0) return false;
    switch (action->kind) {
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_WRITEBACK:
            return action->flags == SOL_MIR_RUNTIME_CLEANUP_ACTION_NORMAL_ONLY
                && action->drop_path == SOL_MIR_RUNTIME_NONE
                && whole_represented_place(concrete, action->target)
                && action->target < concrete->materialization.place_count
                && concrete->materialization.places[action->target].final_type
                    < concrete->layout.type_count
                && concrete->layout.types[concrete->materialization.places[action->target].final_type]
                    .recipe == action->recipe && action->recipe < concrete->representation.recipe_count
                && concrete->representation.recipes[action->recipe].kind == SOL_MIR_RECIPE_INT64;
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_TEMPORARY:
            return action->target < concrete->materialization.temporary_count
                && represented_recipe(concrete,
                    concrete->materialization.temporaries[action->target].type, false);
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE:
            return (whole_represented_place(concrete, action->target)
                    && represented_callable_product_drop_path(request, action))
                || represented_projected_pre_store_cleanup_action(request, action);
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER:
            return action->target < concrete->materialization.local_count
                && represented_recipe(concrete,
                    concrete->materialization.locals[action->target].type, false);
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT:
            return represented_snapshot_cleanup_action(request, action);
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_SCOPE:
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_REGION:
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_HANDLER:
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE:
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_CHECK_CONTRACT:
            return true;
        default: return false;
    }
}

static bool represented_step_route(const SolWasmRepresentedBuildRequest *request,
    SolMirRuntimeCleanupEventKind kind, size_t owner_id, size_t block, size_t operation,
    size_t event_id, size_t site_id) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirRuntimeConventions *conventions = owner->conventions;
    const SolMirRuntimeCleanup *cleanup = owner->cleanup;
    const SolMirConcreteProgram *concrete = conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirOperations *operations = &concrete->operations;
    bool predicate = kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION
        || kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR;
    SolMirRuntimeFailureOriginKind origin = predicate
        ? SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_STEP
        : SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_STEP;
    SolSpan span;
    if (kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION) {
        if (operation >= m->instruction_count || m->instructions[operation].block != block)
            return false;
        span = m->instructions[operation].span;
    } else if (kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR) {
        if (operation != SOL_MIR_RUNTIME_NONE || block >= m->block_count) return false;
        span = m->blocks[block].terminator.span;
    } else {
        if (owner_id >= operations->predicate_body_count
            || block >= operations->predicate_block_count
            || operations->predicate_blocks[block].body != owner_id) return false;
        const SolMirPredicateBody *body = &operations->predicate_bodies[owner_id];
        if (body->context >= m->context_count) return false;
        size_t obligation = m->contexts[body->context].obligation;
        if (obligation >= concrete->program.ir->obligation_count
            || concrete->program.ir->obligations[obligation].predicate
                >= concrete->program.ir->expression_count) return false;
        span = concrete->program.ir->expressions[
            concrete->program.ir->obligations[obligation].predicate].span;
        if (kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION) {
            if (operation >= operations->predicate_instruction_count
                || operations->predicate_instructions[operation].block != block) return false;
        } else if (kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR
            || operation != SOL_MIR_RUNTIME_NONE) return false;
    }
    SolMirRuntimeSource source;
    uint32_t mask = UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_STEP_LIMIT - 1);
    if (event_id >= cleanup->event_count || site_id >= conventions->failure_site_count
        || !represented_runtime_source(concrete->program.ir, span, &source)) return false;
    const SolMirRuntimeCleanupEvent *event = &cleanup->events[event_id];
    const SolMirRuntimeFailureSite *site = &conventions->failure_sites[site_id];
    if (event->kind != kind || event->phase != SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_STEP
        || event->origin != SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT
        || event->owner != owner_id || event->block != block || event->operation != operation
        || event->semantic_site != SOL_MIR_RUNTIME_NONE
        || event->inherited_failure_site != site_id
        || event->supplemental_site != SOL_MIR_RUNTIME_NONE
        || event->producer != SOL_MIR_RUNTIME_CLEANUP_PRODUCER_STEP_METER
        || !event->captures_failure_detail
        || event->capture_detail_kind != SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE
        || event->source.file != source.file || event->source.start != source.start
        || event->source.end != source.end || site->origin_kind != origin
        || site->owner != owner_id || site->block != block || site->instruction != operation
        || site->source.file != source.file || site->source.start != source.start
        || site->source.end != source.end || site->occurrence != site_id
        || site->allowed_codes != mask || event->transitions.count != 2
        || event->transitions.offset > cleanup->transition_count
        || event->transitions.count > cleanup->transition_count - event->transitions.offset
        || event->actions.offset > cleanup->action_count
        || event->actions.count > cleanup->action_count - event->actions.offset) return false;
    const SolMirRuntimeCleanupTransition *ready = &cleanup->transitions[event->transitions.offset];
    const SolMirRuntimeCleanupTransition *failure = &cleanup->transitions[event->transitions.offset + 1];
    SolContractClauseKind phase = SOL_CONTRACT_REQUIRES;
    SolContractOutcomeKind outcome = SOL_CONTRACT_OUTCOME_SUCCESS;
    if (kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR) {
        phase = m->blocks[block].terminator.contract_phase;
        outcome = m->blocks[block].terminator.contract_outcome;
    } else if (predicate) {
        phase = operations->predicate_bodies[owner_id].phase;
        outcome = operations->predicate_bodies[owner_id].outcome;
    }
#define STEP_TRANSITION_COMMON(t) \
    ((t)->event == event_id && (t)->continuation == SOL_MIR_RUNTIME_NONE \
        && (t)->source_edge == SOL_MIR_RUNTIME_NONE \
        && (t)->destination == SOL_MIR_RUNTIME_NONE && (t)->primary_failure_wins \
        && (t)->contract_phase == phase && (t)->contract_outcome == outcome)
    if (!STEP_TRANSITION_COMMON(ready)
        || ready->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL
        || ready->edge_role != SOL_MIR_RUNTIME_CLEANUP_EDGE_PRE_OPERATION_READY
        || ready->failure_source != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE
        || ready->failure_site != SOL_MIR_RUNTIME_NONE || ready->failure_mask != 0
        || ready->actions.offset != event->actions.offset || ready->actions.count != 0
        || !STEP_TRANSITION_COMMON(failure)
        || failure->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
        || failure->edge_role != SOL_MIR_RUNTIME_CLEANUP_EDGE_STEP_FAILURE
        || failure->failure_source != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31
        || failure->failure_site != site_id || failure->failure_mask != mask
        || failure->actions.offset != event->actions.offset
        || failure->actions.count != event->actions.count
        || (predicate ? failure->actions.count != 0 : failure->actions.count == 0)) return false;
#undef STEP_TRANSITION_COMMON
    const SolMirRuntimeLoweredCleanupFailure *event_row = represented_cleanup_row(owner,
        SOL_MIR_RUNTIME_LOWERED_CLEANUP_EVENT, event_id);
    const SolMirRuntimeLoweredCleanupFailure *ready_row = represented_cleanup_row(owner,
        SOL_MIR_RUNTIME_LOWERED_CLEANUP_TRANSITION, event->transitions.offset);
    const SolMirRuntimeLoweredCleanupFailure *failure_row = represented_cleanup_row(owner,
        SOL_MIR_RUNTIME_LOWERED_CLEANUP_TRANSITION, event->transitions.offset + 1);
    if (event_row == NULL || event_row->event != event_id || event_row->event_kind != kind
        || event_row->phase != event->phase || event_row->producer != event->producer
        || event_row->inherited_failure_site != site_id
        || event_row->actions.offset != event->actions.offset
        || event_row->actions.count != event->actions.count
        || event_row->transitions.offset != event->transitions.offset
        || event_row->transitions.count != event->transitions.count
        || ready_row == NULL || ready_row->event != event_id
        || ready_row->edge_role != ready->edge_role || ready_row->outcome != ready->outcome
        || failure_row == NULL || failure_row->event != event_id
        || failure_row->edge_role != failure->edge_role
        || failure_row->failure_site != site_id || failure_row->failure_mask != mask)
        return false;
    for (size_t i = 0; i < failure->actions.count; ++i) {
        size_t action_id = failure->actions.offset + i;
        const SolMirRuntimeCleanupAction *action = &cleanup->actions[action_id];
        const SolMirRuntimeLoweredCleanupFailure *action_row = represented_cleanup_row(owner,
            SOL_MIR_RUNTIME_LOWERED_CLEANUP_ACTION, action_id);
        if (!represented_cleanup_action(request, action) || action_row == NULL
            || action_row->action != action_id
            || action_row->action_kind != action->kind || action_row->action_flags != action->flags
            || action_row->target != action->target || action_row->recipe != action->recipe
            || action_row->drop_path != action->drop_path) return false;
        bool propagate = action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE;
        if (propagate != (i + 1 == failure->actions.count)
            || (propagate && (action->flags != SOL_MIR_RUNTIME_CLEANUP_ACTION_FAILURE_ONLY
                || action->target != site_id || action->recipe != SOL_MIR_RECIPE_NONE
                || action->drop_path != SOL_MIR_RUNTIME_NONE))) return false;
    }
    return true;
}

static bool represented_failure_route(const SolWasmRepresentedBuildRequest *request,
    size_t instruction, const SolMirOperationArithmeticPlan *plan) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirRuntimeCleanup *cleanup = owner->cleanup;
    const SolMirRuntimeConventions *conventions = owner->conventions;
    const SolMirMaterialization *m = &conventions->concrete->materialization;
    if (instruction >= owner->image_instruction_count || instruction >= m->instruction_count)
        return false;
    const SolMirRuntimeLoweredImageInstruction *row = &owner->image_instructions[instruction];
    uint32_t allowed = represented_failure_mask(plan->failures);
    if (allowed == 0 || row->cleanup_event >= cleanup->event_count
        || row->failure_site >= conventions->failure_site_count) return false;
    const SolMirRuntimeCleanupEvent *event = &cleanup->events[row->cleanup_event];
    const SolMirRuntimeFailureSite *site = &conventions->failure_sites[row->failure_site];
    if (event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION
        || event->owner != row->image || event->block != row->block
        || event->operation != instruction || !event->captures_failure_detail
        || event->capture_detail_kind != SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE
        || event->inherited_failure_site != row->failure_site
        || site->origin_kind != SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_ARITHMETIC
        || site->owner != row->image || site->block != row->block
        || site->instruction != instruction || site->allowed_codes != allowed
        || event->transitions.offset > cleanup->transition_count
        || event->transitions.count > cleanup->transition_count - event->transitions.offset)
        return false;
    size_t normal = 0, matches = 0;
    for (size_t i = 0; i < event->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *transition = &cleanup->transitions[
            event->transitions.offset + i];
        if (transition->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL) {
            if (transition->event != row->cleanup_event || ++normal != 1) return false;
            continue;
        }
        if (transition->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE) return false;
        if (transition->event != row->cleanup_event
            || transition->failure_source != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31
            || transition->failure_site != row->failure_site || transition->failure_mask != allowed
            || transition->continuation != SOL_MIR_RUNTIME_NONE
            || transition->actions.offset > cleanup->action_count
            || transition->actions.count > cleanup->action_count - transition->actions.offset)
            return false;
        bool propagate = false;
        for (size_t a = 0; a < transition->actions.count; ++a) {
            const SolMirRuntimeCleanupAction *action = &cleanup->actions[
                transition->actions.offset + a];
            if (!represented_cleanup_action(request, action)) return false;
            if (action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE) {
                if (a + 1 != transition->actions.count || propagate) return false;
                propagate = true;
            } else if (propagate) return false;
        }
        if (!propagate || ++matches != 1) return false;
    }
    return normal == 1 && matches == 1;
}

/* P3.3 owns the allocation occurrence.  This gate deliberately accepts no
 * inferred source span or generic fallback: a physical Text allocation must
 * name one AT_OPERATION supplemental site that permits exactly codes 4 and 5. */
static bool represented_text_allocation_route(const SolWasmRepresentedBuildRequest *request,
    size_t instruction, size_t *supplemental) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirRuntimeCleanup *cleanup = owner->cleanup;
    const SolMirRuntimeConventions *conventions = owner->conventions;
    if (instruction >= owner->image_instruction_count) return false;
    const SolMirRuntimeLoweredImageInstruction *row = &owner->image_instructions[instruction];
    if (row->cleanup_event >= cleanup->event_count) return false;
    const SolMirRuntimeCleanupEvent *event = &cleanup->events[row->cleanup_event];
    uint32_t allowed = (UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED - 1))
        | (UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_ALLOCATION_LIMIT - 1));
    if (event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION
        || event->phase != SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
        || event->owner != row->image || event->block != row->block
        || event->operation != instruction || event->producer
            != SOL_MIR_RUNTIME_CLEANUP_PRODUCER_SUPPLEMENTAL_ALLOCATION
        || event->supplemental_site >= cleanup->supplemental_site_count
        || event->inherited_failure_site != SOL_MIR_RUNTIME_NONE
        || !event->captures_failure_detail || event->capture_detail_kind
            != SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE) return false;
    const SolMirRuntimeCleanupSupplementalSite *site =
        &cleanup->supplemental_sites[event->supplemental_site];
    if (site->event != row->cleanup_event || site->allowed_codes != allowed
        || event->transitions.offset > cleanup->transition_count
        || event->transitions.count > cleanup->transition_count - event->transitions.offset)
        return false;
    size_t normal = 0, failure = 0;
    for (size_t i = 0; i < event->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *transition = &cleanup->transitions[
            event->transitions.offset + i];
        if (transition->event != row->cleanup_event) return false;
        if (transition->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL) {
            if (++normal != 1) return false;
            continue;
        }
        if (transition->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
            || transition->failure_source != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_SUPPLEMENTAL_P33
            || transition->failure_site != event->supplemental_site
            || transition->failure_mask != allowed || transition->continuation != SOL_MIR_RUNTIME_NONE
            || transition->actions.offset > cleanup->action_count
            || transition->actions.count > cleanup->action_count - transition->actions.offset
            || ++failure != 1) return false;
        bool propagate = false;
        for (size_t a = 0; a < transition->actions.count; ++a) {
            const SolMirRuntimeCleanupAction *action = &cleanup->actions[
                transition->actions.offset + a];
            if (!represented_cleanup_action(request, action)) return false;
            if (action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE) {
                if (propagate || a + 1 != transition->actions.count) return false;
                propagate = true;
            } else if (propagate) return false;
        }
        if (!propagate) return false;
    }
    if (normal != 1 || failure != 1 || row->failure_site != SOL_MIR_RUNTIME_NONE
        || event->supplemental_site >= cleanup->supplemental_site_count) return false;
    *supplemental = event->supplemental_site;
    (void)conventions;
    return true;
}

/* P2/P3.6 already authenticate dominance and definite-initialization.  This
 * bridge deliberately does not reimplement that fixed point: it proves that
 * every physical local/get/set selected below is the same image-local represented
 * node that P2 authenticated. */
static bool image_value(const SolMirConcreteProgram *concrete,
    const SolMirMaterializedImage *image, SolMirMaterializedValueId value) {
    return value >= image->values.offset && value - image->values.offset < image->values.count
        && value < concrete->materialization.value_count
        && represented_recipe(concrete, concrete->materialization.values[value].type, false);
}

static bool image_place(const SolMirConcreteProgram *concrete,
    const SolMirMaterializedImage *image, SolMirMaterializedPlaceId place) {
    const SolMirMaterialization *m = &concrete->materialization;
    if (place >= m->place_count || !represented_place(concrete, place)) return false;
    const SolMirMaterializedPlace *item = &m->places[place];
    return item->local >= image->locals.offset
        && item->local - image->locals.offset < image->locals.count
        && (item->projections.count != 0 || item->final_type == m->locals[item->local].type);
}

static bool image_edge_preflight(const SolWasmRepresentedBuildRequest *request,
    const SolMirMaterializedImage *image, size_t image_id, size_t source, size_t edge) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    if (edge >= m->edge_count || source >= m->block_count
        || request->program->image_edges[edge].image != image_id
        || request->program->image_edges[edge].source != source) return false;
    const SolMirMaterializedEdge *item = &m->edges[edge];
    if (item->block < image->blocks.offset || item->block - image->blocks.offset >= image->blocks.count
        || item->arguments.offset > m->edge_value_count) return false;
    const SolMirMaterializedBlock *target = &m->blocks[item->block];
    if (item->arguments.count != target->parameters.count
        || item->arguments.count > m->edge_value_count - item->arguments.offset
        || target->parameters.offset > m->parameter_value_count
        || target->parameters.count > m->parameter_value_count - target->parameters.offset) return false;
    for (size_t i = 0; i < item->arguments.count; ++i) {
        SolMirMaterializedValueId argument = m->edge_values[item->arguments.offset + i];
        SolMirMaterializedValueId parameter = m->parameter_values[target->parameters.offset + i];
        if (!image_value(concrete, image, argument) || !image_value(concrete, image, parameter)
            || m->values[argument].type != m->values[parameter].type) return false;
    }
    return true;
}

/* Normal image control is not an inferred CFG convenience. P3.3 names the
 * one empty cleanup transition which owns each P2 edge (or return exit). P3
 * controls edge identity; P2 alone assigns edge arguments. Any normal-control
 * action is deliberately deferred rather than silently reinterpreted here. */
static const SolMirRuntimeCleanupTransition *represented_control_transition(
    const SolWasmRepresentedBuildRequest *request, const SolMirMaterializedImage *image,
    const RepresentedQualifiedIndex *qualified_index, size_t image_id, size_t block,
    SolMirRuntimeCleanupEdgeRole role,
    SolMirRuntimeCleanupOutcome outcome, size_t edge, size_t expected_transitions) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirRuntimeCleanup *cleanup = owner->cleanup;
    const SolMirMaterialization *m = &owner->conventions->concrete->materialization;
    if (image == NULL || block >= m->block_count || block >= owner->image_terminator_count
        || image_id >= m->image_count || expected_transitions == 0) return NULL;
    const SolMirRuntimeLoweredImageTerminator *row = &owner->image_terminators[block];
    if (row->state != SOL_MIR_RUNTIME_LOWERED_PRESENT || row->image != image_id
        || row->block != block || row->kind != m->blocks[block].terminator.kind
        || row->cleanup_event >= cleanup->event_count) return NULL;
    const SolMirRuntimeCleanupEvent *event = &cleanup->events[row->cleanup_event];
    bool ensures_ingress = false;
    if (role == SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO && edge < m->edge_count
        && m->edges[edge].block == image->contract_epilogue
        && event->inherited_failure_site < owner->conventions->failure_site_count) {
        const SolMirRuntimeFailureSite *site = &owner->conventions->failure_sites[
            event->inherited_failure_site];
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        if (represented_test_control_transition_stats.indexed_lookups != SIZE_MAX)
            ++represented_test_control_transition_stats.indexed_lookups;
#endif
        const SolMirOperationPredicatePlan *qualified_plan =
            represented_qualified_plan_for_body(request, qualified_index, site->owner);
        ensures_ingress = qualified_plan != NULL
            && (qualified_plan->contract_outcome == SOL_CONTRACT_OUTCOME_SUCCESS
                || qualified_plan->contract_outcome == SOL_CONTRACT_OUTCOME_FAILURE)
            && represented_qualified_image(qualified_index, image_id,
                    qualified_plan->result_recipe) != NULL
            && qualified_plan->kind == SOL_MIR_OPERATION_PREDICATE_CONTRACT
            && qualified_plan->image == image_id
            && qualified_plan->block == image->contract_epilogue
            && qualified_plan->contract_phase == SOL_CONTRACT_ENSURES
            && site->origin_kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT
            && site->owner == qualified_plan->body
            && site->allowed_codes == (UINT32_C(1)
                << (SOL_MIR_RUNTIME_FAILURE_ENSURE_VIOLATION - 1));
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        if (ensures_ingress
            && represented_test_control_transition_stats.indexed_matches != SIZE_MAX)
            ++represented_test_control_transition_stats.indexed_matches;
#endif
        if (!ensures_ingress) {
            size_t matches = 0;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
            if (represented_test_control_transition_stats.legacy_fallback_scans != SIZE_MAX)
                ++represented_test_control_transition_stats.legacy_fallback_scans;
#endif
            /* Preserve the pre-C2b2 ALWAYS scan and every original join. */
            for (size_t i = 0;
                 i < owner->conventions->concrete->operations.predicate_count; ++i) {
                const SolMirOperationPredicatePlan *plan =
                    &owner->conventions->concrete->operations.predicates[i];
                if (plan->kind == SOL_MIR_OPERATION_PREDICATE_CONTRACT
                    && plan->image == image_id && plan->block == image->contract_epilogue
                    && plan->contract_phase == SOL_CONTRACT_ENSURES
                    && plan->contract_outcome == SOL_CONTRACT_OUTCOME_ALWAYS
                    && site->origin_kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT
                    && site->owner == plan->body
                    && site->allowed_codes == (UINT32_C(1)
                        << (SOL_MIR_RUNTIME_FAILURE_ENSURE_VIOLATION - 1))) ++matches;
            }
            ensures_ingress = matches == 1;
        }
    }
    if (event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
        || event->phase != SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
        || event->origin != SOL_MIR_RUNTIME_CLEANUP_ORIGIN_EXPLICIT
        || event->owner != image_id || event->block != block
        || event->operation != SOL_MIR_RUNTIME_NONE || event->semantic_site != SOL_MIR_RUNTIME_NONE
        || (event->inherited_failure_site != SOL_MIR_RUNTIME_NONE && !ensures_ingress)
        || event->supplemental_site != SOL_MIR_RUNTIME_NONE
        || event->producer != SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL
        || event->captures_failure_detail
        || event->capture_detail_kind != SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE
        || event->transitions.count != expected_transitions
        || event->transitions.offset > cleanup->transition_count
        || event->transitions.count > cleanup->transition_count - event->transitions.offset
        || event->actions.offset > cleanup->action_count
        || event->actions.count != 0) return NULL;
    const SolMirRuntimeCleanupTransition *selected = NULL;
    size_t action_at = event->actions.offset;
    for (size_t i = 0; i < event->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *candidate = &cleanup->transitions[
            event->transitions.offset + i];
        if (candidate->event != row->cleanup_event || candidate->outcome != outcome
            || !candidate->primary_failure_wins
            || candidate->failure_source != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE
            || candidate->failure_site != SOL_MIR_RUNTIME_NONE || candidate->failure_mask != 0
            || candidate->actions.offset != action_at || candidate->actions.count != 0) return NULL;
        if (candidate->edge_role != role) continue;
        if (selected != NULL || candidate->continuation != edge
            || candidate->source_edge != edge) return NULL;
        if (edge == SOL_MIR_RUNTIME_NONE) {
            if (candidate->destination != SOL_MIR_RUNTIME_NONE) return NULL;
        } else if (edge >= m->edge_count || candidate->destination != m->edges[edge].block) return NULL;
        selected = candidate;
    }
    return action_at == event->actions.offset ? selected : NULL;
}

/* Snapshot-owning returns are the sole normal image-control form with actions
 * in C2b1.  Select that EXIT/RETURN transition directly and require the exact
 * reverse owner-plan sequence; the generic zero-action selector stays intact. */
static const SolMirRuntimeCleanupTransition *represented_snapshot_return_transition(
    const SolWasmRepresentedBuildRequest *request, const SolMirMaterializedImage *image,
    size_t image_id, size_t block) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirRuntimeCleanup *cleanup = owner->cleanup;
    const SolMirConcreteProgram *concrete = owner->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirOperations *operations = &concrete->operations;
    size_t snapshots = 0;
    if (image == NULL || block >= m->block_count || block >= owner->image_terminator_count
        || m->blocks[block].terminator.kind != SOL_MIR_TERM_RETURN
        || !represented_snapshot_count_for_image(concrete, image_id, &snapshots)
        || snapshots == 0) return NULL;
    const SolMirRuntimeLoweredImageTerminator *row = &owner->image_terminators[block];
    if (row->state != SOL_MIR_RUNTIME_LOWERED_PRESENT || row->image != image_id
        || row->block != block || row->kind != SOL_MIR_TERM_RETURN
        || row->cleanup_event >= cleanup->event_count) return NULL;
    const SolMirRuntimeCleanupEvent *event = &cleanup->events[row->cleanup_event];
    if (event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
        || event->phase != SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
        || event->origin != SOL_MIR_RUNTIME_CLEANUP_ORIGIN_EXPLICIT
        || event->owner != image_id || event->block != block
        || event->operation != SOL_MIR_RUNTIME_NONE || event->semantic_site != SOL_MIR_RUNTIME_NONE
        || event->inherited_failure_site != SOL_MIR_RUNTIME_NONE
        || event->supplemental_site != SOL_MIR_RUNTIME_NONE
        || event->producer != SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL
        || event->captures_failure_detail
        || event->capture_detail_kind != SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE
        || event->transitions.count != 1 || event->transitions.offset >= cleanup->transition_count
        || event->actions.offset > cleanup->action_count
        || event->actions.count != snapshots
        || event->actions.count > cleanup->action_count - event->actions.offset) return NULL;
    const SolMirRuntimeCleanupTransition *transition = &cleanup->transitions[
        event->transitions.offset];
    if (transition->event != row->cleanup_event
        || transition->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_EXIT
        || transition->edge_role != SOL_MIR_RUNTIME_CLEANUP_EDGE_RETURN
        || transition->continuation != SOL_MIR_RUNTIME_NONE
        || transition->source_edge != SOL_MIR_RUNTIME_NONE
        || transition->destination != SOL_MIR_RUNTIME_NONE || !transition->primary_failure_wins
        || transition->failure_source != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE
        || transition->failure_site != SOL_MIR_RUNTIME_NONE || transition->failure_mask != 0
        || transition->actions.offset != event->actions.offset
        || transition->actions.count != event->actions.count) return NULL;
    size_t ordinal = snapshots;
    for (size_t i = operations->snapshot_count; i; --i) {
        size_t plan_id = i - 1;
        if (operations->snapshots[plan_id].image != image_id) continue;
        if (ordinal == 0) return NULL;
        const SolMirRuntimeCleanupAction *action = &cleanup->actions[
            transition->actions.offset + snapshots - ordinal];
        if (action->kind != SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT
            || action->target != plan_id || action->recipe != operations->snapshots[plan_id].recipe
            || !represented_snapshot_cleanup_action(request, action)) return NULL;
        --ordinal;
    }
    if (ordinal != 0) return NULL;
    const SolMirRuntimeLoweredCleanupFailure *event_row = represented_cleanup_row(owner,
        SOL_MIR_RUNTIME_LOWERED_CLEANUP_EVENT, row->cleanup_event);
    const SolMirRuntimeLoweredCleanupFailure *transition_row = represented_cleanup_row(owner,
        SOL_MIR_RUNTIME_LOWERED_CLEANUP_TRANSITION, event->transitions.offset);
    return event_row != NULL && event_row->event == row->cleanup_event
        && event_row->actions.offset == event->actions.offset
        && event_row->actions.count == event->actions.count
        && transition_row != NULL && transition_row->transition == event->transitions.offset
        && transition_row->event == row->cleanup_event
        && transition_row->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_RETURN
        && transition_row->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_EXIT
        && transition_row->actions.offset == transition->actions.offset
        && transition_row->actions.count == transition->actions.count ? transition : NULL;
}

static bool represented_same_projection(const SolMirMaterialization *m, size_t left,
    size_t right) {
    if (left >= m->place_count || right >= m->place_count) return false;
    const SolMirMaterializedPlace *a = &m->places[left], *b = &m->places[right];
    if (a->local != b->local || a->projections.count != b->projections.count
        || a->projections.offset > m->projection_count
        || a->projections.count > m->projection_count - a->projections.offset
        || b->projections.offset > m->projection_count
        || b->projections.count > m->projection_count - b->projections.offset) return false;
    for (size_t i = 0; i < a->projections.count; ++i) {
        const SolMirMaterializedProjection *x = &m->projections[a->projections.offset + i];
        const SolMirMaterializedProjection *y = &m->projections[b->projections.offset + i];
        if (x->kind != y->kind || x->source_field != y->source_field
            || x->tuple_ordinal != y->tuple_ordinal) return false;
    }
    return true;
}

/* C3.2 admits no general value-flow recovery.  The value stored back into a
 * callable Pair field must resolve through one unique whole-local move/store
 * link per hop to one unique projected move of that same field. */
static bool represented_value_from_callable_field_move(const SolWasmRepresentedBuildRequest *request,
    const SolMirMaterializedImage *image, SolMirMaterializedValueId value, size_t field,
    size_t depth, size_t *projected_move) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    size_t defining = SOL_MIR_MATERIALIZED_NONE, definitions = 0;
    if (depth > 4 || value >= m->value_count || field >= m->place_count) return false;
    for (size_t instruction = 0; instruction < m->instruction_count; ++instruction) {
        if (m->instructions[instruction].result == value) { defining = instruction; ++definitions; }
    }
    if (definitions != 1) return false;
    const SolMirMaterializedInstruction *move = &m->instructions[defining];
    if (move->kind != SOL_MIR_INST_LOAD_MOVE || move->place >= m->place_count) return false;
    if (m->places[move->place].projections.count != 0) {
        if (!represented_same_projection(m, move->place, field)) return false;
        *projected_move = defining;
        return true;
    }
    if (m->places[move->place].final_type >= concrete->layout.type_count
        || !represented_unbound_function_recipe(concrete, concrete->layout.types[
            m->places[move->place].final_type].recipe)) return false;
    SolMirMaterializedValueId source = SOL_MIR_MATERIALIZED_NONE;
    for (size_t instruction = 0; instruction < m->instruction_count; ++instruction) {
        const SolMirMaterializedInstruction *store = &m->instructions[instruction];
        if (store->kind == SOL_MIR_INST_STORE && store->place < m->place_count
            && m->places[store->place].projections.count == 0
            && m->places[store->place].local == m->places[move->place].local) {
            if (source != SOL_MIR_MATERIALIZED_NONE) return false;
            source = store->left;
        }
    }
    return source != SOL_MIR_MATERIALIZED_NONE
        && represented_value_from_callable_field_move(request, image, source, field, depth + 1,
            projected_move);
}

static bool represented_callable_field_repair(const SolWasmRepresentedBuildRequest *request,
    const SolMirMaterializedImage *image, size_t instruction, const SolMirFieldLayout **field_out) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirLayout *layout = &concrete->layout;
    if (instruction >= m->instruction_count) return false;
    const SolMirMaterializedInstruction *store = &m->instructions[instruction];
    if (store->kind != SOL_MIR_INST_STORE || store->place >= m->place_count) return false;
    const SolMirMaterializedPlace *place = &m->places[store->place];
    if (place->projections.count != 1 || place->projections.offset >= layout->projection_count
        || place->root_type >= layout->type_count || place->final_type >= layout->type_count
        || !represented_callable_product_recipe(concrete, layout->types[place->root_type].recipe)
        || !represented_unbound_function_recipe(concrete, layout->types[place->final_type].recipe))
        return false;
    const SolMirProjectionMap *map = &layout->projections[place->projections.offset];
    if (map->place != store->place || map->field_layout >= layout->field_count
        || map->result_recipe != layout->types[place->final_type].recipe) return false;
    const SolMirFieldLayout *field = &layout->fields[map->field_layout];
    size_t projected_move = SOL_MIR_MATERIALIZED_NONE;
    if (field->owner_recipe != layout->types[place->root_type].recipe || !field->has_storage
        || field->size != 4 || field->alignment != 4 || field->offset != map->object_offset
        || field->offset > UINT32_MAX || !represented_value_from_callable_field_move(request, image,
            store->left, store->place, 0, &projected_move) || projected_move >= instruction)
        return false;
    *field_out = field;
    return true;
}

/* The pre-store marker is the one projected cleanup shape admitted by C3.2.
 * It is not a generic projected drop: the already-set hole proves that the
 * field was moved, and the immediately following authenticated repair owns the
 * only state change (the four-byte write and hole clear). */
static bool represented_projected_pre_store_action(const SolWasmRepresentedBuildRequest *request,
    const SolMirMaterializedImage *image, size_t instruction,
    const SolMirRuntimeCleanupAction *action) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirRuntimeCleanup *cleanup = owner->cleanup;
    const SolMirMaterialization *m = &owner->conventions->concrete->materialization;
    if (instruction >= m->instruction_count || instruction >= owner->image_instruction_count
        || instruction + 1 >= m->instruction_count) return false;
    size_t image_id = SOL_MIR_MATERIALIZED_NONE;
    for (size_t i = 0; i < m->image_count; ++i)
        if (&m->images[i] == image) { if (image_id != SOL_MIR_MATERIALIZED_NONE) return false; image_id = i; }
    const SolMirRuntimeLoweredImageInstruction *row = &owner->image_instructions[instruction];
    if (image_id == SOL_MIR_MATERIALIZED_NONE || row->state != SOL_MIR_RUNTIME_LOWERED_PRESENT
        || row->instruction != instruction || row->image != image_id
        || row->block != m->instructions[instruction].block || row->kind
            != SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED || row->cleanup_event >= cleanup->event_count)
        return false;
    const SolMirRuntimeCleanupEvent *event = &cleanup->events[row->cleanup_event];
    if (event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION
        || event->phase != SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
        || event->origin != SOL_MIR_RUNTIME_CLEANUP_ORIGIN_EXPLICIT
        || event->owner != row->image || event->block != row->block
        || event->operation != instruction || event->transitions.count != 1
        || event->transitions.offset >= cleanup->transition_count) return false;
    const SolMirRuntimeCleanupTransition *transition = &cleanup->transitions[event->transitions.offset];
    if (transition->event != row->cleanup_event
        || transition->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL
        || transition->edge_role != SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO
        || transition->continuation != SOL_MIR_RUNTIME_NONE
        || transition->actions.count != 1 || transition->actions.offset >= cleanup->action_count)
        return false;
    const SolMirRuntimeCleanupAction *named = &cleanup->actions[transition->actions.offset];
    if (action != NULL && action != named) return false;
    action = named;
    if (action->flags != SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED
        || action->kind != SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE
        || action->target >= m->place_count || m->places[action->target].projections.count != 1
        || m->instructions[instruction].kind != SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED
        || m->instructions[instruction].place != action->target
        || action->drop_path >= cleanup->drop_path_count) return false;
    const SolMirRuntimeCleanupDropPath *path = &cleanup->drop_paths[action->drop_path];
    if (path->root >= m->place_count || path->place != action->target
        || path->recipe != action->recipe || m->places[path->root].projections.count != 0
        || m->places[path->root].local != m->places[action->target].local
        || path->liveness != SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL
        || path->holes.count != 1 || path->holes.offset >= cleanup->drop_path_count) return false;
    const SolMirRuntimeCleanupDropPath *hole = &cleanup->drop_paths[path->holes.offset];
    if (hole->root != path->root || hole->recipe != path->recipe
        || !represented_same_projection(m, hole->place, action->target)
        || hole->liveness != SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE) return false;
    const SolMirMaterializedInstruction *store = &m->instructions[instruction + 1];
    const SolMirFieldLayout *field = NULL;
    return store->kind == SOL_MIR_INST_STORE && represented_same_projection(m, store->place,
        action->target) && represented_callable_field_repair(request, image, instruction + 1, &field)
        && field != NULL;
}

static bool represented_projected_pre_store_cleanup_action(
    const SolWasmRepresentedBuildRequest *request, const SolMirRuntimeCleanupAction *action) {
    const SolMirMaterialization *m = &request->program->conventions->concrete->materialization;
    size_t matches = 0;
    for (size_t image = 0; image < m->image_count; ++image) {
        const SolMirMaterializedImage *body = &m->images[image];
        for (size_t i = 0; i < body->instructions.count; ++i) {
            size_t instruction = body->instructions.offset + i;
            if (instruction >= m->instruction_count) return false;
            if (m->instructions[instruction].kind != SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED
                || m->instructions[instruction].place != action->target) continue;
            if (represented_projected_pre_store_action(request, body, instruction, action)) ++matches;
        }
    }
    return matches == 1;
}

static bool represented_eventless_old_root_marker(const RepresentedFunction *function,
    size_t instruction) {
    const SolMirRuntimeLoweredProgram *owner = function->request->program;
    const SolMirMaterialization *m = &owner->conventions->concrete->materialization;
    if (instruction >= m->instruction_count || instruction >= owner->image_instruction_count
        || m->instructions[instruction].kind != SOL_MIR_INST_DROP_IF_INITIALIZED)
        return false;
    const SolMirRuntimeLoweredImageInstruction *row = &owner->image_instructions[instruction];
    if (row->state != SOL_MIR_RUNTIME_LOWERED_PRESENT || row->image != function->image_id
        || row->instruction != instruction || row->block != m->instructions[instruction].block
        || row->kind != m->instructions[instruction].kind
        || row->runtime_class != SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL
        || row->plan_family != SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP
        || row->cleanup_event != SOL_MIR_RUNTIME_LOWERED_NONE
        || row->failure_site != SOL_MIR_RUNTIME_NONE) return false;
    size_t moves = 0;
    for (size_t i = 0; i < function->image->instructions.count; ++i) {
        size_t candidate = function->image->instructions.offset + i;
        if (candidate >= m->instruction_count) return false;
        const SolMirMaterializedInstruction *move = &m->instructions[candidate];
        if (move->kind == SOL_MIR_INST_LOAD_MOVE && move->place < m->place_count
            && m->places[move->place].projections.count == 0
            && m->places[move->place].local == m->instructions[instruction].local) {
            if (candidate >= instruction || !represented_callable_product_whole_transfer(
                    function->request, function->image, candidate)) return false;
            ++moves;
        }
    }
    return moves == 1;
}

/* P3.6 intentionally omits a marker event when replay proves that there is no
 * owned value left to consume.  Do not turn that absence into an arena search:
 * accept it only when the current, present lowered row itself names this exact
 * control marker and carries no failure identity. */
static bool represented_eventless_cleanup_marker(const RepresentedFunction *function,
    size_t instruction) {
    const SolMirRuntimeLoweredProgram *owner = function->request->program;
    const SolMirMaterialization *m = &owner->conventions->concrete->materialization;
    if (instruction >= m->instruction_count || instruction >= owner->image_instruction_count)
        return false;
    const SolMirMaterializedInstruction *item = &m->instructions[instruction];
    const SolMirRuntimeLoweredImageInstruction *row = &owner->image_instructions[instruction];
    return (item->kind == SOL_MIR_INST_TEMPORARY_DROP
            || item->kind == SOL_MIR_INST_DROP_IF_INITIALIZED
            || item->kind == SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED)
        && row->state == SOL_MIR_RUNTIME_LOWERED_PRESENT && row->image == function->image_id
        && row->instruction == instruction && row->block == item->block && row->kind == item->kind
        && row->runtime_class == SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL
        && row->plan_family == SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP
        && row->cleanup_event == SOL_MIR_RUNTIME_LOWERED_NONE
        && row->failure_site == SOL_MIR_RUNTIME_NONE;
}

/* An explicit marker has exactly three outcomes.  An action is selected only
 * from its own event.  An eventless marker is accepted only after the exact
 * lowered control row proves P3 replay found no owned value.  In particular,
 * the absence of a selected action never converts a present event into a
 * local-init clear. */
static RepresentedCleanupMarkerRoute represented_cleanup_instruction_route(
    const RepresentedFunction *function, size_t instruction,
    const SolMirRuntimeCleanupAction **action_out) {
    if (action_out == NULL) return REPRESENTED_CLEANUP_MARKER_INVALID;
    *action_out = represented_cleanup_instruction_action(function, instruction);
    if (*action_out != NULL) return REPRESENTED_CLEANUP_MARKER_ACTION;
    if (represented_eventless_old_root_marker(function, instruction)
        || represented_eventless_cleanup_marker(function, instruction))
        return REPRESENTED_CLEANUP_MARKER_EVENTLESS;
    return REPRESENTED_CLEANUP_MARKER_INVALID;
}

static bool represented_callable_product_zero_hole_drop(const SolWasmRepresentedBuildRequest *request,
    const SolMirMaterializedImage *image, size_t root) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirRuntimeCleanup *cleanup = owner->cleanup;
    const SolMirMaterialization *m = &owner->conventions->concrete->materialization;
    size_t markers = 0;
    for (size_t i = 0; i < image->instructions.count; ++i) {
        size_t instruction = image->instructions.offset + i;
        if (instruction >= m->instruction_count || instruction >= owner->image_instruction_count) return false;
        const SolMirMaterializedInstruction *item = &m->instructions[instruction];
        bool matches = item->kind == SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED && item->place == root;
        if (item->kind == SOL_MIR_INST_DROP_IF_INITIALIZED && root < m->place_count)
            matches = m->places[root].local == item->local;
        if (!matches) continue;
        const SolMirRuntimeLoweredImageInstruction *row = &owner->image_instructions[instruction];
        if (row->state != SOL_MIR_RUNTIME_LOWERED_PRESENT || row->instruction != instruction
            || row->cleanup_event >= cleanup->event_count) return false;
        const SolMirRuntimeCleanupEvent *event = &cleanup->events[row->cleanup_event];
        if (event->operation != instruction || event->transitions.count != 1
            || event->transitions.offset >= cleanup->transition_count) return false;
        const SolMirRuntimeCleanupTransition *transition = &cleanup->transitions[event->transitions.offset];
        if (transition->event != row->cleanup_event
            || transition->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL
            || transition->actions.count != 1 || transition->actions.offset >= cleanup->action_count)
            return false;
        const SolMirRuntimeCleanupAction *action = &cleanup->actions[transition->actions.offset];
        if (action->kind != SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE || action->target != root
            || action->flags != 0 || action->drop_path >= cleanup->drop_path_count) return false;
        const SolMirRuntimeCleanupDropPath *path = &cleanup->drop_paths[action->drop_path];
        if (path->root != root || path->place != root || path->recipe != action->recipe
            || path->holes.count != 0 || path->liveness != SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE)
            return false;
        ++markers;
    }
    return markers == 1;
}

static bool represented_callable_product_whole_transfer(const SolWasmRepresentedBuildRequest *request,
    const SolMirMaterializedImage *image, size_t instruction) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirConcreteProgram *concrete = owner->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    if (instruction >= m->instruction_count || instruction >= owner->image_instruction_count) return false;
    const SolMirMaterializedInstruction *move = &m->instructions[instruction];
    if (move->kind != SOL_MIR_INST_LOAD_MOVE || move->place >= m->place_count
        || m->places[move->place].projections.count != 0
        || m->places[move->place].final_type >= concrete->layout.type_count
        || !represented_callable_product_recipe(concrete, concrete->layout.types[
            m->places[move->place].final_type].recipe)) return false;
    size_t destination = SOL_MIR_MATERIALIZED_NONE, stores = 0, source_markers = 0;
    for (size_t i = 0; i < image->instructions.count; ++i) {
        size_t candidate = image->instructions.offset + i;
        if (candidate >= m->instruction_count || candidate >= owner->image_instruction_count) return false;
        const SolMirMaterializedInstruction *item = &m->instructions[candidate];
        if (item->kind == SOL_MIR_INST_LOAD_MOVE && item->place < m->place_count
            && m->places[item->place].local == m->places[move->place].local
            && m->places[item->place].projections.count != 0) return false;
        if (item->kind == SOL_MIR_INST_STORE && item->left == move->result
            && item->place < m->place_count && m->places[item->place].projections.count == 0) {
            if (candidate <= instruction || m->places[item->place].local == m->places[move->place].local
                || m->places[item->place].final_type != m->places[move->place].final_type) return false;
            destination = item->place; ++stores;
        }
        if (item->kind == SOL_MIR_INST_DROP_IF_INITIALIZED
            && item->local == m->places[move->place].local) {
            if (candidate <= instruction || owner->image_instructions[candidate].cleanup_event
                    != SOL_MIR_RUNTIME_LOWERED_NONE) return false;
            ++source_markers;
        }
    }
    return stores == 1 && source_markers == 1
        && represented_callable_product_zero_hole_drop(request, image, destination);
}

static bool represented_cleanup(const SolWasmRepresentedBuildRequest *request) {
    const SolMirRuntimeCleanup *cleanup = request->program->cleanup;
    const SolMirOperations *operations = &request->program->conventions->concrete->operations;
    for (size_t i = 0; i < cleanup->action_count; ++i) {
        const SolMirRuntimeCleanupAction *action = &cleanup->actions[i];
        if (!represented_cleanup_action(request, action)) return false;
    }
    for (size_t i = 0; i < cleanup->transition_count; ++i) {
        const SolMirRuntimeCleanupTransition *transition = &cleanup->transitions[i];
        if (transition->event >= cleanup->event_count
            || transition->actions.offset > cleanup->action_count
            || transition->actions.count > cleanup->action_count - transition->actions.offset)
            return false;
        size_t image = SOL_MIR_RUNTIME_NONE, prior_slot = SIZE_MAX;
        if (!represented_cleanup_event_image(request, &cleanup->events[transition->event], &image))
            continue;
        for (size_t q = 0; q < transition->actions.count; ++q) {
            const SolMirRuntimeCleanupAction *action = &cleanup->actions[
                transition->actions.offset + q];
            if (action->kind != SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT) continue;
            if (action->target >= operations->snapshot_count
                || operations->snapshots[action->target].image != image
                || operations->snapshots[action->target].slot >= prior_slot) return false;
            prior_slot = operations->snapshots[action->target].slot;
        }
    }
    return true;
}

/* PROPAGATE is the one represented terminator that constructs an aggregate at
 * runtime.  The plan is deliberately joined through the P3.6 semantic row and
 * every P2 layout row it names; no tag, field offset, or allocation site is
 * inferred by the backend. */
static bool represented_propagation_plan(const SolWasmRepresentedBuildRequest *request,
    size_t image, size_t block, const SolMirOperationPropagationPlan **out) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirConcreteProgram *concrete = owner->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirRepresentation *r = &concrete->representation;
    const SolMirLayout *layout = &concrete->layout;
    if (block >= m->block_count || block >= owner->image_terminator_count
        || m->blocks[block].terminator.kind != SOL_MIR_TERM_PROPAGATE) return false;
    const SolMirMaterializedTerminator *term = &m->blocks[block].terminator;
    const SolMirRuntimeLoweredImageTerminator *row = &owner->image_terminators[block];
    if (row->image != image || row->block != block || row->kind != term->kind
        || row->plan >= owner->semantic_plan_count) return false;
    const SolMirRuntimeLoweredSemanticPlan *semantic = &owner->semantic_plans[row->plan];
    if (semantic->state != SOL_MIR_RUNTIME_LOWERED_PRESENT
        || semantic->arena != SOL_MIR_RUNTIME_LOWERED_SEMANTIC_PROPAGATION
        || semantic->plan >= concrete->operations.propagation_count
        || semantic->producer != block) return false;
    const SolMirOperationPropagationPlan *p = &concrete->operations.propagations[semantic->plan];
    if (p->image != image || p->block != block || p->source != term->operand
        || p->success_result != term->value_result || p->residual_result != term->residual_result
        || p->success_edge != term->value_edge || p->residual_edge != term->residual_edge
        || term->operand >= m->temporary_count || term->value_result >= m->value_count
        || term->residual_result >= m->value_count || p->source_recipe >= r->recipe_count
        || p->success_recipe >= r->recipe_count || p->residual_recipe >= r->recipe_count
        || m->temporaries[term->operand].type != p->source_recipe
        || m->values[term->value_result].type != p->success_recipe
        || m->values[term->residual_result].type != p->residual_recipe
        || p->success_variant_layout >= r->variant_count
        || p->source_residual_variant_layout >= r->variant_count
        || p->destination_residual_variant_layout >= r->variant_count
        || p->success_field_layout >= layout->field_count
        || p->success_field_layout >= r->field_count) return false;
    SolMirRecipeKind expected = term->propagation_kind == SOL_IR_PROPAGATE_OPTION
        ? SOL_MIR_RECIPE_OPTION : term->propagation_kind == SOL_IR_PROPAGATE_RESULT
        ? SOL_MIR_RECIPE_RESULT : SOL_MIR_RECIPE_NEVER;
    if (expected == SOL_MIR_RECIPE_NEVER || r->recipes[p->source_recipe].kind != expected
        || r->recipes[p->residual_recipe].kind != expected
        || !represented_sum_recipe(concrete, p->source_recipe)
        || !represented_sum_recipe(concrete, p->residual_recipe)) return false;
    const SolMirVariantLayout *success_variant = &layout->variants[p->success_variant_layout];
    const SolMirVariantLayout *source_variant = &layout->variants[p->source_residual_variant_layout];
    const SolMirVariantLayout *destination_variant = &layout->variants[p->destination_residual_variant_layout];
    const SolMirFieldLayout *success_field = &layout->fields[p->success_field_layout];
    if (success_variant->owner_recipe != p->source_recipe || !success_variant->inhabited
        || success_variant->tag != p->success_tag || success_field->owner_recipe != p->source_recipe
        || success_field->variant != p->success_variant_layout
        || r->fields[p->success_field_layout].type != p->success_recipe
        || success_field->offset != p->success_field_offset) return false;
    if (term->propagation_kind == SOL_IR_PROPAGATE_OPTION) {
        if (p->source_residual_field_layout != SOL_MIR_OPERATION_NONE
            || p->destination_residual_field_layout != SOL_MIR_OPERATION_NONE
            || p->source_residual_field_recipe != SOL_MIR_RECIPE_NONE
            || p->destination_residual_field_recipe != SOL_MIR_RECIPE_NONE
            || p->source_residual_field_offset != SOL_MIR_LAYOUT_OFFSET_NONE
            || p->destination_residual_field_offset != SOL_MIR_LAYOUT_OFFSET_NONE) return false;
    } else {
        if (p->source_residual_field_layout >= layout->field_count
            || p->destination_residual_field_layout >= layout->field_count
            || p->source_residual_field_layout >= r->field_count
            || p->destination_residual_field_layout >= r->field_count) return false;
        const SolMirFieldLayout *source_field = &layout->fields[p->source_residual_field_layout];
        const SolMirFieldLayout *destination_field = &layout->fields[p->destination_residual_field_layout];
        if (source_field->owner_recipe != p->source_recipe
            || source_field->variant != p->source_residual_variant_layout
            || destination_field->owner_recipe != p->residual_recipe
            || destination_field->variant != p->destination_residual_variant_layout
            || source_field->offset != p->source_residual_field_offset
            || destination_field->offset != p->destination_residual_field_offset
            || r->fields[p->source_residual_field_layout].type != p->source_residual_field_recipe
            || r->fields[p->destination_residual_field_layout].type != p->destination_residual_field_recipe
            || p->source_residual_field_recipe != p->destination_residual_field_recipe
            || source_field->size != destination_field->size
            || source_field->alignment != destination_field->alignment
            || !source_field->has_storage || !destination_field->has_storage
            || (source_field->size != 1 && source_field->size != 4
                && source_field->size != 8) || source_field->offset > UINT32_MAX
            || destination_field->offset > UINT32_MAX || source_field->alignment > UINT32_MAX)
            return false;
    }
    if (source_variant->owner_recipe != p->source_recipe || !source_variant->inhabited
        || source_variant->tag != p->source_residual_tag
        || destination_variant->owner_recipe != p->residual_recipe || !destination_variant->inhabited
        || destination_variant->tag != p->destination_residual_tag) return false;
    SolMirRecipeId physical;
    if (!represented_backing_recipe(concrete, p->success_recipe, &physical)
        || !represented_scalar_recipe(concrete, physical, false)
        || (success_field->size == 0 && !represented_zero_width_unit_field(concrete,
            p->success_recipe, success_field))
        || (success_field->size != 0 && (!success_field->has_storage
            || (success_field->size != 1 && success_field->size != 4 && success_field->size != 8)
            || success_field->offset > UINT32_MAX))) return false;
    if (p->residual_recipe >= layout->type_count || !layout->types[p->residual_recipe].has_object
        || layout->types[p->residual_recipe].object_kind != SOL_MIR_LAYOUT_OBJECT_SUM
        || layout->types[p->residual_recipe].object_size == 0
        || layout->types[p->residual_recipe].object_size > INT64_MAX) return false;
    *out = p;
    return true;
}

static bool represented_propagation_pre_event(const SolWasmRepresentedBuildRequest *request,
    size_t image, size_t block, const SolMirOperationPropagationPlan *plan,
    size_t *event_out, size_t *site_out) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirRuntimeLoweredImageTerminator *row = &owner->image_terminators[block];
    const SolMirRuntimeCleanup *cleanup = owner->cleanup;
    if (row->pre_operation_cleanup_event >= cleanup->event_count
        || row->pre_operation_supplemental_site >= cleanup->supplemental_site_count) return false;
    size_t event_id = row->pre_operation_cleanup_event;
    const SolMirRuntimeCleanupEvent *event = &cleanup->events[event_id];
    const uint32_t mask = (UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED - 1))
        | (UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_ALLOCATION_LIMIT - 1));
    if (event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
        || event->phase != SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_PROPAGATE_RESIDUAL
        || event->origin != SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT
        || event->owner != image || event->block != block
        || event->operation >= owner->conventions->concrete->operations.propagation_count
        || &owner->conventions->concrete->operations.propagations[event->operation] != plan
        || event->producer != SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PROPAGATION_RESIDUAL
        || !event->captures_failure_detail || event->capture_detail_kind
            != SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE || event->inherited_failure_site
            != SOL_MIR_RUNTIME_NONE || event->supplemental_site != row->pre_operation_supplemental_site
        || event->supplemental_site >= cleanup->supplemental_site_count
        || cleanup->supplemental_sites[event->supplemental_site].event != event_id
        || cleanup->supplemental_sites[event->supplemental_site].allowed_codes != mask
        || memcmp(&event->source, &cleanup->supplemental_sites[event->supplemental_site].source,
            sizeof event->source) != 0
        || event->transitions.count != 2 || event->transitions.offset > cleanup->transition_count
        || event->transitions.count > cleanup->transition_count - event->transitions.offset) return false;
    const SolMirRuntimeCleanupTransition *normal = NULL, *failure = NULL;
    for (size_t i = 0; i < event->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *candidate = &cleanup->transitions[event->transitions.offset + i];
        if (candidate->event != event_id || candidate->actions.offset > cleanup->action_count
            || candidate->actions.count > cleanup->action_count - candidate->actions.offset) return false;
        if (candidate->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL) {
            if (normal != NULL || candidate->edge_role
                    != SOL_MIR_RUNTIME_CLEANUP_EDGE_PRE_OPERATION_READY
                || candidate->continuation != SOL_MIR_RUNTIME_NONE || candidate->actions.count != 0) return false;
            normal = candidate;
        } else if (candidate->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE) {
            if (failure != NULL || candidate->edge_role
                    != SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE
                || candidate->failure_source != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_SUPPLEMENTAL_P33
                || candidate->failure_site != event->supplemental_site || candidate->failure_mask != mask
                || candidate->continuation != SOL_MIR_RUNTIME_NONE) return false;
            bool propagated = false;
            for (size_t a = 0; a < candidate->actions.count; ++a) {
                const SolMirRuntimeCleanupAction *action = &cleanup->actions[candidate->actions.offset + a];
                if (!represented_cleanup_action(request, action)) return false;
                if (action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE) {
                    if (propagated || a + 1 != candidate->actions.count
                        || action->flags != SOL_MIR_RUNTIME_CLEANUP_ACTION_FAILURE_ONLY
                        || action->target != event->supplemental_site) return false;
                    propagated = true;
                } else if (propagated) return false;
            }
            if (!propagated) return false;
            failure = candidate;
        } else return false;
    }
    if (normal == NULL || failure == NULL) return false;
    *event_out = event_id; *site_out = event->supplemental_site;
    return true;
}

static bool represented_propagation_main_event(const SolWasmRepresentedBuildRequest *request,
    size_t image, size_t block, const SolMirOperationPropagationPlan *plan,
    const SolMirRuntimeCleanupTransition **value_out,
    const SolMirRuntimeCleanupTransition **residual_out) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirRuntimeCleanup *cleanup = owner->cleanup;
    const SolMirMaterialization *m = &owner->conventions->concrete->materialization;
    const SolMirRuntimeLoweredImageTerminator *row = &owner->image_terminators[block];
    if (row->cleanup_event >= cleanup->event_count) return false;
    const SolMirRuntimeCleanupEvent *event = &cleanup->events[row->cleanup_event];
    if (event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
        || event->phase != SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
        || event->origin != SOL_MIR_RUNTIME_CLEANUP_ORIGIN_EXPLICIT
        || event->owner != image || event->block != block
        || event->operation != SOL_MIR_RUNTIME_NONE || event->supplemental_site != SOL_MIR_RUNTIME_NONE
        || event->inherited_failure_site != SOL_MIR_RUNTIME_NONE || event->producer
            != SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL || event->captures_failure_detail
        || event->capture_detail_kind != SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE
        || event->transitions.count != 2 || event->transitions.offset > cleanup->transition_count
        || event->transitions.count > cleanup->transition_count - event->transitions.offset) return false;
    const SolMirRuntimeCleanupTransition *value = NULL, *residual = NULL;
    for (size_t i = 0; i < event->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *candidate = &cleanup->transitions[event->transitions.offset + i];
        if (candidate->event != row->cleanup_event || candidate->outcome
                != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL
            || candidate->failure_source != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE
            || candidate->failure_site != SOL_MIR_RUNTIME_NONE || candidate->failure_mask != 0
            || candidate->actions.offset > cleanup->action_count
            || candidate->actions.count > cleanup->action_count - candidate->actions.offset) return false;
        if (candidate->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_PROPAGATE_VALUE) {
            if (value != NULL || candidate->continuation != plan->success_edge
                || candidate->source_edge != plan->success_edge
                || plan->success_edge >= m->edge_count
                || candidate->destination != m->edges[plan->success_edge].block) return false;
            value = candidate;
        } else if (candidate->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_PROPAGATE_RESIDUAL) {
            if (residual != NULL || candidate->continuation != plan->residual_edge
                || candidate->source_edge != plan->residual_edge
                || plan->residual_edge >= m->edge_count
                || candidate->destination != m->edges[plan->residual_edge].block) return false;
            residual = candidate;
        } else return false;
        for (size_t a = 0; a < candidate->actions.count; ++a)
            if (!represented_cleanup_action(request,
                    &cleanup->actions[candidate->actions.offset + a])) return false;
    }
    if (value == NULL || residual == NULL) return false;
    *value_out = value; *residual_out = residual;
    return true;
}

/* Slice 3A keeps a complete, private call catalog even though the emitter is
 * still deliberately call-free.  In particular, do not shortcut this to the
 * P3.6 call count: every row below is joined back to its P2 terminator and
 * cleanup packet before the normal no-call emitter rejects the closure. */
typedef struct {
    size_t caller_image, caller_block, caller_callable, call;
    size_t callee_callable, signature;
    SolMirRuntimeSlice operands;
    SolMirMaterializedValueId result;
    SolMirRuntimeResultClass result_class;
    size_t normal_edge, failure_edge, failure_site;
    size_t chain_depth;
    bool cyclic;
} RepresentedCallCatalog;

/* Kept outside the direct-call row intentionally.  P4.3's zero-indirect
 * closure census and allocation sequence are frozen, so callback-only state
 * must not widen every direct-call catalog row. */
typedef struct {
    size_t call, table;
} RepresentedCallbackCatalog;

struct RepresentedCallCatalogGraph {
    RepresentedCallCatalog *calls;
    RepresentedCallbackCatalog *callbacks;
    unsigned char *resume_blocks;
    size_t count, callback_count, edge_count, longest_chain;
    bool has_cycle;
    size_t work_bytes;
};

typedef enum {
    REPRESENTED_CATALOG_VALID,
    REPRESENTED_CATALOG_INVALID,
    REPRESENTED_CATALOG_RESOURCE,
    REPRESENTED_CATALOG_ALLOCATION,
} RepresentedCatalogResult;

static bool represented_signature_exact(const SolMirRuntimeConventions *conventions,
    const SolMirRuntimeSignature *function, const SolMirRuntimeSignature *internal) {
    if (function == NULL || internal == NULL || function->result != internal->result
        || function->result_class != internal->result_class || function->effects != internal->effects
        || function->slots.count != internal->slots.count
        || function->slots.offset > conventions->signature_slot_count
        || internal->slots.offset > conventions->signature_slot_count
        || function->slots.count > conventions->signature_slot_count - function->slots.offset
        || internal->slots.count > conventions->signature_slot_count - internal->slots.offset)
        return false;
    for (size_t i = 0; i < function->slots.count; ++i) {
        const SolMirRuntimeSignatureSlot *a = &conventions->signature_slots[function->slots.offset + i];
        const SolMirRuntimeSignatureSlot *b = &conventions->signature_slots[internal->slots.offset + i];
        if (a->role != SOL_MIR_RUNTIME_SLOT_PARAMETER || b->role != SOL_MIR_RUNTIME_SLOT_PARAMETER
            || a->formal != i || b->formal != i || a->recipe != b->recipe || a->access != b->access)
            return false;
    }
    return true;
}

static bool represented_signature_refined(const SolMirRuntimeConventions *conventions,
    const SolMirRuntimeSignature *signature) {
    const SolMirConcreteProgram *concrete = conventions->concrete;
    if (signature == NULL || signature->result >= concrete->representation.recipe_count
        || signature->slots.offset > conventions->signature_slot_count
        || signature->slots.count > conventions->signature_slot_count - signature->slots.offset)
        return false;
    if (concrete->representation.recipes[signature->result].kind == SOL_MIR_RECIPE_REFINED)
        return true;
    for (size_t i = 0; i < signature->slots.count; ++i) {
        SolMirRecipeId recipe = conventions->signature_slots[signature->slots.offset + i].recipe;
        if (recipe < concrete->representation.recipe_count
            && concrete->representation.recipes[recipe].kind == SOL_MIR_RECIPE_REFINED) return true;
    }
    return false;
}

static bool represented_callback_target(const SolWasmRepresentedBuildRequest *request,
    const SolMirRuntimeCall *call, const SolMirMaterializedTerminator *term,
    size_t *internal) {
    const SolMirRuntimeConventions *conventions = request->program->conventions;
    const SolMirConcreteProgram *concrete = conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirLinkage *linkage = &concrete->linkage;
    if (call->call_kind != SOL_IR_CALL_CALLBACK || call->target_kind
            != SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE || call->internal != SOL_MIR_LINKAGE_NONE
        || call->host != SOL_MIR_LINKAGE_NONE || call->table >= linkage->table_entry_count
        || call->callee.kind != SOL_MIR_RUNTIME_VALUE_MATERIALIZED_TEMPORARY
        || call->callee.id != term->callee || term->callee >= m->temporary_count
        || call->signature >= conventions->signature_count || term->callable_site >= m->semantic_site_count)
        return false;
    const SolMirMaterializedSemanticSite *site = &m->semantic_sites[term->callable_site];
    if (site->producer_kind != SOL_MIR_MATERIALIZED_PRODUCER_INSTRUCTION
        || site->instruction >= m->instruction_count) return false;
    size_t table = SOL_MIR_LINKAGE_NONE, target = SOL_MIR_LINKAGE_NONE;
    if (!represented_function_value_route(request, site->instruction, NULL, &table, &target)
        || table != call->table || target >= linkage->callable_count) return false;
    const SolMirRuntimeSignature *function = &conventions->signatures[call->signature];
    const SolMirRuntimeSignature *callee = signature_for(conventions, target);
    if (function->origin != SOL_MIR_RUNTIME_SIGNATURE_FUNCTION_RECIPE
        || function->function_recipe != m->temporaries[term->callee].type
        || !represented_unbound_function_recipe(concrete, function->function_recipe)
        || !represented_signature_exact(conventions, function, callee)) return false;
    *internal = target;
    return true;
}

/* C2b is intentionally a single physical convention, not a general inout
 * adapter: one unbound `function(inout Int64)->Unit` callback transfers its
 * final parameter word through the ordinary i64 Wasm result.  Authenticate the
 * complete P3.1/P3.3/P3.6 chain before emission so a forged access, place,
 * recipe, action, or result cannot widen that convention. */
static bool represented_callback_inout_exact(const SolWasmRepresentedBuildRequest *request,
    const SolMirRuntimeCall *call, const SolMirMaterializedTerminator *term,
    const SolMirRuntimeSignature *signature) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirRuntimeConventions *conventions = owner->conventions;
    const SolMirConcreteProgram *concrete = conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirRuntimeCleanup *cleanup = owner->cleanup;
    if (call->call_kind != SOL_IR_CALL_CALLBACK || signature->slots.count != 1
        || signature->result_class != SOL_MIR_RUNTIME_RESULT_UNIT
        || signature->result >= concrete->representation.recipe_count
        || concrete->representation.recipes[signature->result].kind != SOL_MIR_RECIPE_UNIT
        || call->result.kind != SOL_MIR_RUNTIME_VALUE_NONE
        || call->result.id != SOL_MIR_RUNTIME_NONE || term->result >= m->value_count
        || call->operands.count != 1 || term->arguments.count != 1
        || call->writebacks.count != 1 || term->writebacks.count != 1
        || call->writebacks.offset >= conventions->writeback_count
        || term->writebacks.offset >= m->writeback_count
        || call->block >= owner->image_terminator_count) return false;
    const SolMirRuntimeSignatureSlot *slot = &conventions->signature_slots[signature->slots.offset];
    const SolMirRuntimeOperand *operand = &conventions->operands[call->operands.offset];
    const SolMirMaterializedCallArgument *argument = &m->call_arguments[term->arguments.offset];
    const SolMirRuntimeWriteback *writeback = &conventions->writebacks[call->writebacks.offset];
    const SolMirMaterializedWriteback *materialized = &m->writebacks[term->writebacks.offset];
    if (slot->role != SOL_MIR_RUNTIME_SLOT_PARAMETER || slot->formal != 0
        || slot->access != SOL_ACCESS_EXCLUSIVE || slot->recipe >= concrete->representation.recipe_count
        || concrete->representation.recipes[slot->recipe].kind != SOL_MIR_RECIPE_INT64
        || !represented_scalar_recipe(concrete, slot->recipe, false)
        || operand->signature_slot != signature->slots.offset
        || operand->value.kind != SOL_MIR_RUNTIME_VALUE_MATERIALIZED_PLACE
        || argument->formal != 0 || argument->access != SOL_ACCESS_EXCLUSIVE
        || argument->place != operand->value.id || argument->place >= m->place_count
        || writeback->place != argument->place
        || argument->type >= concrete->layout.type_count
        || concrete->layout.types[argument->type].recipe != slot->recipe
        || m->values[term->result].type >= concrete->layout.type_count
        || concrete->layout.types[m->values[term->result].type].recipe != signature->result
        || writeback->operand != call->operands.offset || writeback->receiver || writeback->formal != 0
        || writeback->recipe != slot->recipe || materialized->receiver || materialized->formal != 0
        || materialized->place != writeback->place || materialized->type >= concrete->layout.type_count
        || concrete->layout.types[materialized->type].recipe != writeback->recipe
        || writeback->place >= m->place_count || !whole_represented_place(concrete, writeback->place)) return false;
    const SolMirMaterializedPlace *place = &m->places[writeback->place];
    if (place->projections.count != 0 || place->local >= m->local_count
        || m->locals[place->local].kind != SOL_MIR_MATERIALIZED_LOCAL_BODY
        || place->local < m->images[call->image].locals.offset
        || place->local - m->images[call->image].locals.offset >= m->images[call->image].locals.count
        || place->final_type >= concrete->layout.type_count
        || concrete->layout.types[place->final_type].recipe != slot->recipe) return false;
    const SolMirRuntimeLoweredImageTerminator *row = &owner->image_terminators[call->block];
    if (row->cleanup_event >= cleanup->event_count) return false;
    const SolMirRuntimeCleanupEvent *event = &cleanup->events[row->cleanup_event];
    size_t normal_matches = 0, failure_matches = 0;
    for (size_t i = 0; i < event->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *transition = &cleanup->transitions[
            event->transitions.offset + i];
        if (transition->actions.offset > cleanup->action_count
            || transition->actions.count > cleanup->action_count - transition->actions.offset) return false;
        for (size_t q = 0; q < transition->actions.count; ++q) {
            const SolMirRuntimeCleanupAction *action = &cleanup->actions[
                transition->actions.offset + q];
            if (action->kind != SOL_MIR_RUNTIME_CLEANUP_ACTION_WRITEBACK) continue;
            if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_NORMAL
                && action->flags == SOL_MIR_RUNTIME_CLEANUP_ACTION_NORMAL_ONLY
                && action->target == writeback->place && action->recipe == writeback->recipe
                && action->drop_path == SOL_MIR_RUNTIME_NONE) ++normal_matches;
            else ++failure_matches;
        }
    }
    return normal_matches == 1 && failure_matches == 0;
}

/* P4.3's method slice is intentionally smaller than the callback convention:
 * one direct monomorphic Int64 receiver and no explicit arguments.  Shared
 * calls snapshot that word as their VALUE result.  Exclusive Unit calls reuse
 * C2b's private result-word transport, but authenticate the receiver slot and
 * receiver writeback independently so no argument, projection, or alias can
 * enter the physical convention. */
static bool represented_method_exact(const SolWasmRepresentedBuildRequest *request,
    const SolMirRuntimeCall *call, const SolMirMaterializedTerminator *term,
    const SolMirRuntimeSignature *signature) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirRuntimeConventions *conventions = owner->conventions;
    const SolMirConcreteProgram *concrete = conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirRuntimeCleanup *cleanup = owner->cleanup;
    if (call->call_kind != SOL_IR_CALL_METHOD
        || call->target_kind != SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL
        || call->internal >= concrete->linkage.callable_count || call->host != SOL_MIR_LINKAGE_NONE
        || call->table != SOL_MIR_LINKAGE_NONE || call->callee.kind != SOL_MIR_RUNTIME_VALUE_NONE
        || call->callee.id != SOL_MIR_RUNTIME_NONE || signature->origin != SOL_MIR_RUNTIME_SIGNATURE_INTERNAL
        || signature->internal != call->internal || signature->slots.count != 1
        || signature->slots.offset >= conventions->signature_slot_count || call->operands.count != 1
        || term->arguments.count != 0 || term->receiver.source_expression == SOL_IR_NONE
        || call->operands.offset >= conventions->operand_count
        || call->block >= owner->image_terminator_count) return false;
    const SolMirRuntimeSignatureSlot *slot = &conventions->signature_slots[signature->slots.offset];
    const SolMirRuntimeOperand *operand = &conventions->operands[call->operands.offset];
    if (slot->role != SOL_MIR_RUNTIME_SLOT_RECEIVER || slot->formal != SOL_MIR_RUNTIME_NONE
        || slot->recipe >= concrete->representation.recipe_count
        || concrete->representation.recipes[slot->recipe].kind != SOL_MIR_RECIPE_INT64
        || !represented_scalar_recipe(concrete, slot->recipe, false)
        || operand->signature_slot != signature->slots.offset
        || operand->value.kind != SOL_MIR_RUNTIME_VALUE_MATERIALIZED_PLACE
        || operand->value.id != term->receiver.place || term->receiver.place >= m->place_count
        || term->receiver.access != slot->access || !whole_represented_place(concrete,
            term->receiver.place)) return false;
    const SolMirMaterializedPlace *place = &m->places[term->receiver.place];
    if (place->projections.count != 0 || place->local >= m->local_count
        || m->locals[place->local].kind != SOL_MIR_MATERIALIZED_LOCAL_BODY
        || place->local < m->images[call->image].locals.offset
        || place->local - m->images[call->image].locals.offset >= m->images[call->image].locals.count
        || place->final_type >= concrete->layout.type_count
        || concrete->layout.types[place->final_type].recipe != slot->recipe) return false;
    bool exclusive = slot->access == SOL_ACCESS_EXCLUSIVE;
    if ((!exclusive && slot->access != SOL_ACCESS_SHARED) || (exclusive
            ? signature->result_class != SOL_MIR_RUNTIME_RESULT_UNIT
                || signature->result >= concrete->representation.recipe_count
                || concrete->representation.recipes[signature->result].kind != SOL_MIR_RECIPE_UNIT
                || call->result.kind != SOL_MIR_RUNTIME_VALUE_NONE
                || call->result.id != SOL_MIR_RUNTIME_NONE || term->result >= m->value_count
                || call->writebacks.count != 1 || term->writebacks.count != 1
            : signature->result_class != SOL_MIR_RUNTIME_RESULT_VALUE
                || signature->result != slot->recipe
                || call->result.kind != SOL_MIR_RUNTIME_VALUE_MATERIALIZED_VALUE
                || call->result.id != term->result || term->result >= m->value_count
                || call->writebacks.count != 0 || term->writebacks.count != 0)) return false;
    const SolMirRuntimeLoweredImageTerminator *row = &owner->image_terminators[call->block];
    if (row->cleanup_event >= cleanup->event_count) return false;
    const SolMirRuntimeCleanupEvent *event = &cleanup->events[row->cleanup_event];
    size_t normal_matches = 0, forbidden = 0;
    for (size_t i = 0; i < event->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *transition = &cleanup->transitions[
            event->transitions.offset + i];
        if (transition->actions.offset > cleanup->action_count
            || transition->actions.count > cleanup->action_count - transition->actions.offset) return false;
        for (size_t q = 0; q < transition->actions.count; ++q) {
            const SolMirRuntimeCleanupAction *action = &cleanup->actions[
                transition->actions.offset + q];
            if (action->kind != SOL_MIR_RUNTIME_CLEANUP_ACTION_WRITEBACK) continue;
            if (!exclusive || transition->edge_role != SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_NORMAL
                || action->flags != SOL_MIR_RUNTIME_CLEANUP_ACTION_NORMAL_ONLY
                || action->drop_path != SOL_MIR_RUNTIME_NONE) { ++forbidden; continue; }
            if (call->writebacks.offset >= conventions->writeback_count
                || term->writebacks.offset >= m->writeback_count) return false;
            const SolMirRuntimeWriteback *writeback = &conventions->writebacks[call->writebacks.offset];
            const SolMirMaterializedWriteback *materialized = &m->writebacks[term->writebacks.offset];
            if (writeback->operand == call->operands.offset && writeback->receiver
                && writeback->formal == 0 && writeback->place == term->receiver.place
                && writeback->recipe == slot->recipe && materialized->receiver
                && materialized->formal == 0 && materialized->place == writeback->place
                && materialized->type < concrete->layout.type_count
                && concrete->layout.types[materialized->type].recipe == slot->recipe
                && action->target == writeback->place && action->recipe == writeback->recipe) ++normal_matches;
            else ++forbidden;
        }
    }
    return forbidden == 0 && normal_matches == (exclusive ? 1u : 0u);
}

static RepresentedCatalogResult represented_catalog_allocation_result(void) {
    return represented_accounting != NULL
        && represented_accounting->status == REPRESENTED_BACKEND_RESOURCE
        ? REPRESENTED_CATALOG_RESOURCE : REPRESENTED_CATALOG_ALLOCATION;
}

static const RepresentedCallbackCatalog *represented_callback_for(
    const RepresentedCallCatalogGraph *graph, size_t call) {
    if (graph == NULL) return NULL;
    const RepresentedCallbackCatalog *found = NULL;
    for (size_t i = 0; i < graph->callback_count; ++i) {
        const RepresentedCallbackCatalog *candidate = &graph->callbacks[i];
        if (candidate->call != call) continue;
        if (found != NULL) return NULL;
        found = candidate;
    }
    return found;
}

static bool represented_size_add(size_t *value, size_t add) {
    if (*value > SIZE_MAX - add) return false;
    *value += add;
    return true;
}

static bool represented_size_mul_add(size_t *value, size_t count, size_t size) {
    if (size != 0 && count > SIZE_MAX / size) return false;
    return represented_size_add(value, count * size);
}

static bool represented_runtime_ref_equal(SolMirRuntimeValueRef a, SolMirRuntimeValueRef b) {
    return a.kind == b.kind && a.id == b.id;
}

static bool represented_lowered_call_exact(const SolMirRuntimeLoweredCall *lowered,
    const SolMirRuntimeCall *call, size_t id) {
    return lowered->state == SOL_MIR_RUNTIME_LOWERED_PRESENT && lowered->call == id
        && lowered->signature == call->signature && lowered->owner_kind == call->owner_kind
        && lowered->image == call->image && lowered->body == call->predicate
        && lowered->block == call->block && lowered->call_kind == call->call_kind
        && lowered->target_kind == call->target_kind && lowered->internal == call->internal
        && lowered->host == call->host && lowered->table == call->table
        && represented_runtime_ref_equal(lowered->callee, call->callee)
        && lowered->operands.offset == call->operands.offset
        && lowered->operands.count == call->operands.count
        && represented_runtime_ref_equal(lowered->result, call->result)
        && lowered->normal_edge == call->normal_edge && lowered->failure_edge == call->failure_edge
        && lowered->writebacks.offset == call->writebacks.offset
        && lowered->writebacks.count == call->writebacks.count
        && lowered->import_id == SOL_MIR_RUNTIME_NONE
        && lowered->bound_environment_import == SOL_MIR_RUNTIME_NONE
        && lowered->entry == SOL_MIR_RUNTIME_NONE && lowered->failure_site == call->failure_site;
}

static bool represented_call_cleanup(const SolWasmRepresentedBuildRequest *request,
    const SolMirRuntimeCall *call, const SolMirMaterializedTerminator *term,
    size_t caller_image, size_t caller_block, unsigned char *resume_blocks) {
    const SolMirRuntimeCleanup *cleanup = request->program->cleanup;
    const SolMirMaterialization *m = &request->program->conventions->concrete->materialization;
    const SolMirRuntimeConventions *conventions = request->program->conventions;
    uint32_t call_limit = UINT32_C(1)
        << (SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT - 1);
    if (caller_block >= request->program->image_terminator_count
        || call->failure_site >= conventions->failure_site_count) return false;
    const SolMirRuntimeLoweredImageTerminator *row = &request->program->image_terminators[caller_block];
    if (row->cleanup_event >= cleanup->event_count || row->failure_site != call->failure_site)
        return false;
    const SolMirRuntimeCleanupEvent *event = &cleanup->events[row->cleanup_event];
    if (event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
        || event->owner != caller_image || event->block != caller_block
        || event->operation != SOL_MIR_RUNTIME_NONE
        || event->inherited_failure_site != call->failure_site
        || event->producer != SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_INVOKE
        || !event->captures_failure_detail
        || event->capture_detail_kind != SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE
        || event->transitions.offset > cleanup->transition_count
        || event->transitions.count > cleanup->transition_count - event->transitions.offset)
        return false;
    const SolMirRuntimeCleanupTransition *normal = NULL, *failure = NULL;
    for (size_t i = 0; i < event->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *transition = &cleanup->transitions[
            event->transitions.offset + i];
        if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_NORMAL) {
            if (normal != NULL || transition->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL
                || transition->continuation != term->normal_edge
                || transition->source_edge != term->normal_edge
                || term->normal_edge >= m->edge_count
                || transition->destination != m->edges[term->normal_edge].block) return false;
            normal = transition;
        } else if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE) {
            if (failure != NULL || transition->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
                || transition->continuation != term->failure_edge
                || transition->source_edge != term->failure_edge
                || term->failure_edge >= m->edge_count
                || transition->destination != m->edges[term->failure_edge].block
                || transition->failure_source
                    != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_LOCAL_OR_PENDING
                || transition->failure_site != call->failure_site
            || transition->failure_mask != call_limit) return false;
            failure = transition;
        } else return false;
    }
    if (normal == NULL || failure == NULL) return false;
    size_t successor = m->edges[term->failure_edge].block;
    if (successor >= m->block_count
        || m->blocks[successor].terminator.kind != SOL_MIR_TERM_RESUME_FAILURE) return false;
    const SolMirRuntimeCleanupEvent *resume = NULL;
    for (size_t i = 0; i < cleanup->event_count; ++i) {
        const SolMirRuntimeCleanupEvent *candidate = &cleanup->events[i];
        if (candidate->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
            && candidate->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
            && candidate->owner == caller_image && candidate->block == successor) {
            if (resume != NULL) return false;
            resume = candidate;
        }
    }
    if (resume == NULL || resume->transitions.offset > cleanup->transition_count
        || resume->transitions.count > cleanup->transition_count - resume->transitions.offset)
        return false;
    size_t pending = 0;
    for (size_t i = 0; i < resume->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *transition = &cleanup->transitions[
            resume->transitions.offset + i];
        if (transition->edge_role != SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE
            || transition->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
            || transition->continuation != SOL_MIR_RUNTIME_NONE
            || transition->failure_source != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING
            || transition->failure_site != SOL_MIR_RUNTIME_NONE || transition->failure_mask != 0)
            return false;
        ++pending;
    }
    if (pending != 1 || resume_blocks == NULL || resume_blocks[successor]) return false;
    resume_blocks[successor] = 1;
    return true;
}

/* A checked requires body can fail before its caller's INVOKE failure edge is
 * resumed.  That resume belongs to the callee's authenticated CHECK_CONTRACT
 * failure continuation, not to an independent call row in the catalog. */
static bool represented_predicate_failure_resume(const SolMirMaterialization *m, size_t resume) {
    size_t source = SOL_MIR_MATERIALIZED_NONE;
    if (resume >= m->block_count) return false;
    for (size_t block = 0; block < m->block_count; ++block) {
        const SolMirMaterializedTerminator *term = &m->blocks[block].terminator;
        if ((term->kind != SOL_MIR_TERM_CHECK_CONTRACT
                && term->kind != SOL_MIR_TERM_CHECK_REFINED)
            || term->failure_edge >= m->edge_count
            || m->edges[term->failure_edge].block != resume) continue;
        if (source != SOL_MIR_MATERIALIZED_NONE) return false;
        source = block;
    }
    return source != SOL_MIR_MATERIALIZED_NONE;
}

static RepresentedCatalogResult represented_call_catalog(const SolWasmRepresentedBuildRequest *request,
    const SolWasmRepresentedLimits *limits, RepresentedQualifiedIndex *qualified_index,
    RepresentedCallCatalogGraph *graph) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirRuntimeConventions *conventions = owner->conventions;
    const SolMirConcreteProgram *concrete = conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirLinkage *linkage = &concrete->linkage;
    memset(graph, 0, sizeof *graph);
    size_t callback_count = 0, work = 0;
    for (size_t i = 0; i < conventions->call_count; ++i)
        if (conventions->calls[i].call_kind == SOL_IR_CALL_CALLBACK) {
            if (callback_count == SIZE_MAX) return REPRESENTED_CATALOG_RESOURCE;
            ++callback_count;
        }
    if (!represented_size_mul_add(&work, conventions->call_count, sizeof *graph->calls)
        || !represented_size_mul_add(&work, m->block_count, sizeof *graph->resume_blocks)
        || !represented_size_mul_add(&work, linkage->callable_count, sizeof(size_t))
        || !represented_size_mul_add(&work, linkage->callable_count, sizeof(size_t))
        || !represented_size_mul_add(&work, linkage->callable_count, sizeof(size_t))
        || (callback_count != 0 && !represented_size_mul_add(&work, callback_count,
            sizeof *graph->callbacks)))
        return REPRESENTED_CATALOG_RESOURCE;
    graph->work_bytes = work;
    if (work > limits->max_work_bytes) return REPRESENTED_CATALOG_RESOURCE;
    graph->calls = conventions->call_count == 0 ? NULL
        : allocate(conventions->call_count, sizeof *graph->calls);
    graph->callbacks = callback_count == 0 ? NULL
        : allocate(callback_count, sizeof *graph->callbacks);
    graph->resume_blocks = m->block_count == 0 ? NULL
        : allocate(m->block_count, sizeof *graph->resume_blocks);
    if ((conventions->call_count != 0 && graph->calls == NULL)
        || (callback_count != 0 && graph->callbacks == NULL)
        || (m->block_count != 0 && graph->resume_blocks == NULL)) {
        deallocate(graph->calls); deallocate(graph->callbacks); deallocate(graph->resume_blocks);
        graph->calls = NULL; graph->callbacks = NULL; graph->resume_blocks = NULL;
        return represented_catalog_allocation_result();
    }
    for (size_t id = 0; id < conventions->call_count; ++id) {
        const SolMirRuntimeCall *call = &conventions->calls[id];
        if (id >= owner->call_count || !represented_lowered_call_exact(&owner->calls[id], call, id)
            || call->owner_kind != SOL_MIR_RUNTIME_CALL_OWNER_IMAGE
            || call->image >= m->image_count || call->block >= m->block_count
            || call->block < m->images[call->image].blocks.offset
            || call->block - m->images[call->image].blocks.offset >= m->images[call->image].blocks.count
            || (call->call_kind != SOL_IR_CALL_FUNCTION && call->call_kind != SOL_IR_CALL_CALLBACK
                && call->call_kind != SOL_IR_CALL_METHOD)
            || call->writebacks.offset > conventions->writeback_count
            || call->signature >= conventions->signature_count
            || call->failure_site >= conventions->failure_site_count
            || call->block >= owner->image_terminator_count) goto invalid;
        const SolMirMaterializedTerminator *term = &m->blocks[call->block].terminator;
        const SolMirRuntimeLoweredImageTerminator *row = &owner->image_terminators[call->block];
        const SolMirRuntimeSignature *signature = &conventions->signatures[call->signature];
        bool indirect = call->call_kind == SOL_IR_CALL_CALLBACK;
        bool method = call->call_kind == SOL_IR_CALL_METHOD;
        bool callback_inout = indirect && call->writebacks.count == 1;
        size_t callee_id = call->internal;
        if ((indirect && !represented_callback_target(request, call, term, &callee_id))
            || (!indirect && (call->target_kind != SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL
                || call->internal >= linkage->callable_count || call->host != SOL_MIR_LINKAGE_NONE
                || call->table != SOL_MIR_LINKAGE_NONE || call->callee.kind != SOL_MIR_RUNTIME_VALUE_NONE
                || call->callee.id != SOL_MIR_RUNTIME_NONE))) goto invalid;
        const SolMirLinkageCallable *callee = &linkage->callables[callee_id];
        const char *symbol = callee->symbol.bytes;
        size_t caller_callable = SOL_MIR_LINKAGE_NONE, caller_matches = 0;
        size_t binding_matches = 0;
        for (size_t i = 0; i < linkage->callable_count; ++i)
            if (linkage->callables[i].instance == call->image) {
                caller_callable = i;
                ++caller_matches;
            }
        for (size_t i = 0; !indirect && i < linkage->binding_count; ++i)
            if (linkage->bindings[i].binding == term->binding
                && linkage->bindings[i].target_kind == SOL_MIR_LINKAGE_TARGET_INTERNAL
                && linkage->bindings[i].internal == callee_id
                && linkage->bindings[i].host == SOL_MIR_LINKAGE_NONE) ++binding_matches;
        bool refined_signature = represented_signature_refined(conventions, signature);
        bool refined_entry = false;
        for (size_t i = 0; i < conventions->entry_count; ++i)
            refined_entry = refined_entry || conventions->entries[i].callable == callee_id;
        if (term->kind != SOL_MIR_TERM_INVOKE || term->call_kind != call->call_kind
            || term->normal_edge != call->normal_edge || term->failure_edge != call->failure_edge
            || term->writebacks.offset > m->writeback_count
            || term->writebacks.count > m->writeback_count - term->writebacks.offset
            || (!indirect && !method && (call->writebacks.count != 0 || term->writebacks.count != 0))
            || (method ? term->receiver.source_expression == SOL_IR_NONE
                       : term->receiver.source_expression != SOL_IR_NONE)
            || row->state != SOL_MIR_RUNTIME_LOWERED_PRESENT || row->image != call->image
            || row->block != call->block || row->kind != SOL_MIR_TERM_INVOKE || row->call != id
            || row->failure_site != call->failure_site || row->plan_family != SOL_MIR_RUNTIME_LOWERED_PLAN_CALLABLE
            || (!indirect && (signature->origin != SOL_MIR_RUNTIME_SIGNATURE_INTERNAL
                || signature->internal != callee_id || signature_for(conventions, callee_id) != signature))
            || (indirect && !represented_signature_exact(conventions, signature,
                signature_for(conventions, callee_id)))
            || (signature->result_class != SOL_MIR_RUNTIME_RESULT_VALUE
                && signature->result_class != SOL_MIR_RUNTIME_RESULT_UNIT)
            || (!represented_propagation_callable_recipe(concrete, callee->instance,
                    signature->result, false)
                && (indirect || method || represented_qualified_image(qualified_index,
                    callee->instance, signature->result) == NULL))
            || callee->instance >= m->image_count || symbol == NULL || symbol[0] == '\0'
            || symbol_for_image(concrete, callee->instance) == NULL
            || strcmp(symbol_for_image(concrete, callee->instance), symbol) != 0
            || call->operands.offset > conventions->operand_count
            || call->operands.count > conventions->operand_count - call->operands.offset
            || signature->slots.offset > conventions->signature_slot_count
            || signature->slots.count > conventions->signature_slot_count - signature->slots.offset
            || (term->arguments.count != 0 && (term->arguments.offset > m->call_argument_count
                || term->arguments.count > m->call_argument_count - term->arguments.offset))
            || call->operands.count != signature->slots.count
            || term->arguments.count != (method ? 0 : signature->slots.count)
            || caller_matches != 1 || (!indirect && binding_matches != 1)
            || (refined_signature && (indirect || method || refined_entry
                || signature->origin != SOL_MIR_RUNTIME_SIGNATURE_INTERNAL))
            || !image_edge_preflight(request, &m->images[call->image], call->image,
                call->block, call->normal_edge)
            || !image_edge_preflight(request, &m->images[call->image], call->image,
                call->block, call->failure_edge)) goto invalid;
        if (callback_inout && !represented_callback_inout_exact(request, call, term, signature))
            goto invalid;
        if (method && !represented_method_exact(request, call, term, signature)) goto invalid;
        if (indirect && !callback_inout
            && (call->writebacks.count != 0 || term->writebacks.count != 0)) goto invalid;
        for (size_t ordinal = 0; !method && ordinal < signature->slots.count; ++ordinal) {
            const SolMirRuntimeSignatureSlot *slot = &conventions->signature_slots[
                signature->slots.offset + ordinal];
            const SolMirRuntimeOperand *operand = &conventions->operands[
                call->operands.offset + ordinal];
            const SolMirMaterializedCallArgument *argument = &m->call_arguments[
                term->arguments.offset + ordinal];
            if (slot->role != (method ? SOL_MIR_RUNTIME_SLOT_RECEIVER : SOL_MIR_RUNTIME_SLOT_PARAMETER)
                || slot->formal != (method ? SOL_MIR_RUNTIME_NONE : ordinal)
                || slot->access != (method ? term->receiver.access
                    : callback_inout ? SOL_ACCESS_EXCLUSIVE : SOL_ACCESS_OWNED)
                || !represented_propagation_callable_recipe(
                    concrete, callee->instance, slot->recipe, false)
                || operand->signature_slot != signature->slots.offset + ordinal
                || operand->value.kind != ((callback_inout || method)
                    ? SOL_MIR_RUNTIME_VALUE_MATERIALIZED_PLACE
                    : SOL_MIR_RUNTIME_VALUE_MATERIALIZED_TEMPORARY)
                || (!method && (argument->formal != ordinal || argument->access
                    != (callback_inout ? SOL_ACCESS_EXCLUSIVE : SOL_ACCESS_OWNED)))
                || operand->value.id != (method ? term->receiver.place
                    : callback_inout ? argument->place : argument->temporary)
                || ((callback_inout || method) ? (method ? term->receiver.place : argument->place) >= m->place_count
                    : argument->temporary >= m->temporary_count)
                || (!method && (argument->type >= concrete->layout.type_count
                    || concrete->layout.types[argument->type].recipe != slot->recipe))
                || (!callback_inout && (m->temporaries[argument->temporary].type != argument->type
                    || argument->temporary < m->images[call->image].temporaries.offset
                    || argument->temporary - m->images[call->image].temporaries.offset
                        >= m->images[call->image].temporaries.count))) goto invalid;
        }
        if (signature->result_class == SOL_MIR_RUNTIME_RESULT_VALUE) {
            if (call->result.kind != SOL_MIR_RUNTIME_VALUE_MATERIALIZED_VALUE
                || call->result.id != term->result || !image_value(concrete,
                    &m->images[call->image], term->result)
                || m->values[term->result].type >= concrete->layout.type_count
                || concrete->layout.types[m->values[term->result].type].recipe != signature->result)
                goto invalid;
        } else if (call->result.kind != SOL_MIR_RUNTIME_VALUE_NONE
            || call->result.id != SOL_MIR_RUNTIME_NONE) goto invalid;
        const SolMirRuntimeFailureSite *site = &conventions->failure_sites[call->failure_site];
        if (site->origin_kind != SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_CALL
            || site->owner != call->image || site->block != call->block
            || site->instruction != SOL_MIR_RUNTIME_NONE
            || site->allowed_codes != (UINT32_C(1)
                << (SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT - 1))
            || !represented_call_cleanup(request, call, term, call->image, call->block,
                graph->resume_blocks)) goto invalid;
        graph->calls[id] = (RepresentedCallCatalog){call->image, call->block, caller_callable, id, callee_id,
            call->signature, call->operands, signature->result_class == SOL_MIR_RUNTIME_RESULT_VALUE
                ? term->result : SOL_MIR_MATERIALIZED_NONE, signature->result_class,
            call->normal_edge, call->failure_edge, call->failure_site, 0, false};
        if (qualified_index != NULL && qualified_index->callable_flags != NULL
            && callee_id < qualified_index->callable_count) {
            if (!indirect && !method)
                qualified_index->callable_flags[callee_id]
                    |= REPRESENTED_QUALIFIED_CALLABLE_DIRECT_TARGET;
            if (callback_inout)
                qualified_index->callable_flags[callee_id]
                    |= REPRESENTED_QUALIFIED_CALLABLE_CALLBACK_INOUT_TARGET;
            if (method)
                qualified_index->callable_flags[callee_id]
                    |= REPRESENTED_QUALIFIED_CALLABLE_METHOD_TARGET;
        }
        if (indirect) {
            if (graph->callback_count >= callback_count) goto invalid;
            graph->callbacks[graph->callback_count++] = (RepresentedCallbackCatalog){id, call->table};
        }
        ++graph->edge_count;
    }
    if (owner->call_count != conventions->call_count || graph->callback_count != callback_count) goto invalid;
    for (size_t block = 0; block < m->block_count; ++block)
        if (m->blocks[block].terminator.kind == SOL_MIR_TERM_RESUME_FAILURE
            && !graph->resume_blocks[block] && !represented_predicate_failure_resume(m, block)) goto invalid;
    graph->count = conventions->call_count;
    if (linkage->callable_count != 0) {
        size_t *indegree = allocate(linkage->callable_count, sizeof *indegree);
        size_t *depth = allocate(linkage->callable_count, sizeof *depth);
        size_t *queue = allocate(linkage->callable_count, sizeof *queue);
        if (indegree == NULL || depth == NULL || queue == NULL) {
            deallocate(indegree); deallocate(depth); deallocate(queue); deallocate(graph->calls); deallocate(graph->callbacks); deallocate(graph->resume_blocks);
            graph->calls = NULL; graph->callbacks = NULL; graph->resume_blocks = NULL;
            return represented_catalog_allocation_result();
        }
        for (size_t i = 0; i < graph->count; ++i) {
            size_t target = graph->calls[i].callee_callable;
            if (target >= linkage->callable_count || indegree[target] == SIZE_MAX) {
                deallocate(indegree); deallocate(depth); deallocate(queue); goto invalid;
            }
            ++indegree[target];
        }
        size_t head = 0, tail = 0, visited = 0;
        for (size_t i = 0; i < linkage->callable_count; ++i)
            if (indegree[i] == 0) queue[tail++] = i;
        while (head < tail) {
            size_t caller = queue[head++];
            ++visited;
            for (size_t j = 0; j < graph->count; ++j) {
                if (graph->calls[j].caller_callable != caller) continue;
                size_t target = graph->calls[j].callee_callable;
                if (depth[caller] == SIZE_MAX) { deallocate(indegree); deallocate(depth); deallocate(queue); goto invalid; }
                if (depth[target] < depth[caller] + 1) depth[target] = depth[caller] + 1;
                if (depth[target] > graph->longest_chain) graph->longest_chain = depth[target];
                if (--indegree[target] == 0) queue[tail++] = target;
            }
        }
        graph->has_cycle = visited != linkage->callable_count;
        /* Kahn establishes whether a cycle exists.  Classify each call edge
         * precisely by asking whether its target reaches its caller; this
         * distinguishes an edge into a recursive SCC from an edge that merely
         * leads to one.  The queue and indegree arrays are reused, so the
         * classification remains within the catalog's declared work budget. */
        for (size_t i = 0; i < graph->count; ++i) {
            RepresentedCallCatalog *edge = &graph->calls[i];
            memset(indegree, 0, linkage->callable_count * sizeof *indegree);
            size_t reach_head = 0, reach_tail = 0;
            queue[reach_tail++] = edge->callee_callable;
            indegree[edge->callee_callable] = 1;
            while (reach_head < reach_tail && !edge->cyclic) {
                size_t node = queue[reach_head++];
                if (node == edge->caller_callable) {
                    edge->cyclic = true;
                    break;
                }
                for (size_t j = 0; j < graph->count; ++j) {
                    size_t next = graph->calls[j].callee_callable;
                    if (graph->calls[j].caller_callable == node && !indegree[next]) {
                        indegree[next] = 1;
                        queue[reach_tail++] = next;
                    }
                }
            }
            edge->chain_depth = graph->has_cycle ? 0 : depth[edge->callee_callable];
        }
        if (graph->has_cycle) graph->longest_chain = 0;
        deallocate(indegree); deallocate(depth); deallocate(queue);
    }
    return REPRESENTED_CATALOG_VALID;
invalid:
    deallocate(graph->calls); deallocate(graph->callbacks); deallocate(graph->resume_blocks);
    graph->calls = NULL; graph->callbacks = NULL; graph->resume_blocks = NULL; graph->count = 0;
    return REPRESENTED_CATALOG_INVALID;
}

static void represented_call_catalog_free(RepresentedCallCatalogGraph *graph) {
    deallocate(graph->calls);
    deallocate(graph->callbacks);
    deallocate(graph->resume_blocks);
    memset(graph, 0, sizeof *graph);
}

static const RepresentedCallCatalog *represented_catalog_for(const RepresentedCallCatalogGraph *graph,
    size_t image, size_t block) {
    if (graph == NULL) return NULL;
    const RepresentedCallCatalog *found = NULL;
    for (size_t i = 0; i < graph->count; ++i) {
        const RepresentedCallCatalog *call = &graph->calls[i];
        if (call->caller_image == image && call->caller_block == block) {
            if (found != NULL) return NULL;
            found = call;
        }
    }
    return found;
}

/* The catalogue admits normal P4.3 producers and the three exact P4.4a
 * terminal producers only.  In particular, a new cleanup producer cannot be
 * made callable merely by adding it to the switch below. */
static bool represented_catalog_terminal_event_supported(
    const SolWasmRepresentedBuildRequest *request, const SolMirRuntimeCleanupEvent *event) {
    const SolMirRuntimeCleanup *cleanup = request->program->cleanup;
    const SolMirRuntimeConventions *conventions = request->program->conventions;
    SolMirRuntimeFailureOriginKind origin;
    SolMirRuntimeFailureCode code;
    SolMirRuntimeFailureDetailKind detail;
    switch (event->producer) {
        case SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_PANIC:
            origin = SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_PANIC;
            code = SOL_MIR_RUNTIME_FAILURE_PANIC;
            detail = SOL_MIR_RUNTIME_FAILURE_DETAIL_PANIC_TEXT;
            break;
        case SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_NO_MATCH:
            origin = SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_NO_MATCH;
            code = SOL_MIR_RUNTIME_FAILURE_NO_MATCH;
            detail = SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE;
            break;
        case SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_UNREACHABLE:
            origin = SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_UNREACHABLE;
            code = SOL_MIR_RUNTIME_FAILURE_REACHED_UNREACHABLE;
            detail = SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE;
            break;
        default: return false;
    }
    if (event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
        || event->phase != SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
        || event->origin != SOL_MIR_RUNTIME_CLEANUP_ORIGIN_EXPLICIT
        || !event->captures_failure_detail || event->capture_detail_kind != detail
        || event->inherited_failure_site >= conventions->failure_site_count
        || event->transitions.offset > cleanup->transition_count
        || event->transitions.count > cleanup->transition_count - event->transitions.offset) return false;
    const SolMirRuntimeFailureSite *site = &conventions->failure_sites[event->inherited_failure_site];
    if (site->origin_kind != origin || site->allowed_codes != (UINT32_C(1) << (code - 1))) return false;
    const SolMirRuntimeCleanupTransition *failure = NULL;
    for (size_t i = 0; i < event->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *candidate = &cleanup->transitions[
            event->transitions.offset + i];
        if (candidate->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE) continue;
        if (failure != NULL || candidate->edge_role != SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE
            || candidate->failure_source != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31
            || candidate->failure_site != event->inherited_failure_site
            || candidate->failure_mask != site->allowed_codes
            || candidate->continuation != SOL_MIR_RUNTIME_NONE
            || candidate->actions.offset > cleanup->action_count
            || candidate->actions.count > cleanup->action_count - candidate->actions.offset) return false;
        failure = candidate;
    }
    if (failure == NULL || failure->actions.count == 0) return false;
    for (size_t i = 0; i < failure->actions.count; ++i) {
        const SolMirRuntimeCleanupAction *action = &cleanup->actions[failure->actions.offset + i];
        if (!represented_cleanup_action(request, action)
            || (action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE)
                != (i + 1 == failure->actions.count)) return false;
    }
    return true;
}

/* 3B admits only supported P4.3 producers plus the independently authenticated
 * terminal set above.  An otherwise unreachable checked operation or unfamiliar
 * failure producer still defers the entire closure to 3C. */
static bool represented_catalog_failures_supported(const SolWasmRepresentedBuildRequest *request) {
    const SolMirRuntimeCleanup *cleanup = request->program->cleanup;
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    /* Graph depth/cycles are catalog metadata, not runtime admission gates.
     * Each source invocation enforces the finite active-frame limit. */
    for (size_t i = 0; i < cleanup->event_count; ++i) {
        const SolMirRuntimeCleanupEvent *event = &cleanup->events[i];
        if (event->producer != SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL
            && event->producer != SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_INVOKE
            && event->producer != SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_ARITHMETIC
            && event->producer != SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_ARITHMETIC
            && event->producer != SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_RESULT
            && event->producer != SOL_MIR_RUNTIME_CLEANUP_PRODUCER_SUPPLEMENTAL_ALLOCATION
            && event->producer != SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PROPAGATION_RESIDUAL
            && event->producer != SOL_MIR_RUNTIME_CLEANUP_PRODUCER_STEP_METER
            && !represented_catalog_terminal_event_supported(request, event))
            return false;
        if (event->transitions.offset > cleanup->transition_count
            || event->transitions.count > cleanup->transition_count - event->transitions.offset)
            return false;
        for (size_t q = 0; q < event->transitions.count; ++q) {
            const SolMirRuntimeCleanupTransition *transition = &cleanup->transitions[
                event->transitions.offset + q];
            if (transition->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE) continue;
            if (transition->actions.offset > cleanup->action_count
                || transition->actions.count > cleanup->action_count - transition->actions.offset)
                return false;
            for (size_t a = 0; a < transition->actions.count; ++a)
                if (!represented_cleanup_action(request,
                        &cleanup->actions[transition->actions.offset + a])) return false;
        }
    }
    for (size_t i = 0; i < m->instruction_count; ++i) {
        const SolMirOperationArithmeticPlan *plan = arithmetic_for_instruction(request, i);
        if (plan != NULL && !infallible_opcode(plan) && !represented_checked_opcode(plan)) return false;
    }
    return true;
}

static bool represented_compound_chain(const SolMirConcreteProgram *concrete,
    const SolMirMaterializedImage *image, const SolMirMaterializedBlock *block,
    size_t instruction, const SolMirOperationArithmeticPlan *plan) {
    const SolMirMaterialization *m = &concrete->materialization;
    if (!plan->compound || plan->previous >= m->temporary_count
        || !whole_represented_place(concrete, m->instructions[instruction].place)
        || !represented_recipe(concrete, m->temporaries[plan->previous].type, false)
        || !image_value(concrete, image, plan->right)) return false;
    bool initialized = false, updated = false;
    for (size_t i = 0; i < block->instructions.count; ++i) {
        size_t candidate = block->instructions.offset + i;
        if (candidate >= instruction) break;
        const SolMirMaterializedInstruction *item = &m->instructions[candidate];
        if (item->kind == SOL_MIR_INST_LOAD_UPDATE && item->place == m->instructions[instruction].place
            && image_value(concrete, image, item->result)) {
            updated = true;
            continue;
        }
        if (updated && item->kind == SOL_MIR_INST_TEMPORARY_INIT
            && item->temporary == plan->previous && image_value(concrete, image, item->left)) {
            initialized = true;
            continue;
        }
    }
    return updated && initialized;
}

static const SolMirOperationConstructPlan *constructor_for_instruction(
    const SolWasmRepresentedBuildRequest *request, size_t instruction) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirConcreteProgram *concrete = owner->conventions->concrete;
    if (instruction >= owner->image_instruction_count
        || instruction >= concrete->materialization.instruction_count) return NULL;
    const SolMirRuntimeLoweredImageInstruction *row = &owner->image_instructions[instruction];
    if (row->plan >= owner->semantic_plan_count) return NULL;
    const SolMirRuntimeLoweredSemanticPlan *semantic = &owner->semantic_plans[row->plan];
    if (semantic->state != SOL_MIR_RUNTIME_LOWERED_PRESENT
        || semantic->arena != SOL_MIR_RUNTIME_LOWERED_SEMANTIC_CONSTRUCT
        || semantic->plan >= concrete->operations.constructor_count) return NULL;
    const SolMirOperationConstructPlan *plan = &concrete->operations.constructors[semantic->plan];
    return plan->instruction == instruction ? plan : NULL;
}

static bool represented_scalar_product_construct(const SolWasmRepresentedBuildRequest *request,
    const SolMirMaterializedImage *image, size_t image_id, size_t instruction) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirRepresentation *r = &concrete->representation;
    const SolMirLayout *layout = &concrete->layout;
    const SolMirRuntimeValues *values = request->program->values;
    const SolMirOperationConstructPlan *plan = constructor_for_instruction(request, instruction);
    if (plan == NULL || instruction >= m->instruction_count || plan->image != image_id
        || plan->result != m->instructions[instruction].result
        || (plan->kind != SOL_MIR_OPERATION_CONSTRUCT_RECORD
            && plan->kind != SOL_MIR_OPERATION_CONSTRUCT_TUPLE)
        || (!represented_scalar_product_recipe(concrete, plan->result_recipe)
            && !represented_callable_product_recipe(concrete, plan->result_recipe))
        || plan->object_kind != SOL_MIR_LAYOUT_OBJECT_PRODUCT
        || plan->variant_layout != SOL_MIR_OPERATION_NONE
        || plan->semantic_tag != 0 || plan->wrapper_backing != SOL_MIR_RECIPE_NONE
        || plan->operands.count != r->recipes[plan->result_recipe].fields.count
        || plan->operands.offset > concrete->operations.construct_operand_count
        || plan->operands.count > concrete->operations.construct_operand_count - plan->operands.offset
        || plan->result_recipe >= values->allocation_plan_count
        || values->allocation_plans[plan->result_recipe].recipe != plan->result_recipe
        || values->allocation_plans[plan->result_recipe].kind
            != SOL_MIR_RUNTIME_ALLOCATION_PLAN_FIXED_OBJECT
        || values->allocation_plans[plan->result_recipe].object_size
            != layout->types[plan->result_recipe].object_size
        || values->allocation_plans[plan->result_recipe].object_alignment
            != layout->types[plan->result_recipe].object_alignment
        || !represented_text_allocation_route(request, instruction, &(size_t){0})) return false;
    unsigned char seen[256] = {0};
    if (plan->operands.count > sizeof seen) return false;
    for (size_t i = 0; i < plan->operands.count; ++i) {
        const SolMirOperationConstructOperand *operand = &concrete->operations.construct_operands[
            plan->operands.offset + i];
        if (operand->source_operand_ordinal != i || operand->recipe_field < r->recipes[
                plan->result_recipe].fields.offset || operand->recipe_field - r->recipes[
                plan->result_recipe].fields.offset >= plan->operands.count || seen[operand->recipe_field
                - r->recipes[plan->result_recipe].fields.offset] || operand->layout_field >= layout->field_count
            || operand->temporary < image->temporaries.offset || operand->temporary - image->temporaries.offset
                >= image->temporaries.count || operand->recipe >= r->recipe_count
            || (!represented_scalar_recipe(concrete, operand->recipe, false)
                && !represented_scalar_product_recipe(concrete, operand->recipe)
                && !represented_unbound_function_recipe(concrete, operand->recipe))
            || layout->fields[operand->layout_field].owner_recipe != plan->result_recipe
            || (layout->fields[operand->layout_field].size == 0
                && !represented_zero_width_unit_field(concrete, operand->recipe,
                    &layout->fields[operand->layout_field]))
            || (layout->fields[operand->layout_field].size != 0
                && (!layout->fields[operand->layout_field].has_storage
                    || (layout->fields[operand->layout_field].size != 1
                    && layout->fields[operand->layout_field].size != 4
                    && layout->fields[operand->layout_field].size != 8)))
            || operand->absolute_offset != layout->fields[operand->layout_field].offset)
            return false;
        seen[operand->recipe_field - r->recipes[plan->result_recipe].fields.offset] = 1;
    }
    return true;
}

/* Sum construction is deliberately a whole-object operation.  Payload paths
 * and match/pattern instructions are excluded by represented_place/preflight;
 * the only field offsets accepted here are P2's absolute active-variant
 * offsets. */
static bool represented_sum_construct(const SolWasmRepresentedBuildRequest *request,
    const SolMirMaterializedImage *image, size_t image_id, size_t instruction) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirRepresentation *r = &concrete->representation;
    const SolMirLayout *layout = &concrete->layout;
    const SolMirRuntimeValues *values = request->program->values;
    const SolMirOperationConstructPlan *plan = constructor_for_instruction(request, instruction);
    if (plan == NULL || instruction >= m->instruction_count || plan->image != image_id
        || plan->result != m->instructions[instruction].result
        || plan->kind != SOL_MIR_OPERATION_CONSTRUCT_SUM
        || !represented_sum_recipe(concrete, plan->result_recipe)
        || plan->result_recipe >= layout->type_count
        || layout->types[plan->result_recipe].object_kind != SOL_MIR_LAYOUT_OBJECT_SUM
        || plan->variant_layout >= layout->variant_count
        || layout->variants[plan->variant_layout].owner_recipe != plan->result_recipe
        || layout->variants[plan->variant_layout].tag != plan->semantic_tag
        || plan->wrapper_backing != SOL_MIR_RECIPE_NONE
        || plan->operands.offset > concrete->operations.construct_operand_count
        || plan->operands.count > concrete->operations.construct_operand_count - plan->operands.offset
        || plan->result_recipe >= values->allocation_plan_count
        || values->allocation_plans[plan->result_recipe].kind
            != SOL_MIR_RUNTIME_ALLOCATION_PLAN_FIXED_OBJECT
        || values->allocation_plans[plan->result_recipe].object_size
            != layout->types[plan->result_recipe].object_size
        || !represented_text_allocation_route(request, instruction, &(size_t){0})) return false;
    const SolMirRecipeVariant *variant = &r->variants[plan->variant_layout];
    if (plan->operands.count != variant->fields.count) return false;
    for (size_t i = 0; i < plan->operands.count; ++i) {
        const SolMirOperationConstructOperand *operand = &concrete->operations.construct_operands[
            plan->operands.offset + i];
        size_t field_id = variant->fields.offset + i;
        if (field_id >= r->field_count || operand->source_operand_ordinal != i
            || operand->recipe_field != field_id || operand->layout_field >= layout->field_count
            || operand->temporary < image->temporaries.offset || operand->temporary - image->temporaries.offset
                >= image->temporaries.count || operand->recipe >= r->recipe_count
            || !represented_recipe(concrete, operand->recipe, false)
            || layout->fields[operand->layout_field].owner_recipe != plan->result_recipe
            || layout->fields[operand->layout_field].variant != plan->variant_layout
            || operand->absolute_offset != layout->fields[operand->layout_field].offset
            || (layout->fields[operand->layout_field].size == 0
                && !represented_zero_width_unit_field(concrete, operand->recipe,
                    &layout->fields[operand->layout_field]))
            || (!layout->fields[operand->layout_field].has_storage
                && layout->fields[operand->layout_field].size != 0)) return false;
    }
    return true;
}

static bool represented_wrapper_construct(const SolWasmRepresentedBuildRequest *request,
    const SolMirMaterializedImage *image, size_t image_id, size_t instruction) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirRepresentation *r = &concrete->representation;
    const SolMirLayout *layout = &concrete->layout;
    const SolMirOperationConstructPlan *plan = constructor_for_instruction(request, instruction);
    if (plan == NULL || plan->image != image_id || plan->kind != SOL_MIR_OPERATION_CONSTRUCT_WRAPPER
        || plan->result_recipe >= r->recipe_count || r->recipes[plan->result_recipe].kind
            != SOL_MIR_RECIPE_DISTINCT || plan->wrapper_backing != r->recipes[plan->result_recipe].backing
        || plan->wrapper_backing >= r->recipe_count || plan->object_kind
            != layout->types[plan->result_recipe].object_kind || plan->operands.count != 1
        || plan->operands.offset >= concrete->operations.construct_operand_count) return false;
    const SolMirOperationConstructOperand *operand = &concrete->operations.construct_operands[
        plan->operands.offset];
    return operand->source_operand_ordinal == 0 && operand->temporary >= image->temporaries.offset
        && operand->temporary - image->temporaries.offset < image->temporaries.count
        && operand->recipe == plan->wrapper_backing && operand->recipe_field == SOL_MIR_OPERATION_NONE
        && operand->layout_field == SOL_MIR_OPERATION_NONE && operand->absolute_offset == 0
        && represented_recipe(concrete, plan->wrapper_backing, false)
        && (!represented_indirect_recipe(concrete, plan->wrapper_backing)
            || represented_text_allocation_route(request, instruction, &(size_t){0}));
}

/* Pattern rows are P2-owned, but this backend treats them as an executable
 * certificate rather than reconstructing a decision tree from syntax.  The
 * materialized instruction names the exact P2 row through the P3.6 semantic
 * table; paths and tags below are then checked against the same P2 layouts
 * consumed by the loads in the emitter. */
static const SolMirOperationPatternTest *represented_pattern_test_for_instruction(
    const SolWasmRepresentedBuildRequest *request, size_t instruction) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirConcreteProgram *concrete = owner->conventions->concrete;
    if (instruction >= owner->image_instruction_count
        || instruction >= concrete->materialization.instruction_count) return NULL;
    const SolMirRuntimeLoweredImageInstruction *row = &owner->image_instructions[instruction];
    if (row->plan >= owner->semantic_plan_count) return NULL;
    const SolMirRuntimeLoweredSemanticPlan *semantic = &owner->semantic_plans[row->plan];
    if (semantic->state != SOL_MIR_RUNTIME_LOWERED_PRESENT
        || semantic->arena != SOL_MIR_RUNTIME_LOWERED_SEMANTIC_PATTERN_TEST
        || semantic->plan >= concrete->operations.pattern_test_count) return NULL;
    const SolMirOperationPatternTest *plan = &concrete->operations.pattern_tests[semantic->plan];
    const SolMirMaterializedInstruction *item = &concrete->materialization.instructions[instruction];
    return plan->image == row->image && plan->instruction == instruction
        && plan->scrutinee == item->pattern_scrutinee && plan->result == item->result ? plan : NULL;
}

static const SolMirOperationPatternExtraction *represented_pattern_extraction_for_instruction(
    const SolWasmRepresentedBuildRequest *request, size_t instruction) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirConcreteProgram *concrete = owner->conventions->concrete;
    if (instruction >= owner->image_instruction_count
        || instruction >= concrete->materialization.instruction_count) return NULL;
    const SolMirRuntimeLoweredImageInstruction *row = &owner->image_instructions[instruction];
    if (row->plan >= owner->semantic_plan_count) return NULL;
    const SolMirRuntimeLoweredSemanticPlan *semantic = &owner->semantic_plans[row->plan];
    if (semantic->state != SOL_MIR_RUNTIME_LOWERED_PRESENT
        || semantic->arena != SOL_MIR_RUNTIME_LOWERED_SEMANTIC_PATTERN_EXTRACTION
        || semantic->plan >= concrete->operations.pattern_extraction_count) return NULL;
    const SolMirOperationPatternExtraction *plan = &concrete->operations.pattern_extractions[semantic->plan];
    const SolMirMaterializedInstruction *item = &concrete->materialization.instructions[instruction];
    return plan->image == row->image && plan->instruction == instruction
        && plan->scrutinee == item->pattern_scrutinee && plan->result == item->result
        && plan->result_recipe == item->type ? plan : NULL;
}

static bool represented_pattern_path(const SolMirConcreteProgram *concrete,
    SolMirRecipeId root, SolMirPlanSlice path, SolMirRecipeId *result) {
    const SolMirRepresentation *r = &concrete->representation;
    const SolMirLayout *layout = &concrete->layout;
    if (root >= r->recipe_count || path.offset > concrete->operations.path_step_count
        || path.count > concrete->operations.path_step_count - path.offset) return false;
    for (size_t i = 0; i < path.count; ++i) {
        const SolMirOperationPathStep *step = &concrete->operations.path_steps[path.offset + i];
        if (step->base_recipe != root || step->field_layout >= layout->field_count
            || step->field_layout >= r->field_count || step->result_recipe != r->fields[
                step->field_layout].type || layout->fields[step->field_layout].owner_recipe != root
            || layout->fields[step->field_layout].offset != step->object_offset
            || (!layout->fields[step->field_layout].has_storage
                && !represented_zero_width_unit_field(concrete, step->result_recipe,
                    &layout->fields[step->field_layout]))) return false;
        root = step->result_recipe;
    }
    *result = root;
    return true;
}

static bool represented_pattern_prefix(SolMirPlanSlice prefix, SolMirPlanSlice whole,
    const SolMirOperationPathStep *steps) {
    if (prefix.count > whole.count || prefix.offset > SIZE_MAX - prefix.count
        || whole.offset > SIZE_MAX - whole.count) return false;
    for (size_t i = 0; i < prefix.count; ++i) {
        const SolMirOperationPathStep *a = &steps[prefix.offset + i];
        const SolMirOperationPathStep *b = &steps[whole.offset + i];
        if (a->base_recipe != b->base_recipe || a->result_recipe != b->result_recipe
            || a->field_layout != b->field_layout || a->object_offset != b->object_offset) return false;
    }
    return true;
}

static bool represented_pattern_total(const SolMirConcreteProgram *concrete,
    SolIrPatternId id, size_t depth) {
    const SolIr *ir = concrete->program.ir;
    if (id >= ir->pattern_count || depth > ir->pattern_count) return false;
    const SolIrPattern *pattern = &ir->patterns[id];
    if (pattern->kind == SOL_IR_PATTERN_WILDCARD || pattern->kind == SOL_IR_PATTERN_BINDING)
        return true;
    if (pattern->kind != SOL_IR_PATTERN_RECORD && pattern->kind != SOL_IR_PATTERN_TUPLE)
        return false;
    for (size_t i = 0; i < pattern->children.count; ++i)
        if (!represented_pattern_total(concrete,
                ir->pattern_children[pattern->children.offset + i].pattern, depth + 1)) return false;
    return true;
}

/* A sum arm covers its tag only when every payload subpattern is total.  Thus
 * a tag census is not accidentally used to certify `Some(true)` as covering
 * `Some(false)`.  This is intentionally small: arbitrary usefulness proof and
 * guarded decisions remain outside Slice B1. */
static bool represented_match_total(const SolWasmRepresentedBuildRequest *request,
    SolIrExpressionId match) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolIr *ir = concrete->program.ir;
    const SolMirRepresentation *r = &concrete->representation;
    const SolIrExpression *expression;
    if (match >= ir->expression_count || (expression = &ir->expressions[match])->kind != SOL_IR_EXPR_MATCH
        || expression->as.match_expr.arms.count == 0) return false;
    SolIrArmId final = ir->arm_ids[expression->as.match_expr.arms.offset
        + expression->as.match_expr.arms.count - 1];
    if (final >= ir->arm_count) return false;
    const SolIrArm *last = &ir->arms[final];
    if (last->guard != SOL_IR_NONE || last->pattern >= ir->pattern_count) return false;
    SolIrPatternKind final_kind = ir->patterns[last->pattern].kind;
    if (final_kind == SOL_IR_PATTERN_WILDCARD || final_kind == SOL_IR_PATTERN_BINDING) {
        for (size_t i = 0; i < expression->as.match_expr.arms.count; ++i)
            if (ir->arms[ir->arm_ids[expression->as.match_expr.arms.offset + i]].guard != SOL_IR_NONE)
                return false;
        return true;
    }
    SolMirRecipeId recipe = SOL_MIR_RECIPE_NONE;
    unsigned char tags[256] = {0};
    for (size_t i = 0; i < expression->as.match_expr.arms.count; ++i) {
        SolIrArmId arm_id = ir->arm_ids[expression->as.match_expr.arms.offset + i];
        if (arm_id >= ir->arm_count || ir->arms[arm_id].guard != SOL_IR_NONE
            || ir->arms[arm_id].pattern >= ir->pattern_count) return false;
        const SolIrPattern *pattern = &ir->patterns[ir->arms[arm_id].pattern];
        if (pattern->kind != SOL_IR_PATTERN_VARIANT || pattern->variant >= ir->variant_count) return false;
        SolMirRecipeId arm_recipe = SOL_MIR_RECIPE_NONE;
        const SolMirMaterialization *m = &concrete->materialization;
        for (size_t instruction = 0; instruction < m->instruction_count; ++instruction) {
            const SolMirMaterializedInstruction *candidate = &m->instructions[instruction];
            if (candidate->kind != SOL_MIR_INST_PATTERN_TEST
                || candidate->match_expression != match || candidate->source_arm != arm_id) continue;
            const SolMirOperationPatternTest *test = represented_pattern_test_for_instruction(request,
                instruction);
            if (test == NULL || arm_recipe != SOL_MIR_RECIPE_NONE) return false;
            arm_recipe = test->scrutinee_recipe;
        }
        if (recipe == SOL_MIR_RECIPE_NONE) recipe = arm_recipe;
        if (recipe != arm_recipe || recipe >= r->recipe_count || r->recipes[recipe].variants.count > 256)
            return false;
        bool payload_total = true;
        for (size_t child = 0; child < pattern->children.count; ++child)
            payload_total = payload_total && represented_pattern_total(concrete,
                ir->pattern_children[pattern->children.offset + child].pattern, 0);
        if (!payload_total) return false;
        for (size_t v = 0; v < r->recipes[recipe].variants.count; ++v) {
            size_t variant = r->recipes[recipe].variants.offset + v;
            if (variant < r->variant_count && r->variants[variant].source_variant == pattern->variant)
                tags[v] = 1;
        }
    }
    if (recipe >= r->recipe_count || r->recipes[recipe].variants.count == 0) return false;
    for (size_t i = 0; i < r->recipes[recipe].variants.count; ++i) if (!tags[i]) return false;
    return true;
}

static bool represented_match_failure_certified(const SolWasmRepresentedBuildRequest *request,
    const SolMirMaterializedTerminator *term) {
    return term->kind == SOL_MIR_TERM_MATCH_FAILURE
        && represented_match_total(request, term->source_expression);
}

/* Terminal failures have no inferred fallback.  P3.3 must name the exact P3.1
 * site, terminal transition, and ordered cleanup actions before this backend
 * is allowed to publish a packet.  This is deliberately shared by PANIC,
 * reached UNREACHABLE, and a genuine exhausted match. */
static bool represented_terminal_failure_route(const SolWasmRepresentedBuildRequest *request,
    const SolMirMaterializedImage *image, size_t image_id, size_t block,
    SolMirTerminatorKind kind, SolMirRuntimeFailureOriginKind origin,
    SolMirRuntimeFailureCode code, SolMirRuntimeFailureDetailKind detail) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirRuntimeConventions *conventions = owner->conventions;
    const SolMirRuntimeCleanup *cleanup = owner->cleanup;
    const SolMirMaterialization *m = &conventions->concrete->materialization;
    uint32_t allowed = UINT32_C(1) << (code - 1);
    if (image == NULL || block >= m->block_count || block >= owner->image_terminator_count
        || image_id >= m->image_count || m->blocks[block].terminator.kind != kind) return false;
    const SolMirRuntimeLoweredImageTerminator *row = &owner->image_terminators[block];
    if (row->state != SOL_MIR_RUNTIME_LOWERED_PRESENT || row->image != image_id
        || row->block != block || row->kind != kind || row->cleanup_event >= cleanup->event_count
        || row->failure_site >= conventions->failure_site_count) return false;
    const SolMirRuntimeCleanupEvent *event = &cleanup->events[row->cleanup_event];
    const SolMirRuntimeFailureSite *site = &conventions->failure_sites[row->failure_site];
    if (event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
        || event->phase != SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
        || event->origin != SOL_MIR_RUNTIME_CLEANUP_ORIGIN_EXPLICIT || event->owner != image_id
        || event->block != block || event->operation != SOL_MIR_RUNTIME_NONE
        || event->semantic_site != SOL_MIR_RUNTIME_NONE
        || event->inherited_failure_site != row->failure_site
        || event->supplemental_site != SOL_MIR_RUNTIME_NONE
        || !event->captures_failure_detail || event->capture_detail_kind != detail
        || event->producer != (kind == SOL_MIR_TERM_PANIC
            ? SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_PANIC
            : kind == SOL_MIR_TERM_MATCH_FAILURE
            ? SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_NO_MATCH
            : SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_UNREACHABLE)
        || site->origin_kind != origin || site->owner != image_id || site->block != block
        || site->instruction != SOL_MIR_RUNTIME_NONE || site->allowed_codes != allowed
        || event->source.file != site->source.file || event->source.start != site->source.start
        || event->source.end != site->source.end
        || event->transitions.offset > cleanup->transition_count
        || event->transitions.count > cleanup->transition_count - event->transitions.offset) return false;
    const SolMirRuntimeCleanupTransition *failure = NULL;
    for (size_t i = 0; i < event->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *candidate = &cleanup->transitions[
            event->transitions.offset + i];
        if (candidate->event != row->cleanup_event
            || candidate->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
            || candidate->edge_role != SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE
            || candidate->failure_source != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31
            || candidate->failure_site != row->failure_site || candidate->failure_mask != allowed
            || candidate->continuation != SOL_MIR_RUNTIME_NONE
            || candidate->actions.offset > cleanup->action_count
            || candidate->actions.count > cleanup->action_count - candidate->actions.offset
            || failure != NULL) return false;
        failure = candidate;
    }
    if (failure == NULL || failure->actions.count == 0) return false;
    for (size_t i = 0; i < failure->actions.count; ++i) {
        const SolMirRuntimeCleanupAction *action = &cleanup->actions[failure->actions.offset + i];
        if (!represented_cleanup_action(request, action)
            || (action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE
                && i + 1 != failure->actions.count)
            || (action->kind != SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE
                && i + 1 == failure->actions.count)) return false;
    }
    return cleanup->actions[failure->actions.offset + failure->actions.count - 1].kind
        == SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE;
}

static bool represented_guard_call(const SolMirConcreteProgram *concrete,
    const SolMirMaterializedTerminator *term) {
    if (term->kind != SOL_MIR_TERM_INVOKE || term->source_expression >= concrete->program.ir->expression_count)
        return false;
    const SolIr *ir = concrete->program.ir;
    for (size_t arm = 0; arm < ir->arm_count; ++arm)
        if (ir->arms[arm].guard == term->source_expression) return true;
    return false;
}

static bool represented_pattern_test_supported(const SolWasmRepresentedBuildRequest *request,
    const SolMirMaterializedImage *image, size_t instruction) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirRepresentation *r = &concrete->representation;
    const SolMirLayout *layout = &concrete->layout;
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirOperationPatternTest *test = represented_pattern_test_for_instruction(request, instruction);
    if (test == NULL || test->image >= m->image_count || test->image != request->program->image_instructions[
            instruction].image || test->scrutinee < image->temporaries.offset
        || test->scrutinee - image->temporaries.offset >= image->temporaries.count
        || test->scrutinee_recipe >= r->recipe_count || test->result >= m->value_count
        || test->nodes.count == 0 || test->nodes.offset > concrete->operations.pattern_node_count
        || test->nodes.count > concrete->operations.pattern_node_count - test->nodes.offset) return false;
    for (size_t i = 0; i < test->nodes.count; ++i) {
        const SolMirOperationPatternNode *node = &concrete->operations.pattern_nodes[test->nodes.offset + i];
        SolMirRecipeId reached = SOL_MIR_RECIPE_NONE, physical = SOL_MIR_RECIPE_NONE;
        if (node->recipe >= r->recipe_count || !represented_pattern_path(concrete,
                test->scrutinee_recipe, node->path, &reached) || reached != node->recipe
            || !represented_backing_recipe(concrete, reached, &physical)) return false;
        for (size_t p = 0; p < node->path.count; ++p) {
            const SolMirOperationPathStep *step = &concrete->operations.path_steps[node->path.offset + p];
            SolMirRecipeId base;
            if (!represented_backing_recipe(concrete, step->base_recipe, &base)
                || !represented_sum_recipe_depth(concrete, base, 0)) continue;
            bool prior = false;
            for (size_t earlier = 0; earlier < i; ++earlier) {
                const SolMirOperationPatternNode *parent = &concrete->operations.pattern_nodes[
                    test->nodes.offset + earlier];
                SolMirRecipeId parent_physical;
                if (parent->kind == SOL_MIR_OPERATION_PATTERN_SUM_TAG
                    && parent->recipe == step->base_recipe
                    && represented_backing_recipe(concrete, parent->recipe, &parent_physical)
                    && parent_physical == base && parent->path.count == p
                    && represented_pattern_prefix(parent->path, node->path,
                        concrete->operations.path_steps)) prior = true;
            }
            if (!prior) return false;
        }
        if (node->kind == SOL_MIR_OPERATION_PATTERN_WILDCARD
            || node->kind == SOL_MIR_OPERATION_PATTERN_BINDING) continue;
        if (node->kind == SOL_MIR_OPERATION_PATTERN_PRODUCT) {
            if (!represented_scalar_product_recipe(concrete, physical)) return false;
            continue;
        }
        if (node->kind == SOL_MIR_OPERATION_PATTERN_BOOL) {
            if (r->recipes[physical].kind != SOL_MIR_RECIPE_BOOL) return false;
            continue;
        }
        if (node->kind != SOL_MIR_OPERATION_PATTERN_SUM_TAG
            || !represented_sum_recipe_depth(concrete, physical, 0)) return false;
        bool tag = false;
        for (size_t v = 0; v < r->recipes[physical].variants.count; ++v) {
            size_t variant = r->recipes[physical].variants.offset + v;
            if (variant < layout->variant_count && layout->variants[variant].tag == node->semantic_tag)
                tag = true;
        }
        if (!tag) return false;
        /* A path into any sum payload is executable only after its matching
         * flattened tag node.  The preorder node sequence is the P2 proof of
         * this short-circuit ordering. */
        for (size_t p = 0; p < node->path.count; ++p) {
            const SolMirOperationPathStep *step = &concrete->operations.path_steps[node->path.offset + p];
            SolMirRecipeId base;
            if (!represented_backing_recipe(concrete, step->base_recipe, &base)
                || !represented_sum_recipe_depth(concrete, base, 0)) continue;
            bool prior = false;
            for (size_t earlier = 0; earlier < i; ++earlier) {
                const SolMirOperationPatternNode *parent = &concrete->operations.pattern_nodes[
                    test->nodes.offset + earlier];
                SolMirRecipeId parent_physical;
                if (parent->kind == SOL_MIR_OPERATION_PATTERN_SUM_TAG
                    && parent->recipe == step->base_recipe
                    && represented_backing_recipe(concrete, parent->recipe, &parent_physical)
                    && parent_physical == base && parent->path.count == p
                    && represented_pattern_prefix(parent->path, node->path,
                        concrete->operations.path_steps)) prior = true;
            }
            if (!prior) return false;
        }
    }
    return true;
}

static bool represented_pattern_extraction_supported(const SolWasmRepresentedBuildRequest *request,
    const SolMirMaterializedImage *image, size_t instruction) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirRepresentation *r = &concrete->representation;
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirOperationPatternExtraction *plan = represented_pattern_extraction_for_instruction(request,
        instruction);
    SolMirRecipeId reached = SOL_MIR_RECIPE_NONE;
    if (plan == NULL || plan->image >= m->image_count || plan->scrutinee < image->temporaries.offset
        || plan->scrutinee - image->temporaries.offset >= image->temporaries.count
        || plan->scrutinee_recipe >= r->recipe_count || plan->result_recipe >= r->recipe_count
        || !represented_pattern_path(concrete, plan->scrutinee_recipe, plan->path, &reached)
        || reached != plan->result_recipe || plan->result >= m->value_count
        || plan->copy_kind != r->recipes[plan->result_recipe].copy_kind
        || plan->copy_kind == SOL_MIR_COPY_FORBIDDEN || plan->copy_kind == SOL_MIR_COPY_UNREACHABLE)
        return false;
    const SolMirMaterializedInstruction *item = &m->instructions[instruction];
    for (size_t p = 0; p < plan->path.count; ++p) {
        const SolMirOperationPathStep *step = &concrete->operations.path_steps[plan->path.offset + p];
        SolMirRecipeId base;
        if (!represented_backing_recipe(concrete, step->base_recipe, &base)
            || !represented_sum_recipe_depth(concrete, base, 0)) continue;
        bool active = false;
        for (size_t q = 0; q < m->instruction_count; ++q) {
            const SolMirMaterializedInstruction *candidate = &m->instructions[q];
            if (candidate->kind != SOL_MIR_INST_PATTERN_TEST
                || candidate->match_expression != item->match_expression
                || candidate->source_arm != item->source_arm) continue;
            const SolMirOperationPatternTest *test = represented_pattern_test_for_instruction(request, q);
            if (test == NULL) return false;
            for (size_t n = 0; n < test->nodes.count; ++n) {
                const SolMirOperationPatternNode *node = &concrete->operations.pattern_nodes[
                    test->nodes.offset + n];
                SolMirRecipeId physical;
                if (node->kind == SOL_MIR_OPERATION_PATTERN_SUM_TAG
                    && node->recipe == step->base_recipe
                    && node->path.count == p && represented_backing_recipe(concrete, node->recipe,
                        &physical) && physical == base && represented_pattern_prefix(node->path,
                            plan->path, concrete->operations.path_steps)) active = true;
            }
        }
        if (!active) return false;
    }
    if (plan->copy_kind == SOL_MIR_COPY_TRIVIAL)
        return represented_scalar_recipe(concrete, plan->result_recipe, false);
    if (plan->copy_kind == SOL_MIR_COPY_TEXT || plan->copy_kind == SOL_MIR_COPY_AGGREGATE
        || plan->copy_kind == SOL_MIR_COPY_WRAPPER) {
        if (!represented_recipe(concrete, plan->result_recipe, false)
            || !represented_text_allocation_route(request, instruction, &(size_t){0})) return false;
        return represented_text_recipe(concrete, plan->result_recipe)
            || represented_product_recipe(concrete, plan->result_recipe)
            || represented_sum_recipe(concrete, plan->result_recipe);
    }
    return false;
}

static bool represented_function_preflight(const SolWasmRepresentedBuildRequest *request,
    size_t callable, const SolMirRuntimeSignature *signature,
    const RepresentedCallCatalogGraph *catalog, const RepresentedQualifiedIndex *qualified_index) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirMaterialization *materialization = &concrete->materialization;
    const SolMirLinkageCallable *linkage = &concrete->linkage.callables[callable];
    bool qualified_result = signature != NULL
        && signature->result_class == SOL_MIR_RUNTIME_RESULT_VALUE
        && represented_qualified_image(qualified_index, linkage->instance,
            signature->result) != NULL;
    unsigned char callable_flags = qualified_index != NULL
            && qualified_index->callable_flags != NULL
            && callable < qualified_index->callable_count
        ? qualified_index->callable_flags[callable] : 0;
    bool direct_target = (callable_flags & REPRESENTED_QUALIFIED_CALLABLE_DIRECT_TARGET) != 0;
    bool entry = (callable_flags & REPRESENTED_QUALIFIED_CALLABLE_ENTRY) != 0;
    if (qualified_index == NULL || qualified_index->callable_flags == NULL) {
        if (catalog != NULL) for (size_t i = 0; i < catalog->count; ++i) {
            const RepresentedCallCatalog *call = &catalog->calls[i];
            direct_target = direct_target || (call->callee_callable == callable
                && call->call < request->program->conventions->call_count
                && request->program->conventions->calls[call->call].call_kind
                    == SOL_IR_CALL_FUNCTION);
        }
        for (size_t i = 0; i < request->program->conventions->entry_count; ++i)
            entry = entry || request->program->conventions->entries[i].callable == callable;
    }
    if (linkage->instance >= materialization->image_count || signature == NULL
        || signature->slots.offset > request->program->conventions->signature_slot_count
        || signature->slots.count > request->program->conventions->signature_slot_count
            - signature->slots.offset || (!represented_propagation_callable_recipe(concrete,
                linkage->instance, signature->result,
                 signature->result_class == SOL_MIR_RUNTIME_RESULT_NEVER)
            && !(signature->result_class == SOL_MIR_RUNTIME_RESULT_VALUE
                && represented_qualified_image(qualified_index, linkage->instance,
                    signature->result) != NULL)) || (qualified_result && (!direct_target || entry)))
        return false;
    if (represented_signature_refined(request->program->conventions, signature)
        && (entry || !direct_target || signature->origin != SOL_MIR_RUNTIME_SIGNATURE_INTERNAL))
        return false;
    bool c2b_target = (callable_flags
        & REPRESENTED_QUALIFIED_CALLABLE_CALLBACK_INOUT_TARGET) != 0;
    bool method_target = (callable_flags & REPRESENTED_QUALIFIED_CALLABLE_METHOD_TARGET) != 0;
    if (qualified_index == NULL || qualified_index->callable_flags == NULL) {
        if (catalog != NULL) for (size_t i = 0; i < catalog->count; ++i) {
            const RepresentedCallCatalog *call = &catalog->calls[i];
            if (call->callee_callable != callable
                || call->call >= request->program->conventions->call_count) continue;
            const SolMirRuntimeCall *source = &request->program->conventions->calls[call->call];
            if (source->call_kind == SOL_IR_CALL_CALLBACK && source->writebacks.count == 1)
                c2b_target = true;
            if (source->call_kind == SOL_IR_CALL_METHOD) method_target = true;
        }
    }
    if ((c2b_target || method_target) && (signature->slots.count != 1
            || signature->slots.offset >= request->program->conventions->signature_slot_count
            || request->program->conventions->signature_slots[signature->slots.offset].role
                != (method_target ? SOL_MIR_RUNTIME_SLOT_RECEIVER : SOL_MIR_RUNTIME_SLOT_PARAMETER)
            || request->program->conventions->signature_slots[signature->slots.offset].recipe
                >= concrete->representation.recipe_count
            || concrete->representation.recipes[request->program->conventions->signature_slots[
                signature->slots.offset].recipe].kind != SOL_MIR_RECIPE_INT64
            || (method_target && request->program->conventions->signature_slots[
                signature->slots.offset].access != SOL_ACCESS_SHARED
                && request->program->conventions->signature_slots[signature->slots.offset].access
                    != SOL_ACCESS_EXCLUSIVE)
            || (method_target && request->program->conventions->signature_slots[
                signature->slots.offset].access == SOL_ACCESS_SHARED
                && (signature->result_class != SOL_MIR_RUNTIME_RESULT_VALUE
                    || signature->result != request->program->conventions->signature_slots[
                        signature->slots.offset].recipe))
            || (method_target && request->program->conventions->signature_slots[
                signature->slots.offset].access == SOL_ACCESS_EXCLUSIVE
                && (signature->result_class != SOL_MIR_RUNTIME_RESULT_UNIT
                    || signature->result >= concrete->representation.recipe_count
                    || concrete->representation.recipes[signature->result].kind != SOL_MIR_RECIPE_UNIT))
            || (c2b_target && (signature->result_class != SOL_MIR_RUNTIME_RESULT_UNIT
            || signature->result >= concrete->representation.recipe_count
            || concrete->representation.recipes[signature->result].kind != SOL_MIR_RECIPE_UNIT
            || request->program->conventions->signature_slots[signature->slots.offset].access
                != SOL_ACCESS_EXCLUSIVE)))) return false;
    for (size_t i = 0; i < signature->slots.count; ++i) {
        const SolMirRuntimeSignatureSlot *slot = &request->program->conventions->signature_slots[
            signature->slots.offset + i];
        if (slot->role != (method_target ? SOL_MIR_RUNTIME_SLOT_RECEIVER
                : SOL_MIR_RUNTIME_SLOT_PARAMETER)
            || slot->formal != (method_target ? SOL_MIR_RUNTIME_NONE : i)
            || (!method_target && slot->access
                != (c2b_target ? SOL_ACCESS_EXCLUSIVE : SOL_ACCESS_OWNED))
            || !represented_propagation_callable_recipe(concrete,
                linkage->instance, slot->recipe, false))
            return false;
    }
    const SolMirMaterializedImage *image = &materialization->images[linkage->instance];
    for (size_t body = 0; body < concrete->operations.predicate_body_count; ++body)
        if (concrete->operations.predicate_bodies[body].owner_kind
                == SOL_MIR_PREDICATE_OWNER_INSTANCE
            && concrete->operations.predicate_bodies[body].instance == linkage->instance
            && !represented_predicate_body_preflight(request, qualified_index, linkage->instance,
                signature, body)) return false;
    for (size_t i = 0; i < image->locals.count; ++i) {
        const SolMirMaterializedLocal *local = &materialization->locals[image->locals.offset + i];
        if (local->kind == (method_target ? SOL_MIR_MATERIALIZED_LOCAL_RECEIVER
                : SOL_MIR_MATERIALIZED_LOCAL_PARAMETER)) {
            if (local->ordinal >= signature->slots.count) return false;
            const SolMirRuntimeSignatureSlot *slot = &request->program->conventions->signature_slots[
                signature->slots.offset + local->ordinal];
            if (slot->formal != (method_target ? SOL_MIR_RUNTIME_NONE : local->ordinal)
                || (!method_target && slot->access
                    != (c2b_target ? SOL_ACCESS_EXCLUSIVE : SOL_ACCESS_OWNED))
                || !represented_propagation_callable_recipe(concrete, linkage->instance,
                    slot->recipe, false)
                || local->type >= concrete->layout.type_count
                || concrete->layout.types[local->type].recipe != slot->recipe) return false;
        }
    }
    for (size_t n = 0; n < image->blocks.count; ++n) {
        size_t block_id = image->blocks.offset + n;
        if (block_id >= materialization->block_count
            || request->program->image_blocks[block_id].image != linkage->instance
            || request->program->image_terminators[block_id].image != linkage->instance) return false;
        const SolMirMaterializedBlock *block = &materialization->blocks[block_id];
        if (block->parameters.offset > materialization->parameter_value_count
            || block->parameters.count > materialization->parameter_value_count - block->parameters.offset)
            return false;
        for (size_t i = 0; i < block->parameters.count; ++i) {
            SolMirMaterializedValueId value = materialization->parameter_values[
                block->parameters.offset + i];
            if (!image_value(concrete, image, value))
                return false;
        }
        for (size_t i = 0; i < block->instructions.count; ++i) {
            size_t instruction = block->instructions.offset + i;
            if (instruction >= materialization->instruction_count
                || request->program->image_instructions[instruction].image != linkage->instance)
                return false;
            const SolMirMaterializedInstruction *item = &materialization->instructions[instruction];
            const SolMirRuntimeLoweredImageInstruction *lowered_instruction
                = &request->program->image_instructions[instruction];
            if (!represented_instruction(concrete, item)
                || !represented_step_route(request,
                    SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION, linkage->instance,
                    block_id, instruction, lowered_instruction->step_cleanup_event,
                    lowered_instruction->step_failure_site)) return false;
            if ((item->kind == SOL_MIR_INST_CONST_TEXT
                    || (item->kind == SOL_MIR_INST_LOAD_COPY && item->place < materialization->place_count
                        && materialization->places[item->place].final_type < concrete->layout.type_count
                        && represented_indirect_recipe(concrete, concrete->layout.types[
                            materialization->places[item->place].final_type].recipe)))
                && !represented_text_allocation_route(request, instruction, &(size_t){0})) return false;
            if (item->result != SOL_MIR_MATERIALIZED_NONE
                && !image_value(concrete, image, item->result)) return false;
            switch (item->kind) {
                case SOL_MIR_INST_PATTERN_TEST:
                    if (!represented_pattern_test_supported(request, image, instruction)) return false;
                    break;
                case SOL_MIR_INST_PATTERN_VALUE:
                    if (!represented_pattern_extraction_supported(request, image, instruction)) return false;
                    break;
                case SOL_MIR_INST_MATCH_ARM:
                    /* The CFG has already evaluated a guard before it reaches
                     * its arm marker.  Keep the marker authenticated, but do
                     * not mistake a guarded arm for an unsupported intrinsic. */
                    if (item->source_arm >= concrete->program.ir->arm_count) return false;
                    break;
                case SOL_MIR_INST_CONSTRUCT:
                    {
                        const SolMirOperationConstructPlan *construct = constructor_for_instruction(
                            request, instruction);
                        if (construct == NULL || (construct->kind == SOL_MIR_OPERATION_CONSTRUCT_RECORD
                                || construct->kind == SOL_MIR_OPERATION_CONSTRUCT_TUPLE
                                ? !represented_scalar_product_construct(request, image, linkage->instance,
                                    instruction)
                                : construct->kind == SOL_MIR_OPERATION_CONSTRUCT_SUM
                                ? !represented_sum_construct(request, image, linkage->instance, instruction)
                                : construct->kind == SOL_MIR_OPERATION_CONSTRUCT_WRAPPER
                                ? !represented_wrapper_construct(request, image, linkage->instance, instruction)
                                : false)) return false;
                    }
                    break;
                case SOL_MIR_INST_FUNCTION_VALUE:
                    if (!represented_function_value_route(request, instruction, NULL, NULL, NULL))
                        return false;
                    break;
                case SOL_MIR_INST_CAPTURE_SNAPSHOT: {
                    const SolMirOperationSnapshotPlan *snapshot = represented_snapshot_for_instruction(
                        request, instruction, NULL);
                    if (snapshot == NULL || snapshot->image != linkage->instance
                        || snapshot->local < image->locals.offset
                        || snapshot->local - image->locals.offset >= image->locals.count)
                        return false;
                    break;
                }
                case SOL_MIR_INST_LOAD_COPY: case SOL_MIR_INST_LOAD_MOVE:
                case SOL_MIR_INST_LOAD_UPDATE: case SOL_MIR_INST_STORE:
                case SOL_MIR_INST_COMPOUND_UPDATE:
                case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED:
                    if (!image_place(concrete, image, item->place)) return false;
                    if (item->kind == SOL_MIR_INST_LOAD_UPDATE
                        && materialization->places[item->place].projections.count != 0) return false;
                    if (item->kind == SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED
                        && materialization->places[item->place].projections.count != 0
                        && !represented_projected_pre_store_action(request, image, instruction, NULL))
                        return false;
                    if (item->kind == SOL_MIR_INST_STORE
                        && materialization->places[item->place].projections.count != 0) {
                        const SolMirFieldLayout *repair_field = NULL;
                        if (!represented_callable_field_repair(request, image, instruction,
                                &repair_field) || repair_field == NULL) return false;
                    }
                    break;
                default: break;
            }
            if ((item->kind == SOL_MIR_INST_STORE || item->kind == SOL_MIR_INST_TEMPORARY_INIT
                    || item->kind == SOL_MIR_INST_EXPRESSION_RESULT)
                && !image_value(concrete, image, item->left)) return false;
            if ((item->kind == SOL_MIR_INST_LOAD_COPY || item->kind == SOL_MIR_INST_LOAD_MOVE
                    || item->kind == SOL_MIR_INST_LOAD_UPDATE)
                && item->place < materialization->place_count) {
                const SolMirMaterializedPlace *place = &materialization->places[item->place];
                bool aggregate_root = place->root_type < concrete->layout.type_count
                    && (represented_product_recipe(concrete, concrete->layout.types[
                        place->root_type].recipe) || represented_sum_recipe(concrete,
                            concrete->layout.types[place->root_type].recipe));
                bool callable_field_move = item->kind == SOL_MIR_INST_LOAD_MOVE
                    && place->root_type < concrete->layout.type_count
                    && place->final_type < concrete->layout.type_count
                    && place->projections.count == 1
                    && represented_callable_product_recipe(concrete, concrete->layout.types[
                        place->root_type].recipe)
                    && represented_unbound_function_recipe(concrete, concrete->layout.types[
                        place->final_type].recipe);
                if (callable_field_move) {
                    size_t projection = place->projections.offset;
                    size_t callable_fields = 0;
                    SolMirRecipeId root_recipe = concrete->layout.types[place->root_type].recipe;
                    if (projection >= concrete->layout.projection_count) return false;
                    for (size_t f = 0; f < concrete->representation.recipes[root_recipe].fields.count; ++f) {
                        size_t field = concrete->representation.recipes[root_recipe].fields.offset + f;
                        if (field >= concrete->layout.field_count || field >= concrete->representation.field_count)
                            return false;
                        if (represented_unbound_function_recipe(concrete,
                                concrete->representation.fields[field].type)) {
                            if (concrete->layout.projections[projection].field_layout != field) return false;
                            ++callable_fields;
                        }
                    }
                    if (callable_fields != 1) return false;
                }
                bool copy_projection = item->kind == SOL_MIR_INST_LOAD_COPY
                    && place->final_type < concrete->layout.type_count;
                bool callable_product_copy = item->kind == SOL_MIR_INST_LOAD_COPY
                    && place->final_type < concrete->layout.type_count
                    && represented_callable_product_recipe(concrete, concrete->layout.types[
                        place->final_type].recipe);
                /* A whole aggregate move transfers its sole handle and clears
                 * the root init bit.  Projected moves would require runtime
                 * holes and remain outside the represented closure. */
                if (aggregate_root && place->projections.count != 0 && !copy_projection) return false;
                if (place->projections.count != 0 && item->kind == SOL_MIR_INST_LOAD_MOVE
                    && !callable_field_move) return false;
                if (item->kind == SOL_MIR_INST_LOAD_MOVE && place->projections.count == 0
                    && represented_callable_product_recipe(concrete, concrete->layout.types[
                        place->final_type].recipe)
                    && !represented_callable_product_whole_transfer(request, image, instruction))
                    return false;
                if (callable_product_copy) return false;
            }
            if (item->kind == SOL_MIR_INST_TEMPORARY_DROP
                && (item->temporary >= materialization->temporary_count
                    || !represented_recipe(concrete,
                        materialization->temporaries[item->temporary].type, false))) return false;
            if (item->kind == SOL_MIR_INST_UNARY || item->kind == SOL_MIR_INST_BINARY
                || item->kind == SOL_MIR_INST_COMPOUND_UPDATE) {
                const SolMirOperationArithmeticPlan *plan = arithmetic_for_instruction(request,
                    instruction);
                if (plan == NULL || plan->compound != (item->kind == SOL_MIR_INST_COMPOUND_UPDATE)
                    || (plan->compound && (plan->previous >= materialization->temporary_count
                        || !represented_recipe(concrete,
                            materialization->temporaries[plan->previous].type, false)))
                    || (!plan->compound && !image_value(concrete, image, plan->left))
                    || (plan->opcode != SOL_MIR_OPERATION_BOOL_NOT
                        && plan->opcode != SOL_MIR_OPERATION_I64_NEG
                        && !image_value(concrete, image, plan->right))
                    || (plan->compound && !represented_compound_chain(concrete, image, block,
                        instruction, plan))
                    || (!infallible_opcode(plan) && !represented_checked_opcode(plan))
                    || (represented_checked_opcode(plan)
                        && !represented_failure_route(request, instruction, plan))
                    || (plan->operand_recipe >= concrete->representation.recipe_count)) return false;
                if ((plan->opcode == SOL_MIR_OPERATION_VALUE_EQ
                        || plan->opcode == SOL_MIR_OPERATION_VALUE_NE)
                    && represented_callable_product_recipe(concrete, plan->operand_recipe)) return false;
                if ((plan->opcode == SOL_MIR_OPERATION_VALUE_EQ
                        || plan->opcode == SOL_MIR_OPERATION_VALUE_NE)
                    && plan->operand_recipe < concrete->representation.recipe_count
                    && concrete->representation.recipes[plan->operand_recipe].kind
                        == SOL_MIR_RECIPE_REFINED
                    && !represented_refined_equality(concrete, plan)) return false;
                if ((plan->opcode == SOL_MIR_OPERATION_VALUE_EQ
                        || plan->opcode == SOL_MIR_OPERATION_VALUE_NE)
                    && (represented_product_recipe(concrete, plan->operand_recipe)
                        || represented_sum_recipe(concrete, plan->operand_recipe))
                    && (plan->equality.count == 0 || plan->equality.offset
                        >= concrete->operations.equality_node_count
                        || concrete->operations.equality_nodes[plan->equality.offset].recipe
                            != plan->operand_recipe
                        || (concrete->representation.recipes[plan->operand_recipe].kind
                                != SOL_MIR_RECIPE_DISTINCT
                            && represented_product_recipe(concrete, plan->operand_recipe)
                            && concrete->operations.equality_nodes[plan->equality.offset].kind
                                != SOL_MIR_OPERATION_EQUAL_PRODUCT)
                        || (concrete->representation.recipes[plan->operand_recipe].kind
                                != SOL_MIR_RECIPE_DISTINCT
                            && represented_sum_recipe(concrete, plan->operand_recipe)
                            && concrete->operations.equality_nodes[plan->equality.offset].kind
                                != SOL_MIR_OPERATION_EQUAL_SUM)
                        || (concrete->representation.recipes[plan->operand_recipe].kind
                                == SOL_MIR_RECIPE_DISTINCT
                            && concrete->operations.equality_nodes[plan->equality.offset].kind
                                != SOL_MIR_OPERATION_EQUAL_WRAPPER))) return false;
            }
        }
        const SolMirMaterializedTerminator *term = &block->terminator;
        const SolMirRuntimeLoweredImageTerminator *lowered_terminator
            = &request->program->image_terminators[block_id];
        if (!represented_step_route(request,
                SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR, linkage->instance, block_id,
                SOL_MIR_RUNTIME_NONE, lowered_terminator->step_cleanup_event,
                lowered_terminator->step_failure_site)) return false;
        const RepresentedCallCatalog *call = represented_catalog_for(catalog, linkage->instance, block_id);
        if ((!represented_terminator(term) && term->kind != SOL_MIR_TERM_RESUME_FAILURE)
            || (term->kind == SOL_MIR_TERM_INVOKE && call == NULL)
            || (term->kind != SOL_MIR_TERM_INVOKE && call != NULL)
            || represented_guard_call(concrete, term)) return false;
        switch (term->kind) {
            case SOL_MIR_TERM_GOTO: case SOL_MIR_TERM_BREAK: case SOL_MIR_TERM_CONTINUE:
                if (represented_control_transition(request, image, qualified_index,
                        linkage->instance, block_id,
                        SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO,
                        SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL, term->edge, 1) == NULL
                    || !image_edge_preflight(request, image, linkage->instance, block_id, term->edge))
                    return false;
                break;
            case SOL_MIR_TERM_BRANCH:
                if (!image_value(concrete, image, term->condition)
                    || represented_control_transition(request, image, qualified_index,
                        linkage->instance, block_id,
                        SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_TRUE,
                        SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL, term->true_edge, 2) == NULL
                    || represented_control_transition(request, image, qualified_index,
                        linkage->instance, block_id,
                        SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_FALSE,
                        SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL, term->false_edge, 2) == NULL
                    || !image_edge_preflight(request, image, linkage->instance, block_id, term->true_edge)
                    || !image_edge_preflight(request, image, linkage->instance, block_id, term->false_edge)) return false;
                break;
            case SOL_MIR_TERM_RETURN:
                if (signature->result_class != SOL_MIR_RUNTIME_RESULT_UNIT
                    && !image_value(concrete, image, term->value)) return false;
                {
                    size_t snapshots = 0;
                    if (!represented_snapshot_count_for_image(concrete, linkage->instance, &snapshots)
                        || (snapshots == 0 ? represented_control_transition(request, image,
                                qualified_index, linkage->instance, block_id,
                                SOL_MIR_RUNTIME_CLEANUP_EDGE_RETURN,
                                SOL_MIR_RUNTIME_CLEANUP_OUTCOME_EXIT, SOL_MIR_RUNTIME_NONE, 1)
                            : represented_snapshot_return_transition(request, image,
                                linkage->instance, block_id)) == NULL) return false;
                }
                break;
            case SOL_MIR_TERM_INVOKE:
                break;
            case SOL_MIR_TERM_CHECK_CONTRACT:
                if ((term->contract_phase != SOL_CONTRACT_REQUIRES
                        && term->contract_phase != SOL_CONTRACT_ENSURES)
                    || (term->contract_outcome != SOL_CONTRACT_OUTCOME_ALWAYS
                        && (term->contract_phase != SOL_CONTRACT_ENSURES
                            || (term->contract_outcome != SOL_CONTRACT_OUTCOME_SUCCESS
                                && term->contract_outcome != SOL_CONTRACT_OUTCOME_FAILURE)
                            || represented_qualified_image(qualified_index, linkage->instance,
                                signature->result) == NULL))
                    || !image_edge_preflight(request, image, linkage->instance, block_id,
                        term->satisfied_edge)
                    || !image_edge_preflight(request, image, linkage->instance, block_id,
                        term->violation_edge)
                    || !image_edge_preflight(request, image, linkage->instance, block_id,
                        term->failure_edge)) return false;
                break;
            case SOL_MIR_TERM_CHECK_REFINED: {
                RepresentedRefinementCheck check;
                if (!represented_refinement_check(request, qualified_index, linkage->instance,
                        block_id, &check)
                    || !image_edge_preflight(request, image, linkage->instance, block_id,
                        term->normal_edge)
                    || !image_edge_preflight(request, image, linkage->instance, block_id,
                        term->failure_edge)) return false;
                (void)check;
                break;
            }
            case SOL_MIR_TERM_CONTRACT_VIOLATION:
            case SOL_MIR_TERM_RESUME_FAILURE:
                break;
            case SOL_MIR_TERM_PANIC:
                if (!image_value(concrete, image, term->value)
                    || !represented_text_recipe(concrete, concrete->layout.types[
                        materialization->values[term->value].type].recipe)
                    || !represented_terminal_failure_route(request, image, linkage->instance, block_id,
                        term->kind, SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_PANIC,
                        SOL_MIR_RUNTIME_FAILURE_PANIC,
                        SOL_MIR_RUNTIME_FAILURE_DETAIL_PANIC_TEXT)) return false;
                break;
            case SOL_MIR_TERM_MATCH_FAILURE:
                if (!represented_match_failure_certified(request, term)
                    && !represented_terminal_failure_route(request, image, linkage->instance, block_id,
                        term->kind, SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_NO_MATCH,
                        SOL_MIR_RUNTIME_FAILURE_NO_MATCH,
                        SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE)) return false;
                break;
            case SOL_MIR_TERM_UNREACHABLE:
                if (!represented_terminal_failure_route(request, image, linkage->instance, block_id,
                        term->kind, SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_UNREACHABLE,
                        SOL_MIR_RUNTIME_FAILURE_REACHED_UNREACHABLE,
                        SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE)) return false;
                break;
            case SOL_MIR_TERM_PROPAGATE: {
                const SolMirOperationPropagationPlan *plan = NULL;
                const SolMirRuntimeCleanupTransition *value = NULL, *residual = NULL;
                size_t pre_event = 0, supplemental = 0;
                bool plan_ok = represented_propagation_plan(request, linkage->instance, block_id, &plan);
                bool pre_ok = plan_ok && represented_propagation_pre_event(request, linkage->instance, block_id,
                    plan, &pre_event, &supplemental);
                bool main_ok = pre_ok && represented_propagation_main_event(request, linkage->instance, block_id,
                    plan, &value, &residual);
                if (!plan_ok || !pre_ok || !main_ok
                    || !image_edge_preflight(request, image, linkage->instance, block_id,
                        plan->success_edge)
                    || !image_edge_preflight(request, image, linkage->instance, block_id,
                        plan->residual_edge)) return false;
                (void)pre_event; (void)supplemental; (void)value; (void)residual;
                break;
            }
            default: return false;
        }
    }
    return true;
}

static bool represented_fixed_products_needed(const SolWasmRepresentedBuildRequest *request) {
    const SolMirMaterialization *m = &request->program->conventions->concrete->materialization;
    for (size_t i = 0; i < m->instruction_count; ++i)
        if (m->instructions[i].kind == SOL_MIR_INST_CONSTRUCT
            || m->instructions[i].kind == SOL_MIR_INST_FUNCTION_VALUE) return true;
    for (size_t i = 0; i < m->block_count; ++i)
        if (m->blocks[i].terminator.kind == SOL_MIR_TERM_PROPAGATE) return true;
    return false;
}

static bool represented_function_table_needed(const SolWasmRepresentedBuildRequest *request) {
    const SolMirMaterialization *m = &request->program->conventions->concrete->materialization;
    for (size_t i = 0; i < m->instruction_count; ++i)
        if (m->instructions[i].kind == SOL_MIR_INST_FUNCTION_VALUE) return true;
    return false;
}

/* A Wasm funcref table is physically untyped in the MVP encoding, while every
 * call_indirect below carries one concrete signature.  C1 consequently admits
 * one, and only one, closed callback shape per private table. */
static bool represented_callback_table_signature_exact(const SolMirRuntimeConventions *conventions,
    const SolMirRuntimeSignature *left, const SolMirRuntimeSignature *right) {
    if (left == NULL || right == NULL || left->origin != SOL_MIR_RUNTIME_SIGNATURE_INTERNAL
        || right->origin != SOL_MIR_RUNTIME_SIGNATURE_INTERNAL || left->result != right->result
        || left->result_class != right->result_class || left->effects != right->effects
        || left->slots.count != right->slots.count
        || left->slots.offset > conventions->signature_slot_count
        || right->slots.offset > conventions->signature_slot_count
        || left->slots.count > conventions->signature_slot_count - left->slots.offset
        || right->slots.count > conventions->signature_slot_count - right->slots.offset)
        return false;
    for (size_t i = 0; i < left->slots.count; ++i) {
        const SolMirRuntimeSignatureSlot *a = &conventions->signature_slots[left->slots.offset + i];
        const SolMirRuntimeSignatureSlot *b = &conventions->signature_slots[right->slots.offset + i];
        if (a->role != SOL_MIR_RUNTIME_SLOT_PARAMETER || b->role != SOL_MIR_RUNTIME_SLOT_PARAMETER
            || a->formal != i || b->formal != i || a->recipe != b->recipe || a->access != b->access)
            return false;
    }
    return true;
}

static bool represented_function_table_preflight(const SolWasmRepresentedBuildRequest *request) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirLinkage *linkage = &concrete->linkage;
    const SolMirRuntimeConventions *conventions = request->program->conventions;
    const SolMirMaterialization *materialization = &concrete->materialization;
    const SolMirRuntimeSignature *canonical = NULL;
    SolMirRecipeId function_recipe = SOL_MIR_RECIPE_NONE;
    size_t function_values = 0;
    if (linkage->table_entry_count == 0 || linkage->table_entry_count == SIZE_MAX) return false;
    for (size_t i = 0; i < linkage->table_entry_count; ++i) {
        const SolMirLinkageTableEntry *entry = &linkage->table_entries[i];
        if (entry->target_kind != SOL_MIR_LINKAGE_TARGET_INTERNAL
            || entry->internal >= linkage->callable_count || entry->host != SOL_MIR_LINKAGE_NONE)
            return false;
        const SolMirRuntimeSignature *signature = signature_for(conventions, entry->internal);
        if (signature == NULL || signature->slots.offset > conventions->signature_slot_count
            || signature->slots.count > conventions->signature_slot_count - signature->slots.offset)
            return false;
        if (canonical == NULL) canonical = signature;
        else if (!represented_callback_table_signature_exact(conventions, canonical, signature)) return false;
    }
    for (size_t i = 0; i < materialization->instruction_count; ++i) {
        if (materialization->instructions[i].kind != SOL_MIR_INST_FUNCTION_VALUE) continue;
        const SolMirOperationCallablePlan *plan = NULL;
        if (!represented_function_value_route(request, i, &plan, NULL, NULL)
            || plan == NULL || (function_values != 0 && plan->function_recipe != function_recipe))
            return false;
        function_recipe = plan->function_recipe;
        ++function_values;
    }
    return canonical != NULL && function_values != 0;
}

static bool represented_sum_copy_needed(const SolWasmRepresentedBuildRequest *request,
    SolMirRecipeId recipe) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    if (!represented_sum_recipe_depth(concrete, recipe, 0)) return false;
    const SolMirMaterialization *m = &concrete->materialization;
    for (size_t i = 0; i < m->instruction_count; ++i) {
        const SolMirMaterializedInstruction *item = &m->instructions[i];
        if (item->kind == SOL_MIR_INST_LOAD_COPY && item->place < m->place_count
            && m->places[item->place].final_type < concrete->layout.type_count
            && represented_aggregate_reaches(concrete,
                concrete->layout.types[m->places[item->place].final_type].recipe, recipe, 0)) return true;
        const SolMirOperationPatternExtraction *extraction = item->kind == SOL_MIR_INST_PATTERN_VALUE
            ? represented_pattern_extraction_for_instruction(request, i) : NULL;
        if (extraction != NULL && represented_aggregate_reaches(concrete,
                extraction->result_recipe, recipe, 0)) return true;
    }
    return false;
}

static bool represented_sum_equal_needed(const SolWasmRepresentedBuildRequest *request,
    SolMirRecipeId recipe) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    if (!represented_sum_recipe_depth(concrete, recipe, 0)) return false;
    for (size_t i = 0; i < concrete->operations.arithmetic_count; ++i) {
        const SolMirOperationArithmeticPlan *plan = &concrete->operations.arithmetic[i];
        if ((plan->opcode == SOL_MIR_OPERATION_VALUE_EQ || plan->opcode == SOL_MIR_OPERATION_VALUE_NE)
            && represented_aggregate_reaches(concrete, plan->operand_recipe, recipe, 0)) return true;
    }
    return false;
}

/* A helper is emitted only for a recipe reached from an admitted product
 * LOAD_COPY/equality operation.  Its finite child closure is emitted too, in
 * recipe order, which keeps physical helper names and bytes deterministic. */
static bool represented_aggregate_reaches(const SolMirConcreteProgram *concrete,
    SolMirRecipeId root, SolMirRecipeId wanted, size_t depth) {
    const SolMirRepresentation *r = &concrete->representation;
    SolMirRecipeId physical;
    if (depth > r->recipe_count || !represented_backing_recipe(concrete, root, &physical)) return false;
    if (physical == wanted) return true;
    if (represented_scalar_product_recipe(concrete, physical)) {
        for (size_t i = 0; i < r->recipes[physical].fields.count; ++i)
            if (represented_aggregate_reaches(concrete,
                    r->fields[r->recipes[physical].fields.offset + i].type, wanted, depth + 1)) return true;
    } else if (represented_sum_recipe_depth(concrete, physical, 0)) {
        for (size_t i = 0; i < r->recipes[physical].variants.count; ++i) {
            const SolMirRecipeVariant *variant = &r->variants[r->recipes[physical].variants.offset + i];
            for (size_t f = 0; f < variant->fields.count; ++f)
                if (represented_aggregate_reaches(concrete, r->fields[variant->fields.offset + f].type,
                        wanted, depth + 1)) return true;
        }
    }
    return false;
}

static bool represented_product_copy_needed(const SolWasmRepresentedBuildRequest *request,
    SolMirRecipeId recipe) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    if (!represented_scalar_product_recipe(concrete, recipe)) return false;
    const SolMirMaterialization *m = &concrete->materialization;
    for (size_t i = 0; i < m->instruction_count; ++i) {
        const SolMirMaterializedInstruction *item = &m->instructions[i];
        if (item->kind == SOL_MIR_INST_LOAD_COPY && item->place < m->place_count) {
            const SolMirMaterializedPlace *place = &m->places[item->place];
            if (place->final_type < concrete->layout.type_count
                && represented_aggregate_reaches(concrete,
                    concrete->layout.types[place->final_type].recipe, recipe, 0)) return true;
        }
        const SolMirOperationPatternExtraction *extraction = item->kind == SOL_MIR_INST_PATTERN_VALUE
            ? represented_pattern_extraction_for_instruction(request, i) : NULL;
        if (extraction != NULL && represented_aggregate_reaches(concrete,
                extraction->result_recipe, recipe, 0)) return true;
    }
    return false;
}

static bool represented_product_equal_needed(const SolWasmRepresentedBuildRequest *request,
    SolMirRecipeId recipe) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    if (!represented_scalar_product_recipe(concrete, recipe)) return false;
    for (size_t i = 0; i < concrete->operations.arithmetic_count; ++i) {
        const SolMirOperationArithmeticPlan *plan = &concrete->operations.arithmetic[i];
        if ((plan->opcode == SOL_MIR_OPERATION_VALUE_EQ || plan->opcode == SOL_MIR_OPERATION_VALUE_NE)
            && represented_aggregate_reaches(concrete, plan->operand_recipe, recipe, 0)) return true;
    }
    return false;
}

static BinaryenExpressionRef represented_binary(const RepresentedFunction *function,
    size_t instruction, const SolMirOperationArithmeticPlan *plan) {
    size_t record = 0;
    if (!represented_failure_record_index(function->provenance,
            function->request->program->image_instructions[instruction].step_failure_site,
            &record)) return NULL;
    BinaryenExpressionRef left = get_value(function, plan->left);
    BinaryenExpressionRef right = plan->opcode == SOL_MIR_OPERATION_BOOL_NOT ? NULL
        : get_value(function, plan->right);
    if (left == NULL || (plan->opcode != SOL_MIR_OPERATION_BOOL_NOT && right == NULL)) return NULL;
    const SolMirConcreteProgram *concrete = function->request->program->conventions->concrete;
    SolMirRecipeId physical = SOL_MIR_RECIPE_NONE;
    bool has_physical = plan->operand_recipe < concrete->representation.recipe_count
        && represented_backing_recipe(concrete, plan->operand_recipe, &physical);
    bool text_equality = (plan->opcode == SOL_MIR_OPERATION_VALUE_EQ
            || plan->opcode == SOL_MIR_OPERATION_VALUE_NE) && has_physical
        && concrete->representation.recipes[physical].kind == SOL_MIR_RECIPE_TEXT;
    if (text_equality) {
        BinaryenExpressionRef arguments[] = {left, right, BinaryenConst(function->module,
            BinaryenLiteralInt64((int64_t)record))};
        BinaryenExpressionRef equal = BinaryenCall(function->module, P43_TEXT_EQUAL, arguments, 3,
            BinaryenTypeInt64());
        return plan->opcode == SOL_MIR_OPERATION_VALUE_EQ ? equal : i64_bool(function->module,
            BinaryenUnary(function->module, BinaryenEqZInt64(), equal));
    }
    if ((plan->opcode == SOL_MIR_OPERATION_VALUE_EQ || plan->opcode == SOL_MIR_OPERATION_VALUE_NE)
        && has_physical && represented_scalar_product_recipe(concrete, physical)) {
        char name[64];
        if (!represented_product_helper_name(name, "equal", physical)) return NULL;
        BinaryenExpressionRef arguments[] = {left, right, BinaryenConst(function->module,
            BinaryenLiteralInt64((int64_t)record))};
        BinaryenExpressionRef equal = BinaryenCall(function->module, name, arguments, 3,
            BinaryenTypeInt64());
        return plan->opcode == SOL_MIR_OPERATION_VALUE_EQ ? equal : i64_bool(function->module,
            BinaryenUnary(function->module, BinaryenEqZInt64(), equal));
    }
    if ((plan->opcode == SOL_MIR_OPERATION_VALUE_EQ || plan->opcode == SOL_MIR_OPERATION_VALUE_NE)
        && has_physical && represented_sum_recipe_depth(concrete, physical, 0)) {
        char name[64];
        if (!represented_sum_helper_name(name, "equal", physical)) return NULL;
        BinaryenExpressionRef arguments[] = {left, right, BinaryenConst(function->module,
            BinaryenLiteralInt64((int64_t)record))};
        BinaryenExpressionRef equal = BinaryenCall(function->module, name, arguments, 3,
            BinaryenTypeInt64());
        return plan->opcode == SOL_MIR_OPERATION_VALUE_EQ ? equal : i64_bool(function->module,
            BinaryenUnary(function->module, BinaryenEqZInt64(), equal));
    }
    switch (plan->opcode) {
        case SOL_MIR_OPERATION_BOOL_NOT:
            return i64_bool(function->module, BinaryenBinary(function->module,
                BinaryenEqInt64(), left, BinaryenConst(function->module, BinaryenLiteralInt64(0))));
        case SOL_MIR_OPERATION_I64_LT: return i64_bool(function->module,
            BinaryenBinary(function->module, BinaryenLtSInt64(), left, right));
        case SOL_MIR_OPERATION_I64_LE: return i64_bool(function->module,
            BinaryenBinary(function->module, BinaryenLeSInt64(), left, right));
        case SOL_MIR_OPERATION_I64_GT: return i64_bool(function->module,
            BinaryenBinary(function->module, BinaryenGtSInt64(), left, right));
        case SOL_MIR_OPERATION_I64_GE: return i64_bool(function->module,
            BinaryenBinary(function->module, BinaryenGeSInt64(), left, right));
        case SOL_MIR_OPERATION_BOOL_AND: return BinaryenBinary(function->module,
            BinaryenAndInt64(), left, right);
        case SOL_MIR_OPERATION_BOOL_OR: return BinaryenBinary(function->module,
            BinaryenOrInt64(), left, right);
        case SOL_MIR_OPERATION_VALUE_EQ: return i64_bool(function->module,
            BinaryenBinary(function->module, BinaryenEqInt64(), left, right));
        case SOL_MIR_OPERATION_VALUE_NE: return i64_bool(function->module,
            BinaryenBinary(function->module, BinaryenNeInt64(), left, right));
        default: return NULL;
    }
}

static BinaryenExpressionRef i64(const RepresentedFunction *function, int64_t value) {
    return BinaryenConst(function->module, BinaryenLiteralInt64(value));
}

static BinaryenExpressionRef wrap_i64(BinaryenModuleRef module, BinaryenExpressionRef value) {
    return BinaryenUnary(module, BinaryenWrapInt64(), value);
}

static BinaryenExpressionRef extend_u32(BinaryenModuleRef module, BinaryenExpressionRef value) {
    return BinaryenUnary(module, BinaryenExtendUInt32(), value);
}

/* An existing packet is authoritative: cleanup and failure predicates neither
 * consume steps nor replace its code/site. Unsigned comparison also admits the
 * full UINT64_MAX limit without overflowing the counter. */
static bool represented_step_function(BinaryenModuleRef module) {
    BinaryenType parameter = BinaryenTypeInt64();
    BinaryenExpressionRef failed[] = {
        BinaryenGlobalSet(module, P43_CODE, BinaryenConst(module,
            BinaryenLiteralInt32(SOL_MIR_RUNTIME_FAILURE_STEP_LIMIT))),
        BinaryenGlobalSet(module, P43_SITE, wrap_i64(module,
            BinaryenLocalGet(module, 0, BinaryenTypeInt64()))),
        BinaryenReturn(module, BinaryenConst(module, BinaryenLiteralInt32(0))),
    };
    BinaryenExpressionRef items[] = {
        BinaryenIf(module, BinaryenGlobalGet(module, P43_CODE, BinaryenTypeInt32()),
            BinaryenReturn(module, BinaryenConst(module, BinaryenLiteralInt32(1))), NULL),
        BinaryenIf(module, BinaryenBinary(module, BinaryenGeUInt64(),
            BinaryenGlobalGet(module, P44_STEPS, BinaryenTypeInt64()),
            BinaryenGlobalGet(module, P44_MAX_STEPS, BinaryenTypeInt64())),
            BinaryenBlock(module, NULL, failed, 3, BinaryenTypeNone()), NULL),
        BinaryenGlobalSet(module, P44_STEPS, BinaryenBinary(module, BinaryenAddInt64(),
            BinaryenGlobalGet(module, P44_STEPS, BinaryenTypeInt64()),
            BinaryenConst(module, BinaryenLiteralInt64(1)))),
        BinaryenConst(module, BinaryenLiteralInt32(1)),
    };
    return BinaryenAddFunction(module, P44_STEP, parameter, BinaryenTypeInt32(), NULL, 0,
        BinaryenBlock(module, NULL, items, 4, BinaryenTypeInt32())) != NULL;
}

static BinaryenExpressionRef represented_step_call(BinaryenModuleRef module,
    BinaryenExpressionRef record) {
    return BinaryenCall(module, P44_STEP, &record, 1, BinaryenTypeInt32());
}

static BinaryenExpressionRef represented_byte_step(BinaryenModuleRef module,
    BinaryenIndex record, int64_t failure_result) {
    return BinaryenIf(module, BinaryenUnary(module, BinaryenEqZInt32(),
        represented_step_call(module, BinaryenLocalGet(module, record, BinaryenTypeInt64()))),
        BinaryenReturn(module, BinaryenConst(module, BinaryenLiteralInt64(failure_result))), NULL);
}

/* A failed allocation is a normal, packet-carrying result, never a Wasm trap.
 * The site is supplied by the authenticated caller as its canonical one-based
 * supplemental provenance record. */
static BinaryenExpressionRef represented_text_copy_fail(BinaryenModuleRef module,
    SolMirRuntimeFailureCode code) {
    BinaryenExpressionRef items[] = {
        BinaryenGlobalSet(module, P43_CODE, BinaryenConst(module,
            BinaryenLiteralInt32((int32_t)code))),
        BinaryenGlobalSet(module, P43_SITE, wrap_i64(module,
            BinaryenLocalGet(module, 1, BinaryenTypeInt64()))),
        BinaryenReturn(module, BinaryenConst(module, BinaryenLiteralInt64(0))),
    };
    return BinaryenBlock(module, NULL, items, 3, BinaryenTypeNone());
}

/* P3.3's panic detail is an owned bounded byte packet, not a borrowed Text
 * handle.  The slot is reserved between active data and the heap, so capture
 * neither allocates nor changes the represented allocator's quotas. */
static bool represented_panic_capture_function(BinaryenModuleRef module) {
    BinaryenType parameters[] = {BinaryenTypeInt64(), BinaryenTypeInt64()};
    BinaryenType locals[] = {BinaryenTypeInt32(), BinaryenTypeInt32(), BinaryenTypeInt32(),
        BinaryenTypeInt32(), BinaryenTypeInt32(), BinaryenTypeInt32()};
    enum { SOURCE = 2, DATA, LENGTH, COUNT, CURSOR, MEMORY_BYTES };
    BinaryenExpressionRef items[15]; size_t item_count = 0;
    BinaryenExpressionRef base = BinaryenGlobalGet(module, P44_PANIC_DETAIL_OFFSET,
        BinaryenTypeInt32());
    items[item_count++] = BinaryenGlobalSet(module, P44_PANIC_DETAIL_LENGTH,
        BinaryenConst(module, BinaryenLiteralInt32(0)));
    items[item_count++] = BinaryenStore(module, 1, 0, 1, base,
        BinaryenConst(module, BinaryenLiteralInt32(0)), BinaryenTypeInt32(), P43_MEMORY);
    items[item_count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenGtUInt64(),
        BinaryenLocalGet(module, 0, BinaryenTypeInt64()), BinaryenConst(module,
            BinaryenLiteralInt64((int64_t)UINT32_MAX))), BinaryenReturn(module,
                BinaryenConst(module, BinaryenLiteralInt32(1))), NULL);
    items[item_count++] = BinaryenLocalSet(module, SOURCE, wrap_i64(module,
        BinaryenLocalGet(module, 0, BinaryenTypeInt64())));
    items[item_count++] = BinaryenLocalSet(module, MEMORY_BYTES, BinaryenBinary(module,
        BinaryenShlInt32(), BinaryenMemorySize(module, P43_MEMORY, false), BinaryenConst(module,
            BinaryenLiteralInt32(16))));
    items[item_count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenOrInt32(),
        BinaryenBinary(module, BinaryenLtUInt32(), BinaryenLocalGet(module, SOURCE,
            BinaryenTypeInt32()), BinaryenConst(module, BinaryenLiteralInt32(P43_STATIC_BASE))),
        BinaryenBinary(module, BinaryenGtUInt32(), BinaryenLocalGet(module, SOURCE,
            BinaryenTypeInt32()), BinaryenBinary(module, BinaryenSubInt32(), BinaryenLocalGet(module,
                MEMORY_BYTES, BinaryenTypeInt32()), BinaryenConst(module, BinaryenLiteralInt32(8))))),
        BinaryenReturn(module, BinaryenConst(module, BinaryenLiteralInt32(1))), NULL);
    items[item_count++] = BinaryenLocalSet(module, DATA, BinaryenLoad(module, 4, false, 0, 4,
        BinaryenTypeInt32(), BinaryenLocalGet(module, SOURCE, BinaryenTypeInt32()), P43_MEMORY));
    items[item_count++] = BinaryenLocalSet(module, LENGTH, BinaryenLoad(module, 4, false, 4, 4,
        BinaryenTypeInt32(), BinaryenLocalGet(module, SOURCE, BinaryenTypeInt32()), P43_MEMORY));
    items[item_count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenOrInt32(),
        BinaryenBinary(module, BinaryenAndInt32(), BinaryenBinary(module, BinaryenEqInt32(),
            BinaryenLocalGet(module, LENGTH, BinaryenTypeInt32()), BinaryenConst(module,
                BinaryenLiteralInt32(0))), BinaryenBinary(module, BinaryenNeInt32(),
            BinaryenLocalGet(module, DATA, BinaryenTypeInt32()), BinaryenConst(module,
                BinaryenLiteralInt32(0)))), BinaryenBinary(module, BinaryenAndInt32(),
            BinaryenBinary(module, BinaryenNeInt32(), BinaryenLocalGet(module, LENGTH,
                BinaryenTypeInt32()), BinaryenConst(module, BinaryenLiteralInt32(0))),
            BinaryenBinary(module, BinaryenOrInt32(), BinaryenBinary(module, BinaryenEqInt32(),
                BinaryenLocalGet(module, DATA, BinaryenTypeInt32()), BinaryenConst(module,
                    BinaryenLiteralInt32(0))), BinaryenBinary(module, BinaryenOrInt32(),
                BinaryenBinary(module, BinaryenGtUInt32(), BinaryenLocalGet(module, DATA,
                    BinaryenTypeInt32()), BinaryenLocalGet(module, MEMORY_BYTES,
                        BinaryenTypeInt32())),
                BinaryenBinary(module, BinaryenGtUInt32(), BinaryenLocalGet(module, LENGTH,
                    BinaryenTypeInt32()), BinaryenBinary(module, BinaryenSubInt32(),
                        BinaryenLocalGet(module, MEMORY_BYTES, BinaryenTypeInt32()),
                        BinaryenLocalGet(module, DATA, BinaryenTypeInt32()))))))),
        BinaryenReturn(module, BinaryenConst(module, BinaryenLiteralInt32(1))), NULL);
    items[item_count++] = BinaryenLocalSet(module, COUNT, BinaryenIf(module,
        BinaryenBinary(module, BinaryenGtUInt32(), BinaryenLocalGet(module, LENGTH,
            BinaryenTypeInt32()), BinaryenConst(module, BinaryenLiteralInt32(P44_PANIC_DETAIL_MAX))),
        BinaryenConst(module, BinaryenLiteralInt32(P44_PANIC_DETAIL_MAX)),
        BinaryenLocalGet(module, LENGTH, BinaryenTypeInt32())));
    BinaryenExpressionRef loop[] = {
        BinaryenIf(module, BinaryenBinary(module, BinaryenEqInt32(), BinaryenLocalGet(module, CURSOR,
            BinaryenTypeInt32()), BinaryenLocalGet(module, COUNT, BinaryenTypeInt32())),
            BinaryenBreak(module, "p44.panic.capture.done", NULL, NULL), NULL),
        BinaryenIf(module, BinaryenUnary(module, BinaryenEqZInt32(), represented_step_call(module,
            BinaryenLocalGet(module, 1, BinaryenTypeInt64()))), BinaryenReturn(module,
                BinaryenConst(module, BinaryenLiteralInt32(0))), NULL),
        BinaryenStore(module, 1, 0, 1, BinaryenBinary(module, BinaryenAddInt32(),
            BinaryenGlobalGet(module, P44_PANIC_DETAIL_OFFSET, BinaryenTypeInt32()),
            BinaryenLocalGet(module, CURSOR, BinaryenTypeInt32())), BinaryenLoad(module, 1, false, 0, 1,
            BinaryenTypeInt32(), BinaryenBinary(module, BinaryenAddInt32(), BinaryenLocalGet(module, DATA,
                BinaryenTypeInt32()), BinaryenLocalGet(module, CURSOR, BinaryenTypeInt32())), P43_MEMORY),
            BinaryenTypeInt32(), P43_MEMORY),
        BinaryenLocalSet(module, CURSOR, BinaryenBinary(module, BinaryenAddInt32(),
            BinaryenLocalGet(module, CURSOR, BinaryenTypeInt32()), BinaryenConst(module,
                BinaryenLiteralInt32(1)))),
        BinaryenBreak(module, "p44.panic.capture.loop", NULL, NULL),
    };
    BinaryenExpressionRef captured = BinaryenBlock(module, "p44.panic.capture.done",
        (BinaryenExpressionRef[]){BinaryenLocalSet(module, CURSOR, BinaryenConst(module,
            BinaryenLiteralInt32(0))), BinaryenLoop(module, "p44.panic.capture.loop",
            BinaryenBlock(module, NULL, loop, 5, BinaryenTypeNone()))}, 2, BinaryenTypeNone());
    items[item_count++] = captured;
    items[item_count++] = BinaryenStore(module, 1, 0, 1, BinaryenBinary(module, BinaryenAddInt32(),
        BinaryenGlobalGet(module, P44_PANIC_DETAIL_OFFSET, BinaryenTypeInt32()),
        BinaryenLocalGet(module, COUNT, BinaryenTypeInt32())), BinaryenConst(module,
            BinaryenLiteralInt32(0)), BinaryenTypeInt32(), P43_MEMORY);
    items[item_count++] = BinaryenGlobalSet(module, P44_PANIC_DETAIL_LENGTH,
        BinaryenLocalGet(module, COUNT, BinaryenTypeInt32()));
    items[item_count++] = BinaryenConst(module, BinaryenLiteralInt32(1));
    return BinaryenAddFunction(module, P44_PANIC_CAPTURE, BinaryenTypeCreate(parameters, 2),
        BinaryenTypeInt32(), locals, 6, BinaryenBlock(module, NULL, items,
            (BinaryenIndex)item_count, BinaryenTypeInt32())) != NULL;
}

/* Fixed P2 product allocation.  The helper validates quota before attempting
 * growth and publishes heap/counters only after growth succeeds, keeping the
 * one-object request transactional.  Slice A products are at most eight-byte
 * aligned, so the shared heap's eight-byte cursor is the exact P2 alignment. */
static bool represented_fixed_alloc_function(BinaryenModuleRef module) {
    BinaryenType parameters[] = {BinaryenTypeInt64(), BinaryenTypeInt64()};
    BinaryenType locals[] = {BinaryenTypeInt32(), BinaryenTypeInt32(), BinaryenTypeInt32(),
        BinaryenTypeInt32(), BinaryenTypeInt32(), BinaryenTypeInt64(), BinaryenTypeInt64()};
    enum { SIZE = 2, HEADER, END, ALIGNED, MEMORY_BYTES, NEXT_REQUESTS, NEXT_BYTES };
    BinaryenExpressionRef fail4 = represented_text_copy_fail(module,
        SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED);
    BinaryenExpressionRef fail5 = represented_text_copy_fail(module,
        SOL_MIR_RUNTIME_FAILURE_ALLOCATION_LIMIT);
    BinaryenExpressionRef items[18]; size_t count = 0;
    items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenOrInt32(),
        BinaryenBinary(module, BinaryenEqInt64(), BinaryenLocalGet(module, 0, BinaryenTypeInt64()),
            BinaryenConst(module, BinaryenLiteralInt64(0))), BinaryenBinary(module,
            BinaryenGtUInt64(), BinaryenLocalGet(module, 0, BinaryenTypeInt64()),
            BinaryenConst(module, BinaryenLiteralInt64((int64_t)UINT32_MAX)))), fail4, NULL);
    items[count++] = BinaryenLocalSet(module, SIZE, wrap_i64(module,
        BinaryenLocalGet(module, 0, BinaryenTypeInt64())));
    items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenOrInt32(),
        BinaryenBinary(module, BinaryenLtUInt64(), BinaryenGlobalGet(module, P43_MAX_REQUESTS,
            BinaryenTypeInt64()), BinaryenConst(module, BinaryenLiteralInt64(1))),
        BinaryenBinary(module, BinaryenGeUInt64(), BinaryenGlobalGet(module, P43_REQUESTS,
            BinaryenTypeInt64()), BinaryenGlobalGet(module, P43_MAX_REQUESTS, BinaryenTypeInt64()))),
        fail5, NULL);
    items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenOrInt32(),
        BinaryenBinary(module, BinaryenLtUInt64(), BinaryenGlobalGet(module, P43_MAX_BYTES,
            BinaryenTypeInt64()), extend_u32(module, BinaryenLocalGet(module, SIZE, BinaryenTypeInt32()))),
        BinaryenBinary(module, BinaryenGtUInt64(), BinaryenGlobalGet(module, P43_BYTES,
            BinaryenTypeInt64()), BinaryenBinary(module, BinaryenSubInt64(), BinaryenGlobalGet(module,
                P43_MAX_BYTES, BinaryenTypeInt64()), extend_u32(module, BinaryenLocalGet(module, SIZE,
                    BinaryenTypeInt32()))))), fail5, NULL);
    items[count++] = BinaryenLocalSet(module, NEXT_REQUESTS, BinaryenBinary(module,
        BinaryenAddInt64(), BinaryenGlobalGet(module, P43_REQUESTS, BinaryenTypeInt64()),
        BinaryenConst(module, BinaryenLiteralInt64(1))));
    items[count++] = BinaryenLocalSet(module, NEXT_BYTES, BinaryenBinary(module, BinaryenAddInt64(),
        BinaryenGlobalGet(module, P43_BYTES, BinaryenTypeInt64()), extend_u32(module,
            BinaryenLocalGet(module, SIZE, BinaryenTypeInt32()))));
    items[count++] = BinaryenLocalSet(module, HEADER, BinaryenGlobalGet(module, P43_HEAP,
        BinaryenTypeInt32()));
    items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenOrInt32(),
        BinaryenBinary(module, BinaryenNeInt32(), BinaryenBinary(module, BinaryenAndInt32(),
            BinaryenLocalGet(module, HEADER, BinaryenTypeInt32()), BinaryenConst(module,
                BinaryenLiteralInt32(7))), BinaryenConst(module, BinaryenLiteralInt32(0))),
        BinaryenBinary(module, BinaryenGtUInt32(), BinaryenLocalGet(module, HEADER,
            BinaryenTypeInt32()), BinaryenBinary(module, BinaryenSubInt32(), BinaryenConst(module,
                BinaryenLiteralInt32(-1)), BinaryenLocalGet(module, SIZE, BinaryenTypeInt32())))), fail4, NULL);
    items[count++] = BinaryenLocalSet(module, END, BinaryenBinary(module, BinaryenAddInt32(),
        BinaryenLocalGet(module, HEADER, BinaryenTypeInt32()), BinaryenLocalGet(module, SIZE,
            BinaryenTypeInt32())));
    items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenGtUInt32(),
        BinaryenLocalGet(module, END, BinaryenTypeInt32()), BinaryenConst(module,
            BinaryenLiteralInt32(-8))), fail4, NULL);
    items[count++] = BinaryenLocalSet(module, ALIGNED, BinaryenBinary(module, BinaryenAndInt32(),
        BinaryenBinary(module, BinaryenAddInt32(), BinaryenLocalGet(module, END, BinaryenTypeInt32()),
            BinaryenConst(module, BinaryenLiteralInt32(7))), BinaryenConst(module, BinaryenLiteralInt32(-8))));
    items[count++] = BinaryenLocalSet(module, MEMORY_BYTES, BinaryenBinary(module, BinaryenShlInt32(),
        BinaryenMemorySize(module, P43_MEMORY, false), BinaryenConst(module, BinaryenLiteralInt32(16))));
    /* A positive deficit rounded up to pages; this expression is evaluated only
     * in the guarded branch, so unsigned subtraction cannot wrap. */
    items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenGtUInt32(),
        BinaryenLocalGet(module, ALIGNED, BinaryenTypeInt32()), BinaryenLocalGet(module, MEMORY_BYTES,
            BinaryenTypeInt32())), BinaryenIf(module, BinaryenBinary(module, BinaryenEqInt32(),
            BinaryenMemoryGrow(module, BinaryenBinary(module, BinaryenShrUInt32(), BinaryenBinary(module,
                BinaryenAddInt32(), BinaryenBinary(module, BinaryenSubInt32(), BinaryenLocalGet(module,
                    ALIGNED, BinaryenTypeInt32()), BinaryenLocalGet(module, MEMORY_BYTES,
                    BinaryenTypeInt32())), BinaryenConst(module, BinaryenLiteralInt32(65535))),
                BinaryenConst(module, BinaryenLiteralInt32(16))), P43_MEMORY, false),
            BinaryenConst(module, BinaryenLiteralInt32(-1))), fail4, NULL), NULL);
    items[count++] = BinaryenGlobalSet(module, P43_HEAP, BinaryenLocalGet(module, ALIGNED,
        BinaryenTypeInt32()));
    items[count++] = BinaryenGlobalSet(module, P43_REQUESTS, BinaryenLocalGet(module, NEXT_REQUESTS,
        BinaryenTypeInt64()));
    items[count++] = BinaryenGlobalSet(module, P43_BYTES, BinaryenLocalGet(module, NEXT_BYTES,
        BinaryenTypeInt64()));
    items[count++] = extend_u32(module, BinaryenLocalGet(module, HEADER, BinaryenTypeInt32()));
    return BinaryenAddFunction(module, P43_FIXED_ALLOC, BinaryenTypeCreate(parameters, 2),
        BinaryenTypeInt64(), locals, 7, BinaryenBlock(module, NULL, items, (BinaryenIndex)count,
            BinaryenTypeInt64())) != NULL;
}

/* This allocator deliberately accepts both a static literal-pool header and a
 * previously published runtime Text header.  CONST_TEXT is its only static
 * consumer: static objects are sources, never values exposed to the program. */
static bool represented_text_copy_function(BinaryenModuleRef module) {
    BinaryenType parameters[] = {BinaryenTypeInt64(), BinaryenTypeInt64(), BinaryenTypeInt64()};
    /* source, data, length, header, payload, end, aligned, memory bytes,
     * grow pages, next requests, next bytes, and byte-copy cursor. */
    BinaryenType locals[] = {BinaryenTypeInt32(), BinaryenTypeInt32(), BinaryenTypeInt32(),
        BinaryenTypeInt32(), BinaryenTypeInt32(), BinaryenTypeInt32(), BinaryenTypeInt32(),
        BinaryenTypeInt32(), BinaryenTypeInt32(), BinaryenTypeInt64(), BinaryenTypeInt64(),
        BinaryenTypeInt32()};
    enum { SOURCE = 3, DATA, LENGTH, HEADER, PAYLOAD, END, ALIGNED, MEMORY_BYTES,
        GROW_PAGES, NEXT_REQUESTS, NEXT_BYTES, CURSOR };
    BinaryenExpressionRef items[32]; size_t count = 0;

    /* Validate both argument widths before narrowing and validate every source
     * range before its first load.  P43 memory has a finite Wasm32 maximum, so
     * MEMORY_BYTES is a safe unsigned i32 byte bound. */
    items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenOrInt32(),
        BinaryenBinary(module, BinaryenGtUInt64(), BinaryenLocalGet(module, 0, BinaryenTypeInt64()),
            BinaryenConst(module, BinaryenLiteralInt64((int64_t)UINT32_MAX))),
        BinaryenBinary(module, BinaryenOrInt32(), BinaryenBinary(module, BinaryenEqInt64(),
            BinaryenLocalGet(module, 1, BinaryenTypeInt64()), BinaryenConst(module,
                BinaryenLiteralInt64(0))), BinaryenBinary(module, BinaryenGtUInt64(),
            BinaryenLocalGet(module, 1, BinaryenTypeInt64()), BinaryenConst(module,
                BinaryenLiteralInt64((int64_t)UINT32_MAX))))),
        represented_text_copy_fail(module, SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED), NULL);
    items[count++] = BinaryenLocalSet(module, SOURCE, wrap_i64(module,
        BinaryenLocalGet(module, 0, BinaryenTypeInt64())));
    items[count++] = BinaryenLocalSet(module, MEMORY_BYTES, BinaryenBinary(module,
        BinaryenShlInt32(), BinaryenMemorySize(module, P43_MEMORY, false), BinaryenConst(module,
            BinaryenLiteralInt32(16))));
    items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenOrInt32(),
        BinaryenBinary(module, BinaryenLtUInt32(), BinaryenLocalGet(module, SOURCE,
            BinaryenTypeInt32()), BinaryenConst(module, BinaryenLiteralInt32(P43_STATIC_BASE))),
        BinaryenBinary(module, BinaryenOrInt32(), BinaryenBinary(module, BinaryenGeUInt32(),
            BinaryenLocalGet(module, SOURCE, BinaryenTypeInt32()), BinaryenGlobalGet(module,
                P43_HEAP, BinaryenTypeInt32())), BinaryenBinary(module, BinaryenOrInt32(),
                BinaryenBinary(module, BinaryenNeInt32(), BinaryenBinary(module, BinaryenAndInt32(),
                    BinaryenLocalGet(module, SOURCE, BinaryenTypeInt32()), BinaryenConst(module,
                        BinaryenLiteralInt32(7))), BinaryenConst(module, BinaryenLiteralInt32(0))),
                BinaryenBinary(module, BinaryenGtUInt32(), BinaryenLocalGet(module, SOURCE,
                    BinaryenTypeInt32()), BinaryenBinary(module, BinaryenSubInt32(),
                        BinaryenLocalGet(module, MEMORY_BYTES, BinaryenTypeInt32()), BinaryenConst(module,
                            BinaryenLiteralInt32(8))))))),
        represented_text_copy_fail(module, SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED), NULL);
    items[count++] = BinaryenLocalSet(module, DATA, BinaryenLoad(module, 4, false, 0, 4,
        BinaryenTypeInt32(), BinaryenLocalGet(module, SOURCE, BinaryenTypeInt32()), P43_MEMORY));
    items[count++] = BinaryenLocalSet(module, LENGTH, BinaryenLoad(module, 4, false, 4, 4,
        BinaryenTypeInt32(), BinaryenLocalGet(module, SOURCE, BinaryenTypeInt32()), P43_MEMORY));
    items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenOrInt32(),
        BinaryenBinary(module, BinaryenAndInt32(), BinaryenBinary(module, BinaryenEqInt32(),
            BinaryenLocalGet(module, LENGTH, BinaryenTypeInt32()), BinaryenConst(module,
                BinaryenLiteralInt32(0))), BinaryenBinary(module, BinaryenNeInt32(),
            BinaryenLocalGet(module, DATA, BinaryenTypeInt32()), BinaryenConst(module,
                BinaryenLiteralInt32(0)))), BinaryenBinary(module, BinaryenAndInt32(),
            BinaryenBinary(module, BinaryenNeInt32(), BinaryenLocalGet(module, LENGTH,
                BinaryenTypeInt32()), BinaryenConst(module, BinaryenLiteralInt32(0))),
            BinaryenBinary(module, BinaryenOrInt32(), BinaryenBinary(module, BinaryenEqInt32(),
                BinaryenLocalGet(module, DATA, BinaryenTypeInt32()), BinaryenConst(module,
                    BinaryenLiteralInt32(0))), BinaryenBinary(module, BinaryenOrInt32(),
                    BinaryenBinary(module, BinaryenGtUInt32(), BinaryenLocalGet(module, DATA,
                        BinaryenTypeInt32()), BinaryenLocalGet(module, MEMORY_BYTES,
                            BinaryenTypeInt32())), BinaryenBinary(module, BinaryenGtUInt32(),
                        BinaryenLocalGet(module, LENGTH, BinaryenTypeInt32()), BinaryenBinary(module,
                            BinaryenSubInt32(), BinaryenLocalGet(module, MEMORY_BYTES,
                                BinaryenTypeInt32()), BinaryenLocalGet(module, DATA,
                                    BinaryenTypeInt32()))))))),
        represented_text_copy_fail(module, SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED), NULL);

    /* Quotas use logical Text demand, not aligned heap consumption. */
    items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenOrInt32(),
        BinaryenBinary(module, BinaryenLtUInt64(), BinaryenGlobalGet(module, P43_MAX_REQUESTS,
            BinaryenTypeInt64()), BinaryenIf(module, BinaryenBinary(module, BinaryenEqInt32(),
                BinaryenLocalGet(module, LENGTH, BinaryenTypeInt32()), BinaryenConst(module,
                    BinaryenLiteralInt32(0))), BinaryenConst(module, BinaryenLiteralInt64(1)),
                BinaryenConst(module, BinaryenLiteralInt64(2)))), BinaryenBinary(module,
            BinaryenGtUInt64(), BinaryenGlobalGet(module, P43_REQUESTS, BinaryenTypeInt64()),
            BinaryenBinary(module, BinaryenSubInt64(), BinaryenGlobalGet(module, P43_MAX_REQUESTS,
                BinaryenTypeInt64()), BinaryenIf(module, BinaryenBinary(module, BinaryenEqInt32(),
                    BinaryenLocalGet(module, LENGTH, BinaryenTypeInt32()), BinaryenConst(module,
                        BinaryenLiteralInt32(0))), BinaryenConst(module, BinaryenLiteralInt64(1)),
                    BinaryenConst(module, BinaryenLiteralInt64(2)))))),
        represented_text_copy_fail(module, SOL_MIR_RUNTIME_FAILURE_ALLOCATION_LIMIT), NULL);
    items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenOrInt32(),
        BinaryenBinary(module, BinaryenLtUInt64(), BinaryenGlobalGet(module, P43_MAX_BYTES,
            BinaryenTypeInt64()), BinaryenBinary(module, BinaryenAddInt64(), BinaryenConst(module,
                BinaryenLiteralInt64(8)), extend_u32(module, BinaryenLocalGet(module, LENGTH,
                    BinaryenTypeInt32())))), BinaryenBinary(module, BinaryenGtUInt64(),
            BinaryenGlobalGet(module, P43_BYTES, BinaryenTypeInt64()), BinaryenBinary(module,
                BinaryenSubInt64(), BinaryenGlobalGet(module, P43_MAX_BYTES, BinaryenTypeInt64()),
                BinaryenBinary(module, BinaryenAddInt64(), BinaryenConst(module,
                    BinaryenLiteralInt64(8)), extend_u32(module, BinaryenLocalGet(module, LENGTH,
                        BinaryenTypeInt32())))))),
        represented_text_copy_fail(module, SOL_MIR_RUNTIME_FAILURE_ALLOCATION_LIMIT), NULL);
    items[count++] = BinaryenLocalSet(module, NEXT_REQUESTS, BinaryenBinary(module,
        BinaryenAddInt64(), BinaryenGlobalGet(module, P43_REQUESTS, BinaryenTypeInt64()),
        BinaryenIf(module, BinaryenBinary(module, BinaryenEqInt32(), BinaryenLocalGet(module, LENGTH,
            BinaryenTypeInt32()), BinaryenConst(module, BinaryenLiteralInt32(0))),
            BinaryenConst(module, BinaryenLiteralInt64(1)), BinaryenConst(module,
                BinaryenLiteralInt64(2)))));
    items[count++] = BinaryenLocalSet(module, NEXT_BYTES, BinaryenBinary(module, BinaryenAddInt64(),
        BinaryenGlobalGet(module, P43_BYTES, BinaryenTypeInt64()), BinaryenBinary(module,
            BinaryenAddInt64(), BinaryenConst(module, BinaryenLiteralInt64(8)), extend_u32(module,
                BinaryenLocalGet(module, LENGTH, BinaryenTypeInt32())))));

    items[count++] = BinaryenLocalSet(module, HEADER, BinaryenGlobalGet(module, P43_HEAP,
        BinaryenTypeInt32()));
    items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenGtUInt32(),
        BinaryenLocalGet(module, HEADER, BinaryenTypeInt32()), BinaryenConst(module,
            BinaryenLiteralInt32(-8))), represented_text_copy_fail(module,
                SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED), NULL);
    items[count++] = BinaryenLocalSet(module, END, BinaryenBinary(module, BinaryenAddInt32(),
        BinaryenLocalGet(module, HEADER, BinaryenTypeInt32()), BinaryenConst(module,
            BinaryenLiteralInt32(8))));
    items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenGtUInt32(),
        BinaryenLocalGet(module, END, BinaryenTypeInt32()), BinaryenBinary(module,
            BinaryenSubInt32(), BinaryenConst(module, BinaryenLiteralInt32(-1)),
            BinaryenLocalGet(module, LENGTH, BinaryenTypeInt32()))), represented_text_copy_fail(module,
                SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED), NULL);
    items[count++] = BinaryenLocalSet(module, END, BinaryenBinary(module, BinaryenAddInt32(),
        BinaryenLocalGet(module, END, BinaryenTypeInt32()), BinaryenLocalGet(module, LENGTH,
            BinaryenTypeInt32())));
    items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenGtUInt32(),
        BinaryenLocalGet(module, END, BinaryenTypeInt32()), BinaryenConst(module,
            BinaryenLiteralInt32(-8))), represented_text_copy_fail(module,
                SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED), NULL);
    items[count++] = BinaryenLocalSet(module, ALIGNED, BinaryenBinary(module, BinaryenAndInt32(),
        BinaryenBinary(module, BinaryenAddInt32(), BinaryenLocalGet(module, END,
            BinaryenTypeInt32()), BinaryenConst(module, BinaryenLiteralInt32(7))), BinaryenConst(module,
            BinaryenLiteralInt32(-8))));
    items[count++] = BinaryenLocalSet(module, GROW_PAGES, BinaryenBinary(module, BinaryenAddInt32(),
        BinaryenBinary(module, BinaryenShrUInt32(), BinaryenBinary(module, BinaryenSubInt32(),
            BinaryenLocalGet(module, ALIGNED, BinaryenTypeInt32()), BinaryenLocalGet(module,
                MEMORY_BYTES, BinaryenTypeInt32())), BinaryenConst(module, BinaryenLiteralInt32(16))),
        BinaryenBinary(module, BinaryenNeInt32(), BinaryenBinary(module, BinaryenAndInt32(),
            BinaryenBinary(module, BinaryenSubInt32(), BinaryenLocalGet(module, ALIGNED,
                BinaryenTypeInt32()), BinaryenLocalGet(module, MEMORY_BYTES, BinaryenTypeInt32())),
            BinaryenConst(module, BinaryenLiteralInt32(65535))), BinaryenConst(module,
                BinaryenLiteralInt32(0)))));
    items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenGtUInt32(),
        BinaryenLocalGet(module, ALIGNED, BinaryenTypeInt32()), BinaryenLocalGet(module,
            MEMORY_BYTES, BinaryenTypeInt32())), BinaryenIf(module, BinaryenBinary(module,
                BinaryenEqInt32(), BinaryenMemoryGrow(module, BinaryenLocalGet(module, GROW_PAGES,
                    BinaryenTypeInt32()), P43_MEMORY, false), BinaryenConst(module,
                    BinaryenLiteralInt32(-1))), represented_text_copy_fail(module,
                        SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED), NULL), NULL);

    items[count++] = BinaryenLocalSet(module, PAYLOAD, BinaryenIf(module, BinaryenBinary(module,
        BinaryenEqInt32(), BinaryenLocalGet(module, LENGTH, BinaryenTypeInt32()), BinaryenConst(module,
            BinaryenLiteralInt32(0))), BinaryenConst(module, BinaryenLiteralInt32(0)),
        BinaryenBinary(module, BinaryenAddInt32(),
                BinaryenLocalGet(module, HEADER, BinaryenTypeInt32()), BinaryenConst(module,
                    BinaryenLiteralInt32(8)))));
    items[count++] = BinaryenLocalSet(module, CURSOR, BinaryenConst(module, BinaryenLiteralInt32(0)));
    BinaryenExpressionRef copy_body[] = {
        BinaryenIf(module, BinaryenBinary(module, BinaryenEqInt32(), BinaryenLocalGet(module,
            CURSOR, BinaryenTypeInt32()), BinaryenLocalGet(module, LENGTH, BinaryenTypeInt32())),
            BinaryenBreak(module, "p43.copy.done", NULL, NULL), NULL),
        represented_byte_step(module, 2, P44_COPY_STEP_FAILURE),
        BinaryenStore(module, 1, 0, 1, BinaryenBinary(module, BinaryenAddInt32(),
            BinaryenLocalGet(module, PAYLOAD, BinaryenTypeInt32()), BinaryenLocalGet(module, CURSOR,
                BinaryenTypeInt32())), BinaryenLoad(module, 1, false, 0, 1, BinaryenTypeInt32(),
            BinaryenBinary(module, BinaryenAddInt32(), BinaryenLocalGet(module, DATA,
                BinaryenTypeInt32()), BinaryenLocalGet(module, CURSOR, BinaryenTypeInt32())), P43_MEMORY),
            BinaryenTypeInt32(), P43_MEMORY),
        BinaryenLocalSet(module, CURSOR, BinaryenBinary(module, BinaryenAddInt32(),
            BinaryenLocalGet(module, CURSOR, BinaryenTypeInt32()), BinaryenConst(module,
                BinaryenLiteralInt32(1)))),
        BinaryenBreak(module, "p43.copy.loop", NULL, NULL),
    };
    items[count++] = BinaryenBlock(module, "p43.copy.done", (BinaryenExpressionRef[]){
        BinaryenLoop(module, "p43.copy.loop", BinaryenBlock(module, NULL, copy_body, 5,
            BinaryenTypeNone()))}, 1, BinaryenTypeNone());
    items[count++] = BinaryenStore(module, 4, 0, 4, BinaryenLocalGet(module, HEADER,
        BinaryenTypeInt32()), BinaryenLocalGet(module, PAYLOAD, BinaryenTypeInt32()), BinaryenTypeInt32(),
        P43_MEMORY);
    items[count++] = BinaryenStore(module, 4, 4, 4, BinaryenLocalGet(module, HEADER,
        BinaryenTypeInt32()), BinaryenLocalGet(module, LENGTH, BinaryenTypeInt32()), BinaryenTypeInt32(),
        P43_MEMORY);
    items[count++] = BinaryenGlobalSet(module, P43_HEAP, BinaryenLocalGet(module, ALIGNED,
        BinaryenTypeInt32()));
    items[count++] = BinaryenGlobalSet(module, P43_REQUESTS, BinaryenLocalGet(module, NEXT_REQUESTS,
        BinaryenTypeInt64()));
    items[count++] = BinaryenGlobalSet(module, P43_BYTES, BinaryenLocalGet(module, NEXT_BYTES,
        BinaryenTypeInt64()));
    items[count++] = extend_u32(module, BinaryenLocalGet(module, HEADER, BinaryenTypeInt32()));
    return BinaryenAddFunction(module, P43_TEXT_COPY, BinaryenTypeCreate(parameters, 3),
        BinaryenTypeInt64(), locals, 12, BinaryenBlock(module, NULL, items,
            (BinaryenIndex)count, BinaryenTypeInt64())) != NULL;
}

static bool represented_text_equal_function(BinaryenModuleRef module) {
    BinaryenType parameters[] = {BinaryenTypeInt64(), BinaryenTypeInt64(), BinaryenTypeInt64()};
    BinaryenType locals[] = {BinaryenTypeInt32(), BinaryenTypeInt32(), BinaryenTypeInt32(),
        BinaryenTypeInt32(), BinaryenTypeInt32()};
    BinaryenExpressionRef left = wrap_i64(module, BinaryenLocalGet(module, 0, BinaryenTypeInt64()));
    BinaryenExpressionRef right = wrap_i64(module, BinaryenLocalGet(module, 1, BinaryenTypeInt64()));
    BinaryenExpressionRef items[10]; size_t count = 0;
    items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenEqInt64(),
        BinaryenLocalGet(module, 0, BinaryenTypeInt64()), BinaryenLocalGet(module, 1,
            BinaryenTypeInt64())), BinaryenReturn(module, BinaryenConst(module,
                BinaryenLiteralInt64(1))), NULL);
    items[count++] = BinaryenLocalSet(module, 3, BinaryenLoad(module, 4, false, 4, 4,
        BinaryenTypeInt32(), left, P43_MEMORY));
    items[count++] = BinaryenLocalSet(module, 4, BinaryenLoad(module, 4, false, 4, 4,
        BinaryenTypeInt32(), right, P43_MEMORY));
    items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenNeInt32(),
        BinaryenLocalGet(module, 3, BinaryenTypeInt32()), BinaryenLocalGet(module, 4,
            BinaryenTypeInt32())), BinaryenReturn(module, BinaryenConst(module,
                BinaryenLiteralInt64(0))), NULL);
    items[count++] = BinaryenLocalSet(module, 5, BinaryenLoad(module, 4, false, 0, 4,
        BinaryenTypeInt32(), wrap_i64(module, BinaryenLocalGet(module, 0, BinaryenTypeInt64())),
        P43_MEMORY));
    items[count++] = BinaryenLocalSet(module, 6, BinaryenLoad(module, 4, false, 0, 4,
        BinaryenTypeInt32(), wrap_i64(module, BinaryenLocalGet(module, 1, BinaryenTypeInt64())),
        P43_MEMORY));
    items[count++] = BinaryenLocalSet(module, 7, BinaryenConst(module, BinaryenLiteralInt32(0)));
    BinaryenExpressionRef loop_items[] = {
        BinaryenIf(module, BinaryenBinary(module, BinaryenEqInt32(), BinaryenLocalGet(module, 7,
            BinaryenTypeInt32()), BinaryenLocalGet(module, 3, BinaryenTypeInt32())),
            BinaryenBreak(module, "p43.equal.done", NULL, NULL), NULL),
        represented_byte_step(module, 2, 0),
        BinaryenIf(module, BinaryenBinary(module, BinaryenNeInt32(), BinaryenLoad(module, 1, false,
            0, 1, BinaryenTypeInt32(), BinaryenBinary(module, BinaryenAddInt32(),
                BinaryenLocalGet(module, 5, BinaryenTypeInt32()), BinaryenLocalGet(module, 7,
                    BinaryenTypeInt32())), P43_MEMORY), BinaryenLoad(module, 1, false, 0, 1,
                BinaryenTypeInt32(), BinaryenBinary(module, BinaryenAddInt32(), BinaryenLocalGet(module,
                    6, BinaryenTypeInt32()), BinaryenLocalGet(module, 7, BinaryenTypeInt32())),
                P43_MEMORY)), BinaryenReturn(module, BinaryenConst(module, BinaryenLiteralInt64(0))), NULL),
        BinaryenLocalSet(module, 7, BinaryenBinary(module, BinaryenAddInt32(), BinaryenLocalGet(module,
            7, BinaryenTypeInt32()), BinaryenConst(module, BinaryenLiteralInt32(1)))),
        BinaryenBreak(module, "p43.equal.loop", NULL, NULL),
    };
    items[count++] = BinaryenBlock(module, "p43.equal.done", (BinaryenExpressionRef[]){
        BinaryenLoop(module, "p43.equal.loop", BinaryenBlock(module, NULL, loop_items, 5,
            BinaryenTypeNone()))}, 1, BinaryenTypeNone());
    items[count++] = BinaryenReturn(module, BinaryenConst(module, BinaryenLiteralInt64(1)));
    return BinaryenAddFunction(module, P43_TEXT_EQUAL, BinaryenTypeCreate(parameters, 3),
        BinaryenTypeInt64(), locals, 5, BinaryenBlock(module, NULL, items,
            (BinaryenIndex)count, BinaryenTypeNone())) != NULL;
}

static bool represented_product_helper_name(char name[64], const char *kind,
    SolMirRecipeId recipe) {
    int written = snprintf(name, 64, "sol.p43.product.%s.%zu", kind, recipe);
    return written > 0 && (size_t)written < 64;
}

/* Every deep-copy helper receives the caller's already authenticated
 * supplemental provenance record.  It charges each fixed/Text allocation as
 * it happens, keeps its result private until all child copies succeed, and
 * deliberately leaves failed staging allocations unpublished. */
static bool represented_product_copy_function(BinaryenModuleRef module,
    const SolMirConcreteProgram *concrete, SolMirRecipeId recipe) {
    const SolMirRepresentation *r = &concrete->representation;
    const SolMirLayout *layout = &concrete->layout;
    char name[64];
    if (!represented_scalar_product_recipe(concrete, recipe)
        || !represented_product_helper_name(name, "copy", recipe)) return false;
    BinaryenType parameters[] = {BinaryenTypeInt64(), BinaryenTypeInt64(), BinaryenTypeInt64()};
    BinaryenType locals[] = {BinaryenTypeInt64(), BinaryenTypeInt64()};
    enum { HANDLE = 3, CHILD };
    BinaryenExpressionRef items[128]; size_t count = 0;
    BinaryenExpressionRef allocation_args[] = {BinaryenConst(module,
        BinaryenLiteralInt64((int64_t)layout->types[recipe].object_size)),
        BinaryenLocalGet(module, 1, BinaryenTypeInt64())};
    items[count++] = BinaryenLocalSet(module, HANDLE, BinaryenCall(module, P43_FIXED_ALLOC,
        allocation_args, 2, BinaryenTypeInt64()));
    items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenEqInt64(),
        BinaryenLocalGet(module, HANDLE, BinaryenTypeInt64()), BinaryenConst(module,
            BinaryenLiteralInt64(0))), BinaryenReturn(module, BinaryenConst(module,
                BinaryenLiteralInt64(0))), NULL);
    for (size_t i = 0; i < r->recipes[recipe].fields.count; ++i) {
        size_t field_id = r->recipes[recipe].fields.offset + i;
        SolMirRecipeId child_recipe = r->fields[field_id].type;
        const SolMirFieldLayout *field = field_id < layout->field_count ? &layout->fields[field_id] : NULL;
        if (field == NULL || field->owner_recipe != recipe) return false;
        if (field->size == 0) {
            if (!represented_zero_width_unit_field(concrete, child_recipe, field)) return false;
            continue;
        }
        if (!field->has_storage) return false;
        if (count + 4 > sizeof items / sizeof *items || (field->size != 1 && field->size != 4 && field->size != 8)
            || field->offset > UINT32_MAX) return false;
        BinaryenExpressionRef source = BinaryenLoad(module, (uint32_t)field->size, false,
            (uint32_t)field->offset, (uint32_t)field->alignment,
            field->size == 8 ? BinaryenTypeInt64() : BinaryenTypeInt32(),
            BinaryenUnary(module, BinaryenWrapInt64(), BinaryenLocalGet(module, 0,
                BinaryenTypeInt64())), P43_MEMORY);
        if (r->recipes[child_recipe].kind == SOL_MIR_RECIPE_TEXT) {
            BinaryenExpressionRef args[] = {field->size == 8 ? source
                : BinaryenUnary(module, BinaryenExtendUInt32(), source), BinaryenLocalGet(module, 1, BinaryenTypeInt64()),
                BinaryenLocalGet(module, 2, BinaryenTypeInt64())};
            items[count++] = BinaryenLocalSet(module, CHILD, BinaryenCall(module, P43_TEXT_COPY,
                args, 3, BinaryenTypeInt64()));
        } else if (represented_scalar_product_recipe(concrete, child_recipe)) {
            char child_name[64];
            if (!represented_product_helper_name(child_name, "copy", child_recipe)) return false;
            BinaryenExpressionRef args[] = {field->size == 8 ? source
                : BinaryenUnary(module, BinaryenExtendUInt32(), source), BinaryenLocalGet(module, 1, BinaryenTypeInt64()),
                BinaryenLocalGet(module, 2, BinaryenTypeInt64())};
            items[count++] = BinaryenLocalSet(module, CHILD, BinaryenCall(module, child_name,
                args, 3, BinaryenTypeInt64()));
        } else {
            items[count++] = BinaryenLocalSet(module, CHILD, field->size != 8
                ? BinaryenUnary(module, BinaryenExtendUInt32(), source) : source);
        }
        if (r->recipes[child_recipe].kind == SOL_MIR_RECIPE_TEXT
            || represented_scalar_product_recipe(concrete, child_recipe))
            items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenLeSInt64(),
                BinaryenLocalGet(module, CHILD, BinaryenTypeInt64()), BinaryenConst(module,
                    BinaryenLiteralInt64(0))), BinaryenReturn(module, BinaryenLocalGet(module,
                        CHILD, BinaryenTypeInt64())), NULL);
        BinaryenExpressionRef value = field->size != 8 ? BinaryenUnary(module, BinaryenWrapInt64(),
            BinaryenLocalGet(module, CHILD, BinaryenTypeInt64())) : BinaryenLocalGet(module, CHILD,
                BinaryenTypeInt64());
        items[count++] = BinaryenStore(module, (uint32_t)field->size, (uint32_t)field->offset,
            (uint32_t)field->alignment, BinaryenUnary(module, BinaryenWrapInt64(),
                BinaryenLocalGet(module, HANDLE, BinaryenTypeInt64())), value,
            field->size == 8 ? BinaryenTypeInt64() : BinaryenTypeInt32(), P43_MEMORY);
    }
    items[count++] = BinaryenReturn(module, BinaryenLocalGet(module, HANDLE, BinaryenTypeInt64()));
    return BinaryenAddFunction(module, name, BinaryenTypeCreate(parameters, 3), BinaryenTypeInt64(),
        locals, 2, BinaryenBlock(module, NULL, items, (BinaryenIndex)count, BinaryenTypeNone())) != NULL;
}

static bool represented_product_equal_function(BinaryenModuleRef module,
    const SolMirConcreteProgram *concrete, SolMirRecipeId recipe) {
    const SolMirRepresentation *r = &concrete->representation;
    const SolMirLayout *layout = &concrete->layout;
    char name[64];
    if (!represented_scalar_product_recipe(concrete, recipe)
        || !represented_product_helper_name(name, "equal", recipe)) return false;
    BinaryenType parameters[] = {BinaryenTypeInt64(), BinaryenTypeInt64(), BinaryenTypeInt64()};
    BinaryenType locals[] = {BinaryenTypeInt64()};
    enum { RESULT = 3 };
    BinaryenExpressionRef items[128]; size_t count = 0;
    items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenEqInt64(),
        BinaryenLocalGet(module, 0, BinaryenTypeInt64()), BinaryenLocalGet(module, 1,
            BinaryenTypeInt64())), BinaryenReturn(module, BinaryenConst(module,
                BinaryenLiteralInt64(1))), NULL);
    for (size_t i = 0; i < r->recipes[recipe].fields.count; ++i) {
        size_t field_id = r->recipes[recipe].fields.offset + i;
        SolMirRecipeId child_recipe = r->fields[field_id].type;
        const SolMirFieldLayout *field = field_id < layout->field_count ? &layout->fields[field_id] : NULL;
        if (field == NULL || field->owner_recipe != recipe) return false;
        if (field->size == 0) {
            if (!represented_zero_width_unit_field(concrete, child_recipe, field)) return false;
            continue;
        }
        if (!field->has_storage) return false;
        if (count + 3 > sizeof items / sizeof *items || (field->size != 1 && field->size != 4 && field->size != 8)
            || field->offset > UINT32_MAX) return false;
        BinaryenExpressionRef left = BinaryenLoad(module, (uint32_t)field->size, false,
            (uint32_t)field->offset, (uint32_t)field->alignment,
            field->size == 8 ? BinaryenTypeInt64() : BinaryenTypeInt32(), BinaryenUnary(module,
                BinaryenWrapInt64(), BinaryenLocalGet(module, 0, BinaryenTypeInt64())), P43_MEMORY);
        BinaryenExpressionRef right = BinaryenLoad(module, (uint32_t)field->size, false,
            (uint32_t)field->offset, (uint32_t)field->alignment,
            field->size == 8 ? BinaryenTypeInt64() : BinaryenTypeInt32(), BinaryenUnary(module,
                BinaryenWrapInt64(), BinaryenLocalGet(module, 1, BinaryenTypeInt64())), P43_MEMORY);
        if (r->recipes[child_recipe].kind == SOL_MIR_RECIPE_TEXT) {
            BinaryenExpressionRef args[] = {field->size == 8 ? left
                : BinaryenUnary(module, BinaryenExtendUInt32(), left), field->size == 8 ? right
                : BinaryenUnary(module, BinaryenExtendUInt32(), right),
                BinaryenLocalGet(module, 2, BinaryenTypeInt64())};
            items[count++] = BinaryenLocalSet(module, RESULT, BinaryenCall(module, P43_TEXT_EQUAL,
                args, 3, BinaryenTypeInt64()));
        } else if (represented_scalar_product_recipe(concrete, child_recipe)) {
            char child_name[64];
            if (!represented_product_helper_name(child_name, "equal", child_recipe)) return false;
            BinaryenExpressionRef args[] = {field->size == 8 ? left
                : BinaryenUnary(module, BinaryenExtendUInt32(), left), field->size == 8 ? right
                : BinaryenUnary(module, BinaryenExtendUInt32(), right),
                BinaryenLocalGet(module, 2, BinaryenTypeInt64())};
            items[count++] = BinaryenLocalSet(module, RESULT, BinaryenCall(module, child_name,
                args, 3, BinaryenTypeInt64()));
        } else items[count++] = BinaryenLocalSet(module, RESULT, BinaryenUnary(module,
            BinaryenExtendUInt32(), BinaryenBinary(module,
                field->size == 8 ? BinaryenEqInt64() : BinaryenEqInt32(), left, right)));
        items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenEqInt64(),
            BinaryenLocalGet(module, RESULT, BinaryenTypeInt64()), BinaryenConst(module,
                BinaryenLiteralInt64(0))), BinaryenReturn(module, BinaryenConst(module,
                    BinaryenLiteralInt64(0))), NULL);
    }
    items[count++] = BinaryenReturn(module, BinaryenConst(module, BinaryenLiteralInt64(1)));
    return BinaryenAddFunction(module, name, BinaryenTypeCreate(parameters, 3), BinaryenTypeInt64(),
        locals, 1, BinaryenBlock(module, NULL, items, (BinaryenIndex)count, BinaryenTypeNone())) != NULL;
}

static bool represented_sum_helper_name(char name[64], const char *kind,
    SolMirRecipeId recipe) {
    int written = snprintf(name, 64, "sol.p43.sum.%s.%zu", kind, recipe);
    return written > 0 && (size_t)written < 64;
}

static BinaryenExpressionRef represented_field_copy(BinaryenModuleRef module,
    const SolMirConcreteProgram *concrete, SolMirRecipeId child, const SolMirFieldLayout *field,
    BinaryenExpressionRef source, BinaryenExpressionRef site, BinaryenExpressionRef step_record) {
    SolMirRecipeId physical;
    if (!represented_backing_recipe(concrete, child, &physical)) return NULL;
    BinaryenExpressionRef loaded = BinaryenLoad(module, (uint32_t)field->size, false,
        (uint32_t)field->offset, (uint32_t)field->alignment, field->size == 8
            ? BinaryenTypeInt64() : BinaryenTypeInt32(), BinaryenUnary(module,
                BinaryenWrapInt64(), source), P43_MEMORY);
    BinaryenExpressionRef value = field->size == 8 ? loaded : extend_u32(module, loaded);
    if (represented_text_recipe(concrete, child)) {
        BinaryenExpressionRef arguments[] = {value, site, step_record};
        return BinaryenCall(module, P43_TEXT_COPY, arguments, 3, BinaryenTypeInt64());
    }
    char name[64];
    if (represented_product_recipe(concrete, child)) {
        if (!represented_product_helper_name(name, "copy", physical)) return NULL;
        BinaryenExpressionRef arguments[] = {value, site, step_record};
        return BinaryenCall(module, name, arguments, 3, BinaryenTypeInt64());
    }
    if (represented_sum_recipe(concrete, child)) {
        if (!represented_sum_helper_name(name, "copy", physical)) return NULL;
        BinaryenExpressionRef arguments[] = {value, site, step_record};
        return BinaryenCall(module, name, arguments, 3, BinaryenTypeInt64());
    }
    return value;
}

static bool represented_sum_copy_function(BinaryenModuleRef module,
    const SolMirConcreteProgram *concrete, SolMirRecipeId recipe) {
    const SolMirRepresentation *r = &concrete->representation;
    const SolMirLayout *layout = &concrete->layout;
    char name[64];
    if (!represented_sum_recipe_depth(concrete, recipe, 0)
        || !represented_sum_helper_name(name, "copy", recipe)) return false;
    BinaryenType parameters[] = {BinaryenTypeInt64(), BinaryenTypeInt64(), BinaryenTypeInt64()};
    BinaryenType locals[] = {BinaryenTypeInt64(), BinaryenTypeInt64(), BinaryenTypeInt32()};
    enum { HANDLE = 3, CHILD, TAG };
    BinaryenExpressionRef items[REPRESENTED_SUM_HELPER_ITEMS]; size_t count = 0;
    if (!represented_sum_helper_variant_count_supported(r->recipes[recipe].variants.count)) return false;
    BinaryenExpressionRef allocation_args[] = {i64(&(RepresentedFunction){.module = module},
        (int64_t)layout->types[recipe].object_size), BinaryenLocalGet(module, 1, BinaryenTypeInt64())};
    items[count++] = BinaryenLocalSet(module, HANDLE, BinaryenCall(module, P43_FIXED_ALLOC,
        allocation_args, 2, BinaryenTypeInt64()));
    items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenEqInt64(),
        BinaryenLocalGet(module, HANDLE, BinaryenTypeInt64()), BinaryenConst(module,
            BinaryenLiteralInt64(0))), BinaryenReturn(module, BinaryenConst(module,
                BinaryenLiteralInt64(0))), NULL);
    items[count++] = BinaryenLocalSet(module, TAG, BinaryenLoad(module, 4, false, 0, 4,
        BinaryenTypeInt32(), BinaryenUnary(module, BinaryenWrapInt64(), BinaryenLocalGet(module, 0,
            BinaryenTypeInt64())), P43_MEMORY));
    for (size_t i = 0; i < r->recipes[recipe].variants.count; ++i) {
        size_t variant_id = r->recipes[recipe].variants.offset + i;
        const SolMirRecipeVariant *variant = &r->variants[variant_id];
        RepresentedNodes branch = {0};
        bool ok = true;
        for (size_t f = 0; ok && f < variant->fields.count; ++f) {
            size_t field_id = variant->fields.offset + f;
            const SolMirFieldLayout *field = field_id < layout->field_count ? &layout->fields[field_id] : NULL;
            SolMirRecipeId child = field_id < r->field_count ? r->fields[field_id].type : SOL_MIR_RECIPE_NONE;
            if (field == NULL || field->owner_recipe != recipe || field->variant != variant_id) {
                ok = false; break;
            }
            if (field->size == 0) {
                if (!represented_zero_width_unit_field(concrete, child, field)) { ok = false; break; }
                continue;
            }
            if (!field->has_storage) { ok = false; break; }
            if ((field->size != 1 && field->size != 4 && field->size != 8)
                || field->offset > UINT32_MAX) { ok = false; break; }
            BinaryenExpressionRef copied = represented_field_copy(module, concrete, child, field,
                BinaryenLocalGet(module, 0, BinaryenTypeInt64()), BinaryenLocalGet(module, 1,
                    BinaryenTypeInt64()), BinaryenLocalGet(module, 2, BinaryenTypeInt64()));
            if (copied == NULL || !represented_nodes_push(&branch, BinaryenLocalSet(module, CHILD, copied))) {
                ok = false; break;
            }
            if (represented_indirect_recipe(concrete, child)
                && !represented_nodes_push(&branch, BinaryenIf(module, BinaryenBinary(module,
                    BinaryenLeSInt64(), BinaryenLocalGet(module, CHILD, BinaryenTypeInt64()),
                    BinaryenConst(module, BinaryenLiteralInt64(0))), BinaryenReturn(module,
                        BinaryenLocalGet(module, CHILD, BinaryenTypeInt64())), NULL))) { ok = false; break; }
            BinaryenExpressionRef value = field->size == 8 ? BinaryenLocalGet(module, CHILD,
                BinaryenTypeInt64()) : BinaryenUnary(module, BinaryenWrapInt64(), BinaryenLocalGet(module,
                    CHILD, BinaryenTypeInt64()));
            if (!represented_nodes_push(&branch, BinaryenStore(module, (uint32_t)field->size,
                    (uint32_t)field->offset, (uint32_t)field->alignment, BinaryenUnary(module,
                        BinaryenWrapInt64(), BinaryenLocalGet(module, HANDLE, BinaryenTypeInt64())), value,
                    field->size == 8 ? BinaryenTypeInt64() : BinaryenTypeInt32(), P43_MEMORY))) { ok = false; break; }
        }
        if (ok) ok = represented_nodes_push(&branch, BinaryenStore(module, 4, 0, 4,
            BinaryenUnary(module, BinaryenWrapInt64(), BinaryenLocalGet(module, HANDLE,
                BinaryenTypeInt64())), BinaryenLocalGet(module, TAG, BinaryenTypeInt32()),
            BinaryenTypeInt32(), P43_MEMORY)) && represented_nodes_push(&branch, BinaryenReturn(module,
                BinaryenLocalGet(module, HANDLE, BinaryenTypeInt64())));
        BinaryenExpressionRef body = ok ? BinaryenBlock(module, NULL, branch.items,
            (BinaryenIndex)branch.count, BinaryenTypeNone()) : NULL;
        deallocate(branch.items);
        if (!ok || count >= REPRESENTED_SUM_HELPER_ITEMS
                - REPRESENTED_SUM_HELPER_FINAL_ITEMS) return false;
        items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenEqInt32(),
            BinaryenLocalGet(module, TAG, BinaryenTypeInt32()), BinaryenConst(module,
                BinaryenLiteralInt32((int32_t)layout->variants[variant_id].tag))), body, NULL);
    }
    if (count >= REPRESENTED_SUM_HELPER_ITEMS) return false;
    items[count++] = represented_text_copy_fail(module, SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED);
    return BinaryenAddFunction(module, name, BinaryenTypeCreate(parameters, 3), BinaryenTypeInt64(),
        locals, 3, BinaryenBlock(module, NULL, items, (BinaryenIndex)count, BinaryenTypeNone())) != NULL;
}

static BinaryenExpressionRef represented_field_equal(BinaryenModuleRef module,
    const SolMirConcreteProgram *concrete, SolMirRecipeId child, const SolMirFieldLayout *field,
    BinaryenExpressionRef left_source, BinaryenExpressionRef right_source,
    BinaryenExpressionRef step_record) {
    SolMirRecipeId physical;
    if (!represented_backing_recipe(concrete, child, &physical)) return NULL;
    BinaryenExpressionRef left = BinaryenLoad(module, (uint32_t)field->size, false,
        (uint32_t)field->offset, (uint32_t)field->alignment, field->size == 8
            ? BinaryenTypeInt64() : BinaryenTypeInt32(), BinaryenUnary(module,
                BinaryenWrapInt64(), left_source), P43_MEMORY);
    BinaryenExpressionRef right = BinaryenLoad(module, (uint32_t)field->size, false,
        (uint32_t)field->offset, (uint32_t)field->alignment, field->size == 8
            ? BinaryenTypeInt64() : BinaryenTypeInt32(), BinaryenUnary(module,
                BinaryenWrapInt64(), right_source), P43_MEMORY);
    BinaryenExpressionRef a = field->size == 8 ? left : extend_u32(module, left);
    BinaryenExpressionRef b = field->size == 8 ? right : extend_u32(module, right);
    if (represented_text_recipe(concrete, child)) {
        BinaryenExpressionRef arguments[] = {a, b, step_record};
        return BinaryenCall(module, P43_TEXT_EQUAL, arguments, 3, BinaryenTypeInt64());
    }
    char name[64];
    if (represented_product_recipe(concrete, child)) {
        if (!represented_product_helper_name(name, "equal", physical)) return NULL;
        BinaryenExpressionRef arguments[] = {a, b, step_record};
        return BinaryenCall(module, name, arguments, 3, BinaryenTypeInt64());
    }
    if (represented_sum_recipe(concrete, child)) {
        if (!represented_sum_helper_name(name, "equal", physical)) return NULL;
        BinaryenExpressionRef arguments[] = {a, b, step_record};
        return BinaryenCall(module, name, arguments, 3, BinaryenTypeInt64());
    }
    return i64_bool(module, BinaryenBinary(module, field->size == 8 ? BinaryenEqInt64()
        : BinaryenEqInt32(), left, right));
}

static bool represented_sum_equal_function(BinaryenModuleRef module,
    const SolMirConcreteProgram *concrete, SolMirRecipeId recipe) {
    const SolMirRepresentation *r = &concrete->representation;
    const SolMirLayout *layout = &concrete->layout;
    char name[64];
    if (!represented_sum_recipe_depth(concrete, recipe, 0)
        || !represented_sum_helper_name(name, "equal", recipe)) return false;
    BinaryenType parameters[] = {BinaryenTypeInt64(), BinaryenTypeInt64(), BinaryenTypeInt64()};
    BinaryenType locals[] = {BinaryenTypeInt32(), BinaryenTypeInt64()};
    enum { TAG = 3, RESULT };
    BinaryenExpressionRef items[REPRESENTED_SUM_HELPER_ITEMS]; size_t count = 0;
    if (!represented_sum_helper_variant_count_supported(r->recipes[recipe].variants.count)) return false;
    items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenEqInt64(),
        BinaryenLocalGet(module, 0, BinaryenTypeInt64()), BinaryenLocalGet(module, 1,
            BinaryenTypeInt64())), BinaryenReturn(module, BinaryenConst(module,
                BinaryenLiteralInt64(1))), NULL);
    items[count++] = BinaryenLocalSet(module, TAG, BinaryenLoad(module, 4, false, 0, 4,
        BinaryenTypeInt32(), BinaryenUnary(module, BinaryenWrapInt64(), BinaryenLocalGet(module, 0,
            BinaryenTypeInt64())), P43_MEMORY));
    items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenNeInt32(),
        BinaryenLocalGet(module, TAG, BinaryenTypeInt32()), BinaryenLoad(module, 4, false, 0, 4,
            BinaryenTypeInt32(), BinaryenUnary(module, BinaryenWrapInt64(), BinaryenLocalGet(module, 1,
                BinaryenTypeInt64())), P43_MEMORY)), BinaryenReturn(module, BinaryenConst(module,
                    BinaryenLiteralInt64(0))), NULL);
    for (size_t i = 0; i < r->recipes[recipe].variants.count; ++i) {
        size_t variant_id = r->recipes[recipe].variants.offset + i;
        const SolMirRecipeVariant *variant = &r->variants[variant_id];
        RepresentedNodes branch = {0}; bool ok = true;
        for (size_t f = 0; ok && f < variant->fields.count; ++f) {
            size_t field_id = variant->fields.offset + f;
            const SolMirFieldLayout *field = field_id < layout->field_count ? &layout->fields[field_id] : NULL;
            SolMirRecipeId child = field_id < r->field_count ? r->fields[field_id].type : SOL_MIR_RECIPE_NONE;
            if (field == NULL || field->owner_recipe != recipe || field->variant != variant_id) {
                ok = false; break;
            }
            if (field->size == 0) {
                if (!represented_zero_width_unit_field(concrete, child, field)) { ok = false; break; }
                continue;
            }
            if (!field->has_storage) { ok = false; break; }
            if ((field->size != 1 && field->size != 4 && field->size != 8)
                || field->offset > UINT32_MAX) { ok = false; break; }
            BinaryenExpressionRef equal = represented_field_equal(module, concrete, child, field,
                BinaryenLocalGet(module, 0, BinaryenTypeInt64()), BinaryenLocalGet(module, 1,
                    BinaryenTypeInt64()), BinaryenLocalGet(module, 2, BinaryenTypeInt64()));
            if (equal == NULL || !represented_nodes_push(&branch, BinaryenLocalSet(module, RESULT,
                    equal)) || !represented_nodes_push(&branch, BinaryenIf(module, BinaryenBinary(module,
                    BinaryenEqInt64(), BinaryenLocalGet(module, RESULT, BinaryenTypeInt64()),
                    BinaryenConst(module, BinaryenLiteralInt64(0))), BinaryenReturn(module,
                        BinaryenConst(module, BinaryenLiteralInt64(0))), NULL))) { ok = false; break; }
        }
        if (ok) ok = represented_nodes_push(&branch, BinaryenReturn(module, BinaryenConst(module,
            BinaryenLiteralInt64(1))));
        BinaryenExpressionRef body = ok ? BinaryenBlock(module, NULL, branch.items,
            (BinaryenIndex)branch.count, BinaryenTypeNone()) : NULL;
        deallocate(branch.items);
        if (!ok || count >= REPRESENTED_SUM_HELPER_ITEMS
                - REPRESENTED_SUM_HELPER_FINAL_ITEMS) return false;
        items[count++] = BinaryenIf(module, BinaryenBinary(module, BinaryenEqInt32(),
            BinaryenLocalGet(module, TAG, BinaryenTypeInt32()), BinaryenConst(module,
                BinaryenLiteralInt32((int32_t)layout->variants[variant_id].tag))), body, NULL);
    }
    if (count >= REPRESENTED_SUM_HELPER_ITEMS) return false;
    items[count++] = BinaryenReturn(module, BinaryenConst(module, BinaryenLiteralInt64(0)));
    return BinaryenAddFunction(module, name, BinaryenTypeCreate(parameters, 3), BinaryenTypeInt64(),
        locals, 2, BinaryenBlock(module, NULL, items, (BinaryenIndex)count, BinaryenTypeNone())) != NULL;
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
/* This test-only probe is deliberately emitted while the authenticated module
 * still has its Binaryen names and memory identity.  It builds two different
 * physical objects, writes unequal bytes at the sum's P2 payload offset, and
 * invokes the ordinary generated equality helper.  Blank has no active field;
 * number does, so their expected results prove the inactive-payload boundary. */
static bool represented_inactive_payload_probe_functions(BinaryenModuleRef module,
    const SolMirConcreteProgram *concrete) {
    const SolMirRepresentation *r = &concrete->representation;
    const SolMirLayout *layout = &concrete->layout;
    SolMirRecipeId recipe = SOL_MIR_RECIPE_NONE;
    for (size_t i = 0; i < r->recipe_count; ++i) {
        if ((r->recipes[i].kind == SOL_MIR_RECIPE_ENUM || r->recipes[i].kind == SOL_MIR_RECIPE_OPTION
                || r->recipes[i].kind == SOL_MIR_RECIPE_RESULT) && r->recipes[i].variants.count == 3) {
            if (recipe != SOL_MIR_RECIPE_NONE) return false;
            recipe = i;
        }
    }
    if (recipe == SOL_MIR_RECIPE_NONE || recipe >= layout->type_count
        || layout->types[recipe].payload_offset > UINT32_MAX
        || r->recipes[recipe].variants.offset + 3 > r->variant_count) return false;
    char helper[64];
    if (!represented_sum_helper_name(helper, "equal", recipe)) return false;
    const uint32_t left = 64000, right = 64016;
    const int64_t left_poison = INT64_C(0x1122334455667788);
    const int64_t right_poison = INT64_C(0x0223344556677889);
    static const char *const names[] = {"sol.p43.test.inactive-payload", "sol.p43.test.active-payload"};
    const size_t variants[] = {0, 2};
    for (size_t probe = 0; probe < 2; ++probe) {
        const SolMirRecipeVariant *variant = &r->variants[r->recipes[recipe].variants.offset + variants[probe]];
        if (variant->semantic_tag > INT32_MAX) return false;
        BinaryenExpressionRef items[6];
        items[0] = BinaryenGlobalSet(module, P44_STEPS,
            BinaryenConst(module, BinaryenLiteralInt64(0)));
        items[1] = BinaryenStore(module, 4, 0, 4, BinaryenConst(module,
            BinaryenLiteralInt32((int32_t)left)), BinaryenConst(module,
            BinaryenLiteralInt32((int32_t)variant->semantic_tag)), BinaryenTypeInt32(), P43_MEMORY);
        items[2] = BinaryenStore(module, 4, 0, 4, BinaryenConst(module,
            BinaryenLiteralInt32((int32_t)right)), BinaryenConst(module,
            BinaryenLiteralInt32((int32_t)variant->semantic_tag)), BinaryenTypeInt32(), P43_MEMORY);
        items[3] = BinaryenStore(module, 8, (uint32_t)layout->types[recipe].payload_offset, 8,
            BinaryenConst(module, BinaryenLiteralInt32((int32_t)left)), BinaryenConst(module,
            BinaryenLiteralInt64(left_poison)), BinaryenTypeInt64(), P43_MEMORY);
        items[4] = BinaryenStore(module, 8, (uint32_t)layout->types[recipe].payload_offset, 8,
            BinaryenConst(module, BinaryenLiteralInt32((int32_t)right)), BinaryenConst(module,
            BinaryenLiteralInt64(right_poison)), BinaryenTypeInt64(), P43_MEMORY);
        BinaryenExpressionRef arguments[] = {BinaryenUnary(module, BinaryenExtendUInt32(),
            BinaryenConst(module, BinaryenLiteralInt32((int32_t)left))), BinaryenUnary(module,
            BinaryenExtendUInt32(), BinaryenConst(module, BinaryenLiteralInt32((int32_t)right))),
            BinaryenConst(module, BinaryenLiteralInt64(0))};
        items[5] = BinaryenCall(module, helper, arguments, 3, BinaryenTypeInt64());
        if (items[0] == NULL || items[1] == NULL || items[2] == NULL || items[3] == NULL
            || items[4] == NULL || items[5] == NULL || BinaryenAddFunction(module, names[probe], BinaryenTypeNone(),
                BinaryenTypeInt64(), NULL, 0, BinaryenBlock(module, NULL, items, 6,
                    BinaryenTypeInt64())) == NULL
            || BinaryenAddFunctionExport(module, names[probe], names[probe]) == NULL) return false;
    }
    return true;
}
#endif

static BinaryenExpressionRef temporary_get(const RepresentedFunction *function,
    SolMirMaterializedTemporaryId temporary) {
    size_t index = temporary_index(function, temporary);
    return index == SIZE_MAX ? NULL : BinaryenLocalGet(function->module,
        (BinaryenIndex)index, BinaryenTypeInt64());
}

static BinaryenExpressionRef checked_value(const RepresentedFunction *function,
    const SolMirOperationArithmeticPlan *plan) {
    BinaryenExpressionRef left = plan->compound ? temporary_get(function, plan->previous)
        : get_value(function, plan->left);
    BinaryenExpressionRef right = (plan->opcode == SOL_MIR_OPERATION_I64_NEG) ? NULL
        : get_value(function, plan->right);
    if (left == NULL || (right == NULL && plan->opcode != SOL_MIR_OPERATION_I64_NEG))
        return NULL;
    switch (plan->opcode) {
        case SOL_MIR_OPERATION_I64_NEG: return BinaryenBinary(function->module,
            BinaryenSubInt64(), i64(function, 0), left);
        case SOL_MIR_OPERATION_I64_ADD: return BinaryenBinary(function->module,
            BinaryenAddInt64(), left, right);
        case SOL_MIR_OPERATION_I64_SUB: return BinaryenBinary(function->module,
            BinaryenSubInt64(), left, right);
        case SOL_MIR_OPERATION_I64_MUL: return BinaryenBinary(function->module,
            BinaryenMulInt64(), left, right);
        case SOL_MIR_OPERATION_I64_DIV: return BinaryenBinary(function->module,
            BinaryenDivSInt64(), left, right);
        case SOL_MIR_OPERATION_I64_REM: return BinaryenBinary(function->module,
            BinaryenRemSInt64(), left, right);
        default: return NULL;
    }
}

static BinaryenExpressionRef mul_overflow(const RepresentedFunction *function,
    BinaryenExpressionRef left, BinaryenExpressionRef right) {
    BinaryenModuleRef module = function->module;
    BinaryenExpressionRef zero = i64(function, 0), minus_one = i64(function, -1);
    BinaryenExpressionRef minimum = i64(function, INT64_MIN);
    BinaryenExpressionRef maximum = i64(function, INT64_MAX);
    BinaryenExpressionRef positive_left = BinaryenBinary(module, BinaryenGtSInt64(), left, zero);
    BinaryenExpressionRef positive_right = BinaryenBinary(module, BinaryenGtSInt64(), right, zero);
    BinaryenExpressionRef positive = BinaryenIf(module, positive_right,
        BinaryenBinary(module, BinaryenGtSInt64(), left,
            BinaryenBinary(module, BinaryenDivSInt64(), maximum, right)),
        BinaryenBinary(module, BinaryenLtSInt64(), right,
            BinaryenBinary(module, BinaryenDivSInt64(), minimum, left)));
    BinaryenExpressionRef negative = BinaryenIf(module, positive_right,
        BinaryenBinary(module, BinaryenLtSInt64(), left,
            BinaryenBinary(module, BinaryenDivSInt64(), minimum, right)),
        BinaryenBinary(module, BinaryenLtSInt64(), left,
            BinaryenBinary(module, BinaryenDivSInt64(), maximum, right)));
    BinaryenExpressionRef ordinary = BinaryenIf(module, positive_left, positive, negative);
    return BinaryenIf(module, BinaryenBinary(module, BinaryenEqInt64(), left, zero),
        BinaryenConst(module, BinaryenLiteralInt32(0)), BinaryenIf(module,
            BinaryenBinary(module, BinaryenEqInt64(), right, zero),
            BinaryenConst(module, BinaryenLiteralInt32(0)), BinaryenIf(module,
                BinaryenBinary(module, BinaryenEqInt64(), left, minus_one),
                BinaryenBinary(module, BinaryenEqInt64(), right, minimum), BinaryenIf(module,
                    BinaryenBinary(module, BinaryenEqInt64(), right, minus_one),
                    BinaryenBinary(module, BinaryenEqInt64(), left, minimum), ordinary))));
}

static BinaryenExpressionRef checked_overflow(const RepresentedFunction *function,
    const SolMirOperationArithmeticPlan *plan) {
    BinaryenExpressionRef left = plan->compound ? temporary_get(function, plan->previous)
        : get_value(function, plan->left);
    BinaryenExpressionRef right = plan->opcode == SOL_MIR_OPERATION_I64_NEG ? NULL
        : get_value(function, plan->right);
    if (left == NULL || (right == NULL && plan->opcode != SOL_MIR_OPERATION_I64_NEG)) return NULL;
    switch (plan->opcode) {
        case SOL_MIR_OPERATION_I64_NEG:
            return BinaryenBinary(function->module, BinaryenEqInt64(), left,
                i64(function, INT64_MIN));
        case SOL_MIR_OPERATION_I64_ADD:
            return BinaryenBinary(function->module, BinaryenOrInt32(),
                BinaryenBinary(function->module, BinaryenAndInt32(),
                    BinaryenBinary(function->module, BinaryenGtSInt64(), right, i64(function, 0)),
                    BinaryenBinary(function->module, BinaryenGtSInt64(), left,
                        BinaryenBinary(function->module, BinaryenSubInt64(), i64(function, INT64_MAX), right))),
                BinaryenBinary(function->module, BinaryenAndInt32(),
                    BinaryenBinary(function->module, BinaryenLtSInt64(), right, i64(function, 0)),
                    BinaryenBinary(function->module, BinaryenLtSInt64(), left,
                        BinaryenBinary(function->module, BinaryenSubInt64(), i64(function, INT64_MIN), right))));
        case SOL_MIR_OPERATION_I64_SUB:
            return BinaryenBinary(function->module, BinaryenOrInt32(),
                BinaryenBinary(function->module, BinaryenAndInt32(),
                    BinaryenBinary(function->module, BinaryenLtSInt64(), right, i64(function, 0)),
                    BinaryenBinary(function->module, BinaryenGtSInt64(), left,
                        BinaryenBinary(function->module, BinaryenAddInt64(), i64(function, INT64_MAX), right))),
                BinaryenBinary(function->module, BinaryenAndInt32(),
                    BinaryenBinary(function->module, BinaryenGtSInt64(), right, i64(function, 0)),
                    BinaryenBinary(function->module, BinaryenLtSInt64(), left,
                        BinaryenBinary(function->module, BinaryenAddInt64(), i64(function, INT64_MIN), right))));
        case SOL_MIR_OPERATION_I64_MUL: return mul_overflow(function, left, right);
        case SOL_MIR_OPERATION_I64_DIV: case SOL_MIR_OPERATION_I64_REM:
            return BinaryenBinary(function->module, BinaryenAndInt32(),
                BinaryenBinary(function->module, BinaryenEqInt64(), left, i64(function, INT64_MIN)),
                BinaryenBinary(function->module, BinaryenEqInt64(), right, i64(function, -1)));
        default: return NULL;
    }
}

static bool represented_edge(const RepresentedFunction *function, size_t edge,
    RepresentedNodes *nodes) {
    const SolMirMaterialization *materialization = &function->request->program->conventions
        ->concrete->materialization;
    if (edge >= materialization->edge_count || function->request->program->image_edges[edge].image
        != function->image_id) return false;
    const SolMirMaterializedEdge *source = &materialization->edges[edge];
    if (source->block < function->image->blocks.offset
        || source->block - function->image->blocks.offset >= function->image->blocks.count)
        return false;
    const SolMirMaterializedBlock *target = &materialization->blocks[source->block];
    if (source->arguments.count != target->parameters.count
        || source->arguments.offset > materialization->edge_value_count
        || source->arguments.count > materialization->edge_value_count - source->arguments.offset)
        return false;
    BinaryenExpressionRef *arguments = source->arguments.count == 0 ? NULL
        : allocate(source->arguments.count, sizeof *arguments);
    BinaryenIndex *destinations = source->arguments.count == 0 ? NULL
        : allocate(source->arguments.count, sizeof *destinations);
    if (source->arguments.count != 0 && (arguments == NULL || destinations == NULL)) {
        deallocate(arguments); deallocate(destinations); return false;
    }
    for (size_t i = 0; i < target->parameters.count; ++i) {
        SolMirMaterializedValueId value = materialization->edge_values[source->arguments.offset + i];
        SolMirMaterializedValueId parameter = materialization->parameter_values[
            target->parameters.offset + i];
        size_t destination = value_index(function, parameter);
        arguments[i] = get_value(function, value);
        destinations[i] = (BinaryenIndex)destination;
        if (arguments[i] == NULL || destination == SIZE_MAX) {
            deallocate(arguments); deallocate(destinations); return false;
        }
    }
    bool assigned = represented_parallel_assign(function->module, nodes, arguments, destinations,
        source->arguments.count, function->scratch_base);
    deallocate(arguments); deallocate(destinations);
    if (!assigned) return false;
    if (!represented_nodes_push(nodes, BinaryenLocalSet(function->module, (BinaryenIndex)function->pc,
            BinaryenConst(function->module, BinaryenLiteralInt32((int32_t)(source->block
                - function->image->blocks.offset)))))
        || !represented_nodes_push(nodes, BinaryenBreak(function->module, "p43.dispatch", NULL, NULL)))
        return false;
    return true;
}

static BinaryenExpressionRef represented_predicate_get(const RepresentedFunction *function,
    size_t body_id, SolMirPredicateValueId value) {
    const SolMirOperations *o = &function->request->program->conventions->concrete->operations;
    if (body_id >= o->predicate_body_count || value >= o->predicate_value_count) return NULL;
    const SolMirPredicateValue *item = &o->predicate_values[value];
    if (item->kind != SOL_MIR_PREDICATE_VALUE_INPUT) return get_predicate_value(function, value);
    const SolMirPredicateBody *body = &o->predicate_bodies[body_id];
    if (item->definition < body->inputs.offset || item->definition - body->inputs.offset >= body->inputs.count)
        return NULL;
    const SolMirPredicateInput *input = &o->predicate_inputs[item->definition];
    if (input->kind == SOL_MIR_PREDICATE_INPUT_REFINEMENT_SELF) {
        const SolMirOperationPredicatePlan *plan = represented_refinement_plan_for_body(
            function->request, function->qualified, function->image_id, body_id, NULL);
        return plan == NULL || plan->input_recipe != input->recipe ? NULL
            : temporary_get(function, plan->representation);
    }
    if (input->kind == SOL_MIR_PREDICATE_INPUT_PARAMETER && input->ordinal < function->parameter_count)
        return BinaryenLocalGet(function->module, (BinaryenIndex)input->ordinal, BinaryenTypeInt64());
    if (input->kind == SOL_MIR_PREDICATE_INPUT_SNAPSHOT) {
        const SolMirOperationSnapshotPlan *snapshot = represented_snapshot_for_predicate_input(
            function->request, body_id, input, NULL);
        size_t index = snapshot == NULL ? SIZE_MAX
            : snapshot_value_index(function, snapshot->slot);
        return index == SIZE_MAX ? NULL : BinaryenLocalGet(function->module,
            (BinaryenIndex)index, BinaryenTypeInt64());
    }
    if (input->kind != SOL_MIR_PREDICATE_INPUT_COMPLETE_RESULT
        && input->kind != SOL_MIR_PREDICATE_INPUT_SUCCESS_RESULT) return NULL;
    const SolMirOperationPredicatePlan *plan = represented_plan_for_body(function->request,
        function->qualified, function->image_id, body_id);
    if (plan == NULL || plan->contract_phase != SOL_CONTRACT_ENSURES
        || plan->result == SOL_MIR_MATERIALIZED_NONE) return NULL;
    if (input->kind == SOL_MIR_PREDICATE_INPUT_COMPLETE_RESULT)
        return plan->contract_outcome == SOL_CONTRACT_OUTCOME_ALWAYS
            ? get_value(function, plan->result) : NULL;
    const RepresentedQualifiedImage *qualified = represented_qualified_image(function->qualified,
        function->image_id, plan->result_recipe);
    const SolMirConcreteProgram *concrete = function->request->program->conventions->concrete;
    if (plan->contract_outcome != SOL_CONTRACT_OUTCOME_SUCCESS
        || qualified == NULL || input->recipe != qualified->result.payload_recipe) return NULL;
    BinaryenExpressionRef object = get_value(function, plan->result);
    const SolMirFieldLayout *field = &concrete->layout.fields[qualified->result.success_field];
    if (object == NULL) return NULL;
    BinaryenExpressionRef loaded = BinaryenLoad(function->module, (uint32_t)field->size, false,
        (uint32_t)field->offset, (uint32_t)field->alignment,
        field->size == 8 ? BinaryenTypeInt64() : BinaryenTypeInt32(),
        wrap_i64(function->module, object), P43_MEMORY);
    return field->size == 8 ? loaded : extend_u32(function->module, loaded);
}

static BinaryenExpressionRef represented_predicate_binary(const RepresentedFunction *function,
    size_t body, const SolMirPredicateInstruction *item) {
    BinaryenExpressionRef left = represented_predicate_get(function, body, item->left);
    BinaryenExpressionRef right = (item->opcode == SOL_MIR_OPERATION_BOOL_NOT
        || item->opcode == SOL_MIR_OPERATION_I64_NEG) ? NULL
        : represented_predicate_get(function, body, item->right);
    if (left == NULL || (right == NULL && item->opcode != SOL_MIR_OPERATION_BOOL_NOT
            && item->opcode != SOL_MIR_OPERATION_I64_NEG)) return NULL;
    switch (item->opcode) {
        case SOL_MIR_OPERATION_BOOL_NOT: return i64_bool(function->module,
            BinaryenBinary(function->module, BinaryenEqInt64(), left, i64(function, 0)));
        case SOL_MIR_OPERATION_I64_LT: return i64_bool(function->module, BinaryenBinary(function->module, BinaryenLtSInt64(), left, right));
        case SOL_MIR_OPERATION_I64_LE: return i64_bool(function->module, BinaryenBinary(function->module, BinaryenLeSInt64(), left, right));
        case SOL_MIR_OPERATION_I64_GT: return i64_bool(function->module, BinaryenBinary(function->module, BinaryenGtSInt64(), left, right));
        case SOL_MIR_OPERATION_I64_GE: return i64_bool(function->module, BinaryenBinary(function->module, BinaryenGeSInt64(), left, right));
        case SOL_MIR_OPERATION_BOOL_AND: return BinaryenBinary(function->module, BinaryenAndInt64(), left, right);
        case SOL_MIR_OPERATION_BOOL_OR: return BinaryenBinary(function->module, BinaryenOrInt64(), left, right);
        case SOL_MIR_OPERATION_VALUE_EQ: return i64_bool(function->module, BinaryenBinary(function->module, BinaryenEqInt64(), left, right));
        case SOL_MIR_OPERATION_VALUE_NE: return i64_bool(function->module, BinaryenBinary(function->module, BinaryenNeInt64(), left, right));
        case SOL_MIR_OPERATION_I64_NEG: return BinaryenBinary(function->module, BinaryenSubInt64(), i64(function, 0), left);
        case SOL_MIR_OPERATION_I64_ADD: return BinaryenBinary(function->module, BinaryenAddInt64(), left, right);
        case SOL_MIR_OPERATION_I64_SUB: return BinaryenBinary(function->module, BinaryenSubInt64(), left, right);
        case SOL_MIR_OPERATION_I64_MUL: return BinaryenBinary(function->module, BinaryenMulInt64(), left, right);
        case SOL_MIR_OPERATION_I64_DIV: return BinaryenBinary(function->module, BinaryenDivSInt64(), left, right);
        case SOL_MIR_OPERATION_I64_REM: return BinaryenBinary(function->module, BinaryenRemSInt64(), left, right);
        default: return NULL;
    }
}

static BinaryenExpressionRef represented_predicate_overflow(const RepresentedFunction *function,
    size_t body, const SolMirPredicateInstruction *item) {
    BinaryenExpressionRef left = represented_predicate_get(function, body, item->left);
    BinaryenExpressionRef right = item->opcode == SOL_MIR_OPERATION_I64_NEG ? NULL
        : represented_predicate_get(function, body, item->right);
    if (left == NULL || (right == NULL && item->opcode != SOL_MIR_OPERATION_I64_NEG)) return NULL;
    switch (item->opcode) {
        case SOL_MIR_OPERATION_I64_NEG: return BinaryenBinary(function->module, BinaryenEqInt64(), left, i64(function, INT64_MIN));
        case SOL_MIR_OPERATION_I64_ADD: return BinaryenBinary(function->module, BinaryenOrInt32(),
            BinaryenBinary(function->module, BinaryenAndInt32(), BinaryenBinary(function->module, BinaryenGtSInt64(), right, i64(function, 0)), BinaryenBinary(function->module, BinaryenGtSInt64(), left, BinaryenBinary(function->module, BinaryenSubInt64(), i64(function, INT64_MAX), right))),
            BinaryenBinary(function->module, BinaryenAndInt32(), BinaryenBinary(function->module, BinaryenLtSInt64(), right, i64(function, 0)), BinaryenBinary(function->module, BinaryenLtSInt64(), left, BinaryenBinary(function->module, BinaryenSubInt64(), i64(function, INT64_MIN), right))));
        case SOL_MIR_OPERATION_I64_SUB: return BinaryenBinary(function->module, BinaryenOrInt32(),
            BinaryenBinary(function->module, BinaryenAndInt32(), BinaryenBinary(function->module, BinaryenLtSInt64(), right, i64(function, 0)), BinaryenBinary(function->module, BinaryenGtSInt64(), left, BinaryenBinary(function->module, BinaryenAddInt64(), i64(function, INT64_MAX), right))),
            BinaryenBinary(function->module, BinaryenAndInt32(), BinaryenBinary(function->module, BinaryenGtSInt64(), right, i64(function, 0)), BinaryenBinary(function->module, BinaryenLtSInt64(), left, BinaryenBinary(function->module, BinaryenAddInt64(), i64(function, INT64_MIN), right))));
        case SOL_MIR_OPERATION_I64_MUL: return mul_overflow(function, left, right);
        case SOL_MIR_OPERATION_I64_DIV: case SOL_MIR_OPERATION_I64_REM: return BinaryenBinary(function->module, BinaryenAndInt32(), BinaryenBinary(function->module, BinaryenEqInt64(), left, i64(function, INT64_MIN)), BinaryenBinary(function->module, BinaryenEqInt64(), right, i64(function, -1)));
        default: return NULL;
    }
}

static bool represented_predicate_edge(const RepresentedFunction *function, size_t edge,
    const char *dispatch_label, RepresentedNodes *nodes) {
    const SolMirOperations *o = &function->request->program->conventions->concrete->operations;
    if (edge >= o->predicate_edge_count) return false;
    const SolMirPredicateEdge *item = &o->predicate_edges[edge];
    if (item->target >= o->predicate_block_count || item->arguments.count != o->predicate_blocks[item->target].parameters.count
        || item->arguments.offset > o->predicate_edge_value_count || item->arguments.count > o->predicate_edge_value_count - item->arguments.offset) return false;
    if (item->source >= o->predicate_block_count || o->predicate_blocks[item->source].body
            >= o->predicate_body_count || o->predicate_blocks[item->target].body
            != o->predicate_blocks[item->source].body) return false;
    size_t body = o->predicate_blocks[item->source].body;
    BinaryenExpressionRef *sources = item->arguments.count ? allocate(item->arguments.count, sizeof *sources) : NULL;
    BinaryenIndex *destinations = item->arguments.count ? allocate(item->arguments.count, sizeof *destinations) : NULL;
    if (item->arguments.count != 0 && (sources == NULL || destinations == NULL)) { deallocate(sources); deallocate(destinations); return false; }
    for (size_t i = 0; i < item->arguments.count; ++i) {
        sources[i] = represented_predicate_get(function, body,
            o->predicate_edge_values[item->arguments.offset + i]);
        size_t index = represented_predicate_value_index(function, o->predicate_blocks[item->target].parameters.offset + i);
        destinations[i] = (BinaryenIndex)index;
        if (sources[i] == NULL || index == SIZE_MAX) { deallocate(sources); deallocate(destinations); return false; }
    }
    bool ok = represented_parallel_assign(function->module, nodes, sources, destinations,
        item->arguments.count, function->scratch_base);
    deallocate(sources); deallocate(destinations);
    return ok && represented_nodes_push(nodes, BinaryenLocalSet(function->module,
        (BinaryenIndex)function->predicate_pc, BinaryenConst(function->module,
            BinaryenLiteralInt32((int32_t)item->target)))) && represented_nodes_push(nodes,
                BinaryenBreak(function->module, dispatch_label, NULL, NULL));
}

static bool represented_predicate_failure(const RepresentedFunction *function, size_t instruction,
    SolMirRuntimeFailureCode code, const char *exit_label, RepresentedNodes *nodes) {
    const SolMirRuntimeLoweredProgram *owner = function->request->program;
    if (instruction >= owner->predicate_instruction_count) return false;
    size_t site = owner->predicate_instructions[instruction].failure_site, record = 0;
    return site < owner->conventions->failure_site_count
        && represented_failure_record_index(function->provenance, site, &record)
        && represented_nodes_push(nodes, BinaryenGlobalSet(function->module, P43_CODE,
            BinaryenConst(function->module, BinaryenLiteralInt32((int32_t)code))))
        && represented_nodes_push(nodes, BinaryenGlobalSet(function->module, P43_SITE,
            BinaryenConst(function->module, BinaryenLiteralInt32((int32_t)record))))
        && represented_nodes_push(nodes, BinaryenBreak(function->module, exit_label, NULL, NULL));
}

static bool represented_predicate_instruction_emit(const RepresentedFunction *function,
    size_t body, size_t id, const char *exit_label, RepresentedNodes *nodes) {
    const SolMirOperations *o = &function->request->program->conventions->concrete->operations;
    if (id >= o->predicate_instruction_count) return false;
    const SolMirPredicateInstruction *item = &o->predicate_instructions[id];
    const SolMirRuntimeLoweredPredicateInstruction *row =
        &function->request->program->predicate_instructions[id];
    size_t record = 0;
    if (row->state != SOL_MIR_RUNTIME_LOWERED_PRESENT
        || !represented_failure_record_index(function->provenance, row->step_failure_site, &record)
        || !represented_nodes_push(nodes, BinaryenIf(function->module,
            BinaryenUnary(function->module, BinaryenEqZInt32(), represented_step_call(function->module,
                i64(function, (int64_t)record))), BinaryenBreak(function->module, exit_label, NULL, NULL),
            NULL))) return false;
    size_t destination = represented_predicate_value_index(function, item->result);
    if (destination == SIZE_MAX) return false;
    if (item->kind == SOL_MIR_PREDICATE_INST_I64) return represented_nodes_push(nodes,
        BinaryenLocalSet(function->module, (BinaryenIndex)destination, i64(function, item->integer)));
    if (item->kind == SOL_MIR_PREDICATE_INST_BOOL) return represented_nodes_push(nodes,
        BinaryenLocalSet(function->module, (BinaryenIndex)destination, i64(function, item->boolean ? 1 : 0)));
    BinaryenExpressionRef value = represented_predicate_binary(function, body, item);
    if (value == NULL) return false;
    if (item->failures == SOL_MIR_OPERATION_FAILURE_NONE) return represented_nodes_push(nodes,
        BinaryenLocalSet(function->module, (BinaryenIndex)destination, value));
    BinaryenExpressionRef overflow = represented_predicate_overflow(function, body, item);
    RepresentedNodes overflow_nodes = {0}, zero_nodes = {0};
    bool ok = overflow != NULL && represented_predicate_failure(function, id,
        SOL_MIR_RUNTIME_FAILURE_INTEGER_OVERFLOW, exit_label, &overflow_nodes);
    BinaryenExpressionRef overflow_body = ok ? BinaryenBlock(function->module, NULL,
        overflow_nodes.items, (BinaryenIndex)overflow_nodes.count, BinaryenTypeNone()) : NULL;
    deallocate(overflow_nodes.items);
    BinaryenExpressionRef success = ok ? BinaryenLocalSet(function->module,
        (BinaryenIndex)destination, value) : NULL;
    if (item->opcode == SOL_MIR_OPERATION_I64_DIV || item->opcode == SOL_MIR_OPERATION_I64_REM) {
        BinaryenExpressionRef divisor = represented_predicate_get(function, body, item->right);
        ok = ok && divisor != NULL && represented_predicate_failure(function, id,
            SOL_MIR_RUNTIME_FAILURE_DIVISION_BY_ZERO, exit_label, &zero_nodes);
        BinaryenExpressionRef zero_body = ok ? BinaryenBlock(function->module, NULL,
            zero_nodes.items, (BinaryenIndex)zero_nodes.count, BinaryenTypeNone()) : NULL;
        deallocate(zero_nodes.items);
        return ok && represented_nodes_push(nodes, BinaryenIf(function->module,
            BinaryenBinary(function->module, BinaryenEqInt64(), divisor, i64(function, 0)), zero_body,
            BinaryenIf(function->module, overflow, overflow_body, success)));
    }
    return ok && represented_nodes_push(nodes, BinaryenIf(function->module, overflow, overflow_body, success));
}

static bool represented_predicate_emit(const RepresentedFunction *function, size_t body_id,
    RepresentedNodes *nodes) {
    const SolMirOperations *o = &function->request->program->conventions->concrete->operations;
    if (body_id >= o->predicate_body_count || function->predicate_value_count == 0) return false;
    const SolMirPredicateBody *body = &o->predicate_bodies[body_id];
    size_t image_bodies = 0; bool has_ensures = false;
    for (size_t i = 0; i < o->predicate_body_count; ++i)
        if (o->predicate_bodies[i].owner_kind == SOL_MIR_PREDICATE_OWNER_INSTANCE
            && o->predicate_bodies[i].instance == function->image_id) {
            ++image_bodies;
            has_ensures = has_ensures || o->predicate_bodies[i].phase == SOL_CONTRACT_ENSURES;
        }
    char exit_storage[64], dispatch_storage[64];
    const char *exit_label = "p43.predicate", *dispatch_label = "p43.predicate.dispatch";
    if (has_ensures && image_bodies > 1) {
        if (snprintf(exit_storage, sizeof exit_storage, "p43.predicate.%zu", body_id) < 0
            || snprintf(dispatch_storage, sizeof dispatch_storage, "p43.predicate.dispatch.%zu",
                body_id) < 0) return false;
        exit_label = exit_storage; dispatch_label = dispatch_storage;
    }
    if (!represented_nodes_push(nodes, BinaryenLocalSet(function->module, (BinaryenIndex)function->predicate_pc,
            BinaryenConst(function->module, BinaryenLiteralInt32((int32_t)body->entry))))) return false;
    RepresentedNodes dispatch = {0}; bool ok = true;
    for (size_t b = body->blocks.offset; ok && b < body->blocks.offset + body->blocks.count; ++b) {
        const SolMirPredicateBlock *block = &o->predicate_blocks[b];
        RepresentedNodes part = {0};
        for (size_t i = 0; ok && i < block->instructions.count; ++i)
            ok = represented_predicate_instruction_emit(function, body_id,
                block->instructions.offset + i, exit_label, &part);
        const SolMirRuntimeLoweredPredicateTerminator *row =
            &function->request->program->predicate_terminators[b];
        size_t record = 0;
        if (ok) ok = row->state == SOL_MIR_RUNTIME_LOWERED_PRESENT
            && represented_failure_record_index(function->provenance, row->step_failure_site, &record)
            && represented_nodes_push(&part, BinaryenIf(function->module,
                BinaryenUnary(function->module, BinaryenEqZInt32(), represented_step_call(function->module,
                    i64(function, (int64_t)record))), BinaryenBreak(function->module, exit_label, NULL, NULL),
                NULL));
        if (ok && block->terminator.kind == SOL_MIR_PREDICATE_TERM_RETURN) {
            BinaryenExpressionRef value = represented_predicate_get(function, body_id, block->terminator.value);
            ok = value != NULL && represented_nodes_push(&part, BinaryenLocalSet(function->module,
                (BinaryenIndex)function->predicate_result, value)) && represented_nodes_push(&part,
                    BinaryenBreak(function->module, exit_label, NULL, NULL));
        } else if (ok && block->terminator.kind == SOL_MIR_PREDICATE_TERM_JUMP) {
            ok = represented_predicate_edge(function, block->terminator.edge, dispatch_label, &part);
        } else if (ok && block->terminator.kind == SOL_MIR_PREDICATE_TERM_BRANCH) {
            RepresentedNodes yes = {0}, no = {0};
            ok = represented_predicate_edge(function, block->terminator.true_edge, dispatch_label, &yes)
                && represented_predicate_edge(function, block->terminator.false_edge, dispatch_label, &no);
            BinaryenExpressionRef condition = represented_predicate_get(function, body_id, block->terminator.condition);
            BinaryenExpressionRef y = ok ? BinaryenBlock(function->module, NULL, yes.items,
                (BinaryenIndex)yes.count, BinaryenTypeNone()) : NULL;
            BinaryenExpressionRef n = ok ? BinaryenBlock(function->module, NULL, no.items,
                (BinaryenIndex)no.count, BinaryenTypeNone()) : NULL;
            deallocate(yes.items); deallocate(no.items);
            ok = ok && condition != NULL && represented_nodes_push(&part, BinaryenIf(function->module,
                BinaryenBinary(function->module, BinaryenNeInt64(), condition, i64(function, 0)), y, n));
        } else ok = false;
        BinaryenExpressionRef contents = ok ? BinaryenBlock(function->module, NULL, part.items,
            (BinaryenIndex)part.count, BinaryenTypeNone()) : NULL;
        deallocate(part.items);
        if (ok) ok = represented_nodes_push(&dispatch, BinaryenIf(function->module,
            BinaryenBinary(function->module, BinaryenEqInt32(), BinaryenLocalGet(function->module,
                (BinaryenIndex)function->predicate_pc, BinaryenTypeInt32()), BinaryenConst(function->module,
                    BinaryenLiteralInt32((int32_t)b))), contents, NULL));
    }
    if (ok) ok = represented_nodes_push(&dispatch, BinaryenBreak(function->module, exit_label, NULL, NULL));
    BinaryenExpressionRef loop = ok ? BinaryenLoop(function->module, dispatch_label,
        BinaryenBlock(function->module, NULL, dispatch.items, (BinaryenIndex)dispatch.count, BinaryenTypeNone())) : NULL;
    BinaryenExpressionRef exit = ok ? BinaryenBlock(function->module, exit_label, &loop, 1,
        BinaryenTypeNone()) : NULL;
    deallocate(dispatch.items);
    return ok && represented_nodes_push(nodes, exit);
}

/* The represented heap is bump-only, but ownership is still consumed in the
 * emitted order.  Keep the no-op leaves as explicit Wasm nodes: the hook-only
 * counters below instrument these exact branches rather than a test surrogate. */
static BinaryenExpressionRef represented_cleanup_probe_increment(BinaryenModuleRef module,
    const char *global) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (represented_test_callable_hole_cleanup_probe)
        return BinaryenGlobalSet(module, global, BinaryenBinary(module, BinaryenAddInt32(),
            BinaryenGlobalGet(module, global, BinaryenTypeInt32()), BinaryenConst(module,
                BinaryenLiteralInt32(1))));
#else
    (void)global;
#endif
    return BinaryenNop(module);
}

typedef struct {
    const SolMirRuntimeCleanupEvent *event;
    const SolMirRuntimeCleanupTransition *transition;
    /* Explicit runtime disposition for the shared LOCAL_OR_PENDING slice. */
    bool pending_from_call;
} RepresentedCleanupTraceContext;

static bool represented_cleanup_trace_enabled(void) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    return represented_test_p44_cleanup_trace_probe;
#else
    return false;
#endif
}

static bool represented_refinement_trace_enabled(void) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    return represented_test_p44_refinement_trace_probe;
#else
    return false;
#endif
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
static bool represented_trace_ledger_enabled(void) {
    return represented_test_p44_cleanup_trace_probe
        || represented_test_p44_refinement_trace_probe;
}
#endif

#ifdef SOL_MIR_PLAN_TEST_HOOKS
static unsigned represented_cleanup_trace_disposition(const SolMirRuntimeCleanupAction *action,
    const RepresentedCleanupTraceContext *context, bool executed) {
    unsigned disposition = executed ? SOL_WASM_REPRESENTED_TEST_P44_TRACE_EXECUTED
        : SOL_WASM_REPRESENTED_TEST_P44_TRACE_SKIPPED;
    if (context->transition->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE)
        disposition |= SOL_WASM_REPRESENTED_TEST_P44_TRACE_FAILURE;
    if (context->event->origin == SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT)
        disposition |= SOL_WASM_REPRESENTED_TEST_P44_TRACE_IMPLICIT;
    if (context->transition->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING)
        disposition |= SOL_WASM_REPRESENTED_TEST_P44_TRACE_PENDING;
    if ((action->flags & SOL_MIR_RUNTIME_CLEANUP_ACTION_NORMAL_ONLY) != 0)
        disposition |= SOL_WASM_REPRESENTED_TEST_P44_TRACE_NORMAL;
    if ((action->flags & SOL_MIR_RUNTIME_CLEANUP_ACTION_FAILURE_ONLY) != 0)
        disposition |= SOL_WASM_REPRESENTED_TEST_P44_TRACE_ACTION_FAILURE;
    if ((action->flags & SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED) != 0)
        disposition |= SOL_WASM_REPRESENTED_TEST_P44_TRACE_GUARDED;
    return disposition;
}
#endif

/* A direct bounded record in D+192..D+960 of P4.3's existing fixed scratch. */
static BinaryenExpressionRef represented_cleanup_trace_emit(const RepresentedFunction *function,
    const SolMirRuntimeCleanupAction *action, const RepresentedCleanupTraceContext *context,
    bool executed) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    const SolMirRuntimeCleanup *cleanup = function->request->program->cleanup;
    if (!represented_test_p44_cleanup_trace_probe || context == NULL) return BinaryenNop(function->module);
    if (action < cleanup->actions || action >= cleanup->actions + cleanup->action_count
        || context->event == NULL || context->transition == NULL) return NULL;
    size_t action_id = (size_t)(action - cleanup->actions);
    if (action_id > UINT32_MAX) return NULL;
    unsigned disposition = represented_cleanup_trace_disposition(action, context, executed);
    BinaryenExpressionRef base = BinaryenBinary(function->module, BinaryenAddInt32(),
        BinaryenGlobalGet(function->module, P44_TRACE_OFFSET, BinaryenTypeInt32()),
        BinaryenBinary(function->module, BinaryenMulInt32(), BinaryenGlobalGet(function->module,
            P44_TRACE_COUNT, BinaryenTypeInt32()), BinaryenConst(function->module,
                BinaryenLiteralInt32(P44_TRACE_SLOT_BYTES))));
    BinaryenExpressionRef disposition_value = BinaryenConst(function->module,
        BinaryenLiteralInt32((int32_t)disposition));
    if (context->pending_from_call) {
        disposition_value = BinaryenIf(function->module, BinaryenBinary(function->module,
            BinaryenNeInt64(), BinaryenLocalGet(function->module,
                (BinaryenIndex)function->scratch_base, BinaryenTypeInt64()), i64(function, 0)),
            BinaryenConst(function->module, BinaryenLiteralInt32((int32_t)(disposition
                | SOL_WASM_REPRESENTED_TEST_P44_TRACE_PENDING))), disposition_value);
    }
    BinaryenExpressionRef write[] = {
        BinaryenStore(function->module, 4, 0, 4, base, BinaryenConst(function->module,
            BinaryenLiteralInt32((int32_t)action_id)), BinaryenTypeInt32(), P43_MEMORY),
        BinaryenStore(function->module, 4, 4, 4, base, disposition_value, BinaryenTypeInt32(), P43_MEMORY),
        BinaryenStore(function->module, 4, 8, 4, base, (disposition
                    & SOL_WASM_REPRESENTED_TEST_P44_TRACE_FAILURE) != 0
                    ? BinaryenGlobalGet(function->module, P43_SITE, BinaryenTypeInt32())
                    : BinaryenConst(function->module, BinaryenLiteralInt32(0)), BinaryenTypeInt32(), P43_MEMORY),
        BinaryenGlobalSet(function->module, P44_TRACE_COUNT, BinaryenBinary(function->module,
            BinaryenAddInt32(), BinaryenGlobalGet(function->module, P44_TRACE_COUNT,
                BinaryenTypeInt32()), BinaryenConst(function->module, BinaryenLiteralInt32(1)))),
    };
    return BinaryenIf(function->module, BinaryenBinary(function->module, BinaryenLtUInt32(),
        BinaryenGlobalGet(function->module, P44_TRACE_COUNT, BinaryenTypeInt32()), BinaryenConst(
            function->module, BinaryenLiteralInt32(P44_TRACE_CAPACITY))), BinaryenBlock(function->module,
                NULL, write, 4, BinaryenTypeNone()), BinaryenGlobalSet(function->module,
                    P44_TRACE_OVERFLOW, BinaryenConst(function->module, BinaryenLiteralInt32(1))));
#else
    (void)action; (void)context; (void)executed;
    return BinaryenNop(function->module);
#endif
}

/* One hook-only route marker shares the bounded action ledger without
 * impersonating an action. The packed event/role pair is derived only from the
 * already-authenticated P3.3 transition selected by CHECK_REFINED. */
static BinaryenExpressionRef represented_refinement_trace_emit(
    const RepresentedFunction *function, const SolMirRuntimeCleanupEvent *event,
    const SolMirRuntimeCleanupTransition *transition) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    const SolMirRuntimeCleanup *cleanup = function->request->program->cleanup;
    if (!represented_test_p44_refinement_trace_probe) return BinaryenNop(function->module);
    if (event == NULL || transition == NULL || event < cleanup->events
        || event >= cleanup->events + cleanup->event_count
        || transition < cleanup->transitions
        || transition >= cleanup->transitions + cleanup->transition_count)
        return NULL;
    size_t event_id = (size_t)(event - cleanup->events);
    if (event_id > UINT16_MAX || transition->event != event_id
        || (unsigned)transition->edge_role > UINT16_MAX) return NULL;
    uint32_t route = (uint32_t)event_id << 16 | (uint32_t)transition->edge_role;
    BinaryenExpressionRef base = BinaryenBinary(function->module, BinaryenAddInt32(),
        BinaryenGlobalGet(function->module, P44_TRACE_OFFSET, BinaryenTypeInt32()),
        BinaryenBinary(function->module, BinaryenMulInt32(), BinaryenGlobalGet(function->module,
            P44_TRACE_COUNT, BinaryenTypeInt32()), BinaryenConst(function->module,
                BinaryenLiteralInt32(P44_TRACE_SLOT_BYTES))));
    BinaryenExpressionRef record = transition->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
        ? BinaryenGlobalGet(function->module, P43_SITE, BinaryenTypeInt32())
        : BinaryenConst(function->module, BinaryenLiteralInt32(0));
    BinaryenExpressionRef write[] = {
        BinaryenStore(function->module, 4, 0, 4, base, BinaryenConst(function->module,
            BinaryenLiteralInt32((int32_t)SOL_WASM_REPRESENTED_TEST_P44_REFINEMENT_ROUTE)),
            BinaryenTypeInt32(), P43_MEMORY),
        BinaryenStore(function->module, 4, 4, 4, base, BinaryenConst(function->module,
            BinaryenLiteralInt32((int32_t)route)), BinaryenTypeInt32(), P43_MEMORY),
        BinaryenStore(function->module, 4, 8, 4, base, record, BinaryenTypeInt32(), P43_MEMORY),
        BinaryenGlobalSet(function->module, P44_TRACE_COUNT, BinaryenBinary(function->module,
            BinaryenAddInt32(), BinaryenGlobalGet(function->module, P44_TRACE_COUNT,
                BinaryenTypeInt32()), BinaryenConst(function->module, BinaryenLiteralInt32(1)))),
    };
    return BinaryenIf(function->module, BinaryenBinary(function->module, BinaryenLtUInt32(),
        BinaryenGlobalGet(function->module, P44_TRACE_COUNT, BinaryenTypeInt32()), BinaryenConst(
            function->module, BinaryenLiteralInt32(P44_TRACE_CAPACITY))), BinaryenBlock(function->module,
                NULL, write, 4, BinaryenTypeNone()), BinaryenGlobalSet(function->module,
                    P44_TRACE_OVERFLOW, BinaryenConst(function->module, BinaryenLiteralInt32(1))));
#else
    (void)event; (void)transition;
    return BinaryenNop(function->module);
#endif
}

static bool represented_cleanup_emit(const RepresentedFunction *function,
    const SolMirRuntimeCleanupAction *action, RepresentedNodes *nodes) {
    const SolMirConcreteProgram *concrete = function->request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    size_t init = SIZE_MAX;
    switch (action->kind) {
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_TEMPORARY:
            init = temporary_init_index(function, action->target); break;
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE:
            if (action->target < m->place_count)
                init = local_init_index(function, m->places[action->target].local);
            if (action->target < m->place_count && action->drop_path != SOL_MIR_RUNTIME_NONE
                && m->places[action->target].projections.count == 0
                && m->places[action->target].final_type < concrete->layout.type_count
                && represented_callable_product_recipe(concrete, concrete->layout.types[
                    m->places[action->target].final_type].recipe)) {
                const SolMirRuntimeCleanup *cleanup = function->request->program->cleanup;
                size_t hole = local_hole_index(function, m->places[action->target].local);
                if (init == SIZE_MAX || hole == SIZE_MAX || action->drop_path >= cleanup->drop_path_count)
                    return false;
                const SolMirRuntimeCleanupDropPath *path = &cleanup->drop_paths[action->drop_path];
                if (path->root != action->target || path->place != action->target
                    || path->recipe != action->recipe
                    || path->holes.count > 1 || path->holes.offset > cleanup->drop_path_count
                    || path->holes.count > cleanup->drop_path_count - path->holes.offset)
                    return false;
                const SolMirRuntimeCleanupDropPath *moved = NULL;
                if (path->holes.count == 0) {
                    if (path->liveness != SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE
                        || (action->flags != 0 && action->flags
                            != SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED)) return false;
                } else {
                    moved = &cleanup->drop_paths[path->holes.offset];
                    if (path->liveness != SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL
                        || moved->root != path->root || (moved->liveness
                            != SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE && moved->liveness
                            != SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL)
                        || moved->recipe != path->recipe
                        || moved->place >= m->place_count
                        || m->places[moved->place].projections.count != 1
                        || m->places[moved->place].final_type >= concrete->layout.type_count
                        || !represented_unbound_function_recipe(concrete, concrete->layout.types[
                            m->places[moved->place].final_type].recipe)) return false;
                }
                SolMirRecipeId root_recipe = concrete->layout.types[m->places[action->target].final_type].recipe;
                const SolMirRecipe *product = root_recipe < concrete->representation.recipe_count
                    ? &concrete->representation.recipes[root_recipe] : NULL;
                size_t callable_field = SOL_MIR_RECIPE_NONE, text_field = SOL_MIR_RECIPE_NONE;
                if (product == NULL || root_recipe != action->recipe || product->fields.count != 2
                    || product->fields.offset > concrete->representation.field_count
                    || product->fields.count > concrete->representation.field_count
                        - product->fields.offset) return false;
                for (size_t i = 0; i < product->fields.count; ++i) {
                    size_t field = product->fields.offset + i;
                    SolMirRecipeId child = concrete->representation.fields[field].type;
                    const SolMirFieldLayout *layout_field = field < concrete->layout.field_count
                        ? &concrete->layout.fields[field] : NULL;
                    if (layout_field == NULL || layout_field->owner_recipe != root_recipe
                        || !layout_field->has_storage || layout_field->size != 4
                        || layout_field->alignment != 4) return false;
                    if (represented_unbound_function_recipe(concrete, child)) {
                        if (callable_field != SOL_MIR_RECIPE_NONE) return false;
                        callable_field = field;
                    } else if (represented_direct_text_handle(concrete, child)) {
                        if (text_field != SOL_MIR_RECIPE_NONE) return false;
                        text_field = field;
                    } else return false;
                }
                if (callable_field == SOL_MIR_RECIPE_NONE || text_field == SOL_MIR_RECIPE_NONE) return false;
                if (moved != NULL) {
                    size_t projection = m->places[moved->place].projections.offset;
                    if (projection >= concrete->layout.projection_count) return false;
                    const SolMirProjectionMap *map = &concrete->layout.projections[projection];
                    if (map->place != moved->place || map->base_recipe != root_recipe
                        || map->field_layout != callable_field || map->result_recipe
                            != concrete->representation.fields[callable_field].type
                        || map->object_offset != concrete->layout.fields[callable_field].offset) return false;
                }
                /* An authenticated zero-hole path consumes both fields.  The
                 * one-hole alternative names the exact callable projection, so
                 * only that leaf is skipped while the Text sibling and root
                 * remain owned by this root action. */
                BinaryenExpressionRef callable = moved == NULL
                    ? represented_cleanup_probe_increment(function->module, P43_CLEANUP_OLD_CALLABLE)
                    : BinaryenIf(function->module, BinaryenLocalGet(function->module,
                        (BinaryenIndex)hole, BinaryenTypeInt32()), BinaryenNop(function->module),
                        represented_cleanup_probe_increment(function->module, P43_CLEANUP_OLD_CALLABLE));
                BinaryenExpressionRef body[5] = {callable,
                    represented_cleanup_probe_increment(function->module, P43_CLEANUP_TEXT_SIBLING),
                    represented_cleanup_probe_increment(function->module, P43_CLEANUP_ROOT), NULL, NULL};
                size_t body_count = 4;
                if (moved != NULL) {
                    body[3] = BinaryenLocalSet(function->module, (BinaryenIndex)hole,
                        BinaryenConst(function->module, BinaryenLiteralInt32(0)));
                    body[4] = BinaryenLocalSet(function->module, (BinaryenIndex)init,
                        BinaryenConst(function->module, BinaryenLiteralInt32(0)));
                    body_count = 5;
                } else body[3] = BinaryenLocalSet(function->module, (BinaryenIndex)init,
                    BinaryenConst(function->module, BinaryenLiteralInt32(0)));
                for (size_t i = 0; i < body_count; ++i) if (body[i] == NULL) return false;
                return represented_nodes_push(nodes, BinaryenIf(function->module,
                    BinaryenLocalGet(function->module, (BinaryenIndex)init, BinaryenTypeInt32()),
                    BinaryenBlock(function->module, NULL, body, (BinaryenIndex)body_count,
                        BinaryenTypeNone()), NULL));
            }
            break;
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER:
            init = local_init_index(function, action->target); break;
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT: {
            const SolMirOperations *operations = &concrete->operations;
            if (action->target >= operations->snapshot_count
                || !represented_snapshot_cleanup_action(function->request, action)) return false;
            const SolMirOperationSnapshotPlan *snapshot = &operations->snapshots[action->target];
            init = snapshot->image == function->image_id
                ? snapshot_init_index(function, snapshot->slot) : SIZE_MAX;
            if (init == SIZE_MAX) return false;
            BinaryenExpressionRef clear = BinaryenLocalSet(function->module,
                (BinaryenIndex)init, BinaryenConst(function->module, BinaryenLiteralInt32(0)));
            BinaryenExpressionRef missing = BinaryenReturn(function->module, i64(function, 0));
            return clear != NULL && missing != NULL && represented_nodes_push(nodes,
                BinaryenIf(function->module, BinaryenLocalGet(function->module,
                    (BinaryenIndex)init, BinaryenTypeInt32()), clear, missing));
        }
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_SCOPE:
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_REGION:
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE:
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_CHECK_CONTRACT:
            return true;
        default: return false;
    }
    if (init == SIZE_MAX) return false;
    if (represented_moved_callable_cleanup_action(function, action)) {
        BinaryenExpressionRef body[] = {
            represented_cleanup_probe_increment(function->module, P43_CLEANUP_MOVED_CALLABLE),
            BinaryenLocalSet(function->module, (BinaryenIndex)init,
                BinaryenConst(function->module, BinaryenLiteralInt32(0))),
        };
        return body[0] != NULL && body[1] != NULL && represented_nodes_push(nodes,
            BinaryenIf(function->module, BinaryenLocalGet(function->module, (BinaryenIndex)init,
                BinaryenTypeInt32()), BinaryenBlock(function->module, NULL, body, 2,
                    BinaryenTypeNone()), NULL));
    }
    return represented_nodes_push(nodes, BinaryenLocalSet(function->module,
        (BinaryenIndex)init, BinaryenConst(function->module, BinaryenLiteralInt32(0))));
}

/* Keep the pre-existing emitter authoritative for action semantics. The ledger
 * is appended only at authenticated emission sites; callers with no event
 * deliberately keep using represented_cleanup_emit directly. */
static size_t represented_cleanup_action_init_index(const RepresentedFunction *function,
    const SolMirRuntimeCleanupAction *action) {
    const SolMirMaterialization *m = &function->request->program->conventions->concrete->materialization;
    switch (action->kind) {
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_TEMPORARY:
            return temporary_init_index(function, action->target);
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER:
            return local_init_index(function, action->target);
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT: {
            const SolMirOperations *operations = &function->request->program->conventions
                ->concrete->operations;
            return action->target < operations->snapshot_count
                && operations->snapshots[action->target].image == function->image_id
                ? snapshot_init_index(function, operations->snapshots[action->target].slot)
                : SIZE_MAX;
        }
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE:
            return action->target < m->place_count
                ? local_init_index(function, m->places[action->target].local) : SIZE_MAX;
        default: return SIZE_MAX;
    }
}

static bool represented_cleanup_emit_traced(const RepresentedFunction *function,
    const SolMirRuntimeCleanupAction *action, const RepresentedCleanupTraceContext *trace,
    RepresentedNodes *nodes) {
    if (!represented_cleanup_trace_enabled()) return represented_cleanup_emit(function, action, nodes);
    if ((action->flags & SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED) != 0) {
        size_t init = represented_cleanup_action_init_index(function, action);
        if (init == SIZE_MAX) return false;
        BinaryenExpressionRef executed = represented_cleanup_trace_emit(function, action, trace, true);
        BinaryenExpressionRef skipped = represented_cleanup_trace_emit(function, action, trace, false);
        RepresentedNodes consumed = {0};
        bool ok = executed != NULL && skipped != NULL
            && represented_nodes_push(&consumed, executed)
            && represented_cleanup_emit(function, action, &consumed);
        BinaryenExpressionRef consume = ok ? BinaryenBlock(function->module, NULL, consumed.items,
            (BinaryenIndex)consumed.count, BinaryenTypeNone()) : NULL;
        deallocate(consumed.items);
        return ok && represented_nodes_push(nodes,
            BinaryenIf(function->module, BinaryenLocalGet(function->module, (BinaryenIndex)init,
                BinaryenTypeInt32()), consume, skipped));
    }
    return represented_cleanup_emit(function, action, nodes)
        && represented_nodes_push(nodes, represented_cleanup_trace_emit(function, action, trace, true));
}

static bool represented_cleanup_actions_emit_traced(const RepresentedFunction *function,
    const SolMirRuntimeCleanupEvent *event, const SolMirRuntimeCleanupTransition *transition,
    RepresentedNodes *nodes) {
    const SolMirRuntimeCleanup *cleanup = function->request->program->cleanup;
    if (event == NULL || transition == NULL || transition->event >= cleanup->event_count
        || &cleanup->events[transition->event] != event
        || transition->actions.offset > cleanup->action_count
        || transition->actions.count > cleanup->action_count - transition->actions.offset) return false;
    RepresentedCleanupTraceContext trace = {event, transition, false};
    for (size_t i = 0; i < transition->actions.count; ++i)
        if (!represented_cleanup_emit_traced(function,
                &cleanup->actions[transition->actions.offset + i], &trace, nodes)) return false;
    return true;
}

static bool represented_cleanup_marker_trace_context(const RepresentedFunction *function,
    size_t instruction, RepresentedCleanupTraceContext *trace) {
    const SolMirRuntimeLoweredProgram *owner = function->request->program;
    const SolMirRuntimeCleanup *cleanup = owner->cleanup;
    if (trace == NULL || instruction >= owner->image_instruction_count) return false;
    size_t event_id = owner->image_instructions[instruction].cleanup_event;
    if (event_id >= cleanup->event_count) return false;
    const SolMirRuntimeCleanupEvent *event = &cleanup->events[event_id];
    if (event->transitions.count != 1 || event->transitions.offset >= cleanup->transition_count) return false;
    const SolMirRuntimeCleanupTransition *transition = &cleanup->transitions[event->transitions.offset];
    if (transition->event != event_id) return false;
    *trace = (RepresentedCleanupTraceContext){event, transition, false};
    return true;
}

/* The checked C2b preflight has proved that this is the sole normal-only
 * WRITEBACK action for the callback.  Its result word is retained in scratch
 * until the packet branch selects this normal body; neither failure cleanup nor
 * a projected/aliased place can reach this store. */
static bool represented_receiver_writeback_emit(const RepresentedFunction *function,
    const RepresentedCallCatalog *call, const SolMirRuntimeCleanupAction *action,
    const RepresentedCleanupTraceContext *trace, RepresentedNodes *nodes) {
    const SolMirRuntimeConventions *conventions = function->request->program->conventions;
    const SolMirMaterialization *m = &conventions->concrete->materialization;
    if (call->call >= conventions->call_count || action == NULL
        || action->kind != SOL_MIR_RUNTIME_CLEANUP_ACTION_WRITEBACK) return false;
    const SolMirRuntimeCall *source = &conventions->calls[call->call];
    const SolMirMaterializedTerminator *term = call->caller_block < m->block_count
        ? &m->blocks[call->caller_block].terminator : NULL;
    const SolMirRuntimeSignature *signature = source->signature < conventions->signature_count
        ? &conventions->signatures[source->signature] : NULL;
    if (term == NULL || signature == NULL || !(source->call_kind == SOL_IR_CALL_CALLBACK
            ? represented_callback_inout_exact(function->request, source, term, signature)
            : represented_method_exact(function->request, source, term, signature))
        || source->writebacks.offset >= conventions->writeback_count)
        return false;
    const SolMirRuntimeWriteback *writeback = &conventions->writebacks[source->writebacks.offset];
    size_t local = writeback->place < m->place_count ? local_index(function,
        m->places[writeback->place].local) : SIZE_MAX;
    size_t init = writeback->place < m->place_count ? local_init_index(function,
        m->places[writeback->place].local) : SIZE_MAX;
    bool ok = action->flags == SOL_MIR_RUNTIME_CLEANUP_ACTION_NORMAL_ONLY
        && action->target == writeback->place && action->recipe == writeback->recipe
        && action->drop_path == SOL_MIR_RUNTIME_NONE && local != SIZE_MAX && init != SIZE_MAX
        && represented_nodes_push(nodes, BinaryenLocalSet(function->module, (BinaryenIndex)local,
            BinaryenLocalGet(function->module, (BinaryenIndex)function->scratch_base,
                BinaryenTypeInt64()))) && represented_nodes_push(nodes, BinaryenLocalSet(function->module,
            (BinaryenIndex)init, BinaryenConst(function->module, BinaryenLiteralInt32(1))));
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (ok && represented_test_callback_writeback_probe)
        ok = represented_nodes_push(nodes, BinaryenGlobalSet(function->module, P43_WRITEBACKS,
            BinaryenBinary(function->module, BinaryenAddInt32(), BinaryenGlobalGet(function->module,
                P43_WRITEBACKS, BinaryenTypeInt32()), BinaryenConst(function->module,
                BinaryenLiteralInt32(1)))));
#endif
    if (ok && represented_cleanup_trace_enabled()) ok = represented_nodes_push(nodes,
        represented_cleanup_trace_emit(function, action, trace, true));
    return ok;
}

static bool represented_failure_record_index(const RepresentedProvenance *provenance,
    size_t site_id, size_t *index) {
    if (provenance == NULL || index == NULL || site_id >= provenance->failure_site_count
        || provenance->failure_record_indices == NULL
        || provenance->failure_record_indices[site_id] == 0) return false;
    *index = provenance->failure_record_indices[site_id];
    return true;
}

static bool represented_supplemental_record_index(const RepresentedProvenance *provenance,
    size_t site_id, size_t *index) {
    if (provenance == NULL || index == NULL || site_id >= provenance->supplemental_site_count
        || provenance->supplemental_record_indices == NULL
        || provenance->supplemental_record_indices[site_id] == 0) return false;
    *index = provenance->supplemental_record_indices[site_id];
    return true;
}

/* Authentication has already certified this exact PRE_STEP pair. Never select
 * this route merely because a previously active packet contains code six. */
static bool represented_step_failure_emit(const RepresentedFunction *function,
    size_t event_id, RepresentedNodes *nodes) {
    const SolMirRuntimeCleanup *cleanup = function->request->program->cleanup;
    if (event_id >= cleanup->event_count) return false;
    const SolMirRuntimeCleanupEvent *event = &cleanup->events[event_id];
    if (event->transitions.count != 2 || event->transitions.offset >= cleanup->transition_count
        || cleanup->transition_count - event->transitions.offset < 2) return false;
    const SolMirRuntimeCleanupTransition *failure = &cleanup->transitions[event->transitions.offset + 1];
    return failure->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_STEP_FAILURE
        && represented_cleanup_actions_emit_traced(function, event, failure, nodes)
        && represented_nodes_push(nodes, BinaryenReturn(function->module, i64(function, 0)));
}

static bool represented_image_step_emit(const RepresentedFunction *function,
    size_t site, size_t event, RepresentedNodes *nodes) {
    size_t record = 0;
    if (!represented_failure_record_index(function->provenance, site, &record)) return false;
    RepresentedNodes failed = {0};
    bool ok = represented_step_failure_emit(function, event, &failed);
    BinaryenExpressionRef failure = ok ? BinaryenBlock(function->module, NULL, failed.items,
        (BinaryenIndex)failed.count, BinaryenTypeNone()) : NULL;
    deallocate(failed.items);
    return ok && represented_nodes_push(nodes, BinaryenIf(function->module,
        BinaryenUnary(function->module, BinaryenEqZInt32(), represented_step_call(function->module,
            i64(function, (int64_t)record))), failure, NULL));
}

static BinaryenExpressionRef represented_instruction_step_record(const RepresentedFunction *function,
    size_t instruction) {
    size_t record = 0;
    return represented_failure_record_index(function->provenance,
        function->request->program->image_instructions[instruction].step_failure_site, &record)
        ? i64(function, (int64_t)record) : NULL;
}

static bool represented_failure_emit(const RepresentedFunction *function, size_t instruction,
    SolMirRuntimeFailureCode code, RepresentedNodes *nodes) {
    const SolMirRuntimeLoweredProgram *owner = function->request->program;
    const SolMirRuntimeCleanup *cleanup = owner->cleanup;
    const SolMirRuntimeLoweredImageInstruction *row = &owner->image_instructions[instruction];
    if (row->cleanup_event >= cleanup->event_count) return false;
    const SolMirRuntimeCleanupEvent *event = &cleanup->events[row->cleanup_event];
    const SolMirRuntimeCleanupTransition *failure = NULL;
    for (size_t i = 0; i < event->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *candidate = &cleanup->transitions[
            event->transitions.offset + i];
        if (candidate->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE) {
            if (failure != NULL) return false;
            failure = candidate;
        }
    }
    if (failure == NULL || failure->actions.offset > cleanup->action_count
        || failure->actions.count > cleanup->action_count - failure->actions.offset) return false;
    size_t record = 0;
    if (!represented_failure_record_index(function->provenance, row->failure_site, &record)) return false;
    if (!represented_nodes_push(nodes, BinaryenGlobalSet(function->module, P43_CODE,
            BinaryenConst(function->module, BinaryenLiteralInt32((int32_t)code))))
        || !represented_nodes_push(nodes, BinaryenGlobalSet(function->module, P43_SITE,
            BinaryenConst(function->module, BinaryenLiteralInt32((int32_t)record)))))
        return false;
    if (!represented_cleanup_actions_emit_traced(function, event, failure, nodes)) return false;
    return represented_nodes_push(nodes, BinaryenReturn(function->module, i64(function, 0)));
}

static bool represented_terminal_failure_emit(const RepresentedFunction *function, size_t block,
    SolMirRuntimeFailureOriginKind origin, SolMirRuntimeFailureCode code,
    SolMirRuntimeFailureDetailKind detail, RepresentedNodes *nodes) {
    const SolWasmRepresentedBuildRequest *request = function->request;
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirMaterialization *m = &owner->conventions->concrete->materialization;
    if (block >= m->block_count || block >= owner->image_terminator_count) return false;
    const SolMirMaterializedTerminator *term = &m->blocks[block].terminator;
    const SolMirRuntimeLoweredImageTerminator *row = &owner->image_terminators[block];
    if (!represented_terminal_failure_route(request, function->image, function->image_id, block,
            term->kind, origin, code, detail)) return false;
    /* Capture before cleanup: P3.3 owns the original byte length and the
     * bounded copied prefix, while this private helper never touches the heap. */
    if (detail == SOL_MIR_RUNTIME_FAILURE_DETAIL_PANIC_TEXT) {
        BinaryenExpressionRef value = get_value(function, term->value);
        size_t step_record = 0;
        if (!represented_failure_record_index(function->provenance, row->step_failure_site,
                &step_record)) return false;
        BinaryenExpressionRef arguments[] = {value, i64(function, (int64_t)step_record)};
        if (value == NULL) return false;
        RepresentedNodes failed = {0};
        bool ok = represented_step_failure_emit(function, row->step_cleanup_event, &failed);
        BinaryenExpressionRef failure = ok ? BinaryenBlock(function->module, NULL, failed.items,
            (BinaryenIndex)failed.count, BinaryenTypeNone()) : NULL;
        deallocate(failed.items);
        if (!ok || !represented_nodes_push(nodes, BinaryenIf(function->module,
                BinaryenUnary(function->module, BinaryenEqZInt32(), BinaryenCall(function->module,
                    P44_PANIC_CAPTURE, arguments, 2, BinaryenTypeInt32())), failure, NULL))) return false;
    }
    size_t record = 0;
    if (!represented_failure_record_index(function->provenance, row->failure_site, &record)
        || !represented_nodes_push(nodes, BinaryenGlobalSet(function->module, P43_CODE,
            BinaryenConst(function->module, BinaryenLiteralInt32((int32_t)code))))
        || !represented_nodes_push(nodes, BinaryenGlobalSet(function->module, P43_SITE,
            BinaryenConst(function->module, BinaryenLiteralInt32((int32_t)record))))) return false;
    const SolMirRuntimeCleanupEvent *event = &owner->cleanup->events[row->cleanup_event];
    const SolMirRuntimeCleanupTransition *failure = NULL;
    for (size_t i = 0; i < event->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *candidate = &owner->cleanup->transitions[
            event->transitions.offset + i];
        if (candidate->event == row->cleanup_event
            && candidate->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
            && candidate->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE) {
            if (failure != NULL) return false;
            failure = candidate;
        }
    }
    if (failure == NULL || failure->actions.offset > owner->cleanup->action_count
        || failure->actions.count > owner->cleanup->action_count - failure->actions.offset) return false;
    if (!represented_cleanup_actions_emit_traced(function, event, failure, nodes)) return false;
    return represented_nodes_push(nodes, BinaryenReturn(function->module, i64(function, 0)));
}

/* The allocation helper has already written the authenticated code and site.
 * This emits precisely the terminal supplemental transition's actions once. */
static bool represented_supplemental_failure_emit(const RepresentedFunction *function,
    size_t instruction, RepresentedNodes *nodes) {
    const SolMirRuntimeCleanup *cleanup = function->request->program->cleanup;
    size_t supplemental = 0;
    if (!represented_text_allocation_route(function->request, instruction, &supplemental)
        || supplemental >= cleanup->supplemental_site_count) return false;
    const SolMirRuntimeCleanupSupplementalSite *site = &cleanup->supplemental_sites[supplemental];
    if (site->event >= cleanup->event_count) return false;
    const SolMirRuntimeCleanupEvent *event = &cleanup->events[site->event];
    const SolMirRuntimeCleanupTransition *failure = NULL;
    for (size_t i = 0; i < event->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *candidate = &cleanup->transitions[
            event->transitions.offset + i];
        if (candidate->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE) {
            if (failure != NULL) return false;
            failure = candidate;
        }
    }
    if (failure == NULL || failure->actions.offset > cleanup->action_count
        || failure->actions.count > cleanup->action_count - failure->actions.offset) return false;
    if (!represented_cleanup_actions_emit_traced(function, event, failure, nodes)) return false;
    return represented_nodes_push(nodes, BinaryenReturn(function->module, i64(function, 0)));
}

static bool represented_supplemental_normal_emit(const RepresentedFunction *function,
    size_t instruction, RepresentedNodes *nodes) {
    const SolMirRuntimeCleanup *cleanup = function->request->program->cleanup;
    size_t supplemental = 0;
    if (!represented_text_allocation_route(function->request, instruction, &supplemental)
        || supplemental >= cleanup->supplemental_site_count) return false;
    const SolMirRuntimeCleanupSupplementalSite *site = &cleanup->supplemental_sites[supplemental];
    if (site->event >= cleanup->event_count) return false;
    const SolMirRuntimeCleanupEvent *event = &cleanup->events[site->event];
    const SolMirRuntimeCleanupTransition *normal = NULL;
    for (size_t i = 0; i < event->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *candidate = &cleanup->transitions[
            event->transitions.offset + i];
        if (candidate->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL) {
            if (normal != NULL) return false;
            normal = candidate;
        }
    }
    if (normal == NULL || normal->actions.offset > cleanup->action_count
        || normal->actions.count > cleanup->action_count - normal->actions.offset) return false;
    if (!represented_cleanup_actions_emit_traced(function, event, normal, nodes)) return false;
    return true;
}

static bool represented_copy_failure_emit(const RepresentedFunction *function,
    size_t instruction, RepresentedNodes *nodes) {
    /* Copy helpers return -1 only for their own exhausted byte tick, zero for
     * allocation failure, and a positive Wasm32 handle on completion. Nested
     * copies preserve that marker. An existing code-six packet is not evidence
     * of a new local step failure. */
    RepresentedNodes step = {0}, allocation = {0};
    bool ok = represented_step_failure_emit(function,
        function->request->program->image_instructions[instruction].step_cleanup_event, &step)
        && represented_supplemental_failure_emit(function, instruction, &allocation);
    BinaryenExpressionRef yes = ok ? BinaryenBlock(function->module, NULL, step.items,
        (BinaryenIndex)step.count, BinaryenTypeNone()) : NULL;
    BinaryenExpressionRef no = ok ? BinaryenBlock(function->module, NULL, allocation.items,
        (BinaryenIndex)allocation.count, BinaryenTypeNone()) : NULL;
    deallocate(step.items); deallocate(allocation.items);
    return ok && represented_nodes_push(nodes, BinaryenIf(function->module,
        BinaryenBinary(function->module, BinaryenEqInt64(), BinaryenLocalGet(function->module,
            (BinaryenIndex)function->scratch_base, BinaryenTypeInt64()),
            i64(function, P44_COPY_STEP_FAILURE)), yes, no));
}

static bool represented_text_allocate_emit(const RepresentedFunction *function, size_t instruction,
    BinaryenExpressionRef source, size_t destination, RepresentedNodes *nodes) {
    size_t supplemental = 0, record = 0;
    if (source == NULL || destination == SIZE_MAX
        || !represented_text_allocation_route(function->request, instruction, &supplemental)
        || !represented_supplemental_record_index(function->provenance, supplemental, &record)) return false;
    BinaryenExpressionRef step_record = represented_instruction_step_record(function, instruction);
    if (step_record == NULL) return false;
    BinaryenExpressionRef arguments[] = {source, i64(function, (int64_t)record), step_record};
    if (!represented_nodes_push(nodes, BinaryenLocalSet(function->module,
            (BinaryenIndex)function->scratch_base, BinaryenCall(function->module, P43_TEXT_COPY,
                arguments, 3, BinaryenTypeInt64())))) return false;
    RepresentedNodes failed = {0};
    bool ok = represented_copy_failure_emit(function, instruction, &failed);
    BinaryenExpressionRef failure = ok ? BinaryenBlock(function->module, NULL, failed.items,
        (BinaryenIndex)failed.count, BinaryenTypeNone()) : NULL;
    deallocate(failed.items);
    RepresentedNodes succeeded = {0};
    if (ok) ok = represented_nodes_push(&succeeded, BinaryenLocalSet(function->module,
        (BinaryenIndex)destination, BinaryenLocalGet(function->module,
            (BinaryenIndex)function->scratch_base, BinaryenTypeInt64())))
        && represented_supplemental_normal_emit(function, instruction, &succeeded);
    BinaryenExpressionRef success = ok ? BinaryenBlock(function->module, NULL, succeeded.items,
        (BinaryenIndex)succeeded.count, BinaryenTypeNone()) : NULL;
    deallocate(succeeded.items);
    return ok && represented_nodes_push(nodes, BinaryenIf(function->module, BinaryenBinary(
        function->module, BinaryenLeSInt64(), BinaryenLocalGet(function->module,
             (BinaryenIndex)function->scratch_base, BinaryenTypeInt64()), i64(function, 0)), failure, success));
}

static bool represented_function_value_emit(const RepresentedFunction *function, size_t instruction,
    size_t destination, RepresentedNodes *nodes) {
    const SolMirConcreteProgram *concrete = function->request->program->conventions->concrete;
    const SolMirLayout *layout = &concrete->layout;
    const SolMirMaterialization *m = &concrete->materialization;
    size_t table = 0, supplemental = 0, record = 0;
    if (destination == SIZE_MAX || instruction >= m->instruction_count
        || !represented_function_value_route(function->request, instruction, NULL, &table, NULL)
        || table == SIZE_MAX || table >= UINT32_MAX
        || m->instructions[instruction].type >= layout->type_count
        || layout->types[m->instructions[instruction].type].object_size != layout->target.pointer_size
        || layout->types[m->instructions[instruction].type].object_alignment
            != layout->target.pointer_alignment
        || layout->types[m->instructions[instruction].type].target_token_offset != 0
        || !represented_text_allocation_route(function->request, instruction, &supplemental)
        || !represented_supplemental_record_index(function->provenance, supplemental, &record)) return false;
    BinaryenExpressionRef arguments[] = {i64(function, (int64_t)layout->types[
        m->instructions[instruction].type].object_size), i64(function, (int64_t)record)};
    if (!represented_nodes_push(nodes, BinaryenLocalSet(function->module,
            (BinaryenIndex)function->scratch_base, BinaryenCall(function->module, P43_FIXED_ALLOC,
                arguments, 2, BinaryenTypeInt64())))) return false;
    RepresentedNodes failed = {0}, succeeded = {0};
    bool ok = represented_supplemental_failure_emit(function, instruction, &failed);
    BinaryenExpressionRef failure = ok ? BinaryenBlock(function->module, NULL, failed.items,
        (BinaryenIndex)failed.count, BinaryenTypeNone()) : NULL;
    deallocate(failed.items);
    if (ok) ok = represented_nodes_push(&succeeded, BinaryenStore(function->module, 4, 0, 4,
        wrap_i64(function->module, BinaryenLocalGet(function->module,
            (BinaryenIndex)function->scratch_base, BinaryenTypeInt64())), BinaryenConst(function->module,
            BinaryenLiteralInt32((int32_t)(table + 1))), BinaryenTypeInt32(), P43_MEMORY))
        && represented_nodes_push(&succeeded, BinaryenLocalSet(function->module,
            (BinaryenIndex)destination, BinaryenLocalGet(function->module,
                (BinaryenIndex)function->scratch_base, BinaryenTypeInt64())))
        && represented_supplemental_normal_emit(function, instruction, &succeeded);
    BinaryenExpressionRef success = ok ? BinaryenBlock(function->module, NULL, succeeded.items,
        (BinaryenIndex)succeeded.count, BinaryenTypeNone()) : NULL;
    deallocate(succeeded.items);
    return ok && represented_nodes_push(nodes, BinaryenIf(function->module, BinaryenBinary(
        function->module, BinaryenEqInt64(), BinaryenLocalGet(function->module,
            (BinaryenIndex)function->scratch_base, BinaryenTypeInt64()), i64(function, 0)), failure, success));
}

static bool represented_product_copy_emit(const RepresentedFunction *function, size_t instruction,
    SolMirRecipeId recipe, BinaryenExpressionRef source, size_t destination, RepresentedNodes *nodes) {
    size_t supplemental = 0, record = 0;
    char name[64];
    if (source == NULL || destination == SIZE_MAX || !represented_product_copy_needed(function->request,
            recipe) || !represented_product_helper_name(name, "copy", recipe)
        || !represented_text_allocation_route(function->request, instruction, &supplemental)
        || !represented_supplemental_record_index(function->provenance, supplemental, &record)) {
        return false;
    }
    BinaryenExpressionRef step_record = represented_instruction_step_record(function, instruction);
    if (step_record == NULL) return false;
    BinaryenExpressionRef arguments[] = {source, i64(function, (int64_t)record), step_record};
    if (!represented_nodes_push(nodes, BinaryenLocalSet(function->module,
            (BinaryenIndex)function->scratch_base, BinaryenCall(function->module, name,
                arguments, 3, BinaryenTypeInt64())))) return false;
    RepresentedNodes failed = {0}, succeeded = {0};
    bool ok = represented_copy_failure_emit(function, instruction, &failed);
    BinaryenExpressionRef failure = ok ? BinaryenBlock(function->module, NULL, failed.items,
        (BinaryenIndex)failed.count, BinaryenTypeNone()) : NULL;
    deallocate(failed.items);
    if (ok) ok = represented_nodes_push(&succeeded, BinaryenLocalSet(function->module,
        (BinaryenIndex)destination, BinaryenLocalGet(function->module,
            (BinaryenIndex)function->scratch_base, BinaryenTypeInt64())))
        && represented_supplemental_normal_emit(function, instruction, &succeeded);
    BinaryenExpressionRef success = ok ? BinaryenBlock(function->module, NULL, succeeded.items,
        (BinaryenIndex)succeeded.count, BinaryenTypeNone()) : NULL;
    deallocate(succeeded.items);
    return ok && represented_nodes_push(nodes, BinaryenIf(function->module, BinaryenBinary(
        function->module, BinaryenLeSInt64(), BinaryenLocalGet(function->module,
             (BinaryenIndex)function->scratch_base, BinaryenTypeInt64()), i64(function, 0)), failure, success));
}

static bool represented_sum_copy_emit(const RepresentedFunction *function, size_t instruction,
    SolMirRecipeId recipe, BinaryenExpressionRef source, size_t destination, RepresentedNodes *nodes) {
    size_t supplemental = 0, record = 0;
    char name[64];
    if (source == NULL || destination == SIZE_MAX || !represented_sum_copy_needed(function->request,
            recipe) || !represented_sum_helper_name(name, "copy", recipe)
        || !represented_text_allocation_route(function->request, instruction, &supplemental)
        || !represented_supplemental_record_index(function->provenance, supplemental, &record)) {
        return false;
    }
    BinaryenExpressionRef step_record = represented_instruction_step_record(function, instruction);
    if (step_record == NULL) return false;
    BinaryenExpressionRef arguments[] = {source, i64(function, (int64_t)record), step_record};
    if (!represented_nodes_push(nodes, BinaryenLocalSet(function->module,
            (BinaryenIndex)function->scratch_base, BinaryenCall(function->module, name,
                arguments, 3, BinaryenTypeInt64())))) return false;
    RepresentedNodes failed = {0}, succeeded = {0};
    bool ok = represented_copy_failure_emit(function, instruction, &failed);
    BinaryenExpressionRef failure = ok ? BinaryenBlock(function->module, NULL, failed.items,
        (BinaryenIndex)failed.count, BinaryenTypeNone()) : NULL;
    deallocate(failed.items);
    if (ok) ok = represented_nodes_push(&succeeded, BinaryenLocalSet(function->module,
        (BinaryenIndex)destination, BinaryenLocalGet(function->module,
            (BinaryenIndex)function->scratch_base, BinaryenTypeInt64())))
        && represented_supplemental_normal_emit(function, instruction, &succeeded);
    BinaryenExpressionRef success = ok ? BinaryenBlock(function->module, NULL, succeeded.items,
        (BinaryenIndex)succeeded.count, BinaryenTypeNone()) : NULL;
    deallocate(succeeded.items);
    return ok && represented_nodes_push(nodes, BinaryenIf(function->module, BinaryenBinary(
        function->module, BinaryenLeSInt64(), BinaryenLocalGet(function->module,
             (BinaryenIndex)function->scratch_base, BinaryenTypeInt64()), i64(function, 0)), failure, success));
}

static bool represented_checked_emit(const RepresentedFunction *function, size_t instruction,
    const SolMirOperationArithmeticPlan *plan, size_t destination, RepresentedNodes *nodes) {
    BinaryenExpressionRef overflow = checked_overflow(function, plan);
    BinaryenExpressionRef success = checked_value(function, plan);
    if (overflow == NULL || success == NULL || destination == SIZE_MAX) return false;
    RepresentedNodes overflow_nodes = {0};
    bool ok = represented_failure_emit(function, instruction,
        SOL_MIR_RUNTIME_FAILURE_INTEGER_OVERFLOW, &overflow_nodes);
    BinaryenExpressionRef overflow_body = ok ? BinaryenBlock(function->module, NULL,
        overflow_nodes.items, (BinaryenIndex)overflow_nodes.count, BinaryenTypeNone()) : NULL;
    deallocate(overflow_nodes.items);
    BinaryenExpressionRef success_set = ok ? BinaryenLocalSet(function->module,
        (BinaryenIndex)destination, success) : NULL;
    if (plan->opcode == SOL_MIR_OPERATION_I64_DIV || plan->opcode == SOL_MIR_OPERATION_I64_REM) {
        BinaryenExpressionRef divisor = get_value(function, plan->right);
        RepresentedNodes zero_nodes = {0};
        ok = ok && divisor != NULL && represented_failure_emit(function, instruction,
            SOL_MIR_RUNTIME_FAILURE_DIVISION_BY_ZERO, &zero_nodes);
        BinaryenExpressionRef zero_body = ok ? BinaryenBlock(function->module, NULL,
            zero_nodes.items, (BinaryenIndex)zero_nodes.count, BinaryenTypeNone()) : NULL;
        deallocate(zero_nodes.items);
        BinaryenExpressionRef otherwise = ok ? BinaryenIf(function->module, overflow,
            overflow_body, success_set) : NULL;
        return ok && represented_nodes_push(nodes, BinaryenIf(function->module,
            BinaryenBinary(function->module, BinaryenEqInt64(), divisor, i64(function, 0)),
            zero_body, otherwise));
    }
    return ok && represented_nodes_push(nodes, BinaryenIf(function->module, overflow,
        overflow_body, success_set));
}

static bool represented_fixed_product_emit(const RepresentedFunction *function, size_t instruction,
    const SolMirOperationConstructPlan *plan, size_t destination, RepresentedNodes *nodes) {
    const SolMirConcreteProgram *concrete = function->request->program->conventions->concrete;
    const SolMirLayout *layout = &concrete->layout;
    size_t supplemental = 0, record = 0;
    if (destination == SIZE_MAX || plan->result_recipe >= layout->type_count
        || !represented_text_allocation_route(function->request, instruction, &supplemental)
        || !represented_supplemental_record_index(function->provenance, supplemental, &record)) return false;
    BinaryenExpressionRef arguments[] = {i64(function, (int64_t)layout->types[plan->result_recipe].object_size),
        i64(function, (int64_t)record)};
    if (!represented_nodes_push(nodes, BinaryenLocalSet(function->module,
            (BinaryenIndex)function->scratch_base, BinaryenCall(function->module, P43_FIXED_ALLOC,
                arguments, 2, BinaryenTypeInt64())))) return false;
    RepresentedNodes failed = {0}, succeeded = {0};
    bool ok = represented_supplemental_failure_emit(function, instruction, &failed);
    BinaryenExpressionRef failure = ok ? BinaryenBlock(function->module, NULL, failed.items,
        (BinaryenIndex)failed.count, BinaryenTypeNone()) : NULL;
    deallocate(failed.items);
    if (plan->kind == SOL_MIR_OPERATION_CONSTRUCT_SUM) {
        if (plan->result_recipe >= layout->type_count || layout->types[plan->result_recipe].tag_offset != 0
            || layout->types[plan->result_recipe].tag_size != 4) ok = false;
        if (ok) ok = represented_nodes_push(&succeeded, BinaryenStore(function->module, 4, 0, 4,
            wrap_i64(function->module, BinaryenLocalGet(function->module,
                (BinaryenIndex)function->scratch_base, BinaryenTypeInt64())), BinaryenConst(function->module,
                BinaryenLiteralInt32((int32_t)plan->semantic_tag)), BinaryenTypeInt32(), P43_MEMORY));
    }
    for (size_t i = 0; ok && i < plan->operands.count; ++i) {
        const SolMirOperationConstructOperand *operand = &concrete->operations.construct_operands[
            plan->operands.offset + i];
        const SolMirFieldLayout *field = operand->layout_field < layout->field_count
            ? &layout->fields[operand->layout_field] : NULL;
        BinaryenExpressionRef value = temporary_get(function, operand->temporary);
        if (field == NULL || value == NULL || field->offset > UINT32_MAX
            || (field->size == 0 && !represented_zero_width_unit_field(concrete, operand->recipe, field))
            || (field->size != 0 && (!field->has_storage || (field->size != 1
                && field->size != 4 && field->size != 8)))) { ok = false; break; }
        if (field->size == 0) continue;
        if (field->size != 8) value = wrap_i64(function->module, value);
        ok = represented_nodes_push(&succeeded, BinaryenStore(function->module, (uint32_t)field->size,
            (uint32_t)field->offset, (uint32_t)field->alignment, wrap_i64(function->module,
                BinaryenLocalGet(function->module, (BinaryenIndex)function->scratch_base,
                    BinaryenTypeInt64())), value, field->size != 8 ? BinaryenTypeInt32()
                        : BinaryenTypeInt64(), P43_MEMORY));
        if (ok && (represented_indirect_recipe(concrete, operand->recipe)
                || represented_unbound_function_recipe(concrete, operand->recipe))) {
            size_t init = temporary_init_index(function, operand->temporary);
            ok = init != SIZE_MAX && represented_nodes_push(&succeeded, BinaryenLocalSet(function->module,
                (BinaryenIndex)init, BinaryenConst(function->module, BinaryenLiteralInt32(0))));
        }
    }
    if (ok) ok = represented_nodes_push(&succeeded, BinaryenLocalSet(function->module,
        (BinaryenIndex)destination, BinaryenLocalGet(function->module, (BinaryenIndex)function->scratch_base,
            BinaryenTypeInt64()))) && represented_supplemental_normal_emit(function, instruction, &succeeded);
    BinaryenExpressionRef success = ok ? BinaryenBlock(function->module, NULL, succeeded.items,
        (BinaryenIndex)succeeded.count, BinaryenTypeNone()) : NULL;
    deallocate(succeeded.items);
    return ok && represented_nodes_push(nodes, BinaryenIf(function->module, BinaryenBinary(function->module,
        BinaryenEqInt64(), BinaryenLocalGet(function->module, (BinaryenIndex)function->scratch_base,
            BinaryenTypeInt64()), i64(function, 0)), failure, success));
}

static bool represented_wrapper_emit(const RepresentedFunction *function, size_t instruction,
    const SolMirOperationConstructPlan *plan, size_t destination, RepresentedNodes *nodes) {
    const SolMirConcreteProgram *concrete = function->request->program->conventions->concrete;
    if (destination == SIZE_MAX || plan->operands.count != 1
        || plan->operands.offset >= concrete->operations.construct_operand_count) return false;
    const SolMirOperationConstructOperand *operand = &concrete->operations.construct_operands[
        plan->operands.offset];
    BinaryenExpressionRef value = temporary_get(function, operand->temporary);
    if (value == NULL || !represented_nodes_push(nodes, BinaryenLocalSet(function->module,
            (BinaryenIndex)destination, value))) return false;
    /* A distinct has no header of its own.  Clearing this one backing edge is
     * its entire logical move/drop protocol; scalar backings have no edge. */
    if (represented_indirect_recipe(concrete, plan->wrapper_backing)) {
        size_t init = temporary_init_index(function, operand->temporary);
        if (init == SIZE_MAX || !represented_nodes_push(nodes, BinaryenLocalSet(function->module,
                (BinaryenIndex)init, BinaryenConst(function->module, BinaryenLiteralInt32(0))))) return false;
    }
    (void)instruction;
    return true;
}

/* Resolve every P2 projection map in source order.  Intermediate fields must
 * be product handles; the final map supplies the exact P2 field offset and
 * physical width. */
static BinaryenExpressionRef represented_projected_load(const RepresentedFunction *function,
    const SolMirMaterializedPlace *place) {
    const SolMirConcreteProgram *concrete = function->request->program->conventions->concrete;
    const SolMirLayout *layout = &concrete->layout;
    size_t root = local_index(function, place->local);
    if (root == SIZE_MAX || place->projections.count == 0) return NULL;
    BinaryenExpressionRef address = BinaryenLocalGet(function->module, (BinaryenIndex)root,
        BinaryenTypeInt64());
    for (size_t i = 0; i < place->projections.count; ++i) {
        const SolMirProjectionMap *map = &layout->projections[place->projections.offset + i];
        if (map->field_layout >= layout->field_count) return NULL;
        const SolMirFieldLayout *field = &layout->fields[map->field_layout];
        if (!field->has_storage) return i64(function, 0);
        if ((field->size != 1 && field->size != 4 && field->size != 8) || field->offset > UINT32_MAX) return NULL;
        BinaryenExpressionRef loaded = BinaryenLoad(function->module, (uint32_t)field->size, false,
            (uint32_t)field->offset, (uint32_t)field->alignment,
            field->size == 8 ? BinaryenTypeInt64() : BinaryenTypeInt32(), BinaryenUnary(function->module,
                BinaryenWrapInt64(), address), P43_MEMORY);
        if (i + 1 == place->projections.count)
            return field->size != 8 ? extend_u32(function->module, loaded) : loaded;
        address = extend_u32(function->module, loaded);
    }
    return NULL;
}

static BinaryenExpressionRef represented_pattern_load(const RepresentedFunction *function,
    SolMirMaterializedTemporaryId scrutinee, SolMirPlanSlice path) {
    const SolMirConcreteProgram *concrete = function->request->program->conventions->concrete;
    const SolMirLayout *layout = &concrete->layout;
    BinaryenExpressionRef value = temporary_get(function, scrutinee);
    if (value == NULL || path.offset > concrete->operations.path_step_count
        || path.count > concrete->operations.path_step_count - path.offset) return NULL;
    for (size_t i = 0; i < path.count; ++i) {
        const SolMirOperationPathStep *step = &concrete->operations.path_steps[path.offset + i];
        if (step->field_layout >= layout->field_count) return NULL;
        const SolMirFieldLayout *field = &layout->fields[step->field_layout];
        if (field->size == 0 && represented_zero_width_unit_field(concrete, step->result_recipe, field))
            return i64(function, 0);
        if (!field->has_storage || field->offset > UINT32_MAX
            || (field->size != 1 && field->size != 4 && field->size != 8)) return NULL;
        BinaryenExpressionRef loaded = BinaryenLoad(function->module, (uint32_t)field->size,
            false, (uint32_t)field->offset, (uint32_t)field->alignment,
            field->size == 8 ? BinaryenTypeInt64() : BinaryenTypeInt32(),
            wrap_i64(function->module, value), P43_MEMORY);
        value = field->size == 8 ? loaded : extend_u32(function->module, loaded);
    }
    return value;
}

static bool represented_pattern_test_emit(const RepresentedFunction *function, size_t instruction,
    RepresentedNodes *nodes) {
    const SolMirConcreteProgram *concrete = function->request->program->conventions->concrete;
    const SolMirOperationPatternTest *test = represented_pattern_test_for_instruction(function->request,
        instruction);
    const SolMirMaterializedInstruction *item = &concrete->materialization.instructions[instruction];
    size_t destination = value_index(function, item->result);
    if (test == NULL || destination == SIZE_MAX || !represented_pattern_test_supported(function->request,
            function->image, instruction) || !represented_nodes_push(nodes, BinaryenLocalSet(
                function->module, (BinaryenIndex)destination, i64(function, 1)))) return false;
    for (size_t i = 0; i < test->nodes.count; ++i) {
        const SolMirOperationPatternNode *node = &concrete->operations.pattern_nodes[test->nodes.offset + i];
        BinaryenExpressionRef condition = NULL;
        if (node->kind == SOL_MIR_OPERATION_PATTERN_BOOL) {
            BinaryenExpressionRef value = represented_pattern_load(function, test->scrutinee, node->path);
            condition = value == NULL ? NULL : BinaryenBinary(function->module, BinaryenEqInt64(), value,
                i64(function, node->boolean ? 1 : 0));
        } else if (node->kind == SOL_MIR_OPERATION_PATTERN_SUM_TAG) {
            BinaryenExpressionRef handle = represented_pattern_load(function, test->scrutinee, node->path);
            condition = handle == NULL ? NULL : BinaryenBinary(function->module, BinaryenEqInt32(),
                BinaryenLoad(function->module, 4, false, 0, 4, BinaryenTypeInt32(),
                    wrap_i64(function->module, handle), P43_MEMORY), BinaryenConst(function->module,
                    BinaryenLiteralInt32((int32_t)node->semantic_tag)));
        }
        if (condition != NULL && !represented_nodes_push(nodes, BinaryenIf(function->module,
                BinaryenBinary(function->module, BinaryenNeInt64(), BinaryenLocalGet(function->module,
                    (BinaryenIndex)destination, BinaryenTypeInt64()), i64(function, 0)),
                BinaryenLocalSet(function->module, (BinaryenIndex)destination,
                    i64_bool(function->module, condition)), NULL))) return false;
    }
    return true;
}

static bool represented_pattern_value_emit(const RepresentedFunction *function, size_t instruction,
    RepresentedNodes *nodes) {
    const SolMirConcreteProgram *concrete = function->request->program->conventions->concrete;
    const SolMirOperationPatternExtraction *plan = represented_pattern_extraction_for_instruction(
        function->request, instruction);
    const SolMirMaterializedInstruction *item = &concrete->materialization.instructions[instruction];
    size_t destination = value_index(function, item->result);
    BinaryenExpressionRef value = plan == NULL ? NULL : represented_pattern_load(function,
        plan->scrutinee, plan->path);
    if (destination == SIZE_MAX || value == NULL || !represented_pattern_extraction_supported(
            function->request, function->image, instruction)) return false;
    if (plan->copy_kind == SOL_MIR_COPY_TRIVIAL)
        return represented_nodes_push(nodes, BinaryenLocalSet(function->module,
            (BinaryenIndex)destination, value));
    if (represented_text_recipe(concrete, plan->result_recipe))
        return represented_text_allocate_emit(function, instruction, value, destination, nodes);
    SolMirRecipeId physical;
    if (!represented_backing_recipe(concrete, plan->result_recipe, &physical)) return false;
    if (represented_scalar_product_recipe(concrete, physical))
        return represented_product_copy_emit(function, instruction, physical, value, destination, nodes);
    if (represented_sum_recipe_depth(concrete, physical, 0))
        return represented_sum_copy_emit(function, instruction, physical, value, destination, nodes);
    return false;
}

static bool represented_equality_emit(const RepresentedFunction *function, size_t instruction,
    const SolMirOperationArithmeticPlan *plan, size_t destination, RepresentedNodes *nodes) {
    RepresentedNodes failed = {0};
    bool ok = represented_step_failure_emit(function,
        function->request->program->image_instructions[instruction].step_cleanup_event, &failed);
    BinaryenExpressionRef failure = ok ? BinaryenBlock(function->module, NULL, failed.items,
        (BinaryenIndex)failed.count, BinaryenTypeNone()) : NULL;
    deallocate(failed.items);
    BinaryenExpressionRef value = represented_binary(function, instruction, plan);
    BinaryenExpressionRef pending_value = represented_binary(function, instruction, plan);
    if (!ok || value == NULL || pending_value == NULL) return false;
    BinaryenExpressionRef items[] = {
        BinaryenLocalSet(function->module, (BinaryenIndex)function->scratch_base, value),
        BinaryenIf(function->module, BinaryenGlobalGet(function->module, P43_CODE,
            BinaryenTypeInt32()), failure, NULL),
        BinaryenLocalSet(function->module, (BinaryenIndex)destination,
            BinaryenLocalGet(function->module, (BinaryenIndex)function->scratch_base, BinaryenTypeInt64())),
    };
    /* Only a packet-free helper invocation can have a new local failure. */
    return represented_nodes_push(nodes, BinaryenIf(function->module,
        BinaryenUnary(function->module, BinaryenEqZInt32(), BinaryenGlobalGet(function->module,
            P43_CODE, BinaryenTypeInt32())), BinaryenBlock(function->module, NULL, items, 3,
                BinaryenTypeNone()), BinaryenLocalSet(function->module, (BinaryenIndex)destination,
                    pending_value)));
}

static bool represented_instruction_emit(const RepresentedFunction *function, size_t instruction,
    RepresentedNodes *nodes) {
    const SolMirConcreteProgram *concrete = function->request->program->conventions->concrete;
    const SolMirMaterialization *materialization = &concrete->materialization;
    const SolMirMaterializedInstruction *item = &materialization->instructions[instruction];
    const SolMirRuntimeLoweredImageInstruction *row =
        &function->request->program->image_instructions[instruction];
    if (row->state == SOL_MIR_RUNTIME_LOWERED_PRESENT
        && !represented_image_step_emit(function, row->step_failure_site, row->step_cleanup_event,
            nodes)) return false;
    size_t destination = value_index(function, item->result);
    size_t local, init, temporary;
    switch (item->kind) {
        case SOL_MIR_INST_PATTERN_TEST:
            return represented_pattern_test_emit(function, instruction, nodes);
        case SOL_MIR_INST_PATTERN_VALUE:
            return represented_pattern_value_emit(function, instruction, nodes);
        case SOL_MIR_INST_MATCH_ARM:
            return item->source_arm < concrete->program.ir->arm_count;
        case SOL_MIR_INST_CONSTRUCT: {
            const SolMirOperationConstructPlan *plan = constructor_for_instruction(function->request,
                instruction);
            if (plan == NULL) return false;
            if (plan->kind == SOL_MIR_OPERATION_CONSTRUCT_RECORD
                || plan->kind == SOL_MIR_OPERATION_CONSTRUCT_TUPLE
                || plan->kind == SOL_MIR_OPERATION_CONSTRUCT_SUM)
                return represented_fixed_product_emit(function, instruction, plan, destination, nodes);
            return plan->kind == SOL_MIR_OPERATION_CONSTRUCT_WRAPPER
                && represented_wrapper_emit(function, instruction, plan, destination, nodes);
        }
        case SOL_MIR_INST_CONST_INT64:
            return destination != SIZE_MAX && represented_nodes_push(nodes, BinaryenLocalSet(function->module,
                (BinaryenIndex)destination, BinaryenConst(function->module,
                    BinaryenLiteralInt64(item->integer))));
        case SOL_MIR_INST_CONST_BOOL:
            return destination != SIZE_MAX && represented_nodes_push(nodes, BinaryenLocalSet(function->module,
                (BinaryenIndex)destination, BinaryenConst(function->module,
                    BinaryenLiteralInt64(item->boolean ? 1 : 0))));
        case SOL_MIR_INST_CONST_TEXT: {
            const RepresentedLiteral *literal = represented_literal_for(function, instruction);
            return literal != NULL && represented_text_allocate_emit(function, instruction,
                BinaryenConst(function->module, BinaryenLiteralInt64((int64_t)literal->handle)),
                destination, nodes);
        }
        case SOL_MIR_INST_FUNCTION_VALUE:
            return represented_function_value_emit(function, instruction, destination, nodes);
        case SOL_MIR_INST_CONST_UNIT:
            return destination != SIZE_MAX && represented_nodes_push(nodes, BinaryenLocalSet(function->module,
                (BinaryenIndex)destination, BinaryenConst(function->module, BinaryenLiteralInt64(0))));
        case SOL_MIR_INST_PARAMETER_LIVE:
            local = local_index(function, item->local); init = local_init_index(function, item->local);
            if (local == SIZE_MAX || init == SIZE_MAX || item->local >= materialization->local_count
                || (materialization->locals[item->local].kind != SOL_MIR_MATERIALIZED_LOCAL_PARAMETER
                    && materialization->locals[item->local].kind != SOL_MIR_MATERIALIZED_LOCAL_RECEIVER)
                || materialization->locals[item->local].ordinal >= function->parameter_count) return false;
            return represented_nodes_push(nodes, BinaryenLocalSet(function->module, (BinaryenIndex)local,
                    BinaryenLocalGet(function->module, (BinaryenIndex)materialization->locals[item->local].ordinal,
                        BinaryenTypeInt64()))) && represented_nodes_push(nodes, BinaryenLocalSet(function->module,
                    (BinaryenIndex)init, BinaryenConst(function->module, BinaryenLiteralInt32(1))));
        case SOL_MIR_INST_STORAGE_LIVE:
        case SOL_MIR_INST_STORAGE_DEAD:
            init = local_init_index(function, item->local);
            local = local_hole_index(function, item->local);
            return init != SIZE_MAX && (local == SIZE_MAX || represented_nodes_push(nodes,
                    BinaryenLocalSet(function->module, (BinaryenIndex)local, BinaryenConst(
                        function->module, BinaryenLiteralInt32(0))))) && represented_nodes_push(nodes, BinaryenLocalSet(function->module,
                (BinaryenIndex)init, BinaryenConst(function->module, BinaryenLiteralInt32(0))));
        case SOL_MIR_INST_DROP_IF_INITIALIZED: {
            const SolMirRuntimeCleanupAction *action =
                NULL;
            RepresentedCleanupMarkerRoute route = represented_cleanup_instruction_route(function,
                instruction, &action);
            if (route == REPRESENTED_CLEANUP_MARKER_EVENTLESS) {
                if (represented_eventless_old_root_marker(function, instruction)) return true;
                init = local_init_index(function, item->local);
                local = local_hole_index(function, item->local);
                return init != SIZE_MAX && (local == SIZE_MAX || represented_nodes_push(nodes,
                    BinaryenLocalSet(function->module, (BinaryenIndex)local, BinaryenConst(
                        function->module, BinaryenLiteralInt32(0))))) && represented_nodes_push(nodes,
                    BinaryenLocalSet(function->module, (BinaryenIndex)init, BinaryenConst(
                        function->module, BinaryenLiteralInt32(0))));
            }
            if (route != REPRESENTED_CLEANUP_MARKER_ACTION) return false;
            RepresentedCleanupTraceContext trace;
            if (!represented_cleanup_marker_trace_context(function, instruction, &trace)) return false;
            local = local_hole_index(function, item->local);
            if (local != SIZE_MAX || represented_moved_callable_cleanup_action(function, action))
                return represented_cleanup_emit_traced(function, action, &trace, nodes);
            return represented_cleanup_emit_traced(function, action, &trace, nodes);
        }
        case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED:
            if (item->place >= materialization->place_count) return false;
            {
                const SolMirRuntimeCleanupAction *action =
                    NULL;
                RepresentedCleanupMarkerRoute route = represented_cleanup_instruction_route(function,
                    instruction, &action);
                if (route == REPRESENTED_CLEANUP_MARKER_EVENTLESS) {
                    init = local_init_index(function, materialization->places[item->place].local);
                    return init != SIZE_MAX && represented_nodes_push(nodes, BinaryenLocalSet(function->module,
                        (BinaryenIndex)init, BinaryenConst(function->module, BinaryenLiteralInt32(0))));
                }
                if (route != REPRESENTED_CLEANUP_MARKER_ACTION) return false;
                RepresentedCleanupTraceContext trace;
                if (!represented_cleanup_marker_trace_context(function, instruction, &trace)) return false;
                if (materialization->places[item->place].projections.count != 0)
                    return represented_projected_pre_store_action(function->request, function->image,
                        instruction, action) && (!represented_cleanup_trace_enabled() || represented_nodes_push(nodes,
                            represented_cleanup_trace_emit(function, action, &trace, false)));
                local = local_hole_index(function, materialization->places[item->place].local);
                if (local != SIZE_MAX || represented_moved_callable_cleanup_action(function, action))
                    return represented_cleanup_emit_traced(function, action, &trace, nodes);
                return represented_cleanup_emit_traced(function, action, &trace, nodes);
            }
        case SOL_MIR_INST_LOAD_COPY:
        case SOL_MIR_INST_LOAD_MOVE:
        case SOL_MIR_INST_LOAD_UPDATE:
            local = item->place < materialization->place_count
                ? local_index(function, materialization->places[item->place].local) : SIZE_MAX;
            if (destination == SIZE_MAX || local == SIZE_MAX) return false;
            if (materialization->places[item->place].projections.count != 0) {
                const SolMirMaterializedPlace *place = &materialization->places[item->place];
                SolMirRecipeId recipe = place->final_type < concrete->layout.type_count
                    ? concrete->layout.types[place->final_type].recipe : SOL_MIR_RECIPE_NONE;
                BinaryenExpressionRef value = represented_projected_load(function, place);
                if (value == NULL) return false;
                if (item->kind == SOL_MIR_INST_LOAD_MOVE) {
                    init = local_hole_index(function, place->local);
                    return represented_unbound_function_recipe(concrete, recipe) && init != SIZE_MAX
                        && represented_nodes_push(nodes, BinaryenLocalSet(function->module,
                            (BinaryenIndex)destination, value))
                        && represented_nodes_push(nodes, BinaryenLocalSet(function->module,
                            (BinaryenIndex)init, BinaryenConst(function->module,
                                BinaryenLiteralInt32(1))));
                }
                if (item->kind != SOL_MIR_INST_LOAD_COPY) return false;
                if (recipe < concrete->representation.recipe_count && represented_text_recipe(concrete, recipe))
                    return represented_text_allocate_emit(function, instruction, value, destination, nodes);
                SolMirRecipeId physical;
                if (represented_backing_recipe(concrete, recipe, &physical)
                    && represented_scalar_product_recipe(concrete, physical))
                    return represented_product_copy_emit(function, instruction, physical, value, destination, nodes);
                if (represented_backing_recipe(concrete, recipe, &physical)
                    && represented_sum_recipe_depth(concrete, physical, 0))
                    return represented_sum_copy_emit(function, instruction, physical, value, destination, nodes);
                return represented_nodes_push(nodes, BinaryenLocalSet(function->module,
                    (BinaryenIndex)destination, value));
            }
            if (item->kind == SOL_MIR_INST_LOAD_COPY
                && item->place < materialization->place_count
                && represented_recipe(concrete, materialization->places[item->place].final_type, false)
                        && represented_text_recipe(concrete, concrete->layout.types[
                    materialization->places[item->place].final_type].recipe)) {
                if (!represented_text_allocate_emit(function, instruction,
                        BinaryenLocalGet(function->module, (BinaryenIndex)local, BinaryenTypeInt64()),
                        destination, nodes)) return false;
            } else if (item->kind == SOL_MIR_INST_LOAD_COPY && item->place < materialization->place_count
                && materialization->places[item->place].final_type < concrete->layout.type_count
                && represented_product_recipe(concrete, concrete->layout.types[
                    materialization->places[item->place].final_type].recipe)) {
                SolMirRecipeId physical;
                if (!represented_backing_recipe(concrete, concrete->layout.types[
                        materialization->places[item->place].final_type].recipe, &physical)
                    || !represented_product_copy_emit(function, instruction, physical,
                        BinaryenLocalGet(function->module, (BinaryenIndex)local, BinaryenTypeInt64()),
                        destination, nodes)) return false;
            } else if (item->kind == SOL_MIR_INST_LOAD_COPY && item->place < materialization->place_count
                && materialization->places[item->place].final_type < concrete->layout.type_count
                && represented_sum_recipe(concrete, concrete->layout.types[
                    materialization->places[item->place].final_type].recipe)) {
                SolMirRecipeId physical;
                if (!represented_backing_recipe(concrete, concrete->layout.types[
                        materialization->places[item->place].final_type].recipe, &physical)
                    || !represented_sum_copy_emit(function, instruction, physical,
                        BinaryenLocalGet(function->module, (BinaryenIndex)local, BinaryenTypeInt64()),
                        destination, nodes)) return false;
            } else if (!represented_nodes_push(nodes, BinaryenLocalSet(function->module,
                    (BinaryenIndex)destination, BinaryenLocalGet(function->module,
                    (BinaryenIndex)local, BinaryenTypeInt64())))) return false;
            if (item->kind == SOL_MIR_INST_LOAD_MOVE) {
                init = local_init_index(function, materialization->places[item->place].local);
                local = local_hole_index(function, materialization->places[item->place].local);
                return init != SIZE_MAX && (local == SIZE_MAX || represented_nodes_push(nodes,
                    BinaryenLocalSet(function->module, (BinaryenIndex)local, BinaryenConst(function->module,
                        BinaryenLiteralInt32(0))))) && represented_nodes_push(nodes, BinaryenLocalSet(
                            function->module, (BinaryenIndex)init, BinaryenConst(function->module,
                                BinaryenLiteralInt32(0))));
            }
            return true;
        case SOL_MIR_INST_STORE:
            if (item->place < materialization->place_count
                && materialization->places[item->place].projections.count != 0) {
                const SolMirFieldLayout *field = NULL;
                local = local_index(function, materialization->places[item->place].local);
                init = local_hole_index(function, materialization->places[item->place].local);
                if (local == SIZE_MAX || init == SIZE_MAX || !represented_callable_field_repair(
                        function->request, function->image, instruction, &field) || field == NULL)
                    return false;
                BinaryenExpressionRef value = get_value(function, item->left);
                return value != NULL && represented_nodes_push(nodes, BinaryenStore(function->module, 4,
                    (uint32_t)field->offset, 4, wrap_i64(function->module,
                        BinaryenLocalGet(function->module, (BinaryenIndex)local, BinaryenTypeInt64())),
                    wrap_i64(function->module, value), BinaryenTypeInt32(), P43_MEMORY))
                    && represented_nodes_push(nodes, BinaryenLocalSet(function->module,
                        (BinaryenIndex)init, BinaryenConst(function->module,
                            BinaryenLiteralInt32(0))));
            }
            local = item->place < materialization->place_count
                ? local_index(function, materialization->places[item->place].local) : SIZE_MAX;
            init = item->place < materialization->place_count
                ? local_init_index(function, materialization->places[item->place].local) : SIZE_MAX;
            return local != SIZE_MAX && init != SIZE_MAX && represented_nodes_push(nodes,
                BinaryenLocalSet(function->module, (BinaryenIndex)local, get_value(function, item->left)))
                && represented_nodes_push(nodes, BinaryenLocalSet(function->module, (BinaryenIndex)init,
                    BinaryenConst(function->module, BinaryenLiteralInt32(1))))
                && (local_hole_index(function, materialization->places[item->place].local) == SIZE_MAX
                    || represented_nodes_push(nodes, BinaryenLocalSet(function->module, (BinaryenIndex)
                        local_hole_index(function, materialization->places[item->place].local), BinaryenConst(
                            function->module, BinaryenLiteralInt32(0)))));
        case SOL_MIR_INST_TEMPORARY_INIT:
            temporary = temporary_index(function, item->temporary);
            init = temporary_init_index(function, item->temporary);
            return temporary != SIZE_MAX && init != SIZE_MAX && represented_nodes_push(nodes,
                BinaryenLocalSet(function->module, (BinaryenIndex)temporary, get_value(function, item->left)))
                && represented_nodes_push(nodes, BinaryenLocalSet(function->module, (BinaryenIndex)init,
                    BinaryenConst(function->module, BinaryenLiteralInt32(1))));
        case SOL_MIR_INST_TEMPORARY_DROP:
            {
                const SolMirRuntimeCleanupAction *action =
                    NULL;
                RepresentedCleanupMarkerRoute route = represented_cleanup_instruction_route(function,
                    instruction, &action);
                init = temporary_init_index(function, item->temporary);
                if (route == REPRESENTED_CLEANUP_MARKER_EVENTLESS)
                    return init != SIZE_MAX && represented_nodes_push(nodes, BinaryenLocalSet(function->module,
                        (BinaryenIndex)init, BinaryenConst(function->module, BinaryenLiteralInt32(0))));
                if (route != REPRESENTED_CLEANUP_MARKER_ACTION || init == SIZE_MAX) return false;
                RepresentedCleanupTraceContext trace;
                if (!represented_cleanup_marker_trace_context(function, instruction, &trace)) return false;
                if (represented_moved_callable_temporary(function, item->temporary)) {
                    if (!represented_cleanup_trace_enabled()) {
                        BinaryenExpressionRef body[] = {
                            represented_cleanup_probe_increment(function->module,
                                P43_CLEANUP_MOVED_CALLABLE), BinaryenLocalSet(function->module,
                                (BinaryenIndex)init, BinaryenConst(function->module,
                                    BinaryenLiteralInt32(0))),
                        };
                        return body[0] != NULL && body[1] != NULL && represented_nodes_push(nodes,
                            BinaryenIf(function->module, BinaryenLocalGet(function->module,
                                (BinaryenIndex)init, BinaryenTypeInt32()), BinaryenBlock(function->module,
                                    NULL, body, 2, BinaryenTypeNone()), NULL));
                    }
                    BinaryenExpressionRef body[] = {
                        represented_cleanup_trace_emit(function, action, &trace, true),
                        represented_cleanup_probe_increment(function->module,
                            P43_CLEANUP_MOVED_CALLABLE),
                        BinaryenLocalSet(function->module, (BinaryenIndex)init,
                            BinaryenConst(function->module, BinaryenLiteralInt32(0))),
                    };
                    return body[0] != NULL && body[1] != NULL && body[2] != NULL && represented_nodes_push(nodes,
                        BinaryenIf(function->module, BinaryenLocalGet(function->module,
                            (BinaryenIndex)init, BinaryenTypeInt32()), BinaryenBlock(function->module, NULL,
                                body, 3, BinaryenTypeNone()), represented_cleanup_trace_emit(function,
                                    action, &trace, false)));
                }
                return represented_cleanup_emit_traced(function, action, &trace, nodes);
            }
        case SOL_MIR_INST_EXPRESSION_RESULT:
            return destination != SIZE_MAX && represented_nodes_push(nodes, BinaryenLocalSet(function->module,
                (BinaryenIndex)destination, get_value(function, item->left)));
        case SOL_MIR_INST_UNARY:
        case SOL_MIR_INST_BINARY:
        case SOL_MIR_INST_COMPOUND_UPDATE: {
            const SolMirOperationArithmeticPlan *plan = arithmetic_for_instruction(
                function->request, instruction);
            bool ok;
            if (plan == NULL || destination == SIZE_MAX) return false;
            if (represented_checked_opcode(plan)) ok = represented_checked_emit(function, instruction,
                plan, destination, nodes);
            else {
                SolMirRecipeId physical;
                if ((plan->opcode == SOL_MIR_OPERATION_VALUE_EQ || plan->opcode == SOL_MIR_OPERATION_VALUE_NE)
                    && represented_backing_recipe(concrete, plan->operand_recipe, &physical)
                    && represented_indirect_recipe(concrete, physical)) {
                    ok = represented_equality_emit(function, instruction, plan, destination, nodes);
                } else {
                    BinaryenExpressionRef value = represented_binary(function, instruction, plan);
                    ok = value != NULL && represented_nodes_push(nodes, BinaryenLocalSet(function->module,
                        (BinaryenIndex)destination, value));
                }
            }
            if (ok && plan->compound) {
                init = temporary_init_index(function, plan->previous);
                ok = init != SIZE_MAX && represented_nodes_push(nodes, BinaryenLocalSet(function->module,
                    (BinaryenIndex)init, BinaryenConst(function->module, BinaryenLiteralInt32(0))));
            }
            return ok;
        }
        case SOL_MIR_INST_REGION_EXIT:
        case SOL_MIR_INST_SCOPE_EXIT: {
            const SolMirRuntimeCleanupAction *action =
                NULL;
            RepresentedCleanupMarkerRoute route = represented_cleanup_instruction_route(function,
                instruction, &action);
            RepresentedCleanupTraceContext trace;
            return route == REPRESENTED_CLEANUP_MARKER_ACTION
                && represented_cleanup_marker_trace_context(function, instruction, &trace)
                && (!represented_cleanup_trace_enabled() || represented_nodes_push(nodes,
                    represented_cleanup_trace_emit(function, action, &trace, true)));
        }
        case SOL_MIR_INST_REGION_ENTER:
        case SOL_MIR_INST_SCOPE_ENTER:
            return true;
        case SOL_MIR_INST_CAPTURE_SNAPSHOT: {
            const SolMirOperationSnapshotPlan *snapshot = represented_snapshot_for_instruction(
                function->request, instruction, NULL);
            local = snapshot == NULL ? SIZE_MAX : local_index(function, snapshot->local);
            size_t slot = snapshot == NULL ? SIZE_MAX
                : snapshot_value_index(function, snapshot->slot);
            init = snapshot == NULL ? SIZE_MAX : snapshot_init_index(function, snapshot->slot);
            return local != SIZE_MAX && slot != SIZE_MAX && init != SIZE_MAX
                && represented_nodes_push(nodes, BinaryenLocalSet(function->module,
                    (BinaryenIndex)slot, BinaryenLocalGet(function->module,
                        (BinaryenIndex)local, BinaryenTypeInt64())))
                && represented_nodes_push(nodes, BinaryenLocalSet(function->module,
                    (BinaryenIndex)init, BinaryenConst(function->module,
                        BinaryenLiteralInt32(1))));
        }
        default:
            return false;
    }
}

static BinaryenExpressionRef represented_call_failure_emit(const RepresentedFunction *function,
    const RepresentedCallCatalog *call, const SolMirRuntimeCleanupEvent *event,
    const SolMirRuntimeCleanupTransition *failure, bool pending) {
    const SolMirRuntimeCleanup *cleanup = function->request->program->cleanup;
    RepresentedCleanupTraceContext trace = {event, failure, pending};
    RepresentedNodes failed = {0};
    bool ok = true;
    for (size_t i = 0; ok && i < failure->actions.count; ++i) {
        const SolMirRuntimeCleanupAction *action = &cleanup->actions[failure->actions.offset + i];
        ok = action->kind != SOL_MIR_RUNTIME_CLEANUP_ACTION_WRITEBACK
            && represented_cleanup_emit_traced(function, action, &trace, &failed);
    }
    if (ok) ok = represented_edge(function, call->failure_edge, &failed);
    BinaryenExpressionRef body = ok ? BinaryenBlock(function->module, NULL, failed.items,
        (BinaryenIndex)failed.count, BinaryenTypeNone()) : NULL;
    deallocate(failed.items);
    return body;
}

static bool represented_call_emit(const RepresentedFunction *function, size_t block,
    RepresentedNodes *nodes) {
    const SolMirRuntimeLoweredProgram *owner = function->request->program;
    const SolMirRuntimeConventions *conventions = owner->conventions;
    const SolMirMaterialization *m = &conventions->concrete->materialization;
    const RepresentedCallCatalog *call = represented_catalog_for(function->catalog, function->image_id, block);
    if (call == NULL || call->callee_callable >= conventions->concrete->linkage.callable_count)
        return false;
    const SolMirMaterializedTerminator *term = &m->blocks[block].terminator;
    BinaryenExpressionRef *arguments = call->operands.count == 0 ? NULL
        : allocate(call->operands.count, sizeof *arguments);
    if (call->operands.count != 0 && arguments == NULL) return false;
    for (size_t i = 0; i < call->operands.count; ++i) {
        const SolMirRuntimeOperand *operand = &conventions->operands[call->operands.offset + i];
        if (operand->value.kind == SOL_MIR_RUNTIME_VALUE_MATERIALIZED_TEMPORARY)
            arguments[i] = temporary_get(function, operand->value.id);
        else if (operand->value.kind == SOL_MIR_RUNTIME_VALUE_MATERIALIZED_PLACE
            && operand->value.id < m->place_count && m->places[operand->value.id].projections.count == 0) {
            size_t local = local_index(function, m->places[operand->value.id].local);
            arguments[i] = local == SIZE_MAX ? NULL : BinaryenLocalGet(function->module,
                (BinaryenIndex)local, BinaryenTypeInt64());
        } else arguments[i] = NULL;
        if (arguments[i] == NULL) { deallocate(arguments); return false; }
    }
    const char *symbol = conventions->concrete->linkage.callables[call->callee_callable].symbol.bytes;
    BinaryenExpressionRef invoke = NULL;
    const RepresentedCallbackCatalog *callback = represented_callback_for(function->catalog, call->call);
    bool receiver_writeback = call->call < conventions->call_count
        && conventions->calls[call->call].writebacks.count == 1;
    if (callback != NULL) {
        const SolMirConcreteProgram *concrete = conventions->concrete;
        const SolMirMaterializedTemporary *callee = call->caller_block < m->block_count
            && m->blocks[call->caller_block].terminator.callee < m->temporary_count
            ? &m->temporaries[m->blocks[call->caller_block].terminator.callee] : NULL;
        SolMirRecipeId recipe = callee == NULL || callee->type >= concrete->layout.type_count
            ? SOL_MIR_RECIPE_NONE : concrete->layout.types[callee->type].recipe;
        if (callback->table == SOL_MIR_LINKAGE_NONE || callback->table >= concrete->linkage.table_entry_count
            || callback->table >= UINT32_MAX || !represented_unbound_function_recipe(concrete, recipe)) {
            deallocate(arguments); return false;
        }
        BinaryenExpressionRef handle = temporary_get(function,
            m->blocks[call->caller_block].terminator.callee);
        BinaryenExpressionRef token = handle == NULL ? NULL : BinaryenLoad(function->module, 4, false, 0,
            4, BinaryenTypeInt32(), wrap_i64(function->module, handle), P43_MEMORY);
        if (token == NULL || !represented_nodes_push(nodes, BinaryenIf(function->module,
                BinaryenBinary(function->module, BinaryenNeInt32(), token, BinaryenConst(function->module,
                    BinaryenLiteralInt32((int32_t)(callback->table + 1)))), BinaryenReturn(function->module,
                    i64(function, 0)), NULL))) { deallocate(arguments); return false; }
        BinaryenType *parameter_types = call->operands.count == 0 ? NULL
            : allocate(call->operands.count, sizeof *parameter_types);
        if (call->operands.count != 0 && parameter_types == NULL) { deallocate(arguments); return false; }
        for (size_t i = 0; i < call->operands.count; ++i) parameter_types[i] = BinaryenTypeInt64();
        BinaryenType parameters = call->operands.count == 0 ? BinaryenTypeNone()
            : BinaryenTypeCreate(parameter_types, (BinaryenIndex)call->operands.count);
        deallocate(parameter_types);
        invoke = BinaryenCallIndirect(function->module, P43_TABLE,
            BinaryenLoad(function->module, 4, false, 0, 4, BinaryenTypeInt32(),
                wrap_i64(function->module, temporary_get(function,
                    m->blocks[call->caller_block].terminator.callee)), P43_MEMORY), arguments,
            (BinaryenIndex)call->operands.count, parameters, BinaryenTypeInt64());
    } else {
        invoke = BinaryenCall(function->module, symbol, arguments,
            (BinaryenIndex)call->operands.count, BinaryenTypeInt64());
    }
    deallocate(arguments);
    if (invoke == NULL) return false;
    /* A callback consumes its callee temporary before invocation.  This is the
     * normal-path counterpart to a post-move failure's DROP_PLACE action. */
    if (callback != NULL && represented_moved_callable_temporary(function, term->callee)) {
        size_t init = temporary_init_index(function, term->callee);
        BinaryenExpressionRef body[] = {
            represented_cleanup_probe_increment(function->module, P43_CLEANUP_MOVED_CALLABLE),
            init == SIZE_MAX ? NULL : BinaryenLocalSet(function->module, (BinaryenIndex)init,
                BinaryenConst(function->module, BinaryenLiteralInt32(0))),
        };
        if (init == SIZE_MAX || body[0] == NULL || body[1] == NULL || !represented_nodes_push(nodes,
                BinaryenIf(function->module, BinaryenLocalGet(function->module, (BinaryenIndex)init,
                    BinaryenTypeInt32()), BinaryenBlock(function->module, NULL, body, 2,
                        BinaryenTypeNone()), NULL))) return false;
    }
    const SolMirRuntimeLoweredImageTerminator *row = &owner->image_terminators[block];
    if (row->cleanup_event >= owner->cleanup->event_count) return false;
    const SolMirRuntimeCleanupEvent *event = &owner->cleanup->events[row->cleanup_event];
    const SolMirRuntimeCleanupTransition *normal = NULL, *failure = NULL;
    for (size_t i = 0; i < event->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *candidate = &owner->cleanup->transitions[
            event->transitions.offset + i];
        if (candidate->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_NORMAL) {
            if (normal != NULL || candidate->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL
                || candidate->continuation != term->normal_edge
                || candidate->actions.offset > owner->cleanup->action_count
                || candidate->actions.count > owner->cleanup->action_count - candidate->actions.offset)
                return false;
            normal = candidate;
        } else if (candidate->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE) {
            if (failure != NULL || candidate->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
                || candidate->continuation != term->failure_edge
                || candidate->actions.offset > owner->cleanup->action_count
                || candidate->actions.count > owner->cleanup->action_count - candidate->actions.offset)
                return false;
            failure = candidate;
        }
    }
    if (normal == NULL || failure == NULL) return false;
    RepresentedCleanupTraceContext normal_trace = {event, normal, false};
    RepresentedNodes success = {0};
    bool ok = true;
    if (call->result_class == SOL_MIR_RUNTIME_RESULT_VALUE) {
        size_t result = value_index(function, call->result);
        ok = result != SIZE_MAX && represented_nodes_push(&success, BinaryenLocalSet(function->module,
            (BinaryenIndex)result, BinaryenLocalGet(function->module,
                (BinaryenIndex)function->scratch_base, BinaryenTypeInt64())));
    }
    for (size_t i = 0; ok && i < normal->actions.count; ++i) {
        const SolMirRuntimeCleanupAction *action = &owner->cleanup->actions[normal->actions.offset + i];
        ok = action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_WRITEBACK
            ? receiver_writeback && represented_receiver_writeback_emit(function, call, action,
                &normal_trace, &success)
            : represented_cleanup_emit_traced(function, action, &normal_trace, &success);
    }
    if (ok) ok = represented_edge(function, call->normal_edge, &success);
    BinaryenExpressionRef success_body = ok ? BinaryenBlock(function->module, NULL, success.items,
        (BinaryenIndex)success.count, BinaryenTypeNone()) : NULL;
    deallocate(success.items);
    if (!ok) return false;
    BinaryenExpressionRef failed = represented_call_failure_emit(function, call, event, failure, true);
    size_t record = 0;
    if (failed == NULL
        || !represented_failure_record_index(function->provenance, call->failure_site, &record)) return false;
    BinaryenExpressionRef local_items[] = {
        BinaryenGlobalSet(function->module, P43_CODE, BinaryenConst(function->module,
            BinaryenLiteralInt32(SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT))),
        BinaryenGlobalSet(function->module, P43_SITE, BinaryenConst(function->module,
            BinaryenLiteralInt32((int32_t)record))),
        BinaryenLocalSet(function->module, (BinaryenIndex)function->scratch_base, i64(function, 0)),
    };
    BinaryenExpressionRef local_failure = BinaryenBlock(function->module, NULL, local_items, 3,
        BinaryenTypeNone());
    /* Only the actual source invocation owns this increment. The staged raw
     * result is never published until the callee packet has been inspected. */
    BinaryenExpressionRef invoked[] = {
        BinaryenGlobalSet(function->module, P44_CALL_DEPTH, BinaryenBinary(function->module,
            BinaryenAddInt32(), BinaryenGlobalGet(function->module, P44_CALL_DEPTH,
                BinaryenTypeInt32()), BinaryenConst(function->module, BinaryenLiteralInt32(1)))),
        call->result_class == SOL_MIR_RUNTIME_RESULT_VALUE || receiver_writeback
            ? BinaryenLocalSet(function->module, (BinaryenIndex)function->scratch_base, invoke)
            : BinaryenDrop(function->module, invoke),
        BinaryenGlobalSet(function->module, P44_CALL_DEPTH, BinaryenBinary(function->module,
            BinaryenSubInt32(), BinaryenGlobalGet(function->module, P44_CALL_DEPTH,
                BinaryenTypeInt32()), BinaryenConst(function->module, BinaryenLiteralInt32(1)))),
        BinaryenIf(function->module, BinaryenGlobalGet(function->module, P43_CODE,
            BinaryenTypeInt32()), BinaryenLocalSet(function->module,
                (BinaryenIndex)function->scratch_base, i64(function, 1)), NULL),
    };
    for (size_t i = 0; i < sizeof invoked / sizeof *invoked; ++i)
        if (invoked[i] == NULL) return false;
    BinaryenExpressionRef allowed = BinaryenBlock(function->module, NULL, invoked,
        (BinaryenIndex)(sizeof invoked / sizeof *invoked), BinaryenTypeNone());
    BinaryenExpressionRef checked = BinaryenIf(function->module,
        BinaryenBinary(function->module, BinaryenGeUInt32(), BinaryenGlobalGet(function->module,
            P44_CALL_DEPTH, BinaryenTypeInt32()), BinaryenGlobalGet(function->module,
                P44_MAX_CALL_DEPTH, BinaryenTypeInt32())), local_failure, allowed);
    /* This explicit flag describes the route, not the published packet: local
     * code 7 is NOT pending at its originating call. Both routes use exactly
     * the same authenticated P3.3 transition and action slice. */
    return represented_nodes_push(nodes, BinaryenIf(function->module,
        BinaryenGlobalGet(function->module, P43_CODE, BinaryenTypeInt32()),
        BinaryenLocalSet(function->module, (BinaryenIndex)function->scratch_base, i64(function, 1)),
        checked)) && represented_nodes_push(nodes, BinaryenIf(function->module,
            BinaryenGlobalGet(function->module, P43_CODE, BinaryenTypeInt32()), failed, success_body));
}

static bool represented_resume_failure_emit(const RepresentedFunction *function, size_t block,
    RepresentedNodes *nodes) {
    const SolMirRuntimeLoweredProgram *owner = function->request->program;
    if (block >= owner->image_terminator_count) return false;
    const SolMirRuntimeLoweredImageTerminator *row = &owner->image_terminators[block];
    if (row->cleanup_event >= owner->cleanup->event_count) return false;
    const SolMirRuntimeCleanupEvent *event = &owner->cleanup->events[row->cleanup_event];
    const SolMirRuntimeCleanupTransition *pending = NULL;
    for (size_t i = 0; i < event->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *candidate = &owner->cleanup->transitions[
            event->transitions.offset + i];
        if (candidate->edge_role != SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE) return false;
        if (pending != NULL || candidate->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
            || candidate->failure_source != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING
            || candidate->continuation != SOL_MIR_RUNTIME_NONE
            || candidate->actions.offset > owner->cleanup->action_count
            || candidate->actions.count > owner->cleanup->action_count - candidate->actions.offset)
            return false;
        pending = candidate;
    }
    if (pending == NULL) return false;
    if (!represented_cleanup_actions_emit_traced(function, event, pending, nodes)) return false;
    return represented_nodes_push(nodes, BinaryenReturn(function->module, i64(function, 0)));
}

static bool represented_refinement_emit(const RepresentedFunction *function, size_t block,
    RepresentedNodes *nodes) {
    RepresentedRefinementCheck check;
    if (!represented_refinement_check(function->request, function->qualified,
            function->image_id, block, &check))
        return false;
    const SolMirMaterializedTerminator *term = &function->request->program->conventions
        ->concrete->materialization.blocks[block].terminator;
    size_t result = value_index(function, term->result);
    size_t representation_init = temporary_init_index(function, term->representation);
    size_t record = 0;
    if (result == SIZE_MAX || representation_init == SIZE_MAX
        || !represented_failure_record_index(function->provenance, check.violation_site, &record)
        || !represented_predicate_emit(function, check.plan->body, nodes)) return false;

    RepresentedNodes yes = {0}, no = {0}, failed = {0};
    BinaryenExpressionRef backing = temporary_get(function, term->representation);
    bool ok = backing != NULL && represented_nodes_push(&yes, BinaryenLocalSet(function->module,
            (BinaryenIndex)result, backing))
        && represented_nodes_push(&yes, BinaryenLocalSet(function->module,
            (BinaryenIndex)representation_init,
            BinaryenConst(function->module, BinaryenLiteralInt32(0))))
        && (!represented_refinement_trace_enabled() || represented_nodes_push(&yes,
            represented_refinement_trace_emit(function, check.event, check.satisfied)))
        && represented_cleanup_actions_emit_traced(function, check.event, check.satisfied, &yes)
        && represented_edge(function, term->normal_edge, &yes)
        && represented_nodes_push(&no, BinaryenGlobalSet(function->module, P43_CODE,
            BinaryenConst(function->module, BinaryenLiteralInt32(
                SOL_MIR_RUNTIME_FAILURE_REFINEMENT_VIOLATION))))
        && represented_nodes_push(&no, BinaryenGlobalSet(function->module, P43_SITE,
            BinaryenConst(function->module, BinaryenLiteralInt32((int32_t)record))))
        && (!represented_refinement_trace_enabled() || represented_nodes_push(&no,
            represented_refinement_trace_emit(function, check.event, check.violation)))
        && represented_cleanup_actions_emit_traced(function, check.event, check.violation, &no)
        && represented_edge(function, term->failure_edge, &no)
        && (!represented_refinement_trace_enabled() || represented_nodes_push(&failed,
            represented_refinement_trace_emit(function, check.event, check.failure)))
        && represented_cleanup_actions_emit_traced(function, check.event, check.failure, &failed)
        && represented_edge(function, term->failure_edge, &failed);
    BinaryenExpressionRef y = ok ? BinaryenBlock(function->module, NULL, yes.items,
        (BinaryenIndex)yes.count, BinaryenTypeNone()) : NULL;
    BinaryenExpressionRef n = ok ? BinaryenBlock(function->module, NULL, no.items,
        (BinaryenIndex)no.count, BinaryenTypeNone()) : NULL;
    BinaryenExpressionRef z = ok ? BinaryenBlock(function->module, NULL, failed.items,
        (BinaryenIndex)failed.count, BinaryenTypeNone()) : NULL;
    deallocate(yes.items); deallocate(no.items); deallocate(failed.items);
    /* Predicate arithmetic publishes code 2/3 and exits its evaluator before
     * the Bool slot is observed.  This packet test must consequently be the
     * outer branch. */
    return ok && represented_nodes_push(nodes, BinaryenIf(function->module,
        BinaryenBinary(function->module, BinaryenNeInt32(),
            BinaryenGlobalGet(function->module, P43_CODE, BinaryenTypeInt32()),
            BinaryenConst(function->module, BinaryenLiteralInt32(0))), z,
        BinaryenIf(function->module, BinaryenBinary(function->module, BinaryenNeInt64(),
            BinaryenLocalGet(function->module, (BinaryenIndex)function->predicate_result,
                BinaryenTypeInt64()), i64(function, 0)), y, n)));
}

static bool represented_contract_emit(const RepresentedFunction *function, size_t block,
    RepresentedNodes *nodes) {
    const SolMirRuntimeLoweredProgram *owner = function->request->program;
    const SolMirConcreteProgram *concrete = owner->conventions->concrete;
    if (block >= owner->image_terminator_count) return false;
    const SolMirMaterializedTerminator *term = &concrete->materialization.blocks[block].terminator;
    const SolMirRuntimeLoweredImageTerminator *row = &owner->image_terminators[block];
    if (row->state != SOL_MIR_RUNTIME_LOWERED_PRESENT || row->image != function->image_id
        || row->block != block || row->kind != SOL_MIR_TERM_CHECK_CONTRACT
        || row->plan >= owner->semantic_plan_count) return false;
    const SolMirRuntimeLoweredSemanticPlan *semantic = &owner->semantic_plans[row->plan];
    if (semantic->state != SOL_MIR_RUNTIME_LOWERED_PRESENT
        || semantic->arena != SOL_MIR_RUNTIME_LOWERED_SEMANTIC_PREDICATE
        || semantic->plan >= concrete->operations.predicate_count) return false;
    const SolMirOperationPredicatePlan *plan = &concrete->operations.predicates[semantic->plan];
    if (plan->kind != SOL_MIR_OPERATION_PREDICATE_CONTRACT
        || plan->image != function->image_id || plan->block != block
        || semantic->producer_kind != SOL_MIR_MATERIALIZED_PRODUCER_PREDICATE
        || semantic->producer != plan->body
        || (plan->contract_phase != SOL_CONTRACT_REQUIRES
            && plan->contract_phase != SOL_CONTRACT_ENSURES)
        || (plan->contract_outcome != SOL_CONTRACT_OUTCOME_ALWAYS
            && (plan->contract_phase != SOL_CONTRACT_ENSURES
                || (plan->contract_outcome != SOL_CONTRACT_OUTCOME_SUCCESS
                    && plan->contract_outcome != SOL_CONTRACT_OUTCOME_FAILURE)))
        || plan->body >= concrete->operations.predicate_body_count) return false;
    bool qualified = plan->contract_outcome == SOL_CONTRACT_OUTCOME_SUCCESS
        || plan->contract_outcome == SOL_CONTRACT_OUTCOME_FAILURE;
    const RepresentedQualifiedImage *qualified_image = qualified
        ? represented_qualified_image(function->qualified, function->image_id,
            plan->result_recipe) : NULL;
    if (qualified && qualified_image == NULL) return false;
    if (row->cleanup_event >= owner->cleanup->event_count) return false;
    const SolMirRuntimeCleanupEvent *event = &owner->cleanup->events[row->cleanup_event];
    const SolMirRuntimeCleanupTransition *satisfied = NULL, *violation = NULL, *failure = NULL;
    for (size_t i = 0; i < event->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *candidate = &owner->cleanup->transitions[event->transitions.offset + i];
        if (candidate->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_SATISFIED) {
            if (satisfied != NULL) return false;
            satisfied = candidate;
        } else if (candidate->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_VIOLATION) {
            if (violation != NULL) return false;
            violation = candidate;
        } else if (candidate->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_FAILURE) {
            if (failure != NULL) return false;
            failure = candidate;
        }
        else return false;
    }
    SolMirRuntimeFailureCode violation_code = plan->contract_phase == SOL_CONTRACT_REQUIRES
        ? SOL_MIR_RUNTIME_FAILURE_REQUIRE_VIOLATION
        : SOL_MIR_RUNTIME_FAILURE_ENSURE_VIOLATION;
    uint32_t violation_mask = UINT32_C(1) << ((unsigned)violation_code - 1u);
    if (satisfied == NULL || violation == NULL || failure == NULL
        || satisfied->continuation != term->satisfied_edge || violation->continuation != term->violation_edge
        || failure->continuation != term->failure_edge
        || satisfied->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL
        || violation->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
        || failure->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
        || satisfied->contract_phase != plan->contract_phase
        || violation->contract_phase != plan->contract_phase
        || failure->contract_phase != plan->contract_phase
        || satisfied->contract_outcome != plan->contract_outcome
        || violation->contract_outcome != plan->contract_outcome
        || failure->contract_outcome != plan->contract_outcome
        || violation->failure_source != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31
        || violation->failure_site >= owner->conventions->failure_site_count
        || violation->failure_mask != violation_mask) return false;
    const SolMirRuntimeFailureSite *violation_site = &owner->conventions->failure_sites[
        violation->failure_site];
    if (violation_site->origin_kind != SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT
        || violation_site->owner != plan->body || violation_site->allowed_codes != violation_mask)
        return false;
    if (!qualified) {
        RepresentedNodes yes = {0}, no = {0}, failed = {0}; size_t record = 0;
        bool ok = represented_predicate_emit(function, plan->body, nodes)
            && represented_cleanup_actions_emit_traced(function, event, satisfied, &yes)
            && represented_edge(function, term->satisfied_edge, &yes)
            && represented_failure_record_index(function->provenance, violation->failure_site,
                &record)
            && represented_nodes_push(&no, BinaryenGlobalSet(function->module, P43_CODE,
                BinaryenConst(function->module, BinaryenLiteralInt32((int32_t)violation_code))))
            && represented_nodes_push(&no, BinaryenGlobalSet(function->module, P43_SITE,
                BinaryenConst(function->module, BinaryenLiteralInt32((int32_t)record))))
            && represented_cleanup_actions_emit_traced(function, event, violation, &no)
            && represented_edge(function, term->violation_edge, &no)
            && represented_cleanup_actions_emit_traced(function, event, failure, &failed)
            && represented_edge(function, term->failure_edge, &failed);
        BinaryenExpressionRef y = ok ? BinaryenBlock(function->module, NULL, yes.items,
            (BinaryenIndex)yes.count, BinaryenTypeNone()) : NULL;
        BinaryenExpressionRef n = ok ? BinaryenBlock(function->module, NULL, no.items,
            (BinaryenIndex)no.count, BinaryenTypeNone()) : NULL;
        BinaryenExpressionRef z = ok ? BinaryenBlock(function->module, NULL, failed.items,
            (BinaryenIndex)failed.count, BinaryenTypeNone()) : NULL;
        deallocate(yes.items); deallocate(no.items); deallocate(failed.items);
        return ok && represented_nodes_push(nodes, BinaryenIf(function->module, BinaryenBinary(
            function->module, BinaryenNeInt32(), BinaryenGlobalGet(function->module, P43_CODE,
                BinaryenTypeInt32()), BinaryenConst(function->module, BinaryenLiteralInt32(0))), z,
            BinaryenIf(function->module, BinaryenBinary(function->module, BinaryenNeInt64(),
                BinaryenLocalGet(function->module, (BinaryenIndex)function->predicate_result,
                    BinaryenTypeInt64()), i64(function, 0)), y, n)));
    }
    RepresentedNodes yes = {0}, no = {0}, failed = {0}, evaluated = {0}, skipped = {0};
    size_t record = 0;
    bool ok = represented_cleanup_actions_emit_traced(function, event, satisfied, &yes)
        && represented_edge(function, term->satisfied_edge, &yes)
        && represented_failure_record_index(function->provenance, violation->failure_site, &record)
        && represented_nodes_push(&no, BinaryenGlobalSet(function->module, P43_CODE,
            BinaryenConst(function->module, BinaryenLiteralInt32((int32_t)violation_code))))
        && represented_nodes_push(&no, BinaryenGlobalSet(function->module, P43_SITE,
            BinaryenConst(function->module, BinaryenLiteralInt32((int32_t)record))))
        && represented_cleanup_actions_emit_traced(function, event, violation, &no)
        && represented_edge(function, term->violation_edge, &no)
        && represented_cleanup_actions_emit_traced(function, event, failure, &failed)
        && represented_edge(function, term->failure_edge, &failed);
    BinaryenExpressionRef y = ok ? BinaryenBlock(function->module, NULL, yes.items,
        (BinaryenIndex)yes.count, BinaryenTypeNone()) : NULL;
    BinaryenExpressionRef n = ok ? BinaryenBlock(function->module, NULL, no.items,
        (BinaryenIndex)no.count, BinaryenTypeNone()) : NULL;
    BinaryenExpressionRef z = ok ? BinaryenBlock(function->module, NULL, failed.items,
        (BinaryenIndex)failed.count, BinaryenTypeNone()) : NULL;
    if (ok) ok = represented_predicate_emit(function, plan->body, &evaluated)
        && represented_nodes_push(&evaluated, BinaryenIf(function->module, BinaryenBinary(
            function->module, BinaryenNeInt32(), BinaryenGlobalGet(function->module, P43_CODE,
                BinaryenTypeInt32()), BinaryenConst(function->module, BinaryenLiteralInt32(0))), z,
            BinaryenIf(function->module, BinaryenBinary(function->module, BinaryenNeInt64(),
                BinaryenLocalGet(function->module, (BinaryenIndex)function->predicate_result,
                    BinaryenTypeInt64()), i64(function, 0)), y, n)));
    deallocate(yes.items); deallocate(no.items); deallocate(failed.items);
    if (!ok) { deallocate(evaluated.items); return false; }
    BinaryenExpressionRef evaluated_body = BinaryenBlock(function->module, NULL, evaluated.items,
        (BinaryenIndex)evaluated.count, BinaryenTypeNone());
    deallocate(evaluated.items);
    if (evaluated_body == NULL) return false;
    ok = represented_cleanup_actions_emit_traced(function, event, satisfied, &skipped)
        && represented_edge(function, term->satisfied_edge, &skipped);
    BinaryenExpressionRef skipped_body = ok ? BinaryenBlock(function->module, NULL, skipped.items,
        (BinaryenIndex)skipped.count, BinaryenTypeNone()) : NULL;
    deallocate(skipped.items);
    BinaryenExpressionRef object = get_value(function, plan->result);
    const SolMirTypeLayout *result_layout = &concrete->layout.types[qualified_image->result.recipe];
    BinaryenExpressionRef tag = object == NULL ? NULL : BinaryenLoad(function->module, 4, false,
        (uint32_t)result_layout->tag_offset, 4, BinaryenTypeInt32(),
        wrap_i64(function->module, object), P43_MEMORY);
    uint32_t expected = plan->contract_outcome == SOL_CONTRACT_OUTCOME_SUCCESS
        ? qualified_image->result.success_tag : qualified_image->result.failure_tag;
    return ok && tag != NULL && skipped_body != NULL && represented_nodes_push(nodes,
        BinaryenIf(function->module, BinaryenBinary(function->module, BinaryenEqInt32(), tag,
            BinaryenConst(function->module, BinaryenLiteralInt32((int32_t)expected))),
            evaluated_body, skipped_body));
}

static bool represented_contract_violation_emit(const RepresentedFunction *function, size_t block,
    RepresentedNodes *nodes) {
    const SolMirRuntimeLoweredProgram *owner = function->request->program;
    if (block >= owner->image_terminator_count) return false;
    const SolMirRuntimeLoweredImageTerminator *row = &owner->image_terminators[block];
    if (row->cleanup_event >= owner->cleanup->event_count) return false;
    const SolMirRuntimeCleanupEvent *event = &owner->cleanup->events[row->cleanup_event];
    if (event->transitions.count != 1 || event->transitions.offset >= owner->cleanup->transition_count) return false;
    const SolMirRuntimeCleanupTransition *transition = &owner->cleanup->transitions[event->transitions.offset];
    return transition->continuation == SOL_MIR_RUNTIME_NONE
        && represented_cleanup_actions_emit_traced(function, event, transition, nodes)
        && represented_nodes_push(nodes, BinaryenReturn(function->module, i64(function, 0)));
}

static bool represented_propagation_actions_emit(const RepresentedFunction *function,
    const SolMirRuntimeCleanupTransition *transition, RepresentedNodes *nodes) {
    const SolMirRuntimeCleanup *cleanup = function->request->program->cleanup;
    return transition->event < cleanup->event_count && represented_cleanup_actions_emit_traced(function,
        &cleanup->events[transition->event], transition, nodes);
}

static bool represented_propagation_pre_failure_emit(const RepresentedFunction *function,
    size_t event_id, RepresentedNodes *nodes) {
    const SolMirRuntimeCleanup *cleanup = function->request->program->cleanup;
    if (event_id >= cleanup->event_count) return false;
    const SolMirRuntimeCleanupEvent *event = &cleanup->events[event_id];
    const SolMirRuntimeCleanupTransition *failure = NULL;
    for (size_t i = 0; i < event->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *candidate = &cleanup->transitions[
            event->transitions.offset + i];
        if (candidate->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE) {
            if (failure != NULL) return false;
            failure = candidate;
        }
    }
    return failure != NULL && represented_propagation_actions_emit(function, failure, nodes)
        && represented_nodes_push(nodes, BinaryenReturn(function->module, i64(function, 0)));
}

static BinaryenExpressionRef represented_propagation_field_load(const RepresentedFunction *function,
    BinaryenExpressionRef object, const SolMirFieldLayout *field) {
    if (object == NULL || field == NULL || !field->has_storage || field->offset > UINT32_MAX
        || field->alignment > UINT32_MAX || (field->size != 1 && field->size != 4
            && field->size != 8)) return NULL;
    BinaryenExpressionRef loaded = BinaryenLoad(function->module, (uint32_t)field->size, false,
        (uint32_t)field->offset, (uint32_t)field->alignment,
        field->size == 8 ? BinaryenTypeInt64() : BinaryenTypeInt32(),
        wrap_i64(function->module, object), P43_MEMORY);
    return field->size == 8 ? loaded : extend_u32(function->module, loaded);
}

static bool represented_propagation_emit(const RepresentedFunction *function, size_t block,
    RepresentedNodes *nodes) {
    const SolWasmRepresentedBuildRequest *request = function->request;
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirLayout *layout = &concrete->layout;
    const SolMirMaterializedTerminator *term = &concrete->materialization.blocks[block].terminator;
    const SolMirOperationPropagationPlan *plan = NULL;
    const SolMirRuntimeCleanupTransition *value_transition = NULL, *residual_transition = NULL;
    size_t pre_event = 0, supplemental = 0;
    if (!represented_propagation_plan(request, function->image_id, block, &plan)
        || !represented_propagation_pre_event(request, function->image_id, block, plan,
            &pre_event, &supplemental)
        || !represented_propagation_main_event(request, function->image_id, block, plan,
            &value_transition, &residual_transition)) return false;
    size_t source_init = temporary_init_index(function, plan->source);
    size_t success_result = value_index(function, plan->success_result);
    size_t residual_result = value_index(function, plan->residual_result);
    if (source_init == SIZE_MAX || success_result == SIZE_MAX || residual_result == SIZE_MAX)
        return false;
    BinaryenExpressionRef source = temporary_get(function, plan->source);
    const SolMirFieldLayout *success_field = &layout->fields[plan->success_field_layout];
    if (source == NULL) return false;

    RepresentedNodes success = {0};
    BinaryenExpressionRef payload = success_field->size == 0 ? i64(function, 0)
        : represented_propagation_field_load(function, temporary_get(function, plan->source), success_field);
    bool ok = payload != NULL && represented_nodes_push(&success, BinaryenLocalSet(function->module,
        (BinaryenIndex)success_result, payload)) && represented_nodes_push(&success,
        BinaryenLocalSet(function->module, (BinaryenIndex)source_init,
            BinaryenConst(function->module, BinaryenLiteralInt32(0))))
        && represented_propagation_actions_emit(function, value_transition, &success)
        && represented_edge(function, plan->success_edge, &success);
    BinaryenExpressionRef success_body = ok ? BinaryenBlock(function->module, NULL, success.items,
        (BinaryenIndex)success.count, BinaryenTypeNone()) : NULL;
    deallocate(success.items);
    if (!ok) return false;

    size_t record = 0;
    if (!represented_supplemental_record_index(function->provenance, supplemental, &record)) return false;
    BinaryenExpressionRef allocation_arguments[] = {
        i64(function, (int64_t)layout->types[plan->residual_recipe].object_size),
        i64(function, (int64_t)record)};
    RepresentedNodes residual_packet = {0};
    if (!represented_nodes_push(&residual_packet, BinaryenLocalSet(function->module,
            (BinaryenIndex)function->scratch_base, BinaryenCall(function->module, P43_FIXED_ALLOC,
                allocation_arguments, 2, BinaryenTypeInt64())))) {
        deallocate(residual_packet.items); return false;
    }
    RepresentedNodes allocation_failed = {0};
    ok = represented_propagation_pre_failure_emit(function, pre_event, &allocation_failed);
    BinaryenExpressionRef failure_body = ok ? BinaryenBlock(function->module, NULL,
        allocation_failed.items, (BinaryenIndex)allocation_failed.count, BinaryenTypeNone()) : NULL;
    deallocate(allocation_failed.items);
    if (!ok) { deallocate(residual_packet.items); return false; }

    RepresentedNodes residual = {0};
    BinaryenExpressionRef destination = BinaryenLocalGet(function->module,
        (BinaryenIndex)function->scratch_base, BinaryenTypeInt64());
    ok = represented_nodes_push(&residual, BinaryenStore(function->module, 4, 0, 4,
        wrap_i64(function->module, destination), BinaryenConst(function->module,
            BinaryenLiteralInt32((int32_t)plan->destination_residual_tag)), BinaryenTypeInt32(),
        P43_MEMORY));
    if (ok && term->propagation_kind == SOL_IR_PROPAGATE_RESULT) {
        const SolMirFieldLayout *source_field = &layout->fields[plan->source_residual_field_layout];
        const SolMirFieldLayout *destination_field = &layout->fields[plan->destination_residual_field_layout];
        BinaryenExpressionRef error = represented_propagation_field_load(function,
            temporary_get(function, plan->source), source_field);
        if (error == NULL) ok = false;
        if (ok && destination_field->size != 8) error = wrap_i64(function->module, error);
        if (ok) ok = represented_nodes_push(&residual, BinaryenStore(function->module,
            (uint32_t)destination_field->size, (uint32_t)destination_field->offset,
            (uint32_t)destination_field->alignment, wrap_i64(function->module, destination), error,
            destination_field->size == 8 ? BinaryenTypeInt64() : BinaryenTypeInt32(), P43_MEMORY));
    }
    if (ok) ok = represented_nodes_push(&residual, BinaryenLocalSet(function->module,
        (BinaryenIndex)residual_result, BinaryenLocalGet(function->module,
            (BinaryenIndex)function->scratch_base, BinaryenTypeInt64()))) && represented_nodes_push(&residual,
        BinaryenLocalSet(function->module, (BinaryenIndex)source_init,
            BinaryenConst(function->module, BinaryenLiteralInt32(0))))
        && represented_propagation_actions_emit(function, residual_transition, &residual)
        && represented_edge(function, plan->residual_edge, &residual);
    BinaryenExpressionRef residual_body = ok ? BinaryenBlock(function->module, NULL, residual.items,
        (BinaryenIndex)residual.count, BinaryenTypeNone()) : NULL;
    deallocate(residual.items);
    if (!ok) { deallocate(residual_packet.items); return false; }
    BinaryenExpressionRef allocation_ok = BinaryenIf(function->module, BinaryenBinary(function->module,
        BinaryenEqInt64(), BinaryenLocalGet(function->module, (BinaryenIndex)function->scratch_base,
            BinaryenTypeInt64()), i64(function, 0)), failure_body, residual_body);
    if (allocation_ok == NULL || !represented_nodes_push(&residual_packet, allocation_ok)) {
        deallocate(residual_packet.items); return false;
    }
    BinaryenExpressionRef residual_packet_body = BinaryenBlock(function->module, NULL,
        residual_packet.items, (BinaryenIndex)residual_packet.count, BinaryenTypeNone());
    deallocate(residual_packet.items);
    BinaryenExpressionRef active_success = BinaryenBinary(function->module, BinaryenEqInt32(),
        BinaryenLoad(function->module, 4, false, 0, 4, BinaryenTypeInt32(),
            wrap_i64(function->module, source), P43_MEMORY), BinaryenConst(function->module,
                BinaryenLiteralInt32((int32_t)plan->success_tag)));
    return residual_packet_body != NULL && represented_nodes_push(nodes, BinaryenIf(function->module,
        active_success, success_body, residual_packet_body));
}

/* Unit remains logical Unit.  The narrow exclusive callback and direct-method
 * conventions alone use the Wasm result slot as private receiver transport. */
static BinaryenExpressionRef represented_return_value(const RepresentedFunction *function) {
    if (function->signature->result_class != SOL_MIR_RUNTIME_RESULT_UNIT)
        return NULL;
    const SolMirConcreteProgram *concrete = function->request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    if (function->signature->slots.count != 1
        || function->signature->slots.offset >= function->request->program->conventions->signature_slot_count)
        return i64(function, 0);
    const SolMirRuntimeSignatureSlot *slot = &function->request->program->conventions->signature_slots[
        function->signature->slots.offset];
    if ((slot->role != SOL_MIR_RUNTIME_SLOT_PARAMETER && slot->role != SOL_MIR_RUNTIME_SLOT_RECEIVER)
        || (slot->role == SOL_MIR_RUNTIME_SLOT_PARAMETER && slot->formal != 0)
        || slot->access != SOL_ACCESS_EXCLUSIVE || slot->recipe >= concrete->representation.recipe_count
        || concrete->representation.recipes[slot->recipe].kind != SOL_MIR_RECIPE_INT64) return i64(function, 0);
    size_t parameter = SIZE_MAX;
    for (size_t i = 0; i < function->image->locals.count; ++i) {
        size_t local = function->image->locals.offset + i;
        if (local >= m->local_count) return NULL;
        const SolMirMaterializedLocal *candidate = &m->locals[local];
        if ((slot->role == SOL_MIR_RUNTIME_SLOT_PARAMETER
                && candidate->kind == SOL_MIR_MATERIALIZED_LOCAL_PARAMETER && candidate->ordinal == 0)
            || (slot->role == SOL_MIR_RUNTIME_SLOT_RECEIVER
                && candidate->kind == SOL_MIR_MATERIALIZED_LOCAL_RECEIVER)) {
            if (parameter != SIZE_MAX || candidate->type >= concrete->layout.type_count
                || concrete->layout.types[candidate->type].recipe != slot->recipe) return NULL;
            parameter = local;
        }
    }
    size_t local = parameter == SIZE_MAX ? SIZE_MAX : local_index(function, parameter);
    return local == SIZE_MAX ? NULL : BinaryenLocalGet(function->module, (BinaryenIndex)local,
        BinaryenTypeInt64());
}

static bool represented_terminator_emit(const RepresentedFunction *function, size_t block,
    RepresentedNodes *nodes) {
    const SolMirMaterializedTerminator *term = &function->request->program->conventions
        ->concrete->materialization.blocks[block].terminator;
    const SolMirRuntimeLoweredImageTerminator *row = &function->request->program->image_terminators[block];
    if (row->state == SOL_MIR_RUNTIME_LOWERED_PRESENT
        && !represented_image_step_emit(function, row->step_failure_site, row->step_cleanup_event,
            nodes)) return false;
    switch (term->kind) {
        case SOL_MIR_TERM_GOTO: case SOL_MIR_TERM_BREAK: case SOL_MIR_TERM_CONTINUE: {
            const SolMirRuntimeCleanupTransition *transition = represented_control_transition(
                function->request, function->image, function->qualified, function->image_id, block,
                SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO, SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL,
                term->edge, 1);
            return transition != NULL && represented_edge(function, transition->continuation, nodes);
        }
        case SOL_MIR_TERM_BRANCH: {
            RepresentedNodes left = {0}, right = {0};
            const SolMirRuntimeCleanupTransition *true_transition = represented_control_transition(
                function->request, function->image, function->qualified, function->image_id, block,
                SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_TRUE, SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL,
                term->true_edge, 2);
            const SolMirRuntimeCleanupTransition *false_transition = represented_control_transition(
                function->request, function->image, function->qualified, function->image_id, block,
                SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_FALSE, SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL,
                term->false_edge, 2);
            bool ok = true_transition != NULL && false_transition != NULL
                && represented_edge(function, true_transition->continuation, &left)
                && represented_edge(function, false_transition->continuation, &right);
            BinaryenExpressionRef condition = get_value(function, term->condition);
            BinaryenExpressionRef yes = ok ? BinaryenBlock(function->module, NULL, left.items,
                (BinaryenIndex)left.count, BinaryenTypeNone()) : NULL;
            BinaryenExpressionRef no = ok ? BinaryenBlock(function->module, NULL, right.items,
                (BinaryenIndex)right.count, BinaryenTypeNone()) : NULL;
            deallocate(left.items); deallocate(right.items);
            return ok && condition != NULL && represented_nodes_push(nodes, BinaryenIf(function->module,
                BinaryenBinary(function->module, BinaryenNeInt64(), condition,
                    BinaryenConst(function->module, BinaryenLiteralInt64(0))), yes, no));
        }
        case SOL_MIR_TERM_RETURN: {
            BinaryenExpressionRef value = function->signature->result_class == SOL_MIR_RUNTIME_RESULT_UNIT
                ? represented_return_value(function)
                : get_value(function, term->value);
            if (function->snapshot_count == 0) {
                const SolMirRuntimeCleanupTransition *transition = represented_control_transition(
                    function->request, function->image, function->qualified, function->image_id,
                    block,
                    SOL_MIR_RUNTIME_CLEANUP_EDGE_RETURN, SOL_MIR_RUNTIME_CLEANUP_OUTCOME_EXIT,
                    SOL_MIR_RUNTIME_NONE, 1);
                if (value == NULL || transition == NULL) return false;
                return represented_nodes_push(nodes, BinaryenReturn(function->module, value));
            }
            const SolMirRuntimeCleanupTransition *transition =
                represented_snapshot_return_transition(function->request, function->image,
                    function->image_id, block);
            if (value == NULL || transition == NULL || function->staged_return == SIZE_MAX
                || !represented_nodes_push(nodes, BinaryenLocalSet(function->module,
                    (BinaryenIndex)function->staged_return, value))) return false;
            const SolMirRuntimeCleanupEvent *event = &function->request->program->cleanup->events[
                transition->event];
            return represented_cleanup_actions_emit_traced(function, event, transition, nodes)
                && represented_nodes_push(nodes, BinaryenReturn(function->module,
                    BinaryenLocalGet(function->module, (BinaryenIndex)function->staged_return,
                        BinaryenTypeInt64())));
        }
        case SOL_MIR_TERM_INVOKE:
            return function->calls_enabled && represented_call_emit(function, block, nodes);
        case SOL_MIR_TERM_RESUME_FAILURE:
            return represented_resume_failure_emit(function, block, nodes);
        case SOL_MIR_TERM_CHECK_CONTRACT:
            return represented_contract_emit(function, block, nodes);
        case SOL_MIR_TERM_CHECK_REFINED:
            return represented_refinement_emit(function, block, nodes);
        case SOL_MIR_TERM_CONTRACT_VIOLATION:
            return represented_contract_violation_emit(function, block, nodes);
        case SOL_MIR_TERM_PANIC:
            return represented_terminal_failure_emit(function, block,
                SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_PANIC, SOL_MIR_RUNTIME_FAILURE_PANIC,
                SOL_MIR_RUNTIME_FAILURE_DETAIL_PANIC_TEXT, nodes);
        case SOL_MIR_TERM_MATCH_FAILURE:
            /* Preflight has independently certified this terminal as unreachable
             * from the authenticated unguarded decision rows.  A guarded or
             * otherwise non-total decision instead names its genuine P3 no-match
             * terminal; it is a packet return, never a Wasm `unreachable`. */
            return represented_match_failure_certified(function->request, term)
                ? represented_nodes_push(nodes, BinaryenReturn(function->module, i64(function, 0)))
                : represented_terminal_failure_emit(function, block,
                    SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_NO_MATCH,
                    SOL_MIR_RUNTIME_FAILURE_NO_MATCH,
                    SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE, nodes);
        case SOL_MIR_TERM_UNREACHABLE:
            return represented_terminal_failure_emit(function, block,
                SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_UNREACHABLE,
                SOL_MIR_RUNTIME_FAILURE_REACHED_UNREACHABLE,
                SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE, nodes);
        case SOL_MIR_TERM_PROPAGATE:
            return represented_propagation_emit(function, block, nodes);
        default:
            return false;
    }
}

static bool represented_function_emit(const SolWasmRepresentedBuildRequest *request,
    BinaryenModuleRef module, size_t callable, const SolMirRuntimeSignature *signature,
    const char *name, const RepresentedCallCatalogGraph *catalog,
    const RepresentedQualifiedIndex *qualified_index,
    const RepresentedLiteral *literals, size_t literal_count,
    const RepresentedProvenance *provenance) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirMaterialization *materialization = &concrete->materialization;
    const SolMirMaterializedImage *image = &materialization->images[
        concrete->linkage.callables[callable].instance];
    size_t predicate_values = 0;
    if (!represented_predicate_values_for_image(concrete,
            concrete->linkage.callables[callable].instance, &predicate_values)) return false;
    size_t scratch_count = 0;
    if (!represented_function_scratch_count(request, image,
            concrete->linkage.callables[callable].instance, &scratch_count)) return false;
    size_t snapshot_count = 0;
    if (!represented_snapshot_count_for_image(concrete,
            concrete->linkage.callables[callable].instance, &snapshot_count)) return false;
    size_t physical = 0;
    if (!represented_function_local_count(request, callable, signature, &physical)
        || physical < signature->slots.count) return false;
    size_t variables = physical - signature->slots.count;
    BinaryenType *types = variables == 0 ? NULL : allocate(variables, sizeof *types);
    BinaryenType *parameters = signature->slots.count == 0 ? NULL
        : allocate(signature->slots.count, sizeof *parameters);
    if ((variables != 0 && types == NULL) || (signature->slots.count != 0 && parameters == NULL)) {
        deallocate(types); deallocate(parameters); return false;
    }
    size_t n = 0;
    for (; n < image->values.count + image->temporaries.count + image->locals.count
        + predicate_values + (predicate_values != 0 ? 1 : 0) + scratch_count
        + snapshot_count + (snapshot_count != 0 ? 1 : 0); ++n)
        types[n] = BinaryenTypeInt64();
    for (; n < variables; ++n) types[n] = BinaryenTypeInt32();
    for (size_t i = 0; i < signature->slots.count; ++i) parameters[i] = BinaryenTypeInt64();
    RepresentedFunction function = {
        .request = request, .module = module, .image = image,
        .signature = signature, .image_id = concrete->linkage.callables[callable].instance,
        .catalog = catalog, .qualified = qualified_index, .calls_enabled = catalog != NULL,
        .literals = literals, .literal_count = literal_count, .provenance = provenance,
        .parameter_count = signature->slots.count,
        .value_base = signature->slots.count,
        .temporary_base = signature->slots.count + image->values.count,
        .local_base = signature->slots.count + image->values.count + image->temporaries.count,
        .predicate_value_base = signature->slots.count + image->values.count + image->temporaries.count
            + image->locals.count,
        .predicate_result = signature->slots.count + image->values.count + image->temporaries.count
            + image->locals.count + predicate_values,
        .predicate_value_count = predicate_values,
        .snapshot_count = snapshot_count,
        .staged_return = SIZE_MAX,
    };
    if (!represented_function_callable_hole_count(request, image, &function.local_hole_count)) {
        deallocate(types); deallocate(parameters); return false;
    }
    function.scratch_base = function.predicate_result + (predicate_values != 0 ? 1 : 0);
    function.snapshot_value_base = function.scratch_base + scratch_count;
    if (snapshot_count != 0) function.staged_return = function.snapshot_value_base + snapshot_count;
    function.temporary_init_base = snapshot_count == 0 ? function.scratch_base + scratch_count
        : function.staged_return + 1;
    function.local_init_base = function.temporary_init_base + image->temporaries.count;
    function.local_hole_base = function.local_init_base + image->locals.count;
    function.snapshot_init_base = function.local_hole_base + function.local_hole_count;
    function.predicate_pc = function.snapshot_init_base + snapshot_count;
    function.pc = function.predicate_pc + (predicate_values != 0 ? 1 : 0);
    RepresentedNodes body = {0}, dispatch = {0};
    bool ok = image->entry >= image->blocks.offset
        && image->entry - image->blocks.offset < image->blocks.count;
    if (ok) ok = represented_nodes_push(&body, BinaryenLocalSet(module, (BinaryenIndex)function.pc,
            BinaryenConst(module, BinaryenLiteralInt32((int32_t)(image->entry - image->blocks.offset)))));
    for (size_t b = 0; ok && b < image->blocks.count; ++b) {
        size_t block = image->blocks.offset + b;
        RepresentedNodes block_nodes = {0};
        const SolMirMaterializedBlock *item = &materialization->blocks[block];
        for (size_t i = 0; ok && i < item->instructions.count; ++i) {
            size_t instruction = item->instructions.offset + i;
            ok = represented_instruction_emit(&function, instruction, &block_nodes);
        }
        if (ok) ok = represented_terminator_emit(&function, block, &block_nodes);
        BinaryenExpressionRef contents = ok ? BinaryenBlock(module, NULL, block_nodes.items,
            (BinaryenIndex)block_nodes.count, BinaryenTypeNone()) : NULL;
        deallocate(block_nodes.items);
        if (ok) ok = represented_nodes_push(&dispatch, BinaryenIf(module,
            BinaryenBinary(module, BinaryenEqInt32(), BinaryenLocalGet(module,
                (BinaryenIndex)function.pc, BinaryenTypeInt32()), BinaryenConst(module,
                    BinaryenLiteralInt32((int32_t)b))), contents, NULL));
    }
    if (ok) ok = represented_nodes_push(&dispatch, BinaryenReturn(module,
        BinaryenConst(module, BinaryenLiteralInt64(0))));
    BinaryenExpressionRef loop = ok ? BinaryenLoop(module, "p43.dispatch",
        BinaryenBlock(module, NULL, dispatch.items, (BinaryenIndex)dispatch.count,
            BinaryenTypeNone())) : NULL;
    if (ok) ok = represented_nodes_push(&body, loop);
    BinaryenExpressionRef final = ok ? BinaryenBlock(module, NULL, body.items,
        (BinaryenIndex)body.count, BinaryenTypeNone()) : NULL;
    BinaryenType parameter_type = signature->slots.count == 0 ? BinaryenTypeNone()
        : BinaryenTypeCreate(parameters, (BinaryenIndex)signature->slots.count);
    if (ok) ok = BinaryenAddFunction(module, name, parameter_type, BinaryenTypeInt64(),
        types, (BinaryenIndex)variables, final) != NULL;
    deallocate(dispatch.items); deallocate(body.items); deallocate(types); deallocate(parameters);
    return ok;
}

typedef struct { size_t callable; const SolMirLinkage *linkage; } RepresentedCallableOrder;
static int callable_order_compare(const void *left, const void *right) {
    const RepresentedCallableOrder *a = left, *b = right;
    return strcmp(a->linkage->callables[a->callable].symbol.bytes,
        b->linkage->callables[b->callable].symbol.bytes);
}

typedef struct { size_t entry; const SolMirRuntimeConventions *conventions; } RepresentedEntryOrder;
static int entry_order_compare(const void *left, const void *right) {
    const RepresentedEntryOrder *a = left, *b = right;
    return strcmp(a->conventions->entries[a->entry].symbol.bytes,
        b->conventions->entries[b->entry].symbol.bytes);
}

/* Keep every entry-like probe on the exact entry reset sequence.  The test-only
 * reset functions below deliberately reuse this builder instead of duplicating
 * a nearly-identical packet clear. */
static bool represented_entry_packet_reset_emit(BinaryenModuleRef module, uint32_t heap_base,
    bool panic_detail, BinaryenExpressionRef *items, size_t capacity, size_t *count) {
    size_t trace_items = 0;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    trace_items = represented_trace_ledger_enabled() ? 2 : 0;
#endif
    if (items == NULL || count == NULL || capacity < 7 + (panic_detail ? 2 : 0) + trace_items) return false;
    *count = 0;
    items[(*count)++] = BinaryenGlobalSet(module, P43_CODE,
        BinaryenConst(module, BinaryenLiteralInt32(0)));
    items[(*count)++] = BinaryenGlobalSet(module, P43_SITE,
        BinaryenConst(module, BinaryenLiteralInt32(0)));
    items[(*count)++] = BinaryenGlobalSet(module, P43_HEAP, BinaryenConst(module,
        BinaryenLiteralInt32((int32_t)heap_base)));
    items[(*count)++] = BinaryenGlobalSet(module, P43_REQUESTS,
        BinaryenConst(module, BinaryenLiteralInt64(0)));
    items[(*count)++] = BinaryenGlobalSet(module, P43_BYTES,
        BinaryenConst(module, BinaryenLiteralInt64(0)));
    items[(*count)++] = BinaryenGlobalSet(module, P44_STEPS,
        BinaryenConst(module, BinaryenLiteralInt64(0)));
    items[(*count)++] = BinaryenGlobalSet(module, P44_CALL_DEPTH,
        BinaryenConst(module, BinaryenLiteralInt32(1)));
    if (panic_detail) {
        items[(*count)++] = BinaryenGlobalSet(module, P44_PANIC_DETAIL_LENGTH,
            BinaryenConst(module, BinaryenLiteralInt32(0)));
        items[(*count)++] = BinaryenStore(module, 1, 0, 1,
            BinaryenGlobalGet(module, P44_PANIC_DETAIL_OFFSET, BinaryenTypeInt32()),
            BinaryenConst(module, BinaryenLiteralInt32(0)), BinaryenTypeInt32(), P43_MEMORY);
    }
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (represented_trace_ledger_enabled()) {
        items[(*count)++] = BinaryenGlobalSet(module, P44_TRACE_COUNT,
            BinaryenConst(module, BinaryenLiteralInt32(0)));
        items[(*count)++] = BinaryenGlobalSet(module, P44_TRACE_OVERFLOW,
            BinaryenConst(module, BinaryenLiteralInt32(0)));
    }
#endif
    for (size_t i = 0; i < *count; ++i)
        if (items[i] == NULL) return false;
    return true;
}

static bool represented_entry_wrapper_emit(BinaryenModuleRef module,
    const SolMirRuntimeEntry *entry, const SolMirLinkage *linkage,
    const SolMirRuntimeConventions *conventions, uint32_t heap_base, bool panic_detail) {
    if (entry->callable >= linkage->callable_count) return false;
    const SolMirRuntimeSignature *signature = signature_for(conventions, entry->callable);
    if (signature == NULL || signature->slots.count != 0) return false;
    BinaryenExpressionRef body_items[P44_ENTRY_WRAPPER_MAX_ITEMS]; size_t body_count = 0;
    if (!represented_entry_packet_reset_emit(module, heap_base, panic_detail, body_items,
            sizeof body_items / sizeof *body_items, &body_count)) return false;
    BinaryenExpressionRef result = BinaryenReturn(module, BinaryenCall(module,
        linkage->callables[entry->callable].symbol.bytes, NULL, 0, BinaryenTypeInt64()));
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (represented_test_callback_writeback_probe) {
        if (body_count >= P44_ENTRY_WRAPPER_MAX_ITEMS) return false;
        body_items[body_count++] = BinaryenGlobalSet(module, P43_WRITEBACKS,
            BinaryenConst(module, BinaryenLiteralInt32(0)));
    }
    if (represented_test_callable_hole_cleanup_probe) {
        const char *const cleanup_counters[] = {P43_CLEANUP_OLD_CALLABLE,
            P43_CLEANUP_MOVED_CALLABLE, P43_CLEANUP_TEXT_SIBLING, P43_CLEANUP_ROOT};
        for (size_t i = 0; i < sizeof cleanup_counters / sizeof *cleanup_counters; ++i) {
            if (body_count >= P44_ENTRY_WRAPPER_MAX_ITEMS) return false;
            body_items[body_count++] = BinaryenGlobalSet(module, cleanup_counters[i],
                BinaryenConst(module, BinaryenLiteralInt32(0)));
        }
    }
#endif
    if (body_count >= P44_ENTRY_WRAPPER_MAX_ITEMS) return false;
    body_items[body_count++] = result;
    for (size_t i = 0; i < body_count; ++i)
        if (body_items[i] == NULL) return false;
    if (BinaryenAddFunction(module, entry->symbol.bytes, BinaryenTypeNone(), BinaryenTypeInt64(),
            NULL, 0, BinaryenBlock(module, NULL, body_items, (BinaryenIndex)body_count,
                BinaryenTypeNone())) == NULL)
        return false;
    BinaryenAddFunctionExport(module, entry->symbol.bytes, entry->symbol.bytes);
    return true;
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
/* Test-only observability for a single instantiated panic module. The success
 * form proves the packet clear; the nonpanic form uses code 2 and sentinel site
 * zero because no source P3.1 code-2 occurrence belongs to this module. */
static bool represented_p44_packet_reset_probe_functions(BinaryenModuleRef module,
    uint32_t heap_base) {
    const char *const names[] = {P44_TEST_PACKET_RESET_SUCCESS, P44_TEST_PACKET_RESET_NONPANIC};
    for (size_t probe = 0; probe < sizeof names / sizeof *names; ++probe) {
        BinaryenExpressionRef items[P44_PACKET_RESET_PROBE_MAX_ITEMS]; size_t count = 0;
        if (!represented_entry_packet_reset_emit(module, heap_base, true, items,
                sizeof items / sizeof *items, &count)) return false;
        if (probe != 0) {
            if (count + 2 > P44_PACKET_RESET_PROBE_MAX_ITEMS) return false;
            items[count++] = BinaryenGlobalSet(module, P43_CODE,
                BinaryenConst(module, BinaryenLiteralInt32(2)));
            items[count++] = BinaryenGlobalSet(module, P43_SITE,
                BinaryenConst(module, BinaryenLiteralInt32(0)));
        }
        if (count >= P44_PACKET_RESET_PROBE_MAX_ITEMS) return false;
        items[count++] = BinaryenReturn(module, BinaryenConst(module, BinaryenLiteralInt64(0)));
        if (BinaryenAddFunction(module, names[probe], BinaryenTypeNone(), BinaryenTypeInt64(),
                NULL, 0, BinaryenBlock(module, NULL, items, (BinaryenIndex)count,
                    BinaryenTypeNone())) == NULL
            || BinaryenAddFunctionExport(module, names[probe], names[probe]) == NULL) return false;
    }
    return true;
}

/* The probe reaches the one non-entry, zero-parameter C2b fixture directly.
 * It is deliberately unavailable unless the counter probe is enabled. */
static bool represented_callback_writeback_failure_export(BinaryenModuleRef module,
    const SolMirRuntimeConventions *conventions, const SolMirLinkage *linkage) {
    size_t candidate = SOL_MIR_LINKAGE_NONE;
    for (size_t i = 0; i < linkage->callable_count; ++i) {
        const SolMirRuntimeSignature *signature = signature_for(conventions, i);
        bool entry = false;
        for (size_t q = 0; q < conventions->entry_count; ++q)
            entry = entry || conventions->entries[q].callable == i;
        if (entry || signature == NULL || signature->slots.count != 0
            || signature->result_class != SOL_MIR_RUNTIME_RESULT_VALUE) continue;
        if (candidate != SOL_MIR_LINKAGE_NONE) return false;
        candidate = i;
    }
    return candidate != SOL_MIR_LINKAGE_NONE && BinaryenAddFunctionExport(module,
        linkage->callables[candidate].symbol.bytes,
        SOL_WASM_REPRESENTED_TEST_FAILURE_ENTRY_EXPORT) != NULL;
}
#endif

static bool wasmtime_validate(const SolWasmBackendBytes *bytes) {
    wasm_engine_t *engine = wasm_engine_new();
    wasm_store_t *store = engine == NULL ? NULL : wasm_store_new(engine);
    wasm_byte_vec_t input = {bytes->count, (wasm_byte_t *)bytes->bytes};
    bool valid = store != NULL && wasm_module_validate(store, &input);
    if (store != NULL) wasm_store_delete(store);
    if (engine != NULL) wasm_engine_delete(engine);
    return valid;
}

typedef struct {
    uint8_t tag, kind;
    const uint8_t *path, *symbol;
    uint32_t path_count, start, end, symbol_count, ordinal;
} RepresentedWireProvenance;

static bool represented_read_uleb32(const uint8_t **cursor, const uint8_t *end,
    uint32_t *value) {
    uint32_t result = 0;
    for (unsigned shift = 0; shift < 35; shift += 7) {
        if (*cursor == end || (shift == 28 && (**cursor & UINT8_C(0xf0)) != 0)) return false;
        uint8_t byte = *(*cursor)++;
        result |= (uint32_t)(byte & UINT8_C(0x7f)) << shift;
        if ((byte & UINT8_C(0x80)) == 0) { *value = result; return true; }
    }
    return false;
}

/* MVP i32.const uses signed LEB, unlike section sizes and indices. Keep the
 * decoded 32-bit pattern unsigned to avoid signed shifts/conversions. At most
 * five bytes are permitted; the fifth's unused bits must extend bit 31 and it
 * must terminate. As with the existing i64 reader, valid redundant sign bytes
 * within the width bound are accepted (Wasm does not require shortest LEBs). */
static bool represented_read_i32_bits(const uint8_t **cursor, const uint8_t *end,
    uint32_t *value) {
    uint32_t bits = 0;
    for (unsigned i = 0; i < 5; ++i) {
        if (*cursor == end) return false;
        uint8_t byte = *(*cursor)++;
        if (i == 4 && ((byte & UINT8_C(0x80)) != 0
                || ((byte & UINT8_C(0x78)) != 0
                    && (byte & UINT8_C(0x78)) != UINT8_C(0x78)))) return false;
        bits |= (uint32_t)(byte & (i == 4 ? UINT8_C(0x0f) : UINT8_C(0x7f))) << (i * 7);
        if ((byte & UINT8_C(0x80)) == 0) {
            unsigned used = (i + 1) * 7;
            if (used < 32 && (byte & UINT8_C(0x40)) != 0) bits |= UINT32_MAX << used;
            *value = bits;
            return true;
        }
    }
    return false;
}

/* Read an MVP i64.const signed LEB as its unsigned bit pattern. The tenth
 * byte may contain only the sign extension of bit 63; overlong/truncated
 * encodings and out-of-width high bits are rejected without a signed shift. */
static bool represented_read_i64_bits(const uint8_t **cursor, const uint8_t *end,
    uint64_t *value) {
    uint64_t bits = 0;
    for (unsigned i = 0; i < 10; ++i) {
        if (*cursor == end) return false;
        uint8_t byte = *(*cursor)++;
        if (i == 9 && byte != 0 && byte != UINT8_C(0x7f)) return false;
        bits |= (uint64_t)(byte & UINT8_C(0x7f)) << (i * 7);
        if ((byte & UINT8_C(0x80)) == 0) {
            unsigned used = (i + 1) * 7;
            if (used < 64 && (byte & UINT8_C(0x40)) != 0) bits |= UINT64_MAX << used;
            *value = bits;
            return true;
        }
    }
    return false;
}

static bool represented_read_u32le(const uint8_t **cursor, const uint8_t *end,
    uint32_t *value) {
    if ((size_t)(end - *cursor) < 4) return false;
    const uint8_t *bytes = *cursor;
    *value = (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16
        | (uint32_t)bytes[3] << 24;
    *cursor += 4;
    return true;
}

static bool represented_read_name(const uint8_t **cursor, const uint8_t *end,
    const uint8_t **name, uint32_t *count) {
    if (!represented_read_uleb32(cursor, end, count) || *count > (size_t)(end - *cursor))
        return false;
    *name = *cursor;
    *cursor += *count;
    return true;
}

static bool represented_utf8(const uint8_t *text, uint32_t count) {
    for (uint32_t i = 0; i < count; ) {
        uint8_t first = text[i++];
        if (first <= UINT8_C(0x7f)) { if (first == 0) return false; continue; }
        unsigned following = first >= UINT8_C(0xc2) && first <= UINT8_C(0xdf) ? 1
            : first >= UINT8_C(0xe0) && first <= UINT8_C(0xef) ? 2
            : first >= UINT8_C(0xf0) && first <= UINT8_C(0xf4) ? 3 : UINT_MAX;
        if (following == UINT_MAX || following > count - i) return false;
        if ((first == UINT8_C(0xe0) && text[i] < UINT8_C(0xa0))
            || (first == UINT8_C(0xed) && text[i] >= UINT8_C(0xa0))
            || (first == UINT8_C(0xf0) && text[i] < UINT8_C(0x90))
            || (first == UINT8_C(0xf4) && text[i] >= UINT8_C(0x90))) return false;
        for (unsigned j = 0; j < following; ++j)
            if ((text[i + j] & UINT8_C(0xc0)) != UINT8_C(0x80)) return false;
        i += following;
    }
    return true;
}

static bool represented_relative_path(const uint8_t *path, uint32_t count) {
    if (count == 0 || path[0] == '/') return false;
    uint32_t component = 0;
    for (uint32_t i = 0; i <= count; ++i) {
        if (i != count && path[i] != '/') continue;
        uint32_t length = i - component;
        if (length == 0 || (length == 1 && path[component] == '.')
            || (length == 2 && path[component] == '.' && path[component + 1] == '.')) return false;
        component = i + 1;
    }
    return represented_utf8(path, count);
}

static int represented_wire_provenance_order(const RepresentedWireProvenance *a,
    const RepresentedWireProvenance *b) {
    unsigned a_rank = a->tag == 3
            && (a->kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_STEP
                || a->kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_STEP)
        ? 5u : a->tag;
    unsigned b_rank = b->tag == 3
            && (b->kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_STEP
                || b->kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_STEP)
        ? 5u : b->tag;
    if (a_rank != b_rank) return a_rank < b_rank ? -1 : 1;
    uint32_t shared = a->path_count < b->path_count ? a->path_count : b->path_count;
    int order = shared == 0 ? 0 : memcmp(a->path, b->path, shared);
    if (order != 0) return order;
    if (a->path_count != b->path_count) return a->path_count < b->path_count ? -1 : 1;
    if (a->start != b->start) return a->start < b->start ? -1 : 1;
    if (a->end != b->end) return a->end < b->end ? -1 : 1;
    shared = a->symbol_count < b->symbol_count ? a->symbol_count : b->symbol_count;
    order = shared == 0 ? 0 : memcmp(a->symbol, b->symbol, shared);
    if (order != 0) return order;
    if (a->symbol_count != b->symbol_count) return a->symbol_count < b->symbol_count ? -1 : 1;
    if (a->ordinal != b->ordinal) return a->ordinal < b->ordinal ? -1 : 1;
    if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
    return 0;
}

typedef enum {
    REPRESENTED_WIRE_VALID,
    REPRESENTED_WIRE_INVALID,
    REPRESENTED_WIRE_ALLOCATION,
} RepresentedWireValidation;

typedef struct {
    const uint8_t *name;
    uint32_t count;
    uint32_t hash;
    bool matched;
} RepresentedWireEntry;

typedef struct {
    RepresentedWireEntry *entries;
    uint16_t *slots;
    uint32_t count;
} RepresentedWireEntryMap;

typedef struct {
    const uint8_t **names;
    uint32_t *counts;
    uint32_t count;
} RepresentedWireExports;

static uint32_t represented_name_hash(const uint8_t *name, uint32_t count) {
    uint32_t hash = UINT32_C(2166136261);
    for (uint32_t i = 0; i < count; ++i) hash = (hash ^ name[i]) * UINT32_C(16777619);
    return hash;
}

static bool represented_wire_entry_map_init(RepresentedWireEntryMap *map) {
    map->entries = allocate(REPRESENTED_WIRE_MAX_FUNCTIONS, sizeof *map->entries);
    map->slots = allocate(REPRESENTED_WIRE_MAX_FUNCTIONS * 2, sizeof *map->slots);
    map->count = 0;
    if (map->entries != NULL && map->slots != NULL) return true;
    deallocate(map->entries); deallocate(map->slots);
    map->entries = NULL; map->slots = NULL;
    return false;
}

static void represented_wire_entry_map_free(RepresentedWireEntryMap *map) {
    deallocate(map->entries); deallocate(map->slots);
    memset(map, 0, sizeof *map);
}

static bool represented_wire_entry_insert(RepresentedWireEntryMap *map,
    const uint8_t *name, uint32_t count) {
    uint32_t hash = represented_name_hash(name, count);
    size_t slot = hash % (REPRESENTED_WIRE_MAX_FUNCTIONS * 2);
    for (size_t probes = 0; probes < REPRESENTED_WIRE_MAX_FUNCTIONS * 2; ++probes) {
        uint16_t value = map->slots[slot];
        if (value == 0) {
            if (map->count >= REPRESENTED_WIRE_MAX_FUNCTIONS) return false;
            map->entries[map->count] = (RepresentedWireEntry){name, count, hash, false};
            map->slots[slot] = (uint16_t)(map->count + 1);
            ++map->count;
            return true;
        }
        RepresentedWireEntry *existing = &map->entries[value - 1];
        if (existing->hash == hash && existing->count == count
            && memcmp(existing->name, name, count) == 0) return false;
        slot = (slot + 1) % (REPRESENTED_WIRE_MAX_FUNCTIONS * 2);
    }
    return false;
}

static RepresentedWireEntry *represented_wire_entry_find(RepresentedWireEntryMap *map,
    const uint8_t *name, uint32_t count) {
    uint32_t hash = represented_name_hash(name, count);
    size_t slot = hash % (REPRESENTED_WIRE_MAX_FUNCTIONS * 2);
    for (size_t probes = 0; probes < REPRESENTED_WIRE_MAX_FUNCTIONS * 2; ++probes) {
        uint16_t value = map->slots[slot];
        if (value == 0) return NULL;
        RepresentedWireEntry *entry = &map->entries[value - 1];
        if (entry->hash == hash && entry->count == count
            && memcmp(entry->name, name, count) == 0) return entry;
        slot = (slot + 1) % (REPRESENTED_WIRE_MAX_FUNCTIONS * 2);
    }
    return NULL;
}

static bool represented_wire_exports_init(RepresentedWireExports *exports) {
    exports->names = allocate(REPRESENTED_WIRE_MAX_FUNCTIONS, sizeof *exports->names);
    exports->counts = allocate(REPRESENTED_WIRE_MAX_FUNCTIONS, sizeof *exports->counts);
    exports->count = 0;
    if (exports->names != NULL && exports->counts != NULL) return true;
    deallocate(exports->names); deallocate(exports->counts);
    exports->names = NULL; exports->counts = NULL;
    return false;
}

static void represented_wire_exports_free(RepresentedWireExports *exports) {
    deallocate(exports->names); deallocate(exports->counts);
    memset(exports, 0, sizeof *exports);
}

/* This is intentionally independent of the producer's in-memory records. It
 * treats the custom payload as untrusted bytes and accepts only the complete,
 * canonical v1 grammar.  Entry names are indexed during this one pass, rather
 * than reparsing the provenance section once for every module export. */
static RepresentedWireValidation represented_provenance_validate(const uint8_t *payload,
    size_t size, RepresentedWireEntryMap *entries, bool *has_panic) {
    const uint8_t *cursor = payload, *end = payload + size;
    uint32_t version = 0, count = 0;
    if (has_panic == NULL || size < 12 || memcmp(cursor, "P43P", 4) != 0)
        return REPRESENTED_WIRE_INVALID;
    *has_panic = false;
    cursor += 4;
    if (!represented_read_u32le(&cursor, end, &version)
        || !represented_read_u32le(&cursor, end, &count) || version != 1
        || count > REPRESENTED_WIRE_MAX_PROVENANCE_RECORDS) return REPRESENTED_WIRE_INVALID;
    if (!represented_wire_entry_map_init(entries)) return REPRESENTED_WIRE_ALLOCATION;
    RepresentedWireProvenance previous = {0};
    for (uint32_t i = 0; i < count; ++i) {
        RepresentedWireProvenance record = {0};
        uint32_t reserved = 0;
        if ((size_t)(end - cursor) < 4) goto invalid;
        record.tag = *cursor++; record.kind = *cursor++;
        reserved = (uint32_t)cursor[0] | (uint32_t)cursor[1] << 8; cursor += 2;
        if (reserved != 0 || record.tag < 1 || record.tag > 4
            || (record.tag != 3 && record.tag != 4 && record.kind != 0)
            || (record.tag == 3 && record.kind > SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_STEP)
            || (record.tag == 4 && record.kind > 1)
            || !represented_read_u32le(&cursor, end, &record.path_count)
            || record.path_count > (size_t)(end - cursor)) goto invalid;
        record.path = cursor; cursor += record.path_count;
        if (!represented_read_u32le(&cursor, end, &record.start)
            || !represented_read_u32le(&cursor, end, &record.end)
            || !represented_read_u32le(&cursor, end, &record.symbol_count)
            || record.symbol_count > (size_t)(end - cursor)) goto invalid;
        record.symbol = cursor; cursor += record.symbol_count;
        if (!represented_read_u32le(&cursor, end, &record.ordinal)
            || record.start > record.end || !represented_relative_path(record.path, record.path_count)
            || record.symbol_count == 0 || !represented_utf8(record.symbol, record.symbol_count)
            || (i != 0 && represented_wire_provenance_order(&previous, &record) >= 0)) goto invalid;
        previous = record;
        if (record.tag == 3 && record.kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_PANIC)
            *has_panic = true;
        if (record.tag == 1 && !represented_wire_entry_insert(entries, record.symbol,
                record.symbol_count)) goto invalid;
    }
    if (cursor == end) return REPRESENTED_WIRE_VALID;
invalid:
    represented_wire_entry_map_free(entries);
    return REPRESENTED_WIRE_INVALID;
}

static bool represented_name_equal(const uint8_t *name, uint32_t count, const char *text) {
    return count == strlen(text) && memcmp(name, text, count) == 0;
}

static bool represented_name_prefix(const uint8_t *name, uint32_t count, const char *prefix) {
    size_t length = strlen(prefix);
    return count > length && memcmp(name, prefix, length) == 0;
}

static bool represented_exports_match_provenance(RepresentedWireExports *exports,
    RepresentedWireEntryMap *entries) {
    if (entries->count == 0 || exports->count != entries->count) return false;
    for (uint32_t i = 0; i < exports->count; ++i) {
        RepresentedWireEntry *entry = represented_wire_entry_find(entries, exports->names[i],
            exports->counts[i]);
        if (entry == NULL || entry->matched) return false;
        entry->matched = true;
    }
    for (uint32_t i = 0; i < entries->count; ++i)
        if (!entries->entries[i].matched) return false;
    return true;
}

/* Verify the intentionally narrow P4.3 physical envelope before passing the
 * bytes to Wasmtime.  Full instruction/type validation remains Wasmtime's job;
 * this parser owns the P4.3 section and import/table/start invariants. */
static RepresentedWireValidation represented_module_shape_validate(const SolWasmBackendBytes *bytes) {
    static const uint8_t header[] = {0, 'a', 's', 'm', 1, 0, 0, 0};
    const uint8_t *cursor = bytes->bytes, *end = bytes->bytes + bytes->count;
    unsigned last_standard = 0, provenance_sections = 0, memory_sections = 0, table_sections = 0,
        element_sections = 0;
    uint32_t function_count = 0, table_initial = 0, element_count = 0;
    RepresentedWireEntryMap provenance_entries = {0};
    RepresentedWireExports entry_exports = {0};
    bool saw_provenance_payload = false, saw_exports_payload = false, provenance_has_panic = false;
    uint32_t table_function_type = 0;
    uint32_t function_types[REPRESENTED_WIRE_MAX_FUNCTIONS] = {0};
    bool table_function_type_set = false;
    bool saw_memory_export = false, saw_code_export = false, saw_site_export = false;
    bool saw_panic_detail_offset_export = false, saw_panic_detail_length_export = false;
    bool saw_p44_packet_reset_success = false, saw_p44_packet_reset_nonpanic = false;
    uint32_t panic_detail_offset_index = 0, panic_detail_length_index = 0;
    bool saw_trace_offset_export = false, saw_trace_count_export = false, saw_trace_overflow_export = false;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    uint32_t trace_offset_index = 0, trace_count_index = 0, trace_overflow_index = 0;
#endif
    uint8_t global_type[REPRESENTED_WIRE_MAX_FUNCTIONS] = {0};
    bool global_mutable[REPRESENTED_WIRE_MAX_FUNCTIONS] = {0};
    uint64_t global_initial[REPRESENTED_WIRE_MAX_FUNCTIONS] = {0};
    uint32_t global_count = 0, active_data_end = 0, heap_base = 0;
    bool saw_active_data = false;
    bool saw_writeback_export = false;
    bool saw_failure_probe_export = false;
    bool saw_cleanup_old_callable_export = false, saw_cleanup_moved_callable_export = false;
    bool saw_cleanup_text_sibling_export = false, saw_cleanup_root_export = false;
    if (bytes->count < sizeof header || memcmp(cursor, header, sizeof header) != 0) goto invalid;
    cursor += sizeof header;
    while (cursor != end) {
        uint32_t section_size = 0;
        if (cursor == end) goto invalid;
        uint8_t id = *cursor++;
        if (id > 12 || !represented_read_uleb32(&cursor, end, &section_size)
            || section_size > (size_t)(end - cursor)) goto invalid;
        const uint8_t *section_end = cursor + section_size;
        if (id != 0) {
            if (id <= last_standard) goto invalid;
            last_standard = id;
        }
        if (id == 0) {
            const uint8_t *name = NULL; uint32_t name_count = 0;
            if (!represented_read_name(&cursor, section_end, &name, &name_count)) goto invalid;
            if (represented_name_equal(name, name_count, SOL_WASM_REPRESENTED_PROVENANCE_SECTION)) {
                if (++provenance_sections != 1) goto invalid;
                RepresentedWireValidation validation = represented_provenance_validate(cursor,
                    (size_t)(section_end - cursor), &provenance_entries, &provenance_has_panic);
                if (validation == REPRESENTED_WIRE_ALLOCATION) goto allocation;
                if (validation != REPRESENTED_WIRE_VALID) goto invalid;
                saw_provenance_payload = true;
            }
        } else if (id == 1) {
            uint32_t count = 0;
            if (!represented_read_uleb32(&cursor, section_end, &count)
                || count > REPRESENTED_WIRE_MAX_FUNCTIONS) goto invalid;
        } else if (id == 6) {
            if (!represented_read_uleb32(&cursor, section_end, &global_count)
                || global_count > REPRESENTED_WIRE_MAX_FUNCTIONS) goto invalid;
            for (uint32_t i = 0; i < global_count; ++i) {
                uint32_t initial = 0;
                if (cursor == section_end || (*cursor != UINT8_C(0x7f)
                        && *cursor != UINT8_C(0x7e))) goto invalid;
                global_type[i] = *cursor++;
                if (cursor == section_end || (*cursor != 0 && *cursor != 1)) goto invalid;
                global_mutable[i] = *cursor++ != 0;
                if (cursor == section_end) goto invalid;
                uint8_t opcode = *cursor++;
                if ((global_type[i] == UINT8_C(0x7f) && opcode != UINT8_C(0x41))
                    || (global_type[i] == UINT8_C(0x7e) && opcode != UINT8_C(0x42))) goto invalid;
                if (global_type[i] == UINT8_C(0x7f)) {
                    if (!represented_read_i32_bits(&cursor, section_end, &initial)) goto invalid;
                    global_initial[i] = initial;
                } else if (!represented_read_i64_bits(&cursor, section_end, &global_initial[i])) goto invalid;
                if (cursor == section_end || *cursor++ != UINT8_C(0x0b)) goto invalid;
            }
            if (cursor != section_end) goto invalid;
            /* Heap is deliberately private. Its fixed physical index is
             * authenticated below only when the P4.4 packet relies on it;
             * P4.3 table-shape controls intentionally permit their legacy
             * synthetic global layouts. */
            if (global_count > 2) {
                if (global_initial[2] > UINT32_MAX) goto invalid;
                heap_base = (uint32_t)global_initial[2];
            }
        } else if (id == 2) {
            uint32_t count = 0;
            if (!represented_read_uleb32(&cursor, section_end, &count) || count != 0) goto invalid;
        } else if (id == 3) {
            uint32_t count = 0;
            if (!represented_read_uleb32(&cursor, section_end, &count)
                || count > REPRESENTED_WIRE_MAX_FUNCTIONS) goto invalid;
            function_count = count;
            for (uint32_t i = 0; i < count; ++i)
                if (!represented_read_uleb32(&cursor, section_end, &function_types[i])) goto invalid;
            if (cursor != section_end) goto invalid;
        } else if (id == 4) {
            uint32_t count = 0, flags = 0, maximum = 0;
            if (++table_sections != 1 || !represented_read_uleb32(&cursor, section_end, &count)
                || count != 1 || cursor == section_end || *cursor++ != UINT8_C(0x70)
                || !represented_read_uleb32(&cursor, section_end, &flags) || flags != 1
                || !represented_read_uleb32(&cursor, section_end, &table_initial)
                || !represented_read_uleb32(&cursor, section_end, &maximum)
                || table_initial < 2 || table_initial > REPRESENTED_WIRE_MAX_TABLE_ELEMENTS
                || maximum != table_initial) goto invalid;
        } else if (id == 8) {
            goto invalid;
        } else if (id == 5) {
            uint32_t count = 0, flags = 0, initial = 0, maximum = 0;
            if (++memory_sections != 1 || !represented_read_uleb32(&cursor, section_end, &count)
                || count != 1 || !represented_read_uleb32(&cursor, section_end, &flags)
                || flags != 1 || !represented_read_uleb32(&cursor, section_end, &initial)
                || !represented_read_uleb32(&cursor, section_end, &maximum) || initial != 1
#ifdef SOL_MIR_PLAN_TEST_HOOKS
                || (maximum != 1 && maximum != 256)
#else
                || maximum != 256
#endif
                ) goto invalid;
        } else if (id == 7) {
            uint32_t count = 0;
            if (saw_exports_payload || !represented_read_uleb32(&cursor, section_end, &count)
                || count > REPRESENTED_WIRE_MAX_FUNCTIONS)
                goto invalid;
            if (!represented_wire_exports_init(&entry_exports)) goto allocation;
            for (uint32_t i = 0; i < count; ++i) {
                const uint8_t *name = NULL; uint32_t name_count = 0, ignored = 0;
                if (!represented_read_name(&cursor, section_end, &name, &name_count)
                    || cursor == section_end) goto invalid;
                uint8_t kind = *cursor++;
                if (kind > 3 || !represented_read_uleb32(&cursor, section_end, &ignored)) goto invalid;
                if (kind == 1) goto invalid; /* The callback table is private. */
                if (kind == 3 && ignored >= 7 && ignored <= 10) goto invalid;
                if (represented_name_equal(name, name_count, P43_MEMORY)) {
                    if (kind != 2 || saw_memory_export) goto invalid;
                    saw_memory_export = true;
                }
                if (represented_name_equal(name, name_count, SOL_WASM_REPRESENTED_FAILURE_CODE_EXPORT)) {
                    if (kind != 3 || saw_code_export) goto invalid;
                    saw_code_export = true;
                }
                if (represented_name_equal(name, name_count, SOL_WASM_REPRESENTED_FAILURE_SITE_EXPORT)) {
                    if (kind != 3 || saw_site_export) goto invalid;
                    saw_site_export = true;
                }
                bool panic_detail_offset = represented_name_equal(name, name_count,
                    SOL_WASM_REPRESENTED_PANIC_DETAIL_OFFSET_EXPORT);
                bool panic_detail_length = represented_name_equal(name, name_count,
                    SOL_WASM_REPRESENTED_PANIC_DETAIL_LENGTH_EXPORT);
                bool p44_packet_reset_success = represented_name_equal(name, name_count,
                    SOL_WASM_REPRESENTED_TEST_P44_PACKET_RESET_SUCCESS_EXPORT);
                bool p44_packet_reset_nonpanic = represented_name_equal(name, name_count,
                    SOL_WASM_REPRESENTED_TEST_P44_PACKET_RESET_NONPANIC_EXPORT);
                bool trace_offset = represented_name_equal(name, name_count,
                    SOL_WASM_REPRESENTED_TEST_P44_TRACE_OFFSET_EXPORT);
                bool trace_count = represented_name_equal(name, name_count,
                    SOL_WASM_REPRESENTED_TEST_P44_TRACE_COUNT_EXPORT);
                bool trace_overflow = represented_name_equal(name, name_count,
                    SOL_WASM_REPRESENTED_TEST_P44_TRACE_OVERFLOW_EXPORT);
                if (panic_detail_offset) {
                    if (kind != 3 || saw_panic_detail_offset_export) goto invalid;
                    saw_panic_detail_offset_export = true; panic_detail_offset_index = ignored;
                }
                if (panic_detail_length) {
                    if (kind != 3 || saw_panic_detail_length_export) goto invalid;
                    saw_panic_detail_length_export = true; panic_detail_length_index = ignored;
                }
                if (p44_packet_reset_success) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
                    if (!represented_test_p44_packet_reset_probe || kind != 0
                        || saw_p44_packet_reset_success) goto invalid;
                    saw_p44_packet_reset_success = true;
#else
                    goto invalid;
#endif
                }
                if (p44_packet_reset_nonpanic) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
                    if (!represented_test_p44_packet_reset_probe || kind != 0
                        || saw_p44_packet_reset_nonpanic) goto invalid;
                    saw_p44_packet_reset_nonpanic = true;
#else
                    goto invalid;
#endif
                }
                if (trace_offset || trace_count || trace_overflow) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
                    if (!represented_trace_ledger_enabled() || kind != 3) goto invalid;
                    if (trace_offset) { if (saw_trace_offset_export) goto invalid;
                        saw_trace_offset_export = true; trace_offset_index = ignored; }
                    if (trace_count) { if (saw_trace_count_export) goto invalid;
                        saw_trace_count_export = true; trace_count_index = ignored; }
                    if (trace_overflow) { if (saw_trace_overflow_export) goto invalid;
                        saw_trace_overflow_export = true; trace_overflow_index = ignored; }
#else
                    goto invalid;
#endif
                }
                if (name_count >= strlen("sol.p44.") && memcmp(name, "sol.p44.",
                        strlen("sol.p44.")) == 0
                    && !panic_detail_offset && !panic_detail_length
                    && !p44_packet_reset_success && !p44_packet_reset_nonpanic
                    && !trace_offset && !trace_count && !trace_overflow) goto invalid;
                if (represented_name_equal(name, name_count, SOL_WASM_REPRESENTED_TEST_WRITEBACK_EXPORT)) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
                    if (!represented_test_callback_writeback_probe || kind != 3
                        || saw_writeback_export) goto invalid;
                    saw_writeback_export = true;
#else
                    goto invalid;
#endif
                }
                if (represented_name_equal(name, name_count,
                        SOL_WASM_REPRESENTED_TEST_FAILURE_ENTRY_EXPORT)) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
                    if (!represented_test_callback_writeback_probe || kind != 0
                        || saw_failure_probe_export) goto invalid;
                    saw_failure_probe_export = true;
#else
                    goto invalid;
#endif
                }
                if (represented_name_equal(name, name_count,
                        SOL_WASM_REPRESENTED_TEST_CLEANUP_OLD_CALLABLE_EXPORT)) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
                    if (!represented_test_callable_hole_cleanup_probe || kind != 3
                        || saw_cleanup_old_callable_export) goto invalid;
                    saw_cleanup_old_callable_export = true;
#else
                    goto invalid;
#endif
                }
                if (represented_name_equal(name, name_count,
                        SOL_WASM_REPRESENTED_TEST_CLEANUP_MOVED_CALLABLE_EXPORT)) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
                    if (!represented_test_callable_hole_cleanup_probe || kind != 3
                        || saw_cleanup_moved_callable_export) goto invalid;
                    saw_cleanup_moved_callable_export = true;
#else
                    goto invalid;
#endif
                }
                if (represented_name_equal(name, name_count,
                        SOL_WASM_REPRESENTED_TEST_CLEANUP_TEXT_SIBLING_EXPORT)) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
                    if (!represented_test_callable_hole_cleanup_probe || kind != 3
                        || saw_cleanup_text_sibling_export) goto invalid;
                    saw_cleanup_text_sibling_export = true;
#else
                    goto invalid;
#endif
                }
                if (represented_name_equal(name, name_count,
                        SOL_WASM_REPRESENTED_TEST_CLEANUP_ROOT_EXPORT)) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
                    if (!represented_test_callable_hole_cleanup_probe || kind != 3
                        || saw_cleanup_root_export) goto invalid;
                    saw_cleanup_root_export = true;
#else
                    goto invalid;
#endif
                }
                if (kind == 0) {
                    bool test_probe = false;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
                    test_probe = represented_test_callback_writeback_probe
                        && represented_name_equal(name, name_count,
                            SOL_WASM_REPRESENTED_TEST_FAILURE_ENTRY_EXPORT);
                    test_probe = test_probe || (represented_test_p44_packet_reset_probe
                        && (p44_packet_reset_success || p44_packet_reset_nonpanic));
#endif
                    if (!test_probe && (!represented_name_prefix(name, name_count, "sol.e1.")
                            || entry_exports.count >= REPRESENTED_WIRE_MAX_FUNCTIONS)) goto invalid;
                    if (!test_probe) {
                        entry_exports.names[entry_exports.count] = name;
                        entry_exports.counts[entry_exports.count] = name_count;
                        ++entry_exports.count;
                    }
                }
            }
            if (cursor != section_end) goto invalid;
            saw_exports_payload = true;
        } else if (id == 9) {
            uint32_t count = 0;
            if (++element_sections != 1 || !represented_read_uleb32(&cursor, section_end, &count)
                || count != 1 || !represented_read_uleb32(&cursor, section_end, &count) || count != 0
                || cursor == section_end || *cursor++ != UINT8_C(0x41) || cursor == section_end
                || *cursor++ != 1 || cursor == section_end || *cursor++ != UINT8_C(0x0b)
                || !represented_read_uleb32(&cursor, section_end, &element_count)
                || element_count == 0 || element_count > REPRESENTED_WIRE_MAX_TABLE_ELEMENTS - 1
                || element_count != table_initial - 1) goto invalid;
            /* Imports are rejected above, so every function-section index is
             * an internal target.  The fixed bitmap both authenticates the
             * linkage's one-target-per-slot rule and preserves O(1) lookup. */
            uint8_t seen[REPRESENTED_WIRE_MAX_FUNCTIONS] = {0};
            for (uint32_t i = 0, function = 0; i < element_count; ++i)
                if (!represented_read_uleb32(&cursor, section_end, &function)
                    || function >= function_count || seen[function]) goto invalid;
                else {
                    uint32_t type = function_types[function];
                    if (table_function_type_set && type != table_function_type)
                        goto invalid;
                    seen[function] = true;
                    table_function_type = type;
                    table_function_type_set = true;
                }
        } else if (id == 10) {
            uint32_t count = 0;
            if (!represented_read_uleb32(&cursor, section_end, &count)
                || count > REPRESENTED_WIRE_MAX_FUNCTIONS || count != function_count) goto invalid;
        } else if (id == 11) {
            uint32_t count = 0, flags = 0, offset = 0, payload = 0;
            if (!represented_read_uleb32(&cursor, section_end, &count) || count > 1) goto invalid;
            if (count == 1) {
                if (!represented_read_uleb32(&cursor, section_end, &flags) || flags != 0
                    || cursor == section_end || *cursor++ != UINT8_C(0x41)
                    || !represented_read_uleb32(&cursor, section_end, &offset)
                    || cursor == section_end || *cursor++ != UINT8_C(0x0b)
                    || !represented_read_uleb32(&cursor, section_end, &payload)
                    || payload > (size_t)(section_end - cursor)
                    || offset > UINT32_C(65536) || payload > UINT32_C(65536) - offset)
                    goto invalid;
                active_data_end = offset + payload;
                saw_active_data = true;
                cursor += payload;
            }
            if (cursor != section_end) goto invalid;
        } else if (id == 12) {
            goto invalid;
        }
        if (cursor > section_end || ((id == 4 || id == 9) && cursor != section_end))
            goto invalid;
        cursor = section_end;
    }
    bool valid = provenance_sections == 1 && memory_sections == 1
        && global_count >= 11 && global_type[7] == UINT8_C(0x7e) && !global_mutable[7]
        && global_initial[7] != 0 && global_type[8] == UINT8_C(0x7e) && global_mutable[8]
        && global_initial[8] == 0
        && global_type[9] == UINT8_C(0x7f) && !global_mutable[9]
        && global_initial[9] >= 1 && global_initial[9] <= 64
        && global_type[10] == UINT8_C(0x7f) && global_mutable[10] && global_initial[10] == 1
        && ((table_sections == 0 && element_sections == 0)
            || (table_sections == 1 && element_sections == 1)) && saw_memory_export
        && saw_code_export && saw_site_export && saw_provenance_payload && saw_exports_payload
        && (saw_panic_detail_offset_export == saw_panic_detail_length_export)
        && (saw_trace_offset_export == saw_trace_count_export)
        && (saw_trace_offset_export == saw_trace_overflow_export)
        && (provenance_has_panic == saw_panic_detail_offset_export)
        && (!provenance_has_panic || (saw_active_data
            && global_count >= 7 && global_type[2] == UINT8_C(0x7f) && global_mutable[2]
            && heap_base >= active_data_end && heap_base <= UINT32_C(65536)
            && panic_detail_offset_index < global_count && panic_detail_length_index < global_count
            && global_type[panic_detail_offset_index] == UINT8_C(0x7f)
            && !global_mutable[panic_detail_offset_index]
            && global_initial[panic_detail_offset_index] == active_data_end
            && global_type[panic_detail_length_index] == UINT8_C(0x7f)
            && global_mutable[panic_detail_length_index]
            && global_initial[panic_detail_length_index] == 0
            && active_data_end <= UINT32_C(65536) - P44_PANIC_DETAIL_BYTES
            && heap_base >= P44_PANIC_DETAIL_BYTES
            && active_data_end <= heap_base - P44_PANIC_DETAIL_BYTES))
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        && (!represented_trace_ledger_enabled() || (saw_trace_offset_export
            && trace_offset_index < global_count && trace_count_index < global_count
            && trace_overflow_index < global_count && global_type[trace_offset_index] == UINT8_C(0x7f)
            && !global_mutable[trace_offset_index] && global_initial[trace_offset_index]
                == (saw_active_data ? active_data_end : P43_STATIC_BASE) + P44_TRACE_OFFSET_IN_SCRATCH
            && global_type[trace_count_index] == UINT8_C(0x7f) && global_mutable[trace_count_index]
            && global_initial[trace_count_index] == 0 && global_type[trace_overflow_index] == UINT8_C(0x7f)
            && global_mutable[trace_overflow_index] && global_initial[trace_overflow_index] == 0
            && heap_base >= P44_TRACE_OFFSET_IN_SCRATCH + P44_TRACE_BYTES
            && (saw_active_data ? active_data_end : P43_STATIC_BASE)
                <= heap_base - (P44_TRACE_OFFSET_IN_SCRATCH + P44_TRACE_BYTES)))
        && (represented_test_callback_writeback_probe == saw_writeback_export)
        && (represented_test_callback_writeback_probe == saw_failure_probe_export)
        && (represented_test_callable_hole_cleanup_probe == saw_cleanup_old_callable_export)
        && (represented_test_callable_hole_cleanup_probe == saw_cleanup_moved_callable_export)
        && (represented_test_callable_hole_cleanup_probe == saw_cleanup_text_sibling_export)
        && (represented_test_callable_hole_cleanup_probe == saw_cleanup_root_export)
        && (saw_p44_packet_reset_success
            == (represented_test_p44_packet_reset_probe && provenance_has_panic))
        && (saw_p44_packet_reset_nonpanic
            == (represented_test_p44_packet_reset_probe && provenance_has_panic))
        && (represented_trace_ledger_enabled() == saw_trace_offset_export)
#else
        && !saw_writeback_export && !saw_failure_probe_export
        && !saw_cleanup_old_callable_export && !saw_cleanup_moved_callable_export
        && !saw_cleanup_text_sibling_export && !saw_cleanup_root_export
        && !saw_p44_packet_reset_success && !saw_p44_packet_reset_nonpanic
        && !saw_trace_offset_export && !saw_trace_count_export && !saw_trace_overflow_export
#endif
        && represented_exports_match_provenance(&entry_exports, &provenance_entries);
    represented_wire_entry_map_free(&provenance_entries);
    represented_wire_exports_free(&entry_exports);
    return valid ? REPRESENTED_WIRE_VALID : REPRESENTED_WIRE_INVALID;
allocation:
    represented_wire_entry_map_free(&provenance_entries);
    represented_wire_exports_free(&entry_exports);
    return REPRESENTED_WIRE_ALLOCATION;
invalid:
    represented_wire_entry_map_free(&provenance_entries);
    represented_wire_exports_free(&entry_exports);
    return REPRESENTED_WIRE_INVALID;
}

SolWasmRepresentedResult sol_wasm_represented_validate(const SolWasmBackendBytes *bytes) {
    if (bytes == NULL || bytes->bytes == NULL || bytes->count == 0)
        return SOL_WASM_REPRESENTED_INVALID_ARGUMENT;
    RepresentedWireValidation validation = represented_module_shape_validate(bytes);
    if (validation == REPRESENTED_WIRE_ALLOCATION) return SOL_WASM_REPRESENTED_ALLOCATION_FAILED;
    if (validation != REPRESENTED_WIRE_VALID) return SOL_WASM_REPRESENTED_INVALID_INPUT;
    return wasmtime_validate(bytes) ? SOL_WASM_REPRESENTED_OK
        : SOL_WASM_REPRESENTED_WASMTIME_VALIDATION_FAILED;
}

SolWasmRepresentedResult sol_wasm_represented_build(
    const SolWasmRepresentedBuildRequest *request, SolWasmRepresentedOutput *output,
    SolDiagnostics *diagnostics) {
    if (output == NULL) return SOL_WASM_REPRESENTED_INVALID_ARGUMENT;
    sol_wasm_represented_output_free(output);
    /* The fault point is deliberately relative to this invocation.  In
     * particular, a failed build cannot poison a later retry or turn the
     * allocator hook into process-global state. */
    allocation_attempts = 0;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    represented_test_qualified_certifications = 0;
    memset(&represented_test_control_transition_stats, 0,
        sizeof represented_test_control_transition_stats);
    memset(&represented_test_refinement_index_stats, 0,
        sizeof represented_test_refinement_index_stats);
#endif
    if (request == NULL || request->program == NULL || request->package_directory == NULL
        || (request->limits != NULL && !limits_zero(request->limits)
            && !limits_complete(request->limits))) {
        diagnostic(diagnostics, "invalid represented Wasm build request");
        return SOL_WASM_REPRESENTED_INVALID_ARGUMENT;
    }
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (represented_test_max_pages == SIZE_MAX) {
        diagnostic(diagnostics, "invalid represented Wasm test memory page cap");
        return SOL_WASM_REPRESENTED_INVALID_ARGUMENT;
    }
#endif
    SolWasmRepresentedLimits limits = request->limits == NULL || limits_zero(request->limits)
        ? sol_wasm_represented_default_limits() : *request->limits;
    SolWasmRepresentedUsage usage = {0};
    if (!represented_owner(request, &usage)) {
        diagnostic(diagnostics, "P4.3 requires an authenticated represented-only closure");
        return SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE;
    }
    if (!represented_usage_census(request, &usage)) {
        diagnostic(diagnostics, "P4.3 represented resource census overflowed or was inconsistent");
        return SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED;
    }
    if (!represented_snapshots_preflight(request)) {
        diagnostic(diagnostics, "P4.4 C2b1 rejected an unauthenticated snapshot closure");
        return SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE;
    }
    if (!represented_cleanup(request)) {
        diagnostic(diagnostics, "P4.3 Slice 1 rejected non-represented cleanup");
        return SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE;
    }
    const SolMirLinkage *linkage = &request->program->conventions->concrete->linkage;
    const SolMirRuntimeConventions *conventions = request->program->conventions;
    const SolMirConcreteProgram *concrete = conventions->concrete;
    bool fixed_products = represented_fixed_products_needed(request);
    bool function_table = represented_function_table_needed(request);
    bool panic_detail = represented_panic_detail_needed(request);
    if (function_table && (!represented_function_table_preflight(request)
            || !represented_add(usage.table_elements, concrete->linkage.table_entry_count,
                &usage.table_elements)
            || !represented_add(usage.table_elements, 1, &usage.table_elements))) {
        diagnostic(diagnostics, "P4.3 rejected an unauthenticated unbound function table");
        return SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE;
    }
    for (size_t i = 0; i < linkage->callable_count; ++i) {
        size_t locals = 0;
        if (!represented_function_local_count(request, i, signature_for(conventions, i), &locals)
            || !represented_add(usage.locals, locals, &usage.locals)) {
            diagnostic(diagnostics, "P4.3 represented local census overflowed");
            return SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED;
        }
    }
    /* Text copy/equality own three parameters each; the meter owns one. */
    if (!represented_add(usage.functions, 3, &usage.functions)
        || !represented_add(usage.locals, 24, &usage.locals)) {
        diagnostic(diagnostics, "P4.3 represented helper census overflowed");
        return SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED;
    }
    /* Two i64 parameters plus six declared locals are eight physical slots in
     * the census convention.  The helper is emitted iff PANIC provenance is
     * present and consequently participates in every function/local limit. */
    if (panic_detail && (!represented_add(usage.functions, 1, &usage.functions)
            || !represented_add(usage.locals, 8, &usage.locals))) {
        diagnostic(diagnostics, "P4.4 panic-detail helper census overflowed");
        return SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED;
    }
    if (fixed_products && (!represented_add(usage.functions, 1, &usage.functions)
            || !represented_add(usage.locals, 9, &usage.locals))) {
        diagnostic(diagnostics, "P4.3 fixed-product helper census overflowed");
        return SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED;
    }
    for (size_t recipe = 0; recipe < concrete->representation.recipe_count; ++recipe) {
        if (represented_product_copy_needed(request, recipe)
            && (!represented_add(usage.functions, 1, &usage.functions)
                || !represented_add(usage.locals, 5, &usage.locals))) {
            diagnostic(diagnostics, "P4.3 product-copy helper census overflowed");
            return SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED;
        }
        if (represented_product_equal_needed(request, recipe)
            && (!represented_add(usage.functions, 1, &usage.functions)
                || !represented_add(usage.locals, 4, &usage.locals))) {
            diagnostic(diagnostics, "P4.3 product-equality helper census overflowed");
            return SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED;
        }
        if (represented_sum_copy_needed(request, recipe)
            && (!represented_add(usage.functions, 1, &usage.functions)
                || !represented_add(usage.locals, 6, &usage.locals))) {
            diagnostic(diagnostics, "P4.3 sum-copy helper census overflowed");
            return SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED;
        }
        if (represented_sum_equal_needed(request, recipe)
            && (!represented_add(usage.functions, 1, &usage.functions)
                || !represented_add(usage.locals, 5, &usage.locals))) {
            diagnostic(diagnostics, "P4.3 sum-equality helper census overflowed");
            return SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED;
        }
    }
    if (usage.functions > REPRESENTED_WIRE_MAX_FUNCTIONS
        || usage.table_elements > REPRESENTED_WIRE_MAX_TABLE_ELEMENTS
        || usage.provenance_records > REPRESENTED_WIRE_MAX_PROVENANCE_RECORDS
        || usage.functions > limits.max_functions || usage.blocks > limits.max_blocks
        || usage.edges > limits.max_edges || usage.values > limits.max_values
        || usage.locals > limits.max_locals
        || usage.table_elements > limits.max_table_elements
        || usage.provenance_records > limits.max_provenance_records) {
        diagnostic(diagnostics, "P4.3 represented Wasm resource limit exceeded");
        return SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED;
    }
    RepresentedAccounting accounting = {&limits, &usage, 0, 0, REPRESENTED_BACKEND_OK};
    represented_accounting = &accounting;
    RepresentedQualifiedIndex qualified_index = {0};
    RepresentedQualifiedIndexResult qualified_result = represented_qualified_index_build(request,
        &qualified_index);
    if (qualified_result != REPRESENTED_QUALIFIED_INDEX_VALID) {
        diagnostic(diagnostics, qualified_result == REPRESENTED_QUALIFIED_INDEX_RESOURCE
            ? "P4.4 C2b2 qualified index resource limit exceeded"
            : qualified_result == REPRESENTED_QUALIFIED_INDEX_ALLOCATION
                ? "P4.4 C2b2 qualified index allocation failed"
                : "P4.4 C2b2 rejected an unauthenticated qualified Result closure");
        SolWasmRepresentedResult result = qualified_result == REPRESENTED_QUALIFIED_INDEX_RESOURCE
            ? SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED
            : qualified_result == REPRESENTED_QUALIFIED_INDEX_ALLOCATION
                ? represented_backend_result() : SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE;
        represented_accounting = NULL;
        return result;
    }
    RepresentedCallCatalogGraph catalog = {0};
    const RepresentedCallCatalogGraph *active_catalog = NULL;
    if (request->program->conventions->call_count != 0) {
        RepresentedCatalogResult catalog_result = represented_call_catalog(request, &limits,
            &qualified_index, &catalog);
        if (catalog_result == REPRESENTED_CATALOG_RESOURCE) {
            diagnostic(diagnostics, "P4.3 represented call catalog resource limit exceeded");
            represented_qualified_index_free(&qualified_index);
            represented_accounting = NULL; return SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED;
        }
        if (catalog_result == REPRESENTED_CATALOG_ALLOCATION) {
            diagnostic(diagnostics, "P4.3 represented call catalog allocation failed");
            SolWasmRepresentedResult result = represented_backend_result();
            represented_qualified_index_free(&qualified_index);
            represented_accounting = NULL; return result;
        }
        if (catalog_result != REPRESENTED_CATALOG_VALID) {
            diagnostic(diagnostics, "P4.3 rejected an unauthenticated represented call catalog");
            SolWasmRepresentedResult result = accounting.status == REPRESENTED_BACKEND_OK
                ? SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE : represented_backend_result();
            represented_qualified_index_free(&qualified_index);
            represented_accounting = NULL; return result;
        }
        if (!represented_catalog_failures_supported(request)) {
            represented_call_catalog_free(&catalog);
            diagnostic(diagnostics, "P4.3 deferred an unsupported call failure closure");
            represented_qualified_index_free(&qualified_index);
            represented_accounting = NULL; return SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE;
        }
        active_catalog = &catalog;
    }
    RepresentedCallableOrder *callables = linkage->callable_count == 0 ? NULL
        : allocate(linkage->callable_count, sizeof *callables);
    RepresentedEntryOrder *entries = conventions->entry_count == 0 ? NULL
        : allocate(conventions->entry_count, sizeof *entries);
    if ((linkage->callable_count != 0 && callables == NULL)
        || (conventions->entry_count != 0 && entries == NULL)) {
        deallocate(callables); deallocate(entries); represented_call_catalog_free(&catalog);
        SolWasmRepresentedResult result = represented_backend_result();
        represented_qualified_index_free(&qualified_index);
        represented_accounting = NULL; return result;
    }
    for (size_t i = 0; i < linkage->callable_count; ++i) {
        callables[i] = (RepresentedCallableOrder){i, linkage};
        const SolMirRuntimeSignature *signature = signature_for(conventions, i);
        if (!represented_function_preflight(request, i, signature, active_catalog,
                &qualified_index)) {
            deallocate(callables); deallocate(entries); represented_call_catalog_free(&catalog);
            diagnostic(diagnostics, "P4.3 Slice 1 rejected a non-infallible represented function");
            represented_qualified_index_free(&qualified_index);
            represented_accounting = NULL; return SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE;
        }
    }
    for (size_t i = 0; i < conventions->entry_count; ++i) {
        entries[i] = (RepresentedEntryOrder){i, conventions};
        if (conventions->entries[i].callable >= linkage->callable_count) {
            deallocate(callables); deallocate(entries); represented_call_catalog_free(&catalog);
            represented_qualified_index_free(&qualified_index);
            represented_accounting = NULL; return SOL_WASM_REPRESENTED_INVALID_INPUT;
        }
    }
    qsort(callables, linkage->callable_count, sizeof *callables, callable_order_compare);
    qsort(entries, conventions->entry_count, sizeof *entries, entry_order_compare);
    for (size_t i = 1; i < linkage->callable_count; ++i)
        if (callable_order_compare(&callables[i - 1], &callables[i]) == 0) {
            deallocate(callables); deallocate(entries); represented_call_catalog_free(&catalog);
            represented_qualified_index_free(&qualified_index);
            represented_accounting = NULL; return SOL_WASM_REPRESENTED_INVALID_INPUT;
        }
    for (size_t i = 1; i < conventions->entry_count; ++i)
        if (entry_order_compare(&entries[i - 1], &entries[i]) == 0) {
            deallocate(callables); deallocate(entries); represented_call_catalog_free(&catalog);
            represented_qualified_index_free(&qualified_index);
            represented_accounting = NULL; return SOL_WASM_REPRESENTED_INVALID_INPUT;
        }
    RepresentedLiteral *literals = NULL;
    uint8_t *static_data = NULL;
    size_t literal_count = 0, static_size = 0;
    uint32_t heap_base = 0;
    uint32_t panic_detail_offset = 0;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    uint32_t trace_offset = 0;
#endif
    if (!represented_static_literals(request, &literals, &literal_count, &static_data,
            &static_size, &heap_base) || static_size > limits.max_static_data_bytes) {
        deallocate(literals); deallocate(static_data); deallocate(callables); deallocate(entries);
        represented_call_catalog_free(&catalog);
        SolWasmRepresentedResult result = accounting.status == REPRESENTED_BACKEND_OK
            ? SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED : represented_backend_result();
        represented_qualified_index_free(&qualified_index);
        represented_accounting = NULL; return result;
    }
    if (panic_detail && (static_size > UINT32_MAX - P43_STATIC_BASE
            || (panic_detail_offset = (uint32_t)(P43_STATIC_BASE + static_size)) > UINT32_C(65536)
            || P44_PANIC_DETAIL_BYTES > UINT32_C(65536) - panic_detail_offset)) {
        deallocate(static_data); deallocate(literals); deallocate(callables); deallocate(entries);
        represented_call_catalog_free(&catalog); represented_qualified_index_free(&qualified_index);
        represented_accounting = NULL;
        return SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED;
    }
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (represented_trace_ledger_enabled()
        && (static_size > UINT32_MAX - P43_STATIC_BASE
            || (trace_offset = (uint32_t)(P43_STATIC_BASE + static_size)) > UINT32_MAX
                - P44_TRACE_OFFSET_IN_SCRATCH
            || trace_offset + P44_TRACE_OFFSET_IN_SCRATCH > UINT32_C(65536)
                - P44_TRACE_BYTES)) {
        deallocate(static_data); deallocate(literals); deallocate(callables); deallocate(entries);
        represented_call_catalog_free(&catalog); represented_qualified_index_free(&qualified_index);
        represented_accounting = NULL;
        return SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED;
    }
    trace_offset += P44_TRACE_OFFSET_IN_SCRATCH;
#endif
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (represented_test_heap_base != 0) {
        if (represented_test_heap_base < heap_base) {
            deallocate(static_data); deallocate(literals); deallocate(callables); deallocate(entries);
            represented_call_catalog_free(&catalog); represented_qualified_index_free(&qualified_index);
            represented_accounting = NULL;
            return SOL_WASM_REPRESENTED_INVALID_INPUT;
        }
        heap_base = represented_test_heap_base;
    }
#endif
    if (panic_detail && (heap_base > UINT32_C(65536) || panic_detail_offset > heap_base
            || P44_PANIC_DETAIL_BYTES > heap_base - panic_detail_offset)) {
        deallocate(static_data); deallocate(literals); deallocate(callables); deallocate(entries);
        represented_call_catalog_free(&catalog); represented_qualified_index_free(&qualified_index);
        represented_accounting = NULL;
        return SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED;
    }
    usage.static_data_bytes = static_size;
    RepresentedProvenance provenance = {0};
    if (!represented_provenance_build(request, &provenance)) {
        deallocate(static_data); deallocate(literals); deallocate(callables); deallocate(entries);
        represented_call_catalog_free(&catalog);
        SolWasmRepresentedResult result = represented_backend_result();
        represented_qualified_index_free(&qualified_index);
        represented_accounting = NULL; return result;
    }
    BinaryenModuleRef module = BinaryenModuleCreate();
    if (module == NULL) { represented_provenance_free(&provenance); deallocate(static_data); deallocate(literals); deallocate(callables); deallocate(entries); represented_call_catalog_free(&catalog); represented_qualified_index_free(&qualified_index); represented_accounting = NULL; return SOL_WASM_REPRESENTED_ALLOCATION_FAILED; }
    BinaryenModuleSetFeatures(module, BinaryenFeatureMVP() | BinaryenFeatureMutableGlobals());
    if (function_table && BinaryenAddTable(module, P43_TABLE, (BinaryenIndex)usage.table_elements,
            (BinaryenIndex)usage.table_elements, BinaryenTypeFuncref(), NULL) == NULL) {
        BinaryenModuleDispose(module); represented_provenance_free(&provenance); deallocate(static_data); deallocate(literals);
        deallocate(callables); deallocate(entries); represented_call_catalog_free(&catalog);
        represented_qualified_index_free(&qualified_index);
        represented_accounting = NULL; return SOL_WASM_REPRESENTED_ALLOCATION_FAILED;
    }
    const char *segment_names[] = {"sol.p43.static"};
    const char *segment_data[] = {(const char *)static_data};
    bool segment_passive[] = {false};
    BinaryenIndex segment_size = (BinaryenIndex)static_size;
    BinaryenExpressionRef segment_offset = static_size == 0 ? NULL
        : BinaryenConst(module, BinaryenLiteralInt32(P43_STATIC_BASE));
    BinaryenSetMemory(module, 1,
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        represented_test_max_pages == 0 ? 256 : (BinaryenIndex)represented_test_max_pages,
#else
        256,
#endif
        NULL, static_size == 0 ? NULL : segment_names,
        static_size == 0 ? NULL : segment_data, static_size == 0 ? NULL : segment_passive,
        static_size == 0 ? NULL : &segment_offset, static_size == 0 ? NULL : &segment_size,
        static_size == 0 ? 0 : 1, false, false, P43_MEMORY);
    BinaryenAddMemoryExport(module, P43_MEMORY, P43_MEMORY);
    bool ok = BinaryenAddGlobal(module, P43_CODE, BinaryenTypeInt32(), true,
        BinaryenConst(module, BinaryenLiteralInt32(0))) != NULL
        && BinaryenAddGlobal(module, P43_SITE, BinaryenTypeInt32(), true,
        BinaryenConst(module, BinaryenLiteralInt32(0))) != NULL
        && BinaryenAddGlobal(module, P43_HEAP, BinaryenTypeInt32(), true,
        BinaryenConst(module, BinaryenLiteralInt32((int32_t)heap_base))) != NULL
        && BinaryenAddGlobal(module, P43_MAX_REQUESTS, BinaryenTypeInt64(), false,
        BinaryenConst(module, BinaryenLiteralInt64((int64_t)(represented_test_max_requests == UINT64_MAX
            ? limits.max_allocation_requests : represented_test_max_requests)))) != NULL
        && BinaryenAddGlobal(module, P43_MAX_BYTES, BinaryenTypeInt64(), false,
        BinaryenConst(module, BinaryenLiteralInt64((int64_t)(represented_test_max_bytes == UINT64_MAX
            ? limits.max_allocation_bytes : represented_test_max_bytes)))) != NULL
        && BinaryenAddGlobal(module, P43_REQUESTS, BinaryenTypeInt64(), true,
        BinaryenConst(module, BinaryenLiteralInt64(0))) != NULL
        && BinaryenAddGlobal(module, P43_BYTES, BinaryenTypeInt64(), true,
        BinaryenConst(module, BinaryenLiteralInt64(0))) != NULL
        && BinaryenAddGlobal(module, P44_MAX_STEPS, BinaryenTypeInt64(), false,
        BinaryenConst(module, BinaryenLiteralInt64((int64_t)limits.max_steps))) != NULL
        && BinaryenAddGlobal(module, P44_STEPS, BinaryenTypeInt64(), true,
        BinaryenConst(module, BinaryenLiteralInt64(0))) != NULL
        && BinaryenAddGlobal(module, P44_MAX_CALL_DEPTH, BinaryenTypeInt32(), false,
        BinaryenConst(module, BinaryenLiteralInt32((int32_t)limits.max_call_depth))) != NULL
        && BinaryenAddGlobal(module, P44_CALL_DEPTH, BinaryenTypeInt32(), true,
        BinaryenConst(module, BinaryenLiteralInt32(1))) != NULL
        && (!panic_detail || (BinaryenAddGlobal(module, P44_PANIC_DETAIL_OFFSET,
                BinaryenTypeInt32(), false, BinaryenConst(module,
                    BinaryenLiteralInt32((int32_t)panic_detail_offset))) != NULL
             && BinaryenAddGlobal(module, P44_PANIC_DETAIL_LENGTH, BinaryenTypeInt32(), true,
                BinaryenConst(module, BinaryenLiteralInt32(0))) != NULL));
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (ok && represented_trace_ledger_enabled())
        ok = BinaryenAddGlobal(module, P44_TRACE_OFFSET, BinaryenTypeInt32(), false,
                BinaryenConst(module, BinaryenLiteralInt32((int32_t)trace_offset))) != NULL
            && BinaryenAddGlobal(module, P44_TRACE_COUNT, BinaryenTypeInt32(), true,
                BinaryenConst(module, BinaryenLiteralInt32(0))) != NULL
            && BinaryenAddGlobal(module, P44_TRACE_OVERFLOW, BinaryenTypeInt32(), true,
                BinaryenConst(module, BinaryenLiteralInt32(0))) != NULL;
    if (ok && represented_test_callback_writeback_probe)
        ok = BinaryenAddGlobal(module, P43_WRITEBACKS, BinaryenTypeInt32(), true,
            BinaryenConst(module, BinaryenLiteralInt32(0))) != NULL;
    if (ok && represented_test_callable_hole_cleanup_probe) {
        const char *const cleanup_counters[] = {P43_CLEANUP_OLD_CALLABLE,
            P43_CLEANUP_MOVED_CALLABLE, P43_CLEANUP_TEXT_SIBLING, P43_CLEANUP_ROOT};
        for (size_t i = 0; ok && i < sizeof cleanup_counters / sizeof *cleanup_counters; ++i)
            ok = BinaryenAddGlobal(module, cleanup_counters[i], BinaryenTypeInt32(), true,
                BinaryenConst(module, BinaryenLiteralInt32(0))) != NULL;
    }
#endif
    if (ok) {
        BinaryenAddGlobalExport(module, P43_CODE, SOL_WASM_REPRESENTED_FAILURE_CODE_EXPORT);
        BinaryenAddGlobalExport(module, P43_SITE, SOL_WASM_REPRESENTED_FAILURE_SITE_EXPORT);
        if (panic_detail) {
            BinaryenAddGlobalExport(module, P44_PANIC_DETAIL_OFFSET,
                SOL_WASM_REPRESENTED_PANIC_DETAIL_OFFSET_EXPORT);
            BinaryenAddGlobalExport(module, P44_PANIC_DETAIL_LENGTH,
                SOL_WASM_REPRESENTED_PANIC_DETAIL_LENGTH_EXPORT);
        }
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        if (represented_trace_ledger_enabled()) {
            BinaryenAddGlobalExport(module, P44_TRACE_OFFSET,
                SOL_WASM_REPRESENTED_TEST_P44_TRACE_OFFSET_EXPORT);
            BinaryenAddGlobalExport(module, P44_TRACE_COUNT,
                SOL_WASM_REPRESENTED_TEST_P44_TRACE_COUNT_EXPORT);
            BinaryenAddGlobalExport(module, P44_TRACE_OVERFLOW,
                SOL_WASM_REPRESENTED_TEST_P44_TRACE_OVERFLOW_EXPORT);
        }
        if (represented_test_callback_writeback_probe)
            BinaryenAddGlobalExport(module, P43_WRITEBACKS,
                SOL_WASM_REPRESENTED_TEST_WRITEBACK_EXPORT);
        if (represented_test_callable_hole_cleanup_probe) {
            BinaryenAddGlobalExport(module, P43_CLEANUP_OLD_CALLABLE,
                SOL_WASM_REPRESENTED_TEST_CLEANUP_OLD_CALLABLE_EXPORT);
            BinaryenAddGlobalExport(module, P43_CLEANUP_MOVED_CALLABLE,
                SOL_WASM_REPRESENTED_TEST_CLEANUP_MOVED_CALLABLE_EXPORT);
            BinaryenAddGlobalExport(module, P43_CLEANUP_TEXT_SIBLING,
                SOL_WASM_REPRESENTED_TEST_CLEANUP_TEXT_SIBLING_EXPORT);
            BinaryenAddGlobalExport(module, P43_CLEANUP_ROOT,
                SOL_WASM_REPRESENTED_TEST_CLEANUP_ROOT_EXPORT);
        }
#endif
    }
    if (ok) ok = represented_step_function(module);
    if (ok) ok = represented_text_copy_function(module);
    if (ok) ok = represented_text_equal_function(module);
    if (ok && panic_detail) ok = represented_panic_capture_function(module);
    if (ok && fixed_products) ok = represented_fixed_alloc_function(module);
    for (size_t recipe = 0; ok && recipe < concrete->representation.recipe_count; ++recipe)
        if (represented_product_copy_needed(request, recipe))
            ok = represented_product_copy_function(module, concrete, recipe);
    for (size_t recipe = 0; ok && recipe < concrete->representation.recipe_count; ++recipe)
        if (represented_product_equal_needed(request, recipe))
            ok = represented_product_equal_function(module, concrete, recipe);
    for (size_t recipe = 0; ok && recipe < concrete->representation.recipe_count; ++recipe)
        if (represented_sum_copy_needed(request, recipe)) {
            ok = represented_sum_copy_function(module, concrete, recipe);
        }
    for (size_t recipe = 0; ok && recipe < concrete->representation.recipe_count; ++recipe)
        if (represented_sum_equal_needed(request, recipe)) {
            ok = represented_sum_equal_function(module, concrete, recipe);
        }
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (ok && represented_test_inactive_payload_probe)
        ok = represented_inactive_payload_probe_functions(module, concrete);
#endif
    for (size_t i = 0; ok && i < linkage->callable_count; ++i) {
        size_t callable = callables[i].callable;
        ok = represented_function_emit(request, module, callable,
            signature_for(conventions, callable), linkage->callables[callable].symbol.bytes,
            active_catalog, &qualified_index, literals, literal_count, &provenance);
    }
    for (size_t i = 0; ok && i < conventions->entry_count; ++i) {
        const SolMirRuntimeEntry *entry = &conventions->entries[entries[i].entry];
        if (entry->callable >= linkage->callable_count) { ok = false; break; }
        ok = represented_entry_wrapper_emit(module, entry, linkage, conventions, heap_base,
            panic_detail);
    }
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (ok && represented_test_p44_packet_reset_probe && panic_detail)
        ok = represented_p44_packet_reset_probe_functions(module, heap_base);
    if (ok && represented_test_callback_writeback_probe)
        ok = represented_callback_writeback_failure_export(module, conventions, linkage);
#endif
    if (ok && function_table) {
        const char **functions = allocate(concrete->linkage.table_entry_count, sizeof *functions);
        if (functions == NULL) ok = false;
        for (size_t i = 0; ok && i < concrete->linkage.table_entry_count; ++i) {
            const SolMirLinkageTableEntry *entry = &concrete->linkage.table_entries[i];
            if (entry->target_kind != SOL_MIR_LINKAGE_TARGET_INTERNAL
                || entry->internal >= concrete->linkage.callable_count) ok = false;
            else functions[i] = concrete->linkage.callables[entry->internal].symbol.bytes;
        }
        BinaryenExpressionRef offset = ok ? BinaryenConst(module, BinaryenLiteralInt32(1)) : NULL;
        if (ok) ok = BinaryenAddActiveElementSegment(module, P43_TABLE, "sol.p43.functions.elements",
            functions, (BinaryenIndex)concrete->linkage.table_entry_count, offset) != NULL;
        deallocate(functions);
    }
    if (ok) BinaryenAddCustomSection(module, SOL_WASM_REPRESENTED_PROVENANCE_SECTION,
        (const char *)provenance.bytes, (BinaryenIndex)provenance.byte_count);
    represented_provenance_free(&provenance); deallocate(callables); deallocate(entries);
    represented_call_catalog_free(&catalog); represented_qualified_index_free(&qualified_index);
    if (accounting.status != REPRESENTED_BACKEND_OK) {
        BinaryenModuleDispose(module);
        deallocate(static_data); deallocate(literals);
        SolWasmRepresentedResult result = represented_backend_result();
        represented_accounting = NULL; return result;
    }
    if (!ok || !BinaryenModuleValidate(module)) {
        BinaryenModuleDispose(module);
        deallocate(static_data); deallocate(literals);
        diagnostic(diagnostics, "Binaryen rejected represented Wasm module");
        represented_accounting = NULL; return SOL_WASM_REPRESENTED_BINARYEN_VALIDATION_FAILED;
    }
    BinaryenModuleAllocateAndWriteResult serialized = BinaryenModuleAllocateAndWrite(module, NULL);
    BinaryenModuleDispose(module);
    deallocate(static_data); deallocate(literals);
    if (serialized.binary == NULL || serialized.binaryBytes == 0) {
        free(serialized.binary);
        represented_accounting = NULL; return SOL_WASM_REPRESENTED_SERIALIZATION_FAILED;
    }
    if (serialized.binaryBytes > limits.max_output_bytes) {
        free(serialized.binary);
        represented_accounting = NULL; return SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED;
    }
    if (!represented_transfer_output(&accounting, serialized.binaryBytes)) {
        free(serialized.binary);
        represented_accounting = NULL; return SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED;
    }
    SolWasmBackendBytes bytes = {(uint8_t *)serialized.binary, serialized.binaryBytes};
    RepresentedWireValidation wire_validation = represented_module_shape_validate(&bytes);
    if (wire_validation != REPRESENTED_WIRE_VALID
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        && !represented_test_inactive_payload_probe
#endif
        ) {
        free(serialized.binary);
        diagnostic(diagnostics, "P4.3 private module-shape validation rejected emitted Wasm");
        SolWasmRepresentedResult result = wire_validation == REPRESENTED_WIRE_ALLOCATION
            ? represented_backend_result() : SOL_WASM_REPRESENTED_INVALID_INPUT;
        represented_accounting = NULL; return result;
    }
    if (!wasmtime_validate(&bytes)) {
        free(serialized.binary);
        diagnostic(diagnostics, "Wasmtime rejected represented Wasm module");
        represented_accounting = NULL; return SOL_WASM_REPRESENTED_WASMTIME_VALIDATION_FAILED;
    }
    usage.output_bytes = bytes.count;
    output->bytes = bytes;
    output->usage = usage;
    represented_accounting = NULL;
    return SOL_WASM_REPRESENTED_OK;
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
SolWasmRepresentedResult sol_wasm_represented_test_call_catalog(
    const SolWasmRepresentedBuildRequest *request, SolWasmRepresentedTestCallCatalogEntry *entries,
    size_t cap, SolWasmRepresentedTestCallCatalog *summary) {
    if (summary == NULL || (cap != 0 && entries == NULL) || request == NULL
        || request->program == NULL || request->package_directory == NULL
        || (request->limits != NULL && !limits_zero(request->limits)
            && !limits_complete(request->limits))) return SOL_WASM_REPRESENTED_INVALID_ARGUMENT;
    SolWasmRepresentedLimits limits = request->limits == NULL || limits_zero(request->limits)
        ? sol_wasm_represented_default_limits() : *request->limits;
    SolWasmRepresentedUsage ignored = {0};
    memset(summary, 0, sizeof *summary);
    if (!represented_owner(request, &ignored) || !represented_cleanup(request))
        return SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE;
    RepresentedQualifiedIndex qualified_index;
    RepresentedQualifiedIndexResult qualified_result = represented_qualified_index_build(request,
        &qualified_index);
    if (qualified_result == REPRESENTED_QUALIFIED_INDEX_RESOURCE)
        return SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED;
    if (qualified_result == REPRESENTED_QUALIFIED_INDEX_ALLOCATION)
        return SOL_WASM_REPRESENTED_ALLOCATION_FAILED;
    if (qualified_result != REPRESENTED_QUALIFIED_INDEX_VALID)
        return SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE;
    RepresentedCallCatalogGraph catalog;
    RepresentedCatalogResult result = represented_call_catalog(request, &limits, &qualified_index,
        &catalog);
    if (result == REPRESENTED_CATALOG_RESOURCE) {
        represented_qualified_index_free(&qualified_index);
        return SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED;
    }
    if (result == REPRESENTED_CATALOG_ALLOCATION) {
        represented_qualified_index_free(&qualified_index);
        return SOL_WASM_REPRESENTED_ALLOCATION_FAILED;
    }
    if (result != REPRESENTED_CATALOG_VALID) {
        represented_qualified_index_free(&qualified_index);
        return SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE;
    }
    summary->calls = catalog.count;
    summary->edges = catalog.edge_count;
    summary->work_bytes = catalog.work_bytes;
    summary->longest_chain = catalog.longest_chain;
    summary->has_cycle = catalog.has_cycle;
    if (entries == NULL && cap == 0) {
        represented_call_catalog_free(&catalog);
        represented_qualified_index_free(&qualified_index);
        return SOL_WASM_REPRESENTED_OK;
    }
    if (cap < catalog.count) {
        represented_call_catalog_free(&catalog);
        represented_qualified_index_free(&qualified_index);
        return SOL_WASM_REPRESENTED_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < catalog.count; ++i) {
        const RepresentedCallCatalog *call = &catalog.calls[i];
        entries[i] = (SolWasmRepresentedTestCallCatalogEntry){
            call->caller_image, call->caller_block, call->call, call->callee_callable,
            call->signature, call->operands.count, call->result, call->result_class,
            call->normal_edge, call->failure_edge, call->failure_site,
            call->cyclic, call->chain_depth};
    }
    represented_call_catalog_free(&catalog);
    represented_qualified_index_free(&qualified_index);
    return SOL_WASM_REPRESENTED_OK;
}

bool sol_wasm_represented_test_call_catalog_operand(const SolWasmRepresentedBuildRequest *request,
    size_t call, size_t ordinal, SolMirMaterializedTemporaryId *temporary) {
    if (temporary == NULL) return false;
    SolWasmRepresentedTestCallCatalog summary;
    SolWasmRepresentedResult result = sol_wasm_represented_test_call_catalog(request, NULL, 0, &summary);
    if (result != SOL_WASM_REPRESENTED_OK) return false;
    if (request == NULL || request->program == NULL || call >= summary.calls) return false;
    const SolMirRuntimeConventions *conventions = request->program->conventions;
    if (call >= conventions->call_count) return false;
    const SolMirRuntimeCall *row = &conventions->calls[call];
    if (row->operands.offset > conventions->operand_count
        || row->operands.count > conventions->operand_count - row->operands.offset
        || ordinal >= row->operands.count) return false;
    const SolMirRuntimeOperand *operand = &conventions->operands[row->operands.offset + ordinal];
    if (operand->value.kind != SOL_MIR_RUNTIME_VALUE_MATERIALIZED_TEMPORARY) return false;
    *temporary = operand->value.id;
    return true;
}

void sol_wasm_represented_test_fail_allocation_after(size_t attempt) { fail_allocation_after = attempt; }
size_t sol_wasm_represented_test_allocation_attempts(void) { return allocation_attempts; }
size_t sol_wasm_represented_test_qualified_certifications(void) {
    return represented_test_qualified_certifications;
}
SolWasmRepresentedTestControlTransitionStats
sol_wasm_represented_test_control_transition_stats(void) {
    return represented_test_control_transition_stats;
}
SolWasmRepresentedTestRefinementIndexStats
sol_wasm_represented_test_refinement_index_stats(void) {
    return represented_test_refinement_index_stats;
}
bool sol_wasm_represented_test_parallel_moves(void) {
    BinaryenModuleRef module = BinaryenModuleCreate();
    if (module == NULL) return false;
    BinaryenModuleSetFeatures(module, BinaryenFeatureMVP());
    BinaryenType parameters[] = {BinaryenTypeInt64(), BinaryenTypeInt64()};
    RepresentedNodes body = {0};
    BinaryenIndex destinations[] = {2, 3};
    BinaryenExpressionRef swap[] = {BinaryenLocalGet(module, 3, BinaryenTypeInt64()),
        BinaryenLocalGet(module, 2, BinaryenTypeInt64())};
    BinaryenExpressionRef repeated[] = {BinaryenLocalGet(module, 2, BinaryenTypeInt64()),
        BinaryenLocalGet(module, 2, BinaryenTypeInt64())};
    bool ok = represented_nodes_push(&body, BinaryenLocalSet(module, 2,
        BinaryenLocalGet(module, 0, BinaryenTypeInt64())))
        && represented_nodes_push(&body, BinaryenLocalSet(module, 3,
            BinaryenLocalGet(module, 1, BinaryenTypeInt64())))
        && represented_parallel_assign(module, &body, swap, destinations, 2, 4)
        && represented_parallel_assign(module, &body, repeated, destinations, 2, 4)
        && represented_nodes_push(&body, BinaryenReturn(module, BinaryenBinary(module, BinaryenOrInt64(),
            BinaryenBinary(module, BinaryenShlInt64(), BinaryenLocalGet(module, 2,
                BinaryenTypeInt64()), BinaryenConst(module, BinaryenLiteralInt64(32))),
            BinaryenLocalGet(module, 3, BinaryenTypeInt64()))));
    BinaryenType variables[] = {BinaryenTypeInt64(), BinaryenTypeInt64(),
        BinaryenTypeInt64(), BinaryenTypeInt64()};
    ok = ok && BinaryenAddFunction(module, "parallel", BinaryenTypeCreate(parameters, 2),
        BinaryenTypeInt64(), variables, 4, BinaryenBlock(module, NULL, body.items,
            (BinaryenIndex)body.count, BinaryenTypeNone())) != NULL;
    deallocate(body.items);
    if (ok) BinaryenAddFunctionExport(module, "parallel", "parallel");
    if (ok) ok = BinaryenModuleValidate(module);
    BinaryenModuleAllocateAndWriteResult serialized = ok
        ? BinaryenModuleAllocateAndWrite(module, NULL)
        : (BinaryenModuleAllocateAndWriteResult){NULL, 0, NULL};
    BinaryenModuleDispose(module);
    if (serialized.binary == NULL) return false;
    wasm_engine_t *engine = wasm_engine_new(); wasm_store_t *store = engine ? wasm_store_new(engine) : NULL;
    wasm_byte_vec_t bytes = {serialized.binaryBytes, (wasm_byte_t *)serialized.binary};
    wasm_module_t *wasm = store ? wasm_module_new(store, &bytes) : NULL;
    wasm_extern_vec_t imports = WASM_EMPTY_VEC, exports = WASM_EMPTY_VEC;
    wasm_trap_t *trap = NULL; wasm_instance_t *instance = wasm ? wasm_instance_new(store, wasm, &imports, &trap) : NULL;
    ok = instance != NULL && trap == NULL;
    if (trap) wasm_trap_delete(trap);
    if (ok) {
        wasm_instance_exports(instance, &exports);
        wasm_val_t input[] = {{.kind = WASM_I64, .of.i64 = 11}, {.kind = WASM_I64, .of.i64 = 29}}, output[1];
        wasm_val_vec_t arguments = WASM_ARRAY_VEC(input), results = WASM_ARRAY_VEC(output);
        trap = exports.size == 1 ? wasm_func_call(wasm_extern_as_func(exports.data[0]), &arguments, &results) : NULL;
        ok = exports.size == 1 && trap == NULL && output[0].kind == WASM_I64
            && output[0].of.i64 == INT64_C(0x0000001d0000001d);
        if (trap) wasm_trap_delete(trap);
    }
    wasm_extern_vec_delete(&exports); if (instance) wasm_instance_delete(instance);
    if (wasm) wasm_module_delete(wasm); if (store) wasm_store_delete(store); if (engine) wasm_engine_delete(engine);
    free(serialized.binary); return ok;
}
bool sol_wasm_represented_test_rejects_capture_snapshot(void) {
    return !represented_instruction_kind_supported(SOL_MIR_INST_CAPTURE_SNAPSHOT);
}
bool sol_wasm_represented_test_rejects_resume_failure(void) {
    return !represented_terminator(&(SolMirMaterializedTerminator){
        .kind = SOL_MIR_TERM_RESUME_FAILURE});
}
bool sol_wasm_represented_test_size_add_overflow(void) {
    size_t result = 0;
    return !represented_add(SIZE_MAX, 1, &result) && result == 0;
}
bool sol_wasm_represented_test_sum_helper_items(size_t variants) {
    if (!represented_sum_helper_variant_count_supported(variants)) return false;
    BinaryenModuleRef module = BinaryenModuleCreate();
    if (module == NULL) return false;
    BinaryenExpressionRef items[REPRESENTED_SUM_HELPER_ITEMS];
    size_t count = 0;
    for (; count < REPRESENTED_SUM_HELPER_INITIAL_ITEMS; ++count)
        items[count] = BinaryenNop(module);
    for (size_t i = 0; i < variants; ++i) {
        if (count >= REPRESENTED_SUM_HELPER_ITEMS - REPRESENTED_SUM_HELPER_FINAL_ITEMS) {
            BinaryenModuleDispose(module);
            return false;
        }
        items[count++] = BinaryenNop(module);
    }
    if (count >= REPRESENTED_SUM_HELPER_ITEMS) {
        BinaryenModuleDispose(module);
        return false;
    }
    items[count++] = BinaryenReturn(module, BinaryenConst(module, BinaryenLiteralInt64(0)));
    bool ok = count == REPRESENTED_SUM_HELPER_ITEMS
        ? BinaryenAddFunction(module, "sum-helper-boundary", BinaryenTypeNone(), BinaryenTypeInt64(),
            NULL, 0, BinaryenBlock(module, NULL, items, (BinaryenIndex)count,
                BinaryenTypeNone())) != NULL && BinaryenModuleValidate(module)
        : false;
    BinaryenModuleDispose(module);
    return ok;
}
void sol_wasm_represented_test_allocator_memory(size_t max_pages, uint32_t heap_base) {
    represented_test_max_pages = max_pages == 0 || max_pages == 1 ? max_pages : SIZE_MAX;
    represented_test_heap_base = heap_base;
}
void sol_wasm_represented_test_allocator_quota(uint64_t max_requests, uint64_t max_bytes) {
    represented_test_max_requests = max_requests;
    represented_test_max_bytes = max_bytes;
}

void sol_wasm_represented_test_inactive_payload_probe(bool enabled) {
    represented_test_inactive_payload_probe = enabled;
}
void sol_wasm_represented_test_callback_writeback_probe(bool enabled) {
    represented_test_callback_writeback_probe = enabled;
}
void sol_wasm_represented_test_callable_hole_cleanup_probe(bool enabled) {
    represented_test_callable_hole_cleanup_probe = enabled;
}
void sol_wasm_represented_test_p44_packet_reset_probe(bool enabled) {
    represented_test_p44_packet_reset_probe = enabled;
}
void sol_wasm_represented_test_p44_cleanup_trace_probe(bool enabled) {
    represented_test_p44_cleanup_trace_probe = enabled;
}
void sol_wasm_represented_test_p44_refinement_trace_probe(bool enabled) {
    represented_test_p44_refinement_trace_probe = enabled;
}
bool sol_wasm_represented_test_control_transition(const SolWasmRepresentedBuildRequest *request,
    size_t block, SolMirRuntimeCleanupEdgeRole role, size_t *transition,
    SolMirRuntimeSlice *actions) {
    if (request == NULL || request->program == NULL || transition == NULL || actions == NULL) return false;
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirMaterialization *m = &owner->conventions->concrete->materialization;
    if (block >= m->block_count || block >= owner->image_terminator_count) return false;
    const SolMirMaterializedTerminator *term = &m->blocks[block].terminator;
    const SolMirRuntimeLoweredImageTerminator *row = &owner->image_terminators[block];
    if (row->image >= m->image_count) return false;
    size_t edge = SOL_MIR_RUNTIME_NONE, count = 0;
    SolMirRuntimeCleanupOutcome outcome;
    switch (term->kind) {
        case SOL_MIR_TERM_GOTO: case SOL_MIR_TERM_BREAK: case SOL_MIR_TERM_CONTINUE:
            if (role != SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO) return false;
            edge = term->edge; count = 1; outcome = SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL; break;
        case SOL_MIR_TERM_BRANCH:
            if (role == SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_TRUE) edge = term->true_edge;
            else if (role == SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_FALSE) edge = term->false_edge;
            else return false;
            count = 2; outcome = SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL; break;
        case SOL_MIR_TERM_RETURN:
            if (role != SOL_MIR_RUNTIME_CLEANUP_EDGE_RETURN) return false;
            count = 1; outcome = SOL_MIR_RUNTIME_CLEANUP_OUTCOME_EXIT; break;
        default: return false;
    }
    RepresentedQualifiedIndex qualified_index;
    if (represented_qualified_index_build(request, &qualified_index)
            != REPRESENTED_QUALIFIED_INDEX_VALID) return false;
    const SolMirRuntimeCleanupTransition *selected = represented_control_transition(request,
        &m->images[row->image], &qualified_index, row->image, block, role, outcome, edge, count);
    if (selected == NULL) { represented_qualified_index_free(&qualified_index); return false; }
    *transition = (size_t)(selected - owner->cleanup->transitions);
    *actions = selected->actions;
    represented_qualified_index_free(&qualified_index);
    return true;
}

SolWasmRepresentedTestCleanupMarkerRoute sol_wasm_represented_test_cleanup_marker(
    const SolWasmRepresentedBuildRequest *request, size_t instruction, size_t *action) {
    if (action != NULL) *action = SOL_MIR_RUNTIME_NONE;
    if (request == NULL || request->program == NULL) return SOL_WASM_REPRESENTED_TEST_CLEANUP_MARKER_INVALID;
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirMaterialization *m = &owner->conventions->concrete->materialization;
    if (instruction >= m->instruction_count || instruction >= owner->image_instruction_count) {
        return SOL_WASM_REPRESENTED_TEST_CLEANUP_MARKER_INVALID;
    }
    const SolMirRuntimeLoweredImageInstruction *row = &owner->image_instructions[instruction];
    if (row->image >= m->image_count) return SOL_WASM_REPRESENTED_TEST_CLEANUP_MARKER_INVALID;
    RepresentedFunction function = {0};
    function.request = request;
    function.image = &m->images[row->image];
    function.image_id = row->image;
    const SolMirRuntimeCleanupAction *selected = NULL;
    switch (represented_cleanup_instruction_route(&function, instruction, &selected)) {
        case REPRESENTED_CLEANUP_MARKER_EVENTLESS:
            return SOL_WASM_REPRESENTED_TEST_CLEANUP_MARKER_EVENTLESS;
        case REPRESENTED_CLEANUP_MARKER_ACTION:
            if (selected == NULL || selected < owner->cleanup->actions
                || selected >= owner->cleanup->actions + owner->cleanup->action_count)
                return SOL_WASM_REPRESENTED_TEST_CLEANUP_MARKER_INVALID;
            if (action != NULL) *action = (size_t)(selected - owner->cleanup->actions);
            return SOL_WASM_REPRESENTED_TEST_CLEANUP_MARKER_ACTION;
        default:
            return SOL_WASM_REPRESENTED_TEST_CLEANUP_MARKER_INVALID;
    }
}
#endif
