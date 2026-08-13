# SOMOLMCP × Elemagic 最常用工具子集

> 从 3130 个工具里按 Elemagic 项目特征筛选出的**高频工具**。
> Elemagic：2D 侧滚动作 roguelike · ASC(GAS) 在 PlayerState · 7元素/3槽位 combo→技能合成 · MVVM UI · Blueprint+C++ 平衡。
> 完整前缀速查见 `docs/SOMOLMCP工具索引.md`。工具名已从 `SOMOLMCP_TOOLS_CATALOG.md` 逐一核实。

---

## 1. GAS 技能系统（项目核心架构）

技能创建、GameplayEffect、GameplayCue、标签，以及运行时检查 ASC。

| 工具 | 用途 |
|---|---|
| `gas_contract_status` | 探测当前工程 GAS 能力边界（先跑这个确认能做什么） |
| `ability_system_component_inspect` | 检查某 Actor 上的 ASC（PlayerState 架构验证用） |
| `gameplay_ability_create` | 创建 GameplayAbility 蓝图 |
| `gameplay_ability_list` | 列出工程里的技能 |
| `gameplay_effect_create` / `gameplay_effect_configure` | 创建/配置 GameplayEffect |
| `gameplay_cue_create_notify` / `gameplay_cue_list` | 创建/列出 GameplayCue（技能表现） |
| `gameplay_tag_add` / `gameplay_tag_list` / `gameplay_tag_remove` | 管理 GameplayTag（技能标签/元素标签） |
| `gameplay_tag_referencers` | 查某标签被谁引用 |
| `gas_attributes_list` / `gas_attribute_sets_find` / `gas_attribute_info` | 列出/查找/读取 AttributeSet 属性 |
| `gas_gameplay_effect_modifier_add_v2` | 给 GE 加 modifier（数值增减） |

> 元素系统若用 `GameplayTag` 表示 7 种元素，`gameplay_tag_*` 是日常工具。

---

## 2. 蓝图（Blueprint+C++ 日常）

从 133 个 `blueprint_` 里挑核心；其余按 `blueprint_` 前缀 grep。

**编译与诊断**
- `blueprint_compile` · `blueprint_compile_diagnostics` · `blueprint_repair_compile_gate` · `blueprint_analyze_graph`

**创建 / 读取**
- `blueprint_create` · `blueprint_read` · `blueprint_read_graph_summary` · `blueprint_graph_explain`（解释图逻辑，新手友好）

**节点操作**
- `blueprint_add_node` · `blueprint_connect_pins` · `blueprint_delete_node` · `blueprint_find_nodes` · `blueprint_get_nodes` · `blueprint_get_node_details`
- `blueprint_add_function_call_node` · `blueprint_add_variable_get_node` · `blueprint_add_variable_set_node` · `blueprint_add_event_node` · `blueprint_add_custom_event_node`

**变量 / 函数 / 接口**
- `blueprint_add_member_variable` · `blueprint_get_variables` · `blueprint_get_variable_details` · `blueprint_rename_member_variable`
- `blueprint_add_function_graph` · `blueprint_list_graphs` · `blueprint_implement_interface`

**蓝图调试（新手排错）**
- `blueprint_debug_set_breakpoint` · `blueprint_debug_play` · `blueprint_debug_step_into` · `blueprint_debug_step_over` · `blueprint_debug_get_watches` · `blueprint_debug_get_call_stack`

---

## 3. UI / MVVM（元素系统 + 技能栏 UI）

从 87 个 `umg_` 里挑核心；MVVM 绑定工具是重点。

**Widget 蓝图创建与编译**
- `umg_widget_blueprint_create` / `_v2` · `umg_widget_blueprint_compile_v2` · `umg_widget_compile_delivery_gate` · `umg_widget_save_verified` · `umg_widget_designer_validate`

**Widget 树操作**
- `umg_widget_tree_list` · `umg_widget_tree_get_widget` · `umg_widget_tree_add_widget` · `umg_widget_tree_remove_widget` · `umg_widget_tree_rename_widget` · `umg_widget_tree_set_widget_properties` · `umg_widget_tree_inspect_properties`

