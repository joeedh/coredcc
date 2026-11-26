#pragma once

/*
 * One ITF tool that hosts coredcc's tools (the P4 spike's U-B): UE's click-drag and hover
 * behaviors become InputEvents for the document's ToolManager, a drag that a modal owns holds
 * one UE transaction open, and Render draws the ToolManager's DrawList through the PDI.
 */

#include "CoreMinimal.h"
#include "BaseBehaviors/BehaviorTargetInterfaces.h"
#include "InteractiveTool.h"
#include "InteractiveToolBuilder.h"

#include "DrawListStats.h"

#include <memory>

#include "CoredccToolAdapter.generated.h"

struct FSpikeDoc;
namespace coredcc
{
struct View;
}

UCLASS(Transient)
class UCoredccToolAdapter : public UInteractiveTool, public IClickDragBehaviorTarget, public IHoverBehaviorTarget
{
	GENERATED_BODY()

public:
	/** The document, and the coredcc tool to make active. */
	FSpikeDoc* Doc = nullptr;
	FString CoredccTool = TEXT("core.tool.transform");
	/** The view input is in: set from each Render's FSceneView, or by the host. Null until
	 * then, and events are dropped. */
	std::shared_ptr<coredcc::View> View;
	/** What the last Render drew. */
	FDrawListStats LastStats;
	int32 LastRecords = 0;

	void Setup() override;
	void Shutdown(EToolShutdownType ShutdownType) override;
	void Render(IToolsContextRenderAPI* RenderAPI) override;
	void DrawHUD(FCanvas* Canvas, IToolsContextRenderAPI* RenderAPI) override;

	FInputRayHit CanBeginClickDragSequence(const FInputDeviceRay& PressPos) override;
	void OnClickPress(const FInputDeviceRay& PressPos) override;
	void OnClickDrag(const FInputDeviceRay& DragPos) override;
	void OnClickRelease(const FInputDeviceRay& ReleasePos) override;
	void OnTerminateDragSequence() override;

	FInputRayHit BeginHoverSequenceHitTest(const FInputDeviceRay& PressPos) override;
	void OnBeginHover(const FInputDeviceRay& DevicePos) override;
	bool OnUpdateHover(const FInputDeviceRay& DevicePos) override;
	void OnEndHover() override;

private:
	/** Sends one event of `Type` (coredcc::EventType) at the ray's screen position. */
	void Send(int32 Type, const FInputDeviceRay& Ray, uint32 Buttons);
	/** Opens UE's transaction when a coredcc drag began, closes it when it ended. */
	void SyncTransaction();
	bool bTransactionOpen = false;
};

UCLASS(Transient)
class UCoredccToolAdapterBuilder : public UInteractiveToolBuilder
{
	GENERATED_BODY()

public:
	FSpikeDoc* Doc = nullptr;
	bool CanBuildTool(const FToolBuilderState&) const override { return Doc != nullptr; }
	UInteractiveTool* BuildTool(const FToolBuilderState& SceneState) const override;
};
