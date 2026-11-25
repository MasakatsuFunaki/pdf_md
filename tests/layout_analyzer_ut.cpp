#include "pdf2md/layout_analyzer.h"

#include "pdf2md/document_model.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

constexpr double kPageWidth = 612.0;
constexpr double kPageHeight = 792.0;
constexpr double kLeftMargin = 72.0;
constexpr double kBodySize = 10.0;
constexpr double kScriptSize = 7.0;
constexpr double kSuperscriptDy = 3.6;
constexpr double kSubscriptDy = -1.5;
constexpr double kGlyphWidthRatio = 0.5;
constexpr double kGlyphAdvanceRatio = 0.6;
constexpr double kInterTokenGap = 8.0;

struct ExpectedRun {
    std::u32string_view text;
    bool bold = false;
    bool italic = false;
    bool mono = false;
    bool math = false;
    bool raw = false;
};

constexpr ExpectedRun plain_run(std::u32string_view text) noexcept {
    return {text, false, false, false, false, false};
}

constexpr ExpectedRun bold_run(std::u32string_view text) noexcept {
    return {text, true, false, false, false, false};
}

constexpr ExpectedRun italic_run(std::u32string_view text) noexcept {
    return {text, false, true, false, false, false};
}

constexpr ExpectedRun mono_run(std::u32string_view text) noexcept {
    return {text, false, false, true, false, false};
}

constexpr ExpectedRun math_run(std::u32string_view text) noexcept {
    return {text, false, false, false, true, false};
}

constexpr ExpectedRun raw_run(std::u32string_view text) noexcept {
    return {text, false, false, false, false, true};
}

void expect_runs(const std::vector<pdf2md::StyledRun>& actual,
                 const std::vector<ExpectedRun>& expected) {
    ASSERT_EQ(actual.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(actual[i].text, std::u32string(expected[i].text));
        EXPECT_EQ(actual[i].bold, expected[i].bold);
        EXPECT_EQ(actual[i].italic, expected[i].italic);
        EXPECT_EQ(actual[i].mono, expected[i].mono);
        EXPECT_EQ(actual[i].math, expected[i].math);
        EXPECT_EQ(actual[i].raw, expected[i].raw);
    }
}

std::u32string join_cell_runs(const pdf2md::TableCell& cell) {
    std::u32string out;
    for (const pdf2md::StyledRun& run : cell) {
        out += run.text;
    }
    return out;
}

void expect_table_cells(const pdf2md::Block& block,
                        const std::vector<std::vector<std::u32string>>& expected) {
    ASSERT_EQ(block.kind, pdf2md::BlockKind::Table);
    ASSERT_EQ(block.table_cells.size(), expected.size());
    for (size_t r = 0; r < expected.size(); ++r) {
        SCOPED_TRACE(r);
        ASSERT_EQ(block.table_cells[r].size(), expected[r].size());
        for (size_t c = 0; c < expected[r].size(); ++c) {
            SCOPED_TRACE(c);
            EXPECT_EQ(join_cell_runs(block.table_cells[r][c]), expected[r][c]);
        }
    }
}

void expect_block_lines(const pdf2md::Block& block, pdf2md::BlockKind expected_kind,
                        const std::vector<std::u32string>& expected_lines) {
    EXPECT_EQ(block.kind, expected_kind);
    ASSERT_EQ(block.lines.size(), expected_lines.size());
    for (size_t i = 0; i < expected_lines.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(block.lines[i].plain, expected_lines[i]);
    }
}

// Builds a single-line page where each inter-word break space shares the exact
// origin x of the next word's first glyph and is emitted after that glyph.
pdf2md::PageData make_coincident_space_line(const std::vector<std::string>& words, double y) {
    pdf2md::PageData page;
    page.width = kPageWidth;
    page.height = kPageHeight;
    double x = kLeftMargin;
    auto push = [&](char32_t code, double left, bool space) {
        pdf2md::CharInfo c;
        c.code = space ? U' ' : code;
        c.x = left;
        c.left = left;
        c.right = left + (space ? 0.0 : 5.0);
        c.y = y;
        c.bottom = y;
        c.top = y + kBodySize;
        c.size = kBodySize;
        c.space = space;
        page.chars.push_back(c);
    };
    for (size_t w = 0; w < words.size(); ++w) {
        for (size_t k = 0; k < words[w].size(); ++k) {
            const auto cp = static_cast<char32_t>(static_cast<unsigned char>(words[w][k]));
            if (w > 0 && k == 0) {
                push(cp, x, false);
                push(U' ', x, true);
            } else {
                push(cp, x, false);
            }
            x += 6.0;
        }
    }
    return page;
}

struct MGlyph {
    char32_t code = 0;
    int role = 0;  // 0 = body, 1 = superscript, 2 = subscript
    bool italic = true;
    bool bold = false;
};

constexpr MGlyph up(char32_t code, int role = 0) noexcept {
    return {code, role, false, false};
}

pdf2md::PageData make_math_line(const std::vector<std::vector<MGlyph>>& tokens, double y) {
    pdf2md::PageData page;
    page.width = kPageWidth;
    page.height = kPageHeight;
    double x = kLeftMargin;
    for (size_t t = 0; t < tokens.size(); ++t) {
        if (t > 0) {
            x += kInterTokenGap;
        }
        for (const MGlyph& g : tokens[t]) {
            pdf2md::CharInfo c;
            const double size = (g.role == 0) ? kBodySize : kScriptSize;
            const double dy = (g.role == 1) ? kSuperscriptDy
                                            : ((g.role == 2) ? kSubscriptDy : 0.0);
            c.code = g.code;
            c.size = size;
            c.x = x;
            c.left = x;
            c.right = x + size * kGlyphWidthRatio;
            c.y = y + dy;
            c.bottom = c.y;
            c.top = c.y + size;
            c.italic = g.italic;
            c.bold = g.bold;
            page.chars.push_back(c);
            x += size * kGlyphAdvanceRatio;
        }
    }
    return page;
}

struct FreeGlyph {
    char32_t code = 0;
    double left = 0;
    double baseline = 0;
    double size = kBodySize;
    bool italic = false;
    bool bold = false;
    bool raised = false;
    bool mono = false;
};

pdf2md::PageData make_free_page(const std::vector<FreeGlyph>& glyphs) {
    pdf2md::PageData page;
    page.width = kPageWidth;
    page.height = kPageHeight;
    for (const FreeGlyph& g : glyphs) {
        pdf2md::CharInfo c;
        c.code = g.code;
        c.size = g.size;
        c.x = g.left;
        c.left = g.left;
        c.right = g.left + (g.code == U' ' ? 0.0 : g.size * kGlyphWidthRatio);
        c.y = g.baseline;
        c.italic = g.italic;
        c.bold = g.bold;
        c.mono = g.mono;
        c.space = (g.code == U' ');
        if (g.raised) {
            c.top = g.baseline + g.size / 2.0;
            c.bottom = g.baseline - g.size / 2.0;
            c.y = c.top;
        } else {
            c.bottom = g.baseline;
            c.top = g.baseline + g.size;
        }
        page.chars.push_back(c);
    }
    return page;
}

void put_token(std::vector<FreeGlyph>& gs, std::string_view s, double x, double y,
               double size = kBodySize, bool bold = false, bool mono = false) {
    for (char ch : s) {
        const auto cp = static_cast<char32_t>(static_cast<unsigned char>(ch));
        gs.push_back({cp, x, y, size, false, bold, false, mono});
        x += size * kGlyphAdvanceRatio;
    }
}

double put_tight(std::vector<FreeGlyph>& gs, std::string_view s, double x, double y,
                 double size = kBodySize) {
    for (char ch : s) {
        const auto cp = static_cast<char32_t>(static_cast<unsigned char>(ch));
        gs.push_back({cp, x, y, size});
        x += size * kGlyphAdvanceRatio;
    }
    return x;
}

void put_body(std::vector<FreeGlyph>& gs, int lines, double top) {
    for (int line = 0; line < lines; ++line) {
        const double y = top - static_cast<double>(line) * 12.0;
        double x = kLeftMargin;
        for (int w = 0; w < 9; ++w) {
            put_token(gs, "bodyword", x, y, kBodySize);
            x += 56.0;
        }
    }
}

pdf2md::Document analyze_page(const pdf2md::PageData& page,
                              const pdf2md::AnalyzeParams& params = {}) {
    pdf2md::DocMeta meta;
    meta.page_count = 1;
    return pdf2md::analyze_layout(meta, {page}, params);
}

pdf2md::Document analyze_pages(std::vector<pdf2md::PageData> pages,
                               const pdf2md::AnalyzeParams& params = {}) {
    pdf2md::DocMeta meta;
    meta.page_count = static_cast<int>(pages.size());
    for (size_t i = 0; i < pages.size(); ++i) {
        pages[i].index = static_cast<int>(i);
    }
    return pdf2md::analyze_layout(meta, pages, params);
}

void hrule(pdf2md::PageData& p, double y, double x0, double x1) {
    p.rules.push_back({x0, y, x1, y, true});
}

void vrule(pdf2md::PageData& p, double x, double y0, double y1) {
    p.rules.push_back({x, y0, x, y1, false});
}

void draw_grid(pdf2md::PageData& p, const std::vector<double>& cols,
               const std::vector<double>& rows) {
    for (double y : rows) {
        hrule(p, y, cols.front(), cols.back());
    }
    for (double x : cols) {
        vrule(p, x, rows.back(), rows.front());
    }
}

pdf2md::PageData ruled_table_page(const std::vector<std::array<std::string, 3>>& rows,
                                  bool first_row_bold) {
    pdf2md::PageData p;
    p.width = kPageWidth;
    p.height = kPageHeight;
    const std::vector<double> cols = {71.0, 210.0, 350.0, 524.0};
    std::vector<double> rails = {720.0};
    std::vector<FreeGlyph> gs;
    double y = 706.0;
    for (size_t r = 0; r < rows.size(); ++r) {
        for (size_t c = 0; c < 3; ++c) {
            if (!rows[r][c].empty()) {
                put_token(gs, rows[r][c], cols[c] + 5.0, y, kBodySize,
                          first_row_bold && r == 0);
            }
        }
        rails.push_back(y + 6.0 - 20.0);
        y -= 20.0;
    }
    p.chars = make_free_page(gs).chars;
    draw_grid(p, cols, rails);
    return p;
}

TEST(LayoutSpacing, CoincidentBreakSpaceStaysBeforeItsGlyph) {
    const pdf2md::Document doc =
        analyze_page(make_coincident_space_line({"we", "compute", "of", "the", "sum"}, 700.0));
    ASSERT_EQ(doc.blocks.size(), 1u);
    EXPECT_EQ(doc.blocks[0].kind, pdf2md::BlockKind::Paragraph);
    ASSERT_EQ(doc.blocks[0].lines.size(), 1u);
    expect_runs(doc.blocks[0].lines[0].runs, {plain_run(U"we compute of the sum")});
}

TEST(LayoutSpacing, IntraTokenGeometricGapVsExplicitSpaceSeams) {
    struct GapCase {
        const char* name;
        const char* lead_word;
        const char* left_part;
        bool explicit_space;
        double gap;
        const char* right_part;
        std::u32string_view expected;
    };
    constexpr GapCase kCases[] = {
        {"geometric_digit_split_rejoins", "revision", "R25-1", false, 3.4, "1",
         U"revision R25-11"},
        {"explicit_space_between_numbers_preserved", "version", "1.40", true, 3.0, "20131220",
         U"version 1.40 20131220"},
        {"geometric_hyphen_split_rejoins", "", "BS_V1", false, 3.4, "-0-0_PR", U"BS_V1-0-0_PR"},
        {"explicit_space_before_hyphen_range_preserved", "pages", "5", true, 3.0, "-9",
         U"pages 5 -9"},
    };

    for (const GapCase& tc : kCases) {
        SCOPED_TRACE(tc.name);
        std::vector<FreeGlyph> gs;
        double x = kLeftMargin;
        if (tc.lead_word[0] != '\0') {
            x = put_tight(gs, tc.lead_word, x, 700.0);
            x += 3.0;
        }
        x = put_tight(gs, tc.left_part, x, 700.0);
        if (tc.explicit_space) {
            gs.push_back({U' ', x, 700.0});
        }
        x += tc.gap;
        put_tight(gs, tc.right_part, x, 700.0);

        const pdf2md::Document doc = analyze_page(make_free_page(gs));
        ASSERT_EQ(doc.blocks.size(), 1u);
        ASSERT_EQ(doc.blocks[0].lines.size(), 1u);
        expect_runs(doc.blocks[0].lines[0].runs, {plain_run(tc.expected)});
    }
}

TEST(LayoutSpacing, PunctuationAndOperatorGapBinding) {
    struct BindingCase {
        const char* name;
        std::vector<FreeGlyph> glyphs;
        std::u32string_view expected;
    };

    std::vector<FreeGlyph> comma_gs;
    double x = put_tight(comma_gs, "Amendment", kLeftMargin, 700.0);
    x += 3.0;
    x = put_tight(comma_gs, "1", x, 700.0);
    x += 3.4;
    x = put_tight(comma_gs, ",", x, 700.0);
    x += 3.0;
    put_tight(comma_gs, "Release", x, 700.0);

    std::vector<FreeGlyph> assign_gs;
    x = put_tight(assign_gs, "v6", kLeftMargin, 700.0);
    x += 3.4;
    put_tight(assign_gs, ":= expr here now", x, 700.0);

    std::vector<FreeGlyph> scope_gs;
    x = put_tight(scope_gs, "the", kLeftMargin, 700.0);
    x += 3.0;
    x = put_tight(scope_gs, "app", x, 700.0);
    x += 3.4;
    x = put_tight(scope_gs, ":", x, 700.0);
    x += 3.4;
    x = put_tight(scope_gs, ":", x, 700.0);
    x += 3.4;
    put_tight(scope_gs, "util here", x, 700.0);

    const std::array<BindingCase, 3> cases = {{
        {"spurious_gap_before_comma_dropped", std::move(comma_gs), U"Amendment 1, Release"},
        {"assignment_operator_preserves_leading_space", std::move(assign_gs),
         U"v6 := expr here now"},
        {"scope_operator_binds_both_sides", std::move(scope_gs), U"the app::util here"},
    }};

    for (const BindingCase& tc : cases) {
        SCOPED_TRACE(tc.name);
        const pdf2md::Document doc = analyze_page(make_free_page(tc.glyphs));
        ASSERT_EQ(doc.blocks.size(), 1u);
        ASSERT_EQ(doc.blocks[0].lines.size(), 1u);
        expect_runs(doc.blocks[0].lines[0].runs, {plain_run(tc.expected)});
    }
}

TEST(LayoutMath, InlineScriptsAndBlackboardBecomeLatexWhileProseStaysPlain) {
    const pdf2md::Document prose_doc =
        analyze_page(make_coincident_space_line({"the", "quick", "brown", "fox"}, 700.0));
    ASSERT_EQ(prose_doc.blocks.size(), 1u);
    ASSERT_EQ(prose_doc.blocks[0].lines.size(), 1u);
    expect_runs(prose_doc.blocks[0].lines[0].runs, {plain_run(U"the quick brown fox")});

    const std::vector<MGlyph> at = {up(U'a'), up(U't')};
    const pdf2md::Document math_doc = analyze_page(make_math_line(
        {
            {up(U'l'), up(U'e'), up(U't')},
            {{U'd'}, {U'k', 2}},
            at,
            {{U'Q'}, {U'K'}, {U'T', 1}},
            at,
            {{U'W'}, {U'i', 2}, {U'Q', 1}},
            at,
            {up(U'R'), {U'a', 1}, {0x00D7, 1}, {U'b', 1}},
        },
        700.0));
    ASSERT_EQ(math_doc.blocks.size(), 1u);
    ASSERT_EQ(math_doc.blocks[0].lines.size(), 1u);
    expect_runs(math_doc.blocks[0].lines[0].runs,
                {
                    plain_run(U"let "),
                    math_run(U"d_k "),
                    plain_run(U"at "),
                    math_run(U"QK^T "),
                    plain_run(U"at "),
                    math_run(U"W_i^Q "),
                    plain_run(U"at "),
                    math_run(U"\\mathbb{R}^{a\\times b}"),
                });
    EXPECT_EQ(math_doc.blocks[0].lines[0].plain, U"let dk at QKT at WiQ at Ra\u00D7b");
}

TEST(LayoutMath, InlineMathTokenAndBoundaryClassification) {
    struct InlineMathCase {
        const char* name;
        std::vector<std::vector<MGlyph>> tokens;
        std::vector<ExpectedRun> expected;
    };

    const std::vector<InlineMathCase> cases = {
        {
            "multiletter_upright_subscript_gets_text",
            {{{U'd'}, up(U'm', 2), up(U'o', 2), up(U'd', 2), up(U'e', 2), up(U'l', 2)}},
            {math_run(U"d_{\\text{model}}")},
        },
        {
            "isolated_bold_letter_is_mathbf_vector",
            {{{U'z', 0, false, true}}, {{U'=', 0, false, false}}, {{U'x'}, {U'1', 2}}},
            {math_run(U"\\mathbf{z} = x_1")},
        },
        {
            "tuple_and_ellipsis_stay_in_single_math_span",
            {
                {up(U'l'), up(U'e'), up(U't')},
                {{U'('},
                 {U'x'},
                 {U'1', 2},
                 {U','},
                 {U'.'},
                 {U'.'},
                 {U'.'},
                 {U','},
                 {U'x'},
                 {U'n', 2},
                 {U')'}},
            },
            {plain_run(U"let "), math_run(U"(x_1, \\ldots , x_n)")},
        },
        {
            "decimal_point_between_digits_not_italicized",
            {{{U'x'}, {U'1', 2}}, {up(U'a'), up(U't')}, {up(U'0'), {U'.'}, up(U'9')}},
            {math_run(U"x_1 "), plain_run(U"at 0.9")},
        },
        {
            "superscript_digit_on_prose_word_is_raw_footnote_ref",
            {{up(U's'), up(U'e'), up(U'e')}, {up(U'G'), up(U'P'), up(U'U'), up(U'5', 1)}},
            {plain_run(U"see "), plain_run(U"GPU"), raw_run(U"[^5]")},
        },
        {
            "word_hyphen_stays_outside_math_span",
            {{{U'd'},
              {U'v', 2},
              up(U'-'),
              up(U'd'),
              up(U'i'),
              up(U'm'),
              up(U'e'),
              up(U'n'),
              up(U's'),
              up(U'i'),
              up(U'o'),
              up(U'n')}},
            {math_run(U"d_v"), plain_run(U"-dimension")},
        },
        {
            "italic_function_name_becomes_operator_macro",
            {{{U'l'}, {U'o'}, {U'g'}, {U'k', 2}, {U'('}, {U'n'}, {U')'}}},
            {math_run(U"\\log_k(n)")},
        },
        {
            "trailing_bracketed_citation_leaves_math_span",
            {{{U'd'}, {U'k', 2}}, {up(U'['), up(U'3'), up(U']')}},
            {math_run(U"d_k "), plain_run(U"[3]")},
        },
    };

    for (const InlineMathCase& tc : cases) {
        SCOPED_TRACE(tc.name);
        const pdf2md::Document doc = analyze_page(make_math_line(tc.tokens, 700.0));
        ASSERT_EQ(doc.blocks.size(), 1u);
        ASSERT_EQ(doc.blocks[0].lines.size(), 1u);
        expect_runs(doc.blocks[0].lines[0].runs, tc.expected);
    }
}

