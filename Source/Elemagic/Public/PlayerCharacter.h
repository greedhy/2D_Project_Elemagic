// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "CharacterBase.h"
#include "PlayerCharacter.generated.h"

class USpringArmComponent;
class UCameraComponent;
class UGameplayAbility;

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

	virtual void BeginPlay() override;
	virtual void PossessedBy(AController* NewController) override;

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Elemagic|Camera")
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Elemagic|Camera")
	TObjectPtr<UCameraComponent> SideViewCamera;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Elemagic|Abilities")
	TSubclassOf<UGameplayAbility> AttackAbilityClass;

public:
	// 移动参数以蓝图中设置的为准；构造里写入 CharacterMovementComponent 的初始值。
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Elemagic|Movement")
	float MoveSpeed = 600.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Elemagic|Movement")
	float JumpVelocity = 700.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Elemagic|Movement")
	float JumpHoldTime = 0.3f;

	// 最大跳跃次数:默认 2 开局即可二段跳,运行时可被 GA 动态修改(如先解锁再赋更高值)。
	// 以蓝图子类 Class Defaults 设置的值为准。
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Elemagic|Movement")
	int32 JumpMaxCount = 2;
};
