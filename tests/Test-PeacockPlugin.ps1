<#
.SYNOPSIS
Runs a real Peacock release with Stoker's plugin and checks what the game would get.

.DESCRIPTION
Starts Peacock the way the hook plugin does - its own node, a data folder as the working folder,
loopback, STOKER_INSTALL_DIR and STOKER_PEACOCK_GAME_PID - and the launcher's agent as
tests/fake_agent.py (a copy of Stoker's) stands in for it, over TCP with a token, as a game's child
reaches the launcher. Then, as the game would:
  1. signs in as a Steam player with a ticket Valve never signed: the profile's entitlements are the
     game's AppId and the DLC in stoker_dlc.txt, and nothing signed in to IOI's servers;
  2. posts a contract's score to Peacock's leaderboard address and reads the board back - through
     the plugin's routes, and through Peacock's own LeaderboardEntries menu route, which proves the
     address was pointed at Stoker: the entry carries its score breakdown, under the Stoker name;
  3. lets the "game" exit: Peacock writes the profile and exits by itself.
A second run without the launcher checks that Peacock's own leaderboards are left alone, and that an
address an earlier run left in options.ini - with that run's port - is given back to Peacock.
The game is a stand-in process (ping). No HITMAN is needed.

.PARAMETER Peacock
The folder of an extracted Peacock release, or of an overlay's Peacock folder (the one holding
chunk0.js and nodedist).
#>
param(
    [Parameter(Mandatory)][string]$Peacock,
    [int]$Port = 41591
)

$ErrorActionPreference = 'Stop'
$server = Join-Path $PSScriptRoot '..\server'
$fakeAgent = Join-Path $PSScriptRoot 'fake_agent.py'
$node = Join-Path $Peacock 'nodedist\node.exe'
if (!(Test-Path (Join-Path $Peacock 'chunk0.js')) -or !(Test-Path $node)) { throw "$Peacock is not an extracted Peacock release." }

# On Top of the World, Dubai: a HITMAN 3 contract Peacock resolves; normal difficulty is level 2.
$contract = '7d85f2b0-80ca-49be-a2b7-d56f67faf252'
$failures = [Collections.Generic.List[string]]::new()

function Start-Peacock([string]$Data, [string]$Install, [int]$GamePid, [hashtable]$Agent) {
    $start = [Diagnostics.ProcessStartInfo]::new($node)
    $start.ArgumentList.Add((Join-Path $Peacock 'chunk0.js'))
    $start.WorkingDirectory = $Data
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($name in 'STOKER_AGENT_TCP', 'STOKER_AGENT_TOKEN', 'STOKER_AGENT_PIPE') { [void]$start.Environment.Remove($name) }
    $start.Environment['HOST'] = '127.0.0.1'
    $start.Environment['PORT'] = "$Port"
    $start.Environment['STOKER_INSTALL_DIR'] = $Install
    $start.Environment['STOKER_PEACOCK_GAME_PID'] = "$GamePid"
    if ($Agent) {
        $start.Environment['STOKER_AGENT_TCP'] = "127.0.0.1:$($Agent.Port)"
        $start.Environment['STOKER_AGENT_TOKEN'] = $Agent.Token
    }
    $process = [Diagnostics.Process]::Start($start)
    $output = $process.StandardOutput.ReadToEndAsync()
    $null = $process.StandardError.ReadToEndAsync()

    $listening = $false
    for ($i = 0; $i -lt 100 -and !$listening; $i++) {
        Start-Sleep -Milliseconds 200
        try { [Net.Sockets.TcpClient]::new('127.0.0.1', $Port).Dispose(); $listening = $true } catch { }
    }
    if (!$listening) { throw 'Peacock did not start listening.' }
    return @{ Process = $process; Output = $output }
}

function New-Folders([string]$Root) {
    $data = Join-Path $Root 'data'
    $install = Join-Path $Root 'install\.stoker'
    New-Item -ItemType Directory (Join-Path $data 'plugins'), $install | Out-Null
    Copy-Item (Join-Path $server 'stoker.plugin.js'), (Join-Path $server 'stoker-agent.js') (Join-Path $data 'plugins')
    Set-Content (Join-Path $install 'stoker_dlc.txt') "1829590=Seven Deadly Sins`r`n1829595=Deluxe Pack`r`n" -NoNewline
    return @{ Data = $data; Install = $install }
}

