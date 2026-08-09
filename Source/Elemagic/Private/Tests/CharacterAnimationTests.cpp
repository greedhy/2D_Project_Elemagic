// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "CharacterBase.h"
#include "PaperFlipbook.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCharacterBaseSelectFlipbookTest, "Elemagic.CharacterBase.SelectFlipbookForState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCharacterBaseSelectFlipbookTest::RunTest(const FString& Parameters)
{
	UPaperFlipbook* Idle = NewObject<UPaperFlipbook>();
	UPaperFlipbook* Run = NewObject<UPaperFlipbook>();
	UPaperFlipbook* Jump = NewObject<UPaperFlipbook>();

	// Signature: (bIsDashing, bIsFalling, bIsMoving, Idle, Run, Jump, Dash)
	TestTrue(TEXT("Grounded and idle picks Idle flipbook"),
		ACharacterBase::SelectFlipbookForState(false, false, false, Idle, Run, Jump, nullptr) == Idle);

	TestTrue(TEXT("Grounded and moving picks Run flipbook"),
		ACharacterBase::SelectFlipbookForState(false, false, true, Idle, Run, Jump, nullptr) == Run);

	TestTrue(TEXT("Falling picks Jump flipbook when assigned"),
		ACharacterBase::SelectFlipbookForState(false, true, true, Idle, Run, Jump, nullptr) == Jump);

	TestTrue(TEXT("Falling without a Jump flipbook falls back to Run"),
		ACharacterBase::SelectFlipbookForState(false, true, true, Idle, Run, nullptr, nullptr) == Run);

	TestTrue(TEXT("Falling without Jump or Run flipbook falls back to Idle"),
		ACharacterBase::SelectFlipbookForState(false, true, true, Idle, nullptr, nullptr, nullptr) == Idle);

	TestTrue(TEXT("Dashing picks Dash flipbook even when falling"),
		ACharacterBase::SelectFlipbookForState(true, true, true, Idle, Run, Jump, Run) == Run);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
