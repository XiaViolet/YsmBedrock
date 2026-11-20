"""Native Java tag routing and wing-roll parity against the official client."""
import struct
import unittest

import elytra_java_vectors
import test_controllers
import test_entity_queries
import test_hand_animations
import test_model_functions


class JavaVanillaInputTests(unittest.TestCase):
    run_frames = test_hand_animations.HandAnimationTests.run_frames
    invoke = test_controllers.ControllerTests.invoke
    player = test_model_functions.ModelFunctionTests.player

    def wing_values(self, entities):
        runner = test_entity_queries.EntityQueryTests()
        return runner.observe('ysm.elytra_rot_z', entities)

    def test_wing_roll_matches_unmodified_official_elytra_model(self):
        vectors = elytra_java_vectors.VECTORS
        values = self.wing_values([{k: v for k, v in frame.items() if k != 'expected_f32_bits'} for frame in vectors])
        for index, (frame, value) in enumerate(zip(vectors, values)):
            with self.subTest(frame=index):
                self.assertEqual(struct.unpack('<I', struct.pack('<f', value))[0], frame['expected_f32_bits'])

    def test_wing_roll_advances_once_for_duplicate_render_passes(self):
        base = {'emulate_elytra': True, 'elytra_equipped': True}
        frames = [dict(base, render_frame=i) for i in range(8)]
        repeated = [dict(frame) for frame in frames for _ in range(3)]
        self.assertEqual(self.wing_values(repeated), [value for value in self.wing_values(frames) for _ in range(3)])

    def test_first_person_and_unequipped_layers_do_not_change_wing_state(self):
        frames = [dict(emulate_elytra=True, render_frame=i, gliding=True, elytra_equipped=True,
                       render_elytra_layer=False) for i in range(5)]
        frames += [dict(emulate_elytra=True, render_frame=i + 5, gliding=True, elytra_equipped=False) for i in range(5)]
        self.assertEqual(self.wing_values(frames), [0] * 10)

    def test_first_person_pass_before_body_still_allows_one_wing_layer_update(self):
        base = {'emulate_elytra': True, 'elytra_equipped': True}
        frames = [dict(base, render_frame=0, render_elytra_layer=False),
                  dict(base, render_frame=0), dict(base, render_frame=0),
                  dict(base, render_frame=1, render_elytra_layer=False)]
        values = self.wing_values(frames)
        self.assertEqual(values[:3], [0, 0, 0])
        self.assertAlmostEqual(values[3], -1.5, places=5)

    def test_wing_query_function_and_property_are_equivalent(self):
        runner = test_entity_queries.EntityQueryTests()
        self.assertEqual(runner.observe('ysm.elytra_rot_z()==ysm.elytra_rot_z', [{'elytra_rot_z': -37.25}]), [1])

    def test_direct_host_roll_does_not_advance_the_emulated_state(self):
        frames = [dict(emulate_elytra=True, elytra_equipped=True, render_frame=0, elytra_rot_z=-37.25),
                  dict(emulate_elytra=True, elytra_equipped=True, render_frame=1),
                  dict(emulate_elytra=True, elytra_equipped=True, render_frame=2)]
        values = self.wing_values(frames)
        self.assertEqual(values[:2], [-37.25, 0])
        self.assertAlmostEqual(values[2], -1.5, places=5)

    def test_unequipping_retains_last_player_wing_angle(self):
        frames = [dict(emulate_elytra=True, elytra_equipped=True, render_frame=i) for i in range(5)]
        frames += [dict(emulate_elytra=True, elytra_equipped=False, render_frame=i) for i in range(5, 9)]
        values = self.wing_values(frames)
        self.assertLess(values[5], values[4])
        self.assertEqual(values[5:], [values[5]] * 4)

    def test_generated_vanilla_and_ysm_tags_select_authored_hold_clips(self):
        for identifier, tag in [('wooden_sword', 'minecraft:swords'), ('diamond_pickaxe', 'ysm:pickaxes'),
                                ('shield', 'ysm:shields'), ('bow', 'ysm:bows'), ('crossbow', 'forge:tools/crossbows'),
                                ('fishing_rod', 'ysm:fishing_rods'), ('trident', 'ysm:tridents')]:
            with self.subTest(item=identifier):
                name = 'hold_mainhand#' + tag
                frames = self.run_frames({name: {'loop': True}},
                    [{'time': 0, 'native_bedrock': True, 'items': [{'id': 'minecraft:' + identifier}]}])
                self.assertEqual(frames[0]['layers'][0]['name'], name)

    def test_native_tags_keep_exact_id_and_declared_tag_priority(self):
        names = ['hold_mainhand#forge:tools', 'hold_mainhand#minecraft:swords',
                 'hold_mainhand:sword', 'hold_mainhand$minecraft:wooden_sword']
        clips = {name: {'loop': True} for name in names}
        frame = {'time': 0, 'native_bedrock': True, 'items': [{'id': 'minecraft:wooden_sword'}]}
        self.assertEqual(self.run_frames(clips, [frame])[0]['layers'][0]['name'], names[-1])
        clips.pop(names[-1])
        self.assertEqual(self.run_frames(clips, [frame])[0]['layers'][0]['name'], names[0])

    def test_legacy_ysm_tags_work_for_weapon_animation_names_and_controller_queries(self):
        for weapon in ('bow', 'crossbow'):
            with self.subTest(weapon=weapon):
                tag = f'#yes_steve_model:{weapon}s'
                name = 'use_mainhand' + tag
                frame = {'time': 0, 'native_bedrock': True, 'using_hand': 0, 'items': [{'id': 'minecraft:' + weapon}]}
                clip = {'loop': True, 'bones': {'root': {'rotation': [15, 0, 0]}}}
                frames = self.player({'@player_update': f"v.matches=ctrl.use('mainhand','{tag}');"},
                                     [frame, dict(frame, time=0.2)], {name: clip})['frames']
                self.assertEqual(frames[0]['variables']['variable.matches'], 1)
                self.assertEqual(frames[0]['controllers']['coded']['player.use']['selected_animation'], name)
                self.assertEqual(frames[1]['pose']['root']['rotation'][0], 15)

    def test_tag_adapter_does_not_assign_vanilla_tool_tags_to_custom_or_empty_items(self):
        clips = {'hold_mainhand#minecraft:swords': {'loop': True}, 'hold_mainhand#pack:weapon': {'loop': True}}
        frames = self.run_frames(clips, [dict(time=i, native_bedrock=True, items=[item]) for i, item in enumerate([
            {'id': 'pack:wooden_sword'}, {'id': 'minecraft:diamond'}, {},
            {'id': 'pack:weapon', 'tags': ['pack:weapon']}])])
        self.assertEqual([len(frame['layers']) for frame in frames], [0, 0, 0, 1])
        self.assertEqual(frames[-1]['layers'][0]['name'], 'hold_mainhand#pack:weapon')

    def test_ysm_classification_uses_resolved_data_tags(self):
        clips = {'hold_mainhand:sword': {'loop': True}, 'hold_mainhand:shield': {'loop': True}}
        frames = self.run_frames(clips, [dict(time=i, native_bedrock=True, items=[{'id': item}])
            for i, item in enumerate(['minecraft:iron_sword', 'minecraft:shield'])])
        self.assertEqual([frame['layers'][0]['name'] for frame in frames], list(clips))

    def test_armor_and_equipment_queries_share_the_resolved_java_tags(self):
        clip = {'loop': True, 'bones': {'root': {'rotation': [17, 0, 0]}}}
        frames = self.player({'@player_update': "v.matches=ctrl.armor('head','#forge:armors/helmets');"},
            [{'time': 0, 'native_bedrock': True, 'armor': {'head': {'id': 'minecraft:diamond_helmet'}}}],
            {'head#forge:armors/helmets': clip})['frames']
        self.assertEqual(frames[0]['logical_variables']['variable.matches'], 1)
        self.assertEqual(frames[0]['pose']['root']['rotation'][0], 17)

    def test_mount_and_passenger_tags_use_entity_type_data(self):
        names = ['vehicle#minecraft:dismounts_underwater', 'passenger#minecraft:fall_damage_immune', 'sit']
        clips = {name: {'loop': True} for name in names}
        frames = self.invoke({}, clips, [dict(time=t, native_bedrock=True, vehicle={'id': 'minecraft:pig'},
            passenger_entity={'id': 'minecraft:chicken'}) for t in [0, .2]], player=True)
        coded = frames[-1]['controllers']['coded']
        self.assertEqual(coded['player.vehicle']['animation'], names[0])
        self.assertEqual(coded['player.passenger']['animation'], names[1])

    def test_boat_does_not_inherit_pig_entity_tags(self):
        clips = {'vehicle#minecraft:dismounts_underwater': {'loop': True}, 'boat': {'loop': True}}
        frames = self.invoke({}, clips, [dict(time=t, native_bedrock=True,
            vehicle={'id': 'minecraft:boat', 'legacy_animation': 'boat'}) for t in [0, .2]], player=True)
        self.assertEqual(frames[-1]['controllers']['coded']['player.vehicle']['animation'], 'boat')
