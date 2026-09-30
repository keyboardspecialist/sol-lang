#include "wasm_scalar_internal.h"
#include "sol/package.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wasm.h>

bool sol_mir_linkage_test_sha256(const void *bytes, size_t length,
    SolMirLinkageDigest *digest);

static int failures;
#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #value); \
    ++failures; \
} } while (0)
static bool output_zero(const SolWasmScalarOutput *output);
static bool usage_equal(const SolWasmScalarUsage *left, const SolWasmScalarUsage *right);

typedef struct { SolDiagnostics d; SolHirModule h; SolTypeTable t; SolEffectTable e; SolContractTable k; SolIr ir; SolPackage p; } Fixture;
typedef struct { SolMirConcreteProgram c; SolMirRuntimeConventions n; SolMirRuntimeValues v; SolMirRuntimeCleanup q; SolMirRuntimeHostAbi a; SolMirRuntimeHandlerAbi h; SolMirRuntimeLoweredProgram l; } Pipeline;

static void fixture_init(Fixture*x){memset(x,0,sizeof*x);sol_diagnostics_init(&x->d);sol_hir_module_init(&x->h);sol_type_table_init(&x->t);sol_effect_table_init(&x->e);sol_contract_table_init(&x->k);sol_ir_init(&x->ir);sol_package_init(&x->p);}
static void fixture_free(Fixture*x){sol_package_free(&x->p);sol_ir_free(&x->ir);sol_contract_table_free(&x->k);sol_effect_table_free(&x->e);sol_type_table_free(&x->t);sol_hir_module_free(&x->h);sol_diagnostics_free(&x->d);}
static SolIrCallableId named(const SolIr*x,const char*n){for(size_t i=0;i<x->callable_count;i++)if(x->callables[i].kind==SOL_IR_CALLABLE_FUNCTION&&!strcmp(x->callables[i].name,n))return i;return SOL_IR_NONE;}
static bool fixture_load(Fixture*x,const char*directory){char error[256];if(!sol_package_load_directory(&x->p,directory,&x->d,error,sizeof error)||x->p.file_count>2)return false;SolHirFileScope s[2];for(size_t i=0;i<x->p.file_count;i++)s[i]=(SolHirFileScope){x->p.files[i].module_name,x->p.files[i].import_start,x->p.files[i].import_count,x->p.files[i].item_start,x->p.files[i].item_count};return sol_hir_lower_scoped(&x->p.source,&x->p.syntax,s,x->p.file_count,&x->h,&x->d)&&sol_type_check(&x->p.source,&x->p.syntax,&x->h,&x->t,&x->d)&&sol_effect_check(&x->p.source,&x->p.syntax,&x->h,&x->t,&x->e,&x->d)&&sol_contract_lower(&x->p.source,&x->p.syntax,&x->h,&x->t,&x->e,&x->k,&x->d)&&sol_ir_lower_scoped(&x->p.source,&x->p.syntax,&x->h,&x->t,&x->e,&x->k,x->p.files,x->p.file_count,&x->ir,&x->d);}
static void pipeline_init(Pipeline*x){memset(x,0,sizeof*x);sol_mir_concrete_program_init(&x->c);sol_mir_runtime_conventions_init(&x->n);sol_mir_runtime_values_init(&x->v);sol_mir_runtime_cleanup_init(&x->q);sol_mir_runtime_host_abi_init(&x->a);sol_mir_runtime_handler_abi_init(&x->h);sol_mir_runtime_lowered_program_init(&x->l);}
static void pipeline_free(Pipeline*x){sol_mir_runtime_lowered_program_free(&x->l);sol_mir_runtime_handler_abi_free(&x->h);sol_mir_runtime_host_abi_free(&x->a);sol_mir_runtime_cleanup_free(&x->q);sol_mir_runtime_values_free(&x->v);sol_mir_runtime_conventions_free(&x->n);sol_mir_concrete_program_free(&x->c);}
static bool pipeline_build(Fixture*f,Pipeline*x,const char*root,SolMirProgramRootKind kind){SolMirProgramRoot r={named(&f->ir,root),kind};SolMirTargetDescriptor t=sol_mir_target_wasm32();return r.callable!=SOL_IR_NONE&&sol_mir_concrete_program_build(&(SolMirConcreteBuildRequest){&f->ir,&r,1,NULL,0,&t,NULL},&x->c,&f->d)==SOL_MIR_CONCRETE_BUILD_SUCCEEDED&&sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&x->c,NULL},&x->n,&f->d)==SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED&&sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&x->n,NULL},&x->v,&f->d)==SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED&&sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&x->n,&x->v,NULL},&x->q,&f->d)==SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED&&sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&x->n,&x->v,&x->q,NULL},&x->a,&f->d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED&&sol_mir_runtime_handler_abi_build(&(SolMirRuntimeHandlerAbiBuildRequest){&x->n,&x->v,&x->q,&x->a,NULL},&x->h,&f->d)==SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_SUCCEEDED&&sol_mir_runtime_lowered_program_build(&(SolMirRuntimeLoweredProgramBuildRequest){&x->n,&x->v,&x->q,&x->a,&x->h,NULL},&x->l,&f->d)==SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED;}

