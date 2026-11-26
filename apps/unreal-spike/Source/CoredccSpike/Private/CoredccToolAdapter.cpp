#include "CoredccToolAdapter.h"

#include "BaseBehaviors/ClickDragBehavior.h"
#include "BaseBehaviors/MouseHoverBehavior.h"
#include "InteractiveToolManager.h"
#include "Materials/Material.h"
#include "SceneView.h"
#include "ToolContextInterfaces.h"

#include "DrawListPdi.h"
#include "SpikeDoc.h"
#include "ViewFromSceneView.h"

#include "CoredccIncludesBegin.h"
#include "coredcc/command/executor.h"
#include "coredcc/tool/input.h"
#include "coredcc/tool/manager.h"
#include "CoredccIncludesEnd.h"

DEFINE_LOG_CATEGORY_STATIC(LogCoredccTool, Log, All);

void UCoredccToolAdapter::Setup()
{
	Super::Setup();
	UClickDragInputBehavior* Drag = NewObject<UClickDragInputBehavior>(this);
	Drag->Initialize(this);
	AddInputBehavior(Drag);
	UMouseHoverBehavior* Hover = NewObject<UMouseHoverBehavior>(this);
	Hover->Initialize(this);
	AddInputBehavior(Hover);
	if (Doc)
	{
		const coredcc::Status S = Doc->Tools->setActive(*Doc->Ctx, TCHAR_TO_UTF8(*CoredccTool));
		if (!S.ok()) UE_LOG(LogCoredccTool, Error, TEXT("setActive: %hs"), S.message());
	}
}

void UCoredccToolAdapter::Shutdown(EToolShutdownType ShutdownType)
{
	if (Doc) Doc->Tools->cancel(*Doc->Ctx).ignore();
	SyncTransaction();
	Super::Shutdown(ShutdownType);
}

void UCoredccToolAdapter::Send(int32 Type, const FInputDeviceRay& Ray, uint32 Buttons)
{
	if (!Doc || !Ray.bHas2D) return;
	coredcc::InputEvent E;
	E.type = coredcc::EventType(Type);
	E.x = Ray.ScreenPosition.X;
	E.y = Ray.ScreenPosition.Y;
	E.buttons = Buttons;
	E.time = FPlatformTime::Seconds();
	if (!View) return;
	const coredcc::Result<int32_t> R = Doc->Tools->dispatch(*Doc->Ctx, *View, E);
	if (!R.ok()) UE_LOG(LogCoredccTool, Warning, TEXT("dispatch: %hs"), R.status().message());
	SyncTransaction();
}

/* One UE transaction for each coredcc drag, so UE edits made during it join it, and UE's undo
 * is refused while it is open (UTransBuffer::CanUndo). */
void UCoredccToolAdapter::SyncTransaction()
{
	const bool bOpen = Doc && Doc->Ex->interactiveOpen();
	if (bOpen && !bTransactionOpen)
	{
		GetToolManager()->BeginUndoTransaction(FText::FromString(TEXT("coredcc drag")));
		bTransactionOpen = true;
	}
	else if (!bOpen && bTransactionOpen)
	{
		GetToolManager()->EndUndoTransaction();
		bTransactionOpen = false;
	}
}

FInputRayHit UCoredccToolAdapter::CanBeginClickDragSequence(const FInputDeviceRay&)
{
	return FInputRayHit(0.0);
}

void UCoredccToolAdapter::OnClickPress(const FInputDeviceRay& PressPos)
{
	Send(int32(coredcc::EventType::PointerDown), PressPos, 1);
}

void UCoredccToolAdapter::OnClickDrag(const FInputDeviceRay& DragPos)
{
	Send(int32(coredcc::EventType::PointerMove), DragPos, 1);
}

void UCoredccToolAdapter::OnClickRelease(const FInputDeviceRay& ReleasePos)
{
	Send(int32(coredcc::EventType::PointerUp), ReleasePos, 0);
}

void UCoredccToolAdapter::OnTerminateDragSequence()
{
	if (Doc) Doc->Tools->cancel(*Doc->Ctx).ignore();
	SyncTransaction();
}

FInputRayHit UCoredccToolAdapter::BeginHoverSequenceHitTest(const FInputDeviceRay&)
{
	return FInputRayHit(0.0);
}

void UCoredccToolAdapter::OnBeginHover(const FInputDeviceRay& DevicePos)
{
	Send(int32(coredcc::EventType::PointerMove), DevicePos, 0);
}

bool UCoredccToolAdapter::OnUpdateHover(const FInputDeviceRay& DevicePos)
{
	Send(int32(coredcc::EventType::PointerMove), DevicePos, 0);
	return true;
}

void UCoredccToolAdapter::OnEndHover()
{
}

void UCoredccToolAdapter::Render(IToolsContextRenderAPI* RenderAPI)
{
	if (!Doc || !RenderAPI) return;
	if (const FSceneView* SV = RenderAPI->GetSceneView())
	{
		coredcc::View V;
		if (ViewFromSceneView(*SV, V).ok()) View = std::make_shared<coredcc::View>(V);
	}
	if (!View) return;
	coredcc::DrawList List;
	const coredcc::Status S = Doc->Tools->draw(*Doc->Ctx, *View, List);
	if (!S.ok())
	{
		UE_LOG(LogCoredccTool, Warning, TEXT("draw: %hs"), S.message());
		return;
	}
	LastRecords = List.recordCount();
	const FSceneView* SV = RenderAPI->GetSceneView();
	const ERHIFeatureLevel::Type Level = SV ? SV->GetFeatureLevel() : GMaxRHIFeatureLevel;
	UMaterialInterface* Material = GetToolManager()->GetContextQueriesAPI()->GetStandardMaterial(EStandardToolContextMaterials::VertexColorMaterial);
	const FMaterialRenderProxy* Proxy = Material ? Material->GetRenderProxy() : nullptr;
	LastStats = DrawDrawListPdi(List, RenderAPI->GetPrimitiveDrawInterface(), Proxy, Level);
}

void UCoredccToolAdapter::DrawHUD(FCanvas* Canvas, IToolsContextRenderAPI*)
{
	if (!Doc || !View) return;
	coredcc::DrawList List;
	if (Doc->Tools->draw(*Doc->Ctx, *View, List).ok()) DrawDrawListHud(List, Canvas);
}

UInteractiveTool* UCoredccToolAdapterBuilder::BuildTool(const FToolBuilderState& SceneState) const
{
	UCoredccToolAdapter* Tool = NewObject<UCoredccToolAdapter>(SceneState.ToolManager);
	Tool->Doc = Doc;
	return Tool;
}
