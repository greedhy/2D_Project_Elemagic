# Elemagic 框架设计

日期: 2026-08-07
状态: 已批准(设计阶段),待写实现计划

## 背景与目标

Elemagic 是一个 2D 横版 roguelike 游戏,核心机制是"拾取元素、合成技能"。项目已经有一部分骨架代码,并且在有意无意地模仿同作者的另一个项目 test_25d(2D 俯视角 GAS ARPG,基于 Aura 课程二次开发)的架构风格。

本文档的目标不是设计某一个具体功能,而是:
1. 确定框架层面的约定(目录结构、命名、GAS 挂载方式、C++/蓝图分工原则),让后续每个功能增量都有章可循。
2. 给出后续功能的优先级路线图,每一项各自走独立的"头脑风暴 → 计划 → 实现"循环。

参考项目:
- **test_25d**(`C:\Users\greedy\Documents\Unreal Projects\test_25d`):C++ 只做机制(网络同步、GAS 管线、tag 分发),具体内容(角色/技能/拾取物)全部下放蓝图子类;用 UDataAsset 做配置表;tag 驱动的输入/技能激活。
- 参考游戏:动物井(Animal Well)、死亡细胞(Dead Cells)、恶魔城(Castlevania)——最终选定死亡细胞式的"关卡制 + 随机性"结构。

## 核心设计原则(YAGNI 优先于照搬 test_25d)

test_25d 的完整架构(Interface 契约、行为树+黑板 AI、存档系统、多张 UDataAsset 配置表)是为俯视角 ARPG 的规模设计的。Elemagic 现阶段应采用**最小可用框架**:命名/目录/tag 约定现在就钉死(成本低,防风格漂移),但 C++ 骨架只写当前功能增量真正需要的部分,不预先搭建还没有消费者的系统(存档、Interface 契约等按"三次法则"在真正需要共享契约时再抽象)。

数据驱动(UDataAsset 配置表)不作为独立的前置阶段,而是"用到哪个系统就在那个系统内部做数据驱动"——例如元素组合表在做"元素合成"功能时才建。

## 1. 目录与命名约定

```
Source/Elemagic/
  Public|Private/
    Character/     CharacterBase, PlayerCharacter, EnemyCharacter(新增)
    Player/         MyPlayerState(承接ASC)、MyPlayerController
    Game/           MyGameModeBase、MyGameInstance
    AbilitySystem/  ElemagicGameplayAbility、CharacterAttributeSetBase、Ability/(具体技能基类)
    Input/          EleInputConfig、EleInputComponent(已存在)
    Actor/          拾取物等通用 Actor 基类(新增,元素拾取物放这里)
    UI/             ViewModel/、Widget/(新增,MVVM 用)

Content/
  Blueprint/
    Character/Player、Character/Enemy
    AbilitySystem/Ability、AbilitySystem/Data(组合表等 DataAsset)
    Actor/ElementPickup
    UI/View、UI/ViewModel
    Input/(已存在)
  Assets/(现有 Asset/ 目录对齐 test_25d 的 Assets/ 命名,内部按 Character/Enemy/Effects 细分)
  Level/
```

命名前缀延用 test_25d:`BP_`(蓝图 Actor/组件)、`GA_`(技能)、`GE_`(GameplayEffect)、`DA_`(数据资产)、`WBP_`(Widget)、`IA_`/`IMC_`(输入)。

GameplayTag 新增命名空间:
- `Element.*`(`Element.Fire`/`Water`/`Earth`/`Wind`/`Lightning`/`Ice`/`Light` —— 7 个占位,具体清单在做元素功能时确定)
- `Ability.Combo.*`(合成后技能的激活/输入 tag,以及合成动作本身的输入 tag `Ability.Combo.Synthesize`)

现有 `State.*`/`Data.Damage`/`Ability.Attack`/`Ability.Jump` 保留。

## 2. GAS 约定

- **ASC 挂载**:`AMyPlayerState` 新增 `AbilitySystemComponent` + `UCharacterAttributeSetBase`(仿 test_25d,跨本局内的角色死亡/重生保留状态)。`InitAbilityActorInfo` 区分服务器(`PossessedBy`)和客户端(`OnRep_PlayerState`)两个初始化时机。`ACharacterBase` 不再自己持有 ASC,通过 `IAbilitySystemInterface::GetAbilitySystemComponent()` 转发到 Owner —— 玩家从 PlayerState 取,敌人从自身取,`ACharacterBase` 依然是玩家/敌人共用基类。
- **属性集**:延续现有 `CharacterAttributeSetBase`(Health/MaxHealth/AttackPower/Defense/MoveSpeed),暂不拆分多个 AttributeSet。
- **技能基类**:保留 `UElemagicGameplayAbility` 作为总基类。元素技能作为其子类(`AbilitySystem/Ability/` 目录),具体判定/数值优先蓝图实现(仿 `GA_Attack` 现有的 `PerformAttack` BlueprintImplementableEvent 模式),C++ 只提供必要的可调用工具函数。
- **伤害规则**:伤害必须通过 GameplayEffect 应用,不允许 Ability 里直接改 Health 属性。具体的 GE 类和 `PostGameplayEffectExecute` 扣血逻辑在"敌人+战斗闭环"增量里补上(目前 `InitializeAttributes` 是空实现)。

