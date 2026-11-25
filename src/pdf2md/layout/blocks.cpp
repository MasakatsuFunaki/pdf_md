#include "pdf2md/layout/blocks.h"

#include "pdf2md/layout/line_cluster.h"
#include "pdf2md/layout/math.h"
#include "pdf2md/layout/tables_text.h"
#include "pdf2md/model/document_utils.h"
#include "pdf2md/model/geometry.h"
#include "pdf2md/utf8.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace pdf2md::layout {

using model::compute_lines_geometry;
using model::count_glyphs;
using model::finalize_block_geometry;
using model::is_citation_lead;
using model::kCoordSentinel;
using model::kCoordSentinelThreshold;
using model::kDefaultBodySizePt;
using model::kDefaultWordGapScale;
using model::kMinFontSizePt;
using model::median;
using model::raised_origin_glyph;

namespace {

// Footnote, task-list, and thematic-rule code points.
constexpr char32_t kSectionSignCp = 0x00A7U;
constexpr char32_t kPilcrowCp = 0x00B6U;
constexpr char32_t kDaggerCp = 0x2020U;
constexpr char32_t kDoubleDaggerCp = 0x2021U;
constexpr char32_t kAsteriskOperatorCp = 0x2217U;
constexpr char32_t kRadicalCp = 0x221AU;
constexpr char32_t kBoxLightHorizCp = 0x2500U;
constexpr char32_t kWhiteSquareCp = 0x25A1U;
constexpr char32_t kWhiteRoundedSquareCp = 0x25A2U;
constexpr char32_t kBallotBoxCp = 0x2610U;
constexpr char32_t kBallotBoxCheckCp = 0x2611U;
constexpr char32_t kBallotBoxXCp = 0x2612U;
constexpr char32_t kWhiteHeavyCheckCp = 0x2705U;
constexpr char32_t kCheckMarkCp = 0x2713U;
constexpr char32_t kHeavyCheckMarkCp = 0x2714U;

// List, checkbox, thematic-rule, and footnote marker limits.
constexpr std::size_t kMaxOrderedMarkerDigits = 3U;
constexpr std::size_t kMaxRomanMarkerLen = 6U;
constexpr std::size_t kMaxFootnoteDigits = 3U;
constexpr std::size_t kMinThematicRuleChars = 3U;
constexpr double kCheckboxBracketMinGapScale = 0.12;
constexpr double kCheckboxBracketMaxWidthScale = 2.5;

// Region baseline and block-splitting ratios (scaled by font/body size).
constexpr double kMinLineBaselineDeltaPt = 0.1;
constexpr double kMaxIntraParagraphGapScale = 3.0;
constexpr double kDefaultLeadingScale = 1.2;

// Rule-anchored fraction, radical vinculum, and math fragment thresholds.
constexpr double kMinMathRuleWidthPt = 3.0;
constexpr double kMaxMathRulePageWidthRatio = 0.35;
constexpr double kFallbackMaxMathRuleWidthPt = 210.0;
constexpr double kMaxMathRuleThicknessPt = 2.5;
constexpr double kMinBlockingVertRuleLenPt = 3.5;
constexpr double kMathRuleCrossTolPt = 2.0;
constexpr double kFracSubClusterSplitScale = 0.65;
constexpr double kFracBarHorizOverlapScale = 0.25;
constexpr double kFracBarContainPadMinPt = 3.0;
constexpr double kFracBarContainPadScale = 0.45;
constexpr double kFracMinSpanToBarRatio = 0.40;
constexpr double kFracMaxBarExcessScale = 2.0;
constexpr double kFracCenterMaxBarOffsetRatio = 0.25;
constexpr double kFracCenterSlackScale = 0.35;
constexpr std::size_t kMaxFracProseWords = 1U;
constexpr double kFracNumBottomSlackPt = 2.0;
constexpr double kFracNumMaxBottomGapScale = 1.35;
constexpr double kFracNumMaxAboveScale = 1.6;
constexpr double kFracDenTopSlackPt = 2.0;
constexpr double kFracDenMaxTopGapScale = 1.35;
constexpr double kFracDenMaxBelowScale = 1.8;
constexpr double kMathAxisOffsetScale = 0.25;
constexpr double kFracHostAxisTolScale = 0.45;
constexpr double kVinculumStartLeftPadScale = 0.25;
constexpr double kVinculumStartRightPadScale = 0.50;
constexpr double kVinculumMinAxisOffScale = 0.40;
constexpr double kVinculumBelowRadTopTolScale = 0.35;
constexpr double kVinculumAboveRadTopTolScale = 0.55;
constexpr double kVinculumHookLeftPadScale = 1.10;
constexpr double kVinculumContainPadScale = 0.30;
constexpr std::size_t kMinFragmentGlyphCap = 6U;
constexpr std::size_t kFragmentHostRatioNum = 2U;
constexpr std::size_t kFragmentHostRatioDen = 5U;
constexpr double kFragmentMaxLeadingScale = 0.9;
constexpr double kFragmentHorizPadScale = 0.5;
constexpr double kPairedFracMinGapScale = 0.30;
constexpr std::size_t kMaxPairedFracGlyphs = 12U;
constexpr double kMaxPairedFracWidthScale = 6.0;
constexpr double kPairedFracHostReachScale = 2.0;
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
    std::size_t content_start = 0;  // char index where item content begins
    double marker_x = 0.0;          // left edge of the marker
    bool keep_marker_text = false;  // letter/roman markers stay in the text
    TaskState task_state = TaskState::None;
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

constexpr bool is_thematic_rule_cp(char32_t cp) noexcept {
    return cp == U'-' || cp == U'*' || cp == U'_' || cp == kBoxLightHorizCp;
}

bool is_thematic_rule_line(const Line& line) noexcept {
    char32_t rule_cp = 0U;
    std::size_t count = 0U;
    for (const CharInfo& c : line.chars) {
        if (c.space) {
            continue;
        }
        if (!is_thematic_rule_cp(c.code)) {
            return false;
        }
        if (rule_cp == 0U) {
            rule_cp = c.code;
        } else if (c.code != rule_cp) {
            return false;
        }
        ++count;
    }
    return count >= kMinThematicRuleChars;
}

constexpr TaskState ballot_glyph_task_state(char32_t cp) noexcept {
    switch (cp) {
        case kBallotBoxCp:
        case kWhiteSquareCp:
        case kWhiteRoundedSquareCp:
            return TaskState::Unchecked;
        case kBallotBoxCheckCp:
        case kBallotBoxXCp:
        case kCheckMarkCp:
        case kHeavyCheckMarkCp:
        case kWhiteHeavyCheckCp:
            return TaskState::Checked;
        default:
            return TaskState::None;
    }
}

constexpr bool is_checked_bracket_cp(char32_t cp) noexcept {
    return cp == U'x' || cp == U'X' || cp == kCheckMarkCp || cp == kHeavyCheckMarkCp ||
           cp == kWhiteHeavyCheckCp;
}

std::optional<ListMarker> detect_list_marker(const Line& line) {
    if (is_thematic_rule_line(line)) {
        return std::nullopt;
    }
    const auto& chars = line.chars;
    std::size_t i = 0U;
    while (i < chars.size() && chars[i].space) {
        ++i;
    }
    if (i >= chars.size()) {
        return std::nullopt;
    }

    const double wgap = line.word_gap > 0.0 ? line.word_gap : kDefaultWordGapScale * line.size;
    const auto followed_by_gap = [&chars, wgap](std::size_t idx) noexcept {
        std::size_t j = idx + 1U;
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
    const auto content_after = [&chars](std::size_t idx) noexcept {
        std::size_t j = idx + 1U;
        while (j < chars.size() && chars[j].space) {
            ++j;
        }
        return j;
    };
    const auto try_checkbox_at = [&](std::size_t pos) noexcept
        -> std::optional<std::pair<TaskState, std::size_t>> {
        if (pos >= chars.size()) {
            return std::nullopt;
        }
        const TaskState glyph_state = ballot_glyph_task_state(chars[pos].code);
        if (glyph_state != TaskState::None) {
            if (!followed_by_gap(pos)) {
                return std::nullopt;
            }
            return std::make_pair(glyph_state, content_after(pos));
        }
        if (chars[pos].code != U'[') {
            return std::nullopt;
        }
        std::size_t mid = pos + 1U;
        while (mid < chars.size() && chars[mid].space) {
            ++mid;
        }
        if (mid >= chars.size()) {
            return std::nullopt;
        }
        const double line_sz = (std::max)(line.size, kMinFontSizePt);
        if (chars[mid].code == U']') {
            const bool has_inner_space =
                mid > pos + 1U ||
                chars[mid].left - chars[pos].right >= kCheckboxBracketMinGapScale * line_sz;
            if (!has_inner_space ||
                chars[mid].left - chars[pos].right > kCheckboxBracketMaxWidthScale * line_sz ||
                !followed_by_gap(mid)) {
                return std::nullopt;
            }
            return std::make_pair(TaskState::Unchecked, content_after(mid));
        }
        if (!is_checked_bracket_cp(chars[mid].code)) {
            return std::nullopt;
        }
        std::size_t close = mid + 1U;
        while (close < chars.size() && chars[close].space) {
            ++close;
        }
        if (close >= chars.size() || chars[close].code != U']' ||
            chars[close].left - chars[pos].right > kCheckboxBracketMaxWidthScale * line_sz ||
            !followed_by_gap(close)) {
            return std::nullopt;
        }
        return std::make_pair(TaskState::Checked, content_after(close));
    };

    ListMarker m;
    m.marker_x = chars[i].left;

    if (const auto cb = try_checkbox_at(i)) {
        m.ordered = false;
        m.marker = U"-";
        m.task_state = cb->first;
        m.content_start = cb->second;
        return m;
    }

    if (is_bullet_cp(chars[i].code) || is_dashy_cp(chars[i].code)) {
        if (!followed_by_gap(i)) {
            return std::nullopt;
        }
        m.ordered = false;
        m.marker = U"-";
        const std::size_t next = content_after(i);
        if (const auto cb = try_checkbox_at(next)) {
            m.task_state = cb->first;
            m.content_start = cb->second;
            return m;
        }
        m.content_start = next;
        return m;
    }

    if (is_ascii_digit(chars[i].code)) {
        std::size_t j = i;
        std::u32string digits;
        while (j < chars.size() && is_ascii_digit(chars[j].code) &&
               digits.size() <= kMaxOrderedMarkerDigits) {
            digits += chars[j].code;
            ++j;
        }
        if (!digits.empty() && digits.size() <= kMaxOrderedMarkerDigits && j < chars.size() &&
            (chars[j].code == U'.' || chars[j].code == U')')) {
            if (j + 1U < chars.size() && is_ascii_digit(chars[j + 1U].code)) {
                return std::nullopt;
            }
            if (!followed_by_gap(j)) {
                return std::nullopt;
            }
            m.ordered = true;
            m.marker = std::move(digits);
            m.marker += U'.';
            const std::size_t next = content_after(j);
            if (const auto cb = try_checkbox_at(next)) {
                m.task_state = cb->first;
                m.content_start = cb->second;
                return m;
            }
            m.content_start = next;
            return m;
        }
        return std::nullopt;
    }

    if (is_ascii_alpha(chars[i].code)) {
        std::size_t j = i;
        std::size_t len = 0U;
        bool roman = true;
        while (j < chars.size() && is_ascii_alpha(chars[j].code) && len < kMaxRomanMarkerLen) {
            roman = roman && is_roman_cp(chars[j].code);
            ++j;
            ++len;
        }
        const bool ok = len == 1U || (roman && len <= kMaxRomanMarkerLen);
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

void strip_marker(Line& line, std::size_t content_start) {
    if (content_start == 0U) {
        return;
    }
    const std::size_t n = (std::min)(content_start, line.chars.size());
    line.chars.erase(line.chars.begin(), line.chars.begin() + static_cast<std::ptrdiff_t>(n));
    finalize_line(line);
    build_runs(line);
}

struct FootnoteMarker {
    bool numbered = false;
    std::u32string label;
    std::size_t content_start = 0;
};

std::optional<FootnoteMarker> leading_footnote_marker(const Line& line) {
    const auto& chars = line.chars;
    std::size_t i = 0U;
    while (i < chars.size() && chars[i].space) {
        ++i;
    }
    if (i >= chars.size()) {
        return std::nullopt;
    }
    if (script_role(line, chars[i]) != 1) {
        return std::nullopt;
    }

    const auto is_fn_symbol = [](char32_t c) noexcept {
        return c == kDaggerCp || c == kDoubleDaggerCp || c == kSectionSignCp ||
               c == kPilcrowCp || c == kAsteriskOperatorCp || c == U'*';
    };
    FootnoteMarker m;
    std::size_t j = i;
    if (is_fn_symbol(chars[i].code)) {
        m.label = std::u32string(1U, chars[i].code);
        j = i + 1U;
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
    std::size_t k = j;
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
    double x0 = 0.0;
    double x1 = 0.0;
    double median_gap = 0.0;
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
    for (std::size_t i = 0U; i < lines.size(); ++i) {
        st.x0 = (std::min)(st.x0, lines[i].x0);
        st.x1 = (std::max)(st.x1, lines[i].x1);
        sizes.push_back(lines[i].size);
        if (i > 0U) {
            const double g = lines[i - 1U].baseline - lines[i].baseline;
            if (g > kMinLineBaselineDeltaPt) {
                gaps.push_back(g);
            }
        }
    }
    st.median_size = (std::max)(median(std::move(sizes)), kMinFontSizePt);
    std::vector<double> line_gaps;
    line_gaps.reserve(gaps.size());
    for (const double g : gaps) {
        if (g < kMaxIntraParagraphGapScale * st.median_size) {
            line_gaps.push_back(g);
        }
    }
    st.median_gap = median(line_gaps.empty() ? std::move(gaps) : std::move(line_gaps));
    if (st.median_gap <= 0.0) {
        st.median_gap = kDefaultLeadingScale * st.median_size;
    }
    return st;
}

double glyph_ref_y(const CharInfo& c) noexcept {
    if (raised_origin_glyph(c)) {
        return (c.top + c.bottom) * 0.5;
    }
    return c.y;
}

bool line_has_non_space(const Line& ln) noexcept {
    return std::any_of(ln.chars.cbegin(), ln.chars.cend(),
                       [](const CharInfo& c) noexcept { return !c.space; });
}

struct LineSubCluster {
    std::size_t line_idx = 0U;
    std::size_t first = 0U;
    std::size_t last = 0U;
    double x0 = 0.0;
    double x1 = 0.0;
    double bottom = 0.0;
    double top = 0.0;
};

std::vector<LineSubCluster> split_line_subclusters(const Line& ln, std::size_t line_idx,
                                                   double max_gap) {
    std::vector<LineSubCluster> out;
    std::optional<LineSubCluster> cur;
    double prev_right = 0.0;
    for (std::size_t i = 0U; i < ln.chars.size(); ++i) {
        const CharInfo& c = ln.chars[i];
        if (c.space) {
            continue;
        }
        if (cur && c.left - prev_right > max_gap) {
            out.push_back(*cur);
            cur.reset();
        }
        if (!cur) {
            cur = LineSubCluster{line_idx, i, i, c.left, c.right, c.bottom, c.top};
        } else {
            cur->last = i;
            cur->x0 = (std::min)(cur->x0, c.left);
            cur->x1 = (std::max)(cur->x1, c.right);
            cur->bottom = (std::min)(cur->bottom, c.bottom);
            cur->top = (std::max)(cur->top, c.top);
        }
        prev_right = (std::max)(prev_right, c.right);
    }
    if (cur) {
        out.push_back(*cur);
    }
    return out;
}

std::size_t count_plain_prose_words(const Line& ln, std::size_t first,
                                    std::size_t last) noexcept {
    std::size_t words = 0U;
    std::size_t alpha_run = 0U;
    double prev_right = 0.0;
    const double wgap = ln.word_gap > 0.0 ? ln.word_gap : kDefaultWordGapScale * ln.size;
    for (std::size_t i = first; i <= last && i < ln.chars.size(); ++i) {
        const CharInfo& c = ln.chars[i];
        if (c.space) {
            if (alpha_run >= 2U) {
                ++words;
            }
            alpha_run = 0U;
            continue;
        }
        if (alpha_run > 0U && c.left - prev_right > wgap) {
            if (alpha_run >= 2U) {
                ++words;
            }
            alpha_run = 0U;
        }
        if (is_ascii_alpha(c.code) && !c.italic) {
            ++alpha_run;
        } else {
            if (alpha_run >= 2U) {
                ++words;
            }
            alpha_run = 0U;
        }
        prev_right = c.right;
    }
    if (alpha_run >= 2U) {
        ++words;
    }
    return words;
}

bool rule_meets_vertical_segment(double rx0, double rx1, double ry,
                                 std::span<const RuleSegment> page_rules) noexcept {
    for (const RuleSegment& vr : page_rules) {
        if (vr.horizontal) {
            continue;
        }
        const double vy0 = (std::min)(vr.y0, vr.y1);
        const double vy1 = (std::max)(vr.y0, vr.y1);
        if (vy1 - vy0 < kMinBlockingVertRuleLenPt) {
            continue;
        }
        const double vx = (vr.x0 + vr.x1) * 0.5;
        if (vx >= rx0 - kMathRuleCrossTolPt && vx <= rx1 + kMathRuleCrossTolPt &&
            vy0 <= ry + kMathRuleCrossTolPt && vy1 >= ry - kMathRuleCrossTolPt) {
            return true;
        }
    }
    return false;
}

// Absorbs stacked numerator/denominator sub-clusters across unconsumed fraction
// bars and attaches fraction & radical-vinculum RuleSegments to host math Lines.
void absorb_rule_fractions(std::vector<Line>& lines, double body_size, double page_width,
                           std::span<const RuleSegment> page_rules,
                           std::vector<char>* consumed_rules) {
    if (lines.empty() || page_rules.empty() || body_size <= 0.0) {
        return;
    }

    double reg_x0 = kCoordSentinel;
    double reg_x1 = -kCoordSentinel;
    double reg_bot = kCoordSentinel;
    double reg_top = -kCoordSentinel;
    for (const Line& ln : lines) {
        reg_x0 = (std::min)(reg_x0, ln.x0);
        reg_x1 = (std::max)(reg_x1, ln.x1);
        for (const CharInfo& c : ln.chars) {
            if (!c.space) {
                reg_bot = (std::min)(reg_bot, c.bottom);
                reg_top = (std::max)(reg_top, c.top);
            }
        }
    }
    if (reg_x0 > kCoordSentinelThreshold) {
        return;
    }

    const double max_rule_w =
        page_width > 0.0 ? kMaxMathRulePageWidthRatio * page_width : kFallbackMaxMathRuleWidthPt;
    const auto is_rule_consumed = [&](std::size_t ri) noexcept {
        return consumed_rules != nullptr && ri < consumed_rules->size() &&
               (*consumed_rules)[ri] != 0;
    };
    const auto mark_rule_consumed = [&](std::size_t ri) noexcept {
        if (consumed_rules != nullptr && ri < consumed_rules->size()) {
            (*consumed_rules)[ri] = 1;
        }
    };

    std::vector<std::size_t> cand_rules;
    for (std::size_t ri = 0U; ri < page_rules.size(); ++ri) {
        if (is_rule_consumed(ri)) {
            continue;
        }
        const RuleSegment& r = page_rules[ri];
        if (!r.horizontal || r.thickness > kMaxMathRuleThicknessPt) {
            continue;
        }
        const double rx0 = (std::min)(r.x0, r.x1);
        const double rx1 = (std::max)(r.x0, r.x1);
        const double ry = (r.y0 + r.y1) * 0.5;
        const double rw = rx1 - rx0;
        if (rw < kMinMathRuleWidthPt || rw > max_rule_w) {
            continue;
        }
        if (rx1 < reg_x0 - body_size || rx0 > reg_x1 + body_size ||
            ry < reg_bot - 2.0 * body_size || ry > reg_top + 2.0 * body_size) {
            continue;
        }
        if (rule_meets_vertical_segment(rx0, rx1, ry, page_rules)) {
            continue;  // rectangle border, bar-chart bar, or grid edge
        }
        cand_rules.push_back(ri);
    }
    if (cand_rules.empty()) {
        return;
    }

    // Inner nested bars first (narrowest -> widest) so inner fractions merge cleanly
    // into outer numerators/denominators.
    std::sort(cand_rules.begin(), cand_rules.end(), [&](std::size_t a, std::size_t b) noexcept {
        const double wa = std::fabs(page_rules[a].x1 - page_rules[a].x0);
        const double wb = std::fabs(page_rules[b].x1 - page_rules[b].x0);
        return wa < wb;
    });

    std::vector<char> dirty(lines.size(), 0);
    std::vector<std::optional<double>> forced_axis_baseline(lines.size());
    const double split_gap = kFracSubClusterSplitScale * body_size;
    const double hpad = kFracBarHorizOverlapScale * body_size;
    const double contain_pad =
        (std::max)(kFracBarContainPadMinPt, kFracBarContainPadScale * body_size);

    for (const std::size_t ri : cand_rules) {
        if (is_rule_consumed(ri)) {
            continue;
        }
        const RuleSegment& bar = page_rules[ri];
        const double bx0 = (std::min)(bar.x0, bar.x1);
        const double bx1 = (std::max)(bar.x0, bar.x1);
        const double by = (bar.y0 + bar.y1) * 0.5;
        const double bw = bx1 - bx0;
        const double bar_cx = (bx0 + bx1) * 0.5;

        // Locate an optional axis-aligned host line (without requiring bx0 >= host.x0).
        std::optional<std::size_t> host_idx;
        double best_axis_dist = kCoordSentinel;
        for (std::size_t li = 0U; li < lines.size(); ++li) {
            if (lines[li].mono_majority || !line_has_non_space(lines[li])) {
                continue;
            }
            const double ref_sz = (std::max)(lines[li].size, body_size);
            const double axis_y = lines[li].baseline + kMathAxisOffsetScale * lines[li].size;
            const double dist = std::fabs(axis_y - by);
            if (dist <= kFracHostAxisTolScale * ref_sz && dist < best_axis_dist) {
                best_axis_dist = dist;
                host_idx = li;
            }
        }

        double up_x0 = kCoordSentinel;
        double up_x1 = -kCoordSentinel;
        double down_x0 = kCoordSentinel;
        double down_x1 = -kCoordSentinel;
        bool host_has_up = false;
        bool host_has_down = false;
        if (host_idx) {
            for (const CharInfo& c : lines[*host_idx].chars) {
                if (c.space) {
                    continue;
                }
                const double cx = (c.left + c.right) * 0.5;
                if (cx < bx0 - hpad || cx > bx1 + hpad) {
                    continue;
                }
                const double cy = glyph_ref_y(c);
                if (cy > by && c.bottom >= by - kFracNumBottomSlackPt &&
                    c.bottom - by <= kFracNumMaxBottomGapScale * body_size &&
                    cy - by <= kFracNumMaxAboveScale * body_size) {
                    host_has_up = true;
                    up_x0 = (std::min)(up_x0, c.left);
                    up_x1 = (std::max)(up_x1, c.right);
                } else if (cy < by && c.top <= by + kFracDenTopSlackPt &&
                           by - c.top <= kFracDenMaxTopGapScale * body_size &&
                           by - cy <= kFracDenMaxBelowScale * body_size) {
                    host_has_down = true;
                    down_x0 = (std::min)(down_x0, c.left);
                    down_x1 = (std::max)(down_x1, c.right);
                }
            }
        }

        std::vector<LineSubCluster> num_clusters;
        std::vector<LineSubCluster> den_clusters;
        bool prose_veto = false;
        for (std::size_t li = 0U; li < lines.size(); ++li) {
            if ((host_idx && li == *host_idx) || lines[li].mono_majority ||
                !line_has_non_space(lines[li])) {
                continue;
            }
            for (const LineSubCluster& sc : split_line_subclusters(lines[li], li, split_gap)) {
                if (sc.x1 < bx0 - hpad || sc.x0 > bx1 + hpad) {
                    continue;
                }
                if (sc.x0 < bx0 - contain_pad || sc.x1 > bx1 + contain_pad) {
                    continue;  // full-width prose or table row extending beyond the bar
                }
                const double ref_y = lines[li].baseline;
                const bool is_num = ref_y > by && sc.bottom >= by - kFracNumBottomSlackPt &&
                                    sc.bottom - by <= kFracNumMaxBottomGapScale * body_size &&
                                    ref_y - by <= kFracNumMaxAboveScale * body_size;
                const bool is_den = !is_num && ref_y < by && sc.top <= by + kFracDenTopSlackPt &&
                                    by - sc.top <= kFracDenMaxTopGapScale * body_size &&
                                    by - ref_y <= kFracDenMaxBelowScale * body_size;
                if (!is_num && !is_den) {
                    continue;
                }
                if (count_plain_prose_words(lines[li], sc.first, sc.last) > kMaxFracProseWords) {
                    prose_veto = true;
                    break;
                }
                if (is_num) {
                    num_clusters.push_back(sc);
                    up_x0 = (std::min)(up_x0, sc.x0);
                    up_x1 = (std::max)(up_x1, sc.x1);
                } else {
                    den_clusters.push_back(sc);
                    down_x0 = (std::min)(down_x0, sc.x0);
                    down_x1 = (std::max)(down_x1, sc.x1);
                }
            }
            if (prose_veto) {
                break;
            }
        }
        if (prose_veto) {
            continue;
        }

        const bool has_up = host_has_up || !num_clusters.empty();
        const bool has_down = host_has_down || !den_clusters.empty();
        if (!has_up || !has_down) {
            continue;
        }
        if (num_clusters.size() > 1U || den_clusters.size() > 1U) {
            continue;  // booktabs \cmidrule spanning multiple separate table columns
        }

        // Genuine fraction bars fit `max(width(num), width(den))` closely and center
        // both numerator and denominator horizontally on the bar.
        const double up_w = up_x1 - up_x0;
        const double down_w = down_x1 - down_x0;
        const double max_span_w = (std::max)(up_w, down_w);
        if (max_span_w < kFracMinSpanToBarRatio * bw &&
            bw - max_span_w > kFracMaxBarExcessScale * body_size) {
            continue;
        }
        const double max_center_off =
            kFracCenterMaxBarOffsetRatio * bw + kFracCenterSlackScale * body_size;
        if (std::fabs((up_x0 + up_x1) * 0.5 - bar_cx) > max_center_off ||
            std::fabs((down_x0 + down_x1) * 0.5 - bar_cx) > max_center_off) {
            continue;
        }

        const bool created_axis_host = !host_idx.has_value();
        if (!host_idx) {
            host_idx = !num_clusters.empty() ? num_clusters.front().line_idx
                                             : den_clusters.front().line_idx;
        }
        Line& host = lines[*host_idx];
        const double saved_host_bl = host.baseline;

        const auto absorb_clusters = [&](const std::vector<LineSubCluster>& clusters) {
            // Process reverse-by-index when multiple sub-clusters come from the same line.
            for (auto it = clusters.rbegin(); it != clusters.rend(); ++it) {
                if (it->line_idx == *host_idx) {
                    continue;
                }
                Line& src = lines[it->line_idx];
                std::size_t copy_lo = it->first;
                std::size_t copy_hi = it->last;
                while (copy_hi + 1U < src.chars.size() && src.chars[copy_hi + 1U].space) {
                    ++copy_hi;
                }
                host.chars.insert(host.chars.end(),
                                  src.chars.begin() + static_cast<std::ptrdiff_t>(copy_lo),
                                  src.chars.begin() + static_cast<std::ptrdiff_t>(copy_hi) + 1);
                src.chars.erase(src.chars.begin() + static_cast<std::ptrdiff_t>(copy_lo),
                                src.chars.begin() + static_cast<std::ptrdiff_t>(copy_hi) + 1);
                if (!line_has_non_space(src)) {
                    for (const RuleSegment& sr : src.rules) {
                        host.rules.push_back(sr);
                    }
                    src.rules.clear();
                    src.chars.clear();
                } else {
                    finalize_line(src);
                    dirty[it->line_idx] = 1;
                }
            }
        };

        absorb_clusters(num_clusters);
        absorb_clusters(den_clusters);
        host.rules.push_back(bar);
        finalize_line(host);
        if (!forced_axis_baseline[*host_idx]) {
            forced_axis_baseline[*host_idx] =
                created_axis_host ? (by - kMathAxisOffsetScale * host.size) : saved_host_bl;
        }
        host.baseline = *forced_axis_baseline[*host_idx];
        dirty[*host_idx] = 1;
        mark_rule_consumed(ri);
    }

    // Attach radical vinculum bars (and any drifting radicand / root-index glyphs)
    // to Lines carrying U+221A.
    for (std::size_t li = 0U; li < lines.size(); ++li) {
        if (lines[li].mono_majority || !line_has_non_space(lines[li])) {
            continue;
        }
        for (std::size_t ci = 0U; ci < lines[li].chars.size(); ++ci) {
            const CharInfo rad_c = lines[li].chars[ci];
            if (rad_c.space || rad_c.code != kRadicalCp) {
                continue;
            }
            const double sz = (std::max)({rad_c.size, lines[li].size, body_size});
            const double max_rad_top = (std::max)(rad_c.top, lines[li].baseline + sz);
            std::optional<std::size_t> best_ri;
            double best_vdist = kCoordSentinel;
            for (const std::size_t ri : cand_rules) {
                if (is_rule_consumed(ri)) {
                    continue;
                }
                const RuleSegment& r = page_rules[ri];
                const double vx0 = (std::min)(r.x0, r.x1);
                const double vx1 = (std::max)(r.x0, r.x1);
                const double vy = (r.y0 + r.y1) * 0.5;
                if (vx0 < rad_c.left - kVinculumStartLeftPadScale * sz ||
                    vx0 > rad_c.right + kVinculumStartRightPadScale * sz ||
                    vx1 <= rad_c.right) {
                    continue;
                }
                if (vy <= lines[li].baseline + kVinculumMinAxisOffScale * sz ||
                    vy < rad_c.top - kVinculumBelowRadTopTolScale * sz ||
                    vy > max_rad_top + kVinculumAboveRadTopTolScale * sz) {
                    continue;
                }
                const double vdist = std::fabs(vy - rad_c.top);
                if (vdist < best_vdist) {
                    best_vdist = vdist;
                    best_ri = ri;
                }
            }
            if (!best_ri) {
                continue;
            }

            const RuleSegment& vinc = page_rules[*best_ri];
            const double vx1 = (std::max)(vinc.x0, vinc.x1);
            const double vy = (vinc.y0 + vinc.y1) * 0.5;
            const double left_bound = rad_c.left - kVinculumHookLeftPadScale * sz;
            const double right_bound = vx1 + kVinculumContainPadScale * sz;
            const double saved_li_bl = lines[li].baseline;

            for (std::size_t si = 0U; si < lines.size(); ++si) {
                if (si == li || lines[si].mono_majority || !line_has_non_space(lines[si])) {
                    continue;
                }
                const auto clusters = split_line_subclusters(lines[si], si, split_gap);
                for (auto it = clusters.rbegin(); it != clusters.rend(); ++it) {
                    if (it->x0 < left_bound || it->x1 > right_bound ||
                        it->bottom < rad_c.bottom - kVinculumContainPadScale * sz ||
                        it->top > vy + kVinculumContainPadScale * sz) {
                        continue;
                    }
                    Line& src = lines[si];
                    lines[li].chars.insert(
                        lines[li].chars.end(),
                        src.chars.begin() + static_cast<std::ptrdiff_t>(it->first),
                        src.chars.begin() + static_cast<std::ptrdiff_t>(it->last) + 1);
                    src.chars.erase(
                        src.chars.begin() + static_cast<std::ptrdiff_t>(it->first),
                        src.chars.begin() + static_cast<std::ptrdiff_t>(it->last) + 1);
                    if (!line_has_non_space(src)) {
                        for (const RuleSegment& sr : src.rules) {
                            lines[li].rules.push_back(sr);
                        }
                        src.rules.clear();
                        src.chars.clear();
                    } else {
                        finalize_line(src);
                        dirty[si] = 1;
                    }
                }
            }

            lines[li].rules.push_back(vinc);
            finalize_line(lines[li]);
            if (!forced_axis_baseline[li]) {
                forced_axis_baseline[li] = saved_li_bl;
            }
            lines[li].baseline = *forced_axis_baseline[li];
            dirty[li] = 1;
            mark_rule_consumed(*best_ri);
        }
    }

    std::size_t out_idx = 0U;
    for (std::size_t i = 0U; i < lines.size(); ++i) {
        if (!line_has_non_space(lines[i])) {
            continue;
        }
        if (dirty[i] != 0) {
            finalize_line(lines[i]);
            if (forced_axis_baseline[i]) {
                lines[i].baseline = *forced_axis_baseline[i];
            }
            build_runs(lines[i]);
        }
        if (lines[i].plain.empty()) {
            continue;
        }
        if (out_idx != i) {
            lines[out_idx] = std::move(lines[i]);
        }
        ++out_idx;
    }
    lines.resize(out_idx);
}

bool line_has_math_operator(const Line& line) noexcept {
    for (const CharInfo& c : line.chars) {
        if (c.space) {
            continue;
        }
        if (c.code == U'=' || c.code == U'+' || c.code == U'-' || c.code == 0x2212U ||
            c.code == U'<' || c.code == U'>' || c.code == 0x00D7U || c.code == 0x2248U ||
            c.code == 0x2264U || c.code == 0x2265U || c.code == 0x2260U || c.code == 0x2261U ||
            c.code == 0x221AU || c.code == 0x2211U || c.code == 0x220FU || c.code == 0x222BU ||
            raised_origin_glyph(c)) {
            return true;
        }
    }
    return false;
}

// Single-pass O(L) math satellite absorption (no mid-vector erase or restart).
void absorb_math_fragments(std::vector<Line>& lines, double body_size, double page_width) {
    if (body_size <= 0.0 || lines.size() < 2U) {
        return;
    }
    const std::size_t n = lines.size();
    std::vector<std::size_t> glyph_counts(n, 0U);
    for (std::size_t i = 0U; i < n; ++i) {
        glyph_counts[i] = count_glyphs(lines[i]);
    }

    std::vector<char> dead(n, 0);
    std::vector<char> dirty(n, 0);
    const double max_frac_w =
        page_width > 0.0 ? kMaxMathRulePageWidthRatio * page_width : kFallbackMaxMathRuleWidthPt;

    // Pass 1: rule-less paired numerator + denominator fraction stacks straddling
    // a math host line (including leading `\frac{a}{b} = c` and trailing `a = \frac{b}{c}`).
    for (std::size_t i = 1U; i + 1U < n; ++i) {
        if (dead[i - 1U] != 0 || dead[i] != 0 || dead[i + 1U] != 0) {
            continue;
        }
        const Line& top_ln = lines[i - 1U];
        Line& host = lines[i];
        const Line& bot_ln = lines[i + 1U];
        if (top_ln.mono_majority || host.mono_majority || bot_ln.mono_majority) {
            continue;
        }
        if (host.segments.size() >= 2U || top_ln.segments.size() >= 2U ||
            bot_ln.segments.size() >= 2U) {
            continue;
        }
        if (glyph_counts[i - 1U] == 0U || glyph_counts[i + 1U] == 0U ||
            glyph_counts[i - 1U] > kMaxPairedFracGlyphs ||
            glyph_counts[i + 1U] > kMaxPairedFracGlyphs) {
            continue;
        }
        if (!line_has_math_operator(host)) {
            continue;
        }
        const double min_sz = (std::min)(body_size, host.size);
        const double top_gap = top_ln.baseline - host.baseline;
        const double bot_gap = host.baseline - bot_ln.baseline;
        if (top_gap < kPairedFracMinGapScale * min_sz ||
            top_gap >= kFragmentMaxLeadingScale * min_sz ||
            bot_gap < kPairedFracMinGapScale * min_sz ||
            bot_gap >= kFragmentMaxLeadingScale * min_sz) {
            continue;
        }
        if ((std::min)(top_ln.x1, bot_ln.x1) <
            (std::max)(top_ln.x0, bot_ln.x0) - kFracBarHorizOverlapScale * body_size) {
            continue;  // numerator and denominator do not align horizontally
        }
        const double fx0 = (std::min)(top_ln.x0, bot_ln.x0);
        const double fx1 = (std::max)(top_ln.x1, bot_ln.x1);
        const double fw = fx1 - fx0;
        if (fw > (std::min)(max_frac_w, kMaxPairedFracWidthScale * body_size)) {
            continue;
        }
        if (fx1 < host.x0 - kPairedFracHostReachScale * body_size ||
            fx0 > host.x1 + kPairedFracHostReachScale * body_size) {
            continue;
        }

        host.chars.insert(host.chars.end(), top_ln.chars.cbegin(), top_ln.chars.cend());
        host.chars.insert(host.chars.end(), bot_ln.chars.cbegin(), bot_ln.chars.cend());
        host.rules.insert(host.rules.end(), top_ln.rules.cbegin(), top_ln.rules.cend());
        host.rules.insert(host.rules.end(), bot_ln.rules.cbegin(), bot_ln.rules.cend());
        host.x0 = (std::min)(host.x0, fx0);
        host.x1 = (std::max)(host.x1, fx1);
        glyph_counts[i] += glyph_counts[i - 1U] + glyph_counts[i + 1U];
        dead[i - 1U] = 1;
        dead[i + 1U] = 1;
        dirty[i] = 1;
    }

    // Pass 2: single satellite fragments absorbed into their nearest live neighbor.
    for (std::size_t s = 0U; s < n; ++s) {
        if (dead[s] != 0 || lines[s].mono_majority) {
            continue;
        }
        const std::size_t ns = glyph_counts[s];
        std::ptrdiff_t prev_h = static_cast<std::ptrdiff_t>(s) - 1;
        while (prev_h >= 0 && dead[static_cast<std::size_t>(prev_h)] != 0) {
            --prev_h;
        }
        std::size_t next_h = s + 1U;
        while (next_h < n && dead[next_h] != 0) {
            ++next_h;
        }

        std::optional<std::size_t> best;
        double best_gap = kCoordSentinel;
        const auto consider_host = [&](std::size_t h) noexcept {
            const Line& host = lines[h];
            if (host.mono_majority) {
                return;
            }
            const std::size_t nh = glyph_counts[h];
            const std::size_t max_frag =
                (std::max)(kMinFragmentGlyphCap, nh * kFragmentHostRatioNum / kFragmentHostRatioDen);
            if (ns > nh || ns > max_frag) {
                return;
            }
            const double gap = std::fabs(lines[s].baseline - host.baseline);
            const double leading = (std::min)(body_size, host.size);
            if (gap >= kFragmentMaxLeadingScale * leading) {
                return;
            }
            if (lines[s].x0 < host.x0 - kFragmentHorizPadScale * body_size ||
                lines[s].x1 > host.x1 + kFragmentHorizPadScale * body_size) {
                return;
            }
            if (gap < best_gap) {
                best_gap = gap;
                best = h;
            }
        };
        if (prev_h >= 0) {
            consider_host(static_cast<std::size_t>(prev_h));
        }
        if (next_h < n) {
            consider_host(next_h);
        }
        if (!best) {
            continue;
        }

        Line& host = lines[*best];
        host.chars.insert(host.chars.end(), lines[s].chars.cbegin(), lines[s].chars.cend());
        host.rules.insert(host.rules.end(), lines[s].rules.cbegin(), lines[s].rules.cend());
        host.x0 = (std::min)(host.x0, lines[s].x0);
        host.x1 = (std::max)(host.x1, lines[s].x1);
        glyph_counts[*best] += ns;
        dead[s] = 1;
        dirty[*best] = 1;
    }

    std::size_t out_idx = 0U;
    for (std::size_t i = 0U; i < n; ++i) {
        if (dead[i] != 0) {
            continue;
        }
        if (dirty[i] != 0) {
            const double saved_bl = lines[i].baseline;
            finalize_line(lines[i]);
            lines[i].baseline = saved_bl;
            build_runs(lines[i]);
        }
        if (out_idx != i) {
            lines[out_idx] = std::move(lines[i]);
        }
        ++out_idx;
    }
    lines.resize(out_idx);
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
    if (!line.rules.empty()) {
        return true;
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
    const auto prev_glyph = [&keep](std::ptrdiff_t idx) noexcept {
        while (idx >= 0 && keep[static_cast<std::size_t>(idx)]->space) {
            --idx;
        }
        return idx;
    };
    const std::ptrdiff_t e = prev_glyph(static_cast<std::ptrdiff_t>(keep.size()) - 1);
    if (e < 0 || keep[static_cast<std::size_t>(e)]->code != U')') {
        return {};
    }
    std::ptrdiff_t k = prev_glyph(e - 1);
    std::u32string digits;
    while (k >= 0 && is_ascii_digit(keep[static_cast<std::size_t>(k)]->code)) {
        digits.push_back(keep[static_cast<std::size_t>(k)]->code);
        k = prev_glyph(k - 1);
    }
    if (digits.empty() || k < 0 || keep[static_cast<std::size_t>(k)]->code != U'(') {
        return {};
    }
    const std::ptrdiff_t prev = prev_glyph(k - 1);
    if (prev < 0 || keep[static_cast<std::size_t>(k)]->left -
                            keep[static_cast<std::size_t>(prev)]->right <
                        kEquationTagMinGapScale * body_size) {
        return {};
    }
    std::reverse(digits.begin(), digits.end());
    keep.resize(static_cast<std::size_t>(k));
    return digits;
}

Block make_display_block(std::span<const Line> lines, std::size_t first, std::size_t last,
                         int page, double body_size) {
    Block b;
    b.kind = BlockKind::MathDisplay;
    b.page = page;
    b.lines.reserve(last - first + 1U);
    for (std::size_t k = first; k <= last; ++k) {
        std::vector<const CharInfo*> gl;
        gl.reserve(lines[k].chars.size());
        for (const CharInfo& c : lines[k].chars) {
            gl.push_back(&c);
        }
        std::stable_sort(gl.begin(), gl.end(), [](const CharInfo* a, const CharInfo* c) noexcept {
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
        std::u32string latex = math_to_latex(std::move(gl), axis, true, 0, lines[k].rules);
        if (!latex.empty()) {
            b.math_lines.push_back(std::move(latex));
        }
        b.lines.push_back(lines[k]);
    }
    finalize_block_geometry(b);
    return b;
}

}  // namespace

void region_to_blocks(std::vector<Line> lines, int page, const AnalyzeParams& params,
                      double col_left, double body_size, double page_width,
                      std::vector<Block>& out,
                      std::span<const RuleSegment> page_rules,
                      std::vector<char>* consumed_rules) {
    if (lines.empty()) {
        return;
    }
    absorb_rule_fractions(lines, body_size, page_width, page_rules, consumed_rules);
    if (lines.empty()) {
        return;
    }
    absorb_math_fragments(lines, body_size, page_width);
    if (lines.empty()) {
        return;
    }
    const RegionStats st = compute_region_stats(lines);

    // Table detection first: consume runs of aligned multi-segment lines.
    std::vector<char> consumed(lines.size(), 0);
    std::vector<std::pair<std::size_t, TableCandidate>> tables;
    if (params.detect_tables) {
        std::size_t i = 0U;
        while (i < lines.size()) {
            if (lines[i].segments.size() >= 2U && !lines[i].mono_majority) {
                if (auto t = try_table(lines, i, lines.size(), /*render=*/true, page_width)) {
                    for (std::size_t k = t->first_line; k <= t->last_line; ++k) {
                        consumed[k] = 1;
                    }
                    const std::size_t next = t->last_line + 1U;
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
    std::vector<std::pair<std::size_t, Block>> displays;
    {
        const auto starts_lower = [](const Line& ln) noexcept {
            for (const CharInfo& c : ln.chars) {
                if (!c.space) {
                    return is_ascii_lower(c.code);
                }
            }
            return false;
        };
        std::size_t i = 0U;
        while (i < lines.size()) {
            if (consumed[i] != 0 || !is_display_equation_line(lines[i], col_left, body_size)) {
                ++i;
                continue;
            }
            std::size_t j = i;
            while (j + 1U < lines.size() && consumed[j + 1U] == 0 &&
                   is_display_equation_line(lines[j + 1U], col_left, body_size) &&
                   lines[j].baseline - lines[j + 1U].baseline <
                       kDisplayContMaxGapScale * st.median_size &&
                   starts_lower(lines[j + 1U])) {
                ++j;
            }
            for (std::size_t k = i; k <= j; ++k) {
                consumed[k] = 1;
            }
            displays.emplace_back(i, make_display_block(lines, i, j, page, body_size));
            i = j + 1U;
        }
    }

    std::vector<Block> blocks;
    Block* cur = nullptr;
    struct PrevLine {
        bool valid = false;
        double baseline = 0.0;
        double size = 0.0;
        double x1 = 0.0;
        char32_t last_cp = U'\0';
    } prev;
    const auto remember = [&prev](const Line& ln) noexcept {
        prev.valid = true;
        prev.baseline = ln.baseline;
        prev.size = ln.size;
        prev.x1 = ln.x1;
        prev.last_cp = ln.plain.empty() ? U'\0' : ln.plain.back();
    };
    double list_content_x = 0.0;

    const auto close = [&cur]() noexcept { cur = nullptr; };
    const auto start_block = [&blocks, &cur, page](BlockKind kind) -> Block& {
        blocks.emplace_back();
        cur = &blocks.back();
        cur->kind = kind;
        cur->page = page;
        return *cur;
    };

    for (std::size_t i = 0U; i < lines.size(); ++i) {
        if (consumed[i] != 0) {
            for (auto& [first, tc] : tables) {
                if (tc.first_line == i) {
                    const std::size_t head_last =
                        (!tc.extra.empty() && tc.extra.front().first_line > tc.first_line)
                            ? tc.extra.front().first_line - 1U
                            : tc.last_line;
                    Block& b = start_block(BlockKind::Table);
                    b.table_cells = std::move(tc.cells);
                    b.table_align = std::move(tc.align);
                    if (tc.first_line <= head_last && head_last < lines.size()) {
                        compute_lines_geometry(
                            std::span<const Line>(lines).subspan(
                                tc.first_line, head_last - tc.first_line + 1U),
                            b);
                    }
                    close();
                    for (ExtraTableGrid& eg : tc.extra) {
                        Block& e = start_block(BlockKind::Table);
                        e.table_cells = std::move(eg.cells);
                        e.table_align = std::move(eg.align);
                        e.page = page;
                        if (eg.first_line <= eg.last_line && eg.last_line < lines.size()) {
                            compute_lines_geometry(
                                std::span<const Line>(lines).subspan(
                                    eg.first_line, eg.last_line - eg.first_line + 1U),
                                e);
                        }
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
        const bool is_mono = ln.mono_majority;
        if (!is_mono && is_thematic_rule_line(ln)) {
            Block& hr = start_block(BlockKind::HorizontalRule);
            remember(ln);
            hr.lines.push_back(std::move(ln));
            finalize_block_geometry(hr);
            close();
            continue;
        }

        auto marker = detect_list_marker(ln);
        if (marker && !marker->ordered && marker->task_state == TaskState::None &&
            cur != nullptr && cur->kind == BlockKind::Paragraph &&
            !cur->lines.empty() && is_citation_lead(cur->lines.front()) && prev.valid) {
            char32_t lead = 0U;
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

        bool new_block = cur == nullptr;
        if (!new_block && prev.valid) {
            const double gap = prev.baseline - ln.baseline;
            const double size_delta = std::fabs(ln.size - prev.size);
            const double right_edge = (std::max)(cur->x1, ln.x1);
            const bool prev_continues = prev.last_cp == U'-' || prev.last_cp == kUnicodeHyphenCp ||
                                        prev.last_cp == kSoftHyphenCp;
            const bool prev_short =
                !prev_continues &&
                prev.x1 < right_edge - (std::max)(kShortLineMarginScale * st.median_size,
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
            const double max_pair_size = (std::max)(ln.size, prev.size);

            if (marker || footnote) {
                new_block = true;
            } else if (is_mono != (cur->kind == BlockKind::Code)) {
                new_block = true;
            } else if (size_delta > kFontSizeBreakRatio * max_pair_size) {
                new_block = true;
            } else if (gap > kParagraphGapLeadingScale *
                                     (std::max)(st.median_gap,
                                                kDefaultLeadingScale * max_pair_size) +
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
                b.task_state = marker->task_state;
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
            const int clamped_blanks = (std::min)(blanks, kMaxBlankCodeLines);
            for (int k = 0; k < clamped_blanks; ++k) {
                cur->lines.emplace_back();
            }
        }

        const double marker_x = cur->x0;
        remember(ln);
        cur->lines.push_back(std::move(ln));
        finalize_block_geometry(*cur);
        if (cur->kind == BlockKind::ListItem) {
            cur->x0 = (std::min)(marker_x, cur->x0);
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
        for (const double x : marker_xs) {
            if (levels.empty() || x > levels.back() + kListNestingStepScale * st.median_size) {
                levels.push_back(x);
            }
        }
        for (Block& b : blocks) {
            if (b.kind != BlockKind::ListItem) {
                continue;
            }
            std::size_t lvl = 0U;
            for (std::size_t k = 0U; k < levels.size(); ++k) {
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

}  // namespace pdf2md::layout
