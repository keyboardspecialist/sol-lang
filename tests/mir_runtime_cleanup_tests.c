#define SOL_MIR_PLAN_TEST_HOOKS 1
#include "sol/mir_runtime_cleanup.h"
#include "sol/effects.h"
#include "sol/ownership.h"
#include "sol/package.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

typedef struct { SolDiagnostics diagnostics; SolHirModule hir; SolTypeTable types;
    SolEffectTable effects; SolContractTable contracts; SolIr ir; SolPackage package; } Compilation;
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
static void check_zero_supplemental_alias_preflight(void) {
    Compilation c; SolMirConcreteProgram program; SolMirRuntimeConventions conventions;
    SolMirRuntimeValues values; SolMirRuntimeCleanup cleanup, rebuilt;
    sol_mir_concrete_program_init(&program); sol_mir_runtime_conventions_init(&conventions);
    sol_mir_runtime_values_init(&values); sol_mir_runtime_cleanup_init(&cleanup);
    sol_mir_runtime_cleanup_init(&rebuilt);
    bool compiled = compile_directory(&c,
        SOL_TEST_SOURCE_DIR "/tests/conformance/p33_alias_zero");
    CHECK(compiled);
    SolIrCallableId choose = compiled
        ? callable(&c.ir, "choose", SOL_IR_CALLABLE_CAPABILITY) : SOL_IR_NONE;
    SolIrCallableId root = compiled
        ? callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION) : SOL_IR_NONE;
    SolMirProgramRoot fixture_root = {root, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    bool built = compiled && choose != SOL_IR_NONE && root != SOL_IR_NONE
        && sol_mir_concrete_program_build(&(SolMirConcreteBuildRequest){&c.ir,
            &fixture_root, 1, &choose, 1, &target, NULL}, &program, &c.diagnostics)
            == SOL_MIR_CONCRETE_BUILD_SUCCEEDED
        && sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&program,
            NULL}, &conventions, &c.diagnostics) == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED
        && sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&conventions,
            NULL}, &values, &c.diagnostics) == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED
        && sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&conventions,
            &values, NULL}, &cleanup, &c.diagnostics) == SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED;
    CHECK(built);
    if (built) {
        CHECK(cleanup.supplemental_site_count == 0
            && cleanup.supplemental_site_capacity == 0 && cleanup.supplemental_sites == NULL
            && sol_mir_runtime_cleanup_validate(&cleanup, NULL));
        cleanup.supplemental_sites = (SolMirRuntimeCleanupSupplementalSite *)cleanup.events;
        CHECK(!sol_mir_runtime_cleanup_validate(&cleanup, NULL));
        cleanup.supplemental_sites = NULL;
        CHECK(sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&conventions,
            &values, NULL}, &rebuilt, &c.diagnostics) == SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED
            && sol_mir_runtime_cleanup_validate(&rebuilt, NULL)
            && memcmp(&cleanup.usage, &rebuilt.usage, sizeof(cleanup.usage)) == 0);
    }
    sol_mir_runtime_cleanup_free(&rebuilt); sol_mir_runtime_cleanup_free(&cleanup);
    sol_mir_runtime_values_free(&values); sol_mir_runtime_conventions_free(&conventions);
    sol_mir_concrete_program_free(&program); compilation_free(&c);
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
static void check_local_or_pending(const SolMirRuntimeCleanup *cleanup,
    const SolMirRuntimeConventions *conventions, const SolMirConcreteProgram *program) {
    size_t event_id=SOL_MIR_RUNTIME_NONE; const SolMirRuntimeCleanupTransition *local=NULL;
    for(size_t i=0;i<cleanup->event_count&&!local;i++)for(size_t j=0;j<cleanup->events[i].transitions.count;j++){const SolMirRuntimeCleanupTransition*t=&cleanup->transitions[cleanup->events[i].transitions.offset+j];if(t->failure_source==SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_LOCAL_OR_PENDING){event_id=i;local=t;break;}}
    CHECK(local!=NULL); if(!local)return;
    const SolMirRuntimeCleanupEvent*event=&cleanup->events[event_id];
    CHECK(event->kind==SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR&&local->edge_role==SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE&&local->failure_site<conventions->call_count&&local->failure_site==event->inherited_failure_site&&local->failure_mask==(UINT32_C(1)<<(SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT-1))&&local->actions.count==0);
    const SolMirRuntimeCall*call=&conventions->calls[local->failure_site];
    CHECK(call->target_kind==SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL&&call->failure_site==local->failure_site&&call->failure_edge==local->continuation&&program->materialization.blocks[event->block].terminator.kind==SOL_MIR_TERM_INVOKE);
    char *first_render=render_cleanup_text(cleanup),*second_render=render_cleanup_text(cleanup);CHECK(first_render&&second_render&&!strcmp(first_render,second_render));if(first_render){char *line=strstr(first_render,"failure-source=local-or-pending failure-site-key=");CHECK(line!=NULL&&strstr(line,"failure-site-key=none")==NULL);}free(first_render);free(second_render);
    SolMirRuntimeCleanupFailureOccurrence produced={SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31,local->failure_site,SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT,SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE,0,{0},conventions->failure_sites[local->failure_site].source};
    SolMirRuntimeCleanupTrace trace; SolMirRuntimeCleanupTraceRequest request={event_id,local->edge_role,&produced,NULL,SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
    CHECK(sol_mir_runtime_cleanup_test_select(cleanup,&request,NULL,0,&trace)&&!memcmp(&trace.primary,&produced,sizeof produced));
    SolMirRuntimeFailureRecord record={SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT,produced.source,SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE,NULL,0}; SolMirRuntimeCleanupDetail detail;
    CHECK(sol_mir_runtime_cleanup_capture_detail(cleanup,event_id,local->edge_role,&record,&detail));
    SolMirRuntimeCleanupFailureOccurrence pending=produced; pending.code=SOL_MIR_RUNTIME_FAILURE_INTEGER_OVERFLOW; pending.site=SOL_MIR_RUNTIME_NONE;
    for(size_t i=0;i<conventions->failure_site_count;i++)if((conventions->failure_sites[i].allowed_codes&(UINT32_C(1)<<(SOL_MIR_RUNTIME_FAILURE_INTEGER_OVERFLOW-1)))!=0){pending.site=i;pending.source=conventions->failure_sites[i].source;break;}
    CHECK(pending.site!=SOL_MIR_RUNTIME_NONE); request.produced=NULL;request.pending=&pending;CHECK(sol_mir_runtime_cleanup_test_select(cleanup,&request,NULL,0,&trace)&&!memcmp(&trace.primary,&pending,sizeof pending));
    SolMirRuntimeCleanupFailureOccurrence malformed=pending; ++malformed.source.start;request.pending=&malformed;CHECK(!sol_mir_runtime_cleanup_test_select(cleanup,&request,NULL,0,&trace));malformed=pending;malformed.site=SOL_MIR_RUNTIME_NONE;request.pending=&malformed;CHECK(!sol_mir_runtime_cleanup_test_select(cleanup,&request,NULL,0,&trace));malformed=pending;malformed.code=SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT;request.pending=&malformed;CHECK(!sol_mir_runtime_cleanup_test_select(cleanup,&request,NULL,0,&trace));malformed=pending;malformed.detail_length=1;request.pending=&malformed;CHECK(!sol_mir_runtime_cleanup_test_select(cleanup,&request,NULL,0,&trace));request.pending=&pending;
    record.code=SOL_MIR_RUNTIME_FAILURE_INTEGER_OVERFLOW;record.source=pending.source;CHECK(!sol_mir_runtime_cleanup_capture_detail(cleanup,event_id,local->edge_role,&record,&detail));
    request.produced=&produced;CHECK(!sol_mir_runtime_cleanup_test_select(cleanup,&request,NULL,0,&trace));request.pending=NULL;request.produced=NULL;CHECK(!sol_mir_runtime_cleanup_test_select(cleanup,&request,NULL,0,&trace));
    SolMirRuntimeCleanupFailureOccurrence bad=produced;bad.code=SOL_MIR_RUNTIME_FAILURE_INTEGER_OVERFLOW;request.produced=&bad;CHECK(!sol_mir_runtime_cleanup_test_select(cleanup,&request,NULL,0,&trace));
    for(size_t j=0;j<event->transitions.count;j++){const SolMirRuntimeCleanupTransition*t=&cleanup->transitions[event->transitions.offset+j];if(t->edge_role==SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_NORMAL){SolMirRuntimeCleanupTraceRequest normal={event_id,t->edge_role,NULL,&pending,SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};CHECK(!sol_mir_runtime_cleanup_test_select(cleanup,&normal,NULL,0,&trace));}}
    bool excluded=false;for(size_t i=0;i<cleanup->event_count&&!excluded;i++)for(size_t j=0;j<cleanup->events[i].transitions.count;j++){const SolMirRuntimeCleanupTransition*t=&cleanup->transitions[cleanup->events[i].transitions.offset+j];if(t->edge_role==SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE&&t->failure_source==SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31){SolMirRuntimeCleanupTraceRequest x={i,t->edge_role,NULL,&pending,SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};CHECK(!sol_mir_runtime_cleanup_test_select(cleanup,&x,NULL,0,&trace));excluded=true;break;}}CHECK(excluded);
    size_t direct_host=0,indirect=0,predicate=0;for(size_t i=0;i<cleanup->event_count;i++){const SolMirRuntimeCleanupEvent*e=&cleanup->events[i];if(e->inherited_failure_site>=conventions->call_count)continue;const SolMirRuntimeCall*c=&conventions->calls[e->inherited_failure_site];for(size_t j=0;j<e->transitions.count;j++){const SolMirRuntimeCleanupTransition*t=&cleanup->transitions[e->transitions.offset+j];if(t->edge_role!=SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE)continue;bool excluded_class=c->owner_kind==SOL_MIR_RUNTIME_CALL_OWNER_PREDICATE||c->target_kind!=SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL;if(excluded_class){CHECK(t->failure_source==SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31);SolMirRuntimeCleanupTraceRequest x={i,t->edge_role,NULL,&pending,SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};CHECK(!sol_mir_runtime_cleanup_test_select(cleanup,&x,NULL,0,&trace));if(c->owner_kind==SOL_MIR_RUNTIME_CALL_OWNER_PREDICATE)++predicate;else if(c->target_kind==SOL_MIR_RUNTIME_TARGET_DIRECT_HOST)++direct_host;else ++indirect;}}}CHECK(direct_host>0);CHECK(indirect==0&&predicate==0);
    size_t resume=program->materialization.edges[local->continuation].block;bool resumed=false;for(size_t i=0;i<cleanup->event_count;i++)if(cleanup->events[i].block==resume)for(size_t j=0;j<cleanup->events[i].transitions.count;j++){const SolMirRuntimeCleanupTransition*t=&cleanup->transitions[cleanup->events[i].transitions.offset+j];if(t->failure_source==SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING){SolMirRuntimeCleanupTraceRequest r={i,t->edge_role,NULL,&pending,SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};CHECK(sol_mir_runtime_cleanup_test_select(cleanup,&r,NULL,0,&trace)&&!memcmp(&trace.primary,&pending,sizeof pending));resumed=true;}}CHECK(resumed);
    SolMirRuntimeCleanupTransition *mutable_local=(SolMirRuntimeCleanupTransition *)local;SolMirRuntimeCleanupFailureSource source=mutable_local->failure_source;size_t site=mutable_local->failure_site;uint32_t mask=mutable_local->failure_mask;mutable_local->failure_source=SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31;CHECK(!sol_mir_runtime_cleanup_validate(cleanup,NULL));mutable_local->failure_source=source;mutable_local->failure_site=SOL_MIR_RUNTIME_NONE;CHECK(!sol_mir_runtime_cleanup_validate(cleanup,NULL));mutable_local->failure_site=site;mutable_local->failure_mask=0;CHECK(!sol_mir_runtime_cleanup_validate(cleanup,NULL));mutable_local->failure_mask=mask;CHECK(sol_mir_runtime_cleanup_validate(cleanup,NULL));
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
                    == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31
                && failure->failure_site == call_id
                && failure->failure_mask
                    == (UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT - 1)));
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
                    CHECK(transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE);
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
}
/* Focused owner-policy assertions over the E6 materialization: this checks
 * local-kind classification rather than relying on an executor or source text. */
