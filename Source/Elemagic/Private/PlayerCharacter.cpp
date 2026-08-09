// Fill out your copyright notice in the Description page of Project Settings.

#include "PlayerCharacter.h"
#include "AbilitySystemComponent.h"
#include "ElemagicGameplayTags.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/CharacterMovementComponent.h"

APlayerCharacter::APlayerCharacter()
{
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 800.f;
	CameraBoom->SetRelativeRotation(FRotator(0.f, -90.f, 0.f));
	CameraBoom->bUsePawnControlRotation = false;
	CameraBoom->bInheritPitch = false;
	CameraBoom->bInheritYaw = false;
	CameraBoom->bInheritRoll = false;
	CameraBoom->bDoCollisionTest = false;

	SideViewCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("SideViewCamera"));
	SideViewCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	SideViewCamera->bUsePawnControlRotation = false;
}

void APlayerCharacter::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	UCharacterMovementComponent* MoveComp = GetCharacterMovement();
	if (!MoveComp) return;

	// 移动速度和跳跃高度以 EditDefaultsOnly 属性为准,PostInit 在 BP 反序列化后执行。
	MoveComp->MaxWalkSpeed = MoveSpeed;
	MoveComp->JumpZVelocity = JumpVelocity;
	JumpMaxHoldTime = JumpHoldTime;
	JumpMaxCount = MaxJumpCount;

	// 横版动作平台跳跃手感
	MoveComp->GravityScale = 2.f;

	// 零惯性即时响应
	MoveComp->MaxAcceleration = 99999.f;
	MoveComp->BrakingDecelerationWalking = 99999.f;
	MoveComp->AirControl = 1.f;
	MoveComp->bUseSeparateBrakingFriction = false;

	UE_LOG(LogTemp, Log, TEXT("[%s] PostInit - MaxWalkSpeed=%.1f JumpZVelocity=%.1f JumpMaxCount=%d GravityScale=%.1f"),
		*GetName(), MoveComp->MaxWalkSpeed, MoveComp->JumpZVelocity, JumpMaxCount, MoveComp->GravityScale);
}

void APlayerCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

	if (AbilitySystemComponent)
	{
		for (const TSubclassOf<UGameplayAbility>& AbilityClass : StartupAbilities)
		{
			if (AbilityClass)
			{
				AbilitySystemComponent->GiveAbility(FGameplayAbilitySpec(AbilityClass, 1, 0));
			}
		}
	}
}

void APlayerCharacter::Landed(const FHitResult& Hit)
{
	Super::Landed(Hit);

	if (AbilitySystemComponent)
	{
		AbilitySystemComponent->RemoveLooseGameplayTag(ElemagicGameplayTags::State_DashedInAir);
	}
}

void APlayerCharacter::OnMovementModeChanged(EMovementMode PrevMovementMode, uint8 PreviousCustomMode)
{
	Super::OnMovementModeChanged(PrevMovementMode, PreviousCustomMode);

	if (!AbilitySystemComponent)
	{
		return;
	}

	if (GetCharacterMovement() && GetCharacterMovement()->MovementMode == MOVE_Falling)
	{
		FGameplayTagContainer CooldownTag;
		CooldownTag.AddTag(ElemagicGameplayTags::Cooldown_Dash);
		AbilitySystemComponent->RemoveActiveEffectsWithGrantedTags(CooldownTag);
	}
}
