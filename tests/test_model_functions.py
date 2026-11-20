import json
import pathlib
import subprocess
import tempfile
import unittest
import zipfile

from test_import import EXE, SAMPLE


class ModelFunctionTests(unittest.TestCase):
    def test_literal_callable_preserves_nested_caller_temporary_and_arguments(self):
        report = self.expression('t.local=9;return fn.outer(5)+t.local;', {
            'outer': 't.local=args[0];v.literal=fn.literal();return t.local+args[0]+v.literal;',
            'literal': '7'})
        self.assertEqual(report['value'], 26)
        self.assertEqual(report['variables']['variable.literal'], 7)

    def test_nested_locals_and_arguments_restore_with_many_persistent_variables(self):
        source = ';'.join('v.saved' + str(i) + '=' + str(i) for i in range(500))
        source += ';t.outer=7;v.result=fn.outer(11);return t.outer;'
        report = self.expression(source, {
            'outer': 't.local=args[0]+1;v.inner=fn.inner(23);return t.local+args[0];',
            'inner': 't.local=99;return args[0]+1;'})
        self.assertEqual(report['value'], 7)
        self.assertEqual(report['variables']['variable.result'], 23)
        self.assertEqual(report['variables']['variable.inner'], 24)
        self.assertTrue(all(report['variables']['variable.saved' + str(i)] == i for i in range(500)))

    def expression(self, source, functions, frames=None, failures=0):
        inputs = {'functions': functions}
        if frames is not None:
            inputs['frames'] = frames
        run = subprocess.run([str(EXE), '--expr', source, json.dumps(inputs)], capture_output=True,
                             encoding='utf-8', timeout=5)
        self.assertEqual(run.returncode, 0, run.stderr)
        report = json.loads(run.stdout)
        self.assertEqual(report['failures'], failures, report)
        return report

    def player(self, functions, frames, clips=None, controllers=None, failures=0, compile_errors=False, local_sync=False):
        document = {'animations': clips or {'idle': {'loop': True, 'bones': {'root': {'position': [2, 0, 0]}}},
                                           'chosen': {'loop': True, 'bones': {'root': {'position': [7, 0, 0]}}}},
                    'ysm_functions': functions, 'animation_controllers': controllers or {}}
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'functions.json'
            path.write_text(json.dumps(document), encoding='utf-8')
            run = subprocess.run([str(EXE), '--player-animation-file', str(path), json.dumps({'frames': frames, 'local_sync': local_sync})],
                                 capture_output=True, encoding='utf-8', timeout=5)
        self.assertEqual(run.returncode, 0, run.stderr)
        report = json.loads(run.stdout)
        self.assertEqual(report['expression_failures'], failures, report)
        self.assertEqual(bool(report['compile_diagnostics']), compile_errors, report)
        return report

    @staticmethod
    def coded(frame, name='player.main'):
        return frame['controllers']['coded'][name]

    def test_nested_user_functions_isolate_locals_and_arguments(self):
        report = self.expression('t.x=11;t.value=fn.outer(3);return t.x*100+t.value;',
            {'outer': 't.x=5;return fn.inner(args[0]+1)+t.x;', 'inner': 't.x=99;return args[0]*2;'})
        self.assertEqual(report['value'], 1113)
        self.assertNotIn('args', report['variables'])

    def test_shared_variables_survive_calls_and_missing_arguments_preserve_presence(self):
        report = self.expression('fn.add(3);fn.add(4);return fn.read();',
            {'add': 'v.sum+=args[0];', 'read': 'return (args[0]??v.sum);'})
        self.assertEqual(report['value'], 7)
        self.assertEqual(report['variables']['variable.sum'], 7)

    def test_comments_bom_and_comment_markers_inside_strings(self):
        report = self.expression("return fn.check('http://x/*a*/');",
            {'check': "\ufeff// 作者说明\n/*block*/return args[0]=='http://x/*a*/';"})
        self.assertEqual(report['value'], 1)

    def test_recursive_calls_are_bounded_and_do_not_poison_later_frames(self):
        report = self.expression('return fn.recurse();',
            {'recurse': 'q.recurse?{return fn.recurse();};return 7;'},
            [{'queries': {'q.recurse': 1}}, {'queries': {'q.recurse': 0}}], failures=1)
        self.assertEqual(report['values'], [0, 7])
        self.assertTrue(any('nesting budget' in item for item in report['diagnostics']))

    def test_nested_failure_aborts_the_caller(self):
        report = self.expression('fn.bad();v.after=1;return 9;', {'bad': 'return math.abs(3.5e38);'}, failures=1)
        self.assertEqual(report['value'], 0)
        self.assertNotIn('variable.after', report['variables'])

    def test_init_update_order_and_repeated_observation(self):
        report = self.player({'a@player_init': 'v.order=v.order*10+1;v.initialized+=1;',
                              'b@player_init': 'v.order=v.order*10+2;',
                              '@player_update': 'v.updated+=1;v.update_arg=args;'},
                             [{'time': 0}, {'time': 0}, {'time': 1}])
        frames = report['frames']
        self.assertEqual([f['variables']['variable.initialized'] for f in frames], [1, 1, 1])
        self.assertEqual([f['variables']['variable.updated'] for f in frames], [1, 1, 2])
        self.assertEqual(frames[-1]['variables']['variable.order'], 12)
        self.assertEqual(frames[-1]['variables']['variable.update_arg'], 1)

    def test_equipped_flags_ids_armor_and_shield_use_the_actual_slots(self):
        report = self.player({'@player_update': """
            v.main=ysm.has_mainhand;
            v.off=ysm.has_offhand();
            v.armor=ysm.has_boots+2*ysm.has_leggings+4*ysm.has_chest_plate+8*ysm.has_helmet;
            v.wings=ysm.has_elytra;
            v.points=ysm.armor_value;
            v.shield_disabled=ysm.in_shield_block_cooldown;
            v.bow=q.is_item_name_any('Mainhand','stick','bow');
            v.rod=q.is_item_name_any('offhand','minecraft:end_rod');
            v.invalid=q.is_item_name_any('HEAD','INVALID')??-1;
            """},
            [{'time': 0, 'items': [{'id': 'minecraft:bow'}, {'id': 'minecraft:end_rod'}],
              'armor': {'head': {'id': 'minecraft:iron_helmet'}, 'chest': {'id': 'minecraft:elytra'},
                        'feet': {'id': 'minecraft:iron_boots'}},
              'armor_value': 4, 'shield_block_cooldown': True},
             {'time': .25, 'items': [{'id': 'minecraft:air'}]}])
        first, second = [frame['variables'] for frame in report['frames']]
        expected = {'main': 1, 'off': 1, 'armor': 13, 'wings': 1, 'points': 4, 'shield_disabled': 1,
                    'bow': 1, 'rod': 1, 'invalid': -1}
        for variable, value in expected.items():
            self.assertEqual(first['variable.' + variable], value, variable)
            self.assertEqual(second['variable.' + variable], 0, variable)

    def test_extra_animation_flag_spans_its_own_events_and_ending_pose(self):
        clips = {'idle': {'loop': True}, 'extra0': {'animation_length': .20,
                 'timeline': {'0': 'v.start=ctrl.playing_extra_animation();'}},
                 'parallel0': {'loop': True, 'animation_length': 2, 'bones': {'marker': {
                      'position': ['ctrl.playing_extra_animation?12:2', 0, 0]}}}}
        report = self.player({'@player_update': 'v.before_cap=ctrl.playing_extra_animation;'},
                             [{'time': t, 'extra': 'extra0'} for t in [0, .08, .22, .26, .40, .47]], clips)
        frames = report['frames']
        self.assertEqual([f['variables']['variable.start'] for f in frames], [1] * 6)
        self.assertEqual([f['pose']['marker']['position'][0] for f in frames], [12, 12, 12, 12, 2, 2])
        self.assertEqual([f['variables']['variable.before_cap'] for f in frames], [0, 1, 1, 1, 1, 0])

    def test_extra_animation_stop_and_replay_publish_the_new_request(self):
        clips = {'idle': {'loop': True}, 'extra0': {'animation_length': 2,
                 'timeline': {'0': 'v.starts+=ctrl.playing_extra_animation();'}},
                 'parallel0': {'loop': True, 'animation_length': 2, 'bones': {'marker': {
                      'position': ['ctrl.playing_extra_animation?12:2', 0, 0]}}}}
        report = self.player({}, [{'time': 0, 'extra': 'extra0'},
                                 {'time': .07}, {'time': .12, 'extra': 'extra0', 'extra_time': 0}], clips)
        frames = report['frames']
        self.assertEqual([f['pose']['marker']['position'][0] for f in frames], [12, 2, 12])
        self.assertEqual([f['variables']['variable.starts'] for f in frames], [1, 1, 2])

    def test_unlocked_movement_stops_only_the_requested_extra_once(self):
        clips={'idle': {'loop':True},'dance':{'loop':True,'animation_length':4},
               'parallel0':{'loop':True,'bones':{'marker':{'position':['ctrl.playing_extra_animation?12:2',0,0]}}}}
        for move in ({'forward':1},{'strafe':-.6},{'jump':True},{'sneak':True}):
            with self.subTest(move=move):
                events=[dict(time=0,play_extra='dance',movement_input={}),
                        dict(time=.05,movement_input=dict(move,locked=True)),
                        dict(time=.1,movement_input={'forward':.000001},entity={'velocity':[0,-.5,1]}),
                        dict(time=.15,movement_input=move),
                        dict(time=.2,movement_input=move),
                        dict(time=.25,play_extra='dance',movement_input={})]
                frames=self.player({},events,clips)['frames']
                self.assertEqual([f['handled']['extra_action']['serial'] for f in frames],[1,1,1,2,2,3])
                self.assertEqual([f['handled']['extra_action']['name'] for f in frames],['dance']*3+['','', 'dance'])
                self.assertEqual([f['pose']['marker']['position'][0] for f in frames],[12,12,12,2,2,12])

    def test_continue_override_selects_animation_only_after_successful_batch(self):
        frames = self.player({'@player_ctrl_main': "v.calls+=1;ctrl.set_beginning_transition_length(0);ctrl.set_animation('chosen');return ctrl.state_continue;"},
                             [{'time': 0}, {'time': 0.2}, {'time': 0.2}, {'time': 0.4}])['frames']
        self.assertEqual(self.coded(frames[0])['animation'], '')
        self.assertEqual(self.coded(frames[0])['selected_animation'], 'chosen')
        self.assertEqual(frames[1]['pose']['root']['position'][0], 7)
        self.assertEqual([f['variables']['variable.calls'] for f in frames], [1, 2, 2, 3])

    def test_continue_stop_pause_and_bypass_use_exact_float_results(self):
        frames = self.player({'@player_ctrl_main': 'return q.command;'},
            [{'time': t, 'queries': {'q.command': cmd}} for t, cmd in [(0, 5), (0.2, 5), (0.4, 4), (0.5, 2), (0.6, 3), (0.8, 3)]])['frames']
        self.assertTrue(self.coded(frames[2])['paused'])
        self.assertEqual(self.coded(frames[3])['generation'], 1)
        self.assertEqual(self.coded(frames[4])['phase'], 'ending')
        self.assertEqual(self.coded(frames[5])['phase'], 'idle')
        for code, phase, paused in [(2.00000001, 'idle', False), (4.00000001, 'idle', True), (2.0001, 'beginning', False)]:
            frame = self.player({'@player_ctrl_main': f'return {code};'}, [{'time': 0}])['frames'][0]
            self.assertEqual(self.coded(frame)['phase'], phase)
            self.assertEqual(self.coded(frame)['paused'], paused)

    def test_update_event_has_no_stale_animation_context(self):
        clips = {'idle': {'loop': True, 'animation_length': 10, 'bones': {'root': {'position': ['q.anim_time', 0, 0]}}}}
        frames = self.player({'@player_update': 'v.update_clock=q.anim_time;'},
                             [{'time': 0}, {'time': 0.5}, {'time': 0.7}], clips)['frames']
        self.assertEqual([f['variables']['variable.update_clock'] for f in frames], [0, 0, 0])

    def test_first_handler_wins_even_if_it_delegates(self):
        frames = self.player({'first@player_ctrl_main': 'v.first+=1;return ctrl.state_bypass;',
                              'second@player_ctrl_main': 'v.second+=1;return ctrl.state_stop;'}, [{'time': 0}])['frames']
        self.assertEqual(self.coded(frames[0])['animation'], 'idle')
        self.assertNotIn('variable.second', frames[0]['variables'])

    def test_bad_first_handler_does_not_activate_second_handler(self):
        report = self.player({'first@player_ctrl_main': 'not valid source',
                              'second@player_ctrl_main': 'v.second+=1;return ctrl.state_stop;'},
                             [{'time': 0}], compile_errors=True)
        self.assertEqual(self.coded(report['frames'][0])['animation'], 'idle')
        self.assertNotIn('variable.second', report['frames'][0]['variables'])

    def test_successful_bypass_still_commits_its_controller_effects(self):
        frame = self.player({'@player_ctrl_main': "ctrl.set_animation('chosen');return ctrl.state_bypass;"}, [{'time': 0}])['frames'][0]
        self.assertEqual(self.coded(frame)['animation'], 'idle')
        self.assertEqual(self.coded(frame)['selected_animation'], 'chosen')

    def test_failed_handler_delegates_and_discards_prepared_actions(self):
        frame = self.player({'@player_ctrl_main': "v.before+=1;ctrl.set_animation('chosen');return math.abs(3.5e38);"},
                            [{'time': 0}], failures=1)['frames'][0]
        self.assertEqual(self.coded(frame)['animation'], 'idle')
        self.assertEqual(self.coded(frame)['selected_animation'], 'idle')
        self.assertEqual(frame['variables']['variable.before'], 1)

    def test_later_failure_discards_earlier_controller_effects(self):
        clips = {'idle': {'loop': True}, 'chosen': {'loop': True},
                 'parallel0': {'loop': True, 'timeline': {'0': 'math.abs(3.5e38);'}}}
        frame = self.player({'@player_ctrl_main': "ctrl.set_animation('chosen');return 5;"},
                            [{'time': 0}], clips, failures=1)['frames'][0]
        self.assertEqual(self.coded(frame)['selected_animation'], 'idle')

    def test_earlier_failure_does_not_misclassify_later_successful_override(self):
        clips = {'idle': {'loop': True, 'timeline': {'0': 'v.native+=1;'}},
                 'pre_parallel0': {'loop': True, 'timeline': {'0': 'math.abs(3.5e38);'}}}
        frame = self.player({'@player_ctrl_main': 'return ctrl.state_stop;'}, [{'time': 0}], clips, failures=1)['frames'][0]
        self.assertEqual(self.coded(frame)['phase'], 'idle')
        self.assertNotIn('variable.native', frame['variables'])

    def test_actions_in_continuous_samples_are_rejected_before_arguments(self):
        clips = {'idle': {'loop': True, 'bones': {'root': {'position': ['fn.sample()', 0, 0]}}}}
        frame = self.player({'sample': 'ctrl.set_animation((v.argument+=1));return 7;'}, [{'time': 0.2}], clips)['frames'][0]
        self.assertNotIn('variable.argument', frame['variables'])
        self.assertEqual(self.coded(frame)['selected_animation'], 'idle')

    def test_custom_controller_does_not_run_the_coded_override(self):
        ctrl = {'player.main': {'states': {'default': {'animations': ['chosen']}}}}
        frame = self.player({'@player_ctrl_main': 'v.called+=1;return 3;'}, [{'time': 0}], controllers=ctrl)['frames'][0]
        self.assertEqual(frame['pose']['root']['position'][0], 7)
        self.assertNotIn('variable.called', frame['variables'])

    def test_event_only_controller_can_start_its_own_clip(self):
        clips = {'idle': {'loop': True}, 'chosen': {'loop': True, 'bones': {'custom': {'position': [7, 0, 0]}}}}
        frames = self.player({'@player_ctrl_pre_main': "ctrl.set_animation('chosen');return 2;"},
                             [{'time': 0}, {'time': 0.2}], clips)['frames']
        self.assertEqual(frames[-1]['pose']['custom']['position'][0], 7)
        self.assertEqual(self.coded(frames[-1], 'player.pre_main')['animation'], 'chosen')

    def test_event_only_parallel_controller_keeps_additive_rotation(self):
        clips = {'idle': {'loop': True, 'bones': {'root': {'rotation': [10, 0, 0]}}},
                 'chosen': {'loop': True, 'bones': {'root': {'rotation': [20, 0, 0]}}}}
        frames = self.player({'@player_ctrl_parallel_9': "ctrl.set_animation('chosen');return 2;"},
                             [{'time': 0}, {'time': 0.2}], clips)['frames']
        self.assertEqual(frames[-1]['pose']['root']['rotation'][0], 30)

    def test_reload_waits_for_next_selection_and_reset_clears_request(self):
        source = "q.action==1?{ctrl.indicate_reload();};q.action==2?{ctrl.set_animation('idle');};q.action==3?{ctrl.reset();};return q.action?2:5;"
        frames = self.player({'@player_ctrl_main': source},
            [{'time': t, 'queries': {'q.action': action}} for t, action in [(0, 0), (0.2, 1), (0.4, 0), (0.6, 2), (0.8, 2), (1, 3)]])['frames']
        self.assertEqual(self.coded(frames[1])['generation'], 1)
        self.assertTrue(self.coded(frames[1])['reload_on_selection'])
        self.assertEqual(self.coded(frames[2])['generation'], 2)
        self.assertEqual(self.coded(frames[-1])['selected_animation'], '')
        self.assertEqual(self.coded(frames[-1])['phase'], 'idle')

    def test_failed_previous_frame_does_not_block_successful_future_batch(self):
        source = "ctrl.set_beginning_transition_length(0);ctrl.set_animation('chosen');q.bad?{return math.abs(3.5e38);};return 2;"
        frames = self.player({'@player_ctrl_main': source},
            [{'time': 0, 'queries': {'q.bad': 1}}, {'time': 0.2, 'queries': {'q.bad': 0}}, {'time': 0.4}], failures=1)['frames']
        self.assertEqual(self.coded(frames[0])['selected_animation'], 'idle')
        self.assertEqual(self.coded(frames[1])['selected_animation'], 'chosen')
        self.assertEqual(frames[2]['pose']['root']['position'][0], 7)

    def test_authored_loop_override_can_hold_a_nonlooping_clip(self):
        source = "ctrl.set_beginning_transition_length(0);ctrl.set_animation('chosen',12);return 2;"
        clips = {'chosen': {'animation_length': 0.1, 'bones': {'root': {'position': ['q.anim_time', 0, 0]}}}}
        frames = self.player({'@player_ctrl_main': source}, [{'time': t} for t in [0, 0.2, 0.5, 1]], clips)['frames']
        self.assertEqual(self.coded(frames[-1])['phase'], 'running')
        self.assertEqual(self.coded(frames[-1])['cycle'], 0)
        self.assertAlmostEqual(frames[-1]['pose']['root']['position'][0], 0.1)

    def test_instruction_actions_capture_their_own_controller(self):
        clips = {'idle': {'loop': True, 'timeline': {'0': "ctrl.set_animation('chosen');"}},
                 'chosen': {'loop': True, 'bones': {'root': {'position': [7, 0, 0]}}}}
        frames = self.player({'@player_ctrl_main': 'return q.custom?2:5;'},
            [{'time': 0}, {'time': 0.2}, {'time': 0.4, 'queries': {'q.custom': 1}}], clips)['frames']
        self.assertEqual(self.coded(frames[1])['selected_animation'], 'chosen')
        self.assertEqual(self.coded(frames[2])['animation'], 'chosen')

    def test_string_code_is_projected_as_object_presence(self):
        frame = self.player({'@player_ctrl_main': "return '3';"}, [{'time': 0}])['frames'][0]
        self.assertEqual(self.coded(frame)['animation'], 'idle')

    def test_effect_batch_is_bounded_and_rejects_partial_commit(self):
        source = "loop(1024,{ctrl.set_animation('chosen');ctrl.set_animation('chosen');ctrl.set_animation('chosen');ctrl.set_animation('chosen');ctrl.set_animation('chosen');});return 2;"
        report = self.player({'@player_ctrl_main': source}, [{'time': 0}], failures=1)
        self.assertEqual(self.coded(report['frames'][0])['selected_animation'], 'idle')
        self.assertTrue(any('effect budget' in item for item in report['evaluation_diagnostics']))

    def test_function_files_in_zip_and_folder_use_root_then_subdirectory_order(self):
        with zipfile.ZipFile(SAMPLE) as source:
            files = {n: source.read(n) for n in source.namelist() if not n.endswith('/')}
        root = next(n[:-8] for n in files if n.endswith('ysm.json'))
        files[root+'functions/z@player_ctrl_main.MOLANG'] = b'v.root+=1;return 3;'
        files[root+'functions/a/a@player_ctrl_main.molang'] = b'v.child+=1;return 2;'
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)/'functions.zip'
            with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as archive:
                for name in sorted(files, reverse=True):
                    archive.writestr(name, files[name])
            with zipfile.ZipFile(path) as archive:
                archive.extractall(path.parent/'folder')
            reports = []
            for candidate in [path, path.parent/'folder']:
                run = subprocess.run([str(EXE), '--player-animations', str(candidate), json.dumps({'frames': [{'time': 0}]})],
                                     capture_output=True, encoding='utf-8', timeout=8)
                self.assertEqual(run.returncode, 0, run.stderr)
                report = json.loads(run.stdout)
                self.assertEqual(report['expression_failures'], 0, report)
                variables = report['frames'][0]['variables']
                self.assertEqual(variables['variable.root'], 1)
                self.assertNotIn('variable.child', variables)
                reports.append(report)
            self.assertEqual(reports[0], reports[1])

    def test_declared_function_directory_and_package_root_directory(self):
        for function_path, relative in [('custom/scripts', 'custom/scripts/a.molang'), ('.', 'a.molang')]:
            with self.subTest(function_path=function_path), zipfile.ZipFile(SAMPLE) as source:
                files = {n: source.read(n) for n in source.namelist() if not n.endswith('/')}
                descriptor = next(n for n in files if n.endswith('ysm.json'))
                root = descriptor[:-8]
                manifest = json.loads(files[descriptor])
                manifest['files']['function_path'] = function_path
                files[descriptor] = json.dumps(manifest).encode('utf-8')
                files[root+relative] = b'return 7;'
                with tempfile.TemporaryDirectory() as directory:
                    path = pathlib.Path(directory)/'functions.zip'
                    with zipfile.ZipFile(path, 'w') as archive:
                        for name, data in files.items():
                            archive.writestr(name, data)
                    run = subprocess.run([str(EXE), '--animation-sets', str(path)], capture_output=True,
                                         encoding='utf-8', timeout=5)
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertEqual(json.loads(run.stdout)['body']['ysm_functions']['a'], 'return 7;')


if __name__ == '__main__':
    unittest.main()
