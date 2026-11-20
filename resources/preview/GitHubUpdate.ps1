param([int]$PreviewProcessId)

# 从 IDE 或其他 PowerShell 版本启动时，也使用系统自带的 Windows PowerShell 模块。
Import-Module ([IO.Path]::Combine($PSHOME, 'Modules\Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1'))
$repository = 'menghuijinxi/DomDownTool'

function Get-ReleaseVersion([string]$tag) {
    if ($tag -notmatch '^v?(\d+\.\d+\.\d+(?:\.\d+)?)$') {
        throw 'Release 标签需要使用 v1.2.3 或 v1.2.3.4 格式。'
    }
    $number = [version]$Matches[1]
    return [version]::new($number.Major, $number.Minor, $number.Build, [Math]::Max(0, $number.Revision))
}

function Get-Architecture([string]$path) {
    $reader = [IO.BinaryReader]::new([IO.File]::OpenRead($path))
    try {
        if ($reader.ReadUInt16() -ne 0x5a4d) { throw '文件不是 Windows 程序。' }
        $reader.BaseStream.Position = 60
        $reader.BaseStream.Position = $reader.ReadInt32()
        if ($reader.ReadUInt32() -ne 0x4550) { throw 'Windows 程序格式无效。' }
        switch ($reader.ReadUInt16()) {
            0x8664 { return 'x64' }
            0x014c { return 'x86' }
            0xaa64 { return 'arm64' }
            default { throw '无法识别程序架构。' }
        }
    } finally { $reader.Dispose() }
}

function Get-ReleaseAsset($release, [string]$architecture) {
    foreach ($name in ('DomDownToolPreview-{0}.exe' -f $architecture), 'DomDownToolPreview.exe') {
        $asset = $release.assets | Where-Object name -CEQ $name | Select-Object -First 1
        if ($null -eq $asset) { continue }
        $url = 'https://github.com/{0}/releases/download/{1}/{2}' -f $repository, $release.tag_name, $name
        if ($asset.browser_download_url -cne $url -or $asset.size -le 0) {
            throw 'GitHub 发布附件的下载地址或大小无效。'
        }
        if ($asset.digest -and $asset.digest -notmatch '^sha256:[a-f0-9]{64}$') {
            throw 'GitHub 发布附件的 SHA-256 摘要无效。'
        }
        return $asset
    }
    throw ('此 Release 缺少适用于 {0} 的 DomDownToolPreview.exe。' -f $architecture)
}

function Assert-UpdateFile($path, $asset, $version, $architecture) {
    if ([IO.FileInfo]::new($path).Length -ne $asset.size) { throw '更新文件大小校验失败。' }
    if ($asset.digest -and ('sha256:' + (Get-FileHash -LiteralPath $path).Hash) -ne $asset.digest) {
        throw '更新文件 SHA-256 校验失败。'
    }
    $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($path)
    if ($info.OriginalFilename -ne 'DomDownToolPreview.exe' -or
        [version]$info.FileVersion -ne $version -or (Get-Architecture $path) -ne $architecture) {
        throw '更新文件的程序名、版本号或架构与当前更新不符。'
    }
}

function Install-UpdateFile($path, $target, $asset, $version, $architecture) {
    Assert-UpdateFile $path $asset $version $architecture
    $mutex = [Threading.Mutex]::new($false, 'Local\DomDownToolPreview.ReleaseInstall')
    try {
        try { $null = $mutex.WaitOne() } catch [Threading.AbandonedMutexException] {}
        try {
            # 多个预览实例可能同时退出，在同一把锁内重新比较版本并替换。
            $installed = [Diagnostics.FileVersionInfo]::GetVersionInfo($target)
            if ([version]$installed.FileVersion -ge $version) { return }
            $backup = $path + '.bak'
            [IO.File]::Replace($path, $target, $backup)
            [IO.File]::Delete($backup)
        } finally { $mutex.ReleaseMutex() }
    } finally { $mutex.Dispose() }
}

function Wait-Transfer($task, $client, $form, $status, $path, $size) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while (-not $task.IsCompleted) {
        [Windows.Forms.Application]::DoEvents()
        if (-not $form.Visible -or $timer.Elapsed.TotalMinutes -gt 10) {
            $client.CancelAsync()
        }
        if ($form.Visible -and $size -gt 0 -and [IO.File]::Exists($path)) {
            $percent = [Math]::Min(100, [Math]::Floor(100 * [IO.FileInfo]::new($path).Length / $size))
            $status.Text = '正在下载更新… {0}%' -f $percent
        }
        [Threading.Thread]::Sleep(60)
    }
    return $task.GetAwaiter().GetResult()
}

