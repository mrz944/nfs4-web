#!/usr/bin/env bash
# Uploads a game pack (tools/pack-game.mjs) to a Cloudflare R2 bucket with wrangler:
#   tools/r2-upload.sh BUCKET build/r2/<id> [PREFIX]      (PREFIX defaults to nfs4)
# Needs `npx wrangler login` once. Objects get a long immutable cache lifetime: a new
# pack always has a new <id> folder, so nothing is ever overwritten.
# Do not set Content-Encoding: the page unpacks the .gz chunks itself.
set -euo pipefail
bucket=${1:?bucket}; dir=${2:?pack dir}; prefix=${3:-nfs4}
id=$(basename "$dir")
[ -f "$dir/index.json" ] || { echo "$dir has no index.json (run tools/pack-game.mjs first)"; exit 1; }
put() { npx --yes wrangler r2 object put "$bucket/$prefix/$id/$1" --remote --file "$dir/$1" \
          --content-type "$2" --cache-control "public, max-age=31536000, immutable"; }
for f in "$dir"/chunk-*.gz; do put "$(basename "$f")" application/gzip; done
put index.json application/json            # last: the pack is only complete once its index is there
echo
echo "uploaded: $prefix/$id/"
echo "now set web/game-source.json to {\"url\": \"https://<your R2 domain>/$prefix/$id/\"}"
