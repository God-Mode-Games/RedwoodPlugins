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

  // The session is coming back after a drop: requests are held.
  static bool IsReauthPending(const URedwoodClientInterface &Client) {
    return Client.Session.IsComingBack();
  }

  static bool IsRealmEntered(const URedwoodClientInterface &Client) {
    return Client.Session.IsRealmEntered();
  }

  static const FString &SelectedCharacterId(
    const URedwoodClientInterface &Client
  ) {
    return Client.Session.GetSelectedCharacterId();
  }

  static FString CurrentRealmId(const URedwoodClientInterface &Client) {
    return Client.Session.GetRealmId();
  }

  static ERedwoodSessionPhase Phase(const URedwoodClientInterface &Client) {
    return Client.Session.GetPhase();
  }

  static TSharedPtr<FRedwoodBifrostConnection> Connection(
    const URedwoodClientInterface &Client
  ) {
    return Client.Connection;
  }

  // The ticketing cluster's flags (HollowedOath#2886, #3002): what a zone
  // assignment is judged by, and what is owed to the realm.
  static bool IsAssignmentExpected(const URedwoodClientInterface &Client) {
    return Client.bAssignmentExpected;
  }

  static bool IsQueueJoinAbandoned(const URedwoodClientInterface &Client) {
    return Client.bAbandonedQueueJoin;
  }

  static bool IsJoinAwaitingReply(const URedwoodClientInterface &Client) {
    return Client.bJoinAwaitingReply;
  }

  static bool HasHeldAssignment(const URedwoodClientInterface &Client) {
    return Client.HeldAssignment.IsValid();
  }

  static const FString &OwnTicketId(const URedwoodClientInterface &Client) {
    return Client.OwnTicketId;
  }

  static bool IsInWorld(const URedwoodClientInterface &Client) {
    return Client.bInWorld;
  }

  static bool IsLeaveTicketingOwed(const URedwoodClientInterface &Client) {
    return Client.bLeaveTicketingOwed;
  }

  static bool IsTravelPending(const URedwoodClientInterface &Client) {
    return Client.bTravelPending;
  }

  static const FString &AcceptedPartyTicketId(
    const URedwoodClientInterface &Client
  ) {
    return Client.AcceptedPartyTicketId;
  }

  static const FString &PendingPartyTicketId(
    const URedwoodClientInterface &Client
  ) {
    return Client.PendingPartyTicketId;
  }

  static uint32 JoinSequence(const URedwoodClientInterface &Client) {
    return Client.JoinSequence;
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
