// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart. Drives the currency
// half of URedwoodServerGameSubsystem's item flush through a real sidecar socket on a fake server:
// a batch of currency alone, the rows a re-sent batch must not carry, the freeze after a final
// refusal, the hold while a rebase reload runs and the settle a held attempt carries, the
// per-character hold, late reload replies, the wait for the sidecar through its real connect
// hook, the give-ups, the shutdown, and a component that was never hydrated; and the trade's
// currency: the lanes it holds, its commit, its refusal, its resend and its freeze. Each test fails
// if the code path it names is removed.

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
#include "RedwoodGameModeComponent.h"
#include "RedwoodPlayerStateComponent.h"
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
  // The same connect callback InitializeSidecar installs, on the test's socket.
  static void BindConnectedCallback(URedwoodServerGameSubsystem &Subsystem) {
    Subsystem.BindSidecarConnectedCallback();
  }
  // A final refusal's freeze, without driving one through a flush.
  static void FreezeCharacter(URedwoodServerGameSubsystem &Subsystem, const FString &CharacterId) {
    Subsystem.FreezeCharacter(CharacterId, nullptr);
  }
  static const FRedwoodPendingItemTrade *Trade(
    URedwoodServerGameSubsystem &Subsystem, const FString &TradeId
  ) {
    return Subsystem.PendingItemTrades.Find(TradeId);
  }
  // The trade's reply timer firing, without waiting for it.
  static void TimeOutTrade(URedwoodServerGameSubsystem &Subsystem, const FString &TradeId) {
    if (const FRedwoodPendingItemTrade *Pending = Subsystem.PendingItemTrades.Find(TradeId)) {
      Subsystem.HandleItemsTradeTimeout(TradeId, Pending->Generation);
    }
  }
  // The trade's resend timer firing, without waiting for it.
  static void ResendTradeNow(URedwoodServerGameSubsystem &Subsystem, const FString &TradeId) {
    Subsystem.SendItemsTrade(TradeId);
  }
};

// Named, not anonymous, for the same unity-build reason as ItemPersistenceTest.cpp.
namespace RedwoodCurrencySidecarTest {
  using namespace RedwoodFakeSocketIo;
  using FAccess = FRedwoodCurrencyFlushTestAccess;

  const FString CharacterId = TEXT("character-1");
  const FString WriterToken = TEXT("writer-token-1");
  const FString EntryIdA = TEXT("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
  const FString EntryIdB = TEXT("BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB");
  const FString CreditEntryId = TEXT("CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC");
  const FString LootReason = TEXT("Currency.Reason.Loot");
  const FString SellReason = TEXT("Currency.Reason.Merchant.Sell");
  const FString OwnedItemId = TEXT("11111111-1111-1111-1111-111111111111");
  const FString PurchasedItemId = TEXT("22222222-2222-2222-2222-222222222222");
  const FString LaterItemId = TEXT("33333333-3333-3333-3333-333333333333");
  constexpr int64 SeededInventorySeq = 4;
  // Long enough for a request the game thread would send to reach the server, so its absence
  // means it was held. Pumped at the fake server's 10 Hz.
  constexpr double QuietWindowSeconds = 0.5;
  // Far inside StepTimeout, so the sidecar's reconnect lands while a test waits for it.
  constexpr uint32 ReconnectDelayForTestMs = 200;
  constexpr uint16 ServiceRestartCloseCode = 1012;

  const TCHAR *const FlushEvent = TEXT("realm:characters:items:flush");
  const TCHAR *const LoadEvent = TEXT("realm:characters:items:load");
  const TCHAR *const TradeUsedSeqReply =
    TEXT(R"({"error":"batch seq was used by a trade","committedSeq":5,"rebaseRequired":true})");
  const TCHAR *const FinalRefusalReply =
    TEXT(R"({"error":"balance below zero","committedSeq":4,"final":true})");

  FRedwoodCurrencyEntry MakeEntry(const FString &Id, int64 Delta, int64 BalanceAfter, const FString &Reason) {
    FRedwoodCurrencyEntry Entry;
    Entry.Id = Id;
    Entry.Delta = Delta;
    Entry.BalanceAfter = BalanceAfter;
    Entry.Reason = Reason;
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

  FString MakeLoadReply(const TCHAR *Balance) {
    return FString::Printf(
      TEXT(R"({"error":null,"items":[],"currency":{"amount":"%s","grants":[]}})"), Balance
    );
  }

  void PumpFor(double Seconds) {
    const double End = FPlatformTime::Seconds() + Seconds;
    while (FPlatformTime::Seconds() < End) {
      FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
      FPlatformProcess::Sleep(PumpIntervalSeconds);
    }
  }

  // RFC 6455 section 5.2: the 16-bit and 64-bit length markers, and the client's mask key.
  constexpr uint8 SixteenBitLengthMarker = 126;
  constexpr uint8 SixtyFourBitLengthMarker = 127;
  constexpr int32 MaskKeyLength = 4;
  constexpr uint8 TextOpcode = 0x1;

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
    bool bShutDown = false;
    // Client bytes read but not yet parsed: the client can send two frames in one TCP segment,
    // and the fake server's own reader keeps only the first.
    TArray<uint8> PendingBytes;

    explicit FSidecarHarness(FAutomationTestBase &InTest) : Test(InTest) {
      GameInstance = NewObject<UGameInstance>(GEngine);
      GameInstance->InitializeStandalone();
      World = GameInstance->GetWorld();
      World->InitializeActorsForPlay(FURL());
      Subsystem = GameInstance->GetSubsystem<URedwoodServerGameSubsystem>();
      if (Subsystem) {
        TSharedPtr<FSocketIONative> &Sidecar = FAccess::Sidecar(*Subsystem);
        Sidecar = ISocketIOClientModule::Get().NewValidNativePointer();
        // Set before Connect(), which applies it.
        Sidecar->ReconnectionDelay = ReconnectDelayForTestMs;
        SidecarCopy = Sidecar;
      }
    }

    bool Connect() {
      if (!Test.TestNotNull(TEXT("The subsystem exists"), Subsystem) ||
          !Test.TestTrue(TEXT("The fake sidecar listens"), Server->Listen())) {
        return false;
      }
      SidecarCopy->Connect(FString::Printf(TEXT("ws://127.0.0.1:%d"), Server->Port));
      return AcceptAndWait();
    }

    // Accepts the client's (re)connect and waits until the game thread has seen it.
    bool AcceptAndWait() {
      PendingBytes.Reset();
      TSharedPtr<FSocketIONative> Socket = SidecarCopy;
      return Test.TestTrue(TEXT("The sidecar opens a session"), Server->AcceptSession()) &&
        Test.TestTrue(
          TEXT("The sidecar connects"),
          PumpGameThreadUntil([Socket]() { return Socket->bIsConnected; })
        );
    }

    // A character component on its own actor, hydrated as the login leg would leave it.
    URedwoodCharacterComponent *MakeComponent(
      ARedwoodCurrencyFlushTestActor *&OutActor, const FString &InCharacterId = CharacterId
    ) {
      OutActor = World->SpawnActor<ARedwoodCurrencyFlushTestActor>();
      URedwoodCharacterComponent *Component = NewObject<URedwoodCharacterComponent>(OutActor);
      Component->bUseItems = true;
      Component->RedwoodCharacterId = InCharacterId;
      Component->SeedWriterToken(WriterToken);
      Component->SeedItemSeqFromCharacter(SeededInventorySeq);
      return Component;
    }

    // Takes one complete client text frame off PendingBytes, if there is one.
    bool TryTakeTextFrame(FString &OutText) {
      while (PendingBytes.Num() >= 2) {
        const uint8 Opcode = PendingBytes[0] & 0x0F;
        const bool bMasked = (PendingBytes[1] & 0x80) != 0;
        int32 Length = PendingBytes[1] & 0x7F;
        int32 Offset = 2;
        if (Length == SixteenBitLengthMarker) {
          if (PendingBytes.Num() < 4) {
            return false;
          }
          Length = (PendingBytes[2] << 8) | PendingBytes[3];
          Offset = 4;
        } else if (Length == SixtyFourBitLengthMarker) {
          // No request here comes near 64 KiB; a frame that claims to is a broken read.
          return false;
        }
        const int32 PayloadStart = Offset + (bMasked ? MaskKeyLength : 0);
        if (PendingBytes.Num() < PayloadStart + Length) {
          return false;
        }
        TArray<uint8> Payload;
        Payload.Reserve(Length);
        for (int32 Index = 0; Index < Length; ++Index) {
          const uint8 Byte = PendingBytes[PayloadStart + Index];
          Payload.Add(bMasked ? Byte ^ PendingBytes[Offset + Index % MaskKeyLength] : Byte);
        }
        PendingBytes.RemoveAt(0, PayloadStart + Length);
        if (Opcode != TextOpcode) {
          continue;
        }
        const FUTF8ToTCHAR Converted(
          reinterpret_cast<const UTF8CHAR *>(Payload.GetData()), Payload.Num()
        );
        OutText = FString(Converted.Length(), Converted.Get());
        return true;
      }
      return false;
    }

    // Reads the next client text frame. The game thread runs meanwhile: a request sent from a
    // reply callback goes out only once the game thread has run that callback.
    bool ReadTextFrame(FString &OutText) {
      const double Deadline = FPlatformTime::Seconds() + StepTimeout.GetTotalSeconds();
      while (true) {
        if (TryTakeTextFrame(OutText)) {
          return true;
        }
        uint32 Size = 0;
        if (Server->Connection && Server->Connection->HasPendingData(Size) && Size > 0) {
          TArray<uint8> Buffer;
          Buffer.SetNumUninitialized(Size);
          int32 Read = 0;
          if (Server->Connection->Recv(Buffer.GetData(), Buffer.Num(), Read) && Read > 0) {
            PendingBytes.Append(Buffer.GetData(), Read);
          }
          continue;
        }
        if (FPlatformTime::Seconds() > Deadline) {
          return false;
        }
        FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        FPlatformProcess::Sleep(PumpIntervalSeconds);
      }
    }

    bool ReadRequest(const TCHAR *ExpectedEvent, FRequest &OutRequest) {
      FString Text;
      if (!Test.TestTrue(TEXT("A request arrives"), ReadTextFrame(Text))) {
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
      return PendingBytes.Num() == 0 && !Server->HasClientData();
    }

    bool PumpUntil(TFunctionRef<bool()> Done) {
      return PumpGameThreadUntil(Done);
    }

    void ShutDown() {
      if (!bShutDown && GameInstance) {
        bShutDown = true;
        GameInstance->Shutdown();
      }
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
      ShutDown();
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

  // Starts a rebase reload: one entry goes out, a trade has used its seq, and the reload request
  // is read (and left unanswered).
  bool StartRebaseReload(
    FSidecarHarness &Harness,
    URedwoodCharacterComponent *Component,
    TFunction<void()> OnAckSettled,
    FRequest &OutLoad
  ) {
    Component->QueueCurrency({MakeEntry(EntryIdA, 500, 1500, LootReason)}, {});
    Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component, MoveTemp(OnAckSettled));
    FRequest Flush;
    return Harness.ReadRequest(FlushEvent, Flush) && Harness.Answer(Flush, TradeUsedSeqReply) &&
      Harness.ReadRequest(LoadEvent, OutLoad);
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
  Component->QueueCurrency({MakeEntry(EntryIdA, 500, 1500, LootReason)}, {});

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
  Component->QueueCurrency({MakeEntry(EntryIdA, -250, 750, LootReason)}, {});

  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component);
  FRequest First;
  if (!Harness.ReadRequest(FlushEvent, First)) {
    return false;
  }
  TestEqual(TEXT("The first attempt carries the row"), CountArray(First.Payload, TEXT("upserts")), 1);
  TestFalse(TEXT("The first attempt is not a probe"), First.Payload->HasField(TEXT("probe")));
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
  bool bProbe = false;
  TestTrue(
    TEXT("The re-send says it is a probe"),
    Resend.Payload->TryGetBoolField(TEXT("probe"), bProbe) && bProbe
  );

  // The first attempt had committed: the backend acks the replay.
  Harness.Answer(Resend, TEXT(R"({"error":null,"committedSeq":5})"));
  Harness.PumpUntil([Component]() { return !Component->IsItemFlushInFlight(); });
  TestEqual(TEXT("The entry is saved"), Component->GetPendingCurrencyEntries().Num(), 0);
  TestTrue(TEXT("The owned row stays dirty"), Component->GetDirtyItemGenerations().Contains(OwnedItemId));
  TestTrue(TEXT("The later row stays dirty"), Component->GetDirtyItemGenerations().Contains(LaterItemId));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarFinalRefusalFreezesTest,
  "Redwood.Currency.Sidecar.FinalRefusalFreezesCharacter",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A refused purchase freezes the character: nothing is repaired, nothing more leaves for it from
// any component, no currency is taken, the game hears it once, and a detached pawn's barrier
// still releases.
bool FRedwoodCurrencySidecarFinalRefusalFreezesTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  AddExpectedError(TEXT("Failed to flush items"), EAutomationExpectedErrorFlags::Contains, 0);
  AddExpectedError(TEXT("dropped currency entry"), EAutomationExpectedErrorFlags::Contains, 0);
  AddExpectedError(TEXT("is frozen after a final refusal"), EAutomationExpectedErrorFlags::Contains, 0);
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Component = Harness.MakeComponent(Actor);
  int32 RefusalCount = 0;
  FString RefusedCharacterId;
  Component->OnItemFlushRefusedFinal.AddLambda(
    [&RefusalCount, &RefusedCharacterId](const FString &InCharacterId, const FString &) {
      ++RefusalCount;
      RefusedCharacterId = InCharacterId;
    }
  );

  // The game buys an item it cannot pay for.
  Actor->Items = {MakeRecord(OwnedItemId), MakeRecord(PurchasedItemId)};
  Component->MarkItemsDirty({PurchasedItemId});
  Component->QueueCurrency({MakeEntry(EntryIdA, -2000, -1000, LootReason)}, {});

  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component);
  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }
  Harness.Answer(Flush, FinalRefusalReply);
  if (!TestTrue(
        TEXT("The component freezes"),
        Harness.PumpUntil([Component]() { return Component->IsFrozenAfterFinalRefusal(); })
      )) {
    return false;
  }

  TestEqual(TEXT("The game hears it once"), RefusalCount, 1);
  TestEqual(TEXT("It names the character"), RefusedCharacterId, CharacterId);
  TestTrue(TEXT("The character is frozen"), Harness.Subsystem->IsCharacterFrozen(CharacterId));
  TestEqual(TEXT("Nothing is restored: the records stay as the game has them"), Actor->Items.Num(), 2);
  TestFalse(TEXT("Nothing staged is left"), Component->IsItemsDirty());
  TestNull(TEXT("No reload runs"), FAccess::Reload(*Harness.Subsystem, CharacterId));
  TestFalse(
    TEXT("New currency is refused"),
    Component->QueueCurrency({MakeEntry(EntryIdA, 10, 10, LootReason)}, {})
  );

  // The game touches the refused item again: no flush leaves.
  Component->MarkItemsDirty({PurchasedItemId});
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component);
  TestTrue(TEXT("No flush leaves for the frozen component"), Harness.IsQuiet());

