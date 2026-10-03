// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins the field names and type words read from the character friend push.
// They must equal the CharacterAlert schema of the RedwoodBackend fork
// (packages/common/src/interfaces.ts).

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
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

  // The listener gets the push as one event value.
  TSharedPtr<FJsonValue> AsCharacterAlertValue(
    const TSharedPtr<FJsonObject> &Obj
  ) {
    return MakeShared<FJsonValueObject>(Obj);
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
      AsCharacterAlertValue(MakeCharacterAlertObj(TEXT("online"))), Alert
    )
  );
  TestTrue(
    TEXT("The type is Online"),
    Alert.Type == ERedwoodCharacterFriendAlertType::Online
  );
  // TestEqualSensitive: the string forms of TestEqual ignore case.
  TestEqualSensitive(TEXT("The recipient id"), *Alert.CharacterId, TEXT("me-1"));
  TestEqualSensitive(TEXT("The other id"), *Alert.OtherCharacterId, TEXT("other-1"));
  TestEqualSensitive(TEXT("The other name"), *Alert.OtherCharacterName, TEXT("Other"));
  TestEqualSensitive(TEXT("The zone"), *Alert.ZoneName, TEXT("L_Freewind"));

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
        AsCharacterAlertValue(MakeCharacterAlertObj(Words[Index])), Each
      )
    );
    TestTrue(Words[Index], Each.Type == Values[Index]);
  }

  // The name and the zone are optional. The output is reused from the online
  // parse above, so this also shows no field stays from an earlier push.
  TSharedPtr<FJsonObject> Bare = MakeCharacterAlertObj(TEXT("requested"));
  Bare->RemoveField(TEXT("otherCharacterName"));
  Bare->RemoveField(TEXT("zoneName"));
  TestTrue(
    TEXT("No name and no zone still parses"),
    URedwoodCommonGameSubsystem::ParseCharacterFriendAlert(
      AsCharacterAlertValue(Bare), Alert
    )
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
      AsCharacterAlertValue(MakeCharacterAlertObj(TEXT("online"))), Alert
    )
  );

  TestFalse(
    TEXT("An unknown type is refused"),
    URedwoodCommonGameSubsystem::ParseCharacterFriendAlert(
      AsCharacterAlertValue(MakeCharacterAlertObj(TEXT("blocked"))), Alert
    )
  );

  TSharedPtr<FJsonObject> NoType = MakeCharacterAlertObj(TEXT("online"));
  NoType->RemoveField(TEXT("type"));
  TestFalse(
    TEXT("A missing type is refused"),
    URedwoodCommonGameSubsystem::ParseCharacterFriendAlert(
      AsCharacterAlertValue(NoType), Alert
    )
  );

  TSharedPtr<FJsonObject> NoRecipient = MakeCharacterAlertObj(TEXT("online"));
  NoRecipient->RemoveField(TEXT("characterId"));
  TestFalse(
    TEXT("A missing recipient id is refused"),
    URedwoodCommonGameSubsystem::ParseCharacterFriendAlert(
      AsCharacterAlertValue(NoRecipient), Alert
    )
  );

  TSharedPtr<FJsonObject> NoOther = MakeCharacterAlertObj(TEXT("online"));
  NoOther->RemoveField(TEXT("otherCharacterId"));
  TestFalse(
    TEXT("A missing other id is refused"),
    URedwoodCommonGameSubsystem::ParseCharacterFriendAlert(
      AsCharacterAlertValue(NoOther), Alert
    )
  );

  TSharedPtr<FJsonObject> EmptyOther = MakeCharacterAlertObj(TEXT("online"));
  EmptyOther->SetStringField(TEXT("otherCharacterId"), TEXT(""));
  TestFalse(
    TEXT("An empty other id is refused"),
    URedwoodCommonGameSubsystem::ParseCharacterFriendAlert(
      AsCharacterAlertValue(EmptyOther), Alert
    )
  );

  // AsObject would turn each of these into a valid empty object.
  TestFalse(
    TEXT("A string push is refused"),
    URedwoodCommonGameSubsystem::ParseCharacterFriendAlert(
      MakeShared<FJsonValueString>(TEXT("online")), Alert
    )
  );
  TestFalse(
    TEXT("A JSON null push is refused"),
    URedwoodCommonGameSubsystem::ParseCharacterFriendAlert(
      MakeShared<FJsonValueNull>(), Alert
    )
  );
  TestFalse(
    TEXT("An object value with no object is refused"),
    URedwoodCommonGameSubsystem::ParseCharacterFriendAlert(
      MakeShared<FJsonValueObject>(TSharedPtr<FJsonObject>()), Alert
    )
  );
  TestFalse(
    TEXT("A null value is refused"),
    URedwoodCommonGameSubsystem::ParseCharacterFriendAlert(nullptr, Alert)
  );

  return true;
}
