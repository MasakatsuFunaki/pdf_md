#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

#include "layout_internal.h"
#include "utf8.h"

namespace pdf2md {
namespace detail {

// ---------------------------------------------------------------- utilities

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    double m = v[mid];
    if (v.size() % 2 == 0) {
        double lo = *std::max_element(v.begin(), v.begin() + mid);
        m = (m + lo) / 2.0;
    }
    return m;
}

bool is_bullet_cp(char32_t cp) {
    switch (cp) {
        case 0x2022: case 0x2023: case 0x25AA: case 0x25CF: case 0x25CB:
        case 0x25A0: case 0x25E6: case 0x00B7: case 0x2043: case 0x2219:
        case U'*':
            return true;
        default:
            return false;
    }
}

bool is_dashy_cp(char32_t cp) {
    return cp == U'-' || cp == 0x2013 || cp == 0x2014;
}

// A tall math symbol (a radical or a big operator like the summation) reports
// its origin near the TOP of its box, with the box descending well below that
// origin. Placing it by its baseline would cluster it with the line above and
// scatter its radicand/limits, so it is located by its box centre instead.
bool raised_origin_glyph(const CharInfo& c) {
    if (c.size <= 0 || c.top <= c.bottom) return false;
    return (c.top - c.y) < 0.30 * c.size && (c.y - c.bottom) > 0.55 * c.size;
}

// ---------------------------------------------------------------- XY cut

struct Interval {
    double lo, hi;
};

// Merge sorted-by-lo intervals; return gaps of at least min_gap between them.
std::vector<double> find_gap_cuts(std::vector<Interval> ivals, double min_gap) {
    std::vector<double> cuts;
    if (ivals.empty()) return cuts;
    std::sort(ivals.begin(), ivals.end(), [](const Interval& a, const Interval& b) {
        return a.lo < b.lo;
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

double region_median_size(const std::vector<const CharInfo*>& chars) {
    std::vector<double> sizes;
    sizes.reserve(chars.size());
    for (const CharInfo* c : chars)
        if (!c->space && c->size > 0) sizes.push_back(c->size);
    double m = median(std::move(sizes));
    return m > 0 ? m : 11.0;
}

size_t estimate_line_count(const std::vector<const CharInfo*>& chars, double size) {
    std::vector<double> ys;
    ys.reserve(chars.size());
    for (const CharInfo* c : chars)
        if (!c->space) ys.push_back(c->y);
    std::sort(ys.begin(), ys.end());
    size_t n = 0;
    double last = -1e30;
    for (double y : ys) {
        if (y - last > 0.5 * size) ++n;
        last = y;
    }
    return n;
}

// Column-gap finder that tolerates a few intervals bridging the gap (page
// rules, captions or formulas crossing a gutter must not defeat the column
// split). Returns cut positions inside low-coverage spans of >= min_gap width
// with substantial interval mass on both sides.
std::vector<double> find_soft_gap_cuts(const std::vector<Interval>& ivals, double min_gap,
                                       size_t bridge_tol) {
    std::vector<double> cuts;
    if (ivals.size() < 4) return cuts;

    struct Event {
        double x;
        int d;
    };
    std::vector<Event> events;
    events.reserve(ivals.size() * 2);
    for (const Interval& iv : ivals) {
        events.push_back({iv.lo, +1});
        events.push_back({iv.hi, -1});
    }
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        if (a.x != b.x) return a.x < b.x;
        return a.d > b.d;  // starts first: coverage never dips spuriously
    });

    const double global_lo = events.front().x;
    const double global_hi = events.back().x;
    size_t cover = 0;
    bool in_span = false;
    double span_start = 0;
    std::vector<std::pair<double, double>> spans;
    for (const Event& e : events) {
        cover += static_cast<size_t>(e.d > 0 ? 1 : 0);
        cover -= static_cast<size_t>(e.d < 0 ? 1 : 0);
        if (!in_span && cover <= bridge_tol) {
            in_span = true;
            span_start = e.x;
        } else if (in_span && cover > bridge_tol) {
            in_span = false;
            spans.emplace_back(span_start, e.x);
        }
    }

    for (auto [lo, hi] : spans) {
        if (hi - lo < min_gap) continue;
        if (lo <= global_lo || hi >= global_hi) continue;  // region edges
        double cut = (lo + hi) / 2.0;
        size_t left = 0, right = 0;
        for (const Interval& iv : ivals) {
            if (iv.hi < cut) ++left;
            else if (iv.lo > cut) ++right;
        }
        size_t min_side = std::max<size_t>(3, ivals.size() / 25);
        if (left >= min_side && right >= min_side) cuts.push_back(cut);
    }
    return cuts;
}

// Recursive XY cut. Horizontal (top/bottom) cuts with a generous gap first,
// then vertical (column) cuts, so a full-width title above two columns is
// separated before the columns are.
void split_region(std::vector<const CharInfo*> chars, int depth,
                  std::vector<std::vector<const CharInfo*>>& out) {
    if (chars.empty()) return;
    if (depth >= 10) {
        out.push_back(std::move(chars));
        return;
    }

    const double size = region_median_size(chars);

    std::vector<Interval> y_ivals, x_ivals;
    y_ivals.reserve(chars.size());
    x_ivals.reserve(chars.size());
    for (const CharInfo* c : chars) {
        if (c->space) continue;
        y_ivals.push_back({c->bottom, c->top});
        x_ivals.push_back({c->left, c->right});
    }

    std::vector<double> y_cuts = find_gap_cuts(y_ivals, 2.6 * size);
    if (!y_cuts.empty()) {
        std::sort(y_cuts.begin(), y_cuts.end(), std::greater<double>());
        std::vector<std::vector<const CharInfo*>> parts(y_cuts.size() + 1);
        for (const CharInfo* c : chars) {
            double mid = (c->bottom + c->top) / 2.0;
            size_t idx = 0;  // parts ordered top to bottom
            while (idx < y_cuts.size() && mid < y_cuts[idx]) ++idx;
            // idx: number of cuts the char lies below
            parts[idx].push_back(c);
        }
        for (auto& part : parts) split_region(std::move(part), depth + 1, out);
        return;
    }

    // Bridge tolerance scales with region height: dense multi-line regions
    // may cut through a few stray glyphs (rules, captions crossing a gutter),
    // but sparse regions get strict zero-coverage cuts — with only a handful
    // of lines, "few glyphs" is indistinguishable from real text.
    size_t line_estimate = estimate_line_count(chars, size);
    size_t bridge_tol = line_estimate / 12;
    std::vector<double> x_cuts =
        find_soft_gap_cuts(x_ivals, std::max(14.0, 1.7 * size), bridge_tol);
    // A column cut through aligned table rows would destroy the table; keep
    // tabular regions whole so table detection can see them.
    if (!x_cuts.empty() && region_looks_tabular(chars)) x_cuts.clear();
    if (!x_cuts.empty()) {
        std::sort(x_cuts.begin(), x_cuts.end());
        std::vector<std::vector<const CharInfo*>> parts(x_cuts.size() + 1);
        for (const CharInfo* c : chars) {
            double mid = (c->left + c->right) / 2.0;
            size_t idx = 0;
            while (idx < x_cuts.size() && mid > x_cuts[idx]) ++idx;
            parts[idx].push_back(c);
        }
        for (auto& part : parts) split_region(std::move(part), depth + 1, out);
        return;
    }

    out.push_back(std::move(chars));
}

// ---------------------------------------------------------------- lines

double column_gap_threshold(double size) { return std::max(1.4 * size, 10.0); }

// Inter-word gap threshold for one line. Default is a fixed fraction of the
// font size; when the line's gaps are clearly bimodal (letter tracking vs.
// word gaps — common in loosely tracked or glyph-positioned PDFs) the
// threshold is placed inside the largest jump between the two clusters.
//
// A repaired OCR layer (line.ocr) gets relaxed gates: it packs words to the
// scan's own geometry, so a genuine word gap can be as tight as ~0.17em —
// under the normal clamp floor — while its letter gaps stay well below 0.1em.
// The clusters remain separable, just at a smaller scale, so the bimodal
// detector runs with a lower jump gate and clamp floor (with an absolute
// 1.2pt floor so a stray kerning outlier inside a word cannot split it).
double compute_word_gap(const Line& line) {
    std::vector<double> gaps;
    const CharInfo* prev = nullptr;
    for (const CharInfo& c : line.chars) {
        if (c.space) continue;
        if (prev) gaps.push_back(std::max(0.0, c.left - prev->right));
        prev = &c;
    }
    const double base = 0.28 * line.size;
    if (gaps.size() < 3) return base;

    std::sort(gaps.begin(), gaps.end());
    double best_jump = 0;
    double threshold = base;
    for (size_t i = 1; i < gaps.size(); ++i) {
        double jump = gaps[i] - gaps[i - 1];
        if (jump > best_jump && gaps[i - 1] <= 0.6 * line.size) {
            best_jump = jump;
            threshold = (gaps[i - 1] + gaps[i]) / 2.0;
        }
    }
    const double jump_gate = (line.ocr ? 0.06 : 0.11) * line.size;
    const double clamp_hi = 0.9 * line.size;
    // The OCR floor is absolute (1.2pt); cap it at the high bound so a line
    // whose (repaired) size stays near the 1.0 median floor cannot hand
    // std::clamp an inverted range (UB).
    const double clamp_lo = std::min(
        line.ocr ? std::max(0.10 * line.size, 1.2) : 0.18 * line.size, clamp_hi);
    if (best_jump < jump_gate) return base;  // not bimodal enough
    return std::clamp(threshold, clamp_lo, clamp_hi);
}

void finalize_line(Line& line) {
    std::stable_sort(line.chars.begin(), line.chars.end(),
                     [](const CharInfo& a, const CharInfo& b) {
                         if (a.x != b.x) return a.x < b.x;
                         // Coincident word-break space stays ahead of its glyph.
                         return a.space && !b.space;
                     });

    std::vector<double> sizes;
    int mono_count = 0, non_space = 0, ocr_count = 0;
    bool has_cjk = false;
    double x0 = 1e30, x1 = -1e30, size_max = 0;
    for (const CharInfo& c : line.chars) {
        if (c.space) continue;
        ++non_space;
        sizes.push_back(c.size);
        if (c.mono) ++mono_count;
        if (c.ocr) ++ocr_count;
        if (is_cjk_cp(c.code)) has_cjk = true;
        x0 = std::min(x0, c.left);
        x1 = std::max(x1, c.right);
        size_max = std::max(size_max, c.size);
    }
    line.ocr = ocr_count * 2 > non_space;
    line.size = std::max(median(sizes), 1.0);
    // Script reference: the body size on the line. Normally the max glyph size,
    // but a single much-larger glyph (a drop cap) falls back to the median so it
    // does not make the ordinary text look like subscripts.
    line.size_max = (size_max > 0 && size_max <= 1.8 * line.size) ? size_max : line.size;
    // Baseline from the base-level glyphs only, so a line that is half subscript
    // (e.g. a stranded "PE_pos") still reports the true text baseline.
    std::vector<double> base_ys, all_ys;
    for (const CharInfo& c : line.chars) {
        if (c.space) continue;
        // Radicals and big operators report a raised origin; they must not drag
        // the line baseline.
        if (raised_origin_glyph(c)) continue;
        all_ys.push_back(c.y);
        if (c.size >= 0.83 * line.size_max) base_ys.push_back(c.y);
    }
    line.baseline = median(base_ys.empty() ? std::move(all_ys) : std::move(base_ys));
    line.x0 = x0;
    line.x1 = x1;
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

std::vector<Line> build_lines(const std::vector<const CharInfo*>& chars) {
    struct OpenLine {
        Line line;
        double baseline_sum = 0;
        double size_max = 0;
        int count = 0;
        double baseline() const { return baseline_sum / count; }
    };
    std::vector<OpenLine> open;

    std::vector<const CharInfo*> sorted = chars;
    std::stable_sort(sorted.begin(), sorted.end(), [](const CharInfo* a, const CharInfo* b) {
        if (a->y != b->y) return a->y > b->y;
        if (a->x != b->x) return a->x < b->x;
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
        double cy = raised_origin_glyph(*c) ? (c->top + c->bottom) / 2.0 : c->y;
        OpenLine* best = nullptr;
        double best_dist = 1e30;
        for (auto& ol : open) {
            double tol = 0.5 * std::max(ol.size_max, static_cast<double>(c->size));
            double dist = std::fabs(ol.baseline() - cy);
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
        best->size_max = std::max(best->size_max, static_cast<double>(c->size));
        best->count += 1;
    }

    std::vector<Line> lines;
    for (auto& ol : open) {
        bool has_text = std::any_of(ol.line.chars.begin(), ol.line.chars.end(),
                                    [](const CharInfo& c) { return !c.space; });
        if (!has_text) continue;
        finalize_line(ol.line);
        build_runs(ol.line);
        if (!ol.line.plain.empty()) lines.push_back(std::move(ol.line));
    }
    std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) {
        if (a.baseline != b.baseline) return a.baseline > b.baseline;
        return a.x0 < b.x0;
    });
    return lines;
}

}  // namespace detail
}  // namespace pdf2md
