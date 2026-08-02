# Chocolatey install script for aidbg.
#
# __URL__ and __SHA256__ are placeholders. The publish workflow substitutes the
# real release URL and SHA256 before running `choco pack`; the AU module
# (update.ps1) can also update them via Get-RemoteChecksum. Do not `choco pack`
# this file as-is.
$ErrorActionPreference = 'Stop'

$toolsDir = "$(Split-Path -Parent $MyInvocation.MyCommand.Definition)"

$packageArgs = @{
    packageName    = 'aidbg'
    url            = '__URL__'
    checksum       = '__SHA256__'
    checksumType   = 'sha256'
    unzipLocation  = $toolsDir
}

Install-ChocolateyZipPackage @packageArgs

Install-BinFile -Name 'aidbg' -Path "$toolsDir\aidbg.exe"
