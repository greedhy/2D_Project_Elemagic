# DamageForm 伤害判定框架实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现基于碰撞体重叠的伤害判定框架(HitboxManager + AttackFrameData + CGF_Damage),并完成 GA_AttackBase→CGF_Damage / GA_DashBase→CGA_Dash 改名。

**Architecture:** 三层分层——UHitboxManager(框架层,归属 ACharacterBase,管理碰撞体+计时器查表+Overlap→伤害)、CGF_Damage(技能层,纯编排器:喂数据给框架)、UAttackFrameData(数据层,DataAsset 帧配置数组)。碰撞通道新增 Hitbox/Hurtbox 两个 ObjectChannel。伤害通过 SetByCaller[IncomingDamage] → PostGameplayEffectExecute 管道。

**Tech Stack:** Unreal Engine 5, C++ (GameplayAbilities, Paper2D, GameplayTags), Blueprint (子类配置)

## 前置依赖

Task 2,3,4 无相互依赖,可并行。Task 5 依赖 1,2,3。Task 6 依赖 4,5。Task 7,8 依赖 5,6。Task 9 依赖 7。Task 10 依赖全部。

```
Task 1 (FAttackFrameConfig)
  └─ Task 5 (UHitboxManager) ─ Task 6 (ACharacterBase 集成) ─ Task 7 (CGF_Damage) ─ Task 9 (IncomingDamage) ─ Task 10 (测试)
Task 2 (UAttackFrameData) ──┘                                                          
Task 3 (Event.Attack.* Tags) ─┘                                        Task 8 (CGA_Dash 改名)
Task 4 (碰撞通道) ───────── Task 6 ──────────────────────────────────┘                    
                                                                      
```

## Global Constraints

- 测试必须 spawn 实例,不依赖 CDO (PostInit 参数在 CDO 上不可用)
- 命名规范: CGF_=框架, CGA_=特定能力, GA_/GE_=BP 子类
- 计时器驱动判定,不依赖动画帧同步
- 所有角色(玩家+怪物)共用 HitboxManager 基础设施
- GA_DashBase→CGA_Dash 改名后,需同步更新 Blueprint 引用

---

### Task 1: FAttackFrameConfig 结构体 + UAttackFrameData DataAsset

**Files:**
- Create: `Source/Elemagic/Public/AttackFrameData.h`
- Create: `Source/Elemagic/Private/AttackFrameData.cpp`

**Interfaces:**
- Produces:
  - `struct FAttackFrameConfig` — NormalizedTime(float), HitboxExtent(FVector2D), HitboxOffset(FVector2D), EventTags(FGameplayTagContainer), DamageMultiplier(float, 默认1.0), HitImpulse(FVector2D)
  - `class UAttackFrameData : public UDataAsset` — SourceAnimation(TSoftObjectPtr<UPaperFlipbook>), Frames(TArray<FAttackFrameConfig>), `GetTotalDuration()` const float

- [ ] **Step 1: 创建 AttackFrameData.h**

```cpp
// Source/Elemagic/Public/AttackFrameData.h
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "AttackFrameData.generated.h"

/**
 * 单个攻击帧配置:定义某个标准化时间点上的 Hitbox 参数/事件/伤害倍率。
 * NormalizedTime 为 0.0~1.0 的比例值,与动画实际时长无关,
 * 实际时间由 Flipbook->GetTotalDuration() × NormalizedTime 计算。
 */
USTRUCT(BlueprintType)
struct FAttackFrameConfig
{
    GENERATED_BODY()

    // 触发时间点(0.0=动画开始, 1.0=动画结束)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frame")
    float NormalizedTime = 0.f;

    // 碰撞体半尺寸(X=宽/2, Y=高/2)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frame")
    FVector2D HitboxExtent = FVector2D(32.f, 32.f);

    // 碰撞体相对角色中心偏移
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frame")
    FVector2D HitboxOffset = FVector2D(0.f, 0.f);

    // 帧事件 Tag(EnableHitbox/DisableHitbox/ResetHitTargets/EndAttack)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frame")
    FGameplayTagContainer EventTags;

    // 当前帧命中伤害倍率(叠加到 AttackPower 上)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frame")
    float DamageMultiplier = 1.f;

    // 命中击退力(最终击退 = BaseImpulse + HitImpulse)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frame")
    FVector2D HitImpulse = FVector2D::ZeroVector;
};

/**
 * 攻击帧数据资产:一个攻击动画对应一个 DataAsset,
 * Frames 按 NormalizedTime 升序排列。
 */
UCLASS(BlueprintType)
class ELEMAGIC_API UAttackFrameData : public UDataAsset
{
    GENERATED_BODY()

public:
    // 配对的 Flipbook,用于获取总时长 + 编辑器对照(不参与运行时播放)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack")
    TSoftObjectPtr<class UPaperFlipbook> SourceAnimation;

    // 帧配置数组,按 NormalizedTime 升序排列
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack")
    TArray<FAttackFrameConfig> Frames;

    // 获取动画总时长(秒),优先从 SourceAnimation 获取,
    // fallback 用最后一帧 NormalizedTime 反算(默认 0.5s)
    float GetTotalDuration() const;
};
```

- [ ] **Step 2: 创建 AttackFrameData.cpp**

```cpp
// Source/Elemagic/Private/AttackFrameData.cpp
#include "AttackFrameData.h"

float UAttackFrameData::GetTotalDuration() const
{
    if (!SourceAnimation.IsNull())
    {
        if (UPaperFlipbook* Flipbook = SourceAnimation.LoadSynchronous())
        {
            return Flipbook->GetTotalDuration();
        }
    }

    // Fallback: 最后一帧 NormalizedTime 反算,默认 0.5s 总时长
    if (Frames.Num() > 0)
    {
        const float LastTime = Frames.Last().NormalizedTime;
        if (LastTime > 0.f)
        {
            return LastTime * 0.5f / LastTime; // 保持比例,假设最后一帧=动画结束
        }
    }
    return 0.5f;
}
```

- [ ] **Step 3: 编译检查**

PowerShell 中 Build:
```
& "C:\Program Files\Epic Games\UE_5.5\Engine\Build\BatchFiles\Build.bat" ElemagicEditor Win64 Development -Project="C:\Users\greedy\Documents\Unreal Projects\Elemagic\Elemagic.uproject" -WaitMutex
```

