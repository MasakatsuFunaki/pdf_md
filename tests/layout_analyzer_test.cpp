// Layout-analysis regression tests driven by synthetic character streams.

#include <array>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "document_model.h"
#include "layout_analyzer.h"
#include "markdown_writer.h"

using ::testing::HasSubstr;
using ::testing::Not;

namespace {

// Builds a single-line page from words. Between words the break space is given
// the SAME origin x as the next word's first glyph -- exactly how PDFium places
// generated word-break spaces -- and is emitted *after* that glyph. That is the
// order an unstable glyph sort can leave behind; the analyzer must re-seat the
// space ahead of the glyph, otherwise the break slides one character into the
// next word ("of the" -> "oft he"). Real example: the arxiv "Attention Is All
// You Need" body text, which was riddled with "oft he", "s oftmax", "g row".
pdf2md::PageData make_line(const std::vector<std::string>& words, double y) {
    pdf2md::PageData page;
    page.width = 612;
    page.height = 792;
    double x = 72;
    auto push = [&](char32_t code, double left, bool space) {
        pdf2md::CharInfo c;
        c.code = space ? U' ' : code;
        c.x = left;
        c.left = left;
        c.right = left + (space ? 0.0 : 5.0);
        c.y = y;
        c.bottom = y;
        c.top = y + 10;
        c.size = 10;
        c.space = space;
        page.chars.push_back(c);
    };
    for (size_t w = 0; w < words.size(); ++w) {
        for (size_t k = 0; k < words[w].size(); ++k) {
            const auto cp = static_cast<char32_t>(words[w][k]);
            if (w > 0 && k == 0) {
                push(cp, x, false);   // next word's first glyph, then...
                push(U' ', x, true);  // ...its break space at the same x
            } else {
                push(cp, x, false);
            }
            x += 6;  // uniform pitch: intra- and inter-word gaps stay below the
                     // word-gap threshold, so only the explicit space separates
        }
    }
    return page;
}

std::string convert(const pdf2md::PageData& page) {
    pdf2md::DocMeta meta;
    std::vector<pdf2md::PageData> pages{page};
    pdf2md::Document doc = pdf2md::analyze_layout(meta, pages, {});
    return pdf2md::write_markdown(doc, {});
}

// Analyzes a whole multi-page document (each page keeps its own index, so
// cross-page passes -- the reference and ruled-table stitchers -- can see the
// page ordering they key on).
std::string convert_pages(std::vector<pdf2md::PageData> pages) {
    pdf2md::DocMeta meta;
    for (size_t i = 0; i < pages.size(); ++i) pages[i].index = static_cast<int>(i);
    pdf2md::Document doc = pdf2md::analyze_layout(meta, pages, {});
    return pdf2md::write_markdown(doc, {});
}

TEST(LayoutSpacing, CoincidentBreakSpaceStaysBeforeItsGlyph) {
    std::string md = convert(make_line({"we", "compute", "of", "the", "sum"}, 700));
    EXPECT_THAT(md, HasSubstr("we compute of the sum"));
    // The signature corruptions when the space is displaced by one glyph.
    EXPECT_THAT(md, Not(HasSubstr("oft he")));
    EXPECT_THAT(md, Not(HasSubstr("weo")));
    EXPECT_THAT(md, Not(HasSubstr("t hes")));
}

// One glyph of a synthetic math line: role 0 = body, 1 = superscript, 2 =
// subscript. Letters default to math italic, matching how PDF producers encode
// variables; upright letters model \text names and blackboard sets, and bold
// models \mathbf vectors.
struct MGlyph {
    char32_t code;
    int role = 0;
    bool italic = true;
    bool bold = false;
};

// Builds a single line from tokens (a token is a group of glyphs with no space
// inside). Scripts are drawn smaller and off the baseline, as PDF producers
// actually typeset them (calibrated from the arxiv paper: body 10pt on the
// baseline, scripts 7pt raised +3.6pt / lowered -1.5pt).
pdf2md::PageData make_math_line(const std::vector<std::vector<MGlyph>>& tokens, double y) {
    pdf2md::PageData page;
    page.width = 612;
    page.height = 792;
    double x = 72;
    auto push = [&](const MGlyph& g) {
        pdf2md::CharInfo c;
        const double size = g.role == 0 ? 10.0 : 7.0;
        const double dy = g.role == 1 ? 3.6 : (g.role == 2 ? -1.5 : 0.0);
        c.code = g.code;
        c.size = size;
        c.x = x;
        c.left = x;
        c.right = x + size * 0.5;
        c.y = y + dy;
        c.bottom = c.y;
        c.top = c.y + size;
        c.italic = g.italic;
        c.bold = g.bold;
        page.chars.push_back(c);
        x += size * 0.6;
    };
    for (size_t t = 0; t < tokens.size(); ++t) {
        if (t) x += 8.0;  // inter-token gap: a word break
        for (const MGlyph& g : tokens[t]) push(g);
    }
    return page;
}

// Upright (non-italic) helpers for readability.
MGlyph up(char32_t code, int role = 0) { return {code, role, /*italic=*/false, false}; }

TEST(LayoutMath, SuperscriptsAndSubscriptsBecomeLatex) {
    const std::vector<MGlyph> at = {up(U'a'), up(U't')};  // prose, to isolate
    std::string md = convert(make_math_line(
        {
            {up(U'l'), up(U'e'), up(U't')},                     // "let" (prose)
            {{U'd'}, {U'k', 2}},                                // d_k
            at,
            {{U'Q'}, {U'K'}, {U'T', 1}},                        // QK^T
            at,
            {{U'W'}, {U'i', 2}, {U'Q', 1}},                     // W_i^Q
            at,
            {up(U'R'), {U'a', 1}, {0x00D7, 1}, {U'b', 1}},      // R^{a x b}
        },
        700));
    // Single-character scripts drop their braces; multi-character ones keep them.
    EXPECT_THAT(md, HasSubstr("$d_k$"));
    EXPECT_THAT(md, HasSubstr("$QK^T$"));
    EXPECT_THAT(md, HasSubstr("$W_i^Q$"));
    // A word macro keeps a trailing space so it can't fuse with the next letter,
    // and an upright blackboard capital becomes \mathbb.
    EXPECT_THAT(md, HasSubstr("$\\mathbb{R}^{a\\times b}$"));
    // Prose in the same line is not swept into math.
    EXPECT_THAT(md, HasSubstr("let "));
    EXPECT_THAT(md, Not(HasSubstr("$let")));
}

TEST(LayoutMath, MultiLetterUprightSubscriptGetsText) {
    // d with an upright multi-letter subscript "model" -> d_{\text{model}}; the
    // flattened "dmodel" must never appear.
    std::string md = convert(make_math_line(
        {
            {{U'd'}, up(U'm', 2), up(U'o', 2), up(U'd', 2), up(U'e', 2), up(U'l', 2)},
        },
        700));
    EXPECT_THAT(md, HasSubstr("$d_{\\text{model}}$"));
    EXPECT_THAT(md, Not(HasSubstr("dmodel")));
}

TEST(LayoutMath, IsolatedBoldLetterIsMathbfVector) {
    // A lone bold letter is a vector; a bold word is just emphasised prose.
    std::string md = convert(make_math_line(
        {
            {{U'z', 0, /*italic=*/false, /*bold=*/true}},
            {{U'=', 0, false, false}},
            {{U'x'}, {U'1', 2}},  // a real atom routes the line through math
        },
        700));
    EXPECT_THAT(md, HasSubstr("\\mathbf{z}"));
}

TEST(LayoutMath, TupleAndOperatorsStayInsideOneMathSpan) {
    // "(x1,...,xn)" must render as one span with \ldots, never an italic
    // "*, ...,*" between broken atoms.
    std::string md = convert(make_math_line(
        {
            {up(U'l'), up(U'e'), up(U't')},
            {{U'('}, {U'x'}, {U'1', 2}, {U','}, {U'.'}, {U'.'}, {U'.'}, {U','},
             {U'x'}, {U'n', 2}, {U')'}},
        },
        700));
    EXPECT_THAT(md, HasSubstr("\\ldots"));
    EXPECT_THAT(md, Not(HasSubstr("*, ...,*")));
    EXPECT_THAT(md, HasSubstr("x_n"));
}

TEST(LayoutMath, DecimalPointNotEmphasisedByFontMix) {
    // A period from a different (italic) font between two upright digits must not
    // split "0.9" into "0*.*9".
    std::string md = convert(make_math_line(
        {
            {up(U'v'), up(U'a', 1)},                 // a superscript -> math line
            {up(U'0'), {U'.'}, up(U'9')},            // 0.9 with an italic period
        },
        700));
    EXPECT_THAT(md, Not(HasSubstr("0*.*9")));
    EXPECT_THAT(md, HasSubstr("0.9"));
}

TEST(LayoutMath, PlainProseIsNotWrappedInMath) {
    std::string md = convert(make_line({"the", "quick", "brown", "fox"}, 700));
    EXPECT_THAT(md, HasSubstr("the quick brown fox"));
    EXPECT_THAT(md, Not(HasSubstr("$")));
}

// A glyph placed freely in page space. `raised` marks a tall symbol (radical /
// big operator) whose origin sits at the top of its box, so it descends a full
// body below the baseline -- exactly how PDFium reports √ and Σ.
struct Free {
    char32_t code;
    double left;
    double baseline;
    double size = 10;
    bool italic = false;
    bool bold = false;
    bool raised = false;
};

pdf2md::PageData make_free_page(const std::vector<Free>& gs) {
    pdf2md::PageData page;
    page.width = 612;
    page.height = 792;
    for (const Free& g : gs) {
        pdf2md::CharInfo c;
        c.code = g.code;
        c.size = g.size;
        c.x = g.left;
        c.left = g.left;
        c.right = g.left + (g.code == U' ' ? 0.0 : g.size * 0.5);
        c.y = g.baseline;
        c.italic = g.italic;
        c.bold = g.bold;
        c.space = (g.code == U' ');
        if (g.raised) {  // origin at the box top; box descends a full size below
            c.top = g.baseline + g.size / 2.0;
            c.bottom = g.baseline - g.size / 2.0;
            c.y = c.top;  // reported baseline sits at the top edge
        } else {
            c.bottom = g.baseline;
            c.top = g.baseline + g.size;
        }
        page.chars.push_back(c);
    }
    return page;
}

TEST(LayoutMath, InlineFractionAndRadicalReconstructed) {
    // Prose "val", then an inline case fraction 1 / √y: numerator "1" a step
    // above the text baseline, "√y" a step below. The stacked pieces must fuse
    // into \frac{1}{\sqrt{y}}, never scatter as scripts of "val".
    std::string md = convert(make_free_page({
        {U'v', 72, 700}, {U'a', 78, 700}, {U'l', 84, 700}, {U' ', 90, 700},
        {U'1', 100, 705, 7},                        // numerator, a step above
        {0x221A, 96, 696, 7, false, false, true},   // radical, in the denominator
        {U'y', 105, 693, 7, true},                  // radicand, a step below
        {U'.', 112, 700},
    }));
    EXPECT_THAT(md, HasSubstr("\\frac{1}{\\sqrt{y}}"));
    EXPECT_THAT(md, HasSubstr("val"));
    EXPECT_THAT(md, Not(HasSubstr("val$")));  // "val" stays prose, not a math base
}

TEST(LayoutMath, BigOperatorCarriesStackedLimits) {
    // A summation with a limit centred above and below becomes \sum_i^n, not a
    // pair of diagonal scripts nor three broken lines.
    std::string md = convert(make_free_page({
        {U'l', 72, 700}, {U'e', 78, 700}, {U't', 84, 700}, {U' ', 90, 700},
        {0x2211, 100, 700, 10, false, false, true},  // Σ, raised origin
        {U'n', 103, 707, 6},                          // upper limit
        {U'i', 103, 693, 6, true},                    // lower limit
        {U'x', 113, 700, 10, true},
    }));
    EXPECT_THAT(md, HasSubstr("\\sum_i^n"));
    EXPECT_THAT(md, Not(HasSubstr("$n")));  // the limit never orphans onto its own span
}

TEST(LayoutMath, CenteredEquationBecomesDisplayBlock) {
    // A body line fixes the column's left edge; a centred formula "z = a/b + c",
    // whose numerator/denominator land on their own baselines, is detected as a
    // display equation and its stack is rebuilt into \frac inside a $$ block.
    std::vector<Free> gs;
    for (int i = 0; i < 20; ++i)  // a wide body line at the left margin, y = 740
        gs.push_back({static_cast<char32_t>(U'a' + (i % 26)), 72.0 + i * 6.0, 740.0});
    // Centred equation at y = 700, far from the column's left edge.
    gs.push_back({U'z', 250, 700, 10, true});
    gs.push_back({U'n', 258, 697, 6, true});  // subscript: z_n
    gs.push_back({U' ', 262, 700});
    gs.push_back({U'=', 266, 700});
    gs.push_back({U' ', 278, 700});
    gs.push_back({U'a', 286, 706, 10, true});  // numerator, a baseline above
    gs.push_back({U'b', 286, 694, 10, true});  // denominator, a baseline below
    gs.push_back({U'+', 300, 700});
    gs.push_back({U'c', 312, 700, 10, true});
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("$$"));
    EXPECT_THAT(md, HasSubstr("\\frac{a}{b}"));
    EXPECT_THAT(md, HasSubstr("z_n = \\frac{a}{b}+c"));
}

