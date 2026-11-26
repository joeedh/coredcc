#include "UnrealHistory.h"

#include "CoredccDocument.h"

#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Editor/Transactor.h"
#include "ScopedTransaction.h"

#include "CoredccIncludesBegin.h"
#include "coredcc/api/context.h"
#include "CoredccIncludesEnd.h"

DEFINE_LOG_CATEGORY_STATIC(LogCoredccHistory, Log, All);

using coredcc::CommandRecord;
using coredcc::RecordPtr;
using coredcc::Status;
using coredcc::StatusCode;

/** One coredcc command in UE's buffer. Owns its record until it expires or is destroyed. */
class FRecordChange final : public FCommandChange
{
public:
	FRecordChange(FUnrealHistory* InHistory, RecordPtr InRec) : History(InHistory), Rec(MoveTemp(InRec))
	{
		History->Changes.Add(this);
	}
	~FRecordChange() override
	{
		if (History)
		{
			if (Rec)
			{
				Rec->discard();
				History->Discards++;
			}
			History->Forget(this);
		}
	}
	void Apply(UObject*) override
	{
		if (History) History->ApplyChange(*this);
	}
	void Revert(UObject*) override
	{
		if (History) History->RevertChange(*this);
	}
	bool HasExpired(UObject*) const override { return !Rec; }
	SIZE_T GetSize() const override { return Rec ? SIZE_T(Rec->memSize()) : 0; }
	FString ToString() const override
	{
		return Rec ? FString(UTF8_TO_TCHAR(Rec->def().name().c_str())) : FString(TEXT("(expired)"));
	}

	FUnrealHistory* History;
	RecordPtr Rec;
};

namespace
{
UTransBuffer* Buffer()
{
	return GEditor ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
}
} // namespace

FUnrealHistory::FUnrealHistory(coredcc::Context& InCtx, UCoredccDocument* InDocument) : Ctx(InCtx), Document(InDocument)
{
	if (UTransBuffer* B = Buffer())
	{
		BeforeUndoRedo = B->OnBeforeRedoUndo().AddRaw(this, &FUnrealHistory::OnBeforeUndoRedo);
	}
}

FUnrealHistory::~FUnrealHistory()
{
	Shutdown();
}

void FUnrealHistory::Shutdown()
{
	if (UTransBuffer* B = Buffer(); B && BeforeUndoRedo.IsValid())
	{
		B->OnBeforeRedoUndo().Remove(BeforeUndoRedo);
	}
	BeforeUndoRedo.Reset();
	for (FRecordChange* C : TArray<FRecordChange*>(Changes))
	{
		Expire(*C);
		C->History = nullptr;
	}
	Changes.Reset();
	Last = nullptr;
	bFresh = false;
}

void FUnrealHistory::Forget(FRecordChange* Change)
{
	Changes.Remove(Change);
	if (Last == Change) Last = nullptr;
}

void FUnrealHistory::Expire(FRecordChange& Change)
{
	if (Change.Rec)
	{
		Change.Rec->discard();
		Discards++;
		Change.Rec.reset();
	}
	if (Last == &Change) Last = nullptr;
}

int32 FUnrealHistory::LiveChanges() const
{
	int32 N = 0;
	for (const FRecordChange* C : Changes) N += C->Rec ? 1 : 0;
	return N;
}

bool FUnrealHistory::HeadIsOurs() const
{
	const UTransBuffer* B = Buffer();
	if (!B || !Last || !Last->Rec || !bFresh || B->GetUndoCount() != 0 || B->GetQueueLength() == 0) return false;
	const FTransaction* Top = B->GetTransaction(B->GetQueueLength() - 1);
	return Top && Top->GetId() == LastTransaction;
}

void FUnrealHistory::push(RecordPtr Rec)
{
	/* Folding, as LinearHistory does: the head absorbs the next run of the same gesture. */
	if (HeadIsOurs() && Rec->command().hasFold() && Rec->subCount() == 0)
	{
		CommandRecord& Head = *Last->Rec;
		const uint64 Key = Rec->command().foldKey();
		const coredcc::StableRef* A = Head.prefix();
		const coredcc::StableRef* B = Rec->prefix();
		const bool SamePrefix = (!A && !B) || (A && B && *A == *B);
		if (Key != 0 && Head.subCount() == 0 && Head.command().hasFold() && Head.command().foldKey() == Key &&
			SamePrefix && Head.command().foldFrom(Rec->command()))
		{
			Head.adoptBags(*Rec);
			Rec->discard();
			Discards++;
			return;
		}
	}
	const FText Title = FText::FromString(UTF8_TO_TCHAR(
		Rec->def().uiNameText().size() == 0 ? Rec->def().name().c_str() : Rec->def().uiNameText().c_str()));
	FRecordChange* Change = nullptr;
	{
		FScopedTransaction Transaction(Title);
		TUniquePtr<FRecordChange> Owned = MakeUnique<FRecordChange>(this, MoveTemp(Rec));
		Change = Owned.Get();
		if (GUndo)
		{
			GUndo->StoreUndo(Document, MoveTemp(Owned));
		}
		else
		{
			/* No transaction (UE is undoing, or transactions are off): nothing can hold it. */
			UE_LOG(LogCoredccHistory, Warning, TEXT("push: no transaction to store '%s' in"), *Title.ToString());
			Change = nullptr;
		}
	}
	const UTransBuffer* B = Buffer();
	Last = Change;
	LastTransaction = B && B->GetQueueLength() > 0 ? B->GetTransaction(B->GetQueueLength() - 1)->GetId() : FGuid();
	bFresh = Change != nullptr;
}

