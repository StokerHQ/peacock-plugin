/*
 *     Stoker's Peacock plugin: HITMAN World of Assassination's entitlements from Stoker.
 *
 *     This file is a plugin for The Peacock Project (https://github.com/thepeacockproject/Peacock),
 *     a HITMAN server replacement, and uses its internals through Peacock's plugin require table.
 *     It is therefore distributed under the same licence:
 *
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU Affero General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU Affero General Public License for more details.
 *
 *     You should have received a copy of the GNU Affero General Public License
 *     along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// A Stoker player's game runs on Stoker's Steamworks shim, whose session tickets are signed by the
// player's stokerd, not by Valve. Peacock's Steam entitlement check verifies Valve's signature, so
// it would find nothing and fall back to IOI's servers, which refuse a Stoker ticket too, and every
// DLC would stay locked. This plugin answers that question from the install instead: the DLC
// Stoker's licences put into it, which the launcher records in `stoker_dlc.txt` in the install's
// data folder (STOKER_INSTALL_DIR, inherited from the game that started this server).
//
// It also keeps Peacock from signing in to IOI's servers with a Stoker ticket. That sign-in only
// feeds features that need IOI's servers, and with a Stoker ticket it can only fail.
//
// And it gives Peacock's leaderboards to Stoker: every Stoker player of the instance on one board per contract, through
// the launcher's own protocol ("Leaderboards" below, stoker-agent.js).
//
// Peacock listens on loopback only when started by Stoker's hook plugin (HOST=127.0.0.1), so the
// one client is the game on this machine, and its entitlements are this install's.

const { readFileSync } = require("fs")
const { join } = require("path")
// Peacock runs a plugin in a context of its own, which has neither timer globals nor Buffer.
const { setInterval, clearInterval, setTimeout } = require("timers")
const { Buffer } = require("buffer")
const { SteamStrategy } = require("@peacockproject/core/entitlementStrategies")
const { OfficialServerAuth } = require("@peacockproject/core/officialServerAuth")
const { STEAM_NAMESPACE_2021 } = require("@peacockproject/core/platformEntitlements")
const { log, LogLevel } = require("@peacockproject/core/loggingInterop")
const { asyncGuard } = require("@peacockproject/core/databaseHandler")
const { setFlag, getFlag, defaultFlags } = require("@peacockproject/core/flags")
const { webFeaturesRouter } = require("@peacockproject/core/webFeatures")
const { PEACOCKVERSTRING } = require("@peacockproject/core/utils")

// The launcher's protocol client, from beside this file, where the hook plugin writes both, by absolute path: this
// plugin runs in a context whose require resolves a relative name against Peacock, not against this file. Peacock
// loads only *.plugin.js as plugins, so it is never one itself.
const { StokerAgent } = require(join(require("path").dirname(__filename), "stoker-agent.js"))

const DLC_FILE = "stoker_dlc.txt"

/** How often the game is looked for, in milliseconds. */
const GAME_POLL = 1000

/** How long the last writes may take once the game is gone, in milliseconds. */
const FLUSH_LIMIT = 10000

/**
 * The Steam AppIds of the DLC in this install: one `appId=name` line each, as the launcher writes
 * them. A missing file means the install carries no DLC.
 *
 * @returns {string[]}
 */
function installedDlc() {
    const folder = process.env.STOKER_INSTALL_DIR

    if (!folder) {
        log(
            LogLevel.WARN,
            "STOKER_INSTALL_DIR is not set, so no DLC can be read; was this server started by Stoker?",
            "stoker",
        )
        return []
    }

    let text

    try {
        text = readFileSync(join(folder, DLC_FILE), "utf8")
    } catch (e) {
        if (e && e.code === "ENOENT") return []
        log(LogLevel.WARN, `Could not read ${DLC_FILE}: ${e}`, "stoker")
        return []
    }

    const ids = []

    for (const line of text.split(/\r?\n/)) {
        const match = /^\s*(\d{1,10})\s*(=|$)/.exec(line)
        if (match && !ids.includes(match[1])) ids.push(match[1])
    }

    return ids
}

/**
 * The entitlements of HITMAN World of Assassination on Steam: the game's own AppId first, then its
 * DLC, which is the shape a Valve-verified ticket gives Peacock.
 *
 * @returns {string[]}
 */
function entitlements() {
    const dlc = installedDlc().filter((id) => id !== STEAM_NAMESPACE_2021)
    return [STEAM_NAMESPACE_2021, ...dlc]
}

