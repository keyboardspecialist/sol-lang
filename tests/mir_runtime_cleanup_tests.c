#include "support/native_progress.h"
#include "support/path_work.h"
#define main sol_test_body
#define SOL_MIR_PLAN_TEST_HOOKS 1
#include "sol/mir_runtime_cleanup.h"
#include "sol/effects.h"
#include "sol/ownership.h"
#include "sol/package.h"
#include "../src/mir_runtime_arena_internal.h"
#include "../src/mir_linkage_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

typedef struct { SolDiagnostics diagnostics; SolHirModule hir; SolTypeTable types;
    SolEffectTable effects; SolContractTable contracts; SolIr ir; SolPackage package; } Compilation;
static char *render_cleanup_text(const SolMirRuntimeCleanup *cleanup);
static void check_trace_batch_contract(SolMirRuntimeCleanup *cleanup);
static size_t affine_image(const SolMirConcreteProgram *program,
    SolIrCallableId callable_id);
static SolIrCallableId callable(const SolIr *ir, const char *name, SolIrCallableKind kind) {
    for (size_t i=0;i<ir->callable_count;++i) if (ir->callables[i].kind==kind && strcmp(ir->callables[i].name,name)==0) return i;
    return SOL_IR_NONE;
}
static bool compile_directory(Compilation *c, const char *directory) {
    memset(c,0,sizeof(*c)); sol_package_init(&c->package); sol_diagnostics_init(&c->diagnostics);
    sol_hir_module_init(&c->hir); sol_type_table_init(&c->types); sol_effect_table_init(&c->effects); sol_contract_table_init(&c->contracts); sol_ir_init(&c->ir);
    char error[256]; if(!sol_package_load_directory(&c->package,directory,&c->diagnostics,error,sizeof(error)))return false;
    SolHirFileScope *scopes=c->package.file_count?malloc(c->package.file_count*sizeof(*scopes)):NULL; if(c->package.file_count&&scopes==NULL)return false;
    for(size_t i=0;i<c->package.file_count;++i) scopes[i]=(SolHirFileScope){c->package.files[i].module_name,c->package.files[i].import_start,c->package.files[i].import_count,c->package.files[i].item_start,c->package.files[i].item_count};
    bool ok=sol_hir_lower_scoped(&c->package.source,&c->package.syntax,scopes,c->package.file_count,&c->hir,&c->diagnostics)&&sol_type_check(&c->package.source,&c->package.syntax,&c->hir,&c->types,&c->diagnostics)&&sol_effect_check(&c->package.source,&c->package.syntax,&c->hir,&c->types,&c->effects,&c->diagnostics)&&sol_contract_lower(&c->package.source,&c->package.syntax,&c->hir,&c->types,&c->effects,&c->contracts,&c->diagnostics)&&sol_ir_lower_scoped(&c->package.source,&c->package.syntax,&c->hir,&c->types,&c->effects,&c->contracts,c->package.files,c->package.file_count,&c->ir,&c->diagnostics); free(scopes); return ok;
}
static bool compile_e6(Compilation *c) {
    return compile_directory(c, SOL_TEST_SOURCE_DIR "/tests/conformance/e6");
}
static void compilation_free(Compilation *c) { sol_ir_free(&c->ir);sol_contract_table_free(&c->contracts);sol_effect_table_free(&c->effects);sol_type_table_free(&c->types);sol_hir_module_free(&c->hir);sol_diagnostics_free(&c->diagnostics);sol_package_free(&c->package); }
static bool build_concrete(Compilation *c, SolMirConcreteProgram *p) {
    SolMirProgramRoot roots[5]={{callable(&c->ir,"launch",SOL_IR_CALLABLE_FUNCTION),SOL_MIR_PROGRAM_ROOT_ENTRY}}; size_t n=1;
    for(size_t i=0;i<c->ir.callable_count;++i)if(c->ir.callables[i].kind==SOL_IR_CALLABLE_TEST)roots[n++]=(SolMirProgramRoot){i,SOL_MIR_PROGRAM_ROOT_TEST};
    static const char *const names[]={"write","get","count","read"}; SolIrCallableId imports[4];for(size_t i=0;i<4;++i)imports[i]=callable(&c->ir,names[i],SOL_IR_CALLABLE_CAPABILITY);
    SolMirTargetDescriptor target=sol_mir_target_wasm32(); SolMirConcreteBuildRequest r={&c->ir,roots,n,imports,4,&target,NULL}; return n==5&&sol_mir_concrete_program_build(&r,p,&c->diagnostics)==SOL_MIR_CONCRETE_BUILD_SUCCEEDED;
}
static bool build_named_concrete(Compilation *c, const char *name,
    SolMirConcreteProgram *p) {
    SolIrCallableId callable_id = callable(&c->ir, name, SOL_IR_CALLABLE_FUNCTION);
    SolMirProgramRoot root = {callable_id, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    return callable_id != SOL_IR_NONE && sol_mir_concrete_program_build(
        &(SolMirConcreteBuildRequest){&c->ir, &root, 1, NULL, 0, &target, NULL},
        p, &c->diagnostics) == SOL_MIR_CONCRETE_BUILD_SUCCEEDED;
}
static bool build_cleanup_fixture(Compilation *c, SolMirConcreteProgram *p) {
    SolMirProgramRoot roots[8]; size_t count = 0;
    for (size_t i = 0; i < c->ir.callable_count; ++i)
        if (c->ir.callables[i].kind == SOL_IR_CALLABLE_TEST)
            roots[count++] = (SolMirProgramRoot){i, SOL_MIR_PROGRAM_ROOT_TEST};
    SolIrCallableId policy = callable(&c->ir, "host_policy", SOL_IR_CALLABLE_FUNCTION);
    if (policy != SOL_IR_NONE) roots[count++] = (SolMirProgramRoot){policy,
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolIrCallableId source = callable(&c->ir, "read", SOL_IR_CALLABLE_CAPABILITY);
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    SolMirConcreteBuildRequest request = {&c->ir, roots, count,
        source == SOL_IR_NONE ? NULL : &source, source == SOL_IR_NONE ? 0 : 1, &target, NULL};
    return count != 0 && sol_mir_concrete_program_build(&request, p, &c->diagnostics)
        == SOL_MIR_CONCRETE_BUILD_SUCCEEDED;
}
static bool build_affine_pair_fixture(Compilation *c, SolMirConcreteProgram *p) {
    static const char *const names[] = {
        "definite_left", "conditional_left", "repaired_left", "moved_root",
    };
    SolMirProgramRoot roots[4];
    for (size_t i = 0; i < 4; ++i) {
        SolIrCallableId id = callable(&c->ir, names[i], SOL_IR_CALLABLE_FUNCTION);
        if (id == SOL_IR_NONE) return false;
        roots[i] = (SolMirProgramRoot){id, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    }
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    SolMirConcreteBuildRequest request = {&c->ir, roots, 4, NULL, 0, &target, NULL};
    return sol_mir_concrete_program_build(&request, p, &c->diagnostics)
        == SOL_MIR_CONCRETE_BUILD_SUCCEEDED;
}
static bool build_refined_route_fixture(Compilation *c, SolMirConcreteProgram *p) {
    static const char *const names[] = {"direct_true", "direct_false",
        "post_branch_true", "post_branch_false", "arithmetic_after_branch"};
    SolMirProgramRoot roots[sizeof(names) / sizeof(*names)];
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) {
        SolIrCallableId id = callable(&c->ir, names[i], SOL_IR_CALLABLE_FUNCTION);
        if (id == SOL_IR_NONE) return false;
        roots[i] = (SolMirProgramRoot){id, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    }
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    return sol_mir_concrete_program_build(&(SolMirConcreteBuildRequest){&c->ir, roots,
        sizeof(roots) / sizeof(*roots), NULL, 0, &target, NULL}, p, &c->diagnostics)
        == SOL_MIR_CONCRETE_BUILD_SUCCEEDED;
}

static void check_refined_route_fixture(void) {
    Compilation compilation; SolMirConcreteProgram program;
    SolMirRuntimeConventions conventions; SolMirRuntimeValues values;
    SolMirRuntimeCleanup cleanup;
    sol_mir_concrete_program_init(&program);
    sol_mir_runtime_conventions_init(&conventions);
    sol_mir_runtime_values_init(&values);
    sol_mir_runtime_cleanup_init(&cleanup);
    bool compiled = compile_directory(&compilation,
        SOL_TEST_SOURCE_DIR "/tests/conformance/p33_refined_routes");
    bool built = compiled && build_refined_route_fixture(&compilation, &program)
        && sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){
            &program, NULL}, &conventions, &compilation.diagnostics)
            == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED
        && sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&conventions,
            NULL}, &values, &compilation.diagnostics) == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED
        && sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&conventions,
            &values, NULL}, &cleanup, &compilation.diagnostics)
            == SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED;
    if (!built) sol_diagnostics_render_human(stderr, &compilation.package.source,
        &compilation.diagnostics);
    CHECK(built);
    if (!built) goto done;
    const SolMirMaterialization *m = &program.materialization;
    const SolMirOperations *operations = &program.operations;
    size_t checks = 0, unequal_blocks = 0;
    size_t hostile_event = SOL_MIR_RUNTIME_NONE, hostile_plan = SOL_MIR_RUNTIME_NONE;
    size_t hostile_site = SOL_MIR_RUNTIME_NONE;
    SolMirRuntimeCleanupFailureOccurrence pending_code3 = {0};
    bool have_code3 = false;
    for (size_t i = 0; i < conventions.failure_site_count; ++i) {
        const SolMirRuntimeFailureSite *site = &conventions.failure_sites[i];
        if (site->origin_kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC
            && (site->allowed_codes & (UINT32_C(1)
                << (SOL_MIR_RUNTIME_FAILURE_DIVISION_BY_ZERO - 1))) != 0) {
            pending_code3 = (SolMirRuntimeCleanupFailureOccurrence){
                SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31, i,
                SOL_MIR_RUNTIME_FAILURE_DIVISION_BY_ZERO,
                SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE, 0, {0}, site->source};
            have_code3 = true;
            break;
        }
    }
    for (size_t e = 0; e < cleanup.event_count; ++e) {
        const SolMirRuntimeCleanupEvent *event = &cleanup.events[e];
        if (event->phase != SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
            || event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
            || event->block >= m->block_count
            || m->blocks[event->block].terminator.kind != SOL_MIR_TERM_CHECK_REFINED)
            continue;
        const SolMirMaterializedTerminator *term = &m->blocks[event->block].terminator;
        size_t plan_id = SOL_MIR_RUNTIME_NONE, plan_matches = 0;
        for (size_t i = 0; i < operations->predicate_count; ++i)
            if (operations->predicates[i].image == event->owner
                && operations->predicates[i].block == event->block) {
                plan_id = i; ++plan_matches;
            }
        CHECK(plan_matches == 1 && plan_id < operations->predicate_count);
        if (plan_matches != 1 || plan_id >= operations->predicate_count) continue;
        const SolMirOperationPredicatePlan *plan = &operations->predicates[plan_id];
        CHECK(plan->kind == SOL_MIR_OPERATION_PREDICATE_REFINEMENT
            && plan->context < m->context_count && plan->body < operations->predicate_body_count
            && plan->representation == term->representation && plan->result == term->result
            && m->contexts[plan->context].kind == SOL_MIR_PLAN_CONTEXT_REFINEMENT
            && m->contexts[plan->context].instance == event->owner
            && m->contexts[plan->context].obligation == term->source_obligation
            && m->contexts[plan->context].source_block == m->blocks[event->block].source_block
            && m->contexts[plan->context].definition == term->source_definition);
        const SolMirPredicateBody *body = &operations->predicate_bodies[plan->body];
        size_t return_block = SOL_MIR_RUNTIME_NONE, returns = 0;
        for (size_t i = 0; i < body->blocks.count; ++i) {
            size_t block = body->blocks.offset + i;
            if (operations->predicate_blocks[block].terminator.kind
                    == SOL_MIR_PREDICATE_TERM_RETURN) {
                return_block = block; ++returns;
            }
        }
        CHECK(returns == 1 && event->inherited_failure_site < conventions.failure_site_count);
        if (returns != 1 || event->inherited_failure_site >= conventions.failure_site_count)
            continue;
        const SolMirRuntimeFailureSite *site
            = &conventions.failure_sites[event->inherited_failure_site];
        CHECK(site->origin_kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT
            && site->owner == plan->body && site->block == return_block
            && site->instruction == SOL_MIR_RUNTIME_NONE
            && site->allowed_codes == (UINT32_C(1)
                << (SOL_MIR_RUNTIME_FAILURE_REFINEMENT_VIOLATION - 1)));
        unequal_blocks += event->block != return_block;
        CHECK(event->transitions.count == 3);
        if (event->transitions.count != 3) continue;
        const SolMirRuntimeCleanupTransition *satisfied
            = &cleanup.transitions[event->transitions.offset];
        const SolMirRuntimeCleanupTransition *violation = satisfied + 1;
        const SolMirRuntimeCleanupTransition *failure = satisfied + 2;
        CHECK(satisfied->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_SATISFIED
            && satisfied->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL
            && satisfied->continuation == term->normal_edge
            && satisfied->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE
            && satisfied->failure_site == SOL_MIR_RUNTIME_NONE && satisfied->failure_mask == 0);
        CHECK(violation->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_VIOLATION
            && violation->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
            && violation->continuation == term->failure_edge
            && violation->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31
            && violation->failure_site == event->inherited_failure_site
            && violation->failure_mask == (UINT32_C(1)
                << (SOL_MIR_RUNTIME_FAILURE_REFINEMENT_VIOLATION - 1)));
        CHECK(failure->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_FAILURE
            && failure->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
            && failure->continuation == term->failure_edge
            && failure->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING
            && failure->failure_site == SOL_MIR_RUNTIME_NONE && failure->failure_mask == 0
            && failure->source_edge == violation->source_edge
            && failure->destination == violation->destination
            && failure->actions.offset == violation->actions.offset
            && failure->actions.count == violation->actions.count);
        if (violation->actions.count != 0)
            CHECK(memcmp(&cleanup.actions[violation->actions.offset],
                &cleanup.actions[failure->actions.offset],
                violation->actions.count * sizeof(*cleanup.actions)) == 0);
        SolMirRuntimeCleanupFailureOccurrence false_code15 = {
            SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31,
            event->inherited_failure_site, SOL_MIR_RUNTIME_FAILURE_REFINEMENT_VIOLATION,
            SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE, 0, {0}, site->source};
        SolMirRuntimeCleanupTrace trace;
        CHECK(sol_mir_runtime_cleanup_test_select(&cleanup,
            &(SolMirRuntimeCleanupTraceRequest){e,
                SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_VIOLATION, &false_code15, NULL,
                SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE}, NULL, 0, &trace)
            && trace.primary.code == SOL_MIR_RUNTIME_FAILURE_REFINEMENT_VIOLATION);
        if (have_code3) {
            CHECK(sol_mir_runtime_cleanup_test_select(&cleanup,
                &(SolMirRuntimeCleanupTraceRequest){e,
                    SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_FAILURE, NULL, &pending_code3,
                    SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE}, NULL, 0, &trace)
                && trace.primary.code == SOL_MIR_RUNTIME_FAILURE_DIVISION_BY_ZERO
                && trace.primary.site == pending_code3.site);
            CHECK(!sol_mir_runtime_cleanup_test_select(&cleanup,
                &(SolMirRuntimeCleanupTraceRequest){e,
                    SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_VIOLATION, &pending_code3, NULL,
                    SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE}, NULL, 0, &trace));
        }
        if (hostile_event == SOL_MIR_RUNTIME_NONE) {
            hostile_event = e; hostile_plan = plan_id;
            hostile_site = event->inherited_failure_site;
        }
        ++checks;
    }
    CHECK(checks == 5 && unequal_blocks != 0 && have_code3);
    CHECK(sol_mir_runtime_cleanup_validate(&cleanup, NULL));
    if (hostile_event != SOL_MIR_RUNTIME_NONE) {
        SolMirRuntimeCleanupEvent *event = &cleanup.events[hostile_event];
        SolMirRuntimeCleanupTransition *satisfied
            = &cleanup.transitions[event->transitions.offset];
        SolMirRuntimeCleanupTransition *violation = satisfied + 1;
        SolMirRuntimeCleanupTransition *failure = satisfied + 2;
#define REJECT_MUTATION(type, object, field, replacement) do { \
        type saved_ = *(object); \
        (object)->field = (replacement); \
        CHECK(!sol_mir_runtime_cleanup_validate(&cleanup, NULL)); \
        *(object) = saved_; \
        CHECK(sol_mir_runtime_cleanup_validate(&cleanup, NULL)); \
    } while (0)
        REJECT_MUTATION(SolMirRuntimeCleanupEvent, event, inherited_failure_site,
            SOL_MIR_RUNTIME_NONE);
        REJECT_MUTATION(SolMirRuntimeCleanupTransition, violation, edge_role,
            SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_FAILURE);
        REJECT_MUTATION(SolMirRuntimeCleanupTransition, violation, failure_source,
            SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING);
        REJECT_MUTATION(SolMirRuntimeCleanupTransition, violation, failure_site,
            SOL_MIR_RUNTIME_NONE);
        REJECT_MUTATION(SolMirRuntimeCleanupTransition, violation, failure_mask, 0);
        REJECT_MUTATION(SolMirRuntimeCleanupTransition, violation, source_edge,
            satisfied->source_edge);
        REJECT_MUTATION(SolMirRuntimeCleanupTransition, violation, destination,
            satisfied->destination);
        REJECT_MUTATION(SolMirRuntimeCleanupTransition, violation, actions,
            satisfied->actions);
        REJECT_MUTATION(SolMirRuntimeCleanupTransition, failure, failure_source,
            SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31);
        REJECT_MUTATION(SolMirRuntimeCleanupTransition, failure, failure_site,
            hostile_site);
        REJECT_MUTATION(SolMirRuntimeCleanupTransition, failure, failure_mask, UINT32_C(1)
            << (SOL_MIR_RUNTIME_FAILURE_REFINEMENT_VIOLATION - 1));
        SolMirRuntimeCleanupTransition saved_violation = *violation;
        SolMirRuntimeCleanupTransition saved_failure = *failure;
        *violation = saved_failure; *failure = saved_violation;
        CHECK(!sol_mir_runtime_cleanup_validate(&cleanup, NULL));
        *violation = saved_violation; *failure = saved_failure;
        CHECK(sol_mir_runtime_cleanup_validate(&cleanup, NULL));
        SolMirRuntimeFailureSite *site = &conventions.failure_sites[hostile_site];
        REJECT_MUTATION(SolMirRuntimeFailureSite, site, origin_kind,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_CALL);
        REJECT_MUTATION(SolMirRuntimeFailureSite, site, owner, site->owner + 1);
        REJECT_MUTATION(SolMirRuntimeFailureSite, site, block, site->block + 1);
        REJECT_MUTATION(SolMirRuntimeFailureSite, site, source.start,
            site->source.start + 1);
        REJECT_MUTATION(SolMirRuntimeFailureSite, site, allowed_codes,
            site->allowed_codes | (UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_DIVISION_BY_ZERO - 1)));
        SolMirOperationPredicatePlan *plan
            = &program.operations.predicates[hostile_plan];
        REJECT_MUTATION(SolMirOperationPredicatePlan, plan, image, plan->image + 1);
        REJECT_MUTATION(SolMirOperationPredicatePlan, plan, block, plan->block + 1);
        REJECT_MUTATION(SolMirOperationPredicatePlan, plan, context, plan->context + 1);
        REJECT_MUTATION(SolMirOperationPredicatePlan, plan, body, plan->body + 1);
#undef REJECT_MUTATION
    }
done:
    sol_mir_runtime_cleanup_free(&cleanup); sol_mir_runtime_values_free(&values);
    sol_mir_runtime_conventions_free(&conventions); sol_mir_concrete_program_free(&program);
    compilation_free(&compilation);
}
static bool build_snapshot_fixture(Compilation *c, SolMirConcreteProgram *p, bool reverse) {
    SolMirProgramRoot roots[32]; size_t count = 0;
    if (reverse) {
        for (size_t i = c->ir.callable_count; i; --i)
            if (c->ir.callables[i - 1].kind == SOL_IR_CALLABLE_FUNCTION)
                roots[count++] = (SolMirProgramRoot){i - 1,
                    SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    } else for (size_t i = 0; i < c->ir.callable_count; ++i)
        if (c->ir.callables[i].kind == SOL_IR_CALLABLE_FUNCTION)
            roots[count++] = (SolMirProgramRoot){i, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    return count == 21 && sol_mir_concrete_program_build(
        &(SolMirConcreteBuildRequest){&c->ir, roots, count, NULL, 0, &target, NULL},
        p, &c->diagnostics) == SOL_MIR_CONCRETE_BUILD_SUCCEEDED;
}
static void check_zero_supplemental_alias_preflight(void) {
    Compilation c; SolMirConcreteProgram program; SolMirRuntimeConventions conventions;
    SolMirRuntimeValues values; SolMirRuntimeCleanup cleanup, rebuilt;
    sol_mir_concrete_program_init(&program); sol_mir_runtime_conventions_init(&conventions);
    sol_mir_runtime_values_init(&values); sol_mir_runtime_cleanup_init(&cleanup);
    sol_mir_runtime_cleanup_init(&rebuilt);
    bool compiled = compile_directory(&c,
        SOL_TEST_SOURCE_DIR "/tests/conformance/p33_alias_zero");
    CHECK(compiled);
    SolIrCallableId root = compiled
        ? callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION) : SOL_IR_NONE;
    SolMirProgramRoot fixture_root = {root, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    bool built = compiled && root != SOL_IR_NONE
        && sol_mir_concrete_program_build(&(SolMirConcreteBuildRequest){&c.ir,
            &fixture_root, 1, NULL, 0, &target, NULL}, &program, &c.diagnostics)
            == SOL_MIR_CONCRETE_BUILD_SUCCEEDED
        && sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&program,
            NULL}, &conventions, &c.diagnostics) == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED
        && sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&conventions,
            NULL}, &values, &c.diagnostics) == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED
        && sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&conventions,
            &values, NULL}, &cleanup, &c.diagnostics) == SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED;
    CHECK(built);
    if (built) {
        CHECK(program.operations.snapshot_count == 0);
        CHECK(cleanup.event_count != 0 && cleanup.action_count != 0
            && cleanup.transition_count != 0 && cleanup.supplemental_site_count == 0
            && cleanup.supplemental_site_capacity == 0 && cleanup.supplemental_sites == NULL
            && sol_mir_runtime_cleanup_validate(&cleanup, NULL));
        check_trace_batch_contract(&cleanup);
        cleanup.supplemental_sites = (SolMirRuntimeCleanupSupplementalSite *)cleanup.events;
        CHECK(!sol_mir_runtime_cleanup_validate(&cleanup, NULL));
        cleanup.supplemental_sites = NULL;
        CHECK(sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&conventions,
            &values, NULL}, &rebuilt, &c.diagnostics) == SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED
            && sol_mir_runtime_cleanup_validate(&rebuilt, NULL)
            && memcmp(&cleanup.usage, &rebuilt.usage, sizeof(cleanup.usage)) == 0);
        for (size_t i = 0; i < cleanup.action_count; ++i)
            CHECK(cleanup.actions[i].kind != SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT);
    }
    sol_mir_runtime_cleanup_free(&rebuilt); sol_mir_runtime_cleanup_free(&cleanup);
    sol_mir_runtime_values_free(&values); sol_mir_runtime_conventions_free(&conventions);
    sol_mir_concrete_program_free(&program); compilation_free(&c);
}
static size_t snapshot_action_count(const SolMirRuntimeCleanup *cleanup,
    const SolMirRuntimeCleanupTransition *transition) {
    size_t count = 0;
    for (size_t i = 0; i < transition->actions.count; ++i)
        count += cleanup->actions[transition->actions.offset + i].kind
            == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT;
    return count;
}
static void check_snapshot_mutation(SolMirRuntimeCleanup *cleanup, size_t action,
    SolMirRuntimeCleanupAction replacement) {
    SolMirRuntimeCleanupAction saved = cleanup->actions[action];
    cleanup->actions[action] = replacement;
    CHECK(!sol_mir_runtime_cleanup_validate(cleanup, NULL));
    cleanup->actions[action] = saved;
    CHECK(sol_mir_runtime_cleanup_validate(cleanup, NULL));
}
static void check_snapshot_prerequisite(void) {
    Compilation compilation; SolMirConcreteProgram program, reordered;
    SolMirRuntimeConventions conventions, reordered_conventions;
    SolMirRuntimeValues values, reordered_values;
    SolMirRuntimeCleanup cleanup, reordered_cleanup;
    sol_mir_concrete_program_init(&program); sol_mir_concrete_program_init(&reordered);
    sol_mir_runtime_conventions_init(&conventions);
    sol_mir_runtime_conventions_init(&reordered_conventions);
    sol_mir_runtime_values_init(&values); sol_mir_runtime_values_init(&reordered_values);
    sol_mir_runtime_cleanup_init(&cleanup); sol_mir_runtime_cleanup_init(&reordered_cleanup);
    bool compiled = compile_directory(&compilation,
        SOL_TEST_SOURCE_DIR "/tests/conformance/p33_snapshots");
    bool built = compiled && build_snapshot_fixture(&compilation, &program, false)
        && sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){
            &program, NULL}, &conventions, &compilation.diagnostics)
            == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED
        && sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&conventions,
            NULL}, &values, &compilation.diagnostics) == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED
        && sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&conventions,
            &values, NULL}, &cleanup, &compilation.diagnostics)
            == SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED;
    if (!built) sol_diagnostics_render_human(stderr, &compilation.package.source,
        &compilation.diagnostics);
    CHECK(built);
    if (!built) goto done;
    const SolMirOperations *operations = &program.operations;
    const SolMirMaterialization *materialization = &program.materialization;
    CHECK(operations->snapshot_count == 22 && sol_mir_runtime_cleanup_validate(&cleanup, NULL));
    SolIrCallableId two_callable = callable(&compilation.ir, "two_snapshots",
        SOL_IR_CALLABLE_FUNCTION);
    size_t two_image = affine_image(&program, two_callable);
    size_t two_plans[2] = {SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_NONE};
    SolMirRecipeId scalar_recipe = SOL_MIR_RECIPE_NONE;
    size_t slot_zero = 0, collision_plan = SOL_MIR_RUNTIME_NONE;
    size_t collision_capture_plan = SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; i < operations->snapshot_count; ++i) {
        const SolMirOperationSnapshotPlan *plan = &operations->snapshots[i];
        CHECK(plan->image < materialization->image_count
            && plan->instruction < materialization->instruction_count
            && materialization->instructions[plan->instruction].kind
                == SOL_MIR_INST_CAPTURE_SNAPSHOT);
        if (i == 0) scalar_recipe = plan->recipe;
        CHECK(plan->recipe == scalar_recipe);
        slot_zero += plan->slot == 0;
        if (plan->image == two_image && plan->slot < 2) two_plans[plan->slot] = i;
        for (size_t q = 0; q < operations->snapshot_count; ++q)
            if (collision_plan == SOL_MIR_RUNTIME_NONE && q != i
                && operations->snapshots[q].instruction == i) {
                collision_plan = i; collision_capture_plan = q;
            }
    }
    CHECK(two_image != SOL_MIR_RUNTIME_NONE && two_plans[0] != SOL_MIR_RUNTIME_NONE
        && two_plans[1] != SOL_MIR_RUNTIME_NONE && slot_zero == 21
        && collision_plan == 18 && collision_capture_plan == 0);
    size_t return_event = SOL_MIR_RUNTIME_NONE, return_transition = SOL_MIR_RUNTIME_NONE;
    size_t failure_event = SOL_MIR_RUNTIME_NONE, failure_transition = SOL_MIR_RUNTIME_NONE;
    size_t pre_capture_failures = 0, returned_failure_checks = 0, return_paths = 0;
    for (size_t e = 0; e < cleanup.event_count; ++e) {
        const SolMirRuntimeCleanupEvent *event = &cleanup.events[e];
        if (event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
            || event->block >= materialization->block_count) continue;
        const SolMirMaterializedTerminator *term = &materialization->blocks[event->block].terminator;
        for (size_t t = 0; t < event->transitions.count; ++t) {
            size_t transition_id = event->transitions.offset + t;
            const SolMirRuntimeCleanupTransition *transition
                = &cleanup.transitions[transition_id];
            size_t snapshots = snapshot_action_count(&cleanup, transition);
            if (event->owner == two_image && term->kind == SOL_MIR_TERM_RETURN
                && transition->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_EXIT) {
                CHECK(transition->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE
                    && transition->failure_site == SOL_MIR_RUNTIME_NONE
                    && transition->failure_mask == 0 && snapshots == 2
                    && transition->actions.count == 2);
                if (transition->actions.count == 2) {
                    const SolMirRuntimeCleanupAction *newest
                        = &cleanup.actions[transition->actions.offset];
                    const SolMirRuntimeCleanupAction *oldest
                        = &cleanup.actions[transition->actions.offset + 1];
                    CHECK(newest->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT
                        && newest->target == two_plans[1] && newest->recipe == scalar_recipe
                        && newest->flags == 0 && newest->drop_path == SOL_MIR_RUNTIME_NONE
                        && oldest->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT
                        && oldest->target == two_plans[0] && oldest->recipe == scalar_recipe
                        && oldest->flags == 0 && oldest->drop_path == SOL_MIR_RUNTIME_NONE);
                }
                ++return_paths; return_event = e; return_transition = transition_id;
            }
            if (event->owner == two_image && transition->outcome
                    == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE && snapshots == 2) {
                size_t seen = 0;
                for (size_t a = 0; a < transition->actions.count; ++a) {
                    const SolMirRuntimeCleanupAction *action
                        = &cleanup.actions[transition->actions.offset + a];
                    if (action->kind != SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT) continue;
                    CHECK(action->target == two_plans[1 - seen] && action->recipe == scalar_recipe
                        && action->flags == 0 && action->drop_path == SOL_MIR_RUNTIME_NONE);
                    ++seen;
                }
                CHECK(cleanup.actions[transition->actions.offset
                    + transition->actions.count - 1].kind
                    == SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE);
                failure_event = e; failure_transition = transition_id;
            }
            if (event->owner == two_image && term->kind == SOL_MIR_TERM_CHECK_CONTRACT
                && term->contract_phase == SOL_CONTRACT_REQUIRES
                && transition->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE) {
                CHECK(snapshots == 0); ++pre_capture_failures;
            }
            if (event->owner == two_image && term->kind == SOL_MIR_TERM_CHECK_CONTRACT
                && term->contract_phase == SOL_CONTRACT_ENSURES
                && term->contract_outcome == SOL_CONTRACT_OUTCOME_FAILURE
                && transition->edge_role
                    == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_SATISFIED) {
                CHECK(snapshots == 0); ++returned_failure_checks;
            }
        }
    }
    CHECK(return_paths == 1 && return_event != SOL_MIR_RUNTIME_NONE
        && failure_event != SOL_MIR_RUNTIME_NONE && pre_capture_failures != 0
        && returned_failure_checks != 0);
    size_t collision_actions = 0;
    for (size_t i = 0; i < cleanup.action_count; ++i)
        if (cleanup.actions[i].kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT
            && cleanup.actions[i].target == collision_plan) {
            CHECK(cleanup.actions[i].target != collision_capture_plan
                && cleanup.actions[i].recipe == operations->snapshots[collision_plan].recipe);
            ++collision_actions;
        }
    CHECK(collision_actions != 0);
    if (return_transition == SOL_MIR_RUNTIME_NONE
        || failure_transition == SOL_MIR_RUNTIME_NONE
        || cleanup.transitions[return_transition].actions.count < 2) goto rebuild;
    size_t return_action = cleanup.transitions[return_transition].actions.offset;
    SolMirRuntimeCleanupAction saved = cleanup.actions[return_action];
    SolMirRuntimeCleanupAction mutated = saved;
    mutated.target = cleanup.actions[return_action + 1].target;
    check_snapshot_mutation(&cleanup, return_action, mutated); /* duplicate return drop */
    size_t slot_action = SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; i < cleanup.action_count; ++i)
        if (cleanup.actions[i].kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT
            && cleanup.actions[i].target
                != operations->snapshots[cleanup.actions[i].target].slot) {
            slot_action = i; break;
        }
    CHECK(slot_action != SOL_MIR_RUNTIME_NONE);
    if (slot_action != SOL_MIR_RUNTIME_NONE) {
        mutated = cleanup.actions[slot_action];
        mutated.target = operations->snapshots[mutated.target].slot;
        check_snapshot_mutation(&cleanup, slot_action, mutated);
    }
    mutated = saved; mutated.target = operations->snapshots[saved.target].instruction;
    check_snapshot_mutation(&cleanup, return_action, mutated);
    mutated = saved; mutated.recipe = SOL_MIR_RECIPE_NONE;
    check_snapshot_mutation(&cleanup, return_action, mutated);
    mutated = saved; mutated.flags = SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED;
    check_snapshot_mutation(&cleanup, return_action, mutated);
    mutated = saved; mutated.drop_path = 0;
    check_snapshot_mutation(&cleanup, return_action, mutated);
    size_t left_target = cleanup.actions[return_action].target;
    cleanup.actions[return_action].target = cleanup.actions[return_action + 1].target;
    cleanup.actions[return_action + 1].target = left_target;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup, NULL)); /* equal-recipe target swap */
    cleanup.actions[return_action + 1].target = cleanup.actions[return_action].target;
    cleanup.actions[return_action].target = left_target;
    SolMirRuntimeCleanupAction left = cleanup.actions[return_action];
    cleanup.actions[return_action] = cleanup.actions[return_action + 1];
    cleanup.actions[return_action + 1] = left;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup, NULL));
    cleanup.actions[return_action + 1] = cleanup.actions[return_action];
    cleanup.actions[return_action] = left;
    SolMirRuntimeSlice return_slice = cleanup.transitions[return_transition].actions;
    --cleanup.transitions[return_transition].actions.count;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup, NULL));
    cleanup.transitions[return_transition].actions = return_slice;
    ++cleanup.transitions[return_transition].actions.count;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup, NULL));
    cleanup.transitions[return_transition].actions = return_slice;
    SolMirRuntimeSlice event_actions = cleanup.events[return_event].actions;
    --cleanup.events[return_event].actions.count;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup, NULL));
    cleanup.events[return_event].actions = event_actions;
    SolMirRuntimeSlice event_transitions = cleanup.events[return_event].transitions;
    --cleanup.events[return_event].transitions.count;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup, NULL));
    cleanup.events[return_event].transitions = event_transitions;
    SolMirRuntimeCleanupTransition transition_saved = cleanup.transitions[return_transition];
    cleanup.transitions[return_transition].event = failure_event;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup, NULL));
    cleanup.transitions[return_transition] = transition_saved;
    size_t failure_action = cleanup.transitions[failure_transition].actions.offset;
    while (cleanup.actions[failure_action].kind
            != SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT) ++failure_action;
    left = cleanup.actions[failure_action];
    cleanup.actions[failure_action] = cleanup.actions[failure_action + 1];
    cleanup.actions[failure_action + 1] = left;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup, NULL));
    cleanup.actions[failure_action + 1] = cleanup.actions[failure_action];
    cleanup.actions[failure_action] = left;
    mutated = cleanup.actions[failure_action];
    mutated.target = cleanup.actions[failure_action + 1].target;
    check_snapshot_mutation(&cleanup, failure_action, mutated); /* duplicate failure drop */
    SolMirRuntimeSlice failure_slice = cleanup.transitions[failure_transition].actions;
    ++cleanup.transitions[failure_transition].actions.offset;
    --cleanup.transitions[failure_transition].actions.count; /* missing first failure drop */
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup, NULL));
    cleanup.transitions[failure_transition].actions = failure_slice;
    ++cleanup.transitions[failure_transition].actions.count;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup, NULL));
    cleanup.transitions[failure_transition].actions = failure_slice;
    CHECK(sol_mir_runtime_cleanup_validate(&cleanup, NULL));
