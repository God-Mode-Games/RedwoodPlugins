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
#include "UObject/StrongObjectPtr.h"

#include "BifrostClientHarness.h"
#include "CharacterFriendAlertListener.h"
#include "ClientTestAccess.h"
#include "FakeBifrost.h"
#include "JoinServerListener.h"
#include "RedwoodClientInterface.h"
#include "RedwoodCommonGameSubsystem.h"
#include "RedwoodPendingReplies.h"

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

// The holes the reviews of the flag version found, on the real adapter:
//  13. A drop that takes the token re-login keeps the session: the next
//      socket logs in again with the same token, and the whole outage is one
//      lost connection and one reestablished one. A drop that takes the
//      realm re-entry keeps the new session: the next socket resumes it, and
//      Bifrost names the realm.
//  14. A login the player makes during the outage ends it: the join in
//      flight (resume or re-login) is replaced on the same socket, the old
//      session's held requests fail, and the game hears no reestablished
//      connection, because the player ended the outage.
//  15. A new connection while a login waits answers that login with the
//      not-connected error and does not send it again. A Deinitialize while
//      a login waits answers nobody: the callers are gone with the
//      subsystem, and the game may still call in after it.
//  16. A held call that acts for a character is not sent when the player
//      selected another character during the hold; one whose character did
//      not change goes out for it.
//  17. A push subscription of the game (OnPush) hears a push's payload, also
//      on a new connection, and not after it is removed. A push the client
//      handles itself reaches its handler and the subscribers.
//  18. A chat send before the realm fails at once. In the realm it goes out
//      with an id the client made and is answered with its seq; a refusal
//      gives the player a text of ours and the game the code; a whisper names
//      the player. A send made during a drop is held and goes out after the
//      resume, behind `chat.resume` with the cursors; the replay reaches the
//      game, and so does the topic whose replay was cut short.
//  19. `chat.topics` updates GetChatTopics and the delegate; a logout clears
//      them and the next session starts live, with no cursor of the last.

namespace RedwoodBifrostClientTest {
  const FString OtherPassword = TEXT("other-secret");

  // The phase by its name, so a failed check reads as a row of the machine.
  void TestPhase(
    FAutomationTestBase &Test,
    const TCHAR *What,
    const FClientHarness &H,
    ERedwoodSessionPhase Expected
  ) {
    Test.TestEqual(
      What,
      FRedwoodClientSession::PhaseName(
        FRedwoodClientTestAccess::Phase(*H.Client())
      ),
      FRedwoodClientSession::PhaseName(Expected)
    );
  }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientReloginLostToDropTest,
  "Redwood.Bifrost.Client.ReloginLostToDropRetries",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientReloginLostToDropTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostClientTest;
  // Two sockets go down inside one outage: the session's, and the one that
  // carried the re-login.
  AddExpectedMessage(TEXT("reconnecting"), ELogVerbosity::Warning, Contains, 2);
  AddExpectedError(TEXT("Lost the connection to Bifrost"), Contains, 1);
  ExpectSessionGone(*this);
  FClientHarness H(*this);
  H.EnterSessionWithCharacter();
  H.DropAndReopen();
  H.RefuseResume();
  const FPhoenixMessage Relogin = H.Socket()->Last(TEXT("phx_join"));
  if (!TestTrue(
        TEXT("The token re-login is in flight"),
        Relogin.IsValid() && Relogin.Payload->HasField(TEXT("login"))
      )) {
    return false;
  }

  // The socket goes before the re-login is answered.
  H.Socket()->Drop();
  TestEqual(TEXT("No authentication failure"), H.Listener->AuthFailedCount, 0);
  TestEqual(
    TEXT("The same outage: one lost connection"), H.Listener->LostCount, 1
  );
  TestPhase(
    *this,
    TEXT("The session waits for a socket to log in on"),
    H,
    ERedwoodSessionPhase::ReloginPending
  );
  TestFalse(TEXT("Not logged in"), H.Client()->IsLoggedIn());
  TestTrue(
    TEXT("The session is kept"),
    FRedwoodClientTestAccess::IsReauthPending(*H.Client())
  );

  // The next socket logs in again with the same token. This is the second
  // drop of the outage for the backoff, so it waits the second delay.
  H.Advance(PastSecondDelaySeconds);
  if (!TestEqual(
        TEXT("A third socket after the longer delay"),
        H.Bifrost.Sockets.Num(),
        3
      )) {
    return false;
  }
  H.Accept();
  const FPhoenixMessage Retry = H.Socket()->Last(TEXT("phx_join"));
  if (!TestTrue(
        TEXT("The new socket logs in again"),
        Retry.IsValid() && Retry.Payload->HasField(TEXT("login"))
      )) {
    return false;
  }
  TestEqual(
    TEXT("With the same token"),
    Retry.Payload->GetObjectField(TEXT("login"))
      ->GetStringField(TEXT("secret")),
    FirstToken
  );
  TestEqual(
    TEXT("And no resume: the old session is gone"),
    H.Socket()->CountSent(TEXT("phx_join")),
    1
  );
  TestEqual(
    TEXT("Nothing is reestablished before the answer"),
    H.Listener->ReestablishedCount,
    0
  );

