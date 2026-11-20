"""Pinned YSM Roaming ownership, named payloads and body/arm intersection."""
import json
import pathlib
import subprocess
import tempfile
import unittest

from test_molang import EXE


class RoamingTests(unittest.TestCase):
    def state(self, fields, operations, role='producer', accepted=True, local_only=False):
        run = subprocess.run([str(EXE), '--roaming-state', json.dumps(
            {'fields': fields, 'role': role, 'operations': operations, 'local_only': local_only})],
            capture_output=True, encoding='utf-8', timeout=5)
        self.assertEqual(run.returncode, 0 if accepted else 1, run.stderr)
        return json.loads(run.stdout)['steps'] if accepted else run.stderr

    def expression(self, source):
        run = subprocess.run([str(EXE), '--expr', source], capture_output=True, encoding='utf-8', timeout=5)
        self.assertEqual(run.returncode, 0, run.stderr)
        result = json.loads(run.stdout)
        self.assertEqual(result['failures'], 0, result)
        return result

    def pair(self, body, arm, frames, failures=0, accepted=True, initial=None, local_only=False):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'pair.json'
            path.write_text(json.dumps({'body': body, 'arm': arm}), encoding='utf-8')
            plan = {'frames': frames, 'local_only': local_only}
            if initial is not None:
                plan['initial_roaming'] = initial
            run = subprocess.run([str(EXE), '--paired-animation-file', str(path), json.dumps(plan)],
                                 capture_output=True, encoding='utf-8', timeout=5)
        self.assertEqual(run.returncode, 0 if accepted else 1, run.stderr)
        if not accepted:
            return run.stderr
        result = json.loads(run.stdout)
        self.assertEqual(result['expression_failures'], failures, result)
        return result['frames']

    @staticmethod
    def clip(source, bone='arm'):
        return {'loop': True, 'animation_length': 1, 'bones': {bone: {'rotation': [source, 0, 0]}}}

    def test_all_source_namespace_aliases_share_one_float_slot(self):
        result = self.expression('r.wheelRocate=2;roaming.wheelrocate+=3;v.roaming.wheelrocate+=4;'
                                 'variable.roaming.wheelrocate+=1;return r.wheelrocate;')
        self.assertEqual(result['value'], 10)
        self.assertEqual(result['variables'], {'variable.roaming.wheelrocate': 10})

    def test_roaming_assignment_returns_the_normalized_float_value(self):
        result = self.expression('v.captured=(r.shared=16777217);return v.captured;')
        self.assertEqual(result['value'], 16777216)
        self.assertEqual(result['variables']['variable.roaming.shared'], 16777216)
        self.assertEqual(self.expression('return r.unwritten??9;')['value'], 0)

    def test_snapshot_preserves_dirty_and_same_value_assignment_creates_another_delta(self):
        # RuntimeRoamingOwnershipTest's 7/9 fixture: non-consuming sample,
        # delta seal, then a legal same-value write creates a fresh batch.
        steps = self.state(['shared', 'body_only'], [
            {'op': 'write', 'name': 'shared', 'value': 7},
            {'op': 'write', 'name': 'body_only', 'value': 9},
            {'op': 'snapshot'}, {'op': 'delta'}, {'op': 'snapshot'},
            {'op': 'write', 'name': 'shared', 'value': 7}, {'op': 'delta'}, {'op': 'delta'}])
        self.assertTrue(steps[2]['state']['dirty'])
        self.assertEqual(steps[2]['result'], {'shared': 7, 'body_only': 9})
        self.assertEqual(steps[3]['result'], steps[2]['result'])
        self.assertFalse(steps[4]['state']['dirty'])
        self.assertEqual(steps[4]['result'], steps[2]['result'])
        self.assertEqual(steps[6]['result'], {'shared': 7})
        self.assertEqual(steps[7]['result'], {})

    def test_full_returns_current_values_and_clears_dirty(self):
        steps = self.state(['a'], [{'op': 'write', 'name': 'a', 'value': 4},
                                  {'op': 'full'}, {'op': 'delta'}])
        self.assertEqual(steps[1]['result'], {'a': 4})
        self.assertFalse(steps[1]['state']['dirty'])
        self.assertEqual(steps[2]['result'], {})

    def test_consumer_writes_stay_local_and_cannot_be_reported_as_producer(self):
        steps = self.state(['a'], [{'op': 'write', 'name': 'a', 'value': 11}, {'op': 'snapshot'},
                                  {'op': 'full'}, {'op': 'delta'}, {'op': 'read', 'name': 'a'}], 'consumer')
        self.assertIsNone(steps[0]['state']['dirty'])
        self.assertTrue(all('error' in step for step in steps[1:4]))
        self.assertEqual(steps[-1]['result'], 11)

    def test_producer_rejects_consumer_payloads_without_clearing_dirty(self):
        steps = self.state(['a'], [{'op': 'write', 'name': 'a', 'value': 7},
                                  {'op': 'apply_full', 'values': {'a': 99}},
                                  {'op': 'apply_patch', 'values': {'a': 99}}, {'op': 'delta'}])
        self.assertIn('error', steps[1])
        self.assertIn('error', steps[2])
        self.assertTrue(steps[2]['state']['dirty'])
        self.assertEqual(steps[3]['result'], {'a': 7})

    def test_patch_preserves_absent_fields_and_full_resets_them_to_zero(self):
        steps = self.state(['a', 'b'], [{'op': 'apply_full', 'values': {'a': 3, 'b': 9}},
            {'op': 'apply_patch', 'values': {'a': 5}}, {'op': 'apply_full', 'values': {'a': 7}},
            {'op': 'read', 'name': 'b'}], 'consumer')
        self.assertEqual(steps[1]['state']['values'], {'a': 5, 'b': 9})
        self.assertEqual(steps[2]['state']['values'], {'a': 7})
        self.assertEqual(steps[3]['result'], 0)

    def test_invalid_patch_and_full_never_publish_partial_updates(self):
        steps = self.state(['a', 'b'], [{'op': 'apply_full', 'values': {'a': 7, 'b': 11}},
            {'op': 'apply_patch', 'values': {'a': 99, 'unknown': 3}},
            {'op': 'apply_full', 'values': {'a': 99, 'b': 1e40}}], 'consumer')
        self.assertTrue(all('error' in step for step in steps[1:]))
        self.assertEqual(steps[-1]['state']['values'], {'a': 7, 'b': 11})

    def test_frozen_layout_rejects_new_fields_and_nonfinite_float_writes(self):
        steps = self.state(['a'], [{'op': 'write', 'name': 'a', 'value': 7},
            {'op': 'write', 'name': 'unknown', 'value': 9}, {'op': 'write', 'name': 'a', 'value': 1e40},
            {'op': 'read', 'name': 'unknown'}, {'op': 'delta'}])
        self.assertTrue(all('error' in step for step in steps[1:4]))
        self.assertEqual(steps[-1]['result'], {'a': 7})

    def test_layout_enforces_upstream_field_count_and_name_byte_limits(self):
        self.state(['n' + str(i) for i in range(64)], [])
        self.assertIn('64 fields', self.state(['n' + str(i) for i in range(65)], [], accepted=False))
        self.state(['a' * 32], [])
        for name in ['a' * 33, 'Upper', '0wrong', '头发', 'a.b', '']:
            with self.subTest(name=name):
                self.assertIn('field name', self.state([name], [], accepted=False))

    def test_local_only_layout_keeps_large_packages_bounded_without_wire_truncation(self):
        fields = ['n' + str(i) for i in range(200)]
        steps = self.state(fields, [{'op': 'write', 'name': 'n199', 'value': 7}, {'op': 'full'}],
                           local_only=True)
        self.assertEqual(set(steps[-1]['state']['fields']), set(fields))
        self.assertEqual(steps[-1]['result'], {'n199': 7})
        self.assertIn('1024 fields', self.state(['n' + str(i) for i in range(1025)], [],
                                               accepted=False, local_only=True))
        self.assertIn('64 fields', self.state(fields, [], accepted=False))

    def test_local_only_large_body_layout_projects_shared_values_to_arm(self):
        source = ';'.join('r.n' + str(i) + '=' + str(i) for i in range(96))
        body = {'animations': {'idle': self.clip(source + ';return 0;', 'body')}}
        arm = {'animations': {'parallel0': self.clip('r.n95')}}
        frames = self.pair(body, arm, [{'time': 0}], local_only=True)
        self.assertEqual(len(frames[0]['body_roaming']['fields']), 96)
        self.assertEqual(frames[0]['arm_roaming']['values']['n95'], 95)
        self.assertEqual(frames[0]['arm_pose']['arm']['rotation'][0], 95)

    def test_body_shared_delta_reaches_arm_without_copying_body_only_or_ordinary_variables(self):
        body = {'animations': {'idle': self.clip('r.shared', 'body')}, 'ysm_functions': {
            '@player_update': 'q.write?{r.shared=7;r.body_only=9;}:0;v.private=2;'}}
        arm = {'animations': {'parallel0': self.clip(
            'v.seen=r.shared;r.shared=99;r.arm_only=11;v.private=5;return v.seen;')}}
        frames = self.pair(body, arm, [
            {'time': 0, 'queries': {'q.write': 1}}, {'time': .2, 'queries': {'q.write': 0}},
            {'time': .4, 'queries': {'q.write': 1}}])
        self.assertEqual([f['arm_pose']['arm']['rotation'][0] for f in frames], [7, 99, 7])
        self.assertEqual([f['roaming_projected'] for f in frames], [True, False, True])
        self.assertEqual(frames[-1]['body_roaming']['values'], {'shared': 7, 'body_only': 9})
        self.assertEqual(frames[-1]['roaming_delta'], {'shared': 7, 'body_only': 9})
        self.assertIsNone(frames[1]['roaming_delta'])
        self.assertEqual(frames[-1]['arm_roaming']['values'], {'shared': 99, 'arm_only': 11})
        self.assertNotIn('body_only', frames[-1]['arm_roaming']['fields'])
        self.assertFalse(frames[-1]['body_roaming']['dirty'])
        self.assertIsNone(frames[-1]['arm_roaming']['dirty'])
        self.assertEqual(frames[-1]['body_variables']['variable.private'], 2)
        self.assertEqual(frames[-1]['arm_variables']['variable.private'], 5)

    def test_body_projection_updates_idle_arm_state_without_advancing_its_clock(self):
        body = {'animations': {'idle': self.clip('r.shared=q.input', 'body')}}
        arm = {'animations': {'parallel0': self.clip('r.shared')}}
        frames = self.pair(body, arm, [{'time': 0, 'queries': {'q.input': 7}},
            {'time': 1, 'first_person': False, 'queries': {'q.input': 9}},
            {'time': 2, 'first_person': False, 'queries': {'q.input': 11}},
            {'time': 3, 'queries': {'q.input': 13}}])
        self.assertEqual([f['arm_frame'] for f in frames], [1, 1, 1, 2])
        self.assertEqual([f['arm_time'] for f in frames], [0, 0, 0, 3])
        self.assertEqual(frames[2]['arm_roaming']['values'], {'shared': 11})
        self.assertEqual(frames[-1]['arm_pose']['arm']['rotation'][0], 13)

    def test_failed_body_round_retains_dirty_until_a_successful_normal_update(self):
        body = {'animations': {'idle': self.clip('q.fail?{r.shared=7;r.bad=1e40;}:0;return 0;', 'body')}}
        arm = {'animations': {'parallel0': self.clip('v.seen=r.shared;r.shared=99;return v.seen;')}}
        frames = self.pair(body, arm, [{'time': 0, 'queries': {'q.fail': 1}},
            {'time': .2, 'queries': {'q.fail': 0}}], failures=1)
        self.assertFalse(frames[0]['roaming_projected'])
        self.assertTrue(frames[0]['body_roaming']['dirty'])
        self.assertEqual(frames[0]['arm_pose']['arm']['rotation'][0], 0)
        self.assertTrue(frames[1]['roaming_projected'])
        self.assertEqual(frames[1]['arm_pose']['arm']['rotation'][0], 7)

    def test_arm_layout_includes_callable_defer_and_controller_but_excludes_body_event_routes(self):
        functions = {'@player_init': 'r.body_init=1;', '@player_update': 'r.body_update=2;',
            '@sync': 'r.sync_only=3;', 'unused': 'return r.callable;', '@defer': 'r.deferred=4;',
            '@fp_ctrl_arm_ctrl_misc': 'r.controller=5;return 5;'}
        frames = self.pair({'animations': {}, 'ysm_functions': functions},
            {'animations': {}, 'ysm_functions': functions}, [{'time': 0}])
        self.assertEqual(set(frames[0]['arm_roaming']['fields']), {'callable', 'deferred', 'controller'})
        self.assertTrue({'body_init', 'body_update', 'sync_only'} <= set(frames[0]['body_roaming']['fields']))

    def test_target_union_is_checked_before_any_animation_runs(self):
        body = {'animations': {'idle': self.clip(';'.join('r.b' + str(i) + '=1' for i in range(32)), 'body')}}
        arm = {'animations': {'parallel0': self.clip(';'.join('r.a' + str(i) + '=1' for i in range(33)))}}
        self.assertIn('64 fields', self.pair(body, arm, [{'time': 0}], accepted=False))

    def test_new_owners_do_not_inherit_mutable_state_from_an_identical_previous_pair(self):
        body = {'animations': {'idle': self.clip('q.write?{r.shared=7;}:0;return 0;', 'body')}}
        arm = {'animations': {'parallel0': self.clip('r.shared')}}
        first = self.pair(body, arm, [{'time': 0, 'queries': {'q.write': 1}}])
        replacement = self.pair(body, arm, [{'time': 0, 'queries': {'q.write': 0}}])
        self.assertEqual(first[0]['arm_pose']['arm']['rotation'][0], 7)
        self.assertEqual(replacement[0]['arm_pose']['arm']['rotation'][0], 0)

    def test_restoration_initializes_body_and_arm_before_the_first_program_runs(self):
        body = {'animations': {'idle': self.clip('v.body_seen=r.body_only;return r.shared;', 'body')}}
        arm = {'animations': {'parallel0': self.clip('v.local_before=r.arm_only;return r.shared;')}}
        frame = self.pair(body, arm, [{'time': 0}], initial={'shared': 9, 'body_only': 13})[0]
        self.assertEqual(frame['body_variables']['variable.body_seen'], 13)
        self.assertEqual(frame['arm_variables']['variable.local_before'], 0)
        self.assertEqual(frame['arm_pose']['arm']['rotation'][0], 9)
        self.assertEqual(frame['arm_roaming']['values'], {'shared': 9})
        self.assertFalse(frame['roaming_projected'])
        self.assertFalse(frame['body_roaming']['dirty'])

    def test_player_init_sees_the_restore_baseline_before_publishing_a_fresh_delta(self):
        body = {'animations': {'idle': self.clip('r.shared', 'body')},
                'ysm_functions': {'@player_init': 'v.initial_before=r.shared;r.shared+=1;'}}
        arm = {'animations': {'parallel0': self.clip('r.shared')}}
        frame = self.pair(body, arm, [{'time': 0}], initial={'shared': 9})[0]
        self.assertEqual(frame['body_variables']['variable.initial_before'], 9)
        self.assertEqual(frame['roaming_delta'], {'shared': 10})
        self.assertEqual(frame['arm_pose']['arm']['rotation'][0], 10)

    def test_restore_payload_uses_finite_float_slots_and_the_frozen_body_layout(self):
        body = {'animations': {'idle': self.clip('r.shared', 'body')}}
        arm = {'animations': {'parallel0': self.clip('r.shared')}}
        frame = self.pair(body, arm, [{'time': 0}], initial={'shared': 16777217})[0]
        self.assertEqual(frame['body_roaming']['values'], {'shared': 16777216})
        self.assertEqual(frame['arm_pose']['arm']['rotation'][0], 16777216)
        self.assertIn('payload field', self.pair(body, arm, [{'time': 0}],
                      initial={'unknown': 7}, accepted=False))
        self.assertIn('payload field', self.pair(body, arm, [{'time': 0}],
                      initial={'shared': 1e40}, accepted=False))

