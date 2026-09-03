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
# Optional manual steps in the web UI (see docs/project-tracking.md):
#   - enable built-in workflows (Auto-add to project, Item closed -> Done)
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
echo "==> link repository"
REPO_ID=$(gh repo view "$OWNER/$REPO" --json id --jq .id 2>/dev/null || true)
if [[ -n "${REPO_ID:-}" ]]; then
  gh api graphql -f query="mutation { linkProjectV2ToRepository(input: { projectId: \"$PID\", repositoryId: \"$REPO_ID\" }) { repository { id } } }" >/dev/null 2>&1 || true
  say "linked to $OWNER/$REPO"
fi

# --------------------------------------------------------------------------
echo "==> fields"
have_field() {
  gh project field-list "$PNUM" --owner "$OWNER" -L 60 --format json \
    --jq "[.fields[] | select(.name==\"$1\")] | length" 2>/dev/null || echo "0"
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

# Cache all field and option IDs in memory to avoid repeated API queries
declare -A FIELD_ID
declare -A OPT_ID

load_fields_and_options() {
  local kind f1 f2 f3
  while IFS=$'\t' read -r kind f1 f2 f3; do
    if [[ "$kind" == "FIELD" ]]; then
      FIELD_ID["$f1"]="$f2"
    elif [[ "$kind" == "OPTION" ]]; then
      local key="${f1,,}:${f2,,}"
      OPT_ID["$key"]="$f3"
    fi
  done < <(gh project field-list "$PNUM" --owner "$OWNER" -L 60 --format json \
    --jq '(.fields[] | [ "FIELD", .name, .id ] | @tsv), (.fields[] | .name as $f | .options[]? | [ "OPTION", $f, .name, .id ] | @tsv)')
}
load_fields_and_options

# --------------------------------------------------------------------------
echo "==> views"
declare -A VIEW_ID VIEW_FILTER
load_views() {
  VIEW_ID=(); VIEW_FILTER=()
  local vname vid vlayout vfilt
  while IFS=$'\t' read -r vname vid vlayout vfilt; do
    [[ -n "$vname" ]] || continue
    VIEW_ID["$vname"]="$vid"
    VIEW_FILTER["$vname"]="$vfilt"
  done < <(gh api graphql -f query="query { user(login: \"$OWNER\") { projectV2(number: $PNUM) { views(first: 20) { nodes { name id layout filter } } } } }" \
    --jq '.data.user.projectV2.views.nodes[]? | [ .name, .id, .layout, (.filter // "") ] | @tsv' 2>/dev/null || true)
}
load_views

ensure_view() {  # name layout [filter]
  local vname="$1" layout="$2" filter="${3:-}"
  local vid="${VIEW_ID[$vname]:-}"

  if [[ -z "$vid" ]]; then
    local def_id="${VIEW_ID["View 1"]:-}"
    if [[ "$vname" == "Board" && -n "$def_id" ]]; then
      gh api graphql -f query="mutation { updateProjectV2View(input: { viewId: \"$def_id\", name: \"$vname\", layout: $layout }) { projectV2View { id } } }" >/dev/null
      say "= View 1 -> Board"
      VIEW_ID["Board"]="$def_id"
      unset 'VIEW_ID["View 1"]'
      return
    fi
    local new_vid
    new_vid=$(gh api graphql -f query="mutation { createProjectV2View(input: { projectId: \"$PID\", name: \"$vname\", layout: $layout }) { projectV2View { id } } }" --jq '.data.createProjectV2View.projectV2View.id // empty' 2>/dev/null || true)
    say "+ view: $vname"
    VIEW_ID["$vname"]="$new_vid"
    if [[ -n "$filter" && -n "$new_vid" ]]; then
      gh api graphql -f query="mutation { updateProjectV2View(input: { viewId: \"$new_vid\", filter: \"$filter\" }) { projectV2View { id } } }" >/dev/null 2>&1 || true
      VIEW_FILTER["$vname"]="$filter"
    fi
  else
    say "= view: $vname"
    if [[ -n "$filter" ]]; then
      local cur_filter="${VIEW_FILTER[$vname]:-}"
      if [[ "$cur_filter" != "$filter" ]]; then
        gh api graphql -f query="mutation { updateProjectV2View(input: { viewId: \"$vid\", filter: \"$filter\" }) { projectV2View { id } } }" >/dev/null 2>&1 || true
        VIEW_FILTER["$vname"]="$filter"
      fi
    fi
  fi
}

ensure_view "Board" "BOARD_LAYOUT"
ensure_view "By priority" "TABLE_LAYOUT"
ensure_view "Roadmap" "ROADMAP_LAYOUT"
ensure_view "Blocked" "TABLE_LAYOUT" "is:open -no:blocked-by"
ensure_view "Independent" "TABLE_LAYOUT" "label:independent is:open"

# --------------------------------------------------------------------------
echo "==> issues -> project items"
# number<TAB>state<TAB>url<TAB>label,label,...
mapfile -t ISSUES < <(gh issue list --repo "$OWNER/$REPO" --state all -L 400 \
  --json number,state,url,labels \
  --jq '.[] | [ (.number|tostring), .state, .url, ([.labels[].name] | join(",")) ] | @tsv')

existing_items() {
  gh project item-list "$PNUM" --owner "$OWNER" -L 400 --format json \
    --jq '.items[] | select(.content.number != null)
          | [ (.content.number|tostring), .id, (.priority // ""), (.area // ""), (.status // "") ] | @tsv' 2>/dev/null || true
}

declare -A ITEM_ID ITEM_PRIO ITEM_AREA ITEM_STAT
load_items() {
  ITEM_ID=(); ITEM_PRIO=(); ITEM_AREA=(); ITEM_STAT=()
  local num id cur_p cur_a cur_s
  while IFS=$'\t' read -r num id cur_p cur_a cur_s; do
    [[ -n "$num" ]] || continue
    ITEM_ID["$num"]="$id"
    ITEM_PRIO["$num"]="$cur_p"
    ITEM_AREA["$num"]="$cur_a"
    ITEM_STAT["$num"]="$cur_s"
  done < <(existing_items)
}
load_items

added=0
for row in "${ISSUES[@]}"; do
  IFS=$'\t' read -r num state url labels <<<"$row"
  if [[ -z "${ITEM_ID[$num]:-}" ]]; then
    gh project item-add "$PNUM" --owner "$OWNER" --url "$url" >/dev/null
    added=$((added+1))
    sleep 0.1
  fi
done
say "added $added new item(s)"

if [[ $added -gt 0 ]]; then
  load_items
fi

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
  iid="${ITEM_ID[$num]:-}"; [[ -n "$iid" ]] || { say "!! no item for #$num"; continue; }

  p=$(pick_prio "$labels")
  if [[ -n "$p" && "${ITEM_PRIO[$num]:-}" != "$p" ]]; then
    opt="${OPT_ID["priority:${p,,}"]:-}"
    fid="${FIELD_ID["Priority"]:-}"
    if [[ -n "$opt" && -n "$fid" ]]; then
      set_select "$iid" "$fid" "$opt"
      sleep 0.1
    fi
  fi

  ar=$(pick_area "$labels")
  if [[ -n "$ar" && "${ITEM_AREA[$num]:-}" != "$ar" ]]; then
    opt="${OPT_ID["area:${ar,,}"]:-}"
    fid="${FIELD_ID["Area"]:-}"
    if [[ -n "$opt" && -n "$fid" ]]; then
      set_select "$iid" "$fid" "$opt"
      sleep 0.1
    fi
  fi

  target_stat="Todo"
  [[ "$state" == "CLOSED" ]] && target_stat="Done"
  if [[ "${ITEM_STAT[$num]:-}" != "$target_stat" ]]; then
    opt="${OPT_ID["status:${target_stat,,}"]:-}"
    fid="${FIELD_ID["Status"]:-}"
    if [[ -n "$opt" && -n "$fid" ]]; then
      set_select "$iid" "$fid" "$opt"
      sleep 0.1
    fi
  fi

  n_done=$((n_done+1))
done
say "synced $n_done item(s)"

echo
echo "project:  https://github.com/users/$OWNER/projects/$PNUM"
echo "next:     docs/project-tracking.md  (views + auto-add workflow)"
