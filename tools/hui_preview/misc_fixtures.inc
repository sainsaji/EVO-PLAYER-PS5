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

static void fill_changelog_fixture(evo_rmlui_changelog_params_t& p)
{
    p.title = "What's New";
    p.subtitle = "Recent updates to EVO Player";
    
    p.release_count = 4;
    p.release_total = 4;
    
    p.releases[0].version = "1.2.0";
    p.releases[0].tagline = "The Surround Update";
    p.releases[0].date = "Oct 8, 2026";
    p.releases[0].is_focused = 1;
    
    p.releases[1].version = "1.1.5";
    p.releases[1].tagline = "Fixes and stability";
    p.releases[1].date = "Sep 15, 2026";
    p.releases[1].is_focused = 0;

    p.releases[2].version = "1.1.0";
    p.releases[2].tagline = "Network shares";
    p.releases[2].date = "Aug 20, 2026";
    p.releases[2].is_focused = 0;

    p.releases[3].version = "1.0.0";
    p.releases[3].tagline = "Initial Release";
    p.releases[3].date = "Jul 1, 2026";
    p.releases[3].is_focused = 0;

    p.detail_version = "Version 1.2.0";
    p.detail_tagline = "The Surround Update";
    
    p.item_count = 8;
    p.item_total = 8;
    
    p.items[0].kind = "NEW";
    p.items[0].text = "Added support for 5.1 and 7.1 surround sound audio tracks.";
    
    p.items[1].kind = "NEW";
    p.items[1].text = "Added an interactive surround sound calibration tool.";
    
    p.items[2].kind = "IMPROVED";
    p.items[2].text = "Subtitle rendering performance is up to 30% faster.";
    
    p.items[3].kind = "FIXED";
    p.items[3].text = "Resolved an issue where some MKV files would not show chapters.";
    
    p.items[4].kind = "FIXED";
    p.items[4].text = "Fixed audio sync drift when pausing for long periods.";
    
    p.items[5].kind = "IMPROVED";
    p.items[5].text = "Better error handling for unsupported HDR formats.";
    
    p.items[6].kind = "REMOVED";
    p.items[6].text = "Removed the old ASCII-based subtitle renderer.";
    
    p.items[7].kind = "FIXED";
    p.items[7].text = "Fixed a memory leak when browsing very large directories.";
}
