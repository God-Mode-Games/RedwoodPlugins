// Copyright Incanta LLC. All rights reserved.

#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"

#include "Types/RedwoodTypes.h"

#include "RedwoodPlayerStateComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnRedwoodPlayerStateUpdated);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnRedwoodPlayerTransferring);

// FORK(hollowed-oath): transfer-abort notifications, plus the server-side
// counterparts of the two client events. See AbortTransferring below.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
  FOnRedwoodPlayerTransferAborted, FString, Error
);
DECLARE_MULTICAST_DELEGATE(FOnRedwoodPlayerTransferringServer);
DECLARE_MULTICAST_DELEGATE_OneParam(
  FOnRedwoodPlayerTransferAbortedServer, const FString & /* Error */
);

UCLASS(
  BlueprintType,
  Blueprintable,
  ClassGroup = (Redwood),
  meta = (BlueprintSpawnableComponent)
)
class REDWOOD_API URedwoodPlayerStateComponent : public UActorComponent {
  GENERATED_BODY()

public:
  URedwoodPlayerStateComponent(const FObjectInitializer &ObjectInitializer);

  // NOT AVAILABLE ON CLIENTS
  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FRedwoodPlayerData RedwoodPlayer;
  // NOT AVAILABLE ON CLIENTS
  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FRedwoodCharacterBackend RedwoodCharacter;

  UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Redwood")
  void MarkCharacterDataDirty() {
    bCharacterDataDirty = true;
  }

  UFUNCTION(BlueprintCallable, Category = "Redwood")
  bool IsCharacterDataDirty() const {
    return bCharacterDataDirty;
  }

  //~ Begin UActorComponent Interface
  virtual void TickComponent(
    float DeltaTime,
    enum ELevelTick TickType,
    FActorComponentTickFunction *ThisTickFunction
  ) override;
  virtual void GetLifetimeReplicatedProps(
    TArray<FLifetimeProperty> &OutLifetimeProps
  ) const override;
  //~ End UActorComponent Interface

  UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Redwood")
  bool bFollowPawn = false;

  UPROPERTY(BlueprintReadOnly, Category = "Redwood")
  bool bClientReady = false;

  UPROPERTY(BlueprintReadOnly, Category = "Redwood")
  bool bServerReady = false;

  UFUNCTION(
    BlueprintCallable,
    Server,
    Reliable,
    WithValidation,
    Category = "Redwood|PlayerState"
  )
  void SetClientReady();

  UFUNCTION(BlueprintCallable, Category = "Redwood")
  void SetServerReady();

  UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Redwood")
  void SetRedwoodPlayer(FRedwoodPlayerData InRedwoodPlayer);

  UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Redwood")
  void SetRedwoodCharacter(FRedwoodCharacterBackend InRedwoodCharacter);

  // The id of the party this player is in; empty if not in a party.
  // Set on the server when the player logs in and replicated to all
  // clients. It does NOT automatically update if the player's party
  // changes while they're in this server; the server can refresh it
  // via URedwoodServerGameSubsystem::GetPartyByPlayerId + SetPartyId.
  UPROPERTY(
    BlueprintReadOnly, ReplicatedUsing = OnRep_PartyId, Category = "Redwood"
  )
  FString PartyId;

  UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Redwood")
  void SetPartyId(const FString &InPartyId);

  /**
   * Returns the URedwoodPlayerStateComponents of all players in this
   * player's party, based on matching replicated PartyId values; usable
   * on both the server and clients. This component is always included
   * when bExcludeSelf is false, even if the player isn't in a party.
   */
  UFUNCTION(BlueprintCallable, Category = "Redwood")
  TArray<URedwoodPlayerStateComponent *> GetPartyMemberPlayerStateComponents(
    bool bExcludeSelf = false
  ) const;

  UFUNCTION(BlueprintCallable, Category = "Redwood")
  bool GetSpawnData(FTransform &Transform, FRotator &ControlRotation);

  UPROPERTY(BlueprintAssignable, Category = "Events")
  FOnRedwoodPlayerStateUpdated OnRedwoodCharacterUpdated;

  UPROPERTY(BlueprintAssignable, Category = "Events")
  FOnRedwoodPlayerStateUpdated OnRedwoodPlayerUpdated;

  // Broadcast on the server and on all clients when PartyId changes.
  UPROPERTY(BlueprintAssignable, Category = "Events")
  FOnRedwoodPlayerStateUpdated OnPartyIdChanged;

