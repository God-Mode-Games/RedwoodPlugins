// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins the parse halves of the game server's copy of an account-wide chat
// mute: URedwoodServerGameSubsystem::RequestPlayerMute (the realm's answer)
// and the `realm:moderation:mute-changed` push:
//   1. A good answer yields the three fields; `muted` tells a player with no
//      mute from one muted with no end, which `until` alone cannot.
//   2. An answer that is not an object, has no `error` string, or has `muted`
//      or `until` of the wrong type, is reported as an error and never as
//      "not muted". AsObject would turn such an answer into a valid EMPTY
//      object, which reads as a player free to speak.
//   3. A backend refusal carries its error string through unchanged.
//   4. A push is read the same way, and one that cannot be read is refused
//      (false), because a half-read push could clear a mute that stands.
//   5. The wire names still hold what the backend declares. The backend keeps
//      its own copy of each string.
// The sidecar socket itself has no fake here, so the emit and the inline
// answer with the sidecar down follow RequestPlayerRoleChange's covered shape.
// An upstream merge must keep these APIs or update this file in lockstep.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "RedwoodServerGameSubsystem.h"

namespace RedwoodPlayerMuteTest {
  // 2026-10-24 13:00:00 UTC in Unix ms: past 2^31, so a 32-bit read shows.
  constexpr int64 UntilMs = 1792846800000LL;

  FRedwoodPlayerMuteOutput ParseOne(
    const TSharedPtr<FJsonValue> &ResponseValue
  ) {
    TArray<TSharedPtr<FJsonValue>> Response;
    Response.Add(ResponseValue);
    return URedwoodServerGameSubsystem::ParsePlayerMute(Response);
  }

  FRedwoodPlayerMuteOutput ParseObject(const TSharedPtr<FJsonObject> &Object) {
    return ParseOne(MakeShared<FJsonValueObject>(Object));
  }

