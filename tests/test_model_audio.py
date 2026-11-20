import json
import unittest
import subprocess

import test_controllers
import test_model_textures
import test_model_functions
from test_import import PROJECT, EXE


class ModelAudioTests(unittest.TestCase):
    manifest = test_model_textures.ModelTextureTests.manifest
    run_pack = test_model_textures.ModelTextureTests.run_pack
    invoke = test_controllers.ControllerTests.invoke
    player = test_model_functions.ModelFunctionTests.player

    def asset(self, model, name):
        return (PROJECT / 'vendor/ysm-upstream/src/main/resources/assets/ysm/builtin/wine_fox'
                / model / 'sounds' / name).read_bytes()

    def test_codecs_custom_directories_aliases_and_unicode(self):
        vorbis = self.asset('22_elf', 'atk1.ogg')
        opus = self.asset('14_momo', 'fly_on.ogg')
        for folder in [True, False]:
            for use_global in [True, False]:
                manifest = self.manifest(['a.png'])
                manifest['files']['player']['sound_path'] = 'fallback' if use_global else 'fx'
                manifest['files']['sound_path'] = 'fx/' if use_global else ' '
                files = {'a.png': test_model_textures.png(), 'ysm.json': json.dumps(manifest).encode(),
                         'fx/効果/strike.data': vorbis, 'fx/copy': vorbis, 'fx/voice.ogg': opus,
                         'fx/notes.txt': b'artist notes', 'sounds/unused.ogg': opus}
                result = self.run_pack(['a.png'], files, folder=folder)
                clips = result['sounds']
                self.assertEqual(set(clips), {'効果/strike', 'copy', 'voice'})
                self.assertEqual(clips['copy'], clips['効果/strike'])
                self.assertEqual(clips['copy']['codec'], 'vorbis')
                self.assertEqual(clips['voice']['codec'], 'opus')
                self.assertEqual(clips['voice']['sample_rate'], 48000)
                self.assertGreater(clips['voice']['frames'], 0)
                self.assertEqual(clips['voice']['decoded_wave_bytes'], 44 + clips['voice']['frames'] * clips['voice']['channels'] * 2)
                self.assertIn('Unknown audio: fx/notes.txt', result['warnings'])

    def test_damaged_recognized_stream_and_duplicate_name(self):
        original = self.asset('22_elf', 'atk1.ogg')
        for damaged in [original[:-1], original[:-1] + bytes([original[-1] ^ 1])]:
            self.run_pack(['a.png'], {'a.png': test_model_textures.png(), 'sounds/a.ogg': damaged},
                          error='Invalid raw sound sounds/a.ogg')
        self.run_pack(['a.png'], {'a.png': test_model_textures.png(), 'sounds/a.ogg': original,
                                  'sounds/a.bin': original}, error='Duplicate raw sound name: a')

    def test_muted_sound_keys_are_consumed_and_enabled_arrays_play_once(self):
        controller = {'states': {'default': {'animations': [{'a': 'q.apply'}]}}}
        clip = {'animation_length': 4, 'sound_effects': {'0': {'effect': 'skip'},
                '1': [{'effect': 'left'}, {'effect': 'right'}]}, 'timeline': {'0': 'v.check=1;'}}
        frames = self.invoke({'player.main': controller}, {'a': clip},
                             [{'time': t, 'queries': {'q.apply': active}} for t, active in [(0, 0), (.5, 1), (1, 1), (1, 1)]])
        self.assertEqual([[e['sound'] for e in frame['sound_cues'] if e['sound']] for frame in frames],
                         [[], [], ['left', 'right'], []])
        self.assertEqual(frames[1]['variables']['variable.check'], 1)

    def test_loop_discards_missed_sound_tail_and_stops_previous_voice(self):
        controller = {'states': {'default': {'animations': ['a']}}}
        clip = {'loop': True, 'animation_length': 1,
                'sound_effects': {'0': {'effect': 'start'}, '.9': {'effect': 'late'}}}
        frames = self.invoke({'player.main': controller}, {'a': clip},
                             [{'time': t} for t in [0, .2, 1.1, 1.1, 1.95]])
        self.assertEqual([[e['sound'] for e in f['sound_cues']] for f in frames],
                         [['start'], [], ['', 'start'], [], ['late']])

    def test_controller_transition_stops_voice_before_new_state(self):
        controller = {'states': {'default': {'animations': ['a'], 'transitions': [{'next': 'q.change'}]},
                                 'next': {'animations': ['b']}}}
        clips = {n: {'animation_length': 5, 'sound_effects': {'0': {'effect': n}}} for n in ['a', 'b']}
        frames = self.invoke({'player.main': controller}, clips,
                             [{'time': 0}, {'time': 1, 'queries': {'q.change': 1}}, {'time': 2}])
        sounds = [e['sound'] for f in frames for e in f['sound_cues']
                  if e['owner'] != 'controller-script:player.main']
        self.assertEqual(sounds, ['a', '', 'b'])

    def test_extra_stops_sound_and_restarts_after_completed_selection(self):
        clips = {'extra': {'animation_length': 1, 'sound_effects': {'0': {'effect': 'voice'}}}}
        frames = self.invoke({}, clips, [
            {'time': 0, 'extra': 'extra', 'extra_time': 0},
            {'time': 1, 'extra': 'extra', 'extra_time': 1},
            {'time': 1.2},
            {'time': 2, 'extra': 'extra', 'extra_time': 0},
            {'time': 2.2},
            {'time': 2.4},
        ], player=True)
        self.assertEqual([[e['sound'] for e in f['sound_cues']] for f in frames],
                         [['voice'], [''], [], ['', 'voice'], [''], []])

    def test_script_sound_ids_modes_and_clamps(self):
        frame = self.player({'@player_init': "ysm.play_sound(0,'anon');ysm.play_sound('0','named',7,-3,2000);"
                             "ysm.play_sound(2.9,'integer',0,0.25,0.5);ysm.stop_sound('0');ysm.stop_sound(0);"
                             "ysm.stop_all_sounds(1);"}, [{'time': 0}])['frames'][0]
        cues = frame['sound_cues']
        self.assertEqual([c['id'] for c in cues], [None, 's:0', 'n:2', 's:0', None, None])
        self.assertTrue(cues[1]['force'] and cues[1]['looping'])
        self.assertAlmostEqual(cues[1]['volume'], .001)
        self.assertEqual(cues[1]['pitch'], 1000)
        self.assertEqual((cues[2]['volume'], cues[2]['pitch']), (.25, .5))
        self.assertTrue(cues[3]['stop_id'] and cues[4]['stop_id'])
        self.assertFalse(cues[5]['stop_id'])

    def test_script_invalid_ids_modes_and_blank_selectors_do_not_play(self):
        frame = self.player({'@player_init': "ysm.play_sound(-1,'a');ysm.play_sound(1,' ');"
                             "ysm.play_sound(1,'a',8);ysm.play_sound(1,'a',-1);ysm.play_sound('','ok');"},
                             [{'time': 0}])['frames'][0]
        self.assertEqual([(c['id'], c['sound']) for c in frame['sound_cues']], [('s:', 'ok')])

    def test_script_failure_discards_sound_effects_from_round(self):
        frame = self.player({'@player_init': "ysm.play_sound(1,'a');v.bad=math.abs(3.5e38);"},
                             [{'time': 0}], failures=1)['frames'][0]
        self.assertEqual(frame['sound_cues'], [])

    def test_script_sound_observation_does_not_repeat_and_bones_cannot_emit(self):
        clips = {'idle': {'loop': True, 'animation_length': 4,
                 'timeline': {'0': "ysm.play_sound(1,'a');"},
                 'bones': {'root': {'position': ["ysm.play_sound(2,'b',v.bad=1)??0", 0, 0]}}}}
        frames = self.player({}, [{'time': 0}, {'time': .2}, {'time': .2, 'observe': True}, {'time': .3}], clips=clips)['frames']
        self.assertEqual([[c['sound'] for c in f['sound_cues']] for f in frames], [[], ['a'], [], []])
        self.assertNotIn('variable.bad', frames[-1]['variables'])

    def test_deferred_sound_keeps_animation_scope_and_global_override(self):
        clips = {'idle': {'loop': True, 'animation_length': 4,
                 'timeline': {'0': "ysm.play_sound(1,'direct');ysm.defer(0);"}}}
        frame = self.player({'@defer': "ysm.play_sound(2,'deferred');ysm.play_sound(3,'global',2);"},
                             [{'time': 0}, {'time': .2}], clips=clips)['frames'][-1]
        cues = frame['sound_cues']
        self.assertEqual([c['sound'] for c in cues], ['direct', 'deferred', 'global'])
        self.assertEqual(cues[0]['owner'], cues[1]['owner'])
        self.assertNotEqual(cues[0]['owner'], 'global')
        self.assertEqual(cues[2]['owner'], 'global')

    def test_controller_sound_script_retires_only_its_scope(self):
        controller = {'states': {
            'default': {'on_entry': "ysm.play_sound('voice','a');", 'transitions': [{'next': 'q.go'}]},
            'next': {'on_entry': "ysm.play_sound('voice','b');"}}}
        frames = self.player({'@player_init': "ysm.play_sound('voice','global');"},
                             [{'time': 0}, {'time': .2, 'queries': {'q.go': 1}}],
                             controllers={'player.main': controller})['frames']
        cues = [c for f in frames for c in f['sound_cues']]
        self.assertEqual([c['sound'] for c in cues], ['global', 'a', '', 'b'])
        self.assertEqual([c['owner'] for c in cues[1:]], ['controller-script:player.main'] * 3)

    def test_state_sounds_enter_once_reenter_and_preserve_exit_script(self):
        controller = {'states': {'default': {
            'sound_effects': [{'effect': 'entry'}, {}, {'effect': ' \t'}, {'effect': 'entry'}],
            'on_entry': "ysm.play_sound(1,'script');",
            'on_exit': "ysm.play_sound(2,'exit');",
            'transitions': [{'default': 'q.again'}]}}}
        frames = self.invoke({'player.main': controller}, {}, [
            {'time': 0}, {'time': .1}, {'time': .1, 'observe': True},
            {'time': .2, 'queries': {'q.again': 1}}, {'time': .2}], player=True)
        self.assertEqual([[c['sound'] for c in f['sound_cues']] for f in frames],
                         [['script', 'entry', 'entry'], [], [], ['', 'exit', 'script', 'entry', 'entry'], []])
        self.assertTrue(all(c['owner'] == 'controller-script:player.main'
                            for f in frames for c in f['sound_cues']))

    def test_nested_state_sounds_retire_both_scopes_before_new_entry(self):
        controllers = {
            'player.main': {'initial_state': 'ysm-entry-sub', 'states': {
                'ysm-entry-sub': {'sound_effects': [{'effect': 'parent'}],
                                  'transitions': [{'done': 'q.quit'}]},
                'done': {'sound_effects': [{'effect': 'done'}]}}},
            'player.main.sub': {'states': {'default': {'sound_effects': [{'effect': 'child'}]}}}}
        frames = self.invoke(controllers, {}, [{'time': 0}, {'time': 1, 'queries': {'q.quit': 1}}])
        self.assertEqual([(c['owner'], c['sound']) for c in frames[1]['sound_cues']],
                         [('controller-script:player.main', ''), ('controller-script:player.main.sub', ''),
                          ('controller-script:player.main', 'done')])

    def test_state_sounds_are_independent_of_animation_conditions_and_blend(self):
        controller = {'states': {'default': {'animations': [{'a': 0}], 'blend_transition': 2,
                                             'sound_effects': [{'effect': 'state'}]}}}
        frames = self.invoke({'player.main': controller},
                             {'a': {'animation_length': 4, 'sound_effects': {'0': {'effect': 'muted'}}}},
                             [{'time': t} for t in [0, 1, 2, 3]], full_report=True)
        self.assertEqual([[c['sound'] for c in f['sound_cues']] for f in frames['frames']],
                         [['state'], [], [], []])
        self.assertNotIn('Controller sounds not connected', str(frames))

    def test_invalid_controller_sounds_are_rejected(self):
        for effects in [None, {}, 'voice', [None], ['voice'], [{'effect': 3}], [{}] * 4097]:
            with self.subTest(effects=str(effects)[:50]):
                self.invoke({'player.main': {'states': {'default': {'sound_effects': effects}}}},
                            {}, [{'time': 0}], succeeds=False)

    def test_final_instruction_sound_is_retired_after_instruction_flush(self):
        clip = {'animation_length': 1, 'timeline': {'1': "ysm.play_sound(1,'last',4);"}}
        controller = {'states': {'default': {'animations': ['a']}}}
        frames = self.invoke({'player.main': controller}, {'a': clip},
                             [{'time': t} for t in [0, .5, 1.2, 1.4]])
        cues = frames[2]['sound_cues']
        self.assertEqual([c['sound'] for c in cues], ['last', ''])
        self.assertTrue(cues[0]['looping'])
        self.assertEqual(cues[0]['owner'], cues[1]['owner'])

    def selectors(self, names):
        run = subprocess.run([str(EXE), '--sound-selectors', json.dumps(names)],
                             capture_output=True, encoding='utf-8', timeout=5)
        self.assertEqual(run.returncode, 0, run.stderr)
        return json.loads(run.stdout)

    def test_game_sound_table_matches_pinned_source_including_pitch(self):
        source = json.loads((PROJECT / 'vendor/geyser-mappings/sounds.json').read_text())
        names = [name for name, entry in source.items() if entry.get('playsound_mapping')]
        self.assertEqual(len(names), 1917)
        for offset in range(0, len(names), 150):
            batch = names[offset:offset + 150]
            actual = self.selectors(['minecraft:' + name for name in batch])
            for name, value in zip(batch, actual):
                self.assertEqual(value['name'], source[name]['playsound_mapping'], name)
                self.assertAlmostEqual(value['pitch'], source[name].get('pitch_adjust', 1), places=6)

    def test_playing_origin_and_gain_follow_entity_without_a_new_cue(self):
        def tick(xyz, percent, ready=True):
            return dict(position=xyz, percent=percent, ready=ready)
        a, b, c = [3, 64, 9], [-2, 64.5, 10], [-2, 60.25, 11]
        request = dict(volume=.625, position=a, percent=40, ticks=[
            tick(a, 40), tick(b, 0, False), tick(b, 0), tick(c, 0), tick(c, 80), tick(c, 80)])
        run = subprocess.run([str(EXE), '--sound-follow', json.dumps(request)],
                             check=True, capture_output=True, encoding='utf-8', timeout=5)
        states = json.loads(run.stdout)
        self.assertEqual([r['position'] for r in states], [a, a, b, c, c, c])
        self.assertEqual([r['volume'] for r in states], [.25, .25, 0, 0, .5, .5])
        self.assertEqual([(r['moved'], r['adjusted'], r['applied']) for r in states],
                         [(False, False, False), (True, True, False), (True, True, True),
                          (True, False, True), (False, True, True), (False, False, False)])

    def test_game_sound_namespace_validation_and_custom_resources(self):
        names = ['voice', 'minecraft:', 'bad:Upper', 'UPPER:path', 'space:name with spaces',
                 'a:b:c', 'a\\b:c', 'a:声音', ':entity.player.levelup',
                 'my_pack:effects/voice', 'minecraft:resource_pack_custom', 'minecraft:note.harp']
        result = self.selectors(names)
        self.assertEqual(result[:8], [None] * 8)
        self.assertEqual([x['name'] for x in result[8:]],
                         ['random.levelup', 'my_pack:effects/voice', 'resource_pack_custom', 'note.harp'])
