#include "sol/mir_runtime_host_abi.h"
#include "mir_runtime_host_abi_internal.h"
#include "mir_runtime_arena_internal.h"

#include <stdlib.h>
#include <string.h>

#ifdef SOL_MIR_PLAN_TEST_HOOKS
static _Thread_local bool fail_validation_scratch;
void sol_mir_runtime_host_abi_test_force_validation_scratch_failure(bool value) { fail_validation_scratch=value; }
#endif

enum { HOST_FAILURE_MASK = UINT32_C(0x00008240) };
static bool bad(SolDiagnostics*d,const char*m){if(d)sol_diagnostics_add(d,"SOL-MIR-RUNTIME-HOST-ABI-002",SOL_SEVERITY_ERROR,(SolSpan){0},m);return false;}
static _Thread_local bool measuring_validation_work;
static _Thread_local size_t observed_validation_work;
static _Thread_local SolMirRuntimeHostAbiWorkMeter *alias_meter;
static bool mul(size_t a,size_t b,size_t*out){if(a&&b>SIZE_MAX/a)return false;*out=a*b;return true;}
static bool add(size_t*a,size_t b){if(b>SIZE_MAX-*a)return false;*a+=b;return true;}
static bool complete(SolMirRuntimeHostAbiLimits l){return l.max_capabilities&&l.max_entry_roots&&l.max_operations&&l.max_arguments&&l.max_formals&&l.max_shapes&&l.max_shape_cases&&l.max_requirements&&l.max_grants&&l.max_owned_bytes&&l.max_build_scratch_bytes&&l.max_build_work&&l.max_validation_scratch_bytes&&l.max_validation_work;}
static bool valid_range(const void*p,size_t n,size_t z){size_t bytes;return !n?p==NULL:p&&mul(n,z,&bytes)&&(uintptr_t)p<=UINTPTR_MAX-bytes;}
static bool overlap(const void*a,size_t an,size_t az,const void*b,size_t bn,size_t bz){size_t ab,bb;bool left,right;if(!an||!bn)return false;if(!mul(an,az,&ab)||!mul(bn,bz,&bb))return true;uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;if(x>UINTPTR_MAX-ab||y>UINTPTR_MAX-bb)return true;if(alias_meter&&(!sol_mir_runtime_host_abi_work_tick(alias_meter)||!sol_mir_runtime_host_abi_work_tick(alias_meter)))return true;left=x<y+bb;right=y<x+ab;return left&&right;}
static bool aliases_host(const SolMirRuntimeHostAbi*o,const void*p,size_t n,size_t z){if(alias_meter&&!sol_mir_runtime_host_abi_work_tick(alias_meter))return true;return overlap(o->capabilities,o->capability_count,sizeof(*o->capabilities),p,n,z)||overlap(o->entry_roots,o->entry_root_count,sizeof(*o->entry_roots),p,n,z)||overlap(o->operations,o->operation_count,sizeof(*o->operations),p,n,z)||overlap(o->arguments,o->argument_count,sizeof(*o->arguments),p,n,z)||overlap(o->formals,o->formal_count,sizeof(*o->formals),p,n,z)||overlap(o->shapes,o->shape_count,sizeof(*o->shapes),p,n,z)||overlap(o->shape_cases,o->shape_case_count,sizeof(*o->shape_cases),p,n,z)||overlap(o->requirements,o->requirement_count,sizeof(*o->requirements),p,n,z);}
typedef struct { const SolMirRuntimeHostAbi *owner; SolMirRuntimeHostAbiWorkMeter *meter; } HostAliasContext;
static SolMirRuntimeTextGuardResult host_text_guard(uintptr_t address,uintptr_t *boundary,void *opaque) { HostAliasContext*x=opaque;const void*p[]={x->owner->capabilities,x->owner->entry_roots,x->owner->operations,x->owner->arguments,x->owner->formals,x->owner->shapes,x->owner->shape_cases,x->owner->requirements};size_t n[]={x->owner->capability_count,x->owner->entry_root_count,x->owner->operation_count,x->owner->argument_count,x->owner->formal_count,x->owner->shape_count,x->owner->shape_case_count,x->owner->requirement_count},z[]={sizeof(*x->owner->capabilities),sizeof(*x->owner->entry_roots),sizeof(*x->owner->operations),sizeof(*x->owner->arguments),sizeof(*x->owner->formals),sizeof(*x->owner->shapes),sizeof(*x->owner->shape_cases),sizeof(*x->owner->requirements)};uintptr_t next=UINTPTR_MAX;bool hit=false;for(size_t i=0;i<8;i++){size_t bytes;if(!sol_mir_runtime_host_abi_work_tick(x->meter)||!mul(n[i],z[i],&bytes))return SOL_MIR_RUNTIME_TEXT_EXHAUSTED;if(!n[i])continue;uintptr_t start=(uintptr_t)p[i],end=start+bytes;hit|=address>=start&&address<end;if(start>address&&start<next)next=start;}if(hit)return SOL_MIR_RUNTIME_TEXT_OVERLAP;*boundary=next;return SOL_MIR_RUNTIME_TEXT_SAFE;}
static bool visit_host_alias(const void *pointer,size_t count,size_t size,void *context) {
    HostAliasContext *x=context;
    if(pointer==NULL&&count==0&&size==0)return sol_mir_runtime_host_abi_work_tick(x->meter);
    return sol_mir_runtime_host_abi_work_tick(x->meter)
        && !aliases_host(x->owner,pointer,count,size);
}
/* The shared raw census includes nested template/image MIR, source storage,
 * and symbolic text before local host records are traversed. */
static bool aliases_transitive(const SolMirRuntimeHostAbi*o,
    SolMirRuntimeHostAbiWorkMeter *meter) {
    HostAliasContext context={o,meter};
    SolMirRuntimeTextGuard saved_guard=sol_mir_runtime_text_guard;void *saved_context=sol_mir_runtime_text_guard_context;
    sol_mir_runtime_text_guard=host_text_guard;sol_mir_runtime_text_guard_context=&context;
    SolMirRuntimeArenaVisit result=sol_mir_runtime_visit_concrete_arenas(o->conventions->concrete,visit_host_alias,&context);
    sol_mir_runtime_text_guard=saved_guard;sol_mir_runtime_text_guard_context=saved_context;
    return result != SOL_MIR_RUNTIME_ARENA_VISIT_OK;
}
static bool kind(const SolMirRecipe*r,SolMirRuntimeHostArgumentKind*k){switch(r->kind){case SOL_MIR_RECIPE_INT64:*k=SOL_MIR_RUNTIME_HOST_ARGUMENT_INT64;return true;case SOL_MIR_RECIPE_BOOL:*k=SOL_MIR_RUNTIME_HOST_ARGUMENT_BOOL;return true;case SOL_MIR_RECIPE_TEXT:*k=SOL_MIR_RUNTIME_HOST_ARGUMENT_TEXT;return true;case SOL_MIR_RECIPE_UNIT:*k=SOL_MIR_RUNTIME_HOST_ARGUMENT_UNIT;return true;case SOL_MIR_RECIPE_OPTION:*k=SOL_MIR_RUNTIME_HOST_ARGUMENT_OPTION;return true;case SOL_MIR_RECIPE_RESULT:*k=SOL_MIR_RUNTIME_HOST_ARGUMENT_RESULT;return true;default:return false;}}
static bool text_is(const char *text,const char *want){return text&&want&&!strcmp(text,want);}
static bool effect_is(const SolMirMaterialization*m,const SolMirMaterializedEffectAtom*a,const char*want){size_t n=strlen(want);return a->name.count==n&&a->name.offset<=m->effect_name_count&&n<=m->effect_name_count-a->name.offset&&!memcmp(m->effect_names+a->name.offset,want,n);}
static bool recipe_kind_is(const SolMirRepresentation*r,SolMirRecipeId id,SolMirRecipeKind kind){return id<r->recipe_count&&r->recipes[id].kind==kind;}
/* This deliberately reconstructs authority from the exact selected P2/P3.1
 * import and semantic requirement.  It must not inherit the builder's
 * spelling-based profile classification. */
