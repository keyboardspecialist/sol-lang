#include "sol/mir_concrete.h"
#include "mir_concrete_internal.h"
#include "mir_linkage_internal.h"

#include <stdint.h>
#include <stdlib.h>

static _Thread_local size_t concrete_work;
static _Thread_local size_t concrete_work_limit;

static bool add_size(size_t *value, size_t amount) {
    if (amount > SIZE_MAX - *value) return false;
    *value += amount;
    return true;
}

static bool tick(size_t amount) {
    return add_size(&concrete_work, amount)
        && concrete_work <= concrete_work_limit;
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
static _Thread_local bool force_validation_allocation_failure;
static _Thread_local size_t validation_allocation_attempts;

void sol_mir_concrete_test_force_validation_allocation_failure(bool force) {
    force_validation_allocation_failure = force;
    validation_allocation_attempts = 0;
}

size_t sol_mir_concrete_test_validation_allocation_attempts(void) {
    return validation_allocation_attempts;
}
#endif

static bool invalid(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics != NULL) sol_diagnostics_add(diagnostics,
        "SOL-MIR-CONCRETE-002", SOL_SEVERITY_ERROR, (SolSpan){0}, message);
    return false;
}

static bool validate_links(const SolMirConcreteProgram *program) {
    return tick(8) && program != NULL && program->program.ir != NULL
        && program->plan.program == &program->program
        && program->materialization.plan == &program->plan
        && program->representation.materialization == &program->materialization
        && program->layout.representation == &program->representation
        && program->operations.layout == &program->layout
        && program->linkage.operations == &program->operations;
}

static bool validate_census(const SolMirConcreteProgram *program,
    SolDiagnostics *diagnostics) {
    const SolMirPlan *plan = &program->plan;
    const SolMirMaterialization *materialization = &program->materialization;
    const SolMirRepresentation *representation = &program->representation;
    const SolMirLayout *layout = &program->layout;
    const SolMirOperations *operations = &program->operations;
    const SolMirLinkage *linkage = &program->linkage;
    if (!tick(3) || materialization->image_count != plan->instance_count
        || materialization->import_count != plan->import_count
        || materialization->binding_count != plan->demand_count)
        return invalid(diagnostics, "plan/materialization census mismatch");
    if (!tick(1)
        || representation->recipe_count != materialization->type_count)
        return invalid(diagnostics,
            "materialization/representation census mismatch");
    if (!tick(4) || layout->type_count != representation->recipe_count
        || layout->field_count != representation->field_count
        || layout->variant_count != representation->variant_count
        || layout->projection_count != materialization->projection_count)
        return invalid(diagnostics, "representation/layout census mismatch");
    if (!tick(2)
        || operations->access_plan_count != materialization->place_count
        || operations->import_envelope_count != materialization->import_count)
        return invalid(diagnostics, "materialization/operations census mismatch");
    if (!tick(4) || linkage->callable_count != materialization->image_count
        || linkage->binding_count != materialization->binding_count
        || linkage->host_requirement_count != materialization->import_count
        || linkage->callable_value_count != operations->callable_count)
        return invalid(diagnostics, "executable/linkage census mismatch");
    return true;
}

