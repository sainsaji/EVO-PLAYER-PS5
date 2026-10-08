#include "../../projects/evoplayer/include/evo_changelog.h"
#include "evo_rmlui_bridge.h"

static void fill_dialog_fixture(evo_rmlui_dialog_params_t& p)
{
    p.eyebrow = "PLAYBACK";
    p.title = "Stop playback?";
    p.detail = "Are you sure you want to stop watching and return to the menu?";
    p.progress_pct = 0.45;
    p.action_count = 2;
    p.focused_action = 1;
    
    p.actions[0].icon_path = "";
    p.actions[0].label = "KEEP WATCHING";
    p.actions[0].is_primary = 1;
    
    p.actions[1].icon_path = "";
    p.actions[1].label = "STOP";
    p.actions[1].is_primary = 0;
}

static void fill_toast_fixture(evo_rmlui_toast_params_t& p)
{
    p.title = "USB Drive Removed";
    p.message = "The media drive was safely disconnected.";
    p.kind = 0;
    p.visible = 1;
    p.alpha = 255;
    p.slide = 0;
}

static void fill_about_fixture(evo_rmlui_about_params_t& p)
{
    p.app_name = "EVO PLAYER";
    p.version = "v1.2.0-beta";
    p.build_tag = "NIGHTLY";
    p.tagline = "CINEMATIC MEDIA PLAYER FOR PLAYSTATION 5 HOMEBREW";
    p.themes_info = "3 AVAILABLE";
    p.action_focused = 1;
}

// Built from the real in-app changelog (the same fill as ChangelogScreen.cpp),
// so the preview shows exactly what the console draws, overflow included.
static void fill_changelog_fixture(evo_rmlui_changelog_params_t& p)
{
    static const char* kindLabel[] = {"NEW", "FIXED", "IMPROVED", "REMOVED", "VERSION"};
    p.title = "CHANGELOG";
    p.subtitle = "WHAT CHANGED IN EACH RELEASE";
    p.release_total = EVO_CHANGELOG_RELEASE_COUNT;
    p.release_count = 0;
    for (int i = 0; i < EVO_RMLUI_CL_RELEASES && i < EVO_CHANGELOG_RELEASE_COUNT; ++i)
    {
        p.releases[i].version = EVO_CHANGELOG_RELEASES[i].version;
        p.releases[i].tagline = EVO_CHANGELOG_RELEASES[i].tagline;
        p.releases[i].date = EVO_CHANGELOG_RELEASES[i].date;
        p.releases[i].is_focused = i == 0;
        p.release_count++;
    }
    const auto& r = EVO_CHANGELOG_RELEASES[0];
    p.detail_version = r.version;
    p.detail_tagline = r.tagline;
    p.item_total = r.item_count;
    p.item_count = 0;
    for (int i = 0; i < r.item_count && i < EVO_RMLUI_CL_ITEMS; ++i)
    {
        int kind = static_cast<int>(r.items[i].kind);
        p.items[i].kind = kindLabel[(kind < 0 || kind > 4) ? 0 : kind];
        p.items[i].text = r.items[i].text;
        p.item_count++;
    }
}
