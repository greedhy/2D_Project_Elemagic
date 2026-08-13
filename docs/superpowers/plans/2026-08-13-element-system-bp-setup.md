# 元素系统 BP 收尾（元素球 → 合成 → 释放）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 跑通元素系统完整流程——玩家拾取元素球 → 3 元素合成技能 → 技能入 4 槽技能栏 → 按键释放，用最小验证（1 个组合）打通全链路。

**Architecture:** 全部 C++ 机制已就绪（commit fcbc7b5），本计划只做**编辑器 BP/数据资产内容**，不改任何 C++。数据流：`AElemagicElementOrb`（元素球，配置 `ElementTag`）→ `UElementSystemComponent::AddElement`（3 槽装载）→ `Synthesize`（查 `UElementComboInfo` 组合表）→ `GiveAbility` 入技能栏 → 输入链路 `IA → DA_EleInputConfig → MyPlayerController → HandleInputTag` 触发 `ActivateSkillSlot`。

**Tech Stack:** UE 5.8 Editor（Windows）、Enhanced Input、GameplayTags、GAS。

## Global Constraints

- **不写任何 C++ 代码**，只创建/编辑 Blueprint 与 DataAsset。
- 7 元素 Tag 名称固定为：`Element.Fire` `Element.Water` `Element.Earth` `Element.Wind` `Element.Lightning` `Element.Light` `Element.Dark`（定义于 `ElemagicGameplayTags.cpp`，不可另造）。
- 5 个输入 Tag 固定为：`Input.Skill1` `Input.Skill2` `Input.Skill3` `Input.Skill4` `Input.Synthesize`。
- **合成允许重复元素**：`FindCombo` 已是多重集语义（顺序无关 + 计数一致，`Elements` 为 `TArray<FGameplayTag>`），组合表可填 `Fire+Fire+Fire` 这样的行，玩家装 3 个相同元素也能合成对应技能（2026-08-13 已改并测试通过）。
- 可用技能类（现有 GA BP，位于 `Content/Blueprint/Atack/`）：`GA_Slash` `GA_Slash2` `GA_Heavy` `GA_HeavyFinish` `GA_Fireball` `GA_Dash`。
- 每步改完点 **Save All**（`Ctrl+Shift+S`），避免编辑器未保存丢失。

---

### Task 1: 创建 7 个元素球 BP

**Files:**
- Create: `Content/Blueprint/Actor/BP_Orb_Fire.uasset`（及 Water/Earth/Wind/Lightning/Light/Dark 共 7 个）
- 父类：`AElemagicElementOrb`（C++ 已实现，含 `ElementTag` + `PickupCollision` 碰撞盒 + `Sprite` flipbook）

**Interfaces:**
- Consumes: `AElemagicElementOrb::ElementTag`（`EditAnywhere`，BP 里配置）
- Produces: 7 个可放关卡的可拾取元素球

- [ ] **Step 1: 在 Content Browser 打开 `Content/Blueprint/Actor` 目录**（左侧内容面板逐级展开 `Content → Blueprint → Actor`）

- [ ] **Step 2: 右键空白处 → Blueprint Class**
    - 弹出「Pick Parent Class」窗口，选 **All Classes** 折叠项，展开后在搜索框输入 `ElemagicElementOrb`
    - 选中 `ElemagicElementOrb` → 点绿色 **Select** 按钮

- [ ] **Step 3: 命名为 `BP_Orb_Fire`**
    - 创建后资源名处于可编辑状态，直接输入 `BP_Orb_Fire` 回车

- [ ] **Step 4: 配置 ElementTag**
    - 双击打开 `BP_Orb_Fire`
    - 右侧 **Details 面板**（若没显示，菜单 Window → Details）找到 **Element** 分类下的 **Element Tag**
    - 点下拉 → 搜索 `Element.Fire` → 选中
    - （Pickup Collision 已由 C++ 配好 WorldDynamic+只与 Pawn 重叠，**无需改动**）

- [ ] **Step 5: 配置 Sprite 外观（临时）**
    - 在左侧 **Components 面板** 选中 `Sprite`（UPaperFlipbookComponent）
    - Details 面板找到 **Sprite** 分类 → **Source Flipbook** 下拉，暂选现有 `Coin` 或任意 flipbook（如 `Content/Asset/Coin`）
    - 说明：外观只是占位，后续换成各元素专属贴图

- [ ] **Step 6: 编译 + 保存**
    - 点工具栏 **Compile**，再 **Save**，关掉窗口

