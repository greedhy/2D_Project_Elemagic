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

	// UPaperFlipbook* 是裸 UObject 指针,FAutomationTestBase::TestEqual 没有匹配的重载,
	// 用 TestTrue + 手动 == 比较来避免重载决议失败。
	TestTrue(TEXT("Grounded and idle picks Idle flipbook"),
		ACharacterBase::SelectFlipbookForState(false, false, Idle, Run, Jump) == Idle);

	TestTrue(TEXT("Grounded and moving picks Run flipbook"),
		ACharacterBase::SelectFlipbookForState(false, true, Idle, Run, Jump) == Run);

	TestTrue(TEXT("Falling picks Jump flipbook when assigned"),
		ACharacterBase::SelectFlipbookForState(true, true, Idle, Run, Jump) == Jump);

	TestTrue(TEXT("Falling without a Jump flipbook falls back to Run"),
		ACharacterBase::SelectFlipbookForState(true, true, Idle, Run, nullptr) == Run);

	TestTrue(TEXT("Falling without Jump or Run flipbook falls back to Idle"),
		ACharacterBase::SelectFlipbookForState(true, true, Idle, nullptr, nullptr) == Idle);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
