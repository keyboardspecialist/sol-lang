#include "sol/mir_layout.h"

#include "sol/effects.h"
#include "sol/lexer.h"
#include "sol/ownership.h"
#include "sol/package.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
            #condition); \
        ++failures; \
    } \
} while (0)

typedef struct {
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree syntax;
    SolHirModule hir;
    SolTypeTable types;
    SolEffectTable effects;
    SolContractTable contracts;
    SolIr ir;
} Compilation;

typedef struct {
    SolMirProgram program;
    SolMirPlan plan;
    SolMirMaterialization materialization;
    SolMirRepresentation representation;
    SolMirLayout layout;
    SolDiagnostics diagnostics;
} Pipeline;

static bool compile_text(Compilation *c, const char *text) {
    memset(c, 0, sizeof(*c));
    sol_tokens_init(&c->tokens); sol_diagnostics_init(&c->diagnostics);
    sol_syntax_tree_init(&c->syntax); sol_hir_module_init(&c->hir);
    sol_type_table_init(&c->types); sol_effect_table_init(&c->effects);
    sol_contract_table_init(&c->contracts); sol_ir_init(&c->ir);
    return sol_source_from_text(&c->source, "layout.sol", text)
        && sol_lex(&c->source, &c->tokens, &c->diagnostics)
        && sol_parse(&c->source, &c->tokens, &c->syntax, &c->diagnostics)
        && sol_hir_lower(&c->source, &c->syntax, &c->hir, &c->diagnostics)
        && sol_type_check(&c->source, &c->syntax, &c->hir, &c->types,
            &c->diagnostics)
        && sol_effect_check(&c->source, &c->syntax, &c->hir, &c->types,
            &c->effects, &c->diagnostics)
        && sol_contract_lower(&c->source, &c->syntax, &c->hir, &c->types,
            &c->effects, &c->contracts, &c->diagnostics)
        && sol_ir_lower(&c->source, &c->syntax, &c->hir, &c->types,
            &c->effects, &c->contracts, &c->ir, &c->diagnostics);
}

static void free_compilation(Compilation *c) {
    sol_ir_free(&c->ir); sol_contract_table_free(&c->contracts);
    sol_effect_table_free(&c->effects); sol_type_table_free(&c->types);
    sol_hir_module_free(&c->hir); sol_syntax_tree_free(&c->syntax);
    sol_tokens_free(&c->tokens); sol_source_free(&c->source);
    sol_diagnostics_free(&c->diagnostics);
}

static SolIrCallableId callable(const SolIr *ir, const char *name,
    SolIrCallableKind kind) {
    for (size_t i = 0; i < ir->callable_count; ++i)
        if (ir->callables[i].kind == kind
            && strcmp(ir->callables[i].name, name) == 0) return i;
    return SOL_IR_NONE;
}

static void pipeline_init(Pipeline *p) {
    memset(p, 0, sizeof(*p));
    sol_mir_program_init(&p->program); sol_mir_plan_init(&p->plan);
    sol_mir_materialization_init(&p->materialization);
    sol_mir_representation_init(&p->representation);
    sol_mir_layout_init(&p->layout); sol_diagnostics_init(&p->diagnostics);
}

static void pipeline_free(Pipeline *p) {
    sol_mir_layout_free(&p->layout);
    sol_mir_representation_free(&p->representation);
    sol_mir_materialization_free(&p->materialization);
    sol_mir_plan_free(&p->plan); sol_mir_program_free(&p->program);
    sol_diagnostics_free(&p->diagnostics);
}

static bool build_pipeline(const SolIr *ir, const SolMirProgramRoot *roots,
    size_t root_count, const SolIrCallableId *imports, size_t import_count,
    Pipeline *p, const SolMirTargetDescriptor *target) {
    SolMirProgramBuildRequest program_request
        = {ir, roots, root_count, imports, import_count, NULL};
    SolMirPlanBuildRequest plan_request = {&p->program, NULL};
    SolMirMaterializeBuildRequest materialize_request = {&p->plan, NULL};
    SolMirRepresentationBuildRequest representation_request
        = {&p->materialization, NULL};
    SolMirLayoutBuildRequest layout_request
        = {&p->representation, target, NULL};
    return sol_mir_program_build(&program_request, &p->program, &p->diagnostics)
            == SOL_MIR_PROGRAM_BUILD_SUCCEEDED
        && sol_mir_plan_build(&plan_request, &p->plan, &p->diagnostics)
            == SOL_MIR_PLAN_BUILD_SUCCEEDED
        && sol_mir_materialize_build(&materialize_request,
            &p->materialization, &p->diagnostics)
            == SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED
        && sol_mir_representation_build(&representation_request,
            &p->representation, &p->diagnostics)
            == SOL_MIR_REPRESENTATION_BUILD_SUCCEEDED
        && sol_mir_layout_build(&layout_request, &p->layout, &p->diagnostics)
            == SOL_MIR_LAYOUT_BUILD_SUCCEEDED;
}

static const SolMirTypeLayout *named_layout(const SolMirLayout *layout,
    const char *name) {
    const SolIr *ir = layout->representation->materialization->plan->program->ir;
    for (size_t i = 0; i < layout->type_count; ++i) {
        SolIrDefinitionId definition
            = layout->representation->recipes[i].concrete_definition;
        if (definition != SOL_IR_NONE
            && strcmp(ir->definitions[definition].name, name) == 0)
            return &layout->types[i];
    }
    return NULL;
}

static char *render(const SolMirLayout *layout, size_t *length) {
    FILE *stream = tmpfile();
    if (stream == NULL || !sol_mir_layout_render(stream, layout)
        || fflush(stream) != 0 || fseek(stream, 0, SEEK_END) != 0) {
        if (stream != NULL) fclose(stream);
        return NULL;
    }
    long end = ftell(stream);
    if (end < 0 || fseek(stream, 0, SEEK_SET) != 0) {
        fclose(stream); return NULL;
    }
    char *text = malloc((size_t)end + 1);
    if (text == NULL) { fclose(stream); return NULL; }
    size_t got = fread(text, 1, (size_t)end, stream); fclose(stream);
    if (got != (size_t)end) { free(text); return NULL; }
    text[got] = '\0'; *length = got; return text;
}

static void test_descriptors(void) {
    SolMirTargetDescriptor wasm = sol_mir_target_wasm32();
    CHECK(wasm.pointer_size == 4 && wasm.pointer_alignment == 4
        && wasm.int64_alignment == 8
        && wasm.endianness == SOL_MIR_ENDIAN_LITTLE
        && wasm.max_object_bytes == UINT32_MAX);
    CHECK(sol_mir_target_descriptor_validate(&wasm));
    SolMirTargetDescriptor wasm_max = wasm;
    wasm_max.max_object_bytes = UINT32_MAX;
    CHECK(sol_mir_target_descriptor_validate(&wasm_max));
    wasm_max.max_object_bytes = (uint64_t)UINT32_MAX + UINT64_C(1);
    CHECK(!sol_mir_target_descriptor_validate(&wasm_max));
    SolMirTargetDescriptor big = {8, 8, 4, SOL_MIR_ENDIAN_BIG, UINT64_MAX};
    CHECK(sol_mir_target_descriptor_validate(&big));
    SolMirTargetDescriptor bad = wasm;
#define INVALID(member, value) do { \
    bad = wasm; bad.member = (value); \
    CHECK(!sol_mir_target_descriptor_validate(&bad)); \
} while (0)
    INVALID(pointer_size, 16); INVALID(pointer_alignment, 3);
    INVALID(pointer_alignment, 8); INVALID(int64_alignment, 16);
    INVALID(int64_alignment, 3); INVALID(endianness, (SolMirEndianness)2);
    INVALID(max_object_bytes, 0);
