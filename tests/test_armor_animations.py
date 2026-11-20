"""Equipment selection, source priority and controller override in both owners."""
import unittest

import test_first_person as fp
import test_model_functions as body


class ArmorAnimationTests(unittest.TestCase):
    def frames(self, clips, frames, first_person=True, controllers=None, functions=None):
        if first_person:
            return fp.FirstPersonTests().run_frames(clips, frames, controllers, functions)
        return body.ModelFunctionTests().player(functions or {}, frames, clips, controllers)['frames']

    def test_id_then_declared_tag_then_default(self):
        clips = {name: fp.FirstPersonTests.clip(value) for name, value in [
            ('head$minecraft:diamond_helmet', 1), ('head#test:z', 2), ('head#test:a', 3), ('head:default', 4)]}
        values = [{'id': 'minecraft:diamond_helmet', 'tags': ['test:a', 'test:z']},
                  {'id': 'test:helmet', 'tags': ['test:a', 'test:z']}, {'id': 'test:helmet'}]
        for first_person in [True, False]:
            with self.subTest(first_person=first_person):
                frames = self.frames(clips, [{'time': i, 'armor': {'head': item}} for i, item in enumerate(values)], first_person)
                self.assertEqual([frame['pose']['arm']['rotation'][0] for frame in frames], [1, 2, 4])

    def test_empty_slot_stops_and_restores_lower_pose(self):
        clips = {'parallel0': fp.FirstPersonTests.clip(3), 'head:default': fp.FirstPersonTests.clip(17)}
        frames = self.frames(clips, [{'time': 0, 'armor': {'head': {'id': 'test:helmet'}}}, {'time': .2}, {'time': .4}])
        self.assertEqual(frames[0]['pose']['arm']['rotation'][0], 17)
        self.assertEqual(frames[-1]['pose']['arm']['rotation'][0], 3)

    def test_armor_forces_loop_and_equipment_identity_does_not_restart_same_clip(self):
        clip = fp.FirstPersonTests.clip('q.anim_time', loop=False)
        clip['timeline'] = {'0': 'v.starts+=1;'}
        frames = self.frames({'head:default': clip},
            [{'time': time, 'armor': {'head': {'id': item}}} for time, item in [(0, 'a'), (.5, 'b'), (1.25, 'b')]])
        self.assertEqual(frames[-1]['pose']['arm']['rotation'][0], .25)
        self.assertEqual(frames[1]['variables']['variable.starts'], 1)
        self.assertEqual(frames[-1]['variables']['variable.starts'], 2)

    def test_java_equipment_slot_order_has_head_after_chest(self):
        clips = {'chest:default': fp.FirstPersonTests.clip(3), 'head:default': fp.FirstPersonTests.clip(7)}
        for first_person in [True, False]:
            frame = self.frames(clips, [{'time': 0, 'armor': {'head': {'id': 'h'}, 'chest': {'id': 'c'}}}], first_person)[0]
            self.assertEqual(frame['pose']['arm']['rotation'][0], 7)

    def test_armor_stage_follows_high_parallel(self):
        clips = {'parallel0': fp.FirstPersonTests.clip(3), 'head:default': fp.FirstPersonTests.clip(7)}
        frame = self.frames(clips, [{'time': 0, 'armor': {'head': {'id': 'h'}}}])[0]
        self.assertEqual(frame['pose']['arm']['rotation'][0], 7)

    def test_explicit_armor_source_controller_runs_without_equipment(self):
        for first_person, category in [(True, 'fp.arm'), (False, 'player')]:
            with self.subTest(category=category):
                controllers = {category + '.armor_head': {'states': {'default': {'animations': ['custom']}}}}
                frame = self.frames({'custom': fp.FirstPersonTests.clip(13)}, [{'time': 0}], first_person, controllers)[0]
                self.assertEqual(frame['pose']['arm']['rotation'][0], 13)

    def test_preview_omits_all_armor_controllers(self):
        controllers = {'fp.arm.armor_head': {'states': {'default': {'on_entry': 'v.wrong=1;', 'animations': ['custom']}}}}
        frame = self.frames({'custom': fp.FirstPersonTests.clip(13)}, [{'time': 0, 'enabled': False}], controllers=controllers)[0]
        self.assertNotIn('variable.wrong', frame['variables'])
        self.assertEqual(frame['pose'], {})

    def test_ctrl_armor_reads_the_actual_slot_and_keeps_hold_kinds_distinct(self):
        source = "ctrl.armor('head','$minecraft:diamond_helmet')*10+(ctrl.hold('head','empty')??2)+(ctrl.armor('mainhand','empty')??3)"
        frame = self.frames({'parallel0': fp.FirstPersonTests.clip(source)},
            [{'time': 0, 'armor': {'head': {'id': 'minecraft:diamond_helmet'}}}])[0]
        self.assertEqual(frame['pose']['arm']['rotation'][0], 15)

    def test_ctrl_armor_supports_tags_and_empty_but_not_use_classifiers(self):
        source = "ctrl.armor('head','#test:a')*100+ctrl.armor('feet','empty')*10+ctrl.armor('head',':eat')"
        frame = self.frames({'parallel0': fp.FirstPersonTests.clip(source)},
            [{'time': 0, 'armor': {'head': {'id': 'helmet', 'tags': ['test:a'], 'use_action': 'eat'}}}])[0]
        self.assertEqual(frame['pose']['arm']['rotation'][0], 110)

    def test_equipment_snapshot_exposes_enchantment_totals_durability_and_tags(self):
        source = """
            v.total=ysm.equipped_enchantment_level('mainhand','mending','minecraft:sharpness','unbreaking');
            v.max=q.max_durability('mainhand');
            v.remaining=q.remaining_durability('mainhand');
            v.any=q.equipped_item_any_tag('mainhand','author:missing','author:a');
            v.all=q.equipped_item_all_tags('head','author:a','author:b');
            v.swinging=ysm.swinging;v.swinging_arm=ysm.swinging_arm;v.swing_time=ysm.swing_time;
            return v.total;
        """
        item = {'id': 'minecraft:diamond_sword', 'tags': ['author:a'], 'max_durability': 100,
                'damage': 17, 'enchantments': {'mending': 1, 'minecraft:sharpness': 5, 'unbreaking': 3, 'looting': 3}}
        armor = {'head': {'id': 'minecraft:diamond_helmet', 'tags': ['author:a', 'author:b']}}
        for first in (True, False):
            with self.subTest(first_person=first):
                frames = self.frames({'parallel0': fp.FirstPersonTests.clip(source)}, [
                    {'time': 0, 'items': [item], 'armor': armor, 'swinging_hand': 1, 'swing_progress': 2}, {'time': .2}], first)
                expected = dict(total=9, max=100, remaining=83, any=1, all=1, swinging=1, swinging_arm=1, swing_time=2)
                for frame, values in zip(frames, (expected, dict.fromkeys(expected, 0))):
                    self.assertEqual({key: frame['variables']['variable.' + key] for key in values}, values)

    def test_armor_observation_keeps_owner_selection_and_events_frozen(self):
        clip = fp.FirstPersonTests.clip('ysm.rendering_in_inventory?45:7')
        clip['timeline'] = {'0': 'v.events+=1;'}
        frames = self.frames({'head:default': clip}, [{'time': 0, 'armor': {'head': {'id': 'helmet'}}},
            {'time': 10, 'observe': True, 'armor': {}, 'queries': {'ysm.rendering_in_inventory': 1}},
            {'time': .2, 'armor': {'head': {'id': 'helmet'}}}])
        self.assertEqual(frames[1]['pose']['arm']['rotation'][0], 45)
        self.assertEqual(frames[1]['logical_variables'], frames[0]['logical_variables'])
        self.assertEqual(frames[-1]['variables']['variable.events'], 1)


if __name__ == '__main__':
    unittest.main(verbosity=2)
