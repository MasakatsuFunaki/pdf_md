#include "pdf2md/markdown_writer.h"

#include "pdf2md/markdown/inline_normalizer.h"
#include "pdf2md/utf8.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pdf2md::markdown {

namespace {

constexpr int kMinHeadingLevel = 1;
constexpr int kMaxHeadingLevel = 6;
constexpr std::size_t kListIndentSpaces = 4U;
constexpr std::size_t kTocIndentSpaces = 2U;
constexpr std::size_t kMinFenceBackticks = 3U;
constexpr std::size_t kMaxOrderedListDigits = 9U;
constexpr int kMaxBareLinkParenDepth = 3;

constexpr char32_t kAsciiControlLimit = 0x20U;
constexpr char32_t kAsciiDel = 0x7FU;
constexpr unsigned int kNibbleShift = 4U;
constexpr unsigned int kNibbleMask = 0x0FU;

constexpr double kMinAdvancePt = 0.05;
constexpr double kMaxAdvanceEmRatio = 1.5;
constexpr double kDefaultMonoCellRatio = 0.6;
constexpr double kMinMedianCellPt = 0.1;

constexpr bool always_escaped(char32_t cp) noexcept {
    switch (cp) {
        case U'\\':
        case U'`':
        case U'*':
        case U'_':
        case U'[':
        case U'<':
        case U'|':
        case U'~':
        case U'$':  // $ delimits inline math
            return true;
        // ']' stays unescaped in prose: escaping both brackets yields "\[...\]",
        // which math-enabled renderers (GitHub, MathJax) read as a display-math
        // delimiter and pull onto its own centered line. Inside explicit link or
        // TOC labels, escape_md(..., escape_rbracket=true) escapes ']' too.
        default:
            return false;
    }
}

constexpr bool starts_with_ascii_icase(std::u32string_view text, std::size_t pos,
                                       std::string_view prefix) noexcept {
    if (pos + prefix.size() > text.size()) {
        return false;
    }
    for (std::size_t i = 0U; i < prefix.size(); ++i) {
        if (ascii_tolower(text[pos + i]) != prefix[i]) {
            return false;
        }
    }
    return true;
}

constexpr bool is_bare_url_char(char32_t cp) noexcept {
    if (cp <= kAsciiControlLimit || cp == kAsciiDel || is_space_cp(cp) || is_line_break_cp(cp)) {
        return false;
    }
    switch (cp) {
        case U'<':
        case U'>':
        case U'"':
        case U'`':
        case U'[':
        case U']':
            return false;
        default:
            return true;
    }
}

// If `pos` starts a bare prose `http://`, `https://`, or `mailto:` URL, returns
// the one-past-end index of that URL token; otherwise returns `pos`.
constexpr std::size_t bare_url_end(std::u32string_view text, std::size_t pos) noexcept {
    if (pos > 0U && (is_ascii_alpha(text[pos - 1U]) || is_ascii_digit(text[pos - 1U]))) {
        return pos;
    }
    std::size_t scheme_len = 0U;
    if (starts_with_ascii_icase(text, pos, "https://")) {
        scheme_len = 8U;
    } else if (starts_with_ascii_icase(text, pos, "http://") ||
               starts_with_ascii_icase(text, pos, "mailto:")) {
        scheme_len = 7U;
    }
    if (scheme_len == 0U) {
        return pos;
    }
    std::size_t end = pos + scheme_len;
    while (end < text.size() && is_bare_url_char(text[end])) {
        ++end;
    }
    return end > pos + scheme_len ? end : pos;
}

std::string escape_md(std::u32string_view text, bool in_link_label = false) {
    std::string out;
    out.reserve(text.size() + 8U);
    std::size_t url_until = 0U;
    for (std::size_t i = 0U; i < text.size(); ++i) {
        const char32_t cp = text[i];
        if (cp == kSoftHyphenCp) {
            continue;
        }
        if (!in_link_label && i >= url_until) {
            url_until = bare_url_end(text, i);
        }
        const bool in_bare_url = !in_link_label && i < url_until;
        const bool skip_autolink_punct = in_bare_url && (cp == U'_' || cp == U'~');
        if (!skip_autolink_punct && (always_escaped(cp) || (in_link_label && cp == U']'))) {
            out += '\\';
        }
        append_utf8(out, cp);
    }
    return out;
}

// Escapes block-level markers that only matter at the start of an output line.
void escape_line_start(std::string& s) {
    if (s.empty()) {
        return;
    }
    if (s[0] == '#' || s[0] == '>' || s[0] == '+' || s[0] == '-') {
        std::size_t run = 1U;
        if (s[0] == '#') {
            while (run < s.size() && s[run] == '#') {
                ++run;
            }
        }
        if (run >= s.size() || s[run] == ' ' || s[0] != '#') {
            s.insert(0U, "\\");
            return;
        }
    }
    std::size_t i = 0U;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        ++i;
    }
    if (i > 0U && i <= kMaxOrderedListDigits && i < s.size() && (s[i] == '.' || s[i] == ')') &&
        (i + 1U == s.size() || s[i + 1U] == ' ')) {
        s.insert(i, "\\");
    }
}

