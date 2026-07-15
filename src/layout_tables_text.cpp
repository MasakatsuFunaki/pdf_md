#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "layout_internal.h"

namespace pdf2md {
namespace detail {

// ---------------------------------------------------------------- tables
//
// Scientific tables carry sparse grids (empty cells, spanning group labels),
// multi-line and two-level headers, and cells with superscripts/subscripts and
// bold highlights. The detector therefore (1) groups vertically contiguous rows,
// (2) derives a *global* set of column separators from the inter-glyph gaps seen
// across all rows -- so a boundary established by one clear row splits a cell
// that a sparser row set too close together (d_k / d_v) -- (3) re-slices every
// row at those boundaries, (4) folds tightly-stacked leading lines into one
// header row and stray sub-pitch label lines into their neighbour, and (5)
// renders each cell through the line machinery so math and bold survive.

// A size for table geometry robust to degenerate font matrices: some producers
// encode every glyph at font size 1.0 and scale by the text matrix, so the font
// size collapses while the glyph boxes stay real. When the reported size is far
// below the glyph heights, fall back to the tallest box (the cap/ascender span,
// a proxy for the line height that the paragraph-gap test needs).
double robust_line_size(const Line& line) {
    std::vector<double> heights;
    for (const CharInfo& c : line.chars)
        if (!c.space && c.top > c.bottom) heights.push_back(c.top - c.bottom);
    double medh = median(heights);
    if (line.size >= 0.5 * medh) return std::max(line.size, 1.0);
    double maxh = heights.empty() ? 0.0 : *std::max_element(heights.begin(), heights.end());
    return std::max({maxh, medh, 1.0});
}

// Non-space glyph count of a char range (used as a cheap "text length").
size_t range_glyphs(const Line& line, size_t lo, size_t hi) {
    size_t n = 0;
    for (size_t i = lo; i <= hi && i < line.chars.size(); ++i)
        if (!line.chars[i].space) ++n;
    return n;
}

// Renders a contiguous glyph range of a line into styled runs, so a cell keeps
// its superscripts/subscripts (via the math renderer), bold and code styling.
TableCell render_cell_runs(const Line& line, size_t lo, size_t hi) {
    Line sub;
    for (size_t i = lo; i <= hi && i < line.chars.size(); ++i)
        sub.chars.push_back(line.chars[i]);
    while (!sub.chars.empty() && sub.chars.front().space) sub.chars.erase(sub.chars.begin());
    while (!sub.chars.empty() && sub.chars.back().space) sub.chars.pop_back();
    if (sub.chars.empty()) return {};
    // A cell whose entire ink is placeholder dashes -- each a single dash the
    // producer draws in an otherwise empty grid cell -- carries no data. When
    // several such cells collapse into one (a row that spans fewer detected
    // columns than the grid drew) their dashes must not concatenate into a run
    // like "- - - -"; keep a single placeholder dash.
    {
        size_t nonspace = 0, dashes = 0;
        for (const CharInfo& c : sub.chars) {
            if (c.space) continue;
            ++nonspace;
            if (is_dashy_cp(c.code)) ++dashes;
        }
        if (nonspace >= 2 && dashes == nonspace) {
            CharInfo keep;
            for (const CharInfo& c : sub.chars)
                if (!c.space) { keep = c; break; }
            sub.chars.assign(1, keep);
        }
    }
    // A cell whose glyphs are mostly bold is a highlighted value; tag every run
    // (math included) bold so the writer emphasises it.
    int bold = 0, total = 0;
    for (const CharInfo& c : sub.chars)
        if (!c.space) { ++total; if (c.bold) ++bold; }
    finalize_line(sub);
    // Inherit the row's word-gap threshold. Isolated in a cell, a short numeric
    // or e-mail token has no wide column gaps to calibrate against, so bimodal
    // detection would misread a narrow-glyph gap (the '.' in "google.com") as a
    // word break; real word breaks still arrive as PDFium space glyphs.
    if (line.word_gap > 0) sub.word_gap = line.word_gap;
    build_runs(sub);
    if (total > 0 && bold * 2 > total)
        for (StyledRun& r : sub.runs) r.bold = true;
    return sub.runs;
}

// Appends src to dst as a continuation (a space between them when both present).
// `tight` suppresses the separating space: a wrapped identifier fragment breaks
// at a connector (an underscore/dot/slash) and rejoins with no space.
void append_cell(TableCell& dst, const TableCell& src, bool tight = false) {
    if (src.empty()) return;
    if (!dst.empty() && !tight) {
        StyledRun sep;
        sep.text = U" ";
        dst.push_back(sep);
    }
    for (const StyledRun& r : src) dst.push_back(r);
}

// A wrapped row-label rejoins tight (no space) when the break sits on a token
// connector -- an identifier split at an underscore/dot/slash across lines
// ("[REQ_ID_" + "CONSTR_" -> "[REQ_ID_CONSTR_"). A word wrap ("Forwarding
// Header" + "File:") ends on a letter and keeps its space.
bool label_join_tight(const TableCell& dst, const TableCell& src) {
    auto connector = [](char32_t c) { return c == U'_' || c == U'.' || c == U'/' || c == U'-'; };
    char32_t last = 0, first = 0;
    for (const StyledRun& r : dst)
        if (!r.text.empty()) last = r.text.back();
    for (const StyledRun& r : src)
        if (!r.text.empty()) { first = r.text.front(); break; }
    return connector(last) || connector(first);
}

// Column separator x-positions shared by the whole row group. A separator is
// accepted when it is a clean gutter (never cuts a glyph, seen in >= 2 rows) or
// is supported by a solid majority of rows while only a few spanning cells cross
// it -- the tolerance is what lets a two-level header or an (E)-style spanning
// row sit inside an otherwise well-gridded table.
std::vector<double> table_column_boundaries(const std::vector<Line>& lines, size_t start,
                                            size_t end, double size) {
    const double min_gap = std::max(6.0, 0.7 * size);
    const double tol = std::max(3.0, 0.4 * size);
    const size_t nrows = end - start;

    // 0 = outside the row's ink, 1 = clear column gap, 2 = blocked (cuts a token).
    auto classify = [&](const Line& ln, double x) -> int {
        const CharInfo* prev = nullptr;
        for (const CharInfo& c : ln.chars) {
            if (c.space) continue;
            if (prev && x > prev->right && x < c.left)
                return (c.left - prev->right) >= min_gap ? 1 : 2;
            if (x >= c.left && x <= c.right) return 2;
            prev = &c;
        }
        return 0;
    };

    // Tokens: maximal glyph runs (gaps below the column threshold). A real column
    // between two separators must hold a token that fits *entirely* between them;
    // the overhang of a wide left-aligned label spans from the far left and so
    // never does, which is what tells a genuine column from gutter slack.
    std::vector<std::pair<double, double>> tokens;  // [left, right]
    std::vector<double> cand;
    for (size_t li = start; li < end; ++li) {
        const CharInfo* prev = nullptr;
        double tlo = 0, thi = 0;
        for (const CharInfo& c : lines[li].chars) {
            if (c.space) continue;
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
        if (prev) tokens.emplace_back(tlo, thi);
    }
    if (cand.empty()) return {};
    std::sort(cand.begin(), cand.end());

    // Accept clean gutters (never cut a glyph) or majority-supported separators.
    std::vector<std::pair<double, size_t>> accepted;  // {x, cut}
    size_t i = 0;
    while (i < cand.size()) {
        size_t j = i;
        double sum = 0;
        while (j < cand.size() && cand[j] - cand[i] <= tol) { sum += cand[j]; ++j; }
        double x = sum / static_cast<double>(j - i);
        size_t support = 0, cut = 0;
        for (size_t li = start; li < end; ++li) {
            int k = classify(lines[li], x);
            if (k == 1) ++support;
            else if (k == 2) ++cut;
        }
        bool clean = support >= 2 && cut == 0;
        bool strong = support * 3 >= nrows && cut * 3 <= nrows && support > cut;
        if ((clean || strong) && (accepted.empty() || x - accepted.back().first > tol))
            accepted.emplace_back(x, cut);
        i = j;
    }

    auto real_column_between = [&](double lo, double hi) {
        for (const auto& [tl, tr] : tokens)
            if (tl > lo && tr < hi) return true;
        return false;
    };

    // Fuse runs of separators that enclose no real column (one wide gutter split
    // by variable label widths); keep the one that cuts the fewest rows.
    std::vector<double> boundaries;
    i = 0;
    while (i < accepted.size()) {
        size_t j = i;
        while (j + 1 < accepted.size() &&
               !real_column_between(accepted[j].first, accepted[j + 1].first))
            ++j;
        size_t best = i;
        for (size_t k = i; k <= j; ++k)
            if (accepted[k].second < accepted[best].second) best = k;
        boundaries.push_back(accepted[best].first);
        i = j + 1;
    }
    return boundaries;
}

// Slices one line into per-column glyph ranges. A token straddling a boundary
// (a spanning header/description) stays in the column it starts in: the column
// only advances at a real gap, so word-spaced phrases are never split.
void assign_row_cells(const Line& ln, const std::vector<double>& boundaries, double switch_gap,
                      std::vector<std::pair<long, long>>& ranges) {
    ranges.assign(boundaries.size() + 1, {-1, -1});
    long curcol = -1;
    const CharInfo* prev = nullptr;
    for (size_t i = 0; i < ln.chars.size(); ++i) {
        const CharInfo& c = ln.chars[i];
        if (c.space) continue;
        double center = (c.left + c.right) / 2.0;
        long col = static_cast<long>(
            std::upper_bound(boundaries.begin(), boundaries.end(), center) - boundaries.begin());
        if (curcol < 0) {
            curcol = col;
        } else if (col != curcol && c.left - prev->right >= switch_gap) {
            curcol = col;
        }
        auto& r = ranges[static_cast<size_t>(curcol)];
        if (r.first < 0) r.first = static_cast<long>(i);
        r.second = static_cast<long>(i);
        prev = &c;
    }
}

// Renders the row group [start, end) into a GFM grid at the given column
// boundaries: folds the header, merges word-flowed spanning cells, and folds
// wrapped continuations back into their row. `inner_left`/`tx0`/`col_tol` carry
// the enclosing region's geometry (the first inner column's left edge, the
// table's left margin, the column-alignment tolerance). `nested` marks a grid
// split off from a richer interior section: any label-less continuation line
// folds up (its wrap may itself carry several columns), matching how a nested
// Attribute/Type/... grid wraps.
std::vector<std::vector<TableCell>> build_table_grid(const std::vector<Line>& lines, size_t start,
                                                     size_t end,
                                                     const std::vector<double>& boundaries,
                                                     double inner_left, double tx0, double col_tol,
                                                     bool nested) {
    const size_t n = end - start;
    const size_t ncol = boundaries.size() + 1;
    std::vector<double> gsizes;
    for (size_t li = start; li < end; ++li) gsizes.push_back(robust_line_size(lines[li]));
    const double gsize = std::max(median(std::move(gsizes)), 1.0);
    const double switch_gap = std::max(6.0, 0.7 * gsize);

    auto inner_continuation = [&](const Line& ln) {
        return ln.segments.size() < 2 && inner_left < 1e30 && ln.x0 >= inner_left - col_tol;
    };

    std::vector<std::vector<std::pair<long, long>>> slices(n);
    for (size_t li = start; li < end; ++li)
        assign_row_cells(lines[li], boundaries, switch_gap, slices[li - start]);

    // 2. Header: fold leading lines that are stacked tighter than the row pitch
    //    into one header row (two-level and wrapped headers).
    std::vector<double> gaps;
    for (size_t li = start + 1; li < end; ++li)
        gaps.push_back(lines[li - 1].baseline - lines[li].baseline);
    const double pitch = std::max(median(gaps), 1.0);
    // A line that carries its own row-label in column 0 AND fills an inner column
    // is an independent data row, never a header continuation: a stack of
    // key/value rows would otherwise be folded together and its distinct rows
    // fused into one cell. The header may still span several lines, but only over
    // wrapped/two-level sub-lines that leave column 0 empty (handled by (b)/(d));
    // it stops at the first genuine labeled data row.
    auto is_new_row = [&](size_t r) {
        if (slices[r][0].first < 0) return false;
        for (size_t c = 1; c < ncol; ++c)
            if (slices[r][c].first >= 0) return true;
        return false;
    };
    size_t first_data = n;
    for (size_t r = 1; r < n; ++r)
        if (is_new_row(r)) { first_data = r; break; }
    // (a) Header lines are stacked tighter than the body pitch.
    size_t hdr = 1;
    for (size_t g = 0; g < gaps.size(); ++g) {
        if (gaps[g] < 0.82 * pitch) hdr = g + 2;
        else break;
    }
    // (b) When the header labels only the inner columns, its lines carry no
    //     row-label in column 0 while every body row (or group label) does; the
    //     leading run of empty-column-0 lines is then the header. This recovers
    //     a two-level header whose sub-lines baseline-clustered into body pitch.
    if (slices[0][0].first < 0) {
        size_t c0 = 0;
        while (c0 < n && slices[c0][0].first < 0) ++c0;
        if (c0 > hdr) hdr = c0;
    }
    // (c) Otherwise the header/body break can show as the widest gap among the
    //     top rows, distinctly wider than the rhythm below it.
    size_t look = std::min(gaps.size(), static_cast<size_t>(4));
    size_t peak = 0;
    for (size_t g = 0; g < look; ++g)
        if (gaps[g] > gaps[peak]) peak = g;
    if (peak + 1 > hdr && peak + 1 < n) {
        double below = peak + 1 < gaps.size() ? gaps[peak + 1] : gaps[peak];
        if (gaps[peak] >= 1.15 * below) hdr = peak + 1;
    }
    // The header can never reach past the first labeled data row, whatever the
    // gap rhythm suggested: that keeps key/value rows distinct.
    if (hdr > first_data) hdr = std::max<size_t>(first_data, 1);
    if (hdr >= n) hdr = 1;

    // (d) A wrapped header cell: the line directly below the header carries no
    //     row-label (column 0 empty) and fills a single inner column that the
    //     header already labels, with purely alphabetic text (e.g. a header cell
    //     "Sequential Operations" set on two lines). It is a header continuation,
    //     not a data row, so fold it into the header.
    auto lone_alpha_header_cont = [&](size_t r) -> bool {
        if (r == 0 || r >= n) return false;
        if (slices[r][0].first >= 0) return false;  // a row label -> real data row
        size_t filled = 0, only = 0;
        for (size_t c = 0; c < ncol; ++c)
            if (slices[r][c].first >= 0) { ++filled; only = c; }
        if (filled != 1 || only == 0) return false;
        bool hdr_labels = false;  // the header must already occupy this column
        for (size_t h = 0; h < r; ++h)
            if (slices[h][only].first >= 0) { hdr_labels = true; break; }
        if (!hdr_labels) return false;
        const Line& ln = lines[start + r];
        size_t lo = static_cast<size_t>(slices[r][only].first);
        size_t hi = static_cast<size_t>(slices[r][only].second);
        size_t letters = 0;
        for (size_t i = lo; i <= hi && i < ln.chars.size(); ++i) {
            const CharInfo& c = ln.chars[i];
            if (c.space) continue;
            if (!is_latin_letter_cp(c.code)) return false;  // any digit/symbol -> data
            ++letters;
        }
        return letters >= 2;
    };
    while (hdr < n && lone_alpha_header_cont(hdr) &&
           lines[start + hdr - 1].baseline - lines[start + hdr].baseline <= 1.6 * pitch)
        ++hdr;

    // A value that merely flows past an inner column boundary (a wrapped C++
    // syntax line) is split across columns by the shared grid even though its
    // glyphs run on with ordinary word spacing -- and a wrapped continuation
    // then folds column-for-column, interleaving the words out of order. Such a
    // line "spans": two adjacent inner columns hold text separated only by a
    // word-sized gap, never a real gutter. A lone spanning line is left alone --
    // a global boundary fixed by other rows must still split even close cells
    // (short data like "34 56"). Only when the spanning is confirmed by a
    // vertically-adjacent spanning line (the wrapped continuation of the same
    // flowing value) are its inner columns merged into the leftmost, so reading
    // order survives the fold. Restricted to body rows: a multi-line header
    // legitimately packs many narrow columns and must keep them.
    const double gutter_min = 1.3 * gsize;
    const double adj_win = 2.0 * gsize;
    auto spanning = [&](size_t r) -> bool {
        const Line& ln = lines[start + r];
        long tgt = -1;
        for (size_t c = 1; c < ncol; ++c) {
            if (slices[r][c].first < 0) continue;
            if (tgt >= 0 &&
                ln.chars[static_cast<size_t>(slices[r][c].first)].left -
                        ln.chars[static_cast<size_t>(slices[r][static_cast<size_t>(tgt)].second)]
                            .right <
                    gutter_min)
                return true;
            tgt = static_cast<long>(c);
        }
        return false;
    };
    std::vector<char> spans(n, 0);
    for (size_t r = hdr; r < n; ++r) spans[r] = spanning(r) ? 1 : 0;
    for (size_t r = hdr; r < n; ++r) {
        if (!spans[r]) continue;
        bool confirmed =
            (r > hdr && lines[start + r - 1].baseline - lines[start + r].baseline <= adj_win &&
             spans[r - 1]) ||
            (r + 1 < n && lines[start + r].baseline - lines[start + r + 1].baseline <= adj_win &&
             spans[r + 1]);
        if (!confirmed) continue;
        const Line& ln = lines[start + r];
        long target = -1;
        for (size_t c = 1; c < ncol; ++c) {
            if (slices[r][c].first < 0) continue;
            if (target >= 0 &&
                ln.chars[static_cast<size_t>(slices[r][c].first)].left -
                        ln.chars[static_cast<size_t>(slices[r][static_cast<size_t>(target)].second)]
                            .right <
                    gutter_min) {
                slices[r][static_cast<size_t>(target)].second = slices[r][c].second;
                slices[r][c] = {-1, -1};
                continue;
            }
            target = static_cast<long>(c);
        }
    }

    auto build_row = [&](size_t r) {
        std::vector<TableCell> row(ncol);
        for (size_t c = 0; c < ncol; ++c)
            if (slices[r][c].first >= 0)
                row[c] = render_cell_runs(lines[start + r], static_cast<size_t>(slices[r][c].first),
                                          static_cast<size_t>(slices[r][c].second));
        return row;
    };

    std::vector<TableCell> header(ncol);
    for (size_t r = 0; r < hdr; ++r) {
        std::vector<TableCell> hr = build_row(r);
        for (size_t c = 0; c < ncol; ++c) append_cell(header[c], hr[c]);
    }

    std::vector<std::vector<TableCell>> data;
    std::vector<double> data_base;
    for (size_t r = hdr; r < n; ++r) {
        data.push_back(build_row(r));
        data_base.push_back(lines[start + r].baseline);
    }

    // 2b. Fold wrapped continuations back into the row they belong to. Each
    //     physical line is one of:
    //       FULL  - a row label in column 0 and inner-column text (a whole row);
    //       COL0  - text only in the row-label column (a wrapped label fragment);
    //       INNER - an empty row label with text indented into an inner column
    //               (a value cell, possibly set clear of its wrapped label);
    //       OTHER - anything else (kept verbatim as its own row).
    //     A row label may wrap across several COL0 lines, and a value cell may be
    //     set vertically centred between the label's fragments (a two-line ID
    //     wrapping around its description). Grouping walks the lines and, using
    //     only the neighbour geometry, keeps each such row's label in one cell
    //     and its value in one cell rather than emitting a fragment per line or
    //     leaking the trailing fragment below the table.
    {
        enum LK { FULL, COL0, INNER, OTHER };
        std::vector<LK> kind(data.size(), OTHER);
        for (size_t di = 0; di < data.size(); ++di) {
            const auto& s = slices[hdr + di];
            bool inner = false;
            for (size_t c = 1; c < ncol; ++c)
                if (s[c].first >= 0) inner = true;
            bool c0 = s[0].first >= 0;
            if (c0 && inner) kind[di] = FULL;
            else if (c0 && !inner) kind[di] = COL0;
            else if (!c0 && inner && (nested || inner_continuation(lines[start + hdr + di])))
                kind[di] = INNER;
        }

        std::vector<std::vector<TableCell>> folded;
        std::vector<double> folded_base;
        std::vector<char> folded_wraps;  // row's label is known to wrap (built from a fragment)
        std::vector<TableCell> pending;   // leading column-0 fragments awaiting their value row
        // Returns whether the row it emits carries a wrapped label.
        auto flush_pending = [&](std::vector<TableCell>& row) {
            if (pending.empty()) return false;
            TableCell joined;
            for (auto& frag : pending) append_cell(joined, frag, label_join_tight(joined, frag));
            append_cell(joined, row[0], label_join_tight(joined, row[0]));
            row[0] = std::move(joined);
            pending.clear();
            return true;
        };
        auto push_row = [&](size_t di, bool wraps) {
            folded.push_back(std::move(data[di]));
            folded_base.push_back(data_base[di]);
            folded_wraps.push_back(wraps ? 1 : 0);
        };

        for (size_t di = 0; di < data.size(); ++di) {
            if (kind[di] == INNER) {
                if (!pending.empty()) {
                    // A column-0 label started just above pairs with this value:
                    // a new row whose label wrapped around a centred value cell.
                    bool w = flush_pending(data[di]);
                    push_row(di, w);
                } else if (!folded.empty()) {
                    for (size_t c = 1; c < ncol; ++c)
                        if (!data[di][c].empty()) append_cell(folded.back()[c], data[di][c]);
                } else {
                    push_row(di, false);
                }
                continue;
            }
            // A FULL row whose column-0 fragment continues the previous row's
            // label as a broken camelCase identifier -- the label above ended on a
            // lowercase letter and this fragment opens uppercase with no gap -- is
            // one identifier set on two lines, not a new row. Both fragments are
            // plain: the structural key/value labels are bold and never wrap this
            // way, and a genuine new attribute/literal opens lowercase. Fold the
            // whole row up: label joined tight, inner columns merged. Chains
            // (label broken over three or more lines) fold one line at a time.
            if (kind[di] == FULL && pending.empty() && !folded.empty() && di > 0 &&
                slices[hdr + di - 1][0].second >= 0 && slices[hdr + di][0].first >= 0) {
                const CharInfo& prev_last =
                    lines[start + hdr + di - 1]
                        .chars[static_cast<size_t>(slices[hdr + di - 1][0].second)];
                const CharInfo& frag_first =
                    lines[start + hdr + di].chars[static_cast<size_t>(slices[hdr + di][0].first)];
                double gap_up = folded_base.empty() ? 1e30 : folded_base.back() - data_base[di];
                bool camel = prev_last.code >= U'a' && prev_last.code <= U'z' &&
                             frag_first.code >= U'A' && frag_first.code <= U'Z' &&
                             !prev_last.bold && !frag_first.bold;
                // The label and its continuation are physically adjacent lines;
                // bound the fold to one row's advance. `pitch` alone under-counts
                // it when tight note-wrap leading from a tall sibling row skews the
                // median, so also admit a step of a couple line-heights.
                bool adjacent = gap_up <= std::max(1.8 * pitch, 2.5 * gsize);
                // Wrap evidence, either of:
                //  (A) the label above ran to the edge of its column (it filled
                //      the column, so a long identifier had to break there); a
                //      short self-contained label ends well clear and never wraps.
                //  (B) in a wide grid (>= 4 columns, a real note column present)
                //      the continuation fills only its label and the trailing note
                //      column -- the short typed columns (Type/Mult/Kind) are
                //      empty, so it carries no new attribute and merely continues
                //      the label wrapping ahead of the note. A genuine data row
                //      fills those typed columns.
                bool fills_col = boundaries[0] > tx0 &&
                                 (prev_last.right - tx0) >= 0.55 * (boundaries[0] - tx0);
                bool bare_label = false;
                if (ncol >= 4) {
                    bool typed = false;
                    for (size_t c = 1; c + 1 < ncol; ++c)
                        if (!data[di][c].empty()) typed = true;
                    bare_label = !typed;
                }
                // Two independent, fully-formed rows that merely meet on a
                // lowercase->uppercase label boundary each carry their own datum in
                // the same middle (typed) column -- that collision marks them as
                // distinct rows and withholds the fold. A lockstep wrap is the
                // exception: a long typed value too wide for its column breaks at the
                // same line as its label, so the fragment's cell there is not a fresh
                // datum but a continuation of the cell above -- the cell above ran to
                // the column's right edge and the fragment opens uppercase where its
                // lowercase run left off (a camelCase value split across the break).
                // A collision in such a column is exempt; a collision anywhere else
                // still withholds the fold. The trailing note column stays exempt: a
                // note legitimately wraps and continues across the pair.
                auto column_wrap_continuation = [&](size_t c) -> bool {
                    long above_i = slices[hdr + di - 1][c].second;
                    long frag_i = slices[hdr + di][c].first;
                    if (above_i < 0 || frag_i < 0) return false;
                    const CharInfo& above_last =
                        lines[start + hdr + di - 1].chars[static_cast<size_t>(above_i)];
                    const CharInfo& frag_first =
                        lines[start + hdr + di].chars[static_cast<size_t>(frag_i)];
                    bool camel_c = above_last.code >= U'a' && above_last.code <= U'z' &&
                                   frag_first.code >= U'A' && frag_first.code <= U'Z' &&
                                   !above_last.bold && !frag_first.bold;
                    double left_c = boundaries[c - 1];
                    bool fills_c = boundaries[c] > left_c &&
                                   (above_last.right - left_c) >= 0.55 * (boundaries[c] - left_c);
                    return camel_c && fills_c;
                };
                bool typed_collision = false;
                for (size_t c = 1; c + 1 < ncol; ++c)
                    if (!folded.back()[c].empty() && !data[di][c].empty() &&
                        !column_wrap_continuation(c))
                        typed_collision = true;
                if (camel && adjacent && (fills_col || bare_label) && !typed_collision) {
                    append_cell(folded.back()[0], data[di][0], /*tight=*/true);
                    for (size_t c = 1; c < ncol; ++c)
                        if (!data[di][c].empty())
                            // A typed value that wrapped in lockstep with the label
                            // rejoins tight (its camelCase split carried no space); a
                            // wrapped note continues as words and keeps its space.
                            append_cell(folded.back()[c], data[di][c],
                                        c + 1 < ncol && column_wrap_continuation(c));
                    folded_wraps.back() = 1;
                    continue;
                }
            }
            if (kind[di] == COL0) {
                bool next_inner = di + 1 < data.size() && kind[di + 1] == INNER;
                bool prev_value = di > 0 && (kind[di - 1] == FULL || kind[di - 1] == INNER);
                double gap_up = folded.empty() ? 1e30 : folded_base.back() - data_base[di];
                double gap_dn = di + 1 < data.size() ? data_base[di] - data_base[di + 1] : 1e30;
                // A trailing fragment continues the label of the open row above
                // when that label wrapped: either the row was itself built from a
                // wrapped label (a centred value cell) or the label line above ran
                // to the edge of the row-label column (its right edge sits within a
                // few glyphs of the first inner column). A short, self-contained
                // label that merely has an empty value (a distinct row) reaches
                // nowhere near the column edge and so stays its own row.
                // Geometric wrap evidence: the label line above ran to the edge of
                // the row-label column AND ended mid-word (its last glyph is a
                // letter/digit). A line padded to the edge by dot leaders (a table
                // of contents entry) ends in punctuation and is not a wrap.
                double prev_rx = -1;
                bool prev_word_end = false;
                if (di > 0 && slices[hdr + di - 1][0].second >= 0) {
                    const CharInfo& pc =
                        lines[start + hdr + di - 1]
                            .chars[static_cast<size_t>(slices[hdr + di - 1][0].second)];
                    prev_rx = pc.right;
                    prev_word_end = is_latin_letter_cp(pc.code) || (pc.code >= U'0' && pc.code <= U'9');
                }
                // Geometric wrap needs the label above to actually fill its
                // column: a label that wrapped ran most of the way across the
                // row-label column before breaking. A one-glyph label ("5" in a
                // numbered code listing) fills almost none of it and never wraps,
                // so a following short line is a new row, not its continuation.
                bool prev_fills_col = prev_rx >= 0 && boundaries[0] > tx0 &&
                                      (prev_rx - tx0) >= 0.55 * (boundaries[0] - tx0);
                bool prev_wrapped = (!folded.empty() && folded_wraps.back()) ||
                                    (prev_rx >= 0 && prev_word_end && inner_left < 1e30 &&
                                     inner_left - prev_rx <= 6.0 * gsize && prev_fills_col);
                // Only a label-like fragment (carrying a letter or digit) continues
                // a wrapped label; a punctuation-only line (an ellipsis "..." row in
                // a code listing) is its own row.
                bool labelish = false;
                for (size_t i = static_cast<size_t>(slices[hdr + di][0].first);
                     slices[hdr + di][0].first >= 0 &&
                     i <= static_cast<size_t>(slices[hdr + di][0].second) &&
                     i < lines[start + hdr + di].chars.size();
                     ++i) {
                    char32_t cp = lines[start + hdr + di].chars[i].code;
                    if (is_latin_letter_cp(cp) || (cp >= U'0' && cp <= U'9')) { labelish = true; break; }
                }
                if (!next_inner && prev_value && !folded.empty() && gap_up <= 1.8 * pitch &&
                    prev_wrapped && labelish) {
                    append_cell(folded.back()[0], data[di][0],
                                label_join_tight(folded.back()[0], data[di][0]));
                    folded_wraps.back() = 1;  // label continues to wrap
                    continue;
                }
                // A leading fragment whose value cell follows on the next line waits
                // to prepend onto the row it introduces. The value line is either an
                // INNER line (a bare centred value) or a FULL line whose own column-0
                // is the middle fragment of the same wrapped label. The FULL-centre
                // sandwich is admitted only when a further column-0 fragment trails
                // the value (label fragments bracket it on both sides) AND this
                // leading fragment itself shows wrap evidence -- it breaks on a token
                // connector, or its ink runs to the edge of the label column. A
                // short self-contained label ("Zzz") ends well clear of the edge on
                // a letter and stays its own row.
                double frag_rx = slices[hdr + di][0].second >= 0
                                     ? lines[start + hdr + di]
                                           .chars[static_cast<size_t>(slices[hdr + di][0].second)]
                                           .right
                                     : -1;
                char32_t frag_last = 0;
                if (slices[hdr + di][0].second >= 0)
                    frag_last = lines[start + hdr + di]
                                    .chars[static_cast<size_t>(slices[hdr + di][0].second)]
                                    .code;
                bool frag_wraps = frag_last == U'_' || frag_last == U'.' || frag_last == U'/' ||
                                  (frag_rx >= 0 && boundaries[0] - frag_rx <= 2.5 * gsize);
                bool next_full_sandwich = di + 2 < data.size() && kind[di + 1] == FULL &&
                                          kind[di + 2] == COL0 && frag_wraps;
                if ((next_inner || next_full_sandwich) && gap_dn <= 1.8 * pitch) {
                    pending.push_back(std::move(data[di][0]));
                    continue;
                }
                // An isolated label with an empty value stays its own row.
            }
            bool w = flush_pending(data[di]);
            push_row(di, w);
        }
        if (!pending.empty() && !folded.empty())
            for (auto& frag : pending)
                append_cell(folded.back()[0], frag, label_join_tight(folded.back()[0], frag));
        data = std::move(folded);
        data_base = std::move(folded_base);
    }

    // 3. Fold a lone-column label that sits at a sub-pitch offset (a group label
    //    like "(A)" centred between two rows) into its nearest neighbour.
    for (size_t r = 0; r < data.size();) {
        size_t f = 0, fc = 0;
        for (size_t c = 0; c < ncol; ++c)
            if (!data[r][c].empty()) { ++f; fc = c; }
        if (f == 1) {
            double gp = r > 0 ? data_base[r - 1] - data_base[r] : 1e30;
            double gn = r + 1 < data.size() ? data_base[r] - data_base[r + 1] : 1e30;
            if (std::min(gp, gn) < 0.65 * pitch) {
                size_t tgt = gp <= gn ? r - 1 : r + 1;
                TableCell merged = data[r][fc];
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
    cells.push_back(std::move(header));
    for (auto& row : data) cells.push_back(std::move(row));
    return cells;
}

std::optional<TableCandidate> try_table(const std::vector<Line>& lines, size_t start,
                                        size_t region_lines, bool render, double page_width) {
    const double seed_size = robust_line_size(lines[start]);
    const double gap_limit = 2.0 * seed_size;  // two body lines: splits grids set apart

    // 1. Extend the group downward over vertically contiguous, non-mono lines.
    //    A wide single-segment line is prose (a caption or a following paragraph
    //    a small gap pulled in) and ends the table; a narrow single-segment line
    //    is a wrapped header cell or a group label and stays. The exception is a
    //    single-segment line indented into an established inner column: that is a
    //    wrapped continuation of that column's cell (a glossary description, a
    //    key/value value set across several lines) and stays no matter how wide.
    const double col_tol = std::max(3.0, 0.4 * seed_size);
    // Left edge of the first inner column, learned from multi-segment rows (the
    // start of their second column segment). A wrapped inner-column cell begins
    // at or right of this x; a following body paragraph starts back at the
    // table's left margin, so the two are told apart by indent alone.
    double inner_left = 1e30;
    auto note_inner = [&](const Line& ln) {
        if (ln.segments.size() >= 2)
            inner_left = std::min(inner_left, ln.chars[ln.segments[1].first].left);
    };
    auto inner_continuation = [&](const Line& ln) {
        return ln.segments.size() < 2 && inner_left < 1e30 && ln.x0 >= inner_left - col_tol;
    };
    size_t end = start + 1;
    double tx0 = lines[start].x0, tx1 = lines[start].x1;
    note_inner(lines[start]);
    // A bold single-segment line, indented from the table's left edge and
    // centred on the page, is a caption/title drawn below the grid ("Table N:
    // <title>" outside the box). A wrapped inner-column cell is flush-left at the
    // inner column and flows to the right edge (its centre lies well right of the
    // page centre); a deep bold sub-label ("Stereotypes:"/"Tags:") is likewise
    // off-centre. Such a caption terminates the table and stays outside it,
    // rather than being swallowed as a continuation of the last row's cell.
    const double page_center = page_width / 2.0;
    auto caption_shaped = [&](const Line& ln) {
        if (ln.segments.size() >= 2 || inner_left >= 1e30 || page_width <= 0) return false;
        size_t nonspace = 0, bold = 0;
        for (const CharInfo& c : ln.chars)
            if (!c.space) { ++nonspace; if (c.bold) ++bold; }
        if (nonspace == 0 || bold * 2 <= nonspace) return false;
        if (ln.x0 - tx0 <= col_tol) return false;  // full-width lines are not captions
        double center = (ln.x0 + ln.x1) / 2.0;
        return std::abs(center - page_center) <= 0.03 * page_width;
    };
    while (end < region_lines) {
        const Line& cur = lines[end];
        if (cur.mono_majority) break;
        if (lines[end - 1].baseline - cur.baseline > gap_limit) break;
        if (caption_shaped(cur)) break;
        if (cur.segments.size() < 2 && (cur.x1 - cur.x0) > 0.55 * (tx1 - tx0) &&
            !inner_continuation(cur))
            break;
        if (cur.segments.size() >= 2) {
            tx0 = std::min(tx0, cur.x0);
            tx1 = std::max(tx1, cur.x1);
            note_inner(cur);
        }
        ++end;
    }
    // A single-segment line whose ink stays entirely within the row-label column
    // (left of the first inner column) is a wrapped label fragment -- real cell
    // content, folded back into its row below.
    auto col0_fragment = [&](const Line& ln) {
        return ln.segments.size() < 2 && inner_left < 1e30 &&
               ln.x0 <= tx0 + col_tol && ln.x1 <= inner_left - col_tol;
    };
    // Trim trailing single-segment lines that dangle past the last grid row. Keep
    // a label fragment (folded below) and a genuine wrapped inner continuation --
    // but a continuation is only genuine when the line above it actually holds an
    // inner cell to continue; a centred caption sitting under a bare label column
    // (its neighbour above has no inner ink) is not, and is left out of the table.
    auto has_inner_ink = [&](const Line& ln) {
        return ln.segments.size() >= 2 || inner_continuation(ln);
    };
    while (end - 1 > start && lines[end - 1].segments.size() < 2) {
        const Line& last = lines[end - 1];
        if (col0_fragment(last)) break;
        if (inner_continuation(last) && has_inner_ink(lines[end - 2])) break;
        --end;
    }

    size_t n = end - start;
    if (n < 2) return std::nullopt;

    auto grid_size = [&](size_t s, size_t e) {
        std::vector<double> sz;
        for (size_t li = s; li < e; ++li) sz.push_back(robust_line_size(lines[li]));
        return std::max(median(std::move(sz)), 1.0);
    };
    double gsize = grid_size(start, end);
    // A requirement heading or caption stacked directly above the grid can align
    // into two segments (an id/label column and a title column) and so seed a
    // table, yet it is set markedly larger than the grid body and is not a row.
    // Drop such oversized leading lines so the heading (and any metadata line or
    // corner glyph below it) stays in the normal text flow, instead of being
    // swallowed as a scrambled header row above the box.
    bool trimmed = false;
    while (end - start > 2 && robust_line_size(lines[start]) > 1.15 * gsize) {
        ++start;
        gsize = grid_size(start, end);
        trimmed = true;
    }
    n = end - start;
    if (n < 2) return std::nullopt;
    if (trimmed) {
        // Recompute the table's left edges over the surviving rows: a dropped
        // heading must not skew the inner-column origin used to build cells.
        inner_left = 1e30;
        tx0 = lines[start].x0;
        tx1 = lines[start].x1;
        for (size_t li = start; li < end; ++li) {
            tx0 = std::min(tx0, lines[li].x0);
            tx1 = std::max(tx1, lines[li].x1);
            note_inner(lines[li]);
        }
    }

    std::vector<double> boundaries = table_column_boundaries(lines, start, end, gsize);
    if (boundaries.empty()) return std::nullopt;  // fewer than two columns
    const size_t ncol = boundaries.size() + 1;
    if (ncol > 20) return std::nullopt;
    const double switch_gap = std::max(6.0, 0.7 * gsize);

    std::vector<std::vector<std::pair<long, long>>> slices(n);
    for (size_t li = start; li < end; ++li)
        assign_row_cells(lines[li], boundaries, switch_gap, slices[li - start]);

    auto filled_count = [&](size_t r) {
        size_t f = 0;
        for (const auto& c : slices[r])
            if (c.first >= 0) ++f;
        return f;
    };

    if (n == 2 && ncol < 3) return std::nullopt;  // too weak a signal

    // Two long-prose columns are a page layout, not a table.
    if (ncol == 2) {
        double len[2] = {0, 0};
        size_t cnt[2] = {0, 0};
        for (size_t r = 0; r < n; ++r)
            for (size_t c = 0; c < 2; ++c)
                if (slices[r][c].first >= 0) {
                    len[c] += static_cast<double>(range_glyphs(
                        lines[start + r], static_cast<size_t>(slices[r][c].first),
                        static_cast<size_t>(slices[r][c].second)));
                    ++cnt[c];
                }
        bool wide0 = cnt[0] > 0 && len[0] / static_cast<double>(cnt[0]) > 18.0;
        bool wide1 = cnt[1] > 0 && len[1] / static_cast<double>(cnt[1]) > 18.0;
        if (wide0 && wide1) return std::nullopt;
    }

    // Need enough genuine multi-column rows to trust the grid.
    size_t multi = 0;
    for (size_t r = 0; r < n; ++r)
        if (filled_count(r) >= 2) ++multi;
    if (multi < 2) return std::nullopt;

    TableCandidate table;
    table.first_line = start;
    table.last_line = end - 1;
    if (!render) return table;

    // A class-style box draws a 2-column key/value head above a nested grid with
    // its own header row (Attribute/Type/Mult./Kind/Note, or Literal/Description).
    // That interior header re-labels the columns: it is a row below row 0 whose
    // filled cells are each predominantly bold in two or more columns, whereas a
    // key/value row is bold only in its label column (its value carries at most a
    // small bold "Tags:"/"Stereotypes:" run). Split at that header so the nested
    // grid renders as its own GFM table -- with the header stripped to plain text
    // and its columns recomputed on the nested rows alone -- instead of the header
    // surfacing as a bold data row and every nested column collapsing into one.
    auto predom_bold_cols = [&](size_t r) {
        size_t cols = 0;
        for (size_t c = 0; c < ncol; ++c) {
            if (slices[r][c].first < 0) continue;
            size_t total = 0, bold = 0;
            for (size_t i = static_cast<size_t>(slices[r][c].first);
                 i <= static_cast<size_t>(slices[r][c].second) && i < lines[start + r].chars.size();
                 ++i) {
                const CharInfo& ch = lines[start + r].chars[i];
                if (ch.space) continue;
                ++total;
                if (ch.bold) ++bold;
            }
            if (total > 0 && bold * 2 > total) ++cols;
        }
        return cols;
    };
    size_t split = 0;
    for (size_t r = 1; r + 1 < n; ++r)
        if (predom_bold_cols(r) >= 2) { split = r; break; }
    if (split >= 1 && split + 1 < n) {
        std::vector<double> head = table_column_boundaries(lines, start, start + split, gsize);
        std::vector<double> rich = table_column_boundaries(lines, start + split, end, gsize);
        if (head.empty()) head = boundaries;
        if (rich.empty()) rich = boundaries;
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

// True when the majority of the region's lines form aligned table rows.
bool region_looks_tabular(const std::vector<const CharInfo*>& chars) {
    std::vector<Line> lines = build_lines(chars);
    if (lines.size() < 2) return false;
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
