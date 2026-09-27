// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.

#include "RedwoodPendingReplies.h"

FRedwoodReplyCallback FRedwoodPendingReplies::Track(
  FRedwoodReplyCallback OnReply, TFunction<void()> OnLost
) {
  const uint64 Id = ++State->NextId;
  State->Pending.Add(Id, MoveTemp(OnLost));
  TWeakPtr<FState> WeakState = State;
  return [WeakState, Id, OnReply = MoveTemp(OnReply)](
           const TArray<TSharedPtr<FJsonValue>> &Response
         ) {
    const TSharedPtr<FState> Pinned = WeakState.Pin();
    // Already failed, dropped by Reset, or the owner is gone.
    if (!Pinned.IsValid() || Pinned->Pending.Remove(Id) == 0) {
      return;
    }
    OnReply(Response);
  };
}

void FRedwoodPendingReplies::FailAll() {
  // Take the map first: a failure handler can send a new request, which
  // must stay pending on the next connection.
  TMap<uint64, TFunction<void()>> Lost = MoveTemp(State->Pending);
  for (TPair<uint64, TFunction<void()>> &Entry : Lost) {
    Entry.Value();
  }
}

void FRedwoodPendingReplies::Reset() {
  State->Pending.Reset();
}