  // Another component of the same character: it takes no currency and sends nothing either.
  ARedwoodCurrencyFlushTestActor *OtherActor = nullptr;
  URedwoodCharacterComponent *OtherComponent = Harness.MakeComponent(OtherActor);
  TestFalse(
    TEXT("The character's other component refuses currency"),
    OtherComponent->QueueCurrency({MakeEntry(EntryIdA, 10, 10, LootReason)}, {})
  );
  OtherActor->Items = {MakeRecord(LaterItemId)};
  OtherComponent->MarkItemsDirty({LaterItemId});
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, OtherComponent);
  TestTrue(TEXT("No flush leaves for the character's other component"), Harness.IsQuiet());
  TestFalse(TEXT("Its staged row is dropped"), OtherComponent->IsItemsDirty());

  // A detached pawn's flush settles at once, with nothing left to save.
  Component->MarkItemsDirty({PurchasedItemId});
  int32 SettleCount = 0;
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component, [&SettleCount]() { ++SettleCount; });
  TestEqual(TEXT("The detached barrier releases"), SettleCount, 1);
  TestTrue(TEXT("Still nothing leaves"), Harness.IsQuiet());
  TestEqual(TEXT("The game heard it only once"), RefusalCount, 1);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarSaleCreditNeverLeavesTest,
  "Redwood.Currency.Sidecar.RefusedBuySaleCreditNeverLeaves",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A buy is refused for good while the player sells the bought item. The sale's credit must never
