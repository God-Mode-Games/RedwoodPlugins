// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins URedwoodClientInterface on its one connection to Bifrost, with the
// fake of FakeBifrost.h as the server. BifrostConnectionTest pins the
// transport (framing, resume, push ids, heartbeat, backoff, step order);
// this file pins what the client does with it:
//   1. A login is the join of the session. An accepted join logs the player
//      in; a refused one reports the director's text, and "Must verify
//      account" is its own outcome. A login whose reply is lost with the
//      socket is answered with the lost-reply error.
//   2. Entering a realm is the step `session.enter-realm`. Its answer holds
//      the regions; a refusal leaves the session in no realm. A realm request
//      before the entry fails at once, and is not held.
//   3. A drop in the realm is one lost connection. A request that waited on
//      the socket fails with the lost-reply error; requests made in the gap
//      are held and nothing goes out. The resume brings the session back as
//      it was: the held requests go out, and no step is sent again.
//   4. A resume that gives the session no realm but names the locked one
//      enters it again before the held requests go, also when this client's
//      own entry was lost with the socket and it never learned the realm; a
//      resume that names another character selects the player's one again.
//      Asking for the realm the session is in sends nothing.
//   5. A resume the pod refuses is the same outage: another socket, one lost
//      connection. A refused resume logs the player in again with the token,
//      then enters the realm and selects the character before the held
//      requests go. A refused re-login, or a refused re-entry, ends the
//      session: the held requests fail, the game hears the failure once, and
//      later requests fail at once; a new session the re-login made is
//      logged out.
//   6. When the socket is not back within the grace, a held request fails,
//      and later requests fail at once.
//   7. A logout during the drop sends nothing, fails the held requests, and
//      the socket that comes back has no session to resume. A logout while
//      a resume is in flight ends the session the resume brings back, and
//      logs nobody in again. A logout while connected sends
//      `session.logout`; the end of the channel that follows is not a lost
//      connection, and the next login joins the same socket.
//   8. `closed replaced` ends the session: the game hears why, a waiting
//      reply fails, requests fail at once, and the end of the channel that
//      follows is not a lost connection. `closed reconnect` is one lost
//      connection, and the session is resumed on a new socket at once.
//   9. A first connect that fails is one lost connection until a socket
//      opens; that open is the first connect and a reestablished connection.
//  10. A character friend call made in the gap is held, and goes out after
//      the resume for the selected character; one with no character selected
//      fails at once.
//  11. A zone assignment reaches the game once for its push id, and not at
//      all for a player at character select with no join out.
//  12. A character friend alert the director pushes reaches the game with
//      its fields; one the game cannot act on is dropped with a warning.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "TimerManager.h"
#include "UObject/StrongObjectPtr.h"

#include "Kismet/GameplayStatics.h"

