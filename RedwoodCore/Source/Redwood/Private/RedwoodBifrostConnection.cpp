// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.

#include "RedwoodBifrostConnection.h"
#include "RedwoodModule.h"

#include "IWebSocket.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "WebSocketsModule.h"

namespace {
  // Phoenix Channels v2: a message is [join_ref, ref, topic, event, payload].
  constexpr int32 MessageFields = 5;
  constexpr int32 JoinRefField = 0;
  constexpr int32 RefField = 1;
  constexpr int32 TopicField = 2;
  constexpr int32 EventField = 3;
  constexpr int32 PayloadField = 4;

  const TCHAR *const SessionTopic = TEXT("session");
  const TCHAR *const HeartbeatTopic = TEXT("phoenix");
  // The transport path and the serializer version of a Phoenix endpoint
  // mounted at /socket.
  const TCHAR *const SocketPath = TEXT("/socket/websocket?vsn=2.0.0");

  TSharedPtr<FJsonValue> ParseJson(const FString &Text) {
    TSharedPtr<FJsonValue> Value;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
    if (!FJsonSerializer::Deserialize(Reader, Value)) {
      return nullptr;
    }
    return Value;
  }

  TSharedPtr<FJsonValue> StringOrNull(const FString &Text) {
    if (Text.IsEmpty()) {
      return MakeShared<FJsonValueNull>();
    }
    return MakeShared<FJsonValueString>(Text);
  }

  FString FieldString(
    const TArray<TSharedPtr<FJsonValue>> &Fields, int32 Index
  ) {
    FString Text;
    if (Fields[Index].IsValid()) {
      Fields[Index]->TryGetString(Text);
    }
    return Text;
  }
}

FRedwoodBifrostConnection::FSocketFactory
  FRedwoodBifrostConnection::CreateSocketOverride;

FRedwoodBifrostConnection::FRedwoodBifrostConnection(
  FTimerManager &InTimerManager
) : TimerManager(InTimerManager) {
  ReconnectSocket = [this]() { OpenSocket(); };
  SeenPushIds.SetNum(RememberedPushIds);
}

FRedwoodBifrostConnection::~FRedwoodBifrostConnection() {
  bCloseRequested = true;
  Backoff.Reset(TimerManager);
  TimerManager.ClearTimer(HeartbeatTimer);
  TimerManager.ClearTimer(ReconnectNowTimer);
  ReleaseSocket();
}

void FRedwoodBifrostConnection::Connect(const FString &BaseUri) {
  Uri = BaseUri;
  Uri.RemoveFromEnd(TEXT("/"));
  Uri += SocketPath;
  bCloseRequested = false;
  Backoff.Reset(TimerManager);
  OpenSocket();
}

void FRedwoodBifrostConnection::Close() {
  bCloseRequested = true;
  Backoff.Reset(TimerManager);
  TimerManager.ClearTimer(HeartbeatTimer);
  TimerManager.ClearTimer(ReconnectNowTimer);
  ReleaseSocket();
  DropPending();
}

bool FRedwoodBifrostConnection::IsConnected() const {
  return Socket.IsValid() && Socket->IsConnected();
}