**MVVM 绑定（元素/技能槽位数据绑定）**
- `umg_widget_bind_property` · `umg_widget_bind_event` · `umg_widget_set_as_variable`
- `umg_binding_create_function_stub` · `umg_binding_validate_function_graph` · `umg_binding_verify_delegate_signature` · `umg_binding_inspect_graph`
- `umg_list_view_bind_items_source` · `umg_list_view_refresh` · `umg_list_view_set_entry_class`（列表显示元素/技能栏）
- `umg_tree_view_bind_hierarchy_source`

**布局与槽位**
- `umg_canvas_slot_configure` · `umg_horizontal_box_slot_configure` · `umg_vertical_box_slot_configure` · `umg_overlay_slot_configure` · `umg_grid_slot_configure`

**CommonUI（可选，若迁移到 CommonUI 框架）**
- `commonui_activatable_widget_create` · `commonui_button_style_inspect` · `commonui_tab_list_configure` · `commonui_input_router_snapshot`

---

## 4. 动画与角色动作（combo / 近战 slash）

**动画蓝图状态机**
- `anim_blueprint_add_state_machine` · `anim_blueprint_add_state` · `anim_blueprint_add_transition` · `anim_blueprint_list_states` · `anim_blueprint_list_transitions`
- 兼容名：`anim_bp_get_state_machine_info` · `anim_bp_set_entry_state`

**动画序列编辑（通知 / 曲线 / 蒙太奇）**
- `animation_create_montage` · `animation_montage_inspect`（做攻击蒙太奇）
- `animation_add_notify` · `animation_add_notify_state` · `animation_list_notifies`（打击帧通知）
- `animation_add_float_curve_key` · `animation_get_float_curve_value`

**角色动作 / 根运动 / 运动扭曲（UE5.8 新）**
- `character_action_control` · `character_action_state_inspect`（角色动作状态，combo 链验证）
- `character_animation_assign` · `character_bind_animation` · `character_montage_control`
- `character_root_motion_set` · `character_motion_warping_target_set` · `animation_root_motion_inspect_native`

---

## 5. 数据表 / 数据资产 / 曲线（技能、元素、属性数值配置）

| 工具 | 用途 |
|---|---|
| `datatable_create` · `datatable_add_row` · `datatable_update_row` · `datatable_get_row` · `datatable_list_rows` · `datatable_delete_row` | 数据表增删改查（技能/元素数值表） |
| `datatable_get_schema` | 读取表结构（需要行类型结构体） |
| `datatable_import_csv` / `datatable_import_json` / `datatable_export_csv` / `datatable_export_json` | 用表格软件批量维护数值后导入 |
| `dataasset_create` · `dataasset_get_property` · `dataasset_set_property` | 数据资产（技能配置对象） |
| `curve_create` · `curve_add_key` · `curve_set_interpolation` · `curve_inspect` | 曲线（伤害成长/冷却曲线） |

---

## 6. 输入（动作游戏输入映射 · Enhanced Input）

- `input_action_create` · `input_action_list`
- `input_mapping_context_create` · `input_mapping_context_add_mapping` · `input_mapping_context_get_mappings` · `input_mapping_context_list`
- `enhanced_input_blueprint_binding_plan`

---

## 7. 场景 / 关卡 / 碰撞（搭建 2.5D 场景）

**Actor 操作**
- `actor_spawn` · `actor_spawn_batch_lite` · `actor_set_transform` · `actor_get_state` · `actor_list`
- `actor_align` · `actor_snap_to_floor` · `actor_place_on_ground` · `actor_group_create`
- `actor_set_properties_batch` · `actor_get_properties_batch`（批量改属性）
- `actor_duplicate` · `actor_destroy`

**关卡 / 世界**
- `level_create` · `level_save` · `level_load` · `level_list` · `level_get_current`
- `world_save_current_level` · `world_save_all_dirty_levels` · `world_get_state` · `world_spawn_character`
- `level_spawn_blockout`（白模搭建）

**碰撞**
- `collision_presets_list` · `collision_preset_configure` · `collision_channels_list` · `collision_channel_create`

**场景查询**
- `scene_get_context` · `scene_find_actors_by_proximity` · `scene_batch_spawn`

---

## 8. 材质（风格化 / 2.5D 表现）

从 92 个 `material_` 里挑核心。

