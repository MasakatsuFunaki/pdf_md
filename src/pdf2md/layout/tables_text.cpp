#include "pdf2md/layout/tables_text.h"

#include "pdf2md/layout/line_cluster.h"
#include "pdf2md/layout/math.h"
#include "pdf2md/model/geometry.h"
#include "pdf2md/utf8.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace pdf2md::layout {

using model::kCoordSentinel;
using model::kDefaultWordGapScale;
using model::kMinFontSizePt;
using model::median;

namespace {

// Degenerate font-matrix fallback and column-boundary tolerances.
constexpr double kMinReportedFontHeightRatio = 0.5;
constexpr double kMinColSepGapPt = 6.0;
constexpr double kMinColSepGapScale = 0.7;
constexpr double kMinColTolPt = 3.0;
constexpr double kColTolScale = 0.4;
constexpr std::size_t kMinCleanGutterSupport = 2U;
constexpr std::size_t kMajorityFractionDen = 3U;

// Header, spanning, and continuation-folding thresholds.
constexpr double kTightHeaderPitchRatio = 0.82;
constexpr std::size_t kHeaderPeakLookahead = 4U;
constexpr double kHeaderPeakMinRatio = 1.15;
constexpr double kWrappedHeaderMaxGapScale = 1.6;
constexpr double kSpanningGutterMinScale = 1.3;
constexpr double kSpanningAdjWindowScale = 2.0;
constexpr double kCamelAdjPitchScale = 1.8;
constexpr double kCamelAdjSizeScale = 2.5;
constexpr double kColFillMinRatio = 0.55;
constexpr std::size_t kMinWideGridCols = 4U;
constexpr double kCol0NearInnerScale = 6.0;
constexpr double kFragNearBoundaryScale = 2.5;
constexpr double kSubPitchLabelScale = 0.65;

// Column alignment inference thresholds.
constexpr std::size_t kMinAlignRows = 2U;
constexpr double kMinAlignWidthSpanScale = 0.45;
constexpr double kMinAlignTolPt = 2.5;
constexpr double kAlignTolScale = 0.35;
constexpr double kRightAlignSpreadMarginScale = 0.35;
constexpr double kCenterAlignSpreadMarginScale = 0.22;

// Code and assembly listing rejection thresholds.
constexpr std::size_t kMinCodeIndicators = 2U;
constexpr std::size_t kCodeStmtMajorityDen = 3U;
constexpr std::size_t kNumberedCodeMajorityDen = 2U;
constexpr std::size_t kMaxCodeLineNumDigits = 4U;

// Table candidate extent and rejection gates.
constexpr double kTableRowMaxGapScale = 2.0;
constexpr double kCaptionCenterTolRatio = 0.03;
constexpr double kSingleSegMaxWidthRatio = 0.55;
constexpr double kOversizedLeadRatio = 1.15;
constexpr std::size_t kMaxTableColumns = 20U;
constexpr double kTwoColProseMaxGlyphs = 18.0;

struct CellSlice {
    std::size_t first = 0;
    std::size_t last = 0;
    bool present = false;
};

struct CellGeom {
    double x_left = 0.0;
    double x_right = 0.0;
    std::uint8_t line_count = 0U;
    bool spanned = false;
    bool dash_only = false;
};

void merge_cell_geom(CellGeom& dst, const CellGeom& src) noexcept {
    if (src.line_count == 0U) {
        return;
    }
    if (dst.line_count == 0U) {
        dst = src;
        return;
    }
    dst.x_left = (std::min)(dst.x_left, src.x_left);
    dst.x_right = (std::max)(dst.x_right, src.x_right);
    dst.line_count = 2U;
    dst.spanned = dst.spanned || src.spanned;
    dst.dash_only = dst.dash_only && src.dash_only;
}

CellGeom measure_slice_geom(const Line& ln, const CellSlice& s, bool spanned) noexcept {
    CellGeom g;
    if (!s.present || ln.chars.empty() || s.first >= ln.chars.size()) {
        return g;
    }
    const std::size_t hi = (std::min)(s.last, ln.chars.size() - 1U);
    double x_left = kCoordSentinel;
    double x_right = -kCoordSentinel;
    std::size_t nonspace = 0U;
    std::size_t dashes = 0U;
    for (std::size_t i = s.first; i <= hi; ++i) {
        const CharInfo& c = ln.chars[i];
        if (c.space) {
            continue;
        }
        x_left = (std::min)(x_left, c.left);
        x_right = (std::max)(x_right, c.right);
        ++nonspace;
        if (is_dashy_cp(c.code)) {
            ++dashes;
        }
    }
    if (nonspace == 0U) {
        return g;
    }
    g.x_left = x_left;
    g.x_right = x_right;
    g.line_count = 1U;
    g.spanned = spanned;
    g.dash_only = (dashes == nonspace);
    return g;
}

bool segment_starts_with_slash_comment(const Line& ln, std::size_t lo, std::size_t hi) noexcept {
    const std::size_t limit = (std::min)(hi, ln.chars.size() - 1U);
    std::size_t first = lo;
    while (first <= limit && ln.chars[first].space) {
        ++first;
    }
    if (first > limit || ln.chars[first].code != U'/') {
        return false;
    }
    if (first > 0U) {
        std::ptrdiff_t p = static_cast<std::ptrdiff_t>(first) - 1;
        while (p >= 0 && ln.chars[static_cast<std::size_t>(p)].space) {
            --p;
        }
        if (p >= 0 && ln.chars[static_cast<std::size_t>(p)].code == U':') {
            return false;
        }
    }
    std::size_t second = first + 1U;
    if (second > limit || ln.chars[second].space || ln.chars[second].code != U'/') {
        return false;
    }
    const double wgap = ln.word_gap > 0.0 ? ln.word_gap : kDefaultWordGapScale * ln.size;
    return ln.chars[second].left - ln.chars[first].right <= wgap;
}

bool row0_is_clean_alpha_header(const Line& ln) noexcept {
    if (ln.segments.size() < 2U || ln.chars.empty()) {
        return false;
    }
    for (const auto& [lo, hi] : ln.segments) {
        if (lo >= ln.chars.size()) {
            return false;
        }
        if (segment_starts_with_slash_comment(ln, lo, hi)) {
            return false;
        }
        const std::size_t limit = (std::min)(hi, ln.chars.size() - 1U);
        bool seen_first = false;
        for (std::size_t i = lo; i <= limit; ++i) {
            const CharInfo& c = ln.chars[i];
            if (c.space) {
                continue;
            }
            const char32_t cp = c.code;
            if (!seen_first) {
                if (!is_ascii_upper(cp) && !is_cjk_cp(cp)) {
                    return false;
                }
                seen_first = true;
            }
            const bool header_char = is_letter_cp(cp) || is_cjk_cp(cp) || cp == U'-' ||
                                     cp == U'/' || cp == U'_' || cp == U'.';
            if (!header_char) {
                return false;
            }
        }
        if (!seen_first) {
            return false;
        }
    }
    return true;
}

bool looks_like_code_listing(const std::vector<Line>& lines, std::size_t start,
                             std::size_t end) noexcept {
    const std::size_t n = end - start;
    if (n < 2U) {
        return false;
    }
    std::size_t stmt_lines = 0U;
    std::size_t comment_lines = 0U;
    std::size_t numbered_code_lines = 0U;

    for (std::size_t li = start; li < end; ++li) {
        const Line& ln = lines[li];
        if (ln.chars.empty()) {
            continue;
        }
        std::size_t code_limit = ln.chars.size();
        bool has_comment = false;
        for (std::size_t si = 1U; si < ln.segments.size(); ++si) {
            const auto [lo, hi] = ln.segments[si];
            if (lo < ln.chars.size() && segment_starts_with_slash_comment(ln, lo, hi)) {
                has_comment = true;
                code_limit = lo;
                break;
            }
        }
        if (has_comment) {
            ++comment_lines;
        }

        char32_t last_cp = 0U;
        char32_t prev_cp = 0U;
        for (std::size_t i = 0U; i < code_limit; ++i) {
            if (!ln.chars[i].space) {
                prev_cp = last_cp;
                last_cp = ln.chars[i].code;
            }
        }
        if (last_cp == U';' || last_cp == U'{' || (last_cp == U'}' && prev_cp != U'>')) {
            ++stmt_lines;
        }

        if (ln.segments.size() >= 2U) {
            const auto [s0_lo, s0_hi] = ln.segments[0];
            const std::size_t s0_lim = (std::min)(s0_hi, ln.chars.size() - 1U);
            std::size_t digits = 0U;
            bool pure_digits = true;
            for (std::size_t i = s0_lo; i <= s0_lim; ++i) {
                if (ln.chars[i].space) {
                    continue;
                }
                if (!is_ascii_digit(ln.chars[i].code)) {
                    pure_digits = false;
                    break;
                }
                ++digits;
            }
            if (pure_digits && digits >= 1U && digits <= kMaxCodeLineNumDigits) {
                const std::size_t body_lo = ln.segments[1].first;
                bool has_code_punct = false;
                for (std::size_t i = body_lo; i < code_limit && i < ln.chars.size(); ++i) {
                    const char32_t cp = ln.chars[i].code;
                    if (cp == U';' || cp == U'{' || cp == U'}' || cp == U'(' || cp == U')' ||
                        cp == U'=' || cp == U'[') {
                        has_code_punct = true;
                        break;
                    }
                }
                if (has_code_punct) {
                    ++numbered_code_lines;
                }
            }
        }
    }

    if (stmt_lines >= kMinCodeIndicators && stmt_lines * kCodeStmtMajorityDen >= n) {
        return true;
    }
    if (numbered_code_lines >= kMinCodeIndicators &&
        numbered_code_lines * kNumberedCodeMajorityDen >= n) {
        return true;
    }
    if (comment_lines >= kMinCodeIndicators && !row0_is_clean_alpha_header(lines[start])) {
        return true;
    }
    return false;
}

// A size for table geometry robust to degenerate font matrices.
double robust_line_size(const Line& line) {
    std::vector<double> heights;
    heights.reserve(line.chars.size());
    double maxh = 0.0;
    for (const CharInfo& c : line.chars) {
        if (!c.space && c.top > c.bottom) {
            const double h = c.top - c.bottom;
            heights.push_back(h);
            maxh = (std::max)(maxh, h);
        }
    }
    const double medh = median(std::move(heights));
    if (line.size >= kMinReportedFontHeightRatio * medh) {
        return (std::max)(line.size, kMinFontSizePt);
    }
    return std::max({maxh, medh, kMinFontSizePt});
}

std::size_t range_glyphs(const Line& line, std::size_t lo, std::size_t hi) noexcept {
    std::size_t n = 0U;
    for (std::size_t i = lo; i <= hi && i < line.chars.size(); ++i) {
        if (!line.chars[i].space) {
            ++n;
        }
    }
    return n;
}

void append_cell(TableCell& dst, const TableCell& src, bool tight = false) {
    if (src.empty()) {
        return;
    }
    if (!dst.empty() && !tight) {
        StyledRun sep;
        sep.text = U" ";
        dst.push_back(std::move(sep));
    }
    dst.insert(dst.end(), src.cbegin(), src.cend());
}

bool label_join_tight(const TableCell& dst, const TableCell& src) noexcept {
    const auto connector = [](char32_t c) noexcept {
        return c == U'_' || c == U'.' || c == U'/' || c == U'-';
    };
    char32_t last = 0U;
    char32_t first = 0U;
    for (const StyledRun& r : dst) {
        if (!r.text.empty()) {
            last = r.text.back();
        }
    }
    for (const StyledRun& r : src) {
        if (!r.text.empty()) {
            first = r.text.front();
            break;
        }
    }
    return connector(last) || connector(first);
}

// Column separator x-positions shared by the whole row group.
std::vector<double> table_column_boundaries(const std::vector<Line>& lines, std::size_t start,
                                            std::size_t end, double size) {
    const double min_gap = (std::max)(kMinColSepGapPt, kMinColSepGapScale * size);
    const double tol = (std::max)(kMinColTolPt, kColTolScale * size);
    const std::size_t nrows = end - start;

    // 0 = outside the row's ink, 1 = clear column gap, 2 = blocked (cuts a token).
    const auto classify = [min_gap](const Line& ln, double x) noexcept -> int {
        const CharInfo* prev = nullptr;
        for (const CharInfo& c : ln.chars) {
            if (c.space) {
                continue;
            }
            if (prev != nullptr && x > prev->right && x < c.left) {
                return (c.left - prev->right) >= min_gap ? 1 : 2;
            }
            if (x >= c.left && x <= c.right) {
                return 2;
            }
            prev = &c;
        }
        return 0;
    };

    std::vector<std::pair<double, double>> tokens;
    std::vector<double> cand;
    tokens.reserve(nrows * 4U);
    cand.reserve(nrows * 3U);
    for (std::size_t li = start; li < end; ++li) {
        const CharInfo* prev = nullptr;
        double tlo = 0.0;
        double thi = 0.0;
        for (const CharInfo& c : lines[li].chars) {
            if (c.space) {
                continue;
            }
            if (prev != nullptr && c.left - prev->right >= min_gap) {
                cand.push_back((prev->right + c.left) / 2.0);
                tokens.emplace_back(tlo, thi);
                tlo = c.left;
            } else if (prev == nullptr) {
                tlo = c.left;
            }
            thi = c.right;
            prev = &c;
        }
        if (prev != nullptr) {
            tokens.emplace_back(tlo, thi);
        }
    }
    if (cand.empty()) {
        return {};
    }
    std::sort(cand.begin(), cand.end());

    std::vector<std::pair<double, std::size_t>> accepted;
    std::size_t i = 0U;
    while (i < cand.size()) {
        std::size_t j = i;
        double sum = 0.0;
        while (j < cand.size() && cand[j] - cand[i] <= tol) {
            sum += cand[j];
            ++j;
        }
        const double x = sum / static_cast<double>(j - i);
        std::size_t support = 0U;
        std::size_t cut = 0U;
        for (std::size_t li = start; li < end; ++li) {
            const int k = classify(lines[li], x);
            if (k == 1) {
                ++support;
            } else if (k == 2) {
                ++cut;
            }
        }
        const bool clean = support >= kMinCleanGutterSupport && cut == 0U;
        const bool strong = support * kMajorityFractionDen >= nrows &&
                            cut * kMajorityFractionDen <= nrows && support > cut;
        if ((clean || strong) && (accepted.empty() || x - accepted.back().first > tol)) {
            accepted.emplace_back(x, cut);
        }
        i = j;
    }

    const auto real_column_between = [&tokens](double lo, double hi) noexcept {
        for (const auto& [tl, tr] : tokens) {
            if (tl > lo && tr < hi) {
                return true;
            }
        }
        return false;
    };

    std::vector<double> boundaries;
    i = 0U;
    while (i < accepted.size()) {
        std::size_t j = i;
        while (j + 1U < accepted.size() &&
               !real_column_between(accepted[j].first, accepted[j + 1U].first)) {
            ++j;
        }
        std::size_t best = i;
        for (std::size_t k = i; k <= j; ++k) {
            if (accepted[k].second < accepted[best].second) {
                best = k;
            }
        }
        boundaries.push_back(accepted[best].first);
        i = j + 1U;
    }
    return boundaries;
}

void assign_row_cells(const Line& ln, std::span<const double> boundaries, double switch_gap,
                      std::vector<CellSlice>& ranges) {
    ranges.assign(boundaries.size() + 1U, CellSlice{});
    std::size_t curcol = 0U;
    bool have_col = false;
    const CharInfo* prev = nullptr;
    for (std::size_t i = 0U; i < ln.chars.size(); ++i) {
        const CharInfo& c = ln.chars[i];
        if (c.space) {
            continue;
        }
        const double center = (c.left + c.right) / 2.0;
        const auto col = static_cast<std::size_t>(
            std::upper_bound(boundaries.begin(), boundaries.end(), center) - boundaries.begin());
        if (!have_col) {
            curcol = col;
            have_col = true;
        } else if (col != curcol && c.left - prev->right >= switch_gap) {
            curcol = col;
        }
        CellSlice& r = ranges[curcol];
        if (!r.present) {
            r.first = i;
            r.present = true;
        }
        r.last = i;
        prev = &c;
    }
}

std::size_t detect_header_rows(const std::vector<Line>& lines,
                               const std::vector<std::vector<CellSlice>>& slices,
                               std::span<const double> gaps, std::size_t start, std::size_t n,
                               std::size_t ncol, double pitch) {
    const auto is_new_row = [&slices, ncol](std::size_t r) noexcept {
        if (!slices[r][0].present) {
            return false;
        }
        for (std::size_t c = 1U; c < ncol; ++c) {
            if (slices[r][c].present) {
                return true;
            }
        }
        return false;
    };
    std::size_t first_data = n;
    for (std::size_t r = 1U; r < n; ++r) {
        if (is_new_row(r)) {
            first_data = r;
            break;
        }
    }
    std::size_t hdr = 1U;
    for (std::size_t g = 0U; g < gaps.size(); ++g) {
        if (gaps[g] < kTightHeaderPitchRatio * pitch) {
            hdr = g + 2U;
        } else {
            break;
        }
    }
    if (!slices[0][0].present) {
        std::size_t c0 = 0U;
        while (c0 < n && !slices[c0][0].present) {
            ++c0;
        }
        if (c0 > hdr) {
            hdr = c0;
        }
    }
    const std::size_t look = (std::min)(gaps.size(), kHeaderPeakLookahead);
    std::size_t peak = 0U;
    for (std::size_t g = 0U; g < look; ++g) {
        if (gaps[g] > gaps[peak]) {
            peak = g;
        }
    }
    if (peak + 1U > hdr && peak + 1U < n) {
        const double below = peak + 1U < gaps.size() ? gaps[peak + 1U] : gaps[peak];
        if (gaps[peak] >= kHeaderPeakMinRatio * below) {
            hdr = peak + 1U;
        }
    }
    if (hdr > first_data) {
        hdr = (std::max)(first_data, static_cast<std::size_t>(1U));
    }
    if (hdr >= n) {
        hdr = 1U;
    }

    const auto lone_alpha_header_cont = [&lines, &slices, start, n, ncol](std::size_t r) -> bool {
        if (r == 0U || r >= n || slices[r][0].present) {
            return false;
        }
        std::size_t filled = 0U;
        std::size_t only = 0U;
        for (std::size_t c = 0U; c < ncol; ++c) {
            if (slices[r][c].present) {
                ++filled;
                only = c;
            }
        }
        if (filled != 1U || only == 0U) {
            return false;
        }
        bool hdr_labels = false;
        for (std::size_t h = 0U; h < r; ++h) {
            if (slices[h][only].present) {
                hdr_labels = true;
                break;
            }
        }
        if (!hdr_labels) {
            return false;
        }
        const Line& ln = lines[start + r];
        const std::size_t lo = slices[r][only].first;
        const std::size_t hi = slices[r][only].last;
        std::size_t letters = 0U;
        for (std::size_t i = lo; i <= hi && i < ln.chars.size(); ++i) {
            const CharInfo& c = ln.chars[i];
            if (c.space) {
                continue;
            }
            if (!is_ascii_alpha(c.code)) {
                return false;
            }
            ++letters;
        }
        return letters >= 2U;
    };
    while (hdr < n && lone_alpha_header_cont(hdr) &&
           lines[start + hdr - 1U].baseline - lines[start + hdr].baseline <=
               kWrappedHeaderMaxGapScale * pitch) {
        ++hdr;
    }
    return hdr;
}

void merge_spanning_cells(const std::vector<Line>& lines,
                          std::vector<std::vector<CellSlice>>& slices,
                          std::vector<std::vector<char>>& spanned, std::size_t start,
                          std::size_t hdr, std::size_t n, std::size_t ncol, double gsize) {
    spanned.assign(n, std::vector<char>(ncol, 0));
    const double gutter_min = kSpanningGutterMinScale * gsize;
    const double adj_win = kSpanningAdjWindowScale * gsize;
    const auto spanning = [&lines, &slices, start, ncol, gutter_min](std::size_t r) -> bool {
        const Line& ln = lines[start + r];
        std::optional<std::size_t> tgt;
        for (std::size_t c = 1U; c < ncol; ++c) {
            if (!slices[r][c].present) {
                continue;
            }
            if (tgt && ln.chars[slices[r][c].first].left - ln.chars[slices[r][*tgt].last].right <
                           gutter_min) {
                return true;
            }
            tgt = c;
        }
        return false;
    };
    std::vector<char> spans(n, 0);
    for (std::size_t r = hdr; r < n; ++r) {
        spans[r] = spanning(r) ? 1 : 0;
    }
    for (std::size_t r = hdr; r < n; ++r) {
        if (!spans[r]) {
            continue;
        }
        const bool confirmed =
            (r > hdr && lines[start + r - 1U].baseline - lines[start + r].baseline <= adj_win &&
             spans[r - 1U]) ||
            (r + 1U < n && lines[start + r].baseline - lines[start + r + 1U].baseline <= adj_win &&
             spans[r + 1U]);
        if (!confirmed) {
            continue;
        }
        const Line& ln = lines[start + r];
        std::optional<std::size_t> target;
        for (std::size_t c = 1U; c < ncol; ++c) {
            if (!slices[r][c].present) {
                continue;
            }
            if (target &&
                ln.chars[slices[r][c].first].left - ln.chars[slices[r][*target].last].right <
                    gutter_min) {
                slices[r][*target].last = slices[r][c].last;
                slices[r][c] = CellSlice{};
                spanned[r][*target] = 1;
                continue;
            }
            target = c;
        }
    }
}

void fold_row_continuations(const std::vector<Line>& lines,
                            const std::vector<std::vector<CellSlice>>& slices,
                            std::span<const double> boundaries, std::size_t start,
                            std::size_t hdr, std::size_t ncol, double gsize, double pitch,
                            double inner_left, double tx0, double col_tol, bool nested,
                            std::vector<std::vector<TableCell>>& data,
                            std::vector<double>& data_base,
                            std::vector<std::vector<CellGeom>>& data_geom) {
    const auto inner_continuation = [inner_left, col_tol](const Line& ln) noexcept {
        return ln.segments.size() < 2U && inner_left < kCoordSentinel &&
               ln.x0 >= inner_left - col_tol;
    };

    enum class RowKind { Full, Col0, Inner, Other };
    std::vector<RowKind> kind(data.size(), RowKind::Other);
    for (std::size_t di = 0U; di < data.size(); ++di) {
        const auto& s = slices[hdr + di];
        bool inner = false;
        for (std::size_t c = 1U; c < ncol; ++c) {
            if (s[c].present) {
                inner = true;
            }
        }
        const bool c0 = s[0].present;
        if (c0 && inner) {
            kind[di] = RowKind::Full;
        } else if (c0 && !inner) {
            kind[di] = RowKind::Col0;
        } else if (!c0 && inner && (nested || inner_continuation(lines[start + hdr + di]))) {
            kind[di] = RowKind::Inner;
        }
    }

    std::vector<std::vector<TableCell>> folded;
    std::vector<double> folded_base;
    std::vector<std::vector<CellGeom>> folded_geom;
    std::vector<char> folded_wraps;
    std::vector<TableCell> pending;
    std::vector<CellGeom> pending_geom;
    folded.reserve(data.size());
    folded_base.reserve(data.size());
    folded_geom.reserve(data.size());
    folded_wraps.reserve(data.size());

    const auto flush_pending = [&pending, &pending_geom](std::vector<TableCell>& row,
                                                         std::vector<CellGeom>& geom) {
        if (pending.empty()) {
            return false;
        }
        TableCell joined;
        CellGeom joined_geom;
        for (std::size_t p = 0U; p < pending.size(); ++p) {
            append_cell(joined, pending[p], label_join_tight(joined, pending[p]));
            merge_cell_geom(joined_geom, pending_geom[p]);
        }
        append_cell(joined, row[0], label_join_tight(joined, row[0]));
        merge_cell_geom(joined_geom, geom[0]);
        row[0] = std::move(joined);
        geom[0] = joined_geom;
        pending.clear();
        pending_geom.clear();
        return true;
    };
    const auto push_row = [&folded, &folded_base, &folded_geom, &folded_wraps, &data, &data_base,
                           &data_geom](std::size_t di, bool wraps) {
        folded.push_back(std::move(data[di]));
        folded_base.push_back(data_base[di]);
        folded_geom.push_back(std::move(data_geom[di]));
        folded_wraps.push_back(wraps ? 1 : 0);
    };

    for (std::size_t di = 0U; di < data.size(); ++di) {
        if (kind[di] == RowKind::Inner) {
            if (!pending.empty()) {
                const bool w = flush_pending(data[di], data_geom[di]);
                push_row(di, w);
            } else if (!folded.empty()) {
                for (std::size_t c = 1U; c < ncol; ++c) {
                    if (!data[di][c].empty()) {
                        append_cell(folded.back()[c], data[di][c]);
                        merge_cell_geom(folded_geom.back()[c], data_geom[di][c]);
                    }
                }
            } else {
                push_row(di, false);
            }
            continue;
        }

        if (kind[di] == RowKind::Full && pending.empty() && !folded.empty() && di > 0U &&
            slices[hdr + di - 1U][0].present && slices[hdr + di][0].present) {
            const CharInfo& prev_last =
                lines[start + hdr + di - 1U].chars[slices[hdr + di - 1U][0].last];
            const CharInfo& frag_first =
                lines[start + hdr + di].chars[slices[hdr + di][0].first];
            const double gap_up =
                folded_base.empty() ? kCoordSentinel : folded_base.back() - data_base[di];
            const bool camel = is_ascii_lower(prev_last.code) && is_ascii_upper(frag_first.code) &&
                               !prev_last.bold && !frag_first.bold;
            const bool adjacent =
                gap_up <= (std::max)(kCamelAdjPitchScale * pitch, kCamelAdjSizeScale * gsize);
            const bool fills_col =
                boundaries[0] > tx0 &&
                (prev_last.right - tx0) >= kColFillMinRatio * (boundaries[0] - tx0);
            bool bare_label = false;
            if (ncol >= kMinWideGridCols) {
                bool typed = false;
                for (std::size_t c = 1U; c + 1U < ncol; ++c) {
                    if (!data[di][c].empty()) {
                        typed = true;
                    }
                }
                bare_label = !typed;
            }
            const auto column_wrap_continuation = [&lines, &slices, boundaries, start, hdr,
                                                   di](std::size_t c) noexcept -> bool {
                if (!slices[hdr + di - 1U][c].present || !slices[hdr + di][c].present) {
                    return false;
                }
                const CharInfo& above_last =
                    lines[start + hdr + di - 1U].chars[slices[hdr + di - 1U][c].last];
                const CharInfo& frag_first_c =
                    lines[start + hdr + di].chars[slices[hdr + di][c].first];
                const bool camel_c = is_ascii_lower(above_last.code) &&
                                     is_ascii_upper(frag_first_c.code) && !above_last.bold &&
                                     !frag_first_c.bold;
                const double left_c = boundaries[c - 1U];
                const bool fills_c =
                    boundaries[c] > left_c &&
                    (above_last.right - left_c) >= kColFillMinRatio * (boundaries[c] - left_c);
                return camel_c && fills_c;
            };
            bool typed_collision = false;
            for (std::size_t c = 1U; c + 1U < ncol; ++c) {
                if (!folded.back()[c].empty() && !data[di][c].empty() &&
                    !column_wrap_continuation(c)) {
                    typed_collision = true;
                }
            }
            if (camel && adjacent && (fills_col || bare_label) && !typed_collision) {
                append_cell(folded.back()[0], data[di][0], /*tight=*/true);
                merge_cell_geom(folded_geom.back()[0], data_geom[di][0]);
                for (std::size_t c = 1U; c < ncol; ++c) {
                    if (!data[di][c].empty()) {
                        append_cell(folded.back()[c], data[di][c],
                                    c + 1U < ncol && column_wrap_continuation(c));
                        merge_cell_geom(folded_geom.back()[c], data_geom[di][c]);
                    }
                }
                folded_wraps.back() = 1;
                continue;
            }
        }

        if (kind[di] == RowKind::Col0) {
            const bool next_inner = di + 1U < data.size() && kind[di + 1U] == RowKind::Inner;
            const bool prev_value =
                di > 0U && (kind[di - 1U] == RowKind::Full || kind[di - 1U] == RowKind::Inner);
            const double gap_up =
                folded.empty() ? kCoordSentinel : folded_base.back() - data_base[di];
            const double gap_dn =
                di + 1U < data.size() ? data_base[di] - data_base[di + 1U] : kCoordSentinel;
            double prev_rx = -1.0;
            bool prev_word_end = false;
            if (di > 0U && slices[hdr + di - 1U][0].present) {
                const CharInfo& pc =
                    lines[start + hdr + di - 1U].chars[slices[hdr + di - 1U][0].last];
                prev_rx = pc.right;
                prev_word_end = is_ascii_alpha(pc.code) || is_ascii_digit(pc.code);
            }
            const bool prev_fills_col =
                prev_rx >= 0.0 && boundaries[0] > tx0 &&
                (prev_rx - tx0) >= kColFillMinRatio * (boundaries[0] - tx0);
            const bool prev_wrapped =
                (!folded.empty() && folded_wraps.back() != 0) ||
                (prev_rx >= 0.0 && prev_word_end && inner_left < kCoordSentinel &&
                 inner_left - prev_rx <= kCol0NearInnerScale * gsize && prev_fills_col);

            bool labelish = false;
            if (slices[hdr + di][0].present) {
                const Line& cur_ln = lines[start + hdr + di];
                for (std::size_t i = slices[hdr + di][0].first;
                     i <= slices[hdr + di][0].last && i < cur_ln.chars.size(); ++i) {
                    const char32_t cp = cur_ln.chars[i].code;
                    if (is_ascii_alpha(cp) || is_ascii_digit(cp)) {
                        labelish = true;
                        break;
                    }
                }
            }
            if (!next_inner && prev_value && !folded.empty() &&
                gap_up <= kCamelAdjPitchScale * pitch && prev_wrapped && labelish) {
                append_cell(folded.back()[0], data[di][0],
                            label_join_tight(folded.back()[0], data[di][0]));
                merge_cell_geom(folded_geom.back()[0], data_geom[di][0]);
                folded_wraps.back() = 1;
                continue;
            }

            double frag_rx = -1.0;
            char32_t frag_last = 0U;
            if (slices[hdr + di][0].present) {
                const CharInfo& fc = lines[start + hdr + di].chars[slices[hdr + di][0].last];
                frag_rx = fc.right;
                frag_last = fc.code;
            }
            const bool frag_wraps =
                frag_last == U'_' || frag_last == U'.' || frag_last == U'/' ||
                (frag_rx >= 0.0 && boundaries[0] - frag_rx <= kFragNearBoundaryScale * gsize);
            const bool next_full_sandwich = di + 2U < data.size() &&
                                            kind[di + 1U] == RowKind::Full &&
                                            kind[di + 2U] == RowKind::Col0 && frag_wraps;
            if ((next_inner || next_full_sandwich) && gap_dn <= kCamelAdjPitchScale * pitch) {
                pending.push_back(std::move(data[di][0]));
                pending_geom.push_back(data_geom[di][0]);
                continue;
            }
        }
        const bool w = flush_pending(data[di], data_geom[di]);
        push_row(di, w);
    }
    if (!pending.empty() && !folded.empty()) {
        for (std::size_t p = 0U; p < pending.size(); ++p) {
            append_cell(folded.back()[0], pending[p],
                        label_join_tight(folded.back()[0], pending[p]));
            merge_cell_geom(folded_geom.back()[0], pending_geom[p]);
        }
    }
    data = std::move(folded);
    data_base = std::move(folded_base);
    data_geom = std::move(folded_geom);
}

void fold_subpitch_labels(std::size_t ncol, double pitch,
                          std::vector<std::vector<TableCell>>& data,
                          std::vector<double>& data_base,
                          std::vector<std::vector<CellGeom>>& data_geom) {
    for (std::size_t r = 0U; r < data.size();) {
        std::size_t f = 0U;
        std::size_t fc = 0U;
        for (std::size_t c = 0U; c < ncol; ++c) {
            if (!data[r][c].empty()) {
                ++f;
                fc = c;
            }
        }
        if (f == 1U) {
            const double gp = r > 0U ? data_base[r - 1U] - data_base[r] : kCoordSentinel;
            const double gn =
                r + 1U < data.size() ? data_base[r] - data_base[r + 1U] : kCoordSentinel;
            if ((std::min)(gp, gn) < kSubPitchLabelScale * pitch) {
                const std::size_t tgt = gp <= gn ? r - 1U : r + 1U;
                TableCell merged = std::move(data[r][fc]);
                append_cell(merged, data[tgt][fc]);
                data[tgt][fc] = std::move(merged);
                merge_cell_geom(data_geom[tgt][fc], data_geom[r][fc]);
                data.erase(data.begin() + static_cast<std::ptrdiff_t>(r));
                data_base.erase(data_base.begin() + static_cast<std::ptrdiff_t>(r));
                data_geom.erase(data_geom.begin() + static_cast<std::ptrdiff_t>(r));
                continue;
            }
        }
        ++r;
    }
}

void infer_text_column_alignments(std::size_t ncol, std::span<const double> boundaries,
                                  double gsize, double col_tol,
                                  const std::vector<std::vector<CellGeom>>& data_geom,
                                  std::vector<TableAlign>& out_align) {
    out_align.assign(ncol, TableAlign::None);
    const double tol = (std::max)(kMinAlignTolPt, kAlignTolScale * gsize);
    const double min_w_span = kMinAlignWidthSpanScale * gsize;

    for (std::size_t c = 0U; c < ncol; ++c) {
        std::size_t count = 0U;
        double min_l = kCoordSentinel;
        double max_l = -kCoordSentinel;
        double min_r = kCoordSentinel;
        double max_r = -kCoordSentinel;
        double min_c = kCoordSentinel;
        double max_c = -kCoordSentinel;
        double min_w = kCoordSentinel;
        double max_w = -kCoordSentinel;

        for (const auto& row_geom : data_geom) {
            if (c >= row_geom.size()) {
                continue;
            }
            const CellGeom& g = row_geom[c];
            if (g.line_count != 1U || g.spanned || g.dash_only) {
                continue;
            }
            if (c > 0U && g.x_left < boundaries[c - 1U] - col_tol) {
                continue;
            }
            if (c + 1U < ncol && g.x_right > boundaries[c] + col_tol) {
                continue;
            }
            const double w = g.x_right - g.x_left;
            const double xc = 0.5 * (g.x_left + g.x_right);
            min_l = (std::min)(min_l, g.x_left);
            max_l = (std::max)(max_l, g.x_left);
            min_r = (std::min)(min_r, g.x_right);
            max_r = (std::max)(max_r, g.x_right);
            min_c = (std::min)(min_c, xc);
            max_c = (std::max)(max_c, xc);
            min_w = (std::min)(min_w, w);
            max_w = (std::max)(max_w, w);
            ++count;
        }
        if (count < kMinAlignRows || (max_w - min_w) < min_w_span) {
            continue;
        }
        const double spread_l = max_l - min_l;
        const double spread_r = max_r - min_r;
        const double spread_c = max_c - min_c;
        if (spread_r <= tol && spread_l > spread_r + kRightAlignSpreadMarginScale * gsize) {
            out_align[c] = TableAlign::Right;
            continue;
        }
        if (spread_c <= tol && spread_l > spread_c + kCenterAlignSpreadMarginScale * gsize &&
            spread_r > spread_c + kCenterAlignSpreadMarginScale * gsize) {
            out_align[c] = TableAlign::Center;
        }
    }
}

void prune_empty_columns(std::size_t ncol, std::vector<std::vector<TableCell>>& cells,
                         std::vector<TableAlign>& align) {
    std::vector<std::size_t> active_cols;
    active_cols.reserve(ncol);
    for (std::size_t c = 0U; c < ncol; ++c) {
        const bool col_has_ink = std::any_of(
            cells.cbegin(), cells.cend(),
            [c](const std::vector<TableCell>& row) { return c < row.size() && !row[c].empty(); });
        if (col_has_ink) {
            active_cols.push_back(c);
        }
    }
    if (active_cols.size() >= 2U && active_cols.size() < ncol) {
        for (auto& row : cells) {
            std::vector<TableCell> compacted;
            compacted.reserve(active_cols.size());
            for (std::size_t c : active_cols) {
                compacted.push_back(std::move(row[c]));
            }
            row = std::move(compacted);
        }
        if (align.size() == ncol) {
            std::vector<TableAlign> compacted_align;
            compacted_align.reserve(active_cols.size());
            for (std::size_t c : active_cols) {
                compacted_align.push_back(align[c]);
            }
            align = std::move(compacted_align);
        }
    }
}

std::vector<std::vector<TableCell>> build_table_grid(const std::vector<Line>& lines,
                                                     std::size_t start, std::size_t end,
                                                     std::span<const double> boundaries,
                                                     double inner_left, double tx0, double col_tol,
                                                     bool nested,
                                                     std::vector<TableAlign>& out_align) {
    const std::size_t n = end - start;
    const std::size_t ncol = boundaries.size() + 1U;
    std::vector<double> gsizes;
    gsizes.reserve(n);
    for (std::size_t li = start; li < end; ++li) {
        gsizes.push_back(robust_line_size(lines[li]));
    }
    const double gsize = (std::max)(median(std::move(gsizes)), kMinFontSizePt);
    const double switch_gap = (std::max)(kMinColSepGapPt, kMinColSepGapScale * gsize);

    std::vector<std::vector<CellSlice>> slices(n);
    for (std::size_t li = start; li < end; ++li) {
        assign_row_cells(lines[li], boundaries, switch_gap, slices[li - start]);
    }

    std::vector<double> gaps;
    gaps.reserve(n);
    for (std::size_t li = start + 1U; li < end; ++li) {
        gaps.push_back(lines[li - 1U].baseline - lines[li].baseline);
    }
    const double pitch = (std::max)(median(gaps), kMinFontSizePt);

    const std::size_t hdr = detect_header_rows(lines, slices, gaps, start, n, ncol, pitch);
    std::vector<std::vector<char>> spanned;
    merge_spanning_cells(lines, slices, spanned, start, hdr, n, ncol, gsize);

    const auto build_row = [&lines, &slices, start, ncol](std::size_t r) {
        std::vector<TableCell> row(ncol);
        for (std::size_t c = 0U; c < ncol; ++c) {
            if (slices[r][c].present) {
                row[c] = render_cell_runs(lines[start + r], slices[r][c].first, slices[r][c].last);
            }
        }
        return row;
    };

    std::vector<TableCell> header(ncol);
    for (std::size_t r = 0U; r < hdr; ++r) {
        std::vector<TableCell> hr = build_row(r);
        for (std::size_t c = 0U; c < ncol; ++c) {
            append_cell(header[c], hr[c]);
        }
    }

    std::vector<std::vector<TableCell>> data;
    std::vector<double> data_base;
    std::vector<std::vector<CellGeom>> data_geom;
    data.reserve(n - hdr);
    data_base.reserve(n - hdr);
    data_geom.reserve(n - hdr);
    for (std::size_t r = hdr; r < n; ++r) {
        data.push_back(build_row(r));
        data_base.push_back(lines[start + r].baseline);
        std::vector<CellGeom> rgeom(ncol);
        for (std::size_t c = 0U; c < ncol; ++c) {
            rgeom[c] = measure_slice_geom(lines[start + r], slices[r][c], spanned[r][c] != 0);
        }
        data_geom.push_back(std::move(rgeom));
    }

    fold_row_continuations(lines, slices, boundaries, start, hdr, ncol, gsize, pitch, inner_left,
                           tx0, col_tol, nested, data, data_base, data_geom);
    fold_subpitch_labels(ncol, pitch, data, data_base, data_geom);
    infer_text_column_alignments(ncol, boundaries, gsize, col_tol, data_geom, out_align);

    std::vector<std::vector<TableCell>> cells;
    cells.reserve(1U + data.size());
    cells.push_back(std::move(header));
    for (auto& row : data) {
        cells.push_back(std::move(row));
    }

    prune_empty_columns(ncol, cells, out_align);
    return cells;
}

}  // namespace

