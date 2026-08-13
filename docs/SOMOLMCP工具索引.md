# SOMOLMCP 工具索引（UE 5.8.1 / 3.13.5）

> 源文档：`SOMOLMCP_FabStandalone_UE58_3.13.5_20260811` 交付包。
> 本文是**快速调取索引**，用于在 3130 个工具里快速定位"该用哪个工具"。
> 权威数据：`ReferenceData/ToolCatalog/SOMOLMCP_TOOLS_CATALOG.json`（机器可读）与运行时 `tools/list`。

---

## 0. 一分钟上手

| 事项 | 做法 |
|---|---|
| 连接入口 | 原生 TCP `127.0.0.1:12000`；标准 MCP Streamable HTTP `http://127.0.0.1:12001/mcp` |
| 发现工具 | 先 `initialize`，再分页 `tools/list`，或打开 `FAB_SOMOLMCP_TOOL_EXPLORER_ZH.html` 按前缀过滤 |
| 长任务 | 用 `job_submit` 提交（立即返回 `job_id`），`job_get`/`job_await` 查结果，`job_events` 看进度，`job_cancel` 取消 |
| 写操作纪律 | 绑定目标 → 加锁 → 执行 → 回读(readback) → 截图 → receipt 验收；批量按锁/lane 分组 |
| 批量 | `tools_batch_execute`、各 `*_batch_*` 工具（单次 GameThread 进入，最多 1000 项） |

**Job / 协议类工具**（长任务与队列核心）：`job_submit` `job_get` `job_await` `job_events` `job_cancel` `job_run_plan` `tools_batch_execute` `tools_capabilities_describe`。

---

## 1. 工具命名规范

- **前缀 = 领域**。如 `blueprint_` 蓝图、`material_` 材质、`niagara_` 特效、`pcg_` 程序化生成。
- `_v2`/`_v3` 后缀 = 版本迭代；`[ALIAS]` = 别名（委托到正式工具）；`_native` = 原生 C++ 实现名。
- `_batch` / `_batch_lite` = 批量变体；`_inspect`/`_list`/`_get` = 只读；`_create`/`_set`/`_apply` = 写入。
- `_plan` = 规划/预演（多为 dry-run）；`_receipt`/`_validate`/`_audit` = 验收与校验。
- **查询时先用前缀过滤，不要从自然语言猜工具名**；找不到就刷新目录。

---

## 2. 12 大领域总览（官方工具数）

| 领域 | 工具数 |
|---|---:|
| 地形、水体与世界构建 | 309 |
| 资产、导入、网格与批处理 | 419 |
| 蓝图、游戏逻辑与 AI | 751 |
| 动画、骨骼、角色与 Control Rig | 309 |
| 材质、贴图与渲染 | 181 |
| PCG、植被与生态 | 144 |
| 关卡、Actor 与 World Partition | 121 |
| 界面、UMG、Slate 与 XR | 157 |
| 镜头、Sequencer、灯光与后处理 | 158 |
| 特效与 Niagara | 111 |
| 音频、MetaSound 与媒体 | 54 |
| 项目设置、构建、自动化与诊断 | 416 |

---

## 3. 前缀速查表（字母序 · 前缀 → 含义 → 数量）

数量来自 `SOMOLMCP_TOOLS_CATALOG.md` 实际统计。仅列数量 ≥ 2 的前缀；单个罕见工具直接按名字 grep。

