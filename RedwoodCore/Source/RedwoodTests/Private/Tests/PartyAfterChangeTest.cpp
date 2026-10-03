// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins the party the client holds after a realm:parties:changed roster
// (HollowedOath#2448). A party never has one member: the realm dissolves it
// and sends the last member the roster with no members.
//   1. A roster with members is held.
//   2. An empty roster of the held party leaves no party.
//   3. An empty roster of another party keeps the held one.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "RedwoodClientInterface.h"

namespace {
  FRedwoodParty MakePartyAfterChangeRoster(
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
  FRedwoodPartyAfterChangeTest,
  "Redwood.Parties.PartyAfterChange",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodPartyAfterChangeTest::RunTest(const FString &Parameters) {
  const FRedwoodParty Held =
    MakePartyAfterChangeRoster(TEXT("party-1"), {TEXT("p1"), TEXT("p2")});

  const FRedwoodParty Larger = MakePartyAfterChangeRoster(
    TEXT("party-1"), {TEXT("p1"), TEXT("p2"), TEXT("p3")}
  );
  const FRedwoodParty AfterLarger =
    URedwoodClientInterface::PartyAfterChange(Held, Larger);
  TestTrue(TEXT("A roster with members is held"), AfterLarger.bValid);
  TestEqual(TEXT("with all its members"), AfterLarger.Members.Num(), 3);

  const FRedwoodParty AfterDissolve = URedwoodClientInterface::PartyAfterChange(
    Held, MakePartyAfterChangeRoster(TEXT("party-1"), {})
  );
  TestFalse(TEXT("An empty roster of the held party leaves no party"), AfterDissolve.bValid);
  TestEqual(TEXT("and no members"), AfterDissolve.Members.Num(), 0);

  const FRedwoodParty AfterOther = URedwoodClientInterface::PartyAfterChange(
    Held, MakePartyAfterChangeRoster(TEXT("party-2"), {})
  );
  TestTrue(TEXT("An empty roster of another party keeps the held one"), AfterOther.bValid);
  // TestEqualSensitive: the string forms of TestEqual ignore case.
  TestEqualSensitive(TEXT("the held id"), *AfterOther.Id, TEXT("party-1"));
  TestEqual(TEXT("the held members"), AfterOther.Members.Num(), 2);

  return true;
}