- `material_create` · `material_create_toon`（卡通风格，2D 表现首选）· `material_create_toon_outline` · `material_create_pbr` · `material_create_fresnel_rim`（技能高亮描边）
- `material_add_node` · `material_connect_expressions` · `material_delete_expression` · `material_list_expressions`
- `material_get_all_params` · `material_get_parameter_info` · `material_set_scalar_param` · `material_set_vector_param` · `material_set_texture_param`
- `material_instance_create` · `material_instance_set_parent` · `material_instance_set_scalar_parameter`（技能元素变色用实例）
- `material_recompile` · `material_diagnose` · `material_usage_audit_v2`

---

## 9. 特效 Niagara（技能特效）

从 97 个 `niagara_` 里挑核心。

- `niagara_create_system` · `niagara_create_emitter` · `niagara_add_sprite_renderer`（2D Sprite 特效，契合 2D 项目）· `niagara_add_ribbon_renderer`
- `niagara_add_user_parameter` · `niagara_set_user_parameter_default`
- `niagara_add_module_to_stack` · `niagara_stack_input_set_v2`
- `niagara_compile_system` · `niagara_compile_diagnostics`
- `niagara_component_activate` · `niagara_component_deactivate` · `niagara_component_set_float` · `niagara_component_set_vector`（运行时触发技能特效）
- `niagara_spawn_actor`

---

## 10. 音频（打击音效 / 背景音乐）

从 50 个 `audio_` + 14 个 `metasound_` 里挑核心。

- `audio_import_sound` · `audio_create_sound_cue` · `audio_create_cue` · `audio_place_ambient_sound`
- `audio_asset_inspect` · `audio_asset_loop_point_audit` · `audio_asset_loudness_analyze`
- `audio_set_attenuation` · `audio_sound_class_create` · `audio_sound_mix_create`
- `metasound_create` · `metasound_node_add` · `metasound_nodes_connect` · `metasound_compile`

---

## 11. 测试 / 调试 / 运行（新手验证）

- `pie_start` · `pie_stop` · `pie_get_status` · `pie_screenshot` · `pie_capture`
- `debug_get_callstack` · `debug_set_breakpoint` · `debug_list_breakpoints` · `debug_get_watched_values`
- `automation_list_tests` · `automation_run_tests` · `automation_get_results` · `automation_get_status`

---

## 12. 资产管理 / 编译 / 保存（日常收尾）

从 79 个 `asset_` 里挑核心。

- `asset_save` · `asset_save_safe` · `asset_editor_save_safe`（安全保存，先备份）
- `asset_list` · `asset_search` · `asset_smart_search` · `asset_open_editor` · `asset_editor_compile_active`
- `asset_import` · `asset_ingest_from_disk` · `asset_import_task_execute_safe`
- `asset_find_references` · `asset_dependencies` · `asset_dependency_graph`
- `asset_fix_redirectors` · `asset_replace_references` · `asset_rename` / `asset_rename_safe`
- `asset_duplicate` · `asset_delete`

---

## 13. 长任务 Job 队列（Cook / 批量 / 打包）

| 工具 | 用途 |
|---|---|
| `job_submit` | 提交长任务，立即返回 `job_id` |
| `job_get` / `job_await` | 查结果 / 阻塞等待 |
| `job_events` | 看进度通知 |
| `job_cancel` | 取消任务 |
| `tools_batch_execute` | 批量执行多个工具 |

---

## 快速决策卡

| 场景 | 先 grep | 首选工具 |
|---|---|---|
| 加一个新技能 | `gameplay_` / `gas_` | `gameplay_ability_create` → `gas_contract_status` |
| 改技能数值 | `datatable_` / `dataasset_` | `datatable_update_row` |
| 编蓝图逻辑 | `blueprint_` | `blueprint_compile` → `blueprint_compile_diagnostics` |
| 绑定 UI 数据（MVVM） | `umg_binding` / `umg_widget_bind` | `umg_widget_bind_property` |
| 做攻击动画 | `animation_` / `anim_blueprint` | `animation_create_montage` → `animation_add_notify` |
| 技能特效 | `niagara_` | `niagara_create_system` → `niagara_add_sprite_renderer` |
| 场景摆位 | `actor_` / `level_` | `actor_spawn` → `actor_set_transform` |
| 风格化材质 | `material_` | `material_create_toon` |
| 跑起来看效果 | `pie_` | `pie_start` → `pie_screenshot` |
| 收工保存 | `asset_` | `asset_save_safe` |
