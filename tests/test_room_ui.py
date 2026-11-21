"""Replay the multiplayer menu through the release verifier's pointer and draw paths."""
import json
from pathlib import Path
import subprocess
import sys


def run(exe, host):
    key = "local1_" + "1" * 64
    stream = "2" * 32
    events = [{"op": "open"}]
    room = {"host": host, "active": True, "upload_bps": 131072, "download_bps": 262144, "rate_limit": 1048576,
            "denied": [{"key": key, "title": "测试模型", "temporary": True}],
            "observations": ["B 看 A · 已显示"]}
    room["upload_retry"] = stream + "|" + key
    player = {"id": "A", "name": "玩家 A", "state": "下载超时", "rates": "上传 256 · 下载 128 KiB/s", "key": key, "stream": stream, "retry": True}

    def draw():
        data = {"width": 1280, "height": 720, "room": room, "players": [player], "events": events}
        result = subprocess.run([exe, "--ui-session", json.dumps(data)], check=True, capture_output=True)
        return json.loads(result.stdout)[-1]

    def click(label, command=None):
        target = next(p for p in draw()["text"] if p["text"] == label)
        events.append({"op": "mouse", "button": 1, "value": 1,
                       "x": target["x"] + target["w"] / 2, "y": target["y"] + target["h"] / 2})
        result = draw()
        if command:
            assert [(c["kind"], c["key"]) for c in result["commands"]] == [command], label
        return {p["text"] for p in result["text"]}

    labels = click("联机玩家")
    assert "本机上传 128 KiB/s · 下载 256 KiB/s" in labels
    assert player["rates"] in labels
    click("本机限速 1024 KiB/s", ("room", "cycle-rate"))
    click("重新上传我的模型", ("room", "retry-upload:" + stream + "|" + key))
    click("重新下载", ("room", "retry:" + stream + "|A"))
    click("本机屏蔽", ("peer_visibility", "A"))
    if host:
        click("本场拒绝", ("room", "deny-once:" + key))
        click("长期拒绝", ("room", "deny:" + key))
        click("重新允许", ("room", "allow-once:" + key))
        click("改为长期拒绝", ("room", "deny:" + key))
        assert "B 看 A · 已显示" in click("客端显示回报")
        assert "下载超时" in click("返回玩家")
        # Replay a fresh snapshot; earlier pointer events retain their original targets.
        events.append({"op": "update", "room": {**room, "denied": [{"key": key, "title": "旧名单", "temporary": False}]}})
        click("重新允许", ("room", "allow:" + key))
    else:
        assert not {"客端显示回报", "重新允许", "长期拒绝", "本场拒绝"}.intersection(labels)


if __name__ == "__main__":
    verifier = str(Path(sys.argv[1]).resolve())
    run(verifier, True)
    run(verifier, False)
    print("PASS host/guest menu pointer replay: local and peer transfer rates, rate limit control, targeted retry, local hide, both deny scopes, promotion, undo and observer view.")
