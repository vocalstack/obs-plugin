#!/bin/bash
# Staples Apple's notarization ticket to an installer package, app or disk image. Apple's ticket
# server sometimes drops the connection right after accepting, so try a few times.
set -uo pipefail
for i in 1 2 3 4 5; do
    xcrun stapler staple "$1" && exit 0
    echo "staple attempt $i failed; retrying in $((i * 15)) s"
    sleep $((i * 15))
done
xcrun stapler staple "$1"
