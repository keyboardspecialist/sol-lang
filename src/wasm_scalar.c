#include "wasm_scalar_internal.h"

#include <binaryen-c.h>
#include <wasm.h>

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* This file intentionally has no dependency on the P4.1 module shape.  P4.1
 * is a compatibility probe, whereas scalar modules have no memory/table/import
 * and use these two diagnostic globals only while P4.2 remains internal. */
#define P42_CODE "sol.p42.failure-code"
#define P42_SITE "sol.p42.failure-site"

static size_t allocation_attempts;
static size_t fail_allocation_after;

typedef enum {
    SCALAR_BACKEND_OK,
    SCALAR_BACKEND_RESOURCE,
    SCALAR_BACKEND_ALLOCATION,
} ScalarBackendStatus;

typedef struct {
    size_t bytes;
    bool scratch;
} ScalarAllocation;

typedef struct {
    const SolWasmScalarLimits *limits;
    SolWasmScalarUsage *usage;
    size_t scratch_live;
    size_t owned_live;
    ScalarBackendStatus status;
} ScalarAccounting;

/* Binaryen and Wasmtime allocate internally.  Their private allocations are
 * outside the P4.2 allocation hook and byte quotas; only backend-owned C
 * buffers and the transferred serialized buffer are metered here. */
static ScalarAccounting *scalar_accounting;

static bool scalar_add(size_t left, size_t right, size_t *result) {
    if (left > SIZE_MAX - right) return false;
    *result = left + right;
    return true;
}

static void *allocate(size_t count, size_t size) {
    size_t bytes, work, scratch, owned;
    if (size != 0 && count > SIZE_MAX / size) {
        if (scalar_accounting != NULL) scalar_accounting->status = SCALAR_BACKEND_RESOURCE;
        return NULL;
    }
    bytes = count * size;
    if (scalar_accounting != NULL) {
        if (!scalar_add(scalar_accounting->usage->work_bytes, bytes, &work)
            || !scalar_add(scalar_accounting->scratch_live, bytes, &scratch)
            || !scalar_add(scalar_accounting->owned_live, bytes, &owned)
            || work > scalar_accounting->limits->max_work_bytes
            || scratch > scalar_accounting->limits->max_scratch_bytes
            || owned > scalar_accounting->limits->max_owned_bytes) {
            scalar_accounting->status = SCALAR_BACKEND_RESOURCE;
            return NULL;
        }
    }
    ++allocation_attempts;
    if (fail_allocation_after != 0 && allocation_attempts == fail_allocation_after) {
        if (scalar_accounting != NULL) scalar_accounting->status = SCALAR_BACKEND_ALLOCATION;
        return NULL;
    }
    if (bytes > SIZE_MAX - sizeof(ScalarAllocation)) {
        if (scalar_accounting != NULL) scalar_accounting->status = SCALAR_BACKEND_RESOURCE;
        return NULL;
    }
    ScalarAllocation *allocation = calloc(1, sizeof *allocation + bytes);
    if (allocation == NULL) {
        if (scalar_accounting != NULL) scalar_accounting->status = SCALAR_BACKEND_ALLOCATION;
        return NULL;
    }
    allocation->bytes = bytes;
    allocation->scratch = scalar_accounting != NULL;
    if (scalar_accounting != NULL) {
        scalar_accounting->usage->work_bytes = work;
        scalar_accounting->scratch_live = scratch;
        scalar_accounting->owned_live = owned;
        if (scratch > scalar_accounting->usage->scratch_bytes)
            scalar_accounting->usage->scratch_bytes = scratch;
        if (owned > scalar_accounting->usage->owned_bytes)
            scalar_accounting->usage->owned_bytes = owned;
    }
    return allocation + 1;
}

static void deallocate(void *pointer) {
    if (pointer == NULL) return;
    ScalarAllocation *allocation = (ScalarAllocation *)pointer - 1;
    if (allocation->scratch && scalar_accounting != NULL) {
        scalar_accounting->scratch_live -= allocation->bytes;
        scalar_accounting->owned_live -= allocation->bytes;
    }
    free(allocation);
}

static void *grow(void *pointer, size_t old_count, size_t new_count, size_t size) {
    if (new_count < old_count || (size != 0 && new_count > SIZE_MAX / size)) {
        if (scalar_accounting != NULL) scalar_accounting->status = SCALAR_BACKEND_RESOURCE;
        return NULL;
    }
    void *replacement = allocate(new_count, size);
    if (replacement == NULL) return NULL;
    if (pointer != NULL && old_count != 0) memcpy(replacement, pointer, old_count * size);
    deallocate(pointer);
    return replacement;
}

static bool scalar_node_reserve(void) {
    if (scalar_accounting == NULL) return true;
    if (scalar_accounting->usage->generated_nodes
        >= scalar_accounting->limits->max_generated_nodes) {
        scalar_accounting->status = SCALAR_BACKEND_RESOURCE;
        return false;
    }
    ++scalar_accounting->usage->generated_nodes;
    return true;
}

static BinaryenExpressionRef scalar_node_finish(BinaryenExpressionRef result) {
    if (result == NULL && scalar_accounting != NULL)
        --scalar_accounting->usage->generated_nodes;
    return result;
}

static BinaryenExpressionRef scalar_const(BinaryenModuleRef m, struct BinaryenLiteral x) {
    return scalar_node_reserve() ? scalar_node_finish(BinaryenConst(m, x)) : NULL;
}
static BinaryenExpressionRef scalar_unary(BinaryenModuleRef m, BinaryenOp o,
    BinaryenExpressionRef x) {
    return scalar_node_reserve() ? scalar_node_finish(BinaryenUnary(m, o, x)) : NULL;
}
static BinaryenExpressionRef scalar_binary_node(BinaryenModuleRef m, BinaryenOp o,
    BinaryenExpressionRef x, BinaryenExpressionRef y) {
    return scalar_node_reserve() ? scalar_node_finish(BinaryenBinary(m, o, x, y)) : NULL;
}
static BinaryenExpressionRef scalar_if(BinaryenModuleRef m, BinaryenExpressionRef c,
    BinaryenExpressionRef yes, BinaryenExpressionRef no) {
    return scalar_node_reserve() ? scalar_node_finish(BinaryenIf(m, c, yes, no)) : NULL;
}
static BinaryenExpressionRef scalar_block(BinaryenModuleRef m, const char *name,
    BinaryenExpressionRef *items, BinaryenIndex count, BinaryenType type) {
    return scalar_node_reserve() ? scalar_node_finish(BinaryenBlock(m, name, items, count, type)) : NULL;
}
static BinaryenExpressionRef scalar_loop(BinaryenModuleRef m, const char *name,
    BinaryenExpressionRef body) {
    return scalar_node_reserve() ? scalar_node_finish(BinaryenLoop(m, name, body)) : NULL;
}
static BinaryenExpressionRef scalar_break(BinaryenModuleRef m, const char *name,
    BinaryenExpressionRef condition, BinaryenExpressionRef value) {
    return scalar_node_reserve() ? scalar_node_finish(BinaryenBreak(m, name, condition, value)) : NULL;
}
static BinaryenExpressionRef scalar_call(BinaryenModuleRef m, const char *target,
    BinaryenExpressionRef *operands, BinaryenIndex count, BinaryenType result) {
    return scalar_node_reserve() ? scalar_node_finish(BinaryenCall(m, target, operands, count, result)) : NULL;
}
static BinaryenExpressionRef scalar_local_get(BinaryenModuleRef m, BinaryenIndex index,
    BinaryenType type) {
    return scalar_node_reserve() ? scalar_node_finish(BinaryenLocalGet(m, index, type)) : NULL;
}
static BinaryenExpressionRef scalar_local_set(BinaryenModuleRef m, BinaryenIndex index,
    BinaryenExpressionRef value) {
    return scalar_node_reserve() ? scalar_node_finish(BinaryenLocalSet(m, index, value)) : NULL;
}
static BinaryenExpressionRef scalar_global_get(BinaryenModuleRef m, const char *name,
    BinaryenType type) {
    return scalar_node_reserve() ? scalar_node_finish(BinaryenGlobalGet(m, name, type)) : NULL;
}
static BinaryenExpressionRef scalar_global_set(BinaryenModuleRef m, const char *name,
    BinaryenExpressionRef value) {
    return scalar_node_reserve() ? scalar_node_finish(BinaryenGlobalSet(m, name, value)) : NULL;
}
static BinaryenExpressionRef scalar_drop(BinaryenModuleRef m, BinaryenExpressionRef value) {
    return scalar_node_reserve() ? scalar_node_finish(BinaryenDrop(m, value)) : NULL;
}
static BinaryenExpressionRef scalar_return(BinaryenModuleRef m, BinaryenExpressionRef value) {
    return scalar_node_reserve() ? scalar_node_finish(BinaryenReturn(m, value)) : NULL;
}

#define BinaryenConst scalar_const
#define BinaryenUnary scalar_unary
#define BinaryenBinary scalar_binary_node
#define BinaryenIf scalar_if
#define BinaryenBlock scalar_block
#define BinaryenLoop scalar_loop
#define BinaryenBreak scalar_break
#define BinaryenCall scalar_call
#define BinaryenLocalGet scalar_local_get
#define BinaryenLocalSet scalar_local_set
#define BinaryenGlobalGet scalar_global_get
#define BinaryenGlobalSet scalar_global_set
#define BinaryenDrop scalar_drop
#define BinaryenReturn scalar_return

static void diagnostic(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics != NULL)
        (void)sol_diagnostics_add(diagnostics, "wasm-p42", SOL_SEVERITY_ERROR,
            (SolSpan){0, 0}, "%s", message);
}

static SolWasmScalarResult scalar_backend_result(void) {
    return scalar_accounting != NULL
        && scalar_accounting->status == SCALAR_BACKEND_RESOURCE
        ? SOL_WASM_SCALAR_RESOURCE_EXHAUSTED : SOL_WASM_SCALAR_ALLOCATION_FAILED;
}

static bool scalar_transfer_output(ScalarAccounting *accounting, size_t bytes) {
    size_t owned;
    if (!scalar_add(accounting->owned_live, bytes, &owned)
        || owned > accounting->limits->max_owned_bytes) {
        accounting->status = SCALAR_BACKEND_RESOURCE;
        return false;
    }
    accounting->owned_live = owned;
    if (owned > accounting->usage->owned_bytes) accounting->usage->owned_bytes = owned;
    return true;
}

void sol_wasm_scalar_output_init(SolWasmScalarOutput *output) {
    if (output == NULL) return;
    sol_wasm_backend_bytes_init(&output->bytes);
    memset(&output->usage, 0, sizeof output->usage);
}

void sol_wasm_scalar_output_free(SolWasmScalarOutput *output) {
    if (output == NULL) return;
    sol_wasm_backend_bytes_free(&output->bytes);
    memset(&output->usage, 0, sizeof output->usage);
}

SolWasmScalarLimits sol_wasm_scalar_default_limits(void) {
    return (SolWasmScalarLimits){
        .max_functions = 256, .max_blocks = 4096, .max_edges = 8192,
        .max_values = 16384, .max_locals = 16384,
        .max_generated_nodes = 262144, .max_provenance_records = 32768,
        .max_work_bytes = 16u * 1024u * 1024u,
        .max_scratch_bytes = 16u * 1024u * 1024u,
        .max_owned_bytes = 32u * 1024u * 1024u,
        .max_output_bytes = 16u * 1024u * 1024u,
    };
}

static bool limits_complete(const SolWasmScalarLimits *limits) {
    return limits->max_functions != 0 && limits->max_blocks != 0
        && limits->max_edges != 0 && limits->max_values != 0 && limits->max_locals != 0
        && limits->max_generated_nodes != 0 && limits->max_provenance_records != 0
        && limits->max_work_bytes != 0 && limits->max_scratch_bytes != 0
        && limits->max_owned_bytes != 0 && limits->max_output_bytes != 0;
}

