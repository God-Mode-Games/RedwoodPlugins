// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): file is fork-added, for the connection tests. The
// connection delegates are Blueprint delegates, so only a UFUNCTION on a
// UObject can listen. The test must hold the listener in a TStrongObjectPtr.

#pragma once

#include "CoreMinimal.h"

#include "RedwoodClientInterface.h"

#include "ReconnectListener.generated.h"

UCLASS()
class URedwoodReconnectListener : public UObject {
  GENERATED_BODY()

public:
  int32 LostCount = 0;
  int32 ReestablishedCount = 0;
  int32 AuthFailedCount = 0;
  FString LastAuthFailure;

  void Watch(URedwoodClientInterface *Client) {
    Client->OnConnectionLost.AddDynamic(
      this, &URedwoodReconnectListener::OnLost
    );
    Client->OnConnectionReestablished.AddDynamic(
      this, &URedwoodReconnectListener::OnReestablished
    );
    Client->OnAuthFailed.AddDynamic(
      this, &URedwoodReconnectListener::OnAuthFailed
    );
  }

  UFUNCTION()
  void OnLost() {
    ++LostCount;
  }

  UFUNCTION()
  void OnReestablished() {
    ++ReestablishedCount;
  }

  UFUNCTION()
  void OnAuthFailed(FString Message) {
    ++AuthFailedCount;
    LastAuthFailure = Message;
  }
};
