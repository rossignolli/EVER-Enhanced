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


def make_status_block(
    config: dict[str, object], current_build: str, locale: str = "en"
) -> tuple[str, str, str]:
    supported_build = str(config["supported_steam_build_id"])
    game_version = str(config["supported_game_version"])
    validated_on = str(config["validated_on"])

    supported = current_build == supported_build
    if supported:
        status = "supported"
        description = str(config["normal_repository_description"])
    else:
        status = "unsupported"
        description = (
            f"⚠ EN: GTA V Enhanced build {current_build} is not validated. | "
            f"PT-BR: Build {current_build} ainda não validada. | "
            f"ES: Build {current_build} aún no validada. | "
            f"Last/Última/Última: {game_version} ({supported_build})."
        )

    if locale == "pt-BR" and supported:
        block = f"""{START_MARKER}
> [!IMPORTANT]
> **Estado de compatibilidade:** validado no GTA V Enhanced `{game_version}`
> (build Steam `{supported_build}`), testado em {validated_on}.
{END_MARKER}"""
    elif locale == "pt-BR":
        block = f"""{START_MARKER}
> [!CAUTION]
> **Aviso de compatibilidade:** a Steam agora distribui o GTA V Enhanced build
> `{current_build}`, mas o EVER Enhanced só foi validado no jogo `{game_version}` /
> build Steam `{supported_build}` ({validated_on}). Não presuma que os hooks
> específicos da versão são seguros antes de este aviso ser removido.
{END_MARKER}"""
    elif locale == "es" and supported:
        block = f"""{START_MARKER}
> [!IMPORTANT]
> **Estado de compatibilidad:** validado para GTA V Enhanced `{game_version}`
> (compilación de Steam `{supported_build}`), probado el {validated_on}.
{END_MARKER}"""
    elif locale == "es":
        block = f"""{START_MARKER}
> [!CAUTION]
> **Aviso de compatibilidad:** Steam distribuye ahora GTA V Enhanced compilación
> `{current_build}`, pero EVER Enhanced solo se validó con el juego `{game_version}` /
> compilación de Steam `{supported_build}` ({validated_on}). No supongas que los
> hooks específicos de esta versión son seguros hasta que se retire este aviso.
{END_MARKER}"""
    elif supported:
        block = f"""{START_MARKER}
> [!IMPORTANT]
> **Compatibility status:** validated for GTA V Enhanced `{game_version}`
> (Steam build `{supported_build}`), tested on {validated_on}.
{END_MARKER}"""
    else:
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
    parser.add_argument("--readme", type=Path, action="append")
    parser.add_argument("--github-output", type=Path)
    args = parser.parse_args()

    config = json.loads(args.config.read_text(encoding="utf-8"))
    current_build = parse_public_build_id(args.steam_output.read_text(encoding="utf-8", errors="replace"))
    readmes = args.readme or [Path("README.md")]
    changed = False
    status = ""
    description = ""
    for readme_path in readmes:
        locale = "pt-BR" if ".pt-BR." in readme_path.name else "es" if ".es." in readme_path.name else "en"
        status, description, block = make_status_block(config, current_build, locale)
        original_readme = readme_path.read_text(encoding="utf-8")
        updated_readme = replace_status_block(original_readme, block)
        if updated_readme != original_readme:
            readme_path.write_text(updated_readme, encoding="utf-8", newline="\n")
            changed = True

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