static bool limits_zero(const SolWasmScalarLimits *limits) {
    return limits->max_functions == 0 && limits->max_blocks == 0
        && limits->max_edges == 0 && limits->max_values == 0 && limits->max_locals == 0
        && limits->max_generated_nodes == 0 && limits->max_provenance_records == 0
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

static bool scalar_recipe(const SolMirConcreteProgram *concrete,
    SolMirMaterializedTypeId type, bool terminal) {
    const SolMirMaterialization *materialization = &concrete->materialization;
    const SolMirLayout *layout = &concrete->layout;
    const SolMirRepresentation *representation = &concrete->representation;
    if (type >= materialization->type_count || type >= layout->type_count) return false;
    SolMirRecipeId recipe = layout->types[type].recipe;
    if (recipe >= representation->recipe_count) return false;
    switch (representation->recipes[recipe].kind) {
        case SOL_MIR_RECIPE_INT64:
        case SOL_MIR_RECIPE_BOOL:
            return representation->recipes[recipe].storage == SOL_MIR_STORAGE_SCALAR;
        case SOL_MIR_RECIPE_UNIT:
            return representation->recipes[recipe].storage == SOL_MIR_STORAGE_NONE;
        case SOL_MIR_RECIPE_NEVER:
            return terminal;
        default:
            return false;
    }
}

static bool whole_scalar_place(const SolMirConcreteProgram *concrete,
    SolMirMaterializedPlaceId place) {
    const SolMirMaterialization *materialization = &concrete->materialization;
    if (place >= materialization->place_count) return false;
    const SolMirMaterializedPlace *item = &materialization->places[place];
    return item->projections.count == 0
        && scalar_recipe(concrete, item->final_type, false);
}

static bool scalar_instruction_kind_supported(SolMirInstructionKind kind) {
    switch (kind) {
        case SOL_MIR_INST_CONST_INT64: case SOL_MIR_INST_CONST_BOOL:
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
            return true;
        default: return false;
    }
}

static bool scalar_instruction(const SolMirConcreteProgram *concrete,
    const SolMirMaterializedInstruction *instruction) {
    if (!scalar_instruction_kind_supported(instruction->kind)) return false;
    switch (instruction->kind) {
        case SOL_MIR_INST_CONST_INT64:
        case SOL_MIR_INST_CONST_BOOL:
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
            return whole_scalar_place(concrete, instruction->place);
        case SOL_MIR_INST_UNARY:
        case SOL_MIR_INST_BINARY:
        case SOL_MIR_INST_EXPRESSION_RESULT:
            return scalar_recipe(concrete, instruction->type, false);
        case SOL_MIR_INST_TEMPORARY_INIT:
        case SOL_MIR_INST_TEMPORARY_DROP:
            return instruction->temporary < concrete->materialization.temporary_count
                && scalar_recipe(concrete,
                    concrete->materialization.temporaries[instruction->temporary].type, false);
        default:
            return false;
    }
}

static bool scalar_terminator(const SolMirMaterializedTerminator *terminator) {
    switch (terminator->kind) {
        case SOL_MIR_TERM_GOTO:
        case SOL_MIR_TERM_BRANCH:
        case SOL_MIR_TERM_BREAK:
        case SOL_MIR_TERM_CONTINUE:
        case SOL_MIR_TERM_RETURN:
        case SOL_MIR_TERM_INVOKE:
            return true;
        default:
            return false;
    }
}

/* A pending resume is not scalar-emittable in Slice 3A.  It is admitted at
 * the owner boundary solely so the call catalog can prove that it is the
 * exact named failure continuation of one direct internal call. */
static bool scalar_owner_terminator(const SolMirMaterializedTerminator *terminator,
    size_t call_count) {
    return scalar_terminator(terminator)
        || (call_count != 0 && terminator->kind == SOL_MIR_TERM_RESUME_FAILURE);
}

/* Owner validation is deliberately the first dereference after request shape
 * checks.  It authenticates the exact P3.6 chain; the backend never accepts a
 * materialization or any predecessor as a substitute input. */
static bool scalar_owner(const SolWasmScalarBuildRequest *request,
    SolWasmScalarUsage *usage) {
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
    if (owner->import_count != 0 || owner->host_requirement_count != 0
        || owner->handler_frame_count != 0 || owner->predicate_body_count != 0)
        return false;
    for (size_t i = 0; i < materialization->local_count; ++i) {
        const SolMirMaterializedLocal *local = &materialization->locals[i];
        if (local->kind == SOL_MIR_MATERIALIZED_LOCAL_RECEIVER
            || local->access != SOL_ACCESS_OWNED
            || !scalar_recipe(concrete, local->type, false)) return false;
    }
    for (size_t i = 0; i < materialization->place_count; ++i)
        if (!whole_scalar_place(concrete, i)) return false;
    for (size_t i = 0; i < materialization->temporary_count; ++i)
        if (!scalar_recipe(concrete, materialization->temporaries[i].type, false)) return false;
    for (size_t i = 0; i < materialization->value_count; ++i)
        if (!scalar_recipe(concrete, materialization->values[i].type, false)) return false;
    for (size_t i = 0; i < materialization->instruction_count; ++i)
        if (!scalar_instruction(concrete, &materialization->instructions[i])) return false;
    for (size_t i = 0; i < materialization->block_count; ++i)
        if (!scalar_owner_terminator(&materialization->blocks[i].terminator,
                owner->call_count)) return false;
    if (!scalar_add(concrete->linkage.callable_count, owner->conventions->entry_count,
            &usage->functions)
        || usage->functions > UINT32_MAX) return false;
    usage->blocks = materialization->block_count;
    usage->edges = materialization->edge_count;
    usage->values = materialization->value_count;
    if (!scalar_add(owner->conventions->entry_count, concrete->linkage.callable_count,
            &usage->provenance_records)
        || !scalar_add(usage->provenance_records, owner->conventions->failure_site_count,
            &usage->provenance_records)
        || usage->provenance_records > UINT32_MAX) return false;
    return usage->blocks <= UINT32_MAX && usage->edges <= UINT32_MAX
        && usage->values <= UINT32_MAX;
}

/* Count emitted MIR quantities by their owning ranges instead of trusting a
 * single aggregate field.  The equality checks keep the limits tied to the
 * same closure that the emitter later walks. */
static bool scalar_usage_census(const SolWasmScalarBuildRequest *request,
    SolWasmScalarUsage *usage) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    size_t blocks = 0, values = 0, edges = 0;
    for (size_t i = 0; i < concrete->linkage.callable_count; ++i) {
        SolMirPlanInstanceId image_id = concrete->linkage.callables[i].instance;
        if (image_id >= m->image_count) return false;
        const SolMirMaterializedImage *image = &m->images[image_id];
        if (!scalar_add(blocks, image->blocks.count, &blocks)
            || !scalar_add(values, image->values.count, &values)) return false;
    }
    for (size_t i = 0; i < m->edge_count; ++i) {
        if (i >= request->program->image_edge_count
            || request->program->image_edges[i].image >= m->image_count
            || !scalar_add(edges, 1, &edges)) return false;
    }
    if (blocks != m->block_count || values != m->value_count || edges != m->edge_count
        || blocks > UINT32_MAX || values > UINT32_MAX || edges > UINT32_MAX) return false;
    usage->blocks = blocks; usage->values = values; usage->edges = edges;
    return true;
}

static void put_u32(uint8_t **cursor, uint32_t value) {
    (*cursor)[0] = (uint8_t)value; (*cursor)[1] = (uint8_t)(value >> 8);
    (*cursor)[2] = (uint8_t)(value >> 16); (*cursor)[3] = (uint8_t)(value >> 24);
    *cursor += 4;
}

typedef struct ScalarCallCatalogGraph ScalarCallCatalogGraph;

typedef struct {
    uint8_t tag, kind;
    const char *path, *symbol;
    size_t start, end, ordinal;
} ProvenanceRecord;

