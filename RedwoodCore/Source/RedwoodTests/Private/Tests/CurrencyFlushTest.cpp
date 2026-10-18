// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart. Pins the game-server
// subsystem's half of the currency ledger: how amounts are read off the wire, what a flush claims
// from the component's queues, and what each item flush reply and each reload reply does to the
// component and to the PlayerState snapshot. CurrencyPersistenceTest.cpp pins the component half.
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "RedwoodCharacterComponent.h"
#include "RedwoodCurrencyTypes.h"
#include "RedwoodPlayerStateComponent.h"
#include "RedwoodServerGameSubsystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Named, not anonymous, for the same unity-build reason as ItemPersistenceTest.cpp.
namespace RedwoodCurrencyFlushTest {
  const FString EntryIdA = TEXT("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
  const FString EntryIdB = TEXT("BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB");
  const FString LootReason = TEXT("Currency.Reason.Loot");
  const FString QuestGrantKey = TEXT("quest:Q1");

  FRedwoodCurrencyEntry MakeEntry(const FString &Id, int64 Delta, int64 BalanceAfter) {
    FRedwoodCurrencyEntry Entry;
    Entry.Id = Id;
    Entry.Delta = Delta;
    Entry.BalanceAfter = BalanceAfter;
    Entry.Reason = LootReason;
    return Entry;
  }

  URedwoodCharacterComponent *MakeCharacterComponent(int64 InventorySeq) {
    URedwoodCharacterComponent *Component = NewObject<URedwoodCharacterComponent>();
    Component->bUseItems = true;
    Component->SeedItemSeqFromCharacter(InventorySeq);
    return Component;
  }

  // Headless: with no PlayerState owner the component only holds the snapshot, which is all the
  // reply steps touch.
  URedwoodPlayerStateComponent *MakePlayerStateComponent(int64 InventorySeq, int64 CurrencyAmount) {
    URedwoodPlayerStateComponent *Component = NewObject<URedwoodPlayerStateComponent>();
    Component->RedwoodCharacter.InventorySeq = InventorySeq;
    Component->RedwoodCharacter.CurrencyAmount = CurrencyAmount;
    return Component;
  }

  // Claims a batch the way FlushItemsForCharacterComponent does: the seq, then exactly the
  // currency the component says this batch carries.
  FRedwoodItemFlushSent ClaimBatch(FAutomationTestBase &Test, URedwoodCharacterComponent *Component) {
    FRedwoodItemFlushSent Sent;
    Test.TestTrue(TEXT("Claim succeeds"), Component->TryBeginItemFlush(Sent.BatchSeq));
    TArray<FRedwoodCurrencyEntry> Entries;
    TArray<FString> GrantKeys;
    URedwoodServerGameSubsystem::ClaimCurrencyToSend(Component, Entries, GrantKeys);
    Sent.CurrencyCount = Entries.Num();
    Sent.GrantKeys = GrantKeys;
    if (Entries.Num() > 0) {
      Sent.bHasBalanceAfter = true;
      Sent.BalanceAfter = Entries.Last().BalanceAfter;
    }
    return Sent;
  }

  FRedwoodItemFlushReply MakeReply(const FString &Error, int64 CommittedSeq) {
    FRedwoodItemFlushReply Reply;
    Reply.Error = Error;
    Reply.CommittedSeq = CommittedSeq;
    return Reply;
  }

  TSharedPtr<FJsonObject> ParseJson(const FString &Text) {
    TSharedPtr<FJsonObject> Object;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
    FJsonSerializer::Deserialize(Reader, Object);
    return Object;
  }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyAmountParseTest,
  "Redwood.Currency.AmountParse",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// The backend's parseAmount rule. Every string it refuses, the game server must refuse too.
bool FRedwoodCurrencyAmountParseTest::RunTest(const FString &Parameters) {
  const TCHAR *Refused[] = {
    TEXT("9223372036854775808"),
    TEXT("-9223372036854775809"),
    TEXT("-0"),
    TEXT("1e3"),
    TEXT(" 5"),
    TEXT("5 "),
    TEXT("+5"),
    TEXT(""),
    TEXT("-"),
    TEXT("05"),
    TEXT("1.5"),
  };
  for (const TCHAR *Text : Refused) {
    int64 Amount = 0;
    TestFalse(FString::Printf(TEXT("'%s' is refused"), Text), TryParseCurrencyAmount(Text, Amount));
  }

  struct FAccepted {
    const TCHAR *Text;
    int64 Expected;
  };
  const FAccepted Accepted[] = {
    {TEXT("0"), 0},
    {TEXT("-250"), -250},
    {TEXT("1000000"), 1000000},
    {TEXT("9223372036854775807"), MAX_int64},
    {TEXT("-9223372036854775808"), MIN_int64},
  };
  for (const FAccepted &Case : Accepted) {
    int64 Amount = 0;
    TestTrue(FString::Printf(TEXT("'%s' is accepted"), Case.Text), TryParseCurrencyAmount(Case.Text, Amount));
    TestEqual(FString::Printf(TEXT("'%s' reads exactly"), Case.Text), Amount, Case.Expected);
  }

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodItemFlushReplyParseTest,
  "Redwood.Currency.FlushReplyParse",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// Every field the backend can put on a flush reply reaches the component, and every field it can
// leave out reads as its safe value.
bool FRedwoodItemFlushReplyParseTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyFlushTest;

  const FRedwoodItemFlushReply Success = URedwoodServerGameSubsystem::ParseItemsFlushReply(
    Fixture::ParseJson(TEXT(R"({"error":null,"committedSeq":7,"alerts":[{"kind":"income","detail":{"reason":"Currency.Reason.Loot"}}]})"))
  );
  TestTrue(TEXT("A null error is success"), Success.Error.IsEmpty());
  TestEqual(TEXT("committedSeq"), Success.CommittedSeq, (int64)7);
  TestFalse(TEXT("final defaults to false"), Success.bFinal);
  TestFalse(TEXT("rebaseRequired defaults to false"), Success.bRebaseRequired);
  TestEqual(TEXT("One alert"), Success.Alerts.Num(), 1);
  if (Success.Alerts.Num() == 1) {
    TestEqual(TEXT("Alert kind"), Success.Alerts[0]->GetStringField(TEXT("kind")), FString(TEXT("income")));
  }

  const FRedwoodItemFlushReply Final = URedwoodServerGameSubsystem::ParseItemsFlushReply(
    Fixture::ParseJson(TEXT(R"({"error":"balance below zero","committedSeq":4,"final":true})"))
  );
  TestEqual(TEXT("Final error text"), Final.Error, FString(TEXT("balance below zero")));
  TestTrue(TEXT("final is read"), Final.bFinal);
  TestFalse(TEXT("A final refusal is not a rebase refusal"), Final.bRebaseRequired);

  const FRedwoodItemFlushReply Rebase = URedwoodServerGameSubsystem::ParseItemsFlushReply(
    Fixture::ParseJson(TEXT(R"({"error":"batch seq was used by a trade","committedSeq":5,"rebaseRequired":true})"))
  );
  TestTrue(TEXT("rebaseRequired is read"), Rebase.bRebaseRequired);
  TestFalse(TEXT("notApplied defaults to false"), Rebase.bNotApplied);

  const FRedwoodItemFlushReply NotApplied = URedwoodServerGameSubsystem::ParseItemsFlushReply(
    Fixture::ParseJson(TEXT(R"({"error":"batch was never applied","committedSeq":4,"notApplied":true})"))
  );
  TestTrue(TEXT("notApplied is read"), NotApplied.bNotApplied);
  TestFalse(TEXT("writerFenceLost defaults to false"), NotApplied.bWriterFenceLost);

  const FRedwoodItemFlushReply FenceLost = URedwoodServerGameSubsystem::ParseItemsFlushReply(
    Fixture::ParseJson(TEXT(R"({"error":"character is owned by another session","committedSeq":4,"writerFenceLost":true})"))
  );
  TestTrue(TEXT("writerFenceLost is read"), FenceLost.bWriterFenceLost);
  TestFalse(TEXT("A rebase refusal is not final"), Rebase.bFinal);

  // A dropped socket's reply has no fence. -1, never 0: 0 is a real fence and would read as a gap.
  const FRedwoodItemFlushReply NoFence = URedwoodServerGameSubsystem::ParseItemsFlushReply(
    Fixture::ParseJson(TEXT(R"({"error":"socket closed"})"))
  );
  TestEqual(TEXT("A missing committedSeq reads as -1"), NoFence.CommittedSeq, (int64)-1);
  TestFalse(TEXT("timedOut defaults to false"), NoFence.bTimedOut);
  TestFalse(TEXT("notForwarded defaults to false"), NoFence.bNotForwarded);

  // FORK(hollowed-oath): the sidecar's flags decide, not its text, and its filler fence of 0 is
  // dropped: read as a fence it would be a sequence gap.
  const FRedwoodItemFlushReply TimedOut = URedwoodServerGameSubsystem::ParseItemsFlushReply(
    Fixture::ParseJson(TEXT(R"({"error":"any text","committedSeq":0,"timedOut":true})"))
  );
  TestTrue(TEXT("timedOut is read from the flag"), TimedOut.bTimedOut);
  TestFalse(TEXT("A timeout was forwarded"), TimedOut.bNotForwarded);
  TestEqual(TEXT("A timeout's filler fence is dropped"), TimedOut.CommittedSeq, (int64)-1);
  const FRedwoodItemFlushReply NotForwarded = URedwoodServerGameSubsystem::ParseItemsFlushReply(
    Fixture::ParseJson(TEXT(R"({"error":"any text","committedSeq":0,"notForwarded":true})"))
  );
  TestTrue(TEXT("notForwarded is read from the flag"), NotForwarded.bNotForwarded);
  TestFalse(TEXT("An unsent batch did not time out"), NotForwarded.bTimedOut);
  TestEqual(TEXT("An unsent batch's filler fence is dropped"), NotForwarded.CommittedSeq, (int64)-1);
  const FRedwoodItemFlushReply FlagWins = URedwoodServerGameSubsystem::ParseItemsFlushReply(
    Fixture::ParseJson(TEXT(R"({"error":"Item flush request timed out.","committedSeq":0,"timedOut":false,"notForwarded":true})"))
  );
  TestFalse(TEXT("A present flag wins over the text"), FlagWins.bTimedOut);
  TestTrue(TEXT("The other flag is read"), FlagWins.bNotForwarded);

  // The realm's own "may have applied" is an unknown outcome: no refusal and no fence, whatever
  // else the reply says, so the batch goes out again as a probe.
  const FRedwoodItemFlushReply CommitUnknown = URedwoodServerGameSubsystem::ParseItemsFlushReply(
    Fixture::ParseJson(TEXT(R"({"error":null,"committedSeq":7,"final":true,"commitUnknown":true})"))
  );
  TestTrue(TEXT("commitUnknown is read"), CommitUnknown.bCommitUnknown);
  TestFalse(TEXT("commitUnknown is no success"), CommitUnknown.Error.IsEmpty());
  TestFalse(TEXT("commitUnknown is no final refusal"), CommitUnknown.bFinal);
  TestEqual(TEXT("commitUnknown carries no fence"), CommitUnknown.CommittedSeq, (int64)-1);

  // A sidecar that predates the flags: the text decides, with a Warning.
  AddExpectedErrorPlain(TEXT("read the outcome from its text"), EAutomationExpectedErrorFlags::Contains, 2);
  const FRedwoodItemFlushReply OldTimedOut = URedwoodServerGameSubsystem::ParseItemsFlushReply(
    Fixture::ParseJson(TEXT(R"({"error":"Item flush request timed out.","committedSeq":0})"))
  );
  TestTrue(TEXT("An old sidecar's timeout is read from its text"), OldTimedOut.bTimedOut);
  TestEqual(TEXT("And its filler fence is dropped"), OldTimedOut.CommittedSeq, (int64)-1);
  const FRedwoodItemFlushReply OldNotForwarded = URedwoodServerGameSubsystem::ParseItemsFlushReply(
    Fixture::ParseJson(TEXT(R"({"error":"Realm server not connected","committedSeq":0})"))
  );
  TestTrue(TEXT("An old sidecar's unsent batch is read from its text"), OldNotForwarded.bNotForwarded);

  // No reply object at all: an error, so the batch is kept.
  const FRedwoodItemFlushReply Unreadable =
    URedwoodServerGameSubsystem::ParseItemsFlushReply(TSharedPtr<FJsonObject>());
  TestFalse(TEXT("An unreadable reply is an error"), Unreadable.Error.IsEmpty());
  TestEqual(TEXT("An unreadable reply has no fence"), Unreadable.CommittedSeq, (int64)-1);

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyClaimTakesToSendCountsTest,
  "Redwood.Currency.ClaimTakesToSendCounts",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// The flush copies exactly the front of the queues that the component names, so a re-sent
// unconfirmed batch goes out unchanged and a newer entry waits for the next batch.
bool FRedwoodCurrencyClaimTakesToSendCountsTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyFlushTest;
  URedwoodCharacterComponent *Component = Fixture::MakeCharacterComponent(1);

  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdA, 500, 500)}, {Fixture::QuestGrantKey});
  const FRedwoodItemFlushSent First = Fixture::ClaimBatch(*this, Component);
  TestEqual(TEXT("The first batch carries the entry"), First.CurrencyCount, 1);
  TestEqual(TEXT("The first batch carries the key"), First.GrantKeys.Num(), 1);
  TestEqual(TEXT("The balance the batch commits"), First.BalanceAfter, (int64)500);

  // The outcome is unknown, and B is staged before the retry.
  URedwoodServerGameSubsystem::ApplyItemsFlushReply(
    Component, nullptr, Fixture::MakeReply(TEXT("socket closed"), -1), First
  );
  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdB, 200, 700)}, {});

  const FRedwoodItemFlushSent Retry = Fixture::ClaimBatch(*this, Component);
  TestEqual(TEXT("The retry carries only the unconfirmed entry"), Retry.CurrencyCount, 1);
  TestEqual(TEXT("The retry carries the unconfirmed key"), Retry.GrantKeys.Num(), 1);
  TestEqual(TEXT("The retry goes under the same seq"), Retry.BatchSeq, First.BatchSeq);
  TestEqual(TEXT("The retry's balance is A's, not B's"), Retry.BalanceAfter, (int64)500);

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyFlushSuccessWritesSnapshotTest,
  "Redwood.Currency.FlushSuccessWritesSnapshot",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A committed batch moves the snapshot's seq and balance, so a recreated component chains from