- [ ] **Step 4: Commit**

```bash
git add Source/Elemagic/Public/AttackFrameData.h Source/Elemagic/Private/AttackFrameData.cpp
git commit -m "feat: add FAttackFrameConfig struct and UAttackFrameData DataAsset"
```

---

### Task 2: Event.Attack.* GameplayTags

**Files:**
- Modify: `Source/Elemagic/Public/ElemagicGameplayTags.h:22-23`
- Modify: `Source/Elemagic/Private/ElemagicGameplayTags.cpp:19-20`

**Interfaces:**
- Produces:
  - `Event_Attack_EnableHitbox` ("Event.Attack.EnableHitbox")
  - `Event_Attack_DisableHitbox` ("Event.Attack.DisableHitbox")
  - `Event_Attack_ResetHitTargets` ("Event.Attack.ResetHitTargets")
  - `Event_Attack_EndAttack` ("Event.Attack.EndAttack")

- [ ] **Step 1: 在 ElemagicGameplayTags.h 声明新 Tag**

在 `Data_Damage` 声明后添加:

```cpp
// Event.Attack.* Tags
ELEMAGIC_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Attack_EnableHitbox);
ELEMAGIC_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Attack_DisableHitbox);
ELEMAGIC_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Attack_ResetHitTargets);
ELEMAGIC_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Attack_EndAttack);
```

- [ ] **Step 2: 在 ElemagicGameplayTags.cpp 定义新 Tag**

在 `Data_Damage` 定义后添加:

```cpp
UE_DEFINE_GAMEPLAY_TAG(Event_Attack_EnableHitbox, "Event.Attack.EnableHitbox");
UE_DEFINE_GAMEPLAY_TAG(Event_Attack_DisableHitbox, "Event.Attack.DisableHitbox");
UE_DEFINE_GAMEPLAY_TAG(Event_Attack_ResetHitTargets, "Event.Attack.ResetHitTargets");
UE_DEFINE_GAMEPLAY_TAG(Event_Attack_EndAttack, "Event.Attack.EndAttack");
```

- [ ] **Step 3: 编译检查**

- [ ] **Step 4: Commit**

```bash
git add Source/Elemagic/Public/ElemagicGameplayTags.h Source/Elemagic/Private/ElemagicGameplayTags.cpp
git commit -m "feat: add Event.Attack.* gameplay tags (EnableHitbox/DisableHitbox/ResetHitTargets/EndAttack)"
```

---

### Task 3: Hitbox / Hurtbox 碰撞通道配置

**Files:**
- Modify: `Config/DefaultEngine.ini`

**Interfaces:**
- Produces: 两个新 ObjectChannel — `Hitbox` (GameTraceChannel1) 和 `Hurtbox` (GameTraceChannel2)

- [ ] **Step 1: 在 DefaultEngine.ini 添加碰撞通道配置**

在 `Config/DefaultEngine.ini` 末尾添加:

```ini
[/Script/Engine.CollisionProfile]
+DefaultChannelResponses=(Channel=ECC_GameTraceChannel1,Name="Hitbox",DefaultResponse=ECR_Overlap)
+DefaultChannelResponses=(Channel=ECC_GameTraceChannel2,Name="Hurtbox",DefaultResponse=ECR_Overlap)
+EditProfiles=(Name="Hitbox",CustomResponses=((Channel="Hurtbox",Response=ECR_Overlap)))
+EditProfiles=(Name="Hurtbox",CustomResponses=((Channel="Hitbox",Response=ECR_Overlap)))
```

- [ ] **Step 2: 验证——编辑器重启后检查 Project Settings → Collision**

在 UE5 Editor 中:
1. Edit → Project Settings → Engine → Collision
2. 确认 Object Channels 列表中有 Hitbox 和 Hurtbox
3. 确认 Hitbox 默认对 Hurtbox 为 Overlap,Hurtbox 默认对 Hitbox 为 Overlap

- [ ] **Step 3: Commit**

```bash
git add Config/DefaultEngine.ini
git commit -m "feat: add Hitbox and Hurtbox collision object channels"
```

---

### Task 4: UHitboxManager 组件

**Files:**
- Create: `Source/Elemagic/Public/HitboxManager.h`
- Create: `Source/Elemagic/Private/HitboxManager.cpp`

**Interfaces:**
- Consumes:
  - `FAttackFrameConfig` / `UAttackFrameData` (Task 1)
  - `Event_Attack_*` gameplay tags (Task 2)
- Produces:
  - `class UHitboxManager : public UActorComponent`
  - `void Init(UBoxComponent* InAttackHitbox, UBoxComponent* InHurtbox)`
  - `void BeginAttack(UAttackFrameData* Data, TSubclassOf<UGameplayEffect> DamageGE, FVector2D BaseImpulse)`
  - `void EndAttack()`
  - `UFUNCTION() void OnAttackHitboxOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)`
  - `FOnAttackHit` delegate (供 CGF_Damage 或其他系统监听)

- [ ] **Step 1: 创建 HitboxManager.h**

```cpp
// Source/Elemagic/Public/HitboxManager.h
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayTagContainer.h"
#include "HitboxManager.generated.h"

class UBoxComponent;
class UAttackFrameData;
class UGameplayEffect;
class UAbilitySystemComponent;

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnAttackHit, AActor* /*Target*/, float /*FinalDamage*/);

UCLASS(ClassGroup = (Elemagic), meta = (BlueprintSpawnableComponent))
class ELEMAGIC_API UHitboxManager : public UActorComponent
{
    GENERATED_BODY()

public:
    UHitboxManager();

    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;

    // 由 ACharacterBase 构造函数调用,绑定碰撞体
    void Init(UBoxComponent* InAttackHitbox, UBoxComponent* InHurtbox);

    // 开始一次攻击
    void BeginAttack(UAttackFrameData* Data, TSubclassOf<UGameplayEffect> DamageGE, FVector2D BaseImpulse);

    // 结束攻击(关闭 Hitbox + 清空去重集合)
    void EndAttack();

    // AttackHitbox 碰到其他角色 Hurtbox 的回调
    UFUNCTION()
    void OnAttackHitboxOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
        UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

    FOnAttackHit OnAttackHit;

protected:
    UPROPERTY()
    TObjectPtr<UBoxComponent> AttackHitbox;

    UPROPERTY()
    TObjectPtr<UBoxComponent> Hurtbox;

private:
    void ProcessFrameConfig(const struct FAttackFrameConfig& Config);
    void ApplyDamage(AActor* Target, float DamageMultiplier);

    UPROPERTY()
    TObjectPtr<UAttackFrameData> CurrentFrameData;

    UPROPERTY()
    TSubclassOf<UGameplayEffect> CurrentDamageEffectClass;

    FVector2D CurrentBaseImpulse = FVector2D::ZeroVector;

    bool bActive = false;
    float StartTime = 0.f;
    float TotalDuration = 0.f;
    TSet<TWeakObjectPtr<AActor>> HitTargets;
    int32 LastProcessedConfigIdx = 0;

    // 当前激活帧的伤害倍率(Overlap 回调时读取)
    float CurrentDamageMultiplier = 1.f;
    FVector2D CurrentHitImpulse = FVector2D::ZeroVector;
};
```

