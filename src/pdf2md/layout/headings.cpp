#include "pdf2md/layout/headings.h"

#include "pdf2md/model/document_utils.h"
#include "pdf2md/model/geometry.h"
#include "pdf2md/utf8.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pdf2md::layout {

using model::is_citation_lead;
using model::kCoordSentinel;
using model::kMinFontSizePt;

namespace {

constexpr char32_t kAsciiLimitCp = 0x80U;
constexpr char32_t kMinusSignCp = 0x2212U;
constexpr double kHalfPtScale = 2.0;
constexpr std::size_t kHeadingMaxLines = 3U;
constexpr std::size_t kOutlineMaxHeadingLines = 4U;
constexpr std::size_t kHeadingMaxChars = 220U;
constexpr double kHeadingMinSizeScale = 1.12;
constexpr std::size_t kHeadingMaxDocCharPercent = 30U;
constexpr std::size_t kPercentScale = 100U;
constexpr int kMaxHeadingLevel = 6;
constexpr int kMinBoldHeadingLevel = 2;
constexpr std::size_t kBoldHeadingMaxChars = 90U;
constexpr double kBoldHeadingMinSizeScale = 0.98;
constexpr std::size_t kMaxMetaLabelLen = 40U;
constexpr int kRepeatedHeadingMinCount = 3;
constexpr double kInsetCalloutMinRatio = 0.02;
constexpr std::size_t kRunInHeadingMaxChars = 140U;
constexpr double kSiblingListSizeTolRatio = 0.12;
constexpr double kOutlineMaxYDistScale = 6.0;

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

std::u32string decode_utf8(std::string_view utf8) {
    std::u32string out;
    out.reserve(utf8.size());
    std::size_t i = 0U;
    while (i < utf8.size()) {
        const auto b0 = static_cast<std::uint32_t>(static_cast<unsigned char>(utf8[i]));
        if (b0 < 0x80U) {
            out.push_back(static_cast<char32_t>(b0));
            ++i;
            continue;
        }
        if ((b0 & 0xE0U) == 0xC0U && i + 1U < utf8.size()) {
            const auto b1 = static_cast<std::uint32_t>(static_cast<unsigned char>(utf8[i + 1U]));
            if ((b1 & 0xC0U) == 0x80U) {
                const char32_t cp = static_cast<char32_t>(((b0 & 0x1FU) << 6U) | (b1 & 0x3FU));
                if (cp >= 0x80U) {
                    out.push_back(cp);
                }
                i += 2U;
                continue;
            }
        } else if ((b0 & 0xF0U) == 0xE0U && i + 2U < utf8.size()) {
            const auto b1 = static_cast<std::uint32_t>(static_cast<unsigned char>(utf8[i + 1U]));
            const auto b2 = static_cast<std::uint32_t>(static_cast<unsigned char>(utf8[i + 2U]));
            if ((b1 & 0xC0U) == 0x80U && (b2 & 0xC0U) == 0x80U) {
                const char32_t cp = static_cast<char32_t>(
                    ((b0 & 0x0FU) << 12U) | ((b1 & 0x3FU) << 6U) | (b2 & 0x3FU));
                if (cp >= 0x800U && (cp < 0xD800U || cp > 0xDFFFU)) {
                    out.push_back(cp);
                }
                i += 3U;
                continue;
            }
        } else if ((b0 & 0xF8U) == 0xF0U && i + 3U < utf8.size()) {
            const auto b1 = static_cast<std::uint32_t>(static_cast<unsigned char>(utf8[i + 1U]));
            const auto b2 = static_cast<std::uint32_t>(static_cast<unsigned char>(utf8[i + 2U]));
            const auto b3 = static_cast<std::uint32_t>(static_cast<unsigned char>(utf8[i + 3U]));
            if ((b1 & 0xC0U) == 0x80U && (b2 & 0xC0U) == 0x80U && (b3 & 0xC0U) == 0x80U) {
                const char32_t cp = static_cast<char32_t>(
                    ((b0 & 0x07U) << 18U) | ((b1 & 0x3FU) << 12U) | ((b2 & 0x3FU) << 6U) |
                    (b3 & 0x3FU));
                if (cp >= 0x10000U && cp <= 0x10FFFFU) {
                    out.push_back(cp);
                }
                i += 4U;
                continue;
            }
        }
        ++i;
    }
    return out;
}

int heading_number_depth(const Block& b) noexcept {
    if (b.lines.empty()) {
        return 0;
    }
    const std::u32string& s = b.lines.front().plain;
    std::size_t i = 0U;
    while (i < s.size() && s[i] == U' ') {
        ++i;
    }
    if (i >= s.size() || !is_ascii_digit(s[i])) {
        return 0;
    }
    int depth = 0;
    while (i < s.size() && is_ascii_digit(s[i])) {
        while (i < s.size() && is_ascii_digit(s[i])) {
            ++i;
        }
        ++depth;
        if (i + 1U < s.size() && s[i] == U'.' && is_ascii_digit(s[i + 1U])) {
            ++i;
        } else {
            break;
        }
    }
    if (i < s.size() && s[i] != U' ' && s[i] != U'.') {
        return 0;
    }
    return depth;
}

struct SectionNumber {
    std::u32string canon;
    int depth = 0;
};

SectionNumber parse_section_number(std::u32string_view s) {
    std::size_t i = 0U;
    while (i < s.size() && s[i] == U' ') {
        ++i;
    }
    std::u32string canon;
    int depth = 0;
    if (i + 2U < s.size() && is_ascii_upper(s[i]) && s[i + 1U] == U'.' &&
        is_ascii_digit(s[i + 2U])) {
        canon += s[i];
        ++depth;
        i += 2U;
    }
    bool have_number = false;
    while (i < s.size() && is_ascii_digit(s[i])) {
        if (!canon.empty()) {
            canon += U'.';
        }
        while (i < s.size() && is_ascii_digit(s[i])) {
            canon += s[i];
            ++i;
        }
        ++depth;
        have_number = true;
        if (i + 1U < s.size() && s[i] == U'.' && is_ascii_digit(s[i + 1U])) {
            ++i;
        } else {
            break;
        }
    }
    if (have_number && i < s.size() && s[i] == U'.') {
        ++i;
    }
    if (!have_number || i >= s.size() || s[i] != U' ') {
        return {};
    }
    std::size_t t = i;
    while (t < s.size() && s[t] == U' ') {
        ++t;
    }
    if (t >= s.size()) {
        return {};
    }
    return {std::move(canon), depth};
}

std::u32string effective_first_line(const Block& b) {
    if (b.lines.empty()) {
        return {};
    }
    if (b.kind == BlockKind::ListItem && !b.list_marker.empty()) {
        std::u32string out = b.list_marker;
        out += U' ';
        out += b.lines.front().plain;
        return out;
    }
    return b.lines.front().plain;
}

std::size_t effective_block_len(const Block& b) noexcept {
    std::size_t len = 0U;
    for (const Line& ln : b.lines) {
        len += ln.plain.size();
    }
    if (b.kind == BlockKind::ListItem && !b.list_marker.empty() && !b.lines.empty()) {
        len += b.list_marker.size() + 1U;
    }
    return len;
}

std::string heading_norm_key_u32(std::span<const std::u32string> lines) {
    std::string k;
    bool gap = false;
    for (const std::u32string& ln : lines) {
        for (const char32_t c : ln) {
            if (is_ascii_upper(c) || is_ascii_lower(c) || is_ascii_digit(c)) {
                if (gap && !k.empty()) {
                    k += ' ';
                }
                k += ascii_tolower(c);
                gap = false;
            } else {
                gap = true;
            }
        }
        gap = true;
    }
    return k;
}

std::string heading_norm_key(const Block& b) {
    std::vector<std::u32string> lines;
    lines.reserve(b.lines.size());
    for (std::size_t i = 0U; i < b.lines.size(); ++i) {
        lines.push_back(i == 0U ? effective_first_line(b) : b.lines[i].plain);
    }
    return heading_norm_key_u32(lines);
}

std::string heading_meta_label(std::u32string_view s) {
    std::size_t i = 0U;
    while (i < s.size() && s[i] == U' ') {
        ++i;
    }
    std::string label;
    bool has_letter = false;
    for (; i < s.size(); ++i) {
        const char32_t c = s[i];
        if (c == U':') {
            if (i + 1U < s.size() && s[i + 1U] != U' ') {
                return {};
            }
            break;
        }
        if (is_letter_cp(c)) {
            has_letter = true;
        } else if (!is_ascii_digit(c) && c != U' ' && c != U'-' && c != U'_' && c != U'/' &&
                   c != U'(' && c != U')' && c != U'&' && c != U',') {
            return {};
        }
        if (c < kAsciiLimitCp) {
            label += static_cast<char>(c);
        }
        if (label.size() > kMaxMetaLabelLen) {
            return {};
        }
    }
    if (i >= s.size() || !has_letter) {
        return {};
    }
    while (!label.empty() && label.back() == ' ') {
        label.pop_back();
    }
    return label;
}

bool heading_is_caption(std::u32string_view s) noexcept {
    std::size_t i = 0U;
    while (i < s.size() && s[i] == U' ') {
        ++i;
    }
    constexpr std::array<std::u32string_view, 7> kPrefixes = {
        U"figure", U"fig", U"table", U"tab", U"chart", U"diagram", U"scheme"};
    for (const std::u32string_view w : kPrefixes) {
        if (s.size() < i + w.size() + 2U) {
            continue;
        }
        if (!ascii_iequals(s.substr(i, w.size()), w)) {
            continue;
        }
        std::size_t k = i + w.size();
        if (k < s.size() && s[k] == U'.') {
            ++k;
        }
        if (k >= s.size() || s[k] != U' ') {
            continue;
        }
        while (k < s.size() && s[k] == U' ') {
            ++k;
        }
        if (k < s.size() && is_ascii_digit(s[k])) {
            return true;
        }
    }
    return false;
}

constexpr bool is_structural_noun(std::u32string_view word) noexcept {
    constexpr std::array<std::u32string_view, 13> kNouns = {
        U"chapter", U"section", U"part",    U"appendix", U"stage",   U"phase",  U"step",
        U"volume",  U"article", U"module",  U"level",    U"version", U"release"};
    for (const std::u32string_view noun : kNouns) {
        if (ascii_iequals(word, noun)) {
            return true;
        }
    }
    return false;
}

constexpr bool is_bare_number_token(std::u32string_view tok) noexcept {
    if (tok.empty()) {
        return false;
    }
    std::size_t i = 0U;
    if (tok[i] == U'+' || tok[i] == U'-' || tok[i] == kMinusSignCp) {
        ++i;
    }
    std::size_t end = tok.size();
    if (end > i && tok[end - 1U] == U'%') {
        --end;
    }
    if (i >= end) {
        return false;
    }
    bool any_digit = false;
    for (std::size_t k = i; k < end; ++k) {
        const char32_t c = tok[k];
        if (is_ascii_digit(c)) {
            any_digit = true;
        } else if (c != U',' && c != U'.') {
            return false;
        }
    }
    return any_digit;
}

bool ends_with_bare_number(const Block& b, std::u32string_view first_line) noexcept {
    if (b.lines.empty()) {
        return false;
    }
    const std::u32string_view lastln =
        b.lines.size() == 1U ? first_line : std::u32string_view(b.lines.back().plain);
    std::size_t e = lastln.size();
    while (e > 0U && lastln[e - 1U] == U' ') {
        --e;
    }
    if (e == 0U) {
        return false;
    }
    std::size_t tok_start = e;
    while (tok_start > 0U && lastln[tok_start - 1U] != U' ') {
        --tok_start;
    }
    if (!is_bare_number_token(lastln.substr(tok_start, e - tok_start))) {
        return false;
    }

    // Extract the immediately preceding whitespace-delimited word.
    std::u32string_view prev_src = lastln.substr(0U, tok_start);
    std::size_t pe = prev_src.size();
    while (pe > 0U && prev_src[pe - 1U] == U' ') {
        --pe;
    }
    if (pe == 0U && b.lines.size() >= 2U) {
        prev_src = b.lines.size() == 2U ? first_line
                                        : std::u32string_view(b.lines[b.lines.size() - 2U].plain);
        pe = prev_src.size();
        while (pe > 0U && prev_src[pe - 1U] == U' ') {
            --pe;
        }
    }
    if (pe == 0U) {
        return true;
    }
    std::size_t ps = pe;
    while (ps > 0U && prev_src[ps - 1U] != U' ') {
        --ps;
    }
    return !is_structural_noun(prev_src.substr(ps, pe - ps));
}

bool is_isolated_ordered_listitem(const std::vector<Block>& blocks, std::size_t idx) noexcept {
    const Block& b = blocks[idx];
    if (b.kind != BlockKind::ListItem || !b.list_ordered || b.task_state != TaskState::None ||
        b.lines.empty() || b.lines.front().plain.empty()) {
        return false;
    }
    const auto same_size_sibling = [&b](const Block& other) noexcept {
        if (other.kind != BlockKind::ListItem || !other.list_ordered ||
            other.list_indent != b.list_indent) {
            return false;
        }
        const double max_sz = (std::max)({other.size, b.size, kMinFontSizePt});
        return std::fabs(other.size - b.size) <= kSiblingListSizeTolRatio * max_sz;
    };

    for (std::ptrdiff_t j = static_cast<std::ptrdiff_t>(idx) - 1; j >= 0; --j) {
        const Block& prev = blocks[static_cast<std::size_t>(j)];
        if (prev.kind == BlockKind::ListItem && prev.list_indent > b.list_indent) {
            continue;
        }
        if (same_size_sibling(prev)) {
            return false;
        }
        break;
    }
    for (std::size_t j = idx + 1U; j < blocks.size(); ++j) {
        const Block& next = blocks[j];
        if (next.kind == BlockKind::ListItem && next.list_indent > b.list_indent) {
            continue;
        }
        if (same_size_sibling(next)) {
            return false;
        }
        break;
    }
    return true;
}

void restore_list_marker_into_line(Block& b) {
    if (b.kind != BlockKind::ListItem) {
        return;
    }
    if (!b.list_marker.empty() && !b.lines.empty()) {
        std::u32string prefix = b.list_marker;
        prefix += U' ';
        Line& ln = b.lines.front();
        ln.plain.insert(0U, prefix);
        if (!ln.runs.empty() && !ln.runs.front().math && !ln.runs.front().raw &&
            ln.runs.front().link_url.empty()) {
            ln.runs.front().text.insert(0U, prefix);
        } else {
            StyledRun lead;
            lead.text = std::move(prefix);
            lead.bold = b.bold_majority;
            ln.runs.insert(ln.runs.begin(), std::move(lead));
        }
    }
    b.list_ordered = false;
    b.list_indent = 0;
    b.list_marker.clear();
}

struct OutlineNorm {
    std::string full;
    std::string sec;
    std::string body;
};

constexpr bool is_outline_word_cp(char32_t cp) noexcept {
    if (cp < kAsciiLimitCp) {
        return is_ascii_alpha(cp) || is_ascii_digit(cp);
    }
    if (is_space_cp(cp) || is_line_break_cp(cp) || is_bullet_cp(cp) || is_dashy_cp(cp)) {
        return false;
    }
    switch (cp) {
        case 0x2018U:
        case 0x2019U:
        case 0x201CU:
        case 0x201DU:
        case 0x3001U:
        case 0x3002U:
        case 0x300CU:
        case 0x300DU:
        case 0xFF01U:
        case 0xFF08U:
        case 0xFF09U:
        case 0xFF0CU:
        case 0xFF0EU:
        case 0xFF1AU:
        case 0xFF1BU:
        case 0xFF1FU:
            return false;
        default:
            return true;
    }
}

std::string normalize_outline_span(std::u32string_view s) {
    std::string out;
    out.reserve(s.size());
    bool gap = false;
    for (const char32_t cp : s) {
        if (is_outline_word_cp(cp)) {
            if (gap && !out.empty()) {
                out += ' ';
            }
            if (cp < kAsciiLimitCp) {
                out += ascii_tolower(cp);
            } else {
                append_utf8(out, cp);
            }
            gap = false;
        } else {
            gap = true;
        }
    }
    return out;
}

OutlineNorm normalize_outline_u32(std::u32string_view s) {
    OutlineNorm res;
    res.full = normalize_outline_span(s);
    if (res.full.empty()) {
        return res;
    }

    std::size_t i = 0U;
    while (i < s.size() && is_space_cp(s[i])) {
        ++i;
    }

    // Optional structural keyword ("Chapter", "Section", "Appendix", "Part") before number.
    std::size_t cursor = i;
    std::size_t wend = cursor;
    while (wend < s.size() && is_ascii_alpha(s[wend])) {
        ++wend;
    }
    if (wend > cursor && is_structural_noun(s.substr(cursor, wend - cursor))) {
        std::size_t after_kw = wend;
        while (after_kw < s.size() && is_space_cp(s[after_kw])) {
            ++after_kw;
        }
        if (after_kw > wend) {
            cursor = after_kw;
        }
    }

    std::string sec;
    if (cursor < s.size() && is_ascii_alpha(s[cursor]) &&
        (cursor + 1U < s.size() && (s[cursor + 1U] == U'.' || is_space_cp(s[cursor + 1U])))) {
        sec += ascii_tolower(s[cursor]);
        ++cursor;
        if (cursor < s.size() && s[cursor] == U'.' && cursor + 1U < s.size() &&
            is_ascii_digit(s[cursor + 1U])) {
            ++cursor;
            sec += '.';
        }
    }
    while (cursor < s.size() && is_ascii_digit(s[cursor])) {
        while (cursor < s.size() && is_ascii_digit(s[cursor])) {
            sec += static_cast<char>(s[cursor]);
            ++cursor;
        }
        if (cursor + 1U < s.size() && s[cursor] == U'.' && is_ascii_digit(s[cursor + 1U])) {
            sec += '.';
            ++cursor;
        } else {
            break;
        }
    }
    if (!sec.empty()) {
        if (cursor < s.size() && (s[cursor] == U'.' || s[cursor] == U':' || s[cursor] == U')')) {
            ++cursor;
        }
        if (cursor < s.size() && (is_space_cp(s[cursor]) || is_dashy_cp(s[cursor]))) {
            while (cursor < s.size() && (is_space_cp(s[cursor]) || is_dashy_cp(s[cursor]))) {
                ++cursor;
            }
            std::string stripped = normalize_outline_span(s.substr(cursor));
            if (!stripped.empty()) {
                res.sec = std::move(sec);
                res.body = std::move(stripped);
                return res;
            }
        }
    }
    res.body = res.full;
    return res;
}

OutlineNorm normalize_block_outline(const Block& b) {
    std::u32string joined;
    for (std::size_t i = 0U; i < b.lines.size(); ++i) {
        if (i > 0U) {
            joined += U' ';
        }
        if (i == 0U) {
            joined += effective_first_line(b);
        } else {
            joined += b.lines[i].plain;
        }
    }
    return normalize_outline_u32(joined);
}

bool outline_titles_match(const OutlineNorm& entry, const OutlineNorm& blk) noexcept {
    if (entry.full.empty() || blk.full.empty()) {
        return false;
    }
    if (entry.full == blk.full) {
        return true;
    }
    if (!entry.body.empty() && entry.body == blk.body &&
        (entry.sec.empty() || blk.sec.empty() || entry.sec == blk.sec)) {
        return true;
    }
    return false;
}

void apply_outline_headings(std::vector<Block>& blocks, double body_size,
                            std::span<const OutlineEntry> outlines,
                            std::span<const char> isolated_ordered) {
    if (outlines.empty() || blocks.empty()) {
        return;
    }
    const double ref_body = (std::max)(body_size, kMinFontSizePt);
    const double max_y_dist = kOutlineMaxYDistScale * ref_body;

    std::vector<OutlineNorm> block_norms(blocks.size());
    std::vector<char> eligible(blocks.size(), 0);
    for (std::size_t bi = 0U; bi < blocks.size(); ++bi) {
        const Block& b = blocks[bi];
        const bool kind_ok = b.kind == BlockKind::Paragraph || b.kind == BlockKind::Heading ||
                             (bi < isolated_ordered.size() && isolated_ordered[bi] != 0);
        if (!kind_ok || b.is_footnote || b.lines.empty() ||
            b.lines.size() > kOutlineMaxHeadingLines) {
            continue;
        }
        block_norms[bi] = normalize_block_outline(b);
        if (!block_norms[bi].full.empty()) {
            eligible[bi] = 1;
        }
    }

    struct Match {
        std::size_t block_idx = 0U;
        int entry_level = 1;
    };
    std::vector<Match> matches;
    matches.reserve(outlines.size());
    std::vector<char> claimed(blocks.size(), 0);

    int first_l1_page = -1;
    std::size_t first_l1_block = blocks.size();
    bool seen_l1 = false;

    for (const OutlineEntry& entry : outlines) {
        if (entry.page < 0 || entry.title.empty()) {
            continue;
        }
        const int eff_level = (std::max)(entry.level, 1);
        const OutlineNorm entry_norm = normalize_outline_u32(decode_utf8(entry.title));
        if (entry_norm.full.empty()) {
            continue;
        }

        std::size_t best = blocks.size();
        double best_dist = kCoordSentinel;
        for (std::size_t bi = 0U; bi < blocks.size(); ++bi) {
            if (!eligible[bi] || claimed[bi] || blocks[bi].page != entry.page) {
                continue;
            }
            if (!outline_titles_match(entry_norm, block_norms[bi])) {
                continue;
            }
            if (entry.has_y) {
                const double dist = std::fabs(blocks[bi].y_top - entry.y);
                if (dist <= max_y_dist && dist < best_dist) {
                    best_dist = dist;
                    best = bi;
                }
            } else {
                best = bi;
                break;
            }
        }
        if (!seen_l1 && eff_level == 1) {
            seen_l1 = true;
            first_l1_page = entry.page;
            first_l1_block = best;
        }
        if (best < blocks.size()) {
            claimed[best] = 1;
            matches.push_back({best, eff_level});
        }
    }

    int level_offset = 0;
    if (seen_l1) {
        for (std::size_t bi = 0U; bi < blocks.size(); ++bi) {
            const Block& b = blocks[bi];
            if (b.page != 0) {
                break;
            }
            if (b.kind == BlockKind::Heading && b.heading_level == 1 &&
                b.size >= body_size * kHeadingMinSizeScale) {
                if (first_l1_page > 0 || (first_l1_block < blocks.size() && bi < first_l1_block)) {
                    level_offset = 1;
                }
                break;
            }
        }
    }

    for (const Match& m : matches) {
        Block& b = blocks[m.block_idx];
        if (b.kind == BlockKind::ListItem) {
            restore_list_marker_into_line(b);
        }
        b.kind = BlockKind::Heading;
        b.blockquote = false;
        b.heading_level = std::clamp(m.entry_level + level_offset, 1, kMaxHeadingLevel);
    }
}

}  // namespace

