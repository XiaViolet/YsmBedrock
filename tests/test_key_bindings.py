"""Release hotkeys and ordered native input handling."""
import json
import subprocess
import unittest

from test_molang import EXE


class KeyBindingTests(unittest.TestCase):
    def events(self, events):
        run = subprocess.run([str(EXE), '--input-events', json.dumps(events)],
                             capture_output=True, encoding='utf-8', timeout=5)
        self.assertEqual(run.returncode, 0, run.stderr)
        return json.loads(run.stdout)

    @staticmethod
    def down(key):
        return {'key': key, 'down': True}

    @staticmethod
    def up(key):
        return {'key': key, 'down': False}

    def test_full_keyboard_has_only_two_global_shortcuts(self):
        # Release each key before the next one, including the modifier keys.
        modifiers = ((), (18,), (164,), (165,), (17,), (16,), (91,), (92,),
                     (17, 18), (16, 18), (91, 18), (92, 18))
        for chord in modifiers:
            with self.subTest(chord=chord):
                keys = [key for key in range(256) if key not in chord]
                # Keep command lines below the Windows argument-size limit.
                for start in range(0, len(keys), 64):
                    events = [self.down(modifier) for modifier in chord]
                    expected = ['none'] * len(events)
                    for key in keys[start:start + 64]:
                        events.extend([self.down(key), self.up(key)])
                        action = 'none'
                        if key == ord('Z') and not chord:
                            action = 'roulette'
                        if key == ord('Y') and chord in ((18,), (164,), (165,)):
                            action = 'model_menu'
                        expected.extend([action, 'none'])
                    self.assertEqual(self.events(events), expected)

    def test_held_shortcuts_do_not_reopen_ui_on_key_repeat(self):
        for chord, key, action in [([], 90, 'roulette'), ([18], 89, 'model_menu')]:
            with self.subTest(action=action):
                events = [self.down(code) for code in chord]
                events += [self.down(key), self.down(key), self.up(key), self.down(key)]
                self.assertEqual(self.events(events)[-4:], [action, 'none', 'none', action])

    def test_menu_reset_discards_lost_alt_release(self):
        events = [self.down(18), self.down(89), {'reset': True}, self.down(89),
                  self.up(89), self.down(90)]
        self.assertEqual(self.events(events), ['none', 'model_menu', 'none', 'none', 'none', 'roulette'])

    def test_invalid_keys_and_modifier_pressed_after_y(self):
        events = [self.down(-1), self.down(256), self.down(89), self.down(18), self.down(89)]
        self.assertEqual(self.events(events), ['none'] * len(events))


if __name__ == '__main__':
    unittest.main()
