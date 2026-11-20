"""Vehicle/projectile controller chains from the pinned public YSM source."""
import json
import pathlib
import subprocess
import tempfile
import unittest

from test_import import EXE


class ReplacedControllerTests(unittest.TestCase):
    @staticmethod
    def clip(value, **options):
        return dict({'loop': True, 'animation_length': 2, 'bones': {'Root': {'position': [value, 0, 0]}}}, **options)

    def run_frames(self, kind, clips, frames, definitions=None, functions=None, geometry=None, expected=None):
        document = {'animations': clips, 'animation_controllers': definitions or {}, 'ysm_functions': functions or {}}
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'replaced.json'
            path.write_text(json.dumps(document), encoding='utf-8')
            run = subprocess.run([str(EXE), '--' + kind + '-animation-file', str(path),
                                  json.dumps({'frames': frames, 'geometry': geometry})],
                                 capture_output=True, encoding='utf-8', timeout=5)
        self.assertEqual(run.returncode, 0, run.stderr)
        report = json.loads(run.stdout)
        self.assertEqual(report['expression_failures'], 0, report)
        self.assertEqual(report['compile_diagnostics'], [], report)
        self.assertEqual(report['evaluation_diagnostics'], expected or [], report)
        return report['frames']

    def test_vehicle_main_priority_water_ground_then_fly(self):
        clips = {name: self.clip(value) for name, value in [('water', 1), ('ground', 2), ('fly', 3)]}
        for entity, selected in [({'in_water': True, 'on_ground': True}, 'water'), ({'on_ground': True}, 'ground'), ({}, 'fly')]:
            with self.subTest(entity=entity):
                frames = self.run_frames('vehicle', clips, [{'time': t, 'entity': entity} for t in [0, .2]])
                self.assertEqual(frames[-1]['controllers']['coded']['vehicle.main']['animation'], selected)

    def test_nonliving_targets_do_not_run_player_jump_fishing_or_blink_pose_branches(self):
        document = self.clip(0)
        document['bones']['Root']['position'] = ['q.is_jumping', 'ysm.is_fishing', 'ysm.is_close_eyes']
        for kind, name in [('vehicle', 'fly'), ('projectile', 'air')]:
            frames = self.run_frames(kind, {name: document}, [{'time': t, 'entity': {
                'render_ticks': 86, 'fishing': True}} for t in [0, .2]])
            self.assertEqual(frames[-1]['pose']['Root']['position'], [0, 0, 0])

    def test_vehicle_motion_uses_horizontal_point_zero_five_threshold_and_all_passengers(self):
        clips = {name: self.clip(value) for name, value in [('idle', 1), ('forward', 2), ('has_ride', 3), ('not_ride', 4)]}
        for entity, move, ride in [({'velocity': [0, 5, .05]}, 'idle', 'not_ride'),
                                   ({'velocity': [.03, 0, .04], 'has_passengers': True}, 'idle', 'has_ride'),
                                   ({'velocity': [.050001, 0, 0]}, 'forward', 'not_ride')]:
            frames = self.run_frames('vehicle', clips, [{'time': 0, 'entity': entity}])
            coded = frames[-1]['controllers']['coded']
            self.assertEqual(coded['vehicle.move']['animation'], move)
            self.assertEqual(coded['vehicle.ride']['animation'], ride)

    def test_projectile_priority_water_fire_embedded_air(self):
        clips = {name: self.clip(value) for name, value in [('water', 1), ('fire', 2), ('ground', 3), ('air', 4)]}
        for entity, selected in [({'in_water': True, 'on_fire': True, 'in_ground': True}, 'water'),
                                  ({'on_fire': True, 'in_ground': True}, 'fire'), ({'in_ground': True}, 'ground'),
                                  ({'on_ground': True}, 'air')]:
            frames = self.run_frames('projectile', clips, [{'time': t, 'entity': entity} for t in [0, .2]])
            self.assertEqual(frames[-1]['controllers']['coded']['projectile.main']['animation'], selected)

    def test_entity_animation_reads_native_states_and_emits_its_own_particle(self):
        clip = self.clip(0, timeline={'0': "q.is_on_fire?ysm.abs_particle('white_smoke',1,2,3):0;"})
        clip['bones']['Root']['position'] = ['ysm.on_ground_time', 'ysm.delta_movement_length', 'ysm.is_biting']
        clip['bones']['Root']['scale'] = [
            "ysm.throwable_item=='minecraft:splash_potion'", "ysm.hooked_in=='minecraft:cod'", 'ysm.is_spectral_arrow']
        entity = {'in_ground': True, 'on_ground_time': 60, 'velocity': [3, 4, 0], 'is_biting': True,
                  'throwable_item': 'minecraft:splash_potion', 'hooked_in': 'minecraft:cod',
                  'is_spectral_arrow': True, 'on_fire': True, 'position': [10, 20, 30]}
        frames = self.run_frames('projectile', {'parallel0': clip}, [{'time': 0, 'entity': entity},
                                                                    {'time': .2, 'entity': {}}])
        self.assertEqual(frames[0]['pose']['Root']['position'], [60, 5, 1])
        self.assertEqual(frames[0]['pose']['Root']['scale'], [1, 1, 1])
        self.assertEqual(frames[0]['particle_cues'][0]['spawns'][0]['position'], [11, 22, 33])
        self.assertEqual(frames[1]['pose']['Root']['position'], [0, 0, 0])
        self.assertEqual(frames[1]['particle_cues'], [])

    def test_fly_only_compatibility_is_explicit_and_air_takes_priority(self):
        frames = self.run_frames('projectile', {'fly': self.clip(3)}, [{'time': t} for t in [0, .2]],
                                 expected=['Projectile air/fly compatibility: using fly'])
        self.assertEqual(frames[-1]['pose']['Root']['position'][0], 3)
        frames = self.run_frames('projectile', {'fly': self.clip(3), 'air': self.clip(4)}, [{'time': t} for t in [0, .2]])
        self.assertEqual(frames[-1]['pose']['Root']['position'][0], 4)

    def test_controller_order_and_player_hooks_are_separate(self):
        functions = {'@player_init': 'v.bad+=1;', '@player_update': 'v.bad+=10;'}
        order = ['pre_parallel_0', 'pre_main', 'main', 'move', 'ride', 'post_main', 'parallel_0']
        for i, name in enumerate(order):
            functions['@vehicle_ctrl_' + name] = 'v.order=v.order*10+' + str(i + 1) + ';'
        functions['@vehicle_ctrl_origin'] = 'v.bad+=100;'
        frames = self.run_frames('vehicle', {}, [{'time': 0}], functions=functions)
        self.assertEqual(frames[0]['variables']['variable.order'], 1234567)
        self.assertNotIn('variable.bad', frames[0]['variables'])
        self.assertEqual(frames[0]['controllers']['stages'], ['vehicle.' + name for name in order[:4]] +
                         ['vehicle.origin'] + ['vehicle.' + name for name in order[4:]])

    def test_parallel_slots_are_limited_and_projectile_has_no_preparallel_stage(self):
        clips = {'parallel0': self.clip(1), 'pre_parallel0': self.clip(8), 'parallel8': self.clip(9)}
        functions = {'@projectile_ctrl_parallel_8': 'v.bad=1;', '@projectile_ctrl_pre_parallel_0': 'v.bad=2;'}
        frames = self.run_frames('projectile', clips, [{'time': 0}], functions=functions)
        self.assertEqual(frames[0]['controllers']['stages'], ['projectile.parallel_0'])
        self.assertNotIn('variable.bad', frames[0]['variables'])

    def test_explicit_empty_single_and_parallel_do_not_create_controllers(self):
        frames = self.run_frames('vehicle', {'water': {}, 'idle': {'loop': True}, 'parallel0': {}}, [{'time': 0}])
        self.assertEqual(frames[0]['controllers']['coded'], {})
        self.assertEqual(frames[0]['controllers']['stages'], ['vehicle.origin'])

    def test_source_definition_overrides_predicate_and_empty_definition_suppresses_it(self):
        clips = {'ground': self.clip(1), 'custom': self.clip(9)}
        definitions = {'vehicle.main': {'states': {'default': {'animations': ['custom']}}}}
        frames = self.run_frames('vehicle', clips, [{'time': 0, 'entity': {'on_ground': True}}], definitions)
        self.assertEqual(frames[0]['pose']['Root']['position'][0], 9)
        frames = self.run_frames('vehicle', clips, [{'time': 0, 'entity': {'on_ground': True}}], {'vehicle.main': {}})
        self.assertEqual(frames[0]['pose'], {})

    def test_predicate_event_can_select_authored_animation_and_continue(self):
        frames = self.run_frames('projectile', {'custom': self.clip(7)}, [{'time': 0}, {'time': .2}, {'time': .4}],
                                 functions={'@projectile_ctrl_main': "ctrl.set_animation('custom');return 2;"})
        self.assertEqual(frames[-1]['pose']['Root']['position'][0], 7)

    def test_main_uses_authored_once_but_parallel_explicitly_loops(self):
        clips = {'ground': self.clip(2, loop=False, animation_length=.5),
                 'parallel0': self.clip(3, loop=False, animation_length=.125, timeline={'0': 'v.n+=1;'})}
        frames = self.run_frames('vehicle', clips, [{'time': t, 'entity': {'on_ground': True}} for t in [0, .2, 1, 1.2]])
        self.assertEqual(frames[-1]['controllers']['coded']['vehicle.main']['phase'], 'idle')
        self.assertEqual(frames[-1]['controllers']['coded']['vehicle.parallel_0']['phase'], 'running')
        self.assertEqual(frames[-1]['variables']['variable.n'], 4)

    def test_scripted_parallel_selection_keeps_source_additive_rotation(self):
        clips = {'air': {'loop': True, 'bones': {'Root': {'rotation': [5, 0, 0]}}},
                 'custom': {'loop': True, 'bones': {'Root': {'rotation': [10, 0, 0]}}}}
        frames = self.run_frames('projectile', clips, [{'time': t} for t in [0, .2, .4]],
                                 functions={'@projectile_ctrl_parallel_0': "ctrl.set_animation('custom',10);return 2;"})
        self.assertEqual(frames[-1]['pose']['Root']['rotation'][0], 15)

    def test_replaced_owner_never_adds_player_head_rotation_and_observation_does_not_advance(self):
        geometry = {'minecraft:geometry': [{'bones': [{'name': 'Head'}]}]}
        clips = {'air': {'loop': True, 'bones': {'Head': {'rotation': [11, 0, 0], 'position': ['q.is_in_ui', 0, 0]}}},
                 'parallel0': {'loop': True, 'animation_length': 2, 'timeline': {'0': 'v.n+=1;'}}}
        frames = self.run_frames('projectile', clips,
            [{'time': 0}, {'time': .2, 'queries': {'ysm.head_pitch': 90}},
             {'time': 500, 'observe': True, 'queries': {'ysm.head_pitch': -90, 'q.is_in_ui': 1}}, {'time': .4}], geometry=geometry)
        self.assertEqual(frames[2]['pose']['Head']['rotation'][0], 11)
        self.assertEqual(frames[2]['pose']['Head']['position'][0], 1)
        self.assertEqual(frames[1]['controllers'], frames[2]['controllers'])
        self.assertEqual(frames[-1]['variables']['variable.n'], 1)

    def test_long_gap_and_same_frame_do_not_replay_missed_parallel_cycles(self):
        clip = {'loop': True, 'animation_length': .125, 'timeline': {'0': 'v.n+=1;'}}
        frames = self.run_frames('vehicle', {'parallel0': clip}, [{'time': t} for t in [0, 1024, 1024, 1024.01]])
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 2, 2, 2])

    def test_missing_entity_stops_predicate_player_without_erasing_parallel(self):
        clips = {'ground': self.clip(2), 'parallel0': self.clip(3)}
        frames = self.run_frames('vehicle', clips, [{'time': t, 'entity': {'on_ground': True, 'present': present}}
                    for t, present in [(0, True), (.2, True), (.3, False), (.5, False)]])
        self.assertEqual(frames[-1]['controllers']['coded']['vehicle.main']['phase'], 'idle')
        self.assertEqual(frames[-1]['controllers']['coded']['vehicle.parallel_0']['phase'], 'running')


if __name__ == '__main__':
    unittest.main()
