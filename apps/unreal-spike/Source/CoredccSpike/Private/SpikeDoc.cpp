#include "SpikeDoc.h"

#include "CoredccDocument.h"

#include "CoredccIncludes.h"
COREDCC_INCLUDES_START
#include "coredcc/api/ref_text.h"
#include "coredcc/api/struct.h"
#include "coredcc/scene/commands.h"
#include "coredcc/scene/object.h"
#include "coredcc/scene/scene.h"
#include "coredcc/tool/builtins.h"
#include "coredcc/tool/manager.h"
COREDCC_INCLUDES_END

using namespace coredcc;

namespace
{
void Must(const Status& S, const TCHAR* What)
{
	if (!S.ok())
	{
		UE_LOG(LogTemp, Fatal, TEXT("coredcc spike: %s: %hs"), What, S.message());
	}
}
} // namespace

FSpikeDoc::FSpikeDoc()
{
	Rt = std::make_unique<Runtime>();
	Must(registerSceneTypes(*Rt), TEXT("scene types"));
	Must(registerSceneCommands(*Rt), TEXT("scene commands"));
	Must(registerToolBuiltins(*Rt), TEXT("tools"));

	ApiRegistry& Api = Rt->ext<ApiRegistry>();
	ApiStruct Root("spike.Root");
	PointerAccessor SceneMember;
	SceneMember.target = StructId::of(kSceneType);
	SceneMember.get = [](const PointerRef&, Context& C) { return detail::blockRoot(C, C.scene()); };
	Root.addPointer(PropertyDef::pointerProp("scene", SceneMember.target), std::move(SceneMember));
	Must(Api.add(std::move(Root)).status(), TEXT("root struct"));
	Must(Api.addRuntimeOwner("root", StructId::of("spike.Root"), [this]() -> void* { return this; }).status(),
		TEXT("root owner"));

	Lib = std::make_unique<Library>(Rt.get());
	Ctx = std::make_unique<Context>(Rt.get(), Lib.get(), Api.runtimeOwner("root"));
	Result<BlockHandle> S = newScene(*Lib, "Scene");
	Must(S.status(), TEXT("scene"));
	Scene = *S;
	Ctx->setScene(Scene);

	Document = NewObject<UCoredccDocument>(GetTransientPackage(), NAME_None, RF_Transactional);
	Document->AddToRoot();
	History = std::make_unique<FUnrealHistory>(*Ctx, Document);
	Ex = std::make_unique<CommandExecutor>(Rt.get(), History.get());
	Ctx->setExecutor(Ex.get());
	Tools = std::make_unique<ToolManager>(*Rt);
	Ctx->setTools(Tools.get());
}

FSpikeDoc::~FSpikeDoc()
{
	Tools->shutdown(*Ctx);
	Ctx->setTools(nullptr);
	Tools.reset();
	if (Ex->interactiveOpen()) Ex->cancel(*Ctx).ignore();
	Ctx->setExecutor(nullptr);
	Ex.reset();
	History.reset();
	Ctx.reset();
	Lib.reset();
	Rt->takeStickyError().ignore();
	Rt.reset();
	Document->RemoveFromRoot();
}

FString FSpikeDoc::Run(const char* Command, FInputs Inputs, PropertyBag* Out)
{
	const CommandDef* Def = Rt->ext<CommandRegistry>().find(Command);
	if (!Def) return FString::Printf(TEXT("no command %hs"), Command);
	PropertyBag Bag = Def->newInputs();
	for (const auto& [Name, Value] : Inputs)
	{
		const Status S = Bag.set(Name, Value);
		if (!S.ok()) return FString(UTF8_TO_TCHAR(S.toString().c_str()));
	}
	Result<PropertyBag> R = Ex->run(*Ctx, *Def, std::move(Bag));
	if (!R.ok()) return FString(UTF8_TO_TCHAR(R.status().toString().c_str()));
	if (Out) *Out = std::move(R).value();
	return FString();
}

BlockHandle FSpikeDoc::AddObject(const char* Name, double X, double Y, double Z, bool Select)
{
	PropertyBag Out;
	const FString E = Run(kAddObjectCommand,
		{{"name", PropValue::ofString(Name)}, {"location", PropValue::ofVec3(X, Y, Z)}, {"select", PropValue::ofBool(Select)}},
		&Out);
	if (!E.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("AddObject: %s"), *E);
		return BlockHandle();
	}
	const StableRef* R = Out.get("object")->tryPointer();
	return R ? Lib->find(R->owner.block) : BlockHandle();
}

FVector FSpikeDoc::Where(BlockHandle Object) const
{
	Result<TransformState> S = computeState(*Lib, Object, 0.0);
	if (!S.ok()) return FVector(NAN, NAN, NAN);
	const double* W = S->world;
	return FVector(W[12], W[13], W[14]);
}

std::string FSpikeDoc::Objects(std::initializer_list<BlockHandle> Handles) const
{
	litestl::util::Vector<BlockId> Ids;
	for (BlockHandle H : Handles) Ids.append(Lib->get(H)->id());
	return formatBlockIds(*Rt, std::span<const BlockId>(Ids.data(), size_t(Ids.size())));
}