TEST(LayoutMath, StackedInlineFractionRadicalAndBigOperatorLimits) {
    struct StackedCase {
        const char* name;
        std::vector<FreeGlyph> glyphs;
        std::vector<ExpectedRun> expected;
    };

    const std::array<StackedCase, 2> cases = {{
        {
            "inline_fraction_and_radical",
            {
                {U'v', 72.0, 700.0},
                {U'a', 78.0, 700.0},
                {U'l', 84.0, 700.0},
                {U' ', 90.0, 700.0},
                {U'1', 100.0, 705.0, 7.0},
                {0x221A, 96.0, 696.0, 7.0, false, false, true},
                {U'y', 105.0, 693.0, 7.0, true},
                {U'.', 112.0, 700.0},
            },
            {plain_run(U"val "), math_run(U"\\frac{1}{\\sqrt{y}}"), plain_run(U".")},
        },
        {
            "big_operator_stacked_limits",
            {
                {U'l', 72.0, 700.0},
                {U'e', 78.0, 700.0},
                {U't', 84.0, 700.0},
                {U' ', 90.0, 700.0},
                {0x2211, 100.0, 700.0, 10.0, false, false, true},
                {U'n', 103.0, 707.0, 6.0},
                {U'i', 103.0, 693.0, 6.0, true},
                {U'x', 113.0, 700.0, 10.0, true},
            },
            {plain_run(U"let "), math_run(U"\\sum_i^n x")},
        },
    }};

    for (const StackedCase& tc : cases) {
        SCOPED_TRACE(tc.name);
        const pdf2md::Document doc = analyze_page(make_free_page(tc.glyphs));
        ASSERT_EQ(doc.blocks.size(), 1u);
        ASSERT_EQ(doc.blocks[0].lines.size(), 1u);
        expect_runs(doc.blocks[0].lines[0].runs, tc.expected);
    }
}

TEST(LayoutMath, CenteredDisplayEquationsReconstructLatexAndTag) {
    auto with_margin_body = [](std::vector<FreeGlyph> eq_glyphs) {
        std::vector<FreeGlyph> gs;
        for (int i = 0; i < 20; ++i) {
            gs.push_back({static_cast<char32_t>(U'a' + (i % 26)),
                          kLeftMargin + static_cast<double>(i) * 6.0, 740.0});
        }
        gs.insert(gs.end(), eq_glyphs.begin(), eq_glyphs.end());
        return gs;
    };

    std::vector<FreeGlyph> frac_eq = {
        {U'z', 250.0, 700.0, 10.0, true}, {U'n', 258.0, 697.0, 6.0, true},
        {U' ', 262.0, 700.0},             {U'=', 266.0, 700.0},
        {U' ', 278.0, 700.0},             {U'a', 286.0, 706.0, 10.0, true},
        {U'b', 286.0, 694.0, 10.0, true}, {U'+', 300.0, 700.0},
        {U'c', 312.0, 700.0, 10.0, true},
    };

    std::vector<FreeGlyph> sin_eq = {
        {U'y', 250.0, 700.0, 10.0, true}, {U'n', 258.0, 697.0, 7.0, true},
        {U'=', 268.0, 700.0},             {U's', 286.0, 700.0, 10.0, true},
        {U'i', 292.0, 700.0, 10.0, true}, {U'n', 298.0, 700.0, 10.0, true},
        {U'(', 304.0, 700.0},             {U'x', 310.0, 700.0, 10.0, true},
        {U')', 316.0, 700.0},
    };

    std::vector<FreeGlyph> text_eq = {
        {U'z', 250.0, 700.0, 10.0, true},
        {U'n', 258.0, 697.0, 7.0, true},
        {U'=', 268.0, 700.0},
    };
    put_token(text_eq, "where", 286.0, 700.0);
    put_token(text_eq, "head", 330.0, 700.0);
    text_eq.push_back({U'i', 354.0, 697.0, 7.0});

    std::vector<FreeGlyph> tagged_full;
    put_body(tagged_full, 4, 764.0);
    for (FreeGlyph g : sin_eq) {
        g.baseline += 6.0;
        tagged_full.push_back(g);
    }
    put_token(tagged_full, "(1)", 500.0, 706.0);
    put_body(tagged_full, 4, 688.0);

    struct DisplayCase {
        const char* name;
        std::vector<FreeGlyph> glyphs;
        size_t expected_blocks;
        std::u32string_view expected_latex;
        std::u32string_view expected_tag;
    };

    const std::array<DisplayCase, 4> cases = {{
        {"centered_fraction", with_margin_body(std::move(frac_eq)), 2u, U"z_n = \\frac{a}{b}+c",
         U""},
        {"italic_sin_macro", with_margin_body(std::move(sin_eq)), 2u, U"y_n = \\sin(x)", U""},
        {"adjacent_text_groups_keep_space", with_margin_body(std::move(text_eq)), 2u,
         U"z_n = \\text{where }\\text{head}_i", U""},
        {"equation_number_extracted_to_tag", std::move(tagged_full), 3u, U"y_n = \\sin(x)", U"1"},
    }};

    for (const DisplayCase& tc : cases) {
        SCOPED_TRACE(tc.name);
        const pdf2md::Document doc = analyze_page(make_free_page(tc.glyphs));
        ASSERT_EQ(doc.blocks.size(), tc.expected_blocks);
        EXPECT_EQ(doc.blocks[0].kind, pdf2md::BlockKind::Paragraph);
        EXPECT_EQ(doc.blocks[1].kind, pdf2md::BlockKind::MathDisplay);
        ASSERT_EQ(doc.blocks[1].math_lines.size(), 1u);
        EXPECT_EQ(doc.blocks[1].math_lines[0], std::u32string(tc.expected_latex));
        EXPECT_EQ(doc.blocks[1].math_tag, std::u32string(tc.expected_tag));
    }
}

TEST(LayoutBlocks, EmptyAndMinimalPageBoundaries) {
    pdf2md::DocMeta meta{"Spec Title", "Spec Author", 0};
    const pdf2md::Document empty_doc = pdf2md::analyze_layout(meta, {}, {});
    EXPECT_EQ(empty_doc.meta.title, "Spec Title");
    EXPECT_EQ(empty_doc.meta.author, "Spec Author");
    EXPECT_EQ(empty_doc.meta.page_count, 0);
    ASSERT_EQ(empty_doc.blocks.size(), 0u);

    pdf2md::PageData blank_page;
    blank_page.width = kPageWidth;
    blank_page.height = kPageHeight;
    const pdf2md::Document blank_doc = analyze_page(blank_page);
    ASSERT_EQ(blank_doc.blocks.size(), 0u);

    // True single-glyph page surrounded by empty pages.
    const pdf2md::PageData one_glyph_page = make_free_page({{U'X', 72.0, 700.0, 12.0}});
    const pdf2md::Document one_glyph_doc =
        analyze_pages({blank_page, one_glyph_page, blank_page});
    ASSERT_EQ(one_glyph_doc.blocks.size(), 1u);
    EXPECT_EQ(one_glyph_doc.blocks[0].page, 1);
    EXPECT_DOUBLE_EQ(one_glyph_doc.body_size, 12.0);
    expect_block_lines(one_glyph_doc.blocks[0], pdf2md::BlockKind::Paragraph, {U"X"});
    expect_runs(one_glyph_doc.blocks[0].lines[0].runs, {plain_run(U"X")});

    // Sign-bit (0x7F/0x80/0xFF) and supplementary-plane codepoint boundaries, plus
    // inline bold/italic/mono StyledRun splitting inside one line.
    const pdf2md::PageData seam_page = make_free_page({
        {0x007E, 72.0, 700.0, 10.0},
        {0x007F, 78.0, 700.0, 10.0},
        {0x0080, 84.0, 700.0, 10.0},
        {0x00FF, 90.0, 700.0, 10.0},
        {0x1D538, 96.0, 700.0, 10.0},
        {U' ', 102.0, 700.0, 10.0},
        {U'B', 106.0, 700.0, 10.0, false, true, false, false},
        {U' ', 112.0, 700.0, 10.0},
        {U'I', 116.0, 700.0, 10.0, true, false, false, false},
        {U' ', 122.0, 700.0, 10.0},
        {U'M', 126.0, 700.0, 10.0, false, false, false, true},
    });
    const pdf2md::Document seam_doc = analyze_page(seam_page);
    ASSERT_EQ(seam_doc.blocks.size(), 1u);
    ASSERT_EQ(seam_doc.blocks[0].lines.size(), 1u);
    expect_runs(seam_doc.blocks[0].lines[0].runs,
                {
                    plain_run(U"~\u007F\u0080\u00FF\U0001D538 "),
                    bold_run(U"B "),
                    italic_run(U"I "),
                    mono_run(U"M"),
                });
}

TEST(LayoutBlocks, PdfBottomLeftYUpAndTwoColumnReadingOrder) {
    // Single-column page with bottom paragraph and rightmost word pushed first.
    std::vector<FreeGlyph> single_col;
    put_token(single_col, "BottomLine", 72.0, 200.0);
    put_token(single_col, "Second", 105.0, 700.0);
    put_token(single_col, "First", 72.0, 700.0);
    const pdf2md::Document single_doc = analyze_page(make_free_page(single_col));
    ASSERT_EQ(single_doc.blocks.size(), 2u);
    expect_block_lines(single_doc.blocks[0], pdf2md::BlockKind::Paragraph, {U"First Second"});
    ASSERT_EQ(single_doc.blocks[0].lines[0].chars.size(), 11u);
    EXPECT_EQ(single_doc.blocks[0].lines[0].chars[0].code, U'F');
    EXPECT_EQ(single_doc.blocks[0].lines[0].chars[5].code, U'S');
    expect_block_lines(single_doc.blocks[1], pdf2md::BlockKind::Paragraph, {U"BottomLine"});
    EXPECT_DOUBLE_EQ(single_doc.blocks[0].y_top, 710.0);
    EXPECT_DOUBLE_EQ(single_doc.blocks[1].y_top, 210.0);

    // Two-column page with wide multi-word prose columns pushed right-before-left at
    // every baseline: XY-cut reads the entire left column before the right column,
    // keeping sentence-terminated columns as two separate paragraphs.
    std::vector<FreeGlyph> two_col;
    for (int i = 0; i < 6; ++i) {
        const double y = 740.0 - static_cast<double>(i) * 12.0;
        const std::string right_line =
            (i == 5) ? "RightFinalSentenceEnds." : ("RightProseColumnLine" + std::to_string(i));
        const std::string left_line =
            (i == 5) ? "LeftFinalSentenceEnds." : ("LeftProseColumnLine" + std::to_string(i));
        put_tight(two_col, right_line, 330.0, y);
        put_tight(two_col, left_line, 72.0, y);
    }
    const pdf2md::Document two_col_doc = analyze_page(make_free_page(two_col));
    ASSERT_EQ(two_col_doc.blocks.size(), 2u);
    expect_block_lines(
        two_col_doc.blocks[0], pdf2md::BlockKind::Paragraph,
        {U"LeftProseColumnLine0", U"LeftProseColumnLine1", U"LeftProseColumnLine2",
         U"LeftProseColumnLine3", U"LeftProseColumnLine4", U"LeftFinalSentenceEnds."});
    expect_block_lines(
        two_col_doc.blocks[1], pdf2md::BlockKind::Paragraph,
        {U"RightProseColumnLine0", U"RightProseColumnLine1", U"RightProseColumnLine2",
         U"RightProseColumnLine3", U"RightProseColumnLine4", U"RightFinalSentenceEnds."});
}

TEST(LayoutBlocks, ListsWithNestingAndMonospaceCodeBlocks) {
    std::vector<FreeGlyph> gs;
    put_token(gs, "- Outer bullet", 72.0, 720.0);
    put_token(gs, "- Nested bullet", 96.0, 704.0);
    put_token(gs, "1. First step", 72.0, 672.0);
    put_token(gs, "2. Second step", 72.0, 656.0);
    put_token(gs, "int main() {", 72.0, 610.0, kBodySize, false, true);
    put_token(gs, "  return 0;", 72.0, 596.0, kBodySize, false, true);
    put_token(gs, "}", 72.0, 582.0, kBodySize, false, true);

    const pdf2md::Document doc = analyze_page(make_free_page(gs));
    ASSERT_EQ(doc.blocks.size(), 5u);

    EXPECT_EQ(doc.blocks[0].kind, pdf2md::BlockKind::ListItem);
    EXPECT_FALSE(doc.blocks[0].list_ordered);
    EXPECT_EQ(doc.blocks[0].list_indent, 0);
    EXPECT_EQ(doc.blocks[0].list_marker, U"-");
    expect_block_lines(doc.blocks[0], pdf2md::BlockKind::ListItem, {U"Outer bullet"});

    EXPECT_EQ(doc.blocks[1].kind, pdf2md::BlockKind::ListItem);
    EXPECT_FALSE(doc.blocks[1].list_ordered);
    EXPECT_EQ(doc.blocks[1].list_indent, 1);
    EXPECT_EQ(doc.blocks[1].list_marker, U"-");
    expect_block_lines(doc.blocks[1], pdf2md::BlockKind::ListItem, {U"Nested bullet"});

    EXPECT_EQ(doc.blocks[2].kind, pdf2md::BlockKind::ListItem);
    EXPECT_TRUE(doc.blocks[2].list_ordered);
    EXPECT_EQ(doc.blocks[2].list_marker, U"1.");
    expect_block_lines(doc.blocks[2], pdf2md::BlockKind::ListItem, {U"First step"});

    EXPECT_EQ(doc.blocks[3].kind, pdf2md::BlockKind::ListItem);
    EXPECT_TRUE(doc.blocks[3].list_ordered);
    EXPECT_EQ(doc.blocks[3].list_marker, U"2.");
    expect_block_lines(doc.blocks[3], pdf2md::BlockKind::ListItem, {U"Second step"});

    EXPECT_EQ(doc.blocks[4].kind, pdf2md::BlockKind::Code);
    ASSERT_EQ(doc.blocks[4].lines.size(), 3u);
    EXPECT_TRUE(doc.blocks[4].lines[0].mono_majority);
    EXPECT_EQ(doc.blocks[4].lines[0].plain, U"int main() {");
    EXPECT_EQ(doc.blocks[4].lines[2].plain, U"}");
}

TEST(LayoutBlocks, HangingIndentAndBibliographyContinuationsStayOneParagraph) {
    auto build_words = [](std::initializer_list<const char*> line1, double y1,
                          std::initializer_list<const char*> line2, double x2, double y2) {
        std::vector<FreeGlyph> gs;
        double x = kLeftMargin;
        for (const char* w : line1) {
            put_token(gs, w, x, y1);
            x += static_cast<double>(std::string_view(w).size()) * 6.0 + kInterTokenGap;
        }
        for (const char* w : line2) {
            put_token(gs, w, x2, y2);
            x2 += static_cast<double>(std::string_view(w).size()) * 6.0 + kInterTokenGap;
        }
        return gs;
    };

    struct RefCase {
        const char* name;
        std::vector<FreeGlyph> glyphs;
        std::vector<std::u32string> expected_lines;
    };

    std::vector<FreeGlyph> dash_ref = build_words(
        {"[21]", "ISO", "15765-2", "Road", "vehicles", "Diagnostics", "on", "CAN"}, 700.0, {},
        92.0, 686.0);
    double x2 = 92.0;
    put_token(dash_ref, "-", x2, 686.0);
    x2 += 10.0;
    for (const char* w : {"Part", "2:", "Network", "layer", "services"}) {
        put_token(dash_ref, w, x2, 686.0);
        x2 += static_cast<double>(std::string_view(w).size()) * 6.0 + kInterTokenGap;
    }

    const std::array<RefCase, 3> cases = {{
        {
            "hanging_indent_unterminated_wrap",
            build_words({"author", "names", "and", "the", "paper", "title", "wide"}, 700.0,
                        {"continued", "second", "line", "ends."}, 92.0, 688.0),
            {U"author names and the paper title wide", U"continued second line ends."},
        },
        {
            "citation_entry_survives_internal_period",
            build_words({"[19]", "Firstauthor", "and", "Secondauthor", "Lastname.", "Some",
                         "descriptive", "paper", "title."},
                        700.0, {"In", "Proceedings", "of", "Venue", "2017."}, 92.0, 688.0),
            {U"[19] Firstauthor and Secondauthor Lastname. Some descriptive paper title.",
             U"In Proceedings of Venue 2017."},
        },
        {
            "dash_continuation_of_reference_not_a_list",
            std::move(dash_ref),
            {U"[21] ISO 15765-2 Road vehicles Diagnostics on CAN",
             U"- Part 2: Network layer services"},
        },
    }};

    for (const RefCase& tc : cases) {
        SCOPED_TRACE(tc.name);
        const pdf2md::Document doc = analyze_page(make_free_page(tc.glyphs));
        ASSERT_EQ(doc.blocks.size(), 1u);
        expect_block_lines(doc.blocks[0], pdf2md::BlockKind::Paragraph, tc.expected_lines);
    }
}