#undef INVALID
}

static void test_layout_contract(void) {
    static const char source[] =
        "module layout\n"
        "capability Token {}\n"
        "capability Base { function choose(value: Int64) -> Bool effects { pure } }\n"
        "record Empty {}\n"
        "record Mixed { flag: Bool, value: Int64, tail: Bool }\n"
        "record Inner { flag: Bool, value: Int64 }\n"
        "record Outer { pad: Bool, inner: Inner, pair: (Int64, Bool) }\n"
        "record Node { next: Option<Node> }\n"
        "record Loop { next: Loop }\n"
        "record DeadLast { live: Int64, dead: Loop }\n"
        "record DeadFirst { dead: Loop, live: Int64 }\n"
        "enum Chain<T> { end, next(value: Chain<T>), item(value: T) }\n"
        "enum DeadEnum { first(live: Int64, dead: Loop), "
            "second(dead: Loop, live: Int64) }\n"
        "enum Void {}\n"
        "enum Payloadless { one }\n"
        "enum Single { one(value: Bool) }\n"
        "enum Multi { first(flag: Bool, value: Int64), second(value: Bool), "
            "dead(value: Loop) }\n"
        "type Distinct = distinct Int64\n"
        "type Refined = refined Int64 where true\n"
        "function callback(value: Int64) -> Bool effects { pure } { return true }\n"
        "function apply(cb: function(Int64) -> Bool effects { pure }) -> Bool "
            "effects { pure } { return true }\n"
        "function root(empty: Empty, mixed: Mixed, outer: Outer, node: Node, looped: Loop, "
            "voided: Void, dead_last: DeadLast, dead_first: DeadFirst, "
            "dead_last_tuple: (Int64, Loop), dead_first_tuple: (Loop, Int64), "
            "dead_enum: DeadEnum, chain: Chain<Int64>, p: Payloadless, s: Single, m: Multi, text: Text, "
            "token: capability Token, base: capability Base, d: Distinct, r: Refined, "
            "cb: function(Int64) -> Bool effects { pure }, "
            "option: Option<Int64>, result: Result<Bool, Int64>, unit: ()) "
            "-> Int64 effects { pure } requires { apply(callback) && "
            "apply(base.choose) } { return outer.inner.value + outer.pair.0 }\n";
    Compilation c; bool compiled = compile_text(&c, source); CHECK(compiled);
    if (!compiled)
        sol_diagnostics_render_human(stderr, &c.source, &c.diagnostics);
    if (!compiled) { free_compilation(&c); return; }
    Pipeline p; pipeline_init(&p);
    SolMirProgramRoot root = {callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirTargetDescriptor wasm = sol_mir_target_wasm32();
    SolIrCallableId import = callable(&c.ir, "choose", SOL_IR_CALLABLE_CAPABILITY);
    bool built = build_pipeline(&c.ir, &root, 1, &import, 1, &p, &wasm);
    if (!built) sol_diagnostics_render_human(stderr, &c.source, &p.diagnostics);
    CHECK(built);
    if (!built) { pipeline_free(&p); free_compilation(&c); return; }
    CHECK(sol_mir_layout_validate(&p.layout, NULL));
    const SolMirTypeLayout *mixed = named_layout(&p.layout, "Mixed");
    const SolMirTypeLayout *empty = named_layout(&p.layout, "Empty");
    const SolMirTypeLayout *node = named_layout(&p.layout, "Node");
    const SolMirTypeLayout *loop = named_layout(&p.layout, "Loop");
    const SolMirTypeLayout *voided = named_layout(&p.layout, "Void");
    const SolMirTypeLayout *dead_last = named_layout(&p.layout, "DeadLast");
    const SolMirTypeLayout *dead_first = named_layout(&p.layout, "DeadFirst");
    const SolMirTypeLayout *dead_enum = named_layout(&p.layout, "DeadEnum");
    const SolMirTypeLayout *chain = named_layout(&p.layout, "Chain");
    const SolMirTypeLayout *payloadless = named_layout(&p.layout, "Payloadless");
    const SolMirTypeLayout *single = named_layout(&p.layout, "Single");
    const SolMirTypeLayout *multi = named_layout(&p.layout, "Multi");
    const SolMirTypeLayout *distinct = named_layout(&p.layout, "Distinct");
    const SolMirTypeLayout *refined = named_layout(&p.layout, "Refined");
    const SolMirTypeLayout *token = named_layout(&p.layout, "Token");
    CHECK(mixed != NULL && mixed->value_size == 4 && mixed->has_object
        && mixed->object_size == 24 && mixed->object_alignment == 8
        && mixed->tail_padding == 7);
    if (mixed != NULL) {
        SolMirPlanSlice fields
            = p.representation.recipes[mixed->recipe].fields;
        CHECK(fields.count == 3);
        CHECK(p.layout.fields[fields.offset].offset == 0
            && p.layout.fields[fields.offset + 1].offset == 8
            && p.layout.fields[fields.offset + 1].padding_before == 7
            && p.layout.fields[fields.offset + 2].offset == 16);
    }
    CHECK(empty != NULL && empty->value_size == 0 && !empty->has_object);
    CHECK(node != NULL && node->value_size == 4 && node->object_size == 4);
    CHECK(loop != NULL && loop->value_size == 0 && !loop->has_object);
    CHECK(voided != NULL && !voided->has_object);
    CHECK(dead_last != NULL && dead_first != NULL && dead_enum != NULL
        && !dead_last->has_object && !dead_first->has_object
        && !dead_enum->has_object);
    const SolMirTypeLayout *dead_products[] = {dead_last, dead_first};
    for (size_t i = 0; i < 2; ++i) {
        if (dead_products[i] == NULL) continue;
        SolMirPlanSlice fields
            = p.representation.recipes[dead_products[i]->recipe].fields;
        CHECK(fields.count == 2);
        for (size_t f = 0; f < fields.count; ++f) {
            const SolMirFieldLayout *field
                = &p.layout.fields[fields.offset + f];
            CHECK(!field->has_storage && field->offset == 0
                && field->size == 0 && field->alignment == 1
                && field->padding_before == 0);
        }
    }
    if (dead_enum != NULL) {
        SolMirPlanSlice variants
            = p.representation.recipes[dead_enum->recipe].variants;
        CHECK(variants.count == 2);
        for (size_t v = 0; v < variants.count; ++v) {
            const SolMirVariantLayout *variant
                = &p.layout.variants[variants.offset + v];
            CHECK(!variant->inhabited && !variant->has_payload_storage
                && variant->payload_size == 0
                && variant->payload_alignment == 1);
            SolMirPlanSlice fields
                = p.representation.variants[variants.offset + v].fields;
            for (size_t f = 0; f < fields.count; ++f)
                CHECK(!p.layout.fields[fields.offset + f].has_storage);
        }
    }
    CHECK(chain != NULL && chain->value_size == 4 && chain->has_object
        && chain->object_kind == SOL_MIR_LAYOUT_OBJECT_SUM);
    CHECK(payloadless != NULL && payloadless->object_size == 4
        && payloadless->tag_offset == 0 && payloadless->tag_size == 4);
    CHECK(single != NULL && single->payload_offset == 4
        && single->payload_size == 1 && single->object_size == 8);
    CHECK(multi != NULL && multi->payload_offset == 8
        && multi->payload_size == 16 && multi->object_size == 24);
    CHECK(distinct != NULL && refined != NULL
        && distinct->value_size == 8 && distinct->value_alignment == 8
        && !distinct->has_object && refined->value_size == 8);
    CHECK(token != NULL && token->object_kind
            == SOL_MIR_LAYOUT_OBJECT_CAPABILITY
        && token->object_size == 8 && token->root_token_offset == 0
        && token->private_source_handle_offset == 4);
    bool text = false, callable_plain = false, callable_environment = false;
    bool option = false, result = false, tuple_padding = false;
    size_t dead_tuples = 0;
    for (size_t i = 0; i < p.layout.type_count; ++i) {
        const SolMirRecipe *recipe = &p.representation.recipes[i];
        const SolMirTypeLayout *layout = &p.layout.types[i];
        if (recipe->kind == SOL_MIR_RECIPE_INT64)
            CHECK(layout->value_size == 8 && layout->value_alignment == 8);
        if (recipe->kind == SOL_MIR_RECIPE_BOOL)
            CHECK(layout->value_size == 1 && layout->value_alignment == 1);
        if (recipe->kind == SOL_MIR_RECIPE_UNIT
            || recipe->kind == SOL_MIR_RECIPE_NEVER)
            CHECK(layout->value_size == 0 && !layout->has_object);
        if (recipe->kind == SOL_MIR_RECIPE_TEXT) {
            text = layout->object_size == 8 && layout->data_handle_offset == 0
                && layout->length_offset == 4;
        } else if (recipe->kind == SOL_MIR_RECIPE_FUNCTION) {
            callable_plain |= layout->target_token_offset == 0
                && layout->object_size == 4;
            callable_environment |= layout->target_token_offset == 0
                && layout->environment_handle_offset == 4
                && layout->object_size == 8;
        } else if (recipe->kind == SOL_MIR_RECIPE_OPTION) {
            option |= layout->tag_size == 4 && layout->payload_offset >= 4;
        } else if (recipe->kind == SOL_MIR_RECIPE_RESULT) {
            result |= layout->tag_size == 4 && layout->payload_offset >= 4;
        } else if (recipe->kind == SOL_MIR_RECIPE_TUPLE
            && recipe->fields.count == 2) {
            const SolMirFieldLayout *fields
                = &p.layout.fields[recipe->fields.offset];
            if (!recipe->inhabited) {
                ++dead_tuples;
                CHECK(!layout->has_object && !fields[0].has_storage
                    && !fields[1].has_storage && fields[0].offset == 0
                    && fields[1].offset == 0 && fields[0].alignment == 1
                    && fields[1].alignment == 1);
            } else {
                tuple_padding |= layout->object_size == 16
                    && fields[0].offset == 0 && fields[1].offset == 8;
            }
        }
    }
    CHECK(text && callable_plain && callable_environment && option && result
        && tuple_padding && dead_tuples == 2);
    if (multi != NULL) {
        SolMirPlanSlice variants
            = p.representation.recipes[multi->recipe].variants;
        bool saw_dead = false;
        for (size_t v = 0; v < variants.count; ++v) {
            size_t variant_id = variants.offset + v;
            const SolMirVariantLayout *variant = &p.layout.variants[variant_id];
            if (variant->inhabited) continue;
            saw_dead = true;
            CHECK(!variant->has_payload_storage && variant->payload_size == 0);
            SolMirPlanSlice fields = p.representation.variants[variant_id].fields;
            for (size_t f = 0; f < fields.count; ++f)
                CHECK(!p.layout.fields[fields.offset + f].has_storage);
        }
        CHECK(saw_dead);
    }
    CHECK(p.layout.projection_count == 4);
    if (p.layout.projection_count == 4) {
        CHECK(p.layout.projections[0].object_offset == 4
            && p.layout.projections[1].object_offset == 8
            && p.layout.projections[2].object_offset == 8
            && p.layout.projections[3].object_offset == 0);
    }
    size_t first_length = 0, second_length = 0;
    char *first = render(&p.layout, &first_length);
    SolMirLayout repeated; sol_mir_layout_init(&repeated);
    SolMirLayoutBuildRequest repeat_request = {&p.representation, &wasm, NULL};
    CHECK(sol_mir_layout_build(&repeat_request, &repeated, &p.diagnostics)
        == SOL_MIR_LAYOUT_BUILD_SUCCEEDED);
    char *second = render(&repeated, &second_length);
    CHECK(first != NULL && second != NULL && first_length == second_length
        && memcmp(first, second, first_length) == 0);
    free(first); free(second); sol_mir_layout_free(&repeated);
    SolMirTargetDescriptor big = wasm;
    big.endianness = SOL_MIR_ENDIAN_BIG;
    SolMirLayoutBuildRequest big_request = {&p.representation, &big, NULL};
    CHECK(sol_mir_layout_build(&big_request, &repeated, &p.diagnostics)
        == SOL_MIR_LAYOUT_BUILD_SUCCEEDED);
    CHECK(repeated.target.endianness == SOL_MIR_ENDIAN_BIG
        && repeated.target.pointer_size == 4);
    for (size_t i = 0; i < p.layout.type_count; ++i)
        CHECK(repeated.types[i].value_size == p.layout.types[i].value_size
            && repeated.types[i].object_size == p.layout.types[i].object_size
            && repeated.types[i].payload_offset == p.layout.types[i].payload_offset);
    for (size_t i = 0; i < p.layout.field_count; ++i)
        CHECK(repeated.fields[i].offset == p.layout.fields[i].offset);
    size_t little_length = 0, big_length = 0;
    char *little_text = render(&p.layout, &little_length);
    char *big_text = render(&repeated, &big_length);
    CHECK(little_text != NULL && big_text != NULL
        && little_length == big_length
        && memcmp(little_text, big_text, little_length) != 0);
    free(little_text); free(big_text);
    sol_mir_layout_free(&repeated);
    SolMirTargetDescriptor pointer8 = {8, 8, 4, SOL_MIR_ENDIAN_LITTLE,
        UINT64_MAX};
    SolMirLayoutBuildRequest pointer8_request
        = {&p.representation, &pointer8, NULL};
    CHECK(sol_mir_layout_build(&pointer8_request, &repeated, &p.diagnostics)
        == SOL_MIR_LAYOUT_BUILD_SUCCEEDED);
    const SolMirTypeLayout *pointer8_mixed = named_layout(&repeated, "Mixed");
    const SolMirTypeLayout *pointer8_token = named_layout(&repeated, "Token");
    CHECK(pointer8_mixed != NULL && pointer8_mixed->value_size == 8);
    CHECK(pointer8_token != NULL && pointer8_token->value_size == 8
        && pointer8_token->object_size == 16
        && pointer8_token->private_source_handle_offset == 8);
    for (size_t i = 0; i < repeated.type_count; ++i)
        if (p.representation.recipes[i].kind == SOL_MIR_RECIPE_TEXT)
            CHECK(repeated.types[i].value_size == 8
                && repeated.types[i].object_size == 16
                && repeated.types[i].length_offset == 8);
    sol_mir_layout_free(&repeated);

    SolMirLayoutLimits exact = {
        p.layout.usage.type_layouts, p.layout.usage.field_layouts,
        p.layout.usage.variant_layouts, p.layout.usage.projection_maps,
        p.layout.usage.owned_bytes, p.layout.usage.build_scratch_bytes,
        p.layout.usage.build_work, p.layout.usage.validation_scratch_bytes,
        p.layout.usage.validation_work,
    };
    SolMirLayout limited; sol_mir_layout_init(&limited);
    SolMirLayoutBuildRequest limited_request
        = {&p.representation, &wasm, &exact};
    CHECK(sol_mir_layout_build(&limited_request, &limited, &p.diagnostics)
        == SOL_MIR_LAYOUT_BUILD_SUCCEEDED);
    sol_mir_layout_free(&limited);
#define LIMIT_FAIL(member) do { \
    SolMirLayoutLimits less = exact; --less.member; \
    limited_request.limits = &less; \
    CHECK(sol_mir_layout_build(&limited_request, &limited, &p.diagnostics) \
        == SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED); \
    CHECK(limited.representation == NULL); \
} while (0)
    LIMIT_FAIL(max_type_layouts); LIMIT_FAIL(max_field_layouts);
    LIMIT_FAIL(max_variant_layouts); LIMIT_FAIL(max_projection_maps);
    LIMIT_FAIL(max_owned_bytes); LIMIT_FAIL(max_build_scratch_bytes);
    LIMIT_FAIL(max_build_work); LIMIT_FAIL(max_validation_scratch_bytes);
    LIMIT_FAIL(max_validation_work);
#undef LIMIT_FAIL

    uint64_t saved_offset = p.layout.fields[0].offset;
    ++p.layout.fields[0].offset;
    CHECK(!sol_mir_layout_validate(&p.layout, NULL));
    FILE *stream = tmpfile(); CHECK(stream != NULL);
    if (stream != NULL) {
        CHECK(!sol_mir_layout_render(stream, &p.layout));
        CHECK(ftell(stream) == 0); fclose(stream);
    }
    p.layout.fields[0].offset = saved_offset;
    p.layout.fields[0].offset = UINT64_MAX;
    CHECK(!sol_mir_layout_validate(&p.layout, NULL));
    p.layout.fields[0].offset = saved_offset;
    SolMirFieldLayout *saved_fields = p.layout.fields;
    p.layout.fields = (SolMirFieldLayout *)(void *)p.layout.types;
    CHECK(!sol_mir_layout_validate(&p.layout, NULL));
    p.layout.fields = saved_fields;
    saved_fields = p.layout.fields;
    p.layout.fields = (SolMirFieldLayout *)(void *)p.representation.fields;
    CHECK(!sol_mir_layout_validate(&p.layout, NULL));
    p.layout.fields = saved_fields;
#define REJECT_CAPACITY(member) do { \
    size_t saved = p.layout.member##_capacity; \
    ++p.layout.member##_capacity; \
    CHECK(!sol_mir_layout_validate(&p.layout, NULL)); \
    p.layout.member##_capacity = saved; \
} while (0)
    REJECT_CAPACITY(type); REJECT_CAPACITY(field);
    REJECT_CAPACITY(variant); REJECT_CAPACITY(projection);