// The engine's socket cannot connect twice, so each attempt is a new one.
// The delegates check the generation: a socket replaced while its close was
// still queued to the game thread must not report a drop of the new one.
void FRedwoodBifrostConnection::OpenSocket() {
  // Outside every callback of theirs: a timer or the owner brought us here.
  // Before the release, so the socket released now lives to the next open.
  DiscardedSockets.Reset();
  ReleaseSocket();
  const uint32 Generation = ++SocketGeneration;
  Socket = CreateSocketOverride
    ? CreateSocketOverride(Uri)
    : FWebSocketsModule::Get().CreateWebSocket(Uri, FString());
  TWeakPtr<FRedwoodBifrostConnection> Weak = AsShared();

  // The lambdas below read their captures only before a call that can reach
  // ReleaseSocket: Clear() destroys a bound lambda even while it runs.
  Socket->OnConnected().AddLambda([Weak, Generation]() {
    const TSharedPtr<FRedwoodBifrostConnection> Self = Weak.Pin();
    if (!Self.IsValid() || Generation != Self->SocketGeneration) {
      return;
    }
    UE_LOG(LogRedwood, Log, TEXT("Connected to Bifrost at %s"), *Self->Uri);
    Self->Backoff.NoteConnected(Self->TimerManager);
    Self->bHeartbeatPending = false;
    Self->TimerManager.SetTimer(
      Self->HeartbeatTimer,
      FTimerDelegate::CreateLambda([Weak]() {
        if (const TSharedPtr<FRedwoodBifrostConnection> Pinned = Weak.Pin()) {
          Pinned->SendHeartbeat();
        }
      }),
      HeartbeatSeconds,
      true
    );
    if (Self->OnConnected) {
      Self->OnConnected();
    }
    if (Self->HasSession()) {
      Self->Resume();
    }
  });

  Socket->OnConnectionError().AddLambda([Weak,
                                         Generation](const FString &Error) {
    const TSharedPtr<FRedwoodBifrostConnection> Self = Weak.Pin();
    if (!Self.IsValid() || Generation != Self->SocketGeneration) {
      return;
    }
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT("Could not connect to Bifrost at %s: %s"),
      *Self->Uri,
      *Error
    );
    Self->HandleSocketClosed(false);
  });

  Socket->OnClosed().AddLambda(
    [Weak,
     Generation](int32 StatusCode, const FString &Reason, bool bWasClean) {
      const TSharedPtr<FRedwoodBifrostConnection> Self = Weak.Pin();
      if (!Self.IsValid() || Generation != Self->SocketGeneration) {
        return;
      }
      const bool bRequested =
        Self->bCloseRequested || Self->bServerCloseExpected;
      if (bRequested) {
        UE_LOG(
          LogRedwood,
          Log,
          TEXT("The Bifrost socket closed (%d %s)."),
          StatusCode,
          *Reason
        );
      } else {
        UE_LOG(
          LogRedwood,
          Warning,
          TEXT("The Bifrost socket closed (%d %s); reconnecting."),
          StatusCode,
          *Reason
        );
      }
      Self->HandleSocketClosed(bRequested);
    }
  );

  Socket->OnMessage().AddLambda([Weak, Generation](const FString &Text) {
    const TSharedPtr<FRedwoodBifrostConnection> Self = Weak.Pin();
    if (Self.IsValid() && Generation == Self->SocketGeneration) {
      Self->HandleMessage(Text);
    }
  });

  Socket->Connect();
}

void FRedwoodBifrostConnection::ReleaseSocket(int32 CloseCode) {
  if (!Socket.IsValid()) {
    return;
  }
  // The generation check above makes a late event of this socket inert;
  // the delegates are cleared too, so nothing holds this object through it.
  ++SocketGeneration;
  Socket->OnConnected().Clear();
  Socket->OnConnectionError().Clear();
  Socket->OnClosed().Clear();
  Socket->OnMessage().Clear();
  // Also a socket still connecting: the engine queues the close and frees
  // the socket once it ran; an open connect would otherwise go on to hold a
  // Bifrost slot until the idle close.
  Socket->Close(CloseCode);
  // This can run inside the socket's own callback (a refused resume, a
  // channel error), so the socket is kept until the next open.
  DiscardedSockets.Add(MoveTemp(Socket));
  Socket.Reset();
}

