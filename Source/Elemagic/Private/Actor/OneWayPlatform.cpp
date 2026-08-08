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

	DetectionBox = CreateDefaultSubobject<UBoxComponent>(TEXT("DetectionBox"));
	DetectionBox->SetupAttachment(RootComponent);
	DetectionBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	DetectionBox->SetCollisionResponseToAllChannels(ECR_Ignore);
	DetectionBox->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	DetectionBox->SetGenerateOverlapEvents(true);
	SyncDetectionBoxToCollisionBox();

	Sprite = CreateDefaultSubobject<UPaperSpriteComponent>(TEXT("Sprite"));
	Sprite->SetupAttachment(RootComponent);
	Sprite->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void AOneWayPlatform::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// BP 子类在编辑器里改了 CollisionBox 的 Box Extent(比如做一块更宽的木板)时,
	// 这里保证 DetectionBox 立刻跟着重新对齐,不需要额外手动同步。
	SyncDetectionBoxToCollisionBox();
}

void AOneWayPlatform::SyncDetectionBoxToCollisionBox()
{
	const FVector CollisionExtent = CollisionBox->GetUnscaledBoxExtent();
	DetectionBox->SetBoxExtent(FVector(CollisionExtent.X, CollisionExtent.Y, CollisionExtent.Z + DetectionMarginZ));
}

bool AOneWayPlatform::ShouldPassThroughPlatform(float CharacterFeetZ, float PlatformTopZ, float CharacterVelocityZ, bool bWasPassingThrough)
{
	if (bWasPassingThrough)
	{
		return CharacterFeetZ < PlatformTopZ;
	}
	if (CharacterFeetZ >= PlatformTopZ)
	{
		return false;
	}
	return CharacterVelocityZ > 0.f;
}

float AOneWayPlatform::GetPlatformTopZ() const
{
	return CollisionBox->Bounds.Origin.Z + CollisionBox->Bounds.BoxExtent.Z;
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
		const bool bWasPassingThrough = Capsule->CopyArrayOfMoveIgnoreComponents().Contains(CollisionBox);
		const bool bShouldPass = ShouldPassThroughPlatform(FeetZ, PlatformTopZ, VelocityZ, bWasPassingThrough);

		// 组件级忽略(只摘掉 CollisionBox,见头文件注释)——不能用 Actor 级的
		// IgnoreActorWhenMoving/MoveIgnoreActors,那会连 DetectionBox 一起变得对角色不可见。
		Capsule->IgnoreComponentWhenMoving(CollisionBox, bShouldPass);
	}
}
