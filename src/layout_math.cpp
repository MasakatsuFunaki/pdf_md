#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "layout_internal.h"
#include "utf8.h"

namespace pdf2md {
namespace detail {

// ---------------------------------------------------------------- inline math

// The LaTeX spelling of a code point inside a `$...$` atom. Letters and digits
// pass through unchanged; the math symbols and Greek letters common in
// scientific PDFs get their macro, and the few LaTeX-active ASCII characters are
// escaped so they render literally.
std::u32string latex_symbol(char32_t cp) {
    // Word macros carry a trailing space so they don't fuse with a following
    // letter into one undefined control sequence (e.g. `\times` + `dk`).
    switch (cp) {
        case 0x03B1: return U"\\alpha ";    case 0x03B2: return U"\\beta ";
        case 0x03B3: return U"\\gamma ";    case 0x03B4: return U"\\delta ";
        case 0x03B5: case 0x03F5: return U"\\epsilon ";
        case 0x03B6: return U"\\zeta ";
        case 0x03B7: return U"\\eta ";      case 0x03B8: return U"\\theta ";
        case 0x03BA: return U"\\kappa ";    case 0x03BB: return U"\\lambda ";
        case 0x03BC: return U"\\mu ";       case 0x03BD: return U"\\nu ";
        case 0x03BE: return U"\\xi ";       case 0x03C0: return U"\\pi ";
        case 0x03C1: return U"\\rho ";      case 0x03C3: return U"\\sigma ";
        case 0x03C4: return U"\\tau ";      case 0x03C7: return U"\\chi ";
        case 0x03C8: return U"\\psi ";      case 0x03C9: return U"\\omega ";
        case 0x03C6: case 0x03D5: return U"\\phi ";
        case 0x0393: return U"\\Gamma ";    case 0x0394: return U"\\Delta ";
        case 0x0398: return U"\\Theta ";    case 0x039B: return U"\\Lambda ";
        case 0x03A0: return U"\\Pi ";       case 0x03A3: return U"\\Sigma ";
        case 0x03A6: return U"\\Phi ";      case 0x03A8: return U"\\Psi ";
        case 0x03A9: return U"\\Omega ";
        case 0x221A: return U"\\surd ";     case 0x221E: return U"\\infty ";
        case 0x2211: return U"\\sum ";      case 0x220F: return U"\\prod ";
        case 0x222B: return U"\\int ";      case 0x2202: return U"\\partial ";
        case 0x2207: return U"\\nabla ";    case 0x2208: return U"\\in ";
        case 0x00D7: return U"\\times ";    case 0x00B7: case 0x22C5: return U"\\cdot ";
        case 0x2264: return U"\\leq ";      case 0x2265: return U"\\geq ";
        case 0x2260: return U"\\neq ";      case 0x2248: return U"\\approx ";
        case 0x2261: return U"\\equiv ";    case 0x00B1: return U"\\pm ";
        case 0x2192: return U"\\rightarrow "; case 0x2190: return U"\\leftarrow ";
        case 0x21D2: return U"\\Rightarrow "; case 0x21D0: return U"\\Leftarrow ";
        case 0x2212: return U"-";           case 0x00F7: return U"\\div ";
        case 0x2209: return U"\\notin ";    case 0x2286: return U"\\subseteq ";
        case 0x2287: return U"\\supseteq ";  case 0x2295: return U"\\oplus ";
        case 0x2297: return U"\\otimes ";   case 0x2299: return U"\\odot ";
        case 0x2218: return U"\\circ ";     case 0x2026: return U"\\ldots ";
        case 0x2032: return U"'";
        // LaTeX-active ASCII: escape so it is shown, not interpreted.
        case U'%': return U"\\%";  case U'#': return U"\\#";
        case U'&': return U"\\&";  case U'$': return U"\\$";
        case U'_': return U"\\_";  case U'{': return U"\\{";
        case U'}': return U"\\}";
        default: return std::u32string(1, cp);
    }
}

bool is_greek_cp(char32_t cp) {
    return (cp >= 0x0391 && cp <= 0x03A9) || (cp >= 0x03B1 && cp <= 0x03C9) ||
           cp == 0x03D1 || cp == 0x03D5 || cp == 0x03D6 || cp == 0x03F1 ||
           cp == 0x03F5;  // theta/phi/pi/rho and lunate-epsilon variants
}

bool is_latin_letter_cp(char32_t cp) {
    return (cp >= U'A' && cp <= U'Z') || (cp >= U'a' && cp <= U'z');
}

bool is_letter_cp(char32_t cp) { return is_latin_letter_cp(cp) || is_greek_cp(cp); }

// A glyph that can belong to a variable/identifier name (so a sub/superscript
// attaches to the whole name, and "step_num" stays one token).
bool is_ident_cp(char32_t cp) {
    return is_latin_letter_cp(cp) || is_greek_cp(cp) || (cp >= U'0' && cp <= U'9') ||
           cp == U'_';
}

// Unambiguous mathematical operators/relations. Their presence marks a line as
// carrying inline math and seeds a `$...$` span. ASCII operators and the
// bullet-like middle dot are excluded here (they occur in ordinary prose too).
bool is_math_symbol_cp(char32_t cp) {
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
bool is_math_connector_cp(char32_t cp) {
    if (cp >= U'0' && cp <= U'9') return true;
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

// 0 = normal, 1 = superscript, 2 = subscript. A glyph is a script only when it
// is clearly smaller than the line's body size *and* sits off the baseline; the
// size test keeps uniformly-small lines (footnotes, captions) from tripping it.
int script_role(const Line& line, const CharInfo& c) {
    if (c.space || c.size <= 0 || line.size <= 0) return 0;
    double ref = line.size_max > 0 ? line.size_max : line.size;
    if (c.size >= 0.82 * ref) return 0;
    double off = c.y - line.baseline;
    if (off > 0.10 * ref) return 1;
    if (off < -0.06 * ref) return 2;
    return 0;
}

// Walks the x-sorted glyphs, records column segments (used by table detection)
// and returns one Emit per non-space glyph. Shared by both renderers so the
// space/segment logic lives in exactly one place.
std::vector<Emit> plan_line(Line& line) {
    const double wgap = line.word_gap > 0 ? line.word_gap : 0.28 * line.size;
    const double cgap = column_gap_threshold(line.size);

    std::vector<Emit> emits;
    const CharInfo* prev = nullptr;
    bool pending_space = false;
    std::optional<size_t> seg_start;
    size_t last_non_space = 0;

    for (size_t i = 0; i < line.chars.size(); ++i) {
        const CharInfo& c = line.chars[i];
        if (c.space) {
            if (prev) pending_space = true;
            continue;
        }
        bool wide_gap = false;
        if (prev) {
            double gap = c.left - prev->right;
            if (gap > cgap) wide_gap = true;
            if (gap > wgap) pending_space = true;
        }
        if (wide_gap && seg_start) {
            line.segments.emplace_back(*seg_start, last_non_space);
            seg_start.reset();
            pending_space = true;
        }
        if (!seg_start) seg_start = i;
        last_non_space = i;
        emits.push_back({i, pending_space});
        pending_space = false;
        prev = &c;
    }
    if (seg_start) line.segments.emplace_back(*seg_start, last_non_space);
    return emits;
}

// Styled runs with spaces collapsed — the original (math-free) rendering.
void render_plain(Line& line, const std::vector<Emit>& emits) {
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

// Wraps a script's inner LaTeX: a single trivial character needs no braces
// (`W_i^K`, `x_n`), anything longer is braced (`d_{ff}`, `d_{\text{model}}`).
std::u32string wrap_script(const std::u32string& inner) {
    if (inner.size() <= 1) return inner;
    return U"{" + inner + U"}";
}

// Function/operator names that TeX typesets upright with their own macro. When
// an upright multi-letter run in math spells one of these it becomes the macro
// (`\max`, `\sin`) rather than plain `\text{...}`.
const std::u32string* math_operator_macro(const std::u32string& name) {
    static const std::unordered_map<std::u32string, std::u32string> ops = {
        {U"max", U"\\max"},   {U"min", U"\\min"},   {U"sin", U"\\sin"},
        {U"cos", U"\\cos"},   {U"tan", U"\\tan"},   {U"cot", U"\\cot"},
        {U"sec", U"\\sec"},   {U"csc", U"\\csc"},   {U"log", U"\\log"},
        {U"ln", U"\\ln"},     {U"exp", U"\\exp"},   {U"lim", U"\\lim"},
        {U"det", U"\\det"},   {U"gcd", U"\\gcd"},   {U"tanh", U"\\tanh"},
        {U"sinh", U"\\sinh"}, {U"cosh", U"\\cosh"},
    };
    auto it = ops.find(name);
    return it == ops.end() ? nullptr : &it->second;
}

// Renders one operand — a maximal same-style Latin-letter run, a number, or a
// single symbol/Greek glyph — to LaTeX. Bold letters become \mathbf vectors,
// known upright function names their operator macro, other upright multi-letter
// names \text{...} (so `d_model` reads `d_{\text{model}}`, not `dmodel`), upright
// single blackboard capitals \mathbb, and italic letters stay bare (math italic).
std::u32string emit_operand(const std::vector<const CharInfo*>& op, bool allow_blackboard) {
    std::u32string s;
    for (const CharInfo* c : op) s += latex_symbol(c->code);
    const CharInfo* first = op[0];
    if (!is_latin_letter_cp(first->code)) return s;  // number run or symbol/Greek
    if (first->bold) return U"\\mathbf{" + s + U"}";
    // A multi-letter run spelling a standard function name is that operator macro
    // regardless of slant: TeX sets `sin`/`cos`/`log` upright even where the PDF
    // renders them in the surrounding math italic (`\sin`, not a product s·i·n).
    if (op.size() >= 2)
        if (const std::u32string* m = math_operator_macro(s)) return *m;
    if (!first->italic) {
        if (op.size() >= 2) {
            return U"\\text{" + s + U"}";
        }
        // A lone upright capital carrying a superscript is a blackboard set
        // (`\mathbb{R}^d`); without that context it is just a name initial.
        if (allow_blackboard && op.size() == 1) {
            char32_t c = first->code;
            if (c == U'R' || c == U'N' || c == U'Z' || c == U'Q' || c == U'C')
                return U"\\mathbb{" + s + U"}";
        }
    }
    return s;
}

// A radical reports a raised origin (the vinculum height), so its box centre —
// not its baseline — locates it against a math axis. Every other glyph uses its
// baseline.
double frac_ref_y(const CharInfo* c) {
    if (raised_origin_glyph(*c)) return (c->top + c->bottom) / 2.0;
    return c->y;
}

// The dominant baseline of a glyph set: median baseline of the full-size,
// text-origin glyphs (radicals/operators and small scripts do not define it).
double math_axis(const std::vector<const CharInfo*>& gl) {
    double base = 0;
    for (const CharInfo* c : gl)
        if (!raised_origin_glyph(*c)) base = std::max(base, c->size);
    if (base <= 0) base = gl.empty() ? 10.0 : gl.front()->size;
    std::vector<double> ys;
    for (const CharInfo* c : gl)
        if (!raised_origin_glyph(*c) && c->size >= 0.8 * base) ys.push_back(c->y);
    if (ys.empty())
        for (const CharInfo* c : gl) ys.push_back(c->y);
    return median(std::move(ys));
}

// Reconstructs a set of glyphs — which may span several baselines — into LaTeX,
// resolving the 2-D structure that a flat line rendering loses: stacked
// numerators/denominators become \frac, a radical swallows its radicand into
// \sqrt, big operators carry their limits as _{}^{}, and diagonal small glyphs
// stay ordinary sub/superscripts. `axis` is the reference baseline for this
// level (the surrounding text baseline for an inline span, the equation baseline
// for a display line); recursion re-derives the axis for each nested group.
std::u32string math_to_latex(std::vector<const CharInfo*> gl, double axis, bool top_level,
                             int depth) {
    std::stable_sort(gl.begin(), gl.end(),
                     [](const CharInfo* a, const CharInfo* b) { return a->left < b->left; });
    // Drop the spaces but remember where a word break sat: PDFium's break spaces
    // are the only reliable signal that two adjacent operands (e.g. `q_i k_i`)
    // are separate tokens when their glyphs are set close together.
    std::vector<char> space_before;
    {
        std::vector<const CharInfo*> ns;
        bool pending = false;
        for (const CharInfo* c : gl) {
            if (c->space) { pending = true; continue; }
            ns.push_back(c);
            space_before.push_back(pending ? 1 : 0);
            pending = false;
        }
        gl.swap(ns);
    }
    if (gl.empty()) return U"";
    if (depth > 24) {  // pathological nesting: fall back to a flat transcription
        std::u32string flat;
        for (const CharInfo* c : gl) flat += latex_symbol(c->code);
        return flat;
    }

    double base = 0;
    for (const CharInfo* c : gl)
        if (c->code != 0x221A) base = std::max(base, c->size);
    if (base <= 0) base = gl.front()->size > 0 ? gl.front()->size : 10.0;

    // Fraction level: +1 numerator (well above axis), -1 denominator (well
    // below), 0 on the axis. A radical in a denominator hangs just below the
    // axis, so it gets a gentler downward threshold via its box centre.
    auto frac_level = [&](const CharInfo* c) -> int {
        double dy = frac_ref_y(c) - axis;
        // A radical/operator is either on the axis or hangs in a denominator; it
        // is never a numerator, so its box centre only ever reads it downward.
        if (raised_origin_glyph(*c)) return dy < -0.10 * base ? -1 : 0;
        if (dy > 0.34 * base) return 1;
        if (dy < -0.30 * base) return -1;
        return 0;
    };
    // A script is drawn a good step smaller than its base; the cutoff is loose
    // enough to catch a subscript nested inside another script (a limit's own
    // subscript, e.g. the `k` of `d_k` above a summation), where both are small.
    auto small = [&](const CharInfo* c) { return c->size < 0.85 * base; };
    // Diagonal script: a small glyph nudged off the axis, attached to the base
    // to its left (`W_i^Q`). The gentle thresholds keep ordinary scripts (which
    // sit far closer to the axis than a fraction) out of the \frac path.
    auto script_level = [&](const CharInfo* c) -> int {
        if (!small(c)) return 0;
        double dy = c->y - axis;
        if (dy > 0.10 * base) return 1;
        if (dy < -0.05 * base) return -1;
        return 0;
    };

    std::u32string out;
    double prev_right = -1e30;
    char32_t prev_code = 0;
    bool prev_scripted = false;  // the previous operand carried a sub/superscript
    // Inter-atom spacing follows math-typesetting convention rather than the raw
    // gaps, which are inconsistent: relations are always spaced, arithmetic
    // operators and brackets stay tight, a comma binds left and spaces right, and
    // everything else spaces only when a real word gap separates it.
    auto is_relation = [](char32_t c) {
        return c == U'=' || c == U'<' || c == U'>' || c == 0x2264 || c == 0x2265 ||
               c == 0x2248 || c == 0x2260 || c == 0x2261 || c == 0x2192 || c == 0x2190 ||
               c == 0x21D2 || c == 0x21D0;
    };
    auto sep_before = [&](char32_t curc, double left, bool word_break) {
        if (out.empty() || out.back() == U' ' || prev_right <= -1e29) return;
        bool space;
        auto is_open = [](char32_t c) { return c == U'(' || c == U'['; };
        auto is_close = [](char32_t c) { return c == U')' || c == U']'; };
        auto is_arith = [](char32_t c) {
            return c == U'+' || c == U'-' || c == U'/' || c == U'*';
        };
        // Relations and post-comma spacing are conventions of the top-level row;
        // inside a script (a limit like `i=1`, a subscript like `(pos,2i)`)
        // TeX sets them tight, so there only real word gaps insert a space.
        if (top_level && (is_relation(curc) || is_relation(prev_code))) space = true;
        else if (is_open(prev_code) || is_close(curc)) space = false;
        else if (is_arith(curc) || is_arith(prev_code)) space = false;
        else if (curc == U',' || curc == U';') space = false;
        else if (top_level && (prev_code == U',' || prev_code == U';')) space = true;
        else {
            // A scripted variable butted against the next variable is an implicit
            // product (`a_i b_i`), which TeX sets with a thin space.
            bool implicit_product =
                top_level && prev_scripted &&
                (is_letter_cp(curc) || (curc >= U'0' && curc <= U'9'));
            space = word_break || implicit_product || left - prev_right > 0.25 * base;
        }
        if (space) {
            // A literal space between two upright groups is invisible in math mode
            // (`\text{where} \text{head}` renders "wherehead"). When the preceding
            // operand is a `\text{...}` group, fold the gap inside it so it stays
            // visible (`\text{where }`); otherwise emit the ordinary math space.
            if (out.back() == U'}') {
                int bd = 0;
                size_t p = out.size();
                do {
                    --p;
                    if (out[p] == U'}') ++bd;
                    else if (out[p] == U'{') --bd;
                } while (p > 0 && bd != 0);
                if (bd == 0 && p >= 5 && out.compare(p - 5, 5, U"\\text") == 0) {
                    out.insert(out.size() - 1, 1, U' ');
                    return;
                }
            }
            out += U' ';
        }
    };

    size_t i = 0, n = gl.size();
    while (i < n) {
        // (1) A fraction stack: a run of off-axis glyphs carrying both a
        // numerator and a denominator, at least one of them full size (so a
        // pair of small scripts on a base is never mistaken for a fraction).
        if (frac_level(gl[i]) != 0) {
            size_t j = i;
            bool has_up = false, has_down = false, has_big = false;
            while (j < n && frac_level(gl[j]) != 0) {
                int fl = frac_level(gl[j]);
                (fl > 0 ? has_up : has_down) = true;
                if (!small(gl[j])) has_big = true;
                ++j;
            }
            if (has_up && has_down && has_big) {
                std::vector<const CharInfo*> up, down;
                double right = -1e30;
                for (size_t t = i; t < j; ++t) {
                    (frac_level(gl[t]) > 0 ? up : down).push_back(gl[t]);
                    right = std::max(right, gl[t]->right);
                }
                sep_before(gl[i]->code, gl[i]->left, space_before[i] != 0);
                out += U"\\frac{" + math_to_latex(up, math_axis(up), false, depth + 1) + U"}{" +
                       math_to_latex(down, math_axis(down), false, depth + 1) + U"}";
                prev_right = right;
                prev_code = 0;
                prev_scripted = false;
                i = j;
                continue;
            }
            // A one-sided off-axis run with no base to attach to (a leading
            // orphan script): render it at its own axis so it reads level.
            std::vector<const CharInfo*> run(gl.begin() + i, gl.begin() + j);
            sep_before(gl[i]->code, gl[i]->left, space_before[i] != 0);
            out += math_to_latex(run, math_axis(run), false, depth + 1);
            prev_right = gl[j - 1]->right;
            prev_code = 0;
            prev_scripted = false;
            i = j;
            continue;
        }

        // (2) A radical swallows the following factor (an operand plus its own
        // scripts) as its radicand.
        if (gl[i]->code == 0x221A) {
            sep_before(gl[i]->code, gl[i]->left, space_before[i] != 0);
            ++i;
            std::vector<const CharInfo*> rad;
            if (i < n && frac_level(gl[i]) == 0) {
                const CharInfo* b0 = gl[i];
                if (is_latin_letter_cp(b0->code)) {
                    bool bold0 = b0->bold, ital0 = b0->italic;
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
            out += U"\\sqrt{" + math_to_latex(rad, math_axis(rad), false, depth + 1) + U"}";
            if (!rad.empty()) prev_right = rad.back()->right;
            prev_code = 0;
            prev_scripted = false;
            continue;
        }

        // (3) An operand at axis level: a same-style letter run, a number, an
        // ellipsis, or a single symbol/Greek/operator glyph.
        sep_before(gl[i]->code, gl[i]->left, space_before[i] != 0);
        std::vector<const CharInfo*> op;
        char32_t c0 = gl[i]->code;
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
            bool bold0 = gl[i]->bold, ital0 = gl[i]->italic;
            while (i < n && frac_level(gl[i]) == 0 && script_level(gl[i]) == 0 &&
                   is_latin_letter_cp(gl[i]->code) && gl[i]->bold == bold0 &&
                   gl[i]->italic == ital0 &&
                   (op.empty() || gl[i]->left - op.back()->right <= 0.28 * base)) {
                op.push_back(gl[i]);
                ++i;
            }
        } else if (c0 >= U'0' && c0 <= U'9') {
            while (i < n && frac_level(gl[i]) == 0 && script_level(gl[i]) == 0) {
                char32_t cc = gl[i]->code;
                bool digit = cc >= U'0' && cc <= U'9';
                bool inner = (cc == U'.' || cc == U',') && !op.empty() && i + 1 < n &&
                             gl[i + 1]->code >= U'0' && gl[i + 1]->code <= U'9';
                if ((!digit && !inner) ||
                    (!op.empty() && gl[i]->left - op.back()->right > 0.28 * base))
                    break;
                op.push_back(gl[i]);
                ++i;
            }
        } else {
            op.push_back(gl[i]);
            ++i;
        }

        bool has_sup = false;
        for (size_t k = i; k < n && script_level(gl[k]) != 0; ++k)
            if (script_level(gl[k]) == 1) has_sup = true;
        out += emit_operand(op, top_level && has_sup);
        prev_right = op.back()->right;
        prev_code = op.back()->code;

        // (4) Diagonal sub/superscripts trailing this operand; recurse so a
        // script can itself carry structure.
        std::vector<const CharInfo*> subs, sups;
        while (i < n && script_level(gl[i]) != 0) {
            (script_level(gl[i]) == 1 ? sups : subs).push_back(gl[i]);
            prev_right = std::max(prev_right, gl[i]->right);
            ++i;
        }
        // A word macro (`\times `) keeps its guard space, but that space must not
        // sit between a base and its `_`/`^`.
        if (!subs.empty() || !sups.empty())
            while (!out.empty() && out.back() == U' ') out.pop_back();
        if (!subs.empty()) out += U"_" + wrap_script(math_to_latex(subs, math_axis(subs), false, depth + 1));
        if (!sups.empty()) out += U"^" + wrap_script(math_to_latex(sups, math_axis(sups), false, depth + 1));
        prev_scripted = !subs.empty() || !sups.empty();
    }
    return out;
}

// Math-aware rendering: classifies each glyph, groups maximal runs of math glyphs
// into single `$...$` spans (so tuples, fractions and relations no longer shatter
// into italic prose), and leaves everything else as styled text.
void render_math(Line& line, const std::vector<Emit>& emits) {
    // plain mirrors the raw glyph sequence (drives hyphen joins / first-char).
    for (const Emit& e : emits) {
        if (e.space_before && !line.plain.empty()) line.plain += U' ';
        line.plain += line.chars[e.idx].code;
    }

    const size_t m = emits.size();
    std::vector<int> role(m, 0);
    std::vector<char> core(m, 0), prose(m, 0);
    auto glyph = [&](size_t k) -> const CharInfo& { return line.chars[emits[k].idx]; };
    auto tight_prev = [&](size_t k) {
        // Distinguish a real inter-word space from the tight (often zero-width)
        // gaps within an identifier or around stacked scripts.
        return k > 0 && glyph(k).left - glyph(k - 1).right <= 0.2 * std::max(line.size, 1.0);
    };
    for (size_t k = 0; k < m; ++k) role[k] = script_role(line, glyph(k));

    // Cores: scripts, Greek and math symbols.
    for (size_t k = 0; k < m; ++k) {
        const CharInfo& c = glyph(k);
        if (role[k] != 0 || is_greek_cp(c.code) || is_math_symbol_cp(c.code)) core[k] = 1;
    }
    // Script bases: the identifier run immediately preceding each script is math,
    // so a stacked subscript never orphans its variable — even across the
    // spurious zero-width spaces PDFium injects between a base and its scripts.
    for (size_t k = 0; k < m; ++k) {
        if (role[k] == 0) continue;
        long j = static_cast<long>(k) - 1;
        while (j >= 0 && role[static_cast<size_t>(j)] != 0) --j;  // step over sibling scripts
        if (j < 0) continue;
        // The base must sit tight against the script cluster. A numerator or a
        // radical set off by a word space (an inline fraction like "1/√dk"
        // after "of") is not a superscript of the preceding word and must not
        // drag that word into the math span.
        size_t first_script = static_cast<size_t>(j) + 1;
        if (glyph(first_script).left - glyph(static_cast<size_t>(j)).right > 0.3 * line.size)
            continue;
        while (j >= 0 && is_ident_cp(glyph(static_cast<size_t>(j)).code)) {
            core[static_cast<size_t>(j)] = 1;
            if (!tight_prev(static_cast<size_t>(j))) break;  // stop at a real space
            --j;
        }
    }
    // Prose words: a run of >=2 adjacent Latin letters with no core is ordinary
    // text and must not be swallowed into a neighbouring math span.
    for (size_t k = 0; k < m;) {
        if (!is_latin_letter_cp(glyph(k).code)) { ++k; continue; }
        size_t e = k;
        bool any_core = core[k];
        while (e + 1 < m && is_latin_letter_cp(glyph(e + 1).code) && tight_prev(e + 1)) {
            ++e;
            any_core = any_core || core[e];
        }
        if (e > k && !any_core)
            for (size_t t = k; t <= e; ++t) prose[t] = 1;
        k = e + 1;
    }
    // A single isolated bold letter is a vector (\mathbf{z}); a bold *word* or a
    // bold label like "i=4." is not, so require non-bold neighbours too.
    for (size_t k = 0; k < m; ++k) {
        const CharInfo& c = glyph(k);
        if (c.bold && is_latin_letter_cp(c.code) && !prose[k] &&
            (k == 0 || !glyph(k - 1).bold) && (k + 1 >= m || !glyph(k + 1).bold))
            core[k] = 1;
    }
    auto eligible = [&](size_t k) {
        if (core[k]) return true;
        if (prose[k]) return false;
        char32_t c = glyph(k).code;
        return is_math_connector_cp(c) || is_letter_cp(c);
    };

    auto prose_matches = [](const StyledRun& r, const CharInfo& c) {
        return !r.math && r.bold == c.bold && r.italic == c.italic && r.mono == c.mono;
    };
    auto push_prose = [&](size_t k) {
        const CharInfo& c = glyph(k);
        bool sp = emits[k].space_before;
        if (line.runs.empty() || !prose_matches(line.runs.back(), c)) {
            if (sp && !line.runs.empty()) line.runs.back().text += U' ';
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
    auto bracket_balance = [&](size_t a, size_t b) {
        int bal = 0;
        for (size_t t = a; t <= b; ++t) {
            char32_t c = glyph(t).code;
            if (c == U'(' || c == U'[') ++bal;
            else if (c == U')' || c == U']') --bal;
        }
        return bal;
    };
    auto is_edge_punct = [](char32_t c) {
        return c == U'.' || c == U',' || c == U';' || c == U':';
    };

    size_t k = 0;
    while (k < m) {
        if (!eligible(k)) { push_prose(k); ++k; continue; }
        size_t s = k, e = k;
        while (e + 1 < m && eligible(e + 1)) ++e;

        // A run with no math core is just prose (e.g. "(Q, K, V )").
        size_t fc = s, lc = s;
        bool found = false;
        for (size_t t = s; t <= e; ++t)
            if (core[t]) { if (!found) fc = t; lc = t; found = true; }
        if (!found) { for (size_t t = s; t <= e; ++t) push_prose(t); k = e + 1; continue; }

        // Trim trailing sentence punctuation / unbalanced closers (and the same
        // on the left) so a sentence period or a prose paren is not pulled in.
        size_t a = s, b = e;
        while (b > lc) {
            char32_t c = glyph(b).code;
            if (is_edge_punct(c) || ((c == U')' || c == U']') && bracket_balance(a, b) < 0)) {
                --b;
                continue;
            }
            // A hyphen that bridges the math token to a following prose word
            // ("d_v-dimensional") is a word hyphen, not a math minus: leave it and
            // the word outside the span so it reads "$d_v$-dimensional".
            if (is_dashy_cp(c) && b == e && e + 1 < m && prose[e + 1]) {
                --b;
                continue;
            }
            // A trailing bracketed numeric group ("[3]", "[36]", "[3, 5]") after
            // the formula is a citation marker, not part of the math: drop it from
            // the span so it renders as escaped prose "[3]" outside the `$...$`.
            if (c == U']') {
                long o = static_cast<long>(b) - 1;
                bool numeric = true, digit = false;
                while (o > static_cast<long>(lc) && glyph(static_cast<size_t>(o)).code != U'[') {
                    char32_t cc = glyph(static_cast<size_t>(o)).code;
                    if (cc >= U'0' && cc <= U'9') digit = true;
                    else if (cc != U',' && cc != U' ') { numeric = false; break; }
                    --o;
                }
                if (numeric && digit && o > static_cast<long>(lc) &&
                    glyph(static_cast<size_t>(o)).code == U'[') {
                    b = static_cast<size_t>(o) - 1;
                    continue;
                }
            }
            break;
        }
        while (a < fc) {
            char32_t c = glyph(a).code;
            if (is_edge_punct(c) || ((c == U'(' || c == U'[') && bracket_balance(a, b) > 0)) {
                ++a;
                continue;
            }
            break;
        }

        for (size_t t = s; t < a; ++t) push_prose(t);
        if (emits[a].space_before && !line.runs.empty()) line.runs.back().text += U' ';
        // Span the raw char range so interior word-break spaces survive: the math
        // renderer uses them to tell adjacent operands ("q_i k_i") apart.
        std::vector<const CharInfo*> span;
        size_t lo = emits[a].idx, hi = emits[b].idx;
        span.reserve(hi - lo + 1);
        for (size_t idx = lo; idx <= hi; ++idx) span.push_back(&line.chars[idx]);
        StyledRun run;
        run.math = true;
        // The surrounding text baseline is this span's math axis, so an inline
        // fraction or radical stacked around it reconstructs correctly.
        run.text = math_to_latex(std::move(span), line.baseline, /*top_level=*/true);
        line.runs.push_back(std::move(run));
        for (size_t t = b + 1; t <= e; ++t) push_prose(t);
        k = e + 1;
    }
}

// A separator glyph inside a number or an identifier sometimes comes from a
// different (math) font than the characters around it: a decimal point in "0.9"
// or the '_' in "warmup_steps" can arrive upright between italic neighbours,
// which would split the token into emphasised fragments ("0*.*9",
// "*warmup*\_*steps*"). Give such a separator its neighbours' own style so the
// token renders as one run.
void normalize_inline_punct(Line& line) {
    for (size_t i = 1; i + 1 < line.chars.size(); ++i) {
        CharInfo& c = line.chars[i];
        if (c.space) continue;
        const CharInfo& p = line.chars[i - 1];
        const CharInfo& q = line.chars[i + 1];
        if (p.space || q.space) continue;
        bool digit_sep = (c.code == U'.' || c.code == U',' || c.code == U'/') &&
                         is_ascii_digit(p.code) && is_ascii_digit(q.code);
        bool ident_sep = c.code == U'_' && is_ident_cp(p.code) && is_ident_cp(q.code);
        if (!digit_sep && !ident_sep) continue;
        c.italic = p.italic;
        c.bold = p.bold;
        c.mono = p.mono;
    }
}

// A line carries inline math if it has a sub/superscript, a Greek letter or an
// unambiguous math operator — any of which routes it through the math renderer.
bool line_needs_math(const Line& line) {
    for (const CharInfo& c : line.chars) {
        if (c.space) continue;
        if (is_greek_cp(c.code) || is_math_symbol_cp(c.code)) return true;
        if (script_role(line, c) != 0) return true;
    }
    return false;
}

// A math run that is exactly `\text{WORD}^{digits}` — a whole prose word carrying
// nothing but a pure-number superscript — is a footnote reference, not real math
// (a genuine power sits on a single/italic variable, e.g. `W^O` or `x^2`). Returns
// {WORD, digits} on a match.
std::optional<std::pair<std::u32string, std::u32string>> match_footnote_ref(
    const std::u32string& t) {
    const std::u32string pfx = U"\\text{";
    if (t.size() < pfx.size() + 1 || t.compare(0, pfx.size(), pfx) != 0) return std::nullopt;
    size_t i = pfx.size();
    std::u32string word;
    while (i < t.size() && t[i] != U'}') {
        if (!is_latin_letter_cp(t[i])) return std::nullopt;
        word += t[i++];
    }
    if (word.size() < 2 || i >= t.size() || t[i] != U'}') return std::nullopt;
    ++i;
    if (i >= t.size() || t[i] != U'^') return std::nullopt;
    ++i;
    bool brace = i < t.size() && t[i] == U'{';
    if (brace) ++i;
    std::u32string digits;
    while (i < t.size() && t[i] >= U'0' && t[i] <= U'9') digits += t[i++];
    if (digits.empty()) return std::nullopt;
    if (brace) {
        if (i >= t.size() || t[i] != U'}') return std::nullopt;
        ++i;
    }
    if (i != t.size()) return std::nullopt;  // the superscript must be the whole run
    return std::make_pair(word, digits);
}

// Rewrites footnote-reference math runs into a prose word plus a literal `[^n]`
// so the marker becomes a real markdown footnote link rather than `$\text{GPU}^5$`.
void rewrite_footnote_refs(Line& line) {
    bool any = false;
    for (const StyledRun& r : line.runs)
        if (r.math && match_footnote_ref(r.text)) { any = true; break; }
    if (!any) return;
    std::vector<StyledRun> out;
    out.reserve(line.runs.size() + 2);
    for (StyledRun& r : line.runs) {
        std::optional<std::pair<std::u32string, std::u32string>> m;
        if (r.math) m = match_footnote_ref(r.text);
        if (m) {
            StyledRun word;
            word.text = m->first;
            out.push_back(std::move(word));
            StyledRun ref;
            ref.raw = true;
            ref.text = U"[^" + m->second + U"]";
            out.push_back(std::move(ref));
        } else {
            out.push_back(std::move(r));
        }
    }
    line.runs = std::move(out);
}

// Clears word breaks that plan_line inserted inside a single token. Loosely
// tracked numeric/identifier runs and glyph-positioned PDFs push
// compute_word_gap's bimodal split below normal letter spacing, so paired digits
// ("2025-1 1-27", "REQ_ID_01 102"), punctuation that binds to the token on its
// left ("Part 1 :", "87961 .html") and the member after a scope operator
// ("app:: util") each acquire a spurious space. Decided purely on character-class
// adjacency, the measured gap and whether PDFium emitted an actual space glyph —
// never a literal string.
void repair_token_spaces(const Line& line, std::vector<Emit>& emits) {
    const double cgap = column_gap_threshold(line.size);
    // True when a real space glyph sits between the two adjacent non-space glyphs,
    // i.e. the break is the document's own word separator, not one the geometric
    // gap heuristic invented. A genuine separator (two distinct numbers "1.40
    // 20131220") is never a mis-split token, so digit runs only rejoin geometric.
    auto glyph_space_between = [&](size_t lo, size_t hi) {
        for (size_t i = lo + 1; i < hi; ++i)
            if (line.chars[i].space) return true;
        return false;
    };
    for (size_t k = 1; k < emits.size(); ++k) {
        if (!emits[k].space_before) continue;
        const CharInfo& a = line.chars[emits[k - 1].idx];
        const CharInfo& b = line.chars[emits[k].idx];
        if (b.left - a.right > cgap) continue;  // a real column break: leave it
        char32_t pc = a.code, cc = b.code;
        char32_t nc = emits[k].idx + 1 < line.chars.size()
                          ? line.chars[emits[k].idx + 1].code
                          : U'\0';
        bool drop = false;
        if (is_ascii_digit(pc) && is_ascii_digit(cc)) {
            drop = !glyph_space_between(emits[k - 1].idx, emits[k].idx);  // one number
        } else if ((cc == U'.' || cc == U',' || cc == U':' || cc == U';') &&
                   (is_ascii_alpha(pc) || is_ascii_digit(pc)) && !(cc == U':' && nc == U'=')) {
            drop = true;  // punctuation binds to the preceding word — but not the
                          // ":=" assignment operator, and not another punctuation
                          // mark (a spaced "..." row stays)
        } else if (cc == U':' && pc == U':') {
            drop = true;  // the two colons of a scope operator "::" join
        } else if (pc == U':' && k >= 2 && line.chars[emits[k - 2].idx].code == U':') {
            drop = true;  // the scope operator "::" binds its member forward
        } else if (cc == U'-' &&
                   (is_ascii_alpha(pc) || is_ascii_digit(pc)) &&
                   (is_ascii_alpha(nc) || is_ascii_digit(nc)) &&
                   !glyph_space_between(emits[k - 1].idx, emits[k].idx)) {
            drop = true;  // a hyphen inside a token ("V1-0-0", "WWH-OBD") that a
                          // near-threshold gap falsely spaced binds to its neighbours;
                          // a real range ("1 - 2") carries its own space glyph
        }
        if (drop) emits[k].space_before = false;
    }
}

}  // namespace detail
}  // namespace pdf2md
