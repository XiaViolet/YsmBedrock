"""Compare two ysm-inspect releases on identical unmodified player packages."""
import argparse
import hashlib
import json
import math
import statistics
import subprocess
from pathlib import Path


def digest(data):
    return hashlib.sha256(data).hexdigest()


def hash_source(path):
    if path.is_file():
        return digest(path.read_bytes())
    # Include relative names, lengths and content digests in deterministic order.
    files = sorted((file for file in path.rglob("*") if file.is_file()),
                   key=lambda file: file.relative_to(path).as_posix())
    entries = [(file.relative_to(path).as_posix(), file.stat().st_size,
                digest(file.read_bytes())) for file in files]
    return digest(json.dumps(entries, ensure_ascii=False).encode())


def frame(index):
    value = {"time": round(index / 60, 8)}
    if index % 96 >= 32:
        value["state"] = "walk"
    if index % 96 >= 64:
        value.update(items=[{"id": "minecraft:bow", "use_action": "bow"}], using_hand=0)
    return value


def replay(exe, pack, frames, **options):
    payload = json.dumps(dict(frames=frames, **options), separators=(",", ":"))
    result = subprocess.run([exe, "--player-animations", pack, payload],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    return json.loads(result.stdout)


def summary(us):
    values = sorted(us)
    return {"samples": len(values), "median_ms": statistics.median(values) / 1000,
            "p95_ms": values[math.ceil(0.95 * len(values)) - 1] / 1000}


def semantic_check(exe, pack):
    # Short replay spans transitions and read-only sampling between logical ticks.
    frames = [frame(index) for index in (0, 1, 31, 32, 33, 63, 64, 65, 95, 96)]
    frames.insert(5, {"time": 0.56, "observe": True})
    data = replay(exe, pack, frames)
    keys = data["frames"][0].keys()
    def signatures(key):
        return [digest(json.dumps(row[key], sort_keys=True, ensure_ascii=False,
                                  separators=(",", ":")).encode()) for row in data["frames"]]
    diagnostics = {key: data[key] for key in ("expression_failures", "evaluation_diagnostics", "compile_diagnostics")}
    return {key: signatures(key) for key in keys}, diagnostics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("old", type=Path)
    parser.add_argument("new", type=Path)
    parser.add_argument("packs", type=Path, nargs="+")
    parser.add_argument("--rounds", type=int, default=4)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.rounds < 2:
        parser.error("Use at least two rounds for counterbalanced execution order.")
    builds = {"old": args.old.resolve(), "new": args.new.resolve()}
    frames = [frame(index) for index in range(176)]
    report = {"binary_sha256": {label: digest(exe.read_bytes()) for label, exe in builds.items()},
              "warmup_frames": 32, "frames_per_run": len(frames), "rounds": args.rounds,
              "scope": "inspect player animation CPU including host input binding; excludes GPU, UI and pack load",
              "packs": {}}
    for path in args.packs:
        pack = path.resolve()
        raw = {label: [] for label in builds}
        print(pack.name, flush=True)
        for run in range(args.rounds):
            for label in (("old", "new") if run % 2 == 0 else ("new", "old")):
                rows = replay(builds[label], pack, frames, timing_only=True)["frames"]
                raw[label].append([row["sample_us"] for row in rows[32:]])
        samples = {label: summary([n for run in runs for n in run]) for label, runs in raw.items()}
        before, old_diagnostics = semantic_check(builds["old"], pack)
        after, new_diagnostics = semantic_check(builds["new"], pack)
        if before.keys() != after.keys() or len(before["pose"]) != len(after["pose"]):
            raise ValueError(f"Replay changed shape for {pack.name}")
        mismatches = {key: [i for i, (a, b) in enumerate(zip(before[key], after[key])) if a != b]
                      for key in before if before[key] != after[key]}
        forms = replay(builds["new"], pack, frames[:48], timing_only=True, profile_forms=True)["frames"][16:]
        result = {"source_sha256": hash_source(pack), "timing": samples, "sample_us_runs": raw,
                  "semantic_frames": len(after["pose"]), "mismatched_fields_and_frames": mismatches,
                  "diagnostics_equal": old_diagnostics == new_diagnostics,
                  "diagnostics": {"old": old_diagnostics, "new": new_diagnostics},
                  "previous_menu_form_count": forms[-1]["form_count"],
                  "isolated_menu_form_observation": summary([r["form_observation_us"] for r in forms])}
        report["packs"][pack.name] = result
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print(samples, "semantic differences", mismatches, flush=True)


if __name__ == "__main__":
    main()
