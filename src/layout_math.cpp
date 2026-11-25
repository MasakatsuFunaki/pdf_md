#include "layout_internal.h"

#include "utf8.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pdf2md {
namespace detail {

namespace {

constexpr char32_t kRadicalCp = 0x221A;

// Script and math-axis geometry ratios (all relative to font/body size).
constexpr double kScriptMaxSizeScale = 0.82;
constexpr double kSuperscriptMinOffsetScale = 0.10;
constexpr double kSubscriptMaxOffsetScale = -0.06;
constexpr double kFallbackMathAxisSizePt = 10.0;
constexpr double kMathAxisBaseMinScale = 0.80;
constexpr int kMaxMathNestingDepth = 24;
constexpr double kRaisedDenomOffsetScale = -0.10;
constexpr double kFracNumOffsetScale = 0.34;
constexpr double kFracDenomOffsetScale = -0.30;
constexpr double kNestedScriptMaxScale = 0.85;
constexpr double kDiagSupOffsetScale = 0.10;
constexpr double kDiagSubOffsetScale = -0.05;
constexpr double kMathWordGapScale = 0.25;
constexpr double kOperandTightGapScale = 0.28;
constexpr double kInlineTightGapScale = 0.20;
constexpr double kScriptBaseMaxGapScale = 0.30;

// Appends the LaTeX spelling of a code point inside a `$...$` atom directly to
// `out` without allocating temporary strings. Letters and digits pass through
// unchanged; math symbols and Greek letters get their macro, and LaTeX-active
// ASCII characters are escaped so they render literally.
void append_latex_symbol(std::u32string& out, char32_t cp) {
    // Word macros carry a trailing space so they don't fuse with a following
    // letter into one undefined control sequence (e.g. `\times` + `dk`).
    std::u32string_view macro;
    switch (cp) {
        case 0x03B1: macro = U"\\alpha "; break;
        case 0x03B2: macro = U"\\beta "; break;
        case 0x03B3: macro = U"\\gamma "; break;
        case 0x03B4: macro = U"\\delta "; break;
        case 0x03B5:
        case 0x03F5: macro = U"\\epsilon "; break;
        case 0x03B6: macro = U"\\zeta "; break;
        case 0x03B7: macro = U"\\eta "; break;
        case 0x03B8: macro = U"\\theta "; break;
        case 0x03BA: macro = U"\\kappa "; break;
        case 0x03BB: macro = U"\\lambda "; break;
        case 0x03BC: macro = U"\\mu "; break;
        case 0x03BD: macro = U"\\nu "; break;
        case 0x03BE: macro = U"\\xi "; break;
        case 0x03C0: macro = U"\\pi "; break;
        case 0x03C1: macro = U"\\rho "; break;
        case 0x03C3: macro = U"\\sigma "; break;
        case 0x03C4: macro = U"\\tau "; break;
        case 0x03C7: macro = U"\\chi "; break;
        case 0x03C8: macro = U"\\psi "; break;
        case 0x03C9: macro = U"\\omega "; break;
        case 0x03C6:
        case 0x03D5: macro = U"\\phi "; break;
        case 0x0393: macro = U"\\Gamma "; break;
        case 0x0394: macro = U"\\Delta "; break;
        case 0x0398: macro = U"\\Theta "; break;
        case 0x039B: macro = U"\\Lambda "; break;
        case 0x03A0: macro = U"\\Pi "; break;
        case 0x03A3: macro = U"\\Sigma "; break;
        case 0x03A6: macro = U"\\Phi "; break;
        case 0x03A8: macro = U"\\Psi "; break;
        case 0x03A9: macro = U"\\Omega "; break;
        case 0x221A: macro = U"\\surd "; break;
        case 0x221E: macro = U"\\infty "; break;
        case 0x2211: macro = U"\\sum "; break;
        case 0x220F: macro = U"\\prod "; break;
        case 0x222B: macro = U"\\int "; break;
        case 0x2202: macro = U"\\partial "; break;
        case 0x2207: macro = U"\\nabla "; break;
        case 0x2208: macro = U"\\in "; break;
        case 0x00D7: macro = U"\\times "; break;
        case 0x00B7:
        case 0x22C5: macro = U"\\cdot "; break;
        case 0x2264: macro = U"\\leq "; break;
        case 0x2265: macro = U"\\geq "; break;
        case 0x2260: macro = U"\\neq "; break;
        case 0x2248: macro = U"\\approx "; break;
        case 0x2261: macro = U"\\equiv "; break;
        case 0x00B1: macro = U"\\pm "; break;
        case 0x2192: macro = U"\\rightarrow "; break;
        case 0x2190: macro = U"\\leftarrow "; break;
        case 0x21D2: macro = U"\\Rightarrow "; break;
        case 0x21D0: macro = U"\\Leftarrow "; break;
        case 0x2212: macro = U"-"; break;
        case 0x00F7: macro = U"\\div "; break;
        case 0x2209: macro = U"\\notin "; break;
        case 0x2286: macro = U"\\subseteq "; break;
        case 0x2287: macro = U"\\supseteq "; break;
        case 0x2295: macro = U"\\oplus "; break;
        case 0x2297: macro = U"\\otimes "; break;
        case 0x2299: macro = U"\\odot "; break;
        case 0x2218: macro = U"\\circ "; break;
        case 0x2026: macro = U"\\ldots "; break;
        case 0x2032: macro = U"'"; break;
        // LaTeX-active ASCII: escape so it is shown, not interpreted.
        case U'%': macro = U"\\%"; break;
        case U'#': macro = U"\\#"; break;
        case U'&': macro = U"\\&"; break;
        case U'$': macro = U"\\$"; break;
        case U'_': macro = U"\\_"; break;
        case U'{': macro = U"\\{"; break;
        case U'}': macro = U"\\}"; break;
        default:
            out += cp;
            return;
    }
    out.append(macro);
}

constexpr bool is_greek_cp(char32_t cp) noexcept {
    return (cp >= 0x0391 && cp <= 0x03A9) || (cp >= 0x03B1 && cp <= 0x03C9) ||
           cp == 0x03D1 || cp == 0x03D5 || cp == 0x03D6 || cp == 0x03F1 ||
           cp == 0x03F5;  // theta/phi/pi/rho and lunate-epsilon variants
}

// A glyph that can belong to a variable/identifier name (so a sub/superscript
// attaches to the whole name, and "step_num" stays one token).
constexpr bool is_ident_cp(char32_t cp) noexcept {
    return (cp >= U'A' && cp <= U'Z') || (cp >= U'a' && cp <= U'z') || is_greek_cp(cp) ||
           (cp >= U'0' && cp <= U'9') || cp == U'_';
}

// Unambiguous mathematical operators/relations. Their presence marks a line as
// carrying inline math and seeds a `$...$` span. ASCII operators and the
// bullet-like middle dot are excluded here (they occur in ordinary prose too).
constexpr bool is_math_symbol_cp(char32_t cp) noexcept {
    switch (cp) {
        case 0x2212: case 0x221E: case 0x00D7: case 0x00F7: case 0x00B1:
        case 0x2208: case 0x2209: case 0x2264: case 0x2265: case 0x2260:
        case 0x2248: case 0x2261: case 0x221A: case 0x2211: case 0x220F:
        case 0x222B: case 0x2202: case 0x2207: case 0x2192: case 0x2190:
        case 0x21D2: case 0x21D0: case 0x2286: case 0x2287: case 0x2295:
        case 0x2297: case 0x2299: case 0x2218:
            return true;
        default:
            return false;
    }
}

// A punctuation/operator glyph that can glue math operands together inside a
// span but never seeds one on its own (parentheses, commas, the decimal point,
// ASCII arithmetic, the middle dot used as a product).
constexpr bool is_math_connector_cp(char32_t cp) noexcept {
    if (cp >= U'0' && cp <= U'9') {
        return true;
    }
    switch (cp) {
        case U'(': case U')': case U'[': case U']': case U'{': case U'}':
        case U'+': case U'-': case U'=': case U'/': case U'*': case U'<':
        case U'>': case U'|': case U',': case U'.': case U':': case U';':
        case U'\'': case 0x00B7: case 0x22C5: case 0x2032: case 0x2026:
            return true;
        default:
            return false;
    }
}

// Appends a script's inner LaTeX prefixed by `prefix` ('_' or '^'): a single
// trivial character needs no braces (`W_i^K`, `x_n`), anything longer is braced.
void append_wrapped_script(std::u32string& out, char32_t prefix, std::u32string_view inner) {
    out += prefix;
    if (inner.size() <= 1) {
        out.append(inner);
        return;
    }
    out += U'{';
    out.append(inner);
    out += U'}';
}

// Function/operator names that TeX typesets upright with their own macro.
// Stored as a sorted compile-time table in .rodata (zero heap allocations, zero
// static initialization races).
constexpr std::u32string_view math_operator_macro(std::u32string_view name) noexcept {
    constexpr std::array<std::pair<std::u32string_view, std::u32string_view>, 17> kOps = {{
        {U"cos", U"\\cos"},   {U"cosh", U"\\cosh"}, {U"cot", U"\\cot"},
        {U"csc", U"\\csc"},   {U"det", U"\\det"},   {U"exp", U"\\exp"},
        {U"gcd", U"\\gcd"},   {U"lim", U"\\lim"},   {U"ln", U"\\ln"},
        {U"log", U"\\log"},   {U"max", U"\\max"},   {U"min", U"\\min"},
        {U"sec", U"\\sec"},   {U"sin", U"\\sin"},   {U"sinh", U"\\sinh"},
        {U"tan", U"\\tan"},   {U"tanh", U"\\tanh"},
    }};
    for (const auto& [op_name, macro] : kOps) {
        if (op_name == name) {
            return macro;
        }
    }
    return {};
}

// Renders one operand — a maximal same-style Latin-letter run, a number, or a
// single symbol/Greek glyph — to LaTeX.
void append_operand(std::u32string& out, std::span<const CharInfo* const> op,
                    bool allow_blackboard) {
    if (op.empty()) {
        return;
    }
    std::u32string s;
    s.reserve(op.size() * 2);
    for (const CharInfo* c : op) {
        append_latex_symbol(s, c->code);
    }
    const CharInfo* first = op[0];
    if (!is_latin_letter_cp(first->code)) {
        out += s;
        return;
    }
    if (first->bold) {
        out += U"\\mathbf{";
        out += s;
        out += U'}';
        return;
    }
    // A multi-letter run spelling a standard function name is that operator macro
    // regardless of slant: TeX sets `sin`/`cos`/`log` upright even where the PDF
    // renders them in the surrounding math italic (`\sin`, not a product s·i·n).
    if (op.size() >= 2) {
        const std::u32string_view m = math_operator_macro(s);
        if (!m.empty()) {
            out.append(m);
            return;
        }
    }
    if (!first->italic) {
        if (op.size() >= 2) {
            out += U"\\text{";
            out += s;
            out += U'}';
            return;
        }
        // A lone upright capital carrying a superscript is a blackboard set
        // (`\mathbb{R}^d`); without that context it is just a name initial.
        if (allow_blackboard && op.size() == 1) {
            const char32_t c = first->code;
            if (c == U'R' || c == U'N' || c == U'Z' || c == U'Q' || c == U'C') {
                out += U"\\mathbb{";
                out += s;
                out += U'}';
                return;
            }
        }
    }
    out += s;
}

double frac_ref_y(const CharInfo* c) noexcept {
    if (raised_origin_glyph(*c)) {
        return (c->top + c->bottom) / 2.0;
    }
    return c->y;
}

// The dominant baseline of a glyph set: median baseline of the full-size,
// text-origin glyphs (radicals/operators and small scripts do not define it).
double math_axis(std::span<const CharInfo* const> gl) {
    double base = 0;
    for (const CharInfo* c : gl) {
        if (!raised_origin_glyph(*c)) {
            base = std::max(base, c->size);
        }
    }
    if (base <= 0) {
        base = gl.empty() ? kFallbackMathAxisSizePt : gl.front()->size;
    }
    std::vector<double> ys;
    ys.reserve(gl.size());
    for (const CharInfo* c : gl) {
        if (!raised_origin_glyph(*c) && c->size >= kMathAxisBaseMinScale * base) {
            ys.push_back(c->y);
        }
    }
    if (ys.empty()) {
        for (const CharInfo* c : gl) {
            ys.push_back(c->y);
        }
    }
    return median(std::move(ys));
}

struct FootnoteRefMatch {
    size_t lead_spaces = 0;
    std::u32string_view word;
    std::u32string_view digits;
    size_t trail_spaces = 0;
};

// Matches a math run that is `\text{WORD}^{digits}` — a whole prose word
// carrying nothing but a pure-number superscript (with optional outer spaces).
std::optional<FootnoteRefMatch> match_footnote_ref(std::u32string_view raw) noexcept {
    const size_t lead = raw.find_first_not_of(U' ');
    if (lead == std::u32string_view::npos) {
        return std::nullopt;
    }
    const size_t last_non_space = raw.find_last_not_of(U' ');
    const size_t trail = raw.size() - 1 - last_non_space;
    const std::u32string_view t = raw.substr(lead, last_non_space + 1 - lead);

    constexpr std::u32string_view kPrefix = U"\\text{";
    if (t.size() < kPrefix.size() + 1 || t.substr(0, kPrefix.size()) != kPrefix) {
        return std::nullopt;
    }
    size_t i = kPrefix.size();
    const size_t word_start = i;
    while (i < t.size() && t[i] != U'}') {
        if (!is_latin_letter_cp(t[i])) {
            return std::nullopt;
        }
        ++i;
    }
    const std::u32string_view word = t.substr(word_start, i - word_start);
    if (word.size() < 2 || i >= t.size() || t[i] != U'}') {
        return std::nullopt;
    }
    ++i;
    if (i >= t.size() || t[i] != U'^') {
        return std::nullopt;
    }
    ++i;
    const bool brace = i < t.size() && t[i] == U'{';
    if (brace) {
        ++i;
    }
    const size_t digits_start = i;
    while (i < t.size() && t[i] >= U'0' && t[i] <= U'9') {
        ++i;
    }
    const std::u32string_view digits = t.substr(digits_start, i - digits_start);
    if (digits.empty()) {
        return std::nullopt;
    }
    if (brace) {
        if (i >= t.size() || t[i] != U'}') {
            return std::nullopt;
        }
        ++i;
    }
    if (i != t.size()) {
        return std::nullopt;  // the superscript must be the whole run
    }
    return FootnoteRefMatch{lead, word, digits, trail};
}

}  // namespace

bool is_latin_letter_cp(char32_t cp) noexcept {
    return (cp >= U'A' && cp <= U'Z') || (cp >= U'a' && cp <= U'z');
}

bool is_letter_cp(char32_t cp) noexcept {
    return is_latin_letter_cp(cp) || is_greek_cp(cp);
}

// 0 = normal, 1 = superscript, 2 = subscript. A glyph is a script only when it
// is clearly smaller than the line's body size *and* sits off the baseline; the
// size test keeps uniformly-small lines (footnotes, captions) from tripping it.
int script_role(const Line& line, const CharInfo& c) noexcept {
    if (c.space || c.size <= 0 || line.size <= 0) {
        return 0;
    }
    const double ref = line.size_max > 0 ? line.size_max : line.size;
    if (c.size >= kScriptMaxSizeScale * ref) {
        return 0;
    }
    const double off = c.y - line.baseline;
    if (off > kSuperscriptMinOffsetScale * ref) {
        return 1;
    }
    if (off < kSubscriptMaxOffsetScale * ref) {
        return 2;
    }
    return 0;
}

// Walks the x-sorted glyphs, records column segments (used by table detection)
// and returns one Emit per non-space glyph.
std::vector<Emit> plan_line(Line& line) {
    const double wgap = line.word_gap > 0 ? line.word_gap : kDefaultWordGapScale * line.size;
    const double cgap = column_gap_threshold(line.size);

    std::vector<Emit> emits;
    emits.reserve(line.chars.size());
    const CharInfo* prev = nullptr;
    bool pending_space = false;
    std::optional<size_t> seg_start;
    size_t last_non_space = 0;

    for (size_t i = 0; i < line.chars.size(); ++i) {
        const CharInfo& c = line.chars[i];
        if (c.space) {
            if (prev) {
                pending_space = true;
            }
            continue;
        }
        bool wide_gap = false;
        if (prev) {
            const double gap = c.left - prev->right;
            if (gap > cgap) {
                wide_gap = true;
            }
            if (gap > wgap) {
                pending_space = true;
            }
        }
        if (wide_gap && seg_start) {
            line.segments.emplace_back(*seg_start, last_non_space);
            seg_start.reset();
            pending_space = true;
        }
        if (!seg_start) {
            seg_start = i;
        }
        last_non_space = i;
        emits.push_back({i, pending_space});
        pending_space = false;
        prev = &c;
    }
    if (seg_start) {
        line.segments.emplace_back(*seg_start, last_non_space);
    }
    return emits;
}

// Styled runs with spaces collapsed — the math-free rendering.
void render_plain(Line& line, std::span<const Emit> emits) {
    line.plain.reserve(emits.size() + emits.size() / 5);
    auto style_matches = [](const StyledRun& r, const CharInfo& c) {
        return r.bold == c.bold && r.italic == c.italic && r.mono == c.mono;
    };
    for (const Emit& e : emits) {
        const CharInfo& c = line.chars[e.idx];
        if (line.runs.empty() || !style_matches(line.runs.back(), c)) {
            if (e.space_before && !line.runs.empty()) {
                line.runs.back().text += U' ';
                line.plain += U' ';
            }
            StyledRun run;
            run.bold = c.bold;
            run.italic = c.italic;
            run.mono = c.mono;
            line.runs.push_back(std::move(run));
        } else if (e.space_before) {
            line.runs.back().text += U' ';
            line.plain += U' ';
        }
        line.runs.back().text += c.code;
        line.plain += c.code;
    }
}

// Reconstructs a set of glyphs — which may span several baselines — into LaTeX,
// resolving stacked numerators/denominators (\frac), radicals (\sqrt), big
// operator limits (_{}^{}), and diagonal sub/superscripts.
std::u32string math_to_latex(std::vector<const CharInfo*> gl, double axis, bool top_level,
                             int depth) {
    std::stable_sort(gl.begin(), gl.end(), [](const CharInfo* a, const CharInfo* b) {
        if (a->left != b->left) {
            return a->left < b->left;
        }
        return a->space && !b->space;
    });
    // Drop the spaces but remember where a word break sat: PDFium's break spaces
    // are the only reliable signal that two adjacent operands (e.g. `q_i k_i`)
    // are separate tokens when their glyphs are set close together.
    std::vector<char> space_before;
    space_before.reserve(gl.size());
    {
        std::vector<const CharInfo*> ns;
        ns.reserve(gl.size());
        bool pending = false;
        for (const CharInfo* c : gl) {
            if (c->space) {
                pending = true;
                continue;
            }
            ns.push_back(c);
            space_before.push_back(pending ? 1 : 0);
            pending = false;
        }
        gl.swap(ns);
    }
    if (gl.empty()) {
        return {};
    }
    if (depth > kMaxMathNestingDepth) {
        std::u32string flat;
        flat.reserve(gl.size() * 2);
        for (const CharInfo* c : gl) {
            append_latex_symbol(flat, c->code);
        }
        return flat;
    }

    double base = 0;
    for (const CharInfo* c : gl) {
        if (c->code != kRadicalCp) {
            base = std::max(base, c->size);
        }
    }
    if (base <= 0) {
        base = gl.front()->size > 0 ? gl.front()->size : kFallbackMathAxisSizePt;
    }

    auto frac_level = [axis, base](const CharInfo* c) -> int {
        const double dy = frac_ref_y(c) - axis;
        if (raised_origin_glyph(*c)) {
            return dy < kRaisedDenomOffsetScale * base ? -1 : 0;
        }
        if (dy > kFracNumOffsetScale * base) {
            return 1;
        }
        if (dy < kFracDenomOffsetScale * base) {
            return -1;
        }
        return 0;
    };
    auto small = [base](const CharInfo* c) { return c->size < kNestedScriptMaxScale * base; };
    auto script_level = [axis, base, &small](const CharInfo* c) -> int {
        if (!small(c)) {
            return 0;
        }
        const double dy = c->y - axis;
        if (dy > kDiagSupOffsetScale * base) {
            return 1;
        }
        if (dy < kDiagSubOffsetScale * base) {
            return -1;
        }
        return 0;
    };

    std::u32string out;
    out.reserve(gl.size() * 2);
    double prev_right = -kCoordSentinel;
    char32_t prev_code = 0;
    bool prev_scripted = false;

    auto is_relation = [](char32_t c) {
        return c == U'=' || c == U'<' || c == U'>' || c == 0x2264 || c == 0x2265 ||
               c == 0x2248 || c == 0x2260 || c == 0x2261 || c == 0x2192 || c == 0x2190 ||
               c == 0x21D2 || c == 0x21D0;
    };
    auto sep_before = [&](char32_t curc, double left, bool word_break) {
        if (out.empty() || out.back() == U' ' || prev_right <= -kCoordSentinelThreshold) {
            return;
        }
        auto is_open = [](char32_t c) { return c == U'(' || c == U'['; };
        auto is_close = [](char32_t c) { return c == U')' || c == U']'; };
        auto is_arith = [](char32_t c) {
            return c == U'+' || c == U'-' || c == U'/' || c == U'*';
        };
        bool space = false;
        if (top_level && (is_relation(curc) || is_relation(prev_code))) {
            space = true;
        } else if (is_open(prev_code) || is_close(curc)) {
            space = false;
        } else if (is_arith(curc) || is_arith(prev_code)) {
            space = false;
        } else if (curc == U',' || curc == U';') {
            space = false;
        } else if (top_level && (prev_code == U',' || prev_code == U';')) {
            space = true;
        } else {
            const bool implicit_product =
                top_level && prev_scripted &&
                (is_letter_cp(curc) || (curc >= U'0' && curc <= U'9'));
            space = word_break || implicit_product || left - prev_right > kMathWordGapScale * base;
        }
        if (!space) {
            return;
        }
        // When the preceding operand is a `\text{...}` group, fold the gap
        // inside it so it stays visible (`\text{where }`).
        if (out.back() == U'}') {
            int bd = 0;
            size_t p = out.size();
            do {
                --p;
                if (out[p] == U'}') {
                    ++bd;
                } else if (out[p] == U'{') {
                    --bd;
                }
            } while (p > 0 && bd != 0);
            constexpr std::u32string_view kTextCmd = U"\\text";
            if (bd == 0 && p >= kTextCmd.size() &&
                out.compare(p - kTextCmd.size(), kTextCmd.size(), kTextCmd) == 0) {
                out.insert(out.size() - 1, 1, U' ');
                return;
            }
        }
        out += U' ';
    };

    size_t i = 0;
    const size_t n = gl.size();
    while (i < n) {
        // (1) A fraction stack: a run of off-axis glyphs carrying both a
        // numerator and a denominator, at least one of them full size.
        if (frac_level(gl[i]) != 0) {
            size_t j = i;
            bool has_up = false;
            bool has_down = false;
            bool has_big = false;
            while (j < n && frac_level(gl[j]) != 0) {
                const int fl = frac_level(gl[j]);
                if (fl > 0) {
                    has_up = true;
                } else {
                    has_down = true;
                }
                if (!small(gl[j])) {
                    has_big = true;
                }
                ++j;
            }
            if (has_up && has_down && has_big) {
                std::vector<const CharInfo*> up;
                std::vector<const CharInfo*> down;
                up.reserve(j - i);
                down.reserve(j - i);
                double right = -kCoordSentinel;
                for (size_t t = i; t < j; ++t) {
                    if (frac_level(gl[t]) > 0) {
                        up.push_back(gl[t]);
                    } else {
                        down.push_back(gl[t]);
                    }
                    right = std::max(right, gl[t]->right);
                }
                sep_before(gl[i]->code, gl[i]->left, space_before[i] != 0);
                const double up_axis = math_axis(up);
                const double down_axis = math_axis(down);
                out += U"\\frac{";
                out += math_to_latex(std::move(up), up_axis, false, depth + 1);
                out += U"}{";
                out += math_to_latex(std::move(down), down_axis, false, depth + 1);
                out += U'}';
                prev_right = right;
                prev_code = 0;
                prev_scripted = false;
                i = j;
                continue;
            }
            // A one-sided off-axis run with no base to attach to: render at its own axis.
            std::vector<const CharInfo*> run(gl.begin() + static_cast<std::ptrdiff_t>(i),
                                             gl.begin() + static_cast<std::ptrdiff_t>(j));
            sep_before(gl[i]->code, gl[i]->left, space_before[i] != 0);
            const double run_axis = math_axis(run);
            out += math_to_latex(std::move(run), run_axis, false, depth + 1);
            prev_right = gl[j - 1]->right;
            prev_code = 0;
            prev_scripted = false;
            i = j;
            continue;
        }

        // (2) A radical swallows the following factor as its radicand.
        if (gl[i]->code == kRadicalCp) {
            sep_before(gl[i]->code, gl[i]->left, space_before[i] != 0);
            ++i;
            std::vector<const CharInfo*> rad;
            if (i < n && frac_level(gl[i]) == 0) {
                const CharInfo* b0 = gl[i];
                if (is_latin_letter_cp(b0->code)) {
                    const bool bold0 = b0->bold;
                    const bool ital0 = b0->italic;
                    while (i < n && frac_level(gl[i]) == 0 && script_level(gl[i]) == 0 &&
                           is_latin_letter_cp(gl[i]->code) && gl[i]->bold == bold0 &&
                           gl[i]->italic == ital0) {
                        rad.push_back(gl[i]);
                        ++i;
                    }
                } else if (b0->code >= U'0' && b0->code <= U'9') {
                    while (i < n && frac_level(gl[i]) == 0 && script_level(gl[i]) == 0 &&
                           gl[i]->code >= U'0' && gl[i]->code <= U'9') {
                        rad.push_back(gl[i]);
                        ++i;
                    }
                } else {
                    rad.push_back(gl[i]);
                    ++i;
                }
                while (i < n && script_level(gl[i]) != 0) {
                    rad.push_back(gl[i]);
                    ++i;
                }
            }
            if (!rad.empty()) {
                prev_right = rad.back()->right;
            }
            const double rad_axis = math_axis(rad);
            out += U"\\sqrt{";
            out += math_to_latex(std::move(rad), rad_axis, false, depth + 1);
            out += U'}';
            prev_code = 0;
            prev_scripted = false;
            continue;
        }

        // (3) An operand at axis level: a same-style letter run, a number, an
        // ellipsis, or a single symbol/Greek/operator glyph.
        sep_before(gl[i]->code, gl[i]->left, space_before[i] != 0);
        std::vector<const CharInfo*> op;
        const char32_t c0 = gl[i]->code;
        if (script_level(gl[i]) != 0) {
            op.push_back(gl[i]);  // orphan script with no base: emit at axis level
            ++i;
        } else if (c0 == U'.' && i + 1 < n && gl[i + 1]->code == U'.' &&
                   frac_level(gl[i + 1]) == 0 && script_level(gl[i + 1]) == 0) {
            const CharInfo* last = gl[i];
            while (i < n && gl[i]->code == U'.' && frac_level(gl[i]) == 0) {
                last = gl[i];
                ++i;
            }
            out += U"\\ldots ";
            prev_right = last->right;
            prev_code = 0;
            continue;
        } else if (is_latin_letter_cp(c0)) {
            const bool bold0 = gl[i]->bold;
            const bool ital0 = gl[i]->italic;
            while (i < n && frac_level(gl[i]) == 0 && script_level(gl[i]) == 0 &&
                   is_latin_letter_cp(gl[i]->code) && gl[i]->bold == bold0 &&
                   gl[i]->italic == ital0 &&
                   (op.empty() || gl[i]->left - op.back()->right <= kOperandTightGapScale * base)) {
                op.push_back(gl[i]);
                ++i;
            }
        } else if (c0 >= U'0' && c0 <= U'9') {
            while (i < n && frac_level(gl[i]) == 0 && script_level(gl[i]) == 0) {
                const char32_t cc = gl[i]->code;
                const bool digit = cc >= U'0' && cc <= U'9';
                const bool inner = (cc == U'.' || cc == U',') && !op.empty() && i + 1 < n &&
                                   gl[i + 1]->code >= U'0' && gl[i + 1]->code <= U'9';
                if ((!digit && !inner) ||
                    (!op.empty() &&
                     gl[i]->left - op.back()->right > kOperandTightGapScale * base)) {
                    break;
                }
                op.push_back(gl[i]);
                ++i;
            }
        } else {
            op.push_back(gl[i]);
            ++i;
        }

        bool has_sup = false;
        for (size_t k = i; k < n && script_level(gl[k]) != 0; ++k) {
            if (script_level(gl[k]) == 1) {
                has_sup = true;
            }
        }
        append_operand(out, op, top_level && has_sup);
        prev_right = op.back()->right;
        prev_code = op.back()->code;

        // (4) Diagonal sub/superscripts trailing this operand.
        std::vector<const CharInfo*> subs;
        std::vector<const CharInfo*> sups;
        while (i < n && script_level(gl[i]) != 0) {
            if (script_level(gl[i]) == 1) {
                sups.push_back(gl[i]);
            } else {
                subs.push_back(gl[i]);
            }
            prev_right = std::max(prev_right, gl[i]->right);
            ++i;
        }
        if (!subs.empty() || !sups.empty()) {
            while (!out.empty() && out.back() == U' ') {
                out.pop_back();
            }
        }
        const bool had_scripts = !subs.empty() || !sups.empty();
        if (!subs.empty()) {
            const double sub_axis = math_axis(subs);
            append_wrapped_script(
                out, U'_', math_to_latex(std::move(subs), sub_axis, false, depth + 1));
        }
        if (!sups.empty()) {
            const double sup_axis = math_axis(sups);
            append_wrapped_script(
                out, U'^', math_to_latex(std::move(sups), sup_axis, false, depth + 1));
        }
        prev_scripted = had_scripts;
    }
    return out;
}

// Math-aware rendering: classifies each glyph, groups maximal runs of math glyphs
// into single `$...$` spans, and leaves everything else as styled text.
void render_math(Line& line, std::span<const Emit> emits) {
    line.plain.reserve(emits.size() + emits.size() / 5);
    for (const Emit& e : emits) {
        if (e.space_before && !line.plain.empty()) {
            line.plain += U' ';
        }
        line.plain += line.chars[e.idx].code;
    }

    const size_t m = emits.size();
    std::vector<int> role(m, 0);
    std::vector<char> core(m, 0);
    std::vector<char> prose(m, 0);
    auto glyph = [&line, emits](size_t k) -> const CharInfo& {
        return line.chars[emits[k].idx];
    };
    auto tight_prev = [&line, &glyph](size_t k) {
        return k > 0 && glyph(k).left - glyph(k - 1).right <=
                            kInlineTightGapScale * std::max(line.size, kMinFontSizePt);
    };
    for (size_t k = 0; k < m; ++k) {
        role[k] = script_role(line, glyph(k));
    }

    // Cores: scripts, Greek and math symbols.
    for (size_t k = 0; k < m; ++k) {
        const CharInfo& c = glyph(k);
        if (role[k] != 0 || is_greek_cp(c.code) || is_math_symbol_cp(c.code)) {
            core[k] = 1;
        }
    }
    // Script bases: the identifier run immediately preceding each script is math.
    for (size_t k = 0; k < m; ++k) {
        if (role[k] == 0) {
            continue;
        }
        std::ptrdiff_t j = static_cast<std::ptrdiff_t>(k) - 1;
        while (j >= 0 && role[static_cast<size_t>(j)] != 0) {
            --j;  // step over sibling scripts
        }
        if (j < 0) {
            continue;
        }
        const size_t first_script = static_cast<size_t>(j) + 1;
        if (glyph(first_script).left - glyph(static_cast<size_t>(j)).right >
            kScriptBaseMaxGapScale * line.size) {
            continue;
        }
        while (j >= 0 && is_ident_cp(glyph(static_cast<size_t>(j)).code)) {
            core[static_cast<size_t>(j)] = 1;
            if (!tight_prev(static_cast<size_t>(j))) {
                break;  // stop at a real space
            }
            --j;
        }
    }
    // Prose words: a run of >=2 adjacent Latin letters with no core is ordinary text.
    for (size_t k = 0; k < m;) {
        if (!is_latin_letter_cp(glyph(k).code)) {
            ++k;
            continue;
        }
        size_t e = k;
        bool any_core = core[k] != 0;
        while (e + 1 < m && is_latin_letter_cp(glyph(e + 1).code) && tight_prev(e + 1)) {
            ++e;
            any_core = any_core || (core[e] != 0);
        }
        if (e > k && !any_core) {
            for (size_t t = k; t <= e; ++t) {
                prose[t] = 1;
            }
        }
        k = e + 1;
    }
    // A single isolated bold letter is a vector (\mathbf{z}); a bold word is not.
    for (size_t k = 0; k < m; ++k) {
        const CharInfo& c = glyph(k);
        if (c.bold && is_latin_letter_cp(c.code) && !prose[k] &&
            (k == 0 || !glyph(k - 1).bold) && (k + 1 >= m || !glyph(k + 1).bold)) {
            core[k] = 1;
        }
    }
    auto eligible = [&core, &prose, &glyph](size_t k) {
        if (core[k]) {
            return true;
        }
        if (prose[k]) {
            return false;
        }
        const char32_t c = glyph(k).code;
        return is_math_connector_cp(c) || is_letter_cp(c);
    };

    auto prose_matches = [](const StyledRun& r, const CharInfo& c) {
        return !r.math && r.bold == c.bold && r.italic == c.italic && r.mono == c.mono;
    };
    auto push_prose = [&line, emits, &glyph, &prose_matches](size_t k) {
        const CharInfo& c = glyph(k);
        const bool sp = emits[k].space_before;
        if (line.runs.empty() || !prose_matches(line.runs.back(), c)) {
            if (sp && !line.runs.empty()) {
                line.runs.back().text += U' ';
            }
            StyledRun run;
            run.bold = c.bold;
            run.italic = c.italic;
            run.mono = c.mono;
            line.runs.push_back(std::move(run));
        } else if (sp) {
            line.runs.back().text += U' ';
        }
        line.runs.back().text += c.code;
    };
    auto bracket_balance = [&glyph](size_t a, size_t b) {
        int bal = 0;
        for (size_t t = a; t <= b; ++t) {
            const char32_t c = glyph(t).code;
            if (c == U'(' || c == U'[') {
                ++bal;
            } else if (c == U')' || c == U']') {
                --bal;
            }
        }
        return bal;
    };
    auto is_edge_punct = [](char32_t c) {
        return c == U'.' || c == U',' || c == U';' || c == U':';
    };

    size_t k = 0;
    while (k < m) {
        if (!eligible(k)) {
            push_prose(k);
            ++k;
            continue;
        }
        const size_t s = k;
        size_t e = k;
        while (e + 1 < m && eligible(e + 1)) {
            ++e;
        }

        size_t fc = s;
        size_t lc = s;
        bool found = false;
        for (size_t t = s; t <= e; ++t) {
            if (core[t]) {
                if (!found) {
                    fc = t;
                }
                lc = t;
                found = true;
            }
        }
        if (!found) {
            for (size_t t = s; t <= e; ++t) {
                push_prose(t);
            }
            k = e + 1;
            continue;
        }

        size_t a = s;
        size_t b = e;
        while (b > lc) {
            const char32_t c = glyph(b).code;
            if (is_edge_punct(c) ||
                ((c == U')' || c == U']') && bracket_balance(a, b) < 0)) {
                --b;
                continue;
            }
            if (is_dashy_cp(c) && b == e && e + 1 < m && prose[e + 1]) {
                --b;
                continue;
            }
            if (c == U']') {
                std::ptrdiff_t o = static_cast<std::ptrdiff_t>(b) - 1;
                bool numeric = true;
                bool digit = false;
                while (o > static_cast<std::ptrdiff_t>(lc) &&
                       glyph(static_cast<size_t>(o)).code != U'[') {
                    const char32_t cc = glyph(static_cast<size_t>(o)).code;
                    if (cc >= U'0' && cc <= U'9') {
                        digit = true;
                    } else if (cc != U',' && cc != U' ') {
                        numeric = false;
                        break;
                    }
                    --o;
                }
                if (numeric && digit && o > static_cast<std::ptrdiff_t>(lc) &&
                    glyph(static_cast<size_t>(o)).code == U'[') {
                    b = static_cast<size_t>(o) - 1;
                    continue;
                }
            }
            break;
        }
        while (a < fc) {
            const char32_t c = glyph(a).code;
            if (is_edge_punct(c) ||
                ((c == U'(' || c == U'[') && bracket_balance(a, b) > 0)) {
                ++a;
                continue;
            }
            break;
        }

        for (size_t t = s; t < a; ++t) {
            push_prose(t);
        }
        if (emits[a].space_before && !line.runs.empty()) {
            line.runs.back().text += U' ';
        }
        std::vector<const CharInfo*> span;
        const size_t lo = emits[a].idx;
        const size_t hi = emits[b].idx;
        span.reserve(hi - lo + 1);
        for (size_t idx = lo; idx <= hi; ++idx) {
            span.push_back(&line.chars[idx]);
        }
        StyledRun run;
        run.math = true;
        run.text = math_to_latex(std::move(span), line.baseline, /*top_level=*/true);
        line.runs.push_back(std::move(run));
        for (size_t t = b + 1; t <= e; ++t) {
            push_prose(t);
        }
        k = e + 1;
    }
}

void normalize_inline_punct(Line& line) {
    for (size_t i = 1; i + 1 < line.chars.size(); ++i) {
        CharInfo& c = line.chars[i];
        if (c.space) {
            continue;
        }
        const CharInfo& p = line.chars[i - 1];
        const CharInfo& q = line.chars[i + 1];
        if (p.space || q.space) {
            continue;
        }
        const bool digit_sep = (c.code == U'.' || c.code == U',' || c.code == U'/') &&
                               is_ascii_digit(p.code) && is_ascii_digit(q.code);
        const bool ident_sep = c.code == U'_' && is_ident_cp(p.code) && is_ident_cp(q.code);
        if (!digit_sep && !ident_sep) {
            continue;
        }
        c.italic = p.italic;
        c.bold = p.bold;
        c.mono = p.mono;
    }
}

bool line_needs_math(const Line& line) noexcept {
    for (const CharInfo& c : line.chars) {
        if (c.space) {
            continue;
        }
        if (is_greek_cp(c.code) || is_math_symbol_cp(c.code)) {
            return true;
        }
        if (script_role(line, c) != 0) {
            return true;
        }
    }
    return false;
}

void rewrite_footnote_refs(Line& line) {
    const bool any = std::any_of(line.runs.cbegin(), line.runs.cend(), [](const StyledRun& r) {
        return r.math && match_footnote_ref(r.text).has_value();
    });
    if (!any) {
        return;
    }
    std::vector<StyledRun> out;
    out.reserve(line.runs.size() + 2);
    for (StyledRun& r : line.runs) {
        if (r.math) {
            if (const auto m = match_footnote_ref(r.text)) {
                StyledRun word;
                word.text.reserve(m->lead_spaces + m->word.size());
                word.text.append(m->lead_spaces, U' ');
                word.text.append(m->word);
                out.push_back(std::move(word));

                StyledRun ref;
                ref.raw = true;
                ref.text.reserve(3 + m->digits.size() + m->trail_spaces);
                ref.text += U"[^";
                ref.text.append(m->digits);
                ref.text += U']';
                ref.text.append(m->trail_spaces, U' ');
                out.push_back(std::move(ref));
                continue;
            }
        }
        out.push_back(std::move(r));
    }
    line.runs = std::move(out);
}

void repair_token_spaces(const Line& line, std::vector<Emit>& emits) {
    const double cgap = column_gap_threshold(line.size);
    auto glyph_space_between = [&line](size_t lo, size_t hi) {
        for (size_t i = lo + 1; i < hi; ++i) {
            if (line.chars[i].space) {
                return true;
            }
        }
        return false;
    };
    for (size_t k = 1; k < emits.size(); ++k) {
        if (!emits[k].space_before) {
            continue;
        }
        const CharInfo& a = line.chars[emits[k - 1].idx];
        const CharInfo& b = line.chars[emits[k].idx];
        if (b.left - a.right > cgap) {
            continue;  // a real column break: leave it
        }
        const char32_t pc = a.code;
        const char32_t cc = b.code;
        const char32_t nc =
            emits[k].idx + 1 < line.chars.size() ? line.chars[emits[k].idx + 1].code : U'\0';
        bool drop = false;
        if (is_ascii_digit(pc) && is_ascii_digit(cc)) {
            drop = !glyph_space_between(emits[k - 1].idx, emits[k].idx);
        } else if ((cc == U'.' || cc == U',' || cc == U':' || cc == U';') &&
                   (is_ascii_alpha(pc) || is_ascii_digit(pc)) && !(cc == U':' && nc == U'=')) {
            drop = true;
        } else if (cc == U':' && pc == U':') {
            drop = true;
        } else if (pc == U':' && k >= 2 && line.chars[emits[k - 2].idx].code == U':') {
            drop = true;
        } else if (cc == U'-' && (is_ascii_alpha(pc) || is_ascii_digit(pc)) &&
                   (is_ascii_alpha(nc) || is_ascii_digit(nc)) &&
                   !glyph_space_between(emits[k - 1].idx, emits[k].idx)) {
            drop = true;
        }
        if (drop) {
            emits[k].space_before = false;
        }
    }
}

}  // namespace detail
}  // namespace pdf2md
