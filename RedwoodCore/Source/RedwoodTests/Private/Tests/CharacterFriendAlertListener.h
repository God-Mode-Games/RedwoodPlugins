// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): file is fork-added, for InFlightReplyTest. The alert
// is a Blueprint delegate, so only a UFUNCTION on a UObject can listen. The
// test must hold the listener in a TStrongObjectPtr.

#pragma once

#include "CoreMinimal.h"

#include "RedwoodClientInterface.h"
#include "Types/RedwoodTypesCharacters.h"

#include "CharacterFriendAlertListener.generated.h"

UCLASS()
class URedwoodCharacterFriendAlertListener : public UObject {
  GENERATED_BODY()

public:
  int32 Count = 0;
  FRedwoodCharacterFriendAlert Last;

  void Watch(URedwoodClientInterface *Client) {
    Client->OnCharacterFriendAlert.AddDynamic(
      this, &URedwoodCharacterFriendAlertListener::OnAlert
    );
  }

  UFUNCTION()
  void OnAlert(const FRedwoodCharacterFriendAlert &Alert) {
    ++Count;
    Last = Alert;
  }
};
