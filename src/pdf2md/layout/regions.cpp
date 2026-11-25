#include "pdf2md/layout/regions.h"

#include "pdf2md/layout/line_cluster.h"
#include "pdf2md/layout/math.h"
#include "pdf2md/layout/tables_text.h"
#include "pdf2md/model/geometry.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace pdf2md::layout {

using model::Interval;
using model::find_gap_cuts;
using model::kCoordSentinel;
using model::kCoordSentinelThreshold;
using model::kDefaultBodySizePt;
using model::median;

namespace {

constexpr int kMaxXyCutDepth = 10;
constexpr double kLineEstimateClusterScale = 0.5;
constexpr std::size_t kMinSoftGapIntervals = 4U;
constexpr std::size_t kMinSoftGapSideCount = 3U;
constexpr std::size_t kSoftGapSideDivisor = 25U;
constexpr double kHorizontalCutGapScale = 2.6;
constexpr std::size_t kBridgeTolLineDivisor = 12U;
constexpr double kMinColumnCutGapPt = 14.0;
constexpr double kColumnCutGapScale = 1.7;

// 1-col <-> 2-col horizontal transition cut thresholds.
constexpr double kMinTransitionRegionWidthScale = 26.0;
constexpr std::size_t kMinTransitionRegionLines = 5U;
constexpr double kCenterSpanMarginRatio = 0.30;
constexpr std::size_t kTransitionBridgeDivisor = 24U;
constexpr double kGutterEdgeMarginRatio = 0.25;
constexpr double kGutterStraddlePadScale = 0.35;
constexpr double kOppositeColMaxDyScale = 2.2;
constexpr std::size_t kMinTwoColSideSegments = 2U;
constexpr double kMinTransitionBaselineGapScale = 1.15;
constexpr int kBandNeutral = 0;
constexpr int kBandFullWidth = 1;
constexpr int kBandTwoCol = 2;

double region_median_size(std::span<const CharInfo* const> chars) {
    std::vector<double> sizes;
    sizes.reserve(chars.size());
    for (const CharInfo* c : chars) {
        if (!c->space && c->size > 0.0) {
            sizes.push_back(c->size);
        }
    }
    const double m = median(std::move(sizes));
    return m > 0.0 ? m : kDefaultBodySizePt;
}

std::size_t estimate_line_count(std::span<const CharInfo* const> chars, double size) {
    std::vector<double> ys;
    ys.reserve(chars.size());
    for (const CharInfo* c : chars) {
        if (!c->space) {
            ys.push_back(c->y);
        }
    }
    std::sort(ys.begin(), ys.end());
    std::size_t n = 0U;
    double last = -kCoordSentinel;
    for (const double y : ys) {
        if (y - last > kLineEstimateClusterScale * size) {
            ++n;
        }
        last = y;
    }
    return n;
}

std::vector<double> find_soft_gap_cuts(std::span<const Interval> ivals, double min_gap,
                                       std::size_t bridge_tol) {
    std::vector<double> cuts;
    if (ivals.size() < kMinSoftGapIntervals) {
        return cuts;
    }

    struct Event {
        double x = 0.0;
        int d = 0;
    };
    std::vector<Event> events;
    events.reserve(ivals.size() * 2U);
    for (const Interval& iv : ivals) {
        events.push_back({iv.lo, +1});
        events.push_back({iv.hi, -1});
    }
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) noexcept {
        if (a.x != b.x) {
            return a.x < b.x;
        }
        return a.d > b.d;  // starts first: coverage never dips spuriously
    });

    const double global_lo = events.front().x;
    const double global_hi = events.back().x;
    std::size_t cover = 0U;
    bool in_span = false;
    double span_start = 0.0;
    std::vector<std::pair<double, double>> spans;
    for (const Event& e : events) {
        if (e.d > 0) {
            ++cover;
        } else if (e.d < 0) {
            --cover;
        }
        if (!in_span && cover <= bridge_tol) {
            in_span = true;
            span_start = e.x;
        } else if (in_span && cover > bridge_tol) {
            in_span = false;
            spans.emplace_back(span_start, e.x);
        }
    }

    const std::size_t min_side =
        (std::max)(kMinSoftGapSideCount, ivals.size() / kSoftGapSideDivisor);
    for (const auto& [lo, hi] : spans) {
        if (hi - lo < min_gap) {
            continue;
        }
        if (lo <= global_lo || hi >= global_hi) {
            continue;  // region edges
        }
        const double cut = (lo + hi) / 2.0;
        std::size_t left = 0U;
        std::size_t right = 0U;
        for (const Interval& iv : ivals) {
            if (iv.hi < cut) {
                ++left;
            } else if (iv.lo > cut) {
                ++right;
            }
        }
        if (left >= min_side && right >= min_side) {
            cuts.push_back(cut);
        }
    }
    return cuts;
}

