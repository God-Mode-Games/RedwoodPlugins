// Copyright Incanta Games. All Rights Reserved.

#include "RedwoodCharacterComponent.h"
#include "RedwoodCommonGameSubsystem.h"
#include "RedwoodModule.h"
#include "RedwoodPlayerStateComponent.h"
#include "RedwoodServerGameSubsystem.h" // FORK(hollowed-oath): the per-character freeze

#include "Engine/GameInstance.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameSession.h"
#include "GameFramework/PlayerController.h" // FORK(hollowed-oath): the desync kick
#include "GameFramework/PlayerState.h"
#include "Kismet/GameplayStatics.h"
// FORK(hollowed-oath): NetCore PushModel include for the push-model rep conversion + the
// MARK_PROPERTY_DIRTY_FROM_NAME calls in the *Updated() handlers below (upstream trunk has
// neither). Engine/GameInstance.h above is likewise fork-added for the same body of work.
#include "Net/Core/PushModel/PushModel.h"
#include "Net/UnrealNetwork.h"
#include "SIOJConvert.h"
#include "SIOJsonObject.h"

URedwoodCharacterComponent::URedwoodCharacterComponent(
  const FObjectInitializer &ObjectInitializer
) :
  Super(ObjectInitializer) {
  SetIsReplicatedByDefault(true);
}

void URedwoodCharacterComponent::GetLifetimeReplicatedProps(
  TArray<FLifetimeProperty> &OutLifetimeProps
) const {
  Super::GetLifetimeReplicatedProps(OutLifetimeProps);

  // FORK(hollowed-oath) BEGIN: push-model replication for every URedwoodCharacterComponent
  // replicated property. Upstream trunk registers these with plain DOREPLIFETIME /
  // DOREPLIFETIME_CONDITION (the else-branch below). The fork converts them to push-model, which
  // makes each property inert until dirty-marked -- so the RedwoodPlayerStatePlayerUpdated() /
  // RedwoodPlayerStateCharacterUpdated() handlers below MUST MARK_PROPERTY_DIRTY_FROM_NAME every
  // field they mutate (they now do). An upstream merge must preserve: (a) BOTH legs here, (b) the
  // exact OwnerOnly-vs-default condition split, and (c) the paired dirty-marks in those handlers.
#if WITH_PUSH_MODEL
  if (IS_PUSH_MODEL_ENABLED()) {
    FDoRepLifetimeParams Params;
    Params.bIsPushBased = true;

    DOREPLIFETIME_WITH_PARAMS_FAST(
      URedwoodCharacterComponent, RedwoodPlayerId, Params
    );
    DOREPLIFETIME_WITH_PARAMS_FAST(
      URedwoodCharacterComponent, RedwoodPlayerNickname, Params
    );
    DOREPLIFETIME_WITH_PARAMS_FAST(
      URedwoodCharacterComponent, RedwoodNameTag, Params
    );

    FDoRepLifetimeParams OwnerOnlyParams;
    OwnerOnlyParams.bIsPushBased = true;
    OwnerOnlyParams.Condition = COND_OwnerOnly;
    DOREPLIFETIME_WITH_PARAMS_FAST(
      URedwoodCharacterComponent, bSelectedGuildValid, OwnerOnlyParams
    );
    DOREPLIFETIME_WITH_PARAMS_FAST(
      URedwoodCharacterComponent, SelectedGuild, OwnerOnlyParams
    );

    DOREPLIFETIME_WITH_PARAMS_FAST(
      URedwoodCharacterComponent, RedwoodCharacterId, Params
    );
    DOREPLIFETIME_WITH_PARAMS_FAST(
      URedwoodCharacterComponent, RedwoodCharacterName, Params
    );
    DOREPLIFETIME_WITH_PARAMS_FAST(
      URedwoodCharacterComponent, RedwoodPlayerUpdateCount, Params
    );
    DOREPLIFETIME_WITH_PARAMS_FAST(
      URedwoodCharacterComponent, RedwoodCharacterUpdateCount, Params
    );
  } else
#endif
  {
    DOREPLIFETIME(URedwoodCharacterComponent, RedwoodPlayerId);
    DOREPLIFETIME(URedwoodCharacterComponent, RedwoodPlayerNickname);
    DOREPLIFETIME(URedwoodCharacterComponent, RedwoodNameTag);
    DOREPLIFETIME_CONDITION(
      URedwoodCharacterComponent, bSelectedGuildValid, COND_OwnerOnly
    );
    DOREPLIFETIME_CONDITION(
      URedwoodCharacterComponent, SelectedGuild, COND_OwnerOnly
    );
    DOREPLIFETIME(URedwoodCharacterComponent, RedwoodCharacterId);
    DOREPLIFETIME(URedwoodCharacterComponent, RedwoodCharacterName);
    DOREPLIFETIME(URedwoodCharacterComponent, RedwoodPlayerUpdateCount);
    DOREPLIFETIME(URedwoodCharacterComponent, RedwoodCharacterUpdateCount);
  }
  // FORK(hollowed-oath) END
}

void URedwoodCharacterComponent::BeginPlay() {
  Super::BeginPlay();

  APawn *Pawn = Cast<APawn>(GetOwner());

  if (Pawn) {
    Pawn->ReceiveControllerChangedDelegate.AddUniqueDynamic(
      this, &URedwoodCharacterComponent::OnControllerChanged
    );
    AController *Controller = Pawn->GetController();
    if (IsValid(Controller) && IsValid(Controller->PlayerState)) {
      OnControllerChanged(Pawn, nullptr, Controller);
    }
  } else {
    APlayerState *PlayerState = Cast<APlayerState>(GetOwner());

    if (IsValid(PlayerState)) {
      URedwoodPlayerStateComponent *PlayerStateComponent =
        PlayerState->FindComponentByClass<URedwoodPlayerStateComponent>();

      if (IsValid(PlayerStateComponent)) {
        PlayerStateComponent->OnRedwoodPlayerUpdated.AddUniqueDynamic(
          this, &URedwoodCharacterComponent::RedwoodPlayerStatePlayerUpdated
        );
        RedwoodPlayerStatePlayerUpdated();

        PlayerStateComponent->OnRedwoodCharacterUpdated.AddUniqueDynamic(
          this, &URedwoodCharacterComponent::RedwoodPlayerStateCharacterUpdated
        );
        RedwoodPlayerStateCharacterUpdated();
      } else {
        UE_LOG(
          LogRedwood,
          Error,
          TEXT(
            "URedwoodCharacterComponent requires the owning APlayerState to have an attached URedwoodPlayerStateComponent"
          )
        );
      }
    } else {
      UE_LOG(
        LogRedwood,
        Error,
        TEXT(
          "URedwoodCharacterComponent must be used with APawn or APlayerState"
        )
      );
    }
  }
}

// FORK(hollowed-oath): R26. Before Super, so the queues are logged or handed off while the
// component still knows its character.
void URedwoodCharacterComponent::EndPlay(const EEndPlayReason::Type EndPlayReason) {
  if (GetOwnerRole() == ROLE_Authority) {
    HandOffCurrencyAtEndPlay(EndPlayReason);
  }
  Super::EndPlay(EndPlayReason);
}

void URedwoodCharacterComponent::OnControllerChanged(
  APawn *Pawn, AController *OldController, AController *NewController
) {
  if (IsValid(NewController)) {
    if (IsValid(NewController->PlayerState)) {
      URedwoodPlayerStateComponent *PlayerStateComponent =
        NewController->PlayerState
          ->FindComponentByClass<URedwoodPlayerStateComponent>();
      if (IsValid(PlayerStateComponent)) {
        PlayerStateComponent->OnRedwoodPlayerUpdated.AddUniqueDynamic(
          this, &URedwoodCharacterComponent::RedwoodPlayerStatePlayerUpdated
        );
        RedwoodPlayerStatePlayerUpdated();
      
        PlayerStateComponent->OnRedwoodCharacterUpdated.AddUniqueDynamic(
          this, &URedwoodCharacterComponent::RedwoodPlayerStateCharacterUpdated
        );
        RedwoodPlayerStateCharacterUpdated();
      }
    }
  }
}

