#define SOL_MIR_PLAN_TEST_HOOKS 1
#include "sol/mir_runtime_host_abi.h"
#include "sol/effects.h"
#include "sol/lexer.h"
#include "sol/ownership.h"
#include "sol/package.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)
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
/* Snapshot exactly the renderer's byte stream.  The terminator is storage for
 * string-oriented assertions only: it is never part of the compared output. */
static bool render_bytes(SolMirRuntimeHostAbi *abi, RenderBytes *render) {
    if (!render) return false;
    *render=(RenderBytes){0}; FILE *stream=tmpfile(); if(!stream) return false;
    bool ok=sol_mir_runtime_host_abi_render(stream,abi)&&fflush(stream)==0
        &&fseek(stream,0,SEEK_END)==0;
    long end=ok?ftell(stream):-1;
    if(end<0||(uintmax_t)end>SIZE_MAX-1)ok=false;
    if(ok){render->size=(size_t)end;render->bytes=malloc(render->size+1);
        ok=render->bytes&&fseek(stream,0,SEEK_SET)==0
            &&fread(render->bytes,1,render->size,stream)==render->size;
        if(ok)render->bytes[render->size]='\0';}
    fclose(stream);if(!ok)render_bytes_free(render);return ok;
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
static SolIrCallableId find(const SolIr *ir,const char*n,SolIrCallableKind k){for(size_t i=0;i<ir->callable_count;i++)if(ir->callables[i].kind==k&&!strcmp(ir->callables[i].name,n))return i;return SOL_IR_NONE;}
static bool setup_at(Fixture *f,const char *directory){memset(f,0,sizeof(*f));sol_package_init(&f->p);sol_diagnostics_init(&f->d);sol_hir_module_init(&f->h);sol_type_table_init(&f->t);sol_effect_table_init(&f->e);sol_contract_table_init(&f->c);sol_ir_init(&f->ir);char error[256];if(!sol_package_load_directory(&f->p,directory,&f->d,error,sizeof(error)))return false;SolHirFileScope scopes[3];if(f->p.file_count>3)return false;for(size_t i=0;i<f->p.file_count;i++)scopes[i]=(SolHirFileScope){f->p.files[i].module_name,f->p.files[i].import_start,f->p.files[i].import_count,f->p.files[i].item_start,f->p.files[i].item_count};return sol_hir_lower_scoped(&f->p.source,&f->p.syntax,scopes,f->p.file_count,&f->h,&f->d)&&sol_type_check(&f->p.source,&f->p.syntax,&f->h,&f->t,&f->d)&&sol_effect_check(&f->p.source,&f->p.syntax,&f->h,&f->t,&f->e,&f->d)&&sol_contract_lower(&f->p.source,&f->p.syntax,&f->h,&f->t,&f->e,&f->c,&f->d)&&sol_ir_lower_scoped(&f->p.source,&f->p.syntax,&f->h,&f->t,&f->e,&f->c,f->p.files,f->p.file_count,&f->ir,&f->d);}
static bool setup(Fixture *f){
    test_distinct_roots_and_derived_sources();
    test_frozen_profile_rejection();
    test_frozen_profile_subset();
    test_fifth_lookalike_rejection();
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
int main(void){Fixture f;CHECK(setup(&f));SolMirConcreteProgram p;sol_mir_concrete_program_init(&p);SolMirProgramRoot root={find(&f.ir,"launch",SOL_IR_CALLABLE_FUNCTION),SOL_MIR_PROGRAM_ROOT_ENTRY};const char*names[]={"write","get","count","read"};SolIrCallableId imports[4];for(size_t i=0;i<4;i++)imports[i]=find(&f.ir,names[i],SOL_IR_CALLABLE_CAPABILITY);SolMirTargetDescriptor target=sol_mir_target_wasm32();SolMirConcreteBuildRequest request={&f.ir,&root,1,imports,4,&target,NULL};CHECK(sol_mir_concrete_program_build(&request,&p,&f.d)==SOL_MIR_CONCRETE_BUILD_SUCCEEDED);SolMirRuntimeConventions c;SolMirRuntimeValues v;SolMirRuntimeCleanup cl;SolMirRuntimeHostAbi a;sol_mir_runtime_conventions_init(&c);sol_mir_runtime_values_init(&v);sol_mir_runtime_cleanup_init(&cl);sol_mir_runtime_host_abi_init(&a);CHECK(sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&p,NULL},&c,&f.d)==SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED);CHECK(sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&c,NULL},&v,&f.d)==SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED);CHECK(sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&c,&v,NULL},&cl,&f.d)==SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED);size_t forbidden=0,unreachable=0;for(size_t i=0;i<v.host_result_plan_count;i++){forbidden+=v.host_result_plans[i].classification==SOL_MIR_RUNTIME_HOST_RESULT_FORBIDDEN;unreachable+=v.host_result_plans[i].classification==SOL_MIR_RUNTIME_HOST_RESULT_UNREACHABLE;}CHECK(forbidden!=0&&unreachable!=0);size_t host_result=c.concrete->linkage.host_requirements[0].result;SolMirRuntimeHostResultClass saved_class=v.host_result_plans[host_result].classification;SolMirRuntimeHostAbi rejected_owner;sol_mir_runtime_host_abi_init(&rejected_owner);v.host_result_plans[host_result].classification=SOL_MIR_RUNTIME_HOST_RESULT_FORBIDDEN;CHECK(sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&c,&v,&cl,NULL},&rejected_owner,&f.d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR);v.host_result_plans[host_result].classification=SOL_MIR_RUNTIME_HOST_RESULT_UNREACHABLE;CHECK(sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&c,&v,&cl,NULL},&rejected_owner,&f.d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR);v.host_result_plans[host_result].classification=saved_class;sol_mir_runtime_host_abi_free(&rejected_owner);CHECK(sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&c,&v,&cl,NULL},&a,&f.d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED);CHECK(sol_mir_runtime_host_abi_validate(&a,NULL));CHECK(a.operation_count==4&&a.requirement_count==5&&a.entry_root_count==3);
    CHECK(a.usage.capabilities==3&&a.usage.entry_roots==3&&a.usage.operations==4
        &&a.usage.arguments==3&&a.usage.formals==3&&a.usage.shapes==3
        &&a.usage.shape_cases==0&&a.usage.requirements==5&&a.usage.grants==4
        &&a.usage.owned_bytes==1816&&a.usage.build_scratch_bytes==185
           &&a.usage.build_work==27963&&a.usage.validation_scratch_bytes==77
             &&a.usage.validation_work==16074);SolMirRuntimeHostAbiWorkCensus meter=sol_mir_runtime_host_abi_test_work_census();CHECK(meter.dry_work==meter.actual_work&&meter.census_work+meter.dry_work+meter.actual_work==a.usage.build_work&&meter.validation_audit_work+meter.validation_replay_work==meter.validation_work&&meter.validation_work==a.usage.validation_work);
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
