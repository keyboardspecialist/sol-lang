#include "support/native_progress.h"
#include "support/path_work.h"
#define main sol_test_default_body
#define SOL_MIR_PLAN_TEST_HOOKS 1
#include "sol/mir_runtime_host_abi.h"
#include "sol/effects.h"
#include "sol/lexer.h"
#include "sol/ownership.h"
#include "sol/package.h"
#include "../src/mir_runtime_host_abi_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)
static void test_host_capability_equality_body(void);
static void test_distinct_roots_and_derived_sources(void);
static void test_frozen_profile_rejection(void);
static void test_frozen_profile_subset(void);
static void test_fifth_lookalike_rejection(void);
static bool counting_callback(void *,const SolMirRuntimeHostValue *const *,size_t,
    const SolMirRuntimeHostValue **,const uint8_t **,size_t *);
typedef struct { unsigned char *bytes; size_t size; } RenderBytes;
static void render_bytes_free(RenderBytes *render) {
    if (render) { free(render->bytes); *render=(RenderBytes){0}; }
}
static bool snapshot_stage_failure(const char *operation,const char *stage) {
    fprintf(stderr,"snapshot failure operation=%s stage=%s\n",operation,stage);
    return false;
}
static bool snapshot_stream_bytes(FILE *stream,RenderBytes *render,
    const char *operation) {
    if(!stream||!render)return snapshot_stage_failure(operation,"arguments");
    *render=(RenderBytes){0};
    if(fflush(stream)!=0)return snapshot_stage_failure(operation,"flush");
    if(fseek(stream,0,SEEK_END)!=0)return snapshot_stage_failure(operation,"seek_end");
    long end=ftell(stream);
    if(end<0)return snapshot_stage_failure(operation,"tell_end");
    if((uintmax_t)end>SIZE_MAX-1)return snapshot_stage_failure(operation,"size_overflow");
    render->size=(size_t)end;
    render->bytes=malloc(render->size+1);
    if(!render->bytes)return snapshot_stage_failure(operation,"allocation");
    if(fseek(stream,0,SEEK_SET)!=0){render_bytes_free(render);return snapshot_stage_failure(operation,"seek_start");}
    size_t read=fread(render->bytes,1,render->size,stream);
    if(read!=render->size||ferror(stream)){render_bytes_free(render);return snapshot_stage_failure(operation,"readback");}
    render->bytes[render->size]='\0';
    return true;
}
/* Snapshot exactly the renderer's byte stream.  The terminator is storage for
 * string-oriented assertions only: it is never part of the compared output. */
static bool render_bytes(SolMirRuntimeHostAbi *abi, RenderBytes *render) {
    if(!render)return snapshot_stage_failure("host-abi-render","output_argument");
    *render=(RenderBytes){0};
    FILE *stream=tmpfile();
    if(!stream)return snapshot_stage_failure("host-abi-render","tmpfile");
    if(!sol_mir_runtime_host_abi_render(stream,abi)){
        fclose(stream);
        return snapshot_stage_failure("host-abi-render","renderer_result");
    }
    bool ok=snapshot_stream_bytes(stream,render,"host-abi-render");
    fclose(stream);
    if(!ok)render_bytes_free(render);
    return ok;
}
static void check_owner_renders(const SolMirRuntimeHostAbi *abi) {
    RenderBytes rendered={0};
    CHECK(render_bytes((SolMirRuntimeHostAbi *)abi,&rendered));
    render_bytes_free(&rendered);
}
static bool observation_bytes(
    const SolMirRuntimeHostAbiObservationContext *context,RenderBytes *render) {
    if(!render)return snapshot_stage_failure("host-abi-observation","output_argument");
    *render=(RenderBytes){0};
    FILE *stream=tmpfile();
    if(!stream)return snapshot_stage_failure("host-abi-observation","tmpfile");
    sol_mir_runtime_host_abi_test_emit_comparison(stream,context);
    bool ok=snapshot_stream_bytes(stream,render,"host-abi-observation");
    fclose(stream);
    if(!ok)render_bytes_free(render);
    return ok;
}
static void check_render_rejects_mutation(SolMirRuntimeHostAbi *abi) {
    FILE *stream=tmpfile(); CHECK(stream!=NULL); if(!stream)return;
    CHECK(!sol_mir_runtime_host_abi_render(stream,abi));
    CHECK(fseek(stream,0,SEEK_END)==0&&ftell(stream)==0); fclose(stream);
}
static void test_render_and_semantic_seal(SolMirRuntimeHostAbi *abi,
    const SolMirRuntimeHostPreflightRequest *preflight,
    SolMirRuntimeHostInvocationRequest *invoke,
    SolMirRuntimeHostInvocationResult *result) {
    RenderBytes first,repeated;
    CHECK(render_bytes(abi,&first));
    CHECK(render_bytes(abi,&repeated)&&first.size==repeated.size
        &&memcmp(first.bytes,repeated.bytes,first.size)==0);
    CHECK(strstr((const char *)first.bytes,"runtime-host-abi host-abi=true host-execution=false\n")!=NULL);
    CHECK(strstr((const char *)first.bytes,"argument key=")!=NULL);
    /* Every semantic owner arena is covered: malformed render is a strict
       zero-write operation, not a partial diagnostic stream. */
#define MUTATE_RENDER(arena, member) do { \
    size_t saved=(size_t)abi->arena[0].member; \
    abi->arena[0].member=(size_t)(saved+1); \
    check_render_rejects_mutation(abi); abi->arena[0].member=saved; \
} while(0)
    MUTATE_RENDER(capabilities,recipe); MUTATE_RENDER(entry_roots,formal);
    MUTATE_RENDER(operations,receiver); MUTATE_RENDER(arguments,recipe);
    MUTATE_RENDER(formals,recipe); MUTATE_RENDER(shapes,recipe);
    if(abi->shape_case_count) MUTATE_RENDER(shape_cases,ordinal);
    MUTATE_RENDER(requirements,call);