// A socket is gone. A close this client asked for, or one Bifrost announced,
// ends here; any other starts the reconnect.
void FRedwoodBifrostConnection::HandleSocketClosed(bool bRequested) {
  TimerManager.ClearTimer(HeartbeatTimer);
  bHeartbeatPending = false;
  JoinRef.Reset();
  DropPending();

  if (bRequested) {
    const bool bAtOnce = bReconnectAtOnce;
    bServerCloseExpected = false;
    bReconnectAtOnce = false;
    if (bCloseRequested) {
      return;
    }
    if (bAtOnce) {
      // The pod is stopping; a sibling takes the resume. The owner holds
      // its requests for the gap. No backoff: this is not a failure.
      if (OnDropped) {
        OnDropped(true);
      }
      TWeakPtr<FRedwoodBifrostConnection> Weak = AsShared();
      ReconnectNowTimer = TimerManager.SetTimerForNextTick([Weak]() {
        if (const TSharedPtr<FRedwoodBifrostConnection> Self = Weak.Pin()) {
          Self->ReconnectSocket();
        }
      });
    } else {
      // Bifrost closed the socket itself after `closed` (it normally keeps
      // it, see EndChannel). The owner heard the end of the session, so
      // this is no loss to report, but it must know the socket is down
      // until the reconnect: it comes back for the next login.
      if (OnDropped) {
        OnDropped(false);
      }
      ScheduleReconnect();
    }
    return;
  }

  if (OnDropped) {
    OnDropped(true);
  }
  ScheduleReconnect();
}

void FRedwoodBifrostConnection::ScheduleReconnect() {
  if (bCloseRequested) {
    return;
  }
  TWeakPtr<FRedwoodBifrostConnection> Weak = AsShared();
  const float DelaySeconds = Backoff.Schedule(TimerManager, [Weak]() {
    if (const TSharedPtr<FRedwoodBifrostConnection> Self = Weak.Pin()) {
      Self->ReconnectSocket();
    }
  });
  UE_LOG(
    LogRedwood,
    Log,
    TEXT("Reconnecting to Bifrost in %.1f s (attempt %d)."),
    DelaySeconds,
    Backoff.NumAttempts()
  );
}

// After `closed`, Bifrost ends the channel and keeps the socket. After
// `replaced` and `logout` the socket is the next login's; after `reconnect`
// the pod is stopping and closes the socket soon, so a new one is opened
// now and the session is resumed on it, with no backoff: this is no failure.
void FRedwoodBifrostConnection::EndChannel() {
  const bool bAtOnce = bReconnectAtOnce;
  bServerCloseExpected = false;
  bReconnectAtOnce = false;
  JoinRef.Reset();
  DropPending();
  if (!bAtOnce) {
    return;
  }
  UE_LOG(LogRedwood, Log, TEXT("The Bifrost pod is stopping; reconnecting."));
  if (OnDropped) {
    OnDropped(true);
  }
  ReleaseSocket();
  TWeakPtr<FRedwoodBifrostConnection> Weak = AsShared();
  ReconnectNowTimer = TimerManager.SetTimerForNextTick([Weak]() {
    if (const TSharedPtr<FRedwoodBifrostConnection> Self = Weak.Pin()) {
      Self->ReconnectSocket();
    }
  });
}

// The one-element response of a request whose reply was lost with its
// socket or channel: the handlers read it as an error, and the owner's
// FRedwoodPendingReplies answers each caller once.
TArray<TSharedPtr<FJsonValue>> FRedwoodBifrostConnection::LostResponse() {
  TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
  Object->SetStringField(TEXT("error"), FRedwoodPendingReplies::LostReplyError);
  Object->SetStringField(TEXT("code"), TEXT("not_connected"));
  TArray<TSharedPtr<FJsonValue>> Response;
  Response.Add(MakeShared<FJsonValueObject>(Object));
  return Response;
}

TSharedPtr<FJsonObject> FRedwoodBifrostConnection::LostJoinRefusal(
  const FString &Message
) {
  TSharedPtr<FJsonObject> Refusal = MakeShared<FJsonObject>();
  Refusal->SetStringField(TEXT("reason"), TEXT("not_connected"));
  Refusal->SetStringField(TEXT("message"), Message);
  return Refusal;
}

