#ifndef SOL_TEST_PATH_WORK_H
#define SOL_TEST_PATH_WORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Exact test expectation B + C*P. Coefficients come from validator charge
 * sites, never observed owner usage. P is the physical prefix's byte length.
 * Failure leaves *out unchanged; no unsigned (P - baseline) subtraction. */
static inline bool sol_test_path_work(size_t base, size_t coefficient,
    size_t prefix_bytes, size_t *out) {
    if (out == NULL || (coefficient != 0 && prefix_bytes > SIZE_MAX / coefficient))
        return false;
    size_t product = coefficient * prefix_bytes;
    if (base > SIZE_MAX - product) return false;
    *out = base + product;
    return true;
}

#endif