void classify_headings(std::vector<Block>& blocks, double body_size,
                       std::span<const OutlineEntry> outlines) {
    std::vector<char> isolated_ordered(blocks.size(), 0);
    std::vector<char> bold_ordered_cand(blocks.size(), 0);
    for (std::size_t i = 0U; i < blocks.size(); ++i) {
        if (is_isolated_ordered_listitem(blocks, i)) {
            isolated_ordered[i] = 1;
            const Block& b = blocks[i];
            if (b.lines.size() == 1U && b.bold_majority &&
                b.size >= body_size * kBoldHeadingMinSizeScale) {
                bold_ordered_cand[i] = 1;
            }
        }
    }

    std::size_t total_chars = 0U;
    for (const Block& b : blocks) {
        for (const Line& ln : b.lines) {
            total_chars += ln.plain.size();
        }
    }

    const auto tier_key = [](double size) noexcept { return std::lround(size * kHalfPtScale); };

    std::vector<std::pair<long, std::size_t>> tier_chars;
    for (std::size_t i = 0U; i < blocks.size(); ++i) {
        const Block& b = blocks[i];
        const bool eligible_kind = (b.kind == BlockKind::Paragraph || bold_ordered_cand[i] != 0);
        if (!eligible_kind || b.lines.size() > kHeadingMaxLines) {
            continue;
        }
        const std::size_t len = effective_block_len(b);
        if (len == 0U || len > kHeadingMaxChars) {
            continue;
        }
        if (b.size >= body_size * kHeadingMinSizeScale) {
            const long key = tier_key(b.size);
            auto it = std::find_if(tier_chars.begin(), tier_chars.end(),
                                   [key](const auto& kv) noexcept { return kv.first == key; });
            if (it != tier_chars.end()) {
                it->second += len;
            } else {
                tier_chars.emplace_back(key, len);
            }
        }
    }
    std::sort(tier_chars.begin(), tier_chars.end(),
              [](const auto& a, const auto& b) noexcept { return a.first > b.first; });

    std::vector<long> tiers;
    tiers.reserve(tier_chars.size());
    for (const auto& [key, chars] : tier_chars) {
        if (total_chars == 0U ||
            chars * kPercentScale <= total_chars * kHeadingMaxDocCharPercent) {
            tiers.push_back(key);
        }
    }

    const auto level_of = [&tiers, &tier_key](double size) -> int {
        const long key = tier_key(size);
        for (std::size_t i = 0U; i < tiers.size(); ++i) {
            if (tiers[i] == key) {
                return static_cast<int>(
                    (std::min)(i + 1U, static_cast<std::size_t>(kMaxHeadingLevel)));
            }
        }
        return 0;
    };

    const auto size_tier_cand = [body_size](const Block& b, std::size_t len) noexcept {
        return b.lines.size() <= kHeadingMaxLines && len > 0U && len <= kHeadingMaxChars &&
               b.size >= body_size * kHeadingMinSizeScale;
    };
    const auto bold_line_cand = [body_size](const Block& b, std::size_t len) noexcept {
        return b.lines.size() == 1U && b.bold_majority && len > 0U && len <= kBoldHeadingMaxChars &&
               b.size >= body_size * kBoldHeadingMinSizeScale &&
               b.size < body_size * kHeadingMinSizeScale;
    };

    std::unordered_map<std::string, int> full_freq;
    std::unordered_map<std::string, int> label_freq;
    for (std::size_t i = 0U; i < blocks.size(); ++i) {
        const Block& b = blocks[i];
        const bool eligible_kind = (b.kind == BlockKind::Paragraph || bold_ordered_cand[i] != 0);
        if (!eligible_kind || b.lines.empty()) {
            continue;
        }
        const std::size_t len = effective_block_len(b);
        if (!size_tier_cand(b, len) && !bold_line_cand(b, len)) {
            continue;
        }
        ++full_freq[heading_norm_key(b)];
        std::string lab = heading_meta_label(effective_first_line(b));
        if (!lab.empty()) {
            ++label_freq[std::move(lab)];
        }
    }

    double body_x0 = 0.0;
    double body_x1 = 0.0;
    {
        std::vector<double> xs0;
        std::vector<double> xs1;
        for (const Block& b : blocks) {
            if (b.kind == BlockKind::Paragraph && b.lines.size() >= 2U) {
                xs0.push_back(b.x0);
                xs1.push_back(b.x1);
            }
        }
        if (!xs0.empty()) {
            std::sort(xs0.begin(), xs0.end());
            std::sort(xs1.begin(), xs1.end());
            body_x0 = xs0[xs0.size() / 10U];
            body_x1 = xs1[xs1.size() * 9U / 10U];
        }
    }
    const double body_w = body_x1 - body_x0;
    const auto is_inset_callout = [body_x0, body_x1, body_w](const Block& b) noexcept {
        return body_w > 0.0 && (b.x0 - body_x0) >= kInsetCalloutMinRatio * body_w &&
               (body_x1 - b.x1) >= kInsetCalloutMinRatio * body_w;
    };

    const auto& const_full_freq = full_freq;
    const auto& const_label_freq = label_freq;
    const auto titleish = [&const_full_freq, &const_label_freq](const Block& b) -> bool {
        if (b.lines.empty()) {
            return false;
        }
        const std::u32string first_ln = effective_first_line(b);
        const std::u32string& lastln = b.lines.size() == 1U ? first_ln : b.lines.back().plain;
        std::size_t e = lastln.size();
        while (e > 0U && lastln[e - 1U] == U' ') {
            --e;
        }
        if (e > 0U) {
            const char32_t last = lastln[e - 1U];
            if (last == U'.' || last == U',' || last == U';' || last == U':' || last == U'!' ||
                last == U'?') {
                return false;
            }
        }
        if (is_citation_lead(b.lines.front()) || heading_is_caption(first_ln) ||
            ends_with_bare_number(b, first_ln)) {
            return false;
        }
        const std::string lab = heading_meta_label(first_ln);
        if (!lab.empty()) {
            const auto lit = const_label_freq.find(lab);
            if (lit != const_label_freq.end() && lit->second >= kRepeatedHeadingMinCount) {
                return false;
            }
        }
        const auto fit = const_full_freq.find(heading_norm_key(b));
        if (fit != const_full_freq.end() && fit->second >= kRepeatedHeadingMinCount) {
            return false;
        }
        return true;
    };

    for (std::size_t i = 0U; i < blocks.size(); ++i) {
        Block& b = blocks[i];
        const bool is_ord_cand = (bold_ordered_cand[i] != 0);
        if (b.kind != BlockKind::Paragraph && !is_ord_cand) {
            continue;
        }
        const std::size_t len = effective_block_len(b);

        if (size_tier_cand(b, len)) {
            const int lvl = level_of(b.size);
            if (lvl > 0) {
                if (titleish(b)) {
                    if (is_ord_cand) {
                        restore_list_marker_into_line(b);
                    }
                    b.kind = BlockKind::Heading;
                    b.heading_level = lvl;
                } else if (!is_ord_cand) {
                    const char32_t last =
                        b.lines.back().plain.empty() ? U'\0' : b.lines.back().plain.back();
                    const bool sentence = last == U'.' || last == U'!' || last == U'?';
                    if (b.lines.size() >= 2U && sentence && is_inset_callout(b)) {
                        b.blockquote = true;
                    }
                }
                continue;
            }
        }
        if (bold_line_cand(b, len) && titleish(b)) {
            if (is_ord_cand) {
                restore_list_marker_into_line(b);
            }
            b.kind = BlockKind::Heading;
            b.heading_level = (std::max)(
                static_cast<int>(
                    (std::min)(tiers.size() + 1U, static_cast<std::size_t>(kMaxHeadingLevel))),
                kMinBoldHeadingLevel);
        }
    }

    int anchor_depth = 0;
    int anchor_level = 0;
    bool have_anchor = false;
    for (const Block& b : blocks) {
        if (b.kind != BlockKind::Heading) {
            continue;
        }
        const int d = heading_number_depth(b);
        if (d == 0) {
            continue;
        }
        if (!have_anchor || d < anchor_depth ||
            (d == anchor_depth && b.heading_level < anchor_level)) {
            anchor_depth = d;
            anchor_level = b.heading_level;
            have_anchor = true;
        }
    }
    if (have_anchor) {
        const int base = anchor_level - (anchor_depth - 1);
        for (Block& b : blocks) {
            if (b.kind != BlockKind::Heading) {
                continue;
            }
            const int d = heading_number_depth(b);
            if (d == 0) {
                continue;
            }
            b.heading_level = std::clamp(base + (d - 1), 1, kMaxHeadingLevel);
        }
    }

    std::unordered_map<std::u32string, int> heading_number_level;
    for (const Block& b : blocks) {
        if (b.kind != BlockKind::Heading || b.lines.empty()) {
            continue;
        }
        SectionNumber sn = parse_section_number(b.lines.front().plain);
        if (sn.depth == 0) {
            continue;
        }
        const auto it = heading_number_level.find(sn.canon);
        if (it == heading_number_level.end() || b.heading_level < it->second) {
            heading_number_level.insert_or_assign(std::move(sn.canon), b.heading_level);
        }
    }
    for (Block& b : blocks) {
        if (b.kind != BlockKind::Paragraph || !b.bold_majority || b.lines.size() != 1U) {
            continue;
        }
        const std::size_t len = b.lines.front().plain.size();
        if (len == 0U || len > kRunInHeadingMaxChars) {
            continue;
        }
        const SectionNumber sn = parse_section_number(b.lines.front().plain);
        if (sn.depth < 2) {
            continue;
        }
        const std::size_t dot = sn.canon.find_last_of(U'.');
        if (dot == std::u32string::npos) {
            continue;
        }
        const auto it = heading_number_level.find(sn.canon.substr(0U, dot));
        if (it == heading_number_level.end() || !titleish(b)) {
            continue;
        }
        b.kind = BlockKind::Heading;
        b.heading_level = std::clamp(it->second + 1, 1, kMaxHeadingLevel);
    }

    apply_outline_headings(blocks, body_size, outlines, isolated_ordered);
}

}  // namespace pdf2md::layout
