# Seto-static launcher: quality settings and paths, then start the manual build.
# Run with: powershell -ExecutionPolicy Bypass -File launcher.ps1
# Headless check: powershell -ExecutionPolicy Bypass -File launcher.ps1 -DryRun
param([switch]$DryRun)

$ErrorActionPreference = 'Stop'
$repoRoot = $PSScriptRoot
$iniPath = Join-Path $repoRoot 'launcher.ini'

$defaults = [ordered]@{
    scale        = '2'
    render_scale = 'auto'
    msaa         = '0'
    filter       = '0'
    post_filter  = '0'
    present      = '0'
    iso          = ''
    eboot        = 'build/s7-eboot-reloc.elf'
    flash0       = ''
    hd_tex_dir   = ''
    server       = ''
    nickname     = ''
}

function Load-Config {
    $cfg = [ordered]@{}
    foreach ($k in $defaults.Keys) { $cfg[$k] = $defaults[$k] }
    if (Test-Path $iniPath) {
        foreach ($line in Get-Content $iniPath) {
            if ($line -match '^\s*#' -or $line -match '^\s*$') { continue }
            $kv = $line -split '=', 2
            if ($kv.Count -eq 2) { $cfg[$kv[0].Trim()] = $kv[1].Trim() }
        }
    }
    return $cfg
}

function Save-Config($cfg) {
    $lines = foreach ($k in $cfg.Keys) { "$k=$($cfg[$k])" }
    Set-Content -Path $iniPath -Value $lines -Encoding ASCII
}

function Resolve-RepoPath($p) {
    if ([string]::IsNullOrWhiteSpace($p)) { return '' }
    if ([IO.Path]::IsPathRooted($p)) { return $p }
    return Join-Path $repoRoot $p
}

function Quote($p) { return '"' + $p + '"' }

function Start-Game($cfg, [switch]$DryRun) {
    $iso = Resolve-RepoPath $cfg.iso
    $eboot = Resolve-RepoPath $cfg.eboot
    $flash0 = Resolve-RepoPath $cfg.flash0

    $missing = @()
    if ([string]::IsNullOrWhiteSpace($iso) -or !(Test-Path $iso)) { $missing += 'ISO' }
    if ([string]::IsNullOrWhiteSpace($eboot) -or !(Test-Path $eboot)) { $missing += 'EBOOT' }
    if ([string]::IsNullOrWhiteSpace($flash0) -or !(Test-Path $flash0)) { $missing += 'flash0' }

    $exe = Join-Path $repoRoot 'build/s6-build/Seto-static.exe'
    if (!(Test-Path $exe) -or (Get-Item $exe).Length -le 1000000) {
        $missing += 'build/s6-build/Seto-static.exe'
    }

    $prx = @(
        'build/s5-font-reloc.elf', 'build/s6-title-reloc.elf',
        'build/s93-story-reloc.elf', 'build/s93-field-reloc.elf',
        'build/s93-deck-reloc.elf', 'build/s93-decktut-reloc.elf',
        'build/s9duel-mgr-reloc.elf', 'build/s9duel-eng-reloc.elf',
        'build/s9duel-draw-reloc.elf', 'build/s9duel-swap-reloc.elf',
        'build/s9duel-tut-reloc.elf', 'build/s9duel-rec-reloc.elf',
        'build/s9charalist-reloc.elf', 'build/s9shop-reloc.elf'
    ) | ForEach-Object { Resolve-RepoPath $_ }
    foreach ($p in $prx) { if (!(Test-Path $p)) { $missing += $p } }

    if ($missing.Count) { throw ("Missing files:`n" + ($missing -join "`n")) }

    $gameArgs = (@($eboot, $iso) + $prx | ForEach-Object { Quote $_ }) -join ' '

    foreach ($e in Get-ChildItem Env: | Where-Object { $_.Name -like 'SETO_*' }) {
        Remove-Item ("Env:" + $e.Name) -ErrorAction SilentlyContinue
    }
    $env:PSPRECOMP_FLASH0 = $flash0
    $env:SETO_MANUAL = '1'
    $env:SETO_HOLD_WINDOW = '1'
    $env:SETO_SCALE = [string]$cfg.scale
    $env:SETO_FILTER = [string]$cfg.filter
    if ($cfg.present -eq 'gl' -or $cfg.present -eq 'ge' -or $cfg.present -eq 'ged') {
        $env:SETO_PRESENT = 'gl'
        $env:SETO_POST_FILTER = [string]$cfg.post_filter
    }
    if ($cfg.present -eq 'ge') { $env:SETO_GE_GPU = 'test' }
    if ($cfg.present -eq 'ged') {
        $env:SETO_GE_GPU = 'draw'
        $env:SETO_RENDERER = 'gl'
        $env:SETO_RENDER_SCALE = [string]$cfg.render_scale
        $env:SETO_MSAA = [string]$cfg.msaa
        if (![string]::IsNullOrWhiteSpace($cfg.hd_tex_dir)) {
            $env:SETO_HD_TEX_DIR = Resolve-RepoPath $cfg.hd_tex_dir
        }
    }
    if (![string]::IsNullOrWhiteSpace($cfg.server)) {
        $env:SETO_SERVER = [string]$cfg.server
    }
    if (![string]::IsNullOrWhiteSpace($cfg.nickname)) {
        $env:SETO_NICKNAME = [string]$cfg.nickname
    }

    if ($DryRun) {
        Write-Output "exe: $exe"
        Write-Output "args: $gameArgs"
        Write-Output "env: SETO_MANUAL=1 SETO_HOLD_WINDOW=1 SETO_SCALE=$($cfg.scale) SETO_FILTER=$($cfg.filter) SETO_RENDER_SCALE=$($cfg.render_scale) SETO_MSAA=$($cfg.msaa) SETO_POST_FILTER=$($cfg.post_filter) SETO_HD_TEX_DIR=$($cfg.hd_tex_dir)"
        Write-Output "present: $($cfg.present)"
        Write-Output "net: server=$($cfg.server) nickname=$($cfg.nickname)"
        Write-Output "flash0: $flash0"
        return
    }
    Start-Process -FilePath $exe -ArgumentList $gameArgs -WorkingDirectory $repoRoot
}

