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
  // warns; expect it in case the harness raises warnings to errors.
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
  FRedwoodCurrencyFinalRefusalDropsSentTest,
  "Redwood.Currency.FinalRefusalDropsSent",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodCurrencyFinalRefusalDropsSentTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodCurrencyPersistenceTest;
  URedwoodCharacterComponent *Component = Fixture::MakeTestComponent();

  FString ReceivedError;
  int32 BroadcastCount = 0;
  Component->OnItemFlushRefusedFinal.AddLambda(
    [&ReceivedError, &BroadcastCount](const FString &Error) {
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
  SentUpserts.Add(TPair<FString, uint64>(Fixture::ItemId, 1));

  Component->QueueCurrency({Fixture::MakeEntry(Fixture::EntryIdB, 100, 300)}, {});

  // A plain error never drops an entry; only an explicit final refusal does.
  Component->CompleteItemFlush(TEXT("overdraft"), 0, SentUpserts, Fixture::NoItems, 1, 1, false);
  TestEqual(TEXT("A non-final error keeps both entries"), Component->GetPendingCurrencyEntries().Num(), 2);
  TestEqual(TEXT("No final broadcast for a plain error"), BroadcastCount, 0);

  TestTrue(TEXT("Retry claim succeeds"), Component->TryBeginItemFlush(BatchSeq));
  Component->CompleteItemFlush(TEXT("overdraft"), 0, SentUpserts, Fixture::NoItems, 1, 1, true);

  const TArray<FRedwoodCurrencyEntry> &Pending = Component->GetPendingCurrencyEntries();
  TestEqual(TEXT("Only the refused entry is dropped"), Pending.Num(), 1);
  if (Pending.Num() == 1) {
    TestEqual(TEXT("The unsent entry stays"), Pending[0].Id, Fixture::EntryIdB);
  }
  TestEqual(TEXT("The refused key is dropped"), Component->GetPendingGrantKeys().Num(), 0);
  TestFalse(
    TEXT("The refused purchase's item row is not re-sent without its debit"),
    Component->GetDirtyItemGenerations().Contains(Fixture::ItemId)
  );
  TestEqual(TEXT("The refusal is broadcast once"), BroadcastCount, 1);
  TestEqual(TEXT("The broadcast carries the error"), ReceivedError, FString(TEXT("overdraft")));

  // B was chained on top of the refused debit, so it waits for the reload.
  TestTrue(TEXT("A rebase is required"), Component->IsCurrencyRebaseRequired());
  TestEqual(TEXT("Nothing is sent before the rebase"), Component->GetCurrencyEntriesToSendCount(), 0);
  Component->RebaseCurrency(500);
  if (Pending.Num() == 1) {
    TestEqual(TEXT("B re-chained from the reloaded balance"), Pending[0].BalanceAfter, (int64)600);
  }

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