TEST(LayoutBlocks, CrossPageReferenceAndProseStitchingSeams) {
    // 1. Multi-page bracketed reference + bare URL stitches into one paragraph.
    std::vector<pdf2md::PageData> ref_pages;
    {
        std::vector<FreeGlyph> gs;
        double x = kLeftMargin;
        for (const char* w :
             {"[12]", "ISO", "27145-3:2012", "Road", "vehicles", "World-Wide", "Harmonized"}) {
            put_token(gs, w, x, 700.0);
            x += static_cast<double>(std::string_view(w).size()) * 6.0 + kInterTokenGap;
        }
        ref_pages.push_back(make_free_page(gs));
    }
    {
        std::vector<FreeGlyph> gs;
        double x = kLeftMargin;
        for (const char* w : {"On-Board", "Diagnostics", "communication", "Part", "3"}) {
            put_token(gs, w, x, 720.0);
            x += static_cast<double>(std::string_view(w).size()) * 6.0 + kInterTokenGap;
        }
        put_token(gs, "https://www.iso.org/standard/46277.html", kLeftMargin, 690.0);
        ref_pages.push_back(make_free_page(gs));
    }
    const pdf2md::Document stitched_ref = analyze_pages(std::move(ref_pages));
    ASSERT_EQ(stitched_ref.blocks.size(), 1u);
    expect_block_lines(stitched_ref.blocks[0], pdf2md::BlockKind::Paragraph,
                       {
                           U"[12] ISO 27145-3:2012 Road vehicles World-Wide Harmonized",
                           U"On-Board Diagnostics communication Part 3",
                           U"https://www.iso.org/standard/46277.html",
                       });

    // 2. Mid-sentence unterminated prose line at bottom of page 0 stitches with
    // lowercase continuation at top of page 1.
    std::vector<FreeGlyph> mid0;
    put_tight(mid0, "A sentence split across a page boundary without punctuation", kLeftMargin,
              90.0);
    std::vector<FreeGlyph> mid1;
    put_tight(mid1, "continues smoothly at the top of the next page.", kLeftMargin, 720.0);
    const pdf2md::Document stitched_prose =
        analyze_pages({make_free_page(mid0), make_free_page(mid1)});
    ASSERT_EQ(stitched_prose.blocks.size(), 1u);
    expect_block_lines(stitched_prose.blocks[0], pdf2md::BlockKind::Paragraph,
                       {
                           U"A sentence split across a page boundary without punctuation",
                           U"continues smoothly at the top of the next page.",
                       });

    // 3. Terminated sentence on page 0 + capitalized new sentence on page 1 stays separate.
    std::vector<FreeGlyph> p0;
    put_token(p0, "First paragraph closes cleanly on page one.", kLeftMargin, 100.0);
    std::vector<FreeGlyph> p1;
    put_token(p1, "Second paragraph opens fresh on page two.", kLeftMargin, 720.0);
    const pdf2md::Document unstitched = analyze_pages({make_free_page(p0), make_free_page(p1)});
    ASSERT_EQ(unstitched.blocks.size(), 2u);
    EXPECT_EQ(unstitched.blocks[0].page, 0);
    EXPECT_EQ(unstitched.blocks[1].page, 1);
    expect_block_lines(unstitched.blocks[0], pdf2md::BlockKind::Paragraph,
                       {U"First paragraph closes cleanly on page one."});
    expect_block_lines(unstitched.blocks[1], pdf2md::BlockKind::Paragraph,
                       {U"Second paragraph opens fresh on page two."});
}

TEST(LayoutBlocks, NumberedAndSymbolFootnotesBecomeDefinitionBlocks) {
    std::vector<FreeGlyph> gs;
    put_token(gs, "Body", 72.0, 740.0);
    put_token(gs, "paragraph", 100.0, 740.0);
    put_token(gs, "text", 158.0, 740.0);
    gs.push_back({U'5', 72.0, 703.6, 6.0});
    put_token(gs, "footnote", 79.0, 700.0);
    put_token(gs, "body", 130.0, 700.0);
    gs.push_back({0x2020, 72.0, 673.6, 6.0});
    put_token(gs, "symbol", 79.0, 670.0);
    put_token(gs, "note", 120.0, 670.0);

    // Per layout_analyzer.h: symbol footnotes stay in flow while numbered
    // footnote definitions float to the end of the document.
    const pdf2md::Document doc = analyze_page(make_free_page(gs));
    ASSERT_EQ(doc.blocks.size(), 3u);
    EXPECT_FALSE(doc.blocks[0].is_footnote);
    expect_block_lines(doc.blocks[0], pdf2md::BlockKind::Paragraph, {U"Body paragraph text"});

    EXPECT_TRUE(doc.blocks[1].is_footnote);
    EXPECT_FALSE(doc.blocks[1].footnote_numbered);
    EXPECT_EQ(doc.blocks[1].footnote_label, U"\u2020");
    ASSERT_EQ(doc.blocks[1].lines.size(), 1u);
    expect_runs(doc.blocks[1].lines[0].runs, {plain_run(U"symbol note")});

    EXPECT_TRUE(doc.blocks[2].is_footnote);
    EXPECT_TRUE(doc.blocks[2].footnote_numbered);
    EXPECT_EQ(doc.blocks[2].footnote_label, U"5");
    ASSERT_EQ(doc.blocks[2].lines.size(), 1u);
    expect_runs(doc.blocks[2].lines[0].runs, {plain_run(U"footnote body")});
}

TEST(LayoutHeadings, SectionNumberSetsNestingDepthAndDisableFlagSuppressesPromotion) {
    std::vector<FreeGlyph> gs;
    put_body(gs, 8, 740.0);
    put_token(gs, "2", 72.0, 620.0, 15.0, true);
    put_token(gs, "Methods", 90.0, 620.0, 15.0, true);
    put_token(gs, "2.1", 72.0, 560.0, 12.0, true);
    put_token(gs, "Setup", 102.0, 560.0, 12.0, true);
    put_token(gs, "2.1.1", 72.0, 500.0, 12.0, true);
    put_token(gs, "Details", 116.0, 500.0, 12.0, true);
    const pdf2md::PageData page = make_free_page(gs);

    const pdf2md::Document enabled = analyze_page(page, {.detect_headings = true});
    EXPECT_DOUBLE_EQ(enabled.body_size, kBodySize);
    ASSERT_EQ(enabled.blocks.size(), 4u);
    EXPECT_EQ(enabled.blocks[0].kind, pdf2md::BlockKind::Paragraph);

    EXPECT_EQ(enabled.blocks[1].kind, pdf2md::BlockKind::Heading);
    EXPECT_EQ(enabled.blocks[1].heading_level, 1);
    expect_block_lines(enabled.blocks[1], pdf2md::BlockKind::Heading, {U"2 Methods"});

    EXPECT_EQ(enabled.blocks[2].kind, pdf2md::BlockKind::Heading);
    EXPECT_EQ(enabled.blocks[2].heading_level, 2);
    expect_block_lines(enabled.blocks[2], pdf2md::BlockKind::Heading, {U"2.1 Setup"});

    EXPECT_EQ(enabled.blocks[3].kind, pdf2md::BlockKind::Heading);
    EXPECT_EQ(enabled.blocks[3].heading_level, 3);
    expect_block_lines(enabled.blocks[3], pdf2md::BlockKind::Heading, {U"2.1.1 Details"});

    const pdf2md::Document disabled = analyze_page(page, {.detect_headings = false});
    ASSERT_EQ(disabled.blocks.size(), 4u);
    for (size_t i = 0; i < 4u; ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(disabled.blocks[i].kind, pdf2md::BlockKind::Paragraph);
        EXPECT_EQ(disabled.blocks[i].heading_level, 0);
    }
}

TEST(LayoutHeadings, StructuralPatternsVetoHeadingPromotion) {
    struct VetoCase {
        const char* name;
        std::vector<FreeGlyph> glyphs;
        std::vector<std::pair<pdf2md::BlockKind, std::u32string>> expected_tail_blocks;
    };

    std::vector<FreeGlyph> metadata_gs;
    put_body(metadata_gs, 10, 740.0);
    put_token(metadata_gs, "Status: DRAFT", 72.0, 600.0, kBodySize, true);
    put_token(metadata_gs, "Status: DRAFT", 72.0, 560.0, kBodySize, true);
    put_token(metadata_gs, "Status: DRAFT", 72.0, 520.0, kBodySize, true);

    std::vector<FreeGlyph> caption_gs;
    put_body(caption_gs, 10, 740.0);
    put_token(caption_gs, "Figure", 72.0, 600.0, 14.0);
    put_token(caption_gs, "3", 140.0, 600.0, 14.0);
    put_token(caption_gs, "System", 170.0, 600.0, 14.0);
    put_token(caption_gs, "Overview", 260.0, 600.0, 14.0);

    std::vector<FreeGlyph> ref_gs;
    put_body(ref_gs, 10, 740.0);
    put_token(ref_gs, "[12]", 72.0, 600.0, kBodySize, true);
    put_token(ref_gs, "ISO Reference Title Nine", 110.0, 600.0, kBodySize, true);

    std::vector<FreeGlyph> sibling_gs;
    put_body(sibling_gs, 10, 740.0);
    put_token(sibling_gs, "Unique Section Title", 72.0, 600.0, kBodySize, true);
    put_token(sibling_gs, "Data Field", 72.0, 560.0, kBodySize, true);
    put_token(sibling_gs, "Data Field", 72.0, 520.0, kBodySize, true);
    put_token(sibling_gs, "Data Field", 72.0, 480.0, kBodySize, true);

    const std::array<VetoCase, 4> cases = {{
        {
            "repeated_metadata_field_stays_paragraph",
            std::move(metadata_gs),
            {
                {pdf2md::BlockKind::Paragraph, U"Status: DRAFT"},
                {pdf2md::BlockKind::Paragraph, U"Status: DRAFT"},
                {pdf2md::BlockKind::Paragraph, U"Status: DRAFT"},
            },
        },
        {
            "figure_caption_not_promoted",
            std::move(caption_gs),
            {{pdf2md::BlockKind::Paragraph, U"Figure 3 System Overview"}},
        },
        {
            "bracketed_reference_not_promoted",
            std::move(ref_gs),
            {{pdf2md::BlockKind::Paragraph, U"[12] ISO Reference Title Nine"}},
        },
        {
            "repeated_sibling_vetoed_while_unique_title_promotes",
            std::move(sibling_gs),
            {
                {pdf2md::BlockKind::Heading, U"Unique Section Title"},
                {pdf2md::BlockKind::Paragraph, U"Data Field"},
                {pdf2md::BlockKind::Paragraph, U"Data Field"},
                {pdf2md::BlockKind::Paragraph, U"Data Field"},
            },
        },
    }};

    for (const VetoCase& tc : cases) {
        SCOPED_TRACE(tc.name);
        const pdf2md::Document doc = analyze_page(make_free_page(tc.glyphs));
        ASSERT_EQ(doc.blocks.size(), 1u + tc.expected_tail_blocks.size());
        EXPECT_EQ(doc.blocks[0].kind, pdf2md::BlockKind::Paragraph);
        for (size_t i = 0; i < tc.expected_tail_blocks.size(); ++i) {
            SCOPED_TRACE(i);
            const pdf2md::Block& blk = doc.blocks[1u + i];
            EXPECT_EQ(blk.kind, tc.expected_tail_blocks[i].first);
            ASSERT_EQ(blk.lines.size(), 1u);
            EXPECT_EQ(blk.lines[0].plain, tc.expected_tail_blocks[i].second);
        }
    }
}

TEST(LayoutHeadings, HeadingSizedSentenceCalloutVsFullWidthProse) {
    struct CalloutCase {
        const char* name;
        double x_left;
        const char* line1;
        const char* line2;
        bool expected_blockquote;
    };

    constexpr CalloutCase kCases[] = {
        {"inset_sentence_callout_becomes_blockquote", 200.0,
         "A centred permission notice reproduced here",
         "for scholarly and journalistic use only.", true},
        {"full_width_sentence_stays_plain_paragraph", 72.0,
         "This introductory sentence spans the whole body column",
         "and continues onto a second line before it ends here.", false},
    };

    for (const CalloutCase& tc : kCases) {
        SCOPED_TRACE(tc.name);
        std::vector<FreeGlyph> gs;
        put_body(gs, 12, 740.0);
        put_token(gs, tc.line1, tc.x_left, 560.0, 13.0);
        put_token(gs, tc.line2, tc.x_left, 544.0, 13.0);

        const pdf2md::Document doc = analyze_page(make_free_page(gs));
        ASSERT_EQ(doc.blocks.size(), 2u);
        EXPECT_EQ(doc.blocks[1].kind, pdf2md::BlockKind::Paragraph);
        EXPECT_EQ(doc.blocks[1].heading_level, 0);
        EXPECT_EQ(doc.blocks[1].blockquote, tc.expected_blockquote);
    }
}

TEST(LayoutHeadings, NumberedRunInTitlePromotedOnlyUnderMatchingParent) {
    std::vector<FreeGlyph> gs;
    put_body(gs, 8, 740.0);
    put_token(gs, "3.2", 72.0, 620.0, 15.0, true);
    put_token(gs, "Setup", 108.0, 620.0, 15.0, true);
    put_token(gs, "3.2.1", 72.0, 584.0, 9.5, true);
    put_token(gs, "Details", 108.0, 584.0, 9.5, true);
    put_token(gs, "9.9.9", 72.0, 548.0, 9.5, true);
    put_token(gs, "Orphan", 108.0, 548.0, 9.5, true);

    const pdf2md::Document doc = analyze_page(make_free_page(gs));
    ASSERT_EQ(doc.blocks.size(), 4u);
    EXPECT_EQ(doc.blocks[1].kind, pdf2md::BlockKind::Heading);
    EXPECT_EQ(doc.blocks[1].heading_level, 1);
    expect_block_lines(doc.blocks[1], pdf2md::BlockKind::Heading, {U"3.2 Setup"});

    EXPECT_EQ(doc.blocks[2].kind, pdf2md::BlockKind::Heading);
    EXPECT_EQ(doc.blocks[2].heading_level, 2);
    expect_block_lines(doc.blocks[2], pdf2md::BlockKind::Heading, {U"3.2.1 Details"});

    EXPECT_EQ(doc.blocks[3].kind, pdf2md::BlockKind::Paragraph);
    EXPECT_EQ(doc.blocks[3].heading_level, 0);
    EXPECT_TRUE(doc.blocks[3].bold_majority);
    ASSERT_EQ(doc.blocks[3].lines.size(), 1u);
    expect_runs(doc.blocks[3].lines[0].runs, {bold_run(U"9.9.9 Orphan")});
}

TEST(LayoutChrome, RecurringHeadersAndFootersStrippedUnlessDisabled) {
    std::vector<pdf2md::PageData> pages;
    for (int p = 1; p <= 6; ++p) {
        std::vector<FreeGlyph> gs;
        put_token(gs, "Specification Running Header", 72.0, 750.0);
        put_token(gs, "Body content unique to page number " + std::to_string(p) + " here", 72.0,
                  400.0);
        put_token(gs, std::to_string(p) + " of 6", 72.0, 60.0);
        put_token(gs, "Reference 5 SpecTitle", 300.0, 60.0);
        pages.push_back(make_free_page(gs));
    }

    const pdf2md::Document stripped = analyze_pages(pages, {.strip_headers_footers = true});
    ASSERT_EQ(stripped.blocks.size(), 6u);
    for (size_t i = 0; i < 6u; ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(stripped.blocks[i].page, static_cast<int>(i));
        const std::u32string expected_body =
            U"Body content unique to page number " +
            std::u32string(1, static_cast<char32_t>(U'1' + i)) + U" here";
        expect_block_lines(stripped.blocks[i], pdf2md::BlockKind::Paragraph, {expected_body});
    }

    const pdf2md::Document retained = analyze_pages(pages, {.strip_headers_footers = false});
    ASSERT_EQ(retained.blocks.size(), 24u);
    expect_block_lines(retained.blocks[0], pdf2md::BlockKind::Paragraph,
                       {U"Specification Running Header"});
    expect_block_lines(retained.blocks[1], pdf2md::BlockKind::Paragraph,
                       {U"Body content unique to page number 1 here"});
    expect_block_lines(retained.blocks[2], pdf2md::BlockKind::Paragraph, {U"1 of 6"});
    expect_block_lines(retained.blocks[3], pdf2md::BlockKind::Paragraph,
                       {U"Reference 5 SpecTitle"});
}

TEST(LayoutChrome, FooterNotFusedWithTrailingBodyLine) {
    std::vector<pdf2md::PageData> pages;
    for (int p = 1; p <= 6; ++p) {
        std::vector<FreeGlyph> gs;
        put_token(gs, "Sentence body for page " + std::to_string(p) + " ends here", 72.0, 108.0);
        put_token(gs, std::to_string(p) + " of 6 Document Ref 42 SpecTitle", 72.0, 74.0);
        pages.push_back(make_free_page(gs));
    }

    const pdf2md::Document doc = analyze_pages(std::move(pages));
    ASSERT_EQ(doc.blocks.size(), 6u);
    for (size_t i = 0; i < 6u; ++i) {
        SCOPED_TRACE(i);
        const std::u32string expected_body =
            U"Sentence body for page " +
            std::u32string(1, static_cast<char32_t>(U'1' + i)) + U" ends here";
        expect_block_lines(doc.blocks[i], pdf2md::BlockKind::Paragraph, {expected_body});
    }
}

TEST(LayoutChrome, UniqueBottomBandLineNotStripped) {
    std::vector<pdf2md::PageData> pages;
    for (int p = 1; p <= 6; ++p) {
        std::vector<FreeGlyph> gs;
        put_token(gs, "Main paragraph on page " + std::to_string(p), 72.0, 400.0);
        put_token(gs, std::to_string(p) + " of 6 SpecTitle", 300.0, 60.0);
        if (p == 2) {
            put_token(gs, "one off footnote about widgets", 72.0, 60.0);
        }
        pages.push_back(make_free_page(gs));
    }

    const pdf2md::Document doc = analyze_pages(std::move(pages));
    ASSERT_EQ(doc.blocks.size(), 7u);
    expect_block_lines(doc.blocks[1], pdf2md::BlockKind::Paragraph, {U"Main paragraph on page 2"});
    EXPECT_EQ(doc.blocks[2].page, 1);
    expect_block_lines(doc.blocks[2], pdf2md::BlockKind::Paragraph,
                       {U"one off footnote about widgets"});
}

