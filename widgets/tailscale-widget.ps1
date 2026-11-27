param(
    [switch]$Once
)

$ErrorActionPreference = 'SilentlyContinue'

$widgetsDir = Join-Path $env:LOCALAPPDATA 'FluidWin\widgets'
$outFile    = Join-Path $widgetsDir 'tailscale.json'
$tmpFile    = Join-Path $widgetsDir 'tailscale.json.tmp'
$cfgFile    = Join-Path $widgetsDir 'tailscale.config.json'
$actFile    = Join-Path $widgetsDir 'tailscale.action.json'

if (-not (Test-Path $widgetsDir)) {
    New-Item -ItemType Directory -Path $widgetsDir -Force | Out-Null
}

function Find-Tailscale {
    $cmd = Get-Command 'tailscale.exe' -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = @(
        'C:\Program Files\Tailscale\tailscale.exe',
        'C:\Program Files (x86)\Tailscale\tailscale.exe',
        (Join-Path $env:LOCALAPPDATA 'Tailscale\tailscale.exe')
    )
    foreach ($p in $candidates) {
        if (Test-Path $p) { return $p }
    }
    return $null
}

function Parse-IntervalSeconds($val) {
    if ($null -eq $val -or $val -is [bool]) { return 10 }
    $num = 0.0
    if ($val -is [string]) {
        $s = $val.Trim()
        if ($s.EndsWith('s', [System.StringComparison]::OrdinalIgnoreCase)) {
            $s = $s.Substring(0, $s.Length - 1).Trim()
        }
        if (-not [double]::TryParse($s, [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$num)) {
            return 10
        }
    } else {
        try { $num = [double]$val } catch { return 10 }
    }
    if ([double]::IsNaN($num) -or [double]::IsInfinity($num)) { return 10 }
    $sec = [int][Math]::Round($num)
    return [Math]::Max(5, [Math]::Min(60, $sec))
}

function Read-WidgetConfig {
    $cfg = @{
        onlineOnly  = $true
        showSelf    = $true
        animateIcon = $true
        animStyle   = 'wave'
        glowColor   = 'auto'
        refreshSec  = 10
    }
    if (Test-Path $cfgFile) {
        try {
            $raw = Get-Content -Path $cfgFile -Raw -Encoding UTF8 | ConvertFrom-Json
            if ($null -ne $raw.onlineOnly)  { $cfg.onlineOnly  = [bool]$raw.onlineOnly }
            if ($null -ne $raw.showSelf)    { $cfg.showSelf    = [bool]$raw.showSelf }
            if ($null -ne $raw.animateIcon) { $cfg.animateIcon = [bool]$raw.animateIcon }
            if ($raw.animStyle -in @('wave', 'pulse')) { $cfg.animStyle = [string]$raw.animStyle }
            if ($raw.glowColor -is [string]) {
                $gc = $raw.glowColor.Trim()
                if ($gc -ieq 'auto') { $cfg.glowColor = 'auto' }
                elseif ($gc -match '^#[0-9a-fA-F]{6}$') { $cfg.glowColor = $gc.ToUpperInvariant() }
            }
            if ($null -ne $raw.refreshSec)  { $cfg.refreshSec  = Parse-IntervalSeconds $raw.refreshSec }
        } catch {}
    }
    return $cfg
}

function Handle-Action([string]$tsBin) {
    if (-not (Test-Path $actFile)) { return $false }
    $raw = $null
    try {
        $raw = Get-Content -Path $actFile -Raw -Encoding UTF8 | ConvertFrom-Json
    } catch {}
    Remove-Item -Path $actFile -Force -ErrorAction SilentlyContinue
    if (-not $raw -or -not $tsBin) { return $false }
    if ($raw.action -eq 'toggle' -and $raw.key -eq 'connected') {
        if ([bool]$raw.value) {
            & $tsBin up 2>$null | Out-Null
        } else {
            & $tsBin down 2>$null | Out-Null
        }
        return $true
    }
    return $false
}

function Build-TailscalePayload([string]$tsBin, [hashtable]$cfg) {
    $configSchema = @(
        @{
            key         = 'onlineOnly'
            label       = 'Online devices only'
            description = 'Hide offline tailnet peers from the panel'
            type        = 'toggle'
            default     = $true
        },
        @{
            key         = 'showSelf'
            label       = 'Show this PC first'
            description = 'Pin local machine IP at the top of the list'
            type        = 'toggle'
            default     = $true
        },
        @{
            key         = 'animateIcon'
            label       = 'Animate icon'
            description = 'Subtle glow animation on the Tailscale logo while connected'
            type        = 'toggle'
            default     = $true
        },
        @{
            key         = 'animStyle'
            label       = 'Animation style'
            description = 'Choose travelling dot wave or soft pulse'
            type        = 'choice'
            default     = 'wave'
            options     = @('wave', 'pulse')
        },
        @{
            key         = 'glowColor'
            label       = 'Glow color'
            description = 'Accent color for the animated logo glow'
            type        = 'color'
            default     = 'auto'
            options     = @('auto', '#30D158', '#32D2FF', '#0A84FF', '#BF5AF2', '#FF9F0A', '#FF453A')
        },
        @{
            key         = 'refreshSec'
            label       = 'Refresh interval'
            description = 'How often to poll tailscale status'
            type        = 'slider'
            min         = 5
            max         = 60
            step        = 5
            default     = 10
            unit        = 's'
        }
    )

    if (-not $tsBin) {
        return @{
            id          = 'tailscale'
            name        = 'Tailscale'
            subtitle    = 'tailscale.exe not found'
            tooltip     = 'Tailscale · Not installed'
            status      = 'Missing'
            statusColor = '#FF453A'
            icon        = @{ glyph = 'tailscale'; color = '#FF453A'; animate = 'none' }
            items       = @(@{
                title    = 'Install Tailscale'
                subtitle = 'Open tailscale.com/download'
                badge    = 'Setup'
                color    = '#FF9F0A'
                url      = 'https://tailscale.com/download'
            })
            config      = $configSchema
            footer      = @{ label = 'Open Tailscale Admin'; url = 'https://login.tailscale.com/admin/machines' }
        }
    }

    $rawJson = & $tsBin status --json 2>$null | Out-String
    if (-not $rawJson -or -not $rawJson.Trim().StartsWith('{')) {
        return @{
            id          = 'tailscale'
            name        = 'Tailscale'
            subtitle    = 'Daemon not responding'
            tooltip     = 'Tailscale · Offline'
            status      = 'Offline'
            statusColor = '#8E8E93'
            icon        = @{ glyph = 'tailscale'; color = '#8E8E93'; animate = 'none' }
            toggle      = @{ key = 'connected'; on = $false }
            items       = @()
            config      = $configSchema
            footer      = @{ label = 'Open Tailscale Admin'; url = 'https://login.tailscale.com/admin/machines' }
        }
    }

    $st = $rawJson | ConvertFrom-Json
    $state = [string]$st.BackendState
    $running = ($state -eq 'Running')

    $selfHost = if ($st.Self -and $st.Self.HostName) { [string]$st.Self.HostName } else { $env:COMPUTERNAME }
    $selfIP   = if ($st.Self -and $st.Self.TailscaleIPs -and $st.Self.TailscaleIPs.Count -gt 0) { [string]$st.Self.TailscaleIPs[0] } else { '' }

    $peers = @()
    if ($st.Peer) {
        foreach ($prop in $st.Peer.PSObject.Properties) {
            $p = $prop.Value
            if (-not $p) { continue }
            $ip = if ($p.TailscaleIPs -and $p.TailscaleIPs.Count -gt 0) { [string]$p.TailscaleIPs[0] } else { '' }
            $peers += [pscustomobject]@{
                HostName = if ($p.HostName) { [string]$p.HostName } else { $ip }
                Online   = [bool]$p.Online
                Active   = [bool]$p.Active
                ExitNode = [bool]$p.ExitNode
                OS       = if ($p.OS) { [string]$p.OS } else { '' }
                IP       = $ip
            }
        }
    }

    $totalPeers  = $peers.Count
    $onlinePeers = @($peers | Where-Object { $_.Online }).Count
    $exitPeer    = $peers | Where-Object { $_.ExitNode } | Select-Object -First 1

    if ($running -and $exitPeer) {
        $statusText  = 'Exit Node'
        $statusColor = '#0A84FF'
        $iconColor   = '#0A84FF'
    } elseif ($running) {
        $statusText  = 'Connected'
        $statusColor = '#30D158'
        $iconColor   = '#30D158'
    } elseif ($state -eq 'Stopped') {
        $statusText  = 'Stopped'
        $statusColor = '#8E8E93'
        $iconColor   = '#8E8E93'
    } else {
        $statusText  = if ($state) { $state } else { 'Unknown' }
        $statusColor = '#FF9F0A'
        $iconColor   = '#FF9F0A'
    }

    $animMode = if ($running -and $cfg.animateIcon) { $cfg.animStyle } else { 'none' }

    $subtitleParts = @($selfHost)
    if ($selfIP) { $subtitleParts += $selfIP }
    if ($exitPeer) { $subtitleParts += "via $($exitPeer.HostName)" }
    $subtitle = $subtitleParts -join ' · '

    $tooltip = if ($running) {
        "Tailscale · $statusText ($onlinePeers/$totalPeers online)"
    } else {
        "Tailscale · $statusText"
    }

    $items = New-Object System.Collections.ArrayList

    if ($cfg.showSelf -and $selfIP) {
        [void]$items.Add(@{
            title    = "$selfHost (This PC)"
            subtitle = "$selfIP · Click to copy IP"
            badge    = $statusText
            color    = $statusColor
            copy     = $selfIP
        })
    }

    $sortedPeers = $peers | Sort-Object @{Expression={$_.ExitNode}; Descending=$true}, @{Expression={$_.Active}; Descending=$true}, @{Expression={$_.Online}; Descending=$true}, @{Expression={$_.HostName}; Ascending=$true}

    foreach ($p in $sortedPeers) {
        if ($items.Count -ge 30) { break }
        if ($cfg.onlineOnly -and -not $p.Online) { continue }

        $badge = if ($p.ExitNode) { 'Exit' } elseif ($p.Active) { 'Active' } elseif ($p.Online) { 'Online' } else { 'Offline' }
        $color = if ($p.ExitNode -or $p.Active) { '#0A84FF' } elseif ($p.Online) { '#30D158' } else { '#636366' }
        $subParts = @()
        if ($p.IP) { $subParts += $p.IP }
        if ($p.OS) { $subParts += $p.OS }
        $subParts += 'Click to copy IP'

        [void]$items.Add(@{
            title    = $p.HostName
            subtitle = ($subParts -join ' · ')
            badge    = $badge
            color    = $color
            copy     = $p.IP
        })
    }

    $iconObj = @{
        glyph   = 'tailscale'
        color   = $iconColor
        animate = $animMode
    }
    if ($cfg.glowColor -and $cfg.glowColor -ne 'auto') {
        $iconObj.glowColor = $cfg.glowColor
    }

    return @{
        id          = 'tailscale'
        name        = 'Tailscale'
        subtitle    = $subtitle
        tooltip     = $tooltip
        status      = $statusText
        statusColor = $statusColor
        icon        = $iconObj
        toggle      = @{
            key = 'connected'
            on  = $running
        }
        items       = @($items)
        config      = $configSchema
        footer      = @{
            label = 'Open Tailscale Admin'
            url   = 'https://login.tailscale.com/admin/machines'
        }
    }
}

$tsBin = Find-Tailscale
$lastWritten = ''
$lastCfgMtime = [datetime]::MinValue
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

do {
    [void](Handle-Action -tsBin $tsBin)
    $cfg = Read-WidgetConfig
    if (Test-Path $cfgFile) { $lastCfgMtime = (Get-Item $cfgFile).LastWriteTimeUtc }
    $payload = Build-TailscalePayload -tsBin $tsBin -cfg $cfg
    $json = $payload | ConvertTo-Json -Depth 6
    if ($json -ne $lastWritten) {
        [System.IO.File]::WriteAllText($tmpFile, $json, $utf8NoBom)
        Move-Item -Path $tmpFile -Destination $outFile -Force
        $lastWritten = $json
    }
    if ($Once) { break }

    $waitTicks = (Parse-IntervalSeconds $cfg.refreshSec) * 4
    for ($i = 0; $i -lt $waitTicks; $i++) {
        Start-Sleep -Milliseconds 250
        if (Test-Path $actFile) { break }
        if (Test-Path $cfgFile) {
            $mt = (Get-Item $cfgFile).LastWriteTimeUtc
            if ($mt -ne $lastCfgMtime) { break }
        }
    }
} while ($true)