std::string escape_pipes(std::string s) {
    if (s.find('|') == std::string::npos) {
        return s;
    }
    std::string esc;
    esc.reserve(s.size() + 4U);
    for (const char ch : s) {
        if (ch == '|') {
            esc += '\\';
        }
        esc += ch;
    }
    return esc;
}

std::string emit_code_span(std::u32string_view text) {
    const std::string content = to_utf8(text);
    std::size_t max_run = 0U;
    std::size_t run = 0U;
    for (const char ch : content) {
        run = (ch == '`') ? run + 1U : 0U;
        max_run = (std::max)(max_run, run);
    }
    const std::string delim(max_run + 1U, '`');
    const bool pad = !content.empty() &&
                     (content.front() == '`' || content.back() == '`' ||
                      content.front() == ' ' || content.back() == ' ');
    std::string out;
    out.reserve(2U * delim.size() + content.size() + (pad ? 2U : 0U));
    out += delim;
    if (pad) {
        out += ' ';
    }
    out += content;
    if (pad) {
        out += ' ';
    }
    out += delim;
    return out;
}

std::string format_link_dest(std::string_view url, bool in_table) {
    std::string cleaned;
    cleaned.reserve(url.size() + 4U);
    bool has_space_or_ctrl = false;
    int paren_depth = 0;
    bool unbalanced_parens = false;

    for (const char ch : url) {
        const auto uch = static_cast<unsigned char>(ch);
        if (in_table && uch == '|') {
            cleaned += "%7C";
            continue;
        }
        if (uch == '\n') {
            cleaned += "%0A";
            continue;
        }
        if (uch == '\r') {
            cleaned += "%0D";
            continue;
        }
        if (uch <= kAsciiControlLimit || uch == kAsciiDel) {
            has_space_or_ctrl = true;
        } else if (uch == '(') {
            ++paren_depth;
            if (paren_depth > kMaxBareLinkParenDepth) {
                unbalanced_parens = true;
            }
        } else if (uch == ')') {
            if (paren_depth == 0) {
                unbalanced_parens = true;
            } else {
                --paren_depth;
            }
        }
        cleaned += ch;
    }
    if (paren_depth != 0) {
        unbalanced_parens = true;
    }

    const bool needs_angle = has_space_or_ctrl || unbalanced_parens ||
                             (!cleaned.empty() && cleaned.front() == '<');
    if (!needs_angle) {
        return cleaned;
    }

    std::string wrapped;
    wrapped.reserve(cleaned.size() + 2U);
    wrapped += '<';
    for (const char ch : cleaned) {
        if (ch == '<') {
            wrapped += "%3C";
        } else if (ch == '>') {
            wrapped += "%3E";
        } else {
            wrapped += ch;
        }
    }
    wrapped += '>';
    return wrapped;
}

struct EmitContext {
    bool strip_styles = false;
    bool allow_links = true;
    bool in_table = false;
    bool escape_rbracket = false;
};

