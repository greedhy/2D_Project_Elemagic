// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OneWayPlatform.generated.h"

class UBoxComponent;
class UPaperSpriteComponent;

/**
 * 单向平台基类:玩家(及以后的敌人)可以从下方跳穿,落到上方后能被正常支撑站立。
 * 具体外观(木板、石台等)都是本类的蓝图子类,只换 Sprite 贴图,不动这里的逻辑——
 * 以后所有单向平台都应该继承这个类,不要另开平行实现。
 *
 * BP 子类如果改了 CollisionBox 的 Box Extent(比如做一块更宽的木板),不需要手动
 * 同步 DetectionBox——OnConstruction/BeginPlay 会自动把 DetectionBox 的 X/Y 对齐到
 * CollisionBox 当前的实际尺寸,只在 Z 方向加上 DetectionMarginZ。
 */
UCLASS()
class ELEMAGIC_API AOneWayPlatform : public AActor
{
	GENERATED_BODY()

public:
	AOneWayPlatform();

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void Tick(float DeltaTime) override;

	// 纯函数,不依赖 UWorld,方便单独做自动化测试。
	// bWasPassingThrough 是角色"上一帧是否已经在穿透这块平台"的状态:
	// - 已经在穿透中:只看角色脚底是否真正越过了顶面,不管速度方向——避免角色在跳跃
	//   最高点恰好卡在 CollisionBox 内部、速度过零那一帧被判定为"不该穿透"从而被
	//   引擎的解穿插逻辑弹飞或卡死。
	// - 还没开始穿透:维持原规则——脚底已到达/高于平台顶面时永远不穿透(保证落地后能
	//   被稳定支撑),只有在平台下方且正在上升时才允许开始穿透。
	static bool ShouldPassThroughPlatform(float CharacterFeetZ, float PlatformTopZ, float CharacterVelocityZ, bool bWasPassingThrough);

protected:
	// 真正提供物理支撑/阻挡的碰撞体,对 Pawn 是 Block——正因为是 Block,
	// 它永远不会跟角色产生"重叠"(UE 里两个组件的碰撞响应取双向 min,
	// 见 PrimitiveComponent.cpp 的 CanComponentsGenerateOverlap 与
	// SceneComponent.cpp 的 GetCollisionResponseToComponent;CollisionBox 对角色是 Block、
	// 角色对 CollisionBox 的 WorldDynamic 默认也是 Block,min 仍是 Block),
	// 所以不能用它来驱动穿透判定。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Elemagic|Platform")
	TObjectPtr<UBoxComponent> CollisionBox;

	// 纯检测用的重叠体,对 Pawn 是 Overlap,范围比 CollisionBox 上下都多探出 DetectionMarginZ,
	// 保证角色从下方靠近时先进入这个区域触发重叠事件,再真正撞上 CollisionBox 之前
	// Tick 就已经有机会用 IgnoreComponentWhenMoving 把 CollisionBox 对这个角色设成忽略。
	//
	// 必须用组件级忽略(IgnoreComponentWhenMoving 只摘掉 CollisionBox),不能用 Actor 级忽略
	// (IgnoreActorWhenMoving/MoveIgnoreActors 会让角色对这个 Actor 的所有组件都失明,
	// 包括 DetectionBox 自己——一旦角色开始穿透就再也检测不到它,忽略状态永远撤不回来,
	// 角色会直接掉穿到底)。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Elemagic|Platform")
	TObjectPtr<UBoxComponent> DetectionBox;

	// DetectionBox 比 CollisionBox 在 Z 方向多探出的距离(单位:uu)。必须大于角色单帧内
	// 能达到的最大垂直位移,否则角色可能在一帧内就穿过整个检测带,导致忽略状态没机会被
	// Tick 清理(尤其是未来做冲刺/高速下落这类能力时需要相应调大)。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Elemagic|Platform")
	float DetectionMarginZ = 60.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Elemagic|Platform")
	TObjectPtr<UPaperSpriteComponent> Sprite;

	// 把 DetectionBox 的水平范围对齐到 CollisionBox 当前的实际尺寸,只在 Z 方向叠加
	// DetectionMarginZ。BP 子类改了 CollisionBox 的 Extent 后不需要手动同步 DetectionBox。
	void SyncDetectionBoxToCollisionBox();

	float GetPlatformTopZ() const;
};
