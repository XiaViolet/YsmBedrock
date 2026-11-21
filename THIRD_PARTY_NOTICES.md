# 来源

- 魔法酒狐含正式版原包的 24 个文件，作者为星海境和伊蕾娜教代行教皇，按 CC BY-NC-SA 4.0 分发。[来源和署名](licenses/magical-wine-fox-source.json)
- 随包内置 Wine Fox（酒狐）基础款。43 个资源文件逐字节取自 Java YSM 2.6.5 Fabric 1.21.1 正式版；仅重新装入 ZIP。完整作者署名保留在原始 `ysm.json`，并附在[资源记录](licenses/wine-fox-source.json)中。该模型按 CC BY-NC-SA 4.0 分发，含此模型的公开包按非商业用途发布。[官方发行页](https://modrinth.com/mod/yes-steve-model/version/eGOUtHWJ) · [模型许可](https://creativecommons.org/licenses/by-nc-sa/4.0/)
- 绘梨衣为开发者自行提供的回归样本。
- YSM：<https://github.com/YesSteveModel/YesSteveModel>，参考提交 `15c2c25f5b50f54ee099bc474483dc4b588d956d`。Head 定位器、控制器状态机、内置播放器、骨骼复位、函数/控制器覆盖、defer 回调、本地 sync 独立交付轮、动作混合、纹理声明及时间线语义对照此提交改编为 C++。
- YesSteveModel-Native：<https://github.com/YesSteveModel/YesSteveModel-Native>，参考提交 `907d1c653b7eb85ee92ee11305134408300797f8`，Apache-2.0。v1/v2 容器读取参考 `modules/legacy/src/bundle.cc` 改为使用 Windows 密码服务和 miniz。v3 适配采用官方容器读取器。
- LeviLamina：<https://github.com/LiteLDev/LeviLamina>，构建依赖 26.60.7（客户端），LGPL-3.0；接口参考 checkout `dacca851fe5f8b97e292496f702db3a73b118f39`。
- 默认动画资产来自 Java YSM 2.6.5 NeoForge 26.1.2 正式发行包的 CC0 默认模板，覆盖身体、第一人称、载具和投射物。来源和校验方式见 `licenses/ysm-default-animations.md`。[官方发行包](https://modrinth.com/mod/yes-steve-model/version/2.6.5-neoforge%2Bmc26.1-hotfix)
- miniz：<https://github.com/richgel999/miniz>，3.1.0，MIT。
- stb_image：<https://github.com/nothings/stb>，来自 LeviLamina 的固定依赖，MIT / public domain。
- libjpeg-turbo：<https://github.com/libjpeg-turbo/libjpeg-turbo>，3.1.2，静态链接用于 JPEG 解码；许可和 IJG 声明保存在 `licenses/libjpeg-turbo-LICENSE.md`、`licenses/libjpeg-turbo-README.ijg`。
- libwebp：<https://chromium.googlesource.com/webm/libwebp/>，1.6.0，静态链接解码器与容器解析器；来源版本及校验值固定在 `packages/libwebp.lua`，许可、专利授权与作者名单保存在 `licenses/libwebp-*`。
- libavif：<https://github.com/AOMediaCodec/libavif>，1.4.2，静态链接用于 AVIF 容器和像素转换；固定来源及 clang-cl 静态归档构建修正在 `packages/libavif.lua`。许可及所含 libyuv 标量转换代码声明保存在 `licenses/libavif-LICENSE`。`tests/avif_upstream_fixtures.py` 保留此版本的 idat、渐进、裁剪测试图片，并将 12 位动画的主图重新封装为静态测试样例，未改动编码像素。
- dav1d：<https://www.videolan.org/projects/dav1d.html>，1.5.2，静态链接用于 AV1 解码；来源及校验值固定在 `packages/dav1d.lua`，许可保存在 `licenses/dav1d-COPYING`。
- nlohmann/json：<https://github.com/nlohmann/json>，3.12.0，MIT。
- fmt：<https://github.com/fmtlib/fmt>，11.2.0，MIT。

上游 checkout 放在 `vendor/`，不修改；本工程的客户端适配代码在 `src/`。
`src/YsmUi.cpp` 对照同一 YSM 提交的 `ModelInfoBuilder`、`AnimationRouletteScreen`、`ConfigActionRoute` 和本地呈现决策改编设置路由与转盘语义，遵循 Apache-2.0；基岩版原生绘制/输入适配在 `src/NativeYsmUi.cpp`，独立基础材质预览在 `src/ModelPreview.cpp`。
`src/MolangPhysics.cpp` 对照 YSM 的 `FirstOrder.java`、`SecondOrder.java`（MicroCraft）、`PhysicsManager.java` 和 `ModelBuiltins.physics` 改写为 C++，保留浮点和生命周期规则并增加运行边界，遵循 Apache-2.0。`tests/physics_java_vectors.py` 保存直接运行这些 Java 源码获得的数值参考，验证工具位于 `build/verification/`。
`src/EntityQueries.cpp` 的实体查询和转身速度对照同一 YSM 提交的 `EntityBuiltins.java`、`PlayerBuiltins.java`、`PlayerStateTracker.java` 改编为 C++，遵循 Apache-2.0；基岩版输入适配位于 `src/AnimationRuntime.cpp`。
依赖许可文本保存在 `licenses/`。
- Java 动画输入：原版标签成员关系取自 Minecraft Java 1.20.1、Forge 47.4.3 与上述 YSM 提交，来源和校验值见 `licenses/java-animation-inputs.md`、`licenses/java-animation-tags-source.json`。鞘翅倾角为独立 C++ 实现，参考值通过运行对应官方 Java 客户端类取得；安装包不包含游戏或 Forge 二进制。
- Java/基岩版音效名称映射取自 GeyserMC/mappings，提交 `e9271c00413bc75d0df5b3c4f1d9ca9c9764d4c7`，MIT。固定数据及 SHA256 见 `vendor/geyser-mappings/source.json`，编译表位于 `src/JavaSoundMappings.inc`，许可保存在 `licenses/GeyserMC-mappings-LICENSE`。映射仅提供名称和音高转换，具体音效由当前游戏资源决定。
音效导入中的 Ogg/Opus/Vorbis 时间线检查对照 YSM SupportedAudioProbe.java 改编，遵循 Apache-2.0。Opus 播放使用静态链接 libopusfile 0.12+20260328、libopus 1.5.2 和 libogg 1.3.6，BSD 许可文本保存在 licenses/libopusfile-COPYING、licenses/libopus-COPYING 和 licenses/libogg-COPYING。
