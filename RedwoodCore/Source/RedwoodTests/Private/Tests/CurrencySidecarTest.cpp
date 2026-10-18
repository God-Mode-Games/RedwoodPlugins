// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart. Drives the currency
// half of URedwoodServerGameSubsystem's item flush through a real sidecar socket on a fake server:
// the hold while a reload is needed, the settle a held attempt carries, the rows a re-sent batch
// must not carry, a batch of currency alone, the per-character hold, late reload replies, the wait
// for the sidecar, and the give-up. Each test fails if the code path it names is removed.

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "CurrencyFlushTestActor.h"
#include "FakeSocketIoServer.h"
#include "RedwoodCharacterComponent.h"
#include "RedwoodCurrencyTypes.h"
#include "RedwoodServerGameSubsystem.h"
#include "SocketIOClient.h"
#include "SocketIONative.h"

#if WITH_AUTOMATION_WORKER

// The private members these tests drive, and nothing else.
class FRedwoodCurrencyFlushTestAccess {
public:
  static TSharedPtr<FSocketIONative> &Sidecar(URedwoodServerGameSubsystem &Subsystem) {
    return Subsystem.Sidecar;
  }
  static const FRedwoodCurrencyReload *Reload(
    URedwoodServerGameSubsystem &Subsystem, const FString &CharacterId
  ) {
    return Subsystem.CurrencyReloads.Find(CharacterId);
  }
  // The reply timer firing, without waiting for it.
  static void TimeOutReload(URedwoodServerGameSubsystem &Subsystem, const FString &CharacterId) {
    if (const FRedwoodCurrencyReload *Reload = Subsystem.CurrencyReloads.Find(CharacterId)) {
      Subsystem.HandleCurrencyReloadTimeout(CharacterId, Reload->Generation);
    }
  }
  // The retry timer firing, without waiting for it.
  static void SendReloadNow(URedwoodServerGameSubsystem &Subsystem, const FString &CharacterId) {
    Subsystem.SendCurrencyReload(CharacterId);
  }
  static void SidecarConnected(URedwoodServerGameSubsystem &Subsystem) {
    Subsystem.ResumeCurrencyReloadsAfterSidecarConnect();
  }
};

// Named, not anonymous, for the same unity-build reason as ItemPersistenceTest.cpp.
namespace RedwoodCurrencySidecarTest {
  using namespace RedwoodFakeSocketIo;
  using FAccess = FRedwoodCurrencyFlushTestAccess;

