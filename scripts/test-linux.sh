#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
remote_host=cocnvim.com
remote_dir=/home/chemzqm/native-watcher

ssh -o BatchMode=yes -o ConnectTimeout=10 "$remote_host" \
  "test \"\$(uname -s)\" = Linux && mkdir -p '$remote_dir'"

cd "$repo_dir"
rsync -az -e 'ssh -o BatchMode=yes -o ConnectTimeout=10' \
  binding.gyp package.json package-lock.json index.js index.d.ts wrapper.js \
  src test scripts \
  "$remote_host:$remote_dir/"

# Match CI: build, strip, then test the same native addon.
ssh -o BatchMode=yes -o ConnectTimeout=10 "$remote_host" \
  "cd '$remote_dir' && npm ci && strip --strip-unneeded build/Release/native_watcher.node && npm run test"
