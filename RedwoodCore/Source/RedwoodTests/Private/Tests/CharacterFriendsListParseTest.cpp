// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins the parse of the three arrays the fork adds to the "realm:contacts:list"
// answer (friends, incomingRequests, outgoingRequests), read by
// URedwoodClientInterface::ListCharacterFriends:
//   1. A good answer gives each row its id, name, online flag and zone.
//   2. A request row has no online flag and no zone; it keeps the defaults.
//   3. A refused answer carries its error unchanged and gives no rows.
//   4. An answer with no `error` field is reported as an error, not as an
//      empty list. AsObject turns a bad answer into a valid EMPTY object,
//      which would read as "no friends".
//   5. A row with no characterId is left out. The game keys on the id.
// The field names below must equal the names in the RedwoodBackend fork,
// packages/common/src/interfaces.ts (Realms.Contacts.List.SResponse).

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "RedwoodCommonGameSubsystem.h"
#include "Types/RedwoodTypesCharacters.h"

namespace {
  TSharedPtr<FJsonObject> MakeRow(
    const TCHAR *Id, const TCHAR *Name, bool bOnline, const TCHAR *Zone
  ) {
    TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
    if (Id) {
      Row->SetStringField(TEXT("characterId"), Id);
    }
    Row->SetStringField(TEXT("characterName"), Name);
    Row->SetBoolField(TEXT("online"), bOnline);
    Row->SetStringField(TEXT("zoneName"), Zone);
    return Row;
  }

  TSharedPtr<FJsonObject> MakeRequestRow(const TCHAR *Id, const TCHAR *Name) {
    TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
    Row->SetStringField(TEXT("characterId"), Id);
    Row->SetStringField(TEXT("characterName"), Name);
    return Row;
  }

