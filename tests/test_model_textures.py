"""Source texture variants use the same decoded assets in the inspector and client."""
import hashlib
import json
import pathlib
import struct
import subprocess
import tempfile
import unittest
import zipfile
import zlib

from test_import import EXE, SAMPLE, PROJECT
from jpeg_fixtures import jpeg
from webp_fixtures import webp
from avif_fixtures import avif


def png(width=2, height=2, rgba=(220, 80, 40, 255)):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    raw = (b'\0' + bytes(rgba) * width) * height
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>2I5B', width, height, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def corrupt_png():
    # Valid dimensions and PNG chunk layout, with an invalid compressed pixel stream.
    data = png()
    return data[:33] + struct.pack('>I', 3) + b'IDATbad' + b'\0' * 4 + data[-12:]


class ModelTextureTests(unittest.TestCase):
    def manifest(self, textures, default=''):
        return {'spec': 2, 'properties': {'default_texture': default},
                'files': {'player': {'model': {'main': 'main.json', 'arm': 'arm.json'},
                                     'animation': {'main': 'animation.json'}, 'texture': textures}}}

    def run_pack(self, textures, images=None, default='', select=None, folder=False, error=None):
        geometry = {'minecraft:geometry': [{'description': {'texture_width': 2, 'texture_height': 2},
                                             'bones': [{'name': 'root'}]}]}
        files = {'ysm.json': json.dumps(self.manifest(textures, default)).encode(),
                 'main.json': json.dumps(geometry).encode(), 'arm.json': json.dumps(geometry).encode(),
                 'animation.json': b'{"animations":{"idle":{"loop":true}}}'}
        files.update(images or {'textures/red.png': png(), 'textures/blue.png': png(rgba=(40, 80, 220, 255))})
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            if folder:
                for name, data in files.items():
                    target = path / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(data)
            else:
                path /= 'textures.zip'
                with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as archive:
                    for name, data in files.items():
                        archive.writestr('source/' + name, data)
            args = [str(EXE), str(path)] if select is None else [str(EXE), '--select-texture', str(path), select]
            run = subprocess.run(args, capture_output=True, encoding='utf-8', timeout=10)
        if error is not None:
            self.assertEqual(run.returncode, 1, run.stdout)
            self.assertIn(error, run.stderr)
            return
        self.assertEqual(run.returncode, 0, run.stderr)
        return json.loads(run.stdout)

    def test_string_and_object_declarations_render_identically(self):
        plain = self.run_pack(['textures/red.png'])
        objects = self.run_pack([{'uv': 'textures/red.png', 'ignored': {'future': 1}}])
        # The source manifests have different bytes; the private restore keys
        # therefore differ. All decoded/rendered properties remain equivalent.
        self.assertNotEqual(plain.pop('local_content_key'), objects.pop('local_content_key'))
        self.assertEqual(plain, objects)

    def test_all_variants_decoded_with_authored_default_selected(self):
        report = self.run_pack(['textures/red.png', {'uv': 'textures/blue.png'}], default='blue')
        self.assertEqual(report['selected_texture'], 'blue')
        self.assertEqual(report['texture_count'], 2)
        self.assertEqual([x['name'] for x in report['textures']], ['red', 'blue'])
        for texture in report['textures']:
            self.assertEqual(texture['uv']['width'], 2)
            self.assertEqual(texture['uv']['height'], 2)
            self.assertEqual(texture['uv']['rgba_bytes'], 16)

    def test_unset_default_uses_first_variant(self):
        report = self.run_pack(['textures/blue.png', 'textures/red.png'])
        self.assertEqual(report['selected_texture'], 'blue')

    def test_missing_default_falls_back_with_diagnostic(self):
        report = self.run_pack(['textures/red.png'], default='missing')
        self.assertEqual(report['selected_texture'], 'red')
        self.assertTrue(any('Default texture not found' in x for x in report['warnings']))

    def test_zip_uses_exact_resource_case_before_legacy_fallback(self):
        maps = {'red.png': png(2, 2), 'RED.png': png(4, 2), 'blue.png': png(8, 2)}
        for files in (maps, dict(reversed(list(maps.items())))):
            with self.subTest(order=list(files)):
                texture = self.run_pack([{'uv': 'red.png', 'normal': 'RED.png', 'specular': 'Blue.png'}],
                                        files)['textures'][0]
                self.assertEqual([texture[key]['width'] for key in ('uv', 'normal', 'specular')], [2, 4, 8])

    def test_normal_and_specular_are_decoded_retained_and_reported_separately(self):
        report = self.run_pack([{'uv': 'textures/red.png', 'normal': 'textures/n.png', 'specular': 'textures/s.png'}],
                               {'textures/red.png': png(), 'textures/n.png': png(4, 2), 'textures/s.png': png(2, 4)})
        texture = report['textures'][0]
        self.assertEqual(texture['normal']['path'], 'textures/n.png')
        self.assertEqual(texture['normal']['rgba_bytes'], 32)
        self.assertEqual(texture['specular']['height'], 4)
        self.assertEqual(report['pbr_encoding'], 'oldPBR')
        self.assertEqual(texture['specular']['mers_first_pixel'], [80, 40, 35, 0])

    def test_declared_labpbr_converts_channels_and_recovers_normal_z(self):
        for folder in (False, True):
            for green, blue, alpha, expected in [(229, 255, 254, [0, 255, 223, 255]),
                                                 (230, 255, 255, [255, 0, 223, 0]),
                                                 (15, 64, 127, [0, 128, 223, 0])]:
                with self.subTest(folder=folder, green=green, blue=blue, alpha=alpha):
                    report = self.run_pack([{'uv': 'a.png', 'normal': 'n.png', 'specular': 's.png'}],
                        {'a.png': png(), 'n.png': png(rgba=(128, 128, 5, 0)),
                         's.png': png(rgba=(32, green, blue, alpha)),
                         'texture.properties': b'# encoding\r\n format : lab-pbr/1.3\r\n'}, folder=folder)
                    self.assertEqual(report['pbr_encoding'], 'lab-pbr/1.3')
                    texture = report['textures'][0]
                    self.assertEqual(texture['normal']['bedrock_first_pixel'], [128, 128, 255, 255])
                    self.assertEqual(texture['specular']['mers_first_pixel'], expected)

    def test_native_cache_distinguishes_pbr_variants_with_identical_albedo(self):
        images = {'a.png': png(), 'b.png': png(), 'data.png': png()}
        variants = [{'uv': 'a.png', 'normal': 'data.png'}, {'uv': 'b.png', 'specular': 'data.png'}]
        first = self.run_pack(variants, images)
        second = self.run_pack(variants, images, select='b')
        self.assertNotEqual(first['render_key'], second['render_key'])
        declared = self.run_pack(variants, dict(images, **{'texture.properties': b'format=lab-pbr'}))
        self.assertNotEqual(first['render_key'], declared['render_key'])

    def test_optional_null_and_blank_channels_are_absent(self):
        report = self.run_pack([{'uv': 'textures/red.png', 'normal': None, 'specular': ' \t\n'}])
        self.assertIsNone(report['textures'][0]['normal'])
        self.assertIsNone(report['textures'][0]['specular'])
        self.assertFalse(report['warnings'])

    def test_missing_required_uv_is_rejected(self):
        self.run_pack([{'normal': 'textures/red.png'}], error='Missing texture uv')

    def test_blank_required_uv_is_rejected(self):
        self.run_pack([' \t'], error='Missing texture uv')

    def test_null_texture_entry_is_rejected(self):
        self.run_pack([None], error='Expected string or object')

    def test_empty_texture_list_is_rejected(self):
        self.run_pack([], error='Missing player texture')

    def test_missing_optional_channel_file_is_not_silently_dropped(self):
        self.run_pack([{'uv': 'textures/red.png', 'normal': 'missing.png'}], error='Missing resource')

    def test_channel_path_traversal_is_rejected(self):
        self.run_pack([{'uv': 'textures/red.png', 'specular': '../secret.png'}], error='escapes')

    def test_invalid_optional_channel_is_rejected(self):
        self.run_pack([{'uv': 'textures/red.png', 'normal': ['textures/blue.png']}], error='Invalid texture normal')

    def test_unselected_corrupted_variant_is_rejected(self):
        self.run_pack(['textures/red.png', 'textures/bad.png'],
                      {'textures/red.png': png(), 'textures/bad.png': corrupt_png()}, error='decode PNG')

    def test_corrupted_pbr_channel_is_rejected(self):
        self.run_pack([{'uv': 'textures/red.png', 'specular': 'textures/bad.png'}],
                      {'textures/red.png': png(), 'textures/bad.png': corrupt_png()}, error='decode PNG')

    def test_oversized_dimensions_are_rejected_before_allocation(self):
        self.run_pack(['textures/red.png'], {'textures/red.png': png(4097, 1)}, error='oversized texture dimensions')

    def test_decoded_texture_memory_is_bounded(self):
        data = png(4096, 4096)
        names = [f'textures/{i}.png' for i in range(8)]
        self.run_pack(names, dict.fromkeys(names, data), error='decoded texture memory limit')

    def test_seven_full_resolution_textures_can_load_without_downsampling(self):
        data = png(4096, 4096)
        names = [f'textures/{i}.png' for i in range(7)]
        report = self.run_pack(names, dict.fromkeys(names, data))
        self.assertEqual(report['texture_count'], 7)
        self.assertTrue(all(t['uv']['width'] == 4096 and t['uv']['height'] == 4096
                            and t['uv']['rgba_bytes'] == 64 * 1024 * 1024 for t in report['textures']))

    def test_repeated_image_references_share_the_decode_budget(self):
        report = self.run_pack([{'uv': 'textures/red.png', 'normal': 'textures/red.png', 'specular': 'textures/red.png'}] * 8,
                               {'textures/red.png': png(4096, 4096)})
        self.assertEqual(report['texture_count'], 1)

    def test_duplicate_basenames_replace_in_place_like_upstream(self):
        report = self.run_pack(['a/red.png', 'textures/blue.png', 'b/red.png'],
                               {'a/red.png': png(), 'textures/blue.png': png(), 'b/red.png': png(4, 4)})
        self.assertEqual([x['name'] for x in report['textures']], ['red', 'blue'])
        self.assertEqual(report['textures'][0]['uv']['path'], 'b/red.png')
        self.assertEqual(report['textures'][0]['uv']['width'], 4)

    def test_normalized_windows_paths_and_unicode_names_match_in_folder_and_zip(self):
        textures = [{'uv': '.\\textures\\蓝色.v2.png'}]
        images = {'textures/蓝色.v2.png': png()}
        zipped = self.run_pack(textures, images)
        folder = self.run_pack(textures, images, folder=True)
        self.assertEqual(zipped, folder)
        self.assertEqual(zipped['selected_texture'], '蓝色.v2')

    def test_selection_changes_cache_identity_and_can_cycle_back(self):
        textures = ['textures/red.png', 'textures/blue.png']
        default = self.run_pack(textures)
        switched = self.run_pack(textures, select='blue')
        returned = self.run_pack(textures, select='red')
        self.assertNotEqual(default['render_key'], switched['render_key'])
        self.assertEqual(default, returned)

    def test_unknown_selection_is_rejected(self):
        self.run_pack(['textures/red.png'], select='missing', error='Unknown texture')

    def test_identical_pixels_with_different_variant_names_share_cache_without_losing_selection(self):
        images = dict.fromkeys(['textures/red.png', 'textures/alias.png'], png())
        red = self.run_pack(list(images), images)
        alias = self.run_pack(list(images), images, select='alias')
        self.assertEqual(red['render_key'], alias['render_key'])
        self.assertEqual(alias['selected_texture'], 'alias')

    def test_original_package_bytes_and_cache_identity_are_preserved(self):
        self.assertEqual(hashlib.sha256(SAMPLE.read_bytes()).hexdigest(),
                         '2a980fc1d8cf2ef7263471106e0b5f9eff651a6256e1dab4d961a303d77498ac')
        run = subprocess.run([str(EXE), str(SAMPLE)], capture_output=True, encoding='utf-8', timeout=5)
        self.assertEqual(run.returncode, 0, run.stderr)
        report = json.loads(run.stdout)
        self.assertEqual(report['texture_count'], 1)
        self.assertEqual(report['render_key'], 'v8_3049229670376254048_vis')
        self.assertEqual(report['textures'][0]['uv']['width'], 512)
        self.assertEqual(report['textures'][0]['uv']['height'], 512)

    def test_png_and_jpeg_variants_share_the_selection_path(self):
        report = self.run_pack(['textures/blue.png', {'uv': 'textures/red.jpg'}],
                               {'textures/blue.png': png(), 'textures/red.jpg': jpeg()}, default='red')
        self.assertEqual(report['selected_texture'], 'red')
        self.assertEqual([t['uv']['format'] for t in report['textures']], ['PNG', 'JPEG'])
        self.assertEqual(report['textures'][1]['uv']['rgba_bytes'], 24)

    def test_jpeg_optional_channels_are_decoded(self):
        report = self.run_pack([{'uv': 'red.png', 'normal': 'normal.jpg', 'specular': 'specular.jpeg'}],
                               {'red.png': png(), 'normal.jpg': jpeg('gray'), 'specular.jpeg': jpeg('progressive')})
        texture = report['textures'][0]
        self.assertEqual(texture['normal']['format'], 'JPEG')
        self.assertEqual(texture['specular']['format'], 'JPEG')

    def test_unselected_truncated_jpeg_is_rejected(self):
        self.run_pack(['red.png', 'bad.jpg'], {'red.png': png(), 'bad.jpg': jpeg()[:-2]}, error='decode JPEG')

    def test_unselected_jpeg_without_entropy_is_rejected(self):
        data = jpeg()
        marker = data.index(b'\xff\xda')
        end = marker + 2 + int.from_bytes(data[marker + 2:marker + 4], 'big')
        self.run_pack(['red.png', 'bad.jpg'], {'red.png': png(), 'bad.jpg': data[:end] + b'\xff\xd9'}, error='decode JPEG')

    def test_jpeg_unicode_paths_match_in_folder_and_zip(self):
        textures = [{'uv': '.\\textures\\红色.v2.JPEG'}]
        images = {'textures/红色.v2.JPEG': jpeg()}
        zipped = self.run_pack(textures, images)
        folder = self.run_pack(textures, images, folder=True)
        self.assertEqual(zipped, folder)
        self.assertEqual(zipped['selected_texture'], '红色.v2')

    def test_jpeg_replaces_same_basename_png_variant(self):
        report = self.run_pack(['a/red.png', 'b/red.jpg'], {'a/red.png': png(), 'b/red.jpg': jpeg()})
        self.assertEqual(report['texture_count'], 1)
        self.assertEqual(report['textures'][0]['uv']['format'], 'JPEG')

    def test_mixed_codec_selection_restores_png_cache_identity(self):
        textures = ['red.png', 'blue.jpeg']
        images = {'red.png': png(), 'blue.jpeg': jpeg()}
        original = self.run_pack(textures, images)
        switched = self.run_pack(textures, images, select='blue')
        restored = self.run_pack(textures, images, select='red')
        self.assertNotEqual(original['render_key'], switched['render_key'])
        self.assertEqual(original, restored)

    def test_png_jpeg_webp_variants_use_the_same_selection_path(self):
        report = self.run_pack(['blue.png', 'red.jpg', 'green.webp'],
                               {'blue.png': png(), 'red.jpg': jpeg(), 'green.webp': webp('rgba')}, default='green')
        self.assertEqual(report['selected_texture'], 'green')
        self.assertEqual([item['uv']['format'] for item in report['textures']], ['PNG', 'JPEG', 'WEBP'])
        self.assertEqual(report['textures'][2]['uv']['rgba_bytes'], 24)

    def test_webp_normal_and_specular_channels_are_retained(self):
        report = self.run_pack([{'uv': 'red.png', 'normal': 'normal.webp', 'specular': 'specular.webp'}],
                               {'red.png': png(), 'normal.webp': webp(), 'specular.webp': webp('lossy')})
        texture = report['textures'][0]
        self.assertEqual(texture['normal']['format'], 'WEBP')
        self.assertEqual(texture['specular']['format'], 'WEBP')

    def test_unselected_truncated_webp_is_rejected(self):
        self.run_pack(['red.png', 'bad.webp'], {'red.png': png(), 'bad.webp': webp()[:-1]}, error='WebP container')

    def test_unselected_animated_webp_is_rejected(self):
        self.run_pack(['red.png', 'bad.webp'], {'red.png': png(), 'bad.webp': webp('animated')}, error='Animated WebP')

    def test_webp_unicode_paths_match_in_folder_and_zip(self):
        textures = [{'uv': '.\\textures\\绿色.v2.WEBP'}]
        images = {'textures/绿色.v2.WEBP': webp()}
        zipped = self.run_pack(textures, images)
        folder = self.run_pack(textures, images, folder=True)
        self.assertEqual(zipped, folder)
        self.assertEqual(zipped['selected_texture'], '绿色.v2')

    def test_webp_cycle_restores_original_cache_identity(self):
        textures = ['red.png', 'blue.webp']
        images = {'red.png': png(), 'blue.webp': webp()}
        original = self.run_pack(textures, images)
        switched = self.run_pack(textures, images, select='blue')
        restored = self.run_pack(textures, images, select='red')
        self.assertNotEqual(original['render_key'], switched['render_key'])
        self.assertEqual(original, restored)

    def test_all_four_codecs_share_texture_selection(self):
        report = self.run_pack(['blue.png', 'red.jpg', 'green.webp', 'gold.avif'],
                               {'blue.png': png(), 'red.jpg': jpeg(), 'green.webp': webp(), 'gold.avif': avif()}, default='gold')
        self.assertEqual(report['selected_texture'], 'gold')
        self.assertEqual([item['uv']['format'] for item in report['textures']], ['PNG', 'JPEG', 'WEBP', 'AVIF'])

    def test_avif_normal_and_specular_channels_are_retained(self):
        report = self.run_pack([{'uv': 'red.png', 'normal': 'normal.avif', 'specular': 'specular.avif'}],
                               {'red.png': png(), 'normal.avif': avif(), 'specular.avif': avif('gray')})
        texture = report['textures'][0]
        self.assertEqual(texture['normal']['format'], 'AVIF')
        self.assertEqual(texture['specular']['format'], 'AVIF')

    def test_unselected_truncated_avif_is_rejected(self):
        self.run_pack(['red.png', 'bad.avif'], {'red.png': png(), 'bad.avif': avif()[:-1]}, error='AVIF container')

    def test_unselected_animated_avif_is_rejected(self):
        self.run_pack(['red.png', 'bad.avif'], {'red.png': png(), 'bad.avif': avif('animated')}, error='Animated AVIF')

    def test_avif_unicode_paths_match_in_folder_and_zip(self):
        textures = [{'uv': '.\\textures\\金色.v2.AVIF'}]
        images = {'textures/金色.v2.AVIF': avif()}
        zipped = self.run_pack(textures, images)
        self.assertEqual(zipped, self.run_pack(textures, images, folder=True))
        self.assertEqual(zipped['selected_texture'], '金色.v2')

    def test_avif_cycle_restores_original_cache_identity(self):
        textures = ['red.png', 'blue.avif']
        images = {'red.png': png(), 'blue.avif': avif()}
        original = self.run_pack(textures, images)
        self.assertNotEqual(original['render_key'], self.run_pack(textures, images, select='blue')['render_key'])
        self.assertEqual(original, self.run_pack(textures, images, select='red'))

    def test_avif_orientation_updates_physical_texture_size(self):
        report = self.run_pack(['gold.avif'], {'gold.avif': avif('orientation6')})
        uv = report['textures'][0]['uv']
        self.assertEqual((uv['width'], uv['height']), (2, 3))

    def test_upstream_builtin_manifests_all_import(self):
        root = PROJECT / 'vendor/ysm-upstream/src/main/resources/assets/ysm/builtin'
        if not root.is_dir():
            self.skipTest('Optional upstream source checkout is not packaged')
        manifests = sorted(root.rglob('ysm.json'))
        self.assertEqual(len(manifests), 27)
        for manifest in manifests:
            with self.subTest(model=manifest.parent.name):
                run = subprocess.run([str(EXE), str(manifest.parent)], capture_output=True, encoding='utf-8', timeout=10)
                self.assertEqual(run.returncode, 0, run.stderr)
                report = json.loads(run.stdout)
                self.assertGreater(report['texture_count'], 0)


if __name__ == '__main__':
    unittest.main()
