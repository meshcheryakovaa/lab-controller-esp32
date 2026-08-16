"""PlatformIO pre-build hook: build the Svelte SPA into firmware/data/www.

The frontend is served from LittleFS as pre-compressed .gz assets, so the ESP32
never spends CPU on gzip at request time.

MILESTONE 12 — WHY THIS FILE IS NOISIER THAN IT WAS.

The first real hardware bring-up produced this, and then moved on:

    [frontend] node_modules not found — skipping frontend build
    [frontend] npm not on PATH — skipping frontend build

The firmware build then succeeded, `pio run -t uploadfs` uploaded an empty
filesystem image, the board came up, and every request for the web interface
returned "not installed".  Half an evening went into the HTTP layer looking for
a routing bug that was never there.

The hook was written to be non-fatal so that a CI runner without Node could
still compile firmware.  That is a good reason, and it stays — but "the
firmware compiles without a UI" and "the filesystem image is uploaded without a
UI" are completely different situations, and the old code could not tell them
apart.  So now:

  * A missing node_modules is repaired, not reported: if npm is available the
    hook runs `npm ci` itself.  The overwhelmingly common cause of a skipped
    build was a fresh clone.
  * npm is looked up the way Windows actually stores it (npm.cmd), because
    "npm not on PATH" on a machine where npm is very much on PATH is a message
    that teaches you to distrust the tool.
  * data/www is replaced only after a build succeeds.  A failed build leaves the
    previous, working assets in place rather than an empty directory.
  * Skipping is FATAL for filesystem targets (uploadfs / buildfs).  Uploading an
    empty image is not a lesser version of uploading the UI; it is the thing
    that wasted the evening.

Set LC_SKIP_FRONTEND=1 to skip deliberately — it is honoured everywhere, on the
principle that an override you asked for is not a mistake.
"""

import gzip
import os
import shutil
import subprocess
import sys

Import("env")  # noqa: F821  (injected by PlatformIO/SCons)

try:  # pragma: no cover - only meaningful inside SCons
    from SCons.Script import COMMAND_LINE_TARGETS  # noqa: F821
except ImportError:  # pragma: no cover
    COMMAND_LINE_TARGETS = []

PROJECT_DIR = env.subst("$PROJECT_DIR")  # noqa: F821
REPO_ROOT = os.path.abspath(os.path.join(PROJECT_DIR, ".."))
FRONTEND_DIR = os.path.join(REPO_ROOT, "frontend")
DIST_DIR = os.path.join(FRONTEND_DIR, "dist")
DATA_WWW = os.path.join(PROJECT_DIR, "data", "www")

# Targets that put the filesystem on the device.  For these, no UI is an error.
FILESYSTEM_TARGETS = ("uploadfs", "buildfs", "uploadfsota")

# Assets that are already compressed or too small to benefit.
NO_GZIP_SUFFIXES = (".gz", ".png", ".jpg", ".jpeg", ".woff2", ".ico")


def log(message):
    print("[frontend] " + message)


def building_filesystem():
    return any(target in FILESYSTEM_TARGETS for target in COMMAND_LINE_TARGETS)


def www_has_content():
    if not os.path.isdir(DATA_WWW):
        return False
    for _root, _dirs, files in os.walk(DATA_WWW):
        if files:
            return True
    return False


def give_up(reason):
    """Stop, or carry on with a warning nobody can miss."""
    if building_filesystem() and not www_has_content():
        sys.stderr.write(
            "\n"
            "  ==========================================================\n"
            "   The web interface has not been built, and this target\n"
            "   uploads the filesystem.  Uploading now would put an empty\n"
            "   image on the device and the controller would answer every\n"
            "   page with 'the web interface is not installed'.\n"
            "\n"
            "   Reason: {}\n"
            "\n"
            "   Fix it with:   cd frontend && npm ci && npm run build\n"
            "   Or skip on purpose:  set LC_SKIP_FRONTEND=1\n"
            "  ==========================================================\n"
            "\n".format(reason))
        env.Exit(1)  # noqa: F821
    log(reason + " — skipping frontend build")
    if not www_has_content():
        log("WARNING: firmware/data/www is empty; the device will serve no UI")
    else:
        log("keeping the assets already in firmware/data/www")
    return False


