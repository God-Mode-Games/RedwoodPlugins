// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): file is fork-added, for ReentryAdmissionTest.
// URedwoodReentryAuthListener records what the player state held at the moment
// OnRedwoodCharacterUpdated fired. The delegate is a Blueprint delegate, so only
// a UFUNCTION on a UObject can listen. The test must hold the listener in a
// TStrongObjectPtr: the dynamic delegate keeps only a weak pointer.

#pragma once

#include "CoreMinimal.h"

#include "RedwoodPlayerStateComponent.h"

#include "ReentryAdmissionListener.generated.h"

UCLASS()
class URedwoodReentryAuthListener : public UObject {
  GENERATED_BODY()

public:
  int32 BroadcastCount = 0;
  FString HeldIdAtBroadcast;

  void Watch(URedwoodPlayerStateComponent *InComponent) {
    Component = InComponent;
    InComponent->OnRedwoodCharacterUpdated.AddDynamic(
      this, &URedwoodReentryAuthListener::OnCharacterUpdated
    );
  }

  UFUNCTION()
  void OnCharacterUpdated() {
    ++BroadcastCount;
    if (Component.IsValid()) {
      HeldIdAtBroadcast = Component->ReentryHeldCharacterId;
    }
  }

private:
  TWeakObjectPtr<URedwoodPlayerStateComponent> Component;
};