#undef MUTATE_RENDER
    size_t callbacks=0;
    SolMirRuntimeHostInvocationRequest rejected=*invoke;
    rejected.callback=counting_callback; rejected.context=&callbacks;
    size_t saved_root=abi->requirements[0].root;abi->requirements[0].root=saved_root+1;
    CHECK(sol_mir_runtime_host_abi_preflight(abi,preflight)==SOL_MIR_RUNTIME_HOST_PREFLIGHT_INVALID);CHECK(sol_mir_runtime_host_abi_test_invoke(&rejected,result)==SOL_MIR_RUNTIME_HOST_INVOKE_INVALID&&callbacks==0);abi->requirements[0].root=saved_root;
    SolAccessMode saved_access=abi->operations[0].receiver_access;abi->operations[0].receiver_access=(SolAccessMode)(saved_access^1u);
    CHECK(sol_mir_runtime_host_abi_preflight(abi,preflight)==SOL_MIR_RUNTIME_HOST_PREFLIGHT_INVALID);CHECK(sol_mir_runtime_host_abi_test_invoke(&rejected,result)==SOL_MIR_RUNTIME_HOST_INVOKE_INVALID&&callbacks==0);abi->operations[0].receiver_access=saved_access;
    saved_access=abi->formals[0].access;abi->formals[0].access=(SolAccessMode)(saved_access^1u);
    CHECK(sol_mir_runtime_host_abi_preflight(abi,preflight)==SOL_MIR_RUNTIME_HOST_PREFLIGHT_INVALID);CHECK(sol_mir_runtime_host_abi_test_invoke(&rejected,result)==SOL_MIR_RUNTIME_HOST_INVOKE_INVALID&&callbacks==0);abi->formals[0].access=saved_access;
    SolMirRuntimeHostArgumentKind saved_kind=abi->shapes[0].kind;abi->shapes[0].kind=(SolMirRuntimeHostArgumentKind)(saved_kind^1u);
    CHECK(sol_mir_runtime_host_abi_preflight(abi,preflight)==SOL_MIR_RUNTIME_HOST_PREFLIGHT_INVALID);CHECK(sol_mir_runtime_host_abi_test_invoke(&rejected,result)==SOL_MIR_RUNTIME_HOST_INVOKE_INVALID&&callbacks==0);abi->shapes[0].kind=saved_kind;
    SolMirRecipeId saved_recipe=abi->capabilities[0].recipe;abi->capabilities[0].recipe=saved_recipe+1;
    CHECK(sol_mir_runtime_host_abi_preflight(abi,preflight)==SOL_MIR_RUNTIME_HOST_PREFLIGHT_INVALID);CHECK(sol_mir_runtime_host_abi_test_invoke(&rejected,result)==SOL_MIR_RUNTIME_HOST_INVOKE_INVALID&&callbacks==0);abi->capabilities[0].recipe=saved_recipe;
    CHECK(sol_mir_runtime_host_abi_preflight(abi,preflight)==SOL_MIR_RUNTIME_HOST_PREFLIGHT_SUCCEEDED);
    render_bytes_free(&repeated); render_bytes_free(&first);
}
typedef struct { SolDiagnostics d; SolHirModule h; SolTypeTable t; SolEffectTable e; SolContractTable c; SolIr ir; SolPackage p; } Fixture;
typedef struct { SolSource source; SolTokens tokens; SolSyntaxTree syntax; SolDiagnostics d; SolHirModule h; SolTypeTable t; SolEffectTable e; SolContractTable c; SolIr ir; } TextFixture;
typedef struct { SolMirConcreteProgram concrete; SolMirRuntimeConventions conventions; SolMirRuntimeValues values; SolMirRuntimeCleanup cleanup; SolMirRuntimeHostAbi host; } EqualityPipeline;
typedef enum { TEXT_SETUP_OK,TEXT_SETUP_SOURCE,TEXT_SETUP_LEX,TEXT_SETUP_PARSE,TEXT_SETUP_HIR,TEXT_SETUP_TYPE,TEXT_SETUP_EFFECT,TEXT_SETUP_CONTRACT,TEXT_SETUP_IR } TextSetupStage;
static SolIrCallableId find(const SolIr *ir,const char*n,SolIrCallableKind k){for(size_t i=0;i<ir->callable_count;i++)if(ir->callables[i].kind==k&&!strcmp(ir->callables[i].name,n))return i;return SOL_IR_NONE;}
static bool setup_at(Fixture *f,const char *directory){memset(f,0,sizeof(*f));sol_package_init(&f->p);sol_diagnostics_init(&f->d);sol_hir_module_init(&f->h);sol_type_table_init(&f->t);sol_effect_table_init(&f->e);sol_contract_table_init(&f->c);sol_ir_init(&f->ir);char error[256];if(!sol_package_load_directory(&f->p,directory,&f->d,error,sizeof(error)))return false;SolHirFileScope scopes[3];if(f->p.file_count>3)return false;for(size_t i=0;i<f->p.file_count;i++)scopes[i]=(SolHirFileScope){f->p.files[i].module_name,f->p.files[i].import_start,f->p.files[i].import_count,f->p.files[i].item_start,f->p.files[i].item_count};return sol_hir_lower_scoped(&f->p.source,&f->p.syntax,scopes,f->p.file_count,&f->h,&f->d)&&sol_type_check(&f->p.source,&f->p.syntax,&f->h,&f->t,&f->d)&&sol_effect_check(&f->p.source,&f->p.syntax,&f->h,&f->t,&f->e,&f->d)&&sol_contract_lower(&f->p.source,&f->p.syntax,&f->h,&f->t,&f->e,&f->c,&f->d)&&sol_ir_lower_scoped(&f->p.source,&f->p.syntax,&f->h,&f->t,&f->e,&f->c,f->p.files,f->p.file_count,&f->ir,&f->d);}
static bool setup(Fixture *f){
    test_distinct_roots_and_derived_sources();
    test_frozen_profile_rejection();
    test_frozen_profile_subset();
    test_fifth_lookalike_rejection();
    test_host_capability_equality_body();
    return setup_at(f,SOL_TEST_SOURCE_DIR "/tests/conformance/e6");
}
static void finish(Fixture*f){sol_ir_free(&f->ir);sol_contract_table_free(&f->c);sol_effect_table_free(&f->e);sol_type_table_free(&f->t);sol_hir_module_free(&f->h);sol_diagnostics_free(&f->d);sol_package_free(&f->p);}
static bool callback(void *x,const SolMirRuntimeHostValue *const*a,size_t n,const SolMirRuntimeHostValue **r,const uint8_t **d,size_t*l){(void)x;(void)a;(void)n;(void)d;(void)l;static const SolMirRuntimeHostValue unit={.kind=SOL_MIR_RUNTIME_HOST_VALUE_UNIT};*r=&unit;return true;}
static bool counting_callback(void *x,const SolMirRuntimeHostValue *const*a,size_t n,const SolMirRuntimeHostValue **r,const uint8_t **d,size_t*l){++*(size_t*)x;return callback(NULL,a,n,r,d,l);}
static bool host_error_callback(void *x,const SolMirRuntimeHostValue *const*a,size_t n,const SolMirRuntimeHostValue **r,const uint8_t **d,size_t*l){(void)x;(void)a;(void)n;(void)r;static const uint8_t detail[]={'x'};*d=detail;*l=sizeof(detail);return false;}
typedef struct { const uint8_t *bytes; size_t length; } HostErrorDetail;
static bool host_error_detail_callback(void *x,const SolMirRuntimeHostValue *const*a,size_t n,const SolMirRuntimeHostValue **r,const uint8_t **d,size_t*l){(void)a;(void)n;(void)r;const HostErrorDetail *detail=x;*d=detail->bytes;*l=detail->length;return false;}
static bool value_callback(void *context,const SolMirRuntimeHostValue *const *arguments,
    size_t argument_count,const SolMirRuntimeHostValue **result,
    const uint8_t **detail,size_t *detail_length){
    (void)arguments;(void)argument_count;(void)detail;(void)detail_length;
    *result=context;return true;
}
static void test_option_result_transfer(const SolMirRuntimeHostAbi *abi,
    const SolMirRuntimeHostPreflightRequest *preflight){
    size_t requirement=SOL_MIR_RUNTIME_NONE;
    for(size_t i=0;i<abi->requirement_count;i++)
        if(abi->operations[abi->requirements[i].operation].result_class
            ==SOL_MIR_RUNTIME_HOST_RESULT_OPTION){requirement=i;break;}
    CHECK(requirement!=SOL_MIR_RUNTIME_NONE);
    if(requirement==SOL_MIR_RUNTIME_NONE)return;
    const SolMirRuntimeHostOperation *operation=
        &abi->operations[abi->requirements[requirement].operation];
    const SolMirRuntimeHostFormal *formal=&abi->formals[operation->formals.offset];
    SolMirRuntimeHostValue argument={0};
    if(abi->shapes[formal->shape].kind==SOL_MIR_RUNTIME_HOST_ARGUMENT_TEXT){
        argument.kind=SOL_MIR_RUNTIME_HOST_VALUE_TEXT;
        argument.as.text.bytes=(const uint8_t *)"key";
        argument.as.text.length=3;
    }else argument=(SolMirRuntimeHostValue){.kind=SOL_MIR_RUNTIME_HOST_VALUE_INT64,
        .as.int64_value=0};
    const SolMirRuntimeHostValue *arguments[]={&argument};
    const SolMirRecipe *recipe=&abi->conventions->concrete->representation.recipes[operation->result];
    size_t payload_ordinal=SOL_MIR_RUNTIME_NONE;
    for(size_t i=0;i<recipe->variants.count;i++){
        const SolMirRecipeVariant *variant=&abi->conventions->concrete->representation.variants[recipe->variants.offset+i];
        if(variant->fields.count==1){payload_ordinal=variant->ordinal;break;}
    }
    CHECK(payload_ordinal!=SOL_MIR_RUNTIME_NONE);
    uint8_t host_bytes[]={'h','o','s','t'};
    SolMirRuntimeHostValue payload={.kind=SOL_MIR_RUNTIME_HOST_VALUE_TEXT,
        .as.text={host_bytes,sizeof(host_bytes)}};
    SolMirRuntimeHostValue source={.kind=SOL_MIR_RUNTIME_HOST_VALUE_OPTION,
        .as.sum={payload_ordinal,&payload}};
    SolMirRuntimeAllocationQuota unlimited={UINT64_MAX,UINT64_MAX};
    SolMirRuntimeAllocationUsage usage={0};size_t calls=0;
    SolMirRuntimeHostInvocationRequest request={abi,preflight,
        abi->requirements[requirement].root,abi->requirements[requirement].operation,
        abi->requirements[requirement].call,arguments,1,value_callback,&source,
        100,&calls,&unlimited,&usage,NULL};
    SolMirRuntimeHostInvocationResult result;
    CHECK(sol_mir_runtime_host_abi_test_invoke(&request,&result)
        ==SOL_MIR_RUNTIME_HOST_INVOKE_SUCCEEDED&&result.value!=NULL);
    CHECK(result.value->kind==SOL_MIR_RUNTIME_HOST_VALUE_OPTION
        &&result.value->as.sum.payload!=NULL
        &&result.value->as.sum.payload->as.text.bytes!=host_bytes);
    host_bytes[0]='X';
    CHECK(result.value->as.sum.payload->as.text.bytes[0]=='h');
    size_t charged_requests=usage.requests,charged_bytes=usage.bytes;
    sol_mir_runtime_host_owned_value_free(result.value);
    CHECK(charged_requests!=0&&charged_bytes!=0);
    for(size_t refusal=1;refusal<=charged_requests;refusal++){
        SolMirRuntimeHostTransferLimits limits={.fail_allocation_attempt=refusal};
        usage=(SolMirRuntimeAllocationUsage){0};calls=0;request.transfer_limits=&limits;
        CHECK(sol_mir_runtime_host_abi_test_invoke(&request,&result)
            ==SOL_MIR_RUNTIME_HOST_INVOKE_ALLOCATION_FAILED&&result.value==NULL
            &&calls==1&&usage.requests==refusal-1);
    }
    usage=(SolMirRuntimeAllocationUsage){0};calls=0;
    SolMirRuntimeAllocationQuota one_below={charged_requests-1,charged_bytes};
    request.quota=&one_below;request.transfer_limits=NULL;
    CHECK(sol_mir_runtime_host_abi_test_invoke(&request,&result)
        ==SOL_MIR_RUNTIME_HOST_INVOKE_ALLOCATION_LIMIT&&result.value==NULL&&calls==1
        &&usage.requests==0&&usage.bytes==0);
    SolMirRuntimeHostTransferLimits exact_transfer={.max_depth=2,.max_nodes=2,.max_work=4};
    usage=(SolMirRuntimeAllocationUsage){0};calls=0;request.quota=&unlimited;
    request.transfer_limits=&exact_transfer;
    CHECK(sol_mir_runtime_host_abi_test_invoke(&request,&result)
        ==SOL_MIR_RUNTIME_HOST_INVOKE_SUCCEEDED&&calls==1);
    sol_mir_runtime_host_owned_value_free(result.value);
    SolMirRuntimeHostTransferLimits depth_below_transfer={.max_depth=1};
    SolMirRuntimeHostTransferLimits nodes_below_transfer={.max_nodes=1};
    SolMirRuntimeHostTransferLimits work_below_transfer={.max_work=3};
    const SolMirRuntimeHostTransferLimits *below[]={&depth_below_transfer,
        &nodes_below_transfer,&work_below_transfer};
    for(size_t i=0;i<3;i++){usage=(SolMirRuntimeAllocationUsage){0};calls=0;
        request.transfer_limits=below[i];
        CHECK(sol_mir_runtime_host_abi_test_invoke(&request,&result)
            ==SOL_MIR_RUNTIME_HOST_INVOKE_TRANSFER_LIMIT&&calls==1&&result.value==NULL
            &&usage.requests==0&&usage.bytes==0);}
}
static void test_exact_host_abi_limits(const SolMirRuntimeConventions *conventions,
    const SolMirRuntimeValues *values,const SolMirRuntimeCleanup *cleanup,
    const SolMirRuntimeHostAbi *model,SolDiagnostics *diagnostics){
    test_distinct_roots_and_derived_sources();
    SolMirRuntimeHostAbiLimits exact=model->limits;
    exact.max_capabilities=model->usage.capabilities;
    exact.max_entry_roots=model->usage.entry_roots;
    exact.max_operations=model->usage.operations;
    exact.max_arguments=model->usage.arguments;
    exact.max_formals=model->usage.formals;
    exact.max_shapes=model->usage.shapes;
    exact.max_shape_cases=model->usage.shape_cases?model->usage.shape_cases:1;
    exact.max_requirements=model->usage.requirements;
    exact.max_grants=model->usage.grants;
    exact.max_owned_bytes=model->usage.owned_bytes;
    exact.max_build_scratch_bytes=model->usage.build_scratch_bytes;
    exact.max_build_work=model->usage.build_work;
    exact.max_validation_scratch_bytes=model->usage.validation_scratch_bytes;
    exact.max_validation_work=model->usage.validation_work;
    size_t *limits[]={&exact.max_capabilities,&exact.max_entry_roots,
        &exact.max_operations,&exact.max_arguments,&exact.max_formals,
        &exact.max_shapes,&exact.max_shape_cases,&exact.max_requirements,
        &exact.max_grants,&exact.max_owned_bytes,&exact.max_build_scratch_bytes,
        &exact.max_build_work,&exact.max_validation_scratch_bytes,
        &exact.max_validation_work};
    for(size_t i=0;i<sizeof(limits)/sizeof(limits[0]);i++){
        SolMirRuntimeHostAbi owner;sol_mir_runtime_host_abi_init(&owner);
        CHECK(sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){
            conventions,values,cleanup,&exact},&owner,diagnostics)
            ==SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED);
        sol_mir_runtime_host_abi_free(&owner);
        if(*limits[i]==1)continue;--*limits[i];
        sol_mir_runtime_host_abi_test_reset_allocation_attempts();
        CHECK(sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){
            conventions,values,cleanup,&exact},&owner,diagnostics)
            ==SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED);
        CHECK(owner.capabilities==NULL&&owner.capability_count==0);
        if(limits[i]==&exact.max_build_work||limits[i]==&exact.max_validation_work)
            CHECK(sol_mir_runtime_host_abi_test_allocation_attempts()!=0);
        else CHECK(sol_mir_runtime_host_abi_test_allocation_attempts()==0);
        ++*limits[i];
    }
    /* Every independent persistent arena refusal is transactional. */
    for(size_t refusal=1;refusal<=7;refusal++){
        SolMirRuntimeHostAbi owner;sol_mir_runtime_host_abi_init(&owner);
        sol_mir_runtime_host_abi_test_reset_allocation_attempts();
        sol_mir_runtime_host_abi_test_force_persistent_allocation_failure_attempt(refusal);
        CHECK(sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){
            conventions,values,cleanup,&exact},&owner,diagnostics)
            ==SOL_MIR_RUNTIME_HOST_ABI_BUILD_ALLOCATION_FAILED);
        CHECK(owner.capability_count==0&&owner.capabilities==NULL);
        sol_mir_runtime_host_abi_free(&owner);
    }
    sol_mir_runtime_host_abi_test_force_persistent_allocation_failure_attempt(0);
    sol_mir_runtime_host_abi_test_force_validation_scratch_failure(true);
    CHECK(!sol_mir_runtime_host_abi_validate(model,NULL));
    sol_mir_runtime_host_abi_test_force_validation_scratch_failure(false);
    CHECK(sol_mir_runtime_host_abi_validate(model,NULL));
}
static void test_distinct_roots_and_derived_sources(void){
    static bool ran;
    if(ran)return;
    ran=true;
    Fixture f;if(!setup_at(&f,SOL_TEST_SOURCE_DIR "/tests/conformance/p34")){CHECK(false);sol_diagnostics_render_human(stderr,&f.p.source,&f.d);finish(&f);return;}
    SolMirConcreteProgram p;sol_mir_concrete_program_init(&p);
    SolMirProgramRoot root={find(&f.ir,"launch",SOL_IR_CALLABLE_FUNCTION),SOL_MIR_PROGRAM_ROOT_ENTRY};
    const char *names[]={"write","count","get","read"};
    SolIrCallableId imports[4];for(size_t i=0;i<4;i++)imports[i]=find(&f.ir,names[i],SOL_IR_CALLABLE_CAPABILITY);
    SolMirTargetDescriptor target=sol_mir_target_wasm32();
    CHECK(sol_mir_concrete_program_build(&(SolMirConcreteBuildRequest){&f.ir,&root,1,imports,4,&target,NULL},&p,&f.d)==SOL_MIR_CONCRETE_BUILD_SUCCEEDED);
    SolMirRuntimeConventions c;SolMirRuntimeValues v;SolMirRuntimeCleanup cl;SolMirRuntimeHostAbi a;
    sol_mir_runtime_conventions_init(&c);sol_mir_runtime_values_init(&v);sol_mir_runtime_cleanup_init(&cl);sol_mir_runtime_host_abi_init(&a);
    CHECK(sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&p,NULL},&c,&f.d)==SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&c,NULL},&v,&f.d)==SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&c,&v,NULL},&cl,&f.d)==SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED);
    if(sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&c,&v,&cl,NULL},&a,&f.d)!=SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED){CHECK(false);sol_diagnostics_render_human(stderr,&f.p.source,&f.d);sol_mir_runtime_host_abi_free(&a);sol_mir_runtime_cleanup_free(&cl);sol_mir_runtime_values_free(&v);sol_mir_runtime_conventions_free(&c);sol_mir_concrete_program_free(&p);finish(&f);return;}
    SolMirRuntimeHostAbiWorkCensus meter=sol_mir_runtime_host_abi_test_work_census();
    CHECK(meter.dry_work==meter.actual_work
        &&meter.census_work+meter.dry_work+meter.actual_work==a.usage.build_work);
    CHECK(sol_mir_runtime_host_abi_validate(&a,NULL));
    CHECK(a.entry_root_count==5&&a.entry_roots[0].recipe==a.entry_roots[1].recipe);
    /* The recursive helper cycle terminates the reachability/provenance fixed
     * points.  There is one static relay host-call site, and it retains
     * launch.left rather than the same-typed right root. */
    CHECK(a.requirement_count==4);
    CHECK(a.requirements[0].root==0&&a.requirements[0].call!=SOL_MIR_RUNTIME_NONE);
    size_t derived[2],count=0;for(size_t i=0;i<a.capability_count;i++)if(a.capabilities[i].source_construct!=SOL_MIR_RUNTIME_NONE&&count<2)derived[count++]=i;
    CHECK(count==2&&a.capabilities[derived[0]].recipe==a.capabilities[derived[1]].recipe&&a.capabilities[derived[0]].parent!=a.capabilities[derived[1]].parent&&a.capabilities[derived[0]].source_temporary!=a.capabilities[derived[1]].source_temporary&&a.capabilities[derived[0]].source_root!=a.capabilities[derived[1]].source_root);
    /* Preserve authenticated source-root lineage and reject a forged private
     * substitution; no private construction is source-reachable here. */
    SolMirRuntimeHostCapabilityPlan *base=&a.capabilities[derived[0]];
    CHECK(base->source==SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT
        &&base->parent==base->source_root&&base->source_root<a.entry_root_count);
    SolMirRuntimeHostCapabilitySource saved_source=base->source;
    SolMirRecipeId saved_private_recipe=base->private_recipe;
    SolMirMaterializedValueId saved_private_value=base->private_value;
    base->source=SOL_MIR_RUNTIME_HOST_CAPABILITY_PRIVATE_SOURCE;
    base->private_recipe=base->recipe;
    base->private_value=base->source_value;
    CHECK(!sol_mir_runtime_host_abi_validate(&a,NULL));
    base->source=saved_source;
    base->private_recipe=saved_private_recipe;
    base->private_value=saved_private_value;
    CHECK(sol_mir_runtime_host_abi_validate(&a,NULL));
    for(size_t i=0;i<a.requirement_count;i++)
        CHECK(a.requirements[i].root<a.entry_root_count
            &&a.requirements[i].root<a.entry_root_count);
    SolMirRuntimeHostRootBinding bindings[]={{0,1},{1,2},{2,3},{3,4},{4,5}};
    SolMirRuntimeHostGrant grants[4];for(size_t i=0;i<4;i++)grants[i]=(SolMirRuntimeHostGrant){a.requirements[i].root,a.requirements[i].operation};
    SolMirRuntimeHostPreflightRequest good={0,bindings,5,grants,4};CHECK(sol_mir_runtime_host_abi_preflight(&a,&good)==SOL_MIR_RUNTIME_HOST_PREFLIGHT_SUCCEEDED);
    SolMirRuntimeHostGrant wrong[]={{a.requirements[0].root^1u,a.requirements[0].operation}};SolMirRuntimeHostPreflightRequest rejected={0,bindings,2,wrong,1};CHECK(sol_mir_runtime_host_abi_preflight(&a,&rejected)==SOL_MIR_RUNTIME_HOST_PREFLIGHT_INVALID);
    /* The test-only borrowed-view checker is intentionally independent of
     * operations, grants, callbacks, and transfer allocation. */
    const SolMirRepresentation *representation=&c.concrete->representation;
    SolMirRecipeId nested=SOL_MIR_RECIPE_NONE,result_recipe=SOL_MIR_RECIPE_NONE;
    size_t none=SOL_MIR_RUNTIME_NONE,some=SOL_MIR_RUNTIME_NONE,text_case=SOL_MIR_RUNTIME_NONE,bool_case=SOL_MIR_RUNTIME_NONE;
    for(size_t i=0;i<representation->recipe_count;i++){
        const SolMirRecipe *option=&representation->recipes[i];
        if(option->kind!=SOL_MIR_RECIPE_OPTION)continue;
        for(size_t q=0;q<option->variants.count;q++){
            const SolMirRecipeVariant *variant=&representation->variants[option->variants.offset+q];
            if(!variant->fields.count)none=variant->ordinal;
            else if(variant->fields.count==1){SolMirRecipeId child=representation->fields[variant->fields.offset].type;
                if(child<representation->recipe_count&&representation->recipes[child].kind==SOL_MIR_RECIPE_RESULT){nested=i;result_recipe=child;some=variant->ordinal;}}
        }
        if(nested!=SOL_MIR_RECIPE_NONE)break;
    }
    CHECK(nested!=SOL_MIR_RECIPE_NONE&&none!=SOL_MIR_RUNTIME_NONE&&some!=SOL_MIR_RUNTIME_NONE);
    if(nested!=SOL_MIR_RECIPE_NONE){
        const SolMirRecipe *sum=&representation->recipes[result_recipe];
        for(size_t i=0;i<sum->variants.count;i++){const SolMirRecipeVariant *variant=&representation->variants[sum->variants.offset+i];if(variant->fields.count!=1)continue;SolMirRecipeId leaf=representation->fields[variant->fields.offset].type;if(representation->recipes[leaf].kind==SOL_MIR_RECIPE_TEXT)text_case=variant->ordinal;else if(representation->recipes[leaf].kind==SOL_MIR_RECIPE_BOOL)bool_case=variant->ordinal;}
        CHECK(text_case!=SOL_MIR_RUNTIME_NONE&&bool_case!=SOL_MIR_RUNTIME_NONE);
        uint8_t bytes[]={'t','e','x','t'};SolMirRuntimeHostValue text={.kind=SOL_MIR_RUNTIME_HOST_VALUE_TEXT,.as.text={bytes,sizeof(bytes)}};SolMirRuntimeHostValue boolean={.kind=SOL_MIR_RUNTIME_HOST_VALUE_BOOL,.as.bool_value=true};SolMirRuntimeHostValue text_result={.kind=SOL_MIR_RUNTIME_HOST_VALUE_RESULT,.as.sum={text_case,&text}};SolMirRuntimeHostValue bool_result={.kind=SOL_MIR_RUNTIME_HOST_VALUE_RESULT,.as.sum={bool_case,&boolean}};SolMirRuntimeHostValue some_text={.kind=SOL_MIR_RUNTIME_HOST_VALUE_OPTION,.as.sum={some,&text_result}};SolMirRuntimeHostValue some_bool={.kind=SOL_MIR_RUNTIME_HOST_VALUE_OPTION,.as.sum={some,&bool_result}};SolMirRuntimeHostValue none_value={.kind=SOL_MIR_RUNTIME_HOST_VALUE_OPTION,.as.sum={none,NULL}};
        SolMirRuntimeHostTransferLimits exact={.max_depth=3,.max_nodes=3,.max_work=3};
        CHECK(sol_mir_runtime_host_abi_test_check_shape(&a,nested,&none_value,&exact));CHECK(sol_mir_runtime_host_abi_test_check_shape(&a,nested,&some_text,&exact));CHECK(sol_mir_runtime_host_abi_test_check_shape(&a,nested,&some_bool,&exact));
        SolMirRuntimeHostTransferLimits depth_below={.max_depth=2},nodes_below={.max_nodes=2},work_below={.max_work=2};
        CHECK(!sol_mir_runtime_host_abi_test_check_shape(&a,nested,&some_text,&depth_below));CHECK(!sol_mir_runtime_host_abi_test_check_shape(&a,nested,&some_text,&nodes_below));CHECK(!sol_mir_runtime_host_abi_test_check_shape(&a,nested,&some_text,&work_below));
        text_result.as.sum.payload=&boolean;CHECK(!sol_mir_runtime_host_abi_test_check_shape(&a,nested,&some_text,NULL));text_result.as.sum.payload=&text;text.as.text.bytes=NULL;CHECK(!sol_mir_runtime_host_abi_test_check_shape(&a,nested,&some_text,NULL));text.as.text.bytes=bytes;some_text.as.sum.payload=NULL;CHECK(!sol_mir_runtime_host_abi_test_check_shape(&a,nested,&some_text,NULL));
    }
    sol_mir_runtime_host_abi_free(&a);sol_mir_runtime_cleanup_free(&cl);sol_mir_runtime_values_free(&v);sol_mir_runtime_conventions_free(&c);sol_mir_concrete_program_free(&p);finish(&f);
}
/* A structurally E3-shaped recursive argument is not itself authority.  The
 * fourth profile table is closed: an otherwise valid fifth host import must
 * fail at the host-ABI boundary, independently of earlier type checking. */
