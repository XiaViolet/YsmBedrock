"""Inventory and replay model packages with the same loader used by the inspector."""

import argparse
import concurrent.futures
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / 'build/windows/x64/release/ysm-inspect.exe'
BUILTINS = ROOT / 'vendor/ysm-upstream/src/main/resources/assets/ysm/builtin'
STATES = (
    'idle', 'walk', 'run', 'idle', 'sneaking', 'sneak', 'jump', 'idle',
    'swim', 'swim_stand', 'elytra_fly', 'sit', 'ride', 'climb', 'sleep', 'death', 'idle',
)


def file_hash(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def source_ticks(seconds):
    as_float = struct.unpack('<f', struct.pack('<f', seconds))[0]
    return struct.pack('<f', as_float * 20)


def inventory(path, report):
    """Inspect authored keyframes when the package exposes its manifest."""
    with ExitStack() as resources:
        if path.is_file():
            report['sha256'] = file_hash(path)
            if path.suffix.lower() != '.zip':
                return {}
            archive = resources.enter_context(zipfile.ZipFile(path))
            manifests = [name for name in archive.namelist()
                         if name == 'ysm.json' or name.endswith('/ysm.json')]
            if len(manifests) != 1:
                return {}
            prefix = manifests[0][:-len('ysm.json')]

            def read(name):
                return json.loads(archive.read(prefix + name).decode('utf-8-sig'))
        else:
            direct = path / 'ysm.json'
            manifests = [direct] if direct.is_file() else list(path.rglob('ysm.json'))
            if len(manifests) != 1:
                return {}
            manifest = manifests[0]
            folder = manifest.parent.resolve()
            report['manifest_sha256'] = file_hash(manifest)

            def read(name):
                candidate = (folder / name).resolve()
                candidate.relative_to(folder)
                return json.loads(candidate.read_text(encoding='utf-8-sig'))

        descriptor = read('ysm.json')
        report['name'] = descriptor.get('metadata', {}).get('name', path.name)
        groups = descriptor.get('files', {}).get('player', {}).get('animation', {})
        clips = {}
        for group, filename in groups.items():
            animations = read(filename).get('animations', {})
            if group not in ('arm', 'fp_arm'):
                clips.update(animations)
            for name, clip in animations.items():
                length = clip.get('animation_length', 0)
                for field in ('timeline', 'sound_effects'):
                    for timestamp in clip.get(field, {}):
                        key_time = float(timestamp)
                        if key_time > length and source_ticks(key_time) == source_ticks(length):
                            report['terminal_precision_cases'].append({
                                'group': group, 'clip': name, 'length': length, 'event': key_time,
                                'channel': field, 'loop': clip.get('loop'),
                            })
        report['authored_body_clips'] = len(clips)
        return clips


def frame_input(time, state, age, **extra):
    return {
        'time': time, 'state': state, 'state_time': age,
        'entity': {'on_ground': state not in ('jump', 'elytra_fly'),
                   'health': 20, 'max_health': 20, 'tick': int(time * 20)},
        **extra,
    }


def replay_frames(clips):
    frames = [frame_input(index * 1.2 + age, state, age)
              for index, state in enumerate(STATES) for age in (0, .12, .3, .6, 1)]
    time = len(STATES) * 1.2
    for name, clip in clips.items():
        length = float(clip.get('animation_length', 0))
        if not name.startswith('extra') or length > 60:
            continue
        for age in (0, .2, max(.3, length + .1)):
            frames.append(frame_input(time + age, 'idle', age, extra=name, extra_time=age))
        time += max(.6, length + .3)
    # Bound the command-line size when sampling additional actions on Windows.
    return frames[:150]


def audit(path):
    report = {'source': str(path), 'name': path.name, 'terminal_precision_cases': []}
    try:
        clips = inventory(path, report)
    except (OSError, ValueError, TypeError, AttributeError, OverflowError, KeyError, zipfile.BadZipFile) as error:
        # The version-specific source inventory is supplementary. The same
        # loader as the mod decides whether the model can actually be opened.
        report['inventory_diagnostic'] = str(error)
        clips = {}

    try:
        sample = json.dumps({'frames': replay_frames(clips)}, separators=(',', ':'))
        process = subprocess.run(
            [str(EXE), '--player-animations', str(path), sample], capture_output=True, encoding='utf-8', timeout=90,
        )
        if process.returncode:
            raise ValueError(process.stderr.strip())
        output = json.loads(process.stdout)
        frames = output['frames']
        report.update(
            sampled_frames=len(frames), expression_failures=output['expression_failures'],
            compile_diagnostics=output.get('compile_diagnostics', []),
            evaluation_diagnostics=output.get('evaluation_diagnostics', []),
            controller_diagnostics=sorted({entry for frame in frames
                                           for entry in frame['controllers'].get('diagnostics', [])}),
            last_controller_states=frames[-1]['controllers'].get('states', {}),
        )
        report['passed'] = report['expression_failures'] == 0 and not report['compile_diagnostics']
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        report.update(passed=False, load_error=str(error))
    return report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--models', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--builtins', action='store_true')
    args = parser.parse_args()
    paths = sorted(p for p in args.models.iterdir() if p.is_dir() or p.suffix.lower() in ('.zip', '.ysm'))
    if args.builtins:
        paths.extend(sorted(p.parent for p in BUILTINS.rglob('ysm.json')))
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        results = list(pool.map(audit, paths))
    report = {
        'packages': results, 'package_count': len(results),
        'sampled_frames': sum(r.get('sampled_frames', 0) for r in results),
        'expression_failures': sum(r.get('expression_failures', 0) for r in results),
        'note': 'CPU/source audit. In-game visuals are outside this check.',
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({
        'packages': len(results), 'passed': sum(r['passed'] for r in results),
        'sampled_frames': report['sampled_frames'],
        'expression_failures': report['expression_failures'],
        'load_errors': [(r['source'], r['load_error']) for r in results if 'load_error' in r],
        'terminal_precision_cases': sum(len(r['terminal_precision_cases']) for r in results),
    }))


if __name__ == '__main__':
    main()
