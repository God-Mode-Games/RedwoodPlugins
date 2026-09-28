// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"

using FRedwoodReplyCallback =
  TFunction<void(const TArray<TSharedPtr<FJsonValue>> &)>;

// HollowedOath#2886. The socket library keeps a reply callback until the
// reply comes, and a reply never comes over a socket that dropped. This
// wraps each callback so a drop fails what is still pending, once, with an
// error the caller can show. A late reply after that is ignored.
// Game thread only, like the socket callbacks (bCallbackOnGameThread).
class REDWOOD_API FRedwoodPendingReplies {
public:
  static constexpr const TCHAR *LostReplyError = TEXT(
    "The connection dropped before the server answered. The request may have run."
  );

  FRedwoodReplyCallback Track(
    FRedwoodReplyCallback OnReply, TFunction<void()> OnLost
  );

  // The socket dropped: every pending request fails now.
  void FailAll();

  // The owner goes away: drop the requests without running anything.
  void Reset();

  int32 Num() const {
    return State->Pending.Num();
  }

private:
  struct FState {
    TMap<uint64, TFunction<void()>> Pending;
    uint64 NextId = 0;
  };
  // Shared, so a callback that outlives its owner finds it gone.
  TSharedRef<FState> State = MakeShared<FState>();
};
