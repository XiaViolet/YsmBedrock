"""Sidecar for one Bedrock server. It serves only models selected by its operator."""
import argparse
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import hashlib
import hmac
import json
import math
from pathlib import Path
import re
import secrets
import ssl
import subprocess
import threading
import time


MAX_ARCHIVE = 128 << 20
MAX_BODY = 128 << 10
LEASE_SECONDS = 8
F32_MAX = 3.4028234663852886e38
ROOM_SIZE = 16
EVENT_PAGE = 64
EVENT_JOURNAL = 256


def bounded_string(value, limit):
    if not isinstance(value, str) or len(value.encode("utf-8")) > limit or "\0" in value:
        raise ValueError("Invalid string")
    return value


def counter(value):
    if type(value) is not int or not 0 <= value <= (1 << 53) - 1:
        raise ValueError("Invalid counter")
    return value


def finite_f32(value):
    if type(value) not in (int, float) or not -F32_MAX <= value <= F32_MAX or not math.isfinite(value):
        raise ValueError("Invalid Molang number")
    return value


def player_state(data, owner, models, allowed):
    if not isinstance(data, dict) or data.get("protocol") != 1 or data.get("xuid") != owner:
        raise ValueError("Invalid player identity or protocol")
    session = data["session"]
    if not isinstance(session, str) or not re.fullmatch("[0-9a-f]{32}", session):
        raise ValueError("Invalid connection identity")
    serial = counter(data["serial"])
    key = bounded_string(data.get("model", ""), 71)
    chosen = models.get(key)
    if not chosen or chosen["name"] not in allowed:
        return dict(xuid=owner, session=session, serial=serial, model="", texture="", action="",
                    action_serial=0, action_time=0, roaming={}, events=[])
    shoulders = data.get("shoulder_birds", [None, None])
    if not isinstance(shoulders, list) or len(shoulders) != 2:
        raise ValueError("Invalid shoulder slots")
    for bird in shoulders:
        if bird is not None and (not isinstance(bird, str) or not re.fullmatch(r"-?[1-9][0-9]{0,18}", bird)
                                 or not -(1 << 63) <= int(bird) < (1 << 63)):
            raise ValueError("Invalid shoulder entity")
    if shoulders[0] is not None and shoulders[0] == shoulders[1]:
        raise ValueError("Duplicate shoulder entity")
    roaming = data.get("roaming", {})
    if not isinstance(roaming, dict) or len(roaming) > 1024:
        raise ValueError("Invalid roaming data")
    for field, value in roaming.items():
        if not re.fullmatch("[a-z_][a-z_0-9]{0,31}", field):
            raise ValueError("Invalid roaming field")
        finite_f32(value)
    events = data.get("events", [])
    if not isinstance(events, list) or len(events) > 64:
        raise ValueError("Invalid event queue")
    for event in events:
        if not isinstance(event, list) or not 1 <= len(event) <= 17 or not counter(event[0]):
            raise ValueError("Invalid animation event")
        for value in event[1:]:
            finite_f32(value)
    action_time = finite_f32(data.get("action_time", 0))
    if not 0 <= action_time <= 86400:
        raise ValueError("Invalid animation time")
    extra = {}
    if "flight" in data:
        flight = data["flight"]
        if type(flight) is not int or not 0 <= flight <= 3:
            raise ValueError("Invalid flight state")
        extra["flight"] = flight
    forms = data.get("form_values", {})
    if not isinstance(forms, dict) or len(forms) > 128:
        raise ValueError("Invalid form state")
    for name, value in forms.items():
        if len(name.encode("utf8")) > 96 or not name.startswith("variable.") or name.startswith("variable.roaming."):
            raise ValueError("Invalid form field")
        finite_f32(value)
    if forms:
        extra["form_values"] = forms
    return dict(**extra, xuid=owner, session=session, serial=serial, model=key,
                texture=bounded_string(data.get("texture", ""), 512),
                action=bounded_string(data.get("action", ""), 512),
                action_serial=counter(data.get("action_serial", 0)), action_time=action_time,
                roaming=roaming, events=events, shoulder_birds=shoulders)