$root = Join-Path ([IO.Path]::GetTempPath()) "stoker-peacock-test-$([Guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory $root | Out-Null

# ── With the launcher ────────────────────────────────────────────────────────────────────
$folders = New-Folders (Join-Path $root 'with')
$token = [Guid]::NewGuid().ToString('N')
$portFile = Join-Path $root 'agent.port'
$requests = Join-Path $root 'agent.jsonl'
$agent = Start-Process python -ArgumentList @($fakeAgent, (Join-Path $root 'unused.sock'), $requests, '--tcp', $portFile, '--token', $token) -WindowStyle Hidden -PassThru
$game = Start-Process ping.exe -ArgumentList '-n', '25', '127.0.0.1' -WindowStyle Hidden -PassThru
$run = $null

try {
    for ($i = 0; $i -lt 50 -and !(Test-Path $portFile); $i++) { Start-Sleep -Milliseconds 100 }
    if (!(Test-Path $portFile)) { throw 'The fake agent did not start.' }
    $run = Start-Peacock $folders.Data $folders.Install $game.Id @{ Port = [int](Get-Content $portFile); Token = $token }

    $body = 'grant_type=external_steam&steam_userid=76561198000000001&steam_appid=1659040' +
        '&steam_clienttoken=00&steam_identity=hitman&access_token=x&locale=en-US&rgn=EU&gs=pc-prod'
    $signIn = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/oauth/token" -Body $body -ContentType 'application/x-www-form-urlencoded'
    if ($signIn.token_type -ne 'bearer') { $failures.Add('The sign-in returned no bearer token.') }

    # A finished contract, as Peacock posts it to its leaderboard address.
    $leaderboards = "http://127.0.0.1:$Port/_wf/stoker/leaderboards/contracts/$contract/h3/steam/normal"
    $score = @{ username = 'ignored'; platformId = 'ignored'; score = 123456
        data = @{ Score = @{ StarCount = 5; SilentAssassin = $true }; Headlines = @(@{ headline = 'UI_SCORING_SUMMARY_KILL_PENALTY' }); PlayStyle = 'UI_PLAYSTYLE_ICA_STEALTH_ASSASSIN' } } |
        ConvertTo-Json -Depth 6
    try { Invoke-RestMethod -Method Post -Uri $leaderboards -Body $score -ContentType 'application/json' | Out-Null }
    catch { $failures.Add("Posting the score failed: $_") }

    $entries = @(Invoke-RestMethod -Uri "$leaderboards/entries?page=1")
    if ($entries.Count -ne 1) { $failures.Add("The board has $($entries.Count) entries, expected 1.") }
    else {
        $entry = $entries[0]
        if ($entry.LeaderboardData.Score -ne 123456 -or $entry.LeaderboardData.Rank -ne 1) { $failures.Add('The entry lost its score or rank.') }
        if ($entry.LeaderboardData.Player.displayName -ne 'Test Player') { $failures.Add("The entry is named '$($entry.LeaderboardData.Player.displayName)', not the Stoker player.") }
        if ($entry.LeaderboardData.Data.Score.StarCount -ne 5 -or !$entry.LeaderboardData.Data.Score.SilentAssassin) { $failures.Add('The entry lost its score breakdown.') }
        if ($entry.detailedscore.Headlines[0].headline -ne 'UI_SCORING_SUMMARY_KILL_PENALTY') { $failures.Add('The entry lost its headlines.') }
    }

    # As the game asks: Peacock's own route, which fetches from leaderboardsHost.
    $menu = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/profiles/page/LeaderboardEntries?contractid=$contract&difficultyLevel=2&page=0" `
        -Headers @{ Authorization = "Bearer $($signIn.access_token)" }
    $first = @($menu.data.Entries)[0]
    if (!$first -or $first.LeaderboardData.Score -ne 123456 -or $first.LeaderboardData.Data.Score.StarCount -ne 5) {
        $failures.Add("Peacock's own leaderboard route did not get the Stoker board: $($menu | ConvertTo-Json -Depth 4 -Compress)")
    }

    $sent = Get-Content $requests | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object { $_.op -eq 'leaderboard.upload' }
    if (@($sent).Count -ne 1 -or $sent.body.data.Score.StarCount -ne 5) { $failures.Add('The launcher was not handed the score with its breakdown.') }
    if (!(Get-Content $requests | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object { $_.op -eq 'session.hello' -and $_.body.integration.name -eq 'peacock' })) {
        $failures.Add('Peacock did not say hello to the launcher as integration peacock.')
    }

    # The game exits: Peacock writes the profile and stops.
    $game.Kill()
    if (!$run.Process.WaitForExit(30000)) { $failures.Add('Peacock did not exit after the game had exited.') }
    elseif ($run.Process.ExitCode -ne 0) { $failures.Add("Peacock exited with $($run.Process.ExitCode).") }

    $userProfile = Get-ChildItem (Join-Path $folders.Data 'userdata\users') -Filter '*.json' | Where-Object Name -ne 'lop.json' | Select-Object -First 1
    if (!$userProfile) { $failures.Add('No profile was written.') }
    else {
        $entitlements = @((Get-Content $userProfile.FullName -Raw | ConvertFrom-Json).Extensions.entP)
        $expected = @('1659040', '1829590', '1829595')
        if (($entitlements -join ',') -ne ($expected -join ',')) { $failures.Add("Entitlements are '$($entitlements -join ',')', expected '$($expected -join ',')'.") }
    }

    $log = $run.Output.Result
    if ($log -notmatch 'Entitlements from Stoker: 2 DLC') { $failures.Add('The plugin did not answer the entitlement check.') }
    if ($log -notmatch "Leaderboards: every Stoker player's") { $failures.Add('The plugin did not take over the leaderboards.') }
    if ($log -match 'Error while evaluating plugin') { $failures.Add('The plugin failed to load.') }
    if ($log -match '(?i)Failed to get entitlements|authentication refresh') { $failures.Add('Peacock asked Steam or IOI after all.') }
}
finally {
    if ($run -and !$run.Process.HasExited) { $run.Process.Kill(); $run.Process.WaitForExit() }
    if (!$game.HasExited) { $game.Kill() }
    if (!$agent.HasExited) { $agent.Kill() }
}

