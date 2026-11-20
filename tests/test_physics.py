"""Numerical and lifecycle behavior, including values run from actual upstream Java."""
import json
import math
import pathlib
import subprocess
import tempfile
import unittest

import test_molang as shared
from physics_java_vectors import REFERENCE


class PhysicsTests(unittest.TestCase):
    value = shared.MolangTests.value
    animation = shared.MolangTests.animation

    def run_expr(self, source, context=None):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'expression.json'
            path.write_text(json.dumps({'source': source, 'context': context or {}}), encoding='utf-8')
            run = subprocess.run([str(shared.EXE), '--expr-file', str(path)], capture_output=True, encoding='utf-8', timeout=10)
        self.assertEqual(run.returncode, 0, run.stderr)
        return json.loads(run.stdout)

    def test_pinned_java_reference_26_scenarios_and_279_individual_reads(self):
        self.assertEqual(len(REFERENCE['scenarios']), 26)
        reads = 0
        for case in REFERENCE['scenarios']:
            with self.subTest(name=case['name']):
                report = self.run_expr(case['source'], {'frames': case['frames'], 'trace_variables': True})
                self.assertEqual(report['failures'], 0, report)
                self.assertEqual(report['diagnostics'], [], report)
                for frame, expected in zip(report['frame_variables'], case['expected']):
                    for column, number in enumerate(expected):
                        actual = frame['variable.o%d' % column]
                        self.assertTrue(math.isclose(actual, number, rel_tol=2e-6, abs_tol=2e-6),
                                        (case['name'], actual, number))
                        reads += 1
                self.assertEqual(len(report['frame_variables']), len(case['expected']))
        self.assertEqual(reads, 279)

    def test_first_read_returns_input_and_a_repeated_read_returns_the_unstepped_simulation(self):
        report = self.run_expr("v.first=ysm.first_order('hair',7);return ysm.first_order('hair',9);", {'delta_time': 0})
        self.assertEqual(report['variables']['variable.first'], 7)
        self.assertEqual(report['value'], 0)

    def test_zero_clock_establishes_a_positive_baseline_before_advancing(self):
        report = self.run_expr("ysm.first_order('hair',10)",
                               {'frames': [{'delta_time': step} for step in [0, 0, 0.1, 0.1]]})
        self.assertEqual(report['values'], [10, 0, 0, 1])

    def test_named_state_is_shared_across_user_function_calls(self):
        source = "v.a=fn.read(7);v.b=fn.read(9);return v.a-v.b;"
        report = self.run_expr(source, {'functions': {'read': "return ysm.first_order('hair',args[0]);"},
                                        'frames': [{'delta_time': 0.1} for _ in range(3)]})
        self.assertEqual(report['values'], [7, 0, 0])

    def test_empty_names_do_not_create_a_simulation(self):
        report = self.run_expr("ysm.first_order('',7)+ysm.second_order('',9)",
                               {'frames': [{'delta_time': 0.1} for _ in range(4)]})
        self.assertEqual(report['values'], [0, 0, 0, 0])
        self.assertEqual(report['diagnostics'], [])

    def test_minimum_and_maximum_argument_counts_are_enforced(self):
        for source in ["ysm.first_order()", "ysm.second_order('hair')",
                       "ysm.first_order('hair'," + ','.join(['1'] * 255) + ')']:
            with self.subTest(source=source[:70]):
                report = self.run_expr(source)
                self.assertEqual(report['failures'], 0)
                self.assertTrue(any('2..255' in item for item in report['diagnostics']))

    def test_names_are_strings_and_all_numeric_arguments_are_finite_float32(self):
        self.assertIsNone(self.run_expr("ysm.first_order(1,2)")['value'])
        self.assertEqual(self.run_expr("ysm.second_order('hair','2')")['value'], 1)
        for source in ["ysm.second_order('hair',1e39)", "ysm.first_order('hair',1,1e39)",
                       "ysm.second_order('hair',1,1,1,1,1e39)"]:
            with self.subTest(source=source):
                report = self.run_expr(source)
                self.assertEqual(report['failures'], 1)
                self.assertEqual(report['value'], 0)

    def test_absent_numeric_inputs_use_the_runtime_zero_projection(self):
        report = self.run_expr("ysm.second_order('hair',ysm.head_pitch,2,0.6,0)")
        self.assertEqual(report['value'], 0)
        self.assertEqual(report['failures'], 0)
        self.assertEqual(report['diagnostics'], [])

    def test_unrepresentable_physics_clock_does_not_break_unrelated_animation_programs(self):
        self.assertEqual(self.run_expr('2+3', {'frames': [{'physics_time': 1e300}]})['value'], 5)
        report = self.run_expr("ysm.first_order('hair',7)",
                               {'frames': [{'physics_time': 1e300}, {'physics_time': 0.1}]})
        self.assertEqual(report['values'], [0, 7])
        self.assertEqual(report['failures'], 1)
        self.assertTrue(any('physics clock' in item for item in report['diagnostics']))

    def test_unused_trailing_coefficients_accept_source_scalar_projection(self):
        report = self.run_expr("ysm.first_order('hair',1,1,1,1,'bad')")
        self.assertEqual(report['failures'], 0)
        self.assertEqual(report['value'], 1)

    def test_zero_frequency_retains_the_source_simulation_instead_of_following_input(self):
        report = self.run_expr("ysm.second_order('hair',10,0,0.6,1)",
                               {'frames': [{'physics_time': time} for time in [1, 1.1, 1.2, 1.3]]})
        self.assertEqual(report['values'], [10, 0, 0, 0])

    def test_nonfinite_first_order_simulation_is_diagnosed_when_read(self):
        report = self.run_expr("ysm.first_order('hair',10,0)",
                               {'frames': [{'physics_time': 1}, {'physics_time': 1.1}]})
        self.assertEqual(report['values'], [10, 0])
        self.assertEqual(report['failures'], 1)
        self.assertTrue(any('Non-finite physics' in item for item in report['diagnostics']))

    def test_excessive_step_work_fails_locally_and_normal_clock_steps_can_resume(self):
        report = self.run_expr("ysm.second_order('hair',10,2,0.6,1)",
                               {'frames': [{'physics_time': time} for time in [1, 100000, 100000.05, 100000.1]]})
        self.assertEqual(report['failures'], 1)
        self.assertTrue(any('budget' in item for item in report['diagnostics']))
        self.assertEqual(report['values'][:2], [10, 0])
        self.assertTrue(all(math.isfinite(value) for value in report['values']))
        self.assertGreater(report['values'][-1], 0)

    def test_physics_advances_on_model_time_across_base_animation_changes(self):
        clips = {name: {'loop': True, 'bones': {'root': {'position': ["ysm.first_order('hair',10)", 0, 0]}}}
                 for name in ['idle', 'walk']}
        report = self.animation(clips, context={'samples': [{'time': time, 'state': state, 'state_time': 0}
                                                            for time, state in [(0, 'idle'), (0.1, 'idle'), (0.2, 'walk')]]})
        self.assertEqual(report['pose']['root']['position'][0], 1)
        self.assertEqual(report['evaluation_diagnostics'], [])

    def test_name_capacity_failure_preserves_existing_states_for_later_frames(self):
        source = 'q.create?{' + ''.join("ysm.first_order('state%d',1);" % i for i in range(4097))
        source += "};return ysm.first_order('state0',2);"
        report = self.run_expr(source, {'frames': [{'physics_time': 1, 'queries': {'q.create': 1}},
                                                  {'physics_time': 1.1, 'queries': {'q.create': 0}}]})
        self.assertEqual(report['failures'], 1)
        self.assertTrue(any('name budget' in item for item in report['diagnostics']))
        self.assertAlmostEqual(report['values'][-1], 0.1, places=6)

    def test_total_substep_budget_is_bounded_across_many_independent_names(self):
        source = '+'.join("ysm.second_order('hair%d',10,5,1,1)" % i for i in range(40))
        times = [1, 151, 151.05] + [151.06 + index * 0.01 for index in range(95)]
        report = self.run_expr(source, {'frames': [{'physics_time': time} for time in times]})
        self.assertEqual(report['failures'], 1)
        self.assertTrue(any('step budget' in item for item in report['diagnostics']))
        self.assertTrue(all(math.isfinite(value) for value in report['values']))
        self.assertGreater(report['values'][2], 350)
        self.assertLess(report['values'][2], 400)  # Skipped simulations resume from their retained state.
        self.assertAlmostEqual(report['values'][-1], 400, places=2)


if __name__ == '__main__':
    unittest.main()
