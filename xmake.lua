add_rules("mode.debug", "mode.release")
add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")
set_project("YsmBedrock")
set_version("0.29.42")
set_languages("c++20")
set_runtimes("MD")
set_toolchains("clang-cl")
includes("packages/*.lua")
add_requires("levilamina 26.60.7", {configs = {target_type = "client"}})
add_requires("levibuildscript 0.6.1")
add_requires("miniz 3.1.0")
add_requires("libjpeg-turbo 3.1.2", {configs = {shared = false}})
add_requires("libwebp 1.6.0", {configs = {shared = false}})
add_requires("libavif 1.4.2", {configs = {shared = false}})
add_requires("libopusfile 0.12+20260328")

-- Both entry points use the same model, animation and UI logic. Only the
-- client target compiles Bedrock-specific hooks and resource bindings.
local core_sources = {
    "src/ModelPack.cpp", "src/ModelLibrary.cpp", "src/ModelTargets.cpp",
    "src/ContentHash.cpp", "src/ImageCodec.cpp", "src/LegacyBundle.cpp",
    "src/ClientGeometry.cpp", "src/BoneVisual.cpp", "src/ModelPreview.cpp", "src/PreviewDepthRenderer.cpp",
    "src/AnimationSampler.cpp", "src/AnimationControllers.cpp",
    "src/CodedAnimationPlayer.cpp", "src/DefaultAnimations.cpp",
    "src/HandAnimationController.cpp", "src/EntityQueries.cpp", "src/JavaVanillaInputs.cpp",
    "src/Molang.cpp", "src/MolangPhysics.cpp", "src/MolangRoaming.cpp", "src/MolangRoamingSession.cpp",
    "src/ModelAudio.cpp", "src/ModelParticles.cpp", "src/ModelRelay.cpp", "src/RoomAssets.cpp", "src/RoomProtocol.cpp", "src/ReplacedModelAssignments.cpp",
    "src/YsmUi.cpp", "src/YsmUiLayout.cpp", "src/UiMesh.cpp", "src/UiTextAtlas.cpp"
}

local function configure_shared_sources()
    add_packages("levilamina", "miniz", "libjpeg-turbo", "libwebp", "libavif", "libopusfile")
    add_syslinks("bcrypt", "user32", "gdi32", "winhttp", "crypt32", "d3d11", "d3dcompiler")
    add_defines("NOMINMAX", "UNICODE", "_HAS_CXX23=1")
    add_cxflags("/utf-8", "/W4", "/EHsc")
    add_includedirs("src")
    add_files(table.unpack(core_sources))
end

target("YsmBedrock")
    set_kind("shared")
    add_shflags("/MAP", {force = true})
    add_rules("@levibuildscript/linkrule")
    configure_shared_sources()
    add_syslinks("shell32", "ole32", "uuid", "psapi")
    add_files("src/Native*.cpp", "src/AnimationRuntime.cpp", "src/SkinRuntime.cpp",
        "src/MemoryOperators.cpp", "src/Mod.cpp")

target("ysm-inspect")
    set_kind("binary")
    configure_shared_sources()
    add_files("tools/inspect.cpp")

target("ysm-menu-touch-test")
    set_default(false)
    set_kind("binary")
    configure_shared_sources()
    add_files("tests/windows_menu_touch.cpp")
