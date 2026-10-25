// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins the client half of account-wide chat mutes, the GM's /mute and
// /unmute, on the fake Bifrost:
//   1. SetMute and ClearMute are director requests: refused inline with
//      "Not connected to Director." before a login, with nothing sent.
//   2. SetMute sends `director:moderation:mute:set` with the target name,
//      the duration as a JSON number and the reason, and reads the end the
//      director committed; ClearMute sends `director:moderation:mute:clear`
//      with the target name alone.
//   3. A director refusal goes back word for word. An answer with no error
//      string is a bad answer, not a mute that was made.
// The wire names are pinned against the RedwoodBackend fork's copies.
// An upstream merge must keep these APIs or update this file in lockstep.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "BifrostClientHarness.h"
#include "RedwoodCommonGameSubsystem.h"

namespace {
  const TCHAR *const TargetName = TEXT("Old Junk");
  const TCHAR *const SetMuteEvent = TEXT("director:moderation:mute:set");
  const TCHAR *const ClearMuteEvent = TEXT("director:moderation:mute:clear");
  // Two hours, as the game's /mute 2h sends it.
  constexpr int64 TwoHoursMs = 2 * 60 * 60 * 1000;
  // 2026-10-24 13:00:00 UTC in Unix ms: an end past 2^31, so a 32-bit read
  // would show.
  constexpr int64 UntilMs = 1792846800000LL;
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostMuteGateTest,
  "Redwood.Bifrost.Mute.RefusedBeforeLogin",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostMuteGateTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostClientTest;
  FClientHarness H(*this);
  H.Open();

  TCapture<FRedwoodSetMuteOutput> Set;
  H.Client()->SetMute(TargetName, TwoHoursMs, TEXT("Spam."), Set.Delegate());
  TestEqual(TEXT("A set before login is answered inline"), Set.Count, 1);
  TestEqual(
    TEXT("With the director gate's error"),
    Set.Last.Error,
    NotConnectedToDirector
  );
  TestEqual(TEXT("Nothing was sent"), H.Socket()->CountSent(SetMuteEvent), 0);

  TCapture<FString> Clear;
  H.Client()->ClearMute(TargetName, Clear.Delegate());
  TestEqual(TEXT("A clear before login is answered inline"), Clear.Count, 1);
  TestEqual(
    TEXT("With the director gate's error"), Clear.Last, NotConnectedToDirector
  );
  TestEqual(TEXT("Nothing was sent"), H.Socket()->CountSent(ClearMuteEvent), 0);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostMuteSetTest,
  "Redwood.Bifrost.Mute.SetSendsAndReadsTheEnd",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostMuteSetTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostClientTest;
  FClientHarness H(*this);
  H.Open();
  H.LoginOk();

  TCapture<FRedwoodSetMuteOutput> Set;
  H.Client()->SetMute(TargetName, TwoHoursMs, TEXT("Spam."), Set.Delegate());
  const FPhoenixMessage Request = H.Socket()->Last(SetMuteEvent);
  if (!TestTrue(
        TEXT("The set went out as a director rpc"), Request.IsValid()
      )) {
    return false;
  }
  const TSharedPtr<FJsonObject> Payload =
    Request.Payload->GetObjectField(TEXT("payload"));
  TestEqual(
    TEXT("With the target's character name"),
    Payload->GetStringField(TEXT("targetCharacterName")),
    TargetName
  );
  // A number, not a string: the backend declares durationMs a number.
  double DurationMs = -1.0;
  TestTrue(
    TEXT("With the duration as a number"),
    Payload->TryGetNumberField(TEXT("durationMs"), DurationMs)
  );
  TestEqual(
    TEXT("Of the right count"), static_cast<int64>(DurationMs), TwoHoursMs
  );
  TestEqual(
    TEXT("With the reason"),
    Payload->GetStringField(TEXT("reason")),
    TEXT("Spam.")
  );
  TestEqual(TEXT("Not answered before the reply"), Set.Count, 0);

