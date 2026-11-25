#include "pdf2md/layout/passes.h"

#include "pdf2md/layout/chrome.h"
#include "pdf2md/layout/headings.h"
#include "pdf2md/layout/tables_text.h"
#include "pdf2md/model/document_utils.h"
#include "pdf2md/model/geometry.h"
#include "pdf2md/utf8.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pdf2md::layout {

using model::finalize_block_geometry;
using model::is_citation_lead;
using model::is_prose_paragraph;
using model::kCoordSentinel;
using model::kDefaultWordGapScale;
using model::kMinFontSizePt;

namespace {

constexpr char32_t kAsciiLimitCp = 0x80U;
constexpr char32_t kContAboveTriangleCp = 0x25B3U;
constexpr char32_t kContBelowTriangleCp = 0x25BDU;

// Cross-block and cross-page stitching constants.
constexpr double kLoneMathSizeDiffRatio = 0.20;
constexpr double kTableSigMinTolPt = 3.0;
constexpr double kTableSigTolScale = 0.4;
constexpr double kTextTableContX0Scale = 3.0;
constexpr double kTocAlignMinTolPt = 3.0;
constexpr double kTocAlignTolScale = 0.5;
constexpr double kTocMaxVGapScale = 2.5;
constexpr double kRefEntrySizeDiffRatio = 0.15;
constexpr double kCrossPageSizeDiffRatio = 0.08;

// Spurious horizontal-rule suppression thresholds.
constexpr double kHrTableClearanceScale = 0.85;
constexpr double kHrHeadingUnderlineScale = 0.65;
constexpr double kHrFootnoteSepAboveScale = 1.25;
constexpr double kHrFootnoteSepBelowTolScale = 0.25;

// Multi-column image placement and caption alt-text pairing constants.
constexpr double kColResetUpwardScale = 2.0;
constexpr double kColResetRightShiftScale = 1.0;
constexpr double kFullWidthSpanMarginScale = 2.0;
constexpr double kWideImagePageRatio = 0.60;
constexpr double kSpannedColOverlapRatio = 0.35;
constexpr double kWideRunAttachMaxGapScale = 6.0;
constexpr double kCaptionMaxVGapScale = 4.0;
constexpr std::size_t kMaxImageAltCodePoints = 160U;
constexpr std::size_t kMinWordBreakCutCodePoints = 110U;

constexpr std::size_t kPipelinePassCount = 10U;

constexpr bool ascii_iequals(std::u32string_view a, std::u32string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0U; i < a.size(); ++i) {
        if (a[i] >= kAsciiLimitCp || b[i] >= kAsciiLimitCp ||
            ascii_tolower(a[i]) != ascii_tolower(b[i])) {
            return false;
        }
    }
    return true;
}

void relocate_footnotes(std::vector<Block>& blocks) {
    int last_page = 0;
    for (const Block& b : blocks) {
        last_page = (std::max)(last_page, b.page);
    }
    std::vector<Block> kept;
    std::vector<Block> notes;
    kept.reserve(blocks.size());
    for (Block& b : blocks) {
        if (b.is_footnote && b.footnote_numbered) {
            b.page = last_page;
            notes.push_back(std::move(b));
        } else {
            kept.push_back(std::move(b));
        }
    }
    for (Block& n : notes) {
        kept.push_back(std::move(n));
    }
    blocks = std::move(kept);
}

bool is_lone_math_atom(const Block& b) noexcept {
    if (!is_prose_paragraph(b) || b.lines.size() != 1U) {
        return false;
    }
    bool has_math = false;
    for (const StyledRun& r : b.lines.front().runs) {
        if (r.math) {
            has_math = true;
            continue;
        }
        for (const char32_t c : r.text) {
            if (c == U' ') {
                continue;
            }
            if (is_letter_cp(c) || is_ascii_digit(c)) {
                return false;
            }
        }
    }
    return has_math;
}

void merge_trailing_math_atoms(std::vector<Block>& blocks) {
    for (std::size_t i = 1U; i < blocks.size(); ++i) {
        Block& prev = blocks[i - 1U];
        Block& cur = blocks[i];
        if (prev.kind != BlockKind::Paragraph || prev.is_footnote) {
            continue;
        }
        if (prev.lines.empty() || cur.lines.empty() || !is_lone_math_atom(cur)) {
            continue;
        }
        if (std::fabs(prev.size - cur.size) >
            kLoneMathSizeDiffRatio * (std::max)(prev.size, cur.size)) {
            continue;
        }
        const std::u32string& ptext = prev.lines.back().plain;
        std::size_t e = ptext.size();
        while (e > 0U && ptext[e - 1U] == U' ') {
            --e;
        }
        if (e == 0U) {
            continue;
        }
        const char32_t last = ptext[e - 1U];
        const bool continues = is_letter_cp(last) || is_ascii_digit(last) || last == U',' ||
                               last == U'-' || last == kUnicodeHyphenCp || last == kSoftHyphenCp;
        if (!continues) {
            continue;
        }

        for (Line& ln : cur.lines) {
            prev.lines.push_back(std::move(ln));
        }
        finalize_block_geometry(prev);
        blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(i));
        --i;
    }
}

bool is_table_continuation_marker(const Block& b) noexcept {
    if (b.kind != BlockKind::Paragraph || b.lines.empty()) {
        return false;
    }
    bool any = false;
    for (const Line& ln : b.lines) {
        for (const char32_t c : ln.plain) {
            if (c == U' ') {
                continue;
            }
            if (c != kContBelowTriangleCp && c != kContAboveTriangleCp) {
                return false;
            }
            any = true;
        }
    }
    return any;
}

