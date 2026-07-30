#!/usr/bin/env python3
"""Compare Steam's public GTA V Enhanced build with EVER's validated build."""

from __future__ import annotations

import argparse
import json
import os
import re
from pathlib import Path


START_MARKER = "<!-- compatibility-status:start -->"
END_MARKER = "<!-- compatibility-status:end -->"


def parse_public_build_id(steam_output: str) -> str:
    match = re.search(
        r'"branches"\s*\{\s*"public"\s*\{\s*"buildid"\s*"(\d+)"',
        steam_output,
        flags=re.DOTALL,
    )
    if not match:
        raise ValueError("SteamCMD output does not contain the public branch buildid")
    return match.group(1)


def make_status_block(config: dict[str, object], current_build: str) -> tuple[str, str, str]:
    supported_build = str(config["supported_steam_build_id"])
    game_version = str(config["supported_game_version"])
    validated_on = str(config["validated_on"])

    if current_build == supported_build:
        status = "supported"
        description = str(config["normal_repository_description"])
        block = f"""{START_MARKER}
> [!IMPORTANT]
> **Compatibility status:** validated for GTA V Enhanced `{game_version}`
> (Steam build `{supported_build}`), tested on {validated_on}.
{END_MARKER}"""
    else:
        status = "unsupported"
        description = (
            f"⚠ GTA V Enhanced Steam build {current_build} detected; compatibility is not yet "
            f"verified. Last validated: {game_version} / Steam build {supported_build}."
        )
        block = f"""{START_MARKER}
> [!CAUTION]
> **Compatibility warning:** Steam now distributes GTA V Enhanced build
> `{current_build}`, but EVER Enhanced has only been validated on game
> `{game_version}` / Steam build `{supported_build}` ({validated_on}). Do not
> assume the version-specific hooks are safe until this notice is cleared.
{END_MARKER}"""

    return status, description, block


def replace_status_block(readme: str, block: str) -> str:
    pattern = re.compile(
        re.escape(START_MARKER) + r".*?" + re.escape(END_MARKER),
        flags=re.DOTALL,
    )
    if not pattern.search(readme):
        raise ValueError("README compatibility status markers are missing")
    return pattern.sub(block, readme, count=1)


def write_github_output(path: Path, values: dict[str, str]) -> None:
    with path.open("a", encoding="utf-8") as output:
        for key, value in values.items():
            delimiter = f"EVER_{key.upper()}_EOF"
            output.write(f"{key}<<{delimiter}\n{value}\n{delimiter}\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--steam-output", type=Path, required=True)
    parser.add_argument("--config", type=Path, default=Path("compatibility.json"))
    parser.add_argument("--readme", type=Path, default=Path("README.md"))
    parser.add_argument("--github-output", type=Path)
    args = parser.parse_args()

    config = json.loads(args.config.read_text(encoding="utf-8"))
    current_build = parse_public_build_id(args.steam_output.read_text(encoding="utf-8", errors="replace"))
    status, description, block = make_status_block(config, current_build)

    original_readme = args.readme.read_text(encoding="utf-8")
    updated_readme = replace_status_block(original_readme, block)
    changed = updated_readme != original_readme
    if changed:
        args.readme.write_text(updated_readme, encoding="utf-8", newline="\n")

    values = {
        "status": status,
        "current_build": current_build,
        "supported_build": str(config["supported_steam_build_id"]),
        "supported_game_version": str(config["supported_game_version"]),
        "validated_on": str(config["validated_on"]),
        "description": description,
        "readme_changed": str(changed).lower(),
    }
    if args.github_output:
        write_github_output(args.github_output, values)

    print(json.dumps(values, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