  TSharedPtr<FJsonObject> Recorded = NoError();
  Recorded->SetNumberField(TEXT("until"), static_cast<double>(UntilMs));
  H.Socket()->ReplyOk(Request, Recorded);
  TestEqual(TEXT("The reply answers the set"), Set.Count, 1);
  TestEqual(TEXT("As a success"), Set.Last.Error, FString());
  TestEqual(
    TEXT("With the end the director committed"), Set.Last.Until, UntilMs
  );

  // A mute with no end: the director answers until 0.
  TCapture<FRedwoodSetMuteOutput> Forever;
  H.Client()->SetMute(TargetName, 0, TEXT("Bot."), Forever.Delegate());
  TSharedPtr<FJsonObject> NoEnd = NoError();
  NoEnd->SetNumberField(TEXT("until"), 0.0);
  H.Socket()->ReplyOk(H.Socket()->Last(SetMuteEvent), NoEnd);
  TestEqual(TEXT("A no-end set is answered"), Forever.Count, 1);
  TestEqual(TEXT("As a success"), Forever.Last.Error, FString());
  TestEqual(TEXT("With no end"), Forever.Last.Until, static_cast<int64>(0));

  // A refusal: the director's words, unchanged, and no end.
  TCapture<FRedwoodSetMuteOutput> Refused;
  H.Client()->SetMute(TargetName, 0, TEXT("Bot."), Refused.Delegate());
  H.Socket()->ReplyWithError(
    H.Socket()->Last(SetMuteEvent), TEXT("no expiry refused")
  );
  TestEqual(TEXT("A refusal is answered"), Refused.Count, 1);
  TestEqual(
    TEXT("With the director's words"),
    Refused.Last.Error,
    TEXT("no expiry refused")
  );
  TestEqual(TEXT("And no end"), Refused.Last.Until, static_cast<int64>(0));

  // An answer with no error string is not the director's: a bad answer, not
  // a mute that was made.
  TCapture<FRedwoodSetMuteOutput> Bad;
  H.Client()->SetMute(TargetName, TwoHoursMs, TEXT("Spam."), Bad.Delegate());
  TSharedPtr<FJsonObject> UntilOnly = MakeShared<FJsonObject>();
  UntilOnly->SetNumberField(TEXT("until"), static_cast<double>(UntilMs));
  H.Socket()->ReplyOk(H.Socket()->Last(SetMuteEvent), UntilOnly);
  TestEqual(TEXT("A bad answer is answered"), Bad.Count, 1);
  TestEqual(
    TEXT("As a bad answer"),
    Bad.Last.Error,
    URedwoodCommonGameSubsystem::BadRealmAnswerError
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodBifrostMuteClearTest,
  "Redwood.Bifrost.Mute.ClearSendsTheName",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodBifrostMuteClearTest::RunTest(const FString &Parameters) {
  using namespace RedwoodBifrostClientTest;
  FClientHarness H(*this);
  H.Open();
  H.LoginOk();

  TCapture<FString> Clear;
  H.Client()->ClearMute(TargetName, Clear.Delegate());
  const FPhoenixMessage Request = H.Socket()->Last(ClearMuteEvent);
  if (!TestTrue(
        TEXT("The clear went out as a director rpc"), Request.IsValid()
      )) {
    return false;
  }
  const TSharedPtr<FJsonObject> Payload =
    Request.Payload->GetObjectField(TEXT("payload"));
  TestEqual(
    TEXT("With the target's character name"),
    Payload->GetStringField(TEXT("targetCharacterName")),
    TargetName
  );
  TestEqual(TEXT("And nothing else"), Payload->Values.Num(), 1);

  TSharedPtr<FJsonObject> Cleared = NoError();
  Cleared->SetNumberField(TEXT("until"), 0.0);
  H.Socket()->ReplyOk(Request, Cleared);
  TestEqual(TEXT("The reply answers the clear"), Clear.Count, 1);
  TestEqual(TEXT("As a success"), Clear.Last, FString());

  TCapture<FString> NotMuted;
  H.Client()->ClearMute(TargetName, NotMuted.Delegate());
  H.Socket()->ReplyWithError(
    H.Socket()->Last(ClearMuteEvent), TEXT("not muted")
  );
  TestEqual(TEXT("A refusal is answered"), NotMuted.Count, 1);
  TestEqual(
    TEXT("With the director's words"), NotMuted.Last, TEXT("not muted")
  );
  return true;
}
