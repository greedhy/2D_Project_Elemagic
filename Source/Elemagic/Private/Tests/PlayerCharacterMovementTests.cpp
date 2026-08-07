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
	const UCharacterMovementComponent* MoveComp = CDO ? CDO->GetCharacterMovement() : nullptr;
	if (!TestNotNull(TEXT("APlayerCharacter CDO has a CharacterMovementComponent"), MoveComp))
	{
		return false;
	}

	TestEqual(TEXT("MaxWalkSpeed tuned for a brisk platformer run"), MoveComp->MaxWalkSpeed, 600.f);
	TestEqual(TEXT("JumpZVelocity tuned for a snappy jump"), MoveComp->JumpZVelocity, 700.f);
	TestEqual(TEXT("GravityScale tuned for a tight fall arc"), MoveComp->GravityScale, 2.f);
	TestEqual(TEXT("AirControl tuned for responsive air movement"), MoveComp->AirControl, 0.8f);
	TestEqual(TEXT("BrakingDecelerationWalking tuned to stop crisply"), MoveComp->BrakingDecelerationWalking, 2048.f);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
