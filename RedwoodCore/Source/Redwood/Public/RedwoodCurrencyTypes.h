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

// The balance of each currency, keyed by the game's currency name (a gameplay tag name such as
// "Currency.Gold"; the plugin never reads it). A currency with no key holds 0.
using FRedwoodCurrencyBalances = TMap<FString, int64>;

inline int64 GetCurrencyBalance(const FRedwoodCurrencyBalances &Balances, const FString &Currency) {
  const int64 *Balance = Balances.Find(Currency);
  return Balance ? *Balance : 0;
}

// One currency ledger entry, as the game server stages it. Amounts are int64 currency units here
// and decimal strings on the wire. The backend recomputes every balance itself and refuses the
// whole batch as final when BalanceAfter is not what it computed, so BalanceAfter must be chained
// from the balance of the entry's own currency that the backend holds (see
// URedwoodCharacterComponent::RebaseCurrency).
USTRUCT()
struct REDWOOD_API FRedwoodCurrencyEntry {
  GENERATED_BODY()

  // 32 uppercase hex digits (FGuid::ToString(EGuidFormats::Digits)). The backend uses the id to
  // tell a replayed batch from a new one, so a resend must keep it.
  UPROPERTY()
  FString Id;

  // The currency this entry changes. Every currency is its own chain: BalanceAfter is the balance
  // of this currency only. The backend refuses an entry without one for good.
  UPROPERTY()
  FString Currency;

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

// The fields of a realm:characters:items:flush reply. An error reply means the batch was NOT
// saved, whatever CommittedSeq says.
struct FRedwoodItemFlushReply {
  FString Error;
  // -1 when the reply carried no fence (a dropped socket, a timeout).
  int64 CommittedSeq = -1;
  bool bFinal = false;
  bool bRebaseRequired = false;
  // The backend's answer to a probe whose seq never committed: nothing was written.
  bool bNotApplied = false;
  // The writer token this session sent is stale: another session owns the character.
  bool bWriterFenceLost = false;
  // Set by the sidecar: it sent the batch and no answer came, so the batch MAY have applied.
  bool bTimedOut = false;
  // Set by the sidecar: it had no realm to send the batch to, so the batch did NOT apply.
  bool bNotForwarded = false;
  // Set by the realm: it could not tell whether its own commit went through. Read like bTimedOut.
  bool bCommitUnknown = false;
  TArray<TSharedPtr<FJsonObject>> Alerts;
};

// What one flush claimed and sent, kept for its reply.
struct FRedwoodItemFlushSent {
  int64 BatchSeq = 0;
  // A re-send of an unconfirmed batch. Only a reply that settles its seq tells anything about the
  // first attempt; a refused or unsent probe leaves that outcome unknown.
  bool bProbe = false;
  TArray<TPair<FString, uint64>> Upserts;
  TArray<TPair<FString, uint64>> Deletes;
  int32 CurrencyCount = 0;
  // The keys themselves, not only their count: a commit adds them to the snapshot's GrantKeys.
  TArray<FString> GrantKeys;
  // Per currency sent, its last entry's BalanceAfter: the balance the backend holds once this batch
  // commits. Empty when the batch carries no currency.
  FRedwoodCurrencyBalances BalancesAfter;
};

// What a flush reply proves about the batch it answers.
enum class ERedwoodItemFlushOutcome : uint8 {
  // Saved: by this attempt, or by an earlier one that this one replays.
  Committed,
  // A probe's batch never committed, and the backend consumed its seq with an empty commit.
  NotApplied,
  // The sidecar never passed it to the realm.
  NotSent,
  // Refused for good and rolled back (`final`).
  FinalRefusal,
  // A trade used the seq and the batch did not apply (`rebaseRequired`).
  RebaseRequired,
  // The backend is behind the seq, so the batch did not apply.
  SequenceGap,
  // It may have committed or not.
  Unknown,
};

// The one reading of a flush reply, shared by the component (its queues and its lane) and the
// subsystem (the snapshot and the staff log), so the two never disagree. SentSeq is the seq the
// batch went out under. A notApplied or a success counts only with a fence at or past SentSeq:
// below it the seq is still free, and the attempt may yet commit there.
inline ERedwoodItemFlushOutcome ClassifyItemFlushReply(
  const FRedwoodItemFlushReply &Reply, int64 SentSeq
) {
  if (Reply.bNotForwarded) {
    return ERedwoodItemFlushOutcome::NotSent;
  }
  if (Reply.bNotApplied) {
    return Reply.CommittedSeq >= SentSeq ? ERedwoodItemFlushOutcome::NotApplied
                                         : ERedwoodItemFlushOutcome::Unknown;
  }
  if (Reply.Error.IsEmpty()) {
    return Reply.CommittedSeq >= SentSeq ? ERedwoodItemFlushOutcome::Committed
                                         : ERedwoodItemFlushOutcome::Unknown;
  }
  if (Reply.bFinal) {
    return ERedwoodItemFlushOutcome::FinalRefusal;
  }
  if (Reply.bRebaseRequired) {
    return ERedwoodItemFlushOutcome::RebaseRequired;
  }
  // Negative means the reply had no fence at all (a dropped socket), which is not a gap.
  if (Reply.CommittedSeq >= 0 && Reply.CommittedSeq < SentSeq - 1) {
    return ERedwoodItemFlushOutcome::SequenceGap;
  }
  return ERedwoodItemFlushOutcome::Unknown;
}

// What staff can tell about an entry or a grant key this server will not save itself.
enum class ERedwoodCurrencySaveState : uint8 {
  // Its batch went out and no reply settled it: it may be saved. A re-grant without reusing its id
  // would pay it twice.
  Unknown,
  // It never reached the database, or the backend refused it.
  NotSaved,
};

// The one staff-rebuild log: one Error line per entry and per grant key, each with the character,
// the save state, Why (what happened to it), and the reuse-id rule. Every place that gives up on
// currency logs through it, so staff read one format. The first UnknownEntryCount entries and
// UnknownGrantCount keys are logged as Unknown, the rest as NotSaved: the front of a queue is what
// went out first.
REDWOOD_API void LogCurrencyForStaff(
  const FString &CharacterId,
  const FString &Why,
  TConstArrayView<FRedwoodCurrencyEntry> Entries,
  int32 UnknownEntryCount,
  TConstArrayView<FString> GrantKeys,
  int32 UnknownGrantCount
);

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

// The field that carries a character's loaded currency, `{ balances, grantKeys }`: beside "items"
// on the player-auth reply, and inline on an offline character file.
inline const TCHAR *const CurrencyLoadField = TEXT("currency");

// Reads a `{ balances: [{ currency, amount }], grantKeys }` object (CurrencyLoadSchema in the
// backend's interfaces.ts). A currency with no row holds 0. Returns false, with the outputs
// untouched, unless every row names a currency once with a strict amount string of at least zero,
// and every grant key is a non-empty string. A load that fails this is a failed load, never a zero
// balance: a game server that started from zero would chain BalanceAfter values the backend
// refuses for good.
inline bool TryParseCurrencyLoad(
  const TSharedPtr<FJsonObject> &CurrencyObject,
  FRedwoodCurrencyBalances &OutBalances,
  TArray<FString> &OutGrantKeys
) {
  const TArray<TSharedPtr<FJsonValue>> *BalanceValues = nullptr;
  const TArray<TSharedPtr<FJsonValue>> *GrantValues = nullptr;
  if (!CurrencyObject.IsValid() ||
      !CurrencyObject->TryGetArrayField(TEXT("balances"), BalanceValues) ||
      !CurrencyObject->TryGetArrayField(TEXT("grantKeys"), GrantValues)) {
    return false;
  }
  FRedwoodCurrencyBalances Balances;
  for (const TSharedPtr<FJsonValue> &BalanceValue : *BalanceValues) {
    const TSharedPtr<FJsonObject> *Row = nullptr;
    if (!BalanceValue.IsValid() || BalanceValue->Type != EJson::Object ||
        !BalanceValue->TryGetObject(Row) || !Row) {
      return false;
    }
    FString Currency;
    FString AmountText;
    int64 Amount = 0;
    // Strings only: TryGetStringField also reads a JSON number, which may have been rounded.
    if (!(*Row)->HasTypedField<EJson::String>(TEXT("currency")) ||
        !(*Row)->TryGetStringField(TEXT("currency"), Currency) || Currency.IsEmpty() ||
        Balances.Contains(Currency) || !(*Row)->HasTypedField<EJson::String>(TEXT("amount")) ||
        !(*Row)->TryGetStringField(TEXT("amount"), AmountText) ||
        !TryParseCurrencyAmount(AmountText, Amount) || Amount < 0) {
      return false;
    }
    Balances.Add(Currency, Amount);
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
  OutBalances = MoveTemp(Balances);
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

// Adds each key that GrantKeys does not hold yet, compared as ContainsGrantKey does. A key list is
// only ever merged into, never replaced: the backend never removes a key, so a replace could drop a
// key that a racing commit added.
inline void MergeGrantKeys(TArray<FString> &GrantKeys, const TArray<FString> &NewKeys) {
  for (const FString &GrantKey : NewKeys) {
    if (!ContainsGrantKey(GrantKeys, GrantKey)) {
      GrantKeys.Add(GrantKey);
    }
  }
}

// The same `{ balances, grantKeys }` shape, for the offline character file. Amounts are decimal
// strings, as on the wire, so an offline file and a backend reply are read by the same rule.
inline TSharedPtr<FJsonObject> MakeCurrencyLoadObject(
  const FRedwoodCurrencyBalances &Balances, const TArray<FString> &GrantKeys
) {
  TSharedPtr<FJsonObject> CurrencyObject = MakeShared<FJsonObject>();
  TArray<TSharedPtr<FJsonValue>> BalanceValues;
  BalanceValues.Reserve(Balances.Num());
  for (const TPair<FString, int64> &Balance : Balances) {
    TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
    Row->SetStringField(TEXT("currency"), Balance.Key);
    Row->SetStringField(TEXT("amount"), LexToString(Balance.Value));
    BalanceValues.Add(MakeShared<FJsonValueObject>(Row));
  }
  CurrencyObject->SetArrayField(TEXT("balances"), BalanceValues);
  TArray<TSharedPtr<FJsonValue>> GrantValues;
  GrantValues.Reserve(GrantKeys.Num());
  for (const FString &GrantKey : GrantKeys) {
    GrantValues.Add(MakeShared<FJsonValueString>(GrantKey));
  }
  CurrencyObject->SetArrayField(TEXT("grantKeys"), GrantValues);
  return CurrencyObject;
}

// A flush the backend refused for good (`final: true`). The backend has raised an alert, and the
// character is frozen with everything staged dropped and logged. The game must send the player
// back through character select: only a fresh hydration from the database repairs the state.
// Also fired, with the same freeze, for both characters of a trade whose outcome could not be
// learned (see URedwoodServerGameSubsystem::EmitItemsTrade). The server then kicks the player,
// unless the game has set URedwoodServerGameSubsystem::bGameHandlesCurrencyDesync.
DECLARE_MULTICAST_DELEGATE_TwoParams(
  FOnItemFlushRefusedFinal, const FString & /*CharacterId*/, const FString & /*Error*/
);

// The realm said this session's writer token is stale: another session owns the character, and
// nothing this server stages for it can be saved. The game must end this session for the
// character, so the player does not keep playing on it. The server then kicks the player, unless
// the game has set URedwoodServerGameSubsystem::bGameHandlesCurrencyDesync.
DECLARE_MULTICAST_DELEGATE_OneParam(FOnWriterFenceLost, const FString & /*CharacterId*/);

// A reload or a settled trade gave the backend's committed balances (every currency; one not in
// the map holds 0), and every pending entry is now re-chained on top of the balance of its own
// currency. The game's running balance of each currency must restart from here: it is the
// BalanceAfter of the last pending entry of that currency, or the committed balance when none is
// pending. A game that keeps chaining from its old running balance writes BalanceAfter values the
// backend refuses for good, and loses those entries.
DECLARE_MULTICAST_DELEGATE_OneParam(
  FOnCurrencyRebased, const FRedwoodCurrencyBalances & /*Balances*/
);

// The alerts a committed flush raised (`alerts` on the ack), for the game to forward to GMs.
DECLARE_MULTICAST_DELEGATE_OneParam(
  FOnCurrencyAlerts, const TArray<TSharedPtr<FJsonObject>> &
);