class Room:
    def __init__(self, config, base, inspector):
        self.models = {}
        for name in config["models"]:
            path = (base / name).resolve(strict=True)
            if not path.is_relative_to(base) or path.suffix.lower() not in (".zip", ".ysm") or not path.is_file():
                raise ValueError("Use approved model archives beside the room configuration")
            if path.stat().st_size > MAX_ARCHIVE:
                raise ValueError("Model ZIP exceeds 128 MiB")
            data = path.read_bytes()
            report = subprocess.run([str(inspector), str(path)], check=True, capture_output=True, encoding="utf-8", timeout=90)
            model = json.loads(report.stdout)
            if hashlib.sha256(path.read_bytes()).digest() != hashlib.sha256(data).digest():
                raise ValueError("Model changed while it was being read")
            key = model["local_content_key"]
            if key in self.models:
                raise ValueError("Two ZIPs have the same model content")
            self.models[key] = dict(name=name, key=key, digest=hashlib.sha256(data).hexdigest(), data=data, extension=path.suffix.lower(),
                                    title=model["name"], size=len(data))
        if not 1 <= len(self.models) <= 64 or sum(m["size"] for m in self.models.values()) > 512 << 20:
            raise ValueError("Room model budget exceeded")
        self.players = config["players"]
        if not 1 <= len(self.players) <= ROOM_SIZE:
            raise ValueError("Room player limit exceeded")
        tokens = set()
        for xuid, settings in self.players.items():
            token = settings["token"]
            if not re.fullmatch("[1-9][0-9]{0,19}", xuid) or not re.fullmatch("[a-zA-Z0-9_-]{32,128}", token) or token in tokens:
                raise ValueError("Invalid or duplicate room credentials")
            tokens.add(token)
            settings["models"] = set(settings.get("models", config["models"]))
            if settings["models"].difference(config["models"]):
                raise ValueError("Player allowed list contains an unknown model")
        self.lock = threading.Lock()
        self.live = {}
        self.retired = {}

    def authenticate(self, credentials):
        for xuid, settings in self.players.items():
            if hmac.compare_digest("Bearer " + settings["token"], credentials):
                return xuid
        return None

    def update(self, owner, data):
        state = player_state(data, owner, self.models, self.players[owner]["models"])
        receipts = data.get("receipts", {})
        if not isinstance(receipts, dict) or len(receipts) > ROOM_SIZE:
            raise ValueError("Invalid event receipts")
        for subject, receipt in receipts.items():
            if (subject == owner or subject not in self.players or not isinstance(receipt, list)
                    or len(receipt) != 3 or not isinstance(receipt[0], str)
                    or not re.fullmatch("[0-9a-f]{32}", receipt[0]) or not isinstance(receipt[1], str)
                    or receipt[1] not in self.models):
                raise ValueError("Invalid event receipt")
            counter(receipt[2])
        now = time.monotonic()
        with self.lock:
            old = self.live.get(owner)
            retired = self.retired.setdefault(owner, deque(maxlen=16))
            session = state["session"]
            if session in retired:
                raise ValueError("Connection has ended")
            if old and old["session"] != session:
                retired.append(old["session"])
                old = None
            if not old or state["serial"] > old["serial"]:
                # Retry and duplicate play packets are idempotent. A new model
                # starts a new event window and observers use its own clock.
                same_model = old and state["model"] == old["model"]
                recorded = old["events"].copy() if same_model else []
                cursor = old["event_cursor"] if old else 0
                reading = [peer["receipts"][owner][2] for peer in self.live.values()
                           if now - peer["updated"] <= LEASE_SECONDS and owner in peer["receipts"]
                           and peer["receipts"][owner][:2] == [session, state["model"]]]
                # Preserve the last page for clients without receipts. A live
                # reader can request older pages until it acknowledges delivery.
                keep_after = min([cursor, *reading])
                recorded = [e for i, e in enumerate(recorded)
                            if e[0] > keep_after or i >= len(recorded) - EVENT_PAGE]
                for record in sorted(state["events"], key=lambda event: event[0]):
                    if record[0] > cursor:
                        if len(recorded) == EVENT_JOURNAL:
                            break  # Sender retains the unacknowledged suffix.
                        recorded.append(record)
                        cursor = record[0]
                state["events"] = recorded
                state["receipts"] = receipts
                state["event_cursor"] = cursor
                state["updated"] = now
                self.live[owner] = state
            else:
                state = old
            players = []
            for peer in self.live.values():
                age = now - peer["updated"]
                if peer["xuid"] != owner and peer["model"] and age <= LEASE_SECONDS:
                    public = {key: value for key, value in peer.items() if key not in ("updated", "receipts", "events")}
                    seen = state["receipts"].get(peer["xuid"])
                    if seen and seen[:2] == [peer["session"], peer["model"]]:
                        public["events"] = [e for e in peer["events"] if e[0] > seen[2]][:EVENT_PAGE]
                    else:
                        public["events"] = peer["events"][-EVENT_PAGE:]
                    if public["action"]:
                        public["action_time"] = min(86400, public["action_time"] + age)
                    players.append(public)
            models = [dict({key: asset[key] for key in ("key", "digest", "title", "size", "extension")},
                           selectable=asset["name"] in self.players[owner]["models"]) for asset in self.models.values()]
            return dict(protocol=1, you=owner, approved=bool(state["model"]),
                        event_cursor=state["event_cursor"], models=models, players=players)


