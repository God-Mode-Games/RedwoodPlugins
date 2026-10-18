// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart. Pins how
// URedwoodCharacterComponent stages currency ledger entries and grant keys beside the item flush:
// which acks may drop an entry, which must keep it, and how an entry whose outcome is unknown is
// re-sent. An amount that is lost or applied twice here is lost or duplicated money.
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "RedwoodCharacterComponent.h"
#include "RedwoodCurrencyTypes.h"

// Named, not anonymous, for the same unity-build reason as ItemPersistenceTest.cpp.
namespace RedwoodCurrencyPersistenceTest {
  const FString EntryIdA = TEXT("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
  const FString EntryIdB = TEXT("BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB");
  const FString LootReason = TEXT("Currency.Reason.Loot");
  const FString QuestGrantKey = TEXT("quest:Q1");
  const FString ItemId = TEXT("11111111-1111-1111-1111-111111111111");

  FRedwoodCurrencyEntry MakeEntry(const FString &Id, int64 Delta, int64 BalanceAfter) {
    FRedwoodCurrencyEntry Entry;
    Entry.Id = Id;
    Entry.Delta = Delta;
    Entry.BalanceAfter = BalanceAfter;
    Entry.Reason = LootReason;
    return Entry;
  }

  URedwoodCharacterComponent *MakeTestComponent() {
    URedwoodCharacterComponent *Component = NewObject<URedwoodCharacterComponent>();
    Component->bUseItems = true;
    return Component;
  }

  const TArray<TPair<FString, uint64>> NoItems;

  // The row half of the flush payload, as the flush path must build it: every dirty row, except in
  // a re-send of an unconfirmed currency batch, which carries none.
  TArray<TPair<FString, uint64>> CollectSentRows(const URedwoodCharacterComponent *Component) {
    TArray<TPair<FString, uint64>> SentRows;
    if (Component->IsItemFlushReplayProbe()) {
      return SentRows;
    }
    for (const TPair<FString, uint64> &Dirty : Component->GetDirtyItemGenerations()) {
      SentRows.Add(Dirty);
    }
    return SentRows;
  }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyQueueMakesDirtyTest,
  "Redwood.Currency.QueueMakesDirty",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodCurrencyQueueMakesDirtyTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyPersistenceTest;

  // A balance change with no item change must still drive a flush.
  URedwoodCharacterComponent *EntryOnly = Fixture::MakeTestComponent();
  TestFalse(TEXT("Clean before queueing"), EntryOnly->IsItemsDirty());
  EntryOnly->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdA, 500, 500)}, {});
  TestTrue(TEXT("A queued entry makes the component dirty"), EntryOnly->IsItemsDirty());
  TestEqual(TEXT("All entries go in the next batch"), EntryOnly->GetCurrencyEntriesToSendCount(), 1);

  // So must a grant key alone.
  URedwoodCharacterComponent *GrantOnly = Fixture::MakeTestComponent();
  GrantOnly->QueueCurrency({}, {Fixture::QuestGrantKey});
  TestTrue(TEXT("A queued grant key makes the component dirty"), GrantOnly->IsItemsDirty());
  TestEqual(TEXT("The key goes in the next batch"), GrantOnly->GetGrantKeysToSendCount(), 1);

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyInFlightEntryKeptTest,
  "Redwood.Currency.InFlightEntryKept",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodCurrencyInFlightEntryKeptTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyPersistenceTest;
  URedwoodCharacterComponent *Component = Fixture::MakeTestComponent();

  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdA, 500, 500)}, {});
  int64 BatchSeq = 0;
  TestTrue(TEXT("Claim succeeds"), Component->TryBeginItemFlush(BatchSeq));
  const int32 SentCount = Component->GetCurrencyEntriesToSendCount();

  // B is staged while A's batch is in flight; A's ack must not take B with it.
  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdB, 200, 700)}, {});
  Component->CompleteItemFlush(TEXT(""), BatchSeq, Fixture::NoItems, Fixture::NoItems, SentCount, 0);

  const TArray<FRedwoodCurrencyEntry> &Pending = Component->GetPendingCurrencyEntries();
  TestEqual(TEXT("One entry remains"), Pending.Num(), 1);
  if (Pending.Num() == 1) {
    TestEqual(TEXT("The remaining entry is the one staged mid-flight"), Pending[0].Id, Fixture::EntryIdB);
  }
  TestTrue(TEXT("Still dirty, so B goes out next"), Component->IsItemsDirty());

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyReplayClearsSentTest,
  "Redwood.Currency.ReplayClearsSent",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodCurrencyReplayClearsSentTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyPersistenceTest;
  URedwoodCharacterComponent *Component = Fixture::MakeTestComponent();
  Component->SeedItemSeqFromCharacter(1);

  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdA, 500, 500)}, {Fixture::QuestGrantKey});
  int64 FirstSeq = 0;
  TestTrue(TEXT("Claim succeeds"), Component->TryBeginItemFlush(FirstSeq));
  TestEqual(TEXT("First attempt uses seq 2"), FirstSeq, (int64)2);

  // The backend committed seq 2, but the ack was lost: the game server sees only a dropped socket.
  Component->CompleteItemFlush(TEXT("socket closed"), -1, Fixture::NoItems, Fixture::NoItems, 1, 1);

  // A newer entry staged before the retry must not join it.
  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdB, 200, 700)}, {});
  TestEqual(TEXT("The retry carries only the unconfirmed entry"), Component->GetCurrencyEntriesToSendCount(), 1);
  TestEqual(TEXT("The retry carries the unconfirmed key"), Component->GetGrantKeysToSendCount(), 1);

  int64 RetrySeq = 0;
  TestTrue(TEXT("Retry claim succeeds"), Component->TryBeginItemFlush(RetrySeq));
  TestEqual(TEXT("The retry re-uses seq 2, so the backend sees a replay"), RetrySeq, (int64)2);

  // The backend finds every entry id in its ledger and acks the replay with its fence (3: a trade
  // has also committed since).
  Component->CompleteItemFlush(TEXT(""), 3, Fixture::NoItems, Fixture::NoItems, 1, 1);

  const TArray<FRedwoodCurrencyEntry> &Pending = Component->GetPendingCurrencyEntries();
  TestEqual(TEXT("The replayed entry is dropped, not sent again"), Pending.Num(), 1);
  if (Pending.Num() == 1) {
    TestEqual(TEXT("Only the newer entry remains"), Pending[0].Id, Fixture::EntryIdB);
  }
  TestEqual(TEXT("The replayed key is dropped"), Component->GetPendingGrantKeys().Num(), 0);

  int64 NextSeq = 0;
  TestTrue(TEXT("Next claim succeeds"), Component->TryBeginItemFlush(NextSeq));
  TestEqual(TEXT("The lane continues after the backend fence"), NextSeq, (int64)4);
  TestEqual(TEXT("The newer entry is now free to go"), Component->GetCurrencyEntriesToSendCount(), 1);

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyErrorKeepsEntriesTest,
  "Redwood.Currency.ErrorKeepsEntries",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodCurrencyErrorKeepsEntriesTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyPersistenceTest;
  URedwoodCharacterComponent *Component = Fixture::MakeTestComponent();
  Component->SeedItemSeqFromCharacter(4);

  Component->QueueCurrency(
    {Fixture::MakeEntry(Fixture::EntryIdA, 500, 1500), Fixture::MakeEntry(Fixture::EntryIdB, -200, 1300)},
    {}
  );
  int64 BatchSeq = 0;
  TestTrue(TEXT("Claim succeeds"), Component->TryBeginItemFlush(BatchSeq));
  TestEqual(TEXT("Claim uses seq 5"), BatchSeq, (int64)5);

  // A trade used seq 5. The backend reports a fence AT the sent seq together with an error and
  // rebaseRequired: the entries did not apply. Dropping them on committedSeq alone would lose the
  // money.
  Component->CompleteItemFlush(
    TEXT("batch seq was used by a trade"), 5, Fixture::NoItems, Fixture::NoItems, 2, 0, false, true
  );

  TestEqual(TEXT("Both entries are kept"), Component->GetPendingCurrencyEntries().Num(), 2);
  TestTrue(TEXT("A rebase is required"), Component->IsCurrencyRebaseRequired());
  TestEqual(TEXT("No currency is sent before the rebase"), Component->GetCurrencyEntriesToSendCount(), 0);

  // The trade took the backend balance to 2000. Every BalanceAfter is re-chained from it.
  Component->RebaseCurrency(2000);
  const TArray<FRedwoodCurrencyEntry> &Pending = Component->GetPendingCurrencyEntries();
  if (Pending.Num() == 2) {
    TestEqual(TEXT("A re-chained from the reloaded balance"), Pending[0].BalanceAfter, (int64)2500);
    TestEqual(TEXT("B re-chained after A"), Pending[1].BalanceAfter, (int64)2300);
  }
  TestFalse(TEXT("The rebase is done"), Component->IsCurrencyRebaseRequired());
  TestEqual(TEXT("Both entries go in the next batch"), Component->GetCurrencyEntriesToSendCount(), 2);

  int64 NextSeq = 0;
  TestTrue(TEXT("Next claim succeeds"), Component->TryBeginItemFlush(NextSeq));
  TestEqual(TEXT("The next batch goes after the trade's seq"), NextSeq, (int64)6);

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyUnknownErrorKeepsBatchTest,
  "Redwood.Currency.UnknownErrorKeepsBatch",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodCurrencyUnknownErrorKeepsBatchTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyPersistenceTest;
  URedwoodCharacterComponent *Component = Fixture::MakeTestComponent();

  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdA, 500, 500)}, {});
  int64 BatchSeq = 0;
  TestTrue(TEXT("Claim succeeds"), Component->TryBeginItemFlush(BatchSeq));
  Component->CompleteItemFlush(TEXT("flush failed"), -1, Fixture::NoItems, Fixture::NoItems, 1, 0);

  const TArray<FRedwoodCurrencyEntry> &Pending = Component->GetPendingCurrencyEntries();
  TestEqual(TEXT("The entry is kept"), Pending.Num(), 1);
  if (Pending.Num() == 1) {
    // The entry may already be committed, so its value must stay exactly as sent: a re-chain from
    // a reload would count it twice.
    TestEqual(TEXT("BalanceAfter is unchanged"), Pending[0].BalanceAfter, (int64)500);
  }
  TestFalse(TEXT("No rebase for an unknown outcome"), Component->IsCurrencyRebaseRequired());

  // A reload arriving now must not re-chain an entry that may already be committed. The refusal
  // logs a Warning, which AddExpectedError matches: it registers the pattern at Warning verbosity,
  // inclusive of errors (UE 5.8 Runtime/Core/Private/Misc/AutomationTest.cpp:1973-1974,
  // IsExpectedMessage at 2700-2708). Occurrences 0 means "at least once", so the refusal must
  // happen for the test to pass.
  AddExpectedError(TEXT("no rebase is required"), EAutomationExpectedErrorFlags::Contains, 0);
  Component->RebaseCurrency(500);
  if (Pending.Num() == 1) {
    TestEqual(TEXT("A stray rebase leaves BalanceAfter alone"), Pending[0].BalanceAfter, (int64)500);
  }

  // A trade fence seeds the lane forward before the retry. The retry must still go out under the
  // seq the unconfirmed batch used, as a replay probe that carries no item rows.
  Component->SeedItemSeqFromCharacter(3);
  int64 ProbeSeq = 0;
  TestTrue(TEXT("Probe claim succeeds"), Component->TryBeginItemFlush(ProbeSeq));
  TestEqual(TEXT("The probe re-uses the unconfirmed seq"), ProbeSeq, BatchSeq);
  TestTrue(TEXT("The claim is a replay probe"), Component->IsItemFlushReplayProbe());

  // The batch never committed: the backend refuses the probe because the trade used its seq.
  Component->CompleteItemFlush(
    TEXT("batch seq was used by a trade"), 3, Fixture::NoItems, Fixture::NoItems, 1, 0, false, true
  );
  TestEqual(TEXT("Still kept after the probe's refusal"), Component->GetPendingCurrencyEntries().Num(), 1);
  TestTrue(TEXT("Now a rebase is required"), Component->IsCurrencyRebaseRequired());

  int64 NextSeq = 0;
  TestTrue(TEXT("Next claim succeeds"), Component->TryBeginItemFlush(NextSeq));
  TestEqual(TEXT("The lane continues after the fence, not from the probe"), NextSeq, (int64)4);
  TestFalse(TEXT("No longer a probe"), Component->IsItemFlushReplayProbe());

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyFinalRefusalFreezesTest,
  "Redwood.Currency.FinalRefusalFreezes",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A final refusal is a desync: everything staged is dropped and logged, nothing is repaired, the