// leave: the item it sells was never saved, so the credit would commit from nothing.
bool FRedwoodCurrencySidecarSaleCreditNeverLeavesTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  AddExpectedError(TEXT("Failed to flush items"), EAutomationExpectedErrorFlags::Contains, 0);
  AddExpectedError(TEXT("dropped currency entry AAAA"), EAutomationExpectedErrorFlags::Contains, 1);
  // The credit is dropped, and logged so staff can see it never committed.
  AddExpectedErrorPlain(
    TEXT("dropped currency entry CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC delta 25 reason Currency.Reason.Merchant.Sell"),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Component = Harness.MakeComponent(Actor);

  Actor->Items = {MakeRecord(PurchasedItemId)};
  Component->MarkItemsDirty({PurchasedItemId});
  Component->QueueCurrency({MakeEntry(EntryIdA, -2000, -1000, LootReason)}, {});
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component);
  FRequest Buy;
  if (!Harness.ReadRequest(FlushEvent, Buy)) {
    return false;
  }

  // While the buy is in flight, the player sells the item.
  Actor->Items.Reset();
  Component->MarkItemsDeleted({PurchasedItemId});
  TestTrue(
    TEXT("The sale is staged"),
    Component->QueueCurrency({MakeEntry(CreditEntryId, 25, -975, SellReason)}, {})
  );

  Harness.Answer(Buy, FinalRefusalReply);
  Harness.PumpUntil([Component]() { return Component->IsFrozenAfterFinalRefusal(); });
  TestEqual(TEXT("The credit is dropped"), Component->GetPendingCurrencyEntries().Num(), 0);
  TestFalse(TEXT("The sale's delete is dropped"), Component->IsItemsDirty());

  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component);
  TestTrue(TEXT("No credit ever leaves"), Harness.IsQuiet());
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarSettleWaitsForReloadTest,
  "Redwood.Currency.Sidecar.SettleWaitsForReload",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A detached pawn's flush that needs a rebase settles only once the re-chained entry is saved, so
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

  int32 SettleCount = 0;
  FRequest Load;
  if (!StartRebaseReload(Harness, Component, [&SettleCount]() { ++SettleCount; }, Load)) {
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

  FRequest Load;
  if (!StartRebaseReload(Harness, CurrencyComponent, nullptr, Load)) {
    return false;
  }

  ItemActor->Items = {MakeRecord(LaterItemId)};
  ItemComponent->MarkItemsDirty({LaterItemId});
  ItemComponent->SeedItemSeqFromCharacter(5);
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, ItemComponent);
  TestTrue(TEXT("The other component of the character is held"), Harness.IsQuiet());

  // Both held components flush when the reload lands: the re-chained entry and the held row.
  Harness.Answer(Load, MakeLoadReply(TEXT("2000")));
  bool bRowSent = false;
  bool bEntrySent = false;
  for (int32 Index = 0; Index < 2; ++Index) {
    FRequest Resumed;
    if (!Harness.ReadRequest(FlushEvent, Resumed)) {
      break;
    }
    bRowSent |= CountArray(Resumed.Payload, TEXT("upserts")) == 1;
    bEntrySent |= FirstBalanceAfter(Resumed.Payload) == TEXT("2500");
  }
  TestTrue(TEXT("The held row goes out after the reload"), bRowSent);
  TestTrue(TEXT("The re-chained entry goes out after the reload"), bEntrySent);
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
  FRequest FirstLoad;
  if (!StartRebaseReload(Harness, Component, nullptr, FirstLoad)) {
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

// The sidecar restarts during a reload. Retries while it is down spend no attempt, and the real
// connect callback sends the reload when the sidecar is back.
bool FRedwoodCurrencySidecarDownWaitsTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  AddExpectedError(TEXT("Failed to flush items"), EAutomationExpectedErrorFlags::Contains, 0);
  AddExpectedError(TEXT("Currency reload failed"), EAutomationExpectedErrorFlags::Contains, 0);
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  FAccess::BindConnectedCallback(*Harness.Subsystem);
  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Component = Harness.MakeComponent(Actor);
  FRequest FirstLoad;
  if (!StartRebaseReload(Harness, Component, nullptr, FirstLoad)) {
    return false;
  }

  TSharedPtr<FSocketIONative> Socket = Harness.SidecarCopy;
  if (!TestTrue(TEXT("The sidecar restarts"), Harness.Server->CloseWithCode(ServiceRestartCloseCode)) ||
      !TestTrue(TEXT("The socket sees it"), Harness.PumpUntil([Socket]() { return !Socket->bIsConnected; }))) {
    return false;
  }

  FAccess::TimeOutReload(*Harness.Subsystem, CharacterId);
  for (int32 Retry = 0; Retry < URedwoodServerGameSubsystem::CurrencyReloadMaxAttempts * 2; ++Retry) {
    FAccess::SendReloadNow(*Harness.Subsystem, CharacterId);
  }
  const FRedwoodCurrencyReload *Waiting = FAccess::Reload(*Harness.Subsystem, CharacterId);
  if (TestNotNull(TEXT("The reload still runs"), Waiting)) {
    TestTrue(TEXT("It waits for the sidecar"), Waiting->bWaitingForSidecar);
    TestEqual(TEXT("Waiting spends no attempt"), Waiting->Attempts, 1);
  }

  if (!Harness.AcceptAndWait()) {
    return false;
  }
  FRequest SecondLoad;
  if (!Harness.ReadRequest(LoadEvent, SecondLoad)) {
    return false;
  }
  Harness.Answer(SecondLoad, MakeLoadReply(TEXT("2000")));
  TestTrue(
    TEXT("The reload lands after the reconnect"),
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

  int32 SettleCount = 0;
  FRequest Load;
  if (!StartRebaseReload(Harness, Component, [&SettleCount]() { ++SettleCount; }, Load)) {
    return false;
  }

  const FString FailedLoad =
    TEXT(R"({"error":"Item load request timed out.","items":[],"currency":{"amount":"0","grants":[]}})");
  for (int32 Attempt = 1; Attempt <= URedwoodServerGameSubsystem::CurrencyReloadMaxAttempts; ++Attempt) {
    if (Attempt > 1) {
      FAccess::SendReloadNow(*Harness.Subsystem, CharacterId);
      if (!Harness.ReadRequest(LoadEvent, Load)) {
        return false;
      }
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
  FRedwoodCurrencySidecarWriterFenceLostTest,
  "Redwood.Currency.Sidecar.WriterFenceLostGivesUpAtOnce",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A "not bound" reply is retried, but the realm's writerFenceLost flag (another session owns the
// character) ends the reload on that reply.
bool FRedwoodCurrencySidecarWriterFenceLostTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  AddExpectedError(TEXT("Failed to flush items"), EAutomationExpectedErrorFlags::Contains, 0);
  AddExpectedError(TEXT("Currency reload failed"), EAutomationExpectedErrorFlags::Contains, 0);
  AddExpectedErrorPlain(TEXT("gave up (this session no longer owns the character)"), EAutomationExpectedErrorFlags::Contains, 1);
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Component = Harness.MakeComponent(Actor);

  int32 SettleCount = 0;
  FRequest Load;
  if (!StartRebaseReload(Harness, Component, [&SettleCount]() { ++SettleCount; }, Load)) {
    return false;
  }

  // The binding is missing for now (a sidecar restart): retried.
  Harness.Answer(
    Load,
    TEXT(R"({"error":"character not bound to this instance","items":[],"currency":{"amount":"0","grants":[]}})")
  );
  Harness.PumpUntil([&Harness]() {
    const FRedwoodCurrencyReload *Reload = FAccess::Reload(*Harness.Subsystem, CharacterId);
    return !Reload || Reload->Generation == 0;
  });
  TestNotNull(TEXT("A plain not-bound reply is retried"), FAccess::Reload(*Harness.Subsystem, CharacterId));
  TestEqual(TEXT("No settle yet"), SettleCount, 0);

  FAccess::SendReloadNow(*Harness.Subsystem, CharacterId);
  if (!Harness.ReadRequest(LoadEvent, Load)) {
    return false;
  }
  Harness.Answer(
    Load,
    TEXT(R"({"error":"character not bound to this instance","writerFenceLost":true,"items":[],"currency":{"amount":"0","grants":[]}})")
  );
  TestTrue(TEXT("The held attempt settles at once"), Harness.PumpUntil([&SettleCount]() { return SettleCount > 0; }));
  TestNull(TEXT("No retry is scheduled"), FAccess::Reload(*Harness.Subsystem, CharacterId));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarShutdownTest,
  "Redwood.Currency.Sidecar.ShutdownSettlesHeldFlush",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A server that shuts down during a reload does not leave a held barrier waiting on it.
bool FRedwoodCurrencySidecarShutdownTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  AddExpectedError(TEXT("Failed to flush items"), EAutomationExpectedErrorFlags::Contains, 0);
  AddExpectedErrorPlain(TEXT("gave up (the server is shutting down)"), EAutomationExpectedErrorFlags::Contains, 1);
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Component = Harness.MakeComponent(Actor);

  int32 SettleCount = 0;
  FRequest Load;
  if (!StartRebaseReload(Harness, Component, [&SettleCount]() { ++SettleCount; }, Load)) {
    return false;
  }
  TestEqual(TEXT("Held while the reload runs"), SettleCount, 0);

  Harness.ShutDown();
  TestEqual(TEXT("Shutdown settles the held attempt"), SettleCount, 1);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarUnhydratedTest,
  "Redwood.Currency.Sidecar.UnhydratedComponentNeverFlushes",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A component with no character id sends nothing and joins no hold or reload.
bool FRedwoodCurrencySidecarUnhydratedTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  AddExpectedError(TEXT("has no character id"), EAutomationExpectedErrorFlags::Contains, 1);
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Component = Harness.MakeComponent(Actor, FString());
  Component->QueueCurrency({MakeEntry(EntryIdA, 500, 1500, LootReason)}, {});

  int32 SettleCount = 0;
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component, [&SettleCount]() { ++SettleCount; });
  TestEqual(TEXT("The attempt settles at once"), SettleCount, 1);
  TestTrue(TEXT("Nothing leaves"), Harness.IsQuiet());
  TestNull(TEXT("No reload under an empty id"), FAccess::Reload(*Harness.Subsystem, FString()));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarProbeNotAppliedTest,
  "Redwood.Currency.Sidecar.ProbeNotAppliedSendsNormalBatch",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// The backend says a probed batch never committed, and consumes the probed seq with an empty
// commit: the entry is pending again, unchanged, and the next flush is a normal batch AFTER that
// fence that carries it WITH the current rows.
bool FRedwoodCurrencySidecarProbeNotAppliedTest::RunTest(const FString &Parameters) {
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
  Component->QueueCurrency({MakeEntry(EntryIdA, -250, 750, LootReason)}, {});

  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component);
  FRequest First;
  if (!Harness.ReadRequest(FlushEvent, First)) {
    return false;
  }
  Harness.Answer(First, TEXT(R"({"error":"Item flush request timed out."})"));
  Harness.PumpUntil([Component]() { return !Component->IsItemFlushInFlight(); });

  Component->MarkItemsDirty({LaterItemId});
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component);
  FRequest Probe;
  if (!Harness.ReadRequest(FlushEvent, Probe)) {
    return false;
  }
  TestTrue(TEXT("The re-send is a probe"), Probe.Payload->HasField(TEXT("probe")));
  TestEqual(TEXT("A probe carries no rows"), CountArray(Probe.Payload, TEXT("upserts")), 0);

  TestEqual(TEXT("The probe goes out under the unconfirmed seq"), Probe.Payload->GetNumberField(TEXT("batchSeq")), 5.0);
  Harness.Answer(Probe, TEXT(R"({"error":"batch was never applied","committedSeq":5,"notApplied":true})"));
  Harness.PumpUntil([Component]() { return !Component->IsItemFlushInFlight(); });
  const TArray<FRedwoodCurrencyEntry> &Pending = Component->GetPendingCurrencyEntries();
  if (TestEqual(TEXT("notApplied drops no entry"), Pending.Num(), 1)) {
    TestEqual(TEXT("The entry is unchanged"), Pending[0].BalanceAfter, (int64)750);
  }

  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component);
  FRequest Normal;
  if (!Harness.ReadRequest(FlushEvent, Normal)) {
    return false;
  }
  TestFalse(TEXT("The next batch is not a probe"), Normal.Payload->HasField(TEXT("probe")));
  TestEqual(TEXT("It goes out after the consumed seq"), Normal.Payload->GetNumberField(TEXT("batchSeq")), 6.0);
  TestEqual(TEXT("It carries both dirty rows"), CountArray(Normal.Payload, TEXT("upserts")), 2);
  TestEqual(TEXT("It carries the entry"), CountArray(Normal.Payload, TEXT("currency")), 1);
  TestEqual(TEXT("The entry keeps its balance"), FirstBalanceAfter(Normal.Payload), FString(TEXT("750")));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarFreezeWaitsForReplyTest,
  "Redwood.Currency.Sidecar.FreezeWaitsForInFlightReply",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// The character freezes while another of its components has a batch in flight. That batch's
// entries are not logged as dropped; its success reply saves them, and only then does the
// component freeze. Any error log for them fails this test, since none is expected.
bool FRedwoodCurrencySidecarFreezeWaitsForReplyTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  AddExpectedError(TEXT("Failed to flush items"), EAutomationExpectedErrorFlags::Contains, 0);
  AddExpectedErrorPlain(
    TEXT("dropped currency entry AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA delta -2000 reason Currency.Reason.Loot, NOT saved"),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyFlushTestActor *SavingActor = nullptr;
  URedwoodCharacterComponent *SavingComponent = Harness.MakeComponent(SavingActor);
  ARedwoodCurrencyFlushTestActor *RefusedActor = nullptr;
  URedwoodCharacterComponent *RefusedComponent = Harness.MakeComponent(RefusedActor);

  SavingComponent->QueueCurrency({MakeEntry(EntryIdB, 100, 1100, LootReason)}, {});
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, SavingComponent);
  FRequest Saving;
  if (!Harness.ReadRequest(FlushEvent, Saving)) {
    return false;
  }

  RefusedComponent->QueueCurrency({MakeEntry(EntryIdA, -2000, -1000, LootReason)}, {});
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, RefusedComponent);
  FRequest Refused;
  if (!Harness.ReadRequest(FlushEvent, Refused)) {
    return false;
  }
  Harness.Answer(Refused, FinalRefusalReply);
  if (!TestTrue(
        TEXT("The character freezes"),
        Harness.PumpUntil([RefusedComponent]() { return RefusedComponent->IsFrozenAfterFinalRefusal(); })
      )) {
    return false;
  }

  // A detached flush of the saving component reaches the freeze while its batch is in flight.
  int32 SettleCount = 0;
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, SavingComponent, [&SettleCount]() { ++SettleCount; });
  TestEqual(TEXT("The attempt settles"), SettleCount, 1);
  TestEqual(TEXT("The in-flight entry is not dropped yet"), SavingComponent->GetPendingCurrencyEntries().Num(), 1);

  Harness.Answer(Saving, TEXT(R"({"error":null,"committedSeq":5})"));
  TestTrue(
    TEXT("The component freezes after its reply"),
    Harness.PumpUntil([SavingComponent]() { return SavingComponent->IsFrozenAfterFinalRefusal(); })
  );
  TestEqual(TEXT("The saved entry left the queue as saved"), SavingComponent->GetPendingCurrencyEntries().Num(), 0);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarFlushWriterFenceLostTest,
  "Redwood.Currency.Sidecar.FlushWriterFenceLostStopsSending",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A flush reply says another session owns the character. This component sends nothing more, not