  bool bTransferring = false;

  /**
   * Server-only entry point that begins a zone transfer for this player.
   * Marks the component as transferring and notifies the owning client
   * via Client_OnTransferring, which broadcasts OnTransferring locally.
   * Called from URedwoodServerGameSubsystem's TravelPlayerToZone* paths
   * in place of setting bTransferring directly.
   * FORK(hollowed-oath): also broadcasts OnTransferringStartedServer.
   */
  void InitTransferring();

  /**
   * Reliable RPC delivered only to this PlayerState's owning client.
   * Broadcasts OnTransferring so C++/Blueprint listeners on the owning
   * client can react (e.g. show a loading screen) before the travel.
   */
  UFUNCTION(Client, Reliable, Category = "Redwood|PlayerState")
  void Client_OnTransferring();

  // Broadcast on the owning client when a zone transfer begins. Bind in
  // C++ or Blueprints. Only fires on the owning client (see
  // Client_OnTransferring); it does not fire on the server or other
  // clients.
  UPROPERTY(BlueprintAssignable, Category = "Events")
  FOnRedwoodPlayerTransferring OnTransferring;

  /**
   * FORK(hollowed-oath): Reliable RPC delivered only to this PlayerState's
   * owning client. Broadcasts OnTransferAborted so the client can remove
   * the loading screen that Client_OnTransferring put up.
   */
  UFUNCTION(Client, Reliable, Category = "Redwood|PlayerState")
  void Client_OnTransferAborted(const FString &Error);

  // FORK(hollowed-oath): Broadcast on the owning client when a zone
  // transfer stops before it completes. Only fires on the owning client
  // (see Client_OnTransferAborted); it does not fire on the server or
  // other clients.
  UPROPERTY(BlueprintAssignable, Category = "Events")
  FOnRedwoodPlayerTransferAborted OnTransferAborted;

  // FORK(hollowed-oath): server-side counterparts of OnTransferring and
  // OnTransferAborted. The two events above only fire on the owning
  // client, but game C++ on the server must also know when a transfer
  // starts and when it stops. Native (not dynamic) because they are
  // server-only and not for Blueprints; bind them per component.
  FOnRedwoodPlayerTransferringServer OnTransferringStartedServer;
  FOnRedwoodPlayerTransferAbortedServer OnTransferAbortedServer;

  // FORK(hollowed-oath): rollback for a transfer that does not complete.
  // InitTransferring runs BEFORE the TravelPlayerToZone* functions test the
  // sidecar, and upstream has no path that clears the flag when that test
  // fails — the player stays marked as transferring for the rest of the
  // session, which also disables linkdead pawn retention and lastLocation
  // persistence in the game project. Called from three places:
  //   1. the sidecar-down early returns, where the request never left this
  //      server;
  //   2. a sidecar error response that the sidecar marks "ambiguous": false,
  //      which proves the realm never started the transfer;
  //   3. the realm's transfer-failed event, which reports a transfer that
  //      the realm accepted but could not complete.
  // Both of those callers first make sure the player is still transferring.
  // An AMBIGUOUS sidecar error (the sidecar lost contact with the realm), or
  // an answer with no "ambiguous" field at all (an older sidecar), keeps the
  // flag and kicks the player, because the realm can have started the
  // transfer and a retained body could duplicate a character.
  // Unlike the earlier fork state, this now also tells the player: the
  // server gets OnTransferAbortedServer and the owning client gets
  // OnTransferAborted, so the game can remove the loading screen.
  // An upstream merge must keep AbortTransferring on all three paths.

  /**
   * FORK(hollowed-oath): Server-only. Clears bTransferring, broadcasts
   * OnTransferAbortedServer, and tells the owning client with
   * Client_OnTransferAborted. Error tells why the transfer stopped.
   * Does nothing when the player is not transferring, so one start can
   * never produce two aborts. The callers gate too, only for better logs.
   */
  void AbortTransferring(const FString &Error);

  void ClearDirtyFlags() {
    bCharacterDataDirty = false;
  }

  bool bRanPostLogin = false;

private:
  UFUNCTION()
  void OnRep_PartyId();

  TWeakObjectPtr<APlayerState> OwnerPlayerState = nullptr;

  bool bCharacterDataDirty = false;
};
