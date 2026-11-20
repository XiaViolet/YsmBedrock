"""Decode complete image streams through the same codec used by the client."""
import json
import pathlib
import struct
import subprocess
import tempfile
import unittest

from jpeg_fixtures import jpeg
from test_import import EXE
from test_model_textures import png


def without_entropy(data):
    marker = data.index(b'\xff\xda')
    end = marker + 2 + int.from_bytes(data[marker + 2:marker + 4], 'big')
    return data[:end] + b'\xff\xd9'


class ImageCodecTests(unittest.TestCase):
    def decode(self, data, error=None):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / '纹理.bin'
            path.write_bytes(data)
            run = subprocess.run([str(EXE), '--image', str(path)], capture_output=True,
                                 encoding='utf-8', timeout=10)
        if error is not None:
            self.assertEqual(run.returncode, 1, run.stdout)
            self.assertIn(error, run.stderr)
            return
        self.assertEqual(run.returncode, 0, run.stderr)
        return json.loads(run.stdout)

    def check_jpeg(self, data, color):
        report = self.decode(data)
        self.assertEqual((report['format'], report['width'], report['height']), ('JPEG', 3, 2))
        self.assertEqual(report['rgba_bytes'], 24)
        self.assertEqual(report['alpha_min'], 255)
        self.assertEqual(report['alpha_max'], 255)
        self.assertEqual(report['first_pixel'][3], 255)
        for actual, expected in zip(report['first_pixel'][:3], color):
            self.assertLessEqual(abs(actual - expected), 2)

    def test_rgb_jpeg_converts_to_opaque_rgba(self):
        self.check_jpeg(jpeg(), (200, 40, 20))

    def test_progressive_jpeg_reads_all_scans(self):
        data = jpeg('progressive')
        self.assertIn(b'\xff\xc2', data)
        self.check_jpeg(data, (200, 40, 20))

    def test_grayscale_jpeg_expands_all_color_channels(self):
        self.check_jpeg(jpeg('gray'), (100, 100, 100))

    def test_adobe_cmyk_jpeg_applies_black_channel_and_inversion(self):
        self.check_jpeg(jpeg('cmyk'), (207, 198, 190))

    def test_low_precision_lossless_samples_use_authored_range(self):
        self.check_jpeg(jpeg('rgb2'), (170, 0, 0))

    def test_nine_bit_lossless_samples_are_normalized_to_rgba8(self):
        self.check_jpeg(jpeg('rgb9'), (200, 40, 20))

    def test_twelve_bit_dct_samples_are_normalized_to_rgba8(self):
        self.check_jpeg(jpeg('rgb12'), (200, 40, 20))

    def test_twelve_bit_lossless_samples_are_normalized_to_rgba8(self):
        self.check_jpeg(jpeg('rgb12_lossless'), (200, 40, 20))

    def test_sixteen_bit_lossless_samples_are_normalized_to_rgba8(self):
        self.check_jpeg(jpeg('rgb16'), (200, 40, 20))

    def test_arithmetic_coded_jpeg(self):
        self.check_jpeg(jpeg('arithmetic'), (200, 40, 20))

    def test_high_precision_lossless_without_pixel_data_is_rejected(self):
        self.decode(without_entropy(jpeg('rgb16')), error='decode JPEG')

    def test_png_transparency_is_preserved(self):
        report = self.decode(png(rgba=(220, 80, 40, 17)))
        self.assertEqual(report['format'], 'PNG')
        self.assertEqual(report['first_pixel'], [220, 80, 40, 17])
        self.assertEqual((report['alpha_min'], report['alpha_max']), (17, 17))

    def test_jpeg_without_end_marker_is_rejected(self):
        self.decode(jpeg()[:-2], error='decode JPEG')

    def test_jpeg_end_marker_without_pixel_data_is_rejected(self):
        self.decode(without_entropy(jpeg()), error='decode JPEG')

    def test_incomplete_progressive_scan_is_rejected(self):
        self.decode(without_entropy(jpeg('progressive')), error='decode JPEG')

    def test_end_marker_inside_metadata_does_not_hide_truncation(self):
        payload = b'embedded thumbnail end: \xff\xd9'
        data = jpeg()
        app = b'\xff\xe1' + struct.pack('>H', len(payload) + 2) + payload
        self.decode(data[:2] + app + data[2:-2], error='decode JPEG')

    def test_segment_length_past_end_is_rejected(self):
        data = bytearray(jpeg())
        data[4:6] = b'\xff\xff'
        self.decode(bytes(data), error='decode JPEG')

    def test_oversized_jpeg_dimensions_are_rejected_before_pixel_allocation(self):
        data = bytearray(jpeg())
        frame = data.index(b'\xff\xc0')
        data[frame + 7:frame + 9] = struct.pack('>H', 4097)
        self.decode(bytes(data), error='oversized texture dimensions')

    def test_valid_jpeg_may_have_trailing_bytes(self):
        self.check_jpeg(jpeg() + b'original trailing metadata', (200, 40, 20))

    def test_unrecognized_format_is_not_passed_to_another_decoder(self):
        self.decode(b'GIF89a' + b'\0' * 32, error='invalid texture format')

    def test_too_short_jpeg_is_rejected_without_crashing(self):
        self.decode(b'\xff\xd8\xff', error='decode JPEG')

    def test_encoded_image_input_is_bounded(self):
        self.decode(b'\0' * (32 * 1024 * 1024 + 1), error='Encoded texture too large')


if __name__ == '__main__':
    unittest.main()
