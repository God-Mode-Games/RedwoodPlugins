// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins the ticketing cluster of URedwoodClientInterface on the fake Bifrost
// of FakeBifrost.h (HollowedOath#2886, #3002): what decides whether a zone
// assignment (realm:servers:connect-to-instance) moves the player, and what
// the client owes the realm after a join went wrong. The session itself is
// pinned in BifrostClientInterfaceTest.cpp; the replay of one push id in its
// ZoneAssignmentOnce test.
//   1. An assignment names its character: one for another character is a
//      replay and does not move the player; one from an old server names no
//      character and moves as before. Selecting another character puts the
//      player back at character select, where no assignment moves them.
//   2. A join whose reply was lost with the socket is dropped: the game gave
//      up on it, so the server's replay of its assignment must not move the
//      player, and the ticket the server may have kept is left at the next
//      realm entry. A failed leave stays owed; a new join replaces it.
//   3. The same for a custom join, a proxy join and a create that joins.
//   4. A refused join, a refused proxy join or a refused create expects no
//      assignment; an ended ticket (realm:ticketing:ticket-error) neither.
//      An accepted create that joins gets its assignment.
//   5. In the world, the server's zone transfers move the player whatever a
//      later join did. An older join lost after a newer one was answered
//      keeps the newer join.
//   6. The game's leave drops the join; a leave lost with the socket is
//      owed, and the resume pays it.
//   7. An owed leave waits while this client's own join is out, and a logout
//      pays it before `session.logout`, then forgets the character.
//   8. The travel of an old assignment that arrives after a newer join keeps
//      the newer join's flags; after a clean leave of the newer join, the
//      player is in the world. Back at the entry level, nothing moves them.
//   9. A party ticket the server names (realm:ticketing:party-queued) is a
//      member's join: repeats, ended tickets, notices for another character
//      and other tickets' assignments are refused.
//  10. The member's own join and the party's ticket: the notice leaves the
//      own ticket, the own join replaces the notice, and a character switch
//      clears the dropped join but keeps the old ticket's owed leave.
//  11. A "queued" that arrives before a character is selected waits for its
//      character.
//  12. HollowedOath#2929. Enter World during a drop (the proxy list, then
//      the join) is held like every realm request, goes out in order after
//      the resume, and the join's assignment moves the player; a join that
//      waits out the grace is a ticket error, and a leave that does is owed
//      and paid at the resume. The game's leave made during a drop is held
//      too, and pays what was owed once.
//  13. HollowedOath#3609. A ticketing join opens the guard when the realm
//      accepts it, not when it goes out: the server replays a lost ticket's
//      assignment at the resume, before it answers the join the player
//      typed during the drop. The realm also pushes the new ticket's
//      assignment before its answer, so an assignment that names a ticket
//      waits for the answer, which names the join's ticket: the join's own
//      moves the player, another's is let go, and after the answer another
//      ticket's is refused. The lost join and its owed leave stay until the
//      acceptance; a refusal in the world lets zone transfers go on; an
//      `unavailable` answer is a lost reply. A proxy join opens the guard
//      at the send, as before. A party notice still leaves the ticket of
//      an own join that waits for its answer.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

#include "BifrostClientHarness.h"
#include "ClientTestAccess.h"
#include "FakeBifrost.h"
#include "JoinServerListener.h"
#include "RedwoodClientInterface.h"
#include "RedwoodCloseBackoff.h"
#include "RedwoodPendingReplies.h"

namespace RedwoodBifrostZoneTravelTest {
  using namespace RedwoodBifrostClientTest;
  using FAccess = FRedwoodClientTestAccess;

  // Just over the time a connection must stay up before the next drop starts
  // the backoff again from its first delay.
  constexpr float PastStableSeconds =
    FRedwoodCloseBackoff::StableConnectionSeconds + 0.1f;

  const FString ProxyId = TEXT("proxy-1");
  const FString ZoneName = TEXT("L_Zone");
  const FString ServerAddress = TEXT("10.0.0.1:7777");

  const TCHAR *const JoinQueueEvent = TEXT("realm:ticketing:join:queue");
  const TCHAR *const LeaveEvent = TEXT("realm:ticketing:leave");

  // The handler's warning for an assignment it refuses. Each test states how
  // many it expects, so a refusal that does not happen fails the test too.
  void ExpectIgnoredAssignments(FAutomationTestBase &Test, int32 Count) {
    Test.AddExpectedMessage(
      TEXT("Ignoring a zone assignment"),
      ELogVerbosity::Warning,
      Contains,
      Count
    );
  }

  // The realm's acceptance of a ticketing join, naming the join's ticket.
  TSharedPtr<FJsonObject> AcceptedJoin(const FString &TicketId) {
    TSharedPtr<FJsonObject> Reply = NoError();
    Reply->SetStringField(TEXT("ticketId"), TicketId);
    return Reply;
  }

  // The client of FClientHarness with the realm's pushes of the cluster, and
  // a listener for the move they can cause.
  struct FZoneHarness : FClientHarness {
    TStrongObjectPtr<URedwoodJoinServerListener> Joins;
    int32 PushCount = 0;
    // The answers of the joins and leaves a test does not watch. The client
    // keeps a join's delegate until the join ends, so it must outlive the
    // call that made it.
    TCapture<FRedwoodTicketingUpdate> Ticketing;
    TCapture<FString> Leaves;

    explicit FZoneHarness(FAutomationTestBase &InTest) :
      FClientHarness(InTest), Joins(NewObject<URedwoodJoinServerListener>()) {
      Joins->Watch(Client());
    }

    // Every push gets its own id: the connection drops a push whose id it
    // saw, which is its own test.
    FString NextPushId() {
      return FString::Printf(TEXT("push-%d"), ++PushCount);
    }

    void Push(const FString &Event, const TSharedPtr<FJsonObject> &Payload) {
      Socket()->Push(NextPushId(), Event, Payload, JoinRef);
    }

    // A zone assignment. An empty character id is an old server's; an empty
    // ticket id a solo one.
    void PushAssignment(
      const FString &ForCharacterId, const FString &TicketId = FString()
    ) {
      TSharedPtr<FJsonObject> Assignment = MakeShared<FJsonObject>();
      Assignment->SetStringField(TEXT("connection"), ServerAddress);
      Assignment->SetStringField(TEXT("token"), TEXT("server-token"));
      Assignment->SetBoolField(TEXT("shouldStitch"), false);
      if (!ForCharacterId.IsEmpty()) {
        Assignment->SetStringField(TEXT("characterId"), ForCharacterId);
      }
      if (!TicketId.IsEmpty()) {
        Assignment->SetStringField(TEXT("ticketId"), TicketId);
      }
      Push(TEXT("realm:servers:connect-to-instance"), Assignment);
    }

    // Pushes an assignment and tells whether it moved the player.
    bool AssignmentMoves(
      const FString &ForCharacterId = CharacterId,
      const FString &TicketId = FString()
    ) {
      const int32 Before = Joins->Count;
      PushAssignment(ForCharacterId, TicketId);
      return Joins->Count > Before;
    }

    // The leader queued the whole party; the server tells this member.
    void PushPartyQueued(
      const FString &ForCharacterId,
      const FString &TicketId,
      const FString &MessageId
    ) {
      TSharedPtr<FJsonObject> Notice = MakeShared<FJsonObject>();
      Notice->SetStringField(TEXT("leaderId"), TEXT("p2"));
      Notice->SetStringField(TEXT("characterId"), ForCharacterId);
      Notice->SetStringField(TEXT("ticketId"), TicketId);
      Notice->SetStringField(TEXT("messageId"), MessageId);
      Push(TEXT("realm:ticketing:party-queued"), Notice);
    }

    void PushPartyLeft(const FString &TicketId, const FString &MessageId) {
      TSharedPtr<FJsonObject> Notice = MakeShared<FJsonObject>();
      Notice->SetStringField(TEXT("leaderId"), TEXT("p2"));
      Notice->SetStringField(TEXT("ticketId"), TicketId);
      Notice->SetStringField(TEXT("messageId"), MessageId);
      Push(TEXT("realm:ticketing:party-left"), Notice);
    }

    void PushTicketError(const FString &Error) {
      TSharedPtr<FJsonObject> Notice = MakeShared<FJsonObject>();
      Notice->SetStringField(TEXT("error"), Error);
      Push(TEXT("realm:ticketing:ticket-error"), Notice);
    }

    // Sends a queue join and returns its rpc, unanswered.
    FPhoenixMessage SendJoinQueue(
      TCapture<FRedwoodTicketingUpdate> &Update,
      bool bTransferWholeParty = false
    ) {
      Client()->JoinQueue(
        ProxyId, ZoneName, bTransferWholeParty, false, Update.Delegate()
      );
      return Socket()->Last(JoinQueueEvent);
    }

    // Sends a queue join and the realm accepts it.
    void JoinQueueOk() {
      Socket()->ReplyOk(SendJoinQueue(Ticketing), NoError());
    }

