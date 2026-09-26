// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins FRedwoodCloseBackoff. A server that accepts and then closes with 1000
// again and again must not get a hot connect loop, and a client must still
// come back when the backend does:
//   1. Each reconnect waits, and the wait grows up to the maximum delay.
//   2. It never gives up: at the maximum, it keeps retrying at the maximum.
//   3. Reset starts again from the first delay, and drops a pending
//      reconnect.
//   4. The delays are spread by the jitter, so many clients that one server
//      closed do not all come back in the same instant.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "RedwoodCloseBackoff.h"
#include "TimerManager.h"

namespace RedwoodCloseBackoffTest {
  // Just under and just over each delay, so the test does not depend on
  // floating-point equality at the boundary.
  constexpr float BoundaryMarginSeconds = 0.05f;

  // Enough retries at the maximum to show that the backoff does not stop.
  constexpr int32 AttemptsToCheckAtMaximum = 3;

  // Enough first delays that equal draws from the jitter are not a chance.
  constexpr int32 JitterSamples = 20;

  bool IsJitteredFrom(float DelaySeconds, float BaseDelaySeconds) {
    return DelaySeconds >=
      BaseDelaySeconds * (1.0f - FRedwoodCloseBackoff::JitterFraction) &&
      DelaySeconds <=
      BaseDelaySeconds * (1.0f + FRedwoodCloseBackoff::JitterFraction);
  }

  struct FHarness {
    FRedwoodCloseBackoff Backoff;
    FTimerManager Timers;
    int32 Reconnects = 0;

    // A timer set inside a frame starts on the next tick, so the empty
    // tick lines the delay up with the test's own steps.
    float Schedule() {
      const float DelaySeconds =
        Backoff.Schedule(Timers, [this]() { ++Reconnects; });
      Advance(0.0f);
      return DelaySeconds;
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
  float ExpectedBaseDelay = FRedwoodCloseBackoff::InitialDelaySeconds;
  int32 AttemptsAtMaximum = 0;
  for (int32 Attempt = 1; AttemptsAtMaximum < AttemptsToCheckAtMaximum;
       ++Attempt) {
    const float DelaySeconds = Harness.Schedule();
    TestTrue(
      FString::Printf(TEXT("Attempt %d waits the grown delay"), Attempt),
      IsJitteredFrom(DelaySeconds, ExpectedBaseDelay)
    );
    TestTrue(
      FString::Printf(TEXT("Attempt %d is scheduled"), Attempt),
      Harness.Backoff.IsReconnectPending(Harness.Timers)
    );

    Harness.Advance(DelaySeconds - BoundaryMarginSeconds);
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

    if (ExpectedBaseDelay >= FRedwoodCloseBackoff::MaxDelaySeconds) {
      ++AttemptsAtMaximum;
    }
    ExpectedBaseDelay = FMath::Min(
      ExpectedBaseDelay * FRedwoodCloseBackoff::DelayMultiplier,
      FRedwoodCloseBackoff::MaxDelaySeconds
    );
  }

  const int32 ReconnectsBeforeReset = Harness.Reconnects;
  Harness.Backoff.Reset(Harness.Timers);
  TestTrue(
    TEXT("Reset starts from the first delay"),
    IsJitteredFrom(
      Harness.Schedule(), FRedwoodCloseBackoff::InitialDelaySeconds
    )
  );
  Harness.Backoff.Reset(Harness.Timers);
  Harness.Advance(2.0f * FRedwoodCloseBackoff::MaxDelaySeconds);
  TestEqual(
    TEXT("Reset drops a pending reconnect"),
    Harness.Reconnects,
    ReconnectsBeforeReset
  );

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCloseBackoffJitterTest,
  "Redwood.Socket.CloseBackoffJitter",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodCloseBackoffJitterTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCloseBackoffTest;

  FRedwoodCloseBackoff Backoff;
  FTimerManager Timers;
  float ShortestDelay = TNumericLimits<float>::Max();
  float LongestDelay = 0.0f;
  for (int32 Sample = 0; Sample < JitterSamples; ++Sample) {
    const float DelaySeconds = Backoff.Schedule(Timers, []() {});
    Backoff.Reset(Timers);
    TestTrue(
      TEXT("Each first delay stays inside the jitter"),
      IsJitteredFrom(DelaySeconds, FRedwoodCloseBackoff::InitialDelaySeconds)
    );
    ShortestDelay = FMath::Min(ShortestDelay, DelaySeconds);
    LongestDelay = FMath::Max(LongestDelay, DelaySeconds);
  }
  TestTrue(
    TEXT("The first delays are spread, not all the same"),
    LongestDelay > ShortestDelay
  );
  return true;
}
