"""Client weather targets after StartGame and independent LevelEvents."""
import json
import subprocess
import unittest

from test_molang import EXE


class WeatherEventTests(unittest.TestCase):
    def events(self, events):
        run = subprocess.run([str(EXE), '--weather-events', json.dumps(events)],
                             capture_output=True, encoding='utf-8', timeout=5)
        self.assertEqual(run.returncode, 0, run.stderr)
        return json.loads(run.stdout)

    def test_join_preserves_weak_native_rain_and_thunder_without_a_followup_event(self):
        states = self.events([{'join': {'rain': 0.05, 'lightning': 0.04998856}}])
        self.assertEqual(states[0]['weather'], 2)
        self.assertEqual(states[0]['rain'], 0.05)
        self.assertEqual(states[0]['lightning'], 0.04998856)
        self.assertEqual(states[0]['source'], 'start_game')
        self.assertEqual(states[0]['events'], 0)

    def test_thunder_command_updates_join_rain_without_replacing_the_rain_channel(self):
        states = self.events([{'join': {'rain': 0.05}}, {'event': 3002, 'data': 3276}])
        self.assertEqual([state['weather'] for state in states], [1, 2])
        self.assertEqual(states[-1]['rain'], 0.05)
        self.assertAlmostEqual(states[-1]['lightning'], 3276 / 65535)

    def test_rain_thunder_and_clear_updates_track_the_two_channels_independently(self):
        states = self.events([{'join': {}}, {'event': 3001, 'data': 65535},
                              {'event': 3002, 'data': 65535}, {'event': 3004},
                              {'event': 3003}])
        self.assertEqual([state['weather'] for state in states], [0, 1, 2, 1, 0])
        self.assertEqual(states[-1]['events'], 4)

    def test_stop_rain_does_not_forget_a_still_active_thunder_channel(self):
        states = self.events([{'join': {'rain': 1, 'lightning': 1}}, {'event': 3003},
                              {'event': 3001, 'data': 3276}, {'event': 3004, 'data': 65535}])
        self.assertEqual([state['weather'] for state in states], [2, 0, 2, 1])
        self.assertEqual(states[1]['lightning'], 1)

    def test_unrelated_level_events_cannot_change_weather_or_initialize_a_world(self):
        states = self.events([{'event': 2001, 'data': 65535}, {'join': {'rain': 1}},
                              {'event': 3005, 'data': 65535}])
        self.assertFalse(states[0]['initialized'])
        self.assertEqual(states[1], states[2])

    def test_leave_and_join_a_clear_world_discard_the_previous_worlds_thunder(self):
        states = self.events([{'join': {'rain': 1, 'lightning': 1}}, {'event': 3002, 'data': 3276},
                              {'reset': True}, {'join': {}}, {'event': 3001, 'data': 65535}])
        self.assertEqual([state['weather'] for state in states], [2, 2, 0, 0, 1])
        self.assertFalse(states[2]['initialized'])
        self.assertEqual(states[3]['events'], 0)
        self.assertEqual(states[3]['last_event'], 0)

    def test_payload_extremes_are_bounded_and_stops_ignore_their_payload(self):
        states = self.events([{'join': {'rain': -4, 'lightning': 8}}, {'event': 3001, 'data': 2147483647},
                              {'event': 3002, 'data': -2147483648}, {'event': 3003, 'data': 65535}])
        self.assertEqual([state['weather'] for state in states], [0, 2, 1, 0])
        self.assertEqual(states[0]['lightning'], 1)
        self.assertEqual(states[1]['rain'], 1)
        self.assertEqual(states[2]['lightning'], 0)


if __name__ == '__main__':
    unittest.main()