### A–C
| 前缀 | 含义 | 数量 |
|---|---|---:|
| `ability_` | 技能系统（GAS） | 1 |
| `abp_` | 动画蓝图（别名族） | 1 |
| `actor_` | 场景 Actor 操作（移动/对齐/分组/属性） | 34 |
| `ai_` | AI 控制器/感知/行为树运行 | 7 |
| `android_` | Android 平台校验 | 3 |
| `anim_` / `animbp_` / `animnext_` | 动画蓝图状态机/节点（`anim_` 为完整名，`animbp_` 为兼容名） | 29 / 23 / 13 |
| `animation_` | 动画序列编辑（曲线/通知/压缩/蒙太奇） | 53 |
| `architecture_` | 建筑/城市场景批量装配 | 28 |
| `asset_` | 资产分析/批量操作/导入规划 | 79 |
| `audio_` | 音频资产与衰减/响度 | 50 |
| `authoring_` | 创作层封装（Graph Authoring） | 14 |
| `automation_` | 自动化测试 | 12 |
| `avalanche_` | Avalanche 视效 | 2 |
| `axf_` | AXF 材质交换格式 | 8 |
| `behavior_` / `behaviortree_` / `bt_` | 行为树 | 10 / 7 / 3 |
| `blackboard_` | 行为树黑板 | 8 |
| `blendspace_` | 混合空间 | 6 |
| `blueprint_` | 蓝图图编辑/编译/节点 | 133 |
| `brush_` | 笔刷/过滤/遮罩（编辑工具） | 18 |
| `bsp_` | BSP/CSG 白模构建 | 15 |
| `build_` | 构建 | 2 |
| `camera_` | 相机动画/参数 | 30 |
| `chaos_` | Chaos 物理/破坏/缓存 | 32 |
| `character_` | 角色动作/动画绑定 | 15 |
| `chooser_` | Chooser 选择器 | 3 |
| `cloth_` | 布料 | 23 |
| `collision_` | 碰撞 | 4 |
| `commonui_` | CommonUI 框架 | 19 |
| `composure_` | Composure 合成 | 4 |
| `content_` | 内容浏览器 | 9 |
| `control_` | Control Rig（动画控制） | 98 |
| `conversation_` | 对话系统 | 4 |
| `cook_` | 烘焙/Cook | 7 |
| `curve_` / `curvetable_` | 曲线 / 曲线表 | 5 / 1 |

### D–I
| 前缀 | 含义 | 数量 |
|---|---|---:|
| `data_` | 数据层/Data Layer | 19 |
| `dataasset_` | 数据资产 | 3 |
| `dataflow_` | Dataflow 图 | 17 |
| `datasmith_` | Datasmith 导入 | 2 |
| `datatable_` | 数据表 | 11 |
| `debug_` | 调试 | 4 |
| `derived_` | 派生数据（DDC） | 6 |
| `desktop_` | 桌面平台 | 5 |
| `direct_` | DirectMeshControl | 5 |
| `editor_` | 编辑器 API（`editor_api_call` 等核心入口） | 72 |
| `embody_` | Embody 化身 | 6 |
| `enhanced_` | Enhanced Input | 2 |
| `eqs_` | 环境查询系统 | 2 |
| `fast_` | Fast 建筑网格 | 9 |
| `fbx_` | FBX 导入 | 3 |
| `fog_` | 雾效 | 4 |
| `foliage_` | 植被 | 37 |
| `fracture_` | 几何体破坏/断裂 | 38 |
| `game_` / `gamemode_` | 游戏模式 | 4 / 2 |
| `gameplay_` | Gameplay 技能/能力（GAS 创建） | 14 |
| `gas_` | Gameplay Ability System | 11 |
| `geometry_` | GeometryScript 几何处理 | 34 |
| `handle_` | 会话句柄管理（本会话已连接） | 3 |
| `hlod_` | HLOD 层级 LOD | 9 |
| `horde_` | Horde CI | 4 |
| `ik_` | IK 逆向运动学 | 5 |
| `import_` | 导入 | 2 |
| `incremental_` | 增量构建 | 7 |
| `input_` | 输入映射 | 6 |
| `insights_` | Unreal Insights 诊断 | 16 |
| `interchange_` | Interchange 导入框架 | 57 |
| `ios_` | iOS 平台 | 9 |
| `iris_` | Iris 网络复制 | 8 |

