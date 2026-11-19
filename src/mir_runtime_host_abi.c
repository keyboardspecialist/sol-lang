#include "sol/mir_runtime_host_abi.h"
#include "mir_runtime_host_abi_internal.h"
#include "mir_linkage_internal.h"

#include <stdlib.h>
#include <string.h>
#ifdef SOL_MIR_PLAN_TEST_HOOKS
#include <stdio.h>
#endif

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

#ifdef SOL_MIR_PLAN_TEST_HOOKS
_Thread_local SolMirRuntimeHostAbiObservationContext
    *sol_mir_runtime_host_abi_observation_context;

static void observation_json_string(FILE *stream, const char *text) {
    if (!text) { fputs("null", stream); return; }
    fputc('"', stream);
    for (const unsigned char *p=(const unsigned char *)text; *p; ++p) {
        switch (*p) {
        case '"': fputs("\\\"", stream); break;
        case '\\': fputs("\\\\", stream); break;
        case '\b': fputs("\\b", stream); break;
        case '\f': fputs("\\f", stream); break;
        case '\n': fputs("\\n", stream); break;
        case '\r': fputs("\\r", stream); break;
        case '\t': fputs("\\t", stream); break;
        default:
            if (*p < 0x20) fprintf(stream, "\\u%04x", (unsigned)*p);
            else fputc(*p, stream);
        }
    }
    fputc('"', stream);
}

void sol_mir_runtime_host_abi_observe_failure(const char *id,
    const char *phase, const char *branch, const char *message) {
    SolMirRuntimeHostAbiObservationContext *c=
        sol_mir_runtime_host_abi_observation_context;
    if (!c || c->first_failure_id) return;
    c->first_failure_id=id; c->first_failure_phase=phase;
    c->first_failure_branch=branch; c->first_failure_message=message;
    if (c->validator_active && !c->validator_rejection_id) {
        c->validator_rejection_id=id;
        c->validator_rejection_message=message;
    }
}

void sol_mir_runtime_host_abi_observe_outcome_message(const char *message) {
    SolMirRuntimeHostAbiObservationContext *c=
        sol_mir_runtime_host_abi_observation_context;
    if (c && !c->producer_message) c->producer_message=message;
}

void sol_mir_runtime_host_abi_observe_shared_failure(const char *producer_id,
    const char *validator_id, const char *producer_phase,
    const char *validator_phase, const char *branch, const char *message) {
    SolMirRuntimeHostAbiObservationContext *c=
        sol_mir_runtime_host_abi_observation_context;
    if (!c) return;
    bool validator=c->validator_active;
    sol_mir_runtime_host_abi_observe_failure(validator?validator_id:producer_id,
        validator?validator_phase:producer_phase,branch,message);
}

void sol_mir_runtime_host_abi_observe_propagation(const char *id,
    const char *branch) {
    SolMirRuntimeHostAbiObservationContext *c=
        sol_mir_runtime_host_abi_observation_context;
    if (!c || c->propagation_id) return;
    c->propagation_id=id; c->propagation_branch=branch;
}

void sol_mir_runtime_host_abi_observe_validator_rejection(const char *id,
    const char *message) {
    SolMirRuntimeHostAbiObservationContext *c=
        sol_mir_runtime_host_abi_observation_context;
    if (!c || c->validator_rejection_id) return;
    c->validator_rejection_id=id; c->validator_rejection_message=message;
    sol_mir_runtime_host_abi_observe_failure(id, "validator",
        "original validator rejection", message);
}

bool sol_mir_runtime_host_abi_observe_check(const char *id, bool result) {
    if (!result) sol_mir_runtime_host_abi_observe_failure(id, "producer",
        "original terminating check returned false", NULL);
    return result;
}

bool sol_mir_runtime_host_abi_observe_reject(const char *id, bool rejected) {
    if (rejected) {
        SolMirRuntimeHostAbiObservationContext *c=
            sol_mir_runtime_host_abi_observation_context;
        sol_mir_runtime_host_abi_observe_failure(id,c&&c->validator_active
            ?"validator":"producer",
            "original atomic rejection predicate returned true",NULL);
        sol_mir_runtime_host_abi_observe_value(id,"evaluated","boolean",1);
    }
    return rejected;
}

void sol_mir_runtime_host_abi_observe_value(const char *name,
    const char *availability, const char *units, size_t value) {
    SolMirRuntimeHostAbiObservationContext *c=
        sol_mir_runtime_host_abi_observation_context;
    if (!c) return;
    if (c->value_count==sizeof(c->values)/sizeof(c->values[0])) {
        c->capture_gap=true;
        if(!c->capture_gap_reason)c->capture_gap_reason="operand storage capacity exceeded";
        return;
    }
    c->values[c->value_count++]=(SolMirRuntimeHostAbiObservationValue){
        name,availability,units,value,true};
}

void sol_mir_runtime_host_abi_observe_unavailable_value(const char *name,
    const char *availability, const char *units) {
    SolMirRuntimeHostAbiObservationContext *c=
        sol_mir_runtime_host_abi_observation_context;
    if (!c) return;
    if (c->value_count==sizeof(c->values)/sizeof(c->values[0])) {
        c->capture_gap=true;
        if(!c->capture_gap_reason)c->capture_gap_reason="operand storage capacity exceeded";
        return;
    }
    c->values[c->value_count++]=(SolMirRuntimeHostAbiObservationValue){
        name,availability,units,0,false};
}

void sol_mir_runtime_host_abi_observe_index(size_t slot, const char *name,
    size_t value) {
    SolMirRuntimeHostAbiObservationContext *c=
        sol_mir_runtime_host_abi_observation_context;
    if (!c) return;
    if (slot>=sizeof(c->current_index_names)/sizeof(c->current_index_names[0])) {
        c->capture_gap=true;
        if(!c->capture_gap_reason)c->capture_gap_reason="loop index metadata capacity exceeded";
        return;
    }
    for (size_t i=slot;i<sizeof(c->current_index_available)
            /sizeof(c->current_index_available[0]);++i)
        c->current_index_available[i]=false;
    c->current_index_names[slot]=name;
    c->current_index_values[slot]=value;
    c->current_index_available[slot]=true;
}

void sol_mir_runtime_host_abi_observe_meter_failure(
    const SolMirRuntimeHostAbiWorkMeter *meter, const char *id,
    bool used_was_evaluated, bool limit_was_evaluated) {
    SolMirRuntimeHostAbiObservationContext *c=
        sol_mir_runtime_host_abi_observation_context;
    if (!c) return;
    const char *predicate=id;
    if (c->validator_active) {
        if (!meter) predicate="H37.V.sol_mir_runtime_host_abi_work_tick.14.meter_null";
        else if (limit_was_evaluated) predicate="H37.V.sol_mir_runtime_host_abi_work_tick.14.limit";
        else predicate="H37.V.sol_mir_runtime_host_abi_work_tick.14.used_overflow";
    }
    sol_mir_runtime_host_abi_observe_failure(predicate,"work-meter",
        "original work_tick returned false",NULL);
    if (!meter) {
        sol_mir_runtime_host_abi_observe_unavailable_value("meter.used",
            "not_reached","operations");
        sol_mir_runtime_host_abi_observe_unavailable_value("meter.limit",
            "not_reached","operations");
    } else {
        if (used_was_evaluated)
            sol_mir_runtime_host_abi_observe_value("meter.used","initialized",
                "operations",meter->used);
        else
            sol_mir_runtime_host_abi_observe_unavailable_value("meter.used",
                "not_reached","operations");
        if (limit_was_evaluated)
            sol_mir_runtime_host_abi_observe_value("meter.limit","initialized",
                "operations",meter->limit);
        else
            sol_mir_runtime_host_abi_observe_unavailable_value("meter.limit",
                "not_reached","operations");
    }
    for(size_t i=0;i<sizeof(c->current_index_names)/sizeof(c->current_index_names[0]);i++)
        if(c->current_index_available[i])
            sol_mir_runtime_host_abi_observe_value(c->current_index_names[i],
                "evaluated","index",c->current_index_values[i]);
}

void sol_mir_runtime_host_abi_observe_validator_result(const char *scope,
    SolMirRuntimeHostAbiBuildOutcome outcome) {
    SolMirRuntimeHostAbiObservationContext *c=
        sol_mir_runtime_host_abi_observation_context;
    if (!c) return;
    if (scope && strcmp(scope,"measurement")==0) {
        c->measurement_scope=scope; c->measurement_outcome=outcome;
        if(outcome!=SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED)
            c->measurement_rejection_id=c->validator_rejection_id
                ?c->validator_rejection_id:c->first_failure_id;
        c->measurement_recorded=true;
    } else {
        c->final_validation_scope=scope; c->final_validation_outcome=outcome;
        if(outcome!=SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED)
            c->final_validation_rejection_id=c->validator_rejection_id
                ?c->validator_rejection_id:c->first_failure_id;
        c->final_validation_recorded=true;
    }
}

int sol_mir_runtime_host_abi_observe_comparison(const char *id,
    const void *left, const void *right, size_t length, int result,
    size_t left_index, size_t right_index, bool equality_is_rejection) {
    SolMirRuntimeHostAbiObservationContext *c=
        sol_mir_runtime_host_abi_observation_context;
    bool rejected=equality_is_rejection ? result==0 : result!=0;
    if (c && rejected && !c->comparison_available) {
        sol_mir_runtime_host_abi_observe_failure(id,
            c->validator_active?"validator-comparison":"producer-comparison",
            equality_is_rejection?"original equality rejected duplicate":"original byte comparison rejected mismatch",NULL);
        if (!left || !right || length>sizeof(c->comparison_left)) {
            c->capture_gap=true;
            if(!c->capture_gap_reason)c->capture_gap_reason="comparison operands unavailable or exceed bounded byte storage";
        } else {
            c->comparison_id=id; c->comparison_left_index=left_index;
            c->comparison_right_index=right_index;
            c->comparison_length=length; c->comparison_result=result;
            c->comparison_is_duplicate=equality_is_rejection;
            c->comparison_left_available=true;
            c->comparison_right_available=true;
            c->comparison_result_available=true;
            memcpy(c->comparison_left,left,length);
            memcpy(c->comparison_right,right,length);
            c->comparison_available=true;
        }
    }
    return result;
}

int sol_mir_runtime_host_abi_observe_compare_once(const char *id,
    const void *left, const void *right, size_t length, size_t left_index,
    size_t right_index, bool equality_is_rejection) {
    int result=memcmp(left,right,length);
    return sol_mir_runtime_host_abi_observe_comparison(id,left,right,length,
        result,left_index,right_index,equality_is_rejection);
}

int sol_mir_runtime_host_abi_observe_compare_once_shared(
    const char *producer_id, const char *validator_id, const void *left,
    const void *right, size_t length, size_t left_index, size_t right_index,
    bool equality_is_rejection) {
    SolMirRuntimeHostAbiObservationContext *c=
        sol_mir_runtime_host_abi_observation_context;
    return sol_mir_runtime_host_abi_observe_compare_once(
        c&&c->validator_active?validator_id:producer_id,left,right,length,
        left_index,right_index,equality_is_rejection);
}

void sol_mir_runtime_host_abi_observe_capability_comparison(const char *id,
    const void *left,const void *right,size_t length,size_t left_index,
    size_t right_index,bool equal) {
    SolMirRuntimeHostAbiObservationContext *c=
        sol_mir_runtime_host_abi_observation_context;
    if(!c||equal||c->comparison_available)return;
    sol_mir_runtime_host_abi_observe_failure(id,
        c->validator_active?"validator-fieldwise-comparison":"producer-fieldwise-comparison",
        "declared capability field inequality rejected row",NULL);
    if(!left||!right||length>sizeof(c->comparison_left)){
        c->capture_gap=true;
        if(!c->capture_gap_reason)c->capture_gap_reason=
            "capability comparison operands unavailable or exceed bounded byte storage";
        return;
    }
    c->comparison_id=id;
    c->comparison_left_index=left_index;
    c->comparison_right_index=right_index;
    c->comparison_length=length;
    c->comparison_left_available=true;
    c->comparison_right_available=true;
    c->comparison_result_available=false;
    c->comparison_is_fieldwise=true;
    c->comparison_is_duplicate=false;
    c->comparison_operation="capability_fields_equal";
    c->comparison_equal_available=true;
    c->comparison_equal=false;
    memcpy(c->comparison_left,left,length);
    memcpy(c->comparison_right,right,length);
    c->comparison_available=true;
}

bool sol_mir_runtime_host_abi_observe_ne(const char *id,
    const char *left_name, size_t left, const char *right_name, size_t right) {
    bool rejected=left!=right;
    if (rejected) {
        SolMirRuntimeHostAbiObservationContext *c=
            sol_mir_runtime_host_abi_observation_context;
        sol_mir_runtime_host_abi_observe_failure(id,c&&c->validator_active
            ?"validator":"producer",
            "original field inequality rejected row",NULL);
        sol_mir_runtime_host_abi_observe_value(left_name,"evaluated",
            "integer-or-enum",left);
        sol_mir_runtime_host_abi_observe_value(right_name,"evaluated",
            "integer-or-enum",right);
    }
    return rejected;
}

bool sol_mir_runtime_host_abi_observe_eq(const char *id,
    const char *left_name, size_t left, const char *right_name, size_t right) {
    bool rejected=left==right;
    if (rejected) {
        SolMirRuntimeHostAbiObservationContext *c=
            sol_mir_runtime_host_abi_observation_context;
        sol_mir_runtime_host_abi_observe_failure(id,c&&c->validator_active
            ?"validator":"producer",
            "original field equality rejected row",NULL);
        sol_mir_runtime_host_abi_observe_value(left_name,"evaluated",
            "integer-or-enum",left);
        sol_mir_runtime_host_abi_observe_value(right_name,"evaluated",
            "integer-or-enum",right);
    }
    return rejected;
}

bool sol_mir_runtime_host_abi_observe_ge(const char *id,
    const char *left_name, size_t left, const char *right_name, size_t right) {
    bool rejected=left>=right;
    if (rejected) {
        SolMirRuntimeHostAbiObservationContext *c=
            sol_mir_runtime_host_abi_observation_context;
        sol_mir_runtime_host_abi_observe_failure(id,c&&c->validator_active
            ?"validator":"producer",
            "original range comparison rejected row",NULL);
        sol_mir_runtime_host_abi_observe_value(left_name,"evaluated",
            "integer-or-enum",left);
        sol_mir_runtime_host_abi_observe_value(right_name,"evaluated",
            "integer-or-enum",right);
    }
    return rejected;
}

bool sol_mir_runtime_host_abi_observe_gt(const char *id,
    const char *left_name, size_t left, const char *right_name, size_t right) {
    bool rejected=left>right;
    if (rejected) {
        SolMirRuntimeHostAbiObservationContext *c=
            sol_mir_runtime_host_abi_observation_context;
        sol_mir_runtime_host_abi_observe_failure(id,c&&c->validator_active
            ?"validator":"producer",
            "original limit comparison rejected owner",NULL);
        sol_mir_runtime_host_abi_observe_value(left_name,"evaluated",
            "units",left);
        sol_mir_runtime_host_abi_observe_value(right_name,"evaluated",
            "units",right);
    }
    return rejected;
}

bool sol_mir_runtime_host_abi_observe_le(const char *id,
    const char *left_name, size_t left, const char *right_name, size_t right) {
    bool accepted=left<=right;
    if (!accepted) {
        SolMirRuntimeHostAbiObservationContext *c=
            sol_mir_runtime_host_abi_observation_context;
        sol_mir_runtime_host_abi_observe_failure(id,c&&c->validator_active
            ?"validator":"producer",
            "original inclusive limit comparison rejected owner",NULL);
        sol_mir_runtime_host_abi_observe_value(left_name,"evaluated",
            "units",left);
        sol_mir_runtime_host_abi_observe_value(right_name,"evaluated",
            "units",right);
    }
    return accepted;
}

void sol_mir_runtime_host_abi_observe_fieldwise_row(const char *id,
    const void *row, size_t length, size_t row_index) {
    SolMirRuntimeHostAbiObservationContext *c=
        sol_mir_runtime_host_abi_observation_context;
    if (!c || c->fieldwise_available) return;
    if (!row || length>sizeof(c->comparison_left)) {
        c->capture_gap=true;
        if(!c->capture_gap_reason)c->capture_gap_reason="fieldwise row unavailable or exceeds bounded byte storage";
        return;
    }
    c->fieldwise_id=id;
    c->fieldwise_index=row_index;
    c->fieldwise_length=length;
    memcpy(c->fieldwise_bytes,row,length);
    c->fieldwise_available=true;
}

