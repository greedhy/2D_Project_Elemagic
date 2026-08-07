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

	// 待美术资源完善前的临时序列帧,先用现有素材跑通表现层
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Elemagic|Animation")
	TObjectPtr<UPaperFlipbook> IdleFlipbook;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Elemagic|Animation")
	TObjectPtr<UPaperFlipbook> RunFlipbook;

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
