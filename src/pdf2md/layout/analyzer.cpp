#include "pdf2md/layout_analyzer.h"

#include "pdf2md/layout/blocks.h"
#include "pdf2md/layout/chrome.h"
#include "pdf2md/layout/math.h"
#include "pdf2md/layout/passes.h"
#include "pdf2md/layout/regions.h"
#include "pdf2md/layout/tables_ruled.h"
#include "pdf2md/model/document_utils.h"
#include "pdf2md/model/geometry.h"
#include "pdf2md/utf8.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pdf2md::layout {

using model::count_glyphs;
using model::kCoordSentinel;
using model::kCoordSentinelThreshold;
using model::kMinFontSizePt;
using model::raised_origin_glyph;

namespace {

// Minimum non-space glyphs for a line to count as a full body line when
// estimating the page text column's left margin.
constexpr std::size_t kBodyColumnMinGlyphs = 15U;

// Midline vector strikethrough detection thresholds.
constexpr double kMaxStrikeRuleThicknessPt = 2.5;
constexpr double kStrikeMidlineMinRatio = 0.18;
constexpr double kStrikeMidlineMaxRatio = 0.68;
constexpr double kStrikeMinHorizOverlapRatio = 0.70;
constexpr double kStrikeBaselineTolScale = 0.25;
constexpr double kStrikeMaxAdjGapScale = 1.2;
constexpr std::size_t kMinStrikeWordGlyphs = 2U;
constexpr double kTightStackedFracSlackPt = 1.5;
constexpr double kTightStackedFracMaxGapScale = 0.35;
constexpr double kStrikeMinSpanCoverageRatio = 0.45;

// Standalone horizontal thematic rule (`---`) detection thresholds.
constexpr double kRuleBodyBottomRatio = 0.12;
constexpr double kRuleBodyTopRatio = 0.88;
constexpr double kFullWidthRuleMinRatio = 0.35;
constexpr double kCenteredRuleMinRatio = 0.20;
constexpr double kCenteredRuleMaxOffsetRatio = 0.12;
constexpr double kRuleMergeYPosTolPt = 2.0;
constexpr double kRuleMergeXJoinTolPt = 3.5;
constexpr double kMinCrossingVertRuleLenPt = 4.0;
constexpr double kRuleCrossTolPt = 2.5;
constexpr double kGlyphStraddleSlackPt = 1.0;
constexpr double kTableRuleClearanceScale = 0.85;
constexpr double kHeadingUnderlineSizeScale = 1.10;
constexpr double kHeadingUnderlineMaxGapScale = 0.65;
constexpr std::size_t kHeadingUnderlineMaxLines = 3U;
constexpr double kCaptionRuleMaxGapScale = 1.25;
constexpr double kFootnoteRuleMaxGapScale = 1.25;

bool starts_with_caption_keyword(const Block& b) noexcept {
    if (b.lines.empty()) {
        return false;
    }
    const std::u32string& s = b.lines.front().plain;
    std::size_t i = 0U;
    while (i < s.size() && s[i] == U' ') {
        ++i;
    }
    const std::u32string_view rest = std::u32string_view(s).substr(i);
    return rest.starts_with(U"Table ") || rest.starts_with(U"Chart ") ||
           rest.starts_with(U"Figure ") || rest.starts_with(U"Fig. ");
}

constexpr bool is_word_like_cp(char32_t cp) noexcept {
    return is_letter_cp(cp) || is_ascii_digit(cp) || is_cjk_cp(cp) || cp >= 0x00C0U;
}

void apply_midline_strikethrough_rules(std::vector<CharInfo>& chars,
                                       std::span<const char> consumed_chars,
                                       std::span<const RuleSegment> rules,
                                       double body_size,
                                       std::vector<char>& consumed_rules) {
    for (std::size_t ri = 0U; ri < rules.size(); ++ri) {
        if (ri < consumed_rules.size() && consumed_rules[ri] != 0) {
            continue;
        }
        const RuleSegment& r = rules[ri];
        if (!r.horizontal || r.thickness > kMaxStrikeRuleThicknessPt) {
            continue;
        }
        const double rx0 = (std::min)(r.x0, r.x1);
        const double rx1 = (std::max)(r.x0, r.x1);
        const double ry = (r.y0 + r.y1) * 0.5;
        if (rx1 <= rx0) {
            continue;
        }

        std::vector<std::size_t> crossed;
        bool has_tight_above = false;
        bool has_tight_below = false;
        double crossed_width = 0.0;

        for (std::size_t ci = 0U; ci < chars.size(); ++ci) {
            if (!consumed_chars.empty() && consumed_chars[ci] != 0) {
                continue;
            }
            const CharInfo& c = chars[ci];
            if (c.space || raised_origin_glyph(c)) {
                continue;
            }
            const double gw = c.right - c.left;
            const double ascent = c.top - c.y;
            if (gw <= 0.0 || ascent <= 0.0) {
                continue;
            }
            const double overlap = (std::min)(rx1, c.right) - (std::max)(rx0, c.left);
            if (overlap <= 0.0) {
                continue;
            }
            if (overlap >= kStrikeMinHorizOverlapRatio * gw &&
                ry >= c.y + kStrikeMidlineMinRatio * ascent &&
                ry <= c.y + kStrikeMidlineMaxRatio * ascent) {
                crossed.push_back(ci);
                crossed_width += overlap;
                continue;
            }
            if (c.y > ry && c.bottom >= ry - kTightStackedFracSlackPt &&
                c.bottom - ry <= kTightStackedFracMaxGapScale * body_size) {
                has_tight_above = true;
            } else if (c.top <= ry + kTightStackedFracSlackPt &&
                       ry - c.top <= kTightStackedFracMaxGapScale * body_size) {
                has_tight_below = true;
            }
        }

        if (crossed.size() < kMinStrikeWordGlyphs) {
            continue;
        }
        if (has_tight_above && has_tight_below &&
            crossed_width < kStrikeMinSpanCoverageRatio * (rx1 - rx0)) {
            continue;
        }

        std::sort(crossed.begin(), crossed.end(), [&chars](std::size_t a, std::size_t b) noexcept {
            return chars[a].left < chars[b].left;
        });

        bool any_matched = false;
        for (std::size_t i = 0U; i < crossed.size(); ++i) {
            const double base_y = chars[crossed[i]].y;
            const double base_sz = (std::max)(chars[crossed[i]].size, kMinFontSizePt);
            std::vector<std::size_t> group = {crossed[i]};
            bool has_word_cp = is_word_like_cp(chars[crossed[i]].code);
            for (std::size_t j = i + 1U; j < crossed.size(); ++j) {
                const CharInfo& cj = chars[crossed[j]];
                const double tol =
                    kStrikeBaselineTolScale * (std::max)(base_sz, cj.size);
                if (std::fabs(cj.y - base_y) > tol) {
                    continue;
                }
                const CharInfo& prev = chars[group.back()];
                if (cj.left - prev.right >
                    kStrikeMaxAdjGapScale * (std::max)(prev.size, cj.size)) {
                    break;
                }
                group.push_back(crossed[j]);
                has_word_cp = has_word_cp || is_word_like_cp(cj.code);
            }
            if (group.size() < kMinStrikeWordGlyphs || !has_word_cp) {
                continue;
            }

            // Propagate strikethrough across all glyphs on this confirmed baseline
            // covered by [rx0, rx1] (so short punctuation like '-' or '.' isn't skipped).
            for (std::size_t ci = 0U; ci < chars.size(); ++ci) {
                if (!consumed_chars.empty() && consumed_chars[ci] != 0) {
                    continue;
                }
                CharInfo& c = chars[ci];
                if (c.space || raised_origin_glyph(c)) {
                    continue;
                }
                const double gw = c.right - c.left;
                if (gw <= 0.0) {
                    continue;
                }
                const double tol = kStrikeBaselineTolScale * (std::max)(base_sz, c.size);
                const double overlap = (std::min)(rx1, c.right) - (std::max)(rx0, c.left);
                if (std::fabs(c.y - base_y) <= tol &&
                    overlap >= kStrikeMinHorizOverlapRatio * gw) {
                    c.strikethrough = true;
                }
            }
            any_matched = true;
            break;
        }

        if (any_matched && ri < consumed_rules.size()) {
            consumed_rules[ri] = 1;
        }
    }
}

void emit_standalone_horizontal_rules(const PageData& page, double body_size,
                                      std::vector<char>& consumed_rules,
                                      std::vector<Block>& page_blocks) {
    if (page.rules.empty() || page.width <= 0.0 || page.height <= 0.0) {
        return;
    }

    struct CandRule {
        double x0 = 0.0;
        double x1 = 0.0;
        double y = 0.0;
        std::vector<std::size_t> indices;
    };
    std::vector<CandRule> raw;
    for (std::size_t ri = 0U; ri < page.rules.size(); ++ri) {
        if (ri < consumed_rules.size() && consumed_rules[ri] != 0) {
            continue;
        }
        const RuleSegment& r = page.rules[ri];
        if (!r.horizontal) {
            continue;
        }
        const double rx0 = (std::min)(r.x0, r.x1);
        const double rx1 = (std::max)(r.x0, r.x1);
        if (rx1 <= rx0) {
            continue;
        }
        raw.push_back({rx0, rx1, r.y0, {ri}});
    }
    if (raw.empty()) {
        return;
    }

    std::sort(raw.begin(), raw.end(), [](const CandRule& a, const CandRule& b) noexcept {
        if (a.y != b.y) {
            return a.y > b.y;
        }
        return a.x0 < b.x0;
    });

    std::vector<CandRule> merged;
    merged.reserve(raw.size());
    for (CandRule& cr : raw) {
        bool joined = false;
        for (CandRule& m : merged) {
            if (std::fabs(m.y - cr.y) <= kRuleMergeYPosTolPt &&
                cr.x0 <= m.x1 + kRuleMergeXJoinTolPt &&
                cr.x1 >= m.x0 - kRuleMergeXJoinTolPt) {
                m.x0 = (std::min)(m.x0, cr.x0);
                m.x1 = (std::max)(m.x1, cr.x1);
                m.indices.insert(m.indices.end(), cr.indices.cbegin(), cr.indices.cend());
                joined = true;
                break;
            }
        }
        if (!joined) {
            merged.push_back(std::move(cr));
        }
    }

    const double y_min = kRuleBodyBottomRatio * page.height;
    const double y_max = kRuleBodyTopRatio * page.height;
    const double page_mid_x = page.width * 0.5;

    for (const CandRule& cr : merged) {
        if (cr.y < y_min || cr.y > y_max) {
            continue;
        }
        const double span_w = cr.x1 - cr.x0;
        const bool wide = span_w >= kFullWidthRuleMinRatio * page.width;
        const bool centered =
            span_w >= kCenteredRuleMinRatio * page.width &&
            std::fabs((cr.x0 + cr.x1) * 0.5 - page_mid_x) <=
                kCenteredRuleMaxOffsetRatio * page.width;
        if (!wide && !centered) {
            continue;
        }

        // Reject rules crossed or met by vertical rules (boxes, rejected grids, diagrams).
        bool crossed_by_vert = false;
        for (const RuleSegment& vr : page.rules) {
            if (vr.horizontal) {
                continue;
            }
            const double vy0 = (std::min)(vr.y0, vr.y1);
            const double vy1 = (std::max)(vr.y0, vr.y1);
            const double vx = (vr.x0 + vr.x1) * 0.5;
            if (vy1 - vy0 < kMinCrossingVertRuleLenPt) {
                continue;
            }
            if (vx >= cr.x0 - kRuleCrossTolPt && vx <= cr.x1 + kRuleCrossTolPt &&
                vy0 <= cr.y + kRuleCrossTolPt && vy1 >= cr.y - kRuleCrossTolPt) {
                crossed_by_vert = true;
                break;
            }
        }
        if (crossed_by_vert) {
            continue;
        }

        // Must straddle zero glyphs.
        bool straddles_glyph = false;
        for (const CharInfo& c : page.chars) {
            if (c.space) {
                continue;
            }
            if (c.right > cr.x0 && c.left < cr.x1 &&
                c.bottom < cr.y + kGlyphStraddleSlackPt &&
                c.top > cr.y - kGlyphStraddleSlackPt) {
                straddles_glyph = true;
                break;
            }
        }
        if (straddles_glyph) {
            continue;
        }

        // Clear of all Table blocks (prevents booktabs \toprule/\midrule/\bottomrule),
        // decorative heading underlines, and TeX footnote separator rules.
        bool vetoed_by_block = false;
        for (const Block& b : page_blocks) {
            if (b.kind == BlockKind::Table) {
                if (cr.x1 >= b.x0 - body_size && cr.x0 <= b.x1 + body_size &&
                    cr.y <= b.y_top + kTableRuleClearanceScale * body_size &&
                    cr.y >= b.y_bottom - kTableRuleClearanceScale * body_size) {
                    vetoed_by_block = true;
                    break;
                }
                continue;
            }
            if (!b.is_footnote && b.lines.size() <= kHeadingUnderlineMaxLines &&
                (b.bold_majority || b.size >= kHeadingUnderlineSizeScale * body_size) &&
                cr.x1 >= b.x0 && cr.x0 <= b.x1 &&
                b.y_bottom >= cr.y &&
                b.y_bottom - cr.y <= kHeadingUnderlineMaxGapScale * (std::max)(b.size, body_size)) {
                vetoed_by_block = true;
                break;
            }
            if (!b.is_footnote && b.bold_majority && starts_with_caption_keyword(b) &&
                cr.x1 >= b.x0 - body_size && cr.x0 <= b.x1 + body_size) {
                const double cap_tol = kCaptionRuleMaxGapScale * (std::max)(b.size, body_size);
                const bool overline = cr.y >= b.y_top && cr.y - b.y_top <= cap_tol;
                const bool underline = b.y_bottom >= cr.y && b.y_bottom - cr.y <= cap_tol;
                if (overline || underline) {
                    vetoed_by_block = true;
                    break;
                }
            }
            if (b.is_footnote && cr.x1 >= b.x0 - body_size && cr.x0 <= b.x1 + body_size &&
                cr.y >= b.y_top && cr.y - b.y_top <= kFootnoteRuleMaxGapScale * body_size) {
                vetoed_by_block = true;
                break;
            }
        }
        if (vetoed_by_block) {
            continue;
        }

        for (const std::size_t idx : cr.indices) {
            if (idx < consumed_rules.size()) {
                consumed_rules[idx] = 1;
            }
        }

        Block hr;
        hr.kind = BlockKind::HorizontalRule;
        hr.page = page.index;
        hr.x0 = cr.x0;
        hr.x1 = cr.x1;
        hr.y_top = cr.y;
        hr.y_bottom = cr.y;
        hr.size = body_size;

        auto pos = page_blocks.end();
        for (auto it = page_blocks.begin(); it != page_blocks.end(); ++it) {
            const bool col_overlaps = (std::min)(it->x1, hr.x1) > (std::max)(it->x0, hr.x0);
            if (col_overlaps && it->y_top < hr.y_top) {
                pos = it;
                break;
            }
        }
        if (pos == page_blocks.end()) {
            for (auto it = page_blocks.begin(); it != page_blocks.end(); ++it) {
                if (it->y_top < hr.y_top) {
                    pos = it;
                    break;
                }
            }
        }
        page_blocks.insert(pos, std::move(hr));
    }

    // Never leave two consecutive HorizontalRule blocks with no content between them
    // (e.g. double-ruled title banners).
    std::size_t out_idx = 0U;
    for (std::size_t i = 0U; i < page_blocks.size(); ++i) {
        if (page_blocks[i].kind == BlockKind::HorizontalRule && out_idx > 0U &&
            page_blocks[out_idx - 1U].kind == BlockKind::HorizontalRule) {
            continue;
        }
        if (out_idx != i) {
            page_blocks[out_idx] = std::move(page_blocks[i]);
        }
        ++out_idx;
    }
    page_blocks.resize(out_idx);
}

void resolve_run_links(std::vector<StyledRun>& runs, std::span<const PageLink> links) {
    if (links.empty()) {
        return;
    }
    for (StyledRun& r : runs) {
        if (r.link_id != 0U && r.link_url.empty() && r.link_id <= links.size()) {
            r.link_url = links[r.link_id - 1U].uri;
        }
    }
}

void resolve_block_links(Block& b, std::span<const PageLink> links) {
    if (links.empty()) {
        return;
    }
    for (Line& ln : b.lines) {
        resolve_run_links(ln.runs, links);
    }
    for (auto& row : b.table_cells) {
        for (TableCell& cell : row) {
            resolve_run_links(cell, links);
        }
    }
}

}  // namespace

