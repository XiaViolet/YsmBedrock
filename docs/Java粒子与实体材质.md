# Java 粒子与实体材质

## 原版粒子覆盖

对照项目固定的 Java **1.20.1** 客户端，从 `ParticleTypes` 的注册调用读取 95 个 ID 及参数要求。可用 `tools/generate_java_particles.py` 重建注册表，用 `ysm-inspect --particle-coverage` 导出带基岩版发射器的映射。下列分类描述映射覆盖。全表通过同一段脚本回放，游戏截图验证了其中四种效果。 [固定注册表](../tests/fixtures/java_particle_registry_1_20_1.json)

### 可对应（52）

`angry_villager`、`bubble`、`bubble_column_up`、`campfire_cosy_smoke`、`campfire_signal_smoke`、`cherry_leaves`、`crit`、`current_down`、`dolphin`、`dragon_breath`、`dripping_dripstone_lava`、`dripping_dripstone_water`、`dripping_honey`、`dripping_lava`、`dripping_obsidian_tear`、`dripping_water`、`electric_spark`、`enchant`、`enchanted_hit`、`end_rod`、`explosion`、`explosion_emitter`、`falling_nectar`、`falling_spore_blossom`、`firework`、`fishing`、`flame`、`happy_villager`、`heart`、`item_slime`、`item_snowball`、`large_smoke`、`lava`、`mycelium`、`note`、`portal`、`rain`、`reverse_portal`、`sculk_charge_pop`、`sculk_soul`、`smoke`、`sneeze`、`snowflake`、`sonic_boom`、`soul`、`soul_fire_flame`、`spit`、`splash`、`spore_blossom_air`、`squid_ink`、`totem_of_undying`、`witch`。

### 近似实现（36）

`ambient_entity_effect`、`block`、`block_marker`、`bubble_pop`、`cloud`、`composter`、`damage_indicator`、`dust`、`dust_color_transition`、`effect`、`elder_guardian`、`entity_effect`、`falling_dripstone_lava`、`falling_dripstone_water`、`falling_dust`、`falling_honey`、`falling_lava`、`falling_obsidian_tear`、`falling_water`、`glow`、`glow_squid_ink`、`instant_effect`、`item`、`landing_honey`、`landing_lava`、`landing_obsidian_tear`、`nautilus`、`poof`、`scrape`、`sculk_charge`、`shriek`、`small_flame`、`sweep_attack`、`underwater`、`wax_off`、`wax_on`。

- 横扫由 13 个末地烛粒子排成白弧。伤害指示使用缩小、调色的原版心形。魔法暴击使用基岩版原生的魔法暴击发射器。 [末地烛](https://raw.githubusercontent.com/Mojang/bedrock-samples/main/resource_pack/particles/endrod.json) · [心形](https://raw.githubusercontent.com/Mojang/bedrock-samples/main/resource_pack/particles/heart.json) · [魔法暴击](https://raw.githubusercontent.com/Mojang/bedrock-samples/main/resource_pack/particles/magic_critical_hit.json)
- 方块及物品碎片按基岩版的资源 ID 取默认状态或图标。液滴在基岩版发射器内经历自身的阶段。尖啸接受 Java 的延时参数，目前在调用时直接发射。

### 仍缺失（7）

`ash`、`crimson_spore`、`egg_crack`、`flash`、`vibration`、`warped_spore`、`white_ash`。

这些合法调用会记录在 `particle-status.json` 的 `selectors.*.unmapped`，同一段脚本后面的效果仍会发出。游戏验证脚本依次调用横扫、7 次魔法暴击、振动、伤害心形和 6 次火焰。0.29.0 的游戏采样收到 3,136 次请求，原生发射 5,292 个粒子，记录 196 次振动缺失，原生发射失败为 0。

## 替换实体的动态材质

用同一份 6 秒动画驱动玩家、真实箭实体和已乘坐的船。三个对象都求出了移动、红色／青色、透明度 90／225 和发光 15／0。加载时已识别骨骼材质，求值也正常。修复把每个替换实体自己的状态通过渲染控制器和网格绑定送到绘制，包含拆为独立表面的骨骼。 [测试动画](../tests/fixtures/java_surface_animation.json)

进入游戏后可在游戏实例的 `mods/YsmBedrock/target-assignments-status.json` 看 `entity_routes`。按实体 `entity` 和实例 ID `actor_id` 查 `stage`、`result`，用 `last_seen_tick` 区分当前和较早的观测。

| 阶段 | 查看内容 |
| --- | --- |
| `feature_switch` | `target-render-probe.enable` 和投射物／载具显示设置，具体停用项见 `reason` |
| `ownership` | 箭的原生归属、船的首位乘员和有效模型归属 |
| `target_match` | 已分配的模型包、原生实体 ID 和用于匹配的 Java ID |
| `animation` | 是否采样、活动动画、是否写入骨架 |
| `draw` | 原生模型创建、网格绑定和材质绘制计数 |

游戏对照中的未乘坐船停在 `ownership`（`no_active_model_owner`）。测试包未声明三叉戟，已有归属的三叉戟停在 `target_match`（`no_matching_target_in_assigned_pack`）。

用未改动的临战爱丽丝原包在 0.29.0 复测。飞行箭与入地箭分别选中 `air` 和 `ground`，均将姿态应用到 233 个原生骨骼，把自己的材质状态交给 30 个网格，网格匹配失败和表达式运行失败皆为 0。实机可见飞行箭的蓝紫色效果。原始模型文件共 154 项，更新前后的 SHA-256 一致。
