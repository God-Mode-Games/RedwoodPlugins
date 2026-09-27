// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins FRedwoodPendingReplies (HollowedOath#2886): a request gets its reply
// or its failure, exactly once, and never both.

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"

#include "RedwoodPendingReplies.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodPendingRepliesTest,
  "Redwood.Socket.PendingReplies",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodPendingRepliesTest::RunTest(const FString &Parameters) {
  const TArray<TSharedPtr<FJsonValue>> Reply;

  {
    FRedwoodPendingReplies Replies;
    int32 Replied = 0;
    int32 Lost = 0;
    FRedwoodReplyCallback Callback = Replies.Track(
      [&Replied](const TArray<TSharedPtr<FJsonValue>> &) { ++Replied; },
      [&Lost]() { ++Lost; }
    );
    Callback(Reply);
    Replies.FailAll();
    TestEqual(TEXT("A reply runs the callback"), Replied, 1);
    TestEqual(TEXT("An answered request is not failed"), Lost, 0);
    TestEqual(TEXT("Nothing stays pending"), Replies.Num(), 0);
  }

  {
    FRedwoodPendingReplies Replies;
    int32 Replied = 0;
    int32 Lost = 0;
    FRedwoodReplyCallback Callback = Replies.Track(
      [&Replied](const TArray<TSharedPtr<FJsonValue>> &) { ++Replied; },
      [&Lost]() { ++Lost; }
    );
    Replies.FailAll();
    Replies.FailAll();
    Callback(Reply);
    TestEqual(TEXT("A drop fails the request once"), Lost, 1);
    TestEqual(TEXT("A late reply after the failure is ignored"), Replied, 0);
  }

  {
    FRedwoodPendingReplies Replies;
    int32 Lost = 0;
    int32 Resent = 0;
    // A failure handler can send a new request; it stays pending.
    Replies.Track([](const TArray<TSharedPtr<FJsonValue>> &) {}, [&]() {
      ++Lost;
      Replies.Track(
        [](const TArray<TSharedPtr<FJsonValue>> &) {}, [&Resent]() { ++Resent; }
      );
    });
    Replies.FailAll();
    TestEqual(TEXT("The failed request ran its handler"), Lost, 1);
    TestEqual(TEXT("The new request is still pending"), Replies.Num(), 1);
    TestEqual(TEXT("The new request is not failed by the same drop"), Resent, 0);
  }

  {
    int32 Replied = 0;
    int32 Lost = 0;
    FRedwoodReplyCallback Callback;
    {
      FRedwoodPendingReplies Replies;
      Callback = Replies.Track(
        [&Replied](const TArray<TSharedPtr<FJsonValue>> &) { ++Replied; },
        [&Lost]() { ++Lost; }
      );
      Replies.Reset();
    }
    Callback(Reply);
    TestEqual(TEXT("Reset drops requests without failing them"), Lost, 0);
    TestEqual(
      TEXT("A reply after the owner is gone does not run"), Replied, 0
    );
  }
  return true;
}
