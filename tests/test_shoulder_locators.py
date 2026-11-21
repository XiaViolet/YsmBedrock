"""Java 2.6.5 applies one exact endpoint and all its animated ancestors per side."""
import json
import pathlib
import subprocess
import tempfile
import unittest

from inspector import EXE as INSPECT


class ShoulderLocatorTests(unittest.TestCase):
    def test_occupied_side_survives_native_seat_compaction(self):
        frames = [[101], [101, 303], [303], [303, 707], [707, 303], [707], [], [303]]
        rows = json.loads(subprocess.check_output(
            [INSPECT, "--shoulder-passengers", json.dumps(frames)], encoding="utf-8"))
        self.assertEqual(rows, [[101, 0], [101, 303], [0, 303], [707, 303],
                                [707, 303], [707, 0], [0, 0], [303, 0]])

    def test_restored_right_precedes_compacted_native_seats(self):
        frames = [[202], {"riders": [202, 303], "saved": [0, 202]},
                  {"riders": [303, 202], "saved": [303, 202]}, [202], []]
        rows = json.loads(subprocess.check_output(
            [INSPECT, "--shoulder-passengers", json.dumps(frames)], encoding="utf-8"))
        self.assertEqual(rows, [[202, 0], [303, 202], [303, 202], [0, 202], [0, 0]])

    def test_relay_uses_current_passengers_and_yields_to_local_save(self):
        frames = [
            {"riders": [27], "remote": [0, 27]},
            {"riders": [27, 44], "remote": [99, 27]},
            {"riders": [27, 44], "remote": [27, 44], "saved": [0, 27]},
            {"riders": [44], "remote": [0, 27]},
        ]
        rows = json.loads(subprocess.check_output(
            [INSPECT, "--shoulder-passengers", json.dumps(frames)], encoding="utf-8"))
        self.assertEqual(rows, [[0, 27], [44, 27], [44, 27], [44, 0]])

    def test_world_path_and_empty_side(self):
        bones = [
            {"name": "root", "pivot": [0, 16, 0]},
            {"name": "LeftShoulderLocator2", "parent": "root", "pivot": [800, 900, 0]},
            {"name": "RightShoulderLocator2", "parent": "root", "pivot": [800, 900, 0]},
            {"name": "LeftShoulderLocator", "parent": "root", "pivot": [4, 24, 0]},
        ]
        geometry = {"minecraft:geometry": [{"bones": bones}]}
        frames = [
            {"left": True, "bones": {}},
            {"left": False, "bones": {}},
            {"left": True, "bones": {"root": {"position": [0, 4, 0], "rotation": [0, 0, 90], "scale": [2, 2, 2]}}},
            {"left": True, "bones": {"root": {"scale": [0, 0, 0]}}},
        ]
        with tempfile.TemporaryDirectory() as name:
            folder = pathlib.Path(name)
            rig, inputs = folder / "rig.json", folder / "frames.json"
            rig.write_text(json.dumps(geometry), encoding="utf-8")
            inputs.write_text(json.dumps(frames), encoding="utf-8")
            result = json.loads(subprocess.check_output([INSPECT, "--shoulder-locators", rig, inputs], encoding="utf-8"))
        self.assertEqual(result[0][12:15], [-0.25, 1.5, 0])
        self.assertIsNone(result[1])
        for actual, expected in zip(result[2][12:15], [-1, 0.75, 0]):
            self.assertAlmostEqual(actual, expected, places=5)
        self.assertEqual(result[3][:12], [0] * 12)

