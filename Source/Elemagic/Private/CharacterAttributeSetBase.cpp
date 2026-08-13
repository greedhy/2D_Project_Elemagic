// Fill out your copyright notice in the Description page of Project Settings.

#include "CharacterAttributeSetBase.h"
#include "Net/UnrealNetwork.h"
#include "GameplayEffectTypes.h"
#include "GameplayEffectExtension.h"
#include "AttributeSet.h"
#include "ElemagicGameplayTags.h"


UCharacterAttributeSetBase::UCharacterAttributeSetBase()
{
	InitHealth(100.f);
	InitMaxHealth(100.f);
	InitAttackPower(10.f);
	InitDefense(0.f);
	InitMoveSpeed(600.f);
	InitIncomingDamage(0.f);

	InitFireResistance(0.f);
	InitWaterResistance(0.f);
	InitEarthResistance(0.f);
	InitWindResistance(0.f);
	InitLightningResistance(0.f);
	InitLightResistance(0.f);
	InitDarkResistance(0.f);
}

void UCharacterAttributeSetBase::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME_CONDITION_NOTIFY(UCharacterAttributeSetBase, Health, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UCharacterAttributeSetBase, MaxHealth, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UCharacterAttributeSetBase, AttackPower, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UCharacterAttributeSetBase, Defense, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UCharacterAttributeSetBase, MoveSpeed, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UCharacterAttributeSetBase, IncomingDamage, COND_None, REPNOTIFY_Always);

	DOREPLIFETIME_CONDITION_NOTIFY(UCharacterAttributeSetBase, FireResistance, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UCharacterAttributeSetBase, WaterResistance, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UCharacterAttributeSetBase, EarthResistance, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UCharacterAttributeSetBase, WindResistance, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UCharacterAttributeSetBase, LightningResistance, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UCharacterAttributeSetBase, LightResistance, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UCharacterAttributeSetBase, DarkResistance, COND_None, REPNOTIFY_Always);
}

void UCharacterAttributeSetBase::ClampHealthAttribute(float& Value) const
{
	Value = FMath::Clamp(Value, 0.f, GetMaxHealth());
}

void UCharacterAttributeSetBase::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);

	if (Attribute == GetHealthAttribute())
	{
		ClampHealthAttribute(NewValue);
	}
	else if (Attribute == GetMaxHealthAttribute())
	{
		NewValue = FMath::Max(NewValue, 1.f);
	}
}

void UCharacterAttributeSetBase::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)
{
	Super::PostGameplayEffectExecute(Data);

	if (Data.EvaluatedData.Attribute == GetHealthAttribute())
	{
		SetHealth(FMath::Clamp(GetHealth(), 0.f, GetMaxHealth()));
	}

	if (Data.EvaluatedData.Attribute == GetIncomingDamageAttribute())
	{
		const float Damage = GetIncomingDamage();
		SetIncomingDamage(0.f);

		if (Damage > 0.f)
		{
			SetHealth(FMath::Clamp(GetHealth() - Damage, 0.f, GetMaxHealth()));
			// Health <= 0 由 OnHealthChanged 回调监听 -> Die()
		}
	}
}

void UCharacterAttributeSetBase::OnRep_Health(const FGameplayAttributeData& OldHealth)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UCharacterAttributeSetBase, Health, OldHealth);
}

void UCharacterAttributeSetBase::OnRep_MaxHealth(const FGameplayAttributeData& OldMaxHealth)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UCharacterAttributeSetBase, MaxHealth, OldMaxHealth);
}

void UCharacterAttributeSetBase::OnRep_AttackPower(const FGameplayAttributeData& OldAttackPower)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UCharacterAttributeSetBase, AttackPower, OldAttackPower);
}

void UCharacterAttributeSetBase::OnRep_Defense(const FGameplayAttributeData& OldDefense)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UCharacterAttributeSetBase, Defense, OldDefense);
}

void UCharacterAttributeSetBase::OnRep_MoveSpeed(const FGameplayAttributeData& OldMoveSpeed)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UCharacterAttributeSetBase, MoveSpeed, OldMoveSpeed);
}

void UCharacterAttributeSetBase::OnRep_IncomingDamage(const FGameplayAttributeData& OldIncomingDamage)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UCharacterAttributeSetBase, IncomingDamage, OldIncomingDamage);
}

void UCharacterAttributeSetBase::OnRep_FireResistance(const FGameplayAttributeData& OldFireResistance)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UCharacterAttributeSetBase, FireResistance, OldFireResistance);
}

void UCharacterAttributeSetBase::OnRep_WaterResistance(const FGameplayAttributeData& OldWaterResistance)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UCharacterAttributeSetBase, WaterResistance, OldWaterResistance);
}

void UCharacterAttributeSetBase::OnRep_EarthResistance(const FGameplayAttributeData& OldEarthResistance)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UCharacterAttributeSetBase, EarthResistance, OldEarthResistance);
}

void UCharacterAttributeSetBase::OnRep_WindResistance(const FGameplayAttributeData& OldWindResistance)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UCharacterAttributeSetBase, WindResistance, OldWindResistance);
}

void UCharacterAttributeSetBase::OnRep_LightningResistance(const FGameplayAttributeData& OldLightningResistance)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UCharacterAttributeSetBase, LightningResistance, OldLightningResistance);
}

void UCharacterAttributeSetBase::OnRep_LightResistance(const FGameplayAttributeData& OldLightResistance)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UCharacterAttributeSetBase, LightResistance, OldLightResistance);
}

void UCharacterAttributeSetBase::OnRep_DarkResistance(const FGameplayAttributeData& OldDarkResistance)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UCharacterAttributeSetBase, DarkResistance, OldDarkResistance);
}

float UCharacterAttributeSetBase::GetResistanceForElement(FGameplayTag ElementTag) const
{
	if (ElementTag == ElemagicGameplayTags::Element_Fire)      return GetFireResistance();
	if (ElementTag == ElemagicGameplayTags::Element_Water)     return GetWaterResistance();
	if (ElementTag == ElemagicGameplayTags::Element_Earth)     return GetEarthResistance();
	if (ElementTag == ElemagicGameplayTags::Element_Wind)      return GetWindResistance();
	if (ElementTag == ElemagicGameplayTags::Element_Lightning) return GetLightningResistance();
	if (ElementTag == ElemagicGameplayTags::Element_Light)     return GetLightResistance();
	if (ElementTag == ElemagicGameplayTags::Element_Dark)      return GetDarkResistance();
	return 0.f;
}
