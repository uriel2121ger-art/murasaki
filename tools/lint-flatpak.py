#!/usr/bin/env python3
"""Lints the Flatpak manifest without flatpak-builder: valid YAML, required keys, pinned sources.

Usage: tools/lint-flatpak.py [manifest.yml]
"""
import sys
from pathlib import Path

try:
    import yaml
except ImportError:
    sys.exit("needs PyYAML (python3-yaml)")

path = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / "packaging/flatpak/io.github.uriel2121ger_art.Murasaki.yml")
manifest = yaml.safe_load(path.read_text())
errors = []
for key in ("app-id", "runtime", "runtime-version", "sdk", "command", "finish-args", "modules"):
    if key not in manifest:
        errors.append(f"missing key {key}")
for arg in manifest.get("finish-args", []):
    if arg in ("--filesystem=host", "--filesystem=home", "--device=all", "--talk-name=org.freedesktop.Flatpak") or arg.startswith("--filesystem=/"):
        errors.append(f"too broad: {arg}")
    if arg.startswith("--filesystem=") and not arg.endswith(":ro"):
        errors.append(f"filesystem access must be read only: {arg}")
for module in manifest.get("modules", []):
    name = module.get("name", "?")
    sources = module.get("sources", [])
    if not sources:
        errors.append(f"{name}: no sources")
    for source in sources:
        kind = source.get("type")
        if kind in ("archive", "file") and "sha256" not in source:
            errors.append(f"{name}: {source.get('url')} has no sha256")
        if kind == "git" and "commit" not in source:
            errors.append(f"{name}: git source without a commit")
        if kind in ("archive", "file", "git") and "url" in source and not source["url"].startswith("https://"):
            errors.append(f"{name}: not https: {source['url']}")
if errors:
    print("\n".join(errors))
    sys.exit(1)
print(f"{path.name}: ok ({len(manifest['modules'])} modules, {len(manifest['finish-args'])} finish-args)")
