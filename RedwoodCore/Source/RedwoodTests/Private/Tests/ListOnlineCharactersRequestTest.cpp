// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins the parse half of URedwoodServerGameSubsystem::RequestOnlineCharacters,
// the game-server side of the in-game /who all command:
//   1. A good answer yields every character with its name and zone.
//   2. An answer that is not an object, has no `error` field, or has no
//      `characters` array, is reported as an error and not as an empty
//      roster. AsObject would turn such an answer into a valid EMPTY object,
//      which reads as "nobody online".
//   3. A backend refusal carries its error string through unchanged.
//   4. An entry with no name is left out rather than shown as a blank line.
//   5. ListOnlineCharactersEventName still holds the wire name the backend
//      declares. The backend keeps its own copy of the string.
// An upstream merge must keep these APIs or update this file in lockstep.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "RedwoodServerGameSubsystem.h"

namespace {
  FRedwoodListOnlineCharactersOutput ParseOne(
    const TSharedPtr<FJsonValue> &ResponseValue
  ) {
    TArray<TSharedPtr<FJsonValue>> Response;
    Response.Add(ResponseValue);
    return URedwoodServerGameSubsystem::ParseListOnlineCharacters(Response);
  }

  TSharedPtr<FJsonObject> MakeEntry(const TCHAR *Name, const TCHAR *Zone) {
    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
    if (Name) {
      Entry->SetStringField(TEXT("name"), Name);
    }
    Entry->SetStringField(TEXT("zoneName"), Zone);
    return Entry;
  }

  TSharedPtr<FJsonObject> MakeAnswer(
    const FString &Error, TArray<TSharedPtr<FJsonObject>> Entries
  ) {
    TArray<TSharedPtr<FJsonValue>> Characters;
    for (const TSharedPtr<FJsonObject> &Entry : Entries) {
      Characters.Add(MakeShared<FJsonValueObject>(Entry));
    }
    TSharedPtr<FJsonObject> Answer = MakeShared<FJsonObject>();
    Answer->SetStringField(TEXT("error"), Error);
    Answer->SetArrayField(TEXT("characters"), Characters);
    return Answer;
  }
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodListOnlineCharactersAnswerTest,
  "Redwood.ListOnlineCharacters.Answer",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodListOnlineCharactersAnswerTest::RunTest(const FString &Parameters
) {
  TestEqual(
    TEXT("The wire name has not drifted"),
    FString(URedwoodServerGameSubsystem::ListOnlineCharactersEventName),
    TEXT("realm:servers:list-online-characters:game-server-to-sidecar")
  );

  const FRedwoodListOnlineCharactersOutput Roster =
    ParseOne(MakeShared<FJsonValueObject>(MakeAnswer(
      FString(),
      {MakeEntry(TEXT("WhoAlpha"), TEXT("main")),
       MakeEntry(nullptr, TEXT("main")),
       MakeEntry(TEXT("WhoBravo"), TEXT("other-zone"))}
    )));
  TestEqual(TEXT("A good answer carries no error"), Roster.Error, FString());
  TestEqual(
    TEXT("An entry with no name is left out"), Roster.Characters.Num(), 2
  );
  if (Roster.Characters.Num() == 2) {
    TestEqual(
      TEXT("The first name"), Roster.Characters[0].Name, TEXT("WhoAlpha")
    );
    TestEqual(
      TEXT("The first zone"), Roster.Characters[0].ZoneName, TEXT("main")
    );
    TestEqual(
      TEXT("The second name"), Roster.Characters[1].Name, TEXT("WhoBravo")
    );
    TestEqual(
      TEXT("The second zone"),
      Roster.Characters[1].ZoneName,
      TEXT("other-zone")
    );
  }

  const FRedwoodListOnlineCharactersOutput Refused = ParseOne(
    MakeShared<FJsonValueObject>(MakeAnswer(TEXT("realm unavailable"), {}))
  );
  TestEqual(
    TEXT("The backend words are not changed"),
    Refused.Error,
    TEXT("realm unavailable")
  );

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodListOnlineCharactersBadAnswerTest,
  "Redwood.ListOnlineCharacters.BadAnswer",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodListOnlineCharactersBadAnswerTest::RunTest(
  const FString &Parameters
) {
  TestFalse(
    TEXT("A null answer is an error, not an empty roster"),
    ParseOne(MakeShared<FJsonValueNull>()).Error.IsEmpty()
  );
  TestFalse(
    TEXT("A string answer is an error"),
    ParseOne(MakeShared<FJsonValueString>(TEXT("nope"))).Error.IsEmpty()
  );

  TSharedPtr<FJsonObject> NoErrorField = MakeShared<FJsonObject>();
  NoErrorField->SetArrayField(TEXT("characters"), {});
  TestFalse(
    TEXT("An object with no error field is not one of our answers"),
    ParseOne(MakeShared<FJsonValueObject>(NoErrorField)).Error.IsEmpty()
  );

  // A success with no array. The backend puts the array on every answer, so
  // this is not one of its answers, and it must not read as nobody online.
  TSharedPtr<FJsonObject> NoArray = MakeShared<FJsonObject>();
  NoArray->SetStringField(TEXT("error"), TEXT(""));
  TestFalse(
    TEXT("A success with no characters array is an error"),
    ParseOne(MakeShared<FJsonValueObject>(NoArray)).Error.IsEmpty()
  );

  TArray<TSharedPtr<FJsonValue>> Empty;
  TestFalse(
    TEXT("No answer at all is an error"),
    URedwoodServerGameSubsystem::ParseListOnlineCharacters(Empty)
      .Error.IsEmpty()
  );

  return true;
}
