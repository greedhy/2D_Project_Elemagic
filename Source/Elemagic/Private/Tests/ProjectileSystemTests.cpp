// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "ElemagicDamageStatics.h"
#include "CGF_Projectile.h"
#include "ProjectileBase.h"
#include "ElemagicGameplayTags.h"
#include "Components/BoxComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "PaperFlipbookComponent.h"

// ---- UElemagicDamageStatics CDO 测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDamageStaticsDefaultsTest,
	"Elemagic.Projectile.DamageStaticsDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDamageStaticsDefaultsTest::RunTest(const FString& Parameters)
{
	const UElemagicDamageStatics* CDO = GetDefault<UElemagicDamageStatics>();
	TestNotNull(TEXT("UElemagicDamageStatics CDO exists"), CDO);
	return true;
}

// ---- UCGF_Projectile CDO 默认值测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGFProjectileDefaultsTest,
	"Elemagic.Projectile.CGFProjectileDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGFProjectileDefaultsTest::RunTest(const FString& Parameters)
{
	const UCGF_Projectile* CDO = GetDefault<UCGF_Projectile>();
	if (!TestNotNull(TEXT("UCGF_Projectile CDO exists"), CDO))
	{
		return false;
	}

	// 默认值验证
	TestNull(TEXT("Default ProjectileClass is null (requires BP subclass)"), CDO->ProjectileClass.Get());
	TestNull(TEXT("Default DamageEffectClass is null (requires BP subclass)"), CDO->DamageEffectClass.Get());
	TestEqual(TEXT("Default LaunchVelocity is (800, 0)"), CDO->LaunchVelocity, FVector2D(800.f, 0.f));
	TestEqual(TEXT("Default GravityScale is 0"), CDO->GravityScale, 0.f);
	TestEqual(TEXT("Default DamageMultiplier is 1.0"), CDO->DamageMultiplier, 1.f);
	TestEqual(TEXT("Default SpawnOffset is (64, 0)"), CDO->SpawnOffset, FVector2D(64.f, 0.f));
	TestEqual(TEXT("Default HitImpulse is (200, 100)"), CDO->HitImpulse, FVector2D(200.f, 100.f));
	TestEqual(TEXT("Default ProjectileLifespan is 5.0"), CDO->ProjectileLifespan, 5.f);

	// 注意: ActivationOwnedTags / ActivationBlockedTags 是 UGameplayAbility 的 protected 成员，
	// 无法在外部测试中直接访问。State_Attacking 的正确性由 CGF_Damage 的现有测试间接覆盖。

	return true;
}

// ---- AProjectileBase CDO 子对象测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAProjectileBaseDefaultsTest,
	"Elemagic.Projectile.ProjectileBaseDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAProjectileBaseDefaultsTest::RunTest(const FString& Parameters)
{
	const AProjectileBase* CDO = GetDefault<AProjectileBase>();
	if (!TestNotNull(TEXT("AProjectileBase CDO exists"), CDO))
	{
		return false;
	}

	// 默认属性
	TestTrue(TEXT("Default bDestroyOnHit is true"), CDO->bDestroyOnHit);

	// 子对象必须由构造函数创建
	TestNotNull(TEXT("CollisionBox exists"), CDO->CollisionBox.Get());
	TestNotNull(TEXT("ProjectileMovement exists"), CDO->ProjectileMovement.Get());
	TestNotNull(TEXT("Sprite exists"), CDO->Sprite.Get());

	if (CDO->ProjectileMovement.Get())
	{
		TestTrue(TEXT("bConstrainToPlane is true"), CDO->ProjectileMovement->bConstrainToPlane);
		TestEqual(TEXT("PlaneConstraintNormal is (0,1,0)"), CDO->ProjectileMovement->GetPlaneConstraintNormal(), FVector(0.f, 1.f, 0.f));
		TestEqual(TEXT("Default ProjectileGravityScale is 0"), CDO->ProjectileMovement->ProjectileGravityScale, 0.f);
	}

	return true;
}

// ---- AProjectileBase 碰撞配置测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectileCollisionConfigTest,
	"Elemagic.Projectile.CollisionConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FProjectileCollisionConfigTest::RunTest(const FString& Parameters)
{
	const AProjectileBase* CDO = GetDefault<AProjectileBase>();
	if (!TestNotNull(TEXT("AProjectileBase CDO exists"), CDO))
	{
		return false;
	}

	const UBoxComponent* Box = CDO->CollisionBox.Get();
	if (!TestNotNull(TEXT("CollisionBox exists"), Box))
	{
		return false;
	}

	// 碰撞通道配置
	TestEqual(TEXT("CollisionObjectType is ECC_GameTraceChannel1 (Hitbox)"),
		static_cast<int32>(Box->GetCollisionObjectType()),
		static_cast<int32>(ECC_GameTraceChannel1));

	// Hurtbox 通道响应为 Overlap
	TestEqual(TEXT("CollisionResponse to ECC_GameTraceChannel2 (Hurtbox) is Overlap"),
		static_cast<int32>(Box->GetCollisionResponseToChannel(ECC_GameTraceChannel2)),
		static_cast<int32>(ECR_Overlap));

	// 其他所有通道应忽略
	TestEqual(TEXT("CollisionResponse to WorldStatic is Ignore"),
		static_cast<int32>(Box->GetCollisionResponseToChannel(ECC_WorldStatic)),
		static_cast<int32>(ECR_Ignore));

	TestEqual(TEXT("CollisionResponse to Pawn is Ignore"),
		static_cast<int32>(Box->GetCollisionResponseToChannel(ECC_Pawn)),
		static_cast<int32>(ECR_Ignore));

	// RootComponent 是 CollisionBox
	TestTrue(TEXT("RootComponent is CollisionBox"), CDO->GetRootComponent() == Box);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
