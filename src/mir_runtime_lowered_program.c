#include "sol/mir_runtime_lowered_program.h"
#include "mir_runtime_lowered_program_internal.h"
#include "mir_linkage_internal.h"
#include <stdlib.h>
#include <string.h>

/* The renderer owns one fixed-size staging record per rendered relation.  The
 * conservative bound is persisted so render has a checked budget before its
 * first allocation. */
enum { SOL_MIR_RUNTIME_LOWERED_RENDER_LINE_CAPACITY = 4096,
    SOL_MIR_RUNTIME_LOWERED_RENDER_HEADER_CAPACITY = 1024 };
typedef struct { const char *table;
    char text[SOL_MIR_RUNTIME_LOWERED_RENDER_LINE_CAPACITY]; } LoweredRenderLine;
_Static_assert(sizeof(LoweredRenderLine) == SOL_MIR_RUNTIME_LOWERED_RENDER_LINE_CAPACITY
    + sizeof(const char *), "render line has no unmetered padding");
#ifdef SOL_MIR_PLAN_TEST_HOOKS
static _Thread_local bool fail_build_scratch,fail_persistent,fail_validation,fail_render;
static _Thread_local size_t allocation_attempts,build_scratch_allocation_attempts,
    persistent_allocation_attempts,fail_build_scratch_after,fail_persistent_after;
static _Thread_local size_t render_write_attempts;
static _Thread_local SolMirRuntimeLoweredProgramTestBuildWork last_build_work;
void sol_mir_runtime_lowered_program_test_force_build_scratch_allocation_failure(bool x){fail_build_scratch=x;}void sol_mir_runtime_lowered_program_test_force_persistent_allocation_failure(bool x){fail_persistent=x;}void sol_mir_runtime_lowered_program_test_force_validation_scratch_failure(bool x){fail_validation=x;}size_t sol_mir_runtime_lowered_program_test_allocation_attempts(void){return allocation_attempts;}void sol_mir_runtime_lowered_program_test_reset_allocation_attempts(void){allocation_attempts=0;build_scratch_allocation_attempts=0;persistent_allocation_attempts=0;}
void sol_mir_runtime_lowered_program_test_force_render_allocation_failure(bool x){fail_render=x;}
void sol_mir_runtime_lowered_program_test_reset_render_write_attempts(void){render_write_attempts=0;}
size_t sol_mir_runtime_lowered_program_test_render_write_attempts(void){return render_write_attempts;}
size_t sol_mir_runtime_lowered_program_test_build_scratch_allocation_attempts(void){return build_scratch_allocation_attempts;}
size_t sol_mir_runtime_lowered_program_test_persistent_allocation_attempts(void){return persistent_allocation_attempts;}
void sol_mir_runtime_lowered_program_test_fail_build_scratch_allocation_after(size_t x){fail_build_scratch_after=x;}
void sol_mir_runtime_lowered_program_test_fail_persistent_allocation_after(size_t x){fail_persistent_after=x;}
SolMirRuntimeLoweredProgramTestBuildWork sol_mir_runtime_lowered_program_test_last_build_work(void){return last_build_work;}
#endif
static bool add(size_t*x,size_t y){if(y>SIZE_MAX-*x)return false;*x+=y;return true;}static bool mul(size_t x,size_t y,size_t*out){if(x&&y>SIZE_MAX/x)return false;*out=x*y;return true;}static bool report(SolDiagnostics*d,const char*m){if(d)sol_diagnostics_add(d,"SOL-MIR-RUNTIME-LOWERED-001",SOL_SEVERITY_ERROR,(SolSpan){0},m);return false;}
/* Builder helpers are deliberately meter-parameter-free.  The meter is scoped
 * to build(), including all of its early exits, so no construction work can
 * accidentally run outside the caller's budget. */
static _Thread_local SolMirRuntimeLoweredWorkMeter *active_build_meter;
static bool build_tick(void) {
    return !active_build_meter || sol_mir_runtime_lowered_tick(active_build_meter);
}
#define BUILD_TICK() build_tick()
static void *allocation(size_t n,size_t z,bool scratch) {
    if(!n||!z||n>SIZE_MAX/z)return NULL;
    if (!BUILD_TICK()) return NULL;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    ++allocation_attempts;
    if (scratch) {
        ++build_scratch_allocation_attempts;
        if (fail_build_scratch || (fail_build_scratch_after
                && build_scratch_allocation_attempts >= fail_build_scratch_after)) return NULL;
    } else {
        ++persistent_allocation_attempts;
        if (fail_persistent || (fail_persistent_after
                && persistent_allocation_attempts >= fail_persistent_after)) return NULL;
    }
#else
    (void)scratch;
#endif
    return calloc(n,z);
}
void *sol_mir_runtime_lowered_program_internal_validation_scratch(size_t n,size_t z) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    ++allocation_attempts;
    if(fail_validation)return NULL;
#endif
    return n&&z&&n<=SIZE_MAX/z?calloc(n,z):NULL;
}
static bool zero(SolMirRuntimeLoweredProgramLimits x){return !memcmp(&x,&(SolMirRuntimeLoweredProgramLimits){0},sizeof x);}
static bool full(SolMirRuntimeLoweredProgramLimits x) {
#define CHECK(member,type,singular) if(!x.max_##member)return false;
    SOL_MIR_RUNTIME_LOWERED_TABLES(CHECK)
#undef CHECK
    return x.max_owned_bytes&&x.max_build_scratch_bytes&&x.max_build_work&&x.max_validation_scratch_bytes&&x.max_validation_work&&x.max_render_bytes&&x.max_render_scratch_bytes;
}
void sol_mir_runtime_lowered_program_init(SolMirRuntimeLoweredProgram*o){if(o)memset(o,0,sizeof*o);}
void sol_mir_runtime_lowered_program_free(SolMirRuntimeLoweredProgram*o){
    if(!o)return;
#define FREE(member,type,singular) free(o->member);
    SOL_MIR_RUNTIME_LOWERED_TABLES(FREE)
#undef FREE
    sol_mir_runtime_lowered_program_init(o);
}
SolMirRuntimeLoweredProgramLimits sol_mir_runtime_lowered_program_default_limits(void){
    SolMirRuntimeLoweredProgramLimits x={0};
#define SET(member,type,singular) x.max_##member=16000000;
    SOL_MIR_RUNTIME_LOWERED_TABLES(SET)
#undef SET
    x.max_owned_bytes=1024u*1024u*1024u;x.max_build_scratch_bytes=256u*1024u*1024u;x.max_build_work=(size_t)4000000000ULL;x.max_validation_scratch_bytes=256u*1024u*1024u;x.max_validation_work=(size_t)4000000000ULL;x.max_render_bytes=64u*1024u*1024u;x.max_render_scratch_bytes=64u*1024u*1024u;return x;
}

#define D(c,p,f) { (c), (p), (f) }
static const SolMirRuntimeLoweredDemandDescriptor image_instruction_descriptors[SOL_MIR_INST_FUNCTION_VALUE+1]={
[SOL_MIR_INST_CONST_INT64]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE),[SOL_MIR_INST_CONST_BOOL]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE),[SOL_MIR_INST_CONST_TEXT]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION),[SOL_MIR_INST_CONST_UNIT]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE),[SOL_MIR_INST_LOAD_COPY]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE),[SOL_MIR_INST_LOAD_MOVE]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE),[SOL_MIR_INST_LOAD_UPDATE]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE),[SOL_MIR_INST_STORE]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP),[SOL_MIR_INST_UNARY]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_ARITHMETIC,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP),[SOL_MIR_INST_BINARY]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_ARITHMETIC,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP|SOL_MIR_RUNTIME_LOWERED_FACILITY_EVENT),[SOL_MIR_INST_COMPOUND_UPDATE]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_ARITHMETIC,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP),[SOL_MIR_INST_PATTERN_TEST]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE),[SOL_MIR_INST_PATTERN_VALUE]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE),[SOL_MIR_INST_CONSTRUCT]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CONSTRUCT,SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE),[SOL_MIR_INST_CAPTURE_SNAPSHOT]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_SNAPSHOT,SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE),
[SOL_MIR_INST_PARAMETER_LIVE]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP),[SOL_MIR_INST_STORAGE_LIVE]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP),[SOL_MIR_INST_DROP_IF_INITIALIZED]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP,SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP|SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP),[SOL_MIR_INST_STORAGE_DEAD]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP),[SOL_MIR_INST_REGION_ENTER]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,0),[SOL_MIR_INST_REGION_EXIT]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP,SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP),[SOL_MIR_INST_TEMPORARY_INIT]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,0),[SOL_MIR_INST_TEMPORARY_DROP]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP,SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP|SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP),[SOL_MIR_INST_EXPRESSION_RESULT]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,0),[SOL_MIR_INST_MATCH_ARM]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,0),[SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP,SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP|SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP),[SOL_MIR_INST_HANDLER_ENTER]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER,SOL_MIR_RUNTIME_LOWERED_PLAN_HANDLER,SOL_MIR_RUNTIME_LOWERED_FACILITY_HANDLER_FRAME),[SOL_MIR_INST_HANDLER_EXIT]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER,SOL_MIR_RUNTIME_LOWERED_PLAN_HANDLER,SOL_MIR_RUNTIME_LOWERED_FACILITY_HANDLER_FRAME|SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP),[SOL_MIR_INST_SCOPE_ENTER]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,0),[SOL_MIR_INST_SCOPE_EXIT]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP,SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP),[SOL_MIR_INST_FUNCTION_VALUE]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CALLABLE,SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP)};
static const SolMirRuntimeLoweredDemandDescriptor image_terminator_descriptors[SOL_MIR_TERM_CONTRACT_VIOLATION+1]={
[SOL_MIR_TERM_GOTO]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,0),[SOL_MIR_TERM_BRANCH]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,0),[SOL_MIR_TERM_RETURN]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP,SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP),[SOL_MIR_TERM_PANIC]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP,SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP|SOL_MIR_RUNTIME_LOWERED_FACILITY_EVENT|SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE),[SOL_MIR_TERM_INVOKE]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CALLABLE,SOL_MIR_RUNTIME_LOWERED_FACILITY_CALL|SOL_MIR_RUNTIME_LOWERED_FACILITY_SIGNATURE|SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP|SOL_MIR_RUNTIME_LOWERED_FACILITY_EVENT|SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE),[SOL_MIR_TERM_RESUME_FAILURE]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP,SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP|SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE),[SOL_MIR_TERM_UNREACHABLE]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP,SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP|SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE),[SOL_MIR_TERM_BREAK]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,0),[SOL_MIR_TERM_CONTINUE]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,0),[SOL_MIR_TERM_CHECK_REFINED]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE,SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP|SOL_MIR_RUNTIME_LOWERED_FACILITY_EVENT|SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE),[SOL_MIR_TERM_MATCH_FAILURE]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN,SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP|SOL_MIR_RUNTIME_LOWERED_FACILITY_EVENT|SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE),[SOL_MIR_TERM_PROPAGATE]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PROPAGATION,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP),[SOL_MIR_TERM_CHECK_CONTRACT]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE,SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP|SOL_MIR_RUNTIME_LOWERED_FACILITY_EVENT|SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE),[SOL_MIR_TERM_CONTRACT_VIOLATION]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP,SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP|SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE)};
static const SolMirRuntimeLoweredDemandDescriptor predicate_instruction_descriptors[SOL_MIR_PREDICATE_INST_PATTERN_EXTRACT+1]={
[SOL_MIR_PREDICATE_INST_I64]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE),[SOL_MIR_PREDICATE_INST_BOOL]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE),[SOL_MIR_PREDICATE_INST_TEXT]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION),[SOL_MIR_PREDICATE_INST_UNIT]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE),[SOL_MIR_PREDICATE_INST_UNARY]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_ARITHMETIC,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP),[SOL_MIR_PREDICATE_INST_BINARY]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_ARITHMETIC,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP),[SOL_MIR_PREDICATE_INST_PROJECT]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE),[SOL_MIR_PREDICATE_INST_FUNCTION]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CALLABLE,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE),[SOL_MIR_PREDICATE_INST_BOUND_OPERATION]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CALLABLE,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE),[SOL_MIR_PREDICATE_INST_CONSTRUCT]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CONSTRUCT,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE|SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION),[SOL_MIR_PREDICATE_INST_PATTERN_TEST]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE),[SOL_MIR_PREDICATE_INST_PATTERN_EXTRACT]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)};
static const SolMirRuntimeLoweredDemandDescriptor predicate_terminator_descriptors[SOL_MIR_PREDICATE_TERM_FAILURE+1]={
[SOL_MIR_PREDICATE_TERM_RETURN]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE,SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP),[SOL_MIR_PREDICATE_TERM_JUMP]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,0),[SOL_MIR_PREDICATE_TERM_BRANCH]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,0),[SOL_MIR_PREDICATE_TERM_INVOKE]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CALLABLE,SOL_MIR_RUNTIME_LOWERED_FACILITY_CALL|SOL_MIR_RUNTIME_LOWERED_FACILITY_SIGNATURE|SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP|SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE),[SOL_MIR_PREDICATE_TERM_PROPAGATE]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PROPAGATION,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP),[SOL_MIR_PREDICATE_TERM_CHECK_REFINED]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE,SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP|SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE),[SOL_MIR_PREDICATE_TERM_FAILURE]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP,SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP|SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE)};
static const SolMirRuntimeLoweredDemandDescriptor provenance_descriptors[SOL_MIR_OPERATION_PROVENANCE_PREDICATE_BODY+1]={
[SOL_MIR_OPERATION_PROVENANCE_CONSTRUCT]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CONSTRUCT,SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE),[SOL_MIR_OPERATION_PROVENANCE_PATTERN_TEST]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE),[SOL_MIR_OPERATION_PROVENANCE_PATTERN_EXTRACTION]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE),[SOL_MIR_OPERATION_PROVENANCE_ARITHMETIC]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_ARITHMETIC,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP),[SOL_MIR_OPERATION_PROVENANCE_PROPAGATION]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PROPAGATION,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP),[SOL_MIR_OPERATION_PROVENANCE_SNAPSHOT]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_SNAPSHOT,SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE),[SOL_MIR_OPERATION_PROVENANCE_PREDICATE]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP),[SOL_MIR_OPERATION_PROVENANCE_HANDLER]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER,SOL_MIR_RUNTIME_LOWERED_PLAN_HANDLER,SOL_MIR_RUNTIME_LOWERED_FACILITY_HANDLER_FRAME),[SOL_MIR_OPERATION_PROVENANCE_CALLABLE]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CALLABLE,SOL_MIR_RUNTIME_LOWERED_FACILITY_CALL|SOL_MIR_RUNTIME_LOWERED_FACILITY_SIGNATURE|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE),[SOL_MIR_OPERATION_PROVENANCE_IMPORT_SNAPSHOT]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_IMPORT_SNAPSHOT,SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE),[SOL_MIR_OPERATION_PROVENANCE_PREDICATE_BODY]=D(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP)};
_Static_assert(sizeof image_instruction_descriptors / sizeof *image_instruction_descriptors == SOL_MIR_INST_FUNCTION_VALUE + 1, "image instruction descriptor coverage");
_Static_assert(sizeof image_terminator_descriptors / sizeof *image_terminator_descriptors == SOL_MIR_TERM_CONTRACT_VIOLATION + 1, "image terminator descriptor coverage");
_Static_assert(sizeof predicate_instruction_descriptors / sizeof *predicate_instruction_descriptors == SOL_MIR_PREDICATE_INST_PATTERN_EXTRACT + 1, "predicate instruction descriptor coverage");
_Static_assert(sizeof predicate_terminator_descriptors / sizeof *predicate_terminator_descriptors == SOL_MIR_PREDICATE_TERM_FAILURE + 1, "predicate terminator descriptor coverage");
_Static_assert(sizeof provenance_descriptors / sizeof *provenance_descriptors == SOL_MIR_OPERATION_PROVENANCE_PREDICATE_BODY + 1, "provenance descriptor coverage");
static bool descriptor(const SolMirRuntimeLoweredDemandDescriptor*t,size_t n,int k,SolMirRuntimeLoweredDemandDescriptor*out){if(k<0||(size_t)k>=n||t[k].plan_family==SOL_MIR_RUNTIME_LOWERED_PLAN_NONE)return false;if(out)*out=t[k];return true;}
#undef D
static bool image_instruction(SolMirInstructionKind k,SolMirRuntimeLoweredExecution*out){SolMirRuntimeLoweredDemandDescriptor d;if(!descriptor(image_instruction_descriptors,sizeof image_instruction_descriptors/sizeof *image_instruction_descriptors,(int)k,&d))return false;if(out)*out=d.runtime_class==SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE?SOL_MIR_RUNTIME_LOWERED_EXECUTABLE:SOL_MIR_RUNTIME_LOWERED_CONTROL_OR_MARKER;return true;}
static bool image_terminator(SolMirTerminatorKind k,SolMirRuntimeLoweredExecution*out){SolMirRuntimeLoweredDemandDescriptor d;if(!descriptor(image_terminator_descriptors,sizeof image_terminator_descriptors/sizeof *image_terminator_descriptors,(int)k,&d))return false;if(out)*out=d.runtime_class==SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE?SOL_MIR_RUNTIME_LOWERED_EXECUTABLE:SOL_MIR_RUNTIME_LOWERED_CONTROL_OR_MARKER;return true;}
static bool predicate_instruction(SolMirPredicateInstructionKind k){return descriptor(predicate_instruction_descriptors,sizeof predicate_instruction_descriptors/sizeof *predicate_instruction_descriptors,(int)k,NULL);}
static bool predicate_terminator(SolMirPredicateTerminatorKind k){return descriptor(predicate_terminator_descriptors,sizeof predicate_terminator_descriptors/sizeof *predicate_terminator_descriptors,(int)k,NULL);}
static bool provenance(SolMirOperationProvenanceKind k){return descriptor(provenance_descriptors,sizeof provenance_descriptors/sizeof *provenance_descriptors,(int)k,NULL);}
#ifdef SOL_MIR_PLAN_TEST_HOOKS
bool sol_mir_runtime_lowered_program_test_image_instruction(SolMirInstructionKind k,SolMirRuntimeLoweredExecution*x){return image_instruction(k,x);}bool sol_mir_runtime_lowered_program_test_image_instruction_descriptor(SolMirInstructionKind k,SolMirRuntimeLoweredDemandDescriptor*x){return descriptor(image_instruction_descriptors,sizeof image_instruction_descriptors/sizeof *image_instruction_descriptors,(int)k,x);}bool sol_mir_runtime_lowered_program_test_image_terminator(SolMirTerminatorKind k,SolMirRuntimeLoweredExecution*x){return image_terminator(k,x);}bool sol_mir_runtime_lowered_program_test_predicate_instruction(SolMirPredicateInstructionKind k){return predicate_instruction(k);}bool sol_mir_runtime_lowered_program_test_predicate_terminator(SolMirPredicateTerminatorKind k){return predicate_terminator(k);}bool sol_mir_runtime_lowered_program_test_provenance(SolMirOperationProvenanceKind k){return provenance(k);}
bool sol_mir_runtime_lowered_program_test_image_terminator_descriptor(SolMirTerminatorKind k,SolMirRuntimeLoweredDemandDescriptor*x){return descriptor(image_terminator_descriptors,sizeof image_terminator_descriptors/sizeof *image_terminator_descriptors,(int)k,x);}bool sol_mir_runtime_lowered_program_test_predicate_instruction_descriptor(SolMirPredicateInstructionKind k,SolMirRuntimeLoweredDemandDescriptor*x){return descriptor(predicate_instruction_descriptors,sizeof predicate_instruction_descriptors/sizeof *predicate_instruction_descriptors,(int)k,x);}bool sol_mir_runtime_lowered_program_test_predicate_terminator_descriptor(SolMirPredicateTerminatorKind k,SolMirRuntimeLoweredDemandDescriptor*x){return descriptor(predicate_terminator_descriptors,sizeof predicate_terminator_descriptors/sizeof *predicate_terminator_descriptors,(int)k,x);}bool sol_mir_runtime_lowered_program_test_provenance_descriptor(SolMirOperationProvenanceKind k,SolMirRuntimeLoweredDemandDescriptor*x){return descriptor(provenance_descriptors,sizeof provenance_descriptors/sizeof *provenance_descriptors,(int)k,x);}bool sol_mir_runtime_lowered_program_test_call_facilities(SolMirRuntimeCallTargetKind target,SolMirRuntimeSignatureOrigin origin,bool authenticated_import,uint32_t*out){if(target<SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL||target>SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE||origin<SOL_MIR_RUNTIME_SIGNATURE_INTERNAL||origin>SOL_MIR_RUNTIME_SIGNATURE_FUNCTION_RECIPE)return false;uint32_t facilities=SOL_MIR_RUNTIME_LOWERED_FACILITY_CALL|SOL_MIR_RUNTIME_LOWERED_FACILITY_SIGNATURE;if(target==SOL_MIR_RUNTIME_TARGET_DIRECT_HOST&&authenticated_import)facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_IMPORT;if(out)*out=facilities;return true;}uint32_t sol_mir_runtime_lowered_program_test_recipe_facilities(uint32_t demand){uint32_t facilities=0;if(demand)facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE;if(demand&SOL_MIR_LINKAGE_RUNTIME_CREATE)facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION;if(demand&SOL_MIR_LINKAGE_RUNTIME_COPY)facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY;if(demand&SOL_MIR_LINKAGE_RUNTIME_DROP)facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP;if(demand&SOL_MIR_LINKAGE_RUNTIME_EQUAL)facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_EQUALITY;return facilities;}uint32_t sol_mir_runtime_lowered_program_test_value_facilities(SolMirRuntimeAllocationPlanKind allocation,SolMirRuntimeCopyClass copy,SolMirRuntimeEqualityClass equality,SolMirRuntimeOwnershipClass ownership,SolMirRuntimeHostResultClass host_result,bool host_requirement){uint32_t facilities=0;if(allocation!=SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE)facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION;if(copy!=SOL_MIR_RUNTIME_COPY_UNREACHABLE&&copy!=SOL_MIR_RUNTIME_COPY_FORBIDDEN)facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY;if(equality!=SOL_MIR_RUNTIME_EQUALITY_UNREACHABLE&&equality!=SOL_MIR_RUNTIME_EQUALITY_FORBIDDEN)facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_EQUALITY;if(ownership!=SOL_MIR_RUNTIME_OWNERSHIP_UNREACHABLE)facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP;if((host_result!=SOL_MIR_RUNTIME_HOST_RESULT_UNREACHABLE&&host_result!=SOL_MIR_RUNTIME_HOST_RESULT_FORBIDDEN)||host_requirement)facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_HOST_RESULT;return facilities?facilities|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE:0;}bool sol_mir_runtime_lowered_program_test_copy_requires_runtime(SolMirCopyKind kind){return kind==SOL_MIR_COPY_TEXT||kind==SOL_MIR_COPY_AGGREGATE||kind==SOL_MIR_COPY_WRAPPER;}uint32_t sol_mir_runtime_lowered_program_test_arithmetic_facilities(unsigned failures){return SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP|(failures?SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE:0);}bool sol_mir_runtime_lowered_program_test_predicate_continuation_ordinal(SolMirPredicateTerminatorKind kind,bool failure,size_t*out){if(kind!=SOL_MIR_PREDICATE_TERM_INVOKE&&kind!=SOL_MIR_PREDICATE_TERM_PROPAGATE&&kind!=SOL_MIR_PREDICATE_TERM_CHECK_REFINED)return false;if(out)*out=failure?1:0;return true;}bool sol_mir_runtime_lowered_program_test_predicate_legacy_edge_rejected(SolMirPredicateTerminatorKind kind){return kind==SOL_MIR_PREDICATE_TERM_PROPAGATE;}
#endif
static size_t import_for_call(const SolMirRuntimeConventions*c,const SolMirRuntimeCall*q){if(q->target_kind!=SOL_MIR_RUNTIME_TARGET_DIRECT_HOST)return SOL_MIR_RUNTIME_NONE;for(size_t i=0;(i<c->import_count)&&BUILD_TICK();i++)if(c->imports[i].kind==SOL_MIR_RUNTIME_IMPORT_HOST&&c->imports[i].host==q->host)return i;return SOL_MIR_RUNTIME_NONE;}static size_t bound_environment_import_for_call(const SolMirRuntimeConventions*c,const SolMirRuntimeCall*q){if(q->target_kind!=SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE||q->signature>=c->signature_count)return SOL_MIR_RUNTIME_NONE;SolMirRecipeId recipe=c->signatures[q->signature].function_recipe;if(recipe==SOL_MIR_RECIPE_NONE)return SOL_MIR_RUNTIME_NONE;for(size_t i=0;(i<c->import_count)&&BUILD_TICK();i++)if(c->imports[i].kind==SOL_MIR_RUNTIME_IMPORT_RECIPE_BOUND_ENVIRONMENT&&c->imports[i].recipe==recipe)return i;return SOL_MIR_RUNTIME_NONE;}
static size_t entry_for_call(const SolMirRuntimeConventions*c,const SolMirRuntimeCall*q){if(q->target_kind!=SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL)return SOL_MIR_RUNTIME_NONE;for(size_t i=0;(i<c->entry_count)&&BUILD_TICK();i++)if(c->entries[i].callable==q->internal)return i;return SOL_MIR_RUNTIME_NONE;}static size_t image_call_for_block(const SolMirRuntimeConventions*c,size_t image,size_t block){for(size_t i=0;(i<c->call_count)&&BUILD_TICK();i++){const SolMirRuntimeCall*q=&c->calls[i];if(q->owner_kind==SOL_MIR_RUNTIME_CALL_OWNER_IMAGE&&q->image==image&&q->block==block)return i;}return SOL_MIR_RUNTIME_NONE;}
static size_t predicate_call_for_block(const SolMirRuntimeConventions*c,size_t body,size_t block){for(size_t i=0;(i<c->call_count)&&BUILD_TICK();i++){const SolMirRuntimeCall*q=&c->calls[i];if(q->owner_kind==SOL_MIR_RUNTIME_CALL_OWNER_PREDICATE&&q->predicate==body&&q->block==block)return i;}return SOL_MIR_RUNTIME_NONE;}
static bool predicate_edge_source(const SolMirOperations*o,size_t edge,size_t*source,size_t*ordinal){for(size_t block=0;(block<o->predicate_block_count)&&BUILD_TICK();block++){const SolMirPredicateTerminator*t=&o->predicate_blocks[block].terminator;size_t candidates[2]={SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE};size_t count=0;switch(t->kind){case SOL_MIR_PREDICATE_TERM_JUMP:candidates[count++]=t->edge;break;case SOL_MIR_PREDICATE_TERM_BRANCH:candidates[count++]=t->true_edge;candidates[count++]=t->false_edge;break;case SOL_MIR_PREDICATE_TERM_INVOKE:case SOL_MIR_PREDICATE_TERM_PROPAGATE:case SOL_MIR_PREDICATE_TERM_CHECK_REFINED:candidates[count++]=t->normal_edge;candidates[count++]=t->failure_edge;break;default:break;}for(size_t i=0;(i<count)&&BUILD_TICK();i++)if(candidates[i]==edge){*source=block;*ordinal=i;return true;}}return false;}
static bool edge_source(const SolMirMaterialization*m,size_t edge,size_t*source,size_t*ordinal){for(size_t block=0;(block<m->block_count)&&BUILD_TICK();block++){const SolMirMaterializedTerminator*t=&m->blocks[block].terminator;size_t candidates[3]={SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE};size_t count=0;switch(t->kind){case SOL_MIR_TERM_GOTO:case SOL_MIR_TERM_BREAK:case SOL_MIR_TERM_CONTINUE:candidates[count++]=t->edge;break;case SOL_MIR_TERM_BRANCH:candidates[count++]=t->true_edge;candidates[count++]=t->false_edge;break;case SOL_MIR_TERM_INVOKE:candidates[count++]=t->normal_edge;candidates[count++]=t->failure_edge;break;case SOL_MIR_TERM_CHECK_REFINED:candidates[count++]=t->normal_edge;candidates[count++]=t->failure_edge;break;case SOL_MIR_TERM_PROPAGATE:candidates[count++]=t->value_edge;candidates[count++]=t->residual_edge;break;case SOL_MIR_TERM_CHECK_CONTRACT:candidates[count++]=t->satisfied_edge;candidates[count++]=t->violation_edge;candidates[count++]=t->failure_edge;break;default:break;}for(size_t i=0;(i<count)&&BUILD_TICK();i++)if(candidates[i]==edge){*source=block;*ordinal=i;return true;}}return false;}
static size_t image_for_block(const SolMirMaterialization*m,size_t id){for(size_t i=0;(i<m->image_count)&&BUILD_TICK();i++){SolMirPlanSlice s=m->images[i].blocks;if(id>=s.offset&&id-s.offset<s.count)return i;}return SOL_MIR_RUNTIME_NONE;}static size_t image_for_instruction(const SolMirMaterialization*m,size_t id){for(size_t i=0;(i<m->image_count)&&BUILD_TICK();i++){SolMirPlanSlice s=m->images[i].instructions;if(id>=s.offset&&id-s.offset<s.count)return i;}return SOL_MIR_RUNTIME_NONE;}
static uint32_t recipe_facilities(uint32_t demand){uint32_t facilities=0;if(demand)facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE;if(demand&SOL_MIR_LINKAGE_RUNTIME_CREATE)facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION;if(demand&SOL_MIR_LINKAGE_RUNTIME_COPY)facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY;if(demand&SOL_MIR_LINKAGE_RUNTIME_DROP)facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP;if(demand&SOL_MIR_LINKAGE_RUNTIME_EQUAL)facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_EQUALITY;return facilities;}
static uint32_t value_facilities(const SolMirRuntimeValues*v,size_t recipe){uint32_t facilities=0;if(v->allocation_plans[recipe].kind!=SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE)facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION;if(v->copy_plans[recipe].classification!=SOL_MIR_RUNTIME_COPY_UNREACHABLE&&v->copy_plans[recipe].classification!=SOL_MIR_RUNTIME_COPY_FORBIDDEN)facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY;if(v->equality_plans[recipe].classification!=SOL_MIR_RUNTIME_EQUALITY_UNREACHABLE&&v->equality_plans[recipe].classification!=SOL_MIR_RUNTIME_EQUALITY_FORBIDDEN)facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_EQUALITY;if(v->ownership_plans[recipe].classification!=SOL_MIR_RUNTIME_OWNERSHIP_UNREACHABLE)facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP;if(v->host_result_plans[recipe].classification!=SOL_MIR_RUNTIME_HOST_RESULT_UNREACHABLE&&v->host_result_plans[recipe].classification!=SOL_MIR_RUNTIME_HOST_RESULT_FORBIDDEN)facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_HOST_RESULT;for(size_t i=0;(i<v->host_result_requirement_count)&&BUILD_TICK();i++)if(v->host_result_requirements[i].result==recipe)facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_HOST_RESULT;return facilities?facilities|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE:0;}
static bool copy_requires_runtime(SolMirCopyKind kind){return kind==SOL_MIR_COPY_TEXT||kind==SOL_MIR_COPY_AGGREGATE||kind==SOL_MIR_COPY_WRAPPER;}
static uint32_t value_demand(const SolMirRuntimeConventions*c,const SolMirRuntimeValues*v,const SolMirRuntimeCleanup*cleanup,const SolMirOperations*o,size_t recipe){uint32_t demand=0;for(size_t i=0;(i<c->import_count)&&BUILD_TICK();i++)if(c->imports[i].recipe==recipe){switch(c->imports[i].kind){case SOL_MIR_RUNTIME_IMPORT_RECIPE_CREATE:demand|=SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION;break;case SOL_MIR_RUNTIME_IMPORT_RECIPE_COPY:demand|=SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY;break;case SOL_MIR_RUNTIME_IMPORT_RECIPE_DROP:demand|=SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP;break;case SOL_MIR_RUNTIME_IMPORT_RECIPE_EQUAL:demand|=SOL_MIR_RUNTIME_LOWERED_FACILITY_EQUALITY;break;default:break;}}for(size_t i=0;(i<o->constructor_count)&&BUILD_TICK();i++)if(o->constructors[i].result_recipe==recipe)demand|=SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION|SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP;for(size_t i=0;(i<o->pattern_extraction_count)&&BUILD_TICK();i++)if(o->pattern_extractions[i].result_recipe==recipe&&copy_requires_runtime(o->pattern_extractions[i].copy_kind))demand|=SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY;for(size_t i=0;(i<o->snapshot_count)&&BUILD_TICK();i++)if(o->snapshots[i].recipe==recipe&&copy_requires_runtime(o->snapshots[i].copy_kind))demand|=SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY;for(size_t i=0;(i<o->arithmetic_count)&&BUILD_TICK();i++)if((o->arithmetic[i].operand_recipe==recipe||o->arithmetic[i].result_recipe==recipe)&&o->arithmetic[i].equality.count)demand|=SOL_MIR_RUNTIME_LOWERED_FACILITY_EQUALITY;for(size_t i=0;(i<cleanup->action_count)&&BUILD_TICK();i++)if(cleanup->actions[i].recipe==recipe&&(cleanup->actions[i].kind==SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_TEMPORARY||cleanup->actions[i].kind==SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE||cleanup->actions[i].kind==SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT||cleanup->actions[i].kind==SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER))demand|=SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP;for(size_t i=0;(i<v->host_result_requirement_count)&&BUILD_TICK();i++)if(v->host_result_requirements[i].result==recipe)demand|=SOL_MIR_RUNTIME_LOWERED_FACILITY_HOST_RESULT;return demand?demand|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE:0;}

