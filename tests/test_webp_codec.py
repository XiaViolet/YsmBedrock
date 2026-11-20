"""Verify complete static WebP assets, rather than accepting partial VP8 fragments."""
import struct
import unittest

import test_image_codec as shared
from webp_fixtures import webp


def chunk(kind, data):
    return kind + struct.pack('<I', len(data)) + data + (b'\0' if len(data) & 1 else b'')


def riff(chunks):
    return b'RIFF' + struct.pack('<I', len(chunks) + 4) + b'WEBP' + chunks


def extended(width=3, height=2, flags=0):
    return chunk(b'VP8X', bytes([flags]) + b'\0' * 3 + (width - 1).to_bytes(3, 'little') + (height - 1).to_bytes(3, 'little'))


class WebpCodecTests(unittest.TestCase):
    decode = shared.ImageCodecTests.decode

    def check_color(self, data, alpha=255, tolerance=0):
        report = self.decode(data)
        self.assertEqual((report['format'], report['width'], report['height']), ('WEBP', 3, 2))
        self.assertEqual(report['rgba_bytes'], 24)
        self.assertEqual((report['alpha_min'], report['alpha_max']), (alpha, alpha))
        for actual, expected in zip(report['first_pixel'], (200, 40, 20, alpha)):
            self.assertLessEqual(abs(actual - expected), tolerance)

    def test_lossless_rgb_is_expanded_to_rgba(self):
        self.check_color(webp())

    def test_semitransparent_color_is_not_premultiplied(self):
        self.check_color(webp('rgba'), alpha=17)

    def test_fully_transparent_pixel_retains_authored_color(self):
        self.check_color(webp('transparent'), alpha=0)

    def test_lossy_rgb(self):
        self.check_color(webp('lossy'), tolerance=2)

    def test_lossy_image_with_separate_alpha_chunk(self):
        self.check_color(webp('lossy_alpha'), alpha=17, tolerance=2)

    def test_animated_webp_is_rejected_in_static_model_profile(self):
        self.decode(webp('animated'), error='Animated WebP')

    def test_animation_flag_without_animation_chunks_is_not_silently_ignored(self):
        self.decode(riff(extended(flags=2) + webp()[12:]), error='Animated WebP')

    def test_riff_declaring_missing_bytes_is_rejected(self):
        self.decode(webp()[:-1], error='WebP container')

    def test_maximum_riff_length_does_not_overflow(self):
        data = bytearray(webp())
        data[4:8] = b'\xfe\xff\xff\xff'
        self.decode(data, error='WebP container')

    def test_truncated_chunk_is_rejected_even_with_adjusted_riff_length(self):
        self.decode(riff(webp()[12:-2]), error='WebP container')

    def test_missing_pixels_are_not_replaced_with_blank_image(self):
        self.decode(riff(chunk(b'VP8L', webp()[20:25])), error='decode WebP')

    def test_incomplete_metadata_after_complete_pixels_is_rejected(self):
        self.decode(riff(webp()[12:] + b'EXIF' + struct.pack('<I', 80) + b'bad'), error='WebP container')

    def test_incomplete_chunk_header_after_complete_pixels_is_rejected(self):
        self.decode(riff(webp()[12:] + b'XY'), error='WebP container')

    def test_chunk_padding_must_be_present(self):
        self.decode(riff(chunk(b'VP8L', webp()[20:37])[:-1]), error='WebP container')

    def test_nonzero_chunk_padding_is_rejected(self):
        data = bytearray(webp())
        data[-1] = 9
        self.decode(data, error='WebP container')

    def test_duplicate_static_images_are_rejected(self):
        self.decode(riff(webp()[12:] + webp()[12:]), error='WebP container')

    def test_unknown_padded_chunk_is_ignored(self):
        self.check_color(riff(webp()[12:] + chunk(b'FUTR', b'abc')))

    def test_canvas_dimensions_must_match_encoded_pixels(self):
        self.decode(riff(extended(width=4) + webp()[12:]), error='WebP container')

    def test_oversized_dimensions_are_rejected_before_pixel_allocation(self):
        self.decode(webp('wide'), error='oversized texture dimensions')

    def test_trailing_bytes_do_not_become_part_of_riff(self):
        self.check_color(webp() + b'original trailing metadata')

    def test_headerless_vp8_fragment_is_not_a_model_image(self):
        self.decode(webp()[12:], error='invalid texture format')


if __name__ == '__main__':
    unittest.main()
