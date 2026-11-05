"""
Capability leaderboards 2 as the agent answers it (an entry's JSON "data" beside its score),
in memory, for fake_agent.py and fake_agent_windows.py.

One board exists from the start: "Fastest", ascending, in milliseconds, with a rival's 5000 on it - a board the
Steam build of a game would have created. Any other is found only once a find has asked to create it. The player
is the one the fakes' hello names. An upload of "Unreachable" is answered queued, as the agent answers an upload
it had to keep for stokerd.
"""

import uuid

PLAYER = "76561197960265729"
RIVAL = "76561197960265730"

# As stokerd names each row's player (LeaderboardEntryResponse.PersonaName), which the agent passes on as "name".
NAMES = {PLAYER: "Test Player", RIVAL: "Rival Racer"}

OPS = ("leaderboard.find", "leaderboard.upload", "leaderboard.entries")


class Leaderboards:
    def __init__(self, player=PLAYER):
        # The player uploads are filed under: the hello's, which fake_agent_windows.py --one-player changes.
        self.player = player
        NAMES.setdefault(player, "Test Player")
        self.boards = {}
        self._add("Fastest", "ascending", "milliseconds")["scores"][RIVAL] = (5000, [], None)

    def _add(self, name, sort, display):
        board = {"id": str(uuid.uuid4()), "name": name, "sort": sort, "display": display, "scores": {}}
        self.boards[name] = board
        return board

    def _ranked(self, board):
        rows = sorted(board["scores"].items(), key=lambda item: item[1][0], reverse=board["sort"] == "descending")
        ranked = []
        for i, (steam_id, (score, details, data)) in enumerate(rows):
            row = {"steamId": steam_id, "name": NAMES.get(steam_id, ""), "rank": i + 1, "score": score, "details": details}
            if data is not None:
                row["data"] = data
            ranked.append(row)
        return ranked

    def _rank_of(self, board, steam_id):
        return next((row["rank"] for row in self._ranked(board) if row["steamId"] == steam_id), 0)

    def answer(self, op, body):
        """(body, None) for one of this capability's ops, (None, error) for a refusal; None for any other op."""
        if op not in OPS:
            return None
        body = body or {}

        if op == "leaderboard.find":
            board = self.boards.get(body.get("name"))
            if board is None and body.get("create"):
                if body.get("sort") not in ("ascending", "descending") or body.get("display") not in ("numeric", "seconds", "milliseconds"):
                    return None, {"code": "request.invalid", "message": "a new board needs sort and display"}
                board = self._add(body["name"], body["sort"], body["display"])
            if board is None:
                return {"found": False, "id": "", "name": body.get("name", ""), "sort": "none", "display": "none", "entryCount": 0}, None
            return {"found": True, "id": board["id"], "name": board["name"], "sort": board["sort"], "display": board["display"],
                    "entryCount": len(board["scores"])}, None

        board = next((b for b in self.boards.values() if b["id"] == body.get("id")), None)
        if board is None:
            return None, {"code": "backend.rejected", "message": "no such board for this game"}

        if op == "leaderboard.upload":
            if board["name"] == "Unreachable":
                return {"queued": True}, None
            score, details, data = body.get("score", 0), body.get("details", []), body.get("data")
            previous = board["scores"].get(self.player)
            better = previous is None or (score < previous[0] if board["sort"] == "ascending" else score > previous[0])
            before = self._rank_of(board, self.player)
            changed = better or not body.get("keepBest", True)
            if changed:
                board["scores"][self.player] = (score, details, data)
            return {"queued": False, "changed": changed, "rank": self._rank_of(board, self.player), "previousRank": before,
                    "entryCount": len(board["scores"])}, None

        rows = self._ranked(board)
        start, end, selected = body.get("start", 0), body.get("end", 0), body.get("range")
        if selected == "global":
            rows = [r for r in rows if start <= r["rank"] <= end]
        elif selected == "around":
            mine = self._rank_of(board, self.player)
            rows = [r for r in rows if mine and mine + start <= r["rank"] <= mine + end]
        elif selected == "friends":
            rows = [r for r in rows if r["steamId"] == self.player]
        elif selected == "players":
            rows = [r for r in rows if r["steamId"] in body.get("players", [])]
        else:
            return None, {"code": "request.invalid", "message": "unknown range"}
        return {"entries": rows, "entryCount": len(board["scores"])}, None