  const FString CharacterId = TEXT("character-1");
  const FString WriterToken = TEXT("writer-token-1");
  const FString EntryIdA = TEXT("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
  const FString LootReason = TEXT("Currency.Reason.Loot");
  const FString OwnedItemId = TEXT("11111111-1111-1111-1111-111111111111");
  const FString PurchasedItemId = TEXT("22222222-2222-2222-2222-222222222222");
  const FString LaterItemId = TEXT("33333333-3333-3333-3333-333333333333");
  constexpr int64 SeededInventorySeq = 4;
  // Long enough for a request the game thread would send to reach the server, so its absence
  // means it was held. Pumped at the fake server's 10 Hz.
  constexpr double QuietWindowSeconds = 0.5;

  const TCHAR *const FlushEvent = TEXT("realm:characters:items:flush");
  const TCHAR *const LoadEvent = TEXT("realm:characters:items:load");
  const TCHAR *const TradeUsedSeqReply =
    TEXT(R"({"error":"batch seq was used by a trade","committedSeq":5,"rebaseRequired":true})");

  FRedwoodCurrencyEntry MakeEntry(const FString &Id, int64 Delta, int64 BalanceAfter) {
    FRedwoodCurrencyEntry Entry;
    Entry.Id = Id;
    Entry.Delta = Delta;
    Entry.BalanceAfter = BalanceAfter;
    Entry.Reason = LootReason;
    return Entry;
  }

  FRedwoodItemRecord MakeRecord(const FString &Id) {
    FRedwoodItemRecord Record;
    Record.Id = Id;
    Record.Domain = TEXT("general");
    Record.Quantity = 1;
    Record.TemplateId = TEXT("T");
    return Record;
  }

  // A load reply with one row (the owned item, as the backend holds it) and the given balance.
  FString MakeLoadReply(const TCHAR *Balance) {
    return FString::Printf(
      TEXT(R"({"error":null,"items":[{"id":"%s","parentId":null,"domain":"general","slot":0,"quantity":1,"templateId":"T","attributes":{}}],"currency":{"amount":"%s","grants":[]}})"),
      *OwnedItemId,
      Balance
    );
  }

  void PumpFor(double Seconds) {
    const double End = FPlatformTime::Seconds() + Seconds;
    while (FPlatformTime::Seconds() < End) {
      FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
      FPlatformProcess::Sleep(PumpIntervalSeconds);
    }
  }

  // One request as the server read it.
  struct FRequest {
    FString AckId;
    FString Event;
    TSharedPtr<FJsonObject> Payload;
  };

  // A game instance with the subsystem, a world for the test actors, and the subsystem's sidecar
  // socket on a fake server.
  struct FSidecarHarness {
    FAutomationTestBase &Test;
    TUniquePtr<FFakeSocketIoServer> Server = MakeUnique<FFakeSocketIoServer>();
    UGameInstance *GameInstance = nullptr;
    UWorld *World = nullptr;
    URedwoodServerGameSubsystem *Subsystem = nullptr;
    TSharedPtr<FSocketIONative> SidecarCopy;

    explicit FSidecarHarness(FAutomationTestBase &InTest) : Test(InTest) {
      GameInstance = NewObject<UGameInstance>(GEngine);
      GameInstance->InitializeStandalone();
      World = GameInstance->GetWorld();
      World->InitializeActorsForPlay(FURL());
      Subsystem = GameInstance->GetSubsystem<URedwoodServerGameSubsystem>();
      if (Subsystem) {
        TSharedPtr<FSocketIONative> &Sidecar = FAccess::Sidecar(*Subsystem);
        Sidecar = ISocketIOClientModule::Get().NewValidNativePointer();
        SidecarCopy = Sidecar;
      }
    }

    bool Connect() {
      if (!Test.TestNotNull(TEXT("The subsystem exists"), Subsystem) ||
          !Test.TestTrue(TEXT("The fake sidecar listens"), Server->Listen())) {
        return false;
      }
      SidecarCopy->Connect(FString::Printf(TEXT("ws://127.0.0.1:%d"), Server->Port));
      TSharedPtr<FSocketIONative> Socket = SidecarCopy;
      return Test.TestTrue(TEXT("The sidecar opens a session"), Server->AcceptSession()) &&
        Test.TestTrue(
          TEXT("The sidecar connects"),
          PumpGameThreadUntil([Socket]() { return Socket->bIsConnected; })
        );
    }

    // A character component on its own actor, hydrated as the login leg would leave it.
    URedwoodCharacterComponent *MakeComponent(ARedwoodCurrencyFlushTestActor *&OutActor) {
      OutActor = World->SpawnActor<ARedwoodCurrencyFlushTestActor>();
      URedwoodCharacterComponent *Component = NewObject<URedwoodCharacterComponent>(OutActor);
      Component->bUseItems = true;
      Component->RedwoodCharacterId = CharacterId;
      Component->SeedWriterToken(WriterToken);
      Component->SeedItemSeqFromCharacter(SeededInventorySeq);
      Component->OnRedwoodCharacterUpdated.AddDynamic(
        OutActor, &ARedwoodCurrencyFlushTestActor::HandleCharacterUpdated
      );
      return Component;
    }

    bool ReadRequest(const TCHAR *ExpectedEvent, FRequest &OutRequest) {
      FString Text;
      if (!Test.TestTrue(TEXT("A request arrives"), Server->ReadClientText(Text))) {
        return false;
      }
      const int32 ArgsStart = Text.Find(TEXT("["));
      if (!Test.TestTrue(TEXT("The request is an event with an ack"), Text.StartsWith(TEXT("42")) && ArgsStart > 2)) {
        return false;
      }
      OutRequest.AckId = Text.Mid(2, ArgsStart - 2);
      TArray<TSharedPtr<FJsonValue>> Args;
      TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text.Mid(ArgsStart));
      if (!Test.TestTrue(TEXT("The request parses"), FJsonSerializer::Deserialize(Reader, Args) && Args.Num() == 2)) {
        return false;
      }
      OutRequest.Event = Args[0]->AsString();
      OutRequest.Payload = Args[1]->AsObject();
      return Test.TestEqual(TEXT("The request is the expected event"), OutRequest.Event, FString(ExpectedEvent));
    }

    bool Answer(const FRequest &Request, const FString &ReplyJson) {
      const FString Reply = FString::Printf(TEXT("43%s[%s]"), *Request.AckId, *ReplyJson);
      return Test.TestTrue(TEXT("The server answers"), Server->SendText(StringCast<ANSICHAR>(*Reply).Get()));
    }

    // Lets any request the game thread would send reach the server, then says whether one did.
    bool IsQuiet() {
      PumpFor(QuietWindowSeconds);
      return !Server->HasClientData();
    }

    bool PumpUntil(TFunctionRef<bool()> Done) {
      return PumpGameThreadUntil(Done);
    }

    ~FSidecarHarness() {
      // The library queues game-thread callbacks that point at the socket, and the subsystem frees
      // it on a background thread. Keep it until that release is done and the queue has run, so no
      // callback of this test runs on a freed socket in a later test.
      Server.Reset();
      if (SidecarCopy.IsValid()) {
        TSharedPtr<FSocketIONative> Socket = SidecarCopy;
        PumpGameThreadUntil([Socket]() { return !Socket->bIsConnected; });
      }
      if (GameInstance) {
        GameInstance->Shutdown();
      }
      if (SidecarCopy.IsValid()) {
        TSharedPtr<FSocketIONative> *Socket = &SidecarCopy;
        Test.TestTrue(
          TEXT("The library releases the sidecar"),
          PumpGameThreadUntil([Socket]() { return Socket->GetSharedReferenceCount() == 1; })
        );
      }
      if (World) {
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
      }
    }
  };

