// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UI/ElemagicAttributeViewModel.h"
#include "UI/ElemagicUserWidget.h"

// ---- AttributeViewModel 默认值测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAttributeVMDefaultsTest,
	"Elemagic.UI.AttributeVMDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAttributeVMDefaultsTest::RunTest(const FString& Parameters)
{
	UElemagicAttributeViewModel* VM = NewObject<UElemagicAttributeViewModel>();
	if (!TestNotNull(TEXT("ViewModel created"), VM))
	{
		return false;
	}

	TestEqual(TEXT("Default Health is 0"), VM->GetHealth(), 0.f);
	TestEqual(TEXT("Default MaxHealth is 0"), VM->GetMaxHealth(), 0.f);
	TestEqual(TEXT("Default HealthPercent is 0"), VM->GetHealthPercent(), 0.f);
	TestEqual(TEXT("Default AttackPower is 0"), VM->GetAttackPower(), 0.f);
	TestEqual(TEXT("Default Defense is 0"), VM->GetDefense(), 0.f);
	TestEqual(TEXT("Default MoveSpeed is 0"), VM->GetMoveSpeed(), 0.f);
	TestEqual(TEXT("Default FireResistance is 0"), VM->GetFireResistance(), 0.f);
	TestEqual(TEXT("Default DarkResistance is 0"), VM->GetDarkResistance(), 0.f);

	return true;
}

// ---- HealthPercent 计算测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHealthPercentMathTest,
	"Elemagic.UI.HealthPercentMath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHealthPercentMathTest::RunTest(const FString& Parameters)
{
	UElemagicAttributeViewModel* VM = NewObject<UElemagicAttributeViewModel>();
	if (!TestNotNull(TEXT("ViewModel created"), VM))
	{
		return false;
	}

	// 200 上限 / 50 当前 = 25%
	VM->SetMaxHealth(200.f);
	VM->SetHealth(50.f);
	TestEqual(TEXT("HealthPercent = 0.25 (50/200)"), VM->GetHealthPercent(), 0.25f);

	// 满血 = 100%
	VM->SetHealth(200.f);
	TestEqual(TEXT("HealthPercent = 1.0 (200/200)"), VM->GetHealthPercent(), 1.f);

	// MaxHealth 为 0 时防除零，返回 0
	VM->SetMaxHealth(0.f);
	TestEqual(TEXT("HealthPercent = 0 when MaxHealth is 0"), VM->GetHealthPercent(), 0.f);

	return true;
}

// ---- UserWidget 基类 CDO 测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUserWidgetCDOTest,
	"Elemagic.UI.UserWidgetCDO",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUserWidgetCDOTest::RunTest(const FString& Parameters)
{
	const UElemagicUserWidget* CDO = GetDefault<UElemagicUserWidget>();
	TestNotNull(TEXT("UElemagicUserWidget CDO exists"), CDO);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
