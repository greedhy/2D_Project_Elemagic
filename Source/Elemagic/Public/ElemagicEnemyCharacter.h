// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "CharacterBase.h"
#include "UI/ElemagicUserWidget.h"
#include "ElemagicEnemyCharacter.generated.h"

class UWidgetComponent;
class UElemagicAttributeViewModel;

/**
 * 敌人基类：继承 ACharacterBase，头顶显示生命值条。
 * 敌人放置于关卡中无 Controller，PossessedBy 不触发，故在 BeginPlay 手动初始化 ASC。
 * AI 控制器留待后续阶段。
 */
UCLASS()
class ELEMAGIC_API AElemagicEnemyCharacter : public ACharacterBase
{
	GENERATED_BODY()

public:
	AElemagicEnemyCharacter();

	virtual void BeginPlay() override;

	// 头顶血条 Widget 组件（Screen 空间）
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Elemagic|UI")
	TObjectPtr<UWidgetComponent> HealthBarWidgetComponent;

	// 血条 Widget 类（BP 子类，父类 UElemagicUserWidget）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Elemagic|UI")
	TSubclassOf<UElemagicUserWidget> HealthBarWidgetClass;

private:
	UPROPERTY(Transient)
	TObjectPtr<UElemagicAttributeViewModel> HealthViewModel;

	bool bAbilityActorInfoInitialized = false;
};
