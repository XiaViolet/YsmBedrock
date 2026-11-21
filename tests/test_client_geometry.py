"""Check the body/hand boundary and texture-coordinate conversion."""
import json
import pathlib
import subprocess
import tempfile
import unittest
import zipfile

from test_import import EXE, SAMPLE


class ClientGeometryTests(unittest.TestCase):
    def test_slot_expression_follows_hand_and_preserves_child_animation(self):
        # Column-major, as supplied by the Bedrock renderer. The new grip
        # turns 90 degrees around Z. The item has a two-unit local X offset.
        identity = [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]
        def translated(x):
            return identity[:12] + [x,0,0,1]
        target = [0,1,0,0, -1,0,0,0, 0,0,1,0, 4,5,0,1]
        expected = target[:12] + [4,7,0,1]
        base = dict(binding='q.item_slot_to_bone_name(c.item_slot)', old_parent=translated(10),
                    child=translated(12), target=target)
        cases = [base, dict(base, identity=True), dict(base, same=True),
                 dict(base, old_parent=[0]*16), dict(base, binding="'head'"),
                 dict(base, binding='query.item_slot_to_bone_name( context.item_slot );'),
                 dict(base, binding="q.item_slot_to_bone_name(c.item_slot) + 1"),
                 dict(base, child=[0]*16, old_parent=[0]*16, same=True, vanilla_charge=True,
                      local=dict(position=[2,-1,0], rotation=[0,0,90], scale=[1,2,1]))]
        result = subprocess.check_output([str(EXE), '--hand-slot-bindings', json.dumps(cases)])
        self.assertEqual(json.loads(result),
                         [expected, target, translated(12), translated(12), None, expected, None,
                          [0,1,0,0, -2,0,0,0, 0,0,1,0, 3,7,0,1]])

    def test_slot_root_rotation_matches_recorded_engine_basis(self):
        path = pathlib.Path(__file__).parent / 'fixtures/native_bone_basis_26_60_05.json'
        recorded = json.loads(path.read_text())['cases']
        cases, expected = [], []
        for key, angles in [('rotation_xyz', [30,40,50]), ('rotation_z', [0,0,50])]:
            for charge in [False, True]:
                cases.append(dict(binding='q.item_slot_to_bone_name(c.item_slot)', child=[0]*16,
                                  target=recorded['neutral'], vanilla_charge=charge,
                                  local=dict(rotation=angles, scale=[1]*3, position=[0]*3)))
                expected.append(recorded['neutral' if charge and key == 'rotation_z' else key])
        result = json.loads(subprocess.check_output([str(EXE), '--hand-slot-bindings', json.dumps(cases)]))
        for actual, reference in zip(result, expected):
            for v, ref in zip(actual, reference):
                self.assertAlmostEqual(v, ref, places=6)

    def test_local_hand_replay_accepts_more_than_64_roaming_fields(self):
        script = ''.join(f'v.roaming.f{i}=1;' for i in range(65))
        with tempfile.TemporaryDirectory() as directory:
            pack = self.fixture(directory, [{'name':'RightArm'}],
                                animations={'main': {'idle': {'timeline': {'0': script}}}})
            report = json.loads(subprocess.check_output(
                [str(EXE), '--hand-animations', str(pack), json.dumps({'frames':[{'time':0}]})]))
            self.assertEqual(report['expression_failures'], 0)
            self.assertEqual(len(report['frames']), 1)

    def inspect(self, path=SAMPLE, command='--client-geometry'):
        result = subprocess.run([str(EXE), command, str(path)], capture_output=True, encoding='utf-8')
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_body_and_first_person_bones_have_separate_names_and_parents(self):
        report = self.inspect()
        bones = report['geometry']['minecraft:geometry'][0]['bones']
        names = {bone['name'] for bone in bones}
        surface, = report['one_sided_parts']
        parent = next(b['parent'] for b in bones if b['name'] == surface)
        self.assertEqual(report['body_bones'][parent], 'Skirt_Front_white')
        self.assertEqual(len(bones), 190)
        self.assertEqual(len(names), 190)
        self.assertEqual(len(report['body_bones']), 178)
        self.assertEqual(len(report['arm_bones']), 6)
        self.assertEqual(set(report['arm_carriers']), {'leftarm', 'rightarm'})
        for bone in bones:
            if 'parent' in bone:
                self.assertIn(bone['parent'], names)
        self.assertEqual(sum(len(bone.get('cubes', [])) for bone in bones), 1524)
        self.assertFalse(set(report['body_bones']) & set(report['arm_bones']))

    def test_first_person_items_have_independent_wrist_locators(self):
        report = self.inspect()
        bones = {bone['name']: bone for bone in report['geometry']['minecraft:geometry'][0]['bones']}
        self.assertEqual(report['first_person_items'], {
            '__ysm_fp_rightitem': 'rightitem', '__ysm_fp_leftitem': 'leftitem'})
        for name, item in report['first_person_items'].items():
            self.assertEqual(bones[name]['parent'], item.replace('item', 'arm'))
            self.assertEqual(bones[name]['pivot'], [-6 if item == 'rightitem' else 6, 15, 1])
            self.assertNotIn(name, report['body_bones'])
            self.assertNotIn(name, report['arm_bones'])
            self.assertNotEqual(bones[name]['parent'], bones[item]['parent'])

    def test_first_person_uvs_keep_the_original_normalized_coordinates(self):
        report = self.inspect()
        with zipfile.ZipFile(SAMPLE) as archive:
            original = json.loads(archive.read('Sahngshan_huiliyi/models/arm.json'))['minecraft:geometry'][0]
        source = {bone['name']: bone for bone in original['bones']}
        native = {bone['name']: bone for bone in report['geometry']['minecraft:geometry'][0]['bones']}
        for name, logical in report['arm_bones'].items():
            for before, after in zip(source[logical]['cubes'], native[name]['cubes']):
                for face, uv in before['uv'].items():
                    for key in ['uv', 'uv_size']:
                        self.assertEqual(after['uv'][face][key], [value * 2 for value in uv[key]])
        right = next(native[name] for name, logical in report['arm_bones'].items() if logical == 'RightArm')
        self.assertEqual(right['pivot'], [-5, 22, 0])

    def fixture(self, directory, arm_bones, animations=None, body_bones=None):
        directory = pathlib.Path(directory)
        geometry = lambda bones, size: {'format_version': '1.12.0', 'minecraft:geometry': [
            {'description': {'identifier': 'geometry.test', 'texture_width': size, 'texture_height': size}, 'bones': bones}]}
        files = {'main.json': geometry(body_bones or [{'name': 'root', 'pivot': [0, 0, 0]}], 256),
                 'arm.json': geometry(arm_bones, 128)}
        sets = animations or {'main': {'idle': {}}}
        manifest = {'spec': 2, 'files': {'player': {'model': {'main': 'main.json', 'arm': 'arm.json'},
                     'texture': ['texture.png'], 'animation': {key: key + '.animation.json' for key in sets}}}}
        files['ysm.json'] = manifest
        for key, clips in sets.items():
            files[key + '.animation.json'] = {'animations': clips}
        for name, value in files.items():
            (directory / name).write_text(json.dumps(value), encoding='utf-8')
        with zipfile.ZipFile(SAMPLE) as archive:
            (directory / 'texture.png').write_bytes(archive.read('Sahngshan_huiliyi/textures/default.png'))
        return directory

    def test_player_and_projectile_split_only_closed_cubes_with_signed_extent(self):
        faces = {key: {'uv': [0, 0], 'uv_size': [1, 1]} for key in ('east', 'west', 'north', 'south', 'up', 'down')}
        def cube(size, uv=faces, **settings):
            return {'origin': [1, 2, 3], 'size': size, 'uv': uv, **settings}
        normal = [cube([2, 3, 4]), cube([0, -3, 4]),
                  cube([-2, 3, 4], {'north': faces['north']})]
        signed_cubes = [cube([-2, 3, 4]), cube([-2, -3, 4]), cube([-2, -3, -4]), cube([1, 3, 4], inflate=-1)]
        with tempfile.TemporaryDirectory() as directory:
            bones = [{'name': 'Mixed', 'pivot': [1, 2, 3], 'mirror': True, 'cubes': normal + signed_cubes},
                     {'name': 'Negative', 'parent': 'Mixed', 'cubes': [cube([-2, -3, -4], [0, 0])]}]
            path = self.fixture(directory, [], body_bones=bones)
            descriptor = json.loads((path / 'ysm.json').read_text())
            descriptor['files']['projectiles'] = {'minecraft:trident': {
                'model': 'main.json', 'texture': 'texture.png'}}
            (path / 'ysm.json').write_text(json.dumps(descriptor))
            target = subprocess.check_output([str(EXE), '--target-client-geometry', str(path),
                json.dumps({'kind': 'projectile', 'entity_type': 'minecraft:trident'})], encoding='utf-8')
            for report in (self.inspect(path), json.loads(target)):
                native = {b['name']: b for b in report['geometry']['minecraft:geometry'][0]['bones']}
                names = {v: k for k, v in report['body_bones'].items()}
                self.assertEqual(native[names['Mixed']]['cubes'], normal)
                part, = set(report['one_sided_parts']) - {names['Negative']}
                self.assertIn(names['Negative'], report['one_sided_parts'])
                self.assertEqual(native[part]['cubes'], signed_cubes)
                self.assertTrue(native[part]['mirror'])
                self.assertEqual(native[part]['parent'], names['Mixed'])
                self.assertEqual(native[part]['pivot'], [1, 2, 3])
                self.assertEqual(report['opacity_parts'][part], names['Mixed'])

    def test_first_person_polygon_positions_follow_arm_pivot_and_preserve_uvs(self):
        for normalized in (False, True):
            with self.subTest(normalized=normalized), tempfile.TemporaryDirectory() as directory:
                mesh = {'positions': [[-10, 10, 1], [-6, 10, 1], [-6, 14, 1], [-10, 14, 1]],
                        'normals': [[0, 0, 1]], 'uvs': [[0.25, 0.5]] * 4,
                        'polys': [[[i, 0, i] for i in range(4)]], 'normalized_uvs': normalized}
                path = self.fixture(directory, [{'name': 'RightArm', 'pivot': [-10, 10, 1], 'poly_mesh': mesh}])
                report = self.inspect(path)
                bones = report['geometry']['minecraft:geometry'][0]['bones']
                arm = next(bone for bone in bones if report['arm_bones'].get(bone['name']) == 'RightArm')
                self.assertEqual(arm['pivot'], [-5, 22, 0])
                self.assertEqual(arm['poly_mesh']['positions'], [[-5, 22, 0], [-1, 22, 0], [-1, 26, 0], [-5, 26, 0]])
                self.assertEqual(arm['poly_mesh']['normals'], mesh['normals'])
                self.assertEqual(arm['poly_mesh']['polys'], mesh['polys'])
                self.assertEqual(arm['poly_mesh']['uvs'], mesh['uvs'] if normalized else [[0.5, 1]] * 4)

    def test_held_items_use_source_hand_locators_and_preserve_their_parent_and_pivot(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.fixture(directory, [], body_bones=[
                {'name': 'root', 'pivot': [0, 0, 0]},
                {'name': 'RightHandLocator1', 'parent': 'root', 'pivot': [-4, 16, 1]},
                {'name': 'RightHandLocator', 'parent': 'root', 'pivot': [-3, 15, 2]},
                {'name': 'LeftHandLocator', 'parent': 'root', 'pivot': [3, 15, 2]},
                {'name': 'LeftHandLocators', 'parent': 'root', 'pivot': [0, 0, 0]}])
            report = self.inspect(path)
        self.assertEqual(report['item_locators'], {'rightitem': 'RightHandLocator', 'leftitem': 'LeftHandLocator'})
        native = {bone['name']: bone for bone in report['geometry']['minecraft:geometry'][0]['bones']}
        root = next(name for name, logical in report['body_bones'].items() if logical == 'root')
        self.assertEqual(native['rightitem']['parent'], root)
        self.assertEqual(native['rightitem']['pivot'], [-3, 15, 2])
        self.assertEqual(native['leftitem']['pivot'], [3, 15, 2])
        self.assertEqual(len(report['body_bones']), 6)

    def test_official_hand_copies_require_the_exact_primary_and_accept_gaps_up_to_eight(self):
        def locator(name):
            return {'name': name, 'parent': 'root', 'pivot': [0, 15, 0]}
        with tempfile.TemporaryDirectory() as directory:
            bones = [{'name': 'root'}]
            bones.extend(locator('RightHandLocator' + suffix) for suffix in ('', '1', '2', '4', '8', '9', '17'))
            bones.extend(locator('LeftHandLocator' + suffix) for suffix in ('1', '2', '3'))
            report = self.inspect(self.fixture(directory, [], body_bones=bones))
        self.assertEqual(report['hand_draw_masks'], [1 + 2 + 8 + 128, 0])
        self.assertEqual(report['item_locators'], {'rightitem': 'RightHandLocator'})
        for suffix in ('2', '4', '8'):
            self.assertEqual(report['body_bones']['__ysm_rightitem' + suffix], 'RightHandLocator' + suffix)

    def test_hidden_primary_and_numbered_locators_are_independent_in_the_render_pose(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.fixture(directory, [{'name': 'LeftArm', 'pivot': [5,22,0]}], body_bones=[
                {'name': 'root'}, {'name': 'grip', 'parent': 'root'},
                {'name': 'RightHandLocator', 'parent': 'grip'},
                {'name': 'RightHandLocator3', 'parent': 'root'}])
            for scales, expected in [({}, 5), ({'grip': [0,0,0]}, 4),
                                     ({'RightHandLocator3': [0,0,0]}, 1), ({'root': [0,0,0]}, 0),
                                     ({'grip': [-1,0,1]}, 5)]:
                out = subprocess.check_output([str(EXE), '--hand-masks', str(path), json.dumps(scales)])
                self.assertEqual(json.loads(out), [expected, 0])
            passes = [{'inventory': False, 'scales': {'grip': [0,0,0]}},
                      {'inventory': True, 'scales': {}},
                      {'inventory': True, 'scales': {'root': [0,0,0]}},
                      {'inventory': False, 'scales': {}}]
            samples = subprocess.check_output([str(EXE), '--hand-masks', str(path), json.dumps(passes)])
            self.assertEqual(json.loads(samples), [[4,5], [4,5], [4,0], [5,0]])
            geometry = self.inspect(path)
            bones = {b['name']: b for b in geometry['geometry']['minecraft:geometry'][0]['bones']}
            self.assertEqual(bones['leftitem']['parent'], 'leftarm')
            self.assertEqual(bones['leftitem']['pivot'], bones['__ysm_fp_leftitem']['pivot'])

    def test_numbered_grip_keeps_its_transform_when_its_own_faces_turn_transparent(self):
        clips = {'main': {'idle': {'bones': {'root': {
            'position': ["ysm.bone_transparency('RightHandLocator2',0)", 0, 0]}}}}}
        with tempfile.TemporaryDirectory() as directory:
            path = self.fixture(directory, [], clips, [
                {'name': 'root'}, {'name': 'RightHandLocator', 'parent': 'root'},
                {'name': 'RightHandLocator2', 'parent': 'root',
                 'cubes': [{'origin': [0,0,0], 'size': [1,1,1], 'uv': [0,0]}]}])
            report = self.inspect(path)
        native = {b['name']: b for b in report['geometry']['minecraft:geometry'][0]['bones']}
        grip = '__ysm_rightitem2'
        leaf, = [part for part, owner in report['opacity_parts'].items() if owner == grip]
        self.assertNotEqual(leaf, grip)
        self.assertEqual(native[leaf]['parent'], grip)
        self.assertNotIn('cubes', native[grip])

    def test_native_spear_can_drive_multiple_body_outfits_without_a_first_person_arm(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.fixture(directory, [], body_bones=[
                {'name': 'root', 'pivot': [0, 0, 0]},
                {'name': 'RightArm', 'parent': 'root', 'pivot': [-5, 22, 0]},
                {'name': 'RightArm3', 'parent': 'root', 'pivot': [-6, 21, 0]},
                {'name': 'RightArm4', 'parent': 'RightArm', 'pivot': [-5, 22, 0]},
                {'name': 'RightArmClothes', 'parent': 'RightArm', 'pivot': [-5, 22, 0]},
                {'name': 'LeftArm', 'parent': 'root', 'pivot': [5, 22, 0]}])
            report = self.inspect(path)
            args = [str(EXE), '--animation-sets', str(path)]
            body = json.loads(subprocess.check_output(args, encoding='utf-8', timeout=5))['body']
        self.assertEqual({report['body_bones'][name] for name in report['melee_spear_arms']},
                         {'RightArm', 'RightArm3', 'RightArm4'})
        bones = body['animations']['hold_mainhand:lance']['bones']
        self.assertEqual(bones['RightArm3'], bones['RightArm'])
        self.assertNotIn('RightArm4', bones)
        source = next(bone for bone in report['geometry']['minecraft:geometry'][0]['bones'] if bone['name'] == 'rightarm')
        self.assertNotIn('cubes', source)
        self.assertNotIn('rightarm', report['arm_carriers'])

    def test_default_weapon_tracks_follow_independent_arm_but_leave_authored_clips_and_extra_grips_alone(self):
        bones = [{'name':'root'}, {'name':'RightArm','parent':'root'}, {'name':'RightArm3','parent':'root'},
                 {'name':'RightArm4','parent':'RightArm'}, {'name':'RightHandLocator','parent':'RightArm'},
                 {'name':'RightHandLocator3','parent':'RightArm3'}, {'name':'RightHandLocator2','parent':'RightArm'}]
        with tempfile.TemporaryDirectory() as directory:
            path = self.fixture(directory, [], body_bones=bones)
            def clips():
                return self.inspect(path, '--animation-sets')['body']['animations']
            merged = clips()
            for action in ('hold_mainhand$minecraft:mace', 'swing$minecraft:mace',
                           'hold_mainhand:lance', 'swing:lance', 'use_mainhand:lance'):
                tracks = merged[action]['bones']
                self.assertEqual(tracks['RightArm'], tracks['RightArm3'])
                self.assertEqual(tracks['RightHandLocator'], tracks['RightHandLocator3'])
                self.assertNotIn('RightArm4', tracks)
                self.assertNotIn('RightHandLocator2', tracks)
            custom = {'bones': {'RightArm': {'rotation': [1,2,3]}}}
            self.fixture(directory, [], {'main': {'swing$minecraft:mace': custom}}, bones)
            self.assertEqual(clips()['swing$minecraft:mace']['bones'], custom['bones'])

    def test_zero_texture_dimensions_use_ysm_default_without_division_by_zero(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.fixture(directory, [{'name': 'RightArm', 'cubes': [
                {'origin': [0, 0, 0], 'size': [4, 12, 4], 'uv': [8, 16]}]}])
            for name in ['main.json', 'arm.json']:
                file = path / name
                document = json.loads(file.read_text(encoding='utf-8'))
                document['minecraft:geometry'][0]['description'].update(texture_width=0, texture_height=0)
                file.write_text(json.dumps(document), encoding='utf-8')
            report = self.inspect(path)
        geometry = report['geometry']['minecraft:geometry'][0]
        self.assertEqual(geometry['description']['texture_width'], 64)
        self.assertEqual(geometry['description']['texture_height'], 64)
        cube = next(bone for bone in geometry['bones'] if bone['name'] in report['arm_bones'])['cubes'][0]
        self.assertEqual(cube['uv']['north']['uv'], [12, 20])

    def test_native_head_keeps_authored_parent_pivot_rotation_and_children(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.fixture(directory, [], body_bones=[
                {'name': 'Root', 'pivot': [0, 0, 0]},
                {'name': 'Head1', 'parent': 'Root', 'pivot': [1, 20, 3]},
                {'name': 'Head', 'parent': 'Root', 'pivot': [2, 24, 4], 'rotation': [5, 6, 7]},
                {'name': 'Hair', 'parent': 'Head', 'pivot': [2, 25, 4]},
                {'name': 'RightHandLocator', 'parent': 'Head1', 'pivot': [3, 15, 2]}])
            report = self.inspect(path)
        native = {bone['name']: bone for bone in report['geometry']['minecraft:geometry'][0]['bones']}
        self.assertEqual(report['head_locator'], 'Head')
        source_head = next(name for name, logical in report['body_bones'].items() if logical == 'Head')
        root = next(name for name, logical in report['body_bones'].items() if logical == 'Root')
        self.assertEqual(native[source_head]['parent'], root)
        self.assertEqual(native['head']['parent'], source_head)
        self.assertEqual(native['head']['pivot'], [2, 24, 4])
        self.assertEqual(native[source_head]['rotation'], [5, 6, 7])
        self.assertNotIn('rotation', native['head'])
        self.assertNotIn('cubes', native['head'])
        self.assertNotIn('head', report['body_bones'])
        hair = next(name for name, logical in report['body_bones'].items() if logical == 'Hair')
        self.assertEqual(native[hair]['parent'], source_head)
        self.assertNotEqual(native['rightitem']['parent'], source_head)
        self.assertEqual(report['item_locators'], {'rightitem': 'RightHandLocator'})

    def test_native_head_accepts_numeric_suffix_but_not_other_case_or_words(self):
        for names, expected in [(['head', 'Heads', 'Head2'], 'Head2'), (['head', 'Heads'], None)]:
            with self.subTest(names=names), tempfile.TemporaryDirectory() as directory:
                path = self.fixture(directory, [], body_bones=[{'name': 'Root', 'pivot': [0, 0, 0]}] + [
                    {'name': name, 'parent': 'Root', 'pivot': [0, 20, 0]} for name in names])
                report = self.inspect(path)
            self.assertEqual(report['head_locator'], expected or '')
            native = {bone['name']: bone for bone in report['geometry']['minecraft:geometry'][0]['bones']}
            parent = next(name for name, logical in report['body_bones'].items() if logical == expected) if expected else report['body_root']
            self.assertEqual(native['head']['parent'], parent)
            self.assertNotIn('cubes', native['head'])
            self.assertNotIn('head', report['body_bones'])

    def test_negative_geometry_texture_dimension_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.fixture(directory, [])
            file = path / 'arm.json'
            document = json.loads(file.read_text(encoding='utf-8'))
            document['minecraft:geometry'][0]['description']['texture_height'] = -1
            file.write_text(json.dumps(document), encoding='utf-8')
            result = subprocess.run([str(EXE), str(path)], capture_output=True, encoding='utf-8')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Invalid geometry texture dimensions', result.stderr)

    def test_first_person_part_roots_are_exact_case_sensitive_names(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.fixture(directory, [
                {'name': 'LeftArm1', 'pivot': [5, 22, 0]},
                {'name': 'child', 'parent': 'LeftArm1', 'pivot': [5, 18, 0]},
                {'name': 'LeftArms', 'pivot': [0, 0, 0]},
                {'name': 'leftarm', 'pivot': [0, 0, 0]}])
            report = self.inspect(path)
        self.assertEqual(report['arm_bones'], {})
        self.assertEqual(report['arm_carriers'], [])

    def test_box_uvs_expand_before_rescaling_both_axes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.fixture(directory, [{'name': 'RightArm', 'pivot': [-5, 22, 0],
                'cubes': [{'origin': [-8, 10, -2], 'size': [4, 12, 4], 'uv': [8, 16]}]}])
            report = self.inspect(path)
        alias = next(iter(report['arm_bones']))
        cube = next(bone for bone in report['geometry']['minecraft:geometry'][0]['bones'] if bone['name'] == alias)['cubes'][0]
        self.assertEqual(cube['uv']['north'], {'uv': [24, 40], 'uv_size': [8, 24]})
        self.assertEqual(cube['uv']['down'], {'uv': [32, 40], 'uv_size': [8, -8]})

    def test_exact_arm_root_keeps_its_children_and_ignores_outside_parent_transform(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.fixture(directory, [
                {'name': 'outside', 'pivot': [50, 50, 50], 'rotation': [20, 30, 40]},
                {'name': 'LeftArm', 'parent': 'outside', 'pivot': [5, 22, 0], 'rotation': [1, 2, 3]},
                {'name': 'LeftArm1', 'parent': 'LeftArm', 'pivot': [5, 18, 0]},
                {'name': 'Background1', 'pivot': [0, 0, 0]}])
            report = self.inspect(path)
        self.assertEqual(set(report['arm_bones'].values()), {'LeftArm', 'LeftArm1'})
        native = {bone['name']: bone for bone in report['geometry']['minecraft:geometry'][0]['bones']}
        left = next(name for name, logical in report['arm_bones'].items() if logical == 'LeftArm')
        self.assertEqual(native[left]['parent'], 'leftarm')
        self.assertEqual(native[left]['rotation'], [1, 2, 3])
        self.assertEqual(native[left]['pivot'], [5, 22, 0])

    def test_background_is_an_independent_part_with_declared_geometry(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.fixture(directory, [
                {'name': 'RightArm', 'pivot': [-5, 22, 0]},
                {'name': 'Background', 'pivot': [1, 2, 3], 'rotation': [7, 8, 9]},
                {'name': 'panel', 'parent': 'Background', 'pivot': [1, 1, 1]}])
            report = self.inspect(path)
        background = {name: logical for name, logical in report['arm_bones'].items()
                      if report['arm_parts'][name] == '__ysm_fp_background'}
        self.assertEqual(set(background.values()), {'Background', 'panel'})
        native = {bone['name']: bone for bone in report['geometry']['minecraft:geometry'][0]['bones']}
        root = next(name for name, logical in background.items() if logical == 'Background')
        self.assertEqual(native[root]['parent'], '__ysm_fp_background')
        self.assertEqual(native[root]['pivot'], [1, 2, 3])
        self.assertEqual(native[root]['rotation'], [7, 8, 9])

    def test_arm_file_is_body_animation_and_fp_arm_is_separate(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.fixture(directory, [], {
                'main': {'idle': {}}, 'arm': {'hold_mainhand:sword': {}},
                'fp_arm': {'parallel0': {}}, 'extra': {'extra0': {}}})
            report = self.inspect(path, '--animation-sets')
        for target, authored in [('body', {'idle', 'hold_mainhand:sword', 'extra0'}),
                                 ('first_person', {'parallel0'})]:
            document = report[target]
            self.assertEqual(set(document['animations']) - set(document['ysm_fallback_animations']), authored)
            for name in authored:
                self.assertEqual(document['animations'][name], {})


if __name__ == '__main__':
    unittest.main()