static int provenance_order(const void *left, const void *right) {
    const ProvenanceRecord *a = left, *b = right;
    int order = (int)a->tag - (int)b->tag;
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

/* The private v1 record schema is declared beside the section name in the
 * internal header.  Records are sorted before encoding so physical P2/P3 IDs
 * never influence either bytes or the one-based failure-site global. */
static uint8_t *provenance(const SolWasmScalarBuildRequest *request,
    size_t *out_size) {
    const SolIr *ir = request->program->conventions->concrete->program.ir;
    const SolMirRuntimeConventions *conventions = request->program->conventions;
    const SolMirConcreteProgram *concrete = conventions->concrete;
    size_t count = 0;
    if (!scalar_add(conventions->entry_count, concrete->linkage.callable_count, &count)
        || !scalar_add(count, conventions->failure_site_count, &count)
        || count > UINT32_MAX) {
        if (scalar_accounting != NULL) scalar_accounting->status = SCALAR_BACKEND_RESOURCE;
        return NULL;
    }
    ProvenanceRecord *records = count == 0 ? NULL : allocate(count, sizeof *records);
    if (count != 0 && records == NULL) return NULL;
    size_t at = 0, bytes = 12;
    for (size_t i = 0; i < conventions->entry_count; ++i) {
        const SolMirRuntimeEntry *entry = &conventions->entries[i]; const char *path = NULL;
        SolMirRuntimeSource source = {entry->source.file, entry->source.start, entry->source.end};
        if (!provenance_source(ir, request->package_directory, source, &path)) goto failed;
        records[at++] = (ProvenanceRecord){1, 0, path, entry->symbol.bytes,
            source.start, source.end, 0};
    }
    for (size_t i = 0; i < concrete->linkage.callable_count; ++i) {
        const SolMirLinkageCallable *callable = &concrete->linkage.callables[i];
        if (callable->instance >= concrete->materialization.image_count) goto failed;
        SolIrCallableId source = concrete->materialization.images[callable->instance].source_callable;
        const char *path = NULL; size_t start, end;
        if (source >= ir->callable_count || !callable_source(ir, request->package_directory,
                ir->callables[source].span, &path, &start, &end)) goto failed;
        records[at++] = (ProvenanceRecord){2, 0, path, callable->symbol.bytes, start, end, 0};
    }
    for (size_t i = 0; i < conventions->failure_site_count; ++i) {
        const SolMirRuntimeFailureSite *site = &conventions->failure_sites[i];
        const char *relative = NULL;
        if (!provenance_source(ir, request->package_directory, site->source, &relative)
            || site->owner >= concrete->materialization.image_count) goto failed;
        const char *symbol = symbol_for_image(concrete, site->owner);
        if (symbol == NULL) goto failed;
        size_t ordinal = 0;
        for (size_t earlier = 0; earlier < conventions->failure_site_count; ++earlier) {
            const SolMirRuntimeFailureSite *other = &conventions->failure_sites[earlier];
            if (other->owner != site->owner) continue;
            if (other->source.file < site->source.file
                || (other->source.file == site->source.file
                    && (other->source.start < site->source.start
                        || (other->source.start == site->source.start
                            && other->source.end < site->source.end)))) ++ordinal;
        }
        records[at++] = (ProvenanceRecord){3, (uint8_t)site->origin_kind, relative,
            symbol, site->source.start, site->source.end, ordinal};
    }
    qsort(records, count, sizeof *records, provenance_order);
    for (size_t i = 1; i < count; ++i)
        if (provenance_order(&records[i - 1], &records[i]) == 0) goto failed;
    for (size_t i = 0; i < count; ++i) {
        size_t path_length = strlen(records[i].path), symbol_length = strlen(records[i].symbol);
        size_t record_bytes = 24;
        if (path_length > UINT32_MAX || symbol_length > UINT32_MAX
            || path_length > SIZE_MAX - record_bytes) goto failed;
        record_bytes += path_length;
        if (symbol_length > SIZE_MAX - record_bytes) goto failed;
        record_bytes += symbol_length;
        if (record_bytes > SIZE_MAX - bytes) goto failed;
        bytes += record_bytes;
    }
    if (bytes > UINT32_MAX) {
        if (scalar_accounting != NULL) scalar_accounting->status = SCALAR_BACKEND_RESOURCE;
        goto failed;
    }
    uint8_t *data = allocate(bytes, 1);
    if (data == NULL) goto failed;
    uint8_t *cursor = data;
    memcpy(cursor, "P42P", 4); cursor += 4;
    put_u32(&cursor, 1); put_u32(&cursor, (uint32_t)count);
    for (size_t i = 0; i < count; ++i) {
        const ProvenanceRecord *record = &records[i]; size_t path_length = strlen(record->path);
        size_t symbol_length = strlen(record->symbol);
        *cursor++ = record->tag; *cursor++ = record->kind; *cursor++ = 0; *cursor++ = 0;
        put_u32(&cursor, (uint32_t)path_length); memcpy(cursor, record->path, path_length); cursor += path_length;
        put_u32(&cursor, (uint32_t)record->start); put_u32(&cursor, (uint32_t)record->end);
        put_u32(&cursor, (uint32_t)symbol_length); memcpy(cursor, record->symbol, symbol_length); cursor += symbol_length;
        put_u32(&cursor, (uint32_t)record->ordinal);
    }
    deallocate(records); *out_size = bytes; return data;
failed:
    deallocate(records); return NULL;
/* Old dense encoding intentionally removed: section records are canonical. */
/*
        size_t length = strlen(relative);
        if (length > UINT32_MAX || bytes > SIZE_MAX - 16 - length) return NULL;
        bytes += 16 + length;
    }
    uint8_t *data = allocate(bytes, 1);
    if (data == NULL) return NULL;
    uint8_t *cursor = data;
    memcpy(cursor, "P42P", 4); cursor += 4;
    put_u32(&cursor, 1); put_u32(&cursor, (uint32_t)conventions->failure_site_count);
    for (size_t i = 0; i < conventions->failure_site_count; ++i) {
        const SolMirRuntimeFailureSite *site = &conventions->failure_sites[i];
        const char *relative = NULL;
        (void)package_relative(request->package_directory,
            ir->files[site->source.file].path, &relative);
        size_t length = strlen(relative);
        put_u32(&cursor, (uint32_t)length); memcpy(cursor, relative, length); cursor += length;
        put_u32(&cursor, (uint32_t)site->source.start);
        put_u32(&cursor, (uint32_t)site->source.end);
        put_u32(&cursor, (uint32_t)site->origin_kind);
    }
    *out_size = bytes;
    return data;
*/
}

typedef struct {
    BinaryenExpressionRef *items;
    size_t count, capacity;
} ScalarNodes;

static bool scalar_nodes_push(ScalarNodes *nodes, BinaryenExpressionRef item) {
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
static bool scalar_parallel_assign(BinaryenModuleRef module, ScalarNodes *nodes,
    BinaryenExpressionRef *sources, const BinaryenIndex *destinations,
    size_t count, size_t scratch_base) {
    for (size_t i = 0; i < count; ++i)
        if (!scalar_nodes_push(nodes, BinaryenLocalSet(module,
                (BinaryenIndex)(scratch_base + i), sources[i]))) return false;
    for (size_t i = 0; i < count; ++i)
        if (!scalar_nodes_push(nodes, BinaryenLocalSet(module, destinations[i],
                BinaryenLocalGet(module, (BinaryenIndex)(scratch_base + i),
                    BinaryenTypeInt64())))) return false;
    return true;
}

typedef struct {
    const SolWasmScalarBuildRequest *request;
    BinaryenModuleRef module;
    const SolMirMaterializedImage *image;
    const SolMirRuntimeSignature *signature;
    const ScalarCallCatalogGraph *catalog;
    bool calls_enabled;
    size_t image_id, parameter_count;
    size_t value_base, temporary_base, local_base, temporary_init_base;
    size_t local_init_base, scratch_base, pc;
} ScalarFunction;

static bool recipe_scalar(const SolMirConcreteProgram *concrete,
    SolMirRecipeId recipe, bool terminal) {
    const SolMirRepresentation *representation = &concrete->representation;
    if (recipe >= representation->recipe_count) return false;
    const SolMirRecipe *item = &representation->recipes[recipe];
    switch (item->kind) {
        case SOL_MIR_RECIPE_INT64:
        case SOL_MIR_RECIPE_BOOL:
            return item->storage == SOL_MIR_STORAGE_SCALAR;
        case SOL_MIR_RECIPE_UNIT:
            return item->storage == SOL_MIR_STORAGE_NONE;
        case SOL_MIR_RECIPE_NEVER:
            return terminal;
        default:
            return false;
    }
}

static size_t local_index(const ScalarFunction *function,
    SolMirMaterializedLocalId local) {
    if (local < function->image->locals.offset
        || local - function->image->locals.offset >= function->image->locals.count)
        return SIZE_MAX;
    return function->local_base + local - function->image->locals.offset;
}

static size_t temporary_index(const ScalarFunction *function,
    SolMirMaterializedTemporaryId temporary) {
    if (temporary < function->image->temporaries.offset
        || temporary - function->image->temporaries.offset >= function->image->temporaries.count)
        return SIZE_MAX;
    return function->temporary_base + temporary - function->image->temporaries.offset;
}

static size_t value_index(const ScalarFunction *function, SolMirMaterializedValueId value) {
    if (value < function->image->values.offset
        || value - function->image->values.offset >= function->image->values.count)
        return SIZE_MAX;
    return function->value_base + value - function->image->values.offset;
}

static size_t temporary_init_index(const ScalarFunction *function,
    SolMirMaterializedTemporaryId temporary) {
    size_t index = temporary_index(function, temporary);
    return index == SIZE_MAX ? SIZE_MAX : function->temporary_init_base
        + index - function->temporary_base;
}

static size_t local_init_index(const ScalarFunction *function,
    SolMirMaterializedLocalId local) {
    size_t index = local_index(function, local);
    return index == SIZE_MAX ? SIZE_MAX : function->local_init_base + index - function->local_base;
}

static BinaryenExpressionRef i64_bool(BinaryenModuleRef module,
    BinaryenExpressionRef value) {
    return BinaryenUnary(module, BinaryenExtendUInt32(), value);
}

static BinaryenExpressionRef get_value(const ScalarFunction *function,
    SolMirMaterializedValueId value) {
    size_t index = value_index(function, value);
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

/* Keep the resource census and the emitter on one physical-local layout:
 * params, values, temporaries, P2 locals, their init flags, edge/call scratch,
 * and the dispatch PC.  Entry wrappers have no locals but are counted as
 * functions by scalar_owner. */
static bool scalar_function_local_count(const SolWasmScalarBuildRequest *request,
    size_t callable, const SolMirRuntimeSignature *signature, size_t *count) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    if (callable >= concrete->linkage.callable_count || signature == NULL
        || concrete->linkage.callables[callable].instance >= m->image_count) return false;
    const SolMirMaterializedImage *image = &m->images[concrete->linkage.callables[callable].instance];
    size_t scratch = request->program->conventions->call_count == 0 ? 0 : 1;
    for (size_t i = 0; i < image->blocks.count; ++i) {
        size_t block = image->blocks.offset + i;
        if (block >= m->block_count) return false;
        if (m->blocks[block].parameters.count > scratch) scratch = m->blocks[block].parameters.count;
    }
    size_t total = signature->slots.count;
    return scalar_add(total, image->values.count, &total)
        && scalar_add(total, image->temporaries.count, &total)
        && scalar_add(total, image->locals.count, &total)
        && scalar_add(total, image->temporaries.count, &total)
        && scalar_add(total, image->locals.count, &total)
        && scalar_add(total, scratch, &total)
        && scalar_add(total, 1, &total) && total <= UINT32_MAX
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

static unsigned scalar_opcode_failures(SolMirOperationOpcode opcode) {
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

static uint32_t scalar_failure_mask(unsigned failures) {
    uint32_t mask = 0;
    if ((failures & SOL_MIR_OPERATION_FAILURE_OVERFLOW) != 0)
        mask |= UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_INTEGER_OVERFLOW - 1);
    if ((failures & SOL_MIR_OPERATION_FAILURE_DIVISION_BY_ZERO) != 0)
        mask |= UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_DIVISION_BY_ZERO - 1);
    return mask;
}

static bool scalar_checked_opcode(const SolMirOperationArithmeticPlan *plan) {
    return scalar_opcode_failures(plan->opcode) != SOL_MIR_OPERATION_FAILURE_NONE
        && plan->failures == scalar_opcode_failures(plan->opcode);
}

static const SolMirOperationArithmeticPlan *arithmetic_for_instruction(
    const SolWasmScalarBuildRequest *request, size_t instruction) {
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

static bool scalar_cleanup_action(const SolWasmScalarBuildRequest *request,
    const SolMirRuntimeCleanupAction *action) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    if ((action->flags & ~((unsigned)SOL_MIR_RUNTIME_CLEANUP_ACTION_NORMAL_ONLY
            | SOL_MIR_RUNTIME_CLEANUP_ACTION_FAILURE_ONLY
            | SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED)) != 0) return false;
    switch (action->kind) {
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_TEMPORARY:
            return action->target < concrete->materialization.temporary_count
                && scalar_recipe(concrete,
                    concrete->materialization.temporaries[action->target].type, false);
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE:
            return whole_scalar_place(concrete, action->target);
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER:
            return action->target < concrete->materialization.local_count
                && scalar_recipe(concrete,
                    concrete->materialization.locals[action->target].type, false);
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_SCOPE:
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_REGION:
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE:
            return true;
        default: return false;
    }
}

static bool scalar_failure_route(const SolWasmScalarBuildRequest *request,
    size_t instruction, const SolMirOperationArithmeticPlan *plan) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirRuntimeCleanup *cleanup = owner->cleanup;
    const SolMirRuntimeConventions *conventions = owner->conventions;
    const SolMirMaterialization *m = &conventions->concrete->materialization;
    if (instruction >= owner->image_instruction_count || instruction >= m->instruction_count)
        return false;
    const SolMirRuntimeLoweredImageInstruction *row = &owner->image_instructions[instruction];
    uint32_t allowed = scalar_failure_mask(plan->failures);
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
            if (!scalar_cleanup_action(request, action)) return false;
            if (action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE) {
                if (a + 1 != transition->actions.count || propagate) return false;
                propagate = true;
            } else if (propagate) return false;
        }
        if (!propagate || ++matches != 1) return false;
    }
    return normal == 1 && matches == 1;
}

/* P2/P3.6 already authenticate dominance and definite-initialization.  This
 * bridge deliberately does not reimplement that fixed point: it proves that
 * every physical local/get/set selected below is the same image-local scalar
 * node that P2 authenticated. */
static bool image_value(const SolMirConcreteProgram *concrete,
    const SolMirMaterializedImage *image, SolMirMaterializedValueId value) {
    return value >= image->values.offset && value - image->values.offset < image->values.count
        && value < concrete->materialization.value_count
        && scalar_recipe(concrete, concrete->materialization.values[value].type, false);
}

static bool image_place(const SolMirConcreteProgram *concrete,
    const SolMirMaterializedImage *image, SolMirMaterializedPlaceId place) {
    const SolMirMaterialization *m = &concrete->materialization;
    if (place >= m->place_count || !whole_scalar_place(concrete, place)) return false;
    const SolMirMaterializedPlace *item = &m->places[place];
    return item->local >= image->locals.offset
        && item->local - image->locals.offset < image->locals.count
        && item->final_type == m->locals[item->local].type;
}

static bool image_edge_preflight(const SolWasmScalarBuildRequest *request,
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

static bool scalar_cleanup(const SolWasmScalarBuildRequest *request) {
    const SolMirRuntimeCleanup *cleanup = request->program->cleanup;
    for (size_t i = 0; i < cleanup->action_count; ++i) {
        const SolMirRuntimeCleanupAction *action = &cleanup->actions[i];
        if (!scalar_cleanup_action(request, action)) return false;
    }
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
} ScalarCallCatalog;

struct ScalarCallCatalogGraph {
    ScalarCallCatalog *calls;
    unsigned char *resume_blocks;
    size_t count, edge_count, longest_chain;
    bool has_cycle;
    size_t work_bytes;
};

typedef enum {
    SCALAR_CATALOG_VALID,
    SCALAR_CATALOG_INVALID,
    SCALAR_CATALOG_RESOURCE,
    SCALAR_CATALOG_ALLOCATION,
} ScalarCatalogResult;

static ScalarCatalogResult scalar_catalog_allocation_result(void) {
    return scalar_accounting != NULL
        && scalar_accounting->status == SCALAR_BACKEND_RESOURCE
        ? SCALAR_CATALOG_RESOURCE : SCALAR_CATALOG_ALLOCATION;
}

static bool scalar_size_add(size_t *value, size_t add) {
    if (*value > SIZE_MAX - add) return false;
    *value += add;
    return true;
}

static bool scalar_size_mul_add(size_t *value, size_t count, size_t size) {
    if (size != 0 && count > SIZE_MAX / size) return false;
    return scalar_size_add(value, count * size);
}

static bool scalar_runtime_ref_equal(SolMirRuntimeValueRef a, SolMirRuntimeValueRef b) {
    return a.kind == b.kind && a.id == b.id;
}

static bool scalar_lowered_call_exact(const SolMirRuntimeLoweredCall *lowered,
    const SolMirRuntimeCall *call, size_t id) {
    return lowered->state == SOL_MIR_RUNTIME_LOWERED_PRESENT && lowered->call == id
        && lowered->signature == call->signature && lowered->owner_kind == call->owner_kind
        && lowered->image == call->image && lowered->body == call->predicate
        && lowered->block == call->block && lowered->call_kind == call->call_kind
        && lowered->target_kind == call->target_kind && lowered->internal == call->internal
        && lowered->host == call->host && lowered->table == call->table
        && scalar_runtime_ref_equal(lowered->callee, call->callee)
        && lowered->operands.offset == call->operands.offset
        && lowered->operands.count == call->operands.count
        && scalar_runtime_ref_equal(lowered->result, call->result)
        && lowered->normal_edge == call->normal_edge && lowered->failure_edge == call->failure_edge
        && lowered->writebacks.offset == call->writebacks.offset
        && lowered->writebacks.count == call->writebacks.count
        && lowered->import_id == SOL_MIR_RUNTIME_NONE
        && lowered->bound_environment_import == SOL_MIR_RUNTIME_NONE
        && lowered->entry == SOL_MIR_RUNTIME_NONE && lowered->failure_site == call->failure_site;
}

static bool scalar_call_cleanup(const SolWasmScalarBuildRequest *request,
    const SolMirRuntimeCall *call, const SolMirMaterializedTerminator *term,
    size_t caller_image, size_t caller_block, unsigned char *resume_blocks) {
    const SolMirRuntimeCleanup *cleanup = request->program->cleanup;
    const SolMirMaterialization *m = &request->program->conventions->concrete->materialization;
    const SolMirRuntimeConventions *conventions = request->program->conventions;
    uint32_t call_depth = UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT - 1);
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
                || transition->failure_source != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_LOCAL_OR_PENDING
                || transition->failure_site != call->failure_site
                || transition->failure_mask != call_depth) return false;
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

