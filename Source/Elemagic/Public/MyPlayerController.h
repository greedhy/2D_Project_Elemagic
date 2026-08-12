// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "GameplayTagContainer.h"
#include "MyPlayerController.generated.h"

class UInputMappingContext;
class UInputAction;
class UEleInputConfig;
struct FInputActionValue;

/**
 * 玩家控制器:统一接管 Enhanced Input 的 Mapping Context 注册与动作绑定,
 * 角色(APlayerCharacter)本身不持有任何 Input 资源引用。
 * 移动/跳跃走原生 CharacterMovementComponent 接口;技能类输入通过 InputConfig
 * (GameplayTag -> InputAction 的数据资源)转发给 Pawn 的 AbilitySystemComponent,
 * 新增技能只需要在 InputConfig 资源里加一条映射,不需要改 C++。
 */
UCLASS()
class ELEMAGIC_API AMyPlayerController : public APlayerController
{
    GENERATED_BODY()

public:
    AMyPlayerController();

    // Combo 输入缓冲
    bool HasBufferedComboInput() const { return BufferedInputTag.IsValid(); }
    bool ConsumeBufferedComboInput(const FGameplayTag& ExpectedTag);

protected:
    virtual void BeginPlay() override;
    virtual void SetupInputComponent() override;

private:
    UPROPERTY(EditDefaultsOnly, Category = "Elemagic|Input")
    TObjectPtr<UInputMappingContext> PlayerMappingContext;

    UPROPERTY(EditDefaultsOnly, Category = "Elemagic|Input")
    TObjectPtr<UInputAction> MoveAction;

    UPROPERTY(EditDefaultsOnly, Category = "Elemagic|Input")
    TObjectPtr<UInputAction> JumpAction;

    // GameplayTag -> InputAction 映射资源(例如 IA_Attack_Light -> Ability.Attack.Light)。
    UPROPERTY(EditDefaultsOnly, Category = "Elemagic|Input")
    TObjectPtr<UEleInputConfig> InputConfig;

    void Move(const FInputActionValue& Value);
    void JumpPressed();
    void JumpReleased();

    void AbilityInputTagPressed(FGameplayTag InputTag);
    void AbilityInputTagReleased(FGameplayTag InputTag);
    void AbilityInputTagHeld(FGameplayTag InputTag);

    // Combo 输入缓冲
    FGameplayTag BufferedInputTag;
    float BufferedInputExpiryTime = 0.f;
    static constexpr float ComboBufferWindow = 0.3f;

    bool IsComboWindowOpen() const;
    void ClearBufferedInput();
};
