"""Exercise real package selection and preference I/O through the compiled client core."""
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest
import zipfile

from test_import import EXE, SAMPLE
from test_model_textures import png


class ModelLibraryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.models = self.root / 'models'
        self.models.mkdir()
        self.selection = self.root / 'selection.json'

    def source(self, name, folder=False, nested=False):
        geometry = {'minecraft:geometry': [{'description': {'texture_width': 2, 'texture_height': 2}, 'bones': [{'name': 'root'}]}]}
        manifest = {'spec': 2, 'metadata': {'name': name}, 'properties': {'default_texture': 'red'},
                    'files': {'player': {'model': {'main': 'main.json', 'arm': 'arm.json'},
                                         'animation': {'main': 'animation.json'},
                                         'texture': ['textures/red.png', 'textures/blue.png']}}}
        files = {'ysm.json': json.dumps(manifest).encode(), 'main.json': json.dumps(geometry).encode(),
                 'arm.json': json.dumps(geometry).encode(), 'animation.json': b'{"animations":{"idle":{"loop":true}}}',
                 'textures/red.png': png(), 'textures/blue.png': png(rgba=(20, 40, 220, 255))}
        path = self.models / name
        if folder:
            for key, data in files.items():
                target = path / ('nested' if nested else '') / key
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
        else:
            with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as archive:
                for key, data in files.items():
                    archive.writestr(('nested/' if nested else '') + key, data)
        return path

    def preferences(self, value):
        self.selection.write_text(json.dumps(value, ensure_ascii=False), encoding='utf-8-sig')

    def run_library(self, actions=None):
        result = subprocess.run([str(EXE), '--model-library', str(self.models), str(self.selection),
                                 json.dumps({'actions': actions or ['initial']})],
                                capture_output=True, encoding='utf-8', timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)['frames']

    def import_model(self, source, check=True):
        result = subprocess.run([EXE, '--import-model', source, self.models],
                                encoding='utf-8', capture_output=True, timeout=20)
        if check:
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)['source']
        return result

    def test_refresh_from_empty_selection_discovers_models_without_wearing_them(self):
        refresh = {'action': 'browse', 'source': ''}
        self.assertEqual(self.run_library([refresh])[0]['library']['catalog'], [])
        model = self.source('第一次使用.zip')
        card = {'action': 'browse', 'source': model.name}
        refreshed, previewed = self.run_library([refresh, card])
        self.assertEqual([row['source'] for row in refreshed['library']['catalog']], [model.name])
        self.assertFalse(refreshed['attempt']['prepared'])
        self.assertEqual(previewed['attempt'], {'prepared': True, 'source': model.name})
        self.assertIsNone(previewed['model'])
        self.assertEqual(previewed['library']['selected_source'], '')
        self.assertFalse(self.selection.exists())
        worn, _, restarted = self.run_library([{'action': 'select', 'source': model.name}, 'save', 'restart'])
        for frame in (worn, restarted):
            self.assertEqual(frame['library']['selected_source'], model.name)

    def test_import_publication_keeps_source_bytes_and_equipped_selection(self):
        original = self.source('穿戴中.zip')
        self.preferences({'source': original.name})
        before = self.selection.read_bytes()
        external = self.root / '服装.zip'
        external.write_bytes(original.read_bytes())
        self.assertEqual(self.import_model(external), external.name)
        self.assertEqual(self.import_model(external), '服装 (2).zip')
        self.assertEqual(self.import_model(self.models / external.name), external.name)
        for name in ['服装.zip', '服装 (2).zip']:
            self.assertEqual((self.models / name).read_bytes(), external.read_bytes())
        self.assertEqual(self.selection.read_bytes(), before)

    def test_import_source_folder_and_reject_incomplete_or_recursive_input(self):
        source = self.source('模型文件夹', folder=True, nested=True).rename(self.root / '外部原包')
        self.assertEqual(self.import_model(source), source.name)
        self.assertEqual(self.run_library()[0]['library']['selected_source'], source.name)
        self.assertEqual((self.models / source.name / 'nested' / 'ysm.json').read_bytes(),
                         (source / 'nested' / 'ysm.json').read_bytes())
        invalid = self.root / '残缺.zip'
        invalid.write_bytes(b'not a package')
        collection = self.root / '合集'
        for name in ['甲', '乙']:
            shutil.copytree(source, collection / name)
        for candidate in [invalid, self.root, self.models, collection]:
            self.assertNotEqual(self.import_model(candidate, check=False).returncode, 0, candidate)
        self.assertEqual({p.name for p in self.models.iterdir()}, {source.name})
        self.assertFalse(list(self.root.glob('.ysm-import-*')))

    def test_saved_source_is_loaded_even_when_it_is_not_first(self):
        self.source('a.zip'); self.source('b.zip')
        self.preferences({'source': 'b.zip', 'model': 'b', 'texture': 'blue'})
        frame = self.run_library()[0]
        self.assertEqual(frame['library']['selected_source'], 'b.zip')
        self.assertEqual(frame['model']['selected_texture'], 'blue')

    def test_group_discovery_keeps_both_models_and_the_exact_saved_key(self):
        (self.models / '角色合集').mkdir()
        self.source('角色合集/甲', folder=True)
        self.source('角色合集/乙.zip', nested=True)
        self.preferences({'source': '角色合集/乙.zip', 'texture': 'blue'})
        frame = self.run_library()[0]
        self.assertEqual({row['source'] for row in frame['attempt']['catalog']}, {'角色合集/甲', '角色合集/乙.zip'})
        self.assertEqual(frame['library']['selected_source'], '角色合集/乙.zip')
        self.assertEqual(frame['model']['selected_texture'], 'blue')

    def test_bad_unselected_binary_card_preserves_selection_and_is_attempted_once_per_scan(self):
        self.source('good.zip')
        (self.models / 'later.ysm').write_bytes(bytes.fromhex('efbbbf') + b'YSGP')
        frames = self.run_library(['initial', 'read_card', 'read_card'])
        self.assertEqual(frames[0]['error'], '')
        self.assertTrue(frames[1]['error'])
        self.assertEqual(frames[2]['error'], '')
        self.assertIsNone(frames[2]['attempt'])
        self.assertTrue(all(frame['library']['selected_source'] == 'good.zip' for frame in frames))

    def test_legacy_folder_and_nested_zip_load_without_manifest_or_clip_file(self):
        geometry = {'minecraft:geometry': [{'description': {'texture_width': 2, 'texture_height': 2},
                                           'bones': [{'name': 'root'}]}]}
        files = {'main.json': json.dumps(geometry).encode(), 'arm.json': json.dumps(geometry).encode(),
                 'texture.png': png(), 'info.json': json.dumps({'name': '旧模型', 'authors': ['作者']}).encode()}
        path = self.models / '旧包'
        path.mkdir()
        for name, content in files.items():
            (path / name).write_bytes(content)
        archive = self.models / '旧式.zip'
        with zipfile.ZipFile(archive, 'w') as source:
            for name, content in files.items():
                source.writestr('外层/模型/' + name, content)
        self.preferences({'source': '旧式.zip'})
        frames = self.run_library(['initial', 'next'])
        self.assertEqual({f['library']['selected_source'] for f in frames}, {'旧包', '旧式.zip'})
        for frame in frames:
            self.assertEqual(frame['model']['name'], '旧模型')
            self.assertEqual(frame['model']['selected_texture'], 'texture')
            self.assertTrue(all(row['author'] == '作者' for row in frame['attempt']['catalog']))
        for name, content in files.items():
            self.assertEqual((path / name).read_bytes(), content)

    def test_legacy_model_and_texture_selection_are_restored(self):
        self.source('a.zip'); self.source('b.zip')
        self.preferences({'model': 'b', 'texture': 'blue'})
        frame = self.run_library()[0]
        self.assertEqual(frame['library']['selected_source'], 'b.zip')
        self.assertEqual(frame['model']['selected_texture'], 'blue')

    def test_directory_and_zip_with_same_legacy_id_remain_distinct(self):
        self.source('same', folder=True); self.source('same.zip')
        self.preferences({'source': 'same.zip', 'model': 'same', 'texture': 'blue'})
        frames = self.run_library(['initial', 'next'])
        self.assertEqual([f['library']['selected_source'] for f in frames], ['same.zip', 'same'])
        self.assertEqual([f['model']['name'] for f in frames], ['same.zip', 'same'])

    def test_ambiguous_legacy_selection_does_not_assign_its_texture_to_another_source(self):
        self.source('same', folder=True); self.source('same.zip')
        self.preferences({'model': 'same', 'texture': 'blue'})
        frame = self.run_library()[0]
        self.assertEqual(frame['model']['selected_texture'], 'red')
        self.assertTrue(any('Ambiguous legacy' in s for s in frame['attempt']['diagnostics']))

    def test_unknown_saved_source_falls_back_without_interpreting_it_as_a_path(self):
        self.source('a.zip')
        outside = self.root / 'outside.zip'
        outside.write_bytes(b'not a library source')
        self.preferences({'source': '../outside.zip', 'texture': 'blue'})
        frame = self.run_library()[0]
        self.assertEqual(frame['library']['selected_source'], 'a.zip')
        self.assertEqual(frame['model']['selected_texture'], 'red')
        self.assertEqual(outside.read_bytes(), b'not a library source')

    def test_bad_first_source_is_isolated_and_next_usable_source_is_loaded(self):
        (self.models / 'a.zip').write_bytes(b'broken zip')
        self.source('b.zip')
        frame = self.run_library()[0]
        self.assertEqual(frame['library']['selected_source'], 'b.zip')
        self.assertEqual([x['status'] for x in frame['attempt']['catalog']], ['invalid', 'ready'])

    def test_bad_saved_source_is_tried_before_a_valid_fallback(self):
        self.source('a.zip'); (self.models / 'b.zip').write_bytes(b'broken')
        self.preferences({'source': 'b.zip'})
        frame = self.run_library()[0]
        self.assertEqual(frame['library']['selected_source'], 'a.zip')
        self.assertEqual(frame['attempt']['catalog'][1]['status'], 'invalid')

    def test_missing_texture_falls_back_to_authored_default_with_a_diagnostic(self):
        self.source('a.zip')
        self.preferences({'source': 'a.zip', 'texture': 'missing'})
        frame = self.run_library()[0]
        self.assertEqual(frame['model']['selected_texture'], 'red')
        self.assertTrue(any('Saved texture unavailable' in s for s in frame['attempt']['diagnostics']))

    def test_cycle_wraps_and_uses_stable_case_insensitive_order(self):
        for name in ['c.zip', 'b.ZIP', 'A.zip']: self.source(name)
        frames = self.run_library(['initial', 'next', 'next', 'next'])
        self.assertEqual([f['library']['selected_source'] for f in frames], ['A.zip', 'b.ZIP', 'c.zip', 'A.zip'])

    def test_cycle_skips_a_broken_alternative_without_reloading_the_current_model(self):
        self.source('a.zip'); (self.models / 'b.zip').write_bytes(b'broken'); self.source('c.zip')
        frames = self.run_library(['initial', 'next'])
        self.assertEqual(frames[-1]['library']['selected_source'], 'c.zip')
        self.assertEqual(frames[-1]['attempt']['catalog'][1]['status'], 'invalid')

    def test_only_one_model_leaves_the_current_selection_and_texture_intact(self):
        self.source('a.zip')
        frames = self.run_library(['initial', {'action': 'texture', 'name': 'blue'}, 'next'])
        self.assertFalse(frames[-1]['attempt']['prepared'])
        self.assertEqual(frames[-1]['model'], frames[-2]['model'])
        self.assertEqual(frames[-1]['library']['preferences'], frames[-2]['library']['preferences'])

    def test_failed_alternatives_do_not_replace_a_working_model_or_save_preferences(self):
        self.source('a.zip'); (self.models / 'b.zip').write_bytes(b'broken')
        self.preferences({'source': 'a.zip', 'texture': 'blue'})
        before = self.selection.read_bytes()
        frames = self.run_library(['initial', 'next'])
        self.assertFalse(frames[-1]['attempt']['prepared'])
        self.assertEqual(frames[-1]['model'], frames[0]['model'])
        self.assertEqual(self.selection.read_bytes(), before)

    def test_preparing_a_candidate_does_not_accept_it_or_change_preferences(self):
        self.source('a.zip'); self.source('b.zip')
        frames = self.run_library(['initial', 'prepare_next'])
        self.assertTrue(frames[-1]['attempt']['prepared'])
        self.assertEqual(frames[-1]['attempt']['source'], 'b.zip')
        self.assertEqual(frames[-1]['library'], frames[0]['library'])
        self.assertEqual(frames[-1]['model'], frames[0]['model'])
        self.assertFalse(self.selection.exists())

    def test_cancelled_load_cannot_replace_current_selection(self):
        self.source('a.zip'); self.source('b.zip')
        frames = self.run_library(['initial', {'action': 'next', 'cancel': True}])
        self.assertTrue(frames[-1]['attempt']['cancelled'])
        self.assertEqual(frames[-1]['model'], frames[0]['model'])
        self.assertEqual(frames[-1]['library'], frames[0]['library'])

    def test_cancelled_initial_load_does_not_publish_a_model(self):
        self.source('a.zip')
        frame = self.run_library([{'action': 'initial', 'cancel': True}])[0]
        self.assertTrue(frame['attempt']['cancelled'])
        self.assertIsNone(frame['model'])
        self.assertEqual(frame['library']['selected_source'], '')

    def test_cancellation_after_cpu_validation_discards_the_initial_candidate(self):
        self.source('a.zip')
        frame = self.run_library([{'action': 'initial', 'cancel_after_validation': True}])[0]
        self.assertTrue(frame['attempt']['cancelled'])
        self.assertIsNone(frame['model'])
        self.assertEqual(frame['library']['selected_source'], '')

    def test_cancellation_after_background_preparation_preserves_the_current_owner(self):
        self.source('a.zip'); self.source('b.zip')
        frames = self.run_library(['initial', {'action': 'async_next', 'cancel_after_validation': True}])
        self.assertTrue(frames[-1]['attempt']['prepared_off_thread'])
        self.assertTrue(frames[-1]['attempt']['cancelled'])
        self.assertEqual(frames[-1]['library'], frames[0]['library'])
        self.assertEqual(frames[-1]['model'], frames[0]['model'])

    def test_background_loading_uses_the_same_selection_and_texture_rules(self):
        self.source('a.zip'); self.source('b.zip')
        self.preferences({'source': 'b.zip', 'texture': 'blue'})
        frames = self.run_library(['async_initial', 'async_next', 'async_next'])
        self.assertTrue(all(frame['attempt']['prepared_off_thread'] for frame in frames))
        self.assertEqual([frame['library']['selected_source'] for frame in frames], ['b.zip', 'a.zip', 'b.zip'])
        self.assertEqual(frames[-1]['model']['selected_texture'], 'blue')

    def test_each_model_keeps_its_texture_across_switches_and_restart(self):
        self.source('a.zip'); self.source('b.zip')
        frames = self.run_library(['initial', {'action': 'texture', 'name': 'blue'}, 'next', 'save', 'restart',
                                   'next', 'save', 'restart', 'next'])
        self.assertEqual(frames[4]['library']['selected_source'], 'b.zip')
        self.assertEqual(frames[5]['model']['selected_texture'], 'blue')
        self.assertEqual(frames[7]['library']['selected_source'], 'a.zip')
        self.assertEqual(frames[-1]['model']['selected_texture'], 'red')
        saved = json.loads(self.selection.read_text(encoding='utf-8'))
        self.assertEqual(saved['textures'], {'a.zip': 'blue', 'b.zip': 'red'})

    def test_per_source_texture_map_takes_precedence_over_legacy_top_level_texture(self):
        self.source('a.zip')
        self.preferences({'model': 'a', 'texture': 'red', 'textures': {'a.zip': 'blue'}})
        self.assertEqual(self.run_library()[0]['model']['selected_texture'], 'blue')

    def test_save_replaces_existing_preferences_atomically_and_preserves_future_fields(self):
        self.source('a.zip')
        self.preferences({'model': 'a', 'texture': 'red', 'future': {'answer': 42}})
        frames = self.run_library(['initial', {'action': 'texture', 'name': 'blue'}, 'save'])
        self.assertEqual(frames[-1]['error'], '')
        saved = json.loads(self.selection.read_text(encoding='utf-8'))
        self.assertEqual(saved['source'], 'a.zip')
        self.assertEqual(saved['texture'], 'blue')
        self.assertEqual(saved['future'], {'answer': 42})
        self.assertEqual(list(self.root.glob('.ysm-selection-*.tmp')), [])

    def test_save_failure_preserves_the_existing_destination_and_cleans_its_temporary_file(self):
        self.source('a.zip')
        self.selection.mkdir()
        keep = self.selection / 'keep.txt'
        keep.write_text('keep', encoding='utf-8')
        frames = self.run_library(['initial', 'save'])
        self.assertNotEqual(frames[-1]['error'], '')
        self.assertEqual(keep.read_text(encoding='utf-8'), 'keep')
        self.assertEqual(list(self.root.glob('.ysm-selection-*.tmp')), [])

    def test_startup_does_not_rewrite_preferences(self):
        self.source('a.zip')
        self.run_library()
        self.assertFalse(self.selection.exists())

    def test_all_broken_models_leave_no_selection_with_individual_diagnostics(self):
        for name in ['a.zip', 'b.zip']: (self.models / name).write_bytes(b'broken')
        frame = self.run_library()[0]
        self.assertIsNone(frame['model'])
        self.assertEqual([x['status'] for x in frame['attempt']['catalog']], ['invalid', 'invalid'])

    def test_unicode_zip_and_nested_folder_sources_are_supported(self):
        self.source('绘梨衣.ZIP'); self.source('角色.v1', folder=True, nested=True)
        self.preferences({'source': '角色.v1'})
        frames = self.run_library(['initial', 'next'])
        self.assertEqual([f['library']['selected_source'] for f in frames], ['角色.v1', '绘梨衣.ZIP'])
        self.assertEqual(frames[0]['library']['preferences']['model'], '角色')

    def test_unrelated_files_are_ignored_and_non_model_directories_are_isolated(self):
        (self.models / 'readme.txt').write_text('test', encoding='utf-8')
        (self.models / '000_cache').mkdir()
        self.source('model.zip')
        frame = self.run_library()[0]
        self.assertEqual(frame['library']['selected_source'], 'model.zip')
        self.assertEqual(len(frame['attempt']['catalog']), 2)

    def test_external_directory_junction_is_not_a_model_source(self):
        self.source('a.zip')
        outside = self.root / 'external'
        outside.mkdir()
        link = self.models / '000_external'
        pwsh = shutil.which('pwsh')
        if not pwsh:
            self.skipTest('PowerShell is needed to create a Windows junction without symlink privileges')
        script = self.root / 'junction.ps1'
        script.write_text('param([string]$Link,[string]$Target)\nNew-Item -ItemType Junction -Path $Link -Target $Target -ErrorAction Stop | Out-Null\n', encoding='utf-8')
        result = subprocess.run([pwsh, '-NoProfile', '-File', str(script), '-Link', str(link), '-Target', str(outside)],
                                capture_output=True, encoding='utf-8', timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        try:
            frame = self.run_library()[0]
            self.assertEqual(frame['library']['selected_source'], 'a.zip')
            self.assertEqual([x['source'] for x in frame['attempt']['catalog']], ['a.zip'])
            self.assertTrue(any('external model source' in s for s in frame['attempt']['diagnostics']))
            self.assertTrue(outside.is_dir())
        finally:
            os.rmdir(link)

    def test_malformed_preferences_recover_without_overwriting_the_source_file(self):
        self.source('a.zip')
        for raw in [b'broken JSON', b'[]', b'{"textures":[]}', b'{"textures":9}']:
            with self.subTest(raw=raw):
                self.selection.write_bytes(raw)
                frame = self.run_library()[0]
                self.assertEqual(frame['library']['selected_source'], 'a.zip')
                self.assertTrue(any('Cannot read saved selection' in s for s in frame['attempt']['diagnostics']))
                self.assertEqual(self.selection.read_bytes(), raw)

    def test_oversized_preference_file_is_not_parsed_or_rewritten(self):
        self.source('a.zip')
        self.selection.write_bytes(b'x' * (1024 * 1024 + 1))
        frame = self.run_library()[0]
        self.assertEqual(frame['library']['selected_source'], 'a.zip')
        self.assertTrue(any('1 MiB' in s for s in frame['attempt']['diagnostics']))
        self.assertEqual(self.selection.stat().st_size, 1024 * 1024 + 1)

    def test_unknown_texture_does_not_mutate_selection(self):
        self.source('a.zip')
        frames = self.run_library(['initial', {'action': 'texture', 'name': 'missing'}])
        self.assertNotEqual(frames[-1]['error'], '')
        self.assertEqual(frames[-1]['model'], frames[0]['model'])
        self.assertEqual(frames[-1]['library'], frames[0]['library'])

    def test_non_string_texture_preference_is_ignored(self):
        self.source('a.zip')
        self.preferences({'source': 'a.zip', 'texture': None, 'textures': {'a.zip': 9}})
        self.assertEqual(self.run_library()[0]['model']['selected_texture'], 'red')

    def test_model_count_limit_rejects_an_incomplete_catalog(self):
        for i in range(1025): (self.models / ('%04d.zip' % i)).write_bytes(b'')
        frame = self.run_library()[0]
        self.assertIsNone(frame['model'])
        self.assertTrue(any('source budget' in s for s in frame['attempt']['diagnostics']))

    def test_directory_entry_limit_rejects_unbounded_discovery(self):
        for i in range(4097): (self.models / ('%04d.txt' % i)).write_bytes(b'')
        frame = self.run_library()[0]
        self.assertIsNone(frame['model'])
        self.assertTrue(any('entry budget' in s for s in frame['attempt']['diagnostics']))

    def test_accept_failure_does_not_partially_publish_a_new_selection(self):
        self.source('a.zip')
        self.preferences({'textures': {'absent-%04d.zip' % i: 'red' for i in range(1024)}})
        frame = self.run_library()[0]
        self.assertNotEqual(frame['error'], '')
        self.assertEqual(frame['library']['selected_source'], '')
        self.assertIsNone(frame['model'])

    def test_missing_saved_file_uses_a_valid_fallback_without_destroying_the_saved_request(self):
        self.source('a.zip'); b = self.source('b.zip')
        self.preferences({'source': 'b.zip', 'texture': 'blue'})
        self.assertEqual(self.run_library()[0]['library']['selected_source'], 'b.zip')
        b.unlink()
        before = self.selection.read_bytes()
        self.assertEqual(self.run_library()[0]['library']['selected_source'], 'a.zip')
        self.assertEqual(self.selection.read_bytes(), before)

    def test_missing_source_keeps_its_texture_after_saving_the_fallback_and_restarting(self):
        self.source('a.zip'); b = self.source('b.zip')
        original = b.read_bytes()
        b.unlink()
        self.preferences({'source': 'b.zip', 'model': 'b', 'texture': 'blue'})
        self.run_library(['initial', 'save'])
        b.write_bytes(original)
        frame = self.run_library(['initial', 'next'])[-1]
        self.assertEqual(frame['library']['selected_source'], 'b.zip')
        self.assertEqual(frame['model']['selected_texture'], 'blue')

    def test_unresolved_legacy_texture_survives_fallback_save_and_later_source_repair(self):
        self.source('a.zip'); b = self.source('b.zip')
        original = b.read_bytes()
        b.unlink()
        self.preferences({'model': 'b', 'texture': 'blue'})
        self.run_library(['initial', 'save'])
        b.write_bytes(original)
        frame = self.run_library(['initial', 'next'])[-1]
        self.assertEqual(frame['library']['selected_source'], 'b.zip')
        self.assertEqual(frame['model']['selected_texture'], 'blue')

    def test_original_eri_can_be_restored_as_a_non_first_library_entry(self):
        self.source('a.zip')
        before = hashlib.sha256(SAMPLE.read_bytes()).hexdigest()
        shutil.copyfile(SAMPLE, self.models / '绘梨衣.zip')
        self.preferences({'model': '绘梨衣', 'texture': 'default'})
        frame = self.run_library()[0]
        self.assertEqual(frame['library']['selected_source'], '绘梨衣.zip')
        self.assertEqual([frame['model'][k] for k in ['main_bones', 'main_cubes', 'animation_clips']], [177, 1512, 242])
        self.assertEqual(hashlib.sha256(SAMPLE.read_bytes()).hexdigest(), before)


if __name__ == '__main__':
    unittest.main()
