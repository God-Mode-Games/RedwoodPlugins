// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins the field names and the type words this plugin reads out of the
// fork-added "director:friends:character-alert" push, so a change to the
// parser cannot stop reading them without a test failure:
//   1. Each of the five type words gives its enum value.
//   2. The two ids, the name and the zone are carried.
//   3. An unknown type word, or a missing id, is refused; the listener in
//      RedwoodClientInterface.cpp then drops the push and logs a warning.
//   4. The name and the zone are optional.
//   5. A reused output keeps nothing from an earlier push.
// The names must equal the names in the RedwoodBackend fork,
// packages/common/src/interfaces.ts (the CharacterAlert schema).

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "RedwoodCommonGameSubsystem.h"
#include "Types/RedwoodTypesCharacters.h"

namespace {
  TSharedPtr<FJsonObject> MakeCharacterAlertObj(const TCHAR *Type) {
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("type"), Type);
    Obj->SetStringField(TEXT("characterId"), TEXT("me-1"));
    Obj->SetStringField(TEXT("otherCharacterId"), TEXT("other-1"));
    Obj->SetStringField(TEXT("otherCharacterName"), TEXT("Other"));
    Obj->SetStringField(TEXT("zoneName"), TEXT("L_Freewind"));
    return Obj;
  }
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCharacterFriendAlertParseTest,
  "Redwood.CharacterFriends.AlertParse",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodCharacterFriendAlertParseTest::RunTest(const FString &Parameters) {
  FRedwoodCharacterFriendAlert Alert;
  TestTrue(
    TEXT("An online alert parses"),
    URedwoodCommonGameSubsystem::ParseCharacterFriendAlert(
      MakeCharacterAlertObj(TEXT("online")), Alert
    )
  );
  TestTrue(
    TEXT("The type is Online"),
    Alert.Type == ERedwoodCharacterFriendAlertType::Online
  );
  TestEqual(TEXT("The recipient id"), Alert.CharacterId, TEXT("me-1"));
  TestEqual(TEXT("The other id"), Alert.OtherCharacterId, TEXT("other-1"));
  TestEqual(TEXT("The other name"), Alert.OtherCharacterName, TEXT("Other"));
  TestEqual(TEXT("The zone"), Alert.ZoneName, TEXT("L_Freewind"));

  const TCHAR *Words[] = {
    TEXT("requested"), TEXT("accepted"), TEXT("removed"), TEXT("offline")
  };
  const ERedwoodCharacterFriendAlertType Values[] = {
    ERedwoodCharacterFriendAlertType::Requested,
    ERedwoodCharacterFriendAlertType::Accepted,
    ERedwoodCharacterFriendAlertType::Removed,
    ERedwoodCharacterFriendAlertType::Offline
  };
  // One entry per word; the two arrays must stay the same length.
  constexpr int32 WordCount = UE_ARRAY_COUNT(Words);
  static_assert(UE_ARRAY_COUNT(Values) == WordCount, "One value per word");
  for (int32 Index = 0; Index < WordCount; ++Index) {
    FRedwoodCharacterFriendAlert Each;
    TestTrue(
      Words[Index],
      URedwoodCommonGameSubsystem::ParseCharacterFriendAlert(
        MakeCharacterAlertObj(Words[Index]), Each
      )
    );
    TestTrue(Words[Index], Each.Type == Values[Index]);
  }

  // The backend always sends a name and a zone (the zone is empty unless
  // online), but the game can act without them, so the parser does not
  // require them. The output is reused from the online parse above, so the
  // test also shows that no name or zone stays from an earlier push.
  TSharedPtr<FJsonObject> Bare = MakeCharacterAlertObj(TEXT("requested"));
  Bare->RemoveField(TEXT("otherCharacterName"));
  Bare->RemoveField(TEXT("zoneName"));
  TestTrue(
    TEXT("No name and no zone still parses"),
    URedwoodCommonGameSubsystem::ParseCharacterFriendAlert(Bare, Alert)
  );
  TestEqual(
    TEXT("An absent name is empty"), Alert.OtherCharacterName, FString()
  );
  TestEqual(TEXT("An absent zone is empty"), Alert.ZoneName, FString());

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCharacterFriendAlertBadPayloadTest,
  "Redwood.CharacterFriends.AlertBadPayload",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodCharacterFriendAlertBadPayloadTest::RunTest(
  const FString &Parameters
) {
  // Start from a filled output, so that a refusal below cannot pass on ids
  // that an earlier push left in it.
  FRedwoodCharacterFriendAlert Alert;
  TestTrue(
    TEXT("A good alert fills the output first"),
    URedwoodCommonGameSubsystem::ParseCharacterFriendAlert(
      MakeCharacterAlertObj(TEXT("online")), Alert
    )
  );

  TestFalse(
    TEXT("An unknown type is refused"),
    URedwoodCommonGameSubsystem::ParseCharacterFriendAlert(
      MakeCharacterAlertObj(TEXT("blocked")), Alert
    )
  );

  TSharedPtr<FJsonObject> NoType = MakeCharacterAlertObj(TEXT("online"));
  NoType->RemoveField(TEXT("type"));
  TestFalse(
    TEXT("A missing type is refused"),
    URedwoodCommonGameSubsystem::ParseCharacterFriendAlert(NoType, Alert)
  );

  TSharedPtr<FJsonObject> NoRecipient = MakeCharacterAlertObj(TEXT("online"));
  NoRecipient->RemoveField(TEXT("characterId"));
  TestFalse(
    TEXT("A missing recipient id is refused"),
    URedwoodCommonGameSubsystem::ParseCharacterFriendAlert(NoRecipient, Alert)
  );

  TSharedPtr<FJsonObject> NoOther = MakeCharacterAlertObj(TEXT("online"));
  NoOther->RemoveField(TEXT("otherCharacterId"));
  TestFalse(
    TEXT("A missing other id is refused"),
    URedwoodCommonGameSubsystem::ParseCharacterFriendAlert(NoOther, Alert)
  );

  TSharedPtr<FJsonObject> EmptyOther = MakeCharacterAlertObj(TEXT("online"));
  EmptyOther->SetStringField(TEXT("otherCharacterId"), TEXT(""));
  TestFalse(
    TEXT("An empty other id is refused"),
    URedwoodCommonGameSubsystem::ParseCharacterFriendAlert(EmptyOther, Alert)
  );

  // AsObject gives an empty object for a push that is not an object; a direct
  // caller can still pass an empty pointer.
  TestFalse(
    TEXT("An empty push is refused"),
    URedwoodCommonGameSubsystem::ParseCharacterFriendAlert(
      MakeShared<FJsonObject>(), Alert
    )
  );
  TestFalse(
    TEXT("A null push is refused"),
    URedwoodCommonGameSubsystem::ParseCharacterFriendAlert(nullptr, Alert)
  );

  return true;
}
