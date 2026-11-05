<#
.SYNOPSIS
Builds peacock.dll (x64) into build\x64.

.DESCRIPTION
Fetches the Stoker plugin SDK release stoker-plugin.yaml pins (scripts\fetch-sdk.ps1: downloaded once, checked by
SHA-256, cached), then builds the hook plugin with the SDK's helper: the Stoker plugin for Peacock and the launcher
protocol it loads go into the DLL as resources (hook-plugin\peacock.rc). Needs Visual Studio with the C++ workload,
found through vswhere. STOKER_PLUGIN_SDK names a local SDK folder or zip instead of the release.
#>
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$sdk = & (Join-Path $root 'scripts\fetch-sdk.ps1') -Project (Join-Path $root 'stoker-plugin.yaml')
$dll = & (Join-Path $sdk 'build\Build-HookPlugin.ps1') `
    -Source (Join-Path $root 'hook-plugin\peacock.cpp') `
    -Resources (Join-Path $root 'hook-plugin\peacock.rc') `
    -Name peacock -Platform x64 -Out (Join-Path $root 'build\x64')
Write-Host "Built $dll"
