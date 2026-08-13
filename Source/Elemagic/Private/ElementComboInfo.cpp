// Fill out your copyright notice in the Description page of Project Settings.

#include "ElementComboInfo.h"

const FElementComboEntry* UElementComboInfo::FindCombo(const FGameplayTagContainer& Elements) const
{
	for (const FElementComboEntry& Entry : Combos)
	{
		// 数量一致且 Elements 包含 Entry.Elements 的全部 Tag → 集合相等
		if (Entry.Elements.Num() == Elements.Num() && Elements.HasAll(Entry.Elements))
		{
			return &Entry;
		}
	}
	return nullptr;
}