std::size_t table_column_count(const Block& b) noexcept {
    std::size_t max_cols = 0U;
    for (const auto& row : b.table_cells) {
        max_cols = (std::max)(max_cols, row.size());
    }
    return max_cols;
}

void merge_table_alignments(std::vector<TableAlign>& dst, std::span<const TableAlign> src,
                            std::size_t ncol) {
    if (ncol == 0U) {
        return;
    }
    if (dst.size() < ncol) {
        dst.resize(ncol, TableAlign::None);
    }
    for (std::size_t c = 0U; c < ncol && c < src.size(); ++c) {
        if (src[c] == TableAlign::None) {
            continue;
        }
        if (dst[c] == TableAlign::None) {
            dst[c] = src[c];
        } else if (dst[c] != src[c]) {
            dst[c] = TableAlign::None;
        }
    }
}

bool same_column_signature(const Block& a, const Block& b) noexcept {
    if (a.col_edges.empty() || a.col_edges.size() != b.col_edges.size()) {
        return false;
    }
    const double tol =
        (std::max)(kTableSigMinTolPt, kTableSigTolScale * (std::max)(a.size, kMinFontSizePt));
    for (std::size_t i = 0U; i < a.col_edges.size(); ++i) {
        if (std::fabs(a.col_edges[i] - b.col_edges[i]) > tol) {
            return false;
        }
    }
    return true;
}

bool table_row_is_header(std::span<const TableCell> row) noexcept {
    std::size_t nonempty = 0U;
    std::size_t bold = 0U;
    for (const TableCell& cell : row) {
        bool has_ink = false;
        bool all_bold = true;
        for (const StyledRun& r : cell) {
            for (const char32_t c : r.text) {
                if (c != U' ') {
                    has_ink = true;
                    if (!r.bold) {
                        all_bold = false;
                    }
                }
            }
        }
        if (!has_ink) {
            continue;
        }
        ++nonempty;
        if (all_bold) {
            ++bold;
        }
    }
    return nonempty > 0U && bold * 2U >= nonempty;
}

bool table_rows_plain_equal(std::span<const TableCell> a, std::span<const TableCell> b) {
    if (a.size() != b.size()) {
        return false;
    }
    const auto cell_plain = [](const TableCell& cell) {
        std::u32string out;
        for (const StyledRun& r : cell) {
            out += r.text;
        }
        return out;
    };
    for (std::size_t c = 0U; c < a.size(); ++c) {
        if (cell_plain(a[c]) != cell_plain(b[c])) {
            return false;
        }
    }
    return true;
}

void merge_ruled_table_continuations(std::vector<Block>& blocks) {
    for (std::size_t i = 0U; i < blocks.size(); ++i) {
        if (blocks[i].kind != BlockKind::Table || blocks[i].table_cells.empty()) {
            continue;
        }
        const bool is_ruled = !blocks[i].col_edges.empty();
        const std::size_t ncol = table_column_count(blocks[i]);
        if (ncol < 2U) {
            continue;
        }
        const bool prev_header = table_row_is_header(blocks[i].table_cells.front());
        int last_page = blocks[i].page;
        while (true) {
            std::size_t j = i + 1U;
            bool marker_between = false;
            while (j < blocks.size() && is_table_continuation_marker(blocks[j])) {
                marker_between = true;
                ++j;
            }
            if (j >= blocks.size()) {
                break;
            }
            Block& later = blocks[j];
            if (later.kind != BlockKind::Table || later.table_cells.empty() ||
                later.page != last_page + 1) {
                break;
            }
            if (is_ruled) {
                if (!same_column_signature(blocks[i], later)) {
                    break;
                }
                const bool later_header = table_row_is_header(later.table_cells.front());
                if (!(marker_between || (prev_header && !later_header))) {
                    break;
                }
            } else {
                if (!marker_between || !later.col_edges.empty() ||
                    table_column_count(later) != ncol) {
                    break;
                }
                const double ref_sz = std::max({blocks[i].size, later.size, kMinFontSizePt});
                if (std::fabs(blocks[i].x0 - later.x0) > kTextTableContX0Scale * ref_sz) {
                    break;
                }
            }

            std::size_t start_row = 0U;
            if (!is_ruled && prev_header && later.table_cells.size() >= 2U &&
                table_row_is_header(later.table_cells.front()) &&
                table_rows_plain_equal(blocks[i].table_cells.front(), later.table_cells.front())) {
                start_row = 1U;
            }
            for (std::size_t r = start_row; r < later.table_cells.size(); ++r) {
                blocks[i].table_cells.push_back(std::move(later.table_cells[r]));
            }
            merge_table_alignments(blocks[i].table_align, later.table_align, ncol);
            blocks[i].y_bottom = later.y_bottom;
            last_page = later.page;
            blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(i) + 1,
                         blocks.begin() + static_cast<std::ptrdiff_t>(j) + 1);
        }
    }
}

std::optional<std::size_t> trailing_page_number(const Line& ln) noexcept {
    std::ptrdiff_t hi = -1;
    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(ln.chars.size()) - 1; i >= 0; --i) {
        if (!ln.chars[static_cast<std::size_t>(i)].space) {
            hi = i;
            break;
        }
    }
    if (hi < 0 || !is_ascii_digit(ln.chars[static_cast<std::size_t>(hi)].code)) {
        return std::nullopt;
    }
    std::size_t lo = static_cast<std::size_t>(hi);
    while (lo > 0U && !ln.chars[lo - 1U].space && is_ascii_digit(ln.chars[lo - 1U].code)) {
        --lo;
    }
    std::ptrdiff_t p = static_cast<std::ptrdiff_t>(lo) - 1;
    while (p >= 0 && ln.chars[static_cast<std::size_t>(p)].space) {
        --p;
    }
    if (p < 0) {
        return std::nullopt;
    }
    const double wgap = ln.word_gap > 0.0 ? ln.word_gap : kDefaultWordGapScale * ln.size;
    bool space_between = false;
    for (std::size_t k = static_cast<std::size_t>(p) + 1U; k < lo; ++k) {
        if (ln.chars[k].space) {
            space_between = true;
            break;
        }
    }
    if (!space_between &&
        ln.chars[lo].left - ln.chars[static_cast<std::size_t>(p)].right < wgap) {
        return std::nullopt;
    }
    return lo;
}

