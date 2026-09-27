// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// A backend that stops gracefully can close the socket with a normal (1000)
// close. The socket library takes that as a close the client asked for and
// does not reconnect, so Redwood never learned of the drop and never started
// its grace and re-login. URedwoodClientInterface::MakeUnrequestedCloseHandler
// reports such a close as a drop and connects again after a backoff.
//
// The first test runs a minimal websocket server on a raw TCP socket, so the
// real socket.io client and websocketpp run the whole close handshake.

#include "CoreMinimal.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/ThreadSafeBool.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "Templates/Atomic.h"
#include "UObject/StrongObjectPtr.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

#include "RedwoodClientInterface.h"
#include "SocketIOClient.h"
#include "SocketIONative.h"

namespace RedwoodServerCloseTest {
  // Long enough for a loaded build machine, short enough to fail fast.
  const FTimespan StepTimeout = FTimespan::FromSeconds(5.0);
  constexpr uint32 StepTimeoutMs = 5000;

  constexpr uint32 LoopbackIp = 0x7F000001;
  constexpr uint16 NormalCloseCode = 1000;

  // The library queues its reports to the game thread, which a test holds.
  // The pump runs those tasks at 10 Hz, the fastest this project allows a
  // wait to check, until the test's condition holds or the step times out.
  constexpr float PumpIntervalSeconds = 0.1f;

  bool PumpGameThreadUntil(TFunctionRef<bool()> Done) {
    const double Deadline =
      FPlatformTime::Seconds() + StepTimeout.GetTotalSeconds();
    while (true) {
      FTaskGraphInterface::Get().ProcessThreadUntilIdle(
        ENamedThreads::GameThread
      );
      if (Done()) {
        return true;
      }
      if (FPlatformTime::Seconds() > Deadline) {
        return false;
      }
      FPlatformProcess::Sleep(PumpIntervalSeconds);
    }
  }

  // RFC 6455 section 1.3.
  const TCHAR *WebSocketAcceptGuid = TEXT("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");

  // An engine.io v4 open packet, so the client starts its socket.io session.
  const char *EngineIoOpenPacket =
    "0{\"sid\":\"test\",\"upgrades\":[],\"pingInterval\":25000,"
    "\"pingTimeout\":20000,\"maxPayload\":1000000}";

  // Accepts the client's "/" namespace. Unanswered, its 20 s connect timer
  // keeps the socket thread alive, and SyncDisconnect waits for it.
  const char *SocketIoConnectPacket = "40{\"sid\":\"test\"}";

  TArray<uint8> AnsiBytes(const char *Text) {
    return TArray<uint8>(
      reinterpret_cast<const uint8 *>(Text), FCStringAnsi::Strlen(Text)
    );
  }

  bool SendAll(FSocket *Socket, const TArray<uint8> &Bytes) {
    int32 Sent = 0;
    return Socket->Send(Bytes.GetData(), Bytes.Num(), Sent) &&
      Sent == Bytes.Num();
  }

  // Reads whatever arrives next, so the test knows the client has spoken.
  bool ReadSome(FSocket *Socket, TArray<uint8> &OutBytes) {
    if (!Socket->Wait(ESocketWaitConditions::WaitForRead, StepTimeout)) {
      return false;
    }
    uint8 Buffer[1024];
    int32 Read = 0;
    if (!Socket->Recv(Buffer, sizeof(Buffer), Read) || Read <= 0) {
      return false;
    }
    OutBytes.Append(Buffer, Read);
    return true;
  }

  // Server frames are not masked, and these payloads are under 126 bytes.
  TArray<uint8> MakeFrame(uint8 Opcode, const TArray<uint8> &Payload) {
    check(Payload.Num() < 126);
    TArray<uint8> Frame = {
      static_cast<uint8>(0x80 | Opcode), static_cast<uint8>(Payload.Num())
    };
    Frame.Append(Payload);
    return Frame;
  }

  struct FFakeSocketIoServer {
    ISocketSubsystem *Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    FSocket *Listener = nullptr;
    FSocket *Connection = nullptr;
    int32 Port = 0;