// what the backend holds. An entry staged mid-flight stays queued.
bool FRedwoodCurrencyFlushSuccessWritesSnapshotTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyFlushTest;
  URedwoodCharacterComponent *Component = Fixture::MakeCharacterComponent(3);
  URedwoodPlayerStateComponent *PlayerState = Fixture::MakePlayerStateComponent(3, 1000);

  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdA, -250, 750)}, {});
  const FRedwoodItemFlushSent Sent = Fixture::ClaimBatch(*this, Component);
  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdB, 50, 800)}, {});

  URedwoodServerGameSubsystem::ApplyItemsFlushReply(
    Component, PlayerState, Fixture::MakeReply(TEXT(""), Sent.BatchSeq), Sent
  );

  TestEqual(TEXT("The snapshot seq moves to the commit"), PlayerState->RedwoodCharacter.InventorySeq, (int64)4);
  TestEqual(TEXT("The snapshot balance is the committed one"), PlayerState->RedwoodCharacter.CurrencyAmount, (int64)750);
  const TArray<FRedwoodCurrencyEntry> &Pending = Component->GetPendingCurrencyEntries();
  TestEqual(TEXT("Only the mid-flight entry remains"), Pending.Num(), 1);
  if (Pending.Num() == 1) {
    TestEqual(TEXT("The remaining entry is B"), Pending[0].Id, Fixture::EntryIdB);
  }

  // A replay acked with a later fence: something else (a trade) committed after this batch, so
  // its BalanceAfter is no longer the balance and must not reach the snapshot.
  const FRedwoodItemFlushSent Second = Fixture::ClaimBatch(*this, Component);
  URedwoodServerGameSubsystem::ApplyItemsFlushReply(
    Component, PlayerState, Fixture::MakeReply(TEXT(""), Second.BatchSeq + 1), Second
  );
  TestEqual(TEXT("The entry is saved"), Component->GetPendingCurrencyEntries().Num(), 0);
  TestEqual(
    TEXT("A balance behind a later fence is not written"), PlayerState->RedwoodCharacter.CurrencyAmount, (int64)750
  );
  TestEqual(TEXT("The seq still moves"), PlayerState->RedwoodCharacter.InventorySeq, Second.BatchSeq + 1);
  TestTrue(TEXT("The snapshot says its balance is stale"), PlayerState->RedwoodCharacter.bCurrencyAmountUnknown);
  TestTrue(TEXT("The component reloads before it sends more currency"), Component->IsCurrencyRebaseRequired());

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyFlushErrorIsNotSavedTest,
  "Redwood.Currency.FlushErrorIsNotSaved",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// An error reply means NOT saved, whatever committedSeq says: nothing is dropped and the snapshot
