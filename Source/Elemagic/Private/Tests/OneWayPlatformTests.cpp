// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Actor/OneWayPlatform.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOneWayPlatformPassThroughTest, "Elemagic.OneWayPlatform.ShouldPassThroughPlatform",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOneWayPlatformPassThroughTest::RunTest(const FString& Parameters)
{
	const float PlatformTopZ = 100.f;

	// bWasPassingThrough = false: 还没开始穿透,走"是否该开始穿透"的规则。
	TestTrue(TEXT("Below platform and rising should start passing through"),
		AOneWayPlatform::ShouldPassThroughPlatform(50.f, PlatformTopZ, 200.f, false));

	TestTrue(TEXT("Just below platform top and rising should start passing through"),
		AOneWayPlatform::ShouldPassThroughPlatform(99.99f, PlatformTopZ, 200.f, false));

	TestFalse(TEXT("Below platform and falling should not start passing through"),
		AOneWayPlatform::ShouldPassThroughPlatform(50.f, PlatformTopZ, -200.f, false));

	TestFalse(TEXT("Below platform and stationary should not start passing through"),
		AOneWayPlatform::ShouldPassThroughPlatform(50.f, PlatformTopZ, 0.f, false));

	TestFalse(TEXT("Exactly at platform top while rising should not start passing through (already arrived)"),
		AOneWayPlatform::ShouldPassThroughPlatform(100.f, PlatformTopZ, 200.f, false));

	TestFalse(TEXT("Exactly at platform top while falling should not start passing through"),
		AOneWayPlatform::ShouldPassThroughPlatform(100.f, PlatformTopZ, -200.f, false));

	TestFalse(TEXT("Above platform and falling onto it should not start passing through (gets supported)"),
		AOneWayPlatform::ShouldPassThroughPlatform(150.f, PlatformTopZ, -200.f, false));

	// bWasPassingThrough = true: 已经在穿透中,只看位置,不看速度——这是修复"跳跃最高点
	// 恰好卡在平台内部、速度过零那一帧被判定为不该穿透从而被解穿插弹飞"这个 bug 的关键。
	TestTrue(TEXT("Already passing through and still below top should keep passing through even if velocity just flipped negative"),
		AOneWayPlatform::ShouldPassThroughPlatform(90.f, PlatformTopZ, -10.f, true));

	TestTrue(TEXT("Already passing through and still below top should keep passing through while stationary"),
		AOneWayPlatform::ShouldPassThroughPlatform(90.f, PlatformTopZ, 0.f, true));

	TestFalse(TEXT("Already passing through but feet have cleared the top should stop passing through"),
		AOneWayPlatform::ShouldPassThroughPlatform(100.f, PlatformTopZ, 50.f, true));

	TestFalse(TEXT("Already passing through but well above the top should stop passing through"),
		AOneWayPlatform::ShouldPassThroughPlatform(150.f, PlatformTopZ, -200.f, true));

	return true;
}

#endif // WITH_AUTOMATION_TESTS