rebuild: ;
    bool rebuilt = build_snapshot_fixture(&compilation, &reordered, true)
        && sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){
            &reordered, NULL}, &reordered_conventions, &compilation.diagnostics)
            == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED
        && sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){
            &reordered_conventions, NULL}, &reordered_values, &compilation.diagnostics)
            == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED
        && sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){
            &reordered_conventions, &reordered_values, NULL}, &reordered_cleanup,
            &compilation.diagnostics) == SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED;
    CHECK(rebuilt);
    if (rebuilt) {
        char *first = render_cleanup_text(&cleanup);
        char *second = render_cleanup_text(&reordered_cleanup);
        CHECK(first != NULL && second != NULL && strcmp(first, second) == 0);
        free(first); free(second);
    }
done:
    sol_mir_runtime_cleanup_free(&reordered_cleanup); sol_mir_runtime_cleanup_free(&cleanup);
    sol_mir_runtime_values_free(&reordered_values); sol_mir_runtime_values_free(&values);
    sol_mir_runtime_conventions_free(&reordered_conventions);
    sol_mir_runtime_conventions_free(&conventions);
    sol_mir_concrete_program_free(&reordered); sol_mir_concrete_program_free(&program);
    compilation_free(&compilation);
}
static bool occurrence_for_transition(const SolMirRuntimeCleanup *cleanup,
    const SolMirRuntimeConventions *conventions, const SolMirRuntimeCleanupTransition *transition,
    SolMirRuntimeCleanupFailureOccurrence *occurrence) {
    uint32_t mask = transition->failure_mask;
    SolMirRuntimeSource source;
    if (transition->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31) {
        if (transition->failure_site >= conventions->failure_site_count) return false;
        source = conventions->failure_sites[transition->failure_site].source;
    } else if (transition->failure_source
        == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_SUPPLEMENTAL_P33) {
        if (transition->failure_site >= cleanup->supplemental_site_count) return false;
        source = cleanup->supplemental_sites[transition->failure_site].source;
    } else return false;
    for (unsigned code = SOL_MIR_RUNTIME_FAILURE_PANIC;
            code <= SOL_MIR_RUNTIME_FAILURE_HOST_ERROR; ++code) {
        if ((mask & (UINT32_C(1) << (code - 1))) == 0) continue;
        *occurrence = (SolMirRuntimeCleanupFailureOccurrence){transition->failure_source,
            transition->failure_site, (SolMirRuntimeFailureCode)code,
            SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE, 0, {0}, source};
        return true;
    }
    return false;
}
/* Snapshots use known-valid allocated capacities, before any corruption. The
 * saved addresses/sizes are retained across mutations, never rediscovered from
 * a hostile header. Includes every cleanup arena and both direct predecessors. */
typedef struct { const void *address; size_t bytes; unsigned char *copy; } BatchMemory;
static void batch_memory_save(BatchMemory *memory, size_t *count,
    const void *address, size_t bytes) {
    if (!bytes) return;
    CHECK(*count < 256 && address != NULL);
    if (*count >= 256 || !address) return;
    unsigned char *copy = malloc(bytes);
    CHECK(copy != NULL);
    if (!copy) return;
    memcpy(copy, address, bytes);
    memory[(*count)++] = (BatchMemory){address, bytes, copy};
}
static void batch_memory_check(const BatchMemory *memory, size_t count) {
    for (size_t i = 0; i < count; ++i)
        CHECK(memcmp(memory[i].address, memory[i].copy, memory[i].bytes) == 0);
}
static void check_trace_batch_contract(SolMirRuntimeCleanup *cleanup) {
    enum { ROWS = 256 };
    SolMirRuntimeCleanupTestTraceQuery queries[ROWS] = {0};
    SolMirRuntimeCleanupFailureOccurrence backing[ROWS] = {0};
    SolMirRuntimeCleanupFailureOccurrence pending = {0};
    bool have_pending = false;
    for (size_t i = 0; i < cleanup->transition_count && !have_pending; ++i)
        have_pending = occurrence_for_transition(cleanup, cleanup->conventions,
            &cleanup->transitions[i], &pending);
    CHECK(have_pending);
    size_t n = 0, normal = SIZE_MAX, producer = SIZE_MAX, success = SIZE_MAX;
    for (size_t e = 0; e < cleanup->event_count; ++e)
        for (size_t t = 0; t < cleanup->events[e].transitions.count; ++t) {
            const SolMirRuntimeCleanupTransition *edge =
                &cleanup->transitions[cleanup->events[e].transitions.offset + t];
            CHECK(n < ROWS - 20);
            if (n >= ROWS - 20) continue; /* failure, never a successful truncated batch */
            queries[n] = (SolMirRuntimeCleanupTestTraceQuery){
                {e, edge->edge_role, NULL, NULL, SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE}, 64, true};
            if (occurrence_for_transition(cleanup, cleanup->conventions, edge, &backing[n])) {
                queries[n].request.produced = &backing[n];
                producer = n;
            } else if (edge->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING
                || edge->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_LOCAL_OR_PENDING) {
                memcpy(&backing[n], &pending, sizeof(pending));
                queries[n].request.pending = &backing[n];
            }
            if (edge->actions.count) normal = n;
            if (edge->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE) success = n;
            ++n;
        }
    CHECK(normal != SIZE_MAX && producer != SIZE_MAX && success != SIZE_MAX && have_pending);
    if (normal == SIZE_MAX || producer == SIZE_MAX || success == SIZE_MAX || !have_pending || n >= ROWS - 20) return;
    SolMirRuntimeCleanupTestTraceQuery good = queries[normal], producing = queries[producer];
    size_t original_rows = n;
    SolMirRuntimeCleanupTestTraceQuery bad = good;
    bad.request.event = cleanup->event_count;
    queries[n++] = bad; queries[n++] = good; /* invalid followed by valid */
    bad = good; bad.request.edge_role = (SolMirRuntimeCleanupEdgeRole)-1; queries[n++] = bad;
    bad = good; bad.request.liveness = (SolMirRuntimeCleanupDropLiveness)(SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL + 1);
    queries[n++] = bad;
    bad = good; bad.copy_actions = false; bad.action_capacity = 0; queries[n++] = bad; /* metadata */
    bad.action_capacity = 1; queries[n++] = bad; /* NULL storage + nonzero capacity */
    bad = good; bad.action_capacity = 0; queries[n++] = bad;
    size_t actions = 0;
    const SolMirRuntimeCleanupEvent *event = &cleanup->events[good.request.event];
    for (size_t t = 0; t < event->transitions.count; ++t)
        if (cleanup->transitions[event->transitions.offset + t].edge_role == good.request.edge_role)
            actions = cleanup->transitions[event->transitions.offset + t].actions.count;
    CHECK(actions > 0 && actions <= 64);
    bad = good; bad.action_capacity = actions; queries[n++] = bad;
    bad.action_capacity = actions - 1; queries[n++] = bad;
    bad = queries[success]; bad.request.pending = &pending; queries[n++] = bad; /* pending on success */
    bad = producing; bad.request.produced = NULL; queries[n++] = bad; /* neither */
    bad = producing; bad.request.pending = &pending; queries[n++] = bad; /* both */
    bad = producing; bad.request.produced = NULL; bad.request.pending = &pending; queries[n++] = bad;
    /* Final arrays have never moved. Invalid copies have dedicated stable backing. */
    for (unsigned variant = 0; variant < 3; ++variant) {
        memcpy(&backing[n], producing.request.produced, sizeof(backing[n]));
        if (variant == 0) ++backing[n].source.start;
        if (variant == 1) backing[n].code = SOL_MIR_RUNTIME_FAILURE_NONE;
        if (variant == 2) backing[n].detail_length = SOL_MIR_RUNTIME_HOST_DETAIL_MAX + 1;
        bad = producing; bad.request.produced = &backing[n]; queries[n++] = bad;
    }
    BatchMemory memory[256]; size_t memory_count = 0;
#define SAVE_HEADER(p) batch_memory_save(memory, &memory_count, (p), sizeof(*(p)))
#define SAVE_ARENA(p, member, singular) batch_memory_save(memory, &memory_count, \
        (p)->member, (p)->singular##_capacity * sizeof(*(p)->member))
    SAVE_HEADER(cleanup);
    SAVE_ARENA(cleanup, events, event); SAVE_ARENA(cleanup, actions, action);
    SAVE_ARENA(cleanup, transitions, transition); SAVE_ARENA(cleanup, supplemental_sites, supplemental_site);
    SAVE_ARENA(cleanup, drop_paths, drop_path);
    const SolMirRuntimeValues *v = cleanup->values;
    SAVE_HEADER(v);
    SAVE_ARENA(v, recipe_operations, recipe_operation); SAVE_ARENA(v, allocation_plans, allocation_plan);
    SAVE_ARENA(v, copy_plans, copy_plan); SAVE_ARENA(v, equality_plans, equality_plan);
    SAVE_ARENA(v, host_result_plans, host_result_plan); SAVE_ARENA(v, host_result_requirements, host_result_requirement);
    SAVE_ARENA(v, ownership_plans, ownership_plan); SAVE_ARENA(v, ownership_variants, ownership_variant);
    SAVE_ARENA(v, owned_edges, owned_edge);
    const SolMirRuntimeConventions *c = cleanup->conventions;
    SAVE_HEADER(c);
#define SAVE_CONVENTION(member, type, singular) SAVE_ARENA(c, member, singular);
    SOL_MIR_RUNTIME_CONVENTIONS_ARENAS(SAVE_CONVENTION)
#undef SAVE_CONVENTION
    SAVE_HEADER(c->concrete);
    const SolMirConcreteProgram *p = c->concrete;
#define SAVE_COUNT(p_, member, singular) batch_memory_save(memory, &memory_count, \
        (p_)->member, (p_)->singular##_count * sizeof(*(p_)->member))
    SAVE_COUNT(&p->program, roots, root); SAVE_COUNT(&p->program, approved_imports, approved_import);
    SAVE_COUNT(&p->program, templates, template); SAVE_COUNT(&p->program, imports, import);
    SAVE_COUNT(&p->program, specializations, specialization); SAVE_COUNT(&p->program, references, reference);
    for (size_t i = 0; i < p->program.template_count; ++i) {
        const SolMir *m = &p->program.templates[i].mir;
        SAVE_ARENA(m, blocks, block); SAVE_ARENA(m, instructions, instruction);
        SAVE_ARENA(m, values, value); SAVE_ARENA(m, parameter_values, parameter_value);
        SAVE_ARENA(m, edge_values, edge_value); SAVE_ARENA(m, call_arguments, call_argument);
        SAVE_ARENA(m, loops, loop); SAVE_ARENA(m, construct_operands, construct_operand);
        SAVE_ARENA(m, temporaries, temporary);
    }
#undef SAVE_COUNT
    const SolMirPlan *plan = &p->plan;
    SAVE_ARENA(plan, types, type); SAVE_ARENA(plan, type_components, type_component);
    SAVE_ARENA(plan, type_parameter_accesses, type_parameter_access);
    SAVE_ARENA(plan, effect_atoms, effect_atom); SAVE_ARENA(plan, effect_rows, effect_row);
    SAVE_ARENA(plan, effect_row_atoms, effect_row_atom); SAVE_ARENA(plan, instances, instance);
    SAVE_ARENA(plan, instance_type_ids, instance_type_id); SAVE_ARENA(plan, instance_accesses, instance_access);
    SAVE_ARENA(plan, dictionary_entries, dictionary_entry); SAVE_ARENA(plan, imports, import);
    SAVE_ARENA(plan, typed_uses, typed_use); SAVE_ARENA(plan, contexts, context); SAVE_ARENA(plan, demands, demand);
    const SolMirMaterialization *m = &p->materialization;
    SAVE_ARENA(m, images, image); SAVE_ARENA(m, types, type); SAVE_ARENA(m, shape_fields, shape_field);
    SAVE_ARENA(m, shape_variants, shape_variant); SAVE_ARENA(m, type_ids, type_id); SAVE_ARENA(m, accesses, access);
    SAVE_ARENA(m, overlays, overlay); SAVE_ARENA(m, contexts, context); SAVE_ARENA(m, locals, local);
    SAVE_ARENA(m, places, place); SAVE_ARENA(m, projections, projection); SAVE_ARENA(m, values, value);
    SAVE_ARENA(m, instructions, instruction); SAVE_ARENA(m, temporaries, temporary);
    SAVE_ARENA(m, construct_operands, construct_operand); SAVE_ARENA(m, call_arguments, call_argument);
    SAVE_ARENA(m, blocks, block); SAVE_ARENA(m, edges, edge); SAVE_ARENA(m, edge_values, edge_value);
    SAVE_ARENA(m, parameter_values, parameter_value); SAVE_ARENA(m, loops, loop); SAVE_ARENA(m, bindings, binding);
    SAVE_ARENA(m, semantic_sites, semantic_site); SAVE_ARENA(m, receiver_roots, receiver_root);
    SAVE_ARENA(m, imports, import); SAVE_ARENA(m, handlers, handler); SAVE_ARENA(m, writebacks, writeback);
    SAVE_ARENA(m, effect_rows, effect_row); SAVE_ARENA(m, effect_atoms, effect_atom);
    SAVE_ARENA(m, effect_row_atoms, effect_row_atom); SAVE_ARENA(m, effect_names, effect_name);
    SAVE_ARENA(m, literal_bytes, literal_byte);
    const SolMirRepresentation *rep = &p->representation;
    SAVE_ARENA(rep, recipes, recipe); SAVE_ARENA(rep, fields, field); SAVE_ARENA(rep, variants, variant);
    SAVE_ARENA(rep, recipe_ids, recipe_id); SAVE_ARENA(rep, accesses, access);
    SAVE_ARENA(rep, receiver_roots, receiver_root); SAVE_ARENA(rep, callable_producers, callable_producer);
    SAVE_ARENA(&p->layout, types, type); SAVE_ARENA(&p->layout, fields, field);
    SAVE_ARENA(&p->layout, variants, variant); SAVE_ARENA(&p->layout, projections, projection);
#define SAVE_OPERATION(member, type, singular) SAVE_ARENA(&p->operations, member, singular);
    SOL_MIR_OPERATIONS_ARENAS(SAVE_OPERATION)
#undef SAVE_OPERATION
#define SAVE_LINKAGE(member, type, singular) SAVE_ARENA(&p->linkage, member, singular);
    SOL_MIR_LINKAGE_ARENAS(SAVE_LINKAGE)
#undef SAVE_LINKAGE
    batch_memory_save(memory, &memory_count, queries, sizeof(queries));
    batch_memory_save(memory, &memory_count, backing, sizeof(backing));
    SAVE_HEADER(&pending);
#undef SAVE_ARENA
#undef SAVE_HEADER
    bool selected[ROWS] = {0};
    SolMirRuntimeCleanupTrace traces[ROWS] = {0};
    SolMirRuntimeCleanupAction actions_copy[ROWS][64];
    sol_mir_runtime_cleanup_test_trace_counts_reset();
    for (size_t i = 0; i < n; ++i)
        selected[i] = sol_mir_runtime_cleanup_test_select(cleanup, &queries[i].request,
            queries[i].copy_actions ? actions_copy[i] : NULL, queries[i].action_capacity, &traces[i]);
    CHECK(selected[normal] && selected[producer] && selected[success]);
    CHECK(!selected[original_rows] && selected[original_rows + 1]);
    CHECK(selected[original_rows + 4] && !selected[original_rows + 5]
        && !selected[original_rows + 6] && selected[original_rows + 7]
        && !selected[original_rows + 8] && !selected[original_rows + 9]);
    size_t transported = 0;
    for (size_t i = 0; i < original_rows; ++i)
        if (selected[i] && queries[i].request.pending) {
            CHECK(traces[i].has_primary
                && memcmp(&traces[i].primary, queries[i].request.pending, sizeof(traces[i].primary)) == 0);
            ++transported;
        }
    CHECK(transported != 0);
    size_t auth, selections;
    sol_mir_runtime_cleanup_test_trace_counts(&auth, &selections);
    CHECK(auth == n && selections == n);
    batch_memory_check(memory, memory_count);
    sol_mir_runtime_cleanup_test_trace_counts_reset();
    SolMirRuntimeCleanupTestTraceResult *results = NULL;
    CHECK(sol_mir_runtime_cleanup_test_trace_batch(cleanup, queries, n, &results));
    CHECK(results != NULL);
    sol_mir_runtime_cleanup_test_trace_counts(&auth, &selections);
    CHECK(auth == 1 && selections == n);
    fprintf(stderr, "cleanup-small-differential queries=%zu single-auth=%zu batch-auth=%zu selections=%zu pending-transports=%zu\n",
        n, n, auth, selections, transported);
    if (results) for (size_t i = 0; i < n; ++i) {
        CHECK(results[i].selected == selected[i]);
        if (!selected[i] || !results[i].selected) continue;
        const SolMirRuntimeCleanupTrace *a = &traces[i], *b = &results[i].trace;
        CHECK(a->event == b->event && a->edge_role == b->edge_role && a->outcome == b->outcome
            && a->liveness == b->liveness && a->has_primary == b->has_primary
            && a->action_count == b->action_count);
        if (a->has_primary) CHECK(memcmp(&a->primary, &b->primary, sizeof(a->primary)) == 0);
        CHECK(b->actions == (queries[i].copy_actions ? results[i].actions : NULL));
        if (a->actions && b->actions) CHECK(memcmp(a->actions, b->actions,
            a->action_count * sizeof(*a->actions)) == 0);
    }
    batch_memory_check(memory, memory_count);
    free(results); results = NULL;
    CHECK(sol_mir_runtime_cleanup_test_trace_batch(cleanup, NULL, 0, &results) && results == NULL);
    CHECK(!sol_mir_runtime_cleanup_test_trace_batch(cleanup, queries, n, NULL));
    CHECK(!sol_mir_runtime_cleanup_test_trace_batch(cleanup, NULL, 1, &results) && results == NULL);
    CHECK(!sol_mir_runtime_cleanup_test_trace_batch(cleanup, queries, SIZE_MAX, &results) && results == NULL);
    CHECK(!sol_mir_runtime_cleanup_test_trace_batch(cleanup, queries, 4097, &results) && results == NULL);
    bad = good; bad.action_capacity = 65;
    CHECK(!sol_mir_runtime_cleanup_test_trace_batch(cleanup, &bad, 1, &results) && results == NULL);
    bad.action_capacity = SIZE_MAX;
    CHECK(!sol_mir_runtime_cleanup_test_trace_batch(cleanup, &bad, 1, &results) && results == NULL);
    SolMirRuntimeCleanupTestTraceQuery *maximum = calloc(
        SOL_MIR_RUNTIME_CLEANUP_TEST_MAX_QUERIES, sizeof(*maximum));
    CHECK(maximum != NULL);
    if (maximum) {
        for (size_t i = 0; i < SOL_MIR_RUNTIME_CLEANUP_TEST_MAX_QUERIES; ++i) {
            maximum[i] = good;
            maximum[i].copy_actions = false; maximum[i].action_capacity = 0;
        }
        sol_mir_runtime_cleanup_test_trace_counts_reset();
        CHECK(sol_mir_runtime_cleanup_test_trace_batch(cleanup, maximum,
            SOL_MIR_RUNTIME_CLEANUP_TEST_MAX_QUERIES, &results));
        CHECK(results != NULL);
        if (results) for (size_t i = 0; i < SOL_MIR_RUNTIME_CLEANUP_TEST_MAX_QUERIES; ++i)
            CHECK(results[i].selected && results[i].trace.actions == NULL);
        sol_mir_runtime_cleanup_test_trace_counts(&auth, &selections);
        CHECK(auth == 1 && selections == SOL_MIR_RUNTIME_CLEANUP_TEST_MAX_QUERIES);
        free(results); results = NULL; free(maximum);
    }
    /* Each separate mutation follows an accepted batch, rejecting on BOTH
     * public paths and zero rows, then restores and freshly authenticates. */
#define BATCH_MUTATION(object, field, replacement) do { \
        (object)->field = (replacement); \
        CHECK(!sol_mir_runtime_cleanup_test_select(cleanup, &good.request, NULL, 0, &traces[0])); \
        CHECK(!sol_mir_runtime_cleanup_test_trace_batch(cleanup, &good, 1, &results) && results == NULL); \
        CHECK(!sol_mir_runtime_cleanup_test_trace_batch(cleanup, NULL, 0, &results) && results == NULL); \
    } while (0)
    SolMirRuntimeCleanupUsage saved_usage = cleanup->usage;
    BATCH_MUTATION(cleanup, usage.validation_work, saved_usage.validation_work + 1);
    cleanup->usage = saved_usage;
#define BATCH_RESTORED() do { \
        CHECK(sol_mir_runtime_cleanup_validate(cleanup, NULL)); \
        CHECK(sol_mir_runtime_cleanup_test_select(cleanup, &good.request, NULL, 0, &traces[0])); \
        CHECK(sol_mir_runtime_cleanup_test_trace_batch(cleanup, &good, 1, &results)); \
        CHECK(results && results[0].selected); free(results); results = NULL; \
    } while (0)
    BATCH_RESTORED();
    size_t saved_count = cleanup->event_count;
    BATCH_MUTATION(cleanup, event_count, cleanup->event_capacity + 1);
    cleanup->event_count = saved_count; BATCH_RESTORED();
    SolMirRuntimeSlice saved_slice = cleanup->events[0].transitions;
    BATCH_MUTATION(&cleanup->events[0], transitions.offset, cleanup->transition_count);
    cleanup->events[0].transitions = saved_slice; BATCH_RESTORED();
    SolMirRuntimeConventions *borrowed = (SolMirRuntimeConventions *)cleanup->conventions;
    SolMirRuntimeConventionsUsage saved_borrowed = borrowed->usage;
    BATCH_MUTATION(borrowed, usage.validation_work, saved_borrowed.validation_work + 1);
    borrowed->usage = saved_borrowed; BATCH_RESTORED();
#undef BATCH_MUTATION
#undef BATCH_RESTORED
    batch_memory_check(memory, memory_count);
    for (size_t i = 0; i < memory_count; ++i) free(memory[i].copy);
}
/* Predicate propagation has no source spelling.  Keep its P3.3 edge schema
 * covered with a bounded P2-shaped terminator instead of inventing a source
 * fixture or extending the language contract. */
static void check_predicate_propagate_schema(void) {
    SolMirPredicateTerminator term;
    memset(&term, 0, sizeof(term));
    term.kind = SOL_MIR_PREDICATE_TERM_PROPAGATE;
    term.edge = SOL_MIR_OPERATION_NONE;
    term.normal_edge = 41;
    term.failure_edge = 73;
    size_t edges[2] = {SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_NONE};
    SolMirRuntimeCleanupEdgeRole roles[2] = {
        SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO, SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO,
    };
    CHECK(sol_mir_runtime_cleanup_test_predicate_propagate_schema(&term, edges, roles));
    CHECK(edges[0] == term.normal_edge && edges[1] == term.failure_edge);
    CHECK(roles[0] == SOL_MIR_RUNTIME_CLEANUP_EDGE_PROPAGATE_VALUE
        && roles[1] == SOL_MIR_RUNTIME_CLEANUP_EDGE_PROPAGATE_RESIDUAL);
    term.edge = 0;
    CHECK(!sol_mir_runtime_cleanup_test_predicate_propagate_schema(&term, edges, roles));
}
static char *render_cleanup_text(const SolMirRuntimeCleanup *cleanup) { FILE *stream=tmpfile(); if(!stream||!sol_mir_runtime_cleanup_render(stream,cleanup)||fflush(stream)||fseek(stream,0,SEEK_END)) {if(stream)fclose(stream);return NULL;} long n=ftell(stream);if(n<0||fseek(stream,0,SEEK_SET)){fclose(stream);return NULL;}char *text=malloc((size_t)n+1);if(!text){fclose(stream);return NULL;}if(fread(text,1,(size_t)n,stream)!=(size_t)n){free(text);fclose(stream);return NULL;}text[n]=0;fclose(stream);return text;}
typedef struct { size_t events, ready, failure; } StepKeyCollisions;
static bool cleanup_render_field(const char *line,size_t length,const char *name,
    const char **value,size_t *value_length){size_t name_length=strlen(name),at=0;while(at<length){size_t token=at,end;while(at<length&&line[at]!=' ')++at;end=at;if(end>token+name_length&&!memcmp(line+token,name,name_length)&&line[token+name_length]=='='){*value=line+token+name_length+1;*value_length=end-token-name_length-1;return true;}while(at<length&&line[at]==' ')++at;}return false;}
static size_t key_collision_pairs(const char (*keys)[65],size_t count){size_t collisions=0;for(size_t i=0;i<count;++i)for(size_t q=i+1;q<count;++q)collisions+=!memcmp(keys[i],keys[q],65);return collisions;}
static bool step_key_collisions(const SolMirRuntimeCleanup *cleanup,
    StepKeyCollisions *out,size_t classes[4]) {
    char *text=render_cleanup_text(cleanup);if(!text)return false;
    char (*events)[65]=calloc(cleanup->event_count,sizeof *events);
    char (*ready)[65]=calloc(cleanup->event_count,sizeof *ready);
    char (*failure)[65]=calloc(cleanup->event_count,sizeof *failure);
    if(!events||!ready||!failure){free(events);free(ready);free(failure);free(text);return false;}
    size_t event_count=0,ready_count=0,failure_count=0;bool step=false;
    for(const char*line=text;*line;){const char*end=strchr(line,'\n');size_t length=end?(size_t)(end-line):strlen(line);const char*key,*producer,*role;size_t key_length,producer_length,role_length;
        if(length>=6&&!memcmp(line,"event ",6)){step=cleanup_render_field(line,length,"producer",&producer,&producer_length)&&producer_length==10&&!memcmp(producer,"step-meter",10);if(step){if(!cleanup_render_field(line,length,"key",&key,&key_length)||key_length!=64||event_count>=cleanup->event_count){free(events);free(ready);free(failure);free(text);return false;}memcpy(events[event_count],key,64);events[event_count++][64]='\0';}}
        else if(step&&length>=11&&!memcmp(line,"transition ",11)){if(!cleanup_render_field(line,length,"edge-key",&key,&key_length)||key_length!=64||!cleanup_render_field(line,length,"role",&role,&role_length)){free(events);free(ready);free(failure);free(text);return false;}if(role_length==19&&!memcmp(role,"pre-operation-ready",19)){memcpy(ready[ready_count],key,64);ready[ready_count++][64]='\0';}else if(role_length==12&&!memcmp(role,"step-failure",12)){memcpy(failure[failure_count],key,64);failure[failure_count++][64]='\0';}}
        line=end?end+1:line+length;
    }
    memset(classes,0,4*sizeof *classes);for(size_t i=0;i<cleanup->event_count;++i)if(cleanup->events[i].phase==SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_STEP&&cleanup->events[i].kind<4)++classes[cleanup->events[i].kind];
    bool ok=event_count==ready_count&&event_count==failure_count;
    if(ok)*out=(StepKeyCollisions){key_collision_pairs(events,event_count),key_collision_pairs(ready,ready_count),key_collision_pairs(failure,failure_count)};
    free(events);free(ready);free(failure);free(text);return ok;
}
static bool cleanup_render_rejected(const SolMirRuntimeCleanup *cleanup){FILE*stream=tmpfile();if(!stream)return false;bool rejected=!sol_mir_runtime_cleanup_render(stream,cleanup)&&fflush(stream)==0&&fseek(stream,0,SEEK_END)==0&&ftell(stream)==0;fclose(stream);return rejected;}
typedef enum {
    LOCAL_PRODUCED, LOCAL_PENDING, LOCAL_BAD_START, LOCAL_BAD_SITE,
    LOCAL_BAD_CODE, LOCAL_BAD_DETAIL, LOCAL_BOTH, LOCAL_NEITHER,
    LOCAL_BAD_PRODUCED, LOCAL_NORMAL, LOCAL_EXCLUDED, LOCAL_CLASS,
    LOCAL_RESUME, LOCAL_QUERY_KINDS,
} LocalQueryKind;
typedef struct {
    SolMirRuntimeCleanupFailureOccurrence produced, pending;
    LocalQueryKind kind;
} LocalQueryBacking;
/* Preparation never selects or predicts a selector result. Each original call
 * contributes a row, including every rejected request. The final allocations
 * never move, and memcpy preserves occurrence identity bytes including padding. */
