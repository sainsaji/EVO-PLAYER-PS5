#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

ICONS=(
  "speaker:icon_speaker.png"
  "cpu:icon_cpu.png"
  "sparkles:icon_sparkles.png"
  "brain:icon_brain.png"
  "gauge:icon_gauge.png"
  "sun:icon_sun.png"
  "type:icon_type.png"
  "volume-2:icon_volume.png"
  "gamepad-2:icon_gamepad.png"
  "keyboard:icon_keyboard.png"
  "file-text:icon_report.png"
  "activity:icon_activity.png"
  "tv:icon_tv.png"
)

for item in "${ICONS[@]}"; do
  name="${item%%:*}"
  file="${item##*:}"
  ./tools/lucide_icon.sh "${name}" "projects/evoplayer/assets/icons/${file}" 96
done

echo "All settings icons generated successfully."
