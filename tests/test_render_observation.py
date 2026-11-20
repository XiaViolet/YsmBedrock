"""Extra render passes re-evaluate channels without taking a logical update."""
import json
import pathlib
import subprocess
import tempfile
import unittest

from test_molang import EXE


class RenderObservationTests(unittest.TestCase):
    def run_player(self, frames, clips=None, controllers=None, functions=None):
        document = {'animations': clips or {'idle': {'loop': True}},
                    'animation_controllers': controllers or {}, 'ysm_functions': functions or {}}
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / 'player.json'
            source.write_text(json.dumps(document), encoding='utf-8')
            run = subprocess.run([str(EXE), '--source-player-animation-file', str(source),
                                  json.dumps({'frames': frames})], capture_output=True, encoding='utf-8', timeout=10)
        self.assertEqual(run.returncode, 0, run.stderr)
        report = json.loads(run.stdout)
        self.assertEqual(report['expression_failures'], 0, report)
        self.assertEqual(report['compile_diagnostics'], [], report)
        return report['frames']

    @staticmethod
    def clip(expression=3, **kwargs):
        return dict({'loop': True, 'animation_length': 1,
                     'bones': {'root': {'position': [expression, 0, 0]}}}, **kwargs)

    def assert_owner_unchanged(self, before, observed):
        for key in ['owner_frame', 'owner_time', 'physics_time', 'physics_states',
                    'timeline_cursors', 'logical_variables', 'controllers', 'sync_dispatches']:
            self.assertEqual(before[key], observed[key], key)

    def test_coded_pose_reads_inventory_query_without_changing_the_world_query(self):
        clip = self.clip('ysm.rendering_in_inventory?9:3')
        frames = self.run_player([{'time': 0}, {'time': 0.2},
                                  {'time': 800, 'observe': True, 'entity': {'rendering_in_inventory': True}},
                                  {'time': 0.2}, {'time': 0.3}], {'idle': clip})
        self.assertEqual([f['pose']['root']['position'][0] for f in frames], [0, 3, 9, 3, 3])
        self.assert_owner_unchanged(frames[1], frames[2])

    def test_observation_does_not_run_init_update_defer_sync_or_controller_predicates(self):
        functions = {'@player_init': 'v.init+=1;', '@player_update': 'v.update+=1;ysm.defer(0);ysm.sync(5);',
                     '@defer': 'v.deferred+=1;', '@sync': 'v.synced+=1;',
                     '@player_ctrl_main': 'v.predicate+=1;return ctrl.state_bypass;'}
        frames = self.run_player([{'time': 0, 'local_sync': True}, {'time': 0.2, 'local_sync': True},
                                  {'time': 50, 'observe': True}, {'time': 100, 'observe': True},
                                  {'time': 0.4, 'local_sync': True}], {'idle': self.clip()}, functions=functions)
        for frame in frames[2:4]:
            self.assert_owner_unchanged(frames[1], frame)
        self.assertEqual(frames[-1]['variables']['variable.init'], 1)
        for name in ['update', 'deferred', 'synced', 'predicate']:
            self.assertEqual(frames[-1]['variables']['variable.' + name], 3)

    def test_observation_keeps_timeline_events_pending_for_the_next_real_update(self):
        clip = self.clip('v.events', timeline={'0': 'v.events+=1;', '0.5': 'v.events+=10;', '1': 'v.events+=100;'})
        frames = self.run_player([{'time': 0}, {'time': 0.2}, {'time': 4, 'observe': True},
                                  {'time': 0.6}, {'time': 2, 'observe': True}, {'time': 1.2}], {'idle': clip})
        self.assertEqual([f['logical_variables'].get('variable.events') for f in frames], [None, 1, 1, 11, 11, 112])
        self.assert_owner_unchanged(frames[1], frames[2])

    def test_physics_reads_and_new_physics_keys_in_an_observation_do_not_mutate_the_owner(self):
        expression = "ysm.rendering_in_inventory?ysm.first_order('preview',90):ysm.first_order('world',q.health)"
        clips = {'idle': self.clip(expression)}
        normal = [{'time': t, 'entity': {'health': value}} for t, value in [(0, 8), (0.2, 8), (0.4, 2), (0.6, 2)]]
        baseline = self.run_player(normal, clips)
        mixed = self.run_player(normal[:2] + [{'time': 100, 'observe': True,
                                             'entity': {'rendering_in_inventory': True, 'health': 99}}] + normal[2:], clips)
        self.assert_owner_unchanged(mixed[1], mixed[2])
        self.assertEqual(mixed[-1], baseline[-1])

    def test_random_sampling_during_extra_draws_does_not_consume_the_owners_random_stream(self):
        clips = {'idle': self.clip('math.random(0,1)')}
        baseline = self.run_player([{'time': 0}, {'time': 0.2}, {'time': 0.4}], clips)
        mixed = self.run_player([{'time': 0}, {'time': 0.2}] + [{'time': 50, 'observe': True}] * 6 + [{'time': 0.4}], clips)
        self.assertEqual(mixed[-1], baseline[-1])
        self.assertEqual([frame['pose'] for frame in mixed[2:8]], [mixed[2]['pose']] * 6)

    def test_source_controller_observes_current_state_without_following_a_new_transition(self):
        controllers = {'player.main': {'states': {
            'default': {'animations': ['a'], 'on_exit': ['v.exit+=1;'], 'transitions': [{'next': 'q.go'}]},
            'next': {'animations': ['b'], 'on_entry': ['v.entry+=1;']}}}}
        frames = self.run_player([{'time': 0}, {'time': 0.2}, {'time': 10, 'observe': True, 'queries': {'q.go': 1}},
                                  {'time': 0.4, 'queries': {'q.go': 1}}],
                                 {'a': self.clip(2), 'b': self.clip(7)}, controllers)
        self.assert_owner_unchanged(frames[1], frames[2])
        self.assertEqual(frames[2]['pose']['root']['position'][0], 2)
        self.assertEqual(frames[-1]['controllers']['states']['player.main']['state'], 'next')
        self.assertEqual(frames[-1]['variables'], {'variable.entry': 1, 'variable.exit': 1})

    def test_source_animation_conditions_can_read_the_observation_scope(self):
        controllers = {'player.main': {'states': {'default': {'animations': [
            {'a': '!ysm.rendering_in_inventory'}, {'b': 'ysm.rendering_in_inventory'}]}}}}
        frames = self.run_player([{'time': 0}, {'time': 0.2},
                                  {'time': 0.3, 'observe': True, 'entity': {'rendering_in_inventory': True}}],
                                 {'a': self.clip(2), 'b': self.clip(8)}, controllers)
        self.assertEqual(frames[1]['pose']['root']['position'][0], 2)
        self.assertEqual(frames[2]['pose']['root']['position'][0], 8)
        self.assert_owner_unchanged(frames[1], frames[2])

    def test_nested_source_controllers_keep_their_own_playback_clocks_during_observation(self):
        controllers = {'player.main': {'initial_state': 'ysm-entry-child', 'states': {'ysm-entry-child': {}}},
                       'player.main.child': {'states': {'default': {'animations': ['a']}}}}
        frames = self.run_player([{'time': 0}, {'time': 0.35}, {'time': 999, 'observe': True}],
                                 {'a': self.clip('q.anim_time')}, controllers)
        self.assertEqual(frames[-1]['pose'], frames[-2]['pose'])
        self.assert_owner_unchanged(frames[-2], frames[-1])

    def test_legacy_channel_helper_writes_stay_in_the_observation_copy(self):
        clips = {'idle': self.clip('ysm.rendering_in_inventory?{v.leak=9;return 8;}:3')}
        frames = self.run_player([{'time': 0}, {'time': 0.2},
                                  {'time': 100, 'observe': True, 'entity': {'rendering_in_inventory': True}}, {'time': 0.4}], clips)
        self.assertEqual(frames[2]['observation_failures'], 0)
        self.assertEqual(frames[2]['pose']['root']['position'][0], 8)
        self.assertEqual(frames[2]['variables']['variable.leak'], 9)
        self.assert_owner_unchanged(frames[1], frames[2])
        self.assertNotIn('variable.leak', frames[-1]['variables'])
        self.assertEqual(frames[-1]['pose']['root']['position'][0], 3)

    def test_repeated_preview_helper_writes_start_from_the_same_logical_snapshot(self):
        clip = self.clip('v.counter+=1', bones={'root': {'position': ['v.counter+=1', 'v.counter', 0]}})
        frames = self.run_player([{'time': 0}, {'time': 0.2}, {'time': 20, 'observe': True},
                                  {'time': 50, 'observe': True}, {'time': 0.4}], {'idle': clip})
        self.assertEqual(frames[2]['pose'], frames[3]['pose'])
        self.assertEqual(frames[2]['pose'], frames[4]['pose'])
        self.assert_owner_unchanged(frames[1], frames[2])
        self.assert_owner_unchanged(frames[1], frames[3])
        self.assertEqual(frames[2]['pose']['root']['position'][0], frames[2]['pose']['root']['position'][1])

    def test_invalid_preview_channel_does_not_poison_the_owner_or_next_real_sample(self):
        clips = {'idle': self.clip('ysm.rendering_in_inventory?math.abs(3.5e38):3')}
        frames = self.run_player([{'time': 0}, {'time': 0.2},
                                  {'time': 20, 'observe': True, 'entity': {'rendering_in_inventory': True}},
                                  {'time': 0.4}], clips)
        self.assertEqual(frames[2]['observation_failures'], 1)
        self.assert_owner_unchanged(frames[1], frames[2])
        self.assertEqual(frames[-1]['pose']['root']['position'][0], 3)

    def test_action_arguments_in_readonly_channels_are_not_evaluated(self):
        clips = {'idle': self.clip('ysm.rendering_in_inventory?{ysm.sync(v.leak=5);return 9;}:3')}
        frames = self.run_player([{'time': 0}, {'time': 0.2},
                                  {'time': 100, 'observe': True, 'entity': {'rendering_in_inventory': True}}], clips)
        self.assertEqual(frames[-1]['pose']['root']['position'][0], 9)
        self.assertEqual(frames[-1]['observation_failures'], 0)
        self.assert_owner_unchanged(frames[-2], frames[-1])

    def test_extra_hand_and_parallel_layers_keep_composition_and_event_ownership(self):
        clips = {'idle': self.clip(2), 'parallel0': self.clip(0, bones={'root': {'rotation': [15, 0, 0]}}),
                 'extra': self.clip(0, bones={'root': {'rotation': [20, 0, 0]}}),
                 'hold_mainhand:bow': self.clip(5)}
        frames = self.run_player([{'time': 0, 'items': [{'id': 'minecraft:bow', 'category': 'bow'}], 'extra': 'extra'},
                                  {'time': 0.3, 'items': [{'id': 'minecraft:bow', 'category': 'bow'}], 'extra': 'extra'},
                                  {'time': 20, 'observe': True}], clips)
        self.assertEqual(frames[-2]['controllers']['coded']['player.hold_mainhand']['animation'], 'hold_mainhand:bow')
        self.assertEqual(frames[-1]['pose'], frames[-2]['pose'])
        self.assert_owner_unchanged(frames[-2], frames[-1])

    def test_weight_and_channel_random_reads_keep_the_normal_sampling_order(self):
        clip = self.clip('math.random(1,2)', blend_weight='math.random(0,1)')
        frames = self.run_player([{'time': 0}, {'time': 0.2}, {'time': 60, 'observe': True}, {'time': 0.4}], {'idle': clip})
        self.assertEqual(frames[2]['pose'], frames[3]['pose'])
        self.assert_owner_unchanged(frames[1], frames[2])

    def test_observation_before_the_first_logical_sample_does_not_initialize_a_model(self):
        frames = self.run_player([{'time': 200, 'observe': True}, {'time': 0}, {'time': 0.2}],
                                 {'idle': self.clip()}, functions={'@player_init': 'v.init+=1;'})
        self.assertEqual(frames[0]['pose'], {})
        self.assertEqual(frames[0]['logical_variables'], {})
        self.assertEqual(frames[0]['owner_frame'], 0)
        self.assertEqual(frames[-1]['variables']['variable.init'], 1)

    def test_coded_beginning_and_ending_poses_match_an_extra_read_at_the_same_owner_clock(self):
        for time in [0, 0.05, 0.1, 0.15, 0.4, 0.51, 0.6, 0.7]:
            with self.subTest(time=time):
                clips = {'idle': self.clip(1), 'jump': self.clip(9, loop=False, animation_length=0.3)}
                frames = self.run_player([{'time': 0}, {'time': 0.2}, {'time': 0.3, 'state': 'jump', 'state_time': 0},
                                          {'time': 0.3 + time, 'state': 'jump', 'state_time': time},
                                          {'time': 99, 'observe': True}], clips)
                self.assertEqual(frames[-1]['pose'], frames[-2]['pose'])
                self.assert_owner_unchanged(frames[-2], frames[-1])

    def test_source_blends_ending_and_undriven_reset_are_stable_under_repeated_observation(self):
        controllers = {'player.main': {'states': {
            'default': {'animations': ['a'], 'transitions': [{'next': 'q.go'}]},
            'next': {'animations': ['b'], 'blend_transition': 0.2}}}}
        for time in [0.3, 0.4, 0.5, 0.6, 0.8, 1.0]:
            with self.subTest(time=time):
                frames = self.run_player([{'time': 0}, {'time': 0.2}, {'time': 0.3, 'queries': {'q.go': 1}},
                                          {'time': time}, {'time': 90, 'observe': True}],
                                         {'a': self.clip(2), 'b': self.clip(7, loop=False, animation_length=0.2)}, controllers)
                self.assertEqual(frames[-1]['pose'], frames[-2]['pose'])
                self.assert_owner_unchanged(frames[-2], frames[-1])


if __name__ == '__main__':
    unittest.main()
