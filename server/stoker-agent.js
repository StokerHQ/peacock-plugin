/*
 *     Stoker's Peacock plugin: the launcher's integration protocol, from Peacock.
 *
 *     Part of Stoker's Peacock plugin (stoker.plugin.js), and distributed under the same licence, the GNU Affero
 *     General Public License version 3 or later.
 */

// A client of the Stoker launcher's integration protocol (version 1): length-prefixed
// JSON frames, session.hello, then requests answered in order. Peacock runs as the game's child, so it inherits the
// game's STOKER_AGENT_TCP and STOKER_AGENT_TOKEN and is admitted as the game's own run - the agent decides which
// game and which player, never this client.
//
// TCP with the token first: it is what the launcher gives every game, and the one transport a Windows node.exe under
// Wine on a Linux launcher can reach. The user's named pipe (STOKER_AGENT_PIPE, else stoker-agent-<SID>) otherwise,
// where the run is the kernel's Job membership instead of the token.

const net = require("net")
const { execFileSync } = require("child_process")

/** A frame larger than this is not one the agent sends; a reader seeing it has lost its place. */
const MAX_FRAME = 16 * 1024 * 1024

/** How long one request may wait for its answer before the connection is given up. */
const REQUEST_TIMEOUT = 15000

function endpoint() {
    const tcp = process.env.STOKER_AGENT_TCP
    const token = process.env.STOKER_AGENT_TOKEN
    if (tcp && token) {
        const at = tcp.lastIndexOf(":")
        const port = Number.parseInt(tcp.slice(at + 1), 10)
        if (at > 0 && Number.isInteger(port)) return { tcp: { host: tcp.slice(0, at), port }, token }
    }

    if (process.env.STOKER_AGENT_PIPE) return { pipe: process.env.STOKER_AGENT_PIPE }

    if (process.platform === "win32") {
        try {
            // The pipe carries the user's SID (integration-protocol.md, "Transports").
            const line = execFileSync("whoami", ["/user", "/fo", "csv", "/nh"], { encoding: "utf8", windowsHide: true })
            const sid = /"(S-1-[\d-]+)"/.exec(line)
            if (sid) return { pipe: `\\\\.\\pipe\\stoker-agent-${sid[1]}` }
        } catch {
            // No whoami: no pipe either.
        }
    }

    return undefined
}

function frame(object) {
    const json = Buffer.from(JSON.stringify(object), "utf8")
    const length = Buffer.alloc(4)
    length.writeUInt32LE(json.length, 0)
    return Buffer.concat([length, json])
}

class StokerAgent {
    /**
     * @param {{ name: string, version: string, engine: { name: string, version: string } }} self
     *     how the hello describes this client: descriptive only, for the agent's logs.
     */
    constructor(self) {
        this.self = self
        this.socket = undefined
        this.ready = undefined
        this.buffer = Buffer.alloc(0)
        this.pending = []
        this.nextId = 1
        this.capabilities = {}
        this.player = undefined
        this.chain = Promise.resolve()
    }

    /** Whether there is anywhere to look for the launcher at all - the environment it gives games, or this user's pipe. */
    static available() {
        return endpoint() !== undefined
    }

    /**
     * Connects and says hello, giving up after `timeout` milliseconds. Resolves true when the launcher admitted this
     * process - as the game's run, which is the only way a hello succeeds - and false otherwise.
     */
    async admitted(timeout) {
        let timer
        const late = new Promise((resolve) => {
            timer = setTimeout(() => resolve(false), timeout)
            timer.unref?.()
        })
        try {
            const admitted = await Promise.race([this._connect().then(() => true, () => false), late])
            // A connection that comes too late is not kept: nothing will use it.
            if (!admitted) this._reset(new Error("the launcher did not admit this process"))
            return admitted
        } finally {
            clearTimeout(timer)
        }
    }

    /** The version the agent offers of a capability, 0 when it offers none (or before the first connection). */
    capability(name) {
        return this.capabilities[name] ?? 0
    }

    /**
     * Sends one request and resolves with its answer's body. Requests are sent one at a time, so a reply is always
     * the answer to the request before it. Rejects with the agent's error ({ code, message }) or a transport failure.
     */
    request(op, body) {
        const run = () => this._connect().then(() => this._send(op, body))
        const result = this.chain.then(run, run)
        this.chain = result.catch(() => undefined)
        return result
    }

    _connect() {
        if (this.ready) return this.ready

        this.ready = new Promise((resolve, reject) => {
            const where = endpoint()
            if (!where) {
                reject(new Error("not started by the Stoker launcher"))
                return
            }

            const socket = where.tcp ? net.connect(where.tcp.port, where.tcp.host) : net.connect(where.pipe)
            this.socket = socket

            const fail = (error) => {
                this._reset(error)
                reject(error)
            }

            socket.once("error", fail)
            socket.on("data", (chunk) => this._read(chunk))
            socket.on("close", () => this._reset(new Error("the launcher closed the connection")))

            socket.once("connect", () => {
                // On TCP the first frame is the token, unanswered (integration-protocol.md, "The auth frame").
                if (where.token) socket.write(frame({ v: 1, type: "auth", token: where.token }))

                this._send("session.hello", {
                    protocol: { min: 1, max: 1 },
                    integration: { name: this.self.name, version: this.self.version },
                    engine: this.self.engine,
                }).then((hello) => {
                    socket.removeListener("error", fail)
                    socket.on("error", (error) => this._reset(error))
                    this.capabilities = hello.capabilities ?? {}
                    this.player = hello.player
                    resolve()
                }, fail)
            })
        })

        return this.ready
    }

    _send(op, body) {
        return new Promise((resolve, reject) => {
            const id = this.nextId++
            const timer = setTimeout(() => this._reset(new Error(`${op} was not answered`)), REQUEST_TIMEOUT)
            timer.unref?.()
            this.pending.push({ id, resolve, reject, timer })
            this.socket.write(frame({ v: 1, type: "request", id, op, body: body ?? {} }))
        })
    }

    _read(chunk) {
        this.buffer = Buffer.concat([this.buffer, chunk])

        while (this.buffer.length >= 4) {
            const length = this.buffer.readUInt32LE(0)
            if (length > MAX_FRAME) {
                this._reset(new Error("the launcher sent a frame this client cannot read"))
                return
            }
            if (this.buffer.length < 4 + length) return

            const text = this.buffer.subarray(4, 4 + length).toString("utf8")
            this.buffer = this.buffer.subarray(4 + length)

            let message
            try {
                message = JSON.parse(text)
            } catch {
                continue
            }
            if (message.type !== "response") continue

            const waiting = this.pending.shift()
            if (!waiting) continue
            clearTimeout(waiting.timer)

            if (message.ok) waiting.resolve(message.body ?? {})
            else waiting.reject(message.error ?? { code: "unknown", message: "the launcher refused the request" })
        }
    }

    /** Forgets the connection, failing whatever waits on it; the next request connects afresh. */
    _reset(error) {
        const socket = this.socket
        this.socket = undefined
        this.ready = undefined
        this.buffer = Buffer.alloc(0)

        for (const waiting of this.pending.splice(0)) {
            clearTimeout(waiting.timer)
            waiting.reject(error)
        }

        if (socket) socket.destroy()
    }
}

module.exports = { StokerAgent }
