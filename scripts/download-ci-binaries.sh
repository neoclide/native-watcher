#!/usr/bin/env bash
set -euo pipefail

repo=neoclide/native-watcher
repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

if [[ $# -gt 1 ]]; then
  echo "Usage: $0 [run-id]" >&2
  exit 2
fi

run_id=${1:-}
if [[ -z "$run_id" ]]; then
  run_id=$(gh run list --repo "$repo" --workflow test.yml --branch main \
    --status success --limit 1 --json databaseId --jq '.[0].databaseId // empty')
fi
if [[ ! "$run_id" =~ ^[0-9]+$ ]]; then
  echo "No successful CI run found (or invalid run ID: $run_id)" >&2
  exit 1
fi

destination="$repo_dir/build/artifacts"
artifacts=$(gh api "repos/$repo/actions/runs/$run_id/artifacts" \
  --jq '.artifacts[].name | select(startswith("native-watcher-"))')
if [[ -z "$artifacts" ]]; then
  echo "No native-watcher artifacts found in CI run $run_id" >&2
  exit 1
fi

rm -rf "$destination"
gh run download "$run_id" --repo "$repo" --pattern 'native-watcher-*' \
  --dir "$destination"

while IFS= read -r artifact; do
  binary="$destination/$artifact/native_watcher.node"
  flat_binary="$destination/${artifact#native-watcher-}.node"
  mv "$binary" "$flat_binary"
  chmod 0644 "$flat_binary"
  rmdir "$destination/$artifact"
done <<< "$artifacts"

printf 'Downloaded CI run %s to %s\n' "$run_id" "$destination"
while IFS= read -r artifact; do
  printf '%s\n' "$destination/${artifact#native-watcher-}.node"
done <<< "$artifacts"
