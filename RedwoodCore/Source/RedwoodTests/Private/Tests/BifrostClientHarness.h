// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): file is fork-added, for the client interface tests.
// The client, its listeners and the fake Bifrost of one test, with the steps
// a test takes to a session: connect, log in, enter the realm, select the
// character, drop and resume. BifrostClientInterfaceTest.cpp pins the
// session on it; BifrostZoneTravelTest.cpp pins the ticketing cluster on it.
// Header only, so each suite is its own translation unit under the unity
// build and shares one harness.

#pragma once

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "TimerManager.h"
#include "UObject/StrongObjectPtr.h"

#include "Kismet/GameplayStatics.h"

#include "ClientTestAccess.h"
#include "FakeBifrost.h"
#include "ReconnectListener.h"
#include "RedwoodClientInterface.h"
#include "RedwoodHeldRequests.h"
#include "RedwoodPendingReplies.h"
#include "RedwoodSettings.h"

namespace RedwoodBifrostClientTest {
  using namespace RedwoodFakeBifrost;

  // Just over the time a held request waits for the socket.
  constexpr float PastGraceSeconds = FRedwoodHeldRequests::GraceSeconds + 0.1f;
  // A reconnect with no backoff runs on the next tick.
  constexpr float NextTickSeconds = 0.01f;

  const FString PlayerId = TEXT("p1");
  const FString Password = TEXT("secret");
  const FString FirstToken = TEXT("tok-1");
  const FString RealmId = TEXT("realm-1");
  const FString CharacterId = TEXT("char-1");
  const FString OtherCharacterId = TEXT("char-2");

  const TCHAR *const NotConnectedToDirector =
    TEXT("Not connected to Director.");
  const TCHAR *const NotConnectedToRealm = TEXT("Not connected to Realm.");

  // A login and a logout write the remember-me save game. Keep the
  // developer's slot as the bytes on disk: take them before the test and put
  // them back after. A slot that is there but cannot be read is left alone.
  struct FRedwoodSaveSlotGuard {
    const FString SlotName = TEXT("RedwoodSaveGame");
    bool bExisted = false;
    bool bRead = false;
    TArray<uint8> Bytes;

    FRedwoodSaveSlotGuard() {
      bExisted = UGameplayStatics::DoesSaveGameExist(SlotName, 0);
      if (bExisted) {
        bRead = UGameplayStatics::LoadDataFromSlot(Bytes, SlotName, 0);
      }
    }

    ~FRedwoodSaveSlotGuard() {
      if (!bExisted) {
        UGameplayStatics::DeleteGameInSlot(SlotName, 0);
      } else if (bRead) {
        UGameplayStatics::SaveDataToSlot(Bytes, SlotName, 0);
      }
    }
  };

  // The realm's regions start pings when they arrive. With no attempts the
  // client keeps the regions and pings nothing, so a test with regions in
  // the realm's answer touches no network.
  struct FNoPingsGuard {
    const int32 SavedAttempts;

    FNoPingsGuard() :
      SavedAttempts(GetMutableDefault<URedwoodSettings>()->PingAttempts) {
      GetMutableDefault<URedwoodSettings>()->PingAttempts = 0;
    }

    ~FNoPingsGuard() {
      GetMutableDefault<URedwoodSettings>()->PingAttempts = SavedAttempts;
    }
  };

  // Records the answers of one request delegate.
  template <typename TOutput> struct TCapture {
    int32 Count = 0;
    TOutput Last;

    TDelegate<void(const TOutput &)> Delegate() {
      return TDelegate<void(const TOutput &)>::CreateLambda(
        [this](const TOutput &Output) {
          ++Count;
          Last = Output;
        }
      );
    }
  };

