import json
import pathlib
import subprocess
import tempfile
import unittest

from test_import import EXE


class CodedPlaybackTests(unittest.TestCase):
    def test_omitted_length_with_zero_time_channels_keeps_clock_and_start_event(self):
        for loop in (False, True, 'hold_on_last_frame'):
            with self.subTest(loop=loop):
                clip = {'loop': loop, 'timeline': {'0': 'v.start+=1;', '0.875': 'v.tail+=1;'},
                        'bones': {'root': {'position': ['q.anim_time * 10', 0, 0]}}}
                times = [0, .05, .25, .5, .875, 1.05]
                frames = self.run_frames({'a': clip}, [{'time': t, 'animation': 'a'} for t in times])
                for frame, t in zip(frames, times):
                    self.assertEqual(frame['player']['phase'], 'running')
                    self.assertAlmostEqual(frame['pose']['root']['position'][0], t * 10, places=5)
                    self.assertEqual(frame['variables']['variable.start'], 1)
                self.assertEqual(frames[-1]['variables']['variable.tail'], 1)

    def test_omitted_length_without_bones_does_not_use_event_times(self):
        clip = {'loop': True, 'timeline': {'0': 'v.start+=1;', '0.5': 'v.tail+=1;'}}
        frames = self.run_frames({'a': clip}, [{'time': t, 'animation': 'a'} for t in [0, .5, .75, 1.25]])
        self.assertEqual([f['variables']['variable.start'] for f in frames], [1, 1, 1, 1])
        self.assertEqual(frames[-1]['variables']['variable.tail'], 1)

    def test_inferred_length_plays_like_explicit_length_of_last_bone_key(self):
        clip = {'timeline': {'0': 'v.start+=1;'},
                'bones': {'root': {'position': {'0': [0, 0, 0], '0.5': [5, 0, 0]}}}}
        for loop in (False, True, 'hold_on_last_frame'):
            with self.subTest(loop=loop):
                frames = [{'time': t, 'animation': 'a'} for t in [0, .25, .5, .625, .75, 1]]
                inferred = self.run_frames({'a': dict(clip, loop=loop)}, frames)
                explicit = self.run_frames({'a': dict(clip, loop=loop, animation_length=.5)}, frames)
                self.assertEqual(inferred, explicit)

    def run_frames(self, clips, frames, expected_diagnostics=None, merge_multiline=False, **options):
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder) / 'coded.json'
            path.write_text(json.dumps({'animations': clips, 'ysm_merge_multiline_expr': merge_multiline}), encoding='utf-8')
            result = subprocess.run([str(EXE), '--coded-animation-file', str(path),
                                     json.dumps(dict(options, frames=frames))],
                                    capture_output=True, encoding='utf-8', timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(result.stdout)
            self.assertEqual(report['expression_failures'], 0, report)
            self.assertEqual(report['compile_diagnostics'], [], report)
            if expected_diagnostics is None:
                self.assertEqual(report['evaluation_diagnostics'], [], report)
            else:
                for expected in expected_diagnostics:
                    self.assertTrue(any(expected in item for item in report['evaluation_diagnostics']), report)
            return report['frames']

    def test_once_uses_single_ending_fade_from_last_drawn_point_then_stays_idle(self):
        clip = {'animation_length': 1, 'bones': {'root': {'position': ['10*q.anim_time', 0, 0]}}}
        frames = self.run_frames({'a': clip}, [{'time': t, 'animation': 'a'} for t in [0, 0.7, 1.2, 1.275, 1.36, 2]],
                                 pose={'root': {'position': [2, 0, 0]}})
        for frame, expected in zip(frames, [0, 7, 7, 4.5, 2, 2]):
            self.assertAlmostEqual(frame['pose']['root']['position'][0], expected)
        self.assertEqual([f['player']['phase'] for f in frames], ['running', 'running', 'ending', 'ending', 'idle', 'idle'])
        self.assertEqual(frames[-1]['player']['generation'], 1)

    def test_beginning_transition_weights_position_and_rotation_and_reveals_scale_immediately(self):
        clip = {'loop': True, 'animation_length': 3, 'blend_weight': 0.5, 'timeline': {'0': 'v.started+=1;'},
                'bones': {'root': {'position': [10, 0, 0], 'rotation': [80, 0, 0], 'scale': 3}}}
        frames = self.run_frames({'a': clip}, [{'time': t, 'animation': 'a'} for t in [0, 0.5, 1, 1.1]],
                                 transition=1, pose={'root': {'position': [4, 0, 0], 'rotation': [20, 0, 0]}})
        self.assertEqual(frames[0]['pose']['root']['position'], [4, 0, 0])
        self.assertAlmostEqual(frames[0]['pose']['root']['rotation'][0], 20)
        self.assertEqual(frames[0]['pose']['root']['scale'], [2, 2, 2])
        self.assertAlmostEqual(frames[1]['pose']['root']['position'][0], 5.25)
        self.assertAlmostEqual(frames[1]['pose']['root']['rotation'][0], 30)
        self.assertNotIn('variable.started', frames[1]['variables'])
        self.assertEqual(frames[3]['variables']['variable.started'], 1)
        self.assertEqual(frames[3]['pose']['root']['position'][0], 5)

    def test_segmented_beginning_curve_uses_remaining_source_weight(self):
        clip = {'loop': True, 'bones': {'root': {'position': [12, 0, 0]}}}
        frames = self.run_frames({'a': clip}, [{'time': t, 'animation': 'a'} for t in [0, 0.5, 1]],
                                 transition={'0': 1, '0.5': 0.75, '1': 0}, pose={'root': {'position': [4, 0, 0]}})
        self.assertEqual([f['pose']['root']['position'][0] for f in frames], [4, 6, 12])

    def test_replacement_blends_from_previous_complete_pose_and_has_new_timeline(self):
        clips = {name: {'loop': True, 'timeline': {'0': 'v.started+=1;'}, 'bones': {'root': {'position': [value, 0, 0]}}}
                 for name, value in [('a', 10), ('b', 20)]}
        frames = self.run_frames(clips, [{'time': 0, 'animation': 'a'}, {'time': 1, 'animation': 'b', 'transition': 1},
                                         {'time': 1.5, 'animation': 'b'}, {'time': 2, 'animation': 'b'}])
        self.assertEqual([f['pose']['root']['position'][0] for f in frames], [10, 10, 15, 20])
        self.assertEqual([f['variables']['variable.started'] for f in frames], [1, 1, 1, 2])

    def test_pause_advances_clock_hides_queues_and_defers_instructions_until_resume(self):
        clip = {'loop': True, 'animation_length': 3,
                'timeline': {'0': 'v.n+=1;', '0.5': 'v.n+=10;', '0.9': 'v.n+=100;'},
                'bones': {'root': {'position': ['v.samples+=1;return q.anim_time;', 0, 0]}}}
        frames = self.run_frames({'a': clip}, [{'time': 0, 'animation': 'a'}, {'time': 0.4, 'command': 'pause'},
                                              {'time': 0.8, 'command': 'pause'}, {'time': 1, 'animation': 'a'}])
        self.assertNotIn('root', frames[1]['pose'])
        self.assertNotIn('root', frames[2]['pose'])
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 1, 1, 111])
        self.assertEqual([f['variables']['variable.samples'] for f in frames], [1, 1, 1, 2])
        self.assertEqual(frames[-1]['pose']['root']['position'][0], 1)
        self.assertEqual(frames[-1]['player']['generation'], 1)

    def test_stop_keeps_first_stop_frame_then_fades_without_flushing_future_instructions(self):
        clip = {'animation_length': 2, 'timeline': {'0': 'v.n+=1;', '1.5': 'v.n+=100;'},
                'bones': {'root': {'position': ['10*q.anim_time', 0, 0]}}}
        frames = self.run_frames({'a': clip}, [{'time': 0, 'animation': 'a'}, {'time': 0.2, 'animation': 'a'},
                                              {'time': 0.5, 'command': 'stop'}, {'time': 0.575, 'command': 'stop'},
                                              {'time': 0.66, 'command': 'stop'}, {'time': 1, 'animation': 'a'}],
                                 pose={'root': {'position': [1, 0, 0]}})
        for frame, expected in zip(frames, [0, 2, 2, 1.5, 1, 0]):
            self.assertAlmostEqual(frame['pose']['root']['position'][0], expected)
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 1, 1, 1, 1, 2])
        self.assertEqual(frames[-1]['player']['generation'], 2)

    def test_stop_during_beginning_captures_polled_position_without_inventing_rotation_queue(self):
        clip = {'loop': True, 'bones': {'root': {'position': [12, 0, 0], 'rotation': [40, 0, 0]}}}
        frames = self.run_frames({'a': clip}, [{'time': 0, 'animation': 'a'}, {'time': 0.5, 'animation': 'a'},
                                              {'time': 0.6, 'command': 'stop'}, {'time': 0.675, 'command': 'stop'}],
                                 transition=1, pose={'root': {'position': [4, 0, 0], 'rotation': [20, 0, 0]}})
        self.assertEqual(frames[2]['pose']['root']['position'][0], 8)
        self.assertAlmostEqual(frames[2]['pose']['root']['rotation'][0], 30)
        self.assertAlmostEqual(frames[3]['pose']['root']['position'][0], 6)
        self.assertAlmostEqual(frames[3]['pose']['root']['rotation'][0], 20)

    def test_scale_track_disables_ending_for_every_channel_of_its_bone(self):
        clip = {'animation_length': 1, 'bones': {'root': {'position': [10, 0, 0], 'rotation': [30, 0, 0], 'scale': 3}}}
        frames = self.run_frames({'a': clip}, [{'time': t, 'animation': 'a'} for t in [0, 0.5, 1, 1.001]],
                                 pose={'root': {'position': [2, 0, 0], 'rotation': [4, 0, 0], 'scale': [2, 2, 2]}})
        self.assertEqual(frames[2]['pose']['root']['position'][0], 10)
        self.assertEqual(frames[3]['pose']['root'], {'position': [2, 0, 0], 'rotation': [4, 0, 0], 'scale': [2, 2, 2]})

    def test_once_completion_flushes_late_instructions_once_and_honors_return_scope(self):
        clip = {'animation_length': 1, 'timeline': {'0': 'v.n+=1;', '1.5': ['v.n+=10;', 'return;', 'v.n+=100;']}}
        frames = self.run_frames({'a': clip}, [{'time': t, 'animation': 'a'} for t in [0, 0.7, 1, 1, 1.1, 2]], merge_multiline=True)
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 1, 11, 11, 11, 11])
        self.assertTrue(frames[2]['player']['finished'])

    def test_explicit_restart_replays_initial_instruction_but_stable_request_does_not(self):
        clip = {'loop': True, 'animation_length': 3, 'timeline': {'0': 'v.n+=1;'}}
        frames = self.run_frames({'a': clip}, [{'time': 0, 'animation': 'a'}, {'time': 0, 'animation': 'a'},
                                              {'time': 1, 'animation': 'a'}, {'time': 1.1, 'animation': 'a', 'restart': True}])
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 1, 1, 2])
        self.assertEqual(frames[-1]['player']['generation'], 2)

    def test_loop_override_controls_sampling_and_event_cycles_without_mutating_clip(self):
        clip = {'animation_length': 1, 'timeline': {'0': 'v.n+=1;'}, 'bones': {'root': {'position': ['q.anim_time', 0, 0]}}}
        loop = self.run_frames({'a': clip}, [{'time': t, 'animation': 'a', 'loop': 'loop'} for t in [0, 1.25, 2.5]])
        self.assertEqual([f['variables']['variable.n'] for f in loop], [1, 2, 3])
        self.assertEqual(loop[-1]['pose']['root']['position'][0], 0.5)
        once = self.run_frames({'a': dict(clip, loop=True)}, [{'time': t, 'animation': 'a', 'loop': 'once'} for t in [0, 1, 2]])
        self.assertEqual(once[-1]['player']['phase'], 'idle')
        self.assertEqual(once[-1]['variables']['variable.n'], 1)
        hold = self.run_frames({'a': clip}, [{'time': t, 'animation': 'a', 'loop': 'hold'} for t in [0, 1, 2]])
        self.assertEqual(hold[-1]['pose']['root']['position'][0], 1)

    def test_stopping_loop_freezes_current_cycle_clock_for_weight(self):
        clip = {'loop': True, 'animation_length': 1, 'blend_weight': '1+q.anim_time',
                'bones': {'root': {'position': [10, 0, 0]}}}
        frames = self.run_frames({'a': clip}, [{'time': 0, 'animation': 'a'}, {'time': 1.2, 'animation': 'a'},
                                              {'time': 1.4, 'command': 'stop'}, {'time': 1.475, 'command': 'stop'}])
        self.assertAlmostEqual(frames[2]['pose']['root']['position'][0], 12)
        self.assertAlmostEqual(frames[3]['pose']['root']['position'][0], 7)

    def test_additive_rotation_applies_only_while_running_and_ending_replaces_then_fades(self):
        clip = {'animation_length': 1, 'bones': {'root': {'rotation': [10, 0, 0]}}}
        frames = self.run_frames({'a': clip}, [{'time': t, 'animation': 'a', 'additive_rotation': True}
                                              for t in [0, 0.5, 1, 1.075]], pose={'root': {'rotation': [20, 0, 0]}})
        self.assertEqual([f['pose']['root']['rotation'][0] for f in frames[:3]], [30, 30, 10])
        self.assertAlmostEqual(frames[3]['pose']['root']['rotation'][0], 15)

    def test_rewind_restarts_owner_timeline_without_stale_instruction_cursor(self):
        clip = {'loop': True, 'animation_length': 2, 'timeline': {'0': 'v.n+=1;'}}
        frames = self.run_frames({'a': clip}, [{'time': t, 'animation': 'a'} for t in [0, 0.5, 0]])
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 1, 2])

    def test_missing_clip_stays_idle_without_substituting_idle_animation(self):
        clips = {'idle': {'bones': {'root': {'position': [10, 0, 0]}}}}
        frames = self.run_frames(clips, [{'time': t, 'animation': 'missing'} for t in [0, 1]])
        self.assertEqual(frames[-1]['pose'], {})
        self.assertEqual(frames[-1]['player']['phase'], 'idle')
        self.assertEqual(frames[-1]['player']['generation'], 1)

    def test_loop_end_and_next_cycle_zero_are_distinct_at_exact_boundary(self):
        clip = {'loop': True, 'animation_length': 1, 'timeline': {
            '0': 'v.seq=v.seq*10+1;v.zero_clock=q.anim_time;', '0.5': 'v.seq=v.seq*10+2;',
            '1': 'v.seq=v.seq*10+3;', '1.1': 'v.seq=v.seq*10+4;v.flush_clock=q.anim_time;'},
            'bones': {'root': {'position': ['q.anim_time', 0, 0]}}}
        frames = self.run_frames({'a': clip}, [{'time': t, 'animation': 'a'} for t in [0, 0.5, 1, 1, 1.01, 2, 2.01]])
        self.assertEqual([f['variables']['variable.seq'] for f in frames], [1, 12, 123, 123, 12341, 1234123, 123412341])
        self.assertEqual(frames[2]['pose']['root']['position'][0], 1)
        self.assertEqual(frames[5]['pose']['root']['position'][0], 1)
        self.assertAlmostEqual(frames[4]['variables']['variable.zero_clock'], 0.01)
        self.assertEqual(frames[4]['variables']['variable.flush_clock'], 1)
        self.assertEqual([f['player']['cycle'] for f in frames], [0, 0, 0, 0, 1, 1, 2])

    def test_render_gap_flushes_old_cycle_once_then_runs_current_cycle_like_java(self):
        clip = {'loop': True, 'animation_length': 1, 'timeline': {
            '0': 'v.seq=v.seq*10+1;', '0.5': 'v.seq=v.seq*10+2;',
            '1': 'v.seq=v.seq*10+3;', '1.1': 'v.seq=v.seq*10+4;'}}
        frames = self.run_frames({'a': clip}, [{'time': t, 'animation': 'a'} for t in [0, 3.25, 3.5]])
        # Source AnimationPlayer.process has one resetEventKeyframes call,
        # rather than a loop replaying every unobserved physical duration.
        self.assertEqual([f['variables']['variable.seq'] for f in frames], [1, 12341, 123412])

    def test_zero_length_loop_executes_one_cycle_per_advancing_update_and_observations_do_not_repeat(self):
        clip = {'loop': True, 'animation_length': 0, 'timeline': {
            '0': 'v.n+=1;v.zero_clock=q.anim_time;', '0.1': 'v.n+=10;v.flush_clock=q.anim_time;'}}
        frames = self.run_frames({'a': clip}, [{'time': 0, 'animation': 'a'}, {'time': 0.5, 'animation': 'a'},
                                              {'time': 0.5, 'animation': 'a'}, {'time': 1, 'animation': 'a'},
                                              {'time': 2, 'command': 'pause'}, {'time': 3, 'animation': 'a'}])
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 12, 12, 23, 23, 34])
        self.assertEqual(frames[-1]['variables']['variable.zero_clock'], 0)
        self.assertEqual(frames[-1]['variables']['variable.flush_clock'], 0)
        self.assertEqual([f['player']['cycle'] for f in frames], [0, 1, 1, 2, 2, 3])

    def test_pause_at_loop_boundary_defers_end_instruction_without_starting_next_cycle(self):
        clip = {'loop': True, 'animation_length': 1, 'timeline': {'0': 'v.n+=1;', '1': 'v.n+=10;'}}
        frames = self.run_frames({'a': clip}, [{'time': 0, 'animation': 'a'}, {'time': 1, 'command': 'pause'},
                                              {'time': 1, 'animation': 'a'}, {'time': 1.01, 'animation': 'a'}])
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 1, 11, 12])

    def test_long_render_gap_does_not_replay_thousands_of_missed_cycles(self):
        clip = {'loop': True, 'animation_length': 0.125, 'timeline': {'0': 'v.n+=1;'}}
        frames = self.run_frames({'a': clip}, [{'time': t, 'animation': 'a'} for t in [0, 1024, 1024, 1024.01, 1024.13]])
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 2, 2, 2, 3])

    def test_extreme_cycle_count_is_bounded_and_does_not_overflow_or_hang(self):
        clip = {'loop': True, 'animation_length': 1e-300, 'timeline': {'0': 'v.n+=1;'}}
        frames = self.run_frames({'a': clip}, [{'time': t, 'animation': 'a'} for t in [0, 1e300, 1e300]])
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 2, 2])

    def test_decimal_loop_uses_remainder_but_counts_observed_resets(self):
        clip = {'loop': True, 'animation_length': 0.01, 'timeline': {'0': 'v.n+=1;'},
                'bones': {'root': {'position': ['q.anim_time', 0, 0]}}}
        frames = self.run_frames({'a': clip}, [{'time': t, 'animation': 'a'} for t in [0, 0.03, 0.04]])
        for index, frame in enumerate(frames):
            cycle = frame['player']['cycle']
            clock = frame['pose']['root']['position'][0]
            self.assertEqual(cycle, index)
            self.assertGreaterEqual(clock, 0)
            self.assertLessEqual(clock, 0.01)
            self.assertEqual(frame['variables']['variable.n'], cycle + 1)

    def test_tiny_distinct_timestamps_are_not_dropped_by_a_fixed_epsilon(self):
        clip = {'animation_length': 1, 'timeline': {'0': 'v.n+=1;', '0.000000000001': 'v.n+=10;', '0.000000000002': 'v.n+=100;'}}
        frames = self.run_frames({'a': clip}, [{'time': t, 'animation': 'a'} for t in [0, 1e-12, 2e-12]])
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 11, 111])

    def test_equal_numeric_timestamps_preserve_source_declaration_order(self):
        timeline = {'0.5' + '0' * i: 'v.seq=v.seq*2+' + str(i % 2) + ';' for i in range(21)}
        frames = self.run_frames({'a': {'animation_length': 1, 'timeline': timeline}},
                                 [{'time': 0, 'animation': 'a'}, {'time': 0.5, 'animation': 'a'}])
        self.assertEqual(frames[-1]['variables']['variable.seq'], int(''.join(str(i % 2) for i in range(21)), 2))

    def test_muted_loop_keeps_instruction_cursor_until_reenabled_like_java(self):
        clip = {'loop': True, 'animation_length': 1,
                'timeline': {'0': 'v.n+=1;', '0.5': 'v.n+=10;', '1': 'v.n+=100;'}}
        frames = self.run_frames({'a': clip}, [{'time': 0, 'animation': 'a'}, {'time': 2.25, 'command': 'pause'},
                                              {'time': 2.5, 'animation': 'a'}])
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 1, 11])

    def test_multiple_muted_wraps_do_not_reset_or_accumulate_instruction_cycles(self):
        clip = {'loop': True, 'animation_length': 1,
                'timeline': {'0': 'v.n+=1;', '0.5': 'v.n+=10;', '1': 'v.n+=100;'}}
        frames = self.run_frames({'a': clip}, [{'time': 0, 'animation': 'a'},
                                              {'time': 2.25, 'command': 'pause'},
                                              {'time': 6.25, 'command': 'pause'},
                                              {'time': 6.5, 'animation': 'a'},
                                              {'time': 7.5, 'animation': 'a'}])
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 1, 1, 11, 122])


if __name__ == '__main__':
    unittest.main()
