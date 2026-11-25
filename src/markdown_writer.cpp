#include "markdown_writer.h"

#include "utf8.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pdf2md {

namespace {

constexpr char32_t kSoftHyphen = 0x00ADU;
constexpr char32_t kUnicodeHyphen = 0x2010U;

constexpr int kMinHeadingLevel = 1;
constexpr int kMaxHeadingLevel = 6;
constexpr std::size_t kListIndentSpaces = 4U;
constexpr std::size_t kMinFenceBackticks = 3U;
constexpr std::size_t kMaxOrderedListDigits = 9U;
constexpr std::size_t kMinCompoundPartLen = 4U;
constexpr std::size_t kMaxUrlSchemeLen = 8U;
constexpr std::size_t kDemoteMonoMaxGlyphs = 2U;

constexpr double kMinAdvancePt = 0.05;
constexpr double kMaxAdvanceEmRatio = 1.5;
constexpr double kDefaultMonoCellRatio = 0.6;
constexpr double kMinMedianCellPt = 0.1;

constexpr std::array<std::u32string_view, 18> kBoundPrefixes = {
    U"anti",  U"counter", U"hyper", U"hypo",  U"inter", U"intra",
    U"macro", U"micro",   U"mono",  U"multi", U"poly",  U"proto",
    U"pseudo", U"quasi",  U"semi",  U"super", U"trans", U"ultra",
};
static_assert(std::is_sorted(kBoundPrefixes.begin(), kBoundPrefixes.end()));

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
        // ']' stays unescaped: escaping both brackets yields "\[...\]", which
        // math-enabled renderers (GitHub, MathJax) read as a display-math
        // delimiter and pull onto its own centered line. A bare ']' is inert
        // because every emitted '[' is escaped, so it can never close a link.
        default:
            return false;
    }
}