static void test_frozen_profile_rejection(void){
    static bool ran;if(ran)return;ran=true;
    Fixture f;if(!setup_at(&f,SOL_TEST_SOURCE_DIR "/tests/conformance/p34_unsupported")){CHECK(false);finish(&f);return;}
    SolMirConcreteProgram p;sol_mir_concrete_program_init(&p);
    SolMirProgramRoot root={find(&f.ir,"launch",SOL_IR_CALLABLE_FUNCTION),SOL_MIR_PROGRAM_ROOT_ENTRY};
    SolIrCallableId import=find(&f.ir,"scalar",SOL_IR_CALLABLE_CAPABILITY);
    SolMirTargetDescriptor target=sol_mir_target_wasm32();
    CHECK(sol_mir_concrete_program_build(&(SolMirConcreteBuildRequest){&f.ir,&root,1,&import,1,&target,NULL},&p,&f.d)==SOL_MIR_CONCRETE_BUILD_SUCCEEDED);
    SolMirRuntimeConventions c;SolMirRuntimeValues v;SolMirRuntimeCleanup cl;SolMirRuntimeHostAbi a;
    sol_mir_runtime_conventions_init(&c);sol_mir_runtime_values_init(&v);sol_mir_runtime_cleanup_init(&cl);sol_mir_runtime_host_abi_init(&a);
    CHECK(sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&p,NULL},&c,&f.d)==SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&c,NULL},&v,&f.d)==SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&c,&v,NULL},&cl,&f.d)==SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&c,&v,&cl,NULL},&a,&f.d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_UNSUPPORTED);
    sol_mir_runtime_host_abi_free(&a);sol_mir_runtime_cleanup_free(&cl);sol_mir_runtime_values_free(&v);sol_mir_runtime_conventions_free(&c);sol_mir_concrete_program_free(&p);finish(&f);
}
/* The frozen profile is a unique subset, rather than an all-four requirement.
 * This also ensures the Probe-only rejection reaches the ABI after all three
 * predecessor owners have successfully built. */
static void test_frozen_profile_subset(void){
    static bool ran;if(ran)return;ran=true;
    Fixture f;if(!setup_at(&f,SOL_TEST_SOURCE_DIR "/tests/conformance/p34_console")){CHECK(false);finish(&f);return;}
    SolMirConcreteProgram p;sol_mir_concrete_program_init(&p);
    SolMirProgramRoot root={find(&f.ir,"launch",SOL_IR_CALLABLE_FUNCTION),SOL_MIR_PROGRAM_ROOT_ENTRY};
    SolIrCallableId import=find(&f.ir,"write",SOL_IR_CALLABLE_CAPABILITY);SolMirTargetDescriptor target=sol_mir_target_wasm32();
    CHECK(sol_mir_concrete_program_build(&(SolMirConcreteBuildRequest){&f.ir,&root,1,&import,1,&target,NULL},&p,&f.d)==SOL_MIR_CONCRETE_BUILD_SUCCEEDED);
    SolMirRuntimeConventions c;SolMirRuntimeValues v;SolMirRuntimeCleanup cl;SolMirRuntimeHostAbi a;
    sol_mir_runtime_conventions_init(&c);sol_mir_runtime_values_init(&v);sol_mir_runtime_cleanup_init(&cl);sol_mir_runtime_host_abi_init(&a);
    CHECK(sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&p,NULL},&c,&f.d)==SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&c,NULL},&v,&f.d)==SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&c,&v,NULL},&cl,&f.d)==SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&c,&v,&cl,NULL},&a,&f.d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED);
    CHECK(a.operation_count==1&&a.requirement_count==1&&sol_mir_runtime_host_abi_validate(&a,NULL));
    sol_mir_runtime_host_abi_free(&a);sol_mir_runtime_cleanup_free(&cl);sol_mir_runtime_values_free(&v);sol_mir_runtime_conventions_free(&c);sol_mir_concrete_program_free(&p);finish(&f);
}
static void test_fifth_lookalike_rejection(void){
    static bool ran;if(ran)return;ran=true;
    Fixture f;if(!setup_at(&f,SOL_TEST_SOURCE_DIR "/tests/conformance/p34_five")){CHECK(false);sol_diagnostics_render_human(stderr,&f.p.source,&f.d);finish(&f);return;}
    SolMirConcreteProgram p;sol_mir_concrete_program_init(&p);
    SolMirProgramRoot root={find(&f.ir,"launch",SOL_IR_CALLABLE_FUNCTION),SOL_MIR_PROGRAM_ROOT_ENTRY};
    SolIrCallableId imports[5];size_t count=0;for(size_t i=0;i<f.ir.callable_count;i++)if(f.ir.callables[i].kind==SOL_IR_CALLABLE_CAPABILITY&&(!strcmp(f.ir.callables[i].name,"write")||!strcmp(f.ir.callables[i].name,"count")||!strcmp(f.ir.callables[i].name,"get")||!strcmp(f.ir.callables[i].name,"read")))imports[count++]=i;
    SolMirTargetDescriptor target=sol_mir_target_wasm32();
    CHECK(count==5&&sol_mir_concrete_program_build(&(SolMirConcreteBuildRequest){&f.ir,&root,1,imports,count,&target,NULL},&p,&f.d)==SOL_MIR_CONCRETE_BUILD_SUCCEEDED);
    SolMirRuntimeConventions c;SolMirRuntimeValues v;SolMirRuntimeCleanup cl;SolMirRuntimeHostAbi a;
    sol_mir_runtime_conventions_init(&c);sol_mir_runtime_values_init(&v);sol_mir_runtime_cleanup_init(&cl);sol_mir_runtime_host_abi_init(&a);
    CHECK(sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&p,NULL},&c,&f.d)==SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&c,NULL},&v,&f.d)==SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&c,&v,NULL},&cl,&f.d)==SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED);
    CHECK(c.concrete->linkage.host_requirement_count==5);
    CHECK(sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&c,&v,&cl,NULL},&a,&f.d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_UNSUPPORTED);
    sol_mir_runtime_host_abi_free(&a);sol_mir_runtime_cleanup_free(&cl);sol_mir_runtime_values_free(&v);sol_mir_runtime_conventions_free(&c);sol_mir_concrete_program_free(&p);finish(&f);
}
/* Rendering must be independent of owner allocation, host-import discovery
 * order, and the lexical spelling of an equivalent package path. */
