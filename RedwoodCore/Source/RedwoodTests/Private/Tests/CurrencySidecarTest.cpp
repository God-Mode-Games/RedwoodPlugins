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
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "CurrencyFlushTestActor.h"
#include "FakeSocketIoServer.h"
#include "RedwoodCharacterComponent.h"
#include "RedwoodCurrencyTypes.h"
#include "RedwoodGameModeComponent.h"
#include "RedwoodPlayerStateComponent.h"
#include "RedwoodServerGameSubsystem.h"
#include "SocketIOClient.h"
#include "SocketIONative.h"
#include "TransferAbortListener.h"

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
  // Whether the connect callback has run: it starts the sidecar update timer.
  static bool HasHandledSidecarConnect(URedwoodServerGameSubsystem &Subsystem) {
    return Subsystem.TimerHandle_UpdateSidecar.IsValid();
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
  static uint64 LastFlushGeneration(URedwoodServerGameSubsystem &Subsystem) {
    return Subsystem.LastItemFlushGeneration;
  }
  // A reply to a flush attempt by its generation, as the socket callback delivers it.
  static void ReplyToFlush(
    URedwoodServerGameSubsystem &Subsystem, uint64 Generation, const FString &ReplyJson
  ) {
    TSharedPtr<FJsonObject> Reply;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ReplyJson);
    FJsonSerializer::Deserialize(Reader, Reply);
    Subsystem.HandleItemsFlushReply(Generation, Reply);
  }
  // The trade's resend timer firing, without waiting for it.
  static void ResendTradeNow(URedwoodServerGameSubsystem &Subsystem, const FString &TradeId) {
    Subsystem.SendItemsTrade(TradeId);
  }
  // The shutdown message's flag, without sending the message.
  static void SetShuttingDown(URedwoodServerGameSubsystem &Subsystem) {
    Subsystem.bIsShuttingDown = true;
  }
  // A flush's reply timer firing, without waiting for it.
  static void TimeOutFlush(URedwoodServerGameSubsystem &Subsystem, uint64 Generation) {
    Subsystem.HandleItemsFlushTimeout(Generation);
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
  const FString GoldCurrency = TEXT("Currency.Gold");
  const FString MarksCurrency = TEXT("Currency.FactionMarks");
  const FString LootReason = TEXT("CurrencyReason.Loot");
  const FString SellReason = TEXT("CurrencyReason.Merchant.Sell");
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

  FRedwoodCurrencyEntry MakeEntry(
    const FString &Id, int64 Delta, int64 BalanceAfter, const FString &Reason, const FString &Currency = GoldCurrency
  ) {
    FRedwoodCurrencyEntry Entry;
    Entry.Id = Id;
    Entry.Currency = Currency;
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
      TEXT(R"({"error":null,"items":[],"currency":{"balances":[{"currency":"Currency.Gold","amount":"%s"}],"grantKeys":[]}})"), Balance
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

    // Connects with the subsystem's real connect callback, and waits until that callback has run
    // for this first connect. The socket reports bIsConnected before the callback reaches the game
    // thread, and a callback that lands after the test's first flush is read as a reconnect: R34
    // turns that flush into a probe, and the test reads a second flush frame where it expects
    // another request. So the callback is bound before connecting and awaited, never bound after.
    bool ConnectWithConnectCallback() {
      if (Subsystem) {
        FAccess::BindConnectedCallback(*Subsystem);
      }
      URedwoodServerGameSubsystem *InSubsystem = Subsystem;
      return Connect() &&
        Test.TestTrue(
          TEXT("The connect callback runs"),
          PumpGameThreadUntil([InSubsystem]() { return FAccess::HasHandledSidecarConnect(*InSubsystem); })
        );
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

  // Reads the flush a landed reload resumed and answers it as committed, so no flush is open at
  // teardown: the shutdown logs an open flush's entries as unknown, at Error.
  bool AnswerResumedFlush(FSidecarHarness &Harness, URedwoodCharacterComponent *Component) {
    FRequest Resumed;
    if (!Harness.ReadRequest(FlushEvent, Resumed)) {
      return false;
    }
    Harness.Answer(
      Resumed,
      FString::Printf(
        TEXT(R"({"error":null,"committedSeq":%lld})"),
        static_cast<int64>(Resumed.Payload->GetNumberField(TEXT("batchSeq")))
      )
    );
    return Harness.PumpUntil([Component]() { return !Component->IsItemFlushInFlight(); });
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
  Harness.Answer(First, TEXT(R"({"error":"Item flush request timed out.","committedSeq":0,"timedOut":true})"));
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
  AddExpectedErrorPlain(TEXT("dropped by the freeze after a final refusal"), EAutomationExpectedErrorFlags::Contains, 0);
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
  AddExpectedErrorPlain(
    TEXT("entry AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA currency Currency.Gold delta -2000 reason CurrencyReason.Loot, NOT saved: dropped by the freeze"),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  // The credit is dropped, and logged so staff can see it never committed.
  AddExpectedErrorPlain(
    TEXT("entry CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC currency Currency.Gold delta 25 reason CurrencyReason.Merchant.Sell, NOT saved: dropped by the freeze"),
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
  TArray<FRequest> ResumedFlushes;
  for (int32 Index = 0; Index < 2; ++Index) {
    FRequest Resumed;
    if (!Harness.ReadRequest(FlushEvent, Resumed)) {
      break;
    }
    bRowSent |= CountArray(Resumed.Payload, TEXT("upserts")) == 1;
    bEntrySent |= FirstBalanceAfter(Resumed.Payload) == TEXT("2500");
    ResumedFlushes.Add(Resumed);
  }
  TestTrue(TEXT("The held row goes out after the reload"), bRowSent);
  TestTrue(TEXT("The re-chained entry goes out after the reload"), bEntrySent);
  // Answered, so no flush is open at teardown (the shutdown logs an open one's entries as unknown).
  for (const FRequest &Resumed : ResumedFlushes) {
    Harness.Answer(Resumed, TEXT(R"({"error":null,"committedSeq":6})"));
  }
  Harness.PumpUntil([CurrencyComponent, ItemComponent]() {
    return !CurrencyComponent->IsItemFlushInFlight() && !ItemComponent->IsItemFlushInFlight();
  });
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
  TestTrue(TEXT("The resumed flush is answered"), AnswerResumedFlush(Harness, Component));
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
  if (!Harness.ConnectWithConnectCallback()) {
    return false;
  }
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
  TestTrue(TEXT("The resumed flush is answered"), AnswerResumedFlush(Harness, Component));
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
    TEXT("gave up (6 attempts failed). Its held flushes now settle."), EAutomationExpectedErrorFlags::Contains, 1
  );
  AddExpectedErrorPlain(
    TEXT("entry AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA currency Currency.Gold delta 500 reason CurrencyReason.Loot, NOT saved: held by a currency reload that gave up (6 attempts failed)"),
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
    TEXT(R"({"error":"Item load request timed out.","items":[],"currency":{"balances":[],"grantKeys":[]}})");
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
// character) ends the reload on that reply, and marks the fence lost as a flush reply's flag does:
// the game hears it once, and no save pass starts the reload again.
bool FRedwoodCurrencySidecarWriterFenceLostTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  AddExpectedError(TEXT("Failed to flush items"), EAutomationExpectedErrorFlags::Contains, 0);
  AddExpectedError(TEXT("Currency reload failed"), EAutomationExpectedErrorFlags::Contains, 0);
  AddExpectedErrorPlain(
    TEXT("gave up (this session no longer owns the character). Its held flushes now settle."),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  // Logged once, by the lost fence; the give-up does not log it a second time.
  AddExpectedErrorPlain(
    TEXT("entry AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA currency Currency.Gold delta 500 reason CurrencyReason.Loot, NOT saved: writer fence lost"),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Component = Harness.MakeComponent(Actor);
  int32 FenceLostCount = 0;
  Component->OnWriterFenceLost.AddLambda([&FenceLostCount](const FString &) { ++FenceLostCount; });

  int32 SettleCount = 0;
  FRequest Load;
  if (!StartRebaseReload(Harness, Component, [&SettleCount]() { ++SettleCount; }, Load)) {
    return false;
  }

  // The binding is missing for now (a sidecar restart): retried.
  Harness.Answer(
    Load,
    TEXT(R"({"error":"character not bound to this instance","items":[],"currency":{"balances":[],"grantKeys":[]}})")
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
    TEXT(R"({"error":"character not bound to this instance","writerFenceLost":true,"items":[],"currency":{"balances":[],"grantKeys":[]}})")
  );
  TestTrue(TEXT("The held attempt settles at once"), Harness.PumpUntil([&SettleCount]() { return SettleCount > 0; }));
  TestNull(TEXT("No retry is scheduled"), FAccess::Reload(*Harness.Subsystem, CharacterId));
  TestTrue(TEXT("The component's fence is lost"), Component->IsWriterFenceLost());
  TestEqual(TEXT("The game hears it once"), FenceLostCount, 1);

  // The next save passes: the component still holds its entry, but no reload or flush goes out.
  for (int32 Pass = 0; Pass < 2; ++Pass) {
    Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component, [&SettleCount]() { ++SettleCount; });
  }
  TestTrue(TEXT("No further reload goes out"), Harness.IsQuiet());
  TestNull(TEXT("No reload runs"), FAccess::Reload(*Harness.Subsystem, CharacterId));
  TestEqual(TEXT("Each later attempt settles at once"), SettleCount, 3);
  TestEqual(TEXT("The game still heard it only once"), FenceLostCount, 1);
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
  AddExpectedErrorPlain(
    TEXT("gave up (the server is shutting down). Its held flushes now settle."), EAutomationExpectedErrorFlags::Contains, 1
  );
  AddExpectedErrorPlain(
    TEXT("NOT saved: held by a currency reload that gave up (the server is shutting down)"),
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
  Harness.Answer(First, TEXT(R"({"error":"Item flush request timed out.","committedSeq":0,"timedOut":true})"));
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
  // Answered, so no flush is open at teardown (the shutdown logs an open one's entries as unknown).
  Harness.Answer(Normal, TEXT(R"({"error":null,"committedSeq":6})"));
  Harness.PumpUntil([Component]() { return !Component->IsItemFlushInFlight(); });
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
    TEXT("entry AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA currency Currency.Gold delta -2000 reason CurrencyReason.Loot, NOT saved: dropped by the freeze"),
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
    TEXT("character character-1 entry AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA currency Currency.Gold delta 500 reason CurrencyReason.Loot, NOT saved: writer fence lost, another session owns the character. A re-grant must reuse entry id AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
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
    TEXT(R"("currency":{"balances":[{"currency":"Currency.Gold","amount":"%lld"}],"grantKeys":["%s"]})"), LoadedBalance, *LoadedGrantKey
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
  TestEqual(TEXT("The snapshot balance follows the commit"), GetCurrencyBalance(Snapshot.CurrencyBalances, GoldCurrency), LoadedBalance + 500);
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
  Snapshot.CurrencyBalances.Add(GoldCurrency, SnapshotBalance);

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
  TestEqual(TEXT("The snapshot takes the committed balance"), GetCurrencyBalance(Snapshot.CurrencyBalances, GoldCurrency), SnapshotBalance + 500);
  TestFalse(TEXT("The balance is known"), Snapshot.bCurrencyBalancesUnknown);

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

// An error reply whose outcome is unknown, for a component that is gone, writes no seq, balance or
// key to the snapshot. It logs the entry and the key for staff, as UNKNOWN, and marks the seq so
// the next component probes it.
bool FRedwoodCurrencySidecarComponentGoneErrorTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  // Plain, not AddExpectedError: that one reads its pattern as a regex, where the parentheses are a
  // group and the logged text no longer matches.
  AddExpectedErrorPlain(
    TEXT("Failed to flush items (component gone): concurrent flush"), EAutomationExpectedErrorFlags::Contains, 1
  );
  AddExpectedErrorPlain(
    TEXT("character character-1 entry AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA currency Currency.Gold delta 500 reason CurrencyReason.Loot, outcome UNKNOWN (it may be saved): its component is gone"),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  AddExpectedErrorPlain(
    TEXT("character character-1 grant quest:Q1, outcome UNKNOWN (it may be saved): its component is gone"),
    EAutomationExpectedErrorFlags::Contains,
    1
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
  Snapshot.CurrencyBalances.Add(GoldCurrency, SnapshotBalance);

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
  TestEqual(TEXT("The balance is unchanged"), GetCurrencyBalance(Snapshot.CurrencyBalances, GoldCurrency), SnapshotBalance);
  TestEqual(TEXT("No key is added"), Snapshot.GrantKeys.Num(), 0);
  TestTrue(TEXT("The seq is marked unconfirmed"), Snapshot.bInventorySeqUnconfirmed);
  TestEqual(TEXT("At the sent seq"), Snapshot.UnconfirmedInventorySeq, SeededInventorySeq + 1);
  TestTrue(TEXT("The balance it may have moved is unknown"), Snapshot.bCurrencyBalancesUnknown);
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
  // A second trade in one test needs its own id: a reused one is refused.
  const FString FreshTradeId = TEXT("D0D0D0D0D0D0D0D0D0D0D0D0D0D0D0D0");
  const FString PaidEntryId = TEXT("EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE");
  const FString ReceivedEntryId = TEXT("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF");
  const FString TradeReason = TEXT("CurrencyReason.Trade");
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
    Snapshot.CurrencyBalances.Add(GoldCurrency, Balance);
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

  // The sidecar's reply to a trade it sent and got no answer for.
  FString TimedOutReply() {
    return FString::Printf(
      TEXT(R"({"error":"%s","fromCommittedSeq":-1,"toCommittedSeq":-1,"timedOut":true})"),
      URedwoodServerGameSubsystem::TradeTimedOutError
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

  // The payload as one line of JSON, to compare two sends byte for byte.
  FString SerializePayload(const TSharedPtr<FJsonObject> &Payload) {
    FString Text;
    if (Payload.IsValid()) {
      TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
      FJsonSerializer::Serialize(Payload.ToSharedRef(), Writer);
    }
    return Text;
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
  From.Component->OnCurrencyRebased.AddLambda([&RebasedCount, &RebasedBalance](const FRedwoodCurrencyBalances &Balances) {
    ++RebasedCount;
    RebasedBalance = GetCurrencyBalance(Balances, GoldCurrency);
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
  TestEqual(TEXT("The payer's snapshot takes the balance"), GetCurrencyBalance(From.PlayerStateComponent->RedwoodCharacter.CurrencyBalances, GoldCurrency), FromBalance - TradedAmount);
  TestEqual(TEXT("The payee's snapshot takes the balance"), GetCurrencyBalance(To.PlayerStateComponent->RedwoodCharacter.CurrencyBalances, GoldCurrency), ToBalance + TradedAmount);
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
  From.Component->OnCurrencyRebased.AddLambda([&RebasedBalance](const FRedwoodCurrencyBalances &Balances) {
    RebasedBalance = GetCurrencyBalance(Balances, GoldCurrency);
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
  TestEqual(TEXT("The payer's snapshot balance is unchanged"), GetCurrencyBalance(From.PlayerStateComponent->RedwoodCharacter.CurrencyBalances, GoldCurrency), FromBalance);
  TestEqual(TEXT("The payee's snapshot balance is unchanged"), GetCurrencyBalance(To.PlayerStateComponent->RedwoodCharacter.CurrencyBalances, GoldCurrency), ToBalance);
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

  // A new trade under the same id is refused: the backend would take it for this one.
  AddExpectedErrorPlain(TEXT("the trade id was already used"), EAutomationExpectedErrorFlags::Contains, 1);
  Harness.Answer(Flush, FString::Printf(TEXT(R"({"error":null,"committedSeq":%lld})"), FromSeq + 1));
  Harness.PumpUntil([&From]() { return From.Component->GetPendingCurrencyEntries().Num() == 0; });
  FTradeResult Reused;
  Harness.Subsystem->EmitItemsTrade(
    From.Component, To.Component, {}, MakeTradeCurrency(TradedAmount, TradedAmount), TradeId, Capture(Reused)
  );
  TestEqual(TEXT("A reused trade id is refused at once"), Reused.Calls, 1);
  TestTrue(TEXT("Nothing leaves for it"), Harness.IsQuiet());
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
  Harness.Answer(First, TimedOutReply());
  To.Component->QueueCurrency({MakeEntry(EntryIdA, LootAmount, ToBalance + TradedAmount + LootAmount, LootReason)}, {});
  int32 SettleCount = 0;
  Harness.Subsystem->FlushItemsForCharacterComponent(
    To.PlayerStateComponent, To.Component, [&SettleCount]() { ++SettleCount; }
  );
  TestTrue(TEXT("Nothing goes out between the timeout and the resend"), Harness.IsQuiet());
  TestEqual(TEXT("The timed-out trade is not settled"), Result.Calls, 0);
  TestEqual(TEXT("The held flush has not settled"), SettleCount, 0);
  TestNotNull(TEXT("The trade is still pending"), FAccess::Trade(*Harness.Subsystem, TradeId));

  // Everything a rebuilt payload would read changes now: the payer's snapshot balance (its
  // balanceAfter) and both writer tokens. A resend that rebuilt the request would differ from the
  // first send.
  From.PlayerStateComponent->RedwoodCharacter.CurrencyBalances.Add(GoldCurrency, FromBalance * 5);
  From.Component->SeedWriterToken(TEXT("writer-token-from-changed"));
  To.Component->SeedWriterToken(TEXT("writer-token-to-changed"));
  FAccess::ResendTradeNow(*Harness.Subsystem, TradeId);
  FRequest Second;
  if (!Harness.ReadRequest(TradeEvent, Second)) {
    return false;
  }
  TestEqual(TEXT("The resend keeps the trade id"), Second.Payload->GetStringField(TEXT("tradeId")), TradeId);
  TestEqual(TEXT("The resend is byte for byte the first send"), SerializePayload(Second.Payload), SerializePayload(First.Payload));
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
  TestEqual(TEXT("The payee's snapshot takes the balance"), GetCurrencyBalance(To.PlayerStateComponent->RedwoodCharacter.CurrencyBalances, GoldCurrency), ToBalance + TradedAmount);

  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }
  TestEqual(TEXT("The held flush goes out after the trade's seq"), BatchSeqOf(Flush.Payload), ToSeq + 2);
  TestEqual(TEXT("The loot keeps its chain on the payee's new balance"), FirstBalanceAfter(Flush.Payload), FString(TEXT("550")));
  // Answered, so no flush is open at teardown (the shutdown logs an open one's entries as unknown).
  Harness.Answer(Flush, FString::Printf(TEXT(R"({"error":null,"committedSeq":%lld})"), ToSeq + 2));
  Harness.PumpUntil([&To]() { return !To.Component->IsItemFlushInFlight(); });
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
  TestTrue(TEXT("The payer's balance is unknown"), From.PlayerStateComponent->RedwoodCharacter.bCurrencyBalancesUnknown);
  TestTrue(TEXT("The payer waits for a reload"), From.Component->IsCurrencyRebaseRequired());
  TestFalse(TEXT("The payee's exact fence keeps its balance known"), To.PlayerStateComponent->RedwoodCharacter.bCurrencyBalancesUnknown);
  TestEqual(TEXT("The payee's balance is committed"), GetCurrencyBalance(To.PlayerStateComponent->RedwoodCharacter.CurrencyBalances, GoldCurrency), ToBalance + TradedAmount);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyTradeRefusalAfterTimeoutTest,
  "Redwood.Currency.Trade.RefusalAfterTimeoutIsNotApplied",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A trade with no reply is sent again, and a late reply to the first attempt is ignored. The realm
// checks for a committed trade after every refusal inside its transaction, so a refusal of the
// resend proves the trade never applied: it settles as NOT applied, and nobody freezes.
bool FRedwoodCurrencyTradeRefusalAfterTimeoutTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyTradeTest;
  AddExpectedErrorPlain(TEXT("got no reply"), EAutomationExpectedErrorFlags::Contains, 1);
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
  TestFalse(TEXT("The caller gets the error"), Result.Error.IsEmpty());
  TestFalse(TEXT("The payer is not frozen"), Harness.Subsystem->IsCharacterFrozen(FromId));
  TestFalse(TEXT("The payee is not frozen"), Harness.Subsystem->IsCharacterFrozen(ToId));
  TestEqual(TEXT("The payer's lane is unchanged"), From.Component->GetLastCommittedItemSeq(), FromSeq);
  TestEqual(TEXT("The payer's balance is unchanged"), GetCurrencyBalance(From.PlayerStateComponent->RedwoodCharacter.CurrencyBalances, GoldCurrency), FromBalance);
  TestEqual(TEXT("The held flush settles"), SettleCount, 1);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyTradeNoSettlingReplyTest,
  "Redwood.Currency.Trade.NoSettlingReplyFreezes",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// After a timeout, a refusal that depends on this server's session (the characters' binding) says
// nothing about the first attempt, so the trade is sent again. When no attempt settles it, its
// outcome cannot be learned: both characters freeze, the game is told, and the held flush settles
// with nothing sent.
bool FRedwoodCurrencyTradeNoSettlingReplyTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyTradeTest;
  const int32 Resends = URedwoodServerGameSubsystem::TradeMaxAttempts - 1;
  AddExpectedErrorPlain(TEXT("got no reply"), EAutomationExpectedErrorFlags::Contains, 1);
  AddExpectedErrorPlain(TEXT("Failed to trade items"), EAutomationExpectedErrorFlags::Contains, Resends);
  AddExpectedErrorPlain(TEXT("Its outcome is UNKNOWN"), EAutomationExpectedErrorFlags::Contains, 1);
  // A staff line for each side's entry.
  AddExpectedErrorPlain(TEXT("outcome UNKNOWN (it may be saved): in trade"), EAutomationExpectedErrorFlags::Contains, 2);
  // The refused new entry at the end. Only that line: each side's desync kick warning also says
  // the character "is frozen after a final refusal", once per side.
  AddExpectedErrorPlain(TEXT("is frozen after a final refusal; refused entry"), EAutomationExpectedErrorFlags::Contains, 1);
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

  for (int32 Attempt = 0; Attempt < Resends; ++Attempt) {
    TestEqual(TEXT("Not settled while attempts remain"), Result.Calls, 0);
    FAccess::ResendTradeNow(*Harness.Subsystem, TradeId);
    FRequest Resend;
    if (!Harness.ReadRequest(TradeEvent, Resend)) {
      return false;
    }
    // The flag decides, so the text is one the plugin knows nothing of.
    Harness.Answer(
      Resend,
      TEXT(R"({"error":"refused by this session","fromCommittedSeq":-1,"toCommittedSeq":-1,"sessionRefused":true})")
    );
    Harness.PumpUntil([&Harness]() {
      const FRedwoodPendingItemTrade *Pending = FAccess::Trade(*Harness.Subsystem, TradeId);
      return !Pending || Pending->Generation == 0;
    });
  }

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
  FRedwoodCurrencyTradeItemOnlyCommitUnknownTest,
  "Redwood.Currency.Trade.ItemOnlyCommitUnknownResends",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A trade with items only, whose commit the realm could not confirm, is sent again byte for byte
// (the backend refuses a reused trade id with other contents), with both lanes held. Its receipt
// lets the backend ack the resend as a replay, and the trade then settles as committed.
bool FRedwoodCurrencyTradeItemOnlyCommitUnknownTest::RunTest(const FString &Parameters) {
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
  FRedwoodTradeRootPlacement Placement;
  Placement.Id = OwnedItemId;
  Placement.Domain = TEXT("general");
  Placement.Slot = 0;
  FTradeResult Result;
  Harness.Subsystem->EmitItemsTrade(From.Component, To.Component, {Placement}, {}, TradeId, Capture(Result));
  FRequest First;
  if (!Harness.ReadRequest(TradeEvent, First)) {
    return false;
  }
  TestEqual(TEXT("The item-only trade carries an empty currency array"), CountArray(First.Payload, TEXT("currency")), 0);
  TestEqual(TEXT("It still carries the trade id"), First.Payload->GetStringField(TEXT("tradeId")), TradeId);

  Harness.Answer(
    First,
    TEXT(R"({"error":"database socket timed out","fromCommittedSeq":-1,"toCommittedSeq":-1,"commitUnknown":true})")
  );
  Harness.Subsystem->FlushItemsForCharacterComponent(To.PlayerStateComponent, To.Component);
  TestTrue(TEXT("Nothing goes out before the resend"), Harness.IsQuiet());
  TestEqual(TEXT("The trade is not settled"), Result.Calls, 0);
  TestFalse(TEXT("Nobody is frozen"), Harness.Subsystem->IsCharacterFrozen(FromId));

  // A rebuilt payload would carry these tokens; the resend must still carry the first send's.
  From.Component->SeedWriterToken(TEXT("writer-token-from-changed"));
  To.Component->SeedWriterToken(TEXT("writer-token-to-changed"));
  FAccess::ResendTradeNow(*Harness.Subsystem, TradeId);
  FRequest Second;
  if (!Harness.ReadRequest(TradeEvent, Second)) {
    return false;
  }
  TestEqual(TEXT("The resend is byte for byte the first send"), SerializePayload(Second.Payload), SerializePayload(First.Payload));

  Harness.Answer(Second, CommittedReply(FromSeq + 1, ToSeq + 1));
  TestTrue(TEXT("The trade settles"), Harness.PumpUntil([&Result]() { return Result.Calls > 0; }));
  TestTrue(TEXT("The replay settles it as committed"), Result.Error.IsEmpty());
  TestEqual(TEXT("The giver's lane takes the fence"), From.Component->GetLastCommittedItemSeq(), FromSeq + 1);
  TestEqual(TEXT("The receiver's lane takes the fence"), To.Component->GetLastCommittedItemSeq(), ToSeq + 1);
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
  AddExpectedErrorPlain(TEXT("refused before sending"), EAutomationExpectedErrorFlags::Contains, 10);
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
    From.PlayerStateComponent->RedwoodCharacter.bCurrencyBalancesUnknown = true;
  });

  const TArray<TPair<FString, FRedwoodCurrencyEntry>> Overdraft =
    MakeTradeCurrency(FromBalance + 1, FromBalance + 1, TEXT("character-payer-6"));
  ExpectRefused(TEXT("cannot pay"), [](FParty &) {}, &Overdraft);
  const TArray<TPair<FString, FRedwoodCurrencyEntry>> Unbalanced =
    MakeTradeCurrency(TradedAmount, TradedAmount - 1, TEXT("character-payer-7"));
  ExpectRefused(TEXT("not summing to zero"), [](FParty &) {}, &Unbalanced);
  TArray<TPair<FString, FRedwoodCurrencyEntry>> WrongReason =
    MakeTradeCurrency(TradedAmount, TradedAmount, TEXT("character-payer-8"));
  WrongReason[0].Value.Reason = LootReason;
  ExpectRefused(TEXT("wrong reason"), [](FParty &) {}, &WrongReason);
  ExpectRefused(TEXT("bad trade id"), [](FParty &) {}, nullptr, TEXT("dddddddddddddddddddddddddddddddd"));

  // A placement that leaves a character outside the trade.
  FParty Stranger;
  if (MakeParty(Harness, TEXT("character-payer-10"), FromToken, FromSeq, FromBalance, Stranger)) {
    FRedwoodTradeRootPlacement Placement;
    Placement.Id = OwnedItemId;
    Placement.Domain = TEXT("general");
    Placement.FromCharacterId = TEXT("character-stranger");
    FTradeResult Result;
    Harness.Subsystem->EmitItemsTrade(Stranger.Component, To.Component, {Placement}, {}, TradeId, Capture(Result));
    TestEqual(TEXT("outside placement: the caller hears at once"), Result.Calls, 1);
    TestFalse(TEXT("outside placement: with an error"), Result.Error.IsEmpty());
  }

  TestTrue(TEXT("No request leaves"), Harness.IsQuiet());
  TestEqual(TEXT("The payee's lane is untouched"), To.Component->GetLastCommittedItemSeq(), ToSeq);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyTradeReplyClassifyTest,
  "Redwood.Currency.Trade.ReplyClassify",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// What each reply proves. The flags decide: a text that says one thing and a flag that says another
// follow the flag, so a test that passes only through the text fallback fails here.
bool FRedwoodCurrencyTradeReplyClassifyTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyTradeTest;
  // The two successes whose fences do not pass a seq, and the two old-sidecar fallbacks.
  AddExpectedErrorPlain(TEXT("do not pass the seqs before the trade"), EAutomationExpectedErrorFlags::Contains, 2);
  AddExpectedErrorPlain(TEXT("read the outcome from its text"), EAutomationExpectedErrorFlags::Contains, 4);

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

  // The flags, each with a text that would mean something else.
  TestTrue(
    TEXT("timedOut is unknown, whatever the text"),
    Classify(TEXT(R"({"error":"empty trade","fromCommittedSeq":-1,"toCommittedSeq":-1,"timedOut":true})")) ==
      ERedwoodItemTradeOutcome::Unknown
  );
  TestTrue(
    TEXT("commitUnknown is unknown, whatever the text"),
    Classify(TEXT(R"({"error":"empty trade","fromCommittedSeq":-1,"toCommittedSeq":-1,"commitUnknown":true})")) ==
      ERedwoodItemTradeOutcome::Unknown
  );
  TestTrue(
    TEXT("commitUnknown with a success shape is still unknown"),
    Classify(TEXT(R"({"error":null,"fromCommittedSeq":5,"toCommittedSeq":10,"commitUnknown":true})")) ==
      ERedwoodItemTradeOutcome::Unknown
  );
  TestTrue(
    TEXT("notForwarded is not forwarded, whatever the text"),
    Classify(TEXT(R"({"error":"empty trade","fromCommittedSeq":-1,"toCommittedSeq":-1,"notForwarded":true})")) ==
      ERedwoodItemTradeOutcome::NotForwarded
  );
  TestTrue(
    TEXT("A present false flag beats the timeout text"),
    Classify(FString::Printf(
      TEXT(R"({"error":"%s","fromCommittedSeq":-1,"toCommittedSeq":-1,"timedOut":false})"),
      URedwoodServerGameSubsystem::TradeTimedOutError
    )) == ERedwoodItemTradeOutcome::Refused
  );
  TestTrue(TEXT("Any other error is a refusal"), Classify(ErrorReply(TEXT("empty trade"))) == ERedwoodItemTradeOutcome::Refused);

  // A sidecar that predates the flags: its texts, with a Warning.
  TestTrue(TEXT("An old sidecar's timeout is unknown"), Classify(ErrorReply(URedwoodServerGameSubsystem::TradeTimedOutError)) == ERedwoodItemTradeOutcome::Unknown);
  TestTrue(
    TEXT("An old sidecar's missing realm is not forwarded"),
    Classify(ErrorReply(URedwoodServerGameSubsystem::SidecarRealmNotConnectedError)) == ERedwoodItemTradeOutcome::NotForwarded
  );

  TestTrue(
    TEXT("An unreadable reply is unknown"),
    URedwoodServerGameSubsystem::ClassifyItemsTradeReply(
      URedwoodServerGameSubsystem::ParseItemsTradeReply(nullptr), FromSeq, ToSeq
    ) == ERedwoodItemTradeOutcome::Unknown
  );
  // sessionRefused: from the flag, whatever the text; from the text only when the flag is absent.
  auto Parse = [](const FString &Json) {
    TSharedPtr<FJsonObject> Object;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    FJsonSerializer::Deserialize(Reader, Object);
    return URedwoodServerGameSubsystem::ParseItemsTradeReply(Object);
  };
  TestTrue(
    TEXT("sessionRefused is read from the flag"),
    Parse(TEXT(R"({"error":"refused by this session","fromCommittedSeq":-1,"toCommittedSeq":-1,"sessionRefused":true})")).bSessionRefused
  );
  TestFalse(
    TEXT("A present false flag beats the binding text"),
    Parse(FString::Printf(
      TEXT(R"({"error":"%s","fromCommittedSeq":-1,"toCommittedSeq":-1,"sessionRefused":false})"),
      URedwoodServerGameSubsystem::TradeNotBoundError
    )).bSessionRefused
  );
  TestFalse(TEXT("Another refusal is no session refusal"), Parse(ErrorReply(TEXT("empty trade"))).bSessionRefused);
  TestTrue(
    TEXT("An old realm's binding text is read, with a Warning"),
    Parse(ErrorReply(URedwoodServerGameSubsystem::TradeNotBoundError)).bSessionRefused
  );

  // tradeIdReused: the same rule.
  TestTrue(
    TEXT("tradeIdReused is read from the flag"),
    Parse(TEXT(R"({"error":"reused for the test","fromCommittedSeq":-1,"toCommittedSeq":-1,"tradeIdReused":true})")).bTradeIdReused
  );
  TestFalse(
    TEXT("A present false flag beats the reused-id text"),
    Parse(FString::Printf(
      TEXT(R"({"error":"%s","fromCommittedSeq":-1,"toCommittedSeq":-1,"tradeIdReused":false})"),
      URedwoodServerGameSubsystem::TradeIdReusedError
    )).bTradeIdReused
  );
  TestFalse(TEXT("Another refusal is no reused id"), Parse(ErrorReply(TEXT("empty trade"))).bTradeIdReused);
  TestTrue(
    TEXT("An old realm's reused-id text is read, with a Warning"),
    Parse(ErrorReply(URedwoodServerGameSubsystem::TradeIdReusedError)).bTradeIdReused
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyTradeFloorTest,
  "Redwood.Currency.Trade.DebitBelowTradeFloorRefused",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// While a trade is unsettled, a debit staged on either side must fit on the lower of the balances
// the trade can leave: after it for the payer, before it for the payee. One that does not is
// refused at the queue, and what was accepted re-chains on the commit without going below zero.
bool FRedwoodCurrencyTradeFloorTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyTradeTest;
  AddExpectedErrorPlain(TEXT("has a trade unsettled"), EAutomationExpectedErrorFlags::Contains, 2);
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
  // The payer offers everything it has.
  FTradeResult Result;
  Harness.Subsystem->EmitItemsTrade(
    From.Component, To.Component, {}, MakeTradeCurrency(FromBalance, FromBalance), TradeId, Capture(Result)
  );
  FRequest Trade;
  if (!Harness.ReadRequest(TradeEvent, Trade)) {
    return false;
  }

  const FString PurchaseId = TEXT("11111111111111111111111111111111");
  TestFalse(
    TEXT("A purchase the payer could not pay after the trade is refused"),
    From.Component->QueueCurrency({MakeEntry(PurchaseId, -100, FromBalance - 100, LootReason)}, {})
  );
  TestEqual(TEXT("It is not staged"), From.Component->GetPendingCurrencyEntries().Num(), 0);
  TestTrue(
    TEXT("A credit is accepted"),
    From.Component->QueueCurrency({MakeEntry(EntryIdA, LootAmount, LootAmount, LootReason)}, {})
  );
  TestTrue(
    TEXT("A debit the credit pays for is accepted"),
    From.Component->QueueCurrency({MakeEntry(EntryIdB, -LootAmount, 0, LootReason)}, {})
  );
  // The payee's floor is its balance before the trade: the trade may not apply.
  TestFalse(
    TEXT("A payee debit past its balance before the trade is refused"),
    To.Component->QueueCurrency({MakeEntry(PurchaseId, -(ToBalance + 1), FromBalance - 1, LootReason)}, {})
  );
  TestTrue(
    TEXT("A payee debit within it is accepted"),
    To.Component->QueueCurrency({MakeEntry(CreditEntryId, -ToBalance, FromBalance, LootReason)}, {})
  );

  Harness.Answer(Trade, CommittedReply(FromSeq + 1, ToSeq + 1));
  TestTrue(TEXT("The trade settles"), Harness.PumpUntil([&Result]() { return Result.Calls > 0; }));
  TestTrue(TEXT("It committed"), Result.Error.IsEmpty());
  const TArray<FRedwoodCurrencyEntry> &FromPending = From.Component->GetPendingCurrencyEntries();
  if (TestEqual(TEXT("The payer keeps both accepted entries"), FromPending.Num(), 2)) {
    TestEqual(TEXT("The credit re-chains from zero"), FromPending[0].BalanceAfter, LootAmount);
    TestEqual(TEXT("The debit ends at zero, not below"), FromPending[1].BalanceAfter, static_cast<int64>(0));
  }
  TestFalse(TEXT("The payer's currency is not held"), From.Component->IsCurrencyRebaseRequired());
  const TArray<FRedwoodCurrencyEntry> &ToPending = To.Component->GetPendingCurrencyEntries();
  if (TestEqual(TEXT("The payee keeps its accepted debit"), ToPending.Num(), 1)) {
    TestEqual(TEXT("It re-chains from the balance after the trade"), ToPending[0].BalanceAfter, FromBalance);
  }
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyTradeOtherComponentInFlightTest,
  "Redwood.Currency.Trade.OtherComponentInFlightRefuses",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// The flight slot is per component, but the seq is per character. While another component of the
// payer has a batch open, the trade is refused: it could take that batch's seq. Once the batch is
// answered, the trade goes out from the seq and balance that batch committed.
bool FRedwoodCurrencyTradeOtherComponentInFlightTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyTradeTest;
  AddExpectedErrorPlain(TEXT("has an item flush in flight"), EAutomationExpectedErrorFlags::Contains, 1);
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
  // The payer's other component (its pawn's, say) sends loot and waits for the reply.
  ARedwoodCurrencyFlushTestActor *PawnActor = nullptr;
  URedwoodCharacterComponent *PawnComponent = Harness.MakeComponent(PawnActor, FromId);
  PawnComponent->QueueCurrency({MakeEntry(EntryIdA, LootAmount, FromBalance + LootAmount, LootReason)}, {});
  Harness.Subsystem->FlushItemsForCharacterComponent(From.PlayerStateComponent, PawnComponent);
  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }

  FTradeResult Refused;
  Harness.Subsystem->EmitItemsTrade(
    From.Component, To.Component, {}, MakeTradeCurrency(TradedAmount, TradedAmount), TradeId, Capture(Refused)
  );
  TestEqual(TEXT("The trade is refused at once"), Refused.Calls, 1);
  TestFalse(TEXT("With an error"), Refused.Error.IsEmpty());
  TestTrue(TEXT("No trade leaves"), Harness.IsQuiet());

  Harness.Answer(Flush, FString::Printf(TEXT(R"({"error":null,"committedSeq":%lld})"), FromSeq + 1));
  TestTrue(
    TEXT("The loot batch is answered"),
    Harness.PumpUntil([PawnComponent]() { return PawnComponent->GetPendingCurrencyEntries().Num() == 0; })
  );

  FTradeResult Result;
  FRequest Trade;
  if (!SendTrade(Harness, From, To, Result, Trade)) {
    return false;
  }
  TestEqual(
    TEXT("The trade chains from the balance the loot batch committed"),
    TradeCurrencyField(Trade.Payload, 0, TEXT("balanceAfter")),
    FString(TEXT("750"))
  );
  Harness.Answer(Trade, CommittedReply(FromSeq + 2, ToSeq + 1));
  TestTrue(TEXT("The trade settles"), Harness.PumpUntil([&Result]() { return Result.Calls > 0; }));
  TestTrue(TEXT("It committed"), Result.Error.IsEmpty());
  TestFalse(
    TEXT("Its fence is its own commit, so the balance stays known"),
    From.PlayerStateComponent->RedwoodCharacter.bCurrencyBalancesUnknown
  );
  TestEqual(TEXT("The payer's balance"), GetCurrencyBalance(From.PlayerStateComponent->RedwoodCharacter.CurrencyBalances, GoldCurrency), FromBalance + LootAmount - TradedAmount);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyTradeTwoWayTest,
  "Redwood.Currency.Trade.TwoWaySwapIsOneRequest",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A swap of an item each way plus currency goes out as ONE request, each placement naming the side
// it leaves, and settles both sides from that one reply.
bool FRedwoodCurrencyTradeTwoWayTest::RunTest(const FString &Parameters) {
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
  FRedwoodTradeRootPlacement Given;
  Given.Id = OwnedItemId;
  Given.Domain = TEXT("general");
  Given.Slot = 2;
  FRedwoodTradeRootPlacement Received;
  Received.Id = PurchasedItemId;
  Received.Domain = TEXT("general");
  Received.Slot = 3;
  Received.FromCharacterId = ToId;
  FTradeResult Result;
  Harness.Subsystem->EmitItemsTrade(
    From.Component, To.Component, {Given, Received}, MakeTradeCurrency(TradedAmount, TradedAmount), TradeId,
    Capture(Result)
  );
  FRequest Trade;
  if (!Harness.ReadRequest(TradeEvent, Trade)) {
    return false;
  }
  const TArray<TSharedPtr<FJsonValue>> *Placements = nullptr;
  if (!TestTrue(TEXT("Both placements go out"), Trade.Payload->TryGetArrayField(TEXT("rootPlacements"), Placements) && Placements->Num() == 2)) {
    return false;
  }
  TestEqual(TEXT("The first item leaves the payer"), (*Placements)[0]->AsObject()->GetStringField(TEXT("fromCharacterId")), FromId);
  TestEqual(TEXT("The second item leaves the payee"), (*Placements)[1]->AsObject()->GetStringField(TEXT("fromCharacterId")), ToId);
  TestEqual(TEXT("The currency rides the same request"), CountArray(Trade.Payload, TEXT("currency")), 2);
  TestTrue(TEXT("No second request goes out"), Harness.IsQuiet());

  Harness.Answer(Trade, CommittedReply(FromSeq + 1, ToSeq + 1));
  TestTrue(TEXT("The trade settles"), Harness.PumpUntil([&Result]() { return Result.Calls > 0; }));
  TestTrue(TEXT("It committed"), Result.Error.IsEmpty());
  TestEqual(TEXT("The payer's lane takes the fence"), From.Component->GetLastCommittedItemSeq(), FromSeq + 1);
  TestEqual(TEXT("The payee's lane takes the fence"), To.Component->GetLastCommittedItemSeq(), ToSeq + 1);
  TestEqual(TEXT("The payer's balance"), GetCurrencyBalance(From.PlayerStateComponent->RedwoodCharacter.CurrencyBalances, GoldCurrency), FromBalance - TradedAmount);
  TestEqual(TEXT("The payee's balance"), GetCurrencyBalance(To.PlayerStateComponent->RedwoodCharacter.CurrencyBalances, GoldCurrency), ToBalance + TradedAmount);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyTradeOtherComponentUnconfirmedTest,
  "Redwood.Currency.Trade.OtherComponentUnconfirmedRefuses",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A batch whose outcome is unknown, on ANY component of the payer, refuses the trade: the probe that
// settles it may find it committed or not, so the balance before the trade is a guess. Once the
// probe settles it, the trade goes out from the committed balance.
bool FRedwoodCurrencyTradeOtherComponentUnconfirmedTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyTradeTest;
  AddExpectedErrorPlain(TEXT("Failed to flush items"), EAutomationExpectedErrorFlags::Contains, 1);
  AddExpectedErrorPlain(TEXT("has an item flush in flight or unconfirmed"), EAutomationExpectedErrorFlags::Contains, 1);
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
  ARedwoodCurrencyFlushTestActor *PawnActor = nullptr;
  URedwoodCharacterComponent *PawnComponent = Harness.MakeComponent(PawnActor, FromId);
  PawnComponent->QueueCurrency({MakeEntry(EntryIdA, LootAmount, FromBalance + LootAmount, LootReason)}, {});
  Harness.Subsystem->FlushItemsForCharacterComponent(From.PlayerStateComponent, PawnComponent);
  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }
  Harness.Answer(Flush, TEXT(R"({"error":"Item flush request timed out.","committedSeq":0,"timedOut":true})"));
  if (!TestTrue(
        TEXT("The loot batch is unconfirmed"),
        Harness.PumpUntil([PawnComponent]() { return PawnComponent->IsCurrencyBatchUnconfirmed(); })
      )) {
    return false;
  }

  FTradeResult Refused;
  Harness.Subsystem->EmitItemsTrade(
    From.Component, To.Component, {}, MakeTradeCurrency(TradedAmount, TradedAmount), TradeId, Capture(Refused)
  );
  TestEqual(TEXT("The trade is refused at once"), Refused.Calls, 1);
  TestTrue(TEXT("No trade leaves"), Harness.IsQuiet());

  // The probe finds the loot committed.
  Harness.Subsystem->FlushItemsForCharacterComponent(From.PlayerStateComponent, PawnComponent);
  FRequest Probe;
  if (!Harness.ReadRequest(FlushEvent, Probe)) {
    return false;
  }
  TestTrue(TEXT("The re-send is a probe"), Probe.Payload->HasField(TEXT("probe")));
  Harness.Answer(Probe, FString::Printf(TEXT(R"({"error":null,"committedSeq":%lld})"), FromSeq + 1));
  TestTrue(
    TEXT("The probe settles the batch"),
    Harness.PumpUntil([PawnComponent]() { return !PawnComponent->IsCurrencyBatchUnconfirmed(); })
  );

  FTradeResult Result;
  Harness.Subsystem->EmitItemsTrade(
    From.Component, To.Component, {}, MakeTradeCurrency(TradedAmount, TradedAmount), FreshTradeId, Capture(Result)
  );
  FRequest Trade;
  if (!Harness.ReadRequest(TradeEvent, Trade)) {
    return false;
  }
  TestEqual(TEXT("The trade is not refused now"), Result.Calls, 0);
  TestEqual(
    TEXT("It chains from the balance the probe confirmed"),
    TradeCurrencyField(Trade.Payload, 0, TEXT("balanceAfter")),
    FString(TEXT("750"))
  );
  // Settled here: a trade still open at teardown gives up with an Error, which fails the test.
  Harness.Answer(Trade, CommittedReply(FromSeq + 2, ToSeq + 1));
  TestTrue(TEXT("The trade settles"), Harness.PumpUntil([&Result]() { return Result.Calls > 0; }));
  TestTrue(TEXT("It committed"), Result.Error.IsEmpty());
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyTradeIdReusedTest,
  "Redwood.Currency.Trade.ReusedIdRefusal",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// "trade id reused with different contents" on a first send is a refusal: NOT applied, nobody
// freezes. On a resend it cannot happen, because a resend is the first send again; when it does, it
// is a bug, the earlier attempt's outcome is unknown, and both characters freeze.
bool FRedwoodCurrencyTradeIdReusedTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyTradeTest;
  AddExpectedErrorPlain(TEXT("Failed to trade items"), EAutomationExpectedErrorFlags::Contains, 3);
  AddExpectedErrorPlain(TEXT("a resend is the same request. This is a bug."), EAutomationExpectedErrorFlags::Contains, 1);
  AddExpectedErrorPlain(TEXT("Its outcome is UNKNOWN"), EAutomationExpectedErrorFlags::Contains, 1);
  // A staff line for each side's entry.
  AddExpectedErrorPlain(TEXT("outcome UNKNOWN (it may be saved): in trade"), EAutomationExpectedErrorFlags::Contains, 2);
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
  // The flag decides, so the text is one the plugin knows nothing of.
  const FString ReusedReply =
    TEXT(R"({"error":"reused for the test","fromCommittedSeq":-1,"toCommittedSeq":-1,"tradeIdReused":true})");

  FTradeResult FirstResult;
  FRequest First;
  if (!SendTrade(Harness, From, To, FirstResult, First)) {
    return false;
  }
  Harness.Answer(First, ReusedReply);
  TestTrue(TEXT("The first trade settles"), Harness.PumpUntil([&FirstResult]() { return FirstResult.Calls > 0; }));
  TestFalse(TEXT("It was refused"), FirstResult.Error.IsEmpty());
  TestFalse(TEXT("A refused first send freezes nobody"), Harness.Subsystem->IsCharacterFrozen(FromId));
  TestEqual(TEXT("And moves no seq"), From.Component->GetLastCommittedItemSeq(), FromSeq);

  FTradeResult SecondResult;
  Harness.Subsystem->EmitItemsTrade(
    From.Component, To.Component, {}, MakeTradeCurrency(TradedAmount, TradedAmount), FreshTradeId,
    Capture(SecondResult)
  );
  FRequest Second;
  if (!Harness.ReadRequest(TradeEvent, Second)) {
    return false;
  }
  Harness.Answer(Second, TimedOutReply());
  PumpFor(QuietWindowSeconds);
  FAccess::ResendTradeNow(*Harness.Subsystem, FreshTradeId);
  FRequest Resend;
  if (!Harness.ReadRequest(TradeEvent, Resend)) {
    return false;
  }
  Harness.Answer(Resend, ReusedReply);
  TestTrue(TEXT("The second trade settles"), Harness.PumpUntil([&SecondResult]() { return SecondResult.Calls > 0; }));
  TestFalse(TEXT("With an error"), SecondResult.Error.IsEmpty());
  TestTrue(TEXT("The payer is frozen"), Harness.Subsystem->IsCharacterFrozen(FromId));
  TestTrue(TEXT("The payee is frozen"), Harness.Subsystem->IsCharacterFrozen(ToId));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyTradeCommitUnknownEveryAttemptTest,
  "Redwood.Currency.Trade.CommitUnknownEveryAttemptGivesUp",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A realm that answers commitUnknown every time (the same throw on every send) is ended by the
// attempt cap: each commitUnknown uses an attempt, as a timeout does, and after TradeMaxAttempts the
// trade gives up, logs its entries and freezes both characters.
bool FRedwoodCurrencyTradeCommitUnknownEveryAttemptTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyTradeTest;
  const int32 MaxAttempts = URedwoodServerGameSubsystem::TradeMaxAttempts;
  AddExpectedErrorPlain(TEXT("Failed to trade items"), EAutomationExpectedErrorFlags::Contains, MaxAttempts);
  AddExpectedErrorPlain(TEXT("Its outcome is UNKNOWN"), EAutomationExpectedErrorFlags::Contains, 1);
  // A staff line for each side's entry.
  AddExpectedErrorPlain(TEXT("outcome UNKNOWN (it may be saved): in trade"), EAutomationExpectedErrorFlags::Contains, 2);
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
  const FString CommitUnknownReply =
    TEXT(R"({"error":"TypeError: x is undefined","fromCommittedSeq":-1,"toCommittedSeq":-1,"commitUnknown":true})");
  FTradeResult Result;
  FRequest Request;
  if (!SendTrade(Harness, From, To, Result, Request)) {
    return false;
  }
  for (int32 Attempt = 1; Attempt <= MaxAttempts; ++Attempt) {
    if (Attempt > 1) {
      TestEqual(TEXT("Not settled while attempts remain"), Result.Calls, 0);
      TestFalse(TEXT("Nobody is frozen yet"), Harness.Subsystem->IsCharacterFrozen(FromId));
      FAccess::ResendTradeNow(*Harness.Subsystem, TradeId);
      if (!Harness.ReadRequest(TradeEvent, Request)) {
        return false;
      }
    }
    const FRedwoodPendingItemTrade *Pending = FAccess::Trade(*Harness.Subsystem, TradeId);
    TestEqual(TEXT("Each send uses an attempt"), Pending ? Pending->Attempts : -1, Attempt);
    Harness.Answer(Request, CommitUnknownReply);
    Harness.PumpUntil([&Harness, &Result]() {
      const FRedwoodPendingItemTrade *Open = FAccess::Trade(*Harness.Subsystem, TradeId);
      return Result.Calls > 0 || (Open && Open->Generation == 0);
    });
  }
  TestEqual(TEXT("The trade gives up after the last attempt"), Result.Calls, 1);
  TestFalse(TEXT("With an error"), Result.Error.IsEmpty());
  TestTrue(TEXT("The payer is frozen"), Harness.Subsystem->IsCharacterFrozen(FromId));
  TestTrue(TEXT("The payee is frozen"), Harness.Subsystem->IsCharacterFrozen(ToId));
  TestTrue(TEXT("No further attempt goes out"), Harness.IsQuiet());
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyTradeReconnectClearsOpenFlushTest,
  "Redwood.Currency.Trade.ReconnectClearsOpenFlush",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A flush whose connection drops before its reply never gets one. It must not block the character's
// trades for the rest of the session: the reconnect turns it into a probe, and once the probe is
// answered the trade goes out.
bool FRedwoodCurrencyTradeReconnectClearsOpenFlushTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyTradeTest;
  AddExpectedErrorPlain(TEXT("has an item flush in flight"), EAutomationExpectedErrorFlags::Contains, 1);
  FSidecarHarness Harness(*this);
  if (!Harness.ConnectWithConnectCallback()) {
    return false;
  }
  FParty From;
  FParty To;
  if (!MakeParty(Harness, FromId, FromToken, FromSeq, FromBalance, From) ||
      !MakeParty(Harness, ToId, ToToken, ToSeq, ToBalance, To)) {
    return false;
  }
  ARedwoodCurrencyFlushTestActor *PawnActor = nullptr;
  URedwoodCharacterComponent *PawnComponent = Harness.MakeComponent(PawnActor, FromId);
  PawnComponent->QueueCurrency({MakeEntry(EntryIdA, LootAmount, FromBalance + LootAmount, LootReason)}, {});
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, PawnComponent);
  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }
  FTradeResult Refused;
  Harness.Subsystem->EmitItemsTrade(
    From.Component, To.Component, {}, MakeTradeCurrency(TradedAmount, TradedAmount), TradeId, Capture(Refused)
  );
  TestEqual(TEXT("While the flush is open, the trade is refused"), Refused.Calls, 1);

  // The connection drops with the flush unanswered, and comes back.
  TSharedPtr<FSocketIONative> Socket = Harness.SidecarCopy;
  if (!TestTrue(TEXT("The sidecar restarts"), Harness.Server->CloseWithCode(ServiceRestartCloseCode)) ||
      !TestTrue(TEXT("The socket sees it"), Harness.PumpUntil([Socket]() { return !Socket->bIsConnected; })) ||
      !Harness.AcceptAndWait()) {
    return false;
  }
  FRequest Probe;
  if (!Harness.ReadRequest(FlushEvent, Probe)) {
    return false;
  }
  Harness.Answer(Probe, FString::Printf(TEXT(R"({"error":null,"committedSeq":%lld})"), FromSeq + 1));
  Harness.PumpUntil([PawnComponent]() { return !PawnComponent->IsItemFlushInFlight(); });

  FTradeResult Result;
  FRequest Trade;
  if (!SendTrade(Harness, From, To, Result, Trade)) {
    return false;
  }
  TestEqual(TEXT("After the reconnect the trade goes out"), Result.Calls, 0);
  Harness.Answer(Trade, CommittedReply(FromSeq + 1, ToSeq + 1));
  TestTrue(TEXT("The trade settles"), Harness.PumpUntil([&Result]() { return Result.Calls > 0; }));
  TestTrue(TEXT("It committed"), Result.Error.IsEmpty());
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarReconnectProbesTest,
  "Redwood.Currency.Sidecar.ReconnectProbesOpenFlush",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// R34. The connection drops with a batch in flight, so its reply never comes. The reconnect marks
// the batch unconfirmed and re-sends it at once as a probe: the same entries, no rows, the same
// seq, carrying the attempt's settle. The probe's success drops the entries exactly once, and a
// late reply from the old connection changes nothing.
bool FRedwoodCurrencySidecarReconnectProbesTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  FSidecarHarness Harness(*this);
  if (!Harness.ConnectWithConnectCallback()) {
    return false;
  }
  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Component = Harness.MakeComponent(Actor);
  Actor->Items = {MakeRecord(OwnedItemId)};
  Component->MarkItemsDirty({OwnedItemId});
  Component->QueueCurrency({MakeEntry(EntryIdA, -250, 750, LootReason)}, {});
  int32 SettleCount = 0;
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component, [&SettleCount]() { ++SettleCount; });
  FRequest First;
  if (!Harness.ReadRequest(FlushEvent, First)) {
    return false;
  }
  const uint64 OldGeneration = FAccess::LastFlushGeneration(*Harness.Subsystem);
  TestEqual(TEXT("The first attempt carries the row"), CountArray(First.Payload, TEXT("upserts")), 1);

  TSharedPtr<FSocketIONative> Socket = Harness.SidecarCopy;
  if (!TestTrue(TEXT("The sidecar restarts"), Harness.Server->CloseWithCode(ServiceRestartCloseCode)) ||
      !TestTrue(TEXT("The socket sees it"), Harness.PumpUntil([Socket]() { return !Socket->bIsConnected; })) ||
      !Harness.AcceptAndWait()) {
    return false;
  }
  FRequest Probe;
  if (!Harness.ReadRequest(FlushEvent, Probe)) {
    return false;
  }
  TestTrue(TEXT("The re-send is a probe"), Probe.Payload->HasField(TEXT("probe")));
  TestEqual(TEXT("At the same seq"), Probe.Payload->GetNumberField(TEXT("batchSeq")), First.Payload->GetNumberField(TEXT("batchSeq")));
  TestEqual(TEXT("With no rows"), CountArray(Probe.Payload, TEXT("upserts")), 0);
  TestEqual(TEXT("With the same entries"), CountArray(Probe.Payload, TEXT("currency")), 1);
  TestEqual(TEXT("The same entry, unchanged"), FirstBalanceAfter(Probe.Payload), FirstBalanceAfter(First.Payload));
  TestEqual(TEXT("The attempt has not settled"), SettleCount, 0);

  // Staged after the probe went out: it must survive both replies.
  Component->QueueCurrency({MakeEntry(EntryIdB, 100, 850, LootReason)}, {});
  Harness.Answer(Probe, FString::Printf(TEXT(R"({"error":null,"committedSeq":%lld})"), SeededInventorySeq + 1));
  TestTrue(TEXT("The probe settles the attempt"), Harness.PumpUntil([&SettleCount]() { return SettleCount > 0; }));
  const TArray<FRedwoodCurrencyEntry> &Pending = Component->GetPendingCurrencyEntries();
  if (TestEqual(TEXT("The probed entry is dropped once"), Pending.Num(), 1)) {
    TestEqual(TEXT("The later entry stays"), Pending[0].Id, EntryIdB);
  }
  TestTrue(TEXT("The row the probe did not carry stays dirty"), Component->GetDirtyItemGenerations().Contains(OwnedItemId));

  // The old connection's reply, late: it would drop the later entry and clear the row.
  FAccess::ReplyToFlush(
    *Harness.Subsystem,
    OldGeneration,
    FString::Printf(TEXT(R"({"error":null,"committedSeq":%lld})"), SeededInventorySeq + 1)
  );
  TestEqual(TEXT("A late reply settles nothing"), SettleCount, 1);
  TestEqual(TEXT("A late reply drops nothing"), Component->GetPendingCurrencyEntries().Num(), 1);
  TestTrue(TEXT("A late reply clears no row"), Component->GetDirtyItemGenerations().Contains(OwnedItemId));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarShutdownOpenFlushTest,
  "Redwood.Currency.Sidecar.ShutdownSettlesOpenFlush",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A server that shuts down with a flush unanswered settles it, so a detached pawn's barrier ends,
// and logs each entry and key of it as UNKNOWN with the reuse-id rule, for staff.
bool FRedwoodCurrencySidecarShutdownOpenFlushTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  const FString RewardKey = TEXT("quest:Q1");
  AddExpectedErrorPlain(
    FString::Printf(
      TEXT("entry %s currency %s delta 500 reason %s, outcome UNKNOWN (it may be saved): in flight when the server shut down. A re-grant must reuse entry id %s"),
      *EntryIdA,
      *GoldCurrency,
      *LootReason,
      *EntryIdA
    ),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  AddExpectedErrorPlain(
    FString::Printf(TEXT("grant %s, outcome UNKNOWN (it may be saved): in flight when the server shut down"), *RewardKey),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  AddExpectedErrorPlain(
    FString::Printf(TEXT("left in flight item row %s (generation"), *OwnedItemId),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  AddExpectedErrorPlain(
    FString::Printf(TEXT("left in flight item row %s (generation"), *LaterItemId),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Component = Harness.MakeComponent(Actor);
  Component->QueueCurrency({MakeEntry(EntryIdA, 500, 1500, LootReason)}, {RewardKey});
  Actor->Items = {MakeRecord(OwnedItemId)};
  Component->MarkItemsDirty({OwnedItemId});
  Component->MarkItemsDeleted({LaterItemId});
  int32 SettleCount = 0;
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component, [&SettleCount]() { ++SettleCount; });
  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }
  TestEqual(TEXT("Open while unanswered"), SettleCount, 0);

  Harness.ShutDown();
  TestEqual(TEXT("Shutdown settles the open flush once"), SettleCount, 1);
  return true;
}

namespace RedwoodCurrencySidecarTest {
  // I3: after a rows-only batch whose outcome is unknown, the probe goes out empty, its replay ack
  // leaves the re-dirtied row dirty, and the row then goes out in a normal batch after the seq.
  bool ProbeThenSendRowsAfterSeq(
    FSidecarHarness &Harness, URedwoodCharacterComponent *Component, double SentSeq
  ) {
    Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component);
    FRequest Probe;
    if (!Harness.ReadRequest(FlushEvent, Probe)) {
      return false;
    }
    Harness.Test.TestTrue(TEXT("The re-send is a probe"), Probe.Payload->HasField(TEXT("probe")));
    Harness.Test.TestEqual(TEXT("At the unknown batch's seq"), Probe.Payload->GetNumberField(TEXT("batchSeq")), SentSeq);
    Harness.Test.TestEqual(TEXT("The probe carries no rows"), CountArray(Probe.Payload, TEXT("upserts")), 0);
    Harness.Test.TestEqual(TEXT("And no currency"), CountArray(Probe.Payload, TEXT("currency")), 0);

    // The original had committed: the probe's answer is a replay ack.
    Harness.Answer(Probe, FString::Printf(TEXT(R"({"error":null,"committedSeq":%lld})"), static_cast<int64>(SentSeq)));
    Harness.PumpUntil([Component]() { return !Component->IsItemFlushInFlight(); });
    Harness.Test.TestTrue(TEXT("The newer row stays dirty"), Component->GetDirtyItemGenerations().Contains(OwnedItemId));
    Harness.Test.TestFalse(TEXT("The seq is settled"), Component->IsCurrencyBatchUnconfirmed());

    Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component);
    FRequest Normal;
    if (!Harness.ReadRequest(FlushEvent, Normal)) {
      return false;
    }
    Harness.Test.TestFalse(TEXT("The next batch is a normal one"), Normal.Payload->HasField(TEXT("probe")));
    Harness.Test.TestEqual(TEXT("After the settled seq"), Normal.Payload->GetNumberField(TEXT("batchSeq")), SentSeq + 1);
    Harness.Test.TestEqual(TEXT("Carrying the newer row"), CountArray(Normal.Payload, TEXT("upserts")), 1);
    Harness.Answer(Normal, FString::Printf(TEXT(R"({"error":null,"committedSeq":%lld})"), static_cast<int64>(SentSeq) + 1));
    Harness.PumpUntil([Component]() { return !Component->IsItemFlushInFlight(); });
    Harness.Test.TestFalse(TEXT("The newer row is saved"), Component->GetDirtyItemGenerations().Contains(OwnedItemId));
    return true;
  }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarRowsOnlyReconnectTest,
  "Redwood.Items.Sidecar.RowsOnlyReconnectProbes",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A batch of item rows alone is in flight when the connection drops, and the row is changed again.
// The original commits after all. Re-sent as a normal batch at its seq it would be acked as a
// replay and the newer row cleared unsaved; as a probe, the newer row goes out after the seq.
bool FRedwoodCurrencySidecarRowsOnlyReconnectTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  FSidecarHarness Harness(*this);
  if (!Harness.ConnectWithConnectCallback()) {
    return false;
  }
  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Component = Harness.MakeComponent(Actor);
  Actor->Items = {MakeRecord(OwnedItemId)};
  Component->MarkItemsDirty({OwnedItemId});
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component);
  FRequest First;
  if (!Harness.ReadRequest(FlushEvent, First)) {
    return false;
  }
  const double SentSeq = First.Payload->GetNumberField(TEXT("batchSeq"));
  Component->MarkItemsDirty({OwnedItemId});

  TSharedPtr<FSocketIONative> Socket = Harness.SidecarCopy;
  if (!TestTrue(TEXT("The sidecar restarts"), Harness.Server->CloseWithCode(ServiceRestartCloseCode)) ||
      !TestTrue(TEXT("The socket sees it"), Harness.PumpUntil([Socket]() { return !Socket->bIsConnected; })) ||
      !Harness.AcceptAndWait()) {
    return false;
  }
  // The reconnect sends the probe itself.
  FRequest Probe;
  if (!Harness.ReadRequest(FlushEvent, Probe)) {
    return false;
  }
  TestTrue(TEXT("The reconnect re-sends a probe"), Probe.Payload->HasField(TEXT("probe")));
  TestEqual(TEXT("At the batch's seq"), Probe.Payload->GetNumberField(TEXT("batchSeq")), SentSeq);
  TestEqual(TEXT("With no rows"), CountArray(Probe.Payload, TEXT("upserts")), 0);
  Harness.Answer(Probe, FString::Printf(TEXT(R"({"error":null,"committedSeq":%lld})"), static_cast<int64>(SentSeq)));
  Harness.PumpUntil([Component]() { return !Component->IsItemFlushInFlight(); });
  TestTrue(TEXT("The newer row stays dirty"), Component->GetDirtyItemGenerations().Contains(OwnedItemId));

  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component);
  FRequest Normal;
  if (!Harness.ReadRequest(FlushEvent, Normal)) {
    return false;
  }
  TestFalse(TEXT("The next batch is a normal one"), Normal.Payload->HasField(TEXT("probe")));
  TestEqual(TEXT("After the settled seq"), Normal.Payload->GetNumberField(TEXT("batchSeq")), SentSeq + 1);
  TestEqual(TEXT("Carrying the newer row"), CountArray(Normal.Payload, TEXT("upserts")), 1);
  Harness.Answer(Normal, FString::Printf(TEXT(R"({"error":null,"committedSeq":%lld})"), static_cast<int64>(SentSeq) + 1));
  Harness.PumpUntil([Component]() { return !Component->IsItemFlushInFlight(); });
  TestFalse(TEXT("The newer row is saved"), Component->GetDirtyItemGenerations().Contains(OwnedItemId));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarRowsOnlyTimedOutTest,
  "Redwood.Items.Sidecar.RowsOnlyTimedOutProbes",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// The same case through a timedOut reply: the batch of rows alone may have applied, so the next
// attempt is an empty probe, and the newer row goes out after the seq it settles.
bool FRedwoodCurrencySidecarRowsOnlyTimedOutTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  AddExpectedErrorPlain(TEXT("Failed to flush items"), EAutomationExpectedErrorFlags::Contains, 1);
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Component = Harness.MakeComponent(Actor);
  Actor->Items = {MakeRecord(OwnedItemId)};
  Component->MarkItemsDirty({OwnedItemId});
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component);
  FRequest First;
  if (!Harness.ReadRequest(FlushEvent, First)) {
    return false;
  }
  const double SentSeq = First.Payload->GetNumberField(TEXT("batchSeq"));
  Component->MarkItemsDirty({OwnedItemId});
  Harness.Answer(First, TEXT(R"({"error":"Item flush request timed out.","committedSeq":-1,"timedOut":true})"));
  Harness.PumpUntil([Component]() { return !Component->IsItemFlushInFlight(); });
  TestTrue(TEXT("The rows-only batch is unconfirmed"), Component->IsCurrencyBatchUnconfirmed());
  return ProbeThenSendRowsAfterSeq(Harness, Component, SentSeq);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyTradeReconnectResendsTest,
  "Redwood.Currency.Trade.ReconnectResendsAtOnce",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A trade whose connection drops before its reply goes out again as soon as the sidecar is back,
// byte for byte, with both lanes still held; it does not wait for the reply timeout.
bool FRedwoodCurrencyTradeReconnectResendsTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyTradeTest;
  FSidecarHarness Harness(*this);
  if (!Harness.ConnectWithConnectCallback()) {
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
  TSharedPtr<FSocketIONative> Socket = Harness.SidecarCopy;
  if (!TestTrue(TEXT("The sidecar restarts"), Harness.Server->CloseWithCode(ServiceRestartCloseCode)) ||
      !TestTrue(TEXT("The socket sees it"), Harness.PumpUntil([Socket]() { return !Socket->bIsConnected; })) ||
      !Harness.AcceptAndWait()) {
    return false;
  }
  FRequest Resend;
  if (!Harness.ReadRequest(TradeEvent, Resend)) {
    return false;
  }
  TestEqual(TEXT("The resend is byte for byte the first send"), SerializePayload(Resend.Payload), SerializePayload(First.Payload));
  TestEqual(TEXT("The trade is not settled yet"), Result.Calls, 0);
  Harness.Answer(Resend, CommittedReply(FromSeq + 1, ToSeq + 1));
  TestTrue(TEXT("The trade settles"), Harness.PumpUntil([&Result]() { return Result.Calls > 0; }));
  TestTrue(TEXT("It committed"), Result.Error.IsEmpty());
  return true;
}

namespace RedwoodCurrencySidecarTest {
  // A player with its own controller, and the character component on its PlayerState: the
  // component reaches its player the way a live one does.
  struct FPlayerParty {
    APlayerController *PlayerController = nullptr;
    ARedwoodCurrencyLoadTestPlayerState *PlayerState = nullptr;
    URedwoodCharacterComponent *Component = nullptr;
  };

  bool MakePlayerParty(FSidecarHarness &Harness, const FString &InCharacterId, FPlayerParty &OutParty) {
    OutParty.PlayerController = Harness.World->SpawnActor<APlayerController>();
    if (!Harness.Test.TestNotNull(TEXT("The controller spawns"), OutParty.PlayerController)) {
      return false;
    }
    FActorSpawnParameters Params;
    Params.Owner = OutParty.PlayerController;
    OutParty.PlayerState = Harness.World->SpawnActor<ARedwoodCurrencyLoadTestPlayerState>(Params);
    if (!Harness.Test.TestNotNull(TEXT("The PlayerState spawns"), OutParty.PlayerState)) {
      return false;
    }
    OutParty.PlayerController->PlayerState = OutParty.PlayerState;
    OutParty.Component = NewObject<URedwoodCharacterComponent>(OutParty.PlayerState);
    OutParty.Component->bUseItems = true;
    OutParty.Component->RedwoodCharacterId = InCharacterId;
    OutParty.Component->SeedWriterToken(WriterToken);
    OutParty.Component->SeedItemSeqFromCharacter(SeededInventorySeq);
    return true;
  }

  // The kick goes through the authority game mode's game session, and a bare test world has
  // neither; the probe session counts the kicks (see ZoneTravelHardeningTest).
  ARedwoodKickProbeGameSession *InstallKickProbe(FSidecarHarness &Harness) {
    Harness.World->SetGameMode(FURL());
    AGameModeBase *GameMode = Harness.World->GetAuthGameMode();
    ARedwoodKickProbeGameSession *KickProbe =
      Harness.World->SpawnActor<ARedwoodKickProbeGameSession>();
    if (!Harness.Test.TestNotNull(TEXT("The world has a game mode"), GameMode) ||
        !Harness.Test.TestNotNull(TEXT("The kick probe spawns"), KickProbe)) {
      return nullptr;
    }
    GameMode->GameSession = KickProbe;
    return KickProbe;
  }

  // Stages a debit the character cannot pay, flushes it, and answers it as a final refusal.
  bool RefuseFlushForGood(FSidecarHarness &Harness, URedwoodCharacterComponent *Component) {
    Component->QueueCurrency({MakeEntry(EntryIdA, -2000, -1000, LootReason)}, {});
    Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component);
    FRequest Flush;
    return Harness.ReadRequest(FlushEvent, Flush) && Harness.Answer(Flush, FinalRefusalReply) &&
      Harness.PumpUntil([Component]() { return Component->IsFrozenAfterFinalRefusal(); });
  }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarDesyncKicksTest,
  "Redwood.Currency.Sidecar.DesyncKicksUnlessGameHandles",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// R35, R36: a lost writer fence and a final refusal kick the player, even with a listener bound,
// until the game says it handles them (bGameHandlesCurrencyDesync). A frozen or fenced character
// that stays in the world saves nothing, and an item it drops there can be picked up while the
// database still holds the original. While the server shuts down, nobody is kicked.
bool FRedwoodCurrencySidecarDesyncKicksTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  AddExpectedErrorPlain(TEXT("Failed to flush items"), EAutomationExpectedErrorFlags::Contains, 2);
  AddExpectedErrorPlain(
    TEXT("NOT saved: dropped by the freeze after a final refusal"), EAutomationExpectedErrorFlags::Contains, 2
  );
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodKickProbeGameSession *KickProbe = InstallKickProbe(Harness);
  FPlayerParty Fenced;
  FPlayerParty Refused;
  FPlayerParty FencedHandled;
  FPlayerParty RefusedHandled;
  FPlayerParty FencedAtShutdown;
  // One character each: a freeze is per character.
  if (!KickProbe || !MakePlayerParty(Harness, TEXT("character-fenced"), Fenced) ||
      !MakePlayerParty(Harness, TEXT("character-refused"), Refused) ||
      !MakePlayerParty(Harness, TEXT("character-fenced-handled"), FencedHandled) ||
      !MakePlayerParty(Harness, TEXT("character-refused-handled"), RefusedHandled) ||
      !MakePlayerParty(Harness, TEXT("character-fenced-shutdown"), FencedAtShutdown)) {
    return false;
  }
  int32 FenceLostCount = 0;
  int32 RefusalCount = 0;
  for (FPlayerParty *Party : {&Fenced, &Refused, &FencedHandled, &RefusedHandled}) {
    Party->Component->OnWriterFenceLost.AddLambda([&FenceLostCount](const FString &) { ++FenceLostCount; });
    Party->Component->OnItemFlushRefusedFinal.AddLambda(
      [&RefusalCount](const FString &, const FString &) { ++RefusalCount; }
    );
  }

  // The flag is off: a bound listener hears it, and the player is kicked all the same.
  Fenced.Component->MarkWriterFenceLost();
  TestTrue(
    TEXT("A lost fence kicks the player, listener or not"),
    Harness.PumpUntil([KickProbe]() { return KickProbe->KickCount == 1; })
  );
  TestEqual(TEXT("The listener hears the lost fence"), FenceLostCount, 1);
  TestEqual(
    TEXT("The kick says why"),
    KickProbe->LastKickReason,
    FString(URedwoodServerGameSubsystem::CurrencyDesyncKickMessage)
  );
  if (!RefuseFlushForGood(Harness, Refused.Component)) {
    return false;
  }
  TestTrue(
    TEXT("A final refusal kicks the player, listener or not"),
    Harness.PumpUntil([KickProbe]() { return KickProbe->KickCount == 2; })
  );
  TestEqual(TEXT("The listener hears the final refusal"), RefusalCount, 1);

  // The game says it handles both: no kick.
  Harness.Subsystem->bGameHandlesCurrencyDesync = true;
  FencedHandled.Component->MarkWriterFenceLost();
  if (!RefuseFlushForGood(Harness, RefusedHandled.Component)) {
    return false;
  }
  PumpFor(QuietWindowSeconds);
  TestEqual(TEXT("The game's listener hears the lost fence"), FenceLostCount, 2);
  TestEqual(TEXT("And the final refusal"), RefusalCount, 2);
  TestEqual(TEXT("A game that handles them kicks nobody"), KickProbe->KickCount, 2);

  // A server that is shutting down kicks nobody, even with the flag off.
  Harness.Subsystem->bGameHandlesCurrencyDesync = false;
  FAccess::SetShuttingDown(*Harness.Subsystem);
  FencedAtShutdown.Component->MarkWriterFenceLost();
  PumpFor(QuietWindowSeconds);
  TestEqual(TEXT("No kick while the server shuts down"), KickProbe->KickCount, 2);
  return true;
}

namespace RedwoodCurrencySidecarTest {
  int64 BatchSeqOfFlush(const TSharedPtr<FJsonObject> &Payload) {
    return Payload.IsValid() ? static_cast<int64>(Payload->GetNumberField(TEXT("batchSeq"))) : -1;
  }

  // A PlayerState with the snapshot of CharacterId at SeededInventorySeq.
  bool MakeSnapshotOwner(
    FSidecarHarness &Harness,
    ARedwoodCurrencyLoadTestPlayerState *&OutPlayerState,
    URedwoodPlayerStateComponent *&OutPlayerStateComponent
  ) {
    OutPlayerState = Harness.World->SpawnActor<ARedwoodCurrencyLoadTestPlayerState>();
    if (!Harness.Test.TestNotNull(TEXT("The PlayerState spawns"), OutPlayerState)) {
      return false;
    }
    OutPlayerStateComponent = NewObject<URedwoodPlayerStateComponent>(OutPlayerState);
    FRedwoodCharacterBackend &Snapshot = OutPlayerStateComponent->RedwoodCharacter;
    Snapshot.Id = CharacterId;
    Snapshot.InventorySeq = SeededInventorySeq;
    Snapshot.CurrencyBalances.Add(GoldCurrency, 1000);
    Snapshot.WriterToken = WriterToken;
    return true;
  }

  // A component on the PlayerState, hydrated from its snapshot the way a live one is (the handler
  // the PlayerState's OnRedwoodCharacterUpdated runs). Only the item channel: the others need data
  // these tests do not give.
  URedwoodCharacterComponent *HydrateOnPlayerState(
    ARedwoodCurrencyLoadTestPlayerState *PlayerState,
    TFunction<void(URedwoodCharacterComponent *)> BeforeHydrate = nullptr
  ) {
    URedwoodCharacterComponent *Component = NewObject<URedwoodCharacterComponent>(PlayerState);
    if (BeforeHydrate) {
      BeforeHydrate(Component);
    }
    Component->bUseCharacterCreatorData = false;
    Component->bUseMetadata = false;
    Component->bUseEquippedInventory = false;
    Component->bUseNonequippedInventory = false;
    Component->bUseData = false;
    Component->bUseItems = true;
    Component->ProcessEvent(
      Component->FindFunctionChecked(TEXT("RedwoodPlayerStateCharacterUpdated")), nullptr
    );
    return Component;
  }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarGoneUnknownProbesFirstTest,
  "Redwood.Currency.Sidecar.ComponentGoneUnknownProbesFirst",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// I3: the pawn respawns while a batch is in flight, and its reply says nothing about the outcome.
// The batch had committed. The respawned component must not send a normal batch at that seq: the
// backend would ack it as a replay and its row would be cleared unsaved. It probes the seq first,
// then reloads the balance the lost batch moved, and only then sends its row after the seq.
bool FRedwoodCurrencySidecarGoneUnknownProbesFirstTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  AddExpectedErrorPlain(
    TEXT("Failed to flush items (component gone): Item flush request timed out."), EAutomationExpectedErrorFlags::Contains, 1
  );
  AddExpectedErrorPlain(
    TEXT("entry AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA currency Currency.Gold delta 500 reason CurrencyReason.Loot, outcome UNKNOWN (it may be saved): its component is gone"),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  constexpr int64 SentSeq = SeededInventorySeq + 1;
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyLoadTestPlayerState *PlayerState = nullptr;
  URedwoodPlayerStateComponent *PlayerStateComponent = nullptr;
  if (!MakeSnapshotOwner(Harness, PlayerState, PlayerStateComponent)) {
    return false;
  }
  const FRedwoodCharacterBackend &Snapshot = PlayerStateComponent->RedwoodCharacter;

  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Sender = Harness.MakeComponent(Actor);
  Actor->Items = {MakeRecord(OwnedItemId)};
  Sender->MarkItemsDirty({OwnedItemId});
  Sender->QueueCurrency({MakeEntry(EntryIdA, 500, 1500, LootReason)}, {});
  Harness.Subsystem->FlushItemsForCharacterComponent(PlayerStateComponent, Sender);
  FRequest First;
  if (!Harness.ReadRequest(FlushEvent, First)) {
    return false;
  }
  Sender->MarkAsGarbage();
  Harness.Answer(First, TEXT(R"({"error":"Item flush request timed out.","committedSeq":-1,"timedOut":true})"));
  if (!TestTrue(
        TEXT("The unknown outcome reaches the snapshot"),
        Harness.PumpUntil([&Snapshot]() { return Snapshot.bInventorySeqUnconfirmed; })
      )) {
    return false;
  }
  TestEqual(TEXT("At the lost batch's seq"), Snapshot.UnconfirmedInventorySeq, SentSeq);
  TestEqual(TEXT("The committed seq is not guessed"), Snapshot.InventorySeq, SeededInventorySeq);
  TestTrue(TEXT("Nor the balance"), Snapshot.bCurrencyBalancesUnknown);

  // The respawned pawn's component, and a row it changes.
  URedwoodCharacterComponent *Respawned = HydrateOnPlayerState(PlayerState);
  TestTrue(TEXT("The respawned component starts with the probe"), Respawned->IsCurrencyBatchUnconfirmed());
  PlayerState->Items = {MakeRecord(LaterItemId)};
  Respawned->MarkItemsDirty({LaterItemId});

  Harness.Subsystem->FlushItemsForCharacterComponent(PlayerStateComponent, Respawned);
  FRequest Probe;
  if (!Harness.ReadRequest(FlushEvent, Probe)) {
    return false;
  }
  TestTrue(TEXT("Its first batch is a probe"), Probe.Payload->HasField(TEXT("probe")));
  TestEqual(TEXT("At the marked seq"), BatchSeqOfFlush(Probe.Payload), SentSeq);
  TestEqual(TEXT("With no rows"), CountArray(Probe.Payload, TEXT("upserts")), 0);
  TestEqual(TEXT("And no currency"), CountArray(Probe.Payload, TEXT("currency")), 0);

  // The lost batch had committed: a replay ack.
  Harness.Answer(Probe, FString::Printf(TEXT(R"({"error":null,"committedSeq":%lld})"), SentSeq));
  FRequest Load;
  if (!Harness.ReadRequest(LoadEvent, Load)) {
    return false;
  }
  TestFalse(TEXT("The settled seq clears the mark"), Snapshot.bInventorySeqUnconfirmed);
  TestEqual(TEXT("The snapshot takes the settled seq"), Snapshot.InventorySeq, SentSeq);
  TestTrue(TEXT("The row waits for the reload"), Respawned->GetDirtyItemGenerations().Contains(LaterItemId));
  Harness.Answer(Load, MakeLoadReply(TEXT("1500")));

  FRequest Normal;
  if (!Harness.ReadRequest(FlushEvent, Normal)) {
    return false;
  }
  TestFalse(TEXT("Then a normal batch"), Normal.Payload->HasField(TEXT("probe")));
  TestEqual(TEXT("After the settled seq"), BatchSeqOfFlush(Normal.Payload), SentSeq + 1);
  TestEqual(TEXT("With the row"), CountArray(Normal.Payload, TEXT("upserts")), 1);
  Harness.Answer(Normal, FString::Printf(TEXT(R"({"error":null,"committedSeq":%lld})"), SentSeq + 1));
  Harness.PumpUntil([Respawned]() { return !Respawned->IsItemFlushInFlight(); });
  TestFalse(TEXT("The row is saved"), Respawned->GetDirtyItemGenerations().Contains(LaterItemId));
  TestEqual(TEXT("The snapshot takes the reloaded balance"), GetCurrencyBalance(Snapshot.CurrencyBalances, GoldCurrency), (int64)1500);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarReconnectGoneComponentTest,
  "Redwood.Currency.Sidecar.ReconnectGoneComponentLogsAndMarks",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// I2: the connection drops with a batch in flight whose component is gone. Nothing is left to
// re-send, so the reconnect logs its entry and key for staff, as UNKNOWN, marks the seq on the
// snapshot, and settles the attempt.
bool FRedwoodCurrencySidecarReconnectGoneComponentTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  const FString RewardKey = TEXT("quest:Q1");
  AddExpectedErrorPlain(
    TEXT("entry AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA currency Currency.Gold delta 500 reason CurrencyReason.Loot, outcome UNKNOWN (it may be saved): its component is gone and its reply was lost with the connection"),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  AddExpectedErrorPlain(
    TEXT("grant quest:Q1, outcome UNKNOWN (it may be saved): its component is gone and its reply was lost with the connection"),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  FSidecarHarness Harness(*this);
  if (!Harness.ConnectWithConnectCallback()) {
    return false;
  }
  ARedwoodCurrencyLoadTestPlayerState *PlayerState = nullptr;
  URedwoodPlayerStateComponent *PlayerStateComponent = nullptr;
  if (!MakeSnapshotOwner(Harness, PlayerState, PlayerStateComponent)) {
    return false;
  }
  const FRedwoodCharacterBackend &Snapshot = PlayerStateComponent->RedwoodCharacter;
  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Sender = Harness.MakeComponent(Actor);
  Sender->QueueCurrency({MakeEntry(EntryIdA, 500, 1500, LootReason)}, {RewardKey});
  int32 SettleCount = 0;
  Harness.Subsystem->FlushItemsForCharacterComponent(
    PlayerStateComponent, Sender, [&SettleCount]() { ++SettleCount; }
  );
  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }
  Sender->MarkAsGarbage();

  TSharedPtr<FSocketIONative> Socket = Harness.SidecarCopy;
  if (!TestTrue(TEXT("The sidecar restarts"), Harness.Server->CloseWithCode(ServiceRestartCloseCode)) ||
      !TestTrue(TEXT("The socket sees it"), Harness.PumpUntil([Socket]() { return !Socket->bIsConnected; })) ||
      !Harness.AcceptAndWait()) {
    return false;
  }
  TestTrue(TEXT("The attempt settles"), Harness.PumpUntil([&SettleCount]() { return SettleCount > 0; }));
  TestEqual(TEXT("Once"), SettleCount, 1);
  TestTrue(TEXT("The seq is marked unconfirmed"), Snapshot.bInventorySeqUnconfirmed);
  TestEqual(TEXT("At the lost batch's seq"), Snapshot.UnconfirmedInventorySeq, SeededInventorySeq + 1);
  TestTrue(TEXT("The balance is unknown"), Snapshot.bCurrencyBalancesUnknown);
  TestTrue(TEXT("Nothing is re-sent for a component that is gone"), Harness.IsQuiet());
  return true;
}

namespace RedwoodCurrencySidecarTest {
  // A pawn whose component hydrated through the controller, on a PlayerState that holds the
  // snapshot (MakeSnapshotOwner). DetachForEndPlay then does what APawn::Destroyed does before
  // EndPlay: the pawn no longer reaches its controller.
  struct FPawnParty {
    APlayerController *PlayerController = nullptr;
    ARedwoodCurrencyLoadTestPlayerState *PlayerState = nullptr;
    URedwoodPlayerStateComponent *PlayerStateComponent = nullptr;
    ARedwoodCurrencyFlushTestPawn *Pawn = nullptr;
    URedwoodCharacterComponent *Component = nullptr;

    void DetachForEndPlay() {
      Pawn->Controller = nullptr;
    }
  };

  bool MakePawnParty(FSidecarHarness &Harness, FPawnParty &OutParty) {
    if (!MakeSnapshotOwner(Harness, OutParty.PlayerState, OutParty.PlayerStateComponent)) {
      return false;
    }
    OutParty.PlayerController = Harness.World->SpawnActor<APlayerController>();
    OutParty.Pawn = Harness.World->SpawnActor<ARedwoodCurrencyFlushTestPawn>();
    if (!Harness.Test.TestNotNull(TEXT("The controller spawns"), OutParty.PlayerController) ||
        !Harness.Test.TestNotNull(TEXT("The pawn spawns"), OutParty.Pawn)) {
      return false;
    }
    OutParty.PlayerController->PlayerState = OutParty.PlayerState;
    OutParty.Pawn->Controller = OutParty.PlayerController;
    OutParty.Component = NewObject<URedwoodCharacterComponent>(OutParty.Pawn);
    OutParty.Component->bUseCharacterCreatorData = false;
    OutParty.Component->bUseMetadata = false;
    OutParty.Component->bUseEquippedInventory = false;
    OutParty.Component->bUseNonequippedInventory = false;
    OutParty.Component->bUseData = false;
    OutParty.Component->bUseItems = true;
    OutParty.Component->ProcessEvent(
      OutParty.Component->FindFunctionChecked(TEXT("RedwoodPlayerStateCharacterUpdated")), nullptr
    );
    return Harness.Test.TestEqual(
      TEXT("The pawn's component hydrates the character"), OutParty.Component->RedwoodCharacterId, CharacterId
    );
  }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarEndPlayHandsOffTest,
  "Redwood.Currency.Sidecar.EndPlayHandsOffUnsent",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// R26: the pawn ends (a respawn) with an entry and a key staged and never sent. They are logged,
// handed to the PlayerState, and the next component of the character takes them, holds them for a
// reload, and sends them re-chained from the reloaded balance.
bool FRedwoodCurrencySidecarEndPlayHandsOffTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  const FString RewardKey = TEXT("quest:Q1");
  AddExpectedErrorPlain(
    TEXT("entry AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA currency Currency.Gold delta 500 reason CurrencyReason.Loot, NOT saved: its component ended, and they are handed to the PlayerState"),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  AddExpectedErrorPlain(
    TEXT("grant quest:Q1, NOT saved: its component ended, and they are handed to the PlayerState"),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  FSidecarHarness Harness(*this);
  FPawnParty Party;
  if (!Harness.Connect() || !MakePawnParty(Harness, Party)) {
    return false;
  }
  const FRedwoodCharacterBackend &Snapshot = Party.PlayerStateComponent->RedwoodCharacter;
  TestTrue(
    TEXT("The pawn stages its reward"),
    Party.Component->QueueCurrency({MakeEntry(EntryIdA, 500, 1500, LootReason)}, {RewardKey})
  );

  Party.DetachForEndPlay();
  Party.Component->HandOffCurrencyAtEndPlay(EEndPlayReason::Destroyed);
  TestEqual(TEXT("The entry is handed to the snapshot"), Snapshot.HandedOffCurrencyEntries.Num(), 1);
  TestEqual(TEXT("And the key"), Snapshot.HandedOffGrantKeys.Num(), 1);
  TestEqual(TEXT("The ended component keeps nothing"), Party.Component->GetPendingCurrencyEntries().Num(), 0);
  // Before any component takes it, the handed-off key already counts as used.
  URedwoodCharacterComponent *NotYetAdopted = NewObject<URedwoodCharacterComponent>(Party.PlayerState);
  NotYetAdopted->RedwoodCharacterId = CharacterId;
  TestTrue(TEXT("A handed-off key is used before it is taken"), NotYetAdopted->HasGrantKey(RewardKey));

  // The respawned component takes them, and holds them for a reload.
  URedwoodCharacterComponent *Respawned = HydrateOnPlayerState(Party.PlayerState);
  const TArray<FRedwoodCurrencyEntry> &Pending = Respawned->GetPendingCurrencyEntries();
  if (!TestEqual(TEXT("The respawned component takes the entry"), Pending.Num(), 1)) {
    return false;
  }
  TestEqual(TEXT("It is the same entry"), Pending[0].Id, EntryIdA);
  TestTrue(TEXT("And the key"), Respawned->HasGrantKey(RewardKey));
  TestEqual(TEXT("The snapshot gives them once"), Snapshot.HandedOffCurrencyEntries.Num(), 0);
  TestTrue(TEXT("They wait for a reload"), Respawned->IsCurrencyRebaseRequired());

  Harness.Subsystem->FlushItemsForCharacterComponent(Party.PlayerStateComponent, Respawned);
  FRequest Load;
  if (!Harness.ReadRequest(LoadEvent, Load)) {
    return false;
  }
  // Something else moved the balance meanwhile.
  Harness.Answer(Load, MakeLoadReply(TEXT("1200")));
  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }
  TestEqual(TEXT("The entry goes out"), CountArray(Flush.Payload, TEXT("currency")), 1);
  TestEqual(TEXT("Re-chained from the reloaded balance"), FirstBalanceAfter(Flush.Payload), FString(TEXT("1700")));
  TestEqual(TEXT("With its key"), CountArray(Flush.Payload, TEXT("grants")), 1);
  Harness.Answer(Flush, FString::Printf(TEXT(R"({"error":null,"committedSeq":%lld})"), SeededInventorySeq + 1));
  Harness.PumpUntil([Respawned]() { return !Respawned->IsItemFlushInFlight(); });
  TestEqual(TEXT("The handed-off entry is saved"), Respawned->GetPendingCurrencyEntries().Num(), 0);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarEndPlayInFlightTest,
  "Redwood.Currency.Sidecar.EndPlayInFlightLogsOnly",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// R26: the pawn ends with a batch in flight and an entry staged behind it. The staged entry's chain
// waits on that batch, so it is not handed off: it is logged as NOT saved, with why. The batch's
// reply, which no longer reaches the ended component, settles it as a gone component's: an unknown
// outcome is logged as UNKNOWN and marked on the snapshot.
bool FRedwoodCurrencySidecarEndPlayInFlightTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  AddExpectedErrorPlain(
    TEXT("entry BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB currency Currency.Gold delta 200 reason CurrencyReason.Loot, NOT saved: its component ended and they cannot be handed on: a batch was in flight"),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  AddExpectedErrorPlain(
    TEXT("Failed to flush items (component gone): Item flush request timed out."), EAutomationExpectedErrorFlags::Contains, 1
  );
  AddExpectedErrorPlain(
    TEXT("entry AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA currency Currency.Gold delta 500 reason CurrencyReason.Loot, outcome UNKNOWN (it may be saved): its component is gone"),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  FSidecarHarness Harness(*this);
  FPawnParty Party;
  if (!Harness.Connect() || !MakePawnParty(Harness, Party)) {
    return false;
  }
  const FRedwoodCharacterBackend &Snapshot = Party.PlayerStateComponent->RedwoodCharacter;
  Party.Component->QueueCurrency({MakeEntry(EntryIdA, 500, 1500, LootReason)}, {});
  int32 SettleCount = 0;
  Harness.Subsystem->FlushItemsForCharacterComponent(
    Party.PlayerStateComponent, Party.Component, [&SettleCount]() { ++SettleCount; }
  );
  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }
  Party.Component->QueueCurrency({MakeEntry(EntryIdB, 200, 1700, LootReason)}, {});

  Party.DetachForEndPlay();
  Party.Component->HandOffCurrencyAtEndPlay(EEndPlayReason::Destroyed);
  TestEqual(TEXT("Nothing is handed off behind an open batch"), Snapshot.HandedOffCurrencyEntries.Num(), 0);
  TestTrue(TEXT("The ended component never opens a second batch"), Party.Component->IsItemFlushInFlight());
  // N1: marked at the detach, before any reply.
  TestTrue(TEXT("The open batch's seq is marked at once"), Snapshot.bInventorySeqUnconfirmed);
  TestEqual(TEXT("At its seq"), Snapshot.UnconfirmedInventorySeq, SeededInventorySeq + 1);

  // The respawn comes before the old batch's reply: its first frame is a probe at that seq, never a
  // normal batch there.
  URedwoodCharacterComponent *Respawned = HydrateOnPlayerState(Party.PlayerState);
  Harness.Subsystem->FlushItemsForCharacterComponent(Party.PlayerStateComponent, Respawned);
  FRequest Probe;
  if (!Harness.ReadRequest(FlushEvent, Probe)) {
    return false;
  }
  TestTrue(TEXT("The respawned component probes first"), Probe.Payload->HasField(TEXT("probe")));
  TestEqual(TEXT("At the open batch's seq"), BatchSeqOfFlush(Probe.Payload), SeededInventorySeq + 1);

  Harness.Answer(Flush, TEXT(R"({"error":"Item flush request timed out.","committedSeq":-1,"timedOut":true})"));
  TestTrue(TEXT("The reply settles the attempt"), Harness.PumpUntil([&SettleCount]() { return SettleCount > 0; }));
  TestTrue(TEXT("Its unknown outcome stays marked"), Snapshot.bInventorySeqUnconfirmed);

  // The old batch had committed: the probe gets the replay ack, and the balance is reloaded.
  Harness.Answer(Probe, FString::Printf(TEXT(R"({"error":null,"committedSeq":%lld})"), SeededInventorySeq + 1));
  FRequest Load;
  if (!Harness.ReadRequest(LoadEvent, Load)) {
    return false;
  }
  TestFalse(TEXT("The settled seq clears the mark"), Snapshot.bInventorySeqUnconfirmed);
  Harness.Answer(Load, MakeLoadReply(TEXT("1500")));
  TestTrue(
    TEXT("The reload lands"),
    Harness.PumpUntil([&Harness]() { return FAccess::Reload(*Harness.Subsystem, CharacterId) == nullptr; })
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarEndPlayUnconfirmedTest,
  "Redwood.Currency.Sidecar.EndPlayHandsOffUnconfirmed",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// The pawn ends with a batch whose outcome is unknown. Its entry and key go to the PlayerState with
// the batch's seq, and the next component probes them there. When the probe says the batch never
// applied, the entry and the key are pending again and are saved: without them the probe would
// carry nothing, and that money would be lost.
bool FRedwoodCurrencySidecarEndPlayUnconfirmedTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  const FString RewardKey = TEXT("quest:Q1");
  AddExpectedError(TEXT("Failed to flush items"), EAutomationExpectedErrorFlags::Contains, 0);
  AddExpectedErrorPlain(
    TEXT("outcome UNKNOWN (it may be saved): its component ended with the batch unconfirmed"),
    EAutomationExpectedErrorFlags::Contains,
    2
  );
  FSidecarHarness Harness(*this);
  FPawnParty Party;
  if (!Harness.Connect() || !MakePawnParty(Harness, Party)) {
    return false;
  }
  const FRedwoodCharacterBackend &Snapshot = Party.PlayerStateComponent->RedwoodCharacter;
  Party.Component->QueueCurrency({MakeEntry(EntryIdA, 500, 1500, LootReason)}, {RewardKey});
  Harness.Subsystem->FlushItemsForCharacterComponent(Party.PlayerStateComponent, Party.Component);
  FRequest First;
  if (!Harness.ReadRequest(FlushEvent, First)) {
    return false;
  }
  Harness.Answer(First, TEXT(R"({"error":"Item flush request timed out.","committedSeq":-1,"timedOut":true})"));
  Harness.PumpUntil([&Party]() { return !Party.Component->IsItemFlushInFlight(); });
  if (!TestTrue(TEXT("The batch is unconfirmed"), Party.Component->IsCurrencyBatchUnconfirmed())) {
    return false;
  }

  Party.DetachForEndPlay();
  Party.Component->HandOffCurrencyAtEndPlay(EEndPlayReason::Destroyed);
  TestEqual(TEXT("The unconfirmed entry is handed to the snapshot"), Snapshot.HandedOffCurrencyEntries.Num(), 1);
  TestEqual(TEXT("And its key"), Snapshot.HandedOffGrantKeys.Num(), 1);
  TestTrue(TEXT("Its seq is marked"), Snapshot.bInventorySeqUnconfirmed);

  URedwoodCharacterComponent *Respawned = HydrateOnPlayerState(Party.PlayerState);
  Harness.Subsystem->FlushItemsForCharacterComponent(Party.PlayerStateComponent, Respawned);
  FRequest Probe;
  if (!Harness.ReadRequest(FlushEvent, Probe)) {
    return false;
  }
  TestTrue(TEXT("The respawned component probes first"), Probe.Payload->HasField(TEXT("probe")));
  TestEqual(TEXT("At the batch's seq"), BatchSeqOfFlush(Probe.Payload), SeededInventorySeq + 1);
  TestEqual(TEXT("With the batch's entry"), CountArray(Probe.Payload, TEXT("currency")), 1);
  TestEqual(TEXT("And its key"), CountArray(Probe.Payload, TEXT("grants")), 1);

  Harness.Answer(
    Probe,
    FString::Printf(
      TEXT(R"({"error":"probe not applied: batch seq consumed","committedSeq":%lld,"notApplied":true})"),
      SeededInventorySeq + 1
    )
  );
  Harness.PumpUntil([Respawned]() { return !Respawned->IsItemFlushInFlight(); });
  TestEqual(TEXT("The entry is pending again"), Respawned->GetPendingCurrencyEntries().Num(), 1);
  TestTrue(TEXT("And the key"), Respawned->HasGrantKey(RewardKey));

  Harness.Subsystem->FlushItemsForCharacterComponent(Party.PlayerStateComponent, Respawned);
  FRequest Load;
  if (!Harness.ReadRequest(LoadEvent, Load)) {
    return false;
  }
  Harness.Answer(Load, MakeLoadReply(TEXT("1000")));
  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }
  TestFalse(TEXT("A normal batch"), Flush.Payload->HasField(TEXT("probe")));
  TestEqual(TEXT("After the consumed seq"), BatchSeqOfFlush(Flush.Payload), SeededInventorySeq + 2);
  TestEqual(TEXT("The entry goes out"), CountArray(Flush.Payload, TEXT("currency")), 1);
  TestEqual(TEXT("With its key"), CountArray(Flush.Payload, TEXT("grants")), 1);
  Harness.Answer(Flush, FString::Printf(TEXT(R"({"error":null,"committedSeq":%lld})"), SeededInventorySeq + 2));
  Harness.PumpUntil([Respawned]() { return !Respawned->IsItemFlushInFlight(); });
  TestEqual(TEXT("The entry is saved"), Respawned->GetPendingCurrencyEntries().Num(), 0);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarEndPlayKeepsBatchWholeTest,
  "Redwood.Currency.Sidecar.EndPlayHandsOffUnconfirmedWithItems",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// As EndPlayHandsOffUnconfirmed, but the batch also carries the reward's item row. That row ends
// with the pawn: the next component hydrates from the snapshot, which never had it. So the entry and
// the key must not be handed on either. Sent alone after a notApplied probe, the key would commit
// without the item and block the reward forever.
bool FRedwoodCurrencySidecarEndPlayKeepsBatchWholeTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  const FString RewardKey = TEXT("quest:Q1:0");
  AddExpectedError(TEXT("Failed to flush items"), EAutomationExpectedErrorFlags::Contains, 0);
  AddExpectedErrorPlain(
    TEXT("outcome UNKNOWN (it may be saved): its component ended with the batch unconfirmed"),
    EAutomationExpectedErrorFlags::Contains,
    2
  );
  FSidecarHarness Harness(*this);
  FPawnParty Party;
  if (!Harness.Connect() || !MakePawnParty(Harness, Party)) {
    return false;
  }
  const FRedwoodCharacterBackend &Snapshot = Party.PlayerStateComponent->RedwoodCharacter;
  Party.Pawn->Items = {MakeRecord(PurchasedItemId)};
  Party.Component->MarkItemsDirty({PurchasedItemId});
  Party.Component->QueueCurrency({MakeEntry(EntryIdA, 500, 1500, LootReason)}, {RewardKey});
  Harness.Subsystem->FlushItemsForCharacterComponent(Party.PlayerStateComponent, Party.Component);
  FRequest First;
  if (!Harness.ReadRequest(FlushEvent, First)) {
    return false;
  }
  TestEqual(TEXT("The batch carries the reward's row"), CountArray(First.Payload, TEXT("upserts")), 1);
  Harness.Answer(First, TEXT(R"({"error":"Item flush request timed out.","committedSeq":-1,"timedOut":true})"));
  Harness.PumpUntil([&Party]() { return !Party.Component->IsItemFlushInFlight(); });
  if (!TestTrue(TEXT("The batch is unconfirmed"), Party.Component->IsCurrencyBatchUnconfirmed())) {
    return false;
  }

  Party.DetachForEndPlay();
  Party.Component->HandOffCurrencyAtEndPlay(EEndPlayReason::Destroyed);
  TestEqual(TEXT("The entry is not handed on without its row"), Snapshot.HandedOffCurrencyEntries.Num(), 0);
  TestEqual(TEXT("Nor the key"), Snapshot.HandedOffGrantKeys.Num(), 0);
  TestTrue(TEXT("Its seq is still marked"), Snapshot.bInventorySeqUnconfirmed);

  URedwoodCharacterComponent *Respawned = HydrateOnPlayerState(Party.PlayerState);
  TestFalse(TEXT("The key is not used"), Respawned->HasGrantKey(RewardKey));
  Harness.Subsystem->FlushItemsForCharacterComponent(Party.PlayerStateComponent, Respawned);
  FRequest Probe;
  if (!Harness.ReadRequest(FlushEvent, Probe)) {
    return false;
  }
  TestTrue(TEXT("The respawned component still probes the seq"), Probe.Payload->HasField(TEXT("probe")));
  TestEqual(TEXT("With no entry"), CountArray(Probe.Payload, TEXT("currency")), 0);
  TestEqual(TEXT("And no key"), CountArray(Probe.Payload, TEXT("grants")), 0);
  Harness.Answer(
    Probe,
    FString::Printf(
      TEXT(R"({"error":"probe not applied: batch seq consumed","committedSeq":%lld,"notApplied":true})"),
      SeededInventorySeq + 1
    )
  );
  Harness.PumpUntil([Respawned]() { return !Respawned->IsItemFlushInFlight(); });
  TestEqual(TEXT("Nothing is pending"), Respawned->GetPendingCurrencyEntries().Num(), 0);
  TestFalse(TEXT("The key is still free for the reward to pay again"), Respawned->HasGrantKey(RewardKey));

  // The batch may have moved the balance, so the next flush reloads it before anything else goes out.
  Harness.Subsystem->FlushItemsForCharacterComponent(Party.PlayerStateComponent, Respawned);
  FRequest Load;
  if (!Harness.ReadRequest(LoadEvent, Load)) {
    return false;
  }
  Harness.Answer(Load, MakeLoadReply(TEXT("1000")));
  TestTrue(
    TEXT("The reload lands"),
    Harness.PumpUntil([&Harness]() { return FAccess::Reload(*Harness.Subsystem, CharacterId) == nullptr; })
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarRehydrateResetsTest,
  "Redwood.Currency.Sidecar.RehydrateForAnotherCharacterResets",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// I6: a component reused for another character carries nothing of the first one into it. What the
// first one left (an unconfirmed entry and a staged one, behind a lost writer fence) is logged
// under the first character's id and dropped, and the component sends the second character's
// batch as an ordinary one, from that character's seq and token.
bool FRedwoodCurrencySidecarRehydrateResetsTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  const FString SecondCharacterId = TEXT("character-2");
  const FString SecondToken = TEXT("writer-token-2");
  constexpr int64 SecondSeq = 9;
  AddExpectedErrorPlain(TEXT("Failed to flush items"), EAutomationExpectedErrorFlags::Contains, 1);
  // The lost fence logs both first, then the reset logs both again under the first character.
  AddExpectedErrorPlain(TEXT("outcome UNKNOWN (it may be saved): writer fence lost"), EAutomationExpectedErrorFlags::Contains, 1);
  AddExpectedErrorPlain(TEXT("NOT saved: writer fence lost"), EAutomationExpectedErrorFlags::Contains, 1);
  AddExpectedErrorPlain(
    TEXT("character character-1 entry AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA currency Currency.Gold delta 500 reason CurrencyReason.Loot, outcome UNKNOWN (it may be saved): its component now plays character character-2"),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  AddExpectedErrorPlain(
    TEXT("character character-1 entry BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB currency Currency.Gold delta 200 reason CurrencyReason.Loot, NOT saved: its component now plays character character-2"),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyLoadTestPlayerState *PlayerState = nullptr;
  URedwoodPlayerStateComponent *PlayerStateComponent = nullptr;
  if (!MakeSnapshotOwner(Harness, PlayerState, PlayerStateComponent)) {
    return false;
  }
  URedwoodCharacterComponent *Component = HydrateOnPlayerState(PlayerState);

  Component->QueueCurrency({MakeEntry(EntryIdA, 500, 1500, LootReason)}, {});
  Harness.Subsystem->FlushItemsForCharacterComponent(PlayerStateComponent, Component);
  FRequest First;
  if (!Harness.ReadRequest(FlushEvent, First)) {
    return false;
  }
  Harness.Answer(First, TEXT(R"({"error":"Item flush request timed out.","committedSeq":-1,"timedOut":true})"));
  Harness.PumpUntil([Component]() { return !Component->IsItemFlushInFlight(); });
  Component->QueueCurrency({MakeEntry(EntryIdB, 200, 1700, LootReason)}, {});
  Component->MarkWriterFenceLost();
  if (!TestTrue(TEXT("The first character's batch is unconfirmed"), Component->IsCurrencyBatchUnconfirmed()) ||
      !TestTrue(TEXT("And its fence is lost"), Component->IsWriterFenceLost())) {
    return false;
  }

  // The PlayerState moves to another character, and the component hydrates for it.
  FRedwoodCharacterBackend Second;
  Second.Id = SecondCharacterId;
  Second.InventorySeq = SecondSeq;
  Second.CurrencyBalances.Add(GoldCurrency, 300);
  Second.WriterToken = SecondToken;
  PlayerStateComponent->RedwoodCharacter = Second;
  Component->ProcessEvent(Component->FindFunctionChecked(TEXT("RedwoodPlayerStateCharacterUpdated")), nullptr);

  TestEqual(TEXT("The component plays the second character"), Component->RedwoodCharacterId, SecondCharacterId);
  TestEqual(TEXT("Nothing of the first one is staged"), Component->GetPendingCurrencyEntries().Num(), 0);
  TestFalse(TEXT("No batch is unconfirmed"), Component->IsCurrencyBatchUnconfirmed());
  TestFalse(TEXT("No rebase is held"), Component->IsCurrencyRebaseRequired());
  TestFalse(TEXT("The fence is the new session's"), Component->IsWriterFenceLost());
  TestFalse(TEXT("Nothing is frozen"), Component->IsFrozenAfterFinalRefusal());

  TestTrue(
    TEXT("The second character takes currency"),
    Component->QueueCurrency({MakeEntry(CreditEntryId, 25, 325, SellReason)}, {})
  );
  Harness.Subsystem->FlushItemsForCharacterComponent(PlayerStateComponent, Component);
  FRequest Next;
  if (!Harness.ReadRequest(FlushEvent, Next)) {
    return false;
  }
  TestFalse(TEXT("An ordinary batch, not the first character's probe"), Next.Payload->HasField(TEXT("probe")));
  TestEqual(TEXT("For the second character"), Next.Payload->GetStringField(TEXT("characterId")), SecondCharacterId);
  TestEqual(TEXT("From its seq"), BatchSeqOfFlush(Next.Payload), SecondSeq + 1);
  TestEqual(TEXT("With its token"), Next.Payload->GetStringField(TEXT("writerToken")), SecondToken);
  TestEqual(TEXT("Carrying only its entry"), CountArray(Next.Payload, TEXT("currency")), 1);
  Harness.Answer(Next, FString::Printf(TEXT(R"({"error":null,"committedSeq":%lld})"), SecondSeq + 1));
  Harness.PumpUntil([Component]() { return !Component->IsItemFlushInFlight(); });
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarFlushReplyTimeoutTest,
  "Redwood.Currency.Sidecar.FlushReplyTimeoutProbes",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// M5: a sidecar that hangs with its connection open never answers a flush. When the reply timer
// fires, the batch becomes unconfirmed, the slot is released and the probe goes out at once, with
// the attempt's settle. The timed-out send's reply, when it comes after all, is ignored.
bool FRedwoodCurrencySidecarFlushReplyTimeoutTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  AddExpectedErrorPlain(TEXT("got no reply in 30 seconds"), EAutomationExpectedErrorFlags::Contains, 1);
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Component = Harness.MakeComponent(Actor);
  Actor->Items = {MakeRecord(OwnedItemId)};
  Component->MarkItemsDirty({OwnedItemId});
  Component->QueueCurrency({MakeEntry(EntryIdA, 500, 1500, LootReason)}, {});
  int32 SettleCount = 0;
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component, [&SettleCount]() { ++SettleCount; });
  FRequest First;
  if (!Harness.ReadRequest(FlushEvent, First)) {
    return false;
  }
  const int64 SentSeq = BatchSeqOfFlush(First.Payload);

  FAccess::TimeOutFlush(*Harness.Subsystem, FAccess::LastFlushGeneration(*Harness.Subsystem));
  FRequest Probe;
  if (!Harness.ReadRequest(FlushEvent, Probe)) {
    return false;
  }
  TestTrue(TEXT("The timeout probes the batch"), Probe.Payload->HasField(TEXT("probe")));
  TestEqual(TEXT("At its seq"), BatchSeqOfFlush(Probe.Payload), SentSeq);
  TestEqual(TEXT("With its entry"), CountArray(Probe.Payload, TEXT("currency")), 1);
  TestEqual(TEXT("And no rows"), CountArray(Probe.Payload, TEXT("upserts")), 0);
  TestEqual(TEXT("The settle waits for the probe"), SettleCount, 0);

  // The timed-out send's reply comes after all. It is ignored: the probe decides.
  Harness.Answer(First, FString::Printf(TEXT(R"({"error":null,"committedSeq":%lld})"), SentSeq));
  PumpFor(QuietWindowSeconds);
  TestTrue(TEXT("The late reply leaves the probe in flight"), Component->IsItemFlushInFlight());
  TestEqual(TEXT("And the entry staged"), Component->GetPendingCurrencyEntries().Num(), 1);
  TestEqual(TEXT("And settles nothing"), SettleCount, 0);

  Harness.Answer(Probe, FString::Printf(TEXT(R"({"error":null,"committedSeq":%lld})"), SentSeq));
  TestTrue(TEXT("The probe's reply settles the attempt"), Harness.PumpUntil([&SettleCount]() { return SettleCount > 0; }));
  TestEqual(TEXT("Once"), SettleCount, 1);
  TestEqual(TEXT("The entry is saved"), Component->GetPendingCurrencyEntries().Num(), 0);
  TestTrue(TEXT("The row stays dirty for a normal batch"), Component->GetDirtyItemGenerations().Contains(OwnedItemId));

  // Answered, so no flush is open at teardown.
  Harness.Subsystem->FlushItemsForCharacterComponent(nullptr, Component);
  FRequest Normal;
  if (!Harness.ReadRequest(FlushEvent, Normal)) {
    return false;
  }
  TestEqual(TEXT("The row goes out after the seq"), BatchSeqOfFlush(Normal.Payload), SentSeq + 1);
  Harness.Answer(Normal, FString::Printf(TEXT(R"({"error":null,"committedSeq":%lld})"), SentSeq + 1));
  Harness.PumpUntil([Component]() { return !Component->IsItemFlushInFlight(); });
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencySidecarGoneFinalRefusalFreezesTest,
  "Redwood.Currency.Sidecar.ComponentGoneFinalRefusalFreezes",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A batch refused for good after its component is gone: the refused entry is logged as NOT saved,
// and the character freezes, snapshot and all, so the respawned component starts frozen instead of
// staging on a state the backend refused. Nobody could tell the game when the refusal came, so the
// respawned component tells it at hydration (N2): the player is kicked, unless the game handles
// it (bGameHandlesCurrencyDesync), and then its handler hears it instead.
bool FRedwoodCurrencySidecarGoneFinalRefusalFreezesTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencySidecarTest;
  AddExpectedErrorPlain(
    TEXT("Failed to flush items (component gone): balance below zero"), EAutomationExpectedErrorFlags::Contains, 1
  );
  AddExpectedErrorPlain(
    TEXT("entry AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA currency Currency.Gold delta -2000 reason CurrencyReason.Loot, NOT saved: its component is gone"),
    EAutomationExpectedErrorFlags::Contains,
    1
  );
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  ARedwoodCurrencyLoadTestPlayerState *PlayerState = nullptr;
  URedwoodPlayerStateComponent *PlayerStateComponent = nullptr;
  ARedwoodKickProbeGameSession *KickProbe = InstallKickProbe(Harness);
  APlayerController *PlayerController = Harness.World->SpawnActor<APlayerController>();
  if (!KickProbe || !TestNotNull(TEXT("The controller spawns"), PlayerController) ||
      !MakeSnapshotOwner(Harness, PlayerState, PlayerStateComponent)) {
    return false;
  }
  // The PlayerState's owner is its player, as in a live session.
  PlayerState->SetOwner(PlayerController);
  PlayerController->PlayerState = PlayerState;
  ARedwoodCurrencyFlushTestActor *Actor = nullptr;
  URedwoodCharacterComponent *Sender = Harness.MakeComponent(Actor);
  Sender->QueueCurrency({MakeEntry(EntryIdA, -2000, -1000, LootReason)}, {});
  Harness.Subsystem->FlushItemsForCharacterComponent(PlayerStateComponent, Sender);
  FRequest Flush;
  if (!Harness.ReadRequest(FlushEvent, Flush)) {
    return false;
  }
  Sender->MarkAsGarbage();
  Harness.Answer(Flush, FinalRefusalReply);
  TestTrue(
    TEXT("The character freezes"),
    Harness.PumpUntil([&Harness]() { return Harness.Subsystem->IsCharacterFrozen(CharacterId); })
  );
  TestTrue(TEXT("The snapshot is marked frozen"), PlayerStateComponent->RedwoodCharacter.bFrozenAfterFinalRefusal);
  TestEqual(TEXT("Nobody is kicked while no component can tell"), KickProbe->KickCount, 0);

  URedwoodCharacterComponent *Respawned = HydrateOnPlayerState(PlayerState);
  TestTrue(TEXT("The respawned component starts frozen"), Respawned->IsFrozenAfterFinalRefusal());
  TestTrue(
    TEXT("With no handler, hydrating frozen kicks the player"),
    Harness.PumpUntil([KickProbe]() { return KickProbe->KickCount == 1; })
  );

  // The game handles it: its handler hears it, and nobody more is kicked.
  Harness.Subsystem->bGameHandlesCurrencyDesync = true;
  int32 RefusalCount = 0;
  URedwoodCharacterComponent *Handled = HydrateOnPlayerState(
    PlayerState,
    [&RefusalCount](URedwoodCharacterComponent *Component) {
      Component->OnItemFlushRefusedFinal.AddLambda(
        [&RefusalCount](const FString &, const FString &) { ++RefusalCount; }
      );
    }
  );
  PumpFor(QuietWindowSeconds);
  TestTrue(TEXT("It starts frozen too"), Handled->IsFrozenAfterFinalRefusal());
  TestEqual(TEXT("The game's handler hears it once"), RefusalCount, 1);
  TestEqual(TEXT("And nobody more is kicked"), KickProbe->KickCount, 1);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyTradeWriterFenceLostTest,
  "Redwood.Currency.Trade.WriterFenceLostMarksNamedSide",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// R37: a first send refused because another session owns a character settles as not applied, and
// only the side the realm names loses its fence. An older realm sends only the summary flag and
// names no side: then both lose it. Nobody freezes.
bool FRedwoodCurrencyTradeWriterFenceLostTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyTradeTest;
  struct FCase {
    const TCHAR *What;
    const TCHAR *Flags;
    bool bFromLost;
    bool bToLost;
  };
  const FCase Cases[] = {
    {TEXT("the from side only"), TEXT(R"("writerFenceLost":true,"fromWriterFenceLost":true)"), true, false},
    {TEXT("the to side only"), TEXT(R"("writerFenceLost":true,"toWriterFenceLost":true)"), false, true},
    {TEXT("the summary only (an older realm)"), TEXT(R"("writerFenceLost":true)"), true, true},
  };
  AddExpectedErrorPlain(TEXT("Failed to trade items"), EAutomationExpectedErrorFlags::Contains, UE_ARRAY_COUNT(Cases));
  FSidecarHarness Harness(*this);
  if (!Harness.Connect()) {
    return false;
  }
  for (const FCase &Case : Cases) {
    // Fresh parties each time: a lost fence never comes back.
    FParty From;
    FParty To;
    if (!MakeParty(Harness, FromId, FromToken, FromSeq, FromBalance, From) ||
        !MakeParty(Harness, ToId, ToToken, ToSeq, ToBalance, To)) {
      return false;
    }
    FTradeResult Result;
    FRequest Request;
    Harness.Subsystem->EmitItemsTrade(
      From.Component, To.Component, {}, MakeTradeCurrency(TradedAmount, TradedAmount),
      MakeCurrencyEntryId(), Capture(Result)
    );
    if (!Harness.ReadRequest(TradeEvent, Request)) {
      return false;
    }
    Harness.Answer(
      Request,
      FString::Printf(
        TEXT(R"({"error":"character is owned by another session","fromCommittedSeq":-1,"toCommittedSeq":-1,%s})"),
        Case.Flags
      )
    );
    const FString What(Case.What);
    TestTrue(What + TEXT(": the trade settles"), Harness.PumpUntil([&Result]() { return Result.Calls > 0; }));
    TestFalse(What + TEXT(": as not applied"), Result.Error.IsEmpty());
    TestEqual(What + TEXT(": the payer's fence"), From.Component->IsWriterFenceLost(), Case.bFromLost);
    TestEqual(What + TEXT(": the payee's fence"), To.Component->IsWriterFenceLost(), Case.bToLost);
    TestFalse(
      What + TEXT(": nobody is frozen"),
      Harness.Subsystem->IsCharacterFrozen(FromId) || Harness.Subsystem->IsCharacterFrozen(ToId)
    );
    TestEqual(What + TEXT(": no seq moves"), From.Component->GetLastCommittedItemSeq(), FromSeq);
  }
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyTradeResendFenceLostGivesUpTest,
  "Redwood.Currency.Trade.ResendFenceLostGivesUp",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// R37: the first send times out, and the resend is refused because another session owns the payee.
// The realm checks the fence before the receipt, so the refusal says nothing about the first send:
// the trade gives up. Every entry is logged as UNKNOWN, the payee loses its fence, and the payer,
// still this session's, freezes so that it loads from the database again.
bool FRedwoodCurrencyTradeResendFenceLostGivesUpTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyTradeTest;
  AddExpectedErrorPlain(TEXT("Failed to trade items"), EAutomationExpectedErrorFlags::Contains, 2);
  AddExpectedErrorPlain(TEXT("Its outcome is UNKNOWN"), EAutomationExpectedErrorFlags::Contains, 1);
  AddExpectedErrorPlain(TEXT("outcome UNKNOWN (it may be saved): in trade"), EAutomationExpectedErrorFlags::Contains, 2);
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
  Harness.Answer(First, TimedOutReply());
  PumpFor(QuietWindowSeconds);
  FAccess::ResendTradeNow(*Harness.Subsystem, TradeId);
  FRequest Resend;
  if (!Harness.ReadRequest(TradeEvent, Resend)) {
    return false;
  }
  Harness.Answer(
    Resend,
    TEXT(R"({"error":"character is owned by another session","fromCommittedSeq":-1,"toCommittedSeq":-1,"writerFenceLost":true,"toWriterFenceLost":true})")
  );
  TestTrue(TEXT("The trade settles"), Harness.PumpUntil([&Result]() { return Result.Calls > 0; }));
  TestFalse(TEXT("With an error"), Result.Error.IsEmpty());
  TestTrue(TEXT("The payee's fence is lost"), To.Component->IsWriterFenceLost());
  TestFalse(TEXT("The payer's fence holds"), From.Component->IsWriterFenceLost());
  TestTrue(TEXT("The payer freezes, so it loads from the database again"), Harness.Subsystem->IsCharacterFrozen(FromId));
  TestFalse(TEXT("The fenced payee is not frozen as well"), Harness.Subsystem->IsCharacterFrozen(ToId));
  TestTrue(TEXT("Nothing more goes out"), Harness.IsQuiet());
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyTradeTwoCurrenciesTest,
  "Redwood.Currency.Trade.TwoCurrenciesChainPerCurrency",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// A trade of two currencies, one each way, chains every entry from the balance of its own currency
// on its own side, names the currency on the wire, and on commit writes each currency of both
// snapshots. The payer is told the balance of every currency it holds.
bool FRedwoodCurrencyTradeTwoCurrenciesTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyTradeTest;
  const FString MarksPaidId = TEXT("A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1");
  const FString MarksReceivedId = TEXT("B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2");
  constexpr int64 FromMarks = 10;
  constexpr int64 ToMarks = 100;
  constexpr int64 TradedMarks = 40;
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
  From.PlayerStateComponent->RedwoodCharacter.CurrencyBalances.Add(MarksCurrency, FromMarks);
  To.PlayerStateComponent->RedwoodCharacter.CurrencyBalances.Add(MarksCurrency, ToMarks);
  FRedwoodCurrencyBalances Rebased;
  From.Component->OnCurrencyRebased.AddLambda([&Rebased](const FRedwoodCurrencyBalances &Balances) {
    Rebased = Balances;
  });

  // From pays gold, To pays marks.
  const TArray<TPair<FString, FRedwoodCurrencyEntry>> Currency = {
    TPair<FString, FRedwoodCurrencyEntry>(FromId, MakeEntry(PaidEntryId, -TradedAmount, 0, TradeReason)),
    TPair<FString, FRedwoodCurrencyEntry>(ToId, MakeEntry(ReceivedEntryId, TradedAmount, 0, TradeReason)),
    TPair<FString, FRedwoodCurrencyEntry>(ToId, MakeEntry(MarksPaidId, -TradedMarks, 0, TradeReason, MarksCurrency)),
    TPair<FString, FRedwoodCurrencyEntry>(FromId, MakeEntry(MarksReceivedId, TradedMarks, 0, TradeReason, MarksCurrency)),
  };
  FTradeResult Result;
  Harness.Subsystem->EmitItemsTrade(From.Component, To.Component, {}, Currency, TradeId, Capture(Result));
  FRequest Trade;
  if (!Harness.ReadRequest(TradeEvent, Trade)) {
    return false;
  }

  // The payload order is not part of the contract, so each entry is found by its id.
  auto WireField = [&Trade](const FString &EntryId, const TCHAR *Field) {
    for (int32 Index = 0; Index < 4; ++Index) {
      if (TradeCurrencyField(Trade.Payload, Index, TEXT("id")) == EntryId) {
        return TradeCurrencyField(Trade.Payload, Index, Field);
      }
    }
    return FString(TEXT("<missing>"));
  };
  TestEqual(TEXT("The payer's gold entry names gold"), WireField(PaidEntryId, TEXT("currency")), GoldCurrency);
  TestEqual(TEXT("It chains from the payer's gold"), WireField(PaidEntryId, TEXT("balanceAfter")), FString(TEXT("700")));
  TestEqual(TEXT("The payee's gold entry names gold"), WireField(ReceivedEntryId, TEXT("currency")), GoldCurrency);
  TestEqual(TEXT("It chains from the payee's gold"), WireField(ReceivedEntryId, TEXT("balanceAfter")), FString(TEXT("500")));
  TestEqual(TEXT("The payee's marks entry names marks"), WireField(MarksPaidId, TEXT("currency")), MarksCurrency);
  TestEqual(TEXT("It chains from the payee's marks, not its gold"), WireField(MarksPaidId, TEXT("balanceAfter")), FString(TEXT("60")));
  TestEqual(TEXT("The payer's marks entry names marks"), WireField(MarksReceivedId, TEXT("currency")), MarksCurrency);
  TestEqual(TEXT("It chains from the payer's marks"), WireField(MarksReceivedId, TEXT("balanceAfter")), FString(TEXT("50")));

  Harness.Answer(Trade, CommittedReply(FromSeq + 1, ToSeq + 1));
  TestTrue(TEXT("The trade settles"), Harness.PumpUntil([&Result]() { return Result.Calls > 0; }));
  TestTrue(TEXT("The trade committed"), Result.Error.IsEmpty());
  const FRedwoodCharacterBackend &FromSnapshot = From.PlayerStateComponent->RedwoodCharacter;
  const FRedwoodCharacterBackend &ToSnapshot = To.PlayerStateComponent->RedwoodCharacter;
  TestEqual(TEXT("The payer's gold"), GetCurrencyBalance(FromSnapshot.CurrencyBalances, GoldCurrency), FromBalance - TradedAmount);
  TestEqual(TEXT("The payer's marks"), GetCurrencyBalance(FromSnapshot.CurrencyBalances, MarksCurrency), FromMarks + TradedMarks);
  TestEqual(TEXT("The payee's gold"), GetCurrencyBalance(ToSnapshot.CurrencyBalances, GoldCurrency), ToBalance + TradedAmount);
  TestEqual(TEXT("The payee's marks"), GetCurrencyBalance(ToSnapshot.CurrencyBalances, MarksCurrency), ToMarks - TradedMarks);
  TestEqual(TEXT("The payer hears gold"), GetCurrencyBalance(Rebased, GoldCurrency), FromBalance - TradedAmount);
  TestEqual(TEXT("The payer hears marks"), GetCurrencyBalance(Rebased, MarksCurrency), FromMarks + TradedMarks);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyTradePerCurrencyZeroSumTest,
  "Redwood.Currency.Trade.EachCurrencyMustSumToZero",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// The backend balances each currency on its own, so a trade whose total is zero but whose
// currencies are not is refused before anything is sent. So is a second entry of one currency for
// the same side, which the backend refuses too.
bool FRedwoodCurrencyTradePerCurrencyZeroSumTest::RunTest(const FString &Parameters) {
  using namespace RedwoodCurrencyTradeTest;
  AddExpectedErrorPlain(TEXT("refused before sending"), EAutomationExpectedErrorFlags::Contains, 3);
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

  auto ExpectRefused = [this, &Harness, &From, &To](
                         const TCHAR *Label, const TArray<TPair<FString, FRedwoodCurrencyEntry>> &Currency
                       ) {
    FTradeResult Result;
    Harness.Subsystem->EmitItemsTrade(From.Component, To.Component, {}, Currency, TradeId, Capture(Result));
    TestEqual(FString::Printf(TEXT("%s: the caller hears at once"), Label), Result.Calls, 1);
    TestFalse(FString::Printf(TEXT("%s: with an error"), Label), Result.Error.IsEmpty());
    TestNull(FString::Printf(TEXT("%s: nothing is pending"), Label), FAccess::Trade(*Harness.Subsystem, TradeId));
  };

  // -5 gold and +5 marks total zero, yet neither currency balances.
  ExpectRefused(
    TEXT("different currencies"),
    {TPair<FString, FRedwoodCurrencyEntry>(FromId, MakeEntry(PaidEntryId, -5, 0, TradeReason)),
     TPair<FString, FRedwoodCurrencyEntry>(ToId, MakeEntry(ReceivedEntryId, 5, 0, TradeReason, MarksCurrency))}
  );
  // Gold balances, and marks are only credited.
  ExpectRefused(
    TEXT("one balanced currency and one one-sided"),
    {TPair<FString, FRedwoodCurrencyEntry>(FromId, MakeEntry(PaidEntryId, -5, 0, TradeReason)),
     TPair<FString, FRedwoodCurrencyEntry>(ToId, MakeEntry(ReceivedEntryId, 5, 0, TradeReason)),
     TPair<FString, FRedwoodCurrencyEntry>(
       ToId, MakeEntry(TEXT("A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1"), 2, 0, TradeReason, MarksCurrency)
     )}
  );
  // Each currency sums to zero overall, but the payer has two gold entries.
  ExpectRefused(
    TEXT("two entries of one currency for one side"),
    {TPair<FString, FRedwoodCurrencyEntry>(FromId, MakeEntry(PaidEntryId, -3, 0, TradeReason)),
     TPair<FString, FRedwoodCurrencyEntry>(FromId, MakeEntry(TEXT("A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1"), -2, 0, TradeReason)),
     TPair<FString, FRedwoodCurrencyEntry>(ToId, MakeEntry(ReceivedEntryId, 5, 0, TradeReason))}
  );

  TestTrue(TEXT("No request leaves"), Harness.IsQuiet());
  TestEqual(TEXT("The payer's lane is untouched"), From.Component->GetLastCommittedItemSeq(), FromSeq);
  return true;
}

#endif // WITH_AUTOMATION_WORKER