function Show-UpdateWindow([int]$processId) {
    Add-Type -AssemblyName System.Windows.Forms
    [Windows.Forms.Application]::EnableVisualStyles()
    $preview = [Diagnostics.Process]::GetProcessById($processId)
    # 保留进程句柄，确保关闭后更新等待的是启动检查的那个实例。
    $null = $preview.Handle
    $target = $preview.MainModule.FileName
    $current = [version][Diagnostics.FileVersionInfo]::GetVersionInfo($target).FileVersion
    $architecture = Get-Architecture $target
    $form = [Windows.Forms.Form]::new()
    $form.Text = 'DomDownTool · 检查更新'
    $form.StartPosition = 'CenterScreen'
    $form.FormBorderStyle = 'FixedDialog'
    $form.MaximizeBox = $false
    $form.AutoSize = $true
    $form.AutoSizeMode = 'GrowAndShrink'
    $panel = [Windows.Forms.FlowLayoutPanel]::new()
    $panel.AutoSize = $true
    $panel.FlowDirection = 'TopDown'
    $panel.Padding = [Windows.Forms.Padding]::new(14)
    $form.Controls.Add($panel)
    $status = [Windows.Forms.Label]::new()
    $status.AutoSize = $true
    $status.Text = '当前版本 {0}，正在检查 GitHub…' -f $current
    $panel.Controls.Add($status)
    $buttons = [Windows.Forms.FlowLayoutPanel]::new()
    $buttons.AutoSize = $true
    $panel.Controls.Add($buttons)
    foreach ($caption in '更新并重启', '关闭后更新', '取消') {
        $button = [Windows.Forms.Button]::new()
        $button.Text = $caption
        $button.AutoSize = $true
        $button.Visible = ($caption -eq '取消')
        $button.Add_Click({ $form.Tag = $this.Text; if ($this.Text -eq '取消') { $form.Close() } })
        $buttons.Controls.Add($button)
    }
    $client = [Net.WebClient]::new()
    $client.Headers['User-Agent'] = 'DomDownToolPreview/' + $current
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $payload = $null
    try {
        $form.Show()
        $form.Activate()
        $url = 'https://api.github.com/repos/{0}/releases/latest' -f $repository
        try {
            $json = Wait-Transfer ($client.DownloadStringTaskAsync($url)) $client $form $status
        } catch {
            $cause = $_.Exception.GetBaseException()
            if ($cause.Response.StatusCode -eq 404) {
                [void][Windows.Forms.MessageBox]::Show($form, 'GitHub 上尚无正式发布版本。', $form.Text)
                return
            }
            throw
        }
        if (-not $form.Visible) { return }
        $release = ConvertFrom-Json -InputObject $json
        $version = Get-ReleaseVersion $release.tag_name
        if ($version -le $current) {
            [void][Windows.Forms.MessageBox]::Show($form, ('当前已是最新版本 {0}。' -f $current), $form.Text)
            return
        }
        $asset = Get-ReleaseAsset $release $architecture
        $status.Text = '当前版本 {0}，可更新至 {1}。' -f $current, $version
        foreach ($button in $buttons.Controls) { $button.Visible = $true }
        while ($form.Visible -and -not $form.Tag) {
            [Windows.Forms.Application]::DoEvents()
            [Threading.Thread]::Sleep(60)
        }
        if (-not $form.Visible) { return }
        $restart = $form.Tag -eq '更新并重启'
        $buttons.Controls[0].Visible = $false
        $buttons.Controls[1].Visible = $false
        $status.Text = '正在下载更新…'
        $payload = Join-Path ([IO.Path]::GetDirectoryName($target)) ([IO.Path]::GetRandomFileName())
        $task = $client.DownloadFileTaskAsync([uri]$asset.browser_download_url, $payload)
        $null = Wait-Transfer $task $client $form $status $payload $asset.size
        if (-not $form.Visible) { return }
        Assert-UpdateFile $payload $asset $version $architecture
        if ($restart -and -not $preview.HasExited -and -not $preview.CloseMainWindow()) {
            throw '无法关闭预览窗口，请重试或选择关闭后更新。'
        }
        $status.Text = '更新已下载，预览窗口关闭后将安装。'
        $form.WindowState = 'Minimized'
        while (-not $preview.WaitForExit(100)) {
            [Windows.Forms.Application]::DoEvents()
            if (-not $form.Visible) { return }
        }
        Install-UpdateFile $payload $target $asset $version $architecture
        if ($restart) { Start-Process -FilePath $target -WorkingDirectory ([IO.Path]::GetDirectoryName($target)) }
    } catch {
        if ($form.Visible) {
            [void][Windows.Forms.MessageBox]::Show($form, $_.Exception.GetBaseException().Message,
                '更新失败', 'OK', 'Error')
        }
    } finally {
        if ($payload -and [IO.File]::Exists($payload)) { [IO.File]::Delete($payload) }
        $client.Dispose()
        $form.Dispose()
        $preview.Dispose()
    }
}

if ($PreviewProcessId) {
    $ErrorActionPreference = 'Stop'
    try { Show-UpdateWindow $PreviewProcessId }
    catch {
        Add-Type -AssemblyName System.Windows.Forms
        [void][Windows.Forms.MessageBox]::Show($_.Exception.GetBaseException().Message, '检查更新失败')
    }
}
