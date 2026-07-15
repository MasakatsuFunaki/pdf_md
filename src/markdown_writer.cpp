#include "markdown_writer.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>
#include <vector>

#include "utf8.h"

namespace pdf2md {

namespace {

constexpr char32_t kSoftHyphen = 0x00AD;

bool always_escaped(char32_t cp) {
    switch (cp) {
        case U'\\': case U'`': case U'*': case U'_': case U'[':
        case U'<': case U'|': case U'~': case U'$':  // $ now delimits inline math
            return true;
        // ']' stays unescaped: escaping both brackets yields "\[…\]", which
        // math-enabled renderers (GitHub, MathJax) read as a display-math
        // delimiter and pull onto its own centered line. A bare ']' is inert
        // because every emitted '[' is escaped, so it can never close a link.
        default:
            return false;
    }
}

std::string escape_md(std::u32string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char32_t cp : text) {
        if (cp == kSoftHyphen) continue;
        if (always_escaped(cp)) out += '\\';
        append_utf8(out, cp);
    }
    return out;
}

// Escapes block-level markers that only matter at the start of an output line.
void escape_line_start(std::string& s) {
    if (s.empty()) return;
    if (s[0] == '#' || s[0] == '>' || s[0] == '+' || s[0] == '-') {
        size_t run = 1;
        if (s[0] == '#')
            while (run < s.size() && s[run] == '#') ++run;
        if (run >= s.size() || s[run] == ' ' || s[0] != '#') {
            s.insert(0, "\\");
            return;
        }
    }
    size_t i = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    if (i > 0 && i <= 9 && i < s.size() && (s[i] == '.' || s[i] == ')') &&
        (i + 1 == s.size() || s[i + 1] == ' ')) {
        s.insert(i, "\\");
    }
}

std::string emit_code_span(const std::u32string& text) {
    std::string content = to_utf8(text);
    size_t max_run = 0, run = 0;
    for (char ch : content) {
        run = (ch == '`') ? run + 1 : 0;
        max_run = std::max(max_run, run);
    }
    std::string delim(max_run + 1, '`');
    bool pad = !content.empty() &&
               (content.front() == '`' || content.back() == '`' ||
                content.front() == ' ' || content.back() == ' ');
    return delim + (pad ? " " : "") + content + (pad ? " " : "") + delim;
}

std::string emit_run(const StyledRun& run, bool strip_styles) {
    if (run.raw) return to_utf8(run.text);  // literal markdown (e.g. a "[^5]" ref)

    if (run.math) {
        // run.text is LaTeX; wrap it in inline-math delimiters without markdown
        // escaping, keeping any surrounding spaces outside the `$...$`. A bold
        // math run (a highlighted table cell) also takes emphasis markers so the
        // cell reads bold; body math never sets run.bold, so this is table-only.
        const std::u32string& t = run.text;
        size_t b = t.find_first_not_of(U' ');
        if (b == std::u32string::npos) return std::string(t.size(), ' ');
        size_t e = t.find_last_not_of(U' ');
        std::string lead(b, ' ');
        std::string trail(t.size() - 1 - e, ' ');
        const char* mk = (!strip_styles && run.bold) ? "**" : "";
        return lead + mk + "$" + to_utf8(t.substr(b, e - b + 1)) + "$" + mk + trail;
    }

    if (!strip_styles && run.mono) {
        // Trim spaces around inline code; markdown treats them specially.
        std::u32string t = run.text;
        size_t b = t.find_first_not_of(U' ');
        size_t e = t.find_last_not_of(U' ');
        if (b == std::u32string::npos) return std::string(t.size(), ' ');
        std::string lead(b, ' ');
        std::string trail(t.size() - 1 - e, ' ');
        return lead + emit_code_span(t.substr(b, e - b + 1)) + trail;
    }

    const std::u32string& t = run.text;
    size_t b = t.find_first_not_of(U' ');
    if (b == std::u32string::npos) return std::string(t.size(), ' ');
    size_t e = t.find_last_not_of(U' ');
    std::string lead(b, ' ');
    std::string trail(t.size() - 1 - e, ' ');
    std::string core = escape_md(std::u32string_view(t).substr(b, e - b + 1));

    const char* marker = "";
    if (!strip_styles) {
        if (run.bold && run.italic) marker = "***";
        else if (run.bold) marker = "**";
        else if (run.italic) marker = "*";
    }
    return lead + marker + core + marker + trail;
}

using WordSet = std::unordered_set<std::u32string>;

// Lowercase the ASCII letters of a token (the document vocabulary is folded).
std::u32string lower_ascii(std::u32string s) {
    for (char32_t& c : s)
        if (c >= U'A' && c <= U'Z') c = c - U'A' + U'a';
    return s;
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
    for (const Block& b : doc.blocks) {
        for (const Line& ln : b.lines) {
            toks.clear();
            std::u32string cur;
            for (char32_t cp : ln.plain) {
                if (is_ascii_alpha(cp)) {
                    cur += (cp >= U'A' && cp <= U'Z') ? char32_t(cp - U'A' + U'a') : cp;
                } else if (!cur.empty()) {
                    toks.push_back(std::move(cur));
                    cur.clear();
                }
            }
            if (!cur.empty()) toks.push_back(std::move(cur));
            // Interior tokens only (skip the first and last of the line).
            for (size_t i = 1; i + 1 < toks.size(); ++i) vocab.insert(toks[i]);
        }
    }
    return vocab;
}

// A line wrapped at a hyphen: decide whether it is a genuine hyphenated compound
// (keep the hyphen, e.g. "sequence-aligned", "position-wise") rather than a
// single word split for line breaking (drop it, e.g. "transduc-tion"). It is a
// compound when the text before the hyphen is a complete word the document uses
// elsewhere and the continuation begins with an independently word-length token.
// `prev` still ends with the wrap-hyphen glyph; `cont` is the continuation line.
bool compound_hyphen(const std::u32string& prev, const std::u32string& cont,
                     const WordSet& vocab) {
    if (prev.empty()) return false;
    size_t e = prev.size() - 1;  // step over the trailing hyphen glyph
    size_t l = e;
    while (l > 0 && is_ascii_alpha(prev[l - 1])) --l;
    std::u32string left = lower_ascii(prev.substr(l, e - l));
    size_t r = 0;
    while (r < cont.size() && is_ascii_alpha(cont[r])) ++r;
    std::u32string right = lower_ascii(cont.substr(0, r));
    // The continuation itself opens a hyphenated compound ("to-German"): the wrap
    // fell inside a multi-part compound ("English-" / "to-German"), so its joining
    // hyphen is a real one to keep even though the first segment is short.
    bool cont_compound = r > 0 && r < cont.size() && r + 1 < cont.size() &&
                         (cont[r] == U'-' || cont[r] == 0x2010) && is_ascii_alpha(cont[r + 1]);
    // Both halves must be word-length, and the pre-hyphen half must be an
    // attested whole word of the document (a fragment like "transduc"/"convolu"
    // is not, so its wrap hyphen is dropped). A short continuation is allowed only
    // when it heads a further hyphenated compound.
    if (left.size() < 4 || (right.size() < 4 && !cont_compound)) return false;
    if (vocab.count(left) == 0) return false;
    // Guard against a single word that merely wrapped at a syllable boundary
    // whose prefix happens to be a real word ("inter"+"preter" = "interpreter",
    // "special"+"ization" = "specialization"): if the joined form is itself an
    // attested word of the document, it is one word, not a compound -- drop it.
    if (vocab.count(left + right) > 0) return false;
    // A bound Latin/Greek prefix attaches to its stem without a hyphen, so a
    // wrap after one is intra-word ("multi"+"plicative" = "multiplicative",
    // "inter"+"face" = "interface") even when the single joined word never
    // recurs to attest it. These prefixes are not free-standing compound heads,
    // unlike "self"/"cross"/"sequence"/"source" which are left out on purpose.
    static const WordSet kBoundPrefixes = {
        U"multi", U"inter", U"intra", U"trans", U"super", U"pseudo", U"quasi",
        U"semi",  U"anti",  U"counter", U"hyper", U"hypo", U"poly", U"mono",
        U"proto", U"ultra", U"micro", U"macro"};
    if (kBoundPrefixes.count(left) > 0) return false;
    return true;
}

// A wrap hyphen whose trailing token shows literal-hyphen evidence: the wrap
// sits inside an unclosed brace-delimited placeholder ("{<routine-interface-
// name-" continuing "x>}"), or the token already carries an interior hyphen (a
// compound identifier such as "data-element-interface-name-"). Only an OPEN
// brace group is placeholder evidence -- a closed one followed by a plain
// suffix ("{<...>}ReadOut-" + "put") wraps at an ordinary syllable; likewise
// scope/path glue (`::`, `/`) and square-bracket refs get no evidence. A
// compound that merely wrapped at a syllable inside its last element
// ("self-atten-" + "tion") is still a soft break: when the trailing letter run
// joins the continuation into an attested document word, the hyphen drops.
// `prev` still ends with the wrap-hyphen glyph.
bool literal_wrap_hyphen(const std::u32string& prev, const std::u32string& cont,
                         const WordSet& vocab) {
    size_t e = prev.size();
    while (e > 0 && (prev[e - 1] == U'-' || prev[e - 1] == 0x2010 || prev[e - 1] == kSoftHyphen))
        --e;
    size_t b = e;
    while (b > 0 && prev[b - 1] != U' ') --b;
    if (b == e) return false;
    std::u32string body = prev.substr(b, e - b);
    int depth = 0;
    size_t frag = std::u32string::npos;  // start of the fragment after the last interior hyphen
    for (size_t i = 0; i < body.size(); ++i) {
        char32_t c = body[i];
        if (c == U'{') ++depth;
        else if (c == U'}') --depth;
        else if (c == U'-' || c == 0x2010) frag = i + 1;
    }
    if (depth > 0) return true;  // the wrap sits inside an open {...} placeholder
    if (frag == std::u32string::npos) return false;
    // The hyphen chain must run unbroken to the wrap: a non-alphanumeric glyph
    // between the last interior hyphen and the wrap ("...camel>}ReadOut-") means
    // the wrap broke a plain suffix, not the compound.
    for (size_t i = frag; i < body.size(); ++i)
        if (!is_ascii_alpha(body[i]) && !is_ascii_digit(body[i])) return false;
    // The fragment plus the continuation's leading letters spelling an attested
    // word marks a syllable wrap inside the compound's last element.
    std::u32string left = lower_ascii(body.substr(frag));
    size_t r = 0;
    while (r < cont.size() && is_ascii_alpha(cont[r])) ++r;
    std::u32string right = lower_ascii(cont.substr(0, r));
    if (!left.empty() && !right.empty() && vocab.count(left + right) > 0) return false;
    return true;
}

// True when a token begins a new self-contained URL: a "www." host, or a short
// scheme-like alpha prefix immediately followed by "://" (covers http, https,
// ftp, ...). Used to stop two independent URLs, wrapped onto adjacent lines,
// from gluing into one unreadable string.
bool starts_new_url(const std::u32string& tok) {
    if (tok.size() >= 4 && tok.compare(0, 4, U"www.") == 0) return true;
    size_t s = tok.find(U"://");
    if (s == std::u32string::npos || s == 0 || s > 8) return false;
    for (size_t i = 0; i < s; ++i)
        if (!is_ascii_alpha(tok[i])) return false;
    return true;
}

// True when the accumulated text ends inside a URL that the line wrap split
// (e.g. "https://github.com/" continued by "tensorflow/..."). The joiner then
// suppresses its separating space so the link survives as one token. Guarded so
// a URL that merely ends a sentence before ordinary prose is left alone, and so
// a continuation that is itself a brand-new URL is never glued on.
bool url_wrap(const std::u32string& prev, const std::u32string& next) {
    char32_t first = next.empty() ? U'\0' : next.front();
    size_t e = prev.size();
    while (e > 0 && prev[e - 1] == U' ') --e;
    size_t b = e;
    while (b > 0 && prev[b - 1] != U' ') --b;
    if (e == b) return false;
    std::u32string tok = prev.substr(b, e - b);
    bool is_url = tok.find(U"://") != std::u32string::npos ||
                  (tok.size() >= 4 && tok.compare(0, 4, U"www.") == 0);
    if (!is_url) return false;
    // The continuation opening a new self-contained URL means these are two
    // independent links, not one split across a wrap: keep the separating space
    // regardless of the preceding URL's last character.
    if (starts_new_url(next)) return false;
    char32_t last = prev[e - 1];
    // Sentence-ending punctuation after the URL means the wrap is a real word
    // break, not a split link.
    if (last == U'.' || last == U'!' || last == U'?' || last == U',' ||
        last == U':' || last == U';' || last == U')')
        return false;
    // A capitalized token following a host/path label that does not end in a URL
    // separator is a new word (a filename listed after the link), not more URL:
    // keep them apart so "www.asam.net" + "ASAM_SOVD..." does not glue. Genuine
    // path continuations wrap after a separator ('/', '-', '_', ...).
    bool sep = last == U'/' || last == U'-' || last == U'_' || last == U'.' ||
               last == U'=' || last == U'?' || last == U'&' || last == U'#' ||
               last == U'%' || last == U'+' || last == U'~';
    if (is_ascii_upper(first) && !sep) return false;
    // The continuation must read as more URL, not the start of a new word.
    return is_ascii_alpha(first) || is_ascii_digit(first) || first == U'/' ||
           first == U'.' || first == U'-' || first == U'_' || first == U'~' ||
           first == U'#' || first == U'?' || first == U'=' || first == U'&' ||
           first == U'%' || first == U'+';
}

// Joins the lines of a block into one run sequence, applying hyphenation
// repair at line boundaries and merging adjacent same-style runs.
std::vector<StyledRun> merge_block_runs(const Block& block, const WordSet& vocab) {
    std::vector<StyledRun> merged;

    auto append_run = [&](const StyledRun& run) {
        if (run.text.empty()) return;
        if (!merged.empty() && !merged.back().raw && !run.raw &&
            merged.back().bold == run.bold && merged.back().italic == run.italic &&
            merged.back().mono == run.mono && merged.back().math == run.math) {
            merged.back().text += run.text;
        } else {
            merged.push_back(run);
        }
    };

    for (const Line& line : block.lines) {
        if (line.runs.empty()) continue;
        if (!merged.empty()) {
            std::u32string& prev = merged.back().text;
            char32_t first = line.plain.empty() ? U'\0' : line.plain.front();
            bool joined = false;
            while (!prev.empty() && prev.back() == U' ') prev.pop_back();
            if (!prev.empty()) {
                char32_t last = prev.back();
                // A lowercase continuation after a line-end hyphen is a wrap: the
                // marker is either PDFium's dehyphenation soft hyphen or a literal
                // hyphen glyph carried to the line edge.
                bool lower_wrap =
                    last == kSoftHyphen ||
                    ((last == U'-' || last == 0x2010) && is_ascii_lower(first));
                if (lower_wrap) {
                    if (compound_hyphen(prev, line.plain, vocab) ||
                        literal_wrap_hyphen(prev, line.plain, vocab)) {
                        // A real hyphenated compound that broke at its hyphen: keep
                        // it, normalizing a soft/Unicode hyphen to an ASCII '-'.
                        if (last == kSoftHyphen || last == 0x2010) {
                            prev.pop_back();
                            prev += U'-';
                        }
                    } else {
                        prev.pop_back();  // single word wrapped: drop the hyphen
                    }
                    joined = true;
                } else if ((last == U'-' || last == 0x2010) && is_ascii_upper(first)) {
                    // Compound word split at its hyphen: keep it, normalizing a
                    // Unicode hyphen to ASCII like the lowercase path above.
                    if (last == 0x2010) {
                        prev.pop_back();
                        prev += U'-';
                    }
                    joined = true;
                } else if (url_wrap(prev, line.plain)) {
                    joined = true;  // a wrapped URL: keep it as one token
                }
            }
            if (!joined) prev += U' ';
            if (merged.back().text.empty()) merged.pop_back();
        }
        for (const StyledRun& run : line.runs) append_run(run);
    }

    // Remove remaining soft hyphens.
    for (StyledRun& run : merged) {
        std::u32string clean;
        clean.reserve(run.text.size());
        for (char32_t cp : run.text)
            if (cp != kSoftHyphen) clean += cp;
        run.text = std::move(clean);
    }
    std::erase_if(merged, [](const StyledRun& r) { return r.text.empty(); });

    // A stray fixed-pitch glyph (punctuation, a digit) inside prose is font
    // noise, not inline code; demote letter-free mono fragments and re-merge.
    for (StyledRun& run : merged) {
        if (!run.mono) continue;
        size_t glyphs = 0;
        bool has_letter = false;
        for (char32_t cp : run.text) {
            if (cp == U' ') continue;
            ++glyphs;
            if (is_ascii_alpha(cp) || cp >= 0x80) has_letter = true;
        }
        if (glyphs <= 2 && !has_letter) run.mono = false;
    }
    std::vector<StyledRun> repacked;
    for (StyledRun& run : merged) {
        if (!repacked.empty() && !repacked.back().raw && !run.raw &&
            repacked.back().bold == run.bold && repacked.back().italic == run.italic &&
            repacked.back().mono == run.mono && repacked.back().math == run.math) {
            repacked.back().text += run.text;
        } else {
            repacked.push_back(std::move(run));
        }
    }
    return repacked;
}

std::string inline_text(const Block& block, bool strip_styles, const WordSet& vocab) {
    std::string out;
    for (const StyledRun& run : merge_block_runs(block, vocab))
        out += emit_run(run, strip_styles);
    // Collapse whitespace-only results and trim edges.
    size_t b = out.find_first_not_of(' ');
    if (b == std::string::npos) return {};
    size_t e = out.find_last_not_of(' ');
    return out.substr(b, e - b + 1);
}

std::string render_code_block(const Block& block) {
    // Estimate the column width from horizontal advances between neighbours.
    std::vector<double> advances;
    for (const Line& ln : block.lines) {
        for (size_t i = 1; i < ln.chars.size(); ++i) {
            double dx = ln.chars[i].x - ln.chars[i - 1].x;
            if (dx > 0.05 && dx < 1.5 * ln.size) advances.push_back(dx);
        }
    }
    double cell = 0.6 * std::max(block.size, 1.0);
    if (!advances.empty()) {
        size_t mid = advances.size() / 2;
        std::nth_element(advances.begin(), advances.begin() + mid, advances.end());
        if (advances[mid] > 0.1) cell = advances[mid];
    }

    std::vector<std::string> rows;
    size_t max_backticks = 0;
    for (const Line& ln : block.lines) {
        if (ln.chars.empty()) {
            rows.emplace_back();
            continue;
        }
        std::u32string row;
        for (const CharInfo& c : ln.chars) {
            if (c.space) continue;
            auto pos = static_cast<size_t>(
                std::max<long>(0, std::lround((c.x - block.x0) / cell)));
            if (row.size() < pos) row.resize(pos, U' ');
            row += c.code;
        }
        std::string utf8_row = to_utf8(row);
        size_t run = 0;
        for (char ch : utf8_row) {
            run = (ch == '`') ? run + 1 : 0;
            max_backticks = std::max(max_backticks, run);
        }
        rows.push_back(std::move(utf8_row));
    }

    std::string fence(std::max<size_t>(3, max_backticks + 1), '`');
    std::string out = fence + "\n";
    for (const std::string& row : rows) out += row + "\n";
    out += fence;
    return out;
}

// One cell's markdown: styled runs emitted and whitespace-normalized to a single
// line. The header row strips emphasis (markdown already bolds header cells and
// the fixtures expect plain header text); data rows keep bold highlights.
std::string render_table_cell(const TableCell& cell, bool header) {
    std::string out;
    for (const StyledRun& r : cell) {
        std::string piece = emit_run(r, /*strip_styles=*/header);
        // Math, code-span and raw runs bypass escape_md, but GFM splits a row
        // at any unescaped '|' -- even inside `...` or $...$ -- so escape the
        // pipes they carry; the plain-text path already emits "\|".
        if (r.math || r.mono || r.raw) {
            std::string esc;
            esc.reserve(piece.size());
            for (char ch : piece) {
                if (ch == '|') esc += '\\';
                esc += ch;
            }
            piece = std::move(esc);
        }
        out += piece;
    }
    std::string norm;
    bool prev_space = false;
    for (char ch : out) {
        if (ch == ' ') {
            if (!prev_space && !norm.empty()) norm += ' ';
            prev_space = true;
        } else {
            norm += ch;
            prev_space = false;
        }
    }
    size_t e = norm.find_last_not_of(' ');
    return e == std::string::npos ? std::string() : norm.substr(0, e + 1);
}

std::string render_table(const Block& block) {
    if (block.table_cells.empty()) return {};
    size_t cols = 0;
    for (const auto& row : block.table_cells) cols = std::max(cols, row.size());
    if (cols == 0) return {};

    auto render_row = [&](const std::vector<TableCell>& row, bool header) {
        std::string out = "|";
        for (size_t c = 0; c < cols; ++c) {
            out += ' ';
            if (c < row.size()) out += render_table_cell(row[c], header);
            out += " |";
        }
        return out;
    };

    std::string out = render_row(block.table_cells[0], /*header=*/true);
    out += "\n|";
    for (size_t c = 0; c < cols; ++c) out += " --- |";
    for (size_t r = 1; r < block.table_cells.size(); ++r) {
        out += "\n" + render_row(block.table_cells[r], /*header=*/false);
    }
    return out;
}

std::string render_math_display(const Block& block) {
    std::vector<std::u32string> lines = block.math_lines;
    if (lines.empty()) return {};
    // The equation number rides the last line as \tag{n}.
    if (!block.math_tag.empty())
        lines.back() += U" \\tag{" + block.math_tag + U"}";

    std::string body;
    if (lines.size() == 1) {
        body = to_utf8(lines[0]);
    } else {
        // Several visual lines form one multi-line equation: keep them aligned.
        body = "\\begin{aligned}\n";
        for (size_t i = 0; i < lines.size(); ++i) {
            body += to_utf8(lines[i]);
            body += (i + 1 < lines.size()) ? " \\\\\n" : "\n";
        }
        body += "\\end{aligned}";
    }
    return "$$\n" + body + "\n$$";
}

std::string render_image(const Block& block) {
    const std::string& p = block.image_path;
    bool needs_brackets = p.find_first_of(" ()") != std::string::npos;
    std::string out = "![](";
    if (needs_brackets) out += "<" + p + ">";
    else out += p;
    out += ")";
    return out;
}

std::string yaml_quote(const std::string& s) {
    std::string out = "\"";
    for (char ch : s) {
        if (ch == '"' || ch == '\\') out += '\\';
        out += ch;
    }
    out += '"';
    return out;
}

// Builder for the document string: parts are accumulated in order, each
// flagged as tight (a single newline joins it to its predecessor) or loose (a
// blank line does). build() reproduces the writer's join exactly, terminating a
// non-empty document with a trailing newline.
class MarkdownAssembler {
public:
    void add(std::string part, bool tight_to_previous = false) {
        parts_.push_back(std::move(part));
        tight_.push_back(tight_to_previous ? 1 : 0);
    }

