# DamageForm 伤害判定框架设计

日期: 2026-08-11
状态: 待批准

## 背景

现有的 `GA_AttackBase` 是单个攻击能力——`PerformAttack()` 为 BlueprintImplementableEvent，命中判定全丢给蓝图层，没有可复用的命中判定基础设施。需要设计一套支持玩家和怪物共用、兼容未来网络同步的伤害判定框架，并建立 C++/BP 命名规范。

## 命名规范

| 前缀 | 含义 | 适用范围 |
|------|------|----------|
| `CGF_` | C++ GameplayForm | 伤害判定框架/架构基类（定义可复用的判定模式） |
| `CGA_` | C++ GameplayAbility | 特定能力的 C++ 基类（单个技能的具体实现） |
| `GA_` / `GE_` | GAS 标准前缀 | BP 子类沿用引擎规范 |

**改名清单**：
- `GA_AttackBase` → `CGF_Damage`（框架）
- `GA_DashBase` → `CGA_Dash`（特定能力）

## 架构总览

```
ACharacterBase (玩家 + 怪物共用)
├── UPaperFlipbookComponent        (仅播放动画，不驱动判定)
├── UHitboxManager                 (攻击判定核心，管理碰撞体 + 查 DataAsset)
│   ├── AttackHitbox (UBoxComponent, Hitbox 通道)
│   └── Hurtbox (UBoxComponent, Hurtbox 通道)
├── UAbilitySystemComponent        (已有)
└── UCharacterAttributeSetBase     (已有)
```

### 三层分层

| 层 | 组件 | 归属 | 职责 |
|----|------|------|------|
| 框架层 | `UHitboxManager` | `ACharacterBase` | 管理 AttackHitbox/Hurtbox，消费 DataAsset，处理 Overlap → 伤害 |
| 技能层 | `CGF_Damage` | GAS Ability | 纯编排器：喂数据给框架，启动/结束攻击。不写命中判定 |
| 数据层 | `UAttackFrameData` | DataAsset | 帧配置数组：标准化时间比 + Hitbox 尺寸/偏移 + EventTags + 伤害倍率 + 击退 |

### 技能实例层

| 技能 | 类型 | 配置方式 |
|------|------|----------|
| `GA_Light` | `CGF_Damage` BP 子类 | 在 Class Defaults 设置 AttackAnimation / FrameData / DamageEffectClass |
| `GA_Heavy` | `CGF_Damage` BP 子类 | 同上，不同数据 |
| `GA_Dash` | `CGA_Dash` BP 子类 | 特定冲刺逻辑（不影响 CGF_Damage 架构） |

## 数据资产

### UAttackFrameData

| 字段 | 类型 | 说明 |
|------|------|------|
| SourceAnimation | TSoftObjectPtr\<UPaperFlipbook\> | 配对的 Flipbook，仅用于视觉播放 + 获取总时长 + 编辑器对照 |
| Frames | TArray\<FAttackFrameConfig\> | 按 NormalizedTime 升序排列 |

### FAttackFrameConfig

| 字段 | 类型 | 说明 |
|------|------|------|
| NormalizedTime | float (0.0~1.0) | 触发时间点（标准化比例，与动画实际时长无关） |
| HitboxExtent | FVector2D | 碰撞体半尺寸（X/Y = Box 宽/高） |
| HitboxOffset | FVector2D | 碰撞体相对角色中心偏移 |
| EventTags | FGameplayTagContainer | 帧事件 Tag |
| DamageMultiplier | float (默认 1.0) | 当前帧命中时的伤害倍率 |
| HitImpulse | FVector2D | 命中击退力（方向 + 强度） |

### EventTags 预定义

| Tag | 语义 |
|-----|------|
| `Event.Attack.EnableHitbox` | 按当前帧参数激活 AttackHitbox |
| `Event.Attack.DisableHitbox` | 关闭 AttackHitbox |
| `Event.Attack.ResetHitTargets` | 清空去重集合（允许多段命中） |
| `Event.Attack.EndAttack` | 提前结束攻击（可选） |

## 碰撞系统

### 自定义碰撞通道

`Project Settings → Engine → Collision` 新增两个 ObjectChannel：

| 通道名 | 默认响应 |
|--------|---------|
| `Hitbox` | Ignore 所有，仅对 `Hurtbox` 设为 Overlap |
| `Hurtbox` | Ignore 所有，仅对 `Hitbox` 设为 Overlap |

### 碰撞体归属

- `AttackHitbox` 和 `Hurtbox` 都是角色自身的子 `UBoxComponent`，构造中在 `ACharacterBase` 上创建
- 每个角色实例的 HitboxManager 只管理自己的碰撞体，互不干扰
- Overlap 回调中 `OtherActor == GetOwner()` 直接跳过，防止自己打自己