static bool name_equal(const wasm_name_t*n,const char*s){return n&&n->size==strlen(s)&&!memcmp(n->data,s,n->size);}
static bool export_shape(const SolWasmBackendBytes*b,const char*entry){wasm_engine_t*e=wasm_engine_new();wasm_store_t*s=e?wasm_store_new(e):NULL;wasm_byte_vec_t bytes={b->count,(wasm_byte_t*)b->bytes};wasm_module_t*m=s?wasm_module_new(s,&bytes):NULL;wasm_exporttype_vec_t x;bool ok=m!=NULL;if(ok){wasm_module_exports(m,&x);ok=x.size==(entry?3:2);bool code=false,site=false,call=false;for(size_t i=0;i<x.size;i++){const wasm_name_t*n=wasm_exporttype_name(x.data[i]);wasm_externkind_t k=wasm_externtype_kind(wasm_exporttype_type(x.data[i]));code|=k==WASM_EXTERN_GLOBAL&&name_equal(n,SOL_WASM_SCALAR_FAILURE_CODE_EXPORT);site|=k==WASM_EXTERN_GLOBAL&&name_equal(n,SOL_WASM_SCALAR_FAILURE_SITE_EXPORT);call|=entry&&k==WASM_EXTERN_FUNC&&name_equal(n,entry);}ok=ok&&code&&site&&(entry?call:true);wasm_exporttype_vec_delete(&x);}if(m)wasm_module_delete(m);if(s)wasm_store_delete(s);if(e)wasm_engine_delete(e);return ok;}
static bool scalar_value(const Pipeline *pipeline, SolMirMaterializedValueId value) {
    const SolMirMaterialization *m = &pipeline->c.materialization;
    if (value >= m->value_count || m->values[value].type >= pipeline->c.layout.type_count)
        return false;
    SolMirRecipeId recipe = pipeline->c.layout.types[m->values[value].type].recipe;
    return recipe < pipeline->c.layout.representation->recipe_count
        && (pipeline->c.layout.representation->recipes[recipe].kind == SOL_MIR_RECIPE_INT64
            || pipeline->c.layout.representation->recipes[recipe].kind == SOL_MIR_RECIPE_BOOL);
}
static bool terminator_uses_edge(const SolMirMaterializedTerminator *term, size_t edge) {
    return ((term->kind == SOL_MIR_TERM_GOTO || term->kind == SOL_MIR_TERM_BREAK
        || term->kind == SOL_MIR_TERM_CONTINUE) && term->edge == edge)
        || (term->kind == SOL_MIR_TERM_BRANCH
            && (term->true_edge == edge || term->false_edge == edge));
}
static bool entry_true_edge_reaches(const SolMirMaterialization *m,
    const SolMirMaterializedImage *image, SolMirMaterializedBlockId block) {
    if (image->entry >= m->block_count) return false;
    const SolMirMaterializedTerminator *term = &m->blocks[image->entry].terminator;
    if (term->kind != SOL_MIR_TERM_BRANCH || term->condition >= m->value_count
        || term->true_edge >= m->edge_count) return false;
    const SolMirMaterializedValue *condition = &m->values[term->condition];
    if (condition->instruction >= m->instruction_count) return false;
    const SolMirMaterializedInstruction *definition = &m->instructions[condition->instruction];
    return definition->kind == SOL_MIR_INST_CONST_BOOL && definition->boolean
        && m->edges[term->true_edge].block == block;
}
static bool entry_uses_one_argument_scalar_return_edge(const Pipeline *pipeline,
    SolMirLinkageCallableId callable) {
    const SolMirConcreteProgram *concrete = &pipeline->c;
    const SolMirMaterialization *m = &concrete->materialization;
    if (callable >= concrete->linkage.callable_count) return false;
    SolMirPlanInstanceId image_id = concrete->linkage.callables[callable].instance;
    if (image_id >= m->image_count) return false;
    const SolMirMaterializedImage *image = &m->images[image_id];
    for (size_t edge = 0; edge < m->edge_count; ++edge) {
        const SolMirMaterializedEdge *item = &m->edges[edge];
        const SolMirRuntimeLoweredImageEdge *owner = &pipeline->l.image_edges[edge];
        if (owner->state != SOL_MIR_RUNTIME_LOWERED_PRESENT || owner->edge != edge
            || owner->image != image_id || owner->target != item->block
            || owner->source < image->blocks.offset
            || owner->source - image->blocks.offset >= image->blocks.count
            || item->block < image->blocks.offset
            || item->block - image->blocks.offset >= image->blocks.count
            || item->block >= m->block_count || item->arguments.count != 1
            || item->arguments.offset >= m->edge_value_count) continue;
        const SolMirMaterializedBlock *target = &m->blocks[item->block];
        if (target->parameters.count != 1 || target->parameters.offset >= m->parameter_value_count)
            continue;
        SolMirMaterializedValueId argument = m->edge_values[item->arguments.offset];
        SolMirMaterializedValueId parameter = m->parameter_values[target->parameters.offset];
        if (argument >= m->value_count || parameter >= m->value_count
            || m->values[argument].type != m->values[parameter].type
            || !scalar_value(pipeline, argument)
            || !entry_true_edge_reaches(m, image, owner->source)
            || !terminator_uses_edge(&m->blocks[owner->source].terminator, edge)
            || target->terminator.kind != SOL_MIR_TERM_RETURN) continue;
        SolMirMaterializedLocalId stored_local = SOL_MIR_MATERIALIZED_NONE;
        bool return_loads_stored_parameter = false;
        for (size_t i = 0; i < target->instructions.count; ++i) {
            const SolMirMaterializedInstruction *instruction = &m->instructions[
                target->instructions.offset + i];
            if (instruction->kind == SOL_MIR_INST_STORE && instruction->left == parameter
                && instruction->place < m->place_count
                && m->places[instruction->place].local < m->local_count)
                stored_local = m->places[instruction->place].local;
            if (instruction->kind == SOL_MIR_INST_LOAD_COPY
                && instruction->result == target->terminator.value
                && instruction->place < m->place_count
                && stored_local != SOL_MIR_MATERIALIZED_NONE
                && m->places[instruction->place].local == stored_local)
                return_loads_stored_parameter = true;
        }
        if (!return_loads_stored_parameter) continue;
        return true;
    }
    return false;
}
typedef struct { uint8_t tag,kind; const uint8_t *path,*symbol;uint32_t path_length,start,end,symbol_length,ordinal; } Provenance;
static bool uleb(const uint8_t*b,size_t n,size_t*at,uint32_t*out){uint32_t value=0;unsigned shift=0;while(*at<n&&shift<35){uint8_t x=b[(*at)++];if((x&0x7f)>UINT32_MAX>>shift)return false;value|=(uint32_t)(x&0x7f)<<shift;if(!(x&0x80)){*out=value;return true;}shift+=7;}return false;}
static int provenance_cmp(const Provenance*a,const Provenance*b){if(a->tag!=b->tag)return a->tag<b->tag?-1:1;size_t n=a->path_length<b->path_length?a->path_length:b->path_length;int q=memcmp(a->path,b->path,n);if(q)return q;if(a->path_length!=b->path_length)return a->path_length<b->path_length?-1:1;if(a->start!=b->start)return a->start<b->start?-1:1;if(a->end!=b->end)return a->end<b->end?-1:1;n=a->symbol_length<b->symbol_length?a->symbol_length:b->symbol_length;q=memcmp(a->symbol,b->symbol,n);if(q)return q;if(a->symbol_length!=b->symbol_length)return a->symbol_length<b->symbol_length?-1:1;if(a->ordinal!=b->ordinal)return a->ordinal<b->ordinal?-1:1;return (int)a->kind-(int)b->kind;}
static bool u32le(const uint8_t*b,size_t n,size_t*at,uint32_t*out){if(*at>n||n-*at<4)return false;*out=(uint32_t)b[*at]|(uint32_t)b[*at+1]<<8|(uint32_t)b[*at+2]<<16|(uint32_t)b[*at+3]<<24;*at+=4;return true;}
static bool path_clean(const uint8_t*p,size_t n){if(n==0||p[0]=='/')return false;for(size_t i=1;i<n;i++)if(p[i-1]=='.'&&p[i]=='.')return false;return true;}
static bool provenance_decode(const SolWasmBackendBytes*b,Provenance*out,size_t cap,size_t*count){size_t at=8,found=0;*count=0;if(b->count<8||memcmp(b->bytes,"\0asm\1\0\0\0",8))return false;while(at<b->count){uint32_t length;if(at>=b->count)return false;uint8_t id=b->bytes[at++];if(!uleb(b->bytes,b->count,&at,&length)||length>b->count-at)return false;size_t end=at+length;if(id!=0){at=end;continue;}uint32_t name;if(!uleb(b->bytes,end,&at,&name)||name>end-at||name!=strlen(SOL_WASM_SCALAR_PROVENANCE_SECTION)||memcmp(b->bytes+at,SOL_WASM_SCALAR_PROVENANCE_SECTION,name)){at=end;continue;}at+=name;if(++found!=1||end-at<12||memcmp(b->bytes+at,"P42P",4))return false;at+=4;uint32_t version,records;if(!u32le(b->bytes,end,&at,&version)||!u32le(b->bytes,end,&at,&records)||version!=1||records>cap)return false;for(uint32_t i=0;i<records;i++){Provenance*r=&out[i];if(end-at<4)return false;r->tag=b->bytes[at++];r->kind=b->bytes[at++];if(b->bytes[at++]||b->bytes[at++])return false;if(!u32le(b->bytes,end,&at,&r->path_length)||r->path_length>end-at)return false;r->path=b->bytes+at;at+=r->path_length;if(!u32le(b->bytes,end,&at,&r->start)||!u32le(b->bytes,end,&at,&r->end)||!u32le(b->bytes,end,&at,&r->symbol_length)||r->symbol_length>end-at)return false;r->symbol=b->bytes+at;at+=r->symbol_length;if(!u32le(b->bytes,end,&at,&r->ordinal)||!path_clean(r->path,r->path_length))return false;if(i&&provenance_cmp(&out[i-1],r)>=0)return false;}if(at!=end)return false;*count=records;}return found==1;}
static bool invoke_named(const SolWasmBackendBytes*b,const char*entry,int64_t value,int32_t code,int32_t site){wasm_engine_t*e=wasm_engine_new();wasm_store_t*s=e?wasm_store_new(e):NULL;wasm_byte_vec_t bytes={b->count,(wasm_byte_t*)b->bytes};wasm_module_t*m=s?wasm_module_new(s,&bytes):NULL;wasm_exporttype_vec_t types;wasm_importtype_vec_t imports;wasm_extern_vec_t exports=WASM_EMPTY_VEC,none=WASM_EMPTY_VEC;wasm_trap_t*t=NULL;wasm_instance_t*i=NULL;size_t fi=SIZE_MAX,ci=SIZE_MAX,si=SIZE_MAX;bool ok=m!=NULL;if(ok){wasm_module_imports(m,&imports);ok=imports.size==0;wasm_importtype_vec_delete(&imports);wasm_module_exports(m,&types);for(size_t q=0;q<types.size;q++){const wasm_name_t*n=wasm_exporttype_name(types.data[q]);wasm_externkind_t k=wasm_externtype_kind(wasm_exporttype_type(types.data[q]));if(name_equal(n,entry)){if(k!=WASM_EXTERN_FUNC||fi!=SIZE_MAX)ok=false;fi=q;}if(name_equal(n,SOL_WASM_SCALAR_FAILURE_CODE_EXPORT)){if(k!=WASM_EXTERN_GLOBAL||ci!=SIZE_MAX)ok=false;ci=q;}if(name_equal(n,SOL_WASM_SCALAR_FAILURE_SITE_EXPORT)){if(k!=WASM_EXTERN_GLOBAL||si!=SIZE_MAX)ok=false;si=q;}}ok=ok&&fi!=SIZE_MAX&&ci!=SIZE_MAX&&si!=SIZE_MAX;wasm_exporttype_vec_delete(&types);}if(ok)i=wasm_instance_new(s,m,&none,&t);if(t){wasm_trap_delete(t);t=NULL;ok=false;}if(ok&&i){wasm_instance_exports(i,&exports);wasm_func_t*f=wasm_extern_as_func(exports.data[fi]);wasm_global_t*c=wasm_extern_as_global(exports.data[ci]),*z=wasm_extern_as_global(exports.data[si]);wasm_functype_t*ft=f?wasm_func_type(f):NULL;const wasm_valtype_vec_t*ps=ft?wasm_functype_params(ft):NULL,*rs=ft?wasm_functype_results(ft):NULL;wasm_val_t out[1],x,y;wasm_val_vec_t a=WASM_EMPTY_VEC,r=WASM_ARRAY_VEC(out);ok=f&&c&&z&&ps->size==0&&rs->size==1&&wasm_valtype_kind(rs->data[0])==WASM_I64;if(ok)t=wasm_func_call(f,&a,&r);if(t){wasm_trap_delete(t);t=NULL;ok=false;}if(ok){wasm_global_get(c,&x);wasm_global_get(z,&y);ok=out[0].kind==WASM_I64&&out[0].of.i64==value&&x.kind==WASM_I32&&y.kind==WASM_I32&&x.of.i32==code&&y.of.i32==site;}if(ft)wasm_functype_delete(ft);}wasm_extern_vec_delete(&exports);if(i)wasm_instance_delete(i);if(m)wasm_module_delete(m);if(s)wasm_store_delete(s);if(e)wasm_engine_delete(e);return ok;}