// keeps its values.
bool FRedwoodCurrencyFlushErrorIsNotSavedTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyFlushTest;
  URedwoodCharacterComponent *Component = Fixture::MakeCharacterComponent(3);
  URedwoodPlayerStateComponent *PlayerState = Fixture::MakePlayerStateComponent(3, 1000);

  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdA, -250, 750)}, {Fixture::QuestGrantKey});
  const FRedwoodItemFlushSent Sent = Fixture::ClaimBatch(*this, Component);

  // A racing retry's refusal names the original's commit as its fence.
  URedwoodServerGameSubsystem::ApplyItemsFlushReply(
    Component, PlayerState, Fixture::MakeReply(TEXT("concurrent flush: mismatch"), Sent.BatchSeq), Sent
  );

  TestEqual(TEXT("The entry is kept"), Component->GetPendingCurrencyEntries().Num(), 1);
  TestEqual(TEXT("The key is kept"), Component->GetPendingGrantKeys().Num(), 1);
  TestEqual(TEXT("The snapshot balance is unchanged"), PlayerState->RedwoodCharacter.CurrencyAmount, (int64)1000);
  TestEqual(TEXT("The snapshot seq is unchanged"), PlayerState->RedwoodCharacter.InventorySeq, (int64)3);
  TestFalse(TEXT("No rebase without the backend's flag"), Component->IsCurrencyRebaseRequired());

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySequenceGapLowersSnapshotTest,
  "Redwood.Currency.SequenceGapLowersSnapshot",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// After a gap the snapshot follows the lane back, so the forward seed from it cannot put the lane
