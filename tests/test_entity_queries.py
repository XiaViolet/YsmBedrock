"""Source query meanings through the actual C++ binder, expressions and players."""
import unittest

import test_controllers as controllers
import test_model_functions as functions
import test_molang as molang


class EntityQueryTests(unittest.TestCase):
    run_expr = molang.MolangTests.run_expr
    value = molang.MolangTests.value
    animation = molang.MolangTests.animation
    invoke = controllers.ControllerTests.invoke
    clip = staticmethod(controllers.ControllerTests.clip)
    state = staticmethod(controllers.ControllerTests.state)
    player = functions.ModelFunctionTests.player

    def observe(self, expression, snapshots):
        report = self.run_expr(expression, {'frames': [{'entity': item} for item in snapshots]})
        self.assertEqual(report['failures'], 0, report)
        self.assertEqual(report['diagnostics'], [], report)
        return report['values']

    def test_flight_velocity_uses_the_clock_that_interpolated_the_position(self):
        # A repeated shadow pass and uneven presentation times during vertical flight.
        frames = [{'render_ticks': wall, 'position_sample_ticks': tick,
                   'render_position': [0, height, 0]}
                  for wall, tick, height in [(1, 20, 64), (1.4, 20.25, 64.125),
                     (2, 20.25, 64.125), (2.1, 21, 64.5), (3.7, 22, 64.5),
                     (4.1, 22.5, 64.25), (6, 23, 64), (7, 1, 16)]]
        self.assertEqual(self.observe('q.vertical_speed', frames), [0, 10, 10, 10, 0, -10, -10, 0])

    def test_scene_and_item_queries_follow_current_input(self):
        entity = {'player_level': 31, 'position': [1, 2, 3], 'camera_position': [4, 6, 3],
                  'camera_rotation': [-12, 23], 'hit_target_id': 'minecraft:pig', 'hit_target_type': 'entity',
                  'biome': 'hell', 'using_item': True, 'item_use_ticks': 12, 'item_max_use_ticks': 20}
        expression = ("return q.player_level>30&&q.distance_from_camera==5&&q.rotation_to_camera(0)==-12"
                      "&&q.rotation_to_camera(1)==23&&ysm.hit_target_id=='minecraft:pig'"
                      "&&ysm.hit_target_type=='entity'&&q.biome_has_all_tags('minecraft:is_nether')"
                      "&&q.item_max_use_duration==1&&q.item_remaining_use_duration>.39"
                      "&&q.item_in_use_duration>.59;")
        self.assertEqual(self.observe(expression, [dict(entity, player_level=30), entity, {'player_level': 31}]),
                         [0, 1, 0])

    def test_fractional_health_maximum_and_food_are_not_truncated_to_integers(self):
        source = 'v.hp=q.health;v.max=q.max_health;v.food=ysm.food_level;return q.health/q.max_health;'
        report = self.run_expr(source, {'entity': {'health': 9.5, 'max_health': 25, 'food_level': 12.25}})
        self.assertEqual(report['value'], 0.38)
        self.assertEqual(report['variables'], {'variable.hp': 9.5, 'variable.max': 25, 'variable.food': 12.25})
        self.assertEqual(report['diagnostics'], [])

    def test_missing_living_attributes_return_zero_instead_of_absent_values(self):
        self.assertEqual(self.observe('(q.health??99)+(q.max_health??99)+(ysm.food_level??99)', [{}]), [0])

    def test_nonliving_subject_does_not_inherit_player_or_living_queries(self):
        fields = {'is_player': False, 'health': 18, 'max_health': 20, 'food_level': 16,
                  'move_input': [0, .5, 0], 'fishing': True, 'eating': True,
                  'sleeping': True, 'render_ticks': 86}
        for expression in ['q.is_jumping', 'ysm.is_fishing', 'ysm.is_close_eyes',
                           'q.is_eating', 'q.health', 'q.max_health', 'ysm.food_level', 'ysm.yya']:
            with self.subTest(expression=expression):
                self.assertEqual(self.observe(expression, [fields]), [0])

    def test_living_nonplayer_keeps_living_queries_without_player_only_states(self):
        fields = {'is_player': False, 'is_living': True, 'health': 18, 'max_health': 20,
                  'food_level': 16, 'move_input': [0, .5, 0], 'fishing': True,
                  'eating': True, 'render_ticks': 86}
        self.assertEqual(self.observe('q.health+q.max_health+ysm.yya+q.is_eating+ysm.is_close_eyes',
                                     [fields]), [40.5])
        self.assertEqual(self.observe('q.is_jumping+ysm.is_fishing+ysm.food_level', [fields]), [0])

    def test_query_subject_transition_clears_player_state_and_recovers_player_values(self):
        player = {'render_ticks': 86, 'fishing': True, 'health': 12, 'max_health': 20,
                  'move_input': [0, .5, 0], 'eating': True}
        expression = 'q.is_jumping+ysm.is_fishing+ysm.is_close_eyes+q.is_eating+q.health+q.max_health+ysm.yya'
        self.assertEqual(self.observe(expression, [player, player | {'is_player': False}, player]),
                         [36.5, 0, 36.5])

    def test_world_timestamp_remains_ticks_across_day_boundaries(self):
        self.assertEqual(self.observe('q.time_stamp', [{'world_time': t} for t in [23999, 24000, 24001, 2400123]]),
                         [23999, 24000, 24001, 2400123])

    def test_speed_uses_horizontal_tick_displacement_and_keeps_signed_vertical_velocity(self):
        report = self.run_expr('v.x=q.position_delta(0);v.y=q.position_delta(1);v.z=q.position_delta(2);'
                               'v.vertical=q.vertical_speed;return q.ground_speed;',
                               {'entity': {'velocity': [3, -0.5, 4]}})
        self.assertEqual(report['value'], 100)
        self.assertEqual(report['variables'], {'variable.x': 3, 'variable.y': -0.5, 'variable.z': 4, 'variable.vertical': -10})

    def test_render_position_axis_and_invalid_axis(self):
        snapshot = {'position': [100, 30, -95], 'render_position': [100.5, 30.75, -94.25]}
        report = self.run_expr('v.x=q.position(0);v.y=q.position(1);v.z=q.position(2);return q.position(3)??27;',
                               {'entity': snapshot})
        self.assertEqual(report['value'], 27)
        self.assertEqual(report['variables'], {'variable.x': 100.5, 'variable.y': 30.75, 'variable.z': -94.25})

    def test_relative_blocks_use_current_feet_and_floor_negative_world_coordinates(self):
        data = {'entity': {'position': [0.25, 64, -0.2], 'render_position': [100, 200, 300], 'view_yaw': 90},
                'blocks': [{'position': [-1, 65, -1], 'id': 'minecraft:stone'},
                           {'position': [0, 65, 0], 'id': 'minecraft:air'}]}
        report = self.run_expr('v.name=ysm.relative_block_name(-0.5,1.25,-0.7);'
                               'return v.name==\'minecraft:stone\' && '
                               'ysm.relative_block_name(0,1.8,0.8)==\'minecraft:air\';', data)
        self.assertEqual(report['value'], 1)
        self.assertEqual(report['variables']['variable.name'], 'minecraft:stone')
        self.assertEqual(report['diagnostics'], [])

    def test_block_tags_use_java_and_custom_memberships_with_bedrock_aliases(self):
        blocks = [dict(position=[x, 0, 0], id='minecraft:' + name)
                  for x, name in enumerate(['snow', 'snow_layer', 'short_grass', 'flowing_water', 'stone'])]
        blocks[-1]['tags'] = ['author:look_at']
        report = self.run_expr("""
            v.full_snow=q.relative_block_has_any_tag(0,0,0,'replaceable');
            v.layer=q.relative_block_has_any_tag(1,0,0,'minecraft:replaceable');
            v.grass=q.relative_block_has_any_tag(2,0,0,'replaceable');
            v.water=q.relative_block_has_any_tag(3,0,0,'replaceable');
            v.stone=q.relative_block_has_any_tag(4,0,0,'minecraft:base_stone_overworld');
            return q.relative_block_has_any_tag(4,0,0,'minecraft:leaves','author:look_at');
            """, {'entity': {}, 'blocks': blocks})
        self.assertEqual(report['value'], 1)
        self.assertEqual(report['variables'], {'variable.full_snow': 0, 'variable.layer': 1,
                         'variable.grass': 1, 'variable.water': 1, 'variable.stone': 1})
        self.assertEqual(report['diagnostics'], [])

    def test_relative_block_bounds_missing_data_and_tag_order(self):
        data = {'blocks': [dict(position=[5, 0, 0], id='minecraft:air')]}
        report = self.run_expr("""
            v.at_limit=q.relative_block_has_any_tag(5,0,0,'replaceable','invalid?');
            v.too_far=q.relative_block_has_any_tag(5.01,0,0,'replaceable')??-1;
            v.missing=ysm.relative_block_name(-1,0,0)??'missing';
            return q.relative_block_has_any_tag(5,0,0,'invalid?','replaceable')??-1;
            """, data)
        self.assertEqual(report['value'], -1)
        self.assertEqual(report['variables'], {'variable.at_limit': 1, 'variable.too_far': -1,
                                               'variable.missing': 'missing'})

    def test_all_relative_block_tags_and_multiple_ids_share_the_sample(self):
        data = {'blocks': [{'position': [1, 0, 0], 'id': 'minecraft:stone', 'tags': ['author:a']}]}
        report = self.run_expr("""
            v.all=q.relative_block_has_all_tags(1,0,0,'base_stone_overworld','author:a');
            v.miss=q.relative_block_has_all_tags(1,0,0,'base_stone_overworld','author:b');
            v.name=ysm.relative_block_name_any(1,0,0,'dirt','stone');
            return ysm.relative_block_name_any(2,0,0,'stone')??-1;
        """, data)
        self.assertEqual(report['variables'], {'variable.all': 1, 'variable.miss': 0, 'variable.name': 1})
        self.assertEqual(report['value'], -1)

    def test_wetness_is_local_and_riding_uses_the_subject(self):
        snapshots = [{}, {'rain_level': 1, 'sky_visible': False},
                     {'in_rain': True}, {'in_water': True, 'passenger': True}, {}]
        self.assertEqual(self.observe('q.is_in_water_or_rain+2*q.is_riding()', snapshots), [0, 0, 1, 3, 0])

    def test_displacement_tracks_one_logical_update_at_a_time(self):
        snapshots = [
            {'render_ticks': 10, 'render_position': [100, 70, -10]},
            {'render_ticks': 12, 'render_position': [102, 69, -10]},
            {'render_ticks': 12, 'render_position': [700, 21, -10]},
            {'render_ticks': 50, 'render_position': [500, 70, 500], 'rendering_in_inventory': True},
            {'render_ticks': 14, 'render_position': [103, 69, -9]},
            {'render_ticks': 1, 'render_position': [200, 3, 300]},
        ]
        self.assertEqual(self.observe('q.position_delta(0)', snapshots), [0, 2, 2, 2, 1, 0])
        self.assertEqual(self.observe('q.vertical_speed', snapshots), [0, -10, -10, -10, 0, 0])
        speeds = self.observe('ysm.ground_speed2()', snapshots)
        self.assertEqual(speeds[:4], [0, 20, 20, 20])
        self.assertAlmostEqual(speeds[4], 2 ** .5 * 10)
        self.assertEqual(speeds[5], 0)

    def test_relative_motion_axes_respect_camera_facing_and_stationary_frames(self):
        snapshots = [
            {'render_ticks': 0, 'render_position': [0, 0, 0]},
            {'render_ticks': 1, 'render_position': [0, 0, 1]},
            {'render_ticks': 2, 'render_position': [1, 0, 1]},
            {'render_ticks': 3, 'render_position': [2, 0, 1], 'view_yaw': -90},
            {'render_ticks': 4, 'render_position': [2, 1, 1], 'view_yaw': 90},
        ]
        lateral = self.observe('ysm.input_horizontal', snapshots)
        forward = self.observe('ysm.input_vertical()', snapshots)
        for actual, desired in zip(lateral, [0, 0, -1, 0, 0]):
            self.assertAlmostEqual(actual, desired)
        for actual, desired in zip(forward, [0, 1, 0, 1, 0]):
            self.assertAlmostEqual(actual, desired)

    def test_vertical_move_input_keeps_signed_fraction_and_does_not_read_world_velocity(self):
        report = self.run_expr('v.vertical=q.vertical_speed;return ysm.yya;',
                               {'entity': {'velocity': [0, 0.75, 0], 'move_input': [0.6, -0.35, 0.8]}})
        self.assertEqual(report['value'], -0.35)
        self.assertEqual(report['variables']['variable.vertical'], 15)
        self.assertEqual(report['diagnostics'], [])

    def test_forward_strafe_and_falling_do_not_become_vertical_input(self):
        snapshots = [{'move_input': [0, 0, 1]}, {'move_input': [1, 0, 0]}, {'velocity': [0, -0.5, 0]},
                     {'move_input': [0, 0.5, 0]}, {'move_input': [0, -0.5, 0]}, {}]
        self.assertEqual(self.observe('ysm.yya()', snapshots), [0, 0, 0, 0.5, -0.5, 0])

    def test_fps_reads_the_host_statistic_as_a_nonnegative_whole_frame_count(self):
        snapshots = [{'fps': rate} for rate in [144.875, 60, 60, 29.75, 0, -1]] + [{}]
        self.assertEqual(self.observe('ysm.fps', snapshots), [144, 60, 60, 29, 0, 0, 0])
        self.assertEqual(self.observe('ysm.fps()==ysm.fps', [{'fps': 73.5}]), [1])

    def test_vertical_input_drives_the_authored_rotation_expression(self):
        clips = {'idle': {'loop': True, 'bones': {'root': {'rotation': [0, 'ysm.yya*20', 0],
                                                          'position': ['ysm.fps', 0, 0]}}}}
        for axis, expected in [(-0.35, -7), (0, 0), (0.5, 10)]:
            report = self.animation(clips, context={'entity': {'move_input': [0, axis, 1], 'fps': 120.5}})
            self.assertEqual(report['pose']['root']['rotation'], [0, expected, 0])
            self.assertEqual(report['pose']['root']['position'], [120, 0, 0])
            self.assertEqual(report['evaluation_diagnostics'], [])

    def test_functions_and_deferred_callbacks_share_the_current_input_and_fps_snapshot(self):
        report = self.player({'@player_update': 'v.input=ysm.yya;ysm.defer(0);', '@defer': 'v.fps=ysm.fps;'},
                             [{'time': time, 'entity': {'move_input': [0, axis, 0], 'fps': fps}}
                              for time, axis, fps in [(0, 0.5, 120.5), (0.2, -0.25, 60.9)]])
        self.assertEqual([frame['variables']['variable.input'] for frame in report['frames']], [0.5, -0.25])
        self.assertEqual([frame['variables']['variable.fps'] for frame in report['frames']], [120, 60])

    def test_airborne_falling_and_apex_still_count_as_jumping(self):
        self.assertEqual(self.observe('q.is_jumping', [{'velocity': [0, y, 0]} for y in [0.5, 0, -0.5]]), [1, 1, 1])

    def test_flying_riding_water_and_ground_suppress_jump_independently(self):
        snapshots = [{}] + [{key: True} for key in ['flying', 'passenger', 'in_water', 'on_ground']] + [{}]
        self.assertEqual(self.observe('q.is_jumping', snapshots), [1, 0, 0, 0, 0, 1])

    def test_crouch_is_a_sneak_query_only_while_grounded(self):
        self.assertEqual(self.observe('q.is_sneaking', [{'on_ground': g, 'crouching': c}
                                                       for g, c in [(True, True), (False, True), (True, False), (True, True)]]),
                         [1, 0, 0, 1])

    def test_source_head_query_x_is_yaw_and_y_is_pitch_with_matching_ysm_aliases(self):
        report = self.run_expr('v.x=q.head_x_rotation;v.y=q.head_y_rotation;'
                               'return (q.head_x_rotation==ysm.head_yaw)&&(q.head_y_rotation()==ysm.head_pitch);',
                               {'entity': {'head_yaw': -63, 'head_pitch': 17}})
        self.assertEqual(report['value'], 1)
        self.assertEqual(report['variables'], {'variable.x': -63, 'variable.y': 17})

    def test_body_rotation_wraps_to_minus_180_inclusive(self):
        self.assertEqual(self.observe('q.body_y_rotation', [{'body_yaw': v} for v in [-540, -180, 180, 540, 720, -721]]),
                         [-180, -180, -180, -180, 0, -1])

    def test_native_pose_without_render_data_keeps_actual_relative_head_yaw(self):
        report = self.run_expr('v.yaw=ysm.head_yaw;v.pitch=ysm.head_pitch;return q.head_x_rotation;',
                               {'entity': {'raw_head_pitch': -3.00491333, 'body_yaw': -100.36302185,
                                           'native_head_yaw': -150.36302185}})
        self.assertAlmostEqual(report['value'], 50)
        self.assertAlmostEqual(report['variables']['variable.yaw'], 50)
        self.assertAlmostEqual(report['variables']['variable.pitch'], 3.00491333)

    def test_native_head_turn_is_relative_to_body_for_every_world_facing(self):
        snapshots = [{'body_yaw': angle, 'native_head_yaw': angle + 30} for angle in [-720, -170, 0, 170, 720]]
        self.assertEqual(self.observe('ysm.head_yaw', snapshots), [-30] * 5)

    def test_native_head_turn_crosses_the_world_yaw_boundary_in_the_short_direction(self):
        self.assertEqual(self.observe('ysm.head_yaw', [{'body_yaw': 179, 'native_head_yaw': -179},
                                                      {'body_yaw': -179, 'native_head_yaw': 179}]), [-2, 2])

    def test_native_relative_head_yaw_wraps_before_clamping_to_java_limit(self):
        self.assertEqual(self.observe('ysm.head_yaw', [{'native_head_yaw': angle}
                                                      for angle in [90, -90, 180, -180, 270, -270]]),
                         [-85, 85, 85, 85, 85, -85])

    def test_native_pitch_conversion_and_source_aliases_share_one_angle_snapshot(self):
        report = self.run_expr('v.pitch=q.head_y_rotation;v.body=q.body_y_rotation;'
                               'return (q.head_x_rotation==ysm.head_yaw)&&(q.head_y_rotation==ysm.head_pitch);',
                               {'entity': {'raw_head_pitch': 17, 'body_yaw': 450, 'native_head_yaw': 460}})
        self.assertEqual(report['value'], 1)
        self.assertEqual(report['variables'], {'variable.pitch': -17, 'variable.body': 90})

    def test_native_head_angles_reach_the_real_coded_head_pass_after_authored_rotation(self):
        frames = self.invoke({'player.main': {'states': {'default': {'animations': ['a']}}}},
                             {'a': {'loop': True, 'bones': {'Head': {'rotation': [10, 20, 5]}}}},
                             [{'time': 0, 'entity': {'raw_head_pitch': -3, 'body_yaw': -100, 'native_head_yaw': -150}}],
                             player=True, geometry={'minecraft:geometry': [{'bones': [{'name': 'Head'}]}]})
        self.assertEqual(frames[0]['pose']['Head']['rotation'], [7, -30, 5])

    def test_cardinal_direction_uses_java_direction_ids_and_half_quadrant_boundaries(self):
        directions = [(-405, 3), (-135.001, 2), (-135, 5), (-45.001, 5), (-45, 3),
                      (0, 3), (44.999, 3), (45, 4), (134.999, 4), (135, 2), (225, 5), (315, 3), (810, 4)]
        self.assertEqual(self.observe('q.cardinal_facing_2d', [{'yaw': yaw} for yaw, _ in directions]),
                         [value for _, value in directions])

    def test_eye_target_rotation_is_the_unwrapped_view_angle_not_body_or_head(self):
        self.assertEqual(self.observe('q.eye_target_y_rotation', [{'yaw': 90, 'view_yaw': 725, 'body_yaw': 4, 'head_yaw': 3}]), [725])

    def test_flags_refresh_without_leaking_a_previous_subject_snapshot(self):
        source = 'q.is_sprinting+q.is_swimming*2+q.is_sleeping*4+q.is_alive*8'
        self.assertEqual(self.observe(source, [{'sprinting': True, 'swimming': True, 'sleeping': True, 'alive': False},
                                               {}, {'alive': False}, {'sprinting': True}]), [7, 8, 0, 9])

    def test_eating_and_fishing_can_start_and_end_independently(self):
        self.assertEqual(self.observe('q.is_eating+ysm.is_fishing*2', [{}, {'eating': True}, {'fishing': True},
                                                                    {'eating': True, 'fishing': True}, {}]), [0, 1, 2, 3, 0])

    def test_weather_thresholds_and_rain_attenuation_match_java_reads(self):
        inputs = [(0, 1), (0.2, 1), (0.20001, 0), (1, 0.9), (1, 0.90001), (0.5, 1), (1, 1)]
        self.assertEqual(self.observe('ysm.weather', [{'rain_level': r, 'thunder_level': t} for r, t in inputs]),
                         [0, 0, 1, 1, 2, 1, 2])

    def test_open_air_requires_both_sky_visibility_and_clearing_the_blocking_height(self):
        self.assertEqual(self.observe('ysm.is_open_air', [{'sky_visible': sky, 'block_y': y, 'blocking_height': height}
                                                         for sky, y, height in [(True, 64, 64), (True, 64, 65),
                                                                                (False, 64, 63), (True, -10, -11)]]),
                         [1, 0, 0, 1])

    def test_bedrock_actual_weak_thunder_snapshot_does_not_become_clear_weather(self):
        weather = {'rain': 0.04999999701976776, 'lightning': 0.049988556653261185}
        self.assertEqual(self.observe('ysm.weather', [{'bedrock_weather': weather}]), [2])

    def test_bedrock_thunder_rain_and_clear_transition_uses_host_types_at_every_strength(self):
        weather = [(1, 1), (0.05, 0.04998856), (0.05, 0), (1, 0), (0, 0)]
        self.assertEqual(self.observe('ysm.weather', [{'bedrock_weather': {'rain': rain, 'lightning': lightning}}
                                                    for rain, lightning in weather]), [2, 2, 1, 1, 0])

    def test_bedrock_global_weather_is_independent_of_the_subjects_roof(self):
        snapshots = [{'bedrock_weather': {'rain': 0.05, 'lightning': 0.04998856},
                      'sky_visible': visible, 'block_y': 79, 'blocking_height': height}
                     for visible, height in [(True, 79), (False, 83), (True, 79)]]
        self.assertEqual(self.observe('ysm.weather+ysm.is_open_air*10', snapshots), [12, 2, 12])

    def test_blink_open_interval_observes_fractional_entity_ticks(self):
        points = [(84, 0.9), (85, 0), (85, 0.1), (89, 0.999), (90, 0), (175, 0), (175, 0.5)]
        self.assertEqual(self.observe('ysm.is_close_eyes', [{'tick': t, 'partial_tick': p} for t, p in points]),
                         [0, 0, 1, 1, 0, 0, 1])

    def test_blink_phase_uses_absolute_uuid_remainder_and_signed_minimum_does_not_overflow(self):
        self.assertEqual(self.observe('ysm.is_close_eyes', [{'tick': 81, 'uuid_least': value}
                                                          for value in [0, 5, -5, 15, -(2 ** 63)]]), [0, 1, 1, 1, 0])

    def test_sleeping_keeps_eyes_closed_outside_the_blink_window(self):
        self.assertEqual(self.observe('ysm.is_close_eyes', [{'tick': 0, 'sleeping': True}, {'tick': 0},
                                                          {'tick': 50, 'sleeping': True}]), [1, 0, 1])

    def test_blink_uses_model_playback_ticks_separately_from_the_entity_tick_counter(self):
        self.assertEqual(self.observe('ysm.is_close_eyes', [{'tick': 86, 'render_ticks': 0},
                                                          {'tick': 0, 'render_ticks': 86},
                                                          {'tick': 1000, 'render_ticks': 90}]), [0, 1, 0])

    def test_first_yaw_observation_does_not_turn_absolute_facing_into_a_speed_spike(self):
        self.assertEqual(self.observe('q.yaw_speed', [{'tick': 5000, 'yaw': 725}]), [0])

    def test_yaw_speed_updates_on_entity_ticks_and_repeated_render_reads_are_stable(self):
        snapshots = [{'tick': t, 'yaw': y} for t, y in [(100, 10), (100, 12), (101, 20), (101, 24), (103, 40)]]
        self.assertEqual(self.observe('q.yaw_speed', snapshots), [0, 0, 200, 200, 200])

    def test_yaw_rewind_resets_its_baseline_and_unwrapped_rotation_keeps_its_direction(self):
        snapshots = [{'tick': t, 'yaw': y} for t, y in [(50, 350), (51, 370), (0, -10), (1, -20)]]
        self.assertEqual(self.observe('q.yaw_speed', snapshots), [0, 400, 0, -200])

    def test_bound_queries_remain_readonly_to_source_expressions(self):
        report = self.run_expr('q.health=99;', {'entity': {'health': 3.5}})
        self.assertEqual(report['failures'], 1)
        self.assertTrue(any('Read-only' in value for value in report['diagnostics']))

    def test_bound_inputs_drive_real_animation_channels(self):
        clips = {'idle': {'loop': True, 'bones': {'root': {'position': ['q.health/q.max_health*10', 'ysm.food_level',
                                                                                   'q.ground_speed'],
                                                              'rotation': [0, 'q.body_y_rotation', 0]}}}}
        report = self.animation(clips, context={'entity': {'health': 9.5, 'max_health': 20, 'food_level': 12.25,
                                                          'velocity': [3, 0, 4], 'body_yaw': 450}})
        self.assertEqual(report['pose']['root']['position'], [4.75, 12.25, 100])
        self.assertEqual(report['pose']['root']['rotation'], [0, 90, 0])
        self.assertEqual(report['evaluation_diagnostics'], [])

    def test_real_controller_changes_state_after_live_health_input_changes(self):
        states = {'default': {'animations': ['idle'], 'transitions': [{'hurt': 'q.health<q.max_health/2'}]},
                  'hurt': {'animations': ['hurt'], 'transitions': [{'default': 'q.health>=q.max_health/2'}]}}
        snapshots = [(0, 20), (0.2, 5.5), (0.4, 20)]
        frames = self.invoke({'player.main': {'states': states}}, {'idle': self.clip(1), 'hurt': self.clip(7)},
                             [{'time': t, 'entity': {'health': hp, 'max_health': 20}} for t, hp in snapshots])
        self.assertEqual([self.state(frame) for frame in frames], ['default', 'hurt', 'default'])
        self.assertEqual([frame['pose']['root']['position'][0] for frame in frames], [1, 7, 1])

    def test_events_and_deferred_handlers_read_the_same_current_host_snapshot(self):
        report = self.player({'@player_update': 'v.hp=q.health;ysm.defer(0);',
                              '@defer': 'v.food=ysm.food_level;'},
                             [{'time': t, 'entity': {'health': hp, 'food_level': food}}
                              for t, hp, food in [(0, 18.5, 16), (0.2, 7.5, 4)]])
        self.assertEqual([frame['variables']['variable.hp'] for frame in report['frames']], [18.5, 7.5])
        self.assertEqual([frame['variables']['variable.food'] for frame in report['frames']], [16, 4])

    def test_extreme_host_velocity_does_not_publish_nonfinite_query_values(self):
        report = self.run_expr('q.ground_speed+q.vertical_speed', {'entity': {'velocity': [1e308, 1e308, 1e308]}})
        self.assertEqual(report['value'], 0)
        self.assertEqual(report['failures'], 0)
        self.assertEqual(len(report['diagnostics']), 2)


if __name__ == '__main__':
    unittest.main()
