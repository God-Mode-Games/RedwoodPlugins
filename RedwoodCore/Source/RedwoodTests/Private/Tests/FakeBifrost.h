// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// A fake Bifrost for the connection tests: an IWebSocket that records what
// the client sends as Phoenix messages, and lets a test play the server
// (open, answer, push, close, drop) on the game thread, with no network and
// no threads. FRedwoodBifrostConnection takes its sockets from
// CreateSocketOverride while an FFakeBifrost exists.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "IWebSocket.h"
#include "Misc/AutomationTest.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "RedwoodBifrostConnection.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace RedwoodFakeBifrost {
  // Just over a backoff delay with its jitter (1 s and 2 s, times 1.3). Here,
  // not in each suite: the test module is a unity build, and the same name in
  // two anonymous namespaces is one translation unit's double definition.
  constexpr float PastFirstDelaySeconds = 1.4f;
  constexpr float PastSecondDelaySeconds = 2.7f;

  constexpr EAutomationExpectedMessageFlags::MatchType Contains =
    EAutomationExpectedMessageFlags::Contains;

  // A test advances GFrameCounter for its timers (see TickTimers); the
  // engine's own count is put back when the test ends.
  struct FFrameCounterGuard {
    const uint64 Saved = GFrameCounter;

    ~FFrameCounterGuard() {
      GFrameCounter = Saved;
    }
  };

  // One Phoenix Channels v2 message: [join_ref, ref, topic, event, payload].
  struct FPhoenixMessage {
    FString JoinRef;
    FString Ref;
    FString Topic;
    FString Event;
    TSharedPtr<FJsonObject> Payload;

    bool IsValid() const {
      return Payload.IsValid();
    }

    // The request's event (`event` of an `rpc`), or the message's.
    FString RequestEvent() const {
      FString Inner;
      if (
        Event == TEXT("rpc") && Payload->TryGetStringField(TEXT("event"), Inner)
      ) {
        return Inner;
      }
      return Event;
    }
  };

  inline FString ToJson(const TArray<TSharedPtr<FJsonValue>> &Fields) {
    FString Text;
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>
      Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(
          &Text
        );
    FJsonSerializer::Serialize(Fields, Writer);
    return Text;
  }

  inline TSharedPtr<FJsonValue> StringOrNull(const FString &Text) {
    if (Text.IsEmpty()) {
      return MakeShared<FJsonValueNull>();
    }
    return MakeShared<FJsonValueString>(Text);
  }

  inline FString EncodeMessage(
    const FString &JoinRef,
    const FString &Ref,
    const FString &Topic,
    const FString &Event,
    const TSharedPtr<FJsonObject> &Payload
  ) {
    TArray<TSharedPtr<FJsonValue>> Fields;
    Fields.Add(StringOrNull(JoinRef));
    Fields.Add(StringOrNull(Ref));
    Fields.Add(MakeShared<FJsonValueString>(Topic));
    Fields.Add(MakeShared<FJsonValueString>(Event));
    Fields.Add(MakeShared<FJsonValueObject>(Payload));
    return ToJson(Fields);
  }

  inline FPhoenixMessage DecodeMessage(const FString &Text) {
    FPhoenixMessage Message;
    TSharedPtr<FJsonValue> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
    const TArray<TSharedPtr<FJsonValue>> *Fields = nullptr;
    if (
      !FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid() ||
      !Root->TryGetArray(Fields) || Fields->Num() != 5
    ) {
      return Message;
    }
    (*Fields)[0]->TryGetString(Message.JoinRef);
    (*Fields)[1]->TryGetString(Message.Ref);
    (*Fields)[2]->TryGetString(Message.Topic);
    (*Fields)[3]->TryGetString(Message.Event);
    const TSharedPtr<FJsonObject> *Payload = nullptr;
    if ((*Fields)[4]->TryGetObject(Payload)) {
      Message.Payload = *Payload;
    }
    return Message;
  }

  // The socket the client holds. The test is the other end.
  class FFakeBifrostSocket : public IWebSocket {
  public:
    FString Uri;
    bool bConnectCalled = false;
    bool bConnected = false;
    // The server accepted this socket once; a socket connects only once.
    bool bOpened = false;
    bool bConnectFailed = false;
    bool bCloseCalled = false;
    int32 CloseCode = 0;
    // Every message the client sent, in order, decoded.
    TArray<FPhoenixMessage> Sent;

    explicit FFakeBifrostSocket(const FString &InUri) : Uri(InUri) {}

    // IWebSocket
    virtual void Connect() override {
      bConnectCalled = true;
    }
    virtual void Close(int32 Code, const FString &Reason) override {
      bCloseCalled = true;
      CloseCode = Code;
      bConnected = false;
    }
    virtual bool IsConnected() override {
      return bConnected;
    }
    virtual void Send(const FString &Data) override {
      Sent.Add(DecodeMessage(Data));
    }
    virtual void Send(const void *Data, SIZE_T Size, bool bIsBinary) override {}
    virtual void SetTextMessageMemoryLimit(
      uint64 TextMessageMemoryLimit
    ) override {}
    virtual FWebSocketConnectedEvent &OnConnected() override {
      return ConnectedEvent;
    }
    virtual FWebSocketConnectionErrorEvent &OnConnectionError() override {
      return ConnectionErrorEvent;
    }
    virtual FWebSocketClosedEvent &OnClosed() override {
      return ClosedEvent;
    }
    virtual FWebSocketMessageEvent &OnMessage() override {
      return MessageEvent;
    }
    virtual FWebSocketBinaryMessageEvent &OnBinaryMessage() override {
      return BinaryMessageEvent;
    }
    virtual FWebSocketRawMessageEvent &OnRawMessage() override {
      return RawMessageEvent;
    }
    virtual FWebSocketMessageSentEvent &OnMessageSent() override {
      return MessageSentEvent;
    }

    // ── The server side ──────────────────────────────────────────────────

    // The connect succeeded.
    void Open() {
      bOpened = true;
      bConnected = true;
      ConnectedEvent.Broadcast();
    }

    // The connect failed; the engine reports no close after it.
    void FailToConnect(const FString &Error = TEXT("connection refused")) {
      bConnectFailed = true;
      ConnectionErrorEvent.Broadcast(Error);
    }

    // The socket closed without the client asking: a drop, or a server close.
    void Drop(int32 Code = 1006, const FString &Reason = FString()) {
      bConnected = false;
      ClosedEvent.Broadcast(Code, Reason, Code == 1000);
    }

    void Receive(
      const FString &JoinRef,
      const FString &Ref,
      const FString &Topic,
      const FString &Event,
      const TSharedPtr<FJsonObject> &Payload
    ) {
      MessageEvent.Broadcast(
        EncodeMessage(JoinRef, Ref, Topic, Event, Payload)
      );
    }

    void ReceiveRaw(const FString &Text) {
      MessageEvent.Broadcast(Text);
    }

    // A reply to one of the client's messages, as Phoenix sends it:
    // {status, response}.
    void Reply(
      const FPhoenixMessage &To,
      const FString &Status,
      const TSharedPtr<FJsonObject> &Response
    ) {
      TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
      Payload->SetStringField(TEXT("status"), Status);
      if (Response.IsValid()) {
        Payload->SetObjectField(TEXT("response"), Response);
      } else {
        Payload->SetField(TEXT("response"), MakeShared<FJsonValueNull>());
      }
      Receive(To.JoinRef, To.Ref, To.Topic, TEXT("phx_reply"), Payload);
    }

    // An `ok` reply to an rpc or a session step: {response: <Redwood object>}.
    // A null Redwood object is an event that sends no reply.
    void ReplyOk(
      const FPhoenixMessage &To, const TSharedPtr<FJsonObject> &RedwoodResponse
    ) {
      TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
      if (RedwoodResponse.IsValid()) {
        Response->SetObjectField(TEXT("response"), RedwoodResponse);
      } else {
        Response->SetField(TEXT("response"), MakeShared<FJsonValueNull>());
      }
      Reply(To, TEXT("ok"), Response);
    }

    // An `ok` reply whose Redwood object is {error: Error}.
    void ReplyWithError(const FPhoenixMessage &To, const FString &Error) {
      TSharedPtr<FJsonObject> Redwood = MakeShared<FJsonObject>();
      Redwood->SetStringField(TEXT("error"), Error);
      ReplyOk(To, Redwood);
    }

    // Bifrost's own refusal of a request: {error: {code, message}}.
    void Refuse(
      const FPhoenixMessage &To, const FString &Code, const FString &Message
    ) {
      TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
      Error->SetStringField(TEXT("code"), Code);
      Error->SetStringField(TEXT("message"), Message);
      TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
      Response->SetObjectField(TEXT("error"), Error);
      Reply(To, TEXT("error"), Response);
    }

    // A refused join: {reason, message?}.
    void RefuseJoin(
      const FPhoenixMessage &Join,
      const FString &Reason,
      const FString &Message = FString()
    ) {
      TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
      Response->SetStringField(TEXT("reason"), Reason);
      if (!Message.IsEmpty()) {
        Response->SetStringField(TEXT("message"), Message);
      }
      Reply(Join, TEXT("error"), Response);
    }

    // A Redwood push: `push {pushId, event, payload}`. An empty id sends none.
    void Push(
      const FString &PushId,
      const FString &Event,
      const TSharedPtr<FJsonObject> &Payload,
      const FString &JoinRef
    ) {
      TSharedPtr<FJsonObject> Message = MakeShared<FJsonObject>();
      if (!PushId.IsEmpty()) {
        Message->SetStringField(TEXT("pushId"), PushId);
      }
      Message->SetStringField(TEXT("event"), Event);
      Message->SetObjectField(
        TEXT("payload"), Payload.IsValid() ? Payload : MakeShared<FJsonObject>()
      );
      Receive(JoinRef, FString(), TEXT("session"), TEXT("push"), Message);
    }

    // A chat line: `chat.msg`, with the payload of FFakeBifrost::ChatLine.
    void ChatMessage(
      const TSharedPtr<FJsonObject> &Line, const FString &JoinRef
    ) {
      Receive(JoinRef, FString(), TEXT("session"), TEXT("chat.msg"), Line);
    }

    // The session's topics changed: `chat.topics {topics}`.
    void ChatTopics(const TArray<FString> &Topics, const FString &JoinRef) {
      TArray<TSharedPtr<FJsonValue>> Values;
      for (const FString &Topic : Topics) {
        Values.Add(MakeShared<FJsonValueString>(Topic));
      }
      TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
      Payload->SetArrayField(TEXT("topics"), Values);
      Receive(
        JoinRef, FString(), TEXT("session"), TEXT("chat.topics"), Payload
      );
    }

    // Bifrost ends the session: `closed {reason}`. The end of the channel
    // that follows it is a separate step (EndChannel), as on the wire.
    void CloseSession(const FString &Reason, const FString &JoinRef) {
      TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
      Payload->SetStringField(TEXT("reason"), Reason);
      Receive(JoinRef, FString(), TEXT("session"), TEXT("closed"), Payload);
    }

    // The channel process stopped: Phoenix sends `phx_close` on the join,
    // and the socket stays open.
    void EndChannel(const FString &JoinRef) {
      Receive(
        JoinRef,
        JoinRef,
        TEXT("session"),
        TEXT("phx_close"),
        MakeShared<FJsonObject>()
      );
    }

    // The newest message the client sent with this event (`rpc` counts by
    // its inner event), or an invalid message.
    FPhoenixMessage Last(const FString &Event) const {
      for (int32 Index = Sent.Num() - 1; Index >= 0; --Index) {
        if (Sent[Index].IsValid() && Sent[Index].RequestEvent() == Event) {
          return Sent[Index];
        }
      }
      return FPhoenixMessage();
    }

    int32 CountSent(const FString &Event) const {
      int32 Count = 0;
      for (const FPhoenixMessage &Message : Sent) {
        if (Message.IsValid() && Message.RequestEvent() == Event) {
          ++Count;
        }
      }
      return Count;
    }

  private:
    FWebSocketConnectedEvent ConnectedEvent;
    FWebSocketConnectionErrorEvent ConnectionErrorEvent;
    FWebSocketClosedEvent ClosedEvent;
    FWebSocketMessageEvent MessageEvent;
    FWebSocketBinaryMessageEvent BinaryMessageEvent;
    FWebSocketRawMessageEvent RawMessageEvent;
    FWebSocketMessageSentEvent MessageSentEvent;
  };

  // Hands fake sockets to every FRedwoodBifrostConnection made while it
  // exists, and keeps them so a test can read the one that was replaced.
  struct FFakeBifrost {
    TArray<TSharedPtr<FFakeBifrostSocket>> Sockets;

    FFakeBifrost() {
      FRedwoodBifrostConnection::CreateSocketOverride =
        [this](const FString &Uri) {
          TSharedPtr<FFakeBifrostSocket> Socket =
            MakeShared<FFakeBifrostSocket>(Uri);
          Sockets.Add(Socket);
          return StaticCastSharedPtr<IWebSocket>(Socket);
        };
    }

    ~FFakeBifrost() {
      FRedwoodBifrostConnection::CreateSocketOverride = nullptr;
    }

    // The socket the client holds now.
    TSharedPtr<FFakeBifrostSocket> Socket() const {
      return Sockets.Num() > 0 ? Sockets.Last() : nullptr;
    }

    // Opens the newest socket, as a server that accepts the connect. False
    // when there is no socket that can still connect: one that was opened,
    // failed or closed already is the OLD socket, and a test that opened it
    // again would pass with the client hearing nothing. Assert the result.
    bool Open() const {
      const TSharedPtr<FFakeBifrostSocket> Current = Socket();
      if (
        !Current.IsValid() || Current->bOpened || Current->bConnectFailed ||
        Current->bCloseCalled
      ) {
        return false;
      }
      Current->Open();
      return true;
    }

    // A join reply: {sessionId, resumeToken, player, realmId?,
    // lockedRealmId?, characterId?}. Player is the login answer or the
    // resume identity, as the test needs.
    static TSharedPtr<FJsonObject> JoinReply(
      const FString &SessionId,
      const FString &ResumeToken,
      const TSharedPtr<FJsonObject> &Player,
      const FString &RealmId = FString(),
      const FString &CharacterId = FString(),
      const FString &LockedRealmId = FString()
    ) {
      TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
      Reply->SetStringField(TEXT("sessionId"), SessionId);
      Reply->SetStringField(TEXT("resumeToken"), ResumeToken);
      Reply->SetObjectField(
        TEXT("player"), Player.IsValid() ? Player : MakeShared<FJsonObject>()
      );
      if (!RealmId.IsEmpty()) {
        Reply->SetStringField(TEXT("realmId"), RealmId);
      }
      if (!LockedRealmId.IsEmpty()) {
        Reply->SetStringField(TEXT("lockedRealmId"), LockedRealmId);
      }
      if (!CharacterId.IsEmpty()) {
        Reply->SetStringField(TEXT("characterId"), CharacterId);
      }
      return Reply;
    }

    // One chat line as Bifrost sends it (`chat.msg`, and a resume's
    // `messages`): {topic, seq, msgId, category, sender, template, args,
    // sentAt}. The body is the template; the args are empty.
    static TSharedPtr<FJsonObject> ChatLine(
      const FString &Topic,
      int64 Seq,
      const FString &MsgId,
      const FString &Body = TEXT("hello"),
      const FString &SentAt = TEXT("2026-10-23T12:00:00Z")
    ) {
      TSharedPtr<FJsonObject> Sender = MakeShared<FJsonObject>();
      Sender->SetStringField(TEXT("characterId"), TEXT("char-9"));
      Sender->SetStringField(TEXT("name"), TEXT("Bravo"));
      TSharedPtr<FJsonObject> Line = MakeShared<FJsonObject>();
      Line->SetStringField(TEXT("topic"), Topic);
      Line->SetNumberField(TEXT("seq"), static_cast<double>(Seq));
      Line->SetStringField(TEXT("msgId"), MsgId);
      Line->SetStringField(TEXT("category"), TEXT("Message.Chat.Party"));
      Line->SetObjectField(TEXT("sender"), Sender);
      Line->SetStringField(TEXT("template"), Body);
      Line->SetArrayField(TEXT("args"), TArray<TSharedPtr<FJsonValue>>());
      Line->SetStringField(TEXT("sentAt"), SentAt);
      return Line;
    }

    // One topic of a `chat.resume` reply: {messages, truncated}.
    static TSharedPtr<FJsonObject> ResumedTopic(
      const TArray<TSharedPtr<FJsonObject>> &Lines, bool bTruncated
    ) {
      TArray<TSharedPtr<FJsonValue>> Messages;
      for (const TSharedPtr<FJsonObject> &Line : Lines) {
        Messages.Add(MakeShared<FJsonValueObject>(Line));
      }
      TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
      Entry->SetArrayField(TEXT("messages"), Messages);
      Entry->SetBoolField(TEXT("truncated"), bTruncated);
      return Entry;
    }

    // The director's login answer inside a join reply.
    static TSharedPtr<FJsonObject> LoginPlayer(
      const FString &PlayerId,
      const FString &Token,
      const FString &Nickname = TEXT("Nick")
    ) {
      TSharedPtr<FJsonObject> Player = MakeShared<FJsonObject>();
      Player->SetStringField(TEXT("playerId"), PlayerId);
      Player->SetStringField(TEXT("username"), PlayerId);
      Player->SetStringField(TEXT("nickname"), Nickname);
      Player->SetStringField(TEXT("token"), Token);
      return Player;
    }
  };

  // FTimerManager ticks at most once per engine frame, and a test runs
  // inside one frame, so each step advances the frame counter itself.
  inline void TickTimers(FTimerManager &Timers, float Seconds) {
    ++GFrameCounter;
    Timers.Tick(Seconds);
  }
}
