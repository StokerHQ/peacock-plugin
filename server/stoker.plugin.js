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
// the launcher's own protocol ("Leaderboards" below, stoker-agent.js), and tells Stoker what the player is doing - which
// menu, which mission where, in which mode - better than the game's own Steam presence ("Presence" below).
//
// Peacock's menu routes are mounted behind its multiplayer menu router on the same path, so a route added there answers
// first: that is how this plugin serves the two leaderboard pages Peacock gets wrong or lacks, and how it sees which
// menu page the game opens.
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
const { multiplayerMenuDataRouter } = require("@peacockproject/core/multiplayer/multiplayerMenuData")
const { getLeaderboardEntries } = require("@peacockproject/core/contracts/leaderboards")
const { getConfig } = require("@peacockproject/core/configSwizzleManager")
const { fakePlayerRegistry } = require("@peacockproject/core/profileHandler")
const { scenePathToRpAsset } = require("@peacockproject/core/discord/discordRp")

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

        // Withdrawn, so the launcher forgets this run's status rather than holding it for nobody.
        presence?.set("")

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

/** A difficulty as the game sends it (1, 2, 4) under the name Peacock's boards use, as Peacock's own entries page names it. */
function difficultyName(level) {
    switch (Number.parseInt(level ?? "0", 10)) {
        case 1:
            return "casual"
        case 2:
            return "normal"
        case 4:
            return "master"
        default:
            return "unset"
    }
}

/** A menu page's page number as Peacock reads one: 0 when absent. */
function pageOf(value) {
    return Math.max(0, Number.parseInt(value ?? "0", 10) || 0)
}

/**
 * The board after a mission. Peacock's own route reads the difficulty from `difficulty`, but the game sends
 * `difficultyLevel` - as it does to the menu's leaderboard, which Peacock reads right - so Peacock asks for the "unset"
 * board, which holds nothing, and the board is empty until the player opens it again from the menu. Answered here as
 * Peacock answers it, with the difficulty the game sent. Served whichever leaderboards Peacock uses: its own service
 * keeps a board per difficulty too.
 */
function serveDebriefingLeaderboard() {
    multiplayerMenuDataRouter.get("/DebriefingLeaderboards", async (req, res, next) => {
        if (!req.query.contractid || req.query.difficulty !== undefined || req.query.difficultyLevel === undefined) {
            next()
            return
        }

        try {
            const data = await getLeaderboardEntries(
                req.query.contractid,
                req.jwt.platform,
                req.gameVersion,
                req.query.difficultyLevel,
                pageOf(req.query.page),
            )
            res.json({ template: getConfig("DebriefingLeaderboardsTemplate", false), data })
        } catch (e) {
            log(LogLevel.WARN, `The mission's leaderboard could not be read: ${e?.message ?? e}`, "stoker")
            next()
        }
    })
}

/**
 * The leaderboard's Friends tab, which Peacock does not serve at all: the same board, the player's Stoker friends and
 * the player alone (the launcher's `friends` range, as a Steam game's friends board), in the shape of the global tab.
 */
