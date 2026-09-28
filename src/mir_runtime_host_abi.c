#include "sol/mir_runtime_host_abi.h"
#include "mir_runtime_host_abi_internal.h"
#include "mir_linkage_internal.h"

#include <stdlib.h>
#include <string.h>

#ifdef SOL_MIR_PLAN_TEST_HOOKS
static _Thread_local bool fail_persistent;
static _Thread_local size_t fail_persistent_attempt;
static _Thread_local bool fail_build_scratch;
_Thread_local size_t sol_mir_runtime_host_abi_internal_allocation_attempts;
_Thread_local SolMirRuntimeHostAbiWorkCensus
    sol_mir_runtime_host_abi_internal_work_census;
size_t sol_mir_runtime_host_abi_test_allocation_attempts(void) {
    return sol_mir_runtime_host_abi_internal_allocation_attempts;
}
void sol_mir_runtime_host_abi_test_reset_allocation_attempts(void) {
    sol_mir_runtime_host_abi_internal_allocation_attempts = 0;
}
SolMirRuntimeHostAbiWorkCensus sol_mir_runtime_host_abi_test_work_census(void) {
    return sol_mir_runtime_host_abi_internal_work_census;
}
void sol_mir_runtime_host_abi_internal_record_validation_work(size_t audit,
    size_t validation) {
    sol_mir_runtime_host_abi_internal_work_census.validation_audit_work=audit;
    sol_mir_runtime_host_abi_internal_work_census.validation_replay_work=validation;
    sol_mir_runtime_host_abi_internal_work_census.validation_work=audit+validation;
}
void sol_mir_runtime_host_abi_test_force_persistent_allocation_failure(bool value) { fail_persistent = value; }
void sol_mir_runtime_host_abi_test_force_persistent_allocation_failure_attempt(size_t value) { fail_persistent_attempt = value; }
void sol_mir_runtime_host_abi_test_force_build_scratch_allocation_failure(bool value) { fail_build_scratch = value; }
#endif

enum { HOST_FAILURE_MASK = UINT32_C(0x00008240) };

static bool add(size_t *a, size_t b) { if (b > SIZE_MAX - *a) return false; *a += b; return true; }
static bool mul(size_t a, size_t b, size_t *out) { if (a && b > SIZE_MAX / a) return false; *out = a * b; return true; }
static bool diagnostic(SolDiagnostics *d, const char *message) {
    if (d) sol_diagnostics_add(d, "SOL-MIR-RUNTIME-HOST-ABI-001", SOL_SEVERITY_ERROR,
        (SolSpan){0}, message);
    return false;
}
static bool zero(SolMirRuntimeHostAbiLimits l) {
    return !l.max_capabilities && !l.max_entry_roots && !l.max_operations && !l.max_arguments
        && !l.max_formals && !l.max_shapes && !l.max_shape_cases && !l.max_requirements
        && !l.max_grants && !l.max_owned_bytes && !l.max_build_scratch_bytes
        && !l.max_build_work && !l.max_validation_scratch_bytes && !l.max_validation_work;
}
static bool full(SolMirRuntimeHostAbiLimits l) {
    return l.max_capabilities && l.max_entry_roots && l.max_operations && l.max_arguments
        && l.max_formals && l.max_shapes && l.max_shape_cases && l.max_requirements
        && l.max_grants && l.max_owned_bytes && l.max_build_scratch_bytes
        && l.max_build_work && l.max_validation_scratch_bytes && l.max_validation_work;
}
static bool empty(const SolMirRuntimeHostAbi *o) {
    SolMirRuntimeHostAbiUsage usage = {0};
    return o && !o->conventions && !o->values && !o->cleanup && !o->capabilities
        && !o->entry_roots && !o->operations && !o->arguments && !o->formals && !o->shapes
        && !o->shape_cases && !o->requirements && !o->capability_count && !o->entry_root_count
        && !o->operation_count && !o->argument_count && !o->formal_count && !o->shape_count
        && !o->shape_case_count && !o->requirement_count && zero(o->limits)
        && !memcmp(&o->usage, &usage, sizeof(usage));
}
void sol_mir_runtime_host_abi_init(SolMirRuntimeHostAbi *o) { if (o) memset(o, 0, sizeof(*o)); }
void sol_mir_runtime_host_abi_free(SolMirRuntimeHostAbi *o) {
    if (!o) return;
    free(o->capabilities); free(o->entry_roots); free(o->operations); free(o->arguments);
    free(o->formals); free(o->shapes); free(o->shape_cases); free(o->requirements);
    sol_mir_runtime_host_abi_init(o);
}
SolMirRuntimeHostAbiLimits sol_mir_runtime_host_abi_default_limits(void) {
    return (SolMirRuntimeHostAbiLimits){4000000,4000000,4000000,4000000,16000000,32000000,
        32000000,16000000,16000000,1024u * 1024u * 1024u,256u * 1024u * 1024u,
        (size_t)4000000000ULL,256u * 1024u * 1024u,(size_t)4000000000ULL};
}
static void *allocate(size_t count, size_t size) {
    if (!count || count > SIZE_MAX / size) return NULL;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    ++sol_mir_runtime_host_abi_internal_allocation_attempts;
#endif
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (fail_persistent || (fail_persistent_attempt
            && sol_mir_runtime_host_abi_internal_allocation_attempts
                == fail_persistent_attempt)) return NULL;
#endif
    return calloc(count, size);
}
static void *scratch_allocate(size_t bytes) {
    if (!bytes) return NULL;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (fail_build_scratch) return NULL;
#endif
    return calloc(bytes, 1);
}
static bool shape_kind(const SolMirRecipe *r, SolMirRuntimeHostArgumentKind *kind) {
    switch (r->kind) {
        case SOL_MIR_RECIPE_INT64: *kind=SOL_MIR_RUNTIME_HOST_ARGUMENT_INT64; return true;
        case SOL_MIR_RECIPE_BOOL: *kind=SOL_MIR_RUNTIME_HOST_ARGUMENT_BOOL; return true;
        case SOL_MIR_RECIPE_TEXT: *kind=SOL_MIR_RUNTIME_HOST_ARGUMENT_TEXT; return true;
        case SOL_MIR_RECIPE_UNIT: *kind=SOL_MIR_RUNTIME_HOST_ARGUMENT_UNIT; return true;
        case SOL_MIR_RECIPE_OPTION: *kind=SOL_MIR_RUNTIME_HOST_ARGUMENT_OPTION; return true;
        case SOL_MIR_RECIPE_RESULT: *kind=SOL_MIR_RUNTIME_HOST_ARGUMENT_RESULT; return true;
        default: return false;
    }
}
static bool text_is(const char *text, const char *want) {
    return text && want && strcmp(text, want) == 0;
}
static bool effect_is(const SolMirMaterialization *m,
    const SolMirMaterializedEffectAtom *atom, const char *want) {
    size_t length = strlen(want);
    return atom->name.count == length && atom->name.offset <= m->effect_name_count
        && length <= m->effect_name_count - atom->name.offset
        && !memcmp(m->effect_names + atom->name.offset, want, length);
}
static bool recipe_kind_is(const SolMirRepresentation *r, SolMirRecipeId id,
    SolMirRecipeKind kind) {
    return id < r->recipe_count && r->recipes[id].kind == kind;
}
/* The predecessor representation is already authenticated.  A depth bound is
 * sufficient for the count-only pass and keeps resource preflight allocation
 * free; append_shape later uses the one workspace to retain exact cycle state. */
static bool count_shape(const SolMirRepresentation *r, SolMirRecipeId recipe,
    size_t depth, size_t *shapes, size_t *cases, SolMirRuntimeHostAbiWorkMeter *work) {
    SolMirRuntimeHostArgumentKind kind;
    if (!sol_mir_runtime_host_abi_work_tick(work) || recipe >= r->recipe_count || depth > r->recipe_count || !r->recipes[recipe].inhabited
        || !shape_kind(&r->recipes[recipe], &kind) || !add(shapes, 1)) return false;
    if (kind != SOL_MIR_RUNTIME_HOST_ARGUMENT_OPTION && kind != SOL_MIR_RUNTIME_HOST_ARGUMENT_RESULT)
        return true;
    for (size_t i=0; i<r->recipes[recipe].variants.count; ++i) {
        if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
        const SolMirRecipeVariant *v = &r->variants[r->recipes[recipe].variants.offset+i];
        if (v->fields.count > 1 || !add(cases, 1)
            || (v->fields.count && !count_shape(r, r->fields[v->fields.offset].type,
                 depth+1, shapes, cases, work))) return false;
    }
    return true;
}
static bool append_shape(SolMirRuntimeHostAbi *o, SolMirRecipeId recipe, SolAccessMode access,
    unsigned char *active, size_t *out, SolMirRuntimeHostAbiWorkMeter *work) {
    const SolMirRepresentation *r=&o->conventions->concrete->representation;
    SolMirRuntimeHostArgumentKind kind;
    if (!sol_mir_runtime_host_abi_work_tick(work) || recipe >= r->recipe_count || active[recipe] || o->shape_count >= o->shape_capacity
        || !r->recipes[recipe].inhabited || !shape_kind(&r->recipes[recipe],&kind)) return false;
    size_t id=o->shape_count++; o->shapes[id]=(SolMirRuntimeHostShape){recipe,kind,access,{o->shape_case_count,0}};
    if (kind==SOL_MIR_RUNTIME_HOST_ARGUMENT_OPTION || kind==SOL_MIR_RUNTIME_HOST_ARGUMENT_RESULT) {
        active[recipe]=1;
        /* Reserve the direct slice before appending children.  Recursing first
         * interleaves a child's cases into its parent's slice. */
        size_t direct = r->recipes[recipe].variants.count;
        if (direct > o->shape_case_capacity-o->shape_case_count) { active[recipe]=0; return false; }
        o->shape_case_count += direct;
        o->shapes[id].cases.count=direct;
        for (size_t i=0;i<r->recipes[recipe].variants.count;i++) {
            if (!sol_mir_runtime_host_abi_work_tick(work)) { active[recipe]=0; return false; }
            const SolMirRecipeVariant *v=&r->variants[r->recipes[recipe].variants.offset+i];
            if (v->fields.count>1) { active[recipe]=0; return false; }
            size_t child=SOL_MIR_RUNTIME_NONE;
            if (v->fields.count && !append_shape(o,r->fields[v->fields.offset].type,access,active,&child,work)) { active[recipe]=0; return false; }
            o->shape_cases[o->shapes[id].cases.offset+i]=(SolMirRuntimeHostShapeCase){id,v->ordinal,child};
        }
        active[recipe]=0;
    }
    *out=id; return true;
}
/* E3 authority is the selected P2/P3.1 requirement/import identity.  Names are
 * intentionally not part of this decision: a same-spelled member in another
 * module is a distinct requirement, import identity, operation, and grant. */
