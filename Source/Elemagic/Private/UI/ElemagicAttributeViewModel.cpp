// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/ElemagicAttributeViewModel.h"
#include "AbilitySystemComponent.h"
#include "CharacterAttributeSetBase.h"
#include "GameplayEffectTypes.h"

void UElemagicAttributeViewModel::BindToAbilitySystem(UAbilitySystemComponent* InASC)
{
	UnbindFromAbilitySystem();
	ASC = InASC;
	if (!InASC)
	{
		return;
	}

	const UCharacterAttributeSetBase* AttrSet = InASC->GetSet<UCharacterAttributeSetBase>();
	if (!AttrSet)
	{
		return;
	}

	// 同步初始值
	SetHealth(AttrSet->GetHealth());
	SetMaxHealth(AttrSet->GetMaxHealth());
	SetAttackPower(AttrSet->GetAttackPower());
	SetDefense(AttrSet->GetDefense());
	SetMoveSpeed(AttrSet->GetMoveSpeed());
	SetFireResistance(AttrSet->GetFireResistance());
	SetWaterResistance(AttrSet->GetWaterResistance());
	SetEarthResistance(AttrSet->GetEarthResistance());
	SetWindResistance(AttrSet->GetWindResistance());
	SetLightningResistance(AttrSet->GetLightningResistance());
	SetLightResistance(AttrSet->GetLightResistance());
	SetDarkResistance(AttrSet->GetDarkResistance());

	// 订阅每个属性的变化委托
	DelegateHandles.Add(InASC->GetGameplayAttributeValueChangeDelegate(AttrSet->GetHealthAttribute())
		.AddUObject(this, &UElemagicAttributeViewModel::OnAttributeChanged));
	DelegateHandles.Add(InASC->GetGameplayAttributeValueChangeDelegate(AttrSet->GetMaxHealthAttribute())
		.AddUObject(this, &UElemagicAttributeViewModel::OnAttributeChanged));
	DelegateHandles.Add(InASC->GetGameplayAttributeValueChangeDelegate(AttrSet->GetAttackPowerAttribute())
		.AddUObject(this, &UElemagicAttributeViewModel::OnAttributeChanged));
	DelegateHandles.Add(InASC->GetGameplayAttributeValueChangeDelegate(AttrSet->GetDefenseAttribute())
		.AddUObject(this, &UElemagicAttributeViewModel::OnAttributeChanged));
	DelegateHandles.Add(InASC->GetGameplayAttributeValueChangeDelegate(AttrSet->GetMoveSpeedAttribute())
		.AddUObject(this, &UElemagicAttributeViewModel::OnAttributeChanged));

	DelegateHandles.Add(InASC->GetGameplayAttributeValueChangeDelegate(AttrSet->GetFireResistanceAttribute())
		.AddUObject(this, &UElemagicAttributeViewModel::OnAttributeChanged));
	DelegateHandles.Add(InASC->GetGameplayAttributeValueChangeDelegate(AttrSet->GetWaterResistanceAttribute())
		.AddUObject(this, &UElemagicAttributeViewModel::OnAttributeChanged));
	DelegateHandles.Add(InASC->GetGameplayAttributeValueChangeDelegate(AttrSet->GetEarthResistanceAttribute())
		.AddUObject(this, &UElemagicAttributeViewModel::OnAttributeChanged));
	DelegateHandles.Add(InASC->GetGameplayAttributeValueChangeDelegate(AttrSet->GetWindResistanceAttribute())
		.AddUObject(this, &UElemagicAttributeViewModel::OnAttributeChanged));
	DelegateHandles.Add(InASC->GetGameplayAttributeValueChangeDelegate(AttrSet->GetLightningResistanceAttribute())
		.AddUObject(this, &UElemagicAttributeViewModel::OnAttributeChanged));
	DelegateHandles.Add(InASC->GetGameplayAttributeValueChangeDelegate(AttrSet->GetLightResistanceAttribute())
		.AddUObject(this, &UElemagicAttributeViewModel::OnAttributeChanged));
	DelegateHandles.Add(InASC->GetGameplayAttributeValueChangeDelegate(AttrSet->GetDarkResistanceAttribute())
		.AddUObject(this, &UElemagicAttributeViewModel::OnAttributeChanged));
}

void UElemagicAttributeViewModel::UnbindFromAbilitySystem()
{
	if (ASC.IsValid())
	{
		const UCharacterAttributeSetBase* AttrSet = ASC->GetSet<UCharacterAttributeSetBase>();
		if (AttrSet)
		{
			// 按订阅顺序移除（与 BindToAbilitySystem 中 Add 的顺序一致）
			const FGameplayAttribute Attributes[] = {
				AttrSet->GetHealthAttribute(),
				AttrSet->GetMaxHealthAttribute(),
				AttrSet->GetAttackPowerAttribute(),
				AttrSet->GetDefenseAttribute(),
				AttrSet->GetMoveSpeedAttribute(),
				AttrSet->GetFireResistanceAttribute(),
				AttrSet->GetWaterResistanceAttribute(),
				AttrSet->GetEarthResistanceAttribute(),
				AttrSet->GetWindResistanceAttribute(),
				AttrSet->GetLightningResistanceAttribute(),
				AttrSet->GetLightResistanceAttribute(),
				AttrSet->GetDarkResistanceAttribute()
			};

			for (int32 i = 0; i < DelegateHandles.Num() && i < 12; ++i)
			{
				ASC->GetGameplayAttributeValueChangeDelegate(Attributes[i]).Remove(DelegateHandles[i]);
			}
		}
	}

	DelegateHandles.Reset();
	ASC = nullptr;
}

