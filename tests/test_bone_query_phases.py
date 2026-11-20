"""Head history ordering from pinned AnimatableEntity/CustomHumanoidEntity."""
import json
import pathlib
import subprocess
import tempfile
import unittest

from test_import import EXE


class BoneQueryPhaseTests(unittest.TestCase):
    def player(self, frames, functions=None, clips=None, geometry=None):
        document = {'animations': clips or {'idle': {'loop': True, 'bones': {
            'Head': {'rotation': [7, 11, 13]}, 'Head2': {'rotation': [2, 3, 4]},
            'head': {'rotation': [5, 6, 7]}}}}, 'ysm_functions': functions or {}}
        geometry = geometry or {'minecraft:geometry': [{'bones': [
            {'name': 'Head', 'rotation': [10, 20, 30]}, {'name': 'Head2'}, {'name': 'head'}]}]}
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'head-history.json'
            path.write_text(json.dumps(document), encoding='utf-8')
            result = subprocess.run([str(EXE), '--source-player-animation-file', str(path),
                                     json.dumps({'geometry': geometry, 'frames': frames})],
                                    capture_output=True, encoding='utf-8', timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report['expression_failures'], 0, report)
        self.assertEqual(report['compile_diagnostics'], [], report)
        self.assertEqual(report['evaluation_diagnostics'], [], report)
        return report['frames']

    @staticmethod
    def frame(time, pitch=-30, yaw=20, **options):
        return dict({'time': time, 'queries': {'ysm.head_pitch': pitch, 'ysm.head_yaw': yaw}}, **options)

    def test_update_recovers_authored_head_before_scripts_and_keeps_initial_rotation(self):
        frames = self.player([self.frame(t) for t in [0, .2, .4]],
                             {'@player_update': "v.head=ysm.bone_rot('Head');v.second=ysm.bone_rot('Head2');v.lower=ysm.bone_rot('head');"})
        self.assertEqual(frames[1]['variables']['variable.head'], {'x': 10, 'y': 20, 'z': 30})
        self.assertEqual(frames[2]['variables']['variable.head'], {'x': 17, 'y': 31, 'z': 43})
        self.assertEqual(frames[2]['variables']['variable.second'], {'x': 2, 'y': 3, 'z': 4})
        self.assertEqual(frames[2]['variables']['variable.lower'], {'x': 5, 'y': 6, 'z': 7})
        self.assertEqual(frames[2]['pose']['Head']['rotation'], [37, -9, 13])

    def test_scale_read_uses_predecessor_pose_and_starts_at_unit_scale(self):
        clips = {'idle': {'bones': {'Head': {'scale': [2, 3, 4]}}}}
        functions = {'@player_update': "v.scale=ysm.bone_scale('Head');v.dt=ysm.time_delta;"}
        frames = self.player([self.frame(0), self.frame(.25), self.frame(.75)], functions, clips)
        self.assertEqual(frames[0]['variables']['variable.scale'], {'x': 1, 'y': 1, 'z': 1})
        self.assertEqual(frames[1]['variables']['variable.scale'], {'x': 2, 'y': 3, 'z': 4})
        self.assertEqual([frame['variables']['variable.dt'] for frame in frames], [0, .25, .5])

    def test_pivot_projects_the_previous_pose_through_parents(self):
        geometry = {'minecraft:geometry': [{'bones': [
            {'name': 'root', 'pivot': [2, 1, 0]}, {'name': 'tip', 'parent': 'root', 'pivot': [5, 1, 0]}]}]}
        functions = {'@player_update': "v.absolute=ysm.bone_pivot_abs('tip');v.local=ysm.bone_pos('tip');"}
        clips = {'idle': {'bones': {'root': {'position': [3, 4, 0], 'scale': [2, 1, 1], 'rotation': [0,0,90]},
                                   'tip': {'position': [1, 0, 2]}}}}
        frames = self.player([self.frame(0), self.frame(.2), self.frame(.4)], functions, clips, geometry)
        self.assertEqual(frames[0]['variables']['variable.absolute'], {'x': 5, 'y': 1, 'z': 0})
        self.assertEqual(frames[2]['variables']['variable.local'], {'x': 1, 'y': 0, 'z': 2})
        absolute = frames[2]['variables']['variable.absolute']
        for axis, value in {'x': 5, 'y': -3, 'z': 2}.items():
            self.assertAlmostEqual(absolute[axis], value)

    def test_update_defer_drains_before_first_controller_but_later_ingress_sees_drawn_head(self):
        frames = self.player([self.frame(0), self.frame(.2), self.frame(.4, sync_events=[[1]])],
                             {'@player_update': "v.before=ysm.bone_rot('Head');ysm.defer(1);",
                              '@defer': "v.deferred=ysm.bone_rot('Head');",
                              '@sync': "v.after=ysm.bone_rot('Head');"})
        self.assertEqual(frames[2]['variables']['variable.before'], {'x': 17, 'y': 31, 'z': 43})
        # Evaluation.setControllerContext drains null-animation captures before
        # the first controller; post-render ingress reads the committed draw.
        self.assertEqual(frames[2]['variables']['variable.deferred'], {'x': 17, 'y': 31, 'z': 43})
        self.assertEqual(frames[2]['variables']['variable.after'], {'x': 47, 'y': 11, 'z': 43})

    def test_controller_and_keyframe_queries_read_recovered_head_without_self_feedback(self):
        clips = {'idle': {'loop': True, 'bones': {'Head': {'rotation': [7, 11, 13]}}},
                 'parallel0': {'loop': True, 'bones': {
                     'head': {'position': ["ysm.bone_rot('Head').x", 0, 0]}}}}
        frames = self.player([self.frame(t) for t in [0, .2, .4, .6]],
                             {'@player_ctrl_pre_main': "v.ctrl=ysm.bone_rot('Head');"}, clips)
        self.assertEqual(frames[2]['variables']['variable.ctrl'], {'x': 17, 'y': 31, 'z': 43})
        self.assertEqual([f['pose']['head']['position'][0] for f in frames], [10, 10, 17, 17])

    def test_observation_queries_recover_head_and_do_not_change_logical_owner(self):
        clips = {'idle': {'loop': True, 'bones': {'Head': {'rotation': [7, 11, 13]}}},
                 'parallel0': {'loop': True, 'bones': {'head': {
                     'position': ["ysm.bone_rot('Head').x", 0, 0]}}}}
        frames = self.player([self.frame(0), self.frame(.2), self.frame(.4),
                              self.frame(900, pitch=15, yaw=-10, observe=True), self.frame(.6)], clips=clips)
        self.assertEqual(frames[3]['pose']['head']['position'][0], 17)
        self.assertEqual(frames[3]['pose']['Head']['rotation'], [-8, 21, 13])
        self.assertEqual(frames[4]['pose']['head']['position'][0], 17)
        self.assertEqual(frames[4]['controllers']['coded']['player.parallel_0']['generation'], 1)

    def test_same_time_redraw_does_not_leave_camera_angles_in_next_update(self):
        frames = self.player([self.frame(0), self.frame(.2), self.frame(.2, pitch=50, yaw=-40), self.frame(.4)],
                             {'@player_update': "v.before=ysm.bone_rot('Head');"})
        self.assertEqual(frames[2]['pose']['Head']['rotation'], [-43, 51, 13])
        self.assertEqual(frames[3]['variables']['variable.before'], {'x': 17, 'y': 31, 'z': 43})


if __name__ == '__main__':
    unittest.main()
