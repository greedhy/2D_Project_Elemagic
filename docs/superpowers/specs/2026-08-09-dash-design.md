# 冲刺设计

日期: 2026-08-09
状态: 已批准

## 背景

角色移动控制已完成跑跳、单向平台、二段跳。下一步需要冲刺（Dash）作为核心位移能力，提升平台跳跃的操作深度。

## 设计

### 行为

| 项目 | 内容 |
|------|------|
| 方向 | 始终冲向角色面朝方向，忽略输入方向（Hollow Knight 式） |
| 参数 | 持续时间 0.15s，距离约 400 单位，纯水平位移，保留垂直速度 |
| 无敌帧 | 冲刺期间有，通过 `State_Invulnerable` GameplayTag 实现 |
| 输入 | C 键 |
| 地面 CD | 有冷却时长（蓝图 Cooldown GE 配），进入空中时 CD 清零 |
| 空中次数 | 最多 1 次 / 滞空，落地重置（`OnLanded` 移除 `State_DashedInAir`） |

**水平速度**：`FacingSign * 2667` (400 单位 / 0.15s ≈ 2667 u/s)，每 Tick 写入 `CharacterMovementComponent::Velocity.X`。

**冲刺结束后**：不再干涉 Velocity，后续移动完全由当前输入决定（与项目"每帧移动只与当前输入有关"原则一致）。

### 设计意图

地面 CD 在进入浮空时清零 → 玩家可以循环：地面冲刺 → 起跳 → 空中冲刺 → 落地 → 地面冲刺。同一跳跃周期内不会发生两次地面冲刺（因为起跳后才空中，落地前不会再地面），但地→空→地的连续节奏是刻意支持的。

## 架构

### GA_Dash（GAS GameplayAbility）

**Tag 配置**：
- `AbilityTags`: `Ability.Dash`
- `ActivationOwnedTags`: `State_Dashing`
- `ActivationBlockedTags`: `State_Dashing` | `State_Dead`
- `ActivationRequiredTags`: 无（但 CanActivate 中判断空中次数限制）

**生命周期**：
1. 输入触发 → `ActivateAbility`
2. `CommitAbility` → 施加 Cooldown GE（地面 CD）
3. 计算冲刺方向：`FMath::Sign(GetActorForwardVector().Dot(FVector::ForwardVector))` 取面朝
4. `GetWorldTimerManager().SetTimer` 设 0.15s 结束回调
5. 每 Tick（或 Timer Tick）写 `MoveComp->Velocity.X = Direction * 2667`
6. EndAbility → 移除 `State_Dashing` tag，动画自动退回

**空中次数限制**：
- `CanActivateAbility` 检查 `State_DashedInAir` tag
- 每次空中冲刺成功激活时，给自己加 `State_DashedInAir`

**地面 CD 空中清零**：
- `APlayerCharacter` 覆写 `OnMovementModeChanged`
- 检测新模式为 `MOVE_Falling` → `AbilitySystemComponent->CancelAbilityCooldown(GA_DashSpecHandle)`（如果当前有冷却）

### 无敌帧

`GA_Dash::ActivateAbility` 中给 ASC 加 `State_Invulnerable` loose tag，`EndAbility` 中移除。伤害系统（后续实现）检查此 tag 跳过扣血。

### 输入

`AMyPlayerController` 新增 `DashAction` (UInputAction*)，EditDefaultsOnly。
`SetupInputComponent` 中通过 `EleInputComponent::BindAbilityActions` 绑定到 `Ability.Dash` tag——**不需要新增 C++ 绑定代码**，在 `InputConfig` 资源里加映射即可。

### 动画

`CharacterBase` 新增：
```cpp
UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Elemagic|Animation")
TObjectPtr<UPaperFlipbook> DashFlipbook;
```

`UpdateAnimation` 优先级调整为：**冲刺 > 下落 > 跑 > 待机**。
通过 `ASC->HasMatchingGameplayTag(ElemagicGameplayTags::State_Dashing)` 检测冲刺状态，
GA_Dash 激活时已施加此 tag，动画可直接响应。

### 蓝图配置

| 资源 | 操作 |
|------|------|
| `EleInputConfig` | 新增 `(IA_Dash, Ability.Dash)` 映射 |
| `IMC_Player` | 新建 `IA_Dash`（C 键） |
| `BP_PlayerCharacter` | 设置 `DashFlipbook`（暂用 Idle 素材占位） |
| `GE_DashCooldown` | 蓝图创建，Duration 配地面冷却时长 |

## 自动化测试

`PlayerCharacterMovementTests.cpp`（或新建 `PlayerCharacterDashTests.cpp`）：
- `GA_Dash` CDO 的 AbilityTags 包含 `Ability.Dash`
- `GA_Dash` CDO 的 ActivationBlockedTags 包含 `State_Dashing`
- 面朝右时冲刺方向为 +X，面朝左时为 -X（纯函数单独测）
- Sprint 持续时间 0.15s（检查内部 SprintTimer 默认值）
- 冲刺速度值 2667（检查内部 DashSpeed 默认值）

注意：CDO 上 `PostInit` 写入的 MoveComp 值会被 BP 序列化覆盖，测试 CMC 透传结果时需要 spawn 实例而非用 CDO（见 [[feedback_testing_postinit]]）。

## PIE 手动验证

1. 地面 C 键 → 面朝方向冲刺 400 单位，0.15s 后结束
2. 地面冲刺后 CD 内再按 C → 不触发
3. 地面冲刺 → 起跳 → CD 清零 → 空中 C 键 → 空中冲刺
4. 空中冲刺后再按 C → 不触发（次数限制）
5. 落地后 C 键 → 地面冲刺可用，空中次数重置
6. 冲刺期间无敌帧确认（后续有伤害系统后验证）
7. 冲刺动画占位符切换
8. 反复多次无异常

## 未决问题

- DashFlipbook 实际素材：待美术资源到位后替换占位符
- 伤害系统接入后验证无敌帧
- Cooldown GE 具体冷却时长：蓝图配置，建议初始 0.8s 地面 CD