static bool e3_host_requirement(const SolMirRuntimeHostAbi *o, size_t host_id,
    SolMirRuntimeHostAbiWorkMeter *work) {
    const SolMirLinkage *l=&o->conventions->concrete->linkage;
    const SolMirMaterialization *m=&o->conventions->concrete->materialization;
    const SolIr *ir=o->conventions->concrete->program.ir;
    const SolMirRepresentation *r=&o->conventions->concrete->representation;
    if(!sol_mir_runtime_host_abi_work_tick(work) || host_id>=l->host_requirement_count || !ir) return false;
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
    const SolMirMaterializedEffectAtom *atom=&m->effect_atoms[atom_id];
    if(atom->authority!=SOL_MIR_PLAN_EFFECT_AUTHORITY_RECEIVER
        || atom->name.offset>m->effect_name_count
        || atom->name.count>m->effect_name_count-atom->name.offset) return false;
    if(h->parameters.offset>m->type_id_count
        || h->parameters.count>m->type_id_count-h->parameters.offset
        || h->parameter_accesses.offset>m->access_count
        || h->parameter_accesses.count!=h->parameters.count
        || h->parameter_accesses.count>m->access_count-h->parameter_accesses.offset)
        return false;
    const char *capability=ir->definitions[callable->owner].name;
    const char *member=callable->name;
    const char *effect=NULL;
    SolMirRecipeKind parameter=SOL_MIR_RECIPE_INT64, result=SOL_MIR_RECIPE_INT64;
    size_t parameter_count=0;
    if(text_is(capability,"Console") && text_is(member,"write")) {
        effect="console.write"; parameter=SOL_MIR_RECIPE_TEXT;
        result=SOL_MIR_RECIPE_UNIT; parameter_count=1;
    } else if(text_is(capability,"Arguments") && text_is(member,"count")) {
        effect="process.arguments.count"; result=SOL_MIR_RECIPE_INT64;
    } else if(text_is(capability,"Arguments") && text_is(member,"get")) {
        effect="process.arguments.get"; parameter=SOL_MIR_RECIPE_INT64;
        result=SOL_MIR_RECIPE_OPTION; parameter_count=1;
    } else if(text_is(capability,"Configuration") && text_is(member,"read")) {
        effect="configuration.read"; parameter=SOL_MIR_RECIPE_TEXT;
        result=SOL_MIR_RECIPE_OPTION; parameter_count=1;
    } else return false;
    if(!effect_is(m,atom,effect) || h->parameters.count!=parameter_count
        || !recipe_kind_is(r,h->result,result)) return false;
    if(result==SOL_MIR_RECIPE_OPTION) {
        const SolMirRecipe *option=&r->recipes[h->result];
        bool text_payload=false;
        for(size_t i=0;i<option->variants.count;i++) {
            if(!sol_mir_runtime_host_abi_work_tick(work)) return false;
            const SolMirRecipeVariant *variant=&r->variants[option->variants.offset+i];
            if(variant->fields.count==1 && recipe_kind_is(r,
                    r->fields[variant->fields.offset].type,SOL_MIR_RECIPE_TEXT))
                text_payload=true;
            else if(variant->fields.count>1) return false;
        }
        if(!text_payload) return false;
    }
    for(size_t i=0;i<h->parameters.count;i++) {
        if(!sol_mir_runtime_host_abi_work_tick(work)) return false;
        SolMirRecipeId recipe=m->type_ids[h->parameters.offset+i];
        if(!recipe_kind_is(r,recipe,parameter)
            ||m->accesses[h->parameter_accesses.offset+i]!=SOL_ACCESS_OWNED) return false;
    }
    return true;
}
static const SolMirRuntimeImport *find_import(const SolMirRuntimeConventions *c,size_t host,
    size_t *id, SolMirRuntimeHostAbiWorkMeter *work) {
    for(size_t i=0;i<c->import_count;i++) {
        if (!sol_mir_runtime_host_abi_work_tick(work)) return NULL;
        if(c->imports[i].kind==SOL_MIR_RUNTIME_IMPORT_HOST&&c->imports[i].host==host){if(id)*id=i;return &c->imports[i];}
    } return NULL;
}
static const SolMirRuntimeSignature *find_signature(const SolMirRuntimeConventions *c,size_t host,
    size_t *id, SolMirRuntimeHostAbiWorkMeter *work) {
    for(size_t i=0;i<c->signature_count;i++) {
        if (!sol_mir_runtime_host_abi_work_tick(work)) return NULL;
        if(c->signatures[i].origin==SOL_MIR_RUNTIME_SIGNATURE_HOST&&c->signatures[i].host==host){if(id)*id=i;return &c->signatures[i];}
    } return NULL;
}
/* The E6 ABI is a closed identity table, not an open spelling convention.
 * In particular, a second Console.write-shaped requirement is not another
 * spelling of the first import: it is a fifth host identity and is rejected. */
static int frozen_profile_kind(const SolMirRuntimeHostAbi *o, size_t host) {
    const SolMirLinkage *l=&o->conventions->concrete->linkage;
    const SolMirMaterialization *m=&o->conventions->concrete->materialization;
    const SolIr *ir=o->conventions->concrete->program.ir;
    if (!ir || host>=l->host_requirement_count) return -1;
    const SolMirLinkageHostRequirement *h=&l->host_requirements[host];
    if (h->import>=m->import_count || m->imports[h->import].source_callable>=ir->callable_count)
        return -1;
    const SolIrCallable *callable=&ir->callables[m->imports[h->import].source_callable];
    if (callable->owner>=ir->definition_count) return -1;
    const char *capability=ir->definitions[callable->owner].name;
    if (text_is(capability,"Console") && text_is(callable->name,"write")) return 0;
    if (text_is(capability,"Arguments") && text_is(callable->name,"count")) return 1;
    if (text_is(capability,"Arguments") && text_is(callable->name,"get")) return 2;
    if (text_is(capability,"Configuration") && text_is(callable->name,"read")) return 3;
    return -1;
}
static bool frozen_e6_surface(const SolMirRuntimeHostAbi *o,
    SolMirRuntimeHostAbiWorkMeter *work) {
    const SolMirLinkage *l=&o->conventions->concrete->linkage;
    const SolMirRuntimeConventions *c=o->conventions;
    unsigned seen=0;
    if (l->host_requirement_count>4) return false;
    size_t imports=0;
    for (size_t i=0;i<c->import_count;i++) {
        if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
        if (c->imports[i].kind==SOL_MIR_RUNTIME_IMPORT_HOST) ++imports;
    }
    if (imports!=l->host_requirement_count) return false;
    for (size_t host=0;host<l->host_requirement_count;host++) {
        if (!sol_mir_runtime_host_abi_work_tick(work) || !e3_host_requirement(o,host,work))
            return false;
        int kind=frozen_profile_kind(o,host);
        if (kind<0 || (seen&(1u<<kind))) return false;
        seen|=1u<<kind;
        size_t import_count=0, signature_count=0;
        for (size_t i=0;i<c->import_count;i++) {
            if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
            if (c->imports[i].kind==SOL_MIR_RUNTIME_IMPORT_HOST && c->imports[i].host==host)
                ++import_count;
        }
        for (size_t i=0;i<c->signature_count;i++) {
            if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
            if (c->signatures[i].origin==SOL_MIR_RUNTIME_SIGNATURE_HOST
                && c->signatures[i].host==host) ++signature_count;
        }
        if (import_count!=1 || signature_count!=1) return false;
        for (size_t prior=0;prior<host;prior++) {
            if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
            const SolMirRuntimeImport *left=find_import(c,prior,NULL,work);
            const SolMirRuntimeImport *right=find_import(c,host,NULL,work);
            if (!left || !right || !memcmp(&left->identity,&right->identity,sizeof(left->identity))
                || !memcmp(&left->symbol,&right->symbol,sizeof(left->symbol))) return false;
        }
    }
    return true;
}
static size_t image_for_instance(const SolMirMaterialization *m, size_t instance,
    SolMirRuntimeHostAbiWorkMeter *work) {
    for(size_t i=0;i<m->image_count;i++) {
        if (!sol_mir_runtime_host_abi_work_tick(work)) return SOL_MIR_RUNTIME_NONE;
        if(m->images[i].instance==instance) return i;
    }
    return SOL_MIR_RUNTIME_NONE;
}
static size_t target_image(const SolMirRuntimeHostAbi *o,const SolMirRuntimeCall *call,
    SolMirRuntimeHostAbiWorkMeter *work) {
    const SolMirLinkage *l=&o->conventions->concrete->linkage; size_t internal=SOL_MIR_RUNTIME_NONE;
    if(call->target_kind==SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL) internal=call->internal;
    else if(call->target_kind==SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE&&call->table<l->table_entry_count
        &&l->table_entries[call->table].target_kind==SOL_MIR_LINKAGE_TARGET_INTERNAL) internal=l->table_entries[call->table].internal;
    if(internal>=l->callable_count)return SOL_MIR_RUNTIME_NONE;
    return image_for_instance(&o->conventions->concrete->materialization,
        l->callables[internal].instance,work);
}
static size_t target_host(const SolMirRuntimeHostAbi *o,const SolMirRuntimeCall *call) {
    const SolMirLinkage *l=&o->conventions->concrete->linkage;
    if(call->target_kind==SOL_MIR_RUNTIME_TARGET_DIRECT_HOST)return call->host;
    if(call->target_kind==SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE&&call->table<l->table_entry_count
        &&l->table_entries[call->table].target_kind==SOL_MIR_LINKAGE_TARGET_HOST)return l->table_entries[call->table].host;
    return SOL_MIR_RUNTIME_NONE;
}
/* A capability transfer is named by the materialized temporary-init edge, not
 * by a recipe or by a construct-operand position. */
static bool temporary_coordinate(const SolMirMaterialization *m,
    SolMirMaterializedTemporaryId temporary, SolMirMaterializedValueId *value,
    SolMirMaterializedPlaceId *place, SolMirRuntimeHostAbiWorkMeter *work) {
    size_t found=SOL_MIR_RUNTIME_NONE;
    if (temporary==SOL_MIR_RUNTIME_NONE) return false;
    for (size_t i=0;i<m->instruction_count;i++) {
        if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
        const SolMirMaterializedInstruction *x=&m->instructions[i];
        if (x->kind!=SOL_MIR_INST_TEMPORARY_INIT || x->temporary!=temporary) continue;
        if (found!=SOL_MIR_RUNTIME_NONE) return false;
        found=x->left;
    }
    if (found>=m->value_count) return false;
    *value=found; *place=SOL_MIR_RUNTIME_NONE;
    const SolMirMaterializedValue *v=&m->values[found];
    if (v->instruction<m->instruction_count) {
        if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
        const SolMirMaterializedInstruction *x=&m->instructions[v->instruction];
        if (x->kind==SOL_MIR_INST_LOAD_COPY || x->kind==SOL_MIR_INST_LOAD_MOVE
            || x->kind==SOL_MIR_INST_LOAD_UPDATE) *place=x->place;
    }
    return true;
}
static bool result_temporary(const SolMirMaterialization *m,
    SolMirMaterializedValueId result, SolMirMaterializedTemporaryId *temporary,
    SolMirRuntimeHostAbiWorkMeter *work) {
    size_t found=SOL_MIR_RUNTIME_NONE;
    for (size_t i=0;i<m->instruction_count;i++) {
        if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
        const SolMirMaterializedInstruction *x=&m->instructions[i];
        if (x->kind!=SOL_MIR_INST_TEMPORARY_INIT || x->left!=result) continue;
        if (found!=SOL_MIR_RUNTIME_NONE) return false;
        found=x->temporary;
    }
    if (found==SOL_MIR_RUNTIME_NONE) return false;
    *temporary=found; return true;
}
static size_t entry_root_for_local(const SolMirRuntimeHostAbi *o,
    SolMirMaterializedLocalId local, SolMirRuntimeHostAbiWorkMeter *work) {
    const SolMirMaterialization *m=&o->conventions->concrete->materialization;
    if (local>=m->local_count) return SOL_MIR_RUNTIME_NONE;
    for (size_t root=0;root<o->entry_root_count;root++) {
        if (!sol_mir_runtime_host_abi_work_tick(work)) return SOL_MIR_RUNTIME_NONE;
        const SolMirRuntimeHostEntryRoot *r=&o->entry_roots[root];
        if (r->entry>=o->conventions->entry_count) continue;
        const SolMirRuntimeEntry *entry=&o->conventions->entries[r->entry];
        const SolMirLinkage *l=&o->conventions->concrete->linkage;
        if (entry->callable>=l->callable_count) continue;
        size_t image=image_for_instance(m,l->callables[entry->callable].instance,work);
        if (image==SOL_MIR_RUNTIME_NONE) continue;
        const SolMirMaterializedImage *x=&m->images[image];
        for (size_t i=0;i<x->locals.count;i++) {
            if (!sol_mir_runtime_host_abi_work_tick(work)) return SOL_MIR_RUNTIME_NONE;
            size_t candidate=x->locals.offset+i;
            if (candidate==local && m->locals[candidate].kind==SOL_MIR_MATERIALIZED_LOCAL_PARAMETER
                && m->locals[candidate].ordinal==r->formal) return root;
        }
    }
    return SOL_MIR_RUNTIME_NONE;
}
static bool call_owner_reachable(const SolMirRuntimeHostAbi *o,size_t entry,const unsigned char *images,const SolMirRuntimeCall *call) {
    const SolMirOperations *ops=&o->conventions->concrete->operations;
    if(call->owner_kind==SOL_MIR_RUNTIME_CALL_OWNER_IMAGE) return call->image<o->conventions->concrete->materialization.image_count&&images[call->image];
    if(call->predicate>=ops->predicate_body_count)return false;
    const SolMirPredicateBody *body=&ops->predicate_bodies[call->predicate];
    (void)entry;
    return body->owner_kind==SOL_MIR_PREDICATE_OWNER_INSTANCE&&body->instance<o->conventions->concrete->materialization.image_count&&images[body->instance];
}
static bool mark_entry_images(const SolMirRuntimeHostAbi *o,size_t entry,unsigned char *images,
    SolMirRuntimeHostAbiWorkMeter *work) {
    const SolMirLinkage *l=&o->conventions->concrete->linkage;
    if(entry>=o->conventions->entry_count||o->conventions->entries[entry].callable>=l->callable_count)return false;
    size_t start=image_for_instance(&o->conventions->concrete->materialization,
        l->callables[o->conventions->entries[entry].callable].instance,work);
    if(start==SOL_MIR_RUNTIME_NONE)return false; images[start]=1;
    bool changed=true; while(changed){if(!sol_mir_runtime_host_abi_work_tick(work))return false;changed=false; for(size_t i=0;i<o->conventions->call_count;i++){if(!sol_mir_runtime_host_abi_work_tick(work))return false;const SolMirRuntimeCall *c=&o->conventions->calls[i];if(!call_owner_reachable(o,entry,images,c))continue;size_t image=target_image(o,c,work);if(image!=SOL_MIR_RUNTIME_NONE&&!images[image]){images[image]=1;changed=true;}}}
    return true;
}
static const SolMirRuntimeOperand *receiver_operand(const SolMirRuntimeHostAbi *o,const SolMirRuntimeCall *c) {
    if(!c->operands.count||c->operands.offset>=o->conventions->operand_count)return NULL;
    const SolMirRuntimeOperand *x=&o->conventions->operands[c->operands.offset];
    return x->signature_slot<o->conventions->signature_slot_count&&o->conventions->signature_slots[x->signature_slot].role==SOL_MIR_RUNTIME_SLOT_RECEIVER?x:NULL;
}
/* A root bit is transferred only through an authenticated actual/formal edge.
 * The loop includes direct and table targets; predicate calls are included in
 * the call closure and have their bound receiver resolved below. */
