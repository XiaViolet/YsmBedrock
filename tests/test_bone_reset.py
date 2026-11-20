import json
import math
import pathlib
import subprocess
import tempfile
import unittest

import test_controllers
from test_import import EXE


class BoneResetTests(unittest.TestCase):
    def sample(self, frames, channels=None, geometry=None, extra_clips=None, extra_controllers=None):
        clip = {'loop': True, 'animation_length': 10,
                'bones': {'root': channels or {'position': [12, 6, -3], 'rotation': [0, 0, 90], 'scale': [3, 2, 0]}},
                'timeline': {'0': 'v.events+=1;'}}
        controllers = {'player.pre_main': {'states': {'default': {'animations': [{'a': 'q.active'}]}}}}
        controllers.update(extra_controllers or {})
        clips = {'a': clip}
        clips.update(extra_clips or {})
        return test_controllers.ControllerTests().invoke(controllers, clips, frames, player=True, geometry=geometry)

    @staticmethod
    def active(time, value=0, **kwargs):
        return dict({'time': time, 'queries': {'q.active': value}}, **kwargs)

    def test_undriven_channels_return_to_default_over_three_ticks(self):
        frames = self.sample([self.active(0, 1), self.active(1, 1), self.active(1.03),
                              self.active(1.075), self.active(1.075), self.active(1.16)])
        half = frames[3]['pose']['root']
        for actual, expected in zip(half['position'], [6, 3, -1.5]):
            self.assertAlmostEqual(actual, expected)
        for actual, expected in zip(half['scale'], [2, 1.5, 0.5]):
            self.assertAlmostEqual(actual, expected)
        self.assertAlmostEqual(half['rotation'][2], 45)
        self.assertEqual(frames[3]['pose'], frames[4]['pose'])
        self.assertNotIn('root', frames[5]['pose'])
        self.assertEqual([f['variables']['variable.events'] for f in frames], [1] * len(frames))

    def test_channels_reset_independently_while_position_remains_driven(self):
        frames = self.sample([self.active(0, 1), self.active(1, 1), self.active(1.075), self.active(1.2)],
                             extra_clips={'position': {'loop': True, 'bones': {'root': {'position': [99, 0, 0]}}}},
                             extra_controllers={'player.post_main': {'states': {'default': {'animations': ['position']}}}})
        self.assertEqual(frames[2]['pose']['root']['position'], [99, 0, 0])
        self.assertAlmostEqual(frames[2]['pose']['root']['rotation'][2], 45)
        self.assertEqual(frames[3]['pose']['root'], {'position': [99, 0, 0], 'rotation': [0, 0, 0], 'scale': [1, 1, 1]})

    def test_reactivation_cancels_the_old_reset_interval(self):
        frames = self.sample([self.active(0, 1), self.active(1, 1), self.active(1.075),
                              self.active(1.08, 1), self.active(1.155)])
        self.assertAlmostEqual(frames[3]['pose']['root']['position'][0], 12)
        self.assertAlmostEqual(frames[4]['pose']['root']['position'][0], 6)
        self.assertAlmostEqual(frames[4]['pose']['root']['rotation'][2], 45)

    def test_large_gap_retires_inactive_channels_without_replaying_instructions(self):
        frames = self.sample([self.active(0, 1), self.active(1, 1), self.active(100)])
        self.assertNotIn('root', frames[-1]['pose'])
        self.assertEqual(frames[-1]['variables']['variable.events'], 1)

    def test_rewind_clears_previous_bone_reset_history(self):
        frames = self.sample([self.active(0, 1), self.active(1, 1), self.active(1.075), self.active(0)])
        self.assertNotIn('root', frames[-1]['pose'])

    def test_rotation_reset_follows_shortest_quaternion_arc(self):
        frames = self.sample([self.active(0, 1), self.active(1, 1), self.active(1.075)],
                             channels={'rotation': [0, 0, 270]})
        self.assertAlmostEqual(frames[-1]['pose']['root']['rotation'][2], -45)

    def test_rotation_reset_uses_nlerp_rather_than_linear_euler_angles(self):
        frames = self.sample([self.active(0, 1), self.active(1, 1), self.active(1.03)],
                             channels={'rotation': [0, 0, 120]})
        expected = math.degrees(2 * math.atan2(0.8 * math.sin(math.pi / 3), 0.2 + 0.8 * math.cos(math.pi / 3)))
        self.assertAlmostEqual(frames[-1]['pose']['root']['rotation'][2], expected)
        self.assertNotAlmostEqual(expected, 120 * 0.8)

    def test_declared_initial_rotation_participates_in_reset(self):
        geometry = {'minecraft:geometry': [{'bones': [{'name': 'root', 'rotation': [0, 0, 60]}]}]}
        frames = self.sample([self.active(0, 1), self.active(1, 1), self.active(1.075)],
                             channels={'rotation': [0, 0, 270]}, geometry=geometry)
        self.assertAlmostEqual(frames[-1]['pose']['root']['rotation'][2], -45)

    def test_controller_retirement_preserves_last_driven_pose_for_global_reset(self):
        clips = {'a': {'animation_length': 1, 'bones': {'root': {'position': [12, 0, 0]}}}}
        ctrl = {'player.main': {'states': {'default': {'animations': ['a']}}}}
        frames = test_controllers.ControllerTests().invoke(ctrl, clips,
            [{'time': t} for t in [0, 0.5, 1, 1.075, 1.151, 1.23]], player=True)
        self.assertAlmostEqual(frames[3]['pose']['root']['position'][0], 3)
        self.assertAlmostEqual(frames[4]['pose']['root']['position'][0], 3 * (1 - 0.076 / 0.15))
        self.assertNotIn('root', frames[5]['pose'])

    def test_head_observations_use_fresh_angles_without_repeating_events(self):
        geometry = {'minecraft:geometry': [{'bones': [{'name': 'Head'}]}]}
        ctrl = {'player.main': {'states': {'default': {'animations': ['a']}}}}
        clips = {'a': {'loop': True, 'animation_length': 10, 'bones': {'Head': {'rotation': [10, 0, 0]}},
                       'timeline': {'0': 'v.n+=1;'}}}
        frames = test_controllers.ControllerTests().invoke(ctrl, clips,
            [{'time': 0, 'queries': {'ysm.head_pitch': -30}},
             {'time': 0, 'queries': {'ysm.head_pitch': -50}},
             {'time': 0, 'queries': {'ysm.head_pitch': -50}}], player=True, geometry=geometry)
        self.assertEqual([f['pose']['Head']['rotation'][0] for f in frames], [40, 60, 60])
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 1, 1])

    def test_head_camera_angle_does_not_enter_authored_beginning_transition(self):
        geometry = {'minecraft:geometry': [{'bones': [{'name': 'Head'}]}]}
        ctrl = {'player.main': {'states': {'default': {'animations': ['a'], 'transitions': [{'b': 'q.go'}]},
                                        'b': {'animations': ['b'], 'blend_transition': 1}}}}
        clips = {name: {'loop': True, 'bones': {'Head': {'rotation': [angle, 0, 0]}}} for name, angle in [('a', 10), ('b', 20)]}
        frames = test_controllers.ControllerTests().invoke(ctrl, clips,
            [{'time': 0, 'queries': {'ysm.head_pitch': -30}},
             {'time': 1, 'queries': {'q.go': 1}}, {'time': 1.5}], player=True, geometry=geometry)
        self.assertEqual([f['pose']['Head']['rotation'][0] for f in frames], [40, 40, 45])

    def test_raw_sampler_and_first_person_path_also_reset_undriven_channels(self):
        document = {'animations': {'idle': {}, 'a': {'loop': True, 'bones': {'arm': {'position': [12, 0, 0]}}}}}
        inputs = {'samples': [{'time': 0, 'layers': [{'name': 'a', 'time': 0}]}, {'time': 0.075, 'layers': []}]}
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'reset.json'
            path.write_text(json.dumps(document), encoding='utf-8')
            run = subprocess.run([str(EXE), '--animation-file', str(path), 'idle', '0.075', json.dumps(inputs)],
                                 capture_output=True, encoding='utf-8', timeout=5)
        self.assertEqual(run.returncode, 0, run.stderr)
        report = json.loads(run.stdout)
        self.assertAlmostEqual(report['pose']['arm']['position'][0], 6)
        self.assertEqual(report['expression_failures'], 0)

    def test_preview_base_changes_preserve_parallel_player_ownership_at_late_world_time(self):
        clips = {'idle': {'loop': True}, 'walk': {'loop': True},
                 'parallel2': {'loop': True, 'animation_length': 0.01, 'timeline': {'0': 'v.n+=1;'}}}
        frames = test_controllers.ControllerTests().invoke({}, clips,
            [{'time': 1000, 'state': 'idle'},
             {'time': 1000.055, 'state': 'walk', 'state_time': 0, 'enabled': False},
             {'time': 1000.105, 'state': 'idle', 'state_time': 0, 'enabled': True}], player=True)
        # Source players reset once per observed boundary, retaining their
        # ownership across preview changes without replaying missed loops.
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 2, 3])
        self.assertEqual([f['controllers']['coded']['player.parallel_2']['generation'] for f in frames], [1, 1, 1])
        self.assertEqual([f['controllers']['coded']['player.parallel_2']['cycle'] for f in frames], [0, 1, 2])

    def test_restarting_the_same_preview_base_does_not_restart_parallel_players(self):
        clips = {'idle': {'loop': True, 'timeline': {'0': 'v.main+=1;'}},
                 'parallel2': {'loop': True, 'animation_length': 1, 'timeline': {'0': 'v.parallel+=1;'}}}
        frames = test_controllers.ControllerTests().invoke({}, clips,
            [{'time': 1000, 'state_time': 0}, {'time': 1000.5, 'state_time': 0.5},
             {'time': 1000.6, 'state_time': 0, 'enabled': False}], player=True)
        self.assertEqual(frames[-1]['controllers']['coded']['player.main']['generation'], 2)
        self.assertEqual(frames[-1]['controllers']['coded']['player.parallel_2']['generation'], 1)
        self.assertEqual(frames[-1]['variables']['variable.parallel'], 1)
