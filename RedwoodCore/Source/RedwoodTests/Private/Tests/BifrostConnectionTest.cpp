// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins FRedwoodBifrostConnection against the client protocol of
// HollowedOath-Services services/bifrost/contract/protocol.md, with the fake
// of FakeBifrost.h as the server:
//   1. A login is a phx_join of `session`; a request is `rpc` on that join;
//      a reply is read back as the one-element Redwood response, and a
//      refusal by Bifrost as an object with the error and its code.
//   2. A request before the join, or after a refused one, goes nowhere.
//   3. A drop keeps the session: the socket comes back after the backoff and
//      resumes it, with the newest token. A request that waited on the old
//      socket is not answered here (the owner tracks it); a join that waited
//      is refused with the lost-reply error.
//   4. `resume_refused` and `invalid_request` end the session; any other
//      refusal of a resume is the pod's trouble, and the client tries
//      another socket. A resume join that went with its socket, or that a
//      newer join replaced, leaves the socket alone.
//   5. A push is delivered once for each push id.
//   6. `closed` is followed by the end of the channel (phx_close), not a
//      drop. After `replaced` the socket stays for the next login; after
//      `reconnect` a new socket is opened at once, with no backoff, and the
//      session is resumed on it. A socket Bifrost closes after `closed` is
//      not a drop either; the owner only hears that the socket went.
//   7. A heartbeat goes every 30 s; one with no answer by the next closes the
//      socket with the client's own code and reconnects.
//   8. Close() stops every reconnect. A message of a replaced join is
//      ignored. A connect that fails backs off like a drop. A channel error
//      (phx_error) reconnects and keeps the session.
//   9. Session steps go one at a time: the next one goes out when the reply
//      to the one in flight arrives, and a drop answers the one in flight and
//      the queued ones with the lost-reply error.
//  10. A join sent while one waits replaces it: the old join hears that, and
//      only the new join's reply makes the session. A reply with no join
//      pending changes nothing. A join also ends the channel it replaces: a
//      step that waited on it is answered as lost, the next step of the new
//      channel goes out, and a `closed` of the old channel is forgotten.
//  11. A chat line (`chat.msg`) is delivered once and moves its topic's
//      cursor; a line at or before the cursor, or with a seen msgId, is
//      dropped; a line of a new topic starts its cursor. `chat.resume` sends
//      the cursors, and the reply's lines are delivered oldest first through
//      the same check, with the truncated topics readable from the reply; a
//      live line that arrives while the resume waits comes after the replay.
//      `chat.send` is answered with its seq, a whisper carries `to` and no
//      topic, and `chat.topics` reaches its callback.
//  12. A resume ended by a reset (the session ended) or a drop is stale: its
//      reply moves no cursor and delivers nothing. A line or a topic list
//      that arrives with no session is dropped.
//  13. A drop with a resume in flight delivers the lines held for it; the
//      next resume asks from the moved cursor.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "TimerManager.h"

#include "FakeBifrost.h"
#include "RedwoodBifrostConnection.h"

namespace RedwoodBifrostConnectionTest {
  using namespace RedwoodFakeBifrost;

  const FString BaseUri = TEXT("ws://bifrost.test/");

  // Just over the heartbeat interval.
  constexpr float PastHeartbeatSeconds =
    FRedwoodBifrostConnection::HeartbeatSeconds + 0.1f;
  // Just over the time a connection must stay up for the backoff to start
  // again from its first delay.
  constexpr float PastStableSeconds =
    FRedwoodCloseBackoff::StableConnectionSeconds + 0.1f;

  struct FHarness {
    FAutomationTestBase &Test;
    FFrameCounterGuard FrameCounter;
    FTimerManager Timers;
    FFakeBifrost Bifrost;
    TSharedPtr<FRedwoodBifrostConnection> Conn;

    int32 Connected = 0;
    int32 Dropped = 0;
    // Sockets Bifrost closed after `closed`: no loss, only the socket going.
    int32 QuietDrops = 0;
    int32 Resumed = 0;
    int32 ResumeRefused = 0;
    TSharedPtr<FJsonObject> LastResumeReply;
    TArray<FString> ServerCloses;

    explicit FHarness(FAutomationTestBase &InTest) : Test(InTest) {
      // A timer set in a frame the manager has not ticked yet waits in a
      // pending set until the end of the next tick, and would fire one step
      // late. The empty tick lines every later timer up with the steps.
      Advance(0.0f);
      Conn = MakeShared<FRedwoodBifrostConnection>(Timers);
      Conn->OnConnected = [this]() { ++Connected; };
      Conn->OnDropped = [this](bool bAnnounced) {
        if (bAnnounced) {
          ++Dropped;
        } else {
          ++QuietDrops;
        }
      };
      Conn->OnResumed = [this](const TSharedPtr<FJsonObject> &Reply) {
        ++Resumed;
        LastResumeReply = Reply;
      };
      Conn->OnResumeRefused = [this]() { ++ResumeRefused; };
      Conn->OnClosedByServer = [this](const FString &Reason) {
        ServerCloses.Add(Reason);
      };
    }

    ~FHarness() {
      Conn.Reset();
    }

    TSharedPtr<FFakeBifrostSocket> Socket() const {
      return Bifrost.Socket();
    }

    void Advance(float Seconds) {
      TickTimers(Timers, Seconds);
    }

    // The server accepts the client's newest socket.
    void Accept() {
      Test.TestTrue(
        TEXT("The server can open the client's newest socket"), Bifrost.Open()
      );
    }

    // Connects, and the server accepts.
    void Open() {
      Conn->Connect(BaseUri);
      Accept();
    }

    static TSharedPtr<FJsonObject> LoginPayload() {
      TSharedPtr<FJsonObject> Login = MakeShared<FJsonObject>();
      Login->SetStringField(TEXT("provider"), TEXT("local"));
      Login->SetStringField(TEXT("username"), TEXT("p1"));
      Login->SetStringField(TEXT("secret"), TEXT("secret"));
      return Login;
    }

    // Sends a login and returns its join message, unanswered.
    FPhoenixMessage SendLogin(
      bool *OutOk = nullptr, TSharedPtr<FJsonObject> *OutResponse = nullptr
    ) {
      Conn->Login(
        LoginPayload(),
        [OutOk,
         OutResponse](bool bOk, const TSharedPtr<FJsonObject> &Response) {
          if (OutOk) {
            *OutOk = bOk;
          }
          if (OutResponse) {
            *OutResponse = Response;
          }
        }
      );
      return Socket()->Last(TEXT("phx_join"));
    }

    // Logs in and the server accepts: the session is on the socket.
    FPhoenixMessage LoginOk(
      const FString &SessionId = TEXT("s1"), const FString &Token = TEXT("t1")
    ) {
      const FPhoenixMessage Join = SendLogin();
      Socket()->Reply(
        Join,
        TEXT("ok"),
        FFakeBifrost::JoinReply(
          SessionId, Token, FFakeBifrost::LoginPlayer(TEXT("p1"), TEXT("tok"))
        )
      );
      return Join;
    }

    // The connection stayed up long enough to count as healthy, so the
    // next drop waits only the first delay.
    void Settle() {
      Advance(PastStableSeconds);
    }

    // Lets the socket drop and the backoff bring a new one, open.
    void DropAndReopen(float PastDelaySeconds = PastFirstDelaySeconds) {
      Socket()->Drop();
      Advance(PastDelaySeconds);
      Accept();
    }
  };

  // The one element the request handlers read.
  TSharedPtr<FJsonObject> ResponseObject(
    const TArray<TSharedPtr<FJsonValue>> &Response
  ) {
    return Response.Num() == 1 && Response[0].IsValid()
      ? Response[0]->AsObject()
      : nullptr;
  }

  // A reply callback that keeps the Redwood object it was given.
  FRedwoodReplyCallback Keep(TSharedPtr<FJsonObject> &Answer) {
    return [&Answer](const TArray<TSharedPtr<FJsonValue>> &Response) {
      Answer = ResponseObject(Response);
    };
  }

  bool IsLostResponse(const TSharedPtr<FJsonObject> &Answer) {
    return Answer.IsValid() &&
      Answer->GetStringField(TEXT("error")) ==
      FRedwoodPendingReplies::LostReplyError &&
      Answer->GetStringField(TEXT("code")) == TEXT("not_connected");
  }

