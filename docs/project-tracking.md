# Project tracking — the "EVO Player Roadmap" board

A GitHub **Project (v2)** that mirrors every issue so the roadmap can be
tracked and visualised (board, priority table, milestone timeline, blocked
list). The dependency-ordered narrative still lives in
[roadmap.md](roadmap.md) — this doc is only about the board.

---

## First-time setup (once)

1. **Grant the `project` scope** to your local `gh` (opens a browser):

   ```sh
   gh auth refresh -s project -h github.com
   ```

2. **Build + populate the board:**

   ```sh
   ./scripts/setup-github-project.sh
   ```

   Creates the Project, adds the `Priority` and `Area` fields, adds **every**
   issue (open + closed) as an item, and back-fills:

   | Field | Filled from | Values |
   |---|---|---|
   | `Status` | issue state | `Todo` (open) · `Done` (closed) |
   | `Priority` | `priority: <x>` label | `Critical` · `High` · `Medium` · `Low` |
   | `Area` | most specific domain label | `native-decode` › `rmlui` › `gpu` › `subtitles` › `audio` › `playback` › `memory` › `performance` › `addons` › `network` › `ci` › `ui` › `docs` |

   The script is **idempotent** — re-run it any time (after re-labelling, after
   filing new issues) to resync the fields.

3. **Blocked-by / sub-issue relationships** need nothing — GitHub issue
   dependencies and sub-issues render on the board automatically. They're
   already wired for the open graph (see roadmap.md § Dependency graph).

---

## Manual bits the API can't do

### A. Views  (Project ▸ **+ New view**)

| View | Layout | Configure |
|---|---|---|
| **Board** | Board | Group by `Status`. The day-to-day kanban. |
| **By priority** | Table | Sort by `Priority` (asc), then `Area`. Field: show `Milestone`, `Labels`, `Linked pull requests`. |
| **Roadmap** | Roadmap | Group/marker by `Milestone`. Set the zoom to *Month*. Give it a real timeline by putting **due dates on the milestones** (`gh api -X PATCH repos/OWNER/REPO/milestones/N -f due_on=2026-10-01T00:00:00Z`). |
| **Blocked** | Table | Filter `is:open` and **Blocked by: has value** (or `-no:blocked-by`). Shows only what's waiting on something else. |
| **Independent** | Table | Filter `label:independent is:open` — the "start any time" pool. |

### B. Built-in workflows  (Project ▸ **•••** ▸ **Workflows**)

Turn on:

- **Auto-add to project** → filter `is:issue,pr is:open` (repo:
  `EVO-PLAYER-PS5`). New issues land on the board with no Action / token.
- **Item closed** → set `Status` = `Done`.
- **Item reopened** → set `Status` = `Todo`.
- **Auto-archive items** → `is:closed updated:<@today-2w` (keeps the board lean).

That covers "track automatically". The optional
`.github/workflows/add-to-project.yml` only matters for issues **transferred**
from another repo — activate it by setting the `ROADMAP_PROJECT_URL` variable
and `ADD_TO_PROJECT_PAT` secret (instructions in the file header).

---

## Keeping it in sync

| When | Do |
|---|---|
| New issue filed | built-in *Auto-add* handles membership; run `setup-github-project.sh` to fill `Priority`/`Area` (or set them by hand on the card) |
| Issue re-labelled (`priority:` / domain) | `./scripts/setup-github-project.sh` |
| Issue closed / reopened | built-in workflow flips `Status` |
| New milestone / due date | edit the milestone; the Roadmap view picks it up |

The board is a **view** of the issues, never the source of truth — labels,
milestones and the `<!-- rel -->` blocks in the issue bodies stay
authoritative.