// component refuses new currency, and the game hears it once, with the character.
bool FRedwoodCurrencyFinalRefusalFreezesTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyPersistenceTest;
  // One line per dropped entry and key, then one per refused entry while frozen.
  AddExpectedError(TEXT("dropped currency entry"), EAutomationExpectedErrorFlags::Contains, 2);
  AddExpectedError(TEXT("dropped grant"), EAutomationExpectedErrorFlags::Contains, 1);
  AddExpectedError(TEXT("is frozen after a final refusal"), EAutomationExpectedErrorFlags::Contains, 1);
  URedwoodCharacterComponent *Component = Fixture::MakeTestComponent();
  Component->RedwoodCharacterId = TEXT("character-1");

  FString ReceivedCharacterId;
  FString ReceivedError;
  int32 BroadcastCount = 0;
  Component->OnItemFlushRefusedFinal.AddLambda(
    [&ReceivedCharacterId, &ReceivedError, &BroadcastCount](const FString &CharacterId, const FString &Error) {
      ReceivedCharacterId = CharacterId;
      ReceivedError = Error;
      ++BroadcastCount;
    }
  );

  // A purchase: the item row and its debit in one batch.
  Component->MarkItemsDirty({Fixture::ItemId});
  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdA, -300, 200)}, {Fixture::QuestGrantKey});
  int64 BatchSeq = 0;
  TestTrue(TEXT("Claim succeeds"), Component->TryBeginItemFlush(BatchSeq));
  TArray<TPair<FString, uint64>> SentUpserts;
  SentUpserts.Add(TPair<FString, uint64>(Fixture::ItemId, Component->GetDirtyItemGenerations().FindRef(Fixture::ItemId)));

  // Staged after the claim, so it may depend on the purchase (a sale of the bought item).
  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdB, 100, 300)}, {});

  // A plain error never drops anything; only an explicit final refusal does.
  Component->CompleteItemFlush(TEXT("overdraft"), 0, SentUpserts, Fixture::NoItems, 1, 1, false);
  TestEqual(TEXT("A non-final error keeps both entries"), Component->GetPendingCurrencyEntries().Num(), 2);
  TestEqual(TEXT("No final broadcast for a plain error"), BroadcastCount, 0);
  TestFalse(TEXT("No freeze for a plain error"), Component->IsFrozenAfterFinalRefusal());

  TestTrue(TEXT("Retry claim succeeds"), Component->TryBeginItemFlush(BatchSeq));
  Component->CompleteItemFlush(TEXT("overdraft"), 0, SentUpserts, Fixture::NoItems, 1, 1, true);

  TestTrue(TEXT("The component is frozen"), Component->IsFrozenAfterFinalRefusal());
  TestEqual(TEXT("Every entry is dropped, sent or not"), Component->GetPendingCurrencyEntries().Num(), 0);
  TestEqual(TEXT("The key is dropped"), Component->GetPendingGrantKeys().Num(), 0);
  TestFalse(TEXT("No item row stays dirty"), Component->IsItemsDirty());
  TestFalse(TEXT("No reload is asked for: nothing is repaired"), Component->IsCurrencyRebaseRequired());
  TestEqual(TEXT("The refusal is broadcast once"), BroadcastCount, 1);
  TestEqual(TEXT("The broadcast names the character"), ReceivedCharacterId, FString(TEXT("character-1")));
  TestEqual(TEXT("The broadcast carries the error"), ReceivedError, FString(TEXT("overdraft")));

  TestFalse(
    TEXT("New currency is refused while frozen"),
    Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdA, 50, 50)}, {})
  );
  TestEqual(TEXT("Nothing is queued while frozen"), Component->GetPendingCurrencyEntries().Num(), 0);

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyConcurrentFlushIsUnconfirmedTest,
  "Redwood.Currency.ConcurrentFlushIsUnconfirmed",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodCurrencyConcurrentFlushIsUnconfirmedTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyPersistenceTest;
  URedwoodCharacterComponent *Component = Fixture::MakeTestComponent();
  Component->SeedItemSeqFromCharacter(4);

  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdA, 500, 1500)}, {});
  int64 FirstSeq = 0;
  TestTrue(TEXT("Claim succeeds"), Component->TryBeginItemFlush(FirstSeq));
  TestEqual(TEXT("Batch N is seq 5"), FirstSeq, (int64)5);

  // Batch N times out while the backend is still applying it.
  Component->CompleteItemFlush(TEXT("socket closed"), -1, Fixture::NoItems, Fixture::NoItems, 1, 0);
  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdB, 200, 1700)}, {});

  // The retry races the original; the original commits seq 5 first, and the retry is refused as a
  // concurrent flush that reports the original's fence. That is NOT a "did not apply" reply.
  int64 RetrySeq = 0;
  TestTrue(TEXT("Retry claim succeeds"), Component->TryBeginItemFlush(RetrySeq));
  TestEqual(TEXT("The retry re-uses seq 5"), RetrySeq, (int64)5);
  Component->CompleteItemFlush(
    TEXT("concurrent flush: inventorySeq or writer changed under fence"),
    5,
    Fixture::NoItems,
    Fixture::NoItems,
    1,
    0
  );
  TestFalse(TEXT("No rebase: the outcome is unknown"), Component->IsCurrencyRebaseRequired());
  const TArray<FRedwoodCurrencyEntry> &Pending = Component->GetPendingCurrencyEntries();
  TestEqual(TEXT("Both entries are kept"), Pending.Num(), 2);
  if (Pending.Num() == 2) {
    TestEqual(TEXT("A is not re-chained"), Pending[0].BalanceAfter, (int64)1500);
  }

  // The probe re-sends A alone under seq 5; the backend finds A in its ledger and acks it.
  int64 ProbeSeq = 0;
  TestTrue(TEXT("Probe claim succeeds"), Component->TryBeginItemFlush(ProbeSeq));
  TestEqual(TEXT("The probe re-uses seq 5"), ProbeSeq, (int64)5);
  TestEqual(TEXT("The probe carries only A"), Component->GetCurrencyEntriesToSendCount(), 1);
  Component->CompleteItemFlush(TEXT(""), 5, Fixture::NoItems, Fixture::NoItems, 1, 0);

  TestEqual(TEXT("A is dropped once, by the replay ack"), Pending.Num(), 1);
  if (Pending.Num() == 1) {
    TestEqual(TEXT("B survives"), Pending[0].Id, Fixture::EntryIdB);
    TestEqual(TEXT("B keeps the chain the backend now holds"), Pending[0].BalanceAfter, (int64)1700);
  }
  int64 NextSeq = 0;
  TestTrue(TEXT("Next claim succeeds"), Component->TryBeginItemFlush(NextSeq));
  TestEqual(TEXT("B goes out under seq 6"), NextSeq, (int64)6);
  TestEqual(TEXT("B is sent"), Component->GetCurrencyEntriesToSendCount(), 1);

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyResendCarriesNoRowsTest,
  "Redwood.Currency.ResendCarriesNoRows",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodCurrencyResendCarriesNoRowsTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyPersistenceTest;
  const FString SecondItemId = TEXT("22222222-2222-2222-2222-222222222222");
  URedwoodCharacterComponent *Component = Fixture::MakeTestComponent();

  // Purchase 1: its item row and its debit in one batch, which times out.
  Component->MarkItemsDirty({Fixture::ItemId});
  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdA, -300, 200)}, {});
  int64 BatchSeq = 0;
  TestTrue(TEXT("Claim succeeds"), Component->TryBeginItemFlush(BatchSeq));
  TestFalse(TEXT("A first send is not a probe"), Component->IsItemFlushReplayProbe());
  TArray<TPair<FString, uint64>> SentUpserts;
  SentUpserts.Add(TPair<FString, uint64>(Fixture::ItemId, 1));
  Component->CompleteItemFlush(TEXT("socket closed"), -1, SentUpserts, Fixture::NoItems, 1, 0);

  // Purchase 2's item row goes dirty before the retry.
  Component->MarkItemsDirty({SecondItemId});

  // The same-seq retry re-sends the unconfirmed debit: it is a probe even though the lane did not
  // move, so the flush sends no item rows in it.
  int64 RetrySeq = 0;
  TestTrue(TEXT("Retry claim succeeds"), Component->TryBeginItemFlush(RetrySeq));
  TestEqual(TEXT("Same seq"), RetrySeq, BatchSeq);
  TestTrue(TEXT("A same-seq re-send is a probe"), Component->IsItemFlushReplayProbe());

  // Purchase 1 had committed; the backend acks the replay. The reply clears whatever rows the
  // re-send carried, so the checks below fail if the probe flag lets rows into it.
  const TArray<TPair<FString, uint64>> ResentRows = Fixture::CollectSentRows(Component);
  TestEqual(TEXT("The re-send carries no rows"), ResentRows.Num(), 0);
  Component->CompleteItemFlush(TEXT(""), BatchSeq, ResentRows, Fixture::NoItems, 1, 0);

  TestEqual(TEXT("The debit is acknowledged"), Component->GetPendingCurrencyEntries().Num(), 0);
  TestTrue(
    TEXT("Purchase 2's row stays dirty after the replay ack"),
    Component->GetDirtyItemGenerations().Contains(SecondItemId)
  );
  TestTrue(
    TEXT("Purchase 1's row stays dirty too, so it is re-sent rather than assumed"),
    Component->GetDirtyItemGenerations().Contains(Fixture::ItemId)
  );

  int64 NextSeq = 0;
  TestTrue(TEXT("Next claim succeeds"), Component->TryBeginItemFlush(NextSeq));
  TestFalse(TEXT("The next batch is an ordinary one that carries rows"), Component->IsItemFlushReplayProbe());

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySequenceGapRealignsTest,
  "Redwood.Currency.SequenceGapRealigns",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodCurrencySequenceGapRealignsTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyPersistenceTest;
  URedwoodCharacterComponent *Component = Fixture::MakeTestComponent();
  Component->SeedItemSeqFromCharacter(9);

  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdA, 500, 500)}, {});
  int64 BatchSeq = 0;
  TestTrue(TEXT("Claim succeeds"), Component->TryBeginItemFlush(BatchSeq));
  TestEqual(TEXT("Claim uses seq 10"), BatchSeq, (int64)10);

  // The backend has committed only seq 7: seq 10 is a gap and did not apply.
  Component->CompleteItemFlush(TEXT("sequence gap"), 7, Fixture::NoItems, Fixture::NoItems, 1, 0);

  TestEqual(TEXT("The entry is kept"), Component->GetPendingCurrencyEntries().Num(), 1);
  TestFalse(TEXT("No rebase: the balance did not move"), Component->IsCurrencyRebaseRequired());
  TestEqual(TEXT("The committed seq follows the backend"), Component->GetLastCommittedItemSeq(), (int64)7);

  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdB, 200, 700)}, {});
  int64 NextSeq = 0;
  TestTrue(TEXT("Next claim succeeds"), Component->TryBeginItemFlush(NextSeq));
  TestEqual(TEXT("The next batch continues from the backend fence, not the gapped seq"), NextSeq, (int64)8);
  TestFalse(TEXT("Not a probe: the gapped batch is known not applied"), Component->IsItemFlushReplayProbe());
  TestEqual(TEXT("Both entries go out"), Component->GetCurrencyEntriesToSendCount(), 2);

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyEntryIdShapeTest,
  "Redwood.Currency.EntryIdShape",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodCurrencyEntryIdShapeTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyPersistenceTest;
  // Each refusal logs an error on purpose: three bad ids.
  AddExpectedError(TEXT("not 32 uppercase hex digits"), EAutomationExpectedErrorFlags::Contains, 3);

  const FString MadeId = MakeCurrencyEntryId();
  TestTrue(TEXT("A made id has the shape the backend checks"), IsValidCurrencyEntryId(MadeId));
  TestNotEqual(TEXT("Two made ids differ"), MadeId, MakeCurrencyEntryId());

  URedwoodCharacterComponent *Component = Fixture::MakeTestComponent();
  const bool bAllQueued = Component->QueueCurrency(
    {
      Fixture::MakeEntry(TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), 1, 1),
      Fixture::MakeEntry(TEXT("AAAA"), 1, 1),
      Fixture::MakeEntry(TEXT("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"), 1, 1),
      Fixture::MakeEntry(MadeId, 1, 1),
    },
    {}
  );

  TestFalse(TEXT("The caller hears that entries were refused"), bAllQueued);
  const TArray<FRedwoodCurrencyEntry> &Pending = Component->GetPendingCurrencyEntries();
  TestEqual(TEXT("Only the valid entry is queued"), Pending.Num(), 1);
  if (Pending.Num() == 1) {
    TestEqual(TEXT("The queued entry is the made one"), Pending[0].Id, MadeId);
  }

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyFreezeMarksUnknownOutcomeTest,
  "Redwood.Currency.FreezeMarksUnknownOutcome",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A probe is refused for good. Its entry's first attempt may have committed (its ack was lost), so
