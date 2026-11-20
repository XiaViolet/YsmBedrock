import json
import math
import pathlib
import subprocess
import tempfile
import unittest

from test_import import EXE, PROJECT


class ControllerTests(unittest.TestCase):
    def invoke(self, controllers, clips, frames, roots=None, succeeds=True, player=False, full_report=False,
               geometry=None, source_profile=False, merge_multiline=False, legacy_paths=False):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'controllers.json'
            path.write_text(json.dumps({'animations': clips, 'animation_controllers': controllers,
                                       'ysm_merge_multiline_expr': merge_multiline,
                                       'ysm_legacy_variable_paths': legacy_paths}), encoding='utf-8')
            inputs = {'frames': frames}
            if geometry is not None:
                inputs['geometry'] = geometry
            if roots is not None:
                inputs['controllers'] = roots
            mode = '--player-animation-file' if player else '--controller-file'
            if source_profile:
                mode = '--source-player-animation-file' if player else '--source-controller-file'
            result = subprocess.run([str(EXE), mode, str(path), json.dumps(inputs)],
                                    capture_output=True, encoding='utf-8', timeout=5)
            if not succeeds:
                self.assertNotEqual(result.returncode, 0, result.stdout)
                return result.stderr
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(result.stdout)
            self.assertEqual(report['expression_failures'], 0, report)
            self.assertEqual(report['compile_diagnostics'], [], report)
            return report if full_report else report['frames']

    @staticmethod
    def clip(value, **kwargs):
        return dict({'loop': True, 'bones': {'root': {'position': [value, 0, 0]}}}, **kwargs)

    @staticmethod
    def state(frame, name='player.main'):
        return frame['controllers']['states'][name]['state']

    def test_legacy_trig_uses_the_release_client_sine_table(self):
        # Observed with YSM 2.6.5 on MC 1.21.1, including float-to-int saturation.
        samples = [
            ('sin', '0.001', 0.0), ('sin', '-0.12345', -0.002109222),
            ('cos', '0.12345', 0.9999978), ('cos', '-0.12345', 0.99999756),
            ('cos', '90', 1.2246469e-16), ('cos', '-90', 0.0),
            ('sin', '10000000000', -9.58738e-5), ('cos', '10000000000', -9.58738e-5),
            ('sin', '-10000000000', 0.0), ('cos', '-10000000000', 0.0),
        ]
        commands = [f'v.sample{i}=math.{fn}({degrees});' for i, (fn, degrees, _) in enumerate(samples)]
        ctrl = {'player.main': {'states': {'default': {'animations': ['a']}}}}
        clips = {'a': {'timeline': {'0': commands}}}
        variables = self.invoke(ctrl, clips, [{'time': 0}], legacy_paths=True)[0]['variables']
        for i, (fn, degrees, expected) in enumerate(samples):
            with self.subTest(function=fn, degrees=degrees):
                self.assertAlmostEqual(variables[f'variable.sample{i}'], expected, delta=abs(expected)*6e-8)
        typed = self.invoke(ctrl, clips, [{'time': 0}])[0]['variables']
        self.assertAlmostEqual(typed['variable.sample0'], math.sin(math.radians(0.001)), delta=1e-11)

    def test_sparse_overlay_retains_large_lower_pose_and_undriven_channels(self):
        base = {'loop': True, 'bones': {
            'bone' + str(i): {'position': [i, 1, 2], 'rotation': [0, 0, 0], 'scale': [2, 2, 2]}
            for i in range(200)}}
        overlay = {'loop': True, 'bones': {'bone100': {'rotation': [3, 4, 5]}}}
        controller = {'states': {'default': {'animations': ['base', 'overlay']}}}
        frames = self.invoke({'player.main': controller}, {'base': base, 'overlay': overlay},
                             [{'time': 0}, {'time': .2}])
        for frame in frames:
            self.assertEqual(len(frame['pose']), 200)
            self.assertEqual(frame['pose']['bone100']['position'], [100, 1, 2])
            self.assertEqual(frame['pose']['bone100']['rotation'], [3, 4, 5])
            self.assertEqual(frame['pose']['bone100']['scale'], [2, 2, 2])
            self.assertEqual(frame['pose']['bone199']['position'], [199, 1, 2])

    def test_source_invalid_transition_is_zero_and_later_transition_still_runs(self):
        controller = {'states': {
            'default': {'animations': ['a'], 'on_entry': ['v.sword_a=3;'],
                        'transitions': [{'broken': "v.sword_a==3(ctrl.swing('mainhand',':sword'))"},
                                        {'working': 'q.go'}]},
            'broken': {'animations': ['bad']}, 'working': {'animations': ['b']}}}
        report = self.invoke({'player.main': controller},
                             {'a': self.clip(1), 'bad': self.clip(99), 'b': self.clip(2)},
                             [{'time': 0}, {'time': .1, 'queries': {'q.go': 1}}],
                             source_profile=True, full_report=True)
        self.assertEqual([self.state(f) for f in report['frames']], ['default', 'working'])
        self.assertEqual(report['frames'][-1]['pose']['root']['position'][0], 2)
        self.assertTrue(any('player.main/default/transitions/broken' in d and 'replaced with 0' in d
                            for d in report['frames'][0]['controllers']['diagnostics']))

    def test_source_invalid_entry_exit_blocks_are_zero_without_partial_writes(self):
        controller = {'states': {
            'default': {'animations': ['a'], 'on_entry': ['v.partial=1;', '1(2);'],
                        'on_exit': ['v.exit_partial=1;', '1(2);'],
                        'transitions': [{'next': 'q.go'}]},
            'next': {'animations': ['a'], 'on_entry': ['v.valid=7;']}}}
        report = self.invoke({'player.main': controller}, {'a': self.clip(1)},
                             [{'time': 0}, {'time': .1, 'queries': {'q.go': 1}}],
                             source_profile=True, full_report=True)
        variables = report['frames'][-1]['variables']
        self.assertNotIn('variable.partial', variables)
        self.assertNotIn('variable.exit_partial', variables)
        self.assertEqual(variables['variable.valid'], 7)
        self.assertEqual(len(report['frames'][0]['controllers']['diagnostics']), 2)

    def test_strict_controller_still_rejects_malformed_source(self):
        controller = {'states': {'default': {'transitions': [{'default': '1(2)'}]}}}
        error = self.invoke({'player.main': controller}, {}, [{'time': 0}], succeeds=False)
        self.assertIn('Unexpected token (', error)

    def test_initial_state_and_first_matching_transition_preserve_declaration_order(self):
        controller = {'initial_state': 'start', 'states': {
            'start': {'animations': ['a'], 'on_entry': ['v.n+=1;'], 'on_exit': ['v.n+=10;'],
                      'transitions': [{'second': 'q.go'}, {'first': 'q.go'}]},
            'first': {'animations': ['a']},
            'second': {'animations': ['b'], 'on_entry': ['v.n+=100;']}}}
        frames = self.invoke({'player.main': controller}, {'a': self.clip(1), 'b': self.clip(2)},
                             [{'time': 0, 'queries': {'q.go': 1}}, {'time': 1, 'queries': {'q.go': 1}}])
        self.assertEqual([self.state(f) for f in frames], ['start', 'second'])
        self.assertEqual(frames[1]['variables']['variable.n'], 111)

    def test_hold_terminal_instruction_uses_source_f32_tick_coordinate(self):
        controller={'initial_state':'state','states':{
            'state':{'transitions':[{'run':'ctrl.run'}]},
            'run':{'transitions':[{'runEnd':'!ctrl.run'}]},
            'runEnd':{'animations':['run_end'],'transitions':[{'state':'v.runEnd==1'}],
                      'on_exit':['v.runEnd=0;']}}}
        clip={'loop':'hold_on_last_frame','animation_length':0.41669997572898865,
              'timeline':{'0.4167':['v.runEnd=1;']}}
        frames=self.invoke({'player.parallel_7':controller},{'run_end':clip},
            [dict(time=t,state=s) for t,s in [(0,'run'),(.1,'run'),(.2,'idle'),(.4,'idle'),(.8,'idle'),(1.2,'idle'),(2,'idle')]],
            player=True,source_profile=True)
        self.assertEqual(self.state(frames[2],'player.parallel_7'),'runEnd')
        self.assertEqual(self.state(frames[-1],'player.parallel_7'),'state')

    def test_empty_state_chain_and_cycle_stop_in_the_same_update(self):
        controller = {'states': {'default': {'transitions': [{'a': 1}]},
                                 'a': {'transitions': [{'b': 1}]}, 'b': {'transitions': [{'a': 1}]}}}
        frames = self.invoke({'player.main': controller}, {}, [{'time': 0}])
        self.assertEqual(self.state(frames[0]), 'b')
        self.assertTrue(any('cycle' in d for d in frames[0]['controllers']['diagnostics']))

    def test_multiple_conditional_animations_sum_vectors_and_multiply_scale(self):
        clips = {'a': {'loop': True, 'bones': {'root': {'position': [2, 0, 0], 'scale': 2}}},
                 'b': {'loop': True, 'bones': {'root': {'position': [3, 0, 0], 'scale': 3}}}}
        controller = {'states': {'default': {'animations': ['a', {'b': 'q.apply'}]}}}
        frames = self.invoke({'player.main': controller}, clips,
                             [{'time': 0, 'queries': {'q.apply': 0}}, {'time': 1, 'queries': {'q.apply': 0.2}}])
        self.assertEqual(frames[0]['pose']['root']['position'][0], 2)
        self.assertEqual(frames[1]['pose']['root']['position'][0], 5)
        self.assertEqual(frames[1]['pose']['root']['scale'], [6, 6, 6])

    def test_clip_blend_weight_uses_linear_scale_weight(self):
        clip = {'loop': True, 'blend_weight': 0.25,
                'bones': {'root': {'position': [8, 0, 0], 'rotation': [40, 0, 0], 'scale': 3}}}
        frames = self.invoke({'player.main': {'states': {'default': {'animations': ['a']}}}}, {'a': clip}, [{'time': 0}])
        self.assertEqual(frames[0]['pose']['root']['position'][0], 2)
        self.assertEqual(frames[0]['pose']['root']['rotation'][0], 10)
        self.assertEqual(frames[0]['pose']['root']['scale'], [1.5, 1.5, 1.5])

    def test_false_condition_advances_clock_and_defers_instructions_until_enabled(self):
        controller = {'states': {'default': {'animations': [{'a': 'q.apply'}]}}}
        clip = {'animation_length': 4, 'bones': {'root': {'position': ['q.anim_time', 0, 0]}},
                'timeline': {'0': 'v.n+=1;', '1': 'v.n+=10;', '2': 'v.n+=100;'}}
        frames = self.invoke({'player.main': controller}, {'a': clip},
                             [{'time': t, 'queries': {'q.apply': active}} for t, active in [(0, 0), (1, 0), (2, 1)]])
        self.assertNotIn('variable.n', frames[1]['variables'])
        self.assertEqual(frames[2]['variables']['variable.n'], 111)
        self.assertEqual(frames[2]['pose']['root']['position'][0], 2)

    def test_repeated_draw_time_does_not_reenter_state_or_repeat_events(self):
        controller = {'states': {'default': {'animations': ['a'], 'on_entry': ['v.entry+=1;'],
                                             'transitions': [{'default': 'q.reset'}]}}}
        clip = {'loop': True, 'timeline': {'0': 'v.event+=1;'}}
        frames = self.invoke({'player.main': controller}, {'a': clip},
                             [{'time': 0}, {'time': 1, 'queries': {'q.reset': 1}},
                              {'time': 1, 'queries': {'q.reset': 1}}])
        self.assertEqual([f['variables']['variable.entry'] for f in frames], [1, 2, 2])
        self.assertEqual([f['variables']['variable.event'] for f in frames], [1, 2, 2])

    def test_loop_completion_becomes_visible_to_next_transition_update(self):
        controller = {'states': {'default': {'animations': ['a'],
                                             'transitions': [{'done': 'q.all_animations_finished'}]},
                                 'done': {'animations': ['b']}}}
        frames = self.invoke({'player.main': controller}, {'a': self.clip(1, animation_length=1), 'b': self.clip(2)},
                             [{'time': t} for t in [0, 0.5, 1.1, 1.2]])
        self.assertEqual([self.state(f) for f in frames], ['default', 'default', 'default', 'done'])

    def test_unapplied_animation_is_excluded_from_completion_queries(self):
        controller = {'states': {'default': {'animations': [{'a': 0}],
                                             'transitions': [{'done': 'q.any_animation_finished&&q.all_animations_finished'}]},
                                 'done': {'animations': ['b']}}}
        frames = self.invoke({'player.main': controller}, {'a': self.clip(1, animation_length=10), 'b': self.clip(2)},
                             [{'time': 0}, {'time': 0.1}])
        self.assertEqual(self.state(frames[1]), 'done')

    def test_nested_controller_exits_before_parent_and_new_entry(self):
        controllers = {'player.main': {'initial_state': 'ysm-entry-sub', 'states': {
            'ysm-entry-sub': {'on_entry': ['v.seq=1;'], 'on_exit': ['v.seq=v.seq*10+4;'],
                              'transitions': [{'done': 'q.quit'}]},
            'done': {'animations': ['a'], 'on_entry': ['v.seq=v.seq*10+5;']}}},
            'player.main.sub': {'states': {'default': {'animations': ['a'],
                                                      'on_entry': ['v.seq=v.seq*10+2;'],
                                                      'on_exit': ['v.seq=v.seq*10+3;']}}}}
        frames = self.invoke(controllers, {'a': self.clip(1)}, [{'time': 0}, {'time': 1, 'queries': {'q.quit': 1}}])
        self.assertEqual(frames[0]['variables']['variable.seq'], 12)
        self.assertEqual(frames[1]['variables']['variable.seq'], 12345)
        self.assertNotIn('player.main.sub', frames[1]['controllers']['states'])

    def test_shared_clip_has_separate_controller_event_cursors(self):
        controller = {'states': {'default': {'animations': ['a']}}}
        clips = {'a': {'loop': True, 'animation_length': 2, 'timeline': {'0': 'v.n+=1;'}}}
        frames = self.invoke({'player.a': controller, 'player.b': controller}, clips,
                             [{'time': 0}, {'time': 1}], roots=['player.a', 'player.b'])
        self.assertEqual([f['variables']['variable.n'] for f in frames], [2, 2])

    def test_earlier_controller_instruction_can_select_later_controller_state(self):
        controllers = {'player.a': {'states': {'default': {'animations': ['a']}}},
                       'player.b': {'states': {'default': {'transitions': [{'on': 'v.go'}]},
                                               'on': {'animations': ['b']}}}}
        clips = {'a': {'timeline': {'0': 'v.go=1;'}}, 'b': self.clip(3)}
        frames = self.invoke(controllers, clips, [{'time': 0}], roots=['player.a', 'player.b'])
        self.assertEqual(self.state(frames[0], 'player.b'), 'on')
        self.assertEqual(frames[0]['pose']['root']['position'][0], 3)

    def test_blend_delays_clip_clock_and_time_zero_instruction_until_completion(self):
        controller = {'states': {'default': {'animations': ['a'], 'transitions': [{'next': 'q.go'}]},
                                 'next': {'animations': ['b'], 'blend_transition': 1}}}
        clips = {'a': self.clip(10), 'b': {'loop': True, 'animation_length': 4, 'timeline': {'0': 'v.started+=1;'},
                                         'bones': {'root': {'position': ['20+q.anim_time', 0, 0]}}}}
        frames = self.invoke({'player.main': controller}, clips,
                             [{'time': t, 'queries': {'q.go': t >= 1}} for t in [0, 1, 1.5, 2, 2.5]])
        self.assertEqual([f['pose']['root']['position'][0] for f in frames], [10, 10, 15, 20, 20.5])
        self.assertNotIn('variable.started', frames[2]['variables'])
        self.assertEqual(frames[4]['variables']['variable.started'], 1)

    def test_segmented_blend_values_describe_remaining_source_weight(self):
        controller = {'states': {'default': {'animations': ['a'], 'transitions': [{'next': 'q.go'}]},
                                 'next': {'animations': ['b'], 'blend_transition': {'0': 1, '0.5': 0.75, '1': 0}}}}
        frames = self.invoke({'player.main': controller}, {'a': self.clip(10), 'b': self.clip(20)},
                             [{'time': t, 'queries': {'q.go': t >= 1}} for t in [0, 1, 1.5]])
        self.assertEqual(frames[2]['pose']['root']['position'][0], 12.5)

    def test_builtin_state_delegates_to_coded_controller(self):
        frames = self.invoke({'player.main': {'initial_state': 'ysm-builtin', 'states': {'ysm-builtin': {}}}}, {}, [{'time': 0}])
        self.assertFalse(frames[0]['handled']['player.main'])

    def test_instruction_array_is_one_program_with_return_scope(self):
        controller = {'states': {'default': {'on_entry': ['v.a=1', 'return;', 'v.b=2;']}}}
        frames = self.invoke({'player.main': controller}, {}, [{'time': 0}])
        self.assertEqual(frames[0]['variables']['variable.a'], 1)
        self.assertNotIn('variable.b', frames[0]['variables'])

    def test_unresolved_state_references_are_rejected(self):
        for controller in [{'initial_state': 'missing', 'states': {'default': {}}},
                           {'states': {'default': {'transitions': [{'missing': 1}]}}}]:
            with self.subTest(controller=controller):
                self.invoke({'player.main': controller}, {}, [{'time': 0}], succeeds=False)

    def test_rewind_restarts_owned_timeline_without_stale_cursor(self):
        controller = {'states': {'default': {'animations': ['a']}}}
        clip = {'loop': True, 'animation_length': 2, 'timeline': {'0': 'v.n+=1;'}}
        frames = self.invoke({'player.main': controller}, {'a': clip}, [{'time': t} for t in [0, 0.5, 0]])
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 1, 2])

    def test_player_controller_order_and_parallel_override(self):
        controllers = {name: {'states': {'default': {'animations': [clip]}}} for name, clip in [
            ('player.post_main_probe', 'post_main'), ('player.post_use_probe', 'post_use'),
            ('player.parallel_0', 'custom_parallel')]}
        clips = {name: {'loop': True, 'bones': {'root': {'rotation': [value, 0, 0]}}}
                 for name, value in [('idle', 10), ('hold_mainhand:sword', 20), ('post_main', 30),
                                     ('post_use', 40), ('custom_parallel', 50), ('parallel0', 100), ('parallel1', 3)]}
        frames = self.invoke(controllers, clips, [{'time': 0, 'items': [{'id': 'minecraft:iron_sword', 'category': 'sword'}]}], player=True)
        self.assertEqual(frames[0]['pose']['root']['rotation'][0], 53)

    def test_player_custom_hand_controller_suppresses_coded_hold_then_resumes_at_zero(self):
        controller = {'states': {'default': {'animations': ['custom'], 'transitions': [{'ysm-builtin': 'q.resume'}]},
                                 'ysm-builtin': {}}}
        clips = {'idle': {'loop': True, 'bones': {'root': {'rotation': [10, 0, 0]}}},
                 'custom': {'loop': True, 'bones': {'root': {'position': [4, 0, 0]}}},
                 'hold_mainhand:sword': {'loop': True, 'animation_length': 4, 'bones': {'root': {'rotation': ['20+q.anim_time', 0, 0]}},
                                        'timeline': {'0': 'v.hold_started+=1;', '1': 'v.hold_started+=100;'}}}
        frames = self.invoke({'player.hold_mainhand': controller}, clips,
                             [{'time': t, 'items': [{'id': 'minecraft:iron_sword', 'category': 'sword'}],
                               'queries': {'q.resume': resume}} for t, resume in [(0, 0), (5, 0), (6, 1), (6.5, 1)]], player=True)
        self.assertEqual(frames[1]['pose']['root']['rotation'][0], 10)
        self.assertEqual(frames[2]['pose']['root']['rotation'][0], 20)
        self.assertEqual(frames[3]['pose']['root']['rotation'][0], 20.5)
        self.assertEqual(frames[3]['variables']['variable.hold_started'], 1)

    def test_player_item_filter_uses_identifier_tag_category_and_use_action(self):
        controller = {'states': {'default': {'animations': [{'a': "ctrl.hold('mainhand','$test:tool')&&ctrl.hold('offhand','#test:tag')&&ctrl.hold('mainhand',':pickaxe')&&ctrl.use('offhand',':block')"}]}}}
        items = [{'id': 'test:tool', 'category': 'pickaxe'}, {'id': 'test:shield', 'tags': ['test:tag'], 'use_action': 'block'}]
        frames = self.invoke({'player.post_use_probe': controller}, {'a': self.clip(4)},
                             [{'time': 0, 'items': items, 'using_hand': 1},
                              {'time': 1, 'items': items}], player=True)
        self.assertEqual(frames[0]['pose']['root']['position'][0], 4)
        self.assertNotIn('root', frames[1]['pose'])

    def test_player_stages_execute_events_before_following_controller(self):
        controllers = {'player.pre_parallel_probe': {'states': {'default': {'animations': ['signal']}}},
                       'player.post_main_probe': {'states': {'default': {'transitions': [{'on': 'v.signal'}]},
                                                             'on': {'animations': ['on']}}}}
        clips = {'signal': {'timeline': {'0': 'v.signal=1;'}}, 'on': self.clip(9)}
        frames = self.invoke(controllers, clips, [{'time': 0}], player=True)
        self.assertEqual(self.state(frames[0], 'player.post_main_probe'), 'on')
        self.assertEqual(frames[0]['pose']['root']['position'][0], 9)

    def test_once_ending_keeps_last_drawn_pose_after_a_dropped_frame_then_retires(self):
        controllers = {name: {'states': {'default': {'animations': [clip]}}}
                       for name, clip in [('player.lower', 'lower'), ('player.main', 'once')]}
        clips = {'lower': self.clip(2), 'once': {'animation_length': 1,
                 'bones': {'root': {'position': ['10*q.anim_time', 0, 0]}}}}
        frames = self.invoke(controllers, clips, [{'time': t} for t in [0, 0.7, 1.2, 1.275, 1.36, 2]],
                             roots=['player.lower', 'player.main'])
        values = [f['pose']['root']['position'][0] for f in frames]
        for actual, expected in zip(values, [0, 7, 7, 2.75, 2, 2]):
            self.assertAlmostEqual(actual, expected)
        self.assertEqual([f['controllers']['states']['player.main']['animations'][0]['phase'] for f in frames],
                         ['running', 'running', 'ending', 'ending', 'idle', 'idle'])

    def test_shared_channel_keeps_running_clip_during_ending_and_observation(self):
        controller = {'states': {'default': {'animations': ['once', 'loop']}}}
        clips = {'once': dict(self.clip(10, animation_length=1), loop=False,
                              timeline={'0': 'v.started+=1;'}),
                 'loop': self.clip(3)}
        frames = self.invoke({'player.main': controller}, clips,
                             [{'time': 0}, {'time': 1}, {'time': 1.075},
                              {'time': 1.075, 'observe': True}, {'time': 1.2}], player=True)
        for frame, expected in zip(frames, [13, 13, 8, 8, 3]):
            self.assertAlmostEqual(frame['pose']['root']['position'][0], expected)
            self.assertEqual(frame['logical_variables']['variable.started'], 1)

    def test_literal_vectors_preserve_the_last_input_axis_for_following_events(self):
        controllers = {name: {'states': {'default': {'animations': [clip]}}} for name, clip in
                       [('player.lower', 'lower'), ('player.main', 'vector'), ('player.tail', 'record')]}
        for value, expected in [([4, 5, 6], 30), (7, 10)]:
            with self.subTest(value=value):
                clips = {'lower': {'loop': True, 'bones': {'root': {'position': [10, 20, 30]}}},
                         'vector': {'loop': True, 'bones': {'root': {'position': value}}},
                         'record': {'timeline': {'0': 'v.input_axis=this;'}}}
                frames = self.invoke(controllers, clips, [{'time': 0}],
                                     roots=['player.lower', 'player.main', 'player.tail'])
                self.assertEqual(frames[0]['variables']['variable.input_axis'], expected)

    def test_once_finishes_at_declared_boundary_and_transition_observes_it_next_update(self):
        controller = {'states': {'default': {'animations': ['once'],
                                             'transitions': [{'done': 'q.all_animations_finished'}]},
                                 'done': {'animations': ['done']}}}
        frames = self.invoke({'player.main': controller},
                             {'once': dict(self.clip(10, animation_length=1), loop=False), 'done': self.clip(2)},
                             [{'time': t} for t in [0, 0.5, 1, 1.01]])
        self.assertEqual([self.state(f) for f in frames], ['default', 'default', 'default', 'done'])
        self.assertTrue(frames[2]['controllers']['states']['player.main']['animations'][0]['finished'])

    def test_running_animation_keeps_group_active_while_once_contribution_fades(self):
        controller = {'states': {'default': {'animations': ['once', 'running']}}}
        clips = {'once': dict(self.clip(10, animation_length=1), loop=False), 'running': self.clip(3)}
        frames = self.invoke({'player.main': controller}, clips,
                             [{'time': t} for t in [0, 0.5, 1, 1.075, 1.16]])
        for frame, expected in zip(frames, [13, 13, 13, 8, 3]):
            self.assertAlmostEqual(frame['pose']['root']['position'][0], expected)

    def test_scale_track_disables_ending_for_all_channels_of_its_bone_only(self):
        controllers = {name: {'states': {'default': {'animations': [clip]}}}
                       for name, clip in [('player.lower', 'lower'), ('player.main', 'once')]}
        clips = {'lower': {'loop': True, 'bones': {'root': {'position': [2, 0, 0], 'rotation': [3, 0, 0], 'scale': 2},
                                                 'plain': {'position': [4, 0, 0]}}},
                 'once': {'animation_length': 1, 'bones': {'root': {'position': [8, 0, 0], 'rotation': [12, 0, 0], 'scale': 1},
                                                         'plain': {'position': [10, 0, 0]}}}}
        frames = self.invoke(controllers, clips, [{'time': t} for t in [0, 0.5, 1, 1.075]],
                             roots=['player.lower', 'player.main'])
        self.assertEqual(frames[2]['pose']['root']['position'][0], 8)
        self.assertEqual(frames[3]['pose']['root']['position'], [2, 0, 0])
        self.assertAlmostEqual(frames[3]['pose']['root']['rotation'][0], 3)
        self.assertEqual(frames[3]['pose']['root']['scale'], [2, 2, 2])
        self.assertAlmostEqual(frames[3]['pose']['plain']['position'][0], 4.5)

    def test_ending_reevaluates_weight_at_frozen_clock_without_resampling_channels(self):
        controller = {'states': {'default': {'animations': ['once']}}}
        clip = {'animation_length': 1, 'blend_weight': 'v.weight_time=q.anim_time; return q.weight;',
                'bones': {'root': {'position': ['v.samples+=1; return 10;', 0, 0]}}}
        frames = self.invoke({'player.main': controller}, {'once': clip},
                             [{'time': t, 'queries': {'q.weight': w}} for t, w in [(0, 1), (0.5, 1), (1, 2), (1.075, 4), (1.16, 5)]])
        self.assertEqual([f['variables']['variable.samples'] for f in frames], [1, 2, 2, 2, 2])
        self.assertEqual(frames[2]['pose']['root']['position'][0], 20)
        self.assertAlmostEqual(frames[3]['pose']['root']['position'][0], 10)
        self.assertEqual(frames[3]['variables']['variable.weight_time'], 1)

    def test_once_flushes_remaining_instructions_at_end_once_including_authored_late_events(self):
        controller = {'states': {'default': {'animations': ['once']}}}
        clip = {'animation_length': 1, 'timeline': {'0': 'v.n+=1;', '0.5': 'v.n+=10; v.draw_time=q.anim_time;',
                '1': 'v.n+=100;', '1.5': 'v.n+=1000; v.flush_time=q.anim_time;'}}
        frames = self.invoke({'player.main': controller}, {'once': clip},
                             [{'time': t} for t in [0, 0.7, 1.2, 1.2, 1.275, 2]])
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 11, 1111, 1111, 1111, 1111])
        self.assertEqual(frames[1]['variables']['variable.draw_time'], 0.7)
        self.assertEqual(frames[2]['variables']['variable.flush_time'], 1)

    def test_suppressed_once_completion_does_not_dispatch_or_replay_its_ending_instructions(self):
        controller = {'states': {'default': {'animations': [{'once': 'q.apply'}]}}}
        clip = {'animation_length': 1, 'timeline': {'0': 'v.n+=1;', '1.5': 'v.n+=100;'}}
        frames = self.invoke({'player.main': controller}, {'once': clip},
                             [{'time': t, 'queries': {'q.apply': active}} for t, active in [(0, 1), (0.5, 0), (1.2, 0), (1.275, 1), (2, 1)]])
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 1, 1, 1, 1])

    def test_zero_length_once_draws_initial_frame_then_ends_without_restarting(self):
        controller = {'states': {'default': {'animations': ['once']}}}
        clip = {'animation_length': 0, 'timeline': {'0': 'v.n+=1;'}, 'bones': {'root': {'position': [10, 0, 0]}}}
        frames = self.invoke({'player.main': controller}, {'once': clip},
                             [{'time': t} for t in [0, 0.1, 0.175, 0.26, 1]])
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 1, 1, 1, 1])
        self.assertAlmostEqual(frames[2]['pose']['root']['position'][0], 2.5)
        self.assertNotIn('root', frames[4]['pose'])

    def test_loop_and_hold_sample_last_frame_at_exact_boundary_without_ending(self):
        for loop, expected in [(True, 2.5), ('hold_on_last_frame', 10)]:
            with self.subTest(loop=loop):
                clip = {'animation_length': 1, 'loop': loop, 'bones': {'root': {'position': ['10*q.anim_time', 0, 0]}}}
                frames = self.invoke({'player.main': {'states': {'default': {'animations': ['a']}}}}, {'a': clip},
                                     [{'time': t} for t in [0, 1, 1.25]])
                self.assertEqual(frames[1]['pose']['root']['position'][0], 10)
                self.assertEqual(frames[2]['pose']['root']['position'][0], expected)
                self.assertEqual(frames[2]['controllers']['states']['player.main']['animations'][0]['phase'], 'running')

    def test_negative_blend_weight_clamps_to_zero_without_inverting_scale(self):
        clip = {'loop': True, 'blend_weight': -2,
                'bones': {'root': {'position': [8, 0, 0], 'rotation': [40, 0, 0], 'scale': 3}}}
        frames = self.invoke({'player.main': {'states': {'default': {'animations': ['a']}}}}, {'a': clip}, [{'time': 0}])
        self.assertEqual(frames[0]['pose']['root'], {'position': [0, 0, 0], 'rotation': [0, 0, 0], 'scale': [1, 1, 1]})

    def test_animation_instruction_array_respects_authored_return_scope(self):
        clip = {'loop': True, 'timeline': {'0': ['v.a=1;', 'return;', 'v.b=2;']}}
        controllers = {'player.main': {'states': {'default': {'animations': ['a']}}}}
        for merge in (True, False):
            with self.subTest(merge=merge):
                frames = self.invoke(controllers, {'a': clip}, [{'time': 0}], merge_multiline=merge)
                self.assertEqual(frames[0]['variables']['variable.a'], 1)
                self.assertEqual('variable.b' in frames[0]['variables'], not merge)

    def test_legacy_dotted_variables_are_available_between_separate_instructions(self):
        clip = {'loop': True, 'timeline': {'0': ['t.swing.axis.x=7;v.swing.axis.x=t.swing.axis.x;',
                                                'v.swing.axis.x+=3;', 'v.result=v.swing.axis.x+(t.swing.axis.x??0);']}}
        controllers = {'player.main': {'states': {'default': {'animations': ['a']}}}}
        frames = self.invoke(controllers, {'a': clip}, [{'time': 0}], legacy_paths=True)
        self.assertEqual(frames[0]['variables']['variable.result'], 10)

    def test_release_inverse_trig_arithmetic_and_nonfinite_projection(self):
        clip = {"loop": True, "timeline": {"0": [
            "v.a=math.acos(0);v.b=math.asin(0.5);v.c=math.atan(1);v.d=math.atan2(1,0);",
            "t.r.x=math.acos(1.0000001);t.r.w=1;v.bad=(t.r.x??9)+7;v.good=t.r.w;",
            "v.float=16777216+1-16777216;v.sqrt=(math.sqrt(-1)??9)+7;v.ok=1;",
        ]}}
        controllers = {"player.main": {"states": {"default": {"animations": ["a"]}}}}
        frame, = self.invoke(controllers, {"a": clip}, [{"time": 0}], legacy_paths=True)
        from math import pi
        for key, value in zip(("a", "b", "c", "d", "bad", "good", "float", "sqrt", "ok"),
                              (pi/2, pi/6, pi/4, pi/2, 7, 1, 0, 7, 1)):
            self.assertAlmostEqual(frame["variables"]["variable."+key], value, places=6)

    def test_legacy_author_struct_arguments_and_returns_are_values(self):
        document = {"animations": {"a": {"loop": True, "timeline": {"0": [
            "v.p=5;v.p.x=3;v.p.w=7;v.copy=fn.edit(v.p);",
            "v.result=v.p.x*100+v.copy.x*10+v.copy.w;",
            "v.absent=(v.p.z??11);v.scalar=v.p+1;",
            "v.p.label='hello';v.p.other=9;v.promoted=v.p.x+v.p.w+v.p.other;",
            "v.p=2;v.cleared=(v.p.x??12);"
        ]}}}, "animation_controllers": {"player.main": {"states": {
            "default": {"animations": ["a"]}}}},
            "ysm_functions": {"edit": "t.out=args[0];t.out.x=4;return t.out;"},
            "ysm_legacy_molang": True}
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp, 'struct.json')
            path.write_text(json.dumps(document), encoding='utf-8')
            process = subprocess.run([str(EXE), '--player-animation-file', str(path),
                                      json.dumps({'frames': [{'time': 0}]})],
                                     check=True, capture_output=True, encoding='utf-8')
        report = json.loads(process.stdout)
        self.assertEqual(report['expression_failures'], 0, report['evaluation_diagnostics'])
        values = report['frames'][0]['variables']
        self.assertEqual([values["variable." + key] for key in
                          ("result", "absent", "scalar", "promoted", "cleared")], [347, 11, 2, 19, 12])
        self.assertEqual(values["variable.copy"], {"x": 4, "w": 7})

    def test_legacy_effect_curves_handle_large_exponents(self):
        clip = {'loop': True, 'timeline': {'0': [
            'v.small=1/(1+math.exp(100));',
            'v.zero=1.0/(math.exp(1000)+1.0);',
            'v.half=1/(1+math.exp(0));',
        ]}}
        controllers = {'player.main': {'states': {'default': {'animations': ['a']}}}}
        frame, = self.invoke(controllers, {'a': clip}, [{'time': 0}], legacy_paths=True)
        variables = frame['variables']
        self.assertTrue(math.isclose(variables['variable.small'], math.exp(-100)))
        self.assertEqual(variables['variable.zero'], 0)
        self.assertEqual(variables['variable.half'], .5)

    def test_legacy_first_order_stays_finite_across_a_skipped_render_interval(self):
        clip = self.clip("ysm.first_order('spring',q.target,.05)")
        controller = {'states': {'default': {'animations': ['smooth']}}}
        frames = self.invoke({'player.main': controller}, {'smooth': clip},
                             [{'time': time, 'queries': {'q.target': target}}
                              for time, target in [(.05, 5), (.1, 7), (.25, 9), (.3, 11), (60, 13)]],
                             legacy_paths=True)
        for frame, expected in zip(frames, [5, 5, 7, 9, 11]):
            self.assertAlmostEqual(frame['pose']['root']['position'][0], expected, places=4)

    def test_legacy_timeline_annotations_leave_nested_instructions_intact(self):
        clip = {'loop': True, 'timeline': {'0': [
            "['髪の動き';", 'v.a=1;', '[;', " '注释：ysm.bone_rot('hat').x'; ",
            'v.a+=2;', '];', 'v.a*=3;', '];',
        ]}}
        controllers = {'player.main': {'states': {'default': {'animations': ['a']}}}}
        for merge in (True, False):
            with self.subTest(merge=merge):
                frame, = self.invoke(controllers, {'a': clip}, [{'time': 0}],
                                     legacy_paths=True, merge_multiline=merge)
                self.assertEqual(frame['variables']['variable.a'], 9)

    def test_first_running_update_after_long_blend_does_not_replay_flushed_instructions(self):
        controller = {'states': {'default': {'animations': ['once'], 'blend_transition': 1}}}
        clip = {'animation_length': 0.1, 'timeline': {'0': 'v.n+=1;', '0.15': 'v.n+=10;', '0.5': 'v.n+=100;'},
                'bones': {'root': {'position': [10, 0, 0]}}}
        frames = self.invoke({'player.main': controller}, {'once': clip},
                             [{'time': t} for t in [0, 0.5, 1.2, 1.21, 1.3, 2]])
        self.assertNotIn('variable.n', frames[1]['variables'])
        self.assertEqual([f['variables']['variable.n'] for f in frames[2:]], [11, 111, 111, 111])

    def test_retired_once_restarts_only_after_state_reload(self):
        controller = {'states': {'default': {'animations': ['once'], 'transitions': [{'default': 'q.reload'}]}}}
        clip = {'animation_length': 0.1, 'timeline': {'0': 'v.n+=1;'}, 'bones': {'root': {'position': [10, 0, 0]}}}
        frames = self.invoke({'player.main': controller}, {'once': clip},
                             [{'time': t, 'queries': {'q.reload': t == 1}} for t in [0, 0.2, 0.4, 0.8, 1]])
        self.assertNotIn('root', frames[3]['pose'])
        self.assertEqual(frames[4]['variables']['variable.n'], 2)
        self.assertEqual(frames[4]['pose']['root']['position'][0], 10)

    def upstream_astronaut(self):
        # Read the checked-out upstream files without rewriting the model or
        # copying its licensed assets into the distributable test fixtures.
        base = PROJECT / 'vendor/ysm-upstream/src/main/resources/assets/ysm/builtin/wine_fox/03_astronaut'
        if not base.exists():
            self.skipTest('Upstream reference checkout is not included in this package')
        controller = json.loads((base / 'controller/controller.json').read_text(encoding='utf-8'))['animation_controllers']['player.parallel_6']
        references = set()
        for state in controller['states'].values():
            for choice in state.get('animations', []):
                references.update([choice] if isinstance(choice, str) else choice)
        clips = {}
        for path in (base / 'animations').glob('*.json'):
            clips.update({name: clip for name, clip in json.loads(path.read_text(encoding='utf-8'))['animations'].items()
                          if name in references})
        self.assertEqual(set(clips), references)
        return {'player.parallel_6': controller}, clips

    def test_upstream_astronaut_three_attack_combo_and_conditional_completion(self):
        controllers, clips = self.upstream_astronaut()
        frames = self.invoke(controllers, clips, [
            {'time': 0, 'queries': {'ctrl.idle': 1}},
            {'time': 0.1, 'variables': {'v.swing_sword': 1}},
            {'time': 0.2},
            {'time': 0.4, 'variables': {'v.swing_sword': 1}},
            {'time': 0.5},
            {'time': 0.7, 'variables': {'v.swing_sword': 1}},
            {'time': 0.725}, {'time': 0.8}, {'time': 1.7}, {'time': 1.71}], roots=['player.parallel_6'])
        self.assertEqual([self.state(f, 'player.parallel_6') for f in frames],
                         ['default', 'attack1', 'attack1', 'attack2', 'attack2', 'attack3', 'attack3', 'attack3', 'attack3', 'default'])
        players = frames[8]['controllers']['states']['player.parallel_6']['animations']
        self.assertFalse(players[0]['applied'])
        self.assertTrue(players[1]['applied'])
        self.assertTrue(players[1]['finished'])
        self.assertEqual(frames[-1]['controllers']['diagnostics'], [])

    def test_upstream_astronaut_jump_attack_completes_without_spurious_combo_restart(self):
        controllers, clips = self.upstream_astronaut()
        frames = self.invoke(controllers, clips,
                             [{'time': 0}, {'time': 0.1, 'variables': {'v.swing_sword': 1, 'v.jump': 1}},
                              {'time': 0.3}, {'time': 0.8}, {'time': 0.81}, {'time': 1}], roots=['player.parallel_6'])
        self.assertEqual([self.state(f, 'player.parallel_6') for f in frames],
                         ['default', 'attack_jump', 'attack_jump', 'attack_jump', 'default', 'default'])
        self.assertEqual(frames[-1]['variables']['variable.swing_sword'], 0)
        self.assertEqual(frames[-1]['controllers']['diagnostics'], [])

    def test_player_without_custom_controllers_uses_coded_use_ending_transition(self):
        clips = {'idle': self.clip(2), 'use_mainhand': {'animation_length': 2,
                 'bones': {'root': {'position': [10, 0, 0]}}}}
        frames = self.invoke({}, clips, [{'time': t, 'using_hand': hand} for t, hand in
                                         [(0, 0), (0.2, 0), (0.3, -1), (0.375, -1), (0.46, -1)]], player=True)
        for frame, expected in zip(frames[1:], [10, 10, 6, 2]):
            self.assertAlmostEqual(frame['pose']['root']['position'][0], expected)
        self.assertEqual(frames[3]['controllers']['coded']['player.use']['phase'], 'ending')
        self.assertEqual(frames[4]['controllers']['coded']['player.use']['phase'], 'idle')

    def test_player_hold_pause_advances_clock_and_resumes_without_reload(self):
        clips = {'idle': self.clip(0), 'hold_mainhand:bow': {'loop': True, 'animation_length': 3,
                 'timeline': {'0': 'v.held+=1;', '0.5': 'v.held+=10;'},
                 'bones': {'held': {'position': ['q.anim_time', 0, 0]}}},
                 'use_mainhand:bow': {'loop': True, 'animation_length': 3,
                 'bones': {'using': {'position': [10, 0, 0]}}}}
        item = {'id': 'minecraft:bow', 'category': 'bow'}
        frames = self.invoke({}, clips, [{'time': t, 'items': [item], 'using_hand': hand}
                                         for t, hand in [(0, -1), (0.2, -1), (0.4, 0), (0.8, 0), (1, -1)]], player=True)
        self.assertNotIn('held', frames[2]['pose'])
        self.assertNotIn('held', frames[3]['pose'])
        self.assertEqual(frames[3]['variables']['variable.held'], 1)
        self.assertEqual(frames[4]['variables']['variable.held'], 11)
        self.assertAlmostEqual(frames[4]['pose']['held']['position'][0], 0.9)
        self.assertEqual(frames[4]['controllers']['coded']['player.hold_mainhand']['generation'], 1)

    def test_extra_once_retires_and_explicit_time_reset_replays_it(self):
        clips = {'idle': self.clip(2), 'extra0': {'animation_length': 1, 'timeline': {'0': 'v.extra+=1;'},
                 'bones': {'root': {'position': [10, 0, 0]}}}}
        frames = self.invoke({}, clips,
                             [{'time': t, 'extra': 'extra0', 'extra_time': clock}
                              for t, clock in [(0, 0), (0.5, 0.5), (1, 1), (1.075, 1.075), (1.16, 1.16), (2, 0)]], player=True)
        for frame, expected in zip(frames, [10, 10, 10, 6, 2, 10]):
            self.assertAlmostEqual(frame['pose']['root']['position'][0], expected)
        self.assertEqual(frames[-1]['variables']['variable.extra'], 2)

    def test_builtin_fallback_inherits_authored_segmented_transition(self):
        controller = {'states': {'default': {'animations': ['custom'], 'transitions': [{'ysm-builtin': 'q.resume'}]},
                                 'ysm-builtin': {'blend_transition': {'0': 1, '0.5': 0.75, '1': 0}}}}
        clips = {'idle': self.clip(10), 'custom': self.clip(20)}
        frames = self.invoke({'player.main': controller}, clips,
                             [{'time': t, 'queries': {'q.resume': t >= 1}} for t in [0, 1, 1.5, 2]], player=True)
        self.assertEqual([f['pose']['root']['position'][0] for f in frames], [20, 20, 17.5, 10])

    def test_custom_loop_boundary_keeps_last_instruction_before_next_cycle_zero(self):
        clip = {'loop': True, 'animation_length': 1, 'timeline': {
            '0': 'v.n+=1;', '1': 'v.n+=10;', '1.1': 'v.n+=100;'},
            'bones': {'root': {'position': ['q.anim_time', 0, 0]}}}
        frames = self.invoke({'player.main': {'states': {'default': {'animations': ['a']}}}}, {'a': clip},
                             [{'time': t} for t in [0, 1, 1.01, 2, 2.01]])
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 11, 112, 122, 223])
        self.assertEqual(frames[1]['pose']['root']['position'][0], 1)
        self.assertEqual(frames[3]['pose']['root']['position'][0], 1)
        self.assertEqual([f['controllers']['states']['player.main']['animations'][0]['cycle'] for f in frames], [0, 0, 1, 1, 2])

    def test_zero_length_custom_loop_resumes_pending_instructions_without_hidden_cycle_replays(self):
        controller = {'states': {'default': {'animations': [{'a': 'q.apply'}]}}}
        clip = {'loop': True, 'animation_length': 0, 'timeline': {'0': 'v.n+=1;', '0.1': 'v.n+=10;'}}
        frames = self.invoke({'player.main': controller}, {'a': clip},
                             [{'time': t, 'queries': {'q.apply': active}} for t, active in [(0, 0), (0.1, 0), (0.2, 1), (0.2, 1), (0.3, 1)]])
        self.assertNotIn('variable.n', frames[1]['variables'])
        self.assertEqual([f['variables']['variable.n'] for f in frames[2:]], [12, 12, 23])

    def test_hold_last_frame_does_not_dispatch_instruction_authored_past_the_end(self):
        clip = {'loop': 'hold_on_last_frame', 'animation_length': 1,
                'timeline': {'0': 'v.n+=1;', '1': 'v.n+=10;', '1.1': 'v.n+=100;'}}
        frames = self.invoke({'player.main': {'states': {'default': {'animations': ['a']}}}}, {'a': clip},
                             [{'time': t} for t in [0, 1, 2, 3]])
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 11, 11, 11])

    def test_custom_players_sharing_clip_have_independent_boundary_instruction_positions(self):
        controller = {'states': {'default': {'animations': ['a']}}}
        clip = {'loop': True, 'animation_length': 1, 'timeline': {'0': 'v.n+=1;', '1': 'v.n+=10;', '1.1': 'v.n+=100;'}}
        frames = self.invoke({'player.a': controller, 'player.b': controller}, {'a': clip},
                             [{'time': t} for t in [0, 1, 1.01]], roots=['player.a', 'player.b'])
        self.assertEqual([f['variables']['variable.n'] for f in frames], [2, 22, 224])

    def test_timeline_catchup_budget_is_shared_across_controllers_in_the_frame(self):
        controllers = {name: {'states': {'default': {'animations': [clip]}}}
                       for name, clip in [('player.a', 'a'), ('player.b', 'b')]}
        # A genuinely dense authored timeline must still share the guard;
        # elapsed loop durations are not replayed by Java source players.
        clip = {'loop': True, 'animation_length': 10,
                'timeline': {str(i / 1000): 'v.n+=1;' for i in range(3000)}}
        report = self.invoke(controllers, {'a': clip, 'b': clip}, [{'time': t} for t in [0, 3, 3.01]],
                             roots=['player.a', 'player.b'], full_report=True)
        self.assertEqual([f['variables']['variable.n'] for f in report['frames']], [2, 4098, 4098])
        self.assertTrue(any('Timeline catch-up budget exceeded' in item for item in report['evaluation_diagnostics']))

    def test_dense_instruction_keyframes_leave_budget_for_bone_channels(self):
        command = 'v.n+=' + '+'.join(['1'] + ['0'] * 20) + ';'
        clip = {'loop': True, 'animation_length': .01,
                'timeline': {'0': [command] * 1000, '.01': [command] * 4000},
                'bones': {'tail': {'rotation': ['v.n', 0, 0]}}}
        controller = {'states': {'default': {'animations': ['a']}}}
        frames = self.invoke({'player.main': controller}, {'a': clip}, [{'time': 0}, {'time': .05}, {'time': .1}])
        self.assertEqual([f['pose']['tail']['rotation'][0] for f in frames], [1000, 6000, 11000])


if __name__ == '__main__':
    unittest.main()