void sol_mir_runtime_host_abi_observe_capture_gap(const char *reason) {
    SolMirRuntimeHostAbiObservationContext *c=
        sol_mir_runtime_host_abi_observation_context;
    if (!c) return;
    c->capture_gap=true;
    if (!c->capture_gap_reason)
        c->capture_gap_reason=reason?reason:"diagnostic object unavailable";
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
static void observe_grants_if_available(bool available,const size_t *grants) {
    if (available && grants)
        sol_mir_runtime_host_abi_observe_value(
            "grants","initialized","records",*grants);
    else
        sol_mir_runtime_host_abi_observe_unavailable_value(
            "grants","not_reached","records");
}
#endif

static const char *observation_outcome_name(SolMirRuntimeHostAbiBuildOutcome x) {
    switch (x) {
    case SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED: return "SUCCEEDED";
    case SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR: return "INVALID_PREDECESSOR";
    case SOL_MIR_RUNTIME_HOST_ABI_BUILD_UNSUPPORTED: return "UNSUPPORTED";
    case SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED: return "RESOURCE_EXHAUSTED";
    case SOL_MIR_RUNTIME_HOST_ABI_BUILD_ALLOCATION_FAILED: return "ALLOCATION_FAILED";
    case SOL_MIR_RUNTIME_HOST_ABI_BUILD_INTERNAL_FAILED: return "INTERNAL_FAILED";
    }
    return "UNKNOWN_ENUM";
}

static void observation_extent(FILE *stream, const char *id, const char *side,
    const char *side_type, const char *field, size_t offset, size_t size) {
    fputs("H37\t{\"kind\":\"extent\",\"comparison\":",stream);
    observation_json_string(stream,id); fputs(",\"side\":",stream);
    observation_json_string(stream,side); fputs(",\"type\":",stream);
    observation_json_string(stream,side_type); fputs(",\"leaf\":",stream);
    observation_json_string(stream,field); fprintf(stream,
        ",\"offset\":\"%zu\",\"size\":\"%zu\"}\n",offset,size);
}

#define OBS_EXTENT(type,member,label) \
    observation_extent(stream,id,"left",#type,label,offsetof(type,member),sizeof(((type *)0)->member)); \
    observation_extent(stream,id,"right",#type,label,offsetof(type,member),sizeof(((type *)0)->member))
#define OBS_LEFT_EXTENT(type,member,label) \
    observation_extent(stream,id,"left",#type,label,offsetof(type,member),sizeof(((type *)0)->member))
#define OBS_NESTED_LEFT_EXTENT(type,member,nested,nested_member,label) \
    observation_extent(stream,id,"left",#type,label,offsetof(type,member)+offsetof(nested,nested_member), \
        sizeof(((nested *)0)->nested_member))
static void observation_emit_extents(FILE *stream, const char *id) {
    if (strstr(id,".entry-root.")) {
        OBS_EXTENT(SolMirRuntimeHostEntryRoot,entry,"entry");
        OBS_EXTENT(SolMirRuntimeHostEntryRoot,formal,"formal");
        OBS_EXTENT(SolMirRuntimeHostEntryRoot,recipe,"recipe");
        OBS_EXTENT(SolMirRuntimeHostEntryRoot,signature,"signature");
    } else if (strstr(id,".capability.")
        ||strstr(id,".capability_fields_equal")) {
        OBS_EXTENT(SolMirRuntimeHostCapabilityPlan,recipe,"recipe");
        OBS_EXTENT(SolMirRuntimeHostCapabilityPlan,source,"source");
        OBS_EXTENT(SolMirRuntimeHostCapabilityPlan,source_construct,"source_construct");
        OBS_EXTENT(SolMirRuntimeHostCapabilityPlan,result,"result");
        OBS_EXTENT(SolMirRuntimeHostCapabilityPlan,temporary,"temporary");
        OBS_EXTENT(SolMirRuntimeHostCapabilityPlan,source_value,"source_value");
        OBS_EXTENT(SolMirRuntimeHostCapabilityPlan,source_temporary,"source_temporary");
        OBS_EXTENT(SolMirRuntimeHostCapabilityPlan,source_place,"source_place");
        OBS_EXTENT(SolMirRuntimeHostCapabilityPlan,source_root,"source_root");
        OBS_EXTENT(SolMirRuntimeHostCapabilityPlan,parent,"parent");
        OBS_EXTENT(SolMirRuntimeHostCapabilityPlan,private_recipe,"private_recipe");
        OBS_EXTENT(SolMirRuntimeHostCapabilityPlan,private_value,"private_value");
    } else if (strstr(id,".requirement.")) {
        OBS_EXTENT(SolMirRuntimeHostRequirement,entry,"entry");
        OBS_EXTENT(SolMirRuntimeHostRequirement,root,"root");
        OBS_EXTENT(SolMirRuntimeHostRequirement,operation,"operation");
        OBS_EXTENT(SolMirRuntimeHostRequirement,call,"call");
        OBS_EXTENT(SolMirRuntimeHostRequirement,event,"event");
        OBS_EXTENT(SolMirRuntimeHostRequirement,failure_transition,"failure_transition");
    } else if (strstr(id,".identity.")) {
        OBS_EXTENT(SolMirLinkageDigest,bytes,"identity.bytes");
    } else if (strstr(id,".identity")) {
        OBS_EXTENT(SolMirLinkageDigest,bytes,"identity.bytes");
    } else if (strstr(id,".symbol.")) {
        OBS_EXTENT(SolMirLinkageSymbol,bytes,"symbol.bytes");
    } else if (strstr(id,".symbol")) {
        OBS_EXTENT(SolMirLinkageSymbol,bytes,"symbol.bytes");
    } else if (strstr(id,".formal.")) {
        OBS_LEFT_EXTENT(SolMirRuntimeHostFormal,recipe,"recipe");
        OBS_LEFT_EXTENT(SolMirRuntimeHostFormal,access,"access");
        OBS_LEFT_EXTENT(SolMirRuntimeHostFormal,shape,"shape");
    } else if (strstr(id,".shape-case.")) {
        OBS_LEFT_EXTENT(SolMirRuntimeHostShapeCase,parent,"parent");
        OBS_LEFT_EXTENT(SolMirRuntimeHostShapeCase,ordinal,"ordinal");
        OBS_LEFT_EXTENT(SolMirRuntimeHostShapeCase,child,"child");
    } else if (strstr(id,".shape.")) {
        OBS_LEFT_EXTENT(SolMirRuntimeHostShape,recipe,"recipe");
        OBS_LEFT_EXTENT(SolMirRuntimeHostShape,kind,"kind");
        OBS_LEFT_EXTENT(SolMirRuntimeHostShape,access,"access");
        OBS_NESTED_LEFT_EXTENT(SolMirRuntimeHostShape,cases,SolMirRuntimeSlice,offset,"cases.offset");
        OBS_NESTED_LEFT_EXTENT(SolMirRuntimeHostShape,cases,SolMirRuntimeSlice,count,"cases.count");
    } else if (strstr(id,".argument.")) {
        OBS_LEFT_EXTENT(SolMirRuntimeHostArgument,recipe,"recipe");
        OBS_LEFT_EXTENT(SolMirRuntimeHostArgument,kind,"kind");
        OBS_LEFT_EXTENT(SolMirRuntimeHostArgument,access,"access");
        OBS_NESTED_LEFT_EXTENT(SolMirRuntimeHostArgument,children,SolMirRuntimeSlice,offset,"children.offset");
        OBS_NESTED_LEFT_EXTENT(SolMirRuntimeHostArgument,children,SolMirRuntimeSlice,count,"children.count");
    } else if (strstr(id,".host-operation.")) {
        OBS_LEFT_EXTENT(SolMirRuntimeHostOperation,host,"host");
        OBS_LEFT_EXTENT(SolMirRuntimeHostOperation,import_id,"import_id");
        OBS_LEFT_EXTENT(SolMirRuntimeHostOperation,signature,"signature");
        OBS_LEFT_EXTENT(SolMirRuntimeHostOperation,receiver,"receiver");
        OBS_LEFT_EXTENT(SolMirRuntimeHostOperation,receiver_access,"receiver_access");
        OBS_NESTED_LEFT_EXTENT(SolMirRuntimeHostOperation,formals,SolMirRuntimeSlice,offset,"formals.offset");
        OBS_NESTED_LEFT_EXTENT(SolMirRuntimeHostOperation,formals,SolMirRuntimeSlice,count,"formals.count");
        OBS_LEFT_EXTENT(SolMirRuntimeHostOperation,result,"result");
        OBS_LEFT_EXTENT(SolMirRuntimeHostOperation,result_class,"result_class");
        OBS_LEFT_EXTENT(SolMirRuntimeHostOperation,result_plan,"result_plan");
        OBS_LEFT_EXTENT(SolMirRuntimeHostOperation,effects,"effects");
        OBS_NESTED_LEFT_EXTENT(SolMirRuntimeHostOperation,identity,SolMirLinkageDigest,bytes,"identity.bytes");
        OBS_NESTED_LEFT_EXTENT(SolMirRuntimeHostOperation,symbol,SolMirLinkageSymbol,bytes,"symbol.bytes");
        OBS_LEFT_EXTENT(SolMirRuntimeHostOperation,failure_mask,"failure_mask");
    } else if (strstr(id,".usage.")) {
        OBS_LEFT_EXTENT(SolMirRuntimeHostAbiUsage,capabilities,"capabilities");
        OBS_LEFT_EXTENT(SolMirRuntimeHostAbiUsage,entry_roots,"entry_roots");
        OBS_LEFT_EXTENT(SolMirRuntimeHostAbiUsage,operations,"operations");
        OBS_LEFT_EXTENT(SolMirRuntimeHostAbiUsage,arguments,"arguments");
        OBS_LEFT_EXTENT(SolMirRuntimeHostAbiUsage,requirements,"requirements");
        OBS_LEFT_EXTENT(SolMirRuntimeHostAbiUsage,grants,"grants");
        OBS_LEFT_EXTENT(SolMirRuntimeHostAbiUsage,formals,"formals");
        OBS_LEFT_EXTENT(SolMirRuntimeHostAbiUsage,shapes,"shapes");
        OBS_LEFT_EXTENT(SolMirRuntimeHostAbiUsage,shape_cases,"shape_cases");
        OBS_LEFT_EXTENT(SolMirRuntimeHostAbiUsage,owned_bytes,"owned_bytes");
        OBS_LEFT_EXTENT(SolMirRuntimeHostAbiUsage,build_scratch_bytes,"build_scratch_bytes");
        OBS_LEFT_EXTENT(SolMirRuntimeHostAbiUsage,build_work,"build_work");
        OBS_LEFT_EXTENT(SolMirRuntimeHostAbiUsage,validation_scratch_bytes,"validation_scratch_bytes");
        OBS_LEFT_EXTENT(SolMirRuntimeHostAbiUsage,validation_work,"validation_work");
    } else {
        SolMirRuntimeHostAbiObservationContext *c=
            sol_mir_runtime_host_abi_observation_context;
        if (c) {
            c->capture_gap=true;
            if(!c->capture_gap_reason)c->capture_gap_reason="comparison extent map missing";
        }
    }
}
#undef OBS_EXTENT
#undef OBS_LEFT_EXTENT
#undef OBS_NESTED_LEFT_EXTENT

static void observation_emit_comparison(FILE *stream,
    const SolMirRuntimeHostAbiObservationContext *c) {
    fputs("H37\t{\"kind\":\"comparison\",\"identity\":",stream);
    observation_json_string(stream,c->comparison_id);
    if(c->comparison_operation){
        fputs(",\"operation\":",stream);
        observation_json_string(stream,c->comparison_operation);
    }
    fprintf(stream,",\"left_index\":\"%zu\",\"right_index\":",c->comparison_left_index);
    if(c->comparison_right_index!=SIZE_MAX)fprintf(stream,"\"%zu\"",c->comparison_right_index);
    else fputs("null",stream);
    fputs(",\"result\":",stream);
    if(c->comparison_result_available)fprintf(stream,"\"%d\"",c->comparison_result);
    else fputs("null",stream);
    if(c->comparison_equal_available)
        fprintf(stream,",\"equal\":%s",c->comparison_equal?"true":"false");
    fprintf(stream,",\"left_length\":\"%zu\",\"right_length\":",c->comparison_left_available?c->comparison_length:0);
    if(c->comparison_right_available)fprintf(stream,"\"%zu\"",c->comparison_length);
    else fputs("null",stream);
    fputs(",\"left_availability\":",stream);
    observation_json_string(stream,c->comparison_left_available?"captured":"unavailable");
    fputs(",\"right_availability\":",stream);
    observation_json_string(stream,c->comparison_right_available?"captured":"no_expected_object");
    fputs(",\"left_hex\":",stream);
    if(c->comparison_left_available){fputc('"',stream);for(size_t i=0;i<c->comparison_length;i++)fprintf(stream,"%02x",c->comparison_left[i]);fputc('"',stream);}else fputs("null",stream);
    fputs(",\"right_hex\":",stream);
    if(c->comparison_right_available){fputc('"',stream);for(size_t i=0;i<c->comparison_length;i++)fprintf(stream,"%02x",c->comparison_right[i]);fputc('"',stream);}else fputs("null",stream);
    fprintf(stream,",\"left_role\":");
    observation_json_string(stream,c->comparison_is_fieldwise?"actual_owner_row":"original_comparison_left");
    fputs(",\"right_role\":",stream);
    observation_json_string(stream,c->comparison_operation
        ?"original_expected_capability_object"
        :c->comparison_is_fieldwise?"no_expected_object":"original_comparison_right");
    fprintf(stream,",\"duplicate_rejection\":%s}\n",c->comparison_is_duplicate?"true":"false");
    observation_emit_extents(stream,c->comparison_id);
}

void sol_mir_runtime_host_abi_test_emit_comparison(FILE *stream,
    const SolMirRuntimeHostAbiObservationContext *context) {
    if(!stream||!context||!context->comparison_available)return;
    observation_emit_comparison(stream,context);
}

static void observation_emit(SolMirRuntimeHostAbiObservationContext *c) {
    bool failed=c->producer_outcome!=SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED;
    fputs("H37\t{\"kind\":\"begin\",\"schema\":\"H37-1\",\"scope\":\"host-abi-build\"}\n",stderr);
    if (failed && c->first_failure_id) {
        fputs("H37\t{\"kind\":\"failure\",\"id\":",stderr);
        observation_json_string(stderr,c->first_failure_id);
        fputs(",\"phase\":",stderr); observation_json_string(stderr,c->first_failure_phase);
        fputs(",\"propagation_link\":",stderr);
        observation_json_string(stderr,c->propagation_id);
        fputs(",\"validator_rejection\":",stderr);
        observation_json_string(stderr,c->validator_rejection_id);
        fputs(",\"original_branch\":",stderr);
        observation_json_string(stderr,c->first_failure_branch);
        fputs(",\"propagation_branch\":",stderr);
        observation_json_string(stderr,c->propagation_branch);
        fputs(",\"original_message\":",stderr);
        observation_json_string(stderr,c->first_failure_message
            ?c->first_failure_message:c->validator_rejection_message
                ?c->validator_rejection_message:c->producer_message);
        fputs("}\n",stderr);
    }
    for (size_t i=0;failed && i<c->value_count;i++) {
        const SolMirRuntimeHostAbiObservationValue *v=&c->values[i];
        fputs("H37\t{\"kind\":\"operand\",\"name\":",stderr);
        observation_json_string(stderr,v->name); fputs(",\"availability\":",stderr);
        observation_json_string(stderr,v->availability); fputs(",\"units\":",stderr);
        observation_json_string(stderr,v->units);
        if(v->value_available)fprintf(stderr,",\"value\":\"%zu\"}\n",v->value);
        else fputs(",\"value\":null}\n",stderr);
    }
    if (failed) {
        if (c->measurement_recorded) {
            fputs("H37\t{\"kind\":\"validator_result\",\"scope\":\"measurement\",\"outcome\":",stderr);
            observation_json_string(stderr,observation_outcome_name(c->measurement_outcome));
            fprintf(stderr,",\"outcome_value\":\"%d\",\"first_rejection\":",(int)c->measurement_outcome);
            observation_json_string(stderr,c->measurement_rejection_id); fputs("}\n",stderr);
        } else {
            fputs("H37\t{\"kind\":\"validator_result\",\"scope\":\"measurement\",\"availability\":\"not_reached\"}\n",stderr);
        }
        if (c->final_validation_recorded) {
            fputs("H37\t{\"kind\":\"validator_result\",\"scope\":\"final\",\"outcome\":",stderr);
            observation_json_string(stderr,observation_outcome_name(c->final_validation_outcome));
            fprintf(stderr,",\"outcome_value\":\"%d\",\"first_rejection\":",(int)c->final_validation_outcome);
            observation_json_string(stderr,c->final_validation_rejection_id); fputs("}\n",stderr);
        } else {
            fputs("H37\t{\"kind\":\"validator_result\",\"scope\":\"final\",\"availability\":\"not_reached\"}\n",stderr);
        }
    }
    if (failed && c->comparison_available)
        observation_emit_comparison(stderr,c);
    if (failed && c->fieldwise_available) {
        fputs("H37\t{\"kind\":\"comparison\",\"identity\":",stderr);
        observation_json_string(stderr,c->fieldwise_id);
        fprintf(stderr,",\"left_index\":\"%zu\",\"right_index\":null,\"result\":null,\"left_length\":\"%zu\",\"right_length\":null,\"left_availability\":\"captured\",\"right_availability\":\"no_expected_object\",\"left_hex\":\"",c->fieldwise_index,c->fieldwise_length);
        for(size_t i=0;i<c->fieldwise_length;i++)
            fprintf(stderr,"%02x",c->fieldwise_bytes[i]);
        fputs("\",\"right_hex\":null,\"left_role\":\"actual_owner_row\",\"right_role\":\"no_expected_object\",\"duplicate_rejection\":false}\n",stderr);
        observation_emit_extents(stderr,c->fieldwise_id);
    }
    if (ferror(stderr)) {
        c->capture_gap=true;
        if(!c->capture_gap_reason)c->capture_gap_reason="diagnostic stream write failed";
    }
    if (failed && c->capture_gap)
    {
        fputs("H37\t{\"kind\":\"capture_gap\",\"reason\":",stderr);
        observation_json_string(stderr,c->capture_gap_reason
            ?c->capture_gap_reason:"bounded diagnostic capture unavailable");
        fputs("}\n",stderr);
    }
    fputs("H37\t{\"kind\":\"end\",\"producer_outcome\":",stderr);
    observation_json_string(stderr,observation_outcome_name(c->producer_outcome));
    fputs(",\"first_failure\":",stderr);
    observation_json_string(stderr,c->first_failure_id);
    fputs("}\n",stderr);
    if (fflush(stderr)!=0) {
        c->capture_gap=true;
        if(!c->capture_gap_reason)c->capture_gap_reason="diagnostic stream flush failed";
    }
}
#endif

#ifndef SOL_MIR_PLAN_TEST_HOOKS
#define sol_mir_runtime_host_abi_observe_failure(...) ((void)0)
#define sol_mir_runtime_host_abi_observe_shared_failure(...) ((void)0)
#define sol_mir_runtime_host_abi_observe_propagation(...) ((void)0)
#define sol_mir_runtime_host_abi_observe_value(...) ((void)0)
#define sol_mir_runtime_host_abi_observe_unavailable_value(...) ((void)0)
#define sol_mir_runtime_host_abi_observe_index(...) ((void)0)
#define sol_mir_runtime_host_abi_observe_reject(id,expression) (expression)
#define sol_mir_runtime_host_abi_observe_ne(id,ln,left,rn,right) ((left)!=(right))
#define sol_mir_runtime_host_abi_observe_ge(id,ln,left,rn,right) ((left)>=(right))
#define sol_mir_runtime_host_abi_observe_gt(id,ln,left,rn,right) ((left)>(right))
#define sol_mir_runtime_host_abi_observe_le(id,ln,left,rn,right) ((left)<=(right))
#define sol_mir_runtime_host_abi_observe_compare_once(id,left,right,length,li,ri,eq) \
    memcmp((left),(right),(length))
#define sol_mir_runtime_host_abi_observe_compare_once_shared(pid,vid,left,right,length,li,ri,eq) \
    memcmp((left),(right),(length))
#endif

enum { HOST_FAILURE_MASK = UINT32_C(0x00008240) };

static bool add(size_t *a, size_t b) {
    if (b > SIZE_MAX - *a) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        sol_mir_runtime_host_abi_observe_failure(
            "H37.P.add.37.overflow","producer-arithmetic",
            "original checked addition rejected",NULL);
        if (a) sol_mir_runtime_host_abi_observe_value("add.left",
            "initialized","bytes-or-count",*a);
        sol_mir_runtime_host_abi_observe_value("add.right",
            "initialized","bytes-or-count",b);
        sol_mir_runtime_host_abi_observe_unavailable_value("add.destination",
            "not_written","sum");
#endif
        return false;
    }
    *a += b; return true;
}
static bool mul(size_t a, size_t b, size_t *out) {
    if (a && b > SIZE_MAX / a) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        sol_mir_runtime_host_abi_observe_failure(
            "H37.P.mul.38.overflow","producer-arithmetic",
            "original checked multiplication rejected",NULL);
        sol_mir_runtime_host_abi_observe_value("mul.left",
            "initialized","count",a);
        sol_mir_runtime_host_abi_observe_value("mul.right",
            "initialized","count",b);
        sol_mir_runtime_host_abi_observe_unavailable_value("mul.destination",
            "not_written","product");
#endif
        return false;
    }
    *out = a * b; return true;
}
static bool diagnostic(SolDiagnostics *d, const char *message) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    sol_mir_runtime_host_abi_observe_outcome_message(message);
#endif
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
#ifdef SOL_MIR_PLAN_TEST_HOOKS
static void *allocate(size_t count, size_t size,size_t owner_slot) {
#else
static void *allocate(size_t count, size_t size) {
#endif
    if (!count || count > SIZE_MAX / size) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        sol_mir_runtime_host_abi_observe_failure("H37.P.allocate.78.invalid_extent","producer-persistent-allocation","original allocation extent rejected",NULL);
        sol_mir_runtime_host_abi_observe_value("allocation.count","evaluated","records",count);
        sol_mir_runtime_host_abi_observe_value("allocation.element_size","initialized","bytes",size);
        sol_mir_runtime_host_abi_observe_value("allocation.owner_slot","initialized","index",owner_slot);
#endif
        return NULL;
    }
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    ++sol_mir_runtime_host_abi_internal_allocation_attempts;
#endif
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (fail_persistent || (fail_persistent_attempt
            && sol_mir_runtime_host_abi_internal_allocation_attempts
                == fail_persistent_attempt)) {
        sol_mir_runtime_host_abi_observe_failure("H37.P.allocate.85.injected_failure","producer-persistent-allocation","original allocation test hook returned null",NULL);
        sol_mir_runtime_host_abi_observe_value("allocation.count","evaluated","records",count);
        sol_mir_runtime_host_abi_observe_value("allocation.element_size","initialized","bytes",size);
        sol_mir_runtime_host_abi_observe_value("allocation.attempt","initialized","ordinal",sol_mir_runtime_host_abi_internal_allocation_attempts);
        sol_mir_runtime_host_abi_observe_value("allocation.owner_slot","initialized","index",owner_slot);
        return NULL;
    }
#endif
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    void *result=calloc(count,size);
    if (!result) {
        sol_mir_runtime_host_abi_observe_failure("H37.P.allocate.87.calloc_failure","producer-persistent-allocation","original calloc returned null",NULL);
        sol_mir_runtime_host_abi_observe_value("allocation.count","evaluated","records",count);
        sol_mir_runtime_host_abi_observe_value("allocation.element_size","initialized","bytes",size);
        sol_mir_runtime_host_abi_observe_value("allocation.attempt","initialized","ordinal",sol_mir_runtime_host_abi_internal_allocation_attempts);
        sol_mir_runtime_host_abi_observe_value("allocation.owner_slot","initialized","index",owner_slot);
    }
    return result;
#else
    return calloc(count,size);
#endif
}
#ifdef SOL_MIR_PLAN_TEST_HOOKS
#define HOST_ALLOC(count,size,slot) allocate(count,size,slot)
#else
#define HOST_ALLOC(count,size,slot) allocate(count,size)
#endif
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
    sol_mir_runtime_host_abi_observe_index(2,"shape.recipe",recipe);
    sol_mir_runtime_host_abi_observe_index(3,"shape.depth",depth);
    if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
    if (recipe >= r->recipe_count) {
        sol_mir_runtime_host_abi_observe_failure(
            "H37.P.count_shape.127.recipe_bounds","producer-shape-count",
            "original recipe bounds predicate rejected",NULL);
        sol_mir_runtime_host_abi_observe_value("recipe","evaluated","index",recipe);
        sol_mir_runtime_host_abi_observe_value("recipe_count","evaluated","count",r->recipe_count);
        return false;
    }
    if (depth > r->recipe_count) {
        sol_mir_runtime_host_abi_observe_failure(
            "H37.P.count_shape.127.depth_bounds","producer-shape-count",
            "original shape depth predicate rejected",NULL);
        sol_mir_runtime_host_abi_observe_value("depth","evaluated","levels",depth);
        sol_mir_runtime_host_abi_observe_value("recipe_count","evaluated","count",r->recipe_count);
        return false;
    }
    if (!r->recipes[recipe].inhabited) {
        sol_mir_runtime_host_abi_observe_failure(
            "H37.P.count_shape.127.uninhabited","producer-shape-count",
            "original inhabited predicate rejected",NULL);
        sol_mir_runtime_host_abi_observe_value("recipe","evaluated","index",recipe);
        return false;
    }
    if (!shape_kind(&r->recipes[recipe], &kind)) {
        sol_mir_runtime_host_abi_observe_failure(
            "H37.P.count_shape.128.shape_kind","producer-shape-count",
            "original shape kind lookup returned false",NULL);
        sol_mir_runtime_host_abi_observe_value("recipe","evaluated","index",recipe);
        return false;
    }
    if (!add(shapes, 1)) return false;
    if (kind != SOL_MIR_RUNTIME_HOST_ARGUMENT_OPTION && kind != SOL_MIR_RUNTIME_HOST_ARGUMENT_RESULT)
        return true;
    for (size_t i=0; i<r->recipes[recipe].variants.count; ++i) {
        sol_mir_runtime_host_abi_observe_index(4,"shape.variant",i);
        if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
        const SolMirRecipeVariant *v = &r->variants[r->recipes[recipe].variants.offset+i];
        if (v->fields.count > 1) {
            sol_mir_runtime_host_abi_observe_failure(
                "H37.P.count_shape.134.multiple_fields","producer-shape-count",
                "original variant field-count predicate rejected",NULL);
            sol_mir_runtime_host_abi_observe_value("variant_index","evaluated","index",i);
            sol_mir_runtime_host_abi_observe_value("variant.fields.count","evaluated","count",v->fields.count);
            return false;
        }
        if (!add(cases, 1)) return false;
        if (v->fields.count && !count_shape(r,
                r->fields[v->fields.offset].type,depth+1,shapes,cases,work))
            return false;
    }
    return true;
}
static bool append_shape(SolMirRuntimeHostAbi *o, SolMirRecipeId recipe, SolAccessMode access,
    unsigned char *active, size_t *out, SolMirRuntimeHostAbiWorkMeter *work) {
    const SolMirRepresentation *r=&o->conventions->concrete->representation;
    SolMirRuntimeHostArgumentKind kind;
    sol_mir_runtime_host_abi_observe_index(2,"shape.recipe",recipe);
    if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
    if (recipe >= r->recipe_count) {
        sol_mir_runtime_host_abi_observe_failure(
            "H37.P.append_shape.144.recipe_bounds","producer-shape-emission",
            "original recipe bounds predicate rejected",NULL);
        sol_mir_runtime_host_abi_observe_value("recipe","evaluated","index",recipe);
        sol_mir_runtime_host_abi_observe_value("recipe_count","evaluated","count",r->recipe_count);
        return false;
    }
    if (active[recipe]) {
        sol_mir_runtime_host_abi_observe_failure(
            "H37.P.append_shape.144.recursive_cycle","producer-shape-emission",
            "original active recursion predicate rejected",NULL);
        sol_mir_runtime_host_abi_observe_value("recipe","evaluated","index",recipe);
        return false;
    }
    if (o->shape_count >= o->shape_capacity) {
        sol_mir_runtime_host_abi_observe_failure(
            "H37.P.append_shape.144.shape_capacity","producer-shape-emission",
            "original shape capacity predicate rejected",NULL);
        sol_mir_runtime_host_abi_observe_value("shape_count","evaluated","count",o->shape_count);
        sol_mir_runtime_host_abi_observe_value("shape_capacity","evaluated","count",o->shape_capacity);
        return false;
    }
    if (!r->recipes[recipe].inhabited) {
        sol_mir_runtime_host_abi_observe_failure(
            "H37.P.append_shape.145.uninhabited","producer-shape-emission",
            "original inhabited predicate rejected",NULL);
        sol_mir_runtime_host_abi_observe_value("recipe","evaluated","index",recipe);
        return false;
    }
    if (!shape_kind(&r->recipes[recipe],&kind)) {
        sol_mir_runtime_host_abi_observe_failure(
            "H37.P.append_shape.145.shape_kind","producer-shape-emission",
            "original shape kind lookup returned false",NULL);
        sol_mir_runtime_host_abi_observe_value("recipe","evaluated","index",recipe);
        return false;
    }
    size_t id=o->shape_count++; o->shapes[id]=(SolMirRuntimeHostShape){recipe,kind,access,{o->shape_case_count,0}};
    if (kind==SOL_MIR_RUNTIME_HOST_ARGUMENT_OPTION || kind==SOL_MIR_RUNTIME_HOST_ARGUMENT_RESULT) {
        active[recipe]=1;
        /* Reserve the direct slice before appending children.  Recursing first
         * interleaves a child's cases into its parent's slice. */
        size_t direct = r->recipes[recipe].variants.count;
        if (direct > o->shape_case_capacity-o->shape_case_count) {
            sol_mir_runtime_host_abi_observe_failure(
                "H37.P.append_shape.152.case_capacity","producer-shape-emission",
                "original direct case capacity predicate rejected",NULL);
            sol_mir_runtime_host_abi_observe_value("direct_cases","evaluated","count",direct);
            sol_mir_runtime_host_abi_observe_value("shape_case_capacity","evaluated","count",o->shape_case_capacity);
            sol_mir_runtime_host_abi_observe_value("shape_case_count","evaluated","count",o->shape_case_count);
            active[recipe]=0; return false;
        }
        o->shape_case_count += direct;
        o->shapes[id].cases.count=direct;
        for (size_t i=0;i<r->recipes[recipe].variants.count;i++) {
            sol_mir_runtime_host_abi_observe_index(4,"shape.variant",i);
            if (!sol_mir_runtime_host_abi_work_tick(work)) { active[recipe]=0; return false; }
            const SolMirRecipeVariant *v=&r->variants[r->recipes[recipe].variants.offset+i];
            if (v->fields.count>1) {
                sol_mir_runtime_host_abi_observe_failure(
                    "H37.P.append_shape.158.multiple_fields","producer-shape-emission",
                    "original variant field-count predicate rejected",NULL);
                sol_mir_runtime_host_abi_observe_value("variant_index","evaluated","index",i);
                sol_mir_runtime_host_abi_observe_value("variant.fields.count","evaluated","count",v->fields.count);
                active[recipe]=0; return false;
            }
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
#define E3_FAIL(atom, branch) do { \
    sol_mir_runtime_host_abi_observe_shared_failure( \
        "H37.P.e3_host_requirement." atom,"H37.V.e3_host_requirement." atom, \
        "producer-E3-predicate","validator-E3-predicate",branch,NULL); \
    sol_mir_runtime_host_abi_observe_value("host_index","evaluated","index",host_id); \
    return false; \
} while (0)
    const SolMirLinkage *l=&o->conventions->concrete->linkage;
    const SolMirMaterialization *m=&o->conventions->concrete->materialization;
    const SolIr *ir=o->conventions->concrete->program.ir;
    const SolMirRepresentation *r=&o->conventions->concrete->representation;
    sol_mir_runtime_host_abi_observe_index(2,"host_index",host_id);
    if(!sol_mir_runtime_host_abi_work_tick(work)) return false;
    if(host_id>=l->host_requirement_count) E3_FAIL("176.host_bounds","host requirement index out of range");
    if(!ir) E3_FAIL("176.ir_null","selected concrete program has no IR");
    const SolMirLinkageHostRequirement *h=&l->host_requirements[host_id];
    size_t import_id=h->import;
    if(import_id>=m->import_count) {
        sol_mir_runtime_host_abi_observe_value("host.import","evaluated","index",import_id);
        sol_mir_runtime_host_abi_observe_value("import_count","evaluated","count",m->import_count);
        E3_FAIL("178.import_bounds","host import index out of range");
    }
    size_t effects_id=h->effects;
    if(effects_id>=m->effect_row_count) {
        sol_mir_runtime_host_abi_observe_value("host.effects","evaluated","index",effects_id);
        sol_mir_runtime_host_abi_observe_value("effect_row_count","evaluated","count",m->effect_row_count);
        E3_FAIL("178.effect_bounds","effect row index out of range");
    }
    size_t receiver_id=h->receiver;
    if(receiver_id>=r->recipe_count) {
        sol_mir_runtime_host_abi_observe_value("host.receiver","evaluated","index",receiver_id);
        sol_mir_runtime_host_abi_observe_value("recipe_count","evaluated","count",r->recipe_count);
        E3_FAIL("179.receiver_bounds","receiver recipe index out of range");
    }
    const SolMirMaterializedImport *im=&m->imports[import_id];
    size_t source_callable=im->source_callable;
    if(source_callable>=ir->callable_count) {
        sol_mir_runtime_host_abi_observe_value("import.source_callable","evaluated","index",source_callable);
        sol_mir_runtime_host_abi_observe_value("callable_count","evaluated","count",ir->callable_count);
        E3_FAIL("181.source_callable_bounds","source callable index out of range");
    }
    const SolIrCallable *callable=&ir->callables[source_callable];
    size_t owner=callable->owner;
    if(owner>=ir->definition_count) {
        sol_mir_runtime_host_abi_observe_value("callable.owner","evaluated","index",owner);
        sol_mir_runtime_host_abi_observe_value("definition_count","evaluated","count",ir->definition_count);
        E3_FAIL("183.owner_bounds","callable owner index out of range");
    }
    if(callable->kind!=SOL_IR_CALLABLE_CAPABILITY) E3_FAIL("184.callable_kind","callable is not a capability");
    if(callable->body!=SOL_IR_NONE) E3_FAIL("184.callable_body","capability callable unexpectedly has a body");
    if(callable->generic_parameters.count) E3_FAIL("185.generic_parameters","capability callable has generic parameters");
    if(callable->effect_parameters.count) E3_FAIL("185.effect_parameters","capability callable has effect parameters");
    if(callable->result_authority_kind!=SOL_IR_AUTHORITY_NONE) E3_FAIL("186.result_authority","capability callable has result authority");
    if(h->semantic_id.high!=ir->definitions[owner].semantic_id.high) E3_FAIL("187.semantic_high","semantic identity high word differs");
    if(h->semantic_id.low!=ir->definitions[owner].semantic_id.low) E3_FAIL("188.semantic_low","semantic identity low word differs");
    if(h->receiver_access!=SOL_ACCESS_SHARED) E3_FAIL("189.receiver_access","receiver access is not shared");
    if(r->recipes[receiver_id].kind!=SOL_MIR_RECIPE_CAPABILITY) E3_FAIL("190.receiver_kind","receiver recipe is not a capability");
    if(r->recipes[receiver_id].concrete_definition!=owner) E3_FAIL("191.receiver_definition","receiver definition differs from callable owner");
    const SolMirMaterializedEffectRow *row=&m->effect_rows[effects_id];
    size_t atom_count=row->atoms.count;
    if(sol_mir_runtime_host_abi_observe_ne(
            "H37.P.e3_host_requirement.193.effect_atom_count",
            "effect_row.atoms.count",atom_count,"expected.atom_count",1)){
        sol_mir_runtime_host_abi_observe_unavailable_value(
            "effect_row.atoms.offset","not_reached","index");
        E3_FAIL("193.effect_atom_count","effect row does not contain exactly one atom");
    }
    size_t atom_offset=row->atoms.offset;
    if(sol_mir_runtime_host_abi_observe_ge(
            "H37.P.e3_host_requirement.193.effect_atom_offset",
            "effect_row.atoms.offset",atom_offset,"effect_row_atom_count",
            m->effect_row_atom_count)){
        E3_FAIL("193.effect_atom_offset","effect atom slice offset is out of range");
    }
    size_t atom_id=m->effect_row_atoms[row->atoms.offset];
    if(atom_id>=m->effect_atom_count) {
        sol_mir_runtime_host_abi_observe_value("effect_atom_id","evaluated","index",atom_id);
        sol_mir_runtime_host_abi_observe_value("effect_atom_count","evaluated","count",m->effect_atom_count);
        E3_FAIL("195.effect_atom_bounds","effect atom index is out of range");
    }
    const SolMirMaterializedEffectAtom *atom=&m->effect_atoms[atom_id];
    if(atom->authority!=SOL_MIR_PLAN_EFFECT_AUTHORITY_RECEIVER) E3_FAIL("197.effect_authority","effect authority is not receiver-derived");
    if(atom->name.offset>m->effect_name_count) E3_FAIL("198.effect_name_offset","effect name offset exceeds name arena");
    if(atom->name.count>m->effect_name_count-atom->name.offset) E3_FAIL("199.effect_name_length","effect name extent exceeds name arena");
    size_t parameter_offset=h->parameters.offset;
    if(parameter_offset>m->type_id_count) E3_FAIL("200.parameter_offset","parameter type offset exceeds type arena");
    size_t parameter_count=h->parameters.count;
    if(parameter_count>m->type_id_count-parameter_offset) E3_FAIL("201.parameter_count","parameter type extent exceeds type arena");
    size_t access_offset=h->parameter_accesses.offset;
    if(access_offset>m->access_count) E3_FAIL("202.access_offset","parameter access offset exceeds access arena");
    size_t access_count=h->parameter_accesses.count;
    if(access_count!=parameter_count) E3_FAIL("203.access_count_mismatch","parameter access count differs from type count");
    if(access_count>m->access_count-access_offset) E3_FAIL("204.access_extent","parameter access extent exceeds access arena");
    const char *capability=ir->definitions[callable->owner].name;
    const char *member=callable->name;
    const char *effect=NULL;
    SolMirRecipeKind parameter=SOL_MIR_RECIPE_INT64, result=SOL_MIR_RECIPE_INT64;
    size_t profile_parameter_count=0;
    if(text_is(capability,"Console") && text_is(member,"write")) {
        effect="console.write"; parameter=SOL_MIR_RECIPE_TEXT;
        result=SOL_MIR_RECIPE_UNIT; profile_parameter_count=1;
    } else if(text_is(capability,"Arguments") && text_is(member,"count")) {
        effect="process.arguments.count"; result=SOL_MIR_RECIPE_INT64;
    } else if(text_is(capability,"Arguments") && text_is(member,"get")) {
        effect="process.arguments.get"; parameter=SOL_MIR_RECIPE_INT64;
        result=SOL_MIR_RECIPE_OPTION; profile_parameter_count=1;
    } else if(text_is(capability,"Configuration") && text_is(member,"read")) {
        effect="configuration.read"; parameter=SOL_MIR_RECIPE_TEXT;
        result=SOL_MIR_RECIPE_OPTION; profile_parameter_count=1;
    } else E3_FAIL("222.profile","host requirement does not match a frozen capability profile");
    if(!effect_is(m,atom,effect)) E3_FAIL("223.effect_name","effect name differs from frozen profile");
    if(h->parameters.count!=profile_parameter_count) E3_FAIL("223.profile_parameter_count","parameter count differs from frozen profile");
    if(!recipe_kind_is(r,h->result,result)) E3_FAIL("224.profile_result_kind","result recipe kind differs from frozen profile");
    if(result==SOL_MIR_RECIPE_OPTION) {
        const SolMirRecipe *option=&r->recipes[h->result];
        bool text_payload=false;
        for(size_t i=0;i<option->variants.count;i++) {
            sol_mir_runtime_host_abi_observe_index(4,"option_variant",i);
            if(!sol_mir_runtime_host_abi_work_tick(work)) return false;
            const SolMirRecipeVariant *variant=&r->variants[option->variants.offset+i];
            if(variant->fields.count==1 && recipe_kind_is(r,
                    r->fields[variant->fields.offset].type,SOL_MIR_RECIPE_TEXT))
                text_payload=true;
            else if(variant->fields.count>1) {
                sol_mir_runtime_host_abi_observe_value("variant_index","evaluated","index",i);
                sol_mir_runtime_host_abi_observe_value("variant.fields.count","evaluated","count",variant->fields.count);
                E3_FAIL("234.option_variant_fields","option result variant has multiple fields");
            }
        }
        if(!text_payload) E3_FAIL("236.option_payload","option result has no text payload variant");
    }
    for(size_t i=0;i<h->parameters.count;i++) {
        sol_mir_runtime_host_abi_observe_index(4,"parameter_index",i);
        if(!sol_mir_runtime_host_abi_work_tick(work)) return false;
        SolMirRecipeId recipe=m->type_ids[parameter_offset+i];
        if(!recipe_kind_is(r,recipe,parameter)) {
            sol_mir_runtime_host_abi_observe_value("parameter_index","evaluated","index",i);
            sol_mir_runtime_host_abi_observe_value("parameter.recipe","evaluated","index",recipe);
            E3_FAIL("241.parameter_kind","parameter recipe kind differs from frozen profile");
        }
        SolAccessMode access=m->accesses[access_offset+i];
        if(access!=SOL_ACCESS_OWNED) {
            sol_mir_runtime_host_abi_observe_value("parameter_index","evaluated","index",i);
            sol_mir_runtime_host_abi_observe_value("parameter.access","evaluated","enum",access);
            E3_FAIL("242.parameter_access","parameter access is not owned");
        }
    }
#undef E3_FAIL
    return true;
}
static const SolMirRuntimeImport *find_import(const SolMirRuntimeConventions *c,size_t host,
    size_t *id, SolMirRuntimeHostAbiWorkMeter *work) {
    for(size_t i=0;i<c->import_count;i++) {
        sol_mir_runtime_host_abi_observe_index(4,"import_index",i);
        if (!sol_mir_runtime_host_abi_work_tick(work)) return NULL;
        if(c->imports[i].kind==SOL_MIR_RUNTIME_IMPORT_HOST&&c->imports[i].host==host){if(id)*id=i;return &c->imports[i];}
    } return NULL;
}
static const SolMirRuntimeSignature *find_signature(const SolMirRuntimeConventions *c,size_t host,
    size_t *id, SolMirRuntimeHostAbiWorkMeter *work) {
    for(size_t i=0;i<c->signature_count;i++) {
        sol_mir_runtime_host_abi_observe_index(4,"signature_index",i);
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
#define FROZEN_FAIL(atom, branch) do { \
    sol_mir_runtime_host_abi_observe_shared_failure( \
        "H37.P.frozen_e6_surface." atom,"H37.V.frozen_e6_surface." atom, \
        "producer-frozen-surface","validator-frozen-surface",branch,NULL); \
    return false; \
} while (0)
    const SolMirLinkage *l=&o->conventions->concrete->linkage;
    const SolMirRuntimeConventions *c=o->conventions;
    unsigned seen=0;
    if (l->host_requirement_count>4) {
        sol_mir_runtime_host_abi_observe_value("host_requirement_count","evaluated","count",l->host_requirement_count);
        FROZEN_FAIL("285.host_requirement_limit","host requirement count exceeds frozen E6 maximum");
    }
    size_t imports=0;
    for (size_t i=0;i<c->import_count;i++) {
        sol_mir_runtime_host_abi_observe_index(4,"import_index",i);
        if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
        if (c->imports[i].kind==SOL_MIR_RUNTIME_IMPORT_HOST) ++imports;
    }
    if (imports!=l->host_requirement_count) {
        sol_mir_runtime_host_abi_observe_value("host_import_count","evaluated","count",imports);
        sol_mir_runtime_host_abi_observe_value("host_requirement_count","evaluated","count",l->host_requirement_count);
        FROZEN_FAIL("291.host_import_count","host import count differs from requirement count");
    }
    for (size_t host=0;host<l->host_requirement_count;host++) {
        sol_mir_runtime_host_abi_observe_index(0,"host_index",host);
        if (!sol_mir_runtime_host_abi_work_tick(work) || !e3_host_requirement(o,host,work))
            return false;
        int kind=frozen_profile_kind(o,host);
        if (kind<0) {
            sol_mir_runtime_host_abi_observe_value("host_index","evaluated","index",host);
            FROZEN_FAIL("296.profile_kind","host requirement has no frozen profile kind");
        }
        if (seen&(1u<<kind)) {
            sol_mir_runtime_host_abi_observe_value("host_index","evaluated","index",host);
            sol_mir_runtime_host_abi_observe_value("profile_kind","evaluated","enum",(size_t)kind);
            sol_mir_runtime_host_abi_observe_value("seen_profiles","initialized","bitset",seen);
            FROZEN_FAIL("296.duplicate_profile","frozen profile kind is duplicated");
        }
        seen|=1u<<kind;
        size_t import_count=0, signature_count=0;
        for (size_t i=0;i<c->import_count;i++) {
            sol_mir_runtime_host_abi_observe_index(4,"import_index",i);
            if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
            if (c->imports[i].kind==SOL_MIR_RUNTIME_IMPORT_HOST && c->imports[i].host==host)
                ++import_count;
        }
        for (size_t i=0;i<c->signature_count;i++) {
            sol_mir_runtime_host_abi_observe_index(4,"signature_index",i);
            if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
            if (c->signatures[i].origin==SOL_MIR_RUNTIME_SIGNATURE_HOST
                && c->signatures[i].host==host) ++signature_count;
        }
        if (import_count!=1) {
            sol_mir_runtime_host_abi_observe_value("host_index","evaluated","index",host);
            sol_mir_runtime_host_abi_observe_value("matching_import_count","evaluated","count",import_count);
            FROZEN_FAIL("309.import_count","host requirement does not have exactly one import");
        }
        if (signature_count!=1) {
            sol_mir_runtime_host_abi_observe_value("host_index","evaluated","index",host);
            sol_mir_runtime_host_abi_observe_value("matching_signature_count","evaluated","count",signature_count);
            FROZEN_FAIL("309.signature_count","host requirement does not have exactly one signature");
        }
        for (size_t prior=0;prior<host;prior++) {
            sol_mir_runtime_host_abi_observe_index(1,"prior_host_index",prior);
            if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
            const SolMirRuntimeImport *left=find_import(c,prior,NULL,work);
            const SolMirRuntimeImport *right=find_import(c,host,NULL,work);
            if (!left || !right) {
                sol_mir_runtime_host_abi_observe_value("prior_host_index","evaluated","index",prior);
                sol_mir_runtime_host_abi_observe_value("host_index","evaluated","index",host);
                FROZEN_FAIL("314.import_lookup","host import lookup returned no compared import");
            }
            if (!sol_mir_runtime_host_abi_observe_compare_once_shared(
                    "H37.P.frozen_e6_surface.314.identity",
                    "H37.V.frozen_e6_surface.314.identity",&left->identity,
                    &right->identity,sizeof(left->identity),prior,host,true)
                || !sol_mir_runtime_host_abi_observe_compare_once_shared(
                    "H37.P.frozen_e6_surface.315.symbol",
                    "H37.V.frozen_e6_surface.315.symbol",&left->symbol,
                    &right->symbol,sizeof(left->symbol),prior,host,true)) FROZEN_FAIL("315.duplicate_symbol","frozen host symbol identity is duplicated");
        }
    }
#undef FROZEN_FAIL
    return true;
}
static size_t image_for_instance(const SolMirMaterialization *m, size_t instance,
    SolMirRuntimeHostAbiWorkMeter *work) {
    for(size_t i=0;i<m->image_count;i++) {
        sol_mir_runtime_host_abi_observe_index(6,"image_index",i);
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
    SolMirMaterializedPlaceId *place, SolMirRuntimeHostAbiWorkMeter *work,
    bool terminal_failure) {
    size_t found=SOL_MIR_RUNTIME_NONE;
    if (temporary==SOL_MIR_RUNTIME_NONE) {
        if (terminal_failure) sol_mir_runtime_host_abi_observe_failure(
            "H37.P.temporary_coordinate.351.none","producer-coordinate",
            "original missing temporary sentinel rejected",NULL);
        if (terminal_failure) sol_mir_runtime_host_abi_observe_value("temporary","evaluated","index",temporary);
        return false;
    }
    for (size_t i=0;i<m->instruction_count;i++) {
        sol_mir_runtime_host_abi_observe_index(6,"instruction_index",i);
        if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
        const SolMirMaterializedInstruction *x=&m->instructions[i];
        if (x->kind!=SOL_MIR_INST_TEMPORARY_INIT || x->temporary!=temporary) continue;
        if (found!=SOL_MIR_RUNTIME_NONE) {
            if (terminal_failure) sol_mir_runtime_host_abi_observe_failure(
                "H37.P.temporary_coordinate.356.duplicate","producer-coordinate",
                "original duplicate temporary initialization rejected",NULL);
            if (terminal_failure) sol_mir_runtime_host_abi_observe_value("temporary","evaluated","index",temporary);
            if (terminal_failure) sol_mir_runtime_host_abi_observe_value("first_instruction_index","evaluated","index",found);
            if (terminal_failure) sol_mir_runtime_host_abi_observe_value("duplicate_instruction_index","evaluated","index",i);
            return false;
        }
        found=x->left;
    }
    if (found>=m->value_count) {
        if (terminal_failure) sol_mir_runtime_host_abi_observe_failure(
            "H37.P.temporary_coordinate.359.value_bounds","producer-coordinate",
            "original materialized value bounds predicate rejected",NULL);
        if (terminal_failure) sol_mir_runtime_host_abi_observe_value("value","evaluated","index",found);
        if (terminal_failure) sol_mir_runtime_host_abi_observe_value("value_count","evaluated","count",m->value_count);
        return false;
    }
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
        sol_mir_runtime_host_abi_observe_index(6,"instruction_index",i);
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
    SolMirMaterializedLocalId local, SolMirRuntimeHostAbiWorkMeter *work,
    bool terminal_no_root) {
    const SolMirMaterialization *m=&o->conventions->concrete->materialization;
    if (local>=m->local_count) {
        if (terminal_no_root) {
            sol_mir_runtime_host_abi_observe_failure("H37.P.entry_root_for_local.387.local_bounds","producer-provenance","original local index rejected",NULL);
            sol_mir_runtime_host_abi_observe_value("local_index","evaluated","index",local);
            sol_mir_runtime_host_abi_observe_value("local_count","evaluated","count",m->local_count);
        }
        return SOL_MIR_RUNTIME_NONE;
    }
    for (size_t root=0;root<o->entry_root_count;root++) {
        sol_mir_runtime_host_abi_observe_index(5,"root_index",root);
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
            sol_mir_runtime_host_abi_observe_index(6,"image_local_index",i);
            if (!sol_mir_runtime_host_abi_work_tick(work)) return SOL_MIR_RUNTIME_NONE;
            size_t candidate=x->locals.offset+i;
            if (candidate==local && m->locals[candidate].kind==SOL_MIR_MATERIALIZED_LOCAL_PARAMETER
                && m->locals[candidate].ordinal==r->formal) return root;
        }
    }
    if (terminal_no_root) {
        sol_mir_runtime_host_abi_observe_failure("H37.P.entry_root_for_local.405.no_root","producer-provenance","original entry-root search returned none",NULL);
        sol_mir_runtime_host_abi_observe_value("local_index","evaluated","index",local);
    }
    return SOL_MIR_RUNTIME_NONE;
}

typedef enum {
    PRODUCER_LINEAGE_OK,
    PRODUCER_LINEAGE_REJECTED,
    PRODUCER_LINEAGE_EXHAUSTED
} ProducerLineageResult;

typedef struct {
    bool is_construction;
    size_t parent;
    size_t root;
    size_t constructor;
} ProducerLineageOrigin;

static ProducerLineageResult producer_lineage_tick(
    SolMirRuntimeHostAbiWorkMeter *meter) {
    return sol_mir_runtime_host_abi_work_tick(meter)
        ? PRODUCER_LINEAGE_OK : PRODUCER_LINEAGE_EXHAUSTED;
}

static bool producer_lineage_slice(size_t offset, size_t count, size_t total,
    size_t index) {
    return offset <= total && count <= total - offset
        && index >= offset && index - offset < count;
}

static bool producer_lineage_view_storage_valid(
    const SolMirRuntimeHostAbi *owner, const SolMirMaterialization *m) {
    if (!owner || !owner->conventions || !owner->conventions->concrete || !m)
        return false;
    const SolMirRuntimeConventions *conventions=owner->conventions;
    const SolMirConcreteProgram *concrete=conventions->concrete;
    const SolMirOperations *operations=&concrete->operations;
    const SolMirLinkage *linkage=&concrete->linkage;
    const SolIr *ir=concrete->program.ir;
#define STORAGE(array,count) ((count)==0 || (array)!=NULL)
    bool valid=STORAGE(m->images,m->image_count)
        &&STORAGE(m->types,m->type_count)&&STORAGE(m->locals,m->local_count)
        &&STORAGE(m->places,m->place_count)
        &&STORAGE(m->projections,m->projection_count)
        &&STORAGE(m->values,m->value_count)
        &&STORAGE(m->instructions,m->instruction_count)
        &&STORAGE(m->temporaries,m->temporary_count)
        &&STORAGE(m->blocks,m->block_count)
        &&STORAGE(m->loops,m->loop_count)
        &&STORAGE(operations->constructors,operations->constructor_count)
        &&STORAGE(operations->construct_operands,
            operations->construct_operand_count)
        &&STORAGE(owner->entry_roots,owner->entry_root_count)
        &&STORAGE(conventions->entries,conventions->entry_count)
        &&STORAGE(linkage->callables,linkage->callable_count)
        &&ir!=NULL&&STORAGE(ir->locals,ir->local_count);
#undef STORAGE
    return valid&&m->image_count<=m->image_capacity
        &&m->type_count<=m->type_capacity&&m->local_count<=m->local_capacity
        &&m->place_count<=m->place_capacity
        &&m->projection_count<=m->projection_capacity
        &&m->value_count<=m->value_capacity
        &&m->instruction_count<=m->instruction_capacity
        &&m->temporary_count<=m->temporary_capacity
        &&m->block_count<=m->block_capacity
        &&m->loop_count<=m->loop_capacity
        &&operations->constructor_count<=operations->constructor_capacity
        &&operations->construct_operand_count
            <=operations->construct_operand_capacity;
}

static ProducerLineageResult producer_lineage_instruction(
    const SolMirMaterialization *m, const SolMirMaterializedImage *image,
    size_t index, SolMirRuntimeHostAbiWorkMeter *meter,
    const SolMirMaterializedInstruction **out) {
    if (!m->instructions||!producer_lineage_slice(image->instructions.offset,
            image->instructions.count, m->instruction_count, index))
        return PRODUCER_LINEAGE_REJECTED;
    ProducerLineageResult tick = producer_lineage_tick(meter);
    if (tick != PRODUCER_LINEAGE_OK) return tick;
    *out = &m->instructions[index];
    return PRODUCER_LINEAGE_OK;
}

static ProducerLineageResult producer_lineage_block(
    const SolMirMaterialization *m, const SolMirMaterializedImage *image,
    SolMirMaterializedBlockId id, SolMirRuntimeHostAbiWorkMeter *meter,
    const SolMirMaterializedBlock **out) {
    if (!m->blocks||image->blocks.offset > m->block_count
        || image->blocks.count > m->block_count - image->blocks.offset)
        return PRODUCER_LINEAGE_REJECTED;
    size_t found = SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; i < image->blocks.count; ++i) {
        ProducerLineageResult tick = producer_lineage_tick(meter);
        if (tick != PRODUCER_LINEAGE_OK) return tick;
        size_t candidate = image->blocks.offset + i;
        if (m->blocks[candidate].id != id) continue;
        if (found != SOL_MIR_RUNTIME_NONE) return PRODUCER_LINEAGE_REJECTED;
        found = candidate;
    }
    if (found == SOL_MIR_RUNTIME_NONE) return PRODUCER_LINEAGE_REJECTED;
    *out = &m->blocks[found];
    return PRODUCER_LINEAGE_OK;
}

static ProducerLineageResult producer_lineage_block_is_looped(
    const SolMirMaterialization *m,const SolMirMaterializedImage *image,
    SolMirMaterializedBlockId block,SolMirRuntimeHostAbiWorkMeter *meter,
    bool *is_looped) {
    if(image->loops.offset>m->loop_count
        ||image->loops.count>m->loop_count-image->loops.offset)
        return PRODUCER_LINEAGE_REJECTED;
    *is_looped=false;
    for(size_t i=0;i<image->loops.count;i++){
        ProducerLineageResult result=producer_lineage_tick(meter);
        if(result!=PRODUCER_LINEAGE_OK)return result;
        const SolMirMaterializedLoop *loop=&m->loops[image->loops.offset+i];
        if(loop->header==block||loop->condition==block
            ||loop->body==block||loop->backedge==block){
            *is_looped=true;return PRODUCER_LINEAGE_OK;
        }
    }
    return PRODUCER_LINEAGE_OK;
}

static ProducerLineageResult producer_lineage_value_definition(
    const SolMirMaterialization *m, const SolMirMaterializedImage *image,
    SolMirMaterializedValueId value_id, SolMirMaterializedBlockId block,
    size_t use_before, SolMirRuntimeHostAbiWorkMeter *meter,
    const SolMirMaterializedValue **value_out, size_t *instruction_out,
    const SolMirMaterializedInstruction **definition_out) {
    if (!m->values||value_id >= m->value_count) return PRODUCER_LINEAGE_REJECTED;
    ProducerLineageResult tick = producer_lineage_tick(meter);
    if (tick != PRODUCER_LINEAGE_OK) return tick;
    const SolMirMaterializedValue *value = &m->values[value_id];
    if (value->kind != SOL_MIR_VALUE_INSTRUCTION || value->block != block
        || value->instruction >= use_before
        || !producer_lineage_slice(image->values.offset, image->values.count,
            m->value_count, value_id)
        || !producer_lineage_slice(image->instructions.offset,
            image->instructions.count, m->instruction_count,
            value->instruction)
        || value->source_definition > SIZE_MAX - image->instructions.offset
        || image->instructions.offset + value->source_definition
            != value->instruction)
        return PRODUCER_LINEAGE_REJECTED;
    const SolMirMaterializedInstruction *definition;
    tick = producer_lineage_instruction(m, image, value->instruction, meter,
        &definition);
    if (tick != PRODUCER_LINEAGE_OK) return tick;
    if (definition->result != value_id || definition->block != block)
        return PRODUCER_LINEAGE_REJECTED;
    const SolMirMaterializedBlock *block_row;
    tick=producer_lineage_block(m,image,block,meter,&block_row);
    if(tick!=PRODUCER_LINEAGE_OK)return tick;
    if(!producer_lineage_slice(block_row->instructions.offset,
            block_row->instructions.count,m->instruction_count,
            value->instruction))return PRODUCER_LINEAGE_REJECTED;
    size_t definitions=0,unique=SOL_MIR_RUNTIME_NONE;
    for(size_t candidate=0;candidate<m->instruction_count;candidate++){
        tick=producer_lineage_tick(meter);
        if(tick!=PRODUCER_LINEAGE_OK)return tick;
        if(m->instructions[candidate].result!=value_id)continue;
        if(++definitions>1)return PRODUCER_LINEAGE_REJECTED;
        unique=candidate;
    }
    if(definitions!=1||unique!=value->instruction)
        return PRODUCER_LINEAGE_REJECTED;
    *value_out = value;
    *instruction_out = value->instruction;
    *definition_out = definition;
    return PRODUCER_LINEAGE_OK;
}

static ProducerLineageResult producer_lineage_temporary_init(
    const SolMirMaterialization *m, const SolMirMaterializedImage *image,
    SolMirMaterializedTemporaryId temporary, SolMirRuntimeHostAbiWorkMeter *meter,
    size_t *instruction_out, SolMirMaterializedValueId *value_out) {
    if (!m->instructions||temporary == SOL_MIR_RUNTIME_NONE
        || !producer_lineage_slice(image->temporaries.offset,
            image->temporaries.count, m->temporary_count, temporary))
        return PRODUCER_LINEAGE_REJECTED;
    size_t found = SOL_MIR_RUNTIME_NONE;
    SolMirMaterializedValueId found_value = SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; i < m->instruction_count; ++i) {
        ProducerLineageResult tick = producer_lineage_tick(meter);
        if (tick != PRODUCER_LINEAGE_OK) return tick;
        const SolMirMaterializedInstruction *instruction = &m->instructions[i];
        if (instruction->kind != SOL_MIR_INST_TEMPORARY_INIT
            || instruction->temporary != temporary) continue;
        if (found != SOL_MIR_RUNTIME_NONE
            || !producer_lineage_slice(image->instructions.offset,
                image->instructions.count, m->instruction_count, i))
            return PRODUCER_LINEAGE_REJECTED;
        found = i;
        found_value = instruction->left;
    }
    if (found == SOL_MIR_RUNTIME_NONE || found_value >= m->value_count)
        return PRODUCER_LINEAGE_REJECTED;
    *instruction_out = found;
    *value_out = found_value;
    return PRODUCER_LINEAGE_OK;
}