/**
 * Whether the process `pid` still runs. Signal 0 only checks; Node raises ESRCH for a process that
 * is gone and EPERM for one that runs under another account.
 *
 * @param {number} pid
 */
function running(pid) {
    try {
        process.kill(pid, 0)
        return true
    } catch (e) {
        return e && e.code === "EPERM"
    }
}

/**
 * Ends this server once the game that started it has exited, after writing every loaded profile.
 *
 * Peacock writes a changed profile on a timer, so killing it with the game could lose the last few
 * seconds of progress, and the launcher collects this folder's saves as soon as the run ends - a run
 * that includes this process. The game's process id comes from the hook plugin that started this
 * server (STOKER_PEACOCK_GAME_PID); without it the server runs until it is stopped, as Peacock
 * always does.
 */
function exitWithGame() {
    const pid = Number.parseInt(process.env.STOKER_PEACOCK_GAME_PID ?? "", 10)
    if (!Number.isInteger(pid) || pid <= 0) return

    const timer = setInterval(() => {
        if (running(pid)) return
        clearInterval(timer)

        log(LogLevel.INFO, `The game (${pid}) has exited; writing profiles and stopping.`, "stoker")
        setTimeout(() => process.exit(0), FLUSH_LIMIT).unref()

        asyncGuard
            .forceFlush()
            .catch((e) => log(LogLevel.ERROR, `Writing profiles failed: ${e}`, "stoker"))
            .finally(() => process.exit(0))
    }, GAME_POLL)
}

// ── Leaderboards ─────────────────────────────────────────────────────────────────────────
//
// Peacock keeps its leaderboards on a separate service (its leaderboardsHost): a POST per finished contract, and a
// page of entries per contract, game version, platform and difficulty. Pointed at its own server here, under /_wf,
// whose routes take no session, these two routes answer it from Stoker's leaderboards, through the launcher: every
// Stoker player of the instance shares one board per contract and difficulty, under their Stoker names, offline
// uploads are kept by the launcher and sent later, and nothing goes to Peacock's public service. Peacock's own code
// builds the request and reads the answer exactly as it would from its own service.

/** The routes' place under Peacock's /_wf router, and so in leaderboardsHost. */
const LEADERBOARD_ROUTE = "/stoker"

/** Peacock's leaderboard service answers 100 entries a page; Peacock guesses there is another page from a full one. */
const PAGE = 100

/** The largest score a Stoker leaderboard keeps: a 32-bit integer. */
const MAX_SCORE = 2147483647

let agent

/** Board ids by name, so each board is looked up once per run. */
const boards = new Map()

/**
 * The Stoker board for a contract on one difficulty of one game version. Every platform's players share it: on a
 * Stoker instance they are all Stoker players, whichever store the game came from.
 */
function boardName(params) {
    return `peacock:${params.gameVersion}:${params.contractId}:${params.difficulty}`
}

async function boardId(name, create) {
    if (boards.has(name)) return boards.get(name)

    const found = await agent.request("leaderboard.find", create
        ? { name, create: true, sort: "descending", display: "numeric" }
        : { name })
    if (!found.found) return undefined

    boards.set(name, found.id)
    return found.id
}

/** A JSON request body: Peacock posts JSON, and /_wf parses no bodies of its own. */
function readBody(req) {
    if (req.body && typeof req.body === "object" && Object.keys(req.body).length) return Promise.resolve(req.body)

    return new Promise((resolve, reject) => {
        const chunks = []
        req.on("data", (chunk) => chunks.push(chunk))
        req.on("error", reject)
        req.on("end", () => {
            try {
                resolve(JSON.parse(Buffer.concat(chunks).toString("utf8") || "{}"))
            } catch (e) {
                reject(e)
            }
        })
    })
}

/** One of Stoker's rows as Peacock's service shapes an entry, which the game's leaderboard templates read. */
function entryOf(row) {
    const data = row.data && typeof row.data === "object" ? row.data : {}
    return {
        entryId: row.rank,
        platformId: row.steamId,
        LeaderboardData: {
            Player: { displayName: row.name || "Stoker player" },
            Rank: row.rank,
            Score: row.score,
            Data: data,
        },
        detailedscore: { Headlines: Array.isArray(data.Headlines) ? data.Headlines : [] },
    }
}

