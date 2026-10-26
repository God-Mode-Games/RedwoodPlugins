// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.

#pragma once

#include "CoreMinimal.h"

// Completion registry for URedwoodServerGameSubsystem::FlushDetachedCharacterData, also used for
// the player-auth request of URedwoodGameModeComponent (an ack lost to a dropped socket must not
// leave the request pending forever).
//
// The socket library keeps an ack callback until the ack comes, and an ack never comes over a
// socket that dropped (see FRedwoodPendingReplies, the client twin of this). A caller that waits
// for OnSettled (the game's pending-release ownership) would then wait forever. Each operation
// settles exactly once, by one of:
//   - its last write ack (MakeWriteSettled): OnSettled(true).
//   - Abandon / MakeAbandon / AbandonAll (socket dropped, deadline passed, teardown):
//     OnSettled(false). What was waited for could not be delivered, so the caller must not act as
//     if it was (a late player-left would revoke a replacement session's binding).
// An ack that arrives after the operation settled is ignored, so nothing runs twice and nothing
// is emitted late. "Settled" is an emit/operation outcome, never a saved or realm-processed one.
// The registry owns no timers: the caller starts its own deadline and binds MakeAbandon to it.
// Game thread only, like the socket callbacks.
class REDWOOD_API FRedwoodPendingOperations {
public:
  // Starts an operation that waits for PendingWrites acks. bDelivered is true when the last ack
  // arrived and false when the operation was abandoned.
  uint64 Begin(int32 PendingWrites, TFunction<void(bool bDelivered)> OnSettled);

  // Reports whether the operation is pending without keeping the registry alive.
  TFunction<bool()> MakeIsPending(uint64 Id) const;

  // The callback for one write ack. It holds only the registry state, never the owner, so it can
  // outlive both.
  TFunction<void()> MakeWriteSettled(uint64 Id);

  // The callback that abandons one operation, for a deadline timer. It holds only the registry
  // state, so it needs no owner capture.
  TFunction<void()> MakeAbandon(uint64 Id);

  // Settles one operation as not delivered. No-op when it already settled.
  void Abandon(uint64 Id);

  // Settles every pending operation as not delivered.
  void AbandonAll();

  int32 Num() const {
    return State->Pending.Num();
  }

private:
  struct FOperation {
    int32 RemainingWrites = 0;
    TFunction<void(bool bDelivered)> OnSettled;
  };
  struct FState {
    TMap<uint64, FOperation> Pending;
    uint64 NextId = 0;
  };

  // Removes the operation before running it: its handler can start a new one.
  static void Settle(FState &State, uint64 Id, bool bDelivered);

  TSharedRef<FState> State = MakeShared<FState>();
};
