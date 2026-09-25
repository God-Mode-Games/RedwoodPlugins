// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins FRedwoodHeldRequests, which holds the requests of one socket while it
// reconnects after a backend move (HollowedOath#2854). A request the player
// makes in those few seconds must not fail, and must not hang.
//   1. With no session, or with a ready socket, nothing is held: the request
//      goes through its own gate at once, as before.
//   2. A request made while the socket reconnects is held, and is sent
//      exactly once when the socket is back.
//   3. When the socket is not back within the grace, the held request fails
//      through its own gate, and later requests fail at once until the
//      socket is back.
//   4. A failed re-login fails the held requests at once.
//   5. The grace counts from the drop, not from the first request.
//   6. Reset drops the held requests without running them.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

#include "RedwoodHeldRequests.h"
#include "TimerManager.h"

namespace {
  // Models the shape of a URedwoodClientInterface request: ask the hold
  // first, then run the old connection gate, which sends or fails.
  struct FFakeSocket {
    bool bSessionEstablished = true;
    bool bReady = false;
    int32 SentCount = 0;
    int32 FailedCount = 0;
    FRedwoodHeldRequests Held;
    FTimerManager Timers;

    void Request() {
      if (Held.HoldIfReconnecting(
            bSessionEstablished, bReady, [this]() { Request(); }, Timers
          )) {
        return;
      }
      if (bReady) {
        ++SentCount;
      } else {
        ++FailedCount;
      }
    }

    // FTimerManager ticks at most once per engine frame, and a test runs
    // inside one frame, so each step advances the frame counter itself.
    void Advance(float Seconds) {
      ++GFrameCounter;
      Timers.Tick(Seconds);
    }
  };

  // Just under and just over the grace, so the test does not depend on
  // floating-point equality at the boundary.
  constexpr float BoundaryMarginSeconds = 0.1f;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodHeldRequestsTest,
  "Redwood.HeldRequests",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodHeldRequestsTest::RunTest(const FString &Parameters) {
  const uint64 SavedFrameCounter = GFrameCounter;
  ON_SCOPE_EXIT {
    GFrameCounter = SavedFrameCounter;
  };

  const float Grace = FRedwoodHeldRequests::GraceSeconds;

  {
    FFakeSocket Socket;
    Socket.bSessionEstablished = false;
    Socket.Request();
    TestEqual(TEXT("No session: fails at once"), Socket.FailedCount, 1);
    TestEqual(TEXT("No session: nothing held"), Socket.Held.Num(), 0);
  }

  {
    FFakeSocket Socket;
    Socket.bReady = true;
    Socket.Request();
    TestEqual(TEXT("Ready socket: sent at once"), Socket.SentCount, 1);
    TestEqual(TEXT("Ready socket: nothing held"), Socket.Held.Num(), 0);
  }

  {
    FFakeSocket Socket;
    Socket.Request();
    TestEqual(TEXT("Reconnecting: held"), Socket.Held.Num(), 1);
    TestEqual(TEXT("Reconnecting: not failed"), Socket.FailedCount, 0);

    Socket.bReady = true;
    Socket.Held.Release(Socket.Timers);
    TestEqual(TEXT("Released: sent once"), Socket.SentCount, 1);
    TestEqual(TEXT("Released: queue empty"), Socket.Held.Num(), 0);

    Socket.Advance(0.0f);
    Socket.Advance(Grace + BoundaryMarginSeconds);
    TestEqual(TEXT("Released: the grace no longer fires"), Socket.FailedCount, 0);
    TestEqual(TEXT("Released: still sent only once"), Socket.SentCount, 1);
  }

  {
    FFakeSocket Socket;
    Socket.Request();
    Socket.Advance(0.0f);
    Socket.Advance(Grace - BoundaryMarginSeconds);
    TestEqual(TEXT("Inside the grace: still held"), Socket.Held.Num(), 1);
    TestEqual(TEXT("Inside the grace: not failed"), Socket.FailedCount, 0);

    Socket.Advance(2.0f * BoundaryMarginSeconds);
    TestEqual(TEXT("Past the grace: failed once"), Socket.FailedCount, 1);
    TestEqual(TEXT("Past the grace: queue empty"), Socket.Held.Num(), 0);

    Socket.Request();
    TestEqual(TEXT("After the grace: fails at once"), Socket.FailedCount, 2);
    TestEqual(TEXT("After the grace: nothing held"), Socket.Held.Num(), 0);

    Socket.bReady = true;
    Socket.Request();
    Socket.bReady = false;
    Socket.Request();
    TestEqual(
      TEXT("Socket back then dropped again: held again"), Socket.Held.Num(), 1
    );
  }

  {
    FFakeSocket Socket;
    Socket.Request();
    Socket.Held.Expire(Socket.Timers);
    TestEqual(TEXT("Failed re-login: failed at once"), Socket.FailedCount, 1);
    TestEqual(TEXT("Failed re-login: queue empty"), Socket.Held.Num(), 0);
  }

  {
    FFakeSocket Socket;
    Socket.Held.StartGrace(Socket.Timers);
    Socket.Advance(0.0f);
    Socket.Advance(Grace - BoundaryMarginSeconds);
    Socket.Request();
    TestEqual(TEXT("Late in the grace: held"), Socket.Held.Num(), 1);

    Socket.Advance(2.0f * BoundaryMarginSeconds);
    TestEqual(
      TEXT("The grace counts from the drop: failed with it"),
      Socket.FailedCount,
      1
    );
  }

  {
    FFakeSocket Socket;
    Socket.Request();
    Socket.Held.Reset(Socket.Timers);
    TestEqual(TEXT("Reset: queue empty"), Socket.Held.Num(), 0);
    Socket.Advance(0.0f);
    Socket.Advance(Grace + BoundaryMarginSeconds);
    TestEqual(TEXT("Reset: nothing ran"), Socket.FailedCount + Socket.SentCount, 0);
  }

  return true;
}
