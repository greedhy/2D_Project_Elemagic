// Fill out your copyright notice in the Description page of Project Settings.

#include "ElemagicEnemyCharacter.h"
#include "AbilitySystemComponent.h"
#include "Components/WidgetComponent.h"
#include "UI/ElemagicUserWidget.h"
#include "UI/ElemagicAttributeViewModel.h"

AElemagicEnemyCharacter::AElemagicEnemyCharacter()
{
	HealthBarWidgetComponent = CreateDefaultSubobject<UWidgetComponent>(TEXT("HealthBarWidgetComponent"));
	HealthBarWidgetComponent->SetupAttachment(RootComponent);
	HealthBarWidgetComponent->SetWidgetSpace(EWidgetSpace::Screen);
	HealthBarWidgetComponent->SetDrawSize(FVector2D(100.f, 20.f));
	HealthBarWidgetComponent->SetRelativeLocation(FVector(0.f, 0.f, 70.f)); // 头顶上方
}

void AElemagicEnemyCharacter::BeginPlay()
{
	Super::BeginPlay();

	// 敌人无 Controller，PossessedBy 不触发，需手动初始化 ASC + 属性
	if (!bAbilityActorInfoInitialized && AbilitySystemComponent)
	{
		AbilitySystemComponent->InitAbilityActorInfo(this, this);
		InitializeAttributes();
		bAbilityActorInfoInitialized = true;
	}

	// 头顶血条
	if (HealthBarWidgetClass && HealthBarWidgetComponent)
	{
		HealthBarWidgetComponent->SetWidgetClass(HealthBarWidgetClass);
		HealthBarWidgetComponent->InitWidget();

		HealthViewModel = NewObject<UElemagicAttributeViewModel>(this);
		HealthViewModel->BindToAbilitySystem(AbilitySystemComponent);

		if (UElemagicUserWidget* Widget = Cast<UElemagicUserWidget>(HealthBarWidgetComponent->GetUserWidgetObject()))
		{
			Widget->BindViewModel(HealthViewModel);
		}
	}
}