// even a probe, drops nothing, and every barrier settles.
bool FRedwoodCurrencySidecarFlushWriterFenceLostTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  AddExpectedError(TEXT("Failed to flush items"), EAutomationExpectedErrorFlags::Contains, 0);
  AddExpectedErrorPlain(
    TEXT("lost its writer fence (another session owns it); refused entry BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  // The kept entry is logged once, the way a freeze logs it, for staff.
  AddExpectedErrorPlain(
    TEXT("Writer fence lost: character character-1 left unsent currency entry AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA delta 500 reason Currency.Reason.Loot, NOT saved. A re-grant must reuse entry id AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Component = Harness.MakeComponent(Actor);
  Component->QueueCurrency({MakeEntry(EntryIdA, 500, 1500, LootReason)}, {});
  int32 FenceLostCount = 0;
  FString FenceLostCharacterId;
  Component->OnWriterFenceLost.AddLambda([&FenceLostCount, &FenceLostCharacterId](const FString &InCharacterId) {
    ++FenceLostCount;
    FenceLostCharacterId = InCharacterId;
  });

  int32 SettleCount = 0;
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component, [&SettleCount]() { ++SettleCount; });
  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }
  Harness.Answer(
    Flush,
    TEXT(R"({"error":"character is owned by another session","committedSeq":4,"writerFenceLost":true})")
  );
  TestTrue(TEXT("The barrier settles"), Harness.PumpUntil([&SettleCount]() { return SettleCount > 0; }));
  TestTrue(TEXT("The session is over for this component"), Component->IsWriterFenceLost());

  // Its batch is now unconfirmed, which would normally make the next flush a probe.
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component, [&SettleCount]() { ++SettleCount; });
  TestEqual(TEXT("A later barrier settles at once"), SettleCount, 2);
  TestTrue(TEXT("No further frame leaves"), Harness.IsQuiet());
  TestEqual(TEXT("Nothing is dropped"), Component->GetPendingCurrencyEntries().Num(), 1);

  // New currency is refused, not silently held: it would never be sent.
  TestFalse(
    TEXT("New currency is refused"),
    Component->QueueCurrency({MakeEntry(EntryIdB, 100, 1600, LootReason)}, {})
  );
  TestEqual(TEXT("Nothing new is queued"), Component->GetPendingCurrencyEntries().Num(), 1);
  TestEqual(TEXT("The game hears it once"), FenceLostCount, 1);
  TestEqual(TEXT("It names the character"), FenceLostCharacterId, CharacterId);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarAuthLoadThawsTest,
  "Redwood.Currency.Sidecar.AuthLoadThawsAndSeedsToken",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// The auth leg is the only fresh load from the database, so it is the only thaw, and only when the
// load is complete: a reply with no balance or no writer token leaves the character frozen and
// unset. The complete load's token then reaches the wire: the hydrated component carries it on its
// flush, and the committed grant key joins the snapshot's keys.
bool FRedwoodCurrencySidecarAuthLoadThawsTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  const FString AuthToken = TEXT("writer-token-from-auth");
  const FString LoadedGrantKey = TEXT("quest:Q1");
  const FString NewGrantKey = TEXT("quest:Q2");
  constexpr int64 LoadedSeq = 7;
  constexpr int64 LoadedBalance = 1500000;
  const FString CharacterFields = FString::Printf(
    TEXT(R"("character":{"id":"%s","playerId":"player-1","name":"Test Character","inventorySeq":%lld,)")
    TEXT(R"("createdAt":"2026-07-20T00:00:00.000Z","updatedAt":"2026-07-20T00:00:00.000Z"})"),
    *CharacterId,
    LoadedSeq
  );
  const FString ItemsField = FString::Printf(
    TEXT(R"("items":[{"id":"%s","parentId":null,"domain":"general","slot":0,"quantity":1,"templateId":"T","attributes":{}}])"),
    *OwnedItemId
  );
  const FString CurrencyField = FString::Printf(
    TEXT(R"("currency":{"amount":"%lld","grants":["%s"]})"), LoadedBalance, *LoadedGrantKey
  );
  const FString TokenField = FString::Printf(TEXT(R"("writerToken":"%s")"), *AuthToken);
  auto MakeReply = [&CharacterFields, &ItemsField](const FString &Currency, const FString &Token) {
    TArray<FString> Fields = {TEXT(R"("error":"")"), CharacterFields, ItemsField};
    if (!Currency.IsEmpty()) {
      Fields.Add(Currency);
    }
    if (!Token.IsEmpty()) {
      Fields.Add(Token);
    }
    TSharedPtr<FJsonObject> Reply;
    TSharedRef<TJsonReader<>> Reader =
      TJsonReaderFactory<>::Create(TEXT("{") + FString::Join(Fields, TEXT(",")) + TEXT("}"));
    FJsonSerializer::Deserialize(Reader, Reply);
    return Reply;
  };

  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  FAccess::FreezeCharacter(*Harness.Subsystem, CharacterId);

  // Incomplete loads: refused, and the freeze stands.
  URedwoodPlayerStateComponent *Refused = NewObject<URedwoodPlayerStateComponent>();
  FString Error;
  TestFalse(
    TEXT("A reply with no currency fails the load"),
    URedwoodGameModeComponent::ApplyPlayerAuthCharacter(
      Harness.Subsystem, Refused, MakeReply(FString(), TokenField), Error
    )
  );
  TestFalse(
    TEXT("A reply with no writer token fails the load"),
    URedwoodGameModeComponent::ApplyPlayerAuthCharacter(
      Harness.Subsystem, Refused, MakeReply(CurrencyField, FString()), Error
    )
  );
  TestTrue(TEXT("A failed load does not thaw"), Harness.Subsystem->IsCharacterFrozen(CharacterId));
  TestTrue(TEXT("A failed load sets no character"), Refused->RedwoodCharacter.Id.IsEmpty());

  // The complete load thaws.
  URedwoodPlayerStateComponent *Loaded = NewObject<URedwoodPlayerStateComponent>();
  TestTrue(
    TEXT("A complete reply loads"),
    URedwoodGameModeComponent::ApplyPlayerAuthCharacter(
      Harness.Subsystem, Loaded, MakeReply(CurrencyField, TokenField), Error
    )
  );
  TestFalse(TEXT("The complete load thaws"), Harness.Subsystem->IsCharacterFrozen(CharacterId));

  // A PlayerState that holds the loaded snapshot, and a component that hydrates from it. Copied
  // rather than set through SetRedwoodCharacter, which would also give the test PlayerState a net
  // id through the online interface.
  ARedwoodCurrencyLoadTestPlayerState *PlayerState =
    Harness.World->SpawnActor<ARedwoodCurrencyLoadTestPlayerState>();
  if (!TestNotNull(TEXT("The PlayerState spawns"), PlayerState)) {
    return false;
  }
  URedwoodPlayerStateComponent *PlayerStateComponent =
    NewObject<URedwoodPlayerStateComponent>(PlayerState);
  PlayerStateComponent->RedwoodCharacter = Loaded->RedwoodCharacter;

  URedwoodCharacterComponent *Component = NewObject<URedwoodCharacterComponent>(PlayerState);
  // Only the item channel: the other channels need data this test does not give.
  Component->bUseCharacterCreatorData = false;
  Component->bUseMetadata = false;
  Component->bUseEquippedInventory = false;
  Component->bUseNonequippedInventory = false;
  Component->bUseData = false;
  Component->bUseItems = true;
  // The handler that the PlayerState's OnRedwoodCharacterUpdated runs on a live component.
  Component->ProcessEvent(
    Component->FindFunctionChecked(TEXT("RedwoodPlayerStateCharacterUpdated")), nullptr
  );
  TestEqual(TEXT("The component hydrates the character"), Component->RedwoodCharacterId, CharacterId);
  TestEqual(TEXT("It takes the session's writer token"), Component->GetWriterToken(), AuthToken);
  TestEqual(TEXT("It takes the claim's seq"), Component->GetLastCommittedItemSeq(), LoadedSeq);
  TestEqual(TEXT("It takes the claim's rows"), PlayerState->Items.Num(), 1);
  TestFalse(TEXT("It is not frozen"), Component->IsFrozenAfterFinalRefusal());
  TestFalse(TEXT("A known balance needs no reload"), Component->IsCurrencyRebaseRequired());

  const FRedwoodCurrencyEntry Entry = MakeEntry(EntryIdA, 500, LoadedBalance + 500, LootReason);
  TestTrue(TEXT("The thawed character takes currency"), Component->QueueCurrency({Entry}, {NewGrantKey}));
  Harness.Subsystem->FlushItemsForCharacterComponent(PlayerStateComponent, Component);
  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }
  FString SentToken;
  Flush.Payload->TryGetStringField(TEXT("writerToken"), SentToken);
  TestEqual(TEXT("The flush carries the auth token"), SentToken, AuthToken);
  double SentSeq = 0.0;
  Flush.Payload->TryGetNumberField(TEXT("batchSeq"), SentSeq);
  TestEqual(TEXT("The flush continues the claim's seq"), static_cast<int64>(SentSeq), LoadedSeq + 1);

  Harness.Answer(
    Flush, FString::Printf(TEXT(R"({"error":null,"committedSeq":%lld})"), LoadedSeq + 1)
  );
  TestTrue(
    TEXT("The reply lands"),
    Harness.PumpUntil([Component]() { return !Component->IsItemFlushInFlight(); })
  );
  const FRedwoodCharacterBackend &Snapshot = PlayerStateComponent->RedwoodCharacter;
  TestEqual(TEXT("The snapshot balance follows the commit"), Snapshot.CurrencyAmount, LoadedBalance + 500);
  TestTrue(
    TEXT("The snapshot keeps the loaded key and adds the committed one"),
    Snapshot.GrantKeys == TArray<FString>{LoadedGrantKey, NewGrantKey}
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarHasGrantKeyTest,
  "Redwood.Currency.Sidecar.HasGrantKey",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// The check game code runs before it stages a one-time reward. A key counts as used when it is
// committed (on the PlayerState snapshot), staged, or in flight, and only in its exact case, as
// the backend compares it.
bool FRedwoodCurrencySidecarHasGrantKeyTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  const FString CommittedKey = TEXT("quest:Q1");
  const FString StagedKey = TEXT("quest:Q2");
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyLoadTestPlayerState *PlayerState =
    Harness.World->SpawnActor<ARedwoodCurrencyLoadTestPlayerState>();
  if (!TestNotNull(TEXT("The PlayerState spawns"), PlayerState)) {
    return false;
  }
  URedwoodPlayerStateComponent *PlayerStateComponent =
    NewObject<URedwoodPlayerStateComponent>(PlayerState);
  PlayerStateComponent->RedwoodCharacter.GrantKeys = {CommittedKey};
  URedwoodCharacterComponent *Component = NewObject<URedwoodCharacterComponent>(PlayerState);
  Component->bUseItems = true;
  Component->RedwoodCharacterId = CharacterId;
  Component->SeedItemSeqFromCharacter(SeededInventorySeq);

  TestTrue(TEXT("A committed key is used"), Component->HasGrantKey(CommittedKey));
  TestFalse(TEXT("The same key in another case is not"), Component->HasGrantKey(TEXT("quest:q1")));
  TestFalse(TEXT("An unknown key is not"), Component->HasGrantKey(StagedKey));

  TestTrue(TEXT("The key is staged"), Component->QueueCurrency({}, {StagedKey}));
  TestTrue(TEXT("A staged key is used"), Component->HasGrantKey(StagedKey));
  TestFalse(TEXT("A staged key in another case is not"), Component->HasGrantKey(TEXT("QUEST:Q2")));

  int64 BatchSeq = 0;
  TestTrue(TEXT("The batch is claimed"), Component->TryBeginItemFlush(BatchSeq));
  TestTrue(TEXT("A key in flight is used"), Component->HasGrantKey(StagedKey));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarComponentGoneCommitTest,
  "Redwood.Currency.Sidecar.ComponentGoneCommitReachesSnapshot",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// The pawn respawns while a quest reward's batch is in flight. The batch commits, but the
// component that sent it is gone. The snapshot must still take the commit's seq, balance and key,
// or the new component reuses the seq and the game stages the reward again, which the backend
// refuses for good.
bool FRedwoodCurrencySidecarComponentGoneCommitTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  const FString RewardKey = TEXT("quest:Q1");
  constexpr int64 SnapshotBalance = 1000;
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyLoadTestPlayerState *PlayerState =
    Harness.World->SpawnActor<ARedwoodCurrencyLoadTestPlayerState>();
  if (!TestNotNull(TEXT("The PlayerState spawns"), PlayerState)) {
    return false;
  }
  URedwoodPlayerStateComponent *PlayerStateComponent =
    NewObject<URedwoodPlayerStateComponent>(PlayerState);
  FRedwoodCharacterBackend &Snapshot = PlayerStateComponent->RedwoodCharacter;
  Snapshot.Id = CharacterId;
  Snapshot.InventorySeq = SeededInventorySeq;
  Snapshot.CurrencyAmount = SnapshotBalance;

  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Sender = Harness.MakeComponent(Actor);
  Sender->QueueCurrency(
    {MakeEntry(EntryIdA, 500, SnapshotBalance + 500, LootReason)}, {RewardKey}
  );
  int32 SettleCount = 0;
  Harness.Subsystem->FlushItemsForCharacterComponent(
    PlayerStateComponent, Sender, [&SettleCount]() { ++SettleCount; }
  );
  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }

  // The respawn: the sending component is destroyed before its reply arrives.
  Sender->MarkAsGarbage();
  Harness.Answer(
    Flush, FString::Printf(TEXT(R"({"error":null,"committedSeq":%lld})"), SeededInventorySeq + 1)
  );
  TestTrue(TEXT("The reply settles"), Harness.PumpUntil([&SettleCount]() { return SettleCount > 0; }));
  TestEqual(TEXT("The snapshot takes the committed seq"), Snapshot.InventorySeq, SeededInventorySeq + 1);
  TestEqual(TEXT("The snapshot takes the committed balance"), Snapshot.CurrencyAmount, SnapshotBalance + 500);
  TestFalse(TEXT("The balance is known"), Snapshot.bCurrencyAmountUnknown);

  // The respawned pawn's component sees the reward as paid.
  URedwoodCharacterComponent *Respawned = NewObject<URedwoodCharacterComponent>(PlayerState);
  TestTrue(TEXT("The new component sees the committed key"), Respawned->HasGrantKey(RewardKey));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarComponentGoneErrorTest,
  "Redwood.Currency.Sidecar.ComponentGoneErrorWritesNothing",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// An error reply for a component that is gone changes nothing on the snapshot.