static bool failure_for(const Pipeline*,SolMirOperationOpcode,bool,size_t*,size_t*);
static bool failure_record(const Pipeline*,const Provenance*,size_t,size_t,size_t*);
static void test_fixture(const char*leaf,const char*root,SolMirProgramRootKind kind,int64_t expected){char directory[512];snprintf(directory,sizeof directory,"%s/tests/conformance/%s",SOL_TEST_SOURCE_DIR,leaf);Fixture f;Pipeline p;fixture_init(&f);pipeline_init(&p);CHECK(fixture_load(&f,directory));CHECK(pipeline_build(&f,&p,root,kind));if(p.l.authentication){SolWasmScalarBuildRequest r={&p.l,directory,NULL};SolWasmScalarOutput a,b;sol_wasm_scalar_output_init(&a);sol_wasm_scalar_output_init(&b);CHECK(sol_wasm_scalar_build_scalar(&r,&a,&f.d)==SOL_WASM_SCALAR_OK);CHECK(sol_wasm_scalar_build_scalar(&r,&b,&f.d)==SOL_WASM_SCALAR_OK);CHECK(a.bytes.count==b.bytes.count&&!memcmp(a.bytes.bytes,b.bytes.bytes,a.bytes.count));CHECK(sol_wasm_scalar_validate_scalar(&a.bytes)==SOL_WASM_SCALAR_OK);if(kind==SOL_MIR_PROGRAM_ROOT_ENTRY){CHECK(p.n.entry_count==1);if(p.n.entry_count==1){if(!strcmp(leaf,"p42_scalar"))CHECK(entry_uses_one_argument_scalar_return_edge(&p,p.n.entries[0].callable));CHECK(export_shape(&a.bytes,p.n.entries[0].symbol.bytes));CHECK(invoke_named(&a.bytes,p.n.entries[0].symbol.bytes,expected,0,0));}}else {CHECK(p.n.entry_count==0);CHECK(export_shape(&a.bytes,NULL));}sol_wasm_scalar_output_free(&a);sol_wasm_scalar_output_free(&b);}pipeline_free(&p);fixture_free(&f);}
static void test_rejections(void){char directory[512];snprintf(directory,sizeof directory,"%s/tests/conformance/p42_scalar_internal",SOL_TEST_SOURCE_DIR);Fixture f;Pipeline p;fixture_init(&f);pipeline_init(&p);CHECK(fixture_load(&f,directory));CHECK(pipeline_build(&f,&p,"scalar",SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE));if(p.l.authentication){const char*bad[]={"","relative","/tmp/./scalar","/tmp/../scalar", "/Users/jsobotka/code/sol-lang/tests/conformance/p42_scalar_internal_suffix"};for(size_t i=0;i<sizeof bad/sizeof*bad;i++){SolWasmScalarOutput o;sol_wasm_scalar_output_init(&o);SolWasmScalarBuildRequest r={&p.l,bad[i],NULL};CHECK(sol_wasm_scalar_build_scalar(&r,&o,NULL)==SOL_WASM_SCALAR_UNSUPPORTED_CLOSURE&&o.bytes.bytes==NULL&&o.bytes.count==0);sol_wasm_scalar_output_free(&o);}SolWasmScalarOutput o;sol_wasm_scalar_output_init(&o);SolWasmScalarBuildRequest r={&p.l,directory,NULL};CHECK(sol_wasm_scalar_build_scalar(&r,&o,NULL)==SOL_WASM_SCALAR_OK);p.l.authentication^=1;CHECK(sol_wasm_scalar_build_scalar(&r,&o,NULL)==SOL_WASM_SCALAR_UNSUPPORTED_CLOSURE&&o.bytes.bytes==NULL);p.l.authentication^=1;if(p.l.image_edge_count){SolMirRuntimeLoweredImageEdge saved=p.l.image_edges[0];p.l.image_edges[0].source=SOL_MIR_RUNTIME_LOWERED_NONE;CHECK(sol_wasm_scalar_build_scalar(&r,&o,NULL)==SOL_WASM_SCALAR_UNSUPPORTED_CLOSURE);p.l.image_edges[0]=saved;}SolMirTargetDescriptor saved_target=p.c.layout.target;p.c.layout.target.pointer_size=8;CHECK(sol_wasm_scalar_build_scalar(&r,&o,NULL)==SOL_WASM_SCALAR_UNSUPPORTED_CLOSURE);p.c.layout.target=saved_target;sol_wasm_scalar_output_free(&o);}pipeline_free(&p);fixture_free(&f);}
static bool compound_cleanup_order(const Pipeline*p){const SolMirMaterialization*m=&p->c.materialization;for(size_t i=0;i<m->instruction_count;i++)if(m->instructions[i].kind==SOL_MIR_INST_COMPOUND_UPDATE){const SolMirRuntimeLoweredImageInstruction*r=&p->l.image_instructions[i];if(r->cleanup_event>=p->q.event_count)return false;const SolMirRuntimeCleanupEvent*e=&p->q.events[r->cleanup_event];const SolMirRuntimeCleanupTransition*t=NULL;for(size_t q=0;q<e->transitions.count;q++){const SolMirRuntimeCleanupTransition*x=&p->q.transitions[e->transitions.offset+q];if(x->outcome==SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE){if(t)return false;t=x;}}if(!t||t->actions.count<3)return false;bool temp=false,cleanup=false;for(size_t q=0;q<t->actions.count;q++){const SolMirRuntimeCleanupAction*a=&p->q.actions[t->actions.offset+q];if(q+1==t->actions.count){if(a->kind!=SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE)return false;continue;}if(a->kind==SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_TEMPORARY)temp=true;else if(temp&&(a->kind==SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE||a->kind==SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_SCOPE||a->kind==SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_REGION))cleanup=true;}bool store=false,drop=false;const SolMirMaterializedBlock*b=&m->blocks[m->instructions[i].block];for(size_t q=0;q<b->instructions.count;q++){const SolMirMaterializedInstruction*x=&m->instructions[b->instructions.offset+q];if(b->instructions.offset+q<=i)continue;store|=x->kind==SOL_MIR_INST_STORE;drop|=x->kind==SOL_MIR_INST_DROP_IF_INITIALIZED||x->kind==SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED;}return temp&&cleanup&&store&&drop;}return false;}
static void test_compound_overflow(void){char directory[512];snprintf(directory,sizeof directory,"%s/tests/conformance/p42_scalar_compound_overflow",SOL_TEST_SOURCE_DIR);Fixture f;Pipeline p;fixture_init(&f);pipeline_init(&p);CHECK(fixture_load(&f,directory));CHECK(pipeline_build(&f,&p,"launch",SOL_MIR_PROGRAM_ROOT_ENTRY));CHECK(compound_cleanup_order(&p));if(p.l.authentication&&p.n.entry_count==1){size_t instruction,site;CHECK(failure_for(&p,SOL_MIR_OPERATION_I64_ADD,true,&instruction,&site));SolWasmScalarOutput a,b;sol_wasm_scalar_output_init(&a);sol_wasm_scalar_output_init(&b);SolWasmScalarBuildRequest r={&p.l,directory,NULL};CHECK(sol_wasm_scalar_build_scalar(&r,&a,&f.d)==SOL_WASM_SCALAR_OK);CHECK(sol_wasm_scalar_build_scalar(&r,&b,&f.d)==SOL_WASM_SCALAR_OK);CHECK(a.bytes.count==b.bytes.count&&!memcmp(a.bytes.bytes,b.bytes.bytes,a.bytes.count));CHECK(export_shape(&a.bytes,p.n.entries[0].symbol.bytes));Provenance records[8];size_t count=0,index=0;CHECK(provenance_decode(&a.bytes,records,8,&count)&&failure_record(&p,records,count,site,&index));if(index<count)CHECK(invoke_named(&a.bytes,p.n.entries[0].symbol.bytes,0,2,(int32_t)(index+1)));sol_wasm_scalar_output_free(&a);sol_wasm_scalar_output_free(&b);}pipeline_free(&p);fixture_free(&f);}
static void test_compound_rejection(void){char directory[512];snprintf(directory,sizeof directory,"%s/tests/conformance/p42_scalar_internal",SOL_TEST_SOURCE_DIR);Fixture f;Pipeline p;fixture_init(&f);pipeline_init(&p);CHECK(fixture_load(&f,directory));CHECK(pipeline_build(&f,&p,"compound",SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE));bool compound=false;for(size_t i=0;i<p.c.materialization.instruction_count;i++)compound|=p.c.materialization.instructions[i].kind==SOL_MIR_INST_COMPOUND_UPDATE;CHECK(compound);if(p.l.authentication){SolWasmScalarOutput o;sol_wasm_scalar_output_init(&o);SolWasmScalarBuildRequest r={&p.l,directory,NULL};CHECK(sol_wasm_scalar_build_scalar(&r,&o,&f.d)==SOL_WASM_SCALAR_OK);sol_wasm_scalar_output_free(&o);}pipeline_free(&p);fixture_free(&f);}

