// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.

#pragma once

#include "RedwoodCloseBackoff.h"
#include "RedwoodPendingReplies.h"

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "TimerManager.h"

class IWebSocket;

// A chat line as Bifrost delivers it (`chat.msg`) and as a `chat.resume`
// reply replays it. Seq orders the lines of one topic; MsgId is the sender's
// id of the line, the same on every delivery of it.
struct REDWOOD_API FRedwoodChatMessage {
  FString Topic;
  int64 Seq = 0;
  FString MsgId;
  FString Category;
  FString SenderCharacterId;
  FString SenderName;
  FString Template;
  TArray<FString> Args;
  // The contract sends an ISO 8601 date-time string.
  FDateTime SentAt;

  // From the payload of `chat.msg`, or one element of a resume's `messages`.
  // False when the topic, the seq or the msgId is missing: a line cannot be
  // ordered or deduplicated without them.
  static bool Parse(
    const TSharedPtr<FJsonObject> &Object, FRedwoodChatMessage &OutMessage
  );
};

// The last ids seen, so a delivery sent again is dropped: a push a resume
// sends again, a chat line a resume replays. Fixed size, because what a
// session can see again is recent; an id that fell out is treated as new.
class REDWOOD_API FRedwoodRecentIds {
public:
  explicit FRedwoodRecentIds(int32 Capacity) {
    Ids.SetNum(Capacity);
  }

  bool Contains(const FString &Id) const {
    return Ids.Contains(Id);
  }

  void Add(const FString &Id) {
    Ids[NextSlot] = Id;
    NextSlot = (NextSlot + 1) % Ids.Num();
  }

  void Reset() {
    for (FString &Id : Ids) {
      Id.Reset();
    }
    NextSlot = 0;
  }

private:
  TArray<FString> Ids;
  int32 NextSlot = 0;
};

