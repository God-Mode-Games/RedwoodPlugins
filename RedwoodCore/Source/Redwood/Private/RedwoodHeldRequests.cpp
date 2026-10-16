// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.

#include "RedwoodHeldRequests.h"

void FRedwoodHeldRequests::Add(TFunction<void()> Request) {
  Requests.Add(MoveTemp(Request));
}

void FRedwoodHeldRequests::Release() {
  RunAll();
}

void FRedwoodHeldRequests::Expire() {
  RunAll();
}

void FRedwoodHeldRequests::Reset() {
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
