// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart. Pins how a character's
// currency arrives: the `{ amount, grants }` load object, and the player-auth reply that must carry
// it with the writer token (a reply without them is a failed load, never a zero balance).
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
  const FString QuestGrantKey = TEXT("quest:Q1");
  // Kept in the outputs of a refused parse, to show that it wrote nothing.
  constexpr int64 UntouchedAmount = 77;
  const FString UntouchedGrantKey = TEXT("untouched");

  TSharedPtr<FJsonObject> ParseJson(const FString &Text) {
    TSharedPtr<FJsonObject> Object;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
    FJsonSerializer::Deserialize(Reader, Object);
    return Object;
  }

  // The backend's player-auth success reply, with the parts a test removes or changes swapped in.
  // Character fields ParseCharacter reads unconditionally are present, so the parse logs nothing.
  FString MakeAuthReply(
    const TCHAR *CurrencyField = TEXT(R"("currency":{"amount":"1500000","grants":["quest:Q1"]},)"),
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

  int64 Amount = 0;
  TArray<FString> GrantKeys;
  TestTrue(
    TEXT("A balance and a key are read"),
    TryParseCurrencyLoad(ParseJson(TEXT(R"({"amount":"1500000","grants":["quest:Q1"]})")), Amount, GrantKeys)
  );
  TestEqual(TEXT("The balance"), Amount, (int64)1500000);
  TestTrue(TEXT("The keys"), GrantKeys == TArray<FString>{QuestGrantKey});

  TestTrue(
    TEXT("A new character's zero balance is read"),
    TryParseCurrencyLoad(ParseJson(TEXT(R"({"amount":"0","grants":[]})")), Amount, GrantKeys)
  );
  TestEqual(TEXT("Zero"), Amount, (int64)0);
  TestEqual(TEXT("No keys"), GrantKeys.Num(), 0);

  const TCHAR *Refused[] = {
    TEXT(R"({"grants":[]})"),
    TEXT(R"({"amount":500,"grants":[]})"),
    TEXT(R"({"amount":null,"grants":[]})"),
    TEXT(R"({"amount":"-5","grants":[]})"),
    TEXT(R"({"amount":"1e3","grants":[]})"),
    TEXT(R"({"amount":"05","grants":[]})"),
    TEXT(R"({"amount":"9223372036854775808","grants":[]})"),
    TEXT(R"({"amount":"5"})"),
    TEXT(R"({"amount":"5","grants":"quest:Q1"})"),
    TEXT(R"({"amount":"5","grants":[5]})"),
    TEXT(R"({"amount":"5","grants":[null]})"),
    TEXT(R"({"amount":"5","grants":[""]})"),
  };
  for (const TCHAR *Text : Refused) {
    Amount = UntouchedAmount;
    GrantKeys = {UntouchedGrantKey};
    TestFalse(
      FString::Printf(TEXT("%s is refused"), Text), TryParseCurrencyLoad(ParseJson(Text), Amount, GrantKeys)
    );
    TestEqual(FString::Printf(TEXT("%s leaves the balance"), Text), Amount, UntouchedAmount);
    TestTrue(FString::Printf(TEXT("%s leaves the keys"), Text), GrantKeys == TArray<FString>{UntouchedGrantKey});
  }
  TestFalse(
    TEXT("No object is refused"), TryParseCurrencyLoad(TSharedPtr<FJsonObject>(), Amount, GrantKeys)
  );

  // The offline writer's shape reads back through the same rule.
  TestTrue(
    TEXT("A written object reads back"),
    TryParseCurrencyLoad(MakeCurrencyLoadObject(MAX_int64, {QuestGrantKey}), Amount, GrantKeys)
  );
  TestEqual(TEXT("Every digit survives"), Amount, MAX_int64);
  TestTrue(TEXT("The key survives"), GrantKeys == TArray<FString>{QuestGrantKey});
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyAuthLoadTest,
  "Redwood.Currency.AuthLoad",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// The player-auth reply carries the balance, the grant keys and the writer token beside
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
  TestEqual(TEXT("The balance of the claim"), Character.CurrencyAmount, (int64)1500000);
  TestTrue(TEXT("The used grant keys"), Character.GrantKeys == TArray<FString>{QuestGrantKey});
  TestEqual(TEXT("The session's writer token"), Character.WriterToken, FString(TEXT("writer-token-1")));
  TestFalse(TEXT("The balance is known"), Character.bCurrencyAmountUnknown);

  struct FIncompleteReply {
    const TCHAR *What;
    FString Reply;
  };
  const FIncompleteReply Incomplete[] = {
    {TEXT("no currency"), MakeAuthReply(TEXT(""))},
    {TEXT("a null currency"), MakeAuthReply(TEXT(R"("currency":null,)"))},
    {TEXT("a numeric amount"), MakeAuthReply(TEXT(R"("currency":{"amount":1500000,"grants":[]},)"))},
    {TEXT("a negative amount"), MakeAuthReply(TEXT(R"("currency":{"amount":"-5","grants":[]},)"))},
    {TEXT("no grant list"), MakeAuthReply(TEXT(R"("currency":{"amount":"5"},)"))},
    {TEXT("no writer token"), MakeAuthReply(
      TEXT(R"("currency":{"amount":"5","grants":[]},)"), TEXT("")
    )},
    {TEXT("an empty writer token"), MakeAuthReply(
      TEXT(R"("currency":{"amount":"5","grants":[]},)"), TEXT(R"("writerToken":"",)")
    )},
    {TEXT("a null writer token"), MakeAuthReply(
      TEXT(R"("currency":{"amount":"5","grants":[]},)"), TEXT(R"("writerToken":null,)")
    )},
    {TEXT("no inventory seq"), MakeAuthReply(
      TEXT(R"("currency":{"amount":"5","grants":[]},)"), TEXT(R"("writerToken":"t",)"), TEXT("")
    )},
    {TEXT("no character"), TEXT(R"({"error":"","character":null,"currency":{"amount":"5","grants":[]},"writerToken":"t"})")},
  };
  for (const FIncompleteReply &Case : Incomplete) {
    URedwoodPlayerStateComponent *Refused = MakePlayerStateComponent();
    FString CaseError;
    TestFalse(
      FString::Printf(TEXT("A reply with %s fails the load"), Case.What),
      URedwoodGameModeComponent::ApplyPlayerAuthCharacter(nullptr, Refused, ParseJson(Case.Reply), CaseError)
    );
    TestFalse(FString::Printf(TEXT("A reply with %s says why"), Case.What), CaseError.IsEmpty());
    TestTrue(
      FString::Printf(TEXT("A reply with %s sets no character"), Case.What), Refused->RedwoodCharacter.Id.IsEmpty()
    );
    TestEqual(
      FString::Printf(TEXT("A reply with %s sets no balance"), Case.What),
      Refused->RedwoodCharacter.CurrencyAmount,
      (int64)0
    );
  }
  return true;
}

