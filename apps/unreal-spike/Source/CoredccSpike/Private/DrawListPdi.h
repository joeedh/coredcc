#pragma once

#include "CoreMinimal.h"
#include "RHIFeatureLevel.h"

#include "DrawListStats.h"

#include "CoredccIncludesBegin.h"
#include "coredcc/draw/drawlist.h"
#include "CoredccIncludesEnd.h"

class FCanvas;
class FMaterialRenderProxy;
class FPrimitiveDrawInterface;

/**
 * Draws a DrawList's 3D records through a PDI: Lines as translucent lines, Points as points,
 * Triangles and Shapes as dynamic meshes in `Material` (vertex-colored). Test depth goes in the
 * world group, Always and X-ray in the foreground group (X-ray's hidden pass isn't drawn).
 */
FDrawListStats DrawDrawListPdi(const coredcc::DrawList& List, FPrimitiveDrawInterface* PDI,
	const FMaterialRenderProxy* Material, ERHIFeatureLevel::Type FeatureLevel);

/** Its 2D records (Line2D, Circle2D) on a canvas. Returns how many it drew. */
int32 DrawDrawListHud(const coredcc::DrawList& List, FCanvas* Canvas);
