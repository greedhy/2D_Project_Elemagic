// Fill out your copyright notice in the Description page of Project Settings.

#include "CGF_Projectile.h"
#include "ProjectileBase.h"
#include "CharacterBase.h"
#include "ElemagicGameplayTags.h"
#include "AbilitySystemComponent.h"
#include "Engine/World.h"
#include "PaperFlipbook.h"
#include "PaperFlipbookComponent.h"
#include "GameFramework/Character.h"
#include "TimerManager.h"

UCGF_Projectile::UCGF_Projectile()
{
	// 发射投射物期间占据 State_Attacking，阻止其他攻击
	ActivationOwnedTags.AddTag(ElemagicGameplayTags::State_Attacking);
	ActivationBlockedTags.AddTag(ElemagicGameplayTags::State_Attacking);
}

void UCGF_Projectile::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// 验证必要配置
	if (!ProjectileClass || !DamageEffectClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("CGF_Projectile::ActivateAbility - ProjectileClass or DamageEffectClass is null, ending ability"));
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// 获取 Avatar Actor
	AActor* Avatar = ActorInfo ? ActorInfo->AvatarActor.Get() : nullptr;
	if (!Avatar)
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	ACharacterBase* CharBase = Cast<ACharacterBase>(Avatar);

	// 确定角色朝向
	const float DirSign = (CharBase && CharBase->IsFacingRight()) ? 1.f : -1.f;

	// 计算生成位置（2D 偏移 → 3D 世界坐标）
	const FVector OwnerLocation = Avatar->GetActorLocation();
	const FVector SpawnLocation = OwnerLocation + FVector(SpawnOffset.X * DirSign, 0.f, SpawnOffset.Y);

	// 生成投射物
	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = Avatar;
	SpawnParams.Instigator = Cast<APawn>(Avatar);
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AProjectileBase* Projectile = World->SpawnActor<AProjectileBase>(
		ProjectileClass, SpawnLocation, FRotator::ZeroRotator, SpawnParams);

	if (!Projectile)
	{
		UE_LOG(LogTemp, Warning, TEXT("CGF_Projectile::ActivateAbility - Failed to spawn projectile"));
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// 根据角色朝向翻转水平速度
	FVector2D FinalVelocity = LaunchVelocity;
	FinalVelocity.X *= DirSign;

	// 配置投射物参数
	Projectile->InitializeProjectile(
		DamageEffectClass,
		DamageMultiplier,
		HitImpulse,
		FinalVelocity,
		GravityScale,
		ProjectileLifespan
	);

	// 播放发射动画（如果有）
	if (FireAnimation && CharBase)
	{
		if (UPaperFlipbookComponent* SpriteComp = CharBase->GetSprite())
		{
			SpriteComp->SetFlipbook(FireAnimation);
			SpriteComp->PlayFromStart();
		}
	}

	// 设 FireEndTimer：发射动画播完后结束 GA
	const float FireDuration = FireAnimation
		? FireAnimation->GetTotalDuration()
		: 0.1f;

	World->GetTimerManager().SetTimer(FireEndTimer,
		[this, Handle, ActorInfo, ActivationInfo]()
		{
			EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
		},
		FireDuration, false);
}

void UCGF_Projectile::EndAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	bool bReplicateCancel, bool bEndedByCancel)
{
	// 清理定时器
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(FireEndTimer);
	}

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateCancel, bEndedByCancel);
}