// Places a left-to-right token of body glyphs at (x, y) into a free-page list.
void put_token(std::vector<Free>& gs, const std::string& s, double x, double y,
               double size = 10, bool bold = false) {
    for (char ch : s) {
        gs.push_back({static_cast<char32_t>(ch), x, y, size, /*italic=*/false, bold});
        x += size * 0.6;
    }
}

// Pushes a glyph run tightly (letter spacing), returning the pen x after it.
double put_tight(std::vector<Free>& gs, const std::string& s, double x, double y,
                 double size = 10) {
    for (char ch : s) {
        gs.push_back({static_cast<char32_t>(ch), x, y, size});
        x += size * 0.6;
    }
    return x;
}

TEST(LayoutSpacing, GeometricDigitSplitInsideNumberRejoins) {
    // A glyph-positioned numeric run whose paired digits sit slightly more than a
    // word gap apart (no space glyph between them) must not gain an interior space.
    std::vector<Free> gs;
    double x = put_tight(gs, "revision", 72, 700);
    x += 3.0;                       // word gap before the id
    x = put_tight(gs, "R25-1", x, 700);
    x += 3.4;                       // > word gap, but NO space glyph: a mis-split
    put_tight(gs, "1", x, 700);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("R25-11"));
    EXPECT_THAT(md, Not(HasSubstr("R25-1 1")));
}

TEST(LayoutSpacing, RealSpaceBetweenNumbersIsPreserved) {
    // Two distinct numbers separated by an actual space glyph are a real word
    // boundary and must stay apart (contrast the geometric mis-split above).
    std::vector<Free> gs;
    double x = put_tight(gs, "version", 72, 700);
    x += 3.0;
    x = put_tight(gs, "1.40", x, 700);
    gs.push_back({U' ', x, 700});   // an explicit separator glyph
    x += 3.0;
    put_tight(gs, "20131220", x, 700);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("1.40 20131220"));
    EXPECT_THAT(md, Not(HasSubstr("1.4020131220")));
}

TEST(LayoutSpacing, SpaceBeforeBoundPunctuationDropped) {
    // A word space that fell before punctuation binding to the preceding token
    // (period, comma, colon) is spurious and removed.
    std::vector<Free> gs;
    double x = put_tight(gs, "Amendment", 72, 700);
    x += 3.0;
    x = put_tight(gs, "1", x, 700);
    x += 3.4;                       // spurious gap before the comma
    x = put_tight(gs, ",", x, 700);
    x += 3.0;
    put_tight(gs, "Release", x, 700);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("Amendment 1, Release"));
    EXPECT_THAT(md, Not(HasSubstr("1 ,")));
}

TEST(LayoutSpacing, AssignmentOperatorSpacePreserved) {
    // The ":=" assignment operator keeps its leading space: the colon is part of
    // an operator, not label punctuation binding to the token on its left.
    std::vector<Free> gs;
    double x = put_tight(gs, "v6", 72, 700);
    x += 3.4;                       // gap that would otherwise be dropped
    put_tight(gs, ":= expr here now", x, 700);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("v6 :="));
    EXPECT_THAT(md, Not(HasSubstr("v6:=")));
}

TEST(LayoutSpacing, ScopeOperatorBindsMemberForward) {
    // A member after the "::" scope operator rejoins even across a spurious gap,
    // so a scoped identifier stays one token.
    std::vector<Free> gs;
    double x = put_tight(gs, "the", 72, 700);
    x += 3.0;
    x = put_tight(gs, "app", x, 700);
    x += 3.4; x = put_tight(gs, ":", x, 700);   // app ::
    x += 3.4; x = put_tight(gs, ":", x, 700);
    x += 3.4; put_tight(gs, "util here", x, 700);  // :: util
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("app::util"));
    EXPECT_THAT(md, Not(HasSubstr("app : : util")));
}

TEST(LayoutBlocks, HangingIndentReferenceStaysOneBlock) {
    // A reference entry: a marker line at the left margin wrapping to an indented
    // continuation. The wrap (a line that did not end a sentence) must not split
    // the entry, so the two lines merge into one paragraph.
    std::vector<Free> gs;
    // Marker line at x=72, wide, ending on a word (no sentence terminator).
    double x = 72;
    for (const char* w : {"author", "names", "and", "the", "paper", "title", "wide"}) {
        put_token(gs, w, x, 700);
        x += std::string(w).size() * 6.0 + 8.0;
    }
    // Continuation, indented under the text, closes the entry.
    double x2 = 92;
    for (const char* w : {"continued", "second", "line", "ends."}) {
        put_token(gs, w, x2, 688);
        x2 += std::string(w).size() * 6.0 + 8.0;
    }
    std::string md = convert(make_free_page(gs));
    // The wrap joins: "wide" and "continued" land in the same paragraph.
    EXPECT_THAT(md, HasSubstr("wide continued"));
}

TEST(LayoutBlocks, CitationEntrySurvivesInternalSentenceBreak) {
    // A numbered bibliography entry ("[19] ...") set with a hanging indent: the
    // "[19]" label sits at the left margin and the wrapped line is inset under the
    // text. Its first line ends on a full stop between citation fields
    // ("...networks."), which for ordinary prose would close a paragraph. The
    // bracketed-citation lead marks it as a reference whose fields are punctuated
    // internally, so the sentence break must NOT split the entry: the indented
    // continuation belongs to the same reference.
    std::vector<Free> gs;
    // Marker line at the left margin, wide, ending on a sentence terminator.
    double x = 72;
    for (const char* w : {"[19]", "Firstauthor", "and", "Secondauthor", "Lastname.",
                          "Some", "descriptive", "paper", "title."}) {
        put_token(gs, w, x, 700);
        x += std::string(w).size() * 6.0 + 8.0;
    }
    // Continuation, indented under the text, opening a new sentence.
    double x2 = 92;
    for (const char* w : {"In", "Proceedings", "of", "Venue", "2017."}) {
        put_token(gs, w, x2, 688);
        x2 += std::string(w).size() * 6.0 + 8.0;
    }
    std::string md = convert(make_free_page(gs));
    // The entry stays one paragraph: "title." joins its indented continuation.
    EXPECT_THAT(md, HasSubstr("title. In Proceedings"));
}

TEST(LayoutBlocks, NumberedFootnoteBecomesDefinition) {
    // A page-bottom footnote: a raised superscript number opening a line of body
    // text is detected, stripped, and re-emitted as a markdown footnote definition.
    std::vector<Free> gs;
    put_token(gs, "Body", 72, 740);
    put_token(gs, "paragraph", 110, 740);
    put_token(gs, "text", 190, 740);
    gs.push_back({U'5', 72, 703.6, 6});  // raised, small: a superscript marker
    put_token(gs, "footnote", 79, 700);
    put_token(gs, "body", 130, 700);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("[^5]: footnote body"));
    EXPECT_THAT(md, Not(HasSubstr("$5$")));
}

TEST(LayoutMath, FootnoteReferenceBecomesMarkdownLink) {
    // A superscript number on a whole prose word ("GPU5") is a footnote reference,
    // not a power: it renders as a "[^5]" link, never as $\text{GPU}^5$.
    std::string md = convert(make_math_line(
        {
            {up(U's'), up(U'e'), up(U'e')},
            {up(U'G'), up(U'P'), up(U'U'), up(U'5', 1)},
        },
        700));
    EXPECT_THAT(md, HasSubstr("GPU[^5]"));
    EXPECT_THAT(md, Not(HasSubstr("text{GPU}")));
}

TEST(LayoutMath, WordHyphenStaysOutsideMathSpan) {
    // A hyphen joining a math token to a prose word ("d_v-dimensional") is a word
    // hyphen: it and the word stay outside the span, giving "$d_v$-dimensional".
    std::string md = convert(make_math_line(
        {
            {{U'd'}, {U'v', 2}, up(U'-'), up(U'd'), up(U'i'), up(U'm'), up(U'e'), up(U'n'),
             up(U's'), up(U'i'), up(U'o'), up(U'n')},
        },
        700));
    EXPECT_THAT(md, HasSubstr("$d_v$-dimension"));
    EXPECT_THAT(md, Not(HasSubstr("$d_v-$")));
}

TEST(LayoutHeadings, SectionNumberSetsNestingDepth) {
    // Three headings sharing tiers by size: "2" (larger) and "2.1"/"2.1.1" (equal
    // size). The dotted section number must nest 2.1.1 one level below 2.1 even
    // though the two share a font size.
    std::vector<Free> gs;
    // Body text so the small headings stay well under the heading-tier char budget
    // and body size is 10.
    for (int line = 0; line < 8; ++line) {
        double y = 740.0 - line * 12.0;
        double x = 72;
        for (int w = 0; w < 9; ++w) {
            put_token(gs, "bodyword", x, y, 10);
            x += 56;
        }
    }
    // A word-sized (not column-sized) gap between the section number and the title
    // so they read as one heading line "N Title", but stay two words.
    put_token(gs, "2", 72, 620, 15, /*bold=*/true);
    put_token(gs, "Methods", 90, 620, 15, true);
    put_token(gs, "2.1", 72, 560, 12, true);
    put_token(gs, "Setup", 102, 560, 12, true);
    put_token(gs, "2.1.1", 72, 500, 12, true);
    put_token(gs, "Details", 116, 500, 12, true);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("## 2.1 Setup"));
    EXPECT_THAT(md, HasSubstr("### 2.1.1 Details"));
}