  // The realm's answer for a mute, or for none.
  TSharedPtr<FJsonObject> MakeMuteObj(
    bool bMuted, double Until, const TCHAR *Reason
  ) {
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("error"), TEXT(""));
    Obj->SetBoolField(TEXT("muted"), bMuted);
    Obj->SetNumberField(TEXT("until"), Until);
    Obj->SetStringField(TEXT("reason"), Reason);
    return Obj;
  }

  bool ParseChanged(
    const TSharedPtr<FJsonValue> &Message, FRedwoodPlayerMuteChanged &Out
  ) {
    return URedwoodServerGameSubsystem::ParseMuteChanged(Message, Out);
  }
} // namespace RedwoodPlayerMuteTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodPlayerMuteWireNamesTest,
  "Redwood.PlayerMute.WireNames",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodPlayerMuteWireNamesTest::RunTest(const FString &Parameters) {
  using namespace RedwoodPlayerMuteTest;
  // Pinned exactly as the RedwoodBackend fork writes them.
  TestEqual(
    TEXT("The get route has not drifted"),
    FString(URedwoodServerGameSubsystem::GetPlayerMuteEventName),
    TEXT("realm:moderation:mute:get")
  );
  TestEqual(
    TEXT("The push name has not drifted"),
    FString(URedwoodServerGameSubsystem::MuteChangedEventName),
    TEXT("realm:moderation:mute-changed")
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodPlayerMuteAnswerTest,
  "Redwood.PlayerMute.Answer",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodPlayerMuteAnswerTest::RunTest(const FString &Parameters) {
  using namespace RedwoodPlayerMuteTest;
  const FRedwoodPlayerMuteOutput WithEnd =
    ParseObject(MakeMuteObj(true, static_cast<double>(UntilMs), TEXT("Spam.")));
  TestEqual(TEXT("A good answer has no error"), WithEnd.Error, FString());
  TestTrue(TEXT("The player is muted"), WithEnd.bMuted);
  TestEqual(TEXT("Until the end the realm holds"), WithEnd.Until, UntilMs);
  TestEqual(TEXT("For the reason"), WithEnd.Reason, TEXT("Spam."));

  // Muted with no end and not muted both carry until 0: `muted` decides.
  const FRedwoodPlayerMuteOutput NoEnd =
    ParseObject(MakeMuteObj(true, 0.0, TEXT("Bot.")));
  TestTrue(TEXT("A mute with no end is a mute"), NoEnd.bMuted);
  TestEqual(TEXT("With until 0"), NoEnd.Until, static_cast<int64>(0));

  const FRedwoodPlayerMuteOutput None =
    ParseObject(MakeMuteObj(false, 0.0, TEXT("")));
  TestEqual(TEXT("No mute has no error"), None.Error, FString());
  TestFalse(TEXT("No mute is not a mute"), None.bMuted);

  // A reason left out is an empty reason, not a bad answer.
  TSharedPtr<FJsonObject> NoReason = MakeMuteObj(true, 0.0, TEXT(""));
  NoReason->RemoveField(TEXT("reason"));
  const FRedwoodPlayerMuteOutput Unreasoned = ParseObject(NoReason);
  TestEqual(TEXT("No reason field still reads"), Unreasoned.Error, FString());
  TestTrue(TEXT("And keeps the mute"), Unreasoned.bMuted);

  // A refusal: the backend words go back unchanged, and nothing is muted.
  TSharedPtr<FJsonObject> RefusedObj = MakeShared<FJsonObject>();
  RefusedObj->SetStringField(TEXT("error"), TEXT("not hosted"));
  const FRedwoodPlayerMuteOutput Refused = ParseObject(RefusedObj);
  TestEqual(
    TEXT("The backend words are not changed"), Refused.Error, TEXT("not hosted")
  );
  TestFalse(TEXT("A refusal mutes nobody"), Refused.bMuted);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodPlayerMuteBadAnswerTest,
  "Redwood.PlayerMute.BadAnswer",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodPlayerMuteBadAnswerTest::RunTest(const FString &Parameters) {
  using namespace RedwoodPlayerMuteTest;
  // Every answer that is not the backend's keeps an error and mutes nobody,
  // so a muted player is never read as free to speak by a bad read.
  auto IsRefusedAsBad =
    [this](const TCHAR *What, const FRedwoodPlayerMuteOutput &Out) {
      TestFalse(
        *FString::Printf(TEXT("%s is an error"), What), Out.Error.IsEmpty()
      );
      TestFalse(*FString::Printf(TEXT("%s mutes nobody"), What), Out.bMuted);
    };

  IsRefusedAsBad(TEXT("A null answer"), ParseOne(MakeShared<FJsonValueNull>()));
  IsRefusedAsBad(
    TEXT("A string answer"),
    ParseOne(MakeShared<FJsonValueString>(TEXT("nope")))
  );
  IsRefusedAsBad(TEXT("An answer with no value"), ParseOne(nullptr));
  TArray<TSharedPtr<FJsonValue>> Empty;
  IsRefusedAsBad(
    TEXT("An empty answer"), URedwoodServerGameSubsystem::ParsePlayerMute(Empty)
  );
  IsRefusedAsBad(
    TEXT("An object with no error field"),
    ParseObject(MakeShared<FJsonObject>())
  );

  // An error that is not a string. The engine's TryGetStringField would make
  // "0" out of it, and report a refusal with those words.
  TSharedPtr<FJsonObject> NumberError = MakeMuteObj(true, 0.0, TEXT("Spam."));
  NumberError->SetNumberField(TEXT("error"), 0);
  IsRefusedAsBad(TEXT("A number in the error field"), ParseObject(NumberError));

  // A success whose fields are of the wrong type: `muted` as a string would
  // read false, and `until` as a string would read 0, so both are refused.
  TSharedPtr<FJsonObject> StringMuted = MakeMuteObj(true, 0.0, TEXT("Spam."));
  StringMuted->SetStringField(TEXT("muted"), TEXT("true"));
  IsRefusedAsBad(TEXT("A string in the muted field"), ParseObject(StringMuted));

  TSharedPtr<FJsonObject> NoMuted = MakeMuteObj(true, 0.0, TEXT("Spam."));
  NoMuted->RemoveField(TEXT("muted"));
  IsRefusedAsBad(TEXT("No muted field"), ParseObject(NoMuted));

  TSharedPtr<FJsonObject> StringUntil = MakeMuteObj(true, 0.0, TEXT("Spam."));
  StringUntil->SetStringField(TEXT("until"), TEXT("soon"));
  IsRefusedAsBad(TEXT("A string in the until field"), ParseObject(StringUntil));

  IsRefusedAsBad(
    TEXT("A negative until"),
    ParseObject(MakeMuteObj(true, -1.0, TEXT("Spam.")))
  );

  TSharedPtr<FJsonObject> NumberReason = MakeMuteObj(true, 0.0, TEXT(""));
  NumberReason->SetNumberField(TEXT("reason"), 7);
  IsRefusedAsBad(
    TEXT("A number in the reason field"), ParseObject(NumberReason)
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodPlayerMuteChangedTest,
  "Redwood.PlayerMute.Changed",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodPlayerMuteChangedTest::RunTest(const FString &Parameters) {
  using namespace RedwoodPlayerMuteTest;
  // A set: the player, the mute and its end.
  TSharedPtr<FJsonObject> SetObj = MakeShared<FJsonObject>();
  SetObj->SetStringField(TEXT("playerId"), TEXT("p1"));
  SetObj->SetBoolField(TEXT("muted"), true);
  SetObj->SetNumberField(TEXT("until"), static_cast<double>(UntilMs));
  SetObj->SetStringField(TEXT("reason"), TEXT("Spam."));
  FRedwoodPlayerMuteChanged Set;
  TestTrue(
    TEXT("A set push reads"),
    ParseChanged(MakeShared<FJsonValueObject>(SetObj), Set)
  );
  TestEqual(TEXT("It names the player"), Set.PlayerId, TEXT("p1"));
  TestTrue(TEXT("It mutes"), Set.bMuted);
  TestEqual(TEXT("Until the end"), Set.Until, UntilMs);
  TestEqual(TEXT("For the reason"), Set.Reason, TEXT("Spam."));

  // A clear: muted false, until 0, reason "".
  TSharedPtr<FJsonObject> ClearObj = MakeShared<FJsonObject>();
  ClearObj->SetStringField(TEXT("playerId"), TEXT("p1"));
  ClearObj->SetBoolField(TEXT("muted"), false);
  ClearObj->SetNumberField(TEXT("until"), 0.0);
  ClearObj->SetStringField(TEXT("reason"), TEXT(""));
  FRedwoodPlayerMuteChanged Clear;
  TestTrue(
    TEXT("A clear push reads"),
    ParseChanged(MakeShared<FJsonValueObject>(ClearObj), Clear)
  );
  TestFalse(TEXT("It clears"), Clear.bMuted);
  TestEqual(TEXT("With no end"), Clear.Until, static_cast<int64>(0));

  // A push that is not the backend's is refused, whatever it half-says: a
  // half-read push could clear a mute that stands.
  FRedwoodPlayerMuteChanged Dropped;
  TestFalse(
    TEXT("A null push is refused"),
    ParseChanged(MakeShared<FJsonValueNull>(), Dropped)
  );
  TestFalse(
    TEXT("A push with no value is refused"), ParseChanged(nullptr, Dropped)
  );

  TSharedPtr<FJsonObject> NoPlayer = MakeShared<FJsonObject>();
  NoPlayer->SetBoolField(TEXT("muted"), false);
  NoPlayer->SetNumberField(TEXT("until"), 0.0);
  TestFalse(
    TEXT("A push naming no player is refused"),
    ParseChanged(MakeShared<FJsonValueObject>(NoPlayer), Dropped)
  );

  TSharedPtr<FJsonObject> EmptyPlayer = MakeShared<FJsonObject>();
  EmptyPlayer->SetStringField(TEXT("playerId"), TEXT(""));
  EmptyPlayer->SetBoolField(TEXT("muted"), false);
  EmptyPlayer->SetNumberField(TEXT("until"), 0.0);
  TestFalse(
    TEXT("A push naming an empty player is refused"),
    ParseChanged(MakeShared<FJsonValueObject>(EmptyPlayer), Dropped)
  );

  TSharedPtr<FJsonObject> StringMuted = MakeShared<FJsonObject>();
  StringMuted->SetStringField(TEXT("playerId"), TEXT("p1"));
  StringMuted->SetStringField(TEXT("muted"), TEXT("false"));
  StringMuted->SetNumberField(TEXT("until"), 0.0);
  TestFalse(
    TEXT("A push with muted of the wrong type is refused"),
    ParseChanged(MakeShared<FJsonValueObject>(StringMuted), Dropped)
  );
  return true;
}
