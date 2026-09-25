// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.

#include "RedwoodHeldRequests.h"

bool FRedwoodHeldRequests::HoldIfReconnecting(
  bool bSessionEstablished,
  bool bSocketReady,
  TFunction<void()> Request,
  FTimerManager &TimerManager
) {
  if (bSocketReady) {
    bGraceExpired = false;
    return false;
  }

  if (!bSessionEstablished || bGraceExpired) {
    return false;
  }

  Requests.Add(MoveTemp(Request));
  StartGrace(TimerManager);
  return true;
}

void FRedwoodHeldRequests::StartGrace(FTimerManager &TimerManager) {
  if (TimerManager.IsTimerActive(GraceTimer)) {
    return;
  }

  // A new drop gets a full grace, even when the last one ran out.
  bGraceExpired = false;

  TimerManager.SetTimer(
    GraceTimer,
    FTimerDelegate::CreateLambda([this, &TimerManager]() {
      Expire(TimerManager);
    }),
    GraceSeconds,
    false
  );
}

void FRedwoodHeldRequests::Release(FTimerManager &TimerManager) {
  TimerManager.ClearTimer(GraceTimer);
  bGraceExpired = false;
  RunAll();
}

void FRedwoodHeldRequests::Expire(FTimerManager &TimerManager) {
  TimerManager.ClearTimer(GraceTimer);
  bGraceExpired = true;
  RunAll();
}

void FRedwoodHeldRequests::Reset(FTimerManager &TimerManager) {
  TimerManager.ClearTimer(GraceTimer);
  bGraceExpired = false;
  Requests.Reset();
}

void FRedwoodHeldRequests::RunAll() {
  // A request can hold itself again while it runs, so run a moved-out copy.
  TArray<TFunction<void()>> ToRun = MoveTemp(Requests);
  Requests.Reset();
  for (TFunction<void()> &Request : ToRun) {
    Request();
  }
}
