// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart. Drives the currency
// half of URedwoodServerGameSubsystem's item flush through a real sidecar socket on a fake server:
// a batch of currency alone, the rows a re-sent batch must not carry, the freeze after a final
// refusal, the hold while a rebase reload runs and the settle a held attempt carries, the
// per-character hold, late reload replies, the wait for the sidecar through its real connect
// hook, the give-ups, the shutdown, and a component that was never hydrated. Each test fails if
// the code path it names is removed.

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
  // The same connect callback InitializeSidecar installs, on the test's socket.
  static void BindConnectedCallback(URedwoodServerGameSubsystem &Subsystem) {
    Subsystem.BindSidecarConnectedCallback();
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

#endif // WITH_AUTOMATION_WORKER
