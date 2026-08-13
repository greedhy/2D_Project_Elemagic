// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "ElemagicUserWidget.generated.h"

class UMVVMViewModelBase;

/**
 * 项目所有 UMG Widget 的基类（View）。
 * 提供从 C++ 绑定 MVVM ViewModel 的入口，并在内部缓存 ViewModel 指针。
 * 注意：不重声明引擎的 SetViewModel/GetViewModel 名称，避免遮蔽。
 */
UCLASS(Abstract)
class ELEMAGIC_API UElemagicUserWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	// 设置引擎 MVVM ViewModel（供 UMG 绑定面板使用）并缓存。
	UFUNCTION(BlueprintCallable, Category = "Elemagic|UI")
	void BindViewModel(UMVVMViewModelBase* InViewModel);

	UFUNCTION(BlueprintPure, Category = "Elemagic|UI")
	UMVVMViewModelBase* GetBoundViewModel() const { return CachedViewModel; }

	template <typename T>
	T* GetBoundViewModelAs() const { return Cast<T>(CachedViewModel); }

protected:
	UPROPERTY(Transient)
	TObjectPtr<UMVVMViewModelBase> CachedViewModel;
};
