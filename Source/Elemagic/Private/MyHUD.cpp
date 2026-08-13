// Fill out your copyright notice in the Description page of Project Settings.


#include "MyHUD.h"
#include "UI/ElemagicHUDWidget.h"

void AMyHUD::BeginPlay()
{
	Super::BeginPlay();

	if (HUDWidgetClass)
	{
		HUDWidgetInstance = CreateWidget<UElemagicHUDWidget>(GetOwningPlayerController(), HUDWidgetClass);
		if (HUDWidgetInstance)
		{
			HUDWidgetInstance->AddToViewport();
			HUDWidgetInstance->BindToPlayer(GetOwningPlayerController());
		}
	}
}