static void check_owner_snapshot_contract_and_selector(const SolMirRuntimeCleanup *cleanup,
    const SolMirConcreteProgram *program, const SolMirRuntimeConventions *conventions) {
    bool saw_owned_formal = false, saw_snapshot = false, saw_violation = false;
    SolMirRuntimeCleanupAction trace_actions[64];
    for (size_t i = 0; i < cleanup->event_count; ++i) {
        const SolMirRuntimeCleanupEvent *event = &cleanup->events[i];
        for (size_t a = 0; a < event->transitions.count; ++a) {
            const SolMirRuntimeCleanupTransition *left = &cleanup->transitions[event->transitions.offset + a];
            for (size_t b = a + 1; b < event->transitions.count; ++b)
                CHECK(left->edge_role != cleanup->transitions[event->transitions.offset + b].edge_role);
            if (left->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL) {
                SolMirRuntimeCleanupTrace trace;
                SolMirRuntimeCleanupTraceRequest request = {i, left->edge_role,
                    NULL, NULL, SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
                CHECK(sol_mir_runtime_cleanup_test_trace(cleanup, &request,
                    trace_actions, 64, &trace));
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
        if (event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION
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
    for (size_t i = 0; i < cleanup->transition_count; ++i) {
        const SolMirRuntimeCleanupTransition *transition = &cleanup->transitions[i];
        if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_VIOLATION) {
            CHECK(transition->failure_site < conventions->failure_site_count);
            CHECK(conventions->failure_sites[transition->failure_site].origin_kind
                == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT);
        }
    }
    CHECK(saw_owned_formal && saw_snapshot && saw_violation);
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
            bool requires_final_propagation = transition->outcome
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
    CHECK(!root_moved_drop);   /* whole-root move leaves its local uninitialized */
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

int main(void) {
    check_predicate_propagate_schema();
    check_zero_supplemental_alias_preflight();
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
        && conventions.import_count == 52 && conventions.failure_site_count == 29);
    CHECK(conventions.usage.signatures == 18 && conventions.usage.signature_slots == 19
        && conventions.usage.calls == 18 && conventions.usage.operands == 24
        && conventions.usage.writebacks == 1 && conventions.usage.entries == 1
        && conventions.usage.imports == 52 && conventions.usage.failure_sites == 29
        && conventions.usage.owned_bytes == 16328
        && conventions.usage.build_scratch_bytes == 21
        && conventions.usage.build_work == 9427
        && conventions.usage.validation_scratch_bytes == 762459500
        && conventions.usage.validation_work == 95865687);
    CHECK(values.recipe_operation_count == 21 && values.allocation_plan_count == 21
        && values.copy_plan_count == 21 && values.equality_plan_count == 21
        && values.host_result_plan_count == 21 && values.host_result_requirement_count == 4
        && values.ownership_plan_count == 21 && values.ownership_variant_count == 9
        && values.owned_edge_count == 12);
    CHECK(values.usage.records == 21 && values.usage.allocation_plans == 21
        && values.usage.copy_plans == 21 && values.usage.equality_plans == 21
        && values.usage.host_result_plans == 21 && values.usage.host_result_requirements == 4
        && values.usage.ownership_plans == 21 && values.usage.ownership_variants == 9
        && values.usage.owned_edges == 12 && values.usage.owned_bytes == 4432
        && values.usage.build_scratch_bytes == 94 && values.usage.build_work == 357
        && values.usage.validation_scratch_bytes == 762459500
        && values.usage.validation_work == 95870865);
    /* Final P3.3 E6 all-roots cleanup-policy census and exact metering. */
    CHECK(cleanup.event_count == 368 && cleanup.action_count == 622
        && cleanup.transition_count == 452 && cleanup.supplemental_site_count == 44
        && cleanup.drop_path_count == 262);
    CHECK(cleanup.usage.events == 368 && cleanup.usage.actions == 622
        && cleanup.usage.transitions == 452 && cleanup.usage.supplemental_sites == 44
        && cleanup.usage.drop_paths == 262 && cleanup.usage.owned_bytes == 121792
        && cleanup.usage.build_scratch_bytes == 246744 && cleanup.usage.build_work == 72327
        && cleanup.usage.validation_scratch_bytes == 246744
        && cleanup.usage.validation_work == 54643);
    CHECK(sol_mir_runtime_cleanup_validate(&cleanup,NULL));
    /* Prediction is address-independent: a deliberately overlapping synthetic
     * placement cannot alter the successful-disjoint alias-work census. */
    size_t alias_work=0, overlapped_alias_work=0;
    CHECK(sol_mir_runtime_cleanup_test_alias_work(&cleanup,&alias_work));
    SolMirRuntimeCleanupEvent *saved_alias_events=cleanup.events;
    cleanup.events=(SolMirRuntimeCleanupEvent *)(void *)conventions.signatures;
    CHECK(sol_mir_runtime_cleanup_test_alias_work(&cleanup,&overlapped_alias_work)
        && alias_work==overlapped_alias_work);
    cleanup.events=saved_alias_events;
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
        sol_mir_runtime_cleanup_test_force_persistent_allocation_failure_attempt(attempt);
        CHECK(sol_mir_runtime_cleanup_build(&rr,&bounded,NULL)==SOL_MIR_RUNTIME_CLEANUP_BUILD_ALLOCATION_FAILED);
        CHECK(sol_mir_runtime_cleanup_test_persistent_allocation_attempts()==attempt);
        CHECK(bounded.events==NULL && bounded.actions==NULL && bounded.transitions==NULL
            && bounded.supplemental_sites==NULL && bounded.drop_paths==NULL);
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
        rr.limits=&one_below[i];
        CHECK(sol_mir_runtime_cleanup_build(&rr,&bounded,NULL)==SOL_MIR_RUNTIME_CLEANUP_BUILD_RESOURCE_EXHAUSTED);
        CHECK(bounded.events==NULL && bounded.event_count==0);
        CHECK(sol_mir_runtime_cleanup_test_persistent_allocation_attempts() == 0);
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
    bool saw_contract_result = false, saw_resume = false, saw_success_reject_pending = false;
    for (size_t i = 0; i < cleanup.event_count; ++i) {
        const SolMirRuntimeCleanupEvent *event = &cleanup.events[i];
        for (size_t j = 0; j < event->transitions.count; ++j) {
            const SolMirRuntimeCleanupTransition *transition =
                &cleanup.transitions[event->transitions.offset + j];
            SolMirRuntimeCleanupFailureOccurrence occurrence;
            if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_VIOLATION
                && occurrence_for_transition(&cleanup, &conventions, transition, &occurrence)) {
                SolMirRuntimeCleanupTraceRequest request = {i, transition->edge_role,
                    &occurrence, NULL, SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL};
                CHECK(sol_mir_runtime_cleanup_test_select(&cleanup, &request,
                    trace_actions, 64, &trace));
                CHECK(trace.primary.failure_source
                    == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31);
                saw_contract_result = true;
            }
            if (transition->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING) {
                SolMirRuntimeCleanupTraceRequest request = {i, transition->edge_role,
                    NULL, &trace_occurrence, SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
                CHECK(sol_mir_runtime_cleanup_test_select(&cleanup, &request,
                    trace_actions, 64, &trace));
                CHECK(memcmp(&trace.primary, &trace_occurrence, sizeof(trace.primary)) == 0);
                saw_resume = true;
            }
            if (transition->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE) {
                SolMirRuntimeCleanupTraceRequest request = {i, transition->edge_role,
                    NULL, &trace_occurrence, SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
                CHECK(!sol_mir_runtime_cleanup_test_select(&cleanup, &request,
                    trace_actions, 64, &trace));
                saw_success_reject_pending = true;
            }
        }
    }
    CHECK(saw_contract_result && saw_resume && saw_success_reject_pending);
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
