// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "ElemagicElementOrb.generated.h"

class UBoxComponent;
class UPaperFlipbookComponent;

/**
 * 元素球拾取物：玩家触碰后把元素加入装载槽。
 * 使用标准 Pawn 碰撞通道（与战斗 Hitbox/Hurtbox 分离）。
 */
UCLASS()
class ELEMAGIC_API AElemagicElementOrb : public AActor
{
	GENERATED_BODY()

public:
	AElemagicElementOrb();

	virtual void BeginPlay() override;

	// 本球代表的元素
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Element")
	FGameplayTag ElementTag;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Element")
	TObjectPtr<UBoxComponent> PickupCollision;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Element")
	TObjectPtr<UPaperFlipbookComponent> Sprite;

protected:
	UFUNCTION()
	void OnPickupOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);
};