TableCell render_cell_runs(const Line& line, std::size_t lo, std::size_t hi) {
    if (line.chars.empty() || lo > hi || lo >= line.chars.size()) {
        return {};
    }
    const std::size_t limit = (std::min)(hi, line.chars.size() - 1U);
    while (lo <= limit && line.chars[lo].space) {
        ++lo;
    }
    if (lo > limit) {
        return {};
    }
    std::size_t trimmed_hi = limit;
    while (trimmed_hi > lo && line.chars[trimmed_hi].space) {
        --trimmed_hi;
    }

    Line sub;
    sub.chars.assign(line.chars.cbegin() + static_cast<std::ptrdiff_t>(lo),
                     line.chars.cbegin() + static_cast<std::ptrdiff_t>(trimmed_hi) + 1);

    // Collapse runs consisting solely of placeholder dashes into a single dash.
    {
        std::size_t nonspace = 0U;
        std::size_t dashes = 0U;
        for (const CharInfo& c : sub.chars) {
            if (c.space) {
                continue;
            }
            ++nonspace;
            if (is_dashy_cp(c.code)) {
                ++dashes;
            }
        }
        if (nonspace >= 2U && dashes == nonspace) {
            for (const CharInfo& c : sub.chars) {
                if (!c.space) {
                    const CharInfo keep = c;
                    sub.chars.assign(1U, keep);
                    break;
                }
            }
        }
    }

    int bold = 0;
    int total = 0;
    for (const CharInfo& c : sub.chars) {
        if (!c.space) {
            ++total;
            if (c.bold) {
                ++bold;
            }
        }
    }
    finalize_line(sub);
    if (line.word_gap > 0.0) {
        sub.word_gap = line.word_gap;
    }
    build_runs(sub);
    if (total > 0 && bold * 2 > total) {
        for (StyledRun& r : sub.runs) {
            r.bold = true;
        }
    }
    return sub.runs;
}

