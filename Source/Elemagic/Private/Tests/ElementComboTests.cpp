// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "ElementComboInfo.h"
#include "ElementSystemComponent.h"
#include "InventoryComponent.h"
#include "ElemagicGameplayTags.h"

// ---- UElementComboInfo 默认值测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FComboInfoDefaultsTest,
	"Elemagic.Element.ComboInfoDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FComboInfoDefaultsTest::RunTest(const FString& Parameters)
{
	const UElementComboInfo* CDO = GetDefault<UElementComboInfo>();
	if (!TestNotNull(TEXT("UElementComboInfo CDO exists"), CDO))
	{
		return false;
	}

	TestTrue(TEXT("Default Combos array is empty"), CDO->Combos.Num() == 0);

	FGameplayTagContainer Empty;
	TestNull(TEXT("FindCombo(empty) returns null"), CDO->FindCombo(Empty));

	return true;
}

// ---- FindCombo 查找顺序无关测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FComboLookupTest,
	"Elemagic.Element.ComboLookup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FComboLookupTest::RunTest(const FString& Parameters)
{
	UElementComboInfo* Info = NewObject<UElementComboInfo>();
	if (!TestNotNull(TEXT("Created combo info"), Info))
	{
		return false;
	}

	// 添加 {Fire, Water, Earth}
	FElementComboEntry Entry;
	Entry.Elements.AddTag(ElemagicGameplayTags::Element_Fire);
	Entry.Elements.AddTag(ElemagicGameplayTags::Element_Water);
	Entry.Elements.AddTag(ElemagicGameplayTags::Element_Earth);
	Info->Combos.Add(Entry);

	// 正序查询
	FGameplayTagContainer Query1;
	Query1.AddTag(ElemagicGameplayTags::Element_Fire);
	Query1.AddTag(ElemagicGameplayTags::Element_Water);
	Query1.AddTag(ElemagicGameplayTags::Element_Earth);
	TestTrue(TEXT("FindCombo(fire,water,earth) found"), Info->FindCombo(Query1) != nullptr);

	// 反序查询（顺序无关）
	FGameplayTagContainer Query2;
	Query2.AddTag(ElemagicGameplayTags::Element_Earth);
	Query2.AddTag(ElemagicGameplayTags::Element_Water);
	Query2.AddTag(ElemagicGameplayTags::Element_Fire);
	TestTrue(TEXT("FindCombo(earth,water,fire) found (order-independent)"), Info->FindCombo(Query2) != nullptr);

	// 缺一个元素 → 未找到
	FGameplayTagContainer Query3;
	Query3.AddTag(ElemagicGameplayTags::Element_Fire);
	Query3.AddTag(ElemagicGameplayTags::Element_Water);
	TestNull(TEXT("FindCombo(fire,water) returns null"), Info->FindCombo(Query3));

	// 多组合共存：添加第二个组合 {Fire, Water, Wind}
	FElementComboEntry Entry2;
	Entry2.Elements.AddTag(ElemagicGameplayTags::Element_Fire);
	Entry2.Elements.AddTag(ElemagicGameplayTags::Element_Water);
	Entry2.Elements.AddTag(ElemagicGameplayTags::Element_Wind);
	Info->Combos.Add(Entry2);

	FGameplayTagContainer Query4;
	Query4.AddTag(ElemagicGameplayTags::Element_Fire);
	Query4.AddTag(ElemagicGameplayTags::Element_Water);
	Query4.AddTag(ElemagicGameplayTags::Element_Wind);
	const FElementComboEntry* Found = Info->FindCombo(Query4);
	TestTrue(TEXT("FindCombo(fire,water,wind) found second combo"), Found != nullptr);

	// 第一个组合仍可被正确查到（不受第二个组合影响）
	TestTrue(TEXT("First combo still findable"), Info->FindCombo(Query1) != nullptr);

	return true;
}

// ---- UElementSystemComponent CDO 默认值测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FElementSystemDefaultsTest,
	"Elemagic.Element.ElementSystemDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FElementSystemDefaultsTest::RunTest(const FString& Parameters)
{
	const UElementSystemComponent* CDO = GetDefault<UElementSystemComponent>();
	if (!TestNotNull(TEXT("UElementSystemComponent CDO exists"), CDO))
	{
		return false;
	}

	// CDO 未 Init，ComboInfo 为 null → 不可合成
	TestFalse(TEXT("CanSynthesize false on CDO (no ComboInfo)"), CDO->CanSynthesize());
	TestTrue(TEXT("GetLoadout empty on CDO"), CDO->GetLoadout().Num() == 0);

	return true;
}

// ---- UInventoryComponent CDO 默认值测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInventoryDefaultsTest,
	"Elemagic.Element.InventoryDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInventoryDefaultsTest::RunTest(const FString& Parameters)
{
	const UInventoryComponent* CDO = GetDefault<UInventoryComponent>();
	if (!TestNotNull(TEXT("UInventoryComponent CDO exists"), CDO))
	{
		return false;
	}

	TestEqual(TEXT("GetItemCount default is 0"), CDO->GetItemCount(), 0);
	TestTrue(TEXT("GetItems empty on CDO"), CDO->GetItems().Num() == 0);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
