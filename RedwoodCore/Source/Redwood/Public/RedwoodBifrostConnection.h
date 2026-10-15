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

// The one player connection: a WebSocket to Bifrost, which carries the
// session, every request to the Redwood director and realm, and every push
// from them. It speaks the Phoenix Channels protocol (v2 JSON) on one
// channel, "session". The contract is HollowedOath-Services
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

  // A Redwood push by its Redwood event name. One handler per event.
  void OnEvent(const FString &Event, FEventHandler Handler);

  // The socket opened, first or again. With a session, a resume follows and
  // OnResumed or OnResumeRefused tells its outcome; without one, this is all.
  TFunction<void()> OnConnected;
  // The socket closed and this client did not ask for it; a reconnect is
  // scheduled. Requests pending on it are gone.
  TFunction<void()> OnDropped;
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
  TArray<FString> SeenPushIds;
  int32 NextSeenPushIdSlot = 0;

  FRedwoodCloseBackoff Backoff;
  FTimerHandle HeartbeatTimer;
  // A reconnect with no backoff (the pod told the client to come back) runs
  // on the next tick, outside the closing socket's callback.
  FTimerHandle ReconnectNowTimer;
  bool bHeartbeatPending = false;
};
