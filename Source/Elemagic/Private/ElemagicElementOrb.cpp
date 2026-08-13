// Fill out your copyright notice in the Description page of Project Settings.

#include "ElemagicElementOrb.h"
#include "ElementSystemComponent.h"
#include "Components/BoxComponent.h"
#include "PaperFlipbookComponent.h"

AElemagicElementOrb::AElemagicElementOrb()
{
	// 拾取碰撞盒：WorldDynamic，仅与 Pawn 重叠
	PickupCollision = CreateDefaultSubobject<UBoxComponent>(TEXT("PickupCollision"));
	PickupCollision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	PickupCollision->SetCollisionObjectType(ECC_WorldDynamic);
	PickupCollision->SetCollisionResponseToAllChannels(ECR_Ignore);
	PickupCollision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	PickupCollision->SetBoxExtent(FVector(16.f, 8.f, 16.f));
	PickupCollision->SetGenerateOverlapEvents(true);
	SetRootComponent(PickupCollision);

	// 视觉 Sprite（纯显示）
	Sprite = CreateDefaultSubobject<UPaperFlipbookComponent>(TEXT("Sprite"));
	Sprite->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Sprite->SetupAttachment(PickupCollision);
}

void AElemagicElementOrb::BeginPlay()
{
	Super::BeginPlay();
	PickupCollision->OnComponentBeginOverlap.AddDynamic(this, &AElemagicElementOrb::OnPickupOverlap);
}

void AElemagicElementOrb::OnPickupOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	if (!OtherActor || !ElementTag.IsValid())
	{
		return;
	}

	// 只有带元素系统组件的角色（玩家）能拾取
	UElementSystemComponent* ElementSystem = OtherActor->FindComponentByClass<UElementSystemComponent>();
	if (ElementSystem && ElementSystem->AddElement(ElementTag))
	{
		Destroy();
	}
}
