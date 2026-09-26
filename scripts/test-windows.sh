#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
remote_host=win11
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/native-watcher-windows.XXXXXX")
run_name=${work_dir##*/}
trap 'rm -rf "$work_dir"' EXIT

# Transfer source, including uncommitted changes, without macOS metadata.
COPYFILE_DISABLE=1 tar -czf "$work_dir/source.tar.gz" -C "$repo_dir" \
  binding.gyp package.json package-lock.json index.js index.d.ts wrapper.js \
  src test scripts
printf 'Uploading source to %s...\n' "$remote_host"
scp -o BatchMode=yes -o ConnectTimeout=10 \
  "$work_dir/source.tar.gz" "$remote_host:$run_name.tar.gz"

remote_script=$(cat <<POWERSHELL
\$ErrorActionPreference = 'Stop'
\$ProgressPreference = 'SilentlyContinue'
\$archive = Join-Path \$env:USERPROFILE '$run_name.tar.gz'
\$directory = Join-Path \$env:USERPROFILE 'native-watcher'
New-Item -ItemType Directory -Path \$directory -Force | Out-Null
try {
    tar.exe -xzf \$archive -C \$directory
    if (\$LASTEXITCODE -ne 0) { exit \$LASTEXITCODE }
    Set-Location -LiteralPath \$directory
    node.exe -p "process.platform + ' ' + process.arch + ' ' + process.version"
    if (\$LASTEXITCODE -ne 0) { exit \$LASTEXITCODE }
    # npm ci rebuilds the addon; npm.cmd avoids PowerShell script policy.
    npm.cmd ci
    if (\$LASTEXITCODE -ne 0) { exit \$LASTEXITCODE }
    npm.cmd run test
    exit \$LASTEXITCODE
} finally {
    Remove-Item -LiteralPath \$archive -Force
}
POWERSHELL
)

# EncodedCommand avoids quoting source paths across Bash and PowerShell.
encoded_script=$(printf '%s' "$remote_script" | iconv -f UTF-8 -t UTF-16LE | base64 | tr -d '\n')
printf 'Building and testing on %s...\n' "$remote_host"
ssh -o BatchMode=yes -o ConnectTimeout=10 "$remote_host" \
  "powershell.exe -NoProfile -NonInteractive -EncodedCommand $encoded_script; exit \$LASTEXITCODE"
