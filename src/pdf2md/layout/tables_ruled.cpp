#include "pdf2md/layout/tables_ruled.h"

#include "pdf2md/layout/math.h"
#include "pdf2md/model/document_utils.h"
#include "pdf2md/model/geometry.h"
#include "pdf2md/utf8.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pdf2md::layout {

using model::finalize_block_geometry;
using model::kCoordSentinel;
using model::kMinFontSizePt;

namespace {

// Ruled-lattice clustering and cell-assignment thresholds.
constexpr double kRulePosTolPt = 2.5;
constexpr double kRuleJoinTolPt = 3.5;
constexpr double kCellFillMinRatio = 0.55;
constexpr double kCellLineClusterScale = 0.6;
constexpr std::size_t kMinRulesForGrid = 6U;
constexpr std::size_t kMinGridLinesPerAxis = 3U;
constexpr double kStraddleMaxGapScale = 0.6;
constexpr double kMinGridFillRatio = 0.33;
constexpr std::size_t kMinOverrideCols = 3U;
constexpr double kBoxFrameMinSpanRatio = 0.5;

// Ruled-cell alignment voting thresholds.
constexpr double kAlignMaxCellFillRatio = 0.75;
constexpr double kRightAlignPadMultiplier = 2.0;
constexpr double kRightAlignMinLeadPadScale = 0.5;
constexpr double kRightAlignMaxTrailPadScale = 1.5;
constexpr double kRightAlignMaxTrailCellRatio = 0.18;
constexpr double kCenterAlignMinTolPt = 3.0;
constexpr double kCenterAlignTolScale = 0.35;
constexpr double kCenterAlignMinPadScale = 0.8;
constexpr std::size_t kMinMajorityCells = 2U;
constexpr std::size_t kMajorityNum = 2U;
constexpr std::size_t kMajorityDen = 3U;

struct GridLine {
    double pos = 0.0;              // horizontal: y; vertical: x
    double lo = 0.0, hi = 0.0;     // horizontal: [x0,x1]; vertical: [y0,y1]
};

// Clusters axis-parallel rules of one orientation into grid lines.
std::vector<GridLine> cluster_grid_lines(std::span<const RuleSegment> rules, bool horizontal) {
    struct Seg {
        double pos = 0.0;
        double lo = 0.0;
        double hi = 0.0;
    };
    std::vector<Seg> segs;
    segs.reserve(rules.size());
    for (const RuleSegment& r : rules) {
        if (r.horizontal != horizontal) {
            continue;
        }
        if (horizontal) {
            segs.push_back(
                {(r.y0 + r.y1) / 2.0, (std::min)(r.x0, r.x1), (std::max)(r.x0, r.x1)});
        } else {
            segs.push_back(
                {(r.x0 + r.x1) / 2.0, (std::min)(r.y0, r.y1), (std::max)(r.y0, r.y1)});
        }
    }
    std::sort(segs.begin(), segs.end(), [](const Seg& a, const Seg& b) noexcept {
        if (a.pos != b.pos) {
            return a.pos < b.pos;
        }
        if (a.lo != b.lo) {
            return a.lo < b.lo;
        }
        return a.hi < b.hi;
    });

    std::vector<GridLine> lines;
    std::size_t i = 0U;
    while (i < segs.size()) {
        std::size_t j = i;
        while (j + 1U < segs.size() && segs[j + 1U].pos - segs[j].pos <= kRulePosTolPt) {
            ++j;
        }
        auto band_begin = segs.begin() + static_cast<std::ptrdiff_t>(i);
        auto band_end = segs.begin() + static_cast<std::ptrdiff_t>(j) + 1;
        std::sort(band_begin, band_end, [](const Seg& a, const Seg& b) noexcept {
            if (a.lo != b.lo) {
                return a.lo < b.lo;
            }
            if (a.hi != b.hi) {
                return a.hi < b.hi;
            }
            return a.pos < b.pos;
        });

        double pos = 0.0;
        for (std::size_t k = i; k <= j; ++k) {
            pos += segs[k].pos;
        }
        pos /= static_cast<double>(j - i + 1U);

        double clo = segs[i].lo;
        double chi = segs[i].hi;
        for (std::size_t k = i + 1U; k <= j; ++k) {
            if (segs[k].lo <= chi + kRuleJoinTolPt) {
                chi = (std::max)(chi, segs[k].hi);
            } else {
                lines.push_back({pos, clo, chi});
                clo = segs[k].lo;
                chi = segs[k].hi;
            }
        }
        lines.push_back({pos, clo, chi});
        i = j + 1U;
    }
    return lines;
}

bool keep_cell_wrap_hyphen(std::u32string_view token) noexcept {
    std::size_t e = token.size();
    while (e > 0U && (token[e - 1U] == U'-' || token[e - 1U] == kUnicodeHyphenCp ||
                      token[e - 1U] == kSoftHyphenCp)) {
        --e;
    }
    if (e == 0U) {
        return false;
    }
    const std::u32string_view body = token.substr(0U, e);
    for (const char32_t c : body) {
        if (c == U'-' || c == kUnicodeHyphenCp || c == U'{' || c == U'}') {
            return true;
        }
    }
    return false;
}

// Renders one cell's characters into styled runs, joining stacked visual lines.
TableCell render_box_cell(std::span<const CharInfo* const> chars,
                          std::span<const double> dividers, double cell_left,
                          double cell_right) {
    std::vector<Line> lines = build_lines(chars);
    const auto connector = [](char32_t c) noexcept {
        return c == U'_' || c == U'.' || c == U'/';
    };
    const auto first_glyph = [](const Line& ln) noexcept -> char32_t {
        for (const CharInfo& c : ln.chars) {
            if (!c.space) {
                return c.code;
            }
        }
        return 0U;
    };
    const auto single_token = [](const Line& ln) noexcept {
        int runs = 0;
        bool in = false;
        for (const CharInfo& c : ln.chars) {
            if (c.space) {
                in = false;
            } else if (!in) {
                ++runs;
                in = true;
            }
        }
        return runs <= 1;
    };

    TableCell cell;
    std::u32string tok_buf;
    const auto trailing_word = [&cell, &tok_buf]() -> std::u32string_view {
        tok_buf.clear();
        for (const StyledRun& r : cell) {
            tok_buf += r.text;
        }
        std::size_t te = tok_buf.size();
        while (te > 0U && tok_buf[te - 1U] == U' ') {
            --te;
        }
        std::size_t tb = te;
        while (tb > 0U && tok_buf[tb - 1U] != U' ') {
            --tb;
        }
        return std::u32string_view(tok_buf).substr(tb, te - tb);
    };

    double prev_baseline = 0.0;
    double prev_x1 = 0.0;
    bool prev_single = false;
    for (Line& ln : lines) {
        if (ln.runs.empty()) {
            continue;
        }
        if (cell.empty()) {
            prev_baseline = ln.baseline;
            prev_x1 = ln.x1;
            prev_single = single_token(ln);
            cell = std::move(ln.runs);
            continue;
        }
        char32_t last = 0U;
        bool last_bold = false;
        for (const StyledRun& r : cell) {
            if (!r.text.empty()) {
                last = r.text.back();
                last_bold = r.bold;
            }
        }
        char32_t first = 0U;
        bool first_bold = false;
        for (const StyledRun& r : ln.runs) {
            if (!r.text.empty()) {
                first = r.text.front();
                first_bold = r.bold;
                break;
            }
        }
        const bool hyph = last == U'-' || last == kUnicodeHyphenCp || last == kSoftHyphenCp;
        const bool camel_hump =
            is_ascii_lower(last) && is_ascii_upper(first) && !last_bold && !first_bold;
        const bool fills_edge = cell_right > cell_left &&
                                (prev_x1 - cell_left) >= kCellFillMinRatio * (cell_right - cell_left);

        bool dot_binds = true;
        if (last == U'.') {
            const std::u32string_view word = trailing_word();
            const char32_t before_dot = word.size() >= 2U ? word[word.size() - 2U] : 0U;
            const bool urlish = word.find(U'/') != std::u32string_view::npos ||
                                word.find(U'@') != std::u32string_view::npos ||
                                word.find(U"::") != std::u32string_view::npos ||
                                word.starts_with(U"www.") ||
                                word.find(U"://") != std::u32string_view::npos;
            if (is_ascii_alpha(before_dot) && is_ascii_alpha(first) && !urlish) {
                dot_binds = false;
            }
        }
        const bool tight = hyph || (connector(last) && (last != U'.' || dot_binds)) ||
                           connector(first) || (camel_hump && prev_single && fills_edge);
        bool split_rule = false;
        for (const double d : dividers) {
            if (d < prev_baseline && d > ln.baseline) {
                split_rule = true;
                break;
            }
        }
        prev_baseline = ln.baseline;
        prev_x1 = ln.x1;
        prev_single = single_token(ln);
        const bool new_item = split_rule || is_bullet_cp(first_glyph(ln));

        if (hyph) {
            const bool keep_hyph = keep_cell_wrap_hyphen(trailing_word());
            for (auto it = cell.rbegin(); it != cell.rend(); ++it) {
                if (!it->text.empty()) {
                    if (keep_hyph) {
                        it->text.back() = U'-';
                    } else {
                        it->text.pop_back();
                    }
                    break;
                }
            }
        }
        if (new_item) {
            StyledRun sep;
            sep.raw = true;
            sep.text = U"<br>";
            cell.push_back(std::move(sep));
        } else if (!tight) {
            StyledRun sep;
            sep.text = U" ";
            cell.push_back(std::move(sep));
        }
        for (StyledRun& r : ln.runs) {
            cell.push_back(std::move(r));
        }
    }
    return cell;
}

int count_text_lines(std::span<const CharInfo* const> cs, double size) {
    std::vector<double> ys;
    ys.reserve(cs.size());
    for (const CharInfo* c : cs) {
        if (!c->space) {
            ys.push_back(c->y);
        }
    }
    std::sort(ys.begin(), ys.end());
    int n = 0;
    double last = -kCoordSentinel;
    const double cluster_step = kCellLineClusterScale * (std::max)(size, kMinFontSizePt);
    for (const double y : ys) {
        if (y - last > cluster_step) {
            ++n;
        }
        last = y;
    }
    return n;
}

// Returns std::nullopt when a cell is empty, multi-line, placeholder-only, or fills
// enough of the column width that alignment is indeterminate; otherwise votes Right,
// Center, or None (left/unaligned).
std::optional<TableAlign> classify_ruled_cell_align(std::span<const CharInfo* const> cs,
                                                    double xa, double xb, double ref_size) {
    const double cell_w = xb - xa;
    if (cs.empty() || cell_w <= 0.0 || count_text_lines(cs, ref_size) != 1) {
        return std::nullopt;
    }
    double text_left = kCoordSentinel;
    double text_right = -kCoordSentinel;
    std::size_t nonspace = 0U;
    std::size_t dashes = 0U;
    for (const CharInfo* c : cs) {
        if (c->space) {
            continue;
        }
        text_left = (std::min)(text_left, c->left);
        text_right = (std::max)(text_right, c->right);
        ++nonspace;
        if (is_dashy_cp(c->code)) {
            ++dashes;
        }
    }
    if (nonspace == 0U || dashes == nonspace) {
        return std::nullopt;
    }
    const double text_w = text_right - text_left;
    if (text_w >= kAlignMaxCellFillRatio * cell_w) {
        return std::nullopt;
    }
    const double pad_l = text_left - xa;
    const double pad_r = xb - text_right;
    const double max_right_pad =
        (std::max)(kRightAlignMaxTrailPadScale * ref_size, kRightAlignMaxTrailCellRatio * cell_w);
    if (pad_r >= -kRulePosTolPt && pad_r <= max_right_pad &&
        pad_l > kRightAlignPadMultiplier * (std::max)(pad_r, 0.0) +
                    kRightAlignMinLeadPadScale * ref_size) {
        return TableAlign::Right;
    }
    const double center_tol = (std::max)(kCenterAlignMinTolPt, kCenterAlignTolScale * ref_size);
    if (std::fabs(pad_l - pad_r) <= center_tol && pad_l >= kCenterAlignMinPadScale * ref_size) {
        return TableAlign::Center;
    }
    return TableAlign::None;
}

void mark_box_frame_rules_consumed(const PageData& page, double x_left, double x_right,
                                   double y_bottom, double y_top, bool consume_interior,
                                   std::vector<char>* consumed_rules) noexcept {
    if (consumed_rules == nullptr) {
        return;
    }
    const double min_span = kBoxFrameMinSpanRatio * (x_right - x_left);
    for (std::size_t ri = 0U; ri < page.rules.size(); ++ri) {
        const RuleSegment& r = page.rules[ri];
        const double rx0 = (std::min)(r.x0, r.x1);
        const double rx1 = (std::max)(r.x0, r.x1);
        const double ry0 = (std::min)(r.y0, r.y1);
        const double ry1 = (std::max)(r.y0, r.y1);
        if (rx0 < x_left - kRulePosTolPt || rx1 > x_right + kRulePosTolPt ||
            ry0 < y_bottom - kRulePosTolPt || ry1 > y_top + kRulePosTolPt) {
            continue;
        }
        if (consume_interior || !r.horizontal || (rx1 - rx0) >= min_span) {
            (*consumed_rules)[ri] = 1;
        }
    }
}

}  // namespace

