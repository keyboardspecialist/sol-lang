#define SOL_MIR_PLAN_TEST_HOOKS 1
#include "sol/mir_runtime_lowered_program.h"
#include "sol/package.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool sol_mir_materialization_validate_concrete(
    const SolMirMaterialization *, SolDiagnostics *);

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)
typedef struct { SolDiagnostics d; SolHirModule h; SolTypeTable t; SolEffectTable e; SolContractTable k; SolIr ir; SolPackage p; } Fixture;
typedef struct { SolMirConcreteProgram concrete; SolMirRuntimeConventions conventions; SolMirRuntimeValues values; SolMirRuntimeCleanup cleanup; SolMirRuntimeHostAbi host; SolMirRuntimeHandlerAbi handlers; SolMirRuntimeLoweredProgram lowered; } Pipeline;
static SolIrCallableId find(const SolIr *ir,const char*n,SolIrCallableKind k) { for(size_t i=0;i<ir->callable_count;i++)if(ir->callables[i].kind==k&&!strcmp(ir->callables[i].name,n))return i;return SOL_IR_NONE; }
static bool setup_at(Fixture*f,const char*directory) { memset(f,0,sizeof *f);sol_package_init(&f->p);sol_diagnostics_init(&f->d);sol_hir_module_init(&f->h);sol_type_table_init(&f->t);sol_effect_table_init(&f->e);sol_contract_table_init(&f->k);sol_ir_init(&f->ir);char message[256];if(!sol_package_load_directory(&f->p,directory,&f->d,message,sizeof message))return false;SolHirFileScope scopes[8];if(f->p.file_count>8)return false;for(size_t i=0;i<f->p.file_count;i++)scopes[i]=(SolHirFileScope){f->p.files[i].module_name,f->p.files[i].import_start,f->p.files[i].import_count,f->p.files[i].item_start,f->p.files[i].item_count};return sol_hir_lower_scoped(&f->p.source,&f->p.syntax,scopes,f->p.file_count,&f->h,&f->d)&&sol_type_check(&f->p.source,&f->p.syntax,&f->h,&f->t,&f->d)&&sol_effect_check(&f->p.source,&f->p.syntax,&f->h,&f->t,&f->e,&f->d)&&sol_contract_lower(&f->p.source,&f->p.syntax,&f->h,&f->t,&f->e,&f->k,&f->d)&&sol_ir_lower_scoped(&f->p.source,&f->p.syntax,&f->h,&f->t,&f->e,&f->k,f->p.files,f->p.file_count,&f->ir,&f->d); }
static bool setup(Fixture*f) { return setup_at(f,SOL_TEST_SOURCE_DIR "/tests/conformance/e6"); }
static void finish(Fixture*f) { sol_ir_free(&f->ir);sol_contract_table_free(&f->k);sol_effect_table_free(&f->e);sol_type_table_free(&f->t);sol_hir_module_free(&f->h);sol_diagnostics_free(&f->d);sol_package_free(&f->p); }
static void init(Pipeline*p) { memset(p,0,sizeof *p);sol_mir_concrete_program_init(&p->concrete);sol_mir_runtime_conventions_init(&p->conventions);sol_mir_runtime_values_init(&p->values);sol_mir_runtime_cleanup_init(&p->cleanup);sol_mir_runtime_host_abi_init(&p->host);sol_mir_runtime_handler_abi_init(&p->handlers);sol_mir_runtime_lowered_program_init(&p->lowered); }
static void done(Pipeline*p) { sol_mir_runtime_lowered_program_free(&p->lowered);sol_mir_runtime_handler_abi_free(&p->handlers);sol_mir_runtime_host_abi_free(&p->host);sol_mir_runtime_cleanup_free(&p->cleanup);sol_mir_runtime_values_free(&p->values);sol_mir_runtime_conventions_free(&p->conventions);sol_mir_concrete_program_free(&p->concrete); }
static char *rendered(const SolMirRuntimeLoweredProgram *lowered, size_t *length) {
    FILE *stream = tmpfile(); long end; char *bytes;
    sol_mir_runtime_lowered_program_test_reset_render_write_attempts();
    if (!stream || !sol_mir_runtime_lowered_program_render(stream, lowered)
        || sol_mir_runtime_lowered_program_test_render_write_attempts() != 1
        || fflush(stream) != 0 || fseek(stream, 0, SEEK_END) != 0
        || (end = ftell(stream)) < 0 || fseek(stream, 0, SEEK_SET) != 0) {
        if (stream) fclose(stream); return NULL;
    }
    bytes = malloc((size_t)end + 1);
    if (!bytes || fread(bytes, 1, (size_t)end, stream) != (size_t)end) {
        free(bytes); fclose(stream); return NULL;
    }
    bytes[end] = '\0'; fclose(stream); if (length) *length = (size_t)end;
    return bytes;
}
static size_t occurrences(const char *text, const char *needle) {
    size_t count = 0, length = strlen(needle);
    while ((text = strstr(text, needle)) != NULL) { ++count; text += length; }
    return count;
}
static bool render_line_field(const char *line, size_t length, const char *name,
    const char **value, size_t *value_length) {
    size_t name_length = strlen(name), at = 0;
    while (at < length) {
        size_t token = at, end;
        while (at < length && line[at] != ' ') ++at;
        end = at;
        if (end > token + name_length && !memcmp(line + token, name, name_length)
            && line[token + name_length] == '=') {
            *value = line + token + name_length + 1;
            *value_length = end - token - name_length - 1;
            return true;
        }
        while (at < length && line[at] == ' ') ++at;
    }
    return false;
}
static bool render_field_is(const char *line, size_t length, const char *name,
    const char *expected) {
    const char *value; size_t value_length;
    return render_line_field(line, length, name, &value, &value_length)
        && value_length == strlen(expected) && !memcmp(value, expected, value_length);
}
static bool lowercase_hex_key(const char *key, size_t length) {
    if (length != 64) return false;
    for (size_t i = 0; i < length; ++i)
        if (!((key[i] >= '0' && key[i] <= '9')
                || (key[i] >= 'a' && key[i] <= 'f'))) return false;
    return true;
}
static void check_build_work(const Pipeline *p) {
    SolMirRuntimeLoweredProgramTestBuildWork work =
        sol_mir_runtime_lowered_program_test_last_build_work();
    CHECK(work.census_work > 0 && work.draft_work >= work.census_work
        && work.reserved_persistent_work > 0
        && work.actual_work == work.draft_work + work.reserved_persistent_work
        && work.actual_work == p->lowered.usage.build_work
        && work.build_scratch_bytes == p->lowered.usage.build_scratch_bytes);
}
static void reseal(SolMirRuntimeLoweredProgram *);
static bool build_pre_operations(Fixture *, Pipeline *, bool);
static void check_pre_operation_joins(Pipeline *p) {
    size_t rows[7], count = 0, invoke_count = 0, residual_count = 0;
    const SolMirOperations *operations = &p->concrete.operations;
    for (size_t i = 0; i < p->lowered.image_terminator_count; ++i)
        if (p->lowered.image_terminators[i].pre_operation_cleanup_event
            != SOL_MIR_RUNTIME_LOWERED_NONE) {
            CHECK(count < sizeof(rows) / sizeof(*rows));
            if (count < sizeof(rows) / sizeof(*rows)) rows[count++] = i;
        }
    CHECK(count == 7); if (count != 7) return;
    char *text = rendered(&p->lowered, NULL);
    CHECK(text != NULL && occurrences(text, "pre-operation-cleanup-event=")
        == p->lowered.image_terminator_count
        && occurrences(text, "pre-operation-supplemental-site=")
            == p->lowered.image_terminator_count);
    free(text);
    for (size_t i = 0; i < count; ++i) {
        SolMirRuntimeLoweredImageTerminator *row = &p->lowered.image_terminators[rows[i]];
        SolMirRuntimeCleanupEventId event = row->pre_operation_cleanup_event;
        SolMirRuntimeCleanupSupplementalSiteId site = row->pre_operation_supplemental_site;
        SolMirRuntimeCleanupEvent *pre = &p->cleanup.events[event];
        CHECK(event < p->cleanup.event_count && site < p->cleanup.supplemental_site_count
            && p->cleanup.supplemental_sites[site].event == event
            && row->cleanup_event != event
            && pre->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
            && pre->origin == SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT
            && pre->source.file != SOL_MIR_RUNTIME_LOWERED_NONE);
        if (row->kind == SOL_MIR_TERM_INVOKE) {
            CHECK(pre->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_INVOKE_CALLABLE
                && pre->semantic_site != SOL_MIR_RUNTIME_NONE
                && pre->producer == SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CALLABLE_CONSTRUCTION);
            ++invoke_count;
            size_t callable_plans = 0, semantic_plans = 0;
            for (size_t q = 0; q < operations->callable_count; ++q)
                callable_plans += operations->callables[q].semantic_site
                    == pre->semantic_site
                    && operations->callables[q].kind
                        == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION;
            for (size_t q = 0; q < p->lowered.semantic_plan_count; ++q)
                semantic_plans += p->lowered.semantic_plans[q].arena
                    == SOL_MIR_RUNTIME_LOWERED_SEMANTIC_CALLABLE
                    && p->lowered.semantic_plans[q].producer == pre->semantic_site;
            CHECK(callable_plans == 1 && semantic_plans == 1);
            SolMirRuntimeCleanupPhase phase = pre->phase;
            pre->phase = SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION;
            CHECK(!sol_mir_runtime_cleanup_validate(&p->cleanup, NULL));
            pre->phase = phase;
            CHECK(sol_mir_runtime_cleanup_validate(&p->cleanup, NULL));
        } else {
            CHECK(row->kind == SOL_MIR_TERM_PROPAGATE
            && pre->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_PROPAGATE_RESIDUAL
            && pre->semantic_site == SOL_MIR_RUNTIME_NONE
            && pre->producer == SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PROPAGATION_RESIDUAL);
            ++residual_count;
        }

        row->pre_operation_cleanup_event = SOL_MIR_RUNTIME_LOWERED_NONE; reseal(&p->lowered);
        CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
        row->pre_operation_cleanup_event = event; reseal(&p->lowered);
        CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
        row->pre_operation_supplemental_site = SOL_MIR_RUNTIME_LOWERED_NONE; reseal(&p->lowered);
        CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
        row->pre_operation_supplemental_site = site; reseal(&p->lowered);
        CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
        size_t other = rows[(i + 1) % count];
        row->pre_operation_cleanup_event = p->lowered.image_terminators[other].pre_operation_cleanup_event;
        reseal(&p->lowered); CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
        row->pre_operation_cleanup_event = event;
        row->pre_operation_supplemental_site = p->lowered.image_terminators[other].pre_operation_supplemental_site;
        reseal(&p->lowered); CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
        row->pre_operation_supplemental_site = site; reseal(&p->lowered);
        CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
    }
    CHECK(invoke_count == 5 && residual_count == 2);
}
static void check_pre_operation_render_stability(Fixture *fixture) {
    Pipeline first, second; init(&first); init(&second);
    bool built = build_pre_operations(fixture, &first, false)
        && build_pre_operations(fixture, &second, true);
    CHECK(built);
    if (built) {
        check_pre_operation_joins(&first); check_pre_operation_joins(&second);
        size_t first_length = 0, second_length = 0;
        char *first_render = rendered(&first.lowered, &first_length);
        char *second_render = rendered(&second.lowered, &second_length);
        CHECK(first_render && second_render && first_length == second_length
            && !memcmp(first_render, second_render, first_length));
        bool distinct_callable_and_call = false;
        if (first_render) for (const char *line = first_render; *line;) {
            const char *end = strchr(line, '\n'); size_t length = end ? (size_t)(end - line) : strlen(line);
            const char *ordinary, *pre; size_t ordinary_length, pre_length;
            if (render_line_field(line, length, "cleanup-event", &ordinary, &ordinary_length)
                && render_line_field(line, length, "pre-operation-cleanup-event", &pre, &pre_length)
                && pre_length != 4 && memcmp(pre, "none", 4)
                && (ordinary_length != pre_length || memcmp(ordinary, pre, ordinary_length)))
                distinct_callable_and_call = true;
            line = end ? end + 1 : line + length;
        }
        CHECK(distinct_callable_and_call);
        free(first_render); free(second_render);
    }
    done(&first); done(&second);
}
static bool build(Fixture*f,Pipeline*p,const SolMirRuntimeLoweredProgramLimits*l) { SolMirProgramRoot root={find(&f->ir,"launch",SOL_IR_CALLABLE_FUNCTION),SOL_MIR_PROGRAM_ROOT_ENTRY};const char*names[]={"write","get","count","read"};SolIrCallableId imports[4];for(size_t i=0;i<4;i++)imports[i]=find(&f->ir,names[i],SOL_IR_CALLABLE_CAPABILITY);SolMirTargetDescriptor target=sol_mir_target_wasm32();return root.callable!=SOL_IR_NONE&&sol_mir_concrete_program_build(&(SolMirConcreteBuildRequest){&f->ir,&root,1,imports,4,&target,NULL},&p->concrete,&f->d)==SOL_MIR_CONCRETE_BUILD_SUCCEEDED&&sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&p->concrete,NULL},&p->conventions,&f->d)==SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED&&sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&p->conventions,NULL},&p->values,&f->d)==SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED&&sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&p->conventions,&p->values,NULL},&p->cleanup,&f->d)==SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED&&sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&p->conventions,&p->values,&p->cleanup,NULL},&p->host,&f->d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED&&sol_mir_runtime_handler_abi_build(&(SolMirRuntimeHandlerAbiBuildRequest){&p->conventions,&p->values,&p->cleanup,&p->host,NULL},&p->handlers,&f->d)==SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_SUCCEEDED&&sol_mir_runtime_lowered_program_build(&(SolMirRuntimeLoweredProgramBuildRequest){&p->conventions,&p->values,&p->cleanup,&p->host,&p->handlers,l},&p->lowered,&f->d)==SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED; }
static bool build_pre_operations(Fixture*f,Pipeline*p, bool reverse_tests) {
    SolMirProgramRoot roots[8] = {{find(&f->ir,"launch",SOL_IR_CALLABLE_FUNCTION),SOL_MIR_PROGRAM_ROOT_ENTRY}};
    size_t root_count = 1;
    if (reverse_tests) {
        for (size_t i = f->ir.callable_count; i-- != 0;)
            if (f->ir.callables[i].kind == SOL_IR_CALLABLE_TEST)
                roots[root_count++] = (SolMirProgramRoot){i,SOL_MIR_PROGRAM_ROOT_TEST};
    } else for (size_t i = 0; i < f->ir.callable_count; ++i)
        if (f->ir.callables[i].kind == SOL_IR_CALLABLE_TEST)
            roots[root_count++] = (SolMirProgramRoot){i,SOL_MIR_PROGRAM_ROOT_TEST};
    const char*names[]={"write","get","count","read"}; SolIrCallableId imports[4];
    for(size_t i=0;i<4;i++)imports[i]=find(&f->ir,names[i],SOL_IR_CALLABLE_CAPABILITY);
    SolMirTargetDescriptor target=sol_mir_target_wasm32();
    return root_count==5&&roots[0].callable!=SOL_IR_NONE
        &&sol_mir_concrete_program_build(&(SolMirConcreteBuildRequest){&f->ir,roots,root_count,imports,4,&target,NULL},&p->concrete,&f->d)==SOL_MIR_CONCRETE_BUILD_SUCCEEDED
        &&sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&p->concrete,NULL},&p->conventions,&f->d)==SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED
        &&sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&p->conventions,NULL},&p->values,&f->d)==SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED
        &&sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&p->conventions,&p->values,NULL},&p->cleanup,&f->d)==SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED
        &&sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&p->conventions,&p->values,&p->cleanup,NULL},&p->host,&f->d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED
        &&sol_mir_runtime_handler_abi_build(&(SolMirRuntimeHandlerAbiBuildRequest){&p->conventions,&p->values,&p->cleanup,&p->host,NULL},&p->handlers,&f->d)==SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_SUCCEEDED
        &&sol_mir_runtime_lowered_program_build(&(SolMirRuntimeLoweredProgramBuildRequest){&p->conventions,&p->values,&p->cleanup,&p->host,&p->handlers,NULL},&p->lowered,&f->d)==SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED;
}
static bool build_graph(Fixture*f,Pipeline*p) { SolMirProgramRoot root={find(&f->ir,"launch",SOL_IR_CALLABLE_FUNCTION),SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};SolMirTargetDescriptor target=sol_mir_target_wasm32();return root.callable!=SOL_IR_NONE&&sol_mir_concrete_program_build(&(SolMirConcreteBuildRequest){&f->ir,&root,1,NULL,0,&target,NULL},&p->concrete,&f->d)==SOL_MIR_CONCRETE_BUILD_SUCCEEDED&&sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&p->concrete,NULL},&p->conventions,&f->d)==SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED&&sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&p->conventions,NULL},&p->values,&f->d)==SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED&&sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&p->conventions,&p->values,NULL},&p->cleanup,&f->d)==SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED&&sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&p->conventions,&p->values,&p->cleanup,NULL},&p->host,&f->d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED&&sol_mir_runtime_handler_abi_build(&(SolMirRuntimeHandlerAbiBuildRequest){&p->conventions,&p->values,&p->cleanup,&p->host,NULL},&p->handlers,&f->d)==SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_SUCCEEDED&&sol_mir_runtime_lowered_program_build(&(SolMirRuntimeLoweredProgramBuildRequest){&p->conventions,&p->values,&p->cleanup,&p->host,&p->handlers,NULL},&p->lowered,&f->d)==SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED; }
static bool build_named(Fixture*f,Pipeline*p,const char*name) { SolMirProgramRoot root={find(&f->ir,name,SOL_IR_CALLABLE_FUNCTION),SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};SolMirTargetDescriptor target=sol_mir_target_wasm32();return root.callable!=SOL_IR_NONE&&sol_mir_concrete_program_build(&(SolMirConcreteBuildRequest){&f->ir,&root,1,NULL,0,&target,NULL},&p->concrete,&f->d)==SOL_MIR_CONCRETE_BUILD_SUCCEEDED&&sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&p->concrete,NULL},&p->conventions,&f->d)==SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED&&sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&p->conventions,NULL},&p->values,&f->d)==SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED&&sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&p->conventions,&p->values,NULL},&p->cleanup,&f->d)==SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED&&sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&p->conventions,&p->values,&p->cleanup,NULL},&p->host,&f->d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED&&sol_mir_runtime_handler_abi_build(&(SolMirRuntimeHandlerAbiBuildRequest){&p->conventions,&p->values,&p->cleanup,&p->host,NULL},&p->handlers,&f->d)==SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_SUCCEEDED&&sol_mir_runtime_lowered_program_build(&(SolMirRuntimeLoweredProgramBuildRequest){&p->conventions,&p->values,&p->cleanup,&p->host,&p->handlers,NULL},&p->lowered,&f->d)==SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED; }
static bool build_handlers(Fixture*f,Pipeline*p) { const char*names[]={"newest","distinct_roots","one_handler","cleanup_return","cleanup_failure"};SolMirProgramRoot roots[5];for(size_t i=0;i<5;i++)roots[i]=(SolMirProgramRoot){find(&f->ir,names[i],SOL_IR_CALLABLE_FUNCTION),SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};SolIrCallableId imports[]={find(&f->ir,"write",SOL_IR_CALLABLE_CAPABILITY)};SolMirTargetDescriptor target=sol_mir_target_wasm32();for(size_t i=0;i<5;i++)if(roots[i].callable==SOL_IR_NONE)return false;return imports[0]!=SOL_IR_NONE&&sol_mir_concrete_program_build(&(SolMirConcreteBuildRequest){&f->ir,roots,5,imports,1,&target,NULL},&p->concrete,&f->d)==SOL_MIR_CONCRETE_BUILD_SUCCEEDED&&sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&p->concrete,NULL},&p->conventions,&f->d)==SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED&&sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&p->conventions,NULL},&p->values,&f->d)==SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED&&sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&p->conventions,&p->values,NULL},&p->cleanup,&f->d)==SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED&&sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&p->conventions,&p->values,&p->cleanup,NULL},&p->host,&f->d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED&&sol_mir_runtime_handler_abi_build(&(SolMirRuntimeHandlerAbiBuildRequest){&p->conventions,&p->values,&p->cleanup,&p->host,NULL},&p->handlers,&f->d)==SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_SUCCEEDED&&sol_mir_runtime_lowered_program_build(&(SolMirRuntimeLoweredProgramBuildRequest){&p->conventions,&p->values,&p->cleanup,&p->host,&p->handlers,NULL},&p->lowered,&f->d)==SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED; }
static void expect_descriptor(bool ok, SolMirRuntimeLoweredDemandDescriptor actual,
    SolMirRuntimeLoweredRuntimeClass runtime_class, SolMirRuntimeLoweredPlanFamily family, uint32_t facilities) {
    CHECK(ok); if (ok) CHECK(actual.runtime_class == runtime_class && actual.plan_family == family && actual.facilities == facilities);
}
#define EXPECT_IMAGE_INSTRUCTION(kind, runtime_class, family, facilities) do { SolMirRuntimeLoweredDemandDescriptor actual; expect_descriptor(sol_mir_runtime_lowered_program_test_image_instruction_descriptor(kind, &actual), actual, runtime_class, family, facilities); } while (0)
#define EXPECT_IMAGE_TERMINATOR(kind, runtime_class, family, facilities) do { SolMirRuntimeLoweredDemandDescriptor actual; expect_descriptor(sol_mir_runtime_lowered_program_test_image_terminator_descriptor(kind, &actual), actual, runtime_class, family, facilities); } while (0)
#define EXPECT_PREDICATE_INSTRUCTION(kind, runtime_class, family, facilities) do { SolMirRuntimeLoweredDemandDescriptor actual; expect_descriptor(sol_mir_runtime_lowered_program_test_predicate_instruction_descriptor(kind, &actual), actual, runtime_class, family, facilities); } while (0)
#define EXPECT_PREDICATE_TERMINATOR(kind, runtime_class, family, facilities) do { SolMirRuntimeLoweredDemandDescriptor actual; expect_descriptor(sol_mir_runtime_lowered_program_test_predicate_terminator_descriptor(kind, &actual), actual, runtime_class, family, facilities); } while (0)
#define EXPECT_PROVENANCE(kind, runtime_class, family, facilities) do { SolMirRuntimeLoweredDemandDescriptor actual; expect_descriptor(sol_mir_runtime_lowered_program_test_provenance_descriptor(kind, &actual), actual, runtime_class, family, facilities); } while (0)
#define F(name) SOL_MIR_RUNTIME_LOWERED_FACILITY_##name
static void test_vocabulary(void) {
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_CONST_INT64,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,F(RECIPE)|F(VALUE));
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_CONST_BOOL,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,F(RECIPE)|F(VALUE));
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_CONST_TEXT,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,F(RECIPE)|F(VALUE)|F(ALLOCATION));
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_CONST_UNIT,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,F(VALUE));
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_LOAD_COPY,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,F(COPY)|F(VALUE));
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_LOAD_MOVE,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,F(OWNERSHIP)|F(VALUE));
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_LOAD_UPDATE,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,F(VALUE));
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_STORE,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,F(OWNERSHIP));
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_UNARY,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_ARITHMETIC,F(VALUE)|F(CLEANUP));
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_BINARY,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_ARITHMETIC,F(VALUE)|F(CLEANUP)|F(EVENT));
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_COMPOUND_UPDATE,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_ARITHMETIC,F(VALUE)|F(CLEANUP));
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_PATTERN_TEST,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN,F(VALUE));
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_PATTERN_VALUE,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN,F(VALUE));
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_CONSTRUCT,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CONSTRUCT,F(RECIPE)|F(VALUE));
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_CAPTURE_SNAPSHOT,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_SNAPSHOT,F(COPY)|F(VALUE));
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_PARAMETER_LIVE,SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,F(CLEANUP));
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_STORAGE_LIVE,SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,F(CLEANUP));
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_DROP_IF_INITIALIZED,SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP,F(CLEANUP)|F(OWNERSHIP));
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_STORAGE_DEAD,SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,F(CLEANUP));
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_REGION_ENTER,SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,0);
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_REGION_EXIT,SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP,F(CLEANUP));
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_TEMPORARY_INIT,SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,0);
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_TEMPORARY_DROP,SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP,F(CLEANUP)|F(OWNERSHIP));
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_EXPRESSION_RESULT,SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,0);
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_MATCH_ARM,SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,0);
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED,SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP,F(CLEANUP)|F(OWNERSHIP));
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_HANDLER_ENTER,SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER,SOL_MIR_RUNTIME_LOWERED_PLAN_HANDLER,F(HANDLER_FRAME));
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_HANDLER_EXIT,SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER,SOL_MIR_RUNTIME_LOWERED_PLAN_HANDLER,F(HANDLER_FRAME)|F(CLEANUP));
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_SCOPE_ENTER,SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,0);
    EXPECT_IMAGE_INSTRUCTION(SOL_MIR_INST_SCOPE_EXIT,SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP,F(CLEANUP));
    EXPECT_IMAGE_TERMINATOR(SOL_MIR_TERM_GOTO,SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,0);
    EXPECT_IMAGE_TERMINATOR(SOL_MIR_TERM_BRANCH,SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,0);
    EXPECT_IMAGE_TERMINATOR(SOL_MIR_TERM_RETURN,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP,F(CLEANUP));
    EXPECT_IMAGE_TERMINATOR(SOL_MIR_TERM_PANIC,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP,F(CLEANUP)|F(EVENT)|F(FAILURE));
    EXPECT_IMAGE_TERMINATOR(SOL_MIR_TERM_INVOKE,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CALLABLE,F(CALL)|F(SIGNATURE)|F(CLEANUP)|F(EVENT)|F(FAILURE));
    EXPECT_IMAGE_TERMINATOR(SOL_MIR_TERM_RESUME_FAILURE,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP,F(CLEANUP)|F(FAILURE));
    EXPECT_IMAGE_TERMINATOR(SOL_MIR_TERM_UNREACHABLE,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP,F(CLEANUP)|F(FAILURE));
    EXPECT_IMAGE_TERMINATOR(SOL_MIR_TERM_BREAK,SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,0);
    EXPECT_IMAGE_TERMINATOR(SOL_MIR_TERM_CONTINUE,SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,0);
    EXPECT_IMAGE_TERMINATOR(SOL_MIR_TERM_CHECK_REFINED,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE,F(CLEANUP)|F(EVENT)|F(FAILURE));
    EXPECT_IMAGE_TERMINATOR(SOL_MIR_TERM_MATCH_FAILURE,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN,F(CLEANUP)|F(EVENT)|F(FAILURE));
    EXPECT_IMAGE_TERMINATOR(SOL_MIR_TERM_PROPAGATE,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PROPAGATION,F(VALUE)|F(CLEANUP));
    EXPECT_IMAGE_TERMINATOR(SOL_MIR_TERM_CHECK_CONTRACT,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE,F(CLEANUP)|F(EVENT)|F(FAILURE));
    EXPECT_IMAGE_TERMINATOR(SOL_MIR_TERM_CONTRACT_VIOLATION,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP,F(CLEANUP)|F(FAILURE));
    EXPECT_PREDICATE_INSTRUCTION(SOL_MIR_PREDICATE_INST_I64,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,F(VALUE));
    EXPECT_PREDICATE_INSTRUCTION(SOL_MIR_PREDICATE_INST_BOOL,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,F(VALUE));
    EXPECT_PREDICATE_INSTRUCTION(SOL_MIR_PREDICATE_INST_TEXT,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,F(VALUE)|F(ALLOCATION));
    EXPECT_PREDICATE_INSTRUCTION(SOL_MIR_PREDICATE_INST_UNIT,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,F(VALUE));
    EXPECT_PREDICATE_INSTRUCTION(SOL_MIR_PREDICATE_INST_UNARY,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_ARITHMETIC,F(VALUE)|F(CLEANUP));
    EXPECT_PREDICATE_INSTRUCTION(SOL_MIR_PREDICATE_INST_BINARY,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_ARITHMETIC,F(VALUE)|F(CLEANUP));
    EXPECT_PREDICATE_INSTRUCTION(SOL_MIR_PREDICATE_INST_PROJECT,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,F(VALUE));
    EXPECT_PREDICATE_INSTRUCTION(SOL_MIR_PREDICATE_INST_FUNCTION,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CALLABLE,F(VALUE)|F(RECIPE));
    EXPECT_PREDICATE_INSTRUCTION(SOL_MIR_PREDICATE_INST_BOUND_OPERATION,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CALLABLE,F(VALUE)|F(RECIPE));
    EXPECT_PREDICATE_INSTRUCTION(SOL_MIR_PREDICATE_INST_CONSTRUCT,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CONSTRUCT,F(VALUE)|F(RECIPE)|F(ALLOCATION));
    EXPECT_PREDICATE_INSTRUCTION(SOL_MIR_PREDICATE_INST_PATTERN_TEST,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN,F(VALUE));
    EXPECT_PREDICATE_INSTRUCTION(SOL_MIR_PREDICATE_INST_PATTERN_EXTRACT,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN,F(VALUE));
    EXPECT_PREDICATE_TERMINATOR(SOL_MIR_PREDICATE_TERM_RETURN,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE,F(CLEANUP));
    EXPECT_PREDICATE_TERMINATOR(SOL_MIR_PREDICATE_TERM_JUMP,SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,0);
    EXPECT_PREDICATE_TERMINATOR(SOL_MIR_PREDICATE_TERM_BRANCH,SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,0);
    EXPECT_PREDICATE_TERMINATOR(SOL_MIR_PREDICATE_TERM_INVOKE,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CALLABLE,F(CALL)|F(SIGNATURE)|F(CLEANUP)|F(FAILURE));
    EXPECT_PREDICATE_TERMINATOR(SOL_MIR_PREDICATE_TERM_PROPAGATE,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PROPAGATION,F(VALUE)|F(CLEANUP));
    EXPECT_PREDICATE_TERMINATOR(SOL_MIR_PREDICATE_TERM_CHECK_REFINED,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE,F(CLEANUP)|F(FAILURE));
    EXPECT_PREDICATE_TERMINATOR(SOL_MIR_PREDICATE_TERM_FAILURE,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP,F(CLEANUP)|F(FAILURE));
    EXPECT_PROVENANCE(SOL_MIR_OPERATION_PROVENANCE_CONSTRUCT,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CONSTRUCT,F(RECIPE)|F(VALUE));
    EXPECT_PROVENANCE(SOL_MIR_OPERATION_PROVENANCE_PATTERN_TEST,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN,F(VALUE));
    EXPECT_PROVENANCE(SOL_MIR_OPERATION_PROVENANCE_PATTERN_EXTRACTION,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN,F(VALUE));
    EXPECT_PROVENANCE(SOL_MIR_OPERATION_PROVENANCE_ARITHMETIC,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_ARITHMETIC,F(VALUE)|F(CLEANUP));
    EXPECT_PROVENANCE(SOL_MIR_OPERATION_PROVENANCE_PROPAGATION,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PROPAGATION,F(VALUE)|F(CLEANUP));
    EXPECT_PROVENANCE(SOL_MIR_OPERATION_PROVENANCE_SNAPSHOT,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_SNAPSHOT,F(COPY)|F(VALUE));
    EXPECT_PROVENANCE(SOL_MIR_OPERATION_PROVENANCE_PREDICATE,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE,F(VALUE)|F(CLEANUP));
    EXPECT_PROVENANCE(SOL_MIR_OPERATION_PROVENANCE_HANDLER,SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER,SOL_MIR_RUNTIME_LOWERED_PLAN_HANDLER,F(HANDLER_FRAME));
    EXPECT_PROVENANCE(SOL_MIR_OPERATION_PROVENANCE_CALLABLE,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CALLABLE,F(CALL)|F(SIGNATURE)|F(VALUE));
    EXPECT_PROVENANCE(SOL_MIR_OPERATION_PROVENANCE_IMPORT_SNAPSHOT,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_IMPORT_SNAPSHOT,F(COPY)|F(VALUE));
    EXPECT_PROVENANCE(SOL_MIR_OPERATION_PROVENANCE_PREDICATE_BODY,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE,F(VALUE)|F(CLEANUP));
    SolMirRuntimeLoweredDemandDescriptor actual;
    CHECK(!sol_mir_runtime_lowered_program_test_image_instruction_descriptor((SolMirInstructionKind)-1,&actual)); CHECK(!sol_mir_runtime_lowered_program_test_image_instruction_descriptor((SolMirInstructionKind)(SOL_MIR_INST_FUNCTION_VALUE+1),&actual));
    CHECK(!sol_mir_runtime_lowered_program_test_image_terminator_descriptor(SOL_MIR_TERM_INVALID,&actual)); CHECK(!sol_mir_runtime_lowered_program_test_image_terminator_descriptor((SolMirTerminatorKind)(SOL_MIR_TERM_CONTRACT_VIOLATION+1),&actual));
    CHECK(!sol_mir_runtime_lowered_program_test_predicate_instruction_descriptor((SolMirPredicateInstructionKind)-1,&actual)); CHECK(!sol_mir_runtime_lowered_program_test_predicate_instruction_descriptor((SolMirPredicateInstructionKind)(SOL_MIR_PREDICATE_INST_PATTERN_EXTRACT+1),&actual));
    CHECK(!sol_mir_runtime_lowered_program_test_predicate_terminator_descriptor((SolMirPredicateTerminatorKind)-1,&actual)); CHECK(!sol_mir_runtime_lowered_program_test_predicate_terminator_descriptor((SolMirPredicateTerminatorKind)(SOL_MIR_PREDICATE_TERM_FAILURE+1),&actual));
    CHECK(!sol_mir_runtime_lowered_program_test_provenance_descriptor((SolMirOperationProvenanceKind)-1,&actual)); CHECK(!sol_mir_runtime_lowered_program_test_provenance_descriptor((SolMirOperationProvenanceKind)(SOL_MIR_OPERATION_PROVENANCE_PREDICATE_BODY+1),&actual));
}
#undef F
#undef EXPECT_PROVENANCE
#undef EXPECT_PREDICATE_TERMINATOR
#undef EXPECT_PREDICATE_INSTRUCTION
#undef EXPECT_IMAGE_TERMINATOR
#undef EXPECT_IMAGE_INSTRUCTION

