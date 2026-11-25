#include "pdf2md/layout/line_cluster.h"

#include "pdf2md/model/document_utils.h"
#include "pdf2md/model/geometry.h"
#include "pdf2md/utf8.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace pdf2md::layout {

using model::kCoordSentinel;
using model::kDefaultWordGapScale;
using model::kMinFontSizePt;
using model::median;
using model::raised_origin_glyph;

namespace {

constexpr double kColumnGapScale = 1.4;
constexpr double kMinColumnGapPt = 10.0;
constexpr std::size_t kMinBimodalGapSamples = 3U;
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
constexpr std::size_t kMinLeaderDotRun = 3U;
constexpr double kMaxLeaderDotGapScale = 3.5;

constexpr bool is_leader_dot_cp(char32_t cp) noexcept {
    return cp == U'.' || cp == 0x2024U || cp == 0x2026U;
}

}  // namespace

double column_gap_threshold(double size) noexcept {
    return (std::max)(kColumnGapScale * size, kMinColumnGapPt);
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
    std::vector<const CharInfo*> ns;
    ns.reserve(line.chars.size());
    for (const CharInfo& c : line.chars) {
        if (!c.space) {
            ns.push_back(&c);
        }
    }

    // Mark repeated `. . .` TOC dot-leader runs so wide leader spacing cannot
    // masquerade as the upper cluster of bimodal inter-word gaps.
    std::vector<char> in_leader(ns.size(), 0);
    const double max_dot_gap = kMaxLeaderDotGapScale * (std::max)(line.size, kMinFontSizePt);
    for (std::size_t i = 0U; i < ns.size();) {
        if (!is_leader_dot_cp(ns[i]->code)) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j + 1U < ns.size() && is_leader_dot_cp(ns[j + 1U]->code) &&
               ns[j + 1U]->left - ns[j]->right <= max_dot_gap) {
            ++j;
        }
        if (j - i + 1U >= kMinLeaderDotRun) {
            for (std::size_t k = i; k <= j; ++k) {
                in_leader[k] = 1;
            }
        }
        i = j + 1U;
    }

    std::vector<double> gaps;
    gaps.reserve(ns.size());
    for (std::size_t i = 1U; i < ns.size(); ++i) {
        if (in_leader[i] != 0 || in_leader[i - 1U] != 0) {
            continue;
        }
        gaps.push_back((std::max)(0.0, ns[i]->left - ns[i - 1U]->right));
    }
    const double base = kDefaultWordGapScale * line.size;
    if (gaps.size() < kMinBimodalGapSamples) {
        return base;
    }

    std::sort(gaps.begin(), gaps.end());
    double best_jump = 0.0;
    double threshold = base;
    for (std::size_t i = 1U; i < gaps.size(); ++i) {
        const double jump = gaps[i] - gaps[i - 1U];
        if (jump > best_jump && gaps[i - 1U] <= kMaxLetterTrackingScale * line.size) {
            best_jump = jump;
            threshold = (gaps[i - 1U] + gaps[i]) / 2.0;
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
    const double clamp_lo = (std::min)(
        line.ocr ? (std::max)(kOcrMinWordGapScale * line.size, kOcrMinWordGapFloorPt)
                 : kNativeMinWordGapScale * line.size,
        clamp_hi);
    return std::clamp(threshold, clamp_lo, clamp_hi);
}

void finalize_line(Line& line) {
    std::stable_sort(line.chars.begin(), line.chars.end(),
                     [](const CharInfo& a, const CharInfo& b) noexcept {
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
    double size_max = 0.0;
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
        x0 = (std::min)(x0, c.left);
        x1 = (std::max)(x1, c.right);
        size_max = (std::max)(size_max, c.size);
    }
    line.ocr = ocr_count * 2 > non_space;
    line.size = (std::max)(median(std::move(sizes)), kMinFontSizePt);
    // Script reference: the body size on the line. Normally the max glyph size,
    // but a single much-larger glyph (a drop cap) falls back to the median so it
    // does not make the ordinary text look like subscripts.
    line.size_max =
        (size_max > 0.0 && size_max <= kMaxDropCapSizeScale * line.size) ? size_max : line.size;
    // Baseline from the base-level glyphs only, so a line that is half subscript
    // (e.g. a stranded "PE_pos") still reports the true text baseline.
    std::vector<double> base_ys;
    std::vector<double> all_ys;
    base_ys.reserve(static_cast<std::size_t>(non_space));
    all_ys.reserve(static_cast<std::size_t>(non_space));
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

std::vector<Line> cluster_lines(std::span<const CharInfo* const> chars) {
    struct OpenLine {
        Line line;
        double baseline_sum = 0.0;
        double size_max = 0.0;
        int count = 0;
        [[nodiscard]] double baseline() const noexcept {
            return count > 0 ? baseline_sum / count : 0.0;
        }
    };
    std::vector<OpenLine> open;

    std::vector<const CharInfo*> sorted(chars.begin(), chars.end());
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const CharInfo* a, const CharInfo* b) noexcept {
                         if (a->y != b->y) {
                             return a->y > b->y;
                         }
                         if (a->x != b->x) {
                             return a->x < b->x;
                         }
                         // PDFium emits a word-break space at the *same* origin as the following
                         // glyph; keep the space first so it isn't displaced into the next word
                         // ("of the" must not become "oft he").
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
            const double tol = kBaselineClusterTolScale * (std::max)(ol.size_max, c->size);
            const double dist = std::fabs(ol.baseline() - cy);
            if (dist <= tol && dist < best_dist) {
                best = &ol;
                best_dist = dist;
            }
        }
        if (best == nullptr) {
            open.emplace_back();
            best = &open.back();
        }
        best->line.chars.push_back(*c);
        best->baseline_sum += cy;
        best->size_max = (std::max)(best->size_max, c->size);
        best->count += 1;
    }

    std::vector<Line> lines;
    lines.reserve(open.size());
    for (auto& ol : open) {
        const bool has_text = std::any_of(ol.line.chars.cbegin(), ol.line.chars.cend(),
                                          [](const CharInfo& c) noexcept { return !c.space; });
        if (!has_text) {
            continue;
        }
        finalize_line(ol.line);
        lines.push_back(std::move(ol.line));
    }
    std::stable_sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) noexcept {
        if (a.baseline != b.baseline) {
            return a.baseline > b.baseline;
        }
        return a.x0 < b.x0;
    });
    return lines;
}

}  // namespace pdf2md::layout