static bool e3_host_requirement(const SolMirRuntimeHostAbi *o, size_t host_id,
    SolMirRuntimeHostAbiWorkMeter *meter) {
    const SolMirLinkage *l=&o->conventions->concrete->linkage;
    const SolMirMaterialization *m=&o->conventions->concrete->materialization;
    const SolIr *ir=o->conventions->concrete->program.ir;
    const SolMirRepresentation *r=&o->conventions->concrete->representation;
    if(host_id>=l->host_requirement_count || !ir) return false;
    const SolMirLinkageHostRequirement *h=&l->host_requirements[host_id];
    if(h->import>=m->import_count || h->effects>=m->effect_row_count
        || h->receiver>=r->recipe_count) return false;
    const SolMirMaterializedImport *im=&m->imports[h->import];
    if(im->source_callable>=ir->callable_count) return false;
    const SolIrCallable *callable=&ir->callables[im->source_callable];
    if(callable->owner>=ir->definition_count
        || callable->kind!=SOL_IR_CALLABLE_CAPABILITY || callable->body!=SOL_IR_NONE
        || callable->generic_parameters.count || callable->effect_parameters.count
        || callable->result_authority_kind!=SOL_IR_AUTHORITY_NONE
        || h->semantic_id.high!=ir->definitions[callable->owner].semantic_id.high
        || h->semantic_id.low!=ir->definitions[callable->owner].semantic_id.low
        || h->receiver_access!=SOL_ACCESS_SHARED
        || r->recipes[h->receiver].kind!=SOL_MIR_RECIPE_CAPABILITY
        || r->recipes[h->receiver].concrete_definition!=callable->owner) return false;
    const SolMirMaterializedEffectRow *row=&m->effect_rows[h->effects];
    if(row->atoms.count!=1 || row->atoms.offset>=m->effect_row_atom_count) return false;
    size_t atom_id=m->effect_row_atoms[row->atoms.offset];
    if(atom_id>=m->effect_atom_count) return false;
    if(m->effect_atoms[atom_id].authority!=SOL_MIR_PLAN_EFFECT_AUTHORITY_RECEIVER
        || m->effect_atoms[atom_id].name.offset>m->effect_name_count
        || m->effect_atoms[atom_id].name.count>m->effect_name_count
            -m->effect_atoms[atom_id].name.offset) return false;
    if(h->parameters.offset>m->type_id_count
        || h->parameters.count>m->type_id_count-h->parameters.offset
        || h->parameter_accesses.offset>m->access_count
        || h->parameter_accesses.count!=h->parameters.count
        || h->parameter_accesses.count>m->access_count-h->parameter_accesses.offset)
        return false;
    const char *capability=ir->definitions[callable->owner].name,*member=callable->name,*effect=NULL;
    SolMirRecipeKind parameter=SOL_MIR_RECIPE_INT64,result=SOL_MIR_RECIPE_INT64;size_t parameter_count=0;
    if(text_is(capability,"Console")&&text_is(member,"write")){effect="console.write";parameter=SOL_MIR_RECIPE_TEXT;result=SOL_MIR_RECIPE_UNIT;parameter_count=1;}
    else if(text_is(capability,"Arguments")&&text_is(member,"count")){effect="process.arguments.count";result=SOL_MIR_RECIPE_INT64;}
    else if(text_is(capability,"Arguments")&&text_is(member,"get")){effect="process.arguments.get";parameter=SOL_MIR_RECIPE_INT64;result=SOL_MIR_RECIPE_OPTION;parameter_count=1;}
    else if(text_is(capability,"Configuration")&&text_is(member,"read")){effect="configuration.read";parameter=SOL_MIR_RECIPE_TEXT;result=SOL_MIR_RECIPE_OPTION;parameter_count=1;}
    else return false;
    if(!effect_is(m,&m->effect_atoms[atom_id],effect)||h->parameters.count!=parameter_count||!recipe_kind_is(r,h->result,result))return false;
    if(result==SOL_MIR_RECIPE_OPTION){const SolMirRecipe*option=&r->recipes[h->result];bool text_payload=false;for(size_t i=0;i<option->variants.count;i++){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;const SolMirRecipeVariant*v=&r->variants[option->variants.offset+i];if(v->fields.count==1&&recipe_kind_is(r,r->fields[v->fields.offset].type,SOL_MIR_RECIPE_TEXT))text_payload=true;else if(v->fields.count>1)return false;}if(!text_payload)return false;}
    for(size_t i=0;i<h->parameters.count;i++){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;SolMirRecipeId recipe=m->type_ids[h->parameters.offset+i];if(!recipe_kind_is(r,recipe,parameter)||m->accesses[h->parameter_accesses.offset+i]!=SOL_ACCESS_OWNED)return false;}
    return true;
}
static int frozen_profile_kind(const SolMirRuntimeHostAbi *o,size_t host){
    const SolMirLinkage*l=&o->conventions->concrete->linkage;
    const SolMirMaterialization*m=&o->conventions->concrete->materialization;
    const SolIr*ir=o->conventions->concrete->program.ir;
    if(!ir||host>=l->host_requirement_count)return -1;
    const SolMirLinkageHostRequirement*h=&l->host_requirements[host];
    if(h->import>=m->import_count||m->imports[h->import].source_callable>=ir->callable_count)return -1;
    const SolIrCallable*c=&ir->callables[m->imports[h->import].source_callable];
    if(c->owner>=ir->definition_count)return -1;
    const char*owner=ir->definitions[c->owner].name;
    if(text_is(owner,"Console")&&text_is(c->name,"write"))return 0;
    if(text_is(owner,"Arguments")&&text_is(c->name,"count"))return 1;
    if(text_is(owner,"Arguments")&&text_is(c->name,"get"))return 2;
    if(text_is(owner,"Configuration")&&text_is(c->name,"read"))return 3;
    return -1;
}
static bool frozen_e6_surface(const SolMirRuntimeHostAbi*o,
    SolMirRuntimeHostAbiWorkMeter *meter){
    const SolMirLinkage*l=&o->conventions->concrete->linkage;
    const SolMirRuntimeConventions*c=o->conventions;
    unsigned seen=0;size_t host_imports=0;
    if(l->host_requirement_count>4)return false;
    for(size_t i=0;i<c->import_count;i++){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;if(c->imports[i].kind==SOL_MIR_RUNTIME_IMPORT_HOST)++host_imports;}
    if(host_imports!=l->host_requirement_count)return false;
    for(size_t host=0;host<l->host_requirement_count;host++){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;
        size_t imports=0,signatures=0;int kind;
        if(!e3_host_requirement(o,host,meter)||(kind=frozen_profile_kind(o,host))<0||(seen&(1u<<kind)))return false;
        seen|=1u<<kind;
        for(size_t i=0;i<c->import_count;i++){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;if(c->imports[i].kind==SOL_MIR_RUNTIME_IMPORT_HOST&&c->imports[i].host==host)++imports;}
        for(size_t i=0;i<c->signature_count;i++){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;if(c->signatures[i].origin==SOL_MIR_RUNTIME_SIGNATURE_HOST&&c->signatures[i].host==host)++signatures;}
        if(imports!=1||signatures!=1)return false;
        for(size_t prior=0;prior<host;prior++){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;
            const SolMirRuntimeImport *left=NULL,*right=NULL;
            for(size_t i=0;i<c->import_count;i++){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;
                if(c->imports[i].kind!=SOL_MIR_RUNTIME_IMPORT_HOST)continue;
                if(c->imports[i].host==prior)left=&c->imports[i];
                if(c->imports[i].host==host)right=&c->imports[i];
            }
            if(!left||!right||!memcmp(&left->identity,&right->identity,sizeof(left->identity))||!memcmp(&left->symbol,&right->symbol,sizeof(left->symbol)))return false;
        }
    }
    return true;
}
static bool validate_shape(const SolMirRuntimeHostAbi*o,size_t id,unsigned char*active,
    unsigned char*seen,SolMirRuntimeHostAbiWorkMeter *meter){
    const SolMirRepresentation*r=&o->conventions->concrete->representation;
    if(!sol_mir_runtime_host_abi_work_tick(meter)||id>=o->shape_count||seen[id])return false;
    const SolMirRuntimeHostShape*s=&o->shapes[id];SolMirRuntimeHostArgumentKind want;
    if(s->recipe>=r->recipe_count||active[s->recipe]||!r->recipes[s->recipe].inhabited
        ||!kind(&r->recipes[s->recipe],&want)||s->kind!=want
        ||s->cases.offset>o->shape_case_count
        ||s->cases.count>o->shape_case_count-s->cases.offset)return false;
    seen[id]=1;
    if(want!=SOL_MIR_RUNTIME_HOST_ARGUMENT_OPTION&&want!=SOL_MIR_RUNTIME_HOST_ARGUMENT_RESULT)
        return s->cases.count==0;
    active[s->recipe]=1;
    if(s->cases.count!=r->recipes[s->recipe].variants.count){active[s->recipe]=0;return false;}
    for(size_t i=0;i<s->cases.count;i++){
        if(!sol_mir_runtime_host_abi_work_tick(meter)){active[s->recipe]=0;return false;}
        const SolMirRuntimeHostShapeCase*c=&o->shape_cases[s->cases.offset+i];
        const SolMirRecipeVariant*v=&r->variants[r->recipes[s->recipe].variants.offset+i];
        if(c->parent!=id||c->ordinal!=v->ordinal||v->fields.count>1
            ||(v->fields.count==0&&c->child!=SOL_MIR_RUNTIME_NONE)
            ||(v->fields.count==1&&(c->child==SOL_MIR_RUNTIME_NONE
                 ||c->child>=o->shape_count||o->shapes[c->child].recipe!=r->fields[v->fields.offset].type
                 ||o->shapes[c->child].access!=s->access
                 ||!validate_shape(o,c->child,active,seen,meter)))){active[s->recipe]=0;return false;}
    }
    active[s->recipe]=0;return true;
}
static bool cleanup_mapping(const SolMirRuntimeHostAbi*o,const SolMirRuntimeHostRequirement*q){if(q->event>=o->cleanup->event_count||q->failure_transition>=o->cleanup->transition_count)return false;const SolMirRuntimeCleanupEvent*e=&o->cleanup->events[q->event];const SolMirRuntimeCleanupTransition*t=&o->cleanup->transitions[q->failure_transition];const SolMirRuntimeCall*c=&o->conventions->calls[q->call];return t->event==q->event&&t->edge_role==SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE&&t->failure_mask==HOST_FAILURE_MASK&&t->failure_source==SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31&&t->failure_site==c->failure_site&&e->block==c->block&&e->producer==(c->owner_kind==SOL_MIR_RUNTIME_CALL_OWNER_IMAGE?SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_INVOKE:SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_INVOKE);}
static size_t call_host(const SolMirRuntimeHostAbi*o,const SolMirRuntimeCall*c){const SolMirLinkage*l=&o->conventions->concrete->linkage;if(c->target_kind==SOL_MIR_RUNTIME_TARGET_DIRECT_HOST)return c->host;if(c->target_kind==SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE&&c->table<l->table_entry_count&&l->table_entries[c->table].target_kind==SOL_MIR_LINKAGE_TARGET_HOST)return l->table_entries[c->table].host;return SOL_MIR_RUNTIME_NONE;}
static size_t image_for_instance(const SolMirMaterialization*m,size_t instance,
    SolMirRuntimeHostAbiWorkMeter *meter){for(size_t i=0;i<m->image_count;i++){
        if(!sol_mir_runtime_host_abi_work_tick(meter))return SOL_MIR_RUNTIME_NONE;
        if(m->images[i].instance==instance)return i;}return SOL_MIR_RUNTIME_NONE;}
