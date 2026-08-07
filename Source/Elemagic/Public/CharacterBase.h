// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "PaperCharacter.h"
#include "AbilitySystemInterface.h"
#include "CharacterBase.generated.h"

class UAbilitySystemComponent;
class UCharacterAttributeSetBase;
class UPaperFlipbook;
struct FOnAttributeChangeData;

/**
 * 2D 横版角色公共基类(玩家与敌人共用)。
 * 使用 Paper2D 的 PaperFlipbookComponent 做序列帧动画显示,
 * 角色运动被约束在 X-Z 平面内(锁定 Y 轴深度),配合 GAS 处理属性/技能/死亡。
 */
UCLASS()
class ELEMAGIC_API ACharacterBase : public APaperCharacter, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	ACharacterBase();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void PossessedBy(AController* NewController) override;

	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;

	UFUNCTION(BlueprintPure, Category = "Elemagic|Character")
	bool IsDead() const;

	// 纯函数,不依赖 UWorld,方便单独做自动化测试。
	// 优先级:下落(有 JumpFlipbook 才用)> 移动(有 RunFlipbook 才用)> 待机。
	static UPaperFlipbook* SelectFlipbookForState(bool bIsFalling, bool bIsMoving,
		UPaperFlipbook* IdleFlipbook, UPaperFlipbook* RunFlipbook, UPaperFlipbook* JumpFlipbook);

	// 待美术资源完善前的临时序列帧,先用现有素材跑通表现层
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Elemagic|Animation")
	TObjectPtr<UPaperFlipbook> IdleFlipbook;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Elemagic|Animation")
	TObjectPtr<UPaperFlipbook> RunFlipbook;

	// 目前还没有专门的跳跃/下落美术,先留空;SelectFlipbookForState 会在未赋值时自动退回 RunFlipbook。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Elemagic|Animation")
	TObjectPtr<UPaperFlipbook> JumpFlipbook;

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Elemagic|Abilities")
	TObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Elemagic|Abilities")
	TObjectPtr<UCharacterAttributeSetBase> AttributeSet;

	virtual void InitializeAttributes();
	virtual void OnHealthChanged(const FOnAttributeChangeData& Data);

	UFUNCTION(BlueprintNativeEvent, Category = "Elemagic|Character")
	void Die();
	virtual void Die_Implementation();

	void UpdateAnimation();
	void UpdateFacing(float MoveDirectionX);

private:
	bool bFacingRight = true;
};