void URedwoodCharacterComponent::RedwoodPlayerStatePlayerUpdated() {
  // The source PlayerStateComponent->RedwoodPlayer struct is server-only and is
  // empty on clients. This function copies it into the replicated fields below,
  // so it must only run on the authority; clients receive the values via
  // replication and learn about the update via OnRep_RedwoodPlayerUpdated.
  // Running it on a client would overwrite the replicated fields with blanks.
  if (GetOwnerRole() != ROLE_Authority) {
    return;
  }

  APawn *Pawn = Cast<APawn>(GetOwner());
  AController *Controller = IsValid(Pawn) ? Pawn->GetController() : nullptr;
  APlayerState *PlayerState = IsValid(Controller)
    ? Cast<APlayerState>(Controller->PlayerState)
    : Cast<APlayerState>(GetOwner());

  URedwoodPlayerStateComponent *PlayerStateComponent = IsValid(PlayerState)
    ? PlayerState->FindComponentByClass<URedwoodPlayerStateComponent>()
    : nullptr;

  if (IsValid(PlayerStateComponent)) {
    FRedwoodPlayerData PlayerData = PlayerStateComponent->RedwoodPlayer;

    RedwoodPlayerNickname = PlayerData.Nickname;
    RedwoodNameTag = PlayerData.bSelectedGuildValid
      ? PlayerData.SelectedGuild.Guild.Tag
      : FString();

    bSelectedGuildValid = PlayerData.bSelectedGuildValid;
    SelectedGuild = PlayerData.SelectedGuild;

    // FORK(hollowed-oath) BEGIN: dirty-marks required by the push-model conversion of these
    // properties (see GetLifetimeReplicatedProps above). Upstream mutates them with no dirty-mark
    // because upstream rep is legacy always-poll. Merge must keep one mark per mutated field.
    MARK_PROPERTY_DIRTY_FROM_NAME(
      URedwoodCharacterComponent, RedwoodPlayerNickname, this
    );
    MARK_PROPERTY_DIRTY_FROM_NAME(
      URedwoodCharacterComponent, RedwoodNameTag, this
    );
    MARK_PROPERTY_DIRTY_FROM_NAME(
      URedwoodCharacterComponent, bSelectedGuildValid, this
    );
    MARK_PROPERTY_DIRTY_FROM_NAME(
      URedwoodCharacterComponent, SelectedGuild, this
    );
    // FORK(hollowed-oath) END

    if (bUsePlayerData) {
      bool bErrored = false;
      bool bDirty = URedwoodCommonGameSubsystem::DeserializeBackendData(
        bStoreDataInActor ? (UObject *)Pawn : (UObject *)this,
        PlayerData.Data,
        *PlayerDataVariableName,
        LatestPlayerDataSchemaVersion,
        bErrored
      );

      if (bErrored) {
        // kick the player as they're not compatible with the server
        APlayerController *PlayerController =
          PlayerState->GetPlayerController();

        if (IsValid(PlayerController)) {
          AGameModeBase *GameMode = UGameplayStatics::GetGameMode(PlayerState);
          if (IsValid(GameMode) && GameMode->GameSession) {
            GameMode->GameSession->KickPlayer(
              PlayerController,
              FText::FromString(TEXT("Could not load player character data"))
            );
          }
        }
      }

      if (bDirty) {
        MarkPlayerDataDirty();
      }
    }

    FString FormatPlayerNameFunctionName = TEXT("FormatPlayerName");
    UFunction *FormatPlayerNameFunction =
      GetOwner()->GetClass()->FindFunctionByName(*FormatPlayerNameFunctionName);
    FString *CustomPlayerName = nullptr;

    if (FormatPlayerNameFunction) {
      // Ensure the function is valid and has the correct signature
      if (
        !FormatPlayerNameFunction->IsValidLowLevel() ||
        FormatPlayerNameFunction->NumParms != 1 ||
        !FormatPlayerNameFunction->ReturnValueOffset
      ) {
        UE_LOG(
          LogRedwood,
          Error,
          TEXT(
            "Function %s in %s has an invalid signature, using default player name."
          ),
          *FormatPlayerNameFunctionName,
          *GetOwner()->GetName()
        );
      } else {
        // Allocate memory for the parameters
        void *Params = FMemory::Malloc(FormatPlayerNameFunction->ParmsSize);
        FMemory::Memzero(Params, FormatPlayerNameFunction->ParmsSize);

        FProperty *FunctionStructProp = FormatPlayerNameFunction->PropertyLink;
        FProperty *FunctionObjectProp = FunctionStructProp->PropertyLinkNext;

        // Call the function
        GetOwner()->ProcessEvent(FormatPlayerNameFunction, Params);

        // Retrieve the return value
        void *ReturnValue =
          (void
             *)((SIZE_T)Params + FormatPlayerNameFunction->ReturnValueOffset);

        // Copy the return value to CustomPlayerName
        CustomPlayerName = (FString *)ReturnValue;

        // Clean up
        FMemory::Free(Params);
      }
    }

    FString DefaultPlayerName = RedwoodNameTag.IsEmpty()
      ? PlayerData.Nickname
      : FString::Printf(TEXT("[%s] %s"), *RedwoodNameTag, *PlayerData.Nickname);

    PlayerState->SetPlayerName(
      CustomPlayerName == nullptr ? DefaultPlayerName : *CustomPlayerName
    );

    // Broadcast locally on the server, then bump the replicated counter so the
    // notify fires on clients once the updated fields have been applied.
    OnRedwoodPlayerUpdated.Broadcast();
    RedwoodPlayerUpdateCount++;
    // FORK(hollowed-oath): dirty-mark for the push-model counter that drives the client-side
    // OnRep notify; without it the incremented count never replicates. See GetLifetimeReplicatedProps.
    MARK_PROPERTY_DIRTY_FROM_NAME(
      URedwoodCharacterComponent, RedwoodPlayerUpdateCount, this
    );
  }
}

void URedwoodCharacterComponent::OnRep_RedwoodPlayerUpdated() {
  OnRedwoodPlayerUpdated.Broadcast();
}