  inline TSharedPtr<FJsonObject> NoError() {
    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("error"), TEXT(""));
    return Response;
  }

  // The player of a resume reply: the identity, with no token.
  inline TSharedPtr<FJsonObject> ResumeIdentity() {
    TSharedPtr<FJsonObject> Player = MakeShared<FJsonObject>();
    Player->SetStringField(TEXT("playerId"), PlayerId);
    Player->SetStringField(TEXT("username"), PlayerId);
    Player->SetStringField(TEXT("nickname"), TEXT("Nick"));
    return Player;
  }

  inline FRedwoodRealm TestRealm() {
    FRedwoodRealm Realm;
    Realm.Id = RealmId;
    Realm.Name = TEXT("Test Realm");
    return Realm;
  }

  // The position of the newest message with this event in what the socket
  // sent, or -1. Tells which of two requests went out first.
  inline int32 IndexOfLast(
    const FFakeBifrostSocket &Socket, const FString &Event
  ) {
    for (int32 Index = Socket.Sent.Num() - 1; Index >= 0; --Index) {
      if (
        Socket.Sent[Index].IsValid() &&
        Socket.Sent[Index].RequestEvent() == Event
      ) {
        return Index;
      }
    }
    return -1;
  }

  // What the client and its connection log when a socket drops: the
  // connection warns that it reconnects, the client reports the loss as an
  // error. Drops counts the sockets that go down in the test.
  inline void ExpectDrop(FAutomationTestBase &Test, int32 Drops = 1) {
    Test.AddExpectedMessage(
      TEXT("reconnecting"), ELogVerbosity::Warning, Contains, Drops
    );
    Test.AddExpectedError(
      TEXT("Lost the connection to Bifrost"), Contains, Drops
    );
  }

  // The connection's warning when a resume is refused for good.
  inline void ExpectSessionGone(FAutomationTestBase &Test) {
    Test.AddExpectedMessage(
      TEXT("session is gone"), ELogVerbosity::Warning, Contains, 1
    );
  }

  // The client, its listeners and the fake server of one test. The client
  // is taken down in the destructor, before the fake goes away.
  struct FClientHarness {
    FAutomationTestBase &Test;
    FFrameCounterGuard FrameCounter;
    FRedwoodSaveSlotGuard SaveSlot;
    FNoPingsGuard NoPings;
    FFakeBifrost Bifrost;
    TStrongObjectPtr<URedwoodClientInterface> Interface;
    TStrongObjectPtr<URedwoodReconnectListener> Listener;

    // The first connect, as InitializeDirectorConnection reports it.
    int32 ConnectedCalls = 0;
    FString LastConnectError;
    // The join of the session, for the server's own messages on the channel.
    FString JoinRef;

    explicit FClientHarness(FAutomationTestBase &InTest) :
      Test(InTest), Interface(NewObject<URedwoodClientInterface>()),
      Listener(NewObject<URedwoodReconnectListener>()) {
      Listener->Watch(Interface.Get());
      // A timer set in a frame the manager has not ticked yet waits in a
      // pending set until the end of the next tick, and would fire one step
      // late. The empty tick lines every later timer up with the steps.
      Advance(0.0f);
    }

    ~FClientHarness() {
      Interface->Deinitialize();
    }

    URedwoodClientInterface *Client() const {
      return Interface.Get();
    }

    TSharedPtr<FFakeBifrostSocket> Socket() const {
      return Bifrost.Socket();
    }

    // The client's timers tick with the client, once per frame.
    void Advance(float Seconds) {
      ++GFrameCounter;
      Client()->Tick(Seconds);
    }

    // Connects; the server has not accepted yet.
    void Connect() {
      Client()->InitializeDirectorConnection(
        FRedwoodSocketConnectedDelegate::CreateLambda(
          [this](const FRedwoodSocketConnected &Details) {
            ++ConnectedCalls;
            LastConnectError = Details.Error;
          }
        )
      );
    }

    // The server accepts the client's newest socket.
    void Accept() {
      Test.TestTrue(
        TEXT("The server can open the client's newest socket"), Bifrost.Open()
      );
    }

    // Connects, and the server accepts.
    void Open() {
      Connect();
      Accept();
    }

    // Sends a local login and returns its join, unanswered.
    FPhoenixMessage SendLogin(TCapture<FRedwoodAuthUpdate> &Update) {
      Client()->Login(
        PlayerId, Password, TEXT("local"), false, Update.Delegate()
      );
      return Socket()->Last(TEXT("phx_join"));
    }

    // Logs in and the server accepts.
    void LoginOk(
      const FString &SessionId = TEXT("s1"),
      const FString &ResumeToken = TEXT("rt1")
    ) {
      TCapture<FRedwoodAuthUpdate> Update;
      const FPhoenixMessage Join = SendLogin(Update);
      JoinRef = Join.Ref;
      Socket()->Reply(
        Join,
        TEXT("ok"),
        FFakeBifrost::JoinReply(
          SessionId,
          ResumeToken,
          FFakeBifrost::LoginPlayer(PlayerId, FirstToken)
        )
      );
    }

    // Asks to enter the realm and returns the step, unanswered.
    FPhoenixMessage SendEnterRealm(TCapture<FRedwoodSocketConnected> &Entered) {
      Client()->InitializeRealmConnection(TestRealm(), Entered.Delegate());
      return Socket()->Last(TEXT("session.enter-realm"));
    }

    // Enters the realm and the realm accepts, with no regions.
    void EnterRealmOk() {
      TCapture<FRedwoodSocketConnected> Entered;
      Socket()->ReplyOk(SendEnterRealm(Entered), NoError());
    }

    // Selects the character and the realm accepts.
    void SelectCharacterOk() {
      Client()->SetSelectedCharacter(CharacterId);
      Socket()->ReplyOk(
        Socket()->Last(TEXT("session.select-character")), NoError()
      );
    }

    // Connected, logged in, in the realm, with the character selected.
    void EnterSessionWithCharacter() {
      Open();
      LoginOk();
      EnterRealmOk();
      SelectCharacterOk();
    }

    // Lets the socket drop and the backoff bring a new one, open. The new
    // socket resumes by itself; the test answers that join.
    void DropAndReopen(float PastDelaySeconds = PastFirstDelaySeconds) {
      Socket()->Drop();
      Advance(PastDelaySeconds);
      Accept();
    }

    // The server accepts the resume with the session as the client had it.
    void ResumeOk(
      const FString &ResumedRealmId = RealmId,
      const FString &ResumedCharacterId = CharacterId
    ) {
      const FPhoenixMessage Resume = Socket()->Last(TEXT("phx_join"));
      JoinRef = Resume.Ref;
      Socket()->Reply(
        Resume,
        TEXT("ok"),
        FFakeBifrost::JoinReply(
          TEXT("s1"),
          TEXT("rt2"),
          ResumeIdentity(),
          ResumedRealmId,
          ResumedCharacterId
        )
      );
    }

    // The server refuses the resume for good: the session is gone.
    void RefuseResume() {
      Socket()->RefuseJoin(
        Socket()->Last(TEXT("phx_join")), TEXT("resume_refused")
      );
    }

    int32 HeldCount() const {
      return FRedwoodClientTestAccess::HeldRequestCount(*Client());
    }

    bool HasSession() const {
      return FRedwoodClientTestAccess::Connection(*Client())->HasSession();
    }
  };
}