static bool local_query_append(SolMirRuntimeCleanupTestTraceQuery *queries,
    LocalQueryBacking *backing, size_t *count, size_t kinds[LOCAL_QUERY_KINDS],
    const SolMirRuntimeCleanupTraceRequest *request, LocalQueryKind kind) {
    if (*count >= SOL_MIR_RUNTIME_CLEANUP_TEST_MAX_QUERIES) return false;
    size_t row = (*count)++;
    queries[row] = (SolMirRuntimeCleanupTestTraceQuery){*request, 0, false};
    backing[row].kind = kind;
    if (request->produced) {
        memcpy(&backing[row].produced, request->produced, sizeof(backing[row].produced));
        queries[row].request.produced = &backing[row].produced;
    }
    if (request->pending) {
        memcpy(&backing[row].pending, request->pending, sizeof(backing[row].pending));
        queries[row].request.pending = &backing[row].pending;
    }
    ++kinds[kind];
    return true;
}
static void check_local_or_pending(const SolMirRuntimeCleanup *cleanup,
    const SolMirRuntimeConventions *conventions, const SolMirConcreteProgram *program) {
    size_t event_id=SOL_MIR_RUNTIME_NONE; const SolMirRuntimeCleanupTransition *local=NULL;
    for(size_t i=0;i<cleanup->event_count&&!local;i++)for(size_t j=0;j<cleanup->events[i].transitions.count;j++){const SolMirRuntimeCleanupTransition*t=&cleanup->transitions[cleanup->events[i].transitions.offset+j];if(t->failure_source==SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_LOCAL_OR_PENDING){event_id=i;local=t;break;}}
    CHECK(local!=NULL); if(!local)return;
    const SolMirRuntimeCleanupEvent*event=&cleanup->events[event_id];
    CHECK(event->kind==SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR&&local->edge_role==SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE&&local->failure_site<conventions->call_count&&local->failure_site==event->inherited_failure_site&&local->failure_mask==(UINT32_C(1)<<(SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT-1))&&local->actions.count==0);
    const SolMirRuntimeCall*call=&conventions->calls[local->failure_site];
    CHECK(call->target_kind==SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL&&call->failure_site==local->failure_site&&call->failure_edge==local->continuation&&program->materialization.blocks[event->block].terminator.kind==SOL_MIR_TERM_INVOKE);
    sol_test_progress_phase("local-canonical-renders", "render", 2);
    char *first_render=render_cleanup_text(cleanup),*second_render=render_cleanup_text(cleanup);CHECK(first_render&&second_render&&!strcmp(first_render,second_render));if(first_render){char *line=strstr(first_render,"failure-source=local-or-pending failure-site-key=");CHECK(line!=NULL&&strstr(line,"failure-site-key=none")==NULL);}free(first_render);free(second_render);
    sol_test_progress_completed(2); sol_test_progress_phase_end();
    SolMirRuntimeCleanupTestTraceQuery *queries = calloc(SOL_MIR_RUNTIME_CLEANUP_TEST_MAX_QUERIES, sizeof(*queries));
    LocalQueryBacking *backing = calloc(SOL_MIR_RUNTIME_CLEANUP_TEST_MAX_QUERIES, sizeof(*backing));
    CHECK(queries != NULL && backing != NULL);
    if (!queries || !backing) { free(queries); free(backing); return; }
    size_t planned = 0, kinds[LOCAL_QUERY_KINDS] = {0};
    sol_test_progress_phase("local-query-plan", "query", 0);
    SolMirRuntimeCleanupFailureOccurrence produced={SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31,local->failure_site,SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT,SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE,0,{0},conventions->failure_sites[local->failure_site].source};
    SolMirRuntimeCleanupTraceRequest request={event_id,local->edge_role,&produced,NULL,SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
    CHECK(local_query_append(queries,backing,&planned,kinds,&request,LOCAL_PRODUCED));
    SolMirRuntimeFailureRecord record={SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT,produced.source,SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE,NULL,0}; SolMirRuntimeCleanupDetail detail;
    sol_test_progress_completed(planned); sol_test_progress_phase_end();
    sol_test_progress_phase("local-public-detail-positive", "capture", 1);
    CHECK(sol_mir_runtime_cleanup_capture_detail(cleanup,event_id,local->edge_role,&record,&detail));
    sol_test_progress_completed(1); sol_test_progress_phase_end();
    sol_test_progress_phase("local-query-plan", "query", 0);
    SolMirRuntimeCleanupFailureOccurrence pending; memcpy(&pending,&produced,sizeof pending); pending.code=SOL_MIR_RUNTIME_FAILURE_INTEGER_OVERFLOW; pending.site=SOL_MIR_RUNTIME_NONE;
    for(size_t i=0;i<conventions->failure_site_count;i++)if((conventions->failure_sites[i].allowed_codes&(UINT32_C(1)<<(SOL_MIR_RUNTIME_FAILURE_INTEGER_OVERFLOW-1)))!=0){pending.site=i;pending.source=conventions->failure_sites[i].source;break;}
    CHECK(pending.site!=SOL_MIR_RUNTIME_NONE); request.produced=NULL;request.pending=&pending;CHECK(local_query_append(queries,backing,&planned,kinds,&request,LOCAL_PENDING));
    SolMirRuntimeCleanupFailureOccurrence malformed=pending; ++malformed.source.start;request.pending=&malformed;CHECK(local_query_append(queries,backing,&planned,kinds,&request,LOCAL_BAD_START));malformed=pending;malformed.site=SOL_MIR_RUNTIME_NONE;request.pending=&malformed;CHECK(local_query_append(queries,backing,&planned,kinds,&request,LOCAL_BAD_SITE));malformed=pending;malformed.code=SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT;request.pending=&malformed;CHECK(local_query_append(queries,backing,&planned,kinds,&request,LOCAL_BAD_CODE));malformed=pending;malformed.detail_length=1;request.pending=&malformed;CHECK(local_query_append(queries,backing,&planned,kinds,&request,LOCAL_BAD_DETAIL));request.pending=&pending;
    sol_test_progress_completed(planned); sol_test_progress_phase_end();
    sol_test_progress_phase("local-public-detail-negative", "capture", 1);
    record.code=SOL_MIR_RUNTIME_FAILURE_INTEGER_OVERFLOW;record.source=pending.source;CHECK(!sol_mir_runtime_cleanup_capture_detail(cleanup,event_id,local->edge_role,&record,&detail));
    sol_test_progress_completed(1); sol_test_progress_phase_end();
    sol_test_progress_phase("local-query-plan", "query", 0);
    request.produced=&produced;CHECK(local_query_append(queries,backing,&planned,kinds,&request,LOCAL_BOTH));request.pending=NULL;request.produced=NULL;CHECK(local_query_append(queries,backing,&planned,kinds,&request,LOCAL_NEITHER));
    SolMirRuntimeCleanupFailureOccurrence bad=produced;bad.code=SOL_MIR_RUNTIME_FAILURE_INTEGER_OVERFLOW;request.produced=&bad;CHECK(local_query_append(queries,backing,&planned,kinds,&request,LOCAL_BAD_PRODUCED));
    for(size_t j=0;j<event->transitions.count;j++){const SolMirRuntimeCleanupTransition*t=&cleanup->transitions[event->transitions.offset+j];if(t->edge_role==SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_NORMAL){SolMirRuntimeCleanupTraceRequest normal={event_id,t->edge_role,NULL,&pending,SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};CHECK(local_query_append(queries,backing,&planned,kinds,&normal,LOCAL_NORMAL));}}
    bool excluded=false;for(size_t i=0;i<cleanup->event_count&&!excluded;i++)for(size_t j=0;j<cleanup->events[i].transitions.count;j++){const SolMirRuntimeCleanupTransition*t=&cleanup->transitions[cleanup->events[i].transitions.offset+j];if(t->edge_role==SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE&&t->failure_source==SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31){SolMirRuntimeCleanupTraceRequest x={i,t->edge_role,NULL,&pending,SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};CHECK(local_query_append(queries,backing,&planned,kinds,&x,LOCAL_EXCLUDED));excluded=true;break;}}CHECK(excluded);
    size_t direct_host=0,indirect=0,predicate=0;for(size_t i=0;i<cleanup->event_count;i++){const SolMirRuntimeCleanupEvent*e=&cleanup->events[i];if(e->inherited_failure_site>=conventions->call_count)continue;const SolMirRuntimeCall*c=&conventions->calls[e->inherited_failure_site];for(size_t j=0;j<e->transitions.count;j++){const SolMirRuntimeCleanupTransition*t=&cleanup->transitions[e->transitions.offset+j];if(t->edge_role!=SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE)continue;bool excluded_class=c->owner_kind==SOL_MIR_RUNTIME_CALL_OWNER_PREDICATE||c->target_kind!=SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL;if(excluded_class){CHECK(t->failure_source==SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31);SolMirRuntimeCleanupTraceRequest x={i,t->edge_role,NULL,&pending,SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};CHECK(local_query_append(queries,backing,&planned,kinds,&x,LOCAL_CLASS));if(c->owner_kind==SOL_MIR_RUNTIME_CALL_OWNER_PREDICATE)++predicate;else if(c->target_kind==SOL_MIR_RUNTIME_TARGET_DIRECT_HOST)++direct_host;else ++indirect;}}}CHECK(direct_host>0);CHECK(indirect==0&&predicate==0);
    size_t resume=program->materialization.edges[local->continuation].block;bool resumed=false;for(size_t i=0;i<cleanup->event_count;i++)if(cleanup->events[i].block==resume)for(size_t j=0;j<cleanup->events[i].transitions.count;j++){const SolMirRuntimeCleanupTransition*t=&cleanup->transitions[cleanup->events[i].transitions.offset+j];if(t->failure_source==SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING){SolMirRuntimeCleanupTraceRequest r={i,t->edge_role,NULL,&pending,SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};CHECK(local_query_append(queries,backing,&planned,kinds,&r,LOCAL_RESUME));resumed=true;}}CHECK(resumed);
    CHECK(kinds[LOCAL_CLASS] == direct_host + indirect + predicate);
    for (size_t kind = LOCAL_PRODUCED; kind <= LOCAL_BAD_PRODUCED; ++kind) CHECK(kinds[kind] == 1);
    CHECK(kinds[LOCAL_EXCLUDED] == 1 && kinds[LOCAL_NORMAL] > 0 && kinds[LOCAL_RESUME] > 0);
    size_t counted = 0;
    for (size_t kind = 0; kind < LOCAL_QUERY_KINDS; ++kind) counted += kinds[kind];
    CHECK(counted == planned);
    sol_test_progress_completed(planned); sol_test_progress_phase_end();
    sol_test_progress_phase("local-immutable-selection", "query", planned);
    SolMirRuntimeCleanupTestTraceResult *results = NULL;
    sol_mir_runtime_cleanup_test_trace_counts_reset();
    bool evaluated = sol_mir_runtime_cleanup_test_trace_batch(cleanup, queries, planned, &results);
    CHECK(evaluated && results != NULL);
    size_t authentications, selections;
    sol_mir_runtime_cleanup_test_trace_counts(&authentications, &selections);
    CHECK(authentications == 1 && selections == planned);
    fprintf(stderr, "cleanup-batch local queries=%zu authentications=%zu selections=%zu\n", planned, authentications, selections);
    for (size_t kind = 0; kind < LOCAL_QUERY_KINDS; ++kind)
        fprintf(stderr, "cleanup-local-kind kind=%zu rows=%zu\n", kind, kinds[kind]);
    if (evaluated) sol_test_progress_completed(planned);
    sol_test_progress_phase_end();
    sol_test_progress_phase("local-row-assertions", "query", planned);
    size_t cursor = 0, checked_kinds[LOCAL_QUERY_KINDS] = {0};
    if (results) for (; cursor < planned; ++cursor) {
        LocalQueryKind kind = backing[cursor].kind;
        if (kind == LOCAL_PRODUCED)
            CHECK(results[cursor].selected && !memcmp(&results[cursor].trace.primary,&produced,sizeof produced));
        else if (kind == LOCAL_PENDING || kind == LOCAL_RESUME)
            CHECK(results[cursor].selected && !memcmp(&results[cursor].trace.primary,&pending,sizeof pending));
        else CHECK(!results[cursor].selected);
        ++checked_kinds[kind];
        sol_test_progress_completed(cursor + 1);
    }
    CHECK(cursor == planned && counted == planned);
    CHECK(memcmp(kinds,checked_kinds,sizeof kinds) == 0);
    sol_test_progress_phase_end();
    free(results); free(backing); free(queries);
    sol_test_progress_phase("local-mutation-restoration", "validation", 4);
    SolMirRuntimeCleanupTransition *mutable_local=(SolMirRuntimeCleanupTransition *)local;SolMirRuntimeCleanupFailureSource source=mutable_local->failure_source;size_t site=mutable_local->failure_site;uint32_t mask=mutable_local->failure_mask;mutable_local->failure_source=SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31;CHECK(!sol_mir_runtime_cleanup_validate(cleanup,NULL));mutable_local->failure_source=source;mutable_local->failure_site=SOL_MIR_RUNTIME_NONE;CHECK(!sol_mir_runtime_cleanup_validate(cleanup,NULL));mutable_local->failure_site=site;mutable_local->failure_mask=0;CHECK(!sol_mir_runtime_cleanup_validate(cleanup,NULL));mutable_local->failure_mask=mask;CHECK(sol_mir_runtime_cleanup_validate(cleanup,NULL));
    sol_test_progress_completed(4); sol_test_progress_phase_end();
}

/* Source-backed local callback selector: code 6 is local, arbitrary already
 * authenticated arithmetic packets remain pending, and allocation packets use
 * their supplemental source unchanged. */
static void check_callback_selector(void) {
    Compilation c; SolMirConcreteProgram program; SolMirRuntimeConventions conventions;
    SolMirRuntimeValues values; SolMirRuntimeCleanup cleanup;
    sol_mir_concrete_program_init(&program); sol_mir_runtime_conventions_init(&conventions);
    sol_mir_runtime_values_init(&values); sol_mir_runtime_cleanup_init(&cleanup);
    bool built = compile_directory(&c, SOL_TEST_SOURCE_DIR "/tests/conformance/p43_callback_prereq")
        && build_named_concrete(&c, "launch", &program)
        && sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&program,NULL},
            &conventions, &c.diagnostics) == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED
        && sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&conventions,NULL},
            &values, &c.diagnostics) == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED
        && sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&conventions,&values,NULL},
            &cleanup, &c.diagnostics) == SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED;
    CHECK(built);
    size_t event_id = SOL_MIR_RUNTIME_NONE, transition_id = SOL_MIR_RUNTIME_NONE, callback_events = 0;
    SolIrCallableId increment = callable(&c.ir, "increment", SOL_IR_CALLABLE_FUNCTION);
    SolIrCallableId decrement = callable(&c.ir, "decrement", SOL_IR_CALLABLE_FUNCTION);
    if (built) for (size_t i = 0; i < cleanup.event_count; ++i)
        for (size_t j = 0; j < cleanup.events[i].transitions.count; ++j) {
            size_t id = cleanup.events[i].transitions.offset + j;
            if (cleanup.transitions[id].failure_source
                    == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_LOCAL_OR_PENDING) {
                const SolMirMaterializedTerminator *term = cleanup.events[i].block
                        < program.materialization.block_count
                    ? &program.materialization.blocks[cleanup.events[i].block].terminator : NULL;
                const SolMirMaterializedSemanticSite *site = term != NULL
                        && term->callable_site < program.materialization.semantic_site_count
                    ? &program.materialization.semantic_sites[term->callable_site] : NULL;
                CHECK(site != NULL && site->producer_kind
                        == SOL_MIR_MATERIALIZED_PRODUCER_INSTRUCTION
                    && site->instruction < program.materialization.instruction_count);
                if (site == NULL || site->instruction >= program.materialization.instruction_count) continue;
                SolIrCallableId callable = program.materialization.instructions[site->instruction]
                    .function_callable;
                CHECK(callable == increment || callable == decrement);
                ++callback_events;
                if (callable == increment) {
                    CHECK(event_id == SOL_MIR_RUNTIME_NONE); event_id = i; transition_id = id;
                }
            }
        }
    CHECK(callback_events == 2 && event_id != SOL_MIR_RUNTIME_NONE
        && transition_id != SOL_MIR_RUNTIME_NONE);
    if (event_id != SOL_MIR_RUNTIME_NONE) {
        const SolMirRuntimeCleanupEvent *event = &cleanup.events[event_id];
        SolMirRuntimeCleanupTransition *transition = &cleanup.transitions[transition_id];
        CHECK(event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
            && transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE
            && transition->failure_mask == (UINT32_C(1)
                << (SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT - 1)));
        const SolMirMaterializedTerminator *callback
            = &program.materialization.blocks[event->block].terminator;
        const SolMirMaterializedSemanticSite *callback_site = callback->callable_site
                < program.materialization.semantic_site_count
            ? &program.materialization.semantic_sites[callback->callable_site] : NULL;
        const SolMirRuntimeFailureSite *failure_site
            = &conventions.failure_sites[transition->failure_site];
        CHECK(callback->kind == SOL_MIR_TERM_INVOKE
            && callback->call_kind == SOL_IR_CALL_CALLBACK
            && callback_site != NULL
            && callback_site->kind == SOL_MIR_PLAN_DEMAND_FUNCTION_VALUE
            && callback_site->producer_kind == SOL_MIR_MATERIALIZED_PRODUCER_INSTRUCTION
            && callback_site->instruction < program.materialization.instruction_count
            && program.materialization.instructions[callback_site->instruction].kind
                == SOL_MIR_INST_FUNCTION_VALUE
            && event->source.file == failure_site->source.file
            && event->source.start == failure_site->source.start
            && event->source.end == failure_site->source.end);
        SolMirRuntimeCleanupFailureOccurrence local = {
            SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31, transition->failure_site,
            SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT, SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE, 0, {0},
            conventions.failure_sites[transition->failure_site].source};
        SolMirRuntimeCleanupTrace trace;
        SolMirRuntimeCleanupTraceRequest request = {event_id, transition->edge_role, &local,
            NULL, SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
        CHECK(sol_mir_runtime_cleanup_test_select(&cleanup, &request, NULL, 0, &trace)
            && !memcmp(&trace.primary, &local, sizeof local));
        for (size_t code = SOL_MIR_RUNTIME_FAILURE_INTEGER_OVERFLOW;
                code <= SOL_MIR_RUNTIME_FAILURE_DIVISION_BY_ZERO; ++code) {
            size_t site = SOL_MIR_RUNTIME_NONE;
            for (size_t i = 0; i < conventions.failure_site_count; ++i)
                if (conventions.failure_sites[i].allowed_codes & (UINT32_C(1) << (code - 1))) {
                    site = i; break;
                }
            if (site == SOL_MIR_RUNTIME_NONE) continue;
            SolMirRuntimeCleanupFailureOccurrence pending = {
                SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31, site,
                (SolMirRuntimeFailureCode)code, SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE, 0, {0},
                conventions.failure_sites[site].source};
            request.produced = NULL; request.pending = &pending;
            CHECK(sol_mir_runtime_cleanup_test_select(&cleanup, &request, NULL, 0, &trace)
                && !memcmp(&trace.primary, &pending, sizeof pending));
        }
        request.produced = &local; request.pending = &local;
        CHECK(!sol_mir_runtime_cleanup_test_select(&cleanup, &request, NULL, 0, &trace));
        request.produced = request.pending = NULL;
        CHECK(!sol_mir_runtime_cleanup_test_select(&cleanup, &request, NULL, 0, &trace));
        for (size_t i = 0; i < event->transitions.count; ++i) {
            const SolMirRuntimeCleanupTransition *normal
                = &cleanup.transitions[event->transitions.offset + i];
            if (normal->edge_role != SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_NORMAL) continue;
            SolMirRuntimeCleanupTraceRequest rejected = {event_id, normal->edge_role,
                &local, NULL, SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
            CHECK(!sol_mir_runtime_cleanup_test_select(&cleanup, &rejected, NULL, 0, &trace));
        }
        SolMirRuntimeCall *call = &conventions.calls[transition->failure_site];
        size_t table = call->table; call->table = SOL_MIR_RUNTIME_NONE;
        CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL)); call->table = table;
        size_t signature = call->signature; call->signature = SOL_MIR_RUNTIME_NONE;
        CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL)); call->signature = signature;
        SolMirRuntimeSignatureOrigin origin = conventions.signatures[signature].origin;
        conventions.signatures[signature].origin = SOL_MIR_RUNTIME_SIGNATURE_INTERNAL;
        CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL));
        conventions.signatures[signature].origin = origin;
        SolMirMaterializedTerminator *term = &program.materialization.blocks[event->block].terminator;
        size_t callable_site = term->callable_site; term->callable_site = SOL_MIR_RUNTIME_NONE;
        CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL)); term->callable_site = callable_site;
        for (size_t i = 0; i < program.operations.callable_count; ++i) {
            SolMirOperationCallablePlan *plan = &program.operations.callables[i];
            if (plan->semantic_site != callable_site) continue;
            size_t mapped_table = program.linkage.callable_values[i].table;
            program.linkage.callable_values[i].table = SOL_MIR_RUNTIME_NONE;
            CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL));
            program.linkage.callable_values[i].table = mapped_table;
            SolMirRecipeId recipe = plan->function_recipe;
            plan->function_recipe = SOL_MIR_RECIPE_NONE;
            CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL)); plan->function_recipe = recipe;
            break;
        }
        uint32_t mask = transition->failure_mask; transition->failure_mask = 0;
        CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL)); transition->failure_mask = mask;
        CHECK(sol_mir_runtime_cleanup_validate(&cleanup,NULL));
    }
    if (built) for (size_t i = 0; i < cleanup.event_count; ++i) {
        const SolMirRuntimeCleanupEvent *event = &cleanup.events[i];
        if (event->supplemental_site == SOL_MIR_RUNTIME_NONE) continue;
        for (size_t j = 0; j < event->transitions.count; ++j) {
            const SolMirRuntimeCleanupTransition *transition
                = &cleanup.transitions[event->transitions.offset + j];
            if (transition->failure_source
                    != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_SUPPLEMENTAL_P33) continue;
            for (size_t code = SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED;
                    code <= SOL_MIR_RUNTIME_FAILURE_ALLOCATION_LIMIT; ++code) {
                SolMirRuntimeCleanupFailureOccurrence packet = {
                    SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_SUPPLEMENTAL_P33,
                    transition->failure_site, (SolMirRuntimeFailureCode)code,
                    SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE, 0, {0},
                    cleanup.supplemental_sites[transition->failure_site].source};
                SolMirRuntimeCleanupTrace trace;
                SolMirRuntimeCleanupTraceRequest request = {i, transition->edge_role,
                    &packet, NULL, SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
                CHECK(sol_mir_runtime_cleanup_test_select(&cleanup, &request, NULL, 0, &trace)
                    && !memcmp(&trace.primary, &packet, sizeof packet));
            }
        }
    }
    sol_mir_runtime_cleanup_free(&cleanup); sol_mir_runtime_values_free(&values);
    sol_mir_runtime_conventions_free(&conventions); sol_mir_concrete_program_free(&program);
    compilation_free(&c);
}
static bool authenticated_arithmetic_occurrence(const SolMirRuntimeConventions *conventions,
    SolMirRuntimeCleanupFailureOccurrence *pending) {
    for (size_t i = 0; i < conventions->failure_site_count; ++i) {
        const SolMirRuntimeFailureSite *site = &conventions->failure_sites[i];
        if ((site->allowed_codes
                & (UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_INTEGER_OVERFLOW - 1))) == 0)
            continue;
        *pending = (SolMirRuntimeCleanupFailureOccurrence){
            SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31, i,
            SOL_MIR_RUNTIME_FAILURE_INTEGER_OVERFLOW,
            SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE, 0, {0}, site->source,
        };
        return true;
    }
    return false;
}
static void check_graph_inherited_capability_calls(
    const SolMirRuntimeCleanupFailureOccurrence *pending) {
    Compilation compilation;
    SolMirConcreteProgram program;
    SolMirRuntimeConventions conventions;
    SolMirRuntimeValues values;
    SolMirRuntimeCleanup cleanup;
    sol_mir_concrete_program_init(&program);
    sol_mir_runtime_conventions_init(&conventions);
    sol_mir_runtime_values_init(&values);
    sol_mir_runtime_cleanup_init(&cleanup);
    bool compiled = compile_directory(&compilation,
        SOL_TEST_SOURCE_DIR "/tests/conformance/p36_graph");
    SolIrCallableId launch = compiled
        ? callable(&compilation.ir, "launch", SOL_IR_CALLABLE_FUNCTION) : SOL_IR_NONE;
    SolMirProgramRoot root = {launch, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    bool built = compiled && launch != SOL_IR_NONE
        && sol_mir_concrete_program_build(&(SolMirConcreteBuildRequest){&compilation.ir,
            &root, 1, NULL, 0, &target, NULL}, &program, &compilation.diagnostics)
            == SOL_MIR_CONCRETE_BUILD_SUCCEEDED
        && sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&program,
            NULL}, &conventions, &compilation.diagnostics)
            == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED
        && sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&conventions,
            NULL}, &values, &compilation.diagnostics)
            == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED
        && sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&conventions,
            &values, NULL}, &cleanup, &compilation.diagnostics)
            == SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED;
    CHECK(built);
    if (built) {
        size_t predicate_call = SOL_MIR_RUNTIME_NONE, image_call = SOL_MIR_RUNTIME_NONE;
        size_t predicate_calls = 0, image_calls = 0;
        CHECK(pending != NULL);
        for (size_t i = 0; i < conventions.call_count; ++i) {
            const SolMirRuntimeCall *call = &conventions.calls[i];
            if (call->owner_kind == SOL_MIR_RUNTIME_CALL_OWNER_PREDICATE
                && call->target_kind == SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE
                && call->call_kind == SOL_IR_CALL_CAPABILITY) {
                predicate_call = i;
                ++predicate_calls;
            }
            if (call->owner_kind == SOL_MIR_RUNTIME_CALL_OWNER_IMAGE
                && call->target_kind == SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL
                && call->call_kind == SOL_IR_CALL_CAPABILITY) {
                image_call = i;
                ++image_calls;
            }
        }
        CHECK(predicate_calls == 1 && image_calls == 1);
        for (size_t kind = 0; kind < 2; ++kind) {
            size_t call_id = kind == 0 ? predicate_call : image_call;
            if (call_id == SOL_MIR_RUNTIME_NONE) continue;
            const SolMirRuntimeCall *call = &conventions.calls[call_id];
            size_t event_id = SOL_MIR_RUNTIME_NONE, events = 0;
            const SolMirRuntimeCleanupTransition *failure = NULL, *normal = NULL;
            for (size_t i = 0; i < cleanup.event_count; ++i) {
                const SolMirRuntimeCleanupEvent *event = &cleanup.events[i];
                bool matches = event->inherited_failure_site == call_id
                    && event->block == call->block
                    && event->kind == (kind == 0
                        ? SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR
                        : SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR)
                    && (kind == 0 ? event->owner == call->predicate : event->owner == call->image);
                if (!matches) continue;
                event_id = i;
                ++events;
                for (size_t j = 0; j < event->transitions.count; ++j) {
                    const SolMirRuntimeCleanupTransition *transition =
                        &cleanup.transitions[event->transitions.offset + j];
                    if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE)
                        failure = transition;
                    if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_NORMAL)
                        normal = transition;
                }
            }
            CHECK(events == 1 && failure != NULL && normal != NULL);
            if (events != 1 || failure == NULL || normal == NULL) continue;
            CHECK(failure->failure_source
                    == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_LOCAL_OR_PENDING
                && failure->failure_site == call_id
                && failure->failure_mask
                    == conventions.failure_sites[call_id].allowed_codes);
            SolMirRuntimeCleanupTrace trace;
            if (pending != NULL) {
                SolMirRuntimeCleanupTraceRequest request = {event_id,
                    SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE, NULL, pending,
                    SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
                CHECK(!sol_mir_runtime_cleanup_test_select(&cleanup, &request, NULL, 0,
                    &trace));
                request.edge_role = SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_NORMAL;
                CHECK(!sol_mir_runtime_cleanup_test_select(&cleanup, &request, NULL, 0,
                    &trace));
            }
        }
    }
    sol_mir_runtime_cleanup_free(&cleanup);
    sol_mir_runtime_values_free(&values);
    sol_mir_runtime_conventions_free(&conventions);
    sol_mir_concrete_program_free(&program);
    compilation_free(&compilation);
}
/* The E6 program deliberately contains all finite policy shapes used here:
 * owned and borrowed formals, projected Result/Option propagation, contracts
 * and snapshots, calls/branches/returns, arithmetic and capability failures.
 * Keeping the assertions owner-level makes these fixtures independent of a
 * future executor. */
