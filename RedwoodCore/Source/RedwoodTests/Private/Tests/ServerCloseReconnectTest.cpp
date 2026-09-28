// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// A backend that stops gracefully can close the socket with a normal (1000)
// close. The socket library takes that as a close the client asked for and
// does not reconnect, so Redwood never learned of the drop and never started
// its grace and re-login. URedwoodClientInterface::MakeUnrequestedCloseHandler
// reports such a close as a drop and connects again after a backoff.
//
// The first test runs the fake server of FakeSocketIoServer.h, so the real
// socket.io client and websocketpp run the whole close handshake.

#include "CoreMinimal.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/ThreadSafeBool.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Templates/Atomic.h"
#include "UObject/StrongObjectPtr.h"

#include "FakeSocketIoServer.h"
#include "RedwoodClientInterface.h"
#include "SocketIOClient.h"
#include "SocketIONative.h"

namespace RedwoodServerCloseTest {
  using namespace RedwoodFakeSocketIo;

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
