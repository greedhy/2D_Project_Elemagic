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
            // Combo window 内按键 → 缓冲而不激活
            if (IsComboWindowOpen())
            {
                BufferedInputTag = InputTag;
                BufferedInputExpiryTime = GetWorld()
                    ? GetWorld()->GetTimeSeconds() + ComboBufferWindow
                    : 0.f;
                UE_LOG(LogTemp, Log, TEXT("[Combo] Buffered input: %s"), *InputTag.ToString());
                return;
            }

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

bool AMyPlayerController::IsComboWindowOpen() const
{
    if (const IAbilitySystemInterface* ASI = Cast<IAbilitySystemInterface>(GetPawn()))
    {
        if (UAbilitySystemComponent* ASC = ASI->GetAbilitySystemComponent())
        {
            return ASC->HasMatchingGameplayTag(ElemagicGameplayTags::Combo_WindowOpen);
        }
    }
    return false;
}

bool AMyPlayerController::ConsumeBufferedComboInput(const FGameplayTag& ExpectedTag)
{
    if (!BufferedInputTag.IsValid())
    {
        return false;
    }

    // 检查缓冲未过期且 Tag 匹配
    const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
    if (Now > BufferedInputExpiryTime)
    {
        ClearBufferedInput();
        return false;
    }

    // ExpectedTag 是 skill 的 ComboInputTag（默认 = AbilityTags 的第一项）
    // 缓冲 Tag 应该是 ExpectedTag 本身或其子 Tag
    if (BufferedInputTag != ExpectedTag)
    {
        ClearBufferedInput();
        return false;
    }

    ClearBufferedInput();
    return true;
}

void AMyPlayerController::ClearBufferedInput()
{
    BufferedInputTag = FGameplayTag();
    BufferedInputExpiryTime = 0.f;
}
