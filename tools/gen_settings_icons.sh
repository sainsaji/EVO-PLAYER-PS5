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
  # 2026-11: one distinct icon per settings row
  "rotate-ccw:icon_rotate_ccw.png"
  "skip-forward:icon_skip_forward.png"
  "sun-medium:icon_sun_medium.png"
  "hard-drive-download:icon_hard_drive_download.png"
  "timer:icon_timer.png"
  "expand:icon_expand.png"
  "cable:icon_cable.png"
  "phone-call:icon_phone_call.png"
  "languages:icon_languages.png"
  "radio:icon_radio.png"
  "mouse-pointer-click:icon_mouse_pointer_click.png"
  "captions:icon_captions.png"
  "a-large-small:icon_a_large_small.png"
  "paintbrush:icon_paintbrush.png"
  "move-vertical:icon_move_vertical.png"
  "square:icon_square.png"
  "rows-2:icon_rows_2.png"
  "paint-bucket:icon_paint_bucket.png"
  "lightbulb:icon_lightbulb.png"
  "folder-tree:icon_folder_tree.png"
  "smartphone:icon_smartphone.png"
  "cast:icon_cast.png"
  "monitor-play:icon_monitor_play.png"
  "link:icon_link.png"
  "list-video:icon_list_video.png"
  "wand-sparkles:icon_wand_sparkles.png"
  "clock:icon_clock.png"
  "monitor:icon_monitor.png"
  "memory-stick:icon_memory_stick.png"
  "download:icon_download.png"
  "wind:icon_wind.png"
  "refresh-cw:icon_refresh_cw.png"
  "moon:icon_moon.png"
)

for item in "${ICONS[@]}"; do
  name="${item%%:*}"
  file="${item##*:}"
  ./tools/lucide_icon.sh "${name}" "projects/evoplayer/assets/icons/${file}" 96
done

echo "All settings icons generated successfully."