void URedwoodCharacterComponent::RedwoodPlayerStateCharacterUpdated() {
  // The source PlayerStateComponent->RedwoodCharacter struct is server-only and
  // is empty on clients. This function copies it into the replicated fields
  // below, so it must only run on the authority; clients receive the values via
  // replication and learn about the update via OnRep_RedwoodCharacterUpdated.
  // Running it on a client would overwrite the replicated fields (including
  // RedwoodCharacterId) with blanks.
  if (GetOwnerRole() != ROLE_Authority) {
    return;
  }

  APawn *Pawn = Cast<APawn>(GetOwner());
  AController *Controller = IsValid(Pawn) ? Pawn->GetController() : nullptr;
  APlayerState *PlayerState = IsValid(Controller)
    ? Cast<APlayerState>(Controller->PlayerState)
    : Cast<APlayerState>(GetOwner());

  URedwoodPlayerStateComponent *PlayerStateComponent = IsValid(PlayerState)
    ? PlayerState->FindComponentByClass<URedwoodPlayerStateComponent>()
    : nullptr;

  if (IsValid(PlayerStateComponent)) {
    FRedwoodCharacterBackend RedwoodCharacterBackend =
      PlayerStateComponent->RedwoodCharacter;

    RedwoodPlayerId = RedwoodCharacterBackend.PlayerId;
    RedwoodCharacterId = RedwoodCharacterBackend.Id;
    RedwoodCharacterName = RedwoodCharacterBackend.Name;

    // FORK(hollowed-oath) BEGIN: dirty-marks required by the push-model conversion of these
    // properties (see GetLifetimeReplicatedProps above). Upstream mutates them with no dirty-mark.
    MARK_PROPERTY_DIRTY_FROM_NAME(
      URedwoodCharacterComponent, RedwoodPlayerId, this
    );
    MARK_PROPERTY_DIRTY_FROM_NAME(
      URedwoodCharacterComponent, RedwoodCharacterId, this
    );
    MARK_PROPERTY_DIRTY_FROM_NAME(
      URedwoodCharacterComponent, RedwoodCharacterName, this
    );
    // FORK(hollowed-oath) END

    if (bUseCharacterCreatorData) {
      bool bErrored = false;
      bool bDirty = URedwoodCommonGameSubsystem::DeserializeBackendData(
        bStoreDataInActor ? (UObject *)Pawn : (UObject *)this,
        RedwoodCharacterBackend.CharacterCreatorData,
        *CharacterCreatorDataVariableName,
        LatestCharacterCreatorDataSchemaVersion,
        bErrored
      );

      if (bErrored) {
        // kick the player as they're not compatible with the server
        APlayerController *PlayerController =
          PlayerState->GetPlayerController();

        if (IsValid(PlayerController)) {
          AGameModeBase *GameMode = UGameplayStatics::GetGameMode(PlayerState);
          if (IsValid(GameMode) && GameMode->GameSession) {
            GameMode->GameSession->KickPlayer(
              PlayerController,
              FText::FromString(TEXT("Could not load player character data"))
            );
          }
        }
      }

      if (bDirty) {
        MarkCharacterCreatorDataDirty();
      }
    }

    if (bUseMetadata) {
      bool bErrored = false;
      bool bDirty = URedwoodCommonGameSubsystem::DeserializeBackendData(
        bStoreDataInActor ? (UObject *)Pawn : (UObject *)this,
        RedwoodCharacterBackend.Metadata,
        *MetadataVariableName,
        LatestMetadataSchemaVersion,
        bErrored
      );

      if (bErrored) {
        // kick the player as they're not compatible with the server
        APlayerController *PlayerController =
          PlayerState->GetPlayerController();

        if (IsValid(PlayerController)) {
          AGameModeBase *GameMode = UGameplayStatics::GetGameMode(PlayerState);
          if (IsValid(GameMode) && GameMode->GameSession) {
            GameMode->GameSession->KickPlayer(
              PlayerController,
              FText::FromString(TEXT("Could not load player character data"))
            );
          }
        }
      }

      if (bDirty) {
        MarkMetadataDirty();
      }
    }

    if (bUseEquippedInventory) {
      bool bErrored = false;
      bool bDirty = URedwoodCommonGameSubsystem::DeserializeBackendData(
        bStoreDataInActor ? (UObject *)Pawn : (UObject *)this,
        RedwoodCharacterBackend.EquippedInventory,
        *EquippedInventoryVariableName,
        LatestEquippedInventorySchemaVersion,
        bErrored
      );

      if (bErrored) {
        // kick the player as they're not compatible with the server
        APlayerController *PlayerController =
          PlayerState->GetPlayerController();

        if (IsValid(PlayerController)) {
          AGameModeBase *GameMode = UGameplayStatics::GetGameMode(PlayerState);
          if (IsValid(GameMode) && GameMode->GameSession) {
            GameMode->GameSession->KickPlayer(
              PlayerController,
              FText::FromString(TEXT("Could not load player character data"))
            );
          }
        }
      }

      if (bDirty) {
        MarkEquippedInventoryDirty();
      }
    }

    if (bUseNonequippedInventory) {
      bool bErrored = false;
      bool bDirty = URedwoodCommonGameSubsystem::DeserializeBackendData(
        bStoreDataInActor ? (UObject *)Pawn : (UObject *)this,
        RedwoodCharacterBackend.NonequippedInventory,
        *NonequippedInventoryVariableName,
        LatestNonequippedInventorySchemaVersion,
        bErrored
      );

      if (bErrored) {
        // kick the player as they're not compatible with the server
        APlayerController *PlayerController =
          PlayerState->GetPlayerController();

        if (IsValid(PlayerController)) {
          AGameModeBase *GameMode = UGameplayStatics::GetGameMode(PlayerState);
          if (IsValid(GameMode) && GameMode->GameSession) {
            GameMode->GameSession->KickPlayer(
              PlayerController,
              FText::FromString(TEXT("Could not load player character data"))
            );
          }
        }
      }

      if (bDirty) {
        MarkNonequippedInventoryDirty();
      }
    }

    if (bUseProgress) {
      bool bErrored = false;
      bool bDirty = URedwoodCommonGameSubsystem::DeserializeBackendData(
        bStoreDataInActor ? (UObject *)Pawn : (UObject *)this,
        RedwoodCharacterBackend.Progress,
        *ProgressVariableName,
        LatestProgressSchemaVersion,
        bErrored
      );

      if (bErrored) {
        // kick the player as they're not compatible with the server
        APlayerController *PlayerController =
          PlayerState->GetPlayerController();

        if (IsValid(PlayerController)) {
          AGameModeBase *GameMode = UGameplayStatics::GetGameMode(PlayerState);
          if (IsValid(GameMode) && GameMode->GameSession) {
            GameMode->GameSession->KickPlayer(
              PlayerController,
              FText::FromString(TEXT("Could not load player character data"))
            );
          }
        }
      }

      if (bDirty) {
        MarkProgressDirty();
      }
    }

    if (bUseData) {
      bool bErrored = false;
      bool bDirty = URedwoodCommonGameSubsystem::DeserializeBackendData(
        bStoreDataInActor ? (UObject *)Pawn : (UObject *)this,
        RedwoodCharacterBackend.Data,
        *DataVariableName,
        LatestDataSchemaVersion,
        bErrored
      );

      if (bErrored) {
        // kick the player as they're not compatible with the server
        APlayerController *PlayerController =
          PlayerState->GetPlayerController();

        if (IsValid(PlayerController)) {
          AGameModeBase *GameMode = UGameplayStatics::GetGameMode(PlayerState);
          if (IsValid(GameMode) && GameMode->GameSession) {
            GameMode->GameSession->KickPlayer(
              PlayerController,
              FText::FromString(TEXT("Could not load player character data"))
            );
          }
        }
      }

      if (bDirty) {
        MarkDataDirty();
      }
    }

    if (bUseAbilitySystem) {
      bool bErrored = false;
      bool bDirty = URedwoodCommonGameSubsystem::DeserializeBackendData(
        bStoreDataInActor ? (UObject *)Pawn : (UObject *)this,
        RedwoodCharacterBackend.AbilitySystem,
        *AbilitySystemVariableName,
        LatestAbilitySystemSchemaVersion,
        bErrored
      );

      if (bErrored) {
        // kick the player as they're not compatible with the server
        APlayerController *PlayerController =
          PlayerState->GetPlayerController();

        if (IsValid(PlayerController)) {
          AGameModeBase *GameMode = UGameplayStatics::GetGameMode(PlayerState);
          if (IsValid(GameMode) && GameMode->GameSession) {
            GameMode->GameSession->KickPlayer(
              PlayerController,
              FText::FromString(TEXT("Could not load player character data"))
            );
          }
        }
      }

      if (bDirty) {
        MarkAbilitySystemDirty();
      }
    }

    // FORK(hollowed-oath) BEGIN: item LOAD leg of HollowedOath's per-bag inventory. Copies
    // the persisted item rows into the game's ItemsVariableName array BEFORE the
    // OnRedwoodCharacterUpdated broadcast, so game code (AClient / the per-bag inventory) sees
    // the rows already present the moment it reacts. Entirely fork-added -- upstream has no
    // item-row concept. Merge must preserve the ordering: this block runs before the broadcast.
    // Items ride the SAME round trip as every field above (RedwoodCharacterBackend.Items
    // -- populated by the player-auth / character-load response, not a separate
    // realm:characters:items:load call), so this runs BEFORE the OnRedwoodCharacterUpdated
    // broadcast below like every other channel: game code reacting to that broadcast can rely on
    // ItemsVariableName already reflecting the persisted rows, with no later-arriving
    // corrective pass required. SeedItemSeqFromCharacter primes the single-flight batchSeq state
    // from the loaded InventorySeq so the first flush of the session continues the backend's
    // sequence (see the state-member FORK block in the header) -- it must run here, before any
    // flush can be triggered by game code reacting to the broadcast.
    // INITIAL-LOAD-ONLY. RedwoodPlayerStateCharacterUpdated() re-runs on every OnControllerChanged,
    // and the note that used to live here said this was safe "until linkdead pawn retention (#1365)
    // lands and a live pawn can be re-possessed after mutations have already dirtied the wire array
    // past the login snapshot". #1365 HAS landed, and the same re-broadcast also fires when the
    // PlayerState save branch publishes its data — so re-applying the login snapshot would overwrite
    // live staging: moves and quantity changes revert on relog, and ids dirtied since login are left
    // with no matching row to send. Hydrate once per character instead; a component genuinely reused
    // for a DIFFERENT character still hydrates, because the guard is keyed on the character id.
    if (bUseItems) {
      const FString &IncomingCharacterId = RedwoodCharacterBackend.Id;
      if (HydratedItemsCharacterId != IncomingCharacterId) {
        // FORK(hollowed-oath): I6. A component reused for another character starts clean.
        if (!HydratedItemsCharacterId.IsEmpty()) {
          ResetCurrencyForNewCharacter(HydratedItemsCharacterId, IncomingCharacterId);
        }
        TArray<FRedwoodItemRecord> *RecordsArray =
          URedwoodCommonGameSubsystem::ResolveItemsRecordsArray(this);
        if (RecordsArray) {
          *RecordsArray = RedwoodCharacterBackend.Items;
        }
        // Seeded alongside the snapshot for the same reason: re-seeding mid-session from the stale
        // login InventorySeq would rewind the fence behind flushes that have already committed.
        SeedItemSeqFromCharacter(RedwoodCharacterBackend.InventorySeq);
        // FORK(hollowed-oath): the session's writer token, which every flush of this component
        // carries. Taken from the snapshot, so a component recreated mid-session (a respawn)
        // has it too.
        SeedWriterToken(RedwoodCharacterBackend.WriterToken);
        // FORK(hollowed-oath): a snapshot whose balance is stale must not seed a currency chain.
        if (RedwoodCharacterBackend.bCurrencyBalancesUnknown) {
          RequireCurrencyReload();
        }
        // FORK(hollowed-oath): what the component this one replaces never sent or never had settled, and the snapshot
        // this one must reach at EndPlay.
        HydratedPlayerStateComponent = PlayerStateComponent;
        AdoptHandedOffCurrency(PlayerStateComponent->RedwoodCharacter);
        // FORK(hollowed-oath): a batch of the component this one replaces may have committed at
        // the marked seq; probe it before anything else goes out (see bInventorySeqUnconfirmed).
        if (RedwoodCharacterBackend.bInventorySeqUnconfirmed) {
          RequireItemSeqProbe(
            RedwoodCharacterBackend.UnconfirmedInventorySeq, RedwoodCharacterBackend.bCurrencyBalancesUnknown
          );
        }
        // FORK(hollowed-oath): a snapshot the freeze marked (a respawn in the same session) starts
        // frozen. A hydration never thaws: only the auth leg's fresh database load does (see
        // URedwoodServerGameSubsystem::ThawCharacter), and the subsystem's per-character freeze
        // covers a snapshot the freeze could not reach.
        const URedwoodServerGameSubsystem *ServerSubsystem = GetServerSubsystem();
        bFrozenAfterFinalRefusal = RedwoodCharacterBackend.bFrozenAfterFinalRefusal ||
          (ServerSubsystem && ServerSubsystem->IsCharacterFrozen(IncomingCharacterId));
        HydratedItemsCharacterId = IncomingCharacterId;
        // FORK(hollowed-oath): a component that hydrates frozen tells the game, or with no
        // handler gets the default kick (R35). It is the one place that covers both a respawn
        // after a live freeze and a freeze whose component was gone (its reply froze the character
        // with nobody to tell).
        if (bFrozenAfterFinalRefusal) {
          NotifyItemFlushRefusedFinal(TEXT("the character is frozen after a final refusal"));
        }
      }
    }
    // FORK(hollowed-oath) END

    // Broadcast locally on the server, then bump the replicated counter so the
    // notify fires on clients once the updated fields have been applied.
    OnRedwoodCharacterUpdated.Broadcast();
    RedwoodCharacterUpdateCount++;
    // FORK(hollowed-oath): dirty-mark for the push-model counter that drives the client-side
    // OnRep notify; upstream leaves it unmarked (legacy always-poll rep).
    MARK_PROPERTY_DIRTY_FROM_NAME(
      URedwoodCharacterComponent, RedwoodCharacterUpdateCount, this
    );
  }
}