TEST(LayoutTable, GlobalColumnBoundarySplitsTightNeighbours) {
    // A 3-column grid whose header labels the columns. In the last data row the
    // two numeric cells are set close enough that a per-line gap would merge them
    // ("34 56"); the boundary fixed by the clearly-spaced rows must still split
    // them into separate cells, and the header row must be row 0.
    std::vector<Free> gs;
    put_token(gs, "Name", 72, 700);
    put_token(gs, "Lo", 150, 700);
    put_token(gs, "Hi", 210, 700);
    put_token(gs, "aa", 72, 684);
    put_token(gs, "10", 150, 684);
    put_token(gs, "20", 210, 684);
    put_token(gs, "bb", 72, 668);
    put_token(gs, "11", 150, 668);
    put_token(gs, "21", 210, 668);
    put_token(gs, "cc", 72, 652);
    put_token(gs, "34", 168, 652);  // sits close to the next cell...
    put_token(gs, "56", 186, 652);  // ...but the column boundary still splits it
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("| Name | Lo | Hi |"));
    EXPECT_THAT(md, HasSubstr("| --- | --- | --- |"));
    EXPECT_THAT(md, HasSubstr("| cc | 34 | 56 |"));
    EXPECT_THAT(md, Not(HasSubstr("34 56")));  // the two columns never merge
}

TEST(LayoutTable, DotLeaderlessTocEntryFoldsIntoTableRow) {
    // A table-of-contents entry runs long enough to leave no dot-leader gap before
    // its right-aligned page number, so its line carries a single column segment
    // and drops out as a bare paragraph between the two halves of its table. It
    // must fold back in as a row and stitch the halves into one continuous table.
    // Every page number is three digits sharing one right edge (273..290); each
    // half needs three rows to register as a two-column table.
    std::vector<Free> gs;
    put_token(gs, "Alpha", 72, 700);    put_token(gs, "101", 273, 700);
    put_token(gs, "Beta", 72, 684);     put_token(gs, "102", 273, 684);
    put_token(gs, "Gamma", 72, 668);    put_token(gs, "103", 273, 668);
    // The orphan: a long entry whose number sits just past it (a set-off gap below
    // the column threshold, so the line stays a single segment) yet right-aligned
    // to the page-number column.
    put_token(gs, "OrphanTocEntryTooLongForALeaderX", 72, 652);
    gs.push_back({U' ', 266, 652});     // a break space ahead of the number
    put_token(gs, "104", 273, 652);
    put_token(gs, "Delta", 72, 636);    put_token(gs, "105", 273, 636);
    put_token(gs, "Epsilon", 72, 620);  put_token(gs, "106", 273, 620);
    put_token(gs, "Zeta", 72, 604);     put_token(gs, "107", 273, 604);
    std::string md = convert(make_free_page(gs));

    EXPECT_THAT(md, HasSubstr("| OrphanTocEntryTooLongForALeaderX | 104 |"));
    EXPECT_THAT(md, Not(HasSubstr("OrphanTocEntryTooLongForALeaderX 104")));  // not bare
    auto count = [](const std::string& hay, const std::string& needle) {
        size_t n = 0, pos = 0;
        while ((pos = hay.find(needle, pos)) != std::string::npos) { ++n; pos += needle.size(); }
        return n;
    };
    EXPECT_EQ(count(md, "| --- | --- |"), 1u);  // the two halves became one table
}

TEST(LayoutTable, RuledCellWrapKeepsCompoundHyphenDropsPlainWrap) {
    // A ruled-grid cell whose text wraps at a hyphen: an ordinary word split
    // ("informa-"+"tion") drops the hyphen, but a compound identifier
    // ("{<data-record-name-"+"x>}") keeps it -- the interior hyphens and bracket
    // glue mark a literal name, not a syllable break.
    // Column-1 text starts clear of the middle post (x=185) so no glyph box
    // straddles the post and collapses the grid into one column.
    std::vector<Free> gs;
    put_token(gs, "Field", 70, 678);   put_token(gs, "Definition", 205, 678);
    put_token(gs, "PlainWrap", 70, 632);
    put_token(gs, "informa-", 205, 645);  put_token(gs, "tion", 205, 620);
    put_token(gs, "IdWrap", 70, 587);
    put_token(gs, "{<data-record-name-", 205, 600);  put_token(gs, "x>}", 205, 575);

    pdf2md::PageData page = make_free_page(gs);
    for (double y : {700.0, 655.0, 610.0, 565.0})
        page.rules.push_back({60, y, 400, y, /*horizontal=*/true});   // full-width rails
    for (double x : {60.0, 185.0, 400.0})
        page.rules.push_back({x, 565, x, 700, /*horizontal=*/false});  // full-height posts
    std::string md = convert(page);

    EXPECT_THAT(md, HasSubstr("information"));            // plain wrap dehyphenates
    EXPECT_THAT(md, Not(HasSubstr("informa-tion")));
    EXPECT_THAT(md, HasSubstr("data-record-name-x"));     // compound hyphen kept
    EXPECT_THAT(md, Not(HasSubstr("data-record-namex")));
}

TEST(LayoutTable, CellSuperscriptAndBoldSurvive) {
    // Cells route through the line renderer: a superscript becomes inline math
    // and a bold-highlighted value keeps its emphasis.
    std::vector<Free> gs;
    put_token(gs, "Key", 72, 700);
    put_token(gs, "Expr", 200, 700);
    put_token(gs, "a", 72, 684);
    gs.push_back({U'n', 200, 684, 10});       // base
    gs.push_back({U'2', 206, 687.6, 7});      // superscript -> n^2
    put_token(gs, "bb", 72, 668, 10, /*bold=*/true);  // bold highlighted cell
    put_token(gs, "99", 200, 668, 10, /*bold=*/true);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("$n^2$"));
    EXPECT_THAT(md, HasSubstr("**bb**"));
    EXPECT_THAT(md, HasSubstr("**99**"));
}

TEST(LayoutImages, FigurePlacedBelowHeadingAndAboveCaption) {
    // A tall figure region overlaps a section heading at its top edge and its
    // caption at its bottom edge -- exactly how a vector figure's bounding box
    // (inflated by baked-in labels) sits against the surrounding text. Anchoring
    // placement on the region centre must keep the heading above the image while
    // the image immediately precedes the caption below it.
    std::vector<Free> gs;
    put_token(gs, "Visualizations", 72, 700, 14, /*bold=*/true);  // heading near top
    put_token(gs, "Figure", 72, 470, 10);                         // caption near bottom
    put_token(gs, "3", 120, 470, 10);
    pdf2md::PageData page = make_free_page(gs);
    pdf2md::ImageRef img;
    img.path = "images/fig.png";
    img.x = 100;
    img.y_top = 690;     // centre 585: below the heading (700), above the caption (470)
    img.y_bottom = 480;
    page.images.push_back(img);

    std::string md = convert(page);
    size_t heading = md.find("Visualizations");
    size_t image = md.find("![](images/fig.png)");
    size_t caption = md.find("Figure");
    ASSERT_NE(heading, std::string::npos);
    ASSERT_NE(image, std::string::npos);
    ASSERT_NE(caption, std::string::npos);
    EXPECT_LT(heading, image);   // the heading stays above the figure
    EXPECT_LT(image, caption);   // the figure immediately precedes its caption
}

TEST(LayoutTable, GroupLabelFoldedIntoRowNotStandalone) {
    // A group label in the leftmost column, at a half-pitch offset between two
    // rows whose own column 0 is empty (as row-group labels sit in real tables),
    // is folded into a table row rather than emitted as a standalone paragraph.
    std::vector<Free> gs;
    put_token(gs, "Key", 72, 700);
    put_token(gs, "P", 150, 700);
    put_token(gs, "Q", 210, 700);
    put_token(gs, "base", 72, 684);  put_token(gs, "1", 150, 684); put_token(gs, "2", 210, 684);
    put_token(gs, "3", 150, 668);    put_token(gs, "4", 210, 668);   // empty column 0
    put_token(gs, "(A)", 72, 660);                                   // lone group label
    put_token(gs, "5", 150, 652);    put_token(gs, "6", 210, 652);   // empty column 0
    put_token(gs, "big", 72, 636);   put_token(gs, "7", 150, 636); put_token(gs, "8", 210, 636);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("(A)"));
    EXPECT_THAT(md, Not(HasSubstr("\n(A)\n")));  // never a standalone paragraph
    EXPECT_THAT(md, HasSubstr("| --- | --- | --- |"));
}

TEST(LayoutMath, ItalicFunctionNameBecomesOperatorMacro) {
    // "log" set in the surrounding math italic still becomes the upright operator
    // macro \log (a Unicode/LaTeX convention), carrying its subscript along.
    std::string md = convert(make_math_line(
        {
            {{U'l'}, {U'o'}, {U'g'}, {U'k', 2}, {U'('}, {U'n'}, {U')'}},  // log_k(n)
        },
        700));
    EXPECT_THAT(md, HasSubstr("\\log_k(n)"));
    EXPECT_THAT(md, Not(HasSubstr("$log")));
}

TEST(LayoutMath, ItalicFunctionNameInDisplayEquationGetsMacro) {
    // A centred equation "y_n = sin(x)"; the italic "sin" becomes \sin.
    std::vector<Free> gs;
    for (int i = 0; i < 20; ++i)  // body line fixes the column's left edge
        gs.push_back({static_cast<char32_t>(U'a' + (i % 26)), 72.0 + i * 6.0, 740.0});
    gs.push_back({U'y', 250, 700, 10, true});
    gs.push_back({U'n', 258, 697, 7, true});  // subscript: marks the line as math
    gs.push_back({U'=', 268, 700});
    gs.push_back({U's', 286, 700, 10, true});
    gs.push_back({U'i', 292, 700, 10, true});
    gs.push_back({U'n', 298, 700, 10, true});
    gs.push_back({U'(', 304, 700});
    gs.push_back({U'x', 310, 700, 10, true});
    gs.push_back({U')', 316, 700});
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("\\sin"));
    EXPECT_THAT(md, Not(HasSubstr("= sin")));
}

TEST(LayoutMath, AdjacentTextGroupsKeepVisibleSpace) {
    // In a display equation two upright words set side by side ("where head")
    // become adjacent \text{} groups. A bare math-mode space between them would
    // vanish ("wherehead"), so the gap folds inside the first group.
    std::vector<Free> gs;
    for (int i = 0; i < 20; ++i)
        gs.push_back({static_cast<char32_t>(U'a' + (i % 26)), 72.0 + i * 6.0, 740.0});
    gs.push_back({U'z', 250, 700, 10, true});
    gs.push_back({U'n', 258, 697, 7, true});   // z_n, makes the line math
    gs.push_back({U'=', 268, 700});
    put_token(gs, "where", 286, 700);          // upright -> \text{where}
    put_token(gs, "head", 330, 700);           // upright, base of a subscript
    gs.push_back({U'i', 354, 697, 7});          // subscript i on "head"
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("\\text{where }\\text{head}"));
    EXPECT_THAT(md, Not(HasSubstr("\\text{where}\\text{head}")));
}

