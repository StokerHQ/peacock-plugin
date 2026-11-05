"""
Enough of the Stoker agent for the runtime's tests: protocol v1's hello, list, grant and icon on a unix
socket, leaderboards (fake_leaderboards.py), and events for the overlay's and an integration's own connections, with every request appended to a JSON-lines file
(each tagged "_integration" with the name its connection's hello gave, so the game's requests and the
overlay's can be told apart).

    python3 fake_agent.py <socket> <requests.jsonl> [--refuse CODE] [--offline] [--tcp PORTFILE --token TOKEN]

The player already has ALREADY unlocked. With --refuse the hello is answered with that error code.
Events say stokerd is reachable and the saves are up to date, or with --offline that it is not and
they are waiting (integration-protocol.md, "Events").

With --tcp it listens on 127.0.0.1, an ephemeral port written to PORTFILE, instead of the socket, and as
the agent does there (integration-protocol.md, "Transports") takes the first frame of each connection as
{"v":1,"type":"auth","token":...}: TOKEN admits it, anything else closes it unanswered and is logged as
an "_refused" line.
"""

import json
import os
import socket
import struct
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fake_leaderboards  # noqa: E402

path, out = sys.argv[1], sys.argv[2]
refuse = sys.argv[sys.argv.index("--refuse") + 1] if "--refuse" in sys.argv else None
offline = "--offline" in sys.argv
tcp_port_file = sys.argv[sys.argv.index("--tcp") + 1] if "--tcp" in sys.argv else None
token = sys.argv[sys.argv.index("--token") + 1] if "--token" in sys.argv else None

if tcp_port_file:
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.bind(("127.0.0.1", 0))
    server.listen(4)
    with open(tcp_port_file + ".tmp", "w") as f:
        f.write(str(server.getsockname()[1]))
    os.rename(tcp_port_file + ".tmp", tcp_port_file)
else:
    if os.path.exists(path):
        os.unlink(path)

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(path)
    server.listen(4)

# A 2x2 PNG for NEW's unlocked icon; the others have none.
PNG = "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAGElEQVR4nGP4z8Dwn+E/QwMDA8N/EGAAAEPTCHlKSuRGAAAAAElFTkSuQmCC"


log_lock = threading.Lock()

LEADERBOARDS = fake_leaderboards.Leaderboards()
leaderboards_lock = threading.Lock()

EVENTS = {
    "connection": {"online": not offline, "offlineSince": 1790000000 if offline else None,
                   "waiting": 2 if offline else 0, "othersWaiting": offline},
    "saves": {"managed": True, "syncsAtExit": False, "state": "unavailable" if offline else "upToDate",
              "direction": None, "lastSyncedAt": 1790000000,
              "message": "Your saves will upload when Stoker can reach the server." if offline else None},
    "friends": {"friends": [
        {"accountId": 2, "steamId": "76561197960265730", "name": "Ben", "state": "inGame",
         "game": {"id": "0198d97d-1111-7000-8000-000000000001", "name": "Portal", "steamAppId": 400},
         "status": "Test chamber 04", "richPresence": {"status": "Test chamber 04"}, "playingSince": 1790000000},
        {"accountId": 3, "steamId": "76561197960265731", "name": "Carl", "state": "online"},
        {"accountId": 4, "steamId": "76561197960265732", "name": "Dora", "state": "offline", "lastOnlineAt": 1790000000},
    ], "incoming": 1, "notify": "always", "thisGame": "0198d97d-1111-7000-8000-000000000002"},
    "notices": {"notices": [{"id": 1, "kind": "request", "title": "Carl sent you a friend request",
                             "detail": "Accept or decline it in the Stoker launcher, under Friends."}]},
    "playtime": {"totalSeconds": 44640, "recentSeconds": 7380, "lastPlayedAt": 1790000000},
    # Two of the topics an integration subscribes to itself (stoker_events_subscribe): no lobbies, no Stop asked.
    "lobbies": {"lobbies": []},
    "shutdown": {"requests": []},
}


# The overlay's Controller window (capability controller 1): a few rows as the agent's ControllerEditor words them,
# their lists, and a change that takes the chosen choice's words. South reads as pressed, as if a thumb were on it.
CONTROLLER = {
    "available": True, "enabled": True, "isDefault": True,
    "pads": [{"slot": 0, "name": "DualSense Wireless Controller", "kind": "PlayStation 5"}],
    "controls": [
        {"id": "Preset", "group": "Layout", "label": "Start from", "value": "", "text": "Choose a layout", "choices": "preset"},
        {"id": "South", "group": "Buttons", "label": "Cross", "value": "pad:A", "text": "Xbox A", "choices": "binding"},
        {"id": "East", "group": "Buttons", "label": "Circle", "value": "pad:B", "text": "Xbox B", "choices": "binding"},
        {"id": "LeftShoulder", "group": "Buttons", "label": "L1", "value": "pad:LeftShoulder", "text": "Xbox LB", "choices": "binding"},
        {"id": "LeftTrigger.Mode", "group": "Triggers", "label": "L2", "value": "Analog", "text": "Trigger", "choices": "trigger"},
        {"id": "LeftStick.Mode", "group": "Sticks", "label": "Left stick", "value": "Stick:Left", "text": "Left stick", "choices": "stick"},
        {"id": "Gyro.Mode", "group": "Gyro", "label": "Gyro", "value": "Off", "text": "Off", "choices": "gyro"},
    ],
    "choices": {
        "binding": [{"value": "none", "text": "Nothing", "group": "Nothing"}]
                   + [{"value": "pad:" + b, "text": "Xbox " + b, "group": "Xbox pad"} for b in ["A", "B", "X", "Y"]]
                   + [{"value": "mouse:Left", "text": "Left click", "group": "Mouse"}]
                   + [{"value": "key:" + k, "text": "Key " + k, "group": "Keyboard"} for k in ["Space", "E", "Q"]],
        "trigger": [{"value": "Analog", "text": "Trigger", "group": "Mode"}, {"value": "Button", "text": "A button", "group": "Mode"}],
        "stick": [{"value": "Stick:Left", "text": "Left stick", "group": "Mode"}, {"value": "Mouse", "text": "Mouse", "group": "Mode"}],
        "gyro": [{"value": "Off", "text": "Off", "group": "Mode"}, {"value": "Mouse", "text": "Mouse", "group": "Mode"}],
        "preset": [{"value": "gamepad", "text": "Gamepad", "group": "Layouts"}],
    },
}