#include "CharacterFriendAlertListener.h"
#include "ClientTestAccess.h"
#include "FakeBifrost.h"
#include "JoinServerListener.h"
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

  TSharedPtr<FJsonObject> NoError() {
    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("error"), TEXT(""));
    return Response;
  }

  // The player of a resume reply: the identity, with no token.
  TSharedPtr<FJsonObject> ResumeIdentity() {
    TSharedPtr<FJsonObject> Player = MakeShared<FJsonObject>();
    Player->SetStringField(TEXT("playerId"), PlayerId);
    Player->SetStringField(TEXT("username"), PlayerId);
    Player->SetStringField(TEXT("nickname"), TEXT("Nick"));
    return Player;
  }

  FRedwoodRealm TestRealm() {
    FRedwoodRealm Realm;
    Realm.Id = RealmId;
    Realm.Name = TEXT("Test Realm");
    return Realm;
  }

  // The position of the newest message with this event in what the socket
  // sent, or -1. Tells which of two requests went out first.
  int32 IndexOfLast(const FFakeBifrostSocket &Socket, const FString &Event) {
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
  void ExpectDrop(FAutomationTestBase &Test, int32 Drops = 1) {
    Test.AddExpectedMessage(
      TEXT("reconnecting"), ELogVerbosity::Warning, Contains, Drops
    );
    Test.AddExpectedError(
      TEXT("Lost the connection to Bifrost"), Contains, Drops
    );
  }

  // The connection's warning when a resume is refused for good.
  void ExpectSessionGone(FAutomationTestBase &Test) {
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientLoginTest,
  "Redwood.Bifrost.Client.Login",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientLoginTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostClientTest;
  FClientHarness H(*this);
  H.Open();
  TestEqual(TEXT("The first connect is reported once"), H.ConnectedCalls, 1);
  TestTrue(TEXT("With no error"), H.LastConnectError.IsEmpty());
  TestFalse(TEXT("Not logged in before the login"), H.Client()->IsLoggedIn());

  // A director request before any login has no session to ride.
  TCapture<FString> BeforeLogin;
  H.Client()->RequestFriend(TEXT("p2"), BeforeLogin.Delegate());
  TestEqual(
    TEXT("A request before the login fails at once"), BeforeLogin.Count, 1
  );
  TestEqual(
    TEXT("With the not-connected error"),
    BeforeLogin.Last,
    NotConnectedToDirector
  );
  TestEqual(TEXT("Nothing went out"), H.Socket()->Sent.Num(), 0);

  // The director refuses.
  TCapture<FRedwoodAuthUpdate> Refused;
  FPhoenixMessage Join = H.SendLogin(Refused);
  if (!TestTrue(TEXT("A login sends a join"), Join.IsValid())) {
    return false;
  }
  TestEqual(
    TEXT("The join names the contract version"),
    static_cast<int32>(Join.Payload->GetNumberField(TEXT("v"))),
    FRedwoodBifrostConnection::ContractVersion
  );
  const TSharedPtr<FJsonObject> Login =
    Join.Payload->GetObjectField(TEXT("login"));
  TestEqual(
    TEXT("The login names its provider"),
    Login->GetStringField(TEXT("provider")),
    TEXT("local")
  );
  TestEqual(
    TEXT("The login carries the username"),
    Login->GetStringField(TEXT("username")),
    PlayerId
  );
  TestEqual(
    TEXT("The login carries the secret"),
    Login->GetStringField(TEXT("secret")),
    Password
  );

  H.Socket()->RefuseJoin(Join, TEXT("login_failed"), TEXT("Wrong password"));
  TestEqual(TEXT("The refusal is answered once"), Refused.Count, 1);
  TestEqual(
    TEXT("As an error"), Refused.Last.Type, ERedwoodAuthUpdateType::Error
  );
  TestEqual(
    TEXT("With the director's text"),
    Refused.Last.Message,
    TEXT("Wrong password")
  );
  TestFalse(TEXT("Not logged in after a refusal"), H.Client()->IsLoggedIn());

  // The account must verify its email first.
  TCapture<FRedwoodAuthUpdate> Unverified;
  Join = H.SendLogin(Unverified);
  H.Socket()->RefuseJoin(
    Join, TEXT("login_failed"), TEXT("Must verify account")
  );
  TestEqual(TEXT("The verification refusal is answered"), Unverified.Count, 1);
  TestEqual(
    TEXT("It is its own outcome"),
    Unverified.Last.Type,
    ERedwoodAuthUpdateType::MustVerifyAccount
  );
  TestFalse(TEXT("Not logged in while unverified"), H.Client()->IsLoggedIn());
  H.Client()->CancelWaitingForAccountVerification();

  // The director accepts.
  TCapture<FRedwoodAuthUpdate> Accepted;
  Join = H.SendLogin(Accepted);
  H.Socket()->Reply(
    Join,
    TEXT("ok"),
    FFakeBifrost::JoinReply(
      TEXT("s1"),
      TEXT("rt1"),
      FFakeBifrost::LoginPlayer(PlayerId, FirstToken, TEXT("Nick"))
    )
  );
  TestEqual(TEXT("The accepted login is answered once"), Accepted.Count, 1);
  TestEqual(
    TEXT("As a success"), Accepted.Last.Type, ERedwoodAuthUpdateType::Success
  );
  TestTrue(TEXT("Logged in"), H.Client()->IsLoggedIn());
  TestEqual(
    TEXT("The player id is the director's"), H.Client()->GetPlayerId(), PlayerId
  );
  TestEqual(
    TEXT("The nickname is the director's"),
    H.Client()->GetNickname(),
    TEXT("Nick")
  );
  TestTrue(TEXT("The connection holds the session"), H.HasSession());
  TestFalse(
    TEXT("A new session is in no realm"), H.Client()->IsRealmConnected()
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientLoginLostTest,
  "Redwood.Bifrost.Client.LoginLostToDrop",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientLoginLostTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostClientTest;
  ExpectDrop(*this);
  FClientHarness H(*this);
  H.Open();

  TCapture<FRedwoodAuthUpdate> Update;
  const FPhoenixMessage Join = H.SendLogin(Update);
  TestTrue(
    TEXT("The login waits for its reply"), Join.IsValid() && Update.Count == 0
  );

  H.Socket()->Drop();
  TestEqual(TEXT("The login is answered at the drop"), Update.Count, 1);
  TestEqual(
    TEXT("As an error"), Update.Last.Type, ERedwoodAuthUpdateType::Error
  );
  TestEqual(
    TEXT("That says the reply was lost"),
    Update.Last.Message,
    FRedwoodPendingReplies::LostReplyError
  );
  TestFalse(TEXT("Not logged in"), H.Client()->IsLoggedIn());
  TestEqual(
    TEXT("The game hears the lost connection"), H.Listener->LostCount, 1
  );
  TestFalse(
    TEXT("No session waits to come back"),
    FRedwoodClientTestAccess::IsReauthPending(*H.Client())
  );

  // The socket comes back with nothing to resume; the player logs in again.
  H.Advance(PastFirstDelaySeconds);
  H.Accept();
  TestEqual(TEXT("No join goes out by itself"), H.Socket()->Sent.Num(), 0);
  TestEqual(TEXT("The connection is back"), H.Listener->ReestablishedCount, 1);
  H.LoginOk();
  TestTrue(TEXT("The next login works"), H.Client()->IsLoggedIn());
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientRealmEntryTest,
  "Redwood.Bifrost.Client.RealmEntry",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientRealmEntryTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostClientTest;
  FClientHarness H(*this);
  H.Open();
  H.LoginOk();

  // A realm request before the entry has no realm to wait for.
  TCapture<FRedwoodListCharactersOutput> Early;
  H.Client()->ListCharacters(Early.Delegate());
  TestEqual(
    TEXT("A realm request before the entry is answered at once"), Early.Count, 1
  );
  TestEqual(
    TEXT("With the not-connected error"), Early.Last.Error, NotConnectedToRealm
  );
  TestEqual(TEXT("It is not held"), H.HeldCount(), 0);
  TestEqual(
    TEXT("Nothing went out"),
    H.Socket()->CountSent(TEXT("realm:characters:list")),
    0
  );

  // The realm refuses the session.
  TCapture<FRedwoodSocketConnected> Refused;
  FPhoenixMessage Step = H.SendEnterRealm(Refused);
  if (!TestTrue(TEXT("Entering a realm sends the step"), Step.IsValid())) {
    return false;
  }
  TestEqual(
    TEXT("The step is its own event"), Step.Event, TEXT("session.enter-realm")
  );
  TestEqual(
    TEXT("It names the realm"),
    Step.Payload->GetStringField(TEXT("realmId")),
    RealmId
  );
  H.Socket()->ReplyWithError(Step, TEXT("Realm is full"));
  TestEqual(TEXT("The refusal is answered once"), Refused.Count, 1);
  TestEqual(
    TEXT("With the realm's error"), Refused.Last.Error, TEXT("Realm is full")
  );
  TestFalse(TEXT("The session is in no realm"), H.Client()->IsRealmConnected());
  TestTrue(TEXT("No realm is remembered"), H.Client()->GetRealmId().IsEmpty());

  // The realm accepts, with its regions.
  TCapture<FRedwoodSocketConnected> Entered;
  Step = H.SendEnterRealm(Entered);
  TSharedPtr<FJsonObject> Region = MakeShared<FJsonObject>();
  Region->SetStringField(TEXT("name"), TEXT("us-east"));
  Region->SetStringField(TEXT("ping"), TEXT("ping.test"));
  TArray<TSharedPtr<FJsonValue>> Regions;
  Regions.Add(MakeShared<FJsonValueObject>(Region));
  TSharedPtr<FJsonObject> Started = NoError();
  Started->SetArrayField(TEXT("regions"), Regions);
  H.Socket()->ReplyOk(Step, Started);
  TestEqual(TEXT("The entry is answered once"), Entered.Count, 1);
  TestTrue(TEXT("With no error"), Entered.Last.Error.IsEmpty());
  TestTrue(TEXT("The session is in the realm"), H.Client()->IsRealmConnected());
  TestEqual(TEXT("The realm is remembered"), H.Client()->GetRealmId(), RealmId);
  FRedwoodRealm Current;
  TestTrue(
    TEXT("The realm's details are kept"), H.Client()->IsRealmConnected(Current)
  );
  TestEqual(
    TEXT("They are the realm that was entered"),
    Current.Name,
    TEXT("Test Realm")
  );
  const TMap<FString, FString> PingUrls =
    FRedwoodClientTestAccess::RegionPingUrls(*H.Client());
  TestEqual(TEXT("The start answer's regions are taken"), PingUrls.Num(), 1);
  const FString *PingUrl = PingUrls.Find(TEXT("us-east"));
  TestTrue(
    TEXT("Each region is pinged at the address the realm gave"),
    PingUrl != nullptr && *PingUrl == TEXT("ping.test")
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientDropResumeTest,
  "Redwood.Bifrost.Client.DropInRealmResumes",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientDropResumeTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostClientTest;
  ExpectDrop(*this);
  FClientHarness H(*this);
  H.EnterSessionWithCharacter();
  const FPhoenixMessage Select =
    H.Bifrost.Sockets[0]->Last(TEXT("session.select-character"));
  TestTrue(
    TEXT("Selecting a character is a session step"),
    Select.IsValid() &&
      Select.Payload->GetStringField(TEXT("characterId")) == CharacterId
  );

  TCapture<FRedwoodListPlayersOutput> InFlight;
  H.Client()->ListFriends(ERedwoodFriendListType::All, InFlight.Delegate());
  TestTrue(
    TEXT("The request went out"),
    H.Socket()->Last(TEXT("director:friends:list")).IsValid()
  );
  TestEqual(
    TEXT("It waits for its reply"),
    FRedwoodClientTestAccess::PendingReplyCount(*H.Client()),
    1
  );

  H.Socket()->Drop();
  TestEqual(
    TEXT("The game hears one lost connection"), H.Listener->LostCount, 1
  );
  TestEqual(TEXT("The waiting request is answered once"), InFlight.Count, 1);
  TestEqual(
    TEXT("With the lost-reply error"),
    InFlight.Last.Error,
    FRedwoodPendingReplies::LostReplyError
  );
  TestEqual(
    TEXT("Nothing waits for a reply"),
    FRedwoodClientTestAccess::PendingReplyCount(*H.Client()),
    0
  );
  TestFalse(TEXT("Not logged in during the gap"), H.Client()->IsLoggedIn());
  TestTrue(
    TEXT("The session waits to come back"),
    FRedwoodClientTestAccess::IsReauthPending(*H.Client())
  );

  TCapture<FString> Friend;
  TCapture<FString> Invite;
  H.Client()->RequestFriend(TEXT("p2"), Friend.Delegate());
  H.Client()->InviteToParty(TEXT("p2"), Invite.Delegate());
  TestEqual(TEXT("A director and a realm request are held"), H.HeldCount(), 2);
  TestEqual(TEXT("Neither is answered"), Friend.Count + Invite.Count, 0);
  TestEqual(
    TEXT("The friend request did not go out"),
    H.Socket()->CountSent(TEXT("director:friends:add")),
    0
  );
  TestEqual(
    TEXT("The invite did not go out"),
    H.Socket()->CountSent(TEXT("realm:parties:invites:initiate")),
    0
  );

  H.Advance(PastFirstDelaySeconds);
  if (!TestEqual(
        TEXT("A new socket after the backoff"), H.Bifrost.Sockets.Num(), 2
      )) {
    return false;
  }
  H.Accept();
  const FPhoenixMessage Resume = H.Socket()->Last(TEXT("phx_join"));
  if (!TestTrue(TEXT("The new socket resumes"), Resume.IsValid())) {
    return false;
  }
  const TSharedPtr<FJsonObject> ResumePayload =
    Resume.Payload->GetObjectField(TEXT("resume"));
  TestEqual(
    TEXT("With the session id"),
    ResumePayload->GetStringField(TEXT("sessionId")),
    TEXT("s1")
  );
  TestEqual(
    TEXT("With the token"),
    ResumePayload->GetStringField(TEXT("resumeToken")),
    TEXT("rt1")
  );
  TestEqual(
    TEXT("Nothing is reestablished before the answer"),
    H.Listener->ReestablishedCount,
    0
  );
  TestEqual(TEXT("The held requests wait for it"), H.HeldCount(), 2);

  H.ResumeOk();
  TestEqual(
    TEXT("The game hears the connection come back once"),
    H.Listener->ReestablishedCount,
    1
  );
  TestEqual(TEXT("It heard no second loss"), H.Listener->LostCount, 1);
  TestTrue(TEXT("Logged in again"), H.Client()->IsLoggedIn());
  TestTrue(TEXT("In the realm again"), H.Client()->IsRealmConnected());
  TestFalse(
    TEXT("The session is back"),
    FRedwoodClientTestAccess::IsReauthPending(*H.Client())
  );
  TestEqual(TEXT("Nothing is held"), H.HeldCount(), 0);
  const FPhoenixMessage FriendRequest =
    H.Socket()->Last(TEXT("director:friends:add"));
  TestTrue(
    TEXT("The held friend request went out on the new socket"),
    FriendRequest.IsValid()
  );
  const FPhoenixMessage InviteRequest =
    H.Socket()->Last(TEXT("realm:parties:invites:initiate"));
  TestTrue(
    TEXT("The held invite went out for the selected character"),
    InviteRequest.IsValid() &&
      InviteRequest.Payload->GetObjectField(TEXT("payload"))
          ->GetStringField(TEXT("characterId")) == CharacterId
  );
  TestEqual(
    TEXT("The realm is not entered again"),
    H.Socket()->CountSent(TEXT("session.enter-realm")),
    0
  );
  TestEqual(
    TEXT("The character is not selected again"),
    H.Socket()->CountSent(TEXT("session.select-character")),
    0
  );

  // The held request's caller still gets its answer.
  H.Socket()->ReplyOk(FriendRequest, NoError());
  TestEqual(TEXT("The held request is answered once"), Friend.Count, 1);
  TestTrue(TEXT("With the director's answer"), Friend.Last.IsEmpty());
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientResumeLockedRealmTest,
  "Redwood.Bifrost.Client.ResumeEntersLockedRealmAgain",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientResumeLockedRealmTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostClientTest;
  ExpectDrop(*this);
  FClientHarness H(*this);
  H.EnterSessionWithCharacter();
  H.Socket()->Drop();
  TCapture<FString> Invite;
  H.Client()->InviteToParty(TEXT("p2"), Invite.Delegate());
  H.Advance(PastFirstDelaySeconds);
  H.Accept();

  // The realm never answered the session's start: the reply has no realm,
  // only the one the session is locked to.
  const FPhoenixMessage Resume = H.Socket()->Last(TEXT("phx_join"));
  H.Socket()->Reply(
    Resume,
    TEXT("ok"),
    FFakeBifrost::JoinReply(
      TEXT("s1"), TEXT("rt2"), ResumeIdentity(), FString(), FString(), RealmId
    )
  );
  const FPhoenixMessage Enter = H.Socket()->Last(TEXT("session.enter-realm"));
  if (!TestTrue(TEXT("The client enters the realm again"), Enter.IsValid())) {
    return false;
  }
  TestEqual(
    TEXT("The locked realm"),
    Enter.Payload->GetStringField(TEXT("realmId")),
    RealmId
  );
  TestEqual(
    TEXT("Not reestablished before the realm answers"),
    H.Listener->ReestablishedCount,
    0
  );
  TestEqual(TEXT("The held request waits"), H.HeldCount(), 1);
  TestFalse(TEXT("Not in the realm yet"), H.Client()->IsRealmConnected());

  H.Socket()->ReplyOk(Enter, NoError());
  TestEqual(
    TEXT("Reestablished once the realm answers"),
    H.Listener->ReestablishedCount,
    1
  );
  TestTrue(TEXT("In the realm again"), H.Client()->IsRealmConnected());
  TestEqual(
    TEXT("The character is selected again for the realm that lost it"),
    H.Socket()->CountSent(TEXT("session.select-character")),
    1
  );
  TestTrue(
    TEXT("The held request went out"),
    H.Socket()->Last(TEXT("realm:parties:invites:initiate")).IsValid()
  );
  TestTrue(
    TEXT("After the character selection"),
    IndexOfLast(*H.Socket(), TEXT("session.select-character")) <
      IndexOfLast(*H.Socket(), TEXT("realm:parties:invites:initiate"))
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientResumeReselectsTest,
  "Redwood.Bifrost.Client.ResumeSelectsCharacterAgain",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientResumeReselectsTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostClientTest;
  ExpectDrop(*this);
  FClientHarness H(*this);
  H.EnterSessionWithCharacter();
  H.DropAndReopen();

  // The session knows an older character than the player selected.
  H.ResumeOk(RealmId, TEXT("char-0"));
  const FPhoenixMessage Select =
    H.Socket()->Last(TEXT("session.select-character"));
  if (!TestTrue(
        TEXT("The client selects its character again"), Select.IsValid()
      )) {
    return false;
  }
  TestEqual(
    TEXT("The selected one"),
    Select.Payload->GetStringField(TEXT("characterId")),
    CharacterId
  );
  TestEqual(
    TEXT("The realm is not entered again"),
    H.Socket()->CountSent(TEXT("session.enter-realm")),
    0
  );
  TestEqual(TEXT("Reestablished once"), H.Listener->ReestablishedCount, 1);
  TestEqual(
    TEXT("The client still holds its selection"),
    H.Client()->GetCharacterId(),
    CharacterId
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientResumeNamedRealmTest,
  "Redwood.Bifrost.Client.ResumeEntersRealmBifrostNames",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientResumeNamedRealmTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostClientTest;
  ExpectDrop(*this);
  FClientHarness H(*this);
  H.Open();
  H.LoginOk();

  // The client's own entry is lost with the socket before the realm answers.
  TCapture<FRedwoodSocketConnected> Entered;
  const FPhoenixMessage Step = H.SendEnterRealm(Entered);
  TestTrue(
    TEXT("The entry waits for the realm"), Step.IsValid() && Entered.Count == 0
  );
  H.Socket()->Drop();
  TestEqual(TEXT("The entry is answered at the drop"), Entered.Count, 1);
  TestEqual(
    TEXT("That its reply was lost"),
    Entered.Last.Error,
    FRedwoodPendingReplies::LostReplyError
  );
  TestFalse(
    TEXT("The client does not know it is in the realm"),
    H.Client()->IsRealmConnected()
  );
  TestTrue(TEXT("No realm is remembered"), H.Client()->GetRealmId().IsEmpty());
  TestEqual(TEXT("One lost connection"), H.Listener->LostCount, 1);

  // The session is locked to that realm, which never answered: Bifrost
  // names it, and the client enters it.
  H.Advance(PastFirstDelaySeconds);
  H.Accept();
  H.Socket()->Reply(
    H.Socket()->Last(TEXT("phx_join")),
    TEXT("ok"),
    FFakeBifrost::JoinReply(
      TEXT("s1"), TEXT("rt2"), ResumeIdentity(), FString(), FString(), RealmId
    )
  );
  const FPhoenixMessage Enter = H.Socket()->Last(TEXT("session.enter-realm"));
  if (!TestTrue(
        TEXT("The client enters the realm Bifrost names"), Enter.IsValid()
      )) {
    return false;
  }
  TestEqual(
    TEXT("That realm"), Enter.Payload->GetStringField(TEXT("realmId")), RealmId
  );
  TestEqual(
    TEXT("Not reestablished before the realm answers"),
    H.Listener->ReestablishedCount,
    0
  );
  H.Socket()->ReplyOk(Enter, NoError());
  TestEqual(TEXT("Reestablished once"), H.Listener->ReestablishedCount, 1);
  TestTrue(TEXT("In the realm"), H.Client()->IsRealmConnected());
  TestEqual(TEXT("The realm is remembered"), H.Client()->GetRealmId(), RealmId);

  // The game asks for that realm again: the session is in it already.
  TCapture<FRedwoodSocketConnected> Again;
  H.Client()->InitializeRealmConnection(TestRealm(), Again.Delegate());
  TestEqual(
    TEXT("The entry of the realm the session is in is answered at once"),
    Again.Count,
    1
  );
  TestTrue(TEXT("As a success"), Again.Last.Error.IsEmpty());
  TestEqual(
    TEXT("With no new step"),
    H.Socket()->CountSent(TEXT("session.enter-realm")),
    1
  );
  TestTrue(TEXT("Still in the realm"), H.Client()->IsRealmConnected());
  FRedwoodRealm Current;
  TestTrue(TEXT("With its details"), H.Client()->IsRealmConnected(Current));
  TestEqual(TEXT("From the game's request"), Current.Name, TEXT("Test Realm"));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientResumeRefusedByPodTest,
  "Redwood.Bifrost.Client.ResumeRefusedByPodIsOneOutage",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientResumeRefusedByPodTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostClientTest;
  // The socket's drop, and the pod's refusal: two reconnects, one loss.
  AddExpectedMessage(TEXT("reconnecting"), ELogVerbosity::Warning, Contains, 2);
  AddExpectedError(TEXT("Lost the connection to Bifrost"), Contains, 1);
  FClientHarness H(*this);
  H.EnterSessionWithCharacter();
  H.Socket()->Drop();
  TCapture<FString> Friend;
  H.Client()->RequestFriend(TEXT("p2"), Friend.Delegate());
  H.Advance(PastFirstDelaySeconds);
  H.Accept();

  // This pod cannot take the resume; the session is still there.
  H.Socket()->RefuseJoin(
    H.Socket()->Last(TEXT("phx_join")), TEXT("rate_limited")
  );
  TestEqual(TEXT("The same outage: no second loss"), H.Listener->LostCount, 1);
  TestEqual(
    TEXT("Nothing is reestablished"), H.Listener->ReestablishedCount, 0
  );
  TestTrue(
    TEXT("The session still waits to come back"),
    FRedwoodClientTestAccess::IsReauthPending(*H.Client())
  );
  TestEqual(TEXT("The held request still waits"), H.HeldCount(), 1);
  TestEqual(TEXT("No authentication failure"), H.Listener->AuthFailedCount, 0);

  // Another socket takes it.
  H.Advance(PastSecondDelaySeconds);
  if (!TestEqual(
        TEXT("A third socket after the longer delay"),
        H.Bifrost.Sockets.Num(),
        3
      )) {
    return false;
  }
  H.Accept();
  H.ResumeOk();
  TestEqual(TEXT("Reestablished once"), H.Listener->ReestablishedCount, 1);
  TestEqual(TEXT("Lost once"), H.Listener->LostCount, 1);
  TestTrue(TEXT("Logged in"), H.Client()->IsLoggedIn());
  TestTrue(
    TEXT("The held request went out"),
    H.Socket()->Last(TEXT("director:friends:add")).IsValid()
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientResumeRefusedTest,
  "Redwood.Bifrost.Client.ResumeRefusedLogsInAgain",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientResumeRefusedTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostClientTest;
  ExpectDrop(*this);
  ExpectSessionGone(*this);
  FClientHarness H(*this);
  H.EnterSessionWithCharacter();
  H.Socket()->Drop();
  TCapture<FString> Friend;
  TCapture<FString> Invite;
  H.Client()->RequestFriend(TEXT("p2"), Friend.Delegate());
  H.Client()->InviteToParty(TEXT("p2"), Invite.Delegate());
  H.Advance(PastFirstDelaySeconds);
  H.Accept();

  // The session's grace ran out while the socket was down.
  H.RefuseResume();
  const FPhoenixMessage Relogin = H.Socket()->Last(TEXT("phx_join"));
  if (!TestTrue(
        TEXT("The client logs in again"),
        Relogin.IsValid() && Relogin.Payload->HasField(TEXT("login"))
      )) {
    return false;
  }
  const TSharedPtr<FJsonObject> Login =
    Relogin.Payload->GetObjectField(TEXT("login"));
  TestEqual(
    TEXT("With the token as a local login"),
    Login->GetStringField(TEXT("provider")),
    TEXT("local")
  );
  TestEqual(
    TEXT("As the player"), Login->GetStringField(TEXT("username")), PlayerId
  );
  TestEqual(
    TEXT("With the token of the first login"),
    Login->GetStringField(TEXT("secret")),
    FirstToken
  );
  TestEqual(TEXT("The held requests wait"), H.HeldCount(), 2);

  H.Socket()->Reply(
    Relogin,
    TEXT("ok"),
    FFakeBifrost::JoinReply(
      TEXT("s2"),
      TEXT("rt9"),
      FFakeBifrost::LoginPlayer(PlayerId, TEXT("tok-2"))
    )
  );
  const FPhoenixMessage Enter = H.Socket()->Last(TEXT("session.enter-realm"));
  if (!TestTrue(TEXT("The new session enters the realm"), Enter.IsValid())) {
    return false;
  }
  TestEqual(
    TEXT("The realm the old one was in"),
    Enter.Payload->GetStringField(TEXT("realmId")),
    RealmId
  );
  TestEqual(
    TEXT("Not reestablished before the realm answers"),
    H.Listener->ReestablishedCount,
    0
  );
  TestEqual(TEXT("The held requests still wait"), H.HeldCount(), 2);

  H.Socket()->ReplyOk(Enter, NoError());
  const FPhoenixMessage Select =
    H.Socket()->Last(TEXT("session.select-character"));
  TestTrue(
    TEXT("The new session selects the character"),
    Select.IsValid() &&
      Select.Payload->GetStringField(TEXT("characterId")) == CharacterId
  );
  TestEqual(TEXT("Reestablished once"), H.Listener->ReestablishedCount, 1);
  TestEqual(TEXT("No authentication failure"), H.Listener->AuthFailedCount, 0);
  TestTrue(TEXT("Logged in"), H.Client()->IsLoggedIn());
  TestTrue(TEXT("In the realm"), H.Client()->IsRealmConnected());
  TestEqual(TEXT("Nothing is held"), H.HeldCount(), 0);
  TestTrue(
    TEXT("The held friend request went out"),
    H.Socket()->Last(TEXT("director:friends:add")).IsValid()
  );
  TestTrue(
    TEXT("The held invite went out"),
    H.Socket()->Last(TEXT("realm:parties:invites:initiate")).IsValid()
  );
  TestTrue(
    TEXT("After the character selection"),
    IndexOfLast(*H.Socket(), TEXT("session.select-character")) <
      IndexOfLast(*H.Socket(), TEXT("realm:parties:invites:initiate"))
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientReloginRefusedTest,
  "Redwood.Bifrost.Client.ReloginRefusedEndsSession",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientReloginRefusedTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostClientTest;
  // The drop of the session's socket, and a drop at the title screen after.
  ExpectDrop(*this, 2);
  ExpectSessionGone(*this);
  AddExpectedMessage(
    TEXT("The session cannot come back"), ELogVerbosity::Warning, Contains, 1
  );
  FClientHarness H(*this);
  H.EnterSessionWithCharacter();
  H.Socket()->Drop();
  TCapture<FString> Friend;
  TCapture<FString> Invite;
  H.Client()->RequestFriend(TEXT("p2"), Friend.Delegate());
  H.Client()->InviteToParty(TEXT("p2"), Invite.Delegate());
  H.Advance(PastFirstDelaySeconds);
  H.Accept();
  H.RefuseResume();

  // The token does not work any more.
  H.Socket()->RefuseJoin(
    H.Socket()->Last(TEXT("phx_join")),
    TEXT("login_failed"),
    TEXT("Token expired")
  );
  TestEqual(
    TEXT("The game hears one authentication failure"),
    H.Listener->AuthFailedCount,
    1
  );
  TestEqual(
    TEXT("With the director's text"),
    H.Listener->LastAuthFailure,
    TEXT("Token expired")
  );
  TestEqual(TEXT("Not reestablished"), H.Listener->ReestablishedCount, 0);
  TestFalse(TEXT("Not logged in"), H.Client()->IsLoggedIn());
  TestFalse(TEXT("Not in the realm"), H.Client()->IsRealmConnected());
  TestFalse(TEXT("The connection has no session"), H.HasSession());
  TestEqual(TEXT("Nothing is held"), H.HeldCount(), 0);
  TestEqual(TEXT("The held director request failed"), Friend.Count, 1);
  TestEqual(
    TEXT("With the not-connected error"), Friend.Last, NotConnectedToDirector
  );
  TestEqual(TEXT("The held realm request failed"), Invite.Count, 1);
  TestEqual(
    TEXT("With the not-connected error"), Invite.Last, NotConnectedToRealm
  );
  TestEqual(
    TEXT("Nothing went out for them"),
    H.Socket()->CountSent(TEXT("director:friends:add")),
    0
  );

  // The session is over: nothing waits for it any more.
  TCapture<FRedwoodListPlayersOutput> Later;
  H.Client()->ListFriends(ERedwoodFriendListType::All, Later.Delegate());
  TestEqual(TEXT("A later director request fails at once"), Later.Count, 1);
  TestEqual(
    TEXT("With the not-connected error"),
    Later.Last.Error,
    NotConnectedToDirector
  );
  TCapture<FRedwoodListCharactersOutput> LaterRealm;
  H.Client()->ListCharacters(LaterRealm.Delegate());
  TestEqual(TEXT("A later realm request fails at once"), LaterRealm.Count, 1);
  TestEqual(
    TEXT("With the not-connected error"),
    LaterRealm.Last.Error,
    NotConnectedToRealm
  );
  TestEqual(TEXT("Nothing is held"), H.HeldCount(), 0);

  // The next socket has nothing to bring back. This is the second drop of
  // the outage for the backoff (the socket between them was not up for the
  // stable time), so it waits the second delay.
  H.Socket()->Drop();
  TestEqual(
    TEXT("A drop at the title screen is a new loss"), H.Listener->LostCount, 2
  );
  H.Advance(PastSecondDelaySeconds);
  H.Accept();
  TestEqual(TEXT("No join goes out"), H.Socket()->Sent.Num(), 0);
  TestEqual(TEXT("Nothing is held"), H.HeldCount(), 0);
  TestFalse(TEXT("Still not logged in"), H.Client()->IsLoggedIn());
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientReentryRefusedTest,
  "Redwood.Bifrost.Client.RealmReentryRefusedEndsSession",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientReentryRefusedTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostClientTest;
  ExpectDrop(*this);
  ExpectSessionGone(*this);
  AddExpectedMessage(
    TEXT("The session cannot come back"), ELogVerbosity::Warning, Contains, 1
  );
  FClientHarness H(*this);
  H.EnterSessionWithCharacter();
  H.Socket()->Drop();
  TCapture<FString> Invite;
  H.Client()->InviteToParty(TEXT("p2"), Invite.Delegate());
  H.Advance(PastFirstDelaySeconds);
  H.Accept();
  H.RefuseResume();

  // The re-login makes a new session, but the realm refuses it.
  H.Socket()->Reply(
    H.Socket()->Last(TEXT("phx_join")),
    TEXT("ok"),
    FFakeBifrost::JoinReply(
      TEXT("s2"),
      TEXT("rt9"),
      FFakeBifrost::LoginPlayer(PlayerId, TEXT("tok-2"))
    )
  );
  const FPhoenixMessage Enter = H.Socket()->Last(TEXT("session.enter-realm"));
  if (!TestTrue(TEXT("The new session enters the realm"), Enter.IsValid())) {
    return false;
  }
  H.Socket()->ReplyWithError(Enter, TEXT("Realm is full"));
  TestEqual(
    TEXT("The game hears one authentication failure"),
    H.Listener->AuthFailedCount,
    1
  );
  TestEqual(
    TEXT("With the realm's error"),
    H.Listener->LastAuthFailure,
    TEXT("Realm is full")
  );
  TestEqual(TEXT("Not reestablished"), H.Listener->ReestablishedCount, 0);
  TestTrue(
    TEXT("The new session is logged out of Bifrost"),
    H.Socket()->Last(TEXT("session.logout")).IsValid()
  );
  TestFalse(TEXT("The connection has no session"), H.HasSession());
  TestFalse(TEXT("Not logged in"), H.Client()->IsLoggedIn());
  TestFalse(TEXT("Not in the realm"), H.Client()->IsRealmConnected());
  TestEqual(TEXT("The held realm request failed"), Invite.Count, 1);
  TestEqual(
    TEXT("With the not-connected error"), Invite.Last, NotConnectedToRealm
  );
  TestEqual(TEXT("Nothing is held"), H.HeldCount(), 0);
  TestEqual(
    TEXT("The character selection was not sent"),
    H.Socket()->CountSent(TEXT("session.select-character")),
    0
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientGraceExpiryTest,
  "Redwood.Bifrost.Client.GraceExpiryFailsHeldRequests",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientGraceExpiryTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostClientTest;
  ExpectDrop(*this);
  FClientHarness H(*this);
  H.Open();
  H.LoginOk();
  H.Socket()->Drop();

  TCapture<FString> Friend;
  H.Client()->RequestFriend(TEXT("p2"), Friend.Delegate());
  TestEqual(TEXT("The request is held"), H.HeldCount(), 1);

  // The backoff opens new sockets, but no server accepts one.
  H.Advance(PastGraceSeconds);
  TestEqual(
    TEXT("The held request is answered when the grace ends"), Friend.Count, 1
  );
  TestEqual(
    TEXT("With the not-connected error"), Friend.Last, NotConnectedToDirector
  );
  TestEqual(TEXT("Nothing is held"), H.HeldCount(), 0);

  TCapture<FString> Later;
  H.Client()->RequestFriend(TEXT("p3"), Later.Delegate());
  TestEqual(TEXT("A later request fails at once"), Later.Count, 1);
  TestEqual(
    TEXT("With the not-connected error"), Later.Last, NotConnectedToDirector
  );
  TestEqual(TEXT("It is not held"), H.HeldCount(), 0);
  TestEqual(
    TEXT("One outage is one lost connection"), H.Listener->LostCount, 1
  );
  TestEqual(TEXT("Nothing came back"), H.Listener->ReestablishedCount, 0);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientLogoutDuringDropTest,
  "Redwood.Bifrost.Client.LogoutDuringDrop",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientLogoutDuringDropTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostClientTest;
  ExpectDrop(*this);
  FClientHarness H(*this);
  H.Open();
  H.LoginOk();
  H.EnterRealmOk();
  H.Socket()->Drop();
  TCapture<FString> Friend;
  H.Client()->RequestFriend(TEXT("p2"), Friend.Delegate());
  const int32 SentBefore = H.Socket()->Sent.Num();

  H.Client()->Logout();
  TestEqual(
    TEXT("A logout with no socket sends nothing"),
    H.Socket()->Sent.Num(),
    SentBefore
  );
  TestFalse(TEXT("Not logged in"), H.Client()->IsLoggedIn());
  TestEqual(TEXT("The held request fails"), Friend.Count, 1);
  TestEqual(
    TEXT("With the not-connected error"), Friend.Last, NotConnectedToDirector
  );
  TestFalse(TEXT("The connection forgets the session"), H.HasSession());
  TestFalse(
    TEXT("Nothing waits to come back"),
    FRedwoodClientTestAccess::IsReauthPending(*H.Client())
  );

  H.Advance(PastFirstDelaySeconds);
  if (!TestEqual(
        TEXT("The socket comes back for the next login"),
        H.Bifrost.Sockets.Num(),
        2
      )) {
    return false;
  }
  H.Accept();
  TestEqual(TEXT("No resume is sent"), H.Socket()->Sent.Num(), 0);
  TestEqual(TEXT("The connection is back"), H.Listener->ReestablishedCount, 1);
  TestFalse(TEXT("Still not logged in"), H.Client()->IsLoggedIn());
  TestEqual(TEXT("No authentication failure"), H.Listener->AuthFailedCount, 0);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientLogoutDuringResumeRefusedTest,
  "Redwood.Bifrost.Client.LogoutDuringResumeThenRefused",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientLogoutDuringResumeRefusedTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostClientTest;
  ExpectDrop(*this);
  ExpectSessionGone(*this);
  FClientHarness H(*this);
  H.EnterSessionWithCharacter();
  H.DropAndReopen();
  TestTrue(
    TEXT("The resume is in flight"),
    H.Socket()->Last(TEXT("phx_join")).IsValid()
  );

  // The player leaves while the resume waits for its answer.
  H.Client()->Logout();
  TestFalse(TEXT("Not logged in"), H.Client()->IsLoggedIn());
  TestFalse(TEXT("The connection forgets the session"), H.HasSession());

  // The session was gone anyway. There is nobody to log in again.
  H.RefuseResume();
  TestEqual(
    TEXT("No login join goes out"), H.Socket()->CountSent(TEXT("phx_join")), 1
  );
  TestEqual(
    TEXT("No logout step goes out either"),
    H.Socket()->CountSent(TEXT("session.logout")),
    0
  );
  TestEqual(TEXT("No authentication failure"), H.Listener->AuthFailedCount, 0);
  TestEqual(
    TEXT("Nothing is reestablished"), H.Listener->ReestablishedCount, 0
  );
  TestFalse(
    TEXT("Nothing waits to come back"),
    FRedwoodClientTestAccess::IsReauthPending(*H.Client())
  );
  TestFalse(TEXT("Still not logged in"), H.Client()->IsLoggedIn());
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientLogoutDuringResumeResumedTest,
  "Redwood.Bifrost.Client.LogoutDuringResumeThenResumed",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientLogoutDuringResumeResumedTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostClientTest;
  ExpectDrop(*this);
  FClientHarness H(*this);
  H.EnterSessionWithCharacter();
  H.DropAndReopen();
  H.Client()->Logout();
  TestEqual(
    TEXT("No step goes out while the resume waits for its answer"),
    H.Socket()->CountSent(TEXT("session.logout")),
    0
  );

  // The resume brings the session back after the player left: it is logged
  // out of Bifrost, and the player is not logged back in.
  H.ResumeOk();
  TestEqual(
    TEXT("The resumed session is logged out of Bifrost"),
    H.Socket()->CountSent(TEXT("session.logout")),
    1
  );
  TestFalse(TEXT("The connection forgets the session"), H.HasSession());
  TestFalse(TEXT("Not logged in"), H.Client()->IsLoggedIn());
  TestFalse(TEXT("Not in the realm"), H.Client()->IsRealmConnected());
  TestEqual(
    TEXT("Nothing is reestablished"), H.Listener->ReestablishedCount, 0
  );
  TestEqual(TEXT("No authentication failure"), H.Listener->AuthFailedCount, 0);
  TestFalse(
    TEXT("Nothing waits to come back"),
    FRedwoodClientTestAccess::IsReauthPending(*H.Client())
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientLogoutConnectedTest,
  "Redwood.Bifrost.Client.LogoutWhileConnected",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientLogoutConnectedTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostClientTest;
  FClientHarness H(*this);
  H.EnterSessionWithCharacter();
  TCapture<FRedwoodListPlayersOutput> InFlight;
  H.Client()->ListFriends(ERedwoodFriendListType::All, InFlight.Delegate());

  H.Client()->Logout();
  const FPhoenixMessage Logout = H.Socket()->Last(TEXT("session.logout"));
  TestTrue(TEXT("The logout is a session step"), Logout.IsValid());
  TestFalse(TEXT("Not logged in"), H.Client()->IsLoggedIn());
  TestFalse(TEXT("Not in the realm"), H.Client()->IsRealmConnected());
  TestTrue(
    TEXT("No character is selected"), H.Client()->GetCharacterId().IsEmpty()
  );
  TestFalse(TEXT("The connection forgets the session"), H.HasSession());
  TestEqual(TEXT("A request that waited fails"), InFlight.Count, 1);
  TestEqual(
    TEXT("With the lost-reply error"),
    InFlight.Last.Error,
    FRedwoodPendingReplies::LostReplyError
  );

  // Bifrost ends the session and the channel; the socket stays.
  H.Socket()->CloseSession(TEXT("logout"), H.JoinRef);
  H.Socket()->EndChannel(H.JoinRef);
  TestEqual(
    TEXT("The end of the channel after a logout is not a lost connection"),
    H.Listener->LostCount,
    0
  );
  TestEqual(
    TEXT("Nor an authentication failure"), H.Listener->AuthFailedCount, 0
  );
  TestEqual(TEXT("Nor a reestablished one"), H.Listener->ReestablishedCount, 0);

  TCapture<FRedwoodListCharactersOutput> Later;
  H.Client()->ListCharacters(Later.Delegate());
  TestEqual(
    TEXT("A realm request after the logout fails at once"), Later.Count, 1
  );
  TestEqual(
    TEXT("With the not-connected error"), Later.Last.Error, NotConnectedToRealm
  );
  TCapture<FString> LaterDirector;
  H.Client()->RequestFriend(TEXT("p2"), LaterDirector.Delegate());
  TestEqual(
    TEXT("A director request after the logout fails at once"),
    LaterDirector.Count,
    1
  );
  TestEqual(
    TEXT("With the not-connected error"),
    LaterDirector.Last,
    NotConnectedToDirector
  );
  TestEqual(TEXT("Neither is held"), H.HeldCount(), 0);

  // The next login joins the same socket.
  H.LoginOk(TEXT("s2"), TEXT("rt2"));
  TestTrue(TEXT("Logged in again"), H.Client()->IsLoggedIn());
  TestEqual(TEXT("On the same socket"), H.Bifrost.Sockets.Num(), 1);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientReplacedTest,
  "Redwood.Bifrost.Client.ReplacedEndsSession",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientReplacedTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostClientTest;
  AddExpectedMessage(
    TEXT("The session cannot come back"), ELogVerbosity::Warning, Contains, 1
  );
  FClientHarness H(*this);
  H.EnterSessionWithCharacter();
  TCapture<FRedwoodListPlayersOutput> InFlight;
  H.Client()->ListFriends(ERedwoodFriendListType::All, InFlight.Delegate());

  // The same account logged in from another client.
  H.Socket()->CloseSession(TEXT("replaced"), H.JoinRef);
  TestEqual(
    TEXT("The game hears one authentication failure"),
    H.Listener->AuthFailedCount,
    1
  );
  TestEqual(
    TEXT("It says why"),
    H.Listener->LastAuthFailure,
    TEXT("This account logged in from another client.")
  );
  TestFalse(TEXT("Not logged in"), H.Client()->IsLoggedIn());
  TestFalse(TEXT("Not in the realm"), H.Client()->IsRealmConnected());
  TestEqual(TEXT("The waiting request fails"), InFlight.Count, 1);
  TestEqual(
    TEXT("With the lost-reply error"),
    InFlight.Last.Error,
    FRedwoodPendingReplies::LostReplyError
  );

  // Before the channel ends, the socket is still open: a request has no
  // session to ride all the same.
  TCapture<FRedwoodListCharactersOutput> RealmRequest;
  H.Client()->ListCharacters(RealmRequest.Delegate());
  TestEqual(TEXT("A realm request fails at once"), RealmRequest.Count, 1);
  TestEqual(
    TEXT("With the not-connected error"),
    RealmRequest.Last.Error,
    NotConnectedToRealm
  );
  TCapture<FString> DirectorRequest;
  H.Client()->RequestFriend(TEXT("p2"), DirectorRequest.Delegate());
  TestEqual(TEXT("A director request fails at once"), DirectorRequest.Count, 1);
  TestEqual(
    TEXT("With the not-connected error"),
    DirectorRequest.Last,
    NotConnectedToDirector
  );
  TestEqual(
    TEXT("Nothing went out"),
    H.Socket()->CountSent(TEXT("director:friends:add")),
    0
  );

  // The end of the channel that follows is Bifrost's, not a drop.
  H.Socket()->EndChannel(H.JoinRef);
  TestEqual(
    TEXT("The end of the channel is not a lost connection"),
    H.Listener->LostCount,
    0
  );
  TestEqual(TEXT("Nor a second failure"), H.Listener->AuthFailedCount, 1);
  TCapture<FString> LaterDirector;
  H.Client()->RequestFriend(TEXT("p3"), LaterDirector.Delegate());
  TestEqual(
    TEXT("A director request after it fails at once"), LaterDirector.Count, 1
  );
  TestEqual(
    TEXT("With the not-connected error"),
    LaterDirector.Last,
    NotConnectedToDirector
  );
  TestEqual(TEXT("Nothing is held"), H.HeldCount(), 0);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientClosedReconnectTest,
  "Redwood.Bifrost.Client.ClosedReconnectResumesAtOnce",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientClosedReconnectTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostClientTest;
  // No socket drops: the pod ends the channel, and the client logs that at
  // Log level. The loss itself is still the client's error.
  AddExpectedError(TEXT("Lost the connection to Bifrost"), Contains, 1);
  FClientHarness H(*this);
  H.EnterSessionWithCharacter();

  // The pod is stopping.
  H.Socket()->CloseSession(TEXT("reconnect"), H.JoinRef);
  TestEqual(
    TEXT("Nothing is lost before the channel ends"), H.Listener->LostCount, 0
  );
  H.Socket()->EndChannel(H.JoinRef);
  TestEqual(
    TEXT("The end of the channel is one lost connection"),
    H.Listener->LostCount,
    1
  );
  TestTrue(
    TEXT("The client closes the old socket"), H.Bifrost.Sockets[0]->bCloseCalled
  );
  TestTrue(
    TEXT("The session waits to come back"),
    FRedwoodClientTestAccess::IsReauthPending(*H.Client())
  );
  TestTrue(TEXT("The session is kept"), H.HasSession());
  TCapture<FString> Friend;
  H.Client()->RequestFriend(TEXT("p2"), Friend.Delegate());
  TestEqual(TEXT("A request in the gap is held"), H.HeldCount(), 1);
  TestEqual(
    TEXT("No new socket before the next tick"), H.Bifrost.Sockets.Num(), 1
  );

  // A sibling takes the resume, with no backoff.
  H.Advance(NextTickSeconds);
  if (!TestEqual(
        TEXT("A new socket on the next tick"), H.Bifrost.Sockets.Num(), 2
      )) {
    return false;
  }
  H.Accept();
  TestTrue(
    TEXT("The new socket resumes"), H.Socket()->Last(TEXT("phx_join")).IsValid()
  );
  H.ResumeOk();
  TestEqual(TEXT("Reestablished once"), H.Listener->ReestablishedCount, 1);
  TestEqual(TEXT("Lost once"), H.Listener->LostCount, 1);
  TestTrue(TEXT("Logged in"), H.Client()->IsLoggedIn());
  TestTrue(TEXT("In the realm"), H.Client()->IsRealmConnected());
  TestTrue(
    TEXT("The held request went out"),
    H.Socket()->Last(TEXT("director:friends:add")).IsValid()
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientFirstConnectFailsTest,
  "Redwood.Bifrost.Client.FirstConnectFails",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientFirstConnectFailsTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostClientTest;
  AddExpectedMessage(
    TEXT("Could not connect to Bifrost"), ELogVerbosity::Warning, Contains, 2
  );
  AddExpectedError(
    TEXT("Unable to establish the initial connection"), Contains, 1
  );
  FClientHarness H(*this);
  H.Connect();

  H.Socket()->FailToConnect();
  TestEqual(
    TEXT("A failed first connect is a lost connection"),
    H.Listener->LostCount,
    1
  );
  TestEqual(TEXT("The first connect is not reported"), H.ConnectedCalls, 0);

  H.Advance(PastFirstDelaySeconds);
  if (!TestEqual(TEXT("A second socket"), H.Bifrost.Sockets.Num(), 2)) {
    return false;
  }
  H.Socket()->FailToConnect();
  TestEqual(
    TEXT("A second failure is the same outage"), H.Listener->LostCount, 1
  );
  TestEqual(TEXT("Still not reported"), H.ConnectedCalls, 0);

  H.Advance(PastSecondDelaySeconds);
  if (!TestEqual(TEXT("A third socket"), H.Bifrost.Sockets.Num(), 3)) {
    return false;
  }
  H.Accept();
  TestEqual(TEXT("The first connect is reported once"), H.ConnectedCalls, 1);
  TestTrue(TEXT("With no error"), H.LastConnectError.IsEmpty());
  TestEqual(
    TEXT("And the connection is back"), H.Listener->ReestablishedCount, 1
  );
  TestTrue(TEXT("Connected"), H.Client()->IsDirectorConnected());
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientCharacterFriendsHeldTest,
  "Redwood.Bifrost.Client.CharacterFriendsHeld",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientCharacterFriendsHeldTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostClientTest;
  ExpectDrop(*this);
  FClientHarness H(*this);
  H.Open();
  H.LoginOk();
  H.EnterRealmOk();

  // In the realm, but no character is selected yet.
  TCapture<FRedwoodListCharacterFriendsOutput> NoCharacter;
  H.Client()->ListCharacterFriends(NoCharacter.Delegate());
  TestEqual(
    TEXT("A character friend call with no character fails at once"),
    NoCharacter.Count,
    1
  );
  TestEqual(
    TEXT("It says so"), NoCharacter.Last.Error, TEXT("No character selected.")
  );
  TestEqual(
    TEXT("Nothing went out"),
    H.Socket()->CountSent(TEXT("realm:contacts:list")),
    0
  );
  TestEqual(TEXT("It is not held"), H.HeldCount(), 0);

  H.SelectCharacterOk();
  H.Socket()->Drop();
  TCapture<FRedwoodListCharacterFriendsOutput> Held;
  H.Client()->ListCharacterFriends(Held.Delegate());
  TestEqual(
    TEXT("A character friend call in the gap is held"), H.HeldCount(), 1
  );
  TestEqual(TEXT("And not answered"), Held.Count, 0);

  H.Advance(PastFirstDelaySeconds);
  H.Accept();
  H.ResumeOk();
  const FPhoenixMessage List = H.Socket()->Last(TEXT("realm:contacts:list"));
  if (!TestTrue(
        TEXT("The held call goes out after the resume"), List.IsValid()
      )) {
    return false;
  }
  const TSharedPtr<FJsonObject> Payload =
    List.Payload->GetObjectField(TEXT("payload"));
  TestEqual(
    TEXT("For the selected character"),
    Payload->GetStringField(TEXT("characterId")),
    CharacterId
  );
  TestEqual(
    TEXT("For the player"), Payload->GetStringField(TEXT("playerId")), PlayerId
  );
  TestEqual(TEXT("Still not answered"), Held.Count, 0);
  TestEqual(TEXT("Nothing is held"), H.HeldCount(), 0);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientZoneAssignmentTest,
  "Redwood.Bifrost.Client.ZoneAssignmentOnce",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientZoneAssignmentTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostClientTest;
  AddExpectedMessage(
    TEXT("Ignoring a zone assignment"), ELogVerbosity::Warning, Contains, 1
  );
  FClientHarness H(*this);
  H.EnterSessionWithCharacter();
  TStrongObjectPtr<URedwoodJoinServerListener> Joins(
    NewObject<URedwoodJoinServerListener>()
  );
  Joins->Watch(H.Client());

  TSharedPtr<FJsonObject> Assignment = MakeShared<FJsonObject>();
  Assignment->SetStringField(TEXT("connection"), TEXT("10.0.0.1:7777"));
  Assignment->SetStringField(TEXT("token"), TEXT("server-token"));
  Assignment->SetBoolField(TEXT("shouldStitch"), false);
  Assignment->SetStringField(TEXT("characterId"), CharacterId);

  // At character select with no join out, a kept assignment is a replay.
  H.Socket()->Push(
    TEXT("zone-0"),
    TEXT("realm:servers:connect-to-instance"),
    Assignment,
    H.JoinRef
  );
  TestEqual(
    TEXT("An assignment with no join out does not move the player"),
    Joins->Count,
    0
  );

  TCapture<FRedwoodTicketingUpdate> Ticketing;
  H.Client()->JoinQueue(
    TEXT("proxy-1"), TEXT("L_Zone"), false, false, Ticketing.Delegate()
  );
  const FPhoenixMessage Join =
    H.Socket()->Last(TEXT("realm:ticketing:join:queue"));
  if (!TestTrue(TEXT("The join goes out"), Join.IsValid())) {
    return false;
  }
  H.Socket()->ReplyOk(Join, NoError());
  TestEqual(TEXT("The join is answered"), Ticketing.Count, 1);
  TestEqual(
    TEXT("As accepted"),
    Ticketing.Last.Type,
    ERedwoodTicketingUpdateType::JoinResponse
  );
  TestTrue(TEXT("With no error"), Ticketing.Last.Message.IsEmpty());

  H.Socket()->Push(
    TEXT("zone-1"),
    TEXT("realm:servers:connect-to-instance"),
    Assignment,
    H.JoinRef
  );
  TestEqual(
    TEXT("The assignment of the join moves the player"), Joins->Count, 1
  );
  TestTrue(
    TEXT("To the server"),
    Joins->LastConsoleCommand.Contains(TEXT("10.0.0.1:7777"))
  );
  TestTrue(
    TEXT("As the character"), Joins->LastConsoleCommand.Contains(CharacterId)
  );

  // The realm sends a kept push again after a resume.
  H.Socket()->Push(
    TEXT("zone-1"),
    TEXT("realm:servers:connect-to-instance"),
    Assignment,
    H.JoinRef
  );
  TestEqual(
    TEXT("The same assignment does not move the player again"), Joins->Count, 1
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientCharacterFriendAlertTest,
  "Redwood.Bifrost.Client.CharacterFriendAlert",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientCharacterFriendAlertTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostClientTest;
  AddExpectedMessage(
    TEXT("Dropped a director:friends:character-alert"),
    ELogVerbosity::Warning,
    Contains,
    1
  );
  FClientHarness H(*this);
  H.EnterSessionWithCharacter();
  TStrongObjectPtr<URedwoodCharacterFriendAlertListener> Alerts(
    NewObject<URedwoodCharacterFriendAlertListener>()
  );
  Alerts->Watch(H.Client());

  // The field names are the CharacterAlert schema of the RedwoodBackend fork
  // (packages/common/src/interfaces.ts).
  TSharedPtr<FJsonObject> Alert = MakeShared<FJsonObject>();
  Alert->SetStringField(TEXT("type"), TEXT("requested"));
  Alert->SetStringField(TEXT("characterId"), CharacterId);
  Alert->SetStringField(TEXT("otherCharacterId"), TEXT("char-2"));
  Alert->SetStringField(TEXT("otherCharacterName"), TEXT("Bravo"));
  H.Socket()->Push(
    TEXT("alert-1"), TEXT("director:friends:character-alert"), Alert, H.JoinRef
  );
  TestEqual(TEXT("The game hears the alert once"), Alerts->Count, 1);
  TestTrue(
    TEXT("As a friend request"),
    Alerts->Last.Type == ERedwoodCharacterFriendAlertType::Requested
  );
  // TestEqualSensitive: the string forms of TestEqual ignore case.
  TestEqualSensitive(
    TEXT("For the player's character"), *Alerts->Last.CharacterId, *CharacterId
  );
  TestEqualSensitive(
    TEXT("From the other character"),
    *Alerts->Last.OtherCharacterId,
    TEXT("char-2")
  );
  TestEqualSensitive(
    TEXT("With that character's name"),
    *Alerts->Last.OtherCharacterName,
    TEXT("Bravo")
  );
  TestEqual(TEXT("A request names no zone"), Alerts->Last.ZoneName, FString());

  // A request with no sender cannot be shown or answered. The director and
  // the game would then disagree on the fields, so the drop is logged.
  Alert->RemoveField(TEXT("otherCharacterId"));
  H.Socket()->Push(
    TEXT("alert-2"), TEXT("director:friends:character-alert"), Alert, H.JoinRef
  );
  TestEqual(
    TEXT("An alert with no sender does not reach the game"), Alerts->Count, 1
  );
  return true;
}
