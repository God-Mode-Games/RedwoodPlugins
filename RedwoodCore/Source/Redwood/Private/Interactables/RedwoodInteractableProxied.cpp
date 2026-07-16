// Copyright Incanta Games. All Rights Reserved.

#include "Interactables/RedwoodInteractableProxied.h"
#include "RedwoodCharacterComponent.h"

// FORK(hollowed-oath): NetCore PushModel include added for MARK_PROPERTY_DIRTY_FROM_NAME below.
// Upstream trunk replicates ARedwoodProxy::Interactable with legacy always-poll DOREPLIFETIME;
// the fork converted it to push-model (see RedwoodProxy.cpp), so every mutation of Interactable
// must now be paired with a dirty-mark or it stops replicating. Preserve this include alongside
// the push-model branch in RedwoodProxy::GetLifetimeReplicatedProps.
#include "Net/Core/PushModel/PushModel.h"

void ARedwoodInteractableProxied::OnInteract_Implementation(
  APawn *Pawn, URedwoodCharacterComponent *CharacterComponent
) {
  if (IsValid(ProxyClass)) {
    FActorSpawnParameters SpawnParameters;
    SpawnParameters.Owner = this;
    SpawnParameters.Instigator = Pawn;

    AActor *Proxy = GetWorld()->SpawnActor<AActor>(
      ProxyClass, GetActorLocation(), GetActorRotation(), SpawnParameters
    );
    if (IsValid(Proxy)) {
      Proxy->SetOwner(Pawn);

      ARedwoodProxy *RedwoodProxy = Cast<ARedwoodProxy>(Proxy);
      if (IsValid(RedwoodProxy)) {
        RedwoodProxy->Interactable = this;
        // FORK(hollowed-oath): dirty-mark paired with the push-model conversion of
        // ARedwoodProxy::Interactable. Upstream mutates Interactable with no dirty-mark (legacy
        // rep). Keep this call adjacent to the assignment on any upstream merge.
        MARK_PROPERTY_DIRTY_FROM_NAME(ARedwoodProxy, Interactable, RedwoodProxy);
      }
    }
  }
}
