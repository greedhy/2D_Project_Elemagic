// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UI/ElemagicUserWidget.h"
#include "ElemagicHUDWidget.generated.h"

class UElemagicAttributeViewModel;
class UElemagicElementLoadoutViewModel;
class UElemagicSkillBarViewModel;
class UElemagicInventoryViewModel;
class UElemagicUserWidget;

/**
 * HUD 复合根 Widget：创建 ViewModel 并绑定到子 Widget。
 * 子 Widget 用 meta=(BindWidget) 在 BP 设计器中命名匹配。
 */
UCLASS()
class ELEMAGIC_API UElemagicHUDWidget : public UElemagicUserWidget
{
	GENERATED_BODY()

public:
	// 绑定到玩家：从 PlayerController 取 Pawn 的 ASC + 元素/背包组件，
	// 创建各 ViewModel 并绑定子 Widget。
	void BindToPlayer(APlayerController* PC);

protected:
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UElemagicUserWidget> HealthBarWidget;

	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UElemagicUserWidget> AttributeListWidget;

	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UElemagicUserWidget> ElementLoadoutWidget;

	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UElemagicUserWidget> SkillBarWidget;

	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UElemagicUserWidget> InventoryWidget;

	UPROPERTY(Transient)
	TObjectPtr<UElemagicAttributeViewModel> AttributeViewModel;

	UPROPERTY(Transient)
	TObjectPtr<UElemagicElementLoadoutViewModel> ElementLoadoutViewModel;

	UPROPERTY(Transient)
	TObjectPtr<UElemagicSkillBarViewModel> SkillBarViewModel;

	UPROPERTY(Transient)
	TObjectPtr<UElemagicInventoryViewModel> InventoryViewModel;
};
