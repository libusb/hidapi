"""Merge complete native captures and reject missing or damaged coverage."""

import collections
import json
import pathlib
import re
import shutil
import subprocess
import sys


def cov(*args):
    result = subprocess.run(
        ["timeout", "--kill-after=3s", "300s", "cov-manage-emit", *map(str, args)],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if result.stderr:
        print(result.stderr, file=sys.stderr, end="")
    result.check_returncode()
    return result.stdout


def version(text):
    release = re.search(r"\bversion (\S+)", text)
    internal = re.search(r"^Internal version numbers: (.+)", text, re.MULTILINE)
    if not release or not internal:
        raise ValueError("Unrecognized Coverity version output")
    return release.group(1) + " " + internal.group(1).strip()


def units(idir):
    listing = cov("--dir", idir, "list")
    print(listing, end="")
    paths = re.findall(r"^\s*\d+ -> (.+)$", listing, re.MULTILINE)
    if not paths or "errors)" in listing:
        raise RuntimeError(f"Empty or incomplete capture: {idir}")
    return collections.Counter(path.replace("\\", "/") for path in paths)


def merge(captures, target):
    expected = collections.Counter()
    counts = {}
    tool_version = version(cov("--ident"))
    required_sources = {
        "windows": ("/windows/hid.c",),
        "macos": ("/mac/hid.c",),
        "linux": ("/linux/hid.c", "/libusb/hid.c"),
    }
    for platform, required in required_sources.items():
        capture = captures / f"coverity-{platform}"
        if version((capture / "coverity-version.txt").read_text(encoding="utf-8-sig")) != tool_version:
            raise RuntimeError(f"Capture tool version mismatch: {platform}")
        idir = capture / "cov-int"
        cov("--dir", idir, "reset-host-name")
        cov("--dir", idir, "check-compatible")
        cov("--dir", idir, "check-integrity")
        paths = units(idir)
        for suffix in required:
            if not any(path.endswith(suffix) for path in paths):
                raise RuntimeError(f"Missing backend: {suffix}")
        counts[platform] = paths.total()
        expected.update(paths)

    # The target must be an existing capture; do not edit emit/ or its locks.
    shutil.copytree(captures / "coverity-linux" / "cov-int", target)
    for platform in ("windows", "macos"):
        cov("--dir", target, "add", captures / f"coverity-{platform}" / "cov-int")
    cov("--dir", target, "check-compatible")
    cov("--dir", target, "check-integrity")
    if units(target) != expected:
        raise RuntimeError("Merged translation units differ from the native captures")
    report = {"tool_version": tool_version, "captures": counts, "merged": expected.total()}
    pathlib.Path("coverity-merge.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    merge(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]))
