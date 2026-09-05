#!/usr/bin/env bash
#
# setup-github-project.sh — build & sync the "EVO Player Roadmap" GitHub Project
# (Projects v2) so every issue is tracked and can be visualised.
#
# Idempotent — safe to re-run any time (e.g. after re-labelling issues) to
# resync the Priority / Area / Status fields.
#
# What it does:
#   1. creates the user-level Project "EVO Player Roadmap" if it doesn't exist
#   2. adds the custom fields  Priority  and  Area   (Status is built-in)
#   3. adds every issue in the repo (open + closed) as a project item
#   4. back-fills each item:
#        Priority  <- the  "priority: <x>"  label
#        Area      <- the most specific domain label (see AREA_RANK below)
#        Status    <- Done if the issue is closed, else Todo
#
# ONE-TIME prerequisite (interactive — opens a browser):
#     gh auth refresh -s project -h github.com
#
# Run from the HOST (needs the `gh` CLI — it is not in the dev container).
#
# Steps the GitHub API can NOT do yet — do them once in the web UI, see
# docs/project-tracking.md:
#   - create the saved Views (Board / Priority / Roadmap / Blocked)
#   - enable the built-in workflows (Auto-add to project, Item closed -> Done)
#
set -euo pipefail

OWNER="${PROJECT_OWNER:-sainsaji}"
REPO="${PROJECT_REPO:-EVO-PLAYER-PS5}"
TITLE="${PROJECT_TITLE:-EVO Player Roadmap}"

# Most specific domain label wins. "documentation" is mapped to "docs".
AREA_RANK="native-decode rmlui gpu subtitles audio playback memory performance addons app-module network ci ui documentation"

command -v gh >/dev/null || { echo "error: gh CLI not found on PATH"; exit 1; }
if ! gh auth status -h github.com 2>&1 | grep -qi "project"; then
  cat <<'MSG'
error: the gh token is missing the 'project' scope. Run this once:

    gh auth refresh -s project -h github.com

then re-run this script.
MSG
  exit 1
fi

say() { printf '  %s\n' "$*"; }

# --------------------------------------------------------------------------
echo "==> project"
PNUM=$(gh project list --owner "$OWNER" --format json \
        --jq "[.projects[] | select(.title==\"$TITLE\") | .number][0] // empty")
if [[ -z "${PNUM:-}" ]]; then
  PNUM=$(gh project create --owner "$OWNER" --title "$TITLE" --format json --jq '.number')
  say "created project #$PNUM"
else
  say "found project #$PNUM"
fi
PID=$(gh project view "$PNUM" --owner "$OWNER" --format json --jq '.id')

# --------------------------------------------------------------------------
echo "==> fields"
have_field() {
  gh project field-list "$PNUM" --owner "$OWNER" -L 60 --format json \
    --jq "[.fields[] | select(.name==\"$1\")] | length"
}
ensure_select() {  # name  "opt,opt,opt"
  if [[ "$(have_field "$1")" == "0" ]]; then
    gh project field-create "$PNUM" --owner "$OWNER" --name "$1" \
      --data-type SINGLE_SELECT --single-select-options "$2" >/dev/null
    say "+ $1"
  else
    say "= $1"
  fi
}
ensure_select Priority "Critical,High,Medium,Low"
ensure_select Area "playback,ui,gpu,rmlui,native-decode,subtitles,audio,addons,network,memory,performance,app-module,ci,docs"

field_id() {
  gh project field-list "$PNUM" --owner "$OWNER" -L 60 --format json \
    --jq ".fields[] | select(.name==\"$1\") | .id"
}
option_id() {  # field-name  option-name  (case-insensitive)
  gh project field-list "$PNUM" --owner "$OWNER" -L 60 --format json \
    --jq ".fields[] | select(.name==\"$1\") | .options[]
          | select((.name|ascii_downcase)==(\"$2\"|ascii_downcase)) | .id"
}

PRIO_FID=$(field_id Priority)
AREA_FID=$(field_id Area)
STAT_FID=$(field_id Status)
STAT_TODO=$(option_id Status Todo)
STAT_DONE=$(option_id Status Done)

# --------------------------------------------------------------------------
echo "==> issues -> project items"
# number<TAB>state<TAB>url<TAB>label,label,...
mapfile -t ISSUES < <(gh issue list --repo "$OWNER/$REPO" --state all -L 400 \
  --json number,state,url,labels \
  --jq '.[] | [ (.number|tostring), .state, .url, ([.labels[].name] | join(",")) ] | @tsv')

existing_items() {
  gh project item-list "$PNUM" --owner "$OWNER" -L 400 --format json \
    --jq '.items[] | select(.content.number != null)
          | [ (.content.number|tostring), .id ] | @tsv'
}
declare -A ITEM
while IFS=$'\t' read -r n id; do ITEM["$n"]="$id"; done < <(existing_items)

added=0
for row in "${ISSUES[@]}"; do
  IFS=$'\t' read -r num state url labels <<<"$row"
  if [[ -z "${ITEM[$num]:-}" ]]; then
    gh project item-add "$PNUM" --owner "$OWNER" --url "$url" >/dev/null
    added=$((added+1))
  fi
done
say "added $added new item(s)"

# refresh the map (covers the just-added items)
unset ITEM; declare -A ITEM
while IFS=$'\t' read -r n id; do ITEM["$n"]="$id"; done < <(existing_items)

# --------------------------------------------------------------------------
echo "==> back-fill Priority / Area / Status"
set_select() {  # item-id  field-id  option-id
  [[ -n "$1" && -n "$2" && -n "$3" ]] || return 0
  gh project item-edit --project-id "$PID" --id "$1" --field-id "$2" \
    --single-select-option-id "$3" >/dev/null
}
pick_area() {  # comma,labels
  local L=",$1,"
  for a in $AREA_RANK; do
    if [[ "$L" == *",$a,"* ]]; then
      [[ "$a" == documentation ]] && echo docs || echo "$a"
      return
    fi
  done
}
pick_prio() {  # comma,labels
  case ",$1," in
    *",priority: critical,"*) echo Critical ;;
    *",priority: high,"*)     echo High ;;
    *",priority: medium,"*)   echo Medium ;;
    *",priority: low,"*)      echo Low ;;
  esac
}

n_done=0
for row in "${ISSUES[@]}"; do
  IFS=$'\t' read -r num state url labels <<<"$row"
  iid="${ITEM[$num]:-}"; [[ -n "$iid" ]] || { say "!! no item for #$num"; continue; }

  p=$(pick_prio "$labels")
  [[ -n "$p" ]] && set_select "$iid" "$PRIO_FID" "$(option_id Priority "$p")"

  ar=$(pick_area "$labels")
  [[ -n "$ar" ]] && set_select "$iid" "$AREA_FID" "$(option_id Area "$ar")"

  if [[ "$state" == "CLOSED" ]]; then set_select "$iid" "$STAT_FID" "$STAT_DONE"
  else                                set_select "$iid" "$STAT_FID" "$STAT_TODO"; fi
  n_done=$((n_done+1))
done
say "synced $n_done item(s)"

echo
echo "project:  https://github.com/users/$OWNER/projects/$PNUM"
echo "next:     docs/project-tracking.md  (views + auto-add workflow)"
