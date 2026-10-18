// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart. The game-server side
// of the currency ledger: entries ride the item flush (realm:characters:items:flush) so a balance
// change commits in the same transaction as the item rows it pays for. See
// URedwoodCharacterComponent::QueueCurrency for the queue and its ack rules.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

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

// Reads a wire amount with the backend's own rule (parseAmount in currency-validator.ts): an
// optional minus, then digits with no leading zero, within int64, and never "-0". A looser reading
// (LexFromString takes " 5", "+5" and "1e3") would let the game server accept a value the backend
// refuses, and an amount the two sides read differently is money that moves on one side only.
inline bool TryParseCurrencyAmount(const FString &Text, int64 &OutAmount) {
  const bool bNegative = Text.StartsWith(TEXT("-"));
  const int32 FirstDigit = bNegative ? 1 : 0;
  const int32 DigitCount = Text.Len() - FirstDigit;
  if (DigitCount <= 0) {
    return false;
  }
  if (Text[FirstDigit] == TEXT('0') && (DigitCount > 1 || bNegative)) {
    return false;
  }
  // Accumulated as a negative number, so MIN_int64 (whose magnitude has no positive int64) fits.
  int64 Negated = 0;
  for (int32 Index = FirstDigit; Index < Text.Len(); ++Index) {
    const TCHAR Character = Text[Index];
    if (Character < TEXT('0') || Character > TEXT('9')) {
      return false;
    }
    const int64 Digit = Character - TEXT('0');
    // Integer division rounds toward zero, which for these negative values is the ceiling: the
    // smallest Negated whose Negated * 10 - Digit still fits.
    if (Negated < (MIN_int64 + Digit) / 10) {
      return false;
    }
    Negated = Negated * 10 - Digit;
  }
  if (!bNegative) {
    if (Negated == MIN_int64) {
      return false;
    }
    Negated = -Negated;
  }
  OutAmount = Negated;
  return true;
}

// The field that carries a character's loaded currency, `{ amount, grants }`: beside "items" on
// the player-auth reply, and inline on an offline character file.
inline const TCHAR *const CurrencyLoadField = TEXT("currency");

// Reads a `{ amount, grants }` object (CurrencyLoadSchema in the backend's interfaces.ts). Returns
// false, with the outputs untouched, unless the amount is a strict amount string of at least zero
// and every grant is a non-empty string. A load that fails this is a failed load, never a zero
// balance: a game server that started from zero would chain BalanceAfter values the backend
// refuses for good.
inline bool TryParseCurrencyLoad(
  const TSharedPtr<FJsonObject> &CurrencyObject, int64 &OutAmount, TArray<FString> &OutGrantKeys
) {
  FString AmountText;
  int64 Amount = 0;
  const TArray<TSharedPtr<FJsonValue>> *GrantValues = nullptr;
  // A string only: TryGetStringField also reads a JSON number, which may have been rounded.
  if (!CurrencyObject.IsValid() || !CurrencyObject->HasTypedField<EJson::String>(TEXT("amount")) ||
      !CurrencyObject->TryGetStringField(TEXT("amount"), AmountText) ||
      !TryParseCurrencyAmount(AmountText, Amount) || Amount < 0 ||
      !CurrencyObject->TryGetArrayField(TEXT("grants"), GrantValues)) {
    return false;
  }
  TArray<FString> GrantKeys;
  GrantKeys.Reserve(GrantValues->Num());
  for (const TSharedPtr<FJsonValue> &GrantValue : *GrantValues) {
    // Checked as a string type, not with TryGetString: that one also turns a number or a bool
    // into text, and the backend schema refuses both.
    if (!GrantValue.IsValid() || GrantValue->Type != EJson::String) {
      return false;
    }
    FString GrantKey = GrantValue->AsString();
    if (GrantKey.IsEmpty()) {
      return false;
    }
    GrantKeys.Add(MoveTemp(GrantKey));
  }
  OutAmount = Amount;
  OutGrantKeys = MoveTemp(GrantKeys);
  return true;
}

// Offline/PIE only: the field in the offline file's currency object that holds the applied entry
// ids (FRedwoodCharacterBackend::OfflineAppliedEntryIds). No backend reply has it.
inline const TCHAR *const OfflineAppliedEntryIdsField = TEXT("appliedEntryIds");

// Grant keys compare case-sensitively, as the backend's key column does. FString's own == and TSet
// ignore case, so "quest:q1" would pass for a used "quest:Q1" that the backend counts as new.
inline bool ContainsGrantKey(const TArray<FString> &GrantKeys, const FString &GrantKey) {
  return GrantKeys.ContainsByPredicate([&GrantKey](const FString &Used) {
    return Used.Equals(GrantKey, ESearchCase::CaseSensitive);
  });
}

// The same `{ amount, grants }` shape, for the offline character file. The amount is a decimal
// string, as on the wire, so an offline file and a backend reply are read by the same rule.
inline TSharedPtr<FJsonObject> MakeCurrencyLoadObject(
  int64 Amount, const TArray<FString> &GrantKeys
) {
  TSharedPtr<FJsonObject> CurrencyObject = MakeShared<FJsonObject>();
  CurrencyObject->SetStringField(TEXT("amount"), LexToString(Amount));
  TArray<TSharedPtr<FJsonValue>> GrantValues;
  GrantValues.Reserve(GrantKeys.Num());
  for (const FString &GrantKey : GrantKeys) {
    GrantValues.Add(MakeShared<FJsonValueString>(GrantKey));
  }
  CurrencyObject->SetArrayField(TEXT("grants"), GrantValues);
  return CurrencyObject;
}

// A flush the backend refused for good (`final: true`). The backend has raised an alert, and the
// character is frozen with everything staged dropped and logged. The game must send the player
// back through character select: only a fresh hydration from the database repairs the state.
// Also fired, with the same freeze, for both characters of a trade whose outcome could not be
// learned (see URedwoodServerGameSubsystem::EmitItemsTrade).
DECLARE_MULTICAST_DELEGATE_TwoParams(
  FOnItemFlushRefusedFinal, const FString & /*CharacterId*/, const FString & /*Error*/
);

// The realm said this session's writer token is stale: another session owns the character, and
// nothing this server stages for it can be saved. The game must end this session for the
// character, so the player does not keep playing on it.
DECLARE_MULTICAST_DELEGATE_OneParam(FOnWriterFenceLost, const FString & /*CharacterId*/);

// A reload gave the backend's balance, and every pending entry is now re-chained on top of it.
// The game's running balance must restart from here: it is the last pending entry's BalanceAfter,
// or Balance itself when nothing is pending. A game that keeps chaining from its old running
// balance writes BalanceAfter values the backend refuses for good, and loses those entries.
DECLARE_MULTICAST_DELEGATE_OneParam(FOnCurrencyRebased, int64 /*Balance*/);

// The alerts a committed flush raised (`alerts` on the ack), for the game to forward to GMs.
DECLARE_MULTICAST_DELEGATE_OneParam(
  FOnCurrencyAlerts, const TArray<TSharedPtr<FJsonObject>> &
);