TEST(LayoutMath, TrailingCitationLeavesMathSpan) {
    // A bracketed numeric reference after a formula ("d_k [3]") is a citation, so
    // it stays escaped prose outside the `$...$` rather than inside the span.
    std::string md = convert(make_math_line(
        {
            {{U'd'}, {U'k', 2}},        // d_k
            {{U'['}, {U'3'}, {U']'}},   // [3] citation marker
        },
        700));
    EXPECT_THAT(md, HasSubstr("$d_k$"));
    EXPECT_THAT(md, HasSubstr("\\[3]"));
    EXPECT_THAT(md, Not(HasSubstr("[3]$")));  // never absorbed into the math span
}

TEST(LayoutTable, WrappedHeaderCellFoldsIntoHeader) {
    // A header cell "Seq Ops" set on two lines: "Seq" in the header row and "Ops"
    // alone on the next line (no row label, only the inner column). It folds into
    // the header rather than surfacing as a phantom data row.
    std::vector<Free> gs;
    put_token(gs, "Type", 72, 700); put_token(gs, "Seq", 150, 700); put_token(gs, "Max", 210, 700);
    put_token(gs, "Ops", 150, 688);  // header continuation: columns 0 and 2 empty
    put_token(gs, "aaa", 72, 676); put_token(gs, "11", 150, 676); put_token(gs, "22", 210, 676);
    put_token(gs, "bbb", 72, 664); put_token(gs, "33", 150, 664); put_token(gs, "44", 210, 664);
    put_token(gs, "ccc", 72, 652); put_token(gs, "55", 150, 652); put_token(gs, "66", 210, 652);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("| Type | Seq Ops | Max |"));
    EXPECT_THAT(md, Not(HasSubstr("|  | Ops |  |")));  // no phantom continuation row
}

TEST(LayoutTable, WrappedValueColumnKeepsGridATable) {
    // A two-column glossary: a short label in column 0 and a wide value column
    // that wraps over several lines. Each wrap is a single-segment line indented
    // to the value column and wider than half the table -- without treating it as
    // a continuation of the value cell, the grid breaks after the first row and no
    // table is emitted; the labels then scatter as headings. The wrap must instead
    // keep the grid whole so it is detected as a table.
    std::vector<Free> gs;
    put_token(gs, "Term", 72, 700);   put_token(gs, "Meaning", 200, 700);
    put_token(gs, "alpha", 72, 686);  put_token(gs, "first entry in the ordered listing", 200, 686);
    put_token(gs, "of common greek symbols here now", 200, 674);  // wrap: wide, indented
    put_token(gs, "beta", 72, 660);   put_token(gs, "second entry in the ordered set here", 200, 660);
    put_token(gs, "seen widely across the statistics", 200, 648);  // trailing wrap
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("| Term | Meaning |"));
    EXPECT_THAT(md, HasSubstr("| --- | --- |"));
    EXPECT_THAT(md, HasSubstr("| alpha |"));
    EXPECT_THAT(md, Not(HasSubstr("###### alpha")));   // label is a cell, not a heading
    EXPECT_THAT(md, Not(HasSubstr("###### beta")));
    // The wide wrapped value lines stay inside the table, not orphaned as prose.
    EXPECT_THAT(md, HasSubstr("of common greek symbols here now |"));
    EXPECT_THAT(md, HasSubstr("seen widely across the statistics |"));
}

TEST(LayoutTable, WrappedInnerCellMergesInsteadOfEmptyFlankedRow) {
    // A three-column grid whose LAST row has a middle-column value that overflows
    // onto two further single-segment lines indented to that column, with the
    // outer columns (row label, description) empty. Those wraps must be folded
    // back into the middle cell of their row -- never emitted as separate rows of
    // empty flanking columns ("|  | ... |  |"), and never orphaned below the table.
    std::vector<Free> gs;
    put_token(gs, "Tag", 72, 700);  put_token(gs, "Type", 160, 700);  put_token(gs, "Note", 320, 700);
    put_token(gs, "001", 72, 686);  put_token(gs, "number", 160, 686); put_token(gs, "small int here", 320, 686);
    put_token(gs, "110", 72, 672);  put_token(gs, "boolean", 160, 672); put_token(gs, "flag enumeration set", 320, 672);
    put_token(gs, "null, or", 160, 658);   // wrap of the middle cell only
    put_token(gs, "undefined", 160, 644);  // further wrap of the same cell
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("| --- | --- | --- |"));
    EXPECT_THAT(md, HasSubstr("| 110 | boolean null, or undefined | flag enumeration set |"));
    EXPECT_THAT(md, Not(HasSubstr("|  | null, or |  |")));    // no empty-flanked row
    EXPECT_THAT(md, Not(HasSubstr("|  | undefined |  |")));
}

TEST(LayoutTable, KeyValueRowsStayDistinctNotFusedIntoHeader) {
    // A key/value table whose top rows are stacked a little tighter than the body
    // pitch. Each row carries its own label in column 0 and a value in column 1,
    // so it is an independent data row -- the header must be the first row alone,
    // never the tight top rows folded together into one fused header cell.
    std::vector<Free> gs;
    put_token(gs, "Kind", 72, 700);    put_token(gs, "function", 200, 700);
    put_token(gs, "Scope", 72, 690);   put_token(gs, "klass", 200, 690);   // tight gap (10)
    put_token(gs, "Syntax", 72, 674);  put_token(gs, "voidf", 200, 674);   // body pitch (16)
    put_token(gs, "Return", 72, 658);  put_token(gs, "intx", 200, 658);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("| Kind | function |"));
    EXPECT_THAT(md, HasSubstr("| --- | --- |"));
    EXPECT_THAT(md, HasSubstr("| Scope | klass |"));
    EXPECT_THAT(md, Not(HasSubstr("Kind Scope")));       // labels never fused
    EXPECT_THAT(md, Not(HasSubstr("function klass")));   // values never fused
}

TEST(LayoutTable, WrappedRowLabelJoinsCellWhileShortLabelStaysARow) {
    // The row label "Forwarding Header File" is too wide for its column and wraps:
    // its first line runs to the edge of the label column (with its value beside
    // it) and "File" sits alone on the next line. That trailing fragment folds
    // back into the label cell. A short self-contained label with an empty value
    // ("Zzz"), which ends nowhere near the column edge, must stay its own row.
    std::vector<Free> gs;
    put_token(gs, "Field", 72, 700);             put_token(gs, "Value", 210, 700);
    put_token(gs, "Aaa", 72, 684);               put_token(gs, "vone", 210, 684);
    put_token(gs, "Zzz", 72, 668);               // short empty-value label: own row
    put_token(gs, "ForwardingHeader", 72, 652);  put_token(gs, "definesheaderfile", 210, 652);
    put_token(gs, "File", 72, 636);              // wrapped label continuation
    put_token(gs, "Scope", 72, 620);             put_token(gs, "final", 210, 620);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("ForwardingHeader File |"));  // fragment folded into label
    EXPECT_THAT(md, Not(HasSubstr("| File |")));            // never its own row
    EXPECT_THAT(md, HasSubstr("| Zzz |"));                  // distinct label kept separate
    EXPECT_THAT(md, Not(HasSubstr("Aaa Zzz")));
}

TEST(LayoutTable, CenteredValueLabelSandwichBecomesOneRow) {
    // A wrapped identifier label set across two column-0 lines with its value cell
    // vertically centred between them ("ABx" / value / "yCD"). Both fragments and
    // the centred value belong to one row: the leading fragment prepends and the
    // trailing fragment appends onto the same label cell.
    std::vector<Free> gs;
    put_token(gs, "Number", 72, 700);  put_token(gs, "Heading", 200, 700);
    put_token(gs, "X001", 72, 684);    put_token(gs, "firstitem", 200, 684);
    put_token(gs, "X002", 72, 668);    put_token(gs, "seconditem", 200, 668);
    put_token(gs, "ABx", 72, 652);                          // leading label fragment
    put_token(gs, "centeredvaluehere", 200, 638);           // centred value cell
    put_token(gs, "yCD", 72, 624);                          // trailing label fragment
    put_token(gs, "X003", 72, 608);    put_token(gs, "thirditem", 200, 608);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("| ABx yCD | centeredvaluehere |"));
    EXPECT_THAT(md, Not(HasSubstr("|  | centeredvaluehere |")));  // value not on its own row
    EXPECT_THAT(md, Not(HasSubstr("| yCD |")));                   // fragment not its own row
}

TEST(LayoutTable, PunctuationOnlyLineIsNotFoldedIntoLabel) {
    // In a code-listing table an instruction fills its column to the edge, and a
    // bare ellipsis "..." sits on the next line. Geometry alone would read the
    // ellipsis as a wrapped-label continuation, but a punctuation-only line
    // (no letter or digit) is not a label fragment and stays its own row.
    std::vector<Free> gs;
    put_token(gs, "Instr", 72, 700);           put_token(gs, "Comment", 190, 700);
    put_token(gs, "je side_exit_2", 72, 684);  put_token(gs, "// exit", 190, 684);
    put_token(gs, ". . .", 72, 668);           // ellipsis row: must stay separate
    put_token(gs, "ret", 72, 652);             put_token(gs, "// done", 190, 652);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("| . . . |"));
    EXPECT_THAT(md, Not(HasSubstr("side_exit_2 . . .")));
}

TEST(LayoutTable, WrappedIdSandwichWithLabeledCentreFoldsToOneRow) {
    // A wrapped identifier whose middle line ALSO carries a column-0 label (the
    // centre fragment "CD_" set beside the value), bracketed by a leading "[AB_"
    // and a trailing "99]" fragment. All three fragments belong to one row-label
    // cell and rejoin tight on their connectors; the value is that row's cell.
    // (The E.38-style table where the centred value line is itself FULL.)
    std::vector<Free> gs;
    put_token(gs, "Num", 72, 700);   put_token(gs, "Heading", 200, 700);
    put_token(gs, "X001", 72, 684);  put_token(gs, "firstitem", 200, 684);
    put_token(gs, "X002", 72, 668);  put_token(gs, "seconditem", 200, 668);
    put_token(gs, "[AB_", 72, 652);                       // leading fragment
    put_token(gs, "CD_", 72, 636);   put_token(gs, "gammavalue", 200, 636);  // labelled centre
    put_token(gs, "99]", 72, 620);                        // trailing fragment
    put_token(gs, "X003", 72, 604);  put_token(gs, "thirditem", 200, 604);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("| \\[AB\\_CD\\_99] | gammavalue |"));
    EXPECT_THAT(md, Not(HasSubstr("| \\[AB\\_ |")));   // leading fragment not its own row
    EXPECT_THAT(md, Not(HasSubstr("| 99] |")));    // trailing fragment not its own row
}

TEST(LayoutTable, CentredCaptionUnderLabelFragmentLeavesTable) {
    // The last table content is a wrapped-label fragment ("77]" in column 0), and
    // a centred caption sits just below it. The caption continues no inner cell
    // (its neighbour above holds only a bare label), so it must not be pulled into
    // the table nor glued onto the last row's value -- it stays outside as text.
    std::vector<Free> gs;
    put_token(gs, "Num", 72, 700);   put_token(gs, "Heading", 200, 700);
    put_token(gs, "X010", 72, 684);  put_token(gs, "alpha", 200, 684);
    put_token(gs, "X011", 72, 668);  put_token(gs, "beta", 200, 668);
    put_token(gs, "[GH_", 72, 652);                       // leading fragment
    put_token(gs, "IJ_", 72, 636);   put_token(gs, "widevalue", 200, 636);   // labelled centre
    put_token(gs, "77]", 72, 620);                        // trailing fragment
    put_token(gs, "Table 9: a centred caption line", 210, 600);  // caption, indented/centred
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("| \\[GH\\_IJ\\_77] | widevalue |"));
    EXPECT_THAT(md, Not(HasSubstr("centred caption line |")));      // not glued into a cell
    EXPECT_THAT(md, Not(HasSubstr("| widevalue Table 9")));         // not appended to value
}

