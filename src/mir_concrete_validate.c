#include "sol/mir_concrete.h"

#include <stdlib.h>

static bool invalid(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics != NULL) sol_diagnostics_add(diagnostics,
        "SOL-MIR-CONCRETE-002", SOL_SEVERITY_ERROR, (SolSpan){0}, message);
    return false;
}

static bool validate_links(const SolMirConcreteProgram *program) {
    return program != NULL && program->program.ir != NULL
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
    if (materialization->image_count != plan->instance_count
        || materialization->import_count != plan->import_count
        || materialization->binding_count != plan->demand_count)
        return invalid(diagnostics, "plan/materialization census mismatch");
    if (representation->recipe_count != materialization->type_count)
        return invalid(diagnostics,
            "materialization/representation census mismatch");
    if (layout->type_count != representation->recipe_count
        || layout->field_count != representation->field_count
        || layout->variant_count != representation->variant_count
        || layout->projection_count != materialization->projection_count)
        return invalid(diagnostics, "representation/layout census mismatch");
    if (operations->access_plan_count != materialization->place_count
        || operations->import_envelope_count != materialization->import_count)
        return invalid(diagnostics, "materialization/operations census mismatch");
    if (linkage->callable_count != materialization->image_count
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
        if (plan->types[i].kind == SOL_IR_TYPE_PARAMETER
            || plan->types[i].kind == SOL_IR_TYPE_SELF)
            return invalid(diagnostics, "symbolic type remains executable");
    for (size_t i = 0; i < materialization->type_count; ++i)
        if (materialization->types[i].kind == SOL_IR_TYPE_PARAMETER
            || materialization->types[i].kind == SOL_IR_TYPE_SELF)
            return invalid(diagnostics, "materialized type is not concrete");

    for (size_t i = 0; i < materialization->image_count; ++i)
        if (materialization->images[i].instance != i)
            return invalid(diagnostics, "materialized image census is not dense");
    unsigned char *resolved = plan->demand_count == 0 ? NULL
        : calloc(plan->demand_count, 1);
    if (plan->demand_count != 0 && resolved == NULL) {
        if (diagnostics != NULL) diagnostics->allocation_failed = true;
        return invalid(diagnostics,
            "concrete demand validation allocation failed");
    }
    for (size_t i = 0; i < materialization->binding_count; ++i) {
        const SolMirMaterializedBinding *binding = &materialization->bindings[i];
        if (binding->source_demand >= plan->demand_count
            || resolved[binding->source_demand] != 0) {
            free(resolved);
            return invalid(diagnostics, "materialized demand is not closed");
        }
        resolved[binding->source_demand] = 1;
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
        if (resolved[demand] == 0) {
            free(resolved);
            return invalid(diagnostics, "planned demand is not uniquely resolved");
        }
    }
    free(resolved);

    size_t blocks = 0, constructors = 0, pattern_tests = 0;
    size_t pattern_extractions = 0, arithmetic = 0, snapshots = 0;
    size_t propagations = 0, predicates = 0;
    for (size_t i = 0; i < materialization->image_count; ++i) {
        const SolMirMaterializedImage *image = &materialization->images[i];
        if (image->blocks.offset != blocks
            || image->blocks.count > materialization->block_count - blocks)
            return invalid(diagnostics, "materialized block slices overlap");
        blocks += image->blocks.count;
    }
    if (blocks != materialization->block_count)
        return invalid(diagnostics, "materialized blocks are not consumed");
    for (size_t i = 0; i < materialization->instruction_count; ++i) {
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
    if (constructors != operations->constructor_count
        || pattern_tests != operations->pattern_test_count
        || pattern_extractions != operations->pattern_extraction_count
        || arithmetic != operations->arithmetic_count
        || snapshots != operations->snapshot_count
        || propagations != operations->propagation_count
        || predicates != operations->predicate_count)
        return invalid(diagnostics, "operation plan census mismatch");
    return true;
}

bool sol_mir_concrete_program_validate(const SolMirConcreteProgram *program,
    SolDiagnostics *diagnostics) {
    if (!validate_links(program))
        return invalid(diagnostics,
            "concrete program embedded predecessor links are malformed");
    if (!validate_census(program, diagnostics)) return false;
    if (!sol_mir_linkage_validate(&program->linkage, diagnostics))
        return invalid(diagnostics, "concrete program stage validation failed");
    if (!validate_closure(program, diagnostics)) return false;
    return true;
}