- [ ] **Step 7: 重复 Step 2–6 创建其余 6 个**，命名与 ElementTag 对应：

    | BP 名称 | ElementTag |
    |---|---|
    | `BP_Orb_Water` | `Element.Water` |
    | `BP_Orb_Earth` | `Element.Earth` |
    | `BP_Orb_Wind` | `Element.Wind` |
    | `BP_Orb_Lightning` | `Element.Lightning` |
    | `BP_Orb_Light` | `Element.Light` |
    | `BP_Orb_Dark` | `Element.Dark` |

- [ ] **Step 8: 验证**
    - 在 Content Browser 里看到 `Actor` 目录下 7 个 `BP_Orb_*`
    - 逐个打开确认 Element Tag 与名字对应

- [ ] **Step 9: 提交（可选）**
    ```bash
    git add Content/Blueprint/Actor/BP_Orb_*.uasset
    git commit -m "feat: add 7 element orb BPs"
    ```

---

### Task 2: 填 DA_ElementComboInfo（最小验证：1 个组合）

**Files:**
- Modify: `Content/Blueprint/DA_ElementComboInfo.uasset`（已存在，当前 Combos 为空）

**Interfaces:**
- Consumes: `UElementComboInfo::Combos`（`TArray<FElementComboEntry>`），每行字段：`Elements`（3 个 Tag）、`ResultAbility`（技能类）、`DisplayName`、`Icon`、`CooldownTag`
- Produces: 一个「火+水+土 → GA_Fireball」的组合映射

- [ ] **Step 1: 双击打开 `DA_ElementComboInfo`**（`Content/Blueprint/DA_ElementComboInfo`）

- [ ] **Step 2: 在 Combos 数组加一行**
    - Details 面板找到 **Combo** 分类 → **Combos** 数组 → 点 **+** 号展开元素 `[0]`

- [ ] **Step 3: 填 Elements（3 个元素，可相同可不同）**
    - `Elements` → 点 **+** 三次，得到 3 个 Tag 槽
    - 三个槽依次选：`Element.Fire`、`Element.Water`、`Element.Earth`（本验证用 3 个不同；若想验证重复，可填 `Fire`/`Fire`/`Fire`）

- [ ] **Step 4: 填 ResultAbility**
    - `Result Ability` 下拉 → 选 `GA_Fireball`（远程火球）

- [ ] **Step 5: 填 DisplayName**
    - `Display Name` → 输入 `Fireball`（技能栏会显示这个名字）

- [ ] **Step 6: Icon / CooldownTag 暂空**
    - `Icon` 留空（本次验证不关心图标）
    - `Cooldown Tag` 留空（本组合不设冷却）

- [ ] **Step 7: Save**（数据资产直接 Ctrl+S 或工具栏 Save）

- [ ] **Step 8: 验证**
    - 关掉再双击打开，确认 Combos[0] 的 Elements 是 Fire/Water/Earth、Result Ability 是 GA_Fireball

- [ ] **Step 9: 提交（可选）**
    ```bash
    git add Content/Blueprint/DA_ElementComboInfo.uasset
    git commit -m "feat: add Fire+Water+Earth -> GA_Fireball combo row"
    ```

---

### Task 3: 输入映射（5 个 Input Action + EleInputConfig + IMC + Controller 引用）

**Files:**
- Create: `Content/Blueprint/Input/IA_Skill1.uasset`（及 Skill2/3/4/Synthesize 共 5 个）
- Modify: `Content/Blueprint/Input/DA_EleInputConfig.uasset`、`Content/Blueprint/Input/IMC_Default.uasset`、`Content/Blueprint/BP_MyPlayerController.uasset`

**Interfaces:**
- Consumes: `UEleInputConfig::AbilityInputActions`（`TArray<FEleInputAction>`，每条 = `InputAction` + `InputTag`）
- Produces: 按键 → IA → InputTag → `AMyPlayerController::AbilityInputTagPressed` → `UElementSystemComponent::HandleInputTag`

- [ ] **Step 1: 创建 5 个 Input Action**
    - `Content/Blueprint/Input` 目录右键 → **Input → Input Action**
    - 依次命名并保存：`IA_Skill1` `IA_Skill2` `IA_Skill3` `IA_Skill4` `IA_Synthesize`
    - 每个打开后 **Value Type** 保持默认 `bool`（或 Digital）即可，无需改

- [ ] **Step 2: 在 DA_EleInputConfig 加 5 条映射**
    - 双击打开 `DA_EleInputConfig`
    - Details 面板 → **Ability Input Actions** 数组 → 若已有战斗映射（IA_Attack_Light → Ability.Attack.Light 等）**保留不动**，在末尾继续 **+** 添加 5 条
    - 每条：`Input Action` 选对应 IA，`Input Tag` 选对应 Tag：

    | Input Action | Input Tag |
    |---|---|
    | `IA_Skill1` | `Input.Skill1` |
    | `IA_Skill2` | `Input.Skill2` |
    | `IA_Skill3` | `Input.Skill3` |
    | `IA_Skill4` | `Input.Skill4` |
    | `IA_Synthesize` | `Input.Synthesize` |

    - Save

