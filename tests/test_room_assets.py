"""Exercise the streaming cache, content identity, private exports and resource limits on disk."""
import hashlib
import json
import pathlib
import shutil
import subprocess
import tempfile
import unittest
import zipfile

from test_import import EXE
import test_model_library


class RoomAssetsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name)
        self.models = self.root / 'models'
        self.models.mkdir()

    def source(self, name, folder=False):
        return test_model_library.ModelLibraryTests.source(self, name, folder)

    def run_replay(self, source, name='work', check=True):
        p = subprocess.run([EXE, '--room-assets-test', self.root / name, source],
                           capture_output=True, encoding='utf-8', timeout=35)
        if not check:
            self.assertNotEqual(p.returncode, 0)
            return p.stderr
        self.assertEqual(p.returncode, 0, p.stderr)
        return json.loads(p.stdout)

    def test_streaming_integrity_cache_fallback_and_deliberate_collection(self):
        source = self.source('原样.zip')
        before = source.read_bytes()
        result = self.run_replay(source)
        self.assertEqual(result['digest'], hashlib.sha256(before).hexdigest())
        self.assertEqual(result['content'], result['guest'])
        self.assertGreater(result['chunks'], 1)
        self.assertEqual(result['spec'], 2)
        self.assertIsNone(result['binary_origin'])
        for flag in ('cached', 'private_reused', 'wrong_key_rejected', 'wrong_offset_rejected', 'canceled',
                     'damaged_rejected', 'capacity_enforced', 'pinned', 'evicted', 'manual_collection',
                     'collection_idempotent', 'stale_world_rejected', 'abandoned_stage_cleaned', 'restart_hit',
                     'disk_corruption_repaired', 'full_cache_local_repair', 'parallel_repair', 'false_format_rejected'):
            self.assertTrue(result[flag], flag)
        self.assertEqual(pathlib.Path(result['collected']).read_bytes(), before)
        self.assertEqual(source.read_bytes(), before)

    def test_directory_snapshot_is_deterministic_and_excludes_private_files(self):
        model = self.source('服装', True)
        (model / '私人备注.txt').write_text('private', encoding='utf8')
        a = self.run_replay(model)
        b = self.run_replay(model, 'repeat')
        self.assertTrue(a['unopened_directory_private'])
        self.assertTrue(a['private_reused'])
        self.assertEqual(a['digest'], b['digest'])
        with zipfile.ZipFile(a['collected']) as z:
            self.assertIn('textures/red.png', z.namelist())
            self.assertNotIn('私人备注.txt', z.namelist())
        renamed = self.source('同名.zip')
        other = self.run_replay(renamed, 'other')
        self.assertNotEqual(a['digest'], other['digest'])

    def test_protected_root_and_unsafe_unused_zip_members_rejected(self):
        model = self.source('原包.zip')
        private = self.models / 'auth'
        private.mkdir()
        shutil.copyfile(model, private / model.name)
        self.assertIn('authorized', self.run_replay(private / model.name, check=False))
        for i, name in enumerate(('../unused.txt', 'safe/tool.dll', 'x' * 1025, 'textures/RED.PNG')):
            suspect = self.root / ('variant' + str(i) + '.zip')
            shutil.copyfile(model, suspect)
            with zipfile.ZipFile(suspect, 'a') as archive:
                archive.writestr(name, 'unused')
            self.run_replay(suspect, 'blocked' + str(i), check=False)

    def test_identical_names_different_content_do_not_share_files(self):
        original = self.source('同一名字.zip')
        group = self.models / '另一组'
        group.mkdir()
        second = group / original.name
        with zipfile.ZipFile(original) as raw, zipfile.ZipFile(second, 'w', zipfile.ZIP_DEFLATED) as changed:
            for name in raw.namelist():
                data = raw.read(name)
                if name == 'animation.json':
                    data = b'{"animations":{"idle":{"loop":true,"animation_length":2.25}}}'
                changed.writestr(name, data)
        a, b = self.run_replay(original), self.run_replay(second, 'work2')
        self.assertNotEqual(a['digest'], b['digest'])
        self.assertNotEqual(a['guest'], b['guest'])

    def test_room_json_field_names_and_string_values_still_bounded(self):
        model = self.source('模型', True)
        descriptor = model / 'ysm.json'
        original = json.loads(descriptor.read_text(encoding='utf8'))
        long_text = 'x' * ((1 << 20) + 1)
        for index, (name, value) in enumerate(((long_text, 0), ('notes', long_text))):
            descriptor.write_text(json.dumps({**original, name: value}), encoding='utf8')
            self.assertIn('string length limit: ysm.json',
                          self.run_replay(model, 'large-string-' + str(index), check=False))

    def test_overdeep_model_document_rejected(self):
        source = self.source('模型', True)
        descriptor = source / 'ysm.json'
        value = json.loads(descriptor.read_text(encoding='utf8'))
        deep = {}
        for _ in range(170):
            deep = {'child': deep}
        value['deep'] = deep
        descriptor.write_text(json.dumps(value), encoding='utf8')
        self.assertIn('complexity', self.run_replay(source, check=False))


if __name__ == '__main__':
    unittest.main()
