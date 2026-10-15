// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins FRedwoodHeldRequests, the queue of the requests made while the
// session comes back after a drop (HollowedOath#2854). Which request is held,
// sent or failed is FRedwoodClientSession's decision, pinned in
// ClientSessionMachineTest; this file pins the queue alone:
//   1. A held request runs exactly once when released, and exactly once
//      when expired; the queue is empty after either.
//   2. A request that holds itself again while the queue runs waits for the
//      next run, and does not run twice in the same one.
//   3. Reset drops the held requests without running them.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "RedwoodHeldRequests.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodHeldRequestsTest,
  "Redwood.HeldRequests.Queue",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodHeldRequestsTest::RunTest(const FString &Parameters) {
  {
    FRedwoodHeldRequests Held;
    int32 Runs = 0;
    Held.Add([&Runs]() { ++Runs; });
    Held.Add([&Runs]() { ++Runs; });
    TestEqual(TEXT("Two held"), Held.Num(), 2);
    Held.Release();
    TestEqual(TEXT("Released: each ran once"), Runs, 2);
    TestEqual(TEXT("Released: queue empty"), Held.Num(), 0);
    Held.Release();
    TestEqual(TEXT("A second release runs nothing"), Runs, 2);
  }

  {
    FRedwoodHeldRequests Held;
    int32 Runs = 0;
    Held.Add([&Runs]() { ++Runs; });
    Held.Expire();
    TestEqual(TEXT("Expired: ran once"), Runs, 1);
    TestEqual(TEXT("Expired: queue empty"), Held.Num(), 0);
  }

  {
    // The shape of a request whose gate holds it again: the session is
    // still not back when the queue runs.
    FRedwoodHeldRequests Held;
    int32 Runs = 0;
    TFunction<void()> Request;
    Request = [&Held, &Runs, &Request]() {
      ++Runs;
      if (Runs == 1) {
        Held.Add(Request);
      }
    };
    Held.Add(Request);
    Held.Release();
    TestEqual(TEXT("Re-held: ran once in that run"), Runs, 1);
    TestEqual(TEXT("Re-held: waits for the next run"), Held.Num(), 1);
    Held.Release();
    TestEqual(TEXT("Re-held: ran again in the next run"), Runs, 2);
    TestEqual(TEXT("Re-held: then the queue is empty"), Held.Num(), 0);
  }

  {
    FRedwoodHeldRequests Held;
    int32 Runs = 0;
    Held.Add([&Runs]() { ++Runs; });
    Held.Reset();
    TestEqual(TEXT("Reset: queue empty"), Held.Num(), 0);
    Held.Release();
    TestEqual(TEXT("Reset: nothing ran"), Runs, 0);
  }

  return true;
}
