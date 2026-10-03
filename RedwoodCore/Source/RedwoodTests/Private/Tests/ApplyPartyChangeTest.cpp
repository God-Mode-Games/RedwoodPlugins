// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins how a realm:parties:changed roster changes the held party
// (HollowedOath#2448). A party never has one member: the realm dissolves it
// and sends the last member the roster with no members.
//   1. A roster with members is held and broadcast.
//   2. An empty roster of the held party leaves no party, and is broadcast.
//   3. An empty roster of another party keeps the held one, and is not
//      broadcast, so the game does not see the held party end.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "RedwoodClientInterface.h"

namespace {
  FRedwoodParty MakeApplyPartyChangeRoster(
    const TCHAR *Id, const TArray<FString> &MemberIds
  ) {
    FRedwoodParty Party;
    Party.bValid = true;
    Party.Id = Id;
    for (const FString &MemberId : MemberIds) {
      FRedwoodPartyMember Member;
      Member.PlayerId = MemberId;
      Party.Members.Add(Member);
    }
    return Party;
  }
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodApplyPartyChangeTest,
  "Redwood.Parties.ApplyPartyChange",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodApplyPartyChangeTest::RunTest(const FString &Parameters) {
  const FRedwoodParty Held =
    MakeApplyPartyChangeRoster(TEXT("party-1"), {TEXT("p1"), TEXT("p2")});

  FRedwoodParty AfterLarger = Held;
  TestTrue(
    TEXT("A roster with members is broadcast"),
    URedwoodClientInterface::ApplyPartyChange(
      AfterLarger,
      MakeApplyPartyChangeRoster(
        TEXT("party-1"), {TEXT("p1"), TEXT("p2"), TEXT("p3")}
      )
    )
  );
  TestTrue(TEXT("A roster with members is held"), AfterLarger.bValid);
  TestEqual(TEXT("with all its members"), AfterLarger.Members.Num(), 3);

  FRedwoodParty AfterDissolve = Held;
  TestTrue(
    TEXT("An empty roster of the held party is broadcast"),
    URedwoodClientInterface::ApplyPartyChange(
      AfterDissolve, MakeApplyPartyChangeRoster(TEXT("party-1"), {})
    )
  );
  TestFalse(TEXT("An empty roster of the held party leaves no party"), AfterDissolve.bValid);
  TestEqual(TEXT("and no members"), AfterDissolve.Members.Num(), 0);

  FRedwoodParty AfterOther = Held;
  TestFalse(
    TEXT("An empty roster of another party is not broadcast"),
    URedwoodClientInterface::ApplyPartyChange(
      AfterOther, MakeApplyPartyChangeRoster(TEXT("party-2"), {})
    )
  );
  TestTrue(TEXT("An empty roster of another party keeps the held one"), AfterOther.bValid);
  // TestEqualSensitive: the string forms of TestEqual ignore case.
  TestEqualSensitive(TEXT("the held id"), *AfterOther.Id, TEXT("party-1"));
  TestEqual(TEXT("the held members"), AfterOther.Members.Num(), 2);

  return true;
}
