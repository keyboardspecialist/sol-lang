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
    SolMirTargetDescriptor big = {8, 8, 4, SOL_MIR_ENDIAN_BIG, UINT64_MAX};
    SolMirLayoutBuildRequest big_request = {&p.representation, &big, NULL};
    CHECK(sol_mir_layout_build(&big_request, &repeated, &p.diagnostics)
        == SOL_MIR_LAYOUT_BUILD_SUCCEEDED);
    CHECK(repeated.target.endianness == SOL_MIR_ENDIAN_BIG
        && repeated.target.pointer_size == 8);
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

    SolMirTargetDescriptor tiny = wasm; tiny.max_object_bytes = 23;
    SolMirLayoutBuildRequest tiny_request = {&p.representation, &tiny, NULL};
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