void fold_toc_orphan_rows(std::vector<Block>& blocks) {
    const auto two_col_toc_table = [](const Block& b) noexcept {
        return b.kind == BlockKind::Table && b.col_edges.empty() && !b.table_cells.empty() &&
               table_column_count(b) == 2U;
    };
    for (std::size_t i = 1U; i < blocks.size(); ++i) {
        Block& P = blocks[i];
        if (P.kind != BlockKind::Paragraph || P.lines.size() != 1U) {
            continue;
        }
        const Line& ln = P.lines[0];
        if (ln.segments.size() >= 2U) {
            continue;
        }
        Block& T1 = blocks[i - 1U];
        if (!two_col_toc_table(T1) || T1.page != P.page) {
            continue;
        }
        const std::optional<std::size_t> ns = trailing_page_number(ln);
        if (!ns || *ns == 0U) {
            continue;
        }
        const double ref_size = (std::max)(T1.size, kMinFontSizePt);
        const double tol = (std::max)(kTocAlignMinTolPt, kTocAlignTolScale * ref_size);
        const double vgap = kTocMaxVGapScale * ref_size;
        if (std::fabs(P.x1 - T1.x1) > tol || P.x0 < T1.x0 - tol ||
            T1.y_bottom - P.y_top > vgap) {
            continue;
        }
        std::vector<TableCell> row(2U);
        row[0] = render_cell_runs(ln, 0U, *ns - 1U);
        row[1] = render_cell_runs(ln, *ns, ln.chars.size() - 1U);
        if (row[0].empty() || row[1].empty()) {
            continue;
        }
        if (T1.table_align.size() < 2U) {
            T1.table_align.resize(2U, TableAlign::None);
        }
        T1.table_cells.push_back(std::move(row));
        T1.x1 = (std::max)(T1.x1, P.x1);
        T1.y_bottom = (std::min)(T1.y_bottom, P.y_bottom);
        std::ptrdiff_t erase_end = static_cast<std::ptrdiff_t>(i) + 1;
        if (i + 1U < blocks.size()) {
            Block& T2 = blocks[i + 1U];
            if (two_col_toc_table(T2) && T2.page == P.page && std::fabs(T2.x1 - T1.x1) <= tol &&
                T2.x0 >= T1.x0 - tol && P.y_bottom - T2.y_top <= vgap) {
                for (auto& r : T2.table_cells) {
                    T1.table_cells.push_back(std::move(r));
                }
                merge_table_alignments(T1.table_align, T2.table_align, 2U);
                T1.x0 = (std::min)(T1.x0, T2.x0);
                T1.x1 = (std::max)(T1.x1, T2.x1);
                T1.y_bottom = (std::min)(T1.y_bottom, T2.y_bottom);
                erase_end = static_cast<std::ptrdiff_t>(i) + 2;
            }
        }
        blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(i),
                     blocks.begin() + erase_end);
        --i;
    }
}

void filter_horizontal_rules(std::vector<Block>& blocks, double body_size) {
    const bool has_hr = std::any_of(blocks.cbegin(), blocks.cend(), [](const Block& b) noexcept {
        return b.kind == BlockKind::HorizontalRule;
    });
    if (!has_hr) {
        return;
    }
    const double ref_body = (std::max)(body_size, kMinFontSizePt);

    const auto x_overlaps = [ref_body](const Block& a, const Block& b) noexcept {
        if (a.x1 <= a.x0 || b.x1 <= b.x0) {
            return true;
        }
        return (std::min)(a.x1, b.x1) - (std::max)(a.x0, b.x0) > -ref_body;
    };

    std::vector<char> drop(blocks.size(), 0);
    for (std::size_t i = 0U; i < blocks.size(); ++i) {
        const Block& hr = blocks[i];
        if (hr.kind != BlockKind::HorizontalRule) {
            continue;
        }
        const double ry_top = (std::max)(hr.y_top, hr.y_bottom);
        const double ry_bot = (std::min)(hr.y_top, hr.y_bottom);

        for (std::size_t j = 0U; j < blocks.size(); ++j) {
            if (j == i || blocks[j].page != hr.page) {
                continue;
            }
            const Block& other = blocks[j];
            if (!x_overlaps(hr, other)) {
                continue;
            }
            if (other.kind == BlockKind::Table) {
                const double clearance = kHrTableClearanceScale * (std::max)(ref_body, other.size);
                const double vgap =
                    std::max({0.0, ry_bot - other.y_top, other.y_bottom - ry_top});
                if (vgap <= clearance) {
                    drop[i] = 1;
                    break;
                }
            } else if (other.kind == BlockKind::Heading) {
                const double ul_dist =
                    kHrHeadingUnderlineScale * (std::max)(ref_body, other.size);
                if (ry_top <= other.y_top && other.y_bottom - ry_top <= ul_dist &&
                    ry_bot >= other.y_bottom - ul_dist) {
                    drop[i] = 1;
                    break;
                }
            } else if (other.is_footnote) {
                const double above = ry_bot - other.y_top;
                if (above >= -kHrFootnoteSepBelowTolScale * ref_body &&
                    above <= kHrFootnoteSepAboveScale * ref_body) {
                    drop[i] = 1;
                    break;
                }
            }
        }
    }

    std::size_t out_idx = 0U;
    for (std::size_t i = 0U; i < blocks.size(); ++i) {
        if (!drop[i]) {
            if (out_idx != i) {
                blocks[out_idx] = std::move(blocks[i]);
            }
            ++out_idx;
        }
    }
    blocks.resize(out_idx);
}

