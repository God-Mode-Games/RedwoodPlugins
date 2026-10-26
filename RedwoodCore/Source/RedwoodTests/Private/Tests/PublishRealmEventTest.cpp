// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins the three pure halves of URedwoodServerGameSubsystem::PublishRealmEvent,
// the game server's way to put a chat line on Bifrost through the realm:
//   1. The FRedwoodRealmEvent helpers build the topic strings Bifrost and the
//      game's chat routing also spell: `r:<realmId>/all`,
//      `r:<realmId>/char:<id>`, `r:<realmId>/party:<id>`.
//   2. MakePublishRealmEventPayload writes the wire shape: the kind word, ONE
//      target field chosen by the kind, the category word, the template, the
//      args in order, and a sender only when there is a name.
//   3. CheckRealmEvent refuses, before anything is sent, an empty template, a
//      template over 256 code points (code points, not TCHARs: a surrogate
//      pair is one), more than 16 args or an arg over 256 code points, a
//      topic event with no topic or with a part that has no id, and a session
//      event with no session id. The helpers refuse an empty id or one with a
//      `/` in it, with an ensure and an empty topic.
//   4. ParsePublishRealmEventAnswer carries the realm's error through word for
//      word, empty on success, and reports an answer that is not the realm's
//      as an error, never as a line that went out.
//   5. The wire name still holds what the backend declares.
// The sidecar socket itself has no fake here, so the emit and the inline
// answer with the sidecar down follow RequestPlayerRoleChange's covered shape.
// An upstream merge must keep these APIs or update this file in lockstep.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "RedwoodServerGameSubsystem.h"
#include "Types/RedwoodTypesEvents.h"

namespace RedwoodPublishRealmEventTest {
  // Names unique in the test module: the unity build merges this file with
  // tests whose anonymous namespaces define their own helpers.
  const TCHAR *const RealmId = TEXT("realm-1");

  // One character outside the basic plane. On a 16-bit TCHAR platform it is
  // a surrogate pair, built from its two units so the test cannot depend on
  // how a compiler encodes a `\U` escape; on a 32-bit TCHAR platform there is
  // no pair, and the test then only shows that the plain count still holds.
  FString AstralCharacter() {
    FString Text;
    if constexpr (sizeof(TCHAR) == 2) {
      Text.AppendChar(static_cast<TCHAR>(0xD83D));
      Text.AppendChar(static_cast<TCHAR>(0xDE00));
    } else {
      Text = TEXT("\U0001F600");
    }
    return Text;
  }

  FString Repeated(const FString &Piece, int32 Times) {
    FString Text;
    for (int32 Index = 0; Index < Times; ++Index) {
      Text += Piece;
    }
    return Text;
  }

  TSharedPtr<FJsonObject> PayloadFor(const FRedwoodRealmEvent &Event) {
    return URedwoodServerGameSubsystem::MakePublishRealmEventPayload(Event);
  }

  FString ParseAnswer(const TSharedPtr<FJsonValue> &ResponseValue) {
    TArray<TSharedPtr<FJsonValue>> Response;
    Response.Add(ResponseValue);
    return URedwoodServerGameSubsystem::ParsePublishRealmEventAnswer(Response);
  }

  FString ParseObject(const TSharedPtr<FJsonObject> &Object) {
    return ParseAnswer(MakeShared<FJsonValueObject>(Object));
  }