$cfg = Load-Config
if ($DryRun) { Start-Game $cfg -DryRun; return }

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$scaleLabels = [ordered]@{
    '960 x 544 (2x)'    = '2'
    '1440 x 816 (3x)'   = '3'
    '1920 x 1088 (4x)'  = '4'
    '2560 x 1448 (5x)'  = '5'
    '2880 x 1632 (6x)'  = '6'
    '3840 x 2176 (8x)'  = '8'
}
$filterLabels = [ordered]@{
    'Nearest (crisp)'       = '0'
    'Bilinear (smooth)'     = '1'
    'Bicubic (high quality)' = '2'
}
$renderScaleLabels = [ordered]@{
    'Auto (match window)' = 'auto'
    '1x (480 x 272)'       = '1'
    '2x (960 x 544)'       = '2'
    '3x (1440 x 816)'      = '3'
    '4x (1920 x 1088)'     = '4'
    '6x (2880 x 1632)'     = '6'
    '8x (3840 x 2176)'     = '8'
}
$msaaLabels = [ordered]@{
    'Off' = '0'
    '2x'  = '2'
    '4x'  = '4'
    '8x'  = '8'
}
$postFilterLabels = [ordered]@{
    'Off'           = '0'
    'CAS Sharpen'   = '1'
    'PSP LCD'       = '2'
    'CRT Scanlines' = '3'
}
$presentLabels = [ordered]@{
    'GDI (default)'                    = '0'
    'OpenGL 3.3 Presenter (M0)'        = 'gl'
    'OpenGL GE test (M1.0)'            = 'ge'
    'OpenGL 3.3 GE GPU Hardware (M5)'  = 'ged'
}

$form = New-Object System.Windows.Forms.Form
$form.Text = 'Seto-static launcher'
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.MinimizeBox = $false
$form.ClientSize = New-Object System.Drawing.Size(560, 539)

