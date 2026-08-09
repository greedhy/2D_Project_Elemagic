// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "GA_Dash.h"
#include "ElemagicGameplayTags.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlayerCharacterDashTest, "Elemagic.PlayerCharacter.Dash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayerCharacterDashTest::RunTest(const FString& Parameters)
{
	const UGA_Dash* CDO = GetDefault<UGA_Dash>();
	if (!TestNotNull(TEXT("GA_Dash CDO exists"), CDO))
	{
		return false;
	}

	// Ability tag (public getter)
	TestTrue(TEXT("GA_Dash has Ability.Dash in AssetTags"),
		CDO->GetAssetTags().HasTagExact(ElemagicGameplayTags::Ability_Dash));

	// Editable parameters
	TestEqual(TEXT("DashSpeed is 2667 (400 units over 0.15s)"), CDO->DashSpeed, 2667.f);
	TestEqual(TEXT("DashDuration is 0.15s"), CDO->DashDuration, 0.15f);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