TEST(LayoutTable, FlowingWrappedValueStaysOneCellNotInterleaved) {
    // A three-column key/value table (its inner boundary fixed by the wide-gutter
    // "Param"/"Ret" rows). The "Syntax" value is one expression that flows past
    // that boundary and wraps to a second line; folding column-for-column would
    // interleave the words. Confirmed as a spanning value by its wrapped
    // continuation, it stays a single reading-order cell.
    std::vector<Free> gs;
    put_token(gs, "Kind", 72, 700);   put_token(gs, "function", 150, 700);
    put_token(gs, "Param", 72, 684);  put_token(gs, "arg", 150, 684);
    put_token(gs, "the input value", 270, 684);   // wide gutter -> real col2
    put_token(gs, "Ret", 72, 668);    put_token(gs, "rtype", 150, 668);
    put_token(gs, "the result value", 270, 668);   // wide gutter -> real col2
    // Syntax value: a long run that crosses the col1|col2 boundary with only
    // word-sized gaps, wrapping to a continuation that also crosses it.
    put_token(gs, "Syntax", 72, 652);
    put_token(gs, "aType Foo (VeryLongParamName", 150, 652);   // spans past the gutter
    put_token(gs, "shortArg) noexcept;", 150, 636);            // continuation, also spans
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("aType Foo (VeryLongParamName shortArg) noexcept;"));
    EXPECT_THAT(md, Not(HasSubstr("VeryLongParamName shortArg) aType")));  // no interleave
}

TEST(LayoutTable, CentredBoldCaptionBelowGridLeavesTable) {
    // A class-style key/value table whose last content is a wrapped value cell
    // (indented to the inner column, flush-left there), with a BOLD title centred
    // on the page drawn just below the grid ("Table 7: WidgetKind"). The title is
    // a caption outside the box, not a continuation of the value cell: it must
    // terminate the table and stay outside it, never be swallowed into a cell.
    // (make_free_page pages are 612 wide, so the page centre is x = 306.)
    std::vector<Free> gs;
    put_token(gs, "Class", 72, 700);   put_token(gs, "Widget", 150, 700);
    put_token(gs, "Note", 72, 684);    put_token(gs, "this note runs wide across the value column to the margin", 150, 684);
    put_token(gs, "Attr", 72, 668);    put_token(gs, "a described value spanning the column out to the right margin", 150, 668);
    put_token(gs, "the continuation of that value flowing on to the right page margin", 150, 652);  // wrapped inner cell
    // Bold caption, single token centred on x = 306 (x0 = 258, right edge = 354).
    put_token(gs, "Table7WidgetKind", 258, 632, 10, /*bold=*/true);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("| Class | Widget |"));
    EXPECT_THAT(md, Not(HasSubstr("Table7WidgetKind |")));   // caption not glued into a cell
    EXPECT_THAT(md, Not(HasSubstr("margin Table7WidgetKind")));  // not appended to the value
}

TEST(LayoutTable, PlaceholderDashCellCollapsesInsteadOfRunningOn) {
    // A row of empty grid cells the producer fills with a lone dash each. When
    // several fall into one detected column (a row that spans fewer columns than
    // the grid drew) their dashes must not concatenate into "- - - -"; the cell
    // keeps a single placeholder dash.
    const char32_t kEn = 0x2013;
    std::vector<Free> gs;
    put_token(gs, "Key", 72, 700);   put_token(gs, "Val", 200, 700);
    put_token(gs, "aaa", 72, 684);   put_token(gs, "xyz", 200, 684);
    put_token(gs, "bbb", 72, 668);
    for (double x : {200.0, 250.0, 300.0, 350.0})  // four placeholder dashes in one column
        gs.push_back({kEn, x, 668});
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("| aaa | xyz |"));
    EXPECT_THAT(md, HasSubstr("| bbb |"));
    EXPECT_THAT(md, Not(HasSubstr("– – –")));  // no run of concatenated dashes
}

TEST(LayoutTable, NestedRicherGridSplitsIntoItsOwnTable) {
    // A class-style box: a 2-column key/value head (Class/Note/Base) drawn above
    // an interior bold header row (Attribute/Type/Mult/Kind/Note) that re-labels a
    // nested 5-column grid inside the same frame. The interior bold header must
    // split the emission into two GFM tables -- the nested grid keeping its five
    // real columns -- rather than collapsing every nested column into one cell and
    // surfacing the bold header as a data row.
    std::vector<Free> gs;
    auto row5 = [&](const char* a, const char* t, const char* m, const char* k, const char* n,
                    double y, bool bold) {
        put_token(gs, a, 72, y, 10, bold);
        put_token(gs, t, 210, y, 10, bold);
        put_token(gs, m, 300, y, 10, bold);
        put_token(gs, k, 350, y, 10, bold);
        put_token(gs, n, 410, y, 10, bold);
    };
    put_token(gs, "Class", 72, 700, 10, /*bold=*/true);  put_token(gs, "Widget", 210, 700);
    put_token(gs, "Note", 72, 684, 10, /*bold=*/true);    put_token(gs, "somenote", 210, 684);
    put_token(gs, "Base", 72, 668, 10, /*bold=*/true);    put_token(gs, "somebase", 210, 668);
    row5("Attribute", "Type", "Mult", "Kind", "Note", 652, /*bold=*/true);  // interior header
    row5("alpha", "IntType", "one", "attr", "firstnote", 636, false);
    row5("beta", "StrType", "two", "ref", "secondnote", 620, false);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("| Class | Widget |"));                        // 2-col head
    EXPECT_THAT(md, HasSubstr("| Attribute | Type | Mult | Kind | Note |"));  // nested header
    EXPECT_THAT(md, HasSubstr("| alpha | IntType | one | attr | firstnote |"));
    EXPECT_THAT(md, Not(HasSubstr("Type Mult")));   // columns never crushed together
    EXPECT_THAT(md, Not(HasSubstr("***Type")));     // header emphasis stripped, not a data row
}

TEST(LayoutTable, WrappedCamelLabelFoldsWhenColumnFilled) {
    // A two-column table whose row label is a camelCase identifier too long for its
    // column: it fills the column and breaks, the continuation ("Continued") set
    // uppercase on the next line. Because the label above filled its column, the
    // fragment folds back into it tight (no space). A short label ("shortlit") that
    // ends well clear of the column edge stays its own distinct row.
    std::vector<Free> gs;
    put_token(gs, "Literal", 72, 700, 10, /*bold=*/true);  put_token(gs, "Description", 280, 700, 10, true);
    put_token(gs, "shortlit", 72, 684);                    put_token(gs, "aaatext", 280, 684);
    put_token(gs, "veryLongLiteralName", 72, 668);         put_token(gs, "bbbhere", 280, 668);
    put_token(gs, "Continued", 72, 652);                   put_token(gs, "cccmore", 280, 652);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("veryLongLiteralNameContinued"));  // fragment rejoined tight
    EXPECT_THAT(md, Not(HasSubstr("| Continued |")));            // never its own row
    EXPECT_THAT(md, HasSubstr("| shortlit |"));                  // short label stays distinct
}

TEST(LayoutTable, BareCamelLabelContinuationFoldsInWideGrid) {
    // A five-column grid where a short camelCase label ("cd") wraps: its
    // continuation ("Xy") sits alone with the typed columns (Type/Mult/Kind) empty
    // and only the trailing note carried on. Even though the short label never
    // filled its column, an uppercase continuation that adds no typed cell is a
    // wrapped label, not a new attribute, so it folds up.
    std::vector<Free> gs;
    auto row5 = [&](const char* a, const char* t, const char* m, const char* k, const char* n,
                    double y, bool bold) {
        put_token(gs, a, 72, y, 10, bold);
        if (*t) put_token(gs, t, 200, y, 10, bold);
        if (*m) put_token(gs, m, 290, y, 10, bold);
        if (*k) put_token(gs, k, 340, y, 10, bold);
        if (*n) put_token(gs, n, 400, y, 10, bold);
    };
    row5("Attribute", "Type", "Mult", "Kind", "Note", 700, /*bold=*/true);
    row5("aa", "TypeA", "one", "attr", "noteAAA", 684, false);
    row5("cd", "TypeB", "two", "ref", "noteStarts", 668, false);
    row5("Xy", "", "", "", "morenote", 652, false);  // bare continuation: typed cells empty
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("| cdXy |"));            // continuation folded into the label
    EXPECT_THAT(md, Not(HasSubstr("| Xy |")));         // never its own row
    EXPECT_THAT(md, HasSubstr("| aa |"));              // an ordinary row stays distinct
}

TEST(LayoutTable, IndependentFullRowsNotFoldedByCaseCoincidence) {
    // Two independent, fully-populated rows of a wide grid whose labels merely meet
    // on a lowercase->uppercase boundary ("...Attention" then "Recurrent"), each
    // carrying its own complete typed data. The label above fills its column (so the
    // camelCase wrap heuristic would otherwise fire), yet both rows supply data in
    // the SAME middle columns -- a collision that marks them as distinct rows, not
    // one wrapped label. They must stay two separate rows, never concatenated.
    std::vector<Free> gs;
    auto row4 = [&](const char* a, const char* b, const char* c, const char* d,
                    double y, bool bold) {
        put_token(gs, a, 72, y, 10, bold);
        put_token(gs, b, 200, y, 10, bold);
        put_token(gs, c, 300, y, 10, bold);
        put_token(gs, d, 400, y, 10, bold);
    };
    row4("Layer", "Complexity", "Sequential", "PathLen", 700, /*bold=*/true);
    row4("SelfAttention", "aaa1", "bbb1", "ccc1", 684, false);
    row4("Recurrent", "aaa2", "bbb2", "ccc2", 668, false);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("| SelfAttention |"));            // stays its own row
    EXPECT_THAT(md, HasSubstr("| Recurrent |"));                // stays its own row
    EXPECT_THAT(md, Not(HasSubstr("SelfAttentionRecurrent")));  // never merged
    EXPECT_THAT(md, Not(HasSubstr("aaa1 aaa2")));               // typed cells never crushed
}

TEST(LayoutTable, LockstepLabelAndTypeWrapFoldsIntoOneRow) {
    // One attribute row of a wide grid whose label AND type value are both too long
    // for their columns and break at the SAME line: label "authenticationEnabled"
    // wraps as "authentication"/"Enabled" and type "DiagnosticAuthRoleProxy" wraps
    // as "DiagnosticAuthRole"/"Proxy" -- two cells of ONE logical row wrapping in
    // lockstep. The Type-column collision must not be read as an independent row: the
    // fragment fills only that one typed column, its cell is a camelCase continuation
    // of a column-filling value above (a wrap signal), and Mult/Kind stay empty. The
    // pair folds into a single row; it must not split into two broken rows.
    std::vector<Free> gs;
    auto row5 = [&](const char* a, const char* t, const char* m, const char* k, const char* n,
                    double y, bool bold) {
        put_token(gs, a, 72, y, 10, bold);
        if (*t) put_token(gs, t, 200, y, 10, bold);
        if (*m) put_token(gs, m, 360, y, 10, bold);
        if (*k) put_token(gs, k, 410, y, 10, bold);
        if (*n) put_token(gs, n, 470, y, 10, bold);
    };
    row5("Attribute", "Type", "Mult", "Kind", "Note", 700, /*bold=*/true);
    row5("shortAttr", "ShortType", "1", "attr", "plainnote", 684, false);  // an ordinary row
    row5("authentication", "DiagnosticAuthRole", "0..1", "aggr", "somenote", 668, false);
    row5("Enabled", "Proxy", "", "", "morenote", 652, false);  // lockstep wrap fragment
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("authenticationEnabled"));         // label folded whole
    EXPECT_THAT(md, HasSubstr("DiagnosticAuthRoleProxy"));       // type folded whole
    EXPECT_THAT(md, Not(HasSubstr("| Enabled | Proxy |")));      // never a broken second row
    EXPECT_THAT(md, HasSubstr("| shortAttr |"));                 // ordinary row stays distinct
}

