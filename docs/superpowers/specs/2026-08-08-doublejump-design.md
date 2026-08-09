# 二段跳设计

日期: 2026-08-08
状态: 已批准

## 背景

角色移动控制已完成基础跑跳，零惯性即时响应的调教也已到位。当前跳跃数默认为 1（引擎 `JumpMaxCount` 默认值），需要扩展为二段跳。

## 设计

**方式**：使用 UE5 `UCharacterMovementComponent::JumpMaxCount` 原生机制，设值为 2。

**理由**：
- 引擎层已内置 `JumpMaxCount`，`ACharacter::Jump()` / `CanJump()` / 落地重置计数（`JumpCurrentCount` → 0）全自动，不需要任何手写状态追踪。
- 一行 C++ 即可完成，不涉及输入绑定改动、不涉及 GAS。

### 新增属性

在 `APlayerCharacter`（`Elemagic|Movement` 分类）新增：

```cpp
UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Elemagic|Movement")
int32 JumpMaxCount = 2;
```

构造中写入 `MoveComp->JumpMaxCount = JumpMaxCount`，**以蓝图子类 Class Defaults 中设置的值为准**。初次落地时 `JumpMaxCount` 控制初始最大跳跃次数；后续可通过 GAS 技能在运行时动态修改（如解锁二段跳技能时把 `JumpMaxCount` 从 1 提升到 2），实现"先获得能力才解锁"的逻辑。

### 当前行为与后续封装路径

**本轮**：`BP_PlayerCharacter` 中 `JumpMaxCount` 默认值设为 2，玩家开局即拥有二段跳，方便在后续功能开发中随时测试平台跳跃手感。

**后续封装为 GA 时**：
1. `BP_PlayerCharacter` 的 `JumpMaxCount` 默认值改为 1（开局单跳）
2. 新建 `GA_DoubleJump`，授予/激活时将 `JumpMaxCount` 动态设为 2
3. 该 GA 可被放入技能栏、通过元素合成获得、或随局内升级解锁——具体获取方式由后续设计决定
4. C++ 骨架不需要任何改动，仅 BP 默认值 + 新增 GA 资源即可完成切换

### 自动化测试

`PlayerCharacterMovementTests.cpp` 新增：
- `CDO->JumpMaxCount == 2` — 默认值合理
- `MoveComp->JumpMaxCount == CDO->JumpMaxCount` — 构造正确写入

### PIE 手动验证

1. 地面起跳 → 可正常跳跃
2. 空中再次按跳跃键 → 可执行第二段跳
3. 空中再按第三次 → 不再起跳（`JumpCurrentCount` 已达上限）
4. 落地后 → 计数重置，可重新二段跳
5. 反复多次二段跳无异常

## 未决问题

- 何时将二段跳从"默认开放"改为"能力解锁"：等待技能系统（元素合成→技能栏→能力授予）链路打通后，把 `BP_PlayerCharacter` 的 `JumpMaxCount` 默认值改为 1，并创建 `GA_DoubleJump` 负责运行时写入 `JumpMaxCount = 2`。此改动不影响 C++ 骨架，仅涉及 BP 默认值变更和新增 GA 资源。