std::string emit_run(const StyledRun& run, const EmitContext& ctx) {
    if (run.raw) {
        std::string raw_utf8 = to_utf8(run.text);
        return ctx.in_table ? escape_pipes(std::move(raw_utf8)) : raw_utf8;
    }

    const std::u32string_view t(run.text);
    const std::size_t b = t.find_first_not_of(U' ');
    if (b == std::u32string_view::npos) {
        return std::string(t.size(), ' ');
    }
    const std::size_t e = t.find_last_not_of(U' ');
    const std::u32string_view trimmed = t.substr(b, e - b + 1U);
    const std::string lead(b, ' ');
    const std::string trail(t.size() - 1U - e, ' ');

    if (run.math) {
        // run.text is LaTeX; wrap it in inline-math delimiters without markdown
        // escaping, keeping any surrounding spaces outside the `$...$`.
        const std::string_view mk = (!ctx.strip_styles && run.bold) ? "**" : "";
        const std::string_view strike = (!ctx.strip_styles && run.strikethrough) ? "~~" : "";
        std::string latex = to_utf8(trimmed);
        if (ctx.in_table) {
            latex = escape_pipes(std::move(latex));
        }
        std::string out;
        out.reserve(lead.size() + 2U * mk.size() + 2U * strike.size() + latex.size() + 2U +
                    trail.size());
        out += lead;
        out += mk;
        out += strike;
        out += '$';
        out += latex;
        out += '$';
        out += strike;
        out += mk;
        out += trail;
        return out;
    }

    const bool has_link = ctx.allow_links && !run.link_url.empty();

    if (!ctx.strip_styles && run.mono) {
        std::string inner = emit_code_span(trimmed);
        if (ctx.in_table) {
            inner = escape_pipes(std::move(inner));
        }
        if (run.strikethrough) {
            inner = "~~" + inner + "~~";
        }
        if (has_link) {
            const std::string dest = format_link_dest(run.link_url, ctx.in_table);
            return lead + "[" + inner + "](" + dest + ")" + trail;
        }
        return lead + inner + trail;
    }

    const std::string core = escape_md(trimmed, has_link || ctx.escape_rbracket);
    std::string_view marker;
    if (!ctx.strip_styles) {
        if (run.bold && run.italic) {
            marker = "***";
        } else if (run.bold) {
            marker = "**";
        } else if (run.italic) {
            marker = "*";
        }
    }
    const std::string_view strike = (!ctx.strip_styles && run.strikethrough) ? "~~" : "";

    std::string styled;
    styled.reserve(2U * marker.size() + 2U * strike.size() + core.size());
    styled += marker;
    styled += strike;
    styled += core;
    styled += strike;
    styled += marker;

    if (has_link) {
        const std::string dest = format_link_dest(run.link_url, ctx.in_table);
        std::string out;
        out.reserve(lead.size() + styled.size() + dest.size() + 4U + trail.size());
        out += lead;
        out += '[';
        out += styled;
        out += "](";
        out += dest;
        out += ')';
        out += trail;
        return out;
    }

    std::string out;
    out.reserve(lead.size() + styled.size() + trail.size());
    out += lead;
    out += styled;
    out += trail;
    return out;
}

std::string trim_ascii_spaces(std::string s) {
    const std::size_t b = s.find_first_not_of(' ');
    if (b == std::string::npos) {
        return {};
    }
    const std::size_t e = s.find_last_not_of(' ');
    return s.substr(b, e - b + 1U);
}

std::string inline_text(const Block& block, const EmitContext& ctx, const WordSet& vocab) {
    std::string out;
    for (const StyledRun& run : merge_block_runs(block, vocab)) {
        out += emit_run(run, ctx);
    }
    return trim_ascii_spaces(std::move(out));
}

constexpr bool is_slug_unicode_alnum(char32_t cp) noexcept {
    if (is_ascii_lower(cp) || is_ascii_upper(cp) || is_ascii_digit(cp)) {
        return true;
    }
    if (cp < 0x00C0U || cp == 0x00D7U || cp == 0x00F7U) {
        return false;
    }
    if ((cp >= 0x2000U && cp <= 0x2BFFU) || (cp >= 0x2E00U && cp <= 0x2E7FU) ||
        (cp >= 0x3000U && cp <= 0x303FU) || (cp >= 0xFE30U && cp <= 0xFE6FU)) {
        return false;
    }
    if ((cp >= 0xFF00U && cp <= 0xFF0FU) || (cp >= 0xFF1AU && cp <= 0xFF20U) ||
        (cp >= 0xFF3BU && cp <= 0xFF40U) || (cp >= 0xFF5BU && cp <= 0xFF65U)) {
        return false;
    }
    return cp <= 0x10FFFFU && (cp < 0xD800U || cp > 0xDFFFU);
}

