#include "layout_internal.h"

#include "utf8.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace pdf2md {
namespace detail {

namespace {

// Hyphen and footnote symbol code points.
constexpr char32_t kSoftHyphenCp = 0x00AD;
constexpr char32_t kSectionSignCp = 0x00A7;
constexpr char32_t kPilcrowCp = 0x00B6;
constexpr char32_t kUnicodeHyphenCp = 0x2010;
constexpr char32_t kDaggerCp = 0x2020;
constexpr char32_t kDoubleDaggerCp = 0x2021;
constexpr char32_t kAsteriskOperatorCp = 0x2217;

// List and footnote marker limits.
constexpr size_t kMaxOrderedMarkerDigits = 3;
constexpr size_t kMaxRomanMarkerLen = 6;
constexpr size_t kMaxFootnoteDigits = 3;

// Region baseline and block-splitting ratios (scaled by font/body size).
constexpr double kMinLineBaselineDeltaPt = 0.1;
constexpr double kMaxIntraParagraphGapScale = 3.0;
constexpr double kDefaultLeadingScale = 1.2;
constexpr int kBoldMajorityNumerator = 7;
constexpr int kBoldMajorityDenominator = 10;

// Math fragment and display-equation thresholds.
constexpr size_t kMinFragmentGlyphCap = 6;
constexpr size_t kFragmentHostRatioNum = 2;
constexpr size_t kFragmentHostRatioDen = 5;
constexpr double kFragmentMaxLeadingScale = 0.9;
constexpr double kFragmentHorizPadScale = 0.5;
constexpr double kDisplayEqMinIndentScale = 4.0;
constexpr double kEquationTagMinGapScale = 3.0;
constexpr double kDisplayContMaxGapScale = 2.0;

// Paragraph, hanging-indent, and list-nesting thresholds.
constexpr double kShortLineMarginScale = 2.5;
constexpr double kShortLineRegionWidthScale = 0.12;
constexpr double kParagraphIndentScale = 1.2;
constexpr double kFontSizeBreakRatio = 0.15;
constexpr double kParagraphGapLeadingScale = 1.45;
constexpr double kParagraphGapEpsilonPt = 0.01;
constexpr double kParagraphHardGapScale = 2.1;
constexpr double kListContinuationAlignScale = 1.0;
constexpr int kMaxBlankCodeLines = 4;
constexpr double kListNestingStepScale = 0.9;
constexpr double kListNestingEpsilonPt = 1e-6;

struct ListMarker {
    bool ordered = false;
    std::u32string marker;          // normalized ("-" or "12.")
    size_t content_start = 0;       // char index where item content begins
    double marker_x = 0;            // left edge of the marker
    bool keep_marker_text = false;  // letter/roman markers stay in the text
};

constexpr bool is_roman_cp(char32_t cp) noexcept {
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
    while (i < chars.size() && chars[i].space) {
        ++i;
    }
    if (i >= chars.size()) {
        return std::nullopt;
    }

    const double wgap = line.word_gap > 0 ? line.word_gap : kDefaultWordGapScale * line.size;
    auto followed_by_gap = [&chars, wgap](size_t idx) {
        size_t j = idx + 1;
        if (j >= chars.size()) {
            return false;
        }
        if (chars[j].space) {
            while (j < chars.size() && chars[j].space) {
                ++j;
            }
            return j < chars.size();
        }
        return chars[j].left - chars[idx].right > wgap;
    };
    auto content_after = [&chars](size_t idx) {
        size_t j = idx + 1;
        while (j < chars.size() && chars[j].space) {
            ++j;
        }
        return j;
    };

    ListMarker m;
    m.marker_x = chars[i].left;

    if (is_bullet_cp(chars[i].code) || is_dashy_cp(chars[i].code)) {
        if (!followed_by_gap(i)) {
            return std::nullopt;
        }
        m.ordered = false;
        m.marker = U"-";
        m.content_start = content_after(i);
        return m;
    }

    if (is_ascii_digit(chars[i].code)) {
        size_t j = i;
        std::u32string digits;
        while (j < chars.size() && is_ascii_digit(chars[j].code) &&
               digits.size() <= kMaxOrderedMarkerDigits) {
            digits += chars[j].code;
            ++j;
        }
        if (!digits.empty() && digits.size() <= kMaxOrderedMarkerDigits && j < chars.size() &&
            (chars[j].code == U'.' || chars[j].code == U')')) {
            if (j + 1 < chars.size() && is_ascii_digit(chars[j + 1].code)) {
                return std::nullopt;
            }
            if (!followed_by_gap(j)) {
                return std::nullopt;
            }
            m.ordered = true;
            m.marker = std::move(digits);
            m.marker += U'.';
            m.content_start = content_after(j);
            return m;
        }
        return std::nullopt;
    }

    if (is_ascii_alpha(chars[i].code)) {
        size_t j = i;
        size_t len = 0;
        bool roman = true;
        while (j < chars.size() && is_ascii_alpha(chars[j].code) && len < kMaxRomanMarkerLen) {
            roman = roman && is_roman_cp(chars[j].code);
            ++j;
            ++len;
        }
        const bool ok = len == 1 || (roman && len <= kMaxRomanMarkerLen);
        if (ok && j < chars.size() && chars[j].code == U')' && followed_by_gap(j)) {
            m.ordered = false;
            m.marker = U"-";
            m.content_start = i;
            m.keep_marker_text = true;
            return m;
        }
    }
    return std::nullopt;
}

void strip_marker(Line& line, size_t content_start) {
    if (content_start == 0) {
        return;
    }
    const size_t n = std::min(content_start, line.chars.size());
    line.chars.erase(line.chars.begin(),
                     line.chars.begin() + static_cast<std::ptrdiff_t>(n));
    finalize_line(line);
    build_runs(line);
}

struct FootnoteMarker {
    bool numbered = false;
    std::u32string label;     // "5" or U"†"
    size_t content_start = 0;
};

std::optional<FootnoteMarker> leading_footnote_marker(const Line& line) {
    const auto& chars = line.chars;
    size_t i = 0;
    while (i < chars.size() && chars[i].space) {
        ++i;
    }
    if (i >= chars.size()) {
        return std::nullopt;
    }
    if (script_role(line, chars[i]) != 1) {
        return std::nullopt;
    }

    auto is_fn_symbol = [](char32_t c) {
        return c == kDaggerCp || c == kDoubleDaggerCp || c == kSectionSignCp ||
               c == kPilcrowCp || c == kAsteriskOperatorCp || c == U'*';
    };
    FootnoteMarker m;
    size_t j = i;
    if (is_fn_symbol(chars[i].code)) {
        m.label = std::u32string(1, chars[i].code);
        j = i + 1;
    } else if (is_ascii_digit(chars[i].code)) {
        std::u32string digits;
        while (j < chars.size() && !chars[j].space && is_ascii_digit(chars[j].code) &&
               script_role(line, chars[j]) == 1 && digits.size() < kMaxFootnoteDigits) {
            digits += chars[j].code;
            ++j;
        }
        if (digits.empty()) {
            return std::nullopt;
        }
        m.numbered = true;
        m.label = std::move(digits);
    } else {
        return std::nullopt;
    }
    size_t k = j;
    while (k < chars.size() && chars[k].space) {
        ++k;
    }
    if (k >= chars.size() || script_role(line, chars[k]) != 0 || !is_letter_cp(chars[k].code)) {
        return std::nullopt;
    }
    m.content_start = k;
    return m;
}

struct RegionStats {
    double x0 = 0;
    double x1 = 0;
    double median_gap = 0;
    double median_size = kDefaultBodySizePt;
};

RegionStats compute_region_stats(std::span<const Line> lines) {
    RegionStats st;
    st.x0 = kCoordSentinel;
    st.x1 = -kCoordSentinel;
    std::vector<double> sizes;
    std::vector<double> gaps;
    sizes.reserve(lines.size());
    gaps.reserve(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        st.x0 = std::min(st.x0, lines[i].x0);
        st.x1 = std::max(st.x1, lines[i].x1);
        sizes.push_back(lines[i].size);
        if (i > 0) {
            const double g = lines[i - 1].baseline - lines[i].baseline;
            if (g > kMinLineBaselineDeltaPt) {
                gaps.push_back(g);
            }
        }
    }
    st.median_size = std::max(median(std::move(sizes)), kMinFontSizePt);
    std::vector<double> line_gaps;
    line_gaps.reserve(gaps.size());
    for (double g : gaps) {
        if (g < kMaxIntraParagraphGapScale * st.median_size) {
            line_gaps.push_back(g);
        }
    }
    st.median_gap = median(line_gaps.empty() ? std::move(gaps) : std::move(line_gaps));
    if (st.median_gap <= 0) {
        st.median_gap = kDefaultLeadingScale * st.median_size;
    }
    return st;
}

// Merges "satellite" fragment lines — numerators, denominators, operator limits,
// stranded subscripts — into the neighbouring text line they belong to.
void absorb_math_fragments(std::vector<Line>& lines, double body_size) {
    if (body_size <= 0) {
        return;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t s = 0; s < lines.size() && !changed; ++s) {
            if (lines[s].mono_majority) {
                continue;
            }
            const size_t ns = count_glyphs(lines[s]);
            std::ptrdiff_t best = -1;
            double best_gap = kCoordSentinel;
            for (int d = -1; d <= 1; d += 2) {
                const std::ptrdiff_t h = static_cast<std::ptrdiff_t>(s) + d;
                if (h < 0 || h >= static_cast<std::ptrdiff_t>(lines.size())) {
                    continue;
                }
                const Line& host = lines[static_cast<size_t>(h)];
                if (host.mono_majority) {
                    continue;
                }
                const size_t nh = count_glyphs(host);
                const size_t max_frag =
                    std::max(kMinFragmentGlyphCap, nh * kFragmentHostRatioNum / kFragmentHostRatioDen);
                if (ns > nh || ns > max_frag) {
                    continue;
                }
                const double gap = std::fabs(lines[s].baseline - host.baseline);
                const double leading = std::min(body_size, host.size);
                if (gap >= kFragmentMaxLeadingScale * leading) {
                    continue;
                }
                if (lines[s].x0 < host.x0 - kFragmentHorizPadScale * body_size ||
                    lines[s].x1 > host.x1 + kFragmentHorizPadScale * body_size) {
                    continue;
                }
                if (gap < best_gap) {
                    best_gap = gap;
                    best = h;
                }
            }
            if (best < 0) {
                continue;
            }
            Line& host = lines[static_cast<size_t>(best)];
            host.chars.insert(host.chars.end(), lines[s].chars.cbegin(), lines[s].chars.cend());
            finalize_line(host);
            build_runs(host);
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(s));
            changed = true;
        }
    }
}

