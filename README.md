# YsmBedrock — English localization

YsmBedrock is an unofficial Windows client port of [Yes Steve Model](https://github.com/YesSteveModel/YesSteveModel), maintained upstream by XiaViolet. It supports custom YSM models, textures, emotes, model settings, and approved room model synchronization.

This localized source is based on YsmBedrock 0.29.44. It uses the language selected in Minecraft: English and Chinese have built-in UI strings; other game languages use the English interface. Imported models use their author-provided language files first, with English and Chinese fallback entries. Common Chinese model labels are translated offline; emotes without known translations use readable animation IDs or numbered emote labels. Author names, animation IDs, MoLang expressions, radio indexes, texture file names and network identifiers are not rewritten. For arbitrary model text, the author-provided English translation is still required for exact wording.

## Requirements

- Windows x64
- Minecraft Bedrock 1.26.60.05
- LeviLamina client 26.60.9 (upstream also lists 26.60.7 and 26.60.8)
- Visual Studio 2022 Build Tools with Desktop development with C++, Windows SDK, LLVM/clang-cl, Git, and xmake for compiling

## Build and package

Open an x64 Native Tools Command Prompt for Visual Studio 2022 or Developer PowerShell in this directory. Make sure `clang-cl` and `xmake` are available on PATH.

```powershell
xmake f -p windows -a x64 -m release -y
xmake build -y YsmBedrock
```

The expected DLL is `build/windows/x64/release/YsmBedrock.dll`. To build the inspector as well, run `xmake build -y ysm-inspect` or `powershell -ExecutionPolicy Bypass -File .\tools\build.ps1`.

Create the launcher archives using:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\package-localized.ps1
```

For legacy v3 `.ysm` support and the original optional bundled models, supply the original upstream `.mcmod` (available on the upstream [0.29.44 release page](https://github.com/XiaViolet/YsmBedrock/releases/tag/v0.29.44)):

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\package-localized.ps1 -BaseMod 'C:\Downloads\YsmBedrock-0.29.44.mcmod'
```

The script uses the newly built YsmBedrock.dll, not the original main DLL. It can reuse the unchanged YsmLegacyV3.dll and checksum-verified model assets from the original package. Alternatively, the upstream `tools/build-legacy-v3.ps1` builds the optional reader with a separately prepared YesSteveModel-Native checkout and Conan dependencies. Neither the optional binary nor the bundled model archives is present in the supplied source ZIP.

The output directory is `dist`. Import `YsmBedrock-0.29.44-localized.mcmod` in LeviLauncher or `YsmBedrock-0.29.44-localized-BedrockBoot.zip` in BedrockBoot. Remove or replace the original version rather than installing both at once. The launcher should be closed while replacing a loaded mod.

## Usage

- Set Minecraft's language in Settings > Language.
- Press Alt+Y in a world to open the model library.
- Import a ZIP, supported YSM file, or model source folder, select the model and click Equip Model.
- Press Z for the emote wheel.
- For a model-specific English translation, add or update `lang/en_us.json` in that model (or the directory defined by `files.language_path`). The JSON uses keys such as `properties.extra_animation.extra0`, `properties.extra_animation.extra0.desc`, `metadata.name`, `files.player.texture.skin`, and `properties.extra_animation_buttons.appearance.config_forms.0.title`. Keep internal IDs and programs unchanged.

## Verification

The portable localization regression can be built with `xmake build ysm-localization-test` and run using `xmake run ysm-localization-test`. The Windows UI integration tests can be run after building `ysm-inspect` using `py -3 -m unittest discover -s tests -p test_ysm_ui.py -v`. Some upstream tests require private model fixtures that are not included in the source archive.

The original project documentation, third-party notices and license files are retained. Original release notes, source attribution and project resources: [upstream YsmBedrock](https://github.com/XiaViolet/YsmBedrock), [third-party notices](THIRD_PARTY_NOTICES.md), [licenses](licenses/), [feature guide](docs/项目功能手册.md), [maintenance guide](docs/维护框架.md). This localization is not an official upstream release.