TEST(LayoutImages, FigurePlacedBelowHeadingAndAboveCaption) {
    std::vector<FreeGlyph> gs;
    put_token(gs, "Visualizations", 72.0, 700.0, 14.0, true);
    put_token(gs, "Figure", 72.0, 470.0, kBodySize);
    put_token(gs, "3", 120.0, 470.0, kBodySize);
    pdf2md::PageData page = make_free_page(gs);
    page.images.push_back({"images/fig.png", 100.0, 690.0, 480.0});

    const pdf2md::Document doc = analyze_page(page);
    ASSERT_EQ(doc.blocks.size(), 3u);
    ASSERT_EQ(doc.blocks[0].lines.size(), 1u);
    EXPECT_EQ(doc.blocks[0].lines[0].plain, U"Visualizations");

    EXPECT_EQ(doc.blocks[1].kind, pdf2md::BlockKind::Image);
    EXPECT_EQ(doc.blocks[1].image_path, "images/fig.png");
    EXPECT_DOUBLE_EQ(doc.blocks[1].y_top, 690.0);
    EXPECT_DOUBLE_EQ(doc.blocks[1].y_bottom, 480.0);

    expect_block_lines(doc.blocks[2], pdf2md::BlockKind::Paragraph, {U"Figure 3"});
}

TEST(LayoutImages, ImageOnlyPageOrdersMultipleImagesTopToBottom) {
    pdf2md::PageData page;
    page.width = kPageWidth;
    page.height = kPageHeight;
    page.images.push_back({"images/bottom.png", 80.0, 300.0, 120.0});
    page.images.push_back({"images/top.png", 80.0, 720.0, 540.0});

    const pdf2md::Document doc = analyze_page(page);
    ASSERT_EQ(doc.blocks.size(), 2u);
    EXPECT_EQ(doc.blocks[0].kind, pdf2md::BlockKind::Image);
    EXPECT_EQ(doc.blocks[0].image_path, "images/top.png");
    EXPECT_EQ(doc.blocks[1].kind, pdf2md::BlockKind::Image);
    EXPECT_EQ(doc.blocks[1].image_path, "images/bottom.png");
}

TEST(LayoutTextTable, OversizedHeadingAboveGridStaysOutside) {
    std::vector<FreeGlyph> gs;
    put_token(gs, "[REQ_ID_09999]", 72.0, 700.0, 12.0, true);
    put_token(gs, "Definition of API function", 205.0, 700.0, 12.0, true);
    put_token(gs, "Kind:", 72.0, 680.0, 8.0);
    put_token(gs, "function", 205.0, 680.0, 8.0);
    put_token(gs, "Scope:", 72.0, 668.0, 8.0);
    put_token(gs, "classFoo", 205.0, 668.0, 8.0);
    put_token(gs, "Syntax:", 72.0, 656.0, 8.0);
    put_token(gs, "voidBar", 205.0, 656.0, 8.0);

    const pdf2md::Document doc = analyze_page(make_free_page(gs));
    ASSERT_EQ(doc.blocks.size(), 2u);
    expect_block_lines(doc.blocks[0], pdf2md::BlockKind::Paragraph,
                       {U"[REQ_ID_09999] Definition of API function"});

    EXPECT_EQ(doc.blocks[1].col_edges.size(), 0u);
    expect_table_cells(doc.blocks[1],
                       {
                           {U"Kind:", U"function"},
                           {U"Scope:", U"classFoo"},
                           {U"Syntax:", U"voidBar"},
                       });
}

TEST(LayoutTextTable, DisableTablesFlagSuppressesTextAndRuledTables) {
    std::vector<FreeGlyph> text_gs;
    put_token(text_gs, "Name", 72.0, 700.0);
    put_token(text_gs, "Lo", 150.0, 700.0);
    put_token(text_gs, "Hi", 210.0, 700.0);
    put_token(text_gs, "aa", 72.0, 684.0);
    put_token(text_gs, "10", 150.0, 684.0);
    put_token(text_gs, "20", 210.0, 684.0);
    put_token(text_gs, "bb", 72.0, 668.0);
    put_token(text_gs, "11", 150.0, 668.0);
    put_token(text_gs, "21", 210.0, 668.0);

    const pdf2md::Document text_disabled =
        analyze_page(make_free_page(text_gs), {.detect_tables = false});
    ASSERT_EQ(text_disabled.blocks.size(), 1u);
    EXPECT_EQ(text_disabled.blocks[0].kind, pdf2md::BlockKind::Paragraph);

    const pdf2md::PageData ruled =
        ruled_table_page({{"Date", "Release", "Who"}, {"2025", "R25", "Org"}}, true);
    const pdf2md::Document ruled_disabled = analyze_page(ruled, {.detect_tables = false});
    ASSERT_EQ(ruled_disabled.blocks.size(), 1u);
    EXPECT_EQ(ruled_disabled.blocks[0].kind, pdf2md::BlockKind::Paragraph);
}

TEST(LayoutTable, GlobalColumnBoundarySplitsTightNeighbours) {
    std::vector<FreeGlyph> gs;
    put_token(gs, "Name", 72.0, 700.0);
    put_token(gs, "Lo", 150.0, 700.0);
    put_token(gs, "Hi", 210.0, 700.0);
    put_token(gs, "aa", 72.0, 684.0);
    put_token(gs, "10", 150.0, 684.0);
    put_token(gs, "20", 210.0, 684.0);
    put_token(gs, "bb", 72.0, 668.0);
    put_token(gs, "11", 150.0, 668.0);
    put_token(gs, "21", 210.0, 668.0);
    put_token(gs, "cc", 72.0, 652.0);
    put_token(gs, "34", 168.0, 652.0);
    put_token(gs, "56", 186.0, 652.0);

    const pdf2md::Document doc = analyze_page(make_free_page(gs));
    ASSERT_EQ(doc.blocks.size(), 1u);
    EXPECT_EQ(doc.blocks[0].col_edges.size(), 0u);
    expect_table_cells(doc.blocks[0],
                       {
                           {U"Name", U"Lo", U"Hi"},
                           {U"aa", U"10", U"20"},
                           {U"bb", U"11", U"21"},
                           {U"cc", U"34", U"56"},
                       });
}

TEST(LayoutTable, DotLeaderlessTocEntryAndGroupLabelFoldIntoSingleTable) {
    // 1. Long leader-less TOC line folds into the 2-column table and stitches halves.
    std::vector<FreeGlyph> toc_gs;
    put_token(toc_gs, "Alpha", 72.0, 700.0);
    put_token(toc_gs, "101", 273.0, 700.0);
    put_token(toc_gs, "Beta", 72.0, 684.0);
    put_token(toc_gs, "102", 273.0, 684.0);
    put_token(toc_gs, "Gamma", 72.0, 668.0);
    put_token(toc_gs, "103", 273.0, 668.0);
    put_token(toc_gs, "OrphanTocEntryTooLongForALeaderX", 72.0, 652.0);
    toc_gs.push_back({U' ', 266.0, 652.0});
    put_token(toc_gs, "104", 273.0, 652.0);
    put_token(toc_gs, "Delta", 72.0, 636.0);
    put_token(toc_gs, "105", 273.0, 636.0);
    put_token(toc_gs, "Epsilon", 72.0, 620.0);
    put_token(toc_gs, "106", 273.0, 620.0);
    put_token(toc_gs, "Zeta", 72.0, 604.0);
    put_token(toc_gs, "107", 273.0, 604.0);

    const pdf2md::Document toc_doc = analyze_page(make_free_page(toc_gs));
    ASSERT_EQ(toc_doc.blocks.size(), 1u);
    expect_table_cells(toc_doc.blocks[0],
                       {
                           {U"Alpha", U"101"},
                           {U"Beta", U"102"},
                           {U"Gamma", U"103"},
                           {U"OrphanTocEntryTooLongForALeaderX", U"104"},
                           {U"Delta", U"105"},
                           {U"Epsilon", U"106"},
                           {U"Zeta", U"107"},
                       });

    // 2. Half-pitch row-group label in column 0 folds into the table rather than
    // breaking it into standalone prose.
    std::vector<FreeGlyph> grp_gs;
    put_token(grp_gs, "Key", 72.0, 700.0);
    put_token(grp_gs, "P", 150.0, 700.0);
    put_token(grp_gs, "Q", 210.0, 700.0);
    put_token(grp_gs, "base", 72.0, 684.0);
    put_token(grp_gs, "1", 150.0, 684.0);
    put_token(grp_gs, "2", 210.0, 684.0);
    put_token(grp_gs, "3", 150.0, 668.0);
    put_token(grp_gs, "4", 210.0, 668.0);
    put_token(grp_gs, "(A)", 72.0, 660.0);
    put_token(grp_gs, "5", 150.0, 652.0);
    put_token(grp_gs, "6", 210.0, 652.0);
    put_token(grp_gs, "big", 72.0, 636.0);
    put_token(grp_gs, "7", 150.0, 636.0);
    put_token(grp_gs, "8", 210.0, 636.0);

    const pdf2md::Document grp_doc = analyze_page(make_free_page(grp_gs));
    ASSERT_EQ(grp_doc.blocks.size(), 1u);
    expect_table_cells(grp_doc.blocks[0],
                       {
                           {U"Key", U"P", U"Q"},
                           {U"base", U"1", U"2"},
                           {U"(A)", U"3", U"4"},
                           {U"", U"5", U"6"},
                           {U"big", U"7", U"8"},
                       });
}

TEST(LayoutTable, CellSuperscriptBoldAndPlaceholderDashCollapse) {
    // 1. Superscript becomes inline math run and bold cells preserve bold flag.
    std::vector<FreeGlyph> styled_gs;
    put_token(styled_gs, "Key", 72.0, 700.0);
    put_token(styled_gs, "Expr", 200.0, 700.0);
    put_token(styled_gs, "a", 72.0, 684.0);
    styled_gs.push_back({U'n', 200.0, 684.0, kBodySize});
    styled_gs.push_back({U'2', 206.0, 687.6, kScriptSize});
    put_token(styled_gs, "bb", 72.0, 668.0, kBodySize, true);
    put_token(styled_gs, "99", 200.0, 668.0, kBodySize, true);

    const pdf2md::Document styled_doc = analyze_page(make_free_page(styled_gs));
    ASSERT_EQ(styled_doc.blocks.size(), 1u);
    expect_table_cells(styled_doc.blocks[0],
                       {{U"Key", U"Expr"}, {U"a", U"n^2"}, {U"bb", U"99"}});
    expect_runs(styled_doc.blocks[0].table_cells[1][1], {math_run(U"n^2")});
    expect_runs(styled_doc.blocks[0].table_cells[2][0], {bold_run(U"bb")});
    expect_runs(styled_doc.blocks[0].table_cells[2][1], {bold_run(U"99")});

    // 2. Multiple placeholder en-dashes inside one wide detected value column
    // collapse to a single placeholder dash instead of running on as "– – – –".
    constexpr char32_t kEnDash = 0x2013;
    std::vector<FreeGlyph> dash_gs;
    put_token(dash_gs, "Key", 72.0, 700.0);
    put_token(dash_gs, "WideHeaderSpanningAllSubcells", 200.0, 700.0);
    put_token(dash_gs, "aaa", 72.0, 684.0);
    put_token(dash_gs, "WideValueOneSpanningSubcellsX", 200.0, 684.0);
    put_token(dash_gs, "ccc", 72.0, 668.0);
    put_token(dash_gs, "WideValueTwoSpanningSubcellsY", 200.0, 668.0);
    put_token(dash_gs, "bbb", 72.0, 652.0);
    for (double x : {200.0, 250.0, 300.0, 350.0}) {
        dash_gs.push_back({kEnDash, x, 652.0});
    }
    const pdf2md::Document dash_doc = analyze_page(make_free_page(dash_gs));
    ASSERT_EQ(dash_doc.blocks.size(), 1u);
    expect_table_cells(dash_doc.blocks[0],
                       {
                           {U"Key", U"WideHeaderSpanningAllSubcells"},
                           {U"aaa", U"WideValueOneSpanningSubcellsX"},
                           {U"ccc", U"WideValueTwoSpanningSubcellsY"},
                           {U"bbb", U"\u2013"},
                       });
}

TEST(LayoutTable, RuledCellWrapKeepsCompoundHyphenDropsPlainWrap) {
    std::vector<FreeGlyph> gs;
    put_token(gs, "Field", 70.0, 678.0);
    put_token(gs, "Definition", 205.0, 678.0);
    put_token(gs, "PlainWrap", 70.0, 632.0);
    put_token(gs, "informa-", 205.0, 645.0);
    put_token(gs, "tion", 205.0, 620.0);
    put_token(gs, "IdWrap", 70.0, 587.0);
    put_token(gs, "{<data-record-name-", 205.0, 600.0);
    put_token(gs, "x>}", 205.0, 575.0);

    pdf2md::PageData page = make_free_page(gs);
    draw_grid(page, {60.0, 185.0, 400.0}, {700.0, 655.0, 610.0, 565.0});

    const pdf2md::Document doc = analyze_page(page);
    ASSERT_EQ(doc.blocks.size(), 1u);
    expect_table_cells(doc.blocks[0],
                       {
                           {U"Field", U"Definition"},
                           {U"PlainWrap", U"information"},
                           {U"IdWrap", U"{<data-record-name-x>}"},
                       });
}

TEST(LayoutTable, WrappedHeaderAndInnerValueColumnsMergeCleanly) {
    // 1. Wrapped header cell folds into header row.
    {
        std::vector<FreeGlyph> gs;
        put_token(gs, "Type", 72.0, 700.0);
        put_token(gs, "Seq", 150.0, 700.0);
        put_token(gs, "Max", 210.0, 700.0);
        put_token(gs, "Ops", 150.0, 688.0);
        put_token(gs, "aaa", 72.0, 676.0);
        put_token(gs, "11", 150.0, 676.0);
        put_token(gs, "22", 210.0, 676.0);
        put_token(gs, "bbb", 72.0, 664.0);
        put_token(gs, "33", 150.0, 664.0);
        put_token(gs, "44", 210.0, 664.0);
        put_token(gs, "ccc", 72.0, 652.0);
        put_token(gs, "55", 150.0, 652.0);
        put_token(gs, "66", 210.0, 652.0);

        const pdf2md::Document doc = analyze_page(make_free_page(gs));
        ASSERT_EQ(doc.blocks.size(), 1u);
        expect_table_cells(doc.blocks[0],
                           {
                               {U"Type", U"Seq Ops", U"Max"},
                               {U"aaa", U"11", U"22"},
                               {U"bbb", U"33", U"44"},
                               {U"ccc", U"55", U"66"},
                           });
    }

    // 2. Wide wrapped value column keeps 2-column glossary a table.
    {
        std::vector<FreeGlyph> gs;
        put_token(gs, "Term", 72.0, 700.0);
        put_token(gs, "Meaning", 200.0, 700.0);
        put_token(gs, "alpha", 72.0, 686.0);
        put_token(gs, "first entry in the ordered listing", 200.0, 686.0);
        put_token(gs, "of common greek symbols here now", 200.0, 674.0);
        put_token(gs, "beta", 72.0, 660.0);
        put_token(gs, "second entry in the ordered set here", 200.0, 660.0);
        put_token(gs, "seen widely across the statistics", 200.0, 648.0);

        const pdf2md::Document doc = analyze_page(make_free_page(gs));
        ASSERT_EQ(doc.blocks.size(), 1u);
        expect_table_cells(
            doc.blocks[0],
            {
                {U"Term", U"Meaning"},
                {U"alpha",
                 U"first entry in the ordered listing of common greek symbols here now"},
                {U"beta", U"second entry in the ordered set here seen widely across the statistics"},
            });
    }

    // 3. Wrapped middle cell on last row merges instead of creating empty-flanked rows.
    {
        std::vector<FreeGlyph> gs;
        put_token(gs, "Tag", 72.0, 700.0);
        put_token(gs, "Type", 160.0, 700.0);
        put_token(gs, "Note", 320.0, 700.0);
        put_token(gs, "001", 72.0, 686.0);
        put_token(gs, "number", 160.0, 686.0);
        put_token(gs, "small int here", 320.0, 686.0);
        put_token(gs, "110", 72.0, 672.0);
        put_token(gs, "boolean", 160.0, 672.0);
        put_token(gs, "flag enumeration set", 320.0, 672.0);
        put_token(gs, "null, or", 160.0, 658.0);
        put_token(gs, "undefined", 160.0, 644.0);

        const pdf2md::Document doc = analyze_page(make_free_page(gs));
        ASSERT_EQ(doc.blocks.size(), 1u);
        expect_table_cells(doc.blocks[0],
                           {
                               {U"Tag", U"Type", U"Note"},
                               {U"001", U"number", U"small int here"},
                               {U"110", U"boolean null, or undefined", U"flag enumeration set"},
                           });
    }

    // 4. Tightly stacked key/value rows keep distinct header and data rows.
    {
        std::vector<FreeGlyph> gs;
        put_token(gs, "Kind", 72.0, 700.0);
        put_token(gs, "function", 200.0, 700.0);
        put_token(gs, "Scope", 72.0, 690.0);
        put_token(gs, "klass", 200.0, 690.0);
        put_token(gs, "Syntax", 72.0, 674.0);
        put_token(gs, "voidf", 200.0, 674.0);
        put_token(gs, "Return", 72.0, 658.0);
        put_token(gs, "intx", 200.0, 658.0);

        const pdf2md::Document doc = analyze_page(make_free_page(gs));
        ASSERT_EQ(doc.blocks.size(), 1u);
        expect_table_cells(doc.blocks[0],
                           {
                               {U"Kind", U"function"},
                               {U"Scope", U"klass"},
                               {U"Syntax", U"voidf"},
                               {U"Return", U"intx"},
                           });
    }
}