std::string make_gfm_slug(std::u32string_view plain) {
    std::size_t b = 0U;
    while (b < plain.size() && (is_space_cp(plain[b]) || is_line_break_cp(plain[b]))) {
        ++b;
    }
    std::size_t e = plain.size();
    while (e > b && (is_space_cp(plain[e - 1U]) || is_line_break_cp(plain[e - 1U]))) {
        --e;
    }

    std::string slug;
    slug.reserve(e - b);
    for (std::size_t i = b; i < e; ++i) {
        const char32_t cp = plain[i];
        if (cp == kSoftHyphenCp) {
            continue;
        }
        if (is_ascii_upper(cp)) {
            slug += ascii_tolower(cp);
            continue;
        }
        if (is_space_cp(cp) || is_line_break_cp(cp)) {
            slug += '-';
            continue;
        }
        if (cp == U'-' || cp == U'_') {
            slug += static_cast<char>(cp);
            continue;
        }
        if (is_slug_unicode_alnum(cp)) {
            append_utf8(slug, cp);
        }
    }
    if (slug.empty()) {
        slug = "section";
    }
    return slug;
}

std::string build_toc(const Document& doc, const WordSet& vocab) {
    struct TocItem {
        int level = 1;
        std::string label;
        std::string slug;
    };

    std::vector<TocItem> items;
    std::unordered_map<std::string, std::size_t> slug_counts;
    std::unordered_set<std::string> used_slugs;
    int min_level = kMaxHeadingLevel;

    const EmitContext toc_label_ctx{
        /*strip_styles=*/true,
        /*allow_links=*/false,
        /*in_table=*/false,
        /*escape_rbracket=*/true,
    };

    for (const Block& block : doc.blocks) {
        if (block.kind != BlockKind::Heading) {
            continue;
        }
        const std::vector<StyledRun> merged = merge_block_runs(block, vocab);
        std::string label;
        std::u32string plain;
        for (const StyledRun& run : merged) {
            label += emit_run(run, toc_label_ctx);
            plain += run.text;
        }
        label = trim_ascii_spaces(std::move(label));
        if (label.empty()) {
            continue;
        }

        const int level = std::clamp(block.heading_level, kMinHeadingLevel, kMaxHeadingLevel);
        min_level = (std::min)(min_level, level);

        const std::string base_slug = make_gfm_slug(plain);
        std::size_t& seq = slug_counts[base_slug];
        std::string unique_slug = seq == 0U ? base_slug : base_slug + "-" + std::to_string(seq);
        ++seq;
        while (used_slugs.find(unique_slug) != used_slugs.end()) {
            unique_slug = base_slug + "-" + std::to_string(seq);
            ++seq;
        }
        used_slugs.insert(unique_slug);
        items.push_back(TocItem{level, std::move(label), std::move(unique_slug)});
    }

    if (items.empty()) {
        return {};
    }

    std::string out;
    for (std::size_t i = 0U; i < items.size(); ++i) {
        if (i > 0U) {
            out += '\n';
        }
        const auto indent_spaces =
            static_cast<std::size_t>(items[i].level - min_level) * kTocIndentSpaces;
        out.append(indent_spaces, ' ');
        out += "- [";
        out += items[i].label;
        out += "](#";
        out += items[i].slug;
        out += ')';
    }
    return out;
}

std::string render_code_block(const Block& block) {
    if (block.lines.empty()) {
        return {};
    }

    // Estimate the column width from horizontal advances between neighbours.
    std::vector<double> advances;
    for (const Line& ln : block.lines) {
        for (std::size_t i = 1U; i < ln.chars.size(); ++i) {
            const double dx = ln.chars[i].x - ln.chars[i - 1U].x;
            if (dx > kMinAdvancePt && dx < kMaxAdvanceEmRatio * ln.size) {
                advances.push_back(dx);
            }
        }
    }
    double cell = kDefaultMonoCellRatio * (std::max)(block.size, 1.0);
    if (!advances.empty()) {
        const std::size_t mid = advances.size() / 2U;
        std::nth_element(advances.begin(), advances.begin() + static_cast<std::ptrdiff_t>(mid),
                         advances.end());
        if (advances[mid] > kMinMedianCellPt) {
            cell = advances[mid];
        }
    }

    std::vector<std::string> rows;
    rows.reserve(block.lines.size());
    std::size_t max_backticks = 0U;
    std::u32string row_buf;

    const auto record_row = [&](std::string utf8_row) {
        std::size_t run = 0U;
        for (const char ch : utf8_row) {
            run = (ch == '`') ? run + 1U : 0U;
            max_backticks = (std::max)(max_backticks, run);
        }
        rows.push_back(std::move(utf8_row));
    };

    for (const Line& ln : block.lines) {
        if (!ln.chars.empty()) {
            row_buf.clear();
            for (const CharInfo& c : ln.chars) {
                if (c.space) {
                    continue;
                }
                const auto pos = static_cast<std::size_t>(
                    (std::max)(0L, std::lround((c.x - block.x0) / cell)));
                if (row_buf.size() < pos) {
                    row_buf.resize(pos, U' ');
                }
                row_buf += c.code;
            }
            record_row(to_utf8(row_buf));
            continue;
        }
        if (!ln.plain.empty()) {
            record_row(to_utf8(ln.plain));
            continue;
        }
        if (!ln.runs.empty()) {
            std::string fallback_row;
            for (const StyledRun& r : ln.runs) {
                fallback_row += to_utf8(r.text);
            }
            record_row(std::move(fallback_row));
            continue;
        }
        rows.emplace_back();
    }

    const std::size_t fence_len = (std::max)(kMinFenceBackticks, max_backticks + 1U);
    std::size_t total_len = 2U * fence_len + 1U;
    for (const std::string& r : rows) {
        total_len += r.size() + 1U;
    }

    const std::string fence(fence_len, '`');
    std::string out;
    out.reserve(total_len);
    out += fence;
    out += '\n';
    for (const std::string& row : rows) {
        out += row;
        out += '\n';
    }
    out += fence;
    return out;
}

