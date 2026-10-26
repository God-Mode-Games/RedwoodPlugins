// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart. Pins how a character's
// currency arrives and is saved without a backend: the `{ balances, grantKeys }` load object, the
// player-auth reply that must carry it with the writer token (a reply without them is a failed
// load, never a zero balance), and the offline save that applies staged entries to the snapshot.
// CurrencySidecarTest.cpp drives the auth leg's thaw and the hydration in a world.

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "RedwoodCharacterComponent.h"
#include "RedwoodCommonGameSubsystem.h"
#include "RedwoodCurrencyTypes.h"
#include "RedwoodGameModeComponent.h"
#include "RedwoodPlayerStateComponent.h"
#include "RedwoodServerGameSubsystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Named, not anonymous, for the same unity-build reason as ItemPersistenceTest.cpp.
namespace RedwoodCurrencyLoadTest {
  const FString EntryIdA = TEXT("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
  const FString EntryIdB = TEXT("BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB");
  const FString EntryIdC = TEXT("CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC");
  const FString GoldCurrency = TEXT("Currency.Gold");
  const FString MarksCurrency = TEXT("Currency.FactionMarks");
  const FString LootReason = TEXT("Currency.Reason.Loot");
  const FString BuyReason = TEXT("Currency.Reason.Merchant.Buy");
  const FString QuestGrantKey = TEXT("quest:Q1");
  // Kept in the outputs of a refused parse, to show that it wrote nothing.
  constexpr int64 UntouchedAmount = 77;
  const FString UntouchedCurrency = TEXT("Currency.Untouched");
  const FString UntouchedGrantKey = TEXT("untouched");
  const FString SentinelCharacterId = TEXT("character-before");

  TSharedPtr<FJsonObject> ParseJson(const FString &Text) {
    TSharedPtr<FJsonObject> Object;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
    FJsonSerializer::Deserialize(Reader, Object);
    return Object;
  }

  FRedwoodCurrencyEntry MakeEntry(
    const FString &Id, int64 Delta, int64 BalanceAfter, const FString &Reason
  ) {
    FRedwoodCurrencyEntry Entry;
    Entry.Id = Id;
    Entry.Currency = GoldCurrency;
    Entry.Delta = Delta;
    Entry.BalanceAfter = BalanceAfter;
    Entry.Reason = Reason;
    return Entry;
  }

  // The backend's player-auth success reply, with the parts a test removes or changes swapped in.
  // Character fields ParseCharacter reads unconditionally are present, so the parse logs nothing.
  FString MakeAuthReply(
    const TCHAR *CurrencyField = TEXT(R"("currency":{"balances":[{"currency":"Currency.Gold","amount":"1500000"}],"grantKeys":["quest:Q1"]},)"),
    const TCHAR *WriterTokenField = TEXT(R"("writerToken":"writer-token-1",)"),
    const TCHAR *InventorySeqField = TEXT(R"("inventorySeq":7,)")
  ) {
    return FString::Printf(
      TEXT(R"({"error":"",%s%s"items":[],"player":{"id":"player-1"},)")
      TEXT(R"("character":{%s"id":"character-1","playerId":"player-1","name":"Test Character",)")
      TEXT(R"("createdAt":"2026-07-20T00:00:00.000Z","updatedAt":"2026-07-20T00:00:00.000Z"}})"),
      CurrencyField,
      WriterTokenField,
      InventorySeqField
    );
  }

  // Headless: with no PlayerState owner the component only holds the snapshot.
  URedwoodPlayerStateComponent *MakePlayerStateComponent() {
    return NewObject<URedwoodPlayerStateComponent>();
  }

  // Headless: with no world QueueCurrency finds no subsystem, so nothing is frozen.
  URedwoodCharacterComponent *MakeCharacterComponent() {
    URedwoodCharacterComponent *Component = NewObject<URedwoodCharacterComponent>();
    Component->bUseItems = true;
    return Component;
  }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyLoadParseTest,
  "Redwood.Currency.LoadParse",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// The backend's CurrencyLoadSchema, read strictly. Every shape it would not send is a failed load,
// and a failed read writes nothing.
bool FRedwoodCurrencyLoadParseTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyLoadTest;

  FRedwoodCurrencyBalances Balances;
  TArray<FString> GrantKeys;
  TestTrue(
    TEXT("A balance and a key are read"),
    TryParseCurrencyLoad(
      ParseJson(TEXT(R"({"balances":[{"currency":"Currency.Gold","amount":"1500000"}],"grantKeys":["quest:Q1"]})")),
      Balances,
      GrantKeys
    )
  );
  TestEqual(TEXT("The balance"), GetCurrencyBalance(Balances, GoldCurrency), (int64)1500000);
  TestTrue(TEXT("The keys"), GrantKeys == TArray<FString>{QuestGrantKey});

  TestTrue(
    TEXT("A new character has an empty list"),
    TryParseCurrencyLoad(ParseJson(TEXT(R"({"balances":[],"grantKeys":[]})")), Balances, GrantKeys)
  );
  TestEqual(TEXT("A currency with no row holds zero"), GetCurrencyBalance(Balances, GoldCurrency), (int64)0);
  TestEqual(TEXT("No rows"), Balances.Num(), 0);
  TestEqual(TEXT("No keys"), GrantKeys.Num(), 0);

  // The backend keys a balance on the exact currency name, so two names that differ only in case are
  // two balances: neither a duplicate row nor one balance read for the other.
  TestTrue(
    TEXT("Two currencies that differ only in case are read"),
    TryParseCurrencyLoad(
      ParseJson(TEXT(R"({"balances":[{"currency":"Currency.Gold","amount":"5"},{"currency":"Currency.GOLD","amount":"6"}],"grantKeys":[]})")),
      Balances,
      GrantKeys
    )
  );
  TestEqual(TEXT("As two balances"), Balances.Num(), 2);
  TestEqual(TEXT("Each keeps its own amount"), GetCurrencyBalance(Balances, TEXT("Currency.GOLD")), (int64)6);
  TestEqual(TEXT("A third spelling holds zero"), GetCurrencyBalance(Balances, TEXT("currency.gold")), (int64)0);

  const TCHAR *Refused[] = {
    TEXT(R"({"grantKeys":[]})"),
    TEXT(R"({"balances":[]})"),
    TEXT(R"({"balances":"Currency.Gold","grantKeys":[]})"),
    TEXT(R"({"balances":[5],"grantKeys":[]})"),
    TEXT(R"({"balances":[{"currency":"Currency.Gold","amount":500}],"grantKeys":[]})"),
    TEXT(R"({"balances":[{"currency":"Currency.Gold","amount":null}],"grantKeys":[]})"),
    TEXT(R"({"balances":[{"currency":"Currency.Gold"}],"grantKeys":[]})"),
    TEXT(R"({"balances":[{"currency":"Currency.Gold","amount":"-5"}],"grantKeys":[]})"),
    TEXT(R"({"balances":[{"currency":"Currency.Gold","amount":"1e3"}],"grantKeys":[]})"),
    TEXT(R"({"balances":[{"currency":"Currency.Gold","amount":"05"}],"grantKeys":[]})"),
    TEXT(R"({"balances":[{"currency":"Currency.Gold","amount":"9223372036854775808"}],"grantKeys":[]})"),
    TEXT(R"({"balances":[{"currency":"","amount":"5"}],"grantKeys":[]})"),
    TEXT(R"({"balances":[{"currency":5,"amount":"5"}],"grantKeys":[]})"),
    TEXT(R"({"balances":[{"amount":"5"}],"grantKeys":[]})"),
    TEXT(R"({"balances":[{"currency":"Currency.Gold","amount":"5"},{"currency":"Currency.Gold","amount":"6"}],"grantKeys":[]})"),
    TEXT(R"({"balances":[{"currency":"Currency.Gold","amount":"5"},{"currency":"Currency.FactionMarks","amount":"-1"}],"grantKeys":[]})"),
    TEXT(R"({"balances":[{"currency":"Currency.Gold","amount":"5"}]})"),
    TEXT(R"({"balances":[{"currency":"Currency.Gold","amount":"5"}],"grantKeys":"quest:Q1"})"),
    TEXT(R"({"balances":[{"currency":"Currency.Gold","amount":"5"}],"grantKeys":[5]})"),
    TEXT(R"({"balances":[{"currency":"Currency.Gold","amount":"5"}],"grantKeys":[null]})"),
    TEXT(R"({"balances":[{"currency":"Currency.Gold","amount":"5"}],"grantKeys":[""]})"),
  };
  for (const TCHAR *Text : Refused) {
    Balances = {{UntouchedCurrency, UntouchedAmount}};
    GrantKeys = {UntouchedGrantKey};
    TestFalse(
      FString::Printf(TEXT("%s is refused"), Text), TryParseCurrencyLoad(ParseJson(Text), Balances, GrantKeys)
    );
    TestEqual(FString::Printf(TEXT("%s leaves the balances"), Text), Balances.Num(), 1);
    TestEqual(
      FString::Printf(TEXT("%s leaves the balance"), Text),
      GetCurrencyBalance(Balances, UntouchedCurrency),
      UntouchedAmount
    );
    TestTrue(FString::Printf(TEXT("%s leaves the keys"), Text), GrantKeys == TArray<FString>{UntouchedGrantKey});
  }
  TestFalse(
    TEXT("No object is refused"), TryParseCurrencyLoad(TSharedPtr<FJsonObject>(), Balances, GrantKeys)
  );

  // The offline writer shape reads back through the same rule.
  FRedwoodCurrencyBalances Written;
  Written.Add(GoldCurrency, MAX_int64);
  Written.Add(MarksCurrency, 0);
  TestTrue(
    TEXT("A written object reads back"),
    TryParseCurrencyLoad(MakeCurrencyLoadObject(Written, {QuestGrantKey}), Balances, GrantKeys)
  );
  TestEqual(TEXT("Every digit survives"), GetCurrencyBalance(Balances, GoldCurrency), MAX_int64);
  TestTrue(TEXT("A zero balance row survives"), Balances.Contains(MarksCurrency));
  TestTrue(TEXT("The key survives"), GrantKeys == TArray<FString>{QuestGrantKey});
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyLoadTwoCurrenciesTest,
  "Redwood.Currency.LoadTwoCurrencies",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// Each currency is its own balance: both rows are read, a currency with no row reads 0, and a
// currency named twice or with an empty name is a failed load.
bool FRedwoodCurrencyLoadTwoCurrenciesTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyLoadTest;

  FRedwoodCurrencyBalances Balances;
  TArray<FString> GrantKeys;
  TestTrue(
    TEXT("Two currencies are read"),
    TryParseCurrencyLoad(
      ParseJson(TEXT(R"({"balances":[{"currency":"Currency.Gold","amount":"900"},{"currency":"Currency.FactionMarks","amount":"12"}],"grantKeys":[]})")),
      Balances,
      GrantKeys
    )
  );
  TestEqual(TEXT("Gold"), GetCurrencyBalance(Balances, GoldCurrency), (int64)900);
  TestEqual(TEXT("Faction marks"), GetCurrencyBalance(Balances, MarksCurrency), (int64)12);
  TestEqual(TEXT("A currency with no row reads 0"), GetCurrencyBalance(Balances, TEXT("Currency.Other")), (int64)0);
  TestFalse(TEXT("Reading a missing currency adds no row"), Balances.Contains(TEXT("Currency.Other")));

  Balances = {{UntouchedCurrency, UntouchedAmount}};
  TestFalse(
    TEXT("A repeated currency fails the parse"),
    TryParseCurrencyLoad(
      ParseJson(TEXT(R"({"balances":[{"currency":"Currency.Gold","amount":"1"},{"currency":"Currency.FactionMarks","amount":"2"},{"currency":"Currency.Gold","amount":"3"}],"grantKeys":[]})")),
      Balances,
      GrantKeys
    )
  );
  TestEqual(TEXT("The failed parse leaves the balances"), GetCurrencyBalance(Balances, UntouchedCurrency), UntouchedAmount);
  TestFalse(
    TEXT("An empty currency name fails the parse"),
    TryParseCurrencyLoad(
      ParseJson(TEXT(R"({"balances":[{"currency":"Currency.Gold","amount":"1"},{"currency":"","amount":"2"}],"grantKeys":[]})")),
      Balances,
      GrantKeys
    )
  );
  TestFalse(TEXT("The failed parse adds no row"), Balances.Contains(GoldCurrency));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyAuthLoadTest,
  "Redwood.Currency.AuthLoad",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// The player-auth reply carries the balances, the grant keys and the writer token beside
// "character". A complete reply puts all of them on the snapshot with the seq of the same claim.
// A reply that lacks any of them is a failed load: nothing is set, so the join is refused rather
// than played with a zero balance or no way to save.
bool FRedwoodCurrencyAuthLoadTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyLoadTest;

  URedwoodPlayerStateComponent *Loaded = MakePlayerStateComponent();
  FString Error;
  TestTrue(
    TEXT("A complete reply loads"),
    URedwoodGameModeComponent::ApplyPlayerAuthCharacter(nullptr, Loaded, ParseJson(MakeAuthReply()), Error)
  );
  const FRedwoodCharacterBackend &Character = Loaded->RedwoodCharacter;
  TestEqual(TEXT("The character is set"), Character.Id, FString(TEXT("character-1")));
  TestEqual(TEXT("The seq of the claim"), Character.InventorySeq, (int64)7);
  TestEqual(
    TEXT("The balance of the claim"), GetCurrencyBalance(Character.CurrencyBalances, GoldCurrency), (int64)1500000
  );
  TestTrue(TEXT("The used grant keys"), Character.GrantKeys == TArray<FString>{QuestGrantKey});
  TestEqual(TEXT("The session's writer token"), Character.WriterToken, FString(TEXT("writer-token-1")));
  TestFalse(TEXT("The balance is known"), Character.bCurrencyBalancesUnknown);

  struct FIncompleteReply {
    const TCHAR *What;
    FString Reply;
  };
  const FIncompleteReply Incomplete[] = {
    {TEXT("no currency"), MakeAuthReply(TEXT(""))},
    {TEXT("a null currency"), MakeAuthReply(TEXT(R"("currency":null,)"))},
    {TEXT("a numeric amount"), MakeAuthReply(
      TEXT(R"("currency":{"balances":[{"currency":"Currency.Gold","amount":1500000}],"grantKeys":[]},)")
    )},
    {TEXT("a negative amount"), MakeAuthReply(
      TEXT(R"("currency":{"balances":[{"currency":"Currency.Gold","amount":"-5"}],"grantKeys":[]},)")
    )},
    {TEXT("a repeated currency"), MakeAuthReply(
      TEXT(R"("currency":{"balances":[{"currency":"Currency.Gold","amount":"5"},{"currency":"Currency.Gold","amount":"6"}],"grantKeys":[]},)")
    )},
    {TEXT("no grant list"), MakeAuthReply(
      TEXT(R"("currency":{"balances":[{"currency":"Currency.Gold","amount":"5"}]},)")
    )},
    {TEXT("no writer token"), MakeAuthReply(
      TEXT(R"("currency":{"balances":[],"grantKeys":[]},)"), TEXT("")
    )},
    {TEXT("an empty writer token"), MakeAuthReply(
      TEXT(R"("currency":{"balances":[],"grantKeys":[]},)"), TEXT(R"("writerToken":"",)")
    )},
    {TEXT("a null writer token"), MakeAuthReply(
      TEXT(R"("currency":{"balances":[],"grantKeys":[]},)"), TEXT(R"("writerToken":null,)")
    )},
    {TEXT("no inventory seq"), MakeAuthReply(
      TEXT(R"("currency":{"balances":[],"grantKeys":[]},)"), TEXT(R"("writerToken":"t",)"), TEXT("")
    )},
    {TEXT("no character"), TEXT(R"({"error":"","character":null,"currency":{"balances":[],"grantKeys":[]},"writerToken":"t"})")},
  };
  for (const FIncompleteReply &Case : Incomplete) {
    // A snapshot already in place, so that any write by the failed load shows.
    URedwoodPlayerStateComponent *Refused = MakePlayerStateComponent();
    Refused->RedwoodCharacter.Id = SentinelCharacterId;
    Refused->RedwoodCharacter.CurrencyBalances = {{UntouchedCurrency, UntouchedAmount}};
    FString CaseError;
    TestFalse(
      FString::Printf(TEXT("A reply with %s fails the load"), Case.What),
      URedwoodGameModeComponent::ApplyPlayerAuthCharacter(nullptr, Refused, ParseJson(Case.Reply), CaseError)
    );
    TestFalse(FString::Printf(TEXT("A reply with %s says why"), Case.What), CaseError.IsEmpty());
    TestEqual(
      FString::Printf(TEXT("A reply with %s sets no character"), Case.What),
      Refused->RedwoodCharacter.Id,
      SentinelCharacterId
    );
    TestEqual(
      FString::Printf(TEXT("A reply with %s sets no balance"), Case.What),
      GetCurrencyBalance(Refused->RedwoodCharacter.CurrencyBalances, UntouchedCurrency),
      UntouchedAmount
    );
  }
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyOfflineSaveTest,
  "Redwood.Currency.OfflineSave",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// Offline, the character file is the database: the save applies the staged entries to the
// snapshot and writes `{ balances, grantKeys }`, and the component is no longer dirty. The file also
// keeps the applied entry ids, so a repeat of one is refused after a reload, as the backend's
// ledger refuses it.
bool FRedwoodCurrencyOfflineSaveTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyLoadTest;
  AddExpectedErrorPlain(
    FString::Printf(TEXT("entry %s is already applied"), *EntryIdA), EAutomationExpectedErrorFlags::Contains, 1
  );

  FRedwoodCharacterBackend Character;
  URedwoodCharacterComponent *Component = MakeCharacterComponent();
  TestTrue(
    TEXT("The entries are staged"),
    Component->QueueCurrency(
      {MakeEntry(EntryIdA, 500, 500, LootReason), MakeEntry(EntryIdB, -200, 300, BuyReason)}, {QuestGrantKey}
    )
  );
  TestTrue(TEXT("The save applies them"), URedwoodServerGameSubsystem::ApplyOfflineCurrency(Character, Component));
  TestEqual(TEXT("+500 then -200 from 0"), GetCurrencyBalance(Character.CurrencyBalances, GoldCurrency), (int64)300);
  TestTrue(TEXT("The key is used"), Character.GrantKeys == TArray<FString>{QuestGrantKey});
  TestFalse(TEXT("Nothing stays staged"), Component->IsItemsDirty());

  TSharedPtr<FJsonObject> CharacterObject = MakeShared<FJsonObject>();
  URedwoodCommonGameSubsystem::WriteOfflineCurrency(Character, CharacterObject);
  const TSharedPtr<FJsonObject> *Written = nullptr;
  if (TestTrue(TEXT("The save writes a currency object"), CharacterObject->TryGetObjectField(TEXT("currency"), Written))) {
    const TArray<TSharedPtr<FJsonValue>> *Rows = nullptr;
    (*Written)->TryGetArrayField(TEXT("balances"), Rows);
    if (TestTrue(TEXT("The balances are a list"), Rows != nullptr) && TestEqual(TEXT("One row"), Rows->Num(), 1)) {
      const TSharedPtr<FJsonObject> *Row = nullptr;
      (*Rows)[0]->TryGetObject(Row);
      FString RowCurrency;
      FString AmountText;
      if (TestTrue(TEXT("The row is an object"), Row != nullptr)) {
        (*Row)->TryGetStringField(TEXT("currency"), RowCurrency);
        (*Row)->TryGetStringField(TEXT("amount"), AmountText);
      }
      TestEqual(TEXT("The row names its currency"), RowCurrency, GoldCurrency);
      TestEqual(TEXT("The amount is the string \"300\""), AmountText, FString(TEXT("300")));
    }
    FRedwoodCurrencyBalances ReadBalances;
    TArray<FString> ReadKeys;
    TestTrue(TEXT("The written object loads"), TryParseCurrencyLoad(*Written, ReadBalances, ReadKeys));
    TestEqual(TEXT("It loads the balance"), GetCurrencyBalance(ReadBalances, GoldCurrency), (int64)300);
    TestTrue(TEXT("It loads the key"), ReadKeys == TArray<FString>{QuestGrantKey});
  }

  TestFalse(
    TEXT("Nothing staged applies nothing"), URedwoodServerGameSubsystem::ApplyOfflineCurrency(Character, Component)
  );

  // The backend's keys are case-sensitive, so a key that differs only in case is a new one.
  const FString OtherCaseKey = TEXT("quest:q1");
  Component->QueueCurrency({}, {OtherCaseKey});
  TestTrue(
    TEXT("A key in another case is not a used one"),
    URedwoodServerGameSubsystem::ApplyOfflineCurrency(Character, Component)
  );
  TestEqual(TEXT("Both keys are kept"), Character.GrantKeys.Num(), 2);

  // The file as the next session loads it.
  TSharedPtr<FJsonObject> FileObject = ParseJson(
    TEXT(R"({"id":"character-1","playerId":"player-1","name":"Test Character",)")
    TEXT(R"("createdAt":"2026-07-20T00:00:00.000Z","updatedAt":"2026-07-20T00:00:00.000Z"})")
  );
  URedwoodCommonGameSubsystem::WriteOfflineCurrency(Character, FileObject);
  FRedwoodCharacterBackend Reloaded = URedwoodCommonGameSubsystem::ParseCharacter(FileObject);
  TestEqual(TEXT("The file keeps both applied ids"), Reloaded.OfflineAppliedEntryIds.Num(), 2);
  Component->QueueCurrency({MakeEntry(EntryIdA, 1, 301, LootReason)}, {});
  TestFalse(
    TEXT("A re-grant under an applied id is refused"),
    URedwoodServerGameSubsystem::ApplyOfflineCurrency(Reloaded, Component)
  );
  TestEqual(TEXT("It moves no balance"), GetCurrencyBalance(Reloaded.CurrencyBalances, GoldCurrency), (int64)300);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyOfflineSaveRefusesTest,
  "Redwood.Currency.OfflineSaveRefuses",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// The offline save refuses what the backend refuses, for the whole batch: the balance does not
// move, the batch is dropped and each part is logged. An unknown balance applies nothing and
// keeps the batch staged, and writes no balance over the file's own.
bool FRedwoodCurrencyOfflineSaveRefusesTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyLoadTest;
  // Below zero (1 entry), a wrong balanceAfter (2 entries), a used key (1 entry, 1 key).
  AddExpectedErrorPlain(TEXT("Offline save refused"), EAutomationExpectedErrorFlags::Contains, 5);
  AddExpectedErrorPlain(TEXT("has an unknown balance"), EAutomationExpectedErrorFlags::Contains, 1);

  struct FRefusedCase {
    const TCHAR *What;
    TArray<FRedwoodCurrencyEntry> Entries;
    TArray<FString> GrantKeys;
  };
  const FRefusedCase Cases[] = {
    {TEXT("an overdraft"), {MakeEntry(EntryIdA, -200, -100, BuyReason)}, {}},
    {TEXT("a wrong balanceAfter"),
     {MakeEntry(EntryIdA, 50, 150, LootReason), MakeEntry(EntryIdB, 50, 250, LootReason)},
     {}},
    {TEXT("a used grant key"), {MakeEntry(EntryIdA, 50, 150, LootReason)}, {QuestGrantKey}},
  };
  for (const FRefusedCase &Case : Cases) {
    FRedwoodCharacterBackend Character;
    Character.CurrencyBalances = {{GoldCurrency, 100}};
    Character.GrantKeys = {QuestGrantKey};
    URedwoodCharacterComponent *Component = MakeCharacterComponent();
    Component->QueueCurrency(Case.Entries, Case.GrantKeys);
    TestFalse(
      FString::Printf(TEXT("%s is refused"), Case.What),
      URedwoodServerGameSubsystem::ApplyOfflineCurrency(Character, Component)
    );
    TestEqual(FString::Printf(TEXT("%s moves no balance"), Case.What), GetCurrencyBalance(Character.CurrencyBalances, GoldCurrency), (int64)100);
    TestTrue(FString::Printf(TEXT("%s uses no key"), Case.What), Character.GrantKeys == TArray<FString>{QuestGrantKey});
    TestFalse(FString::Printf(TEXT("%s is dropped"), Case.What), Component->IsItemsDirty());
  }

  FRedwoodCharacterBackend Unknown;
  Unknown.CurrencyBalances = {{GoldCurrency, 100}};
  Unknown.bCurrencyBalancesUnknown = true;
  URedwoodCharacterComponent *Component = MakeCharacterComponent();
  Component->QueueCurrency({MakeEntry(EntryIdA, 50, 150, LootReason)}, {});
  TestFalse(
    TEXT("An unknown balance applies nothing"), URedwoodServerGameSubsystem::ApplyOfflineCurrency(Unknown, Component)
  );
  TestEqual(TEXT("The entry stays staged"), Component->GetPendingCurrencyEntries().Num(), 1);
  TestEqual(TEXT("The balance does not move"), GetCurrencyBalance(Unknown.CurrencyBalances, GoldCurrency), (int64)100);
  TSharedPtr<FJsonObject> CharacterObject = MakeShared<FJsonObject>();
  URedwoodCommonGameSubsystem::WriteOfflineCurrency(Unknown, CharacterObject);
  TestFalse(TEXT("No guessed balance is written"), CharacterObject->HasField(TEXT("currency")));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyOfflineTwoCurrenciesTest,
  "Redwood.Currency.OfflineTwoCurrencies",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// Offline, every entry is checked against the balance of its own currency: two currencies apply
// each to their own balance, and a debit that another currency could cover is still below zero for
// its own, so the whole batch is refused.
bool FRedwoodCurrencyOfflineTwoCurrenciesTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyLoadTest;
  AddExpectedErrorPlain(TEXT("takes the balance below zero"), EAutomationExpectedErrorFlags::Contains, 1);

  FRedwoodCurrencyEntry GoldCredit = MakeEntry(EntryIdA, 500, 600, LootReason);
  FRedwoodCurrencyEntry MarksCredit = MakeEntry(EntryIdB, 40, 1040, LootReason);
  MarksCredit.Currency = MarksCurrency;

  FRedwoodCharacterBackend Character;
  Character.CurrencyBalances = {{GoldCurrency, 100}, {MarksCurrency, 1000}};
  URedwoodCharacterComponent *Component = MakeCharacterComponent();
  Component->QueueCurrency({GoldCredit, MarksCredit}, {});
  TestTrue(TEXT("Both currencies apply"), URedwoodServerGameSubsystem::ApplyOfflineCurrency(Character, Component));
  TestEqual(TEXT("Gold moves by its own entry"), GetCurrencyBalance(Character.CurrencyBalances, GoldCurrency), (int64)600);
  TestEqual(TEXT("Marks move by their own entry"), GetCurrencyBalance(Character.CurrencyBalances, MarksCurrency), (int64)1040);

  // 700 gold does not exist, though 1040 marks could cover it.
  Component->QueueCurrency({MakeEntry(EntryIdC, -700, -100, BuyReason)}, {});
  TestFalse(TEXT("A debit past its own currency is refused"), URedwoodServerGameSubsystem::ApplyOfflineCurrency(Character, Component));
  TestEqual(TEXT("Gold does not move"), GetCurrencyBalance(Character.CurrencyBalances, GoldCurrency), (int64)600);
  TestEqual(TEXT("Marks do not move"), GetCurrencyBalance(Character.CurrencyBalances, MarksCurrency), (int64)1040);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyQueueRefusesNoCurrencyTest,
  "Redwood.Currency.QueueRefusesNoCurrency",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// The backend refuses an entry with no currency for good, so the queue refuses that one entry,
// loudly, and keeps the valid ones: the same rule as for a bad id.
bool FRedwoodCurrencyQueueRefusesNoCurrencyTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyLoadTest;
  AddExpectedErrorPlain(TEXT("it names no currency"), EAutomationExpectedErrorFlags::Contains, 2);

  FRedwoodCurrencyEntry NoCurrency = MakeEntry(EntryIdB, 5, 505, LootReason);
  NoCurrency.Currency.Empty();
  URedwoodCharacterComponent *Component = MakeCharacterComponent();
  TestFalse(TEXT("An entry with no currency is refused"), Component->QueueCurrency({NoCurrency}, {}));
  TestEqual(TEXT("Nothing is queued"), Component->GetPendingCurrencyEntries().Num(), 0);

  TestFalse(
    TEXT("A mixed call reports the refusal"),
    Component->QueueCurrency({MakeEntry(EntryIdA, 500, 500, LootReason), NoCurrency}, {})
  );
  TestEqual(TEXT("Only the valid entry is queued"), Component->GetPendingCurrencyEntries().Num(), 1);
  return true;
}
