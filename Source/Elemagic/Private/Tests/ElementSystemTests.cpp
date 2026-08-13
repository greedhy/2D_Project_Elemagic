// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "ElemagicGameplayTags.h"
#include "CharacterAttributeSetBase.h"
#include "ElemagicEnemyCharacter.h"
#include "ElemagicElementOrb.h"
#include "Components/BoxComponent.h"

// ---- Element.* Tag 有效性测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FElementTagValidityTest,
	"Elemagic.Element.TagsValid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FElementTagValidityTest::RunTest(const FString& Parameters)
{
	const FGameplayTag Fire      = ElemagicGameplayTags::Element_Fire;
	const FGameplayTag Water     = ElemagicGameplayTags::Element_Water;
	const FGameplayTag Earth     = ElemagicGameplayTags::Element_Earth;
	const FGameplayTag Wind      = ElemagicGameplayTags::Element_Wind;
	const FGameplayTag Lightning = ElemagicGameplayTags::Element_Lightning;
	const FGameplayTag Light     = ElemagicGameplayTags::Element_Light;
	const FGameplayTag Dark      = ElemagicGameplayTags::Element_Dark;

	TestTrue(TEXT("Element.Fire is valid"), Fire.IsValid());
	TestTrue(TEXT("Element.Water is valid"), Water.IsValid());
	TestTrue(TEXT("Element.Earth is valid"), Earth.IsValid());
	TestTrue(TEXT("Element.Wind is valid"), Wind.IsValid());
	TestTrue(TEXT("Element.Lightning is valid"), Lightning.IsValid());
	TestTrue(TEXT("Element.Light is valid"), Light.IsValid());
	TestTrue(TEXT("Element.Dark is valid"), Dark.IsValid());

	TestEqual(TEXT("Element_Fire string"), Fire.ToString(), FString("Element.Fire"));
	TestEqual(TEXT("Element_Dark string"), Dark.ToString(), FString("Element.Dark"));

	return true;
}

// ---- 抗性属性默认值测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResistanceDefaultsTest,
	"Elemagic.Element.ResistanceDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FResistanceDefaultsTest::RunTest(const FString& Parameters)
{
	const UCharacterAttributeSetBase* CDO = GetDefault<UCharacterAttributeSetBase>();
	if (!TestNotNull(TEXT("AttributeSet CDO exists"), CDO))
	{
		return false;
	}

	TestEqual(TEXT("FireResistance default is 0"), CDO->GetFireResistance(), 0.f);
	TestEqual(TEXT("WaterResistance default is 0"), CDO->GetWaterResistance(), 0.f);
	TestEqual(TEXT("EarthResistance default is 0"), CDO->GetEarthResistance(), 0.f);
	TestEqual(TEXT("WindResistance default is 0"), CDO->GetWindResistance(), 0.f);
	TestEqual(TEXT("LightningResistance default is 0"), CDO->GetLightningResistance(), 0.f);
	TestEqual(TEXT("LightResistance default is 0"), CDO->GetLightResistance(), 0.f);
	TestEqual(TEXT("DarkResistance default is 0"), CDO->GetDarkResistance(), 0.f);

	return true;
}

// ---- GetResistanceForElement 映射测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResistanceMappingTest,
	"Elemagic.Element.ResistanceMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FResistanceMappingTest::RunTest(const FString& Parameters)
{
	const UCharacterAttributeSetBase* CDO = GetDefault<UCharacterAttributeSetBase>();
	if (!TestNotNull(TEXT("AttributeSet CDO exists"), CDO))
	{
		return false;
	}

	// 每个元素 Tag 应映射到对应的抗性 getter
	TestEqual(TEXT("Element_Fire maps to FireResistance"),
		CDO->GetResistanceForElement(ElemagicGameplayTags::Element_Fire), CDO->GetFireResistance());
	TestEqual(TEXT("Element_Water maps to WaterResistance"),
		CDO->GetResistanceForElement(ElemagicGameplayTags::Element_Water), CDO->GetWaterResistance());
	TestEqual(TEXT("Element_Earth maps to EarthResistance"),
		CDO->GetResistanceForElement(ElemagicGameplayTags::Element_Earth), CDO->GetEarthResistance());
	TestEqual(TEXT("Element_Wind maps to WindResistance"),
		CDO->GetResistanceForElement(ElemagicGameplayTags::Element_Wind), CDO->GetWindResistance());
	TestEqual(TEXT("Element_Lightning maps to LightningResistance"),
		CDO->GetResistanceForElement(ElemagicGameplayTags::Element_Lightning), CDO->GetLightningResistance());
	TestEqual(TEXT("Element_Light maps to LightResistance"),
		CDO->GetResistanceForElement(ElemagicGameplayTags::Element_Light), CDO->GetLightResistance());
	TestEqual(TEXT("Element_Dark maps to DarkResistance"),
		CDO->GetResistanceForElement(ElemagicGameplayTags::Element_Dark), CDO->GetDarkResistance());

	// 未知/无效 Tag 应返回 0
	FGameplayTag UnknownTag;
	TestEqual(TEXT("Unknown element returns 0"), CDO->GetResistanceForElement(UnknownTag), 0.f);

	return true;
}

// ---- AElemagicEnemyCharacter CDO 测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnemyCDOTest,
	"Elemagic.Element.EnemyCDO",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEnemyCDOTest::RunTest(const FString& Parameters)
{
	const AElemagicEnemyCharacter* CDO = GetDefault<AElemagicEnemyCharacter>();
	if (!TestNotNull(TEXT("AElemagicEnemyCharacter CDO exists"), CDO))
	{
		return false;
	}

	// 血条组件由构造函数创建
	TestNotNull(TEXT("HealthBarWidgetComponent exists"), CDO->HealthBarWidgetComponent.Get());
	TestNull(TEXT("HealthBarWidgetClass null by default (requires BP)"), CDO->HealthBarWidgetClass.Get());

	return true;
}

// ---- AElemagicElementOrb CDO 测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrbCDOTest,
	"Elemagic.Element.OrbCDO",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrbCDOTest::RunTest(const FString& Parameters)
{
	const AElemagicElementOrb* CDO = GetDefault<AElemagicElementOrb>();
	if (!TestNotNull(TEXT("AElemagicElementOrb CDO exists"), CDO))
	{
		return false;
	}

	TestNotNull(TEXT("PickupCollision exists"), CDO->PickupCollision.Get());
	TestNotNull(TEXT("Sprite exists"), CDO->Sprite.Get());
	TestFalse(TEXT("ElementTag invalid by default (set in BP)"), CDO->ElementTag.IsValid());

	if (CDO->PickupCollision)
	{
		// 拾取碰撞盒对 Pawn 重叠
		TestEqual(TEXT("PickupCollision responds Overlap to Pawn"),
			static_cast<int32>(CDO->PickupCollision->GetCollisionResponseToChannel(ECC_Pawn)),
			static_cast<int32>(ECR_Overlap));
	}

	return true;
}

#endif // WITH_AUTOMATION_TESTS
