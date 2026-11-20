"""Replay peer preparation using the scheduler called by NativeModelRelay."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import sys

from test_import import EXE, PROJECT, SAMPLE

sys.path.insert(0, str(PROJECT / 'tools'))
from ysm_relay import EVENT_JOURNAL, EVENT_PAGE, LEASE_SECONDS, Room


class RelayPreparationTests(unittest.TestCase):
    def replay(self, steps):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'peers.json'
            path.write_text(json.dumps(steps), encoding='utf-8')
            return json.loads(subprocess.check_output([str(EXE), '--relay-preparation', str(path)], encoding='utf-8'))

    def test_failed_models_do_not_take_other_players_turns(self):
        peers = [{'id': x} for x in 'abc']
        steps = [dict(ms=t, players=peers, fail=True) for t in (0, 2000, 4000, 6000)]
        self.assertEqual(self.replay(steps), ['a', 'b', 'c', 'a'])
        steps = [dict(ms=0, players=peers, fail=True), dict(ms=1, players=peers, skip=['b'])]
        self.assertEqual(self.replay(steps), ['a', 'c'])

    def test_retry_is_scoped_to_player_session_model_and_texture(self):
        first = {'id': 'a'}
        for changed in ({'session': 'b'}, {'model': 'new'}, {'texture': 'blue'}):
            with self.subTest(changed=changed):
                steps = [dict(ms=0, players=[first], fail=True), dict(ms=10, players=[first]),
                         dict(ms=20, players=[dict(first, **changed)])]
                self.assertEqual(self.replay(steps), ['a', None, 'a'])
        steps = [dict(ms=0, players=[first], fail=True), dict(ms=1999, players=[first]),
                 dict(ms=2000, players=[first], fail=True), dict(ms=2010, players=[]),
                 dict(ms=2020, players=[first])]
        self.assertEqual(self.replay(steps), ['a', None, 'a', None, 'a'])


class RelayDeliveryTests(unittest.TestCase):
    def setUp(self):
        self.sender, self.reader = '101', '102'
        self.session = 'a' * 32
        self.serials = {self.sender: 0, self.reader: 0}
        players = {key: {'token': key * 16} for key in self.serials}
        self.room = Room({'models': [SAMPLE.name], 'players': players}, SAMPLE.parent, EXE)
        self.model = next(iter(self.room.models))

    def update(self, user, **fields):
        self.serials[user] += 1
        return self.room.update(user, dict(protocol=1, xuid=user, serial=self.serials[user],
                                           model=self.model, session=self.session, **fields))

    def request(self, seen):
        result = self.update(self.reader, receipts={self.sender: [self.session, self.model, seen]})
        return [event[0] for event in result['players'][0]['events']]

    def test_active_reader_protects_pages_and_resends_do_not_append_duplicates(self):
        self.update(self.sender)
        self.assertEqual(self.request(0), [])
        for start in range(1, EVENT_JOURNAL + 1, EVENT_PAGE):
            page = [[seq] for seq in range(start, start + EVENT_PAGE)]
            self.assertEqual(self.update(self.sender, events=page)['event_cursor'], page[-1][0])
        extra = [[EVENT_JOURNAL + 1]]
        self.assertEqual(self.update(self.sender, events=extra)['event_cursor'], EVENT_JOURNAL)
        seqs = list(range(1, EVENT_PAGE + 1))
        self.assertEqual(self.request(0), seqs)
        self.assertEqual(self.request(0), seqs)
        self.assertEqual(self.request(EVENT_PAGE), [seq + EVENT_PAGE for seq in seqs])
        self.assertEqual(self.update(self.sender, events=extra)['event_cursor'], extra[0][0])
        self.assertEqual(self.update(self.sender, events=extra)['event_cursor'], extra[0][0])
        self.assertEqual(self.request(EVENT_JOURNAL), extra[0])

    def test_expired_reader_does_not_hold_sender_and_old_clients_get_one_page(self):
        with patch('ysm_relay.time.monotonic', return_value=10):
            self.update(self.sender)
            self.request(0)
            for start in range(1, EVENT_JOURNAL + 1, EVENT_PAGE):
                self.update(self.sender, events=[[seq] for seq in range(start, start + EVENT_PAGE)])
        with patch('ysm_relay.time.monotonic', return_value=11 + LEASE_SECONDS):
            self.assertEqual(self.update(self.sender, events=[[EVENT_JOURNAL + 1]])['event_cursor'],
                             EVENT_JOURNAL + 1)
            page = self.update(self.reader)['players'][0]['events']
            self.assertEqual(page, [[seq] for seq in range(EVENT_JOURNAL - EVENT_PAGE + 2, EVENT_JOURNAL + 2)])

    def test_invalid_receipts_cannot_acknowledge_someone_elses_stream(self):
        for user, receipt in ((self.reader, [self.session, self.model, 99]),
                              ('999', [self.session, self.model, 99]),
                              (self.sender, [self.session, 'unknown-model', 99]),
                              (self.sender, [self.session, self.model, -1])):
            with self.subTest(user=user, receipt=receipt), self.assertRaises(ValueError):
                self.update(self.reader, receipts={user: receipt})
