/*
 * The spike's history half (U-A): S1 to S7 of docs/plans/2026-11-25-p4-tools.md. Each test makes
 * its own document, and starts and ends with an empty transaction buffer.
 */
#include "CoreMinimal.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Editor/Transactor.h"
#include "Misc/AutomationTest.h"
#include "ScopedTransaction.h"

#include "../CoredccDocument.h"
#include "../SpikeDoc.h"

#include "../CoredccIncludesBegin.h"
#include "coredcc/command/set_property.h"
#include "coredcc/scene/commands.h"
#include "coredcc/scene/transform.h"

#include "litestl/util/alloc.h"
#include "../CoredccIncludesEnd.h"

using namespace coredcc;

namespace
{
constexpr EAutomationTestFlags kFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

UTransBuffer* Buffer()
{
	return CastChecked<UTransBuffer>(GEditor->Trans);
}

void ResetBuffer()
{
	GEditor->Trans->Reset(FText::FromString(TEXT("coredcc spike")));
}

FString TitleAt(int32 FromTop)
{
	const UTransBuffer* B = Buffer();
	const FTransaction* T = B->GetTransaction(B->GetQueueLength() - 1 - FromTop);
	return T ? T->GetTitle().ToString() : FString();
}

/* A UE edit of `Doc`'s plain property, in a transaction of its own. */
void UnrealEdit(UCoredccDocument* Doc, int32 Value)
{
	FScopedTransaction T(FText::FromString(TEXT("UE edit")));
	Doc->Modify();
	Doc->Value = Value;
}

bool Near(const FVector& A, const FVector& B)
{
	return A.Equals(B, 1e-9);
}

PropValue Move(double X, double Y, double Z)
{
	return PropValue::ofVec3(X, Y, Z);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoredccS1, "Coredcc.History.S1 loads and frees", kFlags)
bool FCoredccS1::RunTest(const FString&)
{
	const size_t Before = litestl::alloc::getMemorySize();
	{
		FSpikeDoc D;
		TestTrue(TEXT("an object"), bool(D.AddObject("a", 1, 2, 3)));
	}
	ResetBuffer();
	TestEqual(TEXT("litestl's tracked bytes after the document is gone"), int64(litestl::alloc::getMemorySize()), int64(Before));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoredccS2, "Coredcc.History.S2 commands are transactions", kFlags)
bool FCoredccS2::RunTest(const FString&)
{
	ResetBuffer();
	FSpikeDoc D;
	TestEqual(TEXT("scene.new"), D.Run(kNewSceneCommand, {{"name", PropValue::ofString("Second")}}), FString());
	const BlockHandle A = D.AddObject("a", 1, 2, 3);
	TestEqual(TEXT("applyDelta"), D.Run(kApplyObjectDeltaCommand, {{"vector", Move(10, 0, 0)}}), FString());
	TestEqual(TEXT("three transactions"), Buffer()->GetQueueLength(), 3);
	AddInfo(FString::Printf(TEXT("titles: %s | %s | %s"), *TitleAt(2), *TitleAt(1), *TitleAt(0)));
	TestFalse(TEXT("named"), TitleAt(0).IsEmpty());
	TestTrue(TEXT("moved"), Near(D.Where(A), FVector(11, 2, 3)));

	TestTrue(TEXT("UE undo"), GEditor->UndoTransaction());
	TestTrue(TEXT("UE undo reverts"), Near(D.Where(A), FVector(1, 2, 3)));
	TestTrue(TEXT("UE redo"), GEditor->RedoTransaction());
	TestTrue(TEXT("UE redo redoes"), Near(D.Where(A), FVector(11, 2, 3)));
	TestTrue(TEXT("coredcc undo"), D.Ex->undo(*D.Ctx).ok());
	TestTrue(TEXT("coredcc undo reverts"), Near(D.Where(A), FVector(1, 2, 3)));
	TestTrue(TEXT("coredcc redo"), D.Ex->redo(*D.Ctx).ok());
	TestTrue(TEXT("coredcc redo redoes"), Near(D.Where(A), FVector(11, 2, 3)));
	TestTrue(TEXT("no errors"), D.History->LastError.IsEmpty());
	ResetBuffer();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoredccS3, "Coredcc.History.S3 interleaved with UE edits", kFlags)
bool FCoredccS3::RunTest(const FString&)
{
	ResetBuffer();
	FSpikeDoc D;
	UCoredccDocument* Other = NewObject<UCoredccDocument>(GetTransientPackage(), NAME_None, RF_Transactional);
	Other->AddToRoot();
	const BlockHandle A = D.AddObject("a", 0, 0, 0);
	ResetBuffer(); /* only the three below */
	D.Run(kApplyObjectDeltaCommand, {{"vector", Move(1, 0, 0)}});
	UnrealEdit(Other, 5);
	D.Run(kApplyObjectDeltaCommand, {{"vector", Move(0, 1, 0)}});
	TestEqual(TEXT("three transactions"), Buffer()->GetQueueLength(), 3);

	GEditor->UndoTransaction();
	TestTrue(TEXT("1st undo: the second move"), Near(D.Where(A), FVector(1, 0, 0)) && Other->Value == 5);
	GEditor->UndoTransaction();
	TestTrue(TEXT("2nd undo: the UE edit"), Near(D.Where(A), FVector(1, 0, 0)) && Other->Value == 0);
	GEditor->UndoTransaction();
	TestTrue(TEXT("3rd undo: the first move"), Near(D.Where(A), FVector(0, 0, 0)));
	GEditor->RedoTransaction();
	TestTrue(TEXT("1st redo"), Near(D.Where(A), FVector(1, 0, 0)) && Other->Value == 0);
	GEditor->RedoTransaction();
	TestTrue(TEXT("2nd redo"), Other->Value == 5);
	GEditor->RedoTransaction();
	TestTrue(TEXT("3rd redo"), Near(D.Where(A), FVector(1, 1, 0)));
	ResetBuffer();
	Other->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoredccS4, "Coredcc.History.S4 a gesture folds", kFlags)
bool FCoredccS4::RunTest(const FString&)
{
	ResetBuffer();
	FSpikeDoc D;
	const BlockHandle A = D.AddObject("a", 0, 0, 0);
	ResetBuffer();
	const uint64_t Gesture = beginGesture(*D.Rt);
	const char* Path = "scene.objects[0].transform.location";
	const Status S1 = setValueUndo(*D.Ctx, Path, PropValue::ofVec3(1, 0, 0), Gesture);
	const Status S2 = setValueUndo(*D.Ctx, Path, PropValue::ofVec3(2, 0, 0), Gesture);
	if (!S1.ok() || !S2.ok()) AddError(FString::Printf(TEXT("set: %hs / %hs"), S1.message(), S2.message()));
	TestEqual(TEXT("one transaction for the gesture"), Buffer()->GetQueueLength(), 1);
	TestTrue(TEXT("set"), Near(D.Where(A), FVector(2, 0, 0)));
	GEditor->UndoTransaction();
	TestTrue(TEXT("one undo takes both back"), Near(D.Where(A), FVector(0, 0, 0)));
	/* After an undo, nothing folds into what is left. */
	GEditor->RedoTransaction();
	setValueUndo(*D.Ctx, Path, PropValue::ofVec3(3, 0, 0), Gesture).ignore();
	TestEqual(TEXT("no fold after an undo and redo"), Buffer()->GetQueueLength(), 2);
	ResetBuffer();
	return true;
}

namespace
{
/* Begins a drag of the selection and makes ten updates, to (10, 0, 0) in all. */
FString Drag(FSpikeDoc& D)
{
	const CommandDef* Def = D.Rt->ext<CommandRegistry>().find(kDragObjectDeltaCommand);
	Status S = D.Ex->beginInteractive(*D.Ctx, *Def, Def->newInputs());
	if (!S.ok()) return FString(UTF8_TO_TCHAR(S.toString().c_str()));
	for (int I = 1; I <= 10; I++)
	{
		TransformDelta T;
		T.vector = double3(I, 0, 0);
		S = D.Ex->updateInteractive(*D.Ctx, [&](Context& C, Command& Cmd) { return Cmd.as<ApplyDelta>()->update(C, T); });
		if (!S.ok()) return FString(UTF8_TO_TCHAR(S.toString().c_str()));
	}
	return FString();
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoredccS5, "Coredcc.History.S5 drags", kFlags)
bool FCoredccS5::RunTest(const FString&)
{
	ResetBuffer();
	FSpikeDoc D;
	UCoredccDocument* Other = NewObject<UCoredccDocument>(GetTransientPackage(), NAME_None, RF_Transactional);
	Other->AddToRoot();
	const BlockHandle A = D.AddObject("a", 0, 0, 0);
	ResetBuffer();

	TestEqual(TEXT("drag"), Drag(D), FString());
	TestEqual(TEXT("no transaction while open"), Buffer()->GetQueueLength(), 0);
	TestTrue(TEXT("commit"), D.Ex->commit(*D.Ctx).ok());
	TestEqual(TEXT("one transaction"), Buffer()->GetQueueLength(), 1);
	TestTrue(TEXT("dragged"), Near(D.Where(A), FVector(10, 0, 0)));
	GEditor->UndoTransaction();
	TestTrue(TEXT("undone"), Near(D.Where(A), FVector(0, 0, 0)));
	ResetBuffer();

	TestEqual(TEXT("drag"), Drag(D), FString());
	TestTrue(TEXT("cancel"), D.Ex->cancel(*D.Ctx).ok());
	TestEqual(TEXT("a cancelled drag is no transaction"), Buffer()->GetQueueLength(), 0);
	TestTrue(TEXT("restored"), Near(D.Where(A), FVector(0, 0, 0)));

	/* With the ITF adapter's transaction open around the drag: UE refuses to undo. */
	GEditor->BeginTransaction(FText::FromString(TEXT("adapter")));
	TestEqual(TEXT("drag"), Drag(D), FString());
	TestFalse(TEXT("UE can't undo while a transaction is open"), GEditor->Trans->CanUndo());
	TestFalse(TEXT("undo refused"), GEditor->UndoTransaction());
	TestTrue(TEXT("the drag goes on"), D.Ex->interactiveOpen());
	TestTrue(TEXT("commit"), D.Ex->commit(*D.Ctx).ok());
	GEditor->EndTransaction();
	TestEqual(TEXT("the drag joined the adapter's transaction"), Buffer()->GetQueueLength(), 1);
	ResetBuffer();
	GEditor->UndoTransaction(); /* nothing: the buffer was reset */
	D.Run(kApplyObjectDeltaCommand, {{"vector", Move(-10, 0, 0)}});
	ResetBuffer();

	/* Without it: OnBeforeRedoUndo cancels the drag, then UE undoes its own step. */
	UnrealEdit(Other, 7);
	TestEqual(TEXT("drag"), Drag(D), FString());
	TestTrue(TEXT("UE undo"), GEditor->UndoTransaction());
	TestFalse(TEXT("the drag was cancelled"), D.Ex->interactiveOpen());
	TestTrue(TEXT("as before the drag"), Near(D.Where(A), FVector(0, 0, 0)));
	TestEqual(TEXT("UE's own step was undone too"), Other->Value, 0);
	ResetBuffer();
	Other->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoredccS6, "Coredcc.History.S6 discards and sizes", kFlags)
bool FCoredccS6::RunTest(const FString&)
{
	ResetBuffer();
	FSpikeDoc D;
	D.AddObject("a", 0, 0, 0);
	for (int I = 0; I < 3; I++) D.Run(kApplyObjectDeltaCommand, {{"vector", Move(1, 0, 0)}});
	AddInfo(FString::Printf(TEXT("UE's undo size: %llu bytes"), uint64(Buffer()->GetUndoSize())));
	TestTrue(TEXT("the buffer's size counts the records"), Buffer()->GetUndoSize() > 0);

	/* A truncated redo: two undone, then a new command. */
	const int32 Before = D.History->DiscardCount();
	GEditor->UndoTransaction();
	GEditor->UndoTransaction();
	D.Run(kApplyObjectDeltaCommand, {{"vector", Move(0, 1, 0)}});
	TestEqual(TEXT("each truncated record discarded once"), D.History->DiscardCount() - Before, 2);

	const int32 Live = D.History->LiveChanges();
	const int32 Before2 = D.History->DiscardCount();
	ResetBuffer();
	TestEqual(TEXT("Reset discards each record once"), D.History->DiscardCount() - Before2, Live);
	TestEqual(TEXT("none left"), D.History->LiveChanges(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoredccS7, "Coredcc.History.S7 rerunHead", kFlags)
bool FCoredccS7::RunTest(const FString&)
{
	ResetBuffer();
	FSpikeDoc D;
	UCoredccDocument* Other = NewObject<UCoredccDocument>(GetTransientPackage(), NAME_None, RF_Transactional);
	Other->AddToRoot();
	const BlockHandle A = D.AddObject("a", 0, 0, 0);
	D.Run(kApplyObjectDeltaCommand, {{"vector", Move(1, 0, 0)}});
	TestNotNull(TEXT("head"), D.History->head());
	const CommandDef* Def = D.Rt->ext<CommandRegistry>().find(kApplyObjectDeltaCommand);
	PropertyBag In = Def->newInputs();
	In.set("vector", Move(5, 0, 0)).ignore();
	const Status S = D.Ex->rerunHead(*D.Ctx, std::move(In));
	if (!S.ok()) AddError(FString::Printf(TEXT("rerunHead: %hs"), S.message()));
	TestTrue(TEXT("rerun with 5"), Near(D.Where(A), FVector(5, 0, 0)));
	GEditor->UndoTransaction();
	TestTrue(TEXT("one undo for the rerun"), Near(D.Where(A), FVector(0, 0, 0)));
	GEditor->RedoTransaction();

	UnrealEdit(Other, 3);
	TestNull(TEXT("no head after a UE edit"), D.History->head());
	PropertyBag In2 = Def->newInputs();
	TestFalse(TEXT("rerunHead refused"), D.Ex->rerunHead(*D.Ctx, std::move(In2)).ok());
	TestTrue(TEXT("unchanged"), Near(D.Where(A), FVector(5, 0, 0)));
	ResetBuffer();
	Other->RemoveFromRoot();
	return true;
}