// One cell's markdown: styled runs emitted and whitespace-normalized to a single
// line. The header row strips emphasis (markdown already bolds header cells).
std::string render_table_cell(const TableCell& cell, bool header, bool allow_links) {
    const EmitContext ctx{/*strip_styles=*/header, allow_links, /*in_table=*/true,
                          /*escape_rbracket=*/false};
    std::string out;
    if (cell.size() <= 1U) {
        for (const StyledRun& r : cell) {
            out += emit_run(r, ctx);
        }
    } else {
        std::vector<StyledRun> merged;
        merged.reserve(cell.size());
        for (const StyledRun& r : cell) {
            if (r.text.empty()) {
                continue;
            }
            if (!merged.empty() && same_mergeable_style(merged.back(), r)) {
                merged.back().text += r.text;
                continue;
            }
            merged.push_back(r);
        }
        for (const StyledRun& r : merged) {
            out += emit_run(r, ctx);
        }
    }

    std::string norm;
    norm.reserve(out.size());
    bool prev_space = false;
    for (const char ch : out) {
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
            if (!prev_space && !norm.empty()) {
                norm += ' ';
            }
            prev_space = true;
            continue;
        }
        norm += ch;
        prev_space = false;
    }
    const std::size_t e = norm.find_last_not_of(' ');
    return e == std::string::npos ? std::string{} : norm.substr(0U, e + 1U);
}

std::string render_table(const Block& block, bool allow_links) {
    if (block.table_cells.empty()) {
        return {};
    }
    std::size_t cols = 0U;
    for (const auto& row : block.table_cells) {
        cols = (std::max)(cols, row.size());
    }
    if (cols == 0U) {
        return {};
    }

    const auto append_row = [&](std::string& dst, const std::vector<TableCell>& row, bool header) {
        dst += '|';
        for (std::size_t c = 0U; c < cols; ++c) {
            dst += ' ';
            if (c < row.size()) {
                dst += render_table_cell(row[c], header, allow_links);
            }
            dst += " |";
        }
    };

    std::string out;
    append_row(out, block.table_cells[0], /*header=*/true);
    out += "\n|";
    for (std::size_t c = 0U; c < cols; ++c) {
        const TableAlign align =
            c < block.table_align.size() ? block.table_align[c] : TableAlign::None;
        switch (align) {
            case TableAlign::None:
                out += " --- |";
                break;
            case TableAlign::Left:
                out += " :--- |";
                break;
            case TableAlign::Center:
                out += " :---: |";
                break;
            case TableAlign::Right:
                out += " ---: |";
                break;
        }
    }
    for (std::size_t r = 1U; r < block.table_cells.size(); ++r) {
        out += '\n';
        append_row(out, block.table_cells[r], /*header=*/false);
    }
    return out;
}

