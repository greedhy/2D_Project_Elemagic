// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayTagContainer.h"
#include "AbilitySystemComponent.h"
#include "ElementSystemComponent.generated.h"

class UElementComboInfo;
class UTexture2D;

/**
 * 技能栏单槽数据：由 UElementSystemComponent 维护，供 UI ViewModel 读取。
 * 需要 operator== 以支持 UE_MVVM_SET_PROPERTY_VALUE。
 */
USTRUCT(BlueprintType)
struct FElemagicSkillSlot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FGameplayTag AbilityTag;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TObjectPtr<UTexture2D> Icon;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FGameplayTag CooldownTag;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float CooldownRemaining = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bIsEmpty = true;

	bool operator==(const FElemagicSkillSlot& Other) const;
	bool operator!=(const FElemagicSkillSlot& Other) const { return !(*this == Other); }
};

/**
 * 元素系统核心组件：管理 3 槽元素装载 + 4 槽技能栏 + 合成。
 * 放在 APlayerCharacter 上，是元素/技能状态的唯一来源。
 */
UCLASS(ClassGroup = (Elemagic), meta = (BlueprintSpawnableComponent))
class ELEMAGIC_API UElementSystemComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UElementSystemComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	void Init(UAbilitySystemComponent* InASC);

	// === 元素装载（3 槽，未提交） ===
	UFUNCTION(BlueprintCallable, Category = "Element")
	bool AddElement(FGameplayTag ElementTag);

	UFUNCTION(BlueprintPure, Category = "Element")
	const TArray<FGameplayTag>& GetLoadout() const { return Loadout; }

	UFUNCTION(BlueprintPure, Category = "Element")
	FGameplayTag GetLoadoutSlot(int32 Index) const;

	UFUNCTION(BlueprintPure, Category = "Element")
	bool CanSynthesize() const;

	// === 合成 ===
	UFUNCTION(BlueprintCallable, Category = "Element")
	bool Synthesize();

	// === 技能栏（4 槽） ===
	UFUNCTION(BlueprintPure, Category = "Element")
	FElemagicSkillSlot GetSkillSlot(int32 Index) const;

	UFUNCTION(BlueprintCallable, Category = "Element")
	bool RemoveSkill(int32 SlotIndex);

	UFUNCTION(BlueprintCallable, Category = "Element")
	void ClearSkillBar();

	// === 输入路由（P5） ===
	// 处理 Input.Skill1-4 / Input.Synthesize，消费则返回 true。
	UFUNCTION(BlueprintCallable, Category = "Element")
	bool HandleInputTag(FGameplayTag InputTag);

	// UI 订阅此委托，收到后重新读取状态
	DECLARE_MULTICAST_DELEGATE(FOnElementSystemChanged);
	FOnElementSystemChanged OnElementSystemChanged;

	// 元素组合表（在 BP 中赋值）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Element")
	TObjectPtr<UElementComboInfo> ComboInfo;

	static constexpr int32 LoadoutSize = 3;
	static constexpr int32 SkillBarSize = 4;

private:
	int32 FindEmptyLoadoutSlot() const;
	int32 FindEmptySkillSlot() const;
	bool ActivateSkillSlot(int32 Index);

	TWeakObjectPtr<UAbilitySystemComponent> ASC;

	// 3 槽装载
	UPROPERTY()
	TArray<FGameplayTag> Loadout;

	// 4 槽技能栏（AbilitySpecHandle 索引与 SkillBarData 对齐）
	UPROPERTY()
	TArray<FGameplayAbilitySpecHandle> SkillBarHandles;

	UPROPERTY()
	TArray<FElemagicSkillSlot> SkillBarData;
};
