#pragma once

/*
 * Around every coredcc include in this module: two warnings that an Unreal Editor module makes
 * errors and that coredcc's headers trip (S11's findings, in the plan's U-A notes):
 * - C4458: litestl/binding/binding_struct.h, Struct::add's `name` and `type` hide members;
 * - C4668: Eigen (through litestl/math/matrix.h) tests GCC's macros in #if.
 * Unreal's own third-party includes are wrapped the same way (THIRD_PARTY_INCLUDES_START).
 */
#define COREDCC_INCLUDES_START __pragma(warning(push)) __pragma(warning(disable : 4458 4668))
#define COREDCC_INCLUDES_END __pragma(warning(pop))
