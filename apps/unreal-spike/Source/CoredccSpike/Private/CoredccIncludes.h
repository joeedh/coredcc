#pragma once

/*
 * Around every coredcc include in this module, `#include "CoredccIncludesBegin.h"` and
 * `#include "CoredccIncludesEnd.h"` turn off two warnings that an Unreal Editor module makes
 * errors and that coredcc's headers trip (S11's findings, in the plan's U-A notes):
 * - C4458: litestl/binding/binding_struct.h, Struct::add's `name` and `type` hide members;
 * - C4668: Eigen (through litestl/math/matrix.h) tests GCC's macros in #if.
 * C4668 is also only a warning for the whole module (CoredccSpike.Build.cs), because engine
 * headers can reach litestl's math headers by name, outside these two files.
 */