static void test_e6_census_and_missing(Pipeline*p) {
    const SolMirMaterialization*m=&p->concrete.materialization;const SolMirOperations*o=&p->concrete.operations;const SolMirRuntimeCleanup*c=&p->cleanup;
    CHECK(o->callable_count == 5 && o->provenance_count == 42);
    size_t bound_imports = 0, bound_calls = 0, bound_recipes[21] = {0};
    for (size_t i = 0; i < o->callable_count; ++i)
        if (o->callables[i].kind == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION
            && o->callables[i].function_recipe < 21)
            ++bound_recipes[o->callables[i].function_recipe];
    for (size_t i = 0; i < p->conventions.import_count; ++i)
        if (p->conventions.imports[i].kind
            == SOL_MIR_RUNTIME_IMPORT_RECIPE_BOUND_ENVIRONMENT) {
            SolMirRecipeId recipe = p->conventions.imports[i].recipe;
            CHECK(recipe < 21 && bound_recipes[recipe] != 0
                && p->lowered.imports[i].state == SOL_MIR_RUNTIME_LOWERED_PRESENT);
            ++bound_imports;
        }
    for (size_t i = 0; i < p->lowered.call_count; ++i)
        bound_calls += p->lowered.calls[i].bound_environment_import
            != SOL_MIR_RUNTIME_LOWERED_NONE;
    CHECK(bound_imports == 4 && bound_calls == 0);
    CHECK(p->lowered.image_instruction_count==m->instruction_count&&p->lowered.image_block_count==m->block_count&&p->lowered.image_edge_count==m->edge_count&&p->lowered.image_terminator_count==m->block_count);CHECK(p->lowered.predicate_body_count==o->predicate_body_count&&p->lowered.predicate_instruction_count==o->predicate_instruction_count&&p->lowered.predicate_block_count==o->predicate_block_count&&p->lowered.predicate_edge_count==o->predicate_edge_count&&p->lowered.predicate_terminator_count==o->predicate_block_count);CHECK(p->lowered.semantic_plan_count==o->access_plan_count+o->constructor_count+o->pattern_test_count+o->pattern_extraction_count+o->propagation_count+o->arithmetic_count+o->snapshot_count+o->callable_count+o->handler_count+o->predicate_count+o->import_snapshot_count&&p->lowered.usage.provenance_records==o->provenance_count&&p->lowered.call_count==p->conventions.call_count&&p->lowered.signature_count==p->conventions.signature_count&&p->lowered.import_count==p->conventions.import_count&&p->lowered.recipe_count==p->values.recipe_operation_count&&p->lowered.value_plan_count==p->values.recipe_operation_count);size_t distinct_grants=0;for(size_t i=0;i<p->host.requirement_count;i++){bool prior=false;for(size_t j=0;j<i;j++)if(p->host.requirements[j].entry==p->host.requirements[i].entry&&p->host.requirements[j].root==p->host.requirements[i].root&&p->host.requirements[j].operation==p->host.requirements[i].operation)prior=true;if(!prior)++distinct_grants;}CHECK(p->lowered.cleanup_failure_count==c->event_count+c->action_count+c->transition_count+c->supplemental_site_count+c->drop_path_count&&p->lowered.host_requirement_count==p->host.requirement_count&&p->lowered.host_grant_count==distinct_grants&&p->lowered.host_incidence_count==p->host.requirement_count&&p->lowered.handler_frame_count==p->handlers.frame_count&&p->lowered.handler_marker_count==p->handlers.exit_marker_count&&p->lowered.handler_exit_count==p->handlers.cleanup_exit_count&&p->lowered.usage.erased_loops==m->loop_count&&p->lowered.usage.provenance_records==o->provenance_count&&p->lowered.usage.demanded_semantic_plans==p->lowered.semantic_plan_count&&p->lowered.semantic_plan_count!=p->lowered.usage.provenance_records);for(size_t grant=0;grant<p->lowered.host_grant_count;grant++){const SolMirRuntimeLoweredHostGrant*g=&p->lowered.host_grants[grant];CHECK(g->incidences.offset+g->incidences.count<=p->lowered.host_incidence_count);for(size_t j=g->incidences.offset;j<g->incidences.offset+g->incidences.count;j++){const SolMirRuntimeLoweredHostIncidence*inc=&p->lowered.host_incidences[j];CHECK(inc->grant==grant&&inc->requirement<p->host.requirement_count&&p->host.requirements[inc->requirement].root==g->root&&p->host.requirements[inc->requirement].operation==g->operation);}}bool shared=false;for(size_t i=0;i<p->lowered.host_incidence_count;i++)for(size_t j=i+1;j<p->lowered.host_incidence_count;j++)if(p->host.requirements[p->lowered.host_incidences[i].requirement].root==p->host.requirements[p->lowered.host_incidences[j].requirement].root&&p->host.requirements[p->lowered.host_incidences[i].requirement].operation==p->host.requirements[p->lowered.host_incidences[j].requirement].operation){shared=true;CHECK(p->lowered.host_incidences[i].grant==p->lowered.host_incidences[j].grant);}CHECK(shared);
    if(p->lowered.image_instruction_count){SolMirRuntimeLoweredState s=p->lowered.image_instructions[0].state;p->lowered.image_instructions[0].state=SOL_MIR_RUNTIME_LOWERED_NOT_DEMANDED;CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered,NULL));p->lowered.image_instructions[0].state=s;CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered,NULL));SolMirInstructionKind k=p->lowered.image_instructions[0].kind;p->lowered.image_instructions[0].kind=(SolMirInstructionKind)-1;CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered,NULL));p->lowered.image_instructions[0].kind=k;CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered,NULL));uint32_t facilities=p->lowered.image_instructions[0].facilities;p->lowered.image_instructions[0].facilities^=SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE;CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered,NULL));p->lowered.image_instructions[0].facilities=facilities;CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered,NULL));}if(p->lowered.semantic_plan_count){SolMirRuntimeLoweredPlanFamily family=p->lowered.semantic_plans[0].plan_family;p->lowered.semantic_plans[0].plan_family=SOL_MIR_RUNTIME_LOWERED_PLAN_NONE;CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered,NULL));p->lowered.semantic_plans[0].plan_family=family;CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered,NULL));}
    size_t direct_internal=0,direct_host=0,predicate_producers=0,predicate_invokes=0;for(size_t i=0;i<p->lowered.call_count;i++){const SolMirRuntimeLoweredCall*call=&p->lowered.calls[i];if(call->target_kind==SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL){++direct_internal;CHECK(call->import_id==SOL_MIR_RUNTIME_LOWERED_NONE);}else if(call->target_kind==SOL_MIR_RUNTIME_TARGET_DIRECT_HOST){++direct_host;CHECK(call->import_id<p->lowered.import_count);}else if(call->target_kind==SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE){CHECK(call->import_id==SOL_MIR_RUNTIME_LOWERED_NONE);}}for(size_t i=0;i<p->lowered.predicate_instruction_count;i++)if(p->lowered.predicate_instructions[i].kind==SOL_MIR_PREDICATE_INST_FUNCTION||p->lowered.predicate_instructions[i].kind==SOL_MIR_PREDICATE_INST_BOUND_OPERATION){++predicate_producers;CHECK((p->lowered.predicate_instructions[i].facilities&SOL_MIR_RUNTIME_LOWERED_FACILITY_CALL)==0&&(p->lowered.predicate_instructions[i].facilities&SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE)!=0);}for(size_t i=0;i<p->lowered.predicate_terminator_count;i++)if(p->lowered.predicate_terminators[i].kind==SOL_MIR_PREDICATE_TERM_INVOKE){++predicate_invokes;CHECK(p->lowered.predicate_terminators[i].call<p->lowered.call_count&&(p->lowered.predicate_terminators[i].facilities&SOL_MIR_RUNTIME_LOWERED_FACILITY_CALL)!=0&&(p->lowered.predicate_terminators[i].facilities&SOL_MIR_RUNTIME_LOWERED_FACILITY_SIGNATURE)!=0);}CHECK(p->lowered.call_count>0);uint32_t call_facilities=0;CHECK(sol_mir_runtime_lowered_program_test_call_facilities(SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL,SOL_MIR_RUNTIME_SIGNATURE_INTERNAL,false,&call_facilities)&&call_facilities==(SOL_MIR_RUNTIME_LOWERED_FACILITY_CALL|SOL_MIR_RUNTIME_LOWERED_FACILITY_SIGNATURE));CHECK(sol_mir_runtime_lowered_program_test_call_facilities(SOL_MIR_RUNTIME_TARGET_DIRECT_HOST,SOL_MIR_RUNTIME_SIGNATURE_HOST,true,&call_facilities)&&(call_facilities&SOL_MIR_RUNTIME_LOWERED_FACILITY_IMPORT)!=0);CHECK(sol_mir_runtime_lowered_program_test_call_facilities(SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE,SOL_MIR_RUNTIME_SIGNATURE_FUNCTION_RECIPE,false,&call_facilities)&&(call_facilities&SOL_MIR_RUNTIME_LOWERED_FACILITY_IMPORT)==0);CHECK(!sol_mir_runtime_lowered_program_test_copy_requires_runtime(SOL_MIR_COPY_TRIVIAL));CHECK(sol_mir_runtime_lowered_program_test_copy_requires_runtime(SOL_MIR_COPY_TEXT));CHECK((sol_mir_runtime_lowered_program_test_arithmetic_facilities(0)&SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE)==0);CHECK((sol_mir_runtime_lowered_program_test_arithmetic_facilities(SOL_MIR_OPERATION_FAILURE_DIVISION_BY_ZERO)&SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE)!=0);size_t continuation=SOL_MIR_RUNTIME_LOWERED_NONE;/* P2/P3.3 repair 3865c62 specifies PROPAGATE normal/failure continuations; source contracts currently produce none, so this bounded hook covers the repaired predecessor schema and rejects legacy term.edge. */CHECK(sol_mir_runtime_lowered_program_test_predicate_continuation_ordinal(SOL_MIR_PREDICATE_TERM_PROPAGATE,false,&continuation)&&continuation==0);CHECK(sol_mir_runtime_lowered_program_test_predicate_continuation_ordinal(SOL_MIR_PREDICATE_TERM_PROPAGATE,true,&continuation)&&continuation==1);CHECK(sol_mir_runtime_lowered_program_test_predicate_legacy_edge_rejected(SOL_MIR_PREDICATE_TERM_PROPAGATE));(void)direct_internal;(void)direct_host;(void)predicate_producers;(void)predicate_invokes;
    size_t demanded=0,not_demanded=0,demanded_id=SOL_MIR_RUNTIME_LOWERED_NONE,not_demanded_id=SOL_MIR_RUNTIME_LOWERED_NONE;for(size_t i=0;i<p->lowered.recipe_count;i++){if(p->lowered.recipes[i].state==SOL_MIR_RUNTIME_LOWERED_PRESENT){++demanded;demanded_id=i;}if(p->lowered.recipes[i].state==SOL_MIR_RUNTIME_LOWERED_NOT_DEMANDED){++not_demanded;not_demanded_id=i;}CHECK((p->lowered.value_plans[i].state==SOL_MIR_RUNTIME_LOWERED_PRESENT)==(p->lowered.value_plans[i].facilities!=0));}size_t eligible_unused=0,used_value=0;for(size_t i=0;i<p->lowered.recipe_count;i++){const SolMirRuntimeLoweredValuePlan*value=&p->lowered.value_plans[i];if(value->facilities&&!value->demanded_facilities)++eligible_unused;if(value->demanded_facilities)++used_value;CHECK(value->state==(value->demanded_facilities?SOL_MIR_RUNTIME_LOWERED_PRESENT:SOL_MIR_RUNTIME_LOWERED_NOT_DEMANDED));}(void)eligible_unused;CHECK(used_value>0);CHECK(sol_mir_runtime_lowered_program_test_recipe_facilities(SOL_MIR_LINKAGE_RUNTIME_CREATE)!=0&&sol_mir_runtime_lowered_program_test_value_facilities(SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE,SOL_MIR_RUNTIME_COPY_UNREACHABLE,SOL_MIR_RUNTIME_EQUALITY_UNREACHABLE,SOL_MIR_RUNTIME_OWNERSHIP_UNREACHABLE,SOL_MIR_RUNTIME_HOST_RESULT_UNREACHABLE,false)==0);CHECK(sol_mir_runtime_lowered_program_test_recipe_facilities(0)==0&&(sol_mir_runtime_lowered_program_test_value_facilities(SOL_MIR_RUNTIME_ALLOCATION_PLAN_FIXED_OBJECT,SOL_MIR_RUNTIME_COPY_UNREACHABLE,SOL_MIR_RUNTIME_EQUALITY_UNREACHABLE,SOL_MIR_RUNTIME_OWNERSHIP_UNREACHABLE,SOL_MIR_RUNTIME_HOST_RESULT_UNREACHABLE,false)&SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION)!=0);CHECK(demanded>0&&not_demanded>0);if(demanded_id!=SOL_MIR_RUNTIME_LOWERED_NONE){SolMirRuntimeLoweredState state=p->lowered.value_plans[demanded_id].state;p->lowered.value_plans[demanded_id].state=SOL_MIR_RUNTIME_LOWERED_NOT_DEMANDED;CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered,NULL));p->lowered.value_plans[demanded_id].state=state;CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered,NULL));}if(not_demanded_id!=SOL_MIR_RUNTIME_LOWERED_NONE){SolMirRuntimeLoweredState state=p->lowered.recipes[not_demanded_id].state;p->lowered.recipes[not_demanded_id].state=SOL_MIR_RUNTIME_LOWERED_PRESENT;CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered,NULL));p->lowered.recipes[not_demanded_id].state=state;CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered,NULL));}
}
static void test_erased_loop_census(void) {
    Fixture fixture; Pipeline pipeline; init(&pipeline); CHECK(setup_at(&fixture,SOL_TEST_SOURCE_DIR "/tests/conformance/p33"));
    SolMirProgramRoot root={find(&fixture.ir,"branching",SOL_IR_CALLABLE_FUNCTION),SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE}; SolMirTargetDescriptor target=sol_mir_target_wasm32();
    bool built=root.callable!=SOL_IR_NONE&&sol_mir_concrete_program_build(&(SolMirConcreteBuildRequest){&fixture.ir,&root,1,NULL,0,&target,NULL},&pipeline.concrete,&fixture.d)==SOL_MIR_CONCRETE_BUILD_SUCCEEDED&&sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&pipeline.concrete,NULL},&pipeline.conventions,&fixture.d)==SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED&&sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&pipeline.conventions,NULL},&pipeline.values,&fixture.d)==SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED&&sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&pipeline.conventions,&pipeline.values,NULL},&pipeline.cleanup,&fixture.d)==SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED&&sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&pipeline.conventions,&pipeline.values,&pipeline.cleanup,NULL},&pipeline.host,&fixture.d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED&&sol_mir_runtime_handler_abi_build(&(SolMirRuntimeHandlerAbiBuildRequest){&pipeline.conventions,&pipeline.values,&pipeline.cleanup,&pipeline.host,NULL},&pipeline.handlers,&fixture.d)==SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_SUCCEEDED&&sol_mir_runtime_lowered_program_build(&(SolMirRuntimeLoweredProgramBuildRequest){&pipeline.conventions,&pipeline.values,&pipeline.cleanup,&pipeline.host,&pipeline.handlers,NULL},&pipeline.lowered,&fixture.d)==SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED;
    CHECK(built); if (!built) sol_diagnostics_render_human(stderr,&fixture.p.source,&fixture.d); if (built) { CHECK(pipeline.concrete.materialization.loop_count>0); CHECK(pipeline.lowered.usage.erased_loops==pipeline.concrete.materialization.loop_count); CHECK(sol_mir_runtime_lowered_program_validate(&pipeline.lowered,NULL)); }
    done(&pipeline);finish(&fixture);
}