void merge_reference_entries(std::vector<Block>& blocks) {
    for (std::size_t i = 1U; i < blocks.size(); ++i) {
        Block& prev = blocks[i - 1U];
        Block& cur = blocks[i];
        if (!is_prose_paragraph(prev) || !is_prose_paragraph(cur)) {
            continue;
        }
        if (prev.lines.empty() || cur.lines.empty()) {
            continue;
        }
        if (!is_citation_lead(prev.lines.front()) || is_citation_lead(cur.lines.front())) {
            continue;
        }
        if (cur.page != prev.page && cur.page != prev.page + 1) {
            continue;
        }
        if (std::fabs(prev.size - cur.size) >
            kRefEntrySizeDiffRatio * (std::max)(prev.size, cur.size)) {
            continue;
        }
        const std::u32string& ptext = prev.lines.back().plain;
        std::size_t e = ptext.size();
        while (e > 0U && ptext[e - 1U] == U' ') {
            --e;
        }
        if (e == 0U) {
            continue;
        }
        const char32_t last = ptext[e - 1U];
        if (last == U'.' || last == U'!' || last == U'?') {
            continue;
        }

        for (Line& ln : cur.lines) {
            prev.lines.push_back(std::move(ln));
        }
        finalize_block_geometry(prev);
        blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(i));
        --i;
    }
}

void merge_cross_page_paragraphs(std::vector<Block>& blocks) {
    constexpr std::u32string_view kSentenceEnders = U".!?:;\u201D\u2019\"')]";
    for (std::size_t i = 1U; i < blocks.size(); ++i) {
        Block& prev = blocks[i - 1U];
        Block& cur = blocks[i];
        if (prev.page + 1 != cur.page) {
            continue;
        }
        if (prev.kind != BlockKind::Paragraph || cur.kind != BlockKind::Paragraph) {
            continue;
        }
        if (prev.is_footnote || cur.is_footnote || prev.lines.empty() || cur.lines.empty()) {
            continue;
        }
        if (std::fabs(prev.size - cur.size) >
            kCrossPageSizeDiffRatio * (std::max)(prev.size, cur.size)) {
            continue;
        }

        const std::u32string& ptext = prev.lines.back().plain;
        const std::u32string& ctext = cur.lines.front().plain;
        if (ptext.empty() || ctext.empty()) {
            continue;
        }
        const char32_t last = ptext.back();
        const char32_t first = ctext.front();
        const bool scoped =
            last == U':' && ptext.size() >= 2U && ptext[ptext.size() - 2U] == U':';
        const bool continues =
            scoped ? (is_ascii_alpha(first) || is_ascii_digit(first) || first == U'_')
                   : (kSentenceEnders.find(last) == std::u32string_view::npos) &&
                         (is_ascii_lower(first) || last == U'-' || last == kSoftHyphenCp);
        if (!continues) {
            continue;
        }

        for (Line& ln : cur.lines) {
            prev.lines.push_back(std::move(ln));
        }
        finalize_block_geometry(prev);
        prev.page = cur.page;
        blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(i));
        --i;
    }
}

std::string sanitize_single_line_utf8(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    bool gap = false;
    for (std::size_t i = 0U; i < raw.size(); ++i) {
        const auto ch = static_cast<unsigned char>(raw[i]);
        if (ch <= 0x20U || ch == 0x7FU) {
            gap = true;
            continue;
        }
        if (gap && !out.empty()) {
            out += ' ';
        }
        out += static_cast<char>(ch);
        gap = false;
    }
    return out;
}

std::optional<std::size_t> match_image_caption_prefix(std::u32string_view s) noexcept {
    std::size_t i = 0U;
    while (i < s.size() && is_space_cp(s[i])) {
        ++i;
    }
    constexpr std::array<std::u32string_view, 5> kPrefixes = {
        U"figure", U"fig", U"chart", U"diagram", U"scheme"};
    for (const std::u32string_view kw : kPrefixes) {
        if (s.size() < i + kw.size() + 2U) {
            continue;
        }
        if (!ascii_iequals(s.substr(i, kw.size()), kw)) {
            continue;
        }
        std::size_t k = i + kw.size();
        if (kw == U"fig" && k < s.size() && s[k] == U'.') {
            ++k;
        }
        if (k >= s.size() || !is_space_cp(s[k])) {
            continue;
        }
        while (k < s.size() && is_space_cp(s[k])) {
            ++k;
        }
        if (k >= s.size() || !is_ascii_digit(s[k])) {
            continue;
        }
        while (k < s.size() && is_ascii_digit(s[k])) {
            ++k;
        }
        while (k + 1U < s.size() && s[k] == U'.' && is_ascii_digit(s[k + 1U])) {
            ++k;
            while (k < s.size() && is_ascii_digit(s[k])) {
                ++k;
            }
        }
        if (k < s.size() && (s[k] == U'.' || s[k] == U':')) {
            ++k;
        } else if (k < s.size() && !is_space_cp(s[k]) && !is_dashy_cp(s[k])) {
            continue;
        }
        return k;
    }
    return std::nullopt;
}