static bool coordinate(const SolMirMaterialization*m,SolMirMaterializedTemporaryId temporary,
    SolMirMaterializedValueId*value,SolMirMaterializedPlaceId*place,
    SolMirRuntimeHostAbiWorkMeter *meter){size_t found=SOL_MIR_RUNTIME_NONE;
    if(temporary==SOL_MIR_RUNTIME_NONE)return false;for(size_t i=0;i<m->instruction_count;i++){
        if(!sol_mir_runtime_host_abi_work_tick(meter))return false;const SolMirMaterializedInstruction*x=&m->instructions[i];if(x->kind!=SOL_MIR_INST_TEMPORARY_INIT||x->temporary!=temporary)continue;if(found!=SOL_MIR_RUNTIME_NONE)return false;found=x->left;}if(found>=m->value_count)return false;*value=found;*place=SOL_MIR_RUNTIME_NONE;const SolMirMaterializedValue*v=&m->values[found];if(v->instruction<m->instruction_count){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;const SolMirMaterializedInstruction*x=&m->instructions[v->instruction];if(x->kind==SOL_MIR_INST_LOAD_COPY||x->kind==SOL_MIR_INST_LOAD_MOVE||x->kind==SOL_MIR_INST_LOAD_UPDATE)*place=x->place;}return true;}
