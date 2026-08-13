// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/ElemagicUserWidget.h"
#include "MVVMViewModelBase.h"
#include "MVVMBlueprintLibrary.h"

void UElemagicUserWidget::BindViewModel(UMVVMViewModelBase* InViewModel)
{
	CachedViewModel = InViewModel;
	if (InViewModel)
	{
		UMVVMBlueprintLibrary::SetViewModelByClass(this, InViewModel);
	}
}
