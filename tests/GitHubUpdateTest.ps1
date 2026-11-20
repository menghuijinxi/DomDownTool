param([Parameter(Mandatory)][string]$PreviewExe)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
. ([scriptblock]::Create([IO.File]::ReadAllText((Join-Path $root 'resources/preview/GitHubUpdate.ps1'))))

function Assert-True($value, $message) {
    if (-not $value) { throw $message }
}
function Assert-Rejected([scriptblock]$action) {
    try { & $action } catch { return }
    throw ('Expected rejection: ' + $action)
}

$work = Join-Path (Join-Path $root 'tmp') ([Guid]::NewGuid().ToString('N'))
$null = [IO.Directory]::CreateDirectory($work)
try {
    Assert-True ((Get-ReleaseVersion 'v1.10.0') -gt (Get-ReleaseVersion 'v1.9.9.9')) 'Version ordering'
    Assert-Rejected { Get-ReleaseVersion 'v1.2.0-beta' }
    $architecture = Get-Architecture $PreviewExe
    $version = [version][Diagnostics.FileVersionInfo]::GetVersionInfo($PreviewExe).FileVersion
    $tag = 'v' + $version
    $base = 'https://github.com/{0}/releases/download/{1}/' -f $repository, $tag
    $asset = [pscustomobject]@{
        name = 'DomDownToolPreview.exe'
        browser_download_url = $base + 'DomDownToolPreview.exe'
        size = [IO.FileInfo]::new($PreviewExe).Length
        digest = 'sha256:' + (Get-FileHash -LiteralPath $PreviewExe).Hash
    }
    $release = @{tag_name = $tag; assets = @($asset)}
    Assert-True ((Get-ReleaseAsset $release $architecture) -eq $asset) 'Single executable fallback'
    $specific = $asset.PSObject.Copy()
    $specific.name = 'DomDownToolPreview-{0}.exe' -f $architecture
    $specific.browser_download_url = $base + $specific.name
    $release.assets += $specific
    Assert-True ((Get-ReleaseAsset $release $architecture) -eq $specific) 'Architecture selection'
    $specific.browser_download_url = 'https://example.com/other.exe'
    Assert-Rejected { Get-ReleaseAsset $release $architecture }
    Assert-Rejected { Get-ReleaseAsset @{assets = @()} $architecture }

    $target = Join-Path $work 'old app.exe'
    $payload = Join-Path $work 'payload.tmp'
    Add-Type -TypeDefinition @'
using System.Reflection;
[assembly: AssemblyFileVersion("0.0.0.1")]
class OldPreview { static void Main() {} }
'@ -OutputAssembly $target -OutputType ConsoleApplication
    [IO.File]::Copy($PreviewExe, $payload)
    $oldHash = (Get-FileHash -LiteralPath $target).Hash
    $bad = $asset.PSObject.Copy()
    $bad.digest = 'sha256:' + ('0' * 64)
    Assert-Rejected { Install-UpdateFile $payload $target $bad $version $architecture }
    Assert-True ((Get-FileHash -LiteralPath $target).Hash -eq $oldHash) 'Original preserved on failure'
    Assert-Rejected { Assert-UpdateFile $payload $asset $version 'wrong-architecture' }
    Assert-Rejected { Assert-UpdateFile $payload $asset ([version]'0.0.0.2') $architecture }
    Install-UpdateFile $payload $target $asset $version $architecture
    Assert-True (('sha256:' + (Get-FileHash -LiteralPath $target).Hash) -eq $asset.digest) 'Install verified'
    [IO.File]::Copy($PreviewExe, $payload)
    Install-UpdateFile $payload $target $asset $version $architecture
    Assert-True ([IO.File]::Exists($payload)) 'Repeated install is a no-op'
} finally {
    foreach ($file in [IO.Directory]::GetFiles($work)) { [IO.File]::Delete($file) }
    [IO.Directory]::Delete($work)
}
