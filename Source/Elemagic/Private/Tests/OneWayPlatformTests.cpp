// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Actor/OneWayPlatform.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOneWayPlatformPassThroughTest, "Elemagic.OneWayPlatform.ShouldPassThroughPlatform",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOneWayPlatformPassThroughTest::RunTest(const FString& Parameters)
{
	const float PlatformTopZ = 100.f;

	TestTrue(TEXT("Below platform and rising should pass through"),
		AOneWayPlatform::ShouldPassThroughPlatform(50.f, PlatformTopZ, 200.f));

	TestFalse(TEXT("Below platform and falling should not pass through"),
		AOneWayPlatform::ShouldPassThroughPlatform(50.f, PlatformTopZ, -200.f));

	TestFalse(TEXT("Below platform and stationary should not pass through"),
		AOneWayPlatform::ShouldPassThroughPlatform(50.f, PlatformTopZ, 0.f));

	TestFalse(TEXT("Exactly at platform top while rising should not pass through (already arrived)"),
		AOneWayPlatform::ShouldPassThroughPlatform(100.f, PlatformTopZ, 200.f));

	TestFalse(TEXT("Above platform and falling onto it should not pass through (gets supported)"),
		AOneWayPlatform::ShouldPassThroughPlatform(150.f, PlatformTopZ, -200.f));

	return true;
}

#endif // WITH_AUTOMATION_TESTS