    bool Listen() {
      Listener = Subsystem->CreateSocket(
        NAME_Stream, TEXT("RedwoodServerCloseTest"), FNetworkProtocolTypes::IPv4
      );
      TSharedRef<FInternetAddr> Address =
        Subsystem->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
      Address->SetIp(LoopbackIp);
      Address->SetPort(0);
      if (!Listener || !Listener->Bind(*Address) || !Listener->Listen(1)) {
        return false;
      }
      Port = Listener->GetPortNo();
      return true;
    }

    bool WaitForConnection() const {
      return Listener->Wait(ESocketWaitConditions::WaitForRead, StepTimeout);
    }

    // Accepts the websocket upgrade, opens the engine.io session, waits for
    // the client's namespace connect and accepts it.
    bool AcceptSession() {
      if (!WaitForConnection()) {
        return false;
      }
      Connection = Listener->Accept(TEXT("RedwoodServerCloseTestConnection"));
      if (!Connection) {
        return false;
      }

      TArray<uint8> Request;
      FString RequestText;
      while (!RequestText.Contains(TEXT("\r\n\r\n"))) {
        if (!ReadSome(Connection, Request)) {
          return false;
        }
        const FUTF8ToTCHAR Converted(
          reinterpret_cast<const UTF8CHAR *>(Request.GetData()), Request.Num()
        );
        RequestText = FString(Converted.Length(), Converted.Get());
      }

      const FString KeyHeader = TEXT("Sec-WebSocket-Key:");
      const int32 KeyStart = RequestText.Find(KeyHeader);
      if (KeyStart == INDEX_NONE) {
        return false;
      }
      const int32 ValueStart = KeyStart + KeyHeader.Len();
      const int32 ValueEnd = RequestText.Find(
        TEXT("\r\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, ValueStart
      );
      const FString Key =
        RequestText.Mid(ValueStart, ValueEnd - ValueStart).TrimStartAndEnd();

      const FTCHARToUTF8 AcceptSource(*(Key + WebSocketAcceptGuid));
      uint8 Digest[FSHA1::DigestSize];
      FSHA1::HashBuffer(AcceptSource.Get(), AcceptSource.Length(), Digest);
      const FString Response = FString::Printf(
        TEXT("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n")
        TEXT("Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n"),
        *FBase64::Encode(Digest, FSHA1::DigestSize)
      );
      const FTCHARToUTF8 ResponseUtf8(*Response);
      const TArray<uint8> ResponseBytes(
        reinterpret_cast<const uint8 *>(ResponseUtf8.Get()), ResponseUtf8.Length()
      );

      TArray<uint8> ClientPacket;
      return SendAll(Connection, ResponseBytes) &&
        SendAll(Connection, MakeFrame(0x1, AnsiBytes(EngineIoOpenPacket))) &&
        ReadSome(Connection, ClientPacket) &&
        SendAll(Connection, MakeFrame(0x1, AnsiBytes(SocketIoConnectPacket)));
    }

    // Plays the server side of a close handshake: the close frame, the
    // client's echo, then the TCP close, which RFC 6455 gives the server.
    bool CloseNormally() {
      const TArray<uint8> Code = {
        static_cast<uint8>(NormalCloseCode >> 8),
        static_cast<uint8>(NormalCloseCode & 0xFF)
      };
      TArray<uint8> Echo;
      const bool bClosed =
        SendAll(Connection, MakeFrame(0x8, Code)) && ReadSome(Connection, Echo);
      DropConnection();
      return bClosed;
    }

    void DropConnection() {
      if (Connection) {
        Connection->Close();
        Subsystem->DestroySocket(Connection);
        Connection = nullptr;
      }
    }

    ~FFakeSocketIoServer() {
      DropConnection();
      if (Listener) {
        Listener->Close();
        Subsystem->DestroySocket(Listener);
      }
    }
  };

  // Callbacks run on the socket thread, so the test can block on events
  // instead of pumping the game thread.
  struct FClientProbe {
    TSharedPtr<FSocketIONative> Native;
    FEvent *Connected = FPlatformProcess::GetSynchEventFromPool(true);
    FEvent *Disconnected = FPlatformProcess::GetSynchEventFromPool(true);
    FThreadSafeBool bLibraryReconnected = false;
    TAtomic<ESIOConnectionCloseReason> CloseReason{
      ESIOConnectionCloseReason::CLOSE_REASON_DROP
    };