std::vector<LatticeGroup> detect_ruled_tables(const PageData& page, double body_size,
                                              std::vector<char>& consumed_chars,
                                              std::vector<char>* consumed_rules) {
    std::vector<LatticeGroup> groups;
    consumed_chars.assign(page.chars.size(), 0);
    if (consumed_rules != nullptr) {
        consumed_rules->assign(page.rules.size(), 0);
    }
    if (page.rules.size() < kMinRulesForGrid) {
        return groups;
    }

    const std::vector<GridLine> H = cluster_grid_lines(page.rules, true);
    const std::vector<GridLine> V = cluster_grid_lines(page.rules, false);
    if (H.size() < kMinGridLinesPerAxis || V.size() < kMinGridLinesPerAxis) {
        return groups;
    }

    const std::size_t nH = H.size();
    const std::size_t nV = V.size();

    std::vector<std::size_t> uf(nH + nV);
    for (std::size_t k = 0U; k < uf.size(); ++k) {
        uf[k] = k;
    }
    const auto find = [&uf](std::size_t a) noexcept {
        while (uf[a] != a) {
            uf[a] = uf[uf[a]];
            a = uf[a];
        }
        return a;
    };
    const auto unite = [&uf, &find](std::size_t a, std::size_t b) noexcept {
        uf[find(a)] = find(b);
    };
    const auto crosses = [](const GridLine& h, const GridLine& v) noexcept {
        return v.pos >= h.lo - kRulePosTolPt && v.pos <= h.hi + kRulePosTolPt &&
               h.pos >= v.lo - kRulePosTolPt && h.pos <= v.hi + kRulePosTolPt;
    };
    for (std::size_t a = 0U; a < nH; ++a) {
        for (std::size_t b = 0U; b < nV; ++b) {
            if (crosses(H[a], V[b])) {
                unite(a, nH + b);
            }
        }
    }

    std::vector<std::vector<std::size_t>> comps(uf.size());
    for (std::size_t k = 0U; k < uf.size(); ++k) {
        comps[find(k)].push_back(k);
    }

    for (const auto& idxs : comps) {
        if (idxs.size() < 2U * kMinGridLinesPerAxis) {
            continue;
        }
        std::vector<GridLine> hs;
        std::vector<GridLine> vs;
        hs.reserve(idxs.size());
        vs.reserve(idxs.size());
        for (const std::size_t id : idxs) {
            if (id < nH) {
                hs.push_back(H[id]);
            } else {
                vs.push_back(V[id - nH]);
            }
        }
        if (hs.size() < kMinGridLinesPerAxis || vs.size() < kMinGridLinesPerAxis) {
            continue;
        }

        std::sort(vs.begin(), vs.end(), [](const GridLine& a, const GridLine& b) noexcept {
            if (a.pos != b.pos) {
                return a.pos < b.pos;  // left -> right
            }
            return a.lo < b.lo;
        });
        std::sort(hs.begin(), hs.end(), [](const GridLine& a, const GridLine& b) noexcept {
            if (a.pos != b.pos) {
                return a.pos > b.pos;  // top -> bottom (y descending)
            }
            return a.lo < b.lo;
        });
        const auto dedup = [](std::vector<GridLine>& g) {
            std::vector<GridLine> out;
            out.reserve(g.size());
            for (const GridLine& l : g) {
                if (!out.empty() && std::fabs(l.pos - out.back().pos) <= kRulePosTolPt) {
                    out.back().lo = (std::min)(out.back().lo, l.lo);
                    out.back().hi = (std::max)(out.back().hi, l.hi);
                } else {
                    out.push_back(l);
                }
            }
            g = std::move(out);
        };
        dedup(vs);
        dedup(hs);
        if (hs.size() < kMinGridLinesPerAxis || vs.size() < kMinGridLinesPerAxis) {
            continue;
        }

        const double yTop = hs.front().pos;
        const double yBot = hs.back().pos;
        const double xL = vs.front().pos;
        const double xR = vs.back().pos;
        const auto post_spans = [](const GridLine& v, double yb, double yt) noexcept {
            return v.lo <= yb + kRulePosTolPt && v.hi >= yt - kRulePosTolPt;
        };
        const bool box = post_spans(vs.front(), yBot, yTop) &&
                         post_spans(vs.back(), yBot, yTop) &&
                         hs.front().lo <= xL + kRulePosTolPt &&
                         hs.front().hi >= xR - kRulePosTolPt &&
                         hs.back().lo <= xL + kRulePosTolPt &&
                         hs.back().hi >= xR - kRulePosTolPt;
        if (!box) {
            continue;
        }
        mark_box_frame_rules_consumed(page, xL, xR, yBot, yTop, /*consume_interior=*/false,
                                      consumed_rules);

        const std::size_t ncol = vs.size() - 1U;
        const auto full_width = [xL, xR](const GridLine& h) noexcept {
            return h.lo <= xL + kRulePosTolPt && h.hi >= xR - kRulePosTolPt;
        };
        std::vector<GridLine> rails;
        std::vector<GridLine> dividers;
        rails.reserve(hs.size());
        for (const GridLine& h : hs) {
            if (full_width(h)) {
                rails.push_back(h);
            } else {
                dividers.push_back(h);
            }
        }
        if (rails.size() < kMinGridLinesPerAxis) {
            continue;
        }
        const std::size_t nrow = rails.size() - 1U;

        struct RowInfo {
            double yb = 0.0;
            double yt = 0.0;
            std::vector<std::size_t> posts;
        };
        std::vector<RowInfo> band(nrow);
        for (std::size_t k = 0U; k < nrow; ++k) {
            band[k].yt = rails[k].pos;
            band[k].yb = rails[k + 1U].pos;
            for (std::size_t p = 0U; p < vs.size(); ++p) {
                if (post_spans(vs[p], band[k].yb, band[k].yt)) {
                    band[k].posts.push_back(p);
                }
            }
        }

        std::vector<std::vector<std::vector<const CharInfo*>>> row_cells;
        std::vector<const RowInfo*> row_info;
        std::vector<const CharInfo*> caption_chars;
        std::vector<std::vector<std::size_t>> row_char_idx;
        std::vector<std::size_t> caption_idx;
        std::vector<int> band_role(nrow, -1);
        bool seen_row = false;
        for (std::size_t k = 0U; k < nrow; ++k) {
            const bool full = band[k].posts.size() == 2U && band[k].posts.front() == 0U &&
                              band[k].posts.back() == vs.size() - 1U;
            if (full && !seen_row) {
                band_role[k] = -1;
            } else {
                band_role[k] = static_cast<int>(row_cells.size());
                row_cells.emplace_back(ncol);
                row_char_idx.emplace_back();
                row_info.push_back(&band[k]);
                seen_row = true;
            }
        }
        if (row_cells.empty()) {
            continue;
        }

        // Drop interior posts straddled by continuous text flow on that row.
        {
            std::vector<std::vector<const CharInfo*>> band_chars(nrow);
            for (const CharInfo& c : page.chars) {
                const double cx = (c.left + c.right) / 2.0;
                const double cy = (c.bottom + c.top) / 2.0;
                if (cx < xL || cx > xR || cy < yBot || cy > yTop) {
                    continue;
                }
                for (std::size_t b = 0U; b < nrow; ++b) {
                    if (cy < band[b].yt && cy >= band[b].yb) {
                        band_chars[b].push_back(&c);
                        break;
                    }
                }
            }
            for (std::size_t k = 0U; k < nrow; ++k) {
                std::vector<std::size_t>& posts = band[k].posts;
                if (posts.size() <= 2U || band_chars[k].empty()) {
                    continue;
                }
                const std::vector<Line> blines = build_lines(band_chars[k]);
                const auto straddled = [&blines](double px) noexcept {
                    for (const Line& ln : blines) {
                        const CharInfo* left = nullptr;
                        const CharInfo* right = nullptr;
                        for (const CharInfo& g : ln.chars) {
                            if (g.space) {
                                continue;
                            }
                            if (g.left <= px) {
                                if (left == nullptr || g.right > left->right) {
                                    left = &g;
                                }
                            } else if (right == nullptr || g.left < right->left) {
                                right = &g;
                            }
                        }
                        if (left != nullptr && left->right > px + kRulePosTolPt) {
                            return true;
                        }
                        if (left != nullptr && right != nullptr) {
                            const double gap = right->left - left->right;
                            const double sz = std::max({left->size, right->size, kMinFontSizePt});
                            if (gap < kStraddleMaxGapScale * sz) {
                                return true;
                            }
                        }
                    }
                    return false;
                };
                std::vector<std::size_t> kept;
                kept.reserve(posts.size());
                for (std::size_t t = 0U; t < posts.size(); ++t) {
                    const bool outer = t == 0U || t + 1U == posts.size();
                    if (outer || !straddled(vs[posts[t]].pos)) {
                        kept.push_back(posts[t]);
                    }
                }
                posts = std::move(kept);
            }
        }

        for (std::size_t ci = 0U; ci < page.chars.size(); ++ci) {
            const CharInfo& c = page.chars[ci];
            const double cx = (c.left + c.right) / 2.0;
            const double cy = (c.bottom + c.top) / 2.0;
            if (cx < xL || cx > xR || cy < yBot || cy > yTop) {
                continue;
            }
            std::size_t k = nrow;
            for (std::size_t b = 0U; b < nrow; ++b) {
                if (cy < band[b].yt && cy >= band[b].yb) {
                    k = b;
                    break;
                }
            }
            if (k == nrow) {
                continue;
            }
            const std::vector<std::size_t>& posts = band[k].posts;
            if (posts.size() < 2U || cx < vs[posts.front()].pos || cx > vs[posts.back()].pos) {
                continue;
            }
            std::size_t gcol = posts.front();
            for (std::size_t t = 0U; t + 1U < posts.size(); ++t) {
                if (cx >= vs[posts[t]].pos && cx <= vs[posts[t + 1U]].pos) {
                    gcol = posts[t];
                    break;
                }
            }
            if (band_role[k] < 0) {
                caption_chars.push_back(&c);
                caption_idx.push_back(ci);
            } else {
                const auto r = static_cast<std::size_t>(band_role[k]);
                row_cells[r][gcol].push_back(&c);
                row_char_idx[r].push_back(ci);
            }
        }

        std::size_t filled = 0U;
        std::size_t total_cells = 0U;
        bool multiline = false;
        for (const auto& row : row_cells) {
            for (std::size_t col = 0U; col + 1U < vs.size(); ++col) {
                ++total_cells;
                if (!row[col].empty()) {
                    ++filled;
                    if (!multiline && count_text_lines(row[col], body_size) >= 2) {
                        multiline = true;
                    }
                }
            }
        }
        if (total_cells == 0U) {
            continue;
        }
        const double fill = static_cast<double>(filled) / static_cast<double>(total_cells);
        if (fill < kMinGridFillRatio) {
            continue;
        }

        const bool has_title = !caption_chars.empty();
        const bool override_text = ncol >= kMinOverrideCols || multiline || has_title;
        if (!override_text) {
            continue;
        }

        mark_box_frame_rules_consumed(page, xL, xR, yBot, yTop, /*consume_interior=*/true,
                                      consumed_rules);

        LatticeGroup group;
        group.y_top = yTop;
        if (has_title) {
            std::vector<Line> clines = build_lines(caption_chars);
            if (!clines.empty()) {
                Block cap;
                cap.kind = BlockKind::Paragraph;
                cap.page = page.index;
                cap.lines = std::move(clines);
                finalize_block_geometry(cap);
                group.blocks.push_back(std::move(cap));
                for (const std::size_t ci : caption_idx) {
                    consumed_chars[ci] = 1;
                }
            }
        }

        const double ref_size = (std::max)(body_size, kMinFontSizePt);
        Block tbl;
        tbl.kind = BlockKind::Table;
        tbl.page = page.index;
        tbl.x0 = xL;
        tbl.x1 = xR;
        tbl.y_top = yTop;
        tbl.y_bottom = yBot;
        tbl.size = ref_size;
        tbl.col_edges.reserve(vs.size());
        for (const GridLine& v : vs) {
            tbl.col_edges.push_back(v.pos);
        }
        tbl.table_cells.reserve(row_cells.size());
        std::vector<TableAlign> hdr_votes(ncol, TableAlign::None);
        std::vector<std::size_t> eligible_data(ncol, 0U);
        std::vector<std::size_t> right_votes(ncol, 0U);
        std::vector<std::size_t> center_votes(ncol, 0U);

        std::vector<double> cell_divs;
        for (std::size_t r = 0U; r < row_cells.size(); ++r) {
            const RowInfo& ri = *row_info[r];
            std::vector<TableCell> out_row(ncol);
            std::vector<std::size_t> right(ncol, 0U);
            for (std::size_t t = 0U; t + 1U < ri.posts.size(); ++t) {
                right[ri.posts[t]] = ri.posts[t + 1U];
            }
            for (std::size_t col = 0U; col < ncol; ++col) {
                if (row_cells[r][col].empty()) {
                    continue;
                }
                const std::size_t right_post = right[col] != 0U ? right[col] : col + 1U;
                const double xa = vs[col].pos;
                const double xb = vs[right_post].pos;
                cell_divs.clear();
                for (const GridLine& d : dividers) {
                    if (d.pos > ri.yb + kRulePosTolPt && d.pos < ri.yt - kRulePosTolPt &&
                        d.lo <= xa + kRulePosTolPt && d.hi >= xb - kRulePosTolPt) {
                        cell_divs.push_back(d.pos);
                    }
                }
                if (right[col] == col + 1U && cell_divs.empty()) {
                    const std::optional<TableAlign> vote =
                        classify_ruled_cell_align(row_cells[r][col], xa, xb, ref_size);
                    if (vote.has_value()) {
                        if (r == 0U && row_cells.size() >= 2U) {
                            hdr_votes[col] = *vote;
                        } else if (r >= 1U) {
                            ++eligible_data[col];
                            if (*vote == TableAlign::Right) {
                                ++right_votes[col];
                            } else if (*vote == TableAlign::Center) {
                                ++center_votes[col];
                            }
                        }
                    }
                }
                out_row[col] = render_box_cell(row_cells[r][col], cell_divs, xa, xb);
            }
            tbl.table_cells.push_back(std::move(out_row));
            for (const std::size_t ci : row_char_idx[r]) {
                consumed_chars[ci] = 1;
            }
        }
        tbl.table_align.assign(ncol, TableAlign::None);
        for (std::size_t col = 0U; col < ncol; ++col) {
            const std::size_t elig = eligible_data[col];
            const std::size_t rv = right_votes[col];
            const std::size_t cv = center_votes[col];
            if ((rv >= kMinMajorityCells && rv * kMajorityDen >= elig * kMajorityNum) ||
                (elig == 1U && rv == 1U && hdr_votes[col] == TableAlign::Right)) {
                tbl.table_align[col] = TableAlign::Right;
            } else if ((cv >= kMinMajorityCells && cv * kMajorityDen >= elig * kMajorityNum) ||
                       (elig == 1U && cv == 1U && hdr_votes[col] == TableAlign::Center)) {
                tbl.table_align[col] = TableAlign::Center;
            }
        }
        group.blocks.push_back(std::move(tbl));
        groups.push_back(std::move(group));
    }
    return groups;
}

}  // namespace pdf2md::layout
