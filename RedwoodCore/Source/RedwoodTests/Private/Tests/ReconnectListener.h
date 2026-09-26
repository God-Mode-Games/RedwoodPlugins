// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): file is fork-added, for ReloginFailureTest. The
// reestablished delegates are Blueprint delegates, so only a UFUNCTION on a
// UObject can listen. The test must hold the listener in a TStrongObjectPtr.

#pragma once

#include "CoreMinimal.h"

#include "RedwoodClientInterface.h"

#include "ReconnectListener.generated.h"

UCLASS()
class URedwoodReconnectListener : public UObject {
  GENERATED_BODY()

public:
  int32 DirectorCount = 0;
  int32 RealmCount = 0;

  void Watch(URedwoodClientInterface *Client) {
    Client->OnDirectorConnectionReestablished.AddDynamic(
      this, &URedwoodReconnectListener::OnDirectorReestablished
    );
    Client->OnRealmConnectionReestablished.AddDynamic(
      this, &URedwoodReconnectListener::OnRealmReestablished
    );
  }

  UFUNCTION()
  void OnDirectorReestablished() {
    ++DirectorCount;
  }

  UFUNCTION()
  void OnRealmReestablished() {
    ++RealmCount;
  }
};
