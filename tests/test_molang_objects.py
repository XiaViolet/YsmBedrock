"""Records, source call recovery and object consumption across real entries."""
import unittest

import test_molang as shared
import test_model_functions as events
import test_render_observation as rendering


class MolangObjectTests(unittest.TestCase):
    run_expr = shared.MolangTests.run_expr
    value = shared.MolangTests.value

    def test_arithmetic_reads_each_record_at_its_source_position_across_nested_calls(self):
        source = "t.p=ysm.bone_rot('root');v.p=t.p;return 2*(t.p.x+fn.edit()+v.p.x)-t.p.x;"
        result = self.run_expr(source, {
            'functions': {'edit': 'v.p.x=7;t.p=v.p;t.p.x=9;return t.p.x;'},
            'bone_rotations': {'root': [3, 0, 0]},
        })
        self.assertEqual(result['failures'], 0, result)
        self.assertEqual(result['value'], 35)
        self.assertEqual(result['variables']['variable.p']['x'], 7)

    def test_intermediate_nonfinite_stops_before_later_arithmetic_side_effect(self):
        report = self.run_expr('v.big=1e200;return (v.big*v.big)*0+(v.after=7);')
        self.assertEqual(report['failures'], 1, report)
        self.assertEqual(report['value'], 0)
        self.assertNotIn('variable.after', report['variables'])

    def test_host_vector_fields_and_unknown_bone_are_present_zero_records(self):
        host = {'bone_rotations': {'Head': [17, -23, 41]}}
        self.assertEqual(self.value("ysm.bone_rot('Head')", host), {'x': 17, 'y': -23, 'z': 41})
        self.assertEqual(self.value("ysm.bone_rot('Head').x+ysm.bone_rot('Head').z", host), 58)
        self.assertEqual(self.value("ysm.bone_rot('missing')??7"), {'x': 0, 'y': 0, 'z': 0})
        self.assertEqual(self.value("ysm.bone_pivot_abs('head').x"), 0)

    def test_object_presence_and_numeric_projection_are_distinct(self):
        host = {'queries': {'q.empty': [], 'q.zero': [0, 0], 'q.record': {'x': 0, 'y': 0, 'z': 0}}}
        self.assertEqual(self.value('q.empty??7', host), [])
        self.assertEqual(self.value('q.record??7', host), {'x': 0, 'y': 0, 'z': 0})
        self.assertEqual(self.value('q.empty+q.zero+q.record', host), 2)
        self.assertEqual(self.value('(q.record==1)*10+(q.record==2)', host), 10)

    def test_record_values_compare_by_schema_and_field_contents(self):
        host = {'queries': {'q.a': {'x': 2, 'y': 0, 'z': 0}, 'q.b': {'x': 2, 'y': 0, 'z': 0},
                            'q.c': {'$schema': 'other', '$fields': {'x': 2, 'y': 0, 'z': 0}}}}
        self.assertEqual(self.value('(q.a==q.b)*10+(q.a==q.c)', host), 10)
        self.assertEqual(self.value('q.a==q.a.x', host), 0)

    def test_record_field_write_isolates_copies_and_preserves_unrelated_fields(self):
        report = self.run_expr("v.a=ysm.bone_rot('head');v.b=v.a;v.a.x=7;return v.b.x*10+v.a.x;",
                               {'bone_rotations': {'head': [2, 3, 4]}})
        self.assertEqual(report['failures'], 0, report)
        self.assertEqual(report['value'], 27)
        self.assertEqual(report['variables']['variable.a'], {'x': 7, 'y': 3, 'z': 4})
        self.assertEqual(report['variables']['variable.b'], {'x': 2, 'y': 3, 'z': 4})

    def test_captured_receiver_survives_rhs_rebinding(self):
        report = self.run_expr('v.a=q.old;v.alias=v.a;v.a.x=fn.change();return v.a.x+v.a.y+v.alias.x;',
            {'queries': {'q.old': {'x': 1, 'y': 3, 'z': 4}, 'q.new': {'x': 9, 'y': 20, 'z': 30}},
             'functions': {'change': 'v.a=q.new;return 2;'}})
        self.assertEqual(report['failures'], 0, report)
        self.assertEqual(report['value'], 6)
        self.assertEqual(report['variables']['variable.a'], {'x': 2, 'y': 3, 'z': 4})

    def test_compound_assignment_captures_previous_value_before_rhs(self):
        self.assertEqual(self.value('v.x=1;v.x+=(v.x=10);return v.x;'), 11)
        self.assertEqual(self.value("v.a=ysm.bone_rot('head');v.a.x+=(v.a.x=10);return v.a.x;",
                                    {'bone_rotations': {'head': [1, 2, 3]}}), 11)

    def test_temporary_provider_field_write_runs_rhs_without_mutating_provider(self):
        report = self.run_expr("ysm.bone_rot('head').x=(v.called=8);return ysm.bone_rot('head').x;",
                               {'bone_rotations': {'head': [1, 2, 3]}})
        self.assertEqual(report['failures'], 0, report)
        self.assertEqual(report['variables']['variable.called'], 8)
        self.assertEqual(report['value'], 1)

    def test_missing_record_write_runs_rhs_then_fails_without_publishing(self):
        report = self.run_expr('v.missing.x=(v.side=3);return 9;')
        self.assertEqual(report['failures'], 1, report)
        self.assertEqual(report['variables']['variable.side'], 3)
        self.assertNotIn('variable.missing', report['variables'])

    def test_readonly_array_element_is_immutable_and_copy_becomes_writable(self):
        host = {'queries': {'q.items': [{'x': 1, 'y': 2, 'z': 3}]}}
        self.assertEqual(self.value('v.a=q.items[0];v.a.x=9;return q.items[0].x*10+v.a.x;', host), 19)
        bad = self.run_expr('q.items[0].x=7;', host)
        self.assertEqual(bad['failures'], 1)

    def test_user_functions_forward_records_and_preserve_local_isolation(self):
        report = self.run_expr("v.a=fn.copy(ysm.bone_rot('head'));return v.a.z;",
            {'bone_rotations': {'head': [2, 3, 5]}, 'functions': {'copy': 't.a=args[0];t.a.z+=2;return t.a;'}})
        self.assertEqual(report['failures'], 0, report)
        self.assertEqual(report['value'], 7)

    def test_unknown_and_wrong_arity_calls_discard_argument_side_effects(self):
        for source in ['unknown(v.side=1)', 'fn.missing(v.side=2)', 'math.nope(v.side=3)',
                       'math.sin(v.side=4,v.later=5)', 'ysm.first_order(v.side=6)']:
            with self.subTest(source=source):
                report = self.run_expr(source + ';v.after=7;return v.after;')
                self.assertEqual(report['failures'], 0, report)
                self.assertEqual(report['variables'], {'variable.after': 7})

    def test_proven_string_mismatch_discards_entire_call(self):
        report = self.run_expr("ysm.first_order((v.name=2),(v.later=3));return 7;")
        self.assertEqual(report['failures'], 0, report)
        self.assertEqual(report['variables'], {})
        self.assertEqual(report['value'], 7)

    def test_bare_external_call_uses_zero_argument_arity_without_caching(self):
        report = self.run_expr('v.a=math.sin??17;v.b=ysm.bone_rot??23;return v.a+v.b;')
        self.assertEqual(report['failures'], 0, report)
        self.assertEqual(report['value'], 40)
        self.assertEqual(self.value('math.pi==math.pi()'), 1)
        self.assertEqual(self.value('q.health+q.health()', {'entity': {'health': 7}}), 14)

    def test_missing_string_parameter_uses_empty_string_instead_of_skipping_provider(self):
        for source in ['null', 'v.missing', '0', '(0)', 'false']:
            with self.subTest(source=source):
                self.assertEqual(self.value('ysm.mod_version(' + source + ')??7', {'mod_versions': {'': 'empty-id'}}), 'empty-id')
                self.assertEqual(self.value('ysm.first_order(' + source + ',2)??9'), 0)
        for source in ['+0', '-0', '1-1', 'v.zero']:
            with self.subTest(source=source):
                self.assertIsNone(self.value('ysm.mod_version(' + source + ')', {'variables': {'v.zero': 0}}))

    def test_scalar_object_consumption_does_not_change_source_presence(self):
        host = {'variables': {'v.a': []}, 'functions': {'pass': 'return args[0];'}}
        report = self.run_expr('v.scalar=math.abs(fn.pass(v.a));return v.a??7;', host)
        self.assertEqual(report['value'], [])
        self.assertEqual(report['variables']['variable.scalar'], 0)

    def test_roaming_names_remain_scalar_storage_and_keep_model_authored_spelling(self):
        report = self.run_expr('v.roaming.wheelRocate+=2;v.roaming.choice=3;return v.roaming.wheelRocate+v.roaming.choice;')
        self.assertEqual(report['failures'], 0, report)
        self.assertEqual(report['value'], 5)
        self.assertEqual(report['variables']['variable.roaming.wheelrocate'], 2)
        self.assertNotIn('variable.roaming', report['variables'])
        self.assertEqual(self.value('v.roaming.unwritten??7'), 0)

    def test_roaming_uses_float_slots_and_object_scalar_projection(self):
        report = self.run_expr("v.roaming.a=16777217;v.roaming.b='0';v.roaming.c=q.empty;return v.roaming.a+v.roaming.b;",
                               {'queries': {'q.empty': []}})
        self.assertEqual(report['failures'], 0, report)
        self.assertEqual(report['variables']['variable.roaming.a'], 16777216)
        self.assertEqual(report['variables']['variable.roaming.b'], 1)
        self.assertEqual(report['variables']['variable.roaming.c'], 0)

    def test_roaming_overflow_does_not_publish_a_partial_write(self):
        report = self.run_expr('v.roaming.a=7;v.roaming.a=1e40;return 9;')
        self.assertEqual(report['failures'], 1, report)
        self.assertEqual(report['variables']['variable.roaming.a'], 7)

    def test_roaming_observation_does_not_write_back_to_owner(self):
        checker = rendering.RenderObservationTests()
        frames = checker.run_player([{'time': 0}, {'time': 0, 'observe': True}, {'time': .2}],
            {'idle': checker.clip('v.roaming.n+=1;return v.roaming.n;')})
        self.assertEqual(frames[0]['variables']['variable.roaming.n'], 1)
        self.assertEqual(frames[1]['variables']['variable.roaming.n'], 2)
        self.assertEqual(frames[2]['variables']['variable.roaming.n'], 2)

    def test_defer_accepts_record_and_preserves_original_value(self):
        checker = events.ModelFunctionTests()
        frame = checker.player({'@player_init': "v.a=ysm.bone_rot('missing');ysm.defer(v.a);v.a.x=9;",
                                '@defer': 'v.payload=args[0];'}, [{'time': 0}])['frames'][0]
        self.assertEqual(frame['variables']['variable.payload'], 1)
        self.assertEqual(frame['variables']['variable.a']['x'], 9)

    def test_completed_bone_pose_is_available_to_next_update(self):
        checker = events.ModelFunctionTests()
        frames = checker.player({'@player_update': "v.before=ysm.bone_rot('root');"},
            [{'time': 0}, {'time': 0.2}, {'time': 0.4}],
            {'idle': {'loop': True, 'bones': {'root': {'rotation': [7, 11, 13]}}}})['frames']
        self.assertEqual(frames[0]['variables']['variable.before'], {'x': 0, 'y': 0, 'z': 0})
        self.assertEqual(frames[1]['pose']['root']['rotation'], [7, 11, 13])
        self.assertEqual(frames[2]['variables']['variable.before'], {'x': 7, 'y': 11, 'z': 13})

    def test_bone_rotation_includes_geometry_rotation_before_and_after_animation(self):
        host = {'geometry': {'minecraft:geometry': [{'bones': [{'name': 'root', 'rotation': [10, 20, 30]}]}]},
                'frames': [{'time': 0}, {'time': .2}, {'time': .4}]}
        import json
        import pathlib
        import subprocess
        import tempfile
        document = {'animations': {'idle': {'loop': True, 'bones': {'root': {'rotation': [7, 11, 13]}}}},
                    'ysm_functions': {'@player_update': "v.before=ysm.bone_rot('root');"}}
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'rotation.json'
            path.write_text(json.dumps(document), encoding='utf-8')
            run = subprocess.run([str(shared.EXE), '--source-player-animation-file', str(path), json.dumps(host)],
                                 capture_output=True, encoding='utf-8', timeout=5)
        self.assertEqual(run.returncode, 0, run.stderr)
        result = json.loads(run.stdout)
        self.assertEqual(result['expression_failures'], 0, result)
        self.assertEqual(result['frames'][0]['variables']['variable.before'], {'x': 10, 'y': 20, 'z': 30})
        self.assertEqual(result['frames'][2]['variables']['variable.before'], {'x': 17, 'y': 31, 'z': 43})

    def test_independent_readonly_error_is_not_swallowed_as_string_mismatch(self):
        report = self.run_expr('ysm.first_order((q.a=2),(v.later=3));return 7;')
        self.assertEqual(report['failures'], 1, report)
        self.assertEqual(report['variables'], {})

    def test_string_mismatch_stops_before_a_later_independent_error(self):
        report = self.run_expr('ysm.first_order(2,(q.bad=3));return 7;')
        self.assertEqual(report['failures'], 0, report)
        self.assertEqual(report['value'], 7)

    def test_variables_are_not_callables_and_discarded_calls_still_parse_syntax(self):
        report = self.run_expr('v.a=1;v.a(v.side=2);return 7;')
        self.assertEqual(report['failures'], 1, report)
        self.assertNotIn('variable.side', report['variables'])

    def test_observation_record_writes_are_private_and_do_not_change_next_owner_update(self):
        checker = rendering.RenderObservationTests()
        frames = checker.run_player([{'time': 0}, {'time': 0, 'observe': True}, {'time': 0.2}],
            {'idle': checker.clip('v.a.x+=1;return v.a.x;')},
            functions={'@player_init': "v.a=ysm.bone_rot('missing');"})
        self.assertEqual(frames[0]['variables']['variable.a']['x'], 1)
        self.assertEqual(frames[1]['variables']['variable.a']['x'], 2)
        self.assertEqual(frames[2]['variables']['variable.a']['x'], 2)


if __name__ == '__main__':
    unittest.main(verbosity=2)