### J–M
| 前缀 | 含义 | 数量 |
|---|---|---:|
| `job_` | 长任务队列 | 6 |
| `landscape_` | 地形（Landscape） | 70 |
| `level_` | 关卡 | 16 |
| `light_` / `lighting_` | 灯光 / 光照构建 | 10 / 5 |
| `livelink_` | Live Link | 10 |
| `lod_` | LOD | 3 |
| `lumen_` | Lumen 全局光照 | 6 |
| `mass_` | Mass AI/人群 | 20 |
| `material_` | 材质 | 92 |
| `mcp_` | MCP 内部能力/蓝图可调用 | 15 |
| `media_` | 媒体播放 | 23 |
| `megalights_` | MegaLights 灯光 | 12 |
| `mesh_` | 网格（mesh_paint 等，UE5.8 新） | 187 |
| `metahuman_` | MetaHuman | 34 |
| `metasound_` | MetaSound 音频 | 14 |
| `mobile_` | 移动平台 | 7 |
| `mocap_` | 动捕 | 5 |
| `modeling_` | 建模工具（DynamicMesh 变形） | 40 |
| `modular_` | 模块化 | 5 |
| `motion_` | Motion 动作 | 13 |
| `mover_` | Mover 移动组件（UE5.8 新） | 28 |
| `movie_` | 电影渲染队列（MRQ） | 23 |
| `mpc_` | 材质参数集合 | 3 |
| `mrq_` | Movie Render Queue | 5 |
| `mutable_` | Mutable 自定义对象 | 23 |

### N–S
| 前缀 | 含义 | 数量 |
|---|---|---:|
| `nav_` / `navmesh_` / `navigation_` | 导航网格 | 2 / 4 / 4 |
| `ndisplay_` | nDisplay 多屏 | 2 |
| `net_` / `network_` | 网络 | 3 / 9 |
| `neural_` | 神经网络推理 | 2 |
| `niagara_` | Niagara 特效 | 97 |
| `nne_` | 神经网络引擎 | 8 |
| `outfit_` | 服装 | 8 |
| `packaging_` | 打包 | 6 |
| `pak_` | Pak 打包/分块 | 9 |
| `pcg_` | 程序化内容生成 | 172 |
| `physics_` | 物理资产 | 24 |
| `pie_` | PIE 运行 | 7 |
| `plugin_` | 插件管理 | 7 |
| `pose_` | 姿势 | 9 |
| `pp_` | 后处理 | 12 |
| `preview_` | 预览 | 7 |
| `procedural_` | 程序化植被 | 4 |
| `project_` | 项目设置 | 11 |
| `pve_` | 程序化植被编辑器 | 20 |
| `realtime_` | 实时 | 4 |
| `render_` | 渲染队列 | 8 |
| `retarget_` | 重定向 | 7 |
| `rig_` / `rigvm_` / `rigmapper_` | Rig / RigVM / RigMapper | 5 / 6 / 6 |
| `rivermax_` | Rivermax 视频 | 3 |
| `sandboxed_` | 沙箱执行 | 6 |
| `scene_` | 场景批量生成/上下文 | 8 |
| `semantic_` | 语义 | 6 |
| `sequence_` | 关卡序列 | 41 |
| `sequencer_` | Sequencer | 81 |
| `skeletal_` / `skelmesh_` / `skeleton_` | 骨骼网格 | 18 / 2 / 2 |
| `slate_` | Slate UI | 84 |
| `smart_` | SmartObject | 14 |
| `source_` / `sourcecontrol_` | 源码 / 版本控制 | 10 / 4 |
| `spline_` | 样条 | 5 |
| `state_` / `statetree_` | StateTree 状态树 | 18 / 6 |
| `static_` / `staticmesh_` | 静态网格 | 10 / 6 |
| `steam_` | Steam 平台 | 3 |
| `substrate_` | Substrate 材质 | 13 |

### T–Z
| 前缀 | 含义 | 数量 |
|---|---|---:|
| `take_` | Take Recorder | 4 |
| `teds_` | TEDS 数据表系统 | 18 |
| `terrain_` | 地形（Mesh Terrain） | 8 |
| `texture_` | 贴图 | 13 |
| `tiled_` | 平铺世界 | 3 |
| `transaction_` | 编辑器事务 | 4 |
| `uaf_` | UAF 动画框架 | 15 |
| `ue58_` | UE5.8 专项能力探测 | 50 |
| `umg_` | UMG 界面 | 87 |
| `unified_` | 统一 | 8 |
| `unreal_` | Unreal 通用 | 5 |
| `usd_` | USD | 4 |
| `variant_` | Variant 变体 | 3 |
| `vcam_` | 虚拟相机 | 3 |
| `video_` | 视频 | 6 |
| `water_` | 水体 | 24 |
| `waveform_` | 波形（音频） | 9 |
| `world_` | 世界/关卡状态 | 35 |
| `worldpartition_` | World Partition | 10 |
| `xr_` | XR | 5 |
| `zen_` | Zen 加载器 | 7 |
| `zonegraph_` | ZoneGraph | 3 |