    std::string build() const {
        std::string out;
        for (size_t i = 0; i < parts_.size(); ++i) {
            if (i > 0) out += tight_[i] ? "\n" : "\n\n";
            out += parts_[i];
        }
        if (!out.empty()) out += "\n";
        return out;
    }

private:
    std::vector<std::string> parts_;
    std::vector<char> tight_;  // parts separated from predecessor by single newline
};

// The write state for one document conversion: the output options, the attested
// vocabulary driving compound-hyphen repair, the running block context (page,
// kind, and whether anything has been emitted yet), and the document Builder.
// run() emits the front matter and then every block in document order.
class MarkdownWriter {
public:
    explicit MarkdownWriter(const WriteOptions& options) : options_(options) {}

    std::string run(const Document& doc) {
        if (options_.front_matter) {
            std::string fm = "---\n";
            if (!doc.meta.title.empty()) fm += "title: " + yaml_quote(doc.meta.title) + "\n";
            if (!doc.meta.author.empty()) fm += "author: " + yaml_quote(doc.meta.author) + "\n";
            if (!options_.source_name.empty())
                fm += "source: " + yaml_quote(options_.source_name) + "\n";
            fm += "pages: " + std::to_string(doc.meta.page_count) + "\n---";
            doc_.add(std::move(fm));
        }

        // The document's attested-word vocabulary drives compound-hyphen repair.
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
            if (!render_block(block, text, is_tight)) continue;
            doc_.add(std::move(text), is_tight);
            prev_kind_ = block.kind;
            first_ = false;
        }