function Add-Label($text, $y) {
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $text
    $l.SetBounds(12, $y, 110, 20)
    $form.Controls.Add($l)
}

Add-Label 'Resolution' 15
Add-Label 'Filter' 48
Add-Label 'ISO' 81
Add-Label 'EBOOT' 114
Add-Label 'flash0' 147
Add-Label 'Presenter' 180
Add-Label 'Internal Scale' 213
Add-Label 'MSAA (3D)' 246
Add-Label 'Post Filter' 279
Add-Label 'HD Textures' 312
Add-Label 'Server (host:port)' 345
Add-Label 'Nickname' 378

$scaleBox = New-Object System.Windows.Forms.ComboBox
$scaleBox.DropDownStyle = 'DropDownList'
$scaleBox.SetBounds(130, 12, 410, 24)
foreach ($k in $scaleLabels.Keys) { [void]$scaleBox.Items.Add($k) }
$form.Controls.Add($scaleBox)

$filterBox = New-Object System.Windows.Forms.ComboBox
$filterBox.DropDownStyle = 'DropDownList'
$filterBox.SetBounds(130, 45, 410, 24)
foreach ($k in $filterLabels.Keys) { [void]$filterBox.Items.Add($k) }
$form.Controls.Add($filterBox)

$isoBox = New-Object System.Windows.Forms.TextBox
$isoBox.SetBounds(130, 78, 410, 24)
$isoBox.Text = [string]$cfg.iso
$form.Controls.Add($isoBox)

$ebootBox = New-Object System.Windows.Forms.TextBox
$ebootBox.SetBounds(130, 111, 410, 24)
$ebootBox.Text = [string]$cfg.eboot
$form.Controls.Add($ebootBox)

$flashBox = New-Object System.Windows.Forms.TextBox
$flashBox.SetBounds(130, 144, 410, 24)
$flashBox.Text = [string]$cfg.flash0
$form.Controls.Add($flashBox)

$presentBox = New-Object System.Windows.Forms.ComboBox
$presentBox.DropDownStyle = 'DropDownList'
$presentBox.SetBounds(130, 177, 410, 24)
foreach ($k in $presentLabels.Keys) { [void]$presentBox.Items.Add($k) }
$form.Controls.Add($presentBox)

$renderScaleBox = New-Object System.Windows.Forms.ComboBox
$renderScaleBox.DropDownStyle = 'DropDownList'
$renderScaleBox.SetBounds(130, 210, 410, 24)
foreach ($k in $renderScaleLabels.Keys) { [void]$renderScaleBox.Items.Add($k) }
$form.Controls.Add($renderScaleBox)

$msaaBox = New-Object System.Windows.Forms.ComboBox
$msaaBox.DropDownStyle = 'DropDownList'
$msaaBox.SetBounds(130, 243, 410, 24)
foreach ($k in $msaaLabels.Keys) { [void]$msaaBox.Items.Add($k) }
$form.Controls.Add($msaaBox)

$postFilterBox = New-Object System.Windows.Forms.ComboBox
$postFilterBox.DropDownStyle = 'DropDownList'
$postFilterBox.SetBounds(130, 276, 410, 24)
foreach ($k in $postFilterLabels.Keys) { [void]$postFilterBox.Items.Add($k) }
$form.Controls.Add($postFilterBox)

$hdTexBox = New-Object System.Windows.Forms.TextBox
$hdTexBox.SetBounds(130, 309, 410, 24)
$hdTexBox.Text = [string]$cfg.hd_tex_dir
$form.Controls.Add($hdTexBox)

$serverBox = New-Object System.Windows.Forms.TextBox
$serverBox.SetBounds(130, 342, 410, 24)
$serverBox.Text = [string]$cfg.server
$form.Controls.Add($serverBox)

$nicknameBox = New-Object System.Windows.Forms.TextBox
$nicknameBox.SetBounds(130, 375, 410, 24)
$nicknameBox.Text = [string]$cfg.nickname
$form.Controls.Add($nicknameBox)