static size_t cleanup_event_for(const SolMirRuntimeCleanup *, SolMirRuntimeCleanupEventKind, size_t, size_t, size_t, bool, SolMirRuntimeCleanupProducerKind);
typedef struct { SolMirRuntimeLoweredRecipeDemand *rows; size_t count; } DemandWriter;
static bool value_plan_for_recipe(const SolMirRuntimeValues *values, SolMirRecipeId recipe,
    size_t *result) {
    for(size_t i = 0; (i < values->recipe_operation_count) && BUILD_TICK(); ++i)
        if (values->recipe_operations[i].recipe == recipe
            && values->allocation_plans[i].recipe == recipe
            && values->copy_plans[i].recipe == recipe
            && values->equality_plans[i].recipe == recipe
            && values->ownership_plans[i].recipe == recipe
            && values->host_result_plans[i].recipe == recipe) {
            *result = i; return true;
        }
    return false;
}
static bool demand_add(DemandWriter *writer, const SolMirRuntimeValues *values,
    SolMirRecipeId recipe, uint32_t wanted) {
    if (recipe == SOL_MIR_RECIPE_NONE) return true;
    size_t value_plan;
    if (!value_plan_for_recipe(values, recipe, &value_plan)) return false;
    if (!BUILD_TICK()) return false;
    if (writer->rows) writer->rows[writer->count]
        = (SolMirRuntimeLoweredRecipeDemand){SOL_MIR_RUNTIME_LOWERED_PRESENT,
            recipe, value_plan, value_facilities(values, value_plan) & wanted,
            writer->count};
    ++writer->count;
    return true;
}
static bool signature_demands(DemandWriter *writer,
    const SolMirRuntimeConventions *conventions, const SolMirRuntimeValues *values,
    size_t signature) {
    if (signature >= conventions->signature_count) return false;
    const SolMirRuntimeSignature *input = &conventions->signatures[signature];
    if (!demand_add(writer, values, input->function_recipe,
            SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)
        || input->slots.offset > conventions->signature_slot_count
        || input->slots.count > conventions->signature_slot_count - input->slots.offset)
        return false;
    for(size_t i = 0; (i < input->slots.count) && BUILD_TICK(); ++i)
        if (!demand_add(writer, values,
                conventions->signature_slots[input->slots.offset + i].recipe,
                SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)) return false;
    return demand_add(writer, values, input->result,
        SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE
        | (input->origin == SOL_MIR_RUNTIME_SIGNATURE_HOST
            ? SOL_MIR_RUNTIME_LOWERED_FACILITY_HOST_RESULT : 0));
}
static bool image_instruction_demands(DemandWriter *writer,
    const SolMirMaterialization *materialization, const SolMirOperations *operations,
    const SolMirRuntimeValues *values, const SolMirRuntimeCleanup *cleanup, size_t instruction) {
    const SolMirMaterializedInstruction *input = &materialization->instructions[instruction];
    SolMirRuntimeLoweredDemandDescriptor descriptor_value;
    if (!descriptor(image_instruction_descriptors,
            sizeof image_instruction_descriptors / sizeof *image_instruction_descriptors,
            (int)input->kind, &descriptor_value)) return false;
    if (descriptor_value.runtime_class != SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE) {
        switch (input->kind) {
        case SOL_MIR_INST_TEMPORARY_DROP: case SOL_MIR_INST_DROP_IF_INITIALIZED:
        case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED:
            break;
        default: return true;
        }
        size_t event = cleanup_event_for(cleanup,
            SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION,
            image_for_instruction(materialization, instruction), input->block, instruction,
            true, SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL);
        if (event == SOL_MIR_RUNTIME_LOWERED_NONE) return true;
        const SolMirRuntimeCleanupEvent *source = &cleanup->events[event];
        if (source->actions.offset > cleanup->action_count
            || source->actions.count > cleanup->action_count - source->actions.offset) return false;
        for(size_t i = 0; (i < source->actions.count) && BUILD_TICK(); ++i)
            if (!demand_add(writer, values,
                    cleanup->actions[source->actions.offset + i].recipe,
                    SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP)) return false;
        return true;
    }
    uint32_t wanted = descriptor_value.facilities &
        (SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE | SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION
        | SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY | SOL_MIR_RUNTIME_LOWERED_FACILITY_EQUALITY
        | SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP | SOL_MIR_RUNTIME_LOWERED_FACILITY_HOST_RESULT);
    if (input->kind == SOL_MIR_INST_LOAD_COPY && !copy_requires_runtime(operations->layout->representation->recipes[input->type].copy_kind)) wanted &= ~(uint32_t)SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY;
    if (!demand_add(writer, values, input->type, wanted)) return false;
    for(size_t i = 0; (i < operations->constructor_count) && BUILD_TICK(); ++i) {
        const SolMirOperationConstructPlan *plan = &operations->constructors[i];
        if (plan->instruction != instruction) continue;
        if (!demand_add(writer, values, plan->result_recipe,
                SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE | SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION
                | SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP)) return false;
        for(size_t q = 0; (q < plan->operands.count) && BUILD_TICK(); ++q)
            if (!demand_add(writer, values,
                    operations->construct_operands[plan->operands.offset + q].recipe,
                    SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)) return false;
    }
    for(size_t i = 0; (i < operations->pattern_extraction_count) && BUILD_TICK(); ++i) {
        const SolMirOperationPatternExtraction *plan = &operations->pattern_extractions[i];
        if (plan->instruction == instruction
            && (!demand_add(writer, values, plan->scrutinee_recipe,
                    SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)
                || !demand_add(writer, values, plan->result_recipe,
                    SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE
                    | (copy_requires_runtime(plan->copy_kind)
                        ? SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY : 0)))) return false;
    }
    for(size_t i = 0; (i < operations->arithmetic_count) && BUILD_TICK(); ++i) {
        const SolMirOperationArithmeticPlan *plan = &operations->arithmetic[i];
        if (plan->instruction == instruction
            && (!demand_add(writer, values, plan->operand_recipe,
                    SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE
                    | (plan->equality.count ? SOL_MIR_RUNTIME_LOWERED_FACILITY_EQUALITY : 0))
                || !demand_add(writer, values, plan->result_recipe,
                    SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE))) return false;
    }
    for(size_t i = 0; (i < operations->snapshot_count) && BUILD_TICK(); ++i) {
        const SolMirOperationSnapshotPlan *plan = &operations->snapshots[i];
        if (plan->instruction == instruction
            && (!demand_add(writer, values, plan->root_recipe,
                    SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)
                || !demand_add(writer, values, plan->recipe,
                    SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE
                    | (copy_requires_runtime(plan->copy_kind)
                        ? SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY : 0)))) return false;
    }
    return true;
}
static bool image_terminator_demands(DemandWriter *writer,
    const SolMirRuntimeConventions *conventions, const SolMirRuntimeValues *values,
    const SolMirMaterialization *materialization, const SolMirOperations *operations,
    size_t image, size_t block) {
    const SolMirMaterializedTerminator *term = &materialization->blocks[block].terminator;
    SolMirRuntimeLoweredDemandDescriptor descriptor_value;
    if (!descriptor(image_terminator_descriptors,
            sizeof image_terminator_descriptors / sizeof *image_terminator_descriptors,
            (int)term->kind, &descriptor_value)) return false;
    if (descriptor_value.runtime_class != SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE) return true;
    if (term->kind == SOL_MIR_TERM_INVOKE) {
        size_t call = image_call_for_block(conventions, image, block);
        if (call == SOL_MIR_RUNTIME_NONE || !signature_demands(writer, conventions,
                values, conventions->calls[call].signature)) return false;
    }
    for(size_t i = 0; (i < operations->propagation_count) && BUILD_TICK(); ++i) {
        const SolMirOperationPropagationPlan *plan = &operations->propagations[i];
        if (plan->block == block
            && (!demand_add(writer, values, plan->source_recipe, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)
                || !demand_add(writer, values, plan->success_recipe, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)
                || !demand_add(writer, values, plan->residual_recipe, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE))) return false;
    }
    for(size_t i = 0; (i < operations->predicate_count) && BUILD_TICK(); ++i) {
        const SolMirOperationPredicatePlan *plan = &operations->predicates[i];
        if (plan->block == block
            && (!demand_add(writer, values, plan->input_recipe, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)
                || !demand_add(writer, values, plan->result_recipe, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)
                || !demand_add(writer, values, plan->output_recipe, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE))) return false;
    }
    return true;
}
static bool predicate_instruction_demands(DemandWriter *writer,
    const SolMirOperations *operations, const SolMirRuntimeValues *values, size_t instruction) {
    const SolMirPredicateInstruction *input = &operations->predicate_instructions[instruction];
    SolMirRuntimeLoweredDemandDescriptor descriptor_value;
    if (!descriptor(predicate_instruction_descriptors,
            sizeof predicate_instruction_descriptors / sizeof *predicate_instruction_descriptors,
            (int)input->kind, &descriptor_value)) return false;
    if (descriptor_value.runtime_class != SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE) return true;
    uint32_t wanted = descriptor_value.facilities &
        (SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE | SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION
        | SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY | SOL_MIR_RUNTIME_LOWERED_FACILITY_EQUALITY
        | SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP);
    if (input->kind == SOL_MIR_PREDICATE_INST_CONSTRUCT
        && input->recipe < values->allocation_plan_count
        && values->allocation_plans[input->recipe].kind != SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE)
        wanted |= SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION;
    if (input->kind == SOL_MIR_PREDICATE_INST_PATTERN_EXTRACT
        && copy_requires_runtime(operations->layout->representation->recipes[input->recipe].copy_kind))
        wanted |= SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY;
    if (!demand_add(writer, values, input->recipe, wanted)) return false;
    if ((input->kind == SOL_MIR_PREDICATE_INST_BINARY || input->kind == SOL_MIR_PREDICATE_INST_UNARY)
        && !demand_add(writer, values, operations->predicate_values[input->left].recipe,
            SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE
            | ((input->opcode == SOL_MIR_OPERATION_VALUE_EQ || input->opcode == SOL_MIR_OPERATION_VALUE_NE)
                ? SOL_MIR_RUNTIME_LOWERED_FACILITY_EQUALITY : 0))) return false;
    if (input->kind == SOL_MIR_PREDICATE_INST_BINARY
        && !demand_add(writer, values, operations->predicate_values[input->right].recipe,
            SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)) return false;
    for(size_t i = 0; (i < input->operands.count) && BUILD_TICK(); ++i)
        if (!demand_add(writer, values,
                operations->predicate_values[operations->predicate_operands[input->operands.offset + i].value].recipe,
                SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)) return false;
    return true;
}
static bool predicate_terminator_demands(DemandWriter *writer,
    const SolMirRuntimeConventions *conventions, const SolMirRuntimeValues *values,
    const SolMirOperations *operations, size_t body, size_t block) {
    const SolMirPredicateTerminator *term = &operations->predicate_blocks[block].terminator;
    SolMirRuntimeLoweredDemandDescriptor descriptor_value;
    if (!descriptor(predicate_terminator_descriptors,
            sizeof predicate_terminator_descriptors / sizeof *predicate_terminator_descriptors,
            (int)term->kind, &descriptor_value)) return false;
    if (descriptor_value.runtime_class != SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE) return true;
    if (term->kind == SOL_MIR_PREDICATE_TERM_INVOKE) {
        size_t call = predicate_call_for_block(conventions, body, block);
        if (call == SOL_MIR_RUNTIME_NONE || !signature_demands(writer, conventions,
                values, conventions->calls[call].signature)) return false;
    }
    if (!demand_add(writer, values, term->result_recipe,
            SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)) return false;
    if (term->value < operations->predicate_value_count
        && !demand_add(writer, values, operations->predicate_values[term->value].recipe,
            SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)) return false;
    return true;
}
static bool count_demands(DemandWriter *writer, const SolMirRuntimeConventions *conventions,
    const SolMirRuntimeValues *values, const SolMirRuntimeCleanup *cleanup,
    const SolMirMaterialization *materialization, const SolMirOperations *operations) {
    for(size_t i = 0; (i < materialization->instruction_count) && BUILD_TICK(); ++i)
        if (!image_instruction_demands(writer, materialization, operations, values, cleanup, i)) return false;
    for(size_t i = 0; (i < materialization->block_count) && BUILD_TICK(); ++i)
        if (!image_terminator_demands(writer, conventions, values, materialization,
                operations, image_for_block(materialization, i), i)) return false;
    for(size_t i = 0; (i < operations->predicate_instruction_count) && BUILD_TICK(); ++i)
        if (!predicate_instruction_demands(writer, operations, values, i)) return false;
    for(size_t i = 0; (i < operations->predicate_block_count) && BUILD_TICK(); ++i)
        if (!predicate_terminator_demands(writer, conventions, values, operations,
                operations->predicate_blocks[i].body, i)) return false;
    return true;
}