        return doc_.build();
    }

private:
    // Renders one block to markdown. Returns false -- leaving the block context
    // untouched -- when the block emits nothing, matching the original loop's
    // `continue` for empty content.
    bool render_block(const Block& block, std::string& text, bool& is_tight) {
        switch (block.kind) {
            case BlockKind::Heading: {
                std::string content = inline_text(block, /*strip_styles=*/true, vocab_);
                if (content.empty()) return false;
                // A trailing '#' run would be eaten as an ATX closing
                // sequence; escaping its first '#' keeps it literal.
                if (content.back() == '#') {
                    size_t run_start = content.find_last_not_of('#') + 1;
                    content.insert(run_start, "\\");
                }
                text = std::string(static_cast<size_t>(std::clamp(block.heading_level, 1, 6)),
                                   '#') +
                       " " + content;
                break;
            }
            case BlockKind::Paragraph: {
                text = inline_text(block, false, vocab_);
                if (text.empty()) return false;
                if (block.is_footnote) {
                    // A footnote definition: a numbered one becomes a real markdown
                    // footnote ("[^n]: ..."); a symbol note keeps its marker as a
                    // literal prefix ("†...").
                    if (block.footnote_numbered)
                        text = "[^" + to_utf8(block.footnote_label) + "]: " + text;
                    else
                        text = to_utf8(block.footnote_label) + text;
                } else {
                    escape_line_start(text);
                    if (block.blockquote) text = "> " + text;
                }
                break;
            }
            case BlockKind::ListItem: {
                std::string content = inline_text(block, false, vocab_);
                if (content.empty()) return false;
                // The content column starts a fresh block context in
                // CommonMark; leading markers must be escaped here too.
                escape_line_start(content);
                std::string indent(static_cast<size_t>(block.list_indent) * 4, ' ');
                text = indent + to_utf8(block.list_marker) + " " + content;
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

// The module's facade function: build the write state and run one conversion.
std::string write_markdown(const Document& doc, const WriteOptions& options) {
    return MarkdownWriter(options).run(doc);
}

}  // namespace pdf2md