    // Sends the game's leave and returns its rpc, unanswered.
    FPhoenixMessage SendLeave(TCapture<FString> &Left) {
      Client()->LeaveTicketing(Left.Delegate());
      return Socket()->Last(LeaveEvent);
    }

    // Sends the game's leave and the realm accepts it.
    void LeaveOk() {
      Socket()->ReplyOk(SendLeave(Leaves), NoError());
    }

    // The socket drops. The connection counted as stable first, so every
    // drop in a test starts the backoff from its first delay.
    void Drop() {
      Advance(PastStableSeconds);
      Socket()->Drop();
    }

    // The backoff brings a new socket; the server accepts it and resumes the
    // session as it was.
    void ReopenAndResume() {
      Advance(PastFirstDelaySeconds);
      Accept();
      ResumeOk();
    }

    // The server accepts the resume with no realm and names the locked one;
    // the client enters it again. The entry step comes back unanswered.
    FPhoenixMessage ReopenAndReenter() {
      Advance(PastFirstDelaySeconds);
      Accept();
      const FPhoenixMessage Resume = Socket()->Last(TEXT("phx_join"));
      JoinRef = Resume.Ref;
      Socket()->Reply(
        Resume,
        TEXT("ok"),
        FFakeBifrost::JoinReply(
          TEXT("s1"),
          TEXT("rt2"),
          ResumeIdentity(),
          FString(),
          FString(),
          RealmId
        )
      );
      return Socket()->Last(TEXT("session.enter-realm"));
    }

    // The realm accepts the entry, and the character selection the client
    // sends again for the realm that lost it. Answered, so a later step (a
    // logout) does not wait behind it.
    void RealmAcceptsReentry(const FPhoenixMessage &Enter) {
      Socket()->ReplyOk(Enter, NoError());
      Socket()->ReplyOk(
        Socket()->Last(TEXT("session.select-character")), NoError()
      );
    }

    int32 LeavesSent() const {
      return Socket()->CountSent(LeaveEvent);
    }
  };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodZoneTravelAssignmentCharacterTest,
  "Redwood.Bifrost.ZoneTravel.AssignmentNamesTheCharacter",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodZoneTravelAssignmentCharacterTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostZoneTravelTest;
  ExpectIgnoredAssignments(*this, 2);
  FZoneHarness H(*this);
  H.EnterSessionWithCharacter();
  H.JoinQueueOk();
  TestTrue(
    TEXT("A join expects an assignment"),
    FAccess::IsAssignmentExpected(*H.Client())
  );

  TestFalse(
    TEXT("An assignment for another character does not move the player"),
    H.AssignmentMoves(OtherCharacterId)
  );
  TestTrue(
    TEXT("One for the selected character moves the player"),
    H.AssignmentMoves(CharacterId)
  );
  TestTrue(
    TEXT("To the server, as the character"),
    H.Joins->LastConsoleCommand.Contains(ServerAddress) &&
      H.Joins->LastConsoleCommand.Contains(CharacterId)
  );
  TestTrue(
    TEXT("One from an old server names no character and moves as before"),
    H.AssignmentMoves(FString())
  );