- [ ] **Step 2: 创建 HitboxManager.cpp**

```cpp
// Source/Elemagic/Private/HitboxManager.cpp
#include "HitboxManager.h"
#include "AttackFrameData.h"
#include "ElemagicGameplayTags.h"
#include "CharacterAttributeSetBase.h"
#include "Components/BoxComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "GameplayEffect.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

UHitboxManager::UHitboxManager()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;
}

void UHitboxManager::Init(UBoxComponent* InAttackHitbox, UBoxComponent* InHurtbox)
{
    AttackHitbox = InAttackHitbox;
    Hurtbox = InHurtbox;

    if (AttackHitbox)
    {
        AttackHitbox->OnComponentBeginOverlap.AddDynamic(this, &UHitboxManager::OnAttackHitboxOverlap);
    }
}

void UHitboxManager::BeginAttack(UAttackFrameData* Data, TSubclassOf<UGameplayEffect> DamageGE, FVector2D BaseImpulse)
{
    if (!Data || Data->Frames.Num() == 0)
    {
        return;
    }

    CurrentFrameData = Data;
    CurrentDamageEffectClass = DamageGE;
    CurrentBaseImpulse = BaseImpulse;
    TotalDuration = Data->GetTotalDuration();
    StartTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
    LastProcessedConfigIdx = 0;
    HitTargets.Reset();
    bActive = true;

    SetComponentTickEnabled(true);
}

void UHitboxManager::EndAttack()
{
    bActive = false;
    SetComponentTickEnabled(false);

    if (AttackHitbox)
    {
        AttackHitbox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    }

    CurrentFrameData = nullptr;
    CurrentDamageEffectClass = nullptr;
    CurrentBaseImpulse = FVector2D::ZeroVector;
    CurrentDamageMultiplier = 1.f;
    CurrentHitImpulse = FVector2D::ZeroVector;
    HitTargets.Reset();
    LastProcessedConfigIdx = 0;
}

void UHitboxManager::TickComponent(float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (!bActive || !CurrentFrameData || !GetWorld())
    {
        return;
    }

    const float Elapsed = GetWorld()->GetTimeSeconds() - StartTime;
    const float NormalizedTime = FMath::Clamp(Elapsed / TotalDuration, 0.f, 1.f);

    const TArray<FAttackFrameConfig>& Frames = CurrentFrameData->Frames;

    // 增量处理从上次游标到当前时间区间内的所有配置节点
    for (int32 i = LastProcessedConfigIdx; i < Frames.Num(); ++i)
    {
        if (Frames[i].NormalizedTime <= NormalizedTime)
        {
            ProcessFrameConfig(Frames[i]);
            LastProcessedConfigIdx = i + 1;
        }
        else
        {
            break;
        }
    }

    // 动画播放完毕自动结束
    if (NormalizedTime >= 1.f)
    {
        EndAttack();
    }
}

void UHitboxManager::ProcessFrameConfig(const FAttackFrameConfig& Config)
{
    if (!AttackHitbox)
    {
        return;
    }

    if (Config.EventTags.HasTagExact(ElemagicGameplayTags::Event_Attack_EnableHitbox))
    {
        AttackHitbox->SetBoxExtent(FVector(Config.HitboxExtent.X, 1.f, Config.HitboxExtent.Y));
        AttackHitbox->SetRelativeLocation(FVector(Config.HitboxOffset.X, 0.f, Config.HitboxOffset.Y));
        AttackHitbox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        CurrentDamageMultiplier = Config.DamageMultiplier;
        CurrentHitImpulse = Config.HitImpulse;
    }

    if (Config.EventTags.HasTagExact(ElemagicGameplayTags::Event_Attack_DisableHitbox))
    {
        AttackHitbox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        CurrentDamageMultiplier = 1.f;
        CurrentHitImpulse = FVector2D::ZeroVector;
    }

    if (Config.EventTags.HasTagExact(ElemagicGameplayTags::Event_Attack_ResetHitTargets))
    {
        HitTargets.Reset();
    }

    if (Config.EventTags.HasTagExact(ElemagicGameplayTags::Event_Attack_EndAttack))
    {
        EndAttack();
    }
}

void UHitboxManager::OnAttackHitboxOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
    UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
    if (!bActive || !OtherActor || !CurrentDamageEffectClass)
    {
        return;
    }

    // 不攻击自己
    if (OtherActor == GetOwner())
    {
        return;
    }

    // 检查 OtherComp 是否是 Hurtbox 通道(用碰撞对象类型判断)
    if (!OtherComp || OtherComp->GetCollisionObjectType() != ECC_GameTraceChannel2) // Hurtbox
    {
        return;
    }

    // 去重:本段内同一目标只命中一次
    if (HitTargets.Contains(OtherActor))
    {
        return;
    }

    ApplyDamage(OtherActor, CurrentDamageMultiplier);
    HitTargets.Add(OtherActor);
}

void UHitboxManager::ApplyDamage(AActor* Target, float DamageMultiplier)
{
    UAbilitySystemComponent* SourceASC = GetOwner()
        ? GetOwner()->FindComponentByClass<UAbilitySystemComponent>()
        : nullptr;
    UAbilitySystemComponent* TargetASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Target);
    if (!SourceASC || !TargetASC)
    {
        return;
    }

    // 从源角色属性集读 AttackPower,计算最终伤害
    float AttackPower = 0.f;
    if (const UCharacterAttributeSetBase* AttrSet = SourceASC->GetSet<UCharacterAttributeSetBase>())
    {
        AttackPower = AttrSet->GetAttackPower();
    }
    const float FinalDamage = AttackPower * DamageMultiplier;

    // 源 ASC 创建 GE Spec,SetByCaller 传递伤害值(GE 中需配置 SetByCaller Magnitude)
    FGameplayEffectSpecHandle SpecHandle = SourceASC->MakeOutgoingSpec(
        CurrentDamageEffectClass, 1.f, SourceASC->MakeEffectContext());

    if (SpecHandle.IsValid())
    {
        SpecHandle.Data->SetSetByCallerMagnitude(ElemagicGameplayTags::Data_Damage, FinalDamage);
        TargetASC->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data.Get());

        OnAttackHit.Broadcast(Target, FinalDamage);
    }

    // 施加击退:最终击退 = BaseImpulse + 当前帧 HitImpulse
    const FVector2D FinalImpulse = CurrentBaseImpulse + CurrentHitImpulse;
    if (!FinalImpulse.IsNearlyZero() && Target)
    {
        if (ACharacter* TargetChar = Cast<ACharacter>(Target))
        {
            if (UCharacterMovementComponent* MoveComp = TargetChar->GetCharacterMovement())
            {
                // 按源角色朝向方向施加击退
                const float DirectionSign = (GetOwner() && GetOwner()->GetActorForwardVector().X > 0.f) ? 1.f : -1.f;
                MoveComp->Velocity += FVector(FinalImpulse.X * DirectionSign, 0.f, FinalImpulse.Y);
            }
        }
    }
}
```

