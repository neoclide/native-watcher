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

# npm ci runs the native addon install build before the tests load it.
ssh -o BatchMode=yes -o ConnectTimeout=10 "$remote_host" \
  "cd '$remote_dir' && npm ci && npm run test"