  int32 CountArray(const TSharedPtr<FJsonObject> &Payload, const TCHAR *Field) {
    const TArray<TSharedPtr<FJsonValue>> *Values = nullptr;
    return Payload.IsValid() && Payload->TryGetArrayField(Field, Values) ? Values->Num() : -1;
  }

  FString FirstBalanceAfter(const TSharedPtr<FJsonObject> &Payload) {
    const TArray<TSharedPtr<FJsonValue>> *Values = nullptr;
    const TSharedPtr<FJsonObject> *Entry = nullptr;
    if (Payload.IsValid() && Payload->TryGetArrayField(TEXT("currency"), Values) && Values->Num() > 0 &&
        (*Values)[0]->TryGetObject(Entry)) {
      return (*Entry)->GetStringField(TEXT("balanceAfter"));
    }
    return FString();
  }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarCurrencyAloneTest,
  "Redwood.Currency.Sidecar.CurrencyAloneIsSent",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A balance change with no item change still goes out, and its reply drops it.
bool FRedwoodCurrencySidecarCurrencyAloneTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Component = Harness.MakeComponent(Actor);
  Component->QueueCurrency({MakeEntry(EntryIdA, 500, 1500)}, {});

  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component);
  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }
  TestEqual(TEXT("The entry goes out"), CountArray(Flush.Payload, TEXT("currency")), 1);
  TestEqual(TEXT("No rows go out"), CountArray(Flush.Payload, TEXT("upserts")), 0);
  TestEqual(TEXT("The writer token goes out"), Flush.Payload->GetStringField(TEXT("writerToken")), WriterToken);

  Harness.Answer(Flush, TEXT(R"({"error":null,"committedSeq":5})"));
  TestTrue(
    TEXT("The reply drops the saved entry"),
    Harness.PumpUntil([Component]() { return Component->GetPendingCurrencyEntries().Num() == 0; })
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarResendCarriesNoRowsTest,
  "Redwood.Currency.Sidecar.ResendCarriesNoRows",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A re-send of a batch whose outcome is unknown carries no rows: the backend applies none of them
// in a replay, yet its success reply would clear them.
bool FRedwoodCurrencySidecarResendCarriesNoRowsTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  AddExpectedError(TEXT("Failed to flush items"), EAutomationExpectedErrorFlags::Contains, 0);
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Component = Harness.MakeComponent(Actor);
  Actor->Items = {MakeRecord(OwnedItemId), MakeRecord(LaterItemId)};
  Component->MarkItemsDirty({OwnedItemId});
  Component->QueueCurrency({MakeEntry(EntryIdA, -250, 750)}, {});

  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component);
  FRequest First;
  if (!Harness.ReadRequest(FlushEvent, First)) {
    return false;
  }
  TestEqual(TEXT("The first attempt carries the row"), CountArray(First.Payload, TEXT("upserts")), 1);
  // The outcome is unknown: no fence.
  Harness.Answer(First, TEXT(R"({"error":"Item flush request timed out."})"));
  Harness.PumpUntil([Component]() { return !Component->IsItemFlushInFlight(); });

  Component->MarkItemsDirty({LaterItemId});
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component);
  FRequest Resend;
  if (!Harness.ReadRequest(FlushEvent, Resend)) {
    return false;
  }
  TestEqual(TEXT("The re-send keeps its seq"), Resend.Payload->GetNumberField(TEXT("batchSeq")), 5.0);
  TestEqual(TEXT("The re-send carries no upserts"), CountArray(Resend.Payload, TEXT("upserts")), 0);
  TestEqual(TEXT("The re-send carries no deletes"), CountArray(Resend.Payload, TEXT("deletes")), 0);
  TestEqual(TEXT("The re-send carries the entry"), CountArray(Resend.Payload, TEXT("currency")), 1);

  // The first attempt had committed: the backend acks the replay.
  Harness.Answer(Resend, TEXT(R"({"error":null,"committedSeq":5})"));
  Harness.PumpUntil([Component]() { return !Component->IsItemFlushInFlight(); });
  TestEqual(TEXT("The entry is saved"), Component->GetPendingCurrencyEntries().Num(), 0);
  TestTrue(TEXT("The owned row stays dirty"), Component->GetDirtyItemGenerations().Contains(OwnedItemId));
  TestTrue(TEXT("The later row stays dirty"), Component->GetDirtyItemGenerations().Contains(LaterItemId));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarFinalRefusalHoldsTest,
  "Redwood.Currency.Sidecar.FinalRefusalHoldsUntilReload",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A refused purchase: its item is still in the records. A later mark of that item must not save it