#undef REJECT_CAPACITY
#define REJECT_COUNT(member) do { \
    size_t saved = p.layout.member##_count; \
    --p.layout.member##_count; \
    CHECK(!sol_mir_layout_validate(&p.layout, NULL)); \
    p.layout.member##_count = saved; \
} while (0)
    REJECT_COUNT(type); REJECT_COUNT(field);
    REJECT_COUNT(variant); REJECT_COUNT(projection);
#undef REJECT_COUNT
    size_t saved_type_count = p.layout.type_count;
    size_t saved_type_capacity = p.layout.type_capacity;
    size_t saved_usage_types = p.layout.usage.type_layouts;
    size_t saved_type_limit = p.layout.limits.max_type_layouts;
    p.layout.type_count = p.layout.type_capacity = SIZE_MAX;
    p.layout.usage.type_layouts = SIZE_MAX;
    p.layout.limits.max_type_layouts = SIZE_MAX;
    CHECK(!sol_mir_layout_validate(&p.layout, NULL));
    p.layout.type_count = saved_type_count;
    p.layout.type_capacity = saved_type_capacity;
    p.layout.usage.type_layouts = saved_usage_types;
    p.layout.limits.max_type_layouts = saved_type_limit;
#define REJECT_USAGE(member) do { \
    size_t saved = p.layout.usage.member; \
    ++p.layout.usage.member; \
    CHECK(!sol_mir_layout_validate(&p.layout, NULL)); \
    p.layout.usage.member = saved; \
} while (0)
    REJECT_USAGE(type_layouts); REJECT_USAGE(field_layouts);
    REJECT_USAGE(variant_layouts); REJECT_USAGE(projection_maps);
    REJECT_USAGE(owned_bytes); REJECT_USAGE(build_scratch_bytes);
    REJECT_USAGE(build_work); REJECT_USAGE(validation_scratch_bytes);
    REJECT_USAGE(validation_work);
