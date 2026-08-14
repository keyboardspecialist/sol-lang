#ifndef SOL_EFFECTS_H
#define SOL_EFFECTS_H

#include "sol/ir.h"

#include <stdbool.h>
#include <stdio.h>

bool sol_effects_render(FILE *stream, const SolIr *ir, bool json);

#endif
