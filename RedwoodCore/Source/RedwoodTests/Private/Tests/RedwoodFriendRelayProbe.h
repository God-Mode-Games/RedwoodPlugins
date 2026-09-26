// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// A listener for the friend events of URedwoodClientGameSubsystem, used by
// Redwood.ClientSubsystem.FriendRelay. The events are dynamic delegates, so a
// listener must be a UFUNCTION on a UObject.

#pragma once

#include "CoreMinimal.h"
#include "Types/RedwoodTypesCharacters.h"
#include "Types/RedwoodTypesPlayers.h"

#include "RedwoodFriendRelayProbe.generated.h"

UCLASS()
class URedwoodFriendRelayProbe : public UObject {
  GENERATED_BODY()

public:
  int32 CharacterFriendAlertCount = 0;
  FRedwoodCharacterFriendAlert LastCharacterFriendAlert;

  int32 FriendRequestCount = 0;
  FString LastRequesterId;

  UFUNCTION()
  void HandleCharacterFriendAlert(const FRedwoodCharacterFriendAlert &Alert) {
    ++CharacterFriendAlertCount;
    LastCharacterFriendAlert = Alert;
  }

  UFUNCTION()
  void HandleFriendRequestReceived(FRedwoodPlayer Requester) {
    ++FriendRequestCount;
    LastRequesterId = Requester.PlayerId;
  }
};
