#pragma once

#include "CoreMinimal.h"

/** What DrawDrawListPdi made of a DrawList. */
struct FDrawListStats
{
	int32 Lines = 0;
	int32 Points = 0;
	int32 Meshes = 0;
	/** 2D records, which DrawDrawListHud draws instead. */
	int32 Skipped2D = 0;
};