// on the gapped seq again.
bool FRedwoodCurrencySequenceGapLowersSnapshotTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyFlushTest;
  URedwoodCharacterComponent *Component = Fixture::MakeCharacterComponent(9);
  URedwoodPlayerStateComponent *PlayerState = Fixture::MakePlayerStateComponent(9, 1000);

  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdA, 100, 1100)}, {});
  const FRedwoodItemFlushSent Sent = Fixture::ClaimBatch(*this, Component);
  TestEqual(TEXT("Sent at seq 10"), Sent.BatchSeq, (int64)10);

  URedwoodServerGameSubsystem::ApplyItemsFlushReply(
    Component, PlayerState, Fixture::MakeReply(TEXT("sequence gap"), 6), Sent
  );

  TestEqual(TEXT("The snapshot seq is lowered to the backend fence"), PlayerState->RedwoodCharacter.InventorySeq, (int64)6);
  TestEqual(TEXT("The entry is kept"), Component->GetPendingCurrencyEntries().Num(), 1);

  // The flush path seeds the lane forward from the snapshot before each claim; it must not move it.
  if (PlayerState->RedwoodCharacter.InventorySeq > Component->GetLastCommittedItemSeq()) {
    Component->SeedItemSeqFromCharacter(PlayerState->RedwoodCharacter.InventorySeq);
  }
  const FRedwoodItemFlushSent Next = Fixture::ClaimBatch(*this, Component);
  TestEqual(TEXT("The next batch goes right after the fence"), Next.BatchSeq, (int64)7);
  TestEqual(TEXT("The kept entry goes with it"), Next.CurrencyCount, 1);

  // A reply with no fence is not a gap, and must not lower anything.
  URedwoodServerGameSubsystem::ApplyItemsFlushReply(
    Component, PlayerState, Fixture::MakeReply(TEXT("socket closed"), -1), Next
  );
  TestEqual(TEXT("No fence leaves the snapshot alone"), PlayerState->RedwoodCharacter.InventorySeq, (int64)6);

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyFlushAlertsBroadcastTest,
  "Redwood.Currency.FlushAlertsBroadcast",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodCurrencyFlushAlertsBroadcastTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyFlushTest;
  URedwoodCharacterComponent *Component = Fixture::MakeCharacterComponent(1);

  int32 BroadcastCount = 0;
  int32 AlertCount = 0;
  Component->OnCurrencyAlerts.AddLambda(
    [&BroadcastCount, &AlertCount](const TArray<TSharedPtr<FJsonObject>> &Alerts) {
      ++BroadcastCount;
      AlertCount = Alerts.Num();
    }
  );

  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdA, 500, 500)}, {});
  const FRedwoodItemFlushSent Quiet = Fixture::ClaimBatch(*this, Component);
  URedwoodServerGameSubsystem::ApplyItemsFlushReply(
    Component, nullptr, Fixture::MakeReply(TEXT(""), Quiet.BatchSeq), Quiet
  );
  TestEqual(TEXT("No broadcast for a reply with no alerts"), BroadcastCount, 0);

  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdB, 500, 1000)}, {});
  const FRedwoodItemFlushSent Loud = Fixture::ClaimBatch(*this, Component);
  FRedwoodItemFlushReply Reply = Fixture::MakeReply(TEXT(""), Loud.BatchSeq);
  Reply.Alerts.Add(Fixture::ParseJson(TEXT(R"({"kind":"income","detail":{}})")));
  URedwoodServerGameSubsystem::ApplyItemsFlushReply(Component, nullptr, Reply, Loud);
  TestEqual(TEXT("One broadcast"), BroadcastCount, 1);
  TestEqual(TEXT("It carries the alert"), AlertCount, 1);

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyReloadRebasesTest,
  "Redwood.Currency.ReloadRebases",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A trade used the batch's seq: the held entries are re-chained from the reloaded balance, the
