"""Build the real Windows worker against a release ysm-inspect build; only transport uses a test double."""
import argparse
import _winapi
import concurrent.futures
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

repo = Path(__file__).resolve().parents[1]
arg = argparse.ArgumentParser()
arg.add_argument("--original", type=Path, help="Existing publisher-authorized, unmodified model directory or archive used by Alice")
arg.add_argument("--guest-b-original", type=Path, help="Optional unmodified authorized .zip or .ysm for the second guest")
arg.add_argument("--build-dir", type=Path, default=repo/"build", help="Existing release xmake build output, including .objs and .deps")
arg.add_argument("--output", type=Path, default=repo / "build/verification/room-0.29.19")
arg.add_argument("--scratch", type=Path, help="Optional parent directory for temporary build and caches")
options = arg.parse_args()
original = options.original or next(iter((repo.parents[1] / "素材/原始包").glob("*绘梨衣*.zip")), None)
if original is None or not (original.is_dir() or original.is_file()):
    arg.error("Set --original to an existing YSM model directory or archive")
output = options.output.resolve()
output.mkdir(parents=True, exist_ok=True)
release = options.build_dir.resolve()
recipe = release / ".deps/ysm-inspect/windows/x64/release"
objects = release / ".objs/ysm-inspect/windows/x64/release"
if not (recipe / "ysm-inspect.exe.d").is_file():
    raise SystemExit("Build ysm-inspect for Windows x64 release with xmake first.")
def args(section):
    return [a or b for a, b in re.findall(r'\[\[(.*?)\]\]|"([^"]*)"', section)]
def command(record):
    return args(record.split("values = {", 1)[1].split("        }", 1)[0])
compile_flags = command((recipe / "src/ModelPack.cpp.obj.d").read_text(encoding="utf-8"))
link_recipe = (recipe / "ysm-inspect.exe.d").read_text(encoding="utf-8")
link_flags = command(link_recipe)
old_inputs = args(link_recipe.split("files = {", 1)[1].split("    }", 1)[0])
with tempfile.TemporaryDirectory(prefix="ysm-native-room-", dir=options.scratch) as tmp:
    work = Path(tmp)
    env = {**os.environ, "TMP": tmp, "TEMP": tmp}
    # link.exe invoked directly needs the desktop CRT and Windows SDK library paths.
    # xmake's record keeps third-party libraries, while vcvars normally supplies these.
    msvc = Path(link_flags[0]).parents[3] / "lib/x64"
    sdk_root = Path(os.environ["ProgramFiles(x86)"]) / "Windows Kits/10/Lib"
    sdk = max((p for p in sdk_root.iterdir() if (p/"um/x64/bcrypt.lib").is_file()),
              key=lambda p: tuple(int(x) for x in p.name.split(".")))
    env["LIB"] = os.pathsep.join([str(msvc), str(sdk/"um/x64"), str(sdk/"ucrt/x64"), env.get("LIB", "")])
    def compile_source(name):
        obj = work / (Path(name).name + ".obj")
        subprocess.run([*compile_flags, "/nologo", "/c", name, "/Fo"+str(obj)], cwd=repo, env=env, check=True)
        return obj
    # ModelPack and ModelLibrary include the current network import budgets and extension rules.
    names = ["tests/native_room_integration.cpp", "src/NativeRoomSync.cpp", "src/RoomProtocol.cpp", "src/RoomAssets.cpp",
             "src/ModelPack.cpp", "src/ModelLibrary.cpp", "src/ModelRelay.cpp", "src/ContentHash.cpp"]
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        overrides = {p.name: p for p in pool.map(compile_source, names)}
    inputs = [objects / Path(s).parent.name / Path(s).name for s in old_inputs
              if Path(s).name != "inspect.cpp.obj" and Path(s).name not in overrides]
    exe = work / "native-room-integration.exe"
    subprocess.run([*link_flags, *inputs, *overrides.values(), "/out:"+str(exe)], cwd=repo, env=env, check=True)
    testdir = work / "isolated-clients"
    assets = {"Host": repo/"tests/fixtures/SignedFaceLab.zip", "Alice": original,
              "Bea": options.guest_b_original or repo/"tests/fixtures/MeshCoordinateLab.zip"}
    if any(p.suffix.lower()==".ysm" for p in assets.values()):
        bridge = release/"windows/x64/release/YsmLegacyV3.dll"
        if not bridge.is_file():
            bridge=repo/"build/windows/x64/release/YsmLegacyV3.dll"
        shutil.copyfile(bridge,work/bridge.name)
    lines = ["NativeRoomSync Windows offline worker integration. In-memory RoomTransport. No Bedrock client or GPU.",
             "Temporary fresh, independent model libraries and caches. Release inspector core dependencies."]
    for endpoint, asset in assets.items():
        directory = testdir / endpoint / "models"
        if endpoint == "Bea":
            managed = work / "config" / "BedrockBoot2" / "levilamina" / "ll.mods"
            managed.mkdir(parents=True)
            _winapi.CreateJunction(str(managed), str(testdir / endpoint))
            lines.append("Bea uses a launcher-managed directory junction.")
        directory.mkdir(parents=True, exist_ok=True)
        if asset.is_dir():
            shutil.copytree(asset, directory / "current")
            lines.append(f"{endpoint}: {asset.name}, original directory")
        else:
            dest = directory / ("current"+asset.suffix.lower())
            shutil.copyfile(asset, dest)
            lines.append(f"{endpoint}: {asset.name}, {dest.stat().st_size} bytes, SHA256 {hashlib.sha256(dest.read_bytes()).hexdigest()}")
    started = time.monotonic()
    run = subprocess.run([exe, testdir], cwd=repo, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
    result = run.stdout.decode("utf-8", errors="replace")
    lines.extend([result, f"Exit {run.returncode}, elapsed {time.monotonic()-started:.3f} seconds"])
    (output/"native-room-integration.txt").write_text("\n".join(lines)+"\n", encoding="utf-8")
    print(result, end="")
    raise SystemExit(run.returncode)
