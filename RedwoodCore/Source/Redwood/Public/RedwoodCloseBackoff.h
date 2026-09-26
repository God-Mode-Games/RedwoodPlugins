// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.

#pragma once

#include "CoreMinimal.h"
#include "TimerManager.h"

// Spaces out the reconnects after normal closes that the client did not ask
// for. The socket library starts its own backoff again at every Connect(), so
// a server that accepts and then closes again would get a new connect, and a
// full re-login, at once and for good.
class REDWOOD_API FRedwoodCloseBackoff {
public:
  static constexpr float InitialDelaySeconds = 1.0f;
  static constexpr float DelayMultiplier = 2.0f;
  static constexpr float MaxDelaySeconds = 30.0f;
  // About a minute of closes in a row. After that the socket stays down and
  // the lost connection stands, so the game shows it.
  static constexpr int32 MaxAttempts = 6;

  // Runs Reconnect after the next delay. False when the attempts are spent.
  bool Schedule(FTimerManager &TimerManager, TFunction<void()> Reconnect);

  // After a good re-login, and when the socket goes away.
  void Reset(FTimerManager &TimerManager);

  float NextDelaySeconds() const;

  bool IsReconnectPending(const FTimerManager &TimerManager) const {
    return TimerManager.IsTimerActive(ReconnectTimer);
  }

  int32 NumAttempts() const {
    return Attempts;
  }

private:
  int32 Attempts = 0;
  FTimerHandle ReconnectTimer;
};
