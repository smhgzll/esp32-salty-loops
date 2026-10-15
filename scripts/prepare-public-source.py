#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Export the current sources as one repository, without local Git history or ignored files.

Run --check to inspect the selection without copying anything. The default output is
.public/esp32-salty-loops; an existing output is never overwritten. --init-git creates an
uncommitted main branch with the project remote and public author identity. No network
requests, commits or pushes are performed.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
REMOTE = "https://github.com/smhgzll/esp32-salty-loops.git"
PRUNE = {
    ".git", ".public", ".aws", ".agents", ".codex", ".claude", ".idea", ".vscode",
    "node_modules", "managed_components", "__pycache__", ".pytest_cache", "build", "dist",
    "backups", "sf2", "mod",
}
PATTERNS = {
    "local filesystem path": re.compile(rb"/(?:home|Users|mnt)/[^\s/]+/"),
    "local network address": re.compile(
        rb"\b(?:192\.168\.(?!4\.1\b)\d{1,3}\.\d{1,3}|"
        rb"10\.(?:\d{1,3}\.){2}\d{1,3}|172\.(?:1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3})\b"
    ),
    "private key": re.compile(rb"-----BEGIN (?:RSA |OPENSSH |EC )?PRIVATE KEY-----"),
    "GitHub token": re.compile(rb"\b(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{30,})\b"),
    "credential in URL": re.compile(rb"https?://[^\s/@]+:[^\s/@]+@"),
}


def selected_files():
    files = []
    for directory, dirs, names in os.walk(ROOT):
        dirs[:] = sorted(d for d in dirs if d not in PRUNE and not d.startswith("build-"))
        files.extend((Path(directory) / name).relative_to(ROOT).as_posix() for name in names)
    files.sort()
    # A temporary index applies the tracked ignore rules even to files tracked in either of
    # the older nested development repositories. Global Git excludes are deliberately disabled.
    with tempfile.TemporaryDirectory(prefix="salty-public-index-") as scratch:
        subprocess.run(["git", "init", "--quiet", scratch], check=True, capture_output=True)
        result = subprocess.run(
            ["git", "--git-dir=" + str(Path(scratch) / ".git"), "--work-tree=" + str(ROOT),
             "-c", "core.excludesFile=/dev/null", "check-ignore", "--no-index", "-z", "--stdin"],
            input="\0".join(files).encode() + b"\0", capture_output=True,
        )
        if result.returncode not in (0, 1):
            raise SystemExit("Cannot evaluate the repository ignore rules.")
        ignored = set(result.stdout.decode().strip("\0").split("\0"))
    return [name for name in files if name not in ignored]


def local_secrets():
    """Read literal local credentials for comparison; never execute the config or print values."""
    config = ROOT / "esp32-s3-daw/config/local.env"
    if not config.is_file():
        return []
    secrets = []
    for line in config.read_text().splitlines():
        key, separator, raw = line.partition("=")
        if not separator or not re.search(r"PASSWORD|TOKEN|SECRET|API_KEY", key):
            continue
        try:
            words = shlex.split(raw, comments=True)
        except ValueError:
            continue
        if len(words) == 1 and len(words[0]) >= 8 and not words[0].startswith("change-this-"):
            secrets.append(words[0].encode())
    return secrets


def audit(files):
    issues = []
    secrets = local_secrets()
    for name in files:
        path = ROOT / name
        if path.is_symlink():
            issues.append(f"{name}: symlinks require manual review")
            continue
        data = path.read_bytes()
        if any(secret in data for secret in secrets):
            issues.append(f"{name}: contains a local credential")
        # Binary resources are restricted by the ignore allowlist. The supplied interface
        # screenshot is included unchanged, as requested, including its browser chrome.
        if path.suffix.lower() in {".png", ".jpg", ".jpeg", ".webp", ".mid", ".wav", ".sf2"}:
            continue
        for description, pattern in PATTERNS.items():
            if pattern.search(data):
                issues.append(f"{name}: {description}")
    if issues:
        raise SystemExit("Source audit failed (values omitted):\n" + "\n".join(issues))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="audit only; do not create an output")
    parser.add_argument("--output", type=Path, default=ROOT / ".public/esp32-salty-loops")
    parser.add_argument("--init-git", action="store_true", help="initialize the output; do not commit or push")
    args = parser.parse_args()
    files = selected_files()
    audit(files)
    total = sum((ROOT / name).stat().st_size for name in files)
    if args.check:
        print(json.dumps({"audit": "passed", "files": len(files), "bytes": total}))
        return
    output = args.output.resolve()
    if output.exists():
        raise SystemExit("Output already exists; choose a new --output directory.")
    if output == ROOT or output in ROOT.parents:
        raise SystemExit("Output must not replace the source tree or its parent directories.")
    output.mkdir(parents=True)
    for name in files:
        destination = output / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / name, destination)
        destination.chmod(0o755 if (ROOT / name).stat().st_mode & 0o111 else 0o644)
    if args.init_git:
        commands = [
            ["init", "--quiet", "--initial-branch=main"],
            ["remote", "add", "origin", REMOTE],
            ["config", "user.name", "smhgzll"],
            ["config", "user.email", "13357620+smhgzll@users.noreply.github.com"],
        ]
        for command in commands:
            subprocess.run(["git", "-C", str(output), *command], check=True, capture_output=True)
    print(json.dumps({"audit": "passed", "output": str(output), "files": len(files),
                      "bytes": total, "git_initialized": args.init_git, "committed": False, "pushed": False}))


if __name__ == "__main__":
    main()