TEST(LayoutTable, LabellessInnerWrapFoldsUpInNestedGrid) {
    // In a split nested grid, a wrapped inner cell can land on a line with no row
    // label yet still carry two segments (a type fragment plus continued note).
    // Such a label-less line is a continuation of the row above and folds up,
    // never leaking as a phantom row of empty flanking columns.
    std::vector<Free> gs;
    put_token(gs, "Class", 72, 700, 10, /*bold=*/true);  put_token(gs, "Widget", 200, 700);
    put_token(gs, "Note", 72, 684, 10, /*bold=*/true);   put_token(gs, "somenote", 200, 684);
    put_token(gs, "Attribute", 72, 668, 10, true);  put_token(gs, "Type", 200, 668, 10, true);
    put_token(gs, "Mult", 290, 668, 10, true);      put_token(gs, "Kind", 340, 668, 10, true);
    put_token(gs, "Note", 400, 668, 10, true);
    put_token(gs, "attrA", 72, 652);   put_token(gs, "LongType", 200, 652);
    put_token(gs, "one", 290, 652);    put_token(gs, "ref", 340, 652);  put_token(gs, "notetext", 400, 652);
    put_token(gs, "More", 200, 636);   put_token(gs, "andnote", 400, 636);  // label-less continuation
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, Not(HasSubstr("|  | More")));   // no phantom empty-flanked row
    EXPECT_THAT(md, HasSubstr("LongType More"));    // type fragment folded into the cell above
    EXPECT_THAT(md, HasSubstr("notetext andnote")); // note continuation retained in the row
}

// A page-number that varies per page but sits in the same bottom band, alongside
// a fixed running title, on ONE footer line split into two column segments. The
// digit-normalized per-segment signature recurs on every page, so the whole
// footer line is removed before it can reach the body -- even though the running
// page number differs each time.
std::string convert_pages(const std::vector<std::vector<Free>>& page_glyphs) {
    pdf2md::DocMeta meta;
    std::vector<pdf2md::PageData> pages;
    for (const auto& gs : page_glyphs) {
        pages.push_back(make_free_page(gs));
        pages.back().index = static_cast<int>(pages.size()) - 1;
    }
    pdf2md::Document doc = pdf2md::analyze_layout(meta, pages, {});
    return pdf2md::write_markdown(doc, {});
}

TEST(LayoutChrome, RunningFooterWithVaryingPageNumberStripped) {
    std::vector<std::vector<Free>> pages;
    for (int p = 1; p <= 6; ++p) {
        std::vector<Free> gs;
        put_token(gs, "Body content unique to page number " + std::to_string(p) + " here", 72, 400);
        // Footer: "<p> of 6" (left segment) and a fixed running title (right
        // segment) separated by a column-sized gap, both in the bottom band.
        put_token(gs, std::to_string(p) + " of 6", 72, 60);
        put_token(gs, "Reference 5 SpecTitle", 300, 60);
        pages.push_back(std::move(gs));
    }
    std::string md = convert_pages(pages);
    EXPECT_THAT(md, Not(HasSubstr("SpecTitle")));  // running footer removed
    EXPECT_THAT(md, Not(HasSubstr("of 6")));       // page-number half removed too
    EXPECT_THAT(md, HasSubstr("Body content unique to page number 3"));
}

TEST(LayoutChrome, FooterNotFusedWithTrailingBodyLine) {
    // The footer sits just below the last body line, close enough that a naive
    // block merge would glue the running title onto the body text. Stripping the
    // footer at the line level keeps the (per-page unique) body line intact and
    // free of any footer text.
    std::vector<std::vector<Free>> pages;
    for (int p = 1; p <= 6; ++p) {
        std::vector<Free> gs;
        put_token(gs, "Sentence body for page " + std::to_string(p) + " ends here", 72, 108);
        put_token(gs, std::to_string(p) + " of 6 Document Ref 42 SpecTitle", 72, 74);
        pages.push_back(std::move(gs));
    }
    std::string md = convert_pages(pages);
    EXPECT_THAT(md, Not(HasSubstr("SpecTitle")));
    EXPECT_THAT(md, Not(HasSubstr("Document Ref")));
    EXPECT_THAT(md, HasSubstr("Sentence body for page 4 ends here"));
}

TEST(LayoutChrome, UniqueBottomBandLineNotStripped) {
    // A one-off line in the bottom band that does NOT recur across pages must
    // survive: only cross-page repetition qualifies as chrome.
    std::vector<std::vector<Free>> pages;
    for (int p = 1; p <= 6; ++p) {
        std::vector<Free> gs;
        put_token(gs, "Main paragraph on page " + std::to_string(p), 72, 400);
        put_token(gs, std::to_string(p) + " of 6 SpecTitle", 300, 60);  // recurring footer
        if (p == 2) put_token(gs, "one off footnote about widgets", 72, 60);  // unique
        pages.push_back(std::move(gs));
    }
    std::string md = convert_pages(pages);
    EXPECT_THAT(md, Not(HasSubstr("SpecTitle")));                 // footer gone
    EXPECT_THAT(md, HasSubstr("one off footnote about widgets"));  // unique line kept
}

// ---- WS5: heading / blockquote classification ---------------------------

// A left-aligned body paragraph so body_size resolves to 10 and the body-column
// edges are well defined for the callout-geometry tests.
void put_body(std::vector<Free>& gs, int lines, double top) {
    for (int line = 0; line < lines; ++line) {
        double y = top - line * 12.0;
        double x = 72;
        for (int w = 0; w < 9; ++w) {
            put_token(gs, "bodyword", x, y, 10);
            x += 56;
        }
    }
}

TEST(LayoutHeadings, RepeatedMetadataFieldStaysParagraph) {
    // "Label: value" metadata fields (bold, body-sized) that recur across the
    // document are form fields, not section titles: never promoted to a heading.
    std::vector<Free> gs;
    put_body(gs, 10, 740);
    put_token(gs, "Status: DRAFT", 72, 600, 10, /*bold=*/true);
    put_token(gs, "Status: DRAFT", 72, 560, 10, true);
    put_token(gs, "Status: DRAFT", 72, 520, 10, true);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("Status: DRAFT"));
    EXPECT_THAT(md, Not(HasSubstr("# Status:")));  // no heading level of any depth
}

TEST(LayoutHeadings, FigureCaptionNotPromoted) {
    // A heading-sized caption line ("Figure N ...") follows the caption
    // convention and is vetoed from heading promotion.
    std::vector<Free> gs;
    put_body(gs, 10, 740);
    put_token(gs, "Figure", 72, 600, 14);
    put_token(gs, "3", 140, 600, 14);
    put_token(gs, "System", 170, 600, 14);
    put_token(gs, "Overview", 260, 600, 14);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("Figure 3"));
    EXPECT_THAT(md, Not(HasSubstr("# Figure 3")));
}

TEST(LayoutHeadings, BracketedReferenceEntryNotPromoted) {
    // A bracketed citation label leads a reference entry, not a section title.
    std::vector<Free> gs;
    put_body(gs, 10, 740);
    put_token(gs, "[12]", 72, 600, 10, /*bold=*/true);
    put_token(gs, "ISO Reference Title Nine", 110, 600, 10, true);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("ISO Reference Title Nine"));
    EXPECT_THAT(md, Not(HasSubstr("# \\[12]")));  // never a heading
}

TEST(LayoutHeadings, RepeatedSiblingLabelNotPromotedUniqueTitleIs) {
    // A short label that recurs (a table column header) is a structural sibling,
    // not a title; a unique bold line still promotes to a heading.
    std::vector<Free> gs;
    put_body(gs, 10, 740);
    put_token(gs, "Unique Section Title", 72, 600, 10, /*bold=*/true);
    put_token(gs, "Data Field", 72, 560, 10, true);
    put_token(gs, "Data Field", 72, 520, 10, true);
    put_token(gs, "Data Field", 72, 480, 10, true);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("# Unique Section Title"));  // unique -> heading
    EXPECT_THAT(md, HasSubstr("Data Field"));              // still present as text
    EXPECT_THAT(md, Not(HasSubstr("# Data Field")));       // repeated -> not a heading
}

TEST(LayoutHeadings, InsetSentenceCalloutBecomesBlockquote) {
    // A heading-sized, sentence-terminated block inset from the body column on
    // both sides is a genuine display callout: it becomes a blockquote.
    std::vector<Free> gs;
    put_body(gs, 12, 740);
    put_token(gs, "A centred permission notice reproduced here", 200, 560, 13);
    put_token(gs, "for scholarly and journalistic use only.", 200, 544, 13);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("> A centred permission notice"));
}

TEST(LayoutHeadings, FullWidthHeadingSizeSentenceStaysParagraph) {
    // A heading-sized, sentence-terminated block spanning the full body width is
    // ordinary prose that merely measured large: it stays a plain paragraph,
    // neither a heading nor a blockquote.
    std::vector<Free> gs;
    put_body(gs, 12, 740);
    put_token(gs, "This introductory sentence spans the whole body column", 72, 560, 13);
    put_token(gs, "and continues onto a second line before it ends here.", 72, 544, 13);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("This introductory sentence spans"));
    EXPECT_THAT(md, Not(HasSubstr(">")));
    EXPECT_THAT(md, Not(HasSubstr("# This introductory")));
}

// ---------------------------------------------------------------- ruled grids

// Adds a horizontal border rule at page-space y spanning [x0,x1].
void hrule(pdf2md::PageData& p, double y, double x0, double x1) {
    p.rules.push_back({x0, y, x1, y, /*horizontal=*/true});
}
// Adds a vertical border rule at page-space x spanning [y0,y1].
void vrule(pdf2md::PageData& p, double x, double y0, double y1) {
    p.rules.push_back({x, y0, x, y1, /*horizontal=*/false});
}
// Draws the outer box and interior separators for a grid with the given column
// x-boundaries and row y-boundaries (row boundaries top -> bottom, y descending).
void draw_grid(pdf2md::PageData& p, const std::vector<double>& cols,
               const std::vector<double>& rows) {
    for (double y : rows) hrule(p, y, cols.front(), cols.back());
    for (double x : cols) vrule(p, x, rows.back(), rows.front());
}