#undef REJECT_USAGE
    for (size_t i = 0; i < p.layout.type_count; ++i) {
#define MUTATE_TYPE(member, value) do { \
    SolMirTypeLayout saved = p.layout.types[i]; \
    p.layout.types[i].member = (value); \
    CHECK(!sol_mir_layout_validate(&p.layout, NULL)); \
    p.layout.types[i] = saved; \
} while (0)
        MUTATE_TYPE(recipe, p.layout.types[i].recipe ^ 1u);
        MUTATE_TYPE(value_size, p.layout.types[i].value_size ^ UINT64_C(1));
        MUTATE_TYPE(value_alignment,
            p.layout.types[i].value_alignment == 1 ? 2 : 1);
        MUTATE_TYPE(object_kind,
            (SolMirLayoutObjectKind)(p.layout.types[i].object_kind ^ 1));
        MUTATE_TYPE(has_object, !p.layout.types[i].has_object);
        MUTATE_TYPE(object_size, p.layout.types[i].object_size ^ UINT64_C(1));
        MUTATE_TYPE(object_alignment,
            p.layout.types[i].object_alignment == 1 ? 2 : 1);
        MUTATE_TYPE(tail_padding,
            p.layout.types[i].tail_padding ^ UINT64_C(1));
        MUTATE_TYPE(tag_offset, p.layout.types[i].tag_offset ^ UINT64_C(1));
        MUTATE_TYPE(tag_size, p.layout.types[i].tag_size ^ UINT64_C(1));
        MUTATE_TYPE(payload_offset,
            p.layout.types[i].payload_offset ^ UINT64_C(1));
        MUTATE_TYPE(payload_size, p.layout.types[i].payload_size ^ UINT64_C(1));
        MUTATE_TYPE(data_handle_offset,
            p.layout.types[i].data_handle_offset ^ UINT64_C(1));
        MUTATE_TYPE(length_offset,
            p.layout.types[i].length_offset ^ UINT64_C(1));
        MUTATE_TYPE(target_token_offset,
            p.layout.types[i].target_token_offset ^ UINT64_C(1));
        MUTATE_TYPE(environment_handle_offset,
            p.layout.types[i].environment_handle_offset ^ UINT64_C(1));
        MUTATE_TYPE(root_token_offset,
            p.layout.types[i].root_token_offset ^ UINT64_C(1));
        MUTATE_TYPE(private_source_handle_offset,
            p.layout.types[i].private_source_handle_offset ^ UINT64_C(1));