static void test_independent_render_determinism(const SolMirRuntimeHostAbi *first) {
    Fixture f; SolMirConcreteProgram p; SolMirRuntimeConventions c;
    SolMirRuntimeValues v; SolMirRuntimeCleanup cl; SolMirRuntimeHostAbi second;
    RenderBytes left,right;
    sol_mir_concrete_program_init(&p); sol_mir_runtime_conventions_init(&c);
    sol_mir_runtime_values_init(&v); sol_mir_runtime_cleanup_init(&cl);
    sol_mir_runtime_host_abi_init(&second);
    if(!setup_at(&f,SOL_TEST_SOURCE_DIR "/tests/conformance/e6/../e6")) {
        CHECK(false); finish(&f); return;
    }
    SolMirProgramRoot roots[]={{find(&f.ir,"launch",SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_ENTRY}};
    const char *names[]={"read","count","get","write"};
    SolIrCallableId imports[4];
    for(size_t i=0;i<4;i++) imports[i]=find(&f.ir,names[i],SOL_IR_CALLABLE_CAPABILITY);
    SolMirTargetDescriptor target=sol_mir_target_wasm32();
    CHECK(sol_mir_concrete_program_build(&(SolMirConcreteBuildRequest){&f.ir,
        roots,1,imports,4,&target,NULL},&p,&f.d)==SOL_MIR_CONCRETE_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&p,NULL},
        &c,&f.d)==SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&c,NULL},&v,&f.d)
        ==SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&c,&v,NULL},
        &cl,&f.d)==SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&c,&v,&cl,NULL},
        &second,&f.d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED);
    CHECK(render_bytes((SolMirRuntimeHostAbi *)first,&left));
    CHECK(render_bytes(&second,&right));
    CHECK(left.size==right.size&&memcmp(left.bytes,right.bytes,left.size)==0);
    render_bytes_free(&left); render_bytes_free(&right);
    sol_mir_runtime_host_abi_free(&second); sol_mir_runtime_cleanup_free(&cl);
    sol_mir_runtime_values_free(&v); sol_mir_runtime_conventions_free(&c);
    sol_mir_concrete_program_free(&p); finish(&f);
}
static void test_host_call_cleanup_links(const SolMirRuntimeHostAbi *abi,
    const SolMirRuntimeCleanup *cleanup) {
    for (size_t i = 0; i < abi->requirement_count; ++i) {
        const SolMirRuntimeHostRequirement *requirement = &abi->requirements[i];
        CHECK(requirement->event < cleanup->event_count
            && requirement->failure_transition < cleanup->transition_count);
        if (requirement->event >= cleanup->event_count
            || requirement->failure_transition >= cleanup->transition_count) continue;
        const SolMirRuntimeCleanupEvent *event = &cleanup->events[requirement->event];
        const SolMirRuntimeCleanupTransition *transition =
            &cleanup->transitions[requirement->failure_transition];
        CHECK(event->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
            && event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
            && event->producer == SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_INVOKE
            && event->semantic_site == SOL_MIR_RUNTIME_NONE
            && transition->event == requirement->event
            && transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE
            && transition->failure_source
                == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31);
    }
}
static const char *text_setup_stage_name(TextSetupStage stage){switch(stage){case TEXT_SETUP_SOURCE:return "source_from_text";case TEXT_SETUP_LEX:return "lex";case TEXT_SETUP_PARSE:return "parse";case TEXT_SETUP_HIR:return "hir_lower";case TEXT_SETUP_TYPE:return "type_check";case TEXT_SETUP_EFFECT:return "effect_check";case TEXT_SETUP_CONTRACT:return "contract_lower";case TEXT_SETUP_IR:return "ir_lower";case TEXT_SETUP_OK:return "complete";}return "unknown";}
static bool setup_text(TextFixture *f,const char *text,TextSetupStage *failed_stage){
    memset(f,0,sizeof(*f));sol_tokens_init(&f->tokens);sol_diagnostics_init(&f->d);sol_syntax_tree_init(&f->syntax);sol_hir_module_init(&f->h);sol_type_table_init(&f->t);sol_effect_table_init(&f->e);sol_contract_table_init(&f->c);sol_ir_init(&f->ir);
    *failed_stage=TEXT_SETUP_SOURCE;if(!sol_source_from_text(&f->source,"host_capability_equality.sol",text))return false;
    *failed_stage=TEXT_SETUP_LEX;if(!sol_lex(&f->source,&f->tokens,&f->d)||sol_diagnostics_has_errors(&f->d))return false;
    *failed_stage=TEXT_SETUP_PARSE;if(!sol_parse(&f->source,&f->tokens,&f->syntax,&f->d)||sol_diagnostics_has_errors(&f->d))return false;
    *failed_stage=TEXT_SETUP_HIR;if(!sol_hir_lower(&f->source,&f->syntax,&f->h,&f->d)||sol_diagnostics_has_errors(&f->d))return false;
    *failed_stage=TEXT_SETUP_TYPE;if(!sol_type_check(&f->source,&f->syntax,&f->h,&f->t,&f->d)||sol_diagnostics_has_errors(&f->d))return false;
    *failed_stage=TEXT_SETUP_EFFECT;if(!sol_effect_check(&f->source,&f->syntax,&f->h,&f->t,&f->e,&f->d)||sol_diagnostics_has_errors(&f->d))return false;
    *failed_stage=TEXT_SETUP_CONTRACT;if(!sol_contract_lower(&f->source,&f->syntax,&f->h,&f->t,&f->e,&f->c,&f->d)||sol_diagnostics_has_errors(&f->d))return false;
    *failed_stage=TEXT_SETUP_IR;if(!sol_ir_lower(&f->source,&f->syntax,&f->h,&f->t,&f->e,&f->c,&f->ir,&f->d)||sol_diagnostics_has_errors(&f->d))return false;
    *failed_stage=TEXT_SETUP_OK;return true;
}
static void finish_text(TextFixture *f){sol_ir_free(&f->ir);sol_contract_table_free(&f->c);sol_effect_table_free(&f->e);sol_type_table_free(&f->t);sol_hir_module_free(&f->h);sol_syntax_tree_free(&f->syntax);sol_tokens_free(&f->tokens);sol_source_free(&f->source);sol_diagnostics_free(&f->d);}
static bool object_bytes_are_zero(const void *object,size_t size){const unsigned char *bytes=object;for(size_t i=0;i<size;i++)if(bytes[i]!=0)return false;return true;}
static bool text_downstream_state_is_empty(const TextFixture *f){return object_bytes_are_zero(&f->h,sizeof(f->h))&&object_bytes_are_zero(&f->t,sizeof(f->t))&&object_bytes_are_zero(&f->e,sizeof(f->e))&&object_bytes_are_zero(&f->c,sizeof(f->c))&&object_bytes_are_zero(&f->ir,sizeof(f->ir));}
static void test_text_setup_diagnostic_gate(void){
    static const char malformed_source[]=
        "module host.capability.equality\n"
        "@entry public function launch() -> () effects {} {}\n";
    static const char valid_source[]=
        "module host.capability_equality\n"
        "@entry public function launch() -> () effects {} {}\n";
    TextFixture malformed;TextSetupStage failed_stage=TEXT_SETUP_OK;
    bool malformed_ok=setup_text(&malformed,malformed_source,&failed_stage);
    CHECK(!malformed_ok);
    CHECK(failed_stage==TEXT_SETUP_PARSE);
    CHECK(sol_diagnostics_has_errors(&malformed.d));
    CHECK(malformed.d.count>0);
    CHECK(text_downstream_state_is_empty(&malformed));
    if(malformed.source.text)sol_diagnostics_render_human(stderr,&malformed.source,&malformed.d);
    finish_text(&malformed);

    TextFixture valid;TextSetupStage valid_stage=TEXT_SETUP_SOURCE;
    bool valid_ok=setup_text(&valid,valid_source,&valid_stage);
    CHECK(valid_ok);
    CHECK(valid_stage==TEXT_SETUP_OK);
    CHECK(!sol_diagnostics_has_errors(&valid.d));
    CHECK(find(&valid.ir,"launch",SOL_IR_CALLABLE_FUNCTION)!=SOL_IR_NONE);
    finish_text(&valid);
}
static void equality_pipeline_init(EqualityPipeline *p){memset(p,0,sizeof(*p));sol_mir_concrete_program_init(&p->concrete);sol_mir_runtime_conventions_init(&p->conventions);sol_mir_runtime_values_init(&p->values);sol_mir_runtime_cleanup_init(&p->cleanup);sol_mir_runtime_host_abi_init(&p->host);}
static void equality_pipeline_free(EqualityPipeline *p){sol_mir_runtime_host_abi_free(&p->host);sol_mir_runtime_cleanup_free(&p->cleanup);sol_mir_runtime_values_free(&p->values);sol_mir_runtime_conventions_free(&p->conventions);sol_mir_concrete_program_free(&p->concrete);}
static bool equality_pipeline_build(EqualityPipeline *p,const SolIr *ir,SolDiagnostics *d,const char *const *imports,size_t import_count){if(import_count>8)return false;SolIrCallableId import_ids[8];SolIrCallableId launch=find(ir,"launch",SOL_IR_CALLABLE_FUNCTION);if(launch==SOL_IR_NONE)return false;for(size_t i=0;i<import_count;i++){import_ids[i]=find(ir,imports[i],SOL_IR_CALLABLE_CAPABILITY);if(import_ids[i]==SOL_IR_NONE)return false;}SolMirProgramRoot root={launch,SOL_MIR_PROGRAM_ROOT_ENTRY};SolMirTargetDescriptor target=sol_mir_target_wasm32();if(sol_mir_concrete_program_build(&(SolMirConcreteBuildRequest){ir,&root,1,import_ids,import_count,&target,NULL},&p->concrete,d)!=SOL_MIR_CONCRETE_BUILD_SUCCEEDED)return false;if(sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&p->concrete,NULL},&p->conventions,d)!=SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED)return false;if(sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&p->conventions,NULL},&p->values,d)!=SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED)return false;if(sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&p->conventions,&p->values,NULL},&p->cleanup,d)!=SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED)return false;return sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&p->conventions,&p->values,&p->cleanup,NULL},&p->host,d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED;}
typedef struct { const char *name; size_t offset,size; } CapabilityExtent;
static const CapabilityExtent capability_extents[]={
    {"recipe",offsetof(SolMirRuntimeHostCapabilityPlan,recipe),sizeof(((SolMirRuntimeHostCapabilityPlan *)0)->recipe)},
    {"source",offsetof(SolMirRuntimeHostCapabilityPlan,source),sizeof(((SolMirRuntimeHostCapabilityPlan *)0)->source)},
    {"source_construct",offsetof(SolMirRuntimeHostCapabilityPlan,source_construct),sizeof(((SolMirRuntimeHostCapabilityPlan *)0)->source_construct)},
    {"result",offsetof(SolMirRuntimeHostCapabilityPlan,result),sizeof(((SolMirRuntimeHostCapabilityPlan *)0)->result)},
    {"temporary",offsetof(SolMirRuntimeHostCapabilityPlan,temporary),sizeof(((SolMirRuntimeHostCapabilityPlan *)0)->temporary)},
    {"source_value",offsetof(SolMirRuntimeHostCapabilityPlan,source_value),sizeof(((SolMirRuntimeHostCapabilityPlan *)0)->source_value)},
    {"source_temporary",offsetof(SolMirRuntimeHostCapabilityPlan,source_temporary),sizeof(((SolMirRuntimeHostCapabilityPlan *)0)->source_temporary)},
    {"source_place",offsetof(SolMirRuntimeHostCapabilityPlan,source_place),sizeof(((SolMirRuntimeHostCapabilityPlan *)0)->source_place)},
    {"source_root",offsetof(SolMirRuntimeHostCapabilityPlan,source_root),sizeof(((SolMirRuntimeHostCapabilityPlan *)0)->source_root)},
    {"parent",offsetof(SolMirRuntimeHostCapabilityPlan,parent),sizeof(((SolMirRuntimeHostCapabilityPlan *)0)->parent)},
    {"private_recipe",offsetof(SolMirRuntimeHostCapabilityPlan,private_recipe),sizeof(((SolMirRuntimeHostCapabilityPlan *)0)->private_recipe)},
    {"private_value",offsetof(SolMirRuntimeHostCapabilityPlan,private_value),sizeof(((SolMirRuntimeHostCapabilityPlan *)0)->private_value)}
};
typedef struct { SolMirRuntimeHostAbi header; void *arenas[8]; size_t bytes[8]; } HostOwnerSnapshot;
static bool host_owner_snapshot(const SolMirRuntimeHostAbi *o,HostOwnerSnapshot *s){
    const void *arenas[]={o->capabilities,o->entry_roots,o->operations,o->arguments,o->formals,o->shapes,o->shape_cases,o->requirements};
    const size_t capacities[]={o->capability_capacity,o->entry_root_capacity,o->operation_capacity,o->argument_capacity,o->formal_capacity,o->shape_capacity,o->shape_case_capacity,o->requirement_capacity};
    const size_t widths[]={sizeof(*o->capabilities),sizeof(*o->entry_roots),sizeof(*o->operations),sizeof(*o->arguments),sizeof(*o->formals),sizeof(*o->shapes),sizeof(*o->shape_cases),sizeof(*o->requirements)};
    memset(s,0,sizeof(*s));memcpy(&s->header,o,sizeof(*o));
    for(size_t i=0;i<8;i++){if(capacities[i]>SIZE_MAX/widths[i])return false;s->bytes[i]=capacities[i]*widths[i];if(s->bytes[i]&&!arenas[i])return false;if(s->bytes[i]){s->arenas[i]=malloc(s->bytes[i]);if(!s->arenas[i])return false;memcpy(s->arenas[i],arenas[i],s->bytes[i]);}}
    return true;
}
static bool host_owner_snapshot_matches(const SolMirRuntimeHostAbi *o,const HostOwnerSnapshot *s){
    const void *arenas[]={o->capabilities,o->entry_roots,o->operations,o->arguments,o->formals,o->shapes,o->shape_cases,o->requirements};
    if(memcmp(o,&s->header,sizeof(*o))!=0)return false;
    for(size_t i=0;i<8;i++)if(s->bytes[i]&&memcmp(arenas[i],s->arenas[i],s->bytes[i])!=0)return false;
    return true;
}
static void host_owner_snapshot_free(HostOwnerSnapshot *s){for(size_t i=0;i<8;i++)free(s->arenas[i]);memset(s,0,sizeof(*s));}
typedef struct { SolMirRuntimeHostRootBinding *roots; SolMirRuntimeHostGrant *grants; SolMirRuntimeHostPreflightRequest request; } EqualityPreflight;
static bool equality_preflight_make(const SolMirRuntimeHostAbi *o,EqualityPreflight *p){
    memset(p,0,sizeof(*p));if(!o->entry_root_count)return false;SolMirRuntimeEntryId entry=o->entry_roots[0].entry;
    p->roots=calloc(o->entry_root_count,sizeof(*p->roots));p->grants=calloc(o->requirement_count?o->requirement_count:1,sizeof(*p->grants));if(!p->roots||!p->grants)return false;
    for(size_t i=0;i<o->entry_root_count;i++)if(o->entry_roots[i].entry==entry)p->roots[p->request.root_count++]=(SolMirRuntimeHostRootBinding){i,(uint64_t)i+1};
    for(size_t i=0;i<o->requirement_count;i++)if(o->requirements[i].entry==entry){bool seen=false;for(size_t q=0;q<p->request.grant_count;q++)seen|=p->grants[q].root==o->requirements[i].root&&p->grants[q].operation==o->requirements[i].operation;if(!seen)p->grants[p->request.grant_count++]=(SolMirRuntimeHostGrant){o->requirements[i].root,o->requirements[i].operation};}
    p->request=(SolMirRuntimeHostPreflightRequest){entry,p->roots,p->request.root_count,p->grants,p->request.grant_count};return true;
}
static void equality_preflight_free(EqualityPreflight *p){free(p->roots);free(p->grants);memset(p,0,sizeof(*p));}
static bool capability_field_oracle(const SolMirRuntimeHostCapabilityPlan *a,const SolMirRuntimeHostCapabilityPlan *b){return a->recipe==b->recipe&&a->source==b->source&&a->source_construct==b->source_construct&&a->result==b->result&&a->temporary==b->temporary&&a->source_value==b->source_value&&a->source_temporary==b->source_temporary&&a->source_place==b->source_place&&a->source_root==b->source_root&&a->parent==b->parent&&a->private_recipe==b->private_recipe&&a->private_value==b->private_value;}
static bool capability_has_padding(void){bool used[sizeof(SolMirRuntimeHostCapabilityPlan)]={0};for(size_t i=0;i<sizeof(capability_extents)/sizeof(capability_extents[0]);i++){if(capability_extents[i].offset+capability_extents[i].size>sizeof(used))return false;for(size_t q=0;q<capability_extents[i].size;q++)used[capability_extents[i].offset+q]=true;}for(size_t i=0;i<sizeof(used);i++)if(!used[i])return true;return false;}
static void poison_capability_padding(SolMirRuntimeHostCapabilityPlan *row,unsigned char byte){unsigned char *bytes=(unsigned char *)row;bool used[sizeof(*row)]={0};for(size_t i=0;i<sizeof(capability_extents)/sizeof(capability_extents[0]);i++)for(size_t q=0;q<capability_extents[i].size;q++)used[capability_extents[i].offset+q]=true;for(size_t i=0;i<sizeof(*row);i++)if(!used[i])bytes[i]=byte;}
static void mutate_capability_member(SolMirRuntimeHostCapabilityPlan *row,size_t member,const SolMirRuntimeHostAbi *o){
    size_t *slot=NULL,limit=0;
    switch(member){case 0:slot=&row->recipe;limit=o->conventions->concrete->representation.recipe_count;break;case 1:row->source=row->source==SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT?SOL_MIR_RUNTIME_HOST_CAPABILITY_DERIVED:SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT;return;case 2:slot=&row->source_construct;limit=o->conventions->concrete->operations.constructor_count;break;case 3:slot=&row->result;limit=o->conventions->concrete->materialization.value_count;break;case 4:slot=&row->temporary;limit=o->conventions->concrete->materialization.temporary_count;break;case 5:slot=&row->source_value;limit=o->conventions->concrete->materialization.value_count;break;case 6:slot=&row->source_temporary;limit=o->conventions->concrete->materialization.temporary_count;break;case 7:slot=&row->source_place;limit=o->conventions->concrete->materialization.place_count;break;case 8:slot=&row->source_root;limit=o->entry_root_count;break;case 9:slot=&row->parent;limit=o->capability_count;break;case 10:slot=&row->private_recipe;limit=o->conventions->concrete->representation.recipe_count;break;default:slot=&row->private_value;limit=o->conventions->concrete->materialization.value_count;break;}
    size_t original=*slot;if(limit>1)*slot=original==0?1:0;else *slot=original==SOL_MIR_RUNTIME_NONE?0:SOL_MIR_RUNTIME_NONE;
}
static void reject_host_mutation_without_writes(SolMirRuntimeHostAbi *o){HostOwnerSnapshot snapshot;CHECK(host_owner_snapshot(o,&snapshot));CHECK(!sol_mir_runtime_host_abi_validate(o,NULL));check_render_rejects_mutation(o);CHECK(host_owner_snapshot_matches(o,&snapshot));host_owner_snapshot_free(&snapshot);}
static void test_capability_comparator_domain(const SolMirRuntimeHostAbi *o,const SolMirRuntimeHostCapabilityPlan *real){
    SolMirRuntimeHostCapabilityPlan a,b;memcpy(&a,real,sizeof(a));memcpy(&b,real,sizeof(b));
    CHECK(sol_mir_runtime_host_abi_test_capability_fields_equal(&a,&b));
    CHECK(!sol_mir_runtime_host_abi_test_capability_fields_equal(NULL,&b));
    b.source=a.source==SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT?SOL_MIR_RUNTIME_HOST_CAPABILITY_DERIVED:SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT;
    CHECK(!sol_mir_runtime_host_abi_test_capability_fields_equal(&a,&b));
    CHECK(!sol_mir_runtime_host_abi_test_capability_fields_equal(&b,&a));
    b=a;b.source=SOL_MIR_RUNTIME_HOST_CAPABILITY_PRIVATE_SOURCE;b.private_recipe=b.recipe;b.private_value=b.source_value;
    CHECK(sol_mir_runtime_host_abi_test_capability_fields_equal(&b,&b));
    a=b;SolMirRuntimeHostCapabilitySource unsupported=(SolMirRuntimeHostCapabilitySource)(SOL_MIR_RUNTIME_HOST_CAPABILITY_PRIVATE_SOURCE+1);
    memcpy(&a.source,&unsupported,sizeof(a.source));memcpy(&b.source,&unsupported,sizeof(b.source));
    CHECK(!sol_mir_runtime_host_abi_test_capability_fields_equal(&a,&b));
    (void)o;
}
static size_t count_substring(const char *text,const char *needle){
    size_t count=0,n=strlen(needle);
    for(const char *p=text;(p=strstr(p,needle))!=NULL;p+=n)++count;
    return count;
}
static void capability_row_hex(const unsigned char *bytes,size_t length,char *hex){
    static const char digits[]="0123456789abcdef";
    for(size_t i=0;i<length;i++){hex[2*i]=digits[bytes[i]>>4];hex[2*i+1]=digits[bytes[i]&15];}
    hex[2*length]='\0';
}
static void test_serialized_capability_comparison(
    const SolMirRuntimeHostAbiObservationContext *context,const char *identity){
    RenderBytes serialized={0};CHECK(observation_bytes(context,&serialized));
    if(!serialized.bytes)return;
    CHECK(strstr((const char *)serialized.bytes,identity)!=NULL);
    CHECK(strstr((const char *)serialized.bytes,"\"operation\":\"capability_fields_equal\"")!=NULL);
    CHECK(strstr((const char *)serialized.bytes,"\"result\":null")!=NULL);
    CHECK(strstr((const char *)serialized.bytes,"\"equal\":false")!=NULL);
    CHECK(strstr((const char *)serialized.bytes,"\"left_role\":\"actual_owner_row\"")!=NULL);
    CHECK(strstr((const char *)serialized.bytes,"\"right_role\":\"original_expected_capability_object\"")!=NULL);
    CHECK(strstr((const char *)serialized.bytes,"\"left_availability\":\"captured\"")!=NULL);
    CHECK(strstr((const char *)serialized.bytes,"\"right_availability\":\"captured\"")!=NULL);
    char left_hex[sizeof(context->comparison_left)*2+1];
    char right_hex[sizeof(context->comparison_right)*2+1];
    capability_row_hex(context->comparison_left,context->comparison_length,left_hex);
    capability_row_hex(context->comparison_right,context->comparison_length,right_hex);
    char pair[sizeof(left_hex)+sizeof(right_hex)+32];
    snprintf(pair,sizeof(pair),"\"left_hex\":\"%s\",\"right_hex\":\"%s\"",left_hex,right_hex);
    CHECK(strstr((const char *)serialized.bytes,pair)!=NULL);
    CHECK(count_substring((const char *)serialized.bytes,"\"kind\":\"extent\"")==24);
    for(size_t i=0;i<sizeof(capability_extents)/sizeof(capability_extents[0]);i++){
        for(size_t side=0;side<2;side++){
            char extent[256];
            snprintf(extent,sizeof(extent),
                "\"side\":\"%s\",\"type\":\"SolMirRuntimeHostCapabilityPlan\",\"leaf\":\"%s\",\"offset\":\"%zu\",\"size\":\"%zu\"",
                side?"right":"left",capability_extents[i].name,
                capability_extents[i].offset,capability_extents[i].size);
            CHECK(strstr((const char *)serialized.bytes,extent)!=NULL);
        }
    }
    render_bytes_free(&serialized);
}
static void exercise_capability_witness(SolMirRuntimeHostAbi *o,size_t index){
    CHECK(index<o->capability_count);if(index>=o->capability_count)return;
    SolMirRuntimeHostCapabilityPlan *live=&o->capabilities[index],original;
    memcpy(&original,live,sizeof(original));CHECK(sol_mir_runtime_host_abi_validate(o,NULL));
    CHECK(sol_mir_runtime_host_abi_test_capability_fields_equal(&original,&original));
    for(size_t member=0;member<12;member++){
        SolMirRuntimeHostCapabilityPlan changed;memcpy(&changed,&original,sizeof(changed));mutate_capability_member(&changed,member,o);
        CHECK(!capability_field_oracle(&original,&changed));
        CHECK(!sol_mir_runtime_host_abi_test_capability_fields_equal(&original,&changed));
        CHECK(!sol_mir_runtime_host_abi_test_capability_fields_equal(&changed,&original));
    }
    bool has_padding=capability_has_padding();
    if(has_padding){
        SolMirRuntimeHostCapabilityPlan left,right;memcpy(&left,&original,sizeof(left));memcpy(&right,&original,sizeof(right));
        poison_capability_padding(&left,0xa5);poison_capability_padding(&right,0x5a);
        CHECK(capability_field_oracle(&left,&right));CHECK(memcmp(&left,&right,sizeof(left))!=0);
        CHECK(sol_mir_runtime_host_abi_test_capability_fields_equal(&left,&right));
        CHECK(sol_mir_runtime_host_abi_test_capability_fields_equal(&right,&left));
        RenderBytes before={0},after={0};CHECK(render_bytes(o,&before));
        uint64_t authentication=o->authentication;
        poison_capability_padding(live,0xc3);
        CHECK(capability_field_oracle(live,&original));CHECK(sol_mir_runtime_host_abi_validate(o,NULL));
        CHECK(o->authentication==authentication);
        EqualityPreflight preflight;CHECK(equality_preflight_make(o,&preflight));
        CHECK(sol_mir_runtime_host_abi_preflight(o,&preflight.request)==SOL_MIR_RUNTIME_HOST_PREFLIGHT_SUCCEEDED);
        equality_preflight_free(&preflight);
        CHECK(render_bytes(o,&after)&&before.size==after.size&&memcmp(before.bytes,after.bytes,before.size)==0);
        render_bytes_free(&after);render_bytes_free(&before);
    }else CHECK(true); /* Padding differential is NOT_APPLICABLE on such a host. */
    for(size_t member=0;member<12;member++){
        memcpy(live,&original,sizeof(original));mutate_capability_member(live,member,o);
        CHECK(!capability_field_oracle(&original,live));reject_host_mutation_without_writes(o);
        memcpy(live,&original,sizeof(original));CHECK(sol_mir_runtime_host_abi_validate(o,NULL));
    }
    memcpy(live,&original,sizeof(original));CHECK(sol_mir_runtime_host_abi_validate(o,NULL));
}
static void test_actual_validation_observation(SolMirRuntimeHostAbi *o,size_t index){
    CHECK(index<o->capability_count);if(index>=o->capability_count)return;
    SolMirRuntimeHostCapabilityPlan *row=&o->capabilities[index],original;
    memcpy(&original,row,sizeof(original));
    bool constructed=original.source_construct!=SOL_MIR_RUNTIME_NONE;
    row->source_construct=original.source_construct==0?1:0;
    HostOwnerSnapshot snapshot;CHECK(host_owner_snapshot(o,&snapshot));
    SolMirRuntimeHostAbiObservationContext context={0};context.validator_active=true;
    SolMirRuntimeHostAbiObservationContext *previous=sol_mir_runtime_host_abi_observation_context;
    sol_mir_runtime_host_abi_observation_context=&context;
    bool accepted=sol_mir_runtime_host_abi_validate(o,NULL);
    sol_mir_runtime_host_abi_observation_context=previous;
    CHECK(!accepted);CHECK(host_owner_snapshot_matches(o,&snapshot));host_owner_snapshot_free(&snapshot);
    CHECK(context.comparison_available&&context.comparison_is_fieldwise);
    const char *expected_id=constructed
        ?"H37.V.sol_mir_runtime_host_abi_internal_validate.394.capability_fields_equal"
        :"H37.V.sol_mir_runtime_host_abi_internal_validate.348.capability_fields_equal";
    CHECK(context.comparison_id&&strcmp(context.comparison_id,expected_id)==0);
    CHECK(context.comparison_operation&&strcmp(context.comparison_operation,"capability_fields_equal")==0);
    CHECK(context.comparison_left_available&&context.comparison_right_available);
    CHECK(!context.comparison_result_available&&context.comparison_equal_available&&!context.comparison_equal);
    CHECK(context.comparison_length==sizeof(*row)&&context.comparison_left_index==index);
    CHECK(context.comparison_right_index==(constructed?original.source_construct:original.source_root));
    CHECK(!memcmp(context.comparison_left,row,sizeof(*row)));
    SolMirRuntimeHostCapabilityPlan captured_left,captured_right,expected;
    if(constructed)memcpy(&expected,&original,sizeof(expected));
    else expected=(SolMirRuntimeHostCapabilityPlan){
        o->entry_roots[original.source_root].recipe,SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT,
        SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE,
        SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE,
        original.source_root,SOL_MIR_RUNTIME_NONE,SOL_MIR_RECIPE_NONE,SOL_MIR_RUNTIME_NONE};
    memcpy(&captured_left,context.comparison_left,sizeof(captured_left));
    memcpy(&captured_right,context.comparison_right,sizeof(captured_right));
    CHECK(capability_field_oracle(&captured_left,row));CHECK(capability_field_oracle(&captured_right,&expected));
    test_serialized_capability_comparison(&context,expected_id);
    size_t measured=SIZE_MAX-17;CHECK(!sol_mir_runtime_host_abi_internal_measure_validation_work(o,&measured));
    CHECK(measured==SIZE_MAX-17);
    memcpy(row,&original,sizeof(original));CHECK(sol_mir_runtime_host_abi_validate(o,NULL));
}
static void test_public_forged_private(SolMirRuntimeHostAbi *o,size_t index){
    CHECK(index<o->capability_count);if(index>=o->capability_count)return;
    SolMirRuntimeHostCapabilityPlan *row=&o->capabilities[index],original;memcpy(&original,row,sizeof(original));
    row->source=SOL_MIR_RUNTIME_HOST_CAPABILITY_PRIVATE_SOURCE;row->private_recipe=row->recipe;row->private_value=row->source_value;
    reject_host_mutation_without_writes(o);memcpy(row,&original,sizeof(original));CHECK(sol_mir_runtime_host_abi_validate(o,NULL));
}
static void test_public_unsupported_source(SolMirRuntimeHostAbi *o,size_t index){
    CHECK(index<o->capability_count);if(index>=o->capability_count)return;
    SolMirRuntimeHostCapabilityPlan *row=&o->capabilities[index],original;memcpy(&original,row,sizeof(original));
    SolMirRuntimeHostCapabilitySource unsupported=(SolMirRuntimeHostCapabilitySource)(SOL_MIR_RUNTIME_HOST_CAPABILITY_PRIVATE_SOURCE+1);
    memcpy(&row->source,&unsupported,sizeof(row->source));reject_host_mutation_without_writes(o);
    memcpy(row,&original,sizeof(original));CHECK(sol_mir_runtime_host_abi_validate(o,NULL));
}
static size_t find_root_capability(const SolMirRuntimeHostAbi *o){for(size_t i=0;i<o->capability_count;i++)if(o->capabilities[i].source==SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT&&o->capabilities[i].source_construct==SOL_MIR_RUNTIME_NONE&&o->capabilities[i].source_root<o->entry_root_count)return i;return SOL_MIR_RUNTIME_NONE;}
static size_t find_constructed_capability(const SolMirRuntimeHostAbi *o){for(size_t i=0;i<o->capability_count;i++)if(o->capabilities[i].source_construct!=SOL_MIR_RUNTIME_NONE)return i;return SOL_MIR_RUNTIME_NONE;}

