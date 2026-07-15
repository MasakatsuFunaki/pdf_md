#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "layout_internal.h"
#include "utf8.h"

namespace pdf2md {
namespace detail {

// ---------------------------------------------------------------- lists

struct ListMarker {
    bool ordered = false;
    std::u32string marker;        // normalized ("-" or "12.")
    size_t content_start = 0;     // char index where item content begins
    double marker_x = 0;          // left edge of the marker
    bool keep_marker_text = false;  // letter/roman markers stay in the text
};

bool is_roman_cp(char32_t cp) {
    switch (cp) {
        case U'i': case U'v': case U'x': case U'l': case U'c': case U'd': case U'm':
        case U'I': case U'V': case U'X': case U'L': case U'C': case U'D': case U'M':
            return true;
        default:
            return false;
    }
}

std::optional<ListMarker> detect_list_marker(const Line& line) {
    const auto& chars = line.chars;
    size_t i = 0;
    while (i < chars.size() && chars[i].space) ++i;
    if (i >= chars.size()) return std::nullopt;

    const double wgap = line.word_gap > 0 ? line.word_gap : 0.28 * line.size;
    auto followed_by_gap = [&](size_t idx) {
        // idx: last char of the marker. Requires more text after a space/gap.
        size_t j = idx + 1;
        if (j >= chars.size()) return false;
        if (chars[j].space) {
            while (j < chars.size() && chars[j].space) ++j;
            return j < chars.size();
        }
        return chars[j].left - chars[idx].right > wgap;
    };
    auto content_after = [&](size_t idx) {
        size_t j = idx + 1;
        while (j < chars.size() && chars[j].space) ++j;
        return j;
    };

    ListMarker m;
    m.marker_x = chars[i].left;

    // Bullet glyphs.
    if (is_bullet_cp(chars[i].code) || (is_dashy_cp(chars[i].code))) {
        if (!followed_by_gap(i)) return std::nullopt;
        m.ordered = false;
        m.marker = U"-";
        m.content_start = content_after(i);
        return m;
    }

    // Numbered: up to 3 digits then '.' or ')'.
    if (is_ascii_digit(chars[i].code)) {
        size_t j = i;
        std::u32string digits;
        while (j < chars.size() && is_ascii_digit(chars[j].code) && digits.size() < 4) {
            digits += chars[j].code;
            ++j;
        }
        if (digits.size() >= 1 && digits.size() <= 3 && j < chars.size() &&
            (chars[j].code == U'.' || chars[j].code == U')')) {
            // "3.5" is a decimal or section number, not a list marker.
            if (j + 1 < chars.size() && is_ascii_digit(chars[j + 1].code)) return std::nullopt;
            if (!followed_by_gap(j)) return std::nullopt;
            m.ordered = true;
            m.marker = digits + U".";
            m.content_start = content_after(j);
            return m;
        }
        return std::nullopt;
    }

    // Single letter "a." / "a)" or roman "iv." — kept in the text, rendered
    // as an unordered item since markdown has no letter lists.
    if (is_ascii_alpha(chars[i].code)) {
        size_t j = i;
        size_t len = 0;
        bool roman = true;
        while (j < chars.size() && is_ascii_alpha(chars[j].code) && len < 6) {
            roman = roman && is_roman_cp(chars[j].code);
            ++j;
            ++len;
        }
        bool ok = (len == 1 || (roman && len <= 6));
        if (ok && j < chars.size() &&
            chars[j].code == U')' &&  // require ')' for alpha markers: "e." etc. is too ambiguous
            followed_by_gap(j)) {
            m.ordered = false;
            m.marker = U"-";
            m.content_start = i;  // keep the "a)" in the item text
            m.keep_marker_text = true;
            return m;
        }
    }
    return std::nullopt;
}

// Rebuilds runs of a line starting at content_start (drops the marker).
void strip_marker(Line& line, size_t content_start) {
    if (content_start == 0) return;
    line.chars.erase(line.chars.begin(),
                     line.chars.begin() + static_cast<std::ptrdiff_t>(content_start));
    finalize_line(line);
    build_runs(line);
}

// A footnote/affiliation marker opening a line: a raised superscript symbol
// (*/†/‡/§/¶) or number, followed by body text on the normal baseline. The raised
// setting is what tells such a marker from a sentence that merely starts with a
// digit or an asterisk bullet.
struct FootnoteMarker {
    bool numbered = false;
    std::u32string label;     // "5" or U"†"
    size_t content_start = 0;  // char index where the note text begins
};

std::optional<FootnoteMarker> leading_footnote_marker(const Line& line) {
    const auto& chars = line.chars;
    size_t i = 0;
    while (i < chars.size() && chars[i].space) ++i;
    if (i >= chars.size()) return std::nullopt;
    if (script_role(line, chars[i]) != 1) return std::nullopt;  // must be a superscript

    auto is_fn_symbol = [](char32_t c) {
        return c == 0x2020 || c == 0x2021 || c == 0x00A7 || c == 0x00B6 || c == 0x2217 ||
               c == U'*';
    };
    FootnoteMarker m;
    size_t j = i;
    if (is_fn_symbol(chars[i].code)) {
        m.label = std::u32string(1, chars[i].code);
        j = i + 1;
    } else if (is_ascii_digit(chars[i].code)) {
        std::u32string digits;
        while (j < chars.size() && !chars[j].space && is_ascii_digit(chars[j].code) &&
               script_role(line, chars[j]) == 1 && digits.size() < 3) {
            digits += chars[j].code;
            ++j;
        }
        if (digits.empty()) return std::nullopt;
        m.numbered = true;
        m.label = digits;
    } else {
        return std::nullopt;
    }
    // The note text must follow, set as a word on the normal baseline; a bare
    // superscript or a scripted math token is not a footnote.
    size_t k = j;
    while (k < chars.size() && chars[k].space) ++k;
    if (k >= chars.size() || script_role(line, chars[k]) != 0 || !is_letter_cp(chars[k].code))
        return std::nullopt;
    m.content_start = k;
    return m;
}

// ---------------------------------------------------------------- blocks

struct RegionStats {
    double x0 = 0, x1 = 0;
    double median_gap = 0;
    double median_size = 11.0;
};

RegionStats compute_region_stats(const std::vector<Line>& lines) {
    RegionStats st;
    st.x0 = 1e30;
    st.x1 = -1e30;
    std::vector<double> sizes, gaps;
    for (size_t i = 0; i < lines.size(); ++i) {
        st.x0 = std::min(st.x0, lines[i].x0);
        st.x1 = std::max(st.x1, lines[i].x1);
        sizes.push_back(lines[i].size);
        if (i > 0) {
            double g = lines[i - 1].baseline - lines[i].baseline;
            if (g > 0.1) gaps.push_back(g);
        }
    }
    st.median_size = std::max(median(std::move(sizes)), 1.0);
    // Use only line-ish gaps for the baseline-spacing estimate.
    std::vector<double> line_gaps;
    for (double g : gaps)
        if (g < 3.0 * st.median_size) line_gaps.push_back(g);
    st.median_gap = median(line_gaps.empty() ? std::move(gaps) : std::move(line_gaps));
    if (st.median_gap <= 0) st.median_gap = 1.2 * st.median_size;
    return st;
}

void finalize_block_geometry(Block& b) {
    if (b.lines.empty()) return;
    b.x0 = 1e30;
    b.x1 = -1e30;
    b.y_top = -1e30;
    b.y_bottom = 1e30;
    std::vector<double> sizes;
    int bold = 0, total = 0;
    for (const Line& ln : b.lines) {
        if (ln.chars.empty()) continue;  // synthetic blank line in code blocks
        b.x0 = std::min(b.x0, ln.x0);
        b.x1 = std::max(b.x1, ln.x1);
        sizes.push_back(ln.size);
        for (const CharInfo& c : ln.chars) {
            if (c.space) continue;
            ++total;
            if (c.bold) ++bold;
            b.y_top = std::max(b.y_top, c.top);
            b.y_bottom = std::min(b.y_bottom, c.bottom);
        }
    }
    b.size = std::max(median(std::move(sizes)), 1.0);
    b.bold_majority = total > 0 && bold * 10 >= total * 7;
}

// ---------------------------------------------------------------- display math

size_t count_glyphs(const Line& line) {
    size_t n = 0;
    for (const CharInfo& c : line.chars)
        if (!c.space) ++n;
    return n;
}

// Merges "satellite" fragment lines — a numerator, a denominator, a big-operator
// limit, a stranded subscript — into the neighbouring text line they belong to.
// Baseline clustering strands these on their own short lines because they sit a
// fraction of a line above or below the text; pulling them back gives the math
// renderer every glyph of an expression with its true geometry, so the 2-D
// structure (\frac, \sqrt, operator limits) can be recovered downstream.
void absorb_math_fragments(std::vector<Line>& lines, double body_size) {
    if (body_size <= 0) return;
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t s = 0; s < lines.size() && !changed; ++s) {
            if (lines[s].mono_majority) continue;
            size_t ns = count_glyphs(lines[s]);
            long best = -1;
            double best_gap = 1e30;
            for (int d = -1; d <= 1; d += 2) {
                long h = static_cast<long>(s) + d;
                if (h < 0 || h >= static_cast<long>(lines.size())) continue;
                const Line& host = lines[static_cast<size_t>(h)];
                if (host.mono_majority) continue;
                size_t nh = count_glyphs(host);
                // The fragment is the shorter, small member of the pair.
                if (ns > nh || ns > std::max<size_t>(6, nh * 2 / 5)) continue;
                double gap = std::fabs(lines[s].baseline - host.baseline);
                if (gap >= 0.9 * body_size) continue;  // a real line, not a stack
                // A numerator/denominator/limit sits horizontally *inside* the
                // line it belongs to. Two ordinary short lines set side by side
                // (scattered figure labels) fail this and are left alone.
                if (lines[s].x0 < host.x0 - 0.5 * body_size ||
                    lines[s].x1 > host.x1 + 0.5 * body_size)
                    continue;
                if (gap < best_gap) { best_gap = gap; best = h; }
            }
            if (best < 0) continue;
            Line& host = lines[static_cast<size_t>(best)];
            for (const CharInfo& c : lines[s].chars) host.chars.push_back(c);
            finalize_line(host);
            build_runs(host);
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(s));
            changed = true;
        }
    }
}