std::string render_math_display(const Block& block) {
    if (block.math_lines.empty()) {
        return {};
    }

    std::string body;
    if (block.math_lines.size() == 1U) {
        body = to_utf8(block.math_lines[0]);
    } else {
        body = "\\begin{aligned}\n";
        for (std::size_t i = 0U; i < block.math_lines.size(); ++i) {
            body += to_utf8(block.math_lines[i]);
            body += (i + 1U < block.math_lines.size()) ? " \\\\\n" : "\n";
        }
        body += "\\end{aligned}";
    }
    if (!block.math_tag.empty()) {
        body += " \\tag{";
        body += to_utf8(block.math_tag);
        body += '}';
    }
    return "$$\n" + body + "\n$$";
}

std::string sanitize_image_alt(std::string_view alt) {
    if (alt.empty()) {
        return {};
    }
    std::string collapsed;
    collapsed.reserve(alt.size());
    bool pending_space = false;
    for (const char ch : alt) {
        const auto uch = static_cast<unsigned char>(ch);
        if (uch <= kAsciiControlLimit || uch == kAsciiDel) {
            if (!collapsed.empty()) {
                pending_space = true;
            }
            continue;
        }
        if (pending_space) {
            collapsed += ' ';
            pending_space = false;
        }
        collapsed += ch;
    }
    if (collapsed.empty()) {
        return {};
    }

    std::string escaped;
    escaped.reserve(collapsed.size() + 4U);
    std::size_t backslash_run = 0U;
    for (const char ch : collapsed) {
        if (ch == '\\') {
            ++backslash_run;
            escaped += ch;
            continue;
        }
        if ((ch == '[' || ch == ']') && (backslash_run % 2U == 0U)) {
            escaped += '\\';
        }
        backslash_run = 0U;
        escaped += ch;
    }
    if (backslash_run % 2U == 1U) {
        escaped += '\\';
    }
    return escaped;
}

std::string render_image(const Block& block) {
    const std::string alt = sanitize_image_alt(block.image_alt);
    const std::string& p = block.image_path;
    const bool needs_brackets = p.find_first_of(" ()") != std::string::npos;
    std::string out;
    out.reserve(alt.size() + p.size() + 8U);
    out += "![";
    out += alt;
    out += "](";
    if (needs_brackets) {
        out += '<';
        out += p;
        out += '>';
    } else {
        out += p;
    }
    out += ')';
    return out;
}

std::string yaml_quote(std::string_view s) {
    constexpr char kHexDigits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() + 2U);
    out += '"';
    for (const char ch : s) {
        const auto uch = static_cast<unsigned char>(ch);
        if (uch == '"' || uch == '\\') {
            out += '\\';
            out += ch;
            continue;
        }
        if (uch == '\n') {
            out += "\\n";
            continue;
        }
        if (uch == '\r') {
            out += "\\r";
            continue;
        }
        if (uch == '\t') {
            out += "\\t";
            continue;
        }
        if (uch < kAsciiControlLimit || uch == kAsciiDel) {
            out += "\\u00";
            out += kHexDigits[(uch >> kNibbleShift) & kNibbleMask];
            out += kHexDigits[uch & kNibbleMask];
            continue;
        }
        out += ch;
    }
    out += '"';
    return out;
}

std::vector<std::string_view> split_keywords(std::string_view raw) {
    std::vector<std::string_view> items;
    std::size_t pos = 0U;
    while (pos < raw.size()) {
        const std::size_t delim = raw.find_first_of(",;", pos);
        const std::string_view piece =
            delim == std::string_view::npos ? raw.substr(pos) : raw.substr(pos, delim - pos);
        std::size_t b = 0U;
        while (b < piece.size() &&
               (piece[b] == ' ' || piece[b] == '\t' || piece[b] == '\r' || piece[b] == '\n')) {
            ++b;
        }
        std::size_t e = piece.size();
        while (e > b && (piece[e - 1U] == ' ' || piece[e - 1U] == '\t' ||
                         piece[e - 1U] == '\r' || piece[e - 1U] == '\n')) {
            --e;
        }
        if (e > b) {
            items.push_back(piece.substr(b, e - b));
        }
        if (delim == std::string_view::npos) {
            break;
        }
        pos = delim + 1U;
    }
    return items;
}

