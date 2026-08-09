// Fill out your copyright notice in the Description page of Project Settings.

#include "CharacterBase.h"
#include "AbilitySystemComponent.h"
#include "CharacterAttributeSetBase.h"
#include "ElemagicGameplayTags.h"
#include "PaperFlipbookComponent.h"
#include "PaperFlipbook.h"
#include "GameFramework/CharacterMovementComponent.h"

ACharacterBase::ACharacterBase()
{
	PrimaryActorTick.bCanEverTick = true;

	// 横版 2D:角色运动被约束在 X-Z 平面,深度轴(Y)锁死
	if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
	{
		MoveComp->bConstrainToPlane = true;
		MoveComp->SetPlaneConstraintNormal(FVector(0.f, 1.f, 0.f));
		MoveComp->bSnapToPlaneAtStart = true;
	}

	bUseControllerRotationYaw = false;

	AbilitySystemComponent = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
	AbilitySystemComponent->SetIsReplicated(true);

	AttributeSet = CreateDefaultSubobject<UCharacterAttributeSetBase>(TEXT("AttributeSet"));
}

void ACharacterBase::BeginPlay()
{
	Super::BeginPlay();

	if (AbilitySystemComponent)
	{
		AbilitySystemComponent->GetGameplayAttributeValueChangeDelegate(AttributeSet->GetHealthAttribute())
			.AddUObject(this, &ACharacterBase::OnHealthChanged);
	}

	UpdateAnimation();
}

void ACharacterBase::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	UpdateAnimation();

	const float VelocityX = GetVelocity().X;
	if (!FMath::IsNearlyZero(VelocityX))
	{
		UpdateFacing(VelocityX);
	}
}

void ACharacterBase::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

	if (AbilitySystemComponent)
	{
		AbilitySystemComponent->InitAbilityActorInfo(this, this);
		InitializeAttributes();
	}
}

UAbilitySystemComponent* ACharacterBase::GetAbilitySystemComponent() const
{
	return AbilitySystemComponent;
}

bool ACharacterBase::IsDead() const
{
	return AbilitySystemComponent && AbilitySystemComponent->HasMatchingGameplayTag(ElemagicGameplayTags::State_Dead);
}

void ACharacterBase::InitializeAttributes()
{
	// 核心骨架阶段直接使用 AttributeSet 构造函数里的默认值;
	// 后续可替换为通过 GameplayEffect 应用职业/关卡强度相关的初始化数值。
}

void ACharacterBase::OnHealthChanged(const FOnAttributeChangeData& Data)
{
	if (Data.NewValue <= 0.f && !IsDead())
	{
		AbilitySystemComponent->AddLooseGameplayTag(ElemagicGameplayTags::State_Dead);
		Die();
	}
}

void ACharacterBase::Die_Implementation()
{
	if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
	{
		MoveComp->DisableMovement();
	}
	SetActorEnableCollision(false);
}

UPaperFlipbook* ACharacterBase::SelectFlipbookForState(bool bIsDashing, bool bIsFalling, bool bIsMoving,
	UPaperFlipbook* IdleFlipbook, UPaperFlipbook* RunFlipbook, UPaperFlipbook* JumpFlipbook, UPaperFlipbook* DashFlipbook)
{
	if (bIsDashing && DashFlipbook)
	{
		return DashFlipbook;
	}
	if (bIsFalling && JumpFlipbook)
	{
		return JumpFlipbook;
	}
	if (bIsMoving && RunFlipbook)
	{
		return RunFlipbook;
	}
	return IdleFlipbook;
}

void ACharacterBase::UpdateAnimation()
{
	UPaperFlipbookComponent* SpriteComp = GetSprite();
	if (!SpriteComp)
	{
		return;
	}

	const bool bIsDashing = AbilitySystemComponent && AbilitySystemComponent->HasMatchingGameplayTag(ElemagicGameplayTags::State_Dashing);
	const bool bIsFalling = GetCharacterMovement() && GetCharacterMovement()->IsFalling();
	const bool bIsMoving = !FMath::IsNearlyZero(GetVelocity().X);
	UPaperFlipbook* DesiredFlipbook = SelectFlipbookForState(bIsDashing, bIsFalling, bIsMoving,
		IdleFlipbook, RunFlipbook, JumpFlipbook, DashFlipbook);

	if (DesiredFlipbook && SpriteComp->GetFlipbook() != DesiredFlipbook)
	{
		SpriteComp->SetFlipbook(DesiredFlipbook);
	}
}

void ACharacterBase::UpdateFacing(float MoveDirectionX)
{
	const bool bShouldFaceRight = MoveDirectionX > 0.f;
	if (bShouldFaceRight == bFacingRight)
	{
		return;
	}
	bFacingRight = bShouldFaceRight;

	if (UPaperFlipbookComponent* SpriteComp = GetSprite())
	{
		FVector Scale = SpriteComp->GetRelativeScale3D();
		Scale.X = bFacingRight ? FMath::Abs(Scale.X) : -FMath::Abs(Scale.X);
		SpriteComp->SetRelativeScale3D(Scale);
	}
}
