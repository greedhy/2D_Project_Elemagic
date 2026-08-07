// Fill out your copyright notice in the Description page of Project Settings.


#include "MyGameModeBase.h"
#include "PlayerCharacter.h"
#include "MyPlayerController.h"
#include "MyHUD.h"

AMyGameModeBase::AMyGameModeBase()
{
	// 这里只设置原生 C++ 类作为兜底默认值。
	// 实际使用的角色/控制器蓝图(BP_PlayerCharacter、BP_MyPlayerController 等)
	// 通过本类的蓝图子类(BP_GameMode)在 Class Defaults 面板里覆盖指定,
	// 不在 C++ 里写死资源路径。
	DefaultPawnClass = APlayerCharacter::StaticClass();
	PlayerControllerClass = AMyPlayerController::StaticClass();
	HUDClass = AMyHUD::StaticClass();
}
