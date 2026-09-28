// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// A listener for the character friend event of URedwoodClientGameSubsystem.
// The event is a dynamic delegate, so a listener must be a UFUNCTION on a
// UObject.

#pragma once

#include "CoreMinimal.h"
#include "Types/RedwoodTypesCharacters.h"

#include "RedwoodFriendRelayProbe.generated.h"

UCLASS()
class URedwoodFriendRelayProbe : public UObject {
  GENERATED_BODY()

public:
  int32 CharacterFriendAlertCount = 0;
  FRedwoodCharacterFriendAlert LastCharacterFriendAlert;

  UFUNCTION()
  void HandleCharacterFriendAlert(const FRedwoodCharacterFriendAlert &Alert) {
    ++CharacterFriendAlertCount;
    LastCharacterFriendAlert = Alert;
  }
};