static Document run_analyze_layout(
    const DocMeta& meta, const std::vector<PageData>& pages, const AnalyzeParams& params,
    const std::vector<std::unique_ptr<DocumentPass>>& pipeline) {
    Document doc;
    doc.meta = meta;
    doc.body_size = compute_body_size(pages);

    std::vector<PageLines> page_lines;
    std::vector<std::vector<LatticeGroup>> lattice_groups(pages.size());
    std::vector<std::vector<char>> page_consumed_rules(pages.size());
    page_lines.reserve(pages.size());
    for (std::size_t pi = 0U; pi < pages.size(); ++pi) {
        const PageData& page = pages[pi];

        // Bordered-grid tables recovered from the ruling lattice are extracted
        // first; their characters are withheld from the text-geometry pipeline so
        // a heavily wrapped grid is not shredded by the column split.
        std::vector<char> consumed_chars;
        page_consumed_rules[pi].assign(page.rules.size(), 0);
        if (params.detect_tables) {
            lattice_groups[pi] = detect_ruled_tables(page, doc.body_size, consumed_chars,
                                                     &page_consumed_rules[pi]);
        }

        std::vector<CharInfo> working_chars;
        const std::vector<CharInfo>* char_source = &page.chars;
        if (!page.rules.empty()) {
            working_chars = page.chars;
            apply_midline_strikethrough_rules(working_chars, consumed_chars, page.rules,
                                              doc.body_size, page_consumed_rules[pi]);
            char_source = &working_chars;
        }

        std::vector<const CharInfo*> chars;
        chars.reserve(char_source->size());
        for (std::size_t k = 0U; k < char_source->size(); ++k) {
            if (consumed_chars.empty() || consumed_chars[k] == 0) {
                chars.push_back(&(*char_source)[k]);
            }
        }

        std::vector<std::vector<const CharInfo*>> regions;
        split_region(std::move(chars), 0, regions);

        PageLines pl;
        pl.index = page.index;
        pl.width = page.width;
        pl.height = page.height;
        pl.regions.reserve(regions.size());
        for (const auto& region : regions) {
            pl.regions.push_back(build_lines(region));
        }
        page_lines.push_back(std::move(pl));
    }

    // Remove running headers/footers at the line level before blocks form, so a
    // footer can never fuse with an adjacent caption or reference.
    if (params.strip_headers_footers) {
        strip_running_chrome(page_lines);
    }

    for (std::size_t pi = 0U; pi < page_lines.size(); ++pi) {
        PageLines& pl = page_lines[pi];
        // The page text column's left edge: the leftmost start of the long
        // (body) lines. Display equations sit well to the right of it.
        double col_left = kCoordSentinel;
        double any_left = kCoordSentinel;
        for (const auto& lines : pl.regions) {
            for (const Line& ln : lines) {
                any_left = (std::min)(any_left, ln.x0);
                if (count_glyphs(ln) >= kBodyColumnMinGlyphs) {
                    col_left = (std::min)(col_left, ln.x0);
                }
            }
        }
        if (col_left > kCoordSentinelThreshold) {
            col_left = any_left > kCoordSentinelThreshold ? 0.0 : any_left;
        }

        std::vector<Block> page_blocks;
        page_blocks.reserve(pl.regions.size());
        for (auto& lines : pl.regions) {
            region_to_blocks(std::move(lines), pl.index, params, col_left, doc.body_size,
                             pl.width, page_blocks, pages[pi].rules, &page_consumed_rules[pi]);
        }

        // Splice ruled-grid tables into reading order by their top edge: each
        // group inserts ahead of the first text block that starts below it.
        for (auto& group : lattice_groups[pi]) {
            auto pos = page_blocks.end();
            for (auto it = page_blocks.begin(); it != page_blocks.end(); ++it) {
                if (it->y_top < group.y_top) {
                    pos = it;
                    break;
                }
            }
            page_blocks.insert(pos, std::make_move_iterator(group.blocks.begin()),
                               std::make_move_iterator(group.blocks.end()));
        }

        emit_standalone_horizontal_rules(pages[pi], doc.body_size, page_consumed_rules[pi],
                                         page_blocks);

        for (Block& b : page_blocks) {
            resolve_block_links(b, pages[pi].links);
        }

        doc.blocks.reserve(doc.blocks.size() + page_blocks.size());
        for (Block& b : page_blocks) {
            doc.blocks.push_back(std::move(b));
        }
    }

    // Document-level cleanup: reuse the cached Strategy pipeline over doc.blocks.
    const PassContext ctx{pages, params};
    for (const auto& pass : pipeline) {
        pass->run(doc, ctx);
    }

    // Resolve any link_id regenerated from Line::chars during post-block passes
    // (such as FoldTocOrphanRowsPass).
    std::unordered_map<int, const PageData*> page_by_index;
    page_by_index.reserve(pages.size());
    for (const PageData& p : pages) {
        if (!p.links.empty()) {
            page_by_index.insert_or_assign(p.index, &p);
        }
    }
    if (!page_by_index.empty()) {
        for (Block& b : doc.blocks) {
            const auto it = page_by_index.find(b.page);
            if (it != page_by_index.end()) {
                resolve_block_links(b, it->second->links);
            }
        }
    }

    return doc;
}

}  // namespace pdf2md::layout