static ProducerLineageResult producer_lineage_result_temporary(
    const SolMirMaterialization *m, const SolMirMaterializedImage *image,
    SolMirMaterializedValueId result, SolMirMaterializedBlockId block,
    size_t definition, size_t use_before, SolMirRuntimeHostAbiWorkMeter *meter,
    size_t *count_out, SolMirMaterializedTemporaryId *temporary_out) {
    if(!m->instructions)return PRODUCER_LINEAGE_REJECTED;
    size_t count = 0;
    size_t temporary = SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; i < m->instruction_count; ++i) {
        ProducerLineageResult tick = producer_lineage_tick(meter);
        if (tick != PRODUCER_LINEAGE_OK) return tick;
        const SolMirMaterializedInstruction *instruction = &m->instructions[i];
        if (instruction->kind != SOL_MIR_INST_TEMPORARY_INIT
            || instruction->left != result) continue;
        if (++count > 1
            || !producer_lineage_slice(image->instructions.offset,
                image->instructions.count, m->instruction_count, i)
            || instruction->block != block || i <= definition || i > use_before
            || !producer_lineage_slice(image->temporaries.offset,
                image->temporaries.count, m->temporary_count,
                instruction->temporary))
            return PRODUCER_LINEAGE_REJECTED;
        temporary = instruction->temporary;
    }
    *count_out = count;
    *temporary_out = temporary;
    return PRODUCER_LINEAGE_OK;
}

