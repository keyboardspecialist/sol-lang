#ifndef SOL_EFFECT_H
#define SOL_EFFECT_H

#include "sol/ast.h"

#include <stddef.h>

typedef enum {
    SOL_EFFECT_ATOM_NO_ARGUMENT,
    SOL_EFFECT_ATOM_STATIC_PATH,
    SOL_EFFECT_ATOM_PARAMETER,
    SOL_EFFECT_ATOM_SELF,
} SolEffectAtomArgumentKind;

typedef struct {
    SolSpan name;
    SolSpan argument;
    SolSpan span;
    SolEffectAtomArgumentKind argument_kind;
    SolParameterId parameter;
} SolEffectAtom;

typedef struct {
    SolEffectAtom *atoms;
    size_t count;
} SolEffectSet;

#endif
