// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): file is fork-added, for the client interface tests.
// URedwoodClientInterface names this class a friend, so a test can read the
// private state that has no public surface: what is held, what waits for a
// reply, and which session step the client thinks it is in. Read only; a
// test drives the client through its public calls and the fake Bifrost.

#pragma once

#include "CoreMinimal.h"

#include "RedwoodBifrostConnection.h"
#include "RedwoodClientInterface.h"

class FRedwoodClientTestAccess {
public:
  static int32 HeldRequestCount(const URedwoodClientInterface &Client) {
    return Client.HeldRequests.Num();
  }

  static int32 PendingReplyCount(const URedwoodClientInterface &Client) {
    return Client.Replies.Num();
  }

  static bool IsReauthPending(const URedwoodClientInterface &Client) {
    return Client.bReauthPending;
  }

  static bool IsRealmEntered(const URedwoodClientInterface &Client) {
    return Client.bRealmEntered;
  }

  static const FString &SelectedCharacterId(
    const URedwoodClientInterface &Client
  ) {
    return Client.SelectedCharacterId;
  }

  static const FString &CurrentRealmId(const URedwoodClientInterface &Client) {
    return Client.CurrentRealmId;
  }

  static TSharedPtr<FRedwoodBifrostConnection> Connection(
    const URedwoodClientInterface &Client
  ) {
    return Client.Connection;
  }

  // The regions the realm's start answer carried, by name, with the address
  // each one is pinged at. GetRegions() holds only the ping results, which
  // need a network.
  static TMap<FString, FString> RegionPingUrls(
    const URedwoodClientInterface &Client
  ) {
    TMap<FString, FString> Urls;
    for (const TPair<FString, TSharedPtr<FRedwoodRegionLatency>> &Region :
         Client.Regions) {
      if (Region.Value.IsValid()) {
        Urls.Add(Region.Key, Region.Value->Url);
      }
    }
    return Urls;
  }
};