void URedwoodCharacterComponent::OnRep_RedwoodCharacterUpdated() {
  OnRedwoodCharacterUpdated.Broadcast();
}

// FORK(hollowed-oath) BEGIN: protocol-v2 single-flight/batchSeq item-flush lifecycle. Fork-added;
// no upstream counterpart. See the state-member rationale block in the header for WHY the item
// channel is single-flight and sequence-fenced (the backend rejects seq gaps and no-ops replays,
// so overlapping in-flight batches must be prevented).
bool URedwoodCharacterComponent::TryBeginItemFlush(int64 &OutBatchSeq) {
  if (bItemFlushInFlight) {
    // A batch is already outstanding; the caller must not send a second, overlapping one.
    return false;
  }

  bItemFlushInFlight = true;
  // FORK(hollowed-oath): an unconfirmed currency batch goes out again under its OWN seq, even if a
  // trade fence has since seeded the lane forward. Only that seq lets the backend tell whether the
  // batch committed (a replay it acks from its ledger) or not (a retriable refusal); under a new
  // seq a committed entry would be refused as a duplicate and the batch lost with it.
  InFlightBatchSeq = bCurrencyBatchUnconfirmed ? UnconfirmedBatchSeq : NextBatchSeq;
  bInFlightIsCurrencyResend = bCurrencyBatchUnconfirmed;
  // The same counts ClaimCurrencyToSend takes, so EndPlay knows which entries the batch carries.
  InFlightCurrencyCount = FMath::Min(GetCurrencyEntriesToSendCount(), PendingCurrencyEntries.Num());
  InFlightGrantCount = FMath::Min(GetGrantKeysToSendCount(), PendingGrantKeys.Num());
  OutBatchSeq = InFlightBatchSeq;
  return true;
}

void URedwoodCharacterComponent::CompleteItemFlush(
  const FRedwoodItemFlushReply &Reply, const FRedwoodItemFlushSent &Sent
) {
  // The seq the just-completed flush stamped on its payload (claimed in TryBeginItemFlush).
  const int64 SentSeq = InFlightBatchSeq;
  const int64 CommittedSeq = Reply.CommittedSeq;

  // FORK(hollowed-oath): nothing leaves the queues between the claim and this ack except here, so
  // the counts can only exceed the queues through a caller bug. Clamp rather than over-remove.
  const int32 SentCurrencyCount = FMath::Clamp(Sent.CurrencyCount, 0, PendingCurrencyEntries.Num());
  const int32 SentGrantCount = FMath::Clamp(Sent.GrantKeys.Num(), 0, PendingGrantKeys.Num());

  const ERedwoodItemFlushOutcome Outcome = ClassifyItemFlushReply(Reply, SentSeq);
  switch (Outcome) {
    case ERedwoodItemFlushOutcome::NotSent:
      // FORK(hollowed-oath): the realm never saw this batch. Marking it unconfirmed would turn the
      // next attempt into a probe that carries no rows, for no reason; clearing an earlier
      // unconfirmed state would send that batch's entries as new under a seq that may hold them
      // already. Leave both as they are: the rows stay dirty and the entries stay queued.
      break;

    case ERedwoodItemFlushOutcome::NotApplied:
      // FORK(hollowed-oath): the backend answered a probe: the batch never committed. The backend
      // has CONSUMED that seq with an empty commit (CommittedSeq), so a late original arriving
      // under it now fails instead of applying after the fact. The entries are simply pending
      // again, unchanged: the empty commit moved no balance, so their BalanceAfter still holds. The
      // next claim is a normal batch after that fence, so the payment goes out WITH the current
      // item rows: both save or neither does.
      bCurrencyBatchUnconfirmed = false;
      AdoptCommittedItemSeq(CommittedSeq);
      break;

    case ERedwoodItemFlushOutcome::Committed:
      // Success: clear only the ids whose generation still matches what was sent, exactly as the
      // old container ack did -- if MarkItemsDirty/MarkItemsDeleted bumped an id again while the
      // request was in flight, leave it dirty (at its newer generation) so the next flush re-sends
      // the newer record instead of clearing away a write we never actually persisted.
      ClearSentItemGenerations(Sent.Upserts, Sent.Deletes);
      // FORK(hollowed-oath): the backend holds these entries -- committed now, or by the earlier
      // attempt this batch replays (the backend acks a replay that carries currency only when
      // every entry id is already in its ledger). Entries queued after the claim sit behind them
      // and stay.
      RemoveSentCurrency(SentCurrencyCount, SentGrantCount);
      bCurrencyBatchUnconfirmed = false;
      AdoptCommittedItemSeq(CommittedSeq);
      break;

    case ERedwoodItemFlushOutcome::FinalRefusal:
      // FORK(hollowed-oath): the backend refused this batch for good, rolled ALL of it back, and
      // raised an alert. The game's state and the backend's now disagree, and a later staged change
      // may depend on the refused one, so nothing staged can be trusted: freeze (see
      // FreezeAfterFinalRefusal).
      FreezeAfterFinalRefusal(Reply.Error);
      if (CommittedSeq >= SentSeq) {
        AdoptCommittedItemSeq(CommittedSeq);
      }
      break;

    case ERedwoodItemFlushOutcome::RebaseRequired:
      // FORK(hollowed-oath): only the backend's replay-mismatch refusal sets rebaseRequired: a trade
      // used this seq, and the backend checked that the batch's entry ids and keys are NOT all in
      // its ledger. Exactly one batch is re-sent per seq (see the staging block in the header), so
      // none of it applied. The entries are kept -- an error reply means not saved. The trade moved
      // the balance, so every pending BalanceAfter is stale and would be refused as final. Hold
      // currency until the caller reloads; the flag is the signal for the flush path to start that
      // reload and then call RebaseCurrency. The lane moves past the trade's seq, or every later
      // flush would be wedged behind a fence the backend keeps rejecting.
      if (CommittedSeq >= SentSeq) {
        AdoptCommittedItemSeq(CommittedSeq);
      }
      bCurrencyBatchUnconfirmed = false;
      bCurrencyRebaseRequired = true;
      break;

    case ERedwoodItemFlushOutcome::SequenceGap:
      // FORK(hollowed-oath): the backend is behind the seq we sent, so this batch (and any earlier
      // attempt of it under the same seq) cannot have committed. Re-sending the gapped seq would be
      // refused forever; realign to the backend's fence and send the kept entries as an ordinary
      // batch there.
      LastCommittedItemSeq = CommittedSeq;
      NextBatchSeq = CommittedSeq + 1;
      bCurrencyBatchUnconfirmed = false;
      break;

    case ERedwoodItemFlushOutcome::Unknown:
    default:
      if (Reply.bNotApplied) {
        // FORK(hollowed-oath): notApplied without the consumed seq. While the seq is still free,
        // the original attempt can arrive late and commit under it, so a normal batch there could
        // pay the entries twice. Keep the batch unconfirmed: the next attempt is another probe, and
        // no seq below the sent one is ever adopted.
        UE_LOG(
          LogRedwood,
          Error,
          TEXT("CompleteItemFlush: notApplied with committedSeq %lld below the probed seq %lld; probing again"),
          CommittedSeq,
          SentSeq
        );
      } else if (Reply.Error.IsEmpty()) {
        // FORK(hollowed-oath): a malformed SUCCESS ack (committedSeq below the sent seq, which a
        // missing field also gives). Clearing the rows while refusing the reported seq was the
        // worst of both worlds: NextBatchSeq stayed where it was, the NEXT flush re-sent the same
        // batchSeq, the backend no-oped it as a replay, and that no-op's success ack then cleared
        // the NEW mutation. Adopting the bogus seq instead would rewind the lane and wedge it. So
        // the rows stay dirty, the lane stays where it is, and the batch is probed at its seq: the
        // probe's ack, which carries a real fence, settles it.
        UE_LOG(
          LogRedwood,
          Warning,
          TEXT(
            "CompleteItemFlush: success ack reported committedSeq %lld < sent batchSeq %lld "
            "(contract violation -- missing/malformed committedSeq field?); ignoring the reported "
            "seq, KEEPING the sent rows dirty, and probing the batch at its seq"
          ),
          CommittedSeq,
          SentSeq
        );
      } else if (CommittedSeq >= SentSeq) {
        // An error with a fence at or past the sent seq (a replay the backend no-oped, or a fence
        // ahead of us): keeping the old NextBatchSeq would wedge every later flush behind it.
        AdoptCommittedItemSeq(CommittedSeq);
      }
      // FORK(hollowed-oath): the outcome is unknown, WHATEVER committedSeq says. A "concurrent
      // flush" refusal of a retry that raced its own still-running original reports the original's
      // commit as the fence; treating that as "did not apply" and rebasing would re-chain the
      // committed entry on top of itself, and its re-send would be refused as a duplicate. Probe
      // the identical batch under its seq instead: the backend acks it from its ledger if it
      // committed.
      MarkCurrencyBatchUnconfirmed(SentCurrencyCount, SentGrantCount, SentSeq);
      break;
  }

  // Always release the flight slot, whatever the outcome.
  bItemFlushInFlight = false;

  // FORK(hollowed-oath): after the state above is settled, so a listener that reacts (by returning
  // the player to character select) sees the component frozen and empty.
  if (Outcome == ERedwoodItemFlushOutcome::FinalRefusal) {
    NotifyItemFlushRefusedFinal(Reply.Error);
  }

  // A flush attempt that arrived while this batch was outstanding PARKED itself here rather than
  // settling — see FlushItemsForCharacterComponent's single-flight branch. The slot is free again
  // now, so run it. This is what makes a DETACHED pawn's barrier a delivery guarantee instead of an
  // attempt guarantee: a detached component has no next tick to retry on, so without this the parked
  // rows were stranded the moment the binding released (#1534). Move the continuation out BEFORE
  // invoking it: the retry re-enters this function on its own ack, and must find an empty slot
  // rather than re-running itself.
  if (PendingItemFlushRetry) {
    TFunction<void()> Retry = MoveTemp(PendingItemFlushRetry);
    PendingItemFlushRetry = nullptr;
    Retry();
  }
}

