// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AttackFrameData.h"
#include "CGF_Damage.h"
#include "ElemagicGameplayTags.h"
#include "CharacterAttributeSetBase.h"

// ---- FAttackFrameConfig 结构体默认值测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFrameConfigDefaultsTest,
    "Elemagic.DamageForm.FrameConfigDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFrameConfigDefaultsTest::RunTest(const FString& Parameters)
{
    FAttackFrameConfig Config;
    TestEqual(TEXT("Default NormalizedTime is 0"), Config.NormalizedTime, 0.f);
    TestEqual(TEXT("Default HitboxExtent is (32,32)"), Config.HitboxExtent, FVector2D(32.f, 32.f));
    TestEqual(TEXT("Default HitboxOffset is (0,0)"), Config.HitboxOffset, FVector2D(0.f, 0.f));
    TestEqual(TEXT("Default DamageMultiplier is 1.0"), Config.DamageMultiplier, 1.f);
    TestEqual(TEXT("Default HitImpulse is (0,0)"), Config.HitImpulse, FVector2D::ZeroVector);
    TestTrue(TEXT("Default EventTags is empty"), Config.EventTags.IsEmpty());
    return true;
}

// ---- UAttackFrameData DataAsset CDO 测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFrameDataDefaultTest,
    "Elemagic.DamageForm.FrameDataDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFrameDataDefaultTest::RunTest(const FString& Parameters)
{
    const UAttackFrameData* CDO = GetDefault<UAttackFrameData>();
    if (!TestNotNull(TEXT("UAttackFrameData CDO exists"), CDO))
    {
        return false;
    }

    TestTrue(TEXT("Default Frames array is empty"), CDO->Frames.Num() == 0);
    TestTrue(TEXT("Default SourceAnimation is null"), CDO->SourceAnimation.IsNull());
    TestTrue(TEXT("GetTotalDuration() returns fallback > 0"), CDO->GetTotalDuration() > 0.f);
    return true;
}

// ---- UCGF_Damage CDO 标签测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGFDamageDefaultsTest,
    "Elemagic.DamageForm.CGFDamageDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGFDamageDefaultsTest::RunTest(const FString& Parameters)
{
    const UCGF_Damage* CDO = GetDefault<UCGF_Damage>();
    if (!TestNotNull(TEXT("UCGF_Damage CDO exists"), CDO))
    {
        return false;
    }

    TestFalse(TEXT("Framework CDO should NOT have Ability.Attack.Light tag (set by subclass)"),
        CDO->GetAssetTags().HasTagExact(ElemagicGameplayTags::Ability_Attack_Light));

    return true;
}

// ---- UCGF_Damage 配置属性默认值测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGFDamageConfigDefaultsTest,
    "Elemagic.DamageForm.CGFDamageConfigDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGFDamageConfigDefaultsTest::RunTest(const FString& Parameters)
{
    const UCGF_Damage* CDO = GetDefault<UCGF_Damage>();
    if (!TestNotNull(TEXT("UCGF_Damage CDO exists"), CDO))
    {
        return false;
    }

    TestTrue(TEXT("AttackAnimation is null by default"), CDO->AttackAnimation == nullptr);
    TestTrue(TEXT("FrameData is null by default"), CDO->FrameData == nullptr);
    TestTrue(TEXT("DamageEffectClass is null by default"), CDO->DamageEffectClass == nullptr);
    TestEqual(TEXT("Default BaseImpulse is (0,0)"), CDO->BaseImpulse, FVector2D::ZeroVector);
    return true;
}

// ---- IncomingDamage 属性管道默认值测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIncomingDamageAttrTest,
    "Elemagic.DamageForm.IncomingDamageAttr",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FIncomingDamageAttrTest::RunTest(const FString& Parameters)
{
    const UCharacterAttributeSetBase* CDO = GetDefault<UCharacterAttributeSetBase>();
    if (!TestNotNull(TEXT("AttributeSet CDO exists"), CDO))
    {
        return false;
    }

    TestEqual(TEXT("Default IncomingDamage is 0"), CDO->GetIncomingDamage(), 0.f);
    TestEqual(TEXT("Default AttackPower is 10"), CDO->GetAttackPower(), 10.f);
    TestEqual(TEXT("Default Health is 100"), CDO->GetHealth(), 100.f);
    TestEqual(TEXT("Default MaxHealth is 100"), CDO->GetMaxHealth(), 100.f);

    return true;
}

#endif // WITH_AUTOMATION_TESTS
