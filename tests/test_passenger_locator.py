"""Seat matrices measured in the 2.6.5 release with the bundled unmodified Wine Fox."""
import json
import pathlib
import subprocess
import tempfile
import unittest
import zipfile

from inspector import EXE as INSPECT


class PassengerLocatorTests(unittest.TestCase):
    def sample(self, geometry, frames):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            rig, inputs = root / "rig.json", root / "inputs.json"
            rig.write_bytes(geometry)
            inputs.write_text(json.dumps(frames), encoding="utf-8")
            return json.loads(subprocess.check_output([INSPECT, "--passenger-locators", rig, inputs]))

    def test_wine_fox_driver_yaw_second_seat_and_no_locator(self):
        package = pathlib.Path(__file__).resolve().parents[1] / "assets/ysm-wine-fox.zip"
        frames = [dict(bones={}, seat=seat, yaw=yaw, vehicle_minus_rider=[0, -0.84375, 0])
                  for seat, yaw in [(0, 0), (0, 90), (1, 0), (2, 0), (-1, 0)]]
        with zipfile.ZipFile(package) as model:
            matrices = self.sample(model.read("models/foxcar.json"), frames)
        for matrix, offset in zip(matrices, [(0.55790436, -0.6216519, -0.01646625),
                                              (0.01646625, -0.6216518, 0.5579043),
                                              (-0.55790436, -0.6216519, -0.01646625)]):
            for actual, expected in zip(matrix[12:15], offset):
                self.assertAlmostEqual(actual, expected, places=5)
        self.assertEqual(matrices[3:], [None, None])

    def test_rotating_parent_transforms_riding_height_and_passenger_orientation(self):
        rig = {"minecraft:geometry": [{"bones": [{"name": "base"}, {"name": "PassengerLocator", "parent": "base"}]}]}
        frame = dict(seat=0, yaw=180, vehicle_minus_rider=[0, -1, 0],
                     bones={"base": {"rotation": [0, 0, 90], "scale": [1, 2, 1]}})
        custom, vanilla = self.sample(json.dumps(rig).encode(), [frame, dict(frame, custom_player=False)])
        for matrix, x in [(custom, 3), (vanilla, 2)]:
            self.assertAlmostEqual(matrix[12], x, places=5)
            self.assertAlmostEqual(matrix[13], 0, places=5)
            self.assertAlmostEqual(matrix[1], 1, places=5)
            self.assertAlmostEqual(matrix[4], -2, places=5)