void URedwoodCharacterComponent::SeedItemSeqFromCharacter(int64 InventorySeq) {
  LastCommittedItemSeq = InventorySeq;
  NextBatchSeq = InventorySeq + 1;
}

void URedwoodCharacterComponent::AdoptCommittedItemSeq(int64 CommittedSeq) {
  if (CommittedSeq > LastCommittedItemSeq) {
    LastCommittedItemSeq = CommittedSeq;
  }
  if (CommittedSeq + 1 > NextBatchSeq) {
    NextBatchSeq = CommittedSeq + 1;
  }
}

void URedwoodCharacterComponent::ClearSentItemGenerations(
  const TArray<TPair<FString, uint64>> &SentUpserts,
  const TArray<TPair<FString, uint64>> &SentDeletes
) {
  for (const TPair<FString, uint64> &Sent : SentUpserts) {
    uint64 *CurrentGeneration = DirtyItemGenerations.Find(Sent.Key);
    if (CurrentGeneration && *CurrentGeneration == Sent.Value) {
      DirtyItemGenerations.Remove(Sent.Key);
    }
  }
  for (const TPair<FString, uint64> &Sent : SentDeletes) {
    uint64 *CurrentGeneration = PendingDeletedItemGenerations.Find(Sent.Key);
    if (CurrentGeneration && *CurrentGeneration == Sent.Value) {
      PendingDeletedItemGenerations.Remove(Sent.Key);
    }
  }
}

URedwoodPlayerStateComponent *URedwoodCharacterComponent::FindPlayerStateComponent() const {
  // Reached the way hydration reaches it.
  APawn *Pawn = Cast<APawn>(GetOwner());
  AController *Controller = IsValid(Pawn) ? Pawn->GetController() : nullptr;
  APlayerState *PlayerState = IsValid(Controller)
    ? Controller->PlayerState.Get()
    : Cast<APlayerState>(GetOwner());
  return IsValid(PlayerState) ? PlayerState->FindComponentByClass<URedwoodPlayerStateComponent>()
                              : nullptr;
}

APlayerController *URedwoodCharacterComponent::FindPlayerController() const {
  if (const APawn *Pawn = Cast<APawn>(GetOwner())) {
    return Cast<APlayerController>(Pawn->GetController());
  }
  const APlayerState *PlayerState = Cast<APlayerState>(GetOwner());
  return IsValid(PlayerState) ? PlayerState->GetPlayerController() : nullptr;
}

URedwoodServerGameSubsystem *URedwoodCharacterComponent::GetServerSubsystem() const {
  UGameInstance *GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
  return GameInstance ? GameInstance->GetSubsystem<URedwoodServerGameSubsystem>() : nullptr;
}

bool URedwoodCharacterComponent::HasGrantKey(const FString &GrantKey) const {
  if (ContainsGrantKey(PendingGrantKeys, GrantKey)) {
    return true;
  }
  // FORK(hollowed-oath): a key a destroyed component handed off is used too, though no
  // component has taken it yet: staging it again would get the batch refused for good.
  const URedwoodPlayerStateComponent *PlayerStateComponent = FindPlayerStateComponent();
  return PlayerStateComponent &&
    (ContainsGrantKey(PlayerStateComponent->RedwoodCharacter.GrantKeys, GrantKey) ||
     ContainsGrantKey(PlayerStateComponent->RedwoodCharacter.HandedOffGrantKeys, GrantKey));
}

bool URedwoodCharacterComponent::QueueCurrency(
  const TArray<FRedwoodCurrencyEntry> &Entries, const TArray<FString> &GrantKeys
) {
  // Frozen, per component (this one was refused) or per character (another component of it was):
  // an amount staged now would be chained from a balance nobody can vouch for. A lost writer fence
  // is refused the same way: this component never sends again, and an entry accepted here would be
  // money the game believes saved.
  const URedwoodServerGameSubsystem *ServerSubsystem = GetServerSubsystem();
  const bool bFrozen = bFrozenAfterFinalRefusal ||
    (ServerSubsystem && ServerSubsystem->IsCharacterFrozen(RedwoodCharacterId));
  if (bFrozen || bWriterFenceLost) {
    const TCHAR *Why = bFrozen ? TEXT("is frozen after a final refusal")
                               : TEXT("lost its writer fence (another session owns it)");
    for (const FRedwoodCurrencyEntry &Entry : Entries) {
      UE_LOG(
        LogRedwood,
        Error,
        TEXT("QueueCurrency: character %s %s; refused entry %s delta %lld reason %s"),
        *RedwoodCharacterId,
        Why,
        *Entry.Id,
        Entry.Delta,
        *Entry.Reason
      );
    }
    for (const FString &GrantKey : GrantKeys) {
      UE_LOG(
        LogRedwood,
        Error,
        TEXT("QueueCurrency: character %s %s; refused grant %s"),
        *RedwoodCharacterId,
        Why,
        *GrantKey
      );
    }
    return Entries.Num() == 0 && GrantKeys.Num() == 0;
  }

  // FORK(hollowed-oath): while a trade that moves this character's currency is unsettled, every
  // entry staged now is re-chained when it settles, from the balance after it or before it. A debit
  // that would go below zero on the lower one would then be held back for good, so refuse it now,
  // while the game can still undo the purchase it pays for. Credits always fit.
  // Per currency: a trade floors only the currencies it moves.
  FRedwoodCurrencyBalances TradeFloors;
  if (ServerSubsystem) {
    ServerSubsystem->GetUnsettledTradeFloorBalances(RedwoodCharacterId, TradeFloors);
  }
  auto AddToTradeFloor = [&TradeFloors](const FRedwoodCurrencyEntry &Entry) {
    if (int64 *Floor = TradeFloors.Find(Entry.Currency)) {
      // Saturated: an overflowing chain is refused by the re-chain itself.
      *Floor = Entry.Delta > 0 && *Floor > MAX_int64 - Entry.Delta ? MAX_int64 : *Floor + Entry.Delta;
    }
  };
  for (const FRedwoodCurrencyEntry &Pending : PendingCurrencyEntries) {
    AddToTradeFloor(Pending);
  }

  // All or nothing: every entry is checked before any is queued. A caller that hears false undoes
  // its whole change, so an entry or a key queued beside a refused one would be saved for a change
  // that never happened.
  bool bAllValid = true;
  for (const FRedwoodCurrencyEntry &Entry : Entries) {
    // A bad id would make the backend refuse the whole batch for good, taking every valid entry
    // and item row sent with it. Refuse the call here instead, loudly: it is a caller bug.
    // An Error log, not an ensure: the caller learns it from the return value.
    if (!IsValidCurrencyEntryId(Entry.Id)) {
      UE_LOG(
        LogRedwood,
        Error,
        TEXT("QueueCurrency: refused entry id '%s' (delta %lld): not 32 uppercase hex digits"),
        *Entry.Id,
        Entry.Delta
      );
      bAllValid = false;
      continue;
    }
    // The same: the backend refuses an entry without a currency for good.
    if (Entry.Currency.IsEmpty()) {
      UE_LOG(
        LogRedwood,
        Error,
        TEXT("QueueCurrency: refused entry %s (delta %lld): it names no currency"),
        *Entry.Id,
        Entry.Delta
      );
      bAllValid = false;
      continue;
    }
    const int64 *TradeFloor = TradeFloors.Find(Entry.Currency);
    if (TradeFloor && Entry.Delta < 0 && *TradeFloor + Entry.Delta < 0) {
      UE_LOG(
        LogRedwood,
        Error,
        TEXT("QueueCurrency: character %s has a trade unsettled and could hold only %lld %s after it; refused entry %s delta %lld"),
        *RedwoodCharacterId,
        *TradeFloor,
        *Entry.Currency,
        *Entry.Id,
        Entry.Delta
      );
      bAllValid = false;
      continue;
    }
    AddToTradeFloor(Entry);
  }
  if (!bAllValid) {
    return false;
  }
  // Appended, never merged or de-duplicated: the backend checks each BalanceAfter in order, and a
  // repeated grant key must reach the backend so it refuses the second reward instead of the game
  // server quietly paying it without its guard.
  PendingCurrencyEntries.Append(Entries);
  PendingGrantKeys.Append(GrantKeys);
  return true;
}