static void test_propagation_value_demand(void) {
    Fixture fixture; Pipeline pipeline; init(&pipeline);
    CHECK(setup(&fixture)); bool built = !failures && build_named(&fixture, &pipeline, "increment_option");
    if (!built) sol_diagnostics_render_human(stderr, &fixture.p.source, &fixture.d);
    CHECK(built); if (built) check_build_work(&pipeline);
    if (built) {
        size_t propagations = 0, pending = 0;
        for (size_t i = 0; i < pipeline.lowered.image_terminator_count; ++i) {
            SolMirRuntimeLoweredImageTerminator *row = &pipeline.lowered.image_terminators[i];
            if (row->kind != SOL_MIR_TERM_PROPAGATE) continue;
            ++propagations;
            CHECK(row->plan != SOL_MIR_RUNTIME_LOWERED_NONE && row->plan < pipeline.lowered.semantic_plan_count && row->demanded_recipes.count >= 3);
            SolMirRuntimeSlice demands = row->demanded_recipes;
            row->demanded_recipes.count = 0;
            pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline.lowered);
            CHECK(!sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL));
            row->demanded_recipes = demands;
            pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline.lowered);
            CHECK(sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL));
        }
        for (size_t i = 0; i < pipeline.lowered.cleanup_failure_count; ++i) {
            const SolMirRuntimeLoweredCleanupFailure *row = &pipeline.lowered.cleanup_failures[i];
            if (row->kind == SOL_MIR_RUNTIME_LOWERED_CLEANUP_TRANSITION
                && row->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING) {
                ++pending;
                CHECK(row->failure_site == SOL_MIR_RUNTIME_LOWERED_NONE && row->failure_mask == 0);
            }
        }
        CHECK(propagations > 0);
        (void)pending;
    }
    done(&pipeline); finish(&fixture);
}
static void test_pattern_copy_pipeline(void) {
    Fixture fixture; Pipeline pipeline; init(&pipeline);
    CHECK(setup_at(&fixture, SOL_TEST_SOURCE_DIR "/tests/conformance/p43_pattern_copy"));
    bool built = !failures && build_named(&fixture, &pipeline, "launch");
    if (!built) sol_diagnostics_render_human(stderr, &fixture.p.source, &fixture.d);
    CHECK(built);
    if (built) {
        const SolMirOperations *operations = &pipeline.concrete.operations;
        size_t extraction = SOL_MIR_RUNTIME_LOWERED_NONE, event = SOL_MIR_RUNTIME_LOWERED_NONE;
        for (size_t i = 0; i < operations->pattern_extraction_count; ++i) {
            const SolMirOperationPatternExtraction *plan = &operations->pattern_extractions[i];
            if (plan->copy_kind == SOL_MIR_COPY_TEXT
                && plan->result_recipe < pipeline.values.allocation_plan_count
                && pipeline.values.allocation_plans[plan->result_recipe].kind
                    == SOL_MIR_RUNTIME_ALLOCATION_PLAN_TEXT) {
                CHECK(extraction == SOL_MIR_RUNTIME_LOWERED_NONE); extraction = i;
            }
        }
        CHECK(extraction != SOL_MIR_RUNTIME_LOWERED_NONE);
        if (extraction != SOL_MIR_RUNTIME_LOWERED_NONE) {
            const SolMirOperationPatternExtraction *plan = &operations->pattern_extractions[extraction];
            CHECK(plan->instruction < pipeline.concrete.materialization.instruction_count
                && pipeline.concrete.materialization.instructions[plan->instruction].kind
                    == SOL_MIR_INST_PATTERN_VALUE);
            for (size_t i = 0; i < pipeline.cleanup.event_count; ++i) {
                const SolMirRuntimeCleanupEvent *candidate = &pipeline.cleanup.events[i];
                if (candidate->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION
                    && candidate->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
                    && candidate->origin == SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT
                    && candidate->operation == plan->instruction
                    && candidate->supplemental_site != SOL_MIR_RUNTIME_LOWERED_NONE) {
                    CHECK(event == SOL_MIR_RUNTIME_LOWERED_NONE); event = i;
                }
            }
            CHECK(event != SOL_MIR_RUNTIME_LOWERED_NONE);
            if (event != SOL_MIR_RUNTIME_LOWERED_NONE) {
                SolMirRuntimeCleanupEvent *row = &pipeline.cleanup.events[event];
                SolMirRuntimeCleanupSupplementalSite *site = &pipeline.cleanup.supplemental_sites[row->supplemental_site];
                const uint32_t allocation_mask = (UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED - 1))
                    | (UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_ALLOCATION_LIMIT - 1));
                CHECK(site->event == event && site->allowed_codes == allocation_mask
                    && row->transitions.count == 2);
                const SolMirRuntimeCleanupTransition *failure = &pipeline.cleanup.transitions[row->transitions.offset + 1];
                CHECK(failure->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
                    && failure->actions.count >= 1
                    && pipeline.cleanup.actions[failure->actions.offset + failure->actions.count - 1].kind
                        == SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE);
                SolMirCopyKind copy = ((SolMirOperationPatternExtraction *)operations->pattern_extractions)[extraction].copy_kind;
                ((SolMirOperationPatternExtraction *)operations->pattern_extractions)[extraction].copy_kind = SOL_MIR_COPY_TRIVIAL;
                CHECK(!sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL));
                ((SolMirOperationPatternExtraction *)operations->pattern_extractions)[extraction].copy_kind = copy;
                CHECK(sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL));
                SolMirPlanSlice path = ((SolMirOperationPatternExtraction *)operations->pattern_extractions)[extraction].path;
                if (path.count) {
                    --((SolMirOperationPatternExtraction *)operations->pattern_extractions)[extraction].path.count;
                    CHECK(!sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL));
                    ((SolMirOperationPatternExtraction *)operations->pattern_extractions)[extraction].path = path;
                    CHECK(sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL));
                }
                SolMirRuntimeCleanupSupplementalSiteId supplemental = row->supplemental_site;
                row->supplemental_site = SOL_MIR_RUNTIME_LOWERED_NONE;
                CHECK(!sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL));
                row->supplemental_site = supplemental;
                uint32_t mask = site->allowed_codes; site->allowed_codes = 0;
                CHECK(!sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL)); site->allowed_codes = mask;
                SolMirRuntimeCleanupProducerKind producer = row->producer;
                row->producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_ARITHMETIC;
                CHECK(!sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL)); row->producer = producer;
                CHECK(sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL));
                SolMirRuntimeLoweredImageInstruction *lower = &pipeline.lowered.image_instructions[plan->instruction];
                CHECK(lower->cleanup_event == event
                    && lower->failure_site == SOL_MIR_RUNTIME_LOWERED_NONE);
                SolMirRuntimeCleanupEventId join = lower->cleanup_event;
                lower->cleanup_event = SOL_MIR_RUNTIME_LOWERED_NONE; reseal(&pipeline.lowered);
                CHECK(!sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL));
                lower->cleanup_event = join; reseal(&pipeline.lowered);
                CHECK(sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL));
                char *text = rendered(&pipeline.lowered, NULL);
                CHECK(text && strstr(text, "cleanup-event=") != NULL); free(text);
            }
        }
        SolMirRuntimeCleanupLimits exact = {pipeline.cleanup.usage.events,
            pipeline.cleanup.usage.actions, pipeline.cleanup.usage.transitions,
            pipeline.cleanup.usage.supplemental_sites, pipeline.cleanup.usage.drop_paths,
            pipeline.cleanup.usage.owned_bytes, pipeline.cleanup.usage.build_scratch_bytes,
            pipeline.cleanup.usage.build_work, pipeline.cleanup.usage.validation_scratch_bytes,
            pipeline.cleanup.usage.validation_work};
        SolMirRuntimeCleanup exact_cleanup, limited_cleanup;
        sol_mir_runtime_cleanup_init(&exact_cleanup); sol_mir_runtime_cleanup_init(&limited_cleanup);
        CHECK(sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){
            &pipeline.conventions, &pipeline.values, &exact}, &exact_cleanup, &fixture.d)
            == SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED
            && sol_mir_runtime_cleanup_validate(&exact_cleanup, NULL));
        --exact.max_events;
        CHECK(sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){
            &pipeline.conventions, &pipeline.values, &exact}, &limited_cleanup, &fixture.d)
            == SOL_MIR_RUNTIME_CLEANUP_BUILD_RESOURCE_EXHAUSTED);
        sol_mir_runtime_cleanup_free(&limited_cleanup); sol_mir_runtime_cleanup_free(&exact_cleanup);
    }
    done(&pipeline); finish(&fixture);
}

static void test_nontrivial_copy_demand(void) {
    Fixture fixture; Pipeline pipeline; init(&pipeline);
    CHECK(setup_at(&fixture, SOL_TEST_SOURCE_DIR "/tests/conformance/p36_copy"));
    bool built = !failures && build_named(&fixture, &pipeline, "copy_text");
    if (!built) sol_diagnostics_render_human(stderr, &fixture.p.source, &fixture.d);
    CHECK(built); if (built) check_build_work(&pipeline);
    if (built) {
        size_t copies = 0;
        for (size_t i = 0; i < pipeline.lowered.image_instruction_count; ++i) {
            SolMirRuntimeLoweredImageInstruction *row = &pipeline.lowered.image_instructions[i];
            if (row->kind != SOL_MIR_INST_LOAD_COPY
                || (row->facilities & SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY) == 0) continue;
            ++copies;
            CHECK(row->demanded_recipes.count > 0);
            SolMirRecipeId recipe = pipeline.concrete.materialization.instructions[row->instruction].type;
            CHECK(recipe < pipeline.concrete.representation.recipe_count
                && (pipeline.concrete.representation.recipes[recipe].copy_kind == SOL_MIR_COPY_TEXT
                    || pipeline.concrete.representation.recipes[recipe].copy_kind == SOL_MIR_COPY_AGGREGATE
                    || pipeline.concrete.representation.recipes[recipe].copy_kind == SOL_MIR_COPY_WRAPPER));
            for (size_t q = 0; q < row->demanded_recipes.count; ++q) {
                const SolMirRuntimeLoweredRecipeDemand *d = &pipeline.lowered.recipe_demands[row->demanded_recipes.offset + q];
                CHECK(d->recipe == recipe && (d->facilities & SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY) != 0
                    && pipeline.values.copy_plans[d->value_plan].recipe == d->recipe
                    && pipeline.values.copy_plans[d->value_plan].classification != SOL_MIR_RUNTIME_COPY_UNREACHABLE
                    && pipeline.values.copy_plans[d->value_plan].classification != SOL_MIR_RUNTIME_COPY_FORBIDDEN);
            }
            SolMirRuntimeSlice demands = row->demanded_recipes; row->demanded_recipes.count = 0; CHECK(!sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL)); row->demanded_recipes = demands; CHECK(sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL));
        }
        CHECK(copies > 0);
    }
    done(&pipeline); finish(&fixture);
}

static void test_slice_b_graph_joins(Pipeline *p) {
    const SolMirMaterialization *m = &p->concrete.materialization;
    const SolMirOperations *o = &p->concrete.operations;
    size_t image_incoming = 0, image_outgoing = 0;
    size_t predicate_incoming = 0, predicate_outgoing = 0;
    size_t direct_internal = 0, direct_host = 0;
    for (size_t block = 0; block < p->lowered.image_block_count; ++block) {
        const SolMirRuntimeLoweredImageBlock *row = &p->lowered.image_blocks[block];
        CHECK(row->state == SOL_MIR_RUNTIME_LOWERED_PRESENT);
        CHECK(row->incoming_edges.offset == image_incoming && row->outgoing_edges.offset == image_outgoing);
        image_incoming += row->incoming_edges.count;
        image_outgoing += row->outgoing_edges.count;
    }
    CHECK(image_incoming == m->edge_count && image_outgoing == m->edge_count);
    for (size_t block = 0; block < p->lowered.predicate_block_count; ++block) {
        const SolMirRuntimeLoweredPredicateBlock *row = &p->lowered.predicate_blocks[block];
        CHECK(row->state == SOL_MIR_RUNTIME_LOWERED_PRESENT);
        CHECK(row->incoming_edges.offset == predicate_incoming && row->outgoing_edges.offset == predicate_outgoing);
        predicate_incoming += row->incoming_edges.count;
        predicate_outgoing += row->outgoing_edges.count;
    }
    CHECK(predicate_incoming == o->predicate_edge_count && predicate_outgoing == o->predicate_edge_count);
    for (size_t i = 0; i < p->lowered.predicate_body_count; ++i) {
        const SolMirPredicateBody *input = &o->predicate_bodies[i];
        const SolMirRuntimeLoweredPredicateBody *row = &p->lowered.predicate_bodies[i];
        CHECK(row->state == SOL_MIR_RUNTIME_LOWERED_PRESENT && row->body == i && row->owner_kind == input->owner_kind && row->image == input->instance && row->import_id == input->import && row->context == input->context && row->phase == input->phase && row->outcome == input->outcome && row->blocks.offset == input->blocks.offset && row->blocks.count == input->blocks.count && row->entry == input->entry && row->output_recipe == input->output_recipe && row->refinement_self_recipe == input->refinement_self_recipe);
    }
    for (size_t i = 0; i < p->lowered.call_count; ++i) {
        const SolMirRuntimeCall *input = &p->conventions.calls[i];
        const SolMirRuntimeLoweredCall *row = &p->lowered.calls[i];
        CHECK(row->state == SOL_MIR_RUNTIME_LOWERED_PRESENT && row->call == i);
        CHECK(row->signature == input->signature && row->owner_kind == input->owner_kind && row->image == input->image && row->body == input->predicate && row->block == input->block && row->call_kind == input->call_kind);
        CHECK(row->target_kind == input->target_kind && row->internal == input->internal && row->host == input->host && row->table == input->table);
        CHECK(row->callee.kind == input->callee.kind && row->callee.id == input->callee.id && row->operands.offset == input->operands.offset && row->operands.count == input->operands.count && row->result.kind == input->result.kind && row->result.id == input->result.id && row->normal_edge == input->normal_edge && row->failure_edge == input->failure_edge && row->writebacks.offset == input->writebacks.offset && row->writebacks.count == input->writebacks.count && row->failure_site == input->failure_site);
        if (row->target_kind == SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL) ++direct_internal;
        if (row->target_kind == SOL_MIR_RUNTIME_TARGET_DIRECT_HOST) ++direct_host;
    }
    CHECK(direct_internal > 0 && direct_host > 0);
    size_t allocations = 0, copies = 0, equalities = 0, ownerships = 0, host_results = 0;
    for (size_t i = 0; i < p->lowered.recipe_count; ++i) {
        const SolMirRuntimeRecipeOperations *input = &p->values.recipe_operations[i];
        const SolMirRuntimeLoweredRecipe *row = &p->lowered.recipes[i];
        CHECK(row->recipe == input->recipe && row->operations == i && row->demanded_operations == input->demanded_operations);
        CHECK(row->create_import == input->create_import && row->copy_import == input->copy_import && row->drop_import == input->drop_import && row->equal_import == input->equal_import);
        allocations += row->allocation != SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE;
        copies += row->copy != SOL_MIR_RUNTIME_COPY_UNREACHABLE && row->copy != SOL_MIR_RUNTIME_COPY_FORBIDDEN;
        equalities += row->equality != SOL_MIR_RUNTIME_EQUALITY_UNREACHABLE && row->equality != SOL_MIR_RUNTIME_EQUALITY_FORBIDDEN;
        ownerships += row->ownership != SOL_MIR_RUNTIME_OWNERSHIP_UNREACHABLE;
        host_results += row->host_result != SOL_MIR_RUNTIME_HOST_RESULT_UNREACHABLE && row->host_result != SOL_MIR_RUNTIME_HOST_RESULT_FORBIDDEN;
    }
    CHECK(allocations > 0 && copies > 0 && equalities > 0 && ownerships > 0 && host_results > 0);
    if (p->lowered.call_count) { size_t block = p->lowered.calls[0].block; p->lowered.calls[0].block = SOL_MIR_RUNTIME_LOWERED_NONE; CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL)); p->lowered.calls[0].block = block; CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL)); }
    if (p->lowered.image_edge_count) { SolMirMaterializedBlockId target = p->lowered.image_edges[0].target; p->lowered.image_edges[0].target = SOL_MIR_RUNTIME_LOWERED_NONE; CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL)); p->lowered.image_edges[0].target = target; CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL)); }
    for (size_t i = 0; i < p->lowered.recipe_count; ++i) if (p->lowered.recipes[i].demanded_operations) { SolMirRuntimeImportId create = p->lowered.recipes[i].create_import; p->lowered.recipes[i].create_import = SOL_MIR_RUNTIME_LOWERED_NONE; CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL)); p->lowered.recipes[i].create_import = create; CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL)); break; }
}

static void test_executable_plan_event_value_joins(Pipeline *p) {
    size_t semantic = 0, events = 0, invokes = 0, predicate_invokes = 0;
    size_t allocation = 0, copy = 0, equality = 0, ownership = 0, host_result = 0;
    size_t construction = 0, arithmetic_equality = 0, propagation = 0, load_copy = 0;
    size_t live_drop = 0, scope_exit = 0, control_events = 0;
    for (size_t i = 0; i < p->lowered.image_instruction_count; ++i) {
        const SolMirRuntimeLoweredImageInstruction *row = &p->lowered.image_instructions[i];
        if (row->plan != SOL_MIR_RUNTIME_LOWERED_NONE) { ++semantic; CHECK(row->plan < p->lowered.semantic_plan_count); }
        if (row->cleanup_event != SOL_MIR_RUNTIME_LOWERED_NONE) { ++events; CHECK(row->cleanup_event < p->cleanup.event_count && row->failure_site == p->cleanup.events[row->cleanup_event].inherited_failure_site); }
        if (row->runtime_class != SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE && row->cleanup_event != SOL_MIR_RUNTIME_LOWERED_NONE) { ++control_events; CHECK(p->cleanup.events[row->cleanup_event].producer == SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL); }
        if (row->kind == SOL_MIR_INST_DROP_IF_INITIALIZED && row->cleanup_event != SOL_MIR_RUNTIME_LOWERED_NONE) { ++live_drop; CHECK(p->cleanup.events[row->cleanup_event].producer == SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL && row->demanded_recipes.count > 0); }
        if (row->kind == SOL_MIR_INST_SCOPE_EXIT && row->cleanup_event != SOL_MIR_RUNTIME_LOWERED_NONE) { ++scope_exit; CHECK(p->cleanup.events[row->cleanup_event].producer == SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL); }
        if (row->kind == SOL_MIR_INST_LOAD_COPY && (row->facilities & SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY) != 0) { ++load_copy; CHECK(row->demanded_recipes.count > 0); }
        for (size_t q = 0; q < row->demanded_recipes.count; ++q) { const SolMirRuntimeLoweredRecipeDemand *d = &p->lowered.recipe_demands[row->demanded_recipes.offset + q]; construction += row->kind == SOL_MIR_INST_CONSTRUCT && (d->facilities & SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION) != 0; arithmetic_equality += (row->kind == SOL_MIR_INST_BINARY || row->kind == SOL_MIR_INST_COMPOUND_UPDATE) && (d->facilities & SOL_MIR_RUNTIME_LOWERED_FACILITY_EQUALITY) != 0; }
    }
    for (size_t i = 0; i < p->lowered.image_terminator_count; ++i) {
        const SolMirRuntimeLoweredImageTerminator *row = &p->lowered.image_terminators[i];
        if (row->plan != SOL_MIR_RUNTIME_LOWERED_NONE) { ++semantic; CHECK(row->plan < p->lowered.semantic_plan_count); }
        if (row->cleanup_event != SOL_MIR_RUNTIME_LOWERED_NONE) { ++events; CHECK(row->cleanup_event < p->cleanup.event_count && row->failure_site == p->cleanup.events[row->cleanup_event].inherited_failure_site); }
        if (row->kind == SOL_MIR_TERM_INVOKE) { ++invokes; CHECK(row->call < p->lowered.call_count && row->cleanup_event < p->cleanup.event_count && p->cleanup.events[row->cleanup_event].producer == SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_INVOKE && row->failure_site == p->lowered.calls[row->call].failure_site); }
        propagation += row->kind == SOL_MIR_TERM_PROPAGATE && row->demanded_recipes.count > 0;
    }
    for (size_t i = 0; i < p->lowered.predicate_instruction_count; ++i) {
        const SolMirRuntimeLoweredPredicateInstruction *row = &p->lowered.predicate_instructions[i];
        if (row->plan != SOL_MIR_RUNTIME_LOWERED_NONE) { ++semantic; CHECK(row->plan < p->lowered.semantic_plan_count); }
        if (row->cleanup_event != SOL_MIR_RUNTIME_LOWERED_NONE) { ++events; CHECK(row->cleanup_event < p->cleanup.event_count && row->failure_site == p->cleanup.events[row->cleanup_event].inherited_failure_site); }
    }
    for (size_t i = 0; i < p->lowered.predicate_terminator_count; ++i) {
        const SolMirRuntimeLoweredPredicateTerminator *row = &p->lowered.predicate_terminators[i];
        if (row->plan != SOL_MIR_RUNTIME_LOWERED_NONE) { ++semantic; CHECK(row->plan < p->lowered.semantic_plan_count); }
        if (row->cleanup_event != SOL_MIR_RUNTIME_LOWERED_NONE) { ++events; CHECK(row->cleanup_event < p->cleanup.event_count && row->failure_site == p->cleanup.events[row->cleanup_event].inherited_failure_site); }
        if (row->kind == SOL_MIR_PREDICATE_TERM_INVOKE) { ++predicate_invokes; CHECK(row->call < p->lowered.call_count && row->cleanup_event < p->cleanup.event_count && p->cleanup.events[row->cleanup_event].producer == SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_INVOKE && row->failure_site == p->lowered.calls[row->call].failure_site); }
    }
    for (size_t i = 0; i < p->lowered.recipe_demand_count; ++i) {
        const SolMirRuntimeLoweredRecipeDemand *row = &p->lowered.recipe_demands[i];
        CHECK(row->state == SOL_MIR_RUNTIME_LOWERED_PRESENT && row->ordinal == i && row->value_plan < p->lowered.value_plan_count && p->lowered.value_plans[row->value_plan].recipe == row->recipe);
        allocation += (row->facilities & SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION) != 0;
        copy += (row->facilities & SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY) != 0;
        equality += (row->facilities & SOL_MIR_RUNTIME_LOWERED_FACILITY_EQUALITY) != 0;
        ownership += (row->facilities & SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP) != 0;
        host_result += (row->facilities & SOL_MIR_RUNTIME_LOWERED_FACILITY_HOST_RESULT) != 0;
    }
    (void)predicate_invokes;
    (void)propagation;
    CHECK(semantic > 0 && events > 0 && invokes > 0 && p->lowered.recipe_demand_count > 0 && allocation > 0 && copy > 0 && equality > 0 && ownership > 0 && host_result > 0 && construction > 0 && arithmetic_equality > 0 && load_copy > 0 && live_drop > 0 && scope_exit > 0 && control_events > 0);
    for (size_t i = 0; i < p->lowered.image_instruction_count; ++i) if (p->lowered.image_instructions[i].plan != SOL_MIR_RUNTIME_LOWERED_NONE) { size_t plan = p->lowered.image_instructions[i].plan; p->lowered.image_instructions[i].plan = SOL_MIR_RUNTIME_LOWERED_NONE; CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL)); p->lowered.image_instructions[i].plan = plan; CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL)); break; }
    for (size_t i = 0; i < p->lowered.image_terminator_count; ++i) if (p->lowered.image_terminators[i].cleanup_event != SOL_MIR_RUNTIME_LOWERED_NONE) { size_t event = p->lowered.image_terminators[i].cleanup_event; p->lowered.image_terminators[i].cleanup_event = SOL_MIR_RUNTIME_LOWERED_NONE; CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL)); p->lowered.image_terminators[i].cleanup_event = event; CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL)); break; }
    for (size_t i = 0; i < p->lowered.image_instruction_count; ++i) if (p->lowered.image_instructions[i].kind == SOL_MIR_INST_SCOPE_EXIT && p->lowered.image_instructions[i].cleanup_event != SOL_MIR_RUNTIME_LOWERED_NONE) {
        SolMirRuntimeLoweredImageInstruction *row = &p->lowered.image_instructions[i];
        size_t event = row->cleanup_event;
        row->cleanup_event = SOL_MIR_RUNTIME_LOWERED_NONE;
        p->lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&p->lowered);
        CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
        row->cleanup_event = event == 0 ? 1 : 0;
        p->lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&p->lowered);
        CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
        row->cleanup_event = event;
        p->lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&p->lowered);
        CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
        break;
    }
    if (p->lowered.recipe_demand_count) { SolMirRecipeId recipe = p->lowered.recipe_demands[0].recipe; p->lowered.recipe_demands[0].recipe = SOL_MIR_RECIPE_NONE; CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL)); p->lowered.recipe_demands[0].recipe = recipe; CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL)); }
}

