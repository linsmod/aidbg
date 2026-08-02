$ErrorActionPreference = 'Stop'

# AU (Automatic Updater) script used by the chocolatey-community package
# maintainers. The primary publisher is the GitHub Actions workflow
# (.github/workflows/publish.yml); this script is a fallback so the package can
# be updated with the standard `au` module too.

$repo = 'linsmod/-vc-dev-debuging-tool-for-ai-agent'
$release = Invoke-RestMethod -Uri "https://api.github.com/repos/$repo/releases/latest"

$version = $release.tag_name -replace '^v', ''
$url = ($release.assets |
    Where-Object { $_.name -match '^aidbg-x64-.*\.zip$' } |
    Select-Object -First 1).browser_download_url

$checksum = Get-RemoteChecksum $url

$packageArgs = @{
    version      = $version
    url          = $url
    checksum     = $checksum
    checksumType = 'sha256'
}

Update-Package @packageArgs
