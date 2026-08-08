// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "PlayerCharacter.h"
#include "GameFramework/CharacterMovementComponent.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlayerCharacterMovementTuningTest, "Elemagic.PlayerCharacter.MovementTuning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayerCharacterMovementTuningTest::RunTest(const FString& Parameters)
{
	const APlayerCharacter* CDO = GetDefault<APlayerCharacter>();
	if (!TestNotNull(TEXT("APlayerCharacter CDO exists"), CDO))
	{
		return false;
	}

	const UCharacterMovementComponent* MoveComp = CDO->GetCharacterMovement();
	if (!TestNotNull(TEXT("APlayerCharacter CDO has a CharacterMovementComponent"), MoveComp))
	{
		return false;
	}

	// BP 可调参数 C++ 默认值——运行时 BeginPlay 会把这些值同步到 CharacterMovementComponent,
	// 以蓝图子类 Class Defaults 覆盖后的值为准。CDO 上组件值不等于属性值是正常的(引擎默认 vs 我们的默认),
	// BeginPlay 之后才会对齐。
	TestEqual(TEXT("MoveSpeed default for brisk platformer run"), CDO->MoveSpeed, 600.f);
	TestEqual(TEXT("JumpVelocity default for snappy jump"), CDO->JumpVelocity, 700.f);
	TestEqual(TEXT("JumpHoldTime default"), CDO->JumpHoldTime, 0.3f);
	TestEqual(TEXT("JumpMaxCount default is 2 for double jump"), CDO->JumpMaxCount, 2);

	// 构造中直接设置的固定手感参数(不对应蓝图属性,不受 BP 覆盖)
	TestEqual(TEXT("GravityScale tuned for tight fall arc"), MoveComp->GravityScale, 2.f);
	TestEqual(TEXT("MaxAcceleration set for instant response"), MoveComp->MaxAcceleration, 99999.f);
	TestEqual(TEXT("BrakingDecelerationWalking set for instant stop"), MoveComp->BrakingDecelerationWalking, 99999.f);
	TestEqual(TEXT("AirControl set to 1 for full air responsiveness"), MoveComp->AirControl, 1.f);
	TestFalse(TEXT("bUseSeparateBrakingFriction disabled for unified braking"), MoveComp->bUseSeparateBrakingFriction);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
