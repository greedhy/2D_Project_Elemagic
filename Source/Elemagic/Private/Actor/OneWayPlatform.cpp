// Fill out your copyright notice in the Description page of Project Settings.

#include "Actor/OneWayPlatform.h"
#include "Components/BoxComponent.h"
#include "PaperSpriteComponent.h"

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
	// Task 2 补上重叠角色的 MoveIgnoreActors 切换逻辑。
}

void AOneWayPlatform::OnPlatformEndOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex)
{
	// Task 2 补上离开重叠范围时的清理逻辑。
}
