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
	CollisionBox->SetGenerateOverlapEvents(true);

	Sprite = CreateDefaultSubobject<UPaperSpriteComponent>(TEXT("Sprite"));
	Sprite->SetupAttachment(RootComponent);
	Sprite->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void AOneWayPlatform::BeginPlay()
{
	Super::BeginPlay();

	CollisionBox->OnComponentEndOverlap.AddDynamic(this, &AOneWayPlatform::OnPlatformEndOverlap);
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
	CollisionBox->GetOverlappingActors(OverlappingActors, ACharacter::StaticClass());

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

		if (ShouldPassThroughPlatform(FeetZ, PlatformTopZ, VelocityZ))
		{
			Capsule->MoveIgnoreActors.AddUnique(this);
		}
		else
		{
			Capsule->MoveIgnoreActors.Remove(this);
		}
	}
}

void AOneWayPlatform::OnPlatformEndOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex)
{
	// 保险丝:角色离开重叠范围时,不管 Tick 当时把它设成了穿透还是阻挡,
	// 都强制把这块平台从它的 MoveIgnoreActors 里摘掉,避免角色绕开平台后
	// 这块平台被永久标记为"忽略"、之后再也挡不住它。
	if (ACharacter* Character = Cast<ACharacter>(OtherActor))
	{
		if (UCapsuleComponent* Capsule = Character->GetCapsuleComponent())
		{
			Capsule->MoveIgnoreActors.Remove(this);
		}
	}
}
