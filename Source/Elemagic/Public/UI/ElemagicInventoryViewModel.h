// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "MVVMViewModelBase.h"
#include "ElemagicInventoryViewModel.generated.h"

class UInventoryComponent;

/**
 * 背包 ViewModel：镜像物品数量 + 摘要文本。
 */
UCLASS(BlueprintType)
class ELEMAGIC_API UElemagicInventoryViewModel : public UMVVMViewModelBase
{
	GENERATED_BODY()

public:
	void BindToInventory(UInventoryComponent* InComponent);
	void RefreshFromComponent();

	int32 GetItemCount() const { return ItemCount; }
	FText GetItemSummary() const { return ItemSummary; }

	void SetItemCount(int32 V) { UE_MVVM_SET_PROPERTY_VALUE(ItemCount, V); }
	void SetItemSummary(const FText& V) { UE_MVVM_SET_PROPERTY_VALUE(ItemSummary, V); }

private:
	TWeakObjectPtr<UInventoryComponent> Component;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	int32 ItemCount = 0;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta = (AllowPrivateAccess = "true"))
	FText ItemSummary;
};