// The unconfirmed batch first: its probe goes out unchanged even while a reload waits behind it
// (RequireItemSeqProbe), because the reload may read the balance only once the probe has settled.
int32 URedwoodCharacterComponent::GetCurrencyEntriesToSendCount() const {
  if (bCurrencyBatchUnconfirmed) {
    return UnconfirmedCurrencyCount;
  }
  return bCurrencyRebaseRequired ? 0 : PendingCurrencyEntries.Num();
}

int32 URedwoodCharacterComponent::GetGrantKeysToSendCount() const {
  if (bCurrencyBatchUnconfirmed) {
    return UnconfirmedGrantCount;
  }
  return bCurrencyRebaseRequired ? 0 : PendingGrantKeys.Num();
}

void URedwoodCharacterComponent::RebaseCurrency(const FRedwoodCurrencyBalances &Balances) {
  if (!bCurrencyRebaseRequired) {
    UE_LOG(LogRedwood, Warning, TEXT("RebaseCurrency: no rebase is required; ignoring the reloaded balances"));
    return;
  }
  if (bItemFlushInFlight || bCurrencyBatchUnconfirmed) {
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT("RebaseCurrency: a batch is in flight or unconfirmed; ignoring the reloaded balances until it settles")
    );
    return;
  }

  // On an overflow the rebase stays pending: the queue is held, not dropped, until a reload yields
  // a balance that fits.
  if (TryRechainPendingCurrency(Balances)) {
    bCurrencyRebaseRequired = false;
  }
}

void URedwoodCharacterComponent::RechainCurrencyAfterTrade(const FRedwoodCurrencyBalances &Balances) {
  // A batch whose reply is still open may or may not be in Balance, so no balance is known: a
  // re-chain could count its entries twice, and a reload would chain the pending entries on top of
  // that batch while it is still re-sent. The trade's hold makes this unreachable (a trade never
  // starts with a batch open), so reaching it is a bug, and only the database can repair the
  // state: freeze, as a final refusal does.
  if (!ensureMsgf(
        !bItemFlushInFlight && !bCurrencyBatchUnconfirmed,
        TEXT("RechainCurrencyAfterTrade: character %s has a batch in flight or unconfirmed"),
        *RedwoodCharacterId
      )) {
    const FString Reason = TEXT("a trade settled while a batch was in flight or unconfirmed");
    if (URedwoodServerGameSubsystem *ServerSubsystem = GetServerSubsystem()) {
      ServerSubsystem->FreezeCharacter(RedwoodCharacterId, FindPlayerStateComponent());
    }
    // With a batch in flight, its reply decides its entries; the reply then finds the character
    // frozen and drops the rest (URedwoodServerGameSubsystem::HandleItemsFlushReply).
    if (!bItemFlushInFlight) {
      FreezeAfterFinalRefusal(Reason);
    }
    NotifyItemFlushRefusedFinal(Reason);
    return;
  }
  if (!TryRechainPendingCurrency(Balances)) {
    bCurrencyRebaseRequired = true;
    return;
  }
  OnCurrencyRebased.Broadcast(Balances);
}

bool URedwoodCharacterComponent::TryRechainPendingCurrency(const FRedwoodCurrencyBalances &Balances) {
  // Checked before anything is written: a batch that mixes stale and re-chained values is refused
  // for good. So is a chain below zero, and a final refusal freezes the character: keep such a
  // chain out of every batch instead. The caller holds the queue for a reload, and the reload's
  // give-up logs what stays unsaved. Each currency is its own chain.
  FRedwoodCurrencyBalances Running;
  auto RunningOf = [&Running, &Balances](const FString &Currency) -> int64 & {
    if (int64 *Found = Running.Find(Currency)) {
      return *Found;
    }
    return Running.Add(Currency, GetCurrencyBalance(Balances, Currency));
  };
  for (const FRedwoodCurrencyEntry &Entry : PendingCurrencyEntries) {
    int64 &Balance = RunningOf(Entry.Currency);
    const bool bOverflows = Entry.Delta > 0 ? Balance > MAX_int64 - Entry.Delta
                                            : Balance < MIN_int64 - Entry.Delta;
    if (bOverflows) {
      UE_LOG(
        LogRedwood,
        Error,
        TEXT("RebaseCurrency: entry %s overflows int64 from %s balance %lld; currency stays held"),
        *Entry.Id,
        *Entry.Currency,
        Balance
      );
      return false;
    }
    Balance += Entry.Delta;
    if (Balance < 0) {
      UE_LOG(
        LogRedwood,
        Error,
        TEXT("Re-chaining currency: entry %s (delta %lld) of character %s takes the %s balance from %lld to %lld, below zero; currency stays held"),
        *Entry.Id,
        Entry.Delta,
        *RedwoodCharacterId,
        *Entry.Currency,
        GetCurrencyBalance(Balances, Entry.Currency),
        Balance
      );
      return false;
    }
  }
  Running.Reset();
  for (FRedwoodCurrencyEntry &Entry : PendingCurrencyEntries) {
    int64 &Balance = RunningOf(Entry.Currency);
    Balance += Entry.Delta;
    Entry.BalanceAfter = Balance;
  }
  return true;
}

void URedwoodCharacterComponent::AbandonInFlightItemFlush(int32 SentCurrencyCount, int32 SentGrantCount) {
  if (!bItemFlushInFlight) {
    return;
  }
  MarkCurrencyBatchUnconfirmed(
    FMath::Clamp(SentCurrencyCount, 0, PendingCurrencyEntries.Num()),
    FMath::Clamp(SentGrantCount, 0, PendingGrantKeys.Num()),
    InFlightBatchSeq
  );
  bItemFlushInFlight = false;
}

void URedwoodCharacterComponent::MarkWriterFenceLost() {
  if (bWriterFenceLost) {
    return;
  }
  bWriterFenceLost = true;
  UE_LOG(
    LogRedwood,
    Warning,
    TEXT("Character %s: another session owns this character now (writer fence lost); this component stops sending"),
    *RedwoodCharacterId
  );
  // Kept, not dropped, but never sent from here: the same lines a freeze writes, so staff can tell
  // what may need a re-grant.
  LogStagedCurrencyForStaff(TEXT("writer fence lost, another session owns the character"));
  OnWriterFenceLost.Broadcast(RedwoodCharacterId);
  // R36: the default kick, unless the game said it handles this (bGameHandlesCurrencyDesync).
  if (URedwoodServerGameSubsystem *ServerSubsystem = GetServerSubsystem()) {
    ServerSubsystem->KickPlayerAfterCurrencyDesync(this, TEXT("its writer fence is lost"));
  }
}

void URedwoodCharacterComponent::NotifyItemFlushRefusedFinal(const FString &Error) {
  OnItemFlushRefusedFinal.Broadcast(RedwoodCharacterId, Error);
  // R36: as in MarkWriterFenceLost.
  if (URedwoodServerGameSubsystem *ServerSubsystem = GetServerSubsystem()) {
    ServerSubsystem->KickPlayerAfterCurrencyDesync(
      this, FString::Printf(TEXT("it is frozen after a final refusal (%s)"), *Error)
    );
  }
}

