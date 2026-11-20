import json
import pathlib
import subprocess
import unittest

import test_model_functions
from test_import import EXE


class ModelParticleTests(unittest.TestCase):
    player = test_model_functions.ModelFunctionTests.player

    def run_particle(self, code, entity=None, frames=None, failures=0):
        frames = frames or [{'time': 0, 'entity': entity or {}}]
        report = self.player({'@player_init': code}, frames, failures=failures)
        return report, report['frames'][0]['particle_cues']

    def test_defaults_and_namespace(self):
        _, cues = self.run_particle("ysm.particle('note');", {'position': [10, 20, 30]})
        self.assertEqual(cues[0]['name'], 'minecraft:note')
        self.assertEqual(cues[0]['spawns'], [{'position': [10, 20, 30], 'velocity': [0, 0, 0], 'lifetime': 20}])

    def test_relative_single_uses_body_yaw_and_does_not_rotate_velocity(self):
        _, cues = self.run_particle("ysm.particle('minecraft:note',1,2,3,2,3,4,.5);",
                                   {'position': [10, 20, 30], 'body_yaw': 90, 'yaw': 180})
        spawn = cues[0]['spawns'][0]
        for actual, expected in zip(spawn['position'], [7, 22, 31]): self.assertAlmostEqual(actual, expected)
        self.assertEqual(spawn['velocity'], [1, 1.5, 2])

    def test_count_one_uses_entity_yaw(self):
        _, cues = self.run_particle("ysm.particle('minecraft:heart',1,2,3,0,0,0,0,1);",
                                   {'position': [10, 20, 30], 'body_yaw': 90, 'yaw': 180})
        for actual, expected in zip(cues[0]['spawns'][0]['position'], [9, 22, 27]): self.assertAlmostEqual(actual, expected)

    def test_nonplayer_single_uses_entity_yaw(self):
        _, cues = self.run_particle("ysm.particle('note',1,2,3);",
                                   {'position': [10, 20, 30], 'body_yaw': 90, 'yaw': 180, 'is_player': False})
        for actual, expected in zip(cues[0]['spawns'][0]['position'], [9, 22, 27]): self.assertAlmostEqual(actual, expected)

    def test_absolute_is_still_relative_to_entity_origin(self):
        _, cues = self.run_particle("ysm.abs_particle('note',1,2,3);",
                                   {'position': [10, 20, 30], 'body_yaw': 90, 'yaw': 180})
        self.assertEqual(cues[0]['spawns'][0]['position'], [11, 22, 33])

    def test_negative_count_is_single_lifetime_truncates_and_clamps(self):
        for count, lifetime, expected in [(-2, -9, 1), (0, 2.9, 2), (0, 0, 1)]:
            with self.subTest(count=count, lifetime=lifetime):
                _, cues = self.run_particle(f"ysm.particle('note',0,0,0,0,0,0,0,{count},{lifetime});")
                self.assertEqual(len(cues[0]['spawns']), 1)
                self.assertEqual(cues[0]['lifetime'], expected)

    def test_multiple_particles_keep_count_and_zero_spread_exact(self):
        _, cues = self.run_particle("ysm.abs_particle('smoke',1,2,3,0,0,0,0,7,30);",
                                   {'position': [10, 20, 30]})
        self.assertEqual(len(cues[0]['spawns']), 7)
        self.assertTrue(all(s == {'position': [11, 22, 33], 'velocity': [0, 0, 0], 'lifetime': 30}
                            for s in cues[0]['spawns']))

    def test_gaussian_spread_and_speed_are_independent(self):
        _, cues = self.run_particle("ysm.abs_particle('note',0,0,0,2,0,3,.2,12);")
        spawns = cues[0]['spawns']
        self.assertEqual(len({tuple(s['position']) for s in spawns}), 12)
        self.assertTrue(all(s['position'][1] == 0 for s in spawns))
        self.assertTrue(any(s['velocity'][1] != 0 for s in spawns))

    def test_payload_is_captured_before_later_variable_changes(self):
        _, cues = self.run_particle("v.x=2;ysm.particle('note',v.x);v.x=8;")
        self.assertEqual(cues[0]['offset'][0], 2)

    def test_burst_consumes_shared_random_only_at_successful_commit(self):
        def sequence(particle, failure=False):
            return self.player({'@player_update':
                "v.r=math.random(0,1);v.once?{}:{v.once=1;" + particle + "};"},
                [{'time': 0}, {'time': .1}], failures=int(failure))
        baseline = sequence('')
        single = sequence("ysm.particle('note');")
        burst = sequence("ysm.particle('note',0,0,0,0,0,0,0,3);")
        rolled_back = sequence("ysm.particle('note',0,0,0,0,0,0,0,3);math.abs(3.5e38);", True)
        value = lambda r, frame: r['frames'][frame]['logical_variables']['variable.r']
        self.assertEqual(value(burst, 0), value(baseline, 0))
        self.assertEqual(value(single, 1), value(baseline, 1))
        self.assertNotEqual(value(burst, 1), value(baseline, 1))
        self.assertEqual(value(rolled_back, 1), value(baseline, 1))

    def test_round_failure_discards_all_prepared_particles(self):
        _, cues = self.run_particle("ysm.particle('note');math.abs(3.5e38);", failures=1)
        self.assertEqual(cues, [])

    def test_observation_skips_actions_and_argument_side_effects(self):
        report = self.player({'@player_update': "ysm.particle('note',v.calls+=1);"},
                             [{'time': 0}, {'time': .1, 'observe': True}])
        self.assertEqual(len(report['frames'][0]['particle_cues']), 1)
        self.assertEqual(report['frames'][1]['particle_cues'], [])
        self.assertEqual(report['frames'][1]['logical_variables']['variable.calls'], 1)

    def test_extra_numeric_arguments_are_evaluated_and_validated(self):
        _, cues = self.run_particle("ysm.particle('note',0,0,0,0,0,0,0,0,20,1,2,3);")
        self.assertEqual(len(cues), 1)
        _, invalid = self.run_particle("ysm.particle('note',0,0,0,0,0,0,0,0,20,3.5e38);", failures=1)
        self.assertEqual(invalid, [])

    def test_blank_names_noop_but_numeric_validation_still_applies(self):
        _, cues = self.run_particle("ysm.particle(' ');")
        self.assertEqual(cues, [])
        _, cues = self.run_particle("ysm.particle(' ',3.5e38);", failures=1)
        self.assertEqual(cues, [])

    def test_unknown_effect_does_not_silently_become_smoke(self):
        report, cues = self.run_particle("ysm.particle('other:unknown');", failures=1)
        self.assertEqual(cues, [])
        self.assertTrue(any('Unsupported particle effect' in s for s in report['evaluation_diagnostics']))

    def test_entire_java_1_21_1_client_registry_in_one_script(self):
        path = pathlib.Path(__file__).with_name('fixtures') / 'java_particle_registry_1_21_1.json'
        registry = json.loads(path.read_text())['particles']
        options = {'block': '{block_state:"minecraft:stone"}', 'block_marker': '{block_state:"minecraft:glass"}',
                   'dust_pillar': '{block_state:"minecraft:dirt"}', 'falling_dust': '{block_state:"minecraft:sand"}',
                   'item': '{item:"minecraft:apple"}', 'dust': '{color:[1,0,0],scale:1}',
                   'dust_color_transition': '{from_color:[1,0,0],to_color:[0,0,1],scale:1}',
                   'entity_effect': '{color:-65536}', 'sculk_charge': '{roll:1.2}', 'shriek': '{delay:1}',
                   'vibration': '{destination:{type:"minecraft:block",pos:[1,2,3]},arrival_in_ticks:5}'}
        self.assertEqual({name for name, typed in registry.items() if typed}, set(options))
        selectors = [name + options.get(name, '') for name in registry]
        _, cues = self.run_particle(''.join(f"ysm.particle('{selector}');" for selector in selectors))
        self.assertEqual([c['name'] for c in cues], ['minecraft:' + s for s in selectors])
        by_name = dict(zip(registry, cues))
        self.assertEqual(by_name['item_cobweb']['resource'], 'minecraft:web')
        for name in ('trial_spawner_detection', 'trial_spawner_detection_ominous'):
            self.assertEqual(by_name[name]['emitter'], 'minecraft:' + name)

    def test_legacy_simple_selector_leaves_unused_numbers_unconsumed(self):
        _, cues = self.run_particle("ysm.particle('minecraft:cherry_leaves 0 0.7 0.7 1',0,0,0,0,0,0,.1,.1,100);")
        self.assertEqual([c['native_type'] for c in cues], ['CherryLeaves'])
        self.assertEqual(cues[0]['spawns'][0]['lifetime'], 100)

    def test_every_pinned_registration_is_admitted_in_one_script(self):
        fixture = pathlib.Path(__file__).with_name('fixtures') / 'java_particle_registry_1_20_1.json'
        registry = json.loads(fixture.read_text(encoding='utf-8'))['particles']
        coverage = json.loads(subprocess.check_output([str(EXE), '--particle-coverage'], encoding='utf-8', timeout=5))
        self.assertEqual({row['id']: row['parameterized'] for row in coverage}, registry)
        options = {
            'block': ' stone', 'block_marker': ' glass', 'falling_dust': ' oak_log[axis=x]',
            'dust': ' 0.1 0.2 1 1', 'dust_color_transition': ' 1 0 0 1 0 0 1',
            'sculk_charge': ' 1.2', 'item': ' apple', 'vibration': ' 1.5 2 3 15', 'shriek': ' 5',
        }
        _, cues = self.run_particle(''.join(
            f"ysm.particle('{name}{options.get(name, '')}');" for name in registry))
        self.assertEqual([c['name'].split()[0] for c in cues], [f'minecraft:{name}' for name in registry])
        self.assertEqual([c['coverage'] for c in cues], [row['coverage'] for row in coverage])

    def test_recovered_particles_and_unmapped_selectors_preserve_queue_order_and_count(self):
        _, cues = self.run_particle("ysm.particle('heart');ysm.particle('sweep_attack');"
            "ysm.abs_particle('enchanted_hit',0,0,0,0,0,0,0,3);ysm.particle('vibration 1 2 3 4');"
            "ysm.particle('damage_indicator');ysm.particle('heart',2);")
        self.assertEqual([(c['name'], len(c['spawns'])) for c in cues], [
            ('minecraft:heart', 1), ('minecraft:sweep_attack', 1), ('minecraft:enchanted_hit', 3),
            ('minecraft:vibration 1 2 3 4', 1), ('minecraft:damage_indicator', 1), ('minecraft:heart', 1)])
        self.assertEqual(cues[2]['emitter'], 'minecraft:magic_critical_hit_emitter')
        self.assertEqual(cues[3]['emitter'], 'minecraft:vibration_signal')
        self.assertEqual(cues[4]['native_type'], 'Heart')

    def test_block_vibration_uses_world_center_and_script_lifetime(self):
        selectors = ['vibration -2.1 6 12 150',
                     'vibration{destination:{type:"minecraft:block",pos:[-3,6,12]},arrival_in_ticks:2}']
        for selector in selectors:
            _, cues = self.run_particle(f"ysm.abs_particle('{selector}',0,0,0,0,0,0,0,0,11);",
                                        {'position': [2.5, 6.5, 2.5]})
            sample = cues[0]['spawns'][0]
            self.assertEqual(sample['block_center'], [-2.5, 6.5, 12.5])
            self.assertEqual(sample['vibration_velocity_per_second'], [-10, 0, 20])
        _, cues = self.run_particle("ysm.particle('vibration 1 2 3 20',0,0,0,0,0,0,0,0,1);")
        self.assertEqual(cues[0]['spawns'][0]['vibration_velocity_per_second'], [0, 0, 0])

    def test_entity_vibration_rejection_stops_program_and_retains_prior_emissions(self):
        for uuid in ('[I;0,0,0,37]', '[0,0,0,38]'):
            selector = f'vibration{{destination:{{type:"minecraft:entity",source_entity:{uuid},y_offset:1.25f}},arrival_in_ticks:7}}'
            with self.subTest(uuid=uuid):
                report, cues = self.run_particle(
                    f"ysm.particle('heart');ysm.abs_particle('{selector}');ysm.particle('flame');", failures=1)
                self.assertEqual([cue['name'] for cue in cues], ['minecraft:heart'])
                self.assertIn('Entity position sources are not allowed', report['evaluation_diagnostics'])

    def test_rejected_particle_starts_no_emitter_and_next_update_still_runs(self):
        invalid = 'vibration{destination:{type:"minecraft:entity",source_entity:[I;0,0,0,37]},arrival_in_ticks:7}'
        report = self.player({'@player_update': f"v.once?{{ysm.particle('heart');}}:{{v.once=1;ysm.particle('{invalid}');ysm.particle('flame');}};"},
                             [{'time': 0}, {'time': .1}], failures=1)
        self.assertEqual([[cue['name'] for cue in frame['particle_cues']] for frame in report['frames']],
                         [[], ['minecraft:heart']])

    def test_parameterized_registry_entries_reject_missing_and_invalid_options(self):
        for selector in ['shriek', 'shriek 1.25', 'vibration 1 2 3', 'sculk_charge nope']:
            with self.subTest(selector=selector):
                _, cues = self.run_particle(f"ysm.particle('heart');ysm.particle('{selector}');", failures=1)
                self.assertEqual([cue['name'] for cue in cues], ['minecraft:heart'])

    def test_nether_motes_are_local_colored_point_emissions(self):
        colors = {'ash': [.25] * 3, 'white_ash': [186/255, 177/255, 194/255],
                  'crimson_spore': [.9, .4, .5], 'warped_spore': [.1, .1, .3]}
        script = ''.join(f"ysm.abs_particle('{name}',1,2,3,0,0,0,0,1,29);" for name in colors)
        _, cues = self.run_particle(script, {'position': [10, 20, 30]})
        self.assertEqual(len(cues), len(colors))
        for cue, color in zip(cues, colors.values()):
            self.assertEqual(cue['coverage'], 'approximate')
            emitter = 'creaking_heart_trail' if cue['name'].endswith('_spore') else 'falling_dust'
            self.assertEqual(cue['emitter'], 'minecraft:' + emitter)
            self.assertEqual(cue['spawns'][0]['position'], [11, 22, 33])
            self.assertEqual(cue['spawns'][0]['lifetime'], 29)
            for actual, expected in zip(cue['color'], color):
                self.assertAlmostEqual(actual, expected)

    def test_parameterized_particle_selectors_keep_resource_or_color(self):
        for selector, native, resource, color, scale in [
            ('minecraft:dust 0.25 0.5 1 2', 'RedDust', '', [0.25, 0.5, 1], 2),
            ('minecraft:block minecraft:oak_planks', 'Terrain', 'minecraft:oak_planks', None, 1),
            ('item apple', 'IconCrack', 'minecraft:apple', None, 1),
        ]:
            with self.subTest(selector=selector):
                _, cues = self.run_particle(f"ysm.particle('{selector}');")
                self.assertEqual(len(cues), 1)
                self.assertEqual([cues[0][key] for key in ('native_type', 'resource', 'color', 'scale')],
                                 [native, resource, color, scale])

    def test_structured_java_particle_selector_uses_color_and_registry_values(self):
        for selector, expected in [
            ('dust{color:[.25f,0.5f,1f],scale:2f}', ('RedDust', '', [0.25, 0.5, 1], 2)),
            ('entity_effect{color:-65536}', ('MobSpell', '', [1, 0, 0], 1)),
            ('falling_dust{block_state:{Name:"minecraft:sand"}}', ('Terrain', 'minecraft:sand', None, 1)),
            ('block_marker{block_state:"minecraft:glass"}', ('Terrain', 'minecraft:glass', None, 1)),
            ('item{item:{id:"minecraft:apple"}}', ('IconCrack', 'minecraft:apple', None, 1)),
        ]:
            with self.subTest(selector=selector):
                _, cues = self.run_particle(f"ysm.particle('{selector}');")
                self.assertEqual(tuple(cues[0][key] for key in ('native_type', 'resource', 'color', 'scale')), expected)

    def test_legacy_and_structured_dust_keep_both_colors(self):
        for selector in ['dust_color_transition 1 0 0 1.5 0 0 1',
                         'dust_color_transition{from_color:[1,0,0],to_color:[0,0,1],scale:1.5}']:
            with self.subTest(selector=selector):
                _, cues = self.run_particle(f"ysm.abs_particle('{selector}',1,2,3);")
                self.assertEqual(cues[0]['color'], [1, 0, 0])
                self.assertEqual(cues[0]['end_color'], [0, 0, 1])
                self.assertEqual(cues[0]['scale'], 1.5)
                self.assertEqual(cues[0]['spawns'][0]['position'], [1, 2, 3])

    def test_source_integer_saturation_does_not_allocate_unbounded_particles(self):
        report, cues = self.run_particle("ysm.particle('note',0,0,0,0,0,0,0,3.0e12,3.0e12);")
        self.assertEqual(cues[0]['count'], 2147483647)
        self.assertEqual(cues[0]['lifetime'], 2147483647)
        self.assertEqual(cues[0]['spawns'], [])
        self.assertTrue(any('Particle spawn budget' in s for s in report['evaluation_diagnostics']))