bool FRedwoodCurrencySidecarComponentGoneErrorTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  // Plain, not AddExpectedError: that one reads its pattern as a regex, where the parentheses are a
  // group and the logged text no longer matches.
  AddExpectedErrorPlain(
    TEXT("Failed to flush items (component gone): concurrent flush"), EAutomationExpectedErrorFlags::Contains, 1
  );
  const FString RewardKey = TEXT("quest:Q1");
  constexpr int64 SnapshotBalance = 1000;
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  URedwoodPlayerStateComponent *PlayerStateComponent = NewObject<URedwoodPlayerStateComponent>();
  FRedwoodCharacterBackend &Snapshot = PlayerStateComponent->RedwoodCharacter;
  // The flush takes the character id from the snapshot when it has one to give.
  Snapshot.Id = CharacterId;
  Snapshot.InventorySeq = SeededInventorySeq;
  Snapshot.CurrencyAmount = SnapshotBalance;

  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Sender = Harness.MakeComponent(Actor);
  Sender->QueueCurrency(
    {MakeEntry(EntryIdA, 500, SnapshotBalance + 500, LootReason)}, {RewardKey}
  );
  int32 SettleCount = 0;
  Harness.Subsystem->FlushItemsForCharacterComponent(
    PlayerStateComponent, Sender, [&SettleCount]() { ++SettleCount; }
  );
  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }
  Sender->MarkAsGarbage();
  // committedSeq at the sent seq: on the success path this would write the seq and the balance.
  Harness.Answer(
    Flush,
    FString::Printf(TEXT(R"({"error":"concurrent flush","committedSeq":%lld})"), SeededInventorySeq + 1)
  );
  TestTrue(TEXT("The reply settles"), Harness.PumpUntil([&SettleCount]() { return SettleCount > 0; }));
  TestEqual(TEXT("The seq is unchanged"), Snapshot.InventorySeq, SeededInventorySeq);
  TestEqual(TEXT("The balance is unchanged"), Snapshot.CurrencyAmount, SnapshotBalance);
  TestEqual(TEXT("No key is added"), Snapshot.GrantKeys.Num(), 0);
  return true;
}

// FORK(hollowed-oath): the trade's currency, through the same fake sidecar. Each test names the
// ruling of URedwoodServerGameSubsystem::EmitItemsTrade it holds.
namespace RedwoodCurrencyTradeTest {
  using namespace RedwoodCurrencySidecarTest;

  const FString FromId = TEXT("character-from");
  const FString ToId = TEXT("character-to");
  const FString FromToken = TEXT("writer-token-from");
  const FString ToToken = TEXT("writer-token-to");
  const FString TradeId = TEXT("DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD");
  const FString PaidEntryId = TEXT("EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE");
  const FString ReceivedEntryId = TEXT("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF");
  const FString TradeReason = TEXT("Currency.Reason.Trade");
  const TCHAR *const TradeEvent = TEXT("realm:characters:items:trade");
  constexpr int64 FromSeq = 4;
  constexpr int64 FromBalance = 1000;
  constexpr int64 ToSeq = 9;
  constexpr int64 ToBalance = 200;
  constexpr int64 TradedAmount = 300;
  constexpr int64 LootAmount = 50;

  // A character on its own PlayerState, with the snapshot the trade chains from.
  struct FParty {
    ARedwoodCurrencyLoadTestPlayerState *PlayerState = nullptr;
    URedwoodPlayerStateComponent *PlayerStateComponent = nullptr;
    URedwoodCharacterComponent *Component = nullptr;
  };

  bool MakeParty(
    FSidecarHarness &Harness,
    const FString &Id,
    const FString &Token,
    int64 Seq,
    int64 Balance,
    FParty &OutParty
  ) {
    OutParty.PlayerState = Harness.World->SpawnActor<ARedwoodCurrencyLoadTestPlayerState>();
    if (!Harness.Test.TestNotNull(TEXT("The PlayerState spawns"), OutParty.PlayerState)) {
      return false;
    }
    OutParty.PlayerStateComponent = NewObject<URedwoodPlayerStateComponent>(OutParty.PlayerState);
    FRedwoodCharacterBackend &Snapshot = OutParty.PlayerStateComponent->RedwoodCharacter;
    Snapshot.Id = Id;
    Snapshot.InventorySeq = Seq;
    Snapshot.CurrencyAmount = Balance;
    Snapshot.WriterToken = Token;
    OutParty.Component = NewObject<URedwoodCharacterComponent>(OutParty.PlayerState);
    OutParty.Component->bUseItems = true;
    OutParty.Component->RedwoodCharacterId = Id;
    OutParty.Component->SeedWriterToken(Token);
    OutParty.Component->SeedItemSeqFromCharacter(Seq);
    return true;
  }

