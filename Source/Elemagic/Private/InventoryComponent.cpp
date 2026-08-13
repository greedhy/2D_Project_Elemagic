// Fill out your copyright notice in the Description page of Project Settings.

#include "InventoryComponent.h"

void UInventoryComponent::AddItem(FGameplayTag ItemTag)
{
	if (!ItemTag.IsValid())
	{
		return;
	}

	Items.Add(ItemTag);
	OnInventoryChanged.Broadcast();
}

bool UInventoryComponent::RemoveItem(FGameplayTag ItemTag)
{
	const int32 Removed = Items.Remove(ItemTag);
	if (Removed > 0)
	{
		OnInventoryChanged.Broadcast();
		return true;
	}
	return false;
}
