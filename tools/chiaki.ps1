<#
.SYNOPSIS
  Launch the PC-side chiaki-ng (PS Remote Play client) for the dev PS5.

.DESCRIPTION
  Nothing here talks to the console by itself. `where` only reports paths;
  `open` starts the chiaki-ng window (which runs its own LAN discovery once it
  is open); `stream` connects to a console that is ALREADY registered.
  Registering (the PIN / PSN account step) is done by hand in the chiaki-ng
  window - see docs/build/remote-play.md.

  Usage (from the repo root, PowerShell):
    ./tools/chiaki.ps1 where
    ./tools/chiaki.ps1 open
    ./tools/chiaki.ps1 stream [-Nickname ps5] [-Fullscreen]

  The executable is looked up in $env:CHIAKI_NG_EXE, else
  %USERPROFILE%\tools\chiaki-ng\app\chiaki-ng-Win\chiaki.exe. The console
  address is PS5_HOST from .env.local, else .env.
#>
param(
    [Parameter(Position = 0)][ValidateSet("where", "open", "stream")][string]$Command = "where",
    [string]$Nickname = "ps5",
    [switch]$Fullscreen
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot

$exe = $env:CHIAKI_NG_EXE
if (-not $exe) { $exe = Join-Path $env:USERPROFILE "tools\chiaki-ng\app\chiaki-ng-Win\chiaki.exe" }
if (-not (Test-Path $exe)) {
    throw "chiaki-ng not found at $exe - see docs/build/remote-play.md (or set CHIAKI_NG_EXE)."
}

function Get-Ps5Host {
    foreach ($f in @(".env.local", ".env")) {
        $p = Join-Path $repo $f
        if (-not (Test-Path $p)) { continue }
        $m = Select-String -Path $p -Pattern '^\s*PS5_HOST\s*=\s*(\S+)' | Select-Object -First 1
        if ($m) { return $m.Matches[0].Groups[1].Value.Trim('"', "'") }
    }
    return $null
}

switch ($Command) {
    "where" {
        $ver = (Get-Item $exe).VersionInfo.ProductVersion
        "chiaki-ng : $exe (v$ver)"
        "PS5_HOST  : $(if (Get-Ps5Host) { Get-Ps5Host } else { '(not set in .env.local / .env)' })"
    }
    "open" {
        Start-Process -FilePath $exe
    }
    "stream" {
        $h = Get-Ps5Host
        if (-not $h) { throw "PS5_HOST is not set in .env.local or .env." }
        $a = @("stream", $Nickname, $h)
        if ($Fullscreen) { $a = @("--fullscreen") + $a }
        Start-Process -FilePath $exe -ArgumentList $a
    }
}
