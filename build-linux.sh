#!/usr/bin/env bash
set -euo pipefail
python3 tools/generate_web_assets.py
idf.py set-target esp32s3
idf.py build
