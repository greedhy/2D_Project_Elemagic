// Fill out your copyright notice in the Description page of Project Settings.

#include "ProjectileBase.h"
#include "ElemagicDamageStatics.h"
#include "Components/BoxComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "PaperFlipbookComponent.h"
#include "Engine/World.h"

// DefaultEngine.ini 中定义的碰撞通道
// ECC_GameTraceChannel1 = Hitbox
// ECC_GameTraceChannel2 = Hurtbox
static constexpr ECollisionChannel ECC_Hitbox = ECC_GameTraceChannel1;
static constexpr ECollisionChannel ECC_Hurtbox = ECC_GameTraceChannel2;

AProjectileBase::AProjectileBase()
{
	bReplicates = true;

	// RootComponent：碰撞盒
	CollisionBox = CreateDefaultSubobject<UBoxComponent>(TEXT("CollisionBox"));
	CollisionBox->SetCollisionObjectType(ECC_Hitbox);
	CollisionBox->SetCollisionResponseToAllChannels(ECR_Ignore);
	CollisionBox->SetCollisionResponseToChannel(ECC_Hurtbox, ECR_Overlap);
	CollisionBox->SetBoxExtent(FVector(16.f, 8.f, 16.f));
	CollisionBox->SetGenerateOverlapEvents(true);
	SetRootComponent(CollisionBox);

	// 投射物移动组件
	ProjectileMovement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("ProjectileMovement"));
	ProjectileMovement->bConstrainToPlane = true;
	ProjectileMovement->SetPlaneConstraintNormal(FVector(0.f, 1.f, 0.f));  // X-Z 平面
	ProjectileMovement->bInitialVelocityInLocalSpace = false;
	ProjectileMovement->ProjectileGravityScale = 0.f;

	// 视觉 Sprite（纯显示，不参与碰撞）
	Sprite = CreateDefaultSubobject<UPaperFlipbookComponent>(TEXT("Sprite"));
	Sprite->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Sprite->SetupAttachment(CollisionBox);

	// 绑定重叠事件
	CollisionBox->OnComponentBeginOverlap.AddDynamic(this, &AProjectileBase::OnProjectileOverlap);
}

void AProjectileBase::BeginPlay()
{
	Super::BeginPlay();
}

void AProjectileBase::InitializeProjectile(
	TSubclassOf<UGameplayEffect> InDamageEffectClass,
	float InDamageMultiplier,
	FVector2D InHitImpulse,
	FVector2D InLaunchVelocity,
	float InGravityScale,
	float InLifespan)
{
	DamageEffectClass = InDamageEffectClass;
	DamageMultiplier = InDamageMultiplier;
	HitImpulse = InHitImpulse;

	// 设置飞行速度（2D → 3D：X=水平, Y=0 深度, Z=垂直）
	const FVector Velocity3D(InLaunchVelocity.X, 0.f, InLaunchVelocity.Y);
	ProjectileMovement->Velocity = Velocity3D;

	// 设置重力缩放
	ProjectileMovement->ProjectileGravityScale = InGravityScale;

	// 根据水平速度方向翻转 Sprite
	if (Velocity3D.X < 0.f)
	{
		Sprite->SetRelativeScale3D(FVector(-1.f, 1.f, 1.f));
	}
	else
	{
		Sprite->SetRelativeScale3D(FVector(1.f, 1.f, 1.f));
	}

	// 超时自毁
	if (InLifespan > 0.f && GetWorld())
	{
		GetWorld()->GetTimerManager().SetTimer(LifespanTimer,
			[this]()
			{
				Destroy();
			},
			InLifespan, false);
	}
}

void AProjectileBase::OnProjectileOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	UE_LOG(LogTemp, Log, TEXT("[Projectile] Overlap detected with: %s"), *GetNameSafe(OtherActor));

	// 仅服务端执行伤害判定
	if (!HasAuthority())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Projectile] Skip: no authority (client)"));
		return;
	}

	// 基本验证
	if (!OtherActor)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Projectile] Skip: OtherActor is null"));
		return;
	}

	if (!DamageEffectClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Projectile] Skip: DamageEffectClass is null - set it in the GA blueprint!"));
		return;
	}

	// 不自伤（不命中 Instigator）
	if (OtherActor == GetInstigator())
	{
		UE_LOG(LogTemp, Log, TEXT("[Projectile] Skip: hit own Instigator (%s)"), *GetNameSafe(GetInstigator()));
		return;
	}

	// 必须是 Hurtbox 碰撞通道
	if (!OtherComp)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Projectile] Skip: OtherComp is null"));
		return;
	}

	const ECollisionChannel OtherChannel = OtherComp->GetCollisionObjectType();
	if (OtherChannel != ECC_Hurtbox)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Projectile] Skip: OtherComp channel is %d (expected %d = Hurtbox). Overlapped actor: %s, component: %s"),
			static_cast<int32>(OtherChannel), static_cast<int32>(ECC_Hurtbox), *GetNameSafe(OtherActor), *OtherComp->GetName());
		return;
	}

	// 去重：同一飞行周期内不重复命中同一目标
	if (HitTargets.Contains(OtherActor))
	{
		UE_LOG(LogTemp, Log, TEXT("[Projectile] Skip: already hit %s this flight"), *GetNameSafe(OtherActor));
		return;
	}

	HitTargets.Add(OtherActor);

	// 击退方向：按投射物飞行方向
	const float DirectionSign = (ProjectileMovement->Velocity.X >= 0.f) ? 1.f : -1.f;

	UE_LOG(LogTemp, Log, TEXT("[Projectile] HIT! Target=%s, Instigator=%s, DamageMultiplier=%.2f, DirectionSign=%.0f"),
		*GetNameSafe(OtherActor), *GetNameSafe(GetInstigator()), DamageMultiplier, DirectionSign);

	// 施加伤害
	const float FinalDamage = UElemagicDamageStatics::ApplyDamageToTarget(
		GetInstigator(),          // SourceActor = 发射者
		OtherActor,
		DamageEffectClass,
		DamageMultiplier,
		HitImpulse,
		DirectionSign
	);

	UE_LOG(LogTemp, Log, TEXT("[Projectile] Damage applied: %.1f to %s"), FinalDamage, *GetNameSafe(OtherActor));

	// 命中即销毁
	if (bDestroyOnHit)
	{
		Destroy();
	}
}