static bool capability_render_keys_are_unique(const RenderBytes *render,
    size_t expected_count){
    if(!render||!render->bytes)return false;
    const char **keys=calloc(expected_count?expected_count:1,sizeof(*keys));
    size_t *lengths=calloc(expected_count?expected_count:1,sizeof(*lengths));
    if(!keys||!lengths){free(keys);free(lengths);return false;}
    size_t count=0;const char *line=(const char *)render->bytes;
    while(*line){
        if(strncmp(line,"capability key=",15)!=0){
            const char *end=strchr(line,'\n');if(!end)break;line=end+1;continue;
        }
        const char *key=line+15,*end=strchr(key,' ');
        if(!end||count>=expected_count){free(keys);free(lengths);return false;}
        size_t length=(size_t)(end-key);
        for(size_t i=0;i<count;i++)if(lengths[i]==length
            &&memcmp(keys[i],key,length)==0){free(keys);free(lengths);return false;}
        keys[count]=key;lengths[count]=length;++count;
        const char *newline=strchr(end,'\n');if(!newline)break;line=newline+1;
    }
    free(keys);free(lengths);return count==expected_count;
}

static bool capability_instruction_ordinal_witness(
    const SolMirRuntimeHostAbi *owner,size_t *constructor,
    SolMirValueKind *value_kind,size_t *ordinal,size_t *definition_count){
    if(!owner||!owner->conventions||!owner->conventions->concrete
        ||!constructor||!value_kind||!ordinal||!definition_count)return false;
    const SolMirConcreteProgram *concrete=owner->conventions->concrete;
    const SolMirMaterialization *materialization=&concrete->materialization;
    const SolMirOperations *operations=&concrete->operations;
    const SolIr *ir=concrete->program.ir;
    if(!ir)return false;
    for(size_t i=0;i<owner->capability_count;i++){
        const SolMirRuntimeHostCapabilityPlan *capability=&owner->capabilities[i];
        if(capability->source_construct==SOL_MIR_RUNTIME_NONE
            ||capability->source_value>=materialization->value_count
            ||capability->source_construct>=operations->constructor_count)continue;
        const SolMirOperationConstructPlan *construct=
            &operations->constructors[capability->source_construct];
        if(construct->kind!=SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY
            ||construct->image>=materialization->image_count)continue;
        const SolMirMaterializedImage *image=&materialization->images[construct->image];
        const SolMirMaterializedValue *source=
            &materialization->values[capability->source_value];
        if(source->kind!=SOL_MIR_VALUE_INSTRUCTION)continue;
        if(capability->source_value<image->values.offset
            ||capability->source_value-image->values.offset>=image->values.count
            ||source->source_definition>=image->instructions.count
            ||image->instructions.offset>materialization->instruction_count
            ||source->source_definition>materialization->instruction_count-image->instructions.offset
            ||source->instruction!=image->instructions.offset+source->source_definition
            ||source->instruction>=materialization->instruction_count
            ||materialization->instructions[source->instruction].result
                !=capability->source_value)return false;
        const SolMirProgramTemplate *template=NULL;
        for(size_t t=0;t<concrete->program.template_count;t++)
            if(concrete->program.templates[t].callable==image->source_callable){
                template=&concrete->program.templates[t];break;
            }
        if(!template)return false;
        size_t local_value=capability->source_value-image->values.offset;
        const SolMir *mir=&template->mir;
        if(local_value>=mir->value_count
            ||source->source_definition>=mir->instruction_count
            ||mir->values[local_value].kind!=SOL_MIR_VALUE_INSTRUCTION
            ||mir->values[local_value].definition!=source->source_definition
            ||mir->instructions[source->source_definition].result!=local_value)
            return false;
        if(source->source_definition<ir->definition_count)continue;
        *constructor=capability->source_construct;
        *value_kind=source->kind;
        *ordinal=source->source_definition;
        *definition_count=ir->definition_count;
        return true;
    }
    return false;
}

static void test_constructed_render_determinism(
    const SolMirRuntimeHostAbi *first){
    Fixture fixture;EqualityPipeline second;
    equality_pipeline_init(&second);
    CHECK(first&&first->capability_count>0);
    if(!first||!first->capability_count){equality_pipeline_free(&second);return;}
    const SolMirRuntimeHostCapabilityPlan *distinct_left=NULL,*distinct_right=NULL;
    for(size_t i=0;i<first->capability_count;i++){
        const SolMirRuntimeHostCapabilityPlan *left=&first->capabilities[i];
        if(left->source_construct==SOL_MIR_RUNTIME_NONE)continue;
        for(size_t j=i+1;j<first->capability_count;j++){
            const SolMirRuntimeHostCapabilityPlan *right=&first->capabilities[j];
            if(right->source_construct!=SOL_MIR_RUNTIME_NONE
                &&left->source_construct!=right->source_construct
                &&left->parent!=right->parent){
                distinct_left=left;distinct_right=right;break;
            }
        }
        if(distinct_left)break;
    }
    CHECK(distinct_left&&distinct_right);
    if(distinct_left&&distinct_right){
        CHECK(distinct_left->source_root!=distinct_right->source_root);
    }
    if(!setup_at(&fixture,SOL_TEST_SOURCE_DIR "/tests/conformance/p34/../p34")){
        CHECK(false);finish(&fixture);equality_pipeline_free(&second);return;
    }
    static const char *const imports[]={"read","get","count","write"};
    bool built=equality_pipeline_build(&second,&fixture.ir,&fixture.d,imports,4);
    CHECK(built);
    if(built){
        const SolMirRuntimeHostAbi *owner=&second.host;
        CHECK(sol_mir_runtime_host_abi_validate(owner,NULL));
        const SolMirRuntimeHostCapabilityPlan *other_left=NULL,*other_right=NULL;
        for(size_t i=0;i<owner->capability_count;i++){
            const SolMirRuntimeHostCapabilityPlan *left=&owner->capabilities[i];
            if(left->source_construct==SOL_MIR_RUNTIME_NONE)continue;
            for(size_t j=i+1;j<owner->capability_count;j++){
                const SolMirRuntimeHostCapabilityPlan *right=&owner->capabilities[j];
                if(right->source_construct!=SOL_MIR_RUNTIME_NONE
                    &&left->source_construct!=right->source_construct
                    &&left->parent!=right->parent){
                    other_left=left;other_right=right;break;
                }
            }
            if(other_left)break;
        }
        CHECK(other_left&&other_right);
        if(other_left&&other_right)
            CHECK(other_left->source_root!=other_right->source_root);
        RenderBytes left={0},right={0};
        bool left_ok=render_bytes((SolMirRuntimeHostAbi *)first,&left);
        bool right_ok=render_bytes(&second.host,&right);
        CHECK(left_ok&&right_ok);
        if(left_ok&&right_ok){
            CHECK(left.size==right.size&&memcmp(left.bytes,right.bytes,left.size)==0);
            CHECK(capability_render_keys_are_unique(&left,first->capability_count));
            CHECK(capability_render_keys_are_unique(&right,owner->capability_count));
        }
        render_bytes_free(&right);render_bytes_free(&left);
    }
    equality_pipeline_free(&second);finish(&fixture);
}

typedef struct {
    SolMirConcreteProgram concrete;
    SolMirRuntimeConventions conventions;
    SolMirRuntimeHostAbi owner;
    SolMirMaterialization materialization;
    SolMirOperations operations;
    SolIr ir;
    SolMirMaterializedInstruction *instructions;
    SolMirMaterializedValue *values;
    SolMirMaterializedPlace *places;
    SolMirMaterializedProjection *projections;
    SolMirOperationConstructPlan *constructors;
    SolIrLocal *locals;
} CapabilityLineageView;

typedef struct {
    size_t image;
    size_t staging;
    size_t load;
    size_t load_place;
    size_t local;
    size_t store;
} CapabilityLineageCoordinates;

static void capability_lineage_view_free(CapabilityLineageView *view){
    free(view->instructions);free(view->values);free(view->places);
    free(view->projections);free(view->constructors);free(view->locals);
    memset(view,0,sizeof(*view));
}

static bool capability_lineage_view_init(CapabilityLineageView *view,
    const SolMirRuntimeHostAbi *owner){
    memset(view,0,sizeof(*view));
    if(!owner||!owner->conventions||!owner->conventions->concrete
        ||!owner->conventions->concrete->program.ir)return false;
    const SolMirConcreteProgram *source=owner->conventions->concrete;
    view->concrete=*source;view->materialization=source->materialization;
    view->operations=source->operations;view->ir=*source->program.ir;
#define CLONE(member,count,type) do { \
    if(view->materialization.count){ \
        view->member=malloc(view->materialization.count*sizeof(type)); \
        if(!view->member)goto fail; \
        memcpy(view->member,view->materialization.member, \
            view->materialization.count*sizeof(type)); \
        view->materialization.member=view->member; \
    } \
} while(0)
    CLONE(instructions,instruction_count,SolMirMaterializedInstruction);
    CLONE(values,value_count,SolMirMaterializedValue);
    CLONE(places,place_count,SolMirMaterializedPlace);
    CLONE(projections,projection_count,SolMirMaterializedProjection);
#undef CLONE
    if(view->operations.constructor_count){
        view->constructors=malloc(view->operations.constructor_count
            *sizeof(*view->constructors));
        if(!view->constructors)goto fail;
        memcpy(view->constructors,view->operations.constructors,
            view->operations.constructor_count*sizeof(*view->constructors));
        view->operations.constructors=view->constructors;
    }
    if(view->ir.local_count){
        view->locals=malloc(view->ir.local_count*sizeof(*view->locals));
        if(!view->locals)goto fail;
        memcpy(view->locals,view->ir.locals,view->ir.local_count*sizeof(*view->locals));
        view->ir.locals=view->locals;
    }
    view->concrete.materialization=view->materialization;
    view->concrete.operations=view->operations;
    view->concrete.program.ir=&view->ir;
    view->conventions=*owner->conventions;
    view->conventions.concrete=&view->concrete;
    view->owner=*owner;view->owner.conventions=&view->conventions;
    return true;
fail:
    capability_lineage_view_free(view);return false;
}

static bool capability_lineage_coordinates(const SolMirRuntimeHostAbi *owner,
    size_t constructor,CapabilityLineageCoordinates *coordinates){
    if(!owner||!owner->conventions||!owner->conventions->concrete)return false;
    const SolMirConcreteProgram *concrete=owner->conventions->concrete;
    const SolMirMaterialization *m=&concrete->materialization;
    const SolMirOperations *ops=&concrete->operations;
    if(constructor>=ops->constructor_count||!ops->constructors)return false;
    const SolMirOperationConstructPlan *plan=&ops->constructors[constructor];
    if(plan->kind!=SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY
        ||plan->capability_rule!=SOL_MIR_OPERATION_CAPABILITY_BASE_SOURCE
        ||plan->capability_source_operand>=plan->operands.count
        ||plan->operands.offset>ops->construct_operand_count
        ||plan->operands.count>ops->construct_operand_count-plan->operands.offset)
        return false;
    size_t image=plan->image;
    if(image>=m->image_count)return false;
    const SolMirOperationConstructOperand *operand=&ops->construct_operands[
        plan->operands.offset+plan->capability_source_operand];
    size_t staging=SOL_MIR_RUNTIME_NONE,value=SOL_MIR_RUNTIME_NONE;
    for(size_t i=0;i<m->instruction_count;i++)
        if(m->instructions[i].kind==SOL_MIR_INST_TEMPORARY_INIT
            &&m->instructions[i].temporary==operand->temporary){
            if(staging!=SOL_MIR_RUNTIME_NONE)return false;
            staging=i;value=m->instructions[i].left;
        }
    if(staging==SOL_MIR_RUNTIME_NONE||value>=m->value_count)return false;
    size_t load=m->values[value].instruction;
    if(load>=m->instruction_count)return false;
    const SolMirMaterializedInstruction *load_row=&m->instructions[load];
    if((load_row->kind!=SOL_MIR_INST_LOAD_COPY
            &&load_row->kind!=SOL_MIR_INST_LOAD_MOVE)
        ||load_row->place>=m->place_count)return false;
    size_t local=m->places[load_row->place].local;
    if(local>=m->local_count)return false;
    size_t store=SOL_MIR_RUNTIME_NONE;
    const SolMirMaterializedImage *image_row=&m->images[image];
    for(size_t q=0;q<image_row->instructions.count;q++){
        size_t i=image_row->instructions.offset+q;
        const SolMirMaterializedInstruction *x=&m->instructions[i];
        if(x->kind!=SOL_MIR_INST_STORE||x->place>=m->place_count
            ||m->places[x->place].local!=local)continue;
        if(store!=SOL_MIR_RUNTIME_NONE)return false;store=i;
    }
    if(store==SOL_MIR_RUNTIME_NONE)return false;
    *coordinates=(CapabilityLineageCoordinates){image,staging,load,
        load_row->place,local,store};
    return true;
}

static bool capability_lineage_bridge_success(const SolMirRuntimeHostAbi *owner,
    size_t constructor,size_t expected_parent,size_t expected_root){
    size_t parent=SIZE_MAX-1,root=SIZE_MAX-2,used=0;
    SolMirRuntimeHostAbiLineageTestOutcome producer=
        sol_mir_runtime_host_abi_test_resolve_base_parent(owner,constructor,
            SIZE_MAX,&parent,&root,&used);
    CHECK(producer==SOL_MIR_RUNTIME_HOST_ABI_LINEAGE_TEST_SUCCEEDED);
    CHECK(parent==expected_parent&&root==expected_root&&used>0);
    size_t exact_parent=SIZE_MAX-1,exact_root=SIZE_MAX-2,exact_used=0;
    CHECK(sol_mir_runtime_host_abi_test_resolve_base_parent(owner,constructor,
        used,&exact_parent,&exact_root,&exact_used)
        ==SOL_MIR_RUNTIME_HOST_ABI_LINEAGE_TEST_SUCCEEDED);
    CHECK(exact_parent==expected_parent&&exact_root==expected_root
        &&exact_used==used);
    size_t short_parent=SIZE_MAX-1,short_root=SIZE_MAX-2,short_used=0;
    CHECK(sol_mir_runtime_host_abi_test_resolve_base_parent(owner,constructor,
        used-1,&short_parent,&short_root,&short_used)
        ==SOL_MIR_RUNTIME_HOST_ABI_LINEAGE_TEST_EXHAUSTED);
    CHECK(short_parent==SIZE_MAX-1&&short_root==SIZE_MAX-2
        &&short_used==used-1);

    parent=SIZE_MAX-1;root=SIZE_MAX-2;used=0;
    SolMirRuntimeHostAbiLineageTestOutcome validator=
        sol_mir_runtime_host_abi_test_validate_base_parent(owner,constructor,
            SIZE_MAX,&parent,&root,&used);
    CHECK(validator==SOL_MIR_RUNTIME_HOST_ABI_LINEAGE_TEST_SUCCEEDED);
    CHECK(parent==expected_parent&&root==expected_root&&used>0);
    exact_parent=SIZE_MAX-1;exact_root=SIZE_MAX-2;exact_used=0;
    CHECK(sol_mir_runtime_host_abi_test_validate_base_parent(owner,constructor,
        used,&exact_parent,&exact_root,&exact_used)
        ==SOL_MIR_RUNTIME_HOST_ABI_LINEAGE_TEST_SUCCEEDED);
    CHECK(exact_parent==expected_parent&&exact_root==expected_root
        &&exact_used==used);
    short_parent=SIZE_MAX-1;short_root=SIZE_MAX-2;short_used=0;
    CHECK(sol_mir_runtime_host_abi_test_validate_base_parent(owner,constructor,
        used-1,&short_parent,&short_root,&short_used)
        ==SOL_MIR_RUNTIME_HOST_ABI_LINEAGE_TEST_EXHAUSTED);
    CHECK(short_parent==SIZE_MAX-1&&short_root==SIZE_MAX-2
        &&short_used==used-1);
    return producer==SOL_MIR_RUNTIME_HOST_ABI_LINEAGE_TEST_SUCCEEDED
        &&validator==SOL_MIR_RUNTIME_HOST_ABI_LINEAGE_TEST_SUCCEEDED;
}

