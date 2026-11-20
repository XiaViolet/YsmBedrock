"""Observable deferred-handler ordering, animation ownership and batch failure."""
import json
import pathlib
import struct
import subprocess
import tempfile
import unittest

import test_model_functions as shared
from test_import import EXE


class DeferredEventTests(unittest.TestCase):
    player = shared.ModelFunctionTests.player
    coded = staticmethod(shared.ModelFunctionTests.coded)

    def last(self, functions, frames=None, **options):
        return self.player(functions, frames or [{'time': 0}], **options)['frames'][-1]

    def test_captures_are_dispatched_in_reverse_order(self):
        frame = self.last({'@player_init': 'ysm.defer(0,1);ysm.defer(0,2);',
                           '@defer': 'v.order=v.order*10+args[1];'})
        self.assertEqual(frame['variables']['variable.order'], 21)

    def test_parameters_are_copied_before_later_variable_writes(self):
        frame = self.last({'@player_init': 'v.n=1;ysm.defer(0,v.n);v.n=2;ysm.defer(0,v.n);v.n=9;',
                           '@defer': 'v.order=v.order*10+args[1];'})
        self.assertEqual(frame['variables']['variable.order'], 21)
        self.assertEqual(frame['variables']['variable.n'], 9)

    def test_first_float_remains_args_zero_without_cross_frame_timer(self):
        frame = self.last({'@player_init': 'ysm.defer(-0.5,9);',
                           '@defer': 'v.first=args[0];v.payload=args[1];v.absent=args[2]??7;'})
        self.assertEqual([frame['variables']['variable.' + key] for key in ['first', 'payload', 'absent']], [-0.5, 9, 7])

    def test_nested_capture_precedes_older_pending_capture(self):
        frame = self.last({'@player_init': 'ysm.defer(0,1);ysm.defer(0,2);',
                           '@defer': 'v.order=v.order*10+args[1];args[1]==2?ysm.defer(0,3);'})
        self.assertEqual(frame['variables']['variable.order'], 231)

    def test_handlers_preserve_source_order_for_each_capture(self):
        frame = self.last({'@player_init': 'ysm.defer(0,1);ysm.defer(0,2);',
                           'a@defer': 'v.order=v.order*10+args[1];', 'b@defer': 'v.order=v.order*10+9;'})
        self.assertEqual(frame['variables']['variable.order'], 2919)

    def test_child_capture_waits_until_all_handlers_finish_current_capture(self):
        frame = self.last({'@player_init': 'ysm.defer(0,1);ysm.defer(0,2);',
                           'a@defer': 'v.order=v.order*10+args[1];args[1]==2?ysm.defer(0,3);',
                           'b@defer': 'v.order=v.order*10+9;'})
        self.assertEqual(frame['variables']['variable.order'], 293919)

    def test_handler_has_fresh_temporaries_and_its_own_arguments(self):
        frame = self.last({'@player_init': 't.secret=6;ysm.defer(0,8);',
                           '@defer': 'v.local=t.secret??7;v.payload=args[1];'})
        self.assertEqual(frame['variables']['variable.local'], 7)
        self.assertEqual(frame['variables']['variable.payload'], 8)

    def test_temporaries_are_separate_between_handlers(self):
        frame = self.last({'@player_init': 'ysm.defer(0);', 'a@defer': 't.secret=6;',
                           'b@defer': 'v.local=t.secret??7;'})
        self.assertEqual(frame['variables']['variable.local'], 7)

    def test_payload_uses_finite_float32_values(self):
        frame = self.last({'@player_init': 'ysm.defer(0,16777217,0.1);',
                           '@defer': 'v.integer=args[1];v.fraction=args[2];'})
        self.assertEqual(frame['variables']['variable.integer'], 16777216)
        self.assertEqual(frame['variables']['variable.fraction'], struct.unpack('<f', struct.pack('<f', 0.1))[0])

    def test_at_least_one_argument_is_required(self):
        frame = self.last({'@player_init': 'ysm.defer();v.after=1;', '@defer': 'v.called=1;'})
        self.assertEqual(frame['variables']['variable.after'], 1)
        self.assertNotIn('variable.called', frame['variables'])

    def test_255_arguments_are_retained(self):
        frame = self.last({'@player_init': 'ysm.defer(' + ','.join(map(str, range(255))) + ');',
                           '@defer': 'v.last=args[254];v.absent=args[255]??17;'})
        self.assertEqual(frame['variables']['variable.last'], 254)
        self.assertEqual(frame['variables']['variable.absent'], 17)

    def test_256_arguments_are_rejected_before_dispatch(self):
        frame = self.last({'@player_init': 'ysm.defer(' + ','.join(map(str, range(256))) + ');',
                           '@defer': 'v.called=1;'})
        self.assertNotIn('variable.called', frame['variables'])

    def test_string_payload_uses_source_scalar_projection(self):
        frame = self.last({'@player_init': "ysm.defer(0,'text');", '@defer': 'v.called=args[1];'})
        self.assertEqual(frame['variables']['variable.called'], 1)

    def test_array_payload_uses_length_without_consuming_elements(self):
        frame = self.last({'@player_init': 'fn.emit(9);', 'emit': 'ysm.defer(0,args);',
                           '@defer': 'v.called=args[1];'})
        self.assertEqual(frame['variables']['variable.called'], 1)

    def test_float32_overflow_is_rejected_before_dispatch(self):
        frame = self.last({'@player_init': 'ysm.defer(0,1e40);', '@defer': 'v.called=1;'}, failures=1)
        self.assertNotIn('variable.called', frame['variables'])

    def test_readonly_bone_expression_does_not_evaluate_action_arguments(self):
        clip = {'loop': True, 'bones': {'root': {'position': ['ysm.defer(0,v.changed=1)??2', 0, 0]}}}
        frame = self.last({'@defer': 'v.called=1;'}, [{'time': 0}, {'time': 0.2}], clips={'idle': clip})
        self.assertNotIn('variable.changed', frame['variables'])
        self.assertNotIn('variable.called', frame['variables'])
        self.assertEqual(frame['pose']['root']['position'][0], 2)

    def test_readonly_function_cannot_enqueue_deferred_work(self):
        clip = {'loop': True, 'bones': {'root': {'position': ['fn.emit()', 0, 0]}}}
        frame = self.last({'emit': 'ysm.defer(0,1e40);return 2;', '@defer': 'v.called=1;'},
                          [{'time': 0}, {'time': 0.2}], clips={'idle': clip})
        self.assertNotIn('variable.called', frame['variables'])
        self.assertEqual(frame['pose']['root']['position'][0], 2)

    def test_deferred_controller_action_keeps_original_controller_binding(self):
        functions = {'@player_ctrl_pre_main_one': 'ysm.defer(0,1);return 2;',
                     '@player_ctrl_post_main_two': 'ysm.defer(0,2);return 2;',
                     '@defer': "args[1]==1?ctrl.set_animation('chosen'):ctrl.set_animation('idle');"}
        frame = self.last(functions)
        coded = frame['controllers']['coded']
        self.assertEqual(coded['player.pre_main_one']['selected_animation'], 'chosen')
        self.assertEqual(coded['player.post_main_two']['selected_animation'], 'idle')
        self.assertEqual(coded['player.main']['selected_animation'], 'idle')

    def test_deferred_controller_change_applies_after_successful_round(self):
        frames = self.player({'@player_ctrl_main': 'ysm.defer(0);return 2;',
                              '@defer': "ctrl.set_animation('chosen');ctrl.set_beginning_transition_length(0);"},
                             [{'time': 0}, {'time': 0.2}])['frames']
        self.assertEqual(self.coded(frames[0])['animation'], '')
        self.assertEqual(self.coded(frames[0])['selected_animation'], 'chosen')
        self.assertEqual(frames[1]['pose']['root']['position'][0], 7)

    def test_global_init_capture_does_not_borrow_the_first_controller(self):
        frame = self.last({'@player_init': 'ysm.defer(0);', '@defer': "ctrl.set_animation('chosen');"})
        self.assertEqual(self.coded(frame)['selected_animation'], 'idle')
        self.assertEqual(frame['controllers']['coded']['player.vehicle']['selected_animation'], '')

    def test_controller_capture_observes_final_clock_but_copies_call_arguments(self):
        frame = self.last({'@player_ctrl_main': 'ysm.defer(0,q.anim_time);return 5;',
                           '@defer': 'v.seen=q.anim_time;v.captured=args[1];'},
                          [{'time': 0}, {'time': 0.5}], clips={'idle': {'loop': True, 'animation_length': 10}})
        self.assertAlmostEqual(frame['variables']['variable.seen'], 0.4)
        self.assertEqual(frame['variables']['variable.captured'], 0)

    @staticmethod
    def staged_clips():
        return {'idle': {'loop': True, 'animation_length': 1, 'timeline': {'0': 'ysm.defer(0,3);v.mark=1;'},
                         'bones': {'root': {'position': ['v.mark', 0, 0]}}},
                'parallel0': {'loop': True, 'bones': {'root': {'rotation': ['v.mark', 0, 0]}}}}

    def test_timeline_defer_runs_after_its_bones_and_before_next_player(self):
        frame = self.last({'@defer': 'v.mark=args[1];'}, [{'time': 0}, {'time': 0.2}], clips=self.staged_clips())
        self.assertEqual(frame['pose']['root']['position'][0], 1)
        self.assertEqual(frame['pose']['root']['rotation'][0], 3)

    def test_standalone_sampler_preserves_the_same_animation_boundary(self):
        document = {'animations': self.staged_clips(), 'ysm_functions': {'@defer': 'v.mark=args[1];'}}
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'animations.json'
            path.write_text(json.dumps(document), encoding='utf-8')
            run = subprocess.run([str(EXE), '--animation-file', str(path), 'idle', '0', '{}'],
                                 capture_output=True, encoding='utf-8', timeout=5)
        self.assertEqual(run.returncode, 0, run.stderr)
        report = json.loads(run.stdout)
        self.assertEqual(report['expression_failures'], 0, report)
        self.assertEqual(report['pose']['root']['position'][0], 1)
        self.assertEqual(report['pose']['root']['rotation'][0], 3)

    def test_bedrock_players_drain_defer_before_the_next_animation(self):
        clips = {'a': self.staged_clips()['idle'], 'b': self.staged_clips()['parallel0']}
        controller = {'states': {'default': {'animations': ['a', 'b']}}}
        frame = self.last({'@defer': 'v.mark=args[1];'}, clips=clips, controllers={'player.main': controller})
        self.assertEqual(frame['pose']['root']['position'][0], 1)
        self.assertEqual(frame['pose']['root']['rotation'][0], 3)

    def test_controller_entry_capture_drains_before_first_animation(self):
        controller = {'states': {'default': {'on_entry': ['ysm.defer(0,9);'], 'animations': ['idle']}}}
        clip = {'loop': True, 'bones': {'root': {'position': ['v.mark', 0, 0]}}}
        frame = self.last({'@defer': 'v.mark=args[1];'}, clips={'idle': clip}, controllers={'player.main': controller})
        self.assertEqual(frame['pose']['root']['position'][0], 9)

    def test_later_script_failure_discards_actions_from_completed_defer(self):
        clips = {'idle': {'loop': True}, 'chosen': {'loop': True},
                 'parallel0': {'loop': True, 'timeline': {'0': 'math.abs(3.5e38);'}}}
        frame = self.last({'@player_ctrl_main': 'ysm.defer(0);return 5;',
                           '@defer': "v.called+=1;ctrl.set_animation('chosen');"}, clips=clips, failures=1)
        self.assertEqual(frame['variables']['variable.called'], 1)
        self.assertEqual(self.coded(frame)['selected_animation'], 'idle')

    def test_deferred_failure_discards_earlier_prepared_action(self):
        frame = self.last({'@player_ctrl_main': "ctrl.set_animation('chosen');ysm.defer(0);return 5;",
                           '@defer': 'math.abs(3.5e38);'}, failures=1)
        self.assertEqual(self.coded(frame)['selected_animation'], 'idle')

    def test_failed_callback_retires_pending_work_and_next_frame_recovers(self):
        frames = self.player({'@player_init': 'ysm.defer(0,1);ysm.defer(0,2);',
                              '@player_update': 'q.next?ysm.defer(0,3);',
                              '@defer': 'v.last=args[1];args[1]==2?math.abs(3.5e38);'},
                             [{'time': 0}, {'time': 0.2, 'queries': {'q.next': 1}}], failures=1)['frames']
        self.assertEqual([f['variables']['variable.last'] for f in frames], [2, 3])

    def test_capture_limit_is_cumulative_even_after_earlier_context_drained(self):
        frame = self.last({'@player_update': 'loop(40,{ysm.defer(0);});',
                           '@player_ctrl_main': 'loop(25,{ysm.defer(0);});return 5;',
                           '@defer': 'v.called+=1;'}, failures=1)
        self.assertEqual(frame['variables']['variable.called'], 40)

    def test_recursive_defer_is_bounded_without_leaking_into_next_frame(self):
        report = self.player({'@player_init': 'ysm.defer(0);', '@defer': 'v.called+=1;ysm.defer(0);'},
                             [{'time': 0}, {'time': 0.2}], failures=1)
        self.assertEqual([f['variables']['variable.called'] for f in report['frames']], [64, 64])
        self.assertTrue(any('deferred capture budget' in item for item in report['evaluation_diagnostics']))

    def test_each_handler_receives_a_fresh_execution_budget(self):
        frame = self.last({'@player_init': 'loop(64,{ysm.defer(0);});',
                           '@defer': 'loop(1024,{v.called+=1;});'})
        self.assertEqual(frame['variables']['variable.called'], 64 * 1024)

    def test_repeated_observation_does_not_dispatch_defer_twice(self):
        frames = self.player({'@player_update': 'ysm.defer(0);', '@defer': 'v.called+=1;'},
                             [{'time': 0}, {'time': 0}, {'time': 0.2}])['frames']
        self.assertEqual([f['variables']['variable.called'] for f in frames], [1, 1, 2])

    def test_shared_clip_captures_are_independent_between_players(self):
        clips = {'idle': {'loop': True}, 'shared': {'loop': True, 'timeline': {'0': 'ysm.defer(0);'}}}
        controller = {'states': {'default': {'animations': ['shared']}}}
        frame = self.last({'@defer': 'v.called+=1;'}, clips=clips,
                          controllers={'player.pre_main_a': controller, 'player.post_main_b': controller})
        self.assertEqual(frame['variables']['variable.called'], 2)

    def test_capture_without_handler_is_retired_normally(self):
        frame = self.last({'@player_init': 'ysm.defer(0);v.after=1;'})
        self.assertEqual(frame['variables']['variable.after'], 1)


if __name__ == '__main__':
    unittest.main()
