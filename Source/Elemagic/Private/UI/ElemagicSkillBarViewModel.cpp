// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/ElemagicSkillBarViewModel.h"
#include "ElementSystemComponent.h"

void UElemagicSkillBarViewModel::BindToElementSystem(UElementSystemComponent* InComponent)
{
	Component = InComponent;
	if (Component.IsValid())
	{
		Component->OnElementSystemChanged.AddUObject(this, &UElemagicSkillBarViewModel::RefreshFromComponent);
	}
	RefreshFromComponent();
}

void UElemagicSkillBarViewModel::RefreshFromComponent()
{
	if (!Component.IsValid())
	{
		SetSkillSlot0Name(FText::GetEmpty());
		SetSkillSlot0IsEmpty(true);
		SetSkillSlot1Name(FText::GetEmpty());
		SetSkillSlot1IsEmpty(true);
		SetSkillSlot2Name(FText::GetEmpty());
		SetSkillSlot2IsEmpty(true);
		SetSkillSlot3Name(FText::GetEmpty());
		SetSkillSlot3IsEmpty(true);
		return;
	}

	const FElemagicSkillSlot Slot0 = Component->GetSkillSlot(0);
	const FElemagicSkillSlot Slot1 = Component->GetSkillSlot(1);
	const FElemagicSkillSlot Slot2 = Component->GetSkillSlot(2);
	const FElemagicSkillSlot Slot3 = Component->GetSkillSlot(3);

	SetSkillSlot0Name(Slot0.bIsEmpty ? FText::GetEmpty() : Slot0.DisplayName);
	SetSkillSlot0IsEmpty(Slot0.bIsEmpty);

	SetSkillSlot1Name(Slot1.bIsEmpty ? FText::GetEmpty() : Slot1.DisplayName);
	SetSkillSlot1IsEmpty(Slot1.bIsEmpty);

	SetSkillSlot2Name(Slot2.bIsEmpty ? FText::GetEmpty() : Slot2.DisplayName);
	SetSkillSlot2IsEmpty(Slot2.bIsEmpty);

	SetSkillSlot3Name(Slot3.bIsEmpty ? FText::GetEmpty() : Slot3.DisplayName);
	SetSkillSlot3IsEmpty(Slot3.bIsEmpty);
}
