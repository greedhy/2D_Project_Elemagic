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

	// BP 可调参数应设置合理的默认值
	TestEqual(TEXT("MoveSpeed default for brisk platformer run"), CDO->MoveSpeed, 600.f);
	TestEqual(TEXT("JumpVelocity default for snappy jump"), CDO->JumpVelocity, 700.f);
	TestEqual(TEXT("JumpHoldTime default"), CDO->JumpHoldTime, 0.3f);
	TestEqual(TEXT("MaxJumpCount default is 2 for double jump"), CDO->MaxJumpCount, 2);

	// 构造中应把 CDO 属性写入 CharacterMovementComponent / ACharacter
	TestEqual(TEXT("MaxWalkSpeed driven by MoveSpeed"), MoveComp->MaxWalkSpeed, CDO->MoveSpeed);
	TestEqual(TEXT("JumpZVelocity driven by JumpVelocity"), MoveComp->JumpZVelocity, CDO->JumpVelocity);
	TestEqual(TEXT("JumpMaxCount driven by MaxJumpCount"), CDO->JumpMaxCount, CDO->MaxJumpCount);

	// 手感参数
	TestEqual(TEXT("GravityScale tuned for tight fall arc"), MoveComp->GravityScale, 2.f);

	// 零惯性即时响应
	TestEqual(TEXT("MaxAcceleration set for instant response"), MoveComp->MaxAcceleration, 99999.f);
	TestEqual(TEXT("BrakingDecelerationWalking set for instant stop"), MoveComp->BrakingDecelerationWalking, 99999.f);
	TestEqual(TEXT("AirControl set to 1 for full air responsiveness"), MoveComp->AirControl, 1.f);
	TestFalse(TEXT("bUseSeparateBrakingFriction disabled for unified braking"), MoveComp->bUseSeparateBrakingFriction);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