static bool validate_closure(const SolMirConcreteProgram *program,
    SolDiagnostics *diagnostics) {
    const SolMirPlan *plan = &program->plan;
    const SolMirMaterialization *materialization = &program->materialization;
    const SolMirOperations *operations = &program->operations;
    for (size_t i = 0; i < plan->type_count; ++i)
        if (!tick(1) || plan->types[i].kind == SOL_IR_TYPE_PARAMETER
            || plan->types[i].kind == SOL_IR_TYPE_SELF)
            return invalid(diagnostics, "symbolic type remains executable");
    for (size_t i = 0; i < materialization->type_count; ++i)
        if (!tick(1)
            || materialization->types[i].kind == SOL_IR_TYPE_PARAMETER
            || materialization->types[i].kind == SOL_IR_TYPE_SELF)
            return invalid(diagnostics, "materialized type is not concrete");

    for (size_t i = 0; i < materialization->image_count; ++i)
        if (!tick(1) || materialization->images[i].instance != i)
            return invalid(diagnostics, "materialized image census is not dense");
    unsigned char *resolved = NULL;
    if (plan->demand_count != 0) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        ++validation_allocation_attempts;
        if (!force_validation_allocation_failure)
#endif
            resolved = calloc(plan->demand_count, 1);
    }
    if (plan->demand_count != 0 && resolved == NULL) {
        if (diagnostics != NULL) diagnostics->allocation_failed = true;
        return invalid(diagnostics,
            "concrete demand validation allocation failed");
    }
    for (size_t i = 0; i < materialization->binding_count; ++i) {
        if (!tick(1)) {
            free(resolved);
            return false;
        }
        const SolMirMaterializedBinding *binding = &materialization->bindings[i];
        if (binding->source_demand >= plan->demand_count
            || resolved[binding->source_demand] != 0) {
            free(resolved);
            return invalid(diagnostics, "materialized demand is not closed");
        }
        resolved[binding->source_demand] = 1;
        if (!tick(1)) {
            free(resolved);
            return false;
        }
        if (binding->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE) {
            if (binding->instance >= materialization->image_count
                || binding->import != SOL_MIR_MATERIALIZED_NONE) {
                free(resolved);
                return invalid(diagnostics, "instance demand is not closed");
            }
        } else if (binding->target_kind == SOL_MIR_MATERIALIZED_TARGET_IMPORT) {
            if (binding->import >= materialization->import_count
                || binding->instance != SOL_MIR_MATERIALIZED_NONE) {
                free(resolved);
                return invalid(diagnostics, "import demand is not closed");
            }
        } else {
            free(resolved);
            return invalid(diagnostics, "demand target is malformed");
        }
    }
    for (size_t demand = 0; demand < plan->demand_count; ++demand) {
        if (!tick(1) || resolved[demand] == 0) {
            free(resolved);
            return invalid(diagnostics, "planned demand is not uniquely resolved");
        }
    }
    free(resolved);

    size_t blocks = 0, constructors = 0, pattern_tests = 0;
    size_t pattern_extractions = 0, arithmetic = 0, snapshots = 0;
    size_t propagations = 0, predicates = 0;
    for (size_t i = 0; i < materialization->image_count; ++i) {
        if (!tick(1)) return false;
        const SolMirMaterializedImage *image = &materialization->images[i];
        if (image->blocks.offset != blocks
            || image->blocks.count > materialization->block_count - blocks)
            return invalid(diagnostics, "materialized block slices overlap");
        blocks += image->blocks.count;
    }
    if (blocks != materialization->block_count)
        return invalid(diagnostics, "materialized blocks are not consumed");
    for (size_t i = 0; i < materialization->instruction_count; ++i) {
        if (!tick(1)) return false;
        switch (materialization->instructions[i].kind) {
            case SOL_MIR_INST_CONSTRUCT: ++constructors; break;
            case SOL_MIR_INST_PATTERN_TEST: ++pattern_tests; break;
            case SOL_MIR_INST_PATTERN_VALUE: ++pattern_extractions; break;
            case SOL_MIR_INST_UNARY:
            case SOL_MIR_INST_BINARY:
            case SOL_MIR_INST_COMPOUND_UPDATE: ++arithmetic; break;
            case SOL_MIR_INST_CAPTURE_SNAPSHOT: ++snapshots; break;
            default: break;
        }
    }
    for (size_t i = 0; i < materialization->block_count; ++i) {
        if (!tick(1)) return false;
        const SolMirMaterializedBlock *block = &materialization->blocks[i];
        if (!block->started)
            return invalid(diagnostics, "unstarted executable block remains");
        if (block->terminator.kind == SOL_MIR_TERM_PROPAGATE) ++propagations;
        if (block->terminator.kind == SOL_MIR_TERM_CHECK_CONTRACT
            || block->terminator.kind == SOL_MIR_TERM_CHECK_REFINED) ++predicates;
        if (block->terminator.kind == SOL_MIR_TERM_INVOKE) {
            if (block->terminator.binding >= materialization->binding_count)
                return invalid(diagnostics, "invoke binding is not closed");
            if (block->terminator.callable_site != SOL_MIR_MATERIALIZED_NONE
                && (block->terminator.callable_site
                        >= materialization->semantic_site_count
                    || materialization->semantic_sites[
                        block->terminator.callable_site].binding
                        >= materialization->binding_count))
                return invalid(diagnostics, "invoke site is not closed");
        }
    }
    if (!tick(7) || constructors != operations->constructor_count
        || pattern_tests != operations->pattern_test_count
        || pattern_extractions != operations->pattern_extraction_count
        || arithmetic != operations->arithmetic_count
        || snapshots != operations->snapshot_count
        || propagations != operations->propagation_count
        || predicates != operations->predicate_count)
        return invalid(diagnostics, "operation plan census mismatch");
    return true;
}