static bool exact_root_place(const SolMirMaterialization *m,const unsigned char *roots,
    size_t root,SolMirMaterializedPlaceId place, SolMirRuntimeHostAbiWorkMeter *work){
    if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
    return place<m->place_count&&m->places[place].local<m->local_count
        &&!m->places[place].projections.count
        &&roots[root*m->local_count+m->places[place].local];
}
static bool temporary_has_root(const SolMirRuntimeHostAbi *o,const unsigned char *roots,
    size_t root,SolMirMaterializedTemporaryId temporary,size_t depth,
    SolMirRuntimeHostAbiWorkMeter *work){
    const SolMirMaterialization*m=&o->conventions->concrete->materialization;
    const SolMirOperations*ops=&o->conventions->concrete->operations;
    if(depth>ops->constructor_count)return false;
    size_t found=SOL_MIR_RUNTIME_NONE;
    for(size_t i=0;i<ops->constructor_count;i++){
        if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
        const SolMirOperationConstructPlan*p=&ops->constructors[i];
        SolMirMaterializedTemporaryId result;
        if(p->kind!=SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY||!result_temporary(m,p->result,&result,work)
            ||result!=temporary)continue;
        if(found!=SOL_MIR_RUNTIME_NONE)return false;
        found=i;
    }
    if(found==SOL_MIR_RUNTIME_NONE){
        SolMirMaterializedValueId value;SolMirMaterializedPlaceId place;
        return temporary_coordinate(m,temporary,&value,&place,work)
            &&exact_root_place(m,roots,root,place,work);
    }
    const SolMirOperationConstructPlan*p=&ops->constructors[found];
    if(p->capability_source_operand>=p->operands.count)return false;
    const SolMirOperationConstructOperand*source=&ops->construct_operands[p->operands.offset+p->capability_source_operand];
    SolMirMaterializedValueId value;SolMirMaterializedPlaceId place;
    if(!temporary_coordinate(m,source->temporary,&value,&place,work))return false;
    /* A construction is never a host-bindable receiver.  ROOT_SOURCE and
     * BASE_SOURCE describe construction provenance, not an ambient authority
     * conversion.  inherited_root must not rescue a missing materialized
     * source coordinate. */
    (void)value;
    (void)place;
    (void)p;
    return false;
}
static bool operand_has_root(const SolMirRuntimeHostAbi *o,const unsigned char *roots,size_t root,
    const SolMirRuntimeOperand *operand, SolMirRuntimeHostAbiWorkMeter *work) {
    const SolMirMaterialization *m=&o->conventions->concrete->materialization;
    if(!operand)return false;
    if(operand->value.kind==SOL_MIR_RUNTIME_VALUE_MATERIALIZED_PLACE)
        return exact_root_place(m,roots,root,operand->value.id,work);
    return operand->value.kind==SOL_MIR_RUNTIME_VALUE_MATERIALIZED_TEMPORARY
        &&temporary_has_root(o,roots,root,operand->value.id,0,work);
}

/* A receiver is host-bindable when the authenticated provenance fixed point
 * contains exactly one entry-root origin.  This admits an ordinary call or
 * predicate formal reached through helper chains, but never a construction,
 * private source, projection, or ambiguous merge. */
static size_t receiver_origin(const SolMirRuntimeHostAbi *o, size_t entry,
    const unsigned char *roots, const SolMirRuntimeOperand *operand,
    SolMirRuntimeHostAbiWorkMeter *work) {
    size_t origin=SOL_MIR_RUNTIME_NONE;
    for (size_t root=0; root<o->entry_root_count; ++root) {
        if (!sol_mir_runtime_host_abi_work_tick(work)) return SOL_MIR_RUNTIME_NONE;
        if (o->entry_roots[root].entry!=entry
            || !operand_has_root(o,roots,root,operand,work)) continue;
        if (origin!=SOL_MIR_RUNTIME_NONE) return SOL_MIR_RUNTIME_NONE;
        origin=root;
    }
    return origin;
}
static bool closure_roots(const SolMirRuntimeHostAbi *o,size_t entry,const unsigned char *images,
    unsigned char *roots, SolMirRuntimeHostAbiWorkMeter *work) {
    const SolMirMaterialization*m=&o->conventions->concrete->materialization;
    size_t image=image_for_instance(m,o->conventions->concrete->linkage.callables[
        o->conventions->entries[entry].callable].instance,work);
    if (image==SOL_MIR_RUNTIME_NONE) return false;
    for(size_t r=0;r<o->entry_root_count;r++) {
        if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
        if(o->entry_roots[r].entry!=entry) continue;
        for(size_t l=0;l<m->local_count;l++) {
            if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
            if(m->locals[l].instance==m->images[image].instance
                &&m->locals[l].kind==SOL_MIR_MATERIALIZED_LOCAL_PARAMETER
                &&m->locals[l].ordinal==o->entry_roots[r].formal)
                roots[r*m->local_count+l]=1;
        }
    }
    bool changed=true;while(changed){if(!sol_mir_runtime_host_abi_work_tick(work))return false;changed=false;for(size_t ci=0;ci<o->conventions->call_count;ci++){if(!sol_mir_runtime_host_abi_work_tick(work))return false;const SolMirRuntimeCall*c=&o->conventions->calls[ci];if(!call_owner_reachable(o,entry,images,c))continue;size_t target=target_image(o,c,work);if(target==SOL_MIR_RUNTIME_NONE)continue;const SolMirMaterializedImage*im=&m->images[target];for(size_t oi=0;oi<c->operands.count;oi++){if(!sol_mir_runtime_host_abi_work_tick(work))return false;const SolMirRuntimeOperand*operand=&o->conventions->operands[c->operands.offset+oi];if(operand->signature_slot>=o->conventions->signature_slot_count)continue;const SolMirRuntimeSignatureSlot*slot=&o->conventions->signature_slots[operand->signature_slot];if(slot->role!=SOL_MIR_RUNTIME_SLOT_PARAMETER)continue;for(size_t li=0;li<im->locals.count;li++){if(!sol_mir_runtime_host_abi_work_tick(work))return false;size_t local=im->locals.offset+li;if(m->locals[local].kind!=SOL_MIR_MATERIALIZED_LOCAL_PARAMETER||m->locals[local].ordinal!=slot->formal)continue;for(size_t r=0;r<o->entry_root_count;r++)if(!sol_mir_runtime_host_abi_work_tick(work))return false;else if(o->entry_roots[r].entry==entry&&operand_has_root(o,roots,r,operand,work)&&!roots[r*m->local_count+local]){roots[r*m->local_count+local]=1;changed=true;}}}}}
    return true;
}

static bool call_cleanup(const SolMirRuntimeHostAbi *,size_t,size_t *,size_t *,
    SolMirRuntimeHostAbiWorkMeter *);

/* This is the complete read-side traversal paired with persistent emission.
 * It deliberately emits nothing: running it before allocation predicts the
 * exact subsequent write traversal, and running it again meters that write
 * traversal without making owner allocation part of the accounting contract. */
static bool persistent_write_traversal(const SolMirRuntimeHostAbi *o,
    SolMirRuntimeHostAbiWorkMeter *work, unsigned char *images,
    unsigned char *roots, size_t root_bits) {
    const SolMirRepresentation *r=&o->conventions->concrete->representation;
    const SolMirLinkage *l=&o->conventions->concrete->linkage;
    const SolMirMaterialization *m=&o->conventions->concrete->materialization;
    const SolMirOperations *ops=&o->conventions->concrete->operations;
    size_t capability_count=o->entry_root_count;
    for (size_t e=0;e<o->conventions->entry_count;e++) {
        if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
        const SolMirRuntimeSignature *s=&o->conventions->signatures[
            o->conventions->entries[e].signature];
        for (size_t q=0;q<s->slots.count;q++)
            if(!sol_mir_runtime_host_abi_work_tick(work)) return false;
    }
    for (size_t i=0;i<ops->constructor_count;i++)
        if(!sol_mir_runtime_host_abi_work_tick(work)) return false;
        else if (ops->constructors[i].kind==SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY) {
            const SolMirOperationConstructPlan *plan=&ops->constructors[i];
            SolMirMaterializedTemporaryId temporary;
            SolMirMaterializedValueId value;
            SolMirMaterializedPlaceId place;
            if (!result_temporary(m,plan->result,&temporary,work))
                temporary=SOL_MIR_RUNTIME_NONE;
            if (plan->capability_source_operand>=plan->operands.count
                || !temporary_coordinate(m,ops->construct_operands[
                    plan->operands.offset+plan->capability_source_operand].temporary,
                    &value,&place,work)) return false;
            if (plan->capability_rule==SOL_MIR_OPERATION_CAPABILITY_BASE_SOURCE)
                for (size_t q=0;q<capability_count;q++)
                    if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
            if (plan->capability_rule==SOL_MIR_OPERATION_CAPABILITY_ROOT_SOURCE) {
                size_t root=entry_root_for_local(o,plan->inherited_root,work);
                if(root==SOL_MIR_RUNTIME_NONE||place>=m->place_count
                    ||m->places[place].local!=plan->inherited_root
                    ||m->places[place].projections.count) return false;
            }
            ++capability_count;
        }
    for (size_t h=0;h<l->host_requirement_count;h++) {
        size_t shapes=0,cases=0;
        const SolMirLinkageHostRequirement *host=&l->host_requirements[h];
        if(!e3_host_requirement(o,h,work)) return false;
        if(!find_import(o->conventions,h,NULL,work)
            ||!find_signature(o->conventions,h,NULL,work)) return false;
        for (size_t q=0;q<host->parameters.count;q++)
            if(!sol_mir_runtime_host_abi_work_tick(work)
                || !count_shape(r,m->type_ids[host->parameters.offset+q],0,
                    &shapes,&cases,work)) return false;
    }
    for (size_t e=0;e<o->conventions->entry_count;e++) {
        if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
        memset(images,0,m->image_count); memset(roots,0,root_bits);
        if(!mark_entry_images(o,e,images,work)
            ||!closure_roots(o,e,images,roots,work)) return false;
        for (size_t c=0;c<o->conventions->call_count;c++) {
            const SolMirRuntimeCall *call=&o->conventions->calls[c];
            if(!sol_mir_runtime_host_abi_work_tick(work)) return false;
            if(!call_owner_reachable(o,e,images,call)
                ||target_host(o,call)==SOL_MIR_RUNTIME_NONE) continue;
            const SolMirRuntimeOperand *receiver=receiver_operand(o,call);
            if(!receiver) return false;
            if(receiver_origin(o,e,roots,receiver,work)==SOL_MIR_RUNTIME_NONE)
                return false;
            size_t event,transition;
            if(!call_cleanup(o,c,&event,&transition,work)) return false;
        }
    }
    return true;
}
static bool call_cleanup(const SolMirRuntimeHostAbi *o,size_t call,size_t *event,
    size_t *transition, SolMirRuntimeHostAbiWorkMeter *work) {
    if(call>=o->conventions->call_count)return false;const SolMirRuntimeCall*c=&o->conventions->calls[call];
    SolMirRuntimeCleanupProducerKind producer=c->owner_kind==SOL_MIR_RUNTIME_CALL_OWNER_IMAGE?SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_INVOKE:SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_INVOKE;
    size_t found_event=SOL_MIR_RUNTIME_NONE,found_transition=SOL_MIR_RUNTIME_NONE;
    for(size_t i=0;i<o->cleanup->event_count;i++){
        if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
        const SolMirRuntimeCleanupEvent*e=&o->cleanup->events[i];
        if(e->producer!=producer||e->block!=c->block
            ||e->owner!=(c->owner_kind==SOL_MIR_RUNTIME_CALL_OWNER_IMAGE?c->image:c->predicate))continue;
        for(size_t q=0;q<e->transitions.count;q++){
            if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
            size_t t=e->transitions.offset+q;
            const SolMirRuntimeCleanupTransition *candidate=&o->cleanup->transitions[t];
            if(candidate->edge_role!=SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE
                ||candidate->failure_mask!=HOST_FAILURE_MASK
                ||candidate->failure_source!=SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31
                ||candidate->failure_site!=c->failure_site)continue;
            if(found_transition!=SOL_MIR_RUNTIME_NONE)return false;
            found_event=i;found_transition=t;
        }
    }
    if(found_transition!=SOL_MIR_RUNTIME_NONE){*event=found_event;*transition=found_transition;return true;}
    return false;
}
static bool usage_bytes(SolMirRuntimeHostAbi *o) {
    size_t bytes=0,n;
#define B(member,count) do { if(!mul((count),sizeof(*o->member),&n)||!add(&bytes,n))return false; } while(0)
    B(capabilities,o->capability_count); B(entry_roots,o->entry_root_count);
    B(operations,o->operation_count); B(arguments,o->argument_count);
    B(formals,o->formal_count); B(shapes,o->shape_count);
    B(shape_cases,o->shape_case_count); B(requirements,o->requirement_count);
#undef B
    o->usage.owned_bytes=bytes;return true;
}
static bool fit(const SolMirRuntimeHostAbi *o){const SolMirRuntimeHostAbiUsage*u=&o->usage;const SolMirRuntimeHostAbiLimits*l=&o->limits;return u->capabilities<=l->max_capabilities&&u->entry_roots<=l->max_entry_roots&&u->operations<=l->max_operations&&u->arguments<=l->max_arguments&&u->formals<=l->max_formals&&u->shapes<=l->max_shapes&&u->shape_cases<=l->max_shape_cases&&u->requirements<=l->max_requirements&&u->grants<=l->max_grants&&u->owned_bytes<=l->max_owned_bytes&&u->build_scratch_bytes<=l->max_build_scratch_bytes&&u->build_work<=l->max_build_work&&u->validation_scratch_bytes<=l->max_validation_scratch_bytes&&u->validation_work<=l->max_validation_work;}