// snapshot takes that balance, and the game hears it, so it can restart its running balance.
bool FRedwoodCurrencyReloadRebasesTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyFlushTest;
  URedwoodCharacterComponent *Component = Fixture::MakeCharacterComponent(4);
  URedwoodPlayerStateComponent *PlayerState = Fixture::MakePlayerStateComponent(4, 1000);

  int64 RebasedBalance = -1;
  Component->OnCurrencyRebased.AddLambda([&RebasedBalance](int64 Balance) {
    RebasedBalance = Balance;
  });

  Component->QueueCurrency(
    {Fixture::MakeEntry(Fixture::EntryIdA, 500, 1500), Fixture::MakeEntry(Fixture::EntryIdB, -200, 1300)},
    {}
  );
  const FRedwoodItemFlushSent Sent = Fixture::ClaimBatch(*this, Component);
  FRedwoodItemFlushReply Refusal = Fixture::MakeReply(TEXT("batch seq was used by a trade"), Sent.BatchSeq);
  Refusal.bRebaseRequired = true;
  URedwoodServerGameSubsystem::ApplyItemsFlushReply(Component, PlayerState, Refusal, Sent);
  TestTrue(TEXT("The refusal holds the entries for a reload"), Component->IsCurrencyRebaseRequired());

  int64 Balance = 0;
  TArray<FString> GrantKeys;
  const ERedwoodCurrencyReloadResult Result = URedwoodServerGameSubsystem::ParseCurrencyReloadReply(
    Fixture::ParseJson(TEXT(R"({"error":null,"items":[],"currency":{"amount":"2000","grants":["quest:Q1"]}})")),
    Balance,
    GrantKeys
  );
  TestTrue(TEXT("The reply parses"), Result == ERedwoodCurrencyReloadResult::Applied);
  TestEqual(TEXT("The balance is read exactly"), Balance, (int64)2000);
  TestTrue(TEXT("The used keys are read"), GrantKeys == TArray<FString>{Fixture::QuestGrantKey});
  // A key a racing batch committed after the reload read the database: on the snapshot, not in
  // the reply. Keys are never removed, so it must stay.
  const FString RacedGrantKey = TEXT("quest:Raced");
  PlayerState->RedwoodCharacter.GrantKeys = {RacedGrantKey};
  TestTrue(
    TEXT("The reload applies"),
    URedwoodServerGameSubsystem::ApplyCurrencyReload(Component, PlayerState, Balance, GrantKeys)
  );
  // A key that a batch committed while its ack was lost reaches the snapshot only through here.
  TestTrue(
    TEXT("The snapshot keeps its key and gains the reloaded one"),
    PlayerState->RedwoodCharacter.GrantKeys == TArray<FString>{RacedGrantKey, Fixture::QuestGrantKey}
  );

  TestFalse(TEXT("The hold ends"), Component->IsCurrencyRebaseRequired());
  const TArray<FRedwoodCurrencyEntry> &Pending = Component->GetPendingCurrencyEntries();
  TestEqual(TEXT("No entry is dropped"), Pending.Num(), 2);
  if (Pending.Num() == 2) {
    TestEqual(TEXT("A re-chained from the reloaded balance"), Pending[0].BalanceAfter, (int64)2500);
    TestEqual(TEXT("B re-chained after A"), Pending[1].BalanceAfter, (int64)2300);
  }
  TestEqual(TEXT("The snapshot takes the reloaded balance"), PlayerState->RedwoodCharacter.CurrencyAmount, (int64)2000);
  TestEqual(TEXT("The game hears the reloaded balance"), RebasedBalance, (int64)2000);
  TestEqual(TEXT("Both entries go in the next batch"), Component->GetCurrencyEntriesToSendCount(), 2);

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyReloadReplyParseTest,
  "Redwood.Currency.ReloadReplyParse",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A reload that lacks a readable balance is a failed load, retried. An error reply's filler "0"
