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

	if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
	{
		// 移动速度和跳跃高度以 EditDefaultsOnly 属性为准,构造中先写入默认值;
		// BP 子类的 Class Defaults 覆盖这些属性后,运行时取到的就是 BP 设置的值。
		MoveComp->MaxWalkSpeed = MoveSpeed;
		MoveComp->JumpZVelocity = JumpVelocity;
		JumpMaxHoldTime = JumpHoldTime;
		MoveComp->JumpMaxCount = JumpMaxCount;

		// 横版动作平台跳跃手感:CharacterMovementComponent 默认值是为写实 3D 游戏调的,
		// 这里改成更快的下落速度,让跳跃弧线更"脆"。
		MoveComp->GravityScale = 2.f;

		// 零惯性即时响应:按下方向键瞬间到位,松开瞬间停止,空中也跟手。
		MoveComp->MaxAcceleration = 99999.f;
		MoveComp->BrakingDecelerationWalking = 99999.f;
		MoveComp->AirControl = 1.f;
		MoveComp->bUseSeparateBrakingFriction = false;
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
