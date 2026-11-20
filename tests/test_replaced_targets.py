"""Source-package replacement resources, matching and isolated defaults."""
import json
import hashlib
import pathlib
import subprocess
import tempfile
import unittest
import zipfile

from test_import import EXE, SAMPLE
from test_model_textures import png


class ReplacedTargetTests(unittest.TestCase):
    @staticmethod
    def entry(**options):
        return dict({'match': ['minecraft:boat'], 'model': 'target.json',
                     'animation': 'target.animation.json', 'texture': 'target.png'}, **options)

    def invoke(self, sections=None, inputs=None, files=None, folder=False, error=None, mode='--target-animations'):
        geometry = {'minecraft:geometry': [{'description': {'texture_width': 2, 'texture_height': 2},
                                             'bones': [{'name': 'Root'}]}]}
        descriptor = {'spec': 2, 'files': {'player': {'model': {'main': 'main.json', 'arm': 'main.json'},
                      'animation': {'main': 'player.animation.json'}, 'texture': ['player.png']}}}
        descriptor['files'].update(sections or {})
        data = {'ysm.json': json.dumps(descriptor).encode(), 'main.json': json.dumps(geometry).encode(),
                'target.json': json.dumps(geometry).encode(), 'player.png': png(), 'target.png': png(rgba=(1, 2, 3, 255)),
                'player.animation.json': b'{"animations":{"idle":{"loop":true}}}',
                'target.animation.json': b'{"animations":{"ground":{"loop":true,"bones":{"Root":{"position":[9,0,0]}}}}}'}
        data.update(files or {})
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            if folder:
                for name, content in data.items():
                    target = path / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(content)
            else:
                path /= 'targets.zip'
                with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as archive:
                    for name, content in data.items():
                        archive.writestr('nested/' + name, content)
            args = [str(EXE), str(path)] if inputs is None else [str(EXE), mode, str(path), json.dumps(inputs)]
            run = subprocess.run(args, capture_output=True, encoding='utf-8', timeout=10)
        if error:
            self.assertNotEqual(run.returncode, 0, run.stdout)
            self.assertIn(error, run.stderr)
            return None
        self.assertEqual(run.returncode, 0, run.stderr)
        return json.loads(run.stdout)

    def test_legacy_arrow_in_user_sample_is_loaded_without_changing_player_counts(self):
        run = subprocess.run([str(EXE), str(SAMPLE)], capture_output=True, encoding='utf-8', timeout=10)
        self.assertEqual(run.returncode, 0, run.stderr)
        report = json.loads(run.stdout)
        self.assertEqual(report['animation_clips'], 242)
        self.assertEqual(len(report['replaced_targets']), 1)
        self.assertEqual(report['replaced_targets'][0]['kind'], 'projectile')
        self.assertEqual(report['replaced_targets'][0]['matches'], ['minecraft:arrow'])

    def test_object_form_adds_key_to_existing_match_list_and_preserves_order(self):
        report = self.invoke({'vehicles': {'minecraft:boat': self.entry(match=['minecraft:pig']),
                                           'minecraft:minecart': self.entry(match=None)}})
        targets = report['replaced_targets']
        self.assertEqual([t['id'] for t in targets], ['vehicle/0000', 'vehicle/0001'])
        self.assertEqual(targets[0]['matches'], ['minecraft:pig', 'minecraft:boat'])
        self.assertEqual(targets[1]['matches'], ['minecraft:minecart'])

    def test_first_matching_target_wins_and_kind_is_not_crossed(self):
        sections = {'vehicles': [self.entry(), self.entry()],
                    'projectiles': [self.entry(model='target2.json')]}
        geometry = {'minecraft:geometry': [{'bones': [{'name': 'ProjectileRoot'}]}]}
        report = self.invoke(sections, {'kind': 'vehicle', 'entity_type': 'boat'},
                             {'target2.json': json.dumps(geometry).encode()})
        self.assertEqual(report['target']['id'], 'vehicle/0000')
        self.assertEqual(report['target']['bones'], 1)

    def test_legacy_arrow_in_vehicle_map_uses_projectile_stages(self):
        entry = self.entry(match=[])
        animation = json.dumps({'animations': {'air': {'bones': {'Root': {'position': [9, 0, 0]}}}}}).encode()
        report = self.invoke({'vehicles': {'arrow': entry}},
                             {'kind': 'projectile', 'entity_type': 'minecraft:arrow',
                              'frames': [{'time': 0}, {'time': .15}]},
                             {'target.animation.json': animation})
        self.assertEqual(report['target']['kind'], 'projectile')
        self.assertEqual(report['frames'][-1]['pose']['Root']['position'], [9, 0, 0])

    def test_native_projectile_names_match_java_pack_declarations(self):
        cases = [('thrown_trident', 'trident'), ('fishing_hook', 'fishing_bobber'),
                 ('fireworks_rocket', 'firework_rocket'), ('xp_bottle', 'experience_bottle'),
                 ('wind_charge_projectile', 'wind_charge'), ('breeze_wind_charge_projectile', 'breeze_wind_charge')]
        for native, java in cases:
            with self.subTest(entity=native):
                report = self.invoke({'projectiles': [self.entry(match=[f'minecraft:{java}'])]},
                                     {'kind': 'projectile', 'entity_type': native})
                self.assertEqual(report['target']['id'], 'projectile/0000')

    def test_generic_bedrock_boat_distinguishes_the_authored_wood_variant(self):
        for native, variant, specific in [('boat', 8, 'cherry_boat'), ('chest_boat', 7, 'bamboo_chest_raft')]:
            with self.subTest(entity=native):
                sections = {'vehicles': [self.entry(match=[specific]), self.entry(match=[native])]}
                report = self.invoke(sections, {'kind': 'vehicle', 'entity_type': native, 'boat_variant': variant})
                self.assertEqual(report['target']['id'], 'vehicle/0000')
                report = self.invoke(sections, {'kind': 'vehicle', 'entity_type': native, 'boat_variant': 99})
                self.assertEqual(report['target']['id'], 'vehicle/0001')

    def test_entity_tag_and_explicit_host_tag_matching(self):
        report = self.invoke({'vehicles': [self.entry(match=['#minecraft:dismounts_underwater'])]},
                             {'kind': 'vehicle', 'entity_type': 'minecraft:pig'})
        self.assertEqual(report['target']['id'], 'vehicle/0000')
        report = self.invoke({'vehicles': [self.entry(match=['#custom:rideable'])]},
                             {'kind': 'vehicle', 'entity_type': 'custom:plane', 'entity_tags': ['custom:rideable']})
        self.assertEqual(report['target']['id'], 'vehicle/0000')

    def test_ids_are_case_sensitive_and_wildcards_are_not_invented(self):
        for match in ['Minecraft:boat', '*', '#minecraft:missing']:
            with self.subTest(match=match):
                self.invoke({'vehicles': [self.entry(match=[match])]},
                            {'kind': 'vehicle', 'entity_type': 'minecraft:boat'}, error='No matching')

    def test_null_unmatched_entries_do_not_load_resources_and_keep_declared_indices(self):
        report = self.invoke({'projectiles': [None, {'match': [], 'model': '../must-not-open'}],
                              'arrow': self.entry(match=['minecraft:arrow']), 'vehicles': None})
        self.assertEqual([t['id'] for t in report['replaced_targets']], ['projectile/0002'])

    def test_legacy_arrow_only_defaults_match_when_the_field_is_absent(self):
        entry = self.entry()
        del entry['match']
        report = self.invoke({'arrow': entry})
        self.assertEqual(report['replaced_targets'][0]['matches'], ['minecraft:arrow'])
        self.assertTrue(any('Legacy arrow' in warning for warning in report['warnings']))
        self.assertEqual(self.invoke({'arrow': self.entry(match=[])})['replaced_targets'], [])

    def test_target_texture_channels_are_separate_from_player_variant(self):
        report = self.invoke({'vehicles': [self.entry(texture={'uv': 'target.png', 'normal': 'normal.png', 'specular': 'spec.png'})]},
                             files={'normal.png': png(), 'spec.png': png()})
        self.assertEqual(report['texture_count'], 1)
        texture = report['replaced_targets'][0]['texture']
        self.assertEqual(texture['name'], 'default')
        self.assertEqual(texture['normal'], 'normal.png')
        self.assertEqual(texture['specular'], 'spec.png')

    def test_target_controller_and_geometry_feed_the_correct_runtime(self):
        controller = {'animation_controllers': {'vehicle.main': {'states': {'default': {'animations': ['ground']}}}}}
        report = self.invoke({'vehicles': [self.entry(controller='controller.json')]},
                             {'kind': 'vehicle', 'entity_type': 'minecraft:boat', 'frames': [{'time': 0}, {'time': .2}]},
                             {'controller.json': json.dumps(controller).encode()})
        self.assertEqual(report['expression_failures'], 0, report)
        self.assertEqual(report['frames'][-1]['pose']['Root']['position'][0], 9)
        self.assertEqual(report['target']['animation_controllers'], 1)

    def test_defaults_are_partitioned_by_kind_and_exact_sorted_match_domain(self):
        report = self.invoke({'vehicles': [self.entry(animation=None)]},
                             {'kind': 'vehicle', 'entity_type': 'minecraft:boat', 'include_document': True})
        clips = report['document']['animations']
        self.assertIn('forward', clips)
        self.assertNotIn('walk', clips)
        self.assertTrue(report['document']['ysm_default_domain_available'])
        report = self.invoke({'vehicles': [self.entry(match=['minecraft:pig'], animation=None)]},
                             {'kind': 'vehicle', 'entity_type': 'minecraft:pig', 'include_document': True})
        self.assertEqual(report['document']['animations'], {})
        self.assertFalse(report['document']['ysm_default_domain_available'])

    def test_default_arrow_selects_bow_crossbow_and_missing_source(self):
        report = self.invoke({'projectiles': [self.entry(match=['minecraft:arrow'], animation=None)]},
                             {'kind': 'projectile', 'entity_type': 'arrow',
                              'frames': [{'time': t, 'entity': {'shoot_item_id': item}}
                                         for t, item in enumerate(['minecraft:bow', 'minecraft:crossbow', ''])]})
        poses = [frame['pose'] for frame in report['frames']]
        self.assertEqual([p['bow']['scale'] for p in poses], [[1, 1, 1], [0, 0, 0], [1, 1, 1]])
        self.assertEqual([p['crossbow']['scale'] for p in poses], [[0, 0, 0], [1, 1, 1], [0, 0, 0]])
        self.assertEqual(report['evaluation_diagnostics'], [])

    def test_default_boat_tag_matches_bedrock_boat_and_java_wood_variants(self):
        sections = {'vehicles': [self.entry(match=['#minecraft:boat'], animation=None)]}
        for item in ('minecraft:boat', 'minecraft:pale_oak_boat', 'minecraft:bamboo_raft'):
            with self.subTest(item=item):
                report = self.invoke(sections, {'kind': 'vehicle', 'entity_type': item, 'include_document': True})
                self.assertTrue(report['document']['ysm_default_domain_available'])
                self.assertEqual(len(report['document']['animations']), 23)
        self.invoke(sections, {'kind': 'vehicle', 'entity_type': 'minecraft:chest_boat'}, error='No matching replaced model target')

    def test_explicit_empty_clip_overrides_default_and_player_clips_do_not_leak(self):
        report = self.invoke({'projectiles': [self.entry(match=['minecraft:arrow'])]},
                             {'kind': 'projectile', 'entity_type': 'arrow', 'include_document': True},
                             {'target.animation.json': b'{"animations":{"fly":{}}}'})
        self.assertEqual(report['document']['animations']['fly'], {})
        self.assertNotIn('walk', report['document']['animations'])

    def test_default_domain_preserves_exact_match_set_with_order_independent_hash(self):
        for matches in [['minecraft:pig', 'minecraft:boat'], ['minecraft:boat', 'minecraft:pig'], ['', 'minecraft:boat']]:
            report = self.invoke({'vehicles': [self.entry(match=matches)]},
                                 {'kind': 'vehicle', 'entity_type': 'boat', 'include_document': True})
            digest = hashlib.sha256('\0'.join(sorted(matches)).encode()).hexdigest()[:16]
            self.assertEqual(report['document']['ysm_default_animation_domain'], 'vehicle/' + digest + '/main')
            self.assertFalse(report['document']['ysm_default_domain_available'])

    def test_target_bone_material_flags_do_not_opt_the_player_into_extra_surfaces(self):
        geometry = {'minecraft:geometry': [{'bones': [{'name': 'ysmGlowPanel'}]}]}
        report = self.invoke({'vehicles': [self.entry()]}, files={'target.json': json.dumps(geometry).encode()})
        self.assertTrue(report['replaced_targets'][0]['individual_bone_materials'])
        self.assertFalse(report['individual_bone_materials'])

    def test_optional_animation_and_controller_and_folder_zip_identity(self):
        sections = {'vehicles': [self.entry(animation='', controller=None)]}
        archive = self.invoke(sections)
        directory = self.invoke(sections, folder=True)
        self.assertEqual(archive['local_content_key'], directory['local_content_key'])
        self.assertEqual(archive['replaced_targets'][0]['animation_clips'], 0)

    def test_target_asset_edits_change_source_identity_without_rebaking_player(self):
        sections = {'vehicles': [self.entry()]}
        a = self.invoke(sections)
        b = self.invoke(sections, files={'target.animation.json': b'{"animations":{"different":{}}}'})
        self.assertNotEqual(a['local_content_key'], b['local_content_key'])
        self.assertEqual(a['render_key'], b['render_key'])

    def test_target_paths_and_malformed_sections_are_rejected(self):
        self.invoke({'vehicles': [self.entry(model='../outside.json')]}, error='Resource path escapes')
        for entries in [42, 'bad', [self.entry(match='minecraft:boat')], [self.entry(animation=7)], [self.entry(texture=None)]]:
            with self.subTest(entries=entries):
                self.assert_rejected(entries)

    def assert_rejected(self, entries):
        # Match stable error context while retaining the actual structural error.
        self.invoke({'vehicles': entries}, error='Invalid' if entries != [self.entry(texture=None)] else 'Expected string')

    def test_target_count_is_bounded(self):
        self.invoke({'vehicles': [None] * 513}, error='Too many replaced')


if __name__ == '__main__':
    unittest.main()
