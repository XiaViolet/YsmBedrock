# YsmBedrock

YsmBedrock 是 [Yes Steve Model](https://github.com/YesSteveModel/YesSteveModel) 面向 Minecraft 基岩版 Windows 客户端的非官方移植。基岩版移植代码主要由 OpenAI Codex 编写，项目由 XiaViolet 维护。

本仓库收录 **0.29.20 内测版源码**，适配 Minecraft 1.26.60.05 和 LeviLamina 26.60.7／26.60.8。受邀测试者可以在 [Issues](https://github.com/XiaViolet/YsmBedrock/issues) 提交问题，并注明游戏、加载器及模组版本和复现步骤。

内测安装包由维护者单独提供。完整打包所需的 Wine Fox（酒狐）模型来源和作者署名见 [模型记录](licenses/wine-fox-source.json)。本仓库中的上游来源与许可见 [第三方声明](THIRD_PARTY_NOTICES.md)。

房间模型同步的默认规则、缓存、限速和实测范围见[原版房间模型同步](docs/0.29.19-房间模型同步.md)。

## 安装和使用

当前功能与实测范围见[Java 功能对照](docs/Java功能对照.md)和[0.29.5 验收](docs/0.29.5-跨端功能验收.md)。安装后的常用操作见[项目功能手册](docs/项目功能手册.md)。

1. 首次安装时，在 LeviLauncher 选中对应的 Windows 实例，在模组管理中导入 `YsmBedrock-0.29.20.mcmod`。升级已有实例时，先退出游戏，解压工程包，在解压目录运行 `.\install.ps1 -Instance "实例目录"`。安装脚本保留已有模型与设置。新装只内置 Java YSM 2.6.5 官方基础款 **Wine Fox（酒狐）**。
2. 按 Alt+Y 打开模型库，点「导入模型」，选择原始 ZIP、受支持的 .ysm 文件或源文件夹。点模型卡片试看，在「动作预览」里播放作者动作，点「穿戴此模型」后才应用到玩家。点「打开模型文件夹」可在资源管理器中查看当前实例的模型库。菜单默认通过 Windows 显卡的独立深度缓冲绘制模型，再由游戏合成预览图，可在「本地设置 → 原生网格预览」切回位图预览。
3. 在游戏内按 Z 打开动作转盘，按 1–8 选择。滚轮或 PgUp／PgDn 翻页，Esc 返回。装饰可在右侧连续调整，左侧纸娃娃显示当前穿戴模型的实时姿态。普通动画选中即播放并关窗，同时带动作和设置的扇区可单点「设置」。在「本地设置 → 纸娃娃帧率」可调 5–30 帧/秒，默认为 30。

| 按键 | 功能 |
| --- | --- |
| Alt+Y | 模型库、贴图、动作预览、作者设置 |
| Z | 打开动作转盘，选择装饰或播放动作 |

模型库支持搜索、收藏与作者配置。选择和各模型的贴图单独保存。旧式目录和 spec 2 源包适合当前游戏验证。二进制 `.ysm` 支持读取 v1–v3，0.29.5 已在白色大肥鱼和授权版临战爱丽丝的原始 v3 包上验证了修正后的多边形贴图。联机方式见工程中的 `docs/联机使用.md`，发行包中的对应路径为 `source/docs/联机使用.md`。

发布内容与已验证范围见[首发说明](docs/0.29.20-首发说明.md)。

## 构建和检查

接手开发先看[维护框架](docs/维护框架.md)，其中列出了各功能的源码入口、回归分组和 Java 对照验收方法。

部分回归使用绘梨衣原包，可用环境变量 `YSM_TEST_MODEL` 指向自己持有的对应原始 ZIP。内置酒狐的出处与许可见[模型记录](licenses/wine-fox-source.json)。

工程配置使用 xmake、clang-cl 及 LeviLamina 客户端 SDK。`YsmBedrock` 编译游戏绑定，`ysm-inspect` 提供离线检查，共用同一份动画、模型和界面代码。

| 工具 | 用途 |
| --- | --- |
| `tools/build.ps1 -Test` | 编译并执行 Python 回归检查 |
| `py -3 tools/check_feature.py --list` | 列出按功能组合运行的回归分组 |
| `tools/build-legacy-v3.ps1` | 从已配置依赖的官方 Native 工程构建 v3 读取库 |
| `tools/install.ps1 -Instance <目录>` | 游戏退出后安装模组，并复制已构建的 v3 读取库 |
| `tools/package.ps1` | 创建当前版本的预览 ZIP |
| `tools/audit_model_animations.py` | 批量加载并回放目录、ZIP 或 `.ysm` 模型 |

完整回归使用绘梨衣原始样包与固定版上游素材。当前验证范围见 `docs/进展.md`，发行包中的对应路径为 `source/docs/进展.md`。第三方来源和许可见 `THIRD_PARTY_NOTICES.md`。

原版粒子的覆盖和实体替换的诊断字段见 [Java 粒子与实体材质](docs/Java粒子与实体材质.md)。

[Java 功能对照](docs/Java功能对照.md) · [0.29.3 替换实体世界输入验收](docs/替换实体世界输入验收.md) · [0.29.2 动画时长与分组验收](docs/动画时长与分组验收.md)
