// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): file is fork-added, for ZoneTravelHardeningTest.
// Records what a transfer abort delivered, on both sides at once:
// OnTransferAbortedServer (native, server) and OnTransferAborted (dynamic,
// owning client, reached through Client_OnTransferAborted). The client one
// is a Blueprint delegate, so only a UFUNCTION on a UObject can listen.

#pragma once

#include "CoreMinimal.h"

#include "RedwoodPlayerStateComponent.h"

#include "TransferAbortListener.generated.h"

UCLASS()
class URedwoodTransferAbortListener : public UObject {
  GENERATED_BODY()

public:
  int32 ServerCount = 0;
  FString ServerError;
  FString ServerReason;

  int32 ClientCount = 0;
  FString ClientError;
  FString ClientReason;

  // Bind both sides. The lambda below takes a raw "this", and the dynamic
  // delegate holds only a weak object pointer, so neither keeps this
  // listener alive: the test must hold it in a TStrongObjectPtr.
  void Watch(URedwoodPlayerStateComponent *Component) {
    Component->OnTransferAbortedServer.AddLambda(
      [this](const FString &InError, const FString &InReason) {
        ++ServerCount;
        ServerError = InError;
        ServerReason = InReason;
      }
    );
    Component->OnTransferAborted.AddDynamic(
      this, &URedwoodTransferAbortListener::OnTransferAborted
    );
  }

  UFUNCTION()
  void OnTransferAborted(FString InError, FString InReason) {
    ++ClientCount;
    ClientError = InError;
    ClientReason = InReason;
  }
};