- [ ] **Step 3: 编译检查**

- [ ] **Step 4: Commit**

```bash
git add Source/Elemagic/Public/HitboxManager.h Source/Elemagic/Private/HitboxManager.cpp
git commit -m "feat: add UHitboxManager component with timer-driven frame processing"
```

---

### Task 5: ACharacterBase 集成 HitboxManager + AttackHitbox + Hurtbox

**Files:**
- Modify: `Source/Elemagic/Public/CharacterBase.h`
- Modify: `Source/Elemagic/Private/CharacterBase.cpp`

**Interfaces:**
- Consumes:
  - UHitboxManager (Task 4)
  - Collision channels Hitbox/Hurtbox (Task 3)
- Produces:
  - `ACharacterBase` 新增 `UBoxComponent* AttackHitbox` / `UBoxComponent* Hurtbox` / `UHitboxManager* HitboxManager`

- [ ] **Step 1: 在 CharacterBase.h 添加 forward declaration 和成员**

在 `CharacterBase.h` 的 class forward declarations 区添加:

```cpp
class UBoxComponent;
class UHitboxManager;
```

在 `protected:` 区域 AttributeSet 下面添加:

```cpp
UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Elemagic|Combat")
TObjectPtr<UBoxComponent> AttackHitbox;

UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Elemagic|Combat")
TObjectPtr<UBoxComponent> Hurtbox;

UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Elemagic|Combat")
TObjectPtr<UHitboxManager> HitboxManager;
```

- [ ] **Step 2: 在 CharacterBase.cpp 构造函数中创建组件**

在构造函数中,`AttributeSet` 创建后面添加:

```cpp
// Hitbox/Hurtbox 碰撞通道常量(需与 DefaultEngine.ini 中配置一致)
static const ECollisionChannel HITBOX_CHANNEL = ECC_GameTraceChannel1;
static const ECollisionChannel HURTBOX_CHANNEL = ECC_GameTraceChannel2;

AttackHitbox = CreateDefaultSubobject<UBoxComponent>(TEXT("AttackHitbox"));
AttackHitbox->SetupAttachment(RootComponent);
AttackHitbox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
AttackHitbox->SetCollisionObjectType(HITBOX_CHANNEL);
AttackHitbox->SetCollisionResponseToAllChannels(ECR_Ignore);
AttackHitbox->SetCollisionResponseToChannel(HURTBOX_CHANNEL, ECR_Overlap);
AttackHitbox->SetBoxExtent(FVector(32.f, 1.f, 32.f));
AttackHitbox->SetGenerateOverlapEvents(true);

Hurtbox = CreateDefaultSubobject<UBoxComponent>(TEXT("Hurtbox"));
Hurtbox->SetupAttachment(RootComponent);
Hurtbox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
Hurtbox->SetCollisionObjectType(HURTBOX_CHANNEL);
Hurtbox->SetCollisionResponseToAllChannels(ECR_Ignore);
Hurtbox->SetCollisionResponseToChannel(HITBOX_CHANNEL, ECR_Overlap);
Hurtbox->SetBoxExtent(FVector(32.f, 1.f, 48.f));
Hurtbox->SetGenerateOverlapEvents(true);

HitboxManager = CreateDefaultSubobject<UHitboxManager>(TEXT("HitboxManager"));
HitboxManager->Init(AttackHitbox, Hurtbox);
```

在 `ACharacterBase` include 中添加:

```cpp
#include "Components/BoxComponent.h"
#include "HitboxManager.h"
```

- [ ] **Step 3: 编译检查**

- [ ] **Step 4: Commit**

```bash
git add Source/Elemagic/Public/CharacterBase.h Source/Elemagic/Private/CharacterBase.cpp
git commit -m "feat: integrate UHitboxManager, AttackHitbox, and Hurtbox into ACharacterBase"
```

---

### Task 6: 重构 GA_AttackBase → CGF_Damage

**Files:**
- Rename: `Source/Elemagic/Public/GA_AttackBase.h` → `Source/Elemagic/Public/CGF_Damage.h`
- Rename: `Source/Elemagic/Private/GA_AttackBase.cpp` → `Source/Elemagic/Private/CGF_Damage.cpp`
- Modify: `Source/Elemagic/Public/CGF_Damage.h` (类改名 + 接口重构)
- Modify: `Source/Elemagic/Private/CGF_Damage.cpp` (实现重构)

**Interfaces:**
- Consumes:
  - UHitboxManager (Task 4)
  - UAttackFrameData (Task 1)
  - ACharacterBase 新成员 (Task 5)
- Produces:
  - `class UGA_CGF_Damage : public UElemagicGameplayAbility` (注意:原 spec 命名 CGF_Damage,UE 前缀为 U,实际类名 `UCGF_Damage`)
  - 属性: AttackAnimation(UPaperFlipbook*), FrameData(UAttackFrameData*), DamageEffectClass(TSubclassOf<UGameplayEffect>), BaseImpulse(FVector2D), AbilityAttackTag(FGameplayTag)
  - 移除 PerformAttack() BlueprintImplementableEvent