static bool result_temporary_for(const SolMirMaterialization*m,SolMirMaterializedValueId result,SolMirMaterializedTemporaryId*temporary,SolMirRuntimeHostAbiWorkMeter *meter){size_t found=SOL_MIR_RUNTIME_NONE;for(size_t i=0;i<m->instruction_count;i++){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;const SolMirMaterializedInstruction*x=&m->instructions[i];if(x->kind!=SOL_MIR_INST_TEMPORARY_INIT||x->left!=result)continue;if(found!=SOL_MIR_RUNTIME_NONE)return false;found=x->temporary;}if(found==SOL_MIR_RUNTIME_NONE)return false;*temporary=found;return true;}
static size_t root_for_local(const SolMirRuntimeHostAbi*o,SolMirMaterializedLocalId local,SolMirRuntimeHostAbiWorkMeter *meter){const SolMirMaterialization*m=&o->conventions->concrete->materialization;const SolMirLinkage*l=&o->conventions->concrete->linkage;if(local>=m->local_count)return SOL_MIR_RUNTIME_NONE;for(size_t root=0;root<o->entry_root_count;root++){if(!sol_mir_runtime_host_abi_work_tick(meter))return SOL_MIR_RUNTIME_NONE;const SolMirRuntimeHostEntryRoot*r=&o->entry_roots[root];if(r->entry>=o->conventions->entry_count)continue;const SolMirRuntimeEntry*entry=&o->conventions->entries[r->entry];if(entry->callable>=l->callable_count)continue;size_t image=image_for_instance(m,l->callables[entry->callable].instance,meter);if(image==SOL_MIR_RUNTIME_NONE)continue;const SolMirMaterializedImage*x=&m->images[image];for(size_t i=0;i<x->locals.count;i++){if(!sol_mir_runtime_host_abi_work_tick(meter))return SOL_MIR_RUNTIME_NONE;size_t candidate=x->locals.offset+i;if(candidate==local&&m->locals[candidate].kind==SOL_MIR_MATERIALIZED_LOCAL_PARAMETER&&m->locals[candidate].ordinal==r->formal)return root;}}return SOL_MIR_RUNTIME_NONE;}
static size_t call_image(const SolMirRuntimeHostAbi*o,const SolMirRuntimeCall*c,SolMirRuntimeHostAbiWorkMeter *meter){const SolMirLinkage*l=&o->conventions->concrete->linkage;size_t internal=SOL_MIR_RUNTIME_NONE;if(c->target_kind==SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL)internal=c->internal;else if(c->target_kind==SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE&&c->table<l->table_entry_count&&l->table_entries[c->table].target_kind==SOL_MIR_LINKAGE_TARGET_INTERNAL)internal=l->table_entries[c->table].internal;return internal<l->callable_count?image_for_instance(&o->conventions->concrete->materialization,l->callables[internal].instance,meter):SOL_MIR_RUNTIME_NONE;}
static bool owner_reachable(const SolMirRuntimeHostAbi*o,const unsigned char*images,const SolMirRuntimeCall*c){if(c->owner_kind==SOL_MIR_RUNTIME_CALL_OWNER_IMAGE)return c->image<o->conventions->concrete->materialization.image_count&&images[c->image];if(c->predicate>=o->conventions->concrete->operations.predicate_body_count)return false;const SolMirPredicateBody*b=&o->conventions->concrete->operations.predicate_bodies[c->predicate];return b->owner_kind==SOL_MIR_PREDICATE_OWNER_INSTANCE&&b->instance<o->conventions->concrete->materialization.image_count&&images[b->instance];}
static bool mark_images(const SolMirRuntimeHostAbi*o,size_t entry,unsigned char*images,
    SolMirRuntimeHostAbiWorkMeter *meter){const SolMirLinkage*l=&o->conventions->concrete->linkage;const SolMirMaterialization*m=&o->conventions->concrete->materialization;if(entry>=o->conventions->entry_count||o->conventions->entries[entry].callable>=l->callable_count)return false;size_t first=image_for_instance(m,l->callables[o->conventions->entries[entry].callable].instance,meter);if(first==SOL_MIR_RUNTIME_NONE)return false;images[first]=1;bool changed=true;while(changed){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;changed=false;for(size_t i=0;i<o->conventions->call_count;i++){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;const SolMirRuntimeCall*c=&o->conventions->calls[i];size_t target;if(!owner_reachable(o,images,c)||(target=call_image(o,c,meter))==SOL_MIR_RUNTIME_NONE||images[target])continue;images[target]=1;changed=true;}}return true;}
/* Independent provenance fixed point.  A host receiver may be a helper
 * formal, but it must have one and only one entry-root origin and must not be
 * a constructed/private/projection value. */
static bool exact_root_place(const SolMirMaterialization*m,const unsigned char*roots,
    size_t root,SolMirMaterializedPlaceId place,SolMirRuntimeHostAbiWorkMeter *meter){
    if(!sol_mir_runtime_host_abi_work_tick(meter))return false;return place<m->place_count
        &&!m->places[place].projections.count&&m->places[place].local<m->local_count
        &&roots[root*m->local_count+m->places[place].local];}
static bool operand_has_root(const SolMirRuntimeHostAbi*o,const unsigned char*roots,
    size_t root,const SolMirRuntimeOperand*x,SolMirRuntimeHostAbiWorkMeter *meter){const SolMirMaterialization*m=&o->conventions->concrete->materialization;
    if(!x)return false;if(x->value.kind==SOL_MIR_RUNTIME_VALUE_MATERIALIZED_PLACE)
        return exact_root_place(m,roots,root,x->value.id,meter);
    if(x->value.kind!=SOL_MIR_RUNTIME_VALUE_MATERIALIZED_TEMPORARY)return false;
    SolMirMaterializedValueId value;SolMirMaterializedPlaceId place;
    return coordinate(m,x->value.id,&value,&place,meter)&&exact_root_place(m,roots,root,place,meter);}
static bool propagate_roots(const SolMirRuntimeHostAbi*o,size_t entry,const unsigned char*images,
    unsigned char*roots,SolMirRuntimeHostAbiWorkMeter *meter){const SolMirMaterialization*m=&o->conventions->concrete->materialization;
    const SolMirLinkage*l=&o->conventions->concrete->linkage;
    if(entry>=o->conventions->entry_count||o->conventions->entries[entry].callable>=l->callable_count)return false;
    size_t image=image_for_instance(m,l->callables[o->conventions->entries[entry].callable].instance,meter);
    if(image==SOL_MIR_RUNTIME_NONE)return false;
    for(size_t root=0;root<o->entry_root_count;root++){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;if(o->entry_roots[root].entry==entry)
        for(size_t i=0;i<m->images[image].locals.count;i++){size_t local=m->images[image].locals.offset+i;
            if(!sol_mir_runtime_host_abi_work_tick(meter))return false;
            if(m->locals[local].kind==SOL_MIR_MATERIALIZED_LOCAL_PARAMETER&&m->locals[local].ordinal==o->entry_roots[root].formal)
                roots[root*m->local_count+local]=1;}}
    bool changed=true;while(changed){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;changed=false;for(size_t ci=0;ci<o->conventions->call_count;ci++){
        if(!sol_mir_runtime_host_abi_work_tick(meter))return false;const SolMirRuntimeCall*c=&o->conventions->calls[ci];if(!owner_reachable(o,images,c))continue;
        size_t target=call_image(o,c,meter);if(target==SOL_MIR_RUNTIME_NONE)continue;
        const SolMirMaterializedImage*target_image_=&m->images[target];
        for(size_t oi=0;oi<c->operands.count;oi++){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;const SolMirRuntimeOperand*x=&o->conventions->operands[c->operands.offset+oi];
            if(x->signature_slot>=o->conventions->signature_slot_count)continue;
            const SolMirRuntimeSignatureSlot*slot=&o->conventions->signature_slots[x->signature_slot];
            if(slot->role!=SOL_MIR_RUNTIME_SLOT_PARAMETER)continue;
            for(size_t li=0;li<target_image_->locals.count;li++){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;size_t local=target_image_->locals.offset+li;
                if(m->locals[local].kind!=SOL_MIR_MATERIALIZED_LOCAL_PARAMETER||m->locals[local].ordinal!=slot->formal)continue;
                for(size_t root=0;root<o->entry_root_count;root++){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;if(o->entry_roots[root].entry==entry
                    &&operand_has_root(o,roots,root,x,meter)&&!roots[root*m->local_count+local]){
                    roots[root*m->local_count+local]=1;changed=true;}}}}}}
    return true;}