std::optional<TableCandidate> try_table(const std::vector<Line>& lines, std::size_t start,
                                        std::size_t region_lines, bool render, double page_width) {
    const double seed_size = robust_line_size(lines[start]);
    const double gap_limit = kTableRowMaxGapScale * seed_size;
    const double col_tol = (std::max)(kMinColTolPt, kColTolScale * seed_size);

    double inner_left = kCoordSentinel;
    const auto note_inner = [&inner_left](const Line& ln) {
        if (ln.segments.size() >= 2U) {
            inner_left = (std::min)(inner_left, ln.chars[ln.segments[1].first].left);
        }
    };
    const auto inner_continuation = [&inner_left, col_tol](const Line& ln) noexcept {
        return ln.segments.size() < 2U && inner_left < kCoordSentinel &&
               ln.x0 >= inner_left - col_tol;
    };

    std::size_t end = start + 1U;
    double tx0 = lines[start].x0;
    double tx1 = lines[start].x1;
    note_inner(lines[start]);

    const double page_center = page_width / 2.0;
    const auto caption_shaped = [&inner_left, &tx0, col_tol, page_width,
                                 page_center](const Line& ln) noexcept {
        if (ln.segments.size() >= 2U || inner_left >= kCoordSentinel || page_width <= 0.0) {
            return false;
        }
        std::size_t nonspace = 0U;
        std::size_t bold = 0U;
        for (const CharInfo& c : ln.chars) {
            if (!c.space) {
                ++nonspace;
                if (c.bold) {
                    ++bold;
                }
            }
        }
        if (nonspace == 0U || bold * 2U <= nonspace) {
            return false;
        }
        if (ln.x0 - tx0 <= col_tol) {
            return false;
        }
        const double center = (ln.x0 + ln.x1) / 2.0;
        return std::fabs(center - page_center) <= kCaptionCenterTolRatio * page_width;
    };

    while (end < region_lines) {
        const Line& cur = lines[end];
        if (cur.mono_majority) {
            break;
        }
        if (lines[end - 1U].baseline - cur.baseline > gap_limit) {
            break;
        }
        if (caption_shaped(cur)) {
            break;
        }
        if (cur.segments.size() < 2U &&
            (cur.x1 - cur.x0) > kSingleSegMaxWidthRatio * (tx1 - tx0) &&
            !inner_continuation(cur)) {
            break;
        }
        if (cur.segments.size() >= 2U) {
            tx0 = (std::min)(tx0, cur.x0);
            tx1 = (std::max)(tx1, cur.x1);
            note_inner(cur);
        }
        ++end;
    }

    const auto col0_fragment = [&inner_left, &tx0, col_tol](const Line& ln) noexcept {
        return ln.segments.size() < 2U && inner_left < kCoordSentinel && ln.x0 <= tx0 + col_tol &&
               ln.x1 <= inner_left - col_tol;
    };
    const auto has_inner_ink = [&inner_continuation](const Line& ln) noexcept {
        return ln.segments.size() >= 2U || inner_continuation(ln);
    };
    while (end - 1U > start && lines[end - 1U].segments.size() < 2U) {
        const Line& last = lines[end - 1U];
        if (col0_fragment(last)) {
            break;
        }
        if (inner_continuation(last) && has_inner_ink(lines[end - 2U])) {
            break;
        }
        --end;
    }

    std::size_t n = end - start;
    if (n < 2U) {
        return std::nullopt;
    }

    const auto grid_size = [&lines](std::size_t s, std::size_t e) {
        std::vector<double> sz;
        sz.reserve(e - s);
        for (std::size_t li = s; li < e; ++li) {
            sz.push_back(robust_line_size(lines[li]));
        }
        return (std::max)(median(std::move(sz)), kMinFontSizePt);
    };
    double gsize = grid_size(start, end);

    bool trimmed = false;
    while (end - start > 2U && robust_line_size(lines[start]) > kOversizedLeadRatio * gsize) {
        ++start;
        gsize = grid_size(start, end);
        trimmed = true;
    }
    n = end - start;
    if (n < 2U) {
        return std::nullopt;
    }
    if (trimmed) {
        inner_left = kCoordSentinel;
        tx0 = lines[start].x0;
        tx1 = lines[start].x1;
        for (std::size_t li = start; li < end; ++li) {
            tx0 = (std::min)(tx0, lines[li].x0);
            tx1 = (std::max)(tx1, lines[li].x1);
            note_inner(lines[li]);
        }
    }
    if (looks_like_code_listing(lines, start, end)) {
        return std::nullopt;
    }

    std::vector<double> boundaries = table_column_boundaries(lines, start, end, gsize);
    if (boundaries.empty()) {
        return std::nullopt;
    }
    const std::size_t ncol = boundaries.size() + 1U;
    if (ncol > kMaxTableColumns) {
        return std::nullopt;
    }
    const double switch_gap = (std::max)(kMinColSepGapPt, kMinColSepGapScale * gsize);

    std::vector<std::vector<CellSlice>> slices(n);
    for (std::size_t li = start; li < end; ++li) {
        assign_row_cells(lines[li], boundaries, switch_gap, slices[li - start]);
    }

    const auto filled_count = [&slices](std::size_t r) noexcept {
        std::size_t f = 0U;
        for (const CellSlice& c : slices[r]) {
            if (c.present) {
                ++f;
            }
        }
        return f;
    };

    if (n == 2U && ncol < 3U) {
        return std::nullopt;
    }

    // Two long-prose columns are a page layout, not a table.
    if (ncol == 2U) {
        double len[2] = {0.0, 0.0};
        std::size_t cnt[2] = {0U, 0U};
        for (std::size_t r = 0U; r < n; ++r) {
            for (std::size_t c = 0U; c < 2U; ++c) {
                if (slices[r][c].present) {
                    len[c] += static_cast<double>(
                        range_glyphs(lines[start + r], slices[r][c].first, slices[r][c].last));
                    ++cnt[c];
                }
            }
        }
        const bool wide0 =
            cnt[0] > 0U && len[0] / static_cast<double>(cnt[0]) > kTwoColProseMaxGlyphs;
        const bool wide1 =
            cnt[1] > 0U && len[1] / static_cast<double>(cnt[1]) > kTwoColProseMaxGlyphs;
        if (wide0 && wide1) {
            return std::nullopt;
        }
    }

    std::size_t multi = 0U;
    for (std::size_t r = 0U; r < n; ++r) {
        if (filled_count(r) >= 2U) {
            ++multi;
        }
    }
    if (multi < 2U) {
        return std::nullopt;
    }

    TableCandidate table;
    table.first_line = start;
    table.last_line = end - 1U;
    if (!render) {
        return table;
    }

    const auto predom_bold_cols = [&lines, &slices, start, ncol](std::size_t r) noexcept {
        std::size_t cols = 0U;
        for (std::size_t c = 0U; c < ncol; ++c) {
            if (!slices[r][c].present) {
                continue;
            }
            std::size_t total = 0U;
            std::size_t bold = 0U;
            for (std::size_t i = slices[r][c].first;
                 i <= slices[r][c].last && i < lines[start + r].chars.size(); ++i) {
                const CharInfo& ch = lines[start + r].chars[i];
                if (ch.space) {
                    continue;
                }
                ++total;
                if (ch.bold) {
                    ++bold;
                }
            }
            if (total > 0U && bold * 2U > total) {
                ++cols;
            }
        }
        return cols;
    };

    std::size_t split = 0U;
    for (std::size_t r = 1U; r + 1U < n; ++r) {
        if (predom_bold_cols(r) >= 2U) {
            split = r;
            break;
        }
    }
    if (split >= 1U && split + 1U < n) {
        std::vector<double> head = table_column_boundaries(lines, start, start + split, gsize);
        std::vector<double> rich = table_column_boundaries(lines, start + split, end, gsize);
        if (head.empty()) {
            head = boundaries;
        }
        if (rich.empty()) {
            rich = boundaries;
        }
        table.cells = build_table_grid(lines, start, start + split, head, inner_left, tx0, col_tol,
                                       /*nested=*/false, table.align);
        ExtraTableGrid extra_grid;
        extra_grid.first_line = start + split;
        extra_grid.last_line = end - 1U;
        extra_grid.cells = build_table_grid(lines, start + split, end, rich, inner_left, tx0,
                                            col_tol, /*nested=*/true, extra_grid.align);
        table.extra.push_back(std::move(extra_grid));
    } else {
        table.cells = build_table_grid(lines, start, end, boundaries, inner_left, tx0, col_tol,
                                       /*nested=*/false, table.align);
    }
    return table;
}

bool region_looks_tabular(std::span<const CharInfo* const> chars) {
    std::vector<Line> lines = cluster_lines(chars);
    if (lines.size() < 2U) {
        return false;
    }
    for (Line& ln : lines) {
        (void)plan_line(ln);
    }
    std::size_t covered = 0U;
    std::size_t i = 0U;
    while (i < lines.size()) {
        if (lines[i].segments.size() >= 2U && !lines[i].mono_majority) {
            if (auto t = try_table(lines, i, lines.size(), /*render=*/false, /*page_width=*/0.0)) {
                covered += t->last_line - t->first_line + 1U;
                i = t->last_line + 1U;
                continue;
            }
        }
        ++i;
    }
    return covered * 2U > lines.size();
}

}  // namespace pdf2md::layout