  H.Socket()->Reply(
    Retry,
    TEXT("ok"),
    FFakeBifrost::JoinReply(
      TEXT("s2"),
      TEXT("rt9"),
      FFakeBifrost::LoginPlayer(PlayerId, TEXT("tok-2"))
    )
  );
  const FPhoenixMessage Enter = H.Socket()->Last(TEXT("session.enter-realm"));
  if (!TestTrue(
        TEXT("The new session enters the old realm"), Enter.IsValid()
      )) {
    return false;
  }
  TestEqual(
    TEXT("That realm"), Enter.Payload->GetStringField(TEXT("realmId")), RealmId
  );
  H.Socket()->ReplyOk(Enter, NoError());
  TestEqual(TEXT("Reestablished once"), H.Listener->ReestablishedCount, 1);
  TestEqual(TEXT("Lost once"), H.Listener->LostCount, 1);
  TestEqual(TEXT("No authentication failure"), H.Listener->AuthFailedCount, 0);
  TestTrue(TEXT("In the realm"), H.Client()->IsRealmConnected());
  const FPhoenixMessage Select =
    H.Socket()->Last(TEXT("session.select-character"));
  TestTrue(
    TEXT("The character is selected again"),
    Select.IsValid() &&
      Select.Payload->GetStringField(TEXT("characterId")) == CharacterId
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientReentryLostToDropTest,
  "Redwood.Bifrost.Client.ReentryLostToDropResumes",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientReentryLostToDropTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostClientTest;
  // Two sockets go down inside one outage: the session's, and the one that
  // carried the realm re-entry.
  AddExpectedMessage(TEXT("reconnecting"), ELogVerbosity::Warning, Contains, 2);
  AddExpectedError(TEXT("Lost the connection to Bifrost"), Contains, 1);
  ExpectSessionGone(*this);
  FClientHarness H(*this);
  H.EnterSessionWithCharacter();
  H.DropAndReopen();
  H.RefuseResume();

  // The re-login makes a new session; its entry of the old realm is in
  // flight.
  H.Socket()->Reply(
    H.Socket()->Last(TEXT("phx_join")),
    TEXT("ok"),
    FFakeBifrost::JoinReply(
      TEXT("s2"),
      TEXT("rt9"),
      FFakeBifrost::LoginPlayer(PlayerId, TEXT("tok-2"))
    )
  );
  if (!TestTrue(
        TEXT("The re-entry is in flight"),
        H.Socket()->Last(TEXT("session.enter-realm")).IsValid()
      )) {
    return false;
  }

  H.Socket()->Drop();
  TestEqual(TEXT("No authentication failure"), H.Listener->AuthFailedCount, 0);
  TestEqual(
    TEXT("The same outage: one lost connection"), H.Listener->LostCount, 1
  );
  TestEqual(
    TEXT("Nothing is reestablished"), H.Listener->ReestablishedCount, 0
  );
  TestPhase(
    *this,
    TEXT("The new session waits for a socket to resume on"),
    H,
    ERedwoodSessionPhase::Suspended
  );
  TestTrue(
    TEXT("The session is kept"),
    FRedwoodClientTestAccess::IsReauthPending(*H.Client())
  );

  // The next socket resumes the new session, with the token the re-login
  // got. The second drop of the outage waits the second delay.
  H.Advance(PastSecondDelaySeconds);
  if (!TestEqual(
        TEXT("A third socket after the longer delay"),
        H.Bifrost.Sockets.Num(),
        3
      )) {
    return false;
  }
  H.Accept();
  const FPhoenixMessage Resume = H.Socket()->Last(TEXT("phx_join"));
  if (!TestTrue(
        TEXT("The new socket resumes"),
        Resume.IsValid() && Resume.Payload->HasField(TEXT("resume"))
      )) {
    return false;
  }
  const TSharedPtr<FJsonObject> ResumePayload =
    Resume.Payload->GetObjectField(TEXT("resume"));
  TestEqual(
    TEXT("The new session"),
    ResumePayload->GetStringField(TEXT("sessionId")),
    TEXT("s2")
  );
  TestEqual(
    TEXT("With its token"),
    ResumePayload->GetStringField(TEXT("resumeToken")),
    TEXT("rt9")
  );
  TestEqual(
    TEXT("The token login is not sent again"),
    H.Socket()->CountSent(TEXT("phx_join")),
    1
  );

  // Bifrost names the realm the new session is in.
  H.Socket()->Reply(
    Resume,
    TEXT("ok"),
    FFakeBifrost::JoinReply(
      TEXT("s2"), TEXT("rt3"), ResumeIdentity(), RealmId, CharacterId
    )
  );
  TestEqual(TEXT("Reestablished once"), H.Listener->ReestablishedCount, 1);
  TestEqual(TEXT("Lost once"), H.Listener->LostCount, 1);
  TestEqual(TEXT("No authentication failure"), H.Listener->AuthFailedCount, 0);
  TestTrue(TEXT("In the realm"), H.Client()->IsRealmConnected());
  TestEqual(
    TEXT("The realm the session was in"), H.Client()->GetRealmId(), RealmId
  );
  TestEqual(
    TEXT("The realm is not entered again"),
    H.Socket()->CountSent(TEXT("session.enter-realm")),
    0
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientLoginDuringReloginTest,
  "Redwood.Bifrost.Client.LoginDuringReloginStartsClean",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientLoginDuringReloginTest::RunTest(
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
  H.RefuseResume();
  TestEqual(
    TEXT("The token re-login is in flight"),
    H.Socket()->CountSent(TEXT("phx_join")),
    2
  );
  TestEqual(TEXT("The held requests wait for it"), H.HeldCount(), 2);

  // The player logs in with a password while the token re-login waits. That
  // is a new session, not the old one coming back.
  TCapture<FRedwoodAuthUpdate> Caller;
  H.Client()->Login(
    PlayerId, OtherPassword, TEXT("local"), false, Caller.Delegate()
  );
  TestEqual(TEXT("No new socket"), H.Bifrost.Sockets.Num(), 2);
  TestFalse(TEXT("The socket stays open"), H.Socket()->bCloseCalled);
  TestEqual(TEXT("No drop is reported"), H.Listener->LostCount, 1);
  const FPhoenixMessage Join = H.Socket()->Last(TEXT("phx_join"));
  TestEqual(
    TEXT("The login's join replaces the re-login's on the same socket"),
    H.Socket()->CountSent(TEXT("phx_join")),
    3
  );
  TestTrue(
    TEXT("It carries the new credentials"),
    Join.IsValid() && Join.Payload->HasField(TEXT("login")) &&
      Join.Payload->GetObjectField(TEXT("login"))
          ->GetStringField(TEXT("secret")) == OtherPassword
  );
  TestEqual(
    TEXT("The old session's held director request fails"), Friend.Count, 1
  );
  TestEqual(
    TEXT("With the not-connected error"), Friend.Last, NotConnectedToDirector
  );
  TestEqual(
    TEXT("The old session's held realm request fails"), Invite.Count, 1
  );
  TestEqual(
    TEXT("With the not-connected error"), Invite.Last, NotConnectedToRealm
  );
  TestEqual(TEXT("Nothing is held"), H.HeldCount(), 0);
  TestEqual(TEXT("The caller waits for the join"), Caller.Count, 0);
  TestPhase(
    *this, TEXT("A login is in flight"), H, ERedwoodSessionPhase::LoggingIn
  );
  TestFalse(
    TEXT("No session is coming back"),
    FRedwoodClientTestAccess::IsReauthPending(*H.Client())
  );

  H.Socket()->Reply(
    Join,
    TEXT("ok"),
    FFakeBifrost::JoinReply(
      TEXT("s2"),
      TEXT("rt9"),
      FFakeBifrost::LoginPlayer(PlayerId, TEXT("tok-2"))
    )
  );
  TestEqual(TEXT("The caller is answered once"), Caller.Count, 1);
  TestEqual(
    TEXT("As a success"), Caller.Last.Type, ERedwoodAuthUpdateType::Success
  );
  TestPhase(
    *this, TEXT("Logged in, in no realm"), H, ERedwoodSessionPhase::LoggedIn
  );
  TestTrue(TEXT("Logged in"), H.Client()->IsLoggedIn());
  TestTrue(
    TEXT("The old realm is not the new session's"),
    H.Client()->GetRealmId().IsEmpty()
  );
  TestFalse(TEXT("Not in a realm"), H.Client()->IsRealmConnected());
  TestTrue(
    TEXT("Nor the old character"), H.Client()->GetCharacterId().IsEmpty()
  );
  // The player ended the outage with a login of their own: the game hears
  // the login's answer, not a connection that came back.
  TestEqual(
    TEXT("Nothing is reestablished"), H.Listener->ReestablishedCount, 0
  );
  TestEqual(TEXT("No authentication failure"), H.Listener->AuthFailedCount, 0);
  TestEqual(TEXT("Lost once"), H.Listener->LostCount, 1);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientLoginDuringResumeTest,
  "Redwood.Bifrost.Client.LoginDuringResumeReplacesTheJoin",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientLoginDuringResumeTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostClientTest;
  ExpectDrop(*this);
  FClientHarness H(*this);
  H.EnterSessionWithCharacter();
  H.DropAndReopen();
  const FPhoenixMessage Resume = H.Socket()->Last(TEXT("phx_join"));
  if (!TestTrue(
        TEXT("The resume is in flight"),
        Resume.IsValid() && Resume.Payload->HasField(TEXT("resume"))
      )) {
    return false;
  }

  // Another account logs in while the resume waits.
  TCapture<FRedwoodAuthUpdate> Caller;
  H.Client()->Login(
    TEXT("p2"), OtherPassword, TEXT("local"), false, Caller.Delegate()
  );
  TestEqual(TEXT("No new socket"), H.Bifrost.Sockets.Num(), 2);
  TestFalse(TEXT("The socket stays open"), H.Socket()->bCloseCalled);
  TestEqual(TEXT("No drop is reported"), H.Listener->LostCount, 1);
  if (!TestEqual(
        TEXT("The login's join follows the resume's"), H.Socket()->Sent.Num(), 2
      )) {
    return false;
  }
  const FPhoenixMessage Join = H.Socket()->Sent[1];
  TestTrue(
    TEXT("The second join is the login"),
    Join.IsValid() && Join.Event == TEXT("phx_join") &&
      Join.Payload->HasField(TEXT("login")) &&
      Join.Payload->GetObjectField(TEXT("login"))
          ->GetStringField(TEXT("username")) == TEXT("p2")
  );
  TestEqual(TEXT("The caller waits for the join"), Caller.Count, 0);
  TestPhase(
    *this, TEXT("A login is in flight"), H, ERedwoodSessionPhase::LoggingIn
  );
  TestFalse(TEXT("The connection forgets the old session"), H.HasSession());

  H.Socket()->Reply(
    Join,
    TEXT("ok"),
    FFakeBifrost::JoinReply(
      TEXT("s2"),
      TEXT("rt9"),
      FFakeBifrost::LoginPlayer(TEXT("p2"), TEXT("tok-2"))
    )
  );
  TestEqual(TEXT("The caller is answered once"), Caller.Count, 1);
  TestEqual(
    TEXT("As a success"), Caller.Last.Type, ERedwoodAuthUpdateType::Success
  );
  TestPhase(*this, TEXT("Logged in"), H, ERedwoodSessionPhase::LoggedIn);
  TestEqual(
    TEXT("As the account that logged in"), H.Client()->GetPlayerId(), TEXT("p2")
  );
  TestEqual(TEXT("Lost once"), H.Listener->LostCount, 1);
  TestEqual(
    TEXT("Nothing is reestablished"), H.Listener->ReestablishedCount, 0
  );
  TestEqual(TEXT("No authentication failure"), H.Listener->AuthFailedCount, 0);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientInitializeDuringLoginTest,
  "Redwood.Bifrost.Client.InitializeDuringLoginAnswersTheCaller",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientInitializeDuringLoginTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostClientTest;
  FClientHarness H(*this);
  H.Open();
  TCapture<FRedwoodAuthUpdate> Update;
  const FPhoenixMessage Join = H.SendLogin(Update);
  if (!TestTrue(
        TEXT("The login waits for its reply"),
        Join.IsValid() && Update.Count == 0
      )) {
    return false;
  }

  // The game connects again while the login waits.
  H.Connect();
  TestEqual(TEXT("The login is answered once"), Update.Count, 1);
  TestEqual(
    TEXT("As an error"), Update.Last.Type, ERedwoodAuthUpdateType::Error
  );
  TestEqual(
    TEXT("That the director is not connected"),
    Update.Last.Message,
    FRedwoodClientSession::NotConnectedToDirector
  );
  TestTrue(
    TEXT("The old socket is closed"), H.Bifrost.Sockets[0]->bCloseCalled
  );
  if (!TestEqual(TEXT("A new socket opens"), H.Bifrost.Sockets.Num(), 2)) {
    return false;
  }
  TestEqual(
    TEXT("The first connect is not reported before it connects"),
    H.ConnectedCalls,
    1
  );
  TestPhase(
    *this,
    TEXT("Back to the first connect"),
    H,
    ERedwoodSessionPhase::Connecting
  );

  H.Accept();
  TestEqual(TEXT("The new socket is the first connect"), H.ConnectedCalls, 2);
  TestTrue(TEXT("With no error"), H.LastConnectError.IsEmpty());
  TestPhase(
    *this, TEXT("At the title screen"), H, ERedwoodSessionPhase::Online
  );
  TestEqual(TEXT("The login is not sent again"), H.Socket()->Sent.Num(), 0);
  TestFalse(TEXT("Not logged in"), H.Client()->IsLoggedIn());
  // The game asked for the new connection: the machine's Initialize row goes
  // to Connecting with no word to the game, and Connecting to Online reports
  // only the first connect. Nothing was lost, so nothing comes back.
  TestEqual(TEXT("No lost connection"), H.Listener->LostCount, 0);
  TestEqual(
    TEXT("No reestablished connection"), H.Listener->ReestablishedCount, 0
  );
  TestEqual(TEXT("No authentication failure"), H.Listener->AuthFailedCount, 0);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientDeinitializeDuringLoginTest,
  "Redwood.Bifrost.Client.DeinitializeWithPendingJoinDropsTheCaller",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientDeinitializeDuringLoginTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostClientTest;
  FClientHarness H(*this);
  H.Open();
  TCapture<FRedwoodAuthUpdate> Update;
  const FPhoenixMessage Join = H.SendLogin(Update);
  if (!TestTrue(
        TEXT("The login waits for its reply"),
        Join.IsValid() && Update.Count == 0
      )) {
    return false;
  }

  // The subsystem goes away with the join in flight. Its callers go with
  // it: the login is dropped, not failed, and the client is as it was made.
  H.Client()->Deinitialize();
  TestEqual(TEXT("The caller is not answered"), Update.Count, 0);
  TestTrue(TEXT("The socket is closed"), H.Socket()->bCloseCalled);
  TestFalse(TEXT("No connection"), H.Client()->IsDirectorConnected());
  TestFalse(TEXT("Not logged in"), H.Client()->IsLoggedIn());
  TestPhase(
    *this,
    TEXT("The session is at its initial state"),
    H,
    ERedwoodSessionPhase::Connecting
  );

  // No timer is left behind: nothing opens a socket, nobody is answered.
  H.Advance(PastGraceSeconds);
  TestEqual(TEXT("No socket opens later"), H.Bifrost.Sockets.Num(), 1);
  TestEqual(TEXT("Nobody is answered later"), Update.Count, 0);

  // The game can still call in after the subsystem went down.
  H.Client()->SetSelectedCharacter(CharacterId);
  H.Client()->Logout();
  TCapture<FRedwoodAuthUpdate> Later;
  H.Client()->Login(PlayerId, Password, TEXT("local"), false, Later.Delegate());
  TestEqual(TEXT("A login after it fails at once"), Later.Count, 1);
  TestEqual(
    TEXT("With the not-connected error"),
    Later.Last.Message,
    FRedwoodClientSession::NotConnectedToDirector
  );
  TestEqual(TEXT("And opens no socket"), H.Bifrost.Sockets.Num(), 1);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientHeldCallForAnotherCharacterTest,
  "Redwood.Bifrost.Client.HeldCallForAnotherCharacterIsNotSent",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientHeldCallForAnotherCharacterTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostClientTest;
  // One drop in each of the two sessions below.
  ExpectDrop(*this, 2);

  // The player picks another character while the calls wait. The realm
  // accepts any character of the player, so a call sent now would act for
  // the wrong one: it is failed after the resume, and nothing goes out.
  {
    FClientHarness H(*this);
    H.EnterSessionWithCharacter();
    H.Socket()->Drop();
    TCapture<FString> Invite;
    H.Client()->InviteToParty(TEXT("p2"), Invite.Delegate());
    TCapture<FRedwoodListCharacterFriendsOutput> Friends;
    H.Client()->ListCharacterFriends(Friends.Delegate());
    TestEqual(TEXT("Both calls are held"), H.HeldCount(), 2);

    H.Client()->SetSelectedCharacter(OtherCharacterId);
    TestEqual(TEXT("The held calls still wait"), H.HeldCount(), 2);
    TestEqual(
      TEXT("Neither is answered before the session is back"),
      Invite.Count + Friends.Count,
      0
    );

    // Bifrost still has the old character: the selection never reached it.
    H.Advance(PastFirstDelaySeconds);
    H.Accept();
    H.ResumeOk(RealmId, CharacterId);
    const FPhoenixMessage Select =
      H.Socket()->Last(TEXT("session.select-character"));
    TestTrue(
      TEXT("The new character is selected for the realm"),
      Select.IsValid() &&
        Select.Payload->GetStringField(TEXT("characterId")) == OtherCharacterId
    );
    TestEqual(TEXT("The held invite is answered once"), Invite.Count, 1);
    TestEqual(
      TEXT("That the character changed"),
      Invite.Last,
      URedwoodCommonGameSubsystem::CharacterChangedError
    );
    TestEqual(
      TEXT("The invite did not go out"),
      H.Socket()->CountSent(TEXT("realm:parties:invites:initiate")),
      0
    );
    TestEqual(TEXT("The held friend list is answered once"), Friends.Count, 1);
    TestEqual(
      TEXT("That the character changed"),
      Friends.Last.Error,
      URedwoodCommonGameSubsystem::CharacterChangedError
    );
    TestEqual(
      TEXT("The friend list did not go out"),
      H.Socket()->CountSent(TEXT("realm:contacts:list")),
      0
    );
    TestEqual(TEXT("Nothing is held"), H.HeldCount(), 0);
    TestEqual(
      TEXT("The client keeps the new character"),
      H.Client()->GetCharacterId(),
      OtherCharacterId
    );
  }

  // The mirror: a held invite whose character did not change goes out for
  // it, and waits for the realm's answer.
  {
    FClientHarness H(*this);
    H.EnterSessionWithCharacter();
    H.Socket()->Drop();
    TCapture<FString> Invite;
    H.Client()->InviteToParty(TEXT("p2"), Invite.Delegate());
    H.Advance(PastFirstDelaySeconds);
    H.Accept();
    H.ResumeOk();
    const FPhoenixMessage Sent =
      H.Socket()->Last(TEXT("realm:parties:invites:initiate"));
    if (!TestTrue(TEXT("The held invite goes out"), Sent.IsValid())) {
      return false;
    }
    TestEqual(
      TEXT("For the character that made it"),
      Sent.Payload->GetObjectField(TEXT("payload"))
        ->GetStringField(TEXT("characterId")),
      CharacterId
    );
    TestEqual(TEXT("Its caller waits for the realm"), Invite.Count, 0);
  }
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientPushSubscriptionTest,
  "Redwood.Bifrost.Client.PushReachesSubscribers",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientPushSubscriptionTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostClientTest;
  FClientHarness H(*this);
  H.EnterSessionWithCharacter();

  // An event the client does not interpret: the game owns it.
  const FString BroadcastEvent = TEXT("admin:server-broadcast");
  int32 Broadcasts = 0;
  FString LastMessage;
  FRedwoodPushDelegate &OnBroadcast = H.Client()->OnPush(BroadcastEvent);
  const FDelegateHandle Handle = OnBroadcast.AddLambda(
    [&Broadcasts, &LastMessage](const TSharedPtr<FJsonObject> &Payload) {
      ++Broadcasts;
      LastMessage = Payload->GetStringField(TEXT("message"));
    }
  );
  TSharedPtr<FJsonObject> Broadcast = MakeShared<FJsonObject>();
  Broadcast->SetStringField(TEXT("message"), TEXT("Restart in five minutes."));
  H.Socket()->Push(TEXT("bc-1"), BroadcastEvent, Broadcast, H.JoinRef);
  TestEqual(TEXT("The subscriber hears the push once"), Broadcasts, 1);
  TestEqual(
    TEXT("With the push's payload"),
    LastMessage,
    TEXT("Restart in five minutes.")
  );

  // The game connects again: a new connection, and no re-bind.
  H.Open();
  H.LoginOk(TEXT("s2"), TEXT("rt-2"));
  H.Socket()->Push(TEXT("bc-2"), BroadcastEvent, Broadcast, H.JoinRef);
  TestEqual(
    TEXT("A push on the new connection reaches the subscriber"), Broadcasts, 2
  );

  OnBroadcast.Remove(Handle);
  H.Socket()->Push(TEXT("bc-3"), BroadcastEvent, Broadcast, H.JoinRef);
  TestEqual(TEXT("A removed subscriber hears nothing"), Broadcasts, 2);

  // An event the client handles itself: its handler keeps the party, and
  // the subscriber hears the same push after it.
  const FString PartyEvent = TEXT("realm:parties:changed");
  int32 PartyChanges = 0;
  FString LastPartyId;
  FRedwoodPushDelegate &OnPartyChanged = H.Client()->OnPush(PartyEvent);
  const FDelegateHandle PartyHandle = OnPartyChanged.AddLambda(
    [&PartyChanges, &LastPartyId](const TSharedPtr<FJsonObject> &Payload) {
      ++PartyChanges;
      LastPartyId =
        Payload->GetObjectField(TEXT("party"))->GetStringField(TEXT("id"));
    }
  );
  TSharedPtr<FJsonObject> Character = MakeShared<FJsonObject>();
  Character->SetStringField(TEXT("id"), CharacterId);
  Character->SetStringField(TEXT("name"), TEXT("Alpha"));
  TSharedPtr<FJsonObject> Member = MakeShared<FJsonObject>();
  Member->SetStringField(TEXT("playerId"), PlayerId);
  Member->SetStringField(TEXT("nickname"), TEXT("Nick"));
  Member->SetObjectField(TEXT("character"), Character);
  TSharedPtr<FJsonObject> Party = MakeShared<FJsonObject>();
  Party->SetStringField(TEXT("id"), TEXT("party-1"));
  Party->SetStringField(TEXT("lootType"), TEXT("free-for-all"));
  Party->SetStringField(TEXT("leaderId"), PlayerId);
  TArray<TSharedPtr<FJsonValue>> Members;
  Members.Add(MakeShared<FJsonValueObject>(Member));
  Party->SetArrayField(TEXT("members"), Members);
  TSharedPtr<FJsonObject> Changed = MakeShared<FJsonObject>();
  Changed->SetObjectField(TEXT("party"), Party);
  H.Socket()->Push(TEXT("party-1"), PartyEvent, Changed, H.JoinRef);
  TestEqual(
    TEXT("The client's own handler keeps the party"),
    H.Client()->GetCachedParty().Id,
    TEXT("party-1")
  );
  TestEqual(TEXT("The subscriber hears the same push"), PartyChanges, 1);
  TestEqual(TEXT("With its payload"), LastPartyId, TEXT("party-1"));
  OnPartyChanged.Remove(PartyHandle);

  // A subscriber that subscribes to new events from inside its callback: the
  // map grows under the broadcast, and the delegate it runs on must not move.
  constexpr int32 LateEvents = 64;
  int32 LateSubscriptions = 0;
  int32 Rounds = 0;
  TArray<TPair<FString, FDelegateHandle>> LateHandles;
  const FString FirstEvent = TEXT("admin:first");
  FRedwoodPushDelegate &OnFirst = H.Client()->OnPush(FirstEvent);
  const FDelegateHandle FirstHandle =
    OnFirst.AddLambda([&H, &LateSubscriptions, &Rounds, &LateHandles](
                        const TSharedPtr<FJsonObject> &
                      ) {
      ++Rounds;
      for (int32 Index = 0; Index < LateEvents; ++Index) {
        const FString Event = FString::Printf(TEXT("admin:late-%d"), Index);
        LateHandles.Emplace(
          Event,
          H.Client()->OnPush(Event).AddLambda([&LateSubscriptions](
                                                const TSharedPtr<FJsonObject> &
                                              ) { ++LateSubscriptions; })
        );
      }
    });
  H.Socket()->Push(TEXT("first-1"), FirstEvent, Broadcast, H.JoinRef);
  TestEqual(TEXT("The callback ran once"), Rounds, 1);
  H.Socket()->Push(TEXT("late-1"), TEXT("admin:late-7"), Broadcast, H.JoinRef);
  TestEqual(TEXT("A late subscription hears its push"), LateSubscriptions, 1);
  TestTrue(
    TEXT("The reference handed out before the growth is still the delegate"),
    &H.Client()->OnPush(FirstEvent) == &OnFirst
  );
  for (const TPair<FString, FDelegateHandle> &Late : LateHandles) {
    H.Client()->OnPush(Late.Key).Remove(Late.Value);
  }
  OnFirst.Remove(FirstHandle);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientChatSendTest,
  "Redwood.Bifrost.Client.ChatSendAnsweredAndHeldThroughDrop",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientChatSendTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostClientTest;
  ExpectDrop(*this);
  FClientHarness H(*this);
  const FString Party = TEXT("r:realm-1/party:party-1");

  // Chat needs the session in its realm, like a realm request.
  H.Open();
  H.LoginOk();
  TCapture<FRedwoodChatSendOutput> Early;
  H.Client()->SendChat(Party, TEXT("too soon"), Early.Delegate());
  TestEqual(
    TEXT("A send before the realm is answered at once"), Early.Count, 1
  );
  TestEqual(
    TEXT("As not connected to the realm"), Early.Last.Error, NotConnectedToRealm
  );
  TestEqual(
    TEXT("And nothing went out"), H.Socket()->CountSent(TEXT("chat.send")), 0
  );

  H.EnterRealmOk();
  H.SelectCharacterOk();
  TArray<FRedwoodChatMessage> Lines;
  H.Client()->OnChatMessage.AddLambda(
    [&Lines](const FRedwoodChatMessage &Line) { Lines.Add(Line); }
  );

  // A send in the realm, answered with its seq.
  TCapture<FRedwoodChatSendOutput> Sent;
  H.Client()->SendChat(Party, TEXT("hi all"), Sent.Delegate());
  const FPhoenixMessage Send = H.Socket()->Last(TEXT("chat.send"));
  if (!TestTrue(TEXT("The send goes out"), Send.IsValid())) {
    return false;
  }
  const FString MsgId = Send.Payload->GetStringField(TEXT("msgId"));
  TestFalse(TEXT("With an id the client made"), MsgId.IsEmpty());
  TestEqual(
    TEXT("To the topic"), Send.Payload->GetStringField(TEXT("topic")), Party
  );
  TestEqual(
    TEXT("With the body"),
    Send.Payload->GetStringField(TEXT("body")),
    TEXT("hi all")
  );
  TSharedPtr<FJsonObject> Posted = MakeShared<FJsonObject>();
  Posted->SetStringField(TEXT("msgId"), MsgId);
  Posted->SetStringField(TEXT("topic"), Party);
  Posted->SetNumberField(TEXT("seq"), 4);
  H.Socket()->Reply(Send, TEXT("ok"), Posted);
  TestEqual(TEXT("The send is answered once"), Sent.Count, 1);
  TestEqual(TEXT("As a success"), Sent.Last.Error, FString());
  TestEqual(TEXT("With the line's seq"), Sent.Last.Seq, static_cast<int64>(4));
  TestEqual(TEXT("And its topic"), Sent.Last.Topic, Party);

  // A line arrives: the session has a cursor to resume from.
  H.Socket()->ChatMessage(
    FFakeBifrost::ChatLine(Party, 5, TEXT("m5")), H.JoinRef
  );
  TestEqual(TEXT("A live line reaches the game"), Lines.Num(), 1);

  // A refusal: the player reads a text of ours, the game gets the code.
  TCapture<FRedwoodChatSendOutput> Muted;
  H.Client()->SendChat(Party, TEXT("again"), Muted.Delegate());
  H.Socket()->Refuse(
    H.Socket()->Last(TEXT("chat.send")), TEXT("muted"), TEXT("muted by staff")
  );
  TestEqual(TEXT("The refusal is answered"), Muted.Count, 1);
  TestEqual(TEXT("With the code"), Muted.Last.Code, TEXT("muted"));
  TestEqual(
    TEXT("And the player's text for it"),
    Muted.Last.Error,
    URedwoodClientInterface::ChatErrorText(TEXT("muted"), FString())
  );

  // A whisper names the player, not a topic.
  TCapture<FRedwoodChatSendOutput> Whisper;
  H.Client()->SendWhisper(TEXT("p2"), TEXT("psst"), Whisper.Delegate());
  const FPhoenixMessage Whispered = H.Socket()->Last(TEXT("chat.send"));
  TestEqual(
    TEXT("A whisper carries the target player"),
    Whispered.Payload->GetStringField(TEXT("to")),
    TEXT("p2")
  );
  TestFalse(TEXT("And no topic"), Whispered.Payload->HasField(TEXT("topic")));

  // The drop. The whisper that waited fails; a send made in the gap is held.
  H.Socket()->Drop();
  TestEqual(TEXT("The waiting whisper fails"), Whisper.Count, 1);
  TestEqual(
    TEXT("With the lost-reply error"),
    Whisper.Last.Error,
    FRedwoodPendingReplies::LostReplyError
  );
  TCapture<FRedwoodChatSendOutput> Held;
  H.Client()->SendChat(Party, TEXT("while down"), Held.Delegate());
  TestEqual(TEXT("A send in the gap is held"), H.HeldCount(), 1);
  TestEqual(TEXT("And not answered"), Held.Count, 0);

  // The session comes back: the chat is resumed first, from the cursor, and
  // the held send follows it.
  TArray<FString> Truncated;
  H.Client()->OnChatTruncated.AddLambda(
    [&Truncated](const TArray<FString> &In) { Truncated = In; }
  );
  H.Advance(PastFirstDelaySeconds);
  H.Accept();
  H.ResumeOk();
  const FPhoenixMessage Resume = H.Socket()->Last(TEXT("chat.resume"));
  if (!TestTrue(TEXT("The resume goes out"), Resume.IsValid())) {
    return false;
  }
  TestEqual(
    TEXT("With the topic's cursor"),
    static_cast<int32>(
      Resume.Payload->GetObjectField(TEXT("cursors"))->GetNumberField(Party)
    ),
    5
  );
  const FPhoenixMessage HeldSend = H.Socket()->Last(TEXT("chat.send"));
  if (!TestTrue(TEXT("The held send goes out"), HeldSend.IsValid())) {
    return false;
  }
  TestTrue(
    TEXT("After the resume"),
    IndexOfLast(*H.Socket(), TEXT("chat.resume")) <
      IndexOfLast(*H.Socket(), TEXT("chat.send"))
  );
  TestEqual(
    TEXT("With its body"),
    HeldSend.Payload->GetStringField(TEXT("body")),
    TEXT("while down")
  );
  TestEqual(TEXT("Nothing is held"), H.HeldCount(), 0);

  // The replay repairs the gap, and the game hears what was cut short.
  TSharedPtr<FJsonObject> Topics = MakeShared<FJsonObject>();
  Topics->SetObjectField(
    Party,
    FFakeBifrost::ResumedTopic(
      {FFakeBifrost::ChatLine(Party, 6, TEXT("m6"))}, true
    )
  );
  TSharedPtr<FJsonObject> ResumeReply = MakeShared<FJsonObject>();
  ResumeReply->SetObjectField(TEXT("topics"), Topics);
  H.Socket()->Reply(Resume, TEXT("ok"), ResumeReply);
  if (TestEqual(TEXT("The replayed line reaches the game"), Lines.Num(), 2)) {
    TestEqual(TEXT("After the gap"), Lines[1].Seq, static_cast<int64>(6));
  }
  TestEqual(TEXT("The cut topic is reported"), Truncated.Num(), 1);
  TestTrue(TEXT("By name"), Truncated.Contains(Party));

  // The held send is answered when Bifrost answers it.
  Posted->SetStringField(
    TEXT("msgId"), HeldSend.Payload->GetStringField(TEXT("msgId"))
  );
  Posted->SetNumberField(TEXT("seq"), 7);
  H.Socket()->Reply(HeldSend, TEXT("ok"), Posted);
  TestEqual(TEXT("The held send is answered once"), Held.Count, 1);
  TestEqual(TEXT("With its seq"), Held.Last.Seq, static_cast<int64>(7));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClientChatTopicsTest,
  "Redwood.Bifrost.Client.ChatTopicsFollowTheSession",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClientChatTopicsTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostClientTest;
  FClientHarness H(*this);
  H.EnterSessionWithCharacter();
  const FString Party = TEXT("r:realm-1/party:party-1");
  const FString Guild = TEXT("guild:guild-1");

  int32 Updates = 0;
  H.Client()->OnChatTopics.AddLambda([&Updates](const TArray<FString> &) {
    ++Updates;
  });
  TArray<FRedwoodChatMessage> Lines;
  H.Client()->OnChatMessage.AddLambda(
    [&Lines](const FRedwoodChatMessage &Line) { Lines.Add(Line); }
  );
  TestEqual(
    TEXT("No topics before Bifrost names them"),
    H.Client()->GetChatTopics().Num(),
    0
  );

  H.Socket()->ChatTopics({Party, Guild}, H.JoinRef);
  TestEqual(TEXT("The game hears the topics"), Updates, 1);
  TestEqual(TEXT("All of them"), H.Client()->GetChatTopics().Num(), 2);
  TestTrue(
    TEXT("Including the guild's"), H.Client()->GetChatTopics().Contains(Guild)
  );

  H.Socket()->ChatMessage(
    FFakeBifrost::ChatLine(Party, 1, TEXT("m1")), H.JoinRef
  );
  TestEqual(TEXT("A line reaches the game"), Lines.Num(), 1);

  // The session ends: its topics go, and the game hears that too.
  H.Client()->Logout();
  TestEqual(
    TEXT("The logout clears the topics"), H.Client()->GetChatTopics().Num(), 0
  );
  TestEqual(TEXT("And the game hears it"), Updates, 2);

  // The next session starts live: a line with the last session's topic and
  // seq is new to it.
  H.LoginOk(TEXT("s2"), TEXT("rt2"));
  H.EnterRealmOk();
  H.Socket()->ChatMessage(
    FFakeBifrost::ChatLine(Party, 1, TEXT("m1")), H.JoinRef
  );
  TestEqual(TEXT("The new session's line is delivered"), Lines.Num(), 2);
  return true;
}