---

## 4. 高频工作流速查（按任务找工具）

| 想做什么 | 先 grep 这些前缀/工具 |
|---|---|
| 移动/对齐/分组场景物体 | `actor_`（`actor_set_transform` `actor_align` `actor_group_create` `actor_spawn` `actor_list`） |
| 批量生成/改属性 | `actor_spawn_batch_lite` `actor_set_properties_batch` `actor_set_visibility_batch` `scene_batch_spawn` |
| 编译/编辑蓝图 | `blueprint_`（`blueprint_compile` `blueprint_add_node` `blueprint_action_catalog`） |
| 做材质 | `material_` / `substrate_`（`material_add_node` `material_connect_nodes`） |
| 做特效 | `niagara_` |
| 程序化地形/生态 | `pcg_` `landscape_` `foliage_` `pve_` `water_` `terrain_` |
| 动画/状态机 | `anim_blueprint_*` `animation_*` `control_rig_*` |
| 关卡序列/电影 | `sequencer_` `sequence_` `movie_` `mrq_` `take_` |
| UI | `umg_` `slate_` `commonui_` |
| 音频 | `audio_` `metasound_` `media_` |
| 破坏/物理 | `fracture_` `chaos_` `physics_` |
| 导入/迁移资产 | `interchange_` `asset_` `usd_` `datasmith_`（跨项目迁移见 `CROSS_PROJECT_ASSET_MIGRATION` 文档） |
| 打包/发布 | `pak_` `cook_` `packaging_` `build_` `project_` |
| 诊断/自动化 | `insights_` `automation_` `editor_` `debug_` |
| 几何脚本处理网格 | `geometry_` `modeling_` |

---

## 5. 本会话已连接的 MCP 工具（somolmcp）

当前 Claude Code 会话通过 `somolmcp` MCP server 暴露的是**精选子集**，与目录同名：

| 工具 | 用途 |
|---|---|
| `editor_api_domains` | 列出可用 API 域（DynamicMaterial/ClonerEffector/USD/Datasmith…） |
| `editor_api_catalog` | 列出某域可调用函数及参数 |
| `editor_api_call` | 调用单个 API 函数（静态或按句柄） |
| `editor_api_call_batch` | 一次队列里顺序执行多个 API 调用（`$prev` 引用上一步句柄） |
| `geometry_mesh_open` | 打开 StaticMesh 为动态网格会话（返回句柄） |
| `geometry_op_catalog` | 列出 GeometryScripting 操作 |
| `geometry_op_apply` / `geometry_op_apply_batch` | 对动态网格执行操作 |
| `geometry_mesh_query` | 测量网格（计数/边界/面积/体积/封闭性） |
| `geometry_mesh_save_to_asset` | 写回 StaticMesh 资产 |
| `handle_list` / `handle_inspect` / `handle_release` | 管理会话句柄（防 GC） |
| `tool_catalog` | 按预算发现本 server 的全部工具（groups/names/schemas） |

> 完整 3130 工具需通过 `127.0.0.1:12000`（TCP）或 `12001/mcp`（HTTP）连接编辑器后 `tools/list` 获取。

---

## 6. 关键提醒

- **工具数≠当前能力**：以运行时 `tools/list` 为准；工具不存在时刷新目录，不要用旧别名替代。
- **长任务必须走 Job 队列**：`job_submit` 立即返回 `job_id`，避免同步等待误判超时。
- **写操作四件套**：目标绑定 → 资源锁 → readback 回读 → receipt 验收；不能只看首个布尔值就判定成功。
- **批量要分组**：按资源锁/执行 lane 分组，禁止无界并发。
- **各 UE 次版本包不可混用**：预编译二进制与工具目录按版本生成。
- **文档矩阵**：每个文档族都有 ZH/EN 的 Markdown + HTML 四份；UE5.8 另有 Mesh Terrain 专项 `UE58_MESH_TERRAIN_TOOLSET_GUIDE_ZH.md`。