- [ ] **Step 1: 创建新的 CGF_Damage.h**

> **注意:** spec 中的 `CGF_Damage` 去掉 UE 前缀后类名为 `UCGF_Damage`,符合 UE 命名规范。

```cpp
// Source/Elemagic/Public/CGF_Damage.h
#pragma once

#include "CoreMinimal.h"
#include "ElemagicGameplayAbility.h"
#include "GameplayTagContainer.h"
#include "CGF_Damage.generated.h"

class UPaperFlipbook;
class UAttackFrameData;
class UGameplayEffect;
class UHitboxManager;
class ACharacterBase;

/**
 * 伤害判定框架的攻击能力基类(CGF = C++ GameplayForm)。
 * 纯编排器:激活时把 FrameData/DamageGE/BaseImpulse 喂给 HitboxManager,
 * 播放 AttackAnimation(纯视觉),命中判定由 HitboxManager 的计时器驱动。
 * 不再有 PerformAttack() BlueprintImplementableEvent。
 */
UCLASS()
class ELEMAGIC_API UCGF_Damage : public UElemagicGameplayAbility
{
    GENERATED_BODY()

public:
    UCGF_Damage();

    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
        const FGameplayAbilityActorInfo* ActorInfo,
        const FGameplayAbilityActivationInfo ActivationInfo,
        const FGameplayEventData* TriggerEventData) override;

    virtual void EndAbility(const FGameplayAbilitySpecHandle Handle,
        const FGameplayAbilityActorInfo* ActorInfo,
        const FGameplayAbilityActivationInfo ActivationInfo,
        bool bReplicateCancel, bool bEndedByCancel) override;

    // 攻击动画 Flipbook(纯视觉播放,不驱动判定)
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "CGF_Damage")
    TObjectPtr<UPaperFlipbook> AttackAnimation;

    // 帧数据资产
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "CGF_Damage")
    TObjectPtr<UAttackFrameData> FrameData;

    // 伤害 GE 类(通过 SetByCaller[Data.Damage] 传递伤害值)
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "CGF_Damage")
    TSubclassOf<UGameplayEffect> DamageEffectClass;

    // 底板击退力(每帧 HitImpulse 叠加在此之上)
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "CGF_Damage")
    FVector2D BaseImpulse = FVector2D::ZeroVector;

    // 子类自定义 Ability Tag(如 Ability.Attack.Light)
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "CGF_Damage")
    FGameplayTag AbilityAttackTag;

private:
    UHitboxManager* GetHitboxManager() const;
};
```

- [ ] **Step 2: 创建新的 CGF_Damage.cpp**

```cpp
// Source/Elemagic/Private/CGF_Damage.cpp
#include "CGF_Damage.h"
#include "CharacterBase.h"
#include "HitboxManager.h"
#include "AttackFrameData.h"
#include "ElemagicGameplayTags.h"
#include "PaperFlipbookComponent.h"
#include "PaperFlipbook.h"
#include "AbilitySystemComponent.h"

UCGF_Damage::UCGF_Damage()
{
    AbilityTags.AddTag(ElemagicGameplayTags::Ability_Attack);
    ActivationOwnedTags.AddTag(ElemagicGameplayTags::State_Attacking);
    ActivationBlockedTags.AddTag(ElemagicGameplayTags::State_Attacking);
}

void UCGF_Damage::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
    const FGameplayAbilityActorInfo* ActorInfo,
    const FGameplayAbilityActivationInfo ActivationInfo,
    const FGameplayEventData* TriggerEventData)
{
    if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
    {
        EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
        return;
    }

    // 校验:FrameData 的 SourceAnimation 应与 AttackAnimation 匹配
    if (FrameData && AttackAnimation &&
        FrameData->SourceAnimation.LoadSynchronous() != AttackAnimation)
    {
        UE_LOG(LogTemp, Warning, TEXT("[CGF_Damage] FrameData.SourceAnimation mismatch with AttackAnimation for %s"),
            *GetName());
    }

    UHitboxManager* HitboxMan = GetHitboxManager();
    if (!HitboxMan || !FrameData)
    {
        UE_LOG(LogTemp, Warning, TEXT("[CGF_Damage] HitboxManager or FrameData is null, ending ability"));
        EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
        return;
    }

    // 将数据喂给 HitboxManager
    HitboxMan->BeginAttack(FrameData, DamageEffectClass, BaseImpulse);

    // 播放动画(纯视觉)
    ACharacterBase* CharBase = Cast<ACharacterBase>(ActorInfo->AvatarActor.Get());
    if (CharBase && AttackAnimation)
    {
        if (UPaperFlipbookComponent* Sprite = CharBase->GetSprite())
        {
            Sprite->SetFlipbook(AttackAnimation);
            Sprite->PlayFromStart();
        }
    }
}

void UCGF_Damage::EndAbility(const FGameplayAbilitySpecHandle Handle,
    const FGameplayAbilityActorInfo* ActorInfo,
    const FGameplayAbilityActivationInfo ActivationInfo,
    bool bReplicateCancel, bool bEndedByCancel)
{
    if (UHitboxManager* HitboxMan = GetHitboxManager())
    {
        HitboxMan->EndAttack();
    }

    Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateCancel, bEndedByCancel);
}

UHitboxManager* UCGF_Damage::GetHitboxManager() const
{
    const ACharacterBase* CharBase = Cast<ACharacterBase>(GetAvatarActorFromActorInfo());
    if (CharBase)
    {
        return CharBase->HitboxManager;
    }
    return nullptr;
}
```

- [ ] **Step 3: 删除旧文件并更新后编译**

PowerShell 中:
```powershell
# 删除旧 GA_AttackBase 文件
Remove-Item "Source/Elemagic/Public/GA_AttackBase.h"
Remove-Item "Source/Elemagic/Private/GA_AttackBase.cpp"
```

- [ ] **Step 4: 编译检查**

- [ ] **Step 5: Commit**

```bash
git rm Source/Elemagic/Public/GA_AttackBase.h Source/Elemagic/Private/GA_AttackBase.cpp
git add Source/Elemagic/Public/CGF_Damage.h Source/Elemagic/Private/CGF_Damage.cpp
git commit -m "refactor: rename GA_AttackBase to CGF_Damage with HitboxManager orchestration"
```