static bool pipeline_build_roots(Fixture*f,Pipeline*x,const SolMirProgramRoot*roots,size_t count){SolMirTargetDescriptor t=sol_mir_target_wasm32();return sol_mir_concrete_program_build(&(SolMirConcreteBuildRequest){&f->ir,roots,count,NULL,0,&t,NULL},&x->c,&f->d)==SOL_MIR_CONCRETE_BUILD_SUCCEEDED&&sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&x->c,NULL},&x->n,&f->d)==SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED&&sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&x->n,NULL},&x->v,&f->d)==SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED&&sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&x->n,&x->v,NULL},&x->q,&f->d)==SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED&&sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&x->n,&x->v,&x->q,NULL},&x->a,&f->d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED&&sol_mir_runtime_handler_abi_build(&(SolMirRuntimeHandlerAbiBuildRequest){&x->n,&x->v,&x->q,&x->a,NULL},&x->h,&f->d)==SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_SUCCEEDED&&sol_mir_runtime_lowered_program_build(&(SolMirRuntimeLoweredProgramBuildRequest){&x->n,&x->v,&x->q,&x->a,&x->h,NULL},&x->l,&f->d)==SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED;}
static const SolMirOperationArithmeticPlan*plan_at(const Pipeline*p,size_t instruction){if(instruction>=p->l.image_instruction_count)return NULL;const SolMirRuntimeLoweredImageInstruction*r=&p->l.image_instructions[instruction];if(r->plan>=p->l.semantic_plan_count)return NULL;const SolMirRuntimeLoweredSemanticPlan*s=&p->l.semantic_plans[r->plan];return s->state==SOL_MIR_RUNTIME_LOWERED_PRESENT&&s->arena==SOL_MIR_RUNTIME_LOWERED_SEMANTIC_ARITHMETIC&&s->plan<p->c.operations.arithmetic_count?&p->c.operations.arithmetic[s->plan]:NULL;}
static bool failure_for(const Pipeline*p,SolMirOperationOpcode opcode,bool compound,size_t*instruction,size_t*site){bool found=false;size_t latest=0;for(size_t i=0;i<p->c.materialization.instruction_count;i++){const SolMirOperationArithmeticPlan*plan=plan_at(p,i);if(!plan||plan->opcode!=opcode||plan->compound!=compound)continue;const SolMirRuntimeLoweredImageInstruction*r=&p->l.image_instructions[i];if(r->failure_site>=p->n.failure_site_count)return false;const SolMirRuntimeFailureSite*f=&p->n.failure_sites[r->failure_site];if(f->origin_kind!=SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_ARITHMETIC||f->instruction!=i||f->owner!=plan->image)return false;if(!found||f->source.start>latest){*instruction=i;*site=r->failure_site;latest=f->source.start;found=true;}}return found;}
static bool failure_record(const Pipeline*p,const Provenance*records,size_t count,size_t site,size_t*record){if(site>=p->n.failure_site_count||p->n.entry_count!=1)return false;const SolMirRuntimeFailureSite*f=&p->n.failure_sites[site];const char*symbol=p->c.linkage.callables[p->n.entries[0].callable].symbol.bytes;size_t found=0;for(size_t i=0;i<count;i++){const Provenance*r=&records[i];if(r->tag!=3||r->kind!=f->origin_kind||r->path_length!=8||memcmp(r->path,"main.sol",8)||r->start!=f->source.start||r->end!=f->source.end||r->symbol_length!=strlen(symbol)||memcmp(r->symbol,symbol,r->symbol_length))continue;*record=i;++found;}return found==1;}
static void test_arithmetic_failure(const char*leaf,SolMirOperationOpcode opcode,bool compound,int32_t code){char directory[512];snprintf(directory,sizeof directory,"%s/tests/conformance/%s",SOL_TEST_SOURCE_DIR,leaf);Fixture f;Pipeline p;fixture_init(&f);pipeline_init(&p);CHECK(fixture_load(&f,directory));CHECK(pipeline_build(&f,&p,"launch",SOL_MIR_PROGRAM_ROOT_ENTRY));if(p.l.authentication&&p.n.entry_count==1){size_t instruction,site;CHECK(failure_for(&p,opcode,compound,&instruction,&site));SolWasmScalarOutput a,b;sol_wasm_scalar_output_init(&a);sol_wasm_scalar_output_init(&b);SolWasmScalarBuildRequest r={&p.l,directory,NULL};CHECK(sol_wasm_scalar_build_scalar(&r,&a,&f.d)==SOL_WASM_SCALAR_OK);CHECK(sol_wasm_scalar_build_scalar(&r,&b,&f.d)==SOL_WASM_SCALAR_OK);CHECK(a.bytes.count==b.bytes.count&&!memcmp(a.bytes.bytes,b.bytes.bytes,a.bytes.count));CHECK(sol_wasm_scalar_validate_scalar(&a.bytes)==SOL_WASM_SCALAR_OK);CHECK(export_shape(&a.bytes,p.n.entries[0].symbol.bytes));Provenance records[32];size_t count=0,index=0;CHECK(provenance_decode(&a.bytes,records,32,&count));CHECK(failure_record(&p,records,count,site,&index));if(index<count)CHECK(invoke_named(&a.bytes,p.n.entries[0].symbol.bytes,0,code,(int32_t)(index+1)));sol_wasm_scalar_output_free(&a);sol_wasm_scalar_output_free(&b);}pipeline_free(&p);fixture_free(&f);}
static void test_rejected_fixture(const char*leaf){char directory[512];snprintf(directory,sizeof directory,"%s/tests/conformance/%s",SOL_TEST_SOURCE_DIR,leaf);Fixture f;Pipeline p;fixture_init(&f);pipeline_init(&p);CHECK(fixture_load(&f,directory));CHECK(pipeline_build(&f,&p,"launch",SOL_MIR_PROGRAM_ROOT_ENTRY));if(p.l.authentication){SolWasmScalarOutput o;sol_wasm_scalar_output_init(&o);SolWasmScalarBuildRequest r={&p.l,directory,NULL};CHECK(sol_wasm_scalar_build_scalar(&r,&o,&f.d)==SOL_WASM_SCALAR_UNSUPPORTED_CLOSURE&&o.bytes.bytes==NULL&&o.bytes.count==0);sol_wasm_scalar_output_free(&o);}pipeline_free(&p);fixture_free(&f);}
static bool copy_file(const char*from,const char*to){FILE*in=fopen(from,"rb"),*out=to?fopen(to,"wb"):NULL;char buffer[1024];bool ok=in&&out;size_t n;while(ok&&(n=fread(buffer,1,sizeof buffer,in))!=0)ok=fwrite(buffer,1,n,out)==n;ok=ok&&in&&!ferror(in)&&out&&!ferror(out);if(in)fclose(in);if(out)fclose(out);return ok;}
static bool build_entry_bytes(const char*directory,SolWasmScalarOutput*out,Fixture*f,Pipeline*p){if(!fixture_load(f,directory)||!pipeline_build(f,p,"launch",SOL_MIR_PROGRAM_ROOT_ENTRY)||p->n.entry_count!=1)return false;SolWasmScalarBuildRequest r={&p->l,directory,NULL};return sol_wasm_scalar_build_scalar(&r,out,&f->d)==SOL_WASM_SCALAR_OK;}
static bool records_equal(const Provenance*a,const Provenance*b,size_t count){for(size_t i=0;i<count;i++)if(a[i].tag!=b[i].tag||a[i].kind!=b[i].kind||a[i].path_length!=b[i].path_length||a[i].start!=b[i].start||a[i].end!=b[i].end||a[i].symbol_length!=b[i].symbol_length||a[i].ordinal!=b[i].ordinal||memcmp(a[i].path,b[i].path,a[i].path_length)||memcmp(a[i].symbol,b[i].symbol,a[i].symbol_length))return false;return true;}
static bool test_artifact_parent(void){struct stat info;if(SOL_TEST_BINARY_DIR[0]!='/')return false;if(mkdir(SOL_TEST_BINARY_DIR,0700)==0)return true;return errno==EEXIST&&stat(SOL_TEST_BINARY_DIR,&info)==0&&S_ISDIR(info.st_mode);}
static char*test_artifact_root(const char*name){size_t size=strlen(SOL_TEST_BINARY_DIR)+strlen(name)+14;char*path=malloc(size);if(path==NULL||snprintf(path,size,"%s/p42-%s-XXXXXX",SOL_TEST_BINARY_DIR,name)<0){free(path);return NULL;}if(mkdtemp(path)==NULL){free(path);return NULL;}return path;}
static char*joined_path(const char*directory,const char*name){size_t size=strlen(directory)+strlen(name)+2;char*path=malloc(size);if(path==NULL||snprintf(path,size,"%s/%s",directory,name)<0){free(path);return NULL;}return path;}
static void test_relocated_canonicality(void){Fixture fa,fb;Pipeline pa,pb;SolWasmScalarOutput oa,ob;char*left=NULL,*right=NULL,*from=NULL,*a=NULL,*b=NULL;bool copied_a=false,copied_b=false;fixture_init(&fa);fixture_init(&fb);pipeline_init(&pa);pipeline_init(&pb);sol_wasm_scalar_output_init(&oa);sol_wasm_scalar_output_init(&ob);CHECK(test_artifact_parent());if(test_artifact_parent()){left=test_artifact_root("left");right=test_artifact_root("right");}CHECK(left!=NULL&&right!=NULL&&strcmp(left,right));if(left!=NULL&&right!=NULL){from=joined_path(SOL_TEST_SOURCE_DIR,"tests/conformance/p42_scalar_call_nested_overflow/main.sol");a=joined_path(left,"main.sol");b=joined_path(right,"main.sol");CHECK(from!=NULL&&a!=NULL&&b!=NULL);if(from&&a)copied_a=copy_file(from,a);if(from&&b)copied_b=copy_file(from,b);CHECK(copied_a);CHECK(copied_b);bool built_a=false,built_b=false;if(copied_a)built_a=build_entry_bytes(left,&oa,&fa,&pa);if(copied_b)built_b=build_entry_bytes(right,&ob,&fb,&pb);CHECK(built_a);CHECK(built_b);if(built_a&&built_b){CHECK(!strcmp(pa.n.entries[0].symbol.bytes,pb.n.entries[0].symbol.bytes));CHECK(oa.bytes.count==ob.bytes.count&&!memcmp(oa.bytes.bytes,ob.bytes.bytes,oa.bytes.count)&&usage_equal(&oa.usage,&ob.usage));Provenance ra[16],rb[16];size_t na=0,nb=0;CHECK(provenance_decode(&oa.bytes,ra,16,&na)&&provenance_decode(&ob.bytes,rb,16,&nb)&&na==nb&&records_equal(ra,rb,na));}}sol_wasm_scalar_output_free(&oa);sol_wasm_scalar_output_free(&ob);pipeline_free(&pa);pipeline_free(&pb);fixture_free(&fa);fixture_free(&fb);if(copied_a)(void)unlink(a);if(copied_b)(void)unlink(b);if(left)(void)rmdir(left);if(right)(void)rmdir(right);free(from);free(a);free(b);free(left);free(right);}
static void test_internal_root_order(void){char directory[512];snprintf(directory,sizeof directory,"%s/tests/conformance/p42_scalar_internal",SOL_TEST_SOURCE_DIR);Fixture f;Pipeline a,b;fixture_init(&f);pipeline_init(&a);pipeline_init(&b);CHECK(fixture_load(&f,directory));SolMirProgramRoot ab[]={{named(&f.ir,"scalar"),SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE},{named(&f.ir,"compound"),SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE}},ba[]={{named(&f.ir,"compound"),SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE},{named(&f.ir,"scalar"),SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE}};CHECK(pipeline_build_roots(&f,&a,ab,2)&&pipeline_build_roots(&f,&b,ba,2));SolWasmScalarOutput oa,ob;sol_wasm_scalar_output_init(&oa);sol_wasm_scalar_output_init(&ob);SolWasmScalarBuildRequest ra={&a.l,directory,NULL},rb={&b.l,directory,NULL};CHECK(sol_wasm_scalar_build_scalar(&ra,&oa,&f.d)==SOL_WASM_SCALAR_OK&&sol_wasm_scalar_build_scalar(&rb,&ob,&f.d)==SOL_WASM_SCALAR_OK);CHECK(export_shape(&oa.bytes,NULL)&&export_shape(&ob.bytes,NULL));CHECK(oa.bytes.count==ob.bytes.count&&!memcmp(oa.bytes.bytes,ob.bytes.bytes,oa.bytes.count));sol_wasm_scalar_output_free(&oa);sol_wasm_scalar_output_free(&ob);pipeline_free(&a);pipeline_free(&b);fixture_free(&f);}
static void test_provenance_limit(void){char directory[512];snprintf(directory,sizeof directory,"%s/tests/conformance/p42_scalar_add_overflow",SOL_TEST_SOURCE_DIR);Fixture f;Pipeline p;fixture_init(&f);pipeline_init(&p);CHECK(fixture_load(&f,directory)&&pipeline_build(&f,&p,"launch",SOL_MIR_PROGRAM_ROOT_ENTRY));SolWasmScalarLimits limits=sol_wasm_scalar_default_limits();limits.max_provenance_records=2;SolWasmScalarOutput o;sol_wasm_scalar_output_init(&o);SolWasmScalarBuildRequest r={&p.l,directory,&limits};CHECK(sol_wasm_scalar_build_scalar(&r,&o,&f.d)==SOL_WASM_SCALAR_RESOURCE_EXHAUSTED&&o.bytes.bytes==NULL&&o.bytes.count==0);sol_wasm_scalar_output_free(&o);pipeline_free(&p);fixture_free(&f);}
static void test_call_catalog(const char*leaf,bool cycle,size_t depth,int64_t expected){
    char directory[512]; snprintf(directory,sizeof directory,"%s/tests/conformance/%s",SOL_TEST_SOURCE_DIR,leaf);
    Fixture f; Pipeline p; fixture_init(&f); pipeline_init(&p);
    CHECK(fixture_load(&f,directory)); CHECK(pipeline_build(&f,&p,"launch",SOL_MIR_PROGRAM_ROOT_ENTRY));
    if(p.l.authentication){
        SolWasmScalarBuildRequest r={&p.l,directory,NULL}; SolWasmScalarTestCallCatalogEntry rows[8]; SolWasmScalarTestCallCatalog summary;
        CHECK(sol_wasm_scalar_test_call_catalog(&r,rows,8,&summary)==SOL_WASM_SCALAR_OK);
        CHECK(summary.calls==p.n.call_count&&summary.edges==p.n.call_count&&summary.work_bytes>0&&summary.has_cycle==cycle&&summary.longest_chain==(cycle?0:depth));
        for(size_t i=0;i<summary.calls&&i<8;i++){
            const SolMirRuntimeCall*c=&p.n.calls[i]; const SolMirMaterializedTerminator*t=&p.c.materialization.blocks[c->block].terminator;
            bool self=cycle&&c->internal<p.c.linkage.callable_count&&p.c.linkage.callables[c->internal].instance==c->image;
            CHECK(rows[i].call==i&&rows[i].caller_image==c->image&&rows[i].caller_block==c->block&&rows[i].callee_callable==c->internal&&rows[i].signature==c->signature&&rows[i].operand_count==c->operands.count&&rows[i].result_class==p.n.signatures[c->signature].result_class&&rows[i].normal_edge==t->normal_edge&&rows[i].failure_edge==t->failure_edge&&rows[i].failure_site==c->failure_site&&rows[i].cyclic==self&&rows[i].chain_depth<=(cycle?0:depth));
            for(size_t q=0;q<c->operands.count;q++){SolMirMaterializedTemporaryId temporary=SOL_MIR_MATERIALIZED_NONE;CHECK(sol_wasm_scalar_test_call_catalog_operand(&r,i,q,&temporary)&&temporary==p.n.operands[c->operands.offset+q].value.id&&temporary==p.c.materialization.call_arguments[t->arguments.offset+q].temporary);}
        }
        SolWasmScalarOutput o; sol_wasm_scalar_output_init(&o); o.bytes.bytes=malloc(1); o.bytes.count=o.bytes.bytes?1:0;
        if(!cycle){
            CHECK(sol_wasm_scalar_build_scalar(&r,&o,&f.d)==SOL_WASM_SCALAR_OK);
            if(p.n.entry_count==1) CHECK(export_shape(&o.bytes,p.n.entries[0].symbol.bytes));
            if(p.n.entry_count==1) CHECK(invoke_named(&o.bytes,p.n.entries[0].symbol.bytes,expected,0,0));
        }else CHECK(sol_wasm_scalar_build_scalar(&r,&o,&f.d)==SOL_WASM_SCALAR_UNSUPPORTED_CLOSURE&&o.bytes.bytes==NULL&&o.bytes.count==0);
        sol_wasm_scalar_output_free(&o);
        SolWasmScalarLimits limits=sol_wasm_scalar_default_limits(); limits.max_work_bytes=summary.work_bytes; r.limits=&limits;
        sol_wasm_scalar_output_init(&o);
        CHECK(sol_wasm_scalar_build_scalar(&r,&o,&f.d)==(cycle?SOL_WASM_SCALAR_UNSUPPORTED_CLOSURE:SOL_WASM_SCALAR_RESOURCE_EXHAUSTED));
        sol_wasm_scalar_output_free(&o);
        if(summary.work_bytes>0){limits.max_work_bytes=summary.work_bytes-1;sol_wasm_scalar_output_init(&o);CHECK(sol_wasm_scalar_build_scalar(&r,&o,&f.d)==SOL_WASM_SCALAR_RESOURCE_EXHAUSTED&&o.bytes.count==0);sol_wasm_scalar_output_free(&o);}
        size_t attempt=sol_wasm_scalar_test_allocation_attempts(); sol_wasm_scalar_test_fail_allocation_after(attempt+1); r.limits=NULL; sol_wasm_scalar_output_init(&o);
        CHECK(sol_wasm_scalar_build_scalar(&r,&o,&f.d)==SOL_WASM_SCALAR_ALLOCATION_FAILED&&o.bytes.count==0); sol_wasm_scalar_test_fail_allocation_after(0); sol_wasm_scalar_output_free(&o);
    }
    pipeline_free(&p); fixture_free(&f);
}