TEST(LayoutTable, WrappedRowLabelsAndSandwichRowsFoldWithoutSwallowingPunctuation) {
    // 1. Wrapped row label joins while short standalone label stays its own row.
    {
        std::vector<FreeGlyph> gs;
        put_token(gs, "Field", 72.0, 700.0);
        put_token(gs, "Value", 210.0, 700.0);
        put_token(gs, "Aaa", 72.0, 684.0);
        put_token(gs, "vone", 210.0, 684.0);
        put_token(gs, "Zzz", 72.0, 668.0);
        put_token(gs, "ForwardingHeader", 72.0, 652.0);
        put_token(gs, "definesheaderfile", 210.0, 652.0);
        put_token(gs, "File", 72.0, 636.0);
        put_token(gs, "Scope", 72.0, 620.0);
        put_token(gs, "final", 210.0, 620.0);

        const pdf2md::Document doc = analyze_page(make_free_page(gs));
        ASSERT_EQ(doc.blocks.size(), 1u);
        expect_table_cells(doc.blocks[0],
                           {
                               {U"Field", U"Value"},
                               {U"Aaa", U"vone"},
                               {U"Zzz", U""},
                               {U"ForwardingHeader File", U"definesheaderfile"},
                               {U"Scope", U"final"},
                           });
    }

    // 2. Vertically centred value between leading and trailing label fragments.
    {
        std::vector<FreeGlyph> gs;
        put_token(gs, "Number", 72.0, 700.0);
        put_token(gs, "Heading", 200.0, 700.0);
        put_token(gs, "X001", 72.0, 684.0);
        put_token(gs, "firstitem", 200.0, 684.0);
        put_token(gs, "X002", 72.0, 668.0);
        put_token(gs, "seconditem", 200.0, 668.0);
        put_token(gs, "ABx", 72.0, 652.0);
        put_token(gs, "centeredvaluehere", 200.0, 638.0);
        put_token(gs, "yCD", 72.0, 624.0);
        put_token(gs, "X003", 72.0, 608.0);
        put_token(gs, "thirditem", 200.0, 608.0);

        const pdf2md::Document doc = analyze_page(make_free_page(gs));
        ASSERT_EQ(doc.blocks.size(), 1u);
        expect_table_cells(doc.blocks[0],
                           {
                               {U"Number", U"Heading"},
                               {U"X001", U"firstitem"},
                               {U"X002", U"seconditem"},
                               {U"ABx yCD", U"centeredvaluehere"},
                               {U"X003", U"thirditem"},
                           });
    }

    // 3. Punctuation-only ellipsis line stays its own row.
    {
        std::vector<FreeGlyph> gs;
        put_token(gs, "Instr", 72.0, 700.0);
        put_token(gs, "Comment", 190.0, 700.0);
        double x = put_tight(gs, "je", 72.0, 684.0);
        gs.push_back({U' ', x, 684.0});
        put_tight(gs, "side_exit_2", x + 3.0, 684.0);
        x = put_tight(gs, "//", 190.0, 684.0);
        gs.push_back({U' ', x, 684.0});
        put_tight(gs, "exit", x + 3.0, 684.0);
        x = put_tight(gs, ".", 72.0, 668.0);
        gs.push_back({U' ', x, 668.0});
        x = put_tight(gs, ".", x + 3.0, 668.0);
        gs.push_back({U' ', x, 668.0});
        put_tight(gs, ".", x + 3.0, 668.0);
        put_token(gs, "ret", 72.0, 652.0);
        x = put_tight(gs, "//", 190.0, 652.0);
        gs.push_back({U' ', x, 652.0});
        put_tight(gs, "done", x + 3.0, 652.0);

        const pdf2md::Document doc = analyze_page(make_free_page(gs));
        ASSERT_EQ(doc.blocks.size(), 1u);
        expect_table_cells(doc.blocks[0],
                           {
                               {U"Instr", U"Comment"},
                               {U"je side_exit_2", U"// exit"},
                               {U". . .", U""},
                               {U"ret", U"// done"},
                           });
    }

    // 4. Three-line bracketed ID sandwich with labelled centre line joins tight.
    {
        std::vector<FreeGlyph> gs;
        put_token(gs, "Num", 72.0, 700.0);
        put_token(gs, "Heading", 200.0, 700.0);
        put_token(gs, "X001", 72.0, 684.0);
        put_token(gs, "firstitem", 200.0, 684.0);
        put_token(gs, "X002", 72.0, 668.0);
        put_token(gs, "seconditem", 200.0, 668.0);
        put_token(gs, "[AB_", 72.0, 652.0);
        put_token(gs, "CD_", 72.0, 636.0);
        put_token(gs, "gammavalue", 200.0, 636.0);
        put_token(gs, "99]", 72.0, 620.0);
        put_token(gs, "X003", 72.0, 604.0);
        put_token(gs, "thirditem", 200.0, 604.0);

        const pdf2md::Document doc = analyze_page(make_free_page(gs));
        ASSERT_EQ(doc.blocks.size(), 1u);
        expect_table_cells(doc.blocks[0],
                           {
                               {U"Num", U"Heading"},
                               {U"X001", U"firstitem"},
                               {U"X002", U"seconditem"},
                               {U"[AB_CD_99]", U"gammavalue"},
                               {U"X003", U"thirditem"},
                           });
    }
}

TEST(LayoutTable, CentredCaptionUnderLabelFragmentLeavesTable) {
    std::vector<FreeGlyph> gs;
    put_token(gs, "Num", 72.0, 700.0);
    put_token(gs, "Heading", 200.0, 700.0);
    put_token(gs, "X010", 72.0, 684.0);
    put_token(gs, "alpha", 200.0, 684.0);
    put_token(gs, "X011", 72.0, 668.0);
    put_token(gs, "beta", 200.0, 668.0);
    put_token(gs, "[GH_", 72.0, 652.0);
    put_token(gs, "IJ_", 72.0, 636.0);
    put_token(gs, "widevalue", 200.0, 636.0);
    put_token(gs, "77]", 72.0, 620.0);
    put_token(gs, "Table 9: a centred caption line", 210.0, 600.0);

    const pdf2md::Document doc = analyze_page(make_free_page(gs));
    ASSERT_EQ(doc.blocks.size(), 2u);
    expect_table_cells(doc.blocks[0],
                       {
                           {U"Num", U"Heading"},
                           {U"X010", U"alpha"},
                           {U"X011", U"beta"},
                           {U"[GH_IJ_77]", U"widevalue"},
                       });
    expect_block_lines(doc.blocks[1], pdf2md::BlockKind::Paragraph,
                       {U"Table 9: a centred caption line"});
}

TEST(LayoutTable, CentredBoldCaptionBelowGridLeavesTable) {
    std::vector<FreeGlyph> gs;
    put_token(gs, "Class", 72.0, 700.0);
    put_token(gs, "Widget", 150.0, 700.0);
    put_token(gs, "Note", 72.0, 684.0);
    put_token(gs, "this note runs wide across the value column to the margin", 150.0, 684.0);
    put_token(gs, "Attr", 72.0, 668.0);
    put_token(gs, "a described value spanning the column out to the right margin", 150.0, 668.0);
    put_token(gs, "the continuation of that value flowing on to the right page margin", 150.0,
              652.0);
    put_token(gs, "Table7WidgetKind", 258.0, 632.0, kBodySize, true);

    const pdf2md::Document doc = analyze_page(make_free_page(gs));
    ASSERT_EQ(doc.blocks.size(), 2u);
    expect_table_cells(
        doc.blocks[0],
        {
            {U"Class", U"Widget"},
            {U"Note", U"this note runs wide across the value column to the margin"},
            {U"Attr",
             U"a described value spanning the column out to the right margin the continuation "
             U"of that value flowing on to the right page margin"},
        });
    ASSERT_EQ(doc.blocks[1].lines.size(), 1u);
    EXPECT_EQ(doc.blocks[1].lines[0].plain, U"Table7WidgetKind");
}

TEST(LayoutTable, FlowingWrappedValueStaysOneCellNotInterleaved) {
    std::vector<FreeGlyph> gs;
    put_token(gs, "Kind", 72.0, 700.0);
    put_token(gs, "function", 150.0, 700.0);
    put_token(gs, "Param", 72.0, 684.0);
    put_token(gs, "arg", 150.0, 684.0);
    put_token(gs, "the input value", 270.0, 684.0);
    put_token(gs, "Ret", 72.0, 668.0);
    put_token(gs, "rtype", 150.0, 668.0);
    put_token(gs, "the result value", 270.0, 668.0);
    put_token(gs, "Syntax", 72.0, 652.0);
    put_token(gs, "aType Foo (VeryLongParamName", 150.0, 652.0);
    put_token(gs, "shortArg) noexcept;", 150.0, 636.0);

    const pdf2md::Document doc = analyze_page(make_free_page(gs));
    ASSERT_EQ(doc.blocks.size(), 1u);
    expect_table_cells(doc.blocks[0],
                       {
                           {U"Kind", U"function", U""},
                           {U"Param", U"arg", U"the input value"},
                           {U"Ret", U"rtype", U"the result value"},
                           {U"Syntax", U"aType Foo (VeryLongParamName shortArg) noexcept;", U""},
                       });
}

TEST(LayoutTable, NestedRicherGridSplitsAndFoldsLabellessInnerWrap) {
    // 1. Interior bold 5-column header splits 2-column box head into two distinct tables.
    {
        std::vector<FreeGlyph> gs;
        auto row5 = [&](const char* a, const char* t, const char* m, const char* k, const char* n,
                        double y, bool bold) {
            put_token(gs, a, 72.0, y, kBodySize, bold);
            put_token(gs, t, 210.0, y, kBodySize, bold);
            put_token(gs, m, 300.0, y, kBodySize, bold);
            put_token(gs, k, 350.0, y, kBodySize, bold);
            put_token(gs, n, 410.0, y, kBodySize, bold);
        };
        put_token(gs, "Class", 72.0, 700.0, kBodySize, true);
        put_token(gs, "Widget", 210.0, 700.0);
        put_token(gs, "Note", 72.0, 684.0, kBodySize, true);
        put_token(gs, "somenote", 210.0, 684.0);
        put_token(gs, "Base", 72.0, 668.0, kBodySize, true);
        put_token(gs, "somebase", 210.0, 668.0);
        row5("Attribute", "Type", "Mult", "Kind", "Note", 652.0, true);
        row5("alpha", "IntType", "one", "attr", "firstnote", 636.0, false);
        row5("beta", "StrType", "two", "ref", "secondnote", 620.0, false);

        const pdf2md::Document doc = analyze_page(make_free_page(gs));
        ASSERT_EQ(doc.blocks.size(), 2u);
        expect_table_cells(doc.blocks[0],
                           {{U"Class", U"Widget"}, {U"Note", U"somenote"}, {U"Base", U"somebase"}});
        expect_table_cells(doc.blocks[1],
                           {
                               {U"Attribute", U"Type", U"Mult", U"Kind", U"Note"},
                               {U"alpha", U"IntType", U"one", U"attr", U"firstnote"},
                               {U"beta", U"StrType", U"two", U"ref", U"secondnote"},
                           });
    }

    // 2. Label-less two-segment inner wrap in split nested grid folds into row above.
    {
        std::vector<FreeGlyph> gs;
        put_token(gs, "Class", 72.0, 700.0, kBodySize, true);
        put_token(gs, "Widget", 200.0, 700.0);
        put_token(gs, "Note", 72.0, 684.0, kBodySize, true);
        put_token(gs, "somenote", 200.0, 684.0);
        put_token(gs, "Attribute", 72.0, 668.0, kBodySize, true);
        put_token(gs, "Type", 200.0, 668.0, kBodySize, true);
        put_token(gs, "Mult", 290.0, 668.0, kBodySize, true);
        put_token(gs, "Kind", 340.0, 668.0, kBodySize, true);
        put_token(gs, "Note", 400.0, 668.0, kBodySize, true);
        put_token(gs, "attrA", 72.0, 652.0);
        put_token(gs, "LongType", 200.0, 652.0);
        put_token(gs, "one", 290.0, 652.0);
        put_token(gs, "ref", 340.0, 652.0);
        put_token(gs, "notetext", 400.0, 652.0);
        put_token(gs, "More", 200.0, 636.0);
        put_token(gs, "andnote", 400.0, 636.0);

        const pdf2md::Document doc = analyze_page(make_free_page(gs));
        ASSERT_EQ(doc.blocks.size(), 2u);
        expect_table_cells(doc.blocks[0], {{U"Class", U"Widget"}, {U"Note", U"somenote"}});
        expect_table_cells(doc.blocks[1],
                           {
                               {U"Attribute", U"Type", U"Mult", U"Kind", U"Note"},
                               {U"attrA", U"LongType More", U"one", U"ref", U"notetext andnote"},
                           });
    }
}

TEST(LayoutTable, CamelCaseIdentifierWrapsVsIndependentRowsInGrids) {
    // 1. Column-filling camelCase label folds tight while short label stays distinct.
    {
        std::vector<FreeGlyph> gs;
        put_token(gs, "Literal", 72.0, 700.0, kBodySize, true);
        put_token(gs, "Description", 280.0, 700.0, kBodySize, true);
        put_token(gs, "shortlit", 72.0, 684.0);
        put_token(gs, "aaatext", 280.0, 684.0);
        put_token(gs, "veryLongLiteralName", 72.0, 668.0);
        put_token(gs, "bbbhere", 280.0, 668.0);
        put_token(gs, "Continued", 72.0, 652.0);
        put_token(gs, "cccmore", 280.0, 652.0);

        const pdf2md::Document doc = analyze_page(make_free_page(gs));
        ASSERT_EQ(doc.blocks.size(), 1u);
        expect_table_cells(doc.blocks[0],
                           {
                               {U"Literal", U"Description"},
                               {U"shortlit", U"aaatext"},
                               {U"veryLongLiteralNameContinued", U"bbbhere cccmore"},
                           });
    }

    // 2. Bare camelCase label continuation in 5-col grid folds when typed cells are empty.
    {
        std::vector<FreeGlyph> gs;
        auto row5 = [&](const char* a, const char* t, const char* m, const char* k, const char* n,
                        double y, bool bold) {
            put_token(gs, a, 72.0, y, kBodySize, bold);
            if (*t != '\0') put_token(gs, t, 200.0, y, kBodySize, bold);
            if (*m != '\0') put_token(gs, m, 290.0, y, kBodySize, bold);
            if (*k != '\0') put_token(gs, k, 340.0, y, kBodySize, bold);
            if (*n != '\0') put_token(gs, n, 400.0, y, kBodySize, bold);
        };
        row5("Attribute", "Type", "Mult", "Kind", "Note", 700.0, true);
        row5("aa", "TypeA", "one", "attr", "noteAAA", 684.0, false);
        row5("cd", "TypeB", "two", "ref", "noteStarts", 668.0, false);
        row5("Xy", "", "", "", "morenote", 652.0, false);

        const pdf2md::Document doc = analyze_page(make_free_page(gs));
        ASSERT_EQ(doc.blocks.size(), 1u);
        expect_table_cells(
            doc.blocks[0],
            {
                {U"Attribute", U"Type", U"Mult", U"Kind", U"Note"},
                {U"aa", U"TypeA", U"one", U"attr", U"noteAAA"},
                {U"cdXy", U"TypeB", U"two", U"ref", U"noteStarts morenote"},
            });
    }

    // 3. Independent fully populated rows are not folded on lowercase->uppercase coincidence.
    {
        std::vector<FreeGlyph> gs;
        auto row4 = [&](const char* a, const char* b, const char* c, const char* d, double y,
                        bool bold) {
            put_token(gs, a, 72.0, y, kBodySize, bold);
            put_token(gs, b, 200.0, y, kBodySize, bold);
            put_token(gs, c, 300.0, y, kBodySize, bold);
            put_token(gs, d, 400.0, y, kBodySize, bold);
        };
        row4("Layer", "Complexity", "Sequential", "PathLen", 700.0, true);
        row4("SelfAttention", "aaa1", "bbb1", "ccc1", 684.0, false);
        row4("Recurrent", "aaa2", "bbb2", "ccc2", 668.0, false);

        const pdf2md::Document doc = analyze_page(make_free_page(gs));
        ASSERT_EQ(doc.blocks.size(), 1u);
        expect_table_cells(doc.blocks[0],
                           {
                               {U"Layer", U"Complexity", U"Sequential", U"PathLen"},
                               {U"SelfAttention", U"aaa1", U"bbb1", U"ccc1"},
                               {U"Recurrent", U"aaa2", U"bbb2", U"ccc2"},
                           });
    }

    // 4. Lockstep label and Type camelCase wrap folds into a single row.
    {
        std::vector<FreeGlyph> gs;
        auto row5 = [&](const char* a, const char* t, const char* m, const char* k, const char* n,
                        double y, bool bold) {
            put_token(gs, a, 72.0, y, kBodySize, bold);
            if (*t != '\0') put_token(gs, t, 200.0, y, kBodySize, bold);
            if (*m != '\0') put_token(gs, m, 360.0, y, kBodySize, bold);
            if (*k != '\0') put_token(gs, k, 410.0, y, kBodySize, bold);
            if (*n != '\0') put_token(gs, n, 470.0, y, kBodySize, bold);
        };
        row5("Attribute", "Type", "Mult", "Kind", "Note", 700.0, true);
        row5("shortAttr", "ShortType", "1", "attr", "plainnote", 684.0, false);
        row5("authentication", "DiagnosticAuthRole", "0..1", "aggr", "somenote", 668.0, false);
        row5("Enabled", "Proxy", "", "", "morenote", 652.0, false);

        const pdf2md::Document doc = analyze_page(make_free_page(gs));
        ASSERT_EQ(doc.blocks.size(), 1u);
        expect_table_cells(doc.blocks[0],
                           {
                               {U"Attribute", U"Type", U"Mult", U"Kind", U"Note"},
                               {U"shortAttr", U"ShortType", U"1", U"attr", U"plainnote"},
                               {U"authenticationEnabled", U"DiagnosticAuthRoleProxy", U"0..1",
                                U"aggr", U"somenote morenote"},
                           });
    }
}

TEST(LayoutRuledGrid, BorderedTableWithSingleBodyRowAndSpanningTitleCaption) {
    // 1. Bordered 3-column grid with a single body row populates col_edges and cells.
    {
        std::vector<FreeGlyph> gs;
        put_token(gs, "Interface", 76.0, 690.0);
        put_token(gs, "Cluster", 236.0, 690.0);
        put_token(gs, "Purpose", 386.0, 690.0);
        put_token(gs, "No provided interfaces", 76.0, 668.0);
        pdf2md::PageData p = make_free_page(gs);
        draw_grid(p, {71.0, 220.0, 370.0, 524.0}, {700.0, 680.0, 660.0});

        const pdf2md::Document doc = analyze_page(p);
        ASSERT_EQ(doc.blocks.size(), 1u);
        expect_table_cells(doc.blocks[0],
                           {
                               {U"Interface", U"Cluster", U"Purpose"},
                               {U"No provided interfaces", U"", U""},
                           });
        ASSERT_EQ(doc.blocks[0].col_edges.size(), 4u);
        EXPECT_DOUBLE_EQ(doc.blocks[0].col_edges[0], 71.0);
        EXPECT_DOUBLE_EQ(doc.blocks[0].col_edges[1], 220.0);
        EXPECT_DOUBLE_EQ(doc.blocks[0].col_edges[2], 370.0);
        EXPECT_DOUBLE_EQ(doc.blocks[0].col_edges[3], 524.0);
    }

    // 2. Full-width spanning title band becomes caption block preceding the table.
    {
        std::vector<FreeGlyph> gs;
        put_token(gs, "Change History", 250.0, 728.0, kBodySize, true);
        put_token(gs, "Date", 76.0, 706.0);
        put_token(gs, "Who", 236.0, 706.0);
        put_token(gs, "2025-11-27", 76.0, 684.0);
        put_token(gs, "Company", 236.0, 684.0);
        pdf2md::PageData p = make_free_page(gs);
        hrule(p, 740.0, 71.0, 524.0);
        hrule(p, 720.0, 71.0, 524.0);
        hrule(p, 698.0, 71.0, 524.0);
        hrule(p, 676.0, 71.0, 524.0);
        vrule(p, 71.0, 676.0, 740.0);
        vrule(p, 524.0, 676.0, 740.0);
        vrule(p, 220.0, 676.0, 720.0);

        const pdf2md::Document doc = analyze_page(p);
        ASSERT_EQ(doc.blocks.size(), 2u);
        expect_block_lines(doc.blocks[0], pdf2md::BlockKind::Heading, {U"Change History"});
        expect_table_cells(doc.blocks[1], {{U"Date", U"Who"}, {U"2025-11-27", U"Company"}});
    }
}

