import json
import pathlib
import subprocess
import tempfile
import unittest

from test_import import EXE, SAMPLE


class HandAnimationTests(unittest.TestCase):
    def run_frames(self, clips, frames, origin='', fallbacks=()):
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder) / 'hands.json'
            path.write_text(json.dumps({'animations': clips, 'ysm_origin_version': origin,
                                        'ysm_fallback_animations': fallbacks}), encoding='utf-8')
            result = subprocess.run([str(EXE), '--hand-animation-file', str(path), json.dumps({'frames': frames})],
                                    capture_output=True, encoding='utf-8', timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(result.stdout)
            self.assertEqual(report['expression_failures'], 0, report)
            self.assertEqual(report['compile_diagnostics'], [], report)
            return report['frames']

    def test_identifier_then_declared_tag_then_category_then_use_action(self):
        clips = {name: {'loop': True} for name in [
            'hold_mainhand:mace', 'hold_mainhand:eat', 'hold_mainhand#tools:z',
            'hold_mainhand#tools:a', 'hold_mainhand$minecraft:mace']}
        item = {'id': 'minecraft:mace', 'category': 'mace', 'use_action': 'eat', 'tags': ['tools:a', 'tools:z']}
        frames = [{'time': i, 'items': [dict(item, **change)]} for i, change in enumerate([
            {}, {'id': 'test:tool'}, {'id': 'test:tool', 'tags': []},
            {'id': 'test:tool', 'tags': [], 'category': ''}])]
        names = [f['layers'][0]['name'] for f in self.run_frames(clips, frames)]
        self.assertEqual(names, ['hold_mainhand$minecraft:mace', 'hold_mainhand#tools:z', 'hold_mainhand:mace', 'hold_mainhand:eat'])

    def test_native_spears_do_not_play_trident_or_generic_hand_animations(self):
        clips = {name: {} for name in ('hold_mainhand:spear', 'swing:spear', 'use_mainhand:spear',
                                      'swing_hand', 'use_mainhand')}
        materials = ('wooden', 'stone', 'copper', 'iron', 'golden', 'diamond', 'netherite')
        frames = [dict(time=index * 4 + phase, native_bedrock=True, items=[{'id': f'minecraft:{material}_spear'}],
                       swinging_hand=swing, using_hand=use)
                  for index, material in enumerate(materials)
                  for phase, (swing, use) in enumerate([(-1, -1), (0, -1), (-1, 0), (-1, -1)])]
        result = self.run_frames(clips, frames)
        self.assertTrue(all(f['native_melee_spear_pose'] and not f['layers'] for f in result))

    def test_native_spear_prefers_author_selectors_and_leaves_trident_on_legacy_category(self):
        clips = {name: {} for name in ('hold_mainhand:spear', 'hold_mainhand:melee_spear',
                                      'hold_mainhand#minecraft:is_spear', 'hold_mainhand$minecraft:wooden_spear')}
        items = [{'id': 'minecraft:wooden_spear'}, {'id': 'minecraft:stone_spear'},
                 {'id': 'test:lance', 'tags': ['minecraft:is_spear']}, {'id': 'minecraft:trident'},
                 {'id': 'test:wooden_spear'}]
        frames = [dict(time=i, native_bedrock=True, items=[item]) for i, item in enumerate(items)]
        names = lambda result: [[l['name'] for l in f['layers']] for f in result]
        self.assertEqual(names(self.run_frames(clips, frames)), [
            ['hold_mainhand$minecraft:wooden_spear'], ['hold_mainhand#minecraft:is_spear'],
            ['hold_mainhand#minecraft:is_spear'], ['hold_mainhand:spear'], []])
        del clips['hold_mainhand#minecraft:is_spear']
        self.assertEqual(names(self.run_frames(clips, frames[1:3])), [['hold_mainhand:melee_spear']] * 2)

    def test_native_spear_uses_authored_actions_only_in_the_matching_phase(self):
        clips = {name: {'loop': True} for name in ('hold_mainhand:melee_spear', 'swing:melee_spear')}
        item = {'id': 'minecraft:wooden_spear', 'use_action': 'spear'}
        states = [(-1, -1), (0, -1), (0, 0), (-1, -1)]
        frames = self.run_frames(clips, [dict(time=i, native_bedrock=True, items=[item], swinging_hand=swing, using_hand=use)
                                         for i, (swing, use) in enumerate(states)])
        self.assertEqual([f['native_melee_spear_pose'] for f in frames], [False, False, True, False])
        self.assertEqual([[l['name'] for l in f['layers']] for f in frames], [
            ['hold_mainhand:melee_spear'], ['swing:melee_spear'], [], ['hold_mainhand:melee_spear']])

    def test_official_lance_precedes_trident_and_keeps_authored_preview_selectors(self):
        official = 'hold_mainhand:lance'
        legacy = 'hold_mainhand:melee_spear'
        java_tag = 'hold_mainhand#minecraft:spears'
        clips = {name: {'loop': True} for name in (official, legacy, 'hold_mainhand:spear')}
        frame = {'time': 0, 'native_bedrock': True, 'items': [{'id': 'minecraft:copper_spear'}]}
        self.assertEqual(self.run_frames(clips, [frame])[0]['layers'][0]['name'], official)
        self.assertEqual(self.run_frames(clips, [frame], fallbacks=[official])[0]['layers'][0]['name'], legacy)
        clips[java_tag] = {'loop': True}
        self.assertEqual(self.run_frames(clips, [frame], fallbacks=[official])[0]['layers'][0]['name'], java_tag)

    def test_source_model_without_lance_uses_embedded_ysms_hold_jab_and_charge(self):
        mainhand = {'id': 'minecraft:wooden_spear'}
        trident = {'id': 'minecraft:trident'}
        states = [(mainhand, -1, -1), (mainhand, 0, -1), (mainhand, -1, 0), (mainhand, -1, -1), (trident, -1, 0)]
        frames = [dict(time=i, native_bedrock=True, items=[item], swinging_hand=swing, using_hand=use)
                  for i, (item, swing, use) in enumerate(states)]
        run = subprocess.run([str(EXE), '--hand-animations', str(SAMPLE), json.dumps({'frames': frames})],
                             capture_output=True, encoding='utf-8', timeout=10)
        self.assertEqual(run.returncode, 0, run.stderr)
        report = json.loads(run.stdout)['frames']
        for frame, name in zip(report, ['hold_mainhand:lance', 'swing:lance', 'use_mainhand:lance',
                                        'hold_mainhand:lance', 'use_mainhand:spear']):
            self.assertIn(name, [layer['name'] for layer in frame['layers']])
            self.assertFalse(frame['native_melee_spear_pose'])

    def test_native_crossbow_load_aim_fire_and_reload_authored_timeline(self):
        clips = {name: {'loop': True, 'timeline': {'0': f'v.aimbow={aim};'}} for name, aim in (
            ('hold_mainhand:crossbow', 0), ('use_mainhand:crossbow', 1), ('hold_mainhand:charged_crossbow', 1))}
        states = [(-1, False), (0, False), (-1, True), (-1, True), (-1, False), (0, False), (-1, True)]
        frames = self.run_frames(clips, [dict(time=i, native_bedrock=True, using_hand=use,
                                              items=[{'id': 'minecraft:crossbow', 'charged': charged}])
                                         for i, (use, charged) in enumerate(states)])
        self.assertEqual([f['layers'][-1]['name'] for f in frames], [
            'hold_mainhand:crossbow', 'use_mainhand:crossbow', 'hold_mainhand:charged_crossbow',
            'hold_mainhand:charged_crossbow', 'hold_mainhand:crossbow', 'use_mainhand:crossbow', 'hold_mainhand:charged_crossbow'])
        self.assertEqual([f['variables']['variable.aimbow'] for f in frames], [0, 1, 1, 1, 0, 1, 1])

    def test_empty_hand_and_unknown_item_do_not_substitute_idle(self):
        clips = {'idle': {'bones': {'root': {'position': [2, 0, 0]}}},
                 'hold_mainhand:empty': {'loop': True, 'bones': {'root': {'position': [4, 0, 0]}}}}
        frames = self.run_frames(clips, [{'time': 0}, {'time': 1, 'items': [{'id': 'test:unknown'}]}])
        self.assertEqual(frames[0]['pose']['root']['position'][0], 4)
        self.assertEqual(frames[1]['layers'], [])
        self.assertEqual(frames[1]['pose']['root']['position'][0], 2)

    def test_hold_clock_advances_while_use_hides_its_queues_and_resumes_without_restart(self):
        clips = {name: {'loop': True, 'animation_length': 10} for name in ['hold_mainhand:bow', 'use_mainhand:bow']}
        item = {'id': 'minecraft:bow', 'category': 'bow'}
        frames = self.run_frames(clips, [{'time': t, 'items': [item], 'using_hand': hand}
                                        for t, hand in [(0, -1), (1, -1), (2, 0), (5, 0), (6, -1)]])
        hold = [l for f in frames for l in f['layers'] if l['owner'] == 'hand.hold_mainhand']
        self.assertEqual([l['time'] for l in hold], [0, 1, 6])
        self.assertEqual([l['restart'] for l in hold], [True, False, False])
        for f in frames[2:4]:
            self.assertNotIn('hand.hold_mainhand', [l['owner'] for l in f['layers']])
        self.assertEqual(frames[3]['layers'][-1]['time'], 3)
        self.assertEqual(len(frames[4]['layers']), 1)

    def test_item_change_restarts_same_clip_and_zero_time_instruction(self):
        clips = {'hold_mainhand:sword': {'loop': True, 'timeline': {'0': 'v.n+=1;'}}}
        item = {'id': 'minecraft:wooden_sword', 'category': 'sword'}
        frames = self.run_frames(clips, [{'time': 0, 'items': [dict(item, identity=identity)]}
                                        for identity in ['slot0', 'slot0', 'slot1', 'slot1']])
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 1, 2, 2])

    def test_repeated_use_restarts_after_release_even_at_same_time(self):
        clips = {'use_mainhand': {'loop': True, 'timeline': {'0': 'v.n+=1;'}}}
        frames = self.run_frames(clips, [{'time': 0, 'using_hand': hand} for hand in [0, 0, -1, 0]])
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 1, 1, 2])
        self.assertTrue(frames[-1]['layers'][-1]['restart'])

    def test_swing_continues_after_input_and_restarts_on_next_swing(self):
        clips = {'swing:sword': {'loop': 'hold_on_last_frame', 'animation_length': 1,
                                'timeline': {'0': 'v.n+=1;'}, 'bones': {'arm': {'rotation': ['q.anim_time', 0, 0]}}}}
        item = {'id': 'minecraft:iron_sword', 'category': 'sword'}
        frames = self.run_frames(clips, [{'time': t, 'items': [item], 'swinging_hand': hand, 'swing_progress': phase}
                                        for t, hand, phase in [(0, 0, 1), (0.2, 0, 4), (0.3, -1, 0),
                                                               (0.4, 0, 1), (0.6, 0, 4), (0.7, 0, 1), (2, -1, 0)]])
        self.assertEqual([f['variables']['variable.n'] for f in frames], [1, 1, 1, 2, 2, 3, 3])
        self.assertAlmostEqual(frames[2]['pose']['arm']['rotation'][0], 0.3)
        self.assertEqual(frames[-1]['pose']['arm']['rotation'][0], 1)

    def test_offhand_mainhand_swing_use_order_and_other_bones_survive(self):
        clips = {name: {'loop': True, 'bones': {bone: {'position': [value, 0, 0]}}} for name, bone, value in [
            ('idle', 'leg', 7), ('hold_offhand:bow', 'arm', 1), ('hold_mainhand:sword', 'arm', 2),
            ('swing:sword', 'arm', 3), ('use_offhand:bow', 'arm', 4)]}
        items = [{'id': 'minecraft:iron_sword', 'category': 'sword'}, {'id': 'minecraft:bow', 'category': 'bow'}]
        frames = self.run_frames(clips, [{'time': 0, 'items': items},
                                        {'time': 0.1, 'items': items, 'swinging_hand': 0, 'using_hand': 1}])
        self.assertEqual([l['owner'] for l in frames[0]['layers']], ['hand.hold_offhand', 'hand.hold_mainhand'])
        self.assertEqual([l['owner'] for l in frames[1]['layers']], ['hand.swing', 'hand.use'])
        self.assertEqual(frames[1]['pose']['arm']['position'][0], 4)
        self.assertEqual(frames[1]['pose']['leg']['position'][0], 7)

    def test_charged_crossbow_and_fishing_take_priority(self):
        clips = {name: {} for name in ['hold_mainhand:charged_crossbow', 'hold_mainhand:fishing', 'hold_mainhand$minecraft:crossbow']}
        frames = self.run_frames(clips, [{'time': 0, 'items': [{'id': 'minecraft:crossbow', 'charged': True}]},
                                        {'time': 1, 'items': [{'id': 'minecraft:fishing_rod', 'fishing': True}]}])
        self.assertEqual([f['layers'][0]['name'] for f in frames], ['hold_mainhand:charged_crossbow', 'hold_mainhand:fishing'])

    def test_equipment_change_retires_final_swing_pose_and_prop(self):
        clips = {'idle': {'bones': {'blade': {'scale': 0}}},
                 'swing:sword': {'loop': 'hold_on_last_frame', 'animation_length': 1, 'bones': {'blade': {'scale': 1}}}}
        sword = {'id': 'minecraft:iron_sword', 'category': 'sword'}
        frames = self.run_frames(clips, [{'time': 0, 'items': [sword], 'swinging_hand': 0},
                                        {'time': 2, 'items': [sword]}, {'time': 3}])
        self.assertEqual(frames[1]['pose']['blade']['scale'], [1, 1, 1])
        self.assertEqual(frames[2]['pose']['blade']['scale'], [0, 0, 0])
        self.assertEqual(frames[2]['layers'], [])

    def test_preview_disables_and_reactivation_restarts_hand_events(self):
        clips = {'hold_mainhand:empty': {'loop': True, 'timeline': {'0': 'v.n+=1;'}}}
        frames = self.run_frames(clips, [{'time': 0}, {'time': 1, 'enabled': False}, {'time': 2}])
        self.assertEqual(frames[1]['layers'], [])
        self.assertEqual(frames[2]['variables']['variable.n'], 2)
        self.assertEqual(frames[2]['layers'][0]['time'], 0)

    def test_supplied_package_sword_selection_reveals_authored_prop(self):
        context = {'frames': [{'time': 0}, {'time': 0.2, 'items': [{'id': 'minecraft:wooden_sword', 'category': 'sword'}]}]}
        result = subprocess.run([str(EXE), '--hand-animations', str(SAMPLE), json.dumps(context)],
                                capture_output=True, encoding='utf-8', timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report['expression_failures'], 0, report)
        self.assertEqual(report['frames'][0]['pose']['MiaoJian']['scale'], [0, 0, 0])
        self.assertIn('hold_mainhand:sword', [l['name'] for l in report['frames'][1]['layers']])
        self.assertTrue(all(v > 0 for v in report['frames'][1]['pose']['MiaoJian']['scale']))

    def test_origin_version_boundary_selects_authored_or_legacy_hold_loop(self):
        clip = {'animation_length': 1, 'bones': {'arm': {'position': ['q.anim_time', 0, 0]}},
                'timeline': {'0': 'v.n+=1;'}}
        item = {'id': 'test:sword', 'category': 'sword'}
        versions = [('', True), ('  ', True), ('2.3.0', False), ('2.4.1', False),
                    ('2.4.2-snapshot', False), ('2.4.2-snapshot-31', False),
                    ('2.4.2-snapshot-32', True), ('2.4.2-SNAPSHOT-32+build', True),
                    ('2.4.2-rc1', False), ('2.4.2', True), ('2.4.2-final', True), ('3.0.0', True),
                    (0, False), (18, False), (19, True), (31, True)]
        for version, authored in versions:
            with self.subTest(origin=version):
                frames = self.run_frames({'hold_mainhand:sword': clip},
                                         [{'time': t, 'items': [item]} for t in [0, 1.25]], origin=version)
                if authored:
                    self.assertNotIn('arm', frames[-1]['pose'])
                    self.assertEqual(frames[-1]['variables']['variable.n'], 1)
                else:
                    self.assertAlmostEqual(frames[-1]['pose']['arm']['position'][0], 0.25)
                    self.assertEqual(frames[-1]['variables']['variable.n'], 2)

    def test_legacy_use_forces_loop_but_current_use_honors_hold_last_frame(self):
        clip = {'animation_length': 1, 'loop': 'hold_on_last_frame',
                'bones': {'arm': {'position': ['q.anim_time', 0, 0]}}}
        for version, expected in [('2.4.2-snapshot-31', 0.25), ('2.4.2-snapshot-32', 1)]:
            with self.subTest(origin=version):
                frames = self.run_frames({'use_mainhand': clip},
                                         [{'time': t, 'using_hand': 0} for t in [0, 1.25]], origin=version)
                self.assertEqual(frames[-1]['pose']['arm']['position'][0], expected)

    def test_legacy_swing_forces_once_while_current_swing_honors_authored_loop(self):
        clip = {'animation_length': 1, 'loop': True,
                'bones': {'arm': {'position': ['q.anim_time', 0, 0]}}, 'timeline': {'0': 'v.n+=1;'}}
        item = {'id': 'test:sword', 'category': 'sword'}
        for version, authored in [('2.4.2-snapshot-31', False), ('2.4.2-snapshot-32', True)]:
            with self.subTest(origin=version):
                frames = self.run_frames({'swing:sword': clip},
                                         [{'time': 0, 'items': [item], 'swinging_hand': 0},
                                          {'time': 1.25, 'items': [item]}], origin=version)
                if authored:
                    self.assertEqual(frames[-1]['pose']['arm']['position'][0], 0.25)
                    self.assertEqual(frames[-1]['variables']['variable.n'], 2)
                else:
                    self.assertNotIn('arm', frames[-1]['pose'])
                    self.assertEqual(frames[-1]['variables']['variable.n'], 1)


if __name__ == '__main__':
    unittest.main()