function serveLeaderboards() {
    const contract = `${LEADERBOARD_ROUTE}/leaderboards/contracts/:contractId/:gameVersion/:platform/:difficulty`

    webFeaturesRouter.post(contract, async (req, res) => {
        try {
            const body = await readBody(req)
            const id = await boardId(boardName(req.params), true)
            const score = Math.max(-MAX_SCORE, Math.min(MAX_SCORE, Math.trunc(Number(body.score) || 0)))

            // The player is whoever is signed in to the launcher, never the name or id Peacock sends.
            const upload = { id, score, keepBest: true }
            if (agent.capability("leaderboards") >= 2 && body.data && typeof body.data === "object") upload.data = body.data

            const answer = await agent.request("leaderboard.upload", upload)
            log(
                LogLevel.INFO,
                answer.queued
                    ? `Leaderboard ${boardName(req.params)}: ${score} kept by the launcher until the server can be reached.`
                    : `Leaderboard ${boardName(req.params)}: ${score}, rank ${answer.rank} of ${answer.entryCount}.`,
                "stoker",
            )
            res.json({})
        } catch (e) {
            log(LogLevel.WARN, `A leaderboard score could not be handed to the launcher: ${e?.message ?? e}`, "stoker")
            res.status(502).json({})
        }
    })

    webFeaturesRouter.get(`${contract}/entries`, async (req, res) => {
        try {
            const page = Math.max(1, Number.parseInt(req.query.page, 10) || 1)
            const id = await boardId(boardName(req.params), false)
            if (!id) {
                res.json([])
                return
            }

            const answer = await agent.request("leaderboard.entries", {
                id,
                range: "global",
                start: (page - 1) * PAGE + 1,
                end: page * PAGE,
            })
            res.json((answer.entries ?? []).map(entryOf))
        } catch (e) {
            log(LogLevel.WARN, `The leaderboard could not be read from the launcher: ${e?.message ?? e}`, "stoker")
            res.json([])
        }
    })
}

/** How long start-up waits for the launcher's hello before leaving the leaderboards to Peacock. */
const ADMISSION_TIMEOUT = 3000

/**
 * Gives the leaderboards back to Peacock's own service, switched off as the overlay's options ship them, when
 * options.ini still points them at Stoker's routes. Peacock writes its options at every start, so it keeps the
 * address an earlier run set - with that run's port, which the hook plugin chooses afresh each time - and without
 * the launcher those routes are not served.
 */
function releaseLeaderboards() {
    if (!String(getFlag("leaderboardsHost") ?? "").includes(`/_wf${LEADERBOARD_ROUTE}`)) return
    setFlag("leaderboards", false)
    setFlag("leaderboardsHost", defaultFlags.peacock.flags.leaderboardsHost.default)
}

/**
 * Points Peacock's leaderboards at Stoker's, when the launcher admits this server as part of the game's run - which
 * only a launcher that started the game does. Otherwise the options are left as they are, and Peacock uses its own
 * service as it always does.
 */
async function useStokerLeaderboards() {
    const candidate = new StokerAgent({
        name: "peacock",
        version: "1.0.0",
        engine: { name: "peacock", version: String(PEACOCKVERSTRING) },
    })

    if (!StokerAgent.available() || !(await candidate.admitted(ADMISSION_TIMEOUT)) || candidate.capability("leaderboards") < 1) {
        releaseLeaderboards()
        log(LogLevel.INFO, "Not part of a game the Stoker launcher started; the leaderboards are Peacock's own.", "stoker")
        return
    }

    agent = candidate
    serveLeaderboards()

    const port = process.env.PORT || "80"
    setFlag("leaderboards", true)
    setFlag("leaderboardsHost", `http://127.0.0.1:${port}/_wf${LEADERBOARD_ROUTE}`)
    log(LogLevel.INFO, "Leaderboards: every Stoker player's, through the launcher.", "stoker")
}

module.exports = async function stokerPlugin() {
    SteamStrategy.prototype.get = async function stokerEntitlements() {
        const ents = entitlements()
        log(
            LogLevel.INFO,
            `Entitlements from Stoker: ${ents.length - 1} DLC besides the game.`,
            "stoker",
        )
        return ents
    }

    OfficialServerAuth.prototype._initiallyAuthenticate =
        async function stokerSkipsOfficial() {
            this.initialized = false
        }

    OfficialServerAuth.prototype._doRefresh = async function stokerSkipsRefresh() {
        this.initialized = false
    }

    log(
        LogLevel.INFO,
        "Stoker: entitlements come from this install's Stoker licences; IOI's servers are not signed in to.",
        "stoker",
    )

    await useStokerLeaderboards()
    exitWithGame()
}
