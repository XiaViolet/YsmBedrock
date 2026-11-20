"""Run the compiled C++ importer against the supplied package and damaged variants."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest
import zipfile

PROJECT = pathlib.Path(__file__).resolve().parents[1]
WORKSPACE = PROJECT.parents[1]
EXE = PROJECT / 'build/windows/x64/release/ysm-inspect.exe'
if not EXE.exists():
    EXE = PROJECT.parent / 'ysm-inspect.exe'
sample_file = os.environ.get('YSM_TEST_MODEL')
SAMPLE = pathlib.Path(sample_file) if sample_file else next((WORKSPACE / '素材/原始包').glob('*上杉绘梨衣*.zip'), None)
if SAMPLE is None:
    exported_models = PROJECT.parent / 'mods/YsmBedrock/models'
    SAMPLE = next(exported_models.glob('*上杉绘梨衣*.zip'), exported_models / 'eri.zip')


class ImportTests(unittest.TestCase):
    def invoke(self, path):
        return subprocess.run([str(EXE), str(path)], capture_output=True, encoding='utf-8')

    def pose(self, state, time):
        result = subprocess.run([str(EXE), str(SAMPLE), state, str(time)], capture_output=True, encoding='utf-8')
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)['pose']

    def test_walk_uses_original_opposite_stride_keyframes(self):
        start = self.pose('walk', 0)
        half = self.pose('walk', 0.5)
        self.assertAlmostEqual(start['LeftLeg']['rotation'][0], -27.41587)
        self.assertAlmostEqual(half['LeftLeg']['rotation'][0], 22.47285)

    def test_animation_loops(self):
        first = self.pose('walk', 0.25)
        repeated = self.pose('walk', 1.25)
        self.assertEqual(first['LeftLeg'], repeated['LeftLeg'])

    def test_idle_and_default_hidden_props(self):
        idle = self.pose('idle', 0.5417)
        self.assertAlmostEqual(idle['UpBody']['position'][1], 0.08201)
        self.assertEqual(idle['MiaoJian']['scale'], [0, 0, 0])
        self.assertEqual(idle['MHead']['scale'], [0.95, 0.95, 0.95])

    def test_unknown_animation_keeps_parallel_layers_without_substituting_idle(self):
        missing = self.pose('missing_state', 0.2)
        self.assertNotIn('LeftLeg', missing)
        self.assertIn('Breast', missing)

    def test_real_zip_and_folder_are_equivalent(self):
        archive_result = self.invoke(SAMPLE)
        self.assertEqual(archive_result.returncode, 0, archive_result.stderr)
        report = json.loads(archive_result.stdout)
        self.assertEqual(report['main_bones'], 177)
        self.assertEqual(report['main_cubes'], 1512)
        self.assertEqual(report['animation_clips'], 242)
        with tempfile.TemporaryDirectory() as folder:
            with zipfile.ZipFile(SAMPLE) as archive:
                archive.extractall(folder)
            folder_result = self.invoke(folder)
            self.assertEqual(folder_result.returncode, 0, folder_result.stderr)
            self.assertEqual(json.loads(folder_result.stdout), report)

    def test_render_scaling_uses_one_parent_for_detached_props(self):
        result = subprocess.run([str(EXE), '--render-geometry', str(SAMPLE)], capture_output=True, encoding='utf-8')
        self.assertEqual(result.returncode, 0, result.stderr)
        bones = json.loads(result.stdout)['minecraft:geometry'][0]['bones']
        self.assertEqual(len(bones), 178)
        roots = [b for b in bones if 'parent' not in b]
        self.assertEqual(len(roots), 1)
        root = roots[0]['name']
        lookup = {b['name']: b for b in bones}
        for name in ['root', 'game', 'book']:
            self.assertEqual(lookup[name]['parent'], root)
        self.assertEqual(lookup['Head']['parent'], 'MHead')
        with zipfile.ZipFile(SAMPLE) as archive:
            original = json.loads(archive.read('Sahngshan_huiliyi/models/main.json'))
        self.assertEqual(len(original['minecraft:geometry'][0]['bones']), 177)

    def mutated(self, transform, expected):
        with zipfile.ZipFile(SAMPLE) as source:
            files = {item.filename: source.read(item) for item in source.infolist() if not item.is_dir()}
        transform(files)
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / '模型测试.zip'
            with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as archive:
                for name, content in files.items():
                    archive.writestr(name, content)
            result = self.invoke(path)
            self.assertEqual(result.returncode, 1)
            self.assertIn(expected, result.stderr)

    def edit_json(self, files, suffix, change):
        path = next(name for name in files if name.endswith(suffix))
        data = json.loads(files[path].decode('utf-8-sig'))
        change(data)
        files[path] = json.dumps(data, ensure_ascii=False).encode('utf-8')

    def test_source_group_precedence_and_first_person_isolation(self):
        with zipfile.ZipFile(SAMPLE) as source:
            files = {item.filename: source.read(item) for item in source.infolist() if not item.is_dir()}
        manifest = next(name for name in files if name.endswith('/ysm.json'))
        prefix = manifest.removesuffix('ysm.json')
        groups = [('extra', 3), ('arm', 2), ('main', 1), ('fp_arm', 7), ('future_group', 8)]
        for group, marker in groups:
            clips = {'idle': {'animation_length': marker}}
            if group == 'extra':
                clips['extra1'] = {'animation_length': 6}
            if group == 'future_group':
                clips['future_only'] = {'animation_length': marker}
            files[prefix + f'{group}.json'] = json.dumps({'animations': clips}).encode()

        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'animation-groups.zip'
            for order in (groups, list(reversed(groups))):
                self.edit_json(files, '/ysm.json', lambda doc: doc['files']['player'].update(
                    animation={group: f'{group}.json' for group, _ in order}))
                with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as archive:
                    for name, content in files.items():
                        archive.writestr(name, content)
                result = subprocess.run([str(EXE), '--animation-sets', str(path)],
                                        capture_output=True, encoding='utf-8')
                self.assertEqual(result.returncode, 0, result.stderr)
                documents = json.loads(result.stdout)
                body = documents['body']['animations']
                self.assertEqual(body['idle']['animation_length'], 2)
                self.assertEqual(body['extra1']['animation_length'], 6)
                self.assertNotIn('future_only', body)
                self.assertEqual(documents['first_person']['animations']['idle']['animation_length'], 7)

    def test_rejects_missing_model(self):
        self.mutated(lambda f: f.pop('Sahngshan_huiliyi/models/main.json'), 'Missing resource')

    def test_rejects_traversal_reference(self):
        self.mutated(lambda f: self.edit_json(f, '/ysm.json', lambda j: j['files']['player']['model'].update(main='../outside.json')), 'escapes')

    def test_unknown_animation_group_reference_must_stay_in_model(self):
        self.mutated(lambda files: self.edit_json(files, '/ysm.json', lambda j:
            j['files']['player']['animation'].update(future_group='../outside.json')), 'escapes')

    def test_rejects_unsupported_spec(self):
        self.mutated(lambda f: self.edit_json(f, '/ysm.json', lambda j: j.update(spec=99)), 'spec 2')

    def test_rejects_invalid_render_scale(self):
        self.mutated(lambda f: self.edit_json(f, '/ysm.json', lambda j: j['properties'].update(height_scale='wrong')), 'model scale')

    def test_rejects_negative_render_scale(self):
        self.mutated(lambda f: self.edit_json(f, '/ysm.json', lambda j: j['properties'].update(width_scale=-1)), 'model scale')

    def test_rejects_duplicate_bones(self):
        def change(j):
            bones = j['minecraft:geometry'][0]['bones']
            bones.append(bones[0])
        self.mutated(lambda f: self.edit_json(f, '/models/main.json', change), 'Duplicate')

    def test_rejects_bone_cycle(self):
        def change(j):
            bone = j['minecraft:geometry'][0]['bones'][0]
            bone['parent'] = bone['name']
        self.mutated(lambda f: self.edit_json(f, '/models/main.json', change), 'cycle')

    def test_rejects_missing_parent(self):
        def change(j):
            j['minecraft:geometry'][0]['bones'][0]['parent'] = 'does_not_exist'
        self.mutated(lambda f: self.edit_json(f, '/models/main.json', change), 'Missing parent')

    def test_rejects_invalid_png(self):
        self.mutated(lambda f: f.update({'Sahngshan_huiliyi/textures/default.png': b'not an image'}), 'invalid texture format')

    def test_rejects_ambiguous_packages(self):
        self.mutated(lambda f: f.update({'another/ysm.json': b'{}'}), 'exactly one')

    def test_rejects_traversal_archive_root(self):
        def change(files):
            for name, content in list(files.items()):
                files['../' + name] = content
                del files[name]
        self.mutated(change, 'escapes')

    def test_rejects_non_zip(self):
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder) / 'fake.zip'
            path.write_bytes(b'not a zip')
            result = self.invoke(path)
            self.assertEqual(result.returncode, 1)
            self.assertIn('Invalid ZIP', result.stderr)

    def test_source_controller_files_are_loaded_without_changing_original_package(self):
        original = SAMPLE.read_bytes()
        with zipfile.ZipFile(SAMPLE) as archive:
            files = {name: archive.read(name) for name in archive.namelist() if not name.endswith('/')}
        manifest_path = next(name for name in files if name.endswith('/ysm.json'))
        prefix = manifest_path[:-8]
        manifest = json.loads(files[manifest_path].decode('utf-8-sig'))
        manifest['files']['player']['animation_controllers'] = ['controller/main.json']
        files[manifest_path] = json.dumps(manifest).encode('utf-8')
        files[prefix + 'controller/main.json'] = json.dumps({'animation_controllers': {
            'player.post_main_probe': {'states': {'default': {'animations': ['idle']}}}}}).encode('utf-8')
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder) / 'with-controller.zip'
            with zipfile.ZipFile(path, 'w') as archive:
                for name, data in files.items():
                    archive.writestr(name, data)
            result = self.invoke(path)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)['animation_controllers'], 1)
            report = subprocess.run([str(EXE), '--player-animations', str(path), json.dumps({'frames': [{'time': 0}]})],
                                    capture_output=True, encoding='utf-8', timeout=5)
            self.assertEqual(report.returncode, 0, report.stderr)
            data = json.loads(report.stdout)
            self.assertIn('player.post_main_probe', data['frames'][0]['controllers']['states'])
        self.assertEqual(SAMPLE.read_bytes(), original)

    def test_rejects_controller_path_outside_source_package(self):
        self.mutated(lambda f: self.edit_json(f, '/ysm.json', lambda j:
                     j['files']['player'].update(animation_controllers=['../controller.json'])), 'escapes')

    def test_rejects_duplicate_controller_names_across_files(self):
        def change(files):
            self.edit_json(files, '/ysm.json', lambda j:
                           j['files']['player'].update(animation_controllers=['controller/a.json', 'controller/b.json']))
            body = json.dumps({'animation_controllers': {'player.main': {'states': {'default': {}}}}}).encode('utf-8')
            files['Sahngshan_huiliyi/controller/a.json'] = body
            files['Sahngshan_huiliyi/controller/b.json'] = body
        self.mutated(change, 'Duplicate animation controller')

    def test_source_origin_version_reaches_hand_playback_without_changing_shared_animation(self):
        original = SAMPLE.read_bytes()
        with zipfile.ZipFile(SAMPLE) as archive:
            files = {name: archive.read(name) for name in archive.namelist() if not name.endswith('/')}
        manifest_path = next(name for name in files if name.endswith('/ysm.json'))
        prefix = manifest_path[:-8]
        source_manifest = json.loads(files[manifest_path].decode('utf-8-sig'))
        arm_path = prefix + source_manifest['files']['player']['animation']['arm']
        arm = json.loads(files[arm_path].decode('utf-8-sig'))
        arm['animations']['hold_mainhand:sword'] = {'animation_length': 1,
            'bones': {'port_origin_probe': {'position': ['q.anim_time', 0, 0]}}}
        files[arm_path] = json.dumps(arm).encode('utf-8')
        inputs = {'frames': [{'time': i / 20, 'items': [{'id': 'minecraft:wooden_sword', 'category': 'sword'}]}
                             for i in range(36)]}
        for version, authored in [('2.4.2-snapshot-31', False), ('2.4.2-snapshot-32', True)]:
            with self.subTest(origin=version), tempfile.TemporaryDirectory() as folder:
                manifest = dict(source_manifest, properties=dict(source_manifest['properties'], originVer=version))
                files[manifest_path] = json.dumps(manifest).encode('utf-8')
                path = pathlib.Path(folder) / 'origin.zip'
                with zipfile.ZipFile(path, 'w') as archive:
                    for name, data in files.items():
                        archive.writestr(name, data)
                result = subprocess.run([str(EXE), '--player-animations', str(path), json.dumps(inputs)],
                                        capture_output=True, encoding='utf-8', timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)
                report = json.loads(result.stdout)
                self.assertEqual(report['expression_failures'], 0, report['evaluation_diagnostics'])
                final = report['frames'][-1]['pose']
                if authored:
                    self.assertNotIn('port_origin_probe', final)
                else:
                    self.assertAlmostEqual(final['port_origin_probe']['position'][0], 0.65)
        self.assertEqual(SAMPLE.read_bytes(), original)


if __name__ == '__main__':
    unittest.main(verbosity=2)
