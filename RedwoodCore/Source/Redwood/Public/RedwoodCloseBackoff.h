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
  // Each delay is the base delay times a random factor in 1 +/- this. One
  // stopping server closes many clients at once; without it they would all
  // reconnect in the same instant, again and again.
  static constexpr float JitterFraction = 0.3f;

  // Runs Reconnect after the next delay, and returns that delay. It never
  // gives up: a rolling deploy can close a client many times in a row, and
  // the client must come back when the backend does. Once the delay reaches
  // the maximum, it retries at the maximum.
  float Schedule(FTimerManager &TimerManager, TFunction<void()> Reconnect);

  // After a good re-login, and when the socket goes away.
  void Reset(FTimerManager &TimerManager);

  // The next delay before jitter.
  float BaseDelaySeconds() const;

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
