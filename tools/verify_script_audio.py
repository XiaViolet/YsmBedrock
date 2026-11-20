"""Check sound-status.json from native-script-audio.zip after the probe finishes."""
import json
import sys
from pathlib import Path


def verify(trace):
    start = next(i for i, row in enumerate(trace)
                 if row['id'] == 'n:7' and row['sound'] == 'fly' and row['result'] == 'playing')
    rows = trace[start:start + 9]
    expected = [
        ('n:7', 'fly', 'playing'), ('n:7', 'attack', 'duplicate'),
        ('n:7', 'attack', 'playing'), ('s:7', 'fly', 'playing'),
        ('n:7', '', 'stopped'), ('s:7', 'attack', 'duplicate'),
        ('s:7', 'fly', 'playing'), ('s:7', '', 'stopped'), (None, '', 'stopped'),
    ]
    actual = [(r['id'], r['sound'], r['result']) for r in rows]
    if actual != expected:
        raise ValueError(f'unexpected playback decisions: {actual!r}')
    owner = rows[0]['owner']
    if owner == 'global' or not rows[2]['force']:
        raise ValueError('expected an animation owner and a forced replacement')
    def voices(row):
        return {(v['owner'], v['id']): (v['handle'], v['looping'])
                for v in row['voices'] if v['playing']}
    n = (owner, 'n:7')
    s = (owner, 's:7')
    g = ('global', 's:7')
    snapshots = [voices(row) for row in rows]
    expected_keys = [{n}, {n}, {n}, {n, s}, {s}, {s}, {s, g}, {g}, set()]
    if [set(v) for v in snapshots] != expected_keys:
        raise ValueError(f'unexpected active voices: {snapshots!r}')
    if snapshots[0][n] != snapshots[1][n] or not snapshots[0][n][1]:
        raise ValueError('original loop did not survive past its authored duration')
    if snapshots[0][n][0] == snapshots[2][n][0] or snapshots[2][n][1]:
        raise ValueError('force did not replace the loop with a new one-shot handle')
    if snapshots[3][s] != snapshots[5][s] or snapshots[6][s][0] == snapshots[6][g][0]:
        raise ValueError('string or global voice lost ownership')
    if snapshots[6][g] != snapshots[7][g]:
        raise ValueError('local stop interrupted the global voice')
    return {'decisions': len(rows), 'owner': owner, 'loop_handle': snapshots[0][n][0],
            'replacement_handle': snapshots[2][n][0], 'final_active': len(snapshots[-1])}


if __name__ == '__main__':
    print(json.dumps(verify(json.loads(Path(sys.argv[1]).read_text(encoding='utf-8-sig')))))