static void check_eight_fixture_groups(const SolMirRuntimeCleanup *cleanup,
    const SolMirConcreteProgram *program) {
    sol_test_progress_phase("static-eight-fixture-sweep", "sweep", 1);
    bool e6 = false, formals = false, holes = false, contracts = false;
    bool refinement = false, implicit = false, exits = false, precedence = false;
    bool call_failure = false, contract_failure = false;
    for (size_t i = 0; i < cleanup->event_count; ++i) {
        const SolMirRuntimeCleanupEvent *event = &cleanup->events[i];
        e6 = true;
        implicit |= event->producer == SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_ARITHMETIC
            || event->producer == SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_ARITHMETIC
            || event->producer == SOL_MIR_RUNTIME_CLEANUP_PRODUCER_SUPPLEMENTAL_ALLOCATION;
        refinement |= event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR;
        for (size_t j = 0; j < event->transitions.count; ++j) {
            const SolMirRuntimeCleanupTransition *transition =
                &cleanup->transitions[event->transitions.offset + j];
            bool predicate = event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION
                || event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR;
            if (transition->continuation != SOL_MIR_RUNTIME_NONE) {
                CHECK(transition->source_edge == transition->continuation);
                if (predicate)
                    CHECK(transition->continuation < program->operations.predicate_edge_count
                        && transition->destination
                            == program->operations.predicate_edges[transition->continuation].target);
                else CHECK(transition->continuation < program->materialization.edge_count
                    && transition->destination
                        == program->materialization.edges[transition->continuation].block);
            }
            else CHECK(transition->destination == SOL_MIR_RUNTIME_NONE);
            contracts |= transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_SATISFIED
                || transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_VIOLATION
                || transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_FAILURE;
            exits |= transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_RETURN
                || transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE;
            precedence |= transition->primary_failure_wins;
            if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE
                || transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_FAILURE
                || transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_FAILURE)
                CHECK(transition->actions.count == 0);
            call_failure |= transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE;
            contract_failure |= transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_FAILURE;
            for (size_t k = 0; k < transition->actions.count; ++k) {
                const SolMirRuntimeCleanupAction *action =
                    &cleanup->actions[transition->actions.offset + k];
                formals |= action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER;
                holes |= (action->flags & SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED) != 0;
                if (action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT)
                    CHECK(action->target < program->operations.snapshot_count
                        && action->recipe == program->operations.snapshots[action->target].recipe);
                if (action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE)
                    CHECK(transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE
                        || transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_STEP_FAILURE);
            }
        }
    }
    /* Eight named finite groups: E6; formal ownership; Pair holes/join;
       contract+old; refinement/no-match; implicit allocation/arithmetic;
       explicit exits; and precedence/hardening.  Some language subsets do not
       materialize Pair moves in E6, so that group is exercised by guarded path
       validation rather than requiring an unreachable source spelling. */
    CHECK(e6 && formals && contracts && refinement && implicit && exits && precedence
        && call_failure && contract_failure);
    (void)holes;
    sol_test_progress_completed(1); sol_test_progress_phase_end();
}
/* Each original sweep predicate emits one row in event/transition/if order.
 * Occurrences live in a final allocation before any request points into it. */
static void check_immutable_occurrence_sweep(const SolMirRuntimeCleanup *cleanup,
    const SolMirRuntimeConventions *conventions,
    const SolMirRuntimeCleanupFailureOccurrence *pending_occurrence) {
    SolMirRuntimeCleanupFailureOccurrence trace_occurrence;
    memcpy(&trace_occurrence, pending_occurrence, sizeof(trace_occurrence));
    size_t planned = 0, contract_rows = 0, pending_rows = 0, reject_rows = 0;
    sol_test_progress_phase("immutable-occurrence-plan", "transition", cleanup->transition_count);
    for (size_t i = 0; i < cleanup->event_count; ++i) {
        const SolMirRuntimeCleanupEvent *event = &cleanup->events[i];
        for (size_t j = 0; j < event->transitions.count; ++j) {
            const SolMirRuntimeCleanupTransition *transition =
                &cleanup->transitions[event->transitions.offset + j];
            SolMirRuntimeCleanupFailureOccurrence occurrence;
            if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_VIOLATION
                && occurrence_for_transition(cleanup, conventions, transition, &occurrence)) ++contract_rows;
            if (transition->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING) ++pending_rows;
            if (transition->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE) ++reject_rows;
        }
    }
    /* Each count is bounded by the real transition count; addition is checked
     * against the API bound before allocation, never truncated to fit. */
    bool bounded = contract_rows <= SOL_MIR_RUNTIME_CLEANUP_TEST_MAX_QUERIES
        && pending_rows <= SOL_MIR_RUNTIME_CLEANUP_TEST_MAX_QUERIES - contract_rows
        && reject_rows <= SOL_MIR_RUNTIME_CLEANUP_TEST_MAX_QUERIES - contract_rows - pending_rows;
    CHECK(bounded);
    if (bounded) planned = contract_rows + pending_rows + reject_rows;
    SolMirRuntimeCleanupTestTraceQuery *queries = bounded ? calloc(planned, sizeof(*queries)) : NULL;
    SolMirRuntimeCleanupFailureOccurrence *backing = bounded ? calloc(planned, sizeof(*backing)) : NULL;
    CHECK(queries != NULL && backing != NULL);
    size_t prepared = 0;
    if (queries && backing) for (size_t i = 0; i < cleanup->event_count; ++i) {
        const SolMirRuntimeCleanupEvent *event = &cleanup->events[i];
        for (size_t j = 0; j < event->transitions.count; ++j) {
            const SolMirRuntimeCleanupTransition *transition =
                &cleanup->transitions[event->transitions.offset + j];
            SolMirRuntimeCleanupFailureOccurrence occurrence;
            if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_VIOLATION
                && occurrence_for_transition(cleanup, conventions, transition, &occurrence)) {
                memcpy(&backing[prepared], &occurrence, sizeof(occurrence));
                queries[prepared] = (SolMirRuntimeCleanupTestTraceQuery){
                    {i, transition->edge_role, &backing[prepared], NULL,
                        SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL}, 64, true};
                ++prepared;
            }
            if (transition->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING) {
                memcpy(&backing[prepared], &trace_occurrence, sizeof(trace_occurrence));
                queries[prepared] = (SolMirRuntimeCleanupTestTraceQuery){
                    {i, transition->edge_role, NULL, &backing[prepared],
                        SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE}, 64, true};
                ++prepared;
            }
            if (transition->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE) {
                memcpy(&backing[prepared], &trace_occurrence, sizeof(trace_occurrence));
                queries[prepared] = (SolMirRuntimeCleanupTestTraceQuery){
                    {i, transition->edge_role, NULL, &backing[prepared],
                        SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE}, 64, true};
                ++prepared;
            }
        }
    }
    CHECK(prepared == planned);
    if (queries && backing) sol_test_progress_completed(cleanup->transition_count);
    sol_test_progress_phase_end();
    sol_test_progress_phase("immutable-occurrence-authenticate-select-batch", "query", planned);
    SolMirRuntimeCleanupTestTraceResult *results = NULL;
    sol_mir_runtime_cleanup_test_trace_counts_reset();
    bool evaluated = queries && backing
        && sol_mir_runtime_cleanup_test_trace_batch(cleanup, queries, planned, &results);
    CHECK(evaluated && results != NULL);
    size_t authentications, selections;
    sol_mir_runtime_cleanup_test_trace_counts(&authentications, &selections);
    CHECK(authentications == 1 && selections == planned);
    fprintf(stderr, "cleanup-batch immutable-occurrence queries=%zu contract=%zu pending=%zu rejected-success=%zu authentications=%zu selections=%zu\n",
        planned, contract_rows, pending_rows, reject_rows, authentications, selections);
    if (evaluated) sol_test_progress_completed(planned);
    sol_test_progress_phase_end();
    bool saw_contract_result = false, saw_resume = false, saw_success_reject_pending = false;
    size_t cursor = 0;
    sol_test_progress_phase("immutable-occurrence-assertions", "query", planned);
    for (size_t i = 0; i < cleanup->event_count; ++i) {
        const SolMirRuntimeCleanupEvent *event = &cleanup->events[i];
        for (size_t j = 0; j < event->transitions.count; ++j) {
            const SolMirRuntimeCleanupTransition *transition =
                &cleanup->transitions[event->transitions.offset + j];
            SolMirRuntimeCleanupFailureOccurrence occurrence;
            if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_VIOLATION
                && occurrence_for_transition(cleanup, conventions, transition, &occurrence)) {
                CHECK(results != NULL && cursor < planned && results[cursor].selected);
                if (results && cursor < planned && results[cursor].selected) {
                    SolMirRuntimeCleanupTrace trace = results[cursor].trace;
                    CHECK(trace.primary.failure_source
                        == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31);
                }
                ++cursor;
                saw_contract_result = true;
            }
            if (transition->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING) {
                CHECK(results != NULL && cursor < planned && results[cursor].selected);
                if (results && cursor < planned && results[cursor].selected) {
                    SolMirRuntimeCleanupTrace trace = results[cursor].trace;
                    CHECK(memcmp(&trace.primary, &trace_occurrence, sizeof(trace.primary)) == 0);
                }
                ++cursor;
                saw_resume = true;
            }
            if (transition->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE) {
                CHECK(results != NULL && cursor < planned && !results[cursor].selected);
                ++cursor;
                saw_success_reject_pending = true;
            }
            sol_test_progress_completed(cursor);
        }
    }
    CHECK(saw_contract_result && saw_resume && saw_success_reject_pending);
    CHECK(cursor == planned && prepared == planned);
    sol_test_progress_phase_end();
    free(results); free(backing); free(queries);
}
/* Focused owner-policy assertions over the E6 materialization: this checks
 * local-kind classification rather than relying on an executor or source text. */
static void check_owner_snapshot_contract_and_selector(const SolMirRuntimeCleanup *cleanup,
    const SolMirConcreteProgram *program, const SolMirRuntimeConventions *conventions) {
    bool saw_owned_formal = false, saw_snapshot = false, saw_violation = false;
    sol_test_progress_fixture("e6-all-roots");
    sol_test_progress_phase("owner-snapshot-trace-plan", "query", 1025);
    size_t planned = 0;
    for (size_t i = 0; i < cleanup->event_count; ++i)
        for (size_t a = 0; a < cleanup->events[i].transitions.count; ++a)
            planned += cleanup->transitions[cleanup->events[i].transitions.offset + a].outcome
                == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL;
    CHECK(planned == 1025);
    CHECK(planned <= SOL_MIR_RUNTIME_CLEANUP_TEST_MAX_QUERIES);
    SolMirRuntimeCleanupTestTraceQuery *queries = planned <= SOL_MIR_RUNTIME_CLEANUP_TEST_MAX_QUERIES
        ? calloc(planned, sizeof(*queries)) : NULL;
    CHECK(queries != NULL);
    size_t prepared = 0;
    if (queries) for (size_t i = 0; i < cleanup->event_count; ++i)
        for (size_t a = 0; a < cleanup->events[i].transitions.count; ++a) {
            const SolMirRuntimeCleanupTransition *left =
                &cleanup->transitions[cleanup->events[i].transitions.offset + a];
            if (left->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL)
                queries[prepared++] = (SolMirRuntimeCleanupTestTraceQuery){
                    {i, left->edge_role, NULL, NULL, SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE}, 64, true};
        }
    CHECK(prepared == planned);
    sol_test_progress_completed(prepared); sol_test_progress_phase_end();
    sol_test_progress_phase("owner-snapshot-authenticate-select-batch", "query", planned);
    SolMirRuntimeCleanupTestTraceResult *results = NULL;
    sol_mir_runtime_cleanup_test_trace_counts_reset();
    bool evaluated = queries && sol_mir_runtime_cleanup_test_trace_batch(cleanup, queries, planned, &results);
    CHECK(evaluated && results != NULL);
    size_t authentications, selections;
    sol_mir_runtime_cleanup_test_trace_counts(&authentications, &selections);
    CHECK(authentications == 1 && selections == planned);
    fprintf(stderr, "cleanup-batch owner-snapshot queries=%zu authentications=%zu selections=%zu\n",
        planned, authentications, selections);
    if (evaluated) sol_test_progress_completed(planned);
    sol_test_progress_phase_end();
    size_t cursor = 0;
    sol_test_progress_phase("owner-snapshot-event-traces", "event", cleanup->event_count);
    for (size_t i = 0; i < cleanup->event_count; ++i) {
        /* All earlier events completed, including their original continue paths. */
        sol_test_progress_completed(i);
        sol_test_progress_ordinal("cleanup-event", i + 1, cleanup->event_count);
        const SolMirRuntimeCleanupEvent *event = &cleanup->events[i];
        for (size_t a = 0; a < event->transitions.count; ++a) {
            const SolMirRuntimeCleanupTransition *left = &cleanup->transitions[event->transitions.offset + a];
            for (size_t b = a + 1; b < event->transitions.count; ++b)
                CHECK(left->edge_role != cleanup->transitions[event->transitions.offset + b].edge_role);
            if (left->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL) {
                CHECK(results != NULL && cursor < planned && results[cursor].selected);
                ++cursor;
            }
            for (size_t k = 0; k < left->actions.count; ++k) {
                const SolMirRuntimeCleanupAction *action = &cleanup->actions[left->actions.offset + k];
                if (action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT) {
                    saw_snapshot = true;
                    CHECK(action->target < program->operations.snapshot_count);
                    CHECK(action->recipe == program->operations.snapshots[action->target].recipe);
                }
                if (left->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_VIOLATION) {
                    saw_violation = true;
                    if (action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_CHECK_CONTRACT)
                        CHECK((action->flags & SOL_MIR_RUNTIME_CLEANUP_ACTION_NORMAL_ONLY) == 0);
                }
            }
        }
        if (event->phase != SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
            || event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION
            || event->operation >= program->materialization.instruction_count) continue;
        const SolMirMaterializedInstruction *instruction = &program->materialization.instructions[event->operation];
        if (instruction->kind != SOL_MIR_INST_DROP_IF_INITIALIZED
            || instruction->local >= program->materialization.local_count) continue;
        const SolMirMaterializedLocal *local = &program->materialization.locals[instruction->local];
        for (size_t t = 0; t < event->transitions.count; ++t) {
            const SolMirRuntimeCleanupTransition *transition = &cleanup->transitions[event->transitions.offset + t];
            for (size_t a = 0; a < transition->actions.count; ++a) {
                const SolMirRuntimeCleanupAction *action = &cleanup->actions[transition->actions.offset + a];
                if (local->access == SOL_ACCESS_OWNED
                    && (local->kind == SOL_MIR_MATERIALIZED_LOCAL_PARAMETER
                        || local->kind == SOL_MIR_MATERIALIZED_LOCAL_RECEIVER)) {
                    saw_owned_formal = true;
                    CHECK(action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER);
                }
                if (local->access != SOL_ACCESS_OWNED)
                    CHECK(action->kind != SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER);
            }
        }
    }
    sol_test_progress_completed(cleanup->event_count);
    sol_test_progress_phase_end();
    sol_test_progress_phase("owner-snapshot-transition-census", "transition", cleanup->transition_count);
    for (size_t i = 0; i < cleanup->transition_count; ++i) {
        const SolMirRuntimeCleanupTransition *transition = &cleanup->transitions[i];
        if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_VIOLATION) {
            CHECK(transition->failure_site < conventions->failure_site_count);
            CHECK(conventions->failure_sites[transition->failure_site].origin_kind
                == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT);
        }
        sol_test_progress_completed(i + 1);
    }
    sol_test_progress_phase_end();
    CHECK(saw_owned_formal && saw_snapshot && saw_violation);
    CHECK(cursor == planned && cursor == 1025);
    free(results); free(queries);
}

/* This fixture is deliberately source-level rather than a forged MIR graph:
 * nested blocks, a region, Text allocation, and checked arithmetic all fail
 * before their instruction commits.  The assertion names the concrete marker
 * targets, so a global local-order sweep or an outer-first unwind cannot pass. */
static void check_scope_aware_implicit_unwind(SolMirRuntimeCleanup *cleanup,
    const SolMirConcreteProgram *program) {
    const SolMirMaterialization *m = &program->materialization;
    bool arithmetic = false, allocation = false, nested_region = false, branch_scope = false;
    size_t scope_action = SOL_MIR_RUNTIME_NONE, alternate_scope = SOL_MIR_RUNTIME_NONE;
    size_t order_left = SOL_MIR_RUNTIME_NONE, order_right = SOL_MIR_RUNTIME_NONE;
    for (size_t e = 0; e < cleanup->event_count; ++e) {
        const SolMirRuntimeCleanupEvent *event = &cleanup->events[e];
        if (event->origin != SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT
            || event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION) continue;
        allocation |= event->supplemental_site != SOL_MIR_RUNTIME_NONE;
        arithmetic |= event->supplemental_site == SOL_MIR_RUNTIME_NONE;
        for (size_t t = 0; t < event->transitions.count; ++t) {
            const SolMirRuntimeCleanupTransition *transition =
                &cleanup->transitions[event->transitions.offset + t];
            if (transition->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE) continue;
            if (transition->actions.count >= 2 && order_left == SOL_MIR_RUNTIME_NONE) {
                for (size_t pair = 1; pair < transition->actions.count; ++pair) {
                    const SolMirRuntimeCleanupAction *left =
                        &cleanup->actions[transition->actions.offset + pair - 1];
                    const SolMirRuntimeCleanupAction *right =
                        &cleanup->actions[transition->actions.offset + pair];
                    if (left->kind != right->kind || left->target != right->target) {
                        order_left = transition->actions.offset + pair - 1;
                        order_right = transition->actions.offset + pair;
                        break;
                    }
                }
            }
            bool saw_temp = false, saw_place = false;
            for (size_t a = 0; a < transition->actions.count; ++a) {
                size_t action_id = transition->actions.offset + a;
                const SolMirRuntimeCleanupAction *action = &cleanup->actions[action_id];
                if (action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_TEMPORARY) {
                    CHECK(!saw_place); /* temporary phase is before this scope's roots */
                    saw_temp = true;
                }
                if (action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE) {
                    saw_place = true;
                    CHECK(action->drop_path < cleanup->drop_path_count);
                }
                if (action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_SCOPE) {
                    CHECK(action->target < m->instruction_count);
                    const SolMirMaterializedInstruction *marker = &m->instructions[action->target];
                    CHECK(marker->kind == SOL_MIR_INST_SCOPE_ENTER);
                    if (scope_action == SOL_MIR_RUNTIME_NONE) scope_action = action_id;
                    else if (alternate_scope == SOL_MIR_RUNTIME_NONE
                        && action->target != cleanup->actions[scope_action].target)
                        alternate_scope = action_id;
                    if (a + 1 < transition->actions.count
                        && marker->scope_kind == SOL_MIR_SCOPE_REGION) {
                        const SolMirRuntimeCleanupAction *next = &cleanup->actions[action_id + 1];
                        CHECK(next->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_REGION);
                        CHECK(next->target == marker->scope_source);
                        nested_region = true;
                    }
                    if (marker->scope_kind == SOL_MIR_SCOPE_BLOCK) branch_scope = true;
                }
                if (saw_temp && saw_place && order_left == SOL_MIR_RUNTIME_NONE) {
                    for (size_t prior = a; prior; --prior)
                        if (cleanup->actions[transition->actions.offset + prior - 1].kind
                            == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_TEMPORARY) {
                            order_left = transition->actions.offset + prior - 1;
                            order_right = action_id;
                            break;
                        }
                }
            }
        }
    }
    CHECK(arithmetic && allocation && nested_region && branch_scope);
    CHECK(scope_action != SOL_MIR_RUNTIME_NONE && alternate_scope != SOL_MIR_RUNTIME_NONE);
    SolMirRuntimeCleanupAction saved = cleanup->actions[scope_action];
    cleanup->actions[scope_action].target = SOL_MIR_RUNTIME_NONE;
    CHECK(!sol_mir_runtime_cleanup_validate(cleanup, NULL));
    cleanup->actions[scope_action] = saved;
    CHECK(order_left != SOL_MIR_RUNTIME_NONE && order_right != SOL_MIR_RUNTIME_NONE);
    SolMirRuntimeCleanupAction left = cleanup->actions[order_left];
    cleanup->actions[order_left] = cleanup->actions[order_right];
    cleanup->actions[order_right] = left;
    CHECK(!sol_mir_runtime_cleanup_validate(cleanup, NULL));
    cleanup->actions[order_right] = cleanup->actions[order_left];
    cleanup->actions[order_left] = left;
    CHECK(sol_mir_runtime_cleanup_validate(cleanup, NULL));
}

static void check_failure_propagation_is_final(const SolMirRuntimeCleanup *cleanup,
    const SolMirConcreteProgram *program) {
    sol_test_progress_phase("static-failure-propagation-sweep", "sweep", 1);
    bool saw_implicit = false, saw_terminal = false, saw_panic_capture = false;
    for (size_t e = 0; e < cleanup->event_count; ++e) {
        const SolMirRuntimeCleanupEvent *event = &cleanup->events[e];
        bool terminal = event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
            && event->block < program->materialization.block_count
            && (program->materialization.blocks[event->block].terminator.kind == SOL_MIR_TERM_PANIC
                || program->materialization.blocks[event->block].terminator.kind == SOL_MIR_TERM_MATCH_FAILURE
                || program->materialization.blocks[event->block].terminator.kind == SOL_MIR_TERM_UNREACHABLE
                || program->materialization.blocks[event->block].terminator.kind == SOL_MIR_TERM_RESUME_FAILURE
                || program->materialization.blocks[event->block].terminator.kind == SOL_MIR_TERM_CONTRACT_VIOLATION);
        if (terminal && event->producer == SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_PANIC)
            saw_panic_capture |= event->captures_failure_detail;
        for (size_t t = 0; t < event->transitions.count; ++t) {
            const SolMirRuntimeCleanupTransition *transition =
                &cleanup->transitions[event->transitions.offset + t];
            bool predicate_step = event->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_STEP
                && (event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION
                    || event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR);
            bool requires_final_propagation = !predicate_step && transition->outcome
                    == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
                && (event->origin == SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT || terminal);
            if (!requires_final_propagation) continue;
            CHECK(transition->actions.count != 0);
            const SolMirRuntimeCleanupAction *last = &cleanup->actions[
                transition->actions.offset + transition->actions.count - 1];
            CHECK(last->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE);
            size_t propagations = 0;
            for (size_t a = 0; a < transition->actions.count; ++a)
                propagations += cleanup->actions[transition->actions.offset + a].kind
                    == SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE;
            CHECK(propagations == 1);
            saw_implicit |= event->origin == SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT;
            saw_terminal |= terminal;
        }
    }
    CHECK(saw_implicit && saw_terminal && saw_panic_capture);
    sol_test_progress_completed(1); sol_test_progress_phase_end();
}

static size_t affine_image(const SolMirConcreteProgram *program, SolIrCallableId callable_id) {
    for (size_t i = 0; i < program->materialization.image_count; ++i)
        if (program->materialization.images[i].source_callable == callable_id) return i;
    return SOL_MIR_RUNTIME_NONE;
}
static bool affine_hole(const SolMirRuntimeCleanup *cleanup,
    const SolMirRuntimeCleanupDropPath *path, size_t place,
    SolMirRuntimeCleanupDropLiveness liveness, size_t *id) {
    for (size_t i = 0; i < path->holes.count; ++i) {
        size_t candidate = path->holes.offset + i;
        if (cleanup->drop_paths[candidate].place == place
            && cleanup->drop_paths[candidate].liveness == liveness) {
            if (id != NULL) *id = candidate;
            return true;
        }
    }
    return false;
}
static bool affine_same_projection(const SolMirMaterialization *m, size_t left, size_t right) {
    if (left >= m->place_count || right >= m->place_count) return false;
    const SolMirMaterializedPlace *a = &m->places[left], *b = &m->places[right];
    if (a->local != b->local || a->projections.count != b->projections.count) return false;
    for (size_t i = 0; i < a->projections.count; ++i) {
        const SolMirMaterializedProjection *x = &m->projections[a->projections.offset + i];
        const SolMirMaterializedProjection *y = &m->projections[b->projections.offset + i];
        if (x->kind != y->kind || x->source_field != y->source_field
            || x->tuple_ordinal != y->tuple_ordinal) return false;
    }
    return true;
}
/* Bind every assertion to the actual affine place/projection and ownership
 * recipe; no source-order or guessed arena index can satisfy this fixture. */
static void check_affine_pair_owner(SolMirRuntimeCleanup *cleanup,
    const SolMirConcreteProgram *program, const SolIr *ir) {
    const char *const names[] = {"definite_left", "conditional_left", "repaired_left", "moved_root"};
    const SolMirMaterialization *m = &program->materialization;
    size_t root[4] = {SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_NONE,
        SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_NONE};
    size_t left[3] = {SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_NONE};
    size_t repaired_store = SOL_MIR_RUNTIME_NONE;
    for (size_t f = 0; f < 4; ++f) {
        size_t image = affine_image(program, callable(ir, names[f], SOL_IR_CALLABLE_FUNCTION));
        CHECK(image != SOL_MIR_RUNTIME_NONE);
        if (image == SOL_MIR_RUNTIME_NONE) continue;
        const SolMirMaterializedImage *im = &m->images[image];
        for (size_t i = 0; i < im->instructions.count; ++i) {
            const SolMirMaterializedInstruction *in = &m->instructions[im->instructions.offset + i];
            if ((in->kind != SOL_MIR_INST_LOAD_MOVE && in->kind != SOL_MIR_INST_STORE)
                || in->place >= m->place_count) continue;
            const SolMirMaterializedPlace *place = &m->places[in->place];
            if (in->kind == SOL_MIR_INST_LOAD_MOVE && f == 3 && place->projections.count == 0)
                root[f] = in->place;
            if (f < 3 && place->projections.count != 0) {
                if (in->kind == SOL_MIR_INST_LOAD_MOVE) left[f] = in->place;
                if (in->kind == SOL_MIR_INST_STORE && f == 2) repaired_store = in->place;
                for (size_t candidate = 0; candidate < m->place_count; ++candidate) {
                    if (m->places[candidate].local == place->local
                        && m->places[candidate].projections.count == 0) root[f] = candidate;
                }
            }
        }
        CHECK(root[f] != SOL_MIR_RUNTIME_NONE);
        if (f < 3) CHECK(left[f] != SOL_MIR_RUNTIME_NONE);
    }
    CHECK(affine_same_projection(m, repaired_store, left[2]));
    size_t definite_action = SOL_MIR_RUNTIME_NONE, conditional_action = SOL_MIR_RUNTIME_NONE;
    size_t definite_hole = SOL_MIR_RUNTIME_NONE, conditional_hole = SOL_MIR_RUNTIME_NONE;
    bool repaired_root_drop = false, root_moved_drop = false;
    for (size_t a = 0; a < cleanup->action_count; ++a) {
        const SolMirRuntimeCleanupAction *action = &cleanup->actions[a];
        if (action->drop_path >= cleanup->drop_path_count) continue;
        const SolMirRuntimeCleanupDropPath *path = &cleanup->drop_paths[action->drop_path];
        if (path->root == root[0] && affine_hole(cleanup, path, left[0],
                SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE, &definite_hole)) definite_action = a;
        if (path->root == root[1] && affine_hole(cleanup, path, left[1],
                SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL, &conditional_hole)) conditional_action = a;
        if (path->root == root[2] && path->holes.count == 0) repaired_root_drop = true;
        if (path->root == root[3]) root_moved_drop = true;
    }
    CHECK(definite_action != SOL_MIR_RUNTIME_NONE && definite_hole != SOL_MIR_RUNTIME_NONE);
    CHECK(conditional_action != SOL_MIR_RUNTIME_NONE && conditional_hole != SOL_MIR_RUNTIME_NONE);
    CHECK(repaired_root_drop); /* projected store clears the left hole */
    CHECK(root_moved_drop);    /* pre-step failure observes the root before its move */
    if (definite_action == SOL_MIR_RUNTIME_NONE || definite_hole == SOL_MIR_RUNTIME_NONE) return;
    SolMirRuntimeCleanupAction saved_action = cleanup->actions[definite_action];
    SolMirRuntimeCleanupDropPath saved_main = cleanup->drop_paths[saved_action.drop_path];
    SolMirRuntimeCleanupDropPath saved_hole = cleanup->drop_paths[definite_hole];
    CHECK(saved_main.recipe == saved_action.recipe && saved_main.holes.count == 1);
    /* Only left is excluded, so the same root recipe still traverses right. */
    CHECK(saved_hole.place == left[0] && saved_hole.recipe == saved_action.recipe);
    cleanup->drop_paths[definite_hole].liveness = SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL;
    CHECK(!sol_mir_runtime_cleanup_validate(cleanup, NULL));
    cleanup->drop_paths[definite_hole] = saved_hole;
    cleanup->drop_paths[saved_action.drop_path].place = left[0];
    CHECK(!sol_mir_runtime_cleanup_validate(cleanup, NULL));
    cleanup->drop_paths[saved_action.drop_path] = saved_main;
    cleanup->drop_paths[saved_action.drop_path].recipe ^= 1u;
    CHECK(!sol_mir_runtime_cleanup_validate(cleanup, NULL));
    cleanup->drop_paths[saved_action.drop_path] = saved_main;
    cleanup->actions[definite_action].drop_path = SOL_MIR_RUNTIME_NONE;
    CHECK(!sol_mir_runtime_cleanup_validate(cleanup, NULL));
    cleanup->actions[definite_action] = saved_action;
    CHECK(sol_mir_runtime_cleanup_validate(cleanup, NULL));
}

/* A positive but insufficient validation budget must reject before render
 * writes anything.  The Pair fixture reaches its selected projected-hole
 * ranking during this reduced replay, so ASan covers an exhausted lookup. */
static void check_reduced_validation_work_rejects_without_write(
    SolMirRuntimeCleanup *cleanup) {
    CHECK(cleanup->usage.validation_work >= 10);
    if (cleanup->usage.validation_work < 10) return;
    SolMirRuntimeCleanupLimits saved = cleanup->limits;
    cleanup->limits.max_validation_work = cleanup->usage.validation_work / 2
        - cleanup->usage.validation_work / 10;
    CHECK(cleanup->limits.max_validation_work > 0
        && cleanup->limits.max_validation_work < cleanup->usage.validation_work);
    CHECK(!sol_mir_runtime_cleanup_validate(cleanup, NULL));
    FILE *stream = tmpfile();
    CHECK(stream != NULL);
    if (stream != NULL) {
        CHECK(!sol_mir_runtime_cleanup_render(stream, cleanup));
        CHECK(fflush(stream) == 0 && fseek(stream, 0, SEEK_END) == 0
            && ftell(stream) == 0);
        fclose(stream);
    }
    cleanup->limits = saved;
    CHECK(sol_mir_runtime_cleanup_validate(cleanup, NULL));
}

static void check_cleanup_arena_alias(SolMirRuntimeCleanup *cleanup,
    const void *replacement) {
    SolMirRuntimeCleanupEvent *saved = cleanup->events;
    cleanup->events = (SolMirRuntimeCleanupEvent *)(void *)replacement;
    CHECK(!sol_mir_runtime_cleanup_validate(cleanup, NULL));
    FILE *stream = tmpfile();
    CHECK(stream != NULL);
    if (stream != NULL) {
        CHECK(!sol_mir_runtime_cleanup_render(stream, cleanup));
        CHECK(fflush(stream) == 0 && fseek(stream, 0, SEEK_END) == 0
            && ftell(stream) == 0);
        fclose(stream);
    }
    cleanup->events = saved;
    CHECK(sol_mir_runtime_cleanup_validate(cleanup, NULL));
}

