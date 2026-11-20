"""Source string-pool identity must survive sampling, queries and native renaming."""
import json
import pathlib
import subprocess
import tempfile
import unittest

import test_molang
import test_model_library
from test_import import EXE


class BoneIdentityTests(unittest.TestCase):
    animation = test_molang.MolangTests.animation
    value = test_molang.MolangTests.value
    run_expr = test_molang.MolangTests.run_expr

    def test_case_twins_keep_all_three_authored_channels(self):
        bones = {'Arm': {'position': [10, 0, 0], 'rotation': [1, 2, 3], 'scale': [2, 3, 4]},
                 'arm': {'position': [20, 0, 0], 'rotation': [4, 5, 6], 'scale': [5, 6, 7]}}
        result = self.animation({'idle': {'loop': True, 'bones': bones}})
        self.assertEqual(result['expression_failures'], 0)
        self.assertEqual(result['pose'], bones)

    def test_bone_query_matches_exact_string_while_function_names_keep_aliases(self):
        host = {'bone_rotations': {'Arm': [10, 0, 0], 'arm': [20, 0, 0]}}
        self.assertEqual(self.value("YSM.BONE_ROT('Arm').x*100+ysm.bone_rot('arm').x", host), 1020)
        self.assertEqual(self.value("ysm.bone_rot('ARM').x", host), 0)

    def player(self, functions=None):
        geometry = {'minecraft:geometry': [{'bones': [
            {'name': 'Arm', 'rotation': [1, 2, 3]},
            {'name': 'arm', 'rotation': [4, 5, 6]}]}]}
        doc = {'animations': {'idle': {'loop': True, 'bones': {
            'Arm': {'rotation': [10, 20, 30]}, 'arm': {'rotation': [40, 50, 60]}}}},
            'ysm_functions': functions or {}}
        plan = {'geometry': geometry, 'frames': [{'time': 0}, {'time': .2}, {'time': .4}]}
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'case.json'
            path.write_text(json.dumps(doc), encoding='utf-8')
            run = subprocess.run([str(EXE), '--source-player-animation-file', str(path), json.dumps(plan)],
                                 capture_output=True, encoding='utf-8', timeout=5)
        self.assertEqual(run.returncode, 0, run.stderr)
        result = json.loads(run.stdout)
        self.assertEqual(result['expression_failures'], 0)
        return result['frames']

    def test_geometry_and_completed_rotation_queries_keep_separate_case_twins(self):
        frames = self.player({'@player_update': "v.upper=ysm.bone_rot('Arm');v.lower=ysm.bone_rot('arm');"})
        self.assertEqual(frames[0]['variables']['variable.upper'], {'x': 1, 'y': 2, 'z': 3})
        self.assertEqual(frames[0]['variables']['variable.lower'], {'x': 4, 'y': 5, 'z': 6})
        self.assertEqual(frames[-1]['variables']['variable.upper'], {'x': 11, 'y': 22, 'z': 33})
        self.assertEqual(frames[-1]['variables']['variable.lower'], {'x': 44, 'y': 55, 'z': 66})

    def test_case_twins_receive_independent_visual_effects(self):
        frame = self.player({'@player_init': "ysm.bone_color('Arm',10,20,30);"
            "ysm.bone_color('arm',40,50,60);ysm.bone_glow('Arm',8);"
            "ysm.bone_transparency('arm',90);"})[-1]
        self.assertEqual(frame['bone_visuals']['Arm'], {'color': [10, 20, 30], 'glow': 8, 'alpha': 255})
        self.assertEqual(frame['bone_visuals']['arm'], {'color': [40, 50, 60], 'glow': -1, 'alpha': 90})

    def test_wrong_case_visual_target_remains_missing(self):
        frame = self.player({'@player_init': "ysm.bone_color('ARM',0,0,0);"
            "ysm.bone_glow('ARM',15);ysm.bone_transparency('ARM',0);"})[-1]
        self.assertEqual(set(frame['bone_visuals']), {'Arm', 'arm'})
        for visual in frame['bone_visuals'].values():
            self.assertEqual(visual, {'color': [255, 255, 255], 'glow': -1, 'alpha': 255})

    def test_native_body_and_arm_renaming_keeps_exact_source_identity(self):
        builder = test_model_library.ModelLibraryTests()
        builder.setUp()
        self.addCleanup(builder.temporary.cleanup)
        package = builder.source('case', folder=True)
        geometry = {'minecraft:geometry': [{'description': {'texture_width': 2, 'texture_height': 2},
            'bones': [{'name': 'Root'}, {'name': 'Arm', 'parent': 'Root'}, {'name': 'arm', 'parent': 'Arm'},
                      {'name': 'RightArm', 'parent': 'Root'}, {'name': 'Wrist', 'parent': 'RightArm'},
                      {'name': 'wrist', 'parent': 'Wrist'}]}]}
        for name in ['main.json', 'arm.json']:
            (package / name).write_text(json.dumps(geometry), encoding='utf-8')
        run = subprocess.run([str(EXE), '--client-geometry', str(package)],
                             capture_output=True, encoding='utf-8', timeout=5)
        self.assertEqual(run.returncode, 0, run.stderr)
        result = json.loads(run.stdout)
        bones = {b['name']: b for b in result['geometry']['minecraft:geometry'][0]['bones']}
        for mapping, twins in [('body_bones', ['Arm', 'arm']), ('arm_bones', ['Wrist', 'wrist'])]:
            source = {v: k for k, v in result[mapping].items()}
            upper, lower = [source[name] for name in twins]
            self.assertNotEqual(upper, lower)
            self.assertEqual(bones[lower]['parent'], upper)


if __name__ == '__main__':
    unittest.main()