  // From pays To. BalanceAfter is left at zero: the trade chains it.
  TArray<TPair<FString, FRedwoodCurrencyEntry>> MakeTradeCurrency(
    int64 Paid, int64 Received, const FString &InFromId = FromId
  ) {
    return {
      TPair<FString, FRedwoodCurrencyEntry>(InFromId, MakeEntry(PaidEntryId, -Paid, 0, TradeReason)),
      TPair<FString, FRedwoodCurrencyEntry>(ToId, MakeEntry(ReceivedEntryId, Received, 0, TradeReason)),
    };
  }

  struct FTradeResult {
    int32 Calls = 0;
    FString Error;
    int64 FromCommittedSeq = 0;
    int64 ToCommittedSeq = 0;
  };

  TFunction<void(FString, int64, int64)> Capture(FTradeResult &Result) {
    return [&Result](FString Error, int64 FromCommittedSeq, int64 ToCommittedSeq) {
      ++Result.Calls;
      Result.Error = Error;
      Result.FromCommittedSeq = FromCommittedSeq;
      Result.ToCommittedSeq = ToCommittedSeq;
    };
  }

  FString CommittedReply(int64 FromCommittedSeq, int64 ToCommittedSeq) {
    return FString::Printf(
      TEXT(R"({"error":null,"fromCommittedSeq":%lld,"toCommittedSeq":%lld})"),
      FromCommittedSeq,
      ToCommittedSeq
    );
  }

  FString ErrorReply(const FString &Error) {
    return FString::Printf(
      TEXT(R"({"error":"%s","fromCommittedSeq":-1,"toCommittedSeq":-1})"), *Error
    );
  }

  // currency[Index].characterId, or a field of currency[Index].entry.
  FString TradeCurrencyField(const TSharedPtr<FJsonObject> &Payload, int32 Index, const TCHAR *Field) {
    const TArray<TSharedPtr<FJsonValue>> *Values = nullptr;
    const TSharedPtr<FJsonObject> *Side = nullptr;
    const TSharedPtr<FJsonObject> *Entry = nullptr;
    if (!Payload.IsValid() || !Payload->TryGetArrayField(TEXT("currency"), Values) ||
        !Values->IsValidIndex(Index) || !(*Values)[Index]->TryGetObject(Side)) {
      return FString();
    }
    if (FCString::Strcmp(Field, TEXT("characterId")) == 0) {
      return (*Side)->GetStringField(Field);
    }
    return (*Side)->TryGetObjectField(TEXT("entry"), Entry) ? (*Entry)->GetStringField(Field)
                                                            : FString();
  }

  int64 BatchSeqOf(const TSharedPtr<FJsonObject> &Payload) {
    return Payload.IsValid() ? static_cast<int64>(Payload->GetNumberField(TEXT("batchSeq"))) : -1;
  }

  // Sends the standard trade (From pays To) and reads its request.
  bool SendTrade(
    FSidecarHarness &Harness, FParty &From, FParty &To, FTradeResult &Result, FRequest &OutTrade
  ) {
    Harness.Subsystem->EmitItemsTrade(
      From.Component, To.Component, {}, MakeTradeCurrency(TradedAmount, TradedAmount), TradeId,
      Capture(Result)
    );
    return Harness.ReadRequest(TradeEvent, OutTrade);
  }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyTradeCommitTest,
  "Redwood.Currency.Trade.CommitAdoptsSeqAndBalance",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// The trade carries both tokens, the trade id and both chained entries. Its entries never enter a
// flush queue. While it runs, a flush of a trading character is held. On commit both sides adopt
// their fence and balance on the component and the snapshot, an entry staged meanwhile is
// re-chained from the new balance, and the held flush then goes out after the trade's seq.
bool FRedwoodCurrencyTradeCommitTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyTradeTest;
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  FParty From;
  FParty To;
  if (!MakeParty(Harness, FromId, FromToken, FromSeq, FromBalance, From) ||
      !MakeParty(Harness, ToId, ToToken, ToSeq, ToBalance, To)) {
    return false;
  }
  int32 RebasedCount = 0;
  int64 RebasedBalance = -1;
  From.Component->OnCurrencyRebased.AddLambda([&RebasedCount, &RebasedBalance](int64 Balance) {
    ++RebasedCount;
    RebasedBalance = Balance;
  });

  FTradeResult Result;
  FRequest Trade;
  if (!SendTrade(Harness, From, To, Result, Trade)) {
    return false;
  }
  const TSharedPtr<FJsonObject> &Payload = Trade.Payload;
  TestEqual(TEXT("fromCharacterId"), Payload->GetStringField(TEXT("fromCharacterId")), FromId);
  TestEqual(TEXT("toCharacterId"), Payload->GetStringField(TEXT("toCharacterId")), ToId);
  TestEqual(TEXT("fromWriterToken"), Payload->GetStringField(TEXT("fromWriterToken")), FromToken);
  TestEqual(TEXT("toWriterToken"), Payload->GetStringField(TEXT("toWriterToken")), ToToken);
  TestEqual(TEXT("tradeId"), Payload->GetStringField(TEXT("tradeId")), TradeId);
  TestEqual(TEXT("The payer's entry names the payer"), TradeCurrencyField(Payload, 0, TEXT("characterId")), FromId);
  TestEqual(TEXT("The payer's delta"), TradeCurrencyField(Payload, 0, TEXT("delta")), FString(TEXT("-300")));
  TestEqual(
    TEXT("The payer's balance is chained from the snapshot"),
    TradeCurrencyField(Payload, 0, TEXT("balanceAfter")),
    FString(TEXT("700"))
  );
  TestEqual(TEXT("The payee's entry names the payee"), TradeCurrencyField(Payload, 1, TEXT("characterId")), ToId);
  TestEqual(TEXT("The payee's delta"), TradeCurrencyField(Payload, 1, TEXT("delta")), FString(TEXT("300")));
  TestEqual(
    TEXT("The payee's balance is chained from the snapshot"),
    TradeCurrencyField(Payload, 1, TEXT("balanceAfter")),
    FString(TEXT("500"))
  );
  TestEqual(TEXT("The trade's entries are not in the flush queue"), From.Component->GetPendingCurrencyEntries().Num(), 0);

  // Loot during the trade, chained by a game that did not hold the offered amount back.
  From.Component->QueueCurrency(
    {MakeEntry(EntryIdA, LootAmount, FromBalance + LootAmount, LootReason)}, {}
  );
  int32 SettleCount = 0;
  Harness.Subsystem->FlushItemsForCharacterComponent(
    From.PlayerStateComponent, From.Component, [&SettleCount]() { ++SettleCount; }
  );
  TestTrue(TEXT("The flush waits for the trade"), Harness.IsQuiet());
  TestEqual(TEXT("The held flush has not settled"), SettleCount, 0);

  Harness.Answer(Trade, CommittedReply(FromSeq + 1, ToSeq + 1));
  TestTrue(TEXT("The trade settles"), Harness.PumpUntil([&Result]() { return Result.Calls > 0; }));
  TestTrue(TEXT("The trade committed"), Result.Error.IsEmpty());
  TestEqual(TEXT("The caller gets the payer's fence"), Result.FromCommittedSeq, FromSeq + 1);
  TestEqual(TEXT("The caller gets the payee's fence"), Result.ToCommittedSeq, ToSeq + 1);
  TestEqual(TEXT("The payer's lane takes the fence"), From.Component->GetLastCommittedItemSeq(), FromSeq + 1);
  TestEqual(TEXT("The payee's lane takes the fence"), To.Component->GetLastCommittedItemSeq(), ToSeq + 1);
  TestEqual(TEXT("The payer's snapshot takes the fence"), From.PlayerStateComponent->RedwoodCharacter.InventorySeq, FromSeq + 1);
  TestEqual(TEXT("The payee's snapshot takes the fence"), To.PlayerStateComponent->RedwoodCharacter.InventorySeq, ToSeq + 1);
  TestEqual(TEXT("The payer's snapshot takes the balance"), From.PlayerStateComponent->RedwoodCharacter.CurrencyAmount, FromBalance - TradedAmount);
  TestEqual(TEXT("The payee's snapshot takes the balance"), To.PlayerStateComponent->RedwoodCharacter.CurrencyAmount, ToBalance + TradedAmount);
  TestEqual(TEXT("The game is told the payer's balance once"), RebasedCount, 1);
  TestEqual(TEXT("The told balance is the committed one"), RebasedBalance, FromBalance - TradedAmount);

  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }
  TestEqual(TEXT("The held flush goes out after the trade's seq"), BatchSeqOf(Flush.Payload), FromSeq + 2);
  TestEqual(
    TEXT("The loot is re-chained from the committed balance"),
    FirstBalanceAfter(Flush.Payload),
    FString(TEXT("750"))
  );
  Harness.Answer(Flush, FString::Printf(TEXT(R"({"error":null,"committedSeq":%lld})"), FromSeq + 2));
  TestTrue(TEXT("The held flush settles"), Harness.PumpUntil([&SettleCount]() { return SettleCount > 0; }));
  TestEqual(TEXT("It settles once"), SettleCount, 1);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyTradeRefusalTest,
  "Redwood.Currency.Trade.RefusalAppliesNothing",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A trade error means NOT applied: no seq, balance or entry moves, nothing is dropped, and an entry