static size_t receiver_origin(const SolMirRuntimeHostAbi*o,size_t entry,const unsigned char*roots,
    const SolMirRuntimeOperand*x,SolMirRuntimeHostAbiWorkMeter *meter){size_t found=SOL_MIR_RUNTIME_NONE;
    for(size_t root=0;root<o->entry_root_count;root++) {
        if(!sol_mir_runtime_host_abi_work_tick(meter))return SOL_MIR_RUNTIME_NONE;
        if(o->entry_roots[root].entry==entry&&operand_has_root(o,roots,root,x,meter)) {
            if(found!=SOL_MIR_RUNTIME_NONE)return SOL_MIR_RUNTIME_NONE;
            found=root;
        }
    }
    return found;
}
static bool requirements_complete(const SolMirRuntimeHostAbi*o,
    SolMirRuntimeHostAbiWorkMeter *meter){const SolMirMaterialization*m=&o->conventions->concrete->materialization;
    size_t bytes;if(!mul(o->entry_root_count,m->local_count,&bytes))return false;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if(!measuring_validation_work)
        sol_mir_runtime_host_abi_internal_allocation_attempts += 2;
#endif
    unsigned char*images=calloc(m->image_count?m->image_count:1,1);unsigned char*roots=calloc(bytes?bytes:1,1);
    if(!images||!roots){free(images);free(roots);return false;}size_t expected=0;
    for(size_t entry=0;entry<o->conventions->entry_count;entry++){memset(images,0,m->image_count?m->image_count:1);memset(roots,0,bytes?bytes:1);
        if(!sol_mir_runtime_host_abi_work_tick(meter)
            ||!mark_images(o,entry,images,meter)||!propagate_roots(o,entry,images,roots,meter)){free(images);free(roots);return false;}
        for(size_t ci=0;ci<o->conventions->call_count;ci++){const SolMirRuntimeCall*c=&o->conventions->calls[ci];size_t host=call_host(o,c);
            if(!sol_mir_runtime_host_abi_work_tick(meter)){free(images);free(roots);return false;}
            if(!owner_reachable(o,images,c)||host==SOL_MIR_RUNTIME_NONE)continue;
            if(!c->operands.count){free(images);free(roots);return false;}
            size_t root=receiver_origin(o,entry,roots,&o->conventions->operands[c->operands.offset],meter);
            if(root==SOL_MIR_RUNTIME_NONE){free(images);free(roots);return false;}bool found=false;
            for(size_t q=0;q<o->requirement_count;q++){if(!sol_mir_runtime_host_abi_work_tick(meter)){free(images);free(roots);return false;}found|=o->requirements[q].entry==entry&&o->requirements[q].root==root
                &&o->requirements[q].operation<o->operation_count&&o->operations[o->requirements[q].operation].host==host&&o->requirements[q].call==ci;
            }
            if(!found||!sol_mir_runtime_host_abi_work_tick(meter)){free(images);free(roots);return false;}++expected;}}
    free(images);free(roots);return expected==o->requirement_count;}

/* Keep validation accounting coupled to the records it actually authenticates,
 * rather than to a closed-form arena census.  In particular the nested grant
 * identity comparisons are charged as they are performed. */
static bool charge_usage_work(const SolMirRuntimeHostAbi *o,
    SolMirRuntimeHostAbiWorkMeter *meter) {
#define RECORDS(member) for(size_t i=0;i<o->member##_count;i++) if(!sol_mir_runtime_host_abi_work_tick(meter)) return false
    RECORDS(capability); RECORDS(entry_root); RECORDS(operation); RECORDS(argument);
    RECORDS(formal); RECORDS(shape); RECORDS(shape_case);
#undef RECORDS
    for(size_t i=0;i<o->requirement_count;i++) {
        bool seen=false;
        if(!sol_mir_runtime_host_abi_work_tick(meter)) return false;
        for(size_t q=0;q<i;q++) {
            if(!sol_mir_runtime_host_abi_work_tick(meter)) return false;
            seen|=o->requirements[q].entry==o->requirements[i].entry
                &&o->requirements[q].root==o->requirements[i].root
                &&o->requirements[q].operation==o->requirements[i].operation;
        }
        if(!seen&&!sol_mir_runtime_host_abi_work_tick(meter)) return false;
    }
    return true;
}