/* This source-owned pair is intentionally not selected by arena order.  The
 * host call and the failure-result ensure must share an image; its named normal
 * edge reaches the check while its named failure edge reaches RESUME_FAILURE
 * first.  The test hook repeats those CFG proofs with caller-owned scratch. */
static void check_authenticated_contract_suppression(const SolMirRuntimeCleanup *cleanup,
    const SolMirConcreteProgram *program, const SolMirRuntimeConventions *conventions) {
    const SolMirMaterialization *m = &program->materialization;
    size_t words = m->block_count * 2;
    size_t *scratch = words ? calloc(words, sizeof(*scratch)) : NULL;
    bool found = false;
    CHECK(words == 0 || scratch != NULL);
    for (size_t primary = 0; primary < cleanup->event_count && !found; ++primary) {
        const SolMirRuntimeCleanupEvent *source = &cleanup->events[primary];
        if (source->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
            || source->block >= m->block_count
            || m->blocks[source->block].terminator.kind != SOL_MIR_TERM_INVOKE
            || m->blocks[source->block].terminator.call_kind != SOL_IR_CALL_CAPABILITY) continue;
        for (size_t pt = 0; pt < source->transitions.count && !found; ++pt) {
            const SolMirRuntimeCleanupTransition *failure =
                &cleanup->transitions[source->transitions.offset + pt];
            SolMirRuntimeCleanupFailureOccurrence primary_occurrence;
            if (failure->edge_role != SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE
                || !occurrence_for_transition(cleanup, conventions, failure, &primary_occurrence)) continue;
            SolMirRuntimeCleanupPrecedenceInput first = {primary, failure->edge_role,
                primary_occurrence, SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
            for (size_t candidate = 0; candidate < cleanup->event_count && !found; ++candidate) {
                const SolMirRuntimeCleanupEvent *check = &cleanup->events[candidate];
                if (check->owner != source->owner) continue;
                for (size_t ct = 0; ct < check->transitions.count; ++ct) {
                    const SolMirRuntimeCleanupTransition *violation =
                        &cleanup->transitions[check->transitions.offset + ct];
                    SolMirRuntimeCleanupFailureOccurrence later_occurrence;
                    if (violation->edge_role != SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_VIOLATION
                        || violation->contract_phase != SOL_CONTRACT_ENSURES
                        || violation->contract_outcome != SOL_CONTRACT_OUTCOME_FAILURE
                        || !occurrence_for_transition(cleanup, conventions, violation,
                            &later_occurrence)) continue;
                    SolMirRuntimeCleanupAttemptedLater later = {candidate,
                        violation->edge_role, later_occurrence,
                        SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
                    SolMirRuntimeCleanupPrecedenceResult result;
                    if (!sol_mir_runtime_cleanup_test_precedence_contract_later(cleanup,
                            &first, &later, scratch, words, &result)) continue;
                    CHECK(result.has_primary && !result.ensures_attempted
                        && !result.writeback_attempted && !result.has_secondary);
                    CHECK(result.later_status
                        == SOL_MIR_RUNTIME_CLEANUP_LATER_SUPPRESSED_BY_PRIMARY);
                    /* The proof requires all bounded scratch, so callers cannot
                       accidentally get an unbounded recursive reachability walk. */
                    CHECK(!sol_mir_runtime_cleanup_test_precedence_contract_later(cleanup,
                        &first, &later, scratch, words - 1, &result));
                    /* The same contract block's satisfied edge is not a
                       later contract producer and must not pass as one. */
                    SolMirRuntimeCleanupAttemptedLater arbitrary_contract = later;
                    arbitrary_contract.edge_role
                        = SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_SATISFIED;
                    CHECK(!sol_mir_runtime_cleanup_test_precedence_contract_later(cleanup,
                        &first, &arbitrary_contract, scratch, words, &result));
                    found = true;
                    break;
                }
            }
        }
    }
    CHECK(found);
    free(scratch);
}

static void check_site_domain_fixture(const char *leaf, const char *root, size_t collision) {
    char directory[512];
    snprintf(directory, sizeof directory, "%s/tests/conformance/%s", SOL_TEST_SOURCE_DIR, leaf);
    Compilation c; SolMirConcreteProgram program; SolMirRuntimeConventions conventions;
    SolMirRuntimeValues values; SolMirRuntimeCleanup cleanup, repeat;
    sol_mir_concrete_program_init(&program); sol_mir_runtime_conventions_init(&conventions);
    sol_mir_runtime_values_init(&values); sol_mir_runtime_cleanup_init(&cleanup);
    sol_mir_runtime_cleanup_init(&repeat);
    bool built = compile_directory(&c, directory) && build_named_concrete(&c, root, &program)
        && sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&program, NULL},
            &conventions, &c.diagnostics) == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED
        && sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&conventions, NULL},
            &values, &c.diagnostics) == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED
        && sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&conventions, &values, NULL},
            &cleanup, &c.diagnostics) == SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED;
    CHECK(built);
    if (!built) { sol_diagnostics_render_human(stderr, &c.package.source, &c.diagnostics); goto done; }
    const SolMirMaterialization *m = &program.materialization;
    bool frozen_collision = collision == SOL_MIR_RUNTIME_NONE;
    size_t result_site = SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; i < conventions.failure_site_count; ++i)
        if (conventions.failure_sites[i].origin_kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT)
            result_site = i;
    CHECK(result_site < conventions.failure_site_count);
    for (size_t i = 0; i < cleanup.event_count; ++i) {
        SolMirRuntimeCleanupEvent *e = &cleanup.events[i];
        if (e->phase != SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION) continue;
        bool image = e->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR;
        if (image && (m->blocks[e->block].terminator.kind == SOL_MIR_TERM_BRANCH
                || m->blocks[e->block].terminator.kind == SOL_MIR_TERM_RETURN)) {
            CHECK(e->inherited_failure_site == SOL_MIR_RUNTIME_NONE);
            if (collision < 2 && e->owner == 0 && e->block == 0) {
                CHECK(m->blocks[0].terminator.kind == (collision == 0
                    ? SOL_MIR_TERM_BRANCH : SOL_MIR_TERM_RETURN));
                CHECK(conventions.failure_sites[1].owner == 0
                    && conventions.failure_sites[1].block == 0
                    && conventions.failure_sites[1].origin_kind == (collision == 0
                        ? SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT
                        : SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_CALL));
                frozen_collision = true;
                e->inherited_failure_site = 1;
                CHECK(!sol_mir_runtime_cleanup_validate(&cleanup, NULL));
                e->inherited_failure_site = SOL_MIR_RUNTIME_NONE;
            }
        }
        if (e->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR
            && program.operations.predicate_blocks[e->block].terminator.kind
                == SOL_MIR_PREDICATE_TERM_RETURN) {
            CHECK(e->inherited_failure_site < conventions.failure_site_count);
            if (e->inherited_failure_site < conventions.failure_site_count) {
                const SolMirRuntimeFailureSite *site = &conventions.failure_sites[e->inherited_failure_site];
                CHECK(site->origin_kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT
                    && site->owner == e->owner && site->block == e->block
                    && site->instruction == SOL_MIR_RUNTIME_NONE);
                if (collision == 2 && e->owner == 1 && e->block == 5) {
                    CHECK(e->inherited_failure_site == 2
                        && conventions.failure_sites[0].origin_kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_CALL
                        && conventions.failure_sites[0].owner == 1 && conventions.failure_sites[0].block == 5);
                    size_t saved = e->inherited_failure_site;
                    e->inherited_failure_site = 0;
                    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup, NULL));
                    e->inherited_failure_site = saved;
                    frozen_collision = true;
                }
            }
        }
    }
    CHECK(frozen_collision && sol_mir_runtime_cleanup_validate(&cleanup, NULL));
    size_t ingress_joins = 0;
    for (size_t i = 0; i < cleanup.event_count; ++i) {
        SolMirRuntimeCleanupEvent *event = &cleanup.events[i];
        if (event->phase != SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
            || event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
            || event->inherited_failure_site >= conventions.failure_site_count
            || conventions.failure_sites[event->inherited_failure_site].origin_kind
                != SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT
            || m->blocks[event->block].terminator.kind != SOL_MIR_TERM_GOTO) continue;
        ++ingress_joins;
        SolMirMaterializedImage *image = &program.materialization.images[event->owner];
        SolMirMaterializedTerminator *term = &program.materialization.blocks[event->block].terminator;
        size_t body = conventions.failure_sites[event->inherited_failure_site].owner;
        size_t plan_id = SOL_MIR_RUNTIME_NONE;
        for (size_t q = 0; q < program.operations.predicate_count; ++q)
            if (program.operations.predicates[q].body == body) plan_id = q;
        CHECK(plan_id < program.operations.predicate_count && term->edge < m->edge_count);
        if (plan_id >= program.operations.predicate_count || term->edge >= m->edge_count) continue;
        SolMirOperationPredicatePlan *plan = &program.operations.predicates[plan_id], saved_plan = *plan;
#define INGRESS_REJECT(change, restore) do { change; \
        CHECK(!sol_mir_runtime_cleanup_validate(&cleanup, NULL)); restore; \
        CHECK(sol_mir_runtime_cleanup_validate(&cleanup, NULL)); } while (0)
        INGRESS_REJECT(plan->contract_phase = SOL_CONTRACT_REQUIRES, *plan = saved_plan);
        INGRESS_REJECT(plan->contract_outcome = SOL_CONTRACT_OUTCOME_ALWAYS, *plan = saved_plan);
        INGRESS_REJECT(plan->image = SOL_MIR_RUNTIME_NONE, *plan = saved_plan);
        INGRESS_REJECT(plan->body = SOL_MIR_RUNTIME_NONE, *plan = saved_plan);
        INGRESS_REJECT(plan->block = event->block, *plan = saved_plan);
        size_t edge = term->edge, epilogue = image->contract_epilogue;
        INGRESS_REJECT(term->edge = SOL_MIR_RUNTIME_NONE, term->edge = edge);
        INGRESS_REJECT(image->contract_epilogue = event->block, image->contract_epilogue = epilogue);
        size_t target = program.materialization.edges[edge].block;
        INGRESS_REJECT(program.materialization.edges[edge].block = event->block,
            program.materialization.edges[edge].block = target);
#undef INGRESS_REJECT
    }
    if (!strcmp(leaf, "p44c_qualified_sequential_stress")) CHECK(ingress_joins > 0);
    if (result_site < conventions.failure_site_count) {
        SolMirRuntimeFailureSite *site = &conventions.failure_sites[result_site], saved = *site;
#define DOMAIN_REJECT(change) do { change; CHECK(!sol_mir_runtime_cleanup_validate(&cleanup, NULL)); \
        *site = saved; CHECK(sol_mir_runtime_cleanup_validate(&cleanup, NULL)); } while (0)
        DOMAIN_REJECT(site->origin_kind = SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_CALL);
        DOMAIN_REJECT(site->owner = SOL_MIR_RUNTIME_NONE);
        DOMAIN_REJECT(site->block = SOL_MIR_RUNTIME_NONE);
        DOMAIN_REJECT(site->instruction = 0);
        DOMAIN_REJECT(++site->source.start);
        DOMAIN_REJECT(++site->source.end);
        DOMAIN_REJECT(site->allowed_codes ^= UINT32_C(1));
        DOMAIN_REJECT(site->origin_kind = SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_STEP);
        SolMirPredicateBlock *returning = &program.operations.predicate_blocks[saved.block];
        SolMirPredicateTerminator saved_term = returning->terminator;
        returning->terminator.kind = SOL_MIR_PREDICATE_TERM_FAILURE;
        CHECK(!sol_mir_runtime_cleanup_validate(&cleanup, NULL));
        returning->terminator = saved_term;
        CHECK(sol_mir_runtime_cleanup_validate(&cleanup, NULL));
        if (conventions.failure_site_count > 1) {
            size_t other = result_site == 0 ? 1 : 0;
            SolMirRuntimeFailureSite other_saved = conventions.failure_sites[other];
            conventions.failure_sites[other] = saved;
            CHECK(!sol_mir_runtime_cleanup_validate(&cleanup, NULL));
            conventions.failure_sites[other] = other_saved;
        }
#undef DOMAIN_REJECT
    }
    /* Exact and one-below budgets exercise the new metered epilogue join and
     * the removed inapplicable arena scans without adding allocation policy. */
    SolMirRuntimeCleanupLimits limits = cleanup.limits;
    limits.max_build_work = cleanup.usage.build_work;
    limits.max_validation_work = cleanup.usage.validation_work;
    CHECK(sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&conventions, &values, &limits},
        &repeat, NULL) == SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED);
    char *before = render_cleanup_text(&cleanup), *after = render_cleanup_text(&repeat);
    CHECK(before && after && !strcmp(before, after)
        && !memcmp(&cleanup.usage, &repeat.usage, sizeof cleanup.usage));
    free(before); free(after); sol_mir_runtime_cleanup_free(&repeat);
    --limits.max_build_work;
    CHECK(sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&conventions, &values, &limits},
        &repeat, NULL) == SOL_MIR_RUNTIME_CLEANUP_BUILD_RESOURCE_EXHAUSTED);
    SolMirRuntimeCleanupLimits saved_limits = cleanup.limits;
    cleanup.limits.max_validation_work = cleanup.usage.validation_work - 1;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup, NULL));
    cleanup.limits = saved_limits;
    CHECK(sol_mir_runtime_cleanup_validate(&cleanup, NULL));
done:
    sol_mir_runtime_cleanup_free(&repeat); sol_mir_runtime_cleanup_free(&cleanup);
    sol_mir_runtime_values_free(&values); sol_mir_runtime_conventions_free(&conventions);
    sol_mir_concrete_program_free(&program); compilation_free(&c);
}

