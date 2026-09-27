// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// HollowedOath#2886. A request in flight when its socket drops never got its
// reply, so its callback never ran. Each test sends a request to a server
// that reads it and then closes the socket before it answers, and asserts
// that the caller hears back with an error in bounded time.

#include "CoreMinimal.h"
#include "Algo/AllOf.h"
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
  static void NoteRealmReconnected(FClient &C) {
    C.NoteRealmReconnected();
  }
  static bool &bAbandonedQueueJoin(FClient &C) {
    return C.bAbandonedQueueJoin;
  }
  static FString &ServerConnection(FClient &C) {
    return C.ServerConnection;
  }
  static FRedwoodTicketingUpdateDelegate &OnTicketingUpdate(FClient &C) {
    return C.OnTicketingUpdate;
  }
  static void BindRealmEvents(FClient &C) {
    C.BindRealmEvents();
  }
  static TSharedPtr<FSocketIONative> &Director(FClient &C) {
    return C.Director;
  }
  static bool &bDirectorDisconnected(FClient &C) {
    return C.bDirectorDisconnected;
  }
  static bool &bSentDirectorConnected(FClient &C) {
    return C.bSentDirectorConnected;
  }
  static bool &bRealmReauthPending(FClient &C) {
    return C.bRealmReauthPending;
  }
  static void NoteDirectorDrop(FClient &C) {
    C.NoteDirectorDrop();
  }
  static void BeginRealmReauthentication(FClient &C) {
    C.BeginRealmReauthentication();
  }
  static bool IsRealmReauthRetryPending(FClient &C) {
    return C.TimerManager.IsTimerActive(C.ReauthenticationAttemptTimer);
  }
  static int32 NumDirectorReplies(FClient &C) {
    return C.DirectorReplies.Num();
  }
  static int32 NumRealmHeldRequests(FClient &C) {
    return C.RealmHeldRequests.Num();
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
        FAccess::NoteRealmReconnected(*Raw);
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
      // The library queues game-thread callbacks that point at the socket,
      // and Deinitialize frees the socket on a background thread. Keep the
      // sockets until that release is done and the queue has run, so no
      // callback of this test runs on a freed socket in a later test.
      URedwoodClientInterface &C = Client();
      TArray<TSharedPtr<FSocketIONative>> Sockets;
      for (const TSharedPtr<FSocketIONative> &Socket :
           {FAccess::Realm(C), FAccess::Director(C)}) {
        if (Socket.IsValid()) {
          Sockets.Add(Socket);
        }
      }
      Server.Reset();
      Interface->Deinitialize();
      PumpGameThreadUntil([&Sockets]() {
        return Algo::AllOf(Sockets, [](const TSharedPtr<FSocketIONative> &S) {
          return S.GetSharedReferenceCount() == 1;
        });
      });
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

  // Review focus: JoinQueue is not held by a gate, so one sent after the drop
  // was not pending when the drop failed the others. It must fail at the
  // reconnect. Pins: the FailAll in NoteRealmReconnected.
  bool RunZoneJoinSentAfterDrop(FAutomationTestBase &Test) {
    bool bAnswered = false;
    FString Error;
    FRealmHarness Harness;
    if (!Harness.Open(Test)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    Test.TestTrue(TEXT("The server closes"), Harness.Close(EClose::ServiceRestart));
    if (!Test.TestTrue(
          TEXT("The client notes the drop"),
          PumpGameThreadUntil([&C]() { return FAccess::bRealmDisconnected(C); })
        )) {
      return false;
    }

    C.JoinQueue(
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
    Test.TestFalse(TEXT("Nothing answers before the reconnect"), bAnswered);

    Test.TestTrue(TEXT("The client reconnects"), Harness.Server->AcceptSession());
    PumpGameThreadUntil([&bAnswered]() { return bAnswered; });
    ExpectClearError(Test, TEXT("Zone join sent after the drop"), bAnswered, Error);
    Test.TestTrue(
      TEXT("The lost join is dropped until the queue is left"),
      FAccess::bAbandonedQueueJoin(C)
    );
    return true;
  }

  // C1: a Director drop while the Realm re-login waits for its token must not
  // end the session. The retry asks for a new token; the held Realm request
  // waits for it. Pins: the token request of BeginRealmReauthentication is
  // not tracked (tracked, the drop fails it into EndRealmReauthentication(false),
  // which expires the held request and fires OnRealmAuthFailed).
  bool RunDirectorDropDuringRealmRelogin(FAutomationTestBase &Test) {
    bool bInviteAnswered = false;
    FRealmHarness Harness;
    TUniquePtr<FFakeSocketIoServer> DirectorServer =
      MakeUnique<FFakeSocketIoServer>();
    if (!Harness.Open(Test) ||
        !Test.TestTrue(TEXT("Director server listens"), DirectorServer->Listen())) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    URedwoodClientInterface *Raw = &C;

    // As InitializeDirectorConnection wires it.
    TSharedPtr<FSocketIONative> &Director = FAccess::Director(C);
    Director = ISocketIOClientModule::Get().NewValidNativePointer();
    Director->OnReconnectionCallback = [Raw](uint32, uint32) {
      if (!FAccess::bDirectorDisconnected(*Raw)) {
        FAccess::bDirectorDisconnected(*Raw) = true;
        FAccess::NoteDirectorDrop(*Raw);
        FAccess::bAuthenticated(*Raw) = false;
      }
    };
    Director->OnConnectedCallback = [Raw](const FString &, const FString &) {
      FAccess::bDirectorDisconnected(*Raw) = false;
    };
    Director->Connect(
      FString::Printf(TEXT("ws://127.0.0.1:%d"), DirectorServer->Port)
    );
    if (!Test.TestTrue(TEXT("Director opens a session"), DirectorServer->AcceptSession()) ||
        !Test.TestTrue(
          TEXT("Director joins the namespace"),
          PumpGameThreadUntil([Raw]() {
            return !FAccess::bDirectorDisconnected(*Raw);
          })
        )) {
      return false;
    }
    FAccess::bSentDirectorConnected(C) = true;
    // The Realm socket came back; its re-login has not run yet.
    FAccess::bRealmReauthPending(C) = true;

    C.InviteToParty(
      TEXT("player-2"),
      FRedwoodErrorOutputDelegate::CreateLambda(
        [&bInviteAnswered](const FString &) { bInviteAnswered = true; }
      )
    );
    Test.TestEqual(
      TEXT("The gate holds the invite for the re-login"),
      FAccess::NumRealmHeldRequests(C),
      1
    );

    FAccess::BeginRealmReauthentication(C);
    Test.TestTrue(
      TEXT("The token request reaches the Director"),
      DirectorServer->ReadClientFrame()
    );
    Test.TestEqual(
      TEXT("The token request is not tracked"), FAccess::NumDirectorReplies(C), 0
    );

    Test.TestTrue(
      TEXT("The Director closes"),
      DirectorServer->CloseWithCode(ServiceRestartCloseCode)
    );
    if (!Test.TestTrue(
          TEXT("The client notes the Director drop"),
          PumpGameThreadUntil([Raw]() {
            return FAccess::bDirectorDisconnected(*Raw);
          })
        )) {
      return false;
    }

    // EndRealmReauthentication(false) is the only step here that expires the
    // held requests, and OnRealmAuthFailed fires right after it.
    Test.TestEqual(
      TEXT("The invite is still held, so the re-login did not fail"),
      FAccess::NumRealmHeldRequests(C),
      1
    );
    Test.TestFalse(TEXT("The invite has no answer yet"), bInviteAnswered);
    Test.TestTrue(
      TEXT("The Realm re-login asks again"), FAccess::IsRealmReauthRetryPending(C)
    );
    return true;
  }

  // Sends a zone assignment, then a ticketing update as a marker: the client
  // handles events in order, so when the marker arrives, the assignment was
  // handled. The marker's handler records whether the assignment moved.
  bool AssignmentMoves(
    FAutomationTestBase &Test, FRealmHarness &Harness, const char *Assignment
  ) {
    URedwoodClientInterface &C = Harness.Client();
    bool bMarkerSeen = false;
    FAccess::ServerConnection(C).Reset();
    FAccess::OnTicketingUpdate(C) = FRedwoodTicketingUpdateDelegate::CreateLambda(
      [&bMarkerSeen](const FRedwoodTicketingUpdate &) { bMarkerSeen = true; }
    );
    Test.TestTrue(TEXT("The server sends the assignment"), Harness.Server->SendText(Assignment));
    Test.TestTrue(
      TEXT("The server sends the marker"),
      Harness.Server->SendText("42[\"realm:ticketing:update\",{\"message\":\"m\"}]")
    );
    // A handled assignment clears the ticketing delegate, so the marker is
    // not seen then; the connection is set in either case once it ran.
    PumpGameThreadUntil([&C, &bMarkerSeen]() {
      return bMarkerSeen || !FAccess::ServerConnection(C).IsEmpty();
    });
    // Let the marker run too when the assignment was handled.
    PumpGameThreadUntil([]() { return true; });
    FAccess::OnTicketingUpdate(C).Unbind();
    return !FAccess::ServerConnection(C).IsEmpty();
  }

  bool OpenWithRealmEvents(FAutomationTestBase &Test, FRealmHarness &Harness) {
    if (!Harness.Open(Test)) {
      return false;
    }
    FAccess::BindRealmEvents(Harness.Client());
    return true;
  }

  const char *const AssignmentWithoutCharacter =
    "42[\"realm:servers:connect-to-instance\","
    "{\"shouldStitch\":false,\"connection\":\"x:1\",\"token\":\"t\"}]";

  // M7. Pins: the bAbandonedQueueJoin check in the connect-to-instance handler.
  bool RunDroppedQueueRequestDoesNotMove(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenWithRealmEvents(Test, Harness)) {
      return false;
    }
    FAccess::bAbandonedQueueJoin(Harness.Client()) = true;
    Test.TestFalse(
      TEXT("An assignment for a dropped queue request does not move the player"),
      AssignmentMoves(Test, Harness, AssignmentWithoutCharacter)
    );
    return true;
  }

  // M6. Pins: the characterId check in the connect-to-instance handler, and
  // that an old server (no characterId) still moves the player.
  bool RunAssignmentForAnotherCharacter(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenWithRealmEvents(Test, Harness)) {
      return false;
    }
    Test.TestFalse(
      TEXT("An assignment for another character does not move the player"),
      AssignmentMoves(
        Test,
        Harness,
        "42[\"realm:servers:connect-to-instance\",{\"shouldStitch\":false,"
        "\"connection\":\"x:1\",\"token\":\"t\",\"characterId\":\"c-2\"}]"
      )
    );
    Test.TestTrue(
      TEXT("An assignment for the selected character moves the player"),
      AssignmentMoves(
        Test,
        Harness,
        "42[\"realm:servers:connect-to-instance\",{\"shouldStitch\":false,"
        "\"connection\":\"x:1\",\"token\":\"t\",\"characterId\":\"character-1\"}]"
      )
    );
    Test.TestTrue(
      TEXT("An assignment with no characterId moves the player as before"),
      AssignmentMoves(Test, Harness, AssignmentWithoutCharacter)
    );
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
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightZoneJoinAfterDropTest,
  "ZoneJoinSentAfterDropFailsAtReconnect",
  RedwoodInFlightTest::RunZoneJoinSentAfterDrop(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightDirectorDropDuringReloginTest,
  "DirectorDropDuringRealmRelogin",
  RedwoodInFlightTest::RunDirectorDropDuringRealmRelogin(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightDroppedQueueRequestTest,
  "DroppedQueueRequestDoesNotMove",
  RedwoodInFlightTest::RunDroppedQueueRequestDoesNotMove(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightOtherCharacterTest,
  "AssignmentForAnotherCharacterDoesNotMove",
  RedwoodInFlightTest::RunAssignmentForAnotherCharacter(*this)
)
