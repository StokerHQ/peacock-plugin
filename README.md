# Peacock plugin for Stoker

Runs HITMAN World of Assassination against a local [Peacock](https://thepeacockproject.org/) server under
Stoker, in place of IOI's online services: progression, unlocks, challenges,
escalations, contracts. Stoker supplies who is playing and which DLC they own; this plugin starts Peacock with the game,
points the game at it, and stops it with the game. Nobody starts Peacock or PeacockPatcher by hand.

Plugin key **`app.stokerhq.peacock`**, kind `hook`, Windows x64. Licence: GNU Affero General Public License version 3 or
later (`LICENSE`), as Peacock's: the patches it makes are PeacockPatcher's, found by a port of its scanner.

## What is here

| Path | What it is |
|---|---|
| `hook-plugin/peacock.cpp` | `peacock.dll`, loaded by Stoker into `HITMAN3.exe` before the game's first instruction. Starts the Peacock release laid into the install, writes the Stoker plugin for Peacock into Peacock's working folder (`%LOCALAPPDATA%\Stoker Peacock`) at every start, seeds Peacock's options there the first time, and makes PeacockPatcher's patches: certificate pinning off, the auth header always sent, the server address, plain HTTP, dynamic resources |
| `hook-plugin/hitman_scan.h` | The port of PeacockPatcher's pattern scanner. Portable C++ |
| `hook-plugin/peacock.rc` | Carries `server/stoker.plugin.js` and `server/stoker-agent.js` inside `peacock.dll` |
| `server/stoker.plugin.js` | The Stoker plugin for Peacock: entitlements from the install's DLC list, no sign-in to IOI, leaderboards shared by every Stoker player (with the board after a mission at the difficulty played, and a Friends tab), what the player is doing for their friends - menu, planning, mission, mode, difficulty - and every profile written before Peacock exits with its game |
| `server/stoker-agent.js` | The Stoker launcher's integration protocol, which the plugin speaks to the launcher |
| `stoker-plugin.yaml` | The plugin's key, kind and target, and the Stoker plugin SDK release it builds against (StokerHQ/plugin-sdk), pinned by version and the SHA-256 of its zip |
| `scripts/fetch-sdk.ps1`, `.sh` | Fetch that SDK release once into a local cache, check its hash, and print its folder |
| `New-PeacockOverlay.ps1` | Lays a downloaded Peacock release out as `Peacock\`, unchanged, for an install |
| `tests/` | `Test-Peacock.ps1` (everything; no game needed), `Test-PeacockPlugin.ps1` (the Peacock plugin against a release), and the stand-in launcher they talk to |

## Building

Visual Studio with the C++ workload. `build.ps1` fetches the SDK release `stoker-plugin.yaml` pins (into
`%LOCALAPPDATA%\StokerHQ\plugin-sdk`) and builds with its helper; `STOKER_PLUGIN_SDK` names a local SDK folder or zip
instead, which is how an unreleased SDK change is tried:

```powershell
./build.ps1                       # build\x64\peacock.dll
./tests/Test-Peacock.ps1 -Peacock Peacock-v8.10.1.zip
```

The tests need no HITMAN and no Steam. With `-Peacock` (the Windows release from
<https://github.com/thepeacockproject/Peacock/releases>), they start the real Peacock from an unchanged release laid
into a stand-in install. Python 3 runs the stand-in launcher; WSL with g++ adds a Linux build of the scanner.

## How Stoker uses it

A Stoker recipe for HITMAN World of Assassination names this plugin by key and version, with `atLaunch: true`, and lays
the official Peacock release into the install as `Peacock\`. A Stoker instance finds the plugin in the StokerHQ plugin
registry, checks it, and installs it once an administrator approves the version.

## Releases

A release is tagged `v<version>`, the version `peacock.cpp`'s manifest declares, with `peacock.dll` attached as
`peacock-windows-x64.dll`. It is signed by the StokerHQ publisher key with `stoker-registry version`, and listed in the
StokerHQ registry by a pull request carrying that entry. Releases are made by hand; the CI here only builds.
