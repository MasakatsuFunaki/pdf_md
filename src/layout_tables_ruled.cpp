#include "layout_internal.h"

#include "utf8.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pdf2md {
namespace detail {

namespace {

constexpr char32_t kSoftHyphenCp = 0x00AD;
constexpr char32_t kUnicodeHyphenCp = 0x2010;

// Ruled-lattice clustering and cell-assignment thresholds.
constexpr double kRulePosTolPt = 2.5;
constexpr double kRuleJoinTolPt = 3.5;
constexpr double kCellFillMinRatio = 0.55;
constexpr double kCellLineClusterScale = 0.6;
constexpr size_t kMinRulesForGrid = 6;
constexpr size_t kMinGridLinesPerAxis = 3;
constexpr double kStraddleMaxGapScale = 0.6;
constexpr double kMinGridFillRatio = 0.33;
constexpr size_t kMinOverrideCols = 3;

struct GridLine {
    double pos = 0;         // horizontal: y; vertical: x
    double lo = 0, hi = 0;  // horizontal: [x0,x1]; vertical: [y0,y1]
};

// Clusters axis-parallel rules of one orientation into grid lines.
std::vector<GridLine> cluster_grid_lines(std::span<const RuleSegment> rules, bool horizontal) {
    struct Seg {
        double pos = 0;
        double lo = 0;
        double hi = 0;
    };
    std::vector<Seg> segs;
    segs.reserve(rules.size());
    for (const RuleSegment& r : rules) {
        if (r.horizontal != horizontal) {
            continue;
        }
        if (horizontal) {
            segs.push_back({(r.y0 + r.y1) / 2.0, std::min(r.x0, r.x1), std::max(r.x0, r.x1)});
        } else {
            segs.push_back({(r.x0 + r.x1) / 2.0, std::min(r.y0, r.y1), std::max(r.y0, r.y1)});
        }
    }
    std::sort(segs.begin(), segs.end(), [](const Seg& a, const Seg& b) {
        if (a.pos != b.pos) {
            return a.pos < b.pos;
        }
        if (a.lo != b.lo) {
            return a.lo < b.lo;
        }
        return a.hi < b.hi;
    });

    std::vector<GridLine> lines;
    size_t i = 0;
    while (i < segs.size()) {
        size_t j = i;
        while (j + 1 < segs.size() && segs[j + 1].pos - segs[j].pos <= kRulePosTolPt) {
            ++j;
        }
        auto band_begin = segs.begin() + static_cast<std::ptrdiff_t>(i);
        auto band_end = segs.begin() + static_cast<std::ptrdiff_t>(j) + 1;
        std::sort(band_begin, band_end, [](const Seg& a, const Seg& b) {
            if (a.lo != b.lo) {
                return a.lo < b.lo;
            }
            if (a.hi != b.hi) {
                return a.hi < b.hi;
            }
            return a.pos < b.pos;
        });

        double pos = 0;
        for (size_t k = i; k <= j; ++k) {
            pos += segs[k].pos;
        }
        pos /= static_cast<double>(j - i + 1);

        double clo = segs[i].lo;
        double chi = segs[i].hi;
        for (size_t k = i + 1; k <= j; ++k) {
            if (segs[k].lo <= chi + kRuleJoinTolPt) {
                chi = std::max(chi, segs[k].hi);
            } else {
                lines.push_back({pos, clo, chi});
                clo = segs[k].lo;
                chi = segs[k].hi;
            }
        }
        lines.push_back({pos, clo, chi});
        i = j + 1;
    }
    return lines;
}

bool keep_cell_wrap_hyphen(std::u32string_view token) noexcept {
    size_t e = token.size();
    while (e > 0 &&
           (token[e - 1] == U'-' || token[e - 1] == kUnicodeHyphenCp || token[e - 1] == kSoftHyphenCp)) {
        --e;
    }
    if (e == 0) {
        return false;
    }
    const std::u32string_view body = token.substr(0, e);
    for (char32_t c : body) {
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
    // build_lines already returns lines sorted descending by baseline, then x0.
    std::vector<Line> lines = build_lines(chars);
    auto connector = [](char32_t c) { return c == U'_' || c == U'.' || c == U'/'; };
    auto first_glyph = [](const Line& ln) -> char32_t {
        for (const CharInfo& c : ln.chars) {
            if (!c.space) {
                return c.code;
            }
        }
        return 0;
    };
    auto single_token = [](const Line& ln) {
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
    auto trailing_word = [&cell, &tok_buf]() -> std::u32string_view {
        tok_buf.clear();
        for (const StyledRun& r : cell) {
            tok_buf += r.text;
        }
        size_t te = tok_buf.size();
        while (te > 0 && tok_buf[te - 1] == U' ') {
            --te;
        }
        size_t tb = te;
        while (tb > 0 && tok_buf[tb - 1] != U' ') {
            --tb;
        }
        return std::u32string_view(tok_buf).substr(tb, te - tb);
    };

    double prev_baseline = 0;
    double prev_x1 = 0;
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
        char32_t last = 0;
        bool last_bold = false;
        for (const StyledRun& r : cell) {
            if (!r.text.empty()) {
                last = r.text.back();
                last_bold = r.bold;
            }
        }
        char32_t first = 0;
        bool first_bold = false;
        for (const StyledRun& r : ln.runs) {
            if (!r.text.empty()) {
                first = r.text.front();
                first_bold = r.bold;
                break;
            }
        }
        const bool hyph = last == U'-' || last == kUnicodeHyphenCp || last == kSoftHyphenCp;
        const bool camel_hump = last >= U'a' && last <= U'z' && first >= U'A' && first <= U'Z' &&
                                !last_bold && !first_bold;
        const bool fills_edge = cell_right > cell_left &&
                                (prev_x1 - cell_left) >= kCellFillMinRatio * (cell_right - cell_left);

        bool dot_binds = true;
        if (last == U'.') {
            const std::u32string_view word = trailing_word();
            const char32_t before_dot = word.size() >= 2 ? word[word.size() - 2] : 0;
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
        for (double d : dividers) {
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
    const double cluster_step = kCellLineClusterScale * std::max(size, kMinFontSizePt);
    for (double y : ys) {
        if (y - last > cluster_step) {
            ++n;
        }
        last = y;
    }
    return n;
}

}  // namespace

std::vector<LatticeGroup> detect_ruled_tables(const PageData& page, double body_size,
                                              std::vector<char>& consumed) {
    std::vector<LatticeGroup> groups;
    consumed.assign(page.chars.size(), 0);
    if (page.rules.size() < kMinRulesForGrid) {
        return groups;
    }

    const std::vector<GridLine> H = cluster_grid_lines(page.rules, true);
    const std::vector<GridLine> V = cluster_grid_lines(page.rules, false);
    if (H.size() < kMinGridLinesPerAxis || V.size() < kMinGridLinesPerAxis) {
        return groups;
    }

    const size_t nH = H.size();
    const size_t nV = V.size();

    // Union-Find over all grid lines: crossing horizontal and vertical lines
    // belong to the same border network.
    std::vector<size_t> uf(nH + nV);
    for (size_t k = 0; k < uf.size(); ++k) {
        uf[k] = k;
    }
    auto find = [&uf](size_t a) {
        while (uf[a] != a) {
            uf[a] = uf[uf[a]];
            a = uf[a];
        }
        return a;
    };
    auto unite = [&uf, &find](size_t a, size_t b) { uf[find(a)] = find(b); };
    auto crosses = [](const GridLine& h, const GridLine& v) {
        return v.pos >= h.lo - kRulePosTolPt && v.pos <= h.hi + kRulePosTolPt &&
               h.pos >= v.lo - kRulePosTolPt && h.pos <= v.hi + kRulePosTolPt;
    };
    for (size_t a = 0; a < nH; ++a) {
        for (size_t b = 0; b < nV; ++b) {
            if (crosses(H[a], V[b])) {
                unite(a, nH + b);
            }
        }
    }

    // Flat contiguous component buckets indexed by Union-Find root: zero hash
    // allocations and 100% deterministic iteration order.
    std::vector<std::vector<size_t>> comps(uf.size());
    for (size_t k = 0; k < uf.size(); ++k) {
        comps[find(k)].push_back(k);
    }

    for (const auto& idxs : comps) {
        if (idxs.size() < 2 * kMinGridLinesPerAxis) {
            continue;
        }
        std::vector<GridLine> hs;
        std::vector<GridLine> vs;
        hs.reserve(idxs.size());
        vs.reserve(idxs.size());
        for (size_t id : idxs) {
            if (id < nH) {
                hs.push_back(H[id]);
            } else {
                vs.push_back(V[id - nH]);
            }
        }
        if (hs.size() < kMinGridLinesPerAxis || vs.size() < kMinGridLinesPerAxis) {
            continue;
        }

        std::sort(vs.begin(), vs.end(), [](const GridLine& a, const GridLine& b) {
            if (a.pos != b.pos) {
                return a.pos < b.pos;  // left -> right
            }
            return a.lo < b.lo;
        });
        std::sort(hs.begin(), hs.end(), [](const GridLine& a, const GridLine& b) {
            if (a.pos != b.pos) {
                return a.pos > b.pos;  // top -> bottom (y descending)
            }
            return a.lo < b.lo;
        });
        auto dedup = [](std::vector<GridLine>& g) {
            std::vector<GridLine> out;
            out.reserve(g.size());
            for (const GridLine& l : g) {
                if (!out.empty() && std::fabs(l.pos - out.back().pos) <= kRulePosTolPt) {
                    out.back().lo = std::min(out.back().lo, l.lo);
                    out.back().hi = std::max(out.back().hi, l.hi);
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
        auto post_spans = [](const GridLine& v, double yb, double yt) {
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

        const size_t ncol = vs.size() - 1;
        auto full_width = [xL, xR](const GridLine& h) {
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
        const size_t nrow = rails.size() - 1;

        struct RowInfo {
            double yb = 0;
            double yt = 0;
            std::vector<size_t> posts;
        };
        std::vector<RowInfo> band(nrow);
        for (size_t k = 0; k < nrow; ++k) {
            band[k].yt = rails[k].pos;
            band[k].yb = rails[k + 1].pos;
            for (size_t p = 0; p < vs.size(); ++p) {
                if (post_spans(vs[p], band[k].yb, band[k].yt)) {
                    band[k].posts.push_back(p);
                }
            }
        }

        std::vector<std::vector<std::vector<const CharInfo*>>> row_cells;
        std::vector<const RowInfo*> row_info;
        std::vector<const CharInfo*> caption_chars;
        std::vector<std::vector<size_t>> row_char_idx;
        std::vector<size_t> caption_idx;
        std::vector<int> band_role(nrow, -1);
        bool seen_row = false;
        for (size_t k = 0; k < nrow; ++k) {
            const bool full = band[k].posts.size() == 2 && band[k].posts.front() == 0 &&
                              band[k].posts.back() == vs.size() - 1;
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
                for (size_t b = 0; b < nrow; ++b) {
                    if (cy < band[b].yt && cy >= band[b].yb) {
                        band_chars[b].push_back(&c);
                        break;
                    }
                }
            }
            for (size_t k = 0; k < nrow; ++k) {
                std::vector<size_t>& posts = band[k].posts;
                if (posts.size() <= 2 || band_chars[k].empty()) {
                    continue;
                }
                const std::vector<Line> blines = build_lines(band_chars[k]);
                auto straddled = [&blines](double px) {
                    for (const Line& ln : blines) {
                        const CharInfo* left = nullptr;
                        const CharInfo* right = nullptr;
                        for (const CharInfo& g : ln.chars) {
                            if (g.space) {
                                continue;
                            }
                            if (g.left <= px) {
                                if (!left || g.right > left->right) {
                                    left = &g;
                                }
                            } else if (!right || g.left < right->left) {
                                right = &g;
                            }
                        }
                        if (left && left->right > px + kRulePosTolPt) {
                            return true;
                        }
                        if (left && right) {
                            const double gap = right->left - left->right;
                            const double sz = std::max({left->size, right->size, kMinFontSizePt});
                            if (gap < kStraddleMaxGapScale * sz) {
                                return true;
                            }
                        }
                    }
                    return false;
                };
                std::vector<size_t> kept;
                kept.reserve(posts.size());
                for (size_t t = 0; t < posts.size(); ++t) {
                    const bool outer = t == 0 || t + 1 == posts.size();
                    if (outer || !straddled(vs[posts[t]].pos)) {
                        kept.push_back(posts[t]);
                    }
                }
                posts = std::move(kept);
            }
        }

        for (size_t ci = 0; ci < page.chars.size(); ++ci) {
            const CharInfo& c = page.chars[ci];
            const double cx = (c.left + c.right) / 2.0;
            const double cy = (c.bottom + c.top) / 2.0;
            if (cx < xL || cx > xR || cy < yBot || cy > yTop) {
                continue;
            }
            size_t k = nrow;
            for (size_t b = 0; b < nrow; ++b) {
                if (cy < band[b].yt && cy >= band[b].yb) {
                    k = b;
                    break;
                }
            }
            if (k == nrow) {
                continue;
            }
            const std::vector<size_t>& posts = band[k].posts;
            if (posts.size() < 2 || cx < vs[posts.front()].pos || cx > vs[posts.back()].pos) {
                continue;
            }
            size_t gcol = posts.front();
            for (size_t t = 0; t + 1 < posts.size(); ++t) {
                if (cx >= vs[posts[t]].pos && cx <= vs[posts[t + 1]].pos) {
                    gcol = posts[t];
                    break;
                }
            }
            if (band_role[k] < 0) {
                caption_chars.push_back(&c);
                caption_idx.push_back(ci);
            } else {
                const size_t r = static_cast<size_t>(band_role[k]);
                row_cells[r][gcol].push_back(&c);
                row_char_idx[r].push_back(ci);
            }
        }

        size_t filled = 0;
        size_t total_cells = 0;
        bool multiline = false;
        for (const auto& row : row_cells) {
            for (size_t col = 0; col + 1 < vs.size(); ++col) {
                ++total_cells;
                if (!row[col].empty()) {
                    ++filled;
                    if (!multiline && count_text_lines(row[col], body_size) >= 2) {
                        multiline = true;
                    }
                }
            }
        }
        if (total_cells == 0) {
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
                for (size_t ci : caption_idx) {
                    consumed[ci] = 1;
                }
            }
        }

        Block tbl;
        tbl.kind = BlockKind::Table;
        tbl.page = page.index;
        tbl.x0 = xL;
        tbl.x1 = xR;
        tbl.y_top = yTop;
        tbl.y_bottom = yBot;
        tbl.size = std::max(body_size, kMinFontSizePt);
        tbl.col_edges.reserve(vs.size());
        for (const GridLine& v : vs) {
            tbl.col_edges.push_back(v.pos);
        }
        tbl.table_cells.reserve(row_cells.size());
        std::vector<double> cell_divs;
        for (size_t r = 0; r < row_cells.size(); ++r) {
            const RowInfo& ri = *row_info[r];
            std::vector<TableCell> out_row(ncol);
            std::vector<size_t> right(ncol, 0);
            for (size_t t = 0; t + 1 < ri.posts.size(); ++t) {
                right[ri.posts[t]] = ri.posts[t + 1];
            }
            for (size_t col = 0; col < ncol; ++col) {
                if (row_cells[r][col].empty()) {
                    continue;
                }
                const double xa = vs[col].pos;
                const double xb = vs[right[col] ? right[col] : col + 1].pos;
                cell_divs.clear();
                for (const GridLine& d : dividers) {
                    if (d.pos > ri.yb + kRulePosTolPt && d.pos < ri.yt - kRulePosTolPt &&
                        d.lo <= xa + kRulePosTolPt && d.hi >= xb - kRulePosTolPt) {
                        cell_divs.push_back(d.pos);
                    }
                }
                out_row[col] = render_box_cell(row_cells[r][col], cell_divs, xa, xb);
            }
            tbl.table_cells.push_back(std::move(out_row));
            for (size_t ci : row_char_idx[r]) {
                consumed[ci] = 1;
            }
        }
        group.blocks.push_back(std::move(tbl));
        groups.push_back(std::move(group));
    }
    return groups;
}

}  // namespace detail
}  // namespace pdf2md