## CGF_Damage 设计

`CGF_Damage` 继承 `UElemagicGameplayAbility`，是伤害判定的编排器基类。

### 配置属性

```cpp
UPROPERTY TObjectPtr<UPaperFlipbook> AttackAnimation;
UPROPERTY TObjectPtr<UAttackFrameData> FrameData;
UPROPERTY TSubclassOf<UGameplayEffect> DamageEffectClass;
UPROPERTY FVector2D BaseImpulse;
UPROPERTY FGameplayTag AbilityAttackTag;  // 如 Ability.Attack.Light
```

### 生命周期

```
ActivateAbility:
  1. 校验 FrameData.SourceAnimation == AttackAnimation（不匹配 → Warning）
  2. CommitAbility (Cost / Cooldown / Blocked Tags)
  3. HitboxManager->BeginAttack(FrameData, DamageEffectClass, BaseImpulse)
  4. 播放 AttackAnimation（纯视觉）
  5. 添加 State_Attacking Tag

EndAbility:
  1. HitboxManager->EndAttack()（关闭 Hitbox + 清空去重集合）
  2. 移除 State_Attacking Tag
```

不再有 `PerformAttack()` BlueprintImplementableEvent。

## 运行时执行流程（计时器驱动，兼容网络同步）

### 设计原则

伤害判定不依赖动画帧进度（各客户端渲染不同步），改为计时器驱动——以服务器时间作为唯一真相源。

```
GA 激活: StartTime = ServerWorld->GetTimeSeconds()

每 Tick (HitboxManager):
  Elapsed = Now() - StartTime
  NormalizedTime = Clamp(Elapsed / TotalDuration, 0.0, 1.0)
  查 DataAsset.Frames：当前 NormalizedTime 落在哪些区间？
  执行匹配的 EventTags
  NormalizedTime >= 1.0 → 自动调 GA->EndAbility()
```

`TotalDuration` 从 Flipbook 的 `GetTotalDuration()` 获取。若 Flipbook 未配置，fallback 用 DataAsset.Frames 最后一帧的 NormalizedTime 反算（如最后一帧 NormalizedTime=0.9 则 ActualDuration = 推算值，或固定默认 0.5s）。

### 帧配置查询策略

每个 Tick 从上次查询位置继续，增量处理已过时间区间内的所有配置节点（不每帧从头扫描，避免漏掉快速穿过的节点）。

### 单次判定流程

```
OnOverlap (AttackHitbox 碰到其他角色的 Hurtbox):
  OtherActor == Owner? → 跳过
  HitTargets.Contains(OtherActor)? → 跳过（本段内已命中）
  读取当前帧 DamageMultiplier
  构造 GE: SetByCaller[IncomingDamage] = AttackPower × DamageMultiplier
  ApplyGameplayEffectToTarget(OtherActor->ASC)
  施加击退力：最终击退 = BaseImpulse + 当前帧 HitImpulse（CGF_Damage 的 BaseImpulse 为底板，每帧 HitImpulse 为叠加值）
  HitTargets.Add(OtherActor)
```

### 多段命中支持

- 默认每段内同一目标只命中一次（HitTargets 去重）
- 当帧配置包含 `Event.Attack.ResetHitTargets` → 清空 HitTargets，后续可重新命中同一目标
- 不需要多段命中的技能不在 DataAsset 里加这个 Tag 即可

### 网络同步

| 组件 | 同步方式 | 说明 |
|------|----------|------|
| Ability 激活 | ASC 自动复制 | GAS 原生 |
| AttributeSet | ReplicatedUsing 已有 | GAS 原生 |
| GE Apply | ASC 自动复制 | GAS 原生 |
| HitboxManager 计时器 | 服务端 Tick 自驱动 | 不依赖客户端同步 |
| Flipbook 播放 | 各客户端自主播放 | 纯视觉效果，不参与判定 |

## HitboxManager 设计

### 归属

`ACharacterBase` 构造函数中创建，所有角色（玩家 + 怪物）天生拥有。

### 核心接口

```cpp
void BeginAttack(UAttackFrameData* Data, TSubclassOf<UGameplayEffect> DamageGE, FVector2D Impulse);
void EndAttack();
void TickComponent(float DeltaTime); // 内部实现计时器 + 查表 + 控制 Hitbox

UFUNCTION()
void OnAttackHitboxOverlap(AActor* OtherActor); // 绑定到 AttackHitbox 的 OnComponentBeginOverlap
```

### 状态变量

