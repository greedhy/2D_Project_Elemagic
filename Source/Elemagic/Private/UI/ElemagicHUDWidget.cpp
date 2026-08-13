// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/ElemagicHUDWidget.h"
#include "UI/ElemagicAttributeViewModel.h"
#include "UI/ElemagicElementLoadoutViewModel.h"
#include "UI/ElemagicSkillBarViewModel.h"
#include "UI/ElemagicInventoryViewModel.h"
#include "ElementSystemComponent.h"
#include "InventoryComponent.h"
#include "AbilitySystemInterface.h"
#include "AbilitySystemComponent.h"
#include "GameFramework/PlayerController.h"

void UElemagicHUDWidget::BindToPlayer(APlayerController* PC)
{
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn)
	{
		return;
	}

	// === 属性 ViewModel（血条 + 属性列表共用） ===
	UAbilitySystemComponent* ASC = nullptr;
	if (const IAbilitySystemInterface* ASI = Cast<IAbilitySystemInterface>(Pawn))
	{
		ASC = ASI->GetAbilitySystemComponent();
	}

	if (ASC)
	{
		AttributeViewModel = NewObject<UElemagicAttributeViewModel>(this);
		AttributeViewModel->BindToAbilitySystem(ASC);

		if (HealthBarWidget)
		{
			HealthBarWidget->BindViewModel(AttributeViewModel);
		}
		if (AttributeListWidget)
		{
			AttributeListWidget->BindViewModel(AttributeViewModel);
		}
	}

	// === 元素装载 ViewModel ===
	if (UElementSystemComponent* ElementSystem = Pawn->FindComponentByClass<UElementSystemComponent>())
	{
		ElementLoadoutViewModel = NewObject<UElemagicElementLoadoutViewModel>(this);
		ElementLoadoutViewModel->BindToElementSystem(ElementSystem);

		if (ElementLoadoutWidget)
		{
			ElementLoadoutWidget->BindViewModel(ElementLoadoutViewModel);
		}

		SkillBarViewModel = NewObject<UElemagicSkillBarViewModel>(this);
		SkillBarViewModel->BindToElementSystem(ElementSystem);

		if (SkillBarWidget)
		{
			SkillBarWidget->BindViewModel(SkillBarViewModel);
		}
	}

	// === 背包 ViewModel ===
	if (UInventoryComponent* Inventory = Pawn->FindComponentByClass<UInventoryComponent>())
	{
		InventoryViewModel = NewObject<UElemagicInventoryViewModel>(this);
		InventoryViewModel->BindToInventory(Inventory);

		if (InventoryWidget)
		{
			InventoryWidget->BindViewModel(InventoryViewModel);
		}
	}
}
