// Fill out your copyright notice in the Description page of Project Settings.

#include "ElementComboInfo.h"

namespace
{
	// 两个 Tag 数组多重集相等：顺序无关 + 每个 Tag 出现次数一致。
	bool IsMultisetEqual(const TArray<FGameplayTag>& A, const TArray<FGameplayTag>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}

		TArray<FGameplayTag> SortedA = A;
		TArray<FGameplayTag> SortedB = B;
		SortedA.Sort();
		SortedB.Sort();

		for (int32 i = 0; i < SortedA.Num(); ++i)
		{
			if (SortedA[i] != SortedB[i])
			{
				return false;
			}
		}
		return true;
	}
}

const FElementComboEntry* UElementComboInfo::FindCombo(const TArray<FGameplayTag>& Elements) const
{
	for (const FElementComboEntry& Entry : Combos)
	{
		if (IsMultisetEqual(Entry.Elements, Elements))
		{
			return &Entry;
		}
	}
	return nullptr;
}