// The channel is gone with what waited on it. The join and the session steps
// have no other way to hear it, so they are answered here, once, with the
// lost-reply error: a login or a realm entry must not wait for good. The
// other requests are dropped as the socket library dropped them: the owner
// tracks each one (FRedwoodPendingReplies) and fails it with a message of
// its own at the drop. Moved-out copies, because a callback can send a new
// request.
void FRedwoodBifrostConnection::DropPending() {
  PendingJoinRef.Reset();
  FJoinCallback Join = MoveTemp(PendingJoin);
  PendingJoin = nullptr;
  FRedwoodReplyCallback StepInFlight;
  if (!StepRef.IsEmpty()) {
    PendingReplies.RemoveAndCopyValue(StepRef, StepInFlight);
    StepRef.Reset();
  }
  PendingReplies.Reset();
  TArray<FQueuedStep> Steps = MoveTemp(QueuedSteps);
  QueuedSteps.Reset();

  if (Join) {
    Join(false, LostJoinRefusal(FRedwoodPendingReplies::LostReplyError));
  }
  if (StepInFlight) {
    StepInFlight(LostResponse());
  }
  for (FQueuedStep &Queued : Steps) {
    if (Queued.Callback) {
      Queued.Callback(LostResponse());
    }
  }
}

// ── Session ─────────────────────────────────────────────────────────────

void FRedwoodBifrostConnection::Login(
  const TSharedPtr<FJsonObject> &LoginPayload, FJoinCallback Callback
) {
  TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
  Payload->SetNumberField(TEXT("v"), ContractVersion);
  Payload->SetObjectField(TEXT("login"), LoginPayload);
  // A login is a new session; a resume of the old one must not follow.
  SessionId.Reset();
  ResumeToken.Reset();
  Join(Payload, MoveTemp(Callback));
}

void FRedwoodBifrostConnection::Resume() {
  TSharedPtr<FJsonObject> Resume = MakeShared<FJsonObject>();
  Resume->SetStringField(TEXT("sessionId"), SessionId);
  Resume->SetStringField(TEXT("resumeToken"), ResumeToken);
  TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
  Payload->SetNumberField(TEXT("v"), ContractVersion);
  Payload->SetObjectField(TEXT("resume"), Resume);

  TWeakPtr<FRedwoodBifrostConnection> Weak = AsShared();
  Join(Payload, [Weak](bool bOk, const TSharedPtr<FJsonObject> &Response) {
    const TSharedPtr<FRedwoodBifrostConnection> Self = Weak.Pin();
    if (!Self.IsValid()) {
      return;
    }
    if (bOk) {
      UE_LOG(LogRedwood, Log, TEXT("Resumed the Bifrost session."));
      if (Self->OnResumed) {
        Self->OnResumed(Response);
      }
      return;
    }
    FString Reason;
    if (Response.IsValid()) {
      Response->TryGetStringField(TEXT("reason"), Reason);
    }
    // `invalid_request` is this client's own fault (the contract version)
    // and another socket would not change it; the login that follows
    // reports it where the player can read it.
    if (Reason == TEXT("resume_refused") || Reason == TEXT("invalid_request")) {
      UE_LOG(
        LogRedwood,
        Warning,
        TEXT("The Bifrost session is gone (%s); logging in again."),
        *Reason
      );
      Self->SessionId.Reset();
      Self->ResumeToken.Reset();
      if (Self->OnResumeRefused) {
        Self->OnResumeRefused();
      }
      return;
    }
    // The join went with its socket, or a newer join replaced it. Either
    // way this socket is not ours to drop: the close that took the join
    // is handled where it happened, and a newer join is on this socket.
    if (Reason == TEXT("not_connected")) {
      return;
    }
    // Any other refusal (rate limit, Bifrost without Redis) is this pod's
    // trouble, not the session's: try again on another socket.
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT("The resume was refused (%s); reconnecting."),
      *Reason
    );
    Self->ReleaseSocket();
    Self->HandleSocketClosed(false);
  });
}

void FRedwoodBifrostConnection::ForgetSession() {
  SessionId.Reset();
  ResumeToken.Reset();
}

void FRedwoodBifrostConnection::Join(
  const TSharedPtr<FJsonObject> &Payload, FJoinCallback Callback
) {
  if (!IsConnected()) {
    Callback(false, LostJoinRefusal(TEXT("Not connected to Bifrost.")));
    return;
  }
  // A join replaces the channel: the old join's messages are stale, and a
  // join still waiting for its reply hears that it was replaced.
  if (PendingJoin) {
    FJoinCallback Replaced = MoveTemp(PendingJoin);
    PendingJoin = nullptr;
    Replaced(false, LostJoinRefusal(TEXT("A newer join replaced this one.")));
  }
  // The channel this join replaces goes with the steps and requests on it:
  // their replies would carry the old join_ref and be dropped, so they are
  // answered as lost now, and a `closed` of that channel is over too.
  DropPending();
  bServerCloseExpected = false;
  bReconnectAtOnce = false;
  JoinRef = NextRef();
  PendingJoinRef = JoinRef;
  PendingJoin = MoveTemp(Callback);
  Send(JoinRef, JoinRef, SessionTopic, TEXT("phx_join"), Payload);
}

