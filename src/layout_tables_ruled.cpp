#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "layout_internal.h"
#include "utf8.h"

namespace pdf2md {
namespace detail {

// ---------------------------------------------------------------- ruled grids
//
// A bordered/gridded table stands on a lattice of vector ruling lines that the
// text-geometry table detector cannot always recover: cells that wrap over many
// lines, tall rows, or too few columns starve the alignment signal it relies on.
// When a page carries border rules (collected by the extractor) that form a real
// multi-cell grid, the cells are read straight off the lattice -- text assigned
// to a cell by containment, rows emitted top to bottom, a full-width cell at the
// top emitted as a caption above the table, multi-line cells joining their lines.
//
// Gated tightly so it never fires on a single underline, a figure box or a
// diagram's stroked artwork: a component must enclose a bordered rectangle with
// at least a 2x2 cell grid whose cells are mostly filled with text, and it only
// overrides (consumes) a region when that region is one the text-geometry path
// handles poorly -- three or more columns, a multi-line cell, or a spanning title
// row -- so ordinary key/value boxes keep their existing text-geometry output.

struct GridLine {
    double pos = 0;         // horizontal: y; vertical: x
    double lo = 0, hi = 0;  // horizontal: [x0,x1]; vertical: [y0,y1]
};

// Clusters axis-parallel rules of one orientation into grid lines. Rules within
// posTol on their fixed axis and touching (or nearly) along their span fuse: a
// border drawn as a thin filled rectangle yields two near-coincident edges, and
// a row/column separator is often drawn per cell as several collinear segments.
std::vector<GridLine> cluster_grid_lines(const std::vector<RuleSegment>& rules, bool horizontal) {
    struct Seg { double pos, lo, hi; };
    std::vector<Seg> segs;
    for (const RuleSegment& r : rules) {
        if (r.horizontal != horizontal) continue;
        if (horizontal)
            segs.push_back({(r.y0 + r.y1) / 2.0, std::min(r.x0, r.x1), std::max(r.x0, r.x1)});
        else
            segs.push_back({(r.x0 + r.x1) / 2.0, std::min(r.y0, r.y1), std::max(r.y0, r.y1)});
    }
    std::sort(segs.begin(), segs.end(), [](const Seg& a, const Seg& b) { return a.pos < b.pos; });
    const double posTol = 2.5, joinTol = 3.5;
    std::vector<GridLine> lines;
    size_t i = 0;
    while (i < segs.size()) {
        size_t j = i;
        while (j + 1 < segs.size() && segs[j + 1].pos - segs[j].pos <= posTol) ++j;
        std::vector<Seg> band(segs.begin() + static_cast<std::ptrdiff_t>(i),
                              segs.begin() + static_cast<std::ptrdiff_t>(j) + 1);
        std::sort(band.begin(), band.end(), [](const Seg& a, const Seg& b) { return a.lo < b.lo; });
        double pos = 0;
        for (const Seg& s : band) pos += s.pos;
        pos /= static_cast<double>(band.size());
        double clo = band[0].lo, chi = band[0].hi;
        for (size_t k = 1; k < band.size(); ++k) {
            if (band[k].lo <= chi + joinTol) {
                chi = std::max(chi, band[k].hi);
            } else {
                lines.push_back({pos, clo, chi});
                clo = band[k].lo;
                chi = band[k].hi;
            }
        }
        lines.push_back({pos, clo, chi});
        i = j + 1;
    }
    return lines;
}

// A hyphen at a wrap inside a cell is normally a soft break to drop
// ("informa-"+"tion"). Keep it when the wrapped token is a compound identifier --
// it already carries an interior hyphen ("data-element-interface-name-") -- or is
// a brace-delimited placeholder ("{<...>}"), where the trailing hyphen is a
// literal part of the name, not a syllable split. Only the braces mark a
// placeholder: scope/path glue (`::`, `/`) and square-bracket refs ("[REQ_ID_"+
// "CONSTR_...]") wrap at ordinary syllable/underscore breaks -- e.g.
// "app::util::GenericDataIdenti-fier" -- and must still dehyphenate. `token` is
// the trailing whitespace-delimited word above the wrap, still ending in its
// hyphen glyph.
bool keep_cell_wrap_hyphen(const std::u32string& token) {
    size_t e = token.size();
    while (e > 0 && (token[e - 1] == U'-' || token[e - 1] == 0x2010 || token[e - 1] == 0x00AD))
        --e;
    if (e == 0) return false;
    std::u32string body = token.substr(0, e);
    for (char32_t c : body)
        if (c == U'-' || c == 0x2010) return true;  // compound identifier
    for (char32_t c : body)
        if (c == U'{' || c == U'}') return true;  // brace-delimited placeholder ("{<...>}")
    return false;
}

// Renders one cell/caption's characters into styled runs, joining stacked visual
// lines the way the cell reads: a wrap broken on a hyphen or token connector
// rejoins tight (no space, hyphen dropped); a line that opens a fresh list item
// (a bullet glyph) starts a new line with a hard '<br>' break so a multi-item
// cell keeps its items apart; every other line is an ordinary word wrap and joins
// with a single space (so a phrase set on several lines reads as one phrase).
// `dividers` are y-positions of short intra-cell rules: a break between two lines
// straddling one starts a new item too (stacked entries divided by a rule).
TableCell render_box_cell(const std::vector<const CharInfo*>& chars,
                          const std::vector<double>& dividers = {}, double cell_left = 0,
                          double cell_right = 1e30) {
    std::vector<Line> lines = build_lines(chars);
    std::sort(lines.begin(), lines.end(),
              [](const Line& a, const Line& b) { return a.baseline > b.baseline; });
    auto connector = [](char32_t c) { return c == U'_' || c == U'.' || c == U'/'; };
    auto first_glyph = [](const Line& ln) -> char32_t {
        for (const CharInfo& c : ln.chars)
            if (!c.space) return c.code;
        return 0;
    };
    // A line carrying a single token (no interior word space) -- the shape of a
    // wrapped identifier fragment, as opposed to a multi-word prose line.
    auto single_token = [](const Line& ln) {
        int runs = 0;
        bool in = false;
        for (const CharInfo& c : ln.chars) {
            if (c.space) in = false;
            else if (!in) { ++runs; in = true; }
        }
        return runs <= 1;
    };
    TableCell cell;
    double prev_baseline = 0, prev_x1 = 0;
    bool prev_single = false;
    for (Line& ln : lines) {
        if (ln.runs.empty()) continue;
        if (cell.empty()) {
            cell = ln.runs;
            prev_baseline = ln.baseline;
            prev_x1 = ln.x1;
            prev_single = single_token(ln);
            continue;
        }
        char32_t last = 0;
        for (const StyledRun& r : cell)
            if (!r.text.empty()) last = r.text.back();
        bool last_bold = false;
        for (const StyledRun& r : cell)
            if (!r.text.empty()) last_bold = r.bold;
        char32_t first = 0;
        bool first_bold = false;
        for (const StyledRun& r : ln.runs)
            if (!r.text.empty()) { first = r.text.front(); first_bold = r.bold; break; }
        bool hyph = last == U'-' || last == 0x2010 || last == 0x00AD;
        // A lowercase->uppercase break rejoins tight only as a genuine identifier
        // wrap: the line above is a single, non-bold token that ran well across the
        // cell (a long camelCase name forced to break at the hump), not a word of a
        // phrase set on its own line nor the tail of a wrapped prose sentence. In
        // this document prose line breaks often carry no space glyph, so a
        // single-token line above -- an identifier fragment, never a multi-word
        // prose line -- is what distinguishes the wrap; the fill guard rejects a
        // short stacked word, and bold marks a structural label that never wraps so.
        bool camel_hump = last >= U'a' && last <= U'z' && first >= U'A' && first <= U'Z' &&
                          !last_bold && !first_bold;
        bool fills_edge = cell_right > cell_left &&
                          (prev_x1 - cell_left) >= 0.55 * (cell_right - cell_left);
        // A '.' connector binds a wrapped identifier/URL/path ("www.iso." + "org",
        // "app::core."), but a sentence or abbreviation period between two letters
        // wants a following space ("i.e." + "sends", "memory." + "There"). Tell
        // them apart by the trailing token's shape: bind only when it reads as a
        // URL/path/identifier (holds a '/', '@', '::', or a "www."/"://" host).
        bool dot_binds = true;
        if (last == U'.') {
            std::u32string tok;
            for (const StyledRun& r : cell) tok += r.text;
            size_t te = tok.size();
            while (te > 0 && tok[te - 1] == U' ') --te;
            size_t tb = te;
            while (tb > 0 && tok[tb - 1] != U' ') --tb;
            std::u32string word = tok.substr(tb, te - tb);
            char32_t before_dot = word.size() >= 2 ? word[word.size() - 2] : 0;
            bool urlish = word.find(U'/') != std::u32string::npos ||
                          word.find(U'@') != std::u32string::npos ||
                          word.find(U"::") != std::u32string::npos ||
                          word.find(U"www.") == 0 || word.find(U"://") != std::u32string::npos;
            if (is_ascii_alpha(before_dot) && is_ascii_alpha(first) && !urlish) dot_binds = false;
        }
        bool tight = hyph || (connector(last) && (last != U'.' || dot_binds)) ||
                     connector(first) || (camel_hump && prev_single && fills_edge);
        bool split_rule = false;
        for (double d : dividers)
            if (d < prev_baseline && d > ln.baseline) { split_rule = true; break; }
        prev_baseline = ln.baseline;
        prev_x1 = ln.x1;
        prev_single = single_token(ln);
        bool new_item = split_rule || is_bullet_cp(first_glyph(ln));
        // A wrap hyphen drops unless the wrapped token is a compound identifier or
        // a bracketed placeholder, where the hyphen is literal ("...name-"+"x>}").
        bool keep_hyph = false;
        if (hyph) {
            std::u32string tok;
            for (const StyledRun& r : cell) tok += r.text;
            size_t te = tok.size();
            while (te > 0 && tok[te - 1] == U' ') --te;
            size_t tb = te;
            while (tb > 0 && tok[tb - 1] != U' ') --tb;
            keep_hyph = keep_cell_wrap_hyphen(tok.substr(tb, te - tb));
        }
        if (hyph)
            for (auto it = cell.rbegin(); it != cell.rend(); ++it)
                if (!it->text.empty()) {
                    if (keep_hyph) it->text.back() = U'-';  // normalize a soft/Unicode hyphen
                    else it->text.pop_back();
                    break;
                }
        StyledRun sep;
        if (new_item) {
            sep.raw = true;
            sep.text = U"<br>";
            cell.push_back(sep);
        } else if (!tight) {
            sep.text = U" ";
            cell.push_back(sep);
        }
        for (const StyledRun& r : ln.runs) cell.push_back(r);
    }
    return cell;
}

// Number of distinct baseline lines the characters span (a cell's line count).
int count_text_lines(const std::vector<const CharInfo*>& cs, double size) {
    std::vector<double> ys;
    for (const CharInfo* c : cs)
        if (!c->space) ys.push_back(c->y);
    std::sort(ys.begin(), ys.end());
    int n = 0;
    double last = -1e30;
    for (double y : ys) {
        if (y - last > 0.6 * std::max(size, 1.0)) ++n;
        last = y;
    }
    return n;
}

// Detects bordered-grid tables on a page from its ruling lines. Fills `consumed`
// (aligned to page.chars) with the characters absorbed into an emitted table or
// caption, and returns the block groups (each positioned by its top edge).
std::vector<LatticeGroup> detect_ruled_tables(const PageData& page, double body_size,
                                              std::vector<char>& consumed) {
    std::vector<LatticeGroup> groups;
    consumed.assign(page.chars.size(), 0);
    if (page.rules.size() < 6) return groups;

    std::vector<GridLine> H = cluster_grid_lines(page.rules, true);
    std::vector<GridLine> V = cluster_grid_lines(page.rules, false);
    if (H.size() < 3 || V.size() < 3) return groups;

    const double tol = 2.5;
    const size_t nH = H.size(), nV = V.size();

    // Union-Find over all grid lines: a horizontal and a vertical line that cross
    // belong to the same border network (one table).
    std::vector<size_t> uf(nH + nV);
    for (size_t k = 0; k < uf.size(); ++k) uf[k] = k;
    auto find = [&](size_t a) {
        while (uf[a] != a) { uf[a] = uf[uf[a]]; a = uf[a]; }
        return a;
    };
    auto unite = [&](size_t a, size_t b) { uf[find(a)] = find(b); };
    auto crosses = [&](const GridLine& h, const GridLine& v) {
        return v.pos >= h.lo - tol && v.pos <= h.hi + tol && h.pos >= v.lo - tol &&
               h.pos <= v.hi + tol;
    };
    for (size_t a = 0; a < nH; ++a)
        for (size_t b = 0; b < nV; ++b)
            if (crosses(H[a], V[b])) unite(a, nH + b);

    std::unordered_map<size_t, std::vector<size_t>> comps;
    for (size_t k = 0; k < uf.size(); ++k) comps[find(k)].push_back(k);

    for (auto& [root, idxs] : comps) {
        std::vector<GridLine> hs, vs;
        for (size_t id : idxs) {
            if (id < nH) hs.push_back(H[id]);
            else vs.push_back(V[id - nH]);
        }
        if (hs.size() < 3 || vs.size() < 3) continue;  // need >= 2 rows and >= 2 columns

        std::sort(vs.begin(), vs.end(), [](const GridLine& a, const GridLine& b) {
            return a.pos < b.pos;  // left -> right
        });
        std::sort(hs.begin(), hs.end(), [](const GridLine& a, const GridLine& b) {
            return a.pos > b.pos;  // top -> bottom (y descending)
        });
        auto dedup = [&](std::vector<GridLine>& g) {
            std::vector<GridLine> out;
            for (const GridLine& l : g) {
                if (!out.empty() && std::fabs(l.pos - out.back().pos) <= tol) {
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
        if (hs.size() < 3 || vs.size() < 3) continue;

        const double yTop = hs.front().pos, yBot = hs.back().pos;
        const double xL = vs.front().pos, xR = vs.back().pos;
        auto post_spans = [&](const GridLine& v, double yb, double yt) {
            return v.lo <= yb + tol && v.hi >= yt - tol;
        };
        // A real table box: outer posts span the full height, top/bottom rails
        // span the full width. Diagram artwork rarely satisfies this.
        bool box = post_spans(vs.front(), yBot, yTop) && post_spans(vs.back(), yBot, yTop) &&
                   hs.front().lo <= xL + tol && hs.front().hi >= xR - tol &&
                   hs.back().lo <= xL + tol && hs.back().hi >= xR - tol;
        if (!box) continue;

        const size_t ncol = vs.size() - 1;

        // Row boundaries are the rails that span (almost) the full table width;
        // a rail crossing only some columns is an intra-cell divider (it splits a
        // column's cell into stacked entries, not the whole row). This keeps a
        // multi-line cell -- e.g. a column holding several stacked entries divided
        // by short rules -- as one row with one tall cell, not a stack of rows
        // whose neighbouring cells go empty.
        auto full_width = [&](const GridLine& h) {
            return h.lo <= xL + tol && h.hi >= xR - tol;
        };
        std::vector<GridLine> rails, dividers;
        for (const GridLine& h : hs) (full_width(h) ? rails : dividers).push_back(h);
        if (rails.size() < 3) continue;  // need >= 2 rows
        const size_t nrow = rails.size() - 1;

        // Per-row active posts (those spanning the row band): they cut the row into
        // cells. A row with only the two outer posts is a full-width (spanning)
        // band -- a title/caption when it leads the grid.
        struct RowInfo {
            double yb, yt;
            std::vector<size_t> posts;  // global indices into vs, ascending
        };
        std::vector<RowInfo> band(nrow);
        for (size_t k = 0; k < nrow; ++k) {
            band[k].yt = rails[k].pos;
            band[k].yb = rails[k + 1].pos;
            for (size_t p = 0; p < vs.size(); ++p)
                if (post_spans(vs[p], band[k].yb, band[k].yt)) band[k].posts.push_back(p);
        }

        // Assign each in-box char to its (row, cell). A cell spanning several
        // global columns owns the whole span but reports its leftmost column.
        std::vector<std::vector<std::vector<const CharInfo*>>> row_cells;  // row -> col -> chars
        std::vector<const RowInfo*> row_info;
        std::vector<const CharInfo*> caption_chars;
        std::vector<std::vector<size_t>> row_char_idx;  // parallel: consumed indices per row
        std::vector<size_t> caption_idx;
        std::vector<int> band_role(nrow, -1);  // -1 caption, >=0 row index
        bool seen_row = false;
        for (size_t k = 0; k < nrow; ++k) {
            bool full = band[k].posts.size() == 2 && band[k].posts.front() == 0 &&
                        band[k].posts.back() == vs.size() - 1;
            if (full && !seen_row) {
                band_role[k] = -1;  // leading spanning band -> caption
            } else {
                band_role[k] = static_cast<int>(row_cells.size());
                row_cells.emplace_back(ncol);
                row_char_idx.emplace_back();
                row_info.push_back(&band[k]);
                seen_row = true;
            }
        }
        if (row_cells.empty()) continue;

        // Refine per-row posts: an interior rule that a continuous text segment
        // flows across -- a word straddling the column boundary with no
        // column-sized gap at the rule -- is absent for that row, so the cell
        // spans it. (This also undoes a phantom post produced when two collinear
        // segments of a partial divider fuse across the gap between them.)
        // Assigning glyphs by centre against such a post would chop the word at
        // the boundary; dropping the straddled post keeps the word whole.
        {
            std::vector<std::vector<const CharInfo*>> band_chars(nrow);
            for (const CharInfo& c : page.chars) {
                double cx = (c.left + c.right) / 2.0, cy = (c.bottom + c.top) / 2.0;
                if (cx < xL || cx > xR || cy < yBot || cy > yTop) continue;
                for (size_t b = 0; b < nrow; ++b)
                    if (cy < band[b].yt && cy >= band[b].yb) { band_chars[b].push_back(&c); break; }
            }
            for (size_t k = 0; k < nrow; ++k) {
                std::vector<size_t>& posts = band[k].posts;
                if (posts.size() <= 2 || band_chars[k].empty()) continue;
                std::vector<Line> blines = build_lines(band_chars[k]);
                // A post is straddled when, on some visual line of the row, text
                // flows across it with no word break: either a glyph box covers
                // the post, or the glyphs bracketing it are closer than a word
                // space. A genuine column boundary instead sits in a cell-padding
                // gap wider than a word space, even when narrower than the
                // page-wide column-gap threshold (so this is finer than segments).
                auto straddled = [&](double px) {
                    for (const Line& ln : blines) {
                        const CharInfo* left = nullptr;
                        const CharInfo* right = nullptr;
                        for (const CharInfo& g : ln.chars) {
                            if (g.space) continue;
                            if (g.left <= px) {
                                if (!left || g.right > left->right) left = &g;
                            } else if (!right || g.left < right->left) {
                                right = &g;
                            }
                        }
                        if (left && left->right > px + tol) return true;  // glyph covers post
                        if (left && right) {
                            double gap = right->left - left->right;
                            double sz = std::max({left->size, right->size, 1.0});
                            if (gap < 0.6 * sz) return true;  // continuous flow, no word break
                        }
                    }
                    return false;
                };
                std::vector<size_t> kept;
                for (size_t t = 0; t < posts.size(); ++t) {
                    bool outer = t == 0 || t + 1 == posts.size();
                    if (outer || !straddled(vs[posts[t]].pos)) kept.push_back(posts[t]);
                }
                posts = std::move(kept);
            }
        }

        for (size_t ci = 0; ci < page.chars.size(); ++ci) {
            const CharInfo& c = page.chars[ci];
            double cx = (c.left + c.right) / 2.0, cy = (c.bottom + c.top) / 2.0;
            if (cx < xL || cx > xR || cy < yBot || cy > yTop) continue;
            // Locate the row band.
            size_t k = nrow;
            for (size_t b = 0; b < nrow; ++b)
                if (cy < band[b].yt && cy >= band[b].yb) { k = b; break; }
            if (k == nrow) continue;
            const std::vector<size_t>& posts = band[k].posts;
            if (posts.size() < 2 || cx < vs[posts.front()].pos || cx > vs[posts.back()].pos)
                continue;
            // Cell = active-post interval containing cx; report leftmost column.
            size_t gcol = posts.front();
            for (size_t t = 0; t + 1 < posts.size(); ++t)
                if (cx >= vs[posts[t]].pos && cx <= vs[posts[t + 1]].pos) { gcol = posts[t]; break; }
            if (band_role[k] < 0) {
                caption_chars.push_back(&c);
                caption_idx.push_back(ci);
            } else {
                size_t r = static_cast<size_t>(band_role[k]);
                row_cells[r][gcol].push_back(&c);
                row_char_idx[r].push_back(ci);
            }
        }

        // Fill-ratio and gate signals.
        size_t filled = 0, total_cells = 0;
        bool multiline = false;
        for (const auto& row : row_cells) {
            for (size_t col = 0; col + 1 < vs.size(); ++col) {
                // Only count cells at real column starts (a spanned cell's extra
                // columns are structurally empty and must not deflate the ratio).
                bool has = !row[col].empty();
                ++total_cells;
                if (has) {
                    ++filled;
                    if (count_text_lines(row[col], body_size) >= 2) multiline = true;
                }
            }
        }
        if (total_cells == 0) continue;
        double fill = static_cast<double>(filled) / static_cast<double>(total_cells);
        // A real filled grid; diagram lattices (sparse text) are rejected.
        if (fill < 0.33) continue;

        bool has_title = !caption_chars.empty();
        bool override_text = ncol >= 3 || multiline || has_title;
        if (!override_text) continue;  // leave simple key/value boxes to text geometry

        // Commit: consume characters and build blocks.
        LatticeGroup group;
        group.y_top = yTop;
        if (has_title) {
            std::vector<Line> clines = build_lines(caption_chars);
            std::sort(clines.begin(), clines.end(),
                      [](const Line& a, const Line& b) { return a.baseline > b.baseline; });
            if (!clines.empty()) {
                Block cap;
                cap.kind = BlockKind::Paragraph;
                cap.page = page.index;
                cap.lines = std::move(clines);
                finalize_block_geometry(cap);
                group.blocks.push_back(std::move(cap));
                for (size_t ci : caption_idx) consumed[ci] = 1;
            }
        }

        Block tbl;
        tbl.kind = BlockKind::Table;
        tbl.page = page.index;
        tbl.x0 = xL;
        tbl.x1 = xR;
        tbl.y_top = yTop;
        tbl.y_bottom = yBot;
        tbl.size = std::max(body_size, 1.0);
        tbl.col_edges.reserve(vs.size());
        for (const GridLine& v : vs) tbl.col_edges.push_back(v.pos);
        tbl.table_cells.reserve(row_cells.size());
        for (size_t r = 0; r < row_cells.size(); ++r) {
            const RowInfo& ri = *row_info[r];
            std::vector<TableCell> out_row(ncol);
            // Right edge of the cell starting at each active-post column.
            std::vector<size_t> right(ncol, 0);
            for (size_t t = 0; t + 1 < ri.posts.size(); ++t) right[ri.posts[t]] = ri.posts[t + 1];
            for (size_t col = 0; col < ncol; ++col) {
                if (row_cells[r][col].empty()) continue;
                // Intra-cell dividers: short rules crossing this cell's column
                // span within its row band split stacked entries with a break.
                double xa = vs[col].pos, xb = vs[right[col] ? right[col] : col + 1].pos;
                std::vector<double> divs;
                for (const GridLine& d : dividers)
                    if (d.pos > ri.yb + tol && d.pos < ri.yt - tol && d.lo <= xa + tol &&
                        d.hi >= xb - tol)
                        divs.push_back(d.pos);
                out_row[col] = render_box_cell(row_cells[r][col], divs, xa, xb);
            }
            tbl.table_cells.push_back(std::move(out_row));
            for (size_t ci : row_char_idx[r]) consumed[ci] = 1;
        }
        group.blocks.push_back(std::move(tbl));
        groups.push_back(std::move(group));
    }
    return groups;
}

}  // namespace detail
}  // namespace pdf2md