# ── Without the launcher ─────────────────────────────────────────────────────────────────
$folders = New-Folders (Join-Path $root 'without')
Set-Content (Join-Path $folders.Data 'options.ini') "[peacock]`r`nleaderboards=true`r`nleaderboardsHost=http://127.0.0.1:1/_wf/stoker`r`n"
$game = Start-Process ping.exe -ArgumentList '-n', '10', '127.0.0.1' -WindowStyle Hidden -PassThru
$alone = $null
try {
    $alone = Start-Peacock $folders.Data $folders.Install $game.Id $null
    Start-Sleep -Milliseconds 500
    $alone.Process.Kill(); $alone.Process.WaitForExit()
    $log = $alone.Output.Result
    if ($log -notmatch "the leaderboards are Peacock's own") { $failures.Add('Without the launcher, the plugin did not leave the leaderboards to Peacock.') }
    if ($log -match 'Error while evaluating plugin') { $failures.Add('Without the launcher, the plugin failed to load.') }
    $options = Get-Content (Join-Path $folders.Data 'options.ini') -Raw
    if ($options -notmatch '(?m)^leaderboards=false\s*$' -or $options -notmatch '(?m)^leaderboardsHost=https://backend\.rdil\.rocks\s*$') {
        $failures.Add('Without the launcher, the leaderboard address an earlier run left was kept.')
    }
}
finally {
    if ($alone -and !$alone.Process.HasExited) { $alone.Process.Kill() }
    if (!$game.HasExited) { $game.Kill() }
    Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
}

if ($failures.Count) {
    $failures | ForEach-Object { Write-Host "FAIL $_" -ForegroundColor Red }
    if ($run) { Write-Host '--- Peacock output'; Write-Host $run.Output.Result }
    exit 1
}
Write-Host 'Peacock plugin: entitlements from stoker_dlc.txt, no Steam or IOI sign-in, leaderboards through the launcher with their breakdown (and Peacock''s own without it), profile written and server stopped with the game.' -ForegroundColor Green