```cpp
bool bActive;
float StartTime;       // 服务端时间
float TotalDuration;   // 从 Flipbook 获取
TSet<AActor*> HitTargets;  // 去重集合
int32 LastProcessedConfigIdx; // 增量查询游标
```

### 碰撞体控制

- `EnableHitbox`：`AttackHitbox->SetCollisionEnabled(QueryOnly)`，设置 BoxExtent 和相对偏移为当前帧配置值
- `DisableHitbox`：`AttackHitbox->SetCollisionEnabled(NoCollision)`

## CharacterBase 集成

### 新增组件

`ACharacterBase` 构造函数中：
```cpp
AttackHitbox = CreateDefaultSubobject<UBoxComponent>(TEXT("AttackHitbox"));
AttackHitbox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
AttackHitbox->SetCollisionObjectType(ECC_GameTraceChannel1); // Hitbox

Hurtbox = CreateDefaultSubobject<UBoxComponent>(TEXT("Hurtbox"));
Hurtbox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
Hurtbox->SetCollisionObjectType(ECC_GameTraceChannel2); // Hurtbox

HitboxManager = CreateDefaultSubobject<UHitboxManager>(TEXT("HitboxManager"));
HitboxManager->Init(AttackHitbox, Hurtbox);
```

### 碰撞通道设置

在 `DefaultEngine.ini` 或 Project Settings 中配置后，`AttackHitbox` 和 `Hurtbox` 的碰撞响应规则为：
- AttackHitbox (Hitbox 通道) → 只 Overlap Hurtbox 通道的对象
- Hurtbox (Hurtbox 通道) → 只 Overlap Hitbox 通道的对象

## 属性系统扩展

`UCharacterAttributeSetBase` 新增：

```cpp
UPROPERTY(BlueprintReadOnly, Category = "Attributes")
FGameplayAttributeData IncomingDamage;
ATTRIBUTE_ACCESSORS(UCharacterAttributeSetBase, IncomingDamage)
```

`PostGameplayEffectExecute` 中：
```cpp
if (Data.EvaluatedData.Attribute == GetIncomingDamageAttribute())
{
    const float Damage = GetIncomingDamage();
    SetIncomingDamage(0.f);
    if (Damage > 0.f)
    {
        SetHealth(FMath::Clamp(GetHealth() - Damage, 0.f, GetMaxHealth()));
        // Health 减到 0 由 OnHealthChanged 监听 → Die()
    }
}
```

## 怪物支持

- `AEnemyCharacter` 继承 `ACharacterBase`，自动获得 HitboxManager + AttackHitbox + Hurtbox
- 授予攻击技能的方式和玩家一致（StartupAbilities 或 AI 动态授予）
- 攻击由 AI Controller（行为树/状态机）触发 `TryActivateAbilityByTag(Ability_Attack)`
- 每个角色的 HitboxManager 实例独立运作

## 新增攻击技能的步骤

全部在 Editor 中完成，无需写 C++ 或 BP 逻辑：

1. 导入攻击动画 Flipbook
2. 创建 `UAttackFrameData` DataAsset，填入帧配置和时间点
3. 创建 `CGF_Damage` 的 BP 子类（如 `GA_Heavy`）
4. 在 Class Defaults 中设置 AttackAnimation / FrameData / DamageEffectClass / Cooldown / Cost
5. 把新 GA 加入角色 StartupAbilities 数组

## 实现内容清单

| # | 内容 | 类型 |
|---|------|------|
| 1 | `FAttackFrameConfig` 结构体 | C++ |
| 2 | `UAttackFrameData` DataAsset | C++ |
| 3 | `Event.Attack.*` GameplayTags | C++ |
| 4 | Hitbox / Hurtbox 碰撞通道 | Project Settings 配置 |
| 5 | `UHitboxManager` 组件 | C++ |
| 6 | `ACharacterBase` 集成新组件 | C++ |
| 7 | `GA_AttackBase` → `CGF_Damage` 重构 | C++ |
| 8 | `GA_DashBase` → `CGA_Dash` 重命名 | C++ |
| 9 | `UCharacterAttributeSetBase` 加 IncomingDamage + PostGameplayEffectExecute | C++ |
| 10 | 自动化测试 | C++ |

## 不在此范围

- 怪物 AI 行为树（后续设计）
- UI/HUD 伤害数字显示（后续设计）
- 元素系统 / 元素 combo（后续设计）
- 无敌帧的伤害免疫消费（需在 PostGameplayEffectExecute 中检查 State_Invulnerable tag，后续实现）

## 参考

- [[feedback_testing_postinit]] — PostInit 参数测试需 spawn 实例
- [[feedback_ue5_beginner_guidance]] — UE5 新手操作指引
- GAS 网络同步：ASC 自动处理 Ability 激活和 GE 应用
