// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayTagContainer.h"
#include "InventoryComponent.generated.h"

/**
 * 通用物品列表组件：用 GameplayTag 表示物品类型。
 * 完整拾取经济（数量/堆叠/丢弃）留待后续，本组件先支撑 UI 面板。
 */
UCLASS(ClassGroup = (Elemagic), meta = (BlueprintSpawnableComponent))
class ELEMAGIC_API UInventoryComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Inventory")
	void AddItem(FGameplayTag ItemTag);

	UFUNCTION(BlueprintCallable, Category = "Inventory")
	bool RemoveItem(FGameplayTag ItemTag);

	UFUNCTION(BlueprintPure, Category = "Inventory")
	const TArray<FGameplayTag>& GetItems() const { return Items; }

	UFUNCTION(BlueprintPure, Category = "Inventory")
	int32 GetItemCount() const { return Items.Num(); }

	DECLARE_MULTICAST_DELEGATE(FOnInventoryChanged);
	FOnInventoryChanged OnInventoryChanged;

private:
	UPROPERTY()
	TArray<FGameplayTag> Items;
};