// before the reload, and the reload puts the backend's rows back.
bool FRedwoodCurrencySidecarFinalRefusalHoldsTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  AddExpectedError(TEXT("Failed to flush items"), EAutomationExpectedErrorFlags::Contains, 0);
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Component = Harness.MakeComponent(Actor);
  int64 RebasedBalance = -1;
  Component->OnCurrencyRebased.AddLambda([&RebasedBalance](int64 Balance) {
    RebasedBalance = Balance;
  });

  // The game buys an item it cannot pay for.
  Actor->Items = {MakeRecord(OwnedItemId), MakeRecord(PurchasedItemId)};
  Component->MarkItemsDirty({PurchasedItemId});
  Component->QueueCurrency({MakeEntry(EntryIdA, -2000, -1000)}, {});

  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component);
  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }
  Harness.Answer(Flush, TEXT(R"({"error":"balance below zero","committedSeq":4,"final":true})"));

  FRequest Load;
  if (!Harness.ReadRequest(LoadEvent, Load)) {
    return false;
  }
  TestEqual(TEXT("The reload carries the writer token"), Load.Payload->GetStringField(TEXT("writerToken")), WriterToken);

  // The game touches the refused item again while the reload runs.
  Component->MarkItemsDirty({PurchasedItemId});
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component);
  TestTrue(TEXT("Nothing flushes while the reload runs"), Harness.IsQuiet());

  Harness.Answer(Load, MakeLoadReply(TEXT("1000")));
  TestTrue(
    TEXT("The reload lands"),
    Harness.PumpUntil([Component]() { return !Component->IsReloadRequired(); })
  );
  TestEqual(TEXT("The records hold the backend's rows only"), Actor->Items.Num(), 1);
  if (Actor->Items.Num() == 1) {
    TestEqual(TEXT("The owned item stays"), Actor->Items[0].Id, OwnedItemId);
  }
  TestFalse(TEXT("The refused item is not dirty"), Component->GetDirtyItemGenerations().Contains(PurchasedItemId));
  TestEqual(TEXT("The game rereads its inventory"), Actor->CharacterUpdatedCount, 1);
  TestEqual(TEXT("The game hears the balance"), RebasedBalance, (int64)1000);
  TestTrue(TEXT("Nothing is left to send"), Harness.IsQuiet());
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarSettleWaitsForReloadTest,
  "Redwood.Currency.Sidecar.SettleWaitsForReload",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A detached pawn's flush that needs a reload settles only once the re-chained entry is saved, so
