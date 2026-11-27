#!/usr/bin/env python3
"""Migrate an existing Glimply profile from a console outside Plasma."""

import argparse
import filecmp
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def atomic_write(path, content, mode):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as stream:
        staged = Path(stream.name)
    try:
        staged.write_bytes(content)
        staged.chmod(mode)
        staged.replace(path)
    finally:
        staged.unlink(missing_ok=True)


def migrate(config, data, state, apply):
    copies = []
    changes = []
    for root, old, new in (
        (config, "glimplyrc", "itsovarc"),
        (data, "glimply/pins", "itsova/pins"),
        (state, "glimply/history.json", "itsova/history.json"),
    ):
        source, dest = root / old, root / new
        if source.exists():
            if dest.exists():
                if source.is_dir() and dest.is_dir():
                    names = {p.name for p in source.iterdir()}
                    identical = names == {p.name for p in dest.iterdir()} and all(
                        (source / name).is_file() and (dest / name).is_file()
                        and filecmp.cmp(source / name, dest / name, shallow=False)
                        for name in names
                    )
                else:
                    identical = source.is_file() and dest.is_file() and filecmp.cmp(source, dest, shallow=False)
                if not identical:
                    raise ValueError(f"Both profiles contain data at {dest}. Back up and resolve the conflict first.")
            else:
                copies.append((source, dest))

    shortcuts = config / "kglobalshortcutsrc"
    if shortcuts.is_file():
        old = shortcuts.read_text()
        if re.search(r"^\[glimply\](?:\[|$)", old, re.M):
            if re.search(r"^\[itsova\](?:\[|$)", old, re.M):
                raise ValueError("Both shortcut groups exist. Back up and resolve the conflict first.")
            group = ""
            lines = []
            for line in old.splitlines(keepends=True):
                if line.startswith("["):
                    group = line[1:].split("]", 1)[0]
                if group == "glimply":
                    if line.startswith("[glimply]"):
                        line = line.replace("[glimply]", "[itsova]", 1)
                    elif line.startswith("_k_friendly_name="):
                        line = "_k_friendly_name=Itsova\n"
                lines.append(line)
            changes.append((shortcuts, "".join(lines).encode()))

    plasma = config / "plasma-org.kde.plasma.desktop-appletsrc"
    if plasma.is_file():
        original = plasma.read_bytes()
        updated = re.sub(
            rb"(?<![\w.-])io\.github\.m00sfett\.glimply\.widget(?![\w.-])",
            b"io.github.m00sfett.itsova.widget", original,
        )
        if updated != original:
            changes.append((plasma, updated))

    autostart = config / "autostart/io.github.m00sfett.glimply.desktop"
    if autostart.is_file():
        changes.append((autostart, None))

    for source, dest in copies:
        print(f"Copy {source} -> {dest}")
    for path, updated in changes:
        backup = path.with_name(path.name + ".pre-itsova")
        if backup.exists() and backup.read_bytes() != path.read_bytes():
            raise ValueError(f"Backup already exists and differs: {backup}")
        print(f"{'Update' if updated is not None else 'Disable'} {path} (backup: {backup})")
    if not apply:
        return
    for source, dest in copies:
        dest.parent.mkdir(parents=True, exist_ok=True)
        if source.is_dir():
            with tempfile.TemporaryDirectory(dir=dest.parent) as temp:
                staged = Path(temp) / "pins"
                shutil.copytree(source, staged)
                staged.rename(dest)
        else:
            atomic_write(dest, source.read_bytes(), source.stat().st_mode & 0o777)
    for path, updated in changes:
        backup = path.with_name(path.name + ".pre-itsova")
        mode = path.stat().st_mode & 0o777
        if not backup.exists():
            atomic_write(backup, path.read_bytes(), mode)
        if updated is None:
            path.unlink()
        else:
            atomic_write(path, updated, mode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true", help="Apply the planned changes (default: preview)")
    args = parser.parse_args()
    if args.apply:
        for name in ("glimply", "itsova", "plasmashell", "kglobalacceld", "kglobalaccel"):
            result = subprocess.run(["pgrep", "-u", str(os.getuid()), "-x", name], stdout=subprocess.DEVNULL)
            if result.returncode != 1:
                parser.error(f"Cannot confirm {name} is stopped. Log out of Plasma and run this from a console.")
    home = Path.home()
    try:
        migrate(
            Path(os.environ.get("XDG_CONFIG_HOME") or home / ".config"),
            Path(os.environ.get("XDG_DATA_HOME") or home / ".local/share"),
            Path(os.environ.get("XDG_STATE_HOME") or home / ".local/state"),
            args.apply,
        )
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