// must never be used. Only the realm's writerFenceLost flag gives up at once: a "not bound" error
// without it is retriable, since the instance binding comes back after a sidecar restart.
bool FRedwoodCurrencyReloadReplyParseTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyFlushTest;
  AddExpectedError(TEXT("Currency reload failed"), EAutomationExpectedErrorFlags::Contains, 0);

  const TCHAR *RetriedReplies[] = {
    TEXT(R"({"error":"Item load request timed out.","items":[],"currency":{"amount":"0","grants":[]}})"),
    TEXT(R"({"error":"character not bound to this instance","items":[],"currency":{"amount":"0","grants":[]}})"),
    TEXT(R"({"error":"character not bound to this instance","writerFenceLost":false,"items":[],"currency":{"amount":"0","grants":[]}})"),
    TEXT(R"({"error":null,"items":[]})"),
    TEXT(R"({"error":null,"items":[],"currency":{"amount":"1e3","grants":[]}})"),
    TEXT(R"({"error":null,"items":[],"currency":{"amount":"-5","grants":[]}})"),
    TEXT(R"({"error":null,"items":[],"currency":{"amount":2000,"grants":[]}})"),
    TEXT(R"({"items":[],"currency":{"amount":"2000","grants":[]}})"),
    // The keys are part of the loaded state, so a reply without readable keys fails too.
    TEXT(R"({"error":null,"items":[],"currency":{"amount":"2000"}})"),
    TEXT(R"({"error":null,"items":[],"currency":{"amount":"2000","grants":[5]}})"),
  };
  for (const TCHAR *ReplyText : RetriedReplies) {
    int64 Balance = 0;
    TArray<FString> GrantKeys;
    TestTrue(
      FString::Printf(TEXT("Retried: %s"), ReplyText),
      URedwoodServerGameSubsystem::ParseCurrencyReloadReply(Fixture::ParseJson(ReplyText), Balance, GrantKeys) ==
        ERedwoodCurrencyReloadResult::Retry
    );
  }

  int64 Balance = 0;
  TArray<FString> GrantKeys;
  TestTrue(
    TEXT("An unreadable reply is retried"),
    URedwoodServerGameSubsystem::ParseCurrencyReloadReply(TSharedPtr<FJsonObject>(), Balance, GrantKeys) ==
      ERedwoodCurrencyReloadResult::Retry
  );
  TestTrue(
    TEXT("A lost writer fence gives up at once"),
    URedwoodServerGameSubsystem::ParseCurrencyReloadReply(
      Fixture::ParseJson(TEXT(R"({"error":"character not bound to this instance","writerFenceLost":true,"items":[],"currency":{"amount":"0","grants":[]}})")),
      Balance,
      GrantKeys
    ) == ERedwoodCurrencyReloadResult::GiveUp
  );

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyNotAppliedConsumesSeqTest,
  "Redwood.Currency.NotAppliedConsumesSeq",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A notApplied reply commits the probed seq empty. The lane and the snapshot move past it, and the