static ScalarCatalogResult scalar_call_catalog(const SolWasmScalarBuildRequest *request,
    const SolWasmScalarLimits *limits, ScalarCallCatalogGraph *graph) {
    const SolMirRuntimeLoweredProgram *owner = request->program;
    const SolMirRuntimeConventions *conventions = owner->conventions;
    const SolMirConcreteProgram *concrete = conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirLinkage *linkage = &concrete->linkage;
    memset(graph, 0, sizeof *graph);
    size_t work = 0;
    if (!scalar_size_mul_add(&work, conventions->call_count, sizeof *graph->calls)
        || !scalar_size_mul_add(&work, m->block_count, sizeof *graph->resume_blocks)
        || !scalar_size_mul_add(&work, linkage->callable_count, sizeof(size_t))
        || !scalar_size_mul_add(&work, linkage->callable_count, sizeof(size_t))
        || !scalar_size_mul_add(&work, linkage->callable_count, sizeof(size_t)))
        return SCALAR_CATALOG_RESOURCE;
    graph->work_bytes = work;
    if (work > limits->max_work_bytes) return SCALAR_CATALOG_RESOURCE;
    graph->calls = conventions->call_count == 0 ? NULL
        : allocate(conventions->call_count, sizeof *graph->calls);
    graph->resume_blocks = m->block_count == 0 ? NULL
        : allocate(m->block_count, sizeof *graph->resume_blocks);
    if ((conventions->call_count != 0 && graph->calls == NULL)
        || (m->block_count != 0 && graph->resume_blocks == NULL)) {
        deallocate(graph->calls); deallocate(graph->resume_blocks);
        graph->calls = NULL; graph->resume_blocks = NULL;
        return scalar_catalog_allocation_result();
    }
    for (size_t id = 0; id < conventions->call_count; ++id) {
        const SolMirRuntimeCall *call = &conventions->calls[id];
        if (id >= owner->call_count || !scalar_lowered_call_exact(&owner->calls[id], call, id)
            || call->owner_kind != SOL_MIR_RUNTIME_CALL_OWNER_IMAGE
            || call->image >= m->image_count || call->block >= m->block_count
            || call->block < m->images[call->image].blocks.offset
            || call->block - m->images[call->image].blocks.offset >= m->images[call->image].blocks.count
            || call->call_kind != SOL_IR_CALL_FUNCTION
            || call->target_kind != SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL
            || call->internal >= linkage->callable_count || call->host != SOL_MIR_LINKAGE_NONE
            || call->table != SOL_MIR_LINKAGE_NONE || call->callee.kind != SOL_MIR_RUNTIME_VALUE_NONE
            || call->callee.id != SOL_MIR_RUNTIME_NONE || call->writebacks.count != 0
            || call->writebacks.offset > conventions->writeback_count
            || call->signature >= conventions->signature_count
            || call->failure_site >= conventions->failure_site_count
            || call->block >= owner->image_terminator_count) goto invalid;
        const SolMirMaterializedTerminator *term = &m->blocks[call->block].terminator;
        const SolMirRuntimeLoweredImageTerminator *row = &owner->image_terminators[call->block];
        const SolMirRuntimeSignature *signature = &conventions->signatures[call->signature];
        const SolMirLinkageCallable *callee = &linkage->callables[call->internal];
        const char *symbol = callee->symbol.bytes;
        size_t caller_callable = SOL_MIR_LINKAGE_NONE, caller_matches = 0;
        size_t binding_matches = 0;
        for (size_t i = 0; i < linkage->callable_count; ++i)
            if (linkage->callables[i].instance == call->image) {
                caller_callable = i;
                ++caller_matches;
            }
        for (size_t i = 0; i < linkage->binding_count; ++i)
            if (linkage->bindings[i].binding == term->binding
                && linkage->bindings[i].target_kind == SOL_MIR_LINKAGE_TARGET_INTERNAL
                && linkage->bindings[i].internal == call->internal
                && linkage->bindings[i].host == SOL_MIR_LINKAGE_NONE) ++binding_matches;
        if (term->kind != SOL_MIR_TERM_INVOKE || term->call_kind != SOL_IR_CALL_FUNCTION
            || term->normal_edge != call->normal_edge || term->failure_edge != call->failure_edge
            || term->writebacks.count != 0 || term->receiver.source_expression != SOL_IR_NONE
            || row->state != SOL_MIR_RUNTIME_LOWERED_PRESENT || row->image != call->image
            || row->block != call->block || row->kind != SOL_MIR_TERM_INVOKE || row->call != id
            || row->failure_site != call->failure_site || row->plan_family != SOL_MIR_RUNTIME_LOWERED_PLAN_CALLABLE
            || signature->origin != SOL_MIR_RUNTIME_SIGNATURE_INTERNAL
            || signature->internal != call->internal
            || signature_for(conventions, call->internal) != signature
            || (signature->result_class != SOL_MIR_RUNTIME_RESULT_VALUE
                && signature->result_class != SOL_MIR_RUNTIME_RESULT_UNIT)
            || !recipe_scalar(concrete, signature->result, false)
            || callee->instance >= m->image_count || symbol == NULL || symbol[0] == '\0'
            || symbol_for_image(concrete, callee->instance) == NULL
            || strcmp(symbol_for_image(concrete, callee->instance), symbol) != 0
            || call->operands.offset > conventions->operand_count
            || call->operands.count > conventions->operand_count - call->operands.offset
            || signature->slots.offset > conventions->signature_slot_count
            || signature->slots.count > conventions->signature_slot_count - signature->slots.offset
            || term->arguments.offset > m->call_argument_count
            || term->arguments.count > m->call_argument_count - term->arguments.offset
            || call->operands.count != signature->slots.count
            || term->arguments.count != signature->slots.count
            || caller_matches != 1 || binding_matches != 1
            || !image_edge_preflight(request, &m->images[call->image], call->image,
                call->block, call->normal_edge)
            || !image_edge_preflight(request, &m->images[call->image], call->image,
                call->block, call->failure_edge)) goto invalid;
        for (size_t ordinal = 0; ordinal < signature->slots.count; ++ordinal) {
            const SolMirRuntimeSignatureSlot *slot = &conventions->signature_slots[
                signature->slots.offset + ordinal];
            const SolMirRuntimeOperand *operand = &conventions->operands[
                call->operands.offset + ordinal];
            const SolMirMaterializedCallArgument *argument = &m->call_arguments[
                term->arguments.offset + ordinal];
            if (slot->role != SOL_MIR_RUNTIME_SLOT_PARAMETER || slot->formal != ordinal
                || slot->access != SOL_ACCESS_OWNED || !recipe_scalar(concrete, slot->recipe, false)
                || operand->signature_slot != signature->slots.offset + ordinal
                || operand->value.kind != SOL_MIR_RUNTIME_VALUE_MATERIALIZED_TEMPORARY
                || argument->formal != ordinal || argument->access != SOL_ACCESS_OWNED
                || operand->value.id != argument->temporary
                || argument->temporary >= m->temporary_count
                || argument->type >= concrete->layout.type_count
                || concrete->layout.types[argument->type].recipe != slot->recipe
                || m->temporaries[argument->temporary].type != argument->type
                || argument->temporary < m->images[call->image].temporaries.offset
                || argument->temporary - m->images[call->image].temporaries.offset
                    >= m->images[call->image].temporaries.count) goto invalid;
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
            || site->allowed_codes != (UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT - 1))
            || !scalar_call_cleanup(request, call, term, call->image, call->block,
                graph->resume_blocks)) goto invalid;
        graph->calls[id] = (ScalarCallCatalog){call->image, call->block, caller_callable, id, call->internal,
            call->signature, call->operands, signature->result_class == SOL_MIR_RUNTIME_RESULT_VALUE
                ? term->result : SOL_MIR_MATERIALIZED_NONE, signature->result_class,
            call->normal_edge, call->failure_edge, call->failure_site, 0, false};
        ++graph->edge_count;
    }
    if (owner->call_count != conventions->call_count) goto invalid;
    for (size_t block = 0; block < m->block_count; ++block)
        if (m->blocks[block].terminator.kind == SOL_MIR_TERM_RESUME_FAILURE
            && !graph->resume_blocks[block]) goto invalid;
    graph->count = conventions->call_count;
    if (linkage->callable_count != 0) {
        size_t *indegree = allocate(linkage->callable_count, sizeof *indegree);
        size_t *depth = allocate(linkage->callable_count, sizeof *depth);
        size_t *queue = allocate(linkage->callable_count, sizeof *queue);
        if (indegree == NULL || depth == NULL || queue == NULL) {
            deallocate(indegree); deallocate(depth); deallocate(queue); deallocate(graph->calls); deallocate(graph->resume_blocks);
            graph->calls = NULL; graph->resume_blocks = NULL;
            return scalar_catalog_allocation_result();
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
            ScalarCallCatalog *edge = &graph->calls[i];
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
    return SCALAR_CATALOG_VALID;
invalid:
    deallocate(graph->calls); deallocate(graph->resume_blocks);
    graph->calls = NULL; graph->resume_blocks = NULL; graph->count = 0;
    return SCALAR_CATALOG_INVALID;
}

static void scalar_call_catalog_free(ScalarCallCatalogGraph *graph) {
    deallocate(graph->calls);
    deallocate(graph->resume_blocks);
    memset(graph, 0, sizeof *graph);
}

static const ScalarCallCatalog *scalar_catalog_for(const ScalarCallCatalogGraph *graph,
    size_t image, size_t block) {
    if (graph == NULL) return NULL;
    const ScalarCallCatalog *found = NULL;
    for (size_t i = 0; i < graph->count; ++i) {
        const ScalarCallCatalog *call = &graph->calls[i];
        if (call->caller_image == image && call->caller_block == block) {
            if (found != NULL) return NULL;
            found = call;
        }
    }
    return found;
}

/* 3B admits only a proof that every emitted image is success-only.  This is
 * intentionally conservative: an otherwise unreachable checked operation or
 * unfamiliar failure producer defers the entire closure to 3C. */
static bool scalar_catalog_failures_supported(const SolWasmScalarBuildRequest *request,
    const ScalarCallCatalogGraph *graph) {
    const SolMirRuntimeCleanup *cleanup = request->program->cleanup;
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    if (graph->has_cycle || graph->longest_chain > 64) return false;
    for (size_t i = 0; i < cleanup->event_count; ++i) {
        const SolMirRuntimeCleanupEvent *event = &cleanup->events[i];
        if (event->producer != SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL
            && event->producer != SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_INVOKE
            && event->producer != SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_ARITHMETIC)
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
                if (!scalar_cleanup_action(request,
                        &cleanup->actions[transition->actions.offset + a])) return false;
        }
    }
    for (size_t i = 0; i < m->instruction_count; ++i) {
        const SolMirOperationArithmeticPlan *plan = arithmetic_for_instruction(request, i);
        if (plan != NULL && !infallible_opcode(plan) && !scalar_checked_opcode(plan)) return false;
    }
    return true;
}

