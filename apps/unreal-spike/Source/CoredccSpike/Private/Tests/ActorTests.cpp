/*
 * S10 of docs/plans/2026-11-25-p4-tools.md: a coredcc object whose transform is an AActor's.
 * Its Transformable writes the actor's location without Modify (coredcc's undo restores it,
 * not UE's transaction) and marks the package dirty. Only the location is mapped: the spike
 * asks whether the undo works, not how to map a whole transform.
 */
#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Editor/Transactor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"

#include "../SpikeDoc.h"
#include "../CoredccIncludesBegin.h"
#include "coredcc/base/iface/owned.h"
#include "coredcc/base/iface/registry.h"
#include "coredcc/lib/registry.h"
#include "coredcc/scene/builtins.h"
#include "coredcc/scene/capabilities.h"
#include "coredcc/scene/commands.h"
#include "coredcc/scene/scene.h"
#include "coredcc/scene/transform_registry.h"
#include "litestl/util/alloc.h"
#include "../CoredccIncludesEnd.h"

using namespace coredcc;
namespace lbt = litestl::binding::types;

namespace
{
/* The block's data: the actor, and the channels coredcc keeps for it. */
struct FActorObject
{
	TWeakObjectPtr<AActor> Actor;
	ChannelBlock Channels;

	void read(ObjectInfo& Info, ChannelBlock& Out) const
	{
		Info = ObjectInfo();
		Info.transformType = TypeId::of(kTrsTransform);
		Out = Channels;
		if (const AActor* A = Actor.Get())
		{
			const FVector L = A->GetActorLocation();
			Out.set(trs::kLocation, L.X);
			Out.set(trs::kLocation + 1, L.Y);
			Out.set(trs::kLocation + 2, L.Z);
		}
	}
	Status writeChannels(const ChannelBlock& In)
	{
		AActor* A = Actor.Get();
		if (!A) return Status::error(StatusCode::NotFound, "the actor is gone");
		Channels = In;
		A->SetActorLocation(FVector(In.get(trs::kLocation), In.get(trs::kLocation + 1), In.get(trs::kLocation + 2)));
		A->MarkPackageDirty();
		return Status();
	}
	static lbt::Struct<FActorObject>* defineBindings()
	{
		auto* St = new lbt::Struct<FActorObject>("spike::ActorObject", sizeof(FActorObject));
		BIND_STRUCT_METHOD(St, read, MARGS("info", "channels"));
		BIND_STRUCT_METHOD(St, writeChannels, MARGS("channels"));
		return St;
	}
};

struct FActorProvider
{
	void* data(const Datablock& B) const { return B.clientData(); }
	void foreachRef(const Datablock&, RefVisitor&) const {}
	void destroy(const Datablock& B) { delete static_cast<FActorObject*>(B.clientData()); }
	static lbt::Struct<FActorProvider>* defineBindings()
	{
		auto* St = new lbt::Struct<FActorProvider>("spike::ActorProvider", sizeof(FActorProvider));
		BIND_STRUCT_METHOD(St, data, MARGS("block"));
		BIND_STRUCT_METHOD(St, foreachRef, MARGS("block", "visitor"));
		BIND_STRUCT_METHOD(St, destroy, MARGS("block"));
		return St;
	}
};

FString RegisterActorType(Runtime& Rt)
{
	const char* Impl = "spike.Actor#provider";
	Status S = registerImpl<BlockProvider, FActorProvider>(Rt, Impl);
	if (!S.ok()) return S.message();
	Result<Owned<BlockProvider>> P = own<BlockProvider>(Rt, Impl, litestl::alloc::New<FActorProvider>("spike::ActorProvider"));
	BlockTypeInfo Info;
	Info.name = "spike.Actor";
	Info.client = ClientId::of(kCoredccClient);
	Info.dataStruct = StructId::of(kSceneObjectStruct);
	Info.storage = BlockStorage::Transient;
	if (!P.ok()) return P.status().message();
	S = Rt.ext<BlockTypeRegistry>().add(Info, std::move(P).value()).status();
	if (!S.ok()) return S.message();
	S = registerImpl<Transformable, FActorObject>(Rt, "spike.Actor");
	return S.ok() ? FString() : FString(S.message());
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoredccS10, "Coredcc.History.S10 an actor as a coredcc object",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCoredccS10::RunTest(const FString&)
{
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		AddError(TEXT("no editor world"));
		return false;
	}
	AActor* Actor = World->SpawnActor<AActor>();
	USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Root"));
	Actor->SetRootComponent(Root);
	Root->RegisterComponent();
	Actor->SetActorLocation(FVector(100, 0, 0));
	GEditor->Trans->Reset(FText::FromString(TEXT("S10")));
	{
		FSpikeDoc D;
		TestEqual(TEXT("the block type"), RegisterActorType(*D.Rt), FString());
		FActorObject* Obj = new FActorObject;
		Obj->Actor = Actor;
		Obj->Channels = D.Rt->ext<TransformRegistry>().find(kTrsTransform)->defaults();
		Result<BlockHandle> H = D.Lib->create(TypeId::of("spike.Actor"), "actor", Obj);
		TestTrue(TEXT("the block"), H.ok());
		if (!H.ok()) return false;
		TestTrue(TEXT("in the scene"), Scene::of(*D.Lib, D.Scene)->add(*D.Lib, *H).ok());
		TestTrue(TEXT("reads the actor"), D.Where(*H).Equals(FVector(100, 0, 0), 1e-9));

		const FString E = D.Run(kApplyDeltaCommand,
			{{"vector", PropValue::ofVec3(0, 50, 0)}, {"objects", PropValue::ofString(D.Objects({*H}).c_str())}});
		TestEqual(TEXT("applyDelta"), E, FString());
		TestTrue(TEXT("the actor moved"), Actor->GetActorLocation().Equals(FVector(100, 50, 0), 1e-6));
		const UTransBuffer* B = CastChecked<UTransBuffer>(GEditor->Trans);
		TestEqual(TEXT("one transaction"), B->GetQueueLength(), 1);
		const FTransaction* T = B->GetTransaction(0);
		TestFalse(TEXT("UE's transaction doesn't record the actor too"), T && T->ContainsObject(Actor));
		TestTrue(TEXT("UE undo"), GEditor->UndoTransaction());
		TestTrue(TEXT("the actor is back"), Actor->GetActorLocation().Equals(FVector(100, 0, 0), 1e-6));
		TestTrue(TEXT("UE redo"), GEditor->RedoTransaction());
		TestTrue(TEXT("and moved again"), Actor->GetActorLocation().Equals(FVector(100, 50, 0), 1e-6));
		GEditor->Trans->Reset(FText::FromString(TEXT("S10")));
	}
	World->DestroyActor(Actor);
	return true;
}
