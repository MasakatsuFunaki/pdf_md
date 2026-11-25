#include "pdf2md/layout/math.h"

#include "pdf2md/layout/line_cluster.h"
#include "pdf2md/model/document_utils.h"
#include "pdf2md/model/geometry.h"
#include "pdf2md/utf8.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pdf2md::layout {

using model::kCoordSentinel;
using model::kCoordSentinelThreshold;
using model::kDefaultWordGapScale;
using model::kMinFontSizePt;
using model::median;
using model::raised_origin_glyph;

namespace {

constexpr char32_t kRadicalCp = 0x221AU;
constexpr char32_t kSectionSignCp = 0x00A7U;
constexpr char32_t kPilcrowCp = 0x00B6U;
constexpr char32_t kDaggerCp = 0x2020U;
constexpr char32_t kDoubleDaggerCp = 0x2021U;
constexpr char32_t kAsteriskOperatorCp = 0x2217U;

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

// Rule-guided fraction and radical vinculum geometry ratios.
constexpr double kCoreRuleHorizPadScale = 0.25;
constexpr double kFracBarHorizPadScale = 0.20;
constexpr double kFracBarMaxVertDistScale = 2.2;
constexpr double kFracRadicalStraddleScale = 0.15;
constexpr double kSubRuleVertClearancePt = 0.3;
constexpr double kRadicalIndexMaxSizeScale = 0.85;
constexpr double kRadicalIndexMinAxisOffScale = 0.12;
constexpr double kRadicalIndexLeftPadScale = 1.05;
constexpr double kRadicalIndexMaxGapToRadScale = 0.55;
constexpr double kRadicalIndexRightPadScale = 0.25;
constexpr std::size_t kMaxRadicalIndexGlyphs = 3U;
constexpr double kVinculumStartLeftPadScale = 0.25;
constexpr double kVinculumStartRightPadScale = 0.50;
constexpr double kVinculumMinAxisOffScale = 0.40;
constexpr double kVinculumBelowRadTopTolScale = 0.35;
constexpr double kVinculumAboveRadTopTolScale = 0.55;
constexpr double kVinculumEndPadScale = 0.20;
constexpr double kVinculumBottomPadScale = 0.30;
constexpr double kVinculumTopOverhangScale = 0.25;
constexpr std::size_t kMinPlusOnlyWordLen = 3U;

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

// A glyph that can belong to a variable/identifier name (so a sub/superscript
// attaches to the whole name, and "step_num" stays one token).
constexpr bool is_ident_cp(char32_t cp) noexcept {
    return is_letter_cp(cp) || is_ascii_digit(cp) || cp == U'_';
}

// Unambiguous mathematical operators/relations. Their presence marks a line as
// carrying inline math and seeds a `$...$` span.
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
// span but never seeds one on its own.
constexpr bool is_math_connector_cp(char32_t cp) noexcept {
    if (is_ascii_digit(cp)) {
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

void append_wrapped_script(std::u32string& out, char32_t prefix, std::u32string_view inner) {
    out += prefix;
    if (inner.size() <= 1U) {
        out.append(inner);
        return;
    }
    out += U'{';
    out.append(inner);
    out += U'}';
}

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

void append_operand(std::u32string& out, std::span<const CharInfo* const> op,
                    bool allow_blackboard) {
    if (op.empty()) {
        return;
    }
    std::u32string s;
    s.reserve(op.size() * 2U);
    for (const CharInfo* c : op) {
        append_latex_symbol(s, c->code);
    }
    const CharInfo* first = op[0];
    if (!is_ascii_alpha(first->code)) {
        out += s;
        return;
    }
    if (first->bold) {
        out += U"\\mathbf{";
        out += s;
        out += U'}';
        return;
    }
    if (op.size() >= 2U) {
        const std::u32string_view m = math_operator_macro(s);
        if (!m.empty()) {
            out.append(m);
            return;
        }
    }
    if (!first->italic) {
        if (op.size() >= 2U) {
            out += U"\\text{";
            out += s;
            out += U'}';
            return;
        }
        if (allow_blackboard && op.size() == 1U) {
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

double math_axis(std::span<const CharInfo* const> gl) {
    double base = 0.0;
    for (const CharInfo* c : gl) {
        if (!raised_origin_glyph(*c)) {
            base = (std::max)(base, c->size);
        }
    }
    if (base <= 0.0) {
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

struct UnwrappedProseRefItem {
    std::u32string prefix;  // optional leading initials like "R. " or "N. "
    bool prefix_bold = false;
    std::u32string word;
    bool bold = false;
    bool numeric_footnote = false;
    std::u32string marker;  // digits for `[^digits]`, or literal affiliation symbols
    std::u32string sep_after;
};

struct UnwrappedProseRefs {
    std::size_t lead_spaces = 0;
    std::vector<UnwrappedProseRefItem> items;
    std::size_t trail_spaces = 0;
};

constexpr bool is_prose_word_letter(char32_t cp) noexcept {
    return is_ascii_alpha(cp) || (cp >= 0x00C0U && cp <= 0x024FU && cp != 0x00D7U && cp != 0x00F7U);
}

constexpr bool is_affiliation_symbol_cp(char32_t cp) noexcept {
    return cp == U'*' || cp == kAsteriskOperatorCp || cp == kDaggerCp || cp == kDoubleDaggerCp ||
           cp == kSectionSignCp || cp == kPilcrowCp || cp == U'+';
}

std::optional<UnwrappedProseRefs> match_prose_superscript_refs(std::u32string_view raw) {
    const std::size_t lead = raw.find_first_not_of(U' ');
    if (lead == std::u32string_view::npos) {
        return std::nullopt;
    }
    const std::size_t last_non_space = raw.find_last_not_of(U' ');
    const std::size_t trail = raw.size() - 1U - last_non_space;
    const std::u32string_view t = raw.substr(lead, last_non_space + 1U - lead);

    UnwrappedProseRefs result;
    result.lead_spaces = lead;
    result.trail_spaces = trail;

    constexpr std::u32string_view kTextPrefix = U"\\text{";
    constexpr std::u32string_view kBoldPrefix = U"\\mathbf{";

    std::size_t i = 0U;
    while (i < t.size()) {
        std::u32string prefix;
        bool prefix_bold = false;
        while (i < t.size()) {
            if (i + 2U < t.size() && is_ascii_upper(t[i]) && t[i + 1U] == U'.' &&
                t[i + 2U] == U' ') {
                prefix.append(t.substr(i, 3U));
                i += 3U;
                while (i < t.size() && t[i] == U' ') {
                    prefix += U' ';
                    ++i;
                }
                continue;
            }
            if (t.substr(i).starts_with(kBoldPrefix) &&
                i + kBoldPrefix.size() + 4U <= t.size() &&
                is_ascii_upper(t[i + kBoldPrefix.size()]) &&
                t[i + kBoldPrefix.size() + 1U] == U'}' &&
                t[i + kBoldPrefix.size() + 2U] == U'.' &&
                t[i + kBoldPrefix.size() + 3U] == U' ') {
                prefix += t[i + kBoldPrefix.size()];
                prefix += U". ";
                prefix_bold = true;
                i += kBoldPrefix.size() + 4U;
                while (i < t.size() && t[i] == U' ') {
                    prefix += U' ';
                    ++i;
                }
                continue;
            }
            break;
        }

        bool is_bold = false;
        if (t.substr(i).starts_with(kTextPrefix)) {
            i += kTextPrefix.size();
        } else if (t.substr(i).starts_with(kBoldPrefix)) {
            is_bold = true;
            i += kBoldPrefix.size();
        } else {
            return std::nullopt;
        }

        const std::size_t word_start = i;
        while (i < t.size() && t[i] != U'}') {
            if (!is_prose_word_letter(t[i])) {
                return std::nullopt;
            }
            ++i;
        }
        const std::u32string_view word = t.substr(word_start, i - word_start);
        if (word.size() < 2U || i >= t.size() || t[i] != U'}') {
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

        std::u32string marker;
        bool all_digits = true;
        bool all_plus = true;
        while (i < t.size() && (!brace || t[i] != U'}')) {
            if (!brace && !marker.empty()) {
                break;  // unbraced superscript is a single token
            }
            if (brace && t[i] == U' ') {
                ++i;  // collapse interior spacing inside braced affiliation markers (e.g. `^{∗ ‡}`)
                continue;
            }
            if (t[i] == U'\\' && i + 1U < t.size() &&
                (t[i + 1U] == U'#' || t[i + 1U] == U'$' || t[i + 1U] == U'&')) {
                marker += t[i + 1U];
                all_digits = false;
                all_plus = false;
                i += 2U;
                continue;
            }
            const char32_t sc = t[i];
            if (is_ascii_digit(sc)) {
                marker += sc;
                all_plus = false;
                ++i;
                continue;
            }
            if (is_affiliation_symbol_cp(sc) || (sc == U',' && !marker.empty())) {
                marker += sc;
                all_digits = false;
                if (sc != U'+') {
                    all_plus = false;
                }
                ++i;
                continue;
            }
            return std::nullopt;
        }
        if (marker.empty() || marker.back() == U',') {
            return std::nullopt;
        }
        if (all_plus && word.size() < kMinPlusOnlyWordLen) {
            return std::nullopt;  // preserve 2-letter chemical ions like Na^+
        }
        if (brace) {
            if (i >= t.size() || t[i] != U'}') {
                return std::nullopt;
            }
            ++i;
        }

        std::u32string sep;
        while (i < t.size() && (t[i] == U',' || t[i] == U' ')) {
            sep += t[i];
            ++i;
        }
        if (i < t.size() && sep.empty()) {
            return std::nullopt;
        }

        UnwrappedProseRefItem item;
        item.prefix = std::move(prefix);
        item.prefix_bold = prefix_bold;
        item.word.assign(word);
        item.bold = is_bold;
        item.numeric_footnote = all_digits;
        item.marker = std::move(marker);
        item.sep_after = std::move(sep);
        result.items.push_back(std::move(item));
    }

    if (result.items.empty()) {
        return std::nullopt;
    }
    return result;
}

constexpr double glyph_cx(const CharInfo* c) noexcept {
    return (c->left + c->right) * 0.5;
}

bool is_radical_index_glyph(const CharInfo* idx_c, const CharInfo* rad_c, double axis,
                            double base) noexcept {
    if (idx_c->code == kRadicalCp || idx_c->size >= kRadicalIndexMaxSizeScale * base) {
        return false;
    }
    if (idx_c->y <= axis + kRadicalIndexMinAxisOffScale * base) {
        return false;
    }
    return idx_c->left >= rad_c->left - kRadicalIndexLeftPadScale * base &&
           idx_c->right >= rad_c->left - kRadicalIndexMaxGapToRadScale * base &&
           idx_c->right <= rad_c->right + kRadicalIndexRightPadScale * base;
}

void trim_trailing_spaces(std::u32string& s) noexcept {
    while (!s.empty() && s.back() == U' ') {
        s.pop_back();
    }
}

}  // namespace

// 0 = normal, 1 = superscript, 2 = subscript.
int script_role(const Line& line, const CharInfo& c) noexcept {
    if (c.space || c.size <= 0.0 || line.size <= 0.0) {
        return 0;
    }
    const double ref = line.size_max > 0.0 ? line.size_max : line.size;
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
    line.segments.clear();
    const double wgap = line.word_gap > 0.0 ? line.word_gap : kDefaultWordGapScale * line.size;
    const double cgap = column_gap_threshold(line.size);

    std::vector<Emit> emits;
    emits.reserve(line.chars.size());
    const CharInfo* prev = nullptr;
    bool pending_space = false;
    std::optional<std::size_t> seg_start;
    std::size_t last_non_space = 0U;

    for (std::size_t i = 0U; i < line.chars.size(); ++i) {
        const CharInfo& c = line.chars[i];
        if (c.space) {
            if (prev != nullptr) {
                pending_space = true;
            }
            continue;
        }
        bool wide_gap = false;
        if (prev != nullptr) {
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
    line.plain.reserve(emits.size() + emits.size() / 5U);
    const auto style_matches = [](const StyledRun& r, const CharInfo& c) noexcept {
        return r.bold == c.bold && r.italic == c.italic && r.mono == c.mono &&
               r.strikethrough == c.strikethrough && r.link_id == c.link_id;
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
            run.strikethrough = c.strikethrough;
            run.link_id = c.link_id;
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
                             int depth, std::span<const RuleSegment> rules) {
    std::stable_sort(gl.begin(), gl.end(), [](const CharInfo* a, const CharInfo* b) noexcept {
        if (a->left != b->left) {
            return a->left < b->left;
        }
        return a->space && !b->space;
    });
    // Drop the spaces but remember where a word break sat.
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
        flat.reserve(gl.size() * 2U);
        for (const CharInfo* c : gl) {
            append_latex_symbol(flat, c->code);
        }
        trim_trailing_spaces(flat);
        return flat;
    }

    double base = 0.0;
    for (const CharInfo* c : gl) {
        if (c->code != kRadicalCp) {
            base = (std::max)(base, c->size);
        }
    }
    if (base <= 0.0) {
        base = gl.front()->size > 0.0 ? gl.front()->size : kFallbackMathAxisSizePt;
    }

    const auto frac_level = [axis, base](const CharInfo* c) noexcept -> int {
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
    const auto small = [base](const CharInfo* c) noexcept {
        return c->size < kNestedScriptMaxScale * base;
    };
    const auto script_level = [axis, base, &small](const CharInfo* c) noexcept -> int {
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
    out.reserve(gl.size() * 2U);
    double prev_right = -kCoordSentinel;
    char32_t prev_code = 0U;
    bool prev_scripted = false;

    const auto is_relation = [](char32_t c) noexcept {
        return c == U'=' || c == U'<' || c == U'>' || c == 0x2264 || c == 0x2265 ||
               c == 0x2248 || c == 0x2260 || c == 0x2261 || c == 0x2192 || c == 0x2190 ||
               c == 0x21D2 || c == 0x21D0;
    };
    const auto sep_before = [&](char32_t curc, double left, bool word_break) {
        if (out.empty() || out.back() == U' ' || prev_right <= -kCoordSentinelThreshold) {
            return;
        }
        const auto is_open = [](char32_t c) noexcept { return c == U'(' || c == U'['; };
        const auto is_close = [](char32_t c) noexcept { return c == U')' || c == U']'; };
        const auto is_arith = [](char32_t c) noexcept {
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
                top_level && prev_scripted && (is_letter_cp(curc) || is_ascii_digit(curc));
            space = word_break || implicit_product || left - prev_right > kMathWordGapScale * base;
        }
        if (!space) {
            return;
        }
        if (out.back() == U'}') {
            int bd = 0;
            std::size_t p = out.size();
            do {
                --p;
                if (out[p] == U'}') {
                    ++bd;
                } else if (out[p] == U'{') {
                    --bd;
                }
            } while (p > 0U && bd != 0);
            constexpr std::u32string_view kTextCmd = U"\\text";
            if (bd == 0 && p >= kTextCmd.size() &&
                out.compare(p - kTextCmd.size(), kTextCmd.size(), kTextCmd) == 0) {
                out.insert(out.size() - 1U, 1U, U' ');
                return;
            }
        }
        out += U' ';
    };

    std::size_t i = 0U;
    const std::size_t n = gl.size();
    std::vector<char> used_gl(n, 0);
    std::vector<char> used_rule(rules.size(), 0);

    while (i < n) {
        if (used_gl[i] != 0) {
            ++i;
            continue;
        }

        // (1a) Rule-guided stacked fraction (`\frac{num}{den}`).
        if (!rules.empty()) {
            std::optional<std::size_t> best_ri;
            double best_width = -1.0;
            const double cx_i = glyph_cx(gl[i]);
            const double cy_i = frac_ref_y(gl[i]);
            const double hpad = kFracBarHorizPadScale * base;
            const double max_vdist = kFracBarMaxVertDistScale * base;

            for (std::size_t ri = 0U; ri < rules.size(); ++ri) {
                if (used_rule[ri] != 0 || !rules[ri].horizontal) {
                    continue;
                }
                const double bx0 = (std::min)(rules[ri].x0, rules[ri].x1);
                const double bx1 = (std::max)(rules[ri].x0, rules[ri].x1);
                const double by = (rules[ri].y0 + rules[ri].y1) * 0.5;
                const double bw = bx1 - bx0;
                if (bw <= best_width) {
                    continue;
                }
                if (cx_i < bx0 - hpad || cx_i > bx1 + hpad || std::fabs(cy_i - by) > max_vdist) {
                    continue;
                }
                // Do not split across an enclosing radical glyph that straddles `by`.
                bool straddles_radical = false;
                for (std::size_t t = i; t < n; ++t) {
                    if (used_gl[t] != 0 || gl[t]->code != kRadicalCp) {
                        continue;
                    }
                    if (gl[t]->left <= bx0 + hpad &&
                        glyph_cx(gl[t]) >= bx0 - kRadicalIndexLeftPadScale * base &&
                        gl[t]->bottom < by - kFracRadicalStraddleScale * base &&
                        gl[t]->top > by + kFracRadicalStraddleScale * base) {
                        straddles_radical = true;
                        break;
                    }
                }
                if (straddles_radical) {
                    continue;
                }

                bool has_up = false;
                bool has_down = false;
                for (std::size_t t = i; t < n; ++t) {
                    if (used_gl[t] != 0) {
                        continue;
                    }
                    const double cx = glyph_cx(gl[t]);
                    const double cy = frac_ref_y(gl[t]);
                    if (cx < bx0 - hpad || cx > bx1 + hpad || std::fabs(cy - by) > max_vdist) {
                        continue;
                    }
                    if (cy > by) {
                        has_up = true;
                    } else if (cy < by) {
                        has_down = true;
                    }
                }
                if (has_up && has_down) {
                    best_ri = ri;
                    best_width = bw;
                }
            }

            if (best_ri) {
                const RuleSegment& bar = rules[*best_ri];
                used_rule[*best_ri] = 1;
                const double bx0 = (std::min)(bar.x0, bar.x1);
                const double bx1 = (std::max)(bar.x0, bar.x1);
                const double by = (bar.y0 + bar.y1) * 0.5;

                std::vector<const CharInfo*> up;
                std::vector<const CharInfo*> down;
                double right = bx1;
                for (std::size_t t = i; t < n; ++t) {
                    if (used_gl[t] != 0) {
                        continue;
                    }
                    const double cx = glyph_cx(gl[t]);
                    const double cy = frac_ref_y(gl[t]);
                    if (cx < bx0 - hpad || cx > bx1 + hpad || std::fabs(cy - by) > max_vdist) {
                        continue;
                    }
                    used_gl[t] = 1;
                    const double box_mid = (gl[t]->bottom + gl[t]->top) * 0.5;
                    if (cy > by || (cy == by && box_mid >= by)) {
                        up.push_back(gl[t]);
                    } else {
                        down.push_back(gl[t]);
                    }
                    right = (std::max)(right, gl[t]->right);
                }

                std::vector<RuleSegment> up_rules;
                std::vector<RuleSegment> down_rules;
                for (std::size_t ri = 0U; ri < rules.size(); ++ri) {
                    if (used_rule[ri] != 0 || !rules[ri].horizontal) {
                        continue;
                    }
                    const double rx0 = (std::min)(rules[ri].x0, rules[ri].x1);
                    const double rx1 = (std::max)(rules[ri].x0, rules[ri].x1);
                    const double ry = (rules[ri].y0 + rules[ri].y1) * 0.5;
                    if (rx1 < bx0 - hpad || rx0 > bx1 + hpad ||
                        std::fabs(ry - by) > max_vdist) {
                        continue;
                    }
                    if (ry > by + kSubRuleVertClearancePt) {
                        up_rules.push_back(rules[ri]);
                        used_rule[ri] = 1;
                    } else if (ry < by - kSubRuleVertClearancePt) {
                        down_rules.push_back(rules[ri]);
                        used_rule[ri] = 1;
                    }
                }

                sep_before(gl[i]->code, (std::min)(gl[i]->left, bx0), space_before[i] != 0);
                const double up_axis = math_axis(up);
                const double down_axis = math_axis(down);
                out += U"\\frac{";
                out += math_to_latex(std::move(up), up_axis, false, depth + 1, up_rules);
                out += U"}{";
                out += math_to_latex(std::move(down), down_axis, false, depth + 1, down_rules);
                out += U'}';
                prev_right = right;
                prev_code = 0U;
                prev_scripted = false;
                ++i;
                continue;
            }
        }

        // (1b) Geometry-only fraction fallback when no rules accompany the span.
        if (rules.empty() && frac_level(gl[i]) != 0) {
            std::size_t j = i;
            bool has_up = false;
            bool has_down = false;
            bool has_big = false;
            while (j < n && used_gl[j] == 0 && frac_level(gl[j]) != 0) {
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
                for (std::size_t t = i; t < j; ++t) {
                    used_gl[t] = 1;
                    if (frac_level(gl[t]) > 0) {
                        up.push_back(gl[t]);
                    } else {
                        down.push_back(gl[t]);
                    }
                    right = (std::max)(right, gl[t]->right);
                }
                sep_before(gl[i]->code, gl[i]->left, space_before[i] != 0);
                const double up_axis = math_axis(up);
                const double down_axis = math_axis(down);
                out += U"\\frac{";
                out += math_to_latex(std::move(up), up_axis, false, depth + 1, rules);
                out += U"}{";
                out += math_to_latex(std::move(down), down_axis, false, depth + 1, rules);
                out += U'}';
                prev_right = right;
                prev_code = 0U;
                prev_scripted = false;
                i = j;
                continue;
            }
            std::vector<const CharInfo*> run;
            run.reserve(j - i);
            for (std::size_t t = i; t < j; ++t) {
                used_gl[t] = 1;
                run.push_back(gl[t]);
            }
            sep_before(gl[i]->code, gl[i]->left, space_before[i] != 0);
            const double run_axis = math_axis(run);
            out += math_to_latex(std::move(run), run_axis, false, depth + 1, rules);
            prev_right = gl[j - 1U]->right;
            prev_code = 0U;
            prev_scripted = false;
            i = j;
            continue;
        }

        // (2) Radical (`\sqrt{...}` / `\sqrt[n]{...}`) with optional hook index and vinculum.
        std::optional<std::size_t> rad_pos;
        if (gl[i]->code == kRadicalCp) {
            rad_pos = i;
        } else {
            for (std::size_t look = i + 1U;
                 look < n && look <= i + kMaxRadicalIndexGlyphs; ++look) {
                if (used_gl[look] != 0) {
                    break;
                }
                if (gl[look]->code == kRadicalCp) {
                    bool all_hook = true;
                    for (std::size_t t = i; t < look; ++t) {
                        if (!is_radical_index_glyph(gl[t], gl[look], axis, base)) {
                            all_hook = false;
                            break;
                        }
                    }
                    if (all_hook) {
                        rad_pos = look;
                    }
                    break;
                }
            }
        }

        if (rad_pos) {
            const CharInfo* rad_c = gl[*rad_pos];
            const double lead_left = gl[i]->left;
            const bool lead_space = space_before[i] != 0;

            std::vector<const CharInfo*> idx_glyphs;
            for (std::size_t t = i; t < *rad_pos; ++t) {
                if (used_gl[t] == 0) {
                    used_gl[t] = 1;
                    idx_glyphs.push_back(gl[t]);
                }
            }
            used_gl[*rad_pos] = 1;
            for (std::size_t t = *rad_pos + 1U;
                 t < n && idx_glyphs.size() < kMaxRadicalIndexGlyphs; ++t) {
                if (used_gl[t] != 0) {
                    continue;
                }
                if (is_radical_index_glyph(gl[t], rad_c, axis, base)) {
                    used_gl[t] = 1;
                    idx_glyphs.push_back(gl[t]);
                } else {
                    break;
                }
            }

            std::optional<std::size_t> best_vinc;
            double best_vdist = kCoordSentinel;
            const double max_rad_top = (std::max)(rad_c->top, axis + base);
            for (std::size_t ri = 0U; ri < rules.size(); ++ri) {
                if (used_rule[ri] != 0 || !rules[ri].horizontal) {
                    continue;
                }
                const double vx0 = (std::min)(rules[ri].x0, rules[ri].x1);
                const double vx1 = (std::max)(rules[ri].x0, rules[ri].x1);
                const double vy = (rules[ri].y0 + rules[ri].y1) * 0.5;
                if (vx0 < rad_c->left - kVinculumStartLeftPadScale * base ||
                    vx0 > rad_c->right + kVinculumStartRightPadScale * base ||
                    vx1 <= rad_c->right) {
                    continue;
                }
                if (vy <= axis + kVinculumMinAxisOffScale * base ||
                    vy < rad_c->top - kVinculumBelowRadTopTolScale * base ||
                    vy > max_rad_top + kVinculumAboveRadTopTolScale * base) {
                    continue;
                }
                const double vdist = std::fabs(vy - rad_c->top);
                if (vdist < best_vdist) {
                    best_vdist = vdist;
                    best_vinc = ri;
                }
            }

            std::vector<const CharInfo*> rad;
            std::vector<RuleSegment> sub_rules;
            double right = rad_c->right;

            if (best_vinc) {
                const RuleSegment& vinc = rules[*best_vinc];
                used_rule[*best_vinc] = 1;
                const double vx0 = (std::min)(vinc.x0, vinc.x1);
                const double vx1 = (std::max)(vinc.x0, vinc.x1);
                const double vy = (vinc.y0 + vinc.y1) * 0.5;
                right = (std::max)(right, vx1);

                for (std::size_t t = *rad_pos + 1U; t < n; ++t) {
                    if (used_gl[t] != 0) {
                        continue;
                    }
                    const double cx = glyph_cx(gl[t]);
                    const double cy = frac_ref_y(gl[t]);
                    if (gl[t]->left >= rad_c->left - 0.10 * base &&
                        cx <= vx1 + kVinculumEndPadScale * base &&
                        cy >= rad_c->bottom - kVinculumBottomPadScale * base &&
                        cy <= vy + kVinculumTopOverhangScale * base) {
                        used_gl[t] = 1;
                        rad.push_back(gl[t]);
                        right = (std::max)(right, gl[t]->right);
                    }
                }
                for (std::size_t ri = 0U; ri < rules.size(); ++ri) {
                    if (used_rule[ri] != 0 || !rules[ri].horizontal) {
                        continue;
                    }
                    const double rx0 = (std::min)(rules[ri].x0, rules[ri].x1);
                    const double rx1 = (std::max)(rules[ri].x0, rules[ri].x1);
                    const double ry = (rules[ri].y0 + rules[ri].y1) * 0.5;
                    if (rx1 >= vx0 - kFracBarHorizPadScale * base &&
                        rx0 <= vx1 + kFracBarHorizPadScale * base &&
                        ry < vy - kSubRuleVertClearancePt &&
                        ry >= rad_c->bottom - kVinculumBottomPadScale * base) {
                        sub_rules.push_back(rules[ri]);
                        used_rule[ri] = 1;
                    }
                }
            } else {
                std::size_t k = *rad_pos + 1U;
                while (k < n && used_gl[k] != 0) {
                    ++k;
                }
                if (k < n && (!rules.empty() || frac_level(gl[k]) == 0)) {
                    const CharInfo* b0 = gl[k];
                    if (is_ascii_alpha(b0->code)) {
                        const bool bold0 = b0->bold;
                        const bool ital0 = b0->italic;
                        while (k < n && used_gl[k] == 0 &&
                               (!rules.empty() || frac_level(gl[k]) == 0) &&
                               script_level(gl[k]) == 0 && is_ascii_alpha(gl[k]->code) &&
                               gl[k]->bold == bold0 && gl[k]->italic == ital0) {
                            used_gl[k] = 1;
                            rad.push_back(gl[k]);
                            ++k;
                        }
                    } else if (is_ascii_digit(b0->code)) {
                        while (k < n && used_gl[k] == 0 &&
                               (!rules.empty() || frac_level(gl[k]) == 0) &&
                               script_level(gl[k]) == 0 && is_ascii_digit(gl[k]->code)) {
                            used_gl[k] = 1;
                            rad.push_back(gl[k]);
                            ++k;
                        }
                    } else {
                        used_gl[k] = 1;
                        rad.push_back(gl[k]);
                        ++k;
                    }
                    while (k < n && used_gl[k] == 0 && script_level(gl[k]) != 0) {
                        used_gl[k] = 1;
                        rad.push_back(gl[k]);
                        ++k;
                    }
                }
                if (!rad.empty()) {
                    right = (std::max)(right, rad.back()->right);
                }
            }

            sep_before(rad_c->code, lead_left, lead_space);
            out += U"\\sqrt";
            if (!idx_glyphs.empty()) {
                const double idx_axis = math_axis(idx_glyphs);
                out += U'[';
                out += math_to_latex(std::move(idx_glyphs), idx_axis, false, depth + 1, {});
                out += U']';
            }
            const double rad_axis = math_axis(rad);
            out += U'{';
            out += math_to_latex(std::move(rad), rad_axis, false, depth + 1, sub_rules);
            out += U'}';
            prev_right = right;
            prev_code = 0U;
            prev_scripted = false;
            ++i;
            continue;
        }

        // (3) An operand at axis level.
        const auto is_axis_operand_glyph = [&](std::size_t idx) noexcept {
            return used_gl[idx] == 0 && (!rules.empty() || frac_level(gl[idx]) == 0) &&
                   script_level(gl[idx]) == 0;
        };
        sep_before(gl[i]->code, gl[i]->left, space_before[i] != 0);
        std::vector<const CharInfo*> op;
        const char32_t c0 = gl[i]->code;
        if (script_level(gl[i]) != 0) {
            used_gl[i] = 1;
            op.push_back(gl[i]);
            ++i;
        } else if (c0 == U'.' && i + 1U < n && used_gl[i + 1U] == 0 &&
                   gl[i + 1U]->code == U'.' && is_axis_operand_glyph(i + 1U)) {
            const CharInfo* last = gl[i];
            while (i < n && used_gl[i] == 0 && gl[i]->code == U'.' &&
                   (!rules.empty() || frac_level(gl[i]) == 0)) {
                used_gl[i] = 1;
                last = gl[i];
                ++i;
            }
            out += U"\\ldots ";
            prev_right = last->right;
            prev_code = 0U;
            continue;
        } else if (is_ascii_alpha(c0)) {
            const bool bold0 = gl[i]->bold;
            const bool ital0 = gl[i]->italic;
            while (i < n && is_axis_operand_glyph(i) && is_ascii_alpha(gl[i]->code) &&
                   gl[i]->bold == bold0 && gl[i]->italic == ital0 &&
                   (op.empty() ||
                    gl[i]->left - op.back()->right <= kOperandTightGapScale * base)) {
                used_gl[i] = 1;
                op.push_back(gl[i]);
                ++i;
            }
        } else if (is_ascii_digit(c0)) {
            while (i < n && is_axis_operand_glyph(i)) {
                const char32_t cc = gl[i]->code;
                const bool digit = is_ascii_digit(cc);
                const bool inner = (cc == U'.' || cc == U',') && !op.empty() && i + 1U < n &&
                                   used_gl[i + 1U] == 0 && is_ascii_digit(gl[i + 1U]->code);
                if ((!digit && !inner) ||
                    (!op.empty() &&
                     gl[i]->left - op.back()->right > kOperandTightGapScale * base)) {
                    break;
                }
                used_gl[i] = 1;
                op.push_back(gl[i]);
                ++i;
            }
        } else {
            used_gl[i] = 1;
            op.push_back(gl[i]);
            ++i;
        }

        bool has_sup = false;
        for (std::size_t k = i; k < n && used_gl[k] == 0 && script_level(gl[k]) != 0; ++k) {
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
        while (i < n && used_gl[i] == 0 && script_level(gl[i]) != 0) {
            used_gl[i] = 1;
            if (script_level(gl[i]) == 1) {
                sups.push_back(gl[i]);
            } else {
                subs.push_back(gl[i]);
            }
            prev_right = (std::max)(prev_right, gl[i]->right);
            ++i;
        }
        if (!subs.empty() || !sups.empty()) {
            trim_trailing_spaces(out);
        }
        const bool had_scripts = !subs.empty() || !sups.empty();
        if (!subs.empty()) {
            const double sub_axis = math_axis(subs);
            append_wrapped_script(
                out, U'_', math_to_latex(std::move(subs), sub_axis, false, depth + 1, {}));
        }
        if (!sups.empty()) {
            const double sup_axis = math_axis(sups);
            append_wrapped_script(
                out, U'^', math_to_latex(std::move(sups), sup_axis, false, depth + 1, {}));
        }
        prev_scripted = had_scripts;
    }
    trim_trailing_spaces(out);
    return out;
}

void render_math(Line& line, std::span<const Emit> emits) {
    line.plain.reserve(emits.size() + emits.size() / 5U);
    for (const Emit& e : emits) {
        if (e.space_before && !line.plain.empty()) {
            line.plain += U' ';
        }
        line.plain += line.chars[e.idx].code;
    }

    const std::size_t m = emits.size();
    std::vector<int> role(m, 0);
    std::vector<char> core(m, 0);
    std::vector<char> prose(m, 0);
    const auto glyph = [&line, emits](std::size_t k) -> const CharInfo& {
        return line.chars[emits[k].idx];
    };
    const auto tight_prev = [&line, &glyph](std::size_t k) noexcept {
        return k > 0U && glyph(k).left - glyph(k - 1U).right <=
                             kInlineTightGapScale * (std::max)(line.size, kMinFontSizePt);
    };
    for (std::size_t k = 0U; k < m; ++k) {
        role[k] = script_role(line, glyph(k));
    }

    for (std::size_t k = 0U; k < m; ++k) {
        const CharInfo& c = glyph(k);
        if (role[k] != 0 || is_greek_cp(c.code) || is_math_symbol_cp(c.code)) {
            core[k] = 1;
        }
    }
    if (!line.rules.empty()) {
        const double ref_sz = (std::max)(line.size, kMinFontSizePt);
        const double rule_pad = kCoreRuleHorizPadScale * ref_sz;
        for (std::size_t k = 0U; k < m; ++k) {
            const CharInfo& c = glyph(k);
            if (core[k] != 0 || c.link_id != 0U) {
                continue;
            }
            const double cx = glyph_cx(&c);
            for (const RuleSegment& r : line.rules) {
                if (!r.horizontal) {
                    continue;
                }
                const double rx0 = (std::min)(r.x0, r.x1);
                const double rx1 = (std::max)(r.x0, r.x1);
                if (cx >= rx0 - rule_pad && cx <= rx1 + rule_pad) {
                    core[k] = 1;
                    break;
                }
            }
        }
    }
    for (std::size_t k = 0U; k < m; ++k) {
        if (role[k] == 0) {
            continue;
        }
        std::ptrdiff_t j = static_cast<std::ptrdiff_t>(k) - 1;
        while (j >= 0 && role[static_cast<std::size_t>(j)] != 0) {
            --j;
        }
        if (j < 0) {
            continue;
        }
        const std::size_t first_script = static_cast<std::size_t>(j) + 1U;
        if (glyph(first_script).left - glyph(static_cast<std::size_t>(j)).right >
            kScriptBaseMaxGapScale * line.size) {
            continue;
        }
        while (j >= 0 && is_ident_cp(glyph(static_cast<std::size_t>(j)).code)) {
            core[static_cast<std::size_t>(j)] = 1;
            if (!tight_prev(static_cast<std::size_t>(j))) {
                break;
            }
            --j;
        }
    }
    for (std::size_t k = 0U; k < m;) {
        if (!is_ascii_alpha(glyph(k).code)) {
            ++k;
            continue;
        }
        std::size_t e = k;
        bool any_core = core[k] != 0;
        while (e + 1U < m && is_ascii_alpha(glyph(e + 1U).code) && tight_prev(e + 1U)) {
            ++e;
            any_core = any_core || (core[e] != 0);
        }
        if (e > k && !any_core) {
            for (std::size_t t = k; t <= e; ++t) {
                prose[t] = 1;
            }
        }
        k = e + 1U;
    }
    for (std::size_t k = 0U; k < m; ++k) {
        const CharInfo& c = glyph(k);
        if (c.bold && is_ascii_alpha(c.code) && !prose[k] && c.link_id == 0U &&
            (k == 0U || !glyph(k - 1U).bold) && (k + 1U >= m || !glyph(k + 1U).bold)) {
            core[k] = 1;
        }
    }
    const auto eligible = [&core, &prose, &glyph](std::size_t k) noexcept {
        if (core[k] != 0) {
            return true;
        }
        if (prose[k] != 0) {
            return false;
        }
        const CharInfo& ch = glyph(k);
        if (ch.link_id != 0U) {
            return false;
        }
        return is_math_connector_cp(ch.code) || is_letter_cp(ch.code);
    };

    const auto prose_matches = [](const StyledRun& r, const CharInfo& c) noexcept {
        return !r.math && r.bold == c.bold && r.italic == c.italic && r.mono == c.mono &&
               r.strikethrough == c.strikethrough && r.link_id == c.link_id;
    };
    const auto push_prose = [&line, emits, &glyph, &prose_matches](std::size_t k) {
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
            run.strikethrough = c.strikethrough;
            run.link_id = c.link_id;
            line.runs.push_back(std::move(run));
        } else if (sp) {
            line.runs.back().text += U' ';
        }
        line.runs.back().text += c.code;
    };
    const auto bracket_balance = [&glyph](std::size_t a, std::size_t b) noexcept {
        int bal = 0;
        for (std::size_t t = a; t <= b; ++t) {
            const char32_t c = glyph(t).code;
            if (c == U'(' || c == U'[') {
                ++bal;
            } else if (c == U')' || c == U']') {
                --bal;
            }
        }
        return bal;
    };
    const auto is_edge_punct = [](char32_t c) noexcept {
        return c == U'.' || c == U',' || c == U';' || c == U':';
    };

    std::size_t k = 0U;
    while (k < m) {
        if (!eligible(k)) {
            push_prose(k);
            ++k;
            continue;
        }
        const std::size_t s = k;
        std::size_t e = k;
        while (e + 1U < m && eligible(e + 1U)) {
            ++e;
        }

        std::size_t fc = s;
        std::size_t lc = s;
        bool found = false;
        for (std::size_t t = s; t <= e; ++t) {
            if (core[t] != 0) {
                if (!found) {
                    fc = t;
                }
                lc = t;
                found = true;
            }
        }
        if (!found) {
            for (std::size_t t = s; t <= e; ++t) {
                push_prose(t);
            }
            k = e + 1U;
            continue;
        }

        std::size_t a = s;
        std::size_t b = e;
        while (b > lc) {
            const char32_t c = glyph(b).code;
            if (is_edge_punct(c) ||
                ((c == U')' || c == U']') && bracket_balance(a, b) < 0)) {
                --b;
                continue;
            }
            if (is_dashy_cp(c) && b == e && e + 1U < m && prose[e + 1U] != 0) {
                --b;
                continue;
            }
            if (c == U']') {
                std::ptrdiff_t o = static_cast<std::ptrdiff_t>(b) - 1;
                bool numeric = true;
                bool digit = false;
                while (o > static_cast<std::ptrdiff_t>(lc) &&
                       glyph(static_cast<std::size_t>(o)).code != U'[') {
                    const char32_t cc = glyph(static_cast<std::size_t>(o)).code;
                    if (is_ascii_digit(cc)) {
                        digit = true;
                    } else if (cc != U',' && cc != U' ') {
                        numeric = false;
                        break;
                    }
                    --o;
                }
                if (numeric && digit && o > static_cast<std::ptrdiff_t>(lc) &&
                    glyph(static_cast<std::size_t>(o)).code == U'[') {
                    b = static_cast<std::size_t>(o) - 1U;
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

        for (std::size_t t = s; t < a; ++t) {
            push_prose(t);
        }
        if (emits[a].space_before && !line.runs.empty()) {
            line.runs.back().text += U' ';
        }
        std::vector<const CharInfo*> span;
        const std::size_t lo = emits[a].idx;
        const std::size_t hi = emits[b].idx;
        span.reserve(hi - lo + 1U);
        bool all_strike = true;
        bool has_non_space = false;
        for (std::size_t idx = lo; idx <= hi; ++idx) {
            span.push_back(&line.chars[idx]);
            if (!line.chars[idx].space) {
                has_non_space = true;
                all_strike = all_strike && line.chars[idx].strikethrough;
            }
        }
        StyledRun run;
        run.math = true;
        run.strikethrough = has_non_space && all_strike;
        run.text = math_to_latex(std::move(span), line.baseline, /*top_level=*/true, 0, line.rules);
        line.runs.push_back(std::move(run));
        for (std::size_t t = b + 1U; t <= e; ++t) {
            push_prose(t);
        }
        k = e + 1U;
    }
}

void normalize_inline_punct(Line& line) {
    for (std::size_t i = 1U; i + 1U < line.chars.size(); ++i) {
        CharInfo& c = line.chars[i];
        if (c.space) {
            continue;
        }
        const CharInfo& p = line.chars[i - 1U];
        const CharInfo& q = line.chars[i + 1U];
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
    if (!line.rules.empty()) {
        return true;
    }
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
        return r.math && match_prose_superscript_refs(r.text).has_value();
    });
    if (!any) {
        return;
    }
    std::vector<StyledRun> out;
    out.reserve(line.runs.size() + 4U);
    for (StyledRun& r : line.runs) {
        if (!r.math) {
            out.push_back(std::move(r));
            continue;
        }
        const auto m = match_prose_superscript_refs(r.text);
        if (!m) {
            out.push_back(std::move(r));
            continue;
        }
        for (std::size_t idx = 0U; idx < m->items.size(); ++idx) {
            const UnwrappedProseRefItem& item = m->items[idx];
            const bool is_first = idx == 0U;
            const bool is_last = idx + 1U == m->items.size();
            const bool split_prefix = !item.prefix.empty() && item.prefix_bold != item.bold;

            if (split_prefix) {
                StyledRun pre;
                pre.bold = item.prefix_bold;
                pre.strikethrough = r.strikethrough;
                pre.link_id = r.link_id;
                if (is_first) {
                    pre.text.append(m->lead_spaces, U' ');
                }
                pre.text.append(item.prefix);
                out.push_back(std::move(pre));
            }

            StyledRun word;
            word.bold = item.bold;
            word.strikethrough = r.strikethrough;
            word.link_id = r.link_id;
            const bool attach_lead = is_first && !split_prefix;
            const bool attach_prefix = !item.prefix.empty() && !split_prefix;
            word.text.reserve((attach_lead ? m->lead_spaces : 0U) +
                              (attach_prefix ? item.prefix.size() : 0U) + item.word.size());
            if (attach_lead) {
                word.text.append(m->lead_spaces, U' ');
            }
            if (attach_prefix) {
                word.text.append(item.prefix);
            }
            word.text.append(item.word);
            out.push_back(std::move(word));

            StyledRun sup;
            sup.strikethrough = r.strikethrough;
            sup.link_id = r.link_id;
            if (item.numeric_footnote) {
                sup.raw = true;
                sup.text.reserve(3U + item.marker.size() + item.sep_after.size() +
                                 (is_last ? m->trail_spaces : 0U));
                sup.text += U"[^";
                sup.text.append(item.marker);
                sup.text += U']';
            } else {
                sup.text.reserve(item.marker.size() + item.sep_after.size() +
                                 (is_last ? m->trail_spaces : 0U));
                sup.text.append(item.marker);
            }
            if (!item.sep_after.empty()) {
                if (sup.raw) {
                    out.push_back(std::move(sup));
                    StyledRun sep;
                    sep.strikethrough = r.strikethrough;
                    sep.link_id = r.link_id;
                    sep.text = item.sep_after;
                    if (is_last) {
                        sep.text.append(m->trail_spaces, U' ');
                    }
                    out.push_back(std::move(sep));
                    continue;
                }
                sup.text.append(item.sep_after);
            }
            if (is_last) {
                sup.text.append(m->trail_spaces, U' ');
            }
            out.push_back(std::move(sup));
        }
    }
    line.runs = std::move(out);
}

void repair_token_spaces(const Line& line, std::vector<Emit>& emits) {
    const double cgap = column_gap_threshold(line.size);
    const auto glyph_space_between = [&line](std::size_t lo, std::size_t hi) noexcept {
        for (std::size_t i = lo + 1U; i < hi; ++i) {
            if (line.chars[i].space) {
                return true;
            }
        }
        return false;
    };
    for (std::size_t k = 1U; k < emits.size(); ++k) {
        if (!emits[k].space_before) {
            continue;
        }
        const CharInfo& a = line.chars[emits[k - 1U].idx];
        const CharInfo& b = line.chars[emits[k].idx];
        if (b.left - a.right > cgap) {
            continue;  // a real column break: leave it
        }
        const char32_t pc = a.code;
        const char32_t cc = b.code;
        const char32_t nc = emits[k].idx + 1U < line.chars.size()
                                ? line.chars[emits[k].idx + 1U].code
                                : U'\0';
        bool drop = false;
        if (is_ascii_digit(pc) && is_ascii_digit(cc)) {
            drop = !glyph_space_between(emits[k - 1U].idx, emits[k].idx);
        } else if ((cc == U'.' || cc == U',' || cc == U':' || cc == U';') &&
                   (is_ascii_alpha(pc) || is_ascii_digit(pc)) && !(cc == U':' && nc == U'=')) {
            drop = true;
        } else if (cc == U':' && pc == U':') {
            drop = true;
        } else if (pc == U':' && k >= 2U && line.chars[emits[k - 2U].idx].code == U':') {
            drop = true;
        } else if (cc == U'-' && (is_ascii_alpha(pc) || is_ascii_digit(pc)) &&
                   (is_ascii_alpha(nc) || is_ascii_digit(nc)) &&
                   !glyph_space_between(emits[k - 1U].idx, emits[k].idx)) {
            drop = true;
        }
        if (drop) {
            emits[k].space_before = false;
        }
    }
}

void build_runs(Line& line) {
    line.runs.clear();
    line.plain.clear();
    line.segments.clear();

    normalize_inline_punct(line);
    std::vector<Emit> emits = plan_line(line);
    repair_token_spaces(line, emits);
    if (line_needs_math(line)) {
        render_math(line, emits);
        rewrite_footnote_refs(line);
    } else {
        render_plain(line, emits);
    }
}

std::vector<Line> build_lines(std::span<const CharInfo* const> chars) {
    std::vector<Line> lines = cluster_lines(chars);
    std::erase_if(lines, [](Line& ln) {
        build_runs(ln);
        return ln.plain.empty();
    });
    return lines;
}

}  // namespace pdf2md::layout
