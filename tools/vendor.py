#!/usr/bin/env python3
"""
vendor.py — manage vendored libraries via git subtree + vendored.json manifest.

See planning/osc-mixer-foundation/05-vendoring-strategy.md for the design rationale.

Subcommands:
    status              show pinned vs current subtree state for each entry; flag drift
    verify              CI check; exit non-zero on drift
    update <name>       run `git subtree pull` against the manifest's branch; update pinnedCommit
    update <name> <sha> pin to a specific commit (e.g. an open PR's HEAD)
    add <name> <url> <branch> --prefix=lib/<name>
                        run `git subtree add`; append to manifest
    freeze              record current subtree state into manifest (one-shot bootstrap)
    contribute <name> <branch>
                        push subtree to upstreamFork remote on a feature branch
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MANIFEST_PATH = REPO_ROOT / "vendored.json"


def load_manifest() -> dict:
    if not MANIFEST_PATH.exists():
        sys.exit(f"error: {MANIFEST_PATH} does not exist; run `vendor.py freeze` to bootstrap")
    return json.loads(MANIFEST_PATH.read_text())


def save_manifest(manifest: dict) -> None:
    MANIFEST_PATH.write_text(json.dumps(manifest, indent=2) + "\n")


def run(cmd: list[str], check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, text=True, check=check)


def find_subtree_head(prefix: str) -> str | None:
    """
    Walk git log looking for the most recent `git-subtree-dir: <prefix>` trailer
    and return the corresponding `git-subtree-split` value (the upstream commit
    hash that's currently checked out in the subtree).
    """
    log = run(
        ["git", "log", "--grep", f"git-subtree-dir: {prefix}", "--format=%H%n%b%n--END--"],
        check=False,
    )
    if log.returncode != 0 or not log.stdout:
        return None

    blocks = log.stdout.split("--END--")
    for block in blocks:
        if f"git-subtree-dir: {prefix}" not in block:
            continue
        for line in block.splitlines():
            line = line.strip()
            if line.startswith("git-subtree-split:"):
                return line.split(":", 1)[1].strip()
    return None


def cmd_status(args: argparse.Namespace) -> int:
    manifest = load_manifest()
    drifted = 0
    for name, entry in manifest["vendoredLibraries"].items():
        prefix = entry["prefix"]
        pinned = entry["pinnedCommit"]
        current = find_subtree_head(prefix)
        if current is None:
            print(f"  ?  {name:32}  no subtree commit found at {prefix}")
            drifted += 1
        elif current == pinned:
            print(f"  ok {name:32}  {pinned[:12]}")
        else:
            print(f"  !! {name:32}  pinned={pinned[:12]}  current={current[:12]}  DRIFT")
            drifted += 1
    return 1 if drifted else 0


def cmd_verify(args: argparse.Namespace) -> int:
    rc = cmd_status(args)
    if rc:
        print("\nverify FAILED — vendored.json does not match working tree", file=sys.stderr)
    return rc


def cmd_freeze(args: argparse.Namespace) -> int:
    """Record current subtree state into manifest."""
    manifest = load_manifest()
    changed = 0
    for name, entry in manifest["vendoredLibraries"].items():
        prefix = entry["prefix"]
        current = find_subtree_head(prefix)
        if current and current != entry["pinnedCommit"]:
            print(f"  freeze {name}: {entry['pinnedCommit'][:12]} -> {current[:12]}")
            entry["pinnedCommit"] = current
            changed += 1
    if changed:
        save_manifest(manifest)
        print(f"\nupdated vendored.json ({changed} entry{'ies' if changed != 1 else 'y'})")
    else:
        print("no changes; manifest already matches working tree")
    return 0


def cmd_update(args: argparse.Namespace) -> int:
    manifest = load_manifest()
    if args.name not in manifest["vendoredLibraries"]:
        sys.exit(f"error: {args.name} not in manifest")
    entry = manifest["vendoredLibraries"][args.name]
    ref = args.sha if args.sha else entry["branch"]
    print(f"pulling {args.name} from {entry['remote']} ref={ref} into {entry['prefix']}")
    proc = run(
        [
            "git", "subtree", "pull",
            "--prefix", entry["prefix"],
            entry["remote"], ref,
            "--squash",
        ],
        check=False,
    )
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    if proc.returncode != 0:
        return proc.returncode
    new_head = find_subtree_head(entry["prefix"])
    if new_head:
        entry["pinnedCommit"] = new_head
        save_manifest(manifest)
        print(f"updated pinnedCommit to {new_head}")
    return 0


def cmd_add(args: argparse.Namespace) -> int:
    manifest = load_manifest()
    if args.name in manifest["vendoredLibraries"]:
        sys.exit(f"error: {args.name} already in manifest")
    print(f"adding subtree {args.name} from {args.url} branch={args.branch} at {args.prefix}")
    proc = run(
        [
            "git", "subtree", "add",
            "--prefix", args.prefix,
            args.url, args.branch,
            "--squash",
        ],
        check=False,
    )
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    if proc.returncode != 0:
        return proc.returncode
    head = find_subtree_head(args.prefix)
    manifest["vendoredLibraries"][args.name] = {
        "prefix": args.prefix,
        "remote": args.url,
        "branch": args.branch,
        "pinnedCommit": head or "",
        "addedDate": "",
        "license": "",
        "purpose": "",
        "localPatches": [],
        "upstreamFork": None,
        "contributionTargets": [],
    }
    save_manifest(manifest)
    print(f"added {args.name} to manifest; please fill in addedDate, license, and purpose")
    return 0


def cmd_contribute(args: argparse.Namespace) -> int:
    manifest = load_manifest()
    if args.name not in manifest["vendoredLibraries"]:
        sys.exit(f"error: {args.name} not in manifest")
    entry = manifest["vendoredLibraries"][args.name]
    fork = entry.get("upstreamFork")
    if not fork:
        sys.exit(f"error: {args.name} has no upstreamFork set in manifest")
    print(f"pushing {entry['prefix']} subtree to {fork} branch={args.branch}")
    proc = run(
        [
            "git", "subtree", "push",
            "--prefix", entry["prefix"],
            fork, args.branch,
        ],
        check=False,
    )
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    return proc.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status", help="show pinned vs current subtree state")
    sub.add_parser("verify", help="CI check; exit non-zero on drift")
    sub.add_parser("freeze", help="record current subtree state into manifest")

    p_update = sub.add_parser("update", help="run subtree pull and update pinnedCommit")
    p_update.add_argument("name")
    p_update.add_argument("sha", nargs="?", default=None,
                          help="optional specific commit to pin to (default: branch HEAD)")

    p_add = sub.add_parser("add", help="add a new vendored library")
    p_add.add_argument("name")
    p_add.add_argument("url")
    p_add.add_argument("branch")
    p_add.add_argument("--prefix", required=True)

    p_contrib = sub.add_parser("contribute", help="push subtree to upstreamFork branch")
    p_contrib.add_argument("name")
    p_contrib.add_argument("branch")

    args = parser.parse_args()

    handlers = {
        "status": cmd_status,
        "verify": cmd_verify,
        "freeze": cmd_freeze,
        "update": cmd_update,
        "add": cmd_add,
        "contribute": cmd_contribute,
    }
    return handlers[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
