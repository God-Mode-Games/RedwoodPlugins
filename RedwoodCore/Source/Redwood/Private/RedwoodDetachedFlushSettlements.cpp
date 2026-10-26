// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.

#include "RedwoodDetachedFlushSettlements.h"

uint64 FRedwoodDetachedFlushSettlements::Begin(
  int32 PendingWrites,
  TFunction<void()> ReleaseWhenDelivered,
  TFunction<void()> OnSettled,
  TFunction<void()> OnAbandoned
) {
  const uint64 Id = ++State->NextId;
  FOperation &Operation = State->Pending.Add(Id);
  Operation.RemainingWrites = PendingWrites;
  Operation.ReleaseWhenDelivered = MoveTemp(ReleaseWhenDelivered);
  Operation.OnSettled = MoveTemp(OnSettled);
  Operation.OnAbandoned = MoveTemp(OnAbandoned);
  return Id;
}

TFunction<bool()> FRedwoodDetachedFlushSettlements::MakeIsPending(uint64 Id) const {
  TWeakPtr<FState> WeakState = State;
  return [WeakState, Id]() {
    const TSharedPtr<FState> Pinned = WeakState.Pin();
    return Pinned.IsValid() && Pinned->Pending.Contains(Id);
  };
}

TFunction<void()> FRedwoodDetachedFlushSettlements::MakeWriteSettled(uint64 Id) {
  TWeakPtr<FState> WeakState = State;
  return [WeakState, Id]() {
    const TSharedPtr<FState> Pinned = WeakState.Pin();
    if (!Pinned.IsValid()) {
      return;
    }
    FOperation *Operation = Pinned->Pending.Find(Id);
    // Already abandoned: a late ack must do nothing.
    if (Operation == nullptr || --Operation->RemainingWrites > 0) {
      return;
    }
    // Take it out first: the handlers can start a new flush.
    FOperation Done = MoveTemp(*Operation);
    Pinned->Pending.Remove(Id);
    Settle(Done, true);
  };
}

void FRedwoodDetachedFlushSettlements::Abandon(uint64 Id) {
  FOperation Done;
  if (!State->Pending.RemoveAndCopyValue(Id, Done)) {
    return;
  }
  Settle(Done, false);
}

void FRedwoodDetachedFlushSettlements::AbandonAll() {
  TMap<uint64, FOperation> Lost = MoveTemp(State->Pending);
  for (TPair<uint64, FOperation> &Entry : Lost) {
    Settle(Entry.Value, false);
  }
}

void FRedwoodDetachedFlushSettlements::Settle(
  FOperation &Operation, bool bDelivered
) {
  if (bDelivered && Operation.ReleaseWhenDelivered) {
    Operation.ReleaseWhenDelivered();
  }
  if (!bDelivered && Operation.OnAbandoned) {
    Operation.OnAbandoned();
  }
  if (Operation.OnSettled) {
    Operation.OnSettled();
  }
}
