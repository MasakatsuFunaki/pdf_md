#include "layout_internal.h"

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

// Degenerate font-matrix fallback and column-boundary tolerances.
constexpr double kMinReportedFontHeightRatio = 0.5;
constexpr double kMinColSepGapPt = 6.0;
constexpr double kMinColSepGapScale = 0.7;
constexpr double kMinColTolPt = 3.0;
constexpr double kColTolScale = 0.4;
constexpr size_t kMinCleanGutterSupport = 2;
constexpr size_t kMajorityFractionDen = 3;

// Header, spanning, and continuation-folding thresholds.
constexpr double kTightHeaderPitchRatio = 0.82;
constexpr size_t kHeaderPeakLookahead = 4;
constexpr double kHeaderPeakMinRatio = 1.15;
constexpr double kWrappedHeaderMaxGapScale = 1.6;
constexpr double kSpanningGutterMinScale = 1.3;
constexpr double kSpanningAdjWindowScale = 2.0;
constexpr double kCamelAdjPitchScale = 1.8;
constexpr double kCamelAdjSizeScale = 2.5;
constexpr double kColFillMinRatio = 0.55;
constexpr size_t kMinWideGridCols = 4;
constexpr double kCol0NearInnerScale = 6.0;
constexpr double kFragNearBoundaryScale = 2.5;
constexpr double kSubPitchLabelScale = 0.65;

// Table candidate extent and rejection gates.
constexpr double kTableRowMaxGapScale = 2.0;
constexpr double kCaptionCenterTolRatio = 0.03;
constexpr double kSingleSegMaxWidthRatio = 0.55;
constexpr double kOversizedLeadRatio = 1.15;
constexpr size_t kMaxTableColumns = 20;
constexpr double kTwoColProseMaxGlyphs = 18.0;

struct CellSlice {
    size_t first = 0;
    size_t last = 0;
    bool present = false;
};

// A size for table geometry robust to degenerate font matrices.
double robust_line_size(const Line& line) {
    std::vector<double> heights;
    heights.reserve(line.chars.size());
    double maxh = 0.0;
    for (const CharInfo& c : line.chars) {
        if (!c.space && c.top > c.bottom) {
            const double h = c.top - c.bottom;
            heights.push_back(h);
            maxh = std::max(maxh, h);
        }
    }
    const double medh = median(std::move(heights));
    if (line.size >= kMinReportedFontHeightRatio * medh) {
        return std::max(line.size, kMinFontSizePt);
    }
    return std::max({maxh, medh, kMinFontSizePt});
}