typedef enum {
    LINEAGE_DUPLICATE_STORE,
    LINEAGE_MISSING_STORE,
    LINEAGE_PROJECTED_LOAD,
    LINEAGE_PROJECTED_STORE,
    LINEAGE_STORE_AFTER_LOAD,
    LINEAGE_PRIOR_MOVE,
    LINEAGE_PRIOR_DROP,
    LINEAGE_MUTABLE_BINDING,
    LINEAGE_CROSS_BLOCK,
    LINEAGE_FORWARD_ORIGIN,
    LINEAGE_CYCLIC_ORIGIN,
    LINEAGE_PRIVATE_ANCESTOR,
    LINEAGE_NONCANONICAL_STORE_TYPE
} CapabilityLineageMutation;

static bool capability_lineage_add_projection(CapabilityLineageView *view,
    size_t place_id){
    SolMirMaterialization *m=&view->materialization;
    if(place_id>=m->place_count)return false;
    if(!m->projection_count){
        view->projections=malloc(sizeof(*view->projections));
        if(!view->projections)return false;
        view->projections[0]=(SolMirMaterializedProjection){0};
        m->projections=view->projections;m->projection_count=1;
        m->projection_capacity=1;
    }
    m->places[place_id].projections=(SolMirPlanSlice){0,1};
    view->concrete.materialization=*m;
    return true;
}

static bool capability_lineage_mutate(CapabilityLineageView *view,
    const SolMirRuntimeHostAbi *owner,size_t constructor,size_t ancestor,
    CapabilityLineageMutation mutation){
    CapabilityLineageCoordinates c;
    if(!capability_lineage_coordinates(owner,constructor,&c))return false;
    SolMirMaterialization *m=&view->materialization;
    SolMirMaterializedImage *image=&m->images[c.image];
    SolMirMaterializedInstruction *load=&m->instructions[c.load];
    SolMirMaterializedInstruction *store=&m->instructions[c.store];
    switch(mutation){
        case LINEAGE_DUPLICATE_STORE:{
            for(size_t i=image->instructions.offset;
                i<image->instructions.offset+image->instructions.count;i++){
                if(i==c.store)continue;
                SolMirMaterializedInstruction *x=&m->instructions[i];
                if(x->kind==SOL_MIR_INST_STORE) {
                    x->place=store->place;x->block=store->block;
                    x->left=store->left;x->type=store->type;
                    return true;
                }
            }
            for(size_t i=image->instructions.offset;
                i<image->instructions.offset+image->instructions.count;i++){
                if(i==c.store)continue;
                SolMirMaterializedInstruction *x=&m->instructions[i];
                if(x->result!=SOL_MIR_MATERIALIZED_NONE)continue;
                x->kind=SOL_MIR_INST_STORE;x->place=store->place;
                x->block=store->block;x->left=store->left;x->type=store->type;
                return true;
            }
            return false;
        }
        case LINEAGE_MISSING_STORE:
            store->kind=SOL_MIR_INST_CONST_UNIT;store->place=SOL_MIR_MATERIALIZED_NONE;
            return true;
        case LINEAGE_PROJECTED_LOAD:
            return capability_lineage_add_projection(view,c.load_place);
        case LINEAGE_PROJECTED_STORE:
            return capability_lineage_add_projection(view,store->place);
        case LINEAGE_STORE_AFTER_LOAD:{
            for(size_t i=c.load+1;
                i<image->instructions.offset+image->instructions.count;i++){
                if(m->instructions[i].block!=load->block
                    ||m->instructions[i].result!=SOL_MIR_MATERIALIZED_NONE)continue;
                SolMirMaterializedInstruction temporary=m->instructions[i];
                m->instructions[i]=*store;*store=temporary;
                return true;
            }
            return false;
        }
        case LINEAGE_PRIOR_MOVE:
        case LINEAGE_PRIOR_DROP:{
            for(size_t i=c.store+1;i<c.load;i++){
                SolMirMaterializedInstruction *x=&m->instructions[i];
                if(x->result!=SOL_MIR_MATERIALIZED_NONE)continue;
                x->kind=mutation==LINEAGE_PRIOR_MOVE
                    ?SOL_MIR_INST_LOAD_MOVE:SOL_MIR_INST_DROP_IF_INITIALIZED;
                x->block=load->block;x->local=c.local;
                x->place=mutation==LINEAGE_PRIOR_MOVE
                    ?c.load_place:SOL_MIR_MATERIALIZED_NONE;
                return true;
            }
            return false;
        }
        case LINEAGE_MUTABLE_BINDING:{
            size_t source_local=m->locals[c.local].source_local;
            if(source_local>=view->ir.local_count)return false;
            view->ir.locals[source_local].mutable=true;
            view->concrete.program.ir=&view->ir;
            return true;
        }
        case LINEAGE_CROSS_BLOCK:{
            for(size_t i=0;i<image->blocks.count;i++){
                const SolMirMaterializedBlock *block=
                    &m->blocks[image->blocks.offset+i];
                if(block->id==store->block)continue;
                store->block=block->id;return true;
            }
            return false;
        }
        case LINEAGE_FORWARD_ORIGIN:
            store->left=m->instructions[c.load].result;
            return true;
        case LINEAGE_CYCLIC_ORIGIN:{
            size_t previous_value=store->left;
            if(previous_value>=m->value_count)return false;
            size_t previous_load=m->values[previous_value].instruction;
            if(previous_load>=m->instruction_count)return false;
            size_t previous_place=m->instructions[previous_load].place;
            if(previous_place>=m->place_count)return false;
            size_t previous_local=m->places[previous_place].local;
            size_t previous_store=SOL_MIR_RUNTIME_NONE;
            for(size_t i=image->instructions.offset;
                i<image->instructions.offset+image->instructions.count;i++){
                const SolMirMaterializedInstruction *x=&m->instructions[i];
                if(x->kind!=SOL_MIR_INST_STORE||x->place>=m->place_count
                    ||m->places[x->place].local!=previous_local)continue;
                if(previous_store!=SOL_MIR_RUNTIME_NONE)return false;
                previous_store=i;
            }
            if(previous_store==SOL_MIR_RUNTIME_NONE)return false;
            m->instructions[previous_store].left=m->instructions[c.load].result;
            return true;
        }
        case LINEAGE_PRIVATE_ANCESTOR:
            if(ancestor>=view->operations.constructor_count)return false;
            view->constructors[ancestor].capability_rule=
                SOL_MIR_OPERATION_CAPABILITY_PRIVATE_SOURCE;
            view->operations.constructors=view->constructors;
            view->concrete.operations=view->operations;
            return true;
        case LINEAGE_NONCANONICAL_STORE_TYPE:
            if(store->place>=m->place_count)return false;
            store->type=m->places[store->place].root_type;
            return store->type!=SOL_MIR_MATERIALIZED_NONE;
    }
    return false;
}

static void capability_lineage_expect_mutation_rejected(
    const SolMirRuntimeHostAbi *owner,size_t constructor,size_t ancestor,
    CapabilityLineageMutation mutation){
    CapabilityLineageView view;
    CHECK(capability_lineage_view_init(&view,owner));
    if(!view.conventions.concrete)return;
    CHECK(capability_lineage_mutate(&view,owner,constructor,ancestor,mutation));
    size_t parent=SIZE_MAX-1,root=SIZE_MAX-2,used=0;
    CHECK(sol_mir_runtime_host_abi_test_resolve_base_parent(&view.owner,
        constructor,SIZE_MAX,&parent,&root,&used)
        ==SOL_MIR_RUNTIME_HOST_ABI_LINEAGE_TEST_REJECTED);
    CHECK(parent==SIZE_MAX-1&&root==SIZE_MAX-2);
    parent=SIZE_MAX-1;root=SIZE_MAX-2;used=0;
    CHECK(sol_mir_runtime_host_abi_test_validate_base_parent(&view.owner,
        constructor,SIZE_MAX,&parent,&root,&used)
        ==SOL_MIR_RUNTIME_HOST_ABI_LINEAGE_TEST_REJECTED);
    CHECK(parent==SIZE_MAX-1&&root==SIZE_MAX-2);
    capability_lineage_view_free(&view);
}

static void test_capability_lineage_bridges_and_rejections(
    const SolMirRuntimeHostAbi *owner,size_t outer_constructor,
    size_t inner_constructor,size_t expected_parent,size_t expected_root){
    CHECK(capability_lineage_bridge_success(owner,outer_constructor,
        expected_parent,expected_root));
    capability_lineage_expect_mutation_rejected(owner,outer_constructor,
        inner_constructor,LINEAGE_DUPLICATE_STORE);
    capability_lineage_expect_mutation_rejected(owner,outer_constructor,
        inner_constructor,LINEAGE_MISSING_STORE);
    capability_lineage_expect_mutation_rejected(owner,outer_constructor,
        inner_constructor,LINEAGE_PROJECTED_LOAD);
    capability_lineage_expect_mutation_rejected(owner,outer_constructor,
        inner_constructor,LINEAGE_PROJECTED_STORE);
    capability_lineage_expect_mutation_rejected(owner,outer_constructor,
        inner_constructor,LINEAGE_STORE_AFTER_LOAD);
    capability_lineage_expect_mutation_rejected(owner,outer_constructor,
        inner_constructor,LINEAGE_PRIOR_MOVE);
    capability_lineage_expect_mutation_rejected(owner,outer_constructor,
        inner_constructor,LINEAGE_PRIOR_DROP);
    capability_lineage_expect_mutation_rejected(owner,outer_constructor,
        inner_constructor,LINEAGE_MUTABLE_BINDING);
    capability_lineage_expect_mutation_rejected(owner,outer_constructor,
        inner_constructor,LINEAGE_CROSS_BLOCK);
    capability_lineage_expect_mutation_rejected(owner,outer_constructor,
        inner_constructor,LINEAGE_FORWARD_ORIGIN);
    capability_lineage_expect_mutation_rejected(owner,outer_constructor,
        inner_constructor,LINEAGE_CYCLIC_ORIGIN);
    capability_lineage_expect_mutation_rejected(owner,outer_constructor,
        inner_constructor,LINEAGE_PRIVATE_ANCESTOR);
    capability_lineage_expect_mutation_rejected(owner,outer_constructor,
        inner_constructor,LINEAGE_NONCANONICAL_STORE_TYPE);
}

static size_t capability_lineage_fixture_root(
    const SolMirRuntimeHostAbi *owner,const char *parameter_name){
    if(!owner||!owner->conventions||!owner->conventions->concrete
        ||!owner->conventions->concrete->program.ir)return SOL_MIR_RUNTIME_NONE;
    const SolMirRuntimeConventions *conventions=owner->conventions;
    const SolMirConcreteProgram *concrete=conventions->concrete;
    const SolMirMaterialization *m=&concrete->materialization;
    const SolMirLinkage *linkage=&concrete->linkage;
    const SolIr *ir=concrete->program.ir;
    size_t found=SOL_MIR_RUNTIME_NONE;
    for(size_t r=0;r<owner->entry_root_count;r++){
        const SolMirRuntimeHostEntryRoot *root=&owner->entry_roots[r];
        if(root->entry>=conventions->entry_count)continue;
        const SolMirRuntimeEntry *entry=&conventions->entries[root->entry];
        if(entry->callable>=linkage->callable_count)continue;
        SolMirPlanInstanceId instance=linkage->callables[entry->callable].instance;
        for(size_t i=0;i<m->local_count;i++){
            const SolMirMaterializedLocal *local=&m->locals[i];
            if(local->kind!=SOL_MIR_MATERIALIZED_LOCAL_PARAMETER
                ||local->instance!=instance||local->ordinal!=root->formal
                ||local->source_local>=ir->local_count
                ||!ir->locals[local->source_local].name
                ||strcmp(ir->locals[local->source_local].name,parameter_name))
                continue;
            if(found!=SOL_MIR_RUNTIME_NONE)return SOL_MIR_RUNTIME_NONE;
            found=r;
        }
    }
    return found;
}

static bool capability_lineage_fixture_constructors(
    const SolMirRuntimeHostAbi *owner,const SolIr *ir,const char *definition,
    size_t expected_count,size_t *plan_ids,size_t *capability_ids){
    if(!owner||!owner->conventions||!owner->conventions->concrete||!ir)
        return false;
    const SolMirMaterialization *m=
        &owner->conventions->concrete->materialization;
    const SolMirOperations *ops=&owner->conventions->concrete->operations;
    size_t found=0,capability_id=owner->entry_root_count;
    for(size_t q=0;q<ops->constructor_count;q++){
        const SolMirOperationConstructPlan *plan=&ops->constructors[q];
        if(plan->kind==SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY){
            if(plan->image>=m->image_count)return false;
            const SolMirMaterializedImage *image=&m->images[plan->image];
            if(plan->instruction>=m->instruction_count
                ||image->instructions.offset>m->instruction_count
                ||image->instructions.count>m->instruction_count-image->instructions.offset
                ||plan->instruction<image->instructions.offset
                ||plan->instruction-image->instructions.offset>=image->instructions.count)
                return false;
            const SolMirMaterializedInstruction *instruction=
                &m->instructions[plan->instruction];
            if(instruction->kind==SOL_MIR_INST_CONSTRUCT
                &&instruction->construct_kind==SOL_MIR_CONSTRUCT_CAPABILITY
                &&instruction->construct_definition<ir->definition_count
                &&ir->definitions[instruction->construct_definition].name
                &&!strcmp(ir->definitions[instruction->construct_definition].name,
                    definition)){
                if(found>=expected_count)return false;
                plan_ids[found]=q;capability_ids[found]=capability_id;++found;
            }
            if(capability_id==SOL_MIR_RUNTIME_NONE)return false;
            ++capability_id;
        }
    }
    return found==expected_count;
}

