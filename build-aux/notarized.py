#!/usr/bin/env python3
"""Apple notarizations of this repo's files: any still pending, any newer refusal, and the last accepted (with its version).

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


def building():
    """A release run (a version tag) still going in CI, and the step it is on."""
    runs = json.loads(subprocess.run(
        ["gh", "run", "list", "-R", repo, "--limit", "10", "--json", "databaseId,headBranch,status,event"],
        capture_output=True, text=True).stdout or "[]")
    for r in runs:
        if r["status"] != "completed" and r["event"] == "push" and r["headBranch"] not in ("main", "master"):
            jobs = json.loads(subprocess.run(
                ["gh", "run", "view", str(r["databaseId"]), "-R", repo, "--json", "jobs"],
                capture_output=True, text=True).stdout or "{}").get("jobs", [])
            steps = [s["name"] for j in jobs if "mac" in j["name"].lower() for s in j["steps"] if s["status"] == "in_progress"]
            print(f"building  {r['headBranch']}  in CI, not sent to Apple yet" + (f" (now: {steps[0]})" if steps else ""))

if not mine:
    building()
    sys.exit(print("no notarization yet"))
rels = json.loads(subprocess.run(
    ["gh", "release", "list", "-R", repo, "--limit", "50", "--json", "tagName,publishedAt"],
    check=True, capture_output=True, text=True).stdout)


def version(n):
    if n["status"] != "Accepted":
        return "-"
    after = sorted((r for r in rels if r["tagName"] != "latest" and r["publishedAt"] >= n["createdDate"][:19]), key=lambda r: r["publishedAt"])
    return after[0]["tagName"] if after else "not released"


# Whatever is still with Apple, anything newer that Apple refused, then the last accepted.
accepted = next((h for h in mine if h["status"] == "Accepted"), None)
pending = [h for h in mine if h["status"] == "In Progress"]
refused = [h for h in mine if h["status"] not in ("Accepted", "In Progress") and (not accepted or h["createdDate"] > accepted["createdDate"])]
for label, h in [("pending", h) for h in pending] + [(h["status"].lower(), h) for h in refused] + ([("accepted", accepted)] if accepted else []):
    print("  ".join([label.ljust(8), version(h).ljust(6), h["name"], when(h["createdDate"]), h["id"]]))
if not pending:
    building()
    print("nothing waiting on Apple")
