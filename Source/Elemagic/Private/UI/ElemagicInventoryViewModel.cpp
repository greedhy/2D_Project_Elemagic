// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/ElemagicInventoryViewModel.h"
#include "InventoryComponent.h"

void UElemagicInventoryViewModel::BindToInventory(UInventoryComponent* InComponent)
{
	Component = InComponent;
	if (Component.IsValid())
	{
		Component->OnInventoryChanged.AddUObject(this, &UElemagicInventoryViewModel::RefreshFromComponent);
	}
	RefreshFromComponent();
}

void UElemagicInventoryViewModel::RefreshFromComponent()
{
	if (!Component.IsValid())
	{
		SetItemCount(0);
		SetItemSummary(FText::GetEmpty());
		return;
	}

	SetItemCount(Component->GetItemCount());

	// 用逗号连接物品 Tag 名称作为摘要
	FString Summary;
	for (const FGameplayTag& Item : Component->GetItems())
	{
		if (!Summary.IsEmpty())
		{
			Summary += TEXT(", ");
		}
		Summary += Item.ToString();
	}
	SetItemSummary(FText::FromString(Summary));
}