function serveFriendsLeaderboard(controller) {
    multiplayerMenuDataRouter.get("/FriendsLeaderboardEntries", async (req, res, next) => {
        const contract = req.query.contractid ? controller.resolveContract(req.query.contractid, req.gameVersion) : undefined
        if (!contract) {
            next()
            return
        }

        const data = { Entries: [], Contract: contract, Page: pageOf(req.query.page), HasMore: false, LeaderboardType: "singleplayer" }

        try {
            const name = boardName({
                gameVersion: req.gameVersion,
                contractId: req.query.contractid,
                difficulty: difficultyName(req.query.difficultyLevel),
            })
            const id = await boardId(name, false)
            if (id) {
                const answer = await agent.request("leaderboard.entries", { id, range: "friends", start: 1, end: PAGE })
                data.Entries = (answer.entries ?? []).map(entryOf).map((entry) => {
                    // As Peacock does for the global tab: the templates take a player by its id in this registry.
                    entry.LeaderboardData.Player = fakePlayerRegistry.index(
                        entry.LeaderboardData.Player.displayName,
                        req.jwt.platform,
                        entry.platformId,
                    )
                    return entry
                })
            }
        } catch (e) {
            log(LogLevel.WARN, `The friends' leaderboard could not be read from the launcher: ${e?.message ?? e}`, "stoker")
        }

        res.json({ template: getConfig("LeaderboardEntriesTemplate", false), data })
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
 * Points Peacock's leaderboards at Stoker's, and serves the Friends tab from them. Otherwise the options are left as
 * they are, and Peacock uses its own service as it always does.
 */
function useStokerLeaderboards(controller) {
    serveLeaderboards()
    serveFriendsLeaderboard(controller)

    const port = process.env.PORT || "80"
    setFlag("leaderboards", true)
    setFlag("leaderboardsHost", `http://127.0.0.1:${port}/_wf${LEADERBOARD_ROUTE}`)
    log(LogLevel.INFO, "Leaderboards: every Stoker player's, through the launcher.", "stoker")
}

/**
 * The launcher, once it admits this server as part of the game's run - which only a launcher that started the game
 * does - or undefined.
 */
async function connectToLauncher() {
    const candidate = new StokerAgent({
        name: "peacock",
        version: "1.1.0",
        engine: { name: "peacock", version: String(PEACOCKVERSTRING) },
    })

    if (!StokerAgent.available() || !(await candidate.admitted(ADMISSION_TIMEOUT))) return undefined
    return candidate
}

// ── Presence ─────────────────────────────────────────────────────────────────────────────
//
// What the player is doing, for their friends: the game's own Steam presence says "In Menu" and nothing else under
// Peacock, while Peacock knows the menu page, the contract and how it is played. The launcher prefers this status over
// the game's for the rest of the run (presence.set, integration-protocol.md), and puts the game's back when this one
// is withdrawn at exit.
//
// Three things move it: the menu page the game opens (Hub, Planning, the mission's end), and any event of a contract
// session - which is how a mission started from the planning screen and one loaded from a save both look.

/** Parent locations by the start of their location id (LOCATION_<key>…), for a scene Peacock's table does not name. */
const LOCATIONS = {
    PARIS: "Paris",
    COASTALTOWN: "Sapienza",
    MARRAKECH: "Marrakesh",
    BANGKOK: "Bangkok",
    COLORADO: "Colorado",
    HOKKAIDO: "Hokkaido",
    NEWZEALAND: "Hawke's Bay",
    MIAMI: "Miami",
    COLOMBIA: "Santa Fortuna",
    MUMBAI: "Mumbai",
    NORTHAMERICA: "Whittleton Creek",
    NORTHSEA: "Isle of Sgàil",
    GREEDY: "New York",
    OPULENT: "Haven Island",
    GOLDEN: "Dubai",
    ANCESTRAL: "Dartmoor",
    EDGY: "Berlin",
    WET: "Chongqing",
    ELEGANT: "Mendoza",
    TRAPPED: "Carpathian Mountains",
    ROCKY: "Ambrose Island",
    ICA_FACILITY: "ICA Facility",
    AUSTRIA: "Himmelstein",
    SALTY: "Hantu Port",
    CAGED: "Siberia",
}

/** How a contract is played, by its type, as the game's menus name the modes. A main mission names none. */
const MODES = {
    elusive: "Elusive Target",
    escalation: "Escalation",
    arcade: "Arcade",
    evergreen: "Freelancer",
    sniper: "Sniper Assassin",
    featured: "Contract",
    usercreated: "Contract",
    creation: "Contract Creation",
    orbis: "Sarajevo Six",
    vsrace: "Ghost Mode",
    tutorial: "Training",
}

/** How long a mission's events after leaving it are taken as the game leaving it, in milliseconds. */
const LATE_EVENTS = 10000

/** The game's difficulty levels, as its menus name them. */
const DIFFICULTIES = { 1: "Casual", 2: "Professional", 4: "Master" }

/** A title a person can read: a contract's own words, never a localisation key (UI_CONTRACT_…), which only the game reads. */
function readable(text) {
    return typeof text === "string" && text.trim() !== "" && !/^[A-Z0-9_]+$/.test(text.trim()) ? text.trim() : undefined
}

function locationName(id) {
    const key = String(id ?? "").replace(/^LOCATION_/, "").replace(/^PARENT_/, "")
    const match = Object.keys(LOCATIONS)
        .filter((name) => key === name || key.startsWith(`${name}_`))
        .sort((a, b) => b.length - a.length)[0]
    return match ? LOCATIONS[match] : undefined
}

/** An escalation level's number within its group, from the controller's level-to-contract mapping. */
function escalationLevel(controller, metadata) {
    const mapping = metadata.InGroup ? controller.escalationMappings?.get(metadata.InGroup) : undefined
    const level = mapping ? Object.entries(mapping).find(([, id]) => id === metadata.Id)?.[0] : undefined
    return level ? `Level ${level}` : undefined
}

/**
 * The words for a contract: its mode, its name, and where. Peacock's own table for its Discord presence names a
 * scene's mission and location (The Showstopper, Paris); a contract's own title is used where it is words.
 */
function describeContract(controller, contract, gameVersion) {
    const metadata = contract?.Metadata ?? {}
    const type = String(metadata.Type ?? "").toLowerCase()

    let scene = []
    try {
        const asset = scenePathToRpAsset(String(metadata.ScenePath ?? ""), contract?.Data?.Bricks ?? [])
        if (Array.isArray(asset)) scene = asset
    } catch {
        // Peacock's table is a convenience; the location table answers without it.
    }

    const location = scene[2] ?? locationName(metadata.Location)
    const ownTitle = readable(metadata.Title)

    switch (type) {
        case "mission":
        case "campaign":
            return [ownTitle ?? scene[1], location]
        case "elusive":
        case "orbis":
            return [MODES[type], scene[1] ?? ownTitle, location]
        case "escalation": {
            const group = metadata.InGroup ? controller.resolveContract(metadata.InGroup, gameVersion) : undefined
            return [MODES.escalation, readable(group?.Metadata?.Title) ?? ownTitle, escalationLevel(controller, metadata), location]
        }
        case "arcade":
            return [MODES.arcade, escalationLevel(controller, metadata), location]
        default:
            return [MODES[type], ownTitle, location]
    }
}

/** The parts as one line: empty parts and a part that repeats the one before it left out. */
function line(parts) {
    const kept = []
    for (const part of parts) {
        if (part && part !== kept[kept.length - 1]) kept.push(part)
    }
    return kept.join(" · ")
}

class Presence {
    constructor(controller, launcher) {
        this.controller = controller
        this.launcher = launcher
        this.sent = undefined
        this.missionSession = undefined
    }

    /** Sends a status unless it is the one sent last. An empty status withdraws it. Best effort: never awaited. */
    set(status) {
        if (status === this.sent) return
        this.sent = status
        log(LogLevel.INFO, status ? `Presence: ${status}` : "Presence withdrawn.", "stoker")
        this.launcher
            .request("presence.set", status ? { status } : {})
            .catch((e) => log(LogLevel.DEBUG, `The presence could not be handed to the launcher: ${e?.message ?? e}`, "stoker"))
    }

    /**
     * Leaves the mission being played. Its session's events the game sends as it leaves - for a few seconds - do not
     * bring it back; a save of it loaded later, which carries the same session, does.
     */
    leave() {
        if (this.missionSession) {
            this.left = this.missionSession
            this.leftAt = Date.now()
        }
        this.missionSession = undefined
    }

    /** Whether an event belongs to the mission just left, sent as the game left it. */
    lateFromLeft(session) {
        return session.Id === this.left && Date.now() - this.leftAt < LATE_EVENTS
    }

    menu() {
        this.leave()
        this.set("In Menu")
    }

    planning(contractId, gameVersion) {
        const contract = this.controller.resolveContract(contractId, gameVersion)
        if (!contract) return
        this.leave()
        this.set(line(["Planning", ...describeContract(this.controller, contract, gameVersion)]))
    }

    /** Any event of a contract session: the player is in that mission, however they got there. */
    event(session, gameVersion) {
        if (!session?.contractId || session.Id === this.missionSession || this.lateFromLeft(session)) return
        const contract = this.controller.resolveContract(session.contractId, gameVersion)
        if (!contract) return
        this.missionSession = session.Id
        this.set(line([...describeContract(this.controller, contract, gameVersion), DIFFICULTIES[session.difficulty]]))
    }

    debrief() {
        this.leave()
        this.set("In Menu · Mission debrief")
    }

    /** The menu page the game opened, by its route under /profiles/page. Pages a mission's pause menu opens change nothing. */
    page(path, query, gameVersion) {
        switch (path.toLowerCase()) {
            case "/hub":
            case "/destination":
                this.menu()
                break
            case "/planning":
                if (query.contractid) this.planning(query.contractid, gameVersion)
                break
            case "/missionend":
            case "/missionendready":
                this.debrief()
                break
            default:
                break
        }
    }

    /** Starts following the game: menu pages through the router in front of Peacock's, missions through its events. */
    follow() {
        multiplayerMenuDataRouter.use((req, res, next) => {
            try {
                if (req.method === "GET") this.page(req.path, req.query, req.gameVersion)
            } catch (e) {
                log(LogLevel.DEBUG, `Presence: ${e?.message ?? e}`, "stoker")
            }
            next()
        })

        this.controller.hooks.newEvent.tap("StokerPresence", (event, details, session) => {
            try {
                this.event(session, details?.gameVersion ?? session?.gameVersion)
            } catch (e) {
                log(LogLevel.DEBUG, `Presence: ${e?.message ?? e}`, "stoker")
            }
        })

        this.menu()
    }
}

/** The presence this run sends, once the launcher takes it; withdrawn when the game exits. */
let presence

module.exports = async function stokerPlugin(controller) {
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

    serveDebriefingLeaderboard()

    const launcher = await connectToLauncher()
    if (!launcher) {
        releaseLeaderboards()
        log(LogLevel.INFO, "Not part of a game the Stoker launcher started; the leaderboards are Peacock's own.", "stoker")
    } else {
        if (launcher.capability("leaderboards") >= 1) {
            agent = launcher
            useStokerLeaderboards(controller)
        } else {
            releaseLeaderboards()
        }

        if (launcher.capability("presence") >= 1) {
            presence = new Presence(controller, launcher)
            presence.follow()
        }
    }

    exitWithGame()
}
