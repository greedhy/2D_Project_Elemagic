# 二段跳实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 `APlayerCharacter` 上暴露 `JumpMaxCount` 蓝图属性（默认 2），构造中写入 `UCharacterMovementComponent::JumpMaxCount`，实现二段跳。

**Architecture:** 使用 UE 引擎原生 `UCharacterMovementComponent::JumpMaxCount` 机制——`ACharacter::Jump()` / `CanJump()` / 落地重置计数全自动，无需手写跳跃状态追踪。属性以蓝图值为准，后续可改为 GA 运行时写入。

**Tech Stack:** C++ (UE5 CharacterMovementComponent), Blueprint (Class Defaults 覆盖)

## Global Constraints

- 代码约定：C++ 机制 + Blueprint/DataAsset 内容，同 test_25d 模式
- `JumpMaxCount` 以蓝图子类 Class Defaults 设置的值为准
- 保留后续封装为 GA 的可能（C++ 骨架不做假设，仅提供可被运行时修改的属性）
- 自动化测试覆盖属性默认值和构造写入行为
- PIE 手动验证二段跳功能正确性

---

### Task 1: 添加 JumpMaxCount 属性并写入 MovementComponent

**Files:**
- Modify: `Source/Elemagic/Public/PlayerCharacter.h:38-46`
- Modify: `Source/Elemagic/Private/PlayerCharacter.cpp:29-30`

**Interfaces:**
- Produces: `APlayerCharacter::JumpMaxCount` — `int32`，`EditDefaultsOnly | BlueprintReadWrite`，默认 `2`
- Consumes: `UCharacterMovementComponent::JumpMaxCount` — 引擎自带属性，构造中写入

- [ ] **Step 1: 在头文件中添加 JumpMaxCount 属性**

在 `PlayerCharacter.h` 的 `JumpHoldTime` 属性之后新增：

```cpp
	// 最大跳跃次数:默认 2 开局即可二段跳,运行时可被 GA 动态修改(如先解锁再赋更高值)。
	// 以蓝图子类 Class Defaults 设置的值为准。
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Elemagic|Movement")
	int32 JumpMaxCount = 2;
```

- [ ] **Step 2: 在构造函数中将 JumpMaxCount 写入 MovementComponent**

在 `PlayerCharacter.cpp` 构造函数中，于 `JumpMaxHoldTime = JumpHoldTime;` 之后添加：

```cpp
			MoveComp->JumpMaxCount = JumpMaxCount;
```

完整上下文（第 29-31 行区域）：

```cpp
			MoveComp->MaxWalkSpeed = MoveSpeed;
			MoveComp->JumpZVelocity = JumpVelocity;
			JumpMaxHoldTime = JumpHoldTime;
			MoveComp->JumpMaxCount = JumpMaxCount;
```

- [ ] **Step 3: 编译验证**

```powershell
& "C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" ElemagicEditor Win64 Development -Project="C:\Users\greedy\Documents\Unreal Projects\Elemagic\Elemagic.uproject" -WaitMutex -FromMsBuild
```

预期: 编译成功，无错误。

- [ ] **Step 4: 提交**

```bash
git add Source/Elemagic/Public/PlayerCharacter.h Source/Elemagic/Private/PlayerCharacter.cpp
git commit -m "feat: add JumpMaxCount property for double jump"
```

---

### Task 2: 更新自动化测试

**Files:**
- Modify: `Source/Elemagic/Private/Tests/PlayerCharacterMovementTests.cpp:26-42`

**Interfaces:**
- Consumes: `APlayerCharacter::JumpMaxCount`（Task 1）, `UCharacterMovementComponent::JumpMaxCount`（Task 1 写入）

- [ ] **Step 1: 在 BP 可调参数断言区域添加 JumpMaxCount 默认值断言**

在 `TestEqual(TEXT("JumpHoldTime default"), CDO->JumpHoldTime, 0.3f);` 之后添加：

```cpp
	TestEqual(TEXT("JumpMaxCount default is 2 for double jump"), CDO->JumpMaxCount, 2);
```

- [ ] **Step 2: 在构造写入断言区域添加 JumpMaxCount 验证**

在 `TestEqual(TEXT("JumpZVelocity driven by JumpVelocity"), MoveComp->JumpZVelocity, CDO->JumpVelocity);` 之后添加：

```cpp
	TestEqual(TEXT("JumpMaxCount driven by JumpMaxCount property"), MoveComp->JumpMaxCount, CDO->JumpMaxCount);
```

- [ ] **Step 3: 运行自动化测试验证**

在 UE Editor 中：Session Frontend → Automation → 搜索 `Elemagic.PlayerCharacter.MovementTuning` → Run Test

预期: 全部 PASS。

- [ ] **Step 4: 提交**

```bash
git add Source/Elemagic/Private/Tests/PlayerCharacterMovementTests.cpp
git commit -m "test: add JumpMaxCount assertions for double jump"
```

---

### Task 3: PIE 手动验证

**无需代码改动。**

- [ ] **Step 1: 地面起跳**

打开 `level1`，PIE 运行，按下跳跃键。确认角色从地面正常起跳。

- [ ] **Step 2: 空中二段跳**

角色在空中时再次按下跳跃键。确认角色执行第二段跳（垂直速度重新被推至 `JumpZVelocity`），且 `JumpCurrentCount` 可以在 Debug HUD 或断点中确认已达到 2。

- [ ] **Step 3: 第三次跳跃被阻止**

空中再次按跳跃键。确认不发生第三段跳。

- [ ] **Step 4: 落地重置**

角色落回地面后再次起跳 → 空中二段跳。确认落地后 `JumpCurrentCount` 被正确重置为 0，可完整重新执行二段跳。

- [ ] **Step 5: 反复多次**

连续反复执行 5 次二段跳。确认功能稳定，无卡死、无异常位移。

- [ ] **Step 6: 可选 — 验证 BP 覆盖**

在 `BP_PlayerCharacter` 的 Class Defaults 中将 `JumpMaxCount` 改为 1，PIE 确认只能单跳。改回 2，确认恢复二段跳。以此验证"以蓝图设置为准"的行为。

- [ ] **Step 7: 提交关卡改动（如有）**

```bash
git add Content/Level/level1.umap
git commit -m "chore: level1 PIE double jump verification"
```
