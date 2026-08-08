// Fill out your copyright notice in the Description page of Project Settings.

#include "Actor/OneWayPlatform.h"
#include "Components/BoxComponent.h"
#include "PaperSpriteComponent.h"
#include "GameFramework/Character.h"
#include "Components/CapsuleComponent.h"

AOneWayPlatform::AOneWayPlatform()
{
	PrimaryActorTick.bCanEverTick = true;

	CollisionBox = CreateDefaultSubobject<UBoxComponent>(TEXT("CollisionBox"));
	RootComponent = CollisionBox;
	CollisionBox->SetBoxExtent(FVector(50.f, 50.f, 10.f));
	CollisionBox->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);

	// Block 和 Overlap 对同一对组件是互斥的(见头文件注释),CollisionBox 是 Block 就永远不会
	// 触发重叠事件,所以另开一个纯检测用的 Overlap 组件,范围比 CollisionBox 上下都多探出一截。
	constexpr float DetectionMarginZ = 60.f;
	DetectionBox = CreateDefaultSubobject<UBoxComponent>(TEXT("DetectionBox"));
	DetectionBox->SetupAttachment(RootComponent);
	DetectionBox->SetBoxExtent(FVector(50.f, 50.f, 10.f + DetectionMarginZ));
	DetectionBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	DetectionBox->SetCollisionResponseToAllChannels(ECR_Ignore);
	DetectionBox->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	DetectionBox->SetGenerateOverlapEvents(true);

	Sprite = CreateDefaultSubobject<UPaperSpriteComponent>(TEXT("Sprite"));
	Sprite->SetupAttachment(RootComponent);
	Sprite->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

bool AOneWayPlatform::ShouldPassThroughPlatform(float CharacterFeetZ, float PlatformTopZ, float CharacterVelocityZ)
{
	if (CharacterFeetZ >= PlatformTopZ)
	{
		return false;
	}
	return CharacterVelocityZ > 0.f;
}

float AOneWayPlatform::GetPlatformTopZ() const
{
	return CollisionBox->GetComponentLocation().Z + CollisionBox->GetScaledBoxExtent().Z;
}

void AOneWayPlatform::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	TArray<AActor*> OverlappingActors;
	DetectionBox->GetOverlappingActors(OverlappingActors, ACharacter::StaticClass());

	const float PlatformTopZ = GetPlatformTopZ();

	for (AActor* OverlappingActor : OverlappingActors)
	{
		ACharacter* Character = Cast<ACharacter>(OverlappingActor);
		UCapsuleComponent* Capsule = Character ? Character->GetCapsuleComponent() : nullptr;
		if (!Capsule)
		{
			continue;
		}

		const float FeetZ = Capsule->GetComponentLocation().Z - Capsule->GetScaledCapsuleHalfHeight();
		const float VelocityZ = Character->GetVelocity().Z;
		const bool bShouldPass = ShouldPassThroughPlatform(FeetZ, PlatformTopZ, VelocityZ);

		// 注意:这里必须是"组件级"忽略(IgnoreComponentWhenMoving 只忽略 CollisionBox),
		// 不能用"Actor 级"忽略(IgnoreActorWhenMoving 会让角色对这个 Actor 的所有组件都失明,
		// 包括 DetectionBox 自己)。实测过 Actor 级忽略的后果:一旦角色开始穿透,DetectionBox
		// 的重叠检测对它也一起失效,Tick 从此再也检测不到这个角色,忽略状态永远撤不回来,
		// 角色会直接从平台底下一路掉穿到底。组件级忽略只影响 CollisionBox,DetectionBox
		// 全程保持追踪,才能在角色到达平台顶面时正常把忽略状态改回来、恢复支撑。
		Capsule->IgnoreComponentWhenMoving(CollisionBox, bShouldPass);
	}
}
