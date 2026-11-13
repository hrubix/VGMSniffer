param(
  [Parameter(Mandatory = $true)][string]$ClapPath,
  [Parameter(Mandatory = $true)][string]$DestDir
)

New-Item -ItemType Directory -Force -Path $DestDir | Out-Null
$dest = Join-Path $DestDir "scc-vgm.clap"
try {
  Copy-Item $ClapPath $dest -Force -ErrorAction Stop
  Write-Host "Installed $ClapPath -> $dest"
} catch {
  $alt = Join-Path $DestDir "scc-vgm-new.clap"
  Copy-Item $ClapPath $alt -Force
  Write-Host "Plugin locked; wrote $alt instead. Quit Bitwig and rename to scc-vgm.clap"
}
exit 0
