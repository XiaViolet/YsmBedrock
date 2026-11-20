"""Decode static AVIF items with alpha, transforms and complete box validation."""
import struct
import unittest

import test_image_codec as shared
from avif_fixtures import avif, REFERENCE_PIXELS
from avif_upstream_fixtures import upstream_avif, REFERENCE


def box(kind, payload=b''):
    return struct.pack('>I', len(payload) + 8) + kind + payload


def replace_brands(data, old, new):
    data = bytearray(data)
    end = int.from_bytes(data[:4], 'big')
    for offset in range(8, end, 4):
        if offset != 12 and data[offset:offset + 4] == old:
            data[offset:offset + 4] = new
    return data


class AvifCodecTests(unittest.TestCase):
    decode = shared.ImageCodecTests.decode

    def compare_pixels(self, kind, tolerance=2):
        report = self.decode(avif(kind))
        expected = REFERENCE_PIXELS[kind]
        self.assertEqual(report['format'], 'AVIF')
        self.assertEqual((report['width'], report['height']), (expected['width'], expected['height']))
        self.assertEqual(report['rgba_bytes'], expected['width'] * expected['height'] * 4)
        self.assertEqual(len(report['pixels']), len(expected['pixels']))
        for index, (actual, wanted) in enumerate(zip(report['pixels'], expected['pixels'])):
            self.assertLessEqual(abs(actual - wanted), 0 if index % 4 == 3 else tolerance, (kind, index))

    def test_rgb_is_converted_to_opaque_rgba(self):
        self.compare_pixels('rgb')

    def test_straight_alpha_is_not_premultiplied(self):
        self.compare_pixels('rgba')

    def test_premultiplied_input_is_unpremultiplied(self):
        report = self.decode(avif('premultiplied'))
        self.assertEqual((report['alpha_min'], report['alpha_max']), (128, 128))
        # Independent expected authored color, allowing YUV and alpha rounding.
        for actual, expected in zip(report['first_pixel'], (200, 40, 20, 128)):
            self.assertLessEqual(abs(actual - expected), 3)

    def test_fully_transparent_input_retains_authored_rgb(self):
        self.compare_pixels('transparent')

    def test_grayscale_color_is_preserved(self):
        self.compare_pixels('gray')

    def test_all_eight_orientations_match_independent_full_pixel_references(self):
        for orientation in range(1, 9):
            with self.subTest(orientation=orientation):
                self.compare_pixels('orientation%d' % orientation)

    def test_idat_and_progressive_stills_decode_final_image(self):
        for kind in ('idat', 'idat_size_zero', 'progressive', 'progressive_size_zero'):
            with self.subTest(kind=kind):
                report = self.decode(upstream_avif(kind))
                expected = REFERENCE[kind]
                self.assertEqual((report['width'], report['height']), (expected['width'], expected['height']))
                for actual, wanted in zip(report['first_pixel'], expected['first_pixel']):
                    self.assertLessEqual(abs(actual - wanted), 2)
                self.assertEqual([report['alpha_min'], report['alpha_max']], expected['alpha_range'])

    def test_clean_aperture_crops_pixels_and_physical_dimensions(self):
        report = self.decode(upstream_avif('crop'))
        expected = REFERENCE['crop']
        self.assertEqual((report['width'], report['height'], report['rgba_bytes']), (180, 100, 72000))
        for actual, wanted in zip(report['first_pixel'], expected['first_pixel']):
            self.assertLessEqual(abs(actual - wanted), 2)

    def test_twelve_bit_av1_still_is_normalized_to_rgba8(self):
        data = upstream_avif('twelve_bit_still')
        self.assertEqual(data[data.index(b'av1C') + 6] & 0x60, 0x60)
        report = self.decode(data)
        self.assertEqual((report['width'], report['height'], report['rgba_bytes']), (64, 64, 16384))
        for actual, wanted in zip(report['first_pixel'], REFERENCE['twelve_bit_still']['first_pixel']):
            self.assertLessEqual(abs(actual - wanted), 2)

    def test_animated_sequence_is_rejected_in_static_profile(self):
        self.decode(avif('animated'), error='Animated AVIF')

    def test_movie_track_is_rejected_even_when_sequence_brand_is_removed(self):
        self.decode(replace_brands(avif('animated'), b'avis', b'avif'), error='Animated AVIF')

    def test_truncated_file_is_rejected(self):
        self.decode(avif()[:-1], error='AVIF container')

    def test_complete_pixels_do_not_hide_truncated_trailing_box(self):
        self.decode(avif() + box(b'free', b'abc')[:-1], error='AVIF container')

    def test_truncated_trailing_header_is_rejected(self):
        self.decode(avif() + b'free', error='AVIF container')

    def test_unknown_complete_box_is_ignored(self):
        report = self.decode(avif() + box(b'free', b'future metadata'))
        self.assertEqual(report['first_pixel'], self.decode(avif())['first_pixel'])

    def test_extended_size_unknown_box_is_accepted(self):
        self.decode(avif() + struct.pack('>I4sQ', 1, b'free', 19) + b'abc')

    def test_extended_size_cannot_overflow_or_run_past_eof(self):
        self.decode(avif() + struct.pack('>I4sQ', 1, b'free', 2**64 - 1), error='AVIF container')

    def test_extended_box_size_must_include_its_header(self):
        self.decode(avif() + struct.pack('>I4sQ', 1, b'free', 8), error='AVIF container')

    def test_zero_size_final_media_box_uses_remaining_bytes(self):
        data = bytearray(avif())
        media = data.index(b'mdat')
        data[media - 4:media] = b'\0' * 4
        self.decode(data)

    def test_duplicate_file_type_box_is_rejected(self):
        data = avif()
        self.decode(data + data[:int.from_bytes(data[:4], 'big')], error='AVIF container')

    def test_encoded_media_corruption_is_not_replaced_with_blank_pixels(self):
        data = bytearray(avif())
        media = data.index(b'mdat') + 4
        data[media:] = b'\0' * (len(data) - media)
        self.decode(data, error='decode AVIF')

    def test_invalid_clean_aperture_denominator_is_rejected(self):
        data = bytearray(upstream_avif('crop'))
        data[data.index(b'clap') + 8:data.index(b'clap') + 12] = b'\0' * 4
        self.decode(data, error='AVIF')

    def test_oversized_image_is_rejected_by_parser_before_decode(self):
        self.decode(avif('wide'), error='decode AVIF')

    def test_foreign_heic_brand_is_not_guessed_as_avif(self):
        self.decode(replace_brands(avif(), b'avif', b'heic'), error='invalid texture format')


if __name__ == '__main__':
    unittest.main()
