// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.

#pragma once

#include "CoreMinimal.h"
#include "TimerManager.h"

// HollowedOath#2854. Holds the requests of one socket while it reconnects, so
// an action the player takes during a backend move is sent after the re-login
// instead of failing. A held request is a closure that calls its request
// function again: when the socket is back it sends, and when the grace runs
// out it fails through the same connection gate as before, so nothing hangs.
class REDWOOD_API FRedwoodHeldRequests {
public:
  // Same as SocketDropGraceSeconds in HollowedOath: past it the game shows its
  // disconnect modal, so a held request must have failed by then.
  static constexpr float GraceSeconds = 10.0f;

  // True when the request is held and the caller must return. No session
  // means there is nothing to wait for, so the request fails at once as
  // before. bSocketReady is connected AND authenticated: a request sent
  // before the re-login reaches a server socket that does not know the
  // player, and the server refuses it.
  bool HoldIfReconnecting(
    bool bSessionEstablished,
    bool bSocketReady,
    TFunction<void()> Request,
    FTimerManager &TimerManager
  );

  // Call at the drop, so that the grace lines up with the game's own.
  void StartGrace(FTimerManager &TimerManager);

  // The socket is back and authenticated: send every held request.
  void Release(FTimerManager &TimerManager);

  // The grace ran out or the re-login failed: every held request fails now,
  // and later requests fail at once until the socket is back.
  void Expire(FTimerManager &TimerManager);

  // The owner is going away: drop the held requests without running them.
  void Reset(FTimerManager &TimerManager);

  int32 Num() const {
    return Requests.Num();
  }

private:
  void RunAll();

  TArray<TFunction<void()>> Requests;
  FTimerHandle GraceTimer;
  bool bGraceExpired = false;
};
