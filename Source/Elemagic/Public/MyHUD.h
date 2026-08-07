// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "MyHUD.generated.h"

/**
 * 
 */
UCLASS()
class ELEMAGIC_API AMyHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void BeginPlay() override;

protected:
	// 在编辑器里创建具体的 HUD UMG 蓝图后赋值(生命值条/技能图标等)
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Elemagic|UI")
	TSubclassOf<class UUserWidget> HUDWidgetClass;

	UPROPERTY(BlueprintReadOnly, Category = "Elemagic|UI")
	TObjectPtr<class UUserWidget> HUDWidgetInstance;
};
