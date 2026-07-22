// Copyright Incanta Games. All Rights Reserved.

#include "Interactables/RedwoodProxy.h"

// FORK(hollowed-oath): NetCore PushModel include for the push-model rep branch below.
#include "Net/Core/PushModel/PushModel.h"
#include "Net/UnrealNetwork.h"

ARedwoodProxy::ARedwoodProxy() {
  bReplicates = true;
  bNetUseOwnerRelevancy = false;
  bOnlyRelevantToOwner = true;
}

void ARedwoodProxy::GetLifetimeReplicatedProps(
  TArray<FLifetimeProperty> &OutLifetimeProps
) const {
  Super::GetLifetimeReplicatedProps(OutLifetimeProps);

  // FORK(hollowed-oath) BEGIN: push-model replication for Interactable. Upstream trunk is a bare
  // DOREPLIFETIME(ARedwoodProxy, Interactable). The fork converts it to push-model so its
  // mutations must be dirty-marked (see RedwoodInteractableProxied.cpp's
  // MARK_PROPERTY_DIRTY_FROM_NAME). Preserve BOTH legs: the push-model branch and the legacy
  // else-branch fallback for builds where push model is disabled.
#if WITH_PUSH_MODEL
  if (IS_PUSH_MODEL_ENABLED()) {
    FDoRepLifetimeParams Params;
    Params.bIsPushBased = true;
    DOREPLIFETIME_WITH_PARAMS_FAST(ARedwoodProxy, Interactable, Params);
  } else
#endif
  {
    DOREPLIFETIME(ARedwoodProxy, Interactable);
  }
  // FORK(hollowed-oath) END
}
