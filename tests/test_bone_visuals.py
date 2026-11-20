"""Bone surface effects, source action boundaries and the native draw partition."""
import json
import pathlib
import subprocess
import tempfile
import unittest
import zipfile

from test_import import EXE


class CollapsedSkinMatrixTests(unittest.TestCase):
    def test_mixed_surface_keeps_its_glow_pass_across_scale_transitions(self):
        identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
        hidden = [0] * 12 + [5, -7, 100, 1]
        for current, previous in ((hidden, hidden), (identity, hidden), (hidden, identity)):
            with self.subTest(current=current, previous=previous):
                palette = [{'current': identity, 'previous': identity},
                           {'glow': 15, 'current': current, 'previous': previous}]
                run = subprocess.run([str(EXE), '--bone-visual-plan', json.dumps(palette)],
                                     capture_output=True, encoding='utf-8', timeout=5)
                self.assertEqual(run.returncode, 0, run.stderr)
                passes = json.loads(run.stdout)
                self.assertEqual([(part['bones'], part['glow']) for part in passes],
                                 [([0], -1), ([1], 15)])

    def test_only_fully_collapsed_basis_is_culled(self):
        identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
        hidden = [0] * 12 + [5, -7, 100, 1]
        sheet = identity.copy()
        sheet[10] = 0
        tiny = hidden.copy()
        tiny[0] = 1e-20
        shear = hidden.copy()
        shear[1] = .5
        unpopulated = [0] * 16
        run = subprocess.run([str(EXE), '--collapsed-skin-matrix',
                              json.dumps([identity, hidden, sheet, tiny, shear, unpopulated])],
                             capture_output=True, encoding='utf-8', timeout=5)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(json.loads(run.stdout), [False, True, False, False, False, False])
import test_model_library as library


WHITE = {'color': [255, 255, 255], 'alpha': 255, 'glow': -1}


