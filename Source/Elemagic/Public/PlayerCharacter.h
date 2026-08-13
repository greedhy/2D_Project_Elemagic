// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "CharacterBase.h"
#include "PlayerCharacter.generated.h"

class USpringArmComponent;
class UCameraComponent;
class UGameplayAbility;
class UElementSystemComponent;
class UInventoryComponent;

/**
 * 玩家可控角色:侧视 2D 横版相机。
 * 所有 Input 资源(Mapping Context / Input Action)均由 AMyPlayerController 持有和绑定,
 * 本类不直接引用任何 Input 资源,只负责相机与自身技能授予。
 */
UCLASS()
class ELEMAGIC_API APlayerCharacter : public ACharacterBase
{
	GENERATED_BODY()

public:
	APlayerCharacter();

	virtual void PossessedBy(AController* NewController) override;
	virtual void PostInitializeComponents() override;
	virtual void Landed(const FHitResult& Hit) override;
	virtual void OnMovementModeChanged(EMovementMode PrevMovementMode, uint8 PreviousCustomMode) override;


	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Elemagic|Camera")
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Elemagic|Camera")
	TObjectPtr<UCameraComponent> SideViewCamera;

	// 元素系统（3 槽装载 + 4 槽技能栏 + 合成）
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Elemagic|Element")
	TObjectPtr<UElementSystemComponent> ElementSystemComponent;

	// 背包/物品列表
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Elemagic|Inventory")
	TObjectPtr<UInventoryComponent> InventoryComponent;

	// 开局授予的能力:在 BP 子类的 Class Defaults 中向此数组添加 GA 类即可,
	// PossessedBy 时遍历数组逐一授予。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Elemagic|Abilities")
	TArray<TSubclassOf<UGameplayAbility>> StartupAbilities;

	// 移动参数以蓝图中设置的为准；构造里写入 CharacterMovementComponent 的初始值。
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Elemagic|Movement")
	float MoveSpeed = 600.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Elemagic|Movement")
	float JumpVelocity = 700.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Elemagic|Movement")
	float JumpHoldTime = 0.3f;

	// 最大跳跃次数:默认 2 开局即可二段跳,运行时可被 GA 动态修改。
	// 以蓝图子类 Class Defaults 设置的值为准。
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Elemagic|Movement")
	int32 MaxJumpCount = 2;
};