static void reseal(SolMirRuntimeLoweredProgram *program) { program->authentication = sol_mir_runtime_lowered_program_test_seal(program); }

static void test_slice_d_raw_owner_preflight(Pipeline *p) {
    CHECK(p->lowered.image_instruction_count && p->lowered.image_block_count
        && p->cleanup.event_count);
    SolMirRuntimeLoweredImageBlock *blocks = p->lowered.image_blocks;
    size_t block_count = p->lowered.image_block_count;
    size_t block_capacity = p->lowered.image_block_capacity;
    p->lowered.image_blocks = (SolMirRuntimeLoweredImageBlock *)(void *)p->lowered.image_instructions;
    CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
    p->lowered.image_blocks = blocks;
    CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));

    p->lowered.image_block_capacity = block_capacity + 1;
    CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
    p->lowered.image_block_capacity = block_capacity;
    CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));

    p->lowered.image_blocks = NULL;
    CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
    p->lowered.image_blocks = blocks;
    CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));

    p->lowered.image_blocks = (SolMirRuntimeLoweredImageBlock *)(void *)p->cleanup.events;
    p->lowered.image_block_count = p->lowered.image_block_capacity = 1;
    CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
    p->lowered.image_blocks = blocks;
    p->lowered.image_block_count = block_count;
    p->lowered.image_block_capacity = block_capacity;
    CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));

    const void *predecessor_arenas[] = {p->conventions.calls, p->values.recipe_operations,
        p->cleanup.events, p->host.operations};
    const size_t predecessor_counts[] = {p->conventions.call_count, p->values.recipe_operation_count,
        p->cleanup.event_count, p->host.operation_count};
    for (size_t i = 0; i < sizeof predecessor_arenas / sizeof *predecessor_arenas; ++i) {
        CHECK(predecessor_counts[i] != 0);
        if (!predecessor_counts[i]) continue;
        p->lowered.image_blocks = (SolMirRuntimeLoweredImageBlock *)(uintptr_t)predecessor_arenas[i];
        p->lowered.image_block_count = p->lowered.image_block_capacity = 1;
        CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
        p->lowered.image_blocks = blocks;
        p->lowered.image_block_count = block_count;
        p->lowered.image_block_capacity = block_capacity;
        CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
    }

    /* The concrete-arena visitor must reject a local owner range that covers
     * borrowed text at the guard, before the visitor dereferences a byte. */
    CHECK(p->concrete.program.ir->source_path != NULL);
    p->lowered.image_blocks = (SolMirRuntimeLoweredImageBlock *)(void *)
        p->concrete.program.ir->source_path;
    p->lowered.image_block_count = p->lowered.image_block_capacity = 1;
    CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
    p->lowered.image_blocks = blocks;
    p->lowered.image_block_count = block_count;
    p->lowered.image_block_capacity = block_capacity;
    CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));

    p->lowered.image_blocks = (SolMirRuntimeLoweredImageBlock *)(void *)p->cleanup.events;
    p->lowered.image_block_count = p->lowered.image_block_capacity = SIZE_MAX;
    CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
    p->lowered.image_blocks = blocks;
    p->lowered.image_block_count = block_count;
    p->lowered.image_block_capacity = block_capacity;
    CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));

    p->lowered.image_blocks = (SolMirRuntimeLoweredImageBlock *)(uintptr_t)(UINTPTR_MAX - 1);
    p->lowered.image_block_count = p->lowered.image_block_capacity = SIZE_MAX;
    CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
    p->lowered.image_blocks = blocks;
    p->lowered.image_block_count = block_count;
    p->lowered.image_block_capacity = block_capacity;
    CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));

    SolMirRuntimeLoweredProgramUsage usage = p->lowered.usage;
    memset(&p->lowered.usage, 0, sizeof p->lowered.usage);
    reseal(&p->lowered);
    CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
    p->lowered.usage = usage;
    reseal(&p->lowered);
    CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));

    if (p->lowered.image_instruction_count) {
        SolMirRuntimeSlice demands = p->lowered.image_instructions[0].demanded_recipes;
        p->lowered.image_instructions[0].demanded_recipes =
            (SolMirRuntimeSlice){SIZE_MAX, 1};
        reseal(&p->lowered);
        CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
        p->lowered.image_instructions[0].demanded_recipes = demands;
        reseal(&p->lowered);
        CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
    }
    if (p->lowered.host_grant_count) {
        SolMirRuntimeSlice incidences = p->lowered.host_grants[0].incidences;
        p->lowered.host_grants[0].incidences = (SolMirRuntimeSlice){SIZE_MAX, 1};
        reseal(&p->lowered);
        CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
        p->lowered.host_grants[0].incidences = incidences;
        reseal(&p->lowered);
        CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
    }
}

static SolMirRuntimeLoweredProgramLimits exact_limits(const Pipeline *p) {
    SolMirRuntimeLoweredProgramLimits limits = p->lowered.limits;
#define EXACT(member,type,singular) limits.max_##member = p->lowered.usage.member ? p->lowered.usage.member : 1;
    SOL_MIR_RUNTIME_LOWERED_TABLES(EXACT)
#undef EXACT
    limits.max_owned_bytes = p->lowered.usage.owned_bytes ? p->lowered.usage.owned_bytes : 1;
    limits.max_build_scratch_bytes = p->lowered.usage.build_scratch_bytes ? p->lowered.usage.build_scratch_bytes : 1;
    limits.max_build_work = p->lowered.usage.build_work ? p->lowered.usage.build_work : 1;
    limits.max_validation_scratch_bytes = p->lowered.usage.validation_scratch_bytes ? p->lowered.usage.validation_scratch_bytes : 1;
    limits.max_validation_work = p->lowered.usage.validation_work ? p->lowered.usage.validation_work : 1;
    limits.max_render_bytes = p->lowered.usage.render_bytes ? p->lowered.usage.render_bytes : 1;
    limits.max_render_scratch_bytes = p->lowered.usage.render_scratch_bytes ? p->lowered.usage.render_scratch_bytes : 1;
    return limits;
}
static bool rebuild_lowered(const Pipeline *p, const SolMirRuntimeLoweredProgramLimits *limits) {
    SolMirRuntimeLoweredProgram candidate; sol_mir_runtime_lowered_program_init(&candidate);
    SolMirRuntimeLoweredProgramBuildOutcome outcome = sol_mir_runtime_lowered_program_build(
        &(SolMirRuntimeLoweredProgramBuildRequest){&p->conventions, &p->values, &p->cleanup,
            &p->host, &p->handlers, limits}, &candidate, NULL);
    sol_mir_runtime_lowered_program_free(&candidate);
    return outcome == SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED;
}
static void test_exact_validation_work_limit(Pipeline *p) {
    SolMirRuntimeLoweredProgramLimits limits = exact_limits(p);
    SolMirRuntimeLoweredProgram repeated;
    sol_mir_runtime_lowered_program_init(&repeated);
    CHECK(sol_mir_runtime_lowered_program_build(
            &(SolMirRuntimeLoweredProgramBuildRequest){&p->conventions, &p->values,
                &p->cleanup, &p->host, &p->handlers, &limits}, &repeated, NULL)
            == SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED
        && !memcmp(&repeated.usage, &p->lowered.usage, sizeof repeated.usage));
    sol_mir_runtime_lowered_program_free(&repeated);
    if (!p->lowered.usage.validation_work) return;
    SolMirRuntimeLoweredProgramLimits less = limits;
    less.max_validation_work = p->lowered.usage.validation_work - 1;
    CHECK(!rebuild_lowered(p, &less));
    size_t saved_limit = p->lowered.limits.max_validation_work;
    p->lowered.limits.max_validation_work = less.max_validation_work;
    reseal(&p->lowered);
    CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
    p->lowered.limits.max_validation_work = saved_limit;
    reseal(&p->lowered);
    CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
}
static void test_slice_d_resource_limits(Pipeline *p) {
    SolMirRuntimeLoweredProgramLimits limits = exact_limits(p);
    CHECK(rebuild_lowered(p, &limits));
#define BELOW(member,type,singular) do { \
    if (p->lowered.usage.member) { \
        SolMirRuntimeLoweredProgramLimits less = limits; \
        less.max_##member = p->lowered.usage.member - 1; \
        CHECK(!rebuild_lowered(p, &less)); \
    } \
} while (0);
    SOL_MIR_RUNTIME_LOWERED_TABLES(BELOW)