// its write binding cannot be released before then.
bool FRedwoodCurrencySidecarSettleWaitsForReloadTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  AddExpectedError(TEXT("Failed to flush items"), EAutomationExpectedErrorFlags::Contains, 0);
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Component = Harness.MakeComponent(Actor);
  Component->QueueCurrency({MakeEntry(EntryIdA, 500, 1500)}, {});

  int32 SettleCount = 0;
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component, [&SettleCount]() { ++SettleCount; });
  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }
  Harness.Answer(Flush, TradeUsedSeqReply);

  FRequest Load;
  if (!Harness.ReadRequest(LoadEvent, Load)) {
    return false;
  }
  TestEqual(TEXT("No settle while the entry is unsaved"), SettleCount, 0);
  Harness.Answer(Load, MakeLoadReply(TEXT("2000")));

  FRequest Resumed;
  if (!Harness.ReadRequest(FlushEvent, Resumed)) {
    return false;
  }
  TestEqual(TEXT("The entry is re-chained from the reload"), FirstBalanceAfter(Resumed.Payload), FString(TEXT("2500")));
  TestEqual(TEXT("Still no settle"), SettleCount, 0);
  Harness.Answer(Resumed, TEXT(R"({"error":null,"committedSeq":6})"));
  TestTrue(TEXT("The attempt settles once saved"), Harness.PumpUntil([&SettleCount]() { return SettleCount > 0; }));
  TestEqual(TEXT("It settles exactly once"), SettleCount, 1);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarCharacterHoldTest,
  "Redwood.Currency.Sidecar.HoldIsPerCharacter",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// While one component of a character reloads, another component of the same character sends
// nothing, and goes out when the reload lands.
bool FRedwoodCurrencySidecarCharacterHoldTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  AddExpectedError(TEXT("Failed to flush items"), EAutomationExpectedErrorFlags::Contains, 0);
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyFlushTestActor *CurrencyActor = nullptr;
  URedwoodCharacterComponent *CurrencyComponent = Harness.MakeComponent(CurrencyActor);
  ARedwoodCurrencyFlushTestActor *ItemActor = nullptr;
  URedwoodCharacterComponent *ItemComponent = Harness.MakeComponent(ItemActor);

  // Refused for good, so the currency component has nothing left to send after the reload, and
  // the only flush that follows is the held one.
  CurrencyComponent->QueueCurrency({MakeEntry(EntryIdA, -2000, -1000)}, {});
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, CurrencyComponent);
  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }
  Harness.Answer(Flush, TEXT(R"({"error":"balance below zero","committedSeq":4,"final":true})"));
  FRequest Load;
  if (!Harness.ReadRequest(LoadEvent, Load)) {
    return false;
  }

  ItemActor->Items = {MakeRecord(LaterItemId)};
  ItemComponent->MarkItemsDirty({LaterItemId});
  ItemComponent->SeedItemSeqFromCharacter(5);
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, ItemComponent);
  TestTrue(TEXT("The other component of the character is held"), Harness.IsQuiet());

  Harness.Answer(Load, MakeLoadReply(TEXT("1000")));
  FRequest Resumed;
  if (!Harness.ReadRequest(FlushEvent, Resumed)) {
    return false;
  }
  TestEqual(TEXT("The held row goes out after the reload"), CountArray(Resumed.Payload, TEXT("upserts")), 1);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarLateReloadReplyTest,
  "Redwood.Currency.Sidecar.LateReloadReplyIgnored",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// Reload 1 times out and reload 2 goes out. Reload 1's late reply must not rebase: its balance may
