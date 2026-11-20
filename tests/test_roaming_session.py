"""LocalPlayer restore baselines and exact-installation ownership from pinned YSM."""
import json
import subprocess
import unittest

from test_molang import EXE


class RoamingSessionTests(unittest.TestCase):
    @staticmethod
    def install(model='a', identity='one', fields=None, local=True, session='main'):
        return [{'op': 'prepare', 'model': model, 'id': identity, 'fields': fields or ['known'],
                 'local_only': local, 'session': session},
                {'op': 'activate', 'ticket': identity, 'session': session}]

    def run_steps(self, steps, errors=()):
        run = subprocess.run([str(EXE), '--roaming-session', json.dumps({'operations': steps})],
                             capture_output=True, encoding='utf-8', timeout=8)
        self.assertEqual(run.returncode, 0, run.stderr)
        frames = json.loads(run.stdout)['frames']
        self.assertEqual({i for i, frame in enumerate(frames) if 'error' in frame}, set(errors), frames)
        return frames

    def test_large_local_layout_restores_after_reinstall_but_wire_baseline_remains_limited(self):
        fields = ['n' + str(i) for i in range(130)]
        steps = self.install(fields=fields) + [
            {'op': 'normal', 'id': 'one', 'source': 'r.n129=11;'},
            {'op': 'snapshot', 'model': 'a', 'id': 'one'}, {'op': 'end', 'id': 'one'}]
        steps += self.install(identity='two', fields=fields)
        frames = self.run_steps(steps)
        self.assertEqual(frames[3]['values'], {'n129': 11})
        self.assertEqual(frames[5]['initial_values'], {'n129': 11})
        frames = self.run_steps([{'op': 'first_full', 'model': 'remote',
                                 'values': {name: 0 for name in fields}}], errors=(0,))
        self.assertIn('64 fields', frames[0]['error'])

    def test_local_baseline_exists_only_after_successful_publication(self):
        steps = [{'op': 'prepare', 'model': 'a', 'id': 'failed', 'fields': ['known'], 'local_only': True},
                 {'op': 'snapshot', 'model': 'a'}, {'op': 'drop', 'ticket': 'failed'}]
        steps += self.install()
        steps += [{'op': 'snapshot', 'model': 'a', 'id': 'one'}]
        frames = self.run_steps(steps)
        self.assertFalse(frames[1]['available'])
        self.assertEqual(frames[0]['namespaces'], 0)
        self.assertTrue(frames[-1]['available'])
        self.assertEqual(frames[-1]['values'], {})
        self.assertTrue(frames[-1]['exact_installation'])

    def test_temporary_runtime_sampling_and_updates_cannot_claim_a_baseline(self):
        steps = self.install(local=False) + [
            {'op': 'normal', 'id': 'one', 'source': 'r.known=9;'},
            {'op': 'snapshot', 'model': 'a', 'id': 'one'},
            {'op': 'first_full', 'model': 'a', 'values': {'known': 1}},
            {'op': 'adopt', 'id': 'one', 'values': {'known': 99}},
            {'op': 'snapshot', 'model': 'a', 'id': 'one'}]
        steps += self.install(identity='restored', local=False)
        frames = self.run_steps(steps)
        self.assertFalse(frames[2]['adopted'])
        self.assertFalse(frames[3]['available'])
        self.assertTrue(frames[4]['reload'])
        self.assertFalse(frames[5]['adopted'])
        self.assertEqual(frames[6]['values'], {'known': 1})
        self.assertFalse(frames[6]['exact_installation'])
        self.assertEqual(frames[7]['initial_values'], {'known': 1})

    def test_adopted_values_survive_model_hide_and_reenable(self):
        steps = self.install() + [{'op': 'normal', 'id': 'one', 'source': 'r.known=3;v.private=9;'},
            {'op': 'end', 'id': 'one'}, {'op': 'snapshot', 'model': 'a'}]
        steps += self.install(identity='two')
        steps += [{'op': 'normal', 'id': 'two', 'source': 'return v.private??7;'}]
        frames = self.run_steps(steps)
        self.assertEqual(frames[4]['values'], {'known': 3})
        self.assertFalse(frames[4]['exact_installation'])
        self.assertEqual(frames[5]['initial_values'], {'known': 3})
        self.assertEqual(frames[-1]['result'], 7)

    def test_namespaces_stay_separate_when_switching_away_and_back(self):
        steps = self.install() + [{'op': 'normal', 'id': 'one', 'source': 'r.known=7;'}]
        steps += self.install(model='b', identity='two')
        steps += [{'op': 'normal', 'id': 'two', 'source': 'r.known=11;'}]
        steps += self.install(identity='three')
        steps += [{'op': 'snapshot', 'model': 'b'}]
        frames = self.run_steps(steps)
        self.assertEqual(frames[3]['initial_values'], {})
        self.assertEqual(frames[6]['initial_values'], {'known': 7})
        self.assertEqual(frames[-1]['values'], {'known': 11})

    def test_old_installation_cannot_update_an_identical_replacement(self):
        steps = self.install() + [{'op': 'normal', 'id': 'one', 'source': 'r.known=7;'}]
        steps += self.install(identity='two')
        steps += [{'op': 'adopt', 'id': 'one', 'values': {'known': 99}},
                  {'op': 'end', 'id': 'one'},
                  {'op': 'normal', 'id': 'two', 'source': 'r.known=11;'},
                  {'op': 'snapshot', 'model': 'a'}]
        frames = self.run_steps(steps)
        self.assertFalse(frames[5]['adopted'])
        self.assertTrue(frames[7]['adopted'])
        self.assertEqual(frames[-1]['values'], {'known': 11})

    def test_sampling_live_action_state_does_not_retain_it_before_a_normal_update(self):
        steps = self.install() + [{'op': 'action', 'id': 'one', 'source': 'r.known=9;'},
            {'op': 'snapshot', 'model': 'a', 'id': 'one'}, {'op': 'end', 'id': 'one'},
            {'op': 'snapshot', 'model': 'a'}]
        frames = self.run_steps(steps)
        self.assertEqual(frames[3]['values'], {'known': 9})
        self.assertTrue(frames[3]['exact_installation'])
        self.assertEqual(frames[5]['values'], {})

    def test_action_write_waits_until_the_next_successful_normal_update(self):
        frames = self.run_steps(self.install() + [
            {'op': 'action', 'id': 'one', 'source': 'r.known=9;'},
            {'op': 'snapshot', 'model': 'a'}, {'op': 'normal', 'id': 'one'},
            {'op': 'snapshot', 'model': 'a'}])
        self.assertEqual(frames[3]['values'], {})
        self.assertEqual(frames[4]['delta'], {'known': 9})
        self.assertEqual(frames[5]['values'], {'known': 9})

    def test_observation_cannot_write_back_or_dirty_the_logical_owner(self):
        frames = self.run_steps(self.install() + [
            {'op': 'normal', 'id': 'one', 'source': 'r.known=7;'},
            {'op': 'observe', 'id': 'one', 'source': 'r.known=99;'},
            {'op': 'normal', 'id': 'one'}, {'op': 'snapshot', 'model': 'a', 'id': 'one'}])
        self.assertEqual(frames[3]['variables']['variable.roaming.known'], 99)
        self.assertEqual(frames[4]['delta'], {})
        self.assertEqual(frames[-1]['values'], {'known': 7})

    def test_failed_normal_round_does_not_publish_partial_updates(self):
        frames = self.run_steps(self.install(fields=['known', 'bad']) + [
            {'op': 'normal', 'id': 'one', 'source': 'r.known=7;r.bad=1e40;'},
            {'op': 'snapshot', 'model': 'a'}, {'op': 'normal', 'id': 'one'},
            {'op': 'snapshot', 'model': 'a'}])
        self.assertEqual(frames[2]['failures'], 1)
        self.assertNotIn('adopted', frames[2])
        self.assertEqual(frames[3]['values'], {})
        self.assertEqual(frames[5]['values'], {'known': 7})

    def test_ordinary_self_echo_keeps_the_local_baseline(self):
        frames = self.run_steps(self.install() + [
            {'op': 'normal', 'id': 'one', 'source': 'r.known=2;'},
            {'op': 'first_full', 'model': 'a', 'values': {'known': 1}},
            {'op': 'end', 'id': 'one'}, {'op': 'snapshot', 'model': 'a'}])
        self.assertFalse(frames[3]['reload'])
        self.assertEqual(frames[-1]['values'], {'known': 2})

    def test_incompatible_layout_leaves_the_previous_baseline_and_installation_intact(self):
        frames = self.run_steps(self.install() + [
            {'op': 'normal', 'id': 'one', 'source': 'r.known=7;'},
            {'op': 'prepare', 'model': 'a', 'id': 'bad', 'fields': ['other'], 'local_only': True},
            {'op': 'snapshot', 'model': 'a', 'id': 'one'}], errors=[3])
        self.assertEqual(frames[-1]['values'], {'known': 7})
        self.assertTrue(frames[-1]['exact_installation'])

    def test_abandoned_candidate_does_not_replace_the_current_installation(self):
        frames = self.run_steps(self.install() + [
            {'op': 'normal', 'id': 'one', 'source': 'r.known=7;'},
            {'op': 'prepare', 'model': 'a', 'id': 'two', 'fields': ['known'], 'local_only': True},
            {'op': 'drop', 'ticket': 'two'}, {'op': 'normal', 'id': 'one', 'source': 'r.known=11;'},
            {'op': 'snapshot', 'model': 'a', 'id': 'one'}])
        self.assertTrue(frames[5]['adopted'])
        self.assertEqual(frames[-1]['values'], {'known': 11})

    def test_candidate_cannot_overwrite_newer_same_value_writes(self):
        frames = self.run_steps(self.install() + [
            {'op': 'normal', 'id': 'one', 'source': 'r.known=7;'},
            {'op': 'prepare', 'model': 'a', 'id': 'two', 'fields': ['known'], 'local_only': True},
            {'op': 'normal', 'id': 'one', 'source': 'r.known=7;'},
            {'op': 'activate', 'ticket': 'two'}, {'op': 'snapshot', 'model': 'a', 'id': 'one'}], errors=[5])
        self.assertTrue(frames[-1]['exact_installation'])
        self.assertEqual(frames[-1]['values'], {'known': 7})

    def test_noop_adoption_does_not_make_a_valid_preparation_stale(self):
        frames = self.run_steps(self.install() + [
            {'op': 'prepare', 'model': 'a', 'id': 'two', 'fields': ['known'], 'local_only': True},
            {'op': 'normal', 'id': 'one'}, {'op': 'activate', 'ticket': 'two'},
            {'op': 'snapshot', 'model': 'a', 'id': 'two'}])
        self.assertTrue(frames[-1]['exact_installation'])

    def test_prepared_candidate_and_old_delta_are_invalid_after_disconnect(self):
        frames = self.run_steps(self.install() + [
            {'op': 'normal', 'id': 'one', 'source': 'r.known=7;'},
            {'op': 'prepare', 'model': 'a', 'id': 'two', 'fields': ['known'], 'local_only': True},
            {'op': 'disconnect'}, {'op': 'activate', 'ticket': 'two'},
            {'op': 'adopt', 'id': 'one', 'values': {'known': 99}},
            {'op': 'snapshot', 'model': 'a'}], errors=[5])
        self.assertFalse(frames[6]['adopted'])
        self.assertFalse(frames[-1]['available'])

    def test_retained_namespace_does_not_borrow_an_unrelated_live_installation(self):
        steps = self.install() + [{'op': 'normal', 'id': 'one', 'source': 'r.known=7;'}]
        steps += self.install(model='b', identity='two')
        steps += [{'op': 'normal', 'id': 'two', 'source': 'r.known=11;'},
                  {'op': 'snapshot', 'model': 'a', 'id': 'two'}]
        frames = self.run_steps(steps)
        self.assertEqual(frames[-1]['values'], {'known': 7})
        self.assertFalse(frames[-1]['exact_installation'])

    def test_named_clear_keeps_known_empty_and_revokes_old_eligibility(self):
        frames = self.run_steps(self.install() + [
            {'op': 'normal', 'id': 'one', 'source': 'r.known=7;'},
            {'op': 'clear', 'model': 'a'}, {'op': 'snapshot', 'model': 'a', 'id': 'one'},
            {'op': 'adopt', 'id': 'one', 'values': {'known': 99}}])
        self.assertTrue(frames[3]['reload'])
        self.assertTrue(frames[4]['available'])
        self.assertEqual(frames[4]['values'], {})
        self.assertFalse(frames[4]['exact_installation'])
        self.assertFalse(frames[5]['adopted'])

    def test_clear_of_a_different_model_does_not_keep_the_old_baseline(self):
        frames = self.run_steps(self.install() + [
            {'op': 'normal', 'id': 'one', 'source': 'r.known=7;'},
            {'op': 'clear', 'model': 'b'}, {'op': 'snapshot', 'model': 'a'},
            {'op': 'snapshot', 'model': 'b'}])
        self.assertFalse(frames[4]['available'])
        self.assertTrue(frames[5]['available'])
        self.assertEqual(frames[5]['values'], {})

    def test_invalid_adoption_and_invalid_first_full_do_not_create_partial_baselines(self):
        frames = self.run_steps(self.install() + [
            {'op': 'normal', 'id': 'one', 'source': 'r.known=7;'},
            {'op': 'adopt', 'id': 'one', 'values': {'known': 99, 'unknown': 11}},
            {'op': 'first_full', 'model': 'b', 'values': {'known': 1, 'bad': 1e40}},
            {'op': 'snapshot', 'model': 'a'}, {'op': 'snapshot', 'model': 'b'}], errors=[3, 4])
        self.assertEqual(frames[5]['values'], {'known': 7})
        self.assertFalse(frames[6]['available'])

    def test_player_replacement_transfers_baselines_without_installation_eligibility(self):
        steps = self.install(session='old') + [
            {'op': 'normal', 'id': 'one', 'source': 'r.known=7;', 'session': 'old'},
            {'op': 'move_from', 'from': 'old'},
            {'op': 'snapshot', 'model': 'a', 'id': 'one'},
            {'op': 'snapshot', 'model': 'a', 'session': 'old'},
            {'op': 'adopt', 'id': 'one', 'values': {'known': 99}}]
        steps += self.install(identity='two')
        frames = self.run_steps(steps)
        self.assertEqual(frames[4]['values'], {'known': 7})
        self.assertFalse(frames[4]['exact_installation'])
        self.assertFalse(frames[5]['available'])
        self.assertFalse(frames[6]['adopted'])
        self.assertEqual(frames[7]['initial_values'], {'known': 7})

    def test_candidate_cannot_be_activated_twice_or_in_another_session(self):
        self.run_steps(self.install() + [{'op': 'activate', 'ticket': 'one'}], errors=[2])
        self.run_steps([{'op': 'prepare', 'model': 'a', 'id': 'one', 'fields': ['known'], 'local_only': True},
                        {'op': 'activate', 'ticket': 'one', 'session': 'other'},
                        {'op': 'snapshot', 'model': 'a'}], errors=[1])