#undef BELOW
    if (p->lowered.usage.owned_bytes) { SolMirRuntimeLoweredProgramLimits less = limits; less.max_owned_bytes = p->lowered.usage.owned_bytes - 1; CHECK(!rebuild_lowered(p, &less)); }
    if (p->lowered.usage.build_scratch_bytes) {
        SolMirRuntimeLoweredProgramLimits less = limits;
        SolMirRuntimeLoweredProgram candidate;
        less.max_build_scratch_bytes = p->lowered.usage.build_scratch_bytes - 1;
        sol_mir_runtime_lowered_program_init(&candidate);
        sol_mir_runtime_lowered_program_test_reset_allocation_attempts();
        CHECK(sol_mir_runtime_lowered_program_build(
                &(SolMirRuntimeLoweredProgramBuildRequest){&p->conventions, &p->values,
                    &p->cleanup, &p->host, &p->handlers, &less}, &candidate, NULL)
                == SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_RESOURCE_EXHAUSTED);
        CHECK(candidate.authentication == 0 && candidate.image_instructions == NULL
            && sol_mir_runtime_lowered_program_test_build_scratch_allocation_attempts() == 0
            && sol_mir_runtime_lowered_program_test_persistent_allocation_attempts() == 0);
        sol_mir_runtime_lowered_program_free(&candidate);
    }
    if (p->lowered.usage.build_work) { SolMirRuntimeLoweredProgramLimits less = limits; less.max_build_work = p->lowered.usage.build_work - 1; CHECK(!rebuild_lowered(p, &less)); }
    if (p->lowered.usage.build_work) {
        SolMirRuntimeLoweredProgramLimits less = limits;
        SolMirRuntimeLoweredProgram candidate;
        less.max_build_work = p->lowered.usage.build_work - 1;
        sol_mir_runtime_lowered_program_init(&candidate);
        sol_mir_runtime_lowered_program_test_reset_allocation_attempts();
        CHECK(sol_mir_runtime_lowered_program_build(
                &(SolMirRuntimeLoweredProgramBuildRequest){&p->conventions, &p->values,
                    &p->cleanup, &p->host, &p->handlers, &less}, &candidate, NULL)
                == SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_RESOURCE_EXHAUSTED);
        CHECK(candidate.authentication == 0 && candidate.image_instructions == NULL
            && sol_mir_runtime_lowered_program_test_persistent_allocation_attempts() == 0);
        sol_mir_runtime_lowered_program_free(&candidate);
    }
    if (p->lowered.usage.validation_scratch_bytes) { SolMirRuntimeLoweredProgramLimits less = limits; less.max_validation_scratch_bytes = p->lowered.usage.validation_scratch_bytes - 1; CHECK(!rebuild_lowered(p, &less)); }
    test_exact_validation_work_limit(p);
    for (size_t i = 0; i < 2; ++i) {
        void *noise = malloc(97 + i * 131); CHECK(noise != NULL);
        SolMirRuntimeLoweredProgram candidate; sol_mir_runtime_lowered_program_init(&candidate);
        SolMirRuntimeLoweredProgramBuildOutcome outcome = sol_mir_runtime_lowered_program_build(
            &(SolMirRuntimeLoweredProgramBuildRequest){&p->conventions, &p->values, &p->cleanup,
                &p->host, &p->handlers, &limits}, &candidate, NULL);
        CHECK(outcome == SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED
            && !memcmp(&candidate.usage, &p->lowered.usage, sizeof candidate.usage));
        sol_mir_runtime_lowered_program_free(&candidate); free(noise);
    }
}
static void expect_slice_d_usage(const Pipeline *p, const size_t *expected,
    size_t owned, size_t build_scratch, size_t build_work, size_t validation_scratch,
    size_t validation_work, size_t render_bytes, size_t render_scratch) {
    size_t index = 0;
#define EXPECT(member,type,singular) CHECK(p->lowered.usage.member == expected[index++]);
    SOL_MIR_RUNTIME_LOWERED_TABLES(EXPECT)
#undef EXPECT
    CHECK(p->lowered.usage.owned_bytes == owned
        && p->lowered.usage.build_scratch_bytes == build_scratch
        && p->lowered.usage.build_work == build_work
        && p->lowered.usage.validation_scratch_bytes == validation_scratch
        && p->lowered.usage.validation_work == validation_work
        && p->lowered.usage.render_bytes == render_bytes
        && p->lowered.usage.render_scratch_bytes == render_scratch);
}
static void test_slice_d_e6_usage(const Pipeline *p) {
    size_t pre_invoke = 0, pre_residual = 0;
    for (size_t i = 0; i < p->lowered.image_terminator_count; ++i) {
        SolMirRuntimeCleanupEventId event =
            p->lowered.image_terminators[i].pre_operation_cleanup_event;
        if (event == SOL_MIR_RUNTIME_LOWERED_NONE) continue;
        pre_invoke += p->cleanup.events[event].phase
            == SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_INVOKE_CALLABLE;
        pre_residual += p->cleanup.events[event].phase
            == SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_PROPAGATE_RESIDUAL;
    }
    CHECK(pre_invoke == 5 && pre_residual == 0);
    static const size_t expected[] = {471,65,64,64,64,65,4,6,4,0,0,0,4,80,12,12,52,20,20,295,1523,5,4,5,0,0,0};
    expect_slice_d_usage(p, expected, 534352, 534930, 1287019, 5678, 901927,
        11629568, 11651256);
}
static size_t distinct_render_row_keys(const char *text, const char *prefix,
    char keys[][65], size_t capacity) {
    size_t count = 0; const char *line = text;
    while ((line = strstr(line, prefix)) != NULL) {
        const char *end = strchr(line, '\n');
        const char *key = strstr(line, " key=");
        CHECK(key != NULL && (!end || key < end) && strlen(key + 5) >= 64
            && count < capacity);
        if (!key || (end && key >= end) || strlen(key + 5) < 64 || count >= capacity)
            break;
        memcpy(keys[count], key + 5, 64); keys[count][64] = '\0'; ++count;
        line = end ? end + 1 : key + 5;
    }
    for (size_t i = 0; i < count; ++i)
        for (size_t j = 0; j < i; ++j) CHECK(strcmp(keys[i], keys[j]) != 0);
    return count;
}
static void test_slice_e_render(const Pipeline *p) {
    size_t first_length = 0, second_length = 0;
    char *first = rendered(&p->lowered, &first_length);
    char *second = rendered(&p->lowered, &second_length);
    CHECK(first && second && first_length == second_length
        && !memcmp(first, second, first_length));
    if (first) {
        CHECK(strstr(first, "mir_runtime_lowered_program\n") == first
            && strstr(first, "static=true backend-independent=true") != NULL
            && strstr(first, "census erased-loop-obligations=") != NULL);
#define CHECK_RENDER_TABLE(member,type,singular) do { \
        char prefix[128]; \
        CHECK(snprintf(prefix, sizeof prefix, #member " state=") > 0); \
        CHECK(occurrences(first, prefix) == p->lowered.singular##_count); \
        CHECK(strstr(first, "table=" #member " present=") != NULL); \
    } while (0);
        SOL_MIR_RUNTIME_LOWERED_TABLES(CHECK_RENDER_TABLE)
#undef CHECK_RENDER_TABLE
        CHECK(strstr(first, "structural-hash=") == NULL
            && strstr(first, "typed-keys=true") != NULL
            && strstr(first, " key=") != NULL);
        {
            const SolMirMaterialization *m = &p->concrete.materialization;
            size_t left = SOL_MIR_RUNTIME_LOWERED_NONE, right = SOL_MIR_RUNTIME_LOWERED_NONE;
            for (size_t i = 0; i < m->place_count && left == SOL_MIR_RUNTIME_LOWERED_NONE; ++i) {
                const SolMirMaterializedPlace *a = &m->places[i];
                if (a->projections.count == 0) continue;
                const SolMirMaterializedProjection *first_projection = &m->projections[a->projections.offset];
                if (first_projection->kind != SOL_IR_PROJECTION_FIELD) continue;
                for (size_t j = i + 1; j < m->place_count; ++j) {
                    const SolMirMaterializedPlace *b = &m->places[j];
                    if (b->projections.count != a->projections.count || b->projections.count == 0) continue;
                    const SolMirMaterializedProjection *second_projection = &m->projections[b->projections.offset];
                    if (second_projection->kind == SOL_IR_PROJECTION_FIELD
                        && second_projection->source_field != first_projection->source_field) {
                        left = i; right = j; break;
                    }
                }
            }
            CHECK(left != SOL_MIR_RUNTIME_LOWERED_NONE && right != SOL_MIR_RUNTIME_LOWERED_NONE);
            if (left != SOL_MIR_RUNTIME_LOWERED_NONE && right != SOL_MIR_RUNTIME_LOWERED_NONE) {
                char left_key[65], right_key[65];
                CHECK(sol_mir_runtime_lowered_program_test_place_key(&p->lowered, left, left_key)
                    && sol_mir_runtime_lowered_program_test_place_key(&p->lowered, right, right_key)
                    && strcmp(left_key, right_key) != 0
                    && strstr(first, left_key) != NULL && strstr(first, right_key) != NULL);
            }
        }
        {
            char grants[8][65], incidences[8][65];
            CHECK(distinct_render_row_keys(first, "host_grants state=", grants, 8)
                == p->lowered.host_grant_count
                && distinct_render_row_keys(first, "host_incidences state=", incidences, 8)
                    == p->lowered.host_incidence_count
                && strstr(first, "grant=") != NULL
                && strstr(first, "root=") != NULL
                && strstr(first, "operation=") != NULL
                && strstr(first, "recipe-demands-digest=") != NULL
                && strstr(first, "operands-digest=") != NULL
                && strstr(first, "slots-digest=") != NULL
                && strstr(first, "actions-digest=") != NULL
                && strstr(first, " target=") != NULL);
        }
    }
    for (size_t pass = 0; pass < 2 && first; ++pass) {
        SolMirRuntimeLoweredProgram candidate; size_t candidate_length = 0;
        void *noise = malloc(257 + pass * 509);
        char *candidate_text;
        sol_mir_runtime_lowered_program_init(&candidate);
        CHECK(noise != NULL);
        CHECK(sol_mir_runtime_lowered_program_build(
                &(SolMirRuntimeLoweredProgramBuildRequest){&p->conventions, &p->values,
                    &p->cleanup, &p->host, &p->handlers, &p->lowered.limits},
                &candidate, NULL) == SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED);
        candidate_text = rendered(&candidate, &candidate_length);
        CHECK(candidate_text && candidate_length == first_length
            && !memcmp(candidate_text, first, first_length));
        free(candidate_text); sol_mir_runtime_lowered_program_free(&candidate); free(noise);
    }
    {
        FILE *stream = tmpfile(); long before = -1, after = -2;
        uint64_t authentication = p->lowered.authentication;
        CHECK(stream != NULL);
        if (stream) {
            CHECK(fwrite("sentinel", 1, 8, stream) == 8);
            before = ftell(stream);
            ((SolMirRuntimeLoweredProgram *)(void *)&p->lowered)->authentication ^= 1;
            sol_mir_runtime_lowered_program_test_reset_render_write_attempts();
            CHECK(!sol_mir_runtime_lowered_program_render(stream, &p->lowered));
            CHECK(sol_mir_runtime_lowered_program_test_render_write_attempts() == 0);
            after = ftell(stream);
            ((SolMirRuntimeLoweredProgram *)(void *)&p->lowered)->authentication = authentication;
            CHECK(before == 8 && after == before);
            fclose(stream);
        }
    }
    if (p->lowered.usage.render_bytes) {
        SolMirRuntimeLoweredProgramLimits less = p->lowered.limits;
        less.max_render_bytes = p->lowered.usage.render_bytes - 1;
        CHECK(!rebuild_lowered(p, &less));
        SolMirRuntimeLoweredProgram limited = p->lowered;
        FILE *stream = tmpfile(); long before = -1, after = -2;
        limited.limits = less; reseal(&limited);
        CHECK(stream != NULL);
        if (stream) {
            CHECK(fwrite("sentinel", 1, 8, stream) == 8); before = ftell(stream);
            sol_mir_runtime_lowered_program_test_reset_render_write_attempts();
            CHECK(!sol_mir_runtime_lowered_program_render(stream, &limited));
            CHECK(sol_mir_runtime_lowered_program_test_render_write_attempts() == 0);
            after = ftell(stream); CHECK(before == 8 && after == before); fclose(stream);
        }
    }
    if (p->lowered.call_count) {
        SolMirRuntimeSignatureId signature = p->lowered.calls[0].signature;
        FILE *stream = tmpfile(); long before = -1, after = -2;
        p->lowered.calls[0].signature = SOL_MIR_RUNTIME_LOWERED_NONE;
        reseal((SolMirRuntimeLoweredProgram *)(void *)&p->lowered);
        CHECK(stream != NULL);
        if (stream) {
            CHECK(fwrite("sentinel", 1, 8, stream) == 8); before = ftell(stream);
            sol_mir_runtime_lowered_program_test_reset_render_write_attempts();
            CHECK(!sol_mir_runtime_lowered_program_render(stream, &p->lowered));
            CHECK(sol_mir_runtime_lowered_program_test_render_write_attempts() == 0);
            after = ftell(stream); CHECK(before == 8 && after == before); fclose(stream);
        }
        p->lowered.calls[0].signature = signature;
        reseal((SolMirRuntimeLoweredProgram *)(void *)&p->lowered);
        CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
    }
    if (p->lowered.usage.render_scratch_bytes) {
        SolMirRuntimeLoweredProgramLimits less = p->lowered.limits;
        less.max_render_scratch_bytes = p->lowered.usage.render_scratch_bytes - 1;
        CHECK(!rebuild_lowered(p, &less));
    }
    {
        FILE *stream = tmpfile(); long before = -1, after = -2;
        CHECK(stream != NULL);
        if (stream) {
            CHECK(fwrite("sentinel", 1, 8, stream) == 8);
            before = ftell(stream);
            sol_mir_runtime_lowered_program_test_force_render_allocation_failure(true);
            sol_mir_runtime_lowered_program_test_reset_render_write_attempts();
            CHECK(!sol_mir_runtime_lowered_program_render(stream, &p->lowered));
            CHECK(sol_mir_runtime_lowered_program_test_render_write_attempts() == 0);
            sol_mir_runtime_lowered_program_test_force_render_allocation_failure(false);
            after = ftell(stream);
            CHECK(before == 8 && after == before);
            fclose(stream);
        }
    }
    free(first); free(second);
}
static bool set_long_virtual_source_path(SolIr *ir, size_t extension) {
    static const char prefix[] = "virtual://runtime-lowered/";
    size_t length = sizeof prefix - 1 + extension;
    char *path = malloc(length + 1);
    if (!path) return false;
    memcpy(path, prefix, sizeof prefix - 1);
    memset(path + sizeof prefix - 1, 'x', extension);
    path[length] = '\0';
    free(ir->source_path);
    ir->source_path = path;
    return true;
}
static void test_long_virtual_path_validation_work(void) {
    Fixture base_fixture, long_fixture; Pipeline base, longer; init(&base); init(&longer);
    CHECK(setup(&base_fixture)); CHECK(setup(&long_fixture));
    CHECK(set_long_virtual_source_path(&long_fixture.ir, 257));
    bool built = !failures && build(&base_fixture, &base, NULL)
        && build(&long_fixture, &longer, NULL);
    CHECK(built);
    if (built) {
        CHECK(base.lowered.image_instruction_count == longer.lowered.image_instruction_count
            && base.lowered.semantic_plan_count == longer.lowered.semantic_plan_count
            && base.lowered.cleanup_failure_count == longer.lowered.cleanup_failure_count);
        CHECK(longer.lowered.usage.validation_work
            == base.lowered.usage.validation_work + 13281);
        test_exact_validation_work_limit(&base);
        test_exact_validation_work_limit(&longer);
    }
    done(&base); done(&longer); finish(&base_fixture); finish(&long_fixture);
}
static void test_slice_d_p35_usage(const Pipeline *p, size_t validation_work) {
    static const size_t expected[] = {198,16,10,10,10,16,0,0,0,0,0,0,0,33,5,7,11,7,7,54,490,0,0,0,7,14,15};
    expect_slice_d_usage(p, expected, 174472, 174686, 375846, 1820, validation_work + 6706,
        3728384, 3734640);
}
static void test_slice_d_allocation_faults(const Pipeline *p) {
    SolMirRuntimeLoweredProgram candidate; sol_mir_runtime_lowered_program_init(&candidate);
    SolMirRuntimeLoweredProgramBuildRequest request = {&p->conventions, &p->values, &p->cleanup,
        &p->host, &p->handlers, NULL};
    sol_mir_runtime_lowered_program_test_force_persistent_allocation_failure(true);
    CHECK(sol_mir_runtime_lowered_program_build(&request, &candidate, NULL)
        == SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_ALLOCATION_FAILED);
    CHECK(candidate.authentication == 0 && candidate.image_instructions == NULL);
    sol_mir_runtime_lowered_program_test_force_persistent_allocation_failure(false);
    CHECK(sol_mir_runtime_lowered_program_build(&request, &candidate, NULL)
        == SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED);
    sol_mir_runtime_lowered_program_free(&candidate);
    sol_mir_runtime_lowered_program_test_force_build_scratch_allocation_failure(true);
    CHECK(!rebuild_lowered(p, NULL));
    sol_mir_runtime_lowered_program_test_force_build_scratch_allocation_failure(false);
    sol_mir_runtime_lowered_program_test_force_validation_scratch_failure(true);
    CHECK(!rebuild_lowered(p, NULL));
    sol_mir_runtime_lowered_program_test_force_validation_scratch_failure(false);
    CHECK(rebuild_lowered(p, NULL));
    sol_mir_runtime_lowered_program_test_reset_allocation_attempts();
    sol_mir_runtime_lowered_program_test_fail_build_scratch_allocation_after(1);
    CHECK(!rebuild_lowered(p, NULL));
    CHECK(sol_mir_runtime_lowered_program_test_build_scratch_allocation_attempts() == 1
        && sol_mir_runtime_lowered_program_test_persistent_allocation_attempts() == 0);
    sol_mir_runtime_lowered_program_test_reset_allocation_attempts();
    sol_mir_runtime_lowered_program_test_fail_build_scratch_allocation_after(3);
    CHECK(!rebuild_lowered(p, NULL));
    CHECK(sol_mir_runtime_lowered_program_test_build_scratch_allocation_attempts() == 3
        && sol_mir_runtime_lowered_program_test_persistent_allocation_attempts() == 0);
    sol_mir_runtime_lowered_program_test_fail_build_scratch_allocation_after(0);
    sol_mir_runtime_lowered_program_test_reset_allocation_attempts();
    sol_mir_runtime_lowered_program_test_fail_persistent_allocation_after(1);
    CHECK(!rebuild_lowered(p, NULL));
    CHECK(sol_mir_runtime_lowered_program_test_persistent_allocation_attempts() == 1);
    sol_mir_runtime_lowered_program_test_reset_allocation_attempts();
    sol_mir_runtime_lowered_program_test_fail_persistent_allocation_after(3);
    CHECK(!rebuild_lowered(p, NULL));
    CHECK(sol_mir_runtime_lowered_program_test_persistent_allocation_attempts() == 3);
    sol_mir_runtime_lowered_program_test_fail_persistent_allocation_after(0);
    CHECK(rebuild_lowered(p, NULL));
}

static void test_slice_c_cleanup_and_host(Pipeline *p) {
    size_t events = 0, actions = 0, transitions = 0, supplemental = 0, drops = 0;
    size_t inherited = 0, pending = 0, normal = 0, failure = 0, shared = 0, distinct_roots = 0;
    CHECK(p->lowered.handler_frame_count == 0 && p->lowered.handler_marker_count == 0 && p->lowered.handler_exit_count == 0);
    for (size_t i = 0; i < p->lowered.cleanup_failure_count; ++i) {
        const SolMirRuntimeLoweredCleanupFailure *row = &p->lowered.cleanup_failures[i];
        CHECK(row->state == SOL_MIR_RUNTIME_LOWERED_PRESENT && row->record < p->lowered.cleanup_failure_count);
        if (row->kind == SOL_MIR_RUNTIME_LOWERED_CLEANUP_EVENT) {
            const SolMirRuntimeCleanupEvent *source = &p->cleanup.events[row->event]; ++events;
            CHECK(row->event_kind == source->kind && row->origin == source->origin && row->owner == source->owner && row->block == source->block && row->operation == source->operation && row->producer == source->producer && row->inherited_failure_site == source->inherited_failure_site && row->supplemental_site == source->supplemental_site && row->actions.offset == source->actions.offset && row->actions.count == source->actions.count && row->transitions.offset == source->transitions.offset && row->transitions.count == source->transitions.count);
        } else if (row->kind == SOL_MIR_RUNTIME_LOWERED_CLEANUP_ACTION) {
            const SolMirRuntimeCleanupAction *source = &p->cleanup.actions[row->action]; ++actions;
            CHECK(row->action_kind == source->kind && row->action_flags == source->flags && row->target == source->target && row->recipe == source->recipe && row->drop_path == source->drop_path);
        } else if (row->kind == SOL_MIR_RUNTIME_LOWERED_CLEANUP_TRANSITION) {
            const SolMirRuntimeCleanupTransition *source = &p->cleanup.transitions[row->transition]; ++transitions;
            CHECK(row->event == source->event && row->continuation == source->continuation && row->source_edge == source->source_edge && row->destination == source->destination && row->outcome == source->outcome && row->edge_role == source->edge_role && row->actions.offset == source->actions.offset && row->actions.count == source->actions.count && row->primary_failure_wins == source->primary_failure_wins && row->failure_source == source->failure_source && row->failure_site == source->failure_site && row->failure_mask == source->failure_mask);
            inherited += row->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31;
            pending += row->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING && row->failure_site == SOL_MIR_RUNTIME_LOWERED_NONE && row->failure_mask == 0;
            normal += row->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL;
            failure += row->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE;
        } else if (row->kind == SOL_MIR_RUNTIME_LOWERED_CLEANUP_SUPPLEMENTAL_SITE) { ++supplemental; CHECK(row->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_SUPPLEMENTAL_P33 && row->failure_mask == p->cleanup.supplemental_sites[row->record].allowed_codes); }
        else if (row->kind == SOL_MIR_RUNTIME_LOWERED_CLEANUP_DROP_PATH) { ++drops; CHECK(row->drop_root == p->cleanup.drop_paths[row->drop_path].root && row->drop_place == p->cleanup.drop_paths[row->drop_path].place && row->drop_holes.offset == p->cleanup.drop_paths[row->drop_path].holes.offset && row->drop_holes.count == p->cleanup.drop_paths[row->drop_path].holes.count && row->recipe == p->cleanup.drop_paths[row->drop_path].recipe); }
    }
    CHECK(events == p->cleanup.event_count && actions == p->cleanup.action_count && transitions == p->cleanup.transition_count && supplemental == p->cleanup.supplemental_site_count && drops == p->cleanup.drop_path_count && inherited > 0 && normal > 0 && failure > 0);
    SolMirRuntimeLoweredCleanupFailure *local = NULL;
    for (size_t i = 0; i < p->lowered.cleanup_failure_count; ++i) {
        SolMirRuntimeLoweredCleanupFailure *row = &p->lowered.cleanup_failures[i];
        if (row->kind == SOL_MIR_RUNTIME_LOWERED_CLEANUP_TRANSITION
            && row->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_LOCAL_OR_PENDING) {
            local = row; break;
        }
    }
    CHECK(local != NULL);
    if (local) {
        CHECK(local->failure_site < p->conventions.failure_site_count
            && local->failure_mask == (UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT - 1)));
        SolMirRuntimeCleanupFailureSource source = local->failure_source;
        size_t site = local->failure_site; uint32_t mask = local->failure_mask;
        local->failure_source = SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31;
        reseal(&p->lowered); CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
        local->failure_source = source; local->failure_site = SOL_MIR_RUNTIME_LOWERED_NONE;
        reseal(&p->lowered); CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
        local->failure_site = site; local->failure_mask = 0;
        reseal(&p->lowered); CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
        local->failure_mask = mask; reseal(&p->lowered);
        CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
        CHECK(local->failure_site < p->lowered.call_count);
        const SolMirRuntimeLoweredCall *caller = local->failure_site < p->lowered.call_count
            ? &p->lowered.calls[local->failure_site] : NULL;
        CHECK(caller != NULL && caller->state == SOL_MIR_RUNTIME_LOWERED_PRESENT
            && caller->call == local->failure_site
            && caller->failure_site == local->failure_site
            && caller->owner_kind == SOL_MIR_RUNTIME_CALL_OWNER_IMAGE
            && caller->target_kind == SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL
            && caller->call_kind == SOL_IR_CALL_FUNCTION);
        size_t expected_cleanup_rows = 0, expected_call_rows = 0;
        for (size_t i = 0; i < p->lowered.cleanup_failure_count; ++i) {
            const SolMirRuntimeLoweredCleanupFailure *row = &p->lowered.cleanup_failures[i];
            if (row->state == SOL_MIR_RUNTIME_LOWERED_PRESENT
                && row->kind == SOL_MIR_RUNTIME_LOWERED_CLEANUP_TRANSITION
                && row->failure_source == local->failure_source
                && row->failure_mask == local->failure_mask) {
                CHECK(row->failure_site < p->lowered.call_count);
                if (row->failure_site < p->lowered.call_count) {
                    const SolMirRuntimeLoweredCall *related =
                        &p->lowered.calls[row->failure_site];
                    CHECK(related->state == SOL_MIR_RUNTIME_LOWERED_PRESENT
                        && related->call == row->failure_site
                        && related->failure_site == row->failure_site
                        && related->owner_kind == SOL_MIR_RUNTIME_CALL_OWNER_IMAGE
                        && related->target_kind == SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL
                        && related->call_kind == SOL_IR_CALL_FUNCTION);
                }
                ++expected_cleanup_rows;
            }
        }
        for (size_t i = 0; i < p->lowered.call_count; ++i)
            if (p->lowered.calls[i].state == SOL_MIR_RUNTIME_LOWERED_PRESENT
                && p->lowered.calls[i].call == i
                && p->lowered.calls[i].failure_site == i
                && p->lowered.calls[i].owner_kind == SOL_MIR_RUNTIME_CALL_OWNER_IMAGE
                && p->lowered.calls[i].target_kind == SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL
                && p->lowered.calls[i].call_kind == SOL_IR_CALL_FUNCTION) ++expected_call_rows;
        CHECK(expected_cleanup_rows > 0 && expected_cleanup_rows == expected_call_rows);
        size_t left_length = 0, right_length = 0;
        char *left = rendered(&p->lowered, &left_length);
        char *right = rendered(&p->lowered, &right_length);
        CHECK(left && right && left_length == right_length && !memcmp(left, right, left_length));
        if (left) {
            char namespace_value[16], mask_value[] = "00000040";
            const char *line = left; size_t cleanup_matches = 0, call_matches = 0;
            CHECK(snprintf(namespace_value, sizeof namespace_value, "%u",
                (unsigned)SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_LOCAL_OR_PENDING) > 0);
            while (*line) {
                const char *end = strchr(line, '\n');
                size_t length = end ? (size_t)(end - line) : strlen(line);
                const char *site_key; size_t site_length;
                if (length >= strlen("cleanup_failures state=present")
                    && !memcmp(line, "cleanup_failures state=present",
                        strlen("cleanup_failures state=present"))
                    && render_field_is(line, length, "failure-namespace", namespace_value)
                    && render_field_is(line, length, "mask", mask_value)
                    && render_line_field(line, length, "failure-site", &site_key, &site_length)) {
                    CHECK(lowercase_hex_key(site_key, site_length));
                    if (lowercase_hex_key(site_key, site_length)) {
                        char failure_key[65]; size_t links = 0;
                        memcpy(failure_key, site_key, 64);
                        failure_key[64] = '\0';
                        for (const char *call_line = left; *call_line;) {
                            const char *call_end = strchr(call_line, '\n');
                            size_t call_length = call_end
                                ? (size_t)(call_end - call_line) : strlen(call_line);
                            if (call_length >= strlen("calls state=present")
                                && !memcmp(call_line, "calls state=present",
                                    strlen("calls state=present"))
                                && render_field_is(call_line, call_length, "failure-site",
                                    failure_key)) ++links;
                            if (!call_end) break;
                            call_line = call_end + 1;
                        }
                        CHECK(links == 1);
                        call_matches += links;
                    }
                    ++cleanup_matches;
                }
                if (!end) break;
                line = end + 1;
            }
            CHECK(cleanup_matches == expected_cleanup_rows
                && call_matches == expected_call_rows);
        }
        free(left); free(right);
    }
    (void)pending;
    CHECK(p->lowered.host_grant_count == p->host.usage.grants);
    for (size_t i = 0; i < p->lowered.host_requirement_count; ++i) {
        const SolMirRuntimeLoweredHostRequirement *row = &p->lowered.host_requirements[i];
        const SolMirRuntimeHostRequirement *source = &p->host.requirements[i];
        const SolMirRuntimeHostOperation *operation = &p->host.operations[source->operation];
        CHECK(row->requirement == i && row->entry == source->entry && row->root == source->root && row->operation == source->operation && row->call == source->call && row->event == source->event && row->transition == source->failure_transition && row->import_id == operation->import_id && row->signature == operation->signature);
        for (size_t j = 0; j < i; ++j) if (p->host.requirements[j].entry == source->entry && p->host.requirements[j].root == source->root && p->host.requirements[j].operation == source->operation) ++shared;
    }
    for (size_t i = 0; i < p->lowered.host_grant_count; ++i) {
        const SolMirRuntimeLoweredHostGrant *grant = &p->lowered.host_grants[i];
        bool prior = false;
        for (size_t j = 0; j < i; ++j) prior |= p->lowered.host_grants[j].entry == grant->entry && p->lowered.host_grants[j].root == grant->root;
        if (!prior) ++distinct_roots;
        for (size_t q = 0; q < grant->incidences.count; ++q) {
            const SolMirRuntimeLoweredHostIncidence *incidence = &p->lowered.host_incidences[grant->incidences.offset + q];
            CHECK(incidence->grant == i && p->host.requirements[incidence->requirement].entry == grant->entry && p->host.requirements[incidence->requirement].root == grant->root && p->host.requirements[incidence->requirement].operation == grant->operation);
        }
    }
    CHECK(shared > 0 && distinct_roots > 1);
    if (p->cleanup.event_count) { SolMirRuntimeCleanupProducerKind producer = p->lowered.cleanup_failures[0].producer; p->lowered.cleanup_failures[0].producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL; reseal(&p->lowered); CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL)); p->lowered.cleanup_failures[0].producer = producer; reseal(&p->lowered); CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL)); }
    for (size_t i = 0; i < p->lowered.cleanup_failure_count; ++i) if (p->lowered.cleanup_failures[i].kind == SOL_MIR_RUNTIME_LOWERED_CLEANUP_ACTION && p->lowered.cleanup_failures[i].target != SOL_MIR_RUNTIME_LOWERED_NONE) { size_t target = p->lowered.cleanup_failures[i].target; p->lowered.cleanup_failures[i].target = SOL_MIR_RUNTIME_LOWERED_NONE; reseal(&p->lowered); CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL)); p->lowered.cleanup_failures[i].target = target; reseal(&p->lowered); CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL)); break; }
    for (size_t i = 0; i < p->lowered.cleanup_failure_count; ++i) if (p->lowered.cleanup_failures[i].kind == SOL_MIR_RUNTIME_LOWERED_CLEANUP_TRANSITION) { SolMirRuntimeCleanupOutcome outcome = p->lowered.cleanup_failures[i].outcome; p->lowered.cleanup_failures[i].outcome = (SolMirRuntimeCleanupOutcome)((outcome + 1) % 3); reseal(&p->lowered); CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL)); p->lowered.cleanup_failures[i].outcome = outcome; reseal(&p->lowered); CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL)); break; }
    if (p->lowered.host_requirement_count) { SolMirRuntimeSignatureId signature = p->lowered.host_requirements[0].signature; p->lowered.host_requirements[0].signature = SOL_MIR_RUNTIME_LOWERED_NONE; reseal(&p->lowered); CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL)); p->lowered.host_requirements[0].signature = signature; reseal(&p->lowered); CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL)); }
    if (p->lowered.host_incidence_count) { size_t grant = p->lowered.host_incidences[0].grant; p->lowered.host_incidences[0].grant = SOL_MIR_RUNTIME_LOWERED_NONE; reseal(&p->lowered); CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL)); p->lowered.host_incidences[0].grant = grant; reseal(&p->lowered); CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL)); }
    if (p->lowered.host_grant_count > 1) {
        SolMirRuntimeLoweredHostGrant first = p->lowered.host_grants[0], second = p->lowered.host_grants[1];
        SolMirRuntimeLoweredHostIncidence *saved = malloc(p->lowered.host_incidence_count * sizeof *saved);
        CHECK(saved != NULL);
        if (saved) {
            memcpy(saved, p->lowered.host_incidences, p->lowered.host_incidence_count * sizeof *saved);
            p->lowered.host_grants[0] = second;
            p->lowered.host_grants[0].incidences.offset = 0;
            p->lowered.host_grants[1] = first;
            p->lowered.host_grants[1].incidences.offset = second.incidences.count;
            for (size_t i = 0; i < second.incidences.count; ++i) {
                p->lowered.host_incidences[i] = saved[second.incidences.offset + i];
                p->lowered.host_incidences[i].grant = 0;
            }
            for (size_t i = 0; i < first.incidences.count; ++i) {
                p->lowered.host_incidences[second.incidences.count + i] = saved[first.incidences.offset + i];
                p->lowered.host_incidences[second.incidences.count + i].grant = 1;
            }
            reseal(&p->lowered);
            CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
            p->lowered.host_grants[0] = first;
            p->lowered.host_grants[1] = second;
            memcpy(p->lowered.host_incidences, saved, p->lowered.host_incidence_count * sizeof *saved);
            free(saved);
            reseal(&p->lowered);
            CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
        }
    }
}

