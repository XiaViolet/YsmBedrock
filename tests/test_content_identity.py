"""Source identities follow loaded assets and are independent of texture selection/container naming."""
import hashlib
import json
import pathlib
import shutil
import subprocess
import tempfile
import unittest
import zipfile

from test_import import EXE
import test_model_library as library
from test_model_textures import png


class ContentIdentityTests(unittest.TestCase):
    def setUp(self):
        self.source_builder = library.ModelLibraryTests()
        self.source_builder.setUp()
        self.addCleanup(self.source_builder.temporary.cleanup)

    def load(self, path, texture=None):
        args = [str(EXE), str(path)] if texture is None else [str(EXE), '--select-texture', str(path), texture]
        run = subprocess.run(args, capture_output=True, encoding='utf-8', timeout=8)
        self.assertEqual(run.returncode, 0, run.stderr)
        return json.loads(run.stdout)

    @staticmethod
    def rewrite(path, name, data):
        with zipfile.ZipFile(path) as archive:
            files = {n: archive.read(n) for n in archive.namelist()}
        files[name] = data
        with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as archive:
            for n, value in files.items():
                archive.writestr(n, value)

    def test_sha256_content_key_matches_independent_length_framed_reference(self):
        path = self.source_builder.source('probe.zip')
        with zipfile.ZipFile(path) as archive:
            files = {n: archive.read(n) for n in archive.namelist()}
        payload = b'YsmBedrock.SourceContent.1\n'
        for name, data in sorted(files.items()):
            encoded = name.encode('utf-8')
            payload += str(len(encoded)).encode() + b':' + encoded + b':' + hashlib.sha256(data).hexdigest().encode() + b'\n'
        self.assertEqual(self.load(path)['local_content_key'], 'local1_' + hashlib.sha256(payload).hexdigest())

    def test_repacking_renaming_and_directory_source_keep_one_content_key(self):
        path = self.source_builder.source('probe.zip')
        original = self.load(path)['local_content_key']
        renamed = path.with_name('renamed.zip')
        shutil.copy2(path, renamed)
        self.assertEqual(self.load(renamed)['local_content_key'], original)
        folder = path.parent / 'folder'
        with zipfile.ZipFile(path) as archive:
            files = {n: archive.read(n) for n in archive.namelist()}
            archive.extractall(folder)
        repacked = path.with_name('repacked.zip')
        with zipfile.ZipFile(repacked, 'w', zipfile.ZIP_STORED) as archive:
            for name, data in reversed(list(files.items())):
                archive.writestr('new/root/' + name, data)
        self.assertEqual(self.load(folder)['local_content_key'], original)
        self.assertEqual(self.load(repacked)['local_content_key'], original)

    def test_texture_selection_changes_rendering_identity_but_keeps_state_namespace(self):
        path = self.source_builder.source('probe.zip')
        red, blue = self.load(path, 'red'), self.load(path, 'blue')
        self.assertNotEqual(red['render_key'], blue['render_key'])
        self.assertEqual(red['local_content_key'], blue['local_content_key'])

    def test_animation_only_edit_cannot_borrow_a_same_geometry_and_pixels_namespace(self):
        path = self.source_builder.source('probe.zip')
        before = self.load(path)
        self.rewrite(path, 'animation.json', b'{"animations":{"idle":{"loop":true,"animation_length":2}}}')
        after = self.load(path)
        self.assertEqual(before['render_key'], after['render_key'])
        self.assertNotEqual(before['local_content_key'], after['local_content_key'])

    def test_unselected_texture_edit_changes_the_model_namespace(self):
        path = self.source_builder.source('probe.zip')
        before = self.load(path)
        self.rewrite(path, 'textures/blue.png', png(rgba=(3, 7, 11, 255)))
        after = self.load(path)
        self.assertEqual(before['render_key'], after['render_key'])
        self.assertNotEqual(before['local_content_key'], after['local_content_key'])

    def test_function_content_and_empty_function_files_participate_in_identity(self):
        path = self.source_builder.source('probe.zip')
        keys = [self.load(path)['local_content_key']]
        self.rewrite(path, 'functions/value.molang', b'')
        keys.append(self.load(path)['local_content_key'])
        self.rewrite(path, 'functions/value.molang', b'return 7;')
        keys.append(self.load(path)['local_content_key'])
        self.assertEqual(len(set(keys)), 3)

    def test_controller_content_participates_even_when_geometry_and_images_match(self):
        path = self.source_builder.source('probe.zip')
        with zipfile.ZipFile(path) as archive:
            manifest = json.loads(archive.read('ysm.json'))
        manifest['files']['player']['animation_controllers'] = ['controllers.json']
        self.rewrite(path, 'ysm.json', json.dumps(manifest).encode())
        self.rewrite(path, 'controllers.json', b'{"animation_controllers":{"player.main":{"states":{"default":{}}}}}')
        before = self.load(path)
        self.rewrite(path, 'controllers.json', b'{"animation_controllers":{"player.main":{"states":{"default":{"on_entry":"r.a=1;"}}}}}')
        after = self.load(path)
        self.assertEqual(before['render_key'], after['render_key'])
        self.assertNotEqual(before['local_content_key'], after['local_content_key'])

    def test_unused_document_changes_do_not_change_loaded_source_identity(self):
        path = self.source_builder.source('probe.zip')
        before = self.load(path)['local_content_key']
        self.rewrite(path, 'notes/readme.txt', b'Local notes, not a model resource.')
        self.assertEqual(self.load(path)['local_content_key'], before)

