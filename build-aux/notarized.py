#!/usr/bin/env python3
"""The newest Apple notarization of this repo's files, and the version it was for.

    build-aux/notarized.py <file prefix> <owner/repo>

Apple's history (keychain profile vocalstack-notary) has no version, so the
version is the first release published after the notarization: CI notarizes,
then publishes. Without one, the build wasn't released (yet).
"""
import json, subprocess, sys

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
print("  ".join([version, n["status"], n["name"], n["createdDate"], n["id"]]))