bool is_display_equation_line(const Line& line, double col_left, double body_size) {
    if (line.mono_majority) {
        return false;
    }
    if (!line_needs_math(line)) {
        return false;
    }
    if (line.x0 - col_left < kDisplayEqMinIndentScale * body_size) {
        return false;
    }
    for (const CharInfo& c : line.chars) {
        if (c.space) {
            continue;
        }
        if (c.code == U'=' || c.code == U'<' || c.code == U'>' || c.code == 0x2248 ||
            c.code == 0x2264 || c.code == 0x2265 || c.code == 0x2260 || c.code == 0x2261 ||
            c.code == 0x221A || c.code == 0x2211 || c.code == 0x220F || c.code == 0x222B ||
            raised_origin_glyph(c)) {
            return true;
        }
    }
    return false;
}

std::u32string take_equation_tag(std::vector<const CharInfo*>& keep, double body_size) {
    auto prev_glyph = [&keep](std::ptrdiff_t idx) {
        while (idx >= 0 && keep[static_cast<size_t>(idx)]->space) {
            --idx;
        }
        return idx;
    };
    const std::ptrdiff_t e = prev_glyph(static_cast<std::ptrdiff_t>(keep.size()) - 1);
    if (e < 0 || keep[static_cast<size_t>(e)]->code != U')') {
        return {};
    }
    std::ptrdiff_t k = prev_glyph(e - 1);
    std::u32string digits;
    while (k >= 0 && keep[static_cast<size_t>(k)]->code >= U'0' &&
           keep[static_cast<size_t>(k)]->code <= U'9') {
        digits.push_back(keep[static_cast<size_t>(k)]->code);
        k = prev_glyph(k - 1);
    }
    if (digits.empty() || k < 0 || keep[static_cast<size_t>(k)]->code != U'(') {
        return {};
    }
    const std::ptrdiff_t prev = prev_glyph(k - 1);
    if (prev < 0 || keep[static_cast<size_t>(k)]->left - keep[static_cast<size_t>(prev)]->right <
                        kEquationTagMinGapScale * body_size) {
        return {};
    }
    std::reverse(digits.begin(), digits.end());
    keep.resize(static_cast<size_t>(k));
    return digits;
}

