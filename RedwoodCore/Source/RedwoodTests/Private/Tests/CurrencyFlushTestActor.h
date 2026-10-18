// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): file is fork-added, for CurrencySidecarTest. The owners of a test character
// component: they hold the records array that ResolveItemsRecordsArray reflects into, and put the
// component in a world, so the component can reach the server subsystem's per-character freeze.

#pragma once

#include "CoreMinimal.h"

#include "GameFramework/Actor.h"
#include "GameFramework/PlayerState.h"
#include "Types/RedwoodTypesCharacters.h"

#include "CurrencyFlushTestActor.generated.h"

UCLASS()
class ARedwoodCurrencyFlushTestActor : public AActor {
  GENERATED_BODY()

public:
  // The default ItemsVariableName of URedwoodCharacterComponent.
  UPROPERTY()
  TArray<FRedwoodItemRecord> Items;
};

// A PlayerState owner, for a component that hydrates from the PlayerState's Redwood snapshot the
// way a live one does (RedwoodPlayerStateCharacterUpdated reads the snapshot off its owner).
UCLASS()
class ARedwoodCurrencyLoadTestPlayerState : public APlayerState {
  GENERATED_BODY()

public:
  // The default ItemsVariableName of URedwoodCharacterComponent.
  UPROPERTY()
  TArray<FRedwoodItemRecord> Items;
};
