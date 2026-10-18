// Copyright Incanta Games. All Rights Reserved.

#include "RedwoodCharacterComponent.h"
#include "RedwoodCommonGameSubsystem.h"
#include "RedwoodModule.h"
#include "RedwoodPlayerStateComponent.h"
#include "RedwoodServerGameSubsystem.h" // FORK(hollowed-oath): the per-character freeze

#include "Engine/GameInstance.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameSession.h"
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
        if (RedwoodCharacterBackend.bCurrencyAmountUnknown) {
          RequireCurrencyReload();
        }
        // FORK(hollowed-oath): a snapshot the freeze marked (a respawn in the same session) starts
        // frozen. A hydration never thaws: only the auth leg's fresh database load does (see
        // URedwoodServerGameSubsystem::ThawCharacter), and the subsystem's per-character freeze
        // covers a snapshot the freeze could not reach.
        bFrozenAfterFinalRefusal = RedwoodCharacterBackend.bFrozenAfterFinalRefusal;
        HydratedItemsCharacterId = IncomingCharacterId;
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
  OutBatchSeq = InFlightBatchSeq;
  return true;
}

void URedwoodCharacterComponent::CompleteItemFlush(
  const FString &Error,
  int64 CommittedSeq,
  const TArray<TPair<FString, uint64>> &SentUpserts,
  const TArray<TPair<FString, uint64>> &SentDeletes,
  int32 SentCurrencyCount,
  int32 SentGrantCount,
  bool bFinalRefusal,
  bool bRebaseRequired,
  bool bNotApplied,
  bool bNotSent
) {
  // The seq the just-completed flush stamped on its payload (claimed in TryBeginItemFlush).
  const int64 SentSeq = InFlightBatchSeq;

  // FORK(hollowed-oath): nothing leaves the queues between the claim and this ack except here, so
  // the counts can only exceed the queues through a caller bug. Clamp rather than over-remove.
  SentCurrencyCount = FMath::Clamp(SentCurrencyCount, 0, PendingCurrencyEntries.Num());
  SentGrantCount = FMath::Clamp(SentGrantCount, 0, PendingGrantKeys.Num());

  if (bNotSent) {
    // FORK(hollowed-oath): the realm never saw this batch. Marking it unconfirmed would turn the
    // next attempt into a probe that carries no rows, for no reason; clearing an earlier unconfirmed
    // state would send that batch's entries as new under a seq that may hold them already. Leave
    // both as they are: the rows stay dirty and the entries stay queued.
  } else if (bNotApplied && CommittedSeq >= SentSeq) {
    // FORK(hollowed-oath): the backend answered a probe: the batch never committed. The backend
    // has CONSUMED that seq with an empty commit (CommittedSeq), so a late original arriving under
    // it now fails instead of applying after the fact. The entries are simply pending again,
    // unchanged: the empty commit moved no balance, so their BalanceAfter still holds. The next
    // claim is a normal batch after that fence, so the payment goes out WITH the current item
    // rows: both save or neither does.
    bCurrencyBatchUnconfirmed = false;
    AdoptCommittedItemSeq(CommittedSeq);
  } else if (bNotApplied) {
    // FORK(hollowed-oath): notApplied without the consumed seq. While the seq is still free, the
    // original attempt can arrive late and commit under it, so a normal batch there could pay the
    // entries twice. Keep the batch unconfirmed: the next attempt is another probe, and no seq
    // below the sent one is ever adopted.
    UE_LOG(
      LogRedwood,
      Error,
      TEXT("CompleteItemFlush: notApplied with committedSeq %lld below the probed seq %lld; probing again"),
      CommittedSeq,
      SentSeq
    );
    MarkCurrencyBatchUnconfirmed(SentCurrencyCount, SentGrantCount, SentSeq);
  } else if (Error.IsEmpty()) {
    // Success: clear only the ids whose generation still matches what was sent, exactly as the
    // old container ack did -- if MarkItemsDirty/MarkItemsDeleted bumped an id again while the
    // request was in flight, leave it dirty (at its newer generation) so the next flush re-sends
    // the newer record instead of clearing away a write we never actually persisted.
    // FORK(hollowed-oath): a malformed success ack leaves the rows DIRTY. Clearing them while
    // refusing to adopt the reported seq (below) was the worst of both worlds: NextBatchSeq stayed
    // where it was, so the NEXT flush re-sent the same batchSeq, the backend no-oped it as a replay
    // of a seq it had already committed, and that no-op's success ack then cleared the NEW
    // mutation -- silently losing it. Retaining the generations makes the replay self-healing
    // instead: the same rows go out again at the same seq, the backend answers with a fence at or
    // past it, and THAT ack (which passes the check below) clears them properly.
    const bool bSeqTrustworthy = CommittedSeq >= SentSeq;
    if (bSeqTrustworthy) {
      ClearSentItemGenerations(SentUpserts, SentDeletes);
      // FORK(hollowed-oath): the backend holds these entries -- committed now, or by the earlier
      // attempt this batch replays (the backend acks a replay that carries currency only when
      // every entry id is already in its ledger). Entries queued after the claim sit behind them
      // and stay.
      RemoveSentCurrency(SentCurrencyCount, SentGrantCount);
      bCurrencyBatchUnconfirmed = false;
    }

    // FORK(hollowed-oath): defensive guard against a malformed SUCCESS ack. We otherwise trust the
    // backend to report committedSeq >= SentSeq on every success response -- that trust is the
    // whole point of the seq fence. But FlushItemsForCharacterComponent's TryGetNumberField parses
    // a missing/malformed "committedSeq" field to 0.0, and adopting that here would reset
    // NextBatchSeq to 1, wedging the fence (every later flush would then stamp a seq the backend
    // has already moved past). If the contract is ever violated like this, do NOT adopt the bogus
    // seq: leave LastCommittedItemSeq/NextBatchSeq exactly as they were. The generations above are
    // still cleared and the flight slot below is still released either way -- the send itself did
    // succeed; only the seq bookkeeping in the ack is suspect.
    if (!bSeqTrustworthy) {
      UE_LOG(
        LogRedwood,
        Warning,
        TEXT(
          "CompleteItemFlush: success ack reported committedSeq %lld < sent batchSeq %lld "
          "(contract violation -- missing/malformed committedSeq field?); ignoring the reported "
          "seq, KEEPING the sent rows dirty for a self-healing replay, and leaving NextBatchSeq "
          "at %lld"
        ),
        CommittedSeq,
        SentSeq,
        NextBatchSeq
      );
      // FORK(hollowed-oath): the same self-healing replay for currency -- keep the batch exactly as
      // sent, under its seq, until an ack can be trusted.
      MarkCurrencyBatchUnconfirmed(SentCurrencyCount, SentGrantCount, SentSeq);
    } else {
      AdoptCommittedItemSeq(CommittedSeq);
    }
  } else if (bFinalRefusal) {
    // FORK(hollowed-oath): the backend refused this batch for good, rolled ALL of it back, and
    // raised an alert. The game's state and the backend's now disagree, and a later staged change
    // may depend on the refused one, so nothing staged can be trusted: freeze (see
    // FreezeAfterFinalRefusal).
    FreezeAfterFinalRefusal(Error);
    if (CommittedSeq >= SentSeq) {
      AdoptCommittedItemSeq(CommittedSeq);
    }
  } else {
    // Error: leave dirt in place so the next tick re-sends. Normally we also leave NextBatchSeq
    // alone so the retry re-uses the same seq -- EXCEPT when the backend reports it has already
    // committed at or past the seq we sent (a replay it no-oped, or a fence ahead of us). In that
    // case keeping our old NextBatchSeq would wedge every future flush behind a gap the backend
    // keeps rejecting, so realign to the backend's committed fence.
    if (CommittedSeq >= SentSeq) {
      AdoptCommittedItemSeq(CommittedSeq);
    }
    if (bRebaseRequired) {
      // FORK(hollowed-oath): only the backend's replay-mismatch refusal sets rebaseRequired: a trade
      // used this seq, and the backend checked that the batch's entry ids and keys are NOT all in
      // its ledger. Exactly one batch is re-sent per seq (see the staging block in the header), so
      // none of it applied. The entries are kept -- an error reply means not saved. The trade moved
      // the balance, so every pending BalanceAfter is stale and would be refused as final. Hold
      // currency until the caller reloads; the flag is the signal for the flush path (B2) to start
      // that reload and then call RebaseCurrency.
      bCurrencyBatchUnconfirmed = false;
      bCurrencyRebaseRequired = true;
    } else if (CommittedSeq >= 0 && CommittedSeq < SentSeq - 1) {
      // FORK(hollowed-oath): a sequence gap. The backend is behind the seq we sent, so this batch
      // (and any earlier attempt of it under the same seq) cannot have committed. Re-sending the
      // gapped seq would be refused forever; realign to the backend's fence, as the success path
      // does, and send the kept entries as an ordinary batch there. Negative means the reply had no
      // fence at all (a dropped socket), which is not a gap.
      LastCommittedItemSeq = CommittedSeq;
      NextBatchSeq = CommittedSeq + 1;
      bCurrencyBatchUnconfirmed = false;
    } else {
      // FORK(hollowed-oath): any other error leaves the outcome unknown, WHATEVER committedSeq says.
      // A "concurrent flush" refusal of a retry that raced its own still-running original reports
      // the original's commit as the fence; treating that as "did not apply" and rebasing would
      // re-chain the committed entry on top of itself, and its re-send would be refused as a
      // duplicate. Re-send the identical batch under its seq instead: the backend acks it from its
      // ledger if it committed.
      MarkCurrencyBatchUnconfirmed(SentCurrencyCount, SentGrantCount, SentSeq);
    }
  }

  // Always release the flight slot, whatever the outcome.
  bItemFlushInFlight = false;

  // FORK(hollowed-oath): after the state above is settled, so a listener that reacts (by returning
  // the player to character select) sees the component frozen and empty.
  if (bFinalRefusal && !Error.IsEmpty()) {
    OnItemFlushRefusedFinal.Broadcast(RedwoodCharacterId, Error);
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

bool URedwoodCharacterComponent::HasGrantKey(const FString &GrantKey) const {
  if (ContainsGrantKey(PendingGrantKeys, GrantKey)) {
    return true;
  }
  const URedwoodPlayerStateComponent *PlayerStateComponent = FindPlayerStateComponent();
  return PlayerStateComponent &&
    ContainsGrantKey(PlayerStateComponent->RedwoodCharacter.GrantKeys, GrantKey);
}

bool URedwoodCharacterComponent::QueueCurrency(
  const TArray<FRedwoodCurrencyEntry> &Entries, const TArray<FString> &GrantKeys
) {
  // Frozen, per component (this one was refused) or per character (another component of it was):
  // an amount staged now would be chained from a balance nobody can vouch for. A lost writer fence
  // is refused the same way: this component never sends again, and an entry accepted here would be
  // money the game believes saved.
  UGameInstance *GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
  const URedwoodServerGameSubsystem *ServerSubsystem =
    GameInstance ? GameInstance->GetSubsystem<URedwoodServerGameSubsystem>() : nullptr;
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
  int64 TradeFloorRunning = 0;
  const bool bTradeFloor = ServerSubsystem &&
    ServerSubsystem->GetUnsettledTradeFloorBalance(RedwoodCharacterId, TradeFloorRunning);
  auto AddToTradeFloor = [&TradeFloorRunning](int64 Delta) {
    // Saturated: an overflowing chain is refused by the re-chain itself.
    TradeFloorRunning = Delta > 0 && TradeFloorRunning > MAX_int64 - Delta
      ? MAX_int64
      : TradeFloorRunning + Delta;
  };
  if (bTradeFloor) {
    for (const FRedwoodCurrencyEntry &Pending : PendingCurrencyEntries) {
      AddToTradeFloor(Pending.Delta);
    }
  }

  // Appended, never merged or de-duplicated: the backend checks each BalanceAfter in order, and a
  // repeated grant key must reach the backend so it refuses the second reward instead of the game
  // server quietly paying it without its guard.
  bool bAllQueued = true;
  for (const FRedwoodCurrencyEntry &Entry : Entries) {
    // A bad id would make the backend refuse the whole batch for good, taking every valid entry
    // and item row sent with it. Refuse the one entry here instead, loudly: it is a caller bug.
    // An Error log, not an ensure: the caller learns it from the return value.
    if (!IsValidCurrencyEntryId(Entry.Id)) {
      UE_LOG(
        LogRedwood,
        Error,
        TEXT("QueueCurrency: refused entry id '%s' (delta %lld): not 32 uppercase hex digits"),
        *Entry.Id,
        Entry.Delta
      );
      bAllQueued = false;
      continue;
    }
    if (bTradeFloor && Entry.Delta < 0 && TradeFloorRunning + Entry.Delta < 0) {
      UE_LOG(
        LogRedwood,
        Error,
        TEXT("QueueCurrency: character %s has a trade unsettled and could hold only %lld after it; refused entry %s delta %lld"),
        *RedwoodCharacterId,
        TradeFloorRunning,
        *Entry.Id,
        Entry.Delta
      );
      bAllQueued = false;
      continue;
    }
    if (bTradeFloor) {
      AddToTradeFloor(Entry.Delta);
    }
    PendingCurrencyEntries.Add(Entry);
  }
  PendingGrantKeys.Append(GrantKeys);
  return bAllQueued;
}

int32 URedwoodCharacterComponent::GetCurrencyEntriesToSendCount() const {
  if (bCurrencyRebaseRequired) {
    return 0;
  }
  return bCurrencyBatchUnconfirmed ? UnconfirmedCurrencyCount : PendingCurrencyEntries.Num();
}

int32 URedwoodCharacterComponent::GetGrantKeysToSendCount() const {
  if (bCurrencyRebaseRequired) {
    return 0;
  }
  return bCurrencyBatchUnconfirmed ? UnconfirmedGrantCount : PendingGrantKeys.Num();
}

void URedwoodCharacterComponent::RebaseCurrency(int64 Balance) {
  if (!bCurrencyRebaseRequired) {
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT("RebaseCurrency: no rebase is required; ignoring the reloaded balance %lld"),
      Balance
    );
    return;
  }

  // On an overflow the rebase stays pending: the queue is held, not dropped, until a reload yields
  // a balance that fits.
  if (TryRechainPendingCurrency(Balance)) {
    bCurrencyRebaseRequired = false;
  }
}

void URedwoodCharacterComponent::RechainCurrencyAfterTrade(int64 Balance) {
  // A batch whose reply is still open may or may not be in Balance; re-chaining its entries could
  // count them twice. The trade's hold makes this unreachable, so say so loudly and let a reload
  // decide once that reply is in.
  if (bItemFlushInFlight || bCurrencyBatchUnconfirmed) {
    UE_LOG(
      LogRedwood,
      Error,
      TEXT("RechainCurrencyAfterTrade: character %s has a batch in flight or unconfirmed; waiting for a reload instead"),
      *RedwoodCharacterId
    );
    RequireCurrencyReload(true);
    return;
  }
  if (!TryRechainPendingCurrency(Balance)) {
    bCurrencyRebaseRequired = true;
    return;
  }
  OnCurrencyRebased.Broadcast(Balance);
}

bool URedwoodCharacterComponent::TryRechainPendingCurrency(int64 Balance) {
  // Checked before anything is written: a batch that mixes stale and re-chained values is refused
  // for good. So is a chain below zero, and a final refusal freezes the character: keep such a
  // chain out of every batch instead. The caller holds the queue for a reload, and the reload's
  // give-up logs what stays unsaved.
  int64 Running = Balance;
  for (const FRedwoodCurrencyEntry &Entry : PendingCurrencyEntries) {
    const bool bOverflows = Entry.Delta > 0 ? Running > MAX_int64 - Entry.Delta
                                            : Running < MIN_int64 - Entry.Delta;
    if (bOverflows) {
      UE_LOG(
        LogRedwood,
        Error,
        TEXT("RebaseCurrency: entry %s overflows int64 from balance %lld; currency stays held"),
        *Entry.Id,
        Running
      );
      return false;
    }
    Running += Entry.Delta;
    if (Running < 0) {
      UE_LOG(
        LogRedwood,
        Error,
        TEXT("Re-chaining currency: entry %s (delta %lld) of character %s takes the balance from %lld to %lld, below zero; currency stays held"),
        *Entry.Id,
        Entry.Delta,
        *RedwoodCharacterId,
        Balance,
        Running
      );
      return false;
    }
  }
  Running = Balance;
  for (FRedwoodCurrencyEntry &Entry : PendingCurrencyEntries) {
    Running += Entry.Delta;
    Entry.BalanceAfter = Running;
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
  LogStagedCurrency(
    FString::Printf(TEXT("Writer fence lost: character %s left unsent"), *RedwoodCharacterId)
  );
  OnWriterFenceLost.Broadcast(RedwoodCharacterId);
}

void URedwoodCharacterComponent::LogStagedCurrency(const FString &Lead) const {
  // The backend has the refusal alert; these lines are what staff rebuild the rest from. The front
  // of an unconfirmed batch may have committed (its ack was lost), so it is told apart: re-granting
  // a saved amount would pay it twice. A re-grant reuses the logged entry id either way, so the
  // backend's duplicate-id check refuses a copy.
  const int32 UnknownEntryCount = bCurrencyBatchUnconfirmed ? UnconfirmedCurrencyCount : 0;
  const int32 UnknownGrantCount = bCurrencyBatchUnconfirmed ? UnconfirmedGrantCount : 0;
  for (int32 Index = 0; Index < PendingCurrencyEntries.Num(); ++Index) {
    const FRedwoodCurrencyEntry &Entry = PendingCurrencyEntries[Index];
    UE_LOG(
      LogRedwood,
      Error,
      TEXT("%s currency entry %s delta %lld reason %s, %s. A re-grant must reuse entry id %s, so the backend refuses a second copy."),
      *Lead,
      *Entry.Id,
      Entry.Delta,
      *Entry.Reason,
      Index < UnknownEntryCount ? TEXT("outcome UNKNOWN (it may be saved)") : TEXT("NOT saved"),
      *Entry.Id
    );
  }
  for (int32 Index = 0; Index < PendingGrantKeys.Num(); ++Index) {
    UE_LOG(
      LogRedwood,
      Error,
      TEXT("%s grant %s, %s"),
      *Lead,
      *PendingGrantKeys[Index],
      Index < UnknownGrantCount ? TEXT("outcome UNKNOWN (it may be saved)") : TEXT("NOT saved")
    );
  }
}

void URedwoodCharacterComponent::FreezeAfterFinalRefusal(const FString &Reason) {
  LogStagedCurrency(FString::Printf(
    TEXT("Final refusal (%s): character %s dropped"), *Reason, *RedwoodCharacterId
  ));
  PendingCurrencyEntries.Empty();
  PendingGrantKeys.Empty();
  bCurrencyBatchUnconfirmed = false;
  UnconfirmedCurrencyCount = 0;
  UnconfirmedGrantCount = 0;
  bCurrencyRebaseRequired = false;
  DirtyItemGenerations.Empty();
  PendingDeletedItemGenerations.Empty();
  bFrozenAfterFinalRefusal = true;
}

void URedwoodCharacterComponent::RequireCurrencyReload(bool bBeforeSettlingReply) {
  if ((bItemFlushInFlight || bCurrencyBatchUnconfirmed) && !bBeforeSettlingReply) {
    return;
  }
  bCurrencyRebaseRequired = true;
}

void URedwoodCharacterComponent::RemoveSentCurrency(int32 SentCurrencyCount, int32 SentGrantCount) {
  // The queues only grow at the back, so the sent entries are still the front ones.
  PendingCurrencyEntries.RemoveAt(0, SentCurrencyCount);
  PendingGrantKeys.RemoveAt(0, SentGrantCount);
}

void URedwoodCharacterComponent::MarkCurrencyBatchUnconfirmed(
  int32 SentCurrencyCount, int32 SentGrantCount, int64 SentSeq
) {
  if (SentCurrencyCount == 0 && SentGrantCount == 0) {
    return;
  }
  bCurrencyBatchUnconfirmed = true;
  UnconfirmedCurrencyCount = SentCurrencyCount;
  UnconfirmedGrantCount = SentGrantCount;
  UnconfirmedBatchSeq = SentSeq;
}
// FORK(hollowed-oath) END