static bool scalar_compound_chain(const SolMirConcreteProgram *concrete,
    const SolMirMaterializedImage *image, const SolMirMaterializedBlock *block,
    size_t instruction, const SolMirOperationArithmeticPlan *plan) {
    const SolMirMaterialization *m = &concrete->materialization;
    if (!plan->compound || plan->previous >= m->temporary_count
        || !whole_scalar_place(concrete, m->instructions[instruction].place)
        || !scalar_recipe(concrete, m->temporaries[plan->previous].type, false)
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

static bool scalar_function_preflight(const SolWasmScalarBuildRequest *request,
    size_t callable, const SolMirRuntimeSignature *signature,
    const ScalarCallCatalogGraph *catalog) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirMaterialization *materialization = &concrete->materialization;
    const SolMirLinkageCallable *linkage = &concrete->linkage.callables[callable];
    if (linkage->instance >= materialization->image_count || signature == NULL
        || signature->slots.offset > request->program->conventions->signature_slot_count
        || signature->slots.count > request->program->conventions->signature_slot_count
            - signature->slots.offset || !recipe_scalar(concrete, signature->result,
                signature->result_class == SOL_MIR_RUNTIME_RESULT_NEVER)) return false;
    for (size_t i = 0; i < signature->slots.count; ++i) {
        const SolMirRuntimeSignatureSlot *slot = &request->program->conventions->signature_slots[
            signature->slots.offset + i];
        if (slot->role != SOL_MIR_RUNTIME_SLOT_PARAMETER || slot->formal != i
            || slot->access != SOL_ACCESS_OWNED || !recipe_scalar(concrete, slot->recipe, false))
            return false;
    }
    const SolMirMaterializedImage *image = &materialization->images[linkage->instance];
    for (size_t i = 0; i < image->locals.count; ++i) {
        const SolMirMaterializedLocal *local = &materialization->locals[image->locals.offset + i];
        if (local->kind == SOL_MIR_MATERIALIZED_LOCAL_PARAMETER) {
            if (local->ordinal >= signature->slots.count) return false;
            const SolMirRuntimeSignatureSlot *slot = &request->program->conventions->signature_slots[
                signature->slots.offset + local->ordinal];
            if (slot->formal != local->ordinal || slot->access != SOL_ACCESS_OWNED
                || !recipe_scalar(concrete, slot->recipe, false)
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
            if (!scalar_instruction(concrete, item)) return false;
            if (item->result != SOL_MIR_MATERIALIZED_NONE
                && !image_value(concrete, image, item->result)) return false;
            switch (item->kind) {
                case SOL_MIR_INST_LOAD_COPY: case SOL_MIR_INST_LOAD_MOVE:
                case SOL_MIR_INST_LOAD_UPDATE: case SOL_MIR_INST_STORE:
                case SOL_MIR_INST_COMPOUND_UPDATE:
                case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED:
                    if (!image_place(concrete, image, item->place)) return false;
                    break;
                default: break;
            }
            if ((item->kind == SOL_MIR_INST_STORE || item->kind == SOL_MIR_INST_TEMPORARY_INIT
                    || item->kind == SOL_MIR_INST_EXPRESSION_RESULT)
                && !image_value(concrete, image, item->left)) return false;
            if (item->kind == SOL_MIR_INST_TEMPORARY_DROP
                && (item->temporary >= materialization->temporary_count
                    || !scalar_recipe(concrete,
                        materialization->temporaries[item->temporary].type, false))) return false;
            if (item->kind == SOL_MIR_INST_UNARY || item->kind == SOL_MIR_INST_BINARY
                || item->kind == SOL_MIR_INST_COMPOUND_UPDATE) {
                const SolMirOperationArithmeticPlan *plan = arithmetic_for_instruction(request,
                    instruction);
                if (plan == NULL || plan->compound != (item->kind == SOL_MIR_INST_COMPOUND_UPDATE)
                    || (plan->compound && (plan->previous >= materialization->temporary_count
                        || !scalar_recipe(concrete,
                            materialization->temporaries[plan->previous].type, false)))
                    || (!plan->compound && !image_value(concrete, image, plan->left))
                    || (plan->opcode != SOL_MIR_OPERATION_BOOL_NOT
                        && plan->opcode != SOL_MIR_OPERATION_I64_NEG
                        && !image_value(concrete, image, plan->right))
                    || (plan->compound && !scalar_compound_chain(concrete, image, block,
                        instruction, plan))
                    || (!infallible_opcode(plan) && !scalar_checked_opcode(plan))
                    || (scalar_checked_opcode(plan)
                        && !scalar_failure_route(request, instruction, plan))) return false;
            }
        }
        const SolMirMaterializedTerminator *term = &block->terminator;
        const ScalarCallCatalog *call = scalar_catalog_for(catalog, linkage->instance, block_id);
        if ((!scalar_terminator(term) && term->kind != SOL_MIR_TERM_RESUME_FAILURE)
            || (term->kind == SOL_MIR_TERM_INVOKE && call == NULL)
            || (term->kind != SOL_MIR_TERM_INVOKE && call != NULL)
            || (term->kind == SOL_MIR_TERM_RESUME_FAILURE
                && (catalog == NULL || !catalog->resume_blocks[block_id]))) return false;
        switch (term->kind) {
            case SOL_MIR_TERM_GOTO: case SOL_MIR_TERM_BREAK: case SOL_MIR_TERM_CONTINUE:
                if (!image_edge_preflight(request, image, linkage->instance, block_id, term->edge)) return false;
                break;
            case SOL_MIR_TERM_BRANCH:
                if (!image_value(concrete, image, term->condition)
                    || !image_edge_preflight(request, image, linkage->instance, block_id, term->true_edge)
                    || !image_edge_preflight(request, image, linkage->instance, block_id, term->false_edge)) return false;
                break;
            case SOL_MIR_TERM_RETURN:
                if (signature->result_class != SOL_MIR_RUNTIME_RESULT_UNIT
                    && !image_value(concrete, image, term->value)) return false;
                break;
            case SOL_MIR_TERM_INVOKE:
                break;
            case SOL_MIR_TERM_RESUME_FAILURE:
                break;
            default: return false;
        }
    }
    return true;
}

static BinaryenExpressionRef scalar_binary(const ScalarFunction *function,
    const SolMirOperationArithmeticPlan *plan) {
    BinaryenExpressionRef left = get_value(function, plan->left);
    BinaryenExpressionRef right = plan->opcode == SOL_MIR_OPERATION_BOOL_NOT ? NULL
        : get_value(function, plan->right);
    if (left == NULL || (plan->opcode != SOL_MIR_OPERATION_BOOL_NOT && right == NULL)) return NULL;
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

static BinaryenExpressionRef i64(const ScalarFunction *function, int64_t value) {
    return BinaryenConst(function->module, BinaryenLiteralInt64(value));
}

static BinaryenExpressionRef temporary_get(const ScalarFunction *function,
    SolMirMaterializedTemporaryId temporary) {
    size_t index = temporary_index(function, temporary);
    return index == SIZE_MAX ? NULL : BinaryenLocalGet(function->module,
        (BinaryenIndex)index, BinaryenTypeInt64());
}

static BinaryenExpressionRef checked_value(const ScalarFunction *function,
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

static BinaryenExpressionRef mul_overflow(const ScalarFunction *function,
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

static BinaryenExpressionRef checked_overflow(const ScalarFunction *function,
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

static bool scalar_edge(const ScalarFunction *function, size_t edge,
    ScalarNodes *nodes) {
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
    bool assigned = scalar_parallel_assign(function->module, nodes, arguments, destinations,
        source->arguments.count, function->scratch_base);
    deallocate(arguments); deallocate(destinations);
    if (!assigned) return false;
    if (!scalar_nodes_push(nodes, BinaryenLocalSet(function->module, (BinaryenIndex)function->pc,
            BinaryenConst(function->module, BinaryenLiteralInt32((int32_t)(source->block
                - function->image->blocks.offset)))))
        || !scalar_nodes_push(nodes, BinaryenBreak(function->module, "p42.dispatch", NULL, NULL)))
        return false;
    return true;
}

static bool scalar_cleanup_emit(const ScalarFunction *function,
    const SolMirRuntimeCleanupAction *action, ScalarNodes *nodes) {
    const SolMirMaterialization *m = &function->request->program->conventions->concrete->materialization;
    size_t init = SIZE_MAX;
    switch (action->kind) {
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_TEMPORARY:
            init = temporary_init_index(function, action->target); break;
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE:
            if (action->target < m->place_count)
                init = local_init_index(function, m->places[action->target].local);
            break;
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER:
            init = local_init_index(function, action->target); break;
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_SCOPE:
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_REGION:
        case SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE:
            return true;
        default: return false;
    }
    return init != SIZE_MAX && scalar_nodes_push(nodes, BinaryenLocalSet(function->module,
        (BinaryenIndex)init, BinaryenConst(function->module, BinaryenLiteralInt32(0))));
}

static bool scalar_failure_record_index(const SolWasmScalarBuildRequest *request,
    size_t site_id, size_t *index) {
    const SolMirRuntimeConventions *conventions = request->program->conventions;
    const SolMirConcreteProgram *concrete = conventions->concrete;
    const SolIr *ir = concrete->program.ir;
    if (site_id >= conventions->failure_site_count) return false;
    const SolMirRuntimeFailureSite *site = &conventions->failure_sites[site_id];
    const char *path = NULL, *symbol = symbol_for_image(concrete, site->owner);
    if (symbol == NULL || !provenance_source(ir, request->package_directory, site->source, &path))
        return false;
    size_t rank = 0;
    for (size_t i = 0; i < conventions->failure_site_count; ++i) {
        const SolMirRuntimeFailureSite *other = &conventions->failure_sites[i];
        const char *other_path = NULL, *other_symbol = symbol_for_image(concrete, other->owner);
        if (other_symbol == NULL || !provenance_source(ir, request->package_directory,
                other->source, &other_path)) return false;
        ProvenanceRecord a = {3, (uint8_t)other->origin_kind, other_path, other_symbol,
            other->source.start, other->source.end, 0};
        ProvenanceRecord b = {3, (uint8_t)site->origin_kind, path, symbol,
            site->source.start, site->source.end, 0};
        for (size_t q = 0; q < conventions->failure_site_count; ++q) {
            const SolMirRuntimeFailureSite *before = &conventions->failure_sites[q];
            if (before->owner == other->owner && (before->source.file < other->source.file
                || (before->source.file == other->source.file
                    && (before->source.start < other->source.start
                        || (before->source.start == other->source.start
                            && before->source.end < other->source.end))))) ++a.ordinal;
            if (before->owner == site->owner && (before->source.file < site->source.file
                || (before->source.file == site->source.file
                    && (before->source.start < site->source.start
                        || (before->source.start == site->source.start
                            && before->source.end < site->source.end))))) ++b.ordinal;
        }
        if (provenance_order(&a, &b) < 0) ++rank;
    }
    return scalar_add(conventions->entry_count, concrete->linkage.callable_count, index)
        && scalar_add(*index, rank, index) && scalar_add(*index, 1, index)
        && *index <= UINT32_MAX;
}

static bool scalar_failure_emit(const ScalarFunction *function, size_t instruction,
    SolMirRuntimeFailureCode code, ScalarNodes *nodes) {
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
    if (!scalar_failure_record_index(function->request, row->failure_site, &record)) return false;
    if (!scalar_nodes_push(nodes, BinaryenGlobalSet(function->module, P42_CODE,
            BinaryenConst(function->module, BinaryenLiteralInt32((int32_t)code))))
        || !scalar_nodes_push(nodes, BinaryenGlobalSet(function->module, P42_SITE,
            BinaryenConst(function->module, BinaryenLiteralInt32((int32_t)record)))))
        return false;
    for (size_t i = 0; i < failure->actions.count; ++i)
        if (!scalar_cleanup_emit(function, &cleanup->actions[failure->actions.offset + i], nodes))
            return false;
    return scalar_nodes_push(nodes, BinaryenReturn(function->module, i64(function, 0)));
}

static bool scalar_checked_emit(const ScalarFunction *function, size_t instruction,
    const SolMirOperationArithmeticPlan *plan, size_t destination, ScalarNodes *nodes) {
    BinaryenExpressionRef overflow = checked_overflow(function, plan);
    BinaryenExpressionRef success = checked_value(function, plan);
    if (overflow == NULL || success == NULL || destination == SIZE_MAX) return false;
    ScalarNodes overflow_nodes = {0};
    bool ok = scalar_failure_emit(function, instruction,
        SOL_MIR_RUNTIME_FAILURE_INTEGER_OVERFLOW, &overflow_nodes);
    BinaryenExpressionRef overflow_body = ok ? BinaryenBlock(function->module, NULL,
        overflow_nodes.items, (BinaryenIndex)overflow_nodes.count, BinaryenTypeNone()) : NULL;
    deallocate(overflow_nodes.items);
    BinaryenExpressionRef success_set = ok ? BinaryenLocalSet(function->module,
        (BinaryenIndex)destination, success) : NULL;
    if (plan->opcode == SOL_MIR_OPERATION_I64_DIV || plan->opcode == SOL_MIR_OPERATION_I64_REM) {
        BinaryenExpressionRef divisor = get_value(function, plan->right);
        ScalarNodes zero_nodes = {0};
        ok = ok && divisor != NULL && scalar_failure_emit(function, instruction,
            SOL_MIR_RUNTIME_FAILURE_DIVISION_BY_ZERO, &zero_nodes);
        BinaryenExpressionRef zero_body = ok ? BinaryenBlock(function->module, NULL,
            zero_nodes.items, (BinaryenIndex)zero_nodes.count, BinaryenTypeNone()) : NULL;
        deallocate(zero_nodes.items);
        BinaryenExpressionRef otherwise = ok ? BinaryenIf(function->module, overflow,
            overflow_body, success_set) : NULL;
        return ok && scalar_nodes_push(nodes, BinaryenIf(function->module,
            BinaryenBinary(function->module, BinaryenEqInt64(), divisor, i64(function, 0)),
            zero_body, otherwise));
    }
    return ok && scalar_nodes_push(nodes, BinaryenIf(function->module, overflow,
        overflow_body, success_set));
}

static bool scalar_instruction_emit(const ScalarFunction *function, size_t instruction,
    ScalarNodes *nodes) {
    const SolMirConcreteProgram *concrete = function->request->program->conventions->concrete;
    const SolMirMaterialization *materialization = &concrete->materialization;
    const SolMirMaterializedInstruction *item = &materialization->instructions[instruction];
    size_t destination = value_index(function, item->result);
    size_t local, init, temporary;
    switch (item->kind) {
        case SOL_MIR_INST_CONST_INT64:
            return destination != SIZE_MAX && scalar_nodes_push(nodes, BinaryenLocalSet(function->module,
                (BinaryenIndex)destination, BinaryenConst(function->module,
                    BinaryenLiteralInt64(item->integer))));
        case SOL_MIR_INST_CONST_BOOL:
            return destination != SIZE_MAX && scalar_nodes_push(nodes, BinaryenLocalSet(function->module,
                (BinaryenIndex)destination, BinaryenConst(function->module,
                    BinaryenLiteralInt64(item->boolean ? 1 : 0))));
        case SOL_MIR_INST_CONST_UNIT:
            return destination != SIZE_MAX && scalar_nodes_push(nodes, BinaryenLocalSet(function->module,
                (BinaryenIndex)destination, BinaryenConst(function->module, BinaryenLiteralInt64(0))));
        case SOL_MIR_INST_PARAMETER_LIVE:
            local = local_index(function, item->local); init = local_init_index(function, item->local);
            if (local == SIZE_MAX || init == SIZE_MAX || item->local >= materialization->local_count
                || materialization->locals[item->local].kind != SOL_MIR_MATERIALIZED_LOCAL_PARAMETER
                || materialization->locals[item->local].ordinal >= function->parameter_count) return false;
            return scalar_nodes_push(nodes, BinaryenLocalSet(function->module, (BinaryenIndex)local,
                    BinaryenLocalGet(function->module, (BinaryenIndex)materialization->locals[item->local].ordinal,
                        BinaryenTypeInt64()))) && scalar_nodes_push(nodes, BinaryenLocalSet(function->module,
                    (BinaryenIndex)init, BinaryenConst(function->module, BinaryenLiteralInt32(1))));
        case SOL_MIR_INST_STORAGE_LIVE:
        case SOL_MIR_INST_STORAGE_DEAD:
        case SOL_MIR_INST_DROP_IF_INITIALIZED:
            init = local_init_index(function, item->local);
            return init != SIZE_MAX && scalar_nodes_push(nodes, BinaryenLocalSet(function->module,
                (BinaryenIndex)init, BinaryenConst(function->module, BinaryenLiteralInt32(0))));
        case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED:
            init = item->place < materialization->place_count
                ? local_init_index(function, materialization->places[item->place].local) : SIZE_MAX;
            return init != SIZE_MAX && scalar_nodes_push(nodes, BinaryenLocalSet(function->module,
                (BinaryenIndex)init, BinaryenConst(function->module, BinaryenLiteralInt32(0))));
        case SOL_MIR_INST_LOAD_COPY:
        case SOL_MIR_INST_LOAD_MOVE:
        case SOL_MIR_INST_LOAD_UPDATE:
            local = item->place < materialization->place_count
                ? local_index(function, materialization->places[item->place].local) : SIZE_MAX;
            if (destination == SIZE_MAX || local == SIZE_MAX) return false;
            if (!scalar_nodes_push(nodes, BinaryenLocalSet(function->module, (BinaryenIndex)destination,
                    BinaryenLocalGet(function->module, (BinaryenIndex)local, BinaryenTypeInt64())))) return false;
            if (item->kind == SOL_MIR_INST_LOAD_MOVE) {
                init = local_init_index(function, materialization->places[item->place].local);
                return init != SIZE_MAX && scalar_nodes_push(nodes, BinaryenLocalSet(function->module,
                    (BinaryenIndex)init, BinaryenConst(function->module, BinaryenLiteralInt32(0))));
            }
            return true;
        case SOL_MIR_INST_STORE:
            local = item->place < materialization->place_count
                ? local_index(function, materialization->places[item->place].local) : SIZE_MAX;
            init = item->place < materialization->place_count
                ? local_init_index(function, materialization->places[item->place].local) : SIZE_MAX;
            return local != SIZE_MAX && init != SIZE_MAX && scalar_nodes_push(nodes,
                BinaryenLocalSet(function->module, (BinaryenIndex)local, get_value(function, item->left)))
                && scalar_nodes_push(nodes, BinaryenLocalSet(function->module, (BinaryenIndex)init,
                    BinaryenConst(function->module, BinaryenLiteralInt32(1))));
        case SOL_MIR_INST_TEMPORARY_INIT:
            temporary = temporary_index(function, item->temporary);
            init = temporary_init_index(function, item->temporary);
            return temporary != SIZE_MAX && init != SIZE_MAX && scalar_nodes_push(nodes,
                BinaryenLocalSet(function->module, (BinaryenIndex)temporary, get_value(function, item->left)))
                && scalar_nodes_push(nodes, BinaryenLocalSet(function->module, (BinaryenIndex)init,
                    BinaryenConst(function->module, BinaryenLiteralInt32(1))));
        case SOL_MIR_INST_TEMPORARY_DROP:
            init = temporary_init_index(function, item->temporary);
            return init != SIZE_MAX && scalar_nodes_push(nodes, BinaryenLocalSet(function->module,
                (BinaryenIndex)init, BinaryenConst(function->module, BinaryenLiteralInt32(0))));
        case SOL_MIR_INST_EXPRESSION_RESULT:
            return destination != SIZE_MAX && scalar_nodes_push(nodes, BinaryenLocalSet(function->module,
                (BinaryenIndex)destination, get_value(function, item->left)));
        case SOL_MIR_INST_UNARY:
        case SOL_MIR_INST_BINARY:
        case SOL_MIR_INST_COMPOUND_UPDATE: {
            const SolMirOperationArithmeticPlan *plan = arithmetic_for_instruction(
                function->request, instruction);
            bool ok;
            if (plan == NULL || destination == SIZE_MAX) return false;
            if (scalar_checked_opcode(plan)) ok = scalar_checked_emit(function, instruction,
                plan, destination, nodes);
            else {
                BinaryenExpressionRef value = scalar_binary(function, plan);
                ok = value != NULL && scalar_nodes_push(nodes, BinaryenLocalSet(function->module,
                    (BinaryenIndex)destination, value));
            }
            if (ok && plan->compound) {
                init = temporary_init_index(function, plan->previous);
                ok = init != SIZE_MAX && scalar_nodes_push(nodes, BinaryenLocalSet(function->module,
                    (BinaryenIndex)init, BinaryenConst(function->module, BinaryenLiteralInt32(0))));
            }
            return ok;
        }
        case SOL_MIR_INST_REGION_ENTER: case SOL_MIR_INST_REGION_EXIT:
        case SOL_MIR_INST_SCOPE_ENTER: case SOL_MIR_INST_SCOPE_EXIT:
        case SOL_MIR_INST_CAPTURE_SNAPSHOT:
            return true;
        default:
            return false;
    }
}

static bool scalar_call_emit(const ScalarFunction *function, size_t block,
    ScalarNodes *nodes) {
    const SolMirRuntimeLoweredProgram *owner = function->request->program;
    const SolMirRuntimeConventions *conventions = owner->conventions;
    const SolMirMaterialization *m = &conventions->concrete->materialization;
    const ScalarCallCatalog *call = scalar_catalog_for(function->catalog, function->image_id, block);
    if (call == NULL || call->callee_callable >= conventions->concrete->linkage.callable_count)
        return false;
    const SolMirMaterializedTerminator *term = &m->blocks[block].terminator;
    BinaryenExpressionRef *arguments = call->operands.count == 0 ? NULL
        : allocate(call->operands.count, sizeof *arguments);
    if (call->operands.count != 0 && arguments == NULL) return false;
    for (size_t i = 0; i < call->operands.count; ++i) {
        const SolMirRuntimeOperand *operand = &conventions->operands[call->operands.offset + i];
        arguments[i] = temporary_get(function, operand->value.id);
        if (arguments[i] == NULL) { deallocate(arguments); return false; }
    }
    const char *symbol = conventions->concrete->linkage.callables[call->callee_callable].symbol.bytes;
    BinaryenExpressionRef invoke = BinaryenCall(function->module, symbol, arguments,
        (BinaryenIndex)call->operands.count, BinaryenTypeInt64());
    deallocate(arguments);
    if (invoke == NULL) return false;
    if (call->result_class == SOL_MIR_RUNTIME_RESULT_VALUE) {
        if (!scalar_nodes_push(nodes, BinaryenLocalSet(function->module,
                (BinaryenIndex)function->scratch_base, invoke))) return false;
    } else if (!scalar_nodes_push(nodes, BinaryenDrop(function->module, invoke))) return false;
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
    ScalarNodes success = {0};
    bool ok = true;
    if (call->result_class == SOL_MIR_RUNTIME_RESULT_VALUE) {
        size_t result = value_index(function, call->result);
        ok = result != SIZE_MAX && scalar_nodes_push(&success, BinaryenLocalSet(function->module,
            (BinaryenIndex)result, BinaryenLocalGet(function->module,
                (BinaryenIndex)function->scratch_base, BinaryenTypeInt64())));
    }
    for (size_t i = 0; ok && i < normal->actions.count; ++i)
        ok = scalar_cleanup_emit(function,
            &owner->cleanup->actions[normal->actions.offset + i], &success);
    if (ok) ok = scalar_edge(function, call->normal_edge, &success);
    BinaryenExpressionRef success_body = ok ? BinaryenBlock(function->module, NULL, success.items,
        (BinaryenIndex)success.count, BinaryenTypeNone()) : NULL;
    deallocate(success.items);
    if (!ok) return false;
    ScalarNodes failed = {0};
    for (size_t i = 0; ok && i < failure->actions.count; ++i)
        ok = scalar_cleanup_emit(function,
            &owner->cleanup->actions[failure->actions.offset + i], &failed);
    if (ok) ok = scalar_edge(function, call->failure_edge, &failed);
    BinaryenExpressionRef failure_body = ok ? BinaryenBlock(function->module, NULL, failed.items,
        (BinaryenIndex)failed.count, BinaryenTypeNone()) : NULL;
    deallocate(failed.items);
    if (!ok) return false;
    BinaryenExpressionRef packet = BinaryenBinary(function->module, BinaryenNeInt32(),
        BinaryenGlobalGet(function->module, P42_CODE, BinaryenTypeInt32()),
        BinaryenConst(function->module, BinaryenLiteralInt32(0)));
    return scalar_nodes_push(nodes, BinaryenIf(function->module, packet,
        failure_body, success_body));
}

static bool scalar_resume_failure_emit(const ScalarFunction *function, size_t block,
    ScalarNodes *nodes) {
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
    for (size_t i = 0; i < pending->actions.count; ++i)
        if (!scalar_cleanup_emit(function,
                &owner->cleanup->actions[pending->actions.offset + i], nodes)) return false;
    return scalar_nodes_push(nodes, BinaryenReturn(function->module, i64(function, 0)));
}

static bool scalar_terminator_emit(const ScalarFunction *function, size_t block,
    ScalarNodes *nodes) {
    const SolMirMaterializedTerminator *term = &function->request->program->conventions
        ->concrete->materialization.blocks[block].terminator;
    switch (term->kind) {
        case SOL_MIR_TERM_GOTO: case SOL_MIR_TERM_BREAK: case SOL_MIR_TERM_CONTINUE:
            return scalar_edge(function, term->edge, nodes);
        case SOL_MIR_TERM_BRANCH: {
            ScalarNodes left = {0}, right = {0};
            bool ok = scalar_edge(function, term->true_edge, &left)
                && scalar_edge(function, term->false_edge, &right);
            BinaryenExpressionRef condition = get_value(function, term->condition);
            BinaryenExpressionRef yes = ok ? BinaryenBlock(function->module, NULL, left.items,
                (BinaryenIndex)left.count, BinaryenTypeNone()) : NULL;
            BinaryenExpressionRef no = ok ? BinaryenBlock(function->module, NULL, right.items,
                (BinaryenIndex)right.count, BinaryenTypeNone()) : NULL;
            deallocate(left.items); deallocate(right.items);
            return ok && condition != NULL && scalar_nodes_push(nodes, BinaryenIf(function->module,
                BinaryenBinary(function->module, BinaryenNeInt64(), condition,
                    BinaryenConst(function->module, BinaryenLiteralInt64(0))), yes, no));
        }
        case SOL_MIR_TERM_RETURN: {
            BinaryenExpressionRef value = function->signature->result_class == SOL_MIR_RUNTIME_RESULT_UNIT
                ? BinaryenConst(function->module, BinaryenLiteralInt64(0))
                : get_value(function, term->value);
            if (value == NULL) return false;
            return scalar_nodes_push(nodes, BinaryenReturn(function->module, value));
        }
        case SOL_MIR_TERM_INVOKE:
            return function->calls_enabled && scalar_call_emit(function, block, nodes);
        case SOL_MIR_TERM_RESUME_FAILURE:
            return function->calls_enabled && scalar_resume_failure_emit(function, block, nodes);
        default:
            return false;
    }
}

static bool scalar_function_emit(const SolWasmScalarBuildRequest *request,
    BinaryenModuleRef module, size_t callable, const SolMirRuntimeSignature *signature,
    const char *name, const ScalarCallCatalogGraph *catalog) {
    const SolMirConcreteProgram *concrete = request->program->conventions->concrete;
    const SolMirMaterialization *materialization = &concrete->materialization;
    const SolMirMaterializedImage *image = &materialization->images[
        concrete->linkage.callables[callable].instance];
    size_t scratch_count = catalog == NULL ? 0 : 1;
    for (size_t i = 0; i < image->blocks.count; ++i) {
        const SolMirMaterializedBlock *block = &materialization->blocks[image->blocks.offset + i];
        if (block->parameters.count > scratch_count) scratch_count = block->parameters.count;
    }
    size_t physical = 0;
    if (!scalar_function_local_count(request, callable, signature, &physical)
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
        + scratch_count; ++n)
        types[n] = BinaryenTypeInt64();
    for (; n < variables; ++n) types[n] = BinaryenTypeInt32();
    for (size_t i = 0; i < signature->slots.count; ++i) parameters[i] = BinaryenTypeInt64();
    ScalarFunction function = {
        .request = request, .module = module, .image = image,
        .signature = signature, .image_id = concrete->linkage.callables[callable].instance,
        .catalog = catalog, .calls_enabled = catalog != NULL,
        .parameter_count = signature->slots.count,
        .value_base = signature->slots.count,
        .temporary_base = signature->slots.count + image->values.count,
        .local_base = signature->slots.count + image->values.count + image->temporaries.count,
    };
    function.scratch_base = function.local_base + image->locals.count;
    function.temporary_init_base = function.scratch_base + scratch_count;
    function.local_init_base = function.temporary_init_base + image->temporaries.count;
    function.pc = function.local_init_base + image->locals.count;
    ScalarNodes body = {0}, dispatch = {0};
    bool ok = image->entry >= image->blocks.offset
        && image->entry - image->blocks.offset < image->blocks.count;
    if (ok) ok = scalar_nodes_push(&body, BinaryenLocalSet(module, (BinaryenIndex)function.pc,
            BinaryenConst(module, BinaryenLiteralInt32((int32_t)(image->entry - image->blocks.offset)))));
    for (size_t b = 0; ok && b < image->blocks.count; ++b) {
        size_t block = image->blocks.offset + b;
        ScalarNodes block_nodes = {0};
        const SolMirMaterializedBlock *item = &materialization->blocks[block];
        for (size_t i = 0; ok && i < item->instructions.count; ++i)
            ok = scalar_instruction_emit(&function, item->instructions.offset + i, &block_nodes);
        if (ok) ok = scalar_terminator_emit(&function, block, &block_nodes);
        BinaryenExpressionRef contents = ok ? BinaryenBlock(module, NULL, block_nodes.items,
            (BinaryenIndex)block_nodes.count, BinaryenTypeNone()) : NULL;
        deallocate(block_nodes.items);
        if (ok) ok = scalar_nodes_push(&dispatch, BinaryenIf(module,
            BinaryenBinary(module, BinaryenEqInt32(), BinaryenLocalGet(module,
                (BinaryenIndex)function.pc, BinaryenTypeInt32()), BinaryenConst(module,
                    BinaryenLiteralInt32((int32_t)b))), contents, NULL));
    }
    if (ok) ok = scalar_nodes_push(&dispatch, BinaryenReturn(module,
        BinaryenConst(module, BinaryenLiteralInt64(0))));
    BinaryenExpressionRef loop = ok ? BinaryenLoop(module, "p42.dispatch",
        BinaryenBlock(module, NULL, dispatch.items, (BinaryenIndex)dispatch.count,
            BinaryenTypeNone())) : NULL;
    if (ok) ok = scalar_nodes_push(&body, loop);
    BinaryenExpressionRef final = ok ? BinaryenBlock(module, NULL, body.items,
        (BinaryenIndex)body.count, BinaryenTypeNone()) : NULL;
    BinaryenType parameter_type = signature->slots.count == 0 ? BinaryenTypeNone()
        : BinaryenTypeCreate(parameters, (BinaryenIndex)signature->slots.count);
    if (ok) ok = BinaryenAddFunction(module, name, parameter_type, BinaryenTypeInt64(),
        types, (BinaryenIndex)variables, final) != NULL;
    deallocate(dispatch.items); deallocate(body.items); deallocate(types); deallocate(parameters);
    return ok;
}

typedef struct { size_t callable; const SolMirLinkage *linkage; } ScalarCallableOrder;
static int callable_order_compare(const void *left, const void *right) {
    const ScalarCallableOrder *a = left, *b = right;
    return strcmp(a->linkage->callables[a->callable].symbol.bytes,
        b->linkage->callables[b->callable].symbol.bytes);
}

typedef struct { size_t entry; const SolMirRuntimeConventions *conventions; } ScalarEntryOrder;
static int entry_order_compare(const void *left, const void *right) {
    const ScalarEntryOrder *a = left, *b = right;
    return strcmp(a->conventions->entries[a->entry].symbol.bytes,
        b->conventions->entries[b->entry].symbol.bytes);
}

static bool scalar_entry_wrapper_emit(BinaryenModuleRef module,
    const SolMirRuntimeEntry *entry, const SolMirLinkage *linkage,
    const SolMirRuntimeConventions *conventions) {
    if (entry->callable >= linkage->callable_count) return false;
    const SolMirRuntimeSignature *signature = signature_for(conventions, entry->callable);
    if (signature == NULL || signature->slots.count != 0) return false;
    BinaryenExpressionRef body_items[] = {
        BinaryenGlobalSet(module, P42_CODE, BinaryenConst(module, BinaryenLiteralInt32(0))),
        BinaryenGlobalSet(module, P42_SITE, BinaryenConst(module, BinaryenLiteralInt32(0))),
        BinaryenReturn(module, BinaryenCall(module, linkage->callables[entry->callable].symbol.bytes,
            NULL, 0, BinaryenTypeInt64())),
    };
    if (body_items[0] == NULL || body_items[1] == NULL || body_items[2] == NULL
        || BinaryenAddFunction(module, entry->symbol.bytes, BinaryenTypeNone(), BinaryenTypeInt64(),
            NULL, 0, BinaryenBlock(module, NULL, body_items, 3, BinaryenTypeNone())) == NULL)
        return false;
    BinaryenAddFunctionExport(module, entry->symbol.bytes, entry->symbol.bytes);
    return true;
}

static bool wasmtime_validate(const SolWasmBackendBytes *bytes) {
    wasm_engine_t *engine = wasm_engine_new();
    wasm_store_t *store = engine == NULL ? NULL : wasm_store_new(engine);
    wasm_byte_vec_t input = {bytes->count, (wasm_byte_t *)bytes->bytes};
    bool valid = store != NULL && wasm_module_validate(store, &input);
    if (store != NULL) wasm_store_delete(store);
    if (engine != NULL) wasm_engine_delete(engine);
    return valid;
}

SolWasmScalarResult sol_wasm_scalar_validate_scalar(const SolWasmBackendBytes *bytes) {
    if (bytes == NULL || bytes->bytes == NULL || bytes->count == 0)
        return SOL_WASM_SCALAR_INVALID_ARGUMENT;
    return wasmtime_validate(bytes) ? SOL_WASM_SCALAR_OK
        : SOL_WASM_SCALAR_WASMTIME_VALIDATION_FAILED;
}

SolWasmScalarResult sol_wasm_scalar_build_scalar(
    const SolWasmScalarBuildRequest *request, SolWasmScalarOutput *output,
    SolDiagnostics *diagnostics) {
    if (output == NULL) return SOL_WASM_SCALAR_INVALID_ARGUMENT;
    sol_wasm_scalar_output_free(output);
    if (request == NULL || request->program == NULL || request->package_directory == NULL
        || (request->limits != NULL && !limits_zero(request->limits)
            && !limits_complete(request->limits))) {
        diagnostic(diagnostics, "invalid scalar Wasm build request");
        return SOL_WASM_SCALAR_INVALID_ARGUMENT;
    }
    SolWasmScalarLimits limits = request->limits == NULL || limits_zero(request->limits)
        ? sol_wasm_scalar_default_limits() : *request->limits;
    SolWasmScalarUsage usage = {0};
    if (!scalar_owner(request, &usage)) {
        diagnostic(diagnostics, "P4.2 requires an authenticated scalar-only closure");
        return SOL_WASM_SCALAR_UNSUPPORTED_CLOSURE;
    }
    if (!scalar_usage_census(request, &usage)) {
        diagnostic(diagnostics, "P4.2 scalar resource census overflowed or was inconsistent");
        return SOL_WASM_SCALAR_RESOURCE_EXHAUSTED;
    }
    if (!scalar_cleanup(request)) {
        diagnostic(diagnostics, "P4.2 Slice 1 rejected non-scalar cleanup");
        return SOL_WASM_SCALAR_UNSUPPORTED_CLOSURE;
    }
    const SolMirLinkage *linkage = &request->program->conventions->concrete->linkage;
    const SolMirRuntimeConventions *conventions = request->program->conventions;
    for (size_t i = 0; i < linkage->callable_count; ++i) {
        size_t locals = 0;
        if (!scalar_function_local_count(request, i, signature_for(conventions, i), &locals)
            || !scalar_add(usage.locals, locals, &usage.locals)) {
            diagnostic(diagnostics, "P4.2 scalar local census overflowed");
            return SOL_WASM_SCALAR_RESOURCE_EXHAUSTED;
        }
    }
    if (usage.functions > limits.max_functions || usage.blocks > limits.max_blocks
        || usage.edges > limits.max_edges || usage.values > limits.max_values
        || usage.locals > limits.max_locals
        || usage.provenance_records > limits.max_provenance_records) {
        diagnostic(diagnostics, "P4.2 scalar Wasm resource limit exceeded");
        return SOL_WASM_SCALAR_RESOURCE_EXHAUSTED;
    }
    ScalarAccounting accounting = {&limits, &usage, 0, 0, SCALAR_BACKEND_OK};
    scalar_accounting = &accounting;
    ScalarCallCatalogGraph catalog = {0};
    const ScalarCallCatalogGraph *active_catalog = NULL;
    if (request->program->conventions->call_count != 0) {
        ScalarCatalogResult catalog_result = scalar_call_catalog(request, &limits, &catalog);
        if (catalog_result == SCALAR_CATALOG_RESOURCE) {
            diagnostic(diagnostics, "P4.2 scalar call catalog resource limit exceeded");
            scalar_accounting = NULL; return SOL_WASM_SCALAR_RESOURCE_EXHAUSTED;
        }
        if (catalog_result == SCALAR_CATALOG_ALLOCATION) {
            diagnostic(diagnostics, "P4.2 scalar call catalog allocation failed");
            SolWasmScalarResult result = scalar_backend_result();
            scalar_accounting = NULL; return result;
        }
        if (catalog_result != SCALAR_CATALOG_VALID) {
            diagnostic(diagnostics, "P4.2 rejected an unauthenticated scalar call catalog");
            SolWasmScalarResult result = accounting.status == SCALAR_BACKEND_OK
                ? SOL_WASM_SCALAR_UNSUPPORTED_CLOSURE : scalar_backend_result();
            scalar_accounting = NULL; return result;
        }
        if (!scalar_catalog_failures_supported(request, &catalog)) {
            scalar_call_catalog_free(&catalog);
            diagnostic(diagnostics, "P4.2 deferred an unsupported, cyclic, or deep call closure");
            scalar_accounting = NULL; return SOL_WASM_SCALAR_UNSUPPORTED_CLOSURE;
        }
        active_catalog = &catalog;
    }
    ScalarCallableOrder *callables = linkage->callable_count == 0 ? NULL
        : allocate(linkage->callable_count, sizeof *callables);
    ScalarEntryOrder *entries = conventions->entry_count == 0 ? NULL
        : allocate(conventions->entry_count, sizeof *entries);
    if ((linkage->callable_count != 0 && callables == NULL)
        || (conventions->entry_count != 0 && entries == NULL)) {
        deallocate(callables); deallocate(entries); scalar_call_catalog_free(&catalog);
        SolWasmScalarResult result = scalar_backend_result();
        scalar_accounting = NULL; return result;
    }
    for (size_t i = 0; i < linkage->callable_count; ++i) {
        callables[i] = (ScalarCallableOrder){i, linkage};
        const SolMirRuntimeSignature *signature = signature_for(conventions, i);
        if (!scalar_function_preflight(request, i, signature, active_catalog)) {
            deallocate(callables); deallocate(entries); scalar_call_catalog_free(&catalog);
            diagnostic(diagnostics, "P4.2 Slice 1 rejected a non-infallible scalar function");
            scalar_accounting = NULL; return SOL_WASM_SCALAR_UNSUPPORTED_CLOSURE;
        }
    }
    for (size_t i = 0; i < conventions->entry_count; ++i) {
        entries[i] = (ScalarEntryOrder){i, conventions};
        if (conventions->entries[i].callable >= linkage->callable_count) {
            deallocate(callables); deallocate(entries); scalar_call_catalog_free(&catalog);
            scalar_accounting = NULL; return SOL_WASM_SCALAR_INVALID_INPUT;
        }
    }
    qsort(callables, linkage->callable_count, sizeof *callables, callable_order_compare);
    qsort(entries, conventions->entry_count, sizeof *entries, entry_order_compare);
    for (size_t i = 1; i < linkage->callable_count; ++i)
        if (callable_order_compare(&callables[i - 1], &callables[i]) == 0) {
            deallocate(callables); deallocate(entries); scalar_call_catalog_free(&catalog);
            scalar_accounting = NULL; return SOL_WASM_SCALAR_INVALID_INPUT;
        }
    for (size_t i = 1; i < conventions->entry_count; ++i)
        if (entry_order_compare(&entries[i - 1], &entries[i]) == 0) {
            deallocate(callables); deallocate(entries); scalar_call_catalog_free(&catalog);
            scalar_accounting = NULL; return SOL_WASM_SCALAR_INVALID_INPUT;
        }
    size_t provenance_size = 0;
    uint8_t *section = provenance(request, &provenance_size);
    if (section == NULL) { deallocate(callables); deallocate(entries); scalar_call_catalog_free(&catalog); SolWasmScalarResult result = scalar_backend_result(); scalar_accounting = NULL; return result; }
    BinaryenModuleRef module = BinaryenModuleCreate();
    if (module == NULL) { deallocate(section); deallocate(callables); deallocate(entries); scalar_call_catalog_free(&catalog); scalar_accounting = NULL; return SOL_WASM_SCALAR_ALLOCATION_FAILED; }
    BinaryenModuleSetFeatures(module, BinaryenFeatureMVP() | BinaryenFeatureMutableGlobals());
    bool ok = BinaryenAddGlobal(module, P42_CODE, BinaryenTypeInt32(), true,
        BinaryenConst(module, BinaryenLiteralInt32(0))) != NULL
        && BinaryenAddGlobal(module, P42_SITE, BinaryenTypeInt32(), true,
        BinaryenConst(module, BinaryenLiteralInt32(0))) != NULL;
    if (ok) {
        BinaryenAddGlobalExport(module, P42_CODE, SOL_WASM_SCALAR_FAILURE_CODE_EXPORT);
        BinaryenAddGlobalExport(module, P42_SITE, SOL_WASM_SCALAR_FAILURE_SITE_EXPORT);
    }
    for (size_t i = 0; ok && i < linkage->callable_count; ++i) {
        size_t callable = callables[i].callable;
        ok = scalar_function_emit(request, module, callable,
            signature_for(conventions, callable), linkage->callables[callable].symbol.bytes,
            active_catalog);
    }
    for (size_t i = 0; ok && i < conventions->entry_count; ++i) {
        const SolMirRuntimeEntry *entry = &conventions->entries[entries[i].entry];
        if (entry->callable >= linkage->callable_count) { ok = false; break; }
        ok = scalar_entry_wrapper_emit(module, entry, linkage, conventions);
    }
    if (ok) BinaryenAddCustomSection(module, SOL_WASM_SCALAR_PROVENANCE_SECTION,
        (const char *)section, (BinaryenIndex)provenance_size);
    deallocate(section); deallocate(callables); deallocate(entries); scalar_call_catalog_free(&catalog);
    if (accounting.status != SCALAR_BACKEND_OK) {
        BinaryenModuleDispose(module);
        SolWasmScalarResult result = scalar_backend_result();
        scalar_accounting = NULL; return result;
    }
    if (!ok || !BinaryenModuleValidate(module)) {
        BinaryenModuleDispose(module);
        diagnostic(diagnostics, "Binaryen rejected scalar Wasm module");
        scalar_accounting = NULL; return SOL_WASM_SCALAR_BINARYEN_VALIDATION_FAILED;
    }
    BinaryenModuleAllocateAndWriteResult serialized = BinaryenModuleAllocateAndWrite(module, NULL);
    BinaryenModuleDispose(module);
    if (serialized.binary == NULL || serialized.binaryBytes == 0) {
        free(serialized.binary);
        scalar_accounting = NULL; return SOL_WASM_SCALAR_SERIALIZATION_FAILED;
    }
    if (serialized.binaryBytes > limits.max_output_bytes) {
        free(serialized.binary);
        scalar_accounting = NULL; return SOL_WASM_SCALAR_RESOURCE_EXHAUSTED;
    }
    if (!scalar_transfer_output(&accounting, serialized.binaryBytes)) {
        free(serialized.binary);
        scalar_accounting = NULL; return SOL_WASM_SCALAR_RESOURCE_EXHAUSTED;
    }
    SolWasmBackendBytes bytes = {(uint8_t *)serialized.binary, serialized.binaryBytes};
    if (!wasmtime_validate(&bytes)) {
        free(serialized.binary);
        diagnostic(diagnostics, "Wasmtime rejected scalar Wasm module");
        scalar_accounting = NULL; return SOL_WASM_SCALAR_WASMTIME_VALIDATION_FAILED;
    }
    usage.output_bytes = bytes.count;
    output->bytes = bytes;
    output->usage = usage;
    scalar_accounting = NULL;
    return SOL_WASM_SCALAR_OK;
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
SolWasmScalarResult sol_wasm_scalar_test_call_catalog(
    const SolWasmScalarBuildRequest *request, SolWasmScalarTestCallCatalogEntry *entries,
    size_t cap, SolWasmScalarTestCallCatalog *summary) {
    if (summary == NULL || (cap != 0 && entries == NULL) || request == NULL
        || request->program == NULL || request->package_directory == NULL
        || (request->limits != NULL && !limits_zero(request->limits)
            && !limits_complete(request->limits))) return SOL_WASM_SCALAR_INVALID_ARGUMENT;
    SolWasmScalarLimits limits = request->limits == NULL || limits_zero(request->limits)
        ? sol_wasm_scalar_default_limits() : *request->limits;
    SolWasmScalarUsage ignored = {0};
    memset(summary, 0, sizeof *summary);
    if (!scalar_owner(request, &ignored) || !scalar_cleanup(request))
        return SOL_WASM_SCALAR_UNSUPPORTED_CLOSURE;
    ScalarCallCatalogGraph catalog;
    ScalarCatalogResult result = scalar_call_catalog(request, &limits, &catalog);
    if (result == SCALAR_CATALOG_RESOURCE) return SOL_WASM_SCALAR_RESOURCE_EXHAUSTED;
    if (result == SCALAR_CATALOG_ALLOCATION) return SOL_WASM_SCALAR_ALLOCATION_FAILED;
    if (result != SCALAR_CATALOG_VALID) return SOL_WASM_SCALAR_UNSUPPORTED_CLOSURE;
    summary->calls = catalog.count;
    summary->edges = catalog.edge_count;
    summary->work_bytes = catalog.work_bytes;
    summary->longest_chain = catalog.longest_chain;
    summary->has_cycle = catalog.has_cycle;
    if (entries == NULL && cap == 0) {
        scalar_call_catalog_free(&catalog);
        return SOL_WASM_SCALAR_OK;
    }
    if (cap < catalog.count) {
        scalar_call_catalog_free(&catalog);
        return SOL_WASM_SCALAR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < catalog.count; ++i) {
        const ScalarCallCatalog *call = &catalog.calls[i];
        entries[i] = (SolWasmScalarTestCallCatalogEntry){
            call->caller_image, call->caller_block, call->call, call->callee_callable,
            call->signature, call->operands.count, call->result, call->result_class,
            call->normal_edge, call->failure_edge, call->failure_site,
            call->cyclic, call->chain_depth};
    }
    scalar_call_catalog_free(&catalog);
    return SOL_WASM_SCALAR_OK;
}

bool sol_wasm_scalar_test_call_catalog_operand(const SolWasmScalarBuildRequest *request,
    size_t call, size_t ordinal, SolMirMaterializedTemporaryId *temporary) {
    if (temporary == NULL) return false;
    SolWasmScalarTestCallCatalog summary;
    SolWasmScalarResult result = sol_wasm_scalar_test_call_catalog(request, NULL, 0, &summary);
    if (result != SOL_WASM_SCALAR_OK) return false;
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

void sol_wasm_scalar_test_fail_allocation_after(size_t attempt) { fail_allocation_after = attempt; }
size_t sol_wasm_scalar_test_allocation_attempts(void) { return allocation_attempts; }
bool sol_wasm_scalar_test_parallel_moves(void) {
    BinaryenModuleRef module = BinaryenModuleCreate();
    if (module == NULL) return false;
    BinaryenModuleSetFeatures(module, BinaryenFeatureMVP());
    BinaryenType parameters[] = {BinaryenTypeInt64(), BinaryenTypeInt64()};
    ScalarNodes body = {0};
    BinaryenIndex destinations[] = {2, 3};
    BinaryenExpressionRef swap[] = {BinaryenLocalGet(module, 3, BinaryenTypeInt64()),
        BinaryenLocalGet(module, 2, BinaryenTypeInt64())};
    BinaryenExpressionRef repeated[] = {BinaryenLocalGet(module, 2, BinaryenTypeInt64()),
        BinaryenLocalGet(module, 2, BinaryenTypeInt64())};
    bool ok = scalar_nodes_push(&body, BinaryenLocalSet(module, 2,
        BinaryenLocalGet(module, 0, BinaryenTypeInt64())))
        && scalar_nodes_push(&body, BinaryenLocalSet(module, 3,
            BinaryenLocalGet(module, 1, BinaryenTypeInt64())))
        && scalar_parallel_assign(module, &body, swap, destinations, 2, 4)
        && scalar_parallel_assign(module, &body, repeated, destinations, 2, 4)
        && scalar_nodes_push(&body, BinaryenReturn(module, BinaryenBinary(module, BinaryenOrInt64(),
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
bool sol_wasm_scalar_test_rejects_capture_snapshot(void) {
    return !scalar_instruction_kind_supported(SOL_MIR_INST_CAPTURE_SNAPSHOT);
}
bool sol_wasm_scalar_test_rejects_resume_failure(void) {
    return !scalar_terminator(&(SolMirMaterializedTerminator){
        .kind = SOL_MIR_TERM_RESUME_FAILURE});
}
bool sol_wasm_scalar_test_size_add_overflow(void) {
    size_t result = 0;
    return !scalar_add(SIZE_MAX, 1, &result) && result == 0;
}
#endif
