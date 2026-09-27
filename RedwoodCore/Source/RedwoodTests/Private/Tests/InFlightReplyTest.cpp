// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// HollowedOath#2886. A request in flight when its socket drops never got its
// reply, so its callback never ran. Each test sends a request to a server
// that reads it and then closes the socket before it answers, and asserts
// that the caller hears back with an error in bounded time.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

#include "FakeSocketIoServer.h"
#include "RedwoodClientInterface.h"
#include "SocketIOClient.h"
#include "SocketIONative.h"

// The private members these tests drive, and nothing else.
class FRedwoodInFlightTestAccess {
public:
  using FClient = URedwoodClientInterface;

  static TSharedPtr<FSocketIONative> &Realm(FClient &C) {
    return C.Realm;
  }
  static FString &PlayerId(FClient &C) {
    return C.PlayerId;
  }
  static FString &AuthToken(FClient &C) {
    return C.AuthToken;
  }
  static FString &SelectedCharacterId(FClient &C) {
    return C.SelectedCharacterId;
  }
  static bool &bAuthenticated(FClient &C) {
    return C.bAuthenticated;
  }
  static bool &bSentRealmConnected(FClient &C) {
    return C.bSentRealmConnected;
  }
  static bool &bRealmDisconnected(FClient &C) {
    return C.bRealmDisconnected;
  }
  static TFunction<void(FSocketIONative &)> &ReconnectSocket(FClient &C) {
    return C.ReconnectSocket;
  }
  static void BindRealmCloseHandler(FClient &C) {
    C.BindRealmCloseHandler();
  }
  static void NoteRealmDrop(FClient &C) {
    C.NoteRealmDrop();
  }
};

namespace RedwoodInFlightTest {
  using namespace RedwoodFakeSocketIo;
  using FAccess = FRedwoodInFlightTestAccess;

  constexpr uint16 ServiceRestartCloseCode = 1012;
  // The library waits ReconnectionDelay (5000 ms) before it reconnects after
  // a 1012 close, which is the edge of StepTimeout. Far inside it, so a
  // reconnect lands while the test waits.
  constexpr uint32 ReconnectDelayForTestMs = 200;

  enum class EClose { Normal, ServiceRestart, NoCloseFrame };

  // A Realm socket on the fake server, with the callbacks InitiateRealmHandshake
  // installs, and a logged-in player so the gates let a request through.
  struct FRealmHarness {
    TUniquePtr<FFakeSocketIoServer> Server = MakeUnique<FFakeSocketIoServer>();
    TStrongObjectPtr<URedwoodClientInterface> Interface{
      NewObject<URedwoodClientInterface>()
    };

    URedwoodClientInterface &Client() const {
      return *Interface.Get();
    }

    bool Open(FAutomationTestBase &Test) {
      URedwoodClientInterface &C = Client();
      if (!Test.TestTrue(TEXT("Test server listens"), Server->Listen())) {
        return false;
      }
      FAccess::PlayerId(C) = TEXT("player-1");
      FAccess::AuthToken(C) = TEXT("token-1");
      FAccess::SelectedCharacterId(C) = TEXT("character-1");
      FAccess::bAuthenticated(C) = true;
      FAccess::bSentRealmConnected(C) = true;

      TSharedPtr<FSocketIONative> &Realm = FAccess::Realm(C);
      Realm = ISocketIOClientModule::Get().NewValidNativePointer();
      FAccess::BindRealmCloseHandler(C);
      FAccess::ReconnectSocket(C) = [](FSocketIONative &) {};
      // Set before Connect(), which applies it.
      Realm->ReconnectionDelay = ReconnectDelayForTestMs;
      URedwoodClientInterface *Raw = &C;
      Realm->OnReconnectionCallback = [Raw](uint32, uint32) {
        if (!FAccess::bRealmDisconnected(*Raw)) {
          FAccess::bRealmDisconnected(*Raw) = true;
          FAccess::NoteRealmDrop(*Raw);
        }
      };
      Realm->OnConnectedCallback = [Raw](const FString &, const FString &) {
        FAccess::bRealmDisconnected(*Raw) = false;
      };
      Realm->Connect(FString::Printf(TEXT("ws://127.0.0.1:%d"), Server->Port));
      return Test.TestTrue(TEXT("Client opens a session"), Server->AcceptSession()) &&
        Test.TestTrue(
          TEXT("Client joins the namespace"),
          PumpGameThreadUntil([Raw]() {
            return !FAccess::bRealmDisconnected(*Raw);
          })
        );
    }