// the freeze log must not tell staff it was not saved: a re-grant would pay it twice.
bool FRedwoodCurrencyFreezeMarksUnknownOutcomeTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyPersistenceTest;
  AddExpectedErrorPlain(
    TEXT("dropped currency entry AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA delta 500 reason Currency.Reason.Loot, outcome UNKNOWN (it may be saved). A re-grant must reuse entry id AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  AddExpectedErrorPlain(
    TEXT("dropped currency entry BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB delta 200 reason Currency.Reason.Loot, NOT saved"),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  URedwoodCharacterComponent *Component = Fixture::MakeTestComponent();
  Component->SeedItemSeqFromCharacter(1);

  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdA, 500, 500)}, {});
  int64 BatchSeq = 0;
  TestTrue(TEXT("Claim succeeds"), Component->TryBeginItemFlush(BatchSeq));
  Component->CompleteItemFlush(TEXT("socket closed"), -1, Fixture::NoItems, Fixture::NoItems, 1, 0);
  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdB, 200, 700)}, {});

  TestTrue(TEXT("Probe claim succeeds"), Component->TryBeginItemFlush(BatchSeq));
  TestTrue(TEXT("The claim is a probe"), Component->IsItemFlushReplayProbe());
  Component->CompleteItemFlush(TEXT("refused"), 1, Fixture::NoItems, Fixture::NoItems, 1, 0, true);

  TestTrue(TEXT("The component is frozen"), Component->IsFrozenAfterFinalRefusal());
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyNotAppliedNeedsConsumedSeqTest,
  "Redwood.Currency.NotAppliedNeedsConsumedSeq",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A notApplied reply counts only with the consumed seq (committedSeq at or past the probed seq).