SolMirRuntimeHostAbiBuildOutcome sol_mir_runtime_host_abi_internal_validate(const SolMirRuntimeHostAbi*o,SolDiagnostics*d){
    /* Do not authenticate a predecessor until every one of our own ranges has
     * been proved disjoint from it.  This ordering is observable under the
     * restore-safe alias tests: a forged owner must never cause a predecessor
     * validator to dereference one of our mutable arenas. */
    if(!o||!complete(o->limits)||o->capability_count!=o->capability_capacity||o->entry_root_count!=o->entry_root_capacity||o->operation_count!=o->operation_capacity||o->argument_count!=o->argument_capacity||o->formal_count!=o->formal_capacity||o->shape_count!=o->shape_capacity||o->shape_case_count!=o->shape_case_capacity||o->requirement_count!=o->requirement_capacity||!valid_range(o->capabilities,o->capability_count,sizeof(*o->capabilities))||!valid_range(o->entry_roots,o->entry_root_count,sizeof(*o->entry_roots))||!valid_range(o->operations,o->operation_count,sizeof(*o->operations))||!valid_range(o->arguments,o->argument_count,sizeof(*o->arguments))||!valid_range(o->formals,o->formal_count,sizeof(*o->formals))||!valid_range(o->shapes,o->shape_count,sizeof(*o->shapes))||!valid_range(o->shape_cases,o->shape_case_count,sizeof(*o->shape_cases))||!valid_range(o->requirements,o->requirement_count,sizeof(*o->requirements))){bad(d,"malformed runtime host ABI owner");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    SolMirRuntimeHostAbiWorkMeter audit_meter={o->limits.max_validation_work,0};
    alias_meter=&audit_meter;
    {const void*arena[]={o->capabilities,o->entry_roots,o->operations,o->arguments,o->formals,o->shapes,o->shape_cases,o->requirements};const size_t count[]={o->capability_count,o->entry_root_count,o->operation_count,o->argument_count,o->formal_count,o->shape_count,o->shape_case_count,o->requirement_count};const size_t size[]={sizeof(*o->capabilities),sizeof(*o->entry_roots),sizeof(*o->operations),sizeof(*o->arguments),sizeof(*o->formals),sizeof(*o->shapes),sizeof(*o->shape_cases),sizeof(*o->requirements)};for(size_t i=0;i<8;i++)for(size_t j=i+1;j<8;j++)if(overlap(arena[i],count[i],size[i],arena[j],count[j],size[j])){bad(d,"runtime host ABI arenas overlap");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}}
    if(aliases_host(o,o,1,sizeof(*o))||!o->conventions||!o->values||!o->cleanup||aliases_host(o,o->conventions,1,sizeof(*o->conventions))||aliases_host(o,o->values,1,sizeof(*o->values))||aliases_host(o,o->cleanup,1,sizeof(*o->cleanup))){bad(d,"runtime host ABI aliases owner or predecessor");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
#define EARLY_CONVENTIONS_ARENA(member,type,singular) if(aliases_host(o,o->conventions->member,o->conventions->singular##_count,sizeof(type))){bad(d,"runtime host ABI aliases convention arena");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    SOL_MIR_RUNTIME_CONVENTIONS_ARENAS(EARLY_CONVENTIONS_ARENA)
#undef EARLY_CONVENTIONS_ARENA
#define EARLY_VALUES_ARENA(member,type,singular) if(aliases_host(o,o->values->member,o->values->singular##_count,sizeof(type))){bad(d,"runtime host ABI aliases values arena");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    EARLY_VALUES_ARENA(recipe_operations,SolMirRuntimeRecipeOperations,recipe_operation) EARLY_VALUES_ARENA(allocation_plans,SolMirRuntimeAllocationPlan,allocation_plan) EARLY_VALUES_ARENA(copy_plans,SolMirRuntimeCopyPlan,copy_plan) EARLY_VALUES_ARENA(equality_plans,SolMirRuntimeEqualityPlan,equality_plan) EARLY_VALUES_ARENA(host_result_plans,SolMirRuntimeHostResultPlan,host_result_plan) EARLY_VALUES_ARENA(host_result_requirements,SolMirRuntimeHostResultRequirement,host_result_requirement) EARLY_VALUES_ARENA(ownership_plans,SolMirRuntimeOwnershipPlan,ownership_plan) EARLY_VALUES_ARENA(ownership_variants,SolMirRuntimeOwnershipVariant,ownership_variant) EARLY_VALUES_ARENA(owned_edges,SolMirRuntimeOwnedEdge,owned_edge)
#undef EARLY_VALUES_ARENA
#define EARLY_CLEANUP_ARENA(member,type,singular) if(aliases_host(o,o->cleanup->member,o->cleanup->singular##_count,sizeof(type))){bad(d,"runtime host ABI aliases cleanup arena");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    EARLY_CLEANUP_ARENA(events,SolMirRuntimeCleanupEvent,event) EARLY_CLEANUP_ARENA(actions,SolMirRuntimeCleanupAction,action) EARLY_CLEANUP_ARENA(transitions,SolMirRuntimeCleanupTransition,transition) EARLY_CLEANUP_ARENA(supplemental_sites,SolMirRuntimeCleanupSupplementalSite,supplemental_site) EARLY_CLEANUP_ARENA(drop_paths,SolMirRuntimeCleanupDropPath,drop_path)
#undef EARLY_CLEANUP_ARENA
    if(!o->conventions->concrete||aliases_host(o,o->conventions->concrete,1,sizeof(*o->conventions->concrete))||aliases_host(o,&o->conventions->concrete->program,1,sizeof(o->conventions->concrete->program))||aliases_host(o,&o->conventions->concrete->materialization,1,sizeof(o->conventions->concrete->materialization))||aliases_host(o,&o->conventions->concrete->representation,1,sizeof(o->conventions->concrete->representation))||aliases_host(o,&o->conventions->concrete->layout,1,sizeof(o->conventions->concrete->layout))||aliases_host(o,&o->conventions->concrete->operations,1,sizeof(o->conventions->concrete->operations))||aliases_host(o,&o->conventions->concrete->linkage,1,sizeof(o->conventions->concrete->linkage))){bad(d,"runtime host ABI aliases transitive predecessor owner");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    if(aliases_transitive(o,&audit_meter)){bad(d,"runtime host ABI aliases transitive predecessor arena");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    if(!o||!o->conventions||!o->values||!o->cleanup||o->values->conventions!=o->conventions||o->cleanup->conventions!=o->conventions||o->cleanup->values!=o->values||!sol_mir_runtime_conventions_validate(o->conventions,d)||!sol_mir_runtime_values_validate(o->values,d)||!sol_mir_runtime_cleanup_validate(o->cleanup,d)||!complete(o->limits)||o->capability_count!=o->capability_capacity||o->entry_root_count!=o->entry_root_capacity||o->operation_count!=o->operation_capacity||o->argument_count!=o->argument_capacity||o->formal_count!=o->formal_capacity||o->shape_count!=o->shape_capacity||o->shape_case_count!=o->shape_case_capacity||o->requirement_count!=o->requirement_capacity||!valid_range(o->capabilities,o->capability_count,sizeof(*o->capabilities))||!valid_range(o->entry_roots,o->entry_root_count,sizeof(*o->entry_roots))||!valid_range(o->operations,o->operation_count,sizeof(*o->operations))||!valid_range(o->arguments,o->argument_count,sizeof(*o->arguments))||!valid_range(o->formals,o->formal_count,sizeof(*o->formals))||!valid_range(o->shapes,o->shape_count,sizeof(*o->shapes))||!valid_range(o->shape_cases,o->shape_case_count,sizeof(*o->shape_cases))||!valid_range(o->requirements,o->requirement_count,sizeof(*o->requirements))){bad(d,"malformed runtime host ABI owner");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    const void*arena[]={o->capabilities,o->entry_roots,o->operations,o->arguments,o->formals,o->shapes,o->shape_cases,o->requirements};const size_t count[]={o->capability_count,o->entry_root_count,o->operation_count,o->argument_count,o->formal_count,o->shape_count,o->shape_case_count,o->requirement_count};const size_t size[]={sizeof(*o->capabilities),sizeof(*o->entry_roots),sizeof(*o->operations),sizeof(*o->arguments),sizeof(*o->formals),sizeof(*o->shapes),sizeof(*o->shape_cases),sizeof(*o->requirements)};for(size_t i=0;i<8;i++)for(size_t j=i+1;j<8;j++)if(overlap(arena[i],count[i],size[i],arena[j],count[j],size[j])){bad(d,"runtime host ABI arenas overlap");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    if(aliases_host(o,o->conventions,1,sizeof(*o->conventions))||aliases_host(o,o->values,1,sizeof(*o->values))||aliases_host(o,o->cleanup,1,sizeof(*o->cleanup))){bad(d,"runtime host ABI aliases predecessor");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
#define PREDECESSOR_ARENA(member,type,singular) if(aliases_host(o,o->conventions->member,o->conventions->singular##_count,sizeof(type))){bad(d,"runtime host ABI aliases convention arena");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    SOL_MIR_RUNTIME_CONVENTIONS_ARENAS(PREDECESSOR_ARENA)
#undef PREDECESSOR_ARENA
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if(fail_validation_scratch){bad(d,"runtime host ABI validation scratch allocation failed");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_ALLOCATION_FAILED;}
#endif
    alias_meter=NULL;
    const SolMirRepresentation*r=&o->conventions->concrete->representation;const SolMirLinkage*l=&o->conventions->concrete->linkage;
    if(!frozen_e6_surface(o,&audit_meter)){bad(d,"host surface is not the closed E6 identity table");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if(!measuring_validation_work)
        sol_mir_runtime_host_abi_internal_allocation_attempts += 2;
#endif
    unsigned char*active=calloc(r->recipe_count?r->recipe_count:1,1);unsigned char*seen=calloc(o->shape_count?o->shape_count:1,1);if(!active||!seen){free(active);free(seen);bad(d,"runtime host ABI validation scratch allocation failed");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_ALLOCATION_FAILED;}
    size_t roots=0;
    for(size_t e=0;e<o->conventions->entry_count;e++){
        if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){free(active);free(seen);return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
        const SolMirRuntimeEntry*entry=&o->conventions->entries[e];
        if(entry->signature>=o->conventions->signature_count){free(active);free(seen);bad(d,"entry signature");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
        const SolMirRuntimeSignature*s=&o->conventions->signatures[entry->signature];
        for(size_t q=0;q<s->slots.count;q++){
            if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){free(active);free(seen);return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
            const SolMirRuntimeSignatureSlot*slot=&o->conventions->signature_slots[s->slots.offset+q];
            if(slot->role==SOL_MIR_RUNTIME_SLOT_PARAMETER&&slot->recipe<r->recipe_count&&r->recipes[slot->recipe].kind==SOL_MIR_RECIPE_CAPABILITY){
                if(roots>=o->entry_root_count||memcmp(&o->entry_roots[roots],&(SolMirRuntimeHostEntryRoot){e,slot->formal,slot->recipe,entry->signature},sizeof(*o->entry_roots))){free(active);free(seen);bad(d,"entry root mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
                ++roots;
            }
        }
    }
    if(roots!=o->entry_root_count){free(active);free(seen);bad(d,"entry root census mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    /* Construction instances are authoritative: a capability plan may not be
     * replaced by another instance merely because its recipe is identical. */
    size_t capability=0;
    for(size_t i=0;i<o->entry_root_count;i++){
        if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){free(active);free(seen);return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
        const SolMirRuntimeHostEntryRoot*x=&o->entry_roots[i];
        SolMirRuntimeHostCapabilityPlan want={x->recipe,SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT,
            SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE,
            SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE,i,
            SOL_MIR_RUNTIME_NONE,SOL_MIR_RECIPE_NONE,SOL_MIR_RUNTIME_NONE};
        if(capability>=o->capability_count
            ||memcmp(&o->capabilities[capability++],&want,sizeof(want))){
            free(active);free(seen);bad(d,"entry capability source mismatch");
            return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;
        }
    }
    const SolMirMaterialization*cap_m=&o->conventions->concrete->materialization;
    const SolMirOperations*cap_ops=&o->conventions->concrete->operations;
    for(size_t i=0;i<cap_ops->constructor_count;i++){
        if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){free(active);free(seen);return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
        const SolMirOperationConstructPlan*p=&cap_ops->constructors[i];
        if(p->kind!=SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY)continue;
        if(p->capability_source_operand>=p->operands.count){free(active);free(seen);bad(d,"capability source selection");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
        const SolMirOperationConstructOperand*operand=&cap_ops->construct_operands[p->operands.offset+p->capability_source_operand];
        SolMirMaterializedTemporaryId temporary;
        SolMirMaterializedValueId source_value;
        SolMirMaterializedPlaceId source_place;
        if(!result_temporary_for(cap_m,p->result,&temporary,&audit_meter)) temporary=SOL_MIR_RUNTIME_NONE;
        if(!coordinate(cap_m,operand->temporary,&source_value,&source_place,&audit_meter)){
            free(active);free(seen);bad(d,"capability materialization coordinate");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;
        }
        SolMirRuntimeHostCapabilitySource source=p->capability_rule==SOL_MIR_OPERATION_CAPABILITY_PRIVATE_SOURCE
            ?SOL_MIR_RUNTIME_HOST_CAPABILITY_PRIVATE_SOURCE
            :p->capability_rule==SOL_MIR_OPERATION_CAPABILITY_BASE_SOURCE
                ?SOL_MIR_RUNTIME_HOST_CAPABILITY_DERIVED:SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT;
        size_t root=SOL_MIR_RUNTIME_NONE,parent=SOL_MIR_RUNTIME_NONE;
        if(source==SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT){
            root=root_for_local(o,p->inherited_root,&audit_meter);
            if(root==SOL_MIR_RUNTIME_NONE||source_place>=cap_m->place_count
                ||cap_m->places[source_place].local!=p->inherited_root
                ||cap_m->places[source_place].projections.count){free(active);free(seen);bad(d,"capability root coordinate");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
            parent=root;
        }else if(source==SOL_MIR_RUNTIME_HOST_CAPABILITY_DERIVED){
            for(size_t q=0;q<capability;q++){
                if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){free(active);free(seen);return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
                if(o->capabilities[q].temporary==operand->temporary){if(parent!=SOL_MIR_RUNTIME_NONE){free(active);free(seen);bad(d,"ambiguous capability parent");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}parent=q;}
            }
            if(parent==SOL_MIR_RUNTIME_NONE&&source_place<cap_m->place_count
                &&!cap_m->places[source_place].projections.count)
                parent=root_for_local(o,cap_m->places[source_place].local,&audit_meter);
            if(parent==SOL_MIR_RUNTIME_NONE||(root=o->capabilities[parent].source_root)==SOL_MIR_RUNTIME_NONE){free(active);free(seen);bad(d,"unresolved capability parent");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
        }
        SolMirRuntimeHostCapabilityPlan want={p->result_recipe,source,i,p->result,temporary,
            source_value,operand->temporary,source_place,root,parent,
            source==SOL_MIR_RUNTIME_HOST_CAPABILITY_PRIVATE_SOURCE?operand->recipe:SOL_MIR_RECIPE_NONE,
            source==SOL_MIR_RUNTIME_HOST_CAPABILITY_PRIVATE_SOURCE?source_value:SOL_MIR_RUNTIME_NONE};
        if(capability>=o->capability_count
            ||memcmp(&o->capabilities[capability++],&want,sizeof(want))){
            free(active);free(seen);bad(d,"constructed capability source mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;
        }
    }
    if(capability!=o->capability_count){free(active);free(seen);bad(d,"capability source census mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    for(size_t i=0;i<o->capability_count;i++){
        if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){free(active);free(seen);return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
        const SolMirRuntimeHostCapabilityPlan*x=&o->capabilities[i];
        if(x->source==SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT){
            if(x->source_root>=o->entry_root_count
                ||(x->source_construct==SOL_MIR_RUNTIME_NONE
                    ?x->parent!=SOL_MIR_RUNTIME_NONE:x->parent!=x->source_root)
                ||x->private_recipe!=SOL_MIR_RECIPE_NONE||x->private_value!=SOL_MIR_RUNTIME_NONE){free(active);free(seen);bad(d,"capability root lineage");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
        }else if(x->source==SOL_MIR_RUNTIME_HOST_CAPABILITY_DERIVED){
            if(x->parent>=i||x->source_root>=o->entry_root_count
                ||o->capabilities[x->parent].source==SOL_MIR_RUNTIME_HOST_CAPABILITY_PRIVATE_SOURCE
                ||o->capabilities[x->parent].source_root!=x->source_root
                ||x->private_recipe!=SOL_MIR_RECIPE_NONE||x->private_value!=SOL_MIR_RUNTIME_NONE){free(active);free(seen);bad(d,"capability derived lineage");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
        }else if(x->source==SOL_MIR_RUNTIME_HOST_CAPABILITY_PRIVATE_SOURCE){
            if(x->source_root!=SOL_MIR_RUNTIME_NONE||x->parent!=SOL_MIR_RUNTIME_NONE
                ||x->private_recipe==SOL_MIR_RECIPE_NONE||x->private_value!=x->source_value){free(active);free(seen);bad(d,"capability private lineage");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
        }else{free(active);free(seen);bad(d,"capability lineage class");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    }
    size_t formal=0;
    for(size_t h=0;h<l->host_requirement_count;h++){
        if(!e3_host_requirement(o,h,&audit_meter)){free(active);free(seen);bad(d,"host profile is outside frozen E3 surface");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
        const SolMirLinkageHostRequirement*host=&l->host_requirements[h];
        if(h>=o->operation_count){free(active);free(seen);bad(d,"operation census");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
        const SolMirRuntimeHostOperation*x=&o->operations[h];
        size_t import=SOL_MIR_RUNTIME_NONE,sig=SOL_MIR_RUNTIME_NONE;
        for(size_t i=0;i<o->conventions->import_count;i++){
            if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){free(active);free(seen);return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
            if(o->conventions->imports[i].kind==SOL_MIR_RUNTIME_IMPORT_HOST&&o->conventions->imports[i].host==h)import=i;
        }
        for(size_t i=0;i<o->conventions->signature_count;i++){
            if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){free(active);free(seen);return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
            if(o->conventions->signatures[i].origin==SOL_MIR_RUNTIME_SIGNATURE_HOST&&o->conventions->signatures[i].host==h)sig=i;
        }
        if(import==SOL_MIR_RUNTIME_NONE||sig==SOL_MIR_RUNTIME_NONE||x->host!=h||x->import_id!=import||x->signature!=sig||x->receiver!=host->receiver||x->receiver_access!=host->receiver_access||x->result!=host->result||x->result_plan!=host->result||host->result>=o->values->host_result_plan_count||x->result_class!=o->values->host_result_plans[host->result].classification||x->effects!=host->effects||x->failure_mask!=HOST_FAILURE_MASK||memcmp(&x->identity,&o->conventions->imports[import].identity,sizeof(x->identity))||memcmp(&x->symbol,&o->conventions->imports[import].symbol,sizeof(x->symbol))||x->formals.offset!=formal||x->formals.count!=host->parameters.count){free(active);free(seen);bad(d,"host surface mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
        for(size_t q=0;q<host->parameters.count;q++){
            if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){free(active);free(seen);return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
            if(formal>=o->formal_count||o->formals[formal].recipe!=o->conventions->concrete->materialization.type_ids[host->parameters.offset+q]||o->formals[formal].access!=o->conventions->concrete->materialization.accesses[host->parameter_accesses.offset+q]||!validate_shape(o,o->formals[formal].shape,active,seen,&audit_meter)){free(active);free(seen);bad(d,"host formal or E3 shape mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
            ++formal;
        }
    }
    for(size_t i=0;i<o->shape_count;i++){
        if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){free(active);free(seen);return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
        if(!seen[i]){free(active);free(seen);bad(d,"unconsumed or disconnected host shape");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    }
    if(o->operation_count!=l->host_requirement_count||formal!=o->formal_count){free(active);free(seen);bad(d,"host formal census mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    for(size_t i=0;i<o->shape_count;i++){
        if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){free(active);free(seen);return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
        if(!seen[i]){free(active);free(seen);bad(d,"orphan host shape");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    }
    free(active);free(seen);
    /* The flat argument census deliberately mirrors formal order.  It gives
     * max_arguments an independently authenticated meaning while recursive
     * selection remains owned by the formal's shape root. */
    if(o->argument_count!=o->formal_count){bad(d,"host argument census mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    for(size_t i=0;i<o->argument_count;i++){SolMirRuntimeHostArgumentKind want;
        if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){bad(d,"runtime host ABI validation work exhausted");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
        const SolMirRuntimeHostArgument*x=&o->arguments[i];
        const SolMirRuntimeHostFormal*f=&o->formals[i];
        if(x->recipe!=f->recipe||x->access!=f->access||x->children.count
            ||x->children.offset||x->recipe>=r->recipe_count
            ||!kind(&r->recipes[x->recipe],&want)||x->kind!=want){
            bad(d,"host argument descriptor mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;
        }
    }
    for(size_t i=0;i<o->requirement_count;i++){
        if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){bad(d,"runtime host ABI validation work exhausted");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
        const SolMirRuntimeHostRequirement*q=&o->requirements[i];
        if(q->entry>=o->conventions->entry_count||q->root>=o->entry_root_count||q->operation>=o->operation_count||q->call>=o->conventions->call_count||o->entry_roots[q->root].entry!=q->entry||call_host(o,&o->conventions->calls[q->call])!=o->operations[q->operation].host||!cleanup_mapping(o,q)){bad(d,"entry requirement transition mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
        for(size_t j=0;j<i;j++){
            if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){bad(d,"runtime host ABI validation work exhausted");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
            if(!memcmp(q,&o->requirements[j],sizeof(*q))){bad(d,"duplicate entry requirement");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
        }
    }
    SolMirRuntimeHostAbiWorkMeter validation_meter={o->limits.max_validation_work-audit_meter.used,0};
    if(!charge_usage_work(o,&validation_meter)||!requirements_complete(o,&validation_meter)){bad(d,"entry requirement closure mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    size_t bytes=0,t;if(!mul(o->capability_count,sizeof(*o->capabilities),&t)||!add(&bytes,t)||!mul(o->entry_root_count,sizeof(*o->entry_roots),&t)||!add(&bytes,t)||!mul(o->operation_count,sizeof(*o->operations),&t)||!add(&bytes,t)||!mul(o->argument_count,sizeof(*o->arguments),&t)||!add(&bytes,t)||!mul(o->formal_count,sizeof(*o->formals),&t)||!add(&bytes,t)||!mul(o->shape_count,sizeof(*o->shapes),&t)||!add(&bytes,t)||!mul(o->shape_case_count,sizeof(*o->shape_cases),&t)||!add(&bytes,t)||!mul(o->requirement_count,sizeof(*o->requirements),&t)||!add(&bytes,t)){bad(d,"runtime host ABI usage overflow");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}size_t grants=0;for(size_t i=0;i<o->requirement_count;i++){if(!sol_mir_runtime_host_abi_work_tick(&validation_meter)){bad(d,"runtime host ABI validation work exhausted");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}bool seen_grant=false;for(size_t q=0;q<i;q++){if(!sol_mir_runtime_host_abi_work_tick(&validation_meter)){bad(d,"runtime host ABI validation work exhausted");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}seen_grant|=o->requirements[q].entry==o->requirements[i].entry&&o->requirements[q].root==o->requirements[i].root&&o->requirements[q].operation==o->requirements[i].operation;}if(!seen_grant)++grants;}const SolMirMaterialization*m=&o->conventions->concrete->materialization;size_t root_bits,grant_bits,root_scratch,shape_scratch=0,validation_roots=0;if(!mul(o->entry_root_count,m->local_count,&root_bits)||!mul(o->entry_root_count,l->host_requirement_count,&grant_bits)||!mul(o->entry_root_count,sizeof(*o->entry_roots),&root_scratch)||!add(&root_scratch,m->image_count)||!add(&root_scratch,root_bits)||!add(&root_scratch,grant_bits)||!add(&shape_scratch,r->recipe_count)||!add(&shape_scratch,o->shape_count)||!add(&validation_roots,m->image_count)||!add(&validation_roots,root_bits)){bad(d,"runtime host ABI scratch overflow");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}size_t build_scratch=(r->recipe_count?r->recipe_count:1)>root_scratch?(r->recipe_count?r->recipe_count:1):root_scratch;size_t validation_scratch=shape_scratch>validation_roots?shape_scratch:validation_roots;size_t total_validation_work=audit_meter.used;if(!add(&total_validation_work,validation_meter.used)){bad(d,"runtime host ABI validation work overflow");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}if(measuring_validation_work){observed_validation_work=total_validation_work;return SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED;}if(o->usage.capabilities!=o->capability_count||o->usage.entry_roots!=o->entry_root_count||o->usage.operations!=o->operation_count||o->usage.arguments!=o->argument_count||o->usage.formals!=o->formal_count||o->usage.shapes!=o->shape_count||o->usage.shape_cases!=o->shape_case_count||o->usage.requirements!=o->requirement_count||o->usage.grants!=grants||o->usage.owned_bytes!=bytes||o->usage.build_scratch_bytes!=build_scratch||o->usage.validation_scratch_bytes!=validation_scratch||o->usage.validation_work!=total_validation_work){bad(d,"runtime host ABI usage mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    if(o->usage.capabilities>o->limits.max_capabilities||o->usage.entry_roots>o->limits.max_entry_roots||o->usage.operations>o->limits.max_operations||o->usage.arguments>o->limits.max_arguments||o->usage.formals>o->limits.max_formals||o->usage.shapes>o->limits.max_shapes||o->usage.shape_cases>o->limits.max_shape_cases||o->usage.requirements>o->limits.max_requirements||o->usage.grants>o->limits.max_grants||o->usage.owned_bytes>o->limits.max_owned_bytes||o->usage.build_scratch_bytes>o->limits.max_build_scratch_bytes||o->usage.build_work>o->limits.max_build_work||o->usage.validation_scratch_bytes>o->limits.max_validation_scratch_bytes||o->usage.validation_work>o->limits.max_validation_work){bad(d,"runtime host ABI usage exceeds owner limit");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    sol_mir_runtime_host_abi_internal_record_validation_work(audit_meter.used,
        validation_meter.used);
#endif
    return SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED;
}
bool sol_mir_runtime_host_abi_validate(const SolMirRuntimeHostAbi*o,SolDiagnostics*d){return sol_mir_runtime_host_abi_internal_validate(o,d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED;}
bool sol_mir_runtime_host_abi_internal_measure_validation_work(
    const SolMirRuntimeHostAbi *o, size_t *work) {
    if (!o || !work) return false;
    measuring_validation_work=true;
    observed_validation_work=0;
    SolMirRuntimeHostAbiBuildOutcome result=
        sol_mir_runtime_host_abi_internal_validate(o,NULL);
    measuring_validation_work=false;
    if (result!=SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED) return false;
    *work=observed_validation_work;
    return true;
}