std::string build_front_matter(const Document& doc, const WriteOptions& options) {
    std::string fm = "---\n";
    if (!doc.meta.title.empty()) {
        fm += "title: " + yaml_quote(doc.meta.title) + "\n";
    }
    if (!doc.meta.author.empty()) {
        fm += "author: " + yaml_quote(doc.meta.author) + "\n";
    }
    if (!doc.meta.subject.empty()) {
        fm += "subject: " + yaml_quote(doc.meta.subject) + "\n";
    }
    if (!doc.meta.keywords.empty()) {
        const std::vector<std::string_view> kw = split_keywords(doc.meta.keywords);
        if (!kw.empty()) {
            fm += "keywords: [";
            for (std::size_t i = 0U; i < kw.size(); ++i) {
                if (i > 0U) {
                    fm += ", ";
                }
                fm += yaml_quote(kw[i]);
            }
            fm += "]\n";
        }
    }
    if (!doc.meta.creation_date.empty()) {
        fm += "date: " + yaml_quote(doc.meta.creation_date) + "\n";
    }
    if (!doc.meta.mod_date.empty() && doc.meta.mod_date != doc.meta.creation_date) {
        fm += "modified: " + yaml_quote(doc.meta.mod_date) + "\n";
    }
    if (!options.source_name.empty()) {
        fm += "source: " + yaml_quote(options.source_name) + "\n";
    }
    fm += "pages: " + std::to_string(doc.meta.page_count) + "\n---";
    return fm;
}

// Builder for the document string: parts are accumulated in order, each
// flagged as tight (a single newline joins it to its predecessor) or loose (a
// blank line does). build() terminates a non-empty document with a trailing newline.
class MarkdownAssembler {
public:
    void reserve(std::size_t count) {
        parts_.reserve(count);
        tight_.reserve(count);
    }

    void add(std::string part, bool tight_to_previous = false) {
        parts_.push_back(std::move(part));
        tight_.push_back(tight_to_previous ? 1 : 0);
    }

    [[nodiscard]] std::string build() const {
        if (parts_.empty()) {
            return {};
        }
        std::size_t total = 1U;
        for (std::size_t i = 0U; i < parts_.size(); ++i) {
            if (i > 0U) {
                total += tight_[i] != 0 ? 1U : 2U;
            }
            total += parts_[i].size();
        }
        std::string out;
        out.reserve(total);
        for (std::size_t i = 0U; i < parts_.size(); ++i) {
            if (i > 0U) {
                out += tight_[i] != 0 ? "\n" : "\n\n";
            }
            out += parts_[i];
        }
        if (!out.empty()) {
            out += '\n';
        }
        return out;
    }

private:
    std::vector<std::string> parts_;
    std::vector<char> tight_;
};

// Per-conversion state driving front-matter, TOC, and BlockKind dispatch.
class MarkdownRenderer {
public:
    explicit MarkdownRenderer(const WriteOptions& options) : options_(options) {}

