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
#include "RedwoodSettings.h"
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
  static void RequestRealmClose(FClient &C) {
    C.RequestRealmClose();
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
  static void InitiateRealmHandshake(FClient &C, const FRedwoodRealm &InRealm) {
    C.InitiateRealmHandshake(InRealm, FRedwoodSocketConnectedDelegate());
  }
  static void TrackDirectorReply(FClient &C, TFunction<void()> OnLost) {
    C.DirectorReplies.Track(FRedwoodReplyCallback(), MoveTemp(OnLost));
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

  // A Realm socket on the fake server, with copies of the callbacks that
  // InitiateRealmHandshake installs, and a logged-in player so the gates let a
  // request through. OpenThroughHandshake installs the real callbacks.
  struct FRealmHarness {
    TUniquePtr<FFakeSocketIoServer> Server = MakeUnique<FFakeSocketIoServer>();
    TStrongObjectPtr<URedwoodClientInterface> Interface{
      NewObject<URedwoodClientInterface>()
    };

    URedwoodClientInterface &Client() const {
      return *Interface.Get();
    }

    FAutomationTestBase *OpenedBy = nullptr;

    bool Open(FAutomationTestBase &Test) {
      OpenedBy = &Test;
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
      const bool bReleased = PumpGameThreadUntil([&Sockets]() {
        return Algo::AllOf(Sockets, [](const TSharedPtr<FSocketIONative> &S) {
          return S.GetSharedReferenceCount() == 1;
        });
      });
      if (OpenedBy) {
        OpenedBy->TestTrue(TEXT("The library releases the sockets"), bReleased);
      }
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

  // Sets the Director URI that InitializeDirectorConnection reads.
  struct FDirectorUriOverride {
    URedwoodSettings *Settings = GetMutableDefault<URedwoodSettings>();
    const FString OldUri = Settings->DirectorUri;
    const bool bOldJsonEnabled = Settings->bRedwoodJsonEnabled;

    explicit FDirectorUriOverride(int32 Port) {
      Settings->DirectorUri = FString::Printf(TEXT("ws://127.0.0.1:%d"), Port);
      Settings->bRedwoodJsonEnabled = false;
    }
    ~FDirectorUriOverride() {
      Settings->DirectorUri = OldUri;
      Settings->bRedwoodJsonEnabled = bOldJsonEnabled;
    }
  };

  // The production sockets keep ReconnectionDelay (5000 ms), the edge of one
  // step, so a reconnect gets two.
  bool AcceptReconnect(FFakeSocketIoServer &Server) {
    return (Server.WaitForConnection() || Server.WaitForConnection()) &&
      Server.AcceptSession();
  }

  // The Director socket with the callbacks of InitializeDirectorConnection.
  bool OpenProductionDirector(
    FAutomationTestBase &Test, URedwoodClientInterface &C, FFakeSocketIoServer &Server
  ) {
    if (!Test.TestTrue(TEXT("Director server listens"), Server.Listen())) {
      return false;
    }
    {
      FDirectorUriOverride Uri(Server.Port);
      C.InitializeDirectorConnection(FRedwoodSocketConnectedDelegate());
    }
    return Test.TestTrue(TEXT("Director opens a session"), Server.AcceptSession()) &&
      Test.TestTrue(
        TEXT("Director connects"),
        PumpGameThreadUntil([&C]() { return FAccess::bSentDirectorConnected(C); })
      );
  }

  // Runs the real handshake, so the Realm socket has the callbacks that
  // InitiateRealmHandshake installs. The Director answers the token request.
  bool OpenThroughHandshake(
    FAutomationTestBase &Test, FRealmHarness &Harness, FFakeSocketIoServer &DirectorServer
  ) {
    Harness.OpenedBy = &Test;
    URedwoodClientInterface &C = Harness.Client();
    if (!Test.TestTrue(TEXT("Test server listens"), Harness.Server->Listen()) ||
        !OpenProductionDirector(Test, C, DirectorServer)) {
      return false;
    }
    FAccess::PlayerId(C) = TEXT("player-1");
    FAccess::AuthToken(C) = TEXT("token-1");
    FAccess::SelectedCharacterId(C) = TEXT("character-1");
    FAccess::bAuthenticated(C) = true;

    FRedwoodRealm InRealm;
    InRealm.Id = TEXT("realm-1");
    InRealm.Uri = FString::Printf(TEXT("ws://127.0.0.1:%d"), Harness.Server->Port);
    FAccess::InitiateRealmHandshake(C, InRealm);

    // An event with an ack: 42<ack id>["name",{...}].
    FString Request;
    if (!Test.TestTrue(
          TEXT("The token request reaches the Director"),
          DirectorServer.ReadClientText(Request)
        )) {
      return false;
    }
    const int32 ArgsStart = Request.Find(TEXT("["));
    const FString AckId = Request.Mid(2, ArgsStart - 2);
    if (!Test.TestTrue(
          TEXT("The token request has an ack id"),
          Request.StartsWith(TEXT("42")) && !AckId.IsEmpty() && AckId.IsNumeric()
        )) {
      return false;
    }
    const FString Reply = FString::Printf(
      TEXT("43%s[{\"error\":\"\",\"token\":\"realm-token\"}]"), *AckId
    );
    return Test.TestTrue(
             TEXT("The Director answers"),
             DirectorServer.SendText(StringCast<ANSICHAR>(*Reply).Get())
           ) &&
      // The reply runs on the game thread, which AcceptSession blocks.
      Test.TestTrue(
        TEXT("The reply creates the Realm socket"),
        PumpGameThreadUntil([&C]() { return FAccess::Realm(C).IsValid(); })
      ) &&
      Test.TestTrue(TEXT("Realm opens a session"), Harness.Server->AcceptSession()) &&
      Test.TestTrue(
        TEXT("Realm connects"),
        PumpGameThreadUntil([&C]() {
          return FAccess::bSentRealmConnected(C) && !FAccess::bRealmDisconnected(C);
        })
      );
  }

  // Review focus: JoinQueue is not held by a gate, so one sent after the drop
  // was not pending when the drop failed the others. It must fail at the
  // reconnect. Pins: the NoteRealmReconnected call in the real Realm
  // connected callback.
  bool RunZoneJoinSentAfterDrop(FAutomationTestBase &Test) {
    bool bAnswered = false;
    FString Error;
    FRealmHarness Harness;
    // After the harness, so its connection closes before the harness waits
    // for the sockets, as in DirectorDropDuringRealmRelogin.
    FFakeSocketIoServer DirectorServer;
    if (!OpenThroughHandshake(Test, Harness, DirectorServer)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    // The real callback logs the drop as an error.
    Test.AddExpectedError(
      TEXT("Lost connection to Realm"), EAutomationExpectedErrorFlags::Contains, 1
    );
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

    Test.TestTrue(TEXT("The client reconnects"), AcceptReconnect(*Harness.Server));
    PumpGameThreadUntil([&bAnswered]() { return bAnswered; });
    ExpectClearError(Test, TEXT("Zone join sent after the drop"), bAnswered, Error);
    Test.TestTrue(
      TEXT("The lost join is dropped until a new join"),
      FAccess::bAbandonedQueueJoin(C)
    );
    return true;
  }

  // The Director side of the same gap. Pins: the NoteDirectorReconnected call
  // in the real Director connected callback.
  bool RunDirectorReconnectFailsPendingReply(FAutomationTestBase &Test) {
    bool bLost = false;
    FRealmHarness Harness;
    // After the harness, so its connection closes before the harness waits
    // for the sockets, as in DirectorDropDuringRealmRelogin.
    FFakeSocketIoServer DirectorServer;
    Harness.OpenedBy = &Test;
    URedwoodClientInterface &C = Harness.Client();
    if (!OpenProductionDirector(Test, C, DirectorServer)) {
      return false;
    }
    // The real callback logs the drop as an error.
    Test.AddExpectedError(
      TEXT("Lost connection to Director"), EAutomationExpectedErrorFlags::Contains, 1
    );
    Test.TestTrue(
      TEXT("The Director closes"), DirectorServer.CloseWithCode(ServiceRestartCloseCode)
    );
    if (!Test.TestTrue(
          TEXT("The client notes the Director drop"),
          PumpGameThreadUntil([&C]() { return FAccess::bDirectorDisconnected(C); })
        )) {
      return false;
    }

    // Stands in for a request sent between the drop and the reconnect.
    FAccess::TrackDirectorReply(C, [&bLost]() { bLost = true; });
    Test.TestTrue(TEXT("The Director reconnects"), AcceptReconnect(DirectorServer));
    Test.TestTrue(
      TEXT("The reconnect fails the pending request"),
      PumpGameThreadUntil([&bLost]() { return bLost; })
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
    URedwoodClientInterface &C = Harness.Client();
    FAccess::bAbandonedQueueJoin(C) = true;
    Test.TestFalse(
      TEXT("An assignment for a dropped queue request does not move the player"),
      AssignmentMoves(Test, Harness, AssignmentWithoutCharacter)
    );

    // The server answers a leave before it runs it, so only a new join may
    // let an assignment through again. Pins: the reset in JoinQueue.
    C.JoinQueue(
      TEXT("proxy-1"), TEXT("zone-1"), false, false, FRedwoodTicketingUpdateDelegate()
    );
    Test.TestTrue(
      TEXT("After a new join, an assignment moves the player"),
      AssignmentMoves(Test, Harness, AssignmentWithoutCharacter)
    );
    return true;
  }

  // Logout and a failed Director re-login close the Realm socket on purpose.
  // Pins: the FailAll in RequestRealmClose.
  bool RunPartyInviteOnRequestedClose(FAutomationTestBase &Test) {
    bool bAnswered = false;
    FString Error;
    int32 CloseReports = 0;
    FRealmHarness Harness;
    if (!Harness.Open(Test)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    C.InviteToParty(
      TEXT("player-2"),
      FRedwoodErrorOutputDelegate::CreateLambda([&](const FString &InError) {
        bAnswered = true;
        Error = InError;
      })
    );
    Test.TestTrue(TEXT("The request reaches the server"), Harness.Server->ReadClientFrame());
    // The library reports a close we ask for twice: from Disconnect(), and
    // when the close handshake ends. Its close timer runs until then, so the
    // test waits for the second report before it frees the socket.
    TSharedPtr<FSocketIONative> &Realm = FAccess::Realm(C);
    TFunction<void(const ESIOConnectionCloseReason)> Handler =
      Realm->OnDisconnectedCallback;
    Realm->OnDisconnectedCallback =
      [Handler, &CloseReports](const ESIOConnectionCloseReason Reason) {
        Handler(Reason);
        ++CloseReports;
      };

    FAccess::RequestRealmClose(C);
    ExpectClearError(Test, TEXT("Party invite at a requested close"), bAnswered, Error);
    Test.TestTrue(TEXT("Server acknowledges the close"), Harness.Server->CloseNormally());
    Test.TestTrue(
      TEXT("The close handshake ends"),
      PumpGameThreadUntil([&CloseReports]() { return CloseReports >= 2; })
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
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightRequestedCloseTest,
  "PartyInviteFailsOnRequestedClose",
  RedwoodInFlightTest::RunPartyInviteOnRequestedClose(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightDirectorReconnectTest,
  "DirectorReconnectFailsPendingReply",
  RedwoodInFlightTest::RunDirectorReconnectFailsPendingReply(*this)
)