static ProducerLineageResult producer_lineage_local_root(
    const SolMirRuntimeHostAbi *owner, const SolMirMaterialization *m,
    SolMirMaterializedLocalId local_id, SolMirRuntimeHostAbiWorkMeter *meter,
    size_t *root_out) {
    const SolMirConcreteProgram *concrete = owner->conventions->concrete;
    const SolMirLinkage *linkage = &concrete->linkage;
    const SolIr *ir = concrete->program.ir;
    if (!m->locals||local_id >= m->local_count || !ir
        ||(owner->entry_root_count&&!owner->entry_roots)
        ||(owner->conventions->entry_count&&!owner->conventions->entries)
        ||(linkage->callable_count&&!linkage->callables))
        return PRODUCER_LINEAGE_REJECTED;
    ProducerLineageResult tick = producer_lineage_tick(meter);
    if (tick != PRODUCER_LINEAGE_OK) return tick;
    const SolMirMaterializedLocal *local = &m->locals[local_id];
    if (local->kind != SOL_MIR_MATERIALIZED_LOCAL_PARAMETER
        || local->source_local >= ir->local_count)
        return PRODUCER_LINEAGE_REJECTED;
    tick=producer_lineage_tick(meter);
    if(tick!=PRODUCER_LINEAGE_OK)return tick;
    if(ir->locals[local->source_local].kind!=SOL_IR_LOCAL_PARAMETER)
        return PRODUCER_LINEAGE_REJECTED;
    size_t found = SOL_MIR_RUNTIME_NONE;
    for (size_t root = 0; root < owner->entry_root_count; ++root) {
        tick = producer_lineage_tick(meter);
        if (tick != PRODUCER_LINEAGE_OK) return tick;
        const SolMirRuntimeHostEntryRoot *descriptor = &owner->entry_roots[root];
        if (descriptor->entry >= owner->conventions->entry_count) continue;
        tick=producer_lineage_tick(meter);
        if(tick!=PRODUCER_LINEAGE_OK)return tick;
        const SolMirRuntimeEntry *entry =
            &owner->conventions->entries[descriptor->entry];
        if (entry->callable >= linkage->callable_count) continue;
        tick=producer_lineage_tick(meter);
        if(tick!=PRODUCER_LINEAGE_OK)return tick;
        if (local->instance != linkage->callables[entry->callable].instance
            || local->ordinal != descriptor->formal) continue;
        if (found != SOL_MIR_RUNTIME_NONE) return PRODUCER_LINEAGE_REJECTED;
        found = root;
    }
    if (found == SOL_MIR_RUNTIME_NONE) return PRODUCER_LINEAGE_REJECTED;
    *root_out = found;
    return PRODUCER_LINEAGE_OK;
}

static ProducerLineageResult producer_lineage_place_local(
    const SolMirMaterialization *m, const SolMirMaterializedImage *image,
    SolMirMaterializedPlaceId place_id, SolMirPlanInstanceId instance,
    SolMirRuntimeHostAbiWorkMeter *meter, SolMirMaterializedLocalId *local_out,
    bool require_whole) {
    if (!m->places||!m->locals||place_id >= m->place_count)
        return PRODUCER_LINEAGE_REJECTED;
    ProducerLineageResult tick = producer_lineage_tick(meter);
    if (tick != PRODUCER_LINEAGE_OK) return tick;
    const SolMirMaterializedPlace *place = &m->places[place_id];
    if (place->instance != instance || place->local >= m->local_count
        || !producer_lineage_slice(image->locals.offset, image->locals.count,
            m->local_count, place->local)
        || place->projections.offset > m->projection_count
        || place->projections.count > m->projection_count-place->projections.offset
        || (require_whole && place->projections.count))
        return PRODUCER_LINEAGE_REJECTED;
    tick = producer_lineage_tick(meter);
    if (tick != PRODUCER_LINEAGE_OK) return tick;
    if (m->locals[place->local].instance != instance)
        return PRODUCER_LINEAGE_REJECTED;
    *local_out = place->local;
    return PRODUCER_LINEAGE_OK;
}

