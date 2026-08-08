// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OneWayPlatform.generated.h"

class UBoxComponent;
class UPaperSpriteComponent;
class ACharacter;

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
	// 真正提供物理支撑/阻挡的碰撞体,对 Pawn 是 Block——正因为是 Block,
	// 它永远不会跟角色产生"重叠"(UE 里 Block 和 Overlap 对同一对组件是互斥的,
	// 见 PrimitiveComponent.cpp 的 CanComponentsGenerateOverlap),所以不能用它来驱动穿透判定。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Elemagic|Platform")
	TObjectPtr<UBoxComponent> CollisionBox;

	// 纯检测用的重叠体,对 Pawn 是 Overlap,范围比 CollisionBox 上下都多探出一截,
	// 保证角色从下方靠近时先进入这个区域触发重叠事件,再真正撞上 CollisionBox 之前
	// Tick 就已经有机会把平台加进它的 MoveIgnoreActors。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Elemagic|Platform")
	TObjectPtr<UBoxComponent> DetectionBox;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Elemagic|Platform")
	TObjectPtr<UPaperSpriteComponent> Sprite;


private:
	float GetPlatformTopZ() const;
};
