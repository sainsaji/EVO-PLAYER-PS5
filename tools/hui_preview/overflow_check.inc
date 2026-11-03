// overflow_check.inc - find text that does not fit, from a finished DrawList.
//
// Included by hui_preview.cpp. After each screen is recorded, the glyph quads
// in the list are grouped back into text runs and four things are checked:
//
//   CUT       a glyph is partly outside its clip rectangle (letters sliced)
//   OFFSCREEN ink past the 1920x1080 frame
//   SPILL     the run starts inside its smallest panel / pill / button but its
//             ink ends past that box's edge
//   OVERLAP   two separate runs drawn over each other
//
// Glyph quads carry SDF padding, so each is shrunk by its range to approximate
// the ink. The grouping is a heuristic (neighbouring glyphs of one colour on one
// baseline), so a finding is a place to look, not a verdict: every flagged
// screen also gets hui_<name>.overflow.png with the boxes drawn in red.
#pragma once

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace overflow
{

struct Box
{
    float x0, y0, x1, y1;
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
};

struct Finding
{
    const char *kind;
    Box box;      // the text's ink
    Box other;    // the clip, container or other run (empty for OFFSCREEN)
    float by;     // how far past, in pixels
};

struct TextRun
{
    Box ink;
    std::size_t first, last; // instance indices
    bool clipped;
    Box clip;
    bool cut;
    float cut_by;
    float range;    // SDF range of the first glyph: grows with the font size
    int slot;       // font atlas slot: which face (regular, semibold, ...)
    float rgba[4];  // colour of the first glyph
};

inline bool is_glyph(const hui::gfx::Instance &i)
{
    return static_cast<int>(i.params[3]) == static_cast<int>(hui::gfx::Shape::glyph);
}

inline Box ink_of(const hui::gfx::Instance &i)
{
    const float pad = std::min(std::max(i.params[0], 0.0f), 0.3f * std::min(i.rect[2], i.rect[3]));
    return {i.rect[0] + pad, i.rect[1] + pad, i.rect[0] + i.rect[2] - pad, i.rect[1] + i.rect[3] - pad};
}

// Glyph quads grouped back into text runs: consecutive, same colour, same
// line, close together. Invisible and wholly clipped glyphs are left out.
inline std::vector<TextRun> group_text(const hui::gfx::DrawList &list)
{
    const auto &inst = list.instances();

    // The clip of each instance, from the runs.
    std::vector<int> run_of(inst.size(), -1);
    const auto &runs = list.runs();
    for (std::size_t r = 0; r < runs.size(); ++r)
    {
        if (runs[r].mesh)
            continue;
        for (std::uint32_t k = 0; k < runs[r].count && runs[r].first + k < inst.size(); ++k)
            run_of[runs[r].first + k] = static_cast<int>(r);
    }

    // Group glyphs into runs: consecutive, same colour, same line, close together.
    std::vector<TextRun> text;
    for (std::size_t i = 0; i < inst.size(); ++i)
    {
        // Invisible text (faded out, or not yet in) cannot overflow anything.
        if (!is_glyph(inst[i]) || std::max(inst[i].color_top[3], inst[i].color_bottom[3]) < 0.08f)
            continue;
        const Box g = ink_of(inst[i]);
        const int r = run_of[i];
        const bool clipped = r >= 0 && runs[r].clipped;
        const Box clip = clipped ? Box{runs[r].clip.x, runs[r].clip.y, runs[r].clip.x + runs[r].clip.w,
                                       runs[r].clip.y + runs[r].clip.h}
                                 : Box{0, 0, 0, 0};
        // Wholly outside its clip: never drawn (the next lines of a scrolled text).
        if (clipped && (g.x1 <= clip.x0 || g.x0 >= clip.x1 || g.y1 <= clip.y0 || g.y0 >= clip.y1))
            continue;
        bool join = false;
        if (!text.empty() && text.back().last + 1 == i)
        {
            const auto &p = inst[i - 1];
            const Box pg = ink_of(p);
            const float lh = std::max(pg.h(), g.h());
            const bool same_colour = std::equal(p.color_top, p.color_top + 4, inst[i].color_top);
            const bool same_line = std::abs((pg.y0 + pg.y1) - (g.y0 + g.y1)) * 0.5f < 0.45f * lh + 2.0f;
            const bool close = g.x0 >= pg.x0 - 1.0f && g.x0 - pg.x1 < 1.2f * lh;
            join = same_colour && same_line && close;
        }
        if (!join)
            text.push_back({g, i, i, clipped, clip, false, 0.0f, inst[i].params[0], static_cast<int>(inst[i].params[2]),
                            {inst[i].color_top[0], inst[i].color_top[1], inst[i].color_top[2], inst[i].color_top[3]}});
        TextRun &t = text.back();
        t.last = i;
        t.ink.x0 = std::min(t.ink.x0, g.x0);
        t.ink.y0 = std::min(t.ink.y0, g.y0);
        t.ink.x1 = std::max(t.ink.x1, g.x1);
        t.ink.y1 = std::max(t.ink.y1, g.y1);
        if (clipped)
        {
            const bool inside_some = g.x1 > clip.x0 && g.x0 < clip.x1 && g.y1 > clip.y0 && g.y0 < clip.y1;
            const float past = std::max({clip.x0 - g.x0, g.x1 - clip.x1, clip.y0 - g.y0, g.y1 - clip.y1});
            if (inside_some && past > 2.0f)
            {
                t.cut = true;
                t.cut_by = std::max(t.cut_by, past);
                t.clip = clip;
            }
        }
    }
    return text;
}

inline std::vector<Finding> check(const hui::gfx::DrawList &list, float screen_w, float screen_h)
{
    const auto &inst = list.instances();
    std::vector<Finding> out;
    const std::vector<TextRun> text = group_text(list);

    for (const TextRun &t : text)
    {
        if (t.cut)
            out.push_back({"CUT", t.ink, t.clip, t.cut_by});
        const float off = std::max({-t.ink.x0, t.ink.x1 - screen_w, -t.ink.y0, t.ink.y1 - screen_h});
        if (off > 1.0f && t.ink.x1 > 0 && t.ink.x0 < screen_w && t.ink.y1 > 0 && t.ink.y0 < screen_h)
            out.push_back({"OFFSCREEN", t.ink, {0, 0, 0, 0}, off});

        // The topmost filled box (rect, chamfer, image or glass panel) drawn
        // before the run that holds its start: what the text sits on.
        const float sx = t.ink.x0 + 2.0f, sy = (t.ink.y0 + t.ink.y1) * 0.5f;
        const hui::gfx::Instance *best = nullptr;
        for (std::size_t k = t.first; k-- > 0;)
        {
            const auto &c = inst[k];
            const int shape = static_cast<int>(c.params[3]);
            if (shape != 0 && shape != 8 && shape != 3)
                continue;
            if (c.color_top[3] < 0.04f && c.color_bottom[3] < 0.04f && c.border_color[3] < 0.04f)
                continue;
            const float cw = c.rect[2], ch = c.rect[3];
            if (cw < 24.0f || ch < 16.0f || cw * ch > 0.5f * screen_w * screen_h)
                continue;
            if (sx < c.rect[0] || sx > c.rect[0] + cw || sy < c.rect[1] || sy > c.rect[1] + ch)
                continue;
            best = &c;
            break;
        }
        if (best)
        {
            const Box c{best->rect[0], best->rect[1], best->rect[0] + best->rect[2], best->rect[1] + best->rect[3]};
            const float past = std::max(t.ink.x1 - c.x1, t.ink.y1 - c.y1);
            if (past > 2.0f)
                out.push_back({"SPILL", t.ink, c, past});
        }
    }

    // Separate runs whose ink overlaps.
    for (std::size_t a = 0; a < text.size(); ++a)
        for (std::size_t b = a + 1; b < text.size(); ++b)
        {
            const Box &p = text[a].ink, &q = text[b].ink;
            const float ox = std::min(p.x1, q.x1) - std::max(p.x0, q.x0);
            const float oy = std::min(p.y1, q.y1) - std::max(p.y0, q.y0);
            // The same text a pixel or two apart is a drawn outline or shadow.
            const bool shadow = std::abs(p.w() - q.w()) < 3.0f && std::abs(p.h() - q.h()) < 3.0f &&
                                std::abs(p.x0 - q.x0) < 5.0f && std::abs(p.y0 - q.y0) < 5.0f;
            if (shadow || ox <= 3.0f || oy <= 3.0f || oy <= 0.3f * std::min(p.h(), q.h()))
                continue;
            // A panel drawn between the two that covers the spot hides the
            // first one (a dialog over the screen): not a clash.
            const float cx = (std::max(p.x0, q.x0) + std::min(p.x1, q.x1)) * 0.5f;
            const float cy = (std::max(p.y0, q.y0) + std::min(p.y1, q.y1)) * 0.5f;
            bool hidden = false;
            for (std::size_t k = text[a].last + 1; k < text[b].first && !hidden; ++k)
            {
                const auto &c = inst[k];
                const int shape = static_cast<int>(c.params[3]);
                if ((shape == 0 || shape == 3 || shape == 8) && std::max(c.color_top[3], c.color_bottom[3]) >= 0.5f &&
                    cx >= c.rect[0] && cx <= c.rect[0] + c.rect[2] && cy >= c.rect[1] && cy <= c.rect[1] + c.rect[3])
                    hidden = true;
            }
            if (!hidden)
                out.push_back({"OVERLAP", p, q, ox});
        }
    return out;
}

// Red outlines into the RGBA buffer read back from GL: rows bottom-up, which is
// how the preview writes its PNGs (the renderer draws the frame flipped).
inline void outline(std::vector<unsigned char> &px, int w, int h, const Box &b, unsigned char r,
                    unsigned char g, unsigned char bl)
{
    auto put = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= w || y >= h)
            return;
        unsigned char *p = &px[(static_cast<std::size_t>(h - 1 - y) * w + x) * 4];
        p[0] = r;
        p[1] = g;
        p[2] = bl;
        p[3] = 255;
    };
    const int x0 = static_cast<int>(b.x0) - 2, x1 = static_cast<int>(b.x1) + 2;
    const int y0 = static_cast<int>(b.y0) - 2, y1 = static_cast<int>(b.y1) + 2;
    for (int t = 0; t < 2; ++t)
    {
        for (int x = x0; x <= x1; ++x)
        {
            put(x, y0 + t);
            put(x, y1 - t);
        }
        for (int y = y0; y <= y1; ++y)
        {
            put(x0 + t, y);
            put(x1 - t, y);
        }
    }
}

} // namespace overflow
