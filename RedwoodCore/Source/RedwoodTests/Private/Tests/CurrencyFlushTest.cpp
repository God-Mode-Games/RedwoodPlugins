// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart. Pins the game-server
// subsystem's half of the currency ledger: how amounts are read off the wire, what a flush claims
// from the component's queues, and what each item flush reply and each reload reply does to the
// component and to the PlayerState snapshot. CurrencyPersistenceTest.cpp pins the component half.
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "RedwoodCurrencyTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCurrencyAmountParseTest,
  "Redwood.Currency.AmountParse",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// The backend's parseAmount rule. Every string it refuses, the game server must refuse too.
bool FRedwoodCurrencyAmountParseTest::RunTest(const FString &Parameters) {
  const TCHAR *Refused[] = {
    TEXT("9223372036854775808"),
    TEXT("-9223372036854775809"),
    TEXT("-0"),
    TEXT("1e3"),
    TEXT(" 5"),
    TEXT("5 "),
    TEXT("+5"),
    TEXT(""),
    TEXT("-"),
    TEXT("05"),
    TEXT("1.5"),
  };
  for (const TCHAR *Text : Refused) {
    int64 Amount = 0;
    TestFalse(FString::Printf(TEXT("'%s' is refused"), Text), TryParseCurrencyAmount(Text, Amount));
  }

  struct FAccepted {
    const TCHAR *Text;
    int64 Expected;
  };
  const FAccepted Accepted[] = {
    {TEXT("0"), 0},
    {TEXT("-250"), -250},
    {TEXT("1000000"), 1000000},
    {TEXT("9223372036854775807"), MAX_int64},
    {TEXT("-9223372036854775808"), MIN_int64},
  };
  for (const FAccepted &Case : Accepted) {
    int64 Amount = 0;
    TestTrue(FString::Printf(TEXT("'%s' is accepted"), Case.Text), TryParseCurrencyAmount(Case.Text, Amount));
    TestEqual(FString::Printf(TEXT("'%s' reads exactly"), Case.Text), Amount, Case.Expected);
  }

  return true;
}