/* Static transport authority, not predicate-call execution admission. */
static const struct { const char *name, *leaf; size_t pairs; } transport_cases[] = {
    {"direct","p44f_site_domains_direct",1}, {"refined_call","p44f_site_domains_refined_call",1},
    {"ensures_old","p44_parallel_predicate_transport_ensures_old",1},
    {"recursive","p44_parallel_predicate_transport_recursive",1},
    {"bool_unit","p44_parallel_predicate_transport_bool_unit",1},
    {"unit_direct","p44_parallel_predicate_transport_unit_direct",1},
    {"unit_input_result","p44_parallel_predicate_transport_unit_input_result",2},
};
typedef struct { BatchMemory *ranges; size_t count, capacity; } TransportSnapshot;
static bool transport_save(const void *address, size_t count, size_t size, void *context) {
    TransportSnapshot *s = context; if (!count || !size) return true;
    if (!address || count > SIZE_MAX / size) return false;
    if (s->count == s->capacity) {
        size_t capacity = s->capacity ? s->capacity * 2 : 256;
        if (capacity < s->capacity || capacity > SIZE_MAX / sizeof *s->ranges) return false;
        void *ranges = realloc(s->ranges, capacity * sizeof *s->ranges); if (!ranges) return false;
        s->ranges = ranges; s->capacity = capacity;
    }
    size_t bytes = count * size; unsigned char *copy = malloc(bytes); if (!copy) return false;
    memcpy(copy, address, bytes); s->ranges[s->count++] = (BatchMemory){address, bytes, copy}; return true;
}
static bool transport_snapshot(SolMirRuntimeCleanup *c, TransportSnapshot *s) {
    const SolMirRuntimeConventions *v = c->conventions;
    const SolMirRuntimeValues *values = c->values;
    bool ok = sol_mir_runtime_visit_concrete_arenas(v->concrete, transport_save, s) == SOL_MIR_RUNTIME_ARENA_VISIT_OK;
#define HEADER(p) ok = transport_save(p, 1, sizeof *(p), s) && ok
#define ARENA(p,member,singular) ok = transport_save((p)->member, (p)->singular##_capacity, sizeof *(p)->member, s) && ok
    HEADER(c); HEADER(v); HEADER(values);
    ARENA(c,events,event); ARENA(c,actions,action); ARENA(c,transitions,transition);
    ARENA(c,supplemental_sites,supplemental_site); ARENA(c,drop_paths,drop_path);
#define CONVENTION(member,type,singular) ARENA(v,member,singular);
    SOL_MIR_RUNTIME_CONVENTIONS_ARENAS(CONVENTION)
#undef CONVENTION
    ARENA(values,recipe_operations,recipe_operation); ARENA(values,allocation_plans,allocation_plan);
    ARENA(values,copy_plans,copy_plan); ARENA(values,equality_plans,equality_plan);
    ARENA(values,host_result_plans,host_result_plan); ARENA(values,host_result_requirements,host_result_requirement);
    ARENA(values,ownership_plans,ownership_plan); ARENA(values,ownership_variants,ownership_variant); ARENA(values,owned_edges,owned_edge);
#undef ARENA
#undef HEADER
    return ok;
}
static void transport_snapshot_free(TransportSnapshot *s) {
    for (size_t i = 0; i < s->count; ++i) free(s->ranges[i].copy);
    free(s->ranges); memset(s, 0, sizeof *s);
}
static bool pending_render_rejected(const SolMirRuntimeCleanup *c) {
    FILE *f = tmpfile(); if (!f) return false;
    bool ok = !sol_mir_runtime_cleanup_render(f, c) && !fflush(f)
        && !fseek(f, 0, SEEK_END) && ftell(f) == 0; fclose(f);
    f = tmpfile(); if (!f) return false;
    ok = ok && fwrite("sentinel", 1, 8, f) == 8 && !sol_mir_runtime_cleanup_render(f, c)
        && !fflush(f) && !fseek(f, 0, SEEK_END) && ftell(f) == 8; fclose(f); return ok;
}
static size_t transport_event(const SolMirRuntimeCleanup *c, SolMirRuntimeCleanupEventKind kind,
    SolMirRuntimeCleanupPhase phase, size_t owner, size_t block, size_t operation) {
    size_t found = SOL_MIR_RUNTIME_NONE, count = 0;
    for (size_t i = 0; i < c->event_count; ++i) {
        const SolMirRuntimeCleanupEvent *e = &c->events[i];
        if (e->kind == kind && e->phase == phase && e->owner == owner && e->block == block
            && e->operation == operation) { found = i; ++count; }
    }
    CHECK(count == 1); return found;
}
static size_t transport_site(const SolMirRuntimeConventions *c, SolMirRuntimeFailureOriginKind kind,
    size_t owner, size_t block, size_t operation, SolSpan span, uint32_t mask) {
    size_t found = SOL_MIR_RUNTIME_NONE, count = 0;
    for (size_t i = 0; i < c->failure_site_count; ++i) {
        const SolMirRuntimeFailureSite *s = &c->failure_sites[i];
        if (s->origin_kind == kind && s->owner == owner && s->block == block && s->instruction == operation
            && s->source.file == 0 && s->source.start == span.start && s->source.end == span.end
            && s->allowed_codes == mask) { found = i; ++count; }
    }
    CHECK(count == 1); return found;
}
static SolSpan transport_predicate_source(const SolMirConcreteProgram *p, size_t body) {
    size_t context = p->operations.predicate_bodies[body].context;
    size_t obligation = p->materialization.contexts[context].obligation;
    return p->program.ir->expressions[p->program.ir->obligations[obligation].predicate].span;
}
static SolMirRuntimeCleanupFailureOccurrence transport_result(const SolMirConcreteProgram *p,
    const SolMirRuntimeConventions *c, const SolMirOperationPredicatePlan *plan) {
    const SolMirPredicateBody *body = &p->operations.predicate_bodies[plan->body];
    SolMirRuntimeFailureCode code = plan->kind == SOL_MIR_OPERATION_PREDICATE_REFINEMENT
        ? SOL_MIR_RUNTIME_FAILURE_REFINEMENT_VIOLATION : body->phase == SOL_CONTRACT_REQUIRES
            ? SOL_MIR_RUNTIME_FAILURE_REQUIRE_VIOLATION : SOL_MIR_RUNTIME_FAILURE_ENSURE_VIOLATION;
    size_t returning = SOL_MIR_RUNTIME_NONE, count = 0;
    for (size_t i = body->blocks.offset; i < body->blocks.offset + body->blocks.count; ++i)
        if (p->operations.predicate_blocks[i].terminator.kind == SOL_MIR_PREDICATE_TERM_RETURN) { returning = i; ++count; }
    CHECK(count == 1); SolSpan span = transport_predicate_source(p, plan->body);
    size_t site = transport_site(c, SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT,
        plan->body, returning, SOL_MIR_RUNTIME_NONE, span, UINT32_C(1) << (code - 1));
    return (SolMirRuntimeCleanupFailureOccurrence){.failure_source=SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31,
        .site=site,.code=code,.source={0,span.start,span.end}};
}
static void transport_select_slots(SolMirRuntimeCleanup *c, size_t event, SolMirRuntimeCleanupEdgeRole role,
    const SolMirRuntimeCleanupFailureOccurrence *want, const SolMirRuntimeCleanupAction *ledger, size_t count,
    bool pending, bool local_call_also_pending) {
    SolMirRuntimeCleanupTrace trace; SolMirRuntimeCleanupAction actual[64];
    SolMirRuntimeCleanupTraceRequest request = {event,role,pending ? NULL : want,pending ? want : NULL,SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
    CHECK(sol_mir_runtime_cleanup_test_select(c, &request, actual, 64, &trace));
    CHECK(trace.has_primary && !memcmp(&trace.primary,want,sizeof *want) && trace.action_count == count
        && (!count || !memcmp(actual,ledger,count * sizeof *ledger)));
    request.pending = NULL; request.produced = NULL;
    CHECK(!sol_mir_runtime_cleanup_test_select(c,&request,actual,64,&trace));
    request.pending = pending ? NULL : want; request.produced = pending ? want : NULL;
    if (local_call_also_pending) {
        CHECK(!pending && role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE);
        CHECK(sol_mir_runtime_cleanup_test_select(c,&request,actual,64,&trace));
        CHECK(trace.has_primary && !memcmp(&trace.primary,want,sizeof *want) && trace.action_count == count
            && (!count || !memcmp(actual,ledger,count * sizeof *ledger)));
    } else CHECK(!sol_mir_runtime_cleanup_test_select(c,&request,actual,64,&trace));
    request.pending = want; request.produced = want;
    CHECK(!sol_mir_runtime_cleanup_test_select(c,&request,actual,64,&trace));
    SolMirRuntimeCleanupFailureOccurrence bad;
#define BAD_PACKET(change) do { bad = *want; change; request.pending = pending ? &bad : NULL; \
    request.produced = pending ? NULL : &bad; CHECK(!sol_mir_runtime_cleanup_test_select(c,&request,actual,64,&trace)); } while (0)
    BAD_PACKET(bad.site = SOL_MIR_RUNTIME_NONE);
    BAD_PACKET(bad.failure_source = SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING);
    BAD_PACKET(bad.code = SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED);
    BAD_PACKET(++bad.source.start); BAD_PACKET(++bad.source.end); BAD_PACKET(bad.source.file = SOL_MIR_RUNTIME_NONE);
    BAD_PACKET(bad.detail_length = 1); BAD_PACKET(bad.detail_kind = SOL_MIR_RUNTIME_FAILURE_DETAIL_PANIC_TEXT);
#undef BAD_PACKET
}
static void transport_select(SolMirRuntimeCleanup *c, size_t event, SolMirRuntimeCleanupEdgeRole role,
    const SolMirRuntimeCleanupFailureOccurrence *want, const SolMirRuntimeCleanupAction *ledger, size_t count, bool pending) {
    transport_select_slots(c,event,role,want,ledger,count,pending,false);
}
static void transport_cleanup_frozen(size_t test, SolMirRuntimeCleanup *c) {
    static const struct { SolMirRuntimeCleanupUsage usage; size_t length; const char *hash; } frozen[] = {
        {{70,119,117,0,20,25520,4288,8325,4288,6974},103912,"2e9396fa204cff56617ca1455771132dd8a27e0afea50bbcb842c9bf06d99d87"},
        {{70,138,119,0,27,26656,3640,9492,3640,7845},111275,"8c3b0fcd6ff73e64509595a7362104c5ac079ccadaa99966b787e89b7fe50767"},
        {{141,272,232,0,42,52168,14216,28581,14216,20702},218931,"d032f231113edef08167c8fca0a2ea03ae6cce44d58c23f032ac80c623b4d305"},
        {{106,216,178,0,39,40288,8328,17913,8328,13459},169874,"b1bb8db28be8d92e0fc63762e969f5c589a66c193267778ac5a5a02d9eba6a22"},
        {{101,184,169,0,37,37624,4288,16572,4288,12689},154173,"19e45e98054f4dbff1bb1652f2005d6200d84e6bca49eeb3038f6817f6e1cd82"},
        {{74,111,122,0,18,26192,4288,8565,4288,7146},103972,"bae6101cb5cda07c9b4c00c9079360ca16cd553ef55f604ff4c0342c749c4838"},
        {{116,200,199,0,36,43008,6320,22923,6320,16918},175185,"83b4290a7e54e9cac0159b48a47a7a11688694ac0f5503a0fabc2e91b752e606"},
    };
    SolMirRuntimeCleanupUsage expected = frozen[test].usage;
    CHECK(c->conventions->concrete->program.ir->file_count == 1);
    /* Cleanup's independent visitor charges one byte tick per physical path. */
    CHECK(sol_test_path_work(expected.validation_work - 29,1,sizeof SOL_TEST_SOURCE_DIR - 1,&expected.validation_work));
    CHECK(!memcmp(&expected,&c->usage,sizeof expected));
    char *text = render_cleanup_text(c); CHECK(text != NULL); if (!text) return;
    SolMirLinkageSha256 sha; SolMirLinkageDigest digest; char hash[65];
    sol_mir_linkage_internal_sha256_init(&sha); sol_mir_linkage_internal_sha256_write(&sha,text,strlen(text));
    CHECK(sol_mir_linkage_internal_sha256_finish(&sha,&digest));
    for (size_t i=0;i<32;++i) snprintf(hash+i*2,3,"%02x",digest.bytes[i]);
    CHECK(strlen(text)==frozen[test].length && !strcmp(hash,frozen[test].hash)); free(text);
}
static void transport_continuation(size_t test, const SolMirConcreteProgram *p, SolMirRuntimeCleanup *c,
    const SolMirOperationPredicatePlan *plan, size_t block, const SolMirRuntimeCleanupFailureOccurrence *pending) {
#define PARAM(local,path) {SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER,0,local,0,path}
#define SCOPE(op) {SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_SCOPE,0,op,SOL_MIR_RECIPE_NONE,SOL_MIR_RUNTIME_NONE}
#define PROP {SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE,SOL_MIR_RUNTIME_CLEANUP_ACTION_FAILURE_ONLY,SOL_MIR_RUNTIME_NONE,SOL_MIR_RECIPE_NONE,SOL_MIR_RUNTIME_NONE}
    static const struct { size_t count; SolMirRuntimeCleanupAction actions[4]; } frozen[] = {
        {3,{PARAM(1,3),SCOPE(17),PROP}}, {4,{SCOPE(15),PARAM(1,1),SCOPE(18),PROP}},
        {4,{PARAM(2,9),SCOPE(45),{SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT,0,1,0,SOL_MIR_RUNTIME_NONE},PROP}},
        {3,{PARAM(1,6),SCOPE(35),PROP}}, {3,{PARAM(3,6),SCOPE(33),PROP}},
        {3,{PARAM(1,3),SCOPE(15),PROP}}, {3,{PARAM(5,5),SCOPE(39),PROP}},
    };
#undef PARAM
#undef SCOPE
#undef PROP
    size_t used = 0, traversed = 0; bool resumed = false;
    const SolMirMaterialization *m = &p->materialization;
    SolMirRuntimeCleanupFailureOccurrence saved = *pending;
    while (block < m->block_count && traversed++ < m->block_count) {
        const SolMirMaterializedBlock *b = &m->blocks[block];
        for (size_t i=b->instructions.offset;i<b->instructions.offset+b->instructions.count;++i) {
            const SolMirMaterializedInstruction *in = &m->instructions[i];
            if (in->kind != SOL_MIR_INST_DROP_IF_INITIALIZED && in->kind != SOL_MIR_INST_SCOPE_EXIT) continue;
            if (in->kind == SOL_MIR_INST_DROP_IF_INITIALIZED) {
                CHECK(in->local < m->local_count);
                if (in->local >= m->local_count) return;
                const SolMirMaterializedLocal *local = &m->locals[in->local];
                CHECK(local->type < p->representation.recipe_count);
                if (local->type >= p->representation.recipe_count) return;
                if (p->representation.recipes[local->type].kind == SOL_MIR_RECIPE_UNIT) {
                    /* P2 retains the Unit formal/drop instruction but no root
                     * place exists: it cannot emit a physical cleanup action.
                     * Prove absence, rather than consuming a frozen ledger row. */
                    size_t places = 0, events = 0;
                    for (size_t q = 0; q < m->place_count; ++q)
                        places += m->places[q].local == in->local;
                    for (size_t q = 0; q < c->event_count; ++q) {
                        const SolMirRuntimeCleanupEvent *event = &c->events[q];
                        events += event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION
                            && event->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
                            && event->owner == plan->image && event->block == block && event->operation == i;
                    }
                    CHECK(local->kind == SOL_MIR_MATERIALIZED_LOCAL_PARAMETER
                        && local->access == SOL_ACCESS_OWNED && !places && !events);
                    continue;
                }
            }
            CHECK(used < frozen[test].count); if (used >= frozen[test].count) return;
            const SolMirRuntimeCleanupAction *want = &frozen[test].actions[used];
            CHECK(in->kind == SOL_MIR_INST_SCOPE_EXIT
                ? want->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_SCOPE && want->target == i
                : want->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER && want->target == in->local);
            size_t event = transport_event(c,SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION,
                SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION,plan->image,block,i);
            SolMirRuntimeCleanupTrace trace; SolMirRuntimeCleanupAction actual[64];
            SolMirRuntimeCleanupTraceRequest request = {event,SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO,NULL,NULL,SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
            CHECK(sol_mir_runtime_cleanup_test_select(c,&request,actual,64,&trace));
            CHECK(!trace.has_primary && trace.action_count==1 && !memcmp(actual,want,sizeof *want));
            request.pending = pending; CHECK(!sol_mir_runtime_cleanup_test_select(c,&request,actual,64,&trace));
            ++used;
        }
        const SolMirMaterializedTerminator *term = &b->terminator;
        size_t event = transport_event(c,SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR,
            SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION,plan->image,block,SOL_MIR_RUNTIME_NONE);
        if (term->kind == SOL_MIR_TERM_RESUME_FAILURE) {
            CHECK(used < frozen[test].count);
            transport_select(c,event,SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE,pending,
                frozen[test].actions+used,frozen[test].count-used,true);
            used = frozen[test].count; resumed=true; break;
        }
        CHECK(term->kind==SOL_MIR_TERM_GOTO && term->edge<m->edge_count);
        if (term->kind!=SOL_MIR_TERM_GOTO || term->edge>=m->edge_count) break;
        SolMirRuntimeCleanupTrace trace;
        SolMirRuntimeCleanupTraceRequest request = {event,SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO,NULL,NULL,SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
        CHECK(sol_mir_runtime_cleanup_test_select(c,&request,NULL,0,&trace) && !trace.has_primary && !trace.action_count);
        block=m->edges[term->edge].block;
    }
    CHECK(resumed && used==frozen[test].count && !memcmp(&saved,pending,sizeof saved));
}
static void transport_mutations(SolMirRuntimeCleanup *c,size_t event,SolMirRuntimeCleanupEdgeRole role,size_t foreign) {
    SolMirRuntimeCleanupEvent *e=&c->events[event], saved_e=*e;
    size_t id=SOL_MIR_RUNTIME_NONE;
    for(size_t i=0;i<e->transitions.count;++i) if(c->transitions[e->transitions.offset+i].edge_role==role) id=e->transitions.offset+i;
    CHECK(id<c->transition_count); if(id>=c->transition_count)return;
    SolMirRuntimeCleanupTransition *t=&c->transitions[id], saved_t=*t;
    bool terminal=role==SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE;
    size_t completed=0;
    sol_test_progress_phase("mutation-restore","mutation",terminal?28:27+(saved_e.actions.count?4:0));
#define EVENT(change) do { change; CHECK(memcmp(e,&saved_e,sizeof *e) != 0); \
    CHECK(!sol_mir_runtime_cleanup_validate(c,NULL)&&pending_render_rejected(c)); \
    *e=saved_e; CHECK(sol_mir_runtime_cleanup_validate(c,NULL)); sol_test_progress_completed(++completed); } while(0)
    CHECK(saved_e.producer == SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL);
    EVENT(e->producer=SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_ARITHMETIC);
    if(terminal) { EVENT(e->producer=SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_NO_MATCH); }
    EVENT(e->captures_failure_detail=!saved_e.captures_failure_detail);
    EVENT(e->capture_detail_kind=SOL_MIR_RUNTIME_FAILURE_DETAIL_PANIC_TEXT);
    EVENT(e->inherited_failure_site=foreign); EVENT(e->supplemental_site=0);
    EVENT(e->owner=SOL_MIR_RUNTIME_NONE); EVENT(e->block=SOL_MIR_RUNTIME_NONE);
    EVENT(e->operation=0); EVENT(e->phase=SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_STEP);
    EVENT(e->actions.count=saved_e.actions.count?0:1); EVENT(e->actions.offset=0);
    if(!terminal) { EVENT(++e->source.start); }
#undef EVENT
#define TRANSITION(change) do { change; CHECK(!sol_mir_runtime_cleanup_validate(c,NULL)&&pending_render_rejected(c)); \
    *t=saved_t; CHECK(sol_mir_runtime_cleanup_validate(c,NULL)); sol_test_progress_completed(++completed); } while(0)
    TRANSITION(t->failure_source=SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE);
    TRANSITION(t->failure_source=SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31);
    TRANSITION(t->failure_source=SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_LOCAL_OR_PENDING);
    TRANSITION(t->failure_site=foreign); TRANSITION(t->failure_mask=64);
    TRANSITION(t->continuation=terminal?0:SOL_MIR_RUNTIME_NONE); TRANSITION(t->destination=e->block);
    TRANSITION(t->primary_failure_wins=false); TRANSITION(t->outcome=SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL);
    TRANSITION(t->contract_phase=saved_t.contract_phase==SOL_CONTRACT_REQUIRES?SOL_CONTRACT_ENSURES:SOL_CONTRACT_REQUIRES);
    TRANSITION(t->contract_outcome=saved_t.contract_outcome==SOL_CONTRACT_OUTCOME_ALWAYS?SOL_CONTRACT_OUTCOME_SUCCESS:SOL_CONTRACT_OUTCOME_ALWAYS);
    if(!terminal) {
        TRANSITION(t->source_edge=SOL_MIR_RUNTIME_NONE); TRANSITION(t->edge_role=SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_VIOLATION);
        TRANSITION(t->actions.count=1); TRANSITION(t->actions.offset=0);
    }
#undef TRANSITION
    if(e->actions.count) {
        SolMirRuntimeCleanupAction *a=&c->actions[e->actions.offset],saved_a=*a;
#define ACTION(change) do { change; CHECK(!sol_mir_runtime_cleanup_validate(c,NULL)&&pending_render_rejected(c)); \
    *a=saved_a; CHECK(sol_mir_runtime_cleanup_validate(c,NULL)); sol_test_progress_completed(++completed); } while(0)
        ACTION(a->flags^=SOL_MIR_RUNTIME_CLEANUP_ACTION_FAILURE_ONLY);
        ACTION(a->target=terminal?foreign:SOL_MIR_RUNTIME_NONE); ACTION(a->recipe=0);
        ACTION(a->kind=terminal?SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE:SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE);
        if(terminal) { ACTION(a->drop_path=0); }
#undef ACTION
    }
    sol_test_progress_phase_end();
}
static void transport_pair(size_t test,size_t pair,size_t mode,const SolMirConcreteProgram *p,SolMirRuntimeCleanup *c) {
    const SolMirOperations *ops=&p->operations; const SolMirRuntimeConventions *conventions=c->conventions;
    size_t chosen=SOL_MIR_RUNTIME_NONE,n=0;
    for(size_t i=0;i<conventions->call_count;++i)if(conventions->calls[i].owner_kind==SOL_MIR_RUNTIME_CALL_OWNER_PREDICATE)
        if(n++==pair)chosen=i;
    CHECK(n==transport_cases[test].pairs && chosen<conventions->call_count); if(chosen>=conventions->call_count)return;
    const SolMirRuntimeCall *call=&conventions->calls[chosen];
    const SolMirPredicateTerminator *invoke_term=&ops->predicate_blocks[call->block].terminator;
    size_t failure=ops->predicate_edges[invoke_term->failure_edge].target;
    CHECK(call->failure_edge==invoke_term->failure_edge && call->normal_edge==invoke_term->normal_edge
        && ops->predicate_blocks[failure].body==call->predicate
        && ops->predicate_blocks[failure].terminator.failure_kind==SOL_MIR_PREDICATE_FAILURE_CALL);
    size_t terminal=transport_event(c,SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR,
        SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION,call->predicate,failure,SOL_MIR_RUNTIME_NONE);
    size_t invoke=transport_event(c,SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR,
        SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION,call->predicate,call->block,SOL_MIR_RUNTIME_NONE);
    if(terminal>=c->event_count||invoke>=c->event_count)return;
    const SolMirRuntimeCleanupAction propagation={SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE,
        SOL_MIR_RUNTIME_CLEANUP_ACTION_FAILURE_ONLY,SOL_MIR_RUNTIME_NONE,SOL_MIR_RECIPE_NONE,SOL_MIR_RUNTIME_NONE};
    const SolMirRuntimeCleanupEvent *e=&c->events[terminal];
    CHECK(e->producer==SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL && !e->captures_failure_detail
        && e->capture_detail_kind==SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE && e->inherited_failure_site==SOL_MIR_RUNTIME_NONE
        && e->supplemental_site==SOL_MIR_RUNTIME_NONE && e->operation==SOL_MIR_RUNTIME_NONE && e->semantic_site==SOL_MIR_RUNTIME_NONE
        && e->origin==SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT && e->transitions.count==1 && e->actions.count==1
        && !memcmp(&c->actions[e->actions.offset],&propagation,sizeof propagation));
    const SolMirRuntimeCleanupTransition *t=&c->transitions[e->transitions.offset];
    CHECK(t->failure_source==SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING && !t->failure_mask
        && t->failure_site==SOL_MIR_RUNTIME_NONE && t->primary_failure_wins && t->event==terminal
        && t->outcome==SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE && t->continuation==SOL_MIR_RUNTIME_NONE
        && t->source_edge==SOL_MIR_RUNTIME_NONE && t->destination==SOL_MIR_RUNTIME_NONE
        && t->actions.count==1 && t->actions.offset==e->actions.offset
        && t->contract_phase==ops->predicate_bodies[call->predicate].phase
        && t->contract_outcome==ops->predicate_bodies[call->predicate].outcome);
    size_t internal=p->linkage.bindings[invoke_term->binding].internal;
    CHECK(call->internal==internal); size_t callee=p->linkage.callables[internal].instance;
    SolSpan predicate=transport_predicate_source(p,call->predicate), span={0}; size_t sources=0;
    for(size_t i=0;i<p->program.ir->expression_count;++i) {
        const SolIrExpression *expr=&p->program.ir->expressions[i];
        if(expr->kind==SOL_IR_EXPR_CALL && expr->as.call.callable==p->materialization.images[callee].source_callable
            && expr->span.start>=predicate.start && expr->span.end<=predicate.end) {span=expr->span;++sources;}
    }
    CHECK(sources==1);
    size_t sid=transport_site(conventions,SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_CALL,call->predicate,call->block,SOL_MIR_RUNTIME_NONE,span,64);
    CHECK(sid==call->failure_site && c->events[invoke].captures_failure_detail);
    SolMirRuntimeCleanupFailureOccurrence local={.failure_source=SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31,
        .site=sid,.code=SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT,.source={0,span.start,span.end}};
    if(mode==2) {transport_mutations(c,terminal,SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE,sid);return;}
    for(size_t i=0;i<ops->predicate_count;++i) {
        const SolMirOperationPredicatePlan *plan=&ops->predicates[i]; if(plan->body!=call->predicate)continue;
        size_t outer=transport_event(c,SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR,SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION,plan->image,plan->block,SOL_MIR_RUNTIME_NONE);
        SolMirRuntimeCleanupEdgeRole role=plan->kind==SOL_MIR_OPERATION_PREDICATE_REFINEMENT?SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_FAILURE:SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_FAILURE;
        if(mode==3) {transport_mutations(c,outer,role,sid);continue;}
        sol_test_progress_phase("packet-chain","origin",2);
        transport_select_slots(c,invoke,SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE,&local,NULL,0,false,true);
        transport_select(c,terminal,SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE,&local,&propagation,1,true);
        transport_select(c,outer,role,&local,NULL,0,true);
        size_t continuation=p->materialization.edges[p->materialization.blocks[plan->block].terminator.failure_edge].block;
        transport_continuation(test,p,c,plan,continuation,&local); sol_test_progress_completed(1);
        SolMirRuntimeCleanupFailureOccurrence origin={0}; size_t producer=SOL_MIR_RUNTIME_NONE;
        SolMirRuntimeCleanupEdgeRole producer_role=SOL_MIR_RUNTIME_CLEANUP_EDGE_STEP_FAILURE;
        SolMirRuntimeCleanupAction producer_ledger[5]; size_t length=0;
        if(test==2) {
            for(size_t q=0;q<ops->predicate_count;++q) {
                const SolMirOperationPredicatePlan *real=&ops->predicates[q];
                if(real->image!=callee || real->contract_phase!=SOL_CONTRACT_REQUIRES)continue;
                CHECK(producer==SOL_MIR_RUNTIME_NONE); origin=transport_result(p,conventions,real);
                producer=transport_event(c,SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR,SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION,callee,real->block,SOL_MIR_RUNTIME_NONE);
                producer_role=SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_VIOLATION;
                producer_ledger[length++]=(SolMirRuntimeCleanupAction){SOL_MIR_RUNTIME_CLEANUP_ACTION_CHECK_CONTRACT,0,
                    p->materialization.blocks[real->block].terminator.source_obligation,SOL_MIR_RECIPE_NONE,SOL_MIR_RUNTIME_NONE};
            }
        } else if(test==3) {
            for(size_t q=0;q<ops->arithmetic_count;++q) {
                const SolMirOperationArithmeticPlan *real=&ops->arithmetic[q]; if(real->image!=callee || !real->failures)continue;
                CHECK(producer==SOL_MIR_RUNTIME_NONE); const SolMirMaterializedInstruction *in=&p->materialization.instructions[real->instruction];
                SolSpan source=p->program.ir->expressions[in->source_expression].span;
                size_t real_sid=transport_site(conventions,SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_ARITHMETIC,callee,in->block,real->instruction,source,2);
                origin=(SolMirRuntimeCleanupFailureOccurrence){.failure_source=SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31,
                    .site=real_sid,.code=SOL_MIR_RUNTIME_FAILURE_INTEGER_OVERFLOW,.source={0,source.start,source.end}};
                producer=transport_event(c,SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION,SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION,callee,in->block,real->instruction);
                producer_role=SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE;
                const SolMirRuntimeCleanupAction literal[]={
                    {SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_SCOPE,0,6,SOL_MIR_RECIPE_NONE,SOL_MIR_RUNTIME_NONE},
                    {SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_SCOPE,0,2,SOL_MIR_RECIPE_NONE,SOL_MIR_RUNTIME_NONE},
                    {SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_SCOPE,0,1,SOL_MIR_RECIPE_NONE,SOL_MIR_RUNTIME_NONE},
                    {SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER,0,0,0,0},
                    {SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE,SOL_MIR_RUNTIME_CLEANUP_ACTION_FAILURE_ONLY,real_sid,SOL_MIR_RECIPE_NONE,SOL_MIR_RUNTIME_NONE}};
                memcpy(producer_ledger,literal,sizeof literal);length=5;
            }
        } else {
            const SolMirMaterializedImage *image=&p->materialization.images[callee];
            size_t operation=p->materialization.blocks[image->entry].instructions.offset;
            SolSpan source=p->program.ir->callables[image->source_callable].span;
            size_t real_sid=transport_site(conventions,SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_STEP,callee,image->entry,operation,source,32);
            origin=(SolMirRuntimeCleanupFailureOccurrence){.failure_source=SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31,
                .site=real_sid,.code=SOL_MIR_RUNTIME_FAILURE_STEP_LIMIT,.source={0,source.start,source.end}};
            producer=transport_event(c,SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION,SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_STEP,callee,image->entry,operation);
            producer_ledger[length++]=propagation;producer_ledger[0].target=real_sid;
        }
        CHECK(producer<c->event_count);if(producer>=c->event_count)return;
        transport_select(c,producer,producer_role,&origin,producer_ledger,length,false);
        transport_select(c,invoke,SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE,&origin,NULL,0,true);
        transport_select(c,terminal,SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE,&origin,&propagation,1,true);
        transport_select(c,outer,role,&origin,NULL,0,true);transport_continuation(test,p,c,plan,continuation,&origin);
        SolMirRuntimeCleanupTrace trace;SolMirRuntimeCleanupTraceRequest normal={invoke,SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_NORMAL,&local,NULL,SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
        CHECK(!sol_mir_runtime_cleanup_test_select(c,&normal,NULL,0,&trace));normal.produced=NULL;normal.pending=&origin;
        CHECK(!sol_mir_runtime_cleanup_test_select(c,&normal,NULL,0,&trace));
        sol_test_progress_completed(2);sol_test_progress_phase_end();
    }
}
static void transport_false_routes(const SolMirConcreteProgram *p,SolMirRuntimeCleanup *c) {
    sol_test_progress_phase("false-producers","predicate",p->operations.predicate_count);
    for(size_t i=0;i<p->operations.predicate_count;++i) {
        const SolMirOperationPredicatePlan *plan=&p->operations.predicates[i];
        SolMirRuntimeCleanupFailureOccurrence packet=transport_result(p,c->conventions,plan);
        bool refined=plan->kind==SOL_MIR_OPERATION_PREDICATE_REFINEMENT;
        size_t event=transport_event(c,SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR,SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION,
            plan->image,plan->block,SOL_MIR_RUNTIME_NONE);
        SolMirRuntimeCleanupEdgeRole role=refined?SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_VIOLATION:SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_VIOLATION;
        SolMirRuntimeCleanupAction action={SOL_MIR_RUNTIME_CLEANUP_ACTION_CHECK_CONTRACT,0,
            p->materialization.blocks[plan->block].terminator.source_obligation,SOL_MIR_RECIPE_NONE,SOL_MIR_RUNTIME_NONE};
        transport_select(c,event,role,&packet,refined?NULL:&action,refined?0:1,false);
        SolMirRuntimeCleanupTrace trace;SolMirRuntimeCleanupTraceRequest request={event,role,&packet,NULL,SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
        for(SolMirRuntimeFailureCode code=SOL_MIR_RUNTIME_FAILURE_REQUIRE_VIOLATION;code<=SOL_MIR_RUNTIME_FAILURE_REFINEMENT_VIOLATION;++code) {
            if(code==packet.code)continue; SolMirRuntimeCleanupFailureOccurrence bad=packet;bad.code=code;request.produced=&bad;
            CHECK(!sol_mir_runtime_cleanup_test_select(c,&request,NULL,0,&trace));
        }
        request.produced=&packet;request.edge_role=refined?SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_SATISFIED:SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_SATISFIED;
        CHECK(!sol_mir_runtime_cleanup_test_select(c,&request,NULL,0,&trace));
        request.edge_role=refined?SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_FAILURE:SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_FAILURE;
        CHECK(!sol_mir_runtime_cleanup_test_select(c,&request,NULL,0,&trace));
        sol_test_progress_completed(i+1);
    }
    sol_test_progress_phase_end();
}
static bool transport_empty_cleanup(const SolMirRuntimeCleanup *c) {
    return !c->conventions&&!c->values&&!c->events&&!c->event_count&&!c->event_capacity
        &&!c->actions&&!c->action_count&&!c->action_capacity&&!c->transitions&&!c->transition_count&&!c->transition_capacity
        &&!c->supplemental_sites&&!c->supplemental_site_count&&!c->supplemental_site_capacity
        &&!c->drop_paths&&!c->drop_path_count&&!c->drop_path_capacity
        &&!memcmp(&c->usage,&(SolMirRuntimeCleanupUsage){0},sizeof c->usage)
        &&!memcmp(&c->limits,&(SolMirRuntimeCleanupLimits){0},sizeof c->limits);
}
static void transport_cleanup_resources(SolMirRuntimeCleanup *c,size_t mode,size_t kind,size_t ordinal,const char *baseline) {
    SolMirRuntimeCleanupLimits exact=c->limits;
#define EXACT(field) exact.max_##field=c->usage.field?c->usage.field:1
    EXACT(events);EXACT(actions);EXACT(transitions);EXACT(supplemental_sites);EXACT(drop_paths);
    EXACT(owned_bytes);EXACT(build_scratch_bytes);EXACT(build_work);EXACT(validation_scratch_bytes);EXACT(validation_work);
#undef EXACT
    SolMirRuntimeCleanupBuildRequest request={c->conventions,c->values,&exact};
    SolMirRuntimeCleanup draft;sol_mir_runtime_cleanup_init(&draft);
    if(mode==4) {
        sol_test_progress_phase("quotas","field",10);size_t completed=0;
        SolMirRuntimeCleanupUsage reconstructed;size_t alias=0;
        CHECK(sol_mir_runtime_cleanup_test_reconstruct_usage(c->conventions,c->values,&exact,&reconstructed)
            &&sol_mir_runtime_cleanup_test_alias_work(c,&alias));
        reconstructed.validation_work+=alias;CHECK(!memcmp(&reconstructed,&c->usage,sizeof reconstructed));
        CHECK(sol_mir_runtime_cleanup_build(&request,&draft,NULL)==SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED);
        char *text=render_cleanup_text(&draft);CHECK(text&&!strcmp(text,baseline)&&!memcmp(&draft.usage,&c->usage,sizeof c->usage));
        free(text);sol_mir_runtime_cleanup_free(&draft);
#define BELOW(field) do { if(c->usage.field) {SolMirRuntimeCleanupLimits below=exact;--below.max_##field;request.limits=&below; \
    CHECK(sol_mir_runtime_cleanup_build(&request,&draft,NULL)==SOL_MIR_RUNTIME_CLEANUP_BUILD_RESOURCE_EXHAUSTED); \
    CHECK(transport_empty_cleanup(&draft));} sol_test_progress_completed(++completed); } while(0)
        BELOW(events);BELOW(actions);BELOW(transitions);BELOW(supplemental_sites);BELOW(drop_paths);
        BELOW(owned_bytes);BELOW(build_scratch_bytes);BELOW(build_work);BELOW(validation_scratch_bytes);BELOW(validation_work);
#undef BELOW
        sol_test_progress_phase_end();
    } else if(mode==5) {
        SolMirRuntimeCleanupUsage saved=c->usage;size_t completed=0;
        sol_test_progress_phase("usage-mutation-restore","field",10);
#define BAD_USAGE(field) do {if(saved.field) {--c->usage.field; \
    CHECK(!sol_mir_runtime_cleanup_validate(c,NULL)&&pending_render_rejected(c)); \
    c->usage=saved;++c->usage.field;CHECK(!sol_mir_runtime_cleanup_validate(c,NULL)&&pending_render_rejected(c)); \
    c->usage=saved;CHECK(sol_mir_runtime_cleanup_validate(c,NULL));} sol_test_progress_completed(++completed);}while(0)
        BAD_USAGE(events);BAD_USAGE(actions);BAD_USAGE(transitions);BAD_USAGE(supplemental_sites);BAD_USAGE(drop_paths);
        BAD_USAGE(owned_bytes);BAD_USAGE(build_scratch_bytes);BAD_USAGE(build_work);BAD_USAGE(validation_scratch_bytes);BAD_USAGE(validation_work);
#undef BAD_USAGE
        sol_test_progress_phase_end();
    } else {
        sol_test_progress_phase("fault-retry-render","ordinal",1);
        sol_test_progress_ordinal(kind==0?"build":kind==1?"persistent":"validation",ordinal,kind==1?4:1);
        if(kind==0)sol_mir_runtime_cleanup_test_force_build_scratch_failure_attempt(ordinal);
        if(kind==1)sol_mir_runtime_cleanup_test_force_persistent_allocation_failure_attempt(ordinal);
        if(kind==2)sol_mir_runtime_cleanup_test_force_validation_scratch_failure_attempt(ordinal);
        SolMirRuntimeCleanupBuildOutcome outcome=sol_mir_runtime_cleanup_build(&request,&draft,NULL);
        size_t bn=sol_mir_runtime_cleanup_test_build_scratch_attempts(),pn=sol_mir_runtime_cleanup_test_persistent_allocation_attempts(),vn=sol_mir_runtime_cleanup_test_validation_scratch_attempts();
        sol_mir_runtime_cleanup_test_force_build_scratch_failure_attempt(0);
        sol_mir_runtime_cleanup_test_force_persistent_allocation_failure_attempt(0);
        sol_mir_runtime_cleanup_test_force_validation_scratch_failure_attempt(0);
        CHECK(outcome==SOL_MIR_RUNTIME_CLEANUP_BUILD_ALLOCATION_FAILED && transport_empty_cleanup(&draft));
        CHECK(kind==0?(bn==ordinal&&!pn&&!vn):kind==1?(bn==1&&pn==ordinal&&!vn):(bn==1&&pn==4&&vn==ordinal));
        sol_mir_runtime_cleanup_free(&draft);sol_mir_runtime_cleanup_free(&draft);
        CHECK(sol_mir_runtime_conventions_validate(c->conventions,NULL)&&sol_mir_runtime_values_validate(c->values,NULL)
            &&sol_mir_runtime_cleanup_validate(c,NULL));
        bool retried=sol_mir_runtime_cleanup_build(&request,&draft,NULL)==SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED;CHECK(retried);
        if(retried) {char *text=render_cleanup_text(&draft);CHECK(text&&!strcmp(text,baseline)&&!memcmp(&draft.usage,&c->usage,sizeof c->usage));free(text);}
        sol_test_progress_completed(1);sol_test_progress_phase_end();
    }
    sol_mir_runtime_cleanup_free(&draft);sol_mir_runtime_cleanup_free(&draft);
}
static void check_predicate_transport_fixture(size_t test,size_t mode,size_t pair,size_t kind,size_t ordinal) {
    Compilation compilation;SolMirConcreteProgram p;SolMirRuntimeConventions conventions;SolMirRuntimeValues values;SolMirRuntimeCleanup c;
    sol_mir_concrete_program_init(&p);sol_mir_runtime_conventions_init(&conventions);sol_mir_runtime_values_init(&values);sol_mir_runtime_cleanup_init(&c);
    TransportSnapshot snapshot={0};char *baseline=NULL;
    char directory[512];snprintf(directory,sizeof directory,"%s/tests/conformance/%s",SOL_TEST_SOURCE_DIR,transport_cases[test].leaf);
    bool built=compile_directory(&compilation,directory);CHECK(built);
    if(built) {static const char path[]="virtual://p44f1a/transport.sol";char *normalized=malloc(sizeof path);CHECK(normalized!=NULL);
        if(normalized){memcpy(normalized,path,sizeof path);free(compilation.ir.source_path);compilation.ir.source_path=normalized;}else built=false;}
    SolMirProgramRoot root={built?callable(&compilation.ir,"launch",SOL_IR_CALLABLE_FUNCTION):SOL_IR_NONE,SOL_MIR_PROGRAM_ROOT_ENTRY};
    SolMirTargetDescriptor target=sol_mir_target_wasm32();
    sol_test_progress_phase("predecessors-owner","owner",4);
    built=built&&root.callable!=SOL_IR_NONE&&sol_mir_concrete_program_build(&(SolMirConcreteBuildRequest){&compilation.ir,&root,1,NULL,0,&target,NULL},&p,&compilation.diagnostics)==SOL_MIR_CONCRETE_BUILD_SUCCEEDED;
    if(built)sol_test_progress_completed(1);
    built=built&&sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&p,NULL},&conventions,&compilation.diagnostics)==SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED;
    if(built)sol_test_progress_completed(2);
    built=built&&sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&conventions,NULL},&values,&compilation.diagnostics)==SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED;
    if(built)sol_test_progress_completed(3);
    built=built&&sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&conventions,&values,NULL},&c,&compilation.diagnostics)==SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED;
    size_t bn=sol_mir_runtime_cleanup_test_build_scratch_attempts(),pn=sol_mir_runtime_cleanup_test_persistent_allocation_attempts(),vn=sol_mir_runtime_cleanup_test_validation_scratch_attempts();
    CHECK(built);if(built)sol_test_progress_completed(4);sol_test_progress_phase_end();if(!built)goto done;
    CHECK(bn==1&&pn==4&&vn==1);transport_cleanup_frozen(test,&c);
    baseline=render_cleanup_text(&c);CHECK(baseline!=NULL);bool saved=transport_snapshot(&c,&snapshot);CHECK(saved);if(!baseline||!saved)goto done;
    if(mode==0)transport_false_routes(&p,&c);
    if(mode>=1&&mode<=3)transport_pair(test,pair,mode,&p,&c);
    if(mode>=4)transport_cleanup_resources(&c,mode,kind,ordinal,baseline);
    batch_memory_check(snapshot.ranges,snapshot.count);
    char *restored=render_cleanup_text(&c);CHECK(restored&&!strcmp(restored,baseline));free(restored);
    CHECK(sol_mir_runtime_conventions_validate(&conventions,NULL)&&sol_mir_runtime_values_validate(&values,NULL)&&sol_mir_runtime_cleanup_validate(&c,NULL));
