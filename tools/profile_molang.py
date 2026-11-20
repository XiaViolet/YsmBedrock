"""Stage an instrumented, isolated copy of a release for per-clip call profiling.

python tools/profile_molang.py path/to/source new/empty/stage
Build ysm-inspect in that directory, then pass profile_script_clip, profile_script_mode
("clip", "roots", or "calls") and profile_script_warmup with a normal --player-animations replay.
"""
import argparse
from pathlib import Path
import shutil


def change(source, old, new):
    if source.count(old) != 1:
        raise ValueError(f"Profiler hook must match exactly once: {old[:95]!r}")
    return source.replace(old, new, 1)


def patch(directory):
    prefix = '#include "ScriptCallProfiler.h"\n'
    path = directory / "src/AnimationSampler.cpp"
    code = path.read_text(encoding="utf-8")
    code = change(code, "    sampled.clear();\n    MolangActionScope", '    script_profile::Clip profile(name, "bones");\n    sampled.clear();\n    MolangActionScope')
    marker = "double AnimationSampler::blendWeight(std::string const& name, double time, MolangContext& context) const {"
    code = change(code, marker, marker + '\n    script_profile::Clip profile(name, "weight");')
    marker = "        auto const& selected = active[event.layer];"
    code = change(code, marker, marker + '\n        script_profile::Clip profile(selected.name, "timeline");')
    path.write_bytes((prefix + code).encode())

    path = directory / "src/Molang.cpp"
    code = path.read_text(encoding="utf-8")
    marker = "        case MolangKind::Call: {"
    code = change(code, marker, marker + '\n            script_profile::Call profile(node.text);')
    marker = "            return context.call(node.text, arguments);"
    if marker not in code:
        # Newer builds may bind the dynamic call handler at validation time.
        candidates = [line for line in code.splitlines() if "return " in line and
                      ("context.call" in line or "dispatch" in line) and "arguments" in line]
        if len(candidates) != 1:
            raise ValueError("Locate the bound call dispatch before profiling this version")
        marker = candidates[0]
    code = change(code, marker, "            profile.beginDispatch();\n" + marker)
    path.write_bytes((prefix + code).encode())

    path = directory / "tools/inspect.cpp"
    code = path.read_text(encoding="utf-8")
    marker = '    bool timingOnly = input.value("timing_only", false);\n    for (auto const& frame : input.at("frames")) {'
    code = change(code, marker, '''    auto& profile = ysm::script_profile::session;
    profile = {};
    profile.clip = input.value("profile_script_clip", "");
    profile.mode = input.value("profile_script_mode", "clip");
    profile.warmup = input.value("profile_script_warmup", 0u);
''' + marker + "\n        ++profile.frames;")
    begin = code.index(marker)
    marker = '    return {{"frames", frames}, {"expression_failures", evaluation.expressions.failures},'
    end = code.index(marker, begin)
    code = code[:end] + code[end:].replace(marker, '    return {{"script_profile",profile.report()}, {"frames", frames}, {"expression_failures", evaluation.expressions.failures},',1)
    path.write_bytes((prefix + code).encode())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("stage", type=Path)
    args = parser.parse_args()
    source, stage = args.source.resolve(), args.stage.resolve()
    if source == stage or source in stage.parents or stage in source.parents:
        parser.error("The profiling tree must be independent of the source tree")
    if stage.exists():
        parser.error("Choose a new staging directory")
    stage.mkdir(parents=True)
    for folder in ("src", "tools", "packages"):
        shutil.copytree(source / folder, stage / folder, ignore=shutil.ignore_patterns("__pycache__"))
    shutil.copy2(source / "xmake.lua", stage)
    shutil.copy2(Path(__file__).with_name("ScriptCallProfiler.h"), stage / "src")
    patch(stage)


if __name__ == "__main__":
    main()