void FRedwoodBifrostConnection::TakeSession(
  const TSharedPtr<FJsonObject> &Reply
) {
  Reply->TryGetStringField(TEXT("sessionId"), SessionId);
  Reply->TryGetStringField(TEXT("resumeToken"), ResumeToken);
}

// ── Requests ────────────────────────────────────────────────────────────

void FRedwoodBifrostConnection::Rpc(
  const FString &Event,
  const TSharedPtr<FJsonObject> &Payload,
  FRedwoodReplyCallback Callback
) {
  TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
  Request->SetStringField(TEXT("event"), Event);
  Request->SetObjectField(
    TEXT("payload"), Payload.IsValid() ? Payload : MakeShared<FJsonObject>()
  );
  Push(TEXT("rpc"), Request, MoveTemp(Callback));
}

// Bifrost runs one step of a session at a time and refuses a second one
// while the first waits, so the steps queue here and go out in order.
void FRedwoodBifrostConnection::Step(
  const FString &Event,
  const TSharedPtr<FJsonObject> &Payload,
  FRedwoodReplyCallback Callback
) {
  FQueuedStep QueuedStep;
  QueuedStep.Event = Event;
  QueuedStep.Payload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();
  QueuedStep.Callback = MoveTemp(Callback);
  QueuedSteps.Add(MoveTemp(QueuedStep));
  if (StepRef.IsEmpty()) {
    SendNextStep();
  }
}

void FRedwoodBifrostConnection::SendNextStep() {
  while (StepRef.IsEmpty() && QueuedSteps.Num() > 0) {
    FQueuedStep Next = MoveTemp(QueuedSteps[0]);
    QueuedSteps.RemoveAt(0);
    StepRef = Push(Next.Event, Next.Payload, MoveTemp(Next.Callback));
  }
}

// A request before the join, or after a drop, has no channel to go on. It
// is dropped like the socket library dropped one on a closed socket: the
// owner gates requests on the session, and tracks the ones it sends.
// Returns the ref of the message sent, or nothing when it was dropped.
FString FRedwoodBifrostConnection::Push(
  const FString &Event,
  const TSharedPtr<FJsonObject> &Payload,
  FRedwoodReplyCallback Callback
) {
  if (!IsConnected() || JoinRef.IsEmpty()) {
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT("Dropped a %s request: the Bifrost session channel is not joined."),
      *Event
    );
    return FString();
  }
  const FString Ref = NextRef();
  if (Callback) {
    PendingReplies.Add(Ref, MoveTemp(Callback));
  }
  Send(JoinRef, Ref, SessionTopic, Event, Payload);
  return Ref;
}

void FRedwoodBifrostConnection::OnEvent(
  const FString &Event, FEventHandler Handler
) {
  Handlers.Add(Event, MoveTemp(Handler));
}

FString FRedwoodBifrostConnection::NextRef() {
  return FString::Printf(TEXT("%llu"), ++RefCounter);
}

void FRedwoodBifrostConnection::Send(
  const FString &InJoinRef,
  const FString &Ref,
  const FString &Topic,
  const FString &Event,
  const TSharedPtr<FJsonObject> &Payload
) {
  TArray<TSharedPtr<FJsonValue>> Message;
  Message.Add(StringOrNull(InJoinRef));
  Message.Add(StringOrNull(Ref));
  Message.Add(MakeShared<FJsonValueString>(Topic));
  Message.Add(MakeShared<FJsonValueString>(Event));
  Message.Add(MakeShared<FJsonValueObject>(Payload));

  FString Text;
  const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>
    Writer =
      TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(
        &Text
      );
  FJsonSerializer::Serialize(Message, Writer);
  Socket->Send(Text);
}