static void test_capability_lineage_alias_fixture(void){
    static const char source[]=
        "module host.capability_lineage_alias\n"
        "capability Console { function write(value: Text) -> () effects { console.write<Self> } }\n"
        "capability Inner derives_from source: capability Console {}\n"
        "capability Outer derives_from source: capability Inner {}\n"
        "@entry public function launch(left: capability Console, right: capability Console) -> () effects { console.write<left>, console.write<right> } {\n"
        " left.write(\"left\")\n"
        " right.write(\"right\")\n"
        " let first = Inner { source = left }\n"
        " let alias = first\n"
        " let spacer = 7\n"
        " let first_outer = Outer { source = alias }\n"
        " let second = Inner { source = right }\n"
        " let alias2 = second\n"
        " let spacer2 = 8\n"
        " let second_outer = Outer { source = alias2 }\n"
        " if true {\n"
        "  let branch_spacer = 9\n"
        " } else {\n"
        "  let branch_spacer_else = 10\n"
        " }\n"
        "}\n";
    TextFixture text;EqualityPipeline pipeline;equality_pipeline_init(&pipeline);
    TextSetupStage failed=TEXT_SETUP_OK;
    bool text_ok=setup_text(&text,source,&failed);
    if(!text_ok){
        fprintf(stderr,"capability lineage alias fixture failed at frontend stage=%s\n",
            text_setup_stage_name(failed));
        if(text.source.text)sol_diagnostics_render_human(stderr,&text.source,&text.d);
        CHECK(text_ok);
    }else if(!equality_pipeline_build(&pipeline,&text.ir,&text.d,
            (const char *const[]){"write"},1)){
        sol_diagnostics_render_human(stderr,&text.source,&text.d);CHECK(false);
    }else{
        const SolMirRuntimeHostAbi *owner=&pipeline.host;
        CHECK(sol_mir_runtime_host_abi_validate(owner,NULL));
        check_owner_renders(owner);
        size_t inner_constructors[2]={SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE};
        size_t inner_ids[2]={SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE};
        size_t outer_constructors[2]={SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE};
        size_t outer_ids[2]={SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE};
        size_t roots[2]={
            capability_lineage_fixture_root(owner,"left"),
            capability_lineage_fixture_root(owner,"right")};
        CHECK(capability_lineage_fixture_constructors(owner,&text.ir,"Inner",2,
            inner_constructors,inner_ids));
        CHECK(capability_lineage_fixture_constructors(owner,&text.ir,"Outer",2,
            outer_constructors,outer_ids));
        CHECK(roots[0]!=SOL_MIR_RUNTIME_NONE&&roots[1]!=SOL_MIR_RUNTIME_NONE
            &&roots[0]!=roots[1]);
        size_t inner_rows[2]={SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE};
        size_t outer_rows[2]={SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE};
        for(size_t i=0;i<2;i++){
            for(size_t row=0;row<owner->capability_count;row++){
                const SolMirRuntimeHostCapabilityPlan *cap=&owner->capabilities[row];
                if(cap->source_construct==inner_constructors[i]){
                    CHECK(inner_rows[i]==SOL_MIR_RUNTIME_NONE);
                    inner_rows[i]=row;
                }
                if(cap->source_construct==outer_constructors[i]){
                    CHECK(outer_rows[i]==SOL_MIR_RUNTIME_NONE);
                    outer_rows[i]=row;
                }
            }
        }
        CHECK(inner_ids[0]!=inner_ids[1]);
        for(size_t i=0;i<2;i++){
            CHECK(inner_rows[i]==inner_ids[i]&&outer_rows[i]==outer_ids[i]);
            if(inner_rows[i]!=SOL_MIR_RUNTIME_NONE){
                const SolMirRuntimeHostCapabilityPlan *inner=
                    &owner->capabilities[inner_rows[i]];
                CHECK(inner->source==SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT
                    &&inner->parent==roots[i]&&inner->source_root==roots[i]);
            }
            if(outer_rows[i]!=SOL_MIR_RUNTIME_NONE){
                const SolMirRuntimeHostCapabilityPlan *outer=
                    &owner->capabilities[outer_rows[i]];
                CHECK(outer->source==SOL_MIR_RUNTIME_HOST_CAPABILITY_DERIVED
                    &&outer->source_construct==outer_constructors[i]
                    &&outer->parent==inner_ids[i]
                    &&outer->source_root==roots[i]);
            }
        }
        if(inner_rows[0]!=SOL_MIR_RUNTIME_NONE
            &&inner_rows[1]!=SOL_MIR_RUNTIME_NONE
            &&outer_rows[0]!=SOL_MIR_RUNTIME_NONE
            &&outer_rows[1]!=SOL_MIR_RUNTIME_NONE){
            CHECK(owner->capabilities[inner_rows[0]].recipe
                ==owner->capabilities[inner_rows[1]].recipe);
            SolMirRuntimeHostCapabilityPlan *forged=
                &pipeline.host.capabilities[outer_rows[0]];
            size_t saved_parent=forged->parent,saved_root=forged->source_root;
            forged->parent=inner_ids[1];forged->source_root=roots[1];
            reject_host_mutation_without_writes(&pipeline.host);
            forged->parent=saved_parent;forged->source_root=saved_root;
            CHECK(sol_mir_runtime_host_abi_validate(&pipeline.host,NULL));
            forged->parent=inner_ids[1];
            CHECK(forged->source_root==roots[0]);
            reject_host_mutation_without_writes(&pipeline.host);
            forged->parent=saved_parent;
            CHECK(forged->source_root==saved_root);
            CHECK(sol_mir_runtime_host_abi_validate(&pipeline.host,NULL));
            for(size_t i=0;i<2;i++){
                test_capability_lineage_bridges_and_rejections(owner,
                    outer_constructors[i],inner_constructors[i],inner_ids[i],
                    roots[i]);
            }
        }
        SolMirRuntimeHostAbiWorkCensus census=
            sol_mir_runtime_host_abi_test_work_census();
        CHECK(census.dry_work==census.actual_work);
        CHECK(census.census_work+census.dry_work+census.actual_work
            ==owner->usage.build_work);
        CHECK(census.validation_audit_work+census.validation_replay_work
            ==owner->usage.validation_work);
        size_t measured=0;
        CHECK(sol_mir_runtime_host_abi_internal_measure_validation_work(owner,
            &measured));
        CHECK(measured==owner->usage.validation_work);
        SolMirRuntimeHostAbiLimits exact=owner->limits;
        exact.max_build_work=owner->usage.build_work;
        exact.max_validation_work=owner->usage.validation_work;
        SolMirRuntimeHostAbi bounded;sol_mir_runtime_host_abi_init(&bounded);
        CHECK(sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){
            &pipeline.conventions,&pipeline.values,&pipeline.cleanup,&exact},
            &bounded,&text.d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED);
        sol_mir_runtime_host_abi_free(&bounded);
        if(owner->usage.build_work){
            SolMirRuntimeHostAbi failed_owner;sol_mir_runtime_host_abi_init(&failed_owner);
            SolMirRuntimeHostAbiLimits below=exact;
            below.max_build_work=owner->usage.build_work-1;
            CHECK(sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){
                &pipeline.conventions,&pipeline.values,&pipeline.cleanup,&below},
                &failed_owner,&text.d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED);
            CHECK(failed_owner.capabilities==NULL&&failed_owner.capability_count==0
                &&failed_owner.requirements==NULL&&failed_owner.requirement_count==0);
            sol_mir_runtime_host_abi_free(&failed_owner);
        }
        if(owner->usage.validation_work>1){
            SolMirRuntimeHostAbi validation_limited;
            sol_mir_runtime_host_abi_init(&validation_limited);
            SolMirRuntimeHostAbiLimits below_validation=exact;
            below_validation.max_validation_work=
                owner->usage.validation_work-1;
            CHECK(sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){
                &pipeline.conventions,&pipeline.values,&pipeline.cleanup,
                &below_validation},&validation_limited,&text.d)
                ==SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED);
            CHECK(validation_limited.capabilities==NULL
                &&validation_limited.capability_count==0
                &&validation_limited.entry_roots==NULL
                &&validation_limited.entry_root_count==0
                &&validation_limited.operations==NULL
                &&validation_limited.operation_count==0
                &&validation_limited.arguments==NULL
                &&validation_limited.argument_count==0
                &&validation_limited.formals==NULL
                &&validation_limited.formal_count==0
                &&validation_limited.shapes==NULL
                &&validation_limited.shape_count==0
                &&validation_limited.shape_cases==NULL
                &&validation_limited.shape_case_count==0
                &&validation_limited.requirements==NULL
                &&validation_limited.requirement_count==0
                &&validation_limited.authentication==0);
            sol_mir_runtime_host_abi_free(&validation_limited);
        }
        size_t saved_limit=pipeline.host.limits.max_validation_work;
        pipeline.host.limits.max_validation_work=owner->usage.validation_work;
        CHECK(sol_mir_runtime_host_abi_validate(&pipeline.host,NULL));
        if(owner->usage.validation_work){
            pipeline.host.limits.max_validation_work=owner->usage.validation_work-1;
            CHECK(!sol_mir_runtime_host_abi_validate(&pipeline.host,NULL));
        }
        pipeline.host.limits.max_validation_work=saved_limit;
        CHECK(sol_mir_runtime_host_abi_validate(&pipeline.host,NULL));
    }
    equality_pipeline_free(&pipeline);finish_text(&text);
}