// A display-equation line: set clear of the text column (centred/indented) with
// real math content, and either carrying a relation or short enough to be a
// standalone formula rather than an indented sentence.
bool is_display_equation_line(const Line& line, double col_left, double body_size) {
    if (line.mono_majority) return false;
    if (!line_needs_math(line)) return false;
    if (line.x0 - col_left < 4.0 * body_size) return false;
    // A formula carries a relation or a large operator/radical; a centred line
    // that merely ends in a footnote-mark superscript (an author affiliation)
    // does not, and must stay prose.
    bool strong = false;
    for (const CharInfo& c : line.chars) {
        if (c.space) continue;
        if (c.code == U'=' || c.code == U'<' || c.code == U'>' || c.code == 0x2248 ||
            c.code == 0x2264 || c.code == 0x2265 || c.code == 0x2260 || c.code == 0x2261 ||
            c.code == 0x221A || c.code == 0x2211 || c.code == 0x220F || c.code == 0x222B ||
            raised_origin_glyph(c))
            strong = true;
    }
    return strong;
}

// Pulls a trailing equation number "(n)" — hard against the right margin and
// well separated from the formula body — off the glyph list, returning its
// digits; `keep` is truncated to the formula glyphs.
std::u32string take_equation_tag(std::vector<const CharInfo*>& keep, double body_size) {
    auto prev_glyph = [&](long idx) {
        while (idx >= 0 && keep[static_cast<size_t>(idx)]->space) --idx;
        return idx;
    };
    long e = prev_glyph(static_cast<long>(keep.size()) - 1);
    if (e < 0 || keep[static_cast<size_t>(e)]->code != U')') return U"";
    long k = prev_glyph(e - 1);
    std::u32string digits;
    while (k >= 0 && keep[static_cast<size_t>(k)]->code >= U'0' &&
           keep[static_cast<size_t>(k)]->code <= U'9') {
        digits.insert(digits.begin(), keep[static_cast<size_t>(k)]->code);
        k = prev_glyph(k - 1);
    }
    if (digits.empty() || k < 0 || keep[static_cast<size_t>(k)]->code != U'(') return U"";
    long prev = prev_glyph(k - 1);
    if (prev < 0 ||
        keep[static_cast<size_t>(k)]->left - keep[static_cast<size_t>(prev)]->right <
            3.0 * body_size)
        return U"";
    keep.resize(static_cast<size_t>(k));  // drop the tag (and any trailing spaces)
    return digits;
}