void FRedwoodBifrostConnection::SendHeartbeat() {
  if (!IsConnected()) {
    return;
  }
  if (bHeartbeatPending) {
    // The last heartbeat got no reply in a whole interval: the socket is
    // dead and no close came. Close it here, and reconnect.
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT("Bifrost did not answer a heartbeat; reconnecting.")
    );
    ReleaseSocket(HeartbeatCloseCode);
    HandleSocketClosed(false);
    return;
  }
  bHeartbeatPending = true;
  Send(
    FString(),
    NextRef(),
    HeartbeatTopic,
    TEXT("heartbeat"),
    MakeShared<FJsonObject>()
  );
}

// ── Messages ────────────────────────────────────────────────────────────

void FRedwoodBifrostConnection::HandleMessage(const FString &Text) {
  const TSharedPtr<FJsonValue> Root = ParseJson(Text);
  const TArray<TSharedPtr<FJsonValue>> *Fields = nullptr;
  if (
    !Root.IsValid() || !Root->TryGetArray(Fields) ||
    Fields->Num() < MessageFields
  ) {
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT("Dropped a Bifrost message that is not a Phoenix message.")
    );
    return;
  }

  const FString Topic = FieldString(*Fields, TopicField);
  const FString Event = FieldString(*Fields, EventField);
  const FString Ref = FieldString(*Fields, RefField);
  const FString MessageJoinRef = FieldString(*Fields, JoinRefField);
  const TSharedPtr<FJsonObject> *PayloadPtr = nullptr;
  const TSharedPtr<FJsonObject> Payload = (*Fields)[PayloadField].IsValid() &&
      (*Fields)[PayloadField]->TryGetObject(PayloadPtr)
    ? *PayloadPtr
    : MakeShared<FJsonObject>();

  if (Topic == HeartbeatTopic) {
    bHeartbeatPending = false;
    return;
  }
  if (Topic != SessionTopic) {
    return;
  }
  // A message of a join this client replaced.
  if (!MessageJoinRef.IsEmpty() && MessageJoinRef != JoinRef) {
    return;
  }

  if (Event == TEXT("phx_reply")) {
    HandleReply(Ref, Payload);
  } else if (Event == TEXT("push")) {
    HandlePush(Payload);
  } else if (Event == TEXT("closed")) {
    HandleClosedByServer(Payload);
  } else if (Event == TEXT("phx_error") || Event == TEXT("phx_close")) {
    if (bServerCloseExpected) {
      EndChannel();
      return;
    }
    // The channel process ended without a `closed`: Bifrost itself failed.
    // The session may still be there; a new socket resumes it.
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT("The Bifrost session channel ended (%s); reconnecting."),
      *Event
    );
    ReleaseSocket();
    HandleSocketClosed(false);
  }
}

void FRedwoodBifrostConnection::HandleReply(
  const FString &Ref, const TSharedPtr<FJsonObject> &Payload
) {
  FString Status;
  Payload->TryGetStringField(TEXT("status"), Status);
  const TSharedPtr<FJsonObject> *ResponsePtr = nullptr;
  const TSharedPtr<FJsonObject> Response =
    Payload->TryGetObjectField(TEXT("response"), ResponsePtr) ? *ResponsePtr
                                                              : nullptr;
  const bool bOk = Status == TEXT("ok");

  if (!PendingJoinRef.IsEmpty() && Ref == PendingJoinRef) {
    PendingJoinRef.Reset();
    FJoinCallback Callback = MoveTemp(PendingJoin);
    PendingJoin = nullptr;
    if (bOk && Response.IsValid()) {
      TakeSession(Response);
    } else {
      // A refused join leaves no channel; Push drops until the next join.
      JoinRef.Reset();
    }
    if (Callback) {
      Callback(bOk, Response.IsValid() ? Response : MakeShared<FJsonObject>());
    }
    return;
  }

  // The step is over before its callback runs, so a step the callback
  // sends goes out; the ones that queued behind it follow.
  const bool bStepReply = !StepRef.IsEmpty() && Ref == StepRef;
  if (bStepReply) {
    StepRef.Reset();
  }
  FRedwoodReplyCallback Callback;
  if (PendingReplies.RemoveAndCopyValue(Ref, Callback) && Callback) {
    Callback(ReplyAsResponse(Status, Payload));
  }
  if (bStepReply) {
    SendNextStep();
  }
}