TEST(LayoutRuledGrid, MultiLineCellJoiningHyphenCamelCasePeriodUrlAndInteriorRule) {
    // 1. Hyphen wrap rejoins tight while partial interior rule inserts <br>.
    {
        std::vector<FreeGlyph> gs;
        put_token(gs, "Port Interface", 76.0, 706.0);
        put_token(gs, "API Class", 266.0, 706.0);
        put_token(gs, "DiagnosticGenericIn-", 76.0, 684.0);
        put_token(gs, "terface", 76.0, 672.0);
        put_token(gs, "entry one alpha", 266.0, 684.0);
        put_token(gs, "entry two beta", 266.0, 666.0);
        pdf2md::PageData p = make_free_page(gs);
        hrule(p, 720.0, 71.0, 524.0);
        hrule(p, 696.0, 71.0, 524.0);
        hrule(p, 678.0, 255.0, 524.0);
        hrule(p, 656.0, 71.0, 524.0);
        vrule(p, 71.0, 656.0, 720.0);
        vrule(p, 255.0, 656.0, 720.0);
        vrule(p, 524.0, 656.0, 720.0);

        const pdf2md::Document doc = analyze_page(p);
        ASSERT_EQ(doc.blocks.size(), 1u);
        expect_table_cells(doc.blocks[0],
                           {
                               {U"Port Interface", U"API Class"},
                               {U"DiagnosticGenericInterface", U"entry one alpha<br>entry two beta"},
                           });
    }

    // 2. Multi-line phrase without interior rule joins with spaces.
    {
        std::vector<FreeGlyph> gs;
        put_token(gs, "Date", 76.0, 706.0);
        put_token(gs, "Changed", 236.0, 706.0);
        put_token(gs, "2025", 76.0, 680.0);
        put_token(gs, "Company", 236.0, 690.0);
        put_token(gs, "Release", 236.0, 678.0);
        put_token(gs, "Management", 236.0, 666.0);
        pdf2md::PageData p = make_free_page(gs);
        draw_grid(p, {71.0, 220.0, 524.0}, {720.0, 698.0, 654.0});

        const pdf2md::Document doc = analyze_page(p);
        ASSERT_EQ(doc.blocks.size(), 1u);
        expect_table_cells(doc.blocks[0],
                           {{U"Date", U"Changed"}, {U"2025", U"Company Release Management"}});
    }

    // 3. Lockstep camelCase identifiers join tight while prose note keeps space.
    {
        std::vector<FreeGlyph> gs;
        put_token(gs, "Attribute", 76.0, 706.0, kBodySize, true);
        put_token(gs, "Type", 170.0, 706.0, kBodySize, true);
        put_token(gs, "Note", 366.0, 706.0, kBodySize, true);
        put_token(gs, "authentication", 74.0, 684.0);
        put_token(gs, "DiagnosticAuthRole", 170.0, 684.0);
        put_token(gs, "This value", 366.0, 684.0);
        put_token(gs, "Enabled", 74.0, 668.0);
        put_token(gs, "Proxy", 170.0, 668.0);
        put_token(gs, "Wraps here", 366.0, 668.0);
        pdf2md::PageData p = make_free_page(gs);
        draw_grid(p, {71.0, 160.0, 360.0, 524.0}, {720.0, 698.0, 654.0});

        const pdf2md::Document doc = analyze_page(p);
        ASSERT_EQ(doc.blocks.size(), 1u);
        expect_table_cells(doc.blocks[0],
                           {
                               {U"Attribute", U"Type", U"Note"},
                               {U"authenticationEnabled", U"DiagnosticAuthRoleProxy",
                                U"This value Wraps here"},
                           });
    }

    // 4. Sentence period gets space on wrap while wrapped URL dot binds tight.
    {
        std::vector<FreeGlyph> gs;
        put_token(gs, "Term", 76.0, 716.0, kBodySize, true);
        put_token(gs, "Description", 216.0, 716.0, kBodySize, true);
        put_token(gs, "Client", 76.0, 690.0);
        put_token(gs, "requester, i.e.", 216.0, 690.0);
        put_token(gs, "sends more", 216.0, 680.0);
        put_token(gs, "Ref", 76.0, 664.0);
        put_token(gs, "www.iso.", 216.0, 668.0);
        put_token(gs, "org", 216.0, 658.0);
        pdf2md::PageData p = make_free_page(gs);
        draw_grid(p, {71.0, 200.0, 460.0}, {730.0, 710.0, 676.0, 652.0});

        const pdf2md::Document doc = analyze_page(p);
        ASSERT_EQ(doc.blocks.size(), 1u);
        expect_table_cells(doc.blocks[0],
                           {
                               {U"Term", U"Description"},
                               {U"Client", U"requester, i.e. sends more"},
                               {U"Ref", U"www.iso.org"},
                           });
    }
}

TEST(LayoutRuledGrid, WordStraddlingInteriorRuleKeepsCellWhole) {
    std::vector<FreeGlyph> gs;
    put_token(gs, "Exceptions", 75.0, 700.0);
    put_token(gs, "List of Cpp Exceptions that may be thrown", 205.0, 700.0);
    put_token(gs, "Param", 75.0, 670.0);
    put_token(gs, "typeVal", 205.0, 670.0);
    put_token(gs, "descVal", 400.0, 670.0);
    pdf2md::PageData p = make_free_page(gs);
    draw_grid(p, {71.0, 200.0, 360.0, 524.0}, {715.0, 685.0, 655.0});

    const pdf2md::Document doc = analyze_page(p);
    ASSERT_EQ(doc.blocks.size(), 1u);
    expect_table_cells(doc.blocks[0],
                       {
                           {U"Exceptions", U"List of Cpp Exceptions that may be thrown", U""},
                           {U"Param", U"typeVal", U"descVal"},
                       });
}

TEST(LayoutRuledGrid, NonTableRulePatternsRejected) {
    // 1. Single decorative underline under a heading.
    std::vector<FreeGlyph> underline_gs;
    put_body(underline_gs, 6, 780.0);
    put_token(underline_gs, "A Section Title", 72.0, 690.0, 13.0, true);
    put_body(underline_gs, 6, 660.0);
    pdf2md::PageData underline_page = make_free_page(underline_gs);
    hrule(underline_page, 684.0, 72.0, 520.0);

    const pdf2md::Document underline_doc = analyze_page(underline_page);
    ASSERT_EQ(underline_doc.blocks.size(), 3u);
    EXPECT_EQ(underline_doc.blocks[0].kind, pdf2md::BlockKind::Paragraph);
    EXPECT_EQ(underline_doc.blocks[1].kind, pdf2md::BlockKind::Heading);
    EXPECT_EQ(underline_doc.blocks[2].kind, pdf2md::BlockKind::Paragraph);

    // 2. Full 3x3 ruled grid containing zero glyphs.
    std::vector<FreeGlyph> empty_box_gs;
    put_body(empty_box_gs, 8, 760.0);
    pdf2md::PageData empty_box_page = make_free_page(empty_box_gs);
    draw_grid(empty_box_page, {71.0, 200.0, 330.0, 524.0}, {560.0, 520.0, 480.0, 440.0});

    const pdf2md::Document empty_box_doc = analyze_page(empty_box_page);
    ASSERT_EQ(empty_box_doc.blocks.size(), 1u);
    EXPECT_EQ(empty_box_doc.blocks[0].kind, pdf2md::BlockKind::Paragraph);
}

TEST(LayoutRuledGrid, CrossPageContinuationStitchesWhileRepeatedHeadersStaySeparate) {
    // 1. Continuation marker + unbolded data rows stitch onto page-0 table.
    pdf2md::PageData p0 =
        ruled_table_page({{"Date", "Release", "Who"}, {"2025-11-27", "R25", "Company"}}, true);
    pdf2md::PageData p1 = ruled_table_page(
        {{"2024-11-27", "R24", "Company"}, {"2023-11-23", "R23", "Company"}}, false);
    pdf2md::CharInfo marker;
    marker.code = 0x25B3;
    marker.x = 280.0;
    marker.left = 280.0;
    marker.right = 288.0;
    marker.y = 760.0;
    marker.bottom = 760.0;
    marker.top = 770.0;
    marker.size = kBodySize;
    p1.chars.push_back(marker);

    const pdf2md::Document stitched = analyze_pages({p0, p1});
    ASSERT_EQ(stitched.blocks.size(), 1u);
    expect_table_cells(stitched.blocks[0],
                       {
                           {U"Date", U"Release", U"Who"},
                           {U"2025-11-27", U"R25", U"Company"},
                           {U"2024-11-27", U"R24", U"Company"},
                           {U"2023-11-23", U"R23", U"Company"},
                       });

    // 2. Repeated bold headers with no continuation marker stay two distinct tables.
    pdf2md::PageData r0 =
        ruled_table_page({{"Term", "Kind", "Note"}, {"alpha", "one", "first"}}, true);
    pdf2md::PageData r1 =
        ruled_table_page({{"Term", "Kind", "Note"}, {"beta", "two", "second"}}, true);
    const pdf2md::Document separate = analyze_pages({r0, r1});
    ASSERT_EQ(separate.blocks.size(), 2u);
    EXPECT_EQ(separate.blocks[0].page, 0);
    EXPECT_EQ(separate.blocks[1].page, 1);
    expect_table_cells(separate.blocks[0],
                       {{U"Term", U"Kind", U"Note"}, {U"alpha", U"one", U"first"}});
    expect_table_cells(separate.blocks[1],
                       {{U"Term", U"Kind", U"Note"}, {U"beta", U"two", U"second"}});
}

TEST(LayoutLinks, ResolvesCharLinkIdsAcrossRunsTablesAndPages) {
    // 1. Inline prose link resolves 1-based CharInfo::link_id into StyledRun::link_url
    // without pulling URL slashes or hyphens into inline math mode.
    std::vector<FreeGlyph> p0_gs;
    double x = put_tight(p0_gs, "Read", kLeftMargin, 700.0);
    p0_gs.push_back({U' ', x, 700.0});
    x += 4.0;
    const std::size_t link_start = p0_gs.size();
    x = put_tight(p0_gs, "https://p0.example.org/a-b", x, 700.0);
    const std::size_t link_end = p0_gs.size();

    pdf2md::PageData p0 = make_free_page(p0_gs);
    p0.links.push_back({90.0, 698.0, 260.0, 712.0, "https://p0.example.org/a-b"});
    for (std::size_t i = link_start; i < link_end; ++i) {
        p0.chars[i].link_id = 1U;
    }

    // 2. Page 1 has its own independent PageData::links[0] inside a table cell.
    std::vector<FreeGlyph> p1_gs;
    put_token(p1_gs, "Field", 72.0, 700.0);
    put_token(p1_gs, "Link", 200.0, 700.0);
    put_token(p1_gs, "Spec", 72.0, 684.0);
    const std::size_t cell_link_start = p1_gs.size();
    put_token(p1_gs, "Portal", 200.0, 684.0);
    const std::size_t cell_link_end = p1_gs.size();
    put_token(p1_gs, "Other", 72.0, 668.0);
    put_token(p1_gs, "Plain", 200.0, 668.0);

    pdf2md::PageData p1 = make_free_page(p1_gs);
    p1.links.push_back({198.0, 682.0, 245.0, 696.0, "https://p1.example.org/portal"});
    for (std::size_t i = cell_link_start; i < cell_link_end; ++i) {
        p1.chars[i].link_id = 1U;
    }

    const pdf2md::Document doc = analyze_pages({p0, p1});
    ASSERT_EQ(doc.blocks.size(), 2u);

    ASSERT_EQ(doc.blocks[0].kind, pdf2md::BlockKind::Paragraph);
    ASSERT_EQ(doc.blocks[0].lines.size(), 1u);
    ASSERT_EQ(doc.blocks[0].lines[0].runs.size(), 2u);
    EXPECT_EQ(doc.blocks[0].lines[0].runs[0].text, U"Read ");
    EXPECT_EQ(doc.blocks[0].lines[0].runs[0].link_url, "");
    EXPECT_EQ(doc.blocks[0].lines[0].runs[1].text, U"https://p0.example.org/a-b");
    EXPECT_FALSE(doc.blocks[0].lines[0].runs[1].math);
    EXPECT_EQ(doc.blocks[0].lines[0].runs[1].link_url, "https://p0.example.org/a-b");

    ASSERT_EQ(doc.blocks[1].kind, pdf2md::BlockKind::Table);
    ASSERT_EQ(doc.blocks[1].table_cells.size(), 3u);
    ASSERT_EQ(doc.blocks[1].table_cells[1].size(), 2u);
    ASSERT_EQ(doc.blocks[1].table_cells[1][1].size(), 1u);
    EXPECT_EQ(doc.blocks[1].table_cells[1][1][0].text, U"Portal");
    EXPECT_EQ(doc.blocks[1].table_cells[1][1][0].link_url, "https://p1.example.org/portal");
    ASSERT_EQ(doc.blocks[1].table_cells[2][1].size(), 1u);
    EXPECT_EQ(doc.blocks[1].table_cells[2][1][0].link_url, "");
}

TEST(LayoutStrikethrough, MidlineHorizontalRuleAndCharFlagProduceStrikethroughRuns) {
    // 1. Thin horizontal rule through the glyph x-height midline marks covered glyphs
    // strikethrough and is consumed (never emitted as a thematic HorizontalRule block).
    std::vector<FreeGlyph> gs;
    double x = put_tight(gs, "keep", kLeftMargin, 700.0);
    gs.push_back({U' ', x, 700.0});
    x += 4.0;
    const double strike_x0 = x;
    x = put_tight(gs, "obsolete", x, 700.0);
    const double strike_x1 = x;
    gs.push_back({U' ', x, 700.0});
    x += 4.0;
    const double under_x0 = x;
    x = put_tight(gs, "underlined", x, 700.0);
    const double under_x1 = x;

    pdf2md::PageData page = make_free_page(gs);
    page.rules.push_back({strike_x0, 704.0, strike_x1, 704.0, true, 1.0});
    page.rules.push_back({under_x0, 699.5, under_x1, 699.5, true, 1.0});

    const pdf2md::Document doc = analyze_page(page);
    ASSERT_EQ(doc.blocks.size(), 1u);
    ASSERT_EQ(doc.blocks[0].kind, pdf2md::BlockKind::Paragraph);
    ASSERT_EQ(doc.blocks[0].lines.size(), 1u);
    ASSERT_EQ(doc.blocks[0].lines[0].runs.size(), 3u);
    EXPECT_EQ(doc.blocks[0].lines[0].runs[0].text, U"keep ");
    EXPECT_FALSE(doc.blocks[0].lines[0].runs[0].strikethrough);
    EXPECT_EQ(doc.blocks[0].lines[0].runs[1].text, U"obsolete ");
    EXPECT_TRUE(doc.blocks[0].lines[0].runs[1].strikethrough);
    EXPECT_EQ(doc.blocks[0].lines[0].runs[2].text, U"underlined");
    EXPECT_FALSE(doc.blocks[0].lines[0].runs[2].strikethrough);

    // 2. Explicit CharInfo::strikethrough flag (annotation path) propagates to StyledRun.
    pdf2md::PageData annot_page = make_free_page({
        {U'o', 72.0, 700.0},
        {U'l', 78.0, 700.0},
        {U'd', 84.0, 700.0},
    });
    ASSERT_EQ(annot_page.chars.size(), 3u);
    for (std::size_t i = 0; i < 3u; ++i) {
        annot_page.chars[i].strikethrough = true;
    }
    const pdf2md::Document annot_doc = analyze_page(annot_page);
    ASSERT_EQ(annot_doc.blocks.size(), 1u);
    ASSERT_EQ(annot_doc.blocks[0].lines.size(), 1u);
    ASSERT_EQ(annot_doc.blocks[0].lines[0].runs.size(), 1u);
    EXPECT_EQ(annot_doc.blocks[0].lines[0].runs[0].text, U"old");
    EXPECT_TRUE(annot_doc.blocks[0].lines[0].runs[0].strikethrough);
}

TEST(LayoutHorizontalRule, PromotesStandaloneVectorAndDashDividersWhileSkippingTableRules) {
    // 1. Standalone wide horizontal vector rule between paragraphs -> HorizontalRule block.
    {
        std::vector<FreeGlyph> gs;
        put_token(gs, "Lead paragraph text before section break.", 72.0, 700.0);
        put_token(gs, "Trailing paragraph text after section break.", 72.0, 600.0);
        pdf2md::PageData page = make_free_page(gs);
        page.rules.push_back({72.0, 650.0, 520.0, 650.0, true, 1.0});

        const pdf2md::Document doc = analyze_page(page);
        ASSERT_EQ(doc.blocks.size(), 3u);
        EXPECT_EQ(doc.blocks[0].kind, pdf2md::BlockKind::Paragraph);
        EXPECT_EQ(doc.blocks[1].kind, pdf2md::BlockKind::HorizontalRule);
        EXPECT_EQ(doc.blocks[2].kind, pdf2md::BlockKind::Paragraph);
    }

    // 2. Standalone "---" text divider line -> HorizontalRule block.
    {
        std::vector<FreeGlyph> gs;
        put_token(gs, "First section prose line.", 72.0, 700.0);
        put_token(gs, "---", 72.0, 650.0);
        put_token(gs, "Second section prose line.", 72.0, 600.0);

        const pdf2md::Document doc = analyze_page(make_free_page(gs));
        ASSERT_EQ(doc.blocks.size(), 3u);
        EXPECT_EQ(doc.blocks[0].kind, pdf2md::BlockKind::Paragraph);
        EXPECT_EQ(doc.blocks[1].kind, pdf2md::BlockKind::HorizontalRule);
        EXPECT_EQ(doc.blocks[2].kind, pdf2md::BlockKind::Paragraph);
    }

    // 3. Booktabs horizontal rules bordering a text-geometry table do NOT emit HorizontalRule.
    {
        std::vector<FreeGlyph> gs;
        put_token(gs, "Model", 72.0, 700.0, kBodySize, true);
        put_token(gs, "BLEU", 220.0, 700.0, kBodySize, true);
        put_token(gs, "Cost", 340.0, 700.0, kBodySize, true);
        put_token(gs, "Base", 72.0, 684.0);
        put_token(gs, "27.3", 220.0, 684.0);
        put_token(gs, "100", 340.0, 684.0);
        put_token(gs, "Big", 72.0, 668.0);
        put_token(gs, "28.4", 220.0, 668.0);
        put_token(gs, "300", 340.0, 668.0);

        pdf2md::PageData page = make_free_page(gs);
        hrule(page, 713.0, 70.0, 420.0);
        hrule(page, 696.0, 70.0, 420.0);
        hrule(page, 664.0, 70.0, 420.0);

        const pdf2md::Document doc = analyze_page(page);
        ASSERT_EQ(doc.blocks.size(), 1u);
        EXPECT_EQ(doc.blocks[0].kind, pdf2md::BlockKind::Table);
    }
}