std::string extract_caption_alt_text(const Block& cap) {
    if (cap.kind != BlockKind::Paragraph || cap.is_footnote || cap.lines.empty()) {
        return {};
    }
    std::u32string flat;
    for (std::size_t li = 0U; li < cap.lines.size(); ++li) {
        const std::u32string& ln = cap.lines[li].plain;
        if (ln.empty()) {
            continue;
        }
        bool dehyphenated = false;
        if (!flat.empty() &&
            (flat.back() == U'-' || flat.back() == kSoftHyphenCp ||
             flat.back() == kUnicodeHyphenCp) &&
            flat.size() >= 2U && is_ascii_lower(flat[flat.size() - 2U])) {
            std::size_t f = 0U;
            while (f < ln.size() && is_space_cp(ln[f])) {
                ++f;
            }
            if (f < ln.size() && is_ascii_lower(ln[f])) {
                flat.pop_back();
                dehyphenated = true;
            }
        }
        if (!flat.empty() && !dehyphenated) {
            flat += U' ';
        }
        flat += ln;
    }

    // Collapse whitespace and control characters into single ASCII spaces.
    std::u32string clean;
    clean.reserve(flat.size());
    bool gap = false;
    for (const char32_t cp : flat) {
        if (cp < 0x20U || cp == 0x7FU || is_space_cp(cp) || is_line_break_cp(cp)) {
            gap = true;
            continue;
        }
        if (gap && !clean.empty()) {
            clean += U' ';
        }
        clean += cp;
        gap = false;
    }

    const std::optional<std::size_t> after_label = match_image_caption_prefix(clean);
    if (!after_label.has_value()) {
        return {};
    }

    const auto is_abbrev_dot = [&clean](std::size_t dot_idx) noexcept {
        if (dot_idx > 0U && dot_idx + 1U < clean.size() && is_ascii_digit(clean[dot_idx - 1U]) &&
            is_ascii_digit(clean[dot_idx + 1U])) {
            return true;
        }
        std::size_t ws = dot_idx;
        while (ws > 0U && clean[ws - 1U] != U' ') {
            --ws;
        }
        const std::u32string_view token = std::u32string_view(clean).substr(ws, dot_idx - ws);
        return ascii_iequals(token, U"fig") || ascii_iequals(token, U"e.g") ||
               ascii_iequals(token, U"i.e") || ascii_iequals(token, U"vs") ||
               ascii_iequals(token, U"al") || ascii_iequals(token, U"cf");
    };

    std::size_t end_pos = clean.size();
    bool seen_body_word = false;
    for (std::size_t i = *after_label; i < clean.size(); ++i) {
        if (is_letter_cp(clean[i]) || is_cjk_cp(clean[i])) {
            seen_body_word = true;
        }
        if (!seen_body_word) {
            continue;
        }
        const char32_t c = clean[i];
        if ((c == U'.' || c == U'!' || c == U'?') &&
            (i + 1U == clean.size() || clean[i + 1U] == U' ') &&
            (c != U'.' || !is_abbrev_dot(i))) {
            end_pos = i + 1U;
            break;
        }
    }

    if (end_pos > kMaxImageAltCodePoints) {
        end_pos = kMaxImageAltCodePoints;
        if (end_pos < clean.size() && clean[end_pos] != U' ') {
            std::size_t last_space = end_pos;
            while (last_space > kMinWordBreakCutCodePoints && clean[last_space - 1U] != U' ') {
                --last_space;
            }
            if (last_space > kMinWordBreakCutCodePoints) {
                end_pos = last_space - 1U;
            }
        }
        while (end_pos > 0U && clean[end_pos - 1U] == U' ') {
            --end_pos;
        }
    }
    return to_utf8(std::u32string_view(clean).substr(0U, end_pos));
}

struct ColumnRun {
    std::size_t first = 0U;
    std::size_t last = 0U;  // exclusive
    double x0 = kCoordSentinel;
    double x1 = -kCoordSentinel;
    double y_top = -kCoordSentinel;
    double y_bottom = kCoordSentinel;
    bool full_width = false;
};

struct IndexedImage {
    const ImageRef* ref = nullptr;
    double anchor = 0.0;
    std::size_t orig_idx = 0U;
};

Block make_image_block(int page_index, const ImageRef& img) {
    Block b;
    b.kind = BlockKind::Image;
    b.page = page_index;
    b.image_path = img.path;
    b.image_alt = sanitize_single_line_utf8(img.alt);
    b.y_top = img.y_top;
    b.y_bottom = img.y_bottom;
    b.x0 = img.x;
    b.x1 = img.x_right;
    return b;
}

void pair_page_image_captions(std::span<Block> page_blocks, double ref_body) {
    const double max_vgap = kCaptionMaxVGapScale * ref_body;
    std::vector<char> used_caption(page_blocks.size(), 0);

    for (std::size_t i = 0U; i < page_blocks.size(); ++i) {
        Block& img = page_blocks[i];
        if (img.kind != BlockKind::Image || !img.image_alt.empty()) {
            continue;
        }
        const double iy_top = (std::max)(img.y_top, img.y_bottom);
        const double iy_bot = (std::min)(img.y_top, img.y_bottom);

        const auto try_candidate = [&](std::size_t ci) -> bool {
            if (ci >= page_blocks.size() || used_caption[ci] != 0) {
                return false;
            }
            const Block& cap = page_blocks[ci];
            if (cap.kind != BlockKind::Paragraph || cap.is_footnote || cap.page != img.page) {
                return false;
            }
            if (img.x1 > img.x0 && cap.x1 > cap.x0) {
                const double hoverlap = (std::min)(img.x1, cap.x1) - (std::max)(img.x0, cap.x0);
                if (hoverlap <= -ref_body) {
                    return false;
                }
            }
            const double vgap = std::max({0.0, iy_bot - cap.y_top, cap.y_bottom - iy_top});
            if (vgap > max_vgap) {
                return false;
            }
            std::string alt = extract_caption_alt_text(cap);
            if (alt.empty()) {
                return false;
            }
            img.image_alt = std::move(alt);
            used_caption[ci] = 1;
            return true;
        };

        // Prefer caption immediately below in reading order, falling back to immediately above.
        if (i + 1U < page_blocks.size() && try_candidate(i + 1U)) {
            continue;
        }
        if (i > 0U) {
            (void)try_candidate(i - 1U);
        }
    }
}

