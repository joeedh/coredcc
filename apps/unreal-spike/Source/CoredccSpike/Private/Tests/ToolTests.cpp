/*
 * The spike's tool half (U-B): S8 (the ITF adapter in a tools context) and S9 (a View from an
 * FSceneView). The FSceneView is made by the test: no viewport runs headless.
 */
#include "CoreMinimal.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "EngineUtils.h"
#include "InteractiveToolManager.h"
#include "InteractiveToolsContext.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "PrimitiveDrawInterface.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "ShowFlags.h"
#include "ToolContextInterfaces.h"

#include "../CoredccToolAdapter.h"
#include "../SpikeDoc.h"
#include "../ViewFromSceneView.h"

#include "../CoredccIncludesBegin.h"
#include "coredcc/draw/drawlist.h"
#include "coredcc/scene/scene.h"
#include "coredcc/tool/manager.h"
#include "../CoredccIncludesEnd.h"

using namespace coredcc;

namespace
{
constexpr EAutomationTestFlags kFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
constexpr int32 kWidth = 1280, kHeight = 720;

/* A camera 600 cm behind the origin and 300 up, looking at it, with a 90° field of view. */
struct FTestView
{
	FSceneViewFamily Family;
	TUniquePtr<FSceneView> View;

	FTestView()
		: Family(FSceneViewFamily::ConstructionValues(nullptr, nullptr, FEngineShowFlags(ESFIM_Editor)).SetTime(FGameTime()))
	{
		FSceneViewInitOptions O;
		O.ViewFamily = &Family;
		O.SetViewRectangle(FIntRect(0, 0, kWidth, kHeight));
		O.ViewOrigin = FVector(-600, -200, 300);
		const FRotator Look = (FVector(0, 0, 0) - O.ViewOrigin).Rotation();
		O.ViewRotationMatrix = FInverseRotationMatrix(Look) *
			FMatrix(FPlane(0, 0, 1, 0), FPlane(1, 0, 0, 0), FPlane(0, 1, 0, 0), FPlane(0, 0, 0, 1));
		O.ProjectionMatrix = FReversedZPerspectiveMatrix(PI / 4, float(kWidth), float(kHeight), 10.0f);
		View = MakeUnique<FSceneView>(O);
	}
};

/* FSceneView::WorldToPixel in double. */
bool WorldToPixelDouble(const FSceneView& V, const FVector& P, double& X, double& Y)
{
	const FMatrix& M = V.ViewMatrices.GetViewProjectionMatrix();
	double C[4];
	for (int Col = 0; Col < 4; Col++) C[Col] = P.X * M.M[0][Col] + P.Y * M.M[1][Col] + P.Z * M.M[2][Col] + M.M[3][Col];
	if (C[3] <= 0) return false;
	const FIntRect R = V.UnscaledViewRect;
	X = (0.5 + C[0] / C[3] * 0.5) * R.Width() + R.Min.X;
	Y = (0.5 - C[1] / C[3] * 0.5) * R.Height() + R.Min.Y;
	return true;
}

class FQueries : public IToolsContextQueriesAPI
{
public:
	UMaterialInterface* Material = nullptr;
	UWorld* GetCurrentEditingWorld() const override { return GEditor->GetEditorWorldContext().World(); }
	void GetCurrentSelectionState(FToolBuilderState& Out) const override
	{
		Out.World = GetCurrentEditingWorld();
		Out.ToolManager = ToolManager;
	}
	void GetCurrentViewState(FViewCameraState& Out) const override { Out = FViewCameraState(); }
	EToolContextCoordinateSystem GetCurrentCoordinateSystem() const override { return EToolContextCoordinateSystem::World; }
	FToolContextSnappingConfiguration GetCurrentSnappingSettings() const override { return FToolContextSnappingConfiguration(); }
	UMaterialInterface* GetStandardMaterial(EStandardToolContextMaterials) const override { return Material; }
	FViewport* GetHoveredViewport() const override { return nullptr; }
	FViewport* GetFocusedViewport() const override { return nullptr; }
	UInteractiveToolManager* ToolManager = nullptr;
};

/* UE's editor transactions, as the editor's own tools context forwards them. */
class FTransactions : public IToolsContextTransactionsAPI
{
public:
	void DisplayMessage(const FText& Message, EToolMessageLevel) override { UE_LOG(LogTemp, Display, TEXT("%s"), *Message.ToString()); }
	void PostInvalidation() override {}
	void BeginUndoTransaction(const FText& Description) override { GEditor->BeginTransaction(Description); }
	void EndUndoTransaction() override { GEditor->EndTransaction(); }
	void CancelUndoTransaction() override { GEditor->CancelTransaction(0); }
	void AppendChange(UObject* Target, TUniquePtr<FToolCommandChange> Change, const FText& Description) override
	{
		FScopedTransaction T(Description);
		if (GUndo) GUndo->StoreUndo(Target, MoveTemp(Change));
	}
	bool RequestSelectionChange(const FSelectedObjectsChangeList&) override { return false; }
};

/* Counts what Render emits. */
class FRecordingPdi : public FPrimitiveDrawInterface
{
public:
	explicit FRecordingPdi(const FSceneView* View) : FPrimitiveDrawInterface(View) {}
	~FRecordingPdi() override
	{
		for (FDynamicPrimitiveResource* R : Resources) R->ReleasePrimitiveResource();
	}
	bool IsHitTesting() override { return false; }
	void SetHitProxy(HHitProxy*) override {}
	void RegisterDynamicResource(FDynamicPrimitiveResource* R) override { Resources.Add(R); }
	void AddReserveLines(uint8, int32, bool, bool) override {}
	void DrawSprite(const FVector&, float, float, const FTexture*, const FLinearColor&, uint8, float, float, float, float, uint8, float) override {}
	void DrawLine(const FVector&, const FVector&, const FLinearColor&, uint8, float, float, bool) override { Lines++; }
	void DrawTranslucentLine(const FVector&, const FVector&, const FLinearColor&, uint8, float, float, bool) override { Lines++; }
	void DrawPoint(const FVector&, const FLinearColor&, float, uint8) override { Points++; }
	int32 DrawMesh(const FMeshBatch&) override
	{
		Meshes++;
		return 1;
	}
	int32 Lines = 0, Points = 0, Meshes = 0;
	TArray<FDynamicPrimitiveResource*> Resources;
};

class FRenderApi : public IToolsContextRenderAPI
{
public:
	FRenderApi(FPrimitiveDrawInterface* InPdi, const FSceneView* InView) : Pdi(InPdi), View(InView) {}
	FPrimitiveDrawInterface* GetPrimitiveDrawInterface() override { return Pdi; }
	const FSceneView* GetSceneView() override { return View; }
	FViewCameraState GetCameraState() override { return FViewCameraState(); }
	EViewInteractionState GetViewInteractionState() override { return EViewInteractionState::Focused; }
	FPrimitiveDrawInterface* Pdi;
	const FSceneView* View;
};

FInputDeviceRay RayAt(double X, double Y)
{
	return FInputDeviceRay(FRay(FVector::ZeroVector, FVector(1, 0, 0)), FVector2D(X, Y));
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoredccS9, "Coredcc.Tools.S9 a View from an FSceneView", kFlags)
bool FCoredccS9::RunTest(const FString&)
{
	FTestView TV;
	View V;
	const Status S = ViewFromSceneView(*TV.View, V);
	if (!S.ok()) AddError(FString::Printf(TEXT("validate: %hs"), S.message()));
	TestTrue(TEXT("infinite far"), V.infiniteFar);
	ViewMath VM;
	TestTrue(TEXT("ViewMath"), VM.set(V).ok());
	double Worst = 0, WorstUe = 0;
	for (const FVector& P : {FVector(0, 0, 0), FVector(100, 50, 20), FVector(-300, 400, -50)})
	{
		const ViewPoint Pt = VM.project(double3(P.X, P.Y, P.Z));
		double X, Y;
		TestTrue(TEXT("in front"), !Pt.behind && WorldToPixelDouble(*TV.View, P, X, Y));
		FVector2D Ue;
		TestTrue(TEXT("UE's WorldToPixel"), TV.View->WorldToPixel(P, Ue));
		Worst = FMath::Max(Worst, FMath::Max(FMath::Abs(Pt.x - X), FMath::Abs(Pt.y - Y)));
		WorstUe = FMath::Max(WorstUe, FMath::Max(FMath::Abs(Pt.x - Ue.X), FMath::Abs(Pt.y - Ue.Y)));
	}
	AddInfo(FString::Printf(TEXT("worst difference: %.3g px from a double WorldToPixel, %.3g px from UE's"), Worst, WorstUe));
	TestTrue(TEXT("within 1e-6 px of the double version"), Worst < 1e-6);
	TestTrue(TEXT("within 1e-3 px of UE's"), WorstUe < 1e-3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoredccS8, "Coredcc.Tools.S8 the ITF adapter drags the X arrow", kFlags)
bool FCoredccS8::RunTest(const FString&)
{
	GEditor->Trans->Reset(FText::FromString(TEXT("S8")));
	FSpikeDoc D;
	const BlockHandle A = D.AddObject("a", 0, 0, 0);
	TestTrue(TEXT("evaluate"), evaluateScene(*D.Ctx).ok());
	GEditor->Trans->Reset(FText::FromString(TEXT("S8")));

	FQueries Queries;
	FTransactions Transactions;
	UInteractiveToolsContext* TC = NewObject<UInteractiveToolsContext>();
	TC->AddToRoot();
	TC->Initialize(&Queries, &Transactions);
	Queries.ToolManager = TC->ToolManager;
	UCoredccToolAdapterBuilder* Builder = NewObject<UCoredccToolAdapterBuilder>(TC);
	Builder->Doc = &D;
	TC->ToolManager->RegisterToolType(TEXT("Coredcc"), Builder);
	TestTrue(TEXT("select the tool type"), TC->ToolManager->SelectActiveToolType(EToolSide::Left, TEXT("Coredcc")));
	TestTrue(TEXT("activate"), TC->ToolManager->ActivateTool(EToolSide::Left));
	UCoredccToolAdapter* Tool = Cast<UCoredccToolAdapter>(TC->ToolManager->GetActiveTool(EToolSide::Left));
	if (!TestNotNull(TEXT("the adapter"), Tool)) return false;

	FTestView TV;
	Tool->View = std::make_shared<View>();
	TestTrue(TEXT("view"), ViewFromSceneView(*TV.View, *Tool->View).ok());
	ViewMath VM;
	VM.set(*Tool->View).ignore();

	/* The X arrow: its Shape record, by its id (gizmo id × 65,536 + part 0). */
	DrawList List;
	TestTrue(TEXT("draw"), D.Tools->draw(*D.Ctx, *Tool->View, List).ok());
	DrawListReader Reader;
	TestTrue(TEXT("read"), Reader.read(List.bytes()).ok());
	int32 Shapes = 0, LineSegments = 0, Points = 0;
	double3 Base, Tip;
	bool bFound = false;
	for (const DrawRecord& It : Reader.items())
	{
		Shapes += It.kind == DrawRecordKind::Shape || It.kind == DrawRecordKind::Triangles;
		LineSegments += It.kind == DrawRecordKind::Lines ? int32(It.aux / 2) : 0;
		Points += It.kind == DrawRecordKind::Points ? int32(It.aux) : 0;
		if (It.kind == DrawRecordKind::Shape && It.style.id >= 65536 && It.style.id % 65536 == 0 && !bFound)
		{
			const double* M = It.matrix();
			const double3 O = Reader.origin();
			Base = double3(O[0] + M[12], O[1] + M[13], O[2] + M[14]);
			Tip = Base + double3(M[8], M[9], M[10]);
			bFound = true;
		}
	}
	if (!TestTrue(TEXT("found the X arrow"), bFound)) return false;
	const ViewPoint P0 = VM.project(Base + (Tip - Base) * 0.7);
	const ViewPoint P1 = VM.project(Tip);
	const double Len = FMath::Sqrt((P1.x - P0.x) * (P1.x - P0.x) + (P1.y - P0.y) * (P1.y - P0.y));
	const double Dx = (P1.x - P0.x) / Len * 100, Dy = (P1.y - P0.y) / Len * 100;

	/* ITF records activating the tool as a transaction of its own ("Activate Tool"). */
	const int32 Before = CastChecked<UTransBuffer>(GEditor->Trans)->GetQueueLength();
	Tool->OnClickPress(RayAt(P0.x, P0.y));
	TestTrue(TEXT("a drag began"), D.Ex->interactiveOpen());
	Tool->OnClickDrag(RayAt(P0.x + Dx / 2, P0.y + Dy / 2));
	Tool->OnClickDrag(RayAt(P0.x + Dx, P0.y + Dy));
	Tool->OnClickRelease(RayAt(P0.x + Dx, P0.y + Dy));
	TestFalse(TEXT("it ended"), D.Ex->interactiveOpen());
	const FVector Moved = D.Where(A);
	AddInfo(FString::Printf(TEXT("moved to %s"), *Moved.ToString()));
	TestTrue(TEXT("along x"), Moved.X > 1.0 && FMath::Abs(Moved.Y) < 1e-6 && FMath::Abs(Moved.Z) < 1e-6);
	{
		const UTransBuffer* B = CastChecked<UTransBuffer>(GEditor->Trans);
		FString Titles;
		for (int32 I = 0; I < B->GetQueueLength(); I++) Titles += B->GetTransaction(I)->GetContext().Title.ToString() + TEXT("; ");
		AddInfo(FString::Printf(TEXT("transactions: %s"), *Titles));
		TestEqual(TEXT("the drag made one transaction"), B->GetQueueLength() - Before, 1);
	}
	TestTrue(TEXT("undo"), GEditor->UndoTransaction());
	TestTrue(TEXT("back"), D.Where(A).Equals(FVector::ZeroVector, 1e-9));

	/* Render: what reaches the PDI against the list's records. */
	{
		FRecordingPdi Pdi(TV.View.Get());
		FRenderApi Api(&Pdi, TV.View.Get());
		Tool->Render(&Api);
		AddInfo(FString::Printf(TEXT("records %d; PDI lines %d, points %d, meshes %d (without a material)"), Tool->LastRecords,
			Pdi.Lines, Pdi.Points, Tool->LastStats.Meshes));
		TestEqual(TEXT("lines"), Pdi.Lines, LineSegments);
		TestEqual(TEXT("points"), Pdi.Points, Points);
		TestEqual(TEXT("meshes"), Tool->LastStats.Meshes, Shapes);
	}
	{
		Queries.Material = UMaterial::GetDefaultMaterial(MD_Surface);
		FRecordingPdi Pdi(TV.View.Get());
		FRenderApi Api(&Pdi, TV.View.Get());
		Tool->Render(&Api);
		AddInfo(FString::Printf(TEXT("with a material: DrawMesh %d times"), Pdi.Meshes));
		TestEqual(TEXT("one DrawMesh per mesh"), Pdi.Meshes, Shapes);
	}

	TC->ToolManager->DeactivateTool(EToolSide::Left, EToolShutdownType::Cancel);
	TC->Shutdown();
	TC->RemoveFromRoot();
	GEditor->Trans->Reset(FText::FromString(TEXT("S8")));
	return true;
}