  TSharedPtr<FJsonObject> MakeAnswerObj(const TCHAR *Error) {
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("error"), Error);
    return Obj;
  }
} // namespace RedwoodPublishRealmEventTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodPublishRealmEventWireNameTest,
  "Redwood.PublishRealmEvent.WireName",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodPublishRealmEventWireNameTest::RunTest(const FString &Parameters) {
  // Pinned exactly as the RedwoodBackend fork writes it.
  TestEqual(
    TEXT("The publish route has not drifted"),
    FString(URedwoodServerGameSubsystem::PublishRealmEventName),
    TEXT("realm:events:publish")
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodPublishRealmEventHelpersTest,
  "Redwood.PublishRealmEvent.Helpers",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodPublishRealmEventHelpersTest::RunTest(const FString &Parameters) {
  using namespace RedwoodPublishRealmEventTest;
  // The topic strings, pinned exactly as Bifrost and the game's chat routing
  // spell them; each side keeps its own copy.
  const FRedwoodRealmEvent Wide =
    FRedwoodRealmEvent::RealmWide(RealmId, TEXT("server.restart"));
  TestTrue(
    TEXT("A realm-wide line is a topic event"),
    Wide.Kind == ERedwoodRealmEventKind::Topic
  );
  TestEqual(
    TEXT("On the realm's all topic"), Wide.Topic, TEXT("r:realm-1/all")
  );
  TestTrue(
    TEXT("Under the all category"),
    Wide.Category == ERedwoodRealmEventCategory::All
  );
  TestEqual(TEXT("With the template"), Wide.Template, TEXT("server.restart"));
  TestEqual(TEXT("With no args"), Wide.Args.Num(), 0);
  TestEqual(TEXT("With no sender"), Wide.SenderName, FString());
  TestEqual(TEXT("With no session id"), Wide.SessionId, FString());

  const FRedwoodRealmEvent Character =
    FRedwoodRealmEvent::ForCharacter(RealmId, TEXT("c1"), TEXT("mail.new"));
  TestTrue(
    TEXT("A character line is a topic event"),
    Character.Kind == ERedwoodRealmEventKind::Topic
  );
  TestEqual(
    TEXT("On the character's topic"), Character.Topic, TEXT("r:realm-1/char:c1")
  );
  TestTrue(
    TEXT("As a system line"),
    Character.Category == ERedwoodRealmEventCategory::System
  );

  const FRedwoodRealmEvent Party =
    FRedwoodRealmEvent::ForParty(RealmId, TEXT("p1"), TEXT("party.joined"));
  TestTrue(
    TEXT("A party line is a topic event"),
    Party.Kind == ERedwoodRealmEventKind::Topic
  );
  TestEqual(
    TEXT("On the party's topic"), Party.Topic, TEXT("r:realm-1/party:p1")
  );
  TestTrue(
    TEXT("Under the party category"),
    Party.Category == ERedwoodRealmEventCategory::Party
  );

  // Every helper's event can be sent as it is.
  TestEqual(
    TEXT("A realm-wide line passes the check"),
    URedwoodServerGameSubsystem::CheckRealmEvent(Wide),
    FString()
  );
  TestEqual(
    TEXT("A character line passes the check"),
    URedwoodServerGameSubsystem::CheckRealmEvent(Character),
    FString()
  );
  TestEqual(
    TEXT("A party line passes the check"),
    URedwoodServerGameSubsystem::CheckRealmEvent(Party),
    FString()
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodPublishRealmEventBadIdTest,
  "Redwood.PublishRealmEvent.BadId",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodPublishRealmEventBadIdTest::RunTest(const FString &Parameters) {
  using namespace RedwoodPublishRealmEventTest;
  // Each refusal below raises an ensure on purpose: a topic with no id, or
  // with a `/` in an id, names nobody or someone else, and Bifrost would
  // store the line under it for good.
  AddExpectedError(
    TEXT("Ensure condition failed"), EAutomationExpectedErrorFlags::Contains, 0
  );
  AddExpectedError(
    TEXT("Refused the realm topic"), EAutomationExpectedErrorFlags::Contains, 0
  );

  const FRedwoodRealmEvent Refused[] = {
    FRedwoodRealmEvent::RealmWide(FString(), TEXT("t")),
    FRedwoodRealmEvent::RealmWide(TEXT("realm/1"), TEXT("t")),
    FRedwoodRealmEvent::ForCharacter(RealmId, FString(), TEXT("t")),
    FRedwoodRealmEvent::ForCharacter(RealmId, TEXT("c/1"), TEXT("t")),
    FRedwoodRealmEvent::ForParty(RealmId, FString(), TEXT("t")),
    FRedwoodRealmEvent::ForParty(RealmId, TEXT("p/1"), TEXT("t")),
    FRedwoodRealmEvent::ForParty(FString(), TEXT("p1"), TEXT("t")),
  };
  for (const FRedwoodRealmEvent &Event : Refused) {
    TestEqual(TEXT("A bad id leaves no topic"), Event.Topic, FString());
    TestEqual(
      TEXT("So the event is refused before it is sent"),
      URedwoodServerGameSubsystem::CheckRealmEvent(Event),
      FString(URedwoodServerGameSubsystem::RealmEventNoTopicError)
    );
  }
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodPublishRealmEventPayloadTest,
  "Redwood.PublishRealmEvent.Payload",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodPublishRealmEventPayloadTest::RunTest(const FString &Parameters) {
  using namespace RedwoodPublishRealmEventTest;
  // A topic event with args and a sender: every field of the request.
  FRedwoodRealmEvent Party =
    FRedwoodRealmEvent::ForParty(RealmId, TEXT("p1"), TEXT("party.joined"));
  Party.Args = {TEXT("Aldric"), TEXT("Orrathis")};
  Party.SenderName = TEXT("Aldric");
  const TSharedPtr<FJsonObject> PartyPayload = PayloadFor(Party);
  TestEqual(
    TEXT("A topic event says so"),
    PartyPayload->GetStringField(TEXT("kind")),
    TEXT("topic")
  );
  TestEqual(
    TEXT("And names the topic"),
    PartyPayload->GetStringField(TEXT("topic")),
    TEXT("r:realm-1/party:p1")
  );
  TestFalse(
    TEXT("And carries no session id"), PartyPayload->HasField(TEXT("sessionId"))
  );
  TestEqual(
    TEXT("The party category word"),
    PartyPayload->GetStringField(TEXT("category")),
    TEXT("party")
  );
  TestEqual(
    TEXT("The template"),
    PartyPayload->GetStringField(TEXT("template")),
    TEXT("party.joined")
  );
  TArray<FString> Args;
  TestTrue(
    TEXT("The args are an array of strings"),
    PartyPayload->TryGetStringArrayField(TEXT("args"), Args)
  );
  TestEqual(TEXT("With every arg"), Args.Num(), 2);
  if (Args.Num() == 2) {
    TestEqual(TEXT("In order, first"), Args[0], TEXT("Aldric"));
    TestEqual(TEXT("In order, second"), Args[1], TEXT("Orrathis"));
  }
  const TSharedPtr<FJsonObject> *Sender = nullptr;
  TestTrue(
    TEXT("A named sender is an object"),
    PartyPayload->TryGetObjectField(TEXT("sender"), Sender)
  );
  if (Sender != nullptr) {
    TestEqual(
      TEXT("With the name"),
      (*Sender)->GetStringField(TEXT("name")),
      TEXT("Aldric")
    );
  }

  // No args is an empty array, not a missing field; no sender is no field.
  const TSharedPtr<FJsonObject> WidePayload =
    PayloadFor(FRedwoodRealmEvent::RealmWide(RealmId, TEXT("server.restart")));
  const TArray<TSharedPtr<FJsonValue>> *EmptyArgs = nullptr;
  TestTrue(
    TEXT("No args still sends the array"),
    WidePayload->TryGetArrayField(TEXT("args"), EmptyArgs)
  );
  if (EmptyArgs != nullptr) {
    TestEqual(TEXT("And it is empty"), EmptyArgs->Num(), 0);
  }
  TestFalse(
    TEXT("No sender sends no sender field"),
    WidePayload->HasField(TEXT("sender"))
  );
  TestEqual(
    TEXT("The all category word"),
    WidePayload->GetStringField(TEXT("category")),
    TEXT("all")
  );
  TestEqual(
    TEXT("The system category word"),
    PayloadFor(FRedwoodRealmEvent::ForCharacter(RealmId, TEXT("c1"), TEXT("t")))
      ->GetStringField(TEXT("category")),
    TEXT("system")
  );

  // A stray target is stripped, not sent: the kind alone decides where the
  // line goes, and a request with both fields would make the realm guess.
  FRedwoodRealmEvent StrayTopic =
    FRedwoodRealmEvent::RealmWide(RealmId, TEXT("server.restart"));
  StrayTopic.SessionId = TEXT("s1");
  TestFalse(
    TEXT("A topic event drops a stray session id"),
    PayloadFor(StrayTopic)->HasField(TEXT("sessionId"))
  );

  // A session event: the session id, and no topic, even one set by mistake.
  FRedwoodRealmEvent Session;
  Session.Kind = ERedwoodRealmEventKind::Session;
  Session.SessionId = TEXT("s1");
  Session.Topic = TEXT("r:realm-1/all");
  Session.Category = ERedwoodRealmEventCategory::Whisper;
  Session.Template = TEXT("whisper.offline");
  const TSharedPtr<FJsonObject> SessionPayload = PayloadFor(Session);
  TestEqual(
    TEXT("A session event says so"),
    SessionPayload->GetStringField(TEXT("kind")),
    TEXT("session")
  );
  TestEqual(
    TEXT("And names the session"),
    SessionPayload->GetStringField(TEXT("sessionId")),
    TEXT("s1")
  );
  TestFalse(
    TEXT("And carries no topic"), SessionPayload->HasField(TEXT("topic"))
  );
  TestEqual(
    TEXT("The whisper category word"),
    SessionPayload->GetStringField(TEXT("category")),
    TEXT("whisper")
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodPublishRealmEventCheckTest,
  "Redwood.PublishRealmEvent.Check",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodPublishRealmEventCheckTest::RunTest(const FString &Parameters) {
  using namespace RedwoodPublishRealmEventTest;
  const int32 Max =
    URedwoodServerGameSubsystem::RealmEventTemplateMaxCodePoints;

  TestEqual(
    TEXT("An empty template is refused"),
    URedwoodServerGameSubsystem::CheckRealmEvent(
      FRedwoodRealmEvent::RealmWide(RealmId, FString())
    ),
    FString(URedwoodServerGameSubsystem::RealmEventEmptyTemplateError)
  );

  // The longest template goes; one more code point does not.
  TestEqual(
    TEXT("A template at the limit passes"),
    URedwoodServerGameSubsystem::CheckRealmEvent(
      FRedwoodRealmEvent::RealmWide(RealmId, Repeated(TEXT("a"), Max))
    ),
    FString()
  );
  TestEqual(
    TEXT("A template one over the limit is refused"),
    URedwoodServerGameSubsystem::CheckRealmEvent(
      FRedwoodRealmEvent::RealmWide(RealmId, Repeated(TEXT("a"), Max + 1))
    ),
    FString(URedwoodServerGameSubsystem::RealmEventTemplateTooLongError)
  );

  // Code points, not TCHARs. The same count of astral characters is twice as
  // many TCHARs on a 16-bit platform, and must still pass; a count of TCHARs
  // would refuse a template Bifrost accepts.
  const FString Astral = Repeated(AstralCharacter(), Max);
  TestEqual(
    TEXT("The astral template holds one unit per code point, or two"),
    Astral.Len(),
    Max * (sizeof(TCHAR) == 2 ? 2 : 1)
  );
  TestEqual(
    TEXT("A template of astral characters at the limit passes"),
    URedwoodServerGameSubsystem::CheckRealmEvent(
      FRedwoodRealmEvent::RealmWide(RealmId, Astral)
    ),
    FString()
  );
  TestEqual(
    TEXT("One astral character over the limit is refused"),
    URedwoodServerGameSubsystem::CheckRealmEvent(
      FRedwoodRealmEvent::RealmWide(RealmId, Astral + AstralCharacter())
    ),
    FString(URedwoodServerGameSubsystem::RealmEventTemplateTooLongError)
  );

  // The args: at most 16, each at most 256 code points, counted the same way.
  const int32 MaxArgs = URedwoodServerGameSubsystem::RealmEventArgsMax;
  const int32 MaxArg = URedwoodServerGameSubsystem::RealmEventArgMaxCodePoints;
  FRedwoodRealmEvent ManyArgs =
    FRedwoodRealmEvent::RealmWide(RealmId, TEXT("t"));
  for (int32 Index = 0; Index < MaxArgs; ++Index) {
    ManyArgs.Args.Add(TEXT("a"));
  }
  TestEqual(
    TEXT("The most args pass"),
    URedwoodServerGameSubsystem::CheckRealmEvent(ManyArgs),
    FString()
  );
  ManyArgs.Args.Add(TEXT("a"));
  TestEqual(
    TEXT("One arg too many is refused"),
    URedwoodServerGameSubsystem::CheckRealmEvent(ManyArgs),
    FString(URedwoodServerGameSubsystem::RealmEventTooManyArgsError)
  );

  FRedwoodRealmEvent LongArg =
    FRedwoodRealmEvent::RealmWide(RealmId, TEXT("t"));
  LongArg.Args.Add(Repeated(AstralCharacter(), MaxArg));
  TestEqual(
    TEXT("An arg at the limit passes, in code points"),
    URedwoodServerGameSubsystem::CheckRealmEvent(LongArg),
    FString()
  );
  LongArg.Args[0] += TEXT("a");
  TestEqual(
    TEXT("An arg one over the limit is refused"),
    URedwoodServerGameSubsystem::CheckRealmEvent(LongArg),
    FString(URedwoodServerGameSubsystem::RealmEventArgTooLongError)
  );

  // A topic with a part that has no id, as an empty realm, character or party
  // id would leave it. These reach the check only when a caller builds the
  // topic by hand; the helpers refuse the id first (the BadId test).
  const TCHAR *const BadTopics[] = {
    TEXT("r:/all"),
    TEXT("r:realm-1/char:"),
    TEXT("r:realm-1//all"),
    TEXT("r:realm-1/party:/x"),
    TEXT("r:realm-1/")
  };
  for (const TCHAR *BadTopic : BadTopics) {
    FRedwoodRealmEvent Event;
    Event.Kind = ERedwoodRealmEventKind::Topic;
    Event.Topic = BadTopic;
    Event.Template = TEXT("t");
    TestEqual(
      *FString::Printf(TEXT("The topic '%s' is refused"), BadTopic),
      URedwoodServerGameSubsystem::CheckRealmEvent(Event),
      FString(URedwoodServerGameSubsystem::RealmEventBadTopicError)
    );
  }

  // A kind without its target. The payload builder never writes the other
  // field, so this is the only place a mismatch can be caught.
  FRedwoodRealmEvent NoTopic;
  NoTopic.Kind = ERedwoodRealmEventKind::Topic;
  NoTopic.Template = TEXT("t");
  NoTopic.SessionId = TEXT("s1");
  TestEqual(
    TEXT("A topic event with no topic is refused"),
    URedwoodServerGameSubsystem::CheckRealmEvent(NoTopic),
    FString(URedwoodServerGameSubsystem::RealmEventNoTopicError)
  );

  FRedwoodRealmEvent NoSession;
  NoSession.Kind = ERedwoodRealmEventKind::Session;
  NoSession.Template = TEXT("t");
  NoSession.Topic = TEXT("r:realm-1/all");
  TestEqual(
    TEXT("A session event with no session id is refused"),
    URedwoodServerGameSubsystem::CheckRealmEvent(NoSession),
    FString(URedwoodServerGameSubsystem::RealmEventNoSessionError)
  );
  NoSession.SessionId = TEXT("s1");
  TestEqual(
    TEXT("A session event with a session id passes"),
    URedwoodServerGameSubsystem::CheckRealmEvent(NoSession),
    FString()
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodPublishRealmEventAnswerTest,
  "Redwood.PublishRealmEvent.Answer",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodPublishRealmEventAnswerTest::RunTest(const FString &Parameters) {
  using namespace RedwoodPublishRealmEventTest;
  TestEqual(
    TEXT("An empty error is success"),
    ParseObject(MakeAnswerObj(TEXT(""))),
    FString()
  );
  // The realm's words go back unchanged: the game turns them into a log line.
  TestEqual(
    TEXT("The not-configured refusal is carried"),
    ParseObject(MakeAnswerObj(TEXT("Publish is not configured"))),
    TEXT("Publish is not configured")
  );
  TestEqual(
    TEXT("A scope refusal is carried"),
    ParseObject(MakeAnswerObj(TEXT("not hosted"))),
    TEXT("not hosted")
  );

  // Every answer that is not the realm's is an error, so a line that may
  // never have been sent is never reported as sent.
  TestFalse(
    TEXT("A null answer is an error"),
    ParseAnswer(MakeShared<FJsonValueNull>()).IsEmpty()
  );
  TestFalse(
    TEXT("A string answer is an error"),
    ParseAnswer(MakeShared<FJsonValueString>(TEXT("ok"))).IsEmpty()
  );
  TestFalse(
    TEXT("An answer with no value is an error"), ParseAnswer(nullptr).IsEmpty()
  );
  TArray<TSharedPtr<FJsonValue>> Empty;
  TestFalse(
    TEXT("An empty answer is an error"),
    URedwoodServerGameSubsystem::ParsePublishRealmEventAnswer(Empty).IsEmpty()
  );
  TestFalse(
    TEXT("An object with no error field is an error"),
    ParseObject(MakeShared<FJsonObject>()).IsEmpty()
  );
  // An error that is not a string. The engine's TryGetStringField would make
  // "0" out of it, and report a refusal with those words.
  TSharedPtr<FJsonObject> NumberError = MakeShared<FJsonObject>();
  NumberError->SetNumberField(TEXT("error"), 0);
  TestEqual(
    TEXT("A number in the error field is not an answer"),
    ParseObject(NumberError),
    TEXT("The backend did not answer the publish")
  );
  return true;
}