static bool failure_record_for_site(const Pipeline*p,const Provenance*records,size_t count,size_t site,size_t*record){if(site>=p->n.failure_site_count)return false;const SolMirRuntimeFailureSite*f=&p->n.failure_sites[site];const char*symbol=NULL;for(size_t i=0;i<p->c.linkage.callable_count;i++)if(p->c.linkage.callables[i].instance==f->owner){if(symbol!=NULL)return false;symbol=p->c.linkage.callables[i].symbol.bytes;}size_t found=0;if(symbol==NULL)return false;for(size_t i=0;i<count;i++){const Provenance*r=&records[i];if(r->tag!=3||r->kind!=f->origin_kind||r->path_length!=8||memcmp(r->path,"main.sol",8)||r->start!=f->source.start||r->end!=f->source.end||r->symbol_length!=strlen(symbol)||memcmp(r->symbol,symbol,r->symbol_length))continue;*record=i;++found;}return found==1;}
static SolMirLinkageCallableId linked_named(const Fixture*f,const Pipeline*p,const char*name){SolIrCallableId ir=named(&f->ir,name);for(size_t i=0;i<p->c.linkage.callable_count;i++){SolMirPlanInstanceId image=p->c.linkage.callables[i].instance;if(image<p->c.plan.instance_count&&p->c.plan.instances[image].callable==ir)return i;}return SOL_MIR_LINKAGE_NONE;}
static bool failure_for_callable(const Pipeline*p,SolMirLinkageCallableId callable,SolMirOperationOpcode opcode,size_t*instruction,size_t*site){if(callable>=p->c.linkage.callable_count)return false;SolMirPlanInstanceId image=p->c.linkage.callables[callable].instance;for(size_t i=0;i<p->c.materialization.instruction_count;i++){const SolMirOperationArithmeticPlan*plan=plan_at(p,i);if(plan==NULL||plan->image!=image||plan->opcode!=opcode)continue;const SolMirRuntimeLoweredImageInstruction*row=&p->l.image_instructions[i];if(row->failure_site>=p->n.failure_site_count)return false;const SolMirRuntimeFailureSite*f=&p->n.failure_sites[row->failure_site];if(f->origin_kind!=SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_ARITHMETIC||f->owner!=image||f->instruction!=i)return false;*instruction=i;*site=row->failure_site;return true;}return false;}
static bool scalar_failure_call_cleanup(const Pipeline*p,SolMirLinkageCallableId caller,bool require_actions){const SolMirMaterialization*m=&p->c.materialization;if(caller>=p->c.linkage.callable_count)return false;SolMirPlanInstanceId image=p->c.linkage.callables[caller].instance;bool found=false;for(size_t i=0;i<p->n.call_count;i++){const SolMirRuntimeCall*call=&p->n.calls[i];if(call->image!=image)continue;if(call->block>=p->l.image_terminator_count||call->block>=m->block_count)return false;const SolMirMaterializedTerminator*term=&m->blocks[call->block].terminator;const SolMirRuntimeLoweredImageTerminator*row=&p->l.image_terminators[call->block];if(term->kind!=SOL_MIR_TERM_INVOKE||term->normal_edge>=m->edge_count||term->failure_edge>=m->edge_count||row->cleanup_event>=p->q.event_count)return false;const SolMirRuntimeCleanupEvent*event=&p->q.events[row->cleanup_event];const SolMirRuntimeCleanupTransition*normal=NULL,*failure=NULL;for(size_t q=0;q<event->transitions.count;q++){const SolMirRuntimeCleanupTransition*t=&p->q.transitions[event->transitions.offset+q];if(t->edge_role==SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_NORMAL)normal=t;else if(t->edge_role==SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE)failure=t;else return false;}size_t target=m->edges[term->failure_edge].block;if(normal==NULL||failure==NULL||normal->outcome!=SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL||failure->outcome!=SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE||target>=m->block_count||m->blocks[target].terminator.kind!=SOL_MIR_TERM_RESUME_FAILURE||m->edges[term->failure_edge].arguments.count!=0||failure->actions.offset>p->q.action_count||failure->actions.count>p->q.action_count-failure->actions.offset||(require_actions&&failure->actions.count==0))return false;for(size_t q=0;q<failure->actions.count;q++){const SolMirRuntimeCleanupAction*a=&p->q.actions[failure->actions.offset+q];if((a->flags&SOL_MIR_RUNTIME_CLEANUP_ACTION_NORMAL_ONLY)!=0)return false;switch(a->kind){case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_TEMPORARY:case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE:case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER:case SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_SCOPE:case SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_REGION:case SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE:break;default:return false;}}found=true;}return found;}
static bool invoke_reset_instance(const SolWasmBackendBytes *bytes, const char *bad_name,
    const char *good_name, int64_t first_value, int32_t code, int32_t site) {
    wasm_engine_t *engine = wasm_engine_new();
    wasm_store_t *store = engine ? wasm_store_new(engine) : NULL;
    wasm_byte_vec_t input = {bytes->count, (wasm_byte_t *)bytes->bytes};
    wasm_module_t *module = store ? wasm_module_new(store, &input) : NULL;
    wasm_exporttype_vec_t types; wasm_extern_vec_t exports = WASM_EMPTY_VEC, none = WASM_EMPTY_VEC;
    wasm_instance_t *instance = NULL; wasm_trap_t *trap = NULL;
    size_t bad_index = SIZE_MAX, good_index = SIZE_MAX, code_index = SIZE_MAX, site_index = SIZE_MAX;
    bool ok = module != NULL;
    if (ok) {
        wasm_module_exports(module, &types);
        for (size_t i = 0; i < types.size; ++i) {
            const wasm_name_t *name = wasm_exporttype_name(types.data[i]);
            wasm_externkind_t kind = wasm_externtype_kind(wasm_exporttype_type(types.data[i]));
            if (kind == WASM_EXTERN_FUNC) {
                if (name_equal(name, bad_name)) bad_index = i;
                if (name_equal(name, good_name)) good_index = i;
            } else if (name_equal(name, SOL_WASM_SCALAR_FAILURE_CODE_EXPORT) && kind == WASM_EXTERN_GLOBAL) code_index = i;
            else if (name_equal(name, SOL_WASM_SCALAR_FAILURE_SITE_EXPORT) && kind == WASM_EXTERN_GLOBAL) site_index = i;
        }
        ok = bad_index != SIZE_MAX && good_index != SIZE_MAX && code_index != SIZE_MAX && site_index != SIZE_MAX;
        wasm_exporttype_vec_delete(&types);
    }
    if (ok) instance = wasm_instance_new(store, module, &none, &trap);
    if (trap) { wasm_trap_delete(trap); trap = NULL; ok = false; }
    if (ok && instance) {
        wasm_instance_exports(instance, &exports);
        wasm_func_t *bad = wasm_extern_as_func(exports.data[bad_index]);
        wasm_func_t *good = wasm_extern_as_func(exports.data[good_index]);
        wasm_global_t *failure_code = wasm_extern_as_global(exports.data[code_index]);
        wasm_global_t *failure_site = wasm_extern_as_global(exports.data[site_index]);
        wasm_val_t result[1], first_code, first_site, second_code, second_site;
        wasm_val_vec_t arguments = WASM_EMPTY_VEC, results = WASM_ARRAY_VEC(result);
        ok = bad && good && failure_code && failure_site;
        if (ok) trap = wasm_func_call(bad, &arguments, &results);
        if (trap) { wasm_trap_delete(trap); trap = NULL; ok = false; }
        if (ok) {
            wasm_global_get(failure_code, &first_code); wasm_global_get(failure_site, &first_site);
            ok = result[0].kind == WASM_I64 && result[0].of.i64 == first_value
                && first_code.kind == WASM_I32 && first_code.of.i32 == code
                && first_site.kind == WASM_I32 && first_site.of.i32 == site;
        }
        if (ok) {
            wasm_val_t stale_code = {.kind = WASM_I32, .of.i32 = 99};
            wasm_val_t stale_site = {.kind = WASM_I32, .of.i32 = 101};
            wasm_global_set(failure_code, &stale_code); wasm_global_set(failure_site, &stale_site);
            trap = wasm_func_call(good, &arguments, &results);
        }
        if (trap) { wasm_trap_delete(trap); trap = NULL; ok = false; }
        if (ok) {
            wasm_global_get(failure_code, &second_code); wasm_global_get(failure_site, &second_site);
            ok = result[0].kind == WASM_I64 && result[0].of.i64 == 37
                && second_code.kind == WASM_I32 && second_code.of.i32 == 0
                && second_site.kind == WASM_I32 && second_site.of.i32 == 0;
        }
    }
    wasm_extern_vec_delete(&exports); if (instance) wasm_instance_delete(instance);
    if (module) wasm_module_delete(module); if (store) wasm_store_delete(store);
    if (engine) wasm_engine_delete(engine); return ok;
}
static bool scalar_failure_all_call_cleanup(const Pipeline*p){if(p->n.call_count==0)return false;for(size_t i=0;i<p->n.call_count;i++){SolMirLinkageCallableId caller=SOL_MIR_LINKAGE_NONE;for(size_t q=0;q<p->c.linkage.callable_count;q++)if(p->c.linkage.callables[q].instance==p->n.calls[i].image){if(caller!=SOL_MIR_LINKAGE_NONE)return false;caller=q;}if(caller==SOL_MIR_LINKAGE_NONE||!scalar_failure_call_cleanup(p,caller,false))return false;}return true;}
static bool scalar_failure_arithmetic_cleanup(const Pipeline*p,size_t instruction){if(instruction>=p->l.image_instruction_count)return false;const SolMirRuntimeLoweredImageInstruction*row=&p->l.image_instructions[instruction];if(row->cleanup_event>=p->q.event_count)return false;const SolMirRuntimeCleanupEvent*event=&p->q.events[row->cleanup_event];const SolMirRuntimeCleanupTransition*failure=NULL;for(size_t i=0;i<event->transitions.count;i++){const SolMirRuntimeCleanupTransition*t=&p->q.transitions[event->transitions.offset+i];if(t->outcome==SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE){if(failure)return false;failure=t;}}if(failure==NULL||failure->actions.count==0||failure->actions.offset>p->q.action_count||failure->actions.count>p->q.action_count-failure->actions.offset)return false;for(size_t i=0;i<failure->actions.count;i++){const SolMirRuntimeCleanupAction*a=&p->q.actions[failure->actions.offset+i];if((a->flags&SOL_MIR_RUNTIME_CLEANUP_ACTION_NORMAL_ONLY)!=0)return false;if(i+1==failure->actions.count){if(a->kind!=SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE)return false;continue;}switch(a->kind){case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_TEMPORARY:case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE:case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER:case SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_SCOPE:case SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_REGION:break;default:return false;}}return true;}
static void test_call_failure_fixture(void){struct {const char*leaf;SolMirOperationOpcode opcode;int32_t code;bool nested;} cases[]={{"p42_scalar_call_leaf_overflow",SOL_MIR_OPERATION_I64_ADD,2,false},{"p42_scalar_call_leaf_divzero",SOL_MIR_OPERATION_I64_DIV,3,false},{"p42_scalar_call_nested_overflow",SOL_MIR_OPERATION_I64_ADD,2,true},{"p42_scalar_call_nested_divzero",SOL_MIR_OPERATION_I64_DIV,3,true}};for(size_t c=0;c<sizeof cases/sizeof*cases;c++){char directory[512];snprintf(directory,sizeof directory,"%s/tests/conformance/%s",SOL_TEST_SOURCE_DIR,cases[c].leaf);Fixture f;Pipeline p;SolWasmScalarOutput o;fixture_init(&f);pipeline_init(&p);sol_wasm_scalar_output_init(&o);CHECK(fixture_load(&f,directory));CHECK(pipeline_build(&f,&p,"launch",SOL_MIR_PROGRAM_ROOT_ENTRY));SolMirLinkageCallableId leaf=linked_named(&f,&p,"leaf");size_t instruction=0,site=0,record=0;CHECK(leaf!=SOL_MIR_LINKAGE_NONE&&failure_for_callable(&p,leaf,cases[c].opcode,&instruction,&site));CHECK(scalar_failure_all_call_cleanup(&p));CHECK(scalar_failure_arithmetic_cleanup(&p,instruction));if(cases[c].nested){CHECK(p.n.call_count==2);size_t resumes=0;for(size_t i=0;i<p.c.materialization.block_count;i++)resumes+=p.c.materialization.blocks[i].terminator.kind==SOL_MIR_TERM_RESUME_FAILURE;CHECK(resumes==2);}SolWasmScalarBuildRequest r={&p.l,directory,NULL};CHECK(sol_wasm_scalar_build_scalar(&r,&o,&f.d)==SOL_WASM_SCALAR_OK);Provenance records[32];size_t count=0;CHECK(provenance_decode(&o.bytes,records,32,&count));CHECK(failure_record_for_site(&p,records,count,site,&record));if(record<count)CHECK(invoke_named(&o.bytes,p.n.entries[0].symbol.bytes,0,cases[c].code,(int32_t)(record+1)));sol_wasm_scalar_output_free(&o);pipeline_free(&p);fixture_free(&f);}}
static void test_call_failure_reset(void){char directory[512];snprintf(directory,sizeof directory,"%s/tests/conformance/p42_scalar_call_reset",SOL_TEST_SOURCE_DIR);Fixture f;Pipeline p;SolWasmScalarOutput o;fixture_init(&f);pipeline_init(&p);sol_wasm_scalar_output_init(&o);CHECK(fixture_load(&f,directory));CHECK(pipeline_build(&f,&p,"launch",SOL_MIR_PROGRAM_ROOT_ENTRY));SolWasmScalarBuildRequest r={&p.l,directory,NULL};CHECK(sol_wasm_scalar_build_scalar(&r,&o,&f.d)==SOL_WASM_SCALAR_OK);CHECK(p.n.entry_count==1&&invoke_named(&o.bytes,p.n.entries[0].symbol.bytes,37,0,0));CHECK(p.n.entry_count==1&&invoke_reset_instance(&o.bytes,p.n.entries[0].symbol.bytes,p.n.entries[0].symbol.bytes,37,0,0));sol_wasm_scalar_output_free(&o);pipeline_free(&p);fixture_free(&f);}
static bool write_chain_source(const char*path,size_t edges){FILE*s=fopen(path,"w");if(s==NULL||fprintf(s,"module conformance.p42_scalar_chain\n\n")<0){if(s)fclose(s);return false;}for(size_t i=0;i<=edges;i++){int written=i==edges?fprintf(s,"function f%zu() -> Int64 effects { pure } { return 1 }\n\n",i):fprintf(s,i==0?"@entry\npublic function f%zu() -> Int64 effects { pure } { return f%zu() }\n\n":"function f%zu() -> Int64 effects { pure } { return f%zu() }\n\n",i,i+1);if(written<0){fclose(s);return false;}}return fclose(s)==0;}
static void test_call_depth_boundary(size_t edges,SolWasmScalarResult expected){Fixture f;Pipeline p;SolWasmScalarOutput o;fixture_init(&f);pipeline_init(&p);sol_wasm_scalar_output_init(&o);char*directory=test_artifact_root(edges==64?"chain64":"chain65");char*source=directory?joined_path(directory,"main.sol"):NULL;CHECK(directory&&source&&write_chain_source(source,edges));if(directory&&source&&fixture_load(&f,directory)){CHECK(pipeline_build(&f,&p,"f0",SOL_MIR_PROGRAM_ROOT_ENTRY));SolWasmScalarBuildRequest r={&p.l,directory,NULL};SolWasmScalarTestCallCatalog summary;CHECK(sol_wasm_scalar_test_call_catalog(&r,NULL,0,&summary)==SOL_WASM_SCALAR_OK&&summary.calls==edges&&summary.edges==edges&&!summary.has_cycle&&summary.longest_chain==edges);CHECK(sol_wasm_scalar_build_scalar(&r,&o,&f.d)==expected);if(expected==SOL_WASM_SCALAR_OK)CHECK(p.n.entry_count==1&&invoke_named(&o.bytes,p.n.entries[0].symbol.bytes,1,0,0));else CHECK(o.bytes.bytes==NULL&&o.bytes.count==0);}sol_wasm_scalar_output_free(&o);pipeline_free(&p);fixture_free(&f);if(source)(void)unlink(source);if(directory)(void)rmdir(directory);free(source);free(directory);}
static void test_call_root_order(void){char directory[512];snprintf(directory,sizeof directory,"%s/tests/conformance/p42_scalar_call_nested",SOL_TEST_SOURCE_DIR);Fixture f;Pipeline left,right;SolWasmScalarOutput a,b;fixture_init(&f);pipeline_init(&left);pipeline_init(&right);sol_wasm_scalar_output_init(&a);sol_wasm_scalar_output_init(&b);CHECK(fixture_load(&f,directory));SolMirProgramRoot first[]={{named(&f.ir,"launch"),SOL_MIR_PROGRAM_ROOT_ENTRY},{named(&f.ir,"middle"),SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE}},second[]={{named(&f.ir,"middle"),SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE},{named(&f.ir,"launch"),SOL_MIR_PROGRAM_ROOT_ENTRY}};CHECK(pipeline_build_roots(&f,&left,first,2));CHECK(pipeline_build_roots(&f,&right,second,2));SolWasmScalarBuildRequest ra={&left.l,directory,NULL},rb={&right.l,directory,NULL};CHECK(sol_wasm_scalar_build_scalar(&ra,&a,&f.d)==SOL_WASM_SCALAR_OK);CHECK(sol_wasm_scalar_build_scalar(&rb,&b,&f.d)==SOL_WASM_SCALAR_OK);CHECK(left.n.entry_count==1&&right.n.entry_count==1&&export_shape(&a.bytes,left.n.entries[0].symbol.bytes)&&export_shape(&b.bytes,right.n.entries[0].symbol.bytes)&&a.bytes.count==b.bytes.count&&!memcmp(a.bytes.bytes,b.bytes.bytes,a.bytes.count));sol_wasm_scalar_output_free(&a);sol_wasm_scalar_output_free(&b);pipeline_free(&left);pipeline_free(&right);fixture_free(&f);}

