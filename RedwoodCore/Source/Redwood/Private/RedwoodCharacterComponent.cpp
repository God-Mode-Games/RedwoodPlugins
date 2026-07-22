// Copyright Incanta Games. All Rights Reserved.

#include "RedwoodCharacterComponent.h"
#include "RedwoodCommonGameSubsystem.h"
#include "RedwoodModule.h"
#include "RedwoodPlayerStateComponent.h"

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
  InFlightBatchSeq = NextBatchSeq;
  OutBatchSeq = NextBatchSeq;
  return true;
}

void URedwoodCharacterComponent::CompleteItemFlush(
  const FString &Error,
  int64 CommittedSeq,
  const TArray<TPair<FString, uint64>> &SentUpserts,
  const TArray<TPair<FString, uint64>> &SentDeletes
) {
  // The seq the just-completed flush stamped on its payload (claimed in TryBeginItemFlush).
  const int64 SentSeq = InFlightBatchSeq;

  if (Error.IsEmpty()) {
    // Success: clear only the ids whose generation still matches what was sent, exactly as the
    // old container ack did -- if MarkItemsDirty/MarkItemsDeleted bumped an id again while the
    // request was in flight, leave it dirty (at its newer generation) so the next flush re-sends
    // the newer record instead of clearing away a write we never actually persisted.
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

    // FORK(hollowed-oath): defensive guard against a malformed SUCCESS ack. We otherwise trust the
    // backend to report committedSeq >= SentSeq on every success response -- that trust is the
    // whole point of the seq fence. But FlushItemsForCharacterComponent's TryGetNumberField parses
    // a missing/malformed "committedSeq" field to 0.0, and adopting that here would reset
    // NextBatchSeq to 1, wedging the fence (every later flush would then stamp a seq the backend
    // has already moved past). If the contract is ever violated like this, do NOT adopt the bogus
    // seq: leave LastCommittedItemSeq/NextBatchSeq exactly as they were. The generations above are
    // still cleared and the flight slot below is still released either way -- the send itself did
    // succeed; only the seq bookkeeping in the ack is suspect.
    if (CommittedSeq < SentSeq) {
      UE_LOG(
        LogRedwood,
        Warning,
        TEXT(
          "CompleteItemFlush: success ack reported committedSeq %lld < sent batchSeq %lld "
          "(contract violation -- missing/malformed committedSeq field?); ignoring the reported "
          "seq and leaving NextBatchSeq at %lld"
        ),
        CommittedSeq,
        SentSeq,
        NextBatchSeq
      );
    } else {
      LastCommittedItemSeq = CommittedSeq;
      NextBatchSeq = CommittedSeq + 1;
    }
  } else {
    // Error: leave dirt in place so the next tick re-sends. Normally we also leave NextBatchSeq
    // alone so the retry re-uses the same seq -- EXCEPT when the backend reports it has already
    // committed at or past the seq we sent (a replay it no-oped, or a fence ahead of us). In that
    // case keeping our old NextBatchSeq would wedge every future flush behind a gap the backend
    // keeps rejecting, so realign to the backend's committed fence.
    if (CommittedSeq >= SentSeq) {
      LastCommittedItemSeq = CommittedSeq;
      NextBatchSeq = CommittedSeq + 1;
    }
  }

  // Always release the flight slot, whatever the outcome.
  bItemFlushInFlight = false;
}

void URedwoodCharacterComponent::SeedItemSeqFromCharacter(int64 InventorySeq) {
  LastCommittedItemSeq = InventorySeq;
  NextBatchSeq = InventorySeq + 1;
}
// FORK(hollowed-oath) END
