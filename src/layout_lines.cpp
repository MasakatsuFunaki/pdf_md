#include "layout_internal.h"

#include "utf8.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace pdf2md {
namespace detail {

namespace {

// Bullet and dash Unicode code points.
constexpr char32_t kMiddleDot = 0x00B7;
constexpr char32_t kEnDash = 0x2013;
constexpr char32_t kEmDash = 0x2014;
constexpr char32_t kBullet = 0x2022;
constexpr char32_t kTriangularBullet = 0x2023;
constexpr char32_t kHyphenBullet = 0x2043;
constexpr char32_t kBulletOperator = 0x2219;
constexpr char32_t kBlackSquare = 0x25A0;
constexpr char32_t kBlackSmallSquare = 0x25AA;
constexpr char32_t kWhiteCircle = 0x25CB;
constexpr char32_t kBlackCircle = 0x25CF;
constexpr char32_t kWhiteBullet = 0x25E6;

// Raised-origin detection (radicals / big math operators).
constexpr double kRaisedTopMarginScale = 0.30;
constexpr double kRaisedBottomDescentScale = 0.55;

// XY-cut segmentation parameters.
constexpr int kMaxXyCutDepth = 10;
constexpr double kLineEstimateClusterScale = 0.5;
constexpr size_t kMinSoftGapIntervals = 4;
constexpr size_t kMinSoftGapSideCount = 3;
constexpr size_t kSoftGapSideDivisor = 25;
constexpr double kHorizontalCutGapScale = 2.6;
constexpr size_t kBridgeTolLineDivisor = 12;
constexpr double kMinColumnCutGapPt = 14.0;
constexpr double kColumnCutGapScale = 1.7;

// Line and word-gap clustering parameters.
constexpr double kColumnGapScale = 1.4;
constexpr double kMinColumnGapPt = 10.0;
constexpr size_t kMinBimodalGapSamples = 3;
constexpr double kMaxLetterTrackingScale = 0.6;
constexpr double kOcrJumpGateScale = 0.06;
constexpr double kNativeJumpGateScale = 0.11;
constexpr double kMaxWordGapScale = 0.9;
constexpr double kOcrMinWordGapScale = 0.10;
constexpr double kOcrMinWordGapFloorPt = 1.2;
constexpr double kNativeMinWordGapScale = 0.18;
constexpr double kMaxDropCapSizeScale = 1.8;
constexpr double kBaseGlyphMinSizeScale = 0.83;
constexpr double kBaselineClusterTolScale = 0.5;

struct Interval {
    double lo = 0;
    double hi = 0;
};

// Merge sorted-by-lo intervals; return midpoints of gaps >= min_gap between them.
std::vector<double> find_gap_cuts(std::vector<Interval> ivals, double min_gap) {
    std::vector<double> cuts;
    if (ivals.empty()) {
        return cuts;
    }
    std::sort(ivals.begin(), ivals.end(), [](const Interval& a, const Interval& b) {
        if (a.lo != b.lo) {
            return a.lo < b.lo;
        }
        return a.hi < b.hi;
    });
    double cur_hi = ivals[0].hi;
    for (size_t i = 1; i < ivals.size(); ++i) {
        if (ivals[i].lo > cur_hi + min_gap) {
            cuts.push_back((cur_hi + ivals[i].lo) / 2.0);
        }
        cur_hi = std::max(cur_hi, ivals[i].hi);
    }
    return cuts;
}

double region_median_size(std::span<const CharInfo* const> chars) {
    std::vector<double> sizes;
    sizes.reserve(chars.size());
    for (const CharInfo* c : chars) {
        if (!c->space && c->size > 0) {
            sizes.push_back(c->size);
        }
    }
    const double m = median(std::move(sizes));
    return m > 0 ? m : kDefaultBodySizePt;
}

size_t estimate_line_count(std::span<const CharInfo* const> chars, double size) {
    std::vector<double> ys;
    ys.reserve(chars.size());
    for (const CharInfo* c : chars) {
        if (!c->space) {
            ys.push_back(c->y);
        }
    }
    std::sort(ys.begin(), ys.end());
    size_t n = 0;
    double last = -kCoordSentinel;
    for (double y : ys) {
        if (y - last > kLineEstimateClusterScale * size) {
            ++n;
        }
        last = y;
    }
    return n;
}

// Column-gap finder that tolerates a few intervals bridging the gap (page
// rules, captions or formulas crossing a gutter must not defeat the column
// split). Returns cut positions inside low-coverage spans of >= min_gap width
// with substantial interval mass on both sides.
std::vector<double> find_soft_gap_cuts(std::span<const Interval> ivals, double min_gap,
                                       size_t bridge_tol) {
    std::vector<double> cuts;
    if (ivals.size() < kMinSoftGapIntervals) {
        return cuts;
    }

    struct Event {
        double x = 0;
        int d = 0;
    };
    std::vector<Event> events;
    events.reserve(ivals.size() * 2);
    for (const Interval& iv : ivals) {
        events.push_back({iv.lo, +1});
        events.push_back({iv.hi, -1});
    }
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        if (a.x != b.x) {
            return a.x < b.x;
        }
        return a.d > b.d;  // starts first: coverage never dips spuriously
    });

