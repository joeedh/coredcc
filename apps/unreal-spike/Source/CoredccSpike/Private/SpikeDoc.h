#pragma once

/*
 * A coredcc document as an Unreal host would hold one: a Runtime with the scene's commands,
 * a Library with one scene, a Context, and an executor over FUnrealHistory. Torn down in the
 * order bind/foreign_command.h asks for: executor and history before the Library and Runtime.
 */

#include "CoreMinimal.h"

#include "UnrealHistory.h"

#include "CoredccIncludesBegin.h"
#include "coredcc/api/context.h"
#include "coredcc/command/executor.h"
#include "coredcc/lib/library.h"
#include "CoredccIncludesEnd.h"

#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

class UCoredccDocument;

namespace coredcc
{
class ToolManager;
}

struct FSpikeDoc
{
	FSpikeDoc();
	~FSpikeDoc();

	using FInputs = std::initializer_list<std::pair<const char*, coredcc::PropValue>>;
	/** Runs a command through the executor; an error's text, or "" when it worked. */
	FString Run(const char* Command, FInputs Inputs = {}, coredcc::PropertyBag* Out = nullptr);
	/** A new object at a place; selected and active when `Select`. */
	coredcc::BlockHandle AddObject(const char* Name, double X, double Y, double Z, bool Select = true);
	/** Its world translation, as the scene computes it now. */
	FVector Where(coredcc::BlockHandle Object) const;
	/** The `objects` input that names these. */
	std::string Objects(std::initializer_list<coredcc::BlockHandle> Handles) const;

	std::unique_ptr<coredcc::Runtime> Rt;
	std::unique_ptr<coredcc::Library> Lib;
	std::unique_ptr<coredcc::Context> Ctx;
	UCoredccDocument* Document = nullptr;
	std::unique_ptr<FUnrealHistory> History;
	std::unique_ptr<coredcc::CommandExecutor> Ex;
	std::unique_ptr<coredcc::ToolManager> Tools;
	coredcc::BlockHandle Scene;
};