$scaleLabel = ($scaleLabels.Keys | Where-Object { $scaleLabels[$_] -eq [string]$cfg.scale } | Select-Object -First 1)
$filterLabel = ($filterLabels.Keys | Where-Object { $filterLabels[$_] -eq [string]$cfg.filter } | Select-Object -First 1)
$presentLabel = ($presentLabels.Keys | Where-Object { $presentLabels[$_] -eq [string]$cfg.present } | Select-Object -First 1)
$renderScaleLabel = ($renderScaleLabels.Keys | Where-Object { $renderScaleLabels[$_] -eq [string]$cfg.render_scale } | Select-Object -First 1)
$msaaLabel = ($msaaLabels.Keys | Where-Object { $msaaLabels[$_] -eq [string]$cfg.msaa } | Select-Object -First 1)
$postFilterLabel = ($postFilterLabels.Keys | Where-Object { $postFilterLabels[$_] -eq [string]$cfg.post_filter } | Select-Object -First 1)
$scaleBox.SelectedItem = if ($scaleLabel) { $scaleLabel } else { '960 x 544 (2x)' }
$filterBox.SelectedItem = if ($filterLabel) { $filterLabel } else { 'Nearest (crisp)' }
$presentBox.SelectedItem = if ($presentLabel) { $presentLabel } else { 'GDI (default)' }
$renderScaleBox.SelectedItem = if ($renderScaleLabel) { $renderScaleLabel } else { 'Auto (match window)' }
$msaaBox.SelectedItem = if ($msaaLabel) { $msaaLabel } else { 'Off' }
$postFilterBox.SelectedItem = if ($postFilterLabel) { $postFilterLabel } else { 'Off' }

$status = New-Object System.Windows.Forms.Label
$status.SetBounds(12, 411, 530, 40)
$status.ForeColor = [System.Drawing.Color]::DarkRed
$form.Controls.Add($status)

$startBtn = New-Object System.Windows.Forms.Button
$startBtn.Text = 'Play'
$startBtn.SetBounds(130, 459, 120, 30)
$form.Controls.Add($startBtn)
$form.AcceptButton = $startBtn

$saveBtn = New-Object System.Windows.Forms.Button
$saveBtn.Text = 'Save'
$saveBtn.SetBounds(260, 459, 120, 30)
$form.Controls.Add($saveBtn)

$cancelBtn = New-Object System.Windows.Forms.Button
$cancelBtn.Text = 'Cancel'
$cancelBtn.SetBounds(390, 459, 120, 30)
$form.Controls.Add($cancelBtn)

function Update-Config {
    $cfg.scale = $scaleLabels[[string]$scaleBox.SelectedItem]
    $cfg.filter = $filterLabels[[string]$filterBox.SelectedItem]
    $cfg.present = $presentLabels[[string]$presentBox.SelectedItem]
    $cfg.render_scale = $renderScaleLabels[[string]$renderScaleBox.SelectedItem]
    $cfg.msaa = $msaaLabels[[string]$msaaBox.SelectedItem]
    $cfg.post_filter = $postFilterLabels[[string]$postFilterBox.SelectedItem]
    $cfg.iso = $isoBox.Text.Trim()
    $cfg.eboot = $ebootBox.Text.Trim()
    $cfg.flash0 = $flashBox.Text.Trim()
    $cfg.hd_tex_dir = $hdTexBox.Text.Trim()
    $cfg.server = $serverBox.Text.Trim()
    $cfg.nickname = $nicknameBox.Text.Trim()
}

$startBtn.Add_Click({
    try {
        Update-Config
        Save-Config $cfg
        Start-Game $cfg
        $form.Close()
    } catch {
        $status.Text = $_.Exception.Message
    }
})

$saveBtn.Add_Click({
    try {
        Update-Config
        Save-Config $cfg
        $status.ForeColor = [System.Drawing.Color]::DarkGreen
        $status.Text = "Saved to $iniPath"
    } catch {
        $status.Text = $_.Exception.Message
    }
})

$cancelBtn.Add_Click({ $form.Close() })

[void]$form.ShowDialog()