static size_t semantic_offset(const SolMirOperations *operations,
    SolMirOperationProvenanceKind kind) {
    size_t offset = operations->access_plan_count;
    switch (kind) {
    case SOL_MIR_OPERATION_PROVENANCE_CONSTRUCT: return offset;
    case SOL_MIR_OPERATION_PROVENANCE_PATTERN_TEST: return offset + operations->constructor_count;
    case SOL_MIR_OPERATION_PROVENANCE_PATTERN_EXTRACTION: return offset + operations->constructor_count + operations->pattern_test_count;
    case SOL_MIR_OPERATION_PROVENANCE_PROPAGATION: return offset + operations->constructor_count + operations->pattern_test_count + operations->pattern_extraction_count;
    case SOL_MIR_OPERATION_PROVENANCE_ARITHMETIC: return offset + operations->constructor_count + operations->pattern_test_count + operations->pattern_extraction_count + operations->propagation_count;
    case SOL_MIR_OPERATION_PROVENANCE_SNAPSHOT: return offset + operations->constructor_count + operations->pattern_test_count + operations->pattern_extraction_count + operations->propagation_count + operations->arithmetic_count;
    case SOL_MIR_OPERATION_PROVENANCE_CALLABLE: return offset + operations->constructor_count + operations->pattern_test_count + operations->pattern_extraction_count + operations->propagation_count + operations->arithmetic_count + operations->snapshot_count;
    case SOL_MIR_OPERATION_PROVENANCE_HANDLER: return offset + operations->constructor_count + operations->pattern_test_count + operations->pattern_extraction_count + operations->propagation_count + operations->arithmetic_count + operations->snapshot_count + operations->callable_count;
    case SOL_MIR_OPERATION_PROVENANCE_PREDICATE: return offset + operations->constructor_count + operations->pattern_test_count + operations->pattern_extraction_count + operations->propagation_count + operations->arithmetic_count + operations->snapshot_count + operations->callable_count + operations->handler_count;
    case SOL_MIR_OPERATION_PROVENANCE_IMPORT_SNAPSHOT: return offset + operations->constructor_count + operations->pattern_test_count + operations->pattern_extraction_count + operations->propagation_count + operations->arithmetic_count + operations->snapshot_count + operations->callable_count + operations->handler_count + operations->predicate_count;
    case SOL_MIR_OPERATION_PROVENANCE_PREDICATE_BODY: return SOL_MIR_RUNTIME_LOWERED_NONE;
    }
    return SOL_MIR_RUNTIME_LOWERED_NONE;
}
static bool semantic_for_provenance(const SolMirOperations *operations,
    SolMirOperationProvenanceKind kind, size_t executable, size_t *result) {
    size_t matches = 0;
    for(size_t i = 0; (i < operations->provenance_count) && BUILD_TICK(); ++i)
        if (operations->provenance[i].kind == kind
            && operations->provenance[i].executable == executable) ++matches;
    size_t offset = semantic_offset(operations, kind);
    if (matches != 1 || offset == SOL_MIR_RUNTIME_LOWERED_NONE) return false;
    *result = offset + executable;
    return true;
}
static size_t callable_semantic_for_binding(const SolMirMaterialization *materialization,
    const SolMirOperations *operations, size_t binding) {
    size_t result = SOL_MIR_RUNTIME_LOWERED_NONE;
    for(size_t i = 0; (i < operations->callable_count) && BUILD_TICK(); ++i) {
        const SolMirOperationCallablePlan *plan = &operations->callables[i];
        if (plan->semantic_site >= materialization->semantic_site_count
            || materialization->semantic_sites[plan->semantic_site].binding != binding) continue;
        size_t semantic;
        if (!semantic_for_provenance(operations,
                SOL_MIR_OPERATION_PROVENANCE_CALLABLE, i, &semantic)
            || result != SOL_MIR_RUNTIME_LOWERED_NONE) return SOL_MIR_RUNTIME_LOWERED_NONE;
        result = semantic;
    }
    return result;
}
static size_t image_instruction_semantic(const SolMirMaterialization *materialization,
    const SolMirOperations *operations, size_t instruction) {
    size_t result = SOL_MIR_RUNTIME_LOWERED_NONE, semantic;
#define MATCH(member, count, field, provenance_kind) \
    for(size_t i = 0; (i < operations->count) && BUILD_TICK(); ++i) if (operations->member[i].field == instruction) { \
        if (!semantic_for_provenance(operations, provenance_kind, i, &semantic) || result != SOL_MIR_RUNTIME_LOWERED_NONE) return SOL_MIR_RUNTIME_LOWERED_NONE; result = semantic; }
    MATCH(constructors, constructor_count, instruction, SOL_MIR_OPERATION_PROVENANCE_CONSTRUCT)
    MATCH(pattern_tests, pattern_test_count, instruction, SOL_MIR_OPERATION_PROVENANCE_PATTERN_TEST)
    MATCH(pattern_extractions, pattern_extraction_count, instruction, SOL_MIR_OPERATION_PROVENANCE_PATTERN_EXTRACTION)
    MATCH(arithmetic, arithmetic_count, instruction, SOL_MIR_OPERATION_PROVENANCE_ARITHMETIC)
    MATCH(snapshots, snapshot_count, instruction, SOL_MIR_OPERATION_PROVENANCE_SNAPSHOT)
#undef MATCH
    if (instruction < materialization->instruction_count
        && materialization->instructions[instruction].kind == SOL_MIR_INST_FUNCTION_VALUE)
        for (size_t i = 0; i < operations->callable_count && BUILD_TICK(); ++i) {
            size_t site = operations->callables[i].semantic_site;
            if (site >= materialization->semantic_site_count
                || materialization->semantic_sites[site].instruction != instruction) continue;
            if (!semantic_for_provenance(operations, SOL_MIR_OPERATION_PROVENANCE_CALLABLE,
                    i, &semantic) || result != SOL_MIR_RUNTIME_LOWERED_NONE)
                return SOL_MIR_RUNTIME_LOWERED_NONE;
            result = semantic;
        }
    return result;
}
static size_t image_terminator_semantic(const SolMirMaterialization *materialization,
    const SolMirOperations *operations, size_t block) {
    const SolMirMaterializedTerminator *term = &materialization->blocks[block].terminator;
    size_t result = SOL_MIR_RUNTIME_LOWERED_NONE, semantic;
    for(size_t i = 0; (i < operations->propagation_count) && BUILD_TICK(); ++i)
        if (operations->propagations[i].block == block) {
            if (!semantic_for_provenance(operations,
                    SOL_MIR_OPERATION_PROVENANCE_PROPAGATION, i, &semantic)
                || result != SOL_MIR_RUNTIME_LOWERED_NONE) return SOL_MIR_RUNTIME_LOWERED_NONE;
            result = semantic;
        }
    for(size_t i = 0; (i < operations->predicate_count) && BUILD_TICK(); ++i)
        if (operations->predicates[i].block == block) {
            if (!semantic_for_provenance(operations,
                    SOL_MIR_OPERATION_PROVENANCE_PREDICATE, i, &semantic)
                || result != SOL_MIR_RUNTIME_LOWERED_NONE) return SOL_MIR_RUNTIME_LOWERED_NONE;
            result = semantic;
        }
    if (term->kind == SOL_MIR_TERM_INVOKE && term->callable_site != SOL_MIR_MATERIALIZED_NONE) {
        size_t callable = callable_semantic_for_binding(materialization, operations,
            materialization->semantic_sites[term->callable_site].binding);
        if (callable == SOL_MIR_RUNTIME_LOWERED_NONE || result != SOL_MIR_RUNTIME_LOWERED_NONE)
            return SOL_MIR_RUNTIME_LOWERED_NONE;
        result = callable;
    }
    return result;
}
static size_t predicate_instruction_semantic(const SolMirMaterialization *materialization,
    const SolMirOperations *operations, size_t instruction) {
    const SolMirPredicateInstruction *input = &operations->predicate_instructions[instruction];
    if (input->kind != SOL_MIR_PREDICATE_INST_FUNCTION
        && input->kind != SOL_MIR_PREDICATE_INST_BOUND_OPERATION)
        return SOL_MIR_RUNTIME_LOWERED_NONE;
    return callable_semantic_for_binding(materialization, operations, input->binding);
}
static size_t predicate_terminator_semantic(const SolMirMaterialization *materialization,
    const SolMirOperations *operations, size_t block) {
    const SolMirPredicateTerminator *term = &operations->predicate_blocks[block].terminator;
    if (term->kind != SOL_MIR_PREDICATE_TERM_INVOKE) return SOL_MIR_RUNTIME_LOWERED_NONE;
    size_t semantic = callable_semantic_for_binding(materialization, operations,
        term->binding);
    if (semantic != SOL_MIR_RUNTIME_LOWERED_NONE) return semantic;
    if (term->callee >= operations->predicate_value_count) return SOL_MIR_RUNTIME_LOWERED_NONE;
    const SolMirPredicateValue *callee = &operations->predicate_values[term->callee];
    return callee->kind == SOL_MIR_PREDICATE_VALUE_INSTRUCTION
        && callee->definition < operations->predicate_instruction_count
        ? callable_semantic_for_binding(materialization, operations,
            operations->predicate_instructions[callee->definition].binding)
        : SOL_MIR_RUNTIME_LOWERED_NONE;
}
static size_t cleanup_event_for(const SolMirRuntimeCleanup *cleanup,
    SolMirRuntimeCleanupEventKind kind, size_t owner, size_t block, size_t operation,
    bool require_producer, SolMirRuntimeCleanupProducerKind producer) {
    size_t result = SOL_MIR_RUNTIME_LOWERED_NONE;
    for(size_t i = 0; (i < cleanup->event_count) && BUILD_TICK(); ++i) {
        const SolMirRuntimeCleanupEvent *event = &cleanup->events[i];
        if (event->kind != kind || event->phase != SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
            || event->owner != owner || event->block != block || event->operation != operation
            || event->semantic_site != SOL_MIR_RUNTIME_NONE
            || (require_producer && event->producer != producer)) continue;
        if (result != SOL_MIR_RUNTIME_LOWERED_NONE) return SOL_MIR_RUNTIME_LOWERED_NONE;
        result = i;
    }
    return result;
}
static size_t cleanup_pre_operation_event_for(const SolMirRuntimeCleanup *cleanup,
    SolMirRuntimeCleanupPhase phase, size_t owner, size_t block, size_t operation,
    size_t semantic_site, SolMirRuntimeCleanupProducerKind producer) {
    size_t result = SOL_MIR_RUNTIME_LOWERED_NONE;
    for (size_t i = 0; (i < cleanup->event_count) && BUILD_TICK(); ++i) {
        const SolMirRuntimeCleanupEvent *event = &cleanup->events[i];
        if (event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
            || event->phase != phase || event->owner != owner || event->block != block
            || event->operation != operation || event->semantic_site != semantic_site
            || event->producer != producer) continue;
        if (result != SOL_MIR_RUNTIME_LOWERED_NONE) return SOL_MIR_RUNTIME_LOWERED_NONE;
        result = i;
    }
    return result;
}
static size_t cleanup_failure_for_event(const SolMirRuntimeCleanup *cleanup, size_t event) {
    return event == SOL_MIR_RUNTIME_LOWERED_NONE ? SOL_MIR_RUNTIME_LOWERED_NONE
        : cleanup->events[event].inherited_failure_site;
}
static size_t handler_frame_for_cleanup_action(const SolMirRuntimeHandlerAbi *handlers,
    SolMirRuntimeCleanupActionId action) {
    size_t result = SOL_MIR_RUNTIME_LOWERED_NONE;
    for(size_t i = 0; (i < handlers->cleanup_exit_count) && BUILD_TICK(); ++i) {
        const SolMirRuntimeHandlerCleanupExit *exit = &handlers->cleanup_exits[i];
        if (exit->action != action) continue;
        if (result != SOL_MIR_RUNTIME_LOWERED_NONE) return SOL_MIR_RUNTIME_LOWERED_NONE;
        result = exit->frame;
    }
    return result;
}
static bool image_instruction_event_producer(const SolMirMaterialization *materialization,
    const SolMirOperations *operations, const SolMirRuntimeValues *values,
    size_t instruction, SolMirRuntimeCleanupProducerKind *producer) {
    const SolMirMaterializedInstruction *input = &materialization->instructions[instruction];
    for(size_t i = 0; (i < operations->arithmetic_count) && BUILD_TICK(); ++i)
        if (operations->arithmetic[i].instruction == instruction
            && operations->arithmetic[i].failures) {
            *producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_ARITHMETIC; return true;
        }
    SolMirRecipeId recipe = SOL_MIR_RECIPE_NONE;
    if (input->kind == SOL_MIR_INST_CONST_TEXT || input->kind == SOL_MIR_INST_CONSTRUCT
        || input->kind == SOL_MIR_INST_FUNCTION_VALUE)
        recipe = input->type;
    else if (input->kind == SOL_MIR_INST_LOAD_COPY && input->place < materialization->place_count)
        recipe = materialization->places[input->place].final_type;
    if (input->kind == SOL_MIR_INST_FUNCTION_VALUE
        || (recipe < values->allocation_plan_count
        && values->allocation_plans[recipe].kind != SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE)) {
        *producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_SUPPLEMENTAL_ALLOCATION; return true; }
    if (input->kind == SOL_MIR_INST_PATTERN_VALUE) for (size_t i = 0;
            i < operations->pattern_extraction_count; ++i) {
        const SolMirOperationPatternExtraction *plan = &operations->pattern_extractions[i];
        if (plan->instruction == instruction && copy_requires_runtime(plan->copy_kind)
            && plan->result_recipe < values->allocation_plan_count
            && values->allocation_plans[plan->result_recipe].kind
                != SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE) {
            *producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_SUPPLEMENTAL_ALLOCATION;
            return true;
        }
    }
    switch (input->kind) {
    case SOL_MIR_INST_TEMPORARY_DROP: case SOL_MIR_INST_DROP_IF_INITIALIZED:
    case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED: case SOL_MIR_INST_SCOPE_EXIT:
    case SOL_MIR_INST_REGION_EXIT:
        *producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL; return true;
    default: return false;
    }
}
static SolMirRuntimeCleanupProducerKind image_terminator_event_producer(
    SolMirTerminatorKind kind) {
    switch (kind) {
    case SOL_MIR_TERM_INVOKE: return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_INVOKE;
    case SOL_MIR_TERM_PANIC: return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_PANIC;
    case SOL_MIR_TERM_MATCH_FAILURE: return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_NO_MATCH;
    case SOL_MIR_TERM_UNREACHABLE: return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_UNREACHABLE;
    default: return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL;
    }
}
static SolMirRuntimeCleanupProducerKind predicate_instruction_event_producer(
    const SolMirPredicateInstruction *input, const SolMirRuntimeValues *values) {
    if (input->kind == SOL_MIR_PREDICATE_INST_CONSTRUCT
        && input->recipe < values->allocation_plan_count
        && values->allocation_plans[input->recipe].kind != SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE)
        return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_SUPPLEMENTAL_ALLOCATION;
    return input->failures ? SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_ARITHMETIC
        : SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL;
}
static SolMirRuntimeCleanupProducerKind predicate_terminator_event_producer(
    const SolMirPredicateTerminator *term) {
    if (term->kind == SOL_MIR_PREDICATE_TERM_INVOKE)
        return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_INVOKE;
    if (term->kind == SOL_MIR_PREDICATE_TERM_FAILURE
        && term->failure_kind == SOL_MIR_PREDICATE_FAILURE_NO_MATCH)
        return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_NO_MATCH;
    if (term->kind == SOL_MIR_PREDICATE_TERM_RETURN)
        return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_RESULT;
    return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL;
}
static bool alloc_table(void **p,size_t n,size_t z,bool scratch){
    *p=n?allocation(n,z,scratch):NULL;
    if(*p)memset(*p,0xff,n*z);
    return !n||*p;
}
static bool totals(const SolMirRuntimeCleanup *cleanup, const SolMirOperations *operations,
    size_t *cleanup_total, size_t *semantic_total) {
    size_t cleanup_count = 0, semantic_count = 0;
    if (!add(&cleanup_count, cleanup->event_count) || !add(&cleanup_count, cleanup->action_count)
        || !add(&cleanup_count, cleanup->transition_count)
        || !add(&cleanup_count, cleanup->supplemental_site_count)
        || !add(&cleanup_count, cleanup->drop_path_count)
        || !add(&semantic_count, operations->access_plan_count)
        || !add(&semantic_count, operations->constructor_count)
        || !add(&semantic_count, operations->pattern_test_count)
        || !add(&semantic_count, operations->pattern_extraction_count)
        || !add(&semantic_count, operations->propagation_count)
        || !add(&semantic_count, operations->arithmetic_count)
        || !add(&semantic_count, operations->snapshot_count)
        || !add(&semantic_count, operations->callable_count)
        || !add(&semantic_count, operations->handler_count)
        || !add(&semantic_count, operations->predicate_count)
        || !add(&semantic_count, operations->import_snapshot_count)) return false;
    *cleanup_total = cleanup_count; *semantic_total = semantic_count; return true;
}
static bool build_scratch_size(const SolMirMaterialization *materialization,
    const SolMirOperations *operations, size_t grants, size_t owned_bytes,
    size_t *coverage_count, size_t *dedup_count, size_t *bytes) {
    size_t coverage = 0, dedup = 0, coverage_bytes, dedup_bytes;
    if (!add(&coverage, materialization->instruction_count)
        || !add(&coverage, materialization->block_count)
        || !add(&coverage, operations->predicate_instruction_count)
        || !add(&coverage, operations->predicate_block_count)
        || !add(&coverage, operations->predicate_edge_count)
        || !add(&dedup, grants)
        || !mul(coverage, sizeof(unsigned char), &coverage_bytes)
        || !mul(dedup, sizeof(size_t), &dedup_bytes)
        || !add(&coverage_bytes, dedup_bytes)
        || !add(&coverage_bytes, owned_bytes)) return false;
    *coverage_count = coverage; *dedup_count = dedup; *bytes = coverage_bytes;
    return true;
}
static bool table_rows(const SolMirRuntimeLoweredProgram *owner, size_t *out) {
    size_t rows = 0;
#define ADD_ROWS(member,type,singular) if (!add(&rows, owner->singular##_count)) return false;
    SOL_MIR_RUNTIME_LOWERED_TABLES(ADD_ROWS)
#undef ADD_ROWS
    *out = rows;
    return true;
}
static bool table_allocation_attempts(const SolMirRuntimeLoweredProgram *owner,
    size_t *out) {
    size_t attempts = 0;
#define ATTEMPT(member,type,singular) if (owner->singular##_count && !add(&attempts, 1)) return false;
    SOL_MIR_RUNTIME_LOWERED_TABLES(ATTEMPT)
#undef ATTEMPT
    *out = attempts;
    return true;
}
/* Persistent transfer is reserved, but never precharged: one allocation
 * attempt per nonempty table, one byte-preserving record copy per row, and
 * both complete builder seal walks. */