// predate a trade that reload 2's includes.
bool FRedwoodCurrencySidecarLateReloadReplyTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  AddExpectedError(TEXT("Failed to flush items"), EAutomationExpectedErrorFlags::Contains, 0);
  AddExpectedError(TEXT("Currency reload failed"), EAutomationExpectedErrorFlags::Contains, 0);
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Component = Harness.MakeComponent(Actor);
  Component->QueueCurrency({MakeEntry(EntryIdA, 500, 1500)}, {});

  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component);
  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }
  Harness.Answer(Flush, TradeUsedSeqReply);
  FRequest FirstLoad;
  if (!Harness.ReadRequest(LoadEvent, FirstLoad)) {
    return false;
  }

  FAccess::TimeOutReload(*Harness.Subsystem, CharacterId);
  FAccess::SendReloadNow(*Harness.Subsystem, CharacterId);
  FRequest SecondLoad;
  if (!Harness.ReadRequest(LoadEvent, SecondLoad)) {
    return false;
  }

  Harness.Answer(FirstLoad, MakeLoadReply(TEXT("9999")));
  PumpFor(QuietWindowSeconds);
  TestTrue(TEXT("The late reply does not rebase"), Component->IsCurrencyRebaseRequired());

  Harness.Answer(SecondLoad, MakeLoadReply(TEXT("2000")));
  TestTrue(
    TEXT("The current reply rebases"),
    Harness.PumpUntil([Component]() { return !Component->IsCurrencyRebaseRequired(); })
  );
  const TArray<FRedwoodCurrencyEntry> &Pending = Component->GetPendingCurrencyEntries();
  if (TestEqual(TEXT("The entry is kept"), Pending.Num(), 1)) {
    TestEqual(TEXT("It is chained from the current reply"), Pending[0].BalanceAfter, (int64)2500);
  }
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarDownWaitsTest,
  "Redwood.Currency.Sidecar.ReloadWaitsForSidecar",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A reload attempt that finds the sidecar down spends no attempt, and the connect event sends it.
