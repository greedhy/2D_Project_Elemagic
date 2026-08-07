// Fill out your copyright notice in the Description page of Project Settings.

#include "MyPlayerController.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "ElemagicGameplayTags.h"
#include "EnhancedInputSubsystems.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "InputAction.h"
#include "Input/EleInputComponent.h"
#include "Input/EleInputConfig.h"
#include "GameFramework/Character.h"

AMyPlayerController::AMyPlayerController()
{
}

void AMyPlayerController::BeginPlay()
{
	Super::BeginPlay();

	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		if (PlayerMappingContext)
		{
			Subsystem->AddMappingContext(PlayerMappingContext, 0);
		}
	}
}

void AMyPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	UEleInputComponent* EleInputComponent = Cast<UEleInputComponent>(InputComponent);
	if (!EleInputComponent)
	{
		// DefaultInputComponentClass 是引擎启动时从 DefaultInput.ini 缓存的,改配置后需要完整重启编辑器
		// (仅 Live Coding 热编译不够)才会生效,生效前这里会退回引擎默认的 EnhancedInputComponent。
		UE_LOG(LogTemp, Error, TEXT("SetupInputComponent: InputComponent is not a UEleInputComponent (got %s). "
			"Restart the editor so Config/DefaultInput.ini's DefaultInputComponentClass takes effect."),
			*GetNameSafe(InputComponent ? InputComponent->GetClass() : nullptr));
		return;
	}

	if (MoveAction)
	{
		EleInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AMyPlayerController::Move);
	}
	if (JumpAction)
	{
		EleInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &AMyPlayerController::JumpPressed);
		EleInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this, &AMyPlayerController::JumpReleased);
	}
	if (InputConfig)
	{
		EleInputComponent->BindAbilityActions(InputConfig, this,
			&AMyPlayerController::AbilityInputTagPressed, &AMyPlayerController::AbilityInputTagReleased, &AMyPlayerController::AbilityInputTagHeld);
	}
}

void AMyPlayerController::Move(const FInputActionValue& Value)
{
	APawn* ControlledPawn = GetPawn();
	if (!ControlledPawn)
	{
		return;
	}

	if (const IAbilitySystemInterface* ASI = Cast<IAbilitySystemInterface>(ControlledPawn))
	{
		if (UAbilitySystemComponent* ASC = ASI->GetAbilitySystemComponent())
		{
			if (ASC->HasMatchingGameplayTag(ElemagicGameplayTags::State_Dead))
			{
				return;
			}
		}
	}

	const float MoveValue = Value.Get<float>();
	if (!FMath::IsNearlyZero(MoveValue))
	{
		ControlledPawn->AddMovementInput(FVector::ForwardVector, MoveValue);
	}
}

void AMyPlayerController::JumpPressed()
{
	if (ACharacter* ControlledCharacter = Cast<ACharacter>(GetPawn()))
	{
		ControlledCharacter->Jump();
	}
}

void AMyPlayerController::JumpReleased()
{
	if (ACharacter* ControlledCharacter = Cast<ACharacter>(GetPawn()))
	{
		ControlledCharacter->StopJumping();
	}
}

void AMyPlayerController::AbilityInputTagPressed(FGameplayTag InputTag)
{
	if (const IAbilitySystemInterface* ASI = Cast<IAbilitySystemInterface>(GetPawn()))
	{
		if (UAbilitySystemComponent* ASC = ASI->GetAbilitySystemComponent())
		{
			ASC->TryActivateAbilitiesByTag(FGameplayTagContainer(InputTag));
		}
	}
}

void AMyPlayerController::AbilityInputTagReleased(FGameplayTag InputTag)
{
}

void AMyPlayerController::AbilityInputTagHeld(FGameplayTag InputTag)
{
}
