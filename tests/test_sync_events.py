"""Local postcommit delivery, independent rounds and bounded receive ingress."""
import json
import pathlib
import struct
import subprocess
import tempfile
import unittest

from test_import import EXE
import test_model_functions as shared


class SyncEventTests(unittest.TestCase):
    player = shared.ModelFunctionTests.player
    coded = staticmethod(shared.ModelFunctionTests.coded)

    def last(self, functions, frames=None, **options):
        return self.player(functions, frames or [{'time': 0}], local_sync=True, **options)['frames'][-1]

    def test_zero_arguments_remain_an_empty_argument_array(self):
        frame = self.last({'@player_init': 'ysm.sync();', '@sync': 'v.absent=args[0]??7;'})
        self.assertEqual(frame['variables']['variable.absent'], 7)

    def test_all_sixteen_arguments_are_retained(self):
        frame = self.last({'@player_init': 'ysm.sync(' + ','.join(map(str, range(16))) + ');',
                           '@sync': 'v.first=args[0];v.last=args[15];v.absent=args[16]??9;'})
        self.assertEqual([frame['variables']['variable.' + name] for name in ['first', 'last', 'absent']], [0, 15, 9])

    def test_seventeen_arguments_are_rejected(self):
        frame = self.last({'@player_init': 'ysm.sync(' + ','.join(map(str, range(17))) + ');',
                           '@sync': 'v.called=1;'})
        self.assertNotIn('variable.called', frame['variables'])

    def test_payload_is_copied_and_rounded_to_finite_float32(self):
        frame = self.last({'@player_init': 'v.value=16777217;ysm.sync(v.value,0.1);v.value=9;',
                           '@sync': 'v.integer=args[0];v.fraction=args[1];'})
        self.assertEqual(frame['variables']['variable.integer'], 16777216)
        self.assertEqual(frame['variables']['variable.fraction'], struct.unpack('<f', struct.pack('<f', 0.1))[0])
        self.assertEqual(frame['variables']['variable.value'], 9)

    def test_string_argument_uses_source_scalar_projection(self):
        frame = self.last({'@player_init': "ysm.sync('text');", '@sync': 'v.called=args[0];'})
        self.assertEqual(frame['variables']['variable.called'], 1)

    def test_array_argument_uses_length_without_consuming_elements(self):
        frame = self.last({'@player_init': 'fn.emit(3);', 'emit': 'ysm.sync(args);',
                           '@sync': 'v.called=args[0];'})
        self.assertEqual(frame['variables']['variable.called'], 1)

    def test_float32_overflow_is_rejected(self):
        frame = self.last({'@player_init': 'ysm.sync(1e40);', '@sync': 'v.called=1;'}, failures=1)
        self.assertNotIn('variable.called', frame['variables'])

    def test_delivery_is_disabled_for_a_player_without_a_relay_or_preview(self):
        frame = self.player({'@player_init': 'ysm.sync(3);v.after=7;', '@sync': 'v.called=1;'},
                            [{'time': 0}])['frames'][0]
        self.assertNotIn('variable.called', frame['variables'])
        self.assertEqual(frame['variables']['variable.after'], 7)

    def test_disabled_route_still_validates_payloads(self):
        frame = self.player({'@player_init': 'ysm.sync(1e40);', '@sync': 'v.called=1;'},
                            [{'time': 0}], failures=1)['frames'][0]
        self.assertNotIn('variable.called', frame['variables'])

    def test_preview_route_can_be_changed_without_restarting_the_owner(self):
        frames = self.player({'@player_update': 'ysm.sync();', '@sync': 'v.called+=1;'},
                             [{'time': 0}, {'time': 0.2, 'local_sync': True}, {'time': 0.4},
                              {'time': 0.6, 'local_sync': True}])['frames']
        self.assertEqual([f['variables'].get('variable.called', 0) for f in frames], [0, 1, 1, 2])

    def test_readonly_bone_denies_the_action_before_argument_side_effects(self):
        clip = {'loop': True, 'bones': {'root': {'position': ['ysm.sync(v.argument=1)??2', 0, 0]}}}
        frame = self.last({'@sync': 'v.called=1;'}, [{'time': 0}, {'time': 0.2}], clips={'idle': clip})
        self.assertEqual(frame['pose']['root']['position'][0], 2)
        self.assertNotIn('variable.argument', frame['variables'])
        self.assertNotIn('variable.called', frame['variables'])

    def test_readonly_callee_cannot_emit_or_validate_action_payload(self):
        clip = {'loop': True, 'bones': {'root': {'position': ['fn.emit()', 0, 0]}}}
        frame = self.last({'emit': 'ysm.sync(1e40);return 4;', '@sync': 'v.called=1;'},
                          [{'time': 0}, {'time': 0.2}], clips={'idle': clip})
        self.assertEqual(frame['pose']['root']['position'][0], 4)
        self.assertNotIn('variable.called', frame['variables'])

    def test_local_effects_follow_registration_order(self):
        frame = self.last({'@player_init': 'ysm.sync(1);ysm.sync(2);',
                           '@sync': 'v.order=v.order*10+args[0];'})
        self.assertEqual(frame['variables']['variable.order'], 12)

    def test_nested_delivery_finishes_before_the_next_source_effect(self):
        frame = self.last({'@player_init': 'ysm.sync(1);ysm.sync(3);',
                           '@sync': 'v.order=v.order*10+args[0];args[0]==1?ysm.sync(2);'})
        self.assertEqual(frame['variables']['variable.order'], 123)

    def test_all_handlers_finish_before_nested_delivery_starts(self):
        frame = self.last({'@player_init': 'ysm.sync(1);ysm.sync(3);',
                           'a@sync': 'v.order=v.order*10+args[0];args[0]==1?ysm.sync(2);',
                           'b@sync': 'v.order=v.order*10+9;'})
        self.assertEqual(frame['variables']['variable.order'], 192939)

    def test_handler_locals_and_arguments_are_fresh_for_each_entry(self):
        frame = self.last({'@player_init': 't.secret=3;ysm.sync(8);',
                           'a@sync': 'v.local=t.secret??7;t.secret=4;v.payload=args[0];',
                           'b@sync': 'v.second=t.secret??9;v.second_arg=args[0];'})
        self.assertEqual([frame['variables']['variable.' + name] for name in ['local', 'payload', 'second', 'second_arg']], [7, 8, 9, 8])

    def test_sync_runs_after_the_entire_pose_and_not_inside_its_source_animation(self):
        clips = {'idle': {'loop': True, 'animation_length': 10, 'timeline': {'0.3': 'ysm.sync();v.mark=1;'},
                          'bones': {'root': {'position': ['v.mark', 0, 0]}}},
                 'parallel0': {'loop': True, 'animation_length': 10, 'timeline': {'0.3': 'v.mark=9;'}}}
        # Cross the normal beginning transition before the observable timeline.
        frames = self.player({'@sync': 'v.seen=v.mark;v.mark=7;'},
                             [{'time': 0}, {'time': 0.2}, {'time': 0.6}, {'time': 0.8}],
                             clips=clips, local_sync=True)['frames']
        self.assertEqual(frames[-2]['pose']['root']['position'][0], 1)
        self.assertEqual(frames[-2]['variables']['variable.seen'], 9)
        self.assertEqual(frames[-2]['variables']['variable.mark'], 7)
        self.assertEqual(frames[-1]['pose']['root']['position'][0], 7)

    def test_sync_does_not_borrow_a_controller_or_its_animation_clock(self):
        frames = self.player({'@player_ctrl_main': 'ysm.sync();return 5;',
                              '@sync': "v.clock=q.anim_time;v.state=q.state_time;v.done=q.all_animations_finished;ctrl.set_animation('chosen');"},
                             [{'time': 0}, {'time': 0.5}], local_sync=True)['frames']
        self.assertEqual(self.coded(frames[-1])['selected_animation'], 'idle')
        self.assertEqual([frames[-1]['variables']['variable.' + key] for key in ['clock', 'state', 'done']], [0, 0, 0])

    def test_defer_can_prepare_a_sync_for_postcommit_delivery(self):
        frame = self.last({'@player_init': 'ysm.defer(0,8);', '@defer': 'v.prepared=1;ysm.sync(args[1]);',
                           '@sync': 'v.seen=v.prepared;v.payload=args[0];'})
        self.assertEqual(frame['variables']['variable.seen'], 1)
        self.assertEqual(frame['variables']['variable.payload'], 8)

    def test_sync_defer_and_nested_sync_keep_separate_argument_snapshots(self):
        frame = self.last({'@player_init': 'ysm.sync(1);',
                           '@sync': 'v.order=v.order*10+args[0];args[0]==1?{ysm.defer(0,2);ysm.sync(3);};',
                           '@defer': 'v.order=v.order*10+args[1];'})
        self.assertEqual(frame['variables']['variable.order'], 123)

    def test_later_source_failure_discards_prepared_sync(self):
        clips = {'idle': {'loop': True}, 'parallel0': {'loop': True, 'timeline': {'0': 'math.abs(3.5e38);'}}}
        frame = self.last({'@player_init': 'ysm.sync();', '@sync': 'v.called=1;'}, clips=clips, failures=1)
        self.assertNotIn('variable.called', frame['variables'])

    def test_later_deferred_failure_discards_prepared_sync(self):
        frame = self.last({'@player_init': 'ysm.sync();ysm.defer(0);', '@defer': 'math.abs(3.5e38);',
                           '@sync': 'v.called=1;'}, failures=1)
        self.assertNotIn('variable.called', frame['variables'])

    def test_failed_sync_discards_its_own_nested_effects_but_source_commit_continues(self):
        frame = self.last({'@player_ctrl_main': "ysm.sync(1);ctrl.set_animation('chosen');return 5;",
                           '@sync': 'v.order=v.order*10+args[0];ysm.sync(2);math.abs(3.5e38);'}, failures=1)
        self.assertEqual(frame['variables']['variable.order'], 1)
        self.assertEqual(self.coded(frame)['selected_animation'], 'chosen')

    def test_a_failed_sync_does_not_poison_a_later_independent_source_delivery(self):
        frame = self.last({'@player_init': 'ysm.sync(1);ysm.sync(2);',
                           '@sync': 'v.order=v.order*10+args[0];args[0]==1?math.abs(3.5e38);'}, failures=1)
        self.assertEqual(frame['variables']['variable.order'], 12)

    def test_repeated_observation_does_not_emit_update_sync_again(self):
        frames = self.player({'@player_update': 'ysm.sync();', '@sync': 'v.called+=1;'},
                             [{'time': 0}, {'time': 0}, {'time': 0.2}], local_sync=True)['frames']
        self.assertEqual([f['variables']['variable.called'] for f in frames], [1, 1, 2])

    def test_each_handler_receives_a_fresh_execution_budget(self):
        functions = {'@player_init': 'ysm.sync();'}
        functions.update({'handler%02d@sync' % i: 'loop(1024,{v.called+=1;});' for i in range(64)})
        frame = self.last(functions)
        self.assertEqual(frame['variables']['variable.called'], 64 * 1024)

    def test_recursive_local_sync_is_bounded_without_cross_frame_pending_work(self):
        report = self.player({'@player_init': 'ysm.sync();', '@sync': 'v.called+=1;ysm.sync();'},
                             [{'time': 0}, {'time': 0.2}], failures=1, local_sync=True)
        self.assertEqual([f['variables']['variable.called'] for f in report['frames']], [64, 64])
        self.assertTrue(any('sync nesting budget' in item for item in report['evaluation_diagnostics']))

    def test_branching_sync_has_a_total_delivery_bound_and_later_frames_recover(self):
        report = self.player({'@player_init': 'ysm.sync(0);', '@player_update': 'q.next?ysm.sync(42);',
                              '@sync': 'v.called+=1;args[0]<20?{ysm.sync(args[0]+1);ysm.sync(args[0]+1);};'},
                             [{'time': 0}, {'time': 0.2, 'queries': {'q.next': 1}}], failures=1, local_sync=True)
        self.assertEqual([f['variables']['variable.called'] for f in report['frames']], [256, 257])
        self.assertTrue(any('sync delivery budget' in item for item in report['evaluation_diagnostics']))

    def test_received_event_runs_even_when_local_emission_is_disabled(self):
        frame = self.player({'@sync': 'v.payload=args[0];ysm.sync(99);'},
                            [{'time': 0, 'sync_events': [[3]]}])['frames'][0]
        self.assertEqual(frame['variables']['variable.payload'], 3)

    def test_received_events_use_source_order_and_their_own_float_snapshots(self):
        frame = self.player({'a@sync': 'v.order=v.order*10+args[0];', 'b@sync': 'v.last=args[1]??7;'},
                            [{'time': 0, 'sync_events': [[1, 0.1], [2]]}])['frames'][0]
        self.assertEqual(frame['variables']['variable.order'], 12)
        self.assertEqual(frame['variables']['variable.last'], 7)

    def test_receive_normalizes_numbers_to_finite_float32(self):
        frame = self.player({'@sync': 'v.integer=args[0];v.fraction=args[1];'},
                            [{'time': 0, 'sync_events': [[16777217, 0.1]]}])['frames'][0]
        self.assertEqual(frame['variables']['variable.integer'], 16777216)
        self.assertEqual(frame['variables']['variable.fraction'], struct.unpack('<f', struct.pack('<f', 0.1))[0])

    def test_receive_rejects_invalid_payload_before_any_handler_runs(self):
        document = {'animations': {'idle': {'loop': True}}, 'ysm_functions': {'@sync': 'v.called=1;'}}
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'sync.json'
            path.write_text(json.dumps(document), encoding='utf-8')
            for payload, expected in [(list(range(17)), 'argument count'), ([1e40], 'finite floats'), (['text'], 'finite numbers')]:
                with self.subTest(payload=payload):
                    inputs = {'frames': [{'time': 0, 'sync_events': [payload]}]}
                    result = subprocess.run([str(EXE), '--player-animation-file', str(path), json.dumps(inputs)],
                                            capture_output=True, encoding='utf-8', timeout=5)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(expected, result.stderr)

    def test_handler_without_a_sync_event_does_not_run_spontaneously(self):
        frame = self.last({'@sync': 'v.called=1;'})
        self.assertNotIn('variable.called', frame['variables'])


if __name__ == '__main__':
    unittest.main()
