#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export PS5_VULKAN_DIR="$root/vendor/PS5_Vulkan"
case "${1:-}" in
  deploy) shift; exec bash "$root/app/ps5/tools/deploy.sh" "$@" ;;
  run) shift; exec bash "$root/app/ps5/tools/run.sh" "$@" ;;
  *) echo 'Usage: bash tools/console.sh deploy|run [arguments]' >&2; exit 2 ;;
esac
