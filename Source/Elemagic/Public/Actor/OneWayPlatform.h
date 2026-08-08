// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OneWayPlatform.generated.h"

class UBoxComponent;
class UPaperSpriteComponent;
class ACharacter;
class UPrimitiveComponent;

/**
 * 单向平台基类:玩家(及以后的敌人)可以从下方跳穿,落到上方后能被正常支撑站立。
 * 具体外观(木板、石台等)都是本类的蓝图子类,只换 Sprite 贴图,不动这里的逻辑——
 * 以后所有单向平台都应该继承这个类,不要另开平行实现。
 */
UCLASS()
class ELEMAGIC_API AOneWayPlatform : public AActor
{
	GENERATED_BODY()

public:
	AOneWayPlatform();

	virtual void Tick(float DeltaTime) override;

	// 纯函数,不依赖 UWorld,方便单独做自动化测试。
	// 角色脚底已到达/高于平台顶面时(不论速度方向)永远不穿透,保证落地后能被稳定支撑;
	// 只有在平台下方且正在上升时才允许穿透。
	static bool ShouldPassThroughPlatform(float CharacterFeetZ, float PlatformTopZ, float CharacterVelocityZ);

protected:
	virtual void BeginPlay() override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Elemagic|Platform")
	TObjectPtr<UBoxComponent> CollisionBox;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Elemagic|Platform")
	TObjectPtr<UPaperSpriteComponent> Sprite;

	UFUNCTION()
	void OnPlatformEndOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex);

private:
	float GetPlatformTopZ() const;
};