// Without it, the original attempt could still commit under that seq, so the batch stays
// unconfirmed, the next attempt is another probe, and the lane never moves below the probed seq.
bool FRedwoodCurrencyNotAppliedNeedsConsumedSeqTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyPersistenceTest;
  AddExpectedError(TEXT("probing again"), EAutomationExpectedErrorFlags::Contains, 2);
  URedwoodCharacterComponent *Component = Fixture::MakeTestComponent();
  Component->SeedItemSeqFromCharacter(4);
  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdA, 500, 500)}, {});

  int64 BatchSeq = 0;
  TestTrue(TEXT("Claim succeeds"), Component->TryBeginItemFlush(BatchSeq));
  TestEqual(TEXT("The first attempt uses seq 5"), BatchSeq, (int64)5);
  Component->CompleteItemFlush(TEXT("socket closed"), -1, Fixture::NoItems, Fixture::NoItems, 1, 0);

  // The old shape (committedSeq is the seq before the probe), then no fence at all.
  const int64 OldShapeFences[] = {4, -1};
  for (const int64 Fence : OldShapeFences) {
    TestTrue(TEXT("Probe claim succeeds"), Component->TryBeginItemFlush(BatchSeq));
    TestTrue(TEXT("The claim is a probe"), Component->IsItemFlushReplayProbe());
    TestEqual(TEXT("The probe uses the unconfirmed seq"), BatchSeq, (int64)5);
    Component->CompleteItemFlush(
      TEXT("batch was never applied"), Fence, Fixture::NoItems, Fixture::NoItems, 1, 0, false, false, true
    );
    TestEqual(TEXT("The entry is kept"), Component->GetPendingCurrencyEntries().Num(), 1);
    TestEqual(TEXT("No seq below the probed one is adopted"), Component->GetLastCommittedItemSeq(), (int64)4);
  }

  TestTrue(TEXT("Next claim succeeds"), Component->TryBeginItemFlush(BatchSeq));
  TestTrue(TEXT("The next attempt is another probe"), Component->IsItemFlushReplayProbe());
  TestEqual(TEXT("At the same seq"), BatchSeq, (int64)5);

  // The R31 shape: the probed seq is consumed, and the next batch goes after it.
  Component->CompleteItemFlush(
    TEXT("batch was never applied"), 5, Fixture::NoItems, Fixture::NoItems, 1, 0, false, false, true
  );
  TestTrue(TEXT("Claim after the consumed seq succeeds"), Component->TryBeginItemFlush(BatchSeq));
  TestFalse(TEXT("It is a normal batch"), Component->IsItemFlushReplayProbe());
  TestEqual(TEXT("After the consumed seq"), BatchSeq, (int64)6);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyRechainBelowZeroTest,
  "Redwood.Currency.RechainBelowZeroRefused",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A re-chain that would take the balance below zero writes nothing: a negative BalanceAfter is
