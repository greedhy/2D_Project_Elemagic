// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "GA_DashBase.h"
#include "ElemagicGameplayTags.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlayerCharacterDashTest, "Elemagic.PlayerCharacter.Dash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayerCharacterDashTest::RunTest(const FString& Parameters)
{
	const UGA_DashBase* CDO = GetDefault<UGA_DashBase>();
	if (!TestNotNull(TEXT("GA_DashBase CDO exists"), CDO))
	{
		return false;
	}

	TestTrue(TEXT("GA_DashBase has Ability.Dash in AssetTags"),
		CDO->GetAssetTags().HasTagExact(ElemagicGameplayTags::Ability_Dash));

	TestEqual(TEXT("DashSpeed is 2667 (400 units over 0.15s)"), CDO->DashSpeed, 2667.f);
	TestEqual(TEXT("DashDuration is 0.15s"), CDO->DashDuration, 0.15f);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