void URedwoodCharacterComponent::LogStagedCurrencyForStaff(const FString &Why) const {
  // The backend has the refusal alert; these lines are what staff rebuild the rest from. The front
  // of an unconfirmed batch may have committed (its ack was lost), so it is told apart: re-granting
  // a saved amount without its id would pay it twice.
  LogCurrencyForStaff(
    RedwoodCharacterId,
    Why,
    PendingCurrencyEntries,
    bCurrencyBatchUnconfirmed ? UnconfirmedCurrencyCount : 0,
    PendingGrantKeys,
    bCurrencyBatchUnconfirmed ? UnconfirmedGrantCount : 0
  );
}

void URedwoodCharacterComponent::FreezeAfterFinalRefusal(const FString &Reason) {
  DropAllStaged(FString::Printf(TEXT("dropped by the freeze after a final refusal (%s)"), *Reason));
  bFrozenAfterFinalRefusal = true;
}

void URedwoodCharacterComponent::DropStagedForIdentityMismatch(const FString &AuthoritativeCharacterId) {
  if (bItemFlushInFlight || !IsItemsDirty()) {
    return;
  }
  DropAllStaged(FString::Printf(
    TEXT("dropped by the save, which skips a component that does not play the PlayerState's character %s"),
    *AuthoritativeCharacterId
  ));
}

void URedwoodCharacterComponent::DropAllStaged(const FString &Why) {
  LogStagedCurrencyForStaff(Why);
  PendingCurrencyEntries.Empty();
  PendingGrantKeys.Empty();
  bCurrencyBatchUnconfirmed = false;
  UnconfirmedCurrencyCount = 0;
  UnconfirmedGrantCount = 0;
  bCurrencyRebaseRequired = false;
  ClearItemsDirtyState();
}

void URedwoodCharacterComponent::HandOffCurrencyAtEndPlay(EEndPlayReason::Type EndPlayReason) {
  if (!bItemFlushInFlight && !bCurrencyBatchUnconfirmed && PendingCurrencyEntries.Num() == 0 &&
      PendingGrantKeys.Num() == 0) {
    return;
  }
  URedwoodServerGameSubsystem *ServerSubsystem = GetServerSubsystem();
  URedwoodPlayerStateComponent *PlayerStateComponent = FindPlayerStateComponent();
  if (!PlayerStateComponent) {
    PlayerStateComponent = HydratedPlayerStateComponent.Get();
  }
  // A PlayerState that ends with this component (it sits on it) or for another character cannot
  // take anything.
  const bool bSnapshotOutlivesComponent = IsValid(PlayerStateComponent) &&
    !RedwoodCharacterId.IsEmpty() && PlayerStateComponent->RedwoodCharacter.Id == RedwoodCharacterId &&
    PlayerStateComponent->GetOwner() != GetOwner() && IsValid(PlayerStateComponent->GetOwner()) &&
    !PlayerStateComponent->GetOwner()->IsActorBeingDestroyed();

  // Entries and keys are handed off only when nothing can make their chain wrong behind the next
  // component's back: the reload that rebases them must read a balance no open batch can still
  // change, and a frozen or fenced character must not send at all. Nor when item rows are staged:
  // the rows end with this component (the next one hydrates from the snapshot, which never had
  // them), and a key or a payment saved without the rows it pays for is half a transaction. A key
  // saved alone would also block that reward for good.
  const bool bFrozen = bFrozenAfterFinalRefusal ||
    (ServerSubsystem && ServerSubsystem->IsCharacterFrozen(RedwoodCharacterId));
  const bool bWorldEnding = EndPlayReason != EEndPlayReason::Destroyed &&
    EndPlayReason != EEndPlayReason::RemovedFromWorld;
  const TCHAR *Unsafe = bWorldEnding ? TEXT("the world is ending")
    : !bSnapshotOutlivesComponent ? TEXT("no PlayerState of the character outlives the component")
    : bItemFlushInFlight ? TEXT("a batch was in flight, so the balance they chain from waits on its reply")
    : bFrozen ? TEXT("the character is frozen")
    : bWriterFenceLost ? TEXT("the writer fence is lost")
    : DirtyItemGenerations.Num() > 0 || PendingDeletedItemGenerations.Num() > 0
      ? TEXT("item rows staged with them end with the component")
    : nullptr;

  // The front of the queues that already went out. A batch in flight is settled by its reply, as
  // a gone component's: that logs it and writes the snapshot (detached here, so the reply does
  // not reach a component that is ending; the detach marks its seq at once). An unconfirmed batch
  // has no reply coming: logged here as UNKNOWN, and its seq marked, so the next component probes
  // it. When it can, the batch goes to the PlayerState with that seq, so the probe carries it and
  // a notApplied answer puts it back in the queue.
  int32 FirstUnsentEntry = 0;
  int32 FirstUnsentGrant = 0;
  if (bItemFlushInFlight) {
    FirstUnsentEntry = InFlightCurrencyCount;
    FirstUnsentGrant = InFlightGrantCount;
    if (ServerSubsystem) {
      ServerSubsystem->DetachItemFlushesFromComponent(this);
    }
  } else if (bCurrencyBatchUnconfirmed) {
    FirstUnsentEntry = FMath::Clamp(UnconfirmedCurrencyCount, 0, PendingCurrencyEntries.Num());
    FirstUnsentGrant = FMath::Clamp(UnconfirmedGrantCount, 0, PendingGrantKeys.Num());
    const TConstArrayView<FRedwoodCurrencyEntry> UnconfirmedEntries =
      MakeArrayView(PendingCurrencyEntries).Left(FirstUnsentEntry);
    const TConstArrayView<FString> UnconfirmedGrantKeys =
      MakeArrayView(PendingGrantKeys).Left(FirstUnsentGrant);
    if (bSnapshotOutlivesComponent) {
      URedwoodServerGameSubsystem::MarkSnapshotSeqUnconfirmed(
        PlayerStateComponent->RedwoodCharacter,
        UnconfirmedBatchSeq,
        FirstUnsentEntry > 0 || FirstUnsentGrant > 0
      );
    }
    // Only at the front of an empty hand-off, and only while the snapshot probes this very seq: the
    // next component probes whatever is at the front, at the snapshot's marked seq.
    const bool bHandOnUnconfirmed = !Unsafe && (FirstUnsentEntry > 0 || FirstUnsentGrant > 0) &&
      PlayerStateComponent->RedwoodCharacter.HandedOffCurrencyEntries.Num() == 0 &&
      PlayerStateComponent->RedwoodCharacter.HandedOffGrantKeys.Num() == 0 &&
      PlayerStateComponent->RedwoodCharacter.bInventorySeqUnconfirmed &&
      PlayerStateComponent->RedwoodCharacter.UnconfirmedInventorySeq == UnconfirmedBatchSeq;
    LogCurrencyForStaff(
      RedwoodCharacterId,
      bHandOnUnconfirmed
        ? TEXT("its component ended with the batch unconfirmed, and it is handed to the PlayerState for the next component to probe")
        : TEXT("its component ended with the batch unconfirmed"),
      UnconfirmedEntries,
      FirstUnsentEntry,
      UnconfirmedGrantKeys,
      FirstUnsentGrant
    );
    if (bHandOnUnconfirmed) {
      FRedwoodCharacterBackend &Snapshot = PlayerStateComponent->RedwoodCharacter;
      Snapshot.HandedOffCurrencyEntries.Append(UnconfirmedEntries.GetData(), UnconfirmedEntries.Num());
      Snapshot.HandedOffGrantKeys.Append(UnconfirmedGrantKeys.GetData(), UnconfirmedGrantKeys.Num());
      Snapshot.HandedOffUnconfirmedCurrencyCount = FirstUnsentEntry;
      Snapshot.HandedOffUnconfirmedGrantCount = FirstUnsentGrant;
      Snapshot.HandedOffUnconfirmedSeq = UnconfirmedBatchSeq;
    }
  }
  FirstUnsentEntry = FMath::Clamp(FirstUnsentEntry, 0, PendingCurrencyEntries.Num());
  FirstUnsentGrant = FMath::Clamp(FirstUnsentGrant, 0, PendingGrantKeys.Num());
  const TConstArrayView<FRedwoodCurrencyEntry> UnsentEntries =
    MakeArrayView(PendingCurrencyEntries).RightChop(FirstUnsentEntry);
  const TConstArrayView<FString> UnsentGrantKeys =
    MakeArrayView(PendingGrantKeys).RightChop(FirstUnsentGrant);

  if (UnsentEntries.Num() > 0 || UnsentGrantKeys.Num() > 0) {
    if (Unsafe) {
      LogCurrencyForStaff(
        RedwoodCharacterId,
        FString::Printf(TEXT("its component ended and they cannot be handed on: %s"), Unsafe),
        UnsentEntries,
        0,
        UnsentGrantKeys,
        0
      );
    } else {
      // NOT saved yet: logged too, so staff can tell if the next component never takes them.
      LogCurrencyForStaff(
        RedwoodCharacterId,
        TEXT("its component ended, and they are handed to the PlayerState for the next component to send"),
        UnsentEntries,
        0,
        UnsentGrantKeys,
        0
      );
      FRedwoodCharacterBackend &Snapshot = PlayerStateComponent->RedwoodCharacter;
      Snapshot.HandedOffCurrencyEntries.Append(UnsentEntries.GetData(), UnsentEntries.Num());
      Snapshot.HandedOffGrantKeys.Append(UnsentGrantKeys.GetData(), UnsentGrantKeys.Num());
    }
  }

  // Ended: nothing here is this component's to send again, and a second call must not log twice.
  // A detached batch keeps the flight slot taken, so this component can never open a second batch
  // beside it.
  PendingCurrencyEntries.Empty();
  PendingGrantKeys.Empty();
  bCurrencyBatchUnconfirmed = false;
  UnconfirmedCurrencyCount = 0;
  UnconfirmedGrantCount = 0;
  bCurrencyRebaseRequired = false;
}

