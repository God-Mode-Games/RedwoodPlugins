// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.

#include "RedwoodCloseBackoff.h"

float FRedwoodCloseBackoff::Schedule(
  FTimerManager &TimerManager, TFunction<void()> Reconnect
) {
  const float DelaySeconds = NextDelaySeconds();
  ++Attempts;
  TimerManager.SetTimer(
    ReconnectTimer,
    FTimerDelegate::CreateLambda(MoveTemp(Reconnect)),
    DelaySeconds,
    false
  );
  return DelaySeconds;
}

void FRedwoodCloseBackoff::Reset(FTimerManager &TimerManager) {
  Attempts = 0;
  TimerManager.ClearTimer(ReconnectTimer);
}

float FRedwoodCloseBackoff::NextDelaySeconds() const {
  return FMath::Min(
    InitialDelaySeconds * FMath::Pow(DelayMultiplier, Attempts),
    MaxDelaySeconds
  );
}
