#!/bin/bash
# Sends an installer package, zip or disk image to Apple's notary service and waits for it.
#   .github/scripts/notarize.sh <file>
# With NOTARY_KEY_ID and NOTARY_ISSUER_ID set (CI), the App Store Connect
# API key in $RUNNER_TEMP/notary.p8 (or NOTARY_KEY_FILE); otherwise this
# Mac's keychain profile "vocalstack-notary" (xcrun notarytool
# store-credentials). Fails unless Apple accepts it, printing its log.
set -euo pipefail
file="$1"
if [ -n "${NOTARY_KEY_ID:-}" ]; then
  auth=(--key "${NOTARY_KEY_FILE:-$RUNNER_TEMP/notary.p8}" --key-id "$NOTARY_KEY_ID" --issuer "$NOTARY_ISSUER_ID")
else
  auth=(--keychain-profile vocalstack-notary)
fi
out=$(xcrun notarytool submit "$file" "${auth[@]}" --wait --timeout 45m --output-format json)
echo "$out"
id=$(echo "$out" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("id",""))')
status=$(echo "$out" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("status",""))')
if [ "$status" != "Accepted" ]; then
  [ -n "$id" ] && xcrun notarytool log "$id" "${auth[@]}" || true
  echo "notarization of $file: $status" >&2
  exit 1
fi