  // Another character is at character select until it joins.
  H.Client()->SetSelectedCharacter(OtherCharacterId);
  TestFalse(
    TEXT("A new character expects no assignment"),
    FAccess::IsAssignmentExpected(*H.Client())
  );
  TestFalse(
    TEXT("Its travel is not the old character's"),
    FAccess::IsTravelPending(*H.Client())
  );
  TestFalse(
    TEXT("A replay for the new character does not move it before it joins"),
    H.AssignmentMoves(OtherCharacterId)
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodZoneTravelLostJoinTest,
  "Redwood.Bifrost.ZoneTravel.LostJoinIsDropped",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodZoneTravelLostJoinTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostZoneTravelTest;
  ExpectDrop(*this);
  ExpectIgnoredAssignments(*this, 1);
  FZoneHarness H(*this);
  H.EnterSessionWithCharacter();

  TCapture<FRedwoodTicketingUpdate> Update;
  const FPhoenixMessage Join = H.SendJoinQueue(Update);
  if (!TestTrue(TEXT("The join goes out"), Join.IsValid())) {
    return false;
  }
  H.Drop();
  TestEqual(TEXT("The lost join is answered once"), Update.Count, 1);
  TestEqual(
    TEXT("As a join response"),
    Update.Last.Type,
    ERedwoodTicketingUpdateType::JoinResponse
  );
  TestEqual(
    TEXT("With the lost-reply error"),
    Update.Last.Message,
    FRedwoodPendingReplies::LostReplyError
  );
  TestTrue(
    TEXT("The join is dropped"), FAccess::IsQueueJoinAbandoned(*H.Client())
  );
  TestFalse(
    TEXT("No assignment is expected"),
    FAccess::IsAssignmentExpected(*H.Client())
  );
  TestTrue(
    TEXT("A leave is owed for the ticket the server may have kept"),
    FAccess::IsLeaveTicketingOwed(*H.Client())
  );

  // The session comes back through the locked realm: the realm knows the
  // player again at the entry's answer, and the owed leave goes then.
  const FPhoenixMessage Enter = H.ReopenAndReenter();
  if (!TestTrue(TEXT("The client enters the realm again"), Enter.IsValid())) {
    return false;
  }
  TestEqual(TEXT("No leave before the realm answers"), H.LeavesSent(), 0);
  H.RealmAcceptsReentry(Enter);
  const FPhoenixMessage Leave = H.Socket()->Last(LeaveEvent);
  if (!TestTrue(TEXT("The owed leave goes at the entry"), Leave.IsValid())) {
    return false;
  }
  TestEqual(
    TEXT("For the player"),
    Leave.Payload->GetObjectField(TEXT("payload"))
      ->GetStringField(TEXT("playerId")),
    PlayerId
  );
  H.Socket()->ReplyWithError(Leave, TEXT("Leave failed."));
  TestTrue(
    TEXT("A failed leave is still owed"),
    FAccess::IsLeaveTicketingOwed(*H.Client())
  );
  TestFalse(
    TEXT(
      "The replay of the dropped join's assignment does not move the player"
    ),
    H.AssignmentMoves()
  );

  // A new join replaces any ticket the server kept, and the leave with it.
  TCapture<FRedwoodTicketingUpdate> Again;
  H.Socket()->ReplyOk(H.SendJoinQueue(Again), NoError());
  TestFalse(
    TEXT("A new join replaces the owed leave"),
    FAccess::IsLeaveTicketingOwed(*H.Client())
  );
  TestFalse(
    TEXT("And clears the dropped join"),
    FAccess::IsQueueJoinAbandoned(*H.Client())
  );
  TestTrue(TEXT("Its assignment moves the player"), H.AssignmentMoves());

  // Bifrost answers `unavailable` when the realm does not answer in time.
  // The realm may have made the ticket all the same: the join is lost, not
  // refused, and its leave is owed.
  TCapture<FRedwoodTicketingUpdate> Unanswered;
  H.Socket()->Refuse(
    H.SendJoinQueue(Unanswered),
    TEXT("unavailable"),
    TEXT("The realm did not answer.")
  );
  TestEqual(TEXT("An unavailable join is answered once"), Unanswered.Count, 1);
  TestEqual(
    TEXT("With the lost-reply error"),
    Unanswered.Last.Message,
    FRedwoodPendingReplies::LostReplyError
  );
  TestTrue(TEXT("It is dropped"), FAccess::IsQueueJoinAbandoned(*H.Client()));
  TestTrue(
    TEXT("Its leave is owed"), FAccess::IsLeaveTicketingOwed(*H.Client())
  );
  // The socket is up and the realm knows the player: the leave goes now,
  // not at a realm entry that may never come.
  const FPhoenixMessage PaidAtOnce = H.Socket()->Last(LeaveEvent);
  if (!TestTrue(
        TEXT("The owed leave goes out at once"),
        PaidAtOnce.IsValid() && PaidAtOnce.Ref != Leave.Ref
      )) {
    return false;
  }
  TestEqual(TEXT("As the second leave on this socket"), H.LeavesSent(), 2);
  H.Socket()->ReplyOk(PaidAtOnce, NoError());
  TestFalse(
    TEXT("A leave that ran is not owed"),
    FAccess::IsLeaveTicketingOwed(*H.Client())
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodZoneTravelLostProxyJoinsTest,
  "Redwood.Bifrost.ZoneTravel.LostProxyJoinsAreDropped",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodZoneTravelLostProxyJoinsTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostZoneTravelTest;
  ExpectDrop(*this, 3);
  ExpectIgnoredAssignments(*this, 3);
  FZoneHarness H(*this);
  H.EnterSessionWithCharacter();

  // FailTicketingJoin marks every ticketing join, not only the queue's.
  TCapture<FRedwoodTicketingUpdate> Custom;
  H.Client()->JoinCustom(false, TArray<FString>(), Custom.Delegate());
  TestTrue(
    TEXT("A custom join with no regions to ping goes out at once"),
    H.Socket()->Last(TEXT("realm:ticketing:join:custom")).IsValid()
  );
  H.Drop();
  TestEqual(TEXT("The lost custom join is answered once"), Custom.Count, 1);
  TestEqual(
    TEXT("With the lost-reply error"),
    Custom.Last.Message,
    FRedwoodPendingReplies::LostReplyError
  );
  TestTrue(TEXT("It is dropped"), FAccess::IsQueueJoinAbandoned(*H.Client()));
  TestTrue(
    TEXT("Its leave is owed"), FAccess::IsLeaveTicketingOwed(*H.Client())
  );
  H.ReopenAndResume();
  TestFalse(
    TEXT("A replay after the resume does not move the player"),
    H.AssignmentMoves()
  );

  TCapture<FRedwoodJoinServerOutput> ProxyJoin;
  H.Client()->JoinProxyWithSingleInstance(
    TEXT("proxy-ref"), FString(), ProxyJoin.Delegate()
  );
  TestTrue(
    TEXT("The proxy join goes out"),
    H.Socket()->Last(TEXT("realm:servers:join-proxy")).IsValid()
  );
  TestFalse(
    TEXT("A proxy join replaces the dropped join"),
    FAccess::IsQueueJoinAbandoned(*H.Client())
  );
  H.Drop();
  TestEqual(TEXT("The lost proxy join is answered once"), ProxyJoin.Count, 1);
  TestEqual(
    TEXT("With the lost-reply error"),
    ProxyJoin.Last.Error,
    FRedwoodPendingReplies::LostReplyError
  );
  TestTrue(
    TEXT("The lost proxy join is dropped"),
    FAccess::IsQueueJoinAbandoned(*H.Client())
  );
  H.ReopenAndResume();
  TestFalse(
    TEXT("A replay after a lost proxy join does not move the player"),
    H.AssignmentMoves()
  );

  TCapture<FRedwoodCreateProxyOutput> Create;
  H.Client()->CreateProxy(true, FRedwoodCreateProxyInput(), Create.Delegate());
  TestTrue(
    TEXT("The create goes out"),
    H.Socket()->Last(TEXT("realm:servers:create-proxy")).IsValid()
  );
  H.Drop();
  TestEqual(TEXT("The lost create is answered once"), Create.Count, 1);
  TestEqual(
    TEXT("With the lost-reply error"),
    Create.Last.Error,
    FRedwoodPendingReplies::LostReplyError
  );
  TestTrue(
    TEXT("The lost create that joins is dropped"),
    FAccess::IsQueueJoinAbandoned(*H.Client())
  );
  H.ReopenAndResume();
  TestFalse(
    TEXT("A replay after a lost create does not move the player"),
    H.AssignmentMoves()
  );

  // A proxy join is not a ticketing join: the realm can push its assignment
  // before it answers, so the guard opens at the send.
  TCapture<FRedwoodJoinServerOutput> Accepted;
  H.Client()->JoinProxyWithSingleInstance(
    TEXT("proxy-ref"), FString(), Accepted.Delegate()
  );
  TestFalse(
    TEXT("A proxy join replaces the dropped join at the send"),
    FAccess::IsQueueJoinAbandoned(*H.Client())
  );
  TestTrue(
    TEXT("Its assignment moves the player before the realm answers"),
    H.AssignmentMoves()
  );
  H.Socket()->ReplyOk(
    H.Socket()->Last(TEXT("realm:servers:join-proxy")), NoError()
  );
  TestEqual(TEXT("The proxy join is answered"), Accepted.Count, 1);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodZoneTravelRefusedJoinsTest,
  "Redwood.Bifrost.ZoneTravel.RefusedJoinsAndEndedTickets",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodZoneTravelRefusedJoinsTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostZoneTravelTest;
  ExpectIgnoredAssignments(*this, 4);
  FZoneHarness H(*this);
  H.EnterSessionWithCharacter();
  const FString Refusal = TEXT("refused");

  TCapture<FRedwoodTicketingUpdate> Update;
  H.Socket()->ReplyWithError(H.SendJoinQueue(Update), Refusal);
  TestEqual(TEXT("The refusal reaches the game"), Update.Count, 1);
  TestEqual(TEXT("As the join's response"), Update.Last.Message, Refusal);
  TestFalse(
    TEXT("A refused join expects no assignment"),
    FAccess::IsAssignmentExpected(*H.Client())
  );
  TestFalse(
    TEXT("A refused join owes no leave"),
    FAccess::IsLeaveTicketingOwed(*H.Client())
  );
  TestFalse(
    TEXT("After a refused join, a replay does not move the player"),
    H.AssignmentMoves()
  );

  TCapture<FRedwoodJoinServerOutput> ProxyJoin;
  H.Client()->JoinProxyWithSingleInstance(
    TEXT("proxy-ref"), FString(), ProxyJoin.Delegate()
  );
  H.Socket()->ReplyWithError(
    H.Socket()->Last(TEXT("realm:servers:join-proxy")), Refusal
  );
  TestEqual(
    TEXT("The proxy join's refusal reaches the game"), ProxyJoin.Count, 1
  );
  TestEqual(TEXT("With its error"), ProxyJoin.Last.Error, Refusal);
  TestFalse(
    TEXT("After a refused proxy join, a replay does not move the player"),
    H.AssignmentMoves()
  );

  TCapture<FRedwoodCreateProxyOutput> RefusedCreate;
  H.Client()->CreateProxy(
    true, FRedwoodCreateProxyInput(), RefusedCreate.Delegate()
  );
  H.Socket()->ReplyWithError(
    H.Socket()->Last(TEXT("realm:servers:create-proxy")), Refusal
  );
  TestEqual(
    TEXT("The create's refusal reaches the game"), RefusedCreate.Count, 1
  );
  TestFalse(
    TEXT(
      "After a refused create that joins, a replay does not move the player"
    ),
    H.AssignmentMoves()
  );

  // A create that joins its session is a join, and the realm can push its
  // assignment before it answers the create.
  TCapture<FRedwoodCreateProxyOutput> Create;
  H.Client()->CreateProxy(true, FRedwoodCreateProxyInput(), Create.Delegate());
  TestTrue(
    TEXT("Before the realm answers, the create's assignment moves the player"),
    H.AssignmentMoves()
  );
  TSharedPtr<FJsonObject> Created = NoError();
  Created->SetStringField(TEXT("proxyReference"), TEXT("proxy-ref"));
  H.Socket()->ReplyOk(
    H.Socket()->Last(TEXT("realm:servers:create-proxy")), Created
  );
  TestEqual(TEXT("The create is answered"), Create.Count, 1);
  TestEqual(
    TEXT("With its reference"), Create.Last.ProxyReference, TEXT("proxy-ref")
  );
  TestTrue(
    TEXT("The assignment of a create that joins moves the player"),
    H.AssignmentMoves()
  );

  // The server ends the ticket: no assignment comes for it.
  TCapture<FRedwoodTicketingUpdate> Ended;
  H.Socket()->ReplyOk(H.SendJoinQueue(Ended), NoError());
  H.PushTicketError(TEXT("ended"));
  TestEqual(
    TEXT("The join and the ticket error reach the game"), Ended.Count, 2
  );
  TestEqual(
    TEXT("As a ticket error"),
    Ended.Last.Type,
    ERedwoodTicketingUpdateType::TicketError
  );
  TestEqual(TEXT("With the realm's text"), Ended.Last.Message, TEXT("ended"));
  TestFalse(
    TEXT("An ended ticket expects no assignment"),
    FAccess::IsAssignmentExpected(*H.Client())
  );
  TestFalse(
    TEXT("An ended ticket owes no leave"),
    FAccess::IsLeaveTicketingOwed(*H.Client())
  );
  TestFalse(
    TEXT("After a ticket error, a replay does not move the player"),
    H.AssignmentMoves()
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodZoneTravelInWorldTest,
  "Redwood.Bifrost.ZoneTravel.InWorldKeepsZoneTransfers",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodZoneTravelInWorldTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostZoneTravelTest;
  ExpectDrop(*this);
  ExpectIgnoredAssignments(*this, 2);
  FZoneHarness H(*this);
  H.EnterSessionWithCharacter();

  // A client world that is not the travel of an accepted assignment (a PIE
  // client, a future lobby) must not open the guard.
  H.Client()->NoteArrivedInWorld();
  TestFalse(
    TEXT("An arrival with no travel is not the world"),
    FAccess::IsInWorld(*H.Client())
  );
  TestFalse(
    TEXT("It expects no assignment"), FAccess::IsAssignmentExpected(*H.Client())
  );
  TestFalse(TEXT("A replay does not move the player"), H.AssignmentMoves());

  H.JoinQueueOk();
  TestTrue(TEXT("The join's assignment moves the player"), H.AssignmentMoves());
  TestTrue(
    TEXT("Its travel is pending"), FAccess::IsTravelPending(*H.Client())
  );
  H.Client()->NoteArrivedInWorld();
  TestTrue(
    TEXT("The travel ended in the world"), FAccess::IsInWorld(*H.Client())
  );
  TestFalse(
    TEXT("No travel is pending"), FAccess::IsTravelPending(*H.Client())
  );

  TCapture<FRedwoodTicketingUpdate> Refused;
  H.Socket()->ReplyWithError(H.SendJoinQueue(Refused), TEXT("refused"));
  TestTrue(
    TEXT(
      "In the world, after a refused join, a zone transfer moves the player"
    ),
    H.AssignmentMoves()
  );
  TCapture<FRedwoodTicketingUpdate> Ended;
  H.Socket()->ReplyOk(H.SendJoinQueue(Ended), NoError());
  H.PushTicketError(TEXT("ended"));
  TestTrue(
    TEXT(
      "In the world, after a ticket error, a zone transfer moves the player"
    ),
    H.AssignmentMoves()
  );

  // A join lost in the world: the replay of its assignment could come, so
  // zone transfers wait. A join refused after it replaces the dropped join,
  // and the transfers go on.
  TCapture<FRedwoodTicketingUpdate> Lost;
  H.SendJoinQueue(Lost);
  H.Drop();
  H.ReopenAndResume();
  TestTrue(
    TEXT("A join lost in the world is dropped"),
    FAccess::IsQueueJoinAbandoned(*H.Client())
  );
  TestFalse(TEXT("A dropped join blocks a zone transfer"), H.AssignmentMoves());
  TCapture<FRedwoodTicketingUpdate> RefusedAgain;
  H.Socket()->ReplyWithError(H.SendJoinQueue(RefusedAgain), TEXT("refused"));
  TestFalse(
    TEXT("A join refused in the world replaces the dropped join"),
    FAccess::IsQueueJoinAbandoned(*H.Client())
  );
  TestTrue(TEXT("A zone transfer moves the player again"), H.AssignmentMoves());
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodZoneTravelOlderLostJoinTest,
  "Redwood.Bifrost.ZoneTravel.OlderLostJoinKeepsNewerJoin",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodZoneTravelOlderLostJoinTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostZoneTravelTest;
  ExpectDrop(*this);
  FZoneHarness H(*this);
  H.EnterSessionWithCharacter();

  TCapture<FRedwoodTicketingUpdate> Older;
  const FPhoenixMessage OlderJoin = H.SendJoinQueue(Older);
  TCapture<FRedwoodTicketingUpdate> Newer;
  const FPhoenixMessage NewerJoin = H.SendJoinQueue(Newer);
  TestEqual(TEXT("Two joins went out"), FAccess::JoinSequence(*H.Client()), 2u);
  TestNotEqual(TEXT("As two rpcs"), OlderJoin.Ref, NewerJoin.Ref);
  H.Socket()->ReplyOk(NewerJoin, NoError());
  TestEqual(TEXT("The newer join is answered"), Newer.Count, 1);
  TestTrue(TEXT("As accepted"), Newer.Last.Message.IsEmpty());

  // The older join's reply goes with the socket; the newer join owns the
  // flags, so its loss changes nothing.
  H.Drop();
  TestFalse(
    TEXT("The older join's loss does not drop the newer join"),
    FAccess::IsQueueJoinAbandoned(*H.Client())
  );
  TestFalse(
    TEXT("Nor owes a leave"), FAccess::IsLeaveTicketingOwed(*H.Client())
  );
  TestTrue(
    TEXT("The newer join still expects its assignment"),
    FAccess::IsAssignmentExpected(*H.Client())
  );
  H.ReopenAndResume();
  TestTrue(
    TEXT("After the resume, the newer join's assignment moves the player"),
    H.AssignmentMoves()
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodZoneTravelLeaveTest,
  "Redwood.Bifrost.ZoneTravel.LeaveTicketingDropsTheJoin",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodZoneTravelLeaveTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostZoneTravelTest;
  ExpectDrop(*this);
  ExpectIgnoredAssignments(*this, 2);
  FZoneHarness H(*this);
  H.EnterSessionWithCharacter();
  H.JoinQueueOk();

  // The game gives up on the join (its enter-world timer ran out).
  TCapture<FString> Left;
  const FPhoenixMessage Leave = H.SendLeave(Left);
  if (!TestTrue(TEXT("The leave goes out"), Leave.IsValid())) {
    return false;
  }
  TestEqual(
    TEXT("For the player"),
    Leave.Payload->GetObjectField(TEXT("payload"))
      ->GetStringField(TEXT("playerId")),
    PlayerId
  );
  TestEqual(TEXT("It waits for its reply"), Left.Count, 0);
  TestTrue(
    TEXT("The join is dropped"), FAccess::IsQueueJoinAbandoned(*H.Client())
  );
  TestFalse(
    TEXT("No assignment is expected"),
    FAccess::IsAssignmentExpected(*H.Client())
  );
  TestFalse(
    TEXT("A sent leave is not owed"), FAccess::IsLeaveTicketingOwed(*H.Client())
  );
  TestFalse(
    TEXT("A late assignment of the left join does not move the player"),
    H.AssignmentMoves()
  );
  H.Socket()->ReplyOk(Leave, NoError());
  TestEqual(TEXT("The leave is answered once"), Left.Count, 1);
  TestTrue(TEXT("With no error"), Left.Last.IsEmpty());

  // The timer race: the assignment is accepted, and the game's timer leaves
  // during its travel. The travel still arrives.
  H.JoinQueueOk();
  TestTrue(TEXT("The assignment moves the player"), H.AssignmentMoves());
  TestTrue(
    TEXT("Its travel is pending"), FAccess::IsTravelPending(*H.Client())
  );
  H.LeaveOk();
  TestTrue(
    TEXT("The leave keeps the travel pending"),
    FAccess::IsTravelPending(*H.Client())
  );
  TestFalse(
    TEXT(
      "Between the leave and the arrival, a replay does not move the player"
    ),
    H.AssignmentMoves()
  );
  H.Client()->NoteArrivedInWorld();
  TestTrue(
    TEXT("After the arrival, a zone transfer moves the player"),
    H.AssignmentMoves()
  );

  // A leave lost with the socket may not have run: it is owed.
  TCapture<FString> Lost;
  H.SendLeave(Lost);
  H.Drop();
  TestEqual(TEXT("The lost leave is answered once"), Lost.Count, 1);
  TestEqual(
    TEXT("With the lost-reply error"),
    Lost.Last,
    FRedwoodPendingReplies::LostReplyError
  );
  TestTrue(
    TEXT("A lost leave is owed"), FAccess::IsLeaveTicketingOwed(*H.Client())
  );

  // The resume brings the session back into the realm, which knows the
  // player again: the owed leave goes then.
  H.ReopenAndResume();
  const FPhoenixMessage Owed = H.Socket()->Last(LeaveEvent);
  if (!TestTrue(TEXT("The owed leave goes at the resume"), Owed.IsValid())) {
    return false;
  }
  TestEqual(
    TEXT("For the player"),
    Owed.Payload->GetObjectField(TEXT("payload"))
      ->GetStringField(TEXT("playerId")),
    PlayerId
  );
  TestEqual(TEXT("Once"), H.LeavesSent(), 1);
  H.Socket()->ReplyOk(Owed, NoError());
  TestFalse(
    TEXT("A leave that ran is not owed"),
    FAccess::IsLeaveTicketingOwed(*H.Client())
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodZoneTravelOwedLeaveTest,
  "Redwood.Bifrost.ZoneTravel.OwedLeaveWaitsForTheJoin",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodZoneTravelOwedLeaveTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostZoneTravelTest;
  ExpectDrop(*this);
  FZoneHarness H(*this);
  H.EnterSessionWithCharacter();

  // The game leaves, then joins again before the leave is answered; the
  // join is accepted and the leave goes with the socket. The owed leave must
  // not cancel the newer join's ticket.
  TCapture<FString> Left;
  H.SendLeave(Left);
  H.JoinQueueOk();
  H.Drop();
  TestEqual(
    TEXT("The lost leave is answered"),
    Left.Last,
    FRedwoodPendingReplies::LostReplyError
  );
  TestTrue(
    TEXT("The lost leave is owed"), FAccess::IsLeaveTicketingOwed(*H.Client())
  );
  TestTrue(
    TEXT("The newer join still expects its assignment"),
    FAccess::IsAssignmentExpected(*H.Client())
  );

  const FPhoenixMessage Enter = H.ReopenAndReenter();
  if (!TestTrue(TEXT("The client enters the realm again"), Enter.IsValid())) {
    return false;
  }
  H.RealmAcceptsReentry(Enter);
  TestEqual(
    TEXT("No owed leave goes out while the join is out"), H.LeavesSent(), 0
  );
  TestTrue(TEXT("It stays owed"), FAccess::IsLeaveTicketingOwed(*H.Client()));

  // The server ends the newer join's ticket: no join is out. The logout pays
  // the leave while the realm still knows the player, then ends the session.
  H.PushTicketError(TEXT("ended"));
  TestFalse(TEXT("No join is out"), FAccess::IsAssignmentExpected(*H.Client()));
  H.Client()->Logout();
  TestEqual(TEXT("The logout pays the owed leave"), H.LeavesSent(), 1);
  TestEqual(
    TEXT("For the player the session had"),
    H.Socket()
      ->Last(LeaveEvent)
      .Payload->GetObjectField(TEXT("payload"))
      ->GetStringField(TEXT("playerId")),
    PlayerId
  );
  TestTrue(
    TEXT("Before the logout step"),
    IndexOfLast(*H.Socket(), LeaveEvent) <
      IndexOfLast(*H.Socket(), TEXT("session.logout"))
  );
  TestFalse(
    TEXT("The next account owes no leave"),
    FAccess::IsLeaveTicketingOwed(*H.Client())
  );
  TestTrue(
    TEXT("The logout forgets the character"),
    FAccess::SelectedCharacterId(*H.Client()).IsEmpty()
  );
  TestFalse(
    TEXT("And the dropped join"), FAccess::IsQueueJoinAbandoned(*H.Client())
  );
  TestFalse(
    TEXT("And expects no assignment"),
    FAccess::IsAssignmentExpected(*H.Client())
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodZoneTravelOldTravelTest,
  "Redwood.Bifrost.ZoneTravel.OldTravelArrivesAfterNewJoin",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodZoneTravelOldTravelTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostZoneTravelTest;
  ExpectDrop(*this);
  ExpectIgnoredAssignments(*this, 2);
  FZoneHarness H(*this);
  H.EnterSessionWithCharacter();

  // The game's timer fires during the first travel; the player joins again
  // from character select, and that join's reply is lost. Then the OLD
  // travel arrives: the newer join's dropped assignment and its owed leave
  // must stay.
  H.JoinQueueOk();
  TestTrue(TEXT("The first assignment moves the player"), H.AssignmentMoves());
  H.LeaveOk();
  TCapture<FRedwoodTicketingUpdate> Newer;
  H.SendJoinQueue(Newer);
  H.Drop();
  TestTrue(
    TEXT("The newer join is dropped"),
    FAccess::IsQueueJoinAbandoned(*H.Client())
  );
  TestTrue(
    TEXT("Its leave is owed"), FAccess::IsLeaveTicketingOwed(*H.Client())
  );
  H.ReopenAndResume();
  H.Client()->NoteArrivedInWorld();
  TestTrue(
    TEXT("The old travel ends in the world"), FAccess::IsInWorld(*H.Client())
  );
  TestTrue(
    TEXT("It keeps the newer join dropped"),
    FAccess::IsQueueJoinAbandoned(*H.Client())
  );
  TestTrue(
    TEXT("It keeps the newer join's leave owed"),
    FAccess::IsLeaveTicketingOwed(*H.Client())
  );
  TestFalse(
    TEXT("The dropped join's assignment does not move the player in the world"),
    H.AssignmentMoves()
  );

  // Back at the entry level: character select, where nothing moves the
  // player. Then the same race, but the newer join was left and its leave
  // went through: when the old travel arrives, the player is in the world.
  H.Client()->NoteLeftWorld();
  TestFalse(TEXT("Leaving the world ends it"), FAccess::IsInWorld(*H.Client()));
  TestFalse(
    TEXT("At the entry level, no assignment is expected"),
    FAccess::IsAssignmentExpected(*H.Client())
  );
  H.JoinQueueOk();
  TestTrue(TEXT("The next assignment moves the player"), H.AssignmentMoves());
  H.JoinQueueOk();
  H.LeaveOk();
  TestFalse(
    TEXT("A clean leave owes nothing"),
    FAccess::IsLeaveTicketingOwed(*H.Client())
  );
  H.Client()->NoteArrivedInWorld();
  TestFalse(
    TEXT("After the old travel, the left join is no longer dropped"),
    FAccess::IsQueueJoinAbandoned(*H.Client())
  );
  TestTrue(TEXT("A zone transfer moves the player"), H.AssignmentMoves());
  H.Client()->NoteLeftWorld();
  TestFalse(
    TEXT("At the entry level, a replay does not move the player"),
    H.AssignmentMoves()
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodZoneTravelPartyReplaysTest,
  "Redwood.Bifrost.ZoneTravel.PartyTicketReplays",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodZoneTravelPartyReplaysTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostZoneTravelTest;
  ExpectIgnoredAssignments(*this, 9);
  FZoneHarness H(*this);
  // A member at character select, with no join of its own.
  H.EnterSessionWithCharacter();

  // A member relaunched its client: it heard no party notice, so the
  // server's replay of the party's assignment must not move it.
  TestFalse(
    TEXT("A replayed party assignment does not move a fresh member"),
    H.AssignmentMoves(CharacterId, TEXT("t1"))
  );
  TestFalse(
    TEXT("Nor one with no ticket"), H.AssignmentMoves(CharacterId, FString())
  );

  // The notice is this member's join, and the assignment uses the ticket up.
  H.PushPartyQueued(CharacterId, TEXT("t1"), TEXT("m1"));
  TestEqual(
    TEXT("The notice is accepted"),
    FAccess::AcceptedPartyTicketId(*H.Client()),
    TEXT("t1")
  );
  TestTrue(
    TEXT("The member expects the party's assignment"),
    FAccess::IsAssignmentExpected(*H.Client())
  );
  TestTrue(
    TEXT("The party's assignment moves the member"),
    H.AssignmentMoves(CharacterId, TEXT("t1"))
  );
  TestTrue(
    TEXT("The ticket is used up"),
    FAccess::AcceptedPartyTicketId(*H.Client()).IsEmpty()
  );
  H.PushPartyQueued(CharacterId, TEXT("t1"), TEXT("m2"));
  TestTrue(
    TEXT("A kept notice for the used ticket is not accepted again"),
    FAccess::AcceptedPartyTicketId(*H.Client()).IsEmpty()
  );
  TestFalse(
    TEXT("The replay of its assignment does not move the member again"),
    H.AssignmentMoves(CharacterId, TEXT("t1"))
  );

  // A repeat of the accepted ticket must not undo a leave the game made.
  H.PushPartyQueued(CharacterId, TEXT("t2"), TEXT("m3"));
  H.LeaveOk();
  H.PushPartyQueued(CharacterId, TEXT("t2"), TEXT("m4"));
  TestTrue(
    TEXT("A repeat of the accepted ticket keeps the dropped join"),
    FAccess::IsQueueJoinAbandoned(*H.Client())
  );
  TestFalse(
    TEXT("After the game left, the party's assignment does not move it"),
    H.AssignmentMoves(CharacterId, TEXT("t2"))
  );
  H.PushPartyQueued(CharacterId, TEXT("t3"), TEXT("m5"));
  // Redis sends the first notice again, then a kept notice of the replaced
  // ticket with a new id.
  H.PushPartyQueued(CharacterId, TEXT("t2"), TEXT("m3"));
  H.PushPartyQueued(CharacterId, TEXT("t2"), TEXT("m6"));
  TestEqual(
    TEXT("A replaced ticket does not come back"),
    FAccess::AcceptedPartyTicketId(*H.Client()),
    TEXT("t3")
  );
  TestTrue(
    TEXT("The newer ticket's assignment moves the member"),
    H.AssignmentMoves(CharacterId, TEXT("t3"))
  );
  H.Client()->NoteLeftWorld();

  H.PushPartyQueued(OtherCharacterId, TEXT("t4"), TEXT("m7"));
  TestFalse(
    TEXT("A notice for another character expects no assignment"),
    FAccess::IsAssignmentExpected(*H.Client())
  );
  TestFalse(
    TEXT("Its assignment does not move the member"),
    H.AssignmentMoves(CharacterId, TEXT("t4"))
  );

  H.PushPartyQueued(CharacterId, TEXT("t5"), TEXT("m8"));
  H.PushPartyLeft(TEXT("t5"), TEXT("m9"));
  TestFalse(
    TEXT("After the party left, no assignment is expected"),
    FAccess::IsAssignmentExpected(*H.Client())
  );
  TestFalse(
    TEXT("The left ticket's assignment does not move the member"),
    H.AssignmentMoves(CharacterId, TEXT("t5"))
  );

  // A late "left" of an older ticket keeps the newer one.
  H.PushPartyQueued(CharacterId, TEXT("t6"), TEXT("m10"));
  H.PushPartyQueued(CharacterId, TEXT("t7"), TEXT("m11"));
  H.PushPartyLeft(TEXT("t6"), TEXT("m12"));
  TestTrue(
    TEXT("The newer ticket's assignment moves the member"),
    H.AssignmentMoves(CharacterId, TEXT("t7"))
  );
  H.Client()->NoteLeftWorld();

  // A stale "queued" after its ticket ended.
  H.PushPartyLeft(TEXT("t8"), TEXT("m13"));
  H.PushPartyQueued(CharacterId, TEXT("t8"), TEXT("m14"));
  TestFalse(
    TEXT("A notice for an ended ticket expects no assignment"),
    FAccess::IsAssignmentExpected(*H.Client())
  );
  TestFalse(
    TEXT("An ended ticket's assignment does not move the member"),
    H.AssignmentMoves(CharacterId, TEXT("t8"))
  );

  // A member that waits for its party takes only that ticket's assignment;
  // in the world, it takes a zone transfer with no ticket.
  H.PushPartyQueued(CharacterId, TEXT("t9"), TEXT("m15"));
  TestFalse(
    TEXT("Another ticket's assignment does not move the member"),
    H.AssignmentMoves(CharacterId, TEXT("t10"))
  );
  TestFalse(
    TEXT("At character select, one with no ticket does not either"),
    H.AssignmentMoves(CharacterId, FString())
  );
  TestTrue(
    TEXT("The accepted ticket's assignment moves the member"),
    H.AssignmentMoves(CharacterId, TEXT("t9"))
  );
  H.Client()->NoteArrivedInWorld();
  TestTrue(
    TEXT("In the world, a zone transfer with no ticket moves the member"),
    H.AssignmentMoves(CharacterId, FString())
  );

  // The next login can be another account, whose party is not ours.
  H.PushPartyQueued(CharacterId, TEXT("t11"), TEXT("m16"));
  TestEqual(
    TEXT("A notice in the world is accepted"),
    FAccess::AcceptedPartyTicketId(*H.Client()),
    TEXT("t11")
  );
  H.Client()->Logout();
  TestTrue(
    TEXT("The logout forgets the party ticket"),
    FAccess::AcceptedPartyTicketId(*H.Client()).IsEmpty()
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodZoneTravelPartyOwnJoinTest,
  "Redwood.Bifrost.ZoneTravel.PartyTicketAndOwnJoin",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodZoneTravelPartyOwnJoinTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostZoneTravelTest;
  ExpectDrop(*this);
  FZoneHarness H(*this);
  H.EnterSessionWithCharacter();

  // The member's own join is out when the party ticket comes: its own
  // ticket is left, or its solo assignment could move the player after the
  // party's travel.
  H.JoinQueueOk();
  H.PushPartyQueued(CharacterId, TEXT("t1"), TEXT("m1"));
  const FPhoenixMessage OwnLeave = H.Socket()->Last(LeaveEvent);
  if (!TestTrue(
        TEXT("The party notice leaves the member's own ticket"),
        OwnLeave.IsValid()
      )) {
    return false;
  }
  TestEqual(
    TEXT("The notice is accepted"),
    FAccess::AcceptedPartyTicketId(*H.Client()),
    TEXT("t1")
  );
  TestTrue(
    TEXT("The leave is owed until the realm answers"),
    FAccess::IsLeaveTicketingOwed(*H.Client())
  );
  H.Socket()->ReplyOk(OwnLeave, NoError());
  TestFalse(
    TEXT("A leave that ran is not owed"),
    FAccess::IsLeaveTicketingOwed(*H.Client())
  );

  // The own join can still wait for its answer when the notice comes: the
  // server has its ticket, or makes it before it sees the leave, so the
  // ticket is left all the same. At a resume the realm replays the party's
  // assignment before the notice: it waits with the join, and the notice,
  // which names its ticket, moves the member with it.
  TCapture<FRedwoodTicketingUpdate> Waiting;
  const FPhoenixMessage WaitingJoin = H.SendJoinQueue(Waiting);
  TestTrue(
    TEXT("The own join waits for its answer"),
    FAccess::IsJoinAwaitingReply(*H.Client())
  );
  TestFalse(
    TEXT("The party's assignment before the notice waits"),
    H.AssignmentMoves(CharacterId, TEXT("t2"))
  );
  const int32 BeforeNotice = H.Joins->Count;
  H.PushPartyQueued(CharacterId, TEXT("t2"), TEXT("m2"));
  TestEqual(
    TEXT("The notice moves the member with the assignment that waited"),
    H.Joins->Count,
    BeforeNotice + 1
  );
  TestTrue(
    TEXT("Which used the ticket up"),
    FAccess::AcceptedPartyTicketId(*H.Client()).IsEmpty()
  );
  const FPhoenixMessage WaitingLeave = H.Socket()->Last(LeaveEvent);
  if (!TestTrue(
        TEXT(
          "The notice leaves the ticket of a join that waits for its answer"
        ),
        WaitingLeave.IsValid() && WaitingLeave.Ref != OwnLeave.Ref
      )) {
    return false;
  }
  TestEqual(TEXT("Two leaves went out"), H.LeavesSent(), 2);
  H.Socket()->ReplyOk(WaitingJoin, NoError());
  TestEqual(
    TEXT("The replaced join's late acceptance moves nothing"),
    H.Joins->Count,
    BeforeNotice + 1
  );
  TestEqual(
    TEXT("And the travel had answered the game already"), Waiting.Count, 0
  );
  H.Socket()->ReplyOk(WaitingLeave, NoError());
  TestFalse(TEXT("The leave ran"), FAccess::IsLeaveTicketingOwed(*H.Client()));

  // The player's own join replaces the notice: a solo assignment has no
  // ticket, and a leader's names a ticket this client was never told of.
  H.JoinQueueOk();
  TestTrue(
    TEXT("The own join forgets the party ticket"),
    FAccess::AcceptedPartyTicketId(*H.Client()).IsEmpty()
  );
  TestTrue(
    TEXT("After its own join, a solo assignment moves the player"),
    H.AssignmentMoves(CharacterId, FString())
  );
  TCapture<FRedwoodTicketingUpdate> WholeParty;
  H.Socket()->ReplyOk(H.SendJoinQueue(WholeParty, true), NoError());
  TestTrue(
    TEXT("After its own whole-party join, the leader's assignment moves it"),
    H.AssignmentMoves(CharacterId, TEXT("t9"))
  );

  // A member drops a join, then picks another character: the party's
  // assignment for the new character moves it, and the old ticket's leave is
  // still owed and goes with the notice.
  TCapture<FRedwoodTicketingUpdate> Lost;
  H.SendJoinQueue(Lost);
  H.Drop();
  TestTrue(
    TEXT("The lost join is dropped"), FAccess::IsQueueJoinAbandoned(*H.Client())
  );
  H.ReopenAndResume();
  const FPhoenixMessage Paid = H.Socket()->Last(LeaveEvent);
  if (!TestTrue(TEXT("The resume pays the owed leave"), Paid.IsValid())) {
    return false;
  }
  // The realm refuses it: the ticket is still the old character's.
  H.Socket()->ReplyWithError(Paid, TEXT("Leave failed."));
  H.Client()->SetSelectedCharacter(OtherCharacterId);
  H.Socket()->ReplyOk(
    H.Socket()->Last(TEXT("session.select-character")), NoError()
  );
  TestFalse(
    TEXT("A new character clears the dropped join"),
    FAccess::IsQueueJoinAbandoned(*H.Client())
  );
  TestTrue(
    TEXT("The old ticket's leave is still owed"),
    FAccess::IsLeaveTicketingOwed(*H.Client())
  );
  H.PushPartyQueued(OtherCharacterId, TEXT("t3"), TEXT("m3"));
  TestEqual(
    TEXT("The party notice sends the owed leave again"), H.LeavesSent(), 2
  );
  TestTrue(
    TEXT("The notice keeps the leave owed until the realm answers"),
    FAccess::IsLeaveTicketingOwed(*H.Client())
  );
  TestTrue(
    TEXT("The party's assignment for the new character moves the member"),
    H.AssignmentMoves(OtherCharacterId, TEXT("t3"))
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodZoneTravelPartyBeforeSelectTest,
  "Redwood.Bifrost.ZoneTravel.PartyQueuedBeforeCharacterSelect",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodZoneTravelPartyBeforeSelectTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostZoneTravelTest;
  ExpectIgnoredAssignments(*this, 1);
  FZoneHarness H(*this);
  H.Open();
  H.LoginOk();
  H.EnterRealmOk();

  // The server replays the kept "queued" at the realm entry, before the
  // relaunched member selects a character.
  H.PushPartyQueued(CharacterId, TEXT("t1"), TEXT("m1"));
  TestEqual(
    TEXT("The notice waits for its character"),
    FAccess::PendingPartyTicketId(*H.Client()),
    TEXT("t1")
  );
  TestTrue(
    TEXT("It is not accepted yet"),
    FAccess::AcceptedPartyTicketId(*H.Client()).IsEmpty()
  );
  TestFalse(
    TEXT("With no character, the party's assignment does not move anyone"),
    H.AssignmentMoves(CharacterId, TEXT("t1"))
  );

  H.Client()->SetSelectedCharacter(OtherCharacterId);
  H.Socket()->ReplyOk(
    H.Socket()->Last(TEXT("session.select-character")), NoError()
  );
  TestFalse(
    TEXT("Another character does not take the held notice"),
    FAccess::IsAssignmentExpected(*H.Client())
  );
  TestEqual(
    TEXT("The notice still waits"),
    FAccess::PendingPartyTicketId(*H.Client()),
    TEXT("t1")
  );

  H.Client()->SetSelectedCharacter(CharacterId);
  H.Socket()->ReplyOk(
    H.Socket()->Last(TEXT("session.select-character")), NoError()
  );
  TestEqual(
    TEXT("Selecting the notice's character accepts it"),
    FAccess::AcceptedPartyTicketId(*H.Client()),
    TEXT("t1")
  );
  TestTrue(
    TEXT("The notice no longer waits"),
    FAccess::PendingPartyTicketId(*H.Client()).IsEmpty()
  );
  TestTrue(
    TEXT("The party's assignment moves the member"),
    H.AssignmentMoves(CharacterId, TEXT("t1"))
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodZoneTravelJoinHeldTest,
  "Redwood.Bifrost.ZoneTravel.JoinHeldDuringTheDrop",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodZoneTravelJoinHeldTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostZoneTravelTest;
  ExpectDrop(*this, 2);
  FZoneHarness H(*this);
  H.EnterSessionWithCharacter();

  // The player presses Enter World during the drop: the proxy list, then
  // the join.
  H.Drop();
  TCapture<FRedwoodListProxiesOutput> Proxies;
  H.Client()->ListProxies(TArray<FString>(), Proxies.Delegate());
  TestEqual(TEXT("The proxy list is held"), H.HeldCount(), 1);
  TestEqual(TEXT("And not answered"), Proxies.Count, 0);
  TCapture<FRedwoodTicketingUpdate> Update;
  TestFalse(
    TEXT("A join during the drop sends nothing"),
    H.SendJoinQueue(Update).IsValid()
  );
  TestEqual(TEXT("It is held behind the list"), H.HeldCount(), 2);
  TestEqual(TEXT("And not answered"), Update.Count, 0);
  TestFalse(
    TEXT("No join is out yet"), FAccess::IsAssignmentExpected(*H.Client())
  );
  TestEqual(TEXT("None was counted"), FAccess::JoinSequence(*H.Client()), 0u);

  H.ReopenAndResume();
  const FPhoenixMessage List =
    H.Socket()->Last(TEXT("realm:servers:list-proxies"));
  if (!TestTrue(
        TEXT("The held list goes out after the resume"), List.IsValid()
      )) {
    return false;
  }
  TSharedPtr<FJsonObject> Listed = NoError();
  Listed->SetArrayField(TEXT("proxies"), TArray<TSharedPtr<FJsonValue>>());
  H.Socket()->ReplyOk(List, Listed);
  TestEqual(TEXT("The list's answer reaches the game"), Proxies.Count, 1);
  TestTrue(TEXT("With no error"), Proxies.Last.Error.IsEmpty());
  const FPhoenixMessage Join = H.Socket()->Last(JoinQueueEvent);
  if (!TestTrue(
        TEXT("The held join goes out after the resume"), Join.IsValid()
      )) {
    return false;
  }
  TestTrue(
    TEXT("After the list, as the game sent them"),
    IndexOfLast(*H.Socket(), TEXT("realm:servers:list-proxies")) <
      IndexOfLast(*H.Socket(), JoinQueueEvent)
  );
  const TSharedPtr<FJsonObject> Payload =
    Join.Payload->GetObjectField(TEXT("payload"));
  TestEqual(
    TEXT("For the selected character"),
    Payload->GetStringField(TEXT("characterId")),
    CharacterId
  );
  TestEqual(
    TEXT("For the player"), Payload->GetStringField(TEXT("playerId")), PlayerId
  );
  TestEqual(TEXT("Nothing is held"), H.HeldCount(), 0);
  TestTrue(TEXT("The join is out"), FAccess::IsJoinAwaitingReply(*H.Client()));
  TestFalse(
    TEXT("It expects no assignment before the realm answers"),
    FAccess::IsAssignmentExpected(*H.Client())
  );
  TestEqual(TEXT("It was counted"), FAccess::JoinSequence(*H.Client()), 1u);
  TestEqual(TEXT("Still not answered"), Update.Count, 0);
  H.Socket()->ReplyOk(Join, NoError());
  TestTrue(
    TEXT("The realm's answer opens the guard"),
    FAccess::IsAssignmentExpected(*H.Client())
  );
  TestEqual(TEXT("The realm's answer reaches the game"), Update.Count, 1);
  TestEqual(
    TEXT("As the join's response"),
    Update.Last.Type,
    ERedwoodTicketingUpdateType::JoinResponse
  );
  TestTrue(TEXT("Accepted"), Update.Last.Message.IsEmpty());
  TestTrue(TEXT("Its assignment moves the player"), H.AssignmentMoves());

  // A matchmaking join takes the same gate on its attempt.
  H.Drop();
  TCapture<FRedwoodTicketingUpdate> Matchmaking;
  H.Client()->JoinMatchmaking(
    TEXT("profile-1"), TArray<FString>(), Matchmaking.Delegate()
  );
  TestEqual(
    TEXT("A matchmaking join during the drop is held"), H.HeldCount(), 1
  );
  TestEqual(TEXT("And not answered"), Matchmaking.Count, 0);
  H.ReopenAndResume();
  const FPhoenixMessage MatchmakingJoin =
    H.Socket()->Last(TEXT("realm:ticketing:join:matchmaking"));
  if (!TestTrue(
        TEXT("The held matchmaking join goes out after the resume"),
        MatchmakingJoin.IsValid()
      )) {
    return false;
  }
  TestEqual(
    TEXT("With its profile"),
    MatchmakingJoin.Payload->GetObjectField(TEXT("payload"))
      ->GetObjectField(TEXT("data"))
      ->GetStringField(TEXT("profileId")),
    TEXT("profile-1")
  );
  // A matchmaking join names no ticket, in its answer and in its
  // assignment; the assignment that beats the answer waits for it.
  TestFalse(
    TEXT("Its assignment before the answer does not move the player yet"),
    H.AssignmentMoves()
  );
  TestEqual(TEXT("It waits"), FAccess::HeldAssignmentCount(*H.Client()), 1);
  const int32 BeforeMatchmaking = H.Joins->Count;
  H.Socket()->ReplyOk(MatchmakingJoin, NoError());
  TestEqual(TEXT("Its answer reaches the game"), Matchmaking.Count, 1);
  TestEqual(
    TEXT("And the assignment that waited moves the player"),
    H.Joins->Count,
    BeforeMatchmaking + 1
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodZoneTravelReplayBeforeReplyTest,
  "Redwood.Bifrost.ZoneTravel.ReplayBeforeTheJoinsReply",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodZoneTravelReplayBeforeReplyTest::RunTest(
  const FString &Parameters
) {
  using namespace RedwoodBifrostZoneTravelTest;
  ExpectDrop(*this);
  ExpectIgnoredAssignments(*this, 3);
  FZoneHarness H(*this);
  H.EnterSessionWithCharacter();

  // The realm pushes a ticket's assignment before it answers the join, so
  // the push waits for the answer, which names the join's ticket. Each
  // ticket's push waits; the answer picks the join's own, and a replay that
  // came before it is let go.
  TCapture<FRedwoodTicketingUpdate> First;
  const FPhoenixMessage FirstJoin = H.SendJoinQueue(First);
  TestTrue(
    TEXT("A join that is out waits for its answer"),
    FAccess::IsJoinAwaitingReply(*H.Client())
  );
  TestFalse(
    TEXT("It expects no assignment yet"),
    FAccess::IsAssignmentExpected(*H.Client())
  );
  TestFalse(
    TEXT("A replay before the answer does not move the player"),
    H.AssignmentMoves(CharacterId, TEXT("t-old"))
  );
  TestFalse(
    TEXT("Nor does the join's own assignment yet"),
    H.AssignmentMoves(CharacterId, TEXT("t1"))
  );
  TestEqual(
    TEXT("Both wait for the answer"),
    FAccess::HeldAssignmentCount(*H.Client()),
    2
  );
  TestEqual(TEXT("The game saw no response"), First.Count, 0);
  const int32 Before = H.Joins->Count;
  H.Socket()->ReplyOk(FirstJoin, AcceptedJoin(TEXT("t1")));
  TestEqual(TEXT("The answer reaches the game"), First.Count, 1);
  TestTrue(TEXT("Accepted"), First.Last.Message.IsEmpty());
  TestEqual(
    TEXT("And the join's own assignment moves the player, once"),
    H.Joins->Count,
    Before + 1
  );
  TestEqual(
    TEXT("Nothing waits"), FAccess::HeldAssignmentCount(*H.Client()), 0
  );
  TestEqual(
    TEXT("The ticket stays current through the travel"),
    FAccess::OwnTicketId(*H.Client()),
    TEXT("t1")
  );
  H.Client()->NoteArrivedInWorld();
  TestTrue(
    TEXT("The arrival uses it up"), FAccess::OwnTicketId(*H.Client()).IsEmpty()
  );
  H.Client()->NoteLeftWorld();

  // HollowedOath#3609. The join is lost with the socket, and the player
  // presses Enter World during the drop. At the resume the owed leave goes
  // out, then the held join; the server pushes the new ticket's assignment
  // and replays the lost ticket's, in either order, before it answers the
  // new join.
  TCapture<FRedwoodTicketingUpdate> Lost;
  H.SendJoinQueue(Lost);
  H.Drop();
  TestTrue(
    TEXT("The lost join is dropped"), FAccess::IsQueueJoinAbandoned(*H.Client())
  );
  TCapture<FRedwoodTicketingUpdate> Held;
  H.SendJoinQueue(Held);
  TestEqual(TEXT("A join during the drop is held"), H.HeldCount(), 1);
  TestEqual(TEXT("And not answered"), Held.Count, 0);
  H.ReopenAndResume();
  const FPhoenixMessage Join = H.Socket()->Last(JoinQueueEvent);
  if (!TestTrue(TEXT("The held join goes out at the resume"), Join.IsValid())) {
    return false;
  }
  TestEqual(TEXT("After the owed leave"), H.LeavesSent(), 1);
  TestTrue(
    TEXT("Which went first"),
    IndexOfLast(*H.Socket(), LeaveEvent) <
      IndexOfLast(*H.Socket(), JoinQueueEvent)
  );
  TestTrue(
    TEXT("The held join waits for its answer"),
    FAccess::IsJoinAwaitingReply(*H.Client())
  );
  TestTrue(
    TEXT("The lost join stays dropped until then"),
    FAccess::IsQueueJoinAbandoned(*H.Client())
  );
  TestTrue(
    TEXT("And its leave stays owed"), FAccess::IsLeaveTicketingOwed(*H.Client())
  );
  TestFalse(
    TEXT("The new ticket's assignment does not move the player yet"),
    H.AssignmentMoves(CharacterId, TEXT("t2"))
  );
  TestFalse(
    TEXT("Nor does the replay of the lost ticket's, which came after it"),
    H.AssignmentMoves(CharacterId, TEXT("t-lost"))
  );
  TestEqual(
    TEXT("Both wait for the answer"),
    FAccess::HeldAssignmentCount(*H.Client()),
    2
  );
  H.Socket()->ReplyOk(Join, AcceptedJoin(TEXT("t2")));
  TestEqual(TEXT("The realm's answer reaches the game"), Held.Count, 1);
  TestTrue(TEXT("Accepted"), Held.Last.Message.IsEmpty());
  TestEqual(
    TEXT("The new ticket's assignment moves the player; the replay is let go"),
    H.Joins->Count,
    Before + 2
  );
  TestEqual(
    TEXT("Nothing waits"), FAccess::HeldAssignmentCount(*H.Client()), 0
  );
  TestFalse(
    TEXT("The accepted join replaces the dropped join"),
    FAccess::IsQueueJoinAbandoned(*H.Client())
  );
  TestFalse(
    TEXT("And pays its owed leave"), FAccess::IsLeaveTicketingOwed(*H.Client())
  );
  TestEqual(
    TEXT("Its ticket is the current one"),
    FAccess::OwnTicketId(*H.Client()),
    TEXT("t2")
  );
  TestFalse(
    TEXT("A replay after the answer does not move the player either"),
    H.AssignmentMoves(CharacterId, TEXT("t-lost"))
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodZoneTravelJoinGraceTest,
  "Redwood.Bifrost.ZoneTravel.JoinFailsWhenTheGraceRunsOut",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodZoneTravelJoinGraceTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostZoneTravelTest;
  ExpectDrop(*this);
  FZoneHarness H(*this);
  H.EnterSessionWithCharacter();
  H.Drop();
  TCapture<FRedwoodTicketingUpdate> Update;
  H.SendJoinQueue(Update);
  TestEqual(TEXT("The join is held"), H.HeldCount(), 1);

  // The backoff opens new sockets, but no server accepts one.
  H.Advance(PastGraceSeconds);
  TestEqual(
    TEXT("The held join is answered when the grace ends"), Update.Count, 1
  );
  TestEqual(
    TEXT("As a ticket error"),
    Update.Last.Type,
    ERedwoodTicketingUpdateType::TicketError
  );
  TestEqual(
    TEXT("With the not-connected error"),
    Update.Last.Message,
    NotConnectedToRealm
  );
  TestEqual(TEXT("Nothing is held"), H.HeldCount(), 0);
  TestEqual(TEXT("Nothing went out"), H.Socket()->CountSent(JoinQueueEvent), 0);
  TestFalse(TEXT("No join is out"), FAccess::IsAssignmentExpected(*H.Client()));
  TestEqual(TEXT("None was counted"), FAccess::JoinSequence(*H.Client()), 0u);

  // After the grace, a join fails at once, whichever way it is made.
  TCapture<FRedwoodTicketingUpdate> Later;
  H.SendJoinQueue(Later);
  TestEqual(TEXT("A later join fails at once"), Later.Count, 1);
  TestEqual(
    TEXT("As a ticket error"),
    Later.Last.Type,
    ERedwoodTicketingUpdateType::TicketError
  );
  TestEqual(
    TEXT("With the not-connected error"),
    Later.Last.Message,
    NotConnectedToRealm
  );
  TCapture<FRedwoodTicketingUpdate> Matchmaking;
  H.Client()->JoinMatchmaking(
    TEXT("profile-1"), TArray<FString>(), Matchmaking.Delegate()
  );
  TestEqual(
    TEXT("A later matchmaking join fails at once"), Matchmaking.Count, 1
  );
  TestEqual(
    TEXT("As a ticket error"),
    Matchmaking.Last.Type,
    ERedwoodTicketingUpdateType::TicketError
  );
  TestEqual(TEXT("Neither is held"), H.HeldCount(), 0);

  // The game's leave cannot go out either: it is owed.
  TCapture<FString> Left;
  H.Client()->LeaveTicketing(Left.Delegate());
  TestEqual(TEXT("A later leave fails at once"), Left.Count, 1);
  TestEqual(
    TEXT("With the not-connected error"), Left.Last, NotConnectedToRealm
  );
  TestTrue(
    TEXT("The leave is owed"), FAccess::IsLeaveTicketingOwed(*H.Client())
  );

  // The server comes back after all, and the session resumes: the realm
  // knows the player again, and the owed leave goes.
  H.Accept();
  H.ResumeOk();
  const FPhoenixMessage Owed = H.Socket()->Last(LeaveEvent);
  if (!TestTrue(TEXT("The owed leave goes at the resume"), Owed.IsValid())) {
    return false;
  }
  TestEqual(
    TEXT("For the player"),
    Owed.Payload->GetObjectField(TEXT("payload"))
      ->GetStringField(TEXT("playerId")),
    PlayerId
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodZoneTravelLeaveHeldTest,
  "Redwood.Bifrost.ZoneTravel.LeaveHeldDuringTheDrop",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodZoneTravelLeaveHeldTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostZoneTravelTest;
  ExpectDrop(*this, 2);
  FZoneHarness H(*this);
  H.EnterSessionWithCharacter();

  // The game's enter-world timer runs out during the drop.
  H.JoinQueueOk();
  H.Drop();
  TCapture<FString> Left;
  TestFalse(
    TEXT("A leave during the drop sends nothing"), H.SendLeave(Left).IsValid()
  );
  TestEqual(TEXT("It is held"), H.HeldCount(), 1);
  TestEqual(TEXT("And not answered"), Left.Count, 0);
  TestTrue(
    TEXT("The join is dropped at once"),
    FAccess::IsQueueJoinAbandoned(*H.Client())
  );
  TestFalse(
    TEXT("No assignment is expected"),
    FAccess::IsAssignmentExpected(*H.Client())
  );
  TestFalse(
    TEXT("A held leave is not owed: it goes out itself"),
    FAccess::IsLeaveTicketingOwed(*H.Client())
  );
  H.ReopenAndResume();
  const FPhoenixMessage Leave = H.Socket()->Last(LeaveEvent);
  if (!TestTrue(
        TEXT("The held leave goes out after the resume"), Leave.IsValid()
      )) {
    return false;
  }
  TestEqual(TEXT("Nothing is held"), H.HeldCount(), 0);
  H.Socket()->ReplyOk(Leave, NoError());
  TestEqual(TEXT("The realm's answer reaches the game"), Left.Count, 1);
  TestTrue(TEXT("With no error"), Left.Last.IsEmpty());

  // A join lost with the socket owes a leave; the game's leave in the same
  // gap pays it. One leave goes out at the realm entry, not two.
  TCapture<FRedwoodTicketingUpdate> Lost;
  H.SendJoinQueue(Lost);
  H.Drop();
  TestTrue(
    TEXT("The lost join owes a leave"),
    FAccess::IsLeaveTicketingOwed(*H.Client())
  );
  TCapture<FString> Again;
  H.SendLeave(Again);
  TestFalse(
    TEXT("The held leave takes over what was owed"),
    FAccess::IsLeaveTicketingOwed(*H.Client())
  );
  const FPhoenixMessage Enter = H.ReopenAndReenter();
  if (!TestTrue(TEXT("The client enters the realm again"), Enter.IsValid())) {
    return false;
  }
  H.RealmAcceptsReentry(Enter);
  TestEqual(TEXT("One leave goes out at the entry"), H.LeavesSent(), 1);
  TestEqual(TEXT("Nothing is held"), H.HeldCount(), 0);
  H.Socket()->ReplyOk(H.Socket()->Last(LeaveEvent), NoError());
  TestEqual(TEXT("The game's leave is answered"), Again.Count, 1);
  TestTrue(TEXT("With no error"), Again.Last.IsEmpty());
  TestFalse(
    TEXT("Nothing is owed"), FAccess::IsLeaveTicketingOwed(*H.Client())
  );
  return true;
}