  TArray<TSharedPtr<FJsonValue>> AsValues(
    TArray<TSharedPtr<FJsonObject>> Rows
  ) {
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const TSharedPtr<FJsonObject> &Row : Rows) {
      Values.Add(MakeShared<FJsonValueObject>(Row));
    }
    return Values;
  }

  TSharedPtr<FJsonObject> MakeAnswer(const FString &Error) {
    TSharedPtr<FJsonObject> Answer = MakeShared<FJsonObject>();
    Answer->SetStringField(TEXT("error"), Error);
    Answer->SetArrayField(TEXT("contacts"), {});
    Answer->SetArrayField(TEXT("blockedContacts"), {});
    Answer->SetArrayField(TEXT("friends"), {});
    Answer->SetArrayField(TEXT("incomingRequests"), {});
    Answer->SetArrayField(TEXT("outgoingRequests"), {});
    return Answer;
  }
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCharacterFriendsListParseTest,
  "Redwood.CharacterFriends.ListParse",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodCharacterFriendsListParseTest::RunTest(const FString &Parameters) {
  TSharedPtr<FJsonObject> Answer = MakeAnswer(FString());
  Answer->SetArrayField(
    TEXT("friends"),
    AsValues(
      {MakeRow(TEXT("char-online"), TEXT("Alpha"), true, TEXT("L_Freewind")),
       MakeRow(TEXT("char-offline"), TEXT("Bravo"), false, TEXT("")),
       MakeRow(nullptr, TEXT("NoId"), true, TEXT("L_Freewind"))}
    )
  );
  Answer->SetArrayField(
    TEXT("incomingRequests"),
    AsValues({MakeRequestRow(TEXT("char-in"), TEXT("Charlie"))})
  );
  Answer->SetArrayField(
    TEXT("outgoingRequests"),
    AsValues({MakeRequestRow(TEXT("char-out"), TEXT("Delta"))})
  );

  const FRedwoodListCharacterFriendsOutput Output =
    URedwoodCommonGameSubsystem::ParseListCharacterFriends(Answer);

  TestEqual(TEXT("A good answer carries no error"), Output.Error, FString());
  TestEqual(TEXT("A row with no id is left out"), Output.Friends.Num(), 2);
  if (Output.Friends.Num() == 2) {
    TestEqual(TEXT("Friend id"), Output.Friends[0].CharacterId, TEXT("char-online"));
    TestEqual(TEXT("Friend name"), Output.Friends[0].CharacterName, TEXT("Alpha"));
    TestTrue(TEXT("Friend online"), Output.Friends[0].bOnline);
    TestEqual(TEXT("Friend zone"), Output.Friends[0].ZoneName, TEXT("L_Freewind"));
    TestFalse(TEXT("Offline friend"), Output.Friends[1].bOnline);
    TestEqual(TEXT("Offline friend zone"), Output.Friends[1].ZoneName, FString());
  }

  TestEqual(TEXT("One incoming request"), Output.IncomingRequests.Num(), 1);
  if (Output.IncomingRequests.Num() == 1) {
    TestEqual(TEXT("Incoming id"), Output.IncomingRequests[0].CharacterId, TEXT("char-in"));
    TestEqual(TEXT("Incoming name"), Output.IncomingRequests[0].CharacterName, TEXT("Charlie"));
    TestFalse(TEXT("A request row is not online"), Output.IncomingRequests[0].bOnline);
    TestEqual(TEXT("A request row has no zone"), Output.IncomingRequests[0].ZoneName, FString());
  }

  TestEqual(TEXT("One outgoing request"), Output.OutgoingRequests.Num(), 1);
  if (Output.OutgoingRequests.Num() == 1) {
    TestEqual(TEXT("Outgoing id"), Output.OutgoingRequests[0].CharacterId, TEXT("char-out"));
  }

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCharacterFriendsListBadAnswerTest,
  "Redwood.CharacterFriends.ListBadAnswer",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodCharacterFriendsListBadAnswerTest::RunTest(
  const FString &Parameters
) {
  // A refusal keeps its words and gives no rows, even when the arrays are
  // present.
  TSharedPtr<FJsonObject> Refused = MakeAnswer(TEXT("Invalid target character"));
  Refused->SetArrayField(
    TEXT("friends"),
    AsValues({MakeRow(TEXT("char-1"), TEXT("Alpha"), true, TEXT("z"))})
  );
  const FRedwoodListCharacterFriendsOutput FromRefused =
    URedwoodCommonGameSubsystem::ParseListCharacterFriends(Refused);
  TestEqual(
    TEXT("The backend words are not changed"),
    FromRefused.Error,
    TEXT("Invalid target character")
  );
  TestEqual(TEXT("A refusal gives no friends"), FromRefused.Friends.Num(), 0);

  // An empty object is what AsObject gives for an answer that is not an
  // object. It has no error field, so it is not one of the realm's answers.
  TestFalse(
    TEXT("An object with no error field is an error"),
    URedwoodCommonGameSubsystem::ParseListCharacterFriends(
      MakeShared<FJsonObject>()
    ).Error.IsEmpty()
  );

  TestFalse(
    TEXT("A null answer is an error"),
    URedwoodCommonGameSubsystem::ParseListCharacterFriends(nullptr)
      .Error.IsEmpty()
  );

  // A success with the arrays missing (an older backend) is an empty list,
  // not an error: the error field decides.
  TSharedPtr<FJsonObject> NoArrays = MakeShared<FJsonObject>();
  NoArrays->SetStringField(TEXT("error"), TEXT(""));
  const FRedwoodListCharacterFriendsOutput FromNoArrays =
    URedwoodCommonGameSubsystem::ParseListCharacterFriends(NoArrays);
  TestEqual(TEXT("Missing arrays give no error"), FromNoArrays.Error, FString());
  TestEqual(TEXT("Missing arrays give no rows"), FromNoArrays.Friends.Num(), 0);

  // A row that is not an object is skipped, not crashed on.
  TSharedPtr<FJsonObject> BadRow = MakeAnswer(FString());
  TArray<TSharedPtr<FJsonValue>> Rows;
  Rows.Add(MakeShared<FJsonValueString>(TEXT("nope")));
  Rows.Add(MakeShared<FJsonValueObject>(MakeRequestRow(TEXT("char-1"), TEXT("A"))));
  BadRow->SetArrayField(TEXT("friends"), Rows);
  TestEqual(
    TEXT("A row that is not an object is skipped"),
    URedwoodCommonGameSubsystem::ParseListCharacterFriends(BadRow).Friends.Num(),
    1
  );

  return true;
}
