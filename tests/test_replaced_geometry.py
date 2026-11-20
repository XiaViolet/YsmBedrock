"""Native replacement geometry must retain authored data without humanoid carriers."""
import json
import unittest
import test_replaced_targets as fixtures


class ReplacedGeometryTests(unittest.TestCase):
    invoke = fixtures.ReplacedTargetTests.invoke
    entry = staticmethod(fixtures.ReplacedTargetTests.entry)

    def build(self, bones, description=None, visual=False, kind='vehicle'):
        geometry = {'format_version': '1.12.0', 'minecraft:geometry': [{
            'description': dict({'identifier': 'geometry.authored', 'texture_width': 2, 'texture_height': 2},
                                **(description or {})), 'bones': bones}]}
        animation = {'animations': {'ground': {'bones': {'Head': {
            'position': ["ysm.bone_transparency('Head',0)", 0, 0]}}}}} if visual else {'animations': {}}
        entity = 'arrow' if kind == 'projectile' else 'boat'
        section = 'projectiles' if kind == 'projectile' else 'vehicles'
        return self.invoke({section: [self.entry(match=[entity])]}, {'kind': kind, 'entity_type': entity},
                           {'target.json': json.dumps(geometry).encode(),
                            'target.animation.json': json.dumps(animation).encode()}, mode='--target-client-geometry')

    def test_projectile_orientation_wraps_all_roots_at_entity_origin_in_y_then_z_order(self):
        source = [{'name': 'Root', 'pivot': [8, 0, 0], 'rotation': [0, 90, 0]},
                  {'name': 'Child', 'parent': 'Root', 'pivot': [8, 2, 0]},
                  {'name': 'Other', 'pivot': [-5, 3, 1]}]
        report = self.build(source, kind='projectile')
        bones = report['geometry']['minecraft:geometry'][0]['bones']
        yaw, pitch = report['projectile_yaw_carrier'], report['projectile_pitch_carrier']
        self.assertEqual(len(bones), 5)
        self.assertEqual(bones[0]['parent'], pitch)
        self.assertEqual(bones[2]['parent'], pitch)
        self.assertEqual(bones[1]['parent'], '__ysm_target_0')
        self.assertEqual(bones[0]['pivot'], [8, 0, 0])
        self.assertEqual(bones[0]['rotation'], [0, 90, 0])
        self.assertEqual(bones[-2], {'name': yaw, 'pivot': [0, 0, 0]})
        self.assertEqual(bones[-1], {'name': pitch, 'parent': yaw, 'pivot': [0, 0, 0]})
        self.assertNotIn(yaw, report['body_bones'])
        self.assertNotIn(pitch, report['body_bones'])

    def test_vehicle_geometry_does_not_receive_projectile_orientation_carriers(self):
        report = self.build([{'name': 'Root'}])
        self.assertEqual(report['projectile_yaw_carrier'], '')
        self.assertEqual(report['projectile_pitch_carrier'], '')
        self.assertEqual(len(report['geometry']['minecraft:geometry'][0]['bones']), 1)

    def test_head_hand_and_arm_names_do_not_add_player_bones_or_locators(self):
        report = self.build([{'name': 'Head'}, {'name': 'RightArm'}, {'name': 'RightHandLocator'}])
        self.assertEqual(set(report['body_bones'].values()), {'Head', 'RightArm', 'RightHandLocator'})
        self.assertEqual(len(report['geometry']['minecraft:geometry'][0]['bones']), 3)
        for key in ['arm_bones', 'arm_parts', 'item_locators', 'first_person_items', 'opacity_parts']:
            self.assertEqual(report[key], {})
        self.assertEqual(report['arm_carriers'], [])
        self.assertEqual(report['head_locator'], '')
        self.assertEqual(report['body_root'], '')

    def test_multiple_roots_keep_authored_origins_rotations_locators_and_cubes(self):
        cube = {'origin': [-4, -3, -2], 'size': [8, 6, 4], 'uv': [0, 0], 'inflate': .1}
        report = self.build([{'name': 'rootA', 'pivot': [1, 2, 3], 'rotation': [20, 30, 40], 'cubes': [cube]},
                             {'name': 'child', 'parent': 'rootA', 'pivot': [1, 3, 3],
                              'locators': {'authored': [1, 4, 3]}}, {'name': 'rootB'}])
        bones = report['geometry']['minecraft:geometry'][0]['bones']
        self.assertEqual(bones[0], {'name': '__ysm_target_0', 'pivot': [1, 2, 3],
                                    'rotation': [20, 30, 40], 'cubes': [cube]})
        self.assertEqual(bones[1]['parent'], '__ysm_target_0')
        self.assertEqual(bones[1]['locators'], {'authored': [1, 4, 3]})
        self.assertNotIn('parent', bones[2])

    def test_native_reserved_source_names_remain_distinct_after_renaming(self):
        report = self.build([{'name': '__ysm_target_1'}, {'name': 'head', 'parent': '__ysm_target_1'}])
        bones = report['geometry']['minecraft:geometry'][0]['bones']
        self.assertEqual(report['body_bones'], {'__ysm_target_0': '__ysm_target_1', '__ysm_target_1': 'head'})
        self.assertEqual(bones[1]['parent'], '__ysm_target_0')

    def test_zero_dimensions_use_default_but_valid_texture_dimensions_are_preserved(self):
        for dimensions, expected in [((0, 0), (64, 64)), ((16, 32), (16, 32))]:
            with self.subTest(dimensions=dimensions):
                report = self.build([{'name': 'Root'}], {'texture_width': dimensions[0], 'texture_height': dimensions[1]})
                description = report['geometry']['minecraft:geometry'][0]['description']
                self.assertEqual((description['texture_width'], description['texture_height']), expected)

    def test_parent_alpha_isolates_own_faces_and_preserves_child_pivots(self):
        cube = {'origin': [0, 0, 0], 'size': [1, 1, 1], 'uv': [0, 0]}
        report = self.build([{'name': 'Head', 'pivot': [0, 7, 0], 'cubes': [cube]},
                             {'name': 'Child', 'parent': 'Head', 'pivot': [0, 8, 0], 'cubes': [cube]}], visual=True)
        bones = report['geometry']['minecraft:geometry'][0]['bones']
        self.assertNotIn('cubes', bones[0])
        self.assertEqual(bones[1]['parent'], '__ysm_target_0')
        self.assertEqual(bones[1]['pivot'], [0, 8, 0])
        self.assertEqual(report['opacity_parts'], {'__ysm_surface_0': '__ysm_target_0', '__ysm_target_1': '__ysm_target_1'})
        self.assertEqual(bones[2], {'name': '__ysm_surface_0', 'parent': '__ysm_target_0', 'pivot': [0, 7, 0], 'cubes': [cube]})


if __name__ == '__main__':
    unittest.main()