void FUnrealHistory::pushBarrier()
{
	if (UTransBuffer* B = Buffer()) B->SetUndoBarrier();
	bFresh = false;
}

bool FUnrealHistory::canUndo() const
{
	UTransBuffer* B = Buffer();
	return B && B->CanUndo();
}

bool FUnrealHistory::canRedo() const
{
	UTransBuffer* B = Buffer();
	return B && B->CanRedo();
}

Status FUnrealHistory::undo(coredcc::Context&)
{
	if (!canUndo()) return Status::error(StatusCode::FailedPrecondition, "nothing to undo");
	LastError.Reset();
	bFresh = false;
	if (!GEditor->UndoTransaction()) return Status::error(StatusCode::FailedPrecondition, "Unreal refused the undo");
	return LastError.IsEmpty() ? Status() : Status::error(StatusCode::Internal, TCHAR_TO_UTF8(*LastError));
}

Status FUnrealHistory::redo(coredcc::Context&)
{
	if (!canRedo()) return Status::error(StatusCode::FailedPrecondition, "nothing to redo");
	LastError.Reset();
	bFresh = false;
	if (!GEditor->RedoTransaction()) return Status::error(StatusCode::FailedPrecondition, "Unreal refused the redo");
	return LastError.IsEmpty() ? Status() : Status::error(StatusCode::Internal, TCHAR_TO_UTF8(*LastError));
}

CommandRecord* FUnrealHistory::head()
{
	return HeadIsOurs() ? Last->Rec.get() : nullptr;
}

RecordPtr FUnrealHistory::popHead()
{
	if (!HeadIsOurs()) return RecordPtr();
	RecordPtr Rec = MoveTemp(Last->Rec);
	Last = nullptr;
	bFresh = false;
	return Rec;
}

void FUnrealHistory::clear()
{
	for (FRecordChange* C : TArray<FRecordChange*>(Changes)) Expire(*C);
	bFresh = false;
}

/* UE's undo: the record's undo. A failure can't be mirrored as LinearHistory does (dropping the
 * steps after it), so this step expires. */
void FUnrealHistory::RevertChange(FRecordChange& Change)
{
	bFresh = false;
	if (!Change.Rec) return;
	coredcc::Runtime& Rt = Ctx.runtime();
	const bool Stray = Rt.hasStickyError();
	Change.Rec->undo(Ctx);
	if (!Stray && Rt.hasStickyError())
	{
		const Status S = Rt.takeStickyError();
		LastError = FString(UTF8_TO_TCHAR(S.toString().c_str()));
		UE_LOG(LogCoredccHistory, Error, TEXT("undo of a coredcc command failed, so it expired: %s"), *LastError);
		Expire(Change);
	}
}

void FUnrealHistory::ApplyChange(FRecordChange& Change)
{
	bFresh = false;
	if (!Change.Rec) return;
	const Status S = Change.Rec->redo(Ctx);
	if (!S.ok())
	{
		LastError = FString(UTF8_TO_TCHAR(S.toString().c_str()));
		UE_LOG(LogCoredccHistory, Error, TEXT("redo of a coredcc command failed, so it expired: %s"), *LastError);
		Expire(Change);
	}
}

/* An undo or redo from Unreal while a coredcc drag is open, which nothing held a transaction
 * open for: the drag is cancelled first, as coredcc's own undo would. */
void FUnrealHistory::OnBeforeUndoRedo(const FTransactionContext&)
{
	coredcc::CommandExecutor* Ex = Ctx.executor();
	if (Ex && Ex->interactiveOpen())
	{
		const Status S = Ex->cancel(Ctx);
		UE_LOG(LogCoredccHistory, Display, TEXT("an Unreal undo cancelled the open coredcc drag (%hs)"),
			S.ok() ? "ok" : S.message());
	}
}