Block make_display_block(std::span<const Line> lines, size_t first, size_t last, int page,
                         double body_size) {
    Block b;
    b.kind = BlockKind::MathDisplay;
    b.page = page;
    b.lines.reserve(last - first + 1);
    for (size_t k = first; k <= last; ++k) {
        std::vector<const CharInfo*> gl;
        gl.reserve(lines[k].chars.size());
        for (const CharInfo& c : lines[k].chars) {
            gl.push_back(&c);
        }
        std::stable_sort(gl.begin(), gl.end(), [](const CharInfo* a, const CharInfo* c) {
            if (a->left != c->left) {
                return a->left < c->left;
            }
            return a->space && !c->space;
        });
        std::u32string tag = take_equation_tag(gl, body_size);
        if (!tag.empty()) {
            b.math_tag = std::move(tag);
        }
        const double axis = lines[k].baseline;
        std::u32string latex = math_to_latex(std::move(gl), axis, true);
        if (!latex.empty()) {
            b.math_lines.push_back(std::move(latex));
        }
        b.lines.push_back(lines[k]);
    }
    finalize_block_geometry(b);
    return b;
}

}  // namespace

void compute_lines_geometry(std::span<const Line> lines, Block& b) {
    if (lines.empty()) {
        return;
    }
    double x0 = kCoordSentinel;
    double x1 = -kCoordSentinel;
    double y_top = -kCoordSentinel;
    double y_bottom = kCoordSentinel;
    std::vector<double> sizes;
    sizes.reserve(lines.size());
    int bold = 0;
    int total = 0;
    for (const Line& ln : lines) {
        if (ln.chars.empty()) {
            continue;  // synthetic blank line in code blocks
        }
        x0 = std::min(x0, ln.x0);
        x1 = std::max(x1, ln.x1);
        sizes.push_back(ln.size);
        for (const CharInfo& c : ln.chars) {
            if (c.space) {
                continue;
            }
            ++total;
            if (c.bold) {
                ++bold;
            }
            y_top = std::max(y_top, c.top);
            y_bottom = std::min(y_bottom, c.bottom);
        }
    }
    if (total == 0) {
        b.x0 = 0.0;
        b.x1 = 0.0;
        b.y_top = 0.0;
        b.y_bottom = 0.0;
        b.size = kMinFontSizePt;
        b.bold_majority = false;
        return;
    }
    b.x0 = x0;
    b.x1 = x1;
    b.y_top = y_top;
    b.y_bottom = y_bottom;
    b.size = std::max(median(std::move(sizes)), kMinFontSizePt);
    b.bold_majority = bold * kBoldMajorityDenominator >= total * kBoldMajorityNumerator;
}