// refused for good, and that freezes the character. The queue waits for a reload instead, and a
// later balance that fits re-chains it.
bool FRedwoodCurrencyRechainBelowZeroTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyPersistenceTest;
  AddExpectedErrorPlain(TEXT("below zero; currency stays held"), EAutomationExpectedErrorFlags::Contains, 2);
  URedwoodCharacterComponent *Component = Fixture::MakeTestComponent();
  // A purchase staged while 300 was still there.
  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdA, -100, 200)}, {});
  int32 RebasedCount = 0;
  Component->OnCurrencyRebased.AddLambda([&RebasedCount](int64) { ++RebasedCount; });

  // The trade took all 300.
  Component->RechainCurrencyAfterTrade(0);
  const TArray<FRedwoodCurrencyEntry> &Pending = Component->GetPendingCurrencyEntries();
  if (!TestEqual(TEXT("The purchase is kept"), Pending.Num(), 1)) {
    return false;
  }
  TestEqual(TEXT("Its BalanceAfter is not re-chained below zero"), Pending[0].BalanceAfter, (int64)200);
  TestTrue(TEXT("Currency waits for a reload"), Component->IsCurrencyRebaseRequired());
  TestEqual(TEXT("Nothing goes out"), Component->GetCurrencyEntriesToSendCount(), 0);
  TestEqual(TEXT("The game is not told a balance"), RebasedCount, 0);

  // A reload that still cannot pay leaves it held; one that can re-chains it.
  Component->RebaseCurrency(50);
  TestEqual(TEXT("A short reload writes nothing"), Pending[0].BalanceAfter, (int64)200);
  TestTrue(TEXT("It stays held"), Component->IsCurrencyRebaseRequired());
  Component->RebaseCurrency(100);
  TestEqual(TEXT("A reload that pays re-chains it"), Pending[0].BalanceAfter, (int64)0);
  TestFalse(TEXT("And releases it"), Component->IsCurrencyRebaseRequired());
  return true;
}