TEST(LayoutMath, ReconstructsRuleAnchoredFractionsVinculumRadicalsAndNthRoots) {
    // 1. Leading display fraction anchored by horizontal fraction-bar RuleSegment
    // with full-size numerator and denominator glyphs: \frac{a+b}{c+d} = y.
    {
        std::vector<FreeGlyph> gs;
        for (int i = 0; i < 20; ++i) {
            gs.push_back({static_cast<char32_t>(U'a' + (i % 26)),
                          kLeftMargin + static_cast<double>(i) * 6.0, 750.0});
        }
        // Numerator "a+b" above bar at y = 702.5, denominator "c+d" below bar.
        gs.push_back({U'a', 250.0, 707.0, 10.0, true});
        gs.push_back({U'+', 258.0, 707.0, 10.0});
        gs.push_back({U'b', 266.0, 707.0, 10.0, true});
        gs.push_back({U'c', 250.0, 690.0, 10.0, true});
        gs.push_back({U'+', 258.0, 690.0, 10.0});
        gs.push_back({U'd', 266.0, 690.0, 10.0, true});
        // Axis host "= y" to the right of the fraction bar.
        gs.push_back({U'=', 280.0, 700.0, 10.0});
        gs.push_back({U'y', 292.0, 700.0, 10.0, true});

        pdf2md::PageData page = make_free_page(gs);
        page.rules.push_back({248.0, 702.5, 273.0, 702.5, true, 0.5});

        const pdf2md::Document doc = analyze_page(page);
        ASSERT_EQ(doc.blocks.size(), 2u);
        EXPECT_EQ(doc.blocks[1].kind, pdf2md::BlockKind::MathDisplay);
        ASSERT_EQ(doc.blocks[1].math_lines.size(), 1u);
        EXPECT_EQ(doc.blocks[1].math_lines[0], U"\\frac{a+b}{c+d} = y");
    }

    // 2. Vinculum overbar RuleSegment captures multi-atom radicand \sqrt{2\pi}.
    {
        pdf2md::PageData page = make_free_page({
            {U'l', 72.0, 700.0},
            {U'e', 78.0, 700.0},
            {U't', 84.0, 700.0},
            {U' ', 90.0, 700.0},
            {0x221A, 96.0, 704.0, 12.0, false, false, true},
            {U'2', 103.0, 700.0, 10.0},
            {0x03C0, 110.0, 700.0, 10.0, true},
        });
        page.rules.push_back({102.0, 710.0, 118.0, 710.0, true, 0.5});

        const pdf2md::Document doc = analyze_page(page);
        ASSERT_EQ(doc.blocks.size(), 1u);
        ASSERT_EQ(doc.blocks[0].lines.size(), 1u);
        expect_runs(doc.blocks[0].lines[0].runs,
                    {plain_run(U"let "), math_run(U"\\sqrt{2\\pi}")});
    }

    // 3. Hook index before U+221A with vinculum produces Nth-root \sqrt[3]{x}.
    {
        pdf2md::PageData page = make_free_page({
            {U'l', 72.0, 700.0},
            {U'e', 78.0, 700.0},
            {U't', 84.0, 700.0},
            {U' ', 90.0, 700.0},
            {U'3', 98.0, 705.0, 6.0},
            {0x221A, 101.0, 704.0, 12.0, false, false, true},
            {U'x', 108.0, 700.0, 10.0, true},
        });
        page.rules.push_back({107.0, 710.0, 115.0, 710.0, true, 0.5});

        const pdf2md::Document doc = analyze_page(page);
        ASSERT_EQ(doc.blocks.size(), 1u);
        ASSERT_EQ(doc.blocks[0].lines.size(), 1u);
        expect_runs(doc.blocks[0].lines[0].runs,
                    {plain_run(U"let "), math_run(U"\\sqrt[3]{x}")});
    }
}

TEST(LayoutMath, UnwrapsAuthorAffiliationSuperscriptsFromMathMode) {
    // 1. Bold author surname carrying symbolic affiliation superscript stays plain bold prose,
    // never wrapped into \mathbf{Vaswani}^\dagger math mode.
    const pdf2md::Document author_doc = analyze_page(make_math_line(
        {
            {up(U'A'), up(U's'), up(U'h'), up(U'i'), up(U's'), up(U'h')},
            {{U'V', 0, false, true},
             {U'a', 0, false, true},
             {U's', 0, false, true},
             {U'w', 0, false, true},
             {U'a', 0, false, true},
             {U'n', 0, false, true},
             {U'i', 0, false, true},
             up(0x2020, 1)},
        },
        700.0));
    ASSERT_EQ(author_doc.blocks.size(), 1u);
    ASSERT_EQ(author_doc.blocks[0].lines.size(), 1u);
    EXPECT_EQ(author_doc.blocks[0].lines[0].plain, U"Ashish Vaswani\u2020");
    ASSERT_EQ(author_doc.blocks[0].lines[0].runs.size(), 3u);
    EXPECT_EQ(author_doc.blocks[0].lines[0].runs[0].text, U"Ashish ");
    EXPECT_FALSE(author_doc.blocks[0].lines[0].runs[0].math);
    EXPECT_EQ(author_doc.blocks[0].lines[0].runs[1].text, U"Vaswani");
    EXPECT_TRUE(author_doc.blocks[0].lines[0].runs[1].bold);
    EXPECT_FALSE(author_doc.blocks[0].lines[0].runs[1].math);
    EXPECT_EQ(author_doc.blocks[0].lines[0].runs[2].text, U"\u2020");
    EXPECT_FALSE(author_doc.blocks[0].lines[0].runs[2].math);

    // 2. Bold prose word carrying numeric superscript unwraps to bold run + raw [^1] ref.
    const pdf2md::Document bold_fn_doc = analyze_page(make_math_line(
        {
            {up(U's'), up(U'e'), up(U'e')},
            {{U'S', 0, false, true},
             {U'm', 0, false, true},
             {U'i', 0, false, true},
             {U't', 0, false, true},
             {U'h', 0, false, true},
             up(U'1', 1)},
        },
        700.0));
    ASSERT_EQ(bold_fn_doc.blocks.size(), 1u);
    ASSERT_EQ(bold_fn_doc.blocks[0].lines.size(), 1u);
    expect_runs(bold_fn_doc.blocks[0].lines[0].runs,
                {plain_run(U"see "), bold_run(U"Smith"), raw_run(U"[^1]")});
}

TEST(LayoutBlocks, DetectsUnicodeBallotAndBracketTaskListItems) {
    std::vector<FreeGlyph> gs;
    gs.push_back({0x2610, 72.0, 720.0});
    put_token(gs, "Unchecked task", 84.0, 720.0);

    gs.push_back({0x2611, 72.0, 704.0});
    put_token(gs, "Checked task", 84.0, 704.0);

    gs.push_back({0x2612, 96.0, 688.0});
    put_token(gs, "Nested checked task", 108.0, 688.0);

    const pdf2md::Document doc = analyze_page(make_free_page(gs));
    ASSERT_EQ(doc.blocks.size(), 3u);

    EXPECT_EQ(doc.blocks[0].kind, pdf2md::BlockKind::ListItem);
    EXPECT_EQ(doc.blocks[0].task_state, pdf2md::TaskState::Unchecked);
    EXPECT_FALSE(doc.blocks[0].list_ordered);
    EXPECT_EQ(doc.blocks[0].list_indent, 0);
    expect_block_lines(doc.blocks[0], pdf2md::BlockKind::ListItem, {U"Unchecked task"});

    EXPECT_EQ(doc.blocks[1].kind, pdf2md::BlockKind::ListItem);
    EXPECT_EQ(doc.blocks[1].task_state, pdf2md::TaskState::Checked);
    EXPECT_FALSE(doc.blocks[1].list_ordered);
    EXPECT_EQ(doc.blocks[1].list_indent, 0);
    expect_block_lines(doc.blocks[1], pdf2md::BlockKind::ListItem, {U"Checked task"});

    EXPECT_EQ(doc.blocks[2].kind, pdf2md::BlockKind::ListItem);
    EXPECT_EQ(doc.blocks[2].task_state, pdf2md::TaskState::Checked);
    EXPECT_FALSE(doc.blocks[2].list_ordered);
    EXPECT_EQ(doc.blocks[2].list_indent, 1);
    expect_block_lines(doc.blocks[2], pdf2md::BlockKind::ListItem, {U"Nested checked task"});
}

TEST(LayoutTable, InfersColumnAlignmentOnTextAndRuledTables) {
    auto token_width = [](std::string_view s) {
        return static_cast<double>(s.size() - 1U) * (kBodySize * kGlyphAdvanceRatio) +
               kBodySize * kGlyphWidthRatio;
    };
    auto put_centered = [&](std::vector<FreeGlyph>& gs, std::string_view s, double x_center,
                            double y, bool bold = false) {
        put_token(gs, s, x_center - 0.5 * token_width(s), y, kBodySize, bold);
    };
    auto put_right = [&](std::vector<FreeGlyph>& gs, std::string_view s, double x_right, double y,
                         bool bold = false) {
        put_token(gs, s, x_right - token_width(s), y, kBodySize, bold);
    };

    // 1. Text-geometry table: left column -> None, centered column -> Center, numeric -> Right.
    {
        std::vector<FreeGlyph> gs;
        put_token(gs, "Item", 72.0, 700.0, kBodySize, true);
        put_centered(gs, "Status", 240.0, 700.0, true);
        put_right(gs, "Amount", 400.0, 700.0, true);

        put_token(gs, "a", 72.0, 684.0);
        put_centered(gs, "ok", 240.0, 684.0);
        put_right(gs, "9", 400.0, 684.0);

        put_token(gs, "medium", 72.0, 668.0);
        put_centered(gs, "pending", 240.0, 668.0);
        put_right(gs, "420", 400.0, 668.0);

        put_token(gs, "verylongname", 72.0, 652.0);
        put_centered(gs, "validated", 240.0, 652.0);
        put_right(gs, "128,500", 400.0, 652.0);

        const pdf2md::Document doc = analyze_page(make_free_page(gs));
        ASSERT_EQ(doc.blocks.size(), 1u);
        ASSERT_EQ(doc.blocks[0].kind, pdf2md::BlockKind::Table);
        ASSERT_EQ(doc.blocks[0].table_align.size(), 3u);
        EXPECT_EQ(doc.blocks[0].table_align[0], pdf2md::TableAlign::None);
        EXPECT_EQ(doc.blocks[0].table_align[1], pdf2md::TableAlign::Center);
        EXPECT_EQ(doc.blocks[0].table_align[2], pdf2md::TableAlign::Right);
    }

    // 2. Ruled-lattice table with borders {70, 190, 330, 470}.
    {
        std::vector<FreeGlyph> gs;
        put_token(gs, "Name", 76.0, 706.0, kBodySize, true);
        put_centered(gs, "State", 260.0, 706.0, true);
        put_right(gs, "Total", 464.0, 706.0, true);

        put_token(gs, "alpha", 76.0, 684.0);
        put_centered(gs, "ok", 260.0, 684.0);
        put_right(gs, "12", 464.0, 684.0);

        put_token(gs, "beta_item", 76.0, 662.0);
        put_centered(gs, "running", 260.0, 662.0);
        put_right(gs, "98,400", 464.0, 662.0);

        pdf2md::PageData page = make_free_page(gs);
        draw_grid(page, {70.0, 190.0, 330.0, 470.0}, {720.0, 698.0, 676.0, 654.0});

        const pdf2md::Document doc = analyze_page(page);
        ASSERT_EQ(doc.blocks.size(), 1u);
        ASSERT_EQ(doc.blocks[0].kind, pdf2md::BlockKind::Table);
        ASSERT_EQ(doc.blocks[0].table_align.size(), 3u);
        EXPECT_EQ(doc.blocks[0].table_align[0], pdf2md::TableAlign::None);
        EXPECT_EQ(doc.blocks[0].table_align[1], pdf2md::TableAlign::Center);
        EXPECT_EQ(doc.blocks[0].table_align[2], pdf2md::TableAlign::Right);
    }
}

TEST(LayoutHeadings, PromotesOutlineMatchedBlocksAndIsolatedNumberedBoldTitles) {
    // 1. DocMeta::outlines promotes matching unbolded body-size paragraphs to Heading.
    {
        std::vector<FreeGlyph> gs;
        put_body(gs, 8, 760.0);
        put_token(gs, "Overview", 72.0, 600.0, kBodySize, false);
        put_body(gs, 3, 575.0);
        put_token(gs, "Architecture", 72.0, 515.0, kBodySize, false);
        put_token(gs, "Data Flow", 72.0, 480.0, kBodySize, false);

        const pdf2md::PageData page = make_free_page(gs);
        pdf2md::DocMeta meta;
        meta.page_count = 1;
        meta.outlines = {
            {"Overview", 1, 0, 610.0, true},
            {"Architecture", 1, 0, 525.0, true},
            {"Data Flow", 2, 0, 490.0, true},
        };

        const pdf2md::Document promoted =
            pdf2md::analyze_layout(meta, {page}, {.detect_headings = true});
        ASSERT_EQ(promoted.blocks.size(), 5u);
        expect_block_lines(promoted.blocks[1], pdf2md::BlockKind::Heading, {U"Overview"});
        EXPECT_EQ(promoted.blocks[1].heading_level, 1);
        expect_block_lines(promoted.blocks[3], pdf2md::BlockKind::Heading, {U"Architecture"});
        EXPECT_EQ(promoted.blocks[3].heading_level, 1);
        expect_block_lines(promoted.blocks[4], pdf2md::BlockKind::Heading, {U"Data Flow"});
        EXPECT_EQ(promoted.blocks[4].heading_level, 2);

        const pdf2md::Document suppressed =
            pdf2md::analyze_layout(meta, {page}, {.detect_headings = false});
        ASSERT_EQ(suppressed.blocks.size(), 5u);
        EXPECT_EQ(suppressed.blocks[1].kind, pdf2md::BlockKind::Paragraph);
        EXPECT_EQ(suppressed.blocks[3].kind, pdf2md::BlockKind::Paragraph);
        EXPECT_EQ(suppressed.blocks[4].kind, pdf2md::BlockKind::Paragraph);
    }

    // 2. Isolated bold "1. Introduction" / "2. Architecture" promote to Heading with number
    // restored, whereas adjacent sibling bold ordered list items stay ListItems.
    {
        std::vector<FreeGlyph> gs;
        put_body(gs, 8, 760.0);
        put_token(gs, "1. Introduction", 72.0, 620.0, 12.0, true);
        put_body(gs, 4, 590.0);
        put_token(gs, "2. Architecture", 72.0, 510.0, 12.0, true);
        put_body(gs, 4, 480.0);
        put_token(gs, "1. First step", 72.0, 400.0, kBodySize, true);
        put_token(gs, "2. Second step", 72.0, 384.0, kBodySize, true);

        const pdf2md::Document doc = analyze_page(make_free_page(gs));
        ASSERT_EQ(doc.blocks.size(), 7u);
        expect_block_lines(doc.blocks[1], pdf2md::BlockKind::Heading, {U"1. Introduction"});
        expect_block_lines(doc.blocks[3], pdf2md::BlockKind::Heading, {U"2. Architecture"});
        expect_block_lines(doc.blocks[5], pdf2md::BlockKind::ListItem, {U"First step"});
        EXPECT_TRUE(doc.blocks[5].list_ordered);
        expect_block_lines(doc.blocks[6], pdf2md::BlockKind::ListItem, {U"Second step"});
        EXPECT_TRUE(doc.blocks[6].list_ordered);
    }
}

