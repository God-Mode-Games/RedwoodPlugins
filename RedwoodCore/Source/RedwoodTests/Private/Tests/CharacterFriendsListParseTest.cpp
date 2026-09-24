// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins the parse of the three arrays the fork adds to the "realm:contacts:list"
// answer (friends, incomingRequests, outgoingRequests), read by
// URedwoodClientInterface::ListCharacterFriends:
//   1. A good answer gives each row its id, name, online flag and zone.
//   2. A request row has no online flag and no zone; it keeps the defaults.
//   3. A refused answer carries its error unchanged and gives no rows.
//   4. An answer that is not one of the realm's answers is reported as an
//      error, not as an empty list: no answer, a null or non-object answer,
//      an object with no `error` field, or a success that does not carry
//      all three arrays. The game takes a good list as the full truth and
//      removes the settings of each friend that is not in it.
//   5. A row with no characterId or no characterName is left out, and so is
//      a row that is not an object or an object value that holds no object.
//      The game keys on the id and shows the name.
// The field names below must equal the names in the RedwoodBackend fork,
// packages/common/src/interfaces.ts (Realms.Contacts.List.SResponse).

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "RedwoodCommonGameSubsystem.h"
#include "Types/RedwoodTypesCharacters.h"

namespace {
  // A null Id or Name leaves that field out.
  TSharedPtr<FJsonObject> MakeFriendListRow(
    const TCHAR *Id, const TCHAR *Name, bool bOnline, const TCHAR *Zone
  ) {
    TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
    if (Id) {
      Row->SetStringField(TEXT("characterId"), Id);
    }
    if (Name) {
      Row->SetStringField(TEXT("characterName"), Name);
    }
    Row->SetBoolField(TEXT("online"), bOnline);
    Row->SetStringField(TEXT("zoneName"), Zone);
    return Row;
  }

  TSharedPtr<FJsonObject> MakeFriendListRequestRow(
    const TCHAR *Id, const TCHAR *Name
  ) {
    TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
    Row->SetStringField(TEXT("characterId"), Id);
    Row->SetStringField(TEXT("characterName"), Name);
    return Row;
  }