/* The owner seal is deliberately field-wise: it includes every header field and
 * every semantic arena field, but never C padding.  This gives preflight a
 * bounded, allocation-free hostile-mutation check before it follows a grant. */
static uint64_t header_mix(uint64_t state, uint64_t value) {
    state ^= value + UINT64_C(0x9e3779b97f4a7c15) + (state << 6) + (state >> 2);
    return state;
}
static uint64_t seal_bytes(uint64_t state, const void *bytes, size_t length) {
    const uint8_t *p = bytes;
    state = header_mix(state, length);
    for (size_t i = 0; i < length; ++i) state = header_mix(state, p[i]);
    return state;
}
static uint64_t seal_symbol(uint64_t state, const SolMirLinkageSymbol *symbol) {
    size_t length = 0;
    while (length < sizeof(symbol->bytes) && symbol->bytes[length]) ++length;
    return seal_bytes(state, symbol->bytes, length);
}
static uint64_t seal_digest(uint64_t state, const SolMirLinkageDigest *digest) {
    return seal_bytes(state, digest->bytes, sizeof(digest->bytes));
}
static uint64_t header_authentication(const SolMirRuntimeHostAbi *o) {
    uint64_t state=UINT64_C(0x686f73742d616269);
#define HEADER_VALUE(value) state=header_mix(state,(uint64_t)(value))
    HEADER_VALUE((uintptr_t)o->conventions); HEADER_VALUE((uintptr_t)o->values);
    HEADER_VALUE((uintptr_t)o->cleanup); HEADER_VALUE((uintptr_t)o->capabilities);
    HEADER_VALUE(o->capability_count); HEADER_VALUE(o->capability_capacity);
    HEADER_VALUE((uintptr_t)o->entry_roots); HEADER_VALUE(o->entry_root_count); HEADER_VALUE(o->entry_root_capacity);
    HEADER_VALUE((uintptr_t)o->operations); HEADER_VALUE(o->operation_count); HEADER_VALUE(o->operation_capacity);
    HEADER_VALUE((uintptr_t)o->arguments); HEADER_VALUE(o->argument_count); HEADER_VALUE(o->argument_capacity);
    HEADER_VALUE((uintptr_t)o->formals); HEADER_VALUE(o->formal_count); HEADER_VALUE(o->formal_capacity);
    HEADER_VALUE((uintptr_t)o->shapes); HEADER_VALUE(o->shape_count); HEADER_VALUE(o->shape_capacity);
    HEADER_VALUE((uintptr_t)o->shape_cases); HEADER_VALUE(o->shape_case_count); HEADER_VALUE(o->shape_case_capacity);
    HEADER_VALUE((uintptr_t)o->requirements); HEADER_VALUE(o->requirement_count); HEADER_VALUE(o->requirement_capacity);
#define HEADER_LIMIT(member) HEADER_VALUE(o->limits.member)
    HEADER_LIMIT(max_capabilities); HEADER_LIMIT(max_entry_roots); HEADER_LIMIT(max_operations); HEADER_LIMIT(max_arguments);
    HEADER_LIMIT(max_formals); HEADER_LIMIT(max_shapes); HEADER_LIMIT(max_shape_cases); HEADER_LIMIT(max_requirements); HEADER_LIMIT(max_grants);
    HEADER_LIMIT(max_owned_bytes); HEADER_LIMIT(max_build_scratch_bytes); HEADER_LIMIT(max_build_work); HEADER_LIMIT(max_validation_scratch_bytes); HEADER_LIMIT(max_validation_work);
#undef HEADER_LIMIT
#define HEADER_USAGE(member) HEADER_VALUE(o->usage.member)
    HEADER_USAGE(capabilities); HEADER_USAGE(entry_roots); HEADER_USAGE(operations); HEADER_USAGE(arguments); HEADER_USAGE(requirements); HEADER_USAGE(grants);
    HEADER_USAGE(formals); HEADER_USAGE(shapes); HEADER_USAGE(shape_cases); HEADER_USAGE(owned_bytes); HEADER_USAGE(build_scratch_bytes); HEADER_USAGE(build_work); HEADER_USAGE(validation_scratch_bytes); HEADER_USAGE(validation_work);
#undef HEADER_USAGE
#undef HEADER_VALUE
    return state;
}
static uint64_t owner_seal(const SolMirRuntimeHostAbi *o) {
    uint64_t state = header_authentication(o);
#define FIELD(value) state = header_mix(state, (uint64_t)(value))
    for (size_t i=0;i<o->capability_count;i++) { const SolMirRuntimeHostCapabilityPlan *x=&o->capabilities[i];
        FIELD(x->recipe); FIELD(x->source); FIELD(x->source_construct); FIELD(x->result); FIELD(x->temporary); FIELD(x->source_value); FIELD(x->source_temporary); FIELD(x->source_place); FIELD(x->source_root); FIELD(x->parent); FIELD(x->private_recipe); FIELD(x->private_value); }
    for (size_t i=0;i<o->entry_root_count;i++) { const SolMirRuntimeHostEntryRoot *x=&o->entry_roots[i]; FIELD(x->entry); FIELD(x->formal); FIELD(x->recipe); FIELD(x->signature); }
    for (size_t i=0;i<o->operation_count;i++) { const SolMirRuntimeHostOperation *x=&o->operations[i];
        FIELD(x->host); FIELD(x->import_id); FIELD(x->signature); FIELD(x->receiver); FIELD(x->receiver_access); FIELD(x->formals.offset); FIELD(x->formals.count); FIELD(x->result); FIELD(x->result_class); FIELD(x->result_plan); FIELD(x->effects); state=seal_digest(state,&x->identity); state=seal_symbol(state,&x->symbol); FIELD(x->failure_mask); }
    for (size_t i=0;i<o->argument_count;i++) { const SolMirRuntimeHostArgument *x=&o->arguments[i]; FIELD(x->recipe); FIELD(x->kind); FIELD(x->access); FIELD(x->children.offset); FIELD(x->children.count); }
    for (size_t i=0;i<o->formal_count;i++) { const SolMirRuntimeHostFormal *x=&o->formals[i]; FIELD(x->recipe); FIELD(x->access); FIELD(x->shape); }
    for (size_t i=0;i<o->shape_count;i++) { const SolMirRuntimeHostShape *x=&o->shapes[i]; FIELD(x->recipe); FIELD(x->kind); FIELD(x->access); FIELD(x->cases.offset); FIELD(x->cases.count); }
    for (size_t i=0;i<o->shape_case_count;i++) { const SolMirRuntimeHostShapeCase *x=&o->shape_cases[i]; FIELD(x->parent); FIELD(x->ordinal); FIELD(x->child); }
    for (size_t i=0;i<o->requirement_count;i++) { const SolMirRuntimeHostRequirement *x=&o->requirements[i]; FIELD(x->entry); FIELD(x->root); FIELD(x->operation); FIELD(x->call); FIELD(x->event); FIELD(x->failure_transition); }
#undef FIELD
    return state ? state : UINT64_C(1);
}
static bool header_range(const void *pointer,size_t count,size_t size) {
    return !count ? pointer==NULL : pointer!=NULL && count<=SIZE_MAX/size
        && (uintptr_t)pointer<=UINTPTR_MAX-count*size;
}
static bool authenticated_header(const SolMirRuntimeHostAbi *o) {
    if (!o || !o->authentication || !full(o->limits) || !o->conventions || !o->values || !o->cleanup
        || o->capability_count!=o->capability_capacity || o->entry_root_count!=o->entry_root_capacity
        || o->operation_count!=o->operation_capacity || o->argument_count!=o->argument_capacity
        || o->formal_count!=o->formal_capacity || o->shape_count!=o->shape_capacity
        || o->shape_case_count!=o->shape_case_capacity || o->requirement_count!=o->requirement_capacity
        || !header_range(o->capabilities,o->capability_count,sizeof(*o->capabilities))
        || !header_range(o->entry_roots,o->entry_root_count,sizeof(*o->entry_roots))
        || !header_range(o->operations,o->operation_count,sizeof(*o->operations))
        || !header_range(o->arguments,o->argument_count,sizeof(*o->arguments))
        || !header_range(o->formals,o->formal_count,sizeof(*o->formals))
        || !header_range(o->shapes,o->shape_count,sizeof(*o->shapes))
        || !header_range(o->shape_cases,o->shape_case_count,sizeof(*o->shape_cases))
        || !header_range(o->requirements,o->requirement_count,sizeof(*o->requirements))
        || !fit(o) || o->authentication!=owner_seal(o)) return false;
    /* The seal has now authenticated the borrowed predecessor addresses as
     * header data, so following their owner links cannot be induced by an
     * unsealed header mutation. */
    return o->values->conventions==o->conventions
        && o->cleanup->conventions==o->conventions && o->cleanup->values==o->values;
}
static bool calculate_usage_census(SolMirRuntimeHostAbi *o, size_t grants,
    size_t build_scratch, size_t validation_scratch, size_t build_work) {
    size_t validation_work=0, comparisons=0;
    o->usage.capabilities=o->capability_count;
    o->usage.entry_roots=o->entry_root_count;
    o->usage.operations=o->operation_count;
    o->usage.arguments=o->argument_count;
    o->usage.formals=o->formal_count;
    o->usage.shapes=o->shape_count;
    o->usage.shape_cases=o->shape_case_count;
    o->usage.requirements=o->requirement_count;
    o->usage.grants=grants;
    if(o->requirement_count>1
        &&(!mul(o->requirement_count,o->requirement_count-1,&comparisons)))return false;
    comparisons/=2;
    if(!usage_bytes(o)
        ||!add(&validation_work,o->capability_count)||!add(&validation_work,o->entry_root_count)
        ||!add(&validation_work,o->operation_count)||!add(&validation_work,o->argument_count)
        ||!add(&validation_work,o->formal_count)||!add(&validation_work,o->shape_count)
        ||!add(&validation_work,o->shape_case_count)||!add(&validation_work,o->requirement_count)
        /* The independent reachability/provenance replay charges one exact
           host requirement visit in addition to the owner-record check. */
        ||!add(&validation_work,o->requirement_count)
        ||!add(&validation_work,grants)||!add(&validation_work,comparisons))return false;
    o->usage.build_scratch_bytes=build_scratch;
    o->usage.validation_scratch_bytes=validation_scratch;
    o->usage.build_work=build_work;
    o->usage.validation_work=validation_work;
    return true;
}
SolMirRuntimeHostAbiBuildOutcome sol_mir_runtime_host_abi_build(const SolMirRuntimeHostAbiBuildRequest *request,SolMirRuntimeHostAbi *out,SolDiagnostics*d){
    if(!request||!out||!request->conventions||!request->values||!request->cleanup||!empty(out)||(request->limits&&!zero(*request->limits)&&!full(*request->limits))){diagnostic(d,"invalid runtime host ABI build request or destination");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_ARGUMENT;}
    if(request->values->conventions!=request->conventions||request->cleanup->conventions!=request->conventions||request->cleanup->values!=request->values||!sol_mir_runtime_conventions_validate(request->conventions,d)||!sol_mir_runtime_values_validate(request->values,d)||!sol_mir_runtime_cleanup_validate(request->cleanup,d)){diagnostic(d,"runtime host ABI predecessors are invalid");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    SolMirRuntimeHostAbi x;sol_mir_runtime_host_abi_init(&x);x.conventions=request->conventions;x.values=request->values;x.cleanup=request->cleanup;x.limits=!request->limits||zero(*request->limits)?sol_mir_runtime_host_abi_default_limits():*request->limits;
    SolMirRuntimeHostAbiWorkMeter census_work={x.limits.max_build_work,0};
    SolMirRuntimeHostAbiWorkMeter dry_write_work={0,0};
    SolMirRuntimeHostAbiWorkMeter write_work={0,0};
    const SolMirRepresentation*r=&x.conventions->concrete->representation;const SolMirLinkage*l=&x.conventions->concrete->linkage;const SolMirOperations*ops=&x.conventions->concrete->operations;const SolMirMaterialization*m=&x.conventions->concrete->materialization;
    size_t roots=0,caps=0,formals=0,shapes=0,cases=0;
    size_t root_bits=0,grant_bits=0,root_bytes=0,shape_bytes=0,validation_shape=0,
        validation_roots=0,workspace_bytes=0,validation_bytes=0;
    unsigned char *workspace=NULL,*active=NULL,*images=NULL,*root_state=NULL;
    for(size_t e=0;e<x.conventions->entry_count;e++){
        if(!sol_mir_runtime_host_abi_work_tick(&census_work))goto exhausted;
        const SolMirRuntimeSignature*s=&x.conventions->signatures[x.conventions->entries[e].signature];
        for(size_t q=0;q<s->slots.count;q++){
            if(!sol_mir_runtime_host_abi_work_tick(&census_work))goto exhausted;
            const SolMirRuntimeSignatureSlot*slot=&x.conventions->signature_slots[s->slots.offset+q];
            if(slot->role==SOL_MIR_RUNTIME_SLOT_PARAMETER&&slot->recipe<r->recipe_count
                &&r->recipes[slot->recipe].kind==SOL_MIR_RECIPE_CAPABILITY
                &&(!add(&roots,1)||!add(&caps,1)))goto exhausted;
        }
    }
    for(size_t i=0;i<ops->constructor_count;i++){
        if(!sol_mir_runtime_host_abi_work_tick(&census_work))goto exhausted;
        if(ops->constructors[i].kind!=SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY)continue;
        SolMirMaterializedTemporaryId temporary;SolMirMaterializedValueId value;SolMirMaterializedPlaceId place;
        if(!result_temporary(m,ops->constructors[i].result,&temporary,&census_work))temporary=SOL_MIR_RUNTIME_NONE;
        if(ops->constructors[i].capability_source_operand>=ops->constructors[i].operands.count
            ||!temporary_coordinate(m,ops->construct_operands[ops->constructors[i].operands.offset+ops->constructors[i].capability_source_operand].temporary,&value,&place,&census_work)||!add(&caps,1))goto exhausted;
    }
    /* One allocation-free census precedes all persistent owner allocation.
     * The workspace holds either shape recursion state or the complete call
     * closure/provenance state, never both at once. */
    if(!mul(roots,m->local_count,&root_bits)
        ||!mul(roots,l->host_requirement_count,&grant_bits)
        ||!mul(roots,sizeof(SolMirRuntimeHostEntryRoot),&root_bytes)
        ||!add(&root_bytes,m->image_count)||!add(&root_bytes,root_bits)
        ||!add(&root_bytes,grant_bits))goto exhausted;
    shape_bytes=r->recipe_count?r->recipe_count:1;
    workspace_bytes=shape_bytes>root_bytes?shape_bytes:root_bytes;
    if (!frozen_e6_surface(&x,&census_work)) {
        if (census_work.used>=census_work.limit) goto exhausted;
        goto unsupported;
    }
    for(size_t h=0;h<l->host_requirement_count;h++){
        const SolMirLinkageHostRequirement*host=&l->host_requirements[h];
        if(!e3_host_requirement(&x,h,&census_work)){if(census_work.used>=census_work.limit)goto exhausted;goto unsupported;}
        if(!find_import(x.conventions,h,NULL,&census_work)
            ||!find_signature(x.conventions,h,NULL,&census_work))goto unsupported;
        for(size_t q=0;q<host->parameters.count;q++)
            if(!sol_mir_runtime_host_abi_work_tick(&census_work)||!add(&formals,1)||!count_shape(r,m->type_ids[host->parameters.offset+q],0,&shapes,&cases,&census_work)){if(census_work.used>=census_work.limit)goto exhausted;goto unsupported;}
    }
    if(!add(&validation_shape,r->recipe_count)||!add(&validation_shape,shapes)
        ||!add(&validation_roots,m->image_count)||!add(&validation_roots,root_bits))goto exhausted;
    validation_bytes=validation_shape>validation_roots?validation_shape:validation_roots;
    workspace=(unsigned char*)scratch_allocate(workspace_bytes);
    if(!workspace)goto allocation;
    memset(workspace,0,workspace_bytes);
    SolMirRuntimeHostAbi pre=x;
    pre.entry_roots=(SolMirRuntimeHostEntryRoot*)workspace;
    pre.entry_root_count=roots;
    images=workspace+roots*sizeof(*pre.entry_roots);
    root_state=images+m->image_count;
    size_t root_index=0;
    for(size_t e=0;e<pre.conventions->entry_count;e++){
        if(!sol_mir_runtime_host_abi_work_tick(&census_work))goto exhausted;
        const SolMirRuntimeEntry*entry=&pre.conventions->entries[e];
        const SolMirRuntimeSignature*s=&pre.conventions->signatures[entry->signature];
        for(size_t q=0;q<s->slots.count;q++){
            if(!sol_mir_runtime_host_abi_work_tick(&census_work))goto exhausted;
            const SolMirRuntimeSignatureSlot*slot=&pre.conventions->signature_slots[s->slots.offset+q];
            if(slot->role==SOL_MIR_RUNTIME_SLOT_PARAMETER&&slot->recipe<r->recipe_count
                &&r->recipes[slot->recipe].kind==SOL_MIR_RECIPE_CAPABILITY)
                pre.entry_roots[root_index++]=(SolMirRuntimeHostEntryRoot){e,slot->formal,slot->recipe,entry->signature};
        }
    }
    unsigned char *grant_state=root_state+root_bits;
    size_t requirements=0,grants=0;
    for(size_t e=0;e<pre.conventions->entry_count;e++){
        if(!sol_mir_runtime_host_abi_work_tick(&census_work))goto exhausted;
        memset(images,0,m->image_count);memset(root_state,0,root_bits);
        if(!mark_entry_images(&pre,e,images,&census_work)||!closure_roots(&pre,e,images,root_state,&census_work)){if(census_work.used>=census_work.limit)goto exhausted;goto unsupported;}
        for(size_t c=0;c<pre.conventions->call_count;c++){
            if(!sol_mir_runtime_host_abi_work_tick(&census_work))goto exhausted;
            const SolMirRuntimeCall*call=&pre.conventions->calls[c];
            size_t host=target_host(&pre,call);const SolMirRuntimeOperand*receiver=receiver_operand(&pre,call);
            if(!call_owner_reachable(&pre,e,images,call)||host==SOL_MIR_RUNTIME_NONE||!receiver)continue;
            size_t receiver_root=receiver_origin(&pre,e,root_state,receiver,&census_work);
            if(receiver_root==SOL_MIR_RUNTIME_NONE)goto unsupported;
            size_t event,transition;
            if(!call_cleanup(&pre,c,&event,&transition,&census_work))goto unsupported;
            if(!add(&requirements,1))goto exhausted;
            if(!grant_state[receiver_root*l->host_requirement_count+host]){
                grant_state[receiver_root*l->host_requirement_count+host]=1;
                if(!add(&grants,1))goto exhausted;
            }
        }
    }
    /* Predict every persistent-owner traversal before its allocation using
     * only the budget left by the already performed census. */
    dry_write_work.limit=x.limits.max_build_work-census_work.used;
    if(!persistent_write_traversal(&pre,&dry_write_work,images,root_state,root_bits)) {
        if(dry_write_work.used>=dry_write_work.limit) goto exhausted;
        goto unsupported;
    }
    /* The dry traversal is a complete pre-allocation check.  Reserve its
     * already-executed work, but leave the emitting pass to consume its own
     * meter.  Thus an exact-minus-one limit fails on the final emitting tick,
     * after which the transactional owner is still unpublished. */
    if (census_work.used > x.limits.max_build_work
        || dry_write_work.used > x.limits.max_build_work-census_work.used)
        goto exhausted;
    x.capability_capacity=caps;x.entry_root_capacity=roots;x.operation_capacity=l->host_requirement_count;x.argument_capacity=formals;x.formal_capacity=formals;x.shape_capacity=shapes;x.shape_case_capacity=cases;x.requirement_capacity=requirements;
    x.capability_count=caps;x.entry_root_count=roots;x.operation_count=l->host_requirement_count;x.argument_count=formals;x.formal_count=formals;x.shape_count=shapes;x.shape_case_count=cases;x.requirement_count=requirements;
    if(!calculate_usage_census(&x,grants,workspace_bytes,validation_bytes,0)||!fit(&x))goto exhausted;
    /* The physical write receives only the budget not consumed by census and
     * prediction.  A failed tick remains transactional and unpublished. */
    write_work.limit=x.limits.max_build_work-census_work.used-dry_write_work.used;
    x.capability_count=x.entry_root_count=x.operation_count=x.argument_count=x.formal_count=x.shape_count=x.shape_case_count=x.requirement_count=0;
    x.capabilities=allocate(caps,sizeof(*x.capabilities));x.entry_roots=allocate(roots,sizeof(*x.entry_roots));x.operations=allocate(l->host_requirement_count,sizeof(*x.operations));x.arguments=allocate(formals,sizeof(*x.arguments));x.formals=allocate(formals,sizeof(*x.formals));x.shapes=allocate(shapes,sizeof(*x.shapes));x.shape_cases=allocate(cases,sizeof(*x.shape_cases));x.requirements=allocate(requirements,sizeof(*x.requirements));
    if((caps&&!x.capabilities)||(roots&&!x.entry_roots)||(l->host_requirement_count&&!x.operations)||(formals&&!x.arguments)||(formals&&!x.formals)||(shapes&&!x.shapes)||(cases&&!x.shape_cases)||(requirements&&!x.requirements))goto allocation;
    memset(workspace,0,workspace_bytes);
    images=workspace+roots*sizeof(*pre.entry_roots); root_state=images+m->image_count;
    for(size_t e=0;e<x.conventions->entry_count;e++){if(!sol_mir_runtime_host_abi_work_tick(&write_work))goto exhausted;const SolMirRuntimeEntry*entry=&x.conventions->entries[e];const SolMirRuntimeSignature*s=&x.conventions->signatures[entry->signature];for(size_t q=0;q<s->slots.count;q++){if(!sol_mir_runtime_host_abi_work_tick(&write_work))goto exhausted;const SolMirRuntimeSignatureSlot*slot=&x.conventions->signature_slots[s->slots.offset+q];if(slot->role==SOL_MIR_RUNTIME_SLOT_PARAMETER&&slot->recipe<r->recipe_count&&r->recipes[slot->recipe].kind==SOL_MIR_RECIPE_CAPABILITY){size_t root=x.entry_root_count;x.entry_roots[x.entry_root_count++]=(SolMirRuntimeHostEntryRoot){e,slot->formal,slot->recipe,entry->signature};x.capabilities[x.capability_count++]=(SolMirRuntimeHostCapabilityPlan){
                    slot->recipe,SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT,SOL_MIR_RUNTIME_NONE,
                    SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE,
                    SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE,root,SOL_MIR_RUNTIME_NONE,
                    SOL_MIR_RECIPE_NONE,SOL_MIR_RUNTIME_NONE};(void)root;}}}
    for(size_t i=0;i<ops->constructor_count;i++){
        if(!sol_mir_runtime_host_abi_work_tick(&write_work))goto exhausted;
        const SolMirOperationConstructPlan*p=&ops->constructors[i];
        if(p->kind!=SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY)continue;
        if(p->capability_source_operand>=p->operands.count)goto unsupported;
        const SolMirOperationConstructOperand *operand=
            &ops->construct_operands[p->operands.offset+p->capability_source_operand];
        SolMirMaterializedTemporaryId temporary;
        SolMirMaterializedValueId source_value;
        SolMirMaterializedPlaceId source_place;
        if(!result_temporary(m,p->result,&temporary,&write_work)) temporary=SOL_MIR_RUNTIME_NONE;
        if(!temporary_coordinate(m,operand->temporary,&source_value,&source_place,
                &write_work)) goto unsupported;
        SolMirRuntimeHostCapabilitySource source=p->capability_rule==SOL_MIR_OPERATION_CAPABILITY_PRIVATE_SOURCE
            ?SOL_MIR_RUNTIME_HOST_CAPABILITY_PRIVATE_SOURCE
            :p->capability_rule==SOL_MIR_OPERATION_CAPABILITY_BASE_SOURCE
                ?SOL_MIR_RUNTIME_HOST_CAPABILITY_DERIVED:SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT;
        size_t root=SOL_MIR_RUNTIME_NONE,parent=SOL_MIR_RUNTIME_NONE;
        if(source==SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT){
            root=entry_root_for_local(&x,p->inherited_root,&write_work);
            if(root==SOL_MIR_RUNTIME_NONE || source_place>=m->place_count
                ||m->places[source_place].local!=p->inherited_root
                ||m->places[source_place].projections.count) goto unsupported;
            parent=root;
        } else if(source==SOL_MIR_RUNTIME_HOST_CAPABILITY_DERIVED) {
            for(size_t q=0;q<x.capability_count;q++) {
                if(!sol_mir_runtime_host_abi_work_tick(&write_work))goto exhausted;
                if(x.capabilities[q].temporary==operand->temporary){
                if(parent!=SOL_MIR_RUNTIME_NONE)goto unsupported; parent=q;
                }
            }
            if(parent==SOL_MIR_RUNTIME_NONE&&source_place<m->place_count
                &&!m->places[source_place].projections.count)
                parent=entry_root_for_local(&x,m->places[source_place].local,&write_work);
            if(parent==SOL_MIR_RUNTIME_NONE)goto unsupported;
            root=x.capabilities[parent].source_root;
            if(root==SOL_MIR_RUNTIME_NONE)goto unsupported;
        }
        x.capabilities[x.capability_count++]=(SolMirRuntimeHostCapabilityPlan){
            p->result_recipe,source,i,p->result,temporary,source_value,operand->temporary,
            source_place,root,parent,source==SOL_MIR_RUNTIME_HOST_CAPABILITY_PRIVATE_SOURCE
                ?operand->recipe:SOL_MIR_RECIPE_NONE,
            source==SOL_MIR_RUNTIME_HOST_CAPABILITY_PRIVATE_SOURCE?source_value:SOL_MIR_RUNTIME_NONE};
    }
    memset(workspace,0,workspace_bytes);active=workspace;
    for(size_t h=0;h<l->host_requirement_count;h++){if(!e3_host_requirement(&x,h,&write_work)){if(write_work.used>=write_work.limit)goto exhausted;goto unsupported;}const SolMirLinkageHostRequirement*host=&l->host_requirements[h];size_t import,signature;const SolMirRuntimeImport*im=find_import(x.conventions,h,&import,&write_work);const SolMirRuntimeSignature*sig=find_signature(x.conventions,h,&signature,&write_work);if(!im||!sig||host->result>=x.values->host_result_plan_count||x.values->host_result_plans[host->result].classification==SOL_MIR_RUNTIME_HOST_RESULT_FORBIDDEN||x.values->host_result_plans[host->result].classification==SOL_MIR_RUNTIME_HOST_RESULT_UNREACHABLE)goto unsupported;SolMirRuntimeHostOperation*op=&x.operations[x.operation_count++];*op=(SolMirRuntimeHostOperation){h,import,signature,host->receiver,host->receiver_access,{x.formal_count,0},host->result,x.values->host_result_plans[host->result].classification,host->result,host->effects,im->identity,im->symbol,HOST_FAILURE_MASK};for(size_t q=0;q<host->parameters.count;q++){SolMirRecipeId recipe=m->type_ids[host->parameters.offset+q];SolAccessMode access=m->accesses[host->parameter_accesses.offset+q];SolMirRuntimeHostArgumentKind kind;size_t shape;if(!sol_mir_runtime_host_abi_work_tick(&write_work)||!shape_kind(&r->recipes[recipe],&kind)||!append_shape(&x,recipe,access,active,&shape,&write_work)||x.argument_count>=x.argument_capacity)goto exhausted;x.arguments[x.argument_count++]=(SolMirRuntimeHostArgument){recipe,kind,access,{0,0}};x.formals[x.formal_count++]=(SolMirRuntimeHostFormal){recipe,access,shape};}op->formals.count=x.formal_count-op->formals.offset;}
    memset(workspace,0,workspace_bytes);
    images=workspace+roots*sizeof(*x.entry_roots);root_state=images+m->image_count;
    for(size_t e=0;e<x.conventions->entry_count;e++){if(!sol_mir_runtime_host_abi_work_tick(&write_work))goto exhausted;memset(images,0,m->image_count);memset(root_state,0,root_bits);if(!mark_entry_images(&x,e,images,&write_work)||!closure_roots(&x,e,images,root_state,&write_work)){if(write_work.used>=write_work.limit)goto exhausted;goto unsupported;}for(size_t c=0;c<x.conventions->call_count;c++){const SolMirRuntimeCall*call=&x.conventions->calls[c];if(!sol_mir_runtime_host_abi_work_tick(&write_work))goto exhausted;size_t host=target_host(&x,call);if(!call_owner_reachable(&x,e,images,call)||host==SOL_MIR_RUNTIME_NONE||host>=x.operation_count)continue;const SolMirRuntimeOperand*receiver=receiver_operand(&x,call);size_t root=receiver_origin(&x,e,root_state,receiver,&write_work);if(root==SOL_MIR_RUNTIME_NONE)goto unsupported;size_t event,transition;if(!call_cleanup(&x,c,&event,&transition,&write_work))goto unsupported;if(x.requirement_count>=x.requirement_capacity)goto exhausted;x.requirements[x.requirement_count++]=(SolMirRuntimeHostRequirement){e,root,host,c,event,transition};}}
    free(workspace);workspace=NULL;
    if(x.requirement_count!=x.requirement_capacity||x.argument_count!=x.argument_capacity||!calculate_usage_census(&x,grants,workspace_bytes,validation_bytes,census_work.used+dry_write_work.used+write_work.used)||!sol_mir_runtime_host_abi_internal_measure_validation_work(&x,&x.usage.validation_work)||!fit(&x))goto exhausted;
    if(sol_mir_runtime_host_abi_internal_validate(&x,d)!=SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED){sol_mir_runtime_host_abi_free(&x);return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}x.authentication=owner_seal(&x);
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    sol_mir_runtime_host_abi_internal_work_census.census_work=census_work.used;
    sol_mir_runtime_host_abi_internal_work_census.dry_work=dry_write_work.used;
    sol_mir_runtime_host_abi_internal_work_census.actual_work=write_work.used;
    sol_mir_runtime_host_abi_internal_work_census.validation_work=x.usage.validation_work;
#endif
    *out=x;return SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED;
    unsupported:if(census_work.used>=census_work.limit
            ||(dry_write_work.limit&&dry_write_work.used>=dry_write_work.limit)
            ||(write_work.limit&&write_work.used>=write_work.limit))goto exhausted;
        free(workspace);sol_mir_runtime_host_abi_free(&x);diagnostic(d,"host ABI requirement is outside the E3 closure");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_UNSUPPORTED;
    exhausted:free(workspace);sol_mir_runtime_host_abi_free(&x);diagnostic(d,"runtime host ABI resource limit exceeded");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;
    allocation:free(workspace);sol_mir_runtime_host_abi_free(&x);if(d)d->allocation_failed=true;diagnostic(d,"runtime host ABI allocation failed");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_ALLOCATION_FAILED;
}

static bool preflight_configuration(const SolMirRuntimeHostAbi *o,const SolMirRuntimeHostPreflightRequest*p){if(!o||!p||p->entry>=o->conventions->entry_count||(p->root_count&&!p->roots)||(p->grant_count&&!p->grants))return false;for(size_t i=0;i<p->root_count;i++){if(p->roots[i].root>=o->entry_root_count||o->entry_roots[p->roots[i].root].entry!=p->entry||!p->roots[i].opaque)return false;for(size_t q=0;q<i;q++)if(p->roots[q].root==p->roots[i].root||p->roots[q].opaque==p->roots[i].opaque)return false;}for(size_t root=0;root<o->entry_root_count;root++)if(o->entry_roots[root].entry==p->entry){bool seen=false;for(size_t i=0;i<p->root_count;i++)seen|=p->roots[i].root==root;if(!seen)return false;}for(size_t i=0;i<p->grant_count;i++){if(p->grants[i].root>=o->entry_root_count||p->grants[i].operation>=o->operation_count||o->entry_roots[p->grants[i].root].entry!=p->entry)return false;bool required=false;for(size_t q=0;q<o->requirement_count;q++)required|=o->requirements[q].entry==p->entry&&o->requirements[q].root==p->grants[i].root&&o->requirements[q].operation==p->grants[i].operation;if(!required)return false;for(size_t q=0;q<i;q++)if(p->grants[q].root==p->grants[i].root&&p->grants[q].operation==p->grants[i].operation)return false;}for(size_t q=0;q<o->requirement_count;q++)if(o->requirements[q].entry==p->entry){bool seen=false;for(size_t i=0;i<p->grant_count;i++)seen|=p->grants[i].root==o->requirements[q].root&&p->grants[i].operation==o->requirements[q].operation;if(!seen)return false;}return true;}
/* Authenticate before dereferencing any owner or predecessor record.  The
 * configuration pass after authentication is allocation-free. */
SolMirRuntimeHostPreflightOutcome sol_mir_runtime_host_abi_preflight(const SolMirRuntimeHostAbi*o,const SolMirRuntimeHostPreflightRequest*p){
    if (!authenticated_header(o))
        return SOL_MIR_RUNTIME_HOST_PREFLIGHT_INVALID;
    return preflight_configuration(o,p)
        ? SOL_MIR_RUNTIME_HOST_PREFLIGHT_SUCCEEDED
        : SOL_MIR_RUNTIME_HOST_PREFLIGHT_INVALID;
}
typedef struct { char text[224]; } HostRenderLine;
static void host_render_digest(char *out,const SolMirLinkageDigest *digest){for(size_t i=0;i<SOL_MIR_LINKAGE_DIGEST_BYTES;i++)(void)snprintf(out+2*i,3,"%02x",digest->bytes[i]);}
static void key_u64(SolMirLinkageSha256 *state,uint64_t value){uint8_t bytes[8];for(size_t i=0;i<sizeof(bytes);i++)bytes[i]=(uint8_t)(value>>(8*i));sol_mir_linkage_internal_sha256_write(state,bytes,sizeof(bytes));}
static void key_bytes(SolMirLinkageSha256 *state,const void *bytes,size_t length){key_u64(state,length);if(length)sol_mir_linkage_internal_sha256_write(state,bytes,length);}
static void key_text(SolMirLinkageSha256 *state,const char *text){key_bytes(state,text,strlen(text));}
static void key_digest(SolMirLinkageSha256 *state,const SolMirLinkageDigest *digest){key_bytes(state,digest->bytes,sizeof(digest->bytes));}
static bool key_effects(SolMirLinkageSha256 *state, const SolMirRuntimeHostAbi *o,
    SolMirMaterializedEffectRowId effects) {
    const SolMirMaterialization *m=&o->conventions->concrete->materialization;
    if (effects==SOL_MIR_RUNTIME_NONE) { key_u64(state,0); return true; }
    if (effects>=m->effect_row_count) return false;
    const SolMirMaterializedEffectRow *row=&m->effect_rows[effects];
    if (row->atoms.offset>m->effect_row_atom_count || row->atoms.count>m->effect_row_atom_count-row->atoms.offset) return false;
    key_u64(state,1); key_u64(state,row->atoms.count);
    for (size_t i=0;i<row->atoms.count;i++) {
        size_t atom_id=m->effect_row_atoms[row->atoms.offset+i];
        if (atom_id>=m->effect_atom_count) return false;
        const SolMirMaterializedEffectAtom *atom=&m->effect_atoms[atom_id];
        if (atom->name.offset>m->effect_name_count || atom->name.count>m->effect_name_count-atom->name.offset) return false;
        key_u64(state,atom->authority); key_bytes(state,m->effect_names+atom->name.offset,atom->name.count);
    }
    return true;
}
static bool recipe_key_depth(const SolMirRuntimeHostAbi *o, SolMirRecipeId recipe,
    size_t depth, SolMirLinkageDigest *out) {
    const SolMirRepresentation *r=&o->conventions->concrete->representation;
    const SolMirLinkage *l=&o->conventions->concrete->linkage;
    if (recipe>=r->recipe_count || depth>r->recipe_count) return false;
    const SolMirRecipe *x=&r->recipes[recipe];
    SolMirLinkageSha256 state; sol_mir_linkage_internal_sha256_init(&state);
    key_text(&state,"host-abi-recipe/v2");
    key_u64(&state,x->kind); key_u64(&state,x->storage); key_u64(&state,x->copy_kind); key_u64(&state,x->drop_kind);
    key_u64(&state,x->inhabited); key_u64(&state,x->zero_sized); key_u64(&state,x->is_copy);
    bool requirement=false;
    for (size_t i=0;i<l->runtime_requirement_count;i++) if (l->runtime_requirements[i].recipe==recipe) {
        if (requirement) return false;
        requirement=true; key_u64(&state,1); key_digest(&state,&l->runtime_requirements[i].recipe_key);
    }
    if (!requirement) key_u64(&state,0);
    if (x->fields.offset>r->field_count || x->fields.count>r->field_count-x->fields.offset
        || x->variants.offset>r->variant_count || x->variants.count>r->variant_count-x->variants.offset
        || x->parameters.offset>r->recipe_id_count || x->parameters.count>r->recipe_id_count-x->parameters.offset
        || x->parameter_accesses.offset>r->access_count || x->parameter_accesses.count>r->access_count-x->parameter_accesses.offset) return false;
    key_u64(&state,x->fields.count);
    for (size_t i=0;i<x->fields.count;i++) { const SolMirRecipeField *f=&r->fields[x->fields.offset+i]; SolMirLinkageDigest child;
        key_u64(&state,f->ordinal); if(!recipe_key_depth(o,f->type,depth+1,&child))return false; key_digest(&state,&child); }
    key_u64(&state,x->variants.count);
    for (size_t i=0;i<x->variants.count;i++) { const SolMirRecipeVariant *v=&r->variants[x->variants.offset+i];
        if (v->fields.offset>r->field_count || v->fields.count>r->field_count-v->fields.offset) return false;
        key_u64(&state,v->ordinal); key_u64(&state,v->semantic_tag); key_u64(&state,v->fields.count);
        for(size_t q=0;q<v->fields.count;q++){ const SolMirRecipeField *f=&r->fields[v->fields.offset+q]; SolMirLinkageDigest child;
            key_u64(&state,f->ordinal); if(!recipe_key_depth(o,f->type,depth+1,&child))return false; key_digest(&state,&child); }
    }
    key_u64(&state,x->parameters.count);
    for(size_t i=0;i<x->parameters.count;i++){ SolMirLinkageDigest child;
        if(!recipe_key_depth(o,r->recipe_ids[x->parameters.offset+i],depth+1,&child))return false;
        key_digest(&state,&child); key_u64(&state,r->accesses[x->parameter_accesses.offset+i]); }
#define OPTIONAL_RECIPE(value) do { if ((value)==SOL_MIR_RECIPE_NONE) key_u64(&state,0); else { SolMirLinkageDigest child; key_u64(&state,1); if(!recipe_key_depth(o,(value),depth+1,&child))return false; key_digest(&state,&child); } } while(0)
    OPTIONAL_RECIPE(x->result); OPTIONAL_RECIPE(x->backing); OPTIONAL_RECIPE(x->capability_source);
#undef OPTIONAL_RECIPE
    if(!key_effects(&state,o,x->effects)) return false;
    return sol_mir_linkage_internal_sha256_finish(&state,out);
}
static bool recipe_key(const SolMirRuntimeHostAbi *o,SolMirRecipeId recipe,SolMirLinkageDigest *out){return recipe_key_depth(o,recipe,0,out);}
static bool key_recipe(SolMirLinkageSha256 *state,const SolMirRuntimeHostAbi *o,SolMirRecipeId recipe){SolMirLinkageDigest digest;if(!recipe_key(o,recipe,&digest))return false;key_digest(state,&digest);return true;}
static bool entry_key(const SolMirRuntimeHostAbi *o,size_t id,SolMirLinkageDigest *out){if(id>=o->entry_root_count)return false;const SolMirRuntimeHostEntryRoot*r=&o->entry_roots[id];if(r->entry>=o->conventions->entry_count)return false;SolMirLinkageSha256 state;sol_mir_linkage_internal_sha256_init(&state);key_text(&state,"host-abi-entry-root/v1");key_text(&state,o->conventions->entries[r->entry].symbol.bytes);key_u64(&state,r->formal);if(!key_recipe(&state,o,r->recipe))return false;return sol_mir_linkage_internal_sha256_finish(&state,out);}
static bool operation_key(const SolMirRuntimeHostAbi *o,size_t id,SolMirLinkageDigest *out){if(id>=o->operation_count)return false;const SolMirRuntimeHostOperation*x=&o->operations[id];const SolMirLinkage*l=&o->conventions->concrete->linkage;if(x->host>=l->host_requirement_count)return false;SolMirLinkageSha256 state;sol_mir_linkage_internal_sha256_init(&state);key_text(&state,"host-abi-operation/v1");key_digest(&state,&l->host_requirements[x->host].requirement_key);key_digest(&state,&x->identity);key_text(&state,x->symbol.bytes);key_u64(&state,x->receiver_access);key_u64(&state,x->result_class);key_u64(&state,x->failure_mask);if(!key_recipe(&state,o,x->receiver)||!key_recipe(&state,o,x->result))return false;for(size_t i=0;i<x->formals.count;i++){const SolMirRuntimeHostFormal*f=&o->formals[x->formals.offset+i];key_u64(&state,f->access);if(!key_recipe(&state,o,f->recipe))return false;}return sol_mir_linkage_internal_sha256_finish(&state,out);}
static bool shape_key(const SolMirRuntimeHostAbi *o,size_t id,SolMirLinkageDigest *out){if(id>=o->shape_count)return false;const SolMirRuntimeHostShape*x=&o->shapes[id];SolMirLinkageSha256 state;sol_mir_linkage_internal_sha256_init(&state);key_text(&state,"host-abi-shape/v1");key_u64(&state,x->kind);key_u64(&state,x->access);if(!key_recipe(&state,o,x->recipe))return false;for(size_t i=0;i<x->cases.count;i++){const SolMirRuntimeHostShapeCase*c=&o->shape_cases[x->cases.offset+i];key_u64(&state,c->ordinal);if(c->child!=SOL_MIR_RUNTIME_NONE){SolMirLinkageDigest child;if(!shape_key(o,c->child,&child))return false;key_digest(&state,&child);}}return sol_mir_linkage_internal_sha256_finish(&state,out);}
static bool argument_key(const SolMirRuntimeHostAbi *o,size_t id,SolMirLinkageDigest *out){
    if(id>=o->argument_count)return false;const SolMirRuntimeHostArgument*x=&o->arguments[id];
    SolMirLinkageSha256 state;sol_mir_linkage_internal_sha256_init(&state);key_text(&state,"host-abi-argument/v1");
    key_u64(&state,x->kind);key_u64(&state,x->access);key_u64(&state,x->children.offset);key_u64(&state,x->children.count);
    if(!key_recipe(&state,o,x->recipe))return false;return sol_mir_linkage_internal_sha256_finish(&state,out);
}
static bool capability_key(const SolMirRuntimeHostAbi *, size_t, SolMirLinkageDigest *);
static bool source_content_key(const SolIr *ir, SolSpan span,
    SolMirLinkageDigest *out) {
    if (!ir || !out || span.start>span.end || span.end>ir->source_length) return false;
    for (size_t file=0; file<ir->file_count; ++file) {
        const SolIrSourceFile *source=&ir->files[file];
        if (source->aggregate_start>source->aggregate_end
            || source->aggregate_end>ir->source_length
            || span.start<source->aggregate_start || span.end>source->aggregate_end)
            continue;
        SolMirLinkageSha256 sha; sol_mir_linkage_internal_sha256_init(&sha);
        sol_mir_linkage_internal_sha256_write(&sha,
            ir->source_bytes+source->aggregate_start,
            source->aggregate_end-source->aggregate_start);
        return sol_mir_linkage_internal_sha256_finish(&sha,out);
    }
    /* Synthetic materialized spans have no per-file provenance.  The aggregate
       content is still path-neutral and prevents an ID-based fallback. */
    if (!ir->file_count && (!ir->source_length || ir->source_bytes)) {
        SolMirLinkageSha256 sha; sol_mir_linkage_internal_sha256_init(&sha);
        if (ir->source_length) sol_mir_linkage_internal_sha256_write(&sha,
            ir->source_bytes,ir->source_length);
        return sol_mir_linkage_internal_sha256_finish(&sha,out);
    }
    return false;
}
/* A construction is an occurrence, not a recipe census.  Keep its key wholly
 * semantic and path-neutral: parent instance, source-content digest and span,
 * recipe/rule, and materialized source lineage are all stable; local arena IDs
 * and source paths are deliberately absent. */
static bool construction_site_key(const SolMirRuntimeHostAbi *o, size_t id,
    SolMirLinkageDigest *out) {
    if (id>=o->capability_count || !out) return false;
    const SolMirRuntimeHostCapabilityPlan *x=&o->capabilities[id];
    if (x->source_construct==SOL_MIR_RUNTIME_NONE) return false;
    const SolMirConcreteProgram *concrete=o->conventions->concrete;
    const SolMirOperations *ops=&concrete->operations;
    const SolMirMaterialization *m=&concrete->materialization;
    const SolMirLinkage *linkage=&concrete->linkage;
    const SolIr *ir=concrete->program.ir;
    if (!ir || x->source_construct>=ops->constructor_count) return false;
    const SolMirOperationConstructPlan *plan=&ops->constructors[x->source_construct];
    if (plan->kind!=SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY
        || plan->instruction>=m->instruction_count || plan->result>=m->value_count
        || x->source_value>=m->value_count) return false;
    const SolMirMaterializedInstruction *instruction=&m->instructions[plan->instruction];
    const SolMirMaterializedValue *result=&m->values[plan->result];
    const SolMirMaterializedValue *source=&m->values[x->source_value];
    SolMirLinkageDigest parent, content, source_content;
    if (!sol_mir_linkage_internal_instance_key(linkage,plan->image,&parent,NULL)
        || !source_content_key(ir,instruction->span,&content)
        || !source_content_key(ir,source->span,&source_content)) return false;
    SolMirLinkageSha256 state; sol_mir_linkage_internal_sha256_init(&state);
    key_text(&state,"host-abi-construction-site/v2");
    key_digest(&state,&parent); key_digest(&state,&content);
    key_u64(&state,instruction->span.start); key_u64(&state,instruction->span.end);
    /* Result span is separate source lineage when a lowering context wraps the
       construction instruction. */
    key_u64(&state,result->span.start); key_u64(&state,result->span.end);
    key_u64(&state,plan->capability_rule);
    if (!key_recipe(&state,o,plan->result_recipe)) return false;
    key_digest(&state,&source_content);
    key_u64(&state,source->span.start); key_u64(&state,source->span.end);
    if (source->source_definition!=SOL_IR_NONE) {
        if (source->source_definition>=ir->definition_count) return false;
        const SolSemanticId semantic=ir->definitions[source->source_definition].semantic_id;
        key_u64(&state,semantic.high); key_u64(&state,semantic.low);
    } else key_text(&state,"source-definition:none");
    if (x->parent!=SOL_MIR_RUNTIME_NONE) {
        SolMirLinkageDigest lineage;
        if (x->parent>=id || !capability_key(o,x->parent,&lineage)) return false;
        key_digest(&state,&lineage);
    } else if (x->source_root!=SOL_MIR_RUNTIME_NONE) {
        SolMirLinkageDigest root;
        if (!entry_key(o,x->source_root,&root)) return false;
        key_digest(&state,&root);
    } else if (x->private_recipe!=SOL_MIR_RECIPE_NONE) {
        if (!key_recipe(&state,o,x->private_recipe)) return false;
    } else return false;
    return sol_mir_linkage_internal_sha256_finish(&state,out);
}
static bool capability_key(const SolMirRuntimeHostAbi *o,size_t id,SolMirLinkageDigest *out){
    if(id>=o->capability_count)return false;
    const SolMirRuntimeHostCapabilityPlan*x=&o->capabilities[id];
    SolMirLinkageSha256 state;sol_mir_linkage_internal_sha256_init(&state);
    key_text(&state,"host-abi-capability/v2");key_u64(&state,x->source);
    if(!key_recipe(&state,o,x->recipe))return false;
    if(x->source_construct!=SOL_MIR_RUNTIME_NONE){
        SolMirLinkageDigest site;if(!construction_site_key(o,id,&site))return false;key_digest(&state,&site);
    } else if(x->source_root!=SOL_MIR_RUNTIME_NONE){
        SolMirLinkageDigest root;if(!entry_key(o,x->source_root,&root))return false;key_digest(&state,&root);
    } else return false;
    return sol_mir_linkage_internal_sha256_finish(&state,out);
}
static int host_render_compare(const void *left,const void *right){const HostRenderLine *a_=left,*b_=right;return strcmp(a_->text,b_->text);}
static bool host_render_line(HostRenderLine *lines,size_t *count,const char *kind,const SolMirLinkageDigest *key,const char *suffix){char hex[SOL_MIR_LINKAGE_DIGEST_BYTES*2+1]={0};host_render_digest(hex,key);return snprintf(lines[(*count)++].text,sizeof(lines[0].text),"%s key=%s%s\n",kind,hex,suffix)<(int)sizeof(lines[0].text);}
bool sol_mir_runtime_host_abi_render(FILE*stream,const SolMirRuntimeHostAbi*o){static const char header[]="runtime-host-abi host-abi=true host-execution=false\n";if(!stream||!sol_mir_runtime_host_abi_validate(o,NULL))return false;size_t count=0,total=1;if(!add(&total,o->capability_count)||!add(&total,o->entry_root_count)||!add(&total,o->operation_count)||!add(&total,o->argument_count)||!add(&total,o->formal_count)||!add(&total,o->shape_count)||!add(&total,o->shape_case_count)||!add(&total,o->requirement_count)||total>SIZE_MAX/sizeof(HostRenderLine))return false;HostRenderLine *lines=calloc(total,sizeof(*lines));if(!lines)return false;for(size_t i=0;i<o->capability_count;i++){SolMirLinkageDigest key;const char*suffix=o->capabilities[i].source==SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT?" source=root":o->capabilities[i].source==SOL_MIR_RUNTIME_HOST_CAPABILITY_DERIVED?" source=derived":" source=private";if(!capability_key(o,i,&key)||!host_render_line(lines,&count,"capability",&key,suffix))goto failed;}for(size_t i=0;i<o->entry_root_count;i++){SolMirLinkageDigest key;if(!entry_key(o,i,&key)||!host_render_line(lines,&count,"entry-root",&key,""))goto failed;}for(size_t i=0;i<o->operation_count;i++){SolMirLinkageDigest key;if(!operation_key(o,i,&key)||!host_render_line(lines,&count,"operation",&key,""))goto failed;}for(size_t i=0;i<o->argument_count;i++){SolMirLinkageDigest key;if(!argument_key(o,i,&key)||!host_render_line(lines,&count,"argument",&key,""))goto failed;}for(size_t i=0;i<o->formal_count;i++){SolMirLinkageDigest key;if(!shape_key(o,o->formals[i].shape,&key)||!host_render_line(lines,&count,"formal",&key,""))goto failed;}for(size_t i=0;i<o->shape_count;i++){SolMirLinkageDigest key;if(!shape_key(o,i,&key)||!host_render_line(lines,&count,"shape",&key,""))goto failed;}for(size_t i=0;i<o->shape_case_count;i++){const SolMirRuntimeHostShapeCase*c=&o->shape_cases[i];SolMirLinkageDigest parent,child,key;SolMirLinkageSha256 state;if(!shape_key(o,c->parent,&parent)||(c->child!=SOL_MIR_RUNTIME_NONE&&!shape_key(o,c->child,&child)))goto failed;sol_mir_linkage_internal_sha256_init(&state);key_text(&state,"host-abi-shape-case/v1");key_digest(&state,&parent);key_u64(&state,c->ordinal);if(c->child!=SOL_MIR_RUNTIME_NONE)key_digest(&state,&child);if(!sol_mir_linkage_internal_sha256_finish(&state,&key)||!host_render_line(lines,&count,"shape-case",&key,""))goto failed;}for(size_t i=0;i<o->requirement_count;i++){const SolMirRuntimeHostRequirement*r=&o->requirements[i];SolMirLinkageDigest root,operation,key;SolMirLinkageSha256 state;if(!entry_key(o,r->root,&root)||!operation_key(o,r->operation,&operation))goto failed;sol_mir_linkage_internal_sha256_init(&state);key_text(&state,"host-abi-grant/v1");key_digest(&state,&root);key_digest(&state,&operation);if(!sol_mir_linkage_internal_sha256_finish(&state,&key)||!host_render_line(lines,&count,"requirement",&key,""))goto failed;}qsort(lines,count,sizeof(*lines),host_render_compare);size_t bytes=sizeof(header)-1;for(size_t i=0;i<count;i++)if(!add(&bytes,strlen(lines[i].text)))goto failed;char*buffer=malloc(bytes?bytes:1);if(!buffer)goto failed;size_t offset=0;memcpy(buffer+offset,header,sizeof(header)-1);offset+=sizeof(header)-1;for(size_t i=0;i<count;i++){size_t length=strlen(lines[i].text);memcpy(buffer+offset,lines[i].text,length);offset+=length;}bool ok=fwrite(buffer,bytes,1,stream)==1;free(buffer);free(lines);return ok;failed:free(lines);return false;}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
typedef struct { size_t depth,nodes,work,max_depth,max_nodes,max_work; } ShapeBudget;
static bool check_recipe_shape(const SolMirRuntimeHostAbi *o,SolMirRecipeId recipe,
    const SolMirRuntimeHostValue *value,ShapeBudget *budget){
    const SolMirRepresentation *representation=&o->conventions->concrete->representation;
    SolMirRuntimeHostArgumentKind expected;
    if(!value||recipe>=representation->recipe_count||budget->depth>=budget->max_depth
        ||budget->nodes>=budget->max_nodes||budget->work>=budget->max_work
        ||!representation->recipes[recipe].inhabited
        ||!shape_kind(&representation->recipes[recipe],&expected)
        ||(size_t)value->kind!=(size_t)expected)return false;
    ++budget->depth;++budget->nodes;++budget->work;
    if(value->kind==SOL_MIR_RUNTIME_HOST_VALUE_TEXT&&value->as.text.length&&!value->as.text.bytes){--budget->depth;return false;}
    if(value->kind==SOL_MIR_RUNTIME_HOST_VALUE_OPTION||value->kind==SOL_MIR_RUNTIME_HOST_VALUE_RESULT){
        const SolMirRecipe *sum=&representation->recipes[recipe];
        bool matched=false,ok=false;
        if(sum->variants.offset>representation->variant_count||sum->variants.count>representation->variant_count-sum->variants.offset){--budget->depth;return false;}
        for(size_t i=0;i<sum->variants.count;i++){
            const SolMirRecipeVariant *variant=&representation->variants[sum->variants.offset+i];
            if(variant->ordinal!=value->as.sum.ordinal)continue;
            matched=true;
            if(variant->fields.count==0)ok=value->as.sum.payload==NULL;
            else if(variant->fields.count==1&&variant->fields.offset<representation->field_count)
                ok=value->as.sum.payload&&check_recipe_shape(o,
                    representation->fields[variant->fields.offset].type,value->as.sum.payload,budget);
            break;
        }
        --budget->depth;return matched&&ok;
    }
    --budget->depth;return true;
}
bool sol_mir_runtime_host_abi_test_check_shape(const SolMirRuntimeHostAbi *o,
    SolMirRecipeId recipe,const SolMirRuntimeHostValue *value,
    const SolMirRuntimeHostTransferLimits *limits){
    if(!authenticated_header(o))return false;
    ShapeBudget budget={0,0,0,limits&&limits->max_depth?limits->max_depth:256,
        limits&&limits->max_nodes?limits->max_nodes:1048576,
        limits&&limits->max_work?limits->max_work:4000000};
    return check_recipe_shape(o,recipe,value,&budget);
}
static bool check_shape(const SolMirRuntimeHostAbi*o,size_t id,const SolMirRuntimeHostValue*v){if(!v||id>=o->shape_count)return false;const SolMirRuntimeHostShape*s=&o->shapes[id];return sol_mir_runtime_host_abi_test_check_shape(o,s->recipe,v,NULL)&&v->kind==(SolMirRuntimeHostValueKind)s->kind;}
SolMirRuntimeHostInvocationOutcome sol_mir_runtime_host_abi_test_invoke(const SolMirRuntimeHostInvocationRequest*r,SolMirRuntimeHostInvocationResult*out){
    if(!out)return SOL_MIR_RUNTIME_HOST_INVOKE_INVALID;
    memset(out,0,sizeof(*out));out->outcome=SOL_MIR_RUNTIME_HOST_INVOKE_INVALID;
    if(!r||!r->abi||!r->callback||!r->calls||!r->quota||!r->usage
        ||(r->argument_count&&!r->arguments)
        ||sol_mir_runtime_host_abi_preflight(r->abi,r->preflight)
            !=SOL_MIR_RUNTIME_HOST_PREFLIGHT_SUCCEEDED)return out->outcome;
    if(r->operation>=r->abi->operation_count||r->call>=r->abi->conventions->call_count)return out->outcome;
    const SolMirRuntimeHostOperation*op=&r->abi->operations[r->operation];
    if(r->argument_count!=op->formals.count)return out->outcome;
    for(size_t i=0;i<r->argument_count;i++)if(!check_shape(r->abi,r->abi->formals[op->formals.offset+i].shape,r->arguments[i]))return out->outcome;
    const SolMirRuntimeHostRequirement *requirement=NULL;
    bool granted=false;
    for(size_t i=0;i<r->abi->requirement_count;i++)if(r->abi->requirements[i].entry==r->preflight->entry&&r->abi->requirements[i].root==r->root&&r->abi->requirements[i].operation==r->operation&&r->abi->requirements[i].call==r->call){if(requirement)return out->outcome;requirement=&r->abi->requirements[i];}
    for(size_t i=0;i<r->preflight->grant_count;i++)granted|=r->preflight->grants[i].root==r->root&&r->preflight->grants[i].operation==r->operation;
    if(!requirement||!granted)return out->outcome;
    if(*r->calls>=r->max_calls){out->outcome=SOL_MIR_RUNTIME_HOST_INVOKE_CALL_LIMIT;return out->outcome;}
    ++*r->calls;
    const SolMirRuntimeHostValue*borrowed=NULL;const uint8_t*detail=NULL;size_t length=0;
    if(!r->callback(r->context,r->arguments,r->argument_count,&borrowed,&detail,&length)){
        if(length>SOL_MIR_RUNTIME_HOST_DETAIL_MAX||(length&&!detail))return out->outcome;
        for(size_t i=0;i<length;i++)if(!detail[i])return out->outcome;
        const SolMirRuntimeCleanupTransition *transition=&r->abi->cleanup->transitions[requirement->failure_transition];
        if(transition->failure_site>=r->abi->conventions->failure_site_count)return out->outcome;
        const SolMirRuntimeFailureSite*site=&r->abi->conventions->failure_sites[transition->failure_site];
        if(length)memcpy(out->detail,detail,length);
        out->failure=(SolMirRuntimeFailureRecord){SOL_MIR_RUNTIME_FAILURE_HOST_ERROR,site->source,
            SOL_MIR_RUNTIME_FAILURE_DETAIL_HOST_BYTES,length?out->detail:NULL,length};
        SolMirRuntimeCleanupDetail captured;
        if(!sol_mir_runtime_cleanup_capture_detail(r->abi->cleanup,requirement->event,
            SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE,&out->failure,&captured)){
            memset(out,0,sizeof(*out));out->outcome=SOL_MIR_RUNTIME_HOST_INVOKE_INVALID;return out->outcome;
        }
        SolMirRuntimeCleanupFailureOccurrence occurrence={
            SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31,
            transition->failure_site,SOL_MIR_RUNTIME_FAILURE_HOST_ERROR,
            SOL_MIR_RUNTIME_FAILURE_DETAIL_HOST_BYTES,length,{0},site->source};
        if(length)memcpy(occurrence.detail_bytes,out->detail,length);
        SolMirRuntimeCleanupTraceRequest trace_request={requirement->event,
            SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE,&occurrence,NULL,
            SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL};
        if(!sol_mir_runtime_cleanup_test_trace(r->abi->cleanup,&trace_request,
            NULL,0,&out->cleanup)){
            memset(out,0,sizeof(*out));out->outcome=SOL_MIR_RUNTIME_HOST_INVOKE_INVALID;return out->outcome;
        }
        out->outcome=SOL_MIR_RUNTIME_HOST_INVOKE_HOST_ERROR;return out->outcome;
    }
    SolMirRuntimeHostTransferRequest transfer={r->abi->values,op->result_plan,borrowed,r->quota,r->usage,r->transfer_limits};
    SolMirRuntimeHostTransferOutcome result=sol_mir_runtime_values_test_transfer_host_result(&transfer,&out->value);
    out->outcome=result==SOL_MIR_RUNTIME_HOST_TRANSFER_SUCCEEDED?SOL_MIR_RUNTIME_HOST_INVOKE_SUCCEEDED:result==SOL_MIR_RUNTIME_HOST_TRANSFER_LIMIT?SOL_MIR_RUNTIME_HOST_INVOKE_TRANSFER_LIMIT:result==SOL_MIR_RUNTIME_HOST_TRANSFER_ALLOCATION_LIMIT?SOL_MIR_RUNTIME_HOST_INVOKE_ALLOCATION_LIMIT:result==SOL_MIR_RUNTIME_HOST_TRANSFER_ALLOCATION_FAILED?SOL_MIR_RUNTIME_HOST_INVOKE_ALLOCATION_FAILED:SOL_MIR_RUNTIME_HOST_INVOKE_INVALID;
    return out->outcome;
}
#endif