static bool output_zero(const SolWasmScalarOutput *output) {
    return output->bytes.bytes == NULL && output->bytes.count == 0
        && !memcmp(&output->usage, &(SolWasmScalarUsage){0}, sizeof output->usage);
}
static bool usage_equal(const SolWasmScalarUsage *left, const SolWasmScalarUsage *right) {
    return left->functions == right->functions && left->blocks == right->blocks
        && left->edges == right->edges && left->values == right->values
        && left->locals == right->locals && left->generated_nodes == right->generated_nodes
        && left->provenance_records == right->provenance_records
        && left->work_bytes == right->work_bytes && left->scratch_bytes == right->scratch_bytes
        && left->owned_bytes == right->owned_bytes && left->output_bytes == right->output_bytes;
}
static void test_limits_and_usage(void) {
    char directory[512]; snprintf(directory, sizeof directory, "%s/tests/conformance/p42_scalar", SOL_TEST_SOURCE_DIR);
    Fixture f; Pipeline p; SolWasmScalarOutput o; fixture_init(&f); pipeline_init(&p); sol_wasm_scalar_output_init(&o);
    CHECK(fixture_load(&f, directory) && pipeline_build(&f, &p, "launch", SOL_MIR_PROGRAM_ROOT_ENTRY));
    SolWasmScalarLimits zero = {0}; SolWasmScalarBuildRequest r = {&p.l, directory, &zero};
    CHECK(sol_wasm_scalar_build_scalar(&r, &o, &f.d) == SOL_WASM_SCALAR_OK);
    CHECK(o.usage.functions == p.c.linkage.callable_count + p.n.entry_count);
    CHECK(o.usage.locals >= p.c.materialization.value_count + p.c.materialization.temporary_count
        + p.c.materialization.local_count);
    SolWasmScalarUsage published = o.usage;
    size_t function_count = o.usage.functions, local_count = o.usage.locals;
    size_t generated_nodes = o.usage.generated_nodes, scratch_bytes = o.usage.scratch_bytes;
    size_t owned_bytes = o.usage.owned_bytes, output_bytes = o.usage.output_bytes;
    sol_wasm_scalar_output_free(&o);
    SolWasmScalarLimits partial = {0}; partial.max_functions = 1; r.limits = &partial;
    CHECK(sol_wasm_scalar_build_scalar(&r, &o, &f.d) == SOL_WASM_SCALAR_INVALID_ARGUMENT && output_zero(&o));
    SolWasmScalarLimits limited = sol_wasm_scalar_default_limits(); limited.max_functions = function_count - 1;
    r.limits = &limited;
    CHECK(sol_wasm_scalar_build_scalar(&r, &o, &f.d) == SOL_WASM_SCALAR_RESOURCE_EXHAUSTED && output_zero(&o));
    limited = sol_wasm_scalar_default_limits(); limited.max_locals = 1; r.limits = &limited;
    CHECK(sol_wasm_scalar_build_scalar(&r, &o, &f.d) == SOL_WASM_SCALAR_RESOURCE_EXHAUSTED && output_zero(&o));
#define CHECK_EXACT_LIMIT(limit_member, usage_member, measured) do { \
    limited = sol_wasm_scalar_default_limits(); limited.limit_member = (measured); r.limits = &limited; \
    CHECK(sol_wasm_scalar_build_scalar(&r, &o, &f.d) == SOL_WASM_SCALAR_OK \
        && o.usage.usage_member == (measured) && usage_equal(&o.usage, &published)); \
    sol_wasm_scalar_output_free(&o); limited.limit_member = (measured) - 1; \
    CHECK(sol_wasm_scalar_build_scalar(&r, &o, &f.d) == SOL_WASM_SCALAR_RESOURCE_EXHAUSTED \
        && output_zero(&o)); sol_wasm_scalar_output_free(&o); \
} while (0)
    CHECK_EXACT_LIMIT(max_functions, functions, function_count);
    CHECK_EXACT_LIMIT(max_locals, locals, local_count);
    CHECK_EXACT_LIMIT(max_generated_nodes, generated_nodes, generated_nodes);
    CHECK_EXACT_LIMIT(max_scratch_bytes, scratch_bytes, scratch_bytes);
    CHECK_EXACT_LIMIT(max_owned_bytes, owned_bytes, owned_bytes);
    CHECK_EXACT_LIMIT(max_output_bytes, output_bytes, output_bytes);