  TArray<TSharedPtr<FJsonValue>> AsFriendListValues(
    TArray<TSharedPtr<FJsonObject>> Rows
  ) {
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const TSharedPtr<FJsonObject> &Row : Rows) {
      Values.Add(MakeShared<FJsonValueObject>(Row));
    }
    return Values;
  }

  TSharedPtr<FJsonObject> MakeFriendListAnswer(const FString &Error) {
    TSharedPtr<FJsonObject> Answer = MakeShared<FJsonObject>();
    Answer->SetStringField(TEXT("error"), Error);
    Answer->SetArrayField(TEXT("contacts"), {});
    Answer->SetArrayField(TEXT("blockedContacts"), {});
    Answer->SetArrayField(TEXT("friends"), {});
    Answer->SetArrayField(TEXT("incomingRequests"), {});
    Answer->SetArrayField(TEXT("outgoingRequests"), {});
    return Answer;
  }

  // The socket gives the answer as element 0 of the argument array.
  FRedwoodListCharacterFriendsOutput ParseFriendListAnswer(
    const TSharedPtr<FJsonObject> &Answer
  ) {
    return URedwoodCommonGameSubsystem::ParseListCharacterFriends(
      {MakeShared<FJsonValueObject>(Answer)}
    );
  }
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCharacterFriendsListParseTest,
  "Redwood.CharacterFriends.ListParse",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodCharacterFriendsListParseTest::RunTest(const FString &Parameters) {
  TSharedPtr<FJsonObject> Answer = MakeFriendListAnswer(FString());
  TArray<TSharedPtr<FJsonValue>> Friends = AsFriendListValues(
    {MakeFriendListRow(TEXT("char-online"), TEXT("Alpha"), true, TEXT("L_Freewind")),
     MakeFriendListRow(TEXT("char-offline"), TEXT("Bravo"), false, TEXT("")),
     MakeFriendListRow(nullptr, TEXT("NoId"), true, TEXT("L_Freewind")),
     MakeFriendListRow(TEXT("char-noname"), nullptr, true, TEXT("L_Freewind"))}
  );
  Friends.Add(MakeShared<FJsonValueNull>());
  Friends.Add(nullptr);
  // In UE 5.8, TryGetObject says yes to an object value that holds no object.
  Friends.Add(MakeShared<FJsonValueObject>(TSharedPtr<FJsonObject>()));
  Answer->SetArrayField(TEXT("friends"), Friends);
  Answer->SetArrayField(
    TEXT("incomingRequests"),
    AsFriendListValues({MakeFriendListRequestRow(TEXT("char-in"), TEXT("Charlie"))})
  );
  Answer->SetArrayField(
    TEXT("outgoingRequests"),
    AsFriendListValues({MakeFriendListRequestRow(TEXT("char-out"), TEXT("Delta"))})
  );

  const FRedwoodListCharacterFriendsOutput Output = ParseFriendListAnswer(Answer);

  TestEqual(TEXT("A good answer carries no error"), Output.Error, FString());
  TestEqual(
    TEXT("Rows with no id, no name, or no object are left out"),
    Output.Friends.Num(),
    2
  );
  if (Output.Friends.Num() == 2) {
    TestEqual(TEXT("Friend id"), Output.Friends[0].CharacterId, TEXT("char-online"));
    TestEqual(TEXT("Friend name"), Output.Friends[0].CharacterName, TEXT("Alpha"));
    TestTrue(TEXT("Friend online"), Output.Friends[0].bOnline);
    TestEqual(TEXT("Friend zone"), Output.Friends[0].ZoneName, TEXT("L_Freewind"));
    TestEqual(TEXT("Second friend id"), Output.Friends[1].CharacterId, TEXT("char-offline"));
    TestEqual(TEXT("Second friend name"), Output.Friends[1].CharacterName, TEXT("Bravo"));
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
    TestEqual(TEXT("Outgoing name"), Output.OutgoingRequests[0].CharacterName, TEXT("Delta"));
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
  TSharedPtr<FJsonObject> Refused =
    MakeFriendListAnswer(TEXT("Invalid target character"));
  Refused->SetArrayField(
    TEXT("friends"),
    AsFriendListValues(
      {MakeFriendListRow(TEXT("char-1"), TEXT("Alpha"), true, TEXT("z"))}
    )
  );
  const FRedwoodListCharacterFriendsOutput FromRefused =
    ParseFriendListAnswer(Refused);
  TestEqual(
    TEXT("The backend words are not changed"),
    FromRefused.Error,
    TEXT("Invalid target character")
  );
  TestEqual(TEXT("A refusal gives no friends"), FromRefused.Friends.Num(), 0);

  // Answers that are not the realm's answers. Each must be an error, never
  // an empty list.
  TestFalse(
    TEXT("No answer is an error"),
    URedwoodCommonGameSubsystem::ParseListCharacterFriends({})
      .Error.IsEmpty()
  );
  TestFalse(
    TEXT("A null answer is an error"),
    URedwoodCommonGameSubsystem::ParseListCharacterFriends({nullptr})
      .Error.IsEmpty()
  );
  TestFalse(
    TEXT("A JSON null answer is an error"),
    URedwoodCommonGameSubsystem::ParseListCharacterFriends(
      {MakeShared<FJsonValueNull>()}
    ).Error.IsEmpty()
  );
  TestFalse(
    TEXT("An answer that is not an object is an error"),
    URedwoodCommonGameSubsystem::ParseListCharacterFriends(
      {MakeShared<FJsonValueString>(TEXT("nope"))}
    ).Error.IsEmpty()
  );
  TestFalse(
    TEXT("An object with no error field is an error"),
    ParseFriendListAnswer(MakeShared<FJsonObject>()).Error.IsEmpty()
  );

  // In UE 5.8, TryGetObject says yes to an object value that holds no
  // object. The answer reader must refuse it, because every caller reads
  // through the pointer it gives.
  TestNull(
    TEXT("An object value with no object is no answer"),
    URedwoodCommonGameSubsystem::TryGetRedwoodAnswerObject(
      {MakeShared<FJsonValueObject>(TSharedPtr<FJsonObject>())}
    )
  );

  // A success must carry all three arrays. Remove each one in turn from an
  // answer that has a row in every array: the result is an error with no
  // rows, so a partial answer cannot read as a short list.
  const TCHAR *ArrayNames[] = {
    TEXT("friends"), TEXT("incomingRequests"), TEXT("outgoingRequests")
  };
  for (const TCHAR *ArrayName : ArrayNames) {
    TSharedPtr<FJsonObject> Partial = MakeFriendListAnswer(FString());
    Partial->SetArrayField(
      TEXT("friends"),
      AsFriendListValues(
        {MakeFriendListRow(TEXT("char-1"), TEXT("Alpha"), true, TEXT("z"))}
      )
    );
    Partial->SetArrayField(
      TEXT("incomingRequests"),
      AsFriendListValues({MakeFriendListRequestRow(TEXT("char-2"), TEXT("B"))})
    );
    Partial->SetArrayField(
      TEXT("outgoingRequests"),
      AsFriendListValues({MakeFriendListRequestRow(TEXT("char-3"), TEXT("C"))})
    );
    Partial->RemoveField(ArrayName);

    const FRedwoodListCharacterFriendsOutput FromPartial =
      ParseFriendListAnswer(Partial);
    TestFalse(
      FString::Printf(TEXT("A success with no %s is an error"), ArrayName),
      FromPartial.Error.IsEmpty()
    );
    TestEqual(
      FString::Printf(TEXT("A success with no %s gives no rows"), ArrayName),
      FromPartial.Friends.Num() + FromPartial.IncomingRequests.Num() +
        FromPartial.OutgoingRequests.Num(),
      0
    );
  }

  TSharedPtr<FJsonObject> StringFriends = MakeFriendListAnswer(FString());
  StringFriends->SetStringField(TEXT("friends"), TEXT("nope"));
  TestFalse(
    TEXT("A success whose friends is a string is an error"),
    ParseFriendListAnswer(StringFriends).Error.IsEmpty()
  );

  TSharedPtr<FJsonObject> NullFriends = MakeFriendListAnswer(FString());
  NullFriends->SetField(TEXT("friends"), MakeShared<FJsonValueNull>());
  TestFalse(
    TEXT("A success whose friends is null is an error"),
    ParseFriendListAnswer(NullFriends).Error.IsEmpty()
  );

  // A row that is not an object is skipped, not crashed on.
  TSharedPtr<FJsonObject> BadRow = MakeFriendListAnswer(FString());
  TArray<TSharedPtr<FJsonValue>> Rows;
  Rows.Add(MakeShared<FJsonValueString>(TEXT("nope")));
  Rows.Add(MakeShared<FJsonValueObject>(
    MakeFriendListRequestRow(TEXT("char-1"), TEXT("A"))
  ));
  BadRow->SetArrayField(TEXT("friends"), Rows);
  TestEqual(
    TEXT("A row that is not an object is skipped"),
    ParseFriendListAnswer(BadRow).Friends.Num(),
    1
  );

  return true;
}
