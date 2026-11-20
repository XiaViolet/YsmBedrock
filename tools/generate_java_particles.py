"""Read the actual particle registration calls in the project's pinned client."""
import hashlib
import json
import pathlib
import re
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
CLIENT = ROOT / 'vendor/minecraft-1.20.1'
source = json.loads((CLIENT / 'source.json').read_text(encoding='utf-8-sig'))
jar = CLIENT / 'client.jar'
mappings = CLIENT / 'client-mappings.txt'
for path, key in ((jar, 'client'), (mappings, 'client_mappings')):
    if hashlib.sha1(path.read_bytes()).hexdigest() != source['downloads'][key]['sha1']:
        raise SystemExit(f'{path.name} does not match the pinned client SHA-1')
toolchain = json.loads((ROOT / 'build/verification/java-reference-toolchain.json').read_text(encoding='utf-8-sig'))
obfuscated = re.search(r'^net\.minecraft\.core\.particles\.ParticleTypes -> (\w+):$',
                       mappings.read_text(encoding='utf-8'), re.M)[1]
bytecode = subprocess.check_output([str(pathlib.Path(toolchain['java']).with_name('javap.exe')),
                                    '-classpath', str(jar), '-c', '-p', obfuscated], encoding='utf-8', timeout=20)
entries = {}
name = None
for line in bytecode.split('static {};', 1)[1].splitlines():
    literal = re.search(r'// String ([a-z0-9_]+)$', line)
    if literal:
        name = literal[1]
    call = re.search(r'// Method \w+:\(Ljava/lang/String;Z([^)]*)\)', line)
    if call and name:
        if name in entries:
            raise SystemExit(f'Duplicate registration: {name}')
        entries[name] = bool(call[1])
        name = None
if len(entries) != 95:
    raise SystemExit(f'Expected 95 pinned registrations, found {len(entries)}')
fixture = {'minecraft': source['version'], 'client_sha1': source['downloads']['client']['sha1'],
           'particles': entries}
(ROOT / 'tests/fixtures/java_particle_registry_1_20_1.json').write_text(
    json.dumps(fixture, indent=2) + '\n', encoding='utf-8')