#undef MUTATE_TYPE
    }
    for (size_t i = 0; i < p.layout.field_count; ++i) {
#define MUTATE_FIELD(member, value) do { \
    SolMirFieldLayout saved = p.layout.fields[i]; \
    p.layout.fields[i].member = (value); \
    CHECK(!sol_mir_layout_validate(&p.layout, NULL)); \
    p.layout.fields[i] = saved; \
} while (0)
        MUTATE_FIELD(field, p.layout.fields[i].field ^ 1u);
        MUTATE_FIELD(owner_recipe, p.layout.fields[i].owner_recipe ^ 1u);
        MUTATE_FIELD(variant, p.layout.fields[i].variant ^ 1u);
        MUTATE_FIELD(has_storage, !p.layout.fields[i].has_storage);
        MUTATE_FIELD(offset, p.layout.fields[i].offset ^ UINT64_C(1));
        MUTATE_FIELD(size, p.layout.fields[i].size ^ UINT64_C(1));
        MUTATE_FIELD(alignment, p.layout.fields[i].alignment == 1 ? 2 : 1);
        MUTATE_FIELD(padding_before,
            p.layout.fields[i].padding_before ^ UINT64_C(1));
#undef MUTATE_FIELD
    }
    for (size_t i = 0; i < p.layout.variant_count; ++i) {
#define MUTATE_VARIANT(member, value) do { \
    SolMirVariantLayout saved = p.layout.variants[i]; \
    p.layout.variants[i].member = (value); \
    CHECK(!sol_mir_layout_validate(&p.layout, NULL)); \
    p.layout.variants[i] = saved; \
} while (0)
        MUTATE_VARIANT(variant, p.layout.variants[i].variant ^ 1u);
        MUTATE_VARIANT(owner_recipe, p.layout.variants[i].owner_recipe ^ 1u);
        MUTATE_VARIANT(tag, p.layout.variants[i].tag ^ UINT32_C(1));
        MUTATE_VARIANT(inhabited, !p.layout.variants[i].inhabited);
        MUTATE_VARIANT(has_payload_storage,
            !p.layout.variants[i].has_payload_storage);
        MUTATE_VARIANT(payload_size,
            p.layout.variants[i].payload_size ^ UINT64_C(1));
        MUTATE_VARIANT(payload_alignment,
            p.layout.variants[i].payload_alignment == 1 ? 2 : 1);
        MUTATE_VARIANT(tail_padding,
            p.layout.variants[i].tail_padding ^ UINT64_C(1));
#undef MUTATE_VARIANT
    }
    for (size_t i = 0; i < p.layout.projection_count; ++i) {
#define MUTATE_PROJECTION(member, value) do { \
    SolMirProjectionMap saved = p.layout.projections[i]; \
    p.layout.projections[i].member = (value); \
    CHECK(!sol_mir_layout_validate(&p.layout, NULL)); \
    p.layout.projections[i] = saved; \
} while (0)
        MUTATE_PROJECTION(projection, p.layout.projections[i].projection ^ 1u);
        MUTATE_PROJECTION(place, p.layout.projections[i].place ^ 1u);
        MUTATE_PROJECTION(base_recipe, p.layout.projections[i].base_recipe ^ 1u);
        MUTATE_PROJECTION(result_recipe,
            p.layout.projections[i].result_recipe ^ 1u);
        MUTATE_PROJECTION(field_layout,
            p.layout.projections[i].field_layout ^ 1u);
        MUTATE_PROJECTION(object_offset,
            p.layout.projections[i].object_offset ^ UINT64_C(1));
#undef MUTATE_PROJECTION
    }
    for (size_t i = 0; i < p.representation.field_count; ++i) {
#define MUTATE_RECIPE_FIELD(member, value) do { \
    SolMirRecipeField saved = p.representation.fields[i]; \
    p.representation.fields[i].member = (value); \
    CHECK(!sol_mir_layout_validate(&p.layout, NULL)); \
    p.representation.fields[i] = saved; \
} while (0)
        MUTATE_RECIPE_FIELD(source_field,
            p.representation.fields[i].source_field ^ 1u);
        MUTATE_RECIPE_FIELD(ordinal,
            p.representation.fields[i].ordinal ^ 1u);
        MUTATE_RECIPE_FIELD(type, p.representation.fields[i].type ^ 1u);
#undef MUTATE_RECIPE_FIELD
    }
    for (size_t i = 0; i < p.representation.variant_count; ++i) {
#define MUTATE_RECIPE_VARIANT(member, value) do { \
    SolMirRecipeVariant saved = p.representation.variants[i]; \
    p.representation.variants[i].member = (value); \
    CHECK(!sol_mir_layout_validate(&p.layout, NULL)); \
    p.representation.variants[i] = saved; \
} while (0)
        MUTATE_RECIPE_VARIANT(source_variant,
            p.representation.variants[i].source_variant ^ 1u);
        MUTATE_RECIPE_VARIANT(ordinal,
            p.representation.variants[i].ordinal ^ 1u);
        MUTATE_RECIPE_VARIANT(semantic_tag,
            p.representation.variants[i].semantic_tag ^ 1u);
        MUTATE_RECIPE_VARIANT(fields.offset,
            p.representation.variants[i].fields.offset ^ 1u);
        MUTATE_RECIPE_VARIANT(fields.count,
            p.representation.variants[i].fields.count ^ 1u);
#undef MUTATE_RECIPE_VARIANT
    }
    for (size_t i = 0; i < p.materialization.projection_count; ++i) {
#define MUTATE_SOURCE_PROJECTION(member, value) do { \
    SolMirMaterializedProjection saved = p.materialization.projections[i]; \
    p.materialization.projections[i].member = (value); \
    CHECK(!sol_mir_layout_validate(&p.layout, NULL)); \
    p.materialization.projections[i] = saved; \
} while (0)
        MUTATE_SOURCE_PROJECTION(kind, (SolIrProjectionKind)99);
        MUTATE_SOURCE_PROJECTION(type,
            p.materialization.projections[i].type ^ 1u);
        MUTATE_SOURCE_PROJECTION(source_field,
            p.materialization.projections[i].source_field ^ 1u);
        MUTATE_SOURCE_PROJECTION(tuple_ordinal,
            p.materialization.projections[i].tuple_ordinal ^ 1u);
        MUTATE_SOURCE_PROJECTION(source_projection,
            p.materialization.projections[i].source_projection ^ 1u);
#undef MUTATE_SOURCE_PROJECTION
    }
    for (size_t i = 0; i < p.representation.recipe_count; ++i) {
        SolMirRecipe saved = p.representation.recipes[i];
        if (saved.fields.count != 0) {
            p.representation.recipes[i].fields.offset ^= 1u;
            CHECK(!sol_mir_layout_validate(&p.layout, NULL));
            p.representation.recipes[i] = saved;
        }
        if (saved.variants.count != 0) {
            p.representation.recipes[i].variants.offset ^= 1u;
            CHECK(!sol_mir_layout_validate(&p.layout, NULL));
            p.representation.recipes[i] = saved;
        }
    }
    SolMirTargetDescriptor saved_target = p.layout.target;
#define REJECT_TARGET(member, value) do { \
    p.layout.target = saved_target; p.layout.target.member = (value); \
    CHECK(!sol_mir_layout_validate(&p.layout, NULL)); \
} while (0)
    REJECT_TARGET(pointer_size, 16); REJECT_TARGET(pointer_alignment, 3);
    REJECT_TARGET(int64_alignment, 3);
    REJECT_TARGET(endianness, (SolMirEndianness)2);
    REJECT_TARGET(max_object_bytes, 0);
#undef REJECT_TARGET
    p.layout.target = saved_target;
#define REJECT_NULL(member) do { \
    void *saved = p.layout.member; p.layout.member = NULL; \
    CHECK(!sol_mir_layout_validate(&p.layout, NULL)); \
    p.layout.member = saved; \
} while (0)
    REJECT_NULL(types); REJECT_NULL(fields); REJECT_NULL(variants);
    REJECT_NULL(projections);
