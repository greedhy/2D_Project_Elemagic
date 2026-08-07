// Fill out your copyright notice in the Description page of Project Settings.

#include "PlayerCharacter.h"
#include "AbilitySystemComponent.h"
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

	// 按住 Jump 越久跳得越高:持续按住期间 CharacterMovementComponent 会把 Velocity.Z 顶回 JumpZVelocity 对抗重力,
	// 松开时(Controller 的 JumpReleased 调用 StopJumping)重力立刻接管,超过该时长则强制停止施力。
	JumpMaxHoldTime = 0.3f;

	// 横版动作平台跳跃手感:CharacterMovementComponent 默认值是为写实 3D 游戏调的,
	// 这里改成更快的下落速度、更高的空中控制力,让跳跃弧线更"脆"、走位更跟手。
	// 数值先给一版合理起点,具体手感后续可以直接在 BP_PlayerCharacter 的 Class Defaults 里继续微调。
	if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
	{
		MoveComp->MaxWalkSpeed = 600.f;
		MoveComp->JumpZVelocity = 700.f;
		MoveComp->GravityScale = 2.f;
		MoveComp->AirControl = 0.8f;
		MoveComp->BrakingDecelerationWalking = 2048.f;
	}
}

void APlayerCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

	if (AbilitySystemComponent && AttackAbilityClass)
	{
		AbilitySystemComponent->GiveAbility(FGameplayAbilitySpec(AttackAbilityClass, 1, 0));
	}
}