class BoneVisualTests(unittest.TestCase):
    def test_one_authored_clip_moves_and_updates_surfaces_in_all_three_controllers(self):
        fixture = pathlib.Path(__file__).with_name('fixtures') / 'java_surface_animation.json'
        clip = json.loads(fixture.read_text(encoding='utf-8'))
        geometry = {'minecraft:geometry': [{'bones': [{'name': name} for name in ('Tint', 'Veil', 'Lamp')]}]}
        plan = {'frames': [{'time': time} for time in (0, 1.5, 3.0, 6.0, 6.1)], 'geometry': geometry}
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'shared-animation.json'
            path.write_text(json.dumps({'animations': {'parallel0': clip}}), encoding='utf-8')
            for kind in ('source-player', 'projectile', 'vehicle'):
                with self.subTest(kind=kind):
                    result = json.loads(subprocess.check_output(
                        [str(EXE), f'--{kind}-animation-file', str(path), json.dumps(plan)], encoding='utf-8', timeout=5))
                    self.assertEqual(result['expression_failures'], 0)
                    summaries = []
                    for frame in result['frames']:
                        values = frame['bone_visuals']
                        summaries.append((round(frame['pose']['Tint']['position'][0], 3), values['Tint']['color'],
                                          values['Veil']['alpha'], values['Lamp']['glow']))
                    self.assertEqual(summaries, [
                        (-5, [255, 55, 55], 90, 15), (0, [255, 55, 55], 90, 15),
                        (5, [55, 240, 255], 225, 0), (-5, [55, 240, 255], 225, 0),
                        (-4.667, [255, 55, 55], 90, 15)])

    def run_player(self, functions=None, frames=None, animations=None, failures=0, pair=None):
        geometry = {'minecraft:geometry': [{'bones': [
            {'name': 'Root'}, {'name': 'Child', 'parent': 'Root'},
            {'name': 'ysmGlowLamp', 'parent': 'Root'}, {'name': 'ysmglowDim', 'parent': 'Root'}]}]}
        document = {'animations': animations or {'idle': {'loop': True}}, 'ysm_functions': functions or {}}
        plan = {'frames': frames or [{'time': 0}, {'time': .2}], 'geometry': geometry}
        command = '--source-player-animation-file'
        if pair is not None:
            document = {'body': document, 'arm': pair}
            plan.update(body_geometry=geometry, arm_geometry=geometry)
            command = '--paired-animation-file'
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'visual.json'
            path.write_text(json.dumps(document), encoding='utf-8')
            run = subprocess.run([str(EXE), command, str(path), json.dumps(plan)],
                                 capture_output=True, encoding='utf-8', timeout=8)
        self.assertEqual(run.returncode, 0, run.stderr)
        result = json.loads(run.stdout)
        self.assertEqual(result['expression_failures'], failures, result)
        return result['frames']

    def test_named_glow_is_case_sensitive_and_does_not_inherit(self):
        values = self.run_player()[0]['bone_visuals']
        self.assertEqual(values['Root'], WHITE)
        self.assertEqual(values['Child'], WHITE)
        self.assertEqual(values['ysmGlowLamp'], dict(WHITE, glow=15))
        self.assertEqual(values['ysmglowDim'], WHITE)

    def test_three_actions_commit_to_one_exact_bone(self):
        values = self.run_player({'@player_init': "ysm.bone_color('Root',40,90,180);"
            "ysm.bone_transparency('Root',128);ysm.bone_glow('Root',7);"})[-1]['bone_visuals']
        self.assertEqual(values['Root'], {'color': [40, 90, 180], 'alpha': 128, 'glow': 7})
        self.assertEqual(values['Child'], WHITE)

    def test_java_float_rounding_and_clamps(self):
        values = self.run_player({'@player_init': "ysm.bone_color('Root',-999,999,2.49999999);"
            "ysm.bone_transparency('Root',-0.5);ysm.bone_glow('Root',99);"})[-1]['bone_visuals']
        self.assertEqual(values['Root'], {'color': [0, 255, 3], 'alpha': 0, 'glow': 15})

    def test_dynamic_glow_can_override_named_initial_value_and_restore_host_light(self):
        frames = self.run_player({'@player_init': "ysm.bone_glow('ysmGlowLamp',0);",
            '@player_update': "q.life_time>0?ysm.bone_glow('ysmGlowLamp',-5);"})
        self.assertEqual(frames[0]['bone_visuals']['ysmGlowLamp']['glow'], 0)
        self.assertEqual(frames[-1]['bone_visuals']['ysmGlowLamp']['glow'], -1)

    def test_attributes_survive_frames_without_new_writes(self):
        frames = self.run_player({'@player_init': "ysm.bone_color('Root',20,30,40);"},
                                 [{'time': 0}, {'time': .2}, {'time': 2}, {'time': 10}])
        self.assertEqual([f['bone_visuals']['Root']['color'] for f in frames], [[20, 30, 40]] * 4)

    def test_effects_keep_defer_order_and_capture_arguments(self):
        values = self.run_player({'@player_init': "v.n=40;ysm.bone_color('Root',v.n,1,2);v.n=90;ysm.defer(0);",
                                 '@defer': "ysm.bone_transparency('Root',100);"})[-1]['bone_visuals']
        self.assertEqual(values['Root'], {'color': [40, 1, 2], 'alpha': 100, 'glow': -1})

    def test_failure_discards_prepared_visual_effects_for_the_whole_round(self):
        frames = self.run_player({'@player_init': "ysm.bone_color('Root',0,0,0);ysm.defer(0);",
                                 '@defer': 'math.abs(3.5e38);'}, failures=1)
        self.assertEqual(frames[0]['bone_visuals']['Root'], WHITE)
        self.assertEqual(frames[-1]['bone_visuals']['Root'], WHITE)

    def test_readonly_channel_does_not_evaluate_action_arguments(self):
        animation = {'idle': {'loop': True, 'bones': {'Root': {'rotation': [
            "ysm.bone_color('Root',v.side+=1,0,0);return 0;", 0, 0]}}}}
        frame = self.run_player(animations=animation)[-1]
        self.assertNotIn('variable.side', frame['variables'])
        self.assertEqual(frame['bone_visuals']['Root'], WHITE)

    def test_missing_target_does_not_create_an_attribute_or_fail(self):
        values = self.run_player({'@player_init': "ysm.bone_color('missing',1,2,3);"})[-1]['bone_visuals']
        self.assertNotIn('missing', values)
        self.assertEqual(values['Root'], WHITE)

    def test_observation_cannot_modify_or_reinitialize_attributes(self):
        frames = self.run_player({'@player_init': "ysm.bone_transparency('Root',123);"},
            [{'time': 0}, {'time': .2}, {'time': 50, 'observe': True}, {'time': .3}])
        self.assertEqual([f['bone_visuals']['Root']['alpha'] for f in frames], [123] * 4)
        self.assertEqual(frames[2]['logical_bone_visuals'], frames[1]['logical_bone_visuals'])

    def test_body_and_arm_attributes_are_independent(self):
        arm = {'animations': {'parallel0': {'loop': True,
               'timeline': {'0': "ysm.bone_color('Root',0,200,0);"}}}}
        frames = self.run_player({'@player_init': "ysm.bone_color('Root',200,0,0);"},
                                 [{'time': 0, 'first_person': True}, {'time': .2, 'first_person': True}], pair=arm)
        self.assertEqual(frames[-1]['body_bone_visuals']['Root']['color'], [200, 0, 0])
        self.assertEqual(frames[-1]['arm_bone_visuals']['Root']['color'], [0, 200, 0])

    def test_draw_partition_merges_equal_surfaces_and_omits_fully_transparent_bones(self):
        values = [{}, {'color': [255, 0, 0]}, {'alpha': 0}, {}, {'color': [255, 0, 0]}, {'glow': 15}]
        run = subprocess.run([str(EXE), '--bone-visual-plan', json.dumps(values)], capture_output=True,
                             encoding='utf-8', timeout=5)
        self.assertEqual(run.returncode, 0, run.stderr)
        passes = json.loads(run.stdout)
        self.assertEqual([p['bones'] for p in passes], [[0, 3], [1, 4], [5]])
        self.assertEqual(passes[0]['color'], WHITE['color'])
        self.assertEqual(passes[2]['glow'], 15)

    def test_opacity_separates_parent_faces_from_children_and_hand_locators(self):
        builder = library.ModelLibraryTests()
        builder.setUp()
        self.addCleanup(builder.temporary.cleanup)
        folder = builder.source('opacity', folder=True)
        cube = {'origin': [0, 1, 0], 'size': [2, 2, 2], 'uv': [0, 0]}
        for filename, root in [('main.json', 'Root'), ('arm.json', 'RightArm')]:
            geometry = json.loads((folder / filename).read_text())
            geometry['minecraft:geometry'][0]['bones'] = [
                {'name': root, 'pivot': [1, 2, 3], 'rotation': [0, 20, 0],
                 'mirror': True, 'cubes': [cube], 'locators': {'tip': [0, 3, 0]}},
                {'name': 'Child', 'parent': root, 'cubes': [cube]}]
            (folder / filename).write_text(json.dumps(geometry))
        (folder / 'functions').mkdir()
        (folder / 'functions' / '@player_update.molang').write_text("ysm.bone_transparency('Root',0);")
        result = json.loads(subprocess.check_output([str(EXE), '--client-geometry', str(folder)]))
        bones = {b['name']: b for b in result['geometry']['minecraft:geometry'][0]['bones']}
        for mapping, root in [('body_bones', 'Root'), ('arm_bones', 'RightArm')]:
            names = {v: k for k, v in result[mapping].items()}
            owner = names[root]
            leaf, = [part for part, parent in result['opacity_parts'].items() if parent == owner]
            self.assertEqual(bones[leaf]['parent'], owner)
            self.assertEqual(bones[leaf]['pivot'], bones[owner]['pivot'])
            self.assertTrue(bones[leaf]['mirror'])
            self.assertIn('locators', bones[owner])
            self.assertNotIn('cubes', bones[owner])
            self.assertNotIn('rotation', bones[leaf])
            self.assertEqual(bones[names['Child']]['parent'], owner)
            self.assertEqual(result['opacity_parts'][names['Child']], names['Child'])
            if mapping == 'arm_bones':
                self.assertEqual(result['arm_parts'][leaf], 'rightarm')

    def test_real_source_calls_opt_in_and_quoted_names_do_not(self):
        builder = library.ModelLibraryTests()
        builder.setUp()
        self.addCleanup(builder.temporary.cleanup)
        path = builder.source('visual.zip')
        with zipfile.ZipFile(path) as archive:
            files = {n: archive.read(n) for n in archive.namelist()}
        keys = []
        for source, enabled in [("'ysm.bone_color';", False), ("ysm.bone_color('Root',1,2,3);", True)]:
            files['animation.json'] = json.dumps({'animations': {'idle': {'loop': True, 'timeline': {'0': source}}}}).encode()
            with zipfile.ZipFile(path, 'w') as archive:
                for name, value in files.items(): archive.writestr(name, value)
            run = subprocess.run([str(EXE), str(path)], capture_output=True, encoding='utf-8', timeout=5)
            self.assertEqual(run.returncode, 0, run.stderr)
            report = json.loads(run.stdout)
            self.assertEqual(report['individual_bone_materials'], enabled)
            keys.append(report['render_key'])
        self.assertNotEqual(*keys)


if __name__ == '__main__':
    unittest.main()
