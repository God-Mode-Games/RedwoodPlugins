// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins FRedwoodCloseBackoff. A server that accepts and then closes with 1000
// again and again must not get a hot connect loop:
//   1. Each reconnect waits, and the wait grows up to the maximum delay.
//   2. After the maximum attempts, nothing more is scheduled.
//   3. Reset starts again from the first delay, and drops a pending
//      reconnect.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "RedwoodCloseBackoff.h"
#include "TimerManager.h"

namespace RedwoodCloseBackoffTest {
  // Just under and just over each delay, so the test does not depend on
  // floating-point equality at the boundary.
  constexpr float BoundaryMarginSeconds = 0.05f;

  struct FHarness {
    FRedwoodCloseBackoff Backoff;
    FTimerManager Timers;
    int32 Reconnects = 0;

    // A timer set inside a frame starts on the next tick, so the empty
    // tick lines the delay up with the test's own steps.
    bool Schedule() {
      const bool bScheduled =
        Backoff.Schedule(Timers, [this]() { ++Reconnects; });
      Advance(0.0f);
      return bScheduled;
    }

    // FTimerManager ticks at most once per engine frame, and a test runs
    // inside one frame, so each step advances the frame counter itself.
    void Advance(float Seconds) {
      ++GFrameCounter;
      Timers.Tick(Seconds);
    }
  };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCloseBackoffTest,
  "Redwood.Socket.CloseBackoff",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodCloseBackoffTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCloseBackoffTest;

  FHarness Harness;
  float ExpectedDelay = FRedwoodCloseBackoff::InitialDelaySeconds;
  for (int32 Attempt = 1; Attempt <= FRedwoodCloseBackoff::MaxAttempts;
       ++Attempt) {
    if (!TestTrue(
          FString::Printf(TEXT("Attempt %d is scheduled"), Attempt),
          Harness.Schedule()
        )) {
      return false;
    }

    Harness.Advance(ExpectedDelay - BoundaryMarginSeconds);
    TestEqual(
      FString::Printf(TEXT("Attempt %d waits its delay"), Attempt),
      Harness.Reconnects,
      Attempt - 1
    );
    Harness.Advance(2.0f * BoundaryMarginSeconds);
    TestEqual(
      FString::Printf(TEXT("Attempt %d reconnects after its delay"), Attempt),
      Harness.Reconnects,
      Attempt
    );

    ExpectedDelay = FMath::Min(
      ExpectedDelay * FRedwoodCloseBackoff::DelayMultiplier,
      FRedwoodCloseBackoff::MaxDelaySeconds
    );
  }
  TestEqual(
    TEXT("The delay stops at the maximum"),
    Harness.Backoff.NextDelaySeconds(),
    FRedwoodCloseBackoff::MaxDelaySeconds
  );

  TestFalse(TEXT("No attempt after the maximum"), Harness.Schedule());
  Harness.Advance(FRedwoodCloseBackoff::MaxDelaySeconds * 2.0f);
  TestEqual(
    TEXT("Nothing reconnects after the maximum"),
    Harness.Reconnects,
    FRedwoodCloseBackoff::MaxAttempts
  );

  Harness.Backoff.Reset(Harness.Timers);
  TestEqual(
    TEXT("Reset starts from the first delay"),
    Harness.Backoff.NextDelaySeconds(),
    FRedwoodCloseBackoff::InitialDelaySeconds
  );
  TestTrue(TEXT("Reset allows attempts again"), Harness.Schedule());
  Harness.Backoff.Reset(Harness.Timers);
  Harness.Advance(FRedwoodCloseBackoff::MaxDelaySeconds);
  TestEqual(
    TEXT("Reset drops a pending reconnect"),
    Harness.Reconnects,
    FRedwoodCloseBackoff::MaxAttempts
  );

  return true;
}
