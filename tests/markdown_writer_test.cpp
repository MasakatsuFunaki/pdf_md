// Unit tests for the markdown writer on hand-constructed documents.

#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "markdown_writer.h"

using ::testing::HasSubstr;
using pdf2md::Block;
using pdf2md::BlockKind;
using pdf2md::Document;
using pdf2md::Line;
using pdf2md::StyledRun;

namespace {

Line make_line(std::vector<StyledRun> runs) {
    Line line;
    line.runs = std::move(runs);
    for (const StyledRun& r : line.runs) line.plain += r.text;
    line.size = 11;
    return line;
}

Block paragraph(std::vector<StyledRun> runs) {
    Block b;
    b.kind = BlockKind::Paragraph;
    b.size = 11;
    b.lines.push_back(make_line(std::move(runs)));
    return b;
}

std::string render(Document doc, pdf2md::WriteOptions opts = {}) {
    return pdf2md::write_markdown(doc, opts);
}

TEST(MarkdownWriter, EscapesInlineSpecials) {
    Document doc;
    doc.blocks.push_back(paragraph({{U"stars *x* under_score [brackets] <tag> pipe| tick`", false, false, false}}));
    std::string md = render(doc);
    EXPECT_THAT(md, HasSubstr("stars \\*x\\* under\\_score \\[brackets] \\<tag> pipe\\| tick\\`"));
}

TEST(MarkdownWriter, BracketRefsNeverFormMathDelimiters) {
    // Math-enabled renderers (GitHub, MathJax) read "\[…\]" as a display-math
    // delimiter pair, so a citation like "ISO 14229-1[1]" escaped on both sides
    // would render as a centered equation on its own line. Only '[' is escaped.
    Document doc;
    doc.blocks.push_back(paragraph({{U"according to ISO 14229-1[1], see [2]", false, false, false}}));
    std::string md = render(doc);
    EXPECT_THAT(md, HasSubstr("according to ISO 14229-1\\[1], see \\[2]"));
    EXPECT_THAT(md, testing::Not(HasSubstr("\\]")));
}

TEST(MarkdownWriter, EscapesLineLeadingMarkers) {
    Document doc;
    doc.blocks.push_back(paragraph({{U"# not a heading", false, false, false}}));
    doc.blocks.push_back(paragraph({{U"- not a list", false, false, false}}));
    doc.blocks.push_back(paragraph({{U"12. not ordered", false, false, false}}));
    std::string md = render(doc);
    EXPECT_THAT(md, HasSubstr("\\# not a heading"));
    EXPECT_THAT(md, HasSubstr("\\- not a list"));
    EXPECT_THAT(md, HasSubstr("12\\. not ordered"));
}

TEST(MarkdownWriter, EmphasisMarkersHugText) {
    // Word spaces accumulate at the end of runs; markers must not wrap them.
    Document doc;
    doc.blocks.push_back(paragraph({
        {U"lead ", false, false, false},
        {U"bold ", true, false, false},
        {U"tail", false, false, false},
    }));
    std::string md = render(doc);
    EXPECT_THAT(md, HasSubstr("lead **bold** tail"));
}

TEST(MarkdownWriter, BoldItalicCombination) {
    Document doc;
    doc.blocks.push_back(paragraph({{U"both", true, true, false}}));
    EXPECT_THAT(render(doc), HasSubstr("***both***"));
}

TEST(MarkdownWriter, InlineCodeWithBacktickUsesLongerDelimiter) {
    Document doc;
    doc.blocks.push_back(paragraph({{U"a `tick` inside", false, false, true}}));
    EXPECT_THAT(render(doc), HasSubstr("``a `tick` inside``"));
}

TEST(MarkdownWriter, SoftHyphenJoinsLines) {
    Block b;
    b.kind = BlockKind::Paragraph;
    b.size = 11;
    b.lines.push_back(make_line({{std::u32string(U"para") + char32_t(0x00AD), false, false, false}}));
    b.lines.push_back(make_line({{U"graph continues", false, false, false}}));
    Document doc;
    doc.blocks.push_back(b);
    EXPECT_THAT(render(doc), HasSubstr("paragraph continues"));
}

TEST(MarkdownWriter, HyphenBeforeLowercaseJoins) {
    Block b;
    b.kind = BlockKind::Paragraph;
    b.size = 11;
    b.lines.push_back(make_line({{U"hyphen-", false, false, false}}));
    b.lines.push_back(make_line({{U"ation", false, false, false}}));
    Document doc;
    doc.blocks.push_back(b);
    EXPECT_THAT(render(doc), HasSubstr("hyphenation"));
}

TEST(MarkdownWriter, HyphenBeforeUppercaseKeepsCompound) {
    Block b;
    b.kind = BlockKind::Paragraph;
    b.size = 11;
    b.lines.push_back(make_line({{U"Anti-", false, false, false}}));
    b.lines.push_back(make_line({{U"Aliasing", false, false, false}}));
    Document doc;
    doc.blocks.push_back(b);
    EXPECT_THAT(render(doc), HasSubstr("Anti-Aliasing"));
}

// A line-end hyphen that breaks a genuine hyphenated compound must be kept: the
// pre-hyphen half ("context") is a word the document uses elsewhere and the
// halves do not spell a single attested word, so it is not a wrapped word.
TEST(MarkdownWriter, CompoundHyphenPreservedWhenBothSidesWordLike) {
    Document doc;
    // Context so "context" is an attested interior word of the document.
    doc.blocks.push_back(paragraph({{U"the context window here", false, false, false}}));
    Block b;
    b.kind = BlockKind::Paragraph;
    b.size = 11;
    b.lines.push_back(make_line({{std::u32string(U"context") + char32_t(0x00AD), false, false, false}}));
    b.lines.push_back(make_line({{U"aware routing", false, false, false}}));
    doc.blocks.push_back(b);
    std::string md = render(doc);
    EXPECT_THAT(md, HasSubstr("context-aware routing"));
    EXPECT_THAT(md, testing::Not(HasSubstr("contextaware")));
}

// The mirror case: a single word that merely wrapped at a syllable boundary
// whose prefix ("special") is itself a word. The joined form "specialization"
// is an attested word of the document, so the hyphen is dropped.
TEST(MarkdownWriter, WrappedWordDropsHyphenWhenJoinedFormIsAttested) {
    Document doc;
    doc.blocks.push_back(
        paragraph({{U"a special case of specialization here", false, false, false}}));
    Block b;
    b.kind = BlockKind::Paragraph;
    b.size = 11;
    b.lines.push_back(make_line({{std::u32string(U"special") + char32_t(0x00AD), false, false, false}}));
    b.lines.push_back(make_line({{U"ization done", false, false, false}}));
    doc.blocks.push_back(b);
    std::string md = render(doc);
    EXPECT_THAT(md, HasSubstr("specialization done"));
    EXPECT_THAT(md, testing::Not(HasSubstr("special-ization")));
}

// A wrap inside a multi-part hyphenated compound: "English-" ends the line and
// the continuation "to-German" itself opens with a hyphen. The short first
// segment ("to") would normally drop the wrap hyphen, but the continuation being
// a further hyphenated compound keeps it, so the word rejoins "English-to-German".
TEST(MarkdownWriter, CompoundHyphenKeptWhenContinuationIsHyphenated) {
    Document doc;
    // Context so "english" is an attested interior word of the document.
    doc.blocks.push_back(paragraph({{U"the English language here", false, false, false}}));
    Block b;
    b.kind = BlockKind::Paragraph;
    b.size = 11;
    b.lines.push_back(make_line({{std::u32string(U"English") + char32_t(0x00AD), false, false, false}}));
    b.lines.push_back(make_line({{U"to-German task", false, false, false}}));
    doc.blocks.push_back(b);
    std::string md = render(doc);
    EXPECT_THAT(md, HasSubstr("English-to-German task"));
    EXPECT_THAT(md, testing::Not(HasSubstr("Englishto")));
}

// A bound prefix ("multi") attaches to its stem without a hyphen, so a wrap
// after one is intra-word even when the joined word ("multiplicative") never
// recurs to attest it.
TEST(MarkdownWriter, WrappedWordDropsHyphenAfterBoundPrefix) {
    Document doc;
    doc.blocks.push_back(paragraph({{U"using multi head layers", false, false, false}}));
    Block b;
    b.kind = BlockKind::Paragraph;
    b.size = 11;
    b.lines.push_back(make_line({{std::u32string(U"multi") + char32_t(0x00AD), false, false, false}}));
    b.lines.push_back(make_line({{U"plicative weighting", false, false, false}}));
    doc.blocks.push_back(b);
    std::string md = render(doc);
    EXPECT_THAT(md, HasSubstr("multiplicative weighting"));
    EXPECT_THAT(md, testing::Not(HasSubstr("multi-plicative")));
}

// A brace-delimited placeholder identifier ("{<routine-interface-name-x>}")
// wrapped at its final hyphen: the token's braces and interior hyphens mark the
// trailing hyphen as a literal part of the name, so it survives the join even
// though the continuation ("x>}...") is far too short to attest a compound.
TEST(MarkdownWriter, PlaceholderTokenWrapKeepsLiteralHyphen) {
    Block b;
    b.kind = BlockKind::Paragraph;
    b.size = 11;
    b.lines.push_back(make_line({{U"API function {<routine-interface-name-", true, false, false}}));
    b.lines.push_back(make_line({{U"x>}::Result done", true, false, false}}));
    Document doc;
    doc.blocks.push_back(b);
    std::string md = render(doc);
    EXPECT_THAT(md, HasSubstr("routine-interface-name-x>}"));
    EXPECT_THAT(md, testing::Not(HasSubstr("interface-namex")));
}

// The guard on interior-hyphen evidence: a compound that wrapped at a syllable
// inside its last element ("self-atten-" + "tion") still dehyphenates, because
// the fragment after the last interior hyphen joins the continuation into an
// attested document word ("attention").
TEST(MarkdownWriter, SyllableWrapInsideCompoundStillDehyphenates) {
    Document doc;
    // Context so "attention" is an attested interior word of the document.
    doc.blocks.push_back(paragraph({{U"the attention mechanism here", false, false, false}}));
    Block b;
    b.kind = BlockKind::Paragraph;
    b.size = 11;
    b.lines.push_back(make_line({{U"uses self-atten-", false, false, false}}));
    b.lines.push_back(make_line({{U"tion layers", false, false, false}}));
    doc.blocks.push_back(b);
    std::string md = render(doc);
    EXPECT_THAT(md, HasSubstr("self-attention layers"));
    EXPECT_THAT(md, testing::Not(HasSubstr("atten-tion")));
}

// A URL split across a wrapped line must join without the inserted word space.
TEST(MarkdownWriter, WrappedUrlJoinsWithoutSpace) {
    Block b;
    b.kind = BlockKind::Paragraph;
    b.size = 11;
    b.lines.push_back(make_line({{U"code at https://example.org/", false, false, false}}));
    b.lines.push_back(make_line({{U"path/to/resource", false, false, false}}));
    Document doc;
    doc.blocks.push_back(b);
    std::string md = render(doc);
    EXPECT_THAT(md, HasSubstr("https://example.org/path/to/resource"));
    EXPECT_THAT(md, testing::Not(HasSubstr("example.org/ path")));
}

// A capitalized filename listed after a bare host (no trailing path separator)
// is a new token, not a continuation of the link: it must not glue to the host.
TEST(MarkdownWriter, HostFollowedByCapitalizedFilenameKeepsSpace) {
    Block b;
    b.kind = BlockKind::Paragraph;
    b.size = 11;
    b.lines.push_back(make_line({{U"see http://www.example.net", false, false, false}}));
    b.lines.push_back(make_line({{U"Report_V1-0.pdf", false, false, false}}));
    Document doc;
    doc.blocks.push_back(b);
    std::string md = render(doc);
    EXPECT_THAT(md, testing::Not(HasSubstr("example.netReport")));
    EXPECT_THAT(md, HasSubstr("example.net Report"));
}

// A genuine path continuation (host ended with '/') still joins even when the
// next segment happens to start with a capital letter.
TEST(MarkdownWriter, WrappedUrlPathJoinsAcrossCapitalSegment) {
    Block b;
    b.kind = BlockKind::Paragraph;
    b.size = 11;
    b.lines.push_back(make_line({{U"at https://example.org/", false, false, false}}));
    b.lines.push_back(make_line({{U"Docs/page", false, false, false}}));
    Document doc;
    doc.blocks.push_back(b);
    EXPECT_THAT(render(doc), HasSubstr("https://example.org/Docs/page"));
}

// Two independent URLs wrapped onto adjacent lines (a duplicated reference
// entry) must stay separated: the second URL is a new link, not a continuation
// of the first, even though the first ends without a path separator.
TEST(MarkdownWriter, TwoIndependentUrlsStaySeparated) {
    Block b;
    b.kind = BlockKind::Paragraph;
    b.size = 11;
    b.lines.push_back(make_line({{U"https://www.rfc-editor.org/rfc/rfc4291.txt",
                                   false, false, false}}));
    b.lines.push_back(make_line({{U"https://www.rfc-editor.org/rfc/rfc4291.txt",
                                   false, false, false}}));
    Document doc;
    doc.blocks.push_back(b);
    std::string md = render(doc);
    EXPECT_THAT(md, testing::Not(HasSubstr("rfc4291.txthttps://")));
    EXPECT_THAT(md, HasSubstr("rfc4291.txt https://"));
}

// A bare "www." host followed by a wrapped "www." host is likewise two links.
TEST(MarkdownWriter, WwwHostFollowedByNewWwwUrlStaysSeparated) {
    Block b;
    b.kind = BlockKind::Paragraph;
    b.size = 11;
    b.lines.push_back(make_line({{U"www.example.org", false, false, false}}));
    b.lines.push_back(make_line({{U"www.other.net", false, false, false}}));
    Document doc;
    doc.blocks.push_back(b);
    std::string md = render(doc);
    EXPECT_THAT(md, testing::Not(HasSubstr("example.orgwww")));
    EXPECT_THAT(md, HasSubstr("example.org www.other.net"));
}

TEST(MarkdownWriter, HeadingStripsInlineStyles) {
    Block b;
    b.kind = BlockKind::Heading;
    b.heading_level = 2;
    b.size = 16;
    b.lines.push_back(make_line({{U"Bold Title", true, false, false}}));
    Document doc;
    doc.blocks.push_back(b);
    EXPECT_THAT(render(doc), HasSubstr("## Bold Title\n"));
}

TEST(MarkdownWriter, ConsecutiveListItemsAreTight) {
    Document doc;
    for (const char32_t* text : {U"one", U"two"}) {
        Block b;
        b.kind = BlockKind::ListItem;
        b.size = 11;
        b.list_marker = U"-";
        b.lines.push_back(make_line({{text, false, false, false}}));
        doc.blocks.push_back(b);
    }
    EXPECT_THAT(render(doc), HasSubstr("- one\n- two\n"));
}

TEST(MarkdownWriter, TableCellsEscapePipes) {
    auto cell = [](const char32_t* t) { return pdf2md::TableCell{StyledRun{t}}; };
    Block b;
    b.kind = BlockKind::Table;
    b.table_cells = {{cell(U"a|b"), cell(U"c")}, {cell(U"1"), cell(U"2")}};
    Document doc;
    doc.blocks.push_back(b);
    std::string md = render(doc);
    EXPECT_THAT(md, HasSubstr("| a\\|b | c |"));
    EXPECT_THAT(md, HasSubstr("| --- | --- |"));
    EXPECT_THAT(md, HasSubstr("| 1 | 2 |"));
}

TEST(MarkdownWriter, TableCellMathAndCodePipesEscaped) {
    // Math and code-span runs bypass escape_md, but GFM splits a row at any
    // unescaped '|' even inside $...$ or `...`; the cell must escape them.
    StyledRun math;
    math.text = U"P(A|B)";
    math.math = true;
    StyledRun code;
    code.text = U"col|val";
    code.mono = true;
    Block b;
    b.kind = BlockKind::Table;
    b.table_cells = {{pdf2md::TableCell{StyledRun{U"h1"}}, pdf2md::TableCell{StyledRun{U"h2"}}},
                     {pdf2md::TableCell{math}, pdf2md::TableCell{code}}};
    Document doc;
    doc.blocks.push_back(b);
    std::string md = render(doc);
    EXPECT_THAT(md, HasSubstr("| $P(A\\|B)$ | `col\\|val` |"));
}

TEST(MarkdownWriter, UppercaseContinuationNormalizesUnicodeHyphen) {
    // A compound split at a U+2010 hyphen with an uppercase continuation joins
    // with an ASCII '-', matching the lowercase-continuation path.
    Block b;
    b.kind = BlockKind::Paragraph;
    b.size = 11;
    b.lines.push_back(make_line({{U"the English‐", false, false, false}}));
    b.lines.push_back(make_line({{U"German translation", false, false, false}}));
    Document doc;
    doc.blocks.push_back(b);
    std::string md = render(doc);
    EXPECT_THAT(md, HasSubstr("English-German translation"));
}

TEST(MarkdownWriter, TableRendersMathAndBoldHeaderStripped) {
    auto cell = [](std::vector<StyledRun> runs) { return pdf2md::TableCell(std::move(runs)); };
    Block b;
    b.kind = BlockKind::Table;
    StyledRun model{U"Model"}; model.bold = true;   // header emphasis is dropped
    StyledRun cost{U"Cost"};   cost.bold = true;
    StyledRun pow{U"10^{18}"}; pow.math = true;      // math cell -> $...$
    StyledRun big{U"28.4"};    big.bold = true;      // bold data cell -> **...**
    b.table_cells = {
        {cell({model}), cell({cost})},
        {cell({{U"base"}}), cell({pow})},
        {cell({{U"big"}}), cell({big})},
    };
    Document doc;
    doc.blocks.push_back(b);
    std::string md = render(doc);
    EXPECT_THAT(md, HasSubstr("| Model | Cost |"));  // header row: no ** markers
    EXPECT_THAT(md, HasSubstr("$10^{18}$"));
    EXPECT_THAT(md, HasSubstr("**28.4**"));
}

TEST(MarkdownWriter, ImageBlockRendersReference) {
    Block b;
    b.kind = BlockKind::Image;
    b.image_path = "images/pic 1.png";
    Document doc;
    doc.blocks.push_back(b);
    EXPECT_THAT(render(doc), HasSubstr("![](<images/pic 1.png>)"));
}

TEST(MarkdownWriter, EscapesTildes) {
    Document doc;
    doc.blocks.push_back(paragraph({{U"file ~config~ and ~~gone~~", false, false, false}}));
    std::string md = render(doc);
    EXPECT_THAT(md, HasSubstr("file \\~config\\~ and \\~\\~gone\\~\\~"));
}

TEST(MarkdownWriter, ListItemContentEscapesLeadingMarkers) {
    Block b;
    b.kind = BlockKind::ListItem;
    b.size = 11;
    b.list_marker = U"-";
    b.lines.push_back(make_line({{U"1. Introduction", false, false, false}}));
    Document doc;
    doc.blocks.push_back(b);
    EXPECT_THAT(render(doc), HasSubstr("- 1\\. Introduction"));
}

TEST(MarkdownWriter, HeadingKeepsTrailingHash) {
    Block b;
    b.kind = BlockKind::Heading;
    b.heading_level = 1;
    b.size = 20;
    b.lines.push_back(make_line({{U"Section 3 #", false, false, false}}));
    Document doc;
    doc.blocks.push_back(b);
    EXPECT_THAT(render(doc), HasSubstr("# Section 3 \\#"));
}

TEST(MarkdownWriter, RawRunEmittedVerbatim) {
    // A raw run (a footnote reference the analyzer injects) is markdown, not text:
    // it must reach the output without the "[" being escaped to "\[".
    StyledRun word{U"GPU"};
    StyledRun ref;
    ref.text = U"[^5]";
    ref.raw = true;
    Document doc;
    doc.blocks.push_back(paragraph({word, ref}));
    std::string md = render(doc);
    EXPECT_THAT(md, HasSubstr("GPU[^5]"));
    EXPECT_THAT(md, testing::Not(HasSubstr("\\[^5]")));
}

TEST(MarkdownWriter, NumberedFootnoteRendersDefinition) {
    Block b;
    b.kind = BlockKind::Paragraph;
    b.size = 9;
    b.is_footnote = true;
    b.footnote_numbered = true;
    b.footnote_label = U"5";
    b.lines.push_back(make_line({{U"We used values.", false, false, false}}));
    Document doc;
    doc.blocks.push_back(b);
    EXPECT_THAT(render(doc), HasSubstr("[^5]: We used values."));
}

TEST(MarkdownWriter, SymbolFootnotePrefixesLiterally) {
    // A dagger affiliation note keeps its marker as a literal prefix, not $\dagger$.
    Block b;
    b.kind = BlockKind::Paragraph;
    b.size = 9;
    b.is_footnote = true;
    b.footnote_numbered = false;
    b.footnote_label = U"†";  // dagger
    b.lines.push_back(make_line({{U"Work performed while at X.", false, false, false}}));
    Document doc;
    doc.blocks.push_back(b);
    EXPECT_THAT(render(doc), HasSubstr("†Work performed while at X."));
}

TEST(MarkdownWriter, BlockquoteParagraphPrefixed) {
    Block b;
    b.kind = BlockKind::Paragraph;
    b.size = 11;
    b.blockquote = true;
    b.lines.push_back(make_line({{U"A reproduced legal note.", false, false, false}}));
    Document doc;
    doc.blocks.push_back(b);
    EXPECT_THAT(render(doc), HasSubstr("> A reproduced legal note."));
}

TEST(MarkdownWriter, FrontMatterEscapesQuotes) {
    Document doc;
    doc.meta.title = "He said \"hi\"";
    doc.meta.page_count = 1;
    doc.blocks.push_back(paragraph({{U"body", false, false, false}}));
    pdf2md::WriteOptions opts;
    opts.front_matter = true;
    EXPECT_THAT(render(doc, opts), HasSubstr("title: \"He said \\\"hi\\\"\""));
}

}  // namespace
