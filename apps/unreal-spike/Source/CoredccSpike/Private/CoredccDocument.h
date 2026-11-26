#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "CoredccDocument.generated.h"

/**
 * What UnrealHistory's changes are stored on: an FChange needs a UObject. One per coredcc
 * Library, transactional and rooted. `Value` is a plain UE property, for tests that mix UE's
 * own edits with coredcc's commands (S3).
 */
UCLASS(Transient)
class UCoredccDocument : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	int32 Value = 0;
};