    bool Close(EClose How) {
      switch (How) {
        case EClose::Normal:
          return Server->CloseNormally();
        case EClose::ServiceRestart:
          return Server->CloseWithCode(ServiceRestartCloseCode);
        default:
          Server->DropConnection();
          return true;
      }
    }

    ~FRealmHarness() {
      Server.Reset();
      Interface->Deinitialize();
    }
  };

  // Records one of the three outcomes the issue asks for.
  FString Outcome(bool bAnswered, const FString &Error) {
    return !bAnswered      ? TEXT("hang")
      : Error.IsEmpty()    ? TEXT("completion")
                           : TEXT("clear error");
  }

  void ExpectClearError(
    FAutomationTestBase &Test,
    const TCHAR *What,
    bool bAnswered,
    const FString &Error
  ) {
    const FString Result = Outcome(bAnswered, Error);
    Test.AddInfo(FString::Printf(TEXT("%s in flight: %s"), What, *Result));
    Test.TestEqual(
      TEXT("The caller hears a clear error"), Result, FString(TEXT("clear error"))
    );
  }

  bool RunPartyInvite(FAutomationTestBase &Test, EClose How) {
    // Before the harness, so they outlive every callback it can run.
    bool bAnswered = false;
    FString Error;
    FRealmHarness Harness;
    if (!Harness.Open(Test)) {
      return false;
    }
    Harness.Client().InviteToParty(
      TEXT("player-2"),
      FRedwoodErrorOutputDelegate::CreateLambda([&](const FString &InError) {
        bAnswered = true;
        Error = InError;
      })
    );
    Test.TestTrue(TEXT("The request reaches the server"), Harness.Server->ReadClientFrame());
    Test.TestTrue(TEXT("The server closes"), Harness.Close(How));

    PumpGameThreadUntil([&bAnswered]() { return bAnswered; });
    ExpectClearError(Test, TEXT("Party invite"), bAnswered, Error);
    return true;
  }

  bool RunJoinQueue(FAutomationTestBase &Test, EClose How) {
    // Before the harness, so they outlive every callback it can run.
    bool bAnswered = false;
    FString Error;
    FRealmHarness Harness;
    if (!Harness.Open(Test)) {
      return false;
    }
    Harness.Client().JoinQueue(
      TEXT("proxy-1"),
      TEXT("zone-1"),
      false,
      false,
      FRedwoodTicketingUpdateDelegate::CreateLambda(
        [&](const FRedwoodTicketingUpdate &Update) {
          bAnswered = true;
          Error = Update.Message;
        }
      )
    );
    Test.TestTrue(TEXT("The request reaches the server"), Harness.Server->ReadClientFrame());
    Test.TestTrue(TEXT("The server closes"), Harness.Close(How));

    PumpGameThreadUntil([&bAnswered]() { return bAnswered; });
    ExpectClearError(Test, TEXT("Zone join"), bAnswered, Error);
    return true;
  }
}

#define REDWOOD_IN_FLIGHT_TEST(Class, Name, Body)                              \
  IMPLEMENT_SIMPLE_AUTOMATION_TEST(                                           \
    Class,                                                                    \
    "Redwood.Socket.InFlight." Name,                                          \
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter \
  )                                                                           \
  bool Class::RunTest(const FString &Parameters) {                            \
    return Body;                                                              \
  }

REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightPartyNormalCloseTest,
  "PartyInviteNormalClose",
  RedwoodInFlightTest::RunPartyInvite(*this, RedwoodInFlightTest::EClose::Normal)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightPartyRestartCloseTest,
  "PartyInviteServiceRestart",
  RedwoodInFlightTest::RunPartyInvite(
    *this, RedwoodInFlightTest::EClose::ServiceRestart
  )
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightPartyDropTest,
  "PartyInviteNoCloseFrame",
  RedwoodInFlightTest::RunPartyInvite(
    *this, RedwoodInFlightTest::EClose::NoCloseFrame
  )
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightZoneNormalCloseTest,
  "ZoneJoinNormalClose",
  RedwoodInFlightTest::RunJoinQueue(*this, RedwoodInFlightTest::EClose::Normal)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightZoneRestartCloseTest,
  "ZoneJoinServiceRestart",
  RedwoodInFlightTest::RunJoinQueue(
    *this, RedwoodInFlightTest::EClose::ServiceRestart
  )
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightZoneDropTest,
  "ZoneJoinNoCloseFrame",
  RedwoodInFlightTest::RunJoinQueue(
    *this, RedwoodInFlightTest::EClose::NoCloseFrame
  )
)
