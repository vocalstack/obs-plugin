#!/usr/bin/env python3
"""The newest Apple notarization of this repo's files, and the version it was for.

    build-aux/notarized.py <file prefix> <owner/repo>

Apple's history (keychain profile vocalstack-notary) has no version, so the
version is the first release published after the notarization: CI notarizes,
then publishes. Without one, the build wasn't released (yet).
"""
import json, subprocess, sys
from datetime import datetime, timezone


def when(iso):
    """Sat 7 Nov 2026, 11:58 (2 h ago), in this Mac's time zone."""
    t = datetime.fromisoformat(iso.replace("Z", "+00:00"))
    s = int((datetime.now(timezone.utc) - t).total_seconds())
    ago = "just now" if s < 60 else f"{s // 60} min ago" if s < 3600 else f"{s // 3600} h ago" if s < 86400 else f"{s // 86400} d ago"
    return t.astimezone().strftime("%a %-d %b %Y, %H:%M") + f" ({ago})"

prefix, repo = sys.argv[1].lower(), sys.argv[2]
hist = json.loads(subprocess.run(
    ["xcrun", "notarytool", "history", "--keychain-profile", "vocalstack-notary", "--output-format", "json"],
    check=True, capture_output=True, text=True).stdout)["history"]
mine = [h for h in hist if h["name"].lower().startswith(prefix)]
if not mine:
    sys.exit(print("no notarization yet"))
n = mine[0]
rels = json.loads(subprocess.run(
    ["gh", "release", "list", "-R", repo, "--limit", "50", "--json", "tagName,publishedAt"],
    check=True, capture_output=True, text=True).stdout)
after = sorted((r for r in rels if r["tagName"] != "latest" and r["publishedAt"] >= n["createdDate"][:19]), key=lambda r: r["publishedAt"])
version = after[0]["tagName"] if after else "not released"
print("  ".join([version, n["status"], n["name"], when(n["createdDate"]), n["id"]]))