// entry stays as it was.
bool FRedwoodCurrencyNotAppliedConsumesSeqTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyFlushTest;
  URedwoodCharacterComponent *Component = Fixture::MakeCharacterComponent(4);
  URedwoodPlayerStateComponent *PlayerState = Fixture::MakePlayerStateComponent(4, 1000);

  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdA, -250, 750)}, {});
  const FRedwoodItemFlushSent First = Fixture::ClaimBatch(*this, Component);
  URedwoodServerGameSubsystem::ApplyItemsFlushReply(
    Component, PlayerState, Fixture::MakeReply(TEXT("socket closed"), -1), First
  );

  const FRedwoodItemFlushSent Probe = Fixture::ClaimBatch(*this, Component);
  TestTrue(TEXT("The re-send is a probe"), Component->IsItemFlushReplayProbe());
  FRedwoodItemFlushReply Reply = Fixture::MakeReply(TEXT("batch was never applied"), Probe.BatchSeq);
  Reply.bNotApplied = true;
  URedwoodServerGameSubsystem::ApplyItemsFlushReply(Component, PlayerState, Reply, Probe);

  TestEqual(TEXT("The snapshot seq moves to the consumed seq"), PlayerState->RedwoodCharacter.InventorySeq, Probe.BatchSeq);
  const TArray<FRedwoodCurrencyEntry> &Pending = Component->GetPendingCurrencyEntries();
  if (TestEqual(TEXT("The entry is kept"), Pending.Num(), 1)) {
    TestEqual(TEXT("It is unchanged"), Pending[0].BalanceAfter, (int64)750);
  }
  const FRedwoodItemFlushSent Next = Fixture::ClaimBatch(*this, Component);
  TestFalse(TEXT("The next batch is not a probe"), Component->IsItemFlushReplayProbe());
  TestEqual(TEXT("It goes out after the consumed seq"), Next.BatchSeq, Probe.BatchSeq + 1);
  TestEqual(TEXT("It carries the entry"), Next.CurrencyCount, 1);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyFlushCommitAddsGrantKeysTest,
  "Redwood.Currency.FlushCommitAddsGrantKeys",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A grant key joins the snapshot's used keys, and a balance the snapshot's balance, only when the