// The one player connection: a WebSocket to Bifrost, which carries the
// session, every request to the Redwood director and realm, every push
// from them, and the chat. It speaks the Phoenix Channels protocol (v2 JSON)
// on one channel, "session". The contract is HollowedOath-Services
// services/bifrost/contract/protocol.md.
//
// It owns the socket's life: a drop it did not ask for is followed by a
// reconnect after a backoff, and a session it holds is resumed on the new
// socket, so Redwood sees no drop. The owner hears the outcome through the
// callbacks below. Every callback runs on the game thread, where the
// engine's WebSocket delegates fire.
class REDWOOD_API FRedwoodBifrostConnection
  : public TSharedFromThis<FRedwoodBifrostConnection> {
public:
  using FEventHandler = TFunction<
    void(const FString &Event, const TSharedPtr<FJsonValue> &Message)>;
  // bOk is the join's status. Response is the reply's response object: on a
  // refusal, {reason, message?}.
  using FJoinCallback =
    TFunction<void(bool bOk, const TSharedPtr<FJsonObject> &Response)>;

  // The contract version this client speaks.
  static constexpr int32 ContractVersion = 1;

  // The time between heartbeats, and the time a heartbeat's reply may take:
  // Bifrost closes a socket that is quiet for 60 s, and a reply that does
  // not come by the next heartbeat means the socket is dead with no close.
  // This is a keepalive timer, not a poll of game state.
  static constexpr float HeartbeatSeconds = 30.0f;

  // RFC 6455 close codes: the normal close, and the private-use code of a
  // close this client made because a heartbeat went unanswered.
  static constexpr int32 NormalCloseCode = 1000;
  static constexpr int32 HeartbeatCloseCode = 4000;

  // How many push ids are remembered: a push sent again after a resume is
  // dropped if its id was seen.
  static constexpr int32 RememberedPushIds = 256;
  // How many chat msgIds are remembered. The per-topic cursor drops most
  // replays; the ids catch a line delivered again under a seq the cursor
  // has not reached (a resume's replay that overlaps a live delivery).
  static constexpr int32 RememberedChatMsgIds = 256;

  explicit FRedwoodBifrostConnection(FTimerManager &InTimerManager);
  ~FRedwoodBifrostConnection();

  // Opens a socket to the Bifrost of `BaseUri` (ws[s]://host[:port]). After
  // a drop the socket is opened again by itself.
  void Connect(const FString &BaseUri);

  // A close this client asks for. Nothing reconnects after it.
  void Close();

  // The socket is open.
  bool IsConnected() const;

  // The player has a session on this connection: a login or a resume was
  // accepted, and no `closed` has ended it.
  bool HasSession() const {
    return !SessionId.IsEmpty();
  }

  // The session channel is joined: the join was answered and no later join
  // is waiting. A step sent before that reaches no channel.
  bool IsJoined() const {
    return !JoinRef.IsEmpty() && PendingJoinRef.IsEmpty();
  }

  // Joins the session channel with a login. `Login` is {provider, username,
  // secret}. The reply is the join reply of the contract.
  void Login(const TSharedPtr<FJsonObject> &Login, FJoinCallback Callback);

  // Drops the session on this side: no resume is tried after it. For a
  // logout, after `session.logout` went out.
  void ForgetSession();

  // A Redwood request through Bifrost: `rpc {event, payload}`. The callback
  // takes what the socket library gave: one element, the Redwood response
  // object. A refusal by Bifrost is an object {error, code}. A request still
  // waiting when the socket drops or the channel ends is dropped with it;
  // the owner's FRedwoodPendingReplies turns that into a lost reply.
  void Rpc(
    const FString &Event,
    const TSharedPtr<FJsonObject> &Payload,
    FRedwoodReplyCallback Callback = nullptr
  );

  // A step of the session itself (session.enter-realm, session.select-
  // character, session.logout). The reply is shaped as for Rpc. Bifrost runs
  // one step at a time, so a step sent while one waits goes out after it. A
  // step still waiting when the socket drops or the channel ends is answered
  // with {error: FRedwoodPendingReplies::LostReplyError, code:
  // "not_connected"}, as is a login or resume join ({reason:
  // "not_connected"}): nothing else tracks them.
  void Step(
    const FString &Event,
    const TSharedPtr<FJsonObject> &Payload,
    FRedwoodReplyCallback Callback = nullptr
  );

  // ── Chat ────────────────────────────────────────────────────────────────
  // protocol.md, "Chat". The replies are shaped as for Rpc: one element, an
  // object with an `error` string that is empty on success, and the reply's
  // own fields beside it (`chat.send`: msgId, topic, seq). A reply lost with
  // the socket is the owner's to answer, as for Rpc.

  // `chat.send {msgId, topic, body}`. MsgId is the caller's: Bifrost posts
  // a line once per id, so a send that is sent again cannot post twice.
  void ChatSend(
    const FString &MsgId,
    const FString &Topic,
    const FString &Body,
    FRedwoodReplyCallback Callback = nullptr
  );

  // A whisper is addressed to a player, not a topic: `chat.send {msgId, to,
  // body}`. Bifrost finds the player's session and writes the line to both
  // sides' character topics.
  void ChatWhisper(
    const FString &MsgId,
    const FString &ToPlayerId,
    const FString &Body,
    FRedwoodReplyCallback Callback = nullptr
  );

  // `chat.resume {cursors}` with the cursor of every topic this connection
  // has a line of. The reply's lines reach OnChatMessage in seq order,
  // through the same cursor and id check as a live line, before the
  // callback runs; read the topics Bifrost could not replay in full with
  // TruncatedTopics. A live line that arrives while the resume waits is
  // delivered after the reply, so the replay cannot be dropped as old. The
  // hold ends only with the reply, a drop of the channel or ResetChat; there
  // is no timer, because a request on a live channel is always answered or
  // lost with the channel, and a timer that gave up first would deliver the
  // live lines past the gap while the replay could still arrive. A refused
  // resume and a drop deliver the held lines (the owner hears that the
  // resume failed); a reset drops them with the session. A reply of a resume
  // that a reset or a drop ended changes nothing.
  void ChatResume(FRedwoodReplyCallback Callback = nullptr);

  // `chat.report {topic, seq, reason}`.
  void ChatReport(
    const FString &Topic,
    int64 Seq,
    const FString &Reason,
    FRedwoodReplyCallback Callback = nullptr
  );

  // The topics of a resume reply whose replay was cut short: lines before
  // the oldest one sent are lost to this client.
  static TArray<FString> TruncatedTopics(
    const TSharedPtr<FJsonObject> &ResumeReply
  );

  // A new session starts live: no cursor to resume from, no id to drop.
  void ResetChat();

  const TMap<FString, int64> &GetChatCursors() const {
    return ChatCursors;
  }

  // A chat line this client had not seen: delivered live, or replayed by a
  // resume. Each line of a topic comes once, and after the lines before it.
  TFunction<void(const FRedwoodChatMessage &Message)> OnChatMessage;
  // `chat.topics`: the topics this session can read and send to, each time
  // the set changes.
  TFunction<void(const TArray<FString> &Topics)> OnChatTopics;

  // A Redwood push by its Redwood event name. One handler per event.
  void OnEvent(const FString &Event, FEventHandler Handler);

  // Every push, after the event's handler ran, if it has one. The owner fans
  // the push out to subscribers this connection does not know (see
  // URedwoodClientInterface::OnPush), and says whether anyone took it, so a
  // push nobody wants can be logged.
  TFunction<bool(const FString &Event, const TSharedPtr<FJsonObject> &Payload)>
    OnAnyPush;

  // The socket opened, first or again. With a session, a resume follows and
  // OnResumed or OnResumeRefused tells its outcome; without one, this is all.
  TFunction<void()> OnConnected;
  // The socket closed and this client did not ask for it; a reconnect is
  // scheduled. Requests pending on it are gone. bAnnounced is false when
  // Bifrost closed the socket after `closed`: the owner heard the end of
  // the session already, and this is only the socket going.
  TFunction<void(bool bAnnounced)> OnDropped;
  // The session was resumed on a new socket. The argument is the join reply:
  // {sessionId, resumeToken, player, realmId?, characterId?}.
  TFunction<void(const TSharedPtr<FJsonObject> &Reply)> OnResumed;
  // The session is gone (its grace ended, or it was replaced). The owner
  // logs in again with Login.
  TFunction<void()> OnResumeRefused;
  // Bifrost closed the session: "replaced" (another login), "reconnect" (the
  // pod stops; the session is resumed on a new socket by itself), or
  // "logout" (this client's own). For "replaced" and "logout" the session is
  // forgotten before this runs, and the socket stays for the next login.
  TFunction<void(const FString &Reason)> OnClosedByServer;

  // What a reconnect does. Tests count it instead of connecting.
  TFunction<void()> ReconnectSocket;

  // Where a socket comes from. The engine's WebSockets module, unless a test
  // sets this to hand out fake sockets; a process-wide hook, because the
  // owner makes its connection where a test cannot reach it first.
  using FSocketFactory = TFunction<TSharedPtr<IWebSocket>(const FString &Uri)>;
  static FSocketFactory CreateSocketOverride;

  const FRedwoodCloseBackoff &GetBackoff() const {
    return Backoff;
  }

  // Current ids, for a resume on a new socket and for the owner's logs.
  const FString &GetSessionId() const {
    return SessionId;
  }

private:
  void OpenSocket();
  // CloseCode names the reason to the peer; RFC 6455 1000 unless the
  // close is the client's own (the heartbeat went unanswered).
  void ReleaseSocket(int32 CloseCode = NormalCloseCode);
  void Join(const TSharedPtr<FJsonObject> &Payload, FJoinCallback Callback);
  void Resume();
  FString Push(
    const FString &Event,
    const TSharedPtr<FJsonObject> &Payload,
    FRedwoodReplyCallback Callback
  );
  void SendNextStep();
  void Send(
    const FString &JoinRef,
    const FString &Ref,
    const FString &Topic,
    const FString &Event,
    const TSharedPtr<FJsonObject> &Payload
  );
  FString NextRef();
  void HandleMessage(const FString &Text);
  void HandleReply(const FString &Ref, const TSharedPtr<FJsonObject> &Payload);
  void HandlePush(const TSharedPtr<FJsonObject> &Payload);
  void HandleChatMessage(const TSharedPtr<FJsonObject> &Payload);
  void HandleChatTopics(const TSharedPtr<FJsonObject> &Payload);
  // The one path a line takes to OnChatMessage. False when it was seen.
  bool DeliverChatLine(const FRedwoodChatMessage &Message);
  void ApplyChatResume(
    const TArray<TSharedPtr<FJsonValue>> &Response, uint32 Generation
  );
  void HandleClosedByServer(const TSharedPtr<FJsonObject> &Payload);
  void EndChannel();
  void HandleSocketClosed(bool bRequested);
  static TArray<TSharedPtr<FJsonValue>> LostResponse();
  static TSharedPtr<FJsonObject> LostJoinRefusal(const FString &Message);
  void ScheduleReconnect();
  void SendHeartbeat();
  void TakeSession(const TSharedPtr<FJsonObject> &Reply);
  void DropPending();
  static TArray<TSharedPtr<FJsonValue>> ReplyAsResponse(
    const FString &Status, const TSharedPtr<FJsonObject> &Payload
  );

  FTimerManager &TimerManager;
  TSharedPtr<IWebSocket> Socket;
  // Sockets replaced from inside their own callbacks. Freed on the next
  // open or at the end, so a socket is never destroyed while it runs.
  TArray<TSharedPtr<IWebSocket>> DiscardedSockets;
  // Counts the sockets, so a late event of a replaced socket is ignored.
  uint32 SocketGeneration = 0;
  FString Uri;
  bool bCloseRequested = false;
  // Set by a `closed` push: the end of the channel (or of the socket) that
  // follows is Bifrost's, not a drop. "reconnect" asks for an immediate
  // reconnect on a new socket instead of a backoff.
  bool bServerCloseExpected = false;
  bool bReconnectAtOnce = false;

  FString SessionId;
  FString ResumeToken;
  // The ref of the current phx_join; every message of the channel carries
  // it, and a message of an older join is ignored.
  FString JoinRef;
  FString PendingJoinRef;
  FJoinCallback PendingJoin;
  uint64 RefCounter = 0;
  TMap<FString, FRedwoodReplyCallback> PendingReplies;
  struct FQueuedStep {
    FString Event;
    TSharedPtr<FJsonObject> Payload;
    FRedwoodReplyCallback Callback;
  };
  // The steps behind the one in flight, whose ref is StepRef.
  TArray<FQueuedStep> QueuedSteps;
  FString StepRef;
  TMap<FString, FEventHandler> Handlers;
  FRedwoodRecentIds SeenPushIds{RememberedPushIds};

  // The seq of the last line delivered, per topic. A line at or before it
  // was seen (a replay), and a resume asks for what came after.
  TMap<FString, int64> ChatCursors;
  FRedwoodRecentIds SeenChatMsgIds{RememberedChatMsgIds};
  // A live line that arrives while a resume waits would move the cursor past
  // the replay and have it dropped, so it waits until the reply is applied.
  bool bChatResumePending = false;
  TArray<FRedwoodChatMessage> ChatLinesHeldForResume;
  // Counts the resets and drops, so the reply of a resume sent before one
  // is known as stale: a new session must not take the old one's replay.
  uint32 ChatResumeGeneration = 0;

  FRedwoodCloseBackoff Backoff;
  FTimerHandle HeartbeatTimer;
  // A reconnect with no backoff (the pod told the client to come back) runs
  // on the next tick, outside the closing socket's callback.
  FTimerHandle ReconnectNowTimer;
  bool bHeartbeatPending = false;
};