    explicit FClientProbe(int32 Port) {
      Native = ISocketIOClientModule::Get().NewValidNativePointer();
      Native->bCallbackOnGameThread = false;
      Native->OnConnectedCallback = [this](const FString &, const FString &) {
        Connected->Trigger();
      };
      Native->OnReconnectionCallback = [this](uint32, uint32) {
        bLibraryReconnected = true;
      };
      Native->OnDisconnectedCallback =
        [this](const ESIOConnectionCloseReason Reason) {
          CloseReason = Reason;
          Disconnected->Trigger();
        };
      Native->Connect(FString::Printf(TEXT("ws://127.0.0.1:%d"), Port));
    }

    // Call after the server sockets are gone, so no connect attempt is left
    // waiting on them; SyncDisconnect then joins the socket thread.
    void Shutdown() {
      Native->ClearAllCallbacks();
      Native->SyncDisconnect();
      ISocketIOClientModule::Get().ReleaseNativePointer(Native);
      Native.Reset();
      FPlatformProcess::ReturnSynchEventToPool(Connected);
      FPlatformProcess::ReturnSynchEventToPool(Disconnected);
    }
  };
}

// Pins what the Redwood close handler relies on: the library reports a
// server's 1000 close as CLOSE_REASON_NORMAL and does not reconnect by
// itself. If it starts to reconnect, the handler would connect a second time.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodServerNormalCloseIsNormalTest,
  "Redwood.Socket.ServerNormalCloseIsNormal",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodServerNormalCloseIsNormalTest::RunTest(const FString &Parameters) {
  using namespace RedwoodServerCloseTest;

  TUniquePtr<FFakeSocketIoServer> Server = MakeUnique<FFakeSocketIoServer>();
  if (!TestTrue(TEXT("Test server listens"), Server->Listen())) {
    return false;
  }
  FClientProbe Client(Server->Port);
  ON_SCOPE_EXIT {
    Server.Reset();
    Client.Shutdown();
  };

  if (!TestTrue(TEXT("Client opens a session"), Server->AcceptSession()) ||
      !TestTrue(
        TEXT("Client joins the namespace"),
        Client.Connected->Wait(StepTimeoutMs)
      )) {
    return false;
  }
  TestTrue(TEXT("Server closes with 1000"), Server->CloseNormally());

  if (!TestTrue(
        TEXT("The client reports the close"),
        Client.Disconnected->Wait(StepTimeoutMs)
      )) {
    return false;
  }
  TestEqual(
    TEXT("A server's 1000 close reads as a normal close"),
    static_cast<int32>(Client.CloseReason.Load()),
    static_cast<int32>(ESIOConnectionCloseReason::CLOSE_REASON_NORMAL)
  );
  // The library calls its close listener only on the branch where it does
  // not reconnect: the reconnect branch returns before it. So the report
  // above already proves this; the flag guards against a later change.
  TestFalse(
    TEXT("The library does not reconnect by itself"),
    static_cast<bool>(Client.bLibraryReconnected)
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodUnrequestedCloseBacksOffTest,
  "Redwood.Socket.UnrequestedCloseBacksOff",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodUnrequestedCloseBacksOffTest::RunTest(const FString &Parameters) {
  // More closes than it takes the delay to reach its maximum.
  constexpr int32 RepeatedCloses = 10;

  TStrongObjectPtr<URedwoodClientInterface> Interface(
    NewObject<URedwoodClientInterface>()
  );
  URedwoodClientInterface *Client = Interface.Get();

  // Never connected, so no close reaches it but the ones the test sends.
  Client->Realm = ISocketIOClientModule::Get().NewValidNativePointer();
  ON_SCOPE_EXIT {
    Client->Deinitialize();
  };
  Client->BindRealmCloseHandler();
  int32 DropReports = 0;
  Client->Realm->OnReconnectionCallback = [&DropReports](uint32, uint32) {
    ++DropReports;
  };
  int32 Reconnects = 0;
  Client->ReconnectSocket = [&Reconnects](FSocketIONative &) { ++Reconnects; };

  Client->Realm->OnDisconnectedCallback(
    ESIOConnectionCloseReason::CLOSE_REASON_NORMAL
  );
  TestEqual(TEXT("An unrequested close reports the drop"), DropReports, 1);
  TestEqual(
    TEXT("The reconnect does not run at once"), Reconnects, 0
  );

  // A timer set inside a frame starts on the next tick. Then go past the
  // longest first delay the jitter can pick.
  ++GFrameCounter;
  Client->TimerManager.Tick(0.0f);
  ++GFrameCounter;
  Client->TimerManager.Tick(2.0f * FRedwoodCloseBackoff::InitialDelaySeconds);
  TestEqual(TEXT("The reconnect runs after the delay"), Reconnects, 1);

  // A server that accepts and then closes again, long past the point where
  // the delay reaches its maximum.
  for (int32 Close = 1; Close <= RepeatedCloses; ++Close) {
    Client->Realm->OnDisconnectedCallback(
      ESIOConnectionCloseReason::CLOSE_REASON_NORMAL
    );
  }
  TestEqual(
    TEXT("Every close reports the drop, so the lost connection stands"),
    DropReports,
    RepeatedCloses + 1
  );
  TestTrue(
    TEXT("The client still retries after many closes"),
    Client->RealmCloseBackoff.IsReconnectPending(Client->TimerManager)
  );

  // Only a connection that stays up resets the backoff. A server that lets
  // the re-handshake finish and then closes must not bring it back to the
  // first delay.
  Client->EndRealmReauthentication(true);
  TestEqual(
    TEXT("A good re-handshake alone does not reset the backoff"),
    Client->RealmCloseBackoff.NumAttempts(),
    RepeatedCloses + 1
  );
  return true;
}

// The game gets its close reports on the game thread. A close the client
// asks for is reported twice: at once from Disconnect(), and again from a
// lambda the library queues when the close handshake ends. The second report
// reads bRealmCloseRequested late, so the flag must still be set then.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodQueuedRequestedCloseStaysCleanTest,
  "Redwood.Socket.QueuedRequestedCloseStaysClean",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodQueuedRequestedCloseStaysCleanTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodServerCloseTest;

  TUniquePtr<FFakeSocketIoServer> Server = MakeUnique<FFakeSocketIoServer>();
  if (!TestTrue(TEXT("Test server listens"), Server->Listen())) {
    return false;
  }
  TStrongObjectPtr<URedwoodClientInterface> Interface(
    NewObject<URedwoodClientInterface>()
  );
  URedwoodClientInterface *Client = Interface.Get();

  // bCallbackOnGameThread stays on, as in the game.
  Client->Realm = ISocketIOClientModule::Get().NewValidNativePointer();
  ON_SCOPE_EXIT {
    Server.Reset();
    Client->Deinitialize();
  };
  Client->BindRealmCloseHandler();
  int32 CloseReports = 0;
  TFunction<void(const ESIOConnectionCloseReason)> Handler =
    Client->Realm->OnDisconnectedCallback;
  Client->Realm->OnDisconnectedCallback =
    [Handler, &CloseReports](const ESIOConnectionCloseReason Reason) {
      Handler(Reason);
      ++CloseReports;
    };
  int32 DropReports = 0;
  Client->Realm->OnReconnectionCallback = [&DropReports](uint32, uint32) {
    ++DropReports;
  };
  int32 Reconnects = 0;
  Client->ReconnectSocket = [&Reconnects](FSocketIONative &) { ++Reconnects; };

  Client->Realm->Connect(FString::Printf(TEXT("ws://127.0.0.1:%d"), Server->Port));
  if (!TestTrue(TEXT("Client opens a session"), Server->AcceptSession())) {
    return false;
  }

  Client->RequestRealmClose();
  TestTrue(TEXT("Server acknowledges the close"), Server->CloseNormally());
  if (!TestTrue(
        TEXT("The queued close report runs"),
        PumpGameThreadUntil([&CloseReports]() { return CloseReports >= 2; })
      )) {
    return false;
  }

  TestEqual(TEXT("A requested close reports no drop"), DropReports, 0);
  TestFalse(
    TEXT("A requested close leaves no reconnect pending"),
    Client->RealmCloseBackoff.IsReconnectPending(Client->TimerManager)
  );
  TestEqual(TEXT("A requested close does not reconnect"), Reconnects, 0);
  return true;
}
