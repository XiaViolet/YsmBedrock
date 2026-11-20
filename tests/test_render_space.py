"""Compare native rendering composition with YSM's authored coordinate space."""
import hashlib
import itertools
import json
import math
import pathlib
import subprocess
import unittest

from test_import import EXE, SAMPLE


def multiply(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(3)) for c in range(3)] for r in range(3)]


def rotate(axis, degrees):
    c, s = math.cos(math.radians(degrees)), math.sin(math.radians(degrees))
    if axis == 'y':
        return [[c, 0, s], [0, 1, 0], [-s, 0, c]]
    return [[c, -s, 0], [s, c, 0], [0, 0, 1]]


BASIS = [[-1, 0, 0], [0, -1, 0], [0, 0, 1]]


class RenderSpaceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.original_hash = hashlib.sha256(SAMPLE.read_bytes()).hexdigest()
        cls.engine = json.loads((pathlib.Path(__file__).parent / 'fixtures/native_bone_basis_26_60_05.json').read_text())

    @classmethod
    def tearDownClass(cls):
        assert hashlib.sha256(SAMPLE.read_bytes()).hexdigest() == cls.original_hash

    def invoke(self, inputs):
        run = subprocess.run([str(EXE), '--native-render-space', json.dumps(inputs)],
                             capture_output=True, encoding='utf-8')
        self.assertEqual(run.returncode, 0, run.stderr)
        return json.loads(run.stdout)

    def assert_matrix_equal(self, actual, expected):
        for row in range(3):
            for column in range(3):
                self.assertAlmostEqual(actual[row][column], expected[row][column], places=10)

    def native_world_position(self, logical):
        # These basis vectors were produced by the actual client's leaf
        # BoneOrientation routine, rather than an assumed translation matrix.
        basis = [self.engine['cases']['position_' + axis][12:15] for axis in 'xyz']
        model = [sum(logical[a] * basis[a][r] for a in range(3)) for r in range(3)]
        return [-model[0], -model[1], model[2]]

    def test_replicated_ground_waits_for_the_visible_projectile_to_reach_the_block(self):
        # The recorded Alice shots stayed at their release position after the
        # ground bit arrived, then interpolated to these server collision points.
        release = [100.5, 152.52, 100.5]
        for contact, intermediate in [([103.89, 151, 112.771], [102.675, 152.709, 108.371]),
                                      ([105.66, 152.681, 118], [104.503, 152.71, 114.074])]:
            block = {"embedded": True, "position": contact}
            cases = [{"native_ground": True, "block_contact": block, "render_position": p}
                     for p in (release, intermediate, contact)]
            cases.extend([{"native_ground": False, "block_contact": block, "render_position": contact},
                          {"native_ground": True, "block_contact": {"embedded": False}, "render_position": contact},
                          {"native_ground": True, "render_position": contact}])
            self.assertEqual([x["in_ground"] for x in self.invoke(cases)], [False, False, True, False, False, True])

    def test_full_native_projectile_transform_matches_upstream_y_then_z(self):
        cases = []
        for yaw, pitch, body in itertools.product([-179, -120, -30, 0, 40, 90, 179],
                                                  [-85, -30, 0, 20, 85], [0, 17, -109, 157]):
            cases.append({'previous_yaw': yaw, 'yaw': yaw, 'previous_pitch': pitch,
                          'pitch': pitch, 'render_body_yaw': body})
        for start in range(0, len(cases), 40):
            for case, output in zip(cases[start:start + 40], self.invoke(cases[start:start + 40])):
                # Native renderer first applies its body yaw and XY reflection.
                # The last BASIS converts a source YSM point to native space.
                actual = multiply(multiply(multiply(multiply(
                    rotate('y', 180 - case['render_body_yaw']), BASIS),
                    rotate('y', output['yaw'])), rotate('z', output['pitch'])), BASIS)
                # GeoProjectilesRenderer's exact source order and angle offset.
                expected = multiply(rotate('y', case['yaw'] - 90), rotate('z', case['pitch']))
                self.assert_matrix_equal(actual, expected)

    def test_different_render_body_yaw_does_not_change_final_shot_direction(self):
        cases = [{'yaw': 41, 'previous_yaw': 41, 'pitch': 63, 'previous_pitch': 63,
                  'render_body_yaw': body} for body in [-171, 0, 41, 93]]
        for case, output in zip(cases, self.invoke(cases)):
            native = multiply(multiply(rotate('y', 180 - case['render_body_yaw']), BASIS),
                              multiply(rotate('y', output['yaw']), rotate('z', output['pitch'])))
            self.assert_matrix_equal(multiply(native, BASIS),
                                     multiply(rotate('y', 41 - 90), rotate('z', 63)))

    def test_interpolation_uses_upstream_linear_angles_without_shortest_path(self):
        cases = [{'previous_yaw': 179, 'yaw': -179, 'previous_pitch': -60, 'pitch': 60,
                  'alpha': alpha, 'render_body_yaw': -27} for alpha in [0, .25, .5, .75, 1]]
        for case, output in zip(cases, self.invoke(cases)):
            actual = multiply(multiply(multiply(multiply(rotate('y', 207), BASIS),
                rotate('y', output['yaw'])), rotate('z', output['pitch'])), BASIS)
            expected = multiply(rotate('y', (1 - case['alpha']) * 179 + case['alpha'] * -179 - 90),
                                rotate('z', (1 - case['alpha']) * -60 + case['alpha'] * 60))
            self.assert_matrix_equal(actual, expected)

    def test_frame_alpha_is_clamped_to_native_tick_endpoints(self):
        cases = [{'previous_yaw': 10, 'yaw': 40, 'previous_pitch': 5, 'pitch': 20,
                  'alpha': alpha} for alpha in [-1, 0, 1, 2]]
        results = self.invoke(cases)
        self.assertEqual(results[0], results[1])
        self.assertEqual(results[2], results[3])

    def test_authored_positions_match_java_mirrored_x_after_native_basis_change(self):
        positions = [[0, -10.3, 0], [0, 1, 0], [2, -3, 4], [-5, 6, -7]]
        for source, native in zip(positions, self.invoke([{'position': p} for p in positions])):
            actual = self.native_world_position(native['position'])
            self.assertEqual(actual, [-source[0], source[1], source[2]])

    def test_original_eri_stationary_and_moving_crouch_keep_source_vertical_offsets(self):
        positions = []
        for clip, time in [('sneaking', 0), ('sneak', 0), ('sneak', .125), ('sneak', .25)]:
            run = subprocess.run([str(EXE), str(SAMPLE), clip, str(time)],
                                 capture_output=True, encoding='utf-8')
            self.assertEqual(run.returncode, 0, run.stderr)
            pose = json.loads(run.stdout)['pose']
            positions.extend([pose['root']['position'], pose['AllBody']['position']])
        self.assertAlmostEqual(positions[0][1], -10.3)
        for source, native in zip(positions, self.invoke([{'position': p} for p in positions])):
            self.assertAlmostEqual(self.native_world_position(native['position'])[1], source[1])

    def test_native_engine_already_negates_logical_y_translation(self):
        self.assertEqual(self.engine['cases']['position_y'][12:15], [0, -1, 0])

    def test_source_player_offset_is_world_only_and_outside_authored_model_scale(self):
        cases = [{'world_body': world, 'crouching': crouching}
                 for world, crouching in [(True, False), (True, True), (False, True), (False, False)]]
        results = self.invoke(cases)
        for actual, expected in zip(results, [.01, -.115, 0, 0]):
            self.assertAlmostEqual(actual['player_y_offset'], expected)
