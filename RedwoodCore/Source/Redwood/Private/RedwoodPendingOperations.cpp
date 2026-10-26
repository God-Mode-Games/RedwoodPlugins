// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.

#include "RedwoodPendingOperations.h"

uint64 FRedwoodPendingOperations::Begin(
  int32 PendingWrites, TFunction<void(bool bDelivered)> OnSettled
) {
  const uint64 Id = ++State->NextId;
  FOperation &Operation = State->Pending.Add(Id);
  Operation.RemainingWrites = PendingWrites;
  Operation.OnSettled = MoveTemp(OnSettled);
  return Id;
}

TFunction<bool()> FRedwoodPendingOperations::MakeIsPending(uint64 Id) const {
  TWeakPtr<FState> WeakState = State;
  return [WeakState, Id]() {
    const TSharedPtr<FState> Pinned = WeakState.Pin();
    return Pinned.IsValid() && Pinned->Pending.Contains(Id);
  };
}

TFunction<void()> FRedwoodPendingOperations::MakeWriteSettled(uint64 Id) {
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
    Settle(*Pinned, Id, true);
  };
}

TFunction<void()> FRedwoodPendingOperations::MakeAbandon(uint64 Id) {
  TWeakPtr<FState> WeakState = State;
  return [WeakState, Id]() {
    if (const TSharedPtr<FState> Pinned = WeakState.Pin()) {
      Settle(*Pinned, Id, false);
    }
  };
}

void FRedwoodPendingOperations::Abandon(uint64 Id) {
  Settle(*State, Id, false);
}

void FRedwoodPendingOperations::AbandonAll() {
  // Snapshot the ids: a handler can start a new operation, which must stay pending.
  TArray<uint64> Ids;
  State->Pending.GetKeys(Ids);
  for (const uint64 Id : Ids) {
    Settle(*State, Id, false);
  }
}

void FRedwoodPendingOperations::Settle(
  FState &State, uint64 Id, bool bDelivered
) {
  FOperation Done;
  if (!State.Pending.RemoveAndCopyValue(Id, Done)) {
    return;
  }
  if (Done.OnSettled) {
    Done.OnSettled(bDelivered);
  }
}