#undef CHECK_EXACT_LIMIT
    sol_wasm_scalar_output_free(&o); pipeline_free(&p); fixture_free(&f);
}
static void test_call_scratch_limit(void) {
    char directory[512]; snprintf(directory, sizeof directory, "%s/tests/conformance/p42_scalar_call_nested_overflow", SOL_TEST_SOURCE_DIR);
    Fixture f; Pipeline p; SolWasmScalarOutput o; fixture_init(&f); pipeline_init(&p); sol_wasm_scalar_output_init(&o);
    CHECK(fixture_load(&f, directory) && pipeline_build(&f, &p, "launch", SOL_MIR_PROGRAM_ROOT_ENTRY));
    SolWasmScalarLimits limits = sol_wasm_scalar_default_limits(); limits.max_scratch_bytes = 1;
    SolWasmScalarBuildRequest r = {&p.l, directory, &limits};
    CHECK(sol_wasm_scalar_build_scalar(&r, &o, &f.d) == SOL_WASM_SCALAR_RESOURCE_EXHAUSTED
        && output_zero(&o));
    sol_wasm_scalar_output_free(&o); pipeline_free(&p); fixture_free(&f);
}
static bool bytes_contain(const SolWasmBackendBytes *bytes, const char *text) {
    size_t length = strlen(text);
    if (length == 0 || length > bytes->count) return false;
    for (size_t i = 0; i <= bytes->count - length; ++i)
        if (!memcmp(bytes->bytes + i, text, length)) return true;
    return false;
}
static void test_final_acceptance(void) {
    static const SolWasmScalarUsage expected = {
        4, 7, 4, 7, 13, 146, 7, 4115, 1703, 1703, 1504};
    static const unsigned char expected_sha[32] = {
        0x6a, 0x75, 0xa3, 0x62, 0x7d, 0xe5, 0x44, 0x74,
        0x18, 0x85, 0xfc, 0xd1, 0x2a, 0xad, 0x0e, 0xf2,
        0x72, 0x37, 0xe9, 0xd7, 0xd9, 0xf3, 0xc6, 0x16,
        0xf8, 0xce, 0xb1, 0x81, 0xf1, 0x15, 0xdd, 0xb4,
    };
    char directory[512]; snprintf(directory, sizeof directory,
        "%s/tests/conformance/p42_scalar_call_nested_overflow", SOL_TEST_SOURCE_DIR);
    Fixture f; Pipeline p; SolWasmScalarOutput first, output; fixture_init(&f); pipeline_init(&p);
    sol_wasm_scalar_output_init(&first); sol_wasm_scalar_output_init(&output);
    CHECK(fixture_load(&f, directory) && pipeline_build(&f, &p, "launch", SOL_MIR_PROGRAM_ROOT_ENTRY));
    SolWasmScalarBuildRequest r = {&p.l, directory, NULL};
    CHECK(sol_wasm_scalar_build_scalar(&r, &first, &f.d) == SOL_WASM_SCALAR_OK);
    SolMirLinkageDigest digest;
    CHECK(usage_equal(&first.usage, &expected)
        && sol_mir_linkage_test_sha256(first.bytes.bytes, first.bytes.count, &digest)
        && !memcmp(digest.bytes, expected_sha, sizeof expected_sha));
    Provenance records[8]; size_t record_count = 0;
    CHECK(provenance_decode(&first.bytes, records, 8, &record_count) && record_count == 7
        && !bytes_contain(&first.bytes, directory));
    SolWasmScalarLimits zero = {0}; r.limits = &zero;
    CHECK(sol_wasm_scalar_build_scalar(&r, &output, &f.d) == SOL_WASM_SCALAR_OK
        && usage_equal(&output.usage, &expected) && output.bytes.count == first.bytes.count
        && !memcmp(output.bytes.bytes, first.bytes.bytes, first.bytes.count));
    sol_wasm_scalar_output_free(&output);
#define FINAL_LIMIT(field, value) do { \
    SolWasmScalarLimits limits = sol_wasm_scalar_default_limits(); limits.field = (value); r.limits = &limits; \
    CHECK(sol_wasm_scalar_build_scalar(&r, &output, &f.d) == SOL_WASM_SCALAR_OK \
        && usage_equal(&output.usage, &expected)); sol_wasm_scalar_output_free(&output); \
    limits.field = (value) - 1; \
    CHECK(sol_wasm_scalar_build_scalar(&r, &output, &f.d) == SOL_WASM_SCALAR_RESOURCE_EXHAUSTED \
        && output_zero(&output)); sol_wasm_scalar_output_free(&output); \
    limits = sol_wasm_scalar_default_limits(); limits.field = 0; \
    CHECK(sol_wasm_scalar_build_scalar(&r, &output, &f.d) == SOL_WASM_SCALAR_INVALID_ARGUMENT \
        && output_zero(&output)); sol_wasm_scalar_output_free(&output); \
} while (0)
    FINAL_LIMIT(max_functions, expected.functions);
    FINAL_LIMIT(max_blocks, expected.blocks);
    FINAL_LIMIT(max_edges, expected.edges);
    FINAL_LIMIT(max_values, expected.values);
    FINAL_LIMIT(max_locals, expected.locals);
    FINAL_LIMIT(max_generated_nodes, expected.generated_nodes);
    FINAL_LIMIT(max_provenance_records, expected.provenance_records);
    FINAL_LIMIT(max_work_bytes, expected.work_bytes);
    FINAL_LIMIT(max_scratch_bytes, expected.scratch_bytes);
    FINAL_LIMIT(max_owned_bytes, expected.owned_bytes);
    FINAL_LIMIT(max_output_bytes, expected.output_bytes);
