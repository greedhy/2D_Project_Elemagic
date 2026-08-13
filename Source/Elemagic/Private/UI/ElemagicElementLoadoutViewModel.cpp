// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/ElemagicElementLoadoutViewModel.h"
#include "ElementSystemComponent.h"

void UElemagicElementLoadoutViewModel::BindToElementSystem(UElementSystemComponent* InComponent)
{
	Component = InComponent;
	if (Component.IsValid())
	{
		Component->OnElementSystemChanged.AddUObject(this, &UElemagicElementLoadoutViewModel::RefreshFromComponent);
	}
	RefreshFromComponent();
}

void UElemagicElementLoadoutViewModel::RefreshFromComponent()
{
	if (!Component.IsValid())
	{
		SetElementSlot0(TEXT(""));
		SetElementSlot1(TEXT(""));
		SetElementSlot2(TEXT(""));
		SetCanSynthesize(false);
		return;
	}

	const FGameplayTag Slot0 = Component->GetLoadoutSlot(0);
	const FGameplayTag Slot1 = Component->GetLoadoutSlot(1);
	const FGameplayTag Slot2 = Component->GetLoadoutSlot(2);

	SetElementSlot0(Slot0.IsValid() ? Slot0.ToString() : TEXT(""));
	SetElementSlot1(Slot1.IsValid() ? Slot1.ToString() : TEXT(""));
	SetElementSlot2(Slot2.IsValid() ? Slot2.ToString() : TEXT(""));
	SetCanSynthesize(Component->CanSynthesize());
}
