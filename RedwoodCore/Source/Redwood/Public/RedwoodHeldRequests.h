// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.

#pragma once

#include "CoreMinimal.h"

// HollowedOath#2854. Holds the requests made while the session is coming
// back after a drop, so an action the player takes during a backend move is
// sent after the resume instead of failing. A held request is a closure that
// calls its request function again: it goes through the request's gate once
// more, which sends it when the session is back and fails it when the
// session is over or its grace ran out. FRedwoodClientSession decides which;
// this is only the queue.
class REDWOOD_API FRedwoodHeldRequests {
public:
  // HollowedOath reads this for the grace of its disconnect modal
  // (ConnectionNotificationSubsystem), so a held request fails when the
  // player is told. It equals the session grace of Bifrost: a resume inside
  // it brings the session back as it was, so a request held that long is
  // still worth sending.
  static constexpr float GraceSeconds = 30.0f;

  void Add(TFunction<void()> Request);

  // The session is back, or what a held request waited for has come
  // (HollowedOath#3609): every held request runs again, and takes its gate
  // again.
  void Release();

  // The grace ran out, or the session ended: every held request runs again,
  // and fails through its own gate.
  void Expire();

  // The owner is going away: drop the held requests without running them.
  void Reset();

  int32 Num() const {
    return Requests.Num();
  }

private:
  void RunAll();

  TArray<TFunction<void()>> Requests;
};