- [ ] **Step 3: 在 IMC_Default 加 5 个按键映射**
    - 双击打开 `IMC_Default`
    - 找到 **Mappings** 数组（每行 = 一个按键映射），在末尾 **+** 添加 5 条
    - 每条选 Input Action 后，点 **Key** 旁边的下拉，搜键盘键名并选中：

    | Input Action | 建议按键 |
    |---|---|
    | `IA_Skill1` | `1` |
    | `IA_Skill2` | `2` |
    | `IA_Skill3` | `3` |
    | `IA_Skill4` | `4` |
    | `IA_Synthesize` | `E` |

    - Save
    - （若技能释放想和现有 `Ability.Attack.*` 手感一致，可自行换键，但本计划用数字键 1-4 + E）

- [ ] **Step 4: 确认 BP_MyPlayerController 的 InputConfig 引用**
    - 双击打开 `BP_MyPlayerController`
    - 默认打开 **Class Defaults**；若在别处，点工具栏 **Class Defaults** 按钮
    - Details 面板找到 **Elemagic | Input** 分类 → **Input Config** → 设为 `DA_EleInputConfig`
    - 同分类确认 **Player Mapping Context** 已设为 `IMC_Default`（移动/跳跃能玩说明已设，只核对不覆盖）

- [ ] **Step 5: Compile + Save All**（`Ctrl+Shift+S`）

- [ ] **Step 6: 验证**
    - 打开 `IMC_Default`，确认有 IA_Skill1-4 和 IA_Synthesize 五条且各有按键
    - 打开 `DA_EleInputConfig`，确认有 5 条 (IA → Input.Skill1-4/Synthesize)

- [ ] **Step 7: 提交（可选）**
    ```bash
    git add Content/Blueprint/Input/ Content/Blueprint/BP_MyPlayerController.uasset
    git commit -m "feat: wire skill bar + synthesize input mappings"
    ```

---

### Task 4: PIE 完整流程测试

**Files:**
- Modify: 当前测试关卡（`.umap`），放置 3 个不同元素球

**Interfaces:**
- Consumes: Task 1 的 `BP_Orb_*`、Task 2 的组合表、Task 3 的输入映射

- [ ] **Step 1: 打开测试关卡**
    - 打开你常用的测试地图（若没有，新建一个 Basic 关卡并放一个 `BP_PlayerCharacter` + 一个地面）

- [ ] **Step 2: 放 3 个不同元素球**
    - 从 Content Browser 拖 `BP_Orb_Fire`、`BP_Orb_Water`、`BP_Orb_Earth` 各一个到场景，放在玩家起点附近（可重叠放置，保证一出生就拾取）

- [ ] **Step 3: 确认玩家 Pawn 带元素组件**
    - 选中场景里的 `BP_PlayerCharacter`，Details 面板确认 **Components** 里有 `ElementSystemComponent`
    - （若没有：Add 组件 → 搜 `Element System`，加 `UElementSystemComponent`）
    - 该组件 Details 里 **Combo Info** 设为 `DA_ElementComboInfo`

- [ ] **Step 4: 跑 PIE**
    - 点工具栏 **Play**（或 `Alt+P`）

- [ ] **Step 5: 验证全流程，逐条确认**
    1. 角色碰到 3 个球后，球消失（被拾取销毁）
    2. 按 `E`（合成键）→ 技能栏第 1 槽出现 `Fireball`（BP_SkillBar 上可见，若 HUD 已挂 BP_SkillBar）
    3. 按 `1`（技能键）→ 角色朝前发射火球（GA_Fireball 生效）

- [ ] **Step 6: 通过标准**
    - 三条全部成立 → 全链路打通，本计划完成
    - 若第 2 步无反应：检查 DA_ElementComboInfo 的 Elements 是否 3 个不同元素、ResultAbility 是否选了 GA_Fireball
    - 若第 3 步无反应：检查 IMC_Default 按键映射 + DA_EleInputConfig 的 Input Tag 是否 `Input.Skill1`

- [ ] **Step 7: 记录结果**
    - 在 PIE 里确认后截图或记下结果，作为本轮验收证据

---

## 已知边界（本轮不处理，记录备查）

- **冷却 UI 未接线**：`TickComponent` 已更新 `CooldownRemaining`，但 SkillBar ViewModel 未暴露冷却字段，技能栏暂不显示冷却倒计时。
- **技能栏满的合成行为**：`Synthesize` 在 4 槽全满时返回 false（设计里移除技能是关卡内 gated 动作，尚未实现移除节点）。
- **元素球外观为占位**：Sprite 暂用 Coin flipbook，7 元素专属美术后续替换。