static void test_host_capability_equality_body(void){
    test_text_setup_diagnostic_gate();
    static const char *const e6_imports[]={"write","get","count","read"};
    static const char *const p34_imports[]={"write","count","get","read"};
    static const char embedded_source[]=
        "module host.capability_equality\n"
        "capability Console { function write(value: Text) -> () effects { console.write<Self> } }\n"
        "capability Inner derives_from source: capability Console {}\n"
        "capability Outer derives_from source: capability Inner {}\n"
        "function relay(source: borrow capability Console) -> () effects { console.write<source> } { source.write(\"chain\") }\n"
        "@entry public function launch(root: capability Console) -> () effects { console.write<root> } {\n"
        " relay(root)\n"
        " let first = Inner { source = root }\n"
        " let second = Outer { source = first }\n"
        "}\n";
    Fixture e6,p34;EqualityPipeline e6_pipeline,p34_pipeline;
    equality_pipeline_init(&e6_pipeline);equality_pipeline_init(&p34_pipeline);
    CHECK(setup_at(&e6,SOL_TEST_SOURCE_DIR "/tests/conformance/e6"));
    if(e6.p.files&&equality_pipeline_build(&e6_pipeline,&e6.ir,&e6.d,e6_imports,4)){
        SolMirRuntimeHostAbi *o=&e6_pipeline.host;size_t root=find_root_capability(o);
        CHECK(root!=SOL_MIR_RUNTIME_NONE);CHECK(o->entry_root_count==3);
        if(root!=SOL_MIR_RUNTIME_NONE){exercise_capability_witness(o,root);test_actual_validation_observation(o,root);test_capability_comparator_domain(o,&o->capabilities[root]);test_public_unsupported_source(o,root);}
    }else CHECK(false);
    CHECK(setup_at(&p34,SOL_TEST_SOURCE_DIR "/tests/conformance/p34"));
    if(p34.p.files&&equality_pipeline_build(&p34_pipeline,&p34.ir,&p34.d,p34_imports,4)){
        SolMirRuntimeHostAbi *o=&p34_pipeline.host;size_t root=find_root_capability(o),constructed=find_constructed_capability(o);
        CHECK(root!=SOL_MIR_RUNTIME_NONE&&constructed!=SOL_MIR_RUNTIME_NONE);
        size_t witness_constructor=SOL_MIR_RUNTIME_NONE,witness_ordinal=SOL_MIR_RUNTIME_NONE,
            witness_definition_count=SOL_MIR_RUNTIME_NONE;
        SolMirValueKind witness_kind=SOL_MIR_VALUE_BLOCK_PARAMETER;
        bool witness=capability_instruction_ordinal_witness(o,&witness_constructor,
            &witness_kind,&witness_ordinal,&witness_definition_count);
        CHECK(witness);
        if(witness){
            CHECK(witness_kind==SOL_MIR_VALUE_INSTRUCTION);
            CHECK(witness_ordinal>=witness_definition_count);
            fprintf(stderr,
                "constructor=%zu value-kind=%u ordinal=%zu definition-count=%zu\n",
                witness_constructor,(unsigned)witness_kind,witness_ordinal,
                witness_definition_count);
        }
        bool same_typed_roots=false;size_t wrong_root=SOL_MIR_RUNTIME_NONE;
        if(root!=SOL_MIR_RUNTIME_NONE)for(size_t i=0;i<o->capability_count;i++)if(i!=root&&o->capabilities[i].source==SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT&&o->capabilities[i].source_construct==SOL_MIR_RUNTIME_NONE&&o->capabilities[i].recipe==o->capabilities[root].recipe&&o->capabilities[i].source_root!=o->capabilities[root].source_root){same_typed_roots=true;wrong_root=o->capabilities[i].source_root;break;}
        CHECK(same_typed_roots);
        if(root!=SOL_MIR_RUNTIME_NONE){exercise_capability_witness(o,root);if(wrong_root!=SOL_MIR_RUNTIME_NONE){SolMirRuntimeHostCapabilityPlan *row=&o->capabilities[root];size_t saved=row->source_root;row->source_root=wrong_root;reject_host_mutation_without_writes(o);row->source_root=saved;CHECK(sol_mir_runtime_host_abi_validate(o,NULL));}}
        if(constructed!=SOL_MIR_RUNTIME_NONE){exercise_capability_witness(o,constructed);test_capability_comparator_domain(o,&o->capabilities[constructed]);test_public_forged_private(o,constructed);}
        check_owner_renders(o);
        test_constructed_render_determinism(o);
    }else CHECK(false);
    TextFixture text;EqualityPipeline embedded;equality_pipeline_init(&embedded);
    TextSetupStage setup_failure=TEXT_SETUP_OK;
    bool text_ok=setup_text(&text,embedded_source,&setup_failure);
    if(!text_ok){
        fprintf(stderr,"embedded capability fixture failed at frontend stage=%s\n",text_setup_stage_name(setup_failure));
        if(text.source.text)sol_diagnostics_render_human(stderr,&text.source,&text.d);
        CHECK(text_ok);
    }else if(!equality_pipeline_build(&embedded,&text.ir,&text.d,(const char *const[]){"write"},1)){
        sol_diagnostics_render_human(stderr,&text.source,&text.d);CHECK(false);
    }else{
        CHECK(setup_failure==TEXT_SETUP_OK);
        CHECK(!sol_diagnostics_has_errors(&text.d));
        SolMirRuntimeHostAbi *o=&embedded.host;size_t root=find_root_capability(o);
        size_t inner_root=SOL_MIR_RUNTIME_NONE,outer_derived=SOL_MIR_RUNTIME_NONE;
        CHECK(root!=SOL_MIR_RUNTIME_NONE);CHECK(sol_mir_runtime_host_abi_validate(o,NULL));
        check_owner_renders(o);
        for(size_t i=0;i<o->capability_count;i++){
            const SolMirRuntimeHostCapabilityPlan *candidate=&o->capabilities[i];
            if(candidate->source_construct==SOL_MIR_RUNTIME_NONE)continue;
            if(candidate->source==SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT
                &&candidate->parent==candidate->source_root
                &&candidate->source_root<o->entry_root_count
                &&root!=SOL_MIR_RUNTIME_NONE
                &&candidate->source_root==o->capabilities[root].source_root)
                inner_root=i;
        }
        if(inner_root!=SOL_MIR_RUNTIME_NONE)for(size_t i=0;i<o->capability_count;i++){
            const SolMirRuntimeHostCapabilityPlan *candidate=&o->capabilities[i];
            if(candidate->source==SOL_MIR_RUNTIME_HOST_CAPABILITY_DERIVED
                &&candidate->source_construct!=SOL_MIR_RUNTIME_NONE
                &&candidate->parent==inner_root
                &&candidate->source_root==o->capabilities[inner_root].source_root)
                outer_derived=i;
        }
        CHECK(inner_root!=SOL_MIR_RUNTIME_NONE&&outer_derived!=SOL_MIR_RUNTIME_NONE);
        if(inner_root!=SOL_MIR_RUNTIME_NONE){
            CHECK(o->capabilities[inner_root].source==SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT);
            CHECK(o->capabilities[inner_root].parent==o->capabilities[inner_root].source_root);
            exercise_capability_witness(o,inner_root);
        }
        if(outer_derived!=SOL_MIR_RUNTIME_NONE){
            CHECK(o->capabilities[outer_derived].source==SOL_MIR_RUNTIME_HOST_CAPABILITY_DERIVED);
            CHECK(o->capabilities[outer_derived].parent==inner_root);
            CHECK(o->capabilities[outer_derived].source_root==o->capabilities[inner_root].source_root);
            test_actual_validation_observation(o,outer_derived);
            exercise_capability_witness(o,outer_derived);
            test_capability_comparator_domain(o,&o->capabilities[outer_derived]);
            test_public_forged_private(o,outer_derived);
        }
        CHECK(o->requirement_count>0);
        if(root!=SOL_MIR_RUNTIME_NONE)for(size_t i=0;i<o->requirement_count;i++)
            CHECK(o->requirements[i].root==o->capabilities[root].source_root);
    }
    equality_pipeline_free(&embedded);finish_text(&text);
    equality_pipeline_free(&p34_pipeline);equality_pipeline_free(&e6_pipeline);finish(&p34);finish(&e6);
    test_capability_lineage_alias_fixture();
}
#define test_option_result_transfer(...) SOL_TEST_RUN(test_option_result_transfer(__VA_ARGS__))
#define test_render_and_semantic_seal(...) SOL_TEST_RUN(test_render_and_semantic_seal(__VA_ARGS__))
#define check_render_rejects_mutation(...) SOL_TEST_RUN(check_render_rejects_mutation(__VA_ARGS__))
#define test_exact_host_abi_limits(...) SOL_TEST_RUN(test_exact_host_abi_limits(__VA_ARGS__))
#define test_host_call_cleanup_links(...) SOL_TEST_RUN(test_host_call_cleanup_links(__VA_ARGS__))
#define test_independent_render_determinism(...) SOL_TEST_RUN(test_independent_render_determinism(__VA_ARGS__))
#define test_distinct_roots_and_derived_sources(...) SOL_TEST_RUN(test_distinct_roots_and_derived_sources(__VA_ARGS__))
int main(void){Fixture f;CHECK(setup(&f));SolMirConcreteProgram p;sol_mir_concrete_program_init(&p);SolMirProgramRoot root={find(&f.ir,"launch",SOL_IR_CALLABLE_FUNCTION),SOL_MIR_PROGRAM_ROOT_ENTRY};const char*names[]={"write","get","count","read"};SolIrCallableId imports[4];for(size_t i=0;i<4;i++)imports[i]=find(&f.ir,names[i],SOL_IR_CALLABLE_CAPABILITY);SolMirTargetDescriptor target=sol_mir_target_wasm32();SolMirConcreteBuildRequest request={&f.ir,&root,1,imports,4,&target,NULL};CHECK(sol_mir_concrete_program_build(&request,&p,&f.d)==SOL_MIR_CONCRETE_BUILD_SUCCEEDED);SolMirRuntimeConventions c;SolMirRuntimeValues v;SolMirRuntimeCleanup cl;SolMirRuntimeHostAbi a;sol_mir_runtime_conventions_init(&c);sol_mir_runtime_values_init(&v);sol_mir_runtime_cleanup_init(&cl);sol_mir_runtime_host_abi_init(&a);CHECK(sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&p,NULL},&c,&f.d)==SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED);CHECK(sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&c,NULL},&v,&f.d)==SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED);CHECK(sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&c,&v,NULL},&cl,&f.d)==SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED);size_t forbidden=0,unreachable=0;for(size_t i=0;i<v.host_result_plan_count;i++){forbidden+=v.host_result_plans[i].classification==SOL_MIR_RUNTIME_HOST_RESULT_FORBIDDEN;unreachable+=v.host_result_plans[i].classification==SOL_MIR_RUNTIME_HOST_RESULT_UNREACHABLE;}CHECK(forbidden!=0&&unreachable!=0);size_t host_result=c.concrete->linkage.host_requirements[0].result;SolMirRuntimeHostResultClass saved_class=v.host_result_plans[host_result].classification;SolMirRuntimeHostAbi rejected_owner;sol_mir_runtime_host_abi_init(&rejected_owner);v.host_result_plans[host_result].classification=SOL_MIR_RUNTIME_HOST_RESULT_FORBIDDEN;CHECK(sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&c,&v,&cl,NULL},&rejected_owner,&f.d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR);v.host_result_plans[host_result].classification=SOL_MIR_RUNTIME_HOST_RESULT_UNREACHABLE;CHECK(sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&c,&v,&cl,NULL},&rejected_owner,&f.d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR);v.host_result_plans[host_result].classification=saved_class;sol_mir_runtime_host_abi_free(&rejected_owner);CHECK(sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&c,&v,&cl,NULL},&a,&f.d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED);CHECK(sol_mir_runtime_host_abi_validate(&a,NULL));CHECK(a.operation_count==4&&a.requirement_count==5&&a.entry_root_count==3);
    size_t expected_validation_work = 0;
    CHECK(sol_test_path_work(15958, 4, sizeof SOL_TEST_SOURCE_DIR - 1, &expected_validation_work));
    CHECK(a.usage.capabilities==3&&a.usage.entry_roots==3&&a.usage.operations==4
        &&a.usage.arguments==3&&a.usage.formals==3&&a.usage.shapes==3
        &&a.usage.shape_cases==0&&a.usage.requirements==5&&a.usage.grants==4
        &&a.usage.owned_bytes==1816&&a.usage.build_scratch_bytes==185
           &&a.usage.build_work==36153&&a.usage.validation_scratch_bytes==77
             &&a.usage.validation_work==expected_validation_work);SolMirRuntimeHostAbiWorkCensus meter=sol_mir_runtime_host_abi_test_work_census();CHECK(meter.dry_work==meter.actual_work&&meter.census_work+meter.dry_work+meter.actual_work==a.usage.build_work&&meter.validation_audit_work+meter.validation_replay_work==meter.validation_work&&meter.validation_work==a.usage.validation_work);
    /* A one-below total reaches the final persistent replay tick.  It has made
     * persistent allocation attempts, but cannot publish any partial owner. */
    SolMirRuntimeHostAbi final_write;sol_mir_runtime_host_abi_init(&final_write);
    SolMirRuntimeHostAbiLimits final_write_limits=a.limits;
    final_write_limits.max_build_work=a.usage.build_work-1;
    sol_mir_runtime_host_abi_test_reset_allocation_attempts();
    CHECK(sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&c,&v,&cl,
        &final_write_limits},&final_write,&f.d)
        ==SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED);
    CHECK(sol_mir_runtime_host_abi_test_allocation_attempts()!=0
        &&final_write.capabilities==NULL&&final_write.capability_count==0
        &&final_write.requirements==NULL&&final_write.requirement_count==0);
    sol_mir_runtime_host_abi_free(&final_write);
    /* Audit and replay each consume their remaining validation budget. */
    size_t saved_validation_limit=a.limits.max_validation_work;
    a.limits.max_validation_work=meter.validation_audit_work-1;
    CHECK(!sol_mir_runtime_host_abi_validate(&a,NULL));
    a.limits.max_validation_work=a.usage.validation_work-1;
    CHECK(!sol_mir_runtime_host_abi_validate(&a,NULL));
    a.limits.max_validation_work=saved_validation_limit;
    CHECK(sol_mir_runtime_host_abi_validate(&a,NULL));SolMirRuntimeHostRootBinding roots[3];SolMirRuntimeHostGrant grants[4];for(size_t i=0;i<3;i++)roots[i]=(SolMirRuntimeHostRootBinding){i,i+1};size_t grant_count=0;for(size_t i=0;i<a.requirement_count;i++){bool seen=false;for(size_t q=0;q<grant_count;q++)seen|=grants[q].root==a.requirements[i].root&&grants[q].operation==a.requirements[i].operation;if(!seen)grants[grant_count++]=(SolMirRuntimeHostGrant){a.requirements[i].root,a.requirements[i].operation};}SolMirRuntimeHostPreflightRequest pre={0,roots,3,grants,grant_count};CHECK(sol_mir_runtime_host_abi_preflight(&a,&pre)==SOL_MIR_RUNTIME_HOST_PREFLIGHT_SUCCEEDED);CHECK(sol_mir_runtime_host_abi_preflight(&a,&(SolMirRuntimeHostPreflightRequest){0,roots,2,grants,grant_count})==SOL_MIR_RUNTIME_HOST_PREFLIGHT_INVALID);size_t unit=0;for(size_t i=0;i<a.requirement_count;i++)if(a.operations[a.requirements[i].operation].result_class==SOL_MIR_RUNTIME_HOST_RESULT_UNIT){unit=i;break;}SolMirRuntimeHostValue text={.kind=SOL_MIR_RUNTIME_HOST_VALUE_TEXT};text.as.text.bytes=(const uint8_t*)"x";text.as.text.length=1;const SolMirRuntimeHostValue*args[]={&text};size_t calls=0;SolMirRuntimeAllocationQuota quota={100,10000};SolMirRuntimeAllocationUsage usage={0};SolMirRuntimeHostInvocationResult result;SolMirRuntimeHostInvocationRequest invoke={&a,&pre,a.requirements[unit].root,a.requirements[unit].operation,a.requirements[unit].call,args,1,callback,NULL,1,&calls,&quota,&usage,NULL};CHECK(sol_mir_runtime_host_abi_test_invoke(&invoke,&result)==SOL_MIR_RUNTIME_HOST_INVOKE_SUCCEEDED);sol_mir_runtime_host_owned_value_free(result.value);
    calls=0;invoke.callback=host_error_callback;CHECK(sol_mir_runtime_host_abi_test_invoke(&invoke,&result)==SOL_MIR_RUNTIME_HOST_INVOKE_HOST_ERROR);CHECK(result.failure.code==SOL_MIR_RUNTIME_FAILURE_HOST_ERROR&&result.failure.length==1&&result.detail[0]=='x'&&calls==1);CHECK(result.cleanup.event==a.requirements[unit].event&&result.cleanup.edge_role==SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE&&result.cleanup.has_primary&&result.cleanup.primary.code==SOL_MIR_RUNTIME_FAILURE_HOST_ERROR&&result.cleanup.primary.detail_length==1&&result.cleanup.primary.detail_bytes[0]=='x');
    size_t callbacks=0;SolMirRuntimeHostInvocationRequest bounded_invoke=invoke;
    bounded_invoke.callback=counting_callback;bounded_invoke.context=&callbacks;
    bounded_invoke.calls=&calls;calls=0;bounded_invoke.max_calls=0;
    CHECK(sol_mir_runtime_host_abi_test_invoke(&bounded_invoke,&result)==SOL_MIR_RUNTIME_HOST_INVOKE_CALL_LIMIT&&calls==0&&callbacks==0);
    bounded_invoke.max_calls=1;
    CHECK(sol_mir_runtime_host_abi_test_invoke(&bounded_invoke,&result)==SOL_MIR_RUNTIME_HOST_INVOKE_SUCCEEDED&&calls==1&&callbacks==1);sol_mir_runtime_host_owned_value_free(result.value);
    CHECK(sol_mir_runtime_host_abi_test_invoke(&bounded_invoke,&result)==SOL_MIR_RUNTIME_HOST_INVOKE_CALL_LIMIT&&calls==1&&callbacks==1);
    uint8_t detail_191[SOL_MIR_RUNTIME_HOST_DETAIL_MAX],detail_192[SOL_MIR_RUNTIME_HOST_DETAIL_MAX+1],detail_nul[]={'a',0};
    memset(detail_191,'d',sizeof(detail_191));memset(detail_192,'d',sizeof(detail_192));
    HostErrorDetail details[]={{NULL,0},{detail_nul,sizeof(detail_nul)},{detail_191,sizeof(detail_191)},{detail_192,sizeof(detail_192)}};
    for(size_t i=0;i<sizeof(details)/sizeof(details[0]);i++){calls=0;invoke.callback=host_error_detail_callback;invoke.context=&details[i];
        SolMirRuntimeHostInvocationOutcome expected=i==0||i==2?SOL_MIR_RUNTIME_HOST_INVOKE_HOST_ERROR:SOL_MIR_RUNTIME_HOST_INVOKE_INVALID;
        CHECK(sol_mir_runtime_host_abi_test_invoke(&invoke,&result)==expected&&calls==1);
        if(expected==SOL_MIR_RUNTIME_HOST_INVOKE_HOST_ERROR)CHECK(result.failure.length==details[i].length&&result.failure.detail_kind==SOL_MIR_RUNTIME_FAILURE_DETAIL_HOST_BYTES);}
    invoke.callback=callback;invoke.context=NULL;
    test_option_result_transfer(&a,&pre);
    test_render_and_semantic_seal(&a,&pre,&invoke,&result);
    /* A forged owner arena must be rejected before the P2 representation is
       traversed, and rendering a rejected owner remains a zero-write action. */
    SolMirRuntimeHostCapabilityPlan *saved_capabilities=a.capabilities;
    a.capabilities=(SolMirRuntimeHostCapabilityPlan *)(void *)p.representation.recipes;
    CHECK(!sol_mir_runtime_host_abi_validate(&a,NULL));check_render_rejects_mutation(&a);
    a.capabilities=saved_capabilities;
    CHECK(sol_mir_runtime_host_abi_validate(&a,NULL));
    /* The hostile matrix covers independent owner arenas as well as every
     * predecessor generation.  Restore each pointer before the next case: the
     * test deliberately never frees an aliased arena. */
#define REJECT_ARENA_ALIAS(member, type, replacement) do { \
    type *saved=(a.member); a.member=(type *)(void *)(replacement); \
    CHECK(!sol_mir_runtime_host_abi_validate(&a,NULL)); \
    check_render_rejects_mutation(&a); a.member=saved; \
    CHECK(sol_mir_runtime_host_abi_validate(&a,NULL)); \
} while(0)
    REJECT_ARENA_ALIAS(capabilities,SolMirRuntimeHostCapabilityPlan,a.entry_roots);
    REJECT_ARENA_ALIAS(operations,SolMirRuntimeHostOperation,a.formals);
    REJECT_ARENA_ALIAS(shapes,SolMirRuntimeHostShape,a.shape_cases);
    REJECT_ARENA_ALIAS(requirements,SolMirRuntimeHostRequirement,a.arguments);
    REJECT_ARENA_ALIAS(capabilities,SolMirRuntimeHostCapabilityPlan,c.imports);
    REJECT_ARENA_ALIAS(operations,SolMirRuntimeHostOperation,v.host_result_plans);
    REJECT_ARENA_ALIAS(requirements,SolMirRuntimeHostRequirement,cl.transitions);
    /* Keep the full borrowed-range category census visible: nested template
     * MIR, image topology, source storage, P2, and immediate predecessors. */
    REJECT_ARENA_ALIAS(capabilities,SolMirRuntimeHostCapabilityPlan,
        p.program.templates[0].mir.instructions);
    REJECT_ARENA_ALIAS(capabilities,SolMirRuntimeHostCapabilityPlan,
        p.materialization.images[0].topology.blocks);
    REJECT_ARENA_ALIAS(capabilities,SolMirRuntimeHostCapabilityPlan,p.program.ir->source_bytes);
    REJECT_ARENA_ALIAS(capabilities,SolMirRuntimeHostCapabilityPlan,p.program.ir->source_bytes+1);
    REJECT_ARENA_ALIAS(capabilities,SolMirRuntimeHostCapabilityPlan,p.program.ir->definitions[0].name+1);
    REJECT_ARENA_ALIAS(capabilities,SolMirRuntimeHostCapabilityPlan,p.program.roots);
    REJECT_ARENA_ALIAS(capabilities,SolMirRuntimeHostCapabilityPlan,cl.events);
#undef REJECT_ARENA_ALIAS
    SolMirInstruction *saved_template_instructions=p.program.templates[0].mir.instructions;
    p.program.templates[0].mir.instructions=NULL;
    CHECK(!sol_mir_runtime_host_abi_validate(&a,NULL));check_render_rejects_mutation(&a);
    p.program.templates[0].mir.instructions=saved_template_instructions;
    CHECK(sol_mir_runtime_host_abi_validate(&a,NULL));
    test_exact_host_abi_limits(&c,&v,&cl,&a,&f.d);test_host_call_cleanup_links(&a,&cl);
/* Preflight is O(1) and allocation-free after successful construction.  Its
 * immutable-owner prerequisite covers semantic arena bytes; header changes are
 * nevertheless rejected before configuration or callback handling. */
sol_mir_runtime_host_abi_test_reset_allocation_attempts();
CHECK(sol_mir_runtime_host_abi_preflight(&a,&pre)==SOL_MIR_RUNTIME_HOST_PREFLIGHT_SUCCEEDED);
CHECK(sol_mir_runtime_host_abi_test_allocation_attempts()==0);
size_t callback_count=0;SolMirRuntimeHostInvocationRequest rejected=invoke;rejected.callback=counting_callback;rejected.context=&callback_count;rejected.max_calls=2;
size_t saved_capacity=a.requirement_capacity;a.requirement_capacity--;
CHECK(sol_mir_runtime_host_abi_preflight(&a,&pre)==SOL_MIR_RUNTIME_HOST_PREFLIGHT_INVALID);
CHECK(sol_mir_runtime_host_abi_test_invoke(&rejected,&result)==SOL_MIR_RUNTIME_HOST_INVOKE_INVALID);CHECK(callback_count==0);
a.requirement_capacity=saved_capacity;
SolMirRuntimeHostRequirement saved_requirement=a.requirements[0];a.requirements[0].call=c.call_count;
CHECK(!sol_mir_runtime_host_abi_validate(&a,NULL));a.requirements[0]=saved_requirement;
CHECK(sol_mir_runtime_host_abi_validate(&a,NULL));
/* Exact resource boundaries are proved before any persistent arena allocation. */
SolMirRuntimeHostAbiLimits exact=a.limits;exact.max_capabilities=a.usage.capabilities;exact.max_entry_roots=a.usage.entry_roots;exact.max_operations=a.usage.operations;exact.max_formals=a.usage.formals;exact.max_shapes=a.usage.shapes;exact.max_shape_cases=a.usage.shape_cases?a.usage.shape_cases:1;exact.max_requirements=a.usage.requirements;exact.max_grants=a.usage.grants;exact.max_owned_bytes=a.usage.owned_bytes;exact.max_build_scratch_bytes=a.usage.build_scratch_bytes;exact.max_build_work=a.usage.build_work;exact.max_validation_scratch_bytes=a.usage.validation_scratch_bytes;exact.max_validation_work=a.usage.validation_work;
SolMirRuntimeHostAbi bounded;sol_mir_runtime_host_abi_init(&bounded);
    CHECK(sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&c,&v,&cl,&exact},&bounded,&f.d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED);sol_mir_runtime_host_abi_free(&bounded);
    --exact.max_owned_bytes;CHECK(sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&c,&v,&cl,&exact},&bounded,&f.d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED);
    ++exact.max_owned_bytes;size_t saved_validation_work=a.limits.max_validation_work;a.limits.max_validation_work=a.usage.validation_work-1;CHECK(!sol_mir_runtime_host_abi_validate(&a,NULL));CHECK(sol_mir_runtime_host_abi_preflight(&a,&pre)==SOL_MIR_RUNTIME_HOST_PREFLIGHT_INVALID);a.limits.max_validation_work=saved_validation_work;
sol_mir_runtime_host_abi_test_force_build_scratch_allocation_failure(true);exact.max_owned_bytes=a.usage.owned_bytes;CHECK(sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&c,&v,&cl,&exact},&bounded,&f.d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_ALLOCATION_FAILED);sol_mir_runtime_host_abi_test_force_build_scratch_allocation_failure(false);
test_independent_render_determinism(&a);sol_mir_runtime_host_abi_free(&a);sol_mir_runtime_cleanup_free(&cl);sol_mir_runtime_values_free(&v);sol_mir_runtime_conventions_free(&c);sol_mir_concrete_program_free(&p);finish(&f);test_distinct_roots_and_derived_sources();return failures?1:0;}
#undef main
static int sol_test_body(int argc,char **argv){
    if(argc==2&&strcmp(argv[1],"--host-capability-equality")==0){test_host_capability_equality_body();return failures?1:0;}
    if(argc!=1)return 2;
    return sol_test_default_body();
}
SOL_TEST_ARGV_ENTRY("mir_runtime_host_abi")