void splice_page_images(std::span<Block> page_blocks, const PageData& page, double ref_body,
                        std::vector<Block>& out) {
    std::vector<IndexedImage> imgs;
    imgs.reserve(page.images.size());
    for (std::size_t k = 0U; k < page.images.size(); ++k) {
        const ImageRef& im = page.images[k];
        imgs.push_back({&im, 0.5 * (im.y_top + im.y_bottom), k});
    }
    std::stable_sort(imgs.begin(), imgs.end(),
                     [](const IndexedImage& a, const IndexedImage& b) noexcept {
                         if (a.anchor != b.anchor) {
                             return a.anchor > b.anchor;
                         }
                         if (a.ref->x != b.ref->x) {
                             return a.ref->x < b.ref->x;
                         }
                         return a.orig_idx < b.orig_idx;
                     });

    // Keep relocated document-end numbered footnotes after body/image blocks.
    std::size_t body_end = page_blocks.size();
    while (body_end > 0U && page_blocks[body_end - 1U].is_footnote &&
           page_blocks[body_end - 1U].footnote_numbered) {
        --body_end;
    }

    const std::size_t page_out_start = out.size();
    if (body_end == 0U) {
        for (const IndexedImage& im : imgs) {
            out.push_back(make_image_block(page.index, *im.ref));
        }
        for (std::size_t k = 0U; k < page_blocks.size(); ++k) {
            out.push_back(std::move(page_blocks[k]));
        }
        return;
    }

    // Detect upward y-resets indicating a multi-column reading sequence.
    std::vector<double> col_split_xs;
    for (std::size_t i = 1U; i < body_end; ++i) {
        const Block& prev = page_blocks[i - 1U];
        const Block& cur = page_blocks[i];
        if (cur.y_top > prev.y_bottom + kColResetUpwardScale * ref_body &&
            cur.x0 > prev.x0 + kColResetRightShiftScale * ref_body) {
            col_split_xs.push_back(cur.x0);
        }
    }

    const auto spans_gutter = [&col_split_xs, ref_body](const Block& b) noexcept {
        if (col_split_xs.empty() || b.x1 <= b.x0) {
            return false;
        }
        for (const double split_x : col_split_xs) {
            if (b.x0 < split_x - kFullWidthSpanMarginScale * ref_body &&
                b.x1 > split_x + kFullWidthSpanMarginScale * ref_body) {
                return true;
            }
        }
        return false;
    };

    std::vector<ColumnRun> runs;
    if (col_split_xs.empty()) {
        ColumnRun single;
        single.first = 0U;
        single.last = body_end;
        for (std::size_t i = 0U; i < body_end; ++i) {
            single.x0 = (std::min)(single.x0, page_blocks[i].x0);
            single.x1 = (std::max)(single.x1, page_blocks[i].x1);
            single.y_top = (std::max)(single.y_top, page_blocks[i].y_top);
            single.y_bottom = (std::min)(single.y_bottom, page_blocks[i].y_bottom);
        }
        runs.push_back(single);
    } else {
        for (std::size_t i = 0U; i < body_end; ++i) {
            const Block& cur = page_blocks[i];
            const bool cur_full = spans_gutter(cur);
            bool start_new = runs.empty();
            if (!start_new) {
                const Block& prev = page_blocks[i - 1U];
                const ColumnRun& back = runs.back();
                const bool y_reset = cur.y_top > prev.y_bottom + kColResetUpwardScale * ref_body &&
                                     cur.x0 > prev.x0 + kColResetRightShiftScale * ref_body;
                const bool span_change = (cur_full != back.full_width);
                const bool left_jump_back =
                    !cur_full && cur.x0 < back.x0 - kFullWidthSpanMarginScale * ref_body;
                start_new = y_reset || span_change || left_jump_back;
            }
            if (start_new) {
                ColumnRun r;
                r.first = i;
                r.last = i + 1U;
                r.x0 = cur.x0;
                r.x1 = cur.x1;
                r.y_top = cur.y_top;
                r.y_bottom = cur.y_bottom;
                r.full_width = cur_full;
                runs.push_back(r);
            } else {
                ColumnRun& back = runs.back();
                back.last = i + 1U;
                back.x0 = (std::min)(back.x0, cur.x0);
                back.x1 = (std::max)(back.x1, cur.x1);
                back.y_top = (std::max)(back.y_top, cur.y_top);
                back.y_bottom = (std::min)(back.y_bottom, cur.y_bottom);
            }
        }
    }

    const auto choose_run = [&runs, &page, ref_body](const IndexedImage& im) -> std::size_t {
        if (runs.size() == 1U) {
            return 0U;
        }
        const double ix0 = im.ref->x;
        const double ix1 = im.ref->x_right;
        const double anchor = im.anchor;
        const auto vdist_to = [anchor](const ColumnRun& r) noexcept {
            return std::max({0.0, anchor - r.y_top, r.y_bottom - anchor});
        };

        if (ix1 <= ix0) {
            std::size_t best = 0U;
            double best_score = kCoordSentinel;
            for (std::size_t ri = 0U; ri < runs.size(); ++ri) {
                const ColumnRun& r = runs[ri];
                const bool x_in = ix0 >= r.x0 - kFullWidthSpanMarginScale * ref_body &&
                                  ix0 <= r.x1 + kFullWidthSpanMarginScale * ref_body;
                if (!x_in) {
                    continue;
                }
                const double score = vdist_to(r) + (r.full_width ? ref_body : 0.0);
                if (score < best_score) {
                    best_score = score;
                    best = ri;
                }
            }
            return best;
        }

        const double img_w = ix1 - ix0;
        std::vector<std::size_t> overlapped_narrow;
        for (std::size_t ri = 0U; ri < runs.size(); ++ri) {
            const ColumnRun& r = runs[ri];
            if (r.full_width) {
                continue;
            }
            const double rw = (std::max)(r.x1 - r.x0, kMinFontSizePt);
            const double ov = (std::min)(ix1, r.x1) - (std::max)(ix0, r.x0);
            if (ov >= kSpannedColOverlapRatio * rw) {
                overlapped_narrow.push_back(ri);
            }
        }

        const bool wide_spanning =
            (page.width > 0.0 && img_w > kWideImagePageRatio * page.width) ||
            overlapped_narrow.size() >= 2U;
        if (wide_spanning) {
            std::size_t best_full = runs.size();
            double best_vdist = kCoordSentinel;
            for (std::size_t ri = 0U; ri < runs.size(); ++ri) {
                if (!runs[ri].full_width) {
                    continue;
                }
                const double vd = vdist_to(runs[ri]);
                if (vd <= kWideRunAttachMaxGapScale * ref_body && vd < best_vdist) {
                    best_vdist = vd;
                    best_full = ri;
                }
            }
            if (best_full < runs.size()) {
                return best_full;
            }
            if (!overlapped_narrow.empty()) {
                double min_bot = kCoordSentinel;
                for (const std::size_t ri : overlapped_narrow) {
                    min_bot = (std::min)(min_bot, runs[ri].y_bottom);
                }
                if (anchor <= min_bot + kColResetUpwardScale * ref_body) {
                    return overlapped_narrow.back();
                }
                return overlapped_narrow.front();
            }
        }

        std::size_t best = 0U;
        double best_overlap_ratio = -1.0;
        double best_vdist = kCoordSentinel;
        for (std::size_t ri = 0U; ri < runs.size(); ++ri) {
            const ColumnRun& r = runs[ri];
            const double ov = (std::min)(ix1, r.x1) - (std::max)(ix0, r.x0);
            if (ov <= 0.0) {
                continue;
            }
            double ratio = ov / img_w;
            if (r.full_width && !wide_spanning) {
                ratio *= 0.5;
            }
            const double vd = vdist_to(r);
            if (ratio > best_overlap_ratio + 0.05 ||
                (std::fabs(ratio - best_overlap_ratio) <= 0.05 && vd < best_vdist)) {
                best_overlap_ratio = ratio;
                best_vdist = vd;
                best = ri;
            }
        }
        if (best_overlap_ratio > 0.0) {
            return best;
        }

        const double icx = 0.5 * (ix0 + ix1);
        double best_dx = kCoordSentinel;
        for (std::size_t ri = 0U; ri < runs.size(); ++ri) {
            const double rcx = 0.5 * (runs[ri].x0 + runs[ri].x1);
            const double dx = std::fabs(icx - rcx);
            if (dx < best_dx) {
                best_dx = dx;
                best = ri;
            }
        }
        return best;
    };

    std::vector<std::vector<IndexedImage>> run_imgs(runs.size());
    for (const IndexedImage& im : imgs) {
        run_imgs[choose_run(im)].push_back(im);
    }

    for (std::size_t ri = 0U; ri < runs.size(); ++ri) {
        const ColumnRun& r = runs[ri];
        const auto& rimgs = run_imgs[ri];
        if (rimgs.empty()) {
            for (std::size_t bi = r.first; bi < r.last; ++bi) {
                out.push_back(std::move(page_blocks[bi]));
            }
            continue;
        }

        // Single O(M + K) two-pointer sweep over descending y_top and img.anchor.
        const std::size_t run_len = r.last - r.first;
        std::vector<std::pair<double, std::size_t>> order;
        order.reserve(run_len);
        bool monotonic = true;
        for (std::size_t off = 0U; off < run_len; ++off) {
            const double yt = page_blocks[r.first + off].y_top;
            if (off > 0U && yt > order.back().first) {
                monotonic = false;
            }
            order.emplace_back(yt, off);
        }
        if (!monotonic) {
            std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) noexcept {
                if (a.first != b.first) {
                    return a.first > b.first;
                }
                return a.second < b.second;
            });
        }

        std::vector<std::vector<Block>> before_slot(run_len + 1U);
        std::size_t p = 0U;
        for (const IndexedImage& im : rimgs) {
            while (p < run_len && order[p].first >= im.anchor) {
                ++p;
            }
            const std::size_t slot = p < run_len ? order[p].second : run_len;
            before_slot[slot].push_back(make_image_block(page.index, *im.ref));
        }
        for (std::size_t off = 0U; off < run_len; ++off) {
            for (Block& ib : before_slot[off]) {
                out.push_back(std::move(ib));
            }
            out.push_back(std::move(page_blocks[r.first + off]));
        }
        for (Block& ib : before_slot[run_len]) {
            out.push_back(std::move(ib));
        }
    }

    const std::size_t page_body_out_end = out.size();
    pair_page_image_captions(
        std::span<Block>(out).subspan(page_out_start, page_body_out_end - page_out_start),
        ref_body);

    for (std::size_t bi = body_end; bi < page_blocks.size(); ++bi) {
        out.push_back(std::move(page_blocks[bi]));
    }
}