class RelayHandler(BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(6)

    def log_message(self, *args):
        pass

    def respond(self, status, data, mime="application/json"):
        self.send_response(status)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def get_owner(self):
        return self.server.room.authenticate(self.headers.get("Authorization", ""))

    def do_POST(self):
        if self.path != "/v1/state":
            return self.respond(404, b"{}")
        owner = self.get_owner()
        if not owner:
            return self.respond(401, b"{}")
        try:
            count = int(self.headers.get("Content-Length", 0))
            if not 0 < count <= MAX_BODY:
                raise ValueError("Request is too large")
            state = self.server.room.update(owner, json.loads(self.rfile.read(count)))
            self.respond(200, json.dumps(state, separators=(",", ":"), allow_nan=False).encode("utf-8"))
        except (KeyError, TypeError, ValueError, UnicodeError):
            self.respond(400, b"{}")

    def do_GET(self):
        if not self.get_owner():
            return self.respond(401, b"{}")
        if not self.path.startswith("/v1/model/"):
            return self.respond(404, b"{}")
        digest = self.path.removeprefix("/v1/model/")
        match = next((asset for asset in self.server.room.models.values() if asset["digest"] == digest), None)
        if not match:
            return self.respond(404, b"{}")
        self.respond(200, match["data"], "application/octet-stream")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--new-token", action="store_true")
    parser.add_argument("--config", type=Path)
    parser.add_argument("--inspector", type=Path, default=Path("ysm-inspect.exe"))
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", default=19140, type=int)
    parser.add_argument("--cert")
    parser.add_argument("--key")
    args = parser.parse_args()
    if args.new_token:
        print(secrets.token_urlsafe(32))
        return
    if not args.config or bool(args.cert) != bool(args.key):
        parser.error("Use --config, and pair --cert with --key")
    if args.bind != "127.0.0.1" and not args.cert:
        parser.error("An externally reachable relay needs a TLS certificate")
    config = args.config.resolve(strict=True)
    room = Room(json.loads(config.read_text(encoding="utf-8-sig")), config.parent, args.inspector.resolve(strict=True))
    with ThreadingHTTPServer((args.bind, args.port), RelayHandler) as server:
        server.room = room
        if args.cert:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(args.cert, args.key)
            server.socket = context.wrap_socket(server.socket, server_side=True)
        print(f"YSM relay {args.bind}:{server.server_port}, {len(room.models)} approved model(s)", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