static void test_slice_c_handlers(void) {
    Fixture first, reordered; Pipeline left, right; init(&left); init(&right);
    CHECK(setup_at(&first, SOL_TEST_SOURCE_DIR "/tests/conformance/p35"));
    CHECK(setup_at(&reordered, SOL_TEST_SOURCE_DIR "/tests/conformance/p35_reordered"));
    bool built = !failures && build_handlers(&first, &left) && build_handlers(&reordered, &right);
    if (!built) { sol_diagnostics_render_human(stderr, &first.p.source, &first.d); sol_diagnostics_render_human(stderr, &reordered.p.source, &reordered.d); }
    CHECK(built); if (built) check_build_work(&right);
    if (built) {
        CHECK(left.lowered.handler_frame_count == 7 && left.lowered.handler_marker_count == 14 && left.lowered.handler_exit_count == 15);
        CHECK(right.lowered.handler_frame_count == 7 && right.lowered.handler_marker_count == 14 && right.lowered.handler_exit_count == 15);
        test_slice_d_p35_usage(&left, 282188);
        test_slice_d_p35_usage(&right, 284468);
        size_t left_length=0,right_length=0;
        char *left_bytes=rendered(&left.lowered,&left_length);
        char *right_bytes=rendered(&right.lowered,&right_length);
        CHECK(left_bytes && right_bytes && left_length==right_length
            && !memcmp(left_bytes,right_bytes,left_length));
        if (left_bytes) {
            char frames[8][65], markers[16][65], exits[16][65];
            CHECK(distinct_render_row_keys(left_bytes, "handler_frames state=", frames, 8) == 7
                && distinct_render_row_keys(left_bytes, "handler_markers state=", markers, 16) == 14
                && distinct_render_row_keys(left_bytes, "handler_exits state=", exits, 16) == 15
                && strstr(left_bytes, " parent=") != NULL
                && strstr(left_bytes, " frame=") != NULL
                && strstr(left_bytes, "cleanup-action=") != NULL
                && strstr(left_bytes, "transition=") != NULL
                && strstr(left_bytes, "provider-access=") != NULL);
        }
        free(left_bytes); free(right_bytes);
        test_slice_d_resource_limits(&left);
        test_exact_validation_work_limit(&right);
        size_t nested = 0, provider = 0;
        for (size_t i = 0; i < left.lowered.handler_frame_count; ++i) {
            const SolMirRuntimeHandlerFramePlan *source = &left.handlers.frames[i];
            const SolMirRuntimeLoweredHandlerFrame *row = &left.lowered.handler_frames[i];
            const SolMirRuntimeLoweredHandlerFrame *other = &right.lowered.handler_frames[i];
            CHECK(row->frame == i && row->handler == source->handler && row->parent == source->parent && row->source_binding == source->source_binding && row->source_operation.target_kind == source->source_operation.target_kind && row->source_operation.instance == source->source_operation.instance && row->source_operation.import == source->source_operation.import && row->source_operation.receiver == source->source_operation.receiver && row->source_operation.root == source->source_operation.root && row->source_operation.effects == source->source_operation.effects && row->source_signature == source->source_signature && row->root_match == source->root_match && row->root == source->authority_root && row->effects == source->effects && row->provider_binding == source->provider_binding && row->provider_place == source->provider_place && row->provider_recipe == source->provider_recipe && row->provider_access == source->provider_access && row->provider == source->provider_internal && row->signature == source->provider_signature && row->enter == source->enter_marker);
            CHECK(row->parent == other->parent && row->source_binding == other->source_binding && row->source_operation.target_kind == other->source_operation.target_kind && row->source_operation.instance == other->source_operation.instance && row->source_operation.import == other->source_operation.import && row->source_operation.receiver == other->source_operation.receiver && row->source_operation.root == other->source_operation.root && row->source_operation.effects == other->source_operation.effects && row->source_signature == other->source_signature && row->root_match == other->root_match && row->root == other->root && row->effects == other->effects && row->provider_binding == other->provider_binding && row->provider_place == other->provider_place && row->provider_recipe == other->provider_recipe && row->provider_access == other->provider_access && row->provider == other->provider && row->signature == other->signature && row->enter == other->enter);
            nested += row->parent != SOL_MIR_RUNTIME_LOWERED_NONE; provider += row->provider != SOL_MIR_RUNTIME_LOWERED_NONE;
        }
        for (size_t i = 0; i < left.lowered.handler_marker_count; ++i) { const SolMirRuntimeHandlerExitMarker *source = &left.handlers.exit_markers[i]; const SolMirRuntimeLoweredHandlerMarker *row = &left.lowered.handler_markers[i]; CHECK(row->frame == source->frame && row->marker == source->instruction && row->image == source->image && row->block == source->block); }
        for (size_t i = 0; i < left.lowered.handler_exit_count; ++i) { const SolMirRuntimeHandlerCleanupExit *source = &left.handlers.cleanup_exits[i]; const SolMirRuntimeLoweredHandlerExit *row = &left.lowered.handler_exits[i]; CHECK(row->frame == source->frame && row->action == source->action && row->transition == source->transition && row->alternative_one_pop); }
        CHECK(nested > 0 && provider == left.lowered.handler_frame_count);
        SolMirMaterializedPlaceId place = left.lowered.handler_frames[0].provider_place; left.lowered.handler_frames[0].provider_place = SOL_MIR_RUNTIME_LOWERED_NONE; reseal(&left.lowered); CHECK(!sol_mir_runtime_lowered_program_validate(&left.lowered, NULL)); left.lowered.handler_frames[0].provider_place = place; reseal(&left.lowered); CHECK(sol_mir_runtime_lowered_program_validate(&left.lowered, NULL));
        SolMirMaterializedBlockId block = left.lowered.handler_markers[0].block; left.lowered.handler_markers[0].block = SOL_MIR_RUNTIME_LOWERED_NONE; reseal(&left.lowered); CHECK(!sol_mir_runtime_lowered_program_validate(&left.lowered, NULL)); left.lowered.handler_markers[0].block = block; reseal(&left.lowered); CHECK(sol_mir_runtime_lowered_program_validate(&left.lowered, NULL));
        bool alternative = left.lowered.handler_exits[0].alternative_one_pop; left.lowered.handler_exits[0].alternative_one_pop = false; reseal(&left.lowered); CHECK(!sol_mir_runtime_lowered_program_validate(&left.lowered, NULL)); left.lowered.handler_exits[0].alternative_one_pop = alternative; reseal(&left.lowered); CHECK(sol_mir_runtime_lowered_program_validate(&left.lowered, NULL));
        SolMirRuntimeLoweredImageBlock *blocks = left.lowered.image_blocks;
        size_t count = left.lowered.image_block_count, capacity = left.lowered.image_block_capacity;
        left.lowered.image_blocks = (SolMirRuntimeLoweredImageBlock *)(void *)left.handlers.frames;
        left.lowered.image_block_count = left.lowered.image_block_capacity = 1;
        CHECK(!sol_mir_runtime_lowered_program_validate(&left.lowered, NULL));
        left.lowered.image_blocks = blocks;
        left.lowered.image_block_count = count; left.lowered.image_block_capacity = capacity;
        CHECK(sol_mir_runtime_lowered_program_validate(&left.lowered, NULL));
    }
    done(&left); done(&right); finish(&first); finish(&reordered);
}

static void test_bound_environment_indirect_graph(void) {
    Fixture fixture; Pipeline pipeline; init(&pipeline);
    CHECK(setup_at(&fixture, SOL_TEST_SOURCE_DIR "/tests/conformance/p36_graph"));
    bool built = !failures && build_graph(&fixture, &pipeline);
    if (!built) sol_diagnostics_render_human(stderr, &fixture.p.source, &fixture.d);
    CHECK(built); if (built) check_build_work(&pipeline);
    if (built) {
        size_t bound = 0, direct_internal = 0, indirect = 0, host_imports = 0;
        for (size_t i = 0; i < pipeline.lowered.predicate_instruction_count; ++i) {
            const SolMirRuntimeLoweredPredicateInstruction *row = &pipeline.lowered.predicate_instructions[i];
            if (row->kind == SOL_MIR_PREDICATE_INST_BOUND_OPERATION) { ++bound; CHECK(row->plan != SOL_MIR_RUNTIME_LOWERED_NONE && row->plan < pipeline.lowered.semantic_plan_count && row->demanded_recipes.count > 0); }
        }
        for (size_t i = 0; i < pipeline.lowered.call_count; ++i) {
            SolMirRuntimeLoweredCall *row = &pipeline.lowered.calls[i];
            direct_internal += row->target_kind == SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL;
            if (row->target_kind == SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE) {
                ++indirect;
                const SolMirLinkageTableEntry *table = &pipeline.concrete.linkage.table_entries[row->table];
                const SolMirRuntimeImport *import = &pipeline.conventions.imports[row->bound_environment_import];
                const SolMirRuntimeCall *input = &pipeline.conventions.calls[i];
                const SolMirPredicateTerminator *term = &pipeline.concrete.operations.predicate_blocks[row->block].terminator;
                CHECK(row->owner_kind == SOL_MIR_RUNTIME_CALL_OWNER_PREDICATE && row->body < pipeline.lowered.predicate_body_count && row->block < pipeline.lowered.predicate_block_count && term->kind == SOL_MIR_PREDICATE_TERM_INVOKE && row->signature == input->signature && row->table == input->table && row->callee.kind == input->callee.kind && row->callee.id == input->callee.id && row->callee.id == term->callee && row->table < pipeline.concrete.linkage.table_entry_count && table->target_kind == SOL_MIR_LINKAGE_TARGET_INTERNAL && table->internal != SOL_MIR_LINKAGE_NONE && row->callee.kind == SOL_MIR_RUNTIME_VALUE_PREDICATE_VALUE && row->callee.id < pipeline.concrete.operations.predicate_value_count && pipeline.conventions.signatures[row->signature].origin == SOL_MIR_RUNTIME_SIGNATURE_FUNCTION_RECIPE && row->import_id == SOL_MIR_RUNTIME_LOWERED_NONE && row->bound_environment_import < pipeline.lowered.import_count && import->kind == SOL_MIR_RUNTIME_IMPORT_RECIPE_BOUND_ENVIRONMENT && import->recipe == pipeline.conventions.signatures[row->signature].function_recipe && pipeline.lowered.imports[row->bound_environment_import].state == SOL_MIR_RUNTIME_LOWERED_PRESENT);
                const SolMirRuntimeCleanupEvent *event=&pipeline.cleanup.events[pipeline.lowered.predicate_terminators[row->block].cleanup_event];for(size_t q=0;q<event->transitions.count;q++){const SolMirRuntimeCleanupTransition *transition=&pipeline.cleanup.transitions[event->transitions.offset+q];if(transition->edge_role==SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE)CHECK(transition->failure_source==SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31&&transition->failure_site==row->failure_site&&transition->failure_mask==(UINT32_C(1)<<(SOL_MIR_RUNTIME_FAILURE_STEP_LIMIT-1)));}
                SolMirLinkageTableId table_id = row->table; row->table = SOL_MIR_RUNTIME_LOWERED_NONE; CHECK(!sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL)); row->table = table_id; CHECK(sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL));
                SolMirRuntimeSignatureId signature = row->signature; row->signature = 0; CHECK(!sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL)); row->signature = signature; CHECK(sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL));
                SolMirRuntimeImportId bound_import = row->bound_environment_import; row->bound_environment_import = SOL_MIR_RUNTIME_LOWERED_NONE; CHECK(!sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL)); row->bound_environment_import = bound_import; CHECK(sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL));
            }
        }
        for (size_t i = 0; i < pipeline.conventions.import_count; ++i) host_imports += pipeline.conventions.imports[i].kind == SOL_MIR_RUNTIME_IMPORT_HOST;
        for (size_t i = 0; i < pipeline.lowered.predicate_terminator_count; ++i) if (pipeline.lowered.predicate_terminators[i].kind == SOL_MIR_PREDICATE_TERM_INVOKE) { const SolMirRuntimeLoweredPredicateTerminator *term = &pipeline.lowered.predicate_terminators[i]; CHECK(term->plan != SOL_MIR_RUNTIME_LOWERED_NONE && term->plan < pipeline.lowered.semantic_plan_count && term->cleanup_event < pipeline.cleanup.event_count && pipeline.cleanup.events[term->cleanup_event].producer == SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_INVOKE && term->failure_site == pipeline.lowered.calls[term->call].failure_site && term->demanded_recipes.count > 0); }
        CHECK(bound > 0 && direct_internal > 0 && indirect == 1 && host_imports == 0 && pipeline.host.requirement_count == 0 && pipeline.lowered.host_requirement_count == 0 && pipeline.lowered.host_grant_count == 0);
        CHECK(sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL));
    }
    done(&pipeline); finish(&fixture);
}
typedef struct {
    char callable_keys[2][65];
    char preinvoke_event_keys[1][65];
    size_t callable_count, preinvoke_count, source_start;
} P36GraphKeys;
static bool collect_p36_graph_keys(const Pipeline *pipeline, P36GraphKeys *keys) {
    memset(keys, 0, sizeof(*keys));
    char callable_arena[16];
    CHECK(snprintf(callable_arena, sizeof(callable_arena), "%u",
        (unsigned)SOL_MIR_RUNTIME_LOWERED_SEMANTIC_CALLABLE) > 0);
    char *text = rendered(&pipeline->lowered, NULL);
    if (!text) return false;
    for (const char *line = text; *line;) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        const char *value; size_t value_length;
        if (length >= strlen("semantic_plans state=")
            && !memcmp(line, "semantic_plans state=", strlen("semantic_plans state="))
            && render_field_is(line, length, "arena", callable_arena)
            && render_line_field(line, length, "key", &value, &value_length)) {
            if (keys->callable_count >= sizeof(keys->callable_keys) / sizeof(*keys->callable_keys)
                || value_length != 64) { free(text); return false; }
            memcpy(keys->callable_keys[keys->callable_count++], value, 64);
        }
        if (length >= strlen("image_terminators state=")
            && !memcmp(line, "image_terminators state=", strlen("image_terminators state="))
            && render_line_field(line, length, "pre-operation-cleanup-event", &value,
                &value_length) && value_length == 64) {
            if (keys->preinvoke_count >= sizeof(keys->preinvoke_event_keys)
                    / sizeof(*keys->preinvoke_event_keys)) { free(text); return false; }
            memcpy(keys->preinvoke_event_keys[keys->preinvoke_count++], value, 64);
        }
        if (!end) break;
        line = end + 1;
    }
    free(text);
    for (size_t i = 0; i < pipeline->lowered.image_terminator_count; ++i) {
        const SolMirRuntimeLoweredImageTerminator *row =
            &pipeline->lowered.image_terminators[i];
        if (row->pre_operation_cleanup_event == SOL_MIR_RUNTIME_LOWERED_NONE) continue;
        const SolMirRuntimeCleanupEvent *event =
            &pipeline->cleanup.events[row->pre_operation_cleanup_event];
        if (event->phase != SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_INVOKE_CALLABLE
            || event->semantic_site >= pipeline->concrete.materialization.semantic_site_count)
            return false;
        size_t plans = 0;
        for (size_t q = 0; q < pipeline->lowered.semantic_plan_count; ++q)
            plans += pipeline->lowered.semantic_plans[q].arena
                    == SOL_MIR_RUNTIME_LOWERED_SEMANTIC_CALLABLE
                && pipeline->lowered.semantic_plans[q].producer == event->semantic_site;
        if (plans != 1) return false;
        keys->source_start = pipeline->concrete.materialization
            .semantic_sites[event->semantic_site].source.start;
    }
    return keys->callable_count == 2 && keys->preinvoke_count == 1;
}
static void test_source_backed_p36_graph_keys(void) {
    Fixture original_fixture, shifted_fixture; Pipeline original, shifted;
    P36GraphKeys original_keys, shifted_keys;
    init(&original); init(&shifted);
    CHECK(setup_at(&original_fixture, SOL_TEST_SOURCE_DIR "/tests/conformance/p36_graph")
        && setup_at(&shifted_fixture,
            SOL_TEST_SOURCE_DIR "/tests/conformance/p36_graph_shifted"));
    bool built = !failures && build_graph(&original_fixture, &original)
        && build_graph(&shifted_fixture, &shifted);
    CHECK(built);
    if (built) {
        CHECK(collect_p36_graph_keys(&original, &original_keys)
            && collect_p36_graph_keys(&shifted, &shifted_keys)
            && original_keys.source_start != shifted_keys.source_start
            && !memcmp(original_keys.callable_keys, shifted_keys.callable_keys,
                sizeof(original_keys.callable_keys))
            && !memcmp(original_keys.preinvoke_event_keys,
                shifted_keys.preinvoke_event_keys,
                sizeof(original_keys.preinvoke_event_keys)));
    }
    done(&shifted); done(&original); finish(&shifted_fixture); finish(&original_fixture);
}
static void test_generic_type_place_keys(void) {
    Fixture fixture; Pipeline pipeline; init(&pipeline);
    CHECK(setup_at(&fixture, SOL_TEST_SOURCE_DIR "/tests/conformance/p36_generic"));
    bool built = !failures && build_named(&fixture, &pipeline, "boxes");
    CHECK(built);
    if (built) {
        const SolMirMaterialization *m = &pipeline.concrete.materialization;
        size_t integer_type = SOL_MIR_RUNTIME_LOWERED_NONE, text_type = SOL_MIR_RUNTIME_LOWERED_NONE;
        for (size_t i = 0; i < m->type_count; ++i) {
            const SolMirMaterializedType *type = &m->types[i];
            if (type->definition >= fixture.ir.definition_count
                || strcmp(fixture.ir.definitions[type->definition].name, "Box")) continue;
            if (integer_type == SOL_MIR_RUNTIME_LOWERED_NONE) integer_type = i;
            else if (i != integer_type) { text_type = i; break; }
        }
        CHECK(integer_type != SOL_MIR_RUNTIME_LOWERED_NONE && text_type != SOL_MIR_RUNTIME_LOWERED_NONE
            && pipeline.concrete.representation.recipe_count == m->type_count);
        size_t integer_place = SOL_MIR_RUNTIME_LOWERED_NONE, text_place = SOL_MIR_RUNTIME_LOWERED_NONE;
        for (size_t i = 0; i < m->place_count; ++i) {
            if (m->places[i].final_type == integer_type) integer_place = i;
            if (m->places[i].final_type == text_type) text_place = i;
        }
        CHECK(integer_place != SOL_MIR_RUNTIME_LOWERED_NONE && text_place != SOL_MIR_RUNTIME_LOWERED_NONE);
        if (integer_place != SOL_MIR_RUNTIME_LOWERED_NONE && text_place != SOL_MIR_RUNTIME_LOWERED_NONE) {
            char integer_key[65], text_key[65]; size_t length = 0;
            char *output = rendered(&pipeline.lowered, &length);
            CHECK(output && length > 0
                && sol_mir_runtime_lowered_program_test_place_key(&pipeline.lowered, integer_place, integer_key)
                && sol_mir_runtime_lowered_program_test_place_key(&pipeline.lowered, text_place, text_key)
                && strcmp(integer_key, text_key) != 0
                && strstr(output, integer_key) != NULL && strstr(output, text_key) != NULL);
            free(output);
        }
    }
    done(&pipeline); finish(&fixture);
}


static void test_callback_prerequisite(void) {
    Fixture f; Pipeline p; init(&p);
    CHECK(setup_at(&f, SOL_TEST_SOURCE_DIR "/tests/conformance/p43_callback_prereq"));
    bool built = !failures && build_named(&f, &p, "launch");
    if (!built) sol_diagnostics_render_human(stderr, &f.p.source, &f.d);
    CHECK(built);
    if (built) {
        size_t instruction = SOL_MIR_RUNTIME_LOWERED_NONE, block = SOL_MIR_RUNTIME_LOWERED_NONE;
        size_t function_values = 0, callback_invokes = 0;
        SolIrCallableId increment = find(&f.ir, "increment", SOL_IR_CALLABLE_FUNCTION);
        SolIrCallableId decrement = find(&f.ir, "decrement", SOL_IR_CALLABLE_FUNCTION);
        const SolMirMaterialization *m = &p.concrete.materialization;
        for (size_t i = 0; i < m->instruction_count; ++i)
            if (m->instructions[i].kind == SOL_MIR_INST_FUNCTION_VALUE) {
                ++function_values;
                CHECK(m->instructions[i].function_callable == increment
                    || m->instructions[i].function_callable == decrement);
                if (m->instructions[i].function_callable == increment) {
                    CHECK(instruction == SOL_MIR_RUNTIME_LOWERED_NONE); instruction = i;
                }
            }
        for (size_t i = 0; i < m->block_count; ++i)
            if (m->blocks[i].terminator.kind == SOL_MIR_TERM_INVOKE
                && m->blocks[i].terminator.call_kind == SOL_IR_CALL_CALLBACK) {
                ++callback_invokes;
                size_t site = m->blocks[i].terminator.callable_site;
                CHECK(site < m->semantic_site_count
                    && m->semantic_sites[site].producer_kind
                        == SOL_MIR_MATERIALIZED_PRODUCER_INSTRUCTION
                    && m->semantic_sites[site].instruction < m->instruction_count);
                if (site < m->semantic_site_count
                    && m->semantic_sites[site].instruction == instruction) {
                    CHECK(block == SOL_MIR_RUNTIME_LOWERED_NONE); block = i;
                }
            }
        CHECK(function_values == 2 && callback_invokes == 2 && instruction != SOL_MIR_RUNTIME_LOWERED_NONE
            && block != SOL_MIR_RUNTIME_LOWERED_NONE);
        if (instruction != SOL_MIR_RUNTIME_LOWERED_NONE && block != SOL_MIR_RUNTIME_LOWERED_NONE) {
            const SolMirMaterializedTerminator *term = &m->blocks[block].terminator;
            CHECK(term->callable_site != SOL_MIR_MATERIALIZED_NONE
                && term->callable_site < m->semantic_site_count);
            if (term->callable_site == SOL_MIR_MATERIALIZED_NONE
                || term->callable_site >= m->semantic_site_count) { done(&p); finish(&f); return; }
            const SolMirMaterializedSemanticSite *site = &m->semantic_sites[term->callable_site];
            CHECK(site->producer_kind == SOL_MIR_MATERIALIZED_PRODUCER_INSTRUCTION
                && site->instruction == instruction && site->block == m->instructions[instruction].block
                && m->instructions[instruction].function_callable == increment
                && p.concrete.representation.recipes[m->instructions[instruction].type].copy_kind == SOL_MIR_COPY_FORBIDDEN);
            SolMirMaterializedTerminator *mutable_term = &m->blocks[block].terminator;
            size_t saved_site = mutable_term->callable_site;
            mutable_term->callable_site = SOL_MIR_MATERIALIZED_NONE;
            CHECK(!sol_mir_materialization_validate_concrete(m, NULL));
            mutable_term->callable_site = saved_site;
            size_t wrong_site = m->bindings[mutable_term->binding].site;
            if (wrong_site < m->semantic_site_count && wrong_site != saved_site) {
                mutable_term->callable_site = wrong_site;
                CHECK(!sol_mir_materialization_validate_concrete(m, NULL));
                mutable_term->callable_site = saved_site;
            }
            size_t initializer = SOL_MIR_MATERIALIZED_NONE;
            for (size_t i = 0; i < m->instruction_count; ++i)
                if (m->instructions[i].kind == SOL_MIR_INST_TEMPORARY_INIT
                    && m->instructions[i].temporary == mutable_term->callee) initializer = i;
            CHECK(initializer != SOL_MIR_MATERIALIZED_NONE);
            if (initializer != SOL_MIR_MATERIALIZED_NONE) {
                SolMirMaterializedInstruction *init = &m->instructions[initializer];
                size_t load_id = init->left < m->value_count ? m->values[init->left].instruction
                    : SOL_MIR_MATERIALIZED_NONE;
                CHECK(load_id < m->instruction_count);
                size_t saved_temp = init->temporary;
                init->temporary = SOL_MIR_MATERIALIZED_NONE;
                CHECK(!sol_mir_materialization_validate_concrete(m, NULL));
                init->temporary = saved_temp;
                if (load_id < m->instruction_count) {
                    SolMirMaterializedInstruction *load = &m->instructions[load_id];
                    size_t saved_place = load->place;
                    load->place = SOL_MIR_MATERIALIZED_NONE;
                    CHECK(!sol_mir_materialization_validate_concrete(m, NULL));
                    load->place = saved_place;
                    size_t store = SOL_MIR_MATERIALIZED_NONE;
                    for (size_t i = 0; i < m->instruction_count; ++i)
                        if (m->instructions[i].kind == SOL_MIR_INST_STORE
                            && m->instructions[i].place < m->place_count
                            && saved_place < m->place_count
                            && m->places[m->instructions[i].place].local
                                == m->places[saved_place].local) store = i;
                    CHECK(store != SOL_MIR_MATERIALIZED_NONE);
                    if (store != SOL_MIR_MATERIALIZED_NONE) {
                        SolMirMaterializedInstruction *write = &m->instructions[store];
                        size_t saved_value = write->left;
                        write->left = SOL_MIR_MATERIALIZED_NONE;
                        CHECK(!sol_mir_materialization_validate_concrete(m, NULL));
                        write->left = saved_value;
                        size_t intervening = SOL_MIR_MATERIALIZED_NONE;
                        for (size_t i = instruction + 1; i < load_id; ++i)
                            if (m->instructions[i].kind == SOL_MIR_INST_STORAGE_LIVE) {
                                intervening = i; break;
                            }
                        /* The fixture keeps the chain adjacent; turn its local-live
                         * marker into a competing reaching store to test ambiguity. */
                        if (intervening == SOL_MIR_MATERIALIZED_NONE)
                            for (size_t i = 0; i < store; ++i)
                                if (m->instructions[i].kind == SOL_MIR_INST_STORAGE_LIVE) {
                                    intervening = i; break;
                                }
                        if (intervening != SOL_MIR_MATERIALIZED_NONE) {
                            SolMirMaterializedInstruction saved = m->instructions[intervening];
                            m->instructions[intervening].kind = SOL_MIR_INST_STORE;
                            m->instructions[intervening].place = saved_place;
                            m->instructions[intervening].left = m->instructions[instruction].result;
                            CHECK(!sol_mir_materialization_validate_concrete(m, NULL));
                            m->instructions[intervening] = saved;
                        }
                        SolMirMaterializedBlockId saved_block = m->instructions[instruction].block;
                        size_t non_dominating = mutable_term->failure_edge < m->edge_count
                            ? m->edges[mutable_term->failure_edge].block
                            : SOL_MIR_MATERIALIZED_NONE;
                        CHECK(non_dominating < m->block_count
                            && non_dominating != saved_block);
                        if (non_dominating < m->block_count && non_dominating != saved_block) {
                            m->instructions[instruction].block = non_dominating;
                            CHECK(!sol_mir_materialization_validate_concrete(m, NULL));
                            m->instructions[instruction].block = saved_block;
                        }
                    }
                }
            }
            CHECK(sol_mir_materialization_validate_concrete(m, NULL));
            const SolMirRuntimeCall *call = NULL;
            for (size_t i = 0; i < p.conventions.call_count; ++i)
                if (p.conventions.calls[i].owner_kind == SOL_MIR_RUNTIME_CALL_OWNER_IMAGE
                    && p.conventions.calls[i].block == block) call = &p.conventions.calls[i];
            CHECK(call && call->target_kind == SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE
                && call->signature < p.conventions.signature_count
                && p.conventions.signatures[call->signature].origin == SOL_MIR_RUNTIME_SIGNATURE_FUNCTION_RECIPE
                && p.conventions.failure_sites[call->failure_site].allowed_codes
                    == (UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_STEP_LIMIT - 1)));
            size_t allocation = SOL_MIR_RUNTIME_LOWERED_NONE, callback = SOL_MIR_RUNTIME_LOWERED_NONE;
            for (size_t i = 0; i < p.cleanup.event_count; ++i) {
                const SolMirRuntimeCleanupEvent *event = &p.cleanup.events[i];
                if (event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION
                    && event->operation == instruction) allocation = i;
                if (event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
                    && event->block == block) callback = i;
            }
            CHECK(allocation != SOL_MIR_RUNTIME_LOWERED_NONE && callback != SOL_MIR_RUNTIME_LOWERED_NONE);
            if (allocation != SOL_MIR_RUNTIME_LOWERED_NONE) {
                const SolMirRuntimeCleanupEvent *event = &p.cleanup.events[allocation];
                CHECK(event->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
                    && event->producer == SOL_MIR_RUNTIME_CLEANUP_PRODUCER_SUPPLEMENTAL_ALLOCATION
                    && event->supplemental_site < p.cleanup.supplemental_site_count
                    && p.cleanup.supplemental_sites[event->supplemental_site].allowed_codes
                        == ((UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED - 1))
                            | (UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_ALLOCATION_LIMIT - 1))));
            }
            if (callback != SOL_MIR_RUNTIME_LOWERED_NONE) {
                const SolMirRuntimeCleanupEvent *event = &p.cleanup.events[callback];
                CHECK(event->transitions.count == 2);
                const SolMirRuntimeCleanupTransition *failure = &p.cleanup.transitions[event->transitions.offset + 1];
                CHECK(failure->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_LOCAL_OR_PENDING);
            }
            SolMirRuntimeLoweredImageInstruction *row = &p.lowered.image_instructions[instruction];
            CHECK(row->plan_family == SOL_MIR_RUNTIME_LOWERED_PLAN_CALLABLE
                && (row->facilities & (SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE
                    | SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION))
                    == (SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE
                        | SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION));
            SolMirInstructionKind saved = row->kind; row->kind = SOL_MIR_INST_CONST_UNIT; reseal(&p.lowered);
            CHECK(!sol_mir_runtime_lowered_program_validate(&p.lowered, NULL)); row->kind = saved; reseal(&p.lowered);
            CHECK(sol_mir_runtime_lowered_program_validate(&p.lowered, NULL));
        }
    }
    done(&p); finish(&f);
}

