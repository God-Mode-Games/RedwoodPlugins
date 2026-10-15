// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): file is fork-added, for the client interface tests.
// OnRequestToJoinServer is a Blueprint delegate, so only a UFUNCTION on a
// UObject can listen. The test must hold the listener in a TStrongObjectPtr.

#pragma once

#include "CoreMinimal.h"

#include "RedwoodClientInterface.h"

#include "JoinServerListener.generated.h"

UCLASS()
class URedwoodJoinServerListener : public UObject {
  GENERATED_BODY()

public:
  int32 Count = 0;
  FString LastConsoleCommand;

  void Watch(URedwoodClientInterface *Client) {
    Client->OnRequestToJoinServer.AddDynamic(
      this, &URedwoodJoinServerListener::OnRequestToJoinServer
    );
  }

  UFUNCTION()
  void OnRequestToJoinServer(FString ConsoleCommand) {
    ++Count;
    LastConsoleCommand = ConsoleCommand;
  }
};