def controller_change(body):
    for row in CONTROLLER["controls"]:
        if row["id"] == body.get("control"):
            for choice in CONTROLLER["choices"][row["choices"]]:
                if choice["value"] == body.get("value"):
                    row["value"], row["text"] = choice["value"], choice["text"]
                    CONTROLLER["isDefault"] = False
                    return None
    return {"code": "request.invalid", "message": "not a choice"}


def frame(message):
    payload = json.dumps(message).encode()
    return struct.pack("<I", len(payload)) + payload


def read(conn, count):
    data = b""
    while len(data) < count:
        chunk = conn.recv(count - len(data))
        if not chunk:
            raise EOFError
        data += chunk
    return data


def serve(conn):
    integration = None
    try:
        if tcp_port_file:
            first = json.loads(read(conn, struct.unpack("<I", read(conn, 4))[0]))
            if first.get("v") != 1 or first.get("type") != "auth" or first.get("token") != token:
                with log_lock, open(out, "a") as f:
                    f.write(json.dumps({"op": "_refused", "first": first}) + "\n")
                return
        while True:
            request = json.loads(read(conn, struct.unpack("<I", read(conn, 4))[0]))
            if request["op"] == "session.hello":
                integration = request.get("body", {}).get("integration", {}).get("name")
            with log_lock, open(out, "a") as f:
                f.write(json.dumps(dict(request, _integration=integration)) + "\n")

            reply = {"v": 1, "type": "response", "id": request["id"], "ok": True, "body": {}}

            if request["op"] == "events.subscribe":
                # As the agent does: the answer, each topic's state at once, then only events.
                topics = [t for t in request.get("body", {}).get("topics", []) if t in EVENTS]
                reply["body"] = {"topics": topics}
                conn.sendall(frame(reply))
                for topic in topics:
                    conn.sendall(frame({"v": 1, "type": "event", "topic": topic, "body": EVENTS[topic]}))
                while True:
                    read(conn, struct.unpack("<I", read(conn, 4))[0])   # discarded, never answered

            if request["op"] == "session.hello":
                if refuse:
                    reply = {"v": 1, "type": "response", "id": request["id"], "ok": False,
                             "error": {"code": refuse, "message": "refused by the test"}}
                else:
                    reply["body"] = {"protocol": 1, "session": "runtime-test", "capabilities": {"achievements": 2, "events": 1, "telemetry": 1, "controller": 1, "leaderboards": 2, "crash": 1},
                                     "player": {"name": "Test Player", "steamId": "76561197960265729"}}
            elif request["op"] == "controller.get":
                reply["body"] = CONTROLLER
            elif request["op"] == "controller.change":
                problem = controller_change(request.get("body", {}))
                reply = ({"v": 1, "type": "response", "id": request["id"], "ok": False, "error": problem}
                         if problem else dict(reply, body=CONTROLLER))
            elif request["op"] == "controller.preview":
                reply["body"] = {"pressed": ["South"]}
            elif request["op"] == "achievement.list":
                reply["body"] = {"achievements": [{"id": "ALREADY", "achieved": True, "aliases": ["uplay_7"], "globalPercent": 61.3},
                                                  {"id": "NEW", "name": "Brand new", "description": "Unlocked by runtime_test", "hidden": True, "achieved": False, "aliases": ["uplay_8"]}]}
            elif request["op"] == "achievement.icon":
                body = request.get("body", {})
                if body.get("id") == "NEW" and body.get("achieved"):
                    reply["body"] = {"found": True, "type": "image/png", "data": PNG}
                else:
                    reply["body"] = {"found": False}
            elif request["op"] in fake_leaderboards.OPS:
                with leaderboards_lock:
                    body, error = LEADERBOARDS.answer(request["op"], request.get("body"))
                reply = ({"v": 1, "type": "response", "id": request["id"], "ok": False, "error": error}
                         if error else dict(reply, body=body))

            conn.sendall(frame(reply))
    except (EOFError, ConnectionResetError, BrokenPipeError):
        pass
    finally:
        conn.close()


# Every connection on its own thread, as the agent does: the overlay's event connection stays open
# for as long as the game's own does.
while True:
    conn, _ = server.accept()
    threading.Thread(target=serve, args=(conn,), daemon=True).start()