// Detects horizontal y-cuts separating full-width / wide-table bands from
// 2-column gutter-split bands when inter-band vertical spacing is tighter than
// the unconditional `kHorizontalCutGapScale` threshold.
std::vector<double> find_layout_transition_cuts(std::span<const CharInfo* const> chars,
                                                double size) {
    double reg_x0 = kCoordSentinel;
    double reg_x1 = -kCoordSentinel;
    for (const CharInfo* c : chars) {
        if (c->space) {
            continue;
        }
        reg_x0 = (std::min)(reg_x0, c->left);
        reg_x1 = (std::max)(reg_x1, c->right);
    }
    const double reg_w = reg_x1 - reg_x0;
    if (reg_w < kMinTransitionRegionWidthScale * size) {
        return {};
    }

    std::vector<Line> lines = cluster_lines(chars);
    if (lines.size() < kMinTransitionRegionLines) {
        return {};
    }
    for (Line& ln : lines) {
        (void)plan_line(ln);
    }

    const std::size_t n = lines.size();
    std::vector<char> in_table(n, 0);
    std::vector<std::pair<double, double>> table_xspan(n, {0.0, 0.0});
    for (std::size_t i = 0U; i < n;) {
        if (lines[i].segments.size() >= 2U && !lines[i].mono_majority) {
            if (auto t = try_table(lines, i, n, /*render=*/false, /*page_width=*/0.0)) {
                double tx0 = kCoordSentinel;
                double tx1 = -kCoordSentinel;
                for (std::size_t k = t->first_line; k <= t->last_line; ++k) {
                    tx0 = (std::min)(tx0, lines[k].x0);
                    tx1 = (std::max)(tx1, lines[k].x1);
                }
                for (std::size_t k = t->first_line; k <= t->last_line; ++k) {
                    in_table[k] = 1;
                    table_xspan[k] = {tx0, tx1};
                }
                i = t->last_line + 1U;
                continue;
            }
        }
        ++i;
    }

    const double center_lo = reg_x0 + kCenterSpanMarginRatio * reg_w;
    const double center_hi = reg_x1 - kCenterSpanMarginRatio * reg_w;
    std::vector<Interval> col_ivals;
    col_ivals.reserve(n * 2U);
    for (std::size_t i = 0U; i < n; ++i) {
        if (in_table[i] != 0) {
            continue;
        }
        bool spans_center = false;
        for (const auto& [f, l] : lines[i].segments) {
            if (lines[i].chars[f].left < center_lo && lines[i].chars[l].right > center_hi) {
                spans_center = true;
                break;
            }
        }
        if (spans_center) {
            continue;
        }
        for (const auto& [f, l] : lines[i].segments) {
            col_ivals.push_back({lines[i].chars[f].left, lines[i].chars[l].right});
        }
    }

    const double min_col_gap = (std::max)(kMinColumnCutGapPt, kColumnCutGapScale * size);
    const std::vector<double> cand_x =
        find_soft_gap_cuts(col_ivals, min_col_gap, col_ivals.size() / kTransitionBridgeDivisor);
    if (cand_x.empty()) {
        return {};
    }

    const double gutter_lo = reg_x0 + kGutterEdgeMarginRatio * reg_w;
    const double gutter_hi = reg_x1 - kGutterEdgeMarginRatio * reg_w;
    const double reg_mid = (reg_x0 + reg_x1) * 0.5;
    double cut_x = -kCoordSentinel;
    double best_dist = kCoordSentinel;
    for (const double cx : cand_x) {
        if (cx < gutter_lo || cx > gutter_hi) {
            continue;
        }
        const double dist = std::fabs(cx - reg_mid);
        if (dist < best_dist) {
            best_dist = dist;
            cut_x = cx;
        }
    }
    if (cut_x <= -kCoordSentinelThreshold) {
        return {};
    }

    std::vector<double> line_bot(n, kCoordSentinel);
    std::vector<double> line_top(n, -kCoordSentinel);
    for (std::size_t i = 0U; i < n; ++i) {
        for (const CharInfo& c : lines[i].chars) {
            if (c.space) {
                continue;
            }
            line_bot[i] = (std::min)(line_bot[i], c.bottom);
            line_top[i] = (std::max)(line_top[i], c.top);
        }
    }

    const double pad = kGutterStraddlePadScale * size;
    std::vector<int> band(n, kBandNeutral);
    bool has_full_width = false;
    for (std::size_t i = 0U; i < n; ++i) {
        if (in_table[i] != 0 && table_xspan[i].first < cut_x - min_col_gap * 0.5 &&
            table_xspan[i].second > cut_x + min_col_gap * 0.5) {
            band[i] = kBandFullWidth;
            has_full_width = true;
            continue;
        }
        for (const auto& [f, l] : lines[i].segments) {
            if (lines[i].chars[f].left < cut_x - pad && lines[i].chars[l].right > cut_x + pad) {
                band[i] = kBandFullWidth;
                has_full_width = true;
                break;
            }
        }
    }
    if (!has_full_width) {
        return {};
    }

    std::size_t left_two_col = 0U;
    std::size_t right_two_col = 0U;
    for (std::size_t i = 0U; i < n; ++i) {
        if (band[i] == kBandFullWidth) {
            continue;
        }
        bool has_left = false;
        bool has_right = false;
        for (const auto& [f, l] : lines[i].segments) {
            if (lines[i].chars[l].right <= cut_x + pad) {
                has_left = true;
            } else if (lines[i].chars[f].left >= cut_x - pad) {
                has_right = true;
            }
        }
        if (has_left && has_right) {
            band[i] = kBandTwoCol;
            ++left_two_col;
            ++right_two_col;
            continue;
        }
        if (!has_left && !has_right) {
            continue;
        }
        for (std::size_t j = 0U; j < n; ++j) {
            if (j == i || band[j] == kBandFullWidth) {
                continue;
            }
            if (std::fabs(lines[i].baseline - lines[j].baseline) > kOppositeColMaxDyScale * size) {
                continue;
            }
            const bool opp = has_left ? (lines[j].x0 >= cut_x - pad)
                                      : (lines[j].x1 <= cut_x + pad);
            if (opp) {
                band[i] = kBandTwoCol;
                if (has_left) {
                    ++left_two_col;
                } else {
                    ++right_two_col;
                }
                break;
            }
        }
    }
    if (left_two_col < kMinTwoColSideSegments || right_two_col < kMinTwoColSideSegments) {
        return {};
    }

    // Attach single-sided neutral lines (e.g. a short trailing caption line or
    // uneven column tail) to the vertically closer labeled neighbor.
    for (std::size_t i = 0U; i < n; ++i) {
        if (band[i] != kBandNeutral) {
            continue;
        }
        double up_dist = kCoordSentinel;
        int up_band = kBandNeutral;
        for (std::ptrdiff_t u = static_cast<std::ptrdiff_t>(i) - 1; u >= 0; --u) {
            const auto ui = static_cast<std::size_t>(u);
            if (band[ui] != kBandNeutral) {
                up_dist = std::fabs(lines[ui].baseline - lines[i].baseline);
                up_band = band[ui];
                break;
            }
        }
        double dn_dist = kCoordSentinel;
        int dn_band = kBandNeutral;
        for (std::size_t d = i + 1U; d < n; ++d) {
            if (band[d] != kBandNeutral) {
                dn_dist = std::fabs(lines[i].baseline - lines[d].baseline);
                dn_band = band[d];
                break;
            }
        }
        band[i] = (up_dist <= dn_dist && up_band != kBandNeutral) ? up_band : dn_band;
    }

    struct BandRun {
        std::size_t first = 0U;
        std::size_t last = 0U;
        int kind = kBandNeutral;
    };
    std::vector<BandRun> runs;
    for (std::size_t i = 0U; i < n; ++i) {
        if (runs.empty() || runs.back().kind != band[i]) {
            runs.push_back({i, i, band[i]});
        } else {
            runs.back().last = i;
        }
    }
    if (runs.size() < 2U) {
        return {};
    }

    std::vector<double> cuts;
    for (std::size_t r = 0U; r + 1U < runs.size(); ++r) {
        double upper_bot = kCoordSentinel;
        for (std::size_t k = runs[r].first; k <= runs[r].last; ++k) {
            upper_bot = (std::min)(upper_bot, line_bot[k]);
        }
        double lower_top = -kCoordSentinel;
        for (std::size_t k = runs[r + 1U].first; k <= runs[r + 1U].last; ++k) {
            lower_top = (std::max)(lower_top, line_top[k]);
        }
        const double b_gap = lines[runs[r].last].baseline - lines[runs[r + 1U].first].baseline;
        if (b_gap >= kMinTransitionBaselineGapScale * size && upper_bot > lower_top) {
            cuts.push_back((upper_bot + lower_top) * 0.5);
        }
    }
    return cuts;
}

}  // namespace