TEST(LayoutRuledGrid, ThreeColumnBorderedTableWithSingleBodyRow) {
    // A bordered 3-column grid with a header row and one short body row -- the
    // text-geometry detector needs two multi-column rows, so the ruling lattice
    // is what recovers this table (the shape of a spec's short interface tables).
    pdf2md::PageData p;
    p.width = 612;
    p.height = 792;
    std::vector<Free> gs;
    put_token(gs, "Interface", 76, 690);
    put_token(gs, "Cluster", 236, 690);
    put_token(gs, "Purpose", 386, 690);
    put_token(gs, "No provided interfaces", 76, 668);  // fits within the wide first column
    p.chars = make_free_page(gs).chars;
    draw_grid(p, {71, 220, 370, 524}, {700, 680, 660});  // 3 cols, 2 rows
    std::string md = convert(p);
    EXPECT_THAT(md, HasSubstr("| Interface | Cluster | Purpose |"));
    EXPECT_THAT(md, HasSubstr("| --- | --- | --- |"));
    EXPECT_THAT(md, HasSubstr("| No provided interfaces |"));
}

TEST(LayoutRuledGrid, TitleSpanningRowBecomesCaptionAboveTable) {
    // The top band spans the full width (no interior verticals reach it): it is a
    // title and must surface as a caption line above the grid, not as a data row
    // whose text lands in one arbitrary column (a spanning change-history title).
    pdf2md::PageData p;
    p.width = 612;
    p.height = 792;
    std::vector<Free> gs;
    put_token(gs, "Change History", 250, 728, 10, /*bold=*/true);  // spanning title
    put_token(gs, "Date", 76, 706);
    put_token(gs, "Who", 236, 706);
    put_token(gs, "2025-11-27", 76, 684);
    put_token(gs, "Company", 236, 684);
    p.chars = make_free_page(gs).chars;
    // Outer box + a header/body split; the interior vertical stops below the title.
    std::vector<double> cols = {71, 220, 524};
    std::vector<double> rows = {740, 720, 698, 676};  // title / header / body
    hrule(p, 740, 71, 524);
    hrule(p, 720, 71, 524);
    hrule(p, 698, 71, 524);
    hrule(p, 676, 71, 524);
    vrule(p, 71, 676, 740);   // outer left spans everything
    vrule(p, 524, 676, 740);  // outer right spans everything
    vrule(p, 220, 676, 720);  // interior separator only below the title band
    std::string md = convert(p);
    EXPECT_THAT(md, HasSubstr("Change History"));
    EXPECT_THAT(md, HasSubstr("| Date | Who |"));
    EXPECT_THAT(md, HasSubstr("| 2025-11-27 | Company |"));
    // The title is a caption line, never a table row cell.
    EXPECT_THAT(md, Not(HasSubstr("| Change History |")));
    // The caption precedes the table.
    EXPECT_LT(md.find("Change History"), md.find("| Date | Who |"));
}

TEST(LayoutRuledGrid, TwoColumnMultiLineCellsJoinAndInteriorRuleBreaks) {
    // A 2-column grid whose right cell stacks two entries divided by a short rule
    // that crosses only that column: the row stays one row, the left identifier
    // (wrapped on a hyphen) rejoins tight, and the two right entries are kept apart
    // by a <br> (mirrors the PortInterface -> API-class binding table).
    pdf2md::PageData p;
    p.width = 612;
    p.height = 792;
    std::vector<Free> gs;
    put_token(gs, "Port Interface", 76, 706);
    put_token(gs, "API Class", 266, 706);
    // Left cell: an identifier wrapped over two lines on a hyphen.
    put_token(gs, "DiagnosticGenericIn-", 76, 684);
    put_token(gs, "terface", 76, 672);
    // Right cell: two stacked entries separated by the interior rule at y=678.
    put_token(gs, "entry one alpha", 266, 684);
    put_token(gs, "entry two beta", 266, 666);
    p.chars = make_free_page(gs).chars;
    std::vector<double> cols = {71, 255, 524};
    hrule(p, 720, 71, 524);  // top
    hrule(p, 696, 71, 524);  // header/body divider (full width)
    hrule(p, 678, 255, 524);  // interior divider: right column only
    hrule(p, 656, 71, 524);  // bottom (full width)
    vrule(p, 71, 656, 720);
    vrule(p, 255, 656, 720);
    vrule(p, 524, 656, 720);
    std::string md = convert(p);
    EXPECT_THAT(md, HasSubstr("| Port Interface | API Class |"));
    EXPECT_THAT(md, HasSubstr("DiagnosticGenericInterface"));   // wrap rejoined tight
    EXPECT_THAT(md, Not(HasSubstr("In- terface")));
    EXPECT_THAT(md, HasSubstr("entry one alpha<br>entry two beta"));  // rule split
}

TEST(LayoutRuledGrid, MultiLineCellPhraseJoinsWithSpaceNotBreak) {
    // Within one cell a phrase wrapped across lines (no bullet, no dividing rule)
    // reads as one phrase joined by a space, so a search for the phrase succeeds.
    pdf2md::PageData p;
    p.width = 612;
    p.height = 792;
    std::vector<Free> gs;
    put_token(gs, "Date", 76, 706);
    put_token(gs, "Changed", 236, 706);
    put_token(gs, "2025", 76, 680);
    put_token(gs, "Company", 236, 690);
    put_token(gs, "Release", 236, 678);
    put_token(gs, "Management", 236, 666);
    p.chars = make_free_page(gs).chars;
    draw_grid(p, {71, 220, 524}, {720, 698, 654});
    std::string md = convert(p);
    EXPECT_THAT(md, HasSubstr("Company Release Management"));
    EXPECT_THAT(md, Not(HasSubstr("Company<br>Release")));
}

TEST(LayoutRuledGrid, LockstepCamelCaseIdentifierWrapJoinsTight) {
    // In a bordered class table a long camelCase name too wide for its narrow column
    // breaks at the lowercase->uppercase hump: label "authenticationEnabled" as
    // "authentication"/"Enabled" and type "DiagnosticAuthRoleProxy" as
    // "DiagnosticAuthRole"/"Proxy", the two cells wrapping in lockstep within one row
    // band. Each single-token fragment fills its column and rejoins tight (no space).
    // A neighbouring multi-word note cell keeps its space -- its wrapped line is not a
    // lone identifier fragment (the shape of a spec's attribute grids).
    pdf2md::PageData p;
    p.width = 612;
    p.height = 792;
    std::vector<Free> gs;
    put_token(gs, "Attribute", 76, 706, 10, /*bold=*/true);
    put_token(gs, "Type", 170, 706, 10, /*bold=*/true);
    put_token(gs, "Note", 366, 706, 10, /*bold=*/true);
    put_token(gs, "authentication", 74, 684);  put_token(gs, "DiagnosticAuthRole", 170, 684);
    put_token(gs, "This value", 366, 684);
    put_token(gs, "Enabled", 74, 668);         put_token(gs, "Proxy", 170, 668);
    put_token(gs, "Wraps here", 366, 668);
    p.chars = make_free_page(gs).chars;
    draw_grid(p, {71, 160, 360, 524}, {720, 698, 654});
    std::string md = convert(p);
    EXPECT_THAT(md, HasSubstr("authenticationEnabled"));    // label rejoined tight
    EXPECT_THAT(md, HasSubstr("DiagnosticAuthRoleProxy"));  // type rejoined tight in lockstep
    EXPECT_THAT(md, Not(HasSubstr("authentication Enabled")));
    EXPECT_THAT(md, Not(HasSubstr("DiagnosticAuthRole Proxy")));
    EXPECT_THAT(md, HasSubstr("This value Wraps here"));    // multi-word note keeps its spaces
    EXPECT_THAT(md, Not(HasSubstr("valueWraps")));          // never glued at a phrase break
}

TEST(LayoutRuledGrid, WordStraddlingInteriorRuleKeepsCellWhole) {
    // A bordered grid with three columns whose interior post is drawn full height,
    // but on one row a value sentence flows continuously across that post (a word
    // straddles the boundary with no column gap at the rule -- the shape of a
    // spec table whose value sentence runs across an interior post). Assigning glyphs by centre would chop
    // the word at the post ("...that" | "may..."); the straddled post is treated
    // as absent for that row so the sentence stays whole in one spanning cell. A
    // neighbouring row whose two value columns are set apart by a real gap keeps
    // both posts and renders as three distinct cells.
    pdf2md::PageData p;
    p.width = 612;
    p.height = 792;
    std::vector<Free> gs;
    put_token(gs, "Exceptions", 75, 700);
    put_token(gs, "List of Cpp Exceptions that may be thrown", 205, 700);  // spans post 360
    put_token(gs, "Param", 75, 670);
    put_token(gs, "typeVal", 205, 670);   // sits left of post 360, real gap follows
    put_token(gs, "descVal", 400, 670);   // sits right of post 360
    p.chars = make_free_page(gs).chars;
    draw_grid(p, {71, 200, 360, 524}, {715, 685, 655});
    std::string md = convert(p);
    EXPECT_THAT(md, HasSubstr("that may be thrown"));      // sentence not chopped at the post
    EXPECT_THAT(md, Not(HasSubstr("that | ")));            // no mid-word column split
    EXPECT_THAT(md, HasSubstr("| typeVal | descVal |"));   // genuine 3-col row kept apart
}

TEST(LayoutTextTable, OversizedHeadingAboveGridStaysOutside) {
    // A requirement heading set markedly larger than the grid body aligns into two
    // segments (an id column and a title column) and so seeds a key/value table,
    // yet it is not a table row (a requirement heading "[REQ_ID_...] Definition
    // of API function ..." sitting above its spec table). The oversized leading line is
    // dropped from the table and stays in the normal flow as one contiguous line,
    // rather than being swallowed as a scrambled header row.
    pdf2md::PageData p;
    p.width = 612;
    p.height = 792;
    std::vector<Free> gs;
    put_token(gs, "[REQ_ID_09999]", 72, 700, 12, /*bold=*/true);
    put_token(gs, "Definition of API function", 205, 700, 12, /*bold=*/true);
    put_token(gs, "Kind:", 72, 680, 8);      put_token(gs, "function", 205, 680, 8);
    put_token(gs, "Scope:", 72, 668, 8);     put_token(gs, "classFoo", 205, 668, 8);
    put_token(gs, "Syntax:", 72, 656, 8);    put_token(gs, "voidBar", 205, 656, 8);
    p.chars = make_free_page(gs).chars;
    std::string md = convert(p);
    EXPECT_THAT(md, HasSubstr("| --- |"));                       // the spec table survives
    EXPECT_THAT(md, HasSubstr("function"));
    // The heading reads as one contiguous line, not split into table cells.
    EXPECT_THAT(md, HasSubstr("\\[REQ\\_ID\\_09999] Definition of API function"));
    EXPECT_THAT(md, Not(HasSubstr("Definition of API function |")));
}

TEST(LayoutRuledGrid, SingleUnderlineDoesNotFabricateTable) {
    // An isolated horizontal rule under a heading is not a grid: no table appears.
    pdf2md::PageData p;
    p.width = 612;
    p.height = 792;
    std::vector<Free> gs;
    put_body(gs, 6, 740);
    put_token(gs, "A Section Title", 72, 700, 13, /*bold=*/true);
    put_body(gs, 6, 680);
    p.chars = make_free_page(gs).chars;
    hrule(p, 694, 72, 520);  // a single decorative underline
    std::string md = convert(p);
    EXPECT_THAT(md, Not(HasSubstr("| --- |")));
}