    const double global_lo = events.front().x;
    const double global_hi = events.back().x;
    size_t cover = 0;
    bool in_span = false;
    double span_start = 0;
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

    const size_t min_side =
        std::max<size_t>(kMinSoftGapSideCount, ivals.size() / kSoftGapSideDivisor);
    for (const auto& [lo, hi] : spans) {
        if (hi - lo < min_gap) {
            continue;
        }
        if (lo <= global_lo || hi >= global_hi) {
            continue;  // region edges
        }
        const double cut = (lo + hi) / 2.0;
        size_t left = 0;
        size_t right = 0;
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

// Inter-word gap threshold for one line. Default is a fixed fraction of the
// font size; when the line's gaps are clearly bimodal (letter tracking vs.
// word gaps — common in loosely tracked or glyph-positioned PDFs) the
// threshold is placed inside the largest jump between the two clusters.
//
// A repaired OCR layer (line.ocr) gets relaxed gates: it packs words to the
// scan's own geometry, so a genuine word gap can be as tight as ~0.17em —
// under the normal clamp floor — while its letter gaps stay well below 0.1em.
double compute_word_gap(const Line& line) {
    std::vector<double> gaps;
    gaps.reserve(line.chars.size());
    const CharInfo* prev = nullptr;
    for (const CharInfo& c : line.chars) {
        if (c.space) {
            continue;
        }
        if (prev) {
            gaps.push_back(std::max(0.0, c.left - prev->right));
        }
        prev = &c;
    }
    const double base = kDefaultWordGapScale * line.size;
    if (gaps.size() < kMinBimodalGapSamples) {
        return base;
    }

    std::sort(gaps.begin(), gaps.end());
    double best_jump = 0;
    double threshold = base;
    for (size_t i = 1; i < gaps.size(); ++i) {
        const double jump = gaps[i] - gaps[i - 1];
        if (jump > best_jump && gaps[i - 1] <= kMaxLetterTrackingScale * line.size) {
            best_jump = jump;
            threshold = (gaps[i - 1] + gaps[i]) / 2.0;
        }
    }
    const double jump_gate = (line.ocr ? kOcrJumpGateScale : kNativeJumpGateScale) * line.size;
    if (best_jump < jump_gate) {
        return base;  // not bimodal enough
    }
    const double clamp_hi = kMaxWordGapScale * line.size;
    // The OCR floor is absolute (1.2pt); cap it at the high bound so a line
    // whose (repaired) size stays near the 1.0 median floor cannot hand
    // std::clamp an inverted range (UB).
    const double clamp_lo = std::min(
        line.ocr ? std::max(kOcrMinWordGapScale * line.size, kOcrMinWordGapFloorPt)
                 : kNativeMinWordGapScale * line.size,
        clamp_hi);
    return std::clamp(threshold, clamp_lo, clamp_hi);
}

}  // namespace

double median(std::vector<double> v) {
    if (v.empty()) {
        return 0.0;
    }
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    double m = v[mid];
    if (v.size() % 2 == 0) {
        const double lo = *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid));
        m = (m + lo) / 2.0;
    }
    return m;
}