---

### Task 7: 重命名 GA_DashBase → CGA_Dash

**Files:**
- Rename: `Source/Elemagic/Public/GA_DashBase.h` → `Source/Elemagic/Public/CGA_Dash.h`
- Rename: `Source/Elemagic/Private/GA_DashBase.cpp` → `Source/Elemagic/Private/CGA_Dash.cpp`
- Modify: `Source/Elemagic/Public/CGA_Dash.h` (类名 UGA_DashBase → UCGA_Dash)
- Modify: `Source/Elemagic/Private/CGA_Dash.cpp` (构造函数类名 + include)
- Modify: `Source/Elemagic/Private/Tests/PlayerCharacterDashTests.cpp` (更新 include + 类引用)
- 需要在 Editor 中更新 BP 子类引用(Reparent Blueprint)

**Interfaces:**
- Consumes: 无新依赖
- Produces:
  - `class UCGA_Dash : public UElemagicGameplayAbility` (仅改名,接口不变)

- [ ] **Step 1: 创建 CGA_Dash.h**

```cpp
// Source/Elemagic/Public/CGA_Dash.h
#pragma once

#include "CoreMinimal.h"
#include "ElemagicGameplayAbility.h"
#include "CGA_Dash.generated.h"

/**
 * 冲刺能力基类(CGA = C++ GameplayAbility)。
 * 面朝方向水平位移,带无敌帧。
 * 冷却时长/空中次数等配置由蓝图子类在 Class Defaults 中设置。
 * 输入通过 InputConfig 的 Ability.Dash -> ASC::TryActivateAbilitiesByTag 路由。
 */
UCLASS()
class ELEMAGIC_API UCGA_Dash : public UElemagicGameplayAbility
{
    GENERATED_BODY()

public:
    UCGA_Dash();

    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
        const FGameplayAbilityActorInfo* ActorInfo,
        const FGameplayAbilityActivationInfo ActivationInfo,
        const FGameplayEventData* TriggerEventData) override;

    virtual void EndAbility(const FGameplayAbilitySpecHandle Handle,
        const FGameplayAbilityActorInfo* ActorInfo,
        const FGameplayAbilityActivationInfo ActivationInfo,
        bool bReplicateCancel, bool bEndedByCancel) override;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Dash")
    float DashSpeed = 2667.f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Dash")
    float DashDuration = 0.15f;

private:
    void DashTick();
    int32 GetDashDirectionSign() const;
    bool IsInAir() const;

    FTimerHandle DashTickTimer;
    FTimerHandle DashEndTimer;
};
```

- [ ] **Step 2: 创建 CGA_Dash.cpp**

```cpp
// Source/Elemagic/Private/CGA_Dash.cpp
#include "CGA_Dash.h"
#include "CharacterBase.h"
#include "ElemagicGameplayTags.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "AbilitySystemComponent.h"

UCGA_Dash::UCGA_Dash()
{
    AbilityTags.AddTag(ElemagicGameplayTags::Ability_Dash);

    ActivationOwnedTags.AddTag(ElemagicGameplayTags::State_Dashing);

    ActivationBlockedTags.AddTag(ElemagicGameplayTags::State_Dashing);
    ActivationBlockedTags.AddTag(ElemagicGameplayTags::State_DashedInAir);
}

void UCGA_Dash::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
    const FGameplayAbilityActorInfo* ActorInfo,
    const FGameplayAbilityActivationInfo ActivationInfo,
    const FGameplayEventData* TriggerEventData)
{
    if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
    {
        EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
        return;
    }

    UAbilitySystemComponent* ASC = ActorInfo->AbilitySystemComponent.Get();
    ACharacter* Character = Cast<ACharacter>(ActorInfo->AvatarActor.Get());
    if (!ASC || !Character)
    {
        EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
        return;
    }

    ASC->AddLooseGameplayTag(ElemagicGameplayTags::State_Invulnerable);

    if (IsInAir())
    {
        ASC->AddLooseGameplayTag(ElemagicGameplayTags::State_DashedInAir);
    }

    UWorld* World = GetWorld();
    if (World)
    {
        World->GetTimerManager().SetTimer(DashTickTimer, this, &UCGA_Dash::DashTick, 0.001f, true);
        World->GetTimerManager().SetTimer(DashEndTimer,
            [this, Handle, ActorInfo, ActivationInfo]()
            {
                EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
            },
            DashDuration, false);
    }

    DashTick();
}

void UCGA_Dash::EndAbility(const FGameplayAbilitySpecHandle Handle,
    const FGameplayAbilityActorInfo* ActorInfo,
    const FGameplayAbilityActivationInfo ActivationInfo,
    bool bReplicateCancel, bool bEndedByCancel)
{
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(DashTickTimer);
        World->GetTimerManager().ClearTimer(DashEndTimer);
    }

    if (UAbilitySystemComponent* ASC = ActorInfo->AbilitySystemComponent.Get())
    {
        ASC->RemoveLooseGameplayTag(ElemagicGameplayTags::State_Invulnerable);
    }

    Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateCancel, bEndedByCancel);
}

void UCGA_Dash::DashTick()
{
    ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
    if (!Character) return;

    UCharacterMovementComponent* MoveComp = Character->GetCharacterMovement();
    if (!MoveComp) return;

    MoveComp->Velocity.X = GetDashDirectionSign() * DashSpeed;
}

int32 UCGA_Dash::GetDashDirectionSign() const
{
    const ACharacterBase* CharBase = Cast<ACharacterBase>(GetAvatarActorFromActorInfo());
    if (CharBase)
    {
        return CharBase->IsFacingRight() ? 1 : -1;
    }
    return 1;
}

bool UCGA_Dash::IsInAir() const
{
    const ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
    if (!Character) return false;

    const UCharacterMovementComponent* MoveComp = Character->GetCharacterMovement();
    return MoveComp && MoveComp->IsFalling();
}
```

- [ ] **Step 3: 更新测试文件**

`PlayerCharacterDashTests.cpp` 中把 `GA_DashBase.h` → `CGA_Dash.h`, `UGA_DashBase` → `UCGA_Dash`:

```cpp
#include "CGA_Dash.h"

// ...
const UCGA_Dash* CDO = GetDefault<UCGA_Dash>();
if (!TestNotNull(TEXT("UCGA_Dash CDO exists"), CDO))
// ...
TestTrue(TEXT("UCGA_Dash has Ability.Dash in AssetTags"),
// ...
TestEqual(TEXT("DashSpeed is 2667 (400 units over 0.15s)"), CDO->DashSpeed, 2667.f);
TestEqual(TEXT("DashDuration is 0.15s"), CDO->DashDuration, 0.15f);
```

- [ ] **Step 4: 删除旧文件(先不删,等编译通过后再 git rm)**

PowerShell:
```powershell
Remove-Item "Source/Elemagic/Public/GA_DashBase.h"
Remove-Item "Source/Elemagic/Private/GA_DashBase.cpp"
```

- [ ] **Step 5: 编译检查**

- [ ] **Step 6: 更新 Blueprint 父类引用(Editor 操作)**

打开 UE5 Editor:
1. Content Browser 中打开 `Content/Blueprint/GA_Dash`
2. File → Reparent Blueprint,选择 `CGA_Dash`
3. 同样对 `GA_Attack` 改为 `CGF_Damage`
4. 保存所有

- [ ] **Step 7: Commit**

```bash
git rm Source/Elemagic/Public/GA_DashBase.h Source/Elemagic/Private/GA_DashBase.cpp
git add Source/Elemagic/Public/CGA_Dash.h Source/Elemagic/Private/CGA_Dash.cpp
git add Source/Elemagic/Private/Tests/PlayerCharacterDashTests.cpp
git commit -m "refactor: rename GA_DashBase to CGA_Dash"
```

---

### Task 8: UCharacterAttributeSetBase 添加 IncomingDamage + PostGameplayEffectExecute

**Files:**
- Modify: `Source/Elemagic/Public/CharacterAttributeSetBase.h`
- Modify: `Source/Elemagic/Private/CharacterAttributeSetBase.cpp`

**Interfaces:**
- Consumes:
  - Data_Damage GameplayTag (已有)
- Produces:
  - `IncomingDamage` (FGameplayAttributeData, Replicated)
  - PostGameplayEffectExecute 中消费 IncomingDamage → 扣 Health

- [ ] **Step 1: 在 CharacterAttributeSetBase.h 添加 IncomingDamage**

在 `MoveSpeed` 属性后面添加:

```cpp
// 管道属性:SetByCaller[Data.Damage] 把伤害写入此属性,
// PostGameplayEffectExecute 中消费并扣减 Health 后归零。
UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_IncomingDamage, Category = "Attributes")
FGameplayAttributeData IncomingDamage;
ATTRIBUTE_ACCESSORS(UCharacterAttributeSetBase, IncomingDamage)
```

在 OnRep_MoveSpeed 后面添加:

```cpp
UFUNCTION()
virtual void OnRep_IncomingDamage(const FGameplayAttributeData& OldIncomingDamage);
```

- [ ] **Step 2: 在 CharacterAttributeSetBase.cpp 添加实现**

构造函数中添加:

```cpp
InitIncomingDamage(0.f);
```

`GetLifetimeReplicatedProps` 中添加:

```cpp
DOREPLIFETIME_CONDITION_NOTIFY(UCharacterAttributeSetBase, IncomingDamage, COND_None, REPNOTIFY_Always);
```

`PostGameplayEffectExecute` 中 Health 处理后面添加:

```cpp
if (Data.EvaluatedData.Attribute == GetIncomingDamageAttribute())
{
    const float Damage = GetIncomingDamage();
    SetIncomingDamage(0.f);

    if (Damage > 0.f)
    {
        SetHealth(FMath::Clamp(GetHealth() - Damage, 0.f, GetMaxHealth()));
        // Health <= 0 由 OnHealthChanged 回调监听 → Die()
    }
}
```

添加 OnRep 实现(文件末尾):

```cpp
void UCharacterAttributeSetBase::OnRep_IncomingDamage(const FGameplayAttributeData& OldIncomingDamage)
{
    GAMEPLAYATTRIBUTE_REPNOTIFY(UCharacterAttributeSetBase, IncomingDamage, OldIncomingDamage);
}
```

- [ ] **Step 3: 编译检查**

- [ ] **Step 4: Commit**

```bash
git add Source/Elemagic/Public/CharacterAttributeSetBase.h Source/Elemagic/Private/CharacterAttributeSetBase.cpp
git commit -m "feat: add IncomingDamage attribute with PostGameplayEffectExecute damage pipeline"
```

---

### Task 9: 自动化测试

**Files:**
- Create: `Source/Elemagic/Private/Tests/DamageFormTests.cpp`
- Modify: `Source/Elemagic/Private/Tests/PlayerCharacterDashTests.cpp` (已在 Task 7 中完成)

**Interfaces:**
- Consumes: 全部前置 Task

- [ ] **Step 1: 创建 DamageFormTests.cpp**