TEST(LayoutRuledGrid, EmptyBorderedBoxIsNotEmittedAsTable) {
    // A bordered rectangle with a full internal grid but no text (a diagram frame
    // or an empty form box) must not become a table: the fill-ratio gate rejects
    // a lattice whose cells hold no characters.
    pdf2md::PageData p;
    p.width = 612;
    p.height = 792;
    std::vector<Free> gs;
    put_body(gs, 8, 760);  // prose well above the box
    p.chars = make_free_page(gs).chars;
    draw_grid(p, {71, 200, 330, 524}, {560, 520, 480, 440});  // 3x3 empty grid
    std::string md = convert(p);
    EXPECT_THAT(md, Not(HasSubstr("| --- |")));
}

// Counts non-overlapping occurrences of `needle` in `hay`.
int count_occurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// Builds one page carrying a single bordered 3-column table with the given rows
// (each row: three cell strings; an empty string leaves that cell blank). `bold`
// sets every cell of the first row bold (a header). Column x-boundaries are
// fixed, so two such pages share a column signature.
pdf2md::PageData ruled_table_page(const std::vector<std::array<std::string, 3>>& rows,
                                  bool first_row_bold) {
    pdf2md::PageData p;
    p.width = 612;
    p.height = 792;
    const std::vector<double> cols = {71, 210, 350, 524};
    std::vector<double> ys;               // row baselines, top -> bottom
    std::vector<double> rails = {720};     // grid rails, y descending
    std::vector<Free> gs;
    double y = 706;
    for (size_t r = 0; r < rows.size(); ++r) {
        for (int c = 0; c < 3; ++c)
            if (!rows[r][c].empty())
                put_token(gs, rows[r][c], cols[c] + 5, y, 10, first_row_bold && r == 0);
        rails.push_back(y + 6 - 20);       // bottom rail of this row band
        y -= 20;
    }
    p.chars = make_free_page(gs).chars;
    draw_grid(p, cols, rails);
    return p;
}

TEST(LayoutRuledGrid, PageSplitTableStitchesIntoOneTable) {
    // A ruled table split by a page break: page 1 carries the header and one data
    // row, page 2 a continuation-marker triangle and two more data rows with no
    // repeated header. The same column signature plus the marker (and the missing
    // header) identify the continuation, so it merges into one table -- one header,
    // one delimiter row, and no marker glyph in the output (the shape of a
    // change-history table continued across a page break).
    pdf2md::PageData p0 = ruled_table_page(
        {{"Date", "Release", "Who"}, {"2025-11-27", "R25", "Company"}}, /*bold=*/true);
    pdf2md::PageData p1 = ruled_table_page(
        {{"2024-11-27", "R24", "Company"}, {"2023-11-23", "R23", "Company"}}, /*bold=*/false);
    // A standalone continuation-marker glyph at the top of page 2, above the table.
    p1.chars.push_back([] {
        pdf2md::CharInfo c;
        c.code = 0x25B3;  // white up-pointing triangle
        c.x = c.left = 280;
        c.right = 288;
        c.y = c.bottom = 760;
        c.top = 770;
        c.size = 10;
        return c;
    }());
    std::string md = convert_pages({p0, p1});
    EXPECT_THAT(md, HasSubstr("| Date | Release | Who |"));
    // All three data rows sit in one table: exactly one delimiter row, and no
    // second header re-opening a table.
    EXPECT_EQ(count_occurrences(md, "| --- | --- | --- |"), 1);
    EXPECT_LT(md.find("2025-11-27"), md.find("2024-11-27"));
    EXPECT_LT(md.find("2024-11-27"), md.find("2023-11-23"));
    EXPECT_THAT(md, Not(HasSubstr("△")));  // the marker glyph is dropped
    EXPECT_THAT(md, Not(HasSubstr("▽")));
}

TEST(LayoutRuledGrid, RepeatedHeaderFragmentsAreNotMerged) {
    // Two fragments with the same column signature that each repeat their header
    // (and carry no continuation marker between them) are independent tables, not a
    // split one: they must stay separate so each keeps a valid GFM header.
    pdf2md::PageData p0 = ruled_table_page(
        {{"Term", "Kind", "Note"}, {"alpha", "one", "first"}}, /*bold=*/true);
    pdf2md::PageData p1 = ruled_table_page(
        {{"Term", "Kind", "Note"}, {"beta", "two", "second"}}, /*bold=*/true);
    std::string md = convert_pages({p0, p1});
    // Two header rows survive -> two delimiter rows -> two tables.
    EXPECT_EQ(count_occurrences(md, "| --- | --- | --- |"), 2);
}

TEST(LayoutRuledGrid, SentencePeriodInWrappedCellGetsSpaceButUrlStaysTight) {
    // A cell wrapped after an abbreviation/sentence period needs a following space
    // when it rejoins ("i.e." + "sends" -> "i.e. sends"), but a period inside a
    // wrapped URL binds with no space ("www.iso." + "org" -> "www.iso.org").
    pdf2md::PageData p;
    p.width = 612;
    p.height = 792;
    std::vector<Free> gs;
    put_token(gs, "Term", 76, 716, 10, /*bold=*/true);
    put_token(gs, "Description", 216, 716, 10, true);
    // Row 1, right cell wraps after "i.e."
    put_token(gs, "Client", 76, 690);
    put_token(gs, "requester, i.e.", 216, 690);
    put_token(gs, "sends more", 216, 680);
    // Row 2, right cell wraps inside a URL.
    put_token(gs, "Ref", 76, 664);
    put_token(gs, "www.iso.", 216, 668);
    put_token(gs, "org", 216, 658);
    p.chars = make_free_page(gs).chars;
    hrule(p, 730, 71, 460);
    hrule(p, 710, 71, 460);   // header / body divider
    hrule(p, 676, 71, 460);   // row1 / row2 divider
    hrule(p, 652, 71, 460);
    vrule(p, 71, 652, 730);
    vrule(p, 200, 652, 730);
    vrule(p, 460, 652, 730);
    std::string md = convert(p);
    EXPECT_THAT(md, HasSubstr("i.e. sends"));
    EXPECT_THAT(md, Not(HasSubstr("i.e.sends")));
    EXPECT_THAT(md, HasSubstr("www.iso.org"));
    EXPECT_THAT(md, Not(HasSubstr("www.iso. org")));
}

TEST(LayoutSpacing, HyphenInsideTokenBindsWhenFalselySpaced) {
    // An intra-token hyphen that a near-threshold geometric gap falsely spaced
    // ("V1" + gap + "-0-0", from "ASAM_SOVD_BS_V1-0-0_PR") binds to its neighbours;
    // there is no space glyph, so the invented space is removed.
    std::vector<Free> gs;
    double x = put_tight(gs, "BS_V1", 72, 700);
    x += 3.4;                       // > word gap, but NO space glyph: a mis-split
    put_tight(gs, "-0-0_PR", x, 700);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("V1-0-0"));
    EXPECT_THAT(md, Not(HasSubstr("V1 -0-0")));
}

TEST(LayoutSpacing, RealSpacedHyphenRangeIsPreserved) {
    // A hyphen carrying its own space glyph on the left is a real dash between
    // tokens (a range), not a mis-split, and must keep its space.
    std::vector<Free> gs;
    double x = put_tight(gs, "pages", 72, 700);
    x += 3.0;
    x = put_tight(gs, "5", x, 700);
    gs.push_back({U' ', x, 700});   // an explicit separator glyph before the dash
    x += 3.0;
    put_tight(gs, "-9", x, 700);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("5 -9"));
}

TEST(LayoutBlocks, ReferenceEntryContinuationAndUrlRejoin) {
    // A bracketed reference entry ("[12] ISO ...") whose first line breaks mid-title
    // (no terminator), continues on the next page, and ends with a URL on its own
    // line. The three fragments rejoin into one paragraph.
    std::vector<pdf2md::PageData> pages;
    {  // page 1: the entry's first line, ending unterminated on "Harmonized".
        std::vector<Free> gs;
        double x = 72;
        for (const char* w : {"[12]", "ISO", "27145-3:2012", "Road", "vehicles",
                              "World-Wide", "Harmonized"}) {
            put_token(gs, w, x, 700);
            x += std::string(w).size() * 6.0 + 8.0;
        }
        pages.push_back(make_free_page(gs));
    }
    {  // page 2: the continuation clause, then a bare URL on its own line.
        std::vector<Free> gs;
        double x = 72;
        for (const char* w : {"On-Board", "Diagnostics", "communication", "Part", "3"}) {
            put_token(gs, w, x, 720);
            x += std::string(w).size() * 6.0 + 8.0;
        }
        put_token(gs, "https://www.iso.org/standard/46277.html", 72, 690);
        pages.push_back(make_free_page(gs));
    }
    std::string md = convert_pages(pages);
    // One paragraph carries both the entry title and the trailing URL.
    EXPECT_THAT(md, HasSubstr("Harmonized On-Board"));
    EXPECT_THAT(md, HasSubstr("Part 3 https://www.iso.org/standard/46277.html"));
}

TEST(LayoutBlocks, DashContinuationOfReferenceIsNotAList) {
    // A reference entry ("[21] ISO ... (CAN)") whose wrapped clause opens with a
    // dash ("- Part 2: ...") is that entry's continuation, not a bullet: the dash
    // marker is vetoed and the clause rejoins the entry paragraph.
    std::vector<Free> gs;
    double x = 72;
    for (const char* w : {"[21]", "ISO", "15765-2", "Road", "vehicles",
                          "Diagnostics", "on", "CAN"}) {
        put_token(gs, w, x, 700);
        x += std::string(w).size() * 6.0 + 8.0;
    }
    // Continuation, inset under the entry text, opening with a dash.
    double x2 = 92;
    put_token(gs, "-", x2, 686);
    x2 += 10;
    for (const char* w : {"Part", "2:", "Network", "layer", "services"}) {
        put_token(gs, w, x2, 686);
        x2 += std::string(w).size() * 6.0 + 8.0;
    }
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("CAN - Part 2: Network layer services"));
    EXPECT_THAT(md, Not(HasSubstr("\n- Part 2")));  // never emitted as a list item
}

TEST(LayoutHeadings, NumberedRunInTitlePromotedUnderMatchingParent) {
    // A bold run-in title, set below the heading-size band (so the size-based
    // promotions leave it alone), whose dotted number is one level deeper than an
    // enclosing heading of the same scheme ("3.2.1" under "3.2") becomes a heading;
    // a same-styled numbered line with no matching parent heading ("9.9.9", no
    // "9.9" heading) stays a bold paragraph -- the numbering evidence is required.
    std::vector<Free> gs;
    put_body(gs, 8, 740);
    // A real (enlarged) section heading "3.2 Setup".
    put_token(gs, "3.2", 72, 620, 15, /*bold=*/true);
    put_token(gs, "Setup", 108, 620, 15, true);
    // Run-in titles set slightly below body size (9.5 vs body 10): below the
    // bold-body heading band, so only the numbering rule can promote them.
    put_token(gs, "3.2.1", 72, 584, 9.5, true);
    put_token(gs, "Details", 108, 584, 9.5, true);
    put_token(gs, "9.9.9", 72, 548, 9.5, true);   // no "9.9" heading exists
    put_token(gs, "Orphan", 108, 548, 9.5, true);
    std::string md = convert(make_free_page(gs));
    EXPECT_THAT(md, HasSubstr("# 3.2.1 Details"));       // promoted to a heading
    EXPECT_THAT(md, Not(HasSubstr("**3.2.1 Details**")));
    EXPECT_THAT(md, HasSubstr("**9.9.9 Orphan**"));       // stays a bold paragraph
    EXPECT_THAT(md, Not(HasSubstr("# 9.9.9")));
}

}  // namespace