std::string escape_md(std::u32string_view text) {
    std::string out;
    out.reserve(text.size() + 8U);
    for (const char32_t cp : text) {
        if (cp == kSoftHyphen) {
            continue;
        }
        if (always_escaped(cp)) {
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

std::string emit_run(const StyledRun& run, bool strip_styles) {
    if (run.raw) {
        return to_utf8(run.text);  // literal markdown (e.g. a "[^5]" ref)
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
        // escaping, keeping any surrounding spaces outside the `$...$`. A bold
        // math run (a highlighted table cell) also takes emphasis markers.
        const std::string_view mk = (!strip_styles && run.bold) ? "**" : "";
        std::string out;
        const std::string latex = to_utf8(trimmed);
        out.reserve(lead.size() + 2U * mk.size() + latex.size() + 2U + trail.size());
        out += lead;
        out += mk;
        out += '$';
        out += latex;
        out += '$';
        out += mk;
        out += trail;
        return out;
    }

    if (!strip_styles && run.mono) {
        // Trim spaces around inline code; markdown treats them specially.
        return lead + emit_code_span(trimmed) + trail;
    }

    const std::string core = escape_md(trimmed);
    std::string_view marker;
    if (!strip_styles) {
        if (run.bold && run.italic) {
            marker = "***";
        } else if (run.bold) {
            marker = "**";
        } else if (run.italic) {
            marker = "*";
        }
    }
    std::string out;
    out.reserve(lead.size() + 2U * marker.size() + core.size() + trail.size());
    out += lead;
    out += marker;
    out += core;
    out += marker;
    out += trail;
    return out;
}

using WordSet = std::unordered_set<std::u32string>;

// Lowercase the ASCII letters of a token (the document vocabulary is folded).
std::u32string lower_ascii(std::u32string_view s) {
    std::u32string out(s);
    for (char32_t& c : out) {
        if (is_ascii_upper(c)) {
            c = c - U'A' + U'a';
        }
    }
    return out;
}

// The set of whole words the document uses somewhere *other* than at a line
// edge. A token flanked by other tokens on its own line is unambiguously a
// complete word: a hyphenation fragment can only ever surface at a line's start
// (the continuation) or end (before the wrap hyphen), never in the interior.
// This attested-word set lets the joiner tell a real hyphenated compound from a
// single word that merely wrapped at a syllable break.
WordSet build_document_vocab(const Document& doc) {
    WordSet vocab;
    std::vector<std::u32string> toks;
    std::u32string cur;
    for (const Block& b : doc.blocks) {
        for (const Line& ln : b.lines) {
            toks.clear();
            cur.clear();
            for (const char32_t cp : ln.plain) {
                if (is_ascii_alpha(cp)) {
                    cur += is_ascii_upper(cp) ? static_cast<char32_t>(cp - U'A' + U'a') : cp;
                    continue;
                }
                if (!cur.empty()) {
                    toks.push_back(std::move(cur));
                    cur.clear();
                }
            }
            if (!cur.empty()) {
                toks.push_back(std::move(cur));
            }
            // Interior tokens only (skip the first and last of the line).
            for (std::size_t i = 1U; i + 1U < toks.size(); ++i) {
                vocab.insert(toks[i]);
            }
        }
    }
    return vocab;
}

// A line wrapped at a hyphen: decide whether it is a genuine hyphenated compound
// (keep the hyphen, e.g. "sequence-aligned", "position-wise") rather than a
// single word split for line breaking (drop it, e.g. "transduc-tion").
bool compound_hyphen(std::u32string_view prev, std::u32string_view cont, const WordSet& vocab) {
    if (prev.empty()) {
        return false;
    }
    const std::size_t e = prev.size() - 1U;  // step over the trailing hyphen glyph
    std::size_t l = e;
    while (l > 0U && is_ascii_alpha(prev[l - 1U])) {
        --l;
    }
    const std::u32string left = lower_ascii(prev.substr(l, e - l));
    std::size_t r = 0U;
    while (r < cont.size() && is_ascii_alpha(cont[r])) {
        ++r;
    }
    const std::u32string right = lower_ascii(cont.substr(0U, r));

    // The continuation itself opens a hyphenated compound ("to-German"): the wrap
    // fell inside a multi-part compound ("English-" / "to-German").
    const bool cont_compound = r > 0U && r + 1U < cont.size() &&
                               (cont[r] == U'-' || cont[r] == kUnicodeHyphen) &&
                               is_ascii_alpha(cont[r + 1U]);
    if (left.size() < kMinCompoundPartLen ||
        (right.size() < kMinCompoundPartLen && !cont_compound)) {
        return false;
    }
    if (vocab.find(left) == vocab.end()) {
        return false;
    }
    // Guard against a single word that wrapped at a syllable boundary whose
    // prefix happens to be a real word ("special"+"ization" = "specialization").
    if (vocab.find(left + right) != vocab.end()) {
        return false;
    }
    // Bound Latin/Greek prefixes attach to their stem without a hyphen.
    if (std::binary_search(kBoundPrefixes.begin(), kBoundPrefixes.end(),
                           std::u32string_view{left})) {
        return false;
    }
    return true;
}

// A wrap hyphen whose trailing token shows literal-hyphen evidence: the wrap
// sits inside an unclosed brace-delimited placeholder ("{<routine-interface-
// name-" continuing "x>}"), or the token already carries an interior hyphen.
bool literal_wrap_hyphen(std::u32string_view prev, std::u32string_view cont,
                         const WordSet& vocab) {
    std::size_t e = prev.size();
    while (e > 0U && (prev[e - 1U] == U'-' || prev[e - 1U] == kUnicodeHyphen ||
                      prev[e - 1U] == kSoftHyphen)) {
        --e;
    }
    std::size_t b = e;
    while (b > 0U && prev[b - 1U] != U' ') {
        --b;
    }
    if (b == e) {
        return false;
    }
    const std::u32string_view body = prev.substr(b, e - b);
    int depth = 0;
    std::size_t frag = std::u32string_view::npos;
    for (std::size_t i = 0U; i < body.size(); ++i) {
        const char32_t c = body[i];
        if (c == U'{') {
            ++depth;
        } else if (c == U'}') {
            --depth;
        } else if (c == U'-' || c == kUnicodeHyphen) {
            frag = i + 1U;
        }
    }
    if (depth > 0) {
        return true;
    }
    if (frag == std::u32string_view::npos) {
        return false;
    }
    for (std::size_t i = frag; i < body.size(); ++i) {
        if (!is_ascii_alpha(body[i]) && !is_ascii_digit(body[i])) {
            return false;
        }
    }
    const std::u32string left = lower_ascii(body.substr(frag));
    std::size_t r = 0U;
    while (r < cont.size() && is_ascii_alpha(cont[r])) {
        ++r;
    }
    const std::u32string right = lower_ascii(cont.substr(0U, r));
    if (!left.empty() && !right.empty() && vocab.find(left + right) != vocab.end()) {
        return false;
    }
    return true;
}

// True when a token begins a new self-contained URL: a "www." host, or a short
// scheme-like alpha prefix immediately followed by "://".
constexpr bool starts_new_url(std::u32string_view tok) noexcept {
    if (tok.size() >= 4U && tok.substr(0U, 4U) == U"www.") {
        return true;
    }
    const std::size_t s = tok.find(U"://");
    if (s == std::u32string_view::npos || s == 0U || s > kMaxUrlSchemeLen) {
        return false;
    }
    for (std::size_t i = 0U; i < s; ++i) {
        if (!is_ascii_alpha(tok[i])) {
            return false;
        }
    }
    return true;
}

// True when the accumulated text ends inside a URL that the line wrap split.
constexpr bool url_wrap(std::u32string_view prev, std::u32string_view next) noexcept {
    const char32_t first = next.empty() ? U'\0' : next.front();
    std::size_t e = prev.size();
    while (e > 0U && prev[e - 1U] == U' ') {
        --e;
    }
    std::size_t b = e;
    while (b > 0U && prev[b - 1U] != U' ') {
        --b;
    }
    if (e == b) {
        return false;
    }
    const std::u32string_view tok = prev.substr(b, e - b);
    const bool is_url = tok.find(U"://") != std::u32string_view::npos ||
                        (tok.size() >= 4U && tok.substr(0U, 4U) == U"www.");
    if (!is_url || starts_new_url(next)) {
        return false;
    }
    const char32_t last = prev[e - 1U];
    if (last == U'.' || last == U'!' || last == U'?' || last == U',' ||
        last == U':' || last == U';' || last == U')') {
        return false;
    }
    const bool sep = last == U'/' || last == U'-' || last == U'_' || last == U'.' ||
                     last == U'=' || last == U'?' || last == U'&' || last == U'#' ||
                     last == U'%' || last == U'+' || last == U'~';
    if (is_ascii_upper(first) && !sep) {
        return false;
    }
    return is_ascii_alpha(first) || is_ascii_digit(first) || first == U'/' ||
           first == U'.' || first == U'-' || first == U'_' || first == U'~' ||
           first == U'#' || first == U'?' || first == U'=' || first == U'&' ||
           first == U'%' || first == U'+';
}

constexpr bool same_mergeable_style(const StyledRun& a, const StyledRun& b) noexcept {
    return !a.raw && !b.raw && a.bold == b.bold && a.italic == b.italic &&
           a.mono == b.mono && a.math == b.math;
}

// Joins the lines of a block into one run sequence, applying hyphenation
// repair at line boundaries and merging adjacent same-style runs.
std::vector<StyledRun> merge_block_runs(const Block& block, const WordSet& vocab) {
    std::vector<StyledRun> merged;

    const auto append_run = [&](const StyledRun& run) {
        if (run.text.empty()) {
            return;
        }
        if (!merged.empty() && same_mergeable_style(merged.back(), run)) {
            merged.back().text += run.text;
            return;
        }
        merged.push_back(run);
    };

    for (const Line& line : block.lines) {
        if (line.runs.empty()) {
            continue;
        }
        if (!merged.empty()) {
            std::u32string& prev = merged.back().text;
            const char32_t first = line.plain.empty() ? U'\0' : line.plain.front();
            bool joined = false;
            while (!prev.empty() && prev.back() == U' ') {
                prev.pop_back();
            }
            if (!prev.empty()) {
                const char32_t last = prev.back();
                const bool lower_wrap =
                    last == kSoftHyphen ||
                    ((last == U'-' || last == kUnicodeHyphen) && is_ascii_lower(first));
                if (lower_wrap) {
                    if (compound_hyphen(prev, line.plain, vocab) ||
                        literal_wrap_hyphen(prev, line.plain, vocab)) {
                        if (last == kSoftHyphen || last == kUnicodeHyphen) {
                            prev.pop_back();
                            prev += U'-';
                        }
                    } else {
                        prev.pop_back();
                    }
                    joined = true;
                } else if ((last == U'-' || last == kUnicodeHyphen) && is_ascii_upper(first)) {
                    if (last == kUnicodeHyphen) {
                        prev.pop_back();
                        prev += U'-';
                    }
                    joined = true;
                } else if (url_wrap(prev, line.plain)) {
                    joined = true;
                }
            }
            if (!joined) {
                prev += U' ';
            }
            if (merged.back().text.empty()) {
                merged.pop_back();
            }
        }
        for (const StyledRun& run : line.runs) {
            append_run(run);
        }
    }

    // Remove remaining soft hyphens in place without reallocating clean runs.
    for (StyledRun& run : merged) {
        std::erase(run.text, kSoftHyphen);
    }
    std::erase_if(merged, [](const StyledRun& r) { return r.text.empty(); });

    // A stray fixed-pitch glyph (punctuation, a digit) inside prose is font
    // noise, not inline code; demote letter-free mono fragments and re-merge.
    for (StyledRun& run : merged) {
        if (!run.mono) {
            continue;
        }
        std::size_t glyphs = 0U;
        bool has_letter = false;
        for (const char32_t cp : run.text) {
            if (cp == U' ') {
                continue;
            }
            ++glyphs;
            if (is_ascii_alpha(cp) || cp >= detail::kTwoByteMin) {
                has_letter = true;
            }
        }
        if (glyphs <= kDemoteMonoMaxGlyphs && !has_letter) {
            run.mono = false;
        }
    }

    std::vector<StyledRun> repacked;
    repacked.reserve(merged.size());
    for (StyledRun& run : merged) {
        if (!repacked.empty() && same_mergeable_style(repacked.back(), run)) {
            repacked.back().text += run.text;
            continue;
        }
        repacked.push_back(std::move(run));
    }
    return repacked;
}

std::string inline_text(const Block& block, bool strip_styles, const WordSet& vocab) {
    std::string out;
    for (const StyledRun& run : merge_block_runs(block, vocab)) {
        out += emit_run(run, strip_styles);
    }
    const std::size_t b = out.find_first_not_of(' ');
    if (b == std::string::npos) {
        return {};
    }
    const std::size_t e = out.find_last_not_of(' ');
    return out.substr(b, e - b + 1U);
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
std::string render_table_cell(const TableCell& cell, bool header) {
    std::string out;
    for (const StyledRun& r : cell) {
        std::string piece = emit_run(r, /*strip_styles=*/header);
        // Math, code-span and raw runs bypass escape_md, but GFM splits a row
        // at any unescaped '|' -- even inside `...` or $...$ -- so escape pipes.
        if ((r.math || r.mono || r.raw) && piece.find('|') != std::string::npos) {
            std::string esc;
            esc.reserve(piece.size() + 4U);
            for (const char ch : piece) {
                if (ch == '|') {
                    esc += '\\';
                }
                esc += ch;
            }
            piece = std::move(esc);
        }
        out += piece;
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

std::string render_table(const Block& block) {
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
                dst += render_table_cell(row[c], header);
            }
            dst += " |";
        }
    };

    std::string out;
    append_row(out, block.table_cells[0], /*header=*/true);
    out += "\n|";
    for (std::size_t c = 0U; c < cols; ++c) {
        out += " --- |";
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

std::string render_image(const Block& block) {
    const std::string& p = block.image_path;
    const bool needs_brackets = p.find_first_of(" ()") != std::string::npos;
    std::string out;
    out.reserve(p.size() + 8U);
    out += "![](";
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
    std::string out;
    out.reserve(s.size() + 2U);
    out += '"';
    for (const char ch : s) {
        if (ch == '"' || ch == '\\') {
            out += '\\';
            out += ch;
            continue;
        }
        if (ch == '\n') {
            out += "\\n";
            continue;
        }
        if (ch == '\r') {
            out += "\\r";
            continue;
        }
        if (ch == '\t') {
            out += "\\t";
            continue;
        }
        out += ch;
    }
    out += '"';
    return out;
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

// Per-conversion state driving front-matter emission and BlockKind dispatch.
class MarkdownWriter {
public:
    explicit MarkdownWriter(const WriteOptions& options) : options_(options) {}

    [[nodiscard]] std::string run(const Document& doc) {
        doc_.reserve(doc.blocks.size() + (options_.front_matter ? 1U : 0U));

        if (options_.front_matter) {
            std::string fm = "---\n";
            if (!doc.meta.title.empty()) {
                fm += "title: " + yaml_quote(doc.meta.title) + "\n";
            }
            if (!doc.meta.author.empty()) {
                fm += "author: " + yaml_quote(doc.meta.author) + "\n";
            }
            if (!options_.source_name.empty()) {
                fm += "source: " + yaml_quote(options_.source_name) + "\n";
            }
            fm += "pages: " + std::to_string(doc.meta.page_count) + "\n---";
            doc_.add(std::move(fm));
        }

        vocab_ = build_document_vocab(doc);
        prev_page_ = doc.blocks.empty() ? 0 : doc.blocks.front().page;

        for (const Block& block : doc.blocks) {
            if (options_.page_breaks && !first_ && block.page != prev_page_) {
                doc_.add("---");
                prev_kind_ = BlockKind::Image;
            }
            prev_page_ = block.page;

            std::string text;
            bool is_tight = false;
            if (!render_block(block, text, is_tight)) {
                continue;
            }
            doc_.add(std::move(text), is_tight);
            prev_kind_ = block.kind;
            first_ = false;
        }

        return doc_.build();
    }

private:
    bool render_block(const Block& block, std::string& text, bool& is_tight) const {
        switch (block.kind) {
            case BlockKind::Heading: {
                std::string content = inline_text(block, /*strip_styles=*/true, vocab_);
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
                text = inline_text(block, /*strip_styles=*/false, vocab_);
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
                std::string content = inline_text(block, /*strip_styles=*/false, vocab_);
                if (content.empty()) {
                    return false;
                }
                escape_line_start(content);
                const auto indent_level =
                    static_cast<std::size_t>((std::max)(block.list_indent, 0));
                const std::string indent(indent_level * kListIndentSpaces, ' ');
                const std::string marker = block.list_marker.empty()
                                               ? (block.list_ordered ? "1." : "-")
                                               : to_utf8(block.list_marker);
                text = indent + marker + " " + content;
                is_tight = prev_kind_ == BlockKind::ListItem;
                break;
            }
            case BlockKind::Code:
                text = render_code_block(block);
                break;
            case BlockKind::Table:
                text = render_table(block);
                break;
            case BlockKind::Image:
                text = render_image(block);
                break;
            case BlockKind::MathDisplay:
                text = render_math_display(block);
                break;
        }
        return !text.empty();
    }

    const WriteOptions& options_;
    WordSet vocab_;
    int prev_page_ = 0;
    BlockKind prev_kind_ = BlockKind::Image;
    bool first_ = true;
    MarkdownAssembler doc_;
};

}  // namespace

std::string write_markdown(const Document& doc, const WriteOptions& options) {
    return MarkdownWriter(options).run(doc);
}

}  // namespace pdf2md