```cpp
// Source/Elemagic/Private/Tests/DamageFormTests.cpp
#if WITH_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AttackFrameData.h"
#include "CGF_Damage.h"
#include "HitboxManager.h"
#include "ElemagicGameplayTags.h"
#include "CharacterBase.h"
#include "PlayerCharacter.h"
#include "CharacterAttributeSetBase.h"
#include "AbilitySystemComponent.h"
#include "Components/BoxComponent.h"

// ---- FAttackFrameConfig 结构体测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFrameConfigDefaultsTest,
    "Elemagic.DamageForm.FrameConfigDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFrameConfigDefaultsTest::RunTest(const FString& Parameters)
{
    FAttackFrameConfig Config;
    TestEqual(TEXT("Default NormalizedTime is 0"), Config.NormalizedTime, 0.f);
    TestEqual(TEXT("Default HitboxExtent is (32,32)"), Config.HitboxExtent, FVector2D(32.f, 32.f));
    TestEqual(TEXT("Default HitboxOffset is (0,0)"), Config.HitboxOffset, FVector2D(0.f, 0.f));
    TestEqual(TEXT("Default DamageMultiplier is 1.0"), Config.DamageMultiplier, 1.f);
    TestEqual(TEXT("Default HitImpulse is (0,0)"), Config.HitImpulse, FVector2D::ZeroVector);
    TestTrue(TEXT("Default EventTags is empty"), Config.EventTags.IsEmpty());
    return true;
}

// ---- UAttackFrameData DataAsset 测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFrameDataDefaultTest,
    "Elemagic.DamageForm.FrameDataDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFrameDataDefaultTest::RunTest(const FString& Parameters)
{
    const UAttackFrameData* CDO = GetDefault<UAttackFrameData>();
    if (!TestNotNull(TEXT("UAttackFrameData CDO exists"), CDO))
    {
        return false;
    }

    TestTrue(TEXT("Default Frames array is empty"), CDO->Frames.Num() == 0);
    TestTrue(TEXT("Default SourceAnimation is null"), CDO->SourceAnimation.IsNull());
    TestTrue(TEXT("GetTotalDuration() returns fallback > 0"), CDO->GetTotalDuration() > 0.f);
    return true;
}

// ---- UHitboxManager 组件测试(需 spawn Character) ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHitboxManagerInitTest,
    "Elemagic.DamageForm.HitboxManagerInit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHitboxManagerInitTest::RunTest(const FString& Parameters)
{
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("Created test world"), World))
    {
        return false;
    }

    APlayerCharacter* Character = World->SpawnActor<APlayerCharacter>();
    if (!TestNotNull(TEXT("Spawned PlayerCharacter"), Character))
    {
        return false;
    }

    TestNotNull(TEXT("HitboxManager is created"), Character->HitboxManager);
    TestNotNull(TEXT("AttackHitbox is created"), Character->AttackHitbox);
    TestNotNull(TEXT("Hurtbox is created"), Character->Hurtbox);

    TestEqual(TEXT("AttackHitbox collision type is Hitbox channel"),
        (int32)Character->AttackHitbox->GetCollisionObjectType(),
        (int32)ECC_GameTraceChannel1);

    TestEqual(TEXT("Hurtbox collision type is Hurtbox channel"),
        (int32)Character->Hurtbox->GetCollisionObjectType(),
        (int32)ECC_GameTraceChannel2);

    TestTrue(TEXT("AttackHitbox starts disabled"),
        Character->AttackHitbox->GetCollisionEnabled() == ECollisionEnabled::NoCollision);

    TestTrue(TEXT("Hurtbox starts enabled (QueryOnly)"),
        Character->Hurtbox->GetCollisionEnabled() == ECollisionEnabled::QueryOnly);

    return true;
}

// ---- UCGF_Damage CDO 测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGFDamageDefaultsTest,
    "Elemagic.DamageForm.CGFDamageDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGFDamageDefaultsTest::RunTest(const FString& Parameters)
{
    const UCGF_Damage* CDO = GetDefault<UCGF_Damage>();
    if (!TestNotNull(TEXT("UCGF_Damage CDO exists"), CDO))
    {
        return false;
    }

    TestTrue(TEXT("Has Ability.Attack tag"),
        CDO->GetAssetTags().HasTagExact(ElemagicGameplayTags::Ability_Attack));

    TestTrue(TEXT("ActivationOwnedTags has State.Attacking"),
        CDO->ActivationOwnedTags.HasTagExact(ElemagicGameplayTags::State_Attacking));

    TestTrue(TEXT("ActivationBlockedTags has State.Attacking"),
        CDO->ActivationBlockedTags.HasTagExact(ElemagicGameplayTags::State_Attacking));

    TestTrue(TEXT("ActivationBlockedTags has State.Dead"),
        CDO->ActivationBlockedTags.HasTagExact(ElemagicGameplayTags::State_Dead));

    return true;
}

// ---- IncomingDamage 属性管道测试 ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIncomingDamageAttrTest,
    "Elemagic.DamageForm.IncomingDamageAttr",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FIncomingDamageAttrTest::RunTest(const FString& Parameters)
{
    const UCharacterAttributeSetBase* CDO = GetDefault<UCharacterAttributeSetBase>();
    if (!TestNotNull(TEXT("AttributeSet CDO exists"), CDO))
    {
        return false;
    }

    TestEqual(TEXT("Default IncomingDamage is 0"), CDO->GetIncomingDamage(), 0.f);
    TestEqual(TEXT("Default AttackPower is 10"), CDO->GetAttackPower(), 10.f);

    return true;
}

#endif // WITH_AUTOMATION_TESTS
```

- [ ] **Step 2: 编译检查**

- [ ] **Step 3: 运行所有 DamageForm 测试**

UE5 Editor → Window → Test Automation → 搜索 "Elemagic.DamageForm" → Run

- [ ] **Step 4: Commit**

```bash
git add Source/Elemagic/Private/Tests/DamageFormTests.cpp
git commit -m "test: add DamageForm framework unit tests (FrameConfig, FrameData, HitboxManager, CGF_Damage, IncomingDamage)"
```

---

### Task 10: 端到端验证 — Editor 内测试整个流程

**验证步骤:**

- [ ] **Step 1: 打开 UE5 Editor → 打开 level1 关卡**
- [ ] **Step 2: 确认编译通过,无启动错误**
- [ ] **Step 3: 运行自动化测试套件 (Window → Test Automation)**
  - 搜索 "Elemagic" → 全选 → Run
  - 确认所有测试通过(DamageForm + Dash + Animation + Movement)
- [ ] **Step 4: PIE 验证**
  - Play In Editor
  - 确认角色可正常移动/跳跃/冲刺(GA_Dash 已改名为 CGA_Dash 蓝图层仍然工作)
  - 确认 HitboxManager 在 Character 上正常初始化
- [ ] **Step 5: 提交 Blueprint 更新**

```bash
git add Content/Blueprint/GA_Attack.uasset Content/Blueprint/GA_Dash.uasset
git commit -m "fix: reparent GA blueprints to CGF_Damage and CGA_Dash"
```

---

## 编写 (Blueprint) 资产步骤

实现完成后,创建新的攻击技能在 Editor 中完成:

1. **创建 DamageEffect GE** — `Content/Blueprint/GE_Damage` — 类型为 Cost GameplayEffect,添加 modifier: Attribute=IncomingDamage,ModifierOp=Override,以 SetByCaller 获取 magnitude(SetByCallerDataTag=Data.Damage)
2. **创建 AttackFrameData DataAsset** — `Content/Data/DA_Attack_Light` — 填入帧配置
3. **创建 CGF_Damage BP 子类** — `Content/Blueprint/GA_Light` — 设置 AttackAnimation / FrameData / DamageEffectClass
4. **加 StartupAbilities** — 把 GA_Light 加入 BP_PlayerCharacter 的 StartupAbilities 数组