TEST(LayoutImages, PlacesTwoColumnFiguresInMatchingColumnAndPairsCaptionAltText) {
    // 1. Right-column figure stays inside the right column (after all left-column prose,
    // including a left-column paragraph with y_top = 498 < image y_bottom = 540) and pairs
    // with its adjacent "Figure 2: ..." caption to populate Block::image_alt.
    std::vector<FreeGlyph> gs;
    for (int i = 0; i < 6; ++i) {
        const double top_y = 740.0 - static_cast<double>(i) * 12.0;
        const double mid_y = 656.0 - static_cast<double>(i) * 12.0;
        const double low_y = 572.0 - static_cast<double>(i) * 12.0;
        const double right_bot_y = 480.0 - static_cast<double>(i) * 12.0;
        put_tight(gs, (i == 5) ? "LeftTopParagraphEnds." : "LeftTopColumnProseLine", 72.0, top_y);
        put_tight(gs, (i == 5) ? "LeftMidParagraphEnds." : "LeftMidColumnProseLine", 72.0, mid_y);
        put_tight(gs, (i == 5) ? "LeftLowParagraphEnds." : "LeftLowColumnProseLine", 72.0, low_y);
        put_tight(gs, (i == 5) ? "RightTopParagraphEnds." : "RightTopColumnProseLine", 330.0,
                  top_y);
        put_tight(gs, (i == 5) ? "RightBottomParagraphEnds." : "RightBottomColumnProse", 330.0,
                  right_bot_y);
    }
    for (int j = 0; j < 5; ++j) {
        const double left_bot_y = 488.0 - static_cast<double>(j) * 12.0;
        put_tight(gs, (j == 4) ? "LeftBottomParagraphEnds." : "LeftBottomColumnProse", 72.0,
                  left_bot_y);
    }
    put_token(gs, "Figure 2: Trace tree overview.", 330.0, 520.0);

    pdf2md::PageData two_col_page = make_free_page(gs);
    two_col_page.images.push_back({"images/right_fig.png", 330.0, 660.0, 540.0, 530.0, ""});

    const pdf2md::Document doc = analyze_page(two_col_page);
    ASSERT_EQ(doc.blocks.size(), 8u);
    EXPECT_EQ(doc.blocks[0].kind, pdf2md::BlockKind::Paragraph);
    ASSERT_EQ(doc.blocks[0].lines.size(), 6u);
    EXPECT_EQ(doc.blocks[0].lines[5].plain, U"LeftTopParagraphEnds.");

    EXPECT_EQ(doc.blocks[1].kind, pdf2md::BlockKind::Paragraph);
    ASSERT_EQ(doc.blocks[1].lines.size(), 6u);
    EXPECT_EQ(doc.blocks[1].lines[5].plain, U"LeftMidParagraphEnds.");

    EXPECT_EQ(doc.blocks[2].kind, pdf2md::BlockKind::Paragraph);
    ASSERT_EQ(doc.blocks[2].lines.size(), 6u);
    EXPECT_EQ(doc.blocks[2].lines[5].plain, U"LeftLowParagraphEnds.");

    EXPECT_EQ(doc.blocks[3].kind, pdf2md::BlockKind::Paragraph);
    ASSERT_EQ(doc.blocks[3].lines.size(), 5u);
    EXPECT_EQ(doc.blocks[3].lines[4].plain, U"LeftBottomParagraphEnds.");

    EXPECT_EQ(doc.blocks[4].kind, pdf2md::BlockKind::Paragraph);
    ASSERT_EQ(doc.blocks[4].lines.size(), 6u);
    EXPECT_EQ(doc.blocks[4].lines[5].plain, U"RightTopParagraphEnds.");

    EXPECT_EQ(doc.blocks[5].kind, pdf2md::BlockKind::Image);
    EXPECT_EQ(doc.blocks[5].image_path, "images/right_fig.png");
    EXPECT_EQ(doc.blocks[5].image_alt, "Figure 2: Trace tree overview.");

    expect_block_lines(doc.blocks[6], pdf2md::BlockKind::Paragraph,
                       {U"Figure 2: Trace tree overview."});

    EXPECT_EQ(doc.blocks[7].kind, pdf2md::BlockKind::Paragraph);
    ASSERT_EQ(doc.blocks[7].lines.size(), 6u);
    EXPECT_EQ(doc.blocks[7].lines[5].plain, U"RightBottomParagraphEnds.");

    // 2. Tagged PDF ImageRef::alt takes precedence over adjacent caption text.
    std::vector<FreeGlyph> tagged_gs;
    put_token(tagged_gs, "Figure 1: Visible caption below.", 72.0, 470.0);
    pdf2md::PageData tagged_page = make_free_page(tagged_gs);
    tagged_page.images.push_back(
        {"images/tagged.png", 72.0, 680.0, 490.0, 400.0, "Tagged architecture diagram"});

    const pdf2md::Document tagged_doc = analyze_page(tagged_page);
    ASSERT_EQ(tagged_doc.blocks.size(), 2u);
    EXPECT_EQ(tagged_doc.blocks[0].kind, pdf2md::BlockKind::Image);
    EXPECT_EQ(tagged_doc.blocks[0].image_alt, "Tagged architecture diagram");
    expect_block_lines(tagged_doc.blocks[1], pdf2md::BlockKind::Paragraph,
                       {U"Figure 1: Visible caption below."});
}

static_assert(std::is_copy_constructible_v<pdf2md::LayoutAnalyzer>);
static_assert(std::is_copy_assignable_v<pdf2md::LayoutAnalyzer>);
static_assert(std::is_nothrow_move_constructible_v<pdf2md::LayoutAnalyzer> &&
              std::is_nothrow_move_assignable_v<pdf2md::LayoutAnalyzer>);

void expect_styled_runs_identical(const std::vector<pdf2md::StyledRun>& actual,
                                  const std::vector<pdf2md::StyledRun>& expected) {
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(actual[i].text, expected[i].text);
        EXPECT_EQ(actual[i].bold, expected[i].bold);
        EXPECT_EQ(actual[i].italic, expected[i].italic);
        EXPECT_EQ(actual[i].mono, expected[i].mono);
        EXPECT_EQ(actual[i].math, expected[i].math);
        EXPECT_EQ(actual[i].raw, expected[i].raw);
        EXPECT_EQ(actual[i].strikethrough, expected[i].strikethrough);
        EXPECT_EQ(actual[i].link_id, expected[i].link_id);
        EXPECT_EQ(actual[i].link_url, expected[i].link_url);
    }
}

void expect_documents_equal(const pdf2md::Document& actual, const pdf2md::Document& expected) {
    EXPECT_EQ(actual.meta.title, expected.meta.title);
    EXPECT_EQ(actual.meta.author, expected.meta.author);
    EXPECT_EQ(actual.meta.page_count, expected.meta.page_count);
    EXPECT_EQ(actual.meta.subject, expected.meta.subject);
    EXPECT_EQ(actual.meta.keywords, expected.meta.keywords);
    ASSERT_EQ(actual.meta.outlines.size(), expected.meta.outlines.size());
    for (std::size_t i = 0; i < expected.meta.outlines.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(actual.meta.outlines[i].title, expected.meta.outlines[i].title);
        EXPECT_EQ(actual.meta.outlines[i].level, expected.meta.outlines[i].level);
        EXPECT_EQ(actual.meta.outlines[i].page, expected.meta.outlines[i].page);
        EXPECT_DOUBLE_EQ(actual.meta.outlines[i].y, expected.meta.outlines[i].y);
        EXPECT_EQ(actual.meta.outlines[i].has_y, expected.meta.outlines[i].has_y);
    }

    EXPECT_DOUBLE_EQ(actual.body_size, expected.body_size);
    ASSERT_EQ(actual.blocks.size(), expected.blocks.size());
    for (std::size_t b = 0; b < expected.blocks.size(); ++b) {
        SCOPED_TRACE(b);
        const pdf2md::Block& ab = actual.blocks[b];
        const pdf2md::Block& eb = expected.blocks[b];
        EXPECT_EQ(ab.kind, eb.kind);
        EXPECT_EQ(ab.page, eb.page);
        EXPECT_DOUBLE_EQ(ab.x0, eb.x0);
        EXPECT_DOUBLE_EQ(ab.x1, eb.x1);
        EXPECT_DOUBLE_EQ(ab.y_top, eb.y_top);
        EXPECT_DOUBLE_EQ(ab.y_bottom, eb.y_bottom);
        EXPECT_DOUBLE_EQ(ab.size, eb.size);
        EXPECT_EQ(ab.bold_majority, eb.bold_majority);
        EXPECT_EQ(ab.heading_level, eb.heading_level);
        EXPECT_EQ(ab.list_indent, eb.list_indent);
        EXPECT_EQ(ab.list_ordered, eb.list_ordered);
        EXPECT_EQ(ab.blockquote, eb.blockquote);
        EXPECT_EQ(ab.is_footnote, eb.is_footnote);
        EXPECT_EQ(ab.footnote_numbered, eb.footnote_numbered);
        EXPECT_EQ(ab.footnote_label, eb.footnote_label);
        EXPECT_EQ(ab.list_marker, eb.list_marker);
        EXPECT_EQ(ab.col_edges, eb.col_edges);
        EXPECT_EQ(ab.image_path, eb.image_path);
        EXPECT_EQ(ab.math_lines, eb.math_lines);
        EXPECT_EQ(ab.math_tag, eb.math_tag);
        EXPECT_EQ(ab.table_align, eb.table_align);
        EXPECT_EQ(ab.image_alt, eb.image_alt);
        EXPECT_EQ(ab.task_state, eb.task_state);

        ASSERT_EQ(ab.lines.size(), eb.lines.size());
        for (std::size_t l = 0; l < eb.lines.size(); ++l) {
            SCOPED_TRACE(l);
            EXPECT_EQ(ab.lines[l].plain, eb.lines[l].plain);
            expect_styled_runs_identical(ab.lines[l].runs, eb.lines[l].runs);
        }

        ASSERT_EQ(ab.table_cells.size(), eb.table_cells.size());
        for (std::size_t r = 0; r < eb.table_cells.size(); ++r) {
            SCOPED_TRACE(r);
            ASSERT_EQ(ab.table_cells[r].size(), eb.table_cells[r].size());
            for (std::size_t c = 0; c < eb.table_cells[r].size(); ++c) {
                SCOPED_TRACE(c);
                expect_styled_runs_identical(ab.table_cells[r][c], eb.table_cells[r][c]);
            }
        }
    }
}

TEST(LayoutBlocks, PimplAnalyzerSupportsCopyMoveAndReusablePassPipeline) {
    constexpr int kBodyLineCount = 6;
    constexpr int kPageZero = 0;
    constexpr int kPageOne = 1;
    constexpr int kTwoPageCount = 2;
    constexpr int kHeadingLevel1 = 1;
    constexpr int kHeadingLevel2 = 2;
    constexpr double kBodyTopY = 760.0;
    constexpr double kMajorHeadingY = 620.0;
    constexpr double kSubHeadingY = 580.0;
    constexpr double kMajorHeadingSize = 15.0;
    constexpr double kSubHeadingSize = 12.0;
    constexpr double kFootnoteLabelY = 123.6;
    constexpr double kFootnoteBodyY = 120.0;
    constexpr double kFootnoteLabelSize = 6.0;
    constexpr double kFootnoteTextX = 79.0;
    constexpr double kFootnoteSecondWordX = 122.0;
    constexpr double kTextTableCol0X = 72.0;
    constexpr double kTextTableCol1X = 150.0;
    constexpr double kTextTableCol2X = 210.0;
    constexpr double kTextTableRow0Y = 700.0;
    constexpr double kTextTableRow1Y = 684.0;
    constexpr double kTextTableRow2Y = 668.0;
    constexpr std::size_t kExpectedDoc1Blocks = 4U;
    constexpr std::size_t kExpectedDoc2Blocks = 5U;

    // Multi-page input #1: outline + numbered heading, numbered footnote on page 0
    // (floated to document end), and a ruled-lattice table on page 1.
    pdf2md::DocMeta meta1;
    meta1.title = "Pipeline Spec One";
    meta1.author = "Author One";
    meta1.page_count = kTwoPageCount;
    meta1.outlines = {{"1 Overview", kHeadingLevel1, kPageZero, kMajorHeadingY + kBodySize, true}};

    std::vector<FreeGlyph> doc1_p0_gs;
    put_body(doc1_p0_gs, kBodyLineCount, kBodyTopY);
    put_token(doc1_p0_gs, "1 Overview", kLeftMargin, kMajorHeadingY, kMajorHeadingSize, true);
    doc1_p0_gs.push_back({U'1', kLeftMargin, kFootnoteLabelY, kFootnoteLabelSize});
    put_token(doc1_p0_gs, "alpha", kFootnoteTextX, kFootnoteBodyY);
    put_token(doc1_p0_gs, "note", kFootnoteSecondWordX, kFootnoteBodyY);

    std::vector<pdf2md::PageData> pages1 = {
        make_free_page(doc1_p0_gs),
        ruled_table_page({{"Metric", "Unit", "Target"}, {"Latency", "ms", "15"}}, true),
    };
    pages1[0].index = kPageZero;
    pages1[1].index = kPageOne;

    // Multi-page input #2: nested headings, in-flow symbol footnote on page 0 (no outlines,
    // no numbered footnotes), and a text-geometry table on page 1 (empty col_edges).
    pdf2md::DocMeta meta2;
    meta2.title = "Pipeline Spec Two";
    meta2.author = "Author Two";
    meta2.page_count = kTwoPageCount;

    std::vector<FreeGlyph> doc2_p0_gs;
    put_body(doc2_p0_gs, kBodyLineCount, kBodyTopY);
    put_token(doc2_p0_gs, "2 Architecture", kLeftMargin, kMajorHeadingY, kMajorHeadingSize, true);
    put_token(doc2_p0_gs, "2.1 Passes", kLeftMargin, kSubHeadingY, kSubHeadingSize, true);
    doc2_p0_gs.push_back({0x2020, kLeftMargin, kFootnoteLabelY, kFootnoteLabelSize});
    put_token(doc2_p0_gs, "dagger", kFootnoteTextX, kFootnoteBodyY);
    put_token(doc2_p0_gs, "remark", kFootnoteSecondWordX, kFootnoteBodyY);

    std::vector<FreeGlyph> doc2_p1_gs;
    put_token(doc2_p1_gs, "Name", kTextTableCol0X, kTextTableRow0Y);
    put_token(doc2_p1_gs, "Lo", kTextTableCol1X, kTextTableRow0Y);
    put_token(doc2_p1_gs, "Hi", kTextTableCol2X, kTextTableRow0Y);
    put_token(doc2_p1_gs, "aa", kTextTableCol0X, kTextTableRow1Y);
    put_token(doc2_p1_gs, "10", kTextTableCol1X, kTextTableRow1Y);
    put_token(doc2_p1_gs, "20", kTextTableCol2X, kTextTableRow1Y);
    put_token(doc2_p1_gs, "bb", kTextTableCol0X, kTextTableRow2Y);
    put_token(doc2_p1_gs, "11", kTextTableCol1X, kTextTableRow2Y);
    put_token(doc2_p1_gs, "21", kTextTableCol2X, kTextTableRow2Y);

    std::vector<pdf2md::PageData> pages2 = {
        make_free_page(doc2_p0_gs),
        make_free_page(doc2_p1_gs),
    };
    pages2[0].index = kPageZero;
    pages2[1].index = kPageOne;

    const pdf2md::LayoutAnalyzer analyzer(pdf2md::AnalyzeParams{});

    const pdf2md::Document doc1 = analyzer.analyze(meta1, pages1);
    ASSERT_EQ(doc1.blocks.size(), kExpectedDoc1Blocks);
    EXPECT_EQ(doc1.blocks[0].kind, pdf2md::BlockKind::Paragraph);
    expect_block_lines(doc1.blocks[1], pdf2md::BlockKind::Heading, {U"1 Overview"});
    EXPECT_EQ(doc1.blocks[1].heading_level, kHeadingLevel1);
    expect_table_cells(doc1.blocks[2], {{U"Metric", U"Unit", U"Target"}, {U"Latency", U"ms", U"15"}});
    EXPECT_FALSE(doc1.blocks[2].col_edges.empty());
    EXPECT_TRUE(doc1.blocks[3].is_footnote);
    EXPECT_TRUE(doc1.blocks[3].footnote_numbered);
    EXPECT_EQ(doc1.blocks[3].footnote_label, U"1");
    expect_block_lines(doc1.blocks[3], pdf2md::BlockKind::Paragraph, {U"alpha note"});
    expect_documents_equal(doc1, pdf2md::analyze_layout(meta1, pages1));

    const pdf2md::Document doc2 = analyzer.analyze(meta2, pages2);
    ASSERT_EQ(doc2.blocks.size(), kExpectedDoc2Blocks);
    EXPECT_EQ(doc2.blocks[0].kind, pdf2md::BlockKind::Paragraph);
    expect_block_lines(doc2.blocks[1], pdf2md::BlockKind::Heading, {U"2 Architecture"});
    EXPECT_EQ(doc2.blocks[1].heading_level, kHeadingLevel1);
    expect_block_lines(doc2.blocks[2], pdf2md::BlockKind::Heading, {U"2.1 Passes"});
    EXPECT_EQ(doc2.blocks[2].heading_level, kHeadingLevel2);
    EXPECT_TRUE(doc2.blocks[3].is_footnote);
    EXPECT_FALSE(doc2.blocks[3].footnote_numbered);
    EXPECT_EQ(doc2.blocks[3].footnote_label, U"\u2020");
    expect_block_lines(doc2.blocks[3], pdf2md::BlockKind::Paragraph, {U"dagger remark"});
    expect_table_cells(doc2.blocks[4],
                       {{U"Name", U"Lo", U"Hi"}, {U"aa", U"10", U"20"}, {U"bb", U"11", U"21"}});
    EXPECT_TRUE(doc2.blocks[4].col_edges.empty());
    expect_documents_equal(doc2, pdf2md::analyze_layout(meta2, pages2));

    // Re-running input #1 on the same const LayoutAnalyzer produces identical output.
    expect_documents_equal(analyzer.analyze(meta1, pages1), doc1);

    // Copy construction, copy assignment (rebuilding cached pipeline options), and self-copy.
    const pdf2md::AnalyzeParams flat_params{
        .detect_headings = false, .detect_tables = false, .strip_headers_footers = false};
    pdf2md::LayoutAnalyzer mutable_analyzer(flat_params);
    const pdf2md::Document flat_doc1 = mutable_analyzer.analyze(meta1, pages1);
    expect_documents_equal(flat_doc1, pdf2md::analyze_layout(meta1, pages1, flat_params));
    ASSERT_EQ(flat_doc1.blocks.size(), kExpectedDoc1Blocks);
    EXPECT_EQ(flat_doc1.blocks[1].kind, pdf2md::BlockKind::Paragraph);
    EXPECT_EQ(flat_doc1.blocks[2].kind, pdf2md::BlockKind::Paragraph);

    const pdf2md::LayoutAnalyzer copied_analyzer(analyzer);
    expect_documents_equal(copied_analyzer.analyze(meta2, pages2), doc2);

    mutable_analyzer = copied_analyzer;
    const pdf2md::LayoutAnalyzer& self_alias = mutable_analyzer;
    mutable_analyzer = self_alias;
    expect_documents_equal(mutable_analyzer.analyze(meta1, pages1), doc1);
    expect_documents_equal(mutable_analyzer.analyze(meta2, pages2), doc2);

    // Move construction, move assignment, and reassignment of a moved-from instance.
    pdf2md::LayoutAnalyzer move_constructed(std::move(mutable_analyzer));
    expect_documents_equal(move_constructed.analyze(meta2, pages2), doc2);

    pdf2md::LayoutAnalyzer move_assigned(flat_params);
    move_assigned = std::move(move_constructed);
    expect_documents_equal(move_assigned.analyze(meta1, pages1), doc1);

    mutable_analyzer = pdf2md::LayoutAnalyzer(flat_params);
    expect_documents_equal(mutable_analyzer.analyze(meta1, pages1), flat_doc1);
}

}  // namespace
