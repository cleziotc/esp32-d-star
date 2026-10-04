#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / "main" / "web"
OUT = ROOT / "main" / "web_assets.cpp"

FILES = [
    ("kIndexHtml", WEB / "index.html"),
    ("kStyleCss", WEB / "style.css"),
    ("kAppJs", WEB / "app.js"),
]

def emit_array(name: str, data: bytes) -> str:
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    return f"const uint8_t {name}[] = {{\n" + "\n".join(lines) + f"\n}};\nconst size_t {name}Len = sizeof({name});\n\n"

out = ['#include "web_assets.h"\n\n']
for name, path in FILES:
    out.append(emit_array(name, path.read_bytes()))
OUT.write_text("".join(out), encoding="utf-8")
print(f"Generated {OUT}")
