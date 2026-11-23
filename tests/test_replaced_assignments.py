"""Ownership, snapshot and retirement semantics from the public YSM capabilities."""
import json
import subprocess
import unittest

from test_import import EXE


class ReplacedAssignmentTests(unittest.TestCase):
    def run_events(self, *events, packs=None, error=None):
        packs = packs or {
            'a': {'targets': [{'id': 'cart', 'kind': 'vehicle', 'matches': ['boat', 'minecart']},
                              {'id': 'arrow', 'kind': 'projectile', 'matches': ['arrow', 'thrown_trident']}]},
            'b': {'targets': [{'id': 'b-cart', 'kind': 'vehicle', 'matches': ['boat']},
                              {'id': 'b-arrow', 'kind': 'projectile', 'matches': ['arrow']}]},
        }
        run = subprocess.run([str(EXE), '--target-assignments', json.dumps({'packs': packs, 'events': events})],
                             capture_output=True, encoding='utf-8', timeout=10)
        if error:
            self.assertNotEqual(run.returncode, 0)
            self.assertIn(error, run.stderr)
            return
        self.assertEqual(run.returncode, 0, run.stderr)
        return json.loads(run.stdout)

    @staticmethod
    def owner(pack='a', owner=1, **values):
        return dict(op='owner', owner=owner, pack=pack, roaming=values)

    @staticmethod
    def mount(vehicle=10, rider=1, first=1):
        return dict(op='mount', vehicle=vehicle, rider=rider, first=first)

    @staticmethod
    def resolve(kind='vehicle', entity=10, type='boat', **kwargs):
        return dict(op='resolve', kind=kind, entity=entity, type=type, **kwargs)

    def test_unassigned_entity_has_no_current_player_fallback(self):
        frames = self.run_events(self.owner(), self.resolve())
        self.assertIsNone(frames[-1]['resolved'])

    def test_only_first_passenger_assigns_vehicle(self):
        frames = self.run_events(self.owner(), self.mount(first=2), self.resolve(), self.mount(), self.resolve())
        self.assertIsNone(frames[2]['resolved'])
        self.assertEqual(frames[-1]['resolved']['pack'], 'a')

    def test_dismount_retains_model_and_values_while_owner_changes(self):
        frames = self.run_events(self.owner(x=1), self.mount(), dict(op='dismount', rider=1),
                                 self.owner('b', x=7), self.resolve())
        self.assertEqual(frames[-1]['resolved']['pack'], 'a')
        self.assertEqual(frames[-1]['resolved']['roaming'], {'x': 1})

    def test_model_and_full_roaming_updates_reach_current_vehicle(self):
        frames = self.run_events(self.owner(x=1, removed=2), self.mount(), self.owner('b', x=9), self.resolve())
        self.assertEqual(frames[-1]['resolved']['pack'], 'b')
        self.assertEqual(frames[-1]['resolved']['roaming'], {'x': 9})

    def test_peer_switches_model_as_mount_changes(self):
        riding = dict(self.owner(), op='peer', vehicle=10, first=1)
        for current_mount in ({}, {'vehicle': 11, 'first': 1}, {'vehicle': 10, 'first': 2}):
            with self.subTest(mount=current_mount):
                changed = dict(self.owner('b'), op='peer', **current_mount)
                rows = self.run_events(riding, changed, self.resolve(), self.resolve(entity=11))
                self.assertEqual(rows[-2]['resolved']['pack'], 'a')
                if current_mount.get('vehicle') == 11:
                    self.assertEqual(rows[-1]['resolved']['pack'], 'b')
                else:
                    self.assertIsNone(rows[-1]['resolved'])

    def test_visible_peer_without_current_targets_keeps_old_projectile(self):
        arrow = dict(op='projectile', entity=20, owner=1)
        resolve = self.resolve('projectile', 20, 'arrow')
        packs = {'server:archer': {'targets': [{'id': 'bolt', 'kind': 'projectile', 'matches': ['arrow']}]},
                 'server:plain': {'targets': []}}
        rows = self.run_events(dict(self.owner('server:archer'), op='peer'), arrow,
                               dict(self.owner('server:plain'), op='peer'), resolve, dict(arrow, entity=21),
                               dict(resolve, entity=21), dict(op='remove_owner', owner=1), resolve, packs=packs)
        self.assertEqual(rows[3]['resolved']['target'], 'bolt')
        self.assertIsNone(rows[5]['resolved'])
        self.assertIsNone(rows[7]['resolved'])

    def test_new_first_passenger_takes_assignment_and_old_owner_cannot_overwrite(self):
        frames = self.run_events(self.owner(), self.owner('b', owner=2), self.mount(), self.mount(rider=2, first=2),
                                 self.owner(x=8), self.resolve())
        self.assertEqual(frames[-1]['resolved']['owner'], 2)
        self.assertEqual(frames[-1]['resolved']['pack'], 'b')

    def test_second_passenger_updates_do_not_overwrite_first(self):
        frames = self.run_events(self.owner(), self.owner('b', owner=2), self.mount(),
                                 self.mount(rider=2, first=1), self.owner('b', owner=2, x=9), self.resolve())
        self.assertEqual(frames[-1]['resolved']['owner'], 1)

    def test_old_vehicle_keeps_assignment_when_player_moves_to_another_vehicle(self):
        frames = self.run_events(self.owner(), self.mount(), self.mount(vehicle=11), self.owner('b'),
                                 self.resolve(), self.resolve(entity=11))
        self.assertEqual(frames[-2]['resolved']['pack'], 'a')
        self.assertEqual(frames[-1]['resolved']['pack'], 'b')

    def test_projectile_captures_owner_at_assignment_and_does_not_follow_later_updates(self):
        frames = self.run_events(self.owner(x=1), dict(op='projectile', entity=20, owner=1),
                                 self.owner('b', x=2), self.resolve('projectile', 20, 'arrow'))
        self.assertEqual(frames[-1]['resolved']['pack'], 'a')
        self.assertEqual(frames[-1]['resolved']['roaming'], {'x': 1})

    def test_observer_uses_server_shot_even_without_a_spawn_owner_or_after_a_slot_change(self):
        for weapon in ('bow', 'crossbow'):
            owner = dict(op='peer', owner=7, pack='b')
            shot = dict(op='server_shot', entity=20, owner=7, crossbow=weapon == 'crossbow')
            resolve = self.resolve('projectile', 20, 'arrow')
            for spawn in ([], [dict(op='projectile_client_spawn', entity=20, owner=7, shoot_item_id='minecraft:stick')]):
                with self.subTest(weapon=weapon, spawn=bool(spawn)):
                    frames = self.run_events(owner, *spawn, shot, resolve)
                    actual = frames[-1]['resolved']
                    self.assertEqual((actual['pack'], actual['owner'], actual['shoot_item_id']),
                                     ('b', 7, 'minecraft:' + weapon))

    def test_projectile_samples_the_client_slot_for_same_tick_and_delayed_switches(self):
        for weapon in ('minecraft:bow', 'minecraft:crossbow'):
            for client_item, later_item in (('minecraft:stick', weapon), (weapon, 'minecraft:stick')):
                with self.subTest(weapon=weapon, captured=client_item):
                    shot = dict(op='projectile', entity=20, owner=1, shoot_item_id=weapon)
                    spawn = dict(shot, op='projectile_client_spawn', shoot_item_id=client_item)
                    resolve = self.resolve('projectile', 20, 'arrow')
                    frames = self.run_events(self.owner(x=1), shot, resolve, self.owner('b', x=2), spawn, resolve,
                                             shot, dict(spawn, shoot_item_id=later_item), resolve)
                    self.assertEqual(frames[2]['resolved']['shoot_item_id'], '')
                    self.assertEqual(frames[5]['resolved'], frames[-1]['resolved'])
                    resolved = frames[-1]['resolved']
                    self.assertEqual(resolved['shoot_item_id'], client_item)
                    self.assertEqual(resolved['pack'], 'a')
                    self.assertEqual(resolved['roaming'], {'x': 1})

    def test_late_item_capture_fills_missing_field_and_new_owner_gets_new_item(self):
        shot = dict(op='projectile', entity=20, owner=1)
        resolve = self.resolve('projectile', 20, 'arrow')
        frames = self.run_events(self.owner(x=1), shot, self.owner('b', x=2),
                                 dict(shot, op='projectile_client_spawn', shoot_item_id='minecraft:bow'), resolve,
                                 self.owner('b', owner=2, x=3),
                                 dict(shot, op='projectile_client_spawn', owner=2, shoot_item_id='minecraft:crossbow'), resolve)
        self.assertEqual(frames[4]['resolved']['shoot_item_id'], 'minecraft:bow')
        self.assertEqual(frames[4]['resolved']['roaming'], {'x': 1})
        self.assertEqual(frames[-1]['resolved']['shoot_item_id'], 'minecraft:crossbow')
        self.assertEqual(frames[-1]['resolved']['roaming'], {'x': 3})

    def test_projectile_client_empty_hand_is_air_for_the_lifetime_of_the_entity(self):
        spawn = dict(op='projectile_client_spawn', entity=20, owner=1, shoot_item_id='minecraft:air')
        frames = self.run_events(self.owner(), spawn, dict(spawn, shoot_item_id='minecraft:bow'),
                                 self.resolve('projectile', 20, 'arrow'))
        self.assertEqual(frames[-1]['resolved']['shoot_item_id'], 'minecraft:air')

    def test_explicit_projectile_owner_reassignment_takes_new_snapshot(self):
        frames = self.run_events(self.owner(), dict(op='projectile', entity=20, owner=1), self.owner('b', owner=2),
                                 dict(op='projectile', entity=20, owner=2), self.resolve('projectile', 20, 'arrow'))
        self.assertEqual(frames[-1]['resolved']['pack'], 'b')

    def test_unknown_projectile_owner_preserves_existing_assignment(self):
        frames = self.run_events(self.owner(), dict(op='projectile', entity=20, owner=1),
                                 dict(op='projectile', entity=20, owner=99), self.resolve('projectile', 20, 'arrow'))
        self.assertEqual(frames[-1]['resolved']['owner'], 1)

    def test_no_matching_target_does_not_borrow_other_model_or_kind(self):
        frames = self.run_events(self.owner('b'), self.mount(), self.resolve(type='minecart'),
                                 self.resolve('projectile', 10, 'arrow'))
        self.assertIsNone(frames[-2]['resolved'])
        self.assertIsNone(frames[-1]['resolved'])

    def test_explicit_null_model_clears_ridden_vehicle_but_not_fired_projectile(self):
        frames = self.run_events(self.owner(), self.mount(), dict(op='projectile', entity=20, owner=1),
                                 self.owner(None), self.resolve(), self.resolve('projectile', 20, 'arrow'))
        self.assertIsNone(frames[-2]['resolved'])
        self.assertEqual(frames[-1]['resolved']['pack'], 'a')

    def test_identical_owner_update_keeps_revision(self):
        frames = self.run_events(self.owner(x=2), self.mount(), self.resolve(), self.owner(x=2), self.resolve())
        self.assertEqual(frames[-1]['resolved']['revision'], frames[2]['resolved']['revision'])

    def test_roaming_snapshot_preserves_f32_and_empty_full_update_removes_old_fields(self):
        frames = self.run_events(self.owner(x=16777217), self.mount(), self.resolve(), self.owner(), self.resolve())
        self.assertEqual(frames[2]['resolved']['roaming']['x'], 16777216)
        self.assertEqual(frames[-1]['resolved']['roaming'], {})

    def test_retained_assignment_keeps_package_alive_until_entity_is_forgotten(self):
        frames = self.run_events(self.owner(), self.mount(), dict(op='remove_owner', owner=1),
                                 dict(op='drop_pack', pack='a'), self.resolve(),
                                 dict(op='forget', kind='vehicle', entity=10))
        self.assertTrue(frames[-2]['live_packs']['a'])
        self.assertEqual(frames[-2]['resolved']['pack'], 'a')
        self.assertFalse(frames[-1]['live_packs']['a'])

    def test_local_old_arrow_survives_owner_disabling_while_hidden_server_assets_stay_hidden(self):
        for source in ['local.zip', 'server:key']:
            with self.subTest(source=source):
                frames = self.run_events(self.owner(source), dict(op='projectile', entity=20, owner=1),
                    dict(op='remove_owner', owner=1), self.resolve('projectile', 20, 'arrow'),
                    dict(op='projectile', entity=21, owner=1), self.resolve('projectile', 21, 'arrow'),
                    packs={source: {'targets': [{'id': 'bolt', 'kind': 'projectile', 'matches': ['arrow']}]}})
                self.assertEqual(frames[3]['resolved'] is not None, source == 'local.zip')
                self.assertIsNone(frames[5]['resolved'])

    def test_connection_reset_clears_assignments_and_mount_tracking(self):
        frames = self.run_events(self.owner(), self.mount(), dict(op='projectile', entity=20, owner=1),
                                 dict(op='reset'), self.owner('b'), self.resolve(),
                                 self.resolve('projectile', 20, 'arrow'))
        self.assertIsNone(frames[-1]['resolved'])
        self.assertIsNone(frames[-2]['resolved'])
        self.assertEqual(frames[-1]['mounts'], 0)

    def test_target_tag_matching_and_first_declaration_order_survive_assignment(self):
        packs = {'a': {'targets': [{'id': 'first', 'kind': 'vehicle', 'matches': ['#minecraft:boats']},
                                   {'id': 'second', 'kind': 'vehicle', 'matches': ['boat']}]}}
        frames = self.run_events(self.owner(), self.mount(), self.resolve(tags=['minecraft:boats']), packs=packs)
        self.assertEqual(frames[-1]['resolved']['target'], 'first')

    def test_saved_arrow_recovers_model_and_roaming_but_samples_hand_on_new_spawn(self):
        for restore_first in (True, False):
            with self.subTest(restore_first=restore_first):
                before = dict(op='projectile_client_spawn', entity=20, owner=1, shoot_item_id='minecraft:bow')
                after = dict(before, entity=35, shoot_item_id='minecraft:stick')
                load = dict(op='load_entity', object='same-uuid', entity=35, resource_ready=False)
                seq = (load, after) if restore_first else (after, load)
                result = self.run_events(self.owner(x=3.5), before,
                    dict(op='save_entity', kind='projectile', entity=20, object='same-uuid'),
                    dict(op='reset'), self.owner('b', x=11), *seq, dict(op='load_source', pack='a'),
                    self.resolve('projectile', 35, 'arrow'))[-1]['resolved']
                self.assertEqual((result['pack'],result['roaming'],result['shoot_item_id']),
                                 ('a', {'x':3.5}, 'minecraft:stick'))

    def test_restored_hand_uses_equipped_slot_even_when_sent_slot_still_points_to_bow(self):
        spawn = dict(op='projectile_client_spawn', entity=20, owner=1, shoot_item_id='minecraft:bow',
                     equipped='minecraft:stick')
        load = dict(op='load_entity', object='same-arrow', entity=20)
        resolve = self.resolve('projectile', 20, 'arrow')
        for sequence in ((load, spawn), (spawn, load, dict(spawn, op='restore_spawn_hand'))):
            rows = self.run_events(self.owner(), spawn, resolve,
                dict(op='save_entity', kind='projectile', entity=20, object='same-arrow'),
                dict(op='reset'), self.owner('b'), *sequence, resolve)
            self.assertEqual(rows[2]['resolved']['shoot_item_id'], 'minecraft:bow')
            self.assertEqual(rows[-1]['resolved']['shoot_item_id'], 'minecraft:stick')

    def test_server_saves_live_same_model_roaming_until_entity_is_deserialized(self):
        for kind, entity, matched, capture in (
            ('vehicle', 10, 'boat', [self.mount(), dict(op='dismount', rider=1)]),
            ('projectile', 20, 'arrow', [dict(op='projectile', owner=1, entity=20)]),
        ):
            with self.subTest(kind=kind):
                save = dict(op='save_entity', object='native', kind=kind, entity=entity)
                load = dict(op='load_entity', object='native', entity=entity)
                resolve = self.resolve(kind, entity, matched)
                frames = self.run_events(self.owner(gun=1), *capture, self.owner(gun=0),
                    self.owner(owner=2,gun=31), resolve, save, dict(op='reset'), self.owner(gun=0),
                    load, resolve, self.owner(gun=7), save, dict(op='reset'), self.owner(gun=7), load, resolve)
                self.assertEqual([row['resolved']['roaming'] for row in frames if row['resolved']],
                                 [{'gun':1}, {'gun':0}, {'gun':0}])

    def test_dismounted_vehicle_restores_independently_of_current_selection(self):
        result = self.run_events(self.owner(skin=2), self.mount(), dict(op='dismount', rider=1), self.owner('b'),
            dict(op='save_entity', kind='vehicle', entity=10, object='boat'), dict(op='reset'),
            self.owner('b'), dict(op='load_entity', object='boat', entity=12), self.resolve(entity=12),
            self.mount(vehicle=12), self.resolve(entity=12))
        self.assertEqual(result[-3]['resolved']['pack'], 'a')
        self.assertEqual(result[-3]['resolved']['roaming'], {'skin':2})
        self.assertEqual(result[-1]['resolved']['pack'], 'b')

    def test_missing_saved_model_keeps_native_draw_until_exact_source_is_available(self):
        rows = self.run_events(self.owner(), self.mount(), dict(op='save_entity', kind='vehicle', entity=10, object='boat'),
            dict(op='reset'), self.owner('b'), dict(op='load_entity', object='boat', entity=10, resource_ready=False),
            dict(op='load_source', pack='b'), self.resolve(), dict(op='load_source', pack='a'), self.resolve())
        self.assertIsNone(rows[-3]['resolved'])
        self.assertEqual(rows[-1]['resolved']['pack'], 'a')

    def test_wrong_roaming_field_name_rejected(self):
        self.run_events(self.owner(**{'bad-name': 1}), error='Invalid Roaming field name')


if __name__ == '__main__':
    unittest.main()
