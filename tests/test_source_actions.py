"""Source action triggers, priorities, and riding overlays through the player endpoint."""
import json
import subprocess
import unittest

import test_controllers
from test_import import EXE, SAMPLE


class SourceActionTests(unittest.TestCase):
    invoke = test_controllers.ControllerTests.invoke

    def test_original_eri_sword_combo_changes_pose_and_resets_after_timeout(self):
        samples = [(0, -1, 0), (.2, 0, 0), (.3, 0, 2), (.5, -1, 0),
                   (.6, 0, 0), (.7, 0, 2), (.9, -1, 0), (1., 0, 0),
                   (1.1, 0, 2), (2.7, -1, 0), (3., 0, 0)]
        frames = [{'time': time, 'automatic_state': True, 'entity': {'on_ground': True},
                   'items': [{'id': 'minecraft:wooden_sword', 'category': 'sword'}],
                   'swinging_hand': hand, 'swing_progress': progress} for time, hand, progress in samples]
        run = subprocess.run([str(EXE), '--player-animations', str(SAMPLE), json.dumps({'frames': frames})],
                             capture_output=True, encoding='utf-8', timeout=15)
        self.assertEqual(run.returncode, 0, run.stderr)
        report = json.loads(run.stdout)
        self.assertEqual(report['expression_failures'], 0)
        selected = [report['frames'][i] for i in [1, 4, 7, 10]]
        self.assertEqual([frame['variables']['variable.qh'] for frame in selected], [1, 2, 1, 1])
        for frame, expected in zip(selected, [-167.20517, -60, -167.20517, -167.20517]):
            self.assertAlmostEqual(frame['pose']['RightArm']['rotation'][0], expected, places=5)

    @staticmethod
    def clips(names):
        return {name: {'loop': True, 'animation_length': 10,
                       'bones': {'root': {'position': [index + 1, 0, 0]}}}
                for index, name in enumerate(names)}

    def test_all_source_base_actions_use_the_matching_clip(self):
        cases = [
            ('death', {'alive': False}), ('riptide', {'riptide': True}),
            ('sleep', {'sleeping': True}), ('swim', {'swimming': True}),
            ('climb', {'crawling': True, 'walk_animation_speed': .2}),
            ('climbing', {'crawling': True}),
            ('ladder_up', {'on_climbable': True, 'velocity': [0, .0001, 0]}),
            ('ladder_down', {'on_climbable': True, 'velocity': [0, -.0001, 0]}),
            ('ladder_stillness', {'on_climbable': True}),
            ('fly', {'flying': True}), ('elytra_fly', {'gliding': True}),
            ('swim_stand', {'in_water': True, 'on_ground': False}),
            ('attacked', {'hurt': True}), ('jump', {'on_ground': False}),
            ('sneak', {'crouching': True, 'walk_animation_speed': .2}),
            ('sneaking', {'crouching': True}), ('run', {'sprinting': True}),
            ('walk', {'walk_animation_speed': .2}), ('idle', {})]
        clips = self.clips([name for name, _ in cases])
        for name, fields in cases:
            with self.subTest(name=name):
                frames = self.invoke({}, clips, [
                    {'time': t, 'automatic_state': True, 'entity': {'on_ground': True} | fields}
                    for t in [0, .2]], player=True)
                self.assertEqual(frames[-1]['controllers']['coded']['player.main']['animation'], name)
                self.assertEqual(frames[-1]['pose']['root']['position'][0], clips[name]['bones']['root']['position'][0])

    def test_base_priority_hurt_over_jump_and_riptide_over_swimming(self):
        cases = [({'hurt': True, 'on_ground': False}, 'attacked'),
                 ({'hurt': True, 'in_water': True, 'on_ground': False}, 'swim_stand'),
                 ({'riptide': True, 'swimming': True, 'hurt': True}, 'riptide'),
                 ({'riptide': True, 'alive': False}, 'death'),
                 ({'in_water': True, 'on_ground': True}, 'idle')]
        clips = self.clips(['attacked', 'swim_stand', 'riptide', 'death', 'idle'])
        for fields, expected in cases:
            frames = self.invoke({}, clips, [{'time': t, 'automatic_state': True, 'entity': fields}
                                            for t in [0, .2]], player=True)
            self.assertEqual(frames[-1]['controllers']['coded']['player.main']['animation'], expected)

    def test_walk_threshold_uses_smoothed_animation_speed(self):
        clips = self.clips(['idle', 'walk'])
        for speed, expected in [(0.05, 'idle'), (.05001, 'walk'), (-.051, 'idle'), (0, 'idle'),
                                (.0500000001, 'idle'), (.050000001, 'idle'), (.050000004, 'walk')]:
            frames = self.invoke({}, clips, [
                {'time': t, 'automatic_state': True, 'entity': {
                    'on_ground': True, 'velocity': [1, 0, 0], 'walk_animation_speed': speed}}
                for t in [0, .2]], player=True)
            self.assertEqual(frames[-1]['controllers']['coded']['player.main']['animation'], expected)

    def test_crawling_and_sneaking_keep_absolute_limb_amount_and_java_float_threshold(self):
        clips = self.clips(['climb', 'climbing', 'sneak', 'sneaking'])
        for fields, moving, still in [({'crawling': True}, 'climb', 'climbing'),
                                      ({'crouching': True}, 'sneak', 'sneaking')]:
            for speed, selected in [(-.051, moving), (.050000001, still), (-.050000001, still),
                                    (.050000004, moving), (0, still)]:
                with self.subTest(fields=fields, speed=speed):
                    frames = self.invoke({}, clips, [{'time': t, 'automatic_state': True,
                        'entity': {'on_ground': True, 'walk_animation_speed': speed} | fields}
                        for t in [0, .2]], player=True)
                    self.assertEqual(frames[-1]['controllers']['coded']['player.main']['animation'], selected)

    def test_riptide_and_attacked_controls_reach_authored_controllers(self):
        controller = {'states': {'default': {'transitions': [
            {'spin': 'ctrl.riptide'}, {'hit': 'ctrl.attacked'}]},
            'spin': {'animations': ['riptide']}, 'hit': {'animations': ['attacked']}}}
        for state, target in [('riptide', 'spin'), ('attacked', 'hit')]:
            frames = self.invoke({'player.main': controller}, self.clips(['riptide', 'attacked']),
                                 [{'time': 0, 'state': state}], player=True)
            self.assertEqual(frames[0]['controllers']['states']['player.main']['state'], target)

    def test_vehicle_defaults_replace_main_and_dismount_resumes_main(self):
        clips = self.clips(['idle', 'boat', 'ride_pig', 'ride', 'sit'])
        for legacy in ['boat', 'ride_pig', 'ride', '']:
            frames = self.invoke({}, clips, [
                {'time': t, 'vehicle': {'id': 'minecraft:test', 'legacy_animation': legacy}} for t in [0, .2]]
                + [{'time': .4}, {'time': .6}], player=True)
            self.assertEqual(frames[1]['controllers']['coded']['player.vehicle']['animation'], legacy or 'sit')
            self.assertEqual(frames[1]['controllers']['coded']['player.main']['animation'], '')
            self.assertEqual(frames[-1]['controllers']['coded']['player.main']['animation'], 'idle')

    def test_vehicle_exact_id_precedes_tags_and_passenger_overlays_later(self):
        clips = self.clips(['vehicle$minecraft:pig', 'vehicle#test:rideable', 'passenger$minecraft:chicken',
                            'passenger#test:pet', 'ride_pig'])
        frame = {'vehicle': {'id': 'minecraft:pig', 'legacy_animation': 'ride_pig', 'tags': ['test:rideable']},
                 'passenger_entity': {'id': 'minecraft:chicken', 'tags': ['test:pet']}}
        frames = self.invoke({}, clips, [frame | {'time': t} for t in [0, .2]], player=True)
        coded = frames[-1]['controllers']['coded']
        self.assertEqual(coded['player.vehicle']['animation'], 'vehicle$minecraft:pig')
        self.assertEqual(coded['player.passenger']['animation'], 'passenger$minecraft:chicken')
        self.assertEqual(frames[-1]['pose']['root']['position'][0], 3)
        frame['vehicle']['id'] = 'test:custom'
        frame['passenger_entity']['id'] = 'test:custom'
        frames = self.invoke({}, clips, [frame | {'time': t} for t in [0, .2]], player=True)
        self.assertEqual(frames[-1]['controllers']['coded']['player.vehicle']['animation'], 'vehicle#test:rideable')
        self.assertEqual(frames[-1]['controllers']['coded']['player.passenger']['animation'], 'passenger#test:pet')

    def test_dead_mounts_do_not_suppress_main_or_emit_riding(self):
        clips = self.clips(['idle', 'boat', 'passenger$minecraft:chicken'])
        frames = self.invoke({}, clips, [{'time': t, 'vehicle': {'id': 'minecraft:boat', 'alive': False},
            'passenger_entity': {'id': 'minecraft:chicken', 'alive': False}} for t in [0, .2]], player=True)
        self.assertEqual(frames[-1]['controllers']['coded']['player.main']['animation'], 'idle')
        self.assertEqual(frames[-1]['controllers']['coded']['player.vehicle']['animation'], '')
        self.assertEqual(frames[-1]['controllers']['coded']['player.passenger']['animation'], '')

    def test_original_eri_package_riding_and_newly_connected_base_clips(self):
        cases = [('riptide', {'entity': {'riptide': True}}), ('attacked', {'entity': {'hurt': True}}),
                 ('boat', {'vehicle': {'id': 'minecraft:boat', 'legacy_animation': 'boat'}}),
                 ('ride_pig', {'vehicle': {'id': 'minecraft:pig', 'legacy_animation': 'ride_pig'}}),
                 ('ride', {'vehicle': {'id': 'minecraft:horse', 'legacy_animation': 'ride'}}),
                 ('sit', {'vehicle': {'id': 'minecraft:minecart'}})]
        for name, fields in cases:
            frames = [{'time': t, 'automatic_state': True, 'entity': {'on_ground': True}} | fields
                      for t in [0, .2, .4]]
            run = subprocess.run([str(EXE), '--player-animations', str(SAMPLE), json.dumps({'frames': frames})],
                                 capture_output=True, encoding='utf-8', timeout=15)
            self.assertEqual(run.returncode, 0, run.stderr)
            report = json.loads(run.stdout)
            self.assertEqual(report['expression_failures'], 0)
            owner = 'player.main' if name in ['riptide', 'attacked'] else 'player.vehicle'
            self.assertEqual(report['frames'][-1]['controllers']['coded'][owner]['animation'], name)
            self.assertTrue(report['frames'][-1]['pose'])
