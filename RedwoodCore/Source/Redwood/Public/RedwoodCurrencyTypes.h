// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart. The game-server side
// of the currency ledger: entries ride the item flush (realm:characters:items:flush) so a balance
// change commits in the same transaction as the item rows it pays for. See
// URedwoodCharacterComponent::QueueCurrency for the queue and its ack rules.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

#include "RedwoodCurrencyTypes.generated.h"

// One currency ledger entry, as the game server stages it. Amounts are int64 currency units here
// and decimal strings on the wire. The backend recomputes every balance itself and refuses the
// whole batch as final when BalanceAfter is not what it computed, so BalanceAfter must be chained
// from the balance the backend holds (see URedwoodCharacterComponent::RebaseCurrency).
USTRUCT()
struct REDWOOD_API FRedwoodCurrencyEntry {
  GENERATED_BODY()

  // 32 uppercase hex digits (FGuid::ToString(EGuidFormats::Digits)). The backend uses the id to
  // tell a replayed batch from a new one, so a resend must keep it.
  UPROPERTY()
  FString Id;

  UPROPERTY()
  int64 Delta = 0;

  UPROPERTY()
  int64 BalanceAfter = 0;

  // A tag under Currency.Reason.
  UPROPERTY()
  FString Reason;

  // Optional. Empty means "omit from the wire", never null.
  UPROPERTY()
  FString SourceRef;

  // Optional, for GM grants. Empty means "omit from the wire", never null.
  UPROPERTY()
  FString ActorAccountId;
};

// The backend refuses any other id shape for good, so the game server refuses it at the queue.
constexpr int32 CurrencyEntryIdLength = 32;

// A new entry id in the shape the backend checks: 32 uppercase hex digits.
inline FString MakeCurrencyEntryId() {
  return FGuid::NewGuid().ToString(EGuidFormats::Digits).ToUpper();
}

inline bool IsValidCurrencyEntryId(const FString &Id) {
  if (Id.Len() != CurrencyEntryIdLength) {
    return false;
  }
  for (const TCHAR Character : Id) {
    const bool bIsDigit = Character >= TEXT('0') && Character <= TEXT('9');
    const bool bIsUpperHex = Character >= TEXT('A') && Character <= TEXT('F');
    if (!bIsDigit && !bIsUpperHex) {
      return false;
    }
  }
  return true;
}

// A flush the backend refused for good (`final: true`). The sent entries are gone and the backend
// has raised an alert; the character must reload before its balance can be trusted again.
DECLARE_MULTICAST_DELEGATE_OneParam(FOnItemFlushRefusedFinal, const FString & /*Error*/);

// The alerts a committed flush raised (`alerts` on the ack), for the game to forward to GMs.
DECLARE_MULTICAST_DELEGATE_OneParam(
  FOnCurrencyAlerts, const TArray<TSharedPtr<FJsonObject>> &
);