void split_region(std::vector<const CharInfo*> chars, int depth,
                  std::vector<std::vector<const CharInfo*>>& out) {
    if (chars.empty()) {
        return;
    }
    if (depth >= kMaxXyCutDepth) {
        out.push_back(std::move(chars));
        return;
    }

    const double size = region_median_size(chars);

    std::vector<Interval> y_ivals;
    y_ivals.reserve(chars.size());
    for (const CharInfo* c : chars) {
        if (!c->space) {
            y_ivals.push_back({c->bottom, c->top});
        }
    }

    std::vector<double> y_cuts = find_gap_cuts(std::move(y_ivals), kHorizontalCutGapScale * size);
    if (y_cuts.empty()) {
        y_cuts = find_layout_transition_cuts(chars, size);
    }
    if (!y_cuts.empty()) {
        std::sort(y_cuts.begin(), y_cuts.end(), std::greater<double>{});
        std::vector<std::vector<const CharInfo*>> parts(y_cuts.size() + 1U);
        for (const CharInfo* c : chars) {
            const double mid = (c->bottom + c->top) / 2.0;
            std::size_t idx = 0U;  // parts ordered top to bottom
            while (idx < y_cuts.size() && mid < y_cuts[idx]) {
                ++idx;
            }
            parts[idx].push_back(c);
        }
        for (auto& part : parts) {
            if (!part.empty()) {
                split_region(std::move(part), depth + 1, out);
            }
        }
        return;
    }

    std::vector<Interval> x_ivals;
    x_ivals.reserve(chars.size());
    for (const CharInfo* c : chars) {
        if (!c->space) {
            x_ivals.push_back({c->left, c->right});
        }
    }

    const std::size_t line_estimate = estimate_line_count(chars, size);
    const std::size_t bridge_tol = line_estimate / kBridgeTolLineDivisor;
    std::vector<double> x_cuts = find_soft_gap_cuts(
        x_ivals, (std::max)(kMinColumnCutGapPt, kColumnCutGapScale * size), bridge_tol);
    if (!x_cuts.empty() && region_looks_tabular(chars)) {
        x_cuts.clear();
    }
    if (!x_cuts.empty()) {
        std::sort(x_cuts.begin(), x_cuts.end());
        std::vector<std::vector<const CharInfo*>> parts(x_cuts.size() + 1U);
        for (const CharInfo* c : chars) {
            const double mid = (c->left + c->right) / 2.0;
            std::size_t idx = 0U;
            while (idx < x_cuts.size() && mid > x_cuts[idx]) {
                ++idx;
            }
            parts[idx].push_back(c);
        }
        for (auto& part : parts) {
            if (!part.empty()) {
                split_region(std::move(part), depth + 1, out);
            }
        }
        return;
    }

    out.push_back(std::move(chars));
}

}  // namespace pdf2md::layout