size_t range_glyphs(const Line& line, size_t lo, size_t hi) noexcept {
    size_t n = 0;
    for (size_t i = lo; i <= hi && i < line.chars.size(); ++i) {
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
    auto connector = [](char32_t c) {
        return c == U'_' || c == U'.' || c == U'/' || c == U'-';
    };
    char32_t last = 0;
    char32_t first = 0;
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
std::vector<double> table_column_boundaries(const std::vector<Line>& lines, size_t start,
                                            size_t end, double size) {
    const double min_gap = std::max(kMinColSepGapPt, kMinColSepGapScale * size);
    const double tol = std::max(kMinColTolPt, kColTolScale * size);
    const size_t nrows = end - start;

    // 0 = outside the row's ink, 1 = clear column gap, 2 = blocked (cuts a token).
    auto classify = [min_gap](const Line& ln, double x) -> int {
        const CharInfo* prev = nullptr;
        for (const CharInfo& c : ln.chars) {
            if (c.space) {
                continue;
            }
            if (prev && x > prev->right && x < c.left) {
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
    tokens.reserve(nrows * 4);
    cand.reserve(nrows * 3);
    for (size_t li = start; li < end; ++li) {
        const CharInfo* prev = nullptr;
        double tlo = 0;
        double thi = 0;
        for (const CharInfo& c : lines[li].chars) {
            if (c.space) {
                continue;
            }
            if (prev && c.left - prev->right >= min_gap) {
                cand.push_back((prev->right + c.left) / 2.0);
                tokens.emplace_back(tlo, thi);
                tlo = c.left;
            } else if (!prev) {
                tlo = c.left;
            }
            thi = c.right;
            prev = &c;
        }
        if (prev) {
            tokens.emplace_back(tlo, thi);
        }
    }
    if (cand.empty()) {
        return {};
    }
    std::sort(cand.begin(), cand.end());

    std::vector<std::pair<double, size_t>> accepted;
    size_t i = 0;
    while (i < cand.size()) {
        size_t j = i;
        double sum = 0;
        while (j < cand.size() && cand[j] - cand[i] <= tol) {
            sum += cand[j];
            ++j;
        }
        const double x = sum / static_cast<double>(j - i);
        size_t support = 0;
        size_t cut = 0;
        for (size_t li = start; li < end; ++li) {
            const int k = classify(lines[li], x);
            if (k == 1) {
                ++support;
            } else if (k == 2) {
                ++cut;
            }
        }
        const bool clean = support >= kMinCleanGutterSupport && cut == 0;
        const bool strong = support * kMajorityFractionDen >= nrows &&
                            cut * kMajorityFractionDen <= nrows && support > cut;
        if ((clean || strong) && (accepted.empty() || x - accepted.back().first > tol)) {
            accepted.emplace_back(x, cut);
        }
        i = j;
    }

    auto real_column_between = [&tokens](double lo, double hi) {
        for (const auto& [tl, tr] : tokens) {
            if (tl > lo && tr < hi) {
                return true;
            }
        }
        return false;
    };

    std::vector<double> boundaries;
    i = 0;
    while (i < accepted.size()) {
        size_t j = i;
        while (j + 1 < accepted.size() &&
               !real_column_between(accepted[j].first, accepted[j + 1].first)) {
            ++j;
        }
        size_t best = i;
        for (size_t k = i; k <= j; ++k) {
            if (accepted[k].second < accepted[best].second) {
                best = k;
            }
        }
        boundaries.push_back(accepted[best].first);
        i = j + 1;
    }
    return boundaries;
}

void assign_row_cells(const Line& ln, std::span<const double> boundaries, double switch_gap,
                      std::vector<CellSlice>& ranges) {
    ranges.assign(boundaries.size() + 1, CellSlice{});
    size_t curcol = 0;
    bool have_col = false;
    const CharInfo* prev = nullptr;
    for (size_t i = 0; i < ln.chars.size(); ++i) {
        const CharInfo& c = ln.chars[i];
        if (c.space) {
            continue;
        }
        const double center = (c.left + c.right) / 2.0;
        const size_t col = static_cast<size_t>(
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

std::vector<std::vector<TableCell>> build_table_grid(const std::vector<Line>& lines, size_t start,
                                                     size_t end,
                                                     std::span<const double> boundaries,
                                                     double inner_left, double tx0, double col_tol,
                                                     bool nested) {
    const size_t n = end - start;
    const size_t ncol = boundaries.size() + 1;
    std::vector<double> gsizes;
    gsizes.reserve(n);
    for (size_t li = start; li < end; ++li) {
        gsizes.push_back(robust_line_size(lines[li]));
    }
    const double gsize = std::max(median(std::move(gsizes)), kMinFontSizePt);
    const double switch_gap = std::max(kMinColSepGapPt, kMinColSepGapScale * gsize);

    auto inner_continuation = [inner_left, col_tol](const Line& ln) {
        return ln.segments.size() < 2 && inner_left < kCoordSentinel &&
               ln.x0 >= inner_left - col_tol;
    };

    std::vector<std::vector<CellSlice>> slices(n);
    for (size_t li = start; li < end; ++li) {
        assign_row_cells(lines[li], boundaries, switch_gap, slices[li - start]);
    }

    // 2. Header: fold leading lines that are stacked tighter than the row pitch.
    std::vector<double> gaps;
    gaps.reserve(n);
    for (size_t li = start + 1; li < end; ++li) {
        gaps.push_back(lines[li - 1].baseline - lines[li].baseline);
    }
    const double pitch = std::max(median(gaps), kMinFontSizePt);
    auto is_new_row = [&slices, ncol](size_t r) {
        if (!slices[r][0].present) {
            return false;
        }
        for (size_t c = 1; c < ncol; ++c) {
            if (slices[r][c].present) {
                return true;
            }
        }
        return false;
    };
    size_t first_data = n;
    for (size_t r = 1; r < n; ++r) {
        if (is_new_row(r)) {
            first_data = r;
            break;
        }
    }
    size_t hdr = 1;
    for (size_t g = 0; g < gaps.size(); ++g) {
        if (gaps[g] < kTightHeaderPitchRatio * pitch) {
            hdr = g + 2;
        } else {
            break;
        }
    }
    if (!slices[0][0].present) {
        size_t c0 = 0;
        while (c0 < n && !slices[c0][0].present) {
            ++c0;
        }
        if (c0 > hdr) {
            hdr = c0;
        }
    }
    const size_t look = std::min(gaps.size(), kHeaderPeakLookahead);
    size_t peak = 0;
    for (size_t g = 0; g < look; ++g) {
        if (gaps[g] > gaps[peak]) {
            peak = g;
        }
    }
    if (peak + 1 > hdr && peak + 1 < n) {
        const double below = peak + 1 < gaps.size() ? gaps[peak + 1] : gaps[peak];
        if (gaps[peak] >= kHeaderPeakMinRatio * below) {
            hdr = peak + 1;
        }
    }
    if (hdr > first_data) {
        hdr = std::max<size_t>(first_data, 1);
    }
    if (hdr >= n) {
        hdr = 1;
    }

    auto lone_alpha_header_cont = [&lines, &slices, start, n, ncol](size_t r) -> bool {
        if (r == 0 || r >= n || slices[r][0].present) {
            return false;
        }
        size_t filled = 0;
        size_t only = 0;
        for (size_t c = 0; c < ncol; ++c) {
            if (slices[r][c].present) {
                ++filled;
                only = c;
            }
        }
        if (filled != 1 || only == 0) {
            return false;
        }
        bool hdr_labels = false;
        for (size_t h = 0; h < r; ++h) {
            if (slices[h][only].present) {
                hdr_labels = true;
                break;
            }
        }
        if (!hdr_labels) {
            return false;
        }
        const Line& ln = lines[start + r];
        const size_t lo = slices[r][only].first;
        const size_t hi = slices[r][only].last;
        size_t letters = 0;
        for (size_t i = lo; i <= hi && i < ln.chars.size(); ++i) {
            const CharInfo& c = ln.chars[i];
            if (c.space) {
                continue;
            }
            if (!is_latin_letter_cp(c.code)) {
                return false;
            }
            ++letters;
        }
        return letters >= 2;
    };
    while (hdr < n && lone_alpha_header_cont(hdr) &&
           lines[start + hdr - 1].baseline - lines[start + hdr].baseline <=
               kWrappedHeaderMaxGapScale * pitch) {
        ++hdr;
    }

    const double gutter_min = kSpanningGutterMinScale * gsize;
    const double adj_win = kSpanningAdjWindowScale * gsize;
    auto spanning = [&lines, &slices, start, ncol, gutter_min](size_t r) -> bool {
        const Line& ln = lines[start + r];
        std::optional<size_t> tgt;
        for (size_t c = 1; c < ncol; ++c) {
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
    for (size_t r = hdr; r < n; ++r) {
        spans[r] = spanning(r) ? 1 : 0;
    }
    for (size_t r = hdr; r < n; ++r) {
        if (!spans[r]) {
            continue;
        }
        const bool confirmed =
            (r > hdr && lines[start + r - 1].baseline - lines[start + r].baseline <= adj_win &&
             spans[r - 1]) ||
            (r + 1 < n && lines[start + r].baseline - lines[start + r + 1].baseline <= adj_win &&
             spans[r + 1]);
        if (!confirmed) {
            continue;
        }
        const Line& ln = lines[start + r];
        std::optional<size_t> target;
        for (size_t c = 1; c < ncol; ++c) {
            if (!slices[r][c].present) {
                continue;
            }
            if (target &&
                ln.chars[slices[r][c].first].left - ln.chars[slices[r][*target].last].right <
                    gutter_min) {
                slices[r][*target].last = slices[r][c].last;
                slices[r][c] = CellSlice{};
                continue;
            }
            target = c;
        }
    }

    auto build_row = [&lines, &slices, start, ncol](size_t r) {
        std::vector<TableCell> row(ncol);
        for (size_t c = 0; c < ncol; ++c) {
            if (slices[r][c].present) {
                row[c] = render_cell_runs(lines[start + r], slices[r][c].first, slices[r][c].last);
            }
        }
        return row;
    };

    std::vector<TableCell> header(ncol);
    for (size_t r = 0; r < hdr; ++r) {
        std::vector<TableCell> hr = build_row(r);
        for (size_t c = 0; c < ncol; ++c) {
            append_cell(header[c], hr[c]);
        }
    }

    std::vector<std::vector<TableCell>> data;
    std::vector<double> data_base;
    data.reserve(n - hdr);
    data_base.reserve(n - hdr);
    for (size_t r = hdr; r < n; ++r) {
        data.push_back(build_row(r));
        data_base.push_back(lines[start + r].baseline);
    }

    // 2b. Fold wrapped continuations back into the row they belong to.
    {
        enum class RowKind { Full, Col0, Inner, Other };
        std::vector<RowKind> kind(data.size(), RowKind::Other);
        for (size_t di = 0; di < data.size(); ++di) {
            const auto& s = slices[hdr + di];
            bool inner = false;
            for (size_t c = 1; c < ncol; ++c) {
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
        std::vector<char> folded_wraps;
        std::vector<TableCell> pending;
        folded.reserve(data.size());
        folded_base.reserve(data.size());
        folded_wraps.reserve(data.size());

        auto flush_pending = [&pending](std::vector<TableCell>& row) {
            if (pending.empty()) {
                return false;
            }
            TableCell joined;
            for (auto& frag : pending) {
                append_cell(joined, frag, label_join_tight(joined, frag));
            }
            append_cell(joined, row[0], label_join_tight(joined, row[0]));
            row[0] = std::move(joined);
            pending.clear();
            return true;
        };
        auto push_row = [&folded, &folded_base, &folded_wraps, &data, &data_base](size_t di,
                                                                                   bool wraps) {
            folded.push_back(std::move(data[di]));
            folded_base.push_back(data_base[di]);
            folded_wraps.push_back(wraps ? 1 : 0);
        };

        for (size_t di = 0; di < data.size(); ++di) {
            if (kind[di] == RowKind::Inner) {
                if (!pending.empty()) {
                    const bool w = flush_pending(data[di]);
                    push_row(di, w);
                } else if (!folded.empty()) {
                    for (size_t c = 1; c < ncol; ++c) {
                        if (!data[di][c].empty()) {
                            append_cell(folded.back()[c], data[di][c]);
                        }
                    }
                } else {
                    push_row(di, false);
                }
                continue;
            }

            if (kind[di] == RowKind::Full && pending.empty() && !folded.empty() && di > 0 &&
                slices[hdr + di - 1][0].present && slices[hdr + di][0].present) {
                const CharInfo& prev_last =
                    lines[start + hdr + di - 1].chars[slices[hdr + di - 1][0].last];
                const CharInfo& frag_first =
                    lines[start + hdr + di].chars[slices[hdr + di][0].first];
                const double gap_up =
                    folded_base.empty() ? kCoordSentinel : folded_base.back() - data_base[di];
                const bool camel = prev_last.code >= U'a' && prev_last.code <= U'z' &&
                                   frag_first.code >= U'A' && frag_first.code <= U'Z' &&
                                   !prev_last.bold && !frag_first.bold;
                const bool adjacent =
                    gap_up <= std::max(kCamelAdjPitchScale * pitch, kCamelAdjSizeScale * gsize);
                const bool fills_col =
                    boundaries[0] > tx0 &&
                    (prev_last.right - tx0) >= kColFillMinRatio * (boundaries[0] - tx0);
                bool bare_label = false;
                if (ncol >= kMinWideGridCols) {
                    bool typed = false;
                    for (size_t c = 1; c + 1 < ncol; ++c) {
                        if (!data[di][c].empty()) {
                            typed = true;
                        }
                    }
                    bare_label = !typed;
                }
                auto column_wrap_continuation = [&lines, &slices, boundaries, start, hdr,
                                                 di](size_t c) -> bool {
                    if (!slices[hdr + di - 1][c].present || !slices[hdr + di][c].present) {
                        return false;
                    }
                    const CharInfo& above_last =
                        lines[start + hdr + di - 1].chars[slices[hdr + di - 1][c].last];
                    const CharInfo& frag_first_c =
                        lines[start + hdr + di].chars[slices[hdr + di][c].first];
                    const bool camel_c = above_last.code >= U'a' && above_last.code <= U'z' &&
                                         frag_first_c.code >= U'A' && frag_first_c.code <= U'Z' &&
                                         !above_last.bold && !frag_first_c.bold;
                    const double left_c = boundaries[c - 1];
                    const bool fills_c =
                        boundaries[c] > left_c &&
                        (above_last.right - left_c) >= kColFillMinRatio * (boundaries[c] - left_c);
                    return camel_c && fills_c;
                };
                bool typed_collision = false;
                for (size_t c = 1; c + 1 < ncol; ++c) {
                    if (!folded.back()[c].empty() && !data[di][c].empty() &&
                        !column_wrap_continuation(c)) {
                        typed_collision = true;
                    }
                }
                if (camel && adjacent && (fills_col || bare_label) && !typed_collision) {
                    append_cell(folded.back()[0], data[di][0], /*tight=*/true);
                    for (size_t c = 1; c < ncol; ++c) {
                        if (!data[di][c].empty()) {
                            append_cell(folded.back()[c], data[di][c],
                                        c + 1 < ncol && column_wrap_continuation(c));
                        }
                    }
                    folded_wraps.back() = 1;
                    continue;
                }
            }

            if (kind[di] == RowKind::Col0) {
                const bool next_inner = di + 1 < data.size() && kind[di + 1] == RowKind::Inner;
                const bool prev_value =
                    di > 0 && (kind[di - 1] == RowKind::Full || kind[di - 1] == RowKind::Inner);
                const double gap_up =
                    folded.empty() ? kCoordSentinel : folded_base.back() - data_base[di];
                const double gap_dn =
                    di + 1 < data.size() ? data_base[di] - data_base[di + 1] : kCoordSentinel;
                double prev_rx = -1;
                bool prev_word_end = false;
                if (di > 0 && slices[hdr + di - 1][0].present) {
                    const CharInfo& pc =
                        lines[start + hdr + di - 1].chars[slices[hdr + di - 1][0].last];
                    prev_rx = pc.right;
                    prev_word_end =
                        is_latin_letter_cp(pc.code) || (pc.code >= U'0' && pc.code <= U'9');
                }
                const bool prev_fills_col =
                    prev_rx >= 0 && boundaries[0] > tx0 &&
                    (prev_rx - tx0) >= kColFillMinRatio * (boundaries[0] - tx0);
                const bool prev_wrapped =
                    (!folded.empty() && folded_wraps.back()) ||
                    (prev_rx >= 0 && prev_word_end && inner_left < kCoordSentinel &&
                     inner_left - prev_rx <= kCol0NearInnerScale * gsize && prev_fills_col);

                bool labelish = false;
                if (slices[hdr + di][0].present) {
                    const Line& cur_ln = lines[start + hdr + di];
                    for (size_t i = slices[hdr + di][0].first;
                         i <= slices[hdr + di][0].last && i < cur_ln.chars.size(); ++i) {
                        const char32_t cp = cur_ln.chars[i].code;
                        if (is_latin_letter_cp(cp) || (cp >= U'0' && cp <= U'9')) {
                            labelish = true;
                            break;
                        }
                    }
                }
                if (!next_inner && prev_value && !folded.empty() &&
                    gap_up <= kCamelAdjPitchScale * pitch && prev_wrapped && labelish) {
                    append_cell(folded.back()[0], data[di][0],
                                label_join_tight(folded.back()[0], data[di][0]));
                    folded_wraps.back() = 1;
                    continue;
                }

                double frag_rx = -1;
                char32_t frag_last = 0;
                if (slices[hdr + di][0].present) {
                    const CharInfo& fc =
                        lines[start + hdr + di].chars[slices[hdr + di][0].last];
                    frag_rx = fc.right;
                    frag_last = fc.code;
                }
                const bool frag_wraps =
                    frag_last == U'_' || frag_last == U'.' || frag_last == U'/' ||
                    (frag_rx >= 0 && boundaries[0] - frag_rx <= kFragNearBoundaryScale * gsize);
                const bool next_full_sandwich = di + 2 < data.size() &&
                                                kind[di + 1] == RowKind::Full &&
                                                kind[di + 2] == RowKind::Col0 && frag_wraps;
                if ((next_inner || next_full_sandwich) && gap_dn <= kCamelAdjPitchScale * pitch) {
                    pending.push_back(std::move(data[di][0]));
                    continue;
                }
            }
            const bool w = flush_pending(data[di]);
            push_row(di, w);
        }
        if (!pending.empty() && !folded.empty()) {
            for (auto& frag : pending) {
                append_cell(folded.back()[0], frag, label_join_tight(folded.back()[0], frag));
            }
        }
        data = std::move(folded);
        data_base = std::move(folded_base);
    }

    // 3. Fold a lone-column label sitting at a sub-pitch offset into its nearest neighbour.
    for (size_t r = 0; r < data.size();) {
        size_t f = 0;
        size_t fc = 0;
        for (size_t c = 0; c < ncol; ++c) {
            if (!data[r][c].empty()) {
                ++f;
                fc = c;
            }
        }
        if (f == 1) {
            const double gp = r > 0 ? data_base[r - 1] - data_base[r] : kCoordSentinel;
            const double gn =
                r + 1 < data.size() ? data_base[r] - data_base[r + 1] : kCoordSentinel;
            if (std::min(gp, gn) < kSubPitchLabelScale * pitch) {
                const size_t tgt = gp <= gn ? r - 1 : r + 1;
                TableCell merged = std::move(data[r][fc]);
                append_cell(merged, data[tgt][fc]);
                data[tgt][fc] = std::move(merged);
                data.erase(data.begin() + static_cast<std::ptrdiff_t>(r));
                data_base.erase(data_base.begin() + static_cast<std::ptrdiff_t>(r));
                continue;
            }
        }
        ++r;
    }

    std::vector<std::vector<TableCell>> cells;
    cells.reserve(1 + data.size());
    cells.push_back(std::move(header));
    for (auto& row : data) {
        cells.push_back(std::move(row));
    }

    // Prune phantom columns left completely empty across every row (header and
    // body) after spanning-cell and continuation merges, provided at least two
    // populated columns remain.
    std::vector<size_t> active_cols;
    active_cols.reserve(ncol);
    for (size_t c = 0; c < ncol; ++c) {
        const bool col_has_ink = std::any_of(
            cells.cbegin(), cells.cend(),
            [c](const std::vector<TableCell>& row) { return c < row.size() && !row[c].empty(); });
        if (col_has_ink) {
            active_cols.push_back(c);
        }
    }
    if (active_cols.size() >= 2 && active_cols.size() < ncol) {
        for (auto& row : cells) {
            std::vector<TableCell> compacted;
            compacted.reserve(active_cols.size());
            for (size_t c : active_cols) {
                compacted.push_back(std::move(row[c]));
            }
            row = std::move(compacted);
        }
    }
    return cells;
}

}  // namespace

// Renders a contiguous glyph range of a line into styled runs, preserving
// superscripts/subscripts, bold highlights, and code styling.
TableCell render_cell_runs(const Line& line, size_t lo, size_t hi) {
    if (line.chars.empty() || lo > hi || lo >= line.chars.size()) {
        return {};
    }
    const size_t limit = std::min(hi, line.chars.size() - 1);
    while (lo <= limit && line.chars[lo].space) {
        ++lo;
    }
    if (lo > limit) {
        return {};
    }
    size_t trimmed_hi = limit;
    while (trimmed_hi > lo && line.chars[trimmed_hi].space) {
        --trimmed_hi;
    }

    Line sub;
    sub.chars.assign(line.chars.cbegin() + static_cast<std::ptrdiff_t>(lo),
                     line.chars.cbegin() + static_cast<std::ptrdiff_t>(trimmed_hi) + 1);

    // Collapse runs consisting solely of placeholder dashes into a single dash.
    {
        size_t nonspace = 0;
        size_t dashes = 0;
        for (const CharInfo& c : sub.chars) {
            if (c.space) {
                continue;
            }
            ++nonspace;
            if (is_dashy_cp(c.code)) {
                ++dashes;
            }
        }
        if (nonspace >= 2 && dashes == nonspace) {
            for (const CharInfo& c : sub.chars) {
                if (!c.space) {
                    const CharInfo keep = c;
                    sub.chars.assign(1, keep);
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
    if (line.word_gap > 0) {
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

std::optional<TableCandidate> try_table(const std::vector<Line>& lines, size_t start,
                                        size_t region_lines, bool render, double page_width) {
    const double seed_size = robust_line_size(lines[start]);
    const double gap_limit = kTableRowMaxGapScale * seed_size;
    const double col_tol = std::max(kMinColTolPt, kColTolScale * seed_size);

    double inner_left = kCoordSentinel;
    auto note_inner = [&inner_left](const Line& ln) {
        if (ln.segments.size() >= 2) {
            inner_left = std::min(inner_left, ln.chars[ln.segments[1].first].left);
        }
    };
    auto inner_continuation = [&inner_left, col_tol](const Line& ln) {
        return ln.segments.size() < 2 && inner_left < kCoordSentinel &&
               ln.x0 >= inner_left - col_tol;
    };

    size_t end = start + 1;
    double tx0 = lines[start].x0;
    double tx1 = lines[start].x1;
    note_inner(lines[start]);

    const double page_center = page_width / 2.0;
    auto caption_shaped = [&inner_left, &tx0, col_tol, page_width, page_center](const Line& ln) {
        if (ln.segments.size() >= 2 || inner_left >= kCoordSentinel || page_width <= 0) {
            return false;
        }
        size_t nonspace = 0;
        size_t bold = 0;
        for (const CharInfo& c : ln.chars) {
            if (!c.space) {
                ++nonspace;
                if (c.bold) {
                    ++bold;
                }
            }
        }
        if (nonspace == 0 || bold * 2 <= nonspace) {
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
        if (lines[end - 1].baseline - cur.baseline > gap_limit) {
            break;
        }
        if (caption_shaped(cur)) {
            break;
        }
        if (cur.segments.size() < 2 && (cur.x1 - cur.x0) > kSingleSegMaxWidthRatio * (tx1 - tx0) &&
            !inner_continuation(cur)) {
            break;
        }
        if (cur.segments.size() >= 2) {
            tx0 = std::min(tx0, cur.x0);
            tx1 = std::max(tx1, cur.x1);
            note_inner(cur);
        }
        ++end;
    }

    auto col0_fragment = [&inner_left, &tx0, col_tol](const Line& ln) {
        return ln.segments.size() < 2 && inner_left < kCoordSentinel && ln.x0 <= tx0 + col_tol &&
               ln.x1 <= inner_left - col_tol;
    };
    auto has_inner_ink = [&inner_continuation](const Line& ln) {
        return ln.segments.size() >= 2 || inner_continuation(ln);
    };
    while (end - 1 > start && lines[end - 1].segments.size() < 2) {
        const Line& last = lines[end - 1];
        if (col0_fragment(last)) {
            break;
        }
        if (inner_continuation(last) && has_inner_ink(lines[end - 2])) {
            break;
        }
        --end;
    }

    size_t n = end - start;
    if (n < 2) {
        return std::nullopt;
    }

    auto grid_size = [&lines](size_t s, size_t e) {
        std::vector<double> sz;
        sz.reserve(e - s);
        for (size_t li = s; li < e; ++li) {
            sz.push_back(robust_line_size(lines[li]));
        }
        return std::max(median(std::move(sz)), kMinFontSizePt);
    };
    double gsize = grid_size(start, end);

    bool trimmed = false;
    while (end - start > 2 && robust_line_size(lines[start]) > kOversizedLeadRatio * gsize) {
        ++start;
        gsize = grid_size(start, end);
        trimmed = true;
    }
    n = end - start;
    if (n < 2) {
        return std::nullopt;
    }
    if (trimmed) {
        inner_left = kCoordSentinel;
        tx0 = lines[start].x0;
        tx1 = lines[start].x1;
        for (size_t li = start; li < end; ++li) {
            tx0 = std::min(tx0, lines[li].x0);
            tx1 = std::max(tx1, lines[li].x1);
            note_inner(lines[li]);
        }
    }

    std::vector<double> boundaries = table_column_boundaries(lines, start, end, gsize);
    if (boundaries.empty()) {
        return std::nullopt;
    }
    const size_t ncol = boundaries.size() + 1;
    if (ncol > kMaxTableColumns) {
        return std::nullopt;
    }
    const double switch_gap = std::max(kMinColSepGapPt, kMinColSepGapScale * gsize);

    std::vector<std::vector<CellSlice>> slices(n);
    for (size_t li = start; li < end; ++li) {
        assign_row_cells(lines[li], boundaries, switch_gap, slices[li - start]);
    }

    auto filled_count = [&slices](size_t r) {
        size_t f = 0;
        for (const CellSlice& c : slices[r]) {
            if (c.present) {
                ++f;
            }
        }
        return f;
    };

    if (n == 2 && ncol < 3) {
        return std::nullopt;
    }

    // Two long-prose columns are a page layout, not a table.
    if (ncol == 2) {
        double len[2] = {0, 0};
        size_t cnt[2] = {0, 0};
        for (size_t r = 0; r < n; ++r) {
            for (size_t c = 0; c < 2; ++c) {
                if (slices[r][c].present) {
                    len[c] += static_cast<double>(
                        range_glyphs(lines[start + r], slices[r][c].first, slices[r][c].last));
                    ++cnt[c];
                }
            }
        }
        const bool wide0 = cnt[0] > 0 && len[0] / static_cast<double>(cnt[0]) > kTwoColProseMaxGlyphs;
        const bool wide1 = cnt[1] > 0 && len[1] / static_cast<double>(cnt[1]) > kTwoColProseMaxGlyphs;
        if (wide0 && wide1) {
            return std::nullopt;
        }
    }

    size_t multi = 0;
    for (size_t r = 0; r < n; ++r) {
        if (filled_count(r) >= 2) {
            ++multi;
        }
    }
    if (multi < 2) {
        return std::nullopt;
    }

    TableCandidate table;
    table.first_line = start;
    table.last_line = end - 1;
    if (!render) {
        return table;
    }

    auto predom_bold_cols = [&lines, &slices, start, ncol](size_t r) {
        size_t cols = 0;
        for (size_t c = 0; c < ncol; ++c) {
            if (!slices[r][c].present) {
                continue;
            }
            size_t total = 0;
            size_t bold = 0;
            for (size_t i = slices[r][c].first;
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
            if (total > 0 && bold * 2 > total) {
                ++cols;
            }
        }
        return cols;
    };

    size_t split = 0;
    for (size_t r = 1; r + 1 < n; ++r) {
        if (predom_bold_cols(r) >= 2) {
            split = r;
            break;
        }
    }
    if (split >= 1 && split + 1 < n) {
        std::vector<double> head = table_column_boundaries(lines, start, start + split, gsize);
        std::vector<double> rich = table_column_boundaries(lines, start + split, end, gsize);
        if (head.empty()) {
            head = boundaries;
        }
        if (rich.empty()) {
            rich = boundaries;
        }
        table.cells = build_table_grid(lines, start, start + split, head, inner_left, tx0, col_tol,
                                       /*nested=*/false);
        table.extra.push_back(build_table_grid(lines, start + split, end, rich, inner_left, tx0,
                                               col_tol, /*nested=*/true));
    } else {
        table.cells = build_table_grid(lines, start, end, boundaries, inner_left, tx0, col_tol,
                                       /*nested=*/false);
    }
    return table;
}

bool region_looks_tabular(std::span<const CharInfo* const> chars) {
    const std::vector<Line> lines = build_lines(chars);
    if (lines.size() < 2) {
        return false;
    }
    size_t covered = 0;
    size_t i = 0;
    while (i < lines.size()) {
        if (lines[i].segments.size() >= 2 && !lines[i].mono_majority) {
            if (auto t = try_table(lines, i, lines.size(), /*render=*/false, /*page_width=*/0.0)) {
                covered += t->last_line - t->first_line + 1;
                i = t->last_line + 1;
                continue;
            }
        }
        ++i;
    }
    return covered * 2 > lines.size();
}

}  // namespace detail
}  // namespace pdf2md