static ProducerLineageResult producer_lineage_invalidating_instruction(
    const SolMirMaterialization *m, const SolMirMaterializedImage *image,
    const SolMirMaterializedInstruction *instruction, SolMirPlanInstanceId instance,
    SolMirRuntimeHostAbiWorkMeter *meter, SolMirMaterializedLocalId local,
    bool *invalidates) {
    *invalidates = false;
    switch (instruction->kind) {
        case SOL_MIR_INST_STORAGE_LIVE:
        case SOL_MIR_INST_DROP_IF_INITIALIZED:
        case SOL_MIR_INST_STORAGE_DEAD:
            *invalidates = instruction->local == local;
            return PRODUCER_LINEAGE_OK;
        case SOL_MIR_INST_LOAD_MOVE:
        case SOL_MIR_INST_LOAD_UPDATE:
        case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED:
        case SOL_MIR_INST_COMPOUND_UPDATE: {
            SolMirMaterializedLocalId touched;
            ProducerLineageResult status = producer_lineage_place_local(m,image,
                instruction->place,instance,meter,&touched,false);
            if (status != PRODUCER_LINEAGE_OK) return status;
            *invalidates = touched == local;
            return PRODUCER_LINEAGE_OK;
        }
        default:
            return PRODUCER_LINEAGE_OK;
    }
}

static ProducerLineageResult producer_lineage_find_binding_store(
    const SolMirMaterialization *m, const SolMirMaterializedImage *image,
    SolMirPlanInstanceId instance, SolMirMaterializedBlockId block,
    SolMirMaterializedLocalId local, size_t load_instruction,
    SolMirMaterializedTypeId expected_type,
    SolMirRuntimeHostAbiWorkMeter *meter, size_t *store_out,
     SolMirMaterializedValueId *stored_value_out) {
    if (local >= m->local_count || !m->locals || !m->types
        || expected_type >= m->type_count)
        return PRODUCER_LINEAGE_REJECTED;
    ProducerLineageResult local_tick=producer_lineage_tick(meter);
    if(local_tick!=PRODUCER_LINEAGE_OK)return local_tick;
    if(m->locals[local].type!=expected_type)return PRODUCER_LINEAGE_REJECTED;
    size_t store = SOL_MIR_RUNTIME_NONE;
    SolMirMaterializedValueId stored_value = SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; i < image->instructions.count; ++i) {
        size_t index = image->instructions.offset + i;
        ProducerLineageResult tick = producer_lineage_tick(meter);
        if (tick != PRODUCER_LINEAGE_OK) return tick;
        const SolMirMaterializedInstruction *instruction = &m->instructions[index];
        if (instruction->kind != SOL_MIR_INST_STORE) continue;
        SolMirMaterializedLocalId touched;
        ProducerLineageResult status = producer_lineage_place_local(m,image,
            instruction->place,instance,meter,&touched,false);
        if (status != PRODUCER_LINEAGE_OK) return status;
        if (touched != local) continue;
        status=producer_lineage_tick(meter);
        if(status!=PRODUCER_LINEAGE_OK)return status;
        const SolMirMaterializedPlace *place=&m->places[instruction->place];
        if(instruction->type!=SOL_MIR_MATERIALIZED_NONE)
            return PRODUCER_LINEAGE_REJECTED;
        status=producer_lineage_tick(meter);
        if(status!=PRODUCER_LINEAGE_OK)return status;
        if(place->root_type!=expected_type)return PRODUCER_LINEAGE_REJECTED;
        status=producer_lineage_tick(meter);
        if(status!=PRODUCER_LINEAGE_OK)return status;
        if(place->final_type!=expected_type)
            return PRODUCER_LINEAGE_REJECTED;
        if (store != SOL_MIR_RUNTIME_NONE
            || m->places[instruction->place].projections.count
            || instruction->block != block)
            return PRODUCER_LINEAGE_REJECTED;
        store = index;
        stored_value = instruction->left;
    }
    if (store == SOL_MIR_RUNTIME_NONE || store >= load_instruction
        || stored_value >= m->value_count
        || !producer_lineage_slice(image->instructions.offset,
            image->instructions.count, m->instruction_count, load_instruction))
        return PRODUCER_LINEAGE_REJECTED;
    const SolMirMaterializedBlock *block_row;
    ProducerLineageResult status = producer_lineage_block(m,image,block,meter,
        &block_row);
    if (status != PRODUCER_LINEAGE_OK) return status;
    if (!producer_lineage_slice(block_row->instructions.offset,
            block_row->instructions.count, m->instruction_count, store)
        || !producer_lineage_slice(block_row->instructions.offset,
            block_row->instructions.count, m->instruction_count, load_instruction))
        return PRODUCER_LINEAGE_REJECTED;
    bool looped=false;
    status=producer_lineage_block_is_looped(m,image,block,meter,&looped);
    if(status!=PRODUCER_LINEAGE_OK)return status;
    if(looped)return PRODUCER_LINEAGE_REJECTED;

    /* A binding transfer is accepted only when this image has one live
     * storage epoch before the unique whole-local initialization store. */
    size_t live = SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; i < image->instructions.count; ++i) {
        size_t index = image->instructions.offset + i;
        if (index >= store) break;
        status = producer_lineage_tick(meter);
        if (status != PRODUCER_LINEAGE_OK) return status;
        const SolMirMaterializedInstruction *instruction = &m->instructions[index];
        if (instruction->kind != SOL_MIR_INST_STORAGE_LIVE
            || instruction->local != local) continue;
        if (live != SOL_MIR_RUNTIME_NONE || instruction->block != block)
            return PRODUCER_LINEAGE_REJECTED;
        live = index;
    }
    if (live == SOL_MIR_RUNTIME_NONE) return PRODUCER_LINEAGE_REJECTED;
    for (size_t index = live + 1; index < store; ++index) {
        const SolMirMaterializedInstruction *instruction;
        status = producer_lineage_instruction(m,image,index,meter,&instruction);
        if (status != PRODUCER_LINEAGE_OK) return status;
        bool invalidates = false;
        status = producer_lineage_invalidating_instruction(m,image,instruction,
            instance,meter,local,&invalidates);
        if (status != PRODUCER_LINEAGE_OK) return status;
        if (invalidates) return PRODUCER_LINEAGE_REJECTED;
    }

    status = producer_lineage_tick(meter);
    if (status != PRODUCER_LINEAGE_OK) return status;
    const SolMirMaterializedValue *stored_value_row;
    size_t stored_definition;
    const SolMirMaterializedInstruction *stored_instruction;
    status = producer_lineage_value_definition(m,image,stored_value,block,store,
        meter,&stored_value_row,&stored_definition,&stored_instruction);
    if (status != PRODUCER_LINEAGE_OK) return status;
    status=producer_lineage_tick(meter);
    if(status!=PRODUCER_LINEAGE_OK)return status;
    if (stored_value_row->type != expected_type
        || stored_definition >= store)
        return PRODUCER_LINEAGE_REJECTED;
    (void)stored_instruction;

    for (size_t index = store + 1; index < load_instruction; ++index) {
        const SolMirMaterializedInstruction *instruction;
        status = producer_lineage_instruction(m,image,index,meter,&instruction);
        if (status != PRODUCER_LINEAGE_OK) return status;
        bool invalidates = false;
        status = producer_lineage_invalidating_instruction(m,image,instruction,
            instance,meter,local,&invalidates);
        if (status != PRODUCER_LINEAGE_OK) return status;
        if (invalidates) return PRODUCER_LINEAGE_REJECTED;
    }
    *store_out = store;
    *stored_value_out = stored_value;
    return PRODUCER_LINEAGE_OK;
}

static ProducerLineageResult producer_lineage_origin(
    const SolMirRuntimeHostAbi *owner, const SolMirMaterialization *m,
    size_t constructor_index, SolMirRuntimeHostAbiWorkMeter *meter,
    ProducerLineageOrigin *origin) {
    const SolMirConcreteProgram *concrete = owner->conventions->concrete;
    const SolMirOperations *operations = &concrete->operations;
    const SolIr *ir = concrete->program.ir;
    if (constructor_index >= operations->constructor_count || !ir
        || !operations->constructors
        || (operations->construct_operand_count&&!operations->construct_operands))
        return PRODUCER_LINEAGE_REJECTED;
    ProducerLineageResult status = producer_lineage_tick(meter);
    if (status != PRODUCER_LINEAGE_OK) return status;
    const SolMirOperationConstructPlan *plan =
        &operations->constructors[constructor_index];
    if (plan->kind != SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY
        || plan->capability_source_operand >= plan->operands.count
        || plan->operands.offset > operations->construct_operand_count
        || plan->operands.count > operations->construct_operand_count-plan->operands.offset)
        return PRODUCER_LINEAGE_REJECTED;
    if(plan->image>=m->image_count)return PRODUCER_LINEAGE_REJECTED;
    status=producer_lineage_tick(meter);
    if(status!=PRODUCER_LINEAGE_OK)return status;
    const SolMirMaterializedImage *image=&m->images[plan->image];
    if (image->instructions.offset > m->instruction_count
        || image->instructions.count > m->instruction_count-image->instructions.offset
        || image->blocks.offset > m->block_count
        || image->blocks.count > m->block_count-image->blocks.offset
        || image->locals.offset > m->local_count
        || image->locals.count > m->local_count-image->locals.offset
        || image->values.offset > m->value_count
        || image->values.count > m->value_count-image->values.offset
        || image->temporaries.offset > m->temporary_count
        || image->temporaries.count > m->temporary_count-image->temporaries.offset)
        return PRODUCER_LINEAGE_REJECTED;
    const SolMirMaterializedInstruction *constructor_instruction;
    status = producer_lineage_instruction(m,image,plan->instruction,meter,
        &constructor_instruction);
    if (status != PRODUCER_LINEAGE_OK) return status;
    if (constructor_instruction->kind != SOL_MIR_INST_CONSTRUCT
        || constructor_instruction->construct_kind != SOL_MIR_CONSTRUCT_CAPABILITY
        || constructor_instruction->result != plan->result
        || plan->result >= m->value_count||plan->instruction==SIZE_MAX)
        return PRODUCER_LINEAGE_REJECTED;
    const SolMirMaterializedValue *constructor_value;
    size_t constructor_definition;
    const SolMirMaterializedInstruction *constructor_definition_row;
    status = producer_lineage_value_definition(m,image,plan->result,
        constructor_instruction->block,plan->instruction+1,meter,
        &constructor_value,&constructor_definition,&constructor_definition_row);
    if (status != PRODUCER_LINEAGE_OK) return status;
    if (constructor_definition != plan->instruction
        || constructor_value->type != constructor_instruction->type
        || constructor_definition_row->kind != SOL_MIR_INST_CONSTRUCT)
        return PRODUCER_LINEAGE_REJECTED;
    const SolMirMaterializedBlock *constructor_block;
    status = producer_lineage_block(m,image,constructor_instruction->block,meter,
        &constructor_block);
    if (status != PRODUCER_LINEAGE_OK) return status;
    if (!producer_lineage_slice(constructor_block->instructions.offset,
            constructor_block->instructions.count,m->instruction_count,
            plan->instruction)) return PRODUCER_LINEAGE_REJECTED;
    size_t operand_index = plan->operands.offset+plan->capability_source_operand;
    status = producer_lineage_tick(meter);
    if (status != PRODUCER_LINEAGE_OK) return status;
    const SolMirOperationConstructOperand *operand=
        &operations->construct_operands[operand_index];
    if(operand->temporary>=m->temporary_count)
        return PRODUCER_LINEAGE_REJECTED;
    size_t init_instruction;
    SolMirMaterializedValueId value_id;
    status = producer_lineage_temporary_init(m,image,operand->temporary,meter,
        &init_instruction,&value_id);
    if (status != PRODUCER_LINEAGE_OK) return status;
    if (init_instruction >= plan->instruction) return PRODUCER_LINEAGE_REJECTED;
    const SolMirMaterializedInstruction *init_row;
    status = producer_lineage_instruction(m,image,init_instruction,meter,&init_row);
    if (status != PRODUCER_LINEAGE_OK) return status;
    if (init_row->block != constructor_instruction->block
        || init_row->left != value_id) return PRODUCER_LINEAGE_REJECTED;

    const SolMirMaterializedValue *value;
    const SolMirMaterializedInstruction *definition;
    size_t value_instruction;
    status = producer_lineage_value_definition(m,image,value_id,
        constructor_instruction->block,init_instruction,meter,&value,
        &value_instruction,&definition);
    if (status != PRODUCER_LINEAGE_OK) return status;
    status=producer_lineage_tick(meter);
    if(status!=PRODUCER_LINEAGE_OK)return status;
    if (definition->type != value->type || definition->result != value_id
        ||m->temporaries[operand->temporary].type!=value->type)
        return PRODUCER_LINEAGE_REJECTED;
    (void)value_instruction;

    if (plan->capability_rule == SOL_MIR_OPERATION_CAPABILITY_ROOT_SOURCE) {
        if (definition->kind != SOL_MIR_INST_LOAD_COPY
            && definition->kind != SOL_MIR_INST_LOAD_MOVE)
            return PRODUCER_LINEAGE_REJECTED;
        SolMirMaterializedLocalId local_id;
        status = producer_lineage_place_local(m,image,definition->place,
            image->instance,meter,&local_id,true);
        if (status != PRODUCER_LINEAGE_OK) return status;
        if (local_id != plan->inherited_root) return PRODUCER_LINEAGE_REJECTED;
        size_t root;
        status = producer_lineage_local_root(owner,m,local_id,meter,&root);
        if (status != PRODUCER_LINEAGE_OK) return status;
        if (definition->kind == SOL_MIR_INST_LOAD_COPY) {
            if (value->type >= m->type_count) return PRODUCER_LINEAGE_REJECTED;
            status = producer_lineage_tick(meter);
            if (status != PRODUCER_LINEAGE_OK) return status;
            if (!m->types[value->type].is_copy) return PRODUCER_LINEAGE_REJECTED;
        }
        *origin=(ProducerLineageOrigin){false,root,root,SOL_MIR_RUNTIME_NONE};
        return PRODUCER_LINEAGE_OK;
    }
    if (plan->capability_rule != SOL_MIR_OPERATION_CAPABILITY_BASE_SOURCE)
        return PRODUCER_LINEAGE_REJECTED;

    SolMirMaterializedValueId current_value=value_id;
    size_t use_before=init_instruction;
    size_t transfers=0;
    for (size_t step=0;step<m->instruction_count;step++) {
        const SolMirMaterializedValue *current;
        const SolMirMaterializedInstruction *producer;
        size_t producer_index;
        status=producer_lineage_value_definition(m,image,current_value,
            constructor_instruction->block,use_before,meter,&current,
            &producer_index,&producer);
        if (status!=PRODUCER_LINEAGE_OK) return status;
        if (producer->kind==SOL_MIR_INST_CONSTRUCT) {
            size_t candidate=SOL_MIR_RUNTIME_NONE;
            size_t next_parent_id=owner->entry_root_count;
            size_t candidate_id=SOL_MIR_RUNTIME_NONE;
            for (size_t q=0;q<operations->constructor_count;q++) {
                status=producer_lineage_tick(meter);
                if (status!=PRODUCER_LINEAGE_OK) return status;
                const SolMirOperationConstructPlan *prior=&operations->constructors[q];
                bool touches=prior->result==current_value
                    ||prior->instruction==producer_index;
                if (touches) {
                    if (prior->result!=current_value
                        ||prior->instruction!=producer_index
                        ||prior->image!=plan->image
                        ||prior->kind!=SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY
                        ||q>=constructor_index||producer_index>=use_before
                        ||candidate!=SOL_MIR_RUNTIME_NONE)
                        return PRODUCER_LINEAGE_REJECTED;
                    candidate=q;
                    candidate_id=next_parent_id;
                }
                if (prior->kind==SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY
                    &&!add(&next_parent_id,1)) return PRODUCER_LINEAGE_REJECTED;
            }
            if (candidate==SOL_MIR_RUNTIME_NONE
                ||candidate_id==SOL_MIR_RUNTIME_NONE)
                return PRODUCER_LINEAGE_REJECTED;
            if(!transfers){
                size_t result_temp_count;
                SolMirMaterializedTemporaryId result_temp;
                status=producer_lineage_result_temporary(m,image,current_value,
                    constructor_instruction->block,producer_index,use_before,meter,
                    &result_temp_count,&result_temp);
                if (status!=PRODUCER_LINEAGE_OK) return status;
                if (result_temp_count!=1||result_temp!=operand->temporary)
                    return PRODUCER_LINEAGE_REJECTED;
            }
            *origin=(ProducerLineageOrigin){true,candidate_id,SOL_MIR_RUNTIME_NONE,
                candidate};
            return PRODUCER_LINEAGE_OK;
        }
        if (producer->kind!=SOL_MIR_INST_LOAD_COPY
            &&producer->kind!=SOL_MIR_INST_LOAD_MOVE)
            return PRODUCER_LINEAGE_REJECTED;
        SolMirMaterializedLocalId local_id;
        status=producer_lineage_place_local(m,image,producer->place,image->instance,
            meter,&local_id,true);
        if (status!=PRODUCER_LINEAGE_OK) return status;
        const SolMirMaterializedLocal *local=&m->locals[local_id];
        if (producer->type!=local->type||current->type!=local->type)
            return PRODUCER_LINEAGE_REJECTED;
        if (producer->kind==SOL_MIR_INST_LOAD_COPY) {
            if (local->type>=m->type_count) return PRODUCER_LINEAGE_REJECTED;
            status=producer_lineage_tick(meter);
            if (status!=PRODUCER_LINEAGE_OK) return status;
            if (!m->types[local->type].is_copy) return PRODUCER_LINEAGE_REJECTED;
        }
        if (local->kind==SOL_MIR_MATERIALIZED_LOCAL_PARAMETER) {
            size_t root;
            status=producer_lineage_local_root(owner,m,local_id,meter,&root);
            if (status!=PRODUCER_LINEAGE_OK) return status;
            *origin=(ProducerLineageOrigin){false,root,root,SOL_MIR_RUNTIME_NONE};
            return PRODUCER_LINEAGE_OK;
        }
        if (local->kind!=SOL_MIR_MATERIALIZED_LOCAL_BODY
            ||local->source_local>=ir->local_count)
            return PRODUCER_LINEAGE_REJECTED;
        status=producer_lineage_tick(meter);
        if (status!=PRODUCER_LINEAGE_OK) return status;
        const SolIrLocal *source_local=&ir->locals[local->source_local];
        if (source_local->kind!=SOL_IR_LOCAL_BINDING||source_local->mutable)
            return PRODUCER_LINEAGE_REJECTED;
        size_t store;
        SolMirMaterializedValueId stored_value;
        status=producer_lineage_find_binding_store(m,image,image->instance,
            constructor_instruction->block,local_id,producer_index,local->type,
            meter,&store,&stored_value);
        if (status!=PRODUCER_LINEAGE_OK) return status;
        current_value=stored_value;
        use_before=store;
        ++transfers;
    }
    return PRODUCER_LINEAGE_REJECTED;
}