// The shape the request handlers read: one element, an object with an
// `error` string that is empty on success. Bifrost's own refusal becomes
// that object too, with its code beside the message.
TArray<TSharedPtr<FJsonValue>> FRedwoodBifrostConnection::ReplyAsResponse(
  const FString &Status, const TSharedPtr<FJsonObject> &Payload
) {
  TSharedPtr<FJsonObject> Object;
  if (Status == TEXT("ok")) {
    const TSharedPtr<FJsonObject> *Reply = nullptr;
    const TSharedPtr<FJsonObject> *Response = nullptr;
    if (
      Payload->TryGetObjectField(TEXT("response"), Reply) &&
      (*Reply)->TryGetObjectField(TEXT("response"), Response)
    ) {
      Object = *Response;
    }
    if (!Object.IsValid()) {
      // An event that sends no reply (`response: null`).
      Object = MakeShared<FJsonObject>();
      Object->SetStringField(TEXT("error"), TEXT(""));
    }
  } else {
    Object = MakeShared<FJsonObject>();
    FString Code = TEXT("unavailable");
    FString Message = TEXT("The request failed.");
    const TSharedPtr<FJsonObject> *Reply = nullptr;
    const TSharedPtr<FJsonObject> *Error = nullptr;
    if (
      Payload->TryGetObjectField(TEXT("response"), Reply) &&
      (*Reply)->TryGetObjectField(TEXT("error"), Error)
    ) {
      (*Error)->TryGetStringField(TEXT("code"), Code);
      (*Error)->TryGetStringField(TEXT("message"), Message);
    }
    Object->SetStringField(TEXT("error"), Message);
    Object->SetStringField(TEXT("code"), Code);
  }
  TArray<TSharedPtr<FJsonValue>> Response;
  Response.Add(MakeShared<FJsonValueObject>(Object));
  return Response;
}

// {pushId, event, payload}: a Redwood push. The realm sends a kept push
// again after a resume, so one whose id was seen is dropped.
void FRedwoodBifrostConnection::HandlePush(
  const TSharedPtr<FJsonObject> &Payload
) {
  FString PushId;
  FString Event;
  Payload->TryGetStringField(TEXT("pushId"), PushId);
  Payload->TryGetStringField(TEXT("event"), Event);
  if (!PushId.IsEmpty()) {
    if (SeenPushIds.Contains(PushId)) {
      return;
    }
    SeenPushIds[NextSeenPushIdSlot] = PushId;
    NextSeenPushIdSlot = (NextSeenPushIdSlot + 1) % RememberedPushIds;
  }
  const FEventHandler *Handler = Handlers.Find(Event);
  if (!Handler) {
    UE_LOG(LogRedwood, Verbose, TEXT("No handler for the push %s."), *Event);
    return;
  }
  const TSharedPtr<FJsonObject> *Inner = nullptr;
  const TSharedPtr<FJsonObject> Message =
    Payload->TryGetObjectField(TEXT("payload"), Inner)
    ? *Inner
    : MakeShared<FJsonObject>();
  (*Handler)(Event, MakeShared<FJsonValueObject>(Message));
}

void FRedwoodBifrostConnection::HandleClosedByServer(
  const TSharedPtr<FJsonObject> &Payload
) {
  FString Reason;
  Payload->TryGetStringField(TEXT("reason"), Reason);
  UE_LOG(LogRedwood, Log, TEXT("Bifrost closed the session: %s."), *Reason);
  bServerCloseExpected = true;
  bReconnectAtOnce = Reason == TEXT("reconnect");
  if (!bReconnectAtOnce) {
    ForgetSession();
  }
  if (OnClosedByServer) {
    OnClosedByServer(Reason);
  }
}