static void test_callable_hole_prerequisite(void) {
    Fixture f; Pipeline p; init(&p);
    CHECK(setup_at(&f, SOL_TEST_SOURCE_DIR "/tests/conformance/p43_callable_hole_prereq"));
    bool built = !failures && build_named(&f, &p, "launch");
    if (!built) sol_diagnostics_render_human(stderr, &f.p.source, &f.d);
    CHECK(built);
    if (built) {
        const SolMirMaterialization *m = &p.concrete.materialization;
        SolIrCallableId answer = find(&f.ir, "answer", SOL_IR_CALLABLE_FUNCTION);
        size_t producer = SOL_MIR_RUNTIME_LOWERED_NONE;
        size_t projected = SOL_MIR_RUNTIME_LOWERED_NONE;
        size_t callback = SOL_MIR_RUNTIME_LOWERED_NONE;
        size_t root = SOL_MIR_RUNTIME_LOWERED_NONE;
        for (size_t i = 0; i < m->instruction_count; ++i) {
            const SolMirMaterializedInstruction *instruction = &m->instructions[i];
            if (instruction->kind == SOL_MIR_INST_FUNCTION_VALUE
                && instruction->function_callable == answer) {
                CHECK(producer == SOL_MIR_RUNTIME_LOWERED_NONE); producer = i;
            }
            if (instruction->kind == SOL_MIR_INST_LOAD_MOVE && instruction->place < m->place_count
                && m->places[instruction->place].projections.count == 1) {
                CHECK(projected == SOL_MIR_RUNTIME_LOWERED_NONE); projected = i;
            }
        }
        for (size_t i = 0; i < m->block_count; ++i)
            if (m->blocks[i].terminator.kind == SOL_MIR_TERM_INVOKE
                && m->blocks[i].terminator.call_kind == SOL_IR_CALL_CALLBACK) {
                CHECK(callback == SOL_MIR_RUNTIME_LOWERED_NONE); callback = i;
            }
        CHECK(producer != SOL_MIR_RUNTIME_LOWERED_NONE
            && projected != SOL_MIR_RUNTIME_LOWERED_NONE
            && callback != SOL_MIR_RUNTIME_LOWERED_NONE);
        if (projected != SOL_MIR_RUNTIME_LOWERED_NONE) {
            SolMirMaterializedPlaceId field_place = m->instructions[projected].place;
            for (size_t i = 0; i < m->place_count; ++i)
                if (m->places[i].local == m->places[field_place].local
                    && m->places[i].projections.count == 0) root = i;
            CHECK(root != SOL_MIR_RUNTIME_LOWERED_NONE);
            SolMirMaterializedInstruction *load = &((SolMirMaterialization *)m)->instructions[projected];
            SolMirMaterializedTypeId type = load->type;
            load->type = m->type_count;
            CHECK(!sol_mir_materialization_validate_concrete(m, NULL));
            load->type = type;
        }
        if (callback != SOL_MIR_RUNTIME_LOWERED_NONE) {
            SolMirMaterializedTerminator *term
                = &((SolMirMaterialization *)m)->blocks[callback].terminator;
            CHECK(term->callable_site < m->semantic_site_count
                && m->semantic_sites[term->callable_site].instruction == producer
                && m->bindings[term->binding].symbolic_callable == answer);
            size_t site = term->callable_site;
            term->callable_site = SOL_MIR_MATERIALIZED_NONE;
            CHECK(!sol_mir_materialization_validate_concrete(m, NULL));
            term->callable_site = site;
        }
        bool definite_hole = false;
        for (size_t i = 0; i < p.cleanup.action_count; ++i) {
            const SolMirRuntimeCleanupAction *action = &p.cleanup.actions[i];
            if (action->drop_path >= p.cleanup.drop_path_count) continue;
            const SolMirRuntimeCleanupDropPath *path = &p.cleanup.drop_paths[action->drop_path];
            if (path->root != root) continue;
            for (size_t h = 0; h < path->holes.count; ++h) {
                const SolMirRuntimeCleanupDropPath *hole = &p.cleanup.drop_paths[path->holes.offset + h];
                if (projected != SOL_MIR_RUNTIME_LOWERED_NONE
                    && hole->place == m->instructions[projected].place
                    && hole->liveness == SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE)
                    definite_hole = true;
            }
        }
        CHECK(definite_hole && sol_mir_runtime_cleanup_validate(&p.cleanup, NULL));
        CHECK(p.concrete.representation.recipe_count == 6 && m->edge_count == 2
            && m->block_count == 4 && callback == 1);
        if (callback != SOL_MIR_RUNTIME_LOWERED_NONE) {
            const SolMirMaterializedTerminator *term = &m->blocks[callback].terminator;
            CHECK(term->normal_edge == 0 && term->failure_edge == 1
                && m->edges[term->normal_edge].block == 3
                && m->edges[term->failure_edge].block == 2);
            const SolMirRuntimeCall *call = NULL;
            for (size_t i = 0; i < p.conventions.call_count; ++i)
                if (p.conventions.calls[i].block == callback) call = &p.conventions.calls[i];
            CHECK(call != NULL && call->failure_site == 0 && call->table == 0
                && call->signature < p.conventions.signature_count
                && p.conventions.failure_sites[call->failure_site].allowed_codes == UINT32_C(32));
            size_t event_id = SOL_MIR_RUNTIME_LOWERED_NONE, resume_event = SOL_MIR_RUNTIME_LOWERED_NONE;
            const SolMirRuntimeCleanupTransition *failure = NULL;
            for (size_t i = 0; i < p.cleanup.event_count; ++i) {
                const SolMirRuntimeCleanupEvent *event = &p.cleanup.events[i];
                if (event->block == callback
                    && event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR) {
                    event_id = i;
                    for (size_t j = 0; j < event->transitions.count; ++j) {
                        const SolMirRuntimeCleanupTransition *candidate
                            = &p.cleanup.transitions[event->transitions.offset + j];
                        if (candidate->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE)
                            failure = candidate;
                    }
                }
                if (event->block == m->edges[term->failure_edge].block)
                    for (size_t j = 0; j < event->transitions.count; ++j)
                        if (p.cleanup.transitions[event->transitions.offset + j].failure_source
                                == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING)
                            resume_event = i;
            }
            CHECK(event_id != SOL_MIR_RUNTIME_LOWERED_NONE && failure != NULL
                && failure->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_LOCAL_OR_PENDING
                && failure->failure_mask == UINT32_C(32)
                && failure->failure_site == call->failure_site
                && resume_event != SOL_MIR_RUNTIME_LOWERED_NONE);
            if (failure != NULL && resume_event != SOL_MIR_RUNTIME_LOWERED_NONE) {
                SolMirRuntimeCleanupFailureOccurrence occurrence = {
                    SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31,
                    call->failure_site, SOL_MIR_RUNTIME_FAILURE_STEP_LIMIT,
                    SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE, 0, {0},
                    p.conventions.failure_sites[call->failure_site].source,
                };
                SolMirRuntimeCleanupTrace trace;
                size_t action_capacity = p.cleanup.action_count ? p.cleanup.action_count : 1;
                SolMirRuntimeCleanupAction *actions = calloc(action_capacity, sizeof *actions);
                CHECK(actions != NULL);
                SolMirRuntimeCleanupTraceRequest local = {event_id, failure->edge_role,
                    &occurrence, NULL, SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
                CHECK(actions != NULL && sol_mir_runtime_cleanup_test_select(&p.cleanup, &local,
                    actions, action_capacity, &trace)
                    && trace.has_primary && !memcmp(&trace.primary, &occurrence, sizeof occurrence));
                const SolMirRuntimeCleanupEvent *resume = &p.cleanup.events[resume_event];
                const SolMirRuntimeCleanupTransition *pending = NULL;
                for (size_t j = 0; j < resume->transitions.count; ++j) {
                    const SolMirRuntimeCleanupTransition *candidate
                        = &p.cleanup.transitions[resume->transitions.offset + j];
                    if (candidate->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING)
                        pending = candidate;
                }
                CHECK(pending != NULL);
                if (pending != NULL) {
                    SolMirRuntimeCleanupTraceRequest resume_request = {resume_event,
                        pending->edge_role, NULL, &occurrence,
                        SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
                    CHECK(actions != NULL && sol_mir_runtime_cleanup_test_select(&p.cleanup,
                        &resume_request, actions, action_capacity, &trace) && trace.has_primary
                        && !memcmp(&trace.primary, &occurrence, sizeof occurrence));
                }
                free(actions);
            }
            if (call != NULL) {
                SolMirRuntimeCall *mutable_call = &p.conventions.calls[call - p.conventions.calls];
                SolMirLinkageTableId table = mutable_call->table;
                mutable_call->table = SOL_MIR_RUNTIME_NONE;
                CHECK(!sol_mir_runtime_cleanup_validate(&p.cleanup, NULL));
                mutable_call->table = table;
                SolMirRuntimeSignatureId signature = mutable_call->signature;
                mutable_call->signature = SOL_MIR_RUNTIME_NONE;
                CHECK(!sol_mir_runtime_cleanup_validate(&p.cleanup, NULL));
                mutable_call->signature = signature;
                CHECK(sol_mir_runtime_cleanup_validate(&p.cleanup, NULL));
            }
            if (term->callable_site < p.concrete.operations.callable_count) {
                SolMirOperationCallablePlan *plan = NULL;
                for (size_t i = 0; i < p.concrete.operations.callable_count; ++i)
                    if (p.concrete.operations.callables[i].semantic_site == term->callable_site)
                        plan = &p.concrete.operations.callables[i];
                CHECK(plan != NULL);
                if (plan != NULL) {
                    SolMirPlanInstanceId target = plan->target_instance;
                    plan->target_instance = SOL_MIR_PLAN_NONE;
                    CHECK(!sol_mir_runtime_cleanup_validate(&p.cleanup, NULL));
                    plan->target_instance = target;
                    CHECK(sol_mir_runtime_cleanup_validate(&p.cleanup, NULL));
                }
            }
            SolMirMaterializedTerminator *mutable_term
                = &((SolMirMaterialization *)m)->blocks[callback].terminator;
            SolMirMaterializedSemanticSiteId semantic_site = mutable_term->callable_site;
            mutable_term->callable_site = SOL_MIR_MATERIALIZED_NONE;
            CHECK(!sol_mir_runtime_cleanup_validate(&p.cleanup, NULL));
            mutable_term->callable_site = semantic_site;
            SolMirRuntimeCleanupEvent *mutable_event = &p.cleanup.events[event_id];
            SolMirRuntimeSource source = mutable_event->source;
            ++mutable_event->source.start;
            CHECK(!sol_mir_runtime_cleanup_validate(&p.cleanup, NULL));
            mutable_event->source = source;
            CHECK(sol_mir_runtime_cleanup_validate(&p.cleanup, NULL));
        }
    }
    done(&p); finish(&f);
}

typedef enum { EXACT_CONDITIONAL, EXACT_REPAIR, EXACT_REOPEN, EXACT_WHOLE } ExactPairCase;

static bool exact_pair_moved_value(const SolMirMaterialization *m, SolMirMaterializedValueId value,
    SolMirMaterializedPlaceId field, size_t depth) {
    size_t defining = SOL_MIR_RUNTIME_LOWERED_NONE, definitions = 0;
    if (depth > 4 || value >= m->value_count) return false;
    for (size_t i = 0; i < m->instruction_count; ++i)
        if (m->instructions[i].result == value) { defining = i; ++definitions; }
    if (definitions != 1) return false;
    const SolMirMaterializedInstruction *instruction = &m->instructions[defining];
    if (instruction->kind == SOL_MIR_INST_LOAD_MOVE && instruction->place == field) return true;
    if (instruction->kind != SOL_MIR_INST_LOAD_MOVE || instruction->place >= m->place_count
        || m->places[instruction->place].projections.count != 0) return false;
    SolMirMaterializedValueId source = SOL_MIR_RUNTIME_LOWERED_NONE;
    for (size_t i = 0; i < m->instruction_count; ++i) {
        const SolMirMaterializedInstruction *store = &m->instructions[i];
        if (store->kind == SOL_MIR_INST_STORE && store->place < m->place_count
            && m->places[store->place].local == m->places[instruction->place].local) {
            if (source != SOL_MIR_RUNTIME_LOWERED_NONE) return false;
            source = store->left;
        }
    }
    return source != SOL_MIR_RUNTIME_LOWERED_NONE
        && exact_pair_moved_value(m, source, field, depth + 1);
}

static bool exact_pair_same_projection(const SolMirMaterialization *m, size_t left, size_t right) {
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

typedef struct { size_t instruction, event, transition, action, path; } ExactPairDropRoute;

/* The C3.2 gate may consume only the P3.6 row named by this materialized drop,
 * then that event's normal transition and its own action slice. */
static bool exact_pair_drop_route(const Pipeline *p, size_t instruction, size_t root,
    ExactPairDropRoute *route) {
    const SolMirMaterialization *m = &p->concrete.materialization;
    if (instruction >= m->instruction_count || instruction >= p->lowered.image_instruction_count
        || root >= m->place_count || route == NULL) return false;
    const SolMirMaterializedInstruction *item = &m->instructions[instruction];
    const SolMirRuntimeLoweredImageInstruction *row = &p->lowered.image_instructions[instruction];
    if ((item->kind != SOL_MIR_INST_DROP_IF_INITIALIZED
            && item->kind != SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED)
        || row->state != SOL_MIR_RUNTIME_LOWERED_PRESENT || row->instruction != instruction
        || row->block != item->block || row->cleanup_event >= p->cleanup.event_count) return false;
    const SolMirRuntimeCleanupEvent *event = &p->cleanup.events[row->cleanup_event];
    if (event->operation != instruction || event->transitions.offset > p->cleanup.transition_count
        || event->transitions.count > p->cleanup.transition_count - event->transitions.offset) return false;
    size_t transition = SOL_MIR_RUNTIME_LOWERED_NONE, transitions = 0;
    for (size_t i = 0; i < event->transitions.count; ++i) {
        size_t id = event->transitions.offset + i;
        if (p->cleanup.transitions[id].outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL) {
            transition = id; ++transitions;
        }
    }
    if (transitions != 1) return false;
    const SolMirRuntimeCleanupTransition *normal = &p->cleanup.transitions[transition];
    if (normal->actions.offset > p->cleanup.action_count
        || normal->actions.count > p->cleanup.action_count - normal->actions.offset) return false;
    size_t action = SOL_MIR_RUNTIME_LOWERED_NONE, actions = 0;
    for (size_t i = 0; i < normal->actions.count; ++i) {
        size_t id = normal->actions.offset + i;
        const SolMirRuntimeCleanupAction *candidate = &p->cleanup.actions[id];
        if (candidate->kind != SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE || candidate->target != root
            || candidate->drop_path >= p->cleanup.drop_path_count) continue;
        const SolMirRuntimeCleanupDropPath *path = &p->cleanup.drop_paths[candidate->drop_path];
        if (path->root == root && path->place == root && path->recipe == candidate->recipe) {
            action = id; ++actions;
        }
    }
    if (actions != 1) return false;
    *route = (ExactPairDropRoute){instruction, row->cleanup_event, transition, action,
        p->cleanup.actions[action].drop_path};
    return true;
}

static bool exact_pair_final_drop_route(const Pipeline *p, const SolMirMaterializedImage *image,
    size_t root, ExactPairDropRoute *route) {
    const SolMirMaterialization *m = &p->concrete.materialization;
    size_t selected = SOL_MIR_RUNTIME_LOWERED_NONE;
    for (size_t i = 0; i < image->instructions.count; ++i) {
        size_t id = image->instructions.offset + i;
        const SolMirMaterializedInstruction *item = &m->instructions[id];
        bool matches = item->kind == SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED && item->place == root;
        if (item->kind == SOL_MIR_INST_DROP_IF_INITIALIZED && root < m->place_count)
            matches = m->places[root].local == item->local;
        if (matches) selected = id;
    }
    return selected != SOL_MIR_RUNTIME_LOWERED_NONE && exact_pair_drop_route(p, selected, root, route);
}

static void check_exact_pair_case(Pipeline *p, Fixture *f, const char *image_name,
    ExactPairCase kind) {
    const SolMirMaterialization *m = &p->concrete.materialization;
    SolIrCallableId callable = find(&f->ir, image_name, SOL_IR_CALLABLE_FUNCTION);
    size_t image = SOL_MIR_RUNTIME_LOWERED_NONE, field_moves[2] = {
        SOL_MIR_RUNTIME_LOWERED_NONE, SOL_MIR_RUNTIME_LOWERED_NONE};
    size_t field_move_count = 0, field_store = SOL_MIR_RUNTIME_LOWERED_NONE;
    size_t whole_move = SOL_MIR_RUNTIME_LOWERED_NONE, function_values = 0;
    for (size_t i = 0; i < m->image_count; ++i)
        if (m->images[i].source_callable == callable) image = i;
    CHECK(callable != SOL_IR_NONE && image != SOL_MIR_RUNTIME_LOWERED_NONE);
    if (callable == SOL_IR_NONE || image == SOL_MIR_RUNTIME_LOWERED_NONE) return;
    const SolMirMaterializedImage *body = &m->images[image];
    size_t callback_invokes = 0;
    for (size_t i = 0; i < body->blocks.count; ++i)
        callback_invokes += m->blocks[body->blocks.offset + i].terminator.kind == SOL_MIR_TERM_INVOKE
            && m->blocks[body->blocks.offset + i].terminator.call_kind == SOL_IR_CALL_CALLBACK;
    CHECK(callback_invokes == 0);
    for (size_t i = 0; i < body->instructions.count; ++i) {
        size_t id = body->instructions.offset + i;
        const SolMirMaterializedInstruction *item = &m->instructions[id];
        if (item->kind == SOL_MIR_INST_FUNCTION_VALUE) ++function_values;
        if (item->place >= m->place_count) continue;
        if (item->kind == SOL_MIR_INST_LOAD_MOVE && m->places[item->place].projections.count == 1
            && field_move_count < sizeof field_moves / sizeof *field_moves)
            field_moves[field_move_count++] = id;
        if (item->kind == SOL_MIR_INST_STORE && m->places[item->place].projections.count == 1)
            field_store = id;
        if (item->kind == SOL_MIR_INST_LOAD_MOVE && m->places[item->place].projections.count == 0)
            whole_move = id;
    }
    if (kind == EXACT_CONDITIONAL) {
        size_t branch = SOL_MIR_RUNTIME_LOWERED_NONE;
        for (size_t i = 0; i < body->blocks.count; ++i) {
            size_t block = body->blocks.offset + i;
            if (m->blocks[block].terminator.kind == SOL_MIR_TERM_BRANCH) {
                CHECK(branch == SOL_MIR_RUNTIME_LOWERED_NONE); branch = block;
            }
        }
        CHECK(branch != SOL_MIR_RUNTIME_LOWERED_NONE && field_move_count == 1);
        if (branch != SOL_MIR_RUNTIME_LOWERED_NONE && field_move_count == 1) {
            const SolMirMaterializedTerminator *term = &m->blocks[branch].terminator;
            CHECK(term->true_edge < m->edge_count && term->false_edge < m->edge_count
                && term->true_edge != term->false_edge
                && m->edges[term->true_edge].block == m->instructions[field_moves[0]].block
                && m->edges[term->true_edge].block != m->edges[term->false_edge].block);
        }
    }
    if (kind == EXACT_WHOLE) {
        CHECK(whole_move != SOL_MIR_RUNTIME_LOWERED_NONE);
        if (whole_move == SOL_MIR_RUNTIME_LOWERED_NONE) {
            CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL)); return;
        }
        size_t old = m->instructions[whole_move].place, destination = SOL_MIR_RUNTIME_LOWERED_NONE;
        for (size_t i = 0; i < body->instructions.count; ++i) {
            const SolMirMaterializedInstruction *item = &m->instructions[body->instructions.offset + i];
            if (item->kind == SOL_MIR_INST_STORE && item->left == m->instructions[whole_move].result
                && item->place < m->place_count && m->places[item->place].projections.count == 0)
                destination = item->place;
        }
        size_t old_marker = SOL_MIR_RUNTIME_LOWERED_NONE, old_markers = 0;
        for (size_t i = 0; i < body->instructions.count; ++i) {
            size_t instruction = body->instructions.offset + i;
            const SolMirMaterializedInstruction *item = &m->instructions[instruction];
            if (item->kind == SOL_MIR_INST_DROP_IF_INITIALIZED
                && m->places[old].local == item->local) { old_marker = instruction; ++old_markers; }
        }
        CHECK(old_markers == 1 && old_marker < p->lowered.image_instruction_count);
        const SolMirRuntimeLoweredImageInstruction *old_row = old_marker < p->lowered.image_instruction_count
            ? &p->lowered.image_instructions[old_marker] : NULL;
        CHECK(old_row != NULL && old_row->state == SOL_MIR_RUNTIME_LOWERED_PRESENT
            && old_row->instruction == old_marker && old_row->cleanup_event == SOL_MIR_RUNTIME_LOWERED_NONE);
        bool old_targeted = false;
        for (size_t i = 0; i < p->cleanup.action_count; ++i)
            old_targeted |= p->cleanup.actions[i].kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE
                && p->cleanup.actions[i].target == old;
        bool old_event = false;
        for (size_t i = 0; i < p->cleanup.event_count; ++i)
            old_event |= p->cleanup.events[i].operation == old_marker;
        CHECK(!old_targeted && !old_event);
        if (old_row != NULL && old_row->cleanup_event == SOL_MIR_RUNTIME_LOWERED_NONE
            && p->cleanup.event_count != 0) {
            SolMirRuntimeLoweredImageInstruction *mutable_row = &p->lowered.image_instructions[old_marker];
            mutable_row->cleanup_event = 0;
            CHECK(!sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
            mutable_row->cleanup_event = SOL_MIR_RUNTIME_LOWERED_NONE;
            CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
        }
        ExactPairDropRoute route; size_t destination_routes = 0;
        for (size_t i = 0; i < body->instructions.count; ++i) {
            ExactPairDropRoute candidate;
            if (!exact_pair_drop_route(p, body->instructions.offset + i, destination, &candidate)) continue;
            const SolMirRuntimeCleanupDropPath *candidate_path = &p->cleanup.drop_paths[candidate.path];
            if (candidate_path->holes.count == 0
                && candidate_path->liveness == SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE) {
                route = candidate; ++destination_routes;
            }
        }
        bool destination_route = destination != SOL_MIR_RUNTIME_LOWERED_NONE && destination_routes == 1;
        const SolMirRuntimeCleanupDropPath *path = destination_route
            ? &p->cleanup.drop_paths[route.path] : NULL;
        CHECK(destination_route && path != NULL && path->holes.count == 0
            && path->liveness == SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE);
        CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
        return;
    }
    size_t exact_function_plans = 0;
    for (size_t i = 0; i < p->concrete.operations.callable_count; ++i)
        exact_function_plans += p->concrete.operations.callables[i].kind
            == SOL_MIR_CALLABLE_PRODUCER_EXACT_FUNCTION;
    CHECK(function_values == 1 && exact_function_plans == 1
        && field_move_count == (kind == EXACT_REOPEN ? 2 : 1));
    if (field_move_count == 0) return;
    size_t moved = field_moves[field_move_count - 1], root = SOL_MIR_RUNTIME_LOWERED_NONE;
    for (size_t i = 0; i < m->place_count; ++i)
        if (m->places[i].local == m->places[m->instructions[moved].place].local
            && m->places[i].projections.count == 0) root = i;
    CHECK(root != SOL_MIR_RUNTIME_LOWERED_NONE);
    if (root == SOL_MIR_RUNTIME_LOWERED_NONE) return;
    if (kind == EXACT_REPAIR || kind == EXACT_REOPEN)
        CHECK(field_store != SOL_MIR_RUNTIME_LOWERED_NONE
            && exact_pair_same_projection(m, m->instructions[field_store].place,
                m->instructions[field_moves[0]].place)
            && exact_pair_moved_value(m, m->instructions[field_store].left,
                m->instructions[field_moves[0]].place, 0));
    ExactPairDropRoute route;
    bool routed = kind != EXACT_CONDITIONAL && exact_pair_final_drop_route(p, body, root, &route);
    if (kind == EXACT_CONDITIONAL) {
        size_t routes = 0;
        for (size_t i = 0; i < body->instructions.count; ++i) {
            ExactPairDropRoute candidate;
            if (!exact_pair_drop_route(p, body->instructions.offset + i, root, &candidate)) continue;
            const SolMirRuntimeCleanupDropPath *candidate_path = &p->cleanup.drop_paths[candidate.path];
            if (candidate_path->liveness == SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL
                && candidate_path->holes.count == 1) { route = candidate; ++routes; }
        }
        routed = routes == 1;
    }
    CHECK(routed);
    if (!routed) { CHECK(sol_mir_runtime_lowered_program_validate(&p->lowered, NULL)); return; }
    const SolMirRuntimeCleanupAction *action = &p->cleanup.actions[route.action];
    const SolMirRuntimeCleanupDropPath *path = &p->cleanup.drop_paths[route.path];
    CHECK((kind == EXACT_REPAIR) == ((action->flags & SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED) == 0));
    if (kind == EXACT_REPAIR)
        CHECK(path->holes.count == 0 && path->liveness == SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE);
    else {
        CHECK(path->holes.count == 1 && path->liveness == SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL
            && path->holes.offset < p->cleanup.drop_path_count);
        if (path->holes.offset < p->cleanup.drop_path_count) {
            const SolMirRuntimeCleanupDropPath *hole = &p->cleanup.drop_paths[path->holes.offset];
            CHECK(hole->place == m->instructions[moved].place
                && hole->liveness == (kind == EXACT_CONDITIONAL
                    ? SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL
                    : SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE));
        }
    }
    SolMirRuntimeCleanupDropPath saved = p->cleanup.drop_paths[route.path];
    p->cleanup.drop_paths[route.path].liveness = saved.liveness == SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE
        ? SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL : SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE;
    CHECK(!sol_mir_runtime_cleanup_validate(&p->cleanup, NULL)
        && !sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
    p->cleanup.drop_paths[route.path] = saved;
    CHECK(sol_mir_runtime_cleanup_validate(&p->cleanup, NULL)
        && sol_mir_runtime_lowered_program_validate(&p->lowered, NULL));
    if (kind == EXACT_REOPEN) {
        size_t moved_root = SOL_MIR_RUNTIME_LOWERED_NONE;
        for (size_t i = 0; i < body->instructions.count; ++i) {
            const SolMirMaterializedInstruction *store = &m->instructions[body->instructions.offset + i];
            if (store->kind == SOL_MIR_INST_STORE && store->left == m->instructions[moved].result
                && store->place < m->place_count && m->places[store->place].projections.count == 0)
                moved_root = store->place;
        }
        ExactPairDropRoute moved_route;
        bool moved_cleanup = moved_root != SOL_MIR_RUNTIME_LOWERED_NONE
            && exact_pair_final_drop_route(p, body, moved_root, &moved_route);
        CHECK(moved_cleanup && moved_root != root
            && p->cleanup.drop_paths[moved_route.path].holes.count == 0);
    }
}

static void check_exact_pair_wrapper(const Pipeline *p, const Fixture *f, const char *name,
    bool expected) {
    const SolMirMaterialization *m = &p->concrete.materialization;
    SolIrCallableId wrapper = find(&f->ir, name, SOL_IR_CALLABLE_FUNCTION);
    SolIrCallableId conditional = find(&f->ir, "conditional", SOL_IR_CALLABLE_FUNCTION);
    size_t image = SOL_MIR_RUNTIME_LOWERED_NONE, invoke = SOL_MIR_RUNTIME_LOWERED_NONE;
    for (size_t i = 0; i < m->image_count; ++i)
        if (m->images[i].source_callable == wrapper) image = i;
    if (image != SOL_MIR_RUNTIME_LOWERED_NONE)
        for (size_t i = 0; i < m->images[image].blocks.count; ++i) {
            size_t block = m->images[image].blocks.offset + i;
            if (m->blocks[block].terminator.kind == SOL_MIR_TERM_INVOKE) invoke = block;
        }
    CHECK(wrapper != SOL_IR_NONE && conditional != SOL_IR_NONE && image != SOL_MIR_RUNTIME_LOWERED_NONE
        && invoke != SOL_MIR_RUNTIME_LOWERED_NONE);
    if (invoke == SOL_MIR_RUNTIME_LOWERED_NONE) return;
    const SolMirMaterializedTerminator *term = &m->blocks[invoke].terminator;
    CHECK(term->call_kind == SOL_IR_CALL_FUNCTION && term->binding < m->binding_count
        && m->bindings[term->binding].symbolic_callable == conditional
        && term->arguments.count == 1 && term->arguments.offset < m->call_argument_count);
    if (term->arguments.count != 1 || term->arguments.offset >= m->call_argument_count) return;
    SolMirMaterializedTemporaryId temporary = m->call_arguments[term->arguments.offset].temporary;
    SolMirMaterializedValueId value = SOL_MIR_RUNTIME_LOWERED_NONE;
    for (size_t i = 0; i < m->images[image].instructions.count; ++i) {
        const SolMirMaterializedInstruction *item = &m->instructions[m->images[image].instructions.offset + i];
        if (item->kind == SOL_MIR_INST_TEMPORARY_INIT && item->temporary == temporary) value = item->left;
    }
    size_t definition = SOL_MIR_RUNTIME_LOWERED_NONE;
    for (size_t i = 0; i < m->instruction_count; ++i)
        if (m->instructions[i].result == value) definition = i;
    CHECK(definition != SOL_MIR_RUNTIME_LOWERED_NONE
        && m->instructions[definition].kind == SOL_MIR_INST_CONST_BOOL
        && m->instructions[definition].boolean == expected);
}

static void test_callable_hole_exact(void) {
    static const struct { const char *root, *image; ExactPairCase kind; } cases[] = {
        {"conditional_true", "conditional", EXACT_CONDITIONAL},
        {"conditional_false", "conditional", EXACT_CONDITIONAL},
        {"repair", "repair", EXACT_REPAIR}, {"reopen", "reopen", EXACT_REOPEN},
        {"whole_root", "whole_root", EXACT_WHOLE},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        Fixture f; Pipeline p; init(&p);
        CHECK(setup_at(&f, SOL_TEST_SOURCE_DIR "/tests/conformance/p43_callable_hole_exact"));
        bool built = build_named(&f, &p, cases[i].root);
        if (!built) sol_diagnostics_render_human(stderr, &f.p.source, &f.d);
        CHECK(built);
        if (built) {
            check_exact_pair_case(&p, &f, cases[i].image, cases[i].kind);
            if (cases[i].kind == EXACT_CONDITIONAL)
                check_exact_pair_wrapper(&p, &f, cases[i].root,
                    !strcmp(cases[i].root, "conditional_true"));
            for (size_t q = 0; q < p.concrete.materialization.image_count; ++q) {
                const SolMirMaterializedImage *image = &p.concrete.materialization.images[q];
                for (size_t b = 0; b < image->blocks.count; ++b) {
                    const SolMirMaterializedTerminator *term = &p.concrete.materialization.blocks[
                        image->blocks.offset + b].terminator;
                    CHECK(term->kind != SOL_MIR_TERM_INVOKE || term->call_kind != SOL_IR_CALL_CALLBACK);
                }
            }
            for (size_t q = 0; q < p.conventions.call_count; ++q)
                CHECK(p.conventions.calls[q].call_kind != SOL_IR_CALL_CALLBACK);
        }
        done(&p); finish(&f);
    }
}

static void test_immediate_method_prerequisite(void) {
    Fixture f; Pipeline p; init(&p);
    CHECK(setup_at(&f, SOL_TEST_SOURCE_DIR "/tests/conformance/p43_method_prereq"));
    bool built = !failures && build_named(&f, &p, "launch");
    if (!built) sol_diagnostics_render_human(stderr, &f.p.source, &f.d);
    CHECK(built);
    if (built) {
        SolIrCallableId read_requirement = find(&f.ir, "read",
            SOL_IR_CALLABLE_TRAIT_REQUIREMENT);
        SolIrCallableId bump_requirement = find(&f.ir, "bump",
            SOL_IR_CALLABLE_TRAIT_REQUIREMENT);
        SolIrCallableId read_method = find(&f.ir, "read",
            SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION);
        SolIrCallableId bump_method = find(&f.ir, "bump",
            SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION);
        size_t method_references = 0, method_demands = 0;
        size_t read_call = SOL_MIR_RUNTIME_LOWERED_NONE;
        size_t bump_call = SOL_MIR_RUNTIME_LOWERED_NONE;
        CHECK(read_requirement != SOL_IR_NONE && bump_requirement != SOL_IR_NONE
            && read_method != SOL_IR_NONE && bump_method != SOL_IR_NONE);
        for (size_t i = 0; i < p.concrete.program.reference_count; ++i) {
            const SolMirProgramReference *reference = &p.concrete.program.references[i];
            if (reference->kind != SOL_MIR_PROGRAM_REFERENCE_INVOKE) continue;
            const SolIrExpression *call = reference->source.expression < f.ir.expression_count
                ? &f.ir.expressions[reference->source.expression] : NULL;
            if (call != NULL && call->kind == SOL_IR_EXPR_CALL
                && call->as.call.kind == SOL_IR_CALL_METHOD) {
                ++method_references;
                CHECK((call->as.call.callable == read_requirement
                        && reference->target == read_method)
                    || (call->as.call.callable == bump_requirement
                        && reference->target == bump_method));
            }
        }
        for (size_t i = 0; i < p.concrete.plan.demand_count; ++i) {
            const SolMirPlanDemand *demand = &p.concrete.plan.demands[i];
            if (demand->kind != SOL_MIR_PLAN_DEMAND_INVOKE
                || demand->dispatch_trait == SOL_IR_NONE) continue;
            ++method_demands;
            CHECK((demand->symbolic_target == read_requirement
                    && demand->dispatch_requirement == read_requirement
                    && demand->instance < p.concrete.plan.instance_count
                    && p.concrete.plan.instances[demand->instance].callable == read_method)
                || (demand->symbolic_target == bump_requirement
                    && demand->dispatch_requirement == bump_requirement
                    && demand->instance < p.concrete.plan.instance_count
                    && p.concrete.plan.instances[demand->instance].callable == bump_method));
        }
        CHECK(method_references == 2 && method_demands == 2);
        /* P2.1's independent direct-method proof must reject every join it
           relies on; canonical rebuilding remains a second line of defense. */
        SolMir *launch_mir = NULL;
        for (size_t i = 0; i < p.concrete.program.template_count; ++i)
            if (p.concrete.program.templates[i].callable
                    == find(&f.ir, "launch", SOL_IR_CALLABLE_FUNCTION))
                launch_mir = &p.concrete.program.templates[i].mir;
        SolMirTerminator *invoke = NULL;
        SolMirProgramReference *invoke_reference = NULL;
        if (launch_mir != NULL) for (size_t i = 0; i < launch_mir->block_count; ++i)
            if (launch_mir->blocks[i].terminator.kind == SOL_MIR_TERM_INVOKE
                && launch_mir->blocks[i].terminator.as.invoke.kind == SOL_IR_CALL_METHOD
                && launch_mir->blocks[i].terminator.as.invoke.callable == read_method)
                invoke = &launch_mir->blocks[i].terminator;
        if (invoke != NULL) for (size_t i = 0; i < p.concrete.program.reference_count; ++i)
            if (p.concrete.program.references[i].kind == SOL_MIR_PROGRAM_REFERENCE_INVOKE
                && p.concrete.program.references[i].source.expression
                    == invoke->as.invoke.source_expression
                && p.concrete.program.references[i].target == read_method)
                invoke_reference = &p.concrete.program.references[i];
        CHECK(invoke != NULL && invoke_reference != NULL);
        if (invoke != NULL && invoke_reference != NULL) {
            SolIrExpression *source = &f.ir.expressions[invoke->as.invoke.source_expression];
            SolIrDispatchEvidence *evidence = &f.ir.evidence[source->as.call.evidence.offset];
            SolIrCallableId target = invoke->as.invoke.callable;
            invoke->as.invoke.callable = read_requirement;
            CHECK(!sol_mir_program_validate(&p.concrete.program, NULL));
            invoke->as.invoke.callable = target;
            SolSpan span = invoke->span;
            if (invoke->span.start < invoke->span.end) {
                ++invoke->span.start;
                CHECK(!sol_mir_program_validate(&p.concrete.program, NULL));
                invoke->span = span;
            }
            SolIrCallableId reference_target = invoke_reference->target;
            invoke_reference->target = read_requirement;
            CHECK(!sol_mir_program_validate(&p.concrete.program, NULL));
            invoke_reference->target = reference_target;
            SolIrCallableId requirement = source->as.call.callable;
            source->as.call.callable = read_method;
            CHECK(!sol_mir_program_validate(&p.concrete.program, NULL));
            source->as.call.callable = requirement;
            SolIrCallableId method = evidence->method;
            evidence->method = read_requirement;
            CHECK(!sol_mir_program_validate(&p.concrete.program, NULL));
            evidence->method = method;
            SolIrTypeId type = evidence->type;
            evidence->type = SOL_IR_NONE;
            CHECK(!sol_mir_program_validate(&p.concrete.program, NULL));
            evidence->type = type;
            SolIrDefinitionId implementation = evidence->implementation;
            evidence->implementation = SOL_IR_NONE;
            CHECK(!sol_mir_program_validate(&p.concrete.program, NULL));
            evidence->implementation = implementation;
            SolIrGenericParameterId binding = evidence->binding;
            evidence->binding = 0;
            CHECK(!sol_mir_program_validate(&p.concrete.program, NULL));
            evidence->binding = binding;
            SolAccessMode access = source->as.call.receiver_access;
            source->as.call.receiver_access = SOL_ACCESS_OWNED;
            CHECK(!sol_mir_program_validate(&p.concrete.program, NULL));
            source->as.call.receiver_access = access;
            bool forwarded = evidence->forwarded;
            evidence->forwarded = true;
            CHECK(!sol_mir_program_validate(&p.concrete.program, NULL));
            evidence->forwarded = forwarded;
            CHECK(sol_mir_program_validate(&p.concrete.program, NULL));
        }
        for (size_t i = 0; i < p.lowered.call_count; ++i) {
            const SolMirRuntimeLoweredCall *call = &p.lowered.calls[i];
            if (call->call_kind != SOL_IR_CALL_METHOD) continue;
            CHECK(call->signature < p.conventions.signature_count
                && call->operands.count == 1 && call->operands.offset < p.conventions.operand_count);
            if (call->signature >= p.conventions.signature_count
                || call->operands.count != 1 || call->operands.offset >= p.conventions.operand_count)
                continue;
            const SolMirRuntimeSignature *signature = &p.conventions.signatures[call->signature];
            const SolMirRuntimeSignatureSlot *slot = signature->slots.count == 1
                    && signature->slots.offset < p.conventions.signature_slot_count
                ? &p.conventions.signature_slots[signature->slots.offset] : NULL;
            const SolMirRuntimeOperand *operand = &p.conventions.operands[call->operands.offset];
            CHECK(slot != NULL && slot->role == SOL_MIR_RUNTIME_SLOT_RECEIVER
                && operand->signature_slot == signature->slots.offset
                && operand->value.kind == SOL_MIR_RUNTIME_VALUE_MATERIALIZED_PLACE);
            if (slot == NULL) continue;
            if (slot->access == SOL_ACCESS_SHARED) {
                CHECK(read_call == SOL_MIR_RUNTIME_LOWERED_NONE
                    && call->writebacks.count == 0);
                read_call = i;
            } else if (slot->access == SOL_ACCESS_EXCLUSIVE) {
                CHECK(bump_call == SOL_MIR_RUNTIME_LOWERED_NONE
                    && call->writebacks.count == 1
                    && call->writebacks.offset < p.conventions.writeback_count);
                bump_call = i;
            } else CHECK(false);
        }
        CHECK(read_call != SOL_MIR_RUNTIME_LOWERED_NONE
            && bump_call != SOL_MIR_RUNTIME_LOWERED_NONE);
        if (bump_call != SOL_MIR_RUNTIME_LOWERED_NONE) {
            const SolMirRuntimeLoweredCall *call = &p.lowered.calls[bump_call];
            SolMirRuntimeWriteback *writeback
                = &p.conventions.writebacks[call->writebacks.offset];
            CHECK(writeback->receiver && writeback->operand == call->operands.offset);
            size_t action = SOL_MIR_RUNTIME_LOWERED_NONE;
            for (size_t i = 0; i < p.cleanup.event_count; ++i) {
                const SolMirRuntimeCleanupEvent *event = &p.cleanup.events[i];
                if (event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
                    || event->block != call->block) continue;
                for (size_t a = 0; a < event->actions.count; ++a) {
                    size_t id = event->actions.offset + a;
                    if (id < p.cleanup.action_count
                        && p.cleanup.actions[id].kind
                            == SOL_MIR_RUNTIME_CLEANUP_ACTION_WRITEBACK) {
                        CHECK(action == SOL_MIR_RUNTIME_LOWERED_NONE);
                        action = id;
                    }
                }
            }
            CHECK(action != SOL_MIR_RUNTIME_LOWERED_NONE);
            if (action != SOL_MIR_RUNTIME_LOWERED_NONE) {
                CHECK(p.cleanup.actions[action].flags
                    == SOL_MIR_RUNTIME_CLEANUP_ACTION_NORMAL_ONLY);
                unsigned flags = p.cleanup.actions[action].flags;
                p.cleanup.actions[action].flags = 0;
                CHECK(!sol_mir_runtime_cleanup_validate(&p.cleanup, NULL));
                p.cleanup.actions[action].flags = flags;
                CHECK(sol_mir_runtime_cleanup_validate(&p.cleanup, NULL));
            }
            bool receiver = writeback->receiver;
            writeback->receiver = false;
            CHECK(!sol_mir_runtime_lowered_program_validate(&p.lowered, NULL));
            writeback->receiver = receiver;
            CHECK(sol_mir_runtime_lowered_program_validate(&p.lowered, NULL));
            SolMirRuntimeSlice writebacks = p.lowered.calls[bump_call].writebacks;
            p.lowered.calls[bump_call].writebacks.count = 0;
            reseal(&p.lowered);
            CHECK(!sol_mir_runtime_lowered_program_validate(&p.lowered, NULL));
            p.lowered.calls[bump_call].writebacks = writebacks;
            reseal(&p.lowered);
            CHECK(sol_mir_runtime_lowered_program_validate(&p.lowered, NULL));
        }
    }
    done(&p); finish(&f);
}

int main(void) { test_vocabulary();Fixture f;Pipeline p;init(&p);CHECK(setup(&f));bool built=!failures&&build(&f,&p,NULL);if(!built)sol_diagnostics_render_human(stderr,&f.p.source,&f.d);CHECK(built);if(built)check_build_work(&p);CHECK(sol_mir_runtime_lowered_program_validate(&p.lowered,NULL));test_e6_census_and_missing(&p);test_slice_b_graph_joins(&p);test_executable_plan_event_value_joins(&p);test_slice_c_cleanup_and_host(&p);test_slice_d_raw_owner_preflight(&p);test_slice_d_e6_usage(&p);test_slice_e_render(&p);test_slice_d_resource_limits(&p);test_slice_d_allocation_faults(&p);{ Fixture pre_fixture; CHECK(setup(&pre_fixture)); if(!failures) check_pre_operation_render_stability(&pre_fixture); finish(&pre_fixture); }test_erased_loop_census();test_propagation_value_demand();test_pattern_copy_pipeline();test_nontrivial_copy_demand();test_callback_prerequisite();test_callable_hole_prerequisite();test_callable_hole_exact();test_immediate_method_prerequisite();test_bound_environment_indirect_graph();test_source_backed_p36_graph_keys();test_generic_type_place_keys();test_slice_c_handlers();test_long_virtual_path_validation_work();FILE*s=tmpfile();CHECK(s&&sol_mir_runtime_lowered_program_render(s,&p.lowered));if(s)fclose(s);uint64_t seal=p.lowered.authentication;p.lowered.authentication^=1;CHECK(!sol_mir_runtime_lowered_program_validate(&p.lowered,NULL));p.lowered.authentication=seal;CHECK(sol_mir_runtime_lowered_program_validate(&p.lowered,NULL));SolMirRuntimeLoweredProgramLimits exact=p.lowered.limits;exact.max_owned_bytes=p.lowered.usage.owned_bytes? p.lowered.usage.owned_bytes-1:1;Pipeline bad;init(&bad);CHECK(!build(&f,&bad,&exact)&&!bad.lowered.image_instructions);done(&bad);done(&p);finish(&f);return failures?1:0; }