static ProducerLineageResult producer_resolve_base_parent(
    const SolMirRuntimeHostAbi *owner, const SolMirMaterialization *materialization,
    size_t constructor_index, SolMirRuntimeHostAbiWorkMeter *meter,
    size_t *expected_parent, size_t *expected_root) {
    if (!owner || !owner->conventions || !owner->conventions->concrete
        || !materialization || !meter || !expected_parent || !expected_root
        ||!producer_lineage_view_storage_valid(owner,materialization))
        return PRODUCER_LINEAGE_REJECTED;
    const SolMirOperations *operations=&owner->conventions->concrete->operations;
    if (constructor_index>=operations->constructor_count
        ||!operations->constructors)
        return PRODUCER_LINEAGE_REJECTED;
    ProducerLineageResult status=producer_lineage_tick(meter);
    if (status!=PRODUCER_LINEAGE_OK) return status;
    const SolMirOperationConstructPlan *selected=&operations->constructors[constructor_index];
    if (selected->capability_rule!=SOL_MIR_OPERATION_CAPABILITY_BASE_SOURCE)
        return PRODUCER_LINEAGE_REJECTED;
    ProducerLineageOrigin origin;
    status=producer_lineage_origin(owner,materialization,constructor_index,meter,
        &origin);
    if (status!=PRODUCER_LINEAGE_OK) return status;
    size_t parent=origin.parent;
    size_t root=origin.root;
    if (origin.is_construction) {
        size_t cursor=origin.constructor;
        size_t ceiling=constructor_index;
        for (size_t depth=0;depth<operations->constructor_count;depth++) {
            if (cursor>=ceiling || cursor>=operations->constructor_count)
                return PRODUCER_LINEAGE_REJECTED;
            ceiling=cursor;
            status=producer_lineage_tick(meter);
            if (status!=PRODUCER_LINEAGE_OK) return status;
            const SolMirOperationConstructPlan *ancestor=&operations->constructors[cursor];
            if (ancestor->capability_rule==SOL_MIR_OPERATION_CAPABILITY_PRIVATE_SOURCE)
                return PRODUCER_LINEAGE_REJECTED;
            ProducerLineageOrigin next;
            status=producer_lineage_origin(owner,materialization,cursor,meter,&next);
            if (status!=PRODUCER_LINEAGE_OK) return status;
            if (!next.is_construction) {
                root=next.root;
                if (root==SOL_MIR_RUNTIME_NONE) return PRODUCER_LINEAGE_REJECTED;
                break;
            }
            cursor=next.constructor;
            if (depth+1==operations->constructor_count)
                return PRODUCER_LINEAGE_REJECTED;
        }
        if (root==SOL_MIR_RUNTIME_NONE) return PRODUCER_LINEAGE_REJECTED;
    }
    *expected_parent=parent;
    *expected_root=root;
    return PRODUCER_LINEAGE_OK;
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
SolMirRuntimeHostAbiLineageTestOutcome
sol_mir_runtime_host_abi_test_resolve_base_parent(
    const SolMirRuntimeHostAbi *owner, size_t constructor_index, size_t limit,
    size_t *parent, size_t *root, size_t *work_used) {
    SolMirRuntimeHostAbiWorkMeter meter={limit,0};
    size_t resolved_parent=SIZE_MAX,resolved_root=SIZE_MAX;
    ProducerLineageResult result=producer_resolve_base_parent(owner,
        owner&&owner->conventions&&owner->conventions->concrete
            ?&owner->conventions->concrete->materialization:NULL,
        constructor_index,&meter,&resolved_parent,&resolved_root);
    if (work_used) *work_used=meter.used;
    if (result==PRODUCER_LINEAGE_OK) {
        if (parent) *parent=resolved_parent;
        if (root) *root=resolved_root;
        return SOL_MIR_RUNTIME_HOST_ABI_LINEAGE_TEST_SUCCEEDED;
    }
    return result==PRODUCER_LINEAGE_EXHAUSTED
        ?SOL_MIR_RUNTIME_HOST_ABI_LINEAGE_TEST_EXHAUSTED
        :SOL_MIR_RUNTIME_HOST_ABI_LINEAGE_TEST_REJECTED;
}
#endif

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
    sol_mir_runtime_host_abi_observe_index(0,"entry_index",entry);
    if(entry>=o->conventions->entry_count) {
        sol_mir_runtime_host_abi_observe_failure("H37.P.mark_entry_images.418.entry_bounds","producer-image-closure","original entry index rejected",NULL);
        sol_mir_runtime_host_abi_observe_value("entry_index","evaluated","index",entry);
        sol_mir_runtime_host_abi_observe_value("entry_count","evaluated","count",o->conventions->entry_count);
        return false;
    }
    size_t callable=o->conventions->entries[entry].callable;
    if(callable>=l->callable_count) {
        sol_mir_runtime_host_abi_observe_failure("H37.P.mark_entry_images.418.callable_bounds","producer-image-closure","original entry callable index rejected",NULL);
        sol_mir_runtime_host_abi_observe_value("callable_index","evaluated","index",callable);
        sol_mir_runtime_host_abi_observe_value("callable_count","evaluated","count",l->callable_count);
        return false;
    }
    size_t start=image_for_instance(&o->conventions->concrete->materialization,
        l->callables[callable].instance,work);
    if(start==SOL_MIR_RUNTIME_NONE) {
        sol_mir_runtime_host_abi_observe_failure("H37.P.mark_entry_images.421.start_image","producer-image-closure","original entry image lookup returned none",NULL);
        sol_mir_runtime_host_abi_observe_value("entry_index","evaluated","index",entry);
        sol_mir_runtime_host_abi_observe_value("callable_index","evaluated","index",callable);
        return false;
    }
    images[start]=1;
    bool changed=true;
    while(changed){
        if(!sol_mir_runtime_host_abi_work_tick(work))return false;
        changed=false;
        for(size_t i=0;i<o->conventions->call_count;i++){
            sol_mir_runtime_host_abi_observe_index(1,"call_index",i);
            if(!sol_mir_runtime_host_abi_work_tick(work))return false;
            const SolMirRuntimeCall *c=&o->conventions->calls[i];
            if(!call_owner_reachable(o,entry,images,c))continue;
            size_t image=target_image(o,c,work);
            if(image!=SOL_MIR_RUNTIME_NONE&&!images[image]){images[image]=1;changed=true;}
        }
    }
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
        sol_mir_runtime_host_abi_observe_index(4,"constructor_index",i);
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
        return temporary_coordinate(m,temporary,&value,&place,work,false)
            &&exact_root_place(m,roots,root,place,work);
    }
    const SolMirOperationConstructPlan*p=&ops->constructors[found];
    if(p->capability_source_operand>=p->operands.count)return false;
    const SolMirOperationConstructOperand*source=&ops->construct_operands[p->operands.offset+p->capability_source_operand];
    SolMirMaterializedValueId value;SolMirMaterializedPlaceId place;
    if(!temporary_coordinate(m,source->temporary,&value,&place,work,false))return false;
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
        sol_mir_runtime_host_abi_observe_index(5,"root_index",root);
        if (!sol_mir_runtime_host_abi_work_tick(work)) return SOL_MIR_RUNTIME_NONE;
        if (o->entry_roots[root].entry!=entry
            || !operand_has_root(o,roots,root,operand,work)) continue;
        if (origin!=SOL_MIR_RUNTIME_NONE) {
            sol_mir_runtime_host_abi_observe_failure("H37.P.receiver_origin.497.ambiguous","producer-provenance","original duplicate receiver-origin predicate rejected",NULL);
            sol_mir_runtime_host_abi_observe_value("first_root_index","evaluated","index",origin);
            sol_mir_runtime_host_abi_observe_value("second_root_index","evaluated","index",root);
            return SOL_MIR_RUNTIME_NONE;
        }
        origin=root;
    }
    if(origin==SOL_MIR_RUNTIME_NONE){
        sol_mir_runtime_host_abi_observe_failure("H37.P.receiver_origin.500.none","producer-provenance","original receiver had no entry-root origin",NULL);
        sol_mir_runtime_host_abi_observe_value("entry_index","evaluated","index",entry);
    }
    return origin;
}
static bool closure_roots(const SolMirRuntimeHostAbi *o,size_t entry,const unsigned char *images,
    unsigned char *roots, SolMirRuntimeHostAbiWorkMeter *work) {
    const SolMirMaterialization*m=&o->conventions->concrete->materialization;
    sol_mir_runtime_host_abi_observe_index(0,"entry_index",entry);
    size_t image=image_for_instance(m,o->conventions->concrete->linkage.callables[
        o->conventions->entries[entry].callable].instance,work);
    if (image==SOL_MIR_RUNTIME_NONE) {
        sol_mir_runtime_host_abi_observe_failure("H37.P.closure_roots.507.entry_image","producer-provenance","original closure entry-image lookup returned none",NULL);
        sol_mir_runtime_host_abi_observe_value("entry_index","evaluated","index",entry);
        return false;
    }
    for(size_t r=0;r<o->entry_root_count;r++) {
        sol_mir_runtime_host_abi_observe_index(1,"root_index",r);
        if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
        if(o->entry_roots[r].entry!=entry) continue;
        for(size_t l=0;l<m->local_count;l++) {
            sol_mir_runtime_host_abi_observe_index(2,"local_index",l);
            if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
            if(m->locals[l].instance==m->images[image].instance
                &&m->locals[l].kind==SOL_MIR_MATERIALIZED_LOCAL_PARAMETER
                &&m->locals[l].ordinal==o->entry_roots[r].formal)
                roots[r*m->local_count+l]=1;
        }
    }
    bool changed=true;
    while(changed){
        if(!sol_mir_runtime_host_abi_work_tick(work))return false;
        changed=false;
        for(size_t ci=0;ci<o->conventions->call_count;ci++){
            sol_mir_runtime_host_abi_observe_index(1,"call_index",ci);
            if(!sol_mir_runtime_host_abi_work_tick(work))return false;
            const SolMirRuntimeCall*c=&o->conventions->calls[ci];
            if(!call_owner_reachable(o,entry,images,c))continue;
            size_t target=target_image(o,c,work);
            if(target==SOL_MIR_RUNTIME_NONE)continue;
            const SolMirMaterializedImage*im=&m->images[target];
            for(size_t oi=0;oi<c->operands.count;oi++){
                sol_mir_runtime_host_abi_observe_index(2,"operand_index",oi);
                if(!sol_mir_runtime_host_abi_work_tick(work))return false;
                const SolMirRuntimeOperand*operand=&o->conventions->operands[c->operands.offset+oi];
                if(operand->signature_slot>=o->conventions->signature_slot_count)continue;
                const SolMirRuntimeSignatureSlot*slot=&o->conventions->signature_slots[operand->signature_slot];
                if(slot->role!=SOL_MIR_RUNTIME_SLOT_PARAMETER)continue;
                for(size_t li=0;li<im->locals.count;li++){
                    sol_mir_runtime_host_abi_observe_index(3,"image_local_index",li);
                    if(!sol_mir_runtime_host_abi_work_tick(work))return false;
                    size_t local=im->locals.offset+li;
                    if(m->locals[local].kind!=SOL_MIR_MATERIALIZED_LOCAL_PARAMETER||m->locals[local].ordinal!=slot->formal)continue;
                    for(size_t r=0;r<o->entry_root_count;r++){
                        sol_mir_runtime_host_abi_observe_index(4,"root_index",r);
                        if(!sol_mir_runtime_host_abi_work_tick(work))return false;
                        if(o->entry_roots[r].entry==entry&&operand_has_root(o,roots,r,operand,work)&&!roots[r*m->local_count+local]){
                            roots[r*m->local_count+local]=1;changed=true;
                        }
                    }
                }
            }
        }
    }
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
    unsigned char *roots, size_t root_bits,
    ProducerLineageResult *lineage_result) {
    const SolMirRepresentation *r=&o->conventions->concrete->representation;
    const SolMirLinkage *l=&o->conventions->concrete->linkage;
    const SolMirMaterialization *m=&o->conventions->concrete->materialization;
    const SolMirOperations *ops=&o->conventions->concrete->operations;
    for (size_t e=0;e<o->conventions->entry_count;e++) {
        sol_mir_runtime_host_abi_observe_index(0,"entry_index",e);
        if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
        const SolMirRuntimeSignature *s=&o->conventions->signatures[
            o->conventions->entries[e].signature];
        for (size_t q=0;q<s->slots.count;q++) {
            sol_mir_runtime_host_abi_observe_index(1,"signature_slot_index",q);
            if(!sol_mir_runtime_host_abi_work_tick(work)) return false;
        }
    }
    for (size_t i=0;i<ops->constructor_count;i++) {
        sol_mir_runtime_host_abi_observe_index(0,"constructor_index",i);
        if(!sol_mir_runtime_host_abi_work_tick(work)) return false;
        else if (ops->constructors[i].kind==SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY) {
            const SolMirOperationConstructPlan *plan=&ops->constructors[i];
            SolMirMaterializedTemporaryId temporary;
            SolMirMaterializedValueId value;
            SolMirMaterializedPlaceId place;
            if (!result_temporary(m,plan->result,&temporary,work))
                temporary=SOL_MIR_RUNTIME_NONE;
            if (plan->capability_source_operand>=plan->operands.count
                ) {
                sol_mir_runtime_host_abi_observe_failure("H37.P.persistent_write_traversal.554.source_operand_bounds","producer-dry-write","original constructor source operand index rejected",NULL);
                sol_mir_runtime_host_abi_observe_value("constructor_source_operand","evaluated","index",plan->capability_source_operand);
                sol_mir_runtime_host_abi_observe_value("constructor_operand_count","evaluated","count",plan->operands.count);
                return false;
            }
            if (!temporary_coordinate(m,ops->construct_operands[
                    plan->operands.offset+plan->capability_source_operand].temporary,
                    &value,&place,work,true)) return false;
            if (plan->capability_rule==SOL_MIR_OPERATION_CAPABILITY_BASE_SOURCE) {
                size_t parent=SOL_MIR_RUNTIME_NONE,root=SOL_MIR_RUNTIME_NONE;
                ProducerLineageResult result=producer_resolve_base_parent(o,m,i,
                    work,&parent,&root);
                if (result!=PRODUCER_LINEAGE_OK) {
                    if (lineage_result) *lineage_result=result;
                    if (result==PRODUCER_LINEAGE_REJECTED)
                        sol_mir_runtime_host_abi_observe_failure(
                            "H37.P.persistent_write_traversal.560.base_lineage",
                            "producer-dry-write",
                            "original exact capability lineage rejected",NULL);
                    return false;
                }
                (void)parent;
                (void)root;
            }
            if (plan->capability_rule==SOL_MIR_OPERATION_CAPABILITY_ROOT_SOURCE) {
                size_t root=entry_root_for_local(o,plan->inherited_root,work,true);
                if(root==SOL_MIR_RUNTIME_NONE||place>=m->place_count
                    ||m->places[place].local!=plan->inherited_root
                    ||m->places[place].projections.count) {
                    sol_mir_runtime_host_abi_observe_failure("H37.P.persistent_write_traversal.563.root_source","producer-dry-write","original root capability source predicate rejected",NULL);
                    sol_mir_runtime_host_abi_observe_value("root_index","evaluated","index",root);
                    sol_mir_runtime_host_abi_observe_value("source_place","evaluated","index",place);
                    sol_mir_runtime_host_abi_observe_value("inherited_root","evaluated","index",plan->inherited_root);
                    return false;
                }
            }
        }
    }
    for (size_t h=0;h<l->host_requirement_count;h++) {
        sol_mir_runtime_host_abi_observe_index(0,"host_index",h);
        size_t shapes=0,cases=0;
        const SolMirLinkageHostRequirement *host=&l->host_requirements[h];
        if(!e3_host_requirement(o,h,work)) return false;
        if(!find_import(o->conventions,h,NULL,work)) {
            sol_mir_runtime_host_abi_observe_failure("H37.P.persistent_write_traversal.573.import_missing","producer-dry-write","original import lookup returned none",NULL);
            sol_mir_runtime_host_abi_observe_value("host_index","evaluated","index",h);
            return false;
        }
        if(!find_signature(o->conventions,h,NULL,work)) {
            sol_mir_runtime_host_abi_observe_failure("H37.P.persistent_write_traversal.574.signature_missing","producer-dry-write","original signature lookup returned none",NULL);
            sol_mir_runtime_host_abi_observe_value("host_index","evaluated","index",h);
            return false;
        }
        for (size_t q=0;q<host->parameters.count;q++) {
            sol_mir_runtime_host_abi_observe_index(1,"parameter_index",q);
            if(!sol_mir_runtime_host_abi_work_tick(work)
                || !count_shape(r,m->type_ids[host->parameters.offset+q],0,
                    &shapes,&cases,work)) return false;
        }
    }
    for (size_t e=0;e<o->conventions->entry_count;e++) {
        sol_mir_runtime_host_abi_observe_index(0,"entry_index",e);
        if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
        memset(images,0,m->image_count); memset(roots,0,root_bits);
        if(!mark_entry_images(o,e,images,work)
            ||!closure_roots(o,e,images,roots,work)) return false;
        for (size_t c=0;c<o->conventions->call_count;c++) {
            sol_mir_runtime_host_abi_observe_index(1,"call_index",c);
            const SolMirRuntimeCall *call=&o->conventions->calls[c];
            if(!sol_mir_runtime_host_abi_work_tick(work)) return false;
            if(!call_owner_reachable(o,e,images,call)
                ||target_host(o,call)==SOL_MIR_RUNTIME_NONE) continue;
            const SolMirRuntimeOperand *receiver=receiver_operand(o,call);
            if(!receiver) {
                sol_mir_runtime_host_abi_observe_failure("H37.P.persistent_write_traversal.591.receiver_missing","producer-dry-write","original host receiver lookup returned none",NULL);
                sol_mir_runtime_host_abi_observe_value("call_index","evaluated","index",c);
                return false;
            }
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
    if(call>=o->conventions->call_count){
        sol_mir_runtime_host_abi_observe_failure("H37.P.call_cleanup.602.call_bounds","producer-cleanup-mapping","original call index rejected",NULL);
        sol_mir_runtime_host_abi_observe_value("call_index","evaluated","index",call);
        sol_mir_runtime_host_abi_observe_value("call_count","evaluated","count",o->conventions->call_count);
        return false;
    }
    const SolMirRuntimeCall*c=&o->conventions->calls[call];
    SolMirRuntimeCleanupProducerKind producer=c->owner_kind==SOL_MIR_RUNTIME_CALL_OWNER_IMAGE?SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_INVOKE:SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_INVOKE;
    size_t found_event=SOL_MIR_RUNTIME_NONE,found_transition=SOL_MIR_RUNTIME_NONE;
    for(size_t i=0;i<o->cleanup->event_count;i++){
        sol_mir_runtime_host_abi_observe_index(4,"cleanup_event_index",i);
        if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
        const SolMirRuntimeCleanupEvent*e=&o->cleanup->events[i];
        if(e->producer!=producer||e->block!=c->block
            ||e->owner!=(c->owner_kind==SOL_MIR_RUNTIME_CALL_OWNER_IMAGE?c->image:c->predicate))continue;
        for(size_t q=0;q<e->transitions.count;q++){
            sol_mir_runtime_host_abi_observe_index(5,"cleanup_transition_index",q);
            if (!sol_mir_runtime_host_abi_work_tick(work)) return false;
            size_t t=e->transitions.offset+q;
            const SolMirRuntimeCleanupTransition *candidate=&o->cleanup->transitions[t];
            if(candidate->edge_role!=SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE
                ||candidate->failure_mask!=HOST_FAILURE_MASK
                ||candidate->failure_source!=SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31
                ||candidate->failure_site!=c->failure_site)continue;
            if(found_transition!=SOL_MIR_RUNTIME_NONE){
                sol_mir_runtime_host_abi_observe_failure("H37.P.call_cleanup.618.duplicate_transition","producer-cleanup-mapping","original duplicate cleanup transition rejected",NULL);
                sol_mir_runtime_host_abi_observe_value("first_transition_index","evaluated","index",found_transition);
                sol_mir_runtime_host_abi_observe_value("duplicate_transition_index","evaluated","index",t);
                return false;
            }
            found_event=i;found_transition=t;
        }
    }
    if(found_transition!=SOL_MIR_RUNTIME_NONE){*event=found_event;*transition=found_transition;return true;}
    sol_mir_runtime_host_abi_observe_failure("H37.P.call_cleanup.623.no_transition","producer-cleanup-mapping","original cleanup mapping search returned none",NULL);
    sol_mir_runtime_host_abi_observe_value("call_index","evaluated","index",call);
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
static bool fit(const SolMirRuntimeHostAbi *o){
    const SolMirRuntimeHostAbiUsage*u=&o->usage;
    const SolMirRuntimeHostAbiLimits*l=&o->limits;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
#define FIT_CHECK(member,id) do { \
    if (!sol_mir_runtime_host_abi_observe_le(id,"usage." #member,u->member, \
            "limit." #member,l->max_##member)) return false; \
} while (0)
    FIT_CHECK(capabilities,"H37.P.fit.635.capabilities");
    FIT_CHECK(entry_roots,"H37.P.fit.635.entry_roots");
    FIT_CHECK(operations,"H37.P.fit.635.operations");
    FIT_CHECK(arguments,"H37.P.fit.635.arguments");
    FIT_CHECK(formals,"H37.P.fit.635.formals");
    FIT_CHECK(shapes,"H37.P.fit.635.shapes");
    FIT_CHECK(shape_cases,"H37.P.fit.635.shape_cases");
    FIT_CHECK(requirements,"H37.P.fit.635.requirements");
    FIT_CHECK(grants,"H37.P.fit.635.grants");
    FIT_CHECK(owned_bytes,"H37.P.fit.635.owned_bytes");
    FIT_CHECK(build_scratch_bytes,"H37.P.fit.635.build_scratch_bytes");
    FIT_CHECK(build_work,"H37.P.fit.635.build_work");
    FIT_CHECK(validation_scratch_bytes,"H37.P.fit.635.validation_scratch_bytes");
    FIT_CHECK(validation_work,"H37.P.fit.635.validation_work");
#undef FIT_CHECK
    return true;
#else
    return u->capabilities<=l->max_capabilities&&u->entry_roots<=l->max_entry_roots&&u->operations<=l->max_operations&&u->arguments<=l->max_arguments&&u->formals<=l->max_formals&&u->shapes<=l->max_shapes&&u->shape_cases<=l->max_shape_cases&&u->requirements<=l->max_requirements&&u->grants<=l->max_grants&&u->owned_bytes<=l->max_owned_bytes&&u->build_scratch_bytes<=l->max_build_scratch_bytes&&u->build_work<=l->max_build_work&&u->validation_scratch_bytes<=l->max_validation_scratch_bytes&&u->validation_work<=l->max_validation_work;
#endif
}

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
#ifdef SOL_MIR_PLAN_TEST_HOOKS
static SolMirRuntimeHostAbiBuildOutcome host_abi_build_observed(
    const SolMirRuntimeHostAbiBuildRequest *request,SolMirRuntimeHostAbi *out,
    SolDiagnostics*d){
#else
SolMirRuntimeHostAbiBuildOutcome sol_mir_runtime_host_abi_build(
    const SolMirRuntimeHostAbiBuildRequest *request,SolMirRuntimeHostAbi *out,
    SolDiagnostics*d){
#endif
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    bool grants_available=false;
#endif
    if(sol_mir_runtime_host_abi_observe_reject("H37.P.sol_mir_runtime_host_abi_build.755.01.request_null",!request)
        ||sol_mir_runtime_host_abi_observe_reject("H37.P.sol_mir_runtime_host_abi_build.755.02.output_null",!out)
        ||sol_mir_runtime_host_abi_observe_reject("H37.P.sol_mir_runtime_host_abi_build.755.03.conventions_null",!request->conventions)
        ||sol_mir_runtime_host_abi_observe_reject("H37.P.sol_mir_runtime_host_abi_build.755.04.values_null",!request->values)
        ||sol_mir_runtime_host_abi_observe_reject("H37.P.sol_mir_runtime_host_abi_build.755.05.cleanup_null",!request->cleanup)
        ||sol_mir_runtime_host_abi_observe_reject("H37.P.sol_mir_runtime_host_abi_build.755.06.output_not_empty",!empty(out))
        ||sol_mir_runtime_host_abi_observe_reject("H37.P.sol_mir_runtime_host_abi_build.755.07.partial_limits",(request->limits&&!zero(*request->limits)&&!full(*request->limits)))){
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.755.request","producer-entry","original request rejection",NULL);
#endif
        diagnostic(d,"invalid runtime host ABI build request or destination");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_ARGUMENT;}
    if(sol_mir_runtime_host_abi_observe_reject("H37.P.sol_mir_runtime_host_abi_build.756.01.values_conventions_mismatch",request->values->conventions!=request->conventions)
        ||sol_mir_runtime_host_abi_observe_reject("H37.P.sol_mir_runtime_host_abi_build.756.02.cleanup_conventions_mismatch",request->cleanup->conventions!=request->conventions)
        ||sol_mir_runtime_host_abi_observe_reject("H37.P.sol_mir_runtime_host_abi_build.756.03.cleanup_values_mismatch",request->cleanup->values!=request->values)
        ||sol_mir_runtime_host_abi_observe_reject("H37.P.sol_mir_runtime_host_abi_build.756.04.conventions_validation",!sol_mir_runtime_conventions_validate(request->conventions,d))
        ||sol_mir_runtime_host_abi_observe_reject("H37.P.sol_mir_runtime_host_abi_build.756.05.values_validation",!sol_mir_runtime_values_validate(request->values,d))
        ||sol_mir_runtime_host_abi_observe_reject("H37.P.sol_mir_runtime_host_abi_build.756.06.cleanup_validation",!sol_mir_runtime_cleanup_validate(request->cleanup,d))){
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.756.predecessors","producer-entry","original predecessor rejection",NULL);
#endif
        diagnostic(d,"runtime host ABI predecessors are invalid");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
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
        sol_mir_runtime_host_abi_observe_index(0,"entry_index",e);
        if(!sol_mir_runtime_host_abi_work_tick(&census_work))goto exhausted;
        const SolMirRuntimeSignature*s=&x.conventions->signatures[x.conventions->entries[e].signature];
        for(size_t q=0;q<s->slots.count;q++){
            sol_mir_runtime_host_abi_observe_index(1,"signature_slot_index",q);
            if(!sol_mir_runtime_host_abi_work_tick(&census_work))goto exhausted;
            const SolMirRuntimeSignatureSlot*slot=&x.conventions->signature_slots[s->slots.offset+q];
            if(slot->role==SOL_MIR_RUNTIME_SLOT_PARAMETER&&slot->recipe<r->recipe_count
                &&r->recipes[slot->recipe].kind==SOL_MIR_RECIPE_CAPABILITY
                &&(!add(&roots,1)||!add(&caps,1)))goto exhausted;
        }
    }
    for(size_t i=0;i<ops->constructor_count;i++){
        sol_mir_runtime_host_abi_observe_index(0,"constructor_index",i);
        if(!sol_mir_runtime_host_abi_work_tick(&census_work))goto exhausted;
        if(ops->constructors[i].kind!=SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY)continue;
        SolMirMaterializedTemporaryId temporary;SolMirMaterializedValueId value;SolMirMaterializedPlaceId place;
        if(!result_temporary(m,ops->constructors[i].result,&temporary,&census_work))temporary=SOL_MIR_RUNTIME_NONE;
        if(ops->constructors[i].capability_source_operand>=ops->constructors[i].operands.count){
            sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.783.source_operand_index","producer-census","original constructor operand index rejected",NULL);
            sol_mir_runtime_host_abi_observe_value("constructor_index","evaluated","records",i);
            sol_mir_runtime_host_abi_observe_value("capability_source_operand","evaluated","index",ops->constructors[i].capability_source_operand);
            sol_mir_runtime_host_abi_observe_value("constructor_operand_count","evaluated","count",ops->constructors[i].operands.count);
            goto exhausted;
        }
        if(!temporary_coordinate(m,ops->construct_operands[ops->constructors[i].operands.offset+ops->constructors[i].capability_source_operand].temporary,&value,&place,&census_work,true)){
            sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.783.temporary_coordinate","producer-census","original temporary coordinate lookup failed",NULL);
            sol_mir_runtime_host_abi_observe_value("constructor_index","evaluated","records",i);
            sol_mir_runtime_host_abi_observe_value("capability_source_operand","evaluated","index",ops->constructors[i].capability_source_operand);
            goto exhausted;
        }
        if(!add(&caps,1))goto exhausted;
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
        if (sol_mir_runtime_host_abi_observe_ge(
                "H37.P.sol_mir_runtime_host_abi_build.796.surface_work_reclass",
                "census_work.used",census_work.used,"census_work.limit",census_work.limit)) {
            sol_mir_runtime_host_abi_observe_propagation(
                "H37.P.sol_mir_runtime_host_abi_build.796.surface_work_reclass",
                "original frozen-surface failure reclassified by census meter");
            goto exhausted;
        }
        sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.795.frozen_e6_surface","producer-census","original frozen surface predicate returned false",NULL);
        goto unsupported;
    }
    for(size_t h=0;h<l->host_requirement_count;h++){
        sol_mir_runtime_host_abi_observe_index(0,"host_index",h);
        const SolMirLinkageHostRequirement*host=&l->host_requirements[h];
        if(!e3_host_requirement(&x,h,&census_work)){
            if(sol_mir_runtime_host_abi_observe_ge("H37.P.sol_mir_runtime_host_abi_build.801.e3_work_reclass","census_work.used",census_work.used,"census_work.limit",census_work.limit)){
                sol_mir_runtime_host_abi_observe_propagation("H37.P.sol_mir_runtime_host_abi_build.801.e3_work_reclass","original E3 failure reclassified by census meter");
                goto exhausted;
            }
            sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.801.e3_host_requirement","producer-census","original E3 host requirement predicate returned false",NULL);
            sol_mir_runtime_host_abi_observe_value("host_index","evaluated","index",h);
            goto unsupported;
        }
        if(!find_import(x.conventions,h,NULL,&census_work)){
            sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.802.find_import","producer-census","original host import lookup returned null",NULL);
            sol_mir_runtime_host_abi_observe_value("host_index","evaluated","index",h);
            goto unsupported;
        }
        if(!find_signature(x.conventions,h,NULL,&census_work)){
            sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.803.find_signature","producer-census","original host signature lookup returned null",NULL);
            sol_mir_runtime_host_abi_observe_value("host_index","evaluated","index",h);
            goto unsupported;
        }
        for(size_t q=0;q<host->parameters.count;q++){
            sol_mir_runtime_host_abi_observe_index(1,"parameter_index",q);
            bool ticked=sol_mir_runtime_host_abi_work_tick(&census_work);
            bool count_shape_failed=false;
            if(!ticked){
                sol_mir_runtime_host_abi_observe_unavailable_value(
                    "formal_count.add","not_reached","predicate");
                sol_mir_runtime_host_abi_observe_unavailable_value(
                    "count_shape","not_reached","predicate");
            }else if(!add(&formals,1)){
                sol_mir_runtime_host_abi_observe_unavailable_value(
                    "count_shape","not_reached","predicate");
            }else if(!count_shape(r,m->type_ids[host->parameters.offset+q],0,
                    &shapes,&cases,&census_work)){
                count_shape_failed=true;
            }else{
                continue;
            }
            if(sol_mir_runtime_host_abi_observe_ge(
                    "H37.P.sol_mir_runtime_host_abi_build.805.work_reclassification",
                    "census_work.used",census_work.used,
                    "census_work.limit",census_work.limit)){
                sol_mir_runtime_host_abi_observe_propagation(
                    "H37.P.sol_mir_runtime_host_abi_build.805.work_reclassification",
                    "original formal/shape failure reclassified by census meter");
                goto exhausted;
            }
            if(count_shape_failed){
                sol_mir_runtime_host_abi_observe_failure(
                    "H37.P.sol_mir_runtime_host_abi_build.805.count_shape",
                    "producer-census",
                    "original recursive shape count returned false",NULL);
                sol_mir_runtime_host_abi_observe_value(
                    "host_index","evaluated","index",h);
                sol_mir_runtime_host_abi_observe_value(
                    "parameter_index","evaluated","index",q);
            }
            goto unsupported;
        }
    }
    if(!add(&validation_shape,r->recipe_count)||!add(&validation_shape,shapes)
        ||!add(&validation_roots,m->image_count)||!add(&validation_roots,root_bits))goto exhausted;
    validation_bytes=validation_shape>validation_roots?validation_shape:validation_roots;
    workspace=(unsigned char*)scratch_allocate(workspace_bytes);
    if(!workspace){
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.811.workspace_allocation","producer-allocation","original workspace allocation returned null",NULL);
        sol_mir_runtime_host_abi_observe_value("workspace_bytes","initialized","bytes",workspace_bytes);
#endif
        goto allocation;
    }
    memset(workspace,0,workspace_bytes);
    SolMirRuntimeHostAbi pre=x;
    pre.entry_roots=(SolMirRuntimeHostEntryRoot*)workspace;
    pre.entry_root_count=roots;
    images=workspace+roots*sizeof(*pre.entry_roots);
    root_state=images+m->image_count;
    size_t root_index=0;
    for(size_t e=0;e<pre.conventions->entry_count;e++){
        sol_mir_runtime_host_abi_observe_index(0,"entry_index",e);
        if(!sol_mir_runtime_host_abi_work_tick(&census_work))goto exhausted;
        const SolMirRuntimeEntry*entry=&pre.conventions->entries[e];
        const SolMirRuntimeSignature*s=&pre.conventions->signatures[entry->signature];
        for(size_t q=0;q<s->slots.count;q++){
            sol_mir_runtime_host_abi_observe_index(1,"signature_slot_index",q);
            if(!sol_mir_runtime_host_abi_work_tick(&census_work))goto exhausted;
            const SolMirRuntimeSignatureSlot*slot=&pre.conventions->signature_slots[s->slots.offset+q];
            if(slot->role==SOL_MIR_RUNTIME_SLOT_PARAMETER&&slot->recipe<r->recipe_count
                &&r->recipes[slot->recipe].kind==SOL_MIR_RECIPE_CAPABILITY)
                pre.entry_roots[root_index++]=(SolMirRuntimeHostEntryRoot){e,slot->formal,slot->recipe,entry->signature};
        }
    }
    unsigned char *grant_state=root_state+root_bits;
    size_t requirements=0,grants=0;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    grants_available=true;
#endif
    for(size_t e=0;e<pre.conventions->entry_count;e++){
        sol_mir_runtime_host_abi_observe_index(0,"entry_index",e);
        if(!sol_mir_runtime_host_abi_work_tick(&census_work))goto exhausted;
        memset(images,0,m->image_count);memset(root_state,0,root_bits);
        if(!mark_entry_images(&pre,e,images,&census_work)){
            if(sol_mir_runtime_host_abi_observe_ge("H37.P.sol_mir_runtime_host_abi_build.836.mark_images_work_reclass","census_work.used",census_work.used,"census_work.limit",census_work.limit)){
                sol_mir_runtime_host_abi_observe_propagation("H37.P.sol_mir_runtime_host_abi_build.836.mark_images_work_reclass","original image marking failure reclassified by census meter");
                goto exhausted;
            }
            sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.836.mark_entry_images","producer-census","original entry image marking returned false",NULL);
            sol_mir_runtime_host_abi_observe_value("entry_index","evaluated","index",e);
            goto unsupported;
        }
        if(!closure_roots(&pre,e,images,root_state,&census_work)){
            if(sol_mir_runtime_host_abi_observe_ge("H37.P.sol_mir_runtime_host_abi_build.836.closure_work_reclass","census_work.used",census_work.used,"census_work.limit",census_work.limit)){
                sol_mir_runtime_host_abi_observe_propagation("H37.P.sol_mir_runtime_host_abi_build.836.closure_work_reclass","original provenance closure failure reclassified by census meter");
                goto exhausted;
            }
            sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.836.closure_roots","producer-census","original provenance closure returned false",NULL);
            sol_mir_runtime_host_abi_observe_value("entry_index","evaluated","index",e);
            goto unsupported;
        }
        for(size_t c=0;c<pre.conventions->call_count;c++){
            sol_mir_runtime_host_abi_observe_index(1,"call_index",c);
            if(!sol_mir_runtime_host_abi_work_tick(&census_work))goto exhausted;
            const SolMirRuntimeCall*call=&pre.conventions->calls[c];
            size_t host=target_host(&pre,call);const SolMirRuntimeOperand*receiver=receiver_operand(&pre,call);
            if(!call_owner_reachable(&pre,e,images,call)||host==SOL_MIR_RUNTIME_NONE||!receiver)continue;
            size_t receiver_root=receiver_origin(&pre,e,root_state,receiver,&census_work);
            if(receiver_root==SOL_MIR_RUNTIME_NONE){
                sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.843.receiver_origin","producer-census","original receiver origin lookup returned none",NULL);
                sol_mir_runtime_host_abi_observe_value("entry_index","evaluated","index",e);
                sol_mir_runtime_host_abi_observe_value("call_index","evaluated","index",c);
                sol_mir_runtime_host_abi_observe_value("host_index","evaluated","index",host);
                goto unsupported;
            }
            size_t event,transition;
            if(!call_cleanup(&pre,c,&event,&transition,&census_work)){
                sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.845.call_cleanup","producer-census","original cleanup mapping returned false",NULL);
                sol_mir_runtime_host_abi_observe_value("call_index","evaluated","index",c);
                goto unsupported;
            }
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
    ProducerLineageResult dry_lineage_result=PRODUCER_LINEAGE_OK;
    if(!persistent_write_traversal(&pre,&dry_write_work,images,root_state,root_bits,
            &dry_lineage_result)) {
        if (dry_lineage_result==PRODUCER_LINEAGE_EXHAUSTED) goto exhausted;
        if (dry_lineage_result==PRODUCER_LINEAGE_REJECTED) goto unsupported;
        if(sol_mir_runtime_host_abi_observe_ge("H37.P.sol_mir_runtime_host_abi_build.857.dry_work_reclass","dry_write_work.used",dry_write_work.used,"dry_write_work.limit",dry_write_work.limit)) {
            sol_mir_runtime_host_abi_observe_propagation("H37.P.sol_mir_runtime_host_abi_build.857.dry_work_reclass","original dry traversal failure reclassified by dry meter");
            goto exhausted;
        }
        sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.856.persistent_write_traversal","producer-dry-write","original dry traversal returned false",NULL);
        goto unsupported;
    }
    /* The dry traversal is a complete pre-allocation check.  Reserve its
     * already-executed work, but leave the emitting pass to consume its own
     * meter.  Thus an exact-minus-one limit fails on the final emitting tick,
     * after which the transactional owner is still unpublished. */
    if (census_work.used > x.limits.max_build_work) {
        sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.864.census_reservation","producer-reservation","original census reservation predicate rejected",NULL);
        goto exhausted;
    }
    if (dry_write_work.used > x.limits.max_build_work-census_work.used) {
        sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.865.dry_reservation","producer-reservation","original dry-write reservation predicate rejected",NULL);
        goto exhausted;
    }
    x.capability_capacity=caps;x.entry_root_capacity=roots;x.operation_capacity=l->host_requirement_count;x.argument_capacity=formals;x.formal_capacity=formals;x.shape_capacity=shapes;x.shape_case_capacity=cases;x.requirement_capacity=requirements;
    x.capability_count=caps;x.entry_root_count=roots;x.operation_count=l->host_requirement_count;x.argument_count=formals;x.formal_count=formals;x.shape_count=shapes;x.shape_case_count=cases;x.requirement_count=requirements;
    if(!calculate_usage_census(&x,grants,workspace_bytes,validation_bytes,0)){
        sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.869.preliminary_usage_census","producer-preliminary-fit","original preliminary usage census returned false",NULL);
        goto exhausted;
    }
    if(!fit(&x))goto exhausted;
    /* The physical write receives only the budget not consumed by census and
     * prediction.  A failed tick remains transactional and unpublished. */
    write_work.limit=x.limits.max_build_work-census_work.used-dry_write_work.used;
    x.capability_count=x.entry_root_count=x.operation_count=x.argument_count=x.formal_count=x.shape_count=x.shape_case_count=x.requirement_count=0;
    x.capabilities=HOST_ALLOC(caps,sizeof(*x.capabilities),0);x.entry_roots=HOST_ALLOC(roots,sizeof(*x.entry_roots),1);x.operations=HOST_ALLOC(l->host_requirement_count,sizeof(*x.operations),2);x.arguments=HOST_ALLOC(formals,sizeof(*x.arguments),3);x.formals=HOST_ALLOC(formals,sizeof(*x.formals),4);x.shapes=HOST_ALLOC(shapes,sizeof(*x.shapes),5);x.shape_cases=HOST_ALLOC(cases,sizeof(*x.shape_cases),6);x.requirements=HOST_ALLOC(requirements,sizeof(*x.requirements),7);
    if((caps&&!x.capabilities)||(roots&&!x.entry_roots)||(l->host_requirement_count&&!x.operations)||(formals&&!x.arguments)||(formals&&!x.formals)||(shapes&&!x.shapes)||(cases&&!x.shape_cases)||(requirements&&!x.requirements)){
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.875.persistent_allocation","producer-allocation","original persistent allocation rejected",NULL);
#endif
        goto allocation;
    }
    memset(workspace,0,workspace_bytes);
    images=workspace+roots*sizeof(*pre.entry_roots); root_state=images+m->image_count;
    for(size_t e=0;e<x.conventions->entry_count;e++){
        sol_mir_runtime_host_abi_observe_index(0,"entry_index",e);
        if(!sol_mir_runtime_host_abi_work_tick(&write_work))goto exhausted;
        const SolMirRuntimeEntry*entry=&x.conventions->entries[e];
        const SolMirRuntimeSignature*s=&x.conventions->signatures[entry->signature];
        for(size_t q=0;q<s->slots.count;q++){
            sol_mir_runtime_host_abi_observe_index(1,"signature_slot_index",q);
            if(!sol_mir_runtime_host_abi_work_tick(&write_work))goto exhausted;
            const SolMirRuntimeSignatureSlot*slot=&x.conventions->signature_slots[s->slots.offset+q];
            if(slot->role==SOL_MIR_RUNTIME_SLOT_PARAMETER&&slot->recipe<r->recipe_count
                &&r->recipes[slot->recipe].kind==SOL_MIR_RECIPE_CAPABILITY){
                size_t root=x.entry_root_count;
                x.entry_roots[x.entry_root_count++]=(SolMirRuntimeHostEntryRoot){e,slot->formal,slot->recipe,entry->signature};
                x.capabilities[x.capability_count++]=(SolMirRuntimeHostCapabilityPlan){
                    slot->recipe,SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT,SOL_MIR_RUNTIME_NONE,
                    SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE,
                    SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE,root,SOL_MIR_RUNTIME_NONE,
                    SOL_MIR_RECIPE_NONE,SOL_MIR_RUNTIME_NONE};
                (void)root;
            }
        }
    }
    for(size_t i=0;i<ops->constructor_count;i++){
        sol_mir_runtime_host_abi_observe_index(0,"constructor_index",i);
        if(!sol_mir_runtime_host_abi_work_tick(&write_work))goto exhausted;
        const SolMirOperationConstructPlan*p=&ops->constructors[i];
        if(p->kind!=SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY)continue;
        if(p->capability_source_operand>=p->operands.count){
            sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.887.source_operand_index","producer-emission","original constructor operand index rejected",NULL);
            sol_mir_runtime_host_abi_observe_value("constructor_index","evaluated","records",i);
            sol_mir_runtime_host_abi_observe_value("capability_source_operand","evaluated","index",p->capability_source_operand);
            sol_mir_runtime_host_abi_observe_value("constructor_operand_count","evaluated","count",p->operands.count);
            goto unsupported;
        }
        const SolMirOperationConstructOperand *operand=
            &ops->construct_operands[p->operands.offset+p->capability_source_operand];
        SolMirMaterializedTemporaryId temporary;
        SolMirMaterializedValueId source_value;
        SolMirMaterializedPlaceId source_place;
        if(!result_temporary(m,p->result,&temporary,&write_work)) temporary=SOL_MIR_RUNTIME_NONE;
        if(!temporary_coordinate(m,operand->temporary,&source_value,&source_place,
                &write_work,true)){
            sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.895.temporary_coordinate","producer-emission","original temporary coordinate lookup failed",NULL);
            sol_mir_runtime_host_abi_observe_value("constructor_index","evaluated","records",i);
            sol_mir_runtime_host_abi_observe_value("temporary","evaluated","index",operand->temporary);
            goto unsupported;
        }
        SolMirRuntimeHostCapabilitySource source=p->capability_rule==SOL_MIR_OPERATION_CAPABILITY_PRIVATE_SOURCE
            ?SOL_MIR_RUNTIME_HOST_CAPABILITY_PRIVATE_SOURCE
            :p->capability_rule==SOL_MIR_OPERATION_CAPABILITY_BASE_SOURCE
                ?SOL_MIR_RUNTIME_HOST_CAPABILITY_DERIVED:SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT;
        size_t root=SOL_MIR_RUNTIME_NONE,parent=SOL_MIR_RUNTIME_NONE;
        if(source==SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT){
            root=entry_root_for_local(&x,p->inherited_root,&write_work,true);
            if(root==SOL_MIR_RUNTIME_NONE){
                sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.903.root_none","producer-emission","original entry root lookup returned none",NULL);
                sol_mir_runtime_host_abi_observe_value("inherited_root","evaluated","index",p->inherited_root);
                goto unsupported;
            }
            if(source_place>=m->place_count){
                sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.903.source_place_bounds","producer-emission","original source place bounds predicate rejected",NULL);
                sol_mir_runtime_host_abi_observe_value("source_place","evaluated","index",source_place);
                sol_mir_runtime_host_abi_observe_value("place_count","evaluated","count",m->place_count);
                goto unsupported;
            }
            if(m->places[source_place].local!=p->inherited_root){
                sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.904.inherited_root_mismatch","producer-emission","original inherited root comparison rejected",NULL);
                sol_mir_runtime_host_abi_observe_value("source_place.local","evaluated","index",m->places[source_place].local);
                sol_mir_runtime_host_abi_observe_value("inherited_root","evaluated","index",p->inherited_root);
                goto unsupported;
            }
            if(m->places[source_place].projections.count){
                sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.905.root_projection_count","producer-emission","original root projection predicate rejected",NULL);
                sol_mir_runtime_host_abi_observe_value("source_place.projections.count","evaluated","count",m->places[source_place].projections.count);
                goto unsupported;
            }
            parent=root;
        } else if(source==SOL_MIR_RUNTIME_HOST_CAPABILITY_DERIVED) {
            ProducerLineageResult lineage=producer_resolve_base_parent(&x,m,i,
                &write_work,&parent,&root);
            if(lineage==PRODUCER_LINEAGE_EXHAUSTED)goto exhausted;
            if(lineage!=PRODUCER_LINEAGE_OK){
                sol_mir_runtime_host_abi_observe_failure(
                    "H37.P.sol_mir_runtime_host_abi_build.917.base_lineage",
                    "producer-emission",
                    "original exact capability lineage rejected",NULL);
                sol_mir_runtime_host_abi_observe_value(
                    "source_temporary","evaluated","index",operand->temporary);
                goto unsupported;
            }
        }
        x.capabilities[x.capability_count++]=(SolMirRuntimeHostCapabilityPlan){
            p->result_recipe,source,i,p->result,temporary,source_value,operand->temporary,
            source_place,root,parent,source==SOL_MIR_RUNTIME_HOST_CAPABILITY_PRIVATE_SOURCE
                ?operand->recipe:SOL_MIR_RECIPE_NONE,
            source==SOL_MIR_RUNTIME_HOST_CAPABILITY_PRIVATE_SOURCE?source_value:SOL_MIR_RUNTIME_NONE};
    }
    memset(workspace,0,workspace_bytes);active=workspace;
    for(size_t h=0;h<l->host_requirement_count;h++){
        sol_mir_runtime_host_abi_observe_index(0,"host_index",h);
        if(!e3_host_requirement(&x,h,&write_work)){
            if(sol_mir_runtime_host_abi_observe_ge("H37.P.sol_mir_runtime_host_abi_build.928.e3_work_reclass","write_work.used",write_work.used,"write_work.limit",write_work.limit)){
                sol_mir_runtime_host_abi_observe_propagation("H37.P.sol_mir_runtime_host_abi_build.928.e3_work_reclass","original emitted E3 failure reclassified by write meter");
                goto exhausted;
            }
            sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.928.e3_host_requirement","producer-emission","original E3 host requirement predicate returned false",NULL);
            sol_mir_runtime_host_abi_observe_value("host_index","evaluated","index",h);
            goto unsupported;
        }
        const SolMirLinkageHostRequirement*host=&l->host_requirements[h];
        size_t import,signature;
        const SolMirRuntimeImport*im=find_import(x.conventions,h,&import,&write_work);
        const SolMirRuntimeSignature*sig=find_signature(x.conventions,h,&signature,&write_work);
        if(!im){
            sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.928.find_import","producer-emission","original host import lookup returned null",NULL);
            sol_mir_runtime_host_abi_observe_value("host_index","evaluated","index",h);
            goto unsupported;
        }
        if(!sig){
            sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.928.find_signature","producer-emission","original host signature lookup returned null",NULL);
            sol_mir_runtime_host_abi_observe_value("host_index","evaluated","index",h);
            goto unsupported;
        }
        if(host->result>=x.values->host_result_plan_count){
            sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.928.result_plan_bounds","producer-emission","original host result plan bounds predicate rejected",NULL);
            sol_mir_runtime_host_abi_observe_value("host.result","evaluated","index",host->result);
            sol_mir_runtime_host_abi_observe_value("host_result_plan_count","evaluated","count",x.values->host_result_plan_count);
            goto unsupported;
        }
        if(x.values->host_result_plans[host->result].classification==SOL_MIR_RUNTIME_HOST_RESULT_FORBIDDEN){
            sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.928.result_forbidden","producer-emission","original forbidden host result classification rejected",NULL);
            sol_mir_runtime_host_abi_observe_value("host.result_classification","evaluated","enum",x.values->host_result_plans[host->result].classification);
            goto unsupported;
        }
        if(x.values->host_result_plans[host->result].classification==SOL_MIR_RUNTIME_HOST_RESULT_UNREACHABLE){
            sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.928.result_unreachable","producer-emission","original unreachable host result classification rejected",NULL);
            sol_mir_runtime_host_abi_observe_value("host.result_classification","evaluated","enum",x.values->host_result_plans[host->result].classification);
            goto unsupported;
        }
        SolMirRuntimeHostOperation*op=&x.operations[x.operation_count++];
        *op=(SolMirRuntimeHostOperation){h,import,signature,host->receiver,
            host->receiver_access,{x.formal_count,0},host->result,
            x.values->host_result_plans[host->result].classification,host->result,
            host->effects,im->identity,im->symbol,HOST_FAILURE_MASK};
        for(size_t q=0;q<host->parameters.count;q++){
            sol_mir_runtime_host_abi_observe_index(1,"parameter_index",q);
            SolMirRecipeId recipe=m->type_ids[host->parameters.offset+q];
            SolAccessMode access=m->accesses[host->parameter_accesses.offset+q];
            SolMirRuntimeHostArgumentKind kind; size_t shape;
            if(!sol_mir_runtime_host_abi_work_tick(&write_work))goto exhausted;
            if(!shape_kind(&r->recipes[recipe],&kind)){
                sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.928.shape_kind","producer-emission","original host argument kind lookup returned false",NULL);
                sol_mir_runtime_host_abi_observe_value("host_index","evaluated","index",h);
                sol_mir_runtime_host_abi_observe_value("parameter_index","evaluated","index",q);
                goto exhausted;
            }
            if(!append_shape(&x,recipe,access,active,&shape,&write_work)){
                sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.928.append_shape","producer-emission","original recursive shape append returned false",NULL);
                sol_mir_runtime_host_abi_observe_value("host_index","evaluated","index",h);
                sol_mir_runtime_host_abi_observe_value("parameter_index","evaluated","index",q);
                goto exhausted;
            }
            if(x.argument_count>=x.argument_capacity){
                sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.928.argument_capacity","producer-emission","original argument capacity predicate rejected",NULL);
                sol_mir_runtime_host_abi_observe_value("argument_count","evaluated","count",x.argument_count);
                sol_mir_runtime_host_abi_observe_value("argument_capacity","evaluated","count",x.argument_capacity);
                goto exhausted;
            }
            x.arguments[x.argument_count++]=(SolMirRuntimeHostArgument){recipe,kind,access,{0,0}};
            x.formals[x.formal_count++]=(SolMirRuntimeHostFormal){recipe,access,shape};
        }
        op->formals.count=x.formal_count-op->formals.offset;
    }
    memset(workspace,0,workspace_bytes);
    images=workspace+roots*sizeof(*x.entry_roots);root_state=images+m->image_count;
    for(size_t e=0;e<x.conventions->entry_count;e++){
        sol_mir_runtime_host_abi_observe_index(0,"entry_index",e);
        if(!sol_mir_runtime_host_abi_work_tick(&write_work))goto exhausted;
        memset(images,0,m->image_count);memset(root_state,0,root_bits);
        if(!mark_entry_images(&x,e,images,&write_work)){
            if(sol_mir_runtime_host_abi_observe_ge("H37.P.sol_mir_runtime_host_abi_build.931.mark_images_work_reclass","write_work.used",write_work.used,"write_work.limit",write_work.limit)){
                sol_mir_runtime_host_abi_observe_propagation("H37.P.sol_mir_runtime_host_abi_build.931.mark_images_work_reclass","original emitted image marking failure reclassified by write meter");
                goto exhausted;
            }
            sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.931.mark_entry_images","producer-emission","original emitted entry image marking returned false",NULL);
            sol_mir_runtime_host_abi_observe_value("entry_index","evaluated","index",e);
            goto unsupported;
        }
        if(!closure_roots(&x,e,images,root_state,&write_work)){
            if(sol_mir_runtime_host_abi_observe_ge("H37.P.sol_mir_runtime_host_abi_build.931.closure_work_reclass","write_work.used",write_work.used,"write_work.limit",write_work.limit)){
                sol_mir_runtime_host_abi_observe_propagation("H37.P.sol_mir_runtime_host_abi_build.931.closure_work_reclass","original emitted provenance closure failure reclassified by write meter");
                goto exhausted;
            }
            sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.931.closure_roots","producer-emission","original emitted provenance closure returned false",NULL);
            sol_mir_runtime_host_abi_observe_value("entry_index","evaluated","index",e);
            goto unsupported;
        }
        for(size_t c=0;c<x.conventions->call_count;c++){
            sol_mir_runtime_host_abi_observe_index(1,"call_index",c);
            const SolMirRuntimeCall*call=&x.conventions->calls[c];
            if(!sol_mir_runtime_host_abi_work_tick(&write_work))goto exhausted;
            size_t host=target_host(&x,call);
            if(!call_owner_reachable(&x,e,images,call)||host==SOL_MIR_RUNTIME_NONE||host>=x.operation_count)continue;
            const SolMirRuntimeOperand*receiver=receiver_operand(&x,call);
            size_t root=receiver_origin(&x,e,root_state,receiver,&write_work);
            if(root==SOL_MIR_RUNTIME_NONE){
                sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.931.receiver_origin","producer-emission","original emitted receiver origin returned none",NULL);
                sol_mir_runtime_host_abi_observe_value("entry_index","evaluated","index",e);
                sol_mir_runtime_host_abi_observe_value("call_index","evaluated","index",c);
                sol_mir_runtime_host_abi_observe_value("host_index","evaluated","index",host);
                goto unsupported;
            }
            size_t event,transition;
            if(!call_cleanup(&x,c,&event,&transition,&write_work)){
                sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.931.call_cleanup","producer-emission","original emitted cleanup mapping returned false",NULL);
                sol_mir_runtime_host_abi_observe_value("call_index","evaluated","index",c);
                goto unsupported;
            }
            if(x.requirement_count>=x.requirement_capacity){
                sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.931.requirement_capacity","producer-emission","original emitted requirement capacity predicate rejected",NULL);
                sol_mir_runtime_host_abi_observe_value("requirement_count","evaluated","count",x.requirement_count);
                sol_mir_runtime_host_abi_observe_value("requirement_capacity","evaluated","count",x.requirement_capacity);
                goto exhausted;
            }
            x.requirements[x.requirement_count++]=(SolMirRuntimeHostRequirement){e,root,host,c,event,transition};
        }
    }
    free(workspace);workspace=NULL;
    if(sol_mir_runtime_host_abi_observe_ne(
        "H37.P.sol_mir_runtime_host_abi_build.933.requirement_count",
        "emitted.requirements",x.requirement_count,"predicted.requirements",
        x.requirement_capacity)){
        goto exhausted;
    }
    if(sol_mir_runtime_host_abi_observe_ne(
        "H37.P.sol_mir_runtime_host_abi_build.933.argument_count",
        "emitted.arguments",x.argument_count,"predicted.arguments",
        x.argument_capacity)){
        goto exhausted;
    }
    bool usage_census_succeeded=calculate_usage_census(&x,grants,
        workspace_bytes,validation_bytes,census_work.used+dry_write_work.used+
        write_work.used);
    if(!usage_census_succeeded){
        sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.933.calculate_usage_census","producer-final-census","original usage census returned false",NULL);
        goto exhausted;
    }
    bool measurement_succeeded=
        sol_mir_runtime_host_abi_internal_measure_validation_work(
            &x,&x.usage.validation_work);
    if(!measurement_succeeded){
        sol_mir_runtime_host_abi_observe_propagation("H37.P.sol_mir_runtime_host_abi_build.933.measure_validation_work","original measurement result collapsed to false");
        goto exhausted;
    }
    if(!fit(&x)){
        sol_mir_runtime_host_abi_observe_failure("H37.P.sol_mir_runtime_host_abi_build.933.final_fit","producer-final-fit","original final fit returned false",NULL);
        goto exhausted;
    }
    SolMirRuntimeHostAbiBuildOutcome final_validation_result=
        sol_mir_runtime_host_abi_internal_validate(&x,d);
    if(final_validation_result!=SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED){
        sol_mir_runtime_host_abi_observe_propagation(
            "H37.P.sol_mir_runtime_host_abi_build.934.final_validation",
            "original producer propagated non-success validator outcome");
        sol_mir_runtime_host_abi_free(&x);
        return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;
    }
    x.authentication=owner_seal(&x);
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    sol_mir_runtime_host_abi_internal_work_census.census_work=census_work.used;
    sol_mir_runtime_host_abi_internal_work_census.dry_work=dry_write_work.used;
    sol_mir_runtime_host_abi_internal_work_census.actual_work=write_work.used;
    sol_mir_runtime_host_abi_internal_work_census.validation_work=x.usage.validation_work;
#endif
    *out=x;return SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED;
    unsupported:
        if(sol_mir_runtime_host_abi_observe_ge(
                "H37.P.sol_mir_runtime_host_abi_build.942.census_reclassification",
                "census_work.used",census_work.used,"census_work.limit",census_work.limit)
            ||(dry_write_work.limit&&sol_mir_runtime_host_abi_observe_ge(
                "H37.P.sol_mir_runtime_host_abi_build.943.dry_reclassification",
                "dry_write_work.used",dry_write_work.used,"dry_write_work.limit",dry_write_work.limit))
            ||(write_work.limit&&sol_mir_runtime_host_abi_observe_ge(
                "H37.P.sol_mir_runtime_host_abi_build.944.write_reclassification",
                "write_work.used",write_work.used,"write_work.limit",write_work.limit))) {
            sol_mir_runtime_host_abi_observe_propagation(
                "H37.P.sol_mir_runtime_host_abi_build.942-944.meter_reclassification",
                "original unsupported path reclassified by meter state");
            goto exhausted;
        }
        sol_mir_runtime_host_abi_observe_failure(
            "H37.P.sol_mir_runtime_host_abi_build.945.unsupported_origin",
            "producer-unsupported","original unsupported label reached",NULL);
        sol_mir_runtime_host_abi_observe_propagation(
            "H37.P.sol_mir_runtime_host_abi_build.945.unsupported_outcome",
            "original unsupported outcome propagated without meter reclassification");
        free(workspace);sol_mir_runtime_host_abi_free(&x);
        diagnostic(d,"host ABI requirement is outside the E3 closure");
        return SOL_MIR_RUNTIME_HOST_ABI_BUILD_UNSUPPORTED;
    exhausted:
        sol_mir_runtime_host_abi_observe_propagation(
            "H37.P.sol_mir_runtime_host_abi_build.946.exhausted",
            "original resource-exhaustion label propagated the first failure");
        sol_mir_runtime_host_abi_observe_failure(
            "H37.P.sol_mir_runtime_host_abi_build.946.exhausted",
            "producer-exhausted","original exhausted label reached",NULL);
        sol_mir_runtime_host_abi_observe_value("census_work.used","initialized","operations",census_work.used);
        sol_mir_runtime_host_abi_observe_value("census_work.limit","initialized","operations",census_work.limit);
        sol_mir_runtime_host_abi_observe_value("dry_write_work.used","initialized","operations",dry_write_work.used);
        sol_mir_runtime_host_abi_observe_value("dry_write_work.limit","initialized","operations",dry_write_work.limit);
        sol_mir_runtime_host_abi_observe_value("write_work.used","initialized","operations",write_work.used);
        sol_mir_runtime_host_abi_observe_value("write_work.limit","initialized","operations",write_work.limit);
        sol_mir_runtime_host_abi_observe_value("roots","initialized","records",roots);
        sol_mir_runtime_host_abi_observe_value("caps","initialized","records",caps);
        sol_mir_runtime_host_abi_observe_value("formals","initialized","records",formals);
        sol_mir_runtime_host_abi_observe_value("shapes","initialized","records",shapes);
        sol_mir_runtime_host_abi_observe_value("shape_cases","initialized","records",cases);
    sol_mir_runtime_host_abi_observe_value("requirements","initialized","records",x.requirement_count);
#ifdef SOL_MIR_PLAN_TEST_HOOKS
         observe_grants_if_available(grants_available,&grants);
#endif
        free(workspace);sol_mir_runtime_host_abi_free(&x);
        diagnostic(d,"runtime host ABI resource limit exceeded");
        return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;
    allocation:
        sol_mir_runtime_host_abi_observe_propagation(
            "H37.P.sol_mir_runtime_host_abi_build.947.allocation",
            "original allocation label propagated allocation failure");
        sol_mir_runtime_host_abi_observe_failure(
            "H37.P.sol_mir_runtime_host_abi_build.947.allocation",
            "producer-allocation","original allocation label reached",NULL);
        sol_mir_runtime_host_abi_observe_value("workspace_bytes","initialized","bytes",workspace_bytes);
        free(workspace);sol_mir_runtime_host_abi_free(&x);
        if(d)d->allocation_failed=true;
        diagnostic(d,"runtime host ABI allocation failed");
        return SOL_MIR_RUNTIME_HOST_ABI_BUILD_ALLOCATION_FAILED;
}
#undef HOST_ALLOC

#ifdef SOL_MIR_PLAN_TEST_HOOKS
SolMirRuntimeHostAbiBuildOutcome sol_mir_runtime_host_abi_build(
    const SolMirRuntimeHostAbiBuildRequest *request,SolMirRuntimeHostAbi *out,
    SolDiagnostics *d) {
    SolMirRuntimeHostAbiObservationContext context={0};
    SolMirRuntimeHostAbiObservationContext *previous=
        sol_mir_runtime_host_abi_observation_context;
    sol_mir_runtime_host_abi_observation_context=&context;
    SolMirRuntimeHostAbiBuildOutcome outcome=
        host_abi_build_observed(request,out,d);
    context.producer_outcome=outcome;
    if(outcome!=SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED
        &&!context.first_failure_id){
        context.capture_gap=true;
        context.capture_gap_reason="non-success producer outcome without a captured terminating predicate";
    }
    observation_emit(&context);
    sol_mir_runtime_host_abi_observation_context=previous;
    return outcome;
}
#endif

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
static void host_render_digest(char *out,const SolMirLinkageDigest *digest){
    static const char hex[]="0123456789abcdef";
    for(size_t i=0;i<SOL_MIR_LINKAGE_DIGEST_BYTES;i++){
        out[2*i]=hex[digest->bytes[i]>>4];out[2*i+1]=hex[digest->bytes[i]&15];
    }
    out[2*SOL_MIR_LINKAGE_DIGEST_BYTES]='\0';
}
#ifdef SOL_MIR_PLAN_TEST_HOOKS
bool sol_mir_runtime_host_abi_test_hex_regression(void){
    for(size_t offset=0;offset<256;++offset){
        SolMirLinkageDigest digest={{0}};char actual[67],reference[67];
        memset(actual,0x5a,sizeof actual);memset(reference,0x5a,sizeof reference);
        for(size_t i=0;i<sizeof digest.bytes;++i)digest.bytes[i]=(uint8_t)(offset+i);
        host_render_digest(actual+1,&digest);
        for(size_t i=0;i<SOL_MIR_LINKAGE_DIGEST_BYTES;++i)(void)snprintf(reference+1+2*i,3,"%02x",(unsigned)digest.bytes[i]);
        if(memcmp(actual,reference,sizeof actual)!=0)return false;
    }
    return true;
}
#endif
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
    /* This field preserves a template-MIR instruction ordinal for instruction
       values; it is not an IR definition reference. */
    key_text(&state,"source-definition:none");
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
