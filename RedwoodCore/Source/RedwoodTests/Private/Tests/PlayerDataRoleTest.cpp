// Copyright Incanta Games. All Rights Reserved.

// FORK(hollowed-oath): covers the PlayerIdentity.role parsing added to
// ParsePlayerData for #1157. The role feeds a privilege check, so the narrowing
// from the JSON double to int32 is range-checked and everything unrepresentable
// falls back to 0 (no privilege). These pin that fail-closed contract -- without
// them the guard's only evidence is its own comment.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "RedwoodCommonGameSubsystem.h"
#include "Types/RedwoodTypesPlayersGuilds.h"

// No UE equivalent exists for the IEEE special values, and the engine itself
// reaches for <limits> in the same situation.
#include <limits>

namespace {
  // ParsePlayerData reads id/nickname unconditionally, so every fixture supplies
  // them; only the role field varies between cases.
  TSharedPtr<FJsonObject> MakePlayerDataObj() {
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("id"), TEXT("player-1"));
    Obj->SetStringField(TEXT("nickname"), TEXT("Tester"));
    return Obj;
  }

  int32 ParseRoleFromNumber(const double RoleValue) {
    TSharedPtr<FJsonObject> Obj = MakePlayerDataObj();
    Obj->SetField(TEXT("role"), MakeShared<FJsonValueNumber>(RoleValue));
    return URedwoodCommonGameSubsystem::ParsePlayerData(Obj).Role;
  }
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodPlayerDataRoleInRangeTest,
  "Redwood.PlayerData.RoleInRange",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodPlayerDataRoleInRangeTest::RunTest(const FString &Parameters) {
  TestEqual(TEXT("Player role 0 parses"), ParseRoleFromNumber(0.0), 0);
  TestEqual(TEXT("Moderator role 50 parses"), ParseRoleFromNumber(50.0), 50);
  TestEqual(TEXT("GameMaster role 75 parses"), ParseRoleFromNumber(75.0), 75);
  TestEqual(TEXT("Admin role 100 parses"), ParseRoleFromNumber(100.0), 100);

  // A tier this build predates must survive as its raw number, since the game
  // side compares with >= rather than mapping onto a known enum member.
  TestEqual(TEXT("Unknown tier 60 parses"), ParseRoleFromNumber(60.0), 60);

  // The exact representable bounds must pass through untouched -- the guard
  // rejects what is out of range, not what is merely large.
  TestEqual(TEXT("MAX_int32 parses"), ParseRoleFromNumber(2147483647.0), MAX_int32);
  TestEqual(TEXT("MIN_int32 parses"), ParseRoleFromNumber(-2147483648.0), MIN_int32);

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodPlayerDataRoleFailsClosedTest,
  "Redwood.PlayerData.RoleFailsClosed",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodPlayerDataRoleFailsClosedTest::RunTest(const FString &Parameters) {
  // Just past each bound. Narrowing these with a bare static_cast is undefined
  // behaviour, so before the guard the result was whatever the target's
  // float-to-int instruction happened to produce.
  TestEqual(
    TEXT("One past MAX_int32 yields no privilege"),
    ParseRoleFromNumber(2147483648.0),
    0
  );
  TestEqual(
    TEXT("One past MIN_int32 yields no privilege"),
    ParseRoleFromNumber(-2147483649.0),
    0
  );

  // Far out of range, both signs.
  TestEqual(TEXT("Huge positive yields no privilege"), ParseRoleFromNumber(1.0e18), 0);
  TestEqual(TEXT("Huge negative yields no privilege"), ParseRoleFromNumber(-1.0e18), 0);

  // Non-finite. The guard is written as an inclusive range check precisely so
  // these fall out for free: every comparison against NaN is false.
  TestEqual(
    TEXT("NaN yields no privilege"),
    ParseRoleFromNumber(std::numeric_limits<double>::quiet_NaN()),
    0
  );
  TestEqual(
    TEXT("Positive infinity yields no privilege"),
    ParseRoleFromNumber(std::numeric_limits<double>::infinity()),
    0
  );
  TestEqual(
    TEXT("Negative infinity yields no privilege"),
    ParseRoleFromNumber(-std::numeric_limits<double>::infinity()),
    0
  );

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodPlayerDataRoleMissingOrWrongTypeTest,
  "Redwood.PlayerData.RoleMissingOrWrongType",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodPlayerDataRoleMissingOrWrongTypeTest::RunTest(
  const FString &Parameters
) {
  // An unforked backend omits the field entirely; that must read as no
  // privilege rather than leaving the field indeterminate.
  TestEqual(
    TEXT("Absent role yields no privilege"),
    URedwoodCommonGameSubsystem::ParsePlayerData(MakePlayerDataObj()).Role,
    0
  );

  // A non-numeric string and an object both make TryGetNumber return false --
  // TJsonValueString gates on IsNumeric(), and FJsonValueObject inherits the
  // base that always fails. Role is then never assigned and keeps its `= 0`
  // member initialiser, so these two are the same no-privilege fallback the
  // absent-field case above asserts, and are pinned exactly.
  TSharedPtr<FJsonObject> StringObj = MakePlayerDataObj();
  StringObj->SetStringField(TEXT("role"), TEXT("admin"));
  TestEqual(
    TEXT("Non-numeric string role yields no privilege"),
    URedwoodCommonGameSubsystem::ParsePlayerData(StringObj).Role,
    0
  );

  TSharedPtr<FJsonObject> ObjectObj = MakePlayerDataObj();
  ObjectObj->SetObjectField(TEXT("role"), MakeShared<FJsonObject>());
  TestEqual(
    TEXT("Object role yields no privilege"),
    URedwoodCommonGameSubsystem::ParsePlayerData(ObjectObj).Role,
    0
  );

  // A bool is the exception, and does NOT reach that fallback:
  // FJsonValueBoolean::TryGetNumber overrides the base and coerces, so `true`
  // succeeds as 1.0 and lands in Role as 1 (measured, not inferred). That 1 is
  // the engine's coercion rule rather than anything this fork promises, so it
  // is asserted against the invariant that actually matters -- no wrong-typed
  // value may reach a privileged tier -- instead of being pinned to 1 and
  // turning a future engine change into a spurious security-test failure.
  const int32 LowestPrivilegedTier = 50; // PlayerRole.Moderator

  TSharedPtr<FJsonObject> BoolObj = MakePlayerDataObj();
  BoolObj->SetBoolField(TEXT("role"), true);
  TestTrue(
    TEXT("Boolean role stays below every privileged tier"),
    URedwoodCommonGameSubsystem::ParsePlayerData(BoolObj).Role
      < LowestPrivilegedTier
  );

  return true;
}
