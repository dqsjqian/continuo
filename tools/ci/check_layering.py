#!/usr/bin/env python3
"""Enforce Continuo's two architectural invariants at build time.

Both rules exist because the failure mode they prevent is *gradual*. Nobody
decides to weld the socket layer to the parser; it happens one include at a
time, and by the time it hurts, the fix is a rewrite. A comment in a design
document cannot stop that. A failing build can.

Rule 1 — layering is one-way.

    protocol (http, ws, ...)  ->  transport (tcp, udp, unix)  ->  core

    core must not include transport or protocol headers; transport must not
    include protocol headers. Reaching *up* a layer is the exact move that
    makes a library unable to grow a second protocol later.

Rule 2 — no host framework dependency.

    Continuo must stay usable without Aria (or any other framework). Hosts
    integrate through the executor and stream seams; a single `#include
    <aria/...>` anywhere in the library would turn an optional adapter into a
    hard dependency for every consumer.

Usage:
    python3 tools/ci/check_layering.py [repo_root]

Exit status is 0 when clean, 1 when any violation is found.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

SOURCE_SUFFIXES = {".hpp", ".h", ".cpp", ".cc", ".ipp"}

# Layer name -> module directories that make up that layer.
LAYERS: dict[str, tuple[str, ...]] = {
    "core": ("core",),
    "transport": ("transport",),
    "protocol": ("http", "ws", "h2", "h3", "dns"),
}

# Layer -> include path prefixes it is not allowed to reach for.
FORBIDDEN_INCLUDES: dict[str, tuple[str, ...]] = {
    "core": ("continuo/transport/", "continuo/http/", "continuo/ws/",
             "continuo/h2/", "continuo/h3/", "continuo/dns/"),
    "transport": ("continuo/http/", "continuo/ws/", "continuo/h2/",
                  "continuo/h3/", "continuo/dns/"),
    "protocol": (),
}

# Include prefixes no layer may use, with the reason reported to the user.
BANNED_EVERYWHERE: dict[str, str] = {
    "aria/": "Continuo must not depend on Aria; hosts integrate via the executor/stream seams",
}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')


def layer_of(path: Path, modules_root: Path) -> str | None:
    """Return the layer a file belongs to, or None if it is outside modules/."""
    try:
        relative = path.relative_to(modules_root)
    except ValueError:
        return None
    if not relative.parts:
        return None
    module = relative.parts[0]
    for layer, modules in LAYERS.items():
        if module in modules:
            return layer
    return None


def iter_sources(root: Path):
    for path in sorted(root.rglob("*")):
        if path.is_file() and path.suffix in SOURCE_SUFFIXES:
            yield path


def check(repo_root: Path) -> list[str]:
    modules_root = repo_root / "modules"
    if not modules_root.is_dir():
        return [f"{modules_root}: modules/ directory not found"]

    violations: list[str] = []

    for path in iter_sources(modules_root):
        layer = layer_of(path, modules_root)
        forbidden = FORBIDDEN_INCLUDES.get(layer or "", ())
        display = path.relative_to(repo_root)

        for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            match = INCLUDE_RE.match(line)
            if not match:
                continue
            included = match.group(1)

            for prefix, reason in BANNED_EVERYWHERE.items():
                if included.startswith(prefix):
                    violations.append(
                        f"{display}:{lineno}: includes '{included}' — {reason}"
                    )

            for prefix in forbidden:
                if included.startswith(prefix):
                    violations.append(
                        f"{display}:{lineno}: layer '{layer}' includes '{included}' — "
                        f"dependencies only point downwards"
                    )

    return violations


def main(argv: list[str]) -> int:
    repo_root = Path(argv[1]).resolve() if len(argv) > 1 else Path(__file__).resolve().parents[2]
    violations = check(repo_root)

    if violations:
        print("Layering violations found:\n", file=sys.stderr)
        for violation in violations:
            print(f"  {violation}", file=sys.stderr)
        print(f"\n{len(violations)} violation(s).", file=sys.stderr)
        return 1

    print(f"Layering OK — {repo_root}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
