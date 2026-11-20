# YsmBedrock

YsmBedrock 是 [Yes Steve Model](https://github.com/YesSteveModel/YesSteveModel) 面向 Minecraft 基岩版 Windows 客户端的非官方移植，支持导入 YSM 模型、替换玩家外观和播放动作。基岩版移植代码主要由 OpenAI Codex 编写，项目由 XiaViolet 维护。

**内测版 0.29.20** · Windows x64 · Minecraft 1.26.60.05 · LeviLamina 26.60.7／26.60.8

## 安装与反馈

[YsmBedrock-0.29.20.mcmod 下载页](https://github.com/XiaViolet/YsmBedrock/blob/downloads/YsmBedrock-0.29.20.mcmod)。打开后点击文件页的下载按钮。

下载后，在 LeviLauncher 对应 Windows 实例的「模组管理」中导入。游戏内按 `Alt+Y` 打开模型库，按 `Z` 打开动作转盘。

- [内测说明与验证范围](docs/0.29.20-内测说明.md)
- [项目功能手册](docs/项目功能手册.md)
- [房间同步](docs/0.29.19-房间模型同步.md)与[中继联机配置](docs/联机使用.md)

请在 [Issues](https://github.com/XiaViolet/YsmBedrock/issues) 报告问题，注明游戏、LeviLamina 和模组版本，以及复现步骤。

## 源码与构建

- [src/](src/) 包含游戏接入、模型加载、动画和界面实现。
- [tests/](tests/) 包含回归测试及配套夹具。
- 工程使用 xmake、clang-cl 和 LeviLamina 客户端 SDK。用 `tools/build.ps1 -Test` 构建并运行回归，细节见[维护框架](docs/维护框架.md)。
- [Java 功能对照](docs/Java功能对照.md)记录实现差异与验证结果。

## 来源与许可

上游代码及依赖见[第三方声明](THIRD_PARTY_NOTICES.md)和[许可文件](licenses/)。完整打包所需的 Wine Fox（酒狐）模型见[来源和作者署名](licenses/wine-fox-source.json)。
