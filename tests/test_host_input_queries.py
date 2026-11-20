"""Source keyboard IDs, held state, menu gates and native mod versions."""
import json
import subprocess
import unittest

from test_molang import EXE
import test_molang as shared


class HostInputQueryTests(unittest.TestCase):
    run_expr = shared.MolangTests.run_expr
    value = shared.MolangTests.value

    def key_events(self, events):
        run = subprocess.run([str(EXE), '--keyboard-events', json.dumps(events)], capture_output=True,
                             encoding='utf-8', timeout=5)
        self.assertEqual(run.returncode, 0, run.stderr)
        return json.loads(run.stdout)

    def test_native_navigation_and_function_keys_use_glfw_ids_in_source_models(self):
        windows = [27, 13, 9, 8, 45, 46, 39, 37, 40, 38, 33, 34, 36, 35, 112, 115, 135]
        expected = [256, 257, 258, 259, 260, 261, 262, 263, 264, 265, 266, 267, 268, 269, 290, 293, 313]
        frames = self.key_events([{'key': key, 'down': True} for key in windows])
        self.assertEqual(frames[-1]['keys'], sorted(expected))

    def test_keypad_punctuation_and_sided_modifiers_are_distinct(self):
        windows = [96, 105, 110, 111, 106, 109, 107, 160, 161, 162, 163, 164, 165, 186, 222]
        expected = [320, 329, 330, 331, 332, 333, 334, 340, 344, 341, 345, 342, 346, 59, 39]
        frames = self.key_events([{'key': key, 'down': True} for key in windows])
        self.assertEqual(frames[-1]['keys'], sorted(expected))

    def test_repeat_release_and_menu_reset_do_not_leave_a_held_query(self):
        frames = self.key_events([{'key': 32, 'down': True}, {'key': 32, 'down': True},
                                  {'key': 32, 'down': False}, {'key': 163, 'down': True}, {'reset': True}])
        self.assertEqual([frame['keys'] for frame in frames], [[32], [32], [], [345], []])

    def test_keyboard_returns_any_requested_held_key_and_refreshes_each_snapshot(self):
        frames = [{'in_game': True, 'pressed_keys': keys} for keys in [[], [65], [256], [66, 340], []]]
        self.assertEqual(self.run_expr('ysm.keyboard(65,66)', {'frames': frames})['values'], [0, 1, 0, 1, 0])

    def test_keyboard_is_false_in_menus_and_bounded_to_source_key_ids(self):
        self.assertEqual(self.value('ysm.keyboard(65)', {'pressed_keys': [65], 'in_game': False}), 0)
        self.assertEqual(self.value('ysm.keyboard(-1,31,349,99999)', {'pressed_keys': [-1, 31, 349, 99999], 'in_game': True}), 0)
        self.assertEqual(self.value('ysm.keyboard(65.9)', {'pressed_keys': [65], 'in_game': True}), 1)

    def test_keyboard_preserves_the_source_scalar_projection_of_object_arguments(self):
        self.assertEqual(self.value("ysm.keyboard('65',65)", {'pressed_keys': [65], 'in_game': True}), 1)
        self.assertEqual(self.value("ysm.keyboard('65')", {'pressed_keys': [65], 'in_game': True}), 0)

    def test_mod_version_returns_exact_strings_and_an_absent_mod_has_a_present_empty_string(self):
        host = {'mod_versions': {'YsmBedrock': '0.12.0', 'ysm': '0.12.0', 'LeviLamina': '26.60.7'}}
        self.assertEqual(self.value("ysm.mod_version('YsmBedrock')", host), '0.12.0')
        self.assertEqual(self.value("ysm.mod_version('missing')??'fallback'", host), '')
        self.assertEqual(self.value("ysm.mod_version('ysm')=='0.12.0'", host), 1)

    def test_mod_version_argument_is_a_string_identifier_and_input_snapshots_can_change(self):
        frames = [{'mod_versions': {'example': value}} for value in ['1.0.0', '2.0.0']]
        self.assertEqual(self.run_expr("ysm.mod_version('example')", {'frames': frames})['values'], ['1.0.0', '2.0.0'])
        self.assertIsNone(self.value('ysm.mod_version(1)', {'mod_versions': {'1': 'hidden'}}))


if __name__ == '__main__':
    unittest.main()