namespace pdf2md {

struct LayoutAnalyzer::Impl {
    AnalyzeParams params;
    std::vector<std::unique_ptr<layout::DocumentPass>> pipeline;

    explicit Impl(AnalyzeParams p)
        : params(p), pipeline(layout::build_pass_pipeline()) {}

    Impl(const Impl& other)
        : params(other.params), pipeline(layout::build_pass_pipeline()) {}

    Impl& operator=(const Impl& other) {
        if (this != &other) {
            params = other.params;
            if (pipeline.empty()) {
                pipeline = layout::build_pass_pipeline();
            }
        }
        return *this;
    }

    Impl(Impl&&) noexcept = default;
    Impl& operator=(Impl&&) noexcept = default;
};

LayoutAnalyzer::LayoutAnalyzer(AnalyzeParams params)
    : impl_(std::make_unique<Impl>(params)) {}

LayoutAnalyzer::LayoutAnalyzer(const LayoutAnalyzer& other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

LayoutAnalyzer& LayoutAnalyzer::operator=(const LayoutAnalyzer& other) {
    if (this == &other) {
        return *this;
    }
    if (!other.impl_) {
        impl_.reset();
        return *this;
    }
    auto copy = std::make_unique<Impl>(*other.impl_);
    impl_ = std::move(copy);
    return *this;
}

LayoutAnalyzer::LayoutAnalyzer(LayoutAnalyzer&& other) noexcept = default;

LayoutAnalyzer& LayoutAnalyzer::operator=(LayoutAnalyzer&& other) noexcept = default;

LayoutAnalyzer::~LayoutAnalyzer() = default;

Document LayoutAnalyzer::analyze(const DocMeta& meta, const std::vector<PageData>& pages) const {
    if (!impl_) {
        return LayoutAnalyzer{}.analyze(meta, pages);
    }
    return layout::run_analyze_layout(meta, pages, impl_->params, impl_->pipeline);
}

Document analyze_layout(const DocMeta& meta, const std::vector<PageData>& pages,
                        const AnalyzeParams& params) {
    return LayoutAnalyzer(params).analyze(meta, pages);
}

}  // namespace pdf2md