bool FRedwoodCurrencySidecarDownWaitsTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  AddExpectedError(TEXT("Failed to flush items"), EAutomationExpectedErrorFlags::Contains, 0);
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Component = Harness.MakeComponent(Actor);
  Component->QueueCurrency({MakeEntry(EntryIdA, 500, 1500)}, {});

  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component);
  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }
  Harness.Answer(Flush, TradeUsedSeqReply);
  FRequest FirstLoad;
  if (!Harness.ReadRequest(LoadEvent, FirstLoad)) {
    return false;
  }
  FAccess::TimeOutReload(*Harness.Subsystem, CharacterId);

  // The sidecar is reconnecting when the retry fires. Set by hand, as ReloginFailureTest does.
  Harness.SidecarCopy->bIsConnected = false;
  for (int32 Retry = 0; Retry < URedwoodServerGameSubsystem::CurrencyReloadMaxAttempts * 2; ++Retry) {
    FAccess::SendReloadNow(*Harness.Subsystem, CharacterId);
  }
  const FRedwoodCurrencyReload *Waiting = FAccess::Reload(*Harness.Subsystem, CharacterId);
  if (TestNotNull(TEXT("The reload still runs"), Waiting)) {
    TestTrue(TEXT("It waits for the sidecar"), Waiting->bWaitingForSidecar);
    TestEqual(TEXT("Waiting spends no attempt"), Waiting->Attempts, 1);
  }

  Harness.SidecarCopy->bIsConnected = true;
  FAccess::SidecarConnected(*Harness.Subsystem);
  FRequest SecondLoad;
  if (!Harness.ReadRequest(LoadEvent, SecondLoad)) {
    return false;
  }
  Harness.Answer(SecondLoad, MakeLoadReply(TEXT("2000")));
  TestTrue(
    TEXT("The reload lands after the connect"),
    Harness.PumpUntil([Component]() { return !Component->IsCurrencyRebaseRequired(); })
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarGiveUpTest,
  "Redwood.Currency.Sidecar.GiveUpSettlesAndLogs",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// After the last failed attempt the held settle fires, so a detached pawn's binding still
// releases, and the error log names every entry left unsaved.
bool FRedwoodCurrencySidecarGiveUpTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  AddExpectedError(TEXT("Failed to flush items"), EAutomationExpectedErrorFlags::Contains, 0);
  AddExpectedError(TEXT("Currency reload failed"), EAutomationExpectedErrorFlags::Contains, 0);
  AddExpectedErrorPlain(
    TEXT("gave up (6 attempts failed). Its held flushes now settle; these are NOT saved: [entry AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA delta 500 reason Currency.Reason.Loot]"),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Component = Harness.MakeComponent(Actor);
  Component->QueueCurrency({MakeEntry(EntryIdA, 500, 1500)}, {});

  int32 SettleCount = 0;
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component, [&SettleCount]() { ++SettleCount; });
  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }
  Harness.Answer(Flush, TradeUsedSeqReply);

  const FString FailedLoad =
    TEXT(R"({"error":"Item load request timed out.","items":[],"currency":{"amount":"0","grants":[]}})");
  for (int32 Attempt = 1; Attempt <= URedwoodServerGameSubsystem::CurrencyReloadMaxAttempts; ++Attempt) {
    if (Attempt > 1) {
      FAccess::SendReloadNow(*Harness.Subsystem, CharacterId);
    }
    FRequest Load;
    if (!Harness.ReadRequest(LoadEvent, Load)) {
      return false;
    }
    TestEqual(TEXT("No settle before the give-up"), SettleCount, 0);
    Harness.Answer(Load, FailedLoad);
    Harness.PumpUntil([&Harness]() {
      const FRedwoodCurrencyReload *Reload = FAccess::Reload(*Harness.Subsystem, CharacterId);
      return !Reload || Reload->Generation == 0;
    });
  }

  TestEqual(TEXT("The held attempt settles once"), SettleCount, 1);
  TestNull(TEXT("The reload is over"), FAccess::Reload(*Harness.Subsystem, CharacterId));
  TestEqual(TEXT("The entry stays held on the component"), Component->GetPendingCurrencyEntries().Num(), 1);
  TestTrue(TEXT("It still needs a reload"), Component->IsCurrencyRebaseRequired());
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarStaleTokenTest,
  "Redwood.Currency.Sidecar.StaleTokenGivesUpAtOnce",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// Another session owns the character: no retry can help, so the reload ends on the first reply.
bool FRedwoodCurrencySidecarStaleTokenTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  AddExpectedError(TEXT("Failed to flush items"), EAutomationExpectedErrorFlags::Contains, 0);
  AddExpectedError(TEXT("Currency reload failed for good"), EAutomationExpectedErrorFlags::Contains, 1);
  AddExpectedErrorPlain(TEXT("gave up (this session no longer owns the character)"), EAutomationExpectedErrorFlags::Contains, 1);
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Component = Harness.MakeComponent(Actor);
  Component->QueueCurrency({MakeEntry(EntryIdA, 500, 1500)}, {});

  int32 SettleCount = 0;
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component, [&SettleCount]() { ++SettleCount; });
  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }
  Harness.Answer(Flush, TradeUsedSeqReply);
  FRequest Load;
  if (!Harness.ReadRequest(LoadEvent, Load)) {
    return false;
  }
  Harness.Answer(
    Load,
    TEXT(R"({"error":"character not bound to this instance","items":[],"currency":{"amount":"0","grants":[]}})")
  );
  TestTrue(TEXT("The held attempt settles at once"), Harness.PumpUntil([&SettleCount]() { return SettleCount > 0; }));
  TestNull(TEXT("No retry is scheduled"), FAccess::Reload(*Harness.Subsystem, CharacterId));
  return true;
}

#endif // WITH_AUTOMATION_WORKER