#undef REJECT_NULL
#define REJECT_TYPE_ALIAS(pointer) do { \
    SolMirTypeLayout *saved = p.layout.types; \
    p.layout.types = (SolMirTypeLayout *)(void *)(pointer); \
    CHECK(!sol_mir_layout_validate(&p.layout, NULL)); \
    p.layout.types = saved; \
} while (0)
    REJECT_TYPE_ALIAS(p.representation.recipes);
    REJECT_TYPE_ALIAS(p.layout.fields);
    REJECT_TYPE_ALIAS(p.layout.fields + 1);
    REJECT_TYPE_ALIAS(p.layout.variants);
    REJECT_TYPE_ALIAS(p.layout.projections);
    REJECT_TYPE_ALIAS(&p.representation);
    REJECT_TYPE_ALIAS(&p.materialization);
    REJECT_TYPE_ALIAS(&p.plan);
    REJECT_TYPE_ALIAS(&p.program);
    REJECT_TYPE_ALIAS(&c.ir);
    REJECT_TYPE_ALIAS(&p.program.templates[0].mir);
    REJECT_TYPE_ALIAS(&p.materialization.images[0].topology);
    REJECT_TYPE_ALIAS(p.materialization.types);
    REJECT_TYPE_ALIAS(p.plan.types);
    REJECT_TYPE_ALIAS(p.program.templates);
    REJECT_TYPE_ALIAS(p.program.templates[0].mir.blocks);
    REJECT_TYPE_ALIAS(c.ir.types);
    REJECT_TYPE_ALIAS(c.ir.source_path);
#undef REJECT_TYPE_ALIAS
    CHECK(sol_mir_layout_validate(&p.layout, NULL));

    SolMirLayout occupied; sol_mir_layout_init(&occupied);
    occupied.representation = &p.representation;
    CHECK(sol_mir_layout_build(&repeat_request, &occupied, &p.diagnostics)
        == SOL_MIR_LAYOUT_BUILD_INVALID_ARGUMENT);
    occupied.representation = NULL;
    SolMirTargetDescriptor invalid_target = wasm;
    invalid_target.pointer_size = 16;
    SolMirLayoutBuildRequest invalid_request
        = {&p.representation, &invalid_target, NULL};
    CHECK(sol_mir_layout_build(&invalid_request, &occupied, &p.diagnostics)
        == SOL_MIR_LAYOUT_BUILD_INVALID_TARGET);
    CHECK(occupied.representation == NULL);
    size_t saved_tag = p.representation.variants[0].semantic_tag;
    p.representation.variants[0].semantic_tag = UINT64_MAX;
    CHECK(sol_mir_layout_build(&repeat_request, &occupied, &p.diagnostics)
        == SOL_MIR_LAYOUT_BUILD_INVALID_REPRESENTATION);
    CHECK(occupied.representation == NULL);
    p.representation.variants[0].semantic_tag = saved_tag;

    SolMirTargetDescriptor tiny = wasm; tiny.max_object_bytes = 24;
    SolMirLayoutBuildRequest tiny_request = {&p.representation, &tiny, NULL};
    CHECK(sol_mir_layout_build(&tiny_request, &occupied, &p.diagnostics)
        == SOL_MIR_LAYOUT_BUILD_SUCCEEDED);
    sol_mir_layout_free(&occupied);
    tiny.max_object_bytes = 23;
    CHECK(sol_mir_layout_build(&tiny_request, &occupied, &p.diagnostics)
        == SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED);
    tiny.pointer_size = 8; tiny.pointer_alignment = 8;
    tiny.max_object_bytes = UINT64_MAX;
    CHECK(sol_mir_layout_build(&tiny_request, &occupied, &p.diagnostics)
        == SOL_MIR_LAYOUT_BUILD_SUCCEEDED);
    CHECK(occupied.target.endianness == SOL_MIR_ENDIAN_LITTLE);
    sol_mir_layout_free(&occupied);
    pipeline_free(&p); free_compilation(&c);
}