void URedwoodCharacterComponent::ResetCurrencyForNewCharacter(
  const FString &OldCharacterId, const FString &NewCharacterId
) {
  const FString Why = FString::Printf(TEXT("its component now plays character %s"), *NewCharacterId);
  int32 FirstLoggedEntry = 0;
  int32 FirstLoggedGrant = 0;
  if (bItemFlushInFlight) {
    // The in-flight batch's reply logs its own entries, and writes nothing to the snapshot, which
    // now holds the new character. The slot is free again for the new character's batches.
    FirstLoggedEntry = FMath::Clamp(InFlightCurrencyCount, 0, PendingCurrencyEntries.Num());
    FirstLoggedGrant = FMath::Clamp(InFlightGrantCount, 0, PendingGrantKeys.Num());
    if (URedwoodServerGameSubsystem *ServerSubsystem = GetServerSubsystem()) {
      ServerSubsystem->DetachItemFlushesFromComponent(this);
    }
    bItemFlushInFlight = false;
  }
  const int32 UnknownEntryCount =
    bCurrencyBatchUnconfirmed && FirstLoggedEntry == 0 ? UnconfirmedCurrencyCount : 0;
  const int32 UnknownGrantCount =
    bCurrencyBatchUnconfirmed && FirstLoggedGrant == 0 ? UnconfirmedGrantCount : 0;
  LogCurrencyForStaff(
    OldCharacterId,
    Why,
    MakeArrayView(PendingCurrencyEntries).RightChop(FirstLoggedEntry),
    UnknownEntryCount,
    MakeArrayView(PendingGrantKeys).RightChop(FirstLoggedGrant),
    UnknownGrantCount
  );

  PendingCurrencyEntries.Empty();
  PendingGrantKeys.Empty();
  bCurrencyBatchUnconfirmed = false;
  UnconfirmedCurrencyCount = 0;
  UnconfirmedGrantCount = 0;
  bCurrencyRebaseRequired = false;
  bWriterFenceLost = false;
  bFrozenAfterFinalRefusal = false;
  ClearItemsDirtyState();
}

bool URedwoodCharacterComponent::AdoptHandedOffCurrency(FRedwoodCharacterBackend &Snapshot) {
  if ((Snapshot.HandedOffCurrencyEntries.Num() == 0 && Snapshot.HandedOffGrantKeys.Num() == 0) ||
      Snapshot.Id != RedwoodCharacterId || bItemFlushInFlight || bCurrencyBatchUnconfirmed) {
    return false;
  }
  // FORK(hollowed-oath): the front may be a batch whose outcome is unknown. It is probed at its own
  // seq while that seq is unsettled. Once something else has settled the seq, nothing here can
  // learn whether the batch committed, and sent as new it could pay twice: it is dropped, and
  // logged as UNKNOWN for staff.
  const int32 HandedOffEntryCount =
    FMath::Clamp(Snapshot.HandedOffUnconfirmedCurrencyCount, 0, Snapshot.HandedOffCurrencyEntries.Num());
  const int32 HandedOffGrantCount =
    FMath::Clamp(Snapshot.HandedOffUnconfirmedGrantCount, 0, Snapshot.HandedOffGrantKeys.Num());
  const int64 UnconfirmedSeq = Snapshot.HandedOffUnconfirmedSeq;
  Snapshot.HandedOffUnconfirmedCurrencyCount = 0;
  Snapshot.HandedOffUnconfirmedGrantCount = 0;
  Snapshot.HandedOffUnconfirmedSeq = 0;
  const bool bHasUnconfirmed = HandedOffEntryCount > 0 || HandedOffGrantCount > 0;
  const bool bProbeUnconfirmed = bHasUnconfirmed && Snapshot.bInventorySeqUnconfirmed &&
    Snapshot.UnconfirmedInventorySeq == UnconfirmedSeq && UnconfirmedSeq > LastCommittedItemSeq;
  if (bHasUnconfirmed && !bProbeUnconfirmed) {
    LogCurrencyForStaff(
      RedwoodCharacterId,
      TEXT("its batch's seq was settled before the next component could probe it"),
      MakeArrayView(Snapshot.HandedOffCurrencyEntries).Left(HandedOffEntryCount),
      HandedOffEntryCount,
      MakeArrayView(Snapshot.HandedOffGrantKeys).Left(HandedOffGrantCount),
      HandedOffGrantCount
    );
    Snapshot.HandedOffCurrencyEntries.RemoveAt(0, HandedOffEntryCount);
    Snapshot.HandedOffGrantKeys.RemoveAt(0, HandedOffGrantCount);
    if (Snapshot.HandedOffCurrencyEntries.Num() == 0 && Snapshot.HandedOffGrantKeys.Num() == 0) {
      return false;
    }
  }
  UE_LOG(
    LogRedwood,
    Log,
    TEXT("Character %s takes %d currency entries and %d grant keys a destroyed component handed off; they wait for a reload"),
    *RedwoodCharacterId,
    Snapshot.HandedOffCurrencyEntries.Num(),
    Snapshot.HandedOffGrantKeys.Num()
  );
  // In front: they were staged before anything this component staged.
  PendingCurrencyEntries.Insert(MoveTemp(Snapshot.HandedOffCurrencyEntries), 0);
  PendingGrantKeys.Insert(MoveTemp(Snapshot.HandedOffGrantKeys), 0);
  Snapshot.HandedOffCurrencyEntries.Reset();
  Snapshot.HandedOffGrantKeys.Reset();
  // Set directly: nothing of this component is in flight or unconfirmed (checked above), which is
  // all RequireCurrencyReload's guard is for.
  bCurrencyRebaseRequired = true;
  if (bProbeUnconfirmed) {
    MarkCurrencyBatchUnconfirmed(HandedOffEntryCount, HandedOffGrantCount, UnconfirmedSeq);
  }
  return true;
}

bool URedwoodCharacterComponent::RequireItemSeqProbe(int64 Seq, bool bReloadAfterProbe) {
  if (bItemFlushInFlight || bCurrencyBatchUnconfirmed || Seq <= LastCommittedItemSeq) {
    return false;
  }
  // Set directly, past RequireCurrencyReload's guard: the probe below is what that guard waits
  // for, and GetCurrencyEntriesToSendCount sends the probe before the reload is asked for.
  if (bReloadAfterProbe) {
    bCurrencyRebaseRequired = true;
  }
  MarkCurrencyBatchUnconfirmed(0, 0, Seq);
  return true;
}

void URedwoodCharacterComponent::RequireCurrencyReload(bool bBeforeSettlingReply) {
  if ((bItemFlushInFlight || bCurrencyBatchUnconfirmed) && !bBeforeSettlingReply) {
    return;
  }
  bCurrencyRebaseRequired = true;
}

void URedwoodCharacterComponent::RemoveSentCurrency(int32 SentCurrencyCount, int32 SentGrantCount) {
  // While a batch is out the queues only grow at the back (AdoptHandedOffCurrency adds at the
  // front, but never then), so the sent entries are still the front ones.
  PendingCurrencyEntries.RemoveAt(0, SentCurrencyCount);
  PendingGrantKeys.RemoveAt(0, SentGrantCount);
}

void URedwoodCharacterComponent::MarkCurrencyBatchUnconfirmed(
  int32 SentCurrencyCount, int32 SentGrantCount, int64 SentSeq
) {
  // FORK(hollowed-oath): a batch of item rows alone too (I3). Sent again as a normal batch at its
  // seq, it could meet its own late commit there and be acked as a replay: the success clears the
  // generations it carries, which by then may be newer than the committed ones, or a delete made
  // since, and that newer state is never saved. The probe settles the seq first, with no rows.
  bCurrencyBatchUnconfirmed = true;
  UnconfirmedCurrencyCount = SentCurrencyCount;
  UnconfirmedGrantCount = SentGrantCount;
  UnconfirmedBatchSeq = SentSeq;
}
// FORK(hollowed-oath) END