void insert_image_blocks(std::vector<Block>& blocks, const std::vector<PageData>& pages,
                         double body_size) {
    std::size_t total_images = 0U;
    for (const PageData& page : pages) {
        total_images += page.images.size();
    }
    if (total_images == 0U) {
        return;
    }

    const double ref_body = (std::max)(body_size, kMinFontSizePt);
    std::vector<const PageData*> img_pages;
    img_pages.reserve(pages.size());
    for (const PageData& page : pages) {
        if (!page.images.empty()) {
            img_pages.push_back(&page);
        }
    }
    std::stable_sort(img_pages.begin(), img_pages.end(),
                     [](const PageData* a, const PageData* b) noexcept {
                         return a->index < b->index;
                     });

    std::vector<Block> out;
    out.reserve(blocks.size() + total_images);

    std::size_t b_idx = 0U;
    std::size_t p_idx = 0U;
    while (b_idx < blocks.size() || p_idx < img_pages.size()) {
        if (p_idx < img_pages.size() &&
            (b_idx >= blocks.size() || img_pages[p_idx]->index < blocks[b_idx].page)) {
            splice_page_images(std::span<Block>{}, *img_pages[p_idx], ref_body, out);
            ++p_idx;
            continue;
        }
        const int cur_page = blocks[b_idx].page;
        std::size_t b_end = b_idx + 1U;
        while (b_end < blocks.size() && blocks[b_end].page == cur_page) {
            ++b_end;
        }
        if (p_idx < img_pages.size() && img_pages[p_idx]->index == cur_page) {
            splice_page_images(std::span<Block>(blocks).subspan(b_idx, b_end - b_idx),
                               *img_pages[p_idx], ref_body, out);
            ++p_idx;
        } else {
            for (std::size_t k = b_idx; k < b_end; ++k) {
                out.push_back(std::move(blocks[k]));
            }
        }
        b_idx = b_end;
    }

    blocks = std::move(out);
}