## 3. 元素合成技能系统(架构草图)

- **`AElementPickupActor`**(C++ 基类,`Actor/`):携带 `FGameplayTag ElementTag`(EditAnywhere),触碰后把该 tag 交给拾取者的加载器组件。7 种元素对应 7 个蓝图子类,复用同一份重叠检测逻辑(与 test_25d 的 `MyEffectActor` 同一思路:一个 C++ 基类、多个 BP 变体)。
- **`UElementLoadoutComponent`**(C++ ActorComponent,挂在玩家角色/PlayerState 上):持有最多 3 个未合成的 `FGameplayTag`,提供 `TryAddElement`/`RemoveElement`/`GetHeldElements`,并广播加载器变化委托供 UI ViewModel 订阅。
- **`UElementComboInfo`**(C++ UDataAsset,`AbilitySystem/Data/`):`TArray<FElementComboRow>`,每行是`(无序 3 元素 tag 集合 → TSubclassOf<UGameplayAbility>)`。查表用排序后的 tag 数组做 key,天然支持"只实现少数几组,其余先留空"。
- **合成动作**:输入 tag `Ability.Combo.Synthesize` 绑定按键,取出已选的 3 个元素 → 查 `UElementComboInfo` → 命中则 `GiveAbility`,若已拥有该技能则对已有 `FGameplayAbilitySpec` 做 `Level++` → 清空加载器。查不到组合时的处理方式留到该功能实现时再定。
- **技能栏**:`USkillBarComponent`(C++,4 槽位),记录已授予的组合技能与槽位映射,暴露 `RemoveSkillAt(int32 SlotIndex)` 接口。"在特定节点删除技能"的具体触发属于关卡系统,框架层只约定组件接口。

## 4. 敌人 / AI 约定

- `AEnemyCharacter`(新增,继承 `ACharacterBase`):自带 ASC(不走 PlayerState)。
- AI 用简单 C++ 状态机(`EEnemyState { Patrol, Chase, Attack, Dead }`),不引入行为树+黑板 —— 横版敌人的空间判定远比 test_25d 的俯视角+EQS 简单,行为树是为那个规模设计的。逻辑封装在 `AEnemyCharacter` 单个类里,以后如需要可替换为行为树,重构成本可控。
- 攻击时机、受击表现等通过 `BlueprintImplementableEvent`/`BlueprintNativeEvent` 挂钩下放蓝图子类;先只抽最少必要的几个方法(如 `GetAttackMontage`/`IsDead`),不预先照搬 test_25d 完整的 `CombatInterface`。

## 5. UI 架构(MVVM)

- 每个 UI 功能都是 **View**(UMG Widget,继承新增的 `UElemagicUserWidget` 基类)+ **ViewModel**(继承 `UMVVMViewModelBase`,`FieldNotify` 暴露数据)。View 不直接持有游戏对象引用,只绑定 ViewModel。
- ViewModel 数据来源:订阅 `CharacterAttributeSetBase` 属性变化委托(血条)、`UElementLoadoutComponent` 加载器变化委托(元素槽)、`USkillBarComponent` 槽位变化委托(技能栏)。UI 层完全通过订阅这些已有委托取数据,不额外建 WidgetController 中间层(test_25d 简单 HUD 用 WidgetController,Elemagic 统一用 MVVM,由 ViewModel 承担这个职责)。
- 具体的血条/元素槽/技能栏 View、ViewModel 不在本设计文档展开,留到对应功能增量单独设计。

## 6. 功能路线图(增量顺序)

1. **角色移动控制**(当前优先)—— 打磨现有 `PlayerCharacter`/`CharacterBase` 的跑动、跳跃(含 `JumpMaxHoldTime` 可变跳)、朝向翻转,确认 Paper2D flipbook 动画状态机(Idle/Run/Jump)衔接正确,作为后续功能的地基验证。
2. **敌人 + 基础战斗闭环** —— `AEnemyCharacter` 状态机、伤害走 GameplayEffect(补上 `InitializeAttributes`/`PostGameplayEffectExecute`)、`GA_Attack` 打出真实伤害并能击杀敌人。
3. **元素拾取 → 合成技能链路** —— `AElementPickupActor`、`UElementLoadoutComponent`、`UElementComboInfo`(先做 2-3 组占位组合)、合成输入、`USkillBarComponent`。
4. **核心 UI**(血条 + 元素槽 + 技能栏的 MVVM View/ViewModel)—— 依赖 1-3 已有可订阅的数据源。
5. **关卡随机结构骨架**(仿死亡细胞的房间/节点连接与切换,含"删除技能的节点"交互点)。
6. 后续(顺序留到前几项完成后再排):存档/局外成长、更多元素组合与敌人种类、拾取物(金币等)。

每一项都是独立的"头脑风暴 → 计划 → 实现"循环。

## 未决问题(留到对应功能增量时再定)

- 玩家已拥有 4 个技能、又合成出一个全新组合(未拥有过)时如何处理。
- 查不到匹配组合(尚未实现的组合)时,3 个元素如何反馈给玩家。
- 敌人受伤/死亡时的具体表现、判定细节。
- Element.* 具体 7 个元素的最终清单与主题定位。
