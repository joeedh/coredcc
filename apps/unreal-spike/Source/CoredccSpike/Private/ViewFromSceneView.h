#pragma once

#include "CoreMinimal.h"

#include "CoredccIncludesBegin.h"
#include "coredcc/draw/view.h"
#include "CoredccIncludesEnd.h"

class FSceneView;

/**
 * A coredcc View of an FSceneView: its unscaled view rect, its view matrix and its projection
 * without TAA's jitter (ComputeProjectionNoAAMatrix). UE's FMatrix M[r][c] applies to row vectors and
 * coredcc's mat4d to column vectors, so the 16 numbers are copied as they are. Conventions:
 * depth 0..1, reversed Z, an infinite far plane for perspective, a left-handed world with z up.
 */
coredcc::Status ViewFromSceneView(const FSceneView& SceneView, coredcc::View& Out);
