"""Java source null-state and unresolved-transition behavior, with strict gates retained."""
import unittest
import test_controllers as shared


class SourceControllerTests(unittest.TestCase):
    invoke = shared.ControllerTests.invoke
    clip = staticmethod(shared.ControllerTests.clip)
    state = staticmethod(shared.ControllerTests.state)

    def test_empty_definition_suppresses_main_fallback_without_inventing_a_state(self):
        frames = self.invoke({'player.main': {}}, {'idle': self.clip(9)},
                             [{'time': 0}, {'time': 0.2}], player=True, source_profile=True)
        for frame in frames:
            self.assertNotIn('root', frame['pose'])
            self.assertNotIn('player.main', frame['controllers']['coded'])
            self.assertEqual(frame['controllers']['states']['player.main']['state'], '')
            self.assertEqual(frame['controllers']['states']['player.main']['generation'], 0)

    def test_empty_parallel_slot_suppresses_duplicate_pose_from_its_builtin_clip(self):
        clips = {'idle': self.clip(2), 'parallel0': {'loop': True, 'bones': {'root': {'rotation': [10, 0, 0]}}},
                 'parallel1': {'loop': True, 'bones': {'root': {'rotation': [20, 0, 0]}}}}
        frames = self.invoke({'player.parallel_1': {}}, clips, [{'time': 0}, {'time': 0.2}],
                             player=True, source_profile=True)
        self.assertEqual(frames[-1]['pose']['root']['rotation'][0], 10)
        self.assertNotIn('player.parallel_1', frames[-1]['controllers']['coded'])

    def test_explicit_empty_states_are_source_placeholders_but_fail_strict_admission(self):
        definition = {'player.main': {'states': {}}}
        frames = self.invoke(definition, {}, [{'time': 0}], source_profile=True)
        self.assertTrue(frames[0]['handled']['player.main'])
        self.assertEqual(frames[0]['pose'], {})
        self.invoke(definition, {}, [{'time': 0}], succeeds=False)

    def test_missing_initial_state_does_not_select_an_arbitrary_state_or_run_entry(self):
        definition = {'player.main': {'initial_state': 'missing',
                                     'states': {'default': {'animations': ['a'], 'on_entry': ['v.entered=1;']}}}}
        frames = self.invoke(definition, {'a': self.clip(4)}, [{'time': 0}, {'time': 0.3}], source_profile=True)
        for frame in frames:
            self.assertTrue(frame['handled']['player.main'])
            self.assertEqual(frame['pose'], {})
            self.assertNotIn('variable.entered', frame['variables'])
        self.assertTrue(any('Unresolved initial' in s for s in frames[-1]['controllers']['diagnostics']))
        self.invoke(definition, {}, [{'time': 0}], succeeds=False)

    def test_first_true_missing_destination_stops_the_update_without_hooks_or_later_conditions(self):
        definition = {'player.main': {'states': {
            'default': {'animations': ['a'], 'on_exit': ['v.exited+=1;'],
                        'transitions': [{'missing': 'q.bad'}, {'good': 'v.later+=1;return 1;'}]},
            'good': {'animations': ['b'], 'on_entry': ['v.entered+=1;']}}}}
        frames = self.invoke(definition, {'a': self.clip(2), 'b': self.clip(7)},
                             [{'time': t, 'queries': {'q.bad': bad}} for t, bad in [(0, 1), (0.2, 1), (0.2, 0), (0.4, 0)]],
                             source_profile=True)
        self.assertEqual([self.state(f) for f in frames], ['default', 'default', 'default', 'good'])
        for frame in frames[:3]:
            for key in ['variable.entered', 'variable.exited', 'variable.later']:
                self.assertNotIn(key, frame['variables'])
        self.assertEqual([frames[-1]['variables']['variable.' + key] for key in ['entered', 'exited', 'later']], [1, 1, 1])
        self.assertTrue(any('missing' in s for s in frames[-1]['controllers']['diagnostics']))

    def test_false_missing_destination_does_not_block_a_later_true_transition(self):
        definition = {'player.main': {'states': {
            'default': {'animations': ['a'], 'transitions': [{'missing': 0}, {'good': 1}]},
            'good': {'animations': ['b']}}}}
        frames = self.invoke(definition, {'a': self.clip(2), 'b': self.clip(7)}, [{'time': 0}, {'time': 0.2}], source_profile=True)
        self.assertEqual(self.state(frames[-1]), 'good')
        self.assertEqual(frames[-1]['pose']['root']['position'][0], 7)

    def test_combined_preparallel_group_runs_each_timeline_once_with_suppressed_slots(self):
        definition = {'player.pre_parallel_0': {'states': {'default': {'animations': ['pre_parallel0', 'pre_parallel1']}}},
                      'player.pre_parallel_1': {}}
        clips = {'idle': {'loop': True}, 'pre_parallel0': self.clip(1),
                 'pre_parallel1': dict(self.clip(2), animation_length=10, timeline={'0': 'v.events+=1;'})}
        frames = self.invoke(definition, clips, [{'time': 0}, {'time': 0.2}], player=True, source_profile=True)
        self.assertEqual(frames[-1]['pose']['root']['position'][0], 3)
        self.assertEqual(frames[-1]['variables']['variable.events'], 1)

    def test_render_gap_in_parallel_players_resets_each_once_without_event_storm(self):
        definitions = {'player.parallel_%d' % i: {'states': {'default': {'animations': ['a']}}}
                       for i in range(6)}
        clips = {'idle': {'loop': True}, 'a': {'loop': True, 'animation_length': 0.125,
                 'timeline': {'0': 'v.n+=1;', '0.125': 'v.n+=10;'}}}
        report = self.invoke(definitions, clips, [{'time': t} for t in [0, 1024, 1024, 1024.01]],
                             player=True, source_profile=True, full_report=True)
        self.assertEqual(report['evaluation_diagnostics'], [])
        self.assertEqual([f['variables']['variable.n'] for f in report['frames']], [6, 72, 72, 72])

    def test_muted_source_loop_keeps_instructions_but_advances_sound_keys(self):
        definitions = {'player.main': {'states': {'default': {'animations': [{'a': 'q.apply'}]}}}}
        clip = {'loop': True, 'animation_length': 1,
                'timeline': {'0': 'v.n+=1;', '0.5': 'v.n+=10;', '1': 'v.n+=100;'},
                'sound_effects': {'0': {'effect': 'zero'}, '0.5': {'effect': 'middle'}, '1': {'effect': 'end'}}}
        frames = self.invoke(definitions, {'a': clip},
                             [{'time': t, 'queries': {'q.apply': active}}
                              for t, active in [(0, 1), (2.75, 0), (6.75, 0), (6.875, 1), (7.875, 1)]],
                             source_profile=True)
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 1, 1, 11, 122])
        # Muted execution consumes keys up to .75; re-enabling at .875 must not
        # play the unheard zero/middle sounds. A later live wrap starts them anew.
        heard = lambda frame: [c['sound'] for c in frame['sound_cues'] if c['sound']]
        self.assertEqual(heard(frames[0]), ['zero'])
        self.assertEqual([heard(f) for f in frames[1:4]], [[], [], []])
        self.assertEqual(heard(frames[4]), ['zero', 'middle'])

    def test_source_profile_retains_structural_and_count_limits(self):
        for states in [None, [], 'bad', {'state%d' % i: {} for i in range(4097)}]:
            with self.subTest(kind=type(states).__name__):
                self.invoke({'player.main': {'states': states}}, {}, [{'time': 0}], source_profile=True, succeeds=False)
        self.invoke({'player.parallel_%d' % i: {} for i in range(513)}, {}, [{'time': 0}], source_profile=True, succeeds=False)


if __name__ == '__main__':
    unittest.main()