static void test_dead_shapes_do_not_consume_object_bytes(void) {
    static const char source[] =
        "module dead_shapes\n"
        "record Loop { next: Loop }\n"
        "record DeadLast { live: Int64, dead: Loop }\n"
        "record DeadFirst { dead: Loop, live: Int64 }\n"
        "enum MixedReach { live, dead(value: Int64, stop: Loop) }\n"
        "enum AllDead { first(value: Int64, stop: Loop), "
            "second(stop: Loop, value: Int64) }\n"
        "function root(last: DeadLast, first: DeadFirst, "
            "tuple_last: (Int64, Loop), tuple_first: (Loop, Int64), "
            "mixed: MixedReach, dead: AllDead) -> Int64 { return 0 }\n";
    Compilation c; bool compiled = compile_text(&c, source); CHECK(compiled);
    if (!compiled) {
        sol_diagnostics_render_human(stderr, &c.source, &c.diagnostics);
        free_compilation(&c); return;
    }
    Pipeline p; pipeline_init(&p);
    SolMirProgramRoot root = {callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    target.max_object_bytes = 4;
    bool built = build_pipeline(&c.ir, &root, 1, NULL, 0, &p, &target);
    if (!built) sol_diagnostics_render_human(stderr, &c.source, &p.diagnostics);
    CHECK(built);
    if (built) {
        const SolMirTypeLayout *mixed = named_layout(&p.layout, "MixedReach");
        const SolMirTypeLayout *dead = named_layout(&p.layout, "AllDead");
        CHECK(mixed != NULL && mixed->has_object && mixed->object_size == 4);
        CHECK(dead != NULL && !dead->has_object);
        for (size_t i = 0; i < p.layout.type_count; ++i)
            CHECK(!p.layout.types[i].has_object
                || p.layout.types[i].object_size <= 4);
        CHECK(sol_mir_layout_validate(&p.layout, NULL));
    }
    pipeline_free(&p); free_compilation(&c);
}

static bool compile_e6(Compilation *c, SolPackage *package) {
    memset(c, 0, sizeof(*c));
    sol_package_init(package); sol_diagnostics_init(&c->diagnostics);
    sol_hir_module_init(&c->hir); sol_type_table_init(&c->types);
    sol_effect_table_init(&c->effects); sol_contract_table_init(&c->contracts);
    sol_ir_init(&c->ir);
    char error[256];
    if (!sol_package_load_directory(package,
            SOL_TEST_SOURCE_DIR "/tests/conformance/e6", &c->diagnostics,
            error, sizeof(error))) return false;
    SolHirFileScope *scopes = package->file_count == 0 ? NULL
        : malloc(package->file_count * sizeof(*scopes));
    if (package->file_count != 0 && scopes == NULL) return false;
    for (size_t i = 0; i < package->file_count; ++i)
        scopes[i] = (SolHirFileScope){package->files[i].module_name,
            package->files[i].import_start, package->files[i].import_count,
            package->files[i].item_start, package->files[i].item_count};
    bool valid = sol_hir_lower_scoped(&package->source, &package->syntax,
            scopes, package->file_count, &c->hir, &c->diagnostics)
        && sol_type_check(&package->source, &package->syntax, &c->hir,
            &c->types, &c->diagnostics)
        && sol_effect_check(&package->source, &package->syntax, &c->hir,
            &c->types, &c->effects, &c->diagnostics)
        && sol_contract_lower(&package->source, &package->syntax, &c->hir,
            &c->types, &c->effects, &c->contracts, &c->diagnostics)
        && sol_ir_lower_scoped(&package->source, &package->syntax, &c->hir,
            &c->types, &c->effects, &c->contracts, package->files,
            package->file_count, &c->ir, &c->diagnostics);
    free(scopes); return valid;
}

static void free_e6(Compilation *c, SolPackage *package) {
    sol_ir_free(&c->ir); sol_contract_table_free(&c->contracts);
    sol_effect_table_free(&c->effects); sol_type_table_free(&c->types);
    sol_hir_module_free(&c->hir); sol_diagnostics_free(&c->diagnostics);
    sol_package_free(package);
}

static void test_e6_census(void) {
    Compilation c; SolPackage package;
    bool compiled = compile_e6(&c, &package); CHECK(compiled);
    if (!compiled) { free_e6(&c, &package); return; }
    const char *import_names[] = {"write", "get", "count", "read"};
    SolIrCallableId imports[4];
    for (size_t i = 0; i < 4; ++i)
        imports[i] = callable(&c.ir, import_names[i], SOL_IR_CALLABLE_CAPABILITY);
    SolMirProgramRoot roots[5];
    roots[0] = (SolMirProgramRoot){callable(&c.ir, "launch",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_ENTRY};
    size_t root_count = 1;
    for (size_t i = 0; i < c.ir.callable_count; ++i)
        if (c.ir.callables[i].kind == SOL_IR_CALLABLE_TEST)
            roots[root_count++] = (SolMirProgramRoot){i, SOL_MIR_PROGRAM_ROOT_TEST};
    Pipeline p; pipeline_init(&p); SolMirTargetDescriptor wasm = sol_mir_target_wasm32();
    bool built = root_count == 5 && build_pipeline(&c.ir, roots, root_count,
        imports, 4, &p, &wasm);
    CHECK(built);
    if (!built) {
        sol_diagnostics_render_human(stderr, &package.source, &p.diagnostics);
        pipeline_free(&p); free_e6(&c, &package); return;
    }
    CHECK(p.layout.type_count == 21 && p.layout.field_count == 11
        && p.layout.variant_count == 9 && p.layout.projection_count == 5);
    size_t no_storage = 0, bools = 0, wide = 0, handles = 0;
    size_t object4 = 0, object8 = 0, object16 = 0, aggregate_count = 0;
    uint64_t object_total = 0, object_maximum = 0;
    for (size_t i = 0; i < p.layout.type_count; ++i) {
        const SolMirTypeLayout *type = &p.layout.types[i];
        no_storage += type->value_size == 0;
        bools += type->value_size == 1 && type->value_alignment == 1;
        wide += type->value_size == 8 && type->value_alignment == 8;
        handles += type->value_size == 4 && type->value_alignment == 4;
        if (type->object_kind == SOL_MIR_LAYOUT_OBJECT_PRODUCT
            || type->object_kind == SOL_MIR_LAYOUT_OBJECT_SUM) {
            ++aggregate_count; object_total += type->object_size;
            if (type->object_size > object_maximum) object_maximum = type->object_size;
            object4 += type->object_size == 4;
            object8 += type->object_size == 8;
            object16 += type->object_size == 16;
        }
    }
    CHECK(no_storage == 2 && bools == 1 && wide == 2 && handles == 16);
    CHECK(aggregate_count == 8 && object4 == 1 && object8 == 3
        && object16 == 4 && object_total == 92 && object_maximum == 16);
    for (size_t i = 0; i < p.layout.variant_count; ++i)
        CHECK(p.layout.variants[i].tag == p.representation.variants[i].semantic_tag);
    size_t offset0 = 0, offset8 = 0;
    for (size_t i = 0; i < p.layout.projection_count; ++i) {
        offset0 += p.layout.projections[i].object_offset == 0;
        offset8 += p.layout.projections[i].object_offset == 8;
    }
    CHECK(offset0 == 3 && offset8 == 2);
    static const SolMirRecipeKind kinds[21] = {
        SOL_MIR_RECIPE_INT64, SOL_MIR_RECIPE_BOOL, SOL_MIR_RECIPE_TEXT,
        SOL_MIR_RECIPE_UNIT, SOL_MIR_RECIPE_NEVER, SOL_MIR_RECIPE_RECORD,
        SOL_MIR_RECIPE_RECORD, SOL_MIR_RECIPE_ENUM, SOL_MIR_RECIPE_ENUM,
        SOL_MIR_RECIPE_REFINED, SOL_MIR_RECIPE_CAPABILITY,
        SOL_MIR_RECIPE_CAPABILITY, SOL_MIR_RECIPE_CAPABILITY,
        SOL_MIR_RECIPE_OPTION, SOL_MIR_RECIPE_OPTION, SOL_MIR_RECIPE_RESULT,
        SOL_MIR_RECIPE_TUPLE, SOL_MIR_RECIPE_FUNCTION,
        SOL_MIR_RECIPE_FUNCTION, SOL_MIR_RECIPE_FUNCTION,
        SOL_MIR_RECIPE_FUNCTION,
    };
    static const uint64_t value_sizes[21] = {
        8, 1, 4, 0, 0, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    };
    static const uint64_t value_alignments[21] = {
        8, 1, 4, 1, 1, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    };
    static const SolMirLayoutObjectKind object_kinds[21] = {
        0, 0, 3, 0, 0, 1, 1, 2, 2, 0, 5, 5, 5, 2, 2, 2, 1, 4, 4, 4, 4,
    };
    static const uint64_t object_sizes[21] = {
        0, 0, 8, 0, 0, 16, 8, 8, 4, 0, 8, 8, 8, 16, 8, 16, 16, 4, 4, 4, 4,
    };
    static const uint64_t object_alignments[21] = {
        1, 1, 4, 1, 1, 8, 4, 4, 4, 1, 4, 4, 4, 8, 4, 8, 8, 4, 4, 4, 4,
    };
    static const uint64_t tails[21] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0, 0,
    };
    size_t object_none = 0, object_text = 0, object_product = 0;
    size_t object_sum = 0, object_callable = 0, object_capability = 0;
    for (size_t i = 0; i < 21; ++i) {
        const SolMirTypeLayout *type = &p.layout.types[i];
        CHECK(p.representation.recipes[i].kind == kinds[i]);
        CHECK(type->recipe == i && type->value_size == value_sizes[i]
            && type->value_alignment == value_alignments[i]
            && type->object_kind == object_kinds[i]
            && type->has_object == (object_sizes[i] != 0)
            && type->object_size == object_sizes[i]
            && type->object_alignment == object_alignments[i]
            && type->tail_padding == tails[i]);
        object_none += type->object_kind == SOL_MIR_LAYOUT_OBJECT_NONE;
        object_text += type->object_kind == SOL_MIR_LAYOUT_OBJECT_TEXT;
        object_product += type->object_kind == SOL_MIR_LAYOUT_OBJECT_PRODUCT;
        object_sum += type->object_kind == SOL_MIR_LAYOUT_OBJECT_SUM;
        object_callable += type->object_kind == SOL_MIR_LAYOUT_OBJECT_CALLABLE;
        object_capability += type->object_kind == SOL_MIR_LAYOUT_OBJECT_CAPABILITY;
        uint64_t tag_offset = SOL_MIR_LAYOUT_OFFSET_NONE, tag_size = 0;
        uint64_t payload_offset = SOL_MIR_LAYOUT_OFFSET_NONE, payload_size = 0;
        uint64_t data = SOL_MIR_LAYOUT_OFFSET_NONE, length = SOL_MIR_LAYOUT_OFFSET_NONE;
        uint64_t target = SOL_MIR_LAYOUT_OFFSET_NONE, environment = SOL_MIR_LAYOUT_OFFSET_NONE;
        uint64_t root = SOL_MIR_LAYOUT_OFFSET_NONE, private_source = SOL_MIR_LAYOUT_OFFSET_NONE;
        if (i == 2) { data = 0; length = 4; }
        if (i == 7) { tag_offset = 0; tag_size = 4; payload_offset = 4; payload_size = 4; }
        if (i == 8) { tag_offset = 0; tag_size = 4; payload_offset = 4; }
        if (i == 13) { tag_offset = 0; tag_size = 4; payload_offset = 8; payload_size = 8; }
        if (i == 14) { tag_offset = 0; tag_size = 4; payload_offset = 4; payload_size = 4; }
        if (i == 15) { tag_offset = 0; tag_size = 4; payload_offset = 8; payload_size = 8; }
        if (i >= 17) target = 0;
        if (i >= 10 && i <= 12) { root = 0; private_source = 4; }
        CHECK(type->tag_offset == tag_offset && type->tag_size == tag_size
            && type->payload_offset == payload_offset
            && type->payload_size == payload_size
            && type->data_handle_offset == data && type->length_offset == length
            && type->target_token_offset == target
            && type->environment_handle_offset == environment
            && type->root_token_offset == root
            && type->private_source_handle_offset == private_source);
    }
    CHECK(object_none == 5 && object_text == 1 && object_product == 3
        && object_sum == 5 && object_callable == 4 && object_capability == 3);
    CHECK(strcmp(c.ir.definitions[p.representation.recipes[5].concrete_definition].name,
            "Pair") == 0);
    CHECK(strcmp(c.ir.definitions[p.representation.recipes[6].concrete_definition].name,
            "Packet") == 0);
    CHECK(strcmp(c.ir.definitions[p.representation.recipes[7].concrete_definition].name,
            "Envelope") == 0);
    CHECK(strcmp(c.ir.definitions[p.representation.recipes[8].concrete_definition].name,
            "Failure") == 0);
    CHECK(strcmp(c.ir.definitions[p.representation.recipes[9].concrete_definition].name,
            "Positive") == 0);
    CHECK(strcmp(c.ir.definitions[p.representation.recipes[10].concrete_definition].name,
            "Console") == 0);
    CHECK(strcmp(c.ir.definitions[p.representation.recipes[11].concrete_definition].name,
            "Arguments") == 0);
    CHECK(strcmp(c.ir.definitions[p.representation.recipes[12].concrete_definition].name,
            "Configuration") == 0);
    static const size_t field_types[11] = {0, 0, 1, 16, 6, 0, 2, 0, 8, 0, 1};
    static const size_t field_ordinals[11] = {0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1};
    static const size_t field_owners[11] = {5, 5, 6, 6, 7, 13, 14, 15, 15, 16, 16};
    static const size_t field_variants[11] = {
        SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, 0, 4, 6, 7, 8, SIZE_MAX, SIZE_MAX,
    };
    static const uint64_t field_offsets[11] = {0, 8, 0, 4, 4, 8, 4, 8, 8, 0, 8};
    static const uint64_t field_sizes[11] = {8, 8, 1, 4, 4, 8, 4, 8, 4, 8, 1};
    static const uint64_t field_alignments[11] = {8, 8, 1, 4, 4, 8, 4, 8, 4, 8, 1};
    static const uint64_t field_padding[11] = {0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0};
    for (size_t i = 0; i < 11; ++i) {
        const SolMirFieldLayout *field = &p.layout.fields[i];
        CHECK(p.representation.fields[i].type == field_types[i]
            && p.representation.fields[i].ordinal == field_ordinals[i]
            && ((i < 5) == (p.representation.fields[i].source_field
                != SOL_IR_NONE)));
        CHECK(field->field == i && field->owner_recipe == field_owners[i]
            && field->variant == field_variants[i] && field->has_storage
            && field->offset == field_offsets[i] && field->size == field_sizes[i]
            && field->alignment == field_alignments[i]
            && field->padding_before == field_padding[i]);
    }
    CHECK(strcmp(c.ir.fields[p.representation.fields[0].source_field].name,
            "left") == 0);
    CHECK(strcmp(c.ir.fields[p.representation.fields[1].source_field].name,
            "right") == 0);
    CHECK(strcmp(c.ir.fields[p.representation.fields[2].source_field].name,
            "ignored") == 0);
    CHECK(strcmp(c.ir.fields[p.representation.fields[3].source_field].name,
            "data") == 0);
    CHECK(strcmp(c.ir.fields[p.representation.fields[4].source_field].name,
            "packet") == 0);
    static const size_t variant_owners[9] = {7, 7, 8, 13, 13, 14, 14, 15, 15};
    static const uint32_t variant_tags[9] = {0, 1, 0, 0, 1, 0, 1, 0, 1};
    static const uint64_t variant_sizes[9] = {4, 0, 0, 0, 8, 0, 4, 8, 4};
    static const uint64_t variant_alignments[9] = {4, 1, 1, 1, 8, 1, 4, 8, 4};
    static const size_t variant_field_offsets[9] = {4, 5, 5, 5, 5, 6, 6, 7, 8};
    static const size_t variant_field_counts[9] = {1, 0, 0, 0, 1, 0, 1, 1, 1};
    for (size_t i = 0; i < 9; ++i) {
        const SolMirVariantLayout *variant = &p.layout.variants[i];
        CHECK(variant->variant == i && variant->owner_recipe == variant_owners[i]
            && variant->tag == variant_tags[i]
            && p.representation.variants[i].ordinal == variant_tags[i]
            && p.representation.variants[i].semantic_tag == variant_tags[i]
            && ((i < 3) == (p.representation.variants[i].source_variant
                != SOL_IR_NONE))
            && variant->inhabited && variant->has_payload_storage
            && variant->payload_size == variant_sizes[i]
            && variant->payload_alignment == variant_alignments[i]
            && variant->tail_padding == 0);
        CHECK(p.representation.variants[i].fields.offset
                == variant_field_offsets[i]
            && p.representation.variants[i].fields.count
                == variant_field_counts[i]);
    }
    CHECK(strcmp(c.ir.variants[p.representation.variants[0].source_variant].name,
            "wrapped") == 0);
    CHECK(strcmp(c.ir.variants[p.representation.variants[1].source_variant].name,
            "empty") == 0);
    CHECK(strcmp(c.ir.variants[p.representation.variants[2].source_variant].name,
            "invalid") == 0);
    static const size_t projection_fields[5] = {0, 0, 1, 0, 1};
    static const size_t projection_places[5] = {17, 18, 19, 21, 24};
    static const uint64_t projection_offsets[5] = {0, 0, 8, 0, 8};
    for (size_t i = 0; i < 5; ++i) {
        const SolMirProjectionMap *map = &p.layout.projections[i];
        CHECK(map->projection == i
            && map->place == projection_places[i]
            && map->base_recipe == 5 && map->result_recipe == 0
            && map->field_layout == projection_fields[i]
            && map->object_offset == projection_offsets[i]);
    }
    pipeline_free(&p); free_e6(&c, &package);
}

int main(void) {
    test_descriptors();
    test_layout_contract();
    test_dead_shapes_do_not_consume_object_bytes();
    test_e6_census();
    if (failures != 0) {
        fprintf(stderr, "%d MIR layout test(s) failed\n", failures);
        return 1;
    }
    printf("MIR layout tests passed\n");
    return 0;
}