    [[nodiscard]] std::string run(const Document& doc) {
        doc_.reserve(doc.blocks.size() + (options_.front_matter ? 1U : 0U) +
                     (options_.toc ? 1U : 0U));

        if (options_.front_matter) {
            doc_.add(build_front_matter(doc, options_));
        }

        vocab_ = build_document_vocab(doc);

        if (options_.toc) {
            std::string toc = build_toc(doc, vocab_);
            if (!toc.empty()) {
                doc_.add(std::move(toc));
            }
        }

        prev_page_ = doc.blocks.empty() ? 0 : doc.blocks.front().page;

        for (const Block& block : doc.blocks) {
            if (options_.page_breaks && !first_ && block.page != prev_page_) {
                doc_.add("---");
                prev_kind_ = BlockKind::Image;
                has_top_level_list_ = false;
            }
            prev_page_ = block.page;

            std::string text;
            bool is_tight = false;
            if (!render_block(block, text, is_tight)) {
                continue;
            }
            doc_.add(std::move(text), is_tight);
            prev_kind_ = block.kind;
            if (block.kind != BlockKind::ListItem) {
                has_top_level_list_ = false;
            }
            first_ = false;
        }

        return doc_.build();
    }

private:
    bool render_block(const Block& block, std::string& text, bool& is_tight) {
        switch (block.kind) {
            case BlockKind::Heading: {
                const EmitContext ctx{/*strip_styles=*/true, options_.links,
                                      /*in_table=*/false, /*escape_rbracket=*/false};
                std::string content = inline_text(block, ctx, vocab_);
                if (content.empty()) {
                    return false;
                }
                // A trailing '#' run would be eaten as an ATX closing sequence.
                if (content.back() == '#') {
                    const std::size_t last_non_hash = content.find_last_not_of('#');
                    const std::size_t run_start =
                        last_non_hash == std::string::npos ? 0U : last_non_hash + 1U;
                    content.insert(run_start, "\\");
                }
                const auto level = static_cast<std::size_t>(
                    std::clamp(block.heading_level, kMinHeadingLevel, kMaxHeadingLevel));
                text.assign(level, '#');
                text += ' ';
                text += content;
                break;
            }
            case BlockKind::Paragraph: {
                const EmitContext ctx{/*strip_styles=*/false, options_.links,
                                      /*in_table=*/false, /*escape_rbracket=*/false};
                text = inline_text(block, ctx, vocab_);
                if (text.empty()) {
                    return false;
                }
                if (block.is_footnote) {
                    if (block.footnote_numbered) {
                        text = "[^" + to_utf8(block.footnote_label) + "]: " + text;
                    } else {
                        text = to_utf8(block.footnote_label) + text;
                    }
                    break;
                }
                escape_line_start(text);
                if (block.blockquote) {
                    text = "> " + text;
                }
                break;
            }
            case BlockKind::ListItem: {
                const EmitContext ctx{/*strip_styles=*/false, options_.links,
                                      /*in_table=*/false, /*escape_rbracket=*/false};
                std::string content = inline_text(block, ctx, vocab_);
                if (content.empty()) {
                    return false;
                }
                escape_line_start(content);
                const auto indent_level =
                    static_cast<std::size_t>((std::max)(block.list_indent, 0));
                const std::string indent(indent_level * kListIndentSpaces, ' ');
                std::string prefix;
                if (block.task_state == TaskState::Unchecked) {
                    prefix = "- [ ] ";
                } else if (block.task_state == TaskState::Checked) {
                    prefix = "- [x] ";
                } else {
                    const std::string marker = block.list_marker.empty()
                                                   ? (block.list_ordered ? "1." : "-")
                                                   : to_utf8(block.list_marker);
                    prefix = marker + " ";
                }
                text = indent + prefix + content;

                const bool effective_ordered =
                    (block.task_state == TaskState::None) && block.list_ordered;
                const bool switched_top_level_list =
                    prev_kind_ == BlockKind::ListItem && indent_level == 0U &&
                    has_top_level_list_ && top_level_list_ordered_ != effective_ordered;
                is_tight = prev_kind_ == BlockKind::ListItem && !switched_top_level_list;
                if (indent_level == 0U || !has_top_level_list_) {
                    top_level_list_ordered_ = effective_ordered;
                    has_top_level_list_ = true;
                }
                break;
            }
            case BlockKind::Code:
                text = render_code_block(block);
                break;
            case BlockKind::Table:
                text = render_table(block, options_.links);
                break;
            case BlockKind::Image:
                text = render_image(block);
                break;
            case BlockKind::MathDisplay:
                text = render_math_display(block);
                break;
            case BlockKind::HorizontalRule:
                text = "---";
                break;
        }
        return !text.empty();
    }

    const WriteOptions& options_;
    WordSet vocab_;
    int prev_page_ = 0;
    BlockKind prev_kind_ = BlockKind::Image;
    bool has_top_level_list_ = false;
    bool top_level_list_ordered_ = false;
    bool first_ = true;
    MarkdownAssembler doc_;
};

}  // namespace

static std::string run_write_markdown(const Document& doc, const WriteOptions& options) {
    return MarkdownRenderer(options).run(doc);
}

}  // namespace pdf2md::markdown

namespace pdf2md {

struct MarkdownWriter::Impl {
    WriteOptions options;

    explicit Impl(WriteOptions opts) : options(std::move(opts)) {}
};

MarkdownWriter::MarkdownWriter(WriteOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

MarkdownWriter::MarkdownWriter(const MarkdownWriter& other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

MarkdownWriter& MarkdownWriter::operator=(const MarkdownWriter& other) {
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

MarkdownWriter::MarkdownWriter(MarkdownWriter&& other) noexcept = default;

MarkdownWriter& MarkdownWriter::operator=(MarkdownWriter&& other) noexcept = default;

MarkdownWriter::~MarkdownWriter() = default;

std::string MarkdownWriter::write(const Document& doc) const {
    if (!impl_) {
        const WriteOptions default_options{};
        return markdown::run_write_markdown(doc, default_options);
    }
    return markdown::run_write_markdown(doc, impl_->options);
}

std::string write_markdown(const Document& doc, const WriteOptions& options) {
    return MarkdownWriter(options).write(doc);
}

}  // namespace pdf2md
