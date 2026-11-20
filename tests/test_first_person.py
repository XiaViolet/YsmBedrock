"""The separate fp.arm controller collection and paired logical owners."""
import json
import pathlib
import subprocess
import tempfile
import unittest

from test_molang import EXE


class FirstPersonTests(unittest.TestCase):
    @staticmethod
    def clip(value, channel='rotation', bone='arm', **extra):
        return dict({'loop': True, 'animation_length': 1, 'bones': {bone: {channel: [value, 0, 0]}}}, **extra)

    def run_frames(self, clips, frames, controllers=None, functions=None, geometry=None):
        document = {'animations': clips, 'animation_controllers': controllers or {}, 'ysm_functions': functions or {}}
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'fp.json'
            path.write_text(json.dumps(document), encoding='utf-8')
            plan = {'frames': frames}
            if geometry is not None:
                plan['geometry'] = geometry
            run = subprocess.run([str(EXE), '--first-person-animation-file', str(path), json.dumps(plan)],
                                 capture_output=True, encoding='utf-8', timeout=5)
        self.assertEqual(run.returncode, 0, run.stderr)
        result = json.loads(run.stdout)
        self.assertEqual(result['expression_failures'], 0, result)
        self.assertEqual(result['compile_diagnostics'], [], result)
        self.assertTrue(all(frame['observation_failures'] == 0 for frame in result['frames']), result)
        return result['frames']

    def pair(self, body, arm, frames):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'pair.json'
            path.write_text(json.dumps({'body': body, 'arm': arm}), encoding='utf-8')
            run = subprocess.run([str(EXE), '--paired-animation-file', str(path), json.dumps({'frames': frames})],
                                 capture_output=True, encoding='utf-8', timeout=5)
        self.assertEqual(run.returncode, 0, run.stderr)
        result = json.loads(run.stdout)
        self.assertEqual(result['expression_failures'], 0, result)
        self.assertEqual(result['body_compile_diagnostics'], [], result)
        self.assertEqual(result['arm_compile_diagnostics'], [], result)
        return result['frames']

    def test_fp_collection_ignores_body_idle_pre_parallel_and_extra(self):
        clips = {name: self.clip(17) for name in ['idle', 'pre_parallel0', 'hold_mainhand:empty', 'extra0']}
        frames = self.run_frames(clips, [{'time': 0}, {'time': .2}])
        self.assertEqual(frames[-1]['pose'], {})
        self.assertEqual(frames[-1]['controllers']['coded'], {})

    def test_fp_parallel_layers_add_rotation_and_replace_other_channels(self):
        clips = {'parallel0': self.clip(7), 'parallel1': self.clip(3),
                 'parallel2': self.clip(9, channel='position'), 'parallel3': self.clip(4, channel='position')}
        frame = self.run_frames(clips, [{'time': 0}])[0]
        self.assertEqual(frame['pose']['arm']['rotation'], [10, 0, 0])
        self.assertEqual(frame['pose']['arm']['position'], [4, 0, 0])
        self.assertIn('fp.arm.parallel_0', frame['controllers']['coded'])

    def test_misc_source_controller_runs_before_parallel(self):
        controllers = {'fp.arm.misc': {'states': {'default': {'animations': ['chosen']}}}}
        frame = self.run_frames({'chosen': self.clip(10), 'parallel0': self.clip(4)}, [{'time': 0}], controllers)[0]
        self.assertEqual(frame['pose']['arm']['rotation'], [14, 0, 0])

    def test_body_controllers_are_not_entered_in_fp_owner(self):
        controllers = {'player.main': {'states': {'default': {'on_entry': 'v.wrong=1;', 'animations': ['body']}}}}
        frame = self.run_frames({'body': self.clip(50), 'parallel0': self.clip(2)}, [{'time': 0}], controllers)[0]
        self.assertNotIn('variable.wrong', frame['variables'])
        self.assertNotIn('player.main', frame['controllers']['states'])
        self.assertEqual(frame['pose']['arm']['rotation'], [2, 0, 0])

    def test_body_init_and_update_are_not_dispatched_for_fp(self):
        functions = {'@player_init': 'v.body_init+=1;', '@player_update': 'v.body_update+=1;'}
        frames = self.run_frames({'parallel0': self.clip(2)}, [{'time': 0}, {'time': .2}], functions=functions)
        self.assertEqual(frames[-1]['variables'], {})

    def test_canonical_fp_coded_override_keeps_own_scope_and_commits_actions(self):
        functions = {'@fp_ctrl_arm_ctrl_misc': "v.calls+=1;ctrl.set_animation('chosen',10);return 2;"}
        frames = self.run_frames({'chosen': self.clip(6)}, [{'time': 0}, {'time': .2}, {'time': .2}, {'time': .4}], functions=functions)
        self.assertEqual(frames[-1]['pose']['arm']['rotation'], [6, 0, 0])
        self.assertEqual(frames[-1]['variables']['variable.calls'], 3)
        self.assertEqual(frames[-1]['controllers']['coded']['fp.arm.misc']['selected_animation'], 'chosen')

    def test_fp_parallel_override_keeps_additive_rotation(self):
        functions = {'@fp_ctrl_arm_ctrl_parallel_0': "ctrl.set_animation('other',10);return 2;"}
        controllers = {'fp.arm.misc': {'states': {'default': {'animations': ['base']}}}}
        frame = self.run_frames({'base': self.clip(10), 'parallel0': self.clip(4), 'other': self.clip(7)},
            [{'time': 0}, {'time': .2}], controllers, functions)[-1]
        self.assertEqual(frame['pose']['arm']['rotation'], [17, 0, 0])

    def test_fp_source_parallel_controller_is_not_coded_additive(self):
        controllers = {'fp.arm.misc': {'states': {'default': {'animations': ['base']}}},
                       'fp.arm.parallel_0': {'states': {'default': {'animations': ['other']}}}}
        frame = self.run_frames({'base': self.clip(10), 'other': self.clip(7)}, [{'time': 0}], controllers)[0]
        self.assertEqual(frame['pose']['arm']['rotation'], [7, 0, 0])

    def test_fp_timeline_and_deferred_callbacks_have_private_clocks(self):
        clip = self.clip('v.count')
        clip['timeline'] = {'0': 'v.count+=1;ysm.defer(5);', '.5': 'v.count+=10;'}
        frames = self.run_frames({'parallel0': clip}, [{'time': 0}, {'time': .7}],
                                functions={'@defer': 'v.payload=args[0];'})
        self.assertEqual(frames[-1]['variables']['variable.count'], 11)
        self.assertEqual(frames[-1]['variables']['variable.payload'], 5)

    def test_fp_observation_has_no_timeline_or_predicate_updates(self):
        clip = self.clip('ysm.rendering_in_inventory?45:7')
        clip['timeline'] = {'0': 'v.events+=1;', '.5': 'v.events+=10;'}
        frames = self.run_frames({'parallel0': clip},
            [{'time': 0}, {'time': .2}, {'time': 20, 'observe': True, 'queries': {'ysm.rendering_in_inventory': 1}}, {'time': .7}])
        self.assertEqual(frames[2]['pose']['arm']['rotation'][0], 45)
        self.assertEqual(frames[2]['owner_frame'], frames[1]['owner_frame'])
        self.assertEqual(frames[2]['logical_variables'], frames[1]['logical_variables'])
        self.assertEqual(frames[-1]['variables']['variable.events'], 11)

    def test_fp_does_not_apply_the_body_head_locator_pass(self):
        geometry = {'minecraft:geometry': [{'bones': [{'name': 'Head'}]}]}
        frame = self.run_frames({'parallel0': self.clip(5, bone='Head')},
            [{'time': 0, 'queries': {'ysm.head_pitch': 30, 'ysm.head_yaw': 40}}], geometry=geometry)[0]
        self.assertEqual(frame['pose']['Head']['rotation'], [5, 0, 0])

    def test_fp_cold_observation_does_not_start_a_controller(self):
        frames = self.run_frames({'parallel0': self.clip(7)}, [{'time': 0, 'observe': True}, {'time': .2}])
        self.assertEqual(frames[0]['pose'], {})
        self.assertEqual(frames[0]['owner_frame'], 0)
        self.assertEqual(frames[1]['owner_frame'], 1)

    def test_fp_forces_loop_for_parallel_even_if_file_says_once(self):
        frame = self.run_frames({'parallel0': self.clip('q.anim_time', loop=False)}, [{'time': 0}, {'time': 1.25}])[-1]
        self.assertEqual(frame['pose']['arm']['rotation'][0], .25)

    def test_paired_body_and_arm_keep_ordinary_variables_separate(self):
        functions = {'@player_init': 'v.private=7;v.init+=1;', '@player_update': 'v.update+=1;'}
        frames = self.pair({'animations': {'idle': self.clip(1)}, 'ysm_functions': functions},
            {'animations': {'parallel0': self.clip('v.private??23')}, 'ysm_functions': functions},
            [{'time': 0}, {'time': .2}])
        self.assertEqual(frames[-1]['body_variables']['variable.private'], 7)
        self.assertEqual(frames[-1]['arm_pose']['arm']['rotation'][0], 23)
        self.assertNotIn('variable.init', frames[-1]['arm_variables'])

    def test_first_fp_render_begins_at_its_own_zero_lifetime(self):
        frames = self.pair({'animations': {'idle': self.clip(1)}},
            {'animations': {'parallel0': self.clip('q.life_time')}},
            [{'time': 0, 'first_person': False}, {'time': 3, 'first_person': True}, {'time': 3.2, 'first_person': True}])
        self.assertEqual(frames[0]['arm_frame'], 0)
        self.assertEqual(frames[1]['arm_time'], 0)
        self.assertAlmostEqual(frames[2]['arm_time'], .2)

    def test_third_person_gap_does_not_execute_arm_and_return_flushes_elapsed_events(self):
        clip = self.clip('v.n')
        clip['animation_length'] = 10
        clip['timeline'] = {'0': 'v.n+=1;', '.5': 'v.n+=10;', '1': 'v.n+=100;'}
        frames = self.pair({'animations': {'idle': self.clip(1)}}, {'animations': {'parallel0': clip}},
            [{'time': 0}, {'time': .2, 'first_person': False}, {'time': .8, 'first_person': False}, {'time': 1.2}])
        self.assertEqual([frame['arm_frame'] for frame in frames], [1, 1, 1, 2])
        self.assertEqual(frames[-1]['arm_variables']['variable.n'], 111)

    def test_identical_physics_names_do_not_share_body_and_arm_simulations(self):
        body = {'animations': {'parallel0': self.clip("ysm.first_order('shared',3)", channel='position')}}
        arm = {'animations': {'parallel0': self.clip("ysm.first_order('shared',100)", channel='position')}}
        frames = self.pair(body, arm, [{'time': 0, 'first_person': False}, {'time': 3}, {'time': 3.2}])
        self.assertEqual(frames[1]['arm_pose']['arm']['position'][0], 100)
        self.assertEqual(frames[-1]['body_physics_states'], 1)
        self.assertEqual(frames[-1]['arm_physics_states'], 1)
        self.assertLess(frames[-1]['body_pose']['arm']['position'][0], 10)


if __name__ == '__main__':
    unittest.main(verbosity=2)