void finalize_block_geometry(Block& b) {
    compute_lines_geometry(b.lines, b);
}

size_t count_glyphs(const Line& line) noexcept {
    size_t n = 0;
    for (const CharInfo& c : line.chars) {
        if (!c.space) {
            ++n;
        }
    }
    return n;
}

bool is_citation_lead(const Line& ln) noexcept {
    const std::u32string& p = ln.plain;
    size_t i = 0;
    while (i < p.size() && p[i] == U' ') {
        ++i;
    }
    if (i >= p.size() || p[i] != U'[') {
        return false;
    }
    const size_t d = ++i;
    while (i < p.size() && is_ascii_digit(p[i])) {
        ++i;
    }
    return i > d && i < p.size() && p[i] == U']';
}

// Groups region lines into blocks (paragraphs, list items, code, tables, math).
void region_to_blocks(std::vector<Line> lines, int page, const AnalyzeParams& params,
                      double col_left, double body_size, double page_width,
                      std::vector<Block>& out) {
    if (lines.empty()) {
        return;
    }
    absorb_math_fragments(lines, body_size);
    const RegionStats st = compute_region_stats(lines);

    // Table detection first: consume runs of aligned multi-segment lines.
    std::vector<char> consumed(lines.size(), 0);
    std::vector<std::pair<size_t, TableCandidate>> tables;
    if (params.detect_tables) {
        size_t i = 0;
        while (i < lines.size()) {
            if (lines[i].segments.size() >= 2 && !lines[i].mono_majority) {
                if (auto t = try_table(lines, i, lines.size(), /*render=*/true, page_width)) {
                    for (size_t k = t->first_line; k <= t->last_line; ++k) {
                        consumed[k] = 1;
                    }
                    const size_t next = t->last_line + 1;
                    tables.emplace_back(i, std::move(*t));
                    i = next;
                    continue;
                }
            }
            ++i;
        }
    }

    // Display equations: runs of consecutive centred math lines become one
    // reconstructed $$ block.
    std::vector<std::pair<size_t, Block>> displays;
    {
        auto starts_lower = [](const Line& ln) {
            for (const CharInfo& c : ln.chars) {
                if (!c.space) {
                    return is_ascii_lower(c.code);
                }
            }
            return false;
        };
        size_t i = 0;
        while (i < lines.size()) {
            if (consumed[i] || !is_display_equation_line(lines[i], col_left, body_size)) {
                ++i;
                continue;
            }
            size_t j = i;
            while (j + 1 < lines.size() && !consumed[j + 1] &&
                   is_display_equation_line(lines[j + 1], col_left, body_size) &&
                   lines[j].baseline - lines[j + 1].baseline <
                       kDisplayContMaxGapScale * st.median_size &&
                   starts_lower(lines[j + 1])) {
                ++j;
            }
            for (size_t k = i; k <= j; ++k) {
                consumed[k] = 1;
            }
            displays.emplace_back(i, make_display_block(lines, i, j, page, body_size));
            i = j + 1;
        }
    }

    std::vector<Block> blocks;
    Block* cur = nullptr;
    struct PrevLine {
        bool valid = false;
        double baseline = 0;
        double size = 0;
        double x1 = 0;
        char32_t last_cp = U'\0';
    } prev;
    auto remember = [&prev](const Line& ln) {
        prev.valid = true;
        prev.baseline = ln.baseline;
        prev.size = ln.size;
        prev.x1 = ln.x1;
        prev.last_cp = ln.plain.empty() ? U'\0' : ln.plain.back();
    };
    double list_content_x = 0;

    auto close = [&cur]() { cur = nullptr; };
    auto start_block = [&blocks, &cur, page](BlockKind kind) -> Block& {
        blocks.emplace_back();
        cur = &blocks.back();
        cur->kind = kind;
        cur->page = page;
        return *cur;
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        if (consumed[i]) {
            for (auto& [first, tc] : tables) {
                if (tc.first_line == i) {
                    Block& b = start_block(BlockKind::Table);
                    b.table_cells = std::move(tc.cells);
                    compute_lines_geometry(
                        std::span<const Line>(lines).subspan(
                            tc.first_line, tc.last_line - tc.first_line + 1),
                        b);
                    close();
                    for (auto& grid : tc.extra) {
                        Block& e = start_block(BlockKind::Table);
                        e.table_cells = std::move(grid);
                        e.page = page;
                        close();
                    }
                }
            }
            for (auto& [first, db] : displays) {
                if (first == i) {
                    blocks.push_back(std::move(db));
                    close();
                }
            }
            remember(lines[i]);
            continue;
        }

        Line& ln = lines[i];
        auto marker = detect_list_marker(ln);
        if (marker && !marker->ordered && cur && cur->kind == BlockKind::Paragraph &&
            !cur->lines.empty() && is_citation_lead(cur->lines.front()) && prev.valid) {
            char32_t lead = 0;
            for (const CharInfo& c : ln.chars) {
                if (!c.space) {
                    lead = c.code;
                    break;
                }
            }
            const bool prev_unterminated =
                prev.last_cp != U'.' && prev.last_cp != U'!' && prev.last_cp != U'?';
            if (is_dashy_cp(lead) && prev_unterminated &&
                ln.x0 > cur->x0 + kParagraphIndentScale * st.median_size) {
                marker = std::nullopt;
            }
        }
        auto footnote = leading_footnote_marker(ln);
        const bool is_mono = ln.mono_majority;

        bool new_block = cur == nullptr;
        if (!new_block && prev.valid) {
            const double gap = prev.baseline - ln.baseline;
            const double size_delta = std::fabs(ln.size - prev.size);
            const double right_edge = std::max(cur->x1, ln.x1);
            const bool prev_continues = prev.last_cp == U'-' || prev.last_cp == kUnicodeHyphenCp ||
                                        prev.last_cp == kSoftHyphenCp;
            const bool prev_short =
                !prev_continues &&
                prev.x1 < right_edge - std::max(kShortLineMarginScale * st.median_size,
                                                kShortLineRegionWidthScale * (st.x1 - st.x0));
            const bool indented =
                !cur->lines.empty() &&
                ln.x0 > (cur->kind == BlockKind::ListItem ? list_content_x : cur->x0) +
                            kParagraphIndentScale * st.median_size;
            const bool prev_ends_sentence =
                prev.last_cp == U'.' || prev.last_cp == U'!' || prev.last_cp == U'?';
            const bool citation_entry =
                !cur->lines.empty() && is_citation_lead(cur->lines.front());
            const bool hanging_cont = indented && (citation_entry || !prev_ends_sentence);
            const double max_pair_size = std::max(ln.size, prev.size);

            if (marker || footnote) {
                new_block = true;
            } else if (is_mono != (cur->kind == BlockKind::Code)) {
                new_block = true;
            } else if (size_delta > kFontSizeBreakRatio * max_pair_size) {
                new_block = true;
            } else if (gap > kParagraphGapLeadingScale *
                                     std::max(st.median_gap, kDefaultLeadingScale * max_pair_size) +
                                 kParagraphGapEpsilonPt ||
                       gap > kParagraphHardGapScale * max_pair_size) {
                new_block = true;
            } else if (cur->kind != BlockKind::Code && prev_short && !hanging_cont &&
                       !(cur->kind == BlockKind::ListItem &&
                         std::fabs(ln.x0 - list_content_x) <
                             kListContinuationAlignScale * st.median_size)) {
                new_block = true;
            } else if (cur->kind != BlockKind::Code && indented && !hanging_cont) {
                new_block = true;
            } else if (cur->kind == BlockKind::ListItem &&
                       ln.x0 < list_content_x - kParagraphIndentScale * st.median_size) {
                new_block = true;
            }
        }

        if (new_block) {
            if (footnote) {
                Block& b = start_block(BlockKind::Paragraph);
                b.is_footnote = true;
                b.footnote_numbered = footnote->numbered;
                b.footnote_label = std::move(footnote->label);
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
                b.list_marker = std::move(marker->marker);
                b.x0 = marker->marker_x;
                if (!marker->keep_marker_text) {
                    strip_marker(ln, marker->content_start);
                }
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
            const double gap = prev.baseline - ln.baseline;
            const int blanks = static_cast<int>(std::lround(gap / st.median_gap)) - 1;
            const int clamped_blanks = std::min(blanks, kMaxBlankCodeLines);
            for (int k = 0; k < clamped_blanks; ++k) {
                cur->lines.emplace_back();
            }
        }

        const double marker_x = cur->x0;
        remember(ln);
        cur->lines.push_back(std::move(ln));
        finalize_block_geometry(*cur);
        if (cur->kind == BlockKind::ListItem) {
            cur->x0 = std::min(marker_x, cur->x0);
        }
    }

    // Assign list nesting levels from marker x positions within this region.
    std::vector<double> marker_xs;
    for (const Block& b : blocks) {
        if (b.kind == BlockKind::ListItem) {
            marker_xs.push_back(b.x0);
        }
    }
    if (!marker_xs.empty()) {
        std::sort(marker_xs.begin(), marker_xs.end());
        std::vector<double> levels;
        for (double x : marker_xs) {
            if (levels.empty() || x > levels.back() + kListNestingStepScale * st.median_size) {
                levels.push_back(x);
            }
        }
        for (Block& b : blocks) {
            if (b.kind != BlockKind::ListItem) {
                continue;
            }
            size_t lvl = 0;
            for (size_t k = 0; k < levels.size(); ++k) {
                if (std::fabs(b.x0 - levels[k]) <=
                    kListNestingStepScale * st.median_size + kListNestingEpsilonPt) {
                    lvl = k;
                    break;
                }
            }
            b.list_indent = static_cast<int>(lvl);
        }
    }

    out.reserve(out.size() + blocks.size());
    for (auto& b : blocks) {
        out.push_back(std::move(b));
    }
}

}  // namespace detail
}  // namespace pdf2md