static bool persistent_work(const SolMirRuntimeLoweredProgram *owner, size_t *out) {
    size_t rows, bytes = 0, attempts = 0, result, ignored, seal_work = 0;
    if (!table_rows(owner, &rows) || !table_allocation_attempts(owner, &attempts)) return false;
#define SEAL_BYTES(member,type,singular) \
    if (!mul(owner->singular##_count, sizeof(type), &ignored) || !add(&bytes, ignored)) return false;
    SOL_MIR_RUNTIME_LOWERED_TABLES(SEAL_BYTES)
#undef SEAL_BYTES
    /* 27 table raw triples, 27 usage words, 10 remaining usage words,
     * 5 predecessor pointers, and 34 limit words. */
    if (!add(&bytes, 157) || !add(&seal_work, bytes)
        || !add(&seal_work, bytes) || !add(&attempts, rows)
        || !add(&attempts, seal_work)) return false;
    result = attempts;
    *out = result;
    return true;
}
static bool copy_tables(SolMirRuntimeLoweredProgram *to,
    const SolMirRuntimeLoweredProgram *from, SolMirRuntimeLoweredWorkMeter *meter) {
#define COPY(member,type,singular) do { \
    if (!alloc_table((void **)&to->member, to->singular##_count, sizeof(type), false)) return false; \
    for(size_t i = 0; i < to->singular##_count; ++i) { \
        if (!sol_mir_runtime_lowered_tick(meter)) return false; \
        memcpy(&to->member[i], &from->member[i], sizeof to->member[i]); \
    } \
} while (0);
    SOL_MIR_RUNTIME_LOWERED_TABLES(COPY)
#undef COPY
    return true;
}
static bool validation_scratch_size(const SolMirRuntimeLoweredProgram *owner, size_t *out) {
    size_t rows = 0, bytes;
#define ADD_ROWS(member,type,singular) if (!add(&rows, owner->singular##_count)) return false;
    SOL_MIR_RUNTIME_LOWERED_TABLES(ADD_ROWS)
#undef ADD_ROWS
    if (!mul(rows, 2, &bytes)) return false;
    *out = bytes; return true;
}
static bool render_bounds(const SolMirRuntimeLoweredProgram *owner, size_t *bytes,
    size_t *scratch) {
    size_t rows = 0, relation_bytes, line_bytes;
#define ADD_ROWS(member,type,singular) if (!add(&rows, owner->singular##_count)) return false;
    SOL_MIR_RUNTIME_LOWERED_TABLES(ADD_ROWS)
#undef ADD_ROWS
    if (!mul(rows, SOL_MIR_RUNTIME_LOWERED_RENDER_LINE_CAPACITY, &relation_bytes)
        || !add(&relation_bytes, SOL_MIR_RUNTIME_LOWERED_RENDER_HEADER_CAPACITY)
        || !mul(rows, sizeof(LoweredRenderLine), &line_bytes)) return false;
    *bytes = relation_bytes;
    *scratch = line_bytes;
    return true;
}
static bool fit(const SolMirRuntimeLoweredProgram*x){
#define FIT(member,type,singular) if(x->singular##_count>x->limits.max_##member)return false;
    SOL_MIR_RUNTIME_LOWERED_TABLES(FIT)
#undef FIT
    return x->usage.owned_bytes<=x->limits.max_owned_bytes&&x->usage.build_scratch_bytes<=x->limits.max_build_scratch_bytes&&x->usage.build_work<=x->limits.max_build_work&&x->usage.validation_scratch_bytes<=x->limits.max_validation_scratch_bytes&&x->usage.validation_work<=x->limits.max_validation_work&&x->usage.render_bytes<=x->limits.max_render_bytes&&x->usage.render_scratch_bytes<=x->limits.max_render_scratch_bytes;
}
static bool usage(SolMirRuntimeLoweredProgram*x){
    size_t b=0,n=0;
#define COUNT(member,type,singular) if(!mul(x->singular##_count,sizeof(type),&n)||!add(&b,n))return false;
    SOL_MIR_RUNTIME_LOWERED_TABLES(COUNT)
#undef COUNT
    x->usage.owned_bytes=b;return fit(x);
}
SolMirRuntimeLoweredProgramBuildOutcome sol_mir_runtime_lowered_program_build(const SolMirRuntimeLoweredProgramBuildRequest*r,SolMirRuntimeLoweredProgram*out,SolDiagnostics*d){if(!r||!out||!r->conventions||!r->values||!r->cleanup||!r->host_abi||!r->handler_abi||out->authentication||(r->limits&&!zero(*r->limits)&&!full(*r->limits))){report(d,"invalid runtime lowered program build request");return SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_INVALID_ARGUMENT;}if(r->values->conventions!=r->conventions||r->cleanup->conventions!=r->conventions||r->cleanup->values!=r->values||r->host_abi->conventions!=r->conventions||r->host_abi->values!=r->values||r->host_abi->cleanup!=r->cleanup||r->handler_abi->conventions!=r->conventions||r->handler_abi->values!=r->values||r->handler_abi->cleanup!=r->cleanup||r->handler_abi->host_abi!=r->host_abi||!sol_mir_runtime_conventions_validate(r->conventions,d)||!sol_mir_runtime_values_validate(r->values,d)||!sol_mir_runtime_cleanup_validate(r->cleanup,d)||!sol_mir_runtime_host_abi_validate(r->host_abi,d)||!sol_mir_runtime_handler_abi_validate(r->handler_abi,d)){report(d,"invalid runtime lowered predecessor");return SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_INVALID_PREDECESSOR;}SolMirRuntimeLoweredProgram x;sol_mir_runtime_lowered_program_init(&x);x.conventions=r->conventions;x.values=r->values;x.cleanup=r->cleanup;x.host_abi=r->host_abi;x.handler_abi=r->handler_abi;x.limits=!r->limits||zero(*r->limits)?sol_mir_runtime_lowered_program_default_limits():*r->limits;SolMirRuntimeLoweredWorkMeter build_meter={x.limits.max_build_work,0,false};SolMirRuntimeLoweredWorkMeter *previous_build_meter=active_build_meter;active_build_meter=&build_meter;const SolMirMaterialization*m=&r->conventions->concrete->materialization;const SolMirOperations*o=&r->conventions->concrete->operations;
    unsigned char *coverage = NULL;
    size_t *grant_dedup = NULL;
    size_t coverage_count, grant_dedup_count, build_scratch_bytes;
    size_t cleanup_total, semantic_total;
    if (!totals(r->cleanup, o, &cleanup_total, &semantic_total)) goto exhausted;
    DemandWriter demand_counter = {0};
    if (!count_demands(&demand_counter, r->conventions, r->values, r->cleanup, m, o)) goto invalid;
    size_t distinct_grants=0;
    for(size_t i=0;(i<r->host_abi->requirement_count)&&BUILD_TICK();i++){const SolMirRuntimeHostRequirement*q=&r->host_abi->requirements[i];bool prior=false;for(size_t j=0;(j<i)&&BUILD_TICK();j++){const SolMirRuntimeHostRequirement*z=&r->host_abi->requirements[j];if(z->entry==q->entry&&z->root==q->root&&z->operation==q->operation){prior=true;break;}}if(!prior)++distinct_grants;}
#define SET(member,singular,count) x.singular##_count=x.singular##_capacity=(count)
 SET(image_instructions,image_instruction,m->instruction_count);SET(image_blocks,image_block,m->block_count);SET(image_edges,image_edge,m->edge_count);SET(image_incoming_edges,image_incoming_edge,m->edge_count);SET(image_outgoing_edges,image_outgoing_edge,m->edge_count);SET(image_terminators,image_terminator,m->block_count);SET(predicate_bodies,predicate_body,o->predicate_body_count);SET(predicate_instructions,predicate_instruction,o->predicate_instruction_count);SET(predicate_blocks,predicate_block,o->predicate_block_count);SET(predicate_edges,predicate_edge,o->predicate_edge_count);SET(predicate_incoming_edges,predicate_incoming_edge,o->predicate_edge_count);SET(predicate_outgoing_edges,predicate_outgoing_edge,o->predicate_edge_count);SET(predicate_terminators,predicate_terminator,o->predicate_block_count);SET(semantic_plans,semantic_plan,semantic_total);SET(calls,call,r->conventions->call_count);SET(signatures,signature,r->conventions->signature_count);SET(imports,import,r->conventions->import_count);SET(recipes,recipe,r->values->recipe_operation_count);SET(value_plans,value_plan,r->values->recipe_operation_count);SET(recipe_demands,recipe_demand,demand_counter.count);SET(host_requirements,host_requirement,r->host_abi->requirement_count);SET(host_grants,host_grant,distinct_grants);SET(host_incidences,host_incidence,r->host_abi->requirement_count);SET(handler_frames,handler_frame,r->handler_abi->frame_count);SET(handler_markers,handler_marker,r->handler_abi->exit_marker_count);SET(handler_exits,handler_exit,r->handler_abi->cleanup_exit_count);x.cleanup_failure_count=x.cleanup_failure_capacity=cleanup_total;
#undef SET
#define COPY(member,type,singular) x.usage.member=x.singular##_count;
 SOL_MIR_RUNTIME_LOWERED_TABLES(COPY)
#undef COPY
 x.usage.erased_loops=m->loop_count;x.usage.provenance_records=o->provenance_count;x.usage.demanded_semantic_plans=x.semantic_plan_count;
  if (!usage(&x)
      || !build_scratch_size(m,o,distinct_grants,x.usage.owned_bytes,
          &coverage_count,&grant_dedup_count,&build_scratch_bytes)
       || !validation_scratch_size(&x,&x.usage.validation_scratch_bytes)
       || !render_bounds(&x, &x.usage.render_bytes, &x.usage.render_scratch_bytes)) goto exhausted;
  x.usage.build_scratch_bytes=build_scratch_bytes;
  if(!fit(&x))goto exhausted;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
  /* This is an observation, not a precharge. */
  last_build_work=(SolMirRuntimeLoweredProgramTestBuildWork){
      build_meter.used,0,0,0,build_scratch_bytes};
#endif
  if(coverage_count&&(coverage=allocation(coverage_count,sizeof *coverage,true))==NULL)goto allocation;
  if(grant_dedup_count&&(grant_dedup=allocation(grant_dedup_count,sizeof *grant_dedup,true))==NULL)goto allocation;
 if(coverage_count)memset(coverage,0,coverage_count);
 if(grant_dedup_count)memset(grant_dedup,0xff,grant_dedup_count*sizeof *grant_dedup);
#define ALLOC(member,type,singular) if(!alloc_table((void**)&x.member,x.singular##_count,sizeof(type),true))goto allocation;
 SOL_MIR_RUNTIME_LOWERED_TABLES(ALLOC)
#undef ALLOC

    for(size_t i=0;(i<m->instruction_count)&&BUILD_TICK();i++){
        SolMirRuntimeLoweredExecution e;size_t image=image_for_instruction(m,i);
        if(image==SOL_MIR_RUNTIME_NONE||!image_instruction(m->instructions[i].kind,&e)){report(d,"unknown image instruction");goto unsupported;}
        x.image_instructions[i].state=SOL_MIR_RUNTIME_LOWERED_PRESENT;x.image_instructions[i].execution=e;x.image_instructions[i].image=image;x.image_instructions[i].instruction=i;x.image_instructions[i].block=m->instructions[i].block;x.image_instructions[i].kind=m->instructions[i].kind;{SolMirRuntimeLoweredDemandDescriptor desc;descriptor(image_instruction_descriptors,sizeof image_instruction_descriptors/sizeof *image_instruction_descriptors,(int)m->instructions[i].kind,&desc);x.image_instructions[i].runtime_class=desc.runtime_class;x.image_instructions[i].plan_family=desc.plan_family;x.image_instructions[i].facilities=desc.facilities;if(m->instructions[i].kind==SOL_MIR_INST_FUNCTION_VALUE)x.image_instructions[i].facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION;if(m->instructions[i].kind==SOL_MIR_INST_LOAD_COPY&&!copy_requires_runtime(o->layout->representation->recipes[m->instructions[i].type].copy_kind))x.image_instructions[i].facilities&=~(uint32_t)SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY;x.image_instructions[i].plan=image_instruction_semantic(m,o,i);{SolMirRuntimeCleanupProducerKind producer;if(image_instruction_event_producer(m,o,r->values,i,&producer))x.image_instructions[i].cleanup_event=cleanup_event_for(r->cleanup,SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION,image,m->instructions[i].block,i,true,producer);}x.image_instructions[i].failure_site=cleanup_failure_for_event(r->cleanup,x.image_instructions[i].cleanup_event);if(m->instructions[i].kind==SOL_MIR_INST_BINARY||m->instructions[i].kind==SOL_MIR_INST_COMPOUND_UPDATE)for(size_t plan=0;(plan<o->arithmetic_count)&&BUILD_TICK();plan++)if(o->arithmetic[plan].instruction==i&&o->arithmetic[plan].failures)x.image_instructions[i].facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE;if(m->instructions[i].kind==SOL_MIR_INST_CONSTRUCT)for(size_t plan=0;(plan<o->constructor_count)&&BUILD_TICK();plan++)if(o->constructors[plan].instruction==i)x.image_instructions[i].facilities|=value_facilities(r->values,o->constructors[plan].result_recipe)&(SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION|SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP);if(m->instructions[i].kind==SOL_MIR_INST_CAPTURE_SNAPSHOT){bool copy=false;for(size_t plan=0;(plan<o->snapshot_count)&&BUILD_TICK();plan++)if(o->snapshots[plan].instruction==i&&copy_requires_runtime(o->snapshots[plan].copy_kind))copy=true;if(!copy)x.image_instructions[i].facilities&=~(uint32_t)SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY;}if(m->instructions[i].kind==SOL_MIR_INST_PATTERN_VALUE)for(size_t plan=0;(plan<o->pattern_extraction_count)&&BUILD_TICK();plan++)if(o->pattern_extractions[plan].instruction==i&&copy_requires_runtime(o->pattern_extractions[plan].copy_kind))x.image_instructions[i].facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY;}
    }
    for(size_t i=0;(i<m->block_count)&&BUILD_TICK();i++){
        size_t image=image_for_block(m,i);SolMirRuntimeLoweredExecution e;
        if(image==SOL_MIR_RUNTIME_NONE||!image_terminator(m->blocks[i].terminator.kind,&e)){report(d,"unknown image terminator");goto unsupported;}
        x.image_blocks[i].state=SOL_MIR_RUNTIME_LOWERED_PRESENT;x.image_blocks[i].image=image;x.image_blocks[i].block=i;
        x.image_terminators[i].state=SOL_MIR_RUNTIME_LOWERED_PRESENT;x.image_terminators[i].execution=e;x.image_terminators[i].image=image;x.image_terminators[i].block=i;x.image_terminators[i].kind=m->blocks[i].terminator.kind;{SolMirRuntimeLoweredDemandDescriptor desc;descriptor(image_terminator_descriptors,sizeof image_terminator_descriptors/sizeof *image_terminator_descriptors,(int)m->blocks[i].terminator.kind,&desc);x.image_terminators[i].runtime_class=desc.runtime_class;x.image_terminators[i].plan_family=desc.plan_family;x.image_terminators[i].facilities=desc.facilities;x.image_terminators[i].plan=image_terminator_semantic(m,o,i);x.image_terminators[i].cleanup_event=cleanup_event_for(r->cleanup,SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR,image,i,SOL_MIR_RUNTIME_NONE,true,image_terminator_event_producer(m->blocks[i].terminator.kind));x.image_terminators[i].failure_site=cleanup_failure_for_event(r->cleanup,x.image_terminators[i].cleanup_event);x.image_terminators[i].pre_operation_cleanup_event=SOL_MIR_RUNTIME_LOWERED_NONE;x.image_terminators[i].pre_operation_supplemental_site=SOL_MIR_RUNTIME_LOWERED_NONE;if(m->blocks[i].terminator.kind==SOL_MIR_TERM_INVOKE){size_t call=image_call_for_block(r->conventions,image,i);if(call==SOL_MIR_RUNTIME_NONE||x.image_terminators[i].cleanup_event==SOL_MIR_RUNTIME_LOWERED_NONE||x.image_terminators[i].failure_site!=r->conventions->calls[call].failure_site)goto invalid;x.image_terminators[i].call=call;x.image_terminators[i].facilities|=import_for_call(r->conventions,&r->conventions->calls[call])!=SOL_MIR_RUNTIME_NONE?SOL_MIR_RUNTIME_LOWERED_FACILITY_IMPORT:0;}
        const SolMirMaterializedTerminator *term=&m->blocks[i].terminator; size_t pre=SOL_MIR_RUNTIME_LOWERED_NONE;
        if(term->kind==SOL_MIR_TERM_INVOKE&&term->callable_site!=SOL_MIR_MATERIALIZED_NONE) pre=cleanup_pre_operation_event_for(r->cleanup,SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_INVOKE_CALLABLE,image,i,SOL_MIR_RUNTIME_NONE,term->callable_site,SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CALLABLE_CONSTRUCTION);
        else if(term->kind==SOL_MIR_TERM_PROPAGATE) for(size_t plan=0;(plan<o->propagation_count)&&BUILD_TICK();++plan) if(o->propagations[plan].image==image&&o->propagations[plan].block==i&&o->propagations[plan].residual_recipe<r->values->allocation_plan_count&&r->values->allocation_plans[o->propagations[plan].residual_recipe].kind!=SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE){if(pre!=SOL_MIR_RUNTIME_LOWERED_NONE)goto invalid;pre=cleanup_pre_operation_event_for(r->cleanup,SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_PROPAGATE_RESIDUAL,image,i,plan,SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PROPAGATION_RESIDUAL);}
        if(pre!=SOL_MIR_RUNTIME_LOWERED_NONE){x.image_terminators[i].pre_operation_cleanup_event=pre;x.image_terminators[i].pre_operation_supplemental_site=r->cleanup->events[pre].supplemental_site;}}
    }
    for(size_t i=0;(i<m->edge_count)&&BUILD_TICK();i++){size_t image=image_for_block(m,m->edges[i].block),source,ordinal;if(image==SOL_MIR_RUNTIME_NONE||!edge_source(m,i,&source,&ordinal)){report(d,"unknown image edge");goto unsupported;}x.image_edges[i].state=SOL_MIR_RUNTIME_LOWERED_PRESENT;x.image_edges[i].image=image;x.image_edges[i].edge=i;x.image_edges[i].target=m->edges[i].block;x.image_edges[i].source=source;x.image_edges[i].ordinal=ordinal;}
    size_t image_incoming=0,image_outgoing=0;for(size_t block=0;(block<m->block_count)&&BUILD_TICK();block++){SolMirRuntimeLoweredImageBlock*row=&x.image_blocks[block];row->incoming_edges=(SolMirRuntimeSlice){image_incoming,0};row->outgoing_edges=(SolMirRuntimeSlice){image_outgoing,0};for(size_t edge=0;(edge<m->edge_count)&&BUILD_TICK();edge++){const SolMirRuntimeLoweredImageEdge*relation=&x.image_edges[edge];if(relation->target==block){x.image_incoming_edges[image_incoming++]=(SolMirRuntimeLoweredImageBlockEdge){SOL_MIR_RUNTIME_LOWERED_PRESENT,relation->image,block,edge,relation->ordinal};++row->incoming_edges.count;}if(relation->source==block){x.image_outgoing_edges[image_outgoing++]=(SolMirRuntimeLoweredImageBlockEdge){SOL_MIR_RUNTIME_LOWERED_PRESENT,relation->image,block,edge,relation->ordinal};++row->outgoing_edges.count;}}}if(image_incoming!=x.image_incoming_edge_count||image_outgoing!=x.image_outgoing_edge_count)goto invalid;
    for(size_t i=0;(i<o->predicate_body_count)&&BUILD_TICK();i++){const SolMirPredicateBody*q=&o->predicate_bodies[i];x.predicate_bodies[i]=(SolMirRuntimeLoweredPredicateBody){.state=SOL_MIR_RUNTIME_LOWERED_PRESENT,.body=i,.owner_kind=q->owner_kind,.image=q->instance,.import_id=q->import,.context=q->context,.phase=q->phase,.outcome=q->outcome,.blocks={q->blocks.offset,q->blocks.count},.entry=q->entry,.output_recipe=q->output_recipe,.refinement_self_recipe=q->refinement_self_recipe};}
    for(size_t i=0;(i<o->predicate_instruction_count)&&BUILD_TICK();i++){const SolMirPredicateInstruction*q=&o->predicate_instructions[i];if(q->block>=o->predicate_block_count||!predicate_instruction(q->kind)){report(d,"unknown predicate instruction");goto unsupported;}x.predicate_instructions[i].state=SOL_MIR_RUNTIME_LOWERED_PRESENT;x.predicate_instructions[i].body=o->predicate_blocks[q->block].body;x.predicate_instructions[i].instruction=i;x.predicate_instructions[i].block=q->block;x.predicate_instructions[i].kind=q->kind;{SolMirRuntimeLoweredDemandDescriptor desc;descriptor(predicate_instruction_descriptors,sizeof predicate_instruction_descriptors/sizeof *predicate_instruction_descriptors,(int)q->kind,&desc);x.predicate_instructions[i].runtime_class=desc.runtime_class;x.predicate_instructions[i].plan_family=desc.plan_family;x.predicate_instructions[i].facilities=desc.facilities;if(q->kind==SOL_MIR_PREDICATE_INST_CONSTRUCT&&q->recipe<r->values->allocation_plan_count&&r->values->allocation_plans[q->recipe].kind!=SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE)x.predicate_instructions[i].facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION;if(q->kind==SOL_MIR_PREDICATE_INST_PATTERN_EXTRACT&&copy_requires_runtime(o->layout->representation->recipes[q->recipe].copy_kind))x.predicate_instructions[i].facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY;x.predicate_instructions[i].plan=predicate_instruction_semantic(m,o,i);x.predicate_instructions[i].cleanup_event=cleanup_event_for(r->cleanup,SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION,o->predicate_blocks[q->block].body,q->block,i,true,predicate_instruction_event_producer(q,r->values));x.predicate_instructions[i].failure_site=cleanup_failure_for_event(r->cleanup,x.predicate_instructions[i].cleanup_event);if(q->kind==SOL_MIR_PREDICATE_INST_BINARY&&q->failures)x.predicate_instructions[i].facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE;}}
    for(size_t i=0;(i<o->predicate_block_count)&&BUILD_TICK();i++){const SolMirPredicateBlock*q=&o->predicate_blocks[i];if(!predicate_terminator(q->terminator.kind)){report(d,"unknown predicate terminator");goto unsupported;}x.predicate_blocks[i].state=SOL_MIR_RUNTIME_LOWERED_PRESENT;x.predicate_blocks[i].body=q->body;x.predicate_blocks[i].block=i;x.predicate_terminators[i].state=SOL_MIR_RUNTIME_LOWERED_PRESENT;x.predicate_terminators[i].body=q->body;x.predicate_terminators[i].block=i;x.predicate_terminators[i].kind=q->terminator.kind;{SolMirRuntimeLoweredDemandDescriptor desc;descriptor(predicate_terminator_descriptors,sizeof predicate_terminator_descriptors/sizeof *predicate_terminator_descriptors,(int)q->terminator.kind,&desc);x.predicate_terminators[i].runtime_class=desc.runtime_class;x.predicate_terminators[i].plan_family=desc.plan_family;x.predicate_terminators[i].facilities=desc.facilities;x.predicate_terminators[i].plan=predicate_terminator_semantic(m,o,i);x.predicate_terminators[i].cleanup_event=cleanup_event_for(r->cleanup,SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR,q->body,i,SOL_MIR_RUNTIME_NONE,true,predicate_terminator_event_producer(&q->terminator));x.predicate_terminators[i].failure_site=cleanup_failure_for_event(r->cleanup,x.predicate_terminators[i].cleanup_event);if(q->terminator.kind==SOL_MIR_PREDICATE_TERM_INVOKE){size_t call=predicate_call_for_block(r->conventions,q->body,i);if(call==SOL_MIR_RUNTIME_NONE||x.predicate_terminators[i].cleanup_event==SOL_MIR_RUNTIME_LOWERED_NONE||x.predicate_terminators[i].failure_site!=r->conventions->calls[call].failure_site)goto invalid;x.predicate_terminators[i].call=call;x.predicate_terminators[i].facilities|=import_for_call(r->conventions,&r->conventions->calls[call])!=SOL_MIR_RUNTIME_NONE?SOL_MIR_RUNTIME_LOWERED_FACILITY_IMPORT:0;}}}
    for(size_t i=0;(i<o->predicate_edge_count)&&BUILD_TICK();i++){const SolMirPredicateEdge*q=&o->predicate_edges[i];size_t source,ordinal;if(q->source>=o->predicate_block_count||q->target>=o->predicate_block_count||!predicate_edge_source(o,i,&source,&ordinal)||source!=q->source){report(d,"unknown predicate edge");goto unsupported;}x.predicate_edges[i].state=SOL_MIR_RUNTIME_LOWERED_PRESENT;x.predicate_edges[i].body=o->predicate_blocks[q->source].body;x.predicate_edges[i].edge=i;x.predicate_edges[i].target=q->target;x.predicate_edges[i].source=source;x.predicate_edges[i].ordinal=ordinal;}
    size_t predicate_incoming=0,predicate_outgoing=0;for(size_t block=0;(block<o->predicate_block_count)&&BUILD_TICK();block++){SolMirRuntimeLoweredPredicateBlock*row=&x.predicate_blocks[block];row->incoming_edges=(SolMirRuntimeSlice){predicate_incoming,0};row->outgoing_edges=(SolMirRuntimeSlice){predicate_outgoing,0};for(size_t edge=0;(edge<o->predicate_edge_count)&&BUILD_TICK();edge++){const SolMirRuntimeLoweredPredicateEdge*relation=&x.predicate_edges[edge];if(relation->target==block){x.predicate_incoming_edges[predicate_incoming++]=(SolMirRuntimeLoweredPredicateBlockEdge){SOL_MIR_RUNTIME_LOWERED_PRESENT,relation->body,block,edge,relation->ordinal};++row->incoming_edges.count;}if(relation->source==block){x.predicate_outgoing_edges[predicate_outgoing++]=(SolMirRuntimeLoweredPredicateBlockEdge){SOL_MIR_RUNTIME_LOWERED_PRESENT,relation->body,block,edge,relation->ordinal};++row->outgoing_edges.count;}}}if(predicate_incoming!=x.predicate_incoming_edge_count||predicate_outgoing!=x.predicate_outgoing_edge_count)goto invalid;
    DemandWriter demand_writer={x.recipe_demands,0};
    for(size_t i=0;(i<m->instruction_count)&&BUILD_TICK();i++){SolMirRuntimeLoweredImageInstruction*row=&x.image_instructions[i];row->demanded_recipes.offset=demand_writer.count;if(!image_instruction_demands(&demand_writer,m,o,r->values,r->cleanup,i))goto invalid;row->demanded_recipes.count=demand_writer.count-row->demanded_recipes.offset;}
    for(size_t i=0;(i<m->block_count)&&BUILD_TICK();i++){SolMirRuntimeLoweredImageTerminator*row=&x.image_terminators[i];row->demanded_recipes.offset=demand_writer.count;if(!image_terminator_demands(&demand_writer,r->conventions,r->values,m,o,row->image,i))goto invalid;row->demanded_recipes.count=demand_writer.count-row->demanded_recipes.offset;}
    for(size_t i=0;(i<o->predicate_instruction_count)&&BUILD_TICK();i++){SolMirRuntimeLoweredPredicateInstruction*row=&x.predicate_instructions[i];row->demanded_recipes.offset=demand_writer.count;if(!predicate_instruction_demands(&demand_writer,o,r->values,i))goto invalid;row->demanded_recipes.count=demand_writer.count-row->demanded_recipes.offset;}
    for(size_t i=0;(i<o->predicate_block_count)&&BUILD_TICK();i++){SolMirRuntimeLoweredPredicateTerminator*row=&x.predicate_terminators[i];row->demanded_recipes.offset=demand_writer.count;if(!predicate_terminator_demands(&demand_writer,r->conventions,r->values,o,row->body,i))goto invalid;row->demanded_recipes.count=demand_writer.count-row->demanded_recipes.offset;}
    if(demand_writer.count!=x.recipe_demand_count)goto invalid;
    for(size_t i=0;(i<o->provenance_count)&&BUILD_TICK();i++)if(!provenance(o->provenance[i].kind)){report(d,"unknown provenance");goto unsupported;}size_t semantic=0;
#define SEMANTIC(member,arena,family,facilities,producer_kind,producer) do{x.semantic_plans[semantic]=(SolMirRuntimeLoweredSemanticPlan){SOL_MIR_RUNTIME_LOWERED_PRESENT,arena,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,family,i,facilities,producer_kind,producer};++semantic;}while(0)
    for(size_t i=0;(i<o->access_plan_count)&&BUILD_TICK();i++)SEMANTIC(access_plans,SOL_MIR_RUNTIME_LOWERED_SEMANTIC_ACCESS,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE,SOL_MIR_MATERIALIZED_PRODUCER_ROOT,o->access_plans[i].place);
    for(size_t i=0;(i<o->constructor_count)&&BUILD_TICK();i++)SEMANTIC(constructors,SOL_MIR_RUNTIME_LOWERED_SEMANTIC_CONSTRUCT,SOL_MIR_RUNTIME_LOWERED_PLAN_CONSTRUCT,SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION|SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP,SOL_MIR_MATERIALIZED_PRODUCER_INSTRUCTION,o->constructors[i].instruction);
    for(size_t i=0;(i<o->pattern_test_count)&&BUILD_TICK();i++)SEMANTIC(pattern_tests,SOL_MIR_RUNTIME_LOWERED_SEMANTIC_PATTERN_TEST,SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE,SOL_MIR_MATERIALIZED_PRODUCER_INSTRUCTION,o->pattern_tests[i].instruction);
    for(size_t i=0;(i<o->pattern_extraction_count)&&BUILD_TICK();i++)SEMANTIC(pattern_extractions,SOL_MIR_RUNTIME_LOWERED_SEMANTIC_PATTERN_EXTRACTION,SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY,SOL_MIR_MATERIALIZED_PRODUCER_INSTRUCTION,o->pattern_extractions[i].instruction);
    for(size_t i=0;(i<o->propagation_count)&&BUILD_TICK();i++)SEMANTIC(propagations,SOL_MIR_RUNTIME_LOWERED_SEMANTIC_PROPAGATION,SOL_MIR_RUNTIME_LOWERED_PLAN_PROPAGATION,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP,SOL_MIR_MATERIALIZED_PRODUCER_TERMINATOR,o->propagations[i].block);
    for(size_t i=0;(i<o->arithmetic_count)&&BUILD_TICK();i++)SEMANTIC(arithmetic,SOL_MIR_RUNTIME_LOWERED_SEMANTIC_ARITHMETIC,SOL_MIR_RUNTIME_LOWERED_PLAN_ARITHMETIC,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP|SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE,SOL_MIR_MATERIALIZED_PRODUCER_INSTRUCTION,o->arithmetic[i].instruction);
    for(size_t i=0;(i<o->snapshot_count)&&BUILD_TICK();i++)SEMANTIC(snapshots,SOL_MIR_RUNTIME_LOWERED_SEMANTIC_SNAPSHOT,SOL_MIR_RUNTIME_LOWERED_PLAN_SNAPSHOT,SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE,SOL_MIR_MATERIALIZED_PRODUCER_INSTRUCTION,o->snapshots[i].instruction);
    for(size_t i=0;(i<o->callable_count)&&BUILD_TICK();i++)SEMANTIC(callables,SOL_MIR_RUNTIME_LOWERED_SEMANTIC_CALLABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CALLABLE,SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE,m->semantic_sites[o->callables[i].semantic_site].producer_kind,o->callables[i].semantic_site);
    for(size_t i=0;(i<o->handler_count)&&BUILD_TICK();i++)SEMANTIC(handlers,SOL_MIR_RUNTIME_LOWERED_SEMANTIC_HANDLER,SOL_MIR_RUNTIME_LOWERED_PLAN_HANDLER,SOL_MIR_RUNTIME_LOWERED_FACILITY_HANDLER_FRAME,SOL_MIR_MATERIALIZED_PRODUCER_HANDLER,o->handlers[i].handler);
    for(size_t i=0;(i<o->predicate_count)&&BUILD_TICK();i++)SEMANTIC(predicates,SOL_MIR_RUNTIME_LOWERED_SEMANTIC_PREDICATE,SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP,SOL_MIR_MATERIALIZED_PRODUCER_PREDICATE,o->predicates[i].body);
    for(size_t i=0;(i<o->import_snapshot_count)&&BUILD_TICK();i++)SEMANTIC(import_snapshots,SOL_MIR_RUNTIME_LOWERED_SEMANTIC_IMPORT_SNAPSHOT,SOL_MIR_RUNTIME_LOWERED_PLAN_IMPORT_SNAPSHOT,SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE,SOL_MIR_MATERIALIZED_PRODUCER_PREDICATE,o->import_snapshots[i].provenance);
#undef SEMANTIC
    if(semantic!=x.semantic_plan_count)goto invalid;
    for(size_t i=0;(i<r->conventions->call_count)&&BUILD_TICK();i++){const SolMirRuntimeCall*q=&r->conventions->calls[i];x.calls[i]=(SolMirRuntimeLoweredCall){SOL_MIR_RUNTIME_LOWERED_PRESENT,i,q->signature,q->owner_kind,q->image,q->predicate,q->block,q->call_kind,q->target_kind,q->internal,q->host,q->table,q->callee,q->operands,q->result,q->normal_edge,q->failure_edge,q->writebacks,import_for_call(r->conventions,q),bound_environment_import_for_call(r->conventions,q),entry_for_call(r->conventions,q),q->failure_site};}
    for(size_t i=0;(i<r->conventions->signature_count)&&BUILD_TICK();i++){x.signatures[i].state=SOL_MIR_RUNTIME_LOWERED_PRESENT;x.signatures[i].signature=i;}for(size_t i=0;(i<r->conventions->import_count)&&BUILD_TICK();i++){x.imports[i].state=SOL_MIR_RUNTIME_LOWERED_PRESENT;x.imports[i].import_id=i;}
    for(size_t i=0;(i<r->values->recipe_operation_count)&&BUILD_TICK();i++){const SolMirRuntimeRecipeOperations*q=&r->values->recipe_operations[i];uint32_t value_facility=value_facilities(r->values,i),value_demanded=value_demand(r->conventions,r->values,r->cleanup,o,i)&value_facility;SolMirRuntimeLoweredState recipe_state=q->demanded_operations?SOL_MIR_RUNTIME_LOWERED_PRESENT:SOL_MIR_RUNTIME_LOWERED_NOT_DEMANDED;SolMirRuntimeLoweredState value_state=value_demanded?SOL_MIR_RUNTIME_LOWERED_PRESENT:SOL_MIR_RUNTIME_LOWERED_NOT_DEMANDED;x.recipes[i].state=recipe_state;x.recipes[i].recipe=q->recipe;x.recipes[i].demanded_operations=q->demanded_operations;x.recipes[i].operations=i;x.recipes[i].create_import=q->create_import;x.recipes[i].copy_import=q->copy_import;x.recipes[i].drop_import=q->drop_import;x.recipes[i].equal_import=q->equal_import;x.recipes[i].facilities=recipe_facilities(q->demanded_operations);x.recipes[i].allocation=r->values->allocation_plans[i].kind;x.recipes[i].copy=r->values->copy_plans[i].classification;x.recipes[i].equality=r->values->equality_plans[i].classification;x.recipes[i].ownership=r->values->ownership_plans[i].classification;x.recipes[i].host_result=r->values->host_result_plans[i].classification;x.value_plans[i].state=value_state;x.value_plans[i].recipe=q->recipe;x.value_plans[i].operations=i;x.value_plans[i].facilities=value_facility;x.value_plans[i].demanded_facilities=value_demanded;x.value_plans[i].allocation=r->values->allocation_plans[i].kind;x.value_plans[i].copy=r->values->copy_plans[i].classification;x.value_plans[i].equality=r->values->equality_plans[i].classification;x.value_plans[i].ownership=r->values->ownership_plans[i].classification;x.value_plans[i].host_result=r->values->host_result_plans[i].classification;}
    size_t at = 0;
    for(size_t i = 0; (i < r->cleanup->event_count) && BUILD_TICK(); ++i) {
        const SolMirRuntimeCleanupEvent *q = &r->cleanup->events[i];
        x.cleanup_failures[at++] = (SolMirRuntimeLoweredCleanupFailure){
            .state = SOL_MIR_RUNTIME_LOWERED_PRESENT, .kind = SOL_MIR_RUNTIME_LOWERED_CLEANUP_EVENT,
            .record = i, .event = i, .event_kind = q->kind, .phase = q->phase, .origin = q->origin,
            .owner = q->owner, .block = q->block, .operation = q->operation,
            .semantic_site = q->semantic_site,
            .producer = q->producer, .inherited_failure_site = q->inherited_failure_site,
            .supplemental_site = q->supplemental_site, .actions = q->actions,
            .transitions = q->transitions, .captures_failure_detail = q->captures_failure_detail,
            .capture_detail_kind = q->capture_detail_kind,
        };
    }
    for(size_t i = 0; (i < r->cleanup->action_count) && BUILD_TICK(); ++i) {
        const SolMirRuntimeCleanupAction *q = &r->cleanup->actions[i];
        SolMirRuntimeLoweredCleanupFailure *row = &x.cleanup_failures[at++];
        *row = (SolMirRuntimeLoweredCleanupFailure){
            .state = SOL_MIR_RUNTIME_LOWERED_PRESENT, .kind = SOL_MIR_RUNTIME_LOWERED_CLEANUP_ACTION,
            .record = i, .action = i, .action_kind = q->kind, .action_flags = q->flags,
            .target = q->target, .recipe = q->recipe, .drop_path = q->drop_path,
            .frame = handler_frame_for_cleanup_action(r->handler_abi, i),
        };
    }
    for(size_t i = 0; (i < r->cleanup->transition_count) && BUILD_TICK(); ++i) {
        const SolMirRuntimeCleanupTransition *q = &r->cleanup->transitions[i];
        x.cleanup_failures[at++] = (SolMirRuntimeLoweredCleanupFailure){
            .state = SOL_MIR_RUNTIME_LOWERED_PRESENT, .kind = SOL_MIR_RUNTIME_LOWERED_CLEANUP_TRANSITION,
            .record = i, .event = q->event, .transition = i, .continuation = q->continuation,
            .actions = q->actions, .source_edge = q->source_edge, .destination = q->destination,
            .primary_failure_wins = q->primary_failure_wins, .failure_source = q->failure_source,
            .failure_site = q->failure_site, .failure_mask = q->failure_mask,
            .edge_role = q->edge_role, .outcome = q->outcome,
            .contract_phase = q->contract_phase, .contract_outcome = q->contract_outcome,
        };
    }
    for(size_t i = 0; (i < r->cleanup->supplemental_site_count) && BUILD_TICK(); ++i) {
        const SolMirRuntimeCleanupSupplementalSite *q = &r->cleanup->supplemental_sites[i];
        x.cleanup_failures[at++] = (SolMirRuntimeLoweredCleanupFailure){
            .state = SOL_MIR_RUNTIME_LOWERED_PRESENT, .kind = SOL_MIR_RUNTIME_LOWERED_CLEANUP_SUPPLEMENTAL_SITE,
            .record = i, .event = q->event,
            .failure_source = SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_SUPPLEMENTAL_P33,
            .failure_site = i, .failure_mask = q->allowed_codes,
        };
    }
    for(size_t i = 0; (i < r->cleanup->drop_path_count) && BUILD_TICK(); ++i) {
        const SolMirRuntimeCleanupDropPath *q = &r->cleanup->drop_paths[i];
        x.cleanup_failures[at++] = (SolMirRuntimeLoweredCleanupFailure){
            .state = SOL_MIR_RUNTIME_LOWERED_PRESENT, .kind = SOL_MIR_RUNTIME_LOWERED_CLEANUP_DROP_PATH,
            .record = i, .drop_path = i, .drop_root = q->root, .drop_place = q->place,
            .drop_holes = q->holes, .recipe = q->recipe, .drop_liveness = q->liveness,
        };
    }
    if (at != x.cleanup_failure_count) goto invalid;
    for(size_t i = 0; (i < r->host_abi->requirement_count) && BUILD_TICK(); ++i) {
        const SolMirRuntimeHostRequirement *q = &r->host_abi->requirements[i];
        if (q->call >= r->conventions->call_count || q->operation >= r->host_abi->operation_count)
            goto invalid;
        const SolMirRuntimeHostOperation *operation = &r->host_abi->operations[q->operation];
        size_t grant = 0;
        for (; grant < x.host_grant_count; ++grant)
            if (x.host_grants[grant].state == SOL_MIR_RUNTIME_LOWERED_PRESENT
                && x.host_grants[grant].entry == q->entry && x.host_grants[grant].root == q->root
                && x.host_grants[grant].operation == q->operation) break;
        if (grant == x.host_grant_count) {
            for(grant = 0; (grant < x.host_grant_count) && BUILD_TICK(); ++grant)
                if (x.host_grants[grant].state != SOL_MIR_RUNTIME_LOWERED_PRESENT) break;
            if (grant == x.host_grant_count) goto invalid;
            x.host_grants[grant] = (SolMirRuntimeLoweredHostGrant){
                .state = SOL_MIR_RUNTIME_LOWERED_PRESENT, .entry = q->entry,
                .root = q->root, .operation = q->operation,
            };
        }
        x.host_requirements[i] = (SolMirRuntimeLoweredHostRequirement){
            .state = SOL_MIR_RUNTIME_LOWERED_PRESENT, .requirement = i, .entry = q->entry,
            .root = q->root, .operation = q->operation, .call = q->call, .event = q->event,
            .transition = q->failure_transition, .import_id = operation->import_id,
            .signature = operation->signature,
        };
    }
    size_t incidence = 0;
    for(size_t grant = 0; (grant < x.host_grant_count) && BUILD_TICK(); ++grant) {
        x.host_grants[grant].incidences.offset = incidence;
        x.host_grants[grant].incidences.count = 0;
        for(size_t requirement = 0; (requirement < r->host_abi->requirement_count) && BUILD_TICK(); ++requirement) {
            const SolMirRuntimeHostRequirement *q = &r->host_abi->requirements[requirement];
            if (q->entry != x.host_grants[grant].entry || q->root != x.host_grants[grant].root
                || q->operation != x.host_grants[grant].operation) continue;
            x.host_incidences[incidence++] = (SolMirRuntimeLoweredHostIncidence){
                .state = SOL_MIR_RUNTIME_LOWERED_PRESENT, .requirement = requirement, .grant = grant,
            };
            ++x.host_grants[grant].incidences.count;
        }
    }
    if (incidence != x.host_incidence_count) goto invalid;
    for(size_t i = 0; (i < r->handler_abi->frame_count) && BUILD_TICK(); ++i) {
        const SolMirRuntimeHandlerFramePlan *q = &r->handler_abi->frames[i];
        x.handler_frames[i] = (SolMirRuntimeLoweredHandlerFrame){
            .state = SOL_MIR_RUNTIME_LOWERED_PRESENT, .frame = i, .handler = q->handler,
            .parent = q->parent, .source_binding = q->source_binding,
            .source_operation = q->source_operation, .source_signature = q->source_signature,
            .root_match = q->root_match, .root = q->authority_root, .effects = q->effects,
            .provider_binding = q->provider_binding, .provider_place = q->provider_place,
            .provider_recipe = q->provider_recipe, .provider_access = q->provider_access,
            .provider = q->provider_internal, .signature = q->provider_signature,
            .enter = q->enter_marker,
        };
    }
    for(size_t i = 0; (i < r->handler_abi->exit_marker_count) && BUILD_TICK(); ++i) {
        const SolMirRuntimeHandlerExitMarker *q = &r->handler_abi->exit_markers[i];
        x.handler_markers[i] = (SolMirRuntimeLoweredHandlerMarker){
            .state = SOL_MIR_RUNTIME_LOWERED_PRESENT, .frame = q->frame, .marker = q->instruction,
            .image = q->image, .block = q->block,
        };
    }
    for(size_t i = 0; (i < r->handler_abi->cleanup_exit_count) && BUILD_TICK(); ++i) {
        const SolMirRuntimeHandlerCleanupExit *q = &r->handler_abi->cleanup_exits[i];
        x.handler_exits[i] = (SolMirRuntimeLoweredHandlerExit){
            .state = SOL_MIR_RUNTIME_LOWERED_PRESENT, .frame = q->frame,
            .action = q->action, .transition = q->transition, .alternative_one_pop = true,
        };
    }
    /* Admit the exact remaining transfer and both seals before any persistent
     * allocation.  Reservation checks capacity only; every operation below
     * still consumes its own tick. */
    size_t transfer_work, final_build_work = build_meter.used;
    if (!persistent_work(&x, &transfer_work) || !add(&final_build_work, transfer_work)
        || final_build_work > x.limits.max_build_work) goto exhausted;
    x.usage.build_work=final_build_work;
    if (!fit(&x)) goto exhausted;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    last_build_work.draft_work=build_meter.used;
    last_build_work.reserved_persistent_work=transfer_work;
#endif
    SolMirRuntimeLoweredProgram persistent=x;
#define CLEAR(member,type,singular) persistent.member=NULL;
    SOL_MIR_RUNTIME_LOWERED_TABLES(CLEAR)
#undef CLEAR
    if (!copy_tables(&persistent,&x,&build_meter)) {
        sol_mir_runtime_lowered_program_free(&persistent);
        goto allocation;
    }
    free(coverage); coverage=NULL; free(grant_dedup); grant_dedup=NULL;
    sol_mir_runtime_lowered_program_free(&x);
    persistent.authentication=sol_mir_runtime_lowered_program_internal_seal(&persistent,
        &build_meter);
    if (!persistent.authentication) { sol_mir_runtime_lowered_program_free(&persistent); goto exhausted; }
    size_t validation_work=0;
    { SolMirRuntimeLoweredProgramBuildOutcome measured=sol_mir_runtime_lowered_program_internal_validate(&persistent,d,true,&validation_work);if(measured!=SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED){sol_mir_runtime_lowered_program_free(&persistent);if(measured==SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_RESOURCE_EXHAUSTED)goto exhausted;goto invalid;} }
    persistent.usage.validation_work=validation_work;if(!fit(&persistent)){sol_mir_runtime_lowered_program_free(&persistent);goto exhausted;}
    persistent.authentication=sol_mir_runtime_lowered_program_internal_seal(&persistent,
        &build_meter);
    if (!persistent.authentication || build_meter.used != final_build_work) {
        sol_mir_runtime_lowered_program_free(&persistent); goto exhausted;
    }
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    last_build_work.actual_work=build_meter.used;
#endif
    { SolMirRuntimeLoweredProgramBuildOutcome verified=sol_mir_runtime_lowered_program_internal_validate(&persistent,d,false,NULL);if(verified==SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_ALLOCATION_FAILED){sol_mir_runtime_lowered_program_free(&persistent);goto allocation;}if(verified!=SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED){sol_mir_runtime_lowered_program_free(&persistent);if(verified==SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_RESOURCE_EXHAUSTED)goto exhausted;goto invalid;} }
    *out=persistent;active_build_meter=previous_build_meter;return SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED;

    unsupported:
    if (build_meter.exhausted) goto exhausted;
    free(coverage);free(grant_dedup);sol_mir_runtime_lowered_program_free(&x);
    active_build_meter=previous_build_meter;
    report(d,"runtime lowered program has an unknown or unsupported executable vocabulary");return SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_UNSUPPORTED;
    invalid:
    if (build_meter.exhausted) goto exhausted;
    free(coverage);free(grant_dedup);sol_mir_runtime_lowered_program_free(&x);
    active_build_meter=previous_build_meter;
    report(d,"runtime lowered program typed relation is malformed");return SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_INVALID_PREDECESSOR;
    exhausted:
    free(coverage);free(grant_dedup);sol_mir_runtime_lowered_program_free(&x);
    active_build_meter=previous_build_meter;
    report(d,"runtime lowered program resource limit exceeded");return SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_RESOURCE_EXHAUSTED;
    allocation:
    if (build_meter.exhausted) goto exhausted;
    free(coverage);free(grant_dedup);sol_mir_runtime_lowered_program_free(&x);
    active_build_meter=previous_build_meter;
    if(d)d->allocation_failed=true;report(d,"runtime lowered program allocation failed");return SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_ALLOCATION_FAILED;}
bool sol_mir_runtime_lowered_program_validate(const SolMirRuntimeLoweredProgram*o,SolDiagnostics*d){return sol_mir_runtime_lowered_program_internal_validate(o,d,false,NULL)==SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED;}

/* Rendering is deliberately a semantic replay, not a byte serialization of
 * the local join rows.  In particular, no key below is made from a row's
 * representation, padding, owner coordinate, or address. */
typedef struct { char *data; size_t count, capacity; bool failed; } LoweredRenderBuffer;
typedef struct { const SolMirRuntimeLoweredProgram *owner; LoweredRenderLine *lines;
    size_t count, capacity; bool failed; } LoweredRenderContext;

static bool lowered_render_add(size_t *left, size_t right) {
    return right <= SIZE_MAX - *left && ((*left += right), true);
}

static void lowered_render_put(LoweredRenderBuffer *buffer, const char *text) {
    size_t length = text ? strlen(text) : 0;
    if (buffer->failed || !text || length > buffer->capacity - buffer->count) {
        buffer->failed = true; return;
    }
    memcpy(buffer->data + buffer->count, text, length); buffer->count += length;
}
static int lowered_render_compare(const void *left, const void *right) {
    return strcmp(((const LoweredRenderLine *)left)->text,
        ((const LoweredRenderLine *)right)->text);
}
static const char *lowered_state_name(SolMirRuntimeLoweredState state) {
    switch (state) {
    case SOL_MIR_RUNTIME_LOWERED_PRESENT: return "present";
    case SOL_MIR_RUNTIME_LOWERED_NOT_DEMANDED: return "not-demanded";
    case SOL_MIR_RUNTIME_LOWERED_ERASED: return "erased";
    }
    return NULL;
}
static void lowered_render_hex(char out[65], const SolMirLinkageDigest *digest) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof digest->bytes; ++i) {
        out[i * 2] = digits[digest->bytes[i] >> 4];
        out[i * 2 + 1] = digits[digest->bytes[i] & 15u];
    }
    out[64] = '\0';
}
static bool lowered_render_key(char out[65], const char *type, const char *relation) {
    SolMirLinkageSha256 hash;
    SolMirLinkageDigest digest;
    if (!type || !relation) return false;
    sol_mir_linkage_internal_sha256_init(&hash);
    sol_mir_linkage_internal_sha256_write(&hash, "sol.runtime-lowered.key.v2", 26);
    sol_mir_linkage_internal_sha256_write(&hash, type, strlen(type));
    sol_mir_linkage_internal_sha256_write(&hash, relation, strlen(relation));
    if (!sol_mir_linkage_internal_sha256_finish(&hash, &digest)) return false;
    lowered_render_hex(out, &digest); return true;
}
typedef struct { SolMirLinkageSha256 hash; } LoweredRenderList;
static bool lowered_render_hash_size(SolMirLinkageSha256 *hash, size_t value) {
    char text[32];
    int n = snprintf(text, sizeof text, "%zu", value);
    if (n < 0 || (size_t)n >= sizeof text) return false;
    sol_mir_linkage_internal_sha256_write(hash, text, (size_t)n);
    sol_mir_linkage_internal_sha256_write(hash, ":", 1);
    return true;
}
static bool lowered_render_list_begin(LoweredRenderList *list, const char *domain,
    size_t count) {
    if (!list || !domain) return false;
    sol_mir_linkage_internal_sha256_init(&list->hash);
    sol_mir_linkage_internal_sha256_write(&list->hash, "sol.runtime-lowered.list.v1", 27);
    if (!lowered_render_hash_size(&list->hash, strlen(domain))) return false;
    sol_mir_linkage_internal_sha256_write(&list->hash, domain, strlen(domain));
    return lowered_render_hash_size(&list->hash, count);
}
static bool lowered_render_list_append(LoweredRenderList *list, const char *key) {
    if (!list || !key || !lowered_render_hash_size(&list->hash, strlen(key))) return false;
    sol_mir_linkage_internal_sha256_write(&list->hash, key, strlen(key));
    return true;
}
static bool lowered_render_list_finish(LoweredRenderList *list, char out[65]) {
    SolMirLinkageDigest digest;
    if (!list || !sol_mir_linkage_internal_sha256_finish(&list->hash, &digest)) return false;
    lowered_render_hex(out, &digest); return true;
}
static bool lowered_render_digest_key(char out[65], const SolMirLinkageDigest *digest) {
    if (!digest) return false; lowered_render_hex(out, digest); return true;
}
static bool lowered_render_image_key(const LoweredRenderContext *context, size_t image,
    char out[65]) {
    const SolMirLinkage *linkage = &context->owner->conventions->concrete->linkage;
    for (size_t i = 0; i < linkage->callable_count; ++i)
        if (linkage->callables[i].instance == image)
            return lowered_render_digest_key(out, &linkage->callables[i].instance_key);
    return false;
}
static bool lowered_render_recipe_key(const LoweredRenderContext *context, size_t recipe,
    char out[65]) {
    const SolMirLinkage *linkage = &context->owner->conventions->concrete->linkage;
    for (size_t i = 0; i < linkage->runtime_requirement_count; ++i)
        if (linkage->runtime_requirements[i].recipe == recipe)
            return lowered_render_digest_key(out, &linkage->runtime_requirements[i].recipe_key);
    SolMirLinkageDigest digest;
    return sol_mir_linkage_internal_recipe_key(linkage, recipe, &digest, NULL)
        && lowered_render_digest_key(out, &digest);
}
static bool lowered_render_host_key(const LoweredRenderContext *context, size_t host,
    char out[65]) {
    const SolMirLinkage *linkage = &context->owner->conventions->concrete->linkage;
    return host < linkage->host_requirement_count
        && lowered_render_digest_key(out, &linkage->host_requirements[host].requirement_key);
}
static bool lowered_render_table_key(const LoweredRenderContext *context, size_t table,
    char out[65]) {
    const SolMirLinkage *linkage = &context->owner->conventions->concrete->linkage;
    return table < linkage->table_entry_count
        && lowered_render_digest_key(out, &linkage->table_entries[table].identity);
}
static bool lowered_render_import_key(const LoweredRenderContext *context, size_t import_id,
    char out[65]) {
    const SolMirRuntimeConventions *conventions = context->owner->conventions;
    return import_id < conventions->import_count
        && lowered_render_digest_key(out, &conventions->imports[import_id].identity);
}
static bool lowered_render_signature_key(const LoweredRenderContext *context, size_t signature,
    char out[65]) {
    const SolMirRuntimeConventions *c = context->owner->conventions;
    char parent[65], detail[128];
    if (signature >= c->signature_count) return false;
    const SolMirRuntimeSignature *s = &c->signatures[signature];
    if (s->origin == SOL_MIR_RUNTIME_SIGNATURE_INTERNAL) {
        const SolMirLinkage *l = &c->concrete->linkage;
        if (s->internal >= l->callable_count || !lowered_render_digest_key(parent,
                &l->callables[s->internal].instance_key)) return false;
        (void)snprintf(detail, sizeof detail, "internal:%s", parent);
    } else if (s->origin == SOL_MIR_RUNTIME_SIGNATURE_HOST) {
        if (!lowered_render_host_key(context, s->host, parent)) return false;
        (void)snprintf(detail, sizeof detail, "host:%s", parent);
    } else if (s->origin == SOL_MIR_RUNTIME_SIGNATURE_FUNCTION_RECIPE) {
        if (!lowered_render_recipe_key(context, s->function_recipe, parent)) return false;
        (void)snprintf(detail, sizeof detail, "function-recipe:%s", parent);
    } else return false;
    return lowered_render_key(out, "signature", detail);
}
static bool lowered_render_image_coordinate(const LoweredRenderContext *context, size_t image,
    size_t block, size_t instruction, const char *kind, char out[65]) {
    const SolMirMaterialization *m = &context->owner->conventions->concrete->materialization;
    char image_key[65], detail[192];
    if (image >= m->image_count || !lowered_render_image_key(context, image, image_key)
        || block < m->images[image].blocks.offset
        || block - m->images[image].blocks.offset >= m->images[image].blocks.count) return false;
    size_t b = block - m->images[image].blocks.offset;
    if (instruction == SOL_MIR_RUNTIME_LOWERED_NONE)
        (void)snprintf(detail, sizeof detail, "%s:image=%s:B=%zu", kind, image_key, b);
    else {
        if (instruction < m->images[image].instructions.offset
            || instruction - m->images[image].instructions.offset >= m->images[image].instructions.count)
            return false;
        (void)snprintf(detail, sizeof detail, "%s:image=%s:B=%zu:N=%zu", kind, image_key,
            b, instruction - m->images[image].instructions.offset);
    }
    return lowered_render_key(out, kind, detail);
}
static bool lowered_render_predicate_body_key(const LoweredRenderContext *context, size_t body,
    char out[65]) {
    const SolMirOperations *o = &context->owner->conventions->concrete->operations;
    const SolMirPlan *plan = &context->owner->conventions->concrete->plan;
    char owner[65], context_key[65], detail[256]; SolMirLinkageDigest digest;
    if (body >= o->predicate_body_count) return false;
    const SolMirPredicateBody *p = &o->predicate_bodies[body];
    if (p->context >= plan->context_count) return false;
    const SolMirPlanContext *pc = &plan->contexts[p->context];
    size_t occurrence = 0;
    for (size_t i=0;i<p->context;++i) {
        const SolMirPlanContext *prior=&plan->contexts[i];
        if (prior->kind==pc->kind && prior->target_kind==pc->target_kind
            && prior->instance==pc->instance && prior->import==pc->import) ++occurrence;
    }
    if (pc->target_kind == SOL_MIR_PLAN_TARGET_INSTANCE) {
        if (!lowered_render_image_key(context, pc->instance, owner)) return false;
    } else if (pc->target_kind == SOL_MIR_PLAN_TARGET_IMPORT) {
        const SolMirLinkage *l=&context->owner->conventions->concrete->linkage;
        if (!sol_mir_linkage_internal_host_key(l, pc->import, &digest, NULL)) {
            /* Imported non-host contracts still have an authenticated typed
             * relative occurrence; no dense import coordinate is serialized. */
            if (!lowered_render_key(owner,"plan-import","authenticated")) return false;
        } else lowered_render_hex(owner,&digest);
    } else return false;
    (void)snprintf(detail,sizeof detail,"target=%s:kind=%u:occurrence=%zu",owner,
        (unsigned)pc->kind,occurrence);
    if (!lowered_render_key(context_key,"plan-context",detail)) return false;
    if (p->owner_kind == SOL_MIR_PREDICATE_OWNER_INSTANCE) {
        if (!lowered_render_image_key(context, p->instance, owner)) return false;
        (void)snprintf(detail, sizeof detail, "instance=%s:context=%s:phase=%u:outcome=%u",
            owner, context_key, (unsigned)p->phase, (unsigned)p->outcome);
    } else {
        if (!sol_mir_linkage_internal_host_key(&context->owner->conventions->concrete->linkage,
                p->import, &digest, NULL))
            return false;
        lowered_render_hex(owner,&digest);
        (void)snprintf(detail, sizeof detail, "import=%s:context=%s:phase=%u:outcome=%u",
            owner, context_key, (unsigned)p->phase, (unsigned)p->outcome);
    }
    return lowered_render_key(out, "predicate-body", detail);
}
static bool lowered_render_predicate_coordinate(const LoweredRenderContext *context, size_t body,
    size_t block, size_t instruction, const char *kind, char out[65]) {
    const SolMirOperations *o = &context->owner->conventions->concrete->operations;
    char body_key[65], detail[192];
    if (body >= o->predicate_body_count || !lowered_render_predicate_body_key(context, body, body_key)
        || block < o->predicate_bodies[body].blocks.offset
        || block - o->predicate_bodies[body].blocks.offset >= o->predicate_bodies[body].blocks.count)
        return false;
    if (instruction == SOL_MIR_RUNTIME_LOWERED_NONE)
        (void)snprintf(detail, sizeof detail, "%s:body=%s:B=%zu", kind, body_key,
            block - o->predicate_bodies[body].blocks.offset);
    else
        (void)snprintf(detail, sizeof detail, "%s:body=%s:B=%zu:N=%zu", kind, body_key,
            block - o->predicate_bodies[body].blocks.offset,
            instruction - o->predicate_blocks[block].instructions.offset);
    return lowered_render_key(out, kind, detail);
}
static bool lowered_render_emit(LoweredRenderContext *context, const char *table,
    SolMirRuntimeLoweredState state, const char *key, const char *relation) {
    const char *state_name = lowered_state_name(state);
    if (!state_name || !key || !relation || context->failed || context->count >= context->capacity)
        return false;
    int n = snprintf(context->lines[context->count].text,
        sizeof context->lines[context->count].text, "%s state=%s key=%s%s\n", table,
        state_name, key, state == SOL_MIR_RUNTIME_LOWERED_PRESENT ? relation : "");
    if (n < 0 || (size_t)n >= sizeof context->lines[context->count].text) return false;
    context->lines[context->count].table = table;
    ++context->count; return true;
}
static bool lowered_render_row_key(char out[65], const char *table, const char *anchor,
    const char *relation) {
    char detail[512];
    int n;
    if (!anchor || !relation || (n=snprintf(detail, sizeof detail, "%s:%s", anchor, relation)) < 0
        || (size_t)n >= sizeof detail)
        return false;
    return lowered_render_key(out, table, detail);
}
static bool lowered_render_type_key(const LoweredRenderContext *c, size_t type, char out[65]) {
    const SolMirMaterialization *m=&c->owner->conventions->concrete->materialization;
    const SolMirRepresentation *r=&c->owner->conventions->concrete->representation;
    /* P2 freezes the same-ID recipe/type relation.  The linkage recipe key is
     * already the complete authenticated structural identity, including
     * generic parameters, fields, result, effects, backing, and capability. */
    return r->recipe_count==m->type_count&&type<m->type_count
        && lowered_render_recipe_key(c,type,out);
}
static bool lowered_render_field_key(const LoweredRenderContext *c, size_t field, char out[65]) {
    const SolIr *ir=c->owner->conventions->concrete->program.ir;char detail[192];
    if(field==SOL_IR_NONE){strcpy(out,"none");return true;}if(!ir||field>=ir->field_count)return false;
    const SolIrField*f=&ir->fields[field];if(f->owner>=ir->definition_count)return false;const SolIrDefinition*d=&ir->definitions[f->owner];
    if(d->fields.offset>ir->field_count||d->fields.count>ir->field_count-d->fields.offset)return false;size_t ordinal=SOL_MIR_RUNTIME_LOWERED_NONE;
    for(size_t i=0;i<d->fields.count;++i)if(d->fields.offset+i==field){ordinal=i;break;}
    if(ordinal==SOL_MIR_RUNTIME_LOWERED_NONE)return false;const SolSemanticId id=d->semantic_id;
    int n=snprintf(detail,sizeof detail,"definition=%016llx%016llx:field-ordinal=%zu",(unsigned long long)id.high,(unsigned long long)id.low,ordinal);
    return n>=0&&(size_t)n<sizeof detail&&lowered_render_key(out,"definition-field",detail);
}
static bool lowered_render_projection_key(const LoweredRenderContext*c,size_t projection,size_t base_type,char out[65]) {
    const SolMirMaterialization*m=&c->owner->conventions->concrete->materialization;char base[65],type[65],field[65],detail[320];
    if(projection>=m->projection_count)return false;const SolMirMaterializedProjection*p=&m->projections[projection];
    if(!lowered_render_type_key(c,base_type,base)||!lowered_render_type_key(c,p->type,type)||!lowered_render_field_key(c,p->source_field,field))return false;
    int n=snprintf(detail,sizeof detail,"kind=%u:base-type=%s:result-type=%s:field=%s:tuple-ordinal=%zu",(unsigned)p->kind,base,type,field,p->tuple_ordinal);
    return n>=0&&(size_t)n<sizeof detail&&lowered_render_key(out,"projection",detail);
}
static bool lowered_render_place_key(const LoweredRenderContext *, size_t, char[65]);
static bool lowered_render_field_layout_key(const LoweredRenderContext*c,size_t field_layout,char out[65]) {
    const SolMirLayout*l=c->owner->conventions->concrete->operations.layout;char recipe[65],detail[256];
    if(!l||field_layout>=l->field_count)return false;const SolMirFieldLayout*f=&l->fields[field_layout];
    if(!lowered_render_recipe_key(c,f->owner_recipe,recipe))return false;
    int n=snprintf(detail,sizeof detail,"owner-recipe=%s:field-ordinal=%zu:variant-ordinal=%zu:has-storage=%s:offset=%llu",recipe,f->field,f->variant,f->has_storage?"true":"false",(unsigned long long)f->offset);
    return n>=0&&(size_t)n<sizeof detail&&lowered_render_key(out,"field-layout",detail);
}
static bool lowered_render_operation_access_key(const LoweredRenderContext*c,size_t access,char out[65]) {
    const SolMirOperations*o=&c->owner->conventions->concrete->operations;char place[65],root[65],final[65],steps[65],detail[384];LoweredRenderList list;
    if(access>=o->access_plan_count)return false;const SolMirOperationAccessPlan*p=&o->access_plans[access];
    if(p->steps.offset>o->access_step_count||p->steps.count>o->access_step_count-p->steps.offset||!lowered_render_place_key(c,p->place,place)||!lowered_render_recipe_key(c,p->root_recipe,root)||!lowered_render_recipe_key(c,p->final_recipe,final)||!lowered_render_list_begin(&list,"operation-access-steps",p->steps.count))return false;
    size_t current=o->layout->representation->materialization->places[p->place].root_type;
    for(size_t i=0;i<p->steps.count;++i){const SolMirOperationAccessStep*s=&o->access_steps[p->steps.offset+i];char projection[65],base[65],result[65],field[65],step[65],step_detail[384];
        if(!lowered_render_projection_key(c,s->projection,current,projection)||!lowered_render_recipe_key(c,s->base_recipe,base)||!lowered_render_recipe_key(c,s->result_recipe,result)||!lowered_render_field_layout_key(c,s->field_layout,field))return false;
        int n=snprintf(step_detail,sizeof step_detail,"projection=%s:base-recipe=%s:result-recipe=%s:field-layout=%s:object-offset=%llu",projection,base,result,field,(unsigned long long)s->object_offset);
        if(n<0||(size_t)n>=sizeof step_detail||!lowered_render_key(step,"operation-access-step",step_detail)||!lowered_render_list_append(&list,step))return false;current=o->layout->representation->materialization->projections[s->projection].type;
    }
    if(!lowered_render_list_finish(&list,steps))return false;
    int n=snprintf(detail,sizeof detail,"place=%s:root-recipe=%s:final-recipe=%s:steps-count=%zu:steps=%s",place,root,final,p->steps.count,steps);
    return n>=0&&(size_t)n<sizeof detail&&lowered_render_key(out,"operation-access",detail);
}
static bool lowered_render_place_key(const LoweredRenderContext *c, size_t place, char out[65]) {
    const SolMirMaterialization *m=&c->owner->conventions->concrete->materialization;
    char image[65], root_type[65], final_type[65], projections[65], detail[384];LoweredRenderList list;
    if (place>=m->place_count) return false;
    const SolMirMaterializedPlace *p=&m->places[place];
    if (p->instance>=m->image_count || !lowered_render_image_key(c,p->instance,image)
        || p->local<m->images[p->instance].locals.offset
        || p->local-m->images[p->instance].locals.offset>=m->images[p->instance].locals.count
        ||p->projections.offset>m->projection_count||p->projections.count>m->projection_count-p->projections.offset
        ||!lowered_render_type_key(c,p->root_type,root_type)||!lowered_render_type_key(c,p->final_type,final_type)
        ||!lowered_render_list_begin(&list,"place-projections",p->projections.count))return false;
    size_t current=p->root_type;for(size_t i=0;i<p->projections.count;++i){char projection[65];size_t id=p->projections.offset+i;if(!lowered_render_projection_key(c,id,current,projection)||!lowered_render_list_append(&list,projection))return false;current=m->projections[id].type;}
    if(!lowered_render_list_finish(&list,projections))return false;
    int n=snprintf(detail,sizeof detail,"image=%s:L=%zu:root-type=%s:projection-count=%zu:projections=%s:final-type=%s",image,p->local-m->images[p->instance].locals.offset,root_type,p->projections.count,projections,final_type);
    return n>=0&&(size_t)n<sizeof detail&&lowered_render_key(out,"place",detail);
}
#ifdef SOL_MIR_PLAN_TEST_HOOKS
bool sol_mir_runtime_lowered_program_test_place_key(const SolMirRuntimeLoweredProgram *owner,
    size_t place, char out[65]) {
    if (!owner || !out || !sol_mir_runtime_lowered_program_validate(owner, NULL)) return false;
    return lowered_render_place_key(&(LoweredRenderContext){owner, NULL, 0, 0, false}, place, out);
}
#endif
static bool lowered_render_image_edge_key(const LoweredRenderContext *c, size_t edge,char out[65]) {
    const SolMirRuntimeLoweredProgram *o=c->owner;
    if(edge>=o->image_edge_count)return false;
    const SolMirRuntimeLoweredImageEdge*r=&o->image_edges[edge];char source[65],target[65],detail[192];
    if(!lowered_render_image_coordinate(c,r->image,r->source,SOL_MIR_RUNTIME_LOWERED_NONE,"image-edge-source",source)
        ||!lowered_render_image_coordinate(c,r->image,r->target,SOL_MIR_RUNTIME_LOWERED_NONE,"image-edge-target",target))return false;
    (void)snprintf(detail,sizeof detail,"source=%s:target=%s:E=%zu",source,target,r->ordinal);
    return lowered_render_key(out,"image-edge",detail);
}
static bool lowered_render_predicate_edge_key(const LoweredRenderContext *c,size_t edge,char out[65]) {
    const SolMirRuntimeLoweredProgram *o=c->owner;
    if(edge>=o->predicate_edge_count)return false;
    const SolMirRuntimeLoweredPredicateEdge*r=&o->predicate_edges[edge];char source[65],target[65],detail[192];
    if(!lowered_render_predicate_coordinate(c,r->body,r->source,SOL_MIR_RUNTIME_LOWERED_NONE,"predicate-edge-source",source)
        ||!lowered_render_predicate_coordinate(c,r->body,r->target,SOL_MIR_RUNTIME_LOWERED_NONE,"predicate-edge-target",target))return false;
    (void)snprintf(detail,sizeof detail,"source=%s:target=%s:E=%zu",source,target,r->ordinal);
    return lowered_render_key(out,"predicate-edge",detail);
}
static bool lowered_render_failure_key(const LoweredRenderContext *c,size_t failure,char out[65]) {
    const SolMirRuntimeConventions *q=c->owner->conventions; char owner[65],detail[256];
    if(failure==SOL_MIR_RUNTIME_LOWERED_NONE){strcpy(out,"none");return true;}
    if(failure>=q->failure_site_count)return false;
    const SolMirRuntimeFailureSite*f=&q->failure_sites[failure];
    switch(f->origin_kind){
    case SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_ARITHMETIC:case SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_CALL:
    case SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_PANIC:case SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_NO_MATCH:
    case SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_UNREACHABLE:
        if(!lowered_render_image_coordinate(c,f->owner,f->block,f->instruction,"failure-image",owner))return false;break;
    default:
        if(!lowered_render_predicate_coordinate(c,f->owner,f->block,f->instruction,"failure-predicate",owner))return false;break;
    }
    (void)snprintf(detail,sizeof detail,"origin=%u:owner=%s:codes=%08x",(unsigned)f->origin_kind,owner,f->allowed_codes);
    return lowered_render_key(out,"failure-site",detail);
}
static bool lowered_render_semantic_key(const LoweredRenderContext *, size_t, char out[65]);
static bool lowered_render_cleanup_event_key(const LoweredRenderContext*c,size_t event,char out[65]){
    const SolMirRuntimeCleanup *q=c->owner->cleanup;char owner[65],semantic[65],detail[512];
    if(event==SOL_MIR_RUNTIME_LOWERED_NONE){strcpy(out,"none");return true;}
    if(event>=q->event_count)return false;const SolMirRuntimeCleanupEvent*e=&q->events[event];
    if(e->kind==SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION)
        {if(!lowered_render_image_coordinate(c,e->owner,e->block,e->operation,"cleanup-image-instruction",owner))return false;}
    else if(e->kind==SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR)
        {if(!lowered_render_image_coordinate(c,e->owner,e->block,SOL_MIR_RUNTIME_LOWERED_NONE,"cleanup-image-terminator",owner))return false;}
    else if(e->kind==SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION)
        {if(!lowered_render_predicate_coordinate(c,e->owner,e->block,e->operation,"cleanup-predicate-instruction",owner))return false;}
    else {if(!lowered_render_predicate_coordinate(c,e->owner,e->block,SOL_MIR_RUNTIME_LOWERED_NONE,"cleanup-predicate-terminator",owner))return false;}
    if(e->phase==SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_PROPAGATE_RESIDUAL){const SolMirOperations*ops=&c->owner->conventions->concrete->operations;const SolMirOperationPropagationPlan*plan=NULL;char source[65],success[65],residual[65];for(size_t i=0;i<ops->propagation_count;++i)if(ops->propagations[i].image==e->owner&&ops->propagations[i].block==e->block){if(plan)return false;plan=&ops->propagations[i];}if(!plan||!lowered_render_recipe_key(c,plan->source_recipe,source)||!lowered_render_recipe_key(c,plan->success_recipe,success)||!lowered_render_recipe_key(c,plan->residual_recipe,residual))return false;(void)snprintf(detail,sizeof detail,"source-recipe=%s:success-recipe=%s:residual-recipe=%s:success-tag=%u:source-residual-tag=%u:destination-residual-tag=%u:success-offset=%llu:source-residual-offset=%llu:destination-residual-offset=%llu",source,success,residual,plan->success_tag,plan->source_residual_tag,plan->destination_residual_tag,(unsigned long long)plan->success_field_offset,(unsigned long long)plan->source_residual_field_offset,(unsigned long long)plan->destination_residual_field_offset);if(!lowered_render_key(semantic,"cleanup-propagation-plan",detail))return false;}
    else if(e->phase==SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_INVOKE_CALLABLE){size_t callable=SOL_MIR_RUNTIME_LOWERED_NONE;if(e->semantic_site==SOL_MIR_RUNTIME_NONE)return false;for(size_t i=0;i<c->owner->semantic_plan_count;++i){const SolMirRuntimeLoweredSemanticPlan*plan=&c->owner->semantic_plans[i];if(plan->arena!=SOL_MIR_RUNTIME_LOWERED_SEMANTIC_CALLABLE||plan->producer!=e->semantic_site)continue;if(callable!=SOL_MIR_RUNTIME_LOWERED_NONE)return false;callable=i;}if(callable==SOL_MIR_RUNTIME_LOWERED_NONE||!lowered_render_semantic_key(c,callable,semantic))return false;}
    else if(e->semantic_site==SOL_MIR_RUNTIME_NONE)strcpy(semantic,"none");
    else return false;
    (void)snprintf(detail,sizeof detail,"owner=%s:phase=%u:semantic=%s:producer=%u:origin=%u",owner,(unsigned)e->phase,semantic,(unsigned)e->producer,(unsigned)e->origin);
    return lowered_render_key(out,"cleanup-event",detail);
}
static bool lowered_render_value_key(const LoweredRenderContext*c,
    SolMirRuntimeCallOwnerKind owner_kind, size_t body, SolMirRuntimeValueRef value,char out[65]){
    const SolMirMaterialization*m=&c->owner->conventions->concrete->materialization;
    const SolMirOperations*o=&c->owner->conventions->concrete->operations;char detail[256],parent[65];
    if(value.kind==SOL_MIR_RUNTIME_VALUE_NONE){strcpy(out,"none");return true;}
    if(value.kind==SOL_MIR_RUNTIME_VALUE_MATERIALIZED_PLACE)return lowered_render_place_key(c,value.id,out);
    if(value.kind==SOL_MIR_RUNTIME_VALUE_MATERIALIZED_VALUE){
        if(value.id>=m->value_count)return false;const SolMirMaterializedValue*v=&m->values[value.id];
        size_t image=SOL_MIR_RUNTIME_LOWERED_NONE;for(size_t i=0;i<m->image_count;++i)if(value.id>=m->images[i].values.offset&&value.id-m->images[i].values.offset<m->images[i].values.count){image=i;break;}
        if(image==SOL_MIR_RUNTIME_LOWERED_NONE||!lowered_render_image_coordinate(c,image,v->block,SOL_MIR_RUNTIME_LOWERED_NONE,"value-block",parent))return false;
        (void)snprintf(detail,sizeof detail,"block=%s:V=%zu:kind=%u",parent,value.id-m->images[image].values.offset,(unsigned)v->kind);
    } else if(value.kind==SOL_MIR_RUNTIME_VALUE_MATERIALIZED_TEMPORARY){
        size_t image=SOL_MIR_RUNTIME_LOWERED_NONE;for(size_t i=0;i<m->image_count;++i)if(value.id>=m->images[i].temporaries.offset&&value.id-m->images[i].temporaries.offset<m->images[i].temporaries.count){image=i;break;}
        if(image==SOL_MIR_RUNTIME_LOWERED_NONE||!lowered_render_image_key(c,image,parent))return false;
        (void)snprintf(detail,sizeof detail,"image=%s:T=%zu",parent,value.id-m->images[image].temporaries.offset);
    } else if(value.kind==SOL_MIR_RUNTIME_VALUE_PREDICATE_VALUE
        || value.kind==SOL_MIR_RUNTIME_VALUE_BOUND_RECEIVER) {
        if(owner_kind!=SOL_MIR_RUNTIME_CALL_OWNER_PREDICATE || body>=o->predicate_body_count
            || value.id>=o->predicate_value_count) return false;
        const SolMirPredicateBody *owner=&o->predicate_bodies[body];
        const SolMirPredicateValue *v=&o->predicate_values[value.id]; char recipe[65];
        if(!lowered_render_recipe_key(c,v->recipe,recipe))return false;
        if(v->kind==SOL_MIR_PREDICATE_VALUE_INPUT) {
            if(v->definition<owner->inputs.offset || v->definition-owner->inputs.offset>=owner->inputs.count
                || v->definition>=o->predicate_input_count || !lowered_render_predicate_body_key(c,body,parent))return false;
            const SolMirPredicateInput *input=&o->predicate_inputs[v->definition];
            int n=snprintf(detail,sizeof detail,"body=%s:input=%zu:kind=%u:declared-ordinal=%zu:recipe=%s:access=%u:ref-kind=%u",parent,v->definition-owner->inputs.offset,(unsigned)input->kind,input->ordinal,recipe,(unsigned)input->access,(unsigned)value.kind);
            if(n<0||(size_t)n>=sizeof detail)return false;
        } else if(v->kind==SOL_MIR_PREDICATE_VALUE_BLOCK_PARAMETER) {
            if(v->block>=o->predicate_block_count || o->predicate_blocks[v->block].body!=body
                || v->definition>=o->predicate_blocks[v->block].parameters.count
                || !lowered_render_predicate_coordinate(c,body,v->block,SOL_MIR_RUNTIME_LOWERED_NONE,"predicate-value-block",parent))return false;
            int n=snprintf(detail,sizeof detail,"block=%s:parameter=%zu:recipe=%s:ref-kind=%u",parent,v->definition,recipe,(unsigned)value.kind);
            if(n<0||(size_t)n>=sizeof detail)return false;
        } else if(v->kind==SOL_MIR_PREDICATE_VALUE_INSTRUCTION) {
            if(v->definition>=o->predicate_instruction_count || o->predicate_instructions[v->definition].block!=v->block
                || !lowered_render_predicate_coordinate(c,body,v->block,v->definition,"predicate-value-instruction",parent))return false;
            int n=snprintf(detail,sizeof detail,"instruction=%s:recipe=%s:ref-kind=%u",parent,recipe,(unsigned)value.kind);
            if(n<0||(size_t)n>=sizeof detail)return false;
        } else if(v->kind==SOL_MIR_PREDICATE_VALUE_TERMINATOR) {
            if(v->block>=o->predicate_block_count || o->predicate_blocks[v->block].body!=body
                || !lowered_render_predicate_coordinate(c,body,v->block,SOL_MIR_RUNTIME_LOWERED_NONE,"predicate-value-terminator",parent))return false;
            int n=snprintf(detail,sizeof detail,"terminator=%s:recipe=%s:ref-kind=%u",parent,recipe,(unsigned)value.kind);
            if(n<0||(size_t)n>=sizeof detail)return false;
        } else return false;
    } else return false;
    return lowered_render_key(out,"value",detail);
}
static bool lowered_render_call_key(const LoweredRenderContext*c,size_t call,char out[65]){
    const SolMirRuntimeLoweredProgram*o=c->owner;char owner[65],signature[65],target[65],detail[320];
    if(call==SOL_MIR_RUNTIME_LOWERED_NONE){strcpy(out,"none");return true;}if(call>=o->call_count)return false;
    const SolMirRuntimeLoweredCall*r=&o->calls[call];
    if(r->owner_kind==SOL_MIR_RUNTIME_CALL_OWNER_IMAGE){if(!lowered_render_image_coordinate(c,r->image,r->block,SOL_MIR_RUNTIME_LOWERED_NONE,"call-owner",owner))return false;}
    else if(!lowered_render_predicate_coordinate(c,r->body,r->block,SOL_MIR_RUNTIME_LOWERED_NONE,"call-owner",owner))return false;
    if(!lowered_render_signature_key(c,r->signature,signature))return false;
    if(r->target_kind==SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL){if(r->internal>=o->conventions->concrete->linkage.callable_count||!lowered_render_digest_key(target,&o->conventions->concrete->linkage.callables[r->internal].instance_key))return false;}
    else if(r->target_kind==SOL_MIR_RUNTIME_TARGET_DIRECT_HOST){if(!lowered_render_host_key(c,r->host,target))return false;}else if(!lowered_render_table_key(c,r->table,target))return false;
    (void)snprintf(detail,sizeof detail,"owner=%s:kind=%u:target-kind=%u:target=%s:signature=%s",owner,(unsigned)r->call_kind,(unsigned)r->target_kind,target,signature);
    return lowered_render_key(out,"call",detail);
}
static bool lowered_render_demand_key(const LoweredRenderContext*c,size_t demand,char out[65]){
    const SolMirRuntimeLoweredProgram*o=c->owner;char consumer[65],detail[192];size_t ordinal;
#define DEMAND_OWNER(array_value,array_length,slice_field,coordinate) for(size_t i=0;i<(array_length);++i){SolMirRuntimeSlice s=(array_value)[i].slice_field;if(demand>=s.offset&&demand-s.offset<s.count){if(!(coordinate))return false;ordinal=demand-s.offset;(void)snprintf(detail,sizeof detail,"consumer=%s:ordinal=%zu",consumer,ordinal);return lowered_render_key(out,"recipe-demand",detail);}}
    DEMAND_OWNER(o->image_instructions,o->image_instruction_count,demanded_recipes,lowered_render_image_coordinate(c,o->image_instructions[i].image,o->image_instructions[i].block,o->image_instructions[i].instruction,"demand-image",consumer))
    DEMAND_OWNER(o->image_terminators,o->image_terminator_count,demanded_recipes,lowered_render_image_coordinate(c,o->image_terminators[i].image,o->image_terminators[i].block,SOL_MIR_RUNTIME_LOWERED_NONE,"demand-terminator",consumer))
    DEMAND_OWNER(o->predicate_instructions,o->predicate_instruction_count,demanded_recipes,lowered_render_predicate_coordinate(c,o->predicate_instructions[i].body,o->predicate_instructions[i].block,o->predicate_instructions[i].instruction,"demand-predicate",consumer))
    DEMAND_OWNER(o->predicate_terminators,o->predicate_terminator_count,demanded_recipes,lowered_render_predicate_coordinate(c,o->predicate_terminators[i].body,o->predicate_terminators[i].block,SOL_MIR_RUNTIME_LOWERED_NONE,"demand-predicate-terminator",consumer))
#undef DEMAND_OWNER
    return false;
}
static bool lowered_render_demand_list_key(const LoweredRenderContext*c,SolMirRuntimeSlice demands,char out[65]){
    const SolMirRuntimeLoweredProgram*o=c->owner;LoweredRenderList list;
    if(demands.offset>o->recipe_demand_count||demands.count>o->recipe_demand_count-demands.offset||!lowered_render_list_begin(&list,"recipe-demands",demands.count))return false;
    for(size_t i=0;i<demands.count;++i){char key[65];if(!lowered_render_demand_key(c,demands.offset+i,key)||!lowered_render_list_append(&list,key))return false;}
    return lowered_render_list_finish(&list,out);
}
static bool lowered_render_entry_key(const LoweredRenderContext *c, size_t entry, char out[65]) {
    const SolMirRuntimeConventions *q=c->owner->conventions; char callable[65],signature[65],symbol[65],detail[320];
    if(entry>=q->entry_count)return false;const SolMirRuntimeEntry *e=&q->entries[entry];
    if(e->callable>=q->concrete->linkage.callable_count
        || !lowered_render_digest_key(callable,&q->concrete->linkage.callables[e->callable].instance_key)
        || !lowered_render_signature_key(c,e->signature,signature))return false;
    const char *end=memchr(e->symbol.bytes,'\0',sizeof e->symbol.bytes);
    if(!end)return false;
    if(!lowered_render_key(symbol,"entry-export",e->symbol.bytes))return false;
    int n=snprintf(detail,sizeof detail,"callable=%s:signature=%s:export=%s",callable,signature,symbol);
    return n>=0&&(size_t)n<sizeof detail&&lowered_render_key(out,"entry",detail);
}
static bool lowered_render_host_root_key(const LoweredRenderContext*c,size_t root,char out[65]){
    const SolMirRuntimeHostAbi*h=c->owner->host_abi;char entry[65],recipe[65],detail[256];
    if(root>=h->entry_root_count)return false;const SolMirRuntimeHostEntryRoot*r=&h->entry_roots[root];
    if(!lowered_render_entry_key(c,r->entry,entry)||!lowered_render_recipe_key(c,r->recipe,recipe))return false;
    int n=snprintf(detail,sizeof detail,"entry=%s:formal=%zu:recipe=%s",entry,r->formal,recipe);
    return n>=0&&(size_t)n<sizeof detail&&lowered_render_key(out,"host-root",detail);
}
static bool lowered_render_effect_key(const LoweredRenderContext*c,size_t effect,char out[65]){
    const SolMirMaterialization*m=&c->owner->conventions->concrete->materialization;LoweredRenderList list;
    if(effect>=m->effect_row_count||!lowered_render_list_begin(&list,"materialized-effect",m->effect_rows[effect].atoms.count))return false;
    const SolMirPlanSlice atoms=m->effect_rows[effect].atoms;
    if(atoms.offset>m->effect_row_atom_count||atoms.count>m->effect_row_atom_count-atoms.offset)return false;
    for(size_t i=0;i<atoms.count;++i){size_t id=m->effect_row_atoms[atoms.offset+i];char atom[65],detail[256];
        if(id>=m->effect_atom_count)return false;const SolMirMaterializedEffectAtom*a=&m->effect_atoms[id];
        if(a->name.offset>m->effect_name_count||a->name.count>m->effect_name_count-a->name.offset)return false;
        SolMirLinkageSha256 hash;SolMirLinkageDigest digest;sol_mir_linkage_internal_sha256_init(&hash);
        sol_mir_linkage_internal_sha256_write(&hash,"effect-atom",11);
        sol_mir_linkage_internal_sha256_write(&hash,m->effect_names+a->name.offset,a->name.count);
        int n=snprintf(detail,sizeof detail,"authority=%u:ordinal=%zu",(unsigned)a->authority,a->ordinal);
        if(n<0||(size_t)n>=sizeof detail)return false;sol_mir_linkage_internal_sha256_write(&hash,detail,(size_t)n);
        if(!sol_mir_linkage_internal_sha256_finish(&hash,&digest))return false;lowered_render_hex(atom,&digest);
        if(!lowered_render_list_append(&list,atom))return false;
    }
    return lowered_render_list_finish(&list,out);
}
static bool lowered_render_materialized_import_key(const LoweredRenderContext*c,size_t import,char out[65]){
    const SolMirLinkage*l=&c->owner->conventions->concrete->linkage;
    for(size_t i=0;i<l->host_requirement_count;++i)if(l->host_requirements[i].import==import)return lowered_render_digest_key(out,&l->host_requirements[i].requirement_key);
    return false;
}
static bool lowered_render_operation_key(const LoweredRenderContext*c,const SolMirMaterializedOperationKey*op,char out[65]){
    char target[65],root[65],effect[65],detail[320];if(!op)return false;
    if(op->target_kind==SOL_MIR_MATERIALIZED_TARGET_INSTANCE){if(!lowered_render_image_key(c,op->instance,target))return false;}
    else if(op->target_kind==SOL_MIR_MATERIALIZED_TARGET_IMPORT){if(!lowered_render_materialized_import_key(c,op->import,target))return false;}else return false;
    if(!lowered_render_place_key(c,op->root,root)||!lowered_render_effect_key(c,op->effects,effect))return false;
    int n=snprintf(detail,sizeof detail,"target=%s:root=%s:effects=%s",target,root,effect);
    return n>=0&&(size_t)n<sizeof detail&&lowered_render_key(out,"operation",detail);
}
static bool lowered_render_host_operation_key(const LoweredRenderContext*c,size_t operation,char out[65]){
    const SolMirRuntimeHostAbi*h=c->owner->host_abi;char identity[65],host[65],import[65],signature[65],detail[384];
    if(operation>=h->operation_count)return false;const SolMirRuntimeHostOperation*o=&h->operations[operation];
    if(!lowered_render_digest_key(identity,&o->identity)||!lowered_render_host_key(c,o->host,host)||!lowered_render_import_key(c,o->import_id,import)||!lowered_render_signature_key(c,o->signature,signature))return false;
    int n=snprintf(detail,sizeof detail,"identity=%s:host=%s:import=%s:signature=%s",identity,host,import,signature);
    return n>=0&&(size_t)n<sizeof detail&&lowered_render_key(out,"host-operation",detail);
}
static bool lowered_render_host_grant_key(const LoweredRenderContext*c,size_t grant,char out[65]){
    const SolMirRuntimeLoweredProgram*o=c->owner;char entry[65],root[65],operation[65],detail[320];
    if(grant>=o->host_grant_count)return false;const SolMirRuntimeLoweredHostGrant*g=&o->host_grants[grant];
    if(!lowered_render_entry_key(c,g->entry,entry)||!lowered_render_host_root_key(c,g->root,root)||!lowered_render_host_operation_key(c,g->operation,operation))return false;
    int n=snprintf(detail,sizeof detail,"entry=%s:root=%s:operation=%s",entry,root,operation);
    return n>=0&&(size_t)n<sizeof detail&&lowered_render_key(out,"host-grant",detail);
}
static bool lowered_render_cleanup_action_key(const LoweredRenderContext*c,size_t action,char out[65]){
    const SolMirRuntimeCleanup*q=c->owner->cleanup;char event[65],detail[192];
    if(action==SOL_MIR_RUNTIME_LOWERED_NONE){strcpy(out,"none");return true;}if(action>=q->action_count)return false;
    for(size_t i=0;i<q->event_count;++i)if(action>=q->events[i].actions.offset&&action-q->events[i].actions.offset<q->events[i].actions.count){if(!lowered_render_cleanup_event_key(c,i,event))return false;int n=snprintf(detail,sizeof detail,"event=%s:occurrence=%zu",event,action-q->events[i].actions.offset);return n>=0&&(size_t)n<sizeof detail&&lowered_render_key(out,"cleanup-action",detail);}return false;
}
static bool lowered_render_cleanup_transition_key(const LoweredRenderContext*c,size_t transition,char out[65]){
    const SolMirRuntimeCleanup*q=c->owner->cleanup;char event[65],detail[192];
    if(transition==SOL_MIR_RUNTIME_LOWERED_NONE){strcpy(out,"none");return true;}if(transition>=q->transition_count)return false;const SolMirRuntimeCleanupTransition*t=&q->transitions[transition];
    if(!lowered_render_cleanup_event_key(c,t->event,event))return false;size_t occurrence=0;const SolMirRuntimeSlice slice=q->events[t->event].transitions;
    if(transition<slice.offset||transition-slice.offset>=slice.count)return false;occurrence=transition-slice.offset;
    int n=snprintf(detail,sizeof detail,"event=%s:occurrence=%zu",event,occurrence);
    return n>=0&&(size_t)n<sizeof detail&&lowered_render_key(out,"cleanup-transition",detail);
}
static bool lowered_render_cleanup_supplemental_key(const LoweredRenderContext*c,size_t site,char out[65]){
    const SolMirRuntimeCleanup*q=c->owner->cleanup;char event[65],detail[192];
    if(site==SOL_MIR_RUNTIME_LOWERED_NONE){strcpy(out,"none");return true;}if(site>=q->supplemental_site_count)return false;
    const SolMirRuntimeCleanupSupplementalSite*s=&q->supplemental_sites[site];if(!lowered_render_cleanup_event_key(c,s->event,event))return false;
    size_t occurrence=0;for(size_t i=0;i<site;++i)if(q->supplemental_sites[i].event==s->event)++occurrence;
    int n=snprintf(detail,sizeof detail,"event=%s:occurrence=%zu:codes=%08x",event,occurrence,s->allowed_codes);
    return n>=0&&(size_t)n<sizeof detail&&lowered_render_key(out,"cleanup-supplemental",detail);
}
static bool lowered_render_cleanup_drop_path_key(const LoweredRenderContext*c,size_t path,char out[65]){
    const SolMirRuntimeCleanup*q=c->owner->cleanup;char root[65],place[65],recipe[65],detail[320];
    if(path==SOL_MIR_RUNTIME_LOWERED_NONE){strcpy(out,"none");return true;}if(path>=q->drop_path_count)return false;const SolMirRuntimeCleanupDropPath*p=&q->drop_paths[path];
    if(!lowered_render_place_key(c,p->root,root)||!lowered_render_place_key(c,p->place,place)||!lowered_render_recipe_key(c,p->recipe,recipe))return false;
    int n=snprintf(detail,sizeof detail,"root=%s:place=%s:recipe=%s:holes-count=%zu:liveness=%u",root,place,recipe,p->holes.count,(unsigned)p->liveness);
    return n>=0&&(size_t)n<sizeof detail&&lowered_render_key(out,"cleanup-drop-path",detail);
}
static bool lowered_render_handler_frame_key(const LoweredRenderContext*c,size_t frame,char out[65]){
    const SolMirRuntimeLoweredProgram*o=c->owner;const SolMirMaterialization*m=&o->conventions->concrete->materialization;char image[65],detail[192];
    if(frame==SOL_MIR_RUNTIME_LOWERED_NONE){strcpy(out,"none");return true;}if(frame>=o->handler_frame_count)return false;const SolMirRuntimeLoweredHandlerFrame*r=&o->handler_frames[frame];
    if(r->handler>=m->handler_count)return false;const SolMirMaterializedHandler*h=&m->handlers[r->handler];
    if(h->parent>=m->image_count||r->handler<m->images[h->parent].handlers.offset||r->handler-m->images[h->parent].handlers.offset>=m->images[h->parent].handlers.count||!lowered_render_image_key(c,h->parent,image))return false;
    int n=snprintf(detail,sizeof detail,"parent-image=%s:handler-occurrence=%zu",image,r->handler-m->images[h->parent].handlers.offset);
    return n>=0&&(size_t)n<sizeof detail&&lowered_render_key(out,"handler-frame",detail);
}
static bool lowered_render_cleanup_action_target_key(const LoweredRenderContext*c,size_t action,char out[65]){
    const SolMirRuntimeCleanup*q=c->owner->cleanup;const SolMirMaterialization*m=&c->owner->conventions->concrete->materialization;const SolMirOperations*o=&c->owner->conventions->concrete->operations;size_t event=SOL_MIR_RUNTIME_LOWERED_NONE;
    if(action>=q->action_count)return false;for(size_t i=0;i<q->event_count;++i)if(action>=q->events[i].actions.offset&&action-q->events[i].actions.offset<q->events[i].actions.count){event=i;break;}
    if(event==SOL_MIR_RUNTIME_LOWERED_NONE)return false;const SolMirRuntimeCleanupAction*a=&q->actions[action];const SolMirRuntimeCleanupEvent*e=&q->events[event];char image[65],anchor[65],detail[320];
    switch(a->kind){
    case SOL_MIR_RUNTIME_CLEANUP_ACTION_WRITEBACK:
        return lowered_render_place_key(c,a->target,out);
    case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE:case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER:
        if(a->drop_path>=q->drop_path_count)return false;return lowered_render_place_key(c,q->drop_paths[a->drop_path].place,out);
    case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_TEMPORARY:
        for(size_t i=0;i<m->image_count;++i)if(a->target>=m->images[i].temporaries.offset&&a->target-m->images[i].temporaries.offset<m->images[i].temporaries.count){if(!lowered_render_image_key(c,i,image))return false;int n=snprintf(detail,sizeof detail,"image=%s:temporary=%zu",image,a->target-m->images[i].temporaries.offset);return n>=0&&(size_t)n<sizeof detail&&lowered_render_key(out,"temporary",detail);}return false;
    case SOL_MIR_RUNTIME_CLEANUP_ACTION_CHECK_CONTRACT:
        if(!lowered_render_cleanup_event_key(c,event,anchor))return false;return lowered_render_key(out,"contract-obligation",anchor);
    case SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_SCOPE:
        if(a->target>=m->instruction_count|| (e->kind!=SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION&&e->kind!=SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR))return false;return lowered_render_image_coordinate(c,e->owner,m->instructions[a->target].block,a->target,"scope-enter",out);
    case SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_REGION:
        if(e->owner>=m->image_count)return false;for(size_t i=m->images[e->owner].instructions.offset;i<m->images[e->owner].instructions.offset+m->images[e->owner].instructions.count;++i)if(m->instructions[i].scope_kind==SOL_MIR_SCOPE_REGION&&m->instructions[i].scope_source==a->target)return lowered_render_image_coordinate(c,e->owner,m->instructions[i].block,i,"region-enter",out);return false;
    case SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_HANDLER:
        for(size_t i=0;i<c->owner->handler_frame_count;++i)if(c->owner->handler_frames[i].handler==a->target)return lowered_render_handler_frame_key(c,i,out);return false;
    case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT:
        if(a->target>=o->snapshot_count)return false;{const SolMirOperationSnapshotPlan*s=&o->snapshots[a->target];char instruction[65],recipe[65];if(s->instruction>=m->instruction_count||!lowered_render_image_coordinate(c,s->image,m->instructions[s->instruction].block,s->instruction,"snapshot",instruction)||!lowered_render_recipe_key(c,s->recipe,recipe))return false;int n=snprintf(detail,sizeof detail,"instruction=%s:slot=%zu:recipe=%s",instruction,s->slot,recipe);return n>=0&&(size_t)n<sizeof detail&&lowered_render_key(out,"snapshot",detail);}
    case SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE:
        if(e->inherited_failure_site!=SOL_MIR_RUNTIME_LOWERED_NONE)return lowered_render_failure_key(c,e->inherited_failure_site,out);return lowered_render_cleanup_supplemental_key(c,e->supplemental_site,out);
    }
    return false;
}
static bool lowered_render_semantic_producer_key(const LoweredRenderContext*c,size_t plan,char out[65]){
    const SolMirRuntimeLoweredProgram*o=c->owner;const SolMirMaterialization*m=&o->conventions->concrete->materialization;
    char producer[65];if(plan>=o->semantic_plan_count)return false;const SolMirRuntimeLoweredSemanticPlan*r=&o->semantic_plans[plan];
    if(r->arena==SOL_MIR_RUNTIME_LOWERED_SEMANTIC_CALLABLE){
        if(r->producer>=m->semantic_site_count)return false;const SolMirMaterializedSemanticSite*s=&m->semantic_sites[r->producer];
        if(s->producer_kind==SOL_MIR_MATERIALIZED_PRODUCER_INSTRUCTION){if(s->instruction>=m->instruction_count||!lowered_render_image_coordinate(c,o->image_instructions[s->instruction].image,s->block,s->instruction,"semantic-callable",producer))return false;}
        else if(s->producer_kind==SOL_MIR_MATERIALIZED_PRODUCER_TERMINATOR){if(s->block>=m->block_count||!lowered_render_image_coordinate(c,o->image_terminators[s->block].image,s->block,SOL_MIR_RUNTIME_LOWERED_NONE,"semantic-callable",producer))return false;}
        else if(s->producer_kind==SOL_MIR_MATERIALIZED_PRODUCER_PREDICATE){if(!lowered_render_predicate_body_key(c,s->parent,producer))return false;}
        else return false;
    } else if(r->producer_kind==SOL_MIR_MATERIALIZED_PRODUCER_ROOT){if(!lowered_render_place_key(c,r->producer,producer))return false;}
    else if(r->producer_kind==SOL_MIR_MATERIALIZED_PRODUCER_INSTRUCTION){if(r->producer>=m->instruction_count||!lowered_render_image_coordinate(c,o->image_instructions[r->producer].image,m->instructions[r->producer].block,r->producer,"semantic-producer",producer))return false;}
    else if(r->producer_kind==SOL_MIR_MATERIALIZED_PRODUCER_TERMINATOR){if(r->producer>=m->block_count||!lowered_render_image_coordinate(c,o->image_terminators[r->producer].image,r->producer,SOL_MIR_RUNTIME_LOWERED_NONE,"semantic-producer",producer))return false;}
    else if(r->producer_kind==SOL_MIR_MATERIALIZED_PRODUCER_PREDICATE){if(!lowered_render_predicate_body_key(c,r->producer,producer))return false;}
    else if(r->producer_kind==SOL_MIR_MATERIALIZED_PRODUCER_HANDLER){size_t frame=SOL_MIR_RUNTIME_LOWERED_NONE;for(size_t i=0;i<o->handler_frame_count;++i)if(o->handler_frames[i].handler==r->producer){frame=i;break;}if(frame==SOL_MIR_RUNTIME_LOWERED_NONE||!lowered_render_handler_frame_key(c,frame,producer))return false;}
    else return false;
    memcpy(out,producer,sizeof producer);return true;
}
static bool lowered_render_semantic_key(const LoweredRenderContext*c,size_t plan,char out[65]){
    const SolMirRuntimeLoweredProgram*o=c->owner;char producer[65],prior[65],detail[256];size_t occurrence=0;
    if(plan>=o->semantic_plan_count||!lowered_render_semantic_producer_key(c,plan,producer))return false;
    const SolMirRuntimeLoweredSemanticPlan*r=&o->semantic_plans[plan];
    for(size_t i=0;i<plan;++i)if(o->semantic_plans[i].arena==r->arena&&lowered_render_semantic_producer_key(c,i,prior)&&!strcmp(prior,producer))++occurrence;
    int n=snprintf(detail,sizeof detail,"arena=%u:producer=%s:occurrence=%zu",(unsigned)r->arena,producer,occurrence);
    return n>=0&&(size_t)n<sizeof detail&&lowered_render_key(out,"semantic-plan",detail);
}
static bool lowered_render_host_requirement_key(const LoweredRenderContext*c,size_t requirement,char out[65]){
    const SolMirRuntimeLoweredProgram*o=c->owner;char grant[65],call[65],event[65],transition[65],detail[384];size_t grant_index=SOL_MIR_RUNTIME_LOWERED_NONE;
    if(requirement>=o->host_requirement_count)return false;const SolMirRuntimeLoweredHostRequirement*r=&o->host_requirements[requirement];
    for(size_t i=0;i<o->host_grant_count;++i)
        if(o->host_grants[i].entry==r->entry&&o->host_grants[i].root==r->root
            &&o->host_grants[i].operation==r->operation){grant_index=i;break;}
    if(grant_index==SOL_MIR_RUNTIME_LOWERED_NONE||!lowered_render_host_grant_key(c,grant_index,grant)||!lowered_render_call_key(c,r->call,call)||!lowered_render_cleanup_event_key(c,r->event,event)||!lowered_render_cleanup_transition_key(c,r->transition,transition))return false;
    int n=snprintf(detail,sizeof detail,"grant=%s:call=%s:event=%s:transition=%s",grant,call,event,transition);
    return n>=0&&(size_t)n<sizeof detail&&lowered_render_key(out,"host-requirement",detail);
}
static bool lowered_render_host_incidence_key(const LoweredRenderContext*c,size_t incidence,char out[65]){
    const SolMirRuntimeLoweredProgram*o=c->owner;char grant[65],requirement[65],detail[192];
    if(incidence>=o->host_incidence_count)return false;const SolMirRuntimeLoweredHostIncidence*r=&o->host_incidences[incidence];
    if(!lowered_render_host_grant_key(c,r->grant,grant)||!lowered_render_host_requirement_key(c,r->requirement,requirement))return false;
    int n=snprintf(detail,sizeof detail,"grant=%s:requirement=%s",grant,requirement);
    return n>=0&&(size_t)n<sizeof detail&&lowered_render_key(out,"host-incidence",detail);
}
static bool lowered_render_rows(LoweredRenderContext *c) {
    const SolMirRuntimeLoweredProgram *o = c->owner;
    char key[65], a[65], b[65], relation[1200];
    const char *none = "none";
#define LOWERED_RELATION(...) do { int n__=snprintf(relation,sizeof relation,__VA_ARGS__); if(n__<0||(size_t)n__>=sizeof relation)return false; } while(0)
    /* Image rows are authenticated by linkage instance identity and B/N/E
     * coordinates relative to that image, never by materialized arena IDs. */
    for (size_t i=0;i<o->image_instruction_count;++i) { const SolMirRuntimeLoweredImageInstruction*r=&o->image_instructions[i];char plan[65],event[65],failure[65],demands[65]; if(!lowered_render_image_coordinate(c,r->image,r->block,r->instruction,"image-instruction",a)||!lowered_render_row_key(key,"image_instructions",a,"instruction")||!lowered_render_cleanup_event_key(c,r->cleanup_event,event)||!lowered_render_failure_key(c,r->failure_site,failure)||!lowered_render_demand_list_key(c,r->demanded_recipes,demands))return false; if(r->plan==SOL_MIR_RUNTIME_LOWERED_NONE)strcpy(plan,none);else if(!lowered_render_semantic_key(c,r->plan,plan))return false; LOWERED_RELATION(" kind=%u class=%u family=%u facilities=%08x semantic-plan=%s recipe-demands-count=%zu recipe-demands-digest=%s cleanup-event=%s failure-site=%s",(unsigned)r->kind,(unsigned)r->runtime_class,(unsigned)r->plan_family,r->facilities,plan,r->demanded_recipes.count,demands,event,failure); if(!lowered_render_emit(c,"image_instructions",r->state,key,relation))return false; }
    for (size_t i=0;i<o->image_block_count;++i) { const SolMirRuntimeLoweredImageBlock*r=&o->image_blocks[i]; if(!lowered_render_image_coordinate(c,r->image,r->block,SOL_MIR_RUNTIME_LOWERED_NONE,"image-block",a)||!lowered_render_row_key(key,"image_blocks",a,"block"))return false; LOWERED_RELATION(" incoming-ordered=%zu outgoing-ordered=%zu",r->incoming_edges.count,r->outgoing_edges.count);if(!lowered_render_emit(c,"image_blocks",r->state,key,relation))return false; }
    for(size_t i=0;i<o->image_edge_count;++i){const SolMirRuntimeLoweredImageEdge*r=&o->image_edges[i];if(!lowered_render_image_coordinate(c,r->image,r->source,SOL_MIR_RUNTIME_LOWERED_NONE,"image-edge-source",a)||!lowered_render_image_coordinate(c,r->image,r->target,SOL_MIR_RUNTIME_LOWERED_NONE,"image-edge-target",b)||!lowered_render_row_key(key,"image_edges",a,b))return false;LOWERED_RELATION(" source=%s target=%s E=%zu",a,b,r->ordinal);if(!lowered_render_emit(c,"image_edges",r->state,key,relation))return false;}
    for(size_t i=0;i<o->image_incoming_edge_count;++i){const SolMirRuntimeLoweredImageBlockEdge*r=&o->image_incoming_edges[i];if(!lowered_render_image_coordinate(c,r->image,r->block,SOL_MIR_RUNTIME_LOWERED_NONE,"image-block",a)||!lowered_render_image_edge_key(c,r->edge,b)||!lowered_render_row_key(key,"image_incoming_edges",a,b))return false;LOWERED_RELATION(" block=%s edge=%s ordered-occurrence=%zu",a,b,r->ordinal);if(!lowered_render_emit(c,"image_incoming_edges",r->state,key,relation))return false;}
    for(size_t i=0;i<o->image_outgoing_edge_count;++i){const SolMirRuntimeLoweredImageBlockEdge*r=&o->image_outgoing_edges[i];if(!lowered_render_image_coordinate(c,r->image,r->block,SOL_MIR_RUNTIME_LOWERED_NONE,"image-block",a)||!lowered_render_image_edge_key(c,r->edge,b)||!lowered_render_row_key(key,"image_outgoing_edges",a,b))return false;LOWERED_RELATION(" block=%s edge=%s ordered-occurrence=%zu",a,b,r->ordinal);if(!lowered_render_emit(c,"image_outgoing_edges",r->state,key,relation))return false;}
    for(size_t i=0;i<o->image_terminator_count;++i){
        const SolMirRuntimeLoweredImageTerminator*r=&o->image_terminators[i];char call[65],event[65],failure[65],pre_event[65],pre_site[65],demands[65],plan[65];
        if(!lowered_render_image_coordinate(c,r->image,r->block,SOL_MIR_RUNTIME_LOWERED_NONE,"image-terminator",a)||!lowered_render_row_key(key,"image_terminators",a,"terminator")||!lowered_render_call_key(c,r->call,call)||!lowered_render_cleanup_event_key(c,r->cleanup_event,event)||!lowered_render_failure_key(c,r->failure_site,failure)||!lowered_render_cleanup_event_key(c,r->pre_operation_cleanup_event,pre_event)||!lowered_render_cleanup_supplemental_key(c,r->pre_operation_supplemental_site,pre_site)||!lowered_render_demand_list_key(c,r->demanded_recipes,demands))return false;
        if(r->plan==SOL_MIR_RUNTIME_LOWERED_NONE)strcpy(plan,none);else if(!lowered_render_semantic_key(c,r->plan,plan))return false;
        LOWERED_RELATION(" kind=%u class=%u family=%u facilities=%08x semantic-plan=%s recipe-demands-count=%zu recipe-demands-digest=%s call=%s cleanup-event=%s failure-site=%s pre-operation-cleanup-event=%s pre-operation-supplemental-site=%s",(unsigned)r->kind,(unsigned)r->runtime_class,(unsigned)r->plan_family,r->facilities,plan,r->demanded_recipes.count,demands,call,event,failure,pre_event,pre_site);
        if(!lowered_render_emit(c,"image_terminators",r->state,key,relation))return false;
    }
    for(size_t i=0;i<o->predicate_body_count;++i){const SolMirRuntimeLoweredPredicateBody*r=&o->predicate_bodies[i];if(!lowered_render_predicate_body_key(c,r->body,a)||!lowered_render_row_key(key,"predicate_bodies",a,"body"))return false;LOWERED_RELATION(" owner=%s blocks-ordered=%zu entry-relative=%zu output-recipe=%s refinement-self=%s",r->owner_kind==SOL_MIR_PREDICATE_OWNER_INSTANCE?"instance":"import",r->blocks.count,r->entry-r->blocks.offset,lowered_render_recipe_key(c,r->output_recipe,b)?b:none,r->refinement_self_recipe==SOL_MIR_RECIPE_NONE?none:(lowered_render_recipe_key(c,r->refinement_self_recipe,b)?b:none));if(!lowered_render_emit(c,"predicate_bodies",r->state,key,relation))return false;}
    for(size_t i=0;i<o->predicate_instruction_count;++i){
        const SolMirRuntimeLoweredPredicateInstruction*r=&o->predicate_instructions[i];char event[65],failure[65],plan[65],demands[65];
        if(!lowered_render_predicate_coordinate(c,r->body,r->block,r->instruction,"predicate-instruction",a)||!lowered_render_row_key(key,"predicate_instructions",a,"instruction")||!lowered_render_cleanup_event_key(c,r->cleanup_event,event)||!lowered_render_failure_key(c,r->failure_site,failure)||!lowered_render_demand_list_key(c,r->demanded_recipes,demands))return false;
        if(r->plan==SOL_MIR_RUNTIME_LOWERED_NONE)strcpy(plan,none);else if(!lowered_render_semantic_key(c,r->plan,plan))return false;
        LOWERED_RELATION(" kind=%u class=%u family=%u facilities=%08x semantic-plan=%s recipe-demands-count=%zu recipe-demands-digest=%s cleanup-event=%s failure-site=%s",(unsigned)r->kind,(unsigned)r->runtime_class,(unsigned)r->plan_family,r->facilities,plan,r->demanded_recipes.count,demands,event,failure);
        if(!lowered_render_emit(c,"predicate_instructions",r->state,key,relation))return false;
    }
    for(size_t i=0;i<o->predicate_block_count;++i){const SolMirRuntimeLoweredPredicateBlock*r=&o->predicate_blocks[i];if(!lowered_render_predicate_coordinate(c,r->body,r->block,SOL_MIR_RUNTIME_LOWERED_NONE,"predicate-block",a)||!lowered_render_row_key(key,"predicate_blocks",a,"block"))return false;LOWERED_RELATION(" incoming-ordered=%zu outgoing-ordered=%zu",r->incoming_edges.count,r->outgoing_edges.count);if(!lowered_render_emit(c,"predicate_blocks",r->state,key,relation))return false;}
    for(size_t i=0;i<o->predicate_edge_count;++i){const SolMirRuntimeLoweredPredicateEdge*r=&o->predicate_edges[i];if(!lowered_render_predicate_coordinate(c,r->body,r->source,SOL_MIR_RUNTIME_LOWERED_NONE,"predicate-edge-source",a)||!lowered_render_predicate_coordinate(c,r->body,r->target,SOL_MIR_RUNTIME_LOWERED_NONE,"predicate-edge-target",b)||!lowered_render_row_key(key,"predicate_edges",a,b))return false;LOWERED_RELATION(" source=%s target=%s E=%zu",a,b,r->ordinal);if(!lowered_render_emit(c,"predicate_edges",r->state,key,relation))return false;}
    for(size_t i=0;i<o->predicate_incoming_edge_count;++i){const SolMirRuntimeLoweredPredicateBlockEdge*r=&o->predicate_incoming_edges[i];if(!lowered_render_predicate_coordinate(c,r->body,r->block,SOL_MIR_RUNTIME_LOWERED_NONE,"predicate-block",a)||!lowered_render_predicate_edge_key(c,r->edge,b)||!lowered_render_row_key(key,"predicate_incoming_edges",a,b))return false;LOWERED_RELATION(" block=%s edge=%s ordered-occurrence=%zu",a,b,r->ordinal);if(!lowered_render_emit(c,"predicate_incoming_edges",r->state,key,relation))return false;}
    for(size_t i=0;i<o->predicate_outgoing_edge_count;++i){const SolMirRuntimeLoweredPredicateBlockEdge*r=&o->predicate_outgoing_edges[i];if(!lowered_render_predicate_coordinate(c,r->body,r->block,SOL_MIR_RUNTIME_LOWERED_NONE,"predicate-block",a)||!lowered_render_predicate_edge_key(c,r->edge,b)||!lowered_render_row_key(key,"predicate_outgoing_edges",a,b))return false;LOWERED_RELATION(" block=%s edge=%s ordered-occurrence=%zu",a,b,r->ordinal);if(!lowered_render_emit(c,"predicate_outgoing_edges",r->state,key,relation))return false;}
    for(size_t i=0;i<o->predicate_terminator_count;++i){
        const SolMirRuntimeLoweredPredicateTerminator*r=&o->predicate_terminators[i];char call[65],event[65],failure[65],demands[65],plan[65];
        if(!lowered_render_predicate_coordinate(c,r->body,r->block,SOL_MIR_RUNTIME_LOWERED_NONE,"predicate-terminator",a)||!lowered_render_row_key(key,"predicate_terminators",a,"terminator")||!lowered_render_call_key(c,r->call,call)||!lowered_render_cleanup_event_key(c,r->cleanup_event,event)||!lowered_render_failure_key(c,r->failure_site,failure)||!lowered_render_demand_list_key(c,r->demanded_recipes,demands))return false;
        if(r->plan==SOL_MIR_RUNTIME_LOWERED_NONE)strcpy(plan,none);else if(!lowered_render_semantic_key(c,r->plan,plan))return false;
        LOWERED_RELATION(" kind=%u class=%u family=%u facilities=%08x semantic-plan=%s recipe-demands-count=%zu recipe-demands-digest=%s call=%s cleanup-event=%s failure-site=%s",(unsigned)r->kind,(unsigned)r->runtime_class,(unsigned)r->plan_family,r->facilities,plan,r->demanded_recipes.count,demands,call,event,failure);
        if(!lowered_render_emit(c,"predicate_terminators",r->state,key,relation))return false;
    }
    for(size_t i=0;i<o->semantic_plan_count;++i){const SolMirRuntimeLoweredSemanticPlan*r=&o->semantic_plans[i];char producer[65],prior[65];size_t occurrence=0;if(!lowered_render_semantic_key(c,i,a)||!lowered_render_semantic_producer_key(c,i,producer)||!lowered_render_row_key(key,"semantic_plans",a,"plan"))return false;for(size_t q=0;q<i;++q)if(o->semantic_plans[q].arena==r->arena){if(!lowered_render_semantic_producer_key(c,q,prior))return false;if(!strcmp(prior,producer))++occurrence;}LOWERED_RELATION(" arena=%u class=%u family=%u facilities=%08x producer-key=%s producer-occurrence=%zu",(unsigned)r->arena,(unsigned)r->runtime_class,(unsigned)r->plan_family,r->facilities,producer,occurrence);if(!lowered_render_emit(c,"semantic_plans",r->state,key,relation))return false;}
    for(size_t i=0;i<o->call_count;++i){
        const SolMirRuntimeLoweredCall*r=&o->calls[i];char callee[65],result[65],normal[65],failure[65],operands[65],writebacks[65],import[65],bound[65],entry[65],site[65];LoweredRenderList operand_list,writeback_list;
        if(!lowered_render_call_key(c,i,a)||!lowered_render_value_key(c,r->owner_kind,r->body,r->callee,callee)||!lowered_render_value_key(c,r->owner_kind,r->body,r->result,result)||!lowered_render_list_begin(&operand_list,"call-operands",r->operands.count)||!lowered_render_list_begin(&writeback_list,"call-writebacks",r->writebacks.count))return false;
        if(r->owner_kind==SOL_MIR_RUNTIME_CALL_OWNER_IMAGE){if(!lowered_render_image_edge_key(c,r->normal_edge,normal)||!lowered_render_image_edge_key(c,r->failure_edge,failure))return false;}else{if(!lowered_render_predicate_edge_key(c,r->normal_edge,normal)||!lowered_render_predicate_edge_key(c,r->failure_edge,failure))return false;}
        if(r->operands.offset>o->conventions->operand_count||r->operands.count>o->conventions->operand_count-r->operands.offset||r->writebacks.offset>o->conventions->writeback_count||r->writebacks.count>o->conventions->writeback_count-r->writebacks.offset)return false;
        for(size_t q=0;q<r->operands.count;++q){const SolMirRuntimeOperand*x=&o->conventions->operands[r->operands.offset+q];char value[65],detail[192],operand[65];if(!lowered_render_value_key(c,r->owner_kind,r->body,x->value,value))return false;int n=snprintf(detail,sizeof detail,"slot=%zu:value=%s",x->signature_slot,value);if(n<0||(size_t)n>=sizeof detail||!lowered_render_key(operand,"call-operand",detail)||!lowered_render_list_append(&operand_list,operand))return false;}
        for(size_t q=0;q<r->writebacks.count;++q){const SolMirRuntimeWriteback*x=&o->conventions->writebacks[r->writebacks.offset+q];char place[65],recipe[65],detail[256],writeback[65];if(!lowered_render_place_key(c,x->place,place)||!lowered_render_recipe_key(c,x->recipe,recipe))return false;int n=snprintf(detail,sizeof detail,"operand=%zu:receiver=%s:formal=%zu:place=%s:recipe=%s",x->operand,x->receiver?"true":"false",x->formal,place,recipe);if(n<0||(size_t)n>=sizeof detail||!lowered_render_key(writeback,"call-writeback",detail)||!lowered_render_list_append(&writeback_list,writeback))return false;}
        if(!lowered_render_list_finish(&operand_list,operands)||!lowered_render_list_finish(&writeback_list,writebacks)||!lowered_render_failure_key(c,r->failure_site,site))return false;
        if(r->import_id==SOL_MIR_RUNTIME_LOWERED_NONE)strcpy(import,none);else if(!lowered_render_import_key(c,r->import_id,import))return false;
        if(r->bound_environment_import==SOL_MIR_RUNTIME_LOWERED_NONE)strcpy(bound,none);else if(!lowered_render_import_key(c,r->bound_environment_import,bound))return false;
        if(r->entry==SOL_MIR_RUNTIME_LOWERED_NONE)strcpy(entry,none);else if(!lowered_render_entry_key(c,r->entry,entry))return false;
        LOWERED_RELATION(" call=%s callee=%s result=%s operands-count=%zu operands-digest=%s writebacks-count=%zu writebacks-digest=%s normal-edge=%s failure-edge=%s import=%s bound-environment=%s entry=%s failure-site=%s",a,callee,result,r->operands.count,operands,r->writebacks.count,writebacks,normal,failure,import,bound,entry,site);
        if(!lowered_render_row_key(key,"calls",a,"call")||!lowered_render_emit(c,"calls",r->state,key,relation))return false;
    }
    for(size_t i=0;i<o->signature_count;++i){
        const SolMirRuntimeLoweredSignature*r=&o->signatures[i];const SolMirRuntimeSignature*s=&o->conventions->signatures[r->signature];char result[65],slots[65],effects[65];LoweredRenderList list;
        if(!lowered_render_signature_key(c,r->signature,a)||!lowered_render_recipe_key(c,s->result,result)||!lowered_render_effect_key(c,s->effects,effects)||!lowered_render_row_key(key,"signatures",a,"signature")||!lowered_render_list_begin(&list,"signature-slots",s->slots.count))return false;
        if(s->slots.offset>o->conventions->signature_slot_count||s->slots.count>o->conventions->signature_slot_count-s->slots.offset)return false;
        for(size_t q=0;q<s->slots.count;++q){const SolMirRuntimeSignatureSlot*x=&o->conventions->signature_slots[s->slots.offset+q];char recipe[65],detail[192],slot[65];if(!lowered_render_recipe_key(c,x->recipe,recipe))return false;int n=snprintf(detail,sizeof detail,"role=%u:formal=%zu:recipe=%s:access=%u",(unsigned)x->role,x->formal,recipe,(unsigned)x->access);if(n<0||(size_t)n>=sizeof detail||!lowered_render_key(slot,"signature-slot",detail)||!lowered_render_list_append(&list,slot))return false;}
        if(!lowered_render_list_finish(&list,slots))return false;
        LOWERED_RELATION(" origin=%u result=%s result-class=%u slots-count=%zu slots-digest=%s effects=%s",(unsigned)s->origin,result,(unsigned)s->result_class,s->slots.count,slots,effects);
        if(!lowered_render_emit(c,"signatures",r->state,key,relation))return false;
    }
    for(size_t i=0;i<o->import_count;++i){const SolMirRuntimeLoweredImport*r=&o->imports[i];const SolMirRuntimeImport*x=&o->conventions->imports[r->import_id];char subject[65];if(!lowered_render_import_key(c,r->import_id,a)||!lowered_render_row_key(key,"imports",a,"import"))return false;if(x->kind==SOL_MIR_RUNTIME_IMPORT_HOST){if(!lowered_render_host_key(c,x->host,subject))return false;}else if(!lowered_render_recipe_key(c,x->recipe,subject))return false;LOWERED_RELATION(" kind=%u subject=%s recipe-operation=%u",(unsigned)x->kind,subject,x->recipe_operation);if(!lowered_render_emit(c,"imports",r->state,key,relation))return false;}
    for(size_t i=0;i<o->recipe_count;++i){const SolMirRuntimeLoweredRecipe*r=&o->recipes[i];char create[65],copy[65],drop[65],equal[65];if(!lowered_render_recipe_key(c,r->recipe,a)||!lowered_render_row_key(key,"recipes",a,"recipe"))return false;if(r->create_import==SOL_MIR_RUNTIME_LOWERED_NONE)strcpy(create,none);else if(!lowered_render_import_key(c,r->create_import,create))return false;if(r->copy_import==SOL_MIR_RUNTIME_LOWERED_NONE)strcpy(copy,none);else if(!lowered_render_import_key(c,r->copy_import,copy))return false;if(r->drop_import==SOL_MIR_RUNTIME_LOWERED_NONE)strcpy(drop,none);else if(!lowered_render_import_key(c,r->drop_import,drop))return false;if(r->equal_import==SOL_MIR_RUNTIME_LOWERED_NONE)strcpy(equal,none);else if(!lowered_render_import_key(c,r->equal_import,equal))return false;LOWERED_RELATION(" operations=%08x facilities=%08x create-import=%s copy-import=%s drop-import=%s equal-import=%s allocation=%u copy=%u equality=%u ownership=%u host-result=%u",r->demanded_operations,r->facilities,create,copy,drop,equal,(unsigned)r->allocation,(unsigned)r->copy,(unsigned)r->equality,(unsigned)r->ownership,(unsigned)r->host_result);if(!lowered_render_emit(c,"recipes",r->state,key,relation))return false;}
    for(size_t i=0;i<o->value_plan_count;++i){const SolMirRuntimeLoweredValuePlan*r=&o->value_plans[i];if(!lowered_render_recipe_key(c,r->recipe,a)||!lowered_render_row_key(key,"value_plans",a,"value-plan"))return false;LOWERED_RELATION(" facilities=%08x demanded=%08x allocation=%u copy=%u equality=%u ownership=%u host-result=%u",r->facilities,r->demanded_facilities,(unsigned)r->allocation,(unsigned)r->copy,(unsigned)r->equality,(unsigned)r->ownership,(unsigned)r->host_result);if(!lowered_render_emit(c,"value_plans",r->state,key,relation))return false;}
    for(size_t i=0;i<o->recipe_demand_count;++i){const SolMirRuntimeLoweredRecipeDemand*r=&o->recipe_demands[i];if(!lowered_render_recipe_key(c,r->recipe,a)||!lowered_render_demand_key(c,i,b)||!lowered_render_row_key(key,"recipe_demands",b,a))return false;LOWERED_RELATION(" recipe=%s consumer-demand=%s facilities=%08x relative-ordinal=%zu",a,b,r->facilities,r->ordinal);if(!lowered_render_emit(c,"recipe_demands",r->state,key,relation))return false;}
    for(size_t i=0;i<o->cleanup_failure_count;++i){
        const SolMirRuntimeLoweredCleanupFailure*r=&o->cleanup_failures[i];char event[65],row[65],inherited[65],supplemental[65],recipe[65];
        if(!lowered_render_cleanup_event_key(c,r->event,event))return false;
        if(r->kind==SOL_MIR_RUNTIME_LOWERED_CLEANUP_EVENT){
            LoweredRenderList actions,transitions;char action_digest[65],transition_digest[65];
            if(!lowered_render_cleanup_event_key(c,r->event,row)||!lowered_render_failure_key(c,r->inherited_failure_site,inherited)||!lowered_render_cleanup_supplemental_key(c,r->supplemental_site,supplemental)||!lowered_render_list_begin(&actions,"cleanup-event-actions",r->actions.count)||!lowered_render_list_begin(&transitions,"cleanup-event-transitions",r->transitions.count))return false;
            if(r->actions.offset>o->cleanup->action_count||r->actions.count>o->cleanup->action_count-r->actions.offset||r->transitions.offset>o->cleanup->transition_count||r->transitions.count>o->cleanup->transition_count-r->transitions.offset)return false;
            for(size_t q=0;q<r->actions.count;++q){char child[65];if(!lowered_render_cleanup_action_key(c,r->actions.offset+q,child)||!lowered_render_list_append(&actions,child))return false;}
            for(size_t q=0;q<r->transitions.count;++q){char child[65];if(!lowered_render_cleanup_transition_key(c,r->transitions.offset+q,child)||!lowered_render_list_append(&transitions,child))return false;}
            if(!lowered_render_list_finish(&actions,action_digest)||!lowered_render_list_finish(&transitions,transition_digest)||!lowered_render_row_key(key,"cleanup_failures",row,"event"))return false;
            LOWERED_RELATION(" event=%s producer=%u inherited-failure=%s supplemental-site=%s actions-count=%zu actions-digest=%s transitions-count=%zu transitions-digest=%s captures-detail=%s detail-kind=%u",event,(unsigned)r->producer,inherited,supplemental,r->actions.count,action_digest,r->transitions.count,transition_digest,r->captures_failure_detail?"true":"false",(unsigned)r->capture_detail_kind);
        }else if(r->kind==SOL_MIR_RUNTIME_LOWERED_CLEANUP_ACTION){
            char action[65],target[65],drop_path[65],frame[65];
            if(!lowered_render_cleanup_action_key(c,r->action,action)||!lowered_render_cleanup_action_target_key(c,r->action,target)||!lowered_render_cleanup_drop_path_key(c,r->drop_path,drop_path)||!lowered_render_handler_frame_key(c,r->frame,frame))return false;
            if(r->recipe==SOL_MIR_RECIPE_NONE)strcpy(recipe,none);else if(!lowered_render_recipe_key(c,r->recipe,recipe))return false;
            if(!lowered_render_row_key(key,"cleanup_failures",action,"action"))return false;
            LOWERED_RELATION(" action=%s action-kind=%u target=%s flags=%u recipe=%s drop-path=%s handler-frame=%s",action,(unsigned)r->action_kind,target,r->action_flags,recipe,drop_path,frame);
        }else if(r->kind==SOL_MIR_RUNTIME_LOWERED_CLEANUP_TRANSITION){
            char transition[65],edge[65],destination[65],failure[65],actions_digest[65];LoweredRenderList actions;
            if(!lowered_render_cleanup_transition_key(c,r->transition,transition)||!lowered_render_list_begin(&actions,"cleanup-transition-actions",r->actions.count))return false;
            if(r->source_edge==SOL_MIR_RUNTIME_LOWERED_NONE)strcpy(edge,none);else if(o->cleanup->events[r->event].kind==SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION||o->cleanup->events[r->event].kind==SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR){if(!lowered_render_predicate_edge_key(c,r->source_edge,edge))return false;}else if(!lowered_render_image_edge_key(c,r->source_edge,edge))return false;
            if(r->destination==SOL_MIR_RUNTIME_LOWERED_NONE)strcpy(destination,none);else if(o->cleanup->events[r->event].kind==SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION||o->cleanup->events[r->event].kind==SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR){if(!lowered_render_predicate_coordinate(c,o->cleanup->events[r->event].owner,r->destination,SOL_MIR_RUNTIME_LOWERED_NONE,"cleanup-destination",destination))return false;}else if(!lowered_render_image_coordinate(c,o->cleanup->events[r->event].owner,r->destination,SOL_MIR_RUNTIME_LOWERED_NONE,"cleanup-destination",destination))return false;
            if(r->failure_source==SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31||r->failure_source==SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_LOCAL_OR_PENDING){if(!lowered_render_failure_key(c,r->failure_site,failure))return false;}else if(r->failure_source==SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_SUPPLEMENTAL_P33){if(!lowered_render_cleanup_supplemental_key(c,r->failure_site,failure))return false;}else strcpy(failure,none);
            if(r->actions.offset>o->cleanup->action_count||r->actions.count>o->cleanup->action_count-r->actions.offset)return false;for(size_t q=0;q<r->actions.count;++q){char child[65];if(!lowered_render_cleanup_action_key(c,r->actions.offset+q,child)||!lowered_render_list_append(&actions,child))return false;}
            if(!lowered_render_list_finish(&actions,actions_digest)||!lowered_render_row_key(key,"cleanup_failures",transition,"transition"))return false;
            LOWERED_RELATION(" transition=%s event=%s edge=%s destination=%s outcome=%u role=%u actions-count=%zu actions-digest=%s primary-failure=%s failure-namespace=%u failure-site=%s mask=%08x",transition,event,edge,destination,(unsigned)r->outcome,(unsigned)r->edge_role,r->actions.count,actions_digest,r->primary_failure_wins?"true":"false",(unsigned)r->failure_source,failure,r->failure_mask);
        }else if(r->kind==SOL_MIR_RUNTIME_LOWERED_CLEANUP_SUPPLEMENTAL_SITE){
            if(!lowered_render_cleanup_supplemental_key(c,r->supplemental_site,row)||!lowered_render_row_key(key,"cleanup_failures",row,"supplemental"))return false;
            LOWERED_RELATION(" supplemental-site=%s event=%s failure-mask=%08x",row,event,r->failure_mask);
        }else if(r->kind==SOL_MIR_RUNTIME_LOWERED_CLEANUP_DROP_PATH){
            if(!lowered_render_cleanup_drop_path_key(c,r->drop_path,row)||!lowered_render_row_key(key,"cleanup_failures",row,"drop-path"))return false;
            LOWERED_RELATION(" drop-path=%s holes-count=%zu liveness=%u",row,r->drop_holes.count,(unsigned)r->drop_liveness);
        }else return false;
        if(!lowered_render_emit(c,"cleanup_failures",r->state,key,relation))return false;
    }
    for(size_t i=0;i<o->host_requirement_count;++i){
        const SolMirRuntimeLoweredHostRequirement*r=&o->host_requirements[i];char requirement[65],grant[65],entry[65],root[65],operation[65],call[65],event[65],transition[65],import[65],signature[65];size_t grant_index=SOL_MIR_RUNTIME_LOWERED_NONE;
        for(size_t q=0;q<o->host_grant_count;++q)if(o->host_grants[q].entry==r->entry&&o->host_grants[q].root==r->root&&o->host_grants[q].operation==r->operation){grant_index=q;break;}
        if(grant_index==SOL_MIR_RUNTIME_LOWERED_NONE||!lowered_render_host_requirement_key(c,i,requirement)||!lowered_render_host_grant_key(c,grant_index,grant)||!lowered_render_entry_key(c,r->entry,entry)||!lowered_render_host_root_key(c,r->root,root)||!lowered_render_host_operation_key(c,r->operation,operation)||!lowered_render_call_key(c,r->call,call)||!lowered_render_cleanup_event_key(c,r->event,event)||!lowered_render_cleanup_transition_key(c,r->transition,transition)||!lowered_render_import_key(c,r->import_id,import)||!lowered_render_signature_key(c,r->signature,signature)||!lowered_render_row_key(key,"host_requirements",requirement,"requirement"))return false;
        LOWERED_RELATION(" requirement=%s grant=%s entry=%s root=%s operation=%s call=%s cleanup-event=%s transition=%s import=%s signature=%s",requirement,grant,entry,root,operation,call,event,transition,import,signature);
        if(!lowered_render_emit(c,"host_requirements",r->state,key,relation))return false;
    }
    for(size_t i=0;i<o->host_grant_count;++i){
        const SolMirRuntimeLoweredHostGrant*r=&o->host_grants[i];char grant[65],entry[65],root[65],operation[65];
        if(!lowered_render_host_grant_key(c,i,grant)||!lowered_render_entry_key(c,r->entry,entry)||!lowered_render_host_root_key(c,r->root,root)||!lowered_render_host_operation_key(c,r->operation,operation)||!lowered_render_row_key(key,"host_grants",grant,"grant"))return false;
        LOWERED_RELATION(" grant=%s entry=%s root=%s operation=%s incidences-count=%zu",grant,entry,root,operation,r->incidences.count);
        if(!lowered_render_emit(c,"host_grants",r->state,key,relation))return false;
    }
    for(size_t i=0;i<o->host_incidence_count;++i){
        const SolMirRuntimeLoweredHostIncidence*r=&o->host_incidences[i];char incidence[65],grant[65],requirement[65];
        if(!lowered_render_host_incidence_key(c,i,incidence)||!lowered_render_host_grant_key(c,r->grant,grant)||!lowered_render_host_requirement_key(c,r->requirement,requirement)||!lowered_render_row_key(key,"host_incidences",incidence,"incidence"))return false;
        LOWERED_RELATION(" incidence=%s grant=%s requirement=%s",incidence,grant,requirement);
        if(!lowered_render_emit(c,"host_incidences",r->state,key,relation))return false;
    }
    for(size_t i=0;i<o->handler_frame_count;++i){
        const SolMirRuntimeLoweredHandlerFrame*r=&o->handler_frames[i];const SolMirMaterialization*m=&o->conventions->concrete->materialization;char frame[65],parent[65],operation[65],source_signature[65],root[65],effects[65],provider_place[65],recipe[65],access[65],provider[65],signature[65],enter[65];
        if(r->handler>=m->handler_count||r->enter>=m->instruction_count||!lowered_render_handler_frame_key(c,i,frame)||!lowered_render_handler_frame_key(c,r->parent,parent)||!lowered_render_operation_key(c,&r->source_operation,operation)||!lowered_render_signature_key(c,r->source_signature,source_signature)||!lowered_render_place_key(c,r->root,root)||!lowered_render_effect_key(c,r->effects,effects)||!lowered_render_place_key(c,r->provider_place,provider_place)||!lowered_render_recipe_key(c,r->provider_recipe,recipe)||!lowered_render_operation_access_key(c,r->provider_access,access)||r->provider>=o->conventions->concrete->linkage.callable_count||!lowered_render_digest_key(provider,&o->conventions->concrete->linkage.callables[r->provider].instance_key)||!lowered_render_signature_key(c,r->signature,signature)||!lowered_render_image_coordinate(c,m->handlers[r->handler].parent,m->instructions[r->enter].block,r->enter,"handler-enter",enter)||!lowered_render_row_key(key,"handler_frames",frame,"frame"))return false;
        LOWERED_RELATION(" frame=%s parent=%s source-operation=%s source-signature=%s root-match=%u root-place=%s effects=%s provider-place=%s provider-recipe=%s provider-access=%s provider-internal=%s provider-signature=%s enter=%s",frame,parent,operation,source_signature,(unsigned)r->root_match,root,effects,provider_place,recipe,access,provider,signature,enter);
        if(!lowered_render_emit(c,"handler_frames",r->state,key,relation))return false;
    }
    for(size_t i=0;i<o->handler_marker_count;++i){
        const SolMirRuntimeLoweredHandlerMarker*r=&o->handler_markers[i];char frame[65],marker[65];
        if(!lowered_render_handler_frame_key(c,r->frame,frame)||!lowered_render_image_coordinate(c,r->image,r->block,r->marker,"handler-marker",marker)||!lowered_render_row_key(key,"handler_markers",frame,marker))return false;
        LOWERED_RELATION(" frame=%s marker=%s",frame,marker);
        if(!lowered_render_emit(c,"handler_markers",r->state,key,relation))return false;
    }
    for(size_t i=0;i<o->handler_exit_count;++i){
        const SolMirRuntimeLoweredHandlerExit*r=&o->handler_exits[i];char frame[65],action[65],transition[65];
        if(!lowered_render_handler_frame_key(c,r->frame,frame)||!lowered_render_cleanup_action_key(c,r->action,action)||!lowered_render_cleanup_transition_key(c,r->transition,transition)||!lowered_render_row_key(key,"handler_exits",frame,action))return false;
        LOWERED_RELATION(" frame=%s cleanup-action=%s transition=%s one-pop=%s",frame,action,transition,r->alternative_one_pop?"true":"false");
        if(!lowered_render_emit(c,"handler_exits",r->state,key,relation))return false;
    }
    #undef LOWERED_RELATION
    return true;
}

bool sol_mir_runtime_lowered_program_render(FILE *stream,
    const SolMirRuntimeLoweredProgram *owner) {
    size_t rows = 0;
    LoweredRenderBuffer output = {0};
    LoweredRenderLine *lines = NULL;
    LoweredRenderContext context;
    if (stream == NULL || !sol_mir_runtime_lowered_program_validate(owner, NULL)
        || owner->usage.render_bytes > owner->limits.max_render_bytes
        || owner->usage.render_scratch_bytes > owner->limits.max_render_scratch_bytes) return false;
#define COUNT_ROWS(member,type,singular) if (!lowered_render_add(&rows, owner->singular##_count)) return false;
    SOL_MIR_RUNTIME_LOWERED_TABLES(COUNT_ROWS)
#undef COUNT_ROWS
    if (rows > SIZE_MAX / sizeof *lines || owner->usage.render_scratch_bytes / sizeof *lines < rows)
        return false;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (fail_render) return false;
#endif
    output.data = calloc(owner->usage.render_bytes ? owner->usage.render_bytes : 1, 1);
    lines = rows ? calloc(rows, sizeof *lines) : NULL;
    if (!output.data || (rows && !lines)) { free(output.data); free(lines); return false; }
    output.capacity = owner->usage.render_bytes;
    context = (LoweredRenderContext){owner, lines, 0, rows, false};
    lowered_render_put(&output, "mir_runtime_lowered_program\n");
    lowered_render_put(&output, "declaration.kind=runtime-lowered static=true backend-independent=true runtime-execution=false physical-abi=false canonical-relations=true typed-keys=true\n");
    {
        char erased[128];
        int written = snprintf(erased, sizeof erased,
            "census erased-loop-obligations=%zu\n", owner->usage.erased_loops);
        if (written < 0 || (size_t)written >= sizeof erased) output.failed = true;
        else lowered_render_put(&output, erased);
    }
    if (!lowered_render_rows(&context) || context.count != rows) { free(output.data); free(lines); return false; }
    /* Table census is a semantic inventory, not a resource meter. */
#define CENSUS(member,type,singular) do { size_t present_count=0, demanded_count=0, erased_count=0; for(size_t i=0;i<owner->singular##_count;++i){SolMirRuntimeLoweredState state=owner->member[i].state; if(state==SOL_MIR_RUNTIME_LOWERED_PRESENT)++present_count; else if(state==SOL_MIR_RUNTIME_LOWERED_NOT_DEMANDED)++demanded_count; else if(state==SOL_MIR_RUNTIME_LOWERED_ERASED)++erased_count; else { output.failed=true; }} char census[192]; int n=snprintf(census,sizeof census,"table=%s present=%zu not-demanded=%zu erased=%zu\n",#member,present_count,demanded_count,erased_count); if(n<0||(size_t)n>=sizeof census)output.failed=true;else lowered_render_put(&output,census); } while(0);
    SOL_MIR_RUNTIME_LOWERED_TABLES(CENSUS)
#undef CENSUS
    /* Preserve the public macro table order; within each table sort the full
     * typed key/relation line, so equal keys still have deterministic joins. */
    for (size_t first=0; first<rows;) {
        size_t last=first+1;
        while(last<rows && !strcmp(lines[first].table,lines[last].table)) ++last;
        qsort(lines+first,last-first,sizeof *lines,lowered_render_compare);
        for(size_t i=first;i<last;++i) lowered_render_put(&output,lines[i].text);
        first=last;
    }
    if (output.failed || output.count == 0) { free(output.data); free(lines); return false; }
    #ifdef SOL_MIR_PLAN_TEST_HOOKS
    ++render_write_attempts;
    #endif
    bool written = fwrite(output.data, 1, output.count, stream) == output.count;
    free(output.data); free(lines);
    return written;
}
