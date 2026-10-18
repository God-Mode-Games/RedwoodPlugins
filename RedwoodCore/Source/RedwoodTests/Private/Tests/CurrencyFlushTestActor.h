// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): file is fork-added, for CurrencySidecarTest. The owner of a test character
// component: it holds the records array that ResolveItemsRecordsArray reflects into, and puts the
// component in a world, so the component can reach the server subsystem's per-character freeze.

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
};
