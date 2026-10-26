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
// for OnSettled (the game's pending-release ownership) would then wait forever. Each flush with
// writes in flight is one operation here. It settles exactly once, by one of:
//   - its last write ack (WriteSettled): the release runs, then OnSettled.
//   - Abandon / AbandonAll (socket dropped, deadline passed, teardown): OnSettled runs and the
//     release does NOT. The release could not be delivered, or a replacement session may own the
//     character by now, and a late player-left would revoke its binding.
// An ack that arrives after the operation settled is ignored, so nothing runs twice and nothing
// is emitted late. "Settled" is an emit/operation outcome, never a saved or realm-processed one.
// Game thread only, like the socket callbacks.
class REDWOOD_API FRedwoodDetachedFlushSettlements {
public:
  // Starts an operation that waits for PendingWrites acks. ReleaseWhenDelivered may be null.
  // OnAbandoned (optional) runs only when the operation is abandoned, before OnSettled.
  uint64 Begin(
    int32 PendingWrites,
    TFunction<void()> ReleaseWhenDelivered,
    TFunction<void()> OnSettled,
    TFunction<void()> OnAbandoned = nullptr
  );

  // True while the operation has not settled.
  bool IsPending(uint64 Id) const {
    return State->Pending.Contains(Id);
  }

  // Same question for a callback that must not hold the registry itself.
  TFunction<bool()> MakeIsPending(uint64 Id) const;

  // The callback for one write ack. It holds only the registry state, never the owner, so it can
  // outlive both.
  TFunction<void()> MakeWriteSettled(uint64 Id);

  // Settles one operation without the release. No-op when it already settled.
  void Abandon(uint64 Id);

  // Settles every pending operation without the release.
  void AbandonAll();

  int32 Num() const {
    return State->Pending.Num();
  }

private:
  struct FOperation {
    int32 RemainingWrites = 0;
    TFunction<void()> ReleaseWhenDelivered;
    TFunction<void()> OnSettled;
    TFunction<void()> OnAbandoned;
  };
  struct FState {
    TMap<uint64, FOperation> Pending;
    uint64 NextId = 0;
  };

  static void Settle(FOperation &Operation, bool bDelivered);

  TSharedRef<FState> State = MakeShared<FState>();
};
