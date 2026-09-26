// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins the payload that restores the online character of a player after a
// director re-login (HollowedOath#2854). The re-login writes an online state
// with no realm, so friends saw the player with no character until the next
// character selection.
//   1. The wire name matches the director-frontend route.
//   2. A full payload carries the player, the character and the realm.
//   3. A payload with no character or no realm is not sent at all: the
//      director would reject it, and nothing is selected yet.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "RedwoodClientInterface.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodOnlineCharacterPayloadTest,
  "Redwood.OnlineCharacter.Payload",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodOnlineCharacterPayloadTest::RunTest(const FString &Parameters) {
  TestEqual(
    TEXT("The wire name has not drifted"),
    FString(URedwoodClientInterface::SetOnlineCharacterEventName),
    TEXT("director:players:online-state:set-character")
  );

  const TSharedPtr<FJsonObject> Full =
    URedwoodClientInterface::MakeOnlineCharacterPayload(
      TEXT("player-1"), TEXT("character-1"), TEXT("realm-1")
    );
  if (!TestTrue(TEXT("A full payload is made"), Full.IsValid())) {
    return false;
  }
  TestEqual(TEXT("player"), Full->GetStringField(TEXT("playerId")), TEXT("player-1"));
  TestEqual(TEXT("character"), Full->GetStringField(TEXT("characterId")), TEXT("character-1"));
  TestEqual(TEXT("realm"), Full->GetStringField(TEXT("realmId")), TEXT("realm-1"));

  TestFalse(
    TEXT("No character selected: nothing to restore"),
    URedwoodClientInterface::MakeOnlineCharacterPayload(
      TEXT("player-1"), FString(), TEXT("realm-1")
    ).IsValid()
  );
  TestFalse(
    TEXT("No realm yet: nothing to restore"),
    URedwoodClientInterface::MakeOnlineCharacterPayload(
      TEXT("player-1"), TEXT("character-1"), FString()
    ).IsValid()
  );

  return true;
}
