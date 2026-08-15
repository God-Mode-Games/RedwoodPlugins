// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): file is fork-added, for ZoneTravelHardeningTest.
// URedwoodPlayerStateComponent::OnTransferAborted is a dynamic (Blueprint)
// delegate, so only a UFUNCTION on a UObject can listen to it. This holds
// the last pair the delegate carried.

#pragma once

#include "CoreMinimal.h"

#include "TransferAbortListener.generated.h"

UCLASS()
class URedwoodTransferAbortListener : public UObject {
  GENERATED_BODY()

public:
  int32 Count = 0;
  FString Error;
  FString Reason;

  UFUNCTION()
  void OnTransferAborted(FString InError, FString InReason) {
    ++Count;
    Error = InError;
    Reason = InReason;
  }
};
