#!/usr/bin/env bash
set -euo pipefail
BASE="$(cd "$(dirname "$0")/.." && pwd)"
rm -rf "$BASE/artifacts"
echo "Cleaned $BASE/artifacts"