bool is_bullet_cp(char32_t cp) noexcept {
    switch (cp) {
        case kBullet:
        case kTriangularBullet:
        case kBlackSmallSquare:
        case kBlackCircle:
        case kWhiteCircle:
        case kBlackSquare:
        case kWhiteBullet:
        case kMiddleDot:
        case kHyphenBullet:
        case kBulletOperator:
        case U'*':
            return true;
        default:
            return false;
    }
}

bool is_dashy_cp(char32_t cp) noexcept {
    return cp == U'-' || cp == kEnDash || cp == kEmDash;
}

// A tall math symbol (a radical or a big operator like the summation) reports
// its origin near the TOP of its box, with the box descending well below that
// origin. Placing it by its baseline would cluster it with the line above and
// scatter its radicand/limits, so it is located by its box centre instead.
bool raised_origin_glyph(const CharInfo& c) noexcept {
    if (c.size <= 0 || c.top <= c.bottom) {
        return false;
    }
    return (c.top - c.y) < kRaisedTopMarginScale * c.size &&
           (c.y - c.bottom) > kRaisedBottomDescentScale * c.size;
}

// Recursive XY cut. Horizontal (top/bottom) cuts with a generous gap first,
// then vertical (column) cuts, so a full-width title above two columns is
// separated before the columns are.
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
    if (!y_cuts.empty()) {
        std::sort(y_cuts.begin(), y_cuts.end(), std::greater<double>{});
        std::vector<std::vector<const CharInfo*>> parts(y_cuts.size() + 1);
        for (const CharInfo* c : chars) {
            const double mid = (c->bottom + c->top) / 2.0;
            size_t idx = 0;  // parts ordered top to bottom
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

    // Bridge tolerance scales with region height: dense multi-line regions
    // may cut through a few stray glyphs (rules, captions crossing a gutter),
    // but sparse regions get strict zero-coverage cuts.
    const size_t line_estimate = estimate_line_count(chars, size);
    const size_t bridge_tol = line_estimate / kBridgeTolLineDivisor;
    std::vector<double> x_cuts = find_soft_gap_cuts(
        x_ivals, std::max(kMinColumnCutGapPt, kColumnCutGapScale * size), bridge_tol);
    // A column cut through aligned table rows would destroy the table; keep
    // tabular regions whole so table detection can see them.
    if (!x_cuts.empty() && region_looks_tabular(chars)) {
        x_cuts.clear();
    }
    if (!x_cuts.empty()) {
        std::sort(x_cuts.begin(), x_cuts.end());
        std::vector<std::vector<const CharInfo*>> parts(x_cuts.size() + 1);
        for (const CharInfo* c : chars) {
            const double mid = (c->left + c->right) / 2.0;
            size_t idx = 0;
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

double column_gap_threshold(double size) noexcept {
    return std::max(kColumnGapScale * size, kMinColumnGapPt);
}

void finalize_line(Line& line) {
    std::stable_sort(line.chars.begin(), line.chars.end(),
                     [](const CharInfo& a, const CharInfo& b) {
                         if (a.x != b.x) {
                             return a.x < b.x;
                         }
                         // Coincident word-break space stays ahead of its glyph.
                         return a.space && !b.space;
                     });

    std::vector<double> sizes;
    sizes.reserve(line.chars.size());
    int mono_count = 0;
    int non_space = 0;
    int ocr_count = 0;
    bool has_cjk = false;
    double x0 = kCoordSentinel;
    double x1 = -kCoordSentinel;
    double size_max = 0;
    for (const CharInfo& c : line.chars) {
        if (c.space) {
            continue;
        }
        ++non_space;
        sizes.push_back(c.size);
        if (c.mono) {
            ++mono_count;
        }
        if (c.ocr) {
            ++ocr_count;
        }
        if (is_cjk_cp(c.code)) {
            has_cjk = true;
        }
        x0 = std::min(x0, c.left);
        x1 = std::max(x1, c.right);
        size_max = std::max(size_max, c.size);
    }
    line.ocr = ocr_count * 2 > non_space;
    line.size = std::max(median(std::move(sizes)), kMinFontSizePt);
    // Script reference: the body size on the line. Normally the max glyph size,
    // but a single much-larger glyph (a drop cap) falls back to the median so it
    // does not make the ordinary text look like subscripts.
    line.size_max =
        (size_max > 0 && size_max <= kMaxDropCapSizeScale * line.size) ? size_max : line.size;
    // Baseline from the base-level glyphs only, so a line that is half subscript
    // (e.g. a stranded "PE_pos") still reports the true text baseline.
    std::vector<double> base_ys;
    std::vector<double> all_ys;
    base_ys.reserve(static_cast<size_t>(non_space));
    all_ys.reserve(static_cast<size_t>(non_space));
    for (const CharInfo& c : line.chars) {
        if (c.space || raised_origin_glyph(c)) {
            continue;
        }
        all_ys.push_back(c.y);
        if (c.size >= kBaseGlyphMinSizeScale * line.size_max) {
            base_ys.push_back(c.y);
        }
    }
    line.baseline = median(base_ys.empty() ? std::move(all_ys) : std::move(base_ys));
    if (non_space == 0) {
        line.x0 = 0.0;
        line.x1 = 0.0;
    } else {
        line.x0 = x0;
        line.x1 = x1;
    }
    // CJK fonts legitimately declare fixed pitch; that must not turn prose
    // into code blocks.
    line.mono_majority = !has_cjk && non_space > 0 && mono_count * 2 > non_space;
    line.word_gap = compute_word_gap(line);
}

// Builds styled runs, the plain text and column segments for a line.
void build_runs(Line& line) {
    line.runs.clear();
    line.plain.clear();
    line.segments.clear();

    normalize_inline_punct(line);
    std::vector<Emit> emits = plan_line(line);
    repair_token_spaces(line, emits);
    if (line_needs_math(line)) {
        render_math(line, emits);
        rewrite_footnote_refs(line);
    } else {
        render_plain(line, emits);
    }
}

std::vector<Line> build_lines(std::span<const CharInfo* const> chars) {
    struct OpenLine {
        Line line;
        double baseline_sum = 0;
        double size_max = 0;
        int count = 0;
        double baseline() const noexcept { return count > 0 ? baseline_sum / count : 0.0; }
    };
    std::vector<OpenLine> open;

    std::vector<const CharInfo*> sorted(chars.begin(), chars.end());
    std::stable_sort(sorted.begin(), sorted.end(), [](const CharInfo* a, const CharInfo* b) {
        if (a->y != b->y) {
            return a->y > b->y;
        }
        if (a->x != b->x) {
            return a->x < b->x;
        }
        // PDFium emits a word-break space at the *same* origin as the following
        // glyph; keep the space first so it isn't displaced into the next word
        // ("of the" must not become "oft he"). stable_sort keeps drawing order
        // for any other coincident glyphs.
        return a->space && !b->space;
    });

    for (const CharInfo* c : sorted) {
        // A radical/operator origin sits at the top of its tall box; cluster it
        // by its box centre so it joins the line of its radicand/limits, not a
        // line above.
        const double cy = raised_origin_glyph(*c) ? (c->top + c->bottom) / 2.0 : c->y;
        OpenLine* best = nullptr;
        double best_dist = kCoordSentinel;
        for (auto& ol : open) {
            const double tol = kBaselineClusterTolScale * std::max(ol.size_max, c->size);
            const double dist = std::fabs(ol.baseline() - cy);
            if (dist <= tol && dist < best_dist) {
                best = &ol;
                best_dist = dist;
            }
        }
        if (!best) {
            open.emplace_back();
            best = &open.back();
        }
        best->line.chars.push_back(*c);
        best->baseline_sum += cy;
        best->size_max = std::max(best->size_max, c->size);
        best->count += 1;
    }

    std::vector<Line> lines;
    lines.reserve(open.size());
    for (auto& ol : open) {
        const bool has_text = std::any_of(ol.line.chars.cbegin(), ol.line.chars.cend(),
                                          [](const CharInfo& c) { return !c.space; });
        if (!has_text) {
            continue;
        }
        finalize_line(ol.line);
        build_runs(ol.line);
        if (!ol.line.plain.empty()) {
            lines.push_back(std::move(ol.line));
        }
    }
    std::stable_sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) {
        if (a.baseline != b.baseline) {
            return a.baseline > b.baseline;
        }
        return a.x0 < b.x0;
    });
    return lines;
}

}  // namespace detail
}  // namespace pdf2md