def find_npm():
    # shutil.which honours PATHEXT on Windows, but PlatformIO runs the hook
    # inside a SCons environment whose PATH is not always the shell's.  Try the
    # explicit names too before believing that npm is missing.
    for candidate in ("npm", "npm.cmd", "npm.exe"):
        found = shutil.which(candidate)
        if found:
            return found
    for directory in os.environ.get("PATH", "").split(os.pathsep):
        for candidate in ("npm", "npm.cmd", "npm.exe"):
            path = os.path.join(directory, candidate)
            if os.path.isfile(path):
                return path
    return None


def run(npm, args):
    log("running `npm " + " ".join(args) + "`")
    # shell=True on Windows: npm is a .cmd shim, and CreateProcess will not run
    # one directly.
    return subprocess.run([npm] + list(args), cwd=FRONTEND_DIR,
                          shell=(os.name == "nt")).returncode


def prepare():
    if os.environ.get("LC_SKIP_FRONTEND") == "1":
        log("LC_SKIP_FRONTEND=1 — skipping frontend build (as requested)")
        return False

    if not os.path.isdir(FRONTEND_DIR):
        return give_up("no frontend/ directory")

    npm = find_npm()
    if npm is None:
        return give_up("npm was not found on PATH")

    if not os.path.isdir(os.path.join(FRONTEND_DIR, "node_modules")):
        log("node_modules not found — installing dependencies")
        if run(npm, ["ci"]) != 0:
            return give_up("`npm ci` failed")

    if run(npm, ["run", "build"]) != 0:
        # Fail loudly and always: a broken frontend build must never ship a
        # stale UI quietly, whatever the target.
        sys.stderr.write("[frontend] `npm run build` failed\n")
        env.Exit(1)  # noqa: F821
        return False

    if not os.path.isdir(DIST_DIR):
        return give_up("`npm run build` produced no dist/ directory")
    return True


def stage_into_littlefs():
    # Replaced only now, with a finished dist/ in hand: a failed build leaves
    # the last working assets where they are.
    if os.path.isdir(DATA_WWW):
        shutil.rmtree(DATA_WWW)
    os.makedirs(DATA_WWW, exist_ok=True)

    total_raw = 0
    total_stored = 0
    files_staged = 0
    for root, _dirs, files in os.walk(DIST_DIR):
        rel_root = os.path.relpath(root, DIST_DIR)
        target_root = os.path.join(DATA_WWW, rel_root) if rel_root != "." else DATA_WWW
        os.makedirs(target_root, exist_ok=True)
        for name in files:
            source = os.path.join(root, name)
            raw_size = os.path.getsize(source)
            total_raw += raw_size
            files_staged += 1
            if name.endswith(NO_GZIP_SUFFIXES):
                shutil.copy2(source, os.path.join(target_root, name))
                total_stored += raw_size
                continue
            target = os.path.join(target_root, name + ".gz")
            with open(source, "rb") as src, gzip.open(target, "wb", compresslevel=9) as dst:
                shutil.copyfileobj(src, dst)
            total_stored += os.path.getsize(target)

    if files_staged == 0:
        sys.stderr.write("[frontend] dist/ was empty; nothing to serve\n")
        env.Exit(1)  # noqa: F821
        return

    # index.html is what every route falls back to; without it the device has
    # assets and no way to reach them.
    if not (os.path.isfile(os.path.join(DATA_WWW, "index.html"))
            or os.path.isfile(os.path.join(DATA_WWW, "index.html.gz"))):
        sys.stderr.write("[frontend] no index.html in the staged output\n")
        env.Exit(1)  # noqa: F821
        return

    log("staged {} files, {} KiB raw -> {} KiB in the LittleFS image".format(
        files_staged, total_raw // 1024, total_stored // 1024))
    log("run `pio run -t uploadfs` to put it on the device")


if prepare():
    stage_into_littlefs()
