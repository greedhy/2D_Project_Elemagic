// Fill out your copyright notice in the Description page of Project Settings.

#include "ElementSystemComponent.h"
#include "ElementComboInfo.h"
#include "ElemagicGameplayTags.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"

bool FElemagicSkillSlot::operator==(const FElemagicSkillSlot& Other) const
{
	return AbilityTag == Other.AbilityTag
		&& Icon == Other.Icon
		&& DisplayName.IdenticalTo(Other.DisplayName)
		&& CooldownTag == Other.CooldownTag
		&& CooldownRemaining == Other.CooldownRemaining
		&& bIsEmpty == Other.bIsEmpty;
}

UElementSystemComponent::UElementSystemComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UElementSystemComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!ASC.IsValid())
	{
		return;
	}

	// 更新各技能槽的冷却剩余时间
	for (int32 i = 0; i < SkillBarData.Num(); ++i)
	{
		FElemagicSkillSlot& Slot = SkillBarData[i];
		if (Slot.bIsEmpty || !Slot.CooldownTag.IsValid())
		{
			continue;
		}

		FGameplayTagContainer TagContainer;
		TagContainer.AddTag(Slot.CooldownTag);
		const FGameplayEffectQuery Query = FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(TagContainer);

		const TArray<float> RemainingTimes = ASC->GetActiveEffectsTimeRemaining(Query);
		float Remaining = 0.f;
		for (const float T : RemainingTimes)
		{
			Remaining = FMath::Max(Remaining, T);
		}
		Slot.CooldownRemaining = Remaining;
	}
}

void UElementSystemComponent::Init(UAbilitySystemComponent* InASC)
{
	ASC = InASC;

	Loadout.Init(FGameplayTag(), LoadoutSize);
	SkillBarHandles.Init(FGameplayAbilitySpecHandle(), SkillBarSize);
	SkillBarData.SetNum(SkillBarSize);
}

int32 UElementSystemComponent::FindEmptyLoadoutSlot() const
{
	for (int32 i = 0; i < Loadout.Num(); ++i)
	{
		if (!Loadout[i].IsValid())
		{
			return i;
		}
	}
	return INDEX_NONE;
}

int32 UElementSystemComponent::FindEmptySkillSlot() const
{
	for (int32 i = 0; i < SkillBarData.Num(); ++i)
	{
		if (SkillBarData[i].bIsEmpty)
		{
			return i;
		}
	}
	return INDEX_NONE;
}

bool UElementSystemComponent::AddElement(FGameplayTag ElementTag)
{
	if (!ElementTag.IsValid())
	{
		return false;
	}

	const int32 Slot = FindEmptyLoadoutSlot();
	if (Slot == INDEX_NONE)
	{
		return false; // 3 槽已满
	}

	Loadout[Slot] = ElementTag;
	OnElementSystemChanged.Broadcast();
	return true;
}

FGameplayTag UElementSystemComponent::GetLoadoutSlot(int32 Index) const
{
	if (Loadout.IsValidIndex(Index))
	{
		return Loadout[Index];
	}
	return FGameplayTag();
}

bool UElementSystemComponent::CanSynthesize() const
{
	// 必须装满 3 槽
	if (FindEmptyLoadoutSlot() != INDEX_NONE)
	{
		return false;
	}

	if (!ComboInfo)
	{
		return false;
	}

	return ComboInfo->FindCombo(Loadout) != nullptr;
}

bool UElementSystemComponent::Synthesize()
{
	if (!CanSynthesize())
	{
		return false;
	}

	const int32 Slot = FindEmptySkillSlot();
	if (Slot == INDEX_NONE)
	{
		return false; // 技能栏满
	}

	const FElementComboEntry* Combo = ComboInfo->FindCombo(Loadout);
	if (!Combo || !Combo->ResultAbility)
	{
		return false;
	}

	// 授予技能并记录 handle
	if (ASC.IsValid())
	{
		FGameplayAbilitySpec Spec(Combo->ResultAbility, 1, INDEX_NONE, GetOwner());
		SkillBarHandles[Slot] = ASC->GiveAbility(Spec);
	}

	// 填充技能栏数据
	FElemagicSkillSlot& SlotData = SkillBarData[Slot];
	SlotData.DisplayName = Combo->DisplayName;
	SlotData.Icon = Combo->Icon;
	SlotData.CooldownTag = Combo->CooldownTag;
	SlotData.CooldownRemaining = 0.f;
	SlotData.bIsEmpty = false;

	// 从技能 CDO 取第一个 AbilityTag（供未来输入路由参考）
	if (const UGameplayAbility* AbilityCDO = Combo->ResultAbility.GetDefaultObject())
	{
		const FGameplayTagContainer AssetTags = AbilityCDO->GetAssetTags();
		if (AssetTags.Num() > 0)
		{
			SlotData.AbilityTag = AssetTags.First();
		}
	}

	// 清空 3 槽装载
	for (FGameplayTag& Tag : Loadout)
	{
		Tag = FGameplayTag();
	}

	OnElementSystemChanged.Broadcast();
	return true;
}

FElemagicSkillSlot UElementSystemComponent::GetSkillSlot(int32 Index) const
{
	if (SkillBarData.IsValidIndex(Index))
	{
		return SkillBarData[Index];
	}
	return FElemagicSkillSlot();
}

bool UElementSystemComponent::RemoveSkill(int32 SlotIndex)
{
	if (!SkillBarData.IsValidIndex(SlotIndex) || SkillBarData[SlotIndex].bIsEmpty)
	{
		return false;
	}

	if (ASC.IsValid() && SkillBarHandles[SlotIndex].IsValid())
	{
		ASC->ClearAbility(SkillBarHandles[SlotIndex]);
	}

	SkillBarHandles[SlotIndex] = FGameplayAbilitySpecHandle();
	SkillBarData[SlotIndex] = FElemagicSkillSlot();
	OnElementSystemChanged.Broadcast();
	return true;
}

void UElementSystemComponent::ClearSkillBar()
{
	for (int32 i = 0; i < SkillBarSize; ++i)
	{
		if (SkillBarData.IsValidIndex(i) && !SkillBarData[i].bIsEmpty)
		{
			if (ASC.IsValid() && SkillBarHandles[i].IsValid())
			{
				ASC->ClearAbility(SkillBarHandles[i]);
			}
			SkillBarHandles[i] = FGameplayAbilitySpecHandle();
			SkillBarData[i] = FElemagicSkillSlot();
		}
	}
	OnElementSystemChanged.Broadcast();
}

bool UElementSystemComponent::HandleInputTag(FGameplayTag InputTag)
{
	if (InputTag == ElemagicGameplayTags::Input_Synthesize)
	{
		return Synthesize();
	}

	if (InputTag == ElemagicGameplayTags::Input_Skill1) return ActivateSkillSlot(0);
	if (InputTag == ElemagicGameplayTags::Input_Skill2) return ActivateSkillSlot(1);
	if (InputTag == ElemagicGameplayTags::Input_Skill3) return ActivateSkillSlot(2);
	if (InputTag == ElemagicGameplayTags::Input_Skill4) return ActivateSkillSlot(3);

	return false;
}

bool UElementSystemComponent::ActivateSkillSlot(int32 Index)
{
	if (!SkillBarData.IsValidIndex(Index) || SkillBarData[Index].bIsEmpty)
	{
		return false;
	}

	if (ASC.IsValid() && SkillBarHandles[Index].IsValid())
	{
		return ASC->TryActivateAbility(SkillBarHandles[Index]);
	}

	return false;
}
