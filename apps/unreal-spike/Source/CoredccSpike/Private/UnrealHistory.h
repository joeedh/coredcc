#pragma once

/*
 * UnrealHistory: coredcc's UndoHistory over Unreal's transaction buffer (the P4 spike,
 * docs/plans/2026-11-25-p4-tools.md, "The Unreal spike"). Each finished coredcc command is one
 * UE transaction holding an FCommandChange that owns the command's record: UE's undo reverts it,
 * UE's redo applies it again.
 */

#include "CoreMinimal.h"
#include "Misc/Change.h"

#include "CoredccIncludes.h"
COREDCC_INCLUDES_START
#include "coredcc/command/executor.h"
COREDCC_INCLUDES_END

class UCoredccDocument;
class FRecordChange;

class FUnrealHistory final : public coredcc::UndoHistory
{
public:
	/** `Ctx` is what every undo and redo runs in; it must outlive the history. */
	FUnrealHistory(coredcc::Context& Ctx, UCoredccDocument* Document);
	/** Expires every change it made, discarding its record (Shutdown). */
	~FUnrealHistory() override;

	void push(coredcc::RecordPtr Rec) override;
	void pushBarrier() override;
	bool canUndo() const override;
	bool canRedo() const override;
	coredcc::Status undo(coredcc::Context& Ctx) override;
	coredcc::Status redo(coredcc::Context& Ctx) override;
	coredcc::CommandRecord* head() override;
	coredcc::RecordPtr popHead() override;
	/** Expires every change: UE keeps the transactions, which then do nothing. */
	void clear() override;

	/** Before the Runtime goes: expires every change and releases the records. Doesn't reset
	 * UE's buffer, which may outlive the Runtime. */
	void Shutdown();

	/** Records discarded so far (S6). */
	int32 DiscardCount() const { return Discards; }
	/** Changes alive in UE's buffer that still hold a record. */
	int32 LiveChanges() const;
	/** The last error an undo or redo from UE left (Apply and Revert can't return one). */
	FString LastError;

private:
	friend class FRecordChange;
	void Forget(FRecordChange* Change);
	void Expire(FRecordChange& Change);
	void RevertChange(FRecordChange& Change);
	void ApplyChange(FRecordChange& Change);
	bool HeadIsOurs() const;
	void OnBeforeUndoRedo(const struct FTransactionContext& Context);

	coredcc::Context& Ctx;
	UCoredccDocument* Document;
	TArray<FRecordChange*> Changes;
	FRecordChange* Last = nullptr;
	FGuid LastTransaction;
	/** Nothing was undone or redone since the last push: folding and head() need that. */
	bool bFresh = false;
	int32 Discards = 0;
	FDelegateHandle BeforeUndoRedo;
};
