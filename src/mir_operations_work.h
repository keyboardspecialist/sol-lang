#ifndef SOL_MIR_OPERATIONS_WORK_H
#define SOL_MIR_OPERATIONS_WORK_H

/*
 * Internal build-work contract. A charge is made for every entered scan-loop
 * iteration, recursive source node, non-empty allocation, and populated
 * provenance record. Traversal code decides which events occur; these shared
 * constants only define their weights.
 */
enum {
    SOL_MIR_OPERATIONS_WORK_SCAN = 1,
    SOL_MIR_OPERATIONS_WORK_RECURSE = 1,
    SOL_MIR_OPERATIONS_WORK_ALLOCATE = 1,
    SOL_MIR_OPERATIONS_WORK_POPULATE = 1,
};

#endif