void UElemagicAttributeViewModel::BeginDestroy()
{
	UnbindFromAbilitySystem();
	Super::BeginDestroy();
}

void UElemagicAttributeViewModel::OnAttributeChanged(const FOnAttributeChangeData& Data)
{
	const UCharacterAttributeSetBase* AttrSet = ASC.IsValid() ? ASC->GetSet<UCharacterAttributeSetBase>() : nullptr;
	if (!AttrSet)
	{
		return;
	}

	if (Data.Attribute == AttrSet->GetHealthAttribute())
	{
		SetHealth(Data.NewValue);
	}
	else if (Data.Attribute == AttrSet->GetMaxHealthAttribute())
	{
		SetMaxHealth(Data.NewValue);
	}
	else if (Data.Attribute == AttrSet->GetAttackPowerAttribute())
	{
		SetAttackPower(Data.NewValue);
	}
	else if (Data.Attribute == AttrSet->GetDefenseAttribute())
	{
		SetDefense(Data.NewValue);
	}
	else if (Data.Attribute == AttrSet->GetMoveSpeedAttribute())
	{
		SetMoveSpeed(Data.NewValue);
	}
	else if (Data.Attribute == AttrSet->GetFireResistanceAttribute())
	{
		SetFireResistance(Data.NewValue);
	}
	else if (Data.Attribute == AttrSet->GetWaterResistanceAttribute())
	{
		SetWaterResistance(Data.NewValue);
	}
	else if (Data.Attribute == AttrSet->GetEarthResistanceAttribute())
	{
		SetEarthResistance(Data.NewValue);
	}
	else if (Data.Attribute == AttrSet->GetWindResistanceAttribute())
	{
		SetWindResistance(Data.NewValue);
	}
	else if (Data.Attribute == AttrSet->GetLightningResistanceAttribute())
	{
		SetLightningResistance(Data.NewValue);
	}
	else if (Data.Attribute == AttrSet->GetLightResistanceAttribute())
	{
		SetLightResistance(Data.NewValue);
	}
	else if (Data.Attribute == AttrSet->GetDarkResistanceAttribute())
	{
		SetDarkResistance(Data.NewValue);
	}
}

void UElemagicAttributeViewModel::SetHealth(float NewHealth)
{
	if (UE_MVVM_SET_PROPERTY_VALUE(Health, NewHealth))
	{
		UE_MVVM_BROADCAST_FIELD_VALUE_CHANGED(GetHealthPercent);
	}
}

void UElemagicAttributeViewModel::SetMaxHealth(float NewMaxHealth)
{
	if (UE_MVVM_SET_PROPERTY_VALUE(MaxHealth, NewMaxHealth))
	{
		UE_MVVM_BROADCAST_FIELD_VALUE_CHANGED(GetHealthPercent);
	}
}

void UElemagicAttributeViewModel::SetAttackPower(float NewAttackPower)
{
	UE_MVVM_SET_PROPERTY_VALUE(AttackPower, NewAttackPower);
}

void UElemagicAttributeViewModel::SetDefense(float NewDefense)
{
	UE_MVVM_SET_PROPERTY_VALUE(Defense, NewDefense);
}

void UElemagicAttributeViewModel::SetMoveSpeed(float NewMoveSpeed)
{
	UE_MVVM_SET_PROPERTY_VALUE(MoveSpeed, NewMoveSpeed);
}

void UElemagicAttributeViewModel::SetFireResistance(float NewFireResistance)
{
	UE_MVVM_SET_PROPERTY_VALUE(FireResistance, NewFireResistance);
}

void UElemagicAttributeViewModel::SetWaterResistance(float NewWaterResistance)
{
	UE_MVVM_SET_PROPERTY_VALUE(WaterResistance, NewWaterResistance);
}

void UElemagicAttributeViewModel::SetEarthResistance(float NewEarthResistance)
{
	UE_MVVM_SET_PROPERTY_VALUE(EarthResistance, NewEarthResistance);
}

void UElemagicAttributeViewModel::SetWindResistance(float NewWindResistance)
{
	UE_MVVM_SET_PROPERTY_VALUE(WindResistance, NewWindResistance);
}

void UElemagicAttributeViewModel::SetLightningResistance(float NewLightningResistance)
{
	UE_MVVM_SET_PROPERTY_VALUE(LightningResistance, NewLightningResistance);
}

void UElemagicAttributeViewModel::SetLightResistance(float NewLightResistance)
{
	UE_MVVM_SET_PROPERTY_VALUE(LightResistance, NewLightResistance);
}

void UElemagicAttributeViewModel::SetDarkResistance(float NewDarkResistance)
{
	UE_MVVM_SET_PROPERTY_VALUE(DarkResistance, NewDarkResistance);
}