// batch commits, so game code on a recreated component neither pays a reward twice nor skips one
// that never saved.
bool FRedwoodCurrencyFlushCommitAddsGrantKeysTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyFlushTest;
  URedwoodCharacterComponent *Component = Fixture::MakeCharacterComponent(4);
  URedwoodPlayerStateComponent *PlayerState = Fixture::MakePlayerStateComponent(4, 1000);

  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdA, 250, 1250)}, {Fixture::QuestGrantKey});
  const FRedwoodItemFlushSent First = Fixture::ClaimBatch(*this, Component);
  URedwoodServerGameSubsystem::ApplyItemsFlushReply(
    Component, PlayerState, Fixture::MakeReply(TEXT("socket closed"), -1), First
  );
  TestEqual(TEXT("An unknown outcome adds no key"), PlayerState->RedwoodCharacter.GrantKeys.Num(), 0);

  const FRedwoodItemFlushSent Probe = Fixture::ClaimBatch(*this, Component);
  FRedwoodItemFlushReply NotApplied = Fixture::MakeReply(FString(), Probe.BatchSeq);
  // Without the error text the backend sends with it, so only the flag can keep the key out.
  NotApplied.bNotApplied = true;
  URedwoodServerGameSubsystem::ApplyItemsFlushReply(Component, PlayerState, NotApplied, Probe);
  TestEqual(TEXT("A batch that never applied adds no key"), PlayerState->RedwoodCharacter.GrantKeys.Num(), 0);
  TestEqual(TEXT("Nor moves the snapshot balance"), PlayerState->RedwoodCharacter.CurrencyAmount, (int64)1000);

  const FRedwoodItemFlushSent Normal = Fixture::ClaimBatch(*this, Component);
  // A later fence: a trade committed after this batch, which still committed.
  URedwoodServerGameSubsystem::ApplyItemsFlushReply(
    Component, PlayerState, Fixture::MakeReply(FString(), Normal.BatchSeq + 1), Normal
  );
  TestTrue(
    TEXT("The committed key joins the snapshot"),
    PlayerState->RedwoodCharacter.GrantKeys == TArray<FString>{Fixture::QuestGrantKey}
  );
  TestEqual(TEXT("And leaves the queue"), Component->GetPendingGrantKeys().Num(), 0);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyFlushDeliveryFlagsTest,
  "Redwood.Currency.FlushDeliveryFlags",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// What the sidecar's two flags do to the lane. notForwarded: the batch never left the sidecar, so
// it goes out again as a normal batch under the same seq, rows and all. timedOut: it may have
// applied, so the next attempt is a probe. Neither reply's filler fence of 0 reads as a gap.
bool FRedwoodCurrencyFlushDeliveryFlagsTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyFlushTest;
  constexpr int64 SeqBefore = 6;
  URedwoodCharacterComponent *Component = Fixture::MakeCharacterComponent(SeqBefore);
  URedwoodPlayerStateComponent *PlayerState = Fixture::MakePlayerStateComponent(SeqBefore, 1000);
  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdA, -250, 750)}, {});

  FRedwoodItemFlushSent Sent = Fixture::ClaimBatch(*this, Component);
  URedwoodServerGameSubsystem::ApplyItemsFlushReply(
    Component,
    PlayerState,
    URedwoodServerGameSubsystem::ParseItemsFlushReply(Fixture::ParseJson(
      TEXT(R"({"error":"Realm server not connected","committedSeq":0,"notForwarded":true})")
    )),
    Sent
  );
  TestEqual(TEXT("The unsent entry is kept"), Component->GetPendingCurrencyEntries().Num(), 1);
  TestFalse(TEXT("An unsent batch is not unconfirmed"), Component->IsCurrencyBatchUnconfirmed());
  TestEqual(TEXT("The lane is not realigned to the filler 0"), Component->GetLastCommittedItemSeq(), SeqBefore);
  TestEqual(TEXT("The snapshot is not lowered to it"), PlayerState->RedwoodCharacter.InventorySeq, SeqBefore);
  Sent = Fixture::ClaimBatch(*this, Component);
  TestFalse(TEXT("The next attempt is a normal batch"), Component->IsItemFlushReplayProbe());
  TestEqual(TEXT("Under the same seq"), Sent.BatchSeq, SeqBefore + 1);

  URedwoodServerGameSubsystem::ApplyItemsFlushReply(
    Component,
    PlayerState,
    URedwoodServerGameSubsystem::ParseItemsFlushReply(Fixture::ParseJson(
      TEXT(R"({"error":"Item flush request timed out.","committedSeq":0,"timedOut":true})")
    )),
    Sent
  );
  TestTrue(TEXT("A timed-out batch is unconfirmed"), Component->IsCurrencyBatchUnconfirmed());
  TestEqual(TEXT("Its filler 0 is not a gap either"), Component->GetLastCommittedItemSeq(), SeqBefore);

  // An unsent re-send of the unconfirmed batch leaves it unconfirmed: the timed-out attempt may
  // still have applied.
  Sent = Fixture::ClaimBatch(*this, Component);
  TestTrue(TEXT("The re-send is a probe"), Component->IsItemFlushReplayProbe());
  URedwoodServerGameSubsystem::ApplyItemsFlushReply(
    Component,
    PlayerState,
    URedwoodServerGameSubsystem::ParseItemsFlushReply(Fixture::ParseJson(
      TEXT(R"({"error":"Realm server not connected","committedSeq":0,"notForwarded":true})")
    )),
    Sent
  );
  TestTrue(TEXT("It stays unconfirmed"), Component->IsCurrencyBatchUnconfirmed());
  Sent = Fixture::ClaimBatch(*this, Component);
  TestTrue(TEXT("The next attempt is a probe again"), Component->IsItemFlushReplayProbe());
  TestEqual(TEXT("At the timed-out seq"), Sent.BatchSeq, SeqBefore + 1);
  return true;
}