// Builds a display-math block from region lines [first, last]; each visual line
// becomes one reconstructed LaTeX body (aligned when there is more than one).
Block make_display_block(const std::vector<Line>& lines, size_t first, size_t last, int page,
                         double body_size) {
    Block b;
    b.kind = BlockKind::MathDisplay;
    b.page = page;
    for (size_t k = first; k <= last; ++k) {
        // Keep the spaces: math_to_latex reads word breaks from them.
        std::vector<const CharInfo*> gl;
        for (const CharInfo& c : lines[k].chars) gl.push_back(&c);
        std::stable_sort(gl.begin(), gl.end(),
                         [](const CharInfo* a, const CharInfo* c) { return a->left < c->left; });
        std::u32string tag = take_equation_tag(gl, body_size);
        if (!tag.empty()) b.math_tag = tag;
        std::u32string latex = math_to_latex(gl, lines[k].baseline, true);
        if (!latex.empty()) b.math_lines.push_back(std::move(latex));
        b.lines.push_back(lines[k]);
    }
    finalize_block_geometry(b);
    return b;
}

// A line that opens a numbered bibliography entry with a bracketed citation
// label ("[12]", "[3]") at its very start. Such an entry is set with a hanging
// indent: the label sits at the left margin and every wrapped line is inset
// under the text. Its wrapped lines therefore continue the entry even across an
// internal sentence break (a citation runs "authors. Title. Venue, year." with
// full stops between fields), so the sentence-terminator paragraph split must
// not fire inside one.
bool is_citation_lead(const Line& ln) {
    const std::u32string& p = ln.plain;
    size_t i = 0;
    while (i < p.size() && p[i] == U' ') ++i;
    if (i >= p.size() || p[i] != U'[') return false;
    size_t d = ++i;
    while (i < p.size() && is_ascii_digit(p[i])) ++i;
    return i > d && i < p.size() && p[i] == U']';
}

