#!/usr/bin/env python3
"""Validate the NpcChat source/config contract.

This intentionally stays dependency-free so it can run in GitHub Actions or directly
from an AzerothCore module checkout:

    python scripts/validate_config_contract.py

It catches the kind of configuration drift that accumulated during early iteration:
- duplicate active NpcChat.* keys in mod_npcchat.conf.dist
- source GetOption() keys missing from the canonical config
- canonical config keys no longer read anywhere in source

A small explicit alias allowlist covers legacy names that remain accepted only for
backward compatibility and therefore should not be reintroduced into the canonical file.
"""

from __future__ import annotations

from collections import Counter
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
CONF_PATH = ROOT / "conf" / "mod_npcchat.conf.dist"
SRC_DIR = ROOT / "src"

# These are intentionally supported by code but deliberately absent from the canonical config.
LEGACY_SOURCE_ONLY_ALIASES = {
    "NpcChat.GeneratePromptMaxTokens",
    "NpcChat.GeneratePromptTemperature",
    "NpcChat.SubPromptCreatorAccountIds",
    "NpcChat.SubPromptCreatorAccountIDs",
}

CONFIG_LINE_RE = re.compile(r"^\s*(NpcChat\.[A-Za-z0-9_.]+)\s*=", re.MULTILINE)
GET_OPTION_RE = re.compile(
    r"GetOption\s*<[^>]+>\s*\(\s*\"(NpcChat\.[A-Za-z0-9_.]+)\"",
    re.MULTILINE,
)


def load_source() -> str:
    chunks: list[str] = []
    for path in sorted(SRC_DIR.rglob("*")):
        if path.suffix not in {".cpp", ".h", ".hpp"}:
            continue
        chunks.append(path.read_text(encoding="utf-8"))
    return "\n".join(chunks)


def main() -> int:
    if not CONF_PATH.exists():
        print(f"ERROR: missing canonical config: {CONF_PATH.relative_to(ROOT)}")
        return 1

    conf = CONF_PATH.read_text(encoding="utf-8")
    source = load_source()

    config_keys = CONFIG_LINE_RE.findall(conf)
    source_keys = GET_OPTION_RE.findall(source)

    config_counts = Counter(config_keys)
    duplicates = sorted(key for key, count in config_counts.items() if count > 1)

    config_set = set(config_keys)
    source_set = set(source_keys)

    missing_from_config = sorted(
        source_set - config_set - LEGACY_SOURCE_ONLY_ALIASES
    )
    stale_config = sorted(config_set - source_set)

    failed = False
    if duplicates:
        failed = True
        print("ERROR: duplicate active config keys:")
        for key in duplicates:
            print(f"  - {key} ({config_counts[key]} definitions)")

    if missing_from_config:
        failed = True
        print("ERROR: source reads NpcChat options absent from canonical config:")
        for key in missing_from_config:
            print(f"  - {key}")

    if stale_config:
        failed = True
        print("ERROR: canonical config contains options no longer read by source:")
        for key in stale_config:
            print(f"  - {key}")

    aliases_seen = sorted(LEGACY_SOURCE_ONLY_ALIASES & source_set)
    print(
        f"NpcChat config contract: {len(config_set)} canonical keys, "
        f"{len(source_set)} source keys, {len(aliases_seen)} legacy aliases."
    )

    if failed:
        return 1

    print("NpcChat config contract OK.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
