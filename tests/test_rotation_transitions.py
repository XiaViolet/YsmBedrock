"""Transitions compared with unchanged upstream MathUtil running on JOML."""
import json
import pathlib
import unittest

import test_coded_playback as coded
import test_controllers as controllers


class RotationTransitionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.references = json.loads((pathlib.Path(__file__).parent / 'fixtures/source_rotation_reference.json').read_text())['cases']

    def assert_reference(self, actual, name):
        for value, expected in zip(actual, self.references[name]['rotation']):
            self.assertAlmostEqual(value, expected, delta=0.00005)

    def test_coded_beginning_uses_source_shortest_nlerp(self):
        case = self.references['quarter_shortest']
        clip = {'loop': True, 'bones': {'root': {'rotation': case['end']}}}
        frames = coded.CodedPlaybackTests().run_frames({'a': clip},
            [{'time': 0, 'animation': 'a'}, {'time': case['alpha'], 'animation': 'a'}], transition=1)
        self.assert_reference(frames[-1]['pose']['root']['rotation'], 'quarter_shortest')

    def test_coded_multi_axis_beginning_includes_declared_initial_rotation(self):
        case = self.references['mixed_with_initial']
        clip = {'loop': True, 'bones': {'root': {'rotation': case['end']}}}
        geometry = {'minecraft:geometry': [{'bones': [{'name': 'root', 'rotation': case['initial']}]}]}
        frames = coded.CodedPlaybackTests().run_frames({'a': clip},
            [{'time': 0, 'animation': 'a'}, {'time': case['alpha'], 'animation': 'a'}],
            transition=1, pose={'root': {'rotation': case['begin']}}, geometry=geometry)
        self.assert_reference(frames[-1]['pose']['root']['rotation'], 'mixed_with_initial')

    def test_coded_ending_uses_source_nlerp_to_lower_layer(self):
        case = self.references['coded_ending']
        clip = {'animation_length': 1, 'bones': {'root': {'rotation': case['begin']}}}
        frames = coded.CodedPlaybackTests().run_frames({'a': clip},
            [{'time': t, 'animation': 'a'} for t in [0, .5, 1, 1 + .15 * case['alpha']]],
            pose={'root': {'rotation': case['end']}})
        self.assert_reference(frames[-1]['pose']['root']['rotation'], 'coded_ending')

    def test_bedrock_beginning_blends_summed_rotations_and_matches_observation(self):
        controller = {'states': {'default': {'blend_transition': 1, 'animations': ['a', 'b']}}}
        clips = {name: {'loop': True, 'bones': {'root': {'rotation': [0,0,angle]}}}
                 for name, angle in [('a', 90), ('b', 180)]}
        frames = controllers.ControllerTests().invoke({'player.main': controller}, clips,
            [{'time': 0}, {'time': .25}, {'time': .25, 'observe': True}], player=True)
        for frame in frames[-2:]:
            self.assert_reference(frame['pose']['root']['rotation'], 'quarter_shortest')

    def test_bedrock_ending_attenuates_group_then_nlerps_to_lower_layer(self):
        controller = {'states': {'default': {'animations': ['a']}}}
        clips = {'a': {'animation_length': 1, 'bones': {'root': {'rotation': [0,0,270]}}}}
        frames = controllers.ControllerTests().invoke({'player.main': controller}, clips,
            [{'time': t} for t in [0, .5, 1, 1.0375]] + [{'time': 1.0375, 'observe': True}], player=True)
        for frame in frames[-2:]:
            self.assert_reference(frame['pose']['root']['rotation'], 'bedrock_ending')
