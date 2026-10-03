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

#include "CharacterFriendAlertListener.h"
#include "FakeSocketIoServer.h"
#include "RedwoodClientInterface.h"
#include "RedwoodClosingSockets.h"
#include "RedwoodCommonGameSubsystem.h"
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
  static void TrackRealmReply(FClient &C, TFunction<void()> OnLost) {
    C.RealmReplies.Track(FRedwoodReplyCallback(), MoveTemp(OnLost));
  }
  static void SendOwedLeave(FClient &C) {
    C.SendOwedLeave();
  }
  static int32 NumRealmReplies(FClient &C) {
    return C.RealmReplies.Num();
  }
  static bool &bAssignmentExpected(FClient &C) {
    return C.bAssignmentExpected;
  }
  static FRedwoodParty &CurrentParty(FClient &C) {
    return C.CurrentParty;
  }
  static bool &bTravelPending(FClient &C) {
    return C.bTravelPending;
  }
  static bool &bInWorld(FClient &C) {
    return C.bInWorld;
  }
  static bool &bRealmCloseRequested(FClient &C) {
    return C.bRealmCloseRequested;
  }
  static bool &bRealmCloseTimerPending(FClient &C) {
    return C.bRealmCloseTimerPending;
  }
  static bool &bLeaveTicketingOwed(FClient &C) {
    return C.bLeaveTicketingOwed;
  }
  static int32 NumDirectorReplies(FClient &C) {
    return C.DirectorReplies.Num();
  }
  static int32 NumRealmHeldRequests(FClient &C) {
    return C.RealmHeldRequests.Num();
  }
  static FString &AcceptedPartyTicketId(FClient &C) {
    return C.AcceptedPartyTicketId;
  }
  static TArray<FString> &EndedPartyTicketIds(FClient &C) {
    return C.EndedPartyTicketIds;
  }
  static void EndRealmReauthentication(FClient &C, bool bSucceeded) {
    C.EndRealmReauthentication(bSucceeded);
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
      // A socket still connected when Deinitialize closes it starts the
      // library's 3 s close timer, which can outlive the freed socket. Every
      // server is gone now, so wait until the sockets see it.
      const bool bDropped = PumpGameThreadUntil([&Sockets]() {
        return Algo::AllOf(Sockets, [](const TSharedPtr<FSocketIONative> &S) {
          return !S->bIsConnected;
        });
      });
      if (OpenedBy) {
        OpenedBy->TestTrue(TEXT("The sockets see the servers go"), bDropped);
      }
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
    // The real callbacks log each drop as an error, the teardown's too.
    Test.AddExpectedError(
      TEXT("Lost connection to"), EAutomationExpectedErrorFlags::Contains, 0
    );
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

  // Reads the next request, an event with an ack (42<ack id>["name",{...}]),
  // and answers it. OutRequest is the request's text.
  bool AnswerRequest(
    FAutomationTestBase &Test,
    FFakeSocketIoServer &Server,
    const TCHAR *ReplyJson,
    FString &OutRequest
  ) {
    if (!Test.TestTrue(TEXT("A request arrives"), Server.ReadClientText(OutRequest))) {
      return false;
    }
    const int32 ArgsStart = OutRequest.Find(TEXT("["));
    const FString AckId = OutRequest.Mid(2, ArgsStart - 2);
    if (!Test.TestTrue(
          TEXT("The request has an ack id"),
          OutRequest.StartsWith(TEXT("42")) && !AckId.IsEmpty() && AckId.IsNumeric()
        )) {
      return false;
    }
    const FString Reply = FString::Printf(TEXT("43%s[%s]"), *AckId, ReplyJson);
    return Test.TestTrue(
      TEXT("The server answers"), Server.SendText(StringCast<ANSICHAR>(*Reply).Get())
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
    FAccess::bAuthenticated(C) = true;
    // What an earlier session left. The server can replay an assignment for
    // it. Pins: the reset in InitiateRealmHandshake.
    FAccess::SelectedCharacterId(C) = TEXT("character-old");
    FAccess::bAssignmentExpected(C) = true;
    FAccess::CurrentParty(C).bValid = true;
    FAccess::AcceptedPartyTicketId(C) = TEXT("ticket-old");
    FAccess::EndedPartyTicketIds(C).Add(TEXT("ticket-ended"));

    FRedwoodRealm InRealm;
    InRealm.Id = TEXT("realm-1");
    InRealm.Uri = FString::Printf(TEXT("ws://127.0.0.1:%d"), Harness.Server->Port);
    FAccess::InitiateRealmHandshake(C, InRealm);
    Test.TestTrue(
      TEXT("A new handshake forgets the old character"),
      FAccess::SelectedCharacterId(C).IsEmpty() && !FAccess::bAssignmentExpected(C)
    );
    Test.TestFalse(
      TEXT("A new handshake forgets the old party"), FAccess::CurrentParty(C).bValid
    );
    Test.TestTrue(
      TEXT("A new handshake forgets the party tickets"),
      FAccess::AcceptedPartyTicketId(C).IsEmpty() &&
        FAccess::EndedPartyTicketIds(C).IsEmpty()
    );
    FAccess::SelectedCharacterId(C) = TEXT("character-1");

    FString Request;
    return AnswerRequest(
             Test,
             DirectorServer,
             TEXT("{\"error\":\"\",\"token\":\"realm-token\"}"),
             Request
           ) &&
      Test.TestTrue(
        TEXT("The Director got the token request"),
        Request.Contains(TEXT("realm:auth:player:connect:client-to-director"))
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

  // Pins: the NoteRealmReconnected call in the real Realm connected callback
  // (the pending entry stands in for a request sent in the gap), and that the
  // requests the gates do not hold fail at once while the socket is down and
  // in the re-login window after it, where no route answers them.
  bool RunRealmReconnectAndReloginWindow(FAutomationTestBase &Test) {
    bool bLost = false;
    bool bWriteAnswered = false;
    // Before the harness: a request that did go out keeps its callbacks
    // until the teardown fails them.
    bool bJoinAnswered = false;
    bool bListAnswered = false;
    FString JoinError;
    FString ListError;
    FRealmHarness Harness;
    // After the harness, so its connection closes before the harness waits
    // for the sockets, as in DirectorDropDuringRealmRelogin.
    FFakeSocketIoServer DirectorServer;
    if (!OpenThroughHandshake(Test, Harness, DirectorServer)) {
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

    auto ExpectFailsAtOnce = [&](const TCHAR *When) {
      bJoinAnswered = false;
      bListAnswered = false;
      C.JoinQueue(
        TEXT("proxy-1"),
        TEXT("zone-1"),
        false,
        false,
        FRedwoodTicketingUpdateDelegate::CreateLambda(
          [&](const FRedwoodTicketingUpdate &Update) {
            bJoinAnswered = true;
            JoinError = Update.Message;
          }
        )
      );
      C.ListProxies(
        TArray<FString>(),
        FRedwoodListProxiesOutputDelegate::CreateLambda(
          [&](const FRedwoodListProxiesOutput &Output) {
            bListAnswered = true;
            ListError = Output.Error;
          }
        )
      );
      Test.TestTrue(
        *FString::Printf(TEXT("A zone join %s fails at once"), When),
        bJoinAnswered && !JoinError.IsEmpty()
      );
      Test.TestTrue(
        *FString::Printf(TEXT("A proxy list %s fails at once"), When),
        bListAnswered && !ListError.IsEmpty()
      );
      Test.TestEqual(
        *FString::Printf(TEXT("Nothing %s waits for a reply"), When),
        FAccess::NumRealmReplies(C),
        0
      );
    };
    ExpectFailsAtOnce(TEXT("while the socket is down"));

    FAccess::TrackRealmReply(C, [&bLost]() { bLost = true; });
    Test.TestTrue(TEXT("The client reconnects"), AcceptReconnect(*Harness.Server));
    Test.TestTrue(
      TEXT("The reconnect fails the pending request"),
      PumpGameThreadUntil([&bLost]() { return bLost; })
    );
    // The re-login now waits for a Director token, which never comes here.
    if (!Test.TestTrue(
          TEXT("The socket is back and the re-login waits"),
          !FAccess::bRealmDisconnected(C) && FAccess::bRealmReauthPending(C)
        )) {
      return false;
    }
    ExpectFailsAtOnce(TEXT("in the re-login window"));
    Test.TestFalse(
      TEXT("No join was lost, so none is dropped"), FAccess::bAbandonedQueueJoin(C)
    );

    // Blueprint-only writes wait for the re-login. Pins: GateRealm in
    // SetCharacterData.
    C.SetCharacterData(
      TEXT("character-1"),
      TEXT("Name"),
      nullptr,
      FRedwoodGetCharacterOutputDelegate::CreateLambda(
        [&bWriteAnswered](const FRedwoodGetCharacterOutput &) { bWriteAnswered = true; }
      )
    );
    Test.TestFalse(TEXT("A character write is not failed"), bWriteAnswered);
    Test.TestEqual(
      TEXT("The character write is held for the re-login"),
      FAccess::NumRealmHeldRequests(C),
      1
    );

    // The re-login completes and pays an owed leave. Pins: SendOwedLeave on
    // the re-auth path.
    FAccess::bLeaveTicketingOwed(C) = true;
    FString Request;
    if (!AnswerRequest(
          Test,
          DirectorServer,
          TEXT("{\"error\":\"\",\"token\":\"realm-token-2\"}"),
          Request
        ) ||
        !Test.TestTrue(
          TEXT("The re-login asks the Director for a token"),
          Request.Contains(TEXT("realm:auth:player:connect:client-to-director"))
        ) ||
        !Test.TestTrue(
          TEXT("The token reply sends the Realm auth"),
          PumpGameThreadUntil([&Harness]() { return Harness.Server->HasClientData(); })
        ) ||
        !AnswerRequest(Test, *Harness.Server, TEXT("{\"error\":\"\"}"), Request) ||
        !Test.TestTrue(
          TEXT("The Realm got the auth request"),
          Request.Contains(TEXT("realm:auth:player:connect:client-to-realm"))
        ) ||
        !Test.TestTrue(
          TEXT("The auth reply sends the owed leave"),
          PumpGameThreadUntil([&Harness]() { return Harness.Server->HasClientData(); })
        ) ||
        !AnswerRequest(Test, *Harness.Server, TEXT("{\"error\":\"\"}"), Request)) {
      return false;
    }
    Test.TestTrue(
      TEXT("The first request after the re-auth is the leave"),
      Request.Contains(TEXT("realm:ticketing:leave"))
    );
    Test.TestTrue(
      TEXT("A successful leave pays it"),
      PumpGameThreadUntil([&C]() { return !FAccess::bLeaveTicketingOwed(C); })
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
    C.TrackDirectorReply(FRedwoodReplyCallback(), [&bLost]() { bLost = true; });
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
    // As for a player in the world.
    FAccess::bAssignmentExpected(Harness.Client()) = true;
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
    bool bNamespaceClosed = false;
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
    // The library's 3 s close timer reports the namespace closed when it
    // runs; the interface's own handler for it stays first.
    TFunction<void(const FString &)> OnNamespaceClosed =
      Realm->OnNamespaceDisconnectedCallback;
    Realm->OnNamespaceDisconnectedCallback =
      [OnNamespaceClosed, &bNamespaceClosed](const FString &Namespace) {
        if (OnNamespaceClosed) {
          OnNamespaceClosed(Namespace);
        }
        bNamespaceClosed = true;
      };
    ExpectClearError(Test, TEXT("Party invite at a requested close"), bAnswered, Error);
    Test.TestTrue(TEXT("Server acknowledges the close"), Harness.Server->CloseNormally());
    Test.TestTrue(
      TEXT("The close handshake ends"),
      PumpGameThreadUntil([&CloseReports]() { return CloseReports >= 2; })
    );
    Test.TestTrue(
      TEXT("The close timer ran"),
      PumpGameThreadUntil([&bNamespaceClosed]() { return bNamespaceClosed; })
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

  const char *const AssignmentForSelectedCharacter =
    "42[\"realm:servers:connect-to-instance\",{\"shouldStitch\":false,"
    "\"connection\":\"x:1\",\"token\":\"t\",\"characterId\":\"character-1\"}]";

  // A replay reaches a player at character select with no join out. Pins:
  // the bAssignmentExpected check, and that a join sets it.
  bool RunAssignmentWithoutJoin(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenWithRealmEvents(Test, Harness)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    FAccess::bAssignmentExpected(C) = false;
    Test.TestFalse(
      TEXT("With no join out, an assignment does not move the player"),
      AssignmentMoves(Test, Harness, AssignmentForSelectedCharacter)
    );
    C.JoinQueue(
      TEXT("proxy-1"), TEXT("zone-1"), false, false, FRedwoodTicketingUpdateDelegate()
    );
    Test.TestTrue(
      TEXT("After a join, an assignment moves the player"),
      AssignmentMoves(Test, Harness, AssignmentForSelectedCharacter)
    );

    // Back at character select with another character. Pins: the reset in
    // SetSelectedCharacter.
    FAccess::AcceptedPartyTicketId(C) = TEXT("ticket-1");
    FAccess::EndedPartyTicketIds(C).Add(TEXT("ticket-0"));
    C.SetSelectedCharacter(TEXT("character-2"));
    Test.TestFalse(
      TEXT("A new character expects no assignment until it joins"),
      FAccess::bAssignmentExpected(C)
    );
    Test.TestTrue(
      TEXT("A new character forgets the old character's party join"),
      FAccess::AcceptedPartyTicketId(C).IsEmpty()
    );
    Test.TestTrue(
      TEXT("An ended ticket stays ended for a new character"),
      FAccess::EndedPartyTicketIds(C).Contains(TEXT("ticket-0"))
    );
    return true;
  }

  // A create that joins its session gets a connect-to-instance, which must
  // move the player. Pins: NoteJoinSent in CreateProxy.
  bool RunCreateProxyJoinMoves(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenWithRealmEvents(Test, Harness)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    FAccess::bAssignmentExpected(C) = false;
    C.CreateProxy(true, FRedwoodCreateProxyInput(), FRedwoodCreateProxyOutputDelegate());
    Test.TestTrue(
      TEXT("The assignment of a create that joins moves the player"),
      AssignmentMoves(Test, Harness, AssignmentForSelectedCharacter)
    );
    return true;
  }

  // HollowedOath#3002. When the leader queues the whole party, the server
  // tells each other member with realm:ticketing:party-queued, and the
  // party ticket's assignment names its ticketId.
  FString PartyQueued(const TCHAR *CharacterId, const TCHAR *TicketId, const TCHAR *MessageId) {
    return FString::Printf(
      TEXT("42[\"realm:ticketing:party-queued\",{\"leaderId\":\"player-2\","
           "\"characterId\":\"%s\",\"ticketId\":\"%s\",\"messageId\":\"%s\"}]"),
      CharacterId,
      TicketId,
      MessageId
    );
  }

  FString PartyLeft(const TCHAR *TicketId, const TCHAR *MessageId) {
    return FString::Printf(
      TEXT("42[\"realm:ticketing:party-left\",{\"leaderId\":\"player-2\","
           "\"ticketId\":\"%s\",\"messageId\":\"%s\"}]"),
      TicketId,
      MessageId
    );
  }

  // Sends a Realm event and waits until the client handled it: events run
  // in order, so the marker's handler runs after it.
  void SendEvent(FAutomationTestBase &Test, FRealmHarness &Harness, const FString &Event) {
    URedwoodClientInterface &C = Harness.Client();
    bool bMarkerSeen = false;
    FAccess::OnTicketingUpdate(C) = FRedwoodTicketingUpdateDelegate::CreateLambda(
      [&bMarkerSeen](const FRedwoodTicketingUpdate &) { bMarkerSeen = true; }
    );
    Test.TestTrue(
      TEXT("The server sends the event"),
      Harness.Server->SendText(StringCast<ANSICHAR>(*Event).Get()) &&
        Harness.Server->SendText("42[\"realm:ticketing:update\",{\"message\":\"m\"}]")
    );
    Test.TestTrue(
      TEXT("The client handles the event"),
      PumpGameThreadUntil([&bMarkerSeen]() { return bMarkerSeen; })
    );
    FAccess::OnTicketingUpdate(C).Unbind();
  }

  // The party ticket's assignment for the selected character. An empty
  // TicketId sends a solo assignment.
  bool PartyAssignmentMoves(
    FAutomationTestBase &Test, FRealmHarness &Harness, const TCHAR *TicketId
  ) {
    const FString TicketField = *TicketId
      ? FString::Printf(TEXT(",\"ticketId\":\"%s\""), TicketId)
      : FString();
    const FString Assignment = FString::Printf(
      TEXT("42[\"realm:servers:connect-to-instance\",{\"shouldStitch\":false,"
           "\"connection\":\"x:1\",\"token\":\"t\",\"characterId\":\"character-1\"%s}]"),
      *TicketField
    );
    // The accepted travel stays pending in this harness; clear it, so each
    // assignment is judged alone.
    FAccess::bTravelPending(Harness.Client()) = false;
    return AssignmentMoves(Test, Harness, StringCast<ANSICHAR>(*Assignment).Get());
  }

  // A member (not the leader) at character select, with no join of its own.
  bool OpenAsPartyMember(FAutomationTestBase &Test, FRealmHarness &Harness) {
    if (!OpenWithRealmEvents(Test, Harness)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    FAccess::bAssignmentExpected(C) = false;
    FRedwoodParty &Party = FAccess::CurrentParty(C);
    Party.bValid = true;
    Party.LeaderId = TEXT("player-2");
    Party.Members.AddDefaulted_GetRef().PlayerId = TEXT("player-1");
    Party.Members.AddDefaulted_GetRef().PlayerId = TEXT("player-2");
    return true;
  }

  // A member relaunched its client within 60 s: the fresh client heard no
  // party notice, so the server's replay of the old party assignment must
  // not move it, though the party lists it. Pins: no party-member rule.
  bool RunRelaunchedMemberRefusesReplay(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenAsPartyMember(Test, Harness)) {
      return false;
    }
    Test.TestFalse(
      TEXT("A replayed party assignment does not move a fresh member"),
      PartyAssignmentMoves(Test, Harness, TEXT("ticket-1"))
    );
    Test.TestFalse(
      TEXT("A replayed assignment with no ticket does not move a fresh member"),
      PartyAssignmentMoves(Test, Harness, TEXT(""))
    );
    return true;
  }

  // Pins: HandlePartyQueued accepts a new ticket as this member's join, and
  // an accepted assignment ends its ticket.
  bool RunPartyQueuedMovesMember(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenAsPartyMember(Test, Harness)) {
      return false;
    }
    SendEvent(Test, Harness, PartyQueued(TEXT("character-1"), TEXT("ticket-1"), TEXT("m-1")));
    Test.TestTrue(
      TEXT("After the party notice, the party's assignment moves the member at character select"),
      PartyAssignmentMoves(Test, Harness, TEXT("ticket-1"))
    );
    // The server sends the kept "queued" again after an admission (#3031).
    SendEvent(Test, Harness, PartyQueued(TEXT("character-1"), TEXT("ticket-1"), TEXT("m-2")));
    Test.TestFalse(
      TEXT("A replay of the used assignment does not move the member again"),
      PartyAssignmentMoves(Test, Harness, TEXT("ticket-1"))
    );
    return true;
  }

  // Pins: the repeat checks of HandlePartyQueued (the accepted ticket, and
  // the messageId).
  bool RunDuplicatePartyQueuedNoChange(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenAsPartyMember(Test, Harness)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    SendEvent(Test, Harness, PartyQueued(TEXT("character-1"), TEXT("ticket-1"), TEXT("m-1")));
    // The enter-world timer leaves, in the re-login window so it fails at
    // once and drops the join.
    FAccess::bRealmReauthPending(C) = true;
    C.LeaveTicketing(FRedwoodErrorOutputDelegate());
    FAccess::bRealmReauthPending(C) = false;
    // A worker pass sends the same ticket again, with a new messageId.
    SendEvent(Test, Harness, PartyQueued(TEXT("character-1"), TEXT("ticket-1"), TEXT("m-2")));
    Test.TestTrue(
      TEXT("A repeat of the accepted ticket keeps the dropped join"),
      FAccess::bAbandonedQueueJoin(C)
    );
    Test.TestFalse(
      TEXT("After the game left, the party's assignment does not move the member"),
      PartyAssignmentMoves(Test, Harness, TEXT("ticket-1"))
    );

    SendEvent(Test, Harness, PartyQueued(TEXT("character-1"), TEXT("ticket-2"), TEXT("m-3")));
    // Redis sends the first notice again.
    SendEvent(Test, Harness, PartyQueued(TEXT("character-1"), TEXT("ticket-1"), TEXT("m-1")));
    Test.TestEqual(
      TEXT("A resent old notice does not replace the newer ticket"),
      FAccess::AcceptedPartyTicketId(C),
      FString(TEXT("ticket-2"))
    );
    // A stale kept "queued" of the replaced ticket (#3031), with a new id.
    SendEvent(Test, Harness, PartyQueued(TEXT("character-1"), TEXT("ticket-1"), TEXT("m-4")));
    Test.TestEqual(
      TEXT("A replaced ticket does not come back"),
      FAccess::AcceptedPartyTicketId(C),
      FString(TEXT("ticket-2"))
    );
    Test.TestTrue(
      TEXT("The newer ticket's assignment moves the member"),
      PartyAssignmentMoves(Test, Harness, TEXT("ticket-2"))
    );
    return true;
  }

  // Pins: the characterId check of HandlePartyQueued.
  bool RunPartyQueuedForAnotherCharacter(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenAsPartyMember(Test, Harness)) {
      return false;
    }
    SendEvent(Test, Harness, PartyQueued(TEXT("c-2"), TEXT("ticket-1"), TEXT("m-1")));
    Test.TestFalse(
      TEXT("A notice for another character expects no assignment"),
      FAccess::bAssignmentExpected(Harness.Client())
    );
    Test.TestFalse(
      TEXT("After a notice for another character, the assignment does not move the member"),
      PartyAssignmentMoves(Test, Harness, TEXT("ticket-1"))
    );
    return true;
  }

  // Pins: HandlePartyLeft drops the accepted ticket's join.
  bool RunPartyLeftDropsJoin(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenAsPartyMember(Test, Harness)) {
      return false;
    }
    SendEvent(Test, Harness, PartyQueued(TEXT("character-1"), TEXT("ticket-1"), TEXT("m-1")));
    SendEvent(Test, Harness, PartyLeft(TEXT("ticket-1"), TEXT("m-2")));
    Test.TestFalse(
      TEXT("After the party ticket ends, no assignment is expected"),
      FAccess::bAssignmentExpected(Harness.Client())
    );
    Test.TestFalse(
      TEXT("After the party ticket ends, its assignment does not move the member"),
      PartyAssignmentMoves(Test, Harness, TEXT("ticket-1"))
    );
    return true;
  }

  // Pins: HandlePartyLeft acts only on the ticket it names.
  bool RunPartyLeftOlderTicketKeepsNewer(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenAsPartyMember(Test, Harness)) {
      return false;
    }
    SendEvent(Test, Harness, PartyQueued(TEXT("character-1"), TEXT("ticket-1"), TEXT("m-1")));
    SendEvent(Test, Harness, PartyQueued(TEXT("character-1"), TEXT("ticket-2"), TEXT("m-2")));
    SendEvent(Test, Harness, PartyLeft(TEXT("ticket-1"), TEXT("m-3")));
    Test.TestTrue(
      TEXT("A late end of an older ticket keeps the newer ticket's assignment"),
      PartyAssignmentMoves(Test, Harness, TEXT("ticket-2"))
    );
    return true;
  }

  // A stale "queued" after its ticket ended (#3008, #3031). Pins: the ended
  // set in HandlePartyQueued.
  bool RunPartyQueuedForEndedTicket(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenAsPartyMember(Test, Harness)) {
      return false;
    }
    SendEvent(Test, Harness, PartyLeft(TEXT("ticket-1"), TEXT("m-1")));
    SendEvent(Test, Harness, PartyQueued(TEXT("character-1"), TEXT("ticket-1"), TEXT("m-2")));
    Test.TestFalse(
      TEXT("A notice for an ended ticket expects no assignment"),
      FAccess::bAssignmentExpected(Harness.Client())
    );
    Test.TestFalse(
      TEXT("An ended ticket's assignment does not move the member"),
      PartyAssignmentMoves(Test, Harness, TEXT("ticket-1"))
    );
    return true;
  }

  // Pins: the party-ticket check of the assignment guard.
  bool RunOtherTicketAssignmentRefused(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenAsPartyMember(Test, Harness)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    SendEvent(Test, Harness, PartyQueued(TEXT("character-1"), TEXT("ticket-2"), TEXT("m-1")));
    Test.TestFalse(
      TEXT("An older party ticket's assignment does not move the member"),
      PartyAssignmentMoves(Test, Harness, TEXT("ticket-1"))
    );
    Test.TestFalse(
      TEXT("At character select, an assignment with no ticket does not move a member that waits for its party"),
      PartyAssignmentMoves(Test, Harness, TEXT(""))
    );
    FAccess::bInWorld(C) = true;
    Test.TestTrue(
      TEXT("In the world, a zone transfer with no ticket moves the member"),
      PartyAssignmentMoves(Test, Harness, TEXT(""))
    );
    FAccess::bInWorld(C) = false;
    Test.TestTrue(
      TEXT("The accepted ticket's assignment moves the member"),
      PartyAssignmentMoves(Test, Harness, TEXT("ticket-2"))
    );
    return true;
  }

  // The server replays the kept "queued" at the realm join, before the
  // relaunched member selects a character. Pins: PendingPartyTicketId.
  bool RunPartyQueuedBeforeCharacterSelect(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenWithRealmEvents(Test, Harness)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    FAccess::bAssignmentExpected(C) = false;
    FAccess::SelectedCharacterId(C).Reset();
    SendEvent(Test, Harness, PartyQueued(TEXT("character-1"), TEXT("ticket-1"), TEXT("m-1")));
    C.SetSelectedCharacter(TEXT("character-2"));
    Test.TestFalse(
      TEXT("Another character does not take the held notice"),
      FAccess::bAssignmentExpected(C)
    );
    C.SetSelectedCharacter(TEXT("character-1"));
    Test.TestTrue(
      TEXT("Selecting the notice's character moves the member with the party"),
      PartyAssignmentMoves(Test, Harness, TEXT("ticket-1"))
    );
    return true;
  }

  // The member's own join is still out when the party ticket comes: its own
  // ticket must be left, or its solo assignment could move the player after
  // the party's travel. Pins: the own-join check in AcceptPartyTicket.
  bool RunPartyNoticeLeavesOwnJoin(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenAsPartyMember(Test, Harness)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    C.JoinQueue(
      TEXT("proxy-1"), TEXT("zone-1"), false, false, FRedwoodTicketingUpdateDelegate()
    );
    Test.TestTrue(TEXT("The own join reaches the server"), Harness.Server->ReadClientFrame());
    SendEvent(Test, Harness, PartyQueued(TEXT("character-1"), TEXT("ticket-1"), TEXT("m-1")));
    FString Request;
    Test.TestTrue(
      TEXT("The party notice leaves the member's own ticket"),
      Harness.Server->ReadClientText(Request) &&
        Request.Contains(TEXT("realm:ticketing:leave"))
    );
    return true;
  }

  // The player's own join replaces a party notice: a solo assignment has no
  // ticket, and a leader's names a ticket it was never told of. Pins: the
  // reset in NoteJoinSent.
  bool RunOwnJoinAfterPartyNoticeMoves(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenAsPartyMember(Test, Harness)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    SendEvent(Test, Harness, PartyQueued(TEXT("character-1"), TEXT("ticket-1"), TEXT("m-1")));
    C.JoinQueue(
      TEXT("proxy-1"), TEXT("zone-1"), false, false, FRedwoodTicketingUpdateDelegate()
    );
    Test.TestTrue(
      TEXT("After its own join, a solo assignment moves the player"),
      PartyAssignmentMoves(Test, Harness, TEXT(""))
    );
    C.JoinQueue(
      TEXT("proxy-1"), TEXT("zone-1"), true, false, FRedwoodTicketingUpdateDelegate()
    );
    Test.TestTrue(
      TEXT("After its own whole-party join, the leader's party assignment moves it"),
      PartyAssignmentMoves(Test, Harness, TEXT("ticket-9"))
    );
    return true;
  }

  // A dropped join, then a create that joins: its assignment must move the
  // player. Pins: the dropped-join reset in NoteJoinSent.
  bool RunCreateProxyAfterDroppedJoin(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenWithRealmEvents(Test, Harness)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    // In the re-login window, so the leave fails at once and drops the join.
    FAccess::bRealmReauthPending(C) = true;
    C.LeaveTicketing(FRedwoodErrorOutputDelegate());
    FAccess::bRealmReauthPending(C) = false;
    Test.TestTrue(TEXT("The leave drops the join"), FAccess::bAbandonedQueueJoin(C));
    C.CreateProxy(true, FRedwoodCreateProxyInput(), FRedwoodCreateProxyOutputDelegate());
    Test.TestTrue(
      TEXT("After a dropped join, a create that joins moves the player"),
      AssignmentMoves(Test, Harness, AssignmentForSelectedCharacter)
    );
    return true;
  }

  // The server refuses a join: a replay of an earlier ticket must not move
  // the player, and no leave is owed. Pins: HandleTicketingJoinReply.
  bool RunRefusedJoinDoesNotMove(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenWithRealmEvents(Test, Harness)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    FAccess::bAssignmentExpected(C) = false;
    bool bAnswered = false;
    C.JoinQueue(
      TEXT("proxy-1"),
      TEXT("zone-1"),
      false,
      false,
      FRedwoodTicketingUpdateDelegate::CreateLambda(
        [&bAnswered](const FRedwoodTicketingUpdate &) { bAnswered = true; }
      )
    );
    FString Request;
    if (!AnswerRequest(Test, *Harness.Server, TEXT("{\"error\":\"refused\"}"), Request)) {
      return false;
    }
    Test.TestTrue(
      TEXT("The refusal reaches the game"),
      PumpGameThreadUntil([&bAnswered]() { return bAnswered; })
    );
    Test.TestFalse(TEXT("A refused join owes no leave"), FAccess::bLeaveTicketingOwed(C));
    Test.TestFalse(
      TEXT("After a refused join, a replay does not move the player"),
      AssignmentMoves(Test, Harness, AssignmentForSelectedCharacter)
    );
    return true;
  }

  // The server refuses a proxy join or a create that joins: a replay of an
  // earlier ticket must not move the player. Pins: NoteJoinAnswered in both
  // reply handlers.
  bool RunRefusedProxyJoinDoesNotMove(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenWithRealmEvents(Test, Harness)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    const TCHAR *const Refusal = TEXT("{\"error\":\"refused\"}");
    FString Request;

    FAccess::bAssignmentExpected(C) = false;
    bool bJoinAnswered = false;
    C.JoinProxyWithSingleInstance(
      TEXT("proxy-1"),
      FString(),
      FRedwoodJoinServerOutputDelegate::CreateLambda(
        [&bJoinAnswered](const FRedwoodJoinServerOutput &) { bJoinAnswered = true; }
      )
    );
    if (!AnswerRequest(Test, *Harness.Server, Refusal, Request)) {
      return false;
    }
    Test.TestTrue(
      TEXT("The proxy join's refusal reaches the game"),
      PumpGameThreadUntil([&bJoinAnswered]() { return bJoinAnswered; })
    );
    Test.TestFalse(
      TEXT("After a refused proxy join, a replay does not move the player"),
      AssignmentMoves(Test, Harness, AssignmentForSelectedCharacter)
    );

    FAccess::bAssignmentExpected(C) = false;
    bool bCreateAnswered = false;
    C.CreateProxy(
      true,
      FRedwoodCreateProxyInput(),
      FRedwoodCreateProxyOutputDelegate::CreateLambda(
        [&bCreateAnswered](const FRedwoodCreateProxyOutput &) { bCreateAnswered = true; }
      )
    );
    if (!AnswerRequest(Test, *Harness.Server, Refusal, Request)) {
      return false;
    }
    Test.TestTrue(
      TEXT("The create's refusal reaches the game"),
      PumpGameThreadUntil([&bCreateAnswered]() { return bCreateAnswered; })
    );
    Test.TestFalse(
      TEXT("After a refused create that joins, a replay does not move the player"),
      AssignmentMoves(Test, Harness, AssignmentForSelectedCharacter)
    );
    return true;
  }

  // A member drops a join, then picks another character: after the party
  // notice, the leader's party assignment moves it and the old leave is
  // still owed. Pins: the dropped-join reset in SetSelectedCharacter.
  bool RunCharacterSwitchClearsDroppedJoin(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenWithRealmEvents(Test, Harness)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    // In the re-login window, so the leave fails at once, drops the join and
    // is owed.
    FAccess::bRealmReauthPending(C) = true;
    C.LeaveTicketing(FRedwoodErrorOutputDelegate());
    FAccess::bRealmReauthPending(C) = false;
    Test.TestTrue(TEXT("The leave drops the join"), FAccess::bAbandonedQueueJoin(C));

    // Outside a party here, so the switch sends nothing to the Director,
    // which this harness does not open.
    C.SetSelectedCharacter(TEXT("character-2"));
    Test.TestTrue(
      TEXT("The old ticket's leave is still owed"), FAccess::bLeaveTicketingOwed(C)
    );
    // Read now, so the leave below is the next request.
    FString Request;
    Test.TestTrue(
      TEXT("The switch tells the realm the character"),
      Harness.Server->ReadClientText(Request) &&
        Request.Contains(TEXT("realm:parties:select-character"))
    );
    SendEvent(Test, Harness, PartyQueued(TEXT("character-2"), TEXT("ticket-1"), TEXT("m-1")));
    // Pins: AcceptPartyTicket keeps the owed leave, and SendOwedLeave does
    // not wait for a party join.
    Test.TestTrue(
      TEXT("The party notice keeps the old ticket's owed leave"),
      FAccess::bLeaveTicketingOwed(C)
    );
    Test.TestTrue(
      TEXT("The party notice sends the owed leave"),
      Harness.Server->ReadClientText(Request) &&
        Request.Contains(TEXT("realm:ticketing:leave"))
    );
    Test.TestTrue(
      TEXT("After a character switch, the party's assignment for the new character moves the member"),
      AssignmentMoves(
        Test,
        Harness,
        "42[\"realm:servers:connect-to-instance\",{\"shouldStitch\":false,"
        "\"connection\":\"x:1\",\"token\":\"t\",\"characterId\":\"character-2\","
        "\"ticketId\":\"ticket-1\"}]"
      )
    );
    return true;
  }

  // The server drops the socket, and the client reconnects. The re-login
  // window is closed by hand: no Director runs here.
  void DropAndComeBack(FAutomationTestBase &Test, FRealmHarness &Harness) {
    URedwoodClientInterface &C = Harness.Client();
    Test.TestTrue(TEXT("The server drops"), Harness.Close(EClose::NoCloseFrame));
    Test.TestTrue(
      TEXT("The client sees the drop"),
      PumpGameThreadUntil([&C]() { return FAccess::bRealmDisconnected(C); })
    );
    Test.TestTrue(TEXT("The client reconnects"), AcceptReconnect(*Harness.Server));
    Test.TestTrue(
      TEXT("The socket is back"),
      PumpGameThreadUntil([&C]() { return !FAccess::bRealmDisconnected(C); })
    );
    FAccess::bRealmReauthPending(C) = false;
  }

  // A proxy join or a create that joins lost in a drop: after the reconnect,
  // a replay must not move the player. Pins: MakeLostProxyJoin.
  bool RunLostProxyJoinDoesNotMove(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenWithRealmEvents(Test, Harness)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    // Drops the socket with the request out, checks the failure, and lets
    // the socket come back.
    auto DropAndReconnect = [&Test, &Harness, &C](bool &bAnswered, const TCHAR *What) {
      Test.TestTrue(TEXT("The request reaches the server"), Harness.Server->ReadClientFrame());
      DropAndComeBack(Test, Harness);
      Test.TestTrue(*FString::Printf(TEXT("The drop fails the %s"), What), bAnswered);
      Test.TestTrue(
        *FString::Printf(TEXT("The lost %s is dropped"), What),
        FAccess::bAbandonedQueueJoin(C)
      );
    };

    FAccess::bAssignmentExpected(C) = false;
    bool bJoinAnswered = false;
    C.JoinProxyWithSingleInstance(
      TEXT("proxy-1"),
      FString(),
      FRedwoodJoinServerOutputDelegate::CreateLambda(
        [&bJoinAnswered](const FRedwoodJoinServerOutput &) { bJoinAnswered = true; }
      )
    );
    DropAndReconnect(bJoinAnswered, TEXT("proxy join"));
    Test.TestFalse(
      TEXT("After a lost proxy join, a replay does not move the player"),
      AssignmentMoves(Test, Harness, AssignmentForSelectedCharacter)
    );

    bool bCreateAnswered = false;
    C.CreateProxy(
      true,
      FRedwoodCreateProxyInput(),
      FRedwoodCreateProxyOutputDelegate::CreateLambda(
        [&bCreateAnswered](const FRedwoodCreateProxyOutput &) { bCreateAnswered = true; }
      )
    );
    DropAndReconnect(bCreateAnswered, TEXT("create that joins"));
    Test.TestFalse(
      TEXT("After a lost create that joins, a replay does not move the player"),
      AssignmentMoves(Test, Harness, AssignmentForSelectedCharacter)
    );
    return true;
  }

  // The server ends the ticket: no assignment comes for it, so a replay must
  // not move the player, and no leave is owed. Pins: the reset in the
  // ticket-error handler.
  bool RunTicketErrorDoesNotMove(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenWithRealmEvents(Test, Harness)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    FAccess::bAssignmentExpected(C) = false;
    bool bTicketError = false;
    C.JoinQueue(
      TEXT("proxy-1"),
      TEXT("zone-1"),
      false,
      false,
      FRedwoodTicketingUpdateDelegate::CreateLambda(
        [&bTicketError](const FRedwoodTicketingUpdate &Update) {
          bTicketError |= Update.Type == ERedwoodTicketingUpdateType::TicketError;
        }
      )
    );
    Test.TestTrue(TEXT("The join reaches the server"), Harness.Server->ReadClientFrame());
    Test.TestTrue(
      TEXT("The server ends the ticket"),
      Harness.Server->SendText("42[\"realm:ticketing:ticket-error\",{\"error\":\"ended\"}]")
    );
    Test.TestTrue(
      TEXT("The ticket error reaches the game"),
      PumpGameThreadUntil([&bTicketError]() { return bTicketError; })
    );
    Test.TestFalse(TEXT("An ended ticket owes no leave"), FAccess::bLeaveTicketingOwed(C));
    Test.TestFalse(
      TEXT("After a ticket error, a replay does not move the player"),
      AssignmentMoves(Test, Harness, AssignmentForSelectedCharacter)
    );
    return true;
  }

  // A player in the world makes a join that is refused, then one whose
  // ticket ends: the server's zone transfers must still move the player.
  // Pins: bInWorld in the assignment guard.
  bool RunInWorldKeepsZoneTransfers(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenWithRealmEvents(Test, Harness)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    FAccess::bAssignmentExpected(C) = false;
    FAccess::bTravelPending(C) = true;
    C.NoteArrivedInWorld();

    C.JoinQueue(TEXT("proxy-1"), TEXT("zone-1"), false, false, FRedwoodTicketingUpdateDelegate());
    FString Request;
    if (!AnswerRequest(Test, *Harness.Server, TEXT("{\"error\":\"refused\"}"), Request)) {
      return false;
    }
    Test.TestTrue(
      TEXT("In the world, after a refused join, a zone transfer moves the player"),
      AssignmentMoves(Test, Harness, AssignmentForSelectedCharacter)
    );

    C.JoinQueue(TEXT("proxy-1"), TEXT("zone-1"), false, false, FRedwoodTicketingUpdateDelegate());
    Test.TestTrue(TEXT("The join reaches the server"), Harness.Server->ReadClientFrame());
    Test.TestTrue(
      TEXT("The server ends the ticket"),
      Harness.Server->SendText("42[\"realm:ticketing:ticket-error\",{\"error\":\"ended\"}]")
    );
    Test.TestTrue(
      TEXT("In the world, after a ticket error, a zone transfer moves the player"),
      AssignmentMoves(Test, Harness, AssignmentForSelectedCharacter)
    );
    return true;
  }

  // An older join is lost in a drop after a newer join was answered: the
  // newer join's assignment must still move the player. Pins: the
  // latest-join check in FailTicketingJoin.
  bool RunOlderLostJoinKeepsNewerJoin(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenWithRealmEvents(Test, Harness)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    FAccess::bAssignmentExpected(C) = false;
    C.JoinQueue(TEXT("proxy-1"), TEXT("zone-1"), false, false, FRedwoodTicketingUpdateDelegate());
    Test.TestTrue(TEXT("The older join reaches the server"), Harness.Server->ReadClientFrame());
    bool bNewerAnswered = false;
    C.JoinQueue(
      TEXT("proxy-1"),
      TEXT("zone-2"),
      false,
      false,
      FRedwoodTicketingUpdateDelegate::CreateLambda(
        [&bNewerAnswered](const FRedwoodTicketingUpdate &) { bNewerAnswered = true; }
      )
    );
    FString Request;
    if (!AnswerRequest(Test, *Harness.Server, TEXT("{\"error\":\"\"}"), Request)) {
      return false;
    }
    Test.TestTrue(
      TEXT("The newer join is answered"),
      PumpGameThreadUntil([&bNewerAnswered]() { return bNewerAnswered; })
    );
    DropAndComeBack(Test, Harness);
    Test.TestFalse(
      TEXT("The older join's loss does not drop the newer join"), FAccess::bAbandonedQueueJoin(C)
    );
    Test.TestTrue(
      TEXT("After the reconnect, the newer join's assignment moves the player"),
      AssignmentMoves(Test, Harness, AssignmentForSelectedCharacter)
    );
    return true;
  }

  // Pins: the resets in Logout.
  bool RunLogoutForgetsCharacter(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    URedwoodClientInterface &C = Harness.Client();
    FAccess::PlayerId(C) = TEXT("player-1");
    FAccess::AuthToken(C) = TEXT("token-1");
    FAccess::bAuthenticated(C) = true;
    FAccess::SelectedCharacterId(C) = TEXT("character-1");
    FAccess::bAssignmentExpected(C) = true;
    FAccess::bAbandonedQueueJoin(C) = true;
    FAccess::bLeaveTicketingOwed(C) = true;
    FAccess::CurrentParty(C).bValid = true;
    FAccess::AcceptedPartyTicketId(C) = TEXT("ticket-1");
    FAccess::EndedPartyTicketIds(C).Add(TEXT("ticket-0"));
    C.Logout();
    Test.TestTrue(
      TEXT("Logout forgets the character"), FAccess::SelectedCharacterId(C).IsEmpty()
    );
    Test.TestFalse(
      TEXT("Logout expects no assignment"), FAccess::bAssignmentExpected(C)
    );
    Test.TestFalse(
      TEXT("Logout clears the dropped join"), FAccess::bAbandonedQueueJoin(C)
    );
    Test.TestFalse(
      TEXT("The next account owes no leave"), FAccess::bLeaveTicketingOwed(C)
    );
    Test.TestFalse(TEXT("Logout forgets the party"), FAccess::CurrentParty(C).bValid);
    Test.TestTrue(
      TEXT("Logout forgets the party tickets"),
      FAccess::AcceptedPartyTicketId(C).IsEmpty() &&
        FAccess::EndedPartyTicketIds(C).IsEmpty()
    );
    return true;
  }

  // Pins: FailTicketingJoin marks every lost join, not only JoinQueue.
  bool RunLostCustomJoin(FAutomationTestBase &Test) {
    bool bAnswered = false;
    FString Error;
    FRealmHarness Harness;
    if (!Harness.Open(Test)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    C.JoinCustom(
      false,
      TArray<FString>(),
      FRedwoodTicketingUpdateDelegate::CreateLambda(
        [&](const FRedwoodTicketingUpdate &Update) {
          bAnswered = true;
          Error = Update.Message;
        }
      )
    );
    Test.TestTrue(TEXT("The request reaches the server"), Harness.Server->ReadClientFrame());
    Test.TestTrue(TEXT("The server closes"), Harness.Close(EClose::NoCloseFrame));
    PumpGameThreadUntil([&bAnswered]() { return bAnswered; });
    ExpectClearError(Test, TEXT("Custom join"), bAnswered, Error);
    Test.TestTrue(
      TEXT("Its assignment is dropped"), FAccess::bAbandonedQueueJoin(C)
    );
    Test.TestTrue(TEXT("A leave is owed"), FAccess::bLeaveTicketingOwed(C));
    return true;
  }

  // A leave owed from a lost join goes out at the next Realm auth, before
  // the caller can join again. Pins: SendOwedLeave in FinalizeRealmHandshake.
  bool RunOwedLeaveAtNextAuth(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    FFakeSocketIoServer DirectorServer;
    URedwoodClientInterface &C = Harness.Client();
    FAccess::bLeaveTicketingOwed(C) = true;
    if (!OpenThroughHandshake(Test, Harness, DirectorServer)) {
      return false;
    }
    FString Request;
    if (!AnswerRequest(Test, *Harness.Server, TEXT("{\"error\":\"\"}"), Request) ||
        !Test.TestTrue(
          TEXT("The Realm got the auth request"),
          Request.Contains(TEXT("realm:auth:player:connect:client-to-realm"))
        )) {
      return false;
    }
    if (!Test.TestTrue(
          TEXT("The auth reply sends a leave"),
          PumpGameThreadUntil([&C]() { return FAccess::NumRealmReplies(C) == 1; })
        ) ||
        !AnswerRequest(
          Test, *Harness.Server, TEXT("{\"error\":\"Leave failed.\"}"), Request
        )) {
      return false;
    }
    Test.TestTrue(
      TEXT("The request is the leave"), Request.Contains(TEXT("realm:ticketing:leave"))
    );
    Test.TestTrue(
      TEXT("The leave's reply runs"),
      PumpGameThreadUntil([&C]() { return FAccess::NumRealmReplies(C) == 0; })
    );
    Test.TestTrue(TEXT("A failed leave is still owed"), FAccess::bLeaveTicketingOwed(C));

    // Pins: the reset in NoteJoinSent.
    C.JoinQueue(
      TEXT("proxy-1"), TEXT("zone-1"), false, false, FRedwoodTicketingUpdateDelegate()
    );
    Test.TestFalse(
      TEXT("A new join replaces the owed leave"), FAccess::bLeaveTicketingOwed(C)
    );
    return true;
  }

  // Pins: the bAssignmentExpected check in SendOwedLeave. With a join out,
  // the leave would cancel its ticket.
  bool RunOwedLeaveWaitsWhileJoinOut(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!Harness.Open(Test)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    FAccess::bLeaveTicketingOwed(C) = true;
    FAccess::bAssignmentExpected(C) = true;
    FAccess::SendOwedLeave(C);
    Test.TestEqual(
      TEXT("No leave goes out while a join is out"), FAccess::NumRealmReplies(C), 0
    );
    FAccess::bAssignmentExpected(C) = false;
    FAccess::SendOwedLeave(C);
    Test.TestEqual(TEXT("With no join out, it goes"), FAccess::NumRealmReplies(C), 1);
    return true;
  }

  // The game leaves when the enter-world timer runs out, often because the
  // Realm dropped. Pins: the flags LeaveTicketing sets.
  bool RunLeaveTicketingDropsTheJoin(FAutomationTestBase &Test) {
    bool bAnswered = false;
    FString Error;
    FRealmHarness Harness;
    if (!OpenWithRealmEvents(Test, Harness)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    FRedwoodErrorOutputDelegate OnOutput =
      FRedwoodErrorOutputDelegate::CreateLambda([&](const FString &InError) {
        bAnswered = true;
        Error = InError;
      });

    // In the re-login window: the leave cannot go out.
    FAccess::bRealmReauthPending(C) = true;
    C.LeaveTicketing(OnOutput);
    Test.TestTrue(TEXT("The leave fails at once"), bAnswered && !Error.IsEmpty());
    Test.TestTrue(TEXT("The join is dropped"), FAccess::bAbandonedQueueJoin(C));
    Test.TestFalse(TEXT("No assignment is expected"), FAccess::bAssignmentExpected(C));
    Test.TestTrue(TEXT("The leave is owed"), FAccess::bLeaveTicketingOwed(C));
    FAccess::bRealmReauthPending(C) = false;
    Test.TestFalse(
      TEXT("An assignment after the leave does not move the player"),
      AssignmentMoves(Test, Harness, AssignmentForSelectedCharacter)
    );

    // The timer race: an assignment is accepted, and the game's timer then
    // leaves during its travel.
    FAccess::bAbandonedQueueJoin(C) = false;
    FAccess::bAssignmentExpected(C) = true;
    Test.TestTrue(
      TEXT("The assignment moves the player"),
      AssignmentMoves(Test, Harness, AssignmentForSelectedCharacter)
    );
    Test.TestTrue(TEXT("Its travel is pending"), FAccess::bTravelPending(C));

    // Sent and answered: it pays a leave that was owed. Pins: the reset in
    // the LeaveTicketing reply.
    C.LeaveTicketing(OnOutput);
    FString Request;
    if (!AnswerRequest(Test, *Harness.Server, TEXT("{\"error\":\"\"}"), Request)) {
      return false;
    }
    Test.TestTrue(
      TEXT("A successful leave pays the owed one"),
      PumpGameThreadUntil([&C]() { return !FAccess::bLeaveTicketingOwed(C); })
    );
    Test.TestFalse(
      TEXT("Between the leave and the arrival, a replay does not move the player"),
      AssignmentMoves(Test, Harness, AssignmentForSelectedCharacter)
    );

    // The travel of the assignment finished after the leave: the player is
    // in the world. Pins: NoteArrivedInWorld.
    C.NoteArrivedInWorld();
    Test.TestTrue(
      TEXT("After the arrival, a zone transfer moves the player"),
      AssignmentMoves(Test, Harness, AssignmentForSelectedCharacter)
    );

    // Sent, and then lost in a drop: owed again.
    FAccess::bLeaveTicketingOwed(C) = false;
    FAccess::bAbandonedQueueJoin(C) = false;
    FAccess::bAssignmentExpected(C) = true;
    bAnswered = false;
    C.LeaveTicketing(OnOutput);
    Test.TestFalse(TEXT("A sent leave waits for its reply"), bAnswered);
    Test.TestTrue(TEXT("A sent leave drops the join too"), FAccess::bAbandonedQueueJoin(C));
    Test.TestFalse(TEXT("A sent leave is not owed"), FAccess::bLeaveTicketingOwed(C));
    Test.TestTrue(TEXT("The leave reaches the server"), Harness.Server->ReadClientFrame());
    Test.TestTrue(TEXT("The server drops"), Harness.Close(EClose::NoCloseFrame));
    Test.TestTrue(
      TEXT("The drop fails the leave"), PumpGameThreadUntil([&bAnswered]() { return bAnswered; })
    );
    Test.TestTrue(TEXT("A lost leave is owed"), FAccess::bLeaveTicketingOwed(C));
    return true;
  }

  // Pins: Logout sends an owed leave before the Realm logout, so the Realm
  // still knows the player when it runs the leave.
  bool RunLogoutPaysOwedLeave(FAutomationTestBase &Test) {
    int32 CloseReports = 0;
    bool bNamespaceClosed = false;
    FRealmHarness Harness;
    if (!Harness.Open(Test)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    FAccess::bLeaveTicketingOwed(C) = true;
    // The library reports a close we ask for twice; the test waits for the
    // second, as in PartyInviteFailsOnRequestedClose.
    TSharedPtr<FSocketIONative> &Realm = FAccess::Realm(C);
    TFunction<void(const ESIOConnectionCloseReason)> Handler =
      Realm->OnDisconnectedCallback;
    Realm->OnDisconnectedCallback =
      [Handler, &CloseReports](const ESIOConnectionCloseReason Reason) {
        Handler(Reason);
        ++CloseReports;
      };

    C.Logout();
    // The library's 3 s close timer reports the namespace closed when it
    // runs; the interface's own handler for it stays first.
    TFunction<void(const FString &)> OnNamespaceClosed =
      Realm->OnNamespaceDisconnectedCallback;
    Realm->OnNamespaceDisconnectedCallback =
      [OnNamespaceClosed, &bNamespaceClosed](const FString &Namespace) {
        if (OnNamespaceClosed) {
          OnNamespaceClosed(Namespace);
        }
        bNamespaceClosed = true;
      };
    FString Request;
    Test.TestTrue(TEXT("A request arrives"), Harness.Server->ReadClientText(Request));
    Test.TestTrue(
      TEXT("The leave goes before the Realm logout"),
      Request.Contains(TEXT("realm:ticketing:leave"))
    );
    Test.TestFalse(TEXT("The flag is dropped"), FAccess::bLeaveTicketingOwed(C));
    // Not checked: the read above can already hold the client's close frame,
    // so the server may find no echo to read. The close reports below are
    // what the test waits for.
    Harness.Server->CloseNormally();
    Test.TestTrue(
      TEXT("The close handshake ends"),
      PumpGameThreadUntil([&CloseReports]() { return CloseReports >= 2; })
    );
    Test.TestTrue(
      TEXT("The close timer ran"),
      PumpGameThreadUntil([&bNamespaceClosed]() { return bNamespaceClosed; })
    );
    return true;
  }

  // The timer fires, the player joins again from character select, and that
  // join's reply is lost; then the OLD travel arrives. Pins: the join count
  // in NoteArrivedInWorld.
  bool RunOldTravelAfterNewJoin(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenWithRealmEvents(Test, Harness)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    Test.TestTrue(
      TEXT("The first assignment moves the player"),
      AssignmentMoves(Test, Harness, AssignmentForSelectedCharacter)
    );
    C.LeaveTicketing(FRedwoodErrorOutputDelegate());
    C.JoinQueue(
      TEXT("proxy-1"), TEXT("zone-1"), false, false, FRedwoodTicketingUpdateDelegate()
    );
    // What the lost reply of the new join leaves.
    FAccess::bAbandonedQueueJoin(C) = true;
    FAccess::bLeaveTicketingOwed(C) = true;
    C.NoteArrivedInWorld();
    Test.TestTrue(
      TEXT("The old travel keeps the new join dropped"), FAccess::bAbandonedQueueJoin(C)
    );
    Test.TestTrue(
      TEXT("The old travel keeps the new join's leave owed"),
      FAccess::bLeaveTicketingOwed(C)
    );
    return true;
  }

  // The newer join was left, and its leave went through; then the old
  // travel arrives. The player is in the world. Pins: the no-leave-owed case
  // of the join count check in NoteArrivedInWorld.
  bool RunOldTravelAfterCleanLeave(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenWithRealmEvents(Test, Harness)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    Test.TestTrue(
      TEXT("The first assignment moves the player"),
      AssignmentMoves(Test, Harness, AssignmentForSelectedCharacter)
    );
    C.JoinQueue(
      TEXT("proxy-1"), TEXT("zone-1"), false, false, FRedwoodTicketingUpdateDelegate()
    );
    // What a leave of the new join leaves once it went through.
    FAccess::bAbandonedQueueJoin(C) = true;
    FAccess::bAssignmentExpected(C) = false;
    FAccess::bLeaveTicketingOwed(C) = false;
    C.NoteArrivedInWorld();
    Test.TestTrue(
      TEXT("After the old travel arrives, a zone transfer moves the player"),
      AssignmentMoves(Test, Harness, AssignmentForSelectedCharacter)
    );
    return true;
  }

  // Back at the entry level: character select. Pins: NoteLeftWorld.
  bool RunLeftWorld(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenWithRealmEvents(Test, Harness)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    FAccess::bTravelPending(C) = true;
    C.NoteLeftWorld();
    Test.TestFalse(TEXT("Leaving ends the travel"), FAccess::bTravelPending(C));
    Test.TestFalse(
      TEXT("At the entry level, a replay does not move the player"),
      AssignmentMoves(Test, Harness, AssignmentForSelectedCharacter)
    );
    return true;
  }

  // A client world that is not the travel of an accepted assignment (a PIE
  // client, a future lobby) must not open the guard. Pins: the
  // bTravelPending check in NoteArrivedInWorld.
  bool RunArrivalWithoutTravel(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!OpenWithRealmEvents(Test, Harness)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    FAccess::bAssignmentExpected(C) = false;
    C.NoteArrivedInWorld();
    Test.TestFalse(
      TEXT("An arrival with no travel expects no assignment"),
      FAccess::bAssignmentExpected(C)
    );
    Test.TestFalse(
      TEXT("A replay does not move the player"),
      AssignmentMoves(Test, Harness, AssignmentForSelectedCharacter)
    );
    return true;
  }

  // HollowedOath#2999. Freeing the interface right after a close we ask for
  // must keep the socket until the library's close timer ran; freed before,
  // the timer runs on freed memory. Pins: RedwoodClosingSockets in
  // ReleaseRealmSocket.
  // The socket is still alive well inside the library's close timer, and it
  // is freed once the timer ran.
  bool ExpectKeptThenFreed(FAutomationTestBase &Test, TWeakPtr<FSocketIONative> &Socket) {
    // Well inside the timer.
    constexpr double TimerRunningSeconds = 1.0;
    const double Until = FPlatformTime::Seconds() + TimerRunningSeconds;
    PumpGameThreadUntil([Until]() { return FPlatformTime::Seconds() > Until; });
    Test.TestTrue(TEXT("The socket is kept while its close timer runs"), Socket.IsValid());
    Test.TestTrue(
      TEXT("The socket is freed once its close timer ran"),
      PumpGameThreadUntil([&Socket]() { return !Socket.IsValid(); })
    );
    Test.TestEqual(TEXT("Nothing is held"), RedwoodClosingSockets::NumHeld(), 0);
    return true;
  }

  // A close asked while the socket was down, which then connected: no close
  // timer runs yet, so the release must close the socket again and hold it.
  // Pins: the timer condition in ReleaseRealmSocket.
  bool RunCloseWhileDownThenConnected(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!Harness.Open(Test)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    TWeakPtr<FSocketIONative> Socket = FAccess::Realm(C);
    FAccess::bRealmCloseRequested(C) = true;
    FAccess::bRealmCloseTimerPending(C) = false;
    C.Deinitialize();
    return ExpectKeptThenFreed(Test, Socket);
  }

  bool RunFreeDuringRequestedClose(FAutomationTestBase &Test, bool bRequestTwice) {
    TWeakPtr<FSocketIONative> Socket;
    FRealmHarness Harness;
    if (!Harness.Open(Test)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    Socket = FAccess::Realm(C);
    FAccess::RequestRealmClose(C);
    // A Logout during the re-login, then the re-login's own Logout. Pins:
    // the early return in RequestRealmClose.
    if (bRequestTwice) {
      FAccess::RequestRealmClose(C);
    }
    C.Deinitialize();
    return ExpectKeptThenFreed(Test, Socket);
  }

  // The socket sends condensed JSON, so a field and its value are one run of
  // text. The backend reads field names case-sensitively.
  bool HasJsonText(const FString &Request, const TCHAR *Text) {
    return Request.Contains(Text, ESearchCase::CaseSensitive);
  }

  // A cancel must name the pending request only, so the backend cannot end a
  // friendship the other side accepted meanwhile. A remove must not.
  bool RunCharacterFriendCancelIsRequestOnly(FAutomationTestBase &Test) {
    // Before the harness, so they outlive every callback it can run.
    int32 Answers = 0;
    FString Error = TEXT("unanswered");
    FRealmHarness Harness;
    if (!Harness.Open(Test)) {
      return false;
    }
    const FRedwoodErrorOutputDelegate OnOutput =
      FRedwoodErrorOutputDelegate::CreateLambda([&](const FString &InError) {
        ++Answers;
        Error = InError;
      });
    const TCHAR *const RemoveRoute = TEXT("realm:contacts:friends:remove");
    const TCHAR *const RequestOnlyField = TEXT("\"requestOnly\":true");
    const TCHAR *const Success = TEXT("{\"error\":\"\"}");

    FString Request;
    Harness.Client().RemoveCharacterFriend(TEXT("character-2"), OnOutput);
    if (!AnswerRequest(Test, *Harness.Server, Success, Request)) {
      return false;
    }
    Test.TestTrue(TEXT("A remove goes to the remove route"), HasJsonText(Request, RemoveRoute));
    Test.TestTrue(
      TEXT("A remove names the other character"),
      HasJsonText(Request, TEXT("\"otherCharacterId\":\"character-2\""))
    );
    Test.TestFalse(TEXT("A remove is not request-only"), HasJsonText(Request, RequestOnlyField));

    Harness.Client().CancelCharacterFriendRequest(TEXT("character-2"), OnOutput);
    if (!AnswerRequest(Test, *Harness.Server, Success, Request)) {
      return false;
    }
    Test.TestTrue(TEXT("A cancel goes to the remove route"), HasJsonText(Request, RemoveRoute));
    Test.TestTrue(TEXT("A cancel is request-only"), HasJsonText(Request, RequestOnlyField));

    Test.TestTrue(
      TEXT("Both calls are answered"),
      PumpGameThreadUntil([&Answers]() { return Answers == 2; })
    );
    Test.TestEqual(TEXT("An empty realm error is a success"), Error, FString());
    return true;
  }

  // Pins the fields that the RedwoodBackend fork validates
  // (Realms.Contacts.Friends in packages/common/src/interfaces.ts): playerId,
  // characterId and targetCharacterId on a request, otherCharacterId and
  // accept on a respond. Also pins that a reply with no error field, or no
  // object, is an error, that a held call is sent for the character that made
  // it, and that a held call is not sent when the character changed.
  bool RunCharacterFriendWireFields(FAutomationTestBase &Test) {
    // Before the harness, so it outlives every callback the harness can run.
    TArray<FString> Errors;
    FRealmHarness Harness;
    if (!Harness.Open(Test)) {
      return false;
    }
    const FRedwoodErrorOutputDelegate OnOutput =
      FRedwoodErrorOutputDelegate::CreateLambda([&Errors](const FString &Error) {
        Errors.Add(Error);
      });

    FString Request;
    Harness.Client().RequestCharacterFriend(TEXT("character-2"), OnOutput);
    if (!AnswerRequest(Test, *Harness.Server, TEXT("{}"), Request)) {
      return false;
    }
    Test.TestTrue(
      TEXT("A request goes to the request route"),
      HasJsonText(Request, TEXT("\"realm:contacts:friends:request\""))
    );
    Test.TestTrue(
      TEXT("A request names the player"),
      HasJsonText(Request, TEXT("\"playerId\":\"player-1\""))
    );
    Test.TestTrue(
      TEXT("A request names the caller"),
      HasJsonText(Request, TEXT("\"characterId\":\"character-1\""))
    );
    Test.TestTrue(
      TEXT("A request names the target"),
      HasJsonText(Request, TEXT("\"targetCharacterId\":\"character-2\""))
    );

    Harness.Client().RespondToCharacterFriendRequest(TEXT("character-2"), true, OnOutput);
    if (!AnswerRequest(Test, *Harness.Server, TEXT("\"nope\""), Request)) {
      return false;
    }
    Test.TestTrue(
      TEXT("A respond goes to the respond route"),
      HasJsonText(Request, TEXT("\"realm:contacts:friends:respond\""))
    );
    Test.TestTrue(
      TEXT("A respond names the other character"),
      HasJsonText(Request, TEXT("\"otherCharacterId\":\"character-2\""))
    );
    Test.TestTrue(
      TEXT("A respond carries the accept"), HasJsonText(Request, TEXT("\"accept\":true"))
    );

    Test.TestTrue(
      TEXT("Both calls are answered"),
      PumpGameThreadUntil([&Errors]() { return Errors.Num() == 2; })
    );
    for (const FString &Error : Errors) {
      Test.TestEqualSensitive(
        TEXT("A reply that is not an answer is an error"),
        *Error,
        URedwoodCommonGameSubsystem::BadRealmAnswerError
      );
    }

    // The re-login is pending, so the call is held; the selection does not
    // change before the re-login ends.
    URedwoodClientInterface &C = Harness.Client();
    FAccess::NoteRealmDrop(C);
    C.RequestCharacterFriend(TEXT("character-2"), OnOutput);
    if (!Test.TestEqual(TEXT("The call is held"), FAccess::NumRealmHeldRequests(C), 1)) {
      return false;
    }
    FAccess::EndRealmReauthentication(C, true);
    if (!AnswerRequest(Test, *Harness.Server, TEXT("{\"error\":\"\"}"), Request)) {
      return false;
    }
    Test.TestTrue(
      TEXT("A held call names the character that made it"),
      HasJsonText(Request, TEXT("\"characterId\":\"character-1\""))
    );
    Test.TestTrue(
      TEXT("The held call is answered"),
      PumpGameThreadUntil([&Errors]() { return Errors.Num() == 3; })
    );

    // Held again, and another character is selected before the re-login
    // ends: the call must not act for the character the player left.
    FAccess::NoteRealmDrop(C);
    C.RequestCharacterFriend(TEXT("character-2"), OnOutput);
    if (!Test.TestEqual(
          TEXT("The second call is held"), FAccess::NumRealmHeldRequests(C), 1
        )) {
      return false;
    }
    FAccess::SelectedCharacterId(C) = TEXT("character-3");
    FAccess::EndRealmReauthentication(C, true);
    Test.TestEqual(
      TEXT("A call held across a character change is not sent"),
      FAccess::NumRealmReplies(C),
      0
    );
    if (!Test.TestEqual(TEXT("The changed call is answered"), Errors.Num(), 4)) {
      return false;
    }
    Test.TestEqualSensitive(
      TEXT("The changed call tells the character changed"),
      *Errors[3],
      URedwoodCommonGameSubsystem::CharacterChangedError
    );
    return true;
  }

  // Pins the event name and the listener that InitializeDirectorConnection
  // binds: a pushed alert reaches OnCharacterFriendAlert once, parsed.
  bool RunCharacterFriendAlertPush(FAutomationTestBase &Test) {
    // Before the harness, so it outlives every callback the harness can run.
    TStrongObjectPtr<URedwoodCharacterFriendAlertListener> Listener(
      NewObject<URedwoodCharacterFriendAlertListener>()
    );
    FRealmHarness Harness;
    // After the harness, so its connection closes before the harness waits
    // for the sockets, as in DirectorDropDuringRealmRelogin.
    FFakeSocketIoServer DirectorServer;
    Harness.OpenedBy = &Test;
    URedwoodClientInterface &C = Harness.Client();
    Listener->Watch(&C);
    if (!OpenProductionDirector(Test, C, DirectorServer)) {
      return false;
    }

    Test.TestTrue(
      TEXT("The Director pushes an alert"),
      DirectorServer.SendText(
        "42[\"director:friends:character-alert\",{\"type\":\"online\","
        "\"characterId\":\"me-1\",\"otherCharacterId\":\"other-1\","
        "\"otherCharacterName\":\"Bob\",\"zoneName\":\"zone-1\"}]"
      )
    );
    Test.TestTrue(
      TEXT("The alert is broadcast"),
      PumpGameThreadUntil([&Listener]() { return Listener->Count > 0; })
    );
    Test.TestEqual(TEXT("The alert is broadcast once"), Listener->Count, 1);
    const FRedwoodCharacterFriendAlert &Alert = Listener->Last;
    Test.TestTrue(
      TEXT("The type"), Alert.Type == ERedwoodCharacterFriendAlertType::Online
    );
    // TestEqualSensitive: the string forms of TestEqual ignore case.
    Test.TestEqualSensitive(TEXT("The character"), *Alert.CharacterId, TEXT("me-1"));
    Test.TestEqualSensitive(
      TEXT("The other character"), *Alert.OtherCharacterId, TEXT("other-1")
    );
    Test.TestEqualSensitive(TEXT("The name"), *Alert.OtherCharacterName, TEXT("Bob"));
    Test.TestEqualSensitive(TEXT("The zone"), *Alert.ZoneName, TEXT("zone-1"));
    return true;
  }

  // A list answer names no character. Pins: the answer of a character friend
  // call is refused when the selection changed while the call was out.
  bool RunCharacterFriendListAfterSwitch(FAutomationTestBase &Test) {
    // Before the harness, so they outlive every callback it can run.
    TArray<FRedwoodListCharacterFriendsOutput> Outputs;
    FRealmHarness Harness;
    if (!Harness.Open(Test)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    const FRedwoodListCharacterFriendsOutputDelegate OnList =
      FRedwoodListCharacterFriendsOutputDelegate::CreateLambda(
        [&Outputs](const FRedwoodListCharacterFriendsOutput &Output) {
          Outputs.Add(Output);
        }
      );
    const TCHAR *const ListAnswer =
      TEXT("{\"error\":\"\",\"friends\":[{\"characterId\":\"friend-1\",")
      TEXT("\"characterName\":\"Bob\"}],\"incomingRequests\":[],")
      TEXT("\"outgoingRequests\":[]}");

    FString Request;
    C.ListCharacterFriends(OnList);
    if (!AnswerRequest(Test, *Harness.Server, ListAnswer, Request) ||
        !Test.TestTrue(
          TEXT("The answer for the same character arrives"),
          PumpGameThreadUntil([&Outputs]() { return Outputs.Num() == 1; })
        )) {
      return false;
    }
    Test.TestEqual(
      TEXT("The answer for the same character is given"), Outputs[0].Friends.Num(), 1
    );

    // The call is out for character-1 when the player selects another.
    C.ListCharacterFriends(OnList);
    FAccess::SelectedCharacterId(C) = TEXT("character-2");
    if (!AnswerRequest(Test, *Harness.Server, ListAnswer, Request) ||
        !Test.TestTrue(
          TEXT("The answer after the switch arrives"),
          PumpGameThreadUntil([&Outputs]() { return Outputs.Num() == 2; })
        )) {
      return false;
    }
    Test.TestTrue(
      TEXT("The call was sent for the old character"),
      HasJsonText(Request, TEXT("\"characterId\":\"character-1\""))
    );
    Test.TestEqualSensitive(
      TEXT("The answer after the switch tells the character changed"),
      *Outputs[1].Error,
      URedwoodCommonGameSubsystem::CharacterChangedError
    );
    Test.TestEqual(
      TEXT("The old character's friends are not given"), Outputs[1].Friends.Num(), 0
    );
    return true;
  }

  // The realm makes the party from the invite's character when it is
  // accepted. Pins: a held invite goes out for the character that made it,
  // and is refused when another character is selected before it goes.
  bool RunHeldPartyInviteKeepsCharacter(FAutomationTestBase &Test) {
    // Before the harness, so it outlives every callback it can run.
    TArray<FString> Errors;
    FRealmHarness Harness;
    if (!Harness.Open(Test)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    const FRedwoodErrorOutputDelegate OnOutput =
      FRedwoodErrorOutputDelegate::CreateLambda([&Errors](const FString &Error) {
        Errors.Add(Error);
      });

    FAccess::NoteRealmDrop(C);
    C.InviteToParty(TEXT("player-2"), OnOutput);
    if (!Test.TestEqual(TEXT("The invite is held"), FAccess::NumRealmHeldRequests(C), 1)) {
      return false;
    }
    FAccess::EndRealmReauthentication(C, true);
    FString Request;
    if (!AnswerRequest(Test, *Harness.Server, TEXT("{\"error\":\"\"}"), Request)) {
      return false;
    }
    Test.TestTrue(
      TEXT("A held invite names the character that made it"),
      HasJsonText(Request, TEXT("\"characterId\":\"character-1\""))
    );
    Test.TestTrue(
      TEXT("The held invite is answered"),
      PumpGameThreadUntil([&Errors]() { return Errors.Num() == 1; })
    );

    FAccess::NoteRealmDrop(C);
    C.InviteToParty(TEXT("player-2"), OnOutput);
    if (!Test.TestEqual(
          TEXT("The second invite is held"), FAccess::NumRealmHeldRequests(C), 1
        )) {
      return false;
    }
    FAccess::SelectedCharacterId(C) = TEXT("character-2");
    FAccess::EndRealmReauthentication(C, true);
    Test.TestEqual(
      TEXT("An invite held across a character change is not sent"),
      FAccess::NumRealmReplies(C),
      0
    );
    if (!Test.TestEqual(TEXT("The changed invite is answered"), Errors.Num(), 2)) {
      return false;
    }
    Test.TestEqualSensitive(
      TEXT("The changed invite tells the character changed"),
      *Errors[1],
      URedwoodCommonGameSubsystem::CharacterChangedError
    );
    return true;
  }

  // The realm moves a player's pending invites to the character they select.
  // Pins: a selection made while the Realm transport is down reaches the
  // Realm when its re-handshake ends.
  bool RunSelectCharacterAfterRealmRelogin(FAutomationTestBase &Test) {
    FRealmHarness Harness;
    if (!Harness.Open(Test)) {
      return false;
    }
    URedwoodClientInterface &C = Harness.Client();
    FAccess::NoteRealmDrop(C);
    // Down for the selection only: the socket stays open, so the test can
    // read what the end of the re-handshake sends.
    FAccess::Realm(C)->bIsConnected = false;
    C.SetSelectedCharacter(TEXT("character-2"));
    FAccess::Realm(C)->bIsConnected = true;
    FAccess::EndRealmReauthentication(C, true);

    FString Request;
    if (!Test.TestTrue(
          TEXT("The end of the re-handshake sends the selection"),
          Harness.Server->ReadClientText(Request)
        )) {
      return false;
    }
    Test.TestTrue(
      TEXT("It is the character selection"),
      HasJsonText(Request, TEXT("\"realm:parties:select-character\""))
    );
    Test.TestTrue(
      TEXT("It names the selected character"),
      HasJsonText(Request, TEXT("\"characterId\":\"character-2\""))
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
  FRedwoodInFlightRealmReconnectTest,
  "RealmReconnectAndReloginWindow",
  RedwoodInFlightTest::RunRealmReconnectAndReloginWindow(*this)
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
  FRedwoodInFlightAssignmentWithoutJoinTest,
  "AssignmentWithoutJoinDoesNotMove",
  RedwoodInFlightTest::RunAssignmentWithoutJoin(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightLogoutForgetsCharacterTest,
  "LogoutForgetsCharacter",
  RedwoodInFlightTest::RunLogoutForgetsCharacter(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightLostCustomJoinTest,
  "LostCustomJoinIsDropped",
  RedwoodInFlightTest::RunLostCustomJoin(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightOwedLeaveTest,
  "OwedLeaveGoesOutAtNextAuth",
  RedwoodInFlightTest::RunOwedLeaveAtNextAuth(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightOwedLeaveWaitsTest,
  "OwedLeaveWaitsWhileJoinOut",
  RedwoodInFlightTest::RunOwedLeaveWaitsWhileJoinOut(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightLeaveTicketingTest,
  "LeaveTicketingDropsTheJoin",
  RedwoodInFlightTest::RunLeaveTicketingDropsTheJoin(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightLogoutPaysOwedLeaveTest,
  "LogoutPaysOwedLeave",
  RedwoodInFlightTest::RunLogoutPaysOwedLeave(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightArrivalWithoutTravelTest,
  "ArrivalWithoutTravelKeepsGuard",
  RedwoodInFlightTest::RunArrivalWithoutTravel(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightOldTravelTest,
  "OldTravelKeepsNewJoinFlags",
  RedwoodInFlightTest::RunOldTravelAfterNewJoin(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightOldTravelCleanLeaveTest,
  "OldTravelAfterCleanLeaveMoves",
  RedwoodInFlightTest::RunOldTravelAfterCleanLeave(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightLeftWorldTest,
  "LeftWorldExpectsNoAssignment",
  RedwoodInFlightTest::RunLeftWorld(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightFreeDuringCloseTest,
  "FreeDuringRequestedCloseKeepsSocket",
  RedwoodInFlightTest::RunFreeDuringRequestedClose(*this, false)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightFreeDuringSecondCloseTest,
  "FreeAfterSecondRequestedCloseKeepsSocket",
  RedwoodInFlightTest::RunFreeDuringRequestedClose(*this, true)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightCreateProxyJoinTest,
  "CreateProxyJoinMoves",
  RedwoodInFlightTest::RunCreateProxyJoinMoves(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightRelaunchedMemberTest,
  "RelaunchedMemberRefusesReplay",
  RedwoodInFlightTest::RunRelaunchedMemberRefusesReplay(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightPartyQueuedMovesTest,
  "PartyQueuedMovesMember",
  RedwoodInFlightTest::RunPartyQueuedMovesMember(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightDuplicatePartyQueuedTest,
  "DuplicatePartyQueuedNoChange",
  RedwoodInFlightTest::RunDuplicatePartyQueuedNoChange(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightPartyQueuedOtherCharacterTest,
  "PartyQueuedForAnotherCharacterIgnored",
  RedwoodInFlightTest::RunPartyQueuedForAnotherCharacter(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightPartyLeftTest,
  "PartyLeftDropsJoin",
  RedwoodInFlightTest::RunPartyLeftDropsJoin(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightPartyLeftOlderTest,
  "PartyLeftOlderTicketKeepsNewer",
  RedwoodInFlightTest::RunPartyLeftOlderTicketKeepsNewer(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightPartyQueuedEndedTest,
  "PartyQueuedForEndedTicketIgnored",
  RedwoodInFlightTest::RunPartyQueuedForEndedTicket(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightOtherTicketTest,
  "OtherTicketAssignmentRefused",
  RedwoodInFlightTest::RunOtherTicketAssignmentRefused(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightPartyQueuedBeforeSelectTest,
  "PartyQueuedBeforeCharacterSelect",
  RedwoodInFlightTest::RunPartyQueuedBeforeCharacterSelect(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightPartyNoticeLeavesOwnJoinTest,
  "PartyNoticeLeavesOwnJoin",
  RedwoodInFlightTest::RunPartyNoticeLeavesOwnJoin(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightOwnJoinAfterPartyNoticeTest,
  "OwnJoinAfterPartyNoticeMoves",
  RedwoodInFlightTest::RunOwnJoinAfterPartyNoticeMoves(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightCreateProxyAfterDroppedJoinTest,
  "CreateProxyAfterDroppedJoinMoves",
  RedwoodInFlightTest::RunCreateProxyAfterDroppedJoin(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightRefusedJoinTest,
  "RefusedJoinDoesNotMove",
  RedwoodInFlightTest::RunRefusedJoinDoesNotMove(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightRefusedProxyJoinTest,
  "RefusedProxyJoinDoesNotMove",
  RedwoodInFlightTest::RunRefusedProxyJoinDoesNotMove(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightCharacterSwitchTest,
  "CharacterSwitchClearsDroppedJoin",
  RedwoodInFlightTest::RunCharacterSwitchClearsDroppedJoin(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightLostProxyJoinTest,
  "LostProxyJoinDoesNotMove",
  RedwoodInFlightTest::RunLostProxyJoinDoesNotMove(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightTicketErrorTest,
  "TicketErrorDoesNotMove",
  RedwoodInFlightTest::RunTicketErrorDoesNotMove(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightInWorldTest,
  "InWorldKeepsZoneTransfers",
  RedwoodInFlightTest::RunInWorldKeepsZoneTransfers(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightOlderLostJoinTest,
  "OlderLostJoinKeepsNewerJoin",
  RedwoodInFlightTest::RunOlderLostJoinKeepsNewerJoin(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightCloseWhileDownTest,
  "CloseWhileDownThenConnectedKeepsSocket",
  RedwoodInFlightTest::RunCloseWhileDownThenConnected(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightDirectorReconnectTest,
  "DirectorReconnectFailsPendingReply",
  RedwoodInFlightTest::RunDirectorReconnectFailsPendingReply(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightCharacterFriendCancelTest,
  "CharacterFriendCancelIsRequestOnly",
  RedwoodInFlightTest::RunCharacterFriendCancelIsRequestOnly(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightCharacterFriendWireFieldsTest,
  "CharacterFriendWireFields",
  RedwoodInFlightTest::RunCharacterFriendWireFields(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightCharacterFriendAlertTest,
  "CharacterFriendAlertPush",
  RedwoodInFlightTest::RunCharacterFriendAlertPush(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightCharacterFriendListAfterSwitchTest,
  "CharacterFriendListAfterSwitch",
  RedwoodInFlightTest::RunCharacterFriendListAfterSwitch(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightHeldPartyInviteKeepsCharacterTest,
  "HeldPartyInviteKeepsCharacter",
  RedwoodInFlightTest::RunHeldPartyInviteKeepsCharacter(*this)
)
REDWOOD_IN_FLIGHT_TEST(
  FRedwoodInFlightSelectCharacterAfterRealmReloginTest,
  "SelectCharacterAfterRealmRelogin",
  RedwoodInFlightTest::RunSelectCharacterAfterRealmRelogin(*this)
)
