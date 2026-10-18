// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): file is fork-added, for CurrencyFlushTest. The owner of a test character
// component: it holds the records array that ResolveItemsRecordsArray reflects into, and counts
// the OnRedwoodCharacterUpdated broadcasts a reload raises. That delegate is a Blueprint one, so
// only a UFUNCTION can listen.

#pragma once

#include "CoreMinimal.h"

#include "GameFramework/Actor.h"
#include "Types/RedwoodTypesCharacters.h"

#include "CurrencyFlushTestActor.generated.h"

UCLASS()
class ARedwoodCurrencyFlushTestActor : public AActor {
  GENERATED_BODY()

public:
  // The default ItemsVariableName of URedwoodCharacterComponent.
  UPROPERTY()
  TArray<FRedwoodItemRecord> Items;

  int32 CharacterUpdatedCount = 0;

  UFUNCTION()
  void HandleCharacterUpdated() {
    ++CharacterUpdatedCount;
  }
};