  // The reconnect and heartbeat paths log warnings on purpose.
  void ExpectReconnectWarnings(FAutomationTestBase &Test) {
    Test.AddExpectedMessage(
      TEXT("reconnecting"), ELogVerbosity::Warning, Contains, 0
    );
  }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostJoinAndRpcTest,
  "Redwood.Bifrost.Connection.JoinAndRpc",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostJoinAndRpcTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostConnectionTest;
  FHarness H(*this);
  H.Open();

  TestEqual(
    TEXT("The socket goes to the Phoenix transport of the base URI"),
    H.Socket()->Uri,
    TEXT("ws://bifrost.test/socket/websocket?vsn=2.0.0")
  );
  TestTrue(TEXT("Connect was called"), H.Socket()->bConnectCalled);
  TestEqual(TEXT("The owner hears the connect"), H.Connected, 1);
  TestFalse(TEXT("No session before the login"), H.Conn->HasSession());

  bool bLoginOk = false;
  TSharedPtr<FJsonObject> LoginResponse;
  const FPhoenixMessage Join = H.SendLogin(&bLoginOk, &LoginResponse);
  if (!TestTrue(TEXT("A login sends a join"), Join.IsValid())) {
    return false;
  }
  TestEqual(
    TEXT("The join is of the session topic"), Join.Topic, TEXT("session")
  );
  TestEqual(
    TEXT("The join carries its own ref as join_ref"), Join.JoinRef, Join.Ref
  );
  TestEqual(
    TEXT("The join names the contract version"),
    static_cast<int32>(Join.Payload->GetNumberField(TEXT("v"))),
    FRedwoodBifrostConnection::ContractVersion
  );
  TestEqual(
    TEXT("The join carries the login"),
    Join.Payload->GetObjectField(TEXT("login"))
      ->GetStringField(TEXT("username")),
    TEXT("p1")
  );

  H.Socket()->Reply(
    Join,
    TEXT("ok"),
    FFakeBifrost::JoinReply(
      TEXT("s1"), TEXT("t1"), FFakeBifrost::LoginPlayer(TEXT("p1"), TEXT("tok"))
    )
  );
  TestTrue(TEXT("The login callback hears ok"), bLoginOk);
  TestTrue(TEXT("The session is on the socket"), H.Conn->HasSession());
  TestEqual(
    TEXT("The session id is taken"), H.Conn->GetSessionId(), TEXT("s1")
  );
  TestEqual(
    TEXT("The callback gets the join reply"),
    LoginResponse->GetObjectField(TEXT("player"))
      ->GetStringField(TEXT("token")),
    TEXT("tok")
  );

  // A request, answered by Redwood.
  TSharedPtr<FJsonObject> Answer;
  TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
  Payload->SetStringField(TEXT("filter"), TEXT("all"));
  H.Conn->Rpc(TEXT("director:friends:list"), Payload, Keep(Answer));
  const FPhoenixMessage Rpc = H.Socket()->Last(TEXT("director:friends:list"));
  if (!TestTrue(TEXT("The request goes out as rpc"), Rpc.IsValid())) {
    return false;
  }
  TestEqual(TEXT("The request is on the join"), Rpc.JoinRef, Join.Ref);
  TestEqual(TEXT("The request is an rpc"), Rpc.Event, TEXT("rpc"));
  TestEqual(
    TEXT("The rpc carries the Redwood payload"),
    Rpc.Payload->GetObjectField(TEXT("payload"))
      ->GetStringField(TEXT("filter")),
    TEXT("all")
  );
  TestNotEqual(TEXT("Each message has its own ref"), Rpc.Ref, Join.Ref);

  TSharedPtr<FJsonObject> Friends = MakeShared<FJsonObject>();
  Friends->SetStringField(TEXT("error"), TEXT(""));
  Friends->SetNumberField(TEXT("count"), 2);
  H.Socket()->ReplyOk(Rpc, Friends);
  if (TestNotNull(TEXT("The reply reaches the callback"), Answer.Get())) {
    TestEqual(
      TEXT("The Redwood object is the response"),
      Answer->GetStringField(TEXT("error")),
      TEXT("")
    );
    TestEqual(
      TEXT("Its fields come through"),
      static_cast<int32>(Answer->GetNumberField(TEXT("count"))),
      2
    );
  }

  // A request Bifrost refuses.
  Answer.Reset();
  H.Conn->Rpc(TEXT("realm:characters:list"), nullptr, Keep(Answer));
  H.Socket()->Refuse(
    H.Socket()->Last(TEXT("realm:characters:list")),
    TEXT("no_realm"),
    TEXT("Enter a realm first.")
  );
  if (TestNotNull(TEXT("The refusal reaches the callback"), Answer.Get())) {
    TestEqual(
      TEXT("The refusal's message is the error"),
      Answer->GetStringField(TEXT("error")),
      TEXT("Enter a realm first.")
    );
    TestEqual(
      TEXT("The refusal's code comes along"),
      Answer->GetStringField(TEXT("code")),
      TEXT("no_realm")
    );
  }

  // An event that sends no reply.
  Answer.Reset();
  H.Conn->Rpc(TEXT("realm:parties:emote"), nullptr, Keep(Answer));
  H.Socket()->ReplyOk(H.Socket()->Last(TEXT("realm:parties:emote")), nullptr);
  if (TestNotNull(TEXT("A null response still answers"), Answer.Get())) {
    TestEqual(
      TEXT("A null response reads as success"),
      Answer->GetStringField(TEXT("error")),
      TEXT("")
    );
  }

  // A session step is its own event on the channel.
  H.Conn->Step(TEXT("session.logout"), nullptr);
  const FPhoenixMessage Logout = H.Socket()->Last(TEXT("session.logout"));
  TestTrue(
    TEXT("A step goes out under its own event"),
    Logout.IsValid() && Logout.Event == TEXT("session.logout")
  );
  H.Conn->ForgetSession();
  TestFalse(TEXT("A forgotten session is gone"), H.Conn->HasSession());
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostRequestNeedsJoinTest,
  "Redwood.Bifrost.Connection.RequestNeedsJoin",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostRequestNeedsJoinTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostConnectionTest;
  AddExpectedMessage(
    TEXT("Dropped a rpc request"), ELogVerbosity::Warning, Contains, 2
  );
  FHarness H(*this);
  H.Open();

  bool bAnswered = false;
  H.Conn->Rpc(TEXT("realm:list"), nullptr, [&bAnswered](auto) {
    bAnswered = true;
  });
  TestEqual(
    TEXT("A request before the join sends nothing"), H.Socket()->Sent.Num(), 0
  );
  TestFalse(TEXT("It is not answered either"), bAnswered);

  // A refused join leaves no channel.
  bool bLoginOk = true;
  const FPhoenixMessage Join = H.SendLogin(&bLoginOk);
  H.Socket()->RefuseJoin(Join, TEXT("login_failed"), TEXT("Wrong password"));
  TestFalse(TEXT("The login callback hears the refusal"), bLoginOk);
  TestFalse(TEXT("No session after a refused join"), H.Conn->HasSession());
  const int32 SentBefore = H.Socket()->Sent.Num();
  H.Conn->Rpc(TEXT("realm:list"), nullptr);
  TestEqual(
    TEXT("A request after a refused join sends nothing"),
    H.Socket()->Sent.Num(),
    SentBefore
  );

