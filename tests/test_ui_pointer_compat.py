"""Replay physical-pixel pointer samples through the same menu hit tests as the game."""
import argparse
import json
import math
import subprocess


def verify(exe, legacy=False):
    def replay(events, **data):
        payload = dict(width=960, height=540, paint=True, events=events, **data)
        payload.setdefault("locale", "zh_CN")
        result = subprocess.run([exe, '--ui-session', json.dumps(payload)], check=True,
                                capture_output=True, encoding='utf-8', timeout=20)
        return json.loads(result.stdout)

    def hit(frame, label):
        rect = next(r for r in frame['text'] if r['text'] == label)
        return (2 * rect['x'] + rect['w'], 2 * rect['y'] + rect['h'])

    def sample(**kwargs):
        return dict(dict(op='window_mouse', client_width=1920, client_height=1080,
                         window_x=960, window_y=540), **kwargs)

    def touch(x, y, button=1, value=1):
        return sample(touch_x=x, touch_y=y, button=button, value=value)

    opened = [dict(op='open')]
    first = replay(opened)[0]
    x, y = hit(first, '本地设置')
    locked = opened + [sample(poll=True)]
    for _ in range(2):
        locked += [sample(dx=(x-960)/2, dy=(y-540)/2), sample(poll=True)]
    click = sample(button=1, value=1)
    assert (replay(locked+[click])[-1]['page'] == 4) == (not legacy)
    assert (replay(opened+[touch(x, y)])[-1]['page'] == 4) == (not legacy)
    if legacy:
        print('0.29.24 reproduced both missed hits with the desktop cursor pinned.')
        return

    moving = opened + [sample(poll=True)]
    for px in (950, 930, 910):
        moving += [sample(window_x=px, dx=-5)]*4 + [sample(poll=True, window_x=px)]
    assert replay(moving)[-1]['pointer_source'] == 'absolute'
    edge = opened + [sample(window_x=0, poll=True)]
    for _ in range(4):
        edge += [sample(window_x=0, dx=-10), sample(window_x=0, poll=True)]
    assert replay(edge)[-1]['pointer_source'] == 'absolute'
    assert replay(locked+[dict(op='close'), dict(op='open')])[-1]['pointer_source'] == 'absolute'

    for anchor in (960, 0):
        positions = opened + [sample(window_x=anchor, poll=True)]
        for i in range(4):
            positions += [sample(window_x=anchor, x=960+(x-960)*i/3, y=540+(y-540)*i/3),
                          sample(window_x=anchor, poll=True)]
        frames = replay(positions + [sample(window_x=anchor, button=1, value=1)])
        assert frames[-1]['pointer_source'] == 'game_absolute' and frames[-1]['page'] == 4
        assert replay(positions + [sample(window_x=anchor+5, poll=True)])[-1]['pointer_source'] == 'absolute'
    stationary = opened + [sample(poll=True)] + [sample(x=80, y=90), sample(poll=True)] * 4
    assert replay(stationary)[-1]['pointer_source'] == 'absolute'

    tx, ty = hit(first, '动作预览')
    events = locked + [touch(tx, ty), touch(tx, ty, 1, 0)]
    rect = replay(events)[-1]['preview']
    sx, sy = 2*rect['x']+rect['w'], 2*rect['y']+rect['h']
    dragged = events + [touch(sx, sy), touch(sx+60, sy-40, 0, 0), sample(poll=True)]
    released = replay(dragged + [touch(sx+60, sy-40, 1, 0)])[-1]
    yaw, pitch = released['preview_camera'][:2]
    assert math.isclose(yaw, 60, abs_tol=1e-4) and math.isclose(pitch, 13, abs_tol=1e-4)
    assert released['pointer_source'] == 'touch'
    recovered = replay(dragged + [dict(op='focus_lost'), sample(dx=4, force_relative=True)])[-1]
    assert recovered['preview_camera'] == released['preview_camera']
    assert recovered['pointer_source'] == 'relative'
    assert replay(events+[sample(window_x=x, window_y=y, button=1, value=1)])[-1]['page'] == 4

    wheel = [dict(op='open', wheel=True)]
    data = dict(descriptor={'properties': {'extra_animation': {'wave': '招手'}}})
    wx, wy = hit(replay(wheel, **data)[0], '招手')
    frames = replay(wheel + [touch(wx, wy), touch(wx, wy, 1, 0)], **data)
    assert [c['key'] for f in frames for c in f['commands']] == ['wave']
    assert not frames[-1]['open']
    print('PASS pinned desktop with deltas or game coordinates, desktop recovery, edge, session reset, touch tap/drag/wheel and focus loss.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('exe')
    parser.add_argument('--legacy', action='store_true')
    args = parser.parse_args()
    verify(args.exe, args.legacy)
