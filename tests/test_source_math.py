"""Float boundaries and failure behavior of actual pinned Java math methods."""
import math
import struct
import unittest

from math_java_vectors import VECTORS
import test_molang as shared


class SourceMathTests(unittest.TestCase):
    run_expr = shared.MolangTests.run_expr
    value = shared.MolangTests.value

    def test_reference_reads_follow_the_pinned_methods_except_observed_release_differences(self):
        for vector in VECTORS:
            source = 'math.%s(%s)' % (vector['function'], ','.join(map(repr, vector['args'])))
            with self.subTest(source=source):
                report = self.run_expr(source)
                if vector['function'] == 'sqrt' and vector['args'][0] < 0:
                    # The frozen development method throws. The 2.6.5 release produces
                    # zero and continues the script (MathDomainLab, 0.29.5).
                    self.assertEqual(vector['error'], 'java.lang.ArithmeticException')
                    self.assertEqual((report['failures'], report['value']), (0, 0))
                elif vector['error']:
                    self.assertEqual(report['failures'], 1, report)
                    self.assertEqual(report['value'], 0)
                else:
                    self.assertEqual(report['failures'], 0, report)
                    if vector['function'] in ['sin', 'cos', 'sqrt', 'pow', 'exp']:
                        self.assertTrue(math.isclose(report['value'], vector['value'], rel_tol=2e-7, abs_tol=1e-8), report)
                    else:
                        bits = struct.unpack('!I', struct.pack('!f', report['value']))[0]
                        self.assertEqual(bits, vector['bits'], report)

    def test_release_negative_root_preserves_the_rest_of_the_program(self):
        report = self.run_expr('v.s=math.sqrt(q.x);return (v.s==0)+(v.s==v.s)*2+(v.s+2==2)*4;',
                               {'frames': [{'queries': {'q.x': -4}}, {'queries': {'q.x': 9}}]})
        self.assertEqual(report['values'], [7, 2])
        self.assertEqual(report['failures'], 0, report)

    def test_argument_projection_keeps_nonempty_strings_numeric_one(self):
        self.assertEqual(self.value("math.clamp('0',false,true)"), 1)
        self.assertEqual(self.value("math.abs('')"), 0)

    def test_float_overflow_is_local_and_does_not_poison_later_frames(self):
        report = self.run_expr('v.before=7;return math.pow(q.x,3);',
                               {'frames': [{'queries': {'q.x': 1e20}}, {'queries': {'q.x': 2}}]})
        self.assertEqual(report['values'], [0, 8])
        self.assertEqual(report['variables']['variable.before'], 7)
        self.assertEqual(report['failures'], 1)

    def test_finite_double_input_that_overflows_a_float_parameter_is_rejected(self):
        report = self.run_expr('math.abs(1e40)')
        self.assertEqual(report['failures'], 1)
        self.assertTrue(any('math argument' in item for item in report['diagnostics']))


if __name__ == '__main__':
    unittest.main()
