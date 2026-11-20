import json
import math
import pathlib
import subprocess
import tempfile
import unittest

PROJECT = pathlib.Path(__file__).resolve().parents[1]
EXE = PROJECT / 'build/windows/x64/release/ysm-inspect.exe'
if not EXE.exists():
    EXE = PROJECT.parent / 'ysm-inspect.exe'

class MolangTests(unittest.TestCase):
    def run_expr(self, source, context=None):
        result = subprocess.run([str(EXE), '--expr', source, json.dumps(context or {})], capture_output=True, encoding='utf-8', timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def value(self, source, context=None):
        report = self.run_expr(source, context)
        self.assertEqual(report['diagnostics'], [])
        return report['value']

    def animation(self, clips, time=0, context=None):
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder) / 'animations.json'
            path.write_text(json.dumps({'animations': clips}), encoding='utf-8')
            result = subprocess.run([str(EXE), '--animation-file', str(path), 'idle', str(time), json.dumps(context or {})], capture_output=True, encoding='utf-8', timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)

    def test_degrees_and_arithmetic_precedence(self):
        self.assertAlmostEqual(self.value('math.sin(90)+2*3'), 7)
        self.assertAlmostEqual(self.value('math.asin(1)'), 90)
        self.assertAlmostEqual(self.value('math.atan2(1,0)'), 90)

    def test_ysm_lerp_extrapolates(self):
        self.assertEqual(self.value('math.lerp(10,20,1.5)'), 25)

    def test_case_and_namespace_aliases(self):
        self.assertEqual(self.value('V.X=Q.Input*2; return variable.x;', {'queries': {'query.input': 3}}), 6)

    def test_nested_and_omitted_ternary(self):
        self.assertEqual(self.value('0?3:1?4:5'), 4)
        self.assertEqual(self.value('0?3'), 0)
        self.assertEqual(self.value('1?3'), 3)

    def test_short_circuit_does_not_mutate_other_branch(self):
        report = self.run_expr('v.x=0; 0&&(v.x=9); 1||(v.x=8); return v.x;')
        self.assertEqual(report['value'], 0)
        self.assertEqual(report['variables']['variable.x'], 0)

    def test_null_coalescing_preserves_zero_and_empty_string(self):
        self.assertEqual(self.value('v.x=0; return v.x??7;'), 0)
        self.assertEqual(self.value('v.x??7'), 7)
        self.assertEqual(self.value("''??'fallback'"), '')

    def test_ysm_string_projection_and_case(self):
        self.assertEqual(self.value("'0'+2"), 3)
        self.assertEqual(self.value("'A'=='a'"), 0)

    def test_persistent_variables_and_temporary_scope(self):
        frames = {'frames': [{}, {}, {}]}
        self.assertEqual(self.run_expr('v.x=(v.x??0)+1; return v.x;', frames)['values'], [1, 2, 3])
        self.assertEqual(self.run_expr('t.x=(t.x??0)+1; return t.x;', frames)['values'], [1, 1, 1])

    def test_return_and_complex_expression_result(self):
        report = self.run_expr('v.x=2; return v.x; v.x=99;')
        self.assertEqual(report['value'], 2)
        self.assertEqual(report['variables']['variable.x'], 2)
        self.assertEqual(self.value('v.x=2;'), 0)

    def test_single_value_expression_keeps_its_result_with_trailing_semicolon(self):
        for source, expected in [('12.5;', 12.5), ('math.sin(90);', 1),
                                 ('(1?(-167.20517))+(0?(-60))+(0?(-20));', -167.20517),
                                 ('(0?(-167.20517))+(1?(-60))+(0?(-20));', -60),
                                 ('(0?(-167.20517))+(0?(-60))+(1?(-20));', -20)]:
            with self.subTest(source=source):
                self.assertAlmostEqual(self.value(source), expected)

    def test_multiple_statements_still_require_an_explicit_return(self):
        self.assertEqual(self.value('v.x=2;v.x+3;'), 0)
        self.assertEqual(self.value('v.x=2;return v.x+3;'), 5)
        report = self.run_expr('1?(v.x=3);')
        self.assertEqual(report['value'], 0)
        self.assertEqual(report['variables']['variable.x'], 3)

    def test_loop_break_and_continue(self):
        source = 'v.i=0; v.sum=0; loop(10,{v.i+=1; v.i==2?continue; v.i==5?break; v.sum+=v.i;}); return v.sum;'
        self.assertEqual(self.value(source), 8)

    def test_execution_budget_is_local_failure(self):
        report = self.run_expr('loop(10000000,{v.x+=1;}); return v.x;')
        self.assertEqual(report['value'], 0)
        self.assertEqual(report['failures'], 1)
        self.assertTrue(any('budget' in d for d in report['diagnostics']))

    def test_read_only_query_cannot_be_assigned(self):
        report = self.run_expr('q.input=99;', {'queries': {'q.input': 2}})
        self.assertEqual(report['failures'], 1)
        self.assertTrue(any('Read-only' in d for d in report['diagnostics']))

    def test_non_finite_math_is_diagnosed(self):
        report = self.run_expr('math.abs(3.5e38)')
        self.assertEqual(report['value'], 0)
        self.assertEqual(report['failures'], 1)

    def test_square_root_of_negative_is_zero_and_execution_continues(self):
        for argument, expected in [(-9, 0), (0, 0), (9, 3)]:
            with self.subTest(argument=argument):
                result = self.run_expr('v.s=math.sqrt(q.argument);v.after=v.s+2;return v.s;',
                                       {'queries': {'q.argument': argument}})
                self.assertEqual(result['value'], expected)
                self.assertEqual(result['variables']['variable.after'], expected + 2)
                self.assertEqual(result['failures'], 0)

    def test_invalid_expression_rejected(self):
        for source in ['(1+2', 'NaN-2', '(' * 140 + '1' + ')' * 140]:
            result = subprocess.run([str(EXE), '--expr', source], capture_output=True, encoding='utf-8', timeout=5)
            self.assertEqual(result.returncode, 1, source)

    def test_second_order_converges_and_stays_finite(self):
        frames = [{'delta_time': 1/60} for _ in range(180)]
        report = self.run_expr("ysm.second_order('hair',10,2,0.6,0)", {'frames': frames})
        self.assertEqual(report['diagnostics'], [])
        self.assertTrue(all(math.isfinite(v) and abs(v) < 30 for v in report['values']))
        self.assertAlmostEqual(report['values'][-1], 10, delta=0.02)
        self.assertEqual(report['values'][0], 10)  # Source creation returns the input before its simulation starts.

    def test_second_order_repeated_read_does_not_advance_twice_after_creation(self):
        source = "ysm.second_order('hair',10,2,0.6,0)-ysm.second_order('hair',10,2,0.6,0)"
        self.assertEqual(self.run_expr(source, {'frames': [{'delta_time': 0.1} for _ in range(20)]})['values'], [10] + [0]*19)

    def test_zero_frequency_has_finite_result(self):
        self.assertEqual(self.value("ysm.second_order('hair',10,0,0.6,0)", {'delta_time': 0.1}), 10)

    def test_timeline_crosses_every_event_and_loop_boundary(self):
        clips = {'idle': {'loop': True}, 'parallel0': {'loop': True, 'animation_length': 1, 'timeline': {'0': ['v.n+=1;'], '0.5': ['v.n+=10;'], '1': ['v.n+=100;']}, 'bones': {'root': {'rotation': ['v.n',0,0]}}}}
        report = self.animation(clips, 1.1, {'samples': [0, 0.25, 0.5, 1, 1.1, 1.1]})
        self.assertEqual(report['pose']['root']['rotation'], [112,0,0])
        self.assertEqual(report['evaluation_diagnostics'], [])

    def test_parallel_timeline_survives_base_state_change(self):
        clips = {'idle': {'loop': True}, 'walk': {'loop': True}, 'parallel0': {'loop': True, 'animation_length': 1, 'timeline': {'0': 'v.n+=1;', '0.5': 'v.n+=10;'}, 'bones': {'root': {'position': ['v.n',0,0]}}}}
        samples = [{'state': 'idle', 'time': 0.75}, {'state': 'walk', 'time': 1.1, 'state_time': 0.01}]
        self.assertEqual(self.animation(clips, context={'samples': samples})['pose']['root']['position'], [12,0,0])

    def test_base_reactivation_replays_time_zero_instruction(self):
        clips = {'idle': {'loop': True, 'timeline': {'0': 'v.n+=1;'}}, 'walk': {'loop': True}}
        samples = [{'state': state, 'time': i, 'state_time': 0} for i,state in enumerate(['idle','walk','idle','idle'])]
        report = self.animation(clips, context={'samples': samples})
        self.assertEqual(report['variables']['variable.n'], 2)

    def test_shared_clip_has_independent_controller_timeline_cursors(self):
        clips = {'idle': {}, 'shared': {'loop': True, 'timeline': {'0': 'v.n+=1;'}}}
        layers = [{'name': 'shared', 'time': 0, 'timeline_id': owner} for owner in ['first', 'second']]
        report = self.animation(clips, context={'samples': [0, 0], 'layers': layers})
        self.assertEqual(report['variables']['variable.n'], 2)

    def test_explicit_length_and_loop_end_instruction_flush(self):
        clips = {'idle': {'loop': True}, 'parallel0': {'loop': True, 'animation_length': 0.01,
            'timeline': {'0': 'v.n+=1;', '0.0101': 'v.n+=10;'},
            'bones': {'root': {'position': {'0': [0,0,0], '0.02': [20,0,0]}}}}}
        report = self.animation(clips, context={'samples': [0,0.005,0.015]})
        self.assertEqual(report['variables']['variable.n'], 12)
        # Source F32 seconds -> F32 ticks introduces at most a few F32
        # ulps here; the terminal instruction/loop assertions stay exact.
        self.assertAlmostEqual(report['pose']['root']['position'][0], 5, delta=1e-6)

    def test_coded_head_locator_tracks_after_authored_pose(self):
        names = ['Head', 'Head12', 'MHead', 'Headwear', 'head_extra']
        clips = {'idle': {'loop': True, 'bones': {n: {'rotation': [5,6,7]} for n in names}}}
        context = {'queries': {'ysm.head_pitch': -30, 'ysm.head_yaw': 45},
            'geometry': {'minecraft:geometry': [{'bones': [{'name': n} for n in names]}]}}
        pose = self.animation(clips, context=context)['pose']
        self.assertEqual(pose['Head']['rotation'], [35,-39,7])
        self.assertEqual(pose['Head12']['rotation'], [35,-39,7])
        for name in ['MHead','Headwear','head_extra']:
            self.assertEqual(pose[name]['rotation'], [5,6,7])

    def test_ysm_layer_replacement_and_parallel_rotation_addition(self):
        clips = {
            'pre_parallel0': {'loop': True, 'bones': {'root': {'rotation': [10,0,0], 'position': [10,0,0], 'scale': 0}}},
            'idle': {'loop': True, 'bones': {'root': {'rotation': [20,0,0], 'position': [2,0,0], 'scale': 1}}},
            'parallel0': {'loop': True, 'bones': {'root': {'rotation': [5,0,0], 'position': [3,0,0], 'scale': 0.9}}}}
        pose = self.animation(clips)['pose']['root']
        self.assertEqual(pose['rotation'], [25,0,0])
        self.assertEqual(pose['position'], [3,0,0])
        self.assertEqual(pose['scale'], [0.9,0.9,0.9])

    def test_extra_layer_reveals_prop_and_does_not_erase_unrelated_bones(self):
        clips = {'pre_parallel0': {'loop': True, 'bones': {'prop': {'scale': 0}}},
            'idle': {'loop': True, 'bones': {'body': {'rotation': [10,0,0]}}},
            'extra0': {'loop': 'hold_on_last_frame', 'animation_length': 1, 'bones': {'prop': {'scale': 1}}}}
        pose = self.animation(clips, 2, {'layers': [{'name': 'extra0', 'time': 2}]})['pose']
        self.assertEqual(pose['prop']['scale'], [1,1,1])
        self.assertEqual(pose['body']['rotation'], [10,0,0])
        stopped = self.animation(clips, 2)['pose']
        self.assertEqual(stopped['prop']['scale'], [0,0,0])

    def test_scalar_channel_expression_evaluated_once(self):
        clips = {'idle': {'loop': True, 'bones': {'root': {'scale': 'v.n+=1'}}}}
        self.assertEqual(self.animation(clips)['pose']['root']['scale'], [1,1,1])

    def test_pre_post_keyframe_discontinuity(self):
        clips = {'idle': {'loop': True, 'animation_length': 1, 'bones': {'root': {'position': {'0': {'pre': [0,0,0], 'post': [10,0,0]}, '1': [20,0,0]}}}}}
        self.assertEqual(self.animation(clips, 0.5)['pose']['root']['position'], [15,0,0])

    def test_catmullrom_curve_uses_neighboring_keys(self):
        keys = {str(i): {'post': [v,0,0], 'lerp_mode': 'catmullrom'} for i,v in enumerate([0,10,20,0])}
        clips = {'idle': {'loop': True, 'bones': {'root': {'position': keys}}}}
        self.assertAlmostEqual(self.animation(clips, 1.5)['pose']['root']['position'][0], 16.875)

    def test_once_and_hold_last_frame_are_distinct(self):
        clip = {'animation_length': 1, 'bones': {'root': {'position': {'0': [0,0,0], '1': [10,0,0]}}}}
        self.assertNotIn('root', self.animation({'idle': clip}, 2)['pose'])
        clip['loop'] = 'hold_on_last_frame'
        self.assertEqual(self.animation({'idle': clip}, 2)['pose']['root']['position'], [10,0,0])

    def test_bad_axis_does_not_remove_other_axes(self):
        clips = {'idle': {'loop': True, 'bones': {'head': {'rotation': ['NaN-1', 3, 4]}}}}
        report = self.animation(clips)
        self.assertEqual(report['pose']['head']['rotation'], [0,3,4])
        self.assertEqual(report['unsupported_expression_channels'], 1)

    def test_head_queries_are_applied_to_channels(self):
        clips = {'idle': {'loop': True, 'bones': {'head': {'rotation': ['ysm.head_pitch','math.clamp(ysm.head_yaw,-60,60)',0]}}}}
        context = {'queries': {'ysm.head_pitch': 30, 'ysm.head_yaw': 90}}
        self.assertEqual(self.animation(clips, context=context)['pose']['head']['rotation'], [30,60,0])

if __name__ == '__main__':
    unittest.main(verbosity=2)