class MergeRuledTableContinuationsPass final : public DocumentPass {
protected:
    bool enabled(const AnalyzeParams& params) const noexcept override {
        return params.detect_tables;
    }
    void apply(Document& doc, const PassContext&) const override {
        merge_ruled_table_continuations(doc.blocks);
    }
};

class FoldTocOrphanRowsPass final : public DocumentPass {
protected:
    bool enabled(const AnalyzeParams& params) const noexcept override {
        return params.detect_tables;
    }
    void apply(Document& doc, const PassContext&) const override {
        fold_toc_orphan_rows(doc.blocks);
    }
};

class StripHeadersFootersPass final : public DocumentPass {
protected:
    bool enabled(const AnalyzeParams& params) const noexcept override {
        return params.strip_headers_footers;
    }
    void apply(Document& doc, const PassContext& ctx) const override {
        strip_headers_footers(doc.blocks, ctx.pages);
    }
};

class ClassifyHeadingsPass final : public DocumentPass {
protected:
    bool enabled(const AnalyzeParams& params) const noexcept override {
        return params.detect_headings;
    }
    void apply(Document& doc, const PassContext&) const override {
        classify_headings(doc.blocks, doc.body_size, doc.meta.outlines);
    }
};

class FilterHorizontalRulesPass final : public DocumentPass {
protected:
    void apply(Document& doc, const PassContext&) const override {
        filter_horizontal_rules(doc.blocks, doc.body_size);
    }
};

class RelocateFootnotesPass final : public DocumentPass {
protected:
    void apply(Document& doc, const PassContext&) const override {
        relocate_footnotes(doc.blocks);
    }
};

class MergeTrailingMathAtomsPass final : public DocumentPass {
protected:
    void apply(Document& doc, const PassContext&) const override {
        merge_trailing_math_atoms(doc.blocks);
    }
};

class MergeReferenceEntriesPass final : public DocumentPass {
protected:
    void apply(Document& doc, const PassContext&) const override {
        merge_reference_entries(doc.blocks);
    }
};

class MergeCrossPageParagraphsPass final : public DocumentPass {
protected:
    void apply(Document& doc, const PassContext&) const override {
        merge_cross_page_paragraphs(doc.blocks);
    }
};

class InsertImageBlocksPass final : public DocumentPass {
protected:
    void apply(Document& doc, const PassContext& ctx) const override {
        insert_image_blocks(doc.blocks, ctx.pages, doc.body_size);
    }
};

}  // namespace

DocumentPass::~DocumentPass() = default;

void DocumentPass::run(Document& doc, const PassContext& ctx) const {
    if (enabled(ctx.params)) {
        apply(doc, ctx);
    }
}

bool DocumentPass::enabled(const AnalyzeParams& params) const noexcept {
    (void)params;
    return true;
}

// Factory Method: assembles the document-cleanup pipeline. THE ORDER IS A
// CONTRACT. Several passes depend on running before or after another, and the
// output stays byte-identical only when this exact sequence is preserved.
std::vector<std::unique_ptr<DocumentPass>> build_pass_pipeline() {
    std::vector<std::unique_ptr<DocumentPass>> passes;
    passes.reserve(kPipelinePassCount);
    passes.push_back(std::make_unique<MergeRuledTableContinuationsPass>());
    passes.push_back(std::make_unique<FoldTocOrphanRowsPass>());
    passes.push_back(std::make_unique<StripHeadersFootersPass>());
    passes.push_back(std::make_unique<ClassifyHeadingsPass>());
    passes.push_back(std::make_unique<FilterHorizontalRulesPass>());
    passes.push_back(std::make_unique<RelocateFootnotesPass>());
    passes.push_back(std::make_unique<MergeTrailingMathAtomsPass>());
    passes.push_back(std::make_unique<MergeReferenceEntriesPass>());
    passes.push_back(std::make_unique<MergeCrossPageParagraphsPass>());
    passes.push_back(std::make_unique<InsertImageBlocksPass>());
    return passes;
}

}  // namespace pdf2md::layout