  // The next login opens the channel again.
  H.LoginOk();
  H.Conn->Rpc(TEXT("realm:list"), nullptr);
  TestTrue(
    TEXT("A request after the next login goes out"),
    H.Socket()->Last(TEXT("realm:list")).IsValid()
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostDropResumesTest,
  "Redwood.Bifrost.Connection.DropReconnectsAndResumes",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostDropResumesTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostConnectionTest;
  ExpectReconnectWarnings(*this);
  FHarness H(*this);
  H.Open();
  const FPhoenixMessage Join = H.LoginOk(TEXT("s1"), TEXT("t1"));

  TSharedPtr<FJsonObject> LostAnswer;
  H.Conn->Rpc(TEXT("realm:list"), nullptr, Keep(LostAnswer));

  H.Socket()->Drop();
  TestEqual(TEXT("The owner hears the drop"), H.Dropped, 1);
  TestFalse(
    TEXT("The waiting request is not answered here: the owner tracks it"),
    LostAnswer.IsValid()
  );
  TestTrue(TEXT("The session is kept through the drop"), H.Conn->HasSession());
  TestTrue(
    TEXT("A reconnect is scheduled"),
    H.Conn->GetBackoff().IsReconnectPending(H.Timers)
  );
  TestEqual(TEXT("No new socket before the delay"), H.Bifrost.Sockets.Num(), 1);

  H.Advance(PastFirstDelaySeconds);
  if (!TestEqual(
        TEXT("A new socket after the delay"), H.Bifrost.Sockets.Num(), 2
      )) {
    return false;
  }
  TestEqual(
    TEXT("It goes to the same place"),
    H.Socket()->Uri,
    H.Bifrost.Sockets[0]->Uri
  );
  H.Accept();
  TestEqual(TEXT("The owner hears the connect again"), H.Connected, 2);

  const FPhoenixMessage Resume = H.Socket()->Last(TEXT("phx_join"));
  if (!TestTrue(TEXT("The new socket resumes the session"), Resume.IsValid())) {
    return false;
  }
  const TSharedPtr<FJsonObject> ResumePayload =
    Resume.Payload->GetObjectField(TEXT("resume"));
  TestEqual(
    TEXT("The resume names the session"),
    ResumePayload->GetStringField(TEXT("sessionId")),
    TEXT("s1")
  );
  TestEqual(
    TEXT("The resume carries the token"),
    ResumePayload->GetStringField(TEXT("resumeToken")),
    TEXT("t1")
  );
  TestNotEqual(TEXT("The resume is a new join"), Resume.Ref, Join.Ref);
  TestEqual(TEXT("Nothing is resumed before the answer"), H.Resumed, 0);

  TSharedPtr<FJsonObject> Identity = MakeShared<FJsonObject>();
  Identity->SetStringField(TEXT("playerId"), TEXT("p1"));
  H.Socket()->Reply(
    Resume,
    TEXT("ok"),
    FFakeBifrost::JoinReply(
      TEXT("s1"), TEXT("t2"), Identity, TEXT("realm-1"), TEXT("char-1")
    )
  );
  TestEqual(TEXT("The owner hears the resume"), H.Resumed, 1);
  TestEqual(
    TEXT("The owner gets the reply"),
    H.LastResumeReply->GetStringField(TEXT("realmId")),
    TEXT("realm-1")
  );
  TestEqual(TEXT("The session id stays"), H.Conn->GetSessionId(), TEXT("s1"));

  // A request after the resume rides the new join.
  H.Conn->Rpc(TEXT("realm:list"), nullptr);
  TestEqual(
    TEXT("A new request is on the new join"),
    H.Socket()->Last(TEXT("realm:list")).JoinRef,
    Resume.Ref
  );

  // The next resume uses the rotated token.
  H.DropAndReopen(PastSecondDelaySeconds);
  const FPhoenixMessage SecondResume = H.Socket()->Last(TEXT("phx_join"));
  if (TestTrue(TEXT("The third socket resumes too"), SecondResume.IsValid())) {
    TestEqual(
      TEXT("The newest token is sent"),
      SecondResume.Payload->GetObjectField(TEXT("resume"))
        ->GetStringField(TEXT("resumeToken")),
      TEXT("t2")
    );
  }
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostResumeRefusedTest,
  "Redwood.Bifrost.Connection.ResumeRefused",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostResumeRefusedTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostConnectionTest;
  ExpectReconnectWarnings(*this);
  AddExpectedMessage(
    TEXT("session is gone"), ELogVerbosity::Warning, Contains, 1
  );
  AddExpectedMessage(
    TEXT("Dropped a rpc request"), ELogVerbosity::Warning, Contains, 1
  );
  FHarness H(*this);
  H.Open();
  H.LoginOk();
  H.DropAndReopen();

  // The session's grace ran out.
  H.Socket()->RefuseJoin(
    H.Socket()->Last(TEXT("phx_join")), TEXT("resume_refused")
  );
  TestEqual(TEXT("The owner hears the refusal"), H.ResumeRefused, 1);
  TestEqual(TEXT("It is not a resume"), H.Resumed, 0);
  TestFalse(TEXT("The session is forgotten"), H.Conn->HasSession());
  TestTrue(
    TEXT("The socket stays open for the next login"), H.Conn->IsConnected()
  );
  TestFalse(
    TEXT("No reconnect is scheduled"),
    H.Conn->GetBackoff().IsReconnectPending(H.Timers)
  );

  const int32 SentBefore = H.Socket()->Sent.Num();
  H.Conn->Rpc(TEXT("realm:list"), nullptr);
  TestEqual(
    TEXT("A request with no join sends nothing"),
    H.Socket()->Sent.Num(),
    SentBefore
  );

  // The owner logs in again on the same socket.
  H.LoginOk(TEXT("s2"), TEXT("t9"));
  TestEqual(
    TEXT("A new login makes a new session"), H.Conn->GetSessionId(), TEXT("s2")
  );

  // Any other refusal is the pod's trouble: another socket, same session.
  H.Settle();
  H.DropAndReopen();
  const int32 DroppedBefore = H.Dropped;
  H.Socket()->RefuseJoin(
    H.Socket()->Last(TEXT("phx_join")), TEXT("unavailable")
  );
  TestTrue(
    TEXT("The socket is closed by the client"), H.Socket()->bCloseCalled
  );
  TestTrue(TEXT("The session is kept"), H.Conn->HasSession());
  TestEqual(TEXT("The owner hears a drop"), H.Dropped, DroppedBefore + 1);
  TestTrue(
    TEXT("A reconnect is scheduled"),
    H.Conn->GetBackoff().IsReconnectPending(H.Timers)
  );
  H.Advance(PastSecondDelaySeconds);
  H.Accept();
  const FPhoenixMessage Resume = H.Socket()->Last(TEXT("phx_join"));
  TestTrue(
    TEXT("The next socket resumes the same session"),
    Resume.IsValid() &&
      Resume.Payload->GetObjectField(TEXT("resume"))
          ->GetStringField(TEXT("sessionId")) == TEXT("s2")
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostResumeInvalidTest,
  "Redwood.Bifrost.Connection.ResumeRefusedAsInvalid",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostResumeInvalidTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostConnectionTest;
  ExpectReconnectWarnings(*this);
  AddExpectedMessage(
    TEXT("session is gone"), ELogVerbosity::Warning, Contains, 1
  );
  FHarness H(*this);
  H.Open();
  H.LoginOk();
  H.DropAndReopen();

  // Bifrost does not know this client's resume: another socket would not
  // change that, so the session ends here, like a refused resume.
  H.Socket()->RefuseJoin(
    H.Socket()->Last(TEXT("phx_join")), TEXT("invalid_request")
  );
  TestEqual(TEXT("The owner hears the refusal"), H.ResumeRefused, 1);
  TestEqual(TEXT("It is not a drop"), H.Dropped, 1);
  TestFalse(TEXT("The session is forgotten"), H.Conn->HasSession());
  TestTrue(TEXT("The socket stays open"), H.Conn->IsConnected());
  TestFalse(
    TEXT("No reconnect is scheduled"),
    H.Conn->GetBackoff().IsReconnectPending(H.Timers)
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostPushOnceTest,
  "Redwood.Bifrost.Connection.PushDeliveredOnce",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostPushOnceTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostConnectionTest;
  FHarness H(*this);
  H.Open();
  const FPhoenixMessage Join = H.LoginOk();

  int32 Count = 0;
  double LastValue = 0.0;
  H.Conn->OnEvent(
    TEXT("realm:parties:changed"),
    [&Count,
     &LastValue](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      ++Count;
      LastValue = Message->AsObject()->GetNumberField(TEXT("x"));
    }
  );

  TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
  Payload->SetNumberField(TEXT("x"), 7);
  H.Socket()->Push(
    TEXT("m1"), TEXT("realm:parties:changed"), Payload, Join.Ref
  );
  TestEqual(TEXT("A push reaches its handler"), Count, 1);
  TestEqual(TEXT("With its payload"), LastValue, 7.0);

  H.Socket()->Push(
    TEXT("m1"), TEXT("realm:parties:changed"), Payload, Join.Ref
  );
  TestEqual(TEXT("The same push id is dropped"), Count, 1);

  H.Socket()->Push(
    TEXT("m2"), TEXT("realm:parties:changed"), Payload, Join.Ref
  );
  TestEqual(TEXT("Another push id is delivered"), Count, 2);

  H.Socket()->Push(FString(), TEXT("realm:parties:changed"), Payload, Join.Ref);
  H.Socket()->Push(FString(), TEXT("realm:parties:changed"), Payload, Join.Ref);
  TestEqual(TEXT("A push with no id is delivered every time"), Count, 4);

  H.Socket()->Push(TEXT("m3"), TEXT("realm:unknown"), Payload, Join.Ref);
  TestEqual(TEXT("A push with no handler changes nothing"), Count, 4);

  // The realm sends a kept push again after a resume.
  ExpectReconnectWarnings(*this);
  H.DropAndReopen();
  const FPhoenixMessage Resume = H.Socket()->Last(TEXT("phx_join"));
  H.Socket()->Reply(
    Resume, TEXT("ok"), FFakeBifrost::JoinReply(TEXT("s1"), TEXT("t2"), nullptr)
  );
  H.Socket()->Push(
    TEXT("m2"), TEXT("realm:parties:changed"), Payload, Resume.Ref
  );
  TestEqual(TEXT("A replay of a seen push is dropped"), Count, 4);
  H.Socket()->Push(
    TEXT("m4"), TEXT("realm:parties:changed"), Payload, Resume.Ref
  );
  TestEqual(TEXT("A new push after the resume is delivered"), Count, 5);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostClosedByServerTest,
  "Redwood.Bifrost.Connection.ClosedByServer",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostClosedByServerTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostConnectionTest;
  AddExpectedMessage(
    TEXT("Dropped a rpc request"), ELogVerbosity::Warning, Contains, 1
  );
  FHarness H(*this);
  H.Open();
  FPhoenixMessage Join = H.LoginOk();

  // Another login of the same player. Bifrost ends the channel and keeps the
  // socket.
  TSharedPtr<FJsonObject> LostAnswer;
  H.Conn->Rpc(TEXT("realm:list"), nullptr, Keep(LostAnswer));
  H.Socket()->CloseSession(TEXT("replaced"), Join.Ref);
  TestEqual(TEXT("The owner hears the reason"), H.ServerCloses.Num(), 1);
  TestEqual(TEXT("It is replaced"), H.ServerCloses.Last(), TEXT("replaced"));
  TestFalse(TEXT("The session is forgotten"), H.Conn->HasSession());

  H.Socket()->EndChannel(Join.Ref);
  TestEqual(TEXT("The end of the channel is not a drop"), H.Dropped, 0);
  TestTrue(TEXT("The socket stays open"), H.Conn->IsConnected());
  TestFalse(
    TEXT("No reconnect"), H.Conn->GetBackoff().IsReconnectPending(H.Timers)
  );
  TestEqual(TEXT("No new socket"), H.Bifrost.Sockets.Num(), 1);
  TestFalse(
    TEXT("A request that waited on the channel is not answered here"),
    LostAnswer.IsValid()
  );
  const int32 SentBefore = H.Socket()->Sent.Num();
  H.Conn->Rpc(TEXT("realm:list"), nullptr);
  TestEqual(
    TEXT("A request with no channel sends nothing"),
    H.Socket()->Sent.Num(),
    SentBefore
  );

  // The next login joins the same socket.
  Join = H.LoginOk(TEXT("s2"), TEXT("t2"));
  TestEqual(
    TEXT("A new session on the same socket"), H.Conn->GetSessionId(), TEXT("s2")
  );
  TestEqual(TEXT("Still one socket"), H.Bifrost.Sockets.Num(), 1);

  // The pod stops: the channel ends, and the session is resumed on a
  // sibling at once, before the stopping pod closes the socket.
  H.Socket()->CloseSession(TEXT("reconnect"), Join.Ref);
  TestEqual(
    TEXT("The owner hears the reason"), H.ServerCloses.Last(), TEXT("reconnect")
  );
  TestTrue(TEXT("The session is kept"), H.Conn->HasSession());
  H.Socket()->EndChannel(Join.Ref);
  TestEqual(
    TEXT("The end of the channel is a drop: requests are held"), H.Dropped, 1
  );
  TestTrue(
    TEXT("The client closes the old socket"), H.Bifrost.Sockets[0]->bCloseCalled
  );
  TestEqual(
    TEXT("The reconnect waits for the next tick"), H.Bifrost.Sockets.Num(), 1
  );
  TestFalse(
    TEXT("No backoff"), H.Conn->GetBackoff().IsReconnectPending(H.Timers)
  );
  H.Advance(0.01f);
  if (!TestEqual(
        TEXT("A new socket on the next tick"), H.Bifrost.Sockets.Num(), 2
      )) {
    return false;
  }
  TestEqual(
    TEXT("The reconnect counts no attempt"),
    H.Conn->GetBackoff().NumAttempts(),
    0
  );
  H.Accept();
  const FPhoenixMessage Resume = H.Socket()->Last(TEXT("phx_join"));
  TestTrue(
    TEXT("The new socket resumes the session"),
    Resume.IsValid() &&
      Resume.Payload->GetObjectField(TEXT("resume"))
          ->GetStringField(TEXT("sessionId")) == TEXT("s2")
  );
  H.Socket()->Reply(
    Resume, TEXT("ok"), FFakeBifrost::JoinReply(TEXT("s2"), TEXT("t3"), nullptr)
  );

  // A Bifrost that closes the socket itself after `closed`: not a drop, and
  // the socket comes back for the next login.
  const FPhoenixMessage Third = H.SendLogin();
  H.Socket()->Reply(
    Third,
    TEXT("ok"),
    FFakeBifrost::JoinReply(
      TEXT("s3"), TEXT("t4"), FFakeBifrost::LoginPlayer(TEXT("p1"), TEXT("tok"))
    )
  );
  H.Socket()->CloseSession(TEXT("logout"), Third.Ref);
  H.Socket()->Drop(FRedwoodBifrostConnection::NormalCloseCode);
  TestEqual(TEXT("A socket closed after `closed` is not a drop"), H.Dropped, 1);
  TestEqual(TEXT("But the owner hears the socket go"), H.QuietDrops, 1);
  TestTrue(
    TEXT("The socket comes back for the next login"),
    H.Conn->GetBackoff().IsReconnectPending(H.Timers)
  );
  H.Advance(PastFirstDelaySeconds);
  H.Accept();
  TestEqual(TEXT("Nothing is resumed"), H.Socket()->Sent.Num(), 0);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostHeartbeatTest,
  "Redwood.Bifrost.Connection.Heartbeat",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostHeartbeatTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostConnectionTest;
  AddExpectedMessage(
    TEXT("did not answer a heartbeat"), ELogVerbosity::Warning, Contains, 1
  );
  FHarness H(*this);
  H.Open();
  H.LoginOk();

  H.Advance(PastHeartbeatSeconds);
  const FPhoenixMessage Heartbeat = H.Socket()->Last(TEXT("heartbeat"));
  if (!TestTrue(
        TEXT("A heartbeat goes after the interval"), Heartbeat.IsValid()
      )) {
    return false;
  }
  TestEqual(TEXT("On the phoenix topic"), Heartbeat.Topic, TEXT("phoenix"));
  TestTrue(TEXT("With no join_ref"), Heartbeat.JoinRef.IsEmpty());
  TestFalse(TEXT("With a ref"), Heartbeat.Ref.IsEmpty());

  // Answered: the socket lives on.
  TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
  Reply->SetStringField(TEXT("status"), TEXT("ok"));
  Reply->SetObjectField(TEXT("response"), MakeShared<FJsonObject>());
  H.Socket()->Receive(
    FString(), Heartbeat.Ref, TEXT("phoenix"), TEXT("phx_reply"), Reply
  );
  H.Advance(PastHeartbeatSeconds);
  TestEqual(
    TEXT("The next heartbeat goes"), H.Socket()->CountSent(TEXT("heartbeat")), 2
  );
  TestFalse(
    TEXT("An answered heartbeat keeps the socket"), H.Socket()->bCloseCalled
  );

  // Not answered: the socket is dead with no close.
  H.Advance(PastHeartbeatSeconds);
  TestTrue(
    TEXT("The client closes the socket"), H.Bifrost.Sockets[0]->bCloseCalled
  );
  TestEqual(
    TEXT("With its own close code"),
    H.Bifrost.Sockets[0]->CloseCode,
    FRedwoodBifrostConnection::HeartbeatCloseCode
  );
  TestEqual(TEXT("The owner hears a drop"), H.Dropped, 1);
  TestTrue(TEXT("The session is kept"), H.Conn->HasSession());
  TestTrue(
    TEXT("A reconnect is scheduled"),
    H.Conn->GetBackoff().IsReconnectPending(H.Timers)
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostCloseStopsTest,
  "Redwood.Bifrost.Connection.CloseStopsReconnect",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostCloseStopsTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostConnectionTest;
  FHarness H(*this);
  H.Open();
  H.LoginOk();

  // A join and a request wait when the owner closes.
  bool bJoinOk = true;
  TSharedPtr<FJsonObject> JoinResponse;
  H.SendLogin(&bJoinOk, &JoinResponse);
  TSharedPtr<FJsonObject> LostAnswer;
  H.Conn->Rpc(TEXT("realm:list"), nullptr, Keep(LostAnswer));
  H.Conn->Close();
  TestTrue(TEXT("The socket is closed"), H.Socket()->bCloseCalled);
  TestFalse(TEXT("Not connected"), H.Conn->IsConnected());
  TestFalse(
    TEXT("No reconnect"), H.Conn->GetBackoff().IsReconnectPending(H.Timers)
  );
  TestEqual(TEXT("Nothing is reported as a drop"), H.Dropped, 0);
  H.Advance(PastSecondDelaySeconds);
  TestEqual(TEXT("No new socket"), H.Bifrost.Sockets.Num(), 1);
  TestFalse(TEXT("The waiting join is refused"), bJoinOk);
  if (TestNotNull(TEXT("With a response"), JoinResponse.Get())) {
    TestEqual(
      TEXT("Whose reason is not-connected"),
      JoinResponse->GetStringField(TEXT("reason")),
      TEXT("not_connected")
    );
    TestEqual(
      TEXT("And whose message is the lost-reply error"),
      JoinResponse->GetStringField(TEXT("message")),
      FRedwoodPendingReplies::LostReplyError
    );
  }
  TestFalse(
    TEXT("A waiting request is not answered here: the owner tracks it"),
    LostAnswer.IsValid()
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostStaleJoinTest,
  "Redwood.Bifrost.Connection.StaleJoinIgnored",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostStaleJoinTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostConnectionTest;
  FHarness H(*this);
  H.Open();
  const FPhoenixMessage FirstJoin = H.LoginOk();

  int32 Count = 0;
  H.Conn->OnEvent(
    TEXT("realm:regions"),
    [&Count](const FString &, const TSharedPtr<FJsonValue> &) { ++Count; }
  );

  // A second login replaces the join.
  const FPhoenixMessage SecondJoin = H.SendLogin();
  TestNotEqual(
    TEXT("The second join has its own ref"), SecondJoin.Ref, FirstJoin.Ref
  );
  H.Socket()->Push(TEXT("a"), TEXT("realm:regions"), nullptr, FirstJoin.Ref);
  TestEqual(TEXT("A push of the old join is ignored"), Count, 0);
  H.Socket()->Push(TEXT("b"), TEXT("realm:regions"), nullptr, SecondJoin.Ref);
  TestEqual(TEXT("A push of the new join is delivered"), Count, 1);
  H.Socket()->Push(TEXT("c"), TEXT("realm:regions"), nullptr, FString());
  TestEqual(TEXT("A push with no join_ref is delivered"), Count, 2);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostSupersededJoinTest,
  "Redwood.Bifrost.Connection.SupersededJoin",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostSupersededJoinTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostConnectionTest;
  FHarness H(*this);
  H.Open();

  // Two logins before any reply: the first is over the moment the second
  // goes out.
  bool bFirstAnswered = false;
  bool bFirstOk = true;
  TSharedPtr<FJsonObject> FirstResponse;
  H.Conn->Login(
    FHarness::LoginPayload(),
    [&bFirstAnswered,
     &bFirstOk,
     &FirstResponse](bool bOk, const TSharedPtr<FJsonObject> &Response) {
      bFirstAnswered = true;
      bFirstOk = bOk;
      FirstResponse = Response;
    }
  );
  const FPhoenixMessage FirstJoin = H.Socket()->Last(TEXT("phx_join"));
  TestFalse(TEXT("The first join waits"), bFirstAnswered);

  bool bSecondOk = false;
  const FPhoenixMessage SecondJoin = H.SendLogin(&bSecondOk);
  TestTrue(TEXT("The first join is answered by the second"), bFirstAnswered);
  TestFalse(TEXT("As a refusal"), bFirstOk);
  if (TestNotNull(TEXT("With a response"), FirstResponse.Get())) {
    TestEqual(
      TEXT("Whose reason is not-connected"),
      FirstResponse->GetStringField(TEXT("reason")),
      TEXT("not_connected")
    );
    TestEqual(
      TEXT("And whose message says a newer join replaced it"),
      FirstResponse->GetStringField(TEXT("message")),
      TEXT("A newer join replaced this one.")
    );
  }

  // A late reply to the replaced join changes nothing.
  H.Socket()->Reply(
    FirstJoin,
    TEXT("ok"),
    FFakeBifrost::JoinReply(
      TEXT("s1"), TEXT("t1"), FFakeBifrost::LoginPlayer(TEXT("p1"), TEXT("tok"))
    )
  );
  TestFalse(
    TEXT("The old join's reply makes no session"), H.Conn->HasSession()
  );
  TestFalse(TEXT("Nor answers the new join"), bSecondOk);

  H.Socket()->Reply(
    SecondJoin,
    TEXT("ok"),
    FFakeBifrost::JoinReply(
      TEXT("s2"), TEXT("t2"), FFakeBifrost::LoginPlayer(TEXT("p1"), TEXT("tok"))
    )
  );
  TestTrue(TEXT("The new join's reply is heard"), bSecondOk);
  TestEqual(
    TEXT("The new join makes the session"), H.Conn->GetSessionId(), TEXT("s2")
  );

  // A second reply to a join that was answered already is ignored.
  H.Socket()->RefuseJoin(SecondJoin, TEXT("login_failed"), TEXT("Late"));
  TestTrue(TEXT("The session stays"), H.Conn->HasSession());
  TestEqual(TEXT("With its id"), H.Conn->GetSessionId(), TEXT("s2"));
  H.Conn->Rpc(TEXT("realm:list"), nullptr);
  TestTrue(
    TEXT("The channel is still joined"),
    H.Socket()->Last(TEXT("realm:list")).IsValid()
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostStepsOneAtATimeTest,
  "Redwood.Bifrost.Connection.StepsOneAtATime",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostStepsOneAtATimeTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostConnectionTest;
  ExpectReconnectWarnings(*this);
  FHarness H(*this);
  H.Open();
  H.LoginOk();

  // The order the step callbacks run in, and what each was given.
  TArray<FString> Answered;
  TMap<FString, TSharedPtr<FJsonObject>> Answers;
  const auto Record = [&Answered, &Answers](const FString &Name) {
    return [&Answered,
            &Answers,
            Name](const TArray<TSharedPtr<FJsonValue>> &Response) {
      Answered.Add(Name);
      Answers.Add(Name, ResponseObject(Response));
    };
  };

  // The first step's callback sends a step of its own, which goes behind
  // the one that queued while the first was in flight.
  H.Conn->Step(
    TEXT("session.enter-realm"),
    nullptr,
    [&H, Record](const TArray<TSharedPtr<FJsonValue>> &Response) {
      Record(TEXT("enter"))(Response);
      H.Conn->Step(TEXT("session.logout"), nullptr, Record(TEXT("logout")));
    }
  );
  H.Conn->Step(
    TEXT("session.select-character"), nullptr, Record(TEXT("select"))
  );
  TestEqual(
    TEXT("The first step goes out"),
    H.Socket()->CountSent(TEXT("session.enter-realm")),
    1
  );
  TestEqual(
    TEXT("The second waits for its reply"),
    H.Socket()->CountSent(TEXT("session.select-character")),
    0
  );

  TSharedPtr<FJsonObject> NoError = MakeShared<FJsonObject>();
  NoError->SetStringField(TEXT("error"), TEXT(""));
  H.Socket()->ReplyOk(H.Socket()->Last(TEXT("session.enter-realm")), NoError);
  TestEqual(TEXT("The first callback ran"), Answered.Num(), 1);
  TestEqual(
    TEXT("The queued second step goes out at the reply"),
    H.Socket()->CountSent(TEXT("session.select-character")),
    1
  );
  TestEqual(
    TEXT("The step the callback sent waits behind it"),
    H.Socket()->CountSent(TEXT("session.logout")),
    0
  );

  H.Socket()->ReplyOk(
    H.Socket()->Last(TEXT("session.select-character")), NoError
  );
  TestEqual(TEXT("The second callback ran"), Answered.Num(), 2);
  TestEqual(
    TEXT("Then the third step goes out"),
    H.Socket()->CountSent(TEXT("session.logout")),
    1
  );

  // A drop with a step in flight and one queued: both are answered as lost.
  H.Conn->Step(TEXT("session.enter-realm"), nullptr, Record(TEXT("queued")));
  TestEqual(
    TEXT("The queued step did not go out"),
    H.Socket()->CountSent(TEXT("session.enter-realm")),
    1
  );
  H.Socket()->Drop();
  if (TestEqual(TEXT("Both callbacks ran at the drop"), Answered.Num(), 4)) {
    TestEqual(TEXT("The one in flight first"), Answered[2], TEXT("logout"));
    TestEqual(TEXT("Then the queued one"), Answered[3], TEXT("queued"));
    TestTrue(
      TEXT("The step in flight is answered as lost"),
      IsLostResponse(Answers.FindRef(TEXT("logout")))
    );
    TestTrue(
      TEXT("The queued step is answered as lost"),
      IsLostResponse(Answers.FindRef(TEXT("queued")))
    );
  }
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostConnectFailsTest,
  "Redwood.Bifrost.Connection.ConnectFailureBacksOff",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostConnectFailsTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostConnectionTest;
  AddExpectedMessage(
    TEXT("Could not connect to Bifrost"), ELogVerbosity::Warning, Contains, 2
  );
  FHarness H(*this);
  H.Conn->Connect(BaseUri);

  H.Socket()->FailToConnect();
  TestEqual(TEXT("A failed connect is a drop"), H.Dropped, 1);
  TestEqual(
    TEXT("It counts an attempt"), H.Conn->GetBackoff().NumAttempts(), 1
  );
  TestTrue(
    TEXT("A reconnect is scheduled"),
    H.Conn->GetBackoff().IsReconnectPending(H.Timers)
  );

  H.Advance(PastFirstDelaySeconds);
  TestEqual(TEXT("A second socket"), H.Bifrost.Sockets.Num(), 2);
  H.Socket()->FailToConnect();
  TestEqual(TEXT("A second attempt"), H.Conn->GetBackoff().NumAttempts(), 2);

  H.Advance(PastSecondDelaySeconds);
  TestEqual(
    TEXT("A third socket after the longer delay"), H.Bifrost.Sockets.Num(), 3
  );
  H.Accept();
  TestEqual(TEXT("The owner hears the connect"), H.Connected, 1);
  TestFalse(
    TEXT("No reconnect once connected"),
    H.Conn->GetBackoff().IsReconnectPending(H.Timers)
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostChannelErrorTest,
  "Redwood.Bifrost.Connection.ChannelErrorReconnects",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostChannelErrorTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostConnectionTest;
  AddExpectedMessage(
    TEXT("session channel ended"), ELogVerbosity::Warning, Contains, 1
  );
  FHarness H(*this);
  H.Open();
  const FPhoenixMessage Join = H.LoginOk();

  H.Socket()->Receive(
    Join.Ref,
    FString(),
    TEXT("session"),
    TEXT("phx_error"),
    MakeShared<FJsonObject>()
  );
  TestTrue(
    TEXT("The client closes the socket"), H.Bifrost.Sockets[0]->bCloseCalled
  );
  TestEqual(TEXT("The owner hears a drop"), H.Dropped, 1);
  TestTrue(TEXT("The session is kept"), H.Conn->HasSession());
  TestTrue(
    TEXT("A reconnect is scheduled"),
    H.Conn->GetBackoff().IsReconnectPending(H.Timers)
  );
  H.Advance(PastFirstDelaySeconds);
  H.Accept();
  TestTrue(
    TEXT("The new socket resumes"), H.Socket()->Last(TEXT("phx_join")).IsValid()
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostLostResumeKeepsSocketTest,
  "Redwood.Bifrost.Connection.LostResumeLeavesTheSocket",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostLostResumeKeepsSocketTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostConnectionTest;
  AddExpectedMessage(TEXT("reconnecting"), ELogVerbosity::Warning, Contains, 0);

  // A newer join replaces the resume in flight: the resume's refusal is
  // "not_connected", and the socket that carries the new join must stay.
  {
    FHarness H(*this);
    H.Open();
    H.LoginOk();
    H.Socket()->Drop();
    H.Advance(PastFirstDelaySeconds);
    H.Accept();
    TestTrue(
      TEXT("A resume is in flight"),
      H.Socket()->Last(TEXT("phx_join")).IsValid()
    );
    const int32 DroppedBefore = H.Dropped;
    const FPhoenixMessage Login = H.SendLogin();
    TestTrue(TEXT("The login goes out"), Login.IsValid());
    TestFalse(TEXT("The socket is not closed"), H.Socket()->bCloseCalled);
    TestTrue(TEXT("Still connected"), H.Conn->IsConnected());
    TestFalse(
      TEXT("No reconnect"), H.Conn->GetBackoff().IsReconnectPending(H.Timers)
    );
    TestEqual(TEXT("No drop"), H.Dropped, DroppedBefore);
    TestEqual(TEXT("One socket"), H.Bifrost.Sockets.Num(), 2);
  }

  // The socket drops with the resume in flight: the lost join is one drop,
  // not a drop and then a second one from the refusal it got.
  {
    FHarness H(*this);
    H.Open();
    H.LoginOk();
    H.Socket()->Drop();
    H.Advance(PastFirstDelaySeconds);
    H.Accept();
    H.Socket()->Drop();
    TestEqual(TEXT("Two drops for two sockets"), H.Dropped, 2);
    TestTrue(TEXT("The session is kept"), H.Conn->HasSession());
    TestTrue(
      TEXT("One reconnect is scheduled"),
      H.Conn->GetBackoff().IsReconnectPending(H.Timers)
    );
    TestEqual(TEXT("No extra socket yet"), H.Bifrost.Sockets.Num(), 2);
  }
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostJoinEndsOldChannelTest,
  "Redwood.Bifrost.Connection.JoinEndsTheOldChannel",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostJoinEndsOldChannelTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostConnectionTest;

  // A step waits on the old channel when a new login goes out: the step's
  // reply would carry the old join_ref and be dropped, so the step hears it
  // is lost now, and the new channel's first step is not stuck behind it.
  {
    FHarness H(*this);
    H.Open();
    H.LoginOk();
    TSharedPtr<FJsonObject> StepAnswer;
    H.Conn->Step(
      TEXT("session.enter-realm"),
      nullptr,
      [&StepAnswer](const TArray<TSharedPtr<FJsonValue>> &Response) {
        StepAnswer = ResponseObject(Response);
      }
    );
    const FPhoenixMessage Join = H.SendLogin();
    TestTrue(TEXT("The new join goes out"), Join.IsValid());
    if (TestNotNull(TEXT("The waiting step is answered"), StepAnswer.Get())) {
      TestEqual(
        TEXT("As lost"),
        StepAnswer->GetStringField(TEXT("code")),
        TEXT("not_connected")
      );
    }
    H.Socket()->Reply(
      Join,
      TEXT("ok"),
      FFakeBifrost::JoinReply(
        TEXT("s2"),
        TEXT("t2"),
        FFakeBifrost::LoginPlayer(TEXT("p1"), TEXT("tok"))
      )
    );
    H.Conn->Step(TEXT("session.select-character"), nullptr);
    TestTrue(
      TEXT("The new channel's first step goes out"),
      H.Socket()->Last(TEXT("session.select-character")).IsValid()
    );
  }

  // `closed` came, and a new login went out before the channel ended: the
  // old channel's end is filtered by join_ref, so the connection must not
  // wait for it. The next real drop is a drop, not a quiet close.
  {
    FHarness H(*this);
    H.Open();
    const FPhoenixMessage First = H.LoginOk();
    H.Socket()->CloseSession(TEXT("replaced"), First.Ref);
    H.LoginOk(TEXT("s2"), TEXT("t2"));
    H.Socket()->EndChannel(First.Ref);
    TestTrue(
      TEXT("The old channel's end changes nothing"), H.Conn->HasSession()
    );
    ExpectReconnectWarnings(*this);
    H.Socket()->Drop();
    TestEqual(TEXT("The drop is announced"), H.Dropped, 1);
    TestEqual(TEXT("And not quiet"), H.QuietDrops, 0);
  }
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostChatTest,
  "Redwood.Bifrost.Connection.ChatLinesAndResume",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostChatTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostConnectionTest;
  FHarness H(*this);
  H.Open();
  const FPhoenixMessage Join = H.LoginOk();

  const FString Party = TEXT("r:realm-1/party:party-1");
  const FString Guild = TEXT("guild:guild-1");
  TArray<FRedwoodChatMessage> Lines;
  H.Conn->OnChatMessage = [&Lines](const FRedwoodChatMessage &Message) {
    Lines.Add(Message);
  };
  TArray<FString> Topics;
  int32 TopicUpdates = 0;
  H.Conn->OnChatTopics = [&Topics, &TopicUpdates](const TArray<FString> &In) {
    Topics = In;
    ++TopicUpdates;
  };

  // A live line, with its fields.
  H.Socket()->ChatMessage(
    FFakeBifrost::ChatLine(Party, 1, TEXT("m1"), TEXT("first")), Join.Ref
  );
  if (!TestEqual(TEXT("A chat line reaches the callback"), Lines.Num(), 1)) {
    return false;
  }
  TestEqual(TEXT("With its topic"), Lines[0].Topic, Party);
  TestEqual(TEXT("With its seq"), Lines[0].Seq, static_cast<int64>(1));
  TestEqual(TEXT("With its id"), Lines[0].MsgId, TEXT("m1"));
  TestEqual(
    TEXT("With its category"), Lines[0].Category, TEXT("Message.Chat.Party")
  );
  TestEqual(
    TEXT("With the sender's character"),
    Lines[0].SenderCharacterId,
    TEXT("char-9")
  );
  TestEqual(TEXT("With the sender's name"), Lines[0].SenderName, TEXT("Bravo"));
  TestEqual(TEXT("With the template"), Lines[0].Template, TEXT("first"));
  TestTrue(
    TEXT("With the time it was sent, from ISO 8601"),
    Lines[0].SentAt == FDateTime(2026, 10, 23, 12, 0, 0)
  );
  TestEqual(
    TEXT("The topic's cursor is the line's seq"),
    H.Conn->GetChatCursors()[Party],
    static_cast<int64>(1)
  );

  // Replays are dropped: the same seq under a new id, a seen id under a new
  // seq, and a seq before the cursor.
  H.Socket()->ChatMessage(
    FFakeBifrost::ChatLine(Party, 1, TEXT("m1b")), Join.Ref
  );
  H.Socket()->ChatMessage(
    FFakeBifrost::ChatLine(Party, 2, TEXT("m1")), Join.Ref
  );
  TestEqual(
    TEXT("A line at the cursor, or with a seen id, is dropped"), Lines.Num(), 1
  );
  H.Socket()->ChatMessage(
    FFakeBifrost::ChatLine(Party, 3, TEXT("m3")), Join.Ref
  );
  TestEqual(TEXT("A line after the cursor is delivered"), Lines.Num(), 2);
  H.Socket()->ChatMessage(
    FFakeBifrost::ChatLine(Party, 2, TEXT("m2")), Join.Ref
  );
  TestEqual(TEXT("A line before the cursor is dropped"), Lines.Num(), 2);

  // A topic the connection has no cursor for starts live.
  H.Socket()->ChatMessage(
    FFakeBifrost::ChatLine(Guild, 40, TEXT("g40")), Join.Ref
  );
  TestEqual(TEXT("A line of a new topic is delivered"), Lines.Num(), 3);
  TestEqual(
    TEXT("And starts that topic's cursor"),
    H.Conn->GetChatCursors()[Guild],
    static_cast<int64>(40)
  );

  // A line with nothing to order or deduplicate it is dropped, loudly.
  AddExpectedMessage(
    TEXT("Dropped a chat.msg"), ELogVerbosity::Warning, Contains, 1
  );
  TSharedPtr<FJsonObject> NoSeq = FFakeBifrost::ChatLine(Party, 4, TEXT("m4"));
  NoSeq->RemoveField(TEXT("seq"));
  H.Socket()->ChatMessage(NoSeq, Join.Ref);
  TestEqual(TEXT("A line with no seq is dropped"), Lines.Num(), 3);

  // The session's topics.
  H.Socket()->ChatTopics({Party, Guild}, Join.Ref);
  TestEqual(TEXT("The topics reach their callback"), TopicUpdates, 1);
  TestEqual(TEXT("All of them"), Topics.Num(), 2);
  TestTrue(TEXT("Including the guild's"), Topics.Contains(Guild));

  // A send, answered with its seq.
  TSharedPtr<FJsonObject> Answer;
  H.Conn->ChatSend(TEXT("c1"), Party, TEXT("hi all"), Keep(Answer));
  const FPhoenixMessage Send = H.Socket()->Last(TEXT("chat.send"));
  if (!TestTrue(TEXT("A send goes out as chat.send"), Send.IsValid())) {
    return false;
  }
  TestEqual(TEXT("The send is on the join"), Send.JoinRef, Join.Ref);
  TestEqual(
    TEXT("With the caller's id"),
    Send.Payload->GetStringField(TEXT("msgId")),
    TEXT("c1")
  );
  TestEqual(
    TEXT("The topic"), Send.Payload->GetStringField(TEXT("topic")), Party
  );
  TestEqual(
    TEXT("And the body"),
    Send.Payload->GetStringField(TEXT("body")),
    TEXT("hi all")
  );
  TSharedPtr<FJsonObject> Posted = MakeShared<FJsonObject>();
  Posted->SetStringField(TEXT("msgId"), TEXT("c1"));
  Posted->SetStringField(TEXT("topic"), Party);
  Posted->SetNumberField(TEXT("seq"), 4);
  H.Socket()->Reply(Send, TEXT("ok"), Posted);
  if (TestNotNull(TEXT("The send is answered"), Answer.Get())) {
    TestEqual(
      TEXT("As a success"), Answer->GetStringField(TEXT("error")), TEXT("")
    );
    TestEqual(
      TEXT("With the seq Bifrost gave the line"),
      static_cast<int32>(Answer->GetNumberField(TEXT("seq"))),
      4
    );
  }

  // A send Bifrost refuses.
  Answer.Reset();
  H.Conn->ChatSend(TEXT("c2"), Party, TEXT("again"), Keep(Answer));
  H.Socket()->Refuse(
    H.Socket()->Last(TEXT("chat.send")), TEXT("muted"), TEXT("You are muted.")
  );
  if (TestNotNull(TEXT("The refusal is answered"), Answer.Get())) {
    TestEqual(
      TEXT("With its code"), Answer->GetStringField(TEXT("code")), TEXT("muted")
    );
  }

  // A whisper names a player, not a topic.
  H.Conn->ChatWhisper(TEXT("c3"), TEXT("p2"), TEXT("psst"), nullptr);
  const FPhoenixMessage Whisper = H.Socket()->Last(TEXT("chat.send"));
  TestEqual(
    TEXT("A whisper carries the target player"),
    Whisper.Payload->GetStringField(TEXT("to")),
    TEXT("p2")
  );
  TestFalse(TEXT("And no topic"), Whisper.Payload->HasField(TEXT("topic")));

  // A report.
  H.Conn->ChatReport(Party, 3, TEXT("spam"), nullptr);
  const FPhoenixMessage Report = H.Socket()->Last(TEXT("chat.report"));
  TestTrue(
    TEXT("A report names the line and the reason"),
    Report.IsValid() &&
      Report.Payload->GetStringField(TEXT("topic")) == Party &&
      static_cast<int32>(Report.Payload->GetNumberField(TEXT("seq"))) == 3 &&
      Report.Payload->GetStringField(TEXT("reason")) == TEXT("spam")
  );

  // The resume sends the cursors. The reply lists the party's lines newest
  // first and one the client has (seq 3): they are delivered oldest first,
  // and the seen one is dropped. A live line that arrives while the resume
  // waits comes after the replay, so the replay is not dropped as old.
  Answer.Reset();
  H.Conn->ChatResume(Keep(Answer));
  const FPhoenixMessage Resume = H.Socket()->Last(TEXT("chat.resume"));
  if (!TestTrue(TEXT("The resume goes out"), Resume.IsValid())) {
    return false;
  }
  const TSharedPtr<FJsonObject> Cursors =
    Resume.Payload->GetObjectField(TEXT("cursors"));
  TestEqual(TEXT("With a cursor per topic"), Cursors->Values.Num(), 2);
  TestEqual(
    TEXT("The party's cursor"),
    static_cast<int32>(Cursors->GetNumberField(Party)),
    3
  );
  TestEqual(
    TEXT("The guild's cursor"),
    static_cast<int32>(Cursors->GetNumberField(Guild)),
    40
  );
  H.Socket()->ChatMessage(
    FFakeBifrost::ChatLine(Party, 7, TEXT("m7")), Join.Ref
  );
  TestEqual(TEXT("A live line waits for the resume"), Lines.Num(), 3);

  TSharedPtr<FJsonObject> ResumedTopics = MakeShared<FJsonObject>();
  ResumedTopics->SetObjectField(
    Party,
    FFakeBifrost::ResumedTopic(
      {FFakeBifrost::ChatLine(Party, 6, TEXT("m6")),
       FFakeBifrost::ChatLine(Party, 5, TEXT("m5")),
       FFakeBifrost::ChatLine(Party, 3, TEXT("m3"))},
      true
    )
  );
  ResumedTopics->SetObjectField(Guild, FFakeBifrost::ResumedTopic({}, false));
  TSharedPtr<FJsonObject> ResumeReply = MakeShared<FJsonObject>();
  ResumeReply->SetObjectField(TEXT("topics"), ResumedTopics);
  H.Socket()->Reply(Resume, TEXT("ok"), ResumeReply);

  if (!TestEqual(
        TEXT("The replay and the live line are delivered"), Lines.Num(), 6
      )) {
    return false;
  }
  TestEqual(TEXT("Oldest first"), Lines[3].Seq, static_cast<int64>(5));
  TestEqual(TEXT("Then the next"), Lines[4].Seq, static_cast<int64>(6));
  TestEqual(TEXT("Then the live line"), Lines[5].Seq, static_cast<int64>(7));
  TestEqual(
    TEXT("The cursor is at the live line"),
    H.Conn->GetChatCursors()[Party],
    static_cast<int64>(7)
  );
  if (TestNotNull(TEXT("The resume is answered"), Answer.Get())) {
    const TArray<FString> Truncated =
      FRedwoodBifrostConnection::TruncatedTopics(Answer);
    TestEqual(TEXT("One topic was cut short"), Truncated.Num(), 1);
    TestTrue(TEXT("The party's"), Truncated.Contains(Party));
  }

  // Lines Bifrost does not store (`/all`, seq 0) have no cursor: nothing
  // can replay them, so only their id tells a replay.
  const FString All = TEXT("r:realm-1/all");
  H.Socket()->ChatMessage(FFakeBifrost::ChatLine(All, 0, TEXT("a1")), Join.Ref);
  H.Socket()->ChatMessage(FFakeBifrost::ChatLine(All, 0, TEXT("a2")), Join.Ref);
  TestEqual(TEXT("Two unstored lines are both delivered"), Lines.Num(), 8);
  H.Socket()->ChatMessage(FFakeBifrost::ChatLine(All, 0, TEXT("a1")), Join.Ref);
  TestEqual(TEXT("The same unstored line is dropped"), Lines.Num(), 8);
  TestFalse(
    TEXT("No cursor is kept for an unstored topic"),
    H.Conn->GetChatCursors().Contains(All)
  );

  // A new session starts live.
  H.Conn->ResetChat();
  TestEqual(TEXT("No cursor is left"), H.Conn->GetChatCursors().Num(), 0);
  H.Socket()->ChatMessage(
    FFakeBifrost::ChatLine(Party, 1, TEXT("m1")), Join.Ref
  );
  TestEqual(TEXT("A line of the new session is delivered"), Lines.Num(), 9);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostChatResumeAfterResetTest,
  "Redwood.Bifrost.Connection.ChatResumeEndedByResetIsStale",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostChatResumeAfterResetTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostConnectionTest;
  FHarness H(*this);
  H.Open();
  const FPhoenixMessage Join = H.LoginOk();
  const FString Party = TEXT("r:realm-1/party:party-1");
  int32 Delivered = 0;
  int32 TopicUpdates = 0;
  H.Conn->OnChatMessage = [&Delivered](const FRedwoodChatMessage &) {
    ++Delivered;
  };
  H.Conn->OnChatTopics = [&TopicUpdates](const TArray<FString> &) {
    ++TopicUpdates;
  };
  H.Socket()->ChatMessage(
    FFakeBifrost::ChatLine(Party, 1, TEXT("m1")), Join.Ref
  );
  TestEqual(TEXT("A line is delivered"), Delivered, 1);

  // The resume waits; a live line is held for it. The player logs out: the
  // session is forgotten and the chat reset before the reply comes.
  TSharedPtr<FJsonObject> Answer;
  H.Conn->ChatResume(Keep(Answer));
  const FPhoenixMessage Resume = H.Socket()->Last(TEXT("chat.resume"));
  H.Socket()->ChatMessage(
    FFakeBifrost::ChatLine(Party, 2, TEXT("m2")), Join.Ref
  );
  TestEqual(TEXT("The live line waits for the resume"), Delivered, 1);
  H.Conn->ForgetSession();
  H.Conn->ResetChat();

  // The old session's replay arrives: it is stale.
  TSharedPtr<FJsonObject> Topics = MakeShared<FJsonObject>();
  Topics->SetObjectField(
    Party,
    FFakeBifrost::ResumedTopic(
      {FFakeBifrost::ChatLine(Party, 3, TEXT("m3"))}, false
    )
  );
  TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
  Reply->SetObjectField(TEXT("topics"), Topics);
  H.Socket()->Reply(Resume, TEXT("ok"), Reply);
  TestEqual(
    TEXT("Neither the replay nor the held line is delivered"), Delivered, 1
  );
  TestEqual(TEXT("No cursor is kept"), H.Conn->GetChatCursors().Num(), 0);
  TestNotNull(TEXT("The caller still hears the reply"), Answer.Get());

  // Lines and topics between the logout and Bifrost's `closed` are the
  // ended session's.
  H.Socket()->ChatMessage(
    FFakeBifrost::ChatLine(Party, 4, TEXT("m4")), Join.Ref
  );
  H.Socket()->ChatTopics({Party}, Join.Ref);
  TestEqual(TEXT("A line with no session is dropped"), Delivered, 1);
  TestEqual(TEXT("Topics with no session are dropped"), TopicUpdates, 0);
  TestEqual(TEXT("And start no cursor"), H.Conn->GetChatCursors().Num(), 0);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostChatResumeLostTest,
  "Redwood.Bifrost.Connection.ChatResumeLostToDropDeliversHeldLines",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostChatResumeLostTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostConnectionTest;
  ExpectReconnectWarnings(*this);
  FHarness H(*this);
  H.Open();
  const FPhoenixMessage Join = H.LoginOk();
  const FString Party = TEXT("r:realm-1/party:party-1");
  TArray<FRedwoodChatMessage> Lines;
  H.Conn->OnChatMessage = [&Lines](const FRedwoodChatMessage &Line) {
    Lines.Add(Line);
  };
  H.Socket()->ChatMessage(
    FFakeBifrost::ChatLine(Party, 1, TEXT("m1")), Join.Ref
  );

  // The socket goes with the resume in flight: the line held for it is
  // delivered, and the owner (who tracks the reply) hears the loss.
  H.Conn->ChatResume(nullptr);
  H.Socket()->ChatMessage(
    FFakeBifrost::ChatLine(Party, 3, TEXT("m3")), Join.Ref
  );
  TestEqual(TEXT("The live line waits for the resume"), Lines.Num(), 1);
  H.Socket()->Drop();
  TestEqual(TEXT("The drop delivers the held line"), Lines.Num(), 2);
  TestEqual(
    TEXT("The cursor moved on with it"),
    H.Conn->GetChatCursors()[Party],
    static_cast<int64>(3)
  );

  // The next socket's resume asks from the new cursor.
  H.Advance(PastFirstDelaySeconds);
  H.Accept();
  const FPhoenixMessage Resume = H.Socket()->Last(TEXT("phx_join"));
  H.Socket()->Reply(
    Resume, TEXT("ok"), FFakeBifrost::JoinReply(TEXT("s1"), TEXT("t2"), nullptr)
  );
  H.Conn->ChatResume(nullptr);
  TestEqual(
    TEXT("The next resume carries the moved cursor"),
    static_cast<int32>(H.Socket()
                         ->Last(TEXT("chat.resume"))
                         .Payload->GetObjectField(TEXT("cursors"))
                         ->GetNumberField(Party)),
    3
  );
  return true;
}