bool sol_mir_concrete_internal_validation_requirements(
    const SolMirConcreteProgram *program, size_t *work, size_t *scratch_bytes) {
    if (program == NULL || work == NULL || scratch_bytes == NULL) return false;
    size_t result = 8 + 14;
    const SolMirPlan *plan = &program->plan;
    const SolMirMaterialization *materialization = &program->materialization;
    if (!add_size(&result, program->linkage.usage.validation_work)
        || !add_size(&result, plan->type_count)
        || !add_size(&result, materialization->type_count)
        || !add_size(&result, materialization->image_count)
        || materialization->binding_count > SIZE_MAX / 2
        || !add_size(&result, materialization->binding_count * 2)
        || !add_size(&result, plan->demand_count)
        || !add_size(&result, materialization->image_count)
        || !add_size(&result, materialization->instruction_count)
        || !add_size(&result, materialization->block_count)
        || !add_size(&result, 7)) return false;
    *work = result;
    *scratch_bytes = program->linkage.usage.validation_scratch_bytes
            > plan->demand_count
        ? program->linkage.usage.validation_scratch_bytes : plan->demand_count;
    return true;
}

bool sol_mir_concrete_internal_validate_measured(
    const SolMirConcreteProgram *program, SolDiagnostics *diagnostics,
    size_t exact_work_limit, size_t exact_scratch_limit,
    size_t *measured_work, size_t *measured_scratch_bytes) {
    size_t required_work, required_scratch;
    if (measured_work == NULL || measured_scratch_bytes == NULL
        || !sol_mir_concrete_internal_validation_requirements(program,
            &required_work, &required_scratch)
        || exact_work_limit != required_work
        || exact_scratch_limit != required_scratch)
        return invalid(diagnostics, "concrete exact validation limits mismatch");
    concrete_work = 0;
    concrete_work_limit = exact_work_limit;
    if (!validate_links(program))
        return invalid(diagnostics,
            "concrete program embedded predecessor links are malformed");
    if (!validate_census(program, diagnostics)) return false;
    size_t linkage_work = program->linkage.usage.validation_work;
    size_t linkage_scratch = program->linkage.usage.validation_scratch_bytes;
    size_t measured_linkage_work, measured_linkage_scratch;
    if (!tick(linkage_work)
        || !sol_mir_linkage_internal_validate_measured(&program->linkage,
            diagnostics, linkage_work, linkage_scratch,
            &measured_linkage_work, &measured_linkage_scratch)
        || measured_linkage_work != linkage_work
        || measured_linkage_scratch != linkage_scratch)
        return invalid(diagnostics, "concrete program stage validation failed");
    if (program->plan.demand_count > exact_scratch_limit)
        return invalid(diagnostics, "concrete validation scratch limit exceeded");
    if (!validate_closure(program, diagnostics)) return false;
    size_t scratch = measured_linkage_scratch > program->plan.demand_count
        ? measured_linkage_scratch : program->plan.demand_count;
    *measured_work = concrete_work;
    *measured_scratch_bytes = scratch;
    return concrete_work == exact_work_limit && scratch == exact_scratch_limit;
}

bool sol_mir_concrete_program_validate(const SolMirConcreteProgram *program,
    SolDiagnostics *diagnostics) {
    size_t required_work, required_scratch, measured_work, measured_scratch;
    return sol_mir_concrete_internal_validation_requirements(program,
            &required_work, &required_scratch)
        && sol_mir_concrete_internal_validate_measured(program, diagnostics,
            required_work, required_scratch, &measured_work, &measured_scratch)
        && measured_work == required_work
        && measured_scratch == required_scratch;
}