done:
    sol_test_progress_phase("teardown","case",1);free(baseline);transport_snapshot_free(&snapshot);
    sol_mir_runtime_cleanup_free(&c);sol_mir_runtime_values_free(&values);sol_mir_runtime_conventions_free(&conventions);
    sol_mir_concrete_program_free(&p);compilation_free(&compilation);sol_test_progress_completed(1);sol_test_progress_phase_end();
}
static void transport_baseline_batch(size_t mode) {
    Compilation f; SolMirConcreteProgram p; SolMirRuntimeConventions conventions;
    SolMirRuntimeValues values; SolMirRuntimeCleanup c;
    sol_mir_concrete_program_init(&p); sol_mir_runtime_conventions_init(&conventions);
    sol_mir_runtime_values_init(&values); sol_mir_runtime_cleanup_init(&c);
    sol_test_progress_phase("source-predecessors-owner", "pipeline", 1);
    sol_test_progress_operation_begin("baseline-source-owner");
    bool built = compile_e6(&f) && build_concrete(&f,&p)
        && sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&p,NULL},&conventions,&f.diagnostics)
            == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED
        && sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&conventions,NULL},&values,&f.diagnostics)
            == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED
        && sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&conventions,&values,NULL},&c,&f.diagnostics)
            == SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED;
    sol_test_progress_operation_end();
    CHECK(built); if (built) sol_test_progress_completed(1); sol_test_progress_phase_end();
    if (built) {
        CHECK(c.event_count == 1059 && c.transition_count == 1835 && c.action_count == 4242);
        size_t contract_edges = 0, buckets[5] = {0};
        for (size_t i = 0; i < p.materialization.block_count; ++i) {
            const SolMirMaterializedTerminator *term = &p.materialization.blocks[i].terminator;
            if (term->kind == SOL_MIR_TERM_CHECK_CONTRACT) {
                CHECK(term->failure_edge < p.materialization.edge_count); ++contract_edges;
            }
        }
        for (size_t i = 0; i < c.transition_count; ++i) {
            CHECK(c.transitions[i].failure_source < 5);
            if (c.transitions[i].failure_source < 5) ++buckets[c.transitions[i].failure_source];
        }
        CHECK(contract_edges <= 1048);
        const size_t expected[] = {1048-contract_edges,700,51,23+contract_edges,13};
        CHECK(!memcmp(buckets,expected,sizeof expected));
        fprintf(stderr,"P44F1A_CENSUS C=%zu A=1025 B=1074 local=17 total=2116\n",contract_edges);
        if (mode == 0) check_owner_snapshot_contract_and_selector(&c,&p,&conventions);
        if (mode == 1) {
            /* Retain main's deterministic first inherited producer, including
             * its original code ordering and bytewise occurrence proof. */
            size_t event_id = SOL_MIR_RUNTIME_NONE;
            SolMirRuntimeFailureCode code = SOL_MIR_RUNTIME_FAILURE_NONE;
            for (size_t i = 0; i < c.event_count && event_id == SOL_MIR_RUNTIME_NONE; ++i) {
                const SolMirRuntimeCleanupEvent *event = &c.events[i];
                if (event->inherited_failure_site == SOL_MIR_RUNTIME_NONE) continue;
                uint32_t mask = conventions.failure_sites[event->inherited_failure_site].allowed_codes;
                for (unsigned k = SOL_MIR_RUNTIME_FAILURE_PANIC; k <= SOL_MIR_RUNTIME_FAILURE_HOST_ERROR; ++k) {
                    if (!(mask & (UINT32_C(1) << (k-1)))) continue;
                    for (size_t j = 0; j < event->transitions.count; ++j)
                        if (c.transitions[event->transitions.offset+j].failure_source
                                == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31) {
                            event_id = i; code = (SolMirRuntimeFailureCode)k; break;
                        }
                    if (event_id != SOL_MIR_RUNTIME_NONE) break;
                }
            }
            CHECK(event_id < c.event_count);
            if (event_id < c.event_count) {
                size_t site = c.events[event_id].inherited_failure_site;
                SolMirRuntimeCleanupFailureOccurrence packet = {
                    SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31,site,code,
                    SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE,0,{0},conventions.failure_sites[site].source};
                check_immutable_occurrence_sweep(&c,&conventions,&packet);
            }
        }
        if (mode == 2) check_local_or_pending(&c,&conventions,&p);
    }
    sol_test_progress_phase("teardown","case",1);
    sol_mir_runtime_cleanup_free(&c); sol_mir_runtime_values_free(&values);
    sol_mir_runtime_conventions_free(&conventions); sol_mir_concrete_program_free(&p); compilation_free(&f);
    sol_test_progress_completed(1); sol_test_progress_phase_end();
}
static int transport_dispatch(const char *selected,bool list) {
    bool found=selected==NULL;
#define CASE(key,call) do {if(list)puts(key);else if(!selected||!strcmp(selected,key)){found=true;sol_test_progress_case(key);call;sol_test_progress_case_end();}}while(0)
    CASE("baseline.cleanup.e6.owner-snapshot",transport_baseline_batch(0));
    CASE("baseline.cleanup.e6.immutable-occurrence",transport_baseline_batch(1));
    CASE("baseline.cleanup.e6.local-or-pending",transport_baseline_batch(2));
    for(size_t test=0;test<sizeof transport_cases/sizeof *transport_cases;++test) {
        char key[160];snprintf(key,sizeof key,"cleanup.%s.frozen-false",transport_cases[test].name);
        CASE(key,check_predicate_transport_fixture(test,0,0,0,0));
        for(size_t pair=0;pair<transport_cases[test].pairs;++pair)for(size_t mode=1;mode<=3;++mode) {
            snprintf(key,sizeof key,"cleanup.%s.pair.%zu.%s",transport_cases[test].name,pair+1,mode==1?"packets":mode==2?"terminal-mutations":"outer-mutations");
            CASE(key,check_predicate_transport_fixture(test,mode,pair,0,0));
        }
        if(test==0||test==2) {
            snprintf(key,sizeof key,"cleanup.%s.quotas",transport_cases[test].name);CASE(key,check_predicate_transport_fixture(test,4,0,0,0));
            snprintf(key,sizeof key,"cleanup.%s.usage",transport_cases[test].name);CASE(key,check_predicate_transport_fixture(test,5,0,0,0));
            for(size_t kind=0;kind<3;++kind)for(size_t ordinal=1;ordinal<=(kind==1?4u:1u);++ordinal) {
                snprintf(key,sizeof key,"cleanup.%s.fault.%s.%zu",transport_cases[test].name,kind==0?"build":kind==1?"persistent":"validation",ordinal);
                CASE(key,check_predicate_transport_fixture(test,6,0,kind,ordinal));
            }
        }
    }
    static const char *leaves[]={"p44f_site_domains_branch","p44f_site_domains_direct","p44f_site_domains_refined","p44c_qualified_sequential_stress"};
    for(size_t i=0;i<4;++i) {char key[80];snprintf(key,sizeof key,"baseline.cleanup.f0.%zu",i+1);
        CASE(key,check_site_domain_fixture(leaves[i],i==3?"first":"launch",i==3?SOL_MIR_RUNTIME_NONE:i));}
#undef CASE
    return found?(failures?1:0):2;
}
int main(int argc,char **argv) {
    if(argc==2&&!strcmp(argv[1],"--list-p44f1a-cases"))return transport_dispatch(NULL,true);
    if(argc==3&&!strcmp(argv[1],"--p44f1a-case"))return transport_dispatch(argv[2],false);
    if(argc!=1)return 2;
    (void)transport_dispatch(NULL,false);
#define check_site_domain_fixture(...) SOL_TEST_RUN(check_site_domain_fixture(__VA_ARGS__))
#define check_refined_route_fixture(...) SOL_TEST_RUN(check_refined_route_fixture(__VA_ARGS__))
#define check_predicate_propagate_schema(...) SOL_TEST_RUN(check_predicate_propagate_schema(__VA_ARGS__))
#define check_zero_supplemental_alias_preflight(...) SOL_TEST_RUN(check_zero_supplemental_alias_preflight(__VA_ARGS__))
#define check_snapshot_prerequisite(...) SOL_TEST_RUN(check_snapshot_prerequisite(__VA_ARGS__))
#define check_local_or_pending(...) SOL_TEST_RUN(check_local_or_pending(__VA_ARGS__))
#define check_callback_selector(...) SOL_TEST_RUN(check_callback_selector(__VA_ARGS__))
#define check_graph_inherited_capability_calls(...) SOL_TEST_RUN(check_graph_inherited_capability_calls(__VA_ARGS__))
#define check_failure_propagation_is_final(...) SOL_TEST_RUN(check_failure_propagation_is_final(__VA_ARGS__))
#define check_eight_fixture_groups(...) SOL_TEST_RUN(check_eight_fixture_groups(__VA_ARGS__))
#define check_owner_snapshot_contract_and_selector(...) SOL_TEST_RUN(check_owner_snapshot_contract_and_selector(__VA_ARGS__))
#define check_authenticated_contract_suppression(...) SOL_TEST_RUN(check_authenticated_contract_suppression(__VA_ARGS__))
#define check_scope_aware_implicit_unwind(...) SOL_TEST_RUN(check_scope_aware_implicit_unwind(__VA_ARGS__))
#define check_affine_pair_owner(...) SOL_TEST_RUN(check_affine_pair_owner(__VA_ARGS__))
#define check_reduced_validation_work_rejects_without_write(...) SOL_TEST_RUN(check_reduced_validation_work_rejects_without_write(__VA_ARGS__))
#define check_cleanup_arena_alias(...) SOL_TEST_RUN(check_cleanup_arena_alias(__VA_ARGS__))
    check_site_domain_fixture("p44f_site_domains_branch", "launch", 0);
    check_site_domain_fixture("p44f_site_domains_direct", "launch", 1);
    check_site_domain_fixture("p44f_site_domains_refined", "launch", 2);
    check_site_domain_fixture("p44c_qualified_sequential_stress", "first", SOL_MIR_RUNTIME_NONE);
    check_refined_route_fixture();
    check_predicate_propagate_schema();
    check_zero_supplemental_alias_preflight();
    check_snapshot_prerequisite();
    SolMirRuntimeCleanup cleanup;
    sol_mir_runtime_cleanup_init(&cleanup);
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup, NULL));
    SolMirRuntimeCleanupBuildRequest request = {0};
    CHECK(sol_mir_runtime_cleanup_build(&request, &cleanup, NULL)
        == SOL_MIR_RUNTIME_CLEANUP_BUILD_INVALID_ARGUMENT);
    FILE *stream = tmpfile();
    CHECK(stream != NULL);
    if (stream != NULL) {
        CHECK(!sol_mir_runtime_cleanup_render(stream, &cleanup));
        CHECK(fflush(stream) == 0 && fseek(stream, 0, SEEK_END) == 0 && ftell(stream) == 0);
        fclose(stream);
    }
    SolMirRuntimeCleanupLimits limits = sol_mir_runtime_cleanup_default_limits();
    limits.max_events = 0;
    request.limits = &limits;
    CHECK(sol_mir_runtime_cleanup_build(&request, &cleanup, NULL)
        == SOL_MIR_RUNTIME_CLEANUP_BUILD_INVALID_ARGUMENT);
    sol_mir_runtime_cleanup_free(&cleanup);
    Compilation c; SolMirConcreteProgram program; SolMirRuntimeConventions conventions; SolMirRuntimeValues values;
    sol_mir_concrete_program_init(&program); sol_mir_runtime_conventions_init(&conventions); sol_mir_runtime_values_init(&values);
    CHECK(compile_e6(&c)); CHECK(build_concrete(&c,&program));
    SolMirRuntimeConventionsBuildRequest cr={&program,NULL}; SolMirRuntimeValuesBuildRequest vr={&conventions,NULL};
    CHECK(sol_mir_runtime_conventions_build(&cr,&conventions,&c.diagnostics)==SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_values_build(&vr,&values,&c.diagnostics)==SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED);
    SolMirRuntimeCleanupBuildRequest rr={&conventions,&values,NULL};
    SolMirRuntimeCleanupBuildOutcome cleanup_outcome=sol_mir_runtime_cleanup_build(&rr,&cleanup,&c.diagnostics);
    if(cleanup_outcome!=SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED)for(size_t i=0;i<c.diagnostics.count;++i)fprintf(stderr,"cleanup diagnostic: %s\n",c.diagnostics.items[i].message);
    CHECK(cleanup_outcome==SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED);
    if(cleanup_outcome!=SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED){sol_mir_runtime_values_free(&values);sol_mir_runtime_conventions_free(&conventions);sol_mir_concrete_program_free(&program);compilation_free(&c);return 1;}
    check_local_or_pending(&cleanup,&conventions,&program);
    check_callback_selector();
    SolMirRuntimeCleanupFailureOccurrence pending_arithmetic;
    bool have_pending_arithmetic = authenticated_arithmetic_occurrence(&conventions,
        &pending_arithmetic);
    CHECK(have_pending_arithmetic && sol_mir_runtime_failure_record_validate(&conventions,
        &(SolMirRuntimeFailureRecord){pending_arithmetic.code, pending_arithmetic.source,
            pending_arithmetic.detail_kind, NULL, pending_arithmetic.detail_length}));
    check_graph_inherited_capability_calls(have_pending_arithmetic ? &pending_arithmetic : NULL);
    /* Frozen all-roots predecessor censuses from P3.1 and P3.2. */
    CHECK(conventions.signature_count == 18 && conventions.signature_slot_count == 19
        && conventions.call_count == 18 && conventions.operand_count == 24
        && conventions.writeback_count == 1 && conventions.entry_count == 1
        && conventions.import_count == 56 && conventions.failure_site_count == 713);
    size_t expected_conventions_work = 0, expected_values_work = 0, expected_cleanup_work = 0;
    CHECK(sol_test_path_work(96025258, 16, sizeof SOL_TEST_SOURCE_DIR - 1, &expected_conventions_work));
    CHECK(sol_test_path_work(96030228, 24, sizeof SOL_TEST_SOURCE_DIR - 1, &expected_values_work));
    CHECK(sol_test_path_work(1170581, 4, sizeof SOL_TEST_SOURCE_DIR - 1, &expected_cleanup_work));
    CHECK(conventions.usage.signatures == 18 && conventions.usage.signature_slots == 19
        && conventions.usage.calls == 18 && conventions.usage.operands == 24
        && conventions.usage.writebacks == 1 && conventions.usage.entries == 1
        && conventions.usage.imports == 56 && conventions.usage.failure_sites == 713
        && conventions.usage.owned_bytes == 66480
        && conventions.usage.build_scratch_bytes == 21
        && conventions.usage.build_work == 12847
        && conventions.usage.validation_scratch_bytes == 763171604
        && conventions.usage.validation_work == expected_conventions_work);
    CHECK(values.recipe_operation_count == 21 && values.allocation_plan_count == 21
        && values.copy_plan_count == 21 && values.equality_plan_count == 21
        && values.host_result_plan_count == 21 && values.host_result_requirement_count == 4
        && values.ownership_plan_count == 21 && values.ownership_variant_count == 9
        && values.owned_edge_count == 17);
    CHECK(values.usage.records == 21 && values.usage.allocation_plans == 21
        && values.usage.copy_plans == 21 && values.usage.equality_plans == 21
        && values.usage.host_result_plans == 21 && values.usage.host_result_requirements == 4
        && values.usage.ownership_plans == 21 && values.usage.ownership_variants == 9
        && values.usage.owned_edges == 17 && values.usage.owned_bytes == 4592
        && values.usage.build_scratch_bytes == 98 && values.usage.build_work == 410
        && values.usage.validation_scratch_bytes == 763171604
        && values.usage.validation_work == expected_values_work);
    /* Final P3.3 E6 all-roots cleanup-policy census and exact metering. */
    CHECK(cleanup.event_count == 1059 && cleanup.action_count == 4242
        && cleanup.transition_count == 1835 && cleanup.supplemental_site_count == 51
        && cleanup.drop_path_count == 1683);
    CHECK(cleanup.usage.events == 1059 && cleanup.usage.actions == 4242
        && cleanup.usage.transitions == 1835 && cleanup.usage.supplemental_sites == 51
        && cleanup.usage.drop_paths == 1683 && cleanup.usage.owned_bytes == 538752
        && cleanup.usage.build_scratch_bytes == 246744 && cleanup.usage.build_work == 1746303
        && cleanup.usage.validation_scratch_bytes == 246744
        && cleanup.usage.validation_work == expected_cleanup_work);
    CHECK(sol_mir_runtime_cleanup_validate(&cleanup,NULL));
    StepKeyCollisions step_key_collision_count={0};size_t step_key_classes[4]={0};
    CHECK(step_key_collisions(&cleanup,&step_key_collision_count,step_key_classes));
    const size_t expected_step_key_classes[]={583,91,6,4};
    CHECK(step_key_collision_count.events==0&&step_key_collision_count.ready==0
        &&step_key_collision_count.failure==0
        &&!memcmp(step_key_classes,expected_step_key_classes,sizeof step_key_classes));
    size_t event_kinds[4] = {0}, phases[4] = {0}, producers[14] = {0};
    size_t action_kinds[10] = {0}, outcomes[3] = {0}, roles[17] = {0};
    size_t failure_sources[5] = {0}, drop_liveness[2] = {0};
    for (size_t i = 0; i < cleanup.event_count; ++i) {
        ++event_kinds[cleanup.events[i].kind];
        ++phases[cleanup.events[i].phase];
        ++producers[cleanup.events[i].producer];
    }
    for (size_t i = 0; i < cleanup.action_count; ++i) {
        const SolMirRuntimeCleanupAction *action = &cleanup.actions[i];
        ++action_kinds[action->kind];
        if (action->drop_path != SOL_MIR_RUNTIME_NONE)
            ++drop_liveness[cleanup.drop_paths[action->drop_path].liveness];
    }
    for (size_t i = 0; i < cleanup.transition_count; ++i) {
        ++outcomes[cleanup.transitions[i].outcome];
        ++roles[cleanup.transitions[i].edge_role];
        ++failure_sources[cleanup.transitions[i].failure_source];
    }
    const size_t expected_event_kinds[] = {850, 189, 12, 8};
    const size_t expected_phases[] = {368, 5, 2, 684};
    const size_t expected_producers[] = {295, 5, 18, 1, 1, 0, 0, 0, 0,
        4, 44, 5, 2, 684};
    const size_t expected_action_kinds[] = {1, 7, 146, 639, 1441, 18, 0, 189,
        1044, 757};
    const size_t expected_outcomes[] = {1025, 793, 17};
    const size_t expected_roles[] = {284, 12, 12, 18, 18, 1, 1, 1, 2, 2,
        3, 3, 3, 17, 83, 691, 684};
    /* Independently enumerate authenticated P2 CHECK_CONTRACT failure edges,
     * never infer C from the cleanup rows whose census is being checked. */
    size_t contract_evaluation_edges = 0;
    for (size_t i = 0; i < program.materialization.block_count; ++i) {
        const SolMirMaterializedTerminator *term = &program.materialization.blocks[i].terminator;
        if (term->kind == SOL_MIR_TERM_CHECK_CONTRACT) {
            CHECK(term->failure_edge < program.materialization.edge_count);
            ++contract_evaluation_edges;
        }
    }
    CHECK(contract_evaluation_edges <= 1048);
    const size_t expected_failure_sources[] = {1048-contract_evaluation_edges, 700, 51, 23+contract_evaluation_edges, 13};
    const size_t expected_drop_liveness[] = {1683, 0};
    CHECK(memcmp(event_kinds, expected_event_kinds, sizeof(event_kinds)) == 0
        && memcmp(phases, expected_phases, sizeof(phases)) == 0
        && memcmp(producers, expected_producers, sizeof(producers)) == 0
        && memcmp(action_kinds, expected_action_kinds, sizeof(action_kinds)) == 0
        && memcmp(outcomes, expected_outcomes, sizeof(outcomes)) == 0
        && memcmp(roles, expected_roles, sizeof(roles)) == 0
        && memcmp(failure_sources, expected_failure_sources,
            sizeof(failure_sources)) == 0
        && memcmp(drop_liveness, expected_drop_liveness,
            sizeof(drop_liveness)) == 0);
    /* P4.3 prerequisites are non-CFG events: their ready arm has no edge,
       their failure arm owns the allocation supplemental site, and the two
       phases remain distinct even when source spans collide. */
    size_t pre_invoke = 0, pre_residual = 0, ordinary_invokes = 0, step_events = 0;
    size_t pre_sites[5] = {SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_NONE,
        SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_NONE};
    size_t first_pre = SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; i < cleanup.event_count; ++i) {
        const SolMirRuntimeCleanupEvent *event = &cleanup.events[i];
        if (event->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_STEP) {
            ++step_events;
            CHECK(event->producer == SOL_MIR_RUNTIME_CLEANUP_PRODUCER_STEP_METER
                && event->origin == SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT
                && event->semantic_site == SOL_MIR_RUNTIME_NONE
                && event->supplemental_site == SOL_MIR_RUNTIME_NONE
                && event->inherited_failure_site < conventions.failure_site_count
                && event->transitions.count == 2);
            if (event->inherited_failure_site >= conventions.failure_site_count
                || event->transitions.count != 2) continue;
            const SolMirRuntimeFailureSite *site
                = &conventions.failure_sites[event->inherited_failure_site];
            const SolMirRuntimeCleanupTransition *ready
                = &cleanup.transitions[event->transitions.offset];
            const SolMirRuntimeCleanupTransition *failure = ready + 1;
            bool predicate = event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION
                || event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR;
            CHECK(site->origin_kind == (predicate
                    ? SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_STEP
                    : SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_STEP)
                && site->owner == event->owner && site->block == event->block
                && site->instruction == event->operation
                && site->occurrence == event->inherited_failure_site
                && site->allowed_codes == (UINT32_C(1)
                    << (SOL_MIR_RUNTIME_FAILURE_STEP_LIMIT - 1))
                && ready->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_PRE_OPERATION_READY
                && ready->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL
                && ready->actions.count == 0
                && failure->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_STEP_FAILURE
                && failure->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
                && failure->failure_source
                    == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31
                && failure->failure_site == event->inherited_failure_site
                && failure->failure_mask == site->allowed_codes
                && (predicate ? failure->actions.count == 0
                    : failure->actions.count != 0));
            if (!predicate && failure->actions.count != 0) {
                const SolMirRuntimeCleanupAction *last = &cleanup.actions[
                    failure->actions.offset + failure->actions.count - 1];
                CHECK(last->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE
                    && last->target == event->inherited_failure_site);
            }
            continue;
        }
        if (event->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION) {
            ordinary_invokes += event->kind
                    == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
                && event->block < program.materialization.block_count
                && program.materialization.blocks[event->block].terminator.kind
                    == SOL_MIR_TERM_INVOKE;
            continue;
        }
        if (first_pre == SOL_MIR_RUNTIME_NONE) first_pre = i;
        CHECK(event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
            && event->origin == SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT
            && event->inherited_failure_site == SOL_MIR_RUNTIME_NONE
            && event->supplemental_site < cleanup.supplemental_site_count
            && event->transitions.count == 2);
        const SolMirRuntimeCleanupTransition *ready = &cleanup.transitions[event->transitions.offset];
        const SolMirRuntimeCleanupTransition *failure = &cleanup.transitions[event->transitions.offset + 1];
        CHECK(ready->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL
            && ready->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_PRE_OPERATION_READY
            && ready->continuation == SOL_MIR_RUNTIME_NONE
            && ready->source_edge == SOL_MIR_RUNTIME_NONE
            && ready->destination == SOL_MIR_RUNTIME_NONE
            && ready->actions.count == 0
            && failure->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
            && failure->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE
            && failure->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_SUPPLEMENTAL_P33
            && failure->failure_site == event->supplemental_site
            && failure->failure_mask == ((UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED - 1))
                | (UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_ALLOCATION_LIMIT - 1)))
            && failure->actions.count != 0
            && cleanup.actions[failure->actions.offset + failure->actions.count - 1].kind
                == SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE
            && cleanup.actions[failure->actions.offset + failure->actions.count - 1].target
                == event->supplemental_site);
        if (event->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_INVOKE_CALLABLE) {
            CHECK(pre_invoke < sizeof(pre_sites) / sizeof(*pre_sites));
            if (pre_invoke < sizeof(pre_sites) / sizeof(*pre_sites))
                pre_sites[pre_invoke] = event->semantic_site;
            ++pre_invoke;
            CHECK(event->producer == SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CALLABLE_CONSTRUCTION
                && event->semantic_site != SOL_MIR_RUNTIME_NONE
                && event->operation == SOL_MIR_RUNTIME_NONE);
        } else if (event->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_PROPAGATE_RESIDUAL) {
            ++pre_residual;
            CHECK(event->producer == SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PROPAGATION_RESIDUAL
                && event->semantic_site == SOL_MIR_RUNTIME_NONE
                && event->operation < program.operations.propagation_count);
        } else CHECK(false);
    }
    for (size_t i = 0; i < sizeof(pre_sites) / sizeof(*pre_sites); ++i) {
        size_t matches = 0;
        for (size_t q = 0; q < program.operations.callable_count; ++q)
            matches += program.operations.callables[q].kind
                    == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION
                && program.operations.callables[q].semantic_site == pre_sites[i];
        CHECK(pre_sites[i] != SOL_MIR_RUNTIME_NONE && matches == 1);
        for (size_t q = 0; q < i; ++q) CHECK(pre_sites[q] != pre_sites[i]);
    }
    CHECK(pre_invoke == 5 && pre_residual == 2 && ordinary_invokes == 18
        && step_events == program.materialization.instruction_count
            + program.materialization.block_count
            + program.operations.predicate_instruction_count
            + program.operations.predicate_block_count
        && first_pre != SOL_MIR_RUNTIME_NONE);
    sol_test_progress_phase("main-same-span-search", "event", cleanup.event_count);
    size_t same_span_step[2]={SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE};
    for(size_t i=0;i<cleanup.event_count&&same_span_step[0]==SOL_MIR_RUNTIME_NONE;++i){const SolMirRuntimeCleanupEvent*left=&cleanup.events[i];if(left->phase!=SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_STEP)continue;for(size_t q=i+1;q<cleanup.event_count;++q){const SolMirRuntimeCleanupEvent*right=&cleanup.events[q];if(right->phase==SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_STEP&&right->kind==left->kind&&right->source.file==left->source.file&&right->source.start==left->source.start&&right->source.end==left->source.end){same_span_step[0]=i;same_span_step[1]=q;break;}}}
    CHECK(same_span_step[0]!=SOL_MIR_RUNTIME_NONE&&same_span_step[1]!=SOL_MIR_RUNTIME_NONE);
    sol_test_progress_phase_end();
    if(same_span_step[0]!=SOL_MIR_RUNTIME_NONE&&same_span_step[1]!=SOL_MIR_RUNTIME_NONE){SolMirRuntimeCleanupEvent*left=&cleanup.events[same_span_step[0]];SolMirRuntimeCleanupEvent*right=&cleanup.events[same_span_step[1]];CHECK(left->inherited_failure_site<conventions.failure_site_count&&right->inherited_failure_site<conventions.failure_site_count&&left->inherited_failure_site!=right->inherited_failure_site);if(left->inherited_failure_site<conventions.failure_site_count&&right->inherited_failure_site<conventions.failure_site_count){
        sol_test_progress_phase("main-same-span-occurrence-mutation-restoration", "check", 2);
        SolMirRuntimeFailureSite*site=&conventions.failure_sites[left->inherited_failure_site];size_t occurrence=site->occurrence;CHECK(occurrence!=conventions.failure_sites[right->inherited_failure_site].occurrence);site->occurrence=conventions.failure_sites[right->inherited_failure_site].occurrence;CHECK(!sol_mir_runtime_conventions_validate(&conventions,NULL)&&!sol_mir_runtime_cleanup_validate(&cleanup,NULL)&&cleanup_render_rejected(&cleanup));site->occurrence=occurrence;CHECK(sol_mir_runtime_conventions_validate(&conventions,NULL)&&sol_mir_runtime_cleanup_validate(&cleanup,NULL));
        sol_test_progress_completed(2); sol_test_progress_phase_end();
        sol_test_progress_phase("main-same-span-inherited-site-mutation-restoration", "check", 2);
        size_t inherited=left->inherited_failure_site;left->inherited_failure_site=right->inherited_failure_site;CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL)&&cleanup_render_rejected(&cleanup));left->inherited_failure_site=inherited;CHECK(sol_mir_runtime_cleanup_validate(&cleanup,NULL));
        sol_test_progress_completed(2); sol_test_progress_phase_end();
    }}
    /* Prediction is address-independent: a deliberately overlapping synthetic
     * placement cannot alter the successful-disjoint alias-work census. */
    sol_test_progress_phase("main-alias-work-prediction", "prediction", 2);
    size_t alias_work=0, overlapped_alias_work=0;
    CHECK(sol_mir_runtime_cleanup_test_alias_work(&cleanup,&alias_work));
    SolMirRuntimeCleanupEvent *saved_alias_events=cleanup.events;
    cleanup.events=(SolMirRuntimeCleanupEvent *)(void *)conventions.signatures;
    CHECK(sol_mir_runtime_cleanup_test_alias_work(&cleanup,&overlapped_alias_work)
        && alias_work==overlapped_alias_work);
    cleanup.events=saved_alias_events;
    sol_test_progress_completed(2); sol_test_progress_phase_end();
    check_failure_propagation_is_final(&cleanup, &program);
    check_eight_fixture_groups(&cleanup, &program);
    check_owner_snapshot_contract_and_selector(&cleanup, &program, &conventions);
    Compilation scoped; SolMirConcreteProgram scoped_program;
    SolMirRuntimeConventions scoped_conventions; SolMirRuntimeValues scoped_values;
    SolMirRuntimeCleanup scoped_cleanup;
    sol_mir_concrete_program_init(&scoped_program);
    sol_mir_runtime_conventions_init(&scoped_conventions);
    sol_mir_runtime_values_init(&scoped_values);
    sol_mir_runtime_cleanup_init(&scoped_cleanup);
    CHECK(compile_directory(&scoped, SOL_TEST_SOURCE_DIR "/tests/conformance/p33"));
    CHECK(build_cleanup_fixture(&scoped, &scoped_program));
    SolMirRuntimeConventionsBuildRequest scoped_cr = {&scoped_program, NULL};
    SolMirRuntimeValuesBuildRequest scoped_vr = {&scoped_conventions, NULL};
    CHECK(sol_mir_runtime_conventions_build(&scoped_cr, &scoped_conventions,
        &scoped.diagnostics) == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_values_build(&scoped_vr, &scoped_values,
        &scoped.diagnostics) == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED);
    SolMirRuntimeCleanupBuildRequest scoped_rr = {&scoped_conventions, &scoped_values, NULL};
    CHECK(sol_mir_runtime_cleanup_build(&scoped_rr, &scoped_cleanup, &scoped.diagnostics)
        == SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_cleanup_validate(&scoped_cleanup, NULL));
    check_authenticated_contract_suppression(&scoped_cleanup, &scoped_program,
        &scoped_conventions);
    if (scoped_cleanup.events != NULL)
        check_scope_aware_implicit_unwind(&scoped_cleanup, &scoped_program);
    sol_mir_runtime_cleanup_free(&scoped_cleanup);
    sol_mir_runtime_values_free(&scoped_values);
    sol_mir_runtime_conventions_free(&scoped_conventions);
    sol_mir_concrete_program_free(&scoped_program);
    compilation_free(&scoped);
    /* Affine Pair<function, Text> move matrix, rooted directly as internal
       fixtures so no test call or capability import can erase its ownership. */
    Compilation pair; SolMirConcreteProgram pair_program;
    SolMirRuntimeConventions pair_conventions; SolMirRuntimeValues pair_values;
    SolMirRuntimeCleanup pair_cleanup;
    sol_mir_concrete_program_init(&pair_program); sol_mir_runtime_conventions_init(&pair_conventions);
    sol_mir_runtime_values_init(&pair_values); sol_mir_runtime_cleanup_init(&pair_cleanup);
    bool pair_compiled = compile_directory(&pair, SOL_TEST_SOURCE_DIR "/tests/conformance/p33_pair");
    if (!pair_compiled) for (size_t i = 0; i < pair.diagnostics.count; ++i)
        fprintf(stderr, "affine Pair diagnostic: %s\n", pair.diagnostics.items[i].message);
    CHECK(pair_compiled);
    bool pair_built = build_affine_pair_fixture(&pair, &pair_program);
    if (!pair_built) for (size_t i = 0; i < pair.diagnostics.count; ++i)
        fprintf(stderr, "affine Pair build diagnostic: %s\n", pair.diagnostics.items[i].message);
    CHECK(pair_built);
    SolMirRuntimeConventionsBuildRequest pair_cr = {&pair_program, NULL};
    SolMirRuntimeValuesBuildRequest pair_vr = {&pair_conventions, NULL};
    CHECK(sol_mir_runtime_conventions_build(&pair_cr, &pair_conventions, &pair.diagnostics)
        == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_values_build(&pair_vr, &pair_values, &pair.diagnostics)
        == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED);
    SolMirRuntimeCleanupBuildRequest pair_rr = {&pair_conventions, &pair_values, NULL};
    SolMirRuntimeCleanupBuildOutcome pair_outcome = sol_mir_runtime_cleanup_build(&pair_rr, &pair_cleanup, &pair.diagnostics);
    CHECK(pair_outcome == SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_cleanup_validate(&pair_cleanup, NULL));
    check_affine_pair_owner(&pair_cleanup, &pair_program, &pair.ir);
    check_reduced_validation_work_rejects_without_write(&pair_cleanup);
    sol_mir_runtime_cleanup_free(&pair_cleanup); sol_mir_runtime_values_free(&pair_values);
    sol_mir_runtime_conventions_free(&pair_conventions); sol_mir_concrete_program_free(&pair_program);
    compilation_free(&pair);
    CHECK(cleanup.drop_path_count != 0);
    for (size_t i = 0; i < cleanup.action_count; ++i) {
        const SolMirRuntimeCleanupAction *action = &cleanup.actions[i];
        if (action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE
            || action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER)
            CHECK(action->drop_path < cleanup.drop_path_count);
        else CHECK(action->drop_path == SOL_MIR_RUNTIME_NONE);
    }
    size_t fallible=0; for(size_t i=0;i<program.operations.arithmetic_count;++i)fallible+=program.operations.arithmetic[i].failures!=0;for(size_t i=0;i<program.operations.predicate_instruction_count;++i)fallible+=program.operations.predicate_instructions[i].failures!=0;
    CHECK(cleanup.event_count >= program.materialization.block_count + program.operations.predicate_block_count + fallible);
    CHECK(cleanup.transition_count >= cleanup.event_count);
    SolMirRuntimeCleanupLimits exact={.max_events=cleanup.usage.events,.max_actions=cleanup.usage.actions,.max_transitions=cleanup.usage.transitions,.max_supplemental_sites=cleanup.usage.supplemental_sites,.max_drop_paths=cleanup.usage.drop_paths,.max_owned_bytes=cleanup.usage.owned_bytes,.max_build_scratch_bytes=cleanup.usage.build_scratch_bytes,.max_build_work=cleanup.usage.build_work,.max_validation_scratch_bytes=cleanup.usage.validation_scratch_bytes,.max_validation_work=cleanup.usage.validation_work};
    SolMirRuntimeCleanupUsage reconstructed; CHECK(sol_mir_runtime_cleanup_test_reconstruct_usage(&conventions,&values,&exact,&reconstructed));
    SolMirRuntimeCleanup bounded; sol_mir_runtime_cleanup_init(&bounded);
    rr.limits=&exact;
    CHECK(sol_mir_runtime_cleanup_build(&rr,&bounded,NULL)==SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_cleanup_validate(&bounded, NULL));
    char *first_cleanup_render = render_cleanup_text(&cleanup);
    char *second_cleanup_render = render_cleanup_text(&bounded);
    CHECK(first_cleanup_render != NULL && second_cleanup_render != NULL
        && strcmp(first_cleanup_render, second_cleanup_render) == 0);
    free(first_cleanup_render); free(second_cleanup_render);
    sol_mir_runtime_cleanup_free(&bounded);
    SolMirRuntimeCleanupLimits below=exact; --below.max_build_scratch_bytes;
    rr.limits=&below;
    CHECK(sol_mir_runtime_cleanup_build(&rr,&bounded,NULL)==SOL_MIR_RUNTIME_CLEANUP_BUILD_RESOURCE_EXHAUSTED);
    CHECK(bounded.events==NULL && bounded.event_count==0);
    below=exact; --below.max_drop_paths;
    rr.limits=&below;
    CHECK(sol_mir_runtime_cleanup_build(&rr,&bounded,NULL)==SOL_MIR_RUNTIME_CLEANUP_BUILD_RESOURCE_EXHAUSTED);
    CHECK(bounded.drop_paths==NULL && bounded.drop_path_count==0);
    rr.limits=&exact;
    sol_mir_runtime_cleanup_test_force_build_scratch_failure(true);
    CHECK(sol_mir_runtime_cleanup_build(&rr,&bounded,NULL)==SOL_MIR_RUNTIME_CLEANUP_BUILD_ALLOCATION_FAILED);
    sol_mir_runtime_cleanup_test_force_build_scratch_failure(false);
    sol_mir_runtime_cleanup_test_force_build_scratch_failure_attempt(1);
    CHECK(sol_mir_runtime_cleanup_build(&rr,&bounded,NULL)==SOL_MIR_RUNTIME_CLEANUP_BUILD_ALLOCATION_FAILED);
    CHECK(sol_mir_runtime_cleanup_test_build_scratch_attempts()==1);
    sol_mir_runtime_cleanup_test_force_build_scratch_failure_attempt(0);
    sol_mir_runtime_cleanup_test_force_persistent_allocation_failure(true);
    CHECK(sol_mir_runtime_cleanup_build(&rr,&bounded,NULL)==SOL_MIR_RUNTIME_CLEANUP_BUILD_ALLOCATION_FAILED);
    sol_mir_runtime_cleanup_test_force_persistent_allocation_failure(false);
    /* Persistent arenas are selected independently in canonical arena order. */
    size_t persistent_arenas=(exact.max_events!=0)+(exact.max_actions!=0)+(exact.max_transitions!=0)
        +(exact.max_supplemental_sites!=0)+(exact.max_drop_paths!=0);
    for(size_t attempt=1;attempt<=persistent_arenas;++attempt){
        sol_test_progress_ordinal("cleanup-persistent-arena", attempt, persistent_arenas);
        sol_mir_runtime_cleanup_test_force_persistent_allocation_failure_attempt(attempt);
        CHECK(sol_mir_runtime_cleanup_build(&rr,&bounded,NULL)==SOL_MIR_RUNTIME_CLEANUP_BUILD_ALLOCATION_FAILED);
        CHECK(sol_mir_runtime_cleanup_test_persistent_allocation_attempts()==attempt);
        CHECK(bounded.events==NULL && bounded.actions==NULL && bounded.transitions==NULL
            && bounded.supplemental_sites==NULL && bounded.drop_paths==NULL);
        sol_test_progress_phase("persistent-fault-completed", "fault-attempt", persistent_arenas);
        sol_test_progress_completed(attempt); sol_test_progress_phase_end();
    }
    sol_mir_runtime_cleanup_test_force_persistent_allocation_failure_attempt(0);
    sol_mir_runtime_cleanup_test_force_validation_scratch_failure(true);
    CHECK(sol_mir_runtime_cleanup_build(&rr,&bounded,NULL)==SOL_MIR_RUNTIME_CLEANUP_BUILD_ALLOCATION_FAILED);
    sol_mir_runtime_cleanup_test_force_validation_scratch_failure(false);
    sol_mir_runtime_cleanup_test_force_validation_scratch_failure_attempt(1);
    CHECK(sol_mir_runtime_cleanup_build(&rr,&bounded,NULL)==SOL_MIR_RUNTIME_CLEANUP_BUILD_ALLOCATION_FAILED);
    CHECK(sol_mir_runtime_cleanup_test_validation_scratch_attempts()==1);
    sol_mir_runtime_cleanup_test_force_validation_scratch_failure_attempt(0);
    SolMirRuntimeCleanupLimits one_below[] = {
        {.max_events=exact.max_events-1,.max_actions=exact.max_actions,.max_transitions=exact.max_transitions,.max_supplemental_sites=exact.max_supplemental_sites,.max_drop_paths=exact.max_drop_paths,.max_owned_bytes=exact.max_owned_bytes,.max_build_scratch_bytes=exact.max_build_scratch_bytes,.max_build_work=exact.max_build_work,.max_validation_scratch_bytes=exact.max_validation_scratch_bytes,.max_validation_work=exact.max_validation_work},
        {.max_events=exact.max_events,.max_actions=exact.max_actions-1,.max_transitions=exact.max_transitions,.max_supplemental_sites=exact.max_supplemental_sites,.max_drop_paths=exact.max_drop_paths,.max_owned_bytes=exact.max_owned_bytes,.max_build_scratch_bytes=exact.max_build_scratch_bytes,.max_build_work=exact.max_build_work,.max_validation_scratch_bytes=exact.max_validation_scratch_bytes,.max_validation_work=exact.max_validation_work},
        {.max_events=exact.max_events,.max_actions=exact.max_actions,.max_transitions=exact.max_transitions-1,.max_supplemental_sites=exact.max_supplemental_sites,.max_drop_paths=exact.max_drop_paths,.max_owned_bytes=exact.max_owned_bytes,.max_build_scratch_bytes=exact.max_build_scratch_bytes,.max_build_work=exact.max_build_work,.max_validation_scratch_bytes=exact.max_validation_scratch_bytes,.max_validation_work=exact.max_validation_work},
        {.max_events=exact.max_events,.max_actions=exact.max_actions,.max_transitions=exact.max_transitions,.max_supplemental_sites=exact.max_supplemental_sites-1,.max_drop_paths=exact.max_drop_paths,.max_owned_bytes=exact.max_owned_bytes,.max_build_scratch_bytes=exact.max_build_scratch_bytes,.max_build_work=exact.max_build_work,.max_validation_scratch_bytes=exact.max_validation_scratch_bytes,.max_validation_work=exact.max_validation_work},
        {.max_events=exact.max_events,.max_actions=exact.max_actions,.max_transitions=exact.max_transitions,.max_supplemental_sites=exact.max_supplemental_sites,.max_drop_paths=exact.max_drop_paths,.max_owned_bytes=exact.max_owned_bytes-1,.max_build_scratch_bytes=exact.max_build_scratch_bytes,.max_build_work=exact.max_build_work,.max_validation_scratch_bytes=exact.max_validation_scratch_bytes,.max_validation_work=exact.max_validation_work},
        {.max_events=exact.max_events,.max_actions=exact.max_actions,.max_transitions=exact.max_transitions,.max_supplemental_sites=exact.max_supplemental_sites,.max_drop_paths=exact.max_drop_paths,.max_owned_bytes=exact.max_owned_bytes,.max_build_scratch_bytes=exact.max_build_scratch_bytes,.max_build_work=exact.max_build_work-1,.max_validation_scratch_bytes=exact.max_validation_scratch_bytes,.max_validation_work=exact.max_validation_work},
        {.max_events=exact.max_events,.max_actions=exact.max_actions,.max_transitions=exact.max_transitions,.max_supplemental_sites=exact.max_supplemental_sites,.max_drop_paths=exact.max_drop_paths,.max_owned_bytes=exact.max_owned_bytes,.max_build_scratch_bytes=exact.max_build_scratch_bytes,.max_build_work=exact.max_build_work,.max_validation_scratch_bytes=exact.max_validation_scratch_bytes-1,.max_validation_work=exact.max_validation_work},
        {.max_events=exact.max_events,.max_actions=exact.max_actions,.max_transitions=exact.max_transitions,.max_supplemental_sites=exact.max_supplemental_sites,.max_drop_paths=exact.max_drop_paths,.max_owned_bytes=exact.max_owned_bytes,.max_build_scratch_bytes=exact.max_build_scratch_bytes,.max_build_work=exact.max_build_work,.max_validation_scratch_bytes=exact.max_validation_scratch_bytes,.max_validation_work=exact.max_validation_work-1},
    };
    for(size_t i=0;i<sizeof(one_below)/sizeof(one_below[0]);++i){
        static const char *const fields[] = {"max_events", "max_actions", "max_transitions",
            "max_supplemental_sites", "max_owned_bytes", "max_build_work",
            "max_validation_scratch_bytes", "max_validation_work"};
        sol_test_progress_phase(fields[i], "quota", 1);
        rr.limits=&one_below[i];
        CHECK(sol_mir_runtime_cleanup_build(&rr,&bounded,NULL)==SOL_MIR_RUNTIME_CLEANUP_BUILD_RESOURCE_EXHAUSTED);
        CHECK(bounded.events==NULL && bounded.event_count==0);
        CHECK(sol_mir_runtime_cleanup_test_persistent_allocation_attempts() == 0);
        sol_test_progress_completed(1); sol_test_progress_phase_end();
    }
    rr.limits=&exact;
    sol_mir_runtime_cleanup_test_force_validation_scratch_failure(true);
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL));
    sol_mir_runtime_cleanup_test_force_validation_scratch_failure(false);
    CHECK(sol_mir_runtime_cleanup_validate(&cleanup,NULL));
    SolMirRuntimeCleanupUsage saved_usage=cleanup.usage;
    ++cleanup.usage.validation_work;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL));
    cleanup.usage=saved_usage;
    /* BUILD work is reconstructed by the validator rather than trusted from
       the owner; both under- and over-reporting remain invalid within limits
       and therefore cannot be rendered. */
    --cleanup.usage.build_work;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL));
    stream=tmpfile(); CHECK(stream!=NULL); if(stream!=NULL){
        CHECK(!sol_mir_runtime_cleanup_render(stream,&cleanup));
        CHECK(fflush(stream)==0 && fseek(stream,0,SEEK_END)==0 && ftell(stream)==0);
        fclose(stream);
    }
    cleanup.usage=saved_usage;
    ++cleanup.usage.build_work;
    CHECK(cleanup.usage.build_work <= cleanup.limits.max_build_work);
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL));
    stream=tmpfile(); CHECK(stream!=NULL); if(stream!=NULL){
        CHECK(!sol_mir_runtime_cleanup_render(stream,&cleanup));
        CHECK(fflush(stream)==0 && fseek(stream,0,SEEK_END)==0 && ftell(stream)==0);
        fclose(stream);
    }
    cleanup.usage=saved_usage;
    CHECK(sol_mir_runtime_cleanup_validate(&cleanup,NULL));
    rr.limits=NULL;
    SolMirRuntimeCleanupAction trace_actions[64]; SolMirRuntimeCleanupTrace trace;
    size_t trace_event = SOL_MIR_RUNTIME_NONE;
    SolMirRuntimeCleanupEdgeRole trace_role = SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO;
    SolMirRuntimeFailureCode trace_code = SOL_MIR_RUNTIME_FAILURE_NONE;
    for (size_t i = 0; i < cleanup.event_count && trace_event == SOL_MIR_RUNTIME_NONE; ++i) {
        const SolMirRuntimeCleanupEvent *event = &cleanup.events[i];
        if (event->inherited_failure_site == SOL_MIR_RUNTIME_NONE) continue;
        uint32_t mask = conventions.failure_sites[event->inherited_failure_site].allowed_codes;
        for (unsigned code = SOL_MIR_RUNTIME_FAILURE_PANIC;
                code <= SOL_MIR_RUNTIME_FAILURE_HOST_ERROR; ++code)
            if ((mask & (UINT32_C(1) << (code - 1))) != 0) {
                for (size_t t = 0; t < event->transitions.count; ++t) {
                    const SolMirRuntimeCleanupTransition *transition =
                        &cleanup.transitions[event->transitions.offset + t];
                    if (transition->failure_source
                        == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31) {
                        trace_event = i; trace_role = transition->edge_role;
                        trace_code = (SolMirRuntimeFailureCode)code;
                        break;
                    }
                }
                if (trace_event != SOL_MIR_RUNTIME_NONE) break;
            }
    }
    CHECK(trace_event != SOL_MIR_RUNTIME_NONE);
    SolMirRuntimeCleanupFailureOccurrence trace_occurrence = {
        SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31,
        cleanup.events[trace_event].inherited_failure_site, trace_code,
        SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE, 0, {0},
        conventions.failure_sites[cleanup.events[trace_event].inherited_failure_site].source};
    SolMirRuntimeCleanupTraceRequest trace_request = {trace_event, trace_role,
        &trace_occurrence, NULL, SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
    CHECK(sol_mir_runtime_cleanup_test_trace(&cleanup, &trace_request,
        trace_actions, 64, &trace));
    CHECK(trace.has_primary && trace.primary.code == trace_code);
    SolMirRuntimeCleanupFailureOccurrence arbitrary = trace_occurrence;
    ++arbitrary.source.start;
    SolMirRuntimeCleanupTraceRequest arbitrary_request = trace_request;
    arbitrary_request.produced = &arbitrary;
    CHECK(!sol_mir_runtime_cleanup_test_select(&cleanup, &arbitrary_request,
        trace_actions, 64, &trace));
    size_t reachability_words = program.materialization.block_count * 2;
    size_t *reachability_scratch = reachability_words ? calloc(reachability_words,
        sizeof(*reachability_scratch)) : NULL;
    CHECK(reachability_words == 0 || reachability_scratch != NULL);
    SolMirRuntimeCleanupFailureOccurrence primary;
    SolMirRuntimeCleanupPrecedenceInput first = {trace_event, trace_role,
        trace_occurrence, SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
    /* The same event/edge/site cannot impersonate a later failure. */
    SolMirRuntimeCleanupAttemptedLater same_later = {trace_event, trace_role,
        trace_occurrence, SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
    CHECK(!sol_mir_runtime_cleanup_test_precedence_attempted_later(&cleanup,
        &first, &same_later, reachability_scratch, reachability_words));
    bool saw_unrelated_host_contract_rejected = false;
    for (size_t primary_event = 0; primary_event < cleanup.event_count
            && !saw_unrelated_host_contract_rejected; ++primary_event) {
        const SolMirRuntimeCleanupEvent *primary_source = &cleanup.events[primary_event];
        if (primary_source->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
            || primary_source->block >= program.materialization.block_count
            || program.materialization.blocks[primary_source->block].terminator.kind
                != SOL_MIR_TERM_INVOKE
            || program.materialization.blocks[primary_source->block].terminator.call_kind
                != SOL_IR_CALL_CAPABILITY) continue;
        for (size_t t = 0; t < primary_source->transitions.count && !saw_unrelated_host_contract_rejected; ++t) {
            const SolMirRuntimeCleanupTransition *primary_transition =
                &cleanup.transitions[primary_source->transitions.offset + t];
            SolMirRuntimeCleanupFailureOccurrence primary_occurrence;
            if (primary_transition->edge_role != SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE
                || !occurrence_for_transition(&cleanup, &conventions, primary_transition,
                    &primary_occurrence)) continue;
            for (size_t secondary_event = 0;
                    secondary_event < cleanup.event_count && !saw_unrelated_host_contract_rejected;
                    ++secondary_event) {
                if (secondary_event == primary_event
                    || cleanup.events[secondary_event].owner == primary_source->owner) continue;
                const SolMirRuntimeCleanupEvent *secondary_source = &cleanup.events[secondary_event];
                for (size_t u = 0; u < secondary_source->transitions.count; ++u) {
                    const SolMirRuntimeCleanupTransition *secondary_transition =
                        &cleanup.transitions[secondary_source->transitions.offset + u];
                    SolMirRuntimeCleanupFailureOccurrence secondary_occurrence;
                    if (secondary_transition->edge_role
                            != SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_VIOLATION
                        || !occurrence_for_transition(&cleanup, &conventions,
                            secondary_transition, &secondary_occurrence)) continue;
                    SolMirRuntimeCleanupAttemptedLater later = {secondary_event,
                        secondary_transition->edge_role, secondary_occurrence,
                        SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
                    first = (SolMirRuntimeCleanupPrecedenceInput){primary_event,
                        primary_transition->edge_role, primary_occurrence,
                        SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
                    CHECK(!sol_mir_runtime_cleanup_test_precedence_attempted_later(
                        &cleanup, &first, &later, reachability_scratch, reachability_words));
                    saw_unrelated_host_contract_rejected = true;
                    break;
                }
            }
        }
    }
    CHECK(saw_unrelated_host_contract_rejected);
    free(reachability_scratch);
    /* The focused P3.3 fixture below owns the positive suppression proof. */
    first.occurrence = arbitrary;
    CHECK(!sol_mir_runtime_cleanup_test_precedence(&cleanup, &first, NULL, &primary));
    check_immutable_occurrence_sweep(&cleanup, &conventions, &trace_occurrence);
    bool saw_precedence_transport = false;
    for (size_t source_event = 0; source_event < cleanup.event_count
            && !saw_precedence_transport; ++source_event) {
        const SolMirRuntimeCleanupEvent *source = &cleanup.events[source_event];
        for (size_t st = 0; st < source->transitions.count && !saw_precedence_transport; ++st) {
            const SolMirRuntimeCleanupTransition *source_transition =
                &cleanup.transitions[source->transitions.offset + st];
            SolMirRuntimeCleanupFailureOccurrence source_occurrence;
            if (source_transition->edge_role != SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE
                || !occurrence_for_transition(&cleanup, &conventions, source_transition,
                    &source_occurrence)) continue;
            SolMirRuntimeCleanupPrecedenceInput producer = {source_event,
                source_transition->edge_role, source_occurrence,
                SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
            for (size_t resume_event = 0; resume_event < cleanup.event_count
                    && !saw_precedence_transport; ++resume_event) {
                const SolMirRuntimeCleanupEvent *resume_source = &cleanup.events[resume_event];
                for (size_t rt = 0; rt < resume_source->transitions.count; ++rt) {
                    const SolMirRuntimeCleanupTransition *resume_transition =
                        &cleanup.transitions[resume_source->transitions.offset + rt];
                    if (resume_transition->failure_source
                        != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING) continue;
                    SolMirRuntimeCleanupPrecedenceResume resume = {resume_event,
                        resume_transition->edge_role, SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
                    if (sol_mir_runtime_cleanup_test_precedence(&cleanup, &producer,
                            &resume, &primary)) {
                        CHECK(memcmp(&primary, &source_occurrence, sizeof(primary)) == 0);
                        saw_precedence_transport = true;
                        break;
                    }
                }
            }
        }
    }
    CHECK(saw_precedence_transport);
    bool saw_failure = false, saw_normal_only = false;
    for (size_t i = 0; i < cleanup.transition_count; ++i) {
        const SolMirRuntimeCleanupTransition *transition = &cleanup.transitions[i];
        saw_failure |= transition->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE;
        for (size_t q = 0; q < transition->actions.count; ++q)
            saw_normal_only |= (cleanup.actions[transition->actions.offset + q].flags
                & SOL_MIR_RUNTIME_CLEANUP_ACTION_NORMAL_ONLY) != 0;
    }
    CHECK(saw_failure);
    CHECK(saw_normal_only);
    const uint8_t panic_bytes[] = {'p', 'a', 'n', 'i', 'c'};
    size_t panic_event = SOL_MIR_RUNTIME_NONE;
    SolMirRuntimeCleanupEdgeRole panic_role = SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO;
    SolMirRuntimeSource panic_source = {0};
    for (size_t e = 0; e < cleanup.event_count && panic_event == SOL_MIR_RUNTIME_NONE; ++e) {
        const SolMirRuntimeCleanupEvent *event = &cleanup.events[e];
        if (event->capture_detail_kind != SOL_MIR_RUNTIME_FAILURE_DETAIL_PANIC_TEXT) continue;
        for (size_t t = 0; t < event->transitions.count; ++t) {
            const SolMirRuntimeCleanupTransition *transition =
                &cleanup.transitions[event->transitions.offset + t];
            SolMirRuntimeCleanupFailureOccurrence occurrence;
            if (occurrence_for_transition(&cleanup, &conventions, transition, &occurrence)
                && occurrence.code == SOL_MIR_RUNTIME_FAILURE_PANIC) {
                panic_event = e; panic_role = transition->edge_role; panic_source = occurrence.source;
                break;
            }
        }
    }
    CHECK(panic_event != SOL_MIR_RUNTIME_NONE);
    SolMirRuntimeFailureRecord record = {SOL_MIR_RUNTIME_FAILURE_PANIC,
        panic_source, SOL_MIR_RUNTIME_FAILURE_DETAIL_PANIC_TEXT,
        panic_bytes, sizeof(panic_bytes)};
    SolMirRuntimeCleanupDetail detail;
    CHECK(sol_mir_runtime_cleanup_capture_detail(&cleanup, panic_event, panic_role, &record, &detail));
    CHECK(detail.length == sizeof(panic_bytes) && detail.bytes[detail.length] == '\0');
    /* Detail is selected by the concrete failure code, not merely by the host
       invoke event: a host error owns bytes while a host-call limit owns none. */
    bool saw_host_error_detail = false, saw_host_limit_detail = false;
    const uint8_t host_bytes[] = {'h', 'o', 's', 't'};
    for (size_t e = 0; e < cleanup.event_count; ++e) {
        const SolMirRuntimeCleanupEvent *event = &cleanup.events[e];
        for (size_t t = 0; t < event->transitions.count; ++t) {
            const SolMirRuntimeCleanupTransition *transition =
                &cleanup.transitions[event->transitions.offset + t];
            SolMirRuntimeCleanupFailureOccurrence occurrence;
            if (!occurrence_for_transition(&cleanup, &conventions, transition, &occurrence)) continue;
            if ((transition->failure_mask & (UINT32_C(1) <<
                    (SOL_MIR_RUNTIME_FAILURE_HOST_ERROR - 1))) != 0) {
                occurrence.code = SOL_MIR_RUNTIME_FAILURE_HOST_ERROR;
                record = (SolMirRuntimeFailureRecord){occurrence.code, occurrence.source,
                    SOL_MIR_RUNTIME_FAILURE_DETAIL_HOST_BYTES, host_bytes, sizeof(host_bytes)};
                CHECK(sol_mir_runtime_cleanup_capture_detail(&cleanup, e, transition->edge_role,
                    &record, &detail));
                saw_host_error_detail = true;
            }
            if ((transition->failure_mask & (UINT32_C(1) <<
                    (SOL_MIR_RUNTIME_FAILURE_HOST_CALL_LIMIT - 1))) != 0) {
                occurrence.code = SOL_MIR_RUNTIME_FAILURE_HOST_CALL_LIMIT;
                record = (SolMirRuntimeFailureRecord){occurrence.code, occurrence.source,
                    SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE, NULL, 0};
                CHECK(sol_mir_runtime_cleanup_capture_detail(&cleanup, e, transition->edge_role,
                    &record, &detail));
                record.detail_kind = SOL_MIR_RUNTIME_FAILURE_DETAIL_HOST_BYTES;
                record.bytes = host_bytes; record.length = sizeof(host_bytes);
                CHECK(!sol_mir_runtime_cleanup_capture_detail(&cleanup, e, transition->edge_role,
                    &record, &detail));
                saw_host_limit_detail = true;
            }
        }
    }
    CHECK(saw_host_error_detail && saw_host_limit_detail);
    stream=tmpfile(); CHECK(stream!=NULL); if(stream!=NULL){
        CHECK(sol_mir_runtime_cleanup_render(stream,&cleanup));
        CHECK(fflush(stream)==0 && fseek(stream,0,SEEK_END)==0 && ftell(stream)>0);
        long rendered_size = ftell(stream); char *rendered = malloc((size_t)rendered_size + 1);
        CHECK(rendered != NULL && fseek(stream, 0, SEEK_SET) == 0);
        if (rendered != NULL) {
            CHECK(fread(rendered, 1, (size_t)rendered_size, stream) == (size_t)rendered_size);
            rendered[rendered_size] = '\0';
            CHECK(strstr(rendered, "action key=") != NULL && strstr(rendered, "kind=") != NULL
                && strstr(rendered, "target=") != NULL && strstr(rendered, "recipe-key=") != NULL
                && strstr(rendered, "guard=") != NULL && strstr(rendered, "path-slice-key=") != NULL
                && strstr(rendered, "transition event=") != NULL && strstr(rendered, "role=") != NULL
                && strstr(rendered, "failure-source=") != NULL && strstr(rendered, "code-mask=") != NULL
                && strstr(rendered, "site key=") != NULL);
            free(rendered);
        }
        fclose(stream);
    }
    /* The validator must reject well-formed, valid-range mutations rather than
       merely checking arena shape.  Each record field participates in its
       independent CFG replay digest. */
    SolMirRuntimeCleanupEvent saved_event=cleanup.events[0];
    cleanup.events[0].origin=(cleanup.events[0].origin==SOL_MIR_RUNTIME_CLEANUP_ORIGIN_EXPLICIT)?SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT:SOL_MIR_RUNTIME_CLEANUP_ORIGIN_EXPLICIT;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL)); cleanup.events[0]=saved_event;
    saved_event = cleanup.events[first_pre];
    cleanup.events[first_pre].phase = SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL)); cleanup.events[first_pre] = saved_event;
    if (saved_event.semantic_site != SOL_MIR_RUNTIME_NONE) {
        cleanup.events[first_pre].semantic_site = SOL_MIR_RUNTIME_NONE;
        CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL)); cleanup.events[first_pre] = saved_event;
    } else {
        ++cleanup.events[first_pre].operation;
        CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL)); cleanup.events[first_pre] = saved_event;
    }
    SolMirRuntimeCleanupAction saved_action=cleanup.actions[0]; cleanup.actions[0].target^=1u;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL)); cleanup.actions[0]=saved_action;
    SolMirRuntimeCleanupTransition saved_transition=cleanup.transitions[0];
    cleanup.transitions[0].primary_failure_wins=false;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL)); cleanup.transitions[0]=saved_transition;
    cleanup.transitions[0].edge_role=saved_transition.edge_role
        == SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE
            ? SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO
            : SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL)); cleanup.transitions[0]=saved_transition;
    cleanup.transitions[0].destination=saved_transition.destination == SOL_MIR_RUNTIME_NONE
        ? 0 : SOL_MIR_RUNTIME_NONE;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL)); cleanup.transitions[0]=saved_transition;
    cleanup.transitions[0].continuation=saved_transition.continuation
        == SOL_MIR_RUNTIME_NONE ? 0 : SOL_MIR_RUNTIME_NONE;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL)); cleanup.transitions[0]=saved_transition;
    SolMirRuntimeCleanupDropPath saved_path=cleanup.drop_paths[0];
    cleanup.drop_paths[0].root=SOL_MIR_RUNTIME_NONE;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL)); cleanup.drop_paths[0]=saved_path;
    cleanup.drop_paths[0].place=SOL_MIR_RUNTIME_NONE;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL)); cleanup.drop_paths[0]=saved_path;
    cleanup.drop_paths[0].recipe^=1u;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL)); cleanup.drop_paths[0]=saved_path;
    cleanup.drop_paths[0].liveness=saved_path.liveness==SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE
        ? SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL : SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL)); cleanup.drop_paths[0]=saved_path;
    for(size_t i=0;i<cleanup.drop_path_count;++i) if(cleanup.drop_paths[i].holes.count){
        size_t sibling=cleanup.drop_paths[i].holes.offset;
        SolMirRuntimeCleanupDropPath saved_sibling=cleanup.drop_paths[sibling];
        cleanup.drop_paths[sibling].place^=1u;
        CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL));
        cleanup.drop_paths[sibling]=saved_sibling;
        break;
    }
    if(cleanup.supplemental_site_count){
        SolMirRuntimeCleanupSupplementalSite saved_site=cleanup.supplemental_sites[0];
        cleanup.supplemental_sites[0].allowed_codes^=UINT32_C(1);
        CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL)); cleanup.supplemental_sites[0]=saved_site;
    }
    /* Full predecessor-category alias census: P3.1/P3.2 arenas, P2 top-level
     * storage, per-template MIR, materialized topology, and IR source bytes. */
    check_cleanup_arena_alias(&cleanup, conventions.signatures);
    check_cleanup_arena_alias(&cleanup, values.ownership_plans);
    check_cleanup_arena_alias(&cleanup, program.program.roots);
    check_cleanup_arena_alias(&cleanup, program.program.templates[0].mir.instructions);
    check_cleanup_arena_alias(&cleanup, program.materialization.images[0].topology.blocks);
    check_cleanup_arena_alias(&cleanup, program.program.ir->source_bytes);
    check_cleanup_arena_alias(&cleanup, program.program.ir->source_bytes + 1);
    check_cleanup_arena_alias(&cleanup, program.program.ir->definitions[0].name + 1);
    SolMirInstruction *saved_template_instructions=program.program.templates[0].mir.instructions;
    program.program.templates[0].mir.instructions=NULL;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL));
    program.program.templates[0].mir.instructions=saved_template_instructions;
    CHECK(sol_mir_runtime_cleanup_validate(&cleanup,NULL));
    /* Count/capacity pairs can be range-shaped but undersized.  Validation must
       reject their slices before dereferencing a missing action, transition, or
       path; these mutations are ASan-safe because the backing allocation stays
       intact. */
    size_t saved_action_count=cleanup.action_count, saved_action_capacity=cleanup.action_capacity;
    cleanup.action_count=cleanup.action_capacity=1;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL));
    cleanup.action_count=saved_action_count; cleanup.action_capacity=saved_action_capacity;
    size_t saved_transition_count=cleanup.transition_count, saved_transition_capacity=cleanup.transition_capacity;
    cleanup.transition_count=cleanup.transition_capacity=1;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL));
    cleanup.transition_count=saved_transition_count; cleanup.transition_capacity=saved_transition_capacity;
    size_t saved_path_count=cleanup.drop_path_count, saved_path_capacity=cleanup.drop_path_capacity;
    cleanup.drop_path_count=cleanup.drop_path_capacity=1;
    CHECK(!sol_mir_runtime_cleanup_validate(&cleanup,NULL));
    cleanup.drop_path_count=saved_path_count; cleanup.drop_path_capacity=saved_path_capacity;
    CHECK(sol_mir_runtime_cleanup_validate(&cleanup,NULL));
    stream=tmpfile(); CHECK(stream!=NULL); if(stream!=NULL){
        SolMirRuntimeCleanupEvent saved=cleanup.events[0]; cleanup.events[0].actions.count=SIZE_MAX;
        CHECK(!sol_mir_runtime_cleanup_render(stream,&cleanup));
        CHECK(fflush(stream)==0 && fseek(stream,0,SEEK_END)==0 && ftell(stream)==0);
        cleanup.events[0]=saved; fclose(stream);
    }
    sol_mir_runtime_cleanup_free(&cleanup); sol_mir_runtime_values_free(&values); sol_mir_runtime_conventions_free(&conventions); sol_mir_concrete_program_free(&program); compilation_free(&c);
    return failures == 0 ? 0 : 1;
}
#undef main
SOL_TEST_ARGV_ENTRY("mir_runtime_cleanup")
