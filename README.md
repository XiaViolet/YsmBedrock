# YsmBedrock

[English](#english) · [简体中文](#简体中文)

> [!IMPORTANT]
> 导入模组前，请先在对应的 Minecraft 实例中安装 **LeviLamina 客户端版**。  
> Install **LeviLamina Client** in your Minecraft instance before importing the mod.

![YsmBedrock gameplay screenshot / YsmBedrock 实机截图](docs/images/gameplay.webp)

## English

YsmBedrock is an unofficial Windows client port of [Yes Steve Model](https://github.com/YesSteveModel/YesSteveModel) for Minecraft Bedrock Edition. Wear custom YSM models, switch textures, and play custom animations and emotes.

### Requirements

- Windows x64 and Minecraft Bedrock Edition **1.26.60.05**.
- [LeviLamina Client 26.60.9](https://github.com/LiteLDev/LeviLamina/releases/tag/v26.60.9) is used for builds. The **0.29.45** preview installers support Client 26.60.7, 26.60.8, and 26.60.9.

### Features

- Import supported `.ysm` files, model ZIP archives, and model folders. The public installers include Wine Fox and Magical Wine Fox.
- Preview and equip models, switch textures, manage favorites, and adjust model settings. Play custom actions from the emote wheel.
- Sync models and appearances in friend rooms with the host's permission. Use the same YsmBedrock version on both sides.
- The development branch adds English and Chinese interfaces that follow Minecraft's language. The interface falls back to English for other game languages. Model text uses author-provided translations where available, with built-in translations for common model and emote labels. Some custom labels may still appear in their original language.
- The development branch also adds four menu themes (Midnight, Light, Forest, and Violet), sharper icons and text, cached UI layouts, and asynchronous model previews with adjustable quality and a frame-rate limit up to 60 FPS.

### Installation and usage

The current public preview is [0.29.45](https://github.com/XiaViolet/YsmBedrock/releases/tag/v0.29.45). See the [flight, emote, equipment and Arke verification notes](docs/0.29.45.md).

1. Install LeviLamina Client in the matching Minecraft instance, then exit the game before importing the mod.
2. In your launcher's **Mod Management → LeviLamina → Import Mod**, select the package for your launcher.

   | Launcher | Package |
   | --- | --- |
   | LeviLauncher | [Download `.mcmod`](https://github.com/XiaViolet/YsmBedrock/releases/download/v0.29.45/YsmBedrock-0.29.45.mcmod) |
   | BedrockBoot | [Download `.zip`](https://github.com/XiaViolet/YsmBedrock/releases/download/v0.29.45/YsmBedrock-0.29.45-BedrockBoot.zip) |

3. Enter a world, press `Alt+Y` to open the model library, and select **Equip Model** after choosing a model.
4. Press `Z` to open the emote wheel.

### Build

Install [xmake](https://xmake.io/), Visual Studio 2022 Build Tools with C++ and the Windows SDK, and LLVM (`clang-cl`). Run this command from the project root in an x64 developer terminal.

```powershell
.\tools\build.ps1
```

The script builds the mod DLL and `ysm-inspect.exe`. To generate launcher archives, run [`tools/package.ps1`](tools/package.ps1) separately with the legacy v3 reader `YsmLegacyV3.dll` and both bundled model archives in place.

### Credits and documentation

XiaViolet maintains the Bedrock port. OpenAI Codex wrote most of the original port code. [RadiantByte](https://github.com/RadiantByte) contributed the new localization and UI work in [PR #1](https://github.com/XiaViolet/YsmBedrock/pull/1).

- [Feature guide](docs/项目功能手册.md) · [Friend-room synchronization](docs/0.29.19-房间模型同步.md) · [Server relay setup](docs/联机使用.md)
- [Source](src/) · [Regression tests](tests/) · [Build and maintenance](docs/维护框架.md)
- [Third-party notices](THIRD_PARTY_NOTICES.md) · [Licenses](licenses/) · [Wine Fox credits](licenses/wine-fox-source.json) · [Magical Wine Fox credits](licenses/magical-wine-fox-source.json)
- [Previous releases](https://github.com/XiaViolet/YsmBedrock/releases) · [Report an issue](https://github.com/XiaViolet/YsmBedrock/issues)

When reporting an issue, include your Minecraft, LeviLamina, and YsmBedrock versions and the steps to reproduce it.

---

## 简体中文

YsmBedrock 是 [Yes Steve Model](https://github.com/YesSteveModel/YesSteveModel) 面向 Minecraft 基岩版 Windows 客户端的非官方移植。可以穿戴自定义 YSM 模型、切换贴图，并播放自定义动画和动作。

### 环境要求

- Windows x64 和 Minecraft 基岩版 **1.26.60.05**。
- 项目使用 [LeviLamina 客户端 26.60.9](https://github.com/LiteLDev/LeviLamina/releases/tag/v26.60.9) 构建。**0.29.45** 预览版安装包支持客户端 26.60.7、26.60.8 和 26.60.9。

### 主要功能

- 导入受支持的 `.ysm`、模型 ZIP 或模型文件夹。公开安装包内置酒狐和魔法酒狐。
- 预览和穿戴模型、切换贴图、管理收藏并调整模型设置。通过动作转盘播放自定义动作。
- 获得房主许可后，可以在好友房间同步模型和外观。联机双方请使用同一版 YsmBedrock。
- 开发分支新增中英文界面，跟随 Minecraft 的语言设置。其他游戏语言下，界面默认使用英文。模型文本优先采用作者提供的翻译，常见的模型和动作标签有内置译文。部分自定义标签可能仍显示原文。
- 开发分支还新增四套菜单主题（深蓝、浅色、森林和紫罗兰）、更清晰的图标和文字、界面布局缓存，以及可调画质、最高可设为 60 FPS 的异步模型预览。

### 安装与使用

当前公开预览版为 [0.29.45](https://github.com/XiaViolet/YsmBedrock/releases/tag/v0.29.45)，详见[飞行、人偶、装备和 Arke 实测记录](docs/0.29.45.md)。

1. 在对应的 Minecraft 实例中安装 LeviLamina 客户端版，退出游戏后再导入模组。
2. 在启动器的「模组管理 → LeviLamina → 导入模组」选择对应安装包。

   | 启动器 | 安装包 |
   | --- | --- |
   | LeviLauncher | [下载 `.mcmod`](https://github.com/XiaViolet/YsmBedrock/releases/download/v0.29.45/YsmBedrock-0.29.45.mcmod) |
   | BedrockBoot | [下载 `.zip`](https://github.com/XiaViolet/YsmBedrock/releases/download/v0.29.45/YsmBedrock-0.29.45-BedrockBoot.zip) |

3. 进入世界后按 `Alt+Y` 打开模型库，选中模型后点「穿戴此模型」。
4. 按 `Z` 打开动作转盘。

### 从源码构建

安装 [xmake](https://xmake.io/)、含 C++ 和 Windows SDK 的 Visual Studio 2022 Build Tools，以及 LLVM（`clang-cl`）。在 x64 开发者终端进入项目根目录并运行以下命令。

```powershell
.\tools\build.ps1
```

该脚本会构建模组 DLL 和 `ysm-inspect.exe`。生成启动器安装包时，准备好旧版 v3 读取库 `YsmLegacyV3.dll` 和两个内置模型压缩包，再单独运行 [`tools/package.ps1`](tools/package.ps1)。

### 署名与文档

基岩版移植由 XiaViolet 维护，原始移植代码主要由 OpenAI Codex 编写。[RadiantByte](https://github.com/RadiantByte) 在 [PR #1](https://github.com/XiaViolet/YsmBedrock/pull/1) 中贡献了本地化和界面改进。

- [功能手册](docs/项目功能手册.md) · [好友房间同步](docs/0.29.19-房间模型同步.md) · [服务器中继配置](docs/联机使用.md)
- [源码](src/) · [回归测试](tests/) · [构建与维护](docs/维护框架.md)
- [第三方声明](THIRD_PARTY_NOTICES.md) · [许可文件](licenses/) · [酒狐署名](licenses/wine-fox-source.json) · [魔法酒狐署名](licenses/magical-wine-fox-source.json)
- [历史版本](https://github.com/XiaViolet/YsmBedrock/releases) · [提交问题](https://github.com/XiaViolet/YsmBedrock/issues)

反馈问题时，请附上 Minecraft、LeviLamina 和 YsmBedrock 的版本，以及复现步骤。