// Groups region lines into blocks (paragraphs, list items, code).
void region_to_blocks(std::vector<Line> lines, int page, const AnalyzeParams& params,
                      double col_left, double body_size, double page_width,
                      std::vector<Block>& out) {
    if (lines.empty()) return;
    // Reunite stacked math fragments before anything reads the line geometry.
    absorb_math_fragments(lines, body_size);
    RegionStats st = compute_region_stats(lines);

    // Table detection first: consume runs of aligned multi-segment lines.
    std::vector<char> consumed(lines.size(), 0);
    std::vector<std::pair<size_t, TableCandidate>> tables;
    if (params.detect_tables) {
        size_t i = 0;
        while (i < lines.size()) {
            if (lines[i].segments.size() >= 2 && !lines[i].mono_majority) {
                if (auto t = try_table(lines, i, lines.size(), /*render=*/true, page_width)) {
                    for (size_t k = t->first_line; k <= t->last_line; ++k) consumed[k] = 1;
                    size_t next = t->last_line + 1;
                    tables.emplace_back(i, std::move(*t));
                    i = next;
                    continue;
                }
            }
            ++i;
        }
    }

    // Display equations: runs of consecutive centred math lines become one
    // reconstructed $$ block (aligned when several lines make one formula).
    std::vector<std::pair<size_t, Block>> displays;
    {
        size_t i = 0;
        while (i < lines.size()) {
            if (consumed[i] || !is_display_equation_line(lines[i], col_left, body_size)) {
                ++i;
                continue;
            }
            auto starts_lower = [](const Line& ln) {
                for (const CharInfo& c : ln.chars)
                    if (!c.space) return is_ascii_lower(c.code);
                return false;
            };
            size_t j = i;
            // Extend into a following line only when it is a continuation clause
            // of the same formula (an aligned "where ..." line, set lowercase);
            // a line that opens with its own left-hand side (`PE...=`) is a
            // separate equation and gets its own block.
            while (j + 1 < lines.size() && !consumed[j + 1] &&
                   is_display_equation_line(lines[j + 1], col_left, body_size) &&
                   lines[j].baseline - lines[j + 1].baseline < 2.0 * st.median_size &&
                   starts_lower(lines[j + 1])) {
                ++j;
            }
            for (size_t k = i; k <= j; ++k) consumed[k] = 1;
            displays.emplace_back(i, make_display_block(lines, i, j, page, body_size));
            i = j + 1;
        }
    }

    std::vector<Block> blocks;
    Block* cur = nullptr;
    // Facts about the previous line, captured before it is moved into a block.
    struct PrevLine {
        bool valid = false;
        double baseline = 0, size = 0, x1 = 0;
        char32_t last_cp = U'\0';
    } prev;
    auto remember = [&](const Line& ln) {
        prev.valid = true;
        prev.baseline = ln.baseline;
        prev.size = ln.size;
        prev.x1 = ln.x1;
        prev.last_cp = ln.plain.empty() ? U'\0' : ln.plain.back();
    };
    double list_content_x = 0;

    auto close = [&]() { cur = nullptr; };
    auto start_block = [&](BlockKind kind) -> Block& {
        blocks.emplace_back();
        cur = &blocks.back();
        cur->kind = kind;
        cur->page = page;
        return *cur;
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        if (consumed[i]) {
            // Emit the table exactly once, at its first line.
            for (auto& [first, tc] : tables) {
                if (tc.first_line == i) {
                    Block& b = start_block(BlockKind::Table);
                    b.table_cells = std::move(tc.cells);
                    // Geometry from covered lines.
                    for (size_t k = tc.first_line; k <= tc.last_line; ++k)
                        b.lines.push_back(lines[k]);
                    finalize_block_geometry(b);
                    b.lines.clear();
                    close();
                    // A split class-style box trails one or more nested grids;
                    // each renders as its own GFM table below the key/value head.
                    for (auto& grid : tc.extra) {
                        Block& e = start_block(BlockKind::Table);
                        e.table_cells = std::move(grid);
                        e.page = page;
                        close();
                    }
                }
            }
            // Emit each display equation once, at its first line.
            for (auto& [first, db] : displays) {
                if (first == i) {
                    blocks.push_back(db);
                    close();
                }
            }
            remember(lines[i]);
            continue;
        }

        Line& ln = lines[i];
        auto marker = detect_list_marker(ln);
        // A lone dash opening an indented line that continues a hanging-indent
        // reference entry is that entry's wrapped clause ("...(CAN)" / "– Part 2:
        // ..."), not a new list item: the dash is a phrase separator carried to the
        // wrap. Veto the marker so the line rejoins the entry rather than starting a
        // list. Keyed on the citation hanging-indent structure (a bracketed lead, a
        // dash glyph, the line inset under the entry text, the entry unterminated).
        if (marker && !marker->ordered && cur && cur->kind == BlockKind::Paragraph &&
            !cur->lines.empty() && is_citation_lead(cur->lines.front()) && prev.valid) {
            char32_t lead = 0;
            for (const CharInfo& c : ln.chars)
                if (!c.space) { lead = c.code; break; }
            bool prev_unterminated =
                prev.last_cp != U'.' && prev.last_cp != U'!' && prev.last_cp != U'?';
            if (is_dashy_cp(lead) && prev_unterminated &&
                ln.x0 > cur->x0 + 1.2 * st.median_size)
                marker = std::nullopt;
        }
        auto footnote = leading_footnote_marker(ln);
        bool is_mono = ln.mono_majority;

        bool new_block = cur == nullptr;
        if (!new_block && prev.valid) {
            double gap = prev.baseline - ln.baseline;
            double size_delta = std::fabs(ln.size - prev.size);
            // A line is "short" (ends its paragraph) relative to the right
            // edge of its own block, not the whole region: a wide sibling
            // block (title, table) must not inflate the reference edge.
            double right_edge = std::max(cur->x1, ln.x1);
            bool prev_continues =
                prev.last_cp == U'-' || prev.last_cp == 0x2010 || prev.last_cp == 0x00AD;
            bool prev_short = !prev_continues &&
                              prev.x1 < right_edge - std::max(2.5 * st.median_size,
                                                              0.12 * (st.x1 - st.x0));
            bool indented =
                !cur->lines.empty() &&
                ln.x0 > (cur->kind == BlockKind::ListItem ? list_content_x : cur->x0) +
                            1.2 * st.median_size;
            // Hanging-indent continuation: in a reference or footnote list the
            // marker line sits at the region's left edge and each wrapped line is
            // indented under the text. Such an indented line is a continuation, not
            // a new paragraph, unless the line above actually ended one with a
            // sentence terminator -- so a short first line before an over-long token
            // (a URL, an arXiv id) still keeps its entry, and its hyphenation, whole.
            // The same rule keeps a centred short last line (a display note) with its
            // block instead of reading its inset as an indent. Blank-line paragraph
            // breaks are unaffected: the vertical-gap test above already split them.
            // Only a genuine sentence terminator ends the entry. A colon or
            // semicolon is common mid-title in a citation ("Context Threading:"),
            // so it must not break a hanging-indent reference.
            bool prev_ends_sentence =
                prev.last_cp == U'.' || prev.last_cp == U'!' || prev.last_cp == U'?';
            // A numbered bibliography entry keeps its internal full stops between
            // fields (authors. title. venue.), so a sentence terminator there is
            // never an entry break -- only a new label or a blank-line gap ends it.
            bool citation_entry = !cur->lines.empty() && is_citation_lead(cur->lines.front());
            // `indented` already means the line sits right of the block's outdented
            // first line -- the hanging-indent signature. The wrap only breaks the
            // entry when the line above closed a sentence, unless this is a bracketed
            // citation whose fields are punctuated internally.
            bool hanging_cont = indented && (citation_entry || !prev_ends_sentence);
            if (marker) new_block = true;
            else if (footnote) new_block = true;
            else if (is_mono != (cur->kind == BlockKind::Code)) new_block = true;
            else if (size_delta > 0.15 * std::max(ln.size, prev.size)) new_block = true;
            else if (gap > 1.45 * st.median_gap + 0.01 || gap > 2.1 * std::max(ln.size, prev.size))
                new_block = true;
            else if (cur->kind != BlockKind::Code && prev_short && !hanging_cont &&
                     !(cur->kind == BlockKind::ListItem && std::fabs(ln.x0 - list_content_x) < 1.0 * st.median_size))
                new_block = true;
            else if (cur->kind != BlockKind::Code && indented && !hanging_cont) new_block = true;
            else if (cur->kind == BlockKind::ListItem &&
                     ln.x0 < list_content_x - 1.2 * st.median_size)
                new_block = true;
        }

        if (new_block) {
            if (footnote) {
                Block& b = start_block(BlockKind::Paragraph);
                b.is_footnote = true;
                b.footnote_numbered = footnote->numbered;
                b.footnote_label = footnote->label;
                strip_marker(ln, footnote->content_start);
                if (ln.plain.empty()) {
                    blocks.pop_back();
                    close();
                    remember(lines[i]);
                    continue;
                }
            } else if (marker) {
                Block& b = start_block(BlockKind::ListItem);
                b.list_ordered = marker->ordered;
                b.list_marker = marker->marker;
                b.x0 = marker->marker_x;  // marker position defines nesting
                if (!marker->keep_marker_text) strip_marker(ln, marker->content_start);
                list_content_x = ln.chars.empty() ? marker->marker_x : ln.x0;
                if (ln.plain.empty()) {
                    blocks.pop_back();
                    close();
                    remember(lines[i]);
                    continue;
                }
            } else if (is_mono) {
                start_block(BlockKind::Code);
            } else {
                start_block(BlockKind::Paragraph);
            }
        } else if (cur->kind == BlockKind::Code && prev.valid) {
            // Preserve blank lines inside code blocks.
            double gap = prev.baseline - ln.baseline;
            int blanks = static_cast<int>(std::lround(gap / st.median_gap)) - 1;
            for (int k = 0; k < std::min(blanks, 4); ++k) cur->lines.emplace_back();
        }

        double marker_x = cur->x0;  // keep list marker x, set before strip
        remember(ln);
        cur->lines.push_back(std::move(ln));
        finalize_block_geometry(*cur);
        if (cur->kind == BlockKind::ListItem) cur->x0 = std::min(marker_x, cur->x0);
    }

    // Merge adjacent code blocks separated by short gaps (blank code lines).
    // Assign list nesting levels from marker x positions within this region.
    std::vector<double> marker_xs;
    for (const Block& b : blocks)
        if (b.kind == BlockKind::ListItem) marker_xs.push_back(b.x0);
    if (!marker_xs.empty()) {
        std::sort(marker_xs.begin(), marker_xs.end());
        std::vector<double> levels;
        for (double x : marker_xs) {
            if (levels.empty() || x > levels.back() + 0.9 * st.median_size) levels.push_back(x);
        }
        for (Block& b : blocks) {
            if (b.kind != BlockKind::ListItem) continue;
            size_t lvl = 0;
            for (size_t k = 0; k < levels.size(); ++k) {
                if (std::fabs(b.x0 - levels[k]) <= 0.9 * st.median_size + 1e-6) {
                    lvl = k;
                    break;
                }
            }
            b.list_indent = static_cast<int>(lvl);
        }
    }

    for (auto& b : blocks) out.push_back(std::move(b));
}

}  // namespace detail
}  // namespace pdf2md