// staged during the trade is re-chained from the balance before it.
bool FRedwoodCurrencyTradeRefusalTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyTradeTest;
  AddExpectedErrorPlain(TEXT("Failed to trade items"), EAutomationExpectedErrorFlags::Contains, 1);
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  FParty From;
  FParty To;
  if (!MakeParty(Harness, FromId, FromToken, FromSeq, FromBalance, From) ||
      !MakeParty(Harness, ToId, ToToken, ToSeq, ToBalance, To)) {
    return false;
  }
  int64 RebasedBalance = -1;
  From.Component->OnCurrencyRebased.AddLambda([&RebasedBalance](int64 Balance) {
    RebasedBalance = Balance;
  });

  FTradeResult Result;
  FRequest Trade;
  if (!SendTrade(Harness, From, To, Result, Trade)) {
    return false;
  }
  // Loot during the trade, chained by a game that held the offered amount back.
  From.Component->QueueCurrency(
    {MakeEntry(EntryIdA, LootAmount, FromBalance - TradedAmount + LootAmount, LootReason)}, {}
  );
  Harness.Subsystem->FlushItemsForCharacterComponent(From.PlayerStateComponent, From.Component);
  TestTrue(TEXT("The flush waits for the trade"), Harness.IsQuiet());

  Harness.Answer(Trade, ErrorReply(TEXT("item 1 not owned by from-character")));
  TestTrue(TEXT("The trade settles"), Harness.PumpUntil([&Result]() { return Result.Calls > 0; }));
  TestFalse(TEXT("The caller gets the error"), Result.Error.IsEmpty());
  TestEqual(TEXT("No payer fence"), Result.FromCommittedSeq, static_cast<int64>(-1));
  TestEqual(TEXT("The payer's lane is unchanged"), From.Component->GetLastCommittedItemSeq(), FromSeq);
  TestEqual(TEXT("The payee's lane is unchanged"), To.Component->GetLastCommittedItemSeq(), ToSeq);
  TestEqual(TEXT("The payer's snapshot seq is unchanged"), From.PlayerStateComponent->RedwoodCharacter.InventorySeq, FromSeq);
  TestEqual(TEXT("The payer's snapshot balance is unchanged"), From.PlayerStateComponent->RedwoodCharacter.CurrencyAmount, FromBalance);
  TestEqual(TEXT("The payee's snapshot balance is unchanged"), To.PlayerStateComponent->RedwoodCharacter.CurrencyAmount, ToBalance);
  TestEqual(TEXT("The game is told the balance before the trade"), RebasedBalance, FromBalance);
  TestEqual(TEXT("The loot is kept"), From.Component->GetPendingCurrencyEntries().Num(), 1);

  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }
  TestEqual(TEXT("The held flush takes the seq the trade did not use"), BatchSeqOf(Flush.Payload), FromSeq + 1);
  TestEqual(
    TEXT("The loot is re-chained from the balance before the trade"),
    FirstBalanceAfter(Flush.Payload),
    FString(TEXT("1050"))
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyTradeTimeoutResendTest,
  "Redwood.Currency.Trade.TimeoutResendsWithLanesHeld",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A timed-out trade is not a refusal: the lanes stay held, the identical trade goes out again, and
// the backend's replay ack, which carries the current fences, settles it as committed.
bool FRedwoodCurrencyTradeTimeoutResendTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyTradeTest;
  AddExpectedErrorPlain(TEXT("Failed to trade items"), EAutomationExpectedErrorFlags::Contains, 1);
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  FParty From;
  FParty To;
  if (!MakeParty(Harness, FromId, FromToken, FromSeq, FromBalance, From) ||
      !MakeParty(Harness, ToId, ToToken, ToSeq, ToBalance, To)) {
    return false;
  }
  FTradeResult Result;
  FRequest First;
  if (!SendTrade(Harness, From, To, Result, First)) {
    return false;
  }
  Harness.Answer(First, ErrorReply(URedwoodServerGameSubsystem::TradeTimedOutError));
  To.Component->QueueCurrency({MakeEntry(EntryIdA, LootAmount, ToBalance + TradedAmount + LootAmount, LootReason)}, {});
  int32 SettleCount = 0;
  Harness.Subsystem->FlushItemsForCharacterComponent(
    To.PlayerStateComponent, To.Component, [&SettleCount]() { ++SettleCount; }
  );
  TestTrue(TEXT("Nothing goes out between the timeout and the resend"), Harness.IsQuiet());
  TestEqual(TEXT("The timed-out trade is not settled"), Result.Calls, 0);
  TestEqual(TEXT("The held flush has not settled"), SettleCount, 0);
  TestNotNull(TEXT("The trade is still pending"), FAccess::Trade(*Harness.Subsystem, TradeId));

  FAccess::ResendTradeNow(*Harness.Subsystem, TradeId);
  FRequest Second;
  if (!Harness.ReadRequest(TradeEvent, Second)) {
    return false;
  }
  TestEqual(TEXT("The resend keeps the trade id"), Second.Payload->GetStringField(TEXT("tradeId")), TradeId);
  for (int32 Index = 0; Index < 2; ++Index) {
    for (const TCHAR *Field : {TEXT("characterId"), TEXT("id"), TEXT("delta"), TEXT("balanceAfter")}) {
      TestEqual(
        FString::Printf(TEXT("The resend keeps currency[%d].%s"), Index, Field),
        TradeCurrencyField(Second.Payload, Index, Field),
        TradeCurrencyField(First.Payload, Index, Field)
      );
    }
  }

  // The replay ack: the first attempt had committed.
  Harness.Answer(Second, CommittedReply(FromSeq + 1, ToSeq + 1));
  TestTrue(TEXT("The trade settles"), Harness.PumpUntil([&Result]() { return Result.Calls > 0; }));
  TestTrue(TEXT("The replay settles it as committed"), Result.Error.IsEmpty());
  TestEqual(TEXT("The payee's lane takes the fence"), To.Component->GetLastCommittedItemSeq(), ToSeq + 1);
  TestEqual(TEXT("The payee's snapshot takes the balance"), To.PlayerStateComponent->RedwoodCharacter.CurrencyAmount, ToBalance + TradedAmount);

  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }
  TestEqual(TEXT("The held flush goes out after the trade's seq"), BatchSeqOf(Flush.Payload), ToSeq + 2);
  TestEqual(TEXT("The loot keeps its chain on the payee's new balance"), FirstBalanceAfter(Flush.Payload), FString(TEXT("550")));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyTradeLaterFenceTest,
  "Redwood.Currency.Trade.LaterFenceReloadsBalance",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A committed trade whose fence is past its own commit (a replay ack after something else
// committed) moves the seq, but its balance is not the last BalanceAfter: the snapshot marks it
// unknown and the component waits for a reload.
bool FRedwoodCurrencyTradeLaterFenceTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyTradeTest;
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  FParty From;
  FParty To;
  if (!MakeParty(Harness, FromId, FromToken, FromSeq, FromBalance, From) ||
      !MakeParty(Harness, ToId, ToToken, ToSeq, ToBalance, To)) {
    return false;
  }
  FTradeResult Result;
  FRequest Trade;
  if (!SendTrade(Harness, From, To, Result, Trade)) {
    return false;
  }
  Harness.Answer(Trade, CommittedReply(FromSeq + 2, ToSeq + 1));
  TestTrue(TEXT("The trade settles"), Harness.PumpUntil([&Result]() { return Result.Calls > 0; }));
  TestTrue(TEXT("It committed"), Result.Error.IsEmpty());
  TestEqual(TEXT("The payer's lane takes the later fence"), From.Component->GetLastCommittedItemSeq(), FromSeq + 2);
  TestEqual(TEXT("The payer's snapshot takes the later fence"), From.PlayerStateComponent->RedwoodCharacter.InventorySeq, FromSeq + 2);
  TestTrue(TEXT("The payer's balance is unknown"), From.PlayerStateComponent->RedwoodCharacter.bCurrencyAmountUnknown);
  TestTrue(TEXT("The payer waits for a reload"), From.Component->IsCurrencyRebaseRequired());
  TestFalse(TEXT("The payee's exact fence keeps its balance known"), To.PlayerStateComponent->RedwoodCharacter.bCurrencyAmountUnknown);
  TestEqual(TEXT("The payee's balance is committed"), To.PlayerStateComponent->RedwoodCharacter.CurrencyAmount, ToBalance + TradedAmount);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyTradeRefusalAfterTimeoutTest,
  "Redwood.Currency.Trade.RefusalAfterTimeoutFreezes",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A trade with no reply is sent again. A late reply to the first attempt is ignored. A refusal of
// the resend proves nothing about the first attempt, so both characters freeze, the game is told,
// and the held flush settles with nothing sent.
bool FRedwoodCurrencyTradeRefusalAfterTimeoutTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyTradeTest;
  AddExpectedErrorPlain(TEXT("got no reply"), EAutomationExpectedErrorFlags::Contains, 1);
  AddExpectedErrorPlain(TEXT("Failed to trade items"), EAutomationExpectedErrorFlags::Contains, 1);
  AddExpectedErrorPlain(TEXT("Its outcome is UNKNOWN"), EAutomationExpectedErrorFlags::Contains, 1);
  AddExpectedErrorPlain(TEXT("is frozen after a final refusal"), EAutomationExpectedErrorFlags::Contains, 1);
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  FParty From;
  FParty To;
  if (!MakeParty(Harness, FromId, FromToken, FromSeq, FromBalance, From) ||
      !MakeParty(Harness, ToId, ToToken, ToSeq, ToBalance, To)) {
    return false;
  }
  TArray<FString> FrozenIds;
  auto CountFreeze = [&FrozenIds](const FString &FrozenCharacterId, const FString &Error) {
    FrozenIds.Add(FrozenCharacterId);
  };
  From.Component->OnItemFlushRefusedFinal.AddLambda(CountFreeze);
  To.Component->OnItemFlushRefusedFinal.AddLambda(CountFreeze);

  FTradeResult Result;
  FRequest First;
  if (!SendTrade(Harness, From, To, Result, First)) {
    return false;
  }
  int32 SettleCount = 0;
  Harness.Subsystem->FlushItemsForCharacterComponent(
    From.PlayerStateComponent, From.Component, [&SettleCount]() { ++SettleCount; }
  );

  FAccess::TimeOutTrade(*Harness.Subsystem, TradeId);
  FAccess::ResendTradeNow(*Harness.Subsystem, TradeId);
  FRequest Second;
  if (!Harness.ReadRequest(TradeEvent, Second)) {
    return false;
  }
  Harness.Answer(First, CommittedReply(FromSeq + 1, ToSeq + 1));
  PumpFor(QuietWindowSeconds);
  TestEqual(TEXT("A late reply settles nothing"), Result.Calls, 0);
  TestEqual(TEXT("A late reply moves no seq"), From.Component->GetLastCommittedItemSeq(), FromSeq);

  Harness.Answer(Second, ErrorReply(TEXT("character is owned by another session")));
  TestTrue(TEXT("The trade settles"), Harness.PumpUntil([&Result]() { return Result.Calls > 0; }));
  TestFalse(TEXT("The caller gets an error"), Result.Error.IsEmpty());
  TestTrue(TEXT("The payer is frozen"), Harness.Subsystem->IsCharacterFrozen(FromId));
  TestTrue(TEXT("The payee is frozen"), Harness.Subsystem->IsCharacterFrozen(ToId));
  TestEqual(TEXT("The game is told once per character"), FrozenIds.Num(), 2);
  TestTrue(TEXT("The payer is told"), FrozenIds.Contains(FromId));
  TestTrue(TEXT("The payee is told"), FrozenIds.Contains(ToId));
  TestEqual(TEXT("The held flush settles"), SettleCount, 1);
  TestFalse(
    TEXT("A frozen character takes no new currency"),
    From.Component->QueueCurrency({MakeEntry(EntryIdB, LootAmount, FromBalance + LootAmount, LootReason)}, {})
  );
  TestTrue(TEXT("Nothing more goes out"), Harness.IsQuiet());
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyTradeItemOnlyTimeoutTest,
  "Redwood.Currency.Trade.ItemOnlyTimeoutFreezes",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A trade with no currency cannot be resent: the backend knows a committed resend only by its
// ledger rows. A timeout then leaves the outcome unknown for good, and both characters freeze.
bool FRedwoodCurrencyTradeItemOnlyTimeoutTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyTradeTest;
  AddExpectedErrorPlain(TEXT("Failed to trade items"), EAutomationExpectedErrorFlags::Contains, 1);
  AddExpectedErrorPlain(TEXT("Its outcome is UNKNOWN"), EAutomationExpectedErrorFlags::Contains, 1);
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  FParty From;
  FParty To;
  if (!MakeParty(Harness, FromId, FromToken, FromSeq, FromBalance, From) ||
      !MakeParty(Harness, ToId, ToToken, ToSeq, ToBalance, To)) {
    return false;
  }
  FRedwoodTradeRootPlacement Placement;
  Placement.Id = OwnedItemId;
  Placement.Domain = TEXT("general");
  Placement.Slot = 0;
  FTradeResult Result;
  Harness.Subsystem->EmitItemsTrade(From.Component, To.Component, {Placement}, {}, TradeId, Capture(Result));
  FRequest Trade;
  if (!Harness.ReadRequest(TradeEvent, Trade)) {
    return false;
  }
  TestEqual(TEXT("The item-only trade carries an empty currency array"), CountArray(Trade.Payload, TEXT("currency")), 0);
  TestEqual(TEXT("It still carries the trade id"), Trade.Payload->GetStringField(TEXT("tradeId")), TradeId);

  Harness.Answer(Trade, ErrorReply(URedwoodServerGameSubsystem::TradeTimedOutError));
  TestTrue(TEXT("The trade settles"), Harness.PumpUntil([&Result]() { return Result.Calls > 0; }));
  TestFalse(TEXT("The caller gets an error"), Result.Error.IsEmpty());
  TestTrue(TEXT("The giver is frozen"), Harness.Subsystem->IsCharacterFrozen(FromId));
  TestTrue(TEXT("The receiver is frozen"), Harness.Subsystem->IsCharacterFrozen(ToId));
  TestTrue(TEXT("No resend goes out"), Harness.IsQuiet());
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyTradeRefusedBeforeSendTest,
  "Redwood.Currency.Trade.RefusedBeforeSend",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A character that is frozen, lost its writer fence, has a batch in flight or currency staged, and
// currency the backend would refuse, are each refused before anything is sent: the caller hears at
// once, nothing is held, and no request leaves.
bool FRedwoodCurrencyTradeRefusedBeforeSendTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyTradeTest;
  AddExpectedErrorPlain(TEXT("refused before sending"), EAutomationExpectedErrorFlags::Contains, 9);
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  FParty To;
  if (!MakeParty(Harness, ToId, ToToken, ToSeq, ToBalance, To)) {
    return false;
  }

  // Each case gets its own payer, so one case's state cannot refuse the next.
  int32 CaseIndex = 0;
  auto ExpectRefused = [this, &Harness, &To, &CaseIndex](
                         const TCHAR *Label,
                         TFunctionRef<void(FParty &)> Arrange,
                         const TArray<TPair<FString, FRedwoodCurrencyEntry>> *Currency = nullptr,
                         const FString &InTradeId = TradeId
                       ) {
    FParty From;
    const FString PayerId = FString::Printf(TEXT("character-payer-%d"), ++CaseIndex);
    if (!MakeParty(Harness, PayerId, FromToken, FromSeq, FromBalance, From)) {
      return;
    }
    Arrange(From);
    FTradeResult Result;
    Harness.Subsystem->EmitItemsTrade(
      From.Component,
      To.Component,
      {},
      Currency ? *Currency : MakeTradeCurrency(TradedAmount, TradedAmount, PayerId),
      InTradeId,
      Capture(Result)
    );
    TestEqual(FString::Printf(TEXT("%s: the caller hears at once"), Label), Result.Calls, 1);
    TestFalse(FString::Printf(TEXT("%s: with an error"), Label), Result.Error.IsEmpty());
    TestNull(FString::Printf(TEXT("%s: nothing is pending"), Label), FAccess::Trade(*Harness.Subsystem, InTradeId));
  };

  ExpectRefused(TEXT("frozen"), [&Harness](FParty &From) {
    FAccess::FreezeCharacter(*Harness.Subsystem, From.Component->RedwoodCharacterId);
  });
  ExpectRefused(TEXT("writer fence lost"), [](FParty &From) { From.Component->MarkWriterFenceLost(); });
  ExpectRefused(TEXT("batch in flight"), [](FParty &From) {
    int64 BatchSeq = 0;
    From.Component->TryBeginItemFlush(BatchSeq);
  });
  ExpectRefused(TEXT("currency staged"), [](FParty &From) {
    From.Component->QueueCurrency({MakeEntry(EntryIdA, LootAmount, FromBalance + LootAmount, LootReason)}, {});
  });
  ExpectRefused(TEXT("balance unknown"), [](FParty &From) {
    From.PlayerStateComponent->RedwoodCharacter.bCurrencyAmountUnknown = true;
  });

  const FString PayerId = TEXT("character-payer-6");
  const TArray<TPair<FString, FRedwoodCurrencyEntry>> Overdraft =
    MakeTradeCurrency(FromBalance + 1, FromBalance + 1, PayerId);
  ExpectRefused(TEXT("cannot pay"), [](FParty &) {}, &Overdraft);
  const TArray<TPair<FString, FRedwoodCurrencyEntry>> Unbalanced =
    MakeTradeCurrency(TradedAmount, TradedAmount - 1, TEXT("character-payer-7"));
  ExpectRefused(TEXT("not summing to zero"), [](FParty &) {}, &Unbalanced);
  TArray<TPair<FString, FRedwoodCurrencyEntry>> WrongReason =
    MakeTradeCurrency(TradedAmount, TradedAmount, TEXT("character-payer-8"));
  WrongReason[0].Value.Reason = LootReason;
  ExpectRefused(TEXT("wrong reason"), [](FParty &) {}, &WrongReason);
  ExpectRefused(TEXT("bad trade id"), [](FParty &) {}, nullptr, TEXT("dddddddddddddddddddddddddddddddd"));

  TestTrue(TEXT("No request leaves"), Harness.IsQuiet());
  TestEqual(TEXT("The payee's lane is untouched"), To.Component->GetLastCommittedItemSeq(), ToSeq);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyTradeReplyClassifyTest,
  "Redwood.Currency.Trade.ReplyClassify",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// What each reply proves. The sidecar's two texts are pinned here: if either changes on one side
// only, a timeout reads as a refusal (settled as not applied while it may have committed).
bool FRedwoodCurrencyTradeReplyClassifyTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyTradeTest;
  AddExpectedErrorPlain(TEXT("fences"), EAutomationExpectedErrorFlags::Contains, 0);
  TestEqual(TEXT("The sidecar's timeout text"), FString(URedwoodServerGameSubsystem::TradeTimedOutError), FString(TEXT("Item trade request timed out.")));
  TestEqual(TEXT("The sidecar's no-realm text"), FString(URedwoodServerGameSubsystem::TradeRealmNotConnectedError), FString(TEXT("Realm server not connected")));

  auto Classify = [](const FString &Json) {
    TSharedPtr<FJsonObject> Object;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    FJsonSerializer::Deserialize(Reader, Object);
    return URedwoodServerGameSubsystem::ClassifyItemsTradeReply(
      URedwoodServerGameSubsystem::ParseItemsTradeReply(Object), FromSeq, ToSeq
    );
  };
  TestTrue(TEXT("A success past both seqs is a commit"), Classify(CommittedReply(FromSeq + 1, ToSeq + 1)) == ERedwoodItemTradeOutcome::Committed);
  TestTrue(TEXT("A success that does not pass a seq is unknown"), Classify(CommittedReply(FromSeq + 1, ToSeq)) == ERedwoodItemTradeOutcome::Unknown);
  TestTrue(TEXT("A success without fences is unknown"), Classify(TEXT(R"({"error":null})")) == ERedwoodItemTradeOutcome::Unknown);
  TestTrue(TEXT("The sidecar's timeout is unknown"), Classify(ErrorReply(URedwoodServerGameSubsystem::TradeTimedOutError)) == ERedwoodItemTradeOutcome::Unknown);
  TestTrue(TEXT("No realm is not forwarded"), Classify(ErrorReply(URedwoodServerGameSubsystem::TradeRealmNotConnectedError)) == ERedwoodItemTradeOutcome::NotForwarded);
  TestTrue(TEXT("Any other error is a refusal"), Classify(ErrorReply(TEXT("empty trade"))) == ERedwoodItemTradeOutcome::Refused);
  TestTrue(
    TEXT("An unreadable reply is unknown"),
    URedwoodServerGameSubsystem::ClassifyItemsTradeReply(
      URedwoodServerGameSubsystem::ParseItemsTradeReply(nullptr), FromSeq, ToSeq
    ) == ERedwoodItemTradeOutcome::Unknown
  );
  TestEqual(
    TEXT("Alerts are read"),
    URedwoodServerGameSubsystem::ParseItemsTradeReply([]() {
      TSharedPtr<FJsonObject> Object;
      TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(
        TEXT(R"({"error":null,"fromCommittedSeq":5,"toCommittedSeq":10,"alerts":[{"kind":"income","detail":{"ownerId":"character-to"}}]})")
      );
      FJsonSerializer::Deserialize(Reader, Object);
      return Object;
    }()).Alerts.Num(),
    1
  );
  return true;
}

#endif // WITH_AUTOMATION_WORKER
