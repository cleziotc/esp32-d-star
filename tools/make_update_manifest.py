#!/usr/bin/env python3
import argparse
import json
import re
from datetime import datetime, timezone
from pathlib import Path

REPO = "cleziotc/esp32-d-star"

def changelog_notes(version: str) -> str:
    text = Path("CHANGELOG.md").read_text(encoding="utf-8")
    match = re.search(
        rf"^## v{re.escape(version)}\s*\n(.*?)(?=^## v|\Z)",
        text,
        flags=re.MULTILINE | re.DOTALL,
    )
    if not match:
        return "Release do firmware Polar D-Star ESP32."
    lines = []
    for raw in match.group(1).strip().splitlines():
        line = raw.strip()
        if line.startswith("- "):
            line = line[2:]
        if line:
            lines.append(line)
    return "\n".join(lines)

def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--size", required=True, type=int)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--notes-output", required=True)
    args = parser.parse_args()

    notes = changelog_notes(args.version)
    tag = f"v{args.version}"
    manifest = {
        "version": args.version,
        "name": f"Polar D-Star {tag}",
        "target": "esp32s3",
        "idf": "6.1",
        "published_at": datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z"),
        "size": args.size,
        "sha256": args.sha256.lower(),
        "firmware": f"https://github.com/{REPO}/releases/download/{tag}/polar_dstar_esp32.bin",
        "release_url": f"https://github.com/{REPO}/releases/tag/{tag}",
        "notes": notes,
    }
    Path(args.output).write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    Path(args.notes_output).write_text(
        f"## Polar D-Star {tag}\n\n" + "\n".join(f"- {line}" for line in notes.splitlines()) + "\n",
        encoding="utf-8",
    )

if __name__ == "__main__":
    main()