#undef FINAL_LIMIT
    sol_wasm_scalar_output_free(&first); sol_wasm_scalar_output_free(&output);
    pipeline_free(&p); fixture_free(&f);
}
static void test_allocation_sweep_fixture(const char *leaf) {
    char directory[512]; snprintf(directory, sizeof directory, "%s/tests/conformance/%s", SOL_TEST_SOURCE_DIR, leaf);
    Fixture f; Pipeline p; SolWasmScalarOutput o; fixture_init(&f); pipeline_init(&p); sol_wasm_scalar_output_init(&o);
    CHECK(fixture_load(&f, directory) && pipeline_build(&f, &p, "launch", SOL_MIR_PROGRAM_ROOT_ENTRY));
    SolWasmScalarBuildRequest r = {&p.l, directory, NULL}; size_t before = sol_wasm_scalar_test_allocation_attempts();
    CHECK(sol_wasm_scalar_build_scalar(&r, &o, &f.d) == SOL_WASM_SCALAR_OK); size_t count = sol_wasm_scalar_test_allocation_attempts() - before;
    sol_wasm_scalar_output_free(&o);
    for (size_t nth = 1; nth <= count; ++nth) {
        size_t start = sol_wasm_scalar_test_allocation_attempts();
        sol_wasm_scalar_test_fail_allocation_after(start + nth);
        CHECK(sol_wasm_scalar_build_scalar(&r, &o, &f.d) == SOL_WASM_SCALAR_ALLOCATION_FAILED && output_zero(&o));
        sol_wasm_scalar_test_fail_allocation_after(0); sol_wasm_scalar_output_free(&o);
    }
    pipeline_free(&p); fixture_free(&f);
}
static void test_allocation_sweep(void) {
    test_allocation_sweep_fixture("p42_scalar");
    test_allocation_sweep_fixture("p42_scalar_call_nested_overflow");
}

int main(void) { SolWasmScalarOutput output;sol_wasm_scalar_output_init(&output);CHECK(sol_wasm_scalar_build_scalar(NULL,&output,NULL)==SOL_WASM_SCALAR_INVALID_ARGUMENT);CHECK(sol_wasm_scalar_validate_scalar(NULL)==SOL_WASM_SCALAR_INVALID_ARGUMENT);SolWasmBackendBytes malformed={(uint8_t*)"x",1};CHECK(sol_wasm_scalar_validate_scalar(&malformed)==SOL_WASM_SCALAR_WASMTIME_VALIDATION_FAILED);CHECK(sol_wasm_scalar_test_parallel_moves());CHECK(sol_wasm_scalar_test_rejects_capture_snapshot());CHECK(sol_wasm_scalar_test_rejects_resume_failure());CHECK(sol_wasm_scalar_test_size_add_overflow());sol_wasm_scalar_output_free(&output);test_limits_and_usage();test_call_scratch_limit();test_final_acceptance();test_allocation_sweep();test_fixture("p42_scalar","launch",SOL_MIR_PROGRAM_ROOT_ENTRY,29);test_fixture("p42_scalar_zero_loop","launch",SOL_MIR_PROGRAM_ROOT_ENTRY,31);test_fixture("p42_scalar_checked","launch",SOL_MIR_PROGRAM_ROOT_ENTRY,42);test_fixture("p42_scalar_compound","launch",SOL_MIR_PROGRAM_ROOT_ENTRY,42);test_fixture("p42_scalar_sub_success","launch",SOL_MIR_PROGRAM_ROOT_ENTRY,-5);test_fixture("p42_scalar_div_success","launch",SOL_MIR_PROGRAM_ROOT_ENTRY,-4);test_fixture("p42_scalar_rem_success","launch",SOL_MIR_PROGRAM_ROOT_ENTRY,-1);test_fixture("p42_scalar_mul_success","launch",SOL_MIR_PROGRAM_ROOT_ENTRY,0);test_fixture("p42_scalar_mul_signed","launch",SOL_MIR_PROGRAM_ROOT_ENTRY,-12);test_fixture("p42_scalar_neg_success","launch",SOL_MIR_PROGRAM_ROOT_ENTRY,-7);test_fixture("p42_scalar_compound_div","launch",SOL_MIR_PROGRAM_ROOT_ENTRY,4);test_fixture("p42_scalar_internal","scalar",SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE,0);test_rejections();test_compound_rejection();test_compound_overflow();test_arithmetic_failure("p42_scalar_add_overflow",SOL_MIR_OPERATION_I64_ADD,false,2);test_arithmetic_failure("p42_scalar_sub_overflow",SOL_MIR_OPERATION_I64_SUB,false,2);test_arithmetic_failure("p42_scalar_mul_overflow",SOL_MIR_OPERATION_I64_MUL,false,2);test_arithmetic_failure("p42_scalar_neg_overflow",SOL_MIR_OPERATION_I64_NEG,false,2);test_arithmetic_failure("p42_scalar_divzero",SOL_MIR_OPERATION_I64_DIV,false,3);test_arithmetic_failure("p42_scalar_remzero",SOL_MIR_OPERATION_I64_REM,false,3);test_arithmetic_failure("p42_scalar_div_overflow",SOL_MIR_OPERATION_I64_DIV,false,2);test_arithmetic_failure("p42_scalar_rem_overflow",SOL_MIR_OPERATION_I64_REM,false,2);test_arithmetic_failure("p42_scalar_compound_divzero",SOL_MIR_OPERATION_I64_DIV,true,3);test_rejected_fixture("p42_scalar_panic");test_call_catalog("p42_scalar_call",false,1,7);test_call_catalog("p42_scalar_call_args",false,1,5);test_call_catalog("p42_scalar_call_nested",false,2,19);test_call_catalog("p42_scalar_call_unit",false,1,0);test_call_catalog("p42_scalar_call_repeated",false,1,23);test_call_catalog("p42_scalar_call_cycle",true,0,0);test_provenance_limit();test_relocated_canonicality();test_internal_root_order();test_call_failure_fixture();test_call_failure_reset();test_call_depth_boundary(64,SOL_WASM_SCALAR_OK);test_call_depth_boundary(65,SOL_WASM_SCALAR_UNSUPPORTED_CLOSURE);test_call_root_order();return failures==0?0:1;}
