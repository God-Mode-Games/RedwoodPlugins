// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart. See
// RedwoodCurrencyTypes.h.

#include "RedwoodCurrencyTypes.h"
#include "RedwoodModule.h"

static const TCHAR *CurrencySaveStateText(ERedwoodCurrencySaveState State) {
  return State == ERedwoodCurrencySaveState::Unknown ? TEXT("outcome UNKNOWN (it may be saved)")
                                                     : TEXT("NOT saved");
}

void LogCurrencyForStaff(
  const FString &CharacterId,
  const FString &Why,
  TConstArrayView<FRedwoodCurrencyEntry> Entries,
  int32 UnknownEntryCount,
  TConstArrayView<FString> GrantKeys,
  int32 UnknownGrantCount
) {
  // The backend refuses an entry id or a grant key it already holds, so a re-grant that reuses the
  // logged one can never pay twice, whatever the state says. That is why every line repeats it.
  for (int32 Index = 0; Index < Entries.Num(); ++Index) {
    const FRedwoodCurrencyEntry &Entry = Entries[Index];
    UE_LOG(
      LogRedwood,
      Error,
      TEXT("Currency for staff rebuild: character %s entry %s delta %lld reason %s, %s: %s. A re-grant must reuse entry id %s, so the backend refuses a second copy."),
      *CharacterId,
      *Entry.Id,
      Entry.Delta,
      *Entry.Reason,
      CurrencySaveStateText(
        Index < UnknownEntryCount ? ERedwoodCurrencySaveState::Unknown : ERedwoodCurrencySaveState::NotSaved
      ),
      *Why,
      *Entry.Id
    );
  }
  for (int32 Index = 0; Index < GrantKeys.Num(); ++Index) {
    UE_LOG(
      LogRedwood,
      Error,
      TEXT("Currency for staff rebuild: character %s grant %s, %s: %s. A re-grant must reuse grant key %s, so the backend refuses a second use."),
      *CharacterId,
      *GrantKeys[Index],
      CurrencySaveStateText(
        Index < UnknownGrantCount ? ERedwoodCurrencySaveState::Unknown : ERedwoodCurrencySaveState::NotSaved
      ),
      *Why,
      *GrantKeys[Index]
    );
  }
}
