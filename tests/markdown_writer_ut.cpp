#include "pdf2md/markdown_writer.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using pdf2md::Block;
using pdf2md::BlockKind;
using pdf2md::Document;
using pdf2md::Line;
using pdf2md::StyledRun;
using pdf2md::TableAlign;
using pdf2md::TableCell;
using pdf2md::TaskState;
using pdf2md::WriteOptions;

constexpr double kDefaultFontSize = 11.0;
constexpr double kCodeFontSize = 10.0;
constexpr double kFootnoteFontSize = 9.0;
constexpr double kHeadingFontSize = 16.0;

Line make_line(std::vector<StyledRun> runs) {
    Line line;
    line.runs = std::move(runs);
    for (const StyledRun& r : line.runs) {
        line.plain += r.text;
    }
    line.size = kDefaultFontSize;
    return line;
}

Block make_paragraph(std::vector<StyledRun> runs, int page = 0) {
    Block b;
    b.kind = BlockKind::Paragraph;
    b.page = page;
    b.size = kDefaultFontSize;
    b.lines.push_back(make_line(std::move(runs)));
    return b;
}

Block make_wrapped_paragraph(std::vector<std::vector<StyledRun>> line_runs) {
    Block b;
    b.kind = BlockKind::Paragraph;
    b.size = kDefaultFontSize;
    for (auto& runs : line_runs) {
        b.lines.push_back(make_line(std::move(runs)));
    }
    return b;
}

TEST(MarkdownWriter, EmptyDocumentProducesEmptyString) {
    const Document empty_doc;
    EXPECT_EQ(pdf2md::write_markdown(empty_doc, WriteOptions{}), "");
}

struct PlainEscapeCase {
    const char* name;
    const char32_t* input;
    const char* expected_md;
};

constexpr PlainEscapeCase kEscapeCases[] = {
    {"inline_specials_without_closing_bracket_escape",
     U"stars *x* under_score [brackets] <tag> pipe| tick`",
     "stars \\*x\\* under\\_score \\[brackets] \\<tag> pipe\\| tick\\`\n"},
    {"bracket_citations_never_form_display_math_delimiter",
     U"according to ISO 14229-1[1], see [2]",
     "according to ISO 14229-1\\[1], see \\[2]\n"},
    {"tilde_strikethrough_markers_escaped",
     U"file ~config~ and ~~gone~~",
     "file \\~config\\~ and \\~\\~gone\\~\\~\n"},
    {"leading_atx_hash_escaped", U"# not a heading", "\\# not a heading\n"},
    {"leading_unordered_dash_escaped", U"- not a list", "\\- not a list\n"},
    {"leading_unordered_plus_escaped", U"+ not a list", "\\+ not a list\n"},
    {"leading_blockquote_angle_escaped", U"> not a quote", "\\> not a quote\n"},
    {"leading_ordered_dot_escaped", U"12. not ordered", "12\\. not ordered\n"},
};

constexpr std::size_t kExpectedEscapeCaseCount = 8;
static_assert(std::size(kEscapeCases) == kExpectedEscapeCaseCount);

TEST(MarkdownWriter, EscapesInlineAndLineLeadingMarkdownSyntax) {
    ASSERT_EQ(std::size(kEscapeCases), kExpectedEscapeCaseCount);
    for (const auto& tc : kEscapeCases) {
        SCOPED_TRACE(tc.name);
        Document doc;
        doc.blocks.push_back(make_paragraph({StyledRun{tc.input}}));
        EXPECT_EQ(pdf2md::write_markdown(doc, WriteOptions{}), tc.expected_md);
    }
}

struct StyledRunCase {
    const char* name;
    std::vector<StyledRun> runs;
    const char* expected_md;
};

TEST(MarkdownWriter, FormatsInlineStyledRunsAndDelimiters) {
    StyledRun math_run;
    math_run.text = U"W_i^Q + \\beta_1";
    math_run.math = true;

    StyledRun raw_ref;
    raw_ref.text = U"[^5]";
    raw_ref.raw = true;

    const std::vector<StyledRunCase> cases = {
        {"emphasis_hugs_non_space_text",
         {{U"lead ", false, false, false},
          {U"bold ", true, false, false},
          {U"italic ", false, true, false},
          {U"tail", false, false, false}},
         "lead **bold** *italic* tail\n"},
        {"leading_and_trailing_run_spaces_pushed_outside_markers",
         {{U"lead", false, false, false},
          {U" bold ", true, false, false},
          {U"tail", false, false, false}},
         "lead **bold** tail\n"},
        {"bold_and_italic_combined",
         {{U"both", true, true, false}},
         "***both***\n"},
        {"inline_code_plain",
         {{U"fn(a_b * c)", false, false, true}},
         "`fn(a_b * c)`\n"},
        {"inline_code_with_embedded_backtick_uses_longer_fence",
         {{U"a `tick` inside", false, false, true}},
         "``a `tick` inside``\n"},
        {"inline_math_wrapped_verbatim_in_dollars",
         {{U"let ", false, false, false}, math_run, {U" hold", false, false, false}},
         "let $W_i^Q + \\beta_1$ hold\n"},
        {"raw_run_emitted_verbatim_without_bracket_escaping",
         {{U"GPU", false, false, false}, raw_ref, {U" [note]", false, false, false}},
         "GPU[^5] \\[note]\n"},
    };

    constexpr std::size_t kExpectedStyledCount = 7;
    ASSERT_EQ(cases.size(), kExpectedStyledCount);
    for (const auto& tc : cases) {
        SCOPED_TRACE(tc.name);
        Document doc;
        doc.blocks.push_back(make_paragraph(tc.runs));
        EXPECT_EQ(pdf2md::write_markdown(doc, WriteOptions{}), tc.expected_md);
    }
}

struct WrapRepairCase {
    const char* name;
    const char32_t* context_paragraph;  // optional attesting paragraph
    StyledRun first_line;
    StyledRun second_line;
    const char* expected_md;
};

TEST(MarkdownWriter, RepairsLineWrapHyphensAndUrlsAcrossSeams) {
    const std::u32string soft_para = std::u32string(U"para") + char32_t{0x00ADU};
    const std::u32string soft_context = std::u32string(U"context") + char32_t{0x00ADU};
    const std::u32string soft_special = std::u32string(U"special") + char32_t{0x00ADU};
    const std::u32string soft_english = std::u32string(U"English") + char32_t{0x00ADU};
    const std::u32string soft_multi = std::u32string(U"multi") + char32_t{0x00ADU};
    const std::u32string unicode_hyphen_english = std::u32string(U"the English") + char32_t{0x2010U};

    const std::vector<WrapRepairCase> cases = {
        {"soft_hyphen_joins_wrapped_word",
         nullptr,
         StyledRun{soft_para},
         StyledRun{U"graph continues"},
         "paragraph continues\n"},
        {"ascii_hyphen_before_lowercase_joins",
         nullptr,
         StyledRun{U"hyphen-"},
         StyledRun{U"ation"},
         "hyphenation\n"},
        {"ascii_hyphen_before_uppercase_keeps_compound",
         nullptr,
         StyledRun{U"Anti-"},
         StyledRun{U"Aliasing"},
         "Anti-Aliasing\n"},
        {"unicode_u2010_hyphen_before_uppercase_normalizes_to_ascii",
         nullptr,
         StyledRun{unicode_hyphen_english},
         StyledRun{U"German translation"},
         "the English-German translation\n"},
        {"attested_prefix_preserves_compound_hyphen",
         U"the context window here",
         StyledRun{soft_context},
         StyledRun{U"aware routing"},
         "the context window here\n\ncontext-aware routing\n"},
        {"attested_joined_word_drops_wrap_hyphen",
         U"a special case of specialization here",
         StyledRun{soft_special},
         StyledRun{U"ization done"},
         "a special case of specialization here\n\nspecialization done\n"},
        {"continuation_with_own_hyphen_keeps_multi_part_compound",
         U"the English language here",
         StyledRun{soft_english},
         StyledRun{U"to-German task"},
         "the English language here\n\nEnglish-to-German task\n"},
        {"bound_prefix_drops_hyphen_without_attested_full_word",
         U"using multi head layers",
         StyledRun{soft_multi},
         StyledRun{U"plicative weighting"},
         "using multi head layers\n\nmultiplicative weighting\n"},
        {"brace_placeholder_identifier_keeps_literal_trailing_hyphen",
         nullptr,
         StyledRun{U"API function {<routine-interface-name-", true, false, false},
         StyledRun{U"x>}::Result done", true, false, false},
         "**API function {\\<routine-interface-name-x>}::Result done**\n"},
        {"syllable_wrap_inside_compound_still_dehyphenates_attested_tail",
         U"the attention mechanism here",
         StyledRun{U"uses self-atten-"},
         StyledRun{U"tion layers"},
         "the attention mechanism here\n\nuses self-attention layers\n"},
        {"wrapped_url_with_slash_joins_lowercase_path",
         nullptr,
         StyledRun{U"code at https://example.org/"},
         StyledRun{U"path/to/resource"},
         "code at https://example.org/path/to/resource\n"},
        {"wrapped_url_with_slash_joins_capitalized_path_segment",
         nullptr,
         StyledRun{U"at https://example.org/"},
         StyledRun{U"Docs/page"},
         "at https://example.org/Docs/page\n"},
        {"bare_host_followed_by_capitalized_filename_keeps_space",
         nullptr,
         StyledRun{U"see http://www.example.net"},
         StyledRun{U"Report_V1-0.pdf"},
         "see http://www.example.net Report\\_V1-0.pdf\n"},
        {"two_independent_https_urls_stay_space_separated",
         nullptr,
         StyledRun{U"https://www.rfc-editor.org/rfc/rfc4291.txt"},
         StyledRun{U"https://www.rfc-editor.org/rfc/rfc4291.txt"},
         "https://www.rfc-editor.org/rfc/rfc4291.txt https://www.rfc-editor.org/rfc/rfc4291.txt\n"},
        {"two_independent_www_hosts_stay_space_separated",
         nullptr,
         StyledRun{U"www.example.org"},
         StyledRun{U"www.other.net"},
         "www.example.org www.other.net\n"},
    };

    constexpr std::size_t kExpectedWrapCaseCount = 15;
    ASSERT_EQ(cases.size(), kExpectedWrapCaseCount);
    for (const auto& tc : cases) {
        SCOPED_TRACE(tc.name);
        Document doc;
        if (tc.context_paragraph != nullptr) {
            doc.blocks.push_back(make_paragraph({StyledRun{tc.context_paragraph}}));
        }
        doc.blocks.push_back(make_wrapped_paragraph({{tc.first_line}, {tc.second_line}}));
        EXPECT_EQ(pdf2md::write_markdown(doc, WriteOptions{}), tc.expected_md);
    }
}

struct HeadingCase {
    const char* name;
    int level;
    const char32_t* text;
    bool bold;
    bool italic;
    const char* expected_md;
};

constexpr HeadingCase kHeadingCases[] = {
    {"clamps_zero_to_h1", 0, U"Zero Clamped", false, false, "# Zero Clamped\n"},
    {"clamps_negative_to_h1", -3, U"Negative Clamped", false, false, "# Negative Clamped\n"},
    {"h1_escapes_trailing_hash", 1, U"Section 3 #", false, false, "# Section 3 \\#\n"},
    {"h2_strips_bold_and_italic_emphasis", 2, U"Bold Title", true, true, "## Bold Title\n"},
    {"h6_max_valid_atx_level", 6, U"Deep Heading", false, false, "###### Deep Heading\n"},
    {"clamps_above_six_to_h6", 9, U"Clamped Max", false, false, "###### Clamped Max\n"},
};

constexpr std::size_t kExpectedHeadingCaseCount = 6;
static_assert(std::size(kHeadingCases) == kExpectedHeadingCaseCount);

TEST(MarkdownWriter, RendersHeadingsAcrossLevelsWithClampingAndStyleStripping) {
    ASSERT_EQ(std::size(kHeadingCases), kExpectedHeadingCaseCount);
    for (const auto& tc : kHeadingCases) {
        SCOPED_TRACE(tc.name);
        Block b;
        b.kind = BlockKind::Heading;
        b.heading_level = tc.level;
        b.size = kHeadingFontSize;
        b.lines.push_back(make_line({StyledRun{tc.text, tc.bold, tc.italic, false}}));
        Document doc;
        doc.blocks.push_back(std::move(b));
        EXPECT_EQ(pdf2md::write_markdown(doc, WriteOptions{}), tc.expected_md);
    }
}

TEST(MarkdownWriter, RendersTightOrderedUnorderedAndNestedLists) {
    auto make_item = [](const char32_t* marker, bool ordered, int indent, const char32_t* text) {
        Block b;
        b.kind = BlockKind::ListItem;
        b.size = kDefaultFontSize;
        b.list_marker = marker;
        b.list_ordered = ordered;
        b.list_indent = indent;
        b.lines.push_back(make_line({StyledRun{text}}));
        return b;
    };

    Document doc;
    doc.blocks.push_back(make_paragraph({StyledRun{U"Intro"}}));
    doc.blocks.push_back(make_item(U"-", false, 0, U"one"));
    doc.blocks.push_back(make_item(U"-", false, 1, U"1. Introduction"));
    doc.blocks.push_back(make_item(U"-", false, 2, U"deep child"));
    doc.blocks.push_back(make_item(U"2.", true, 0, U"second"));
    doc.blocks.push_back(make_paragraph({StyledRun{U"Outro"}}));

    const std::string expected =
        "Intro\n\n"
        "- one\n"
        "    - 1\\. Introduction\n"
        "        - deep child\n\n"
        "2. second\n\n"
        "Outro\n";
    EXPECT_EQ(pdf2md::write_markdown(doc, WriteOptions{}), expected);
}

TEST(MarkdownWriter, RendersTaskListsNestingAndTopLevelListKindTransitions) {
    auto make_item = [](const char32_t* marker, bool ordered, int indent, const char32_t* text,
                        TaskState task = TaskState::None) {
        Block b;
        b.kind = BlockKind::ListItem;
        b.size = kDefaultFontSize;
        b.list_marker = marker;
        b.list_ordered = ordered;
        b.list_indent = indent;
        b.task_state = task;
        b.lines.push_back(make_line({StyledRun{text}}));
        return b;
    };

    Document doc;
    doc.blocks.push_back(make_item(U"-", false, 0, U"Write [unit] tests", TaskState::Unchecked));
    doc.blocks.push_back(make_item(U"-", false, 0, U"Verify release", TaskState::Checked));
    doc.blocks.push_back(make_item(U"-", false, 1, U"Nested task", TaskState::Checked));
    doc.blocks.push_back(make_item(U"-", false, 0, U"Final bullet"));
    doc.blocks.push_back(make_item(U"1.", true, 0, U"First ordered"));
    doc.blocks.push_back(make_item(U"2.", true, 0, U"Second ordered"));

    const std::string expected =
        "- [ ] Write \\[unit] tests\n"
        "- [x] Verify release\n"
        "    - [x] Nested task\n"
        "- Final bullet\n\n"
        "1. First ordered\n"
        "2. Second ordered\n";
    EXPECT_EQ(pdf2md::write_markdown(doc, WriteOptions{}), expected);
}

Line make_code_line(std::u32string_view content, int indent_cols, double block_x0,
                    double baseline_y) {
    constexpr double kMonoPitch = 6.0;
    Line line;
    line.size = kCodeFontSize;
    line.baseline = baseline_y;
    line.mono_majority = true;
    const double start_x = block_x0 + static_cast<double>(indent_cols) * kMonoPitch;
    line.x0 = start_x;
    double cur_x = start_x;
    for (char32_t cp : content) {
        pdf2md::CharInfo ch;
        ch.code = cp;
        ch.x = cur_x;
        ch.y = baseline_y;
        ch.left = cur_x;
        ch.right = cur_x + kMonoPitch;
        ch.bottom = baseline_y;
        ch.top = baseline_y + kCodeFontSize;
        ch.size = kCodeFontSize;
        ch.mono = true;
        ch.space = (cp == U' ');
        line.chars.push_back(ch);
        cur_x += kMonoPitch;
    }
    line.x1 = cur_x;
    line.plain = std::u32string(content);
    line.runs.push_back(StyledRun{line.plain, false, false, true});
    return line;
}

struct CodeFenceCase {
    const char* name;
    std::u32string_view line1;
    int indent1;
    std::u32string_view line2;
    int indent2;
    const char* expected_md;
};

constexpr CodeFenceCase kCodeCases[] = {
    {"standard_three_backtick_fence_reconstructs_glyph_indentation_and_raw_chars",
     U"if (a * b < c_d) {",
     0,
     U"return x | y;",
     4,
     "```\nif (a * b < c_d) {\n    return x | y;\n```\n"},
    {"embedded_triple_backtick_upgrades_outer_fence_to_four",
     U"markdown example:",
     0,
     U"```cpp",
     0,
     "````\nmarkdown example:\n```cpp\n````\n"},
    {"embedded_quadruple_backtick_upgrades_outer_fence_to_five",
     U"````",
     0,
     U"nested fence",
     0,
     "`````\n````\nnested fence\n`````\n"},
};

constexpr std::size_t kExpectedCodeCaseCount = 3;
static_assert(std::size(kCodeCases) == kExpectedCodeCaseCount);

TEST(MarkdownWriter, RendersFencedCodeBlocksAndResolvesBacktickCollisions) {
    constexpr double kBlockLeftX = 72.0;
    constexpr double kLine1Baseline = 700.0;
    constexpr double kLine2Baseline = 688.0;

    ASSERT_EQ(std::size(kCodeCases), kExpectedCodeCaseCount);
    for (const auto& tc : kCodeCases) {
        SCOPED_TRACE(tc.name);
        Block b;
        b.kind = BlockKind::Code;
        b.x0 = kBlockLeftX;
        b.size = kCodeFontSize;
        b.lines.push_back(make_code_line(tc.line1, tc.indent1, kBlockLeftX, kLine1Baseline));
        b.lines.push_back(make_code_line(tc.line2, tc.indent2, kBlockLeftX, kLine2Baseline));
        Document doc;
        doc.blocks.push_back(std::move(b));
        EXPECT_EQ(pdf2md::write_markdown(doc, WriteOptions{}), tc.expected_md);
    }
}

TEST(MarkdownWriter, RendersTablesWithHeaderCleanupAndPipeEscaping) {
    StyledRun bold_header_1{U"Model", true, false, false};
    StyledRun bold_header_2{U"Metric|Spec", true, false, false};

    StyledRun math_pipe;
    math_pipe.text = U"P(A|B)";
    math_pipe.math = true;

    StyledRun code_pipe;
    code_pipe.text = U"col|val";
    code_pipe.mono = true;

    StyledRun math_pow;
    math_pow.text = U"10^{18}";
    math_pow.math = true;

    StyledRun bold_val{U"28.4", true, false, false};

    Block table;
    table.kind = BlockKind::Table;
    table.table_cells = {
        {TableCell{bold_header_1}, TableCell{bold_header_2}},
        {TableCell{math_pipe}, TableCell{code_pipe}},
        {TableCell{math_pow}, TableCell{bold_val}},
    };

    Document doc;
    doc.blocks.push_back(std::move(table));

    const std::string expected =
        "| Model | Metric\\|Spec |\n"
        "| --- | --- |\n"
        "| $P(A\\|B)$ | `col\\|val` |\n"
        "| $10^{18}$ | **28.4** |\n";
    EXPECT_EQ(pdf2md::write_markdown(doc, WriteOptions{}), expected);
}

TEST(MarkdownWriter, RendersImagesDisplayMathBlockquotesAndFootnotes) {
    Document doc;

    // Paragraph with raw footnote reference.
    StyledRun ref;
    ref.text = U"[^5]";
    ref.raw = true;
    doc.blocks.push_back(make_paragraph({StyledRun{U"Claim"}, ref}));

    // Symbol footnote: rendered with literal symbol prefix, not LaTeX math.
    Block symbol_fn;
    symbol_fn.kind = BlockKind::Paragraph;
    symbol_fn.size = kFootnoteFontSize;
    symbol_fn.is_footnote = true;
    symbol_fn.footnote_numbered = false;
    symbol_fn.footnote_label = U"\u2020";
    symbol_fn.lines.push_back(make_line({StyledRun{U"Work performed while at X."}}));
    doc.blocks.push_back(std::move(symbol_fn));

    // Blockquote paragraph.
    Block quote = make_paragraph({StyledRun{U"A reproduced legal note."}});
    quote.blockquote = true;
    doc.blocks.push_back(std::move(quote));

    // Clean image path and space-containing image path (angle-bracketed GFM destination).
    Block img_clean;
    img_clean.kind = BlockKind::Image;
    img_clean.image_path = "images/fig1.png";
    doc.blocks.push_back(std::move(img_clean));

    Block img_space;
    img_space.kind = BlockKind::Image;
    img_space.image_path = "images/pic 1.png";
    doc.blocks.push_back(std::move(img_space));

    // Single-line display math with equation tag.
    Block math_single;
    math_single.kind = BlockKind::MathDisplay;
    math_single.math_lines = {U"E = mc^2"};
    math_single.math_tag = U"1";
    doc.blocks.push_back(std::move(math_single));

    // Multi-line aligned display math without tag, and with outer equation tag.
    Block math_multi_untagged;
    math_multi_untagged.kind = BlockKind::MathDisplay;
    math_multi_untagged.math_lines = {U"x &= 1", U"y &= 2"};
    doc.blocks.push_back(std::move(math_multi_untagged));

    Block math_multi_tagged;
    math_multi_tagged.kind = BlockKind::MathDisplay;
    math_multi_tagged.math_lines = {U"a + b = c", U"d + e = f"};
    math_multi_tagged.math_tag = U"2";
    doc.blocks.push_back(std::move(math_multi_tagged));

    // Numbered footnote definition at document tail (placed by layout pass pipeline).
    Block numbered_fn;
    numbered_fn.kind = BlockKind::Paragraph;
    numbered_fn.size = kFootnoteFontSize;
    numbered_fn.is_footnote = true;
    numbered_fn.footnote_numbered = true;
    numbered_fn.footnote_label = U"5";
    numbered_fn.lines.push_back(make_line({StyledRun{U"We used values."}}));
    doc.blocks.push_back(std::move(numbered_fn));

    const std::string expected =
        "Claim[^5]\n\n"
        "\u2020Work performed while at X.\n\n"
        "> A reproduced legal note.\n\n"
        "![](images/fig1.png)\n\n"
        "![](<images/pic 1.png>)\n\n"
        "$$\nE = mc^2 \\tag{1}\n$$\n\n"
        "$$\n\\begin{aligned}\nx &= 1 \\\\\ny &= 2\n\\end{aligned}\n$$\n\n"
        "$$\n\\begin{aligned}\na + b = c \\\\\nd + e = f\n\\end{aligned} \\tag{2}\n$$\n\n"
        "[^5]: We used values.\n";
    EXPECT_EQ(pdf2md::write_markdown(doc, WriteOptions{}), expected);
}

TEST(MarkdownWriter, EmitsFrontMatterAndPageBreaksFromWriteOptions) {
    Document doc;
    doc.meta.title = "He said \"hi\"";
    doc.meta.author = "Ada Lovelace";
    doc.meta.page_count = 2;

    doc.blocks.push_back(make_paragraph({StyledRun{U"Page one first"}}, 0));
    doc.blocks.push_back(make_paragraph({StyledRun{U"Page one second"}}, 0));
    doc.blocks.push_back(make_paragraph({StyledRun{U"Page two body"}}, 1));

    WriteOptions opts;
    opts.front_matter = true;
    opts.page_breaks = true;
    opts.source_name = "spec.pdf";

    const std::string expected =
        "---\n"
        "title: \"He said \\\"hi\\\"\"\n"
        "author: \"Ada Lovelace\"\n"
        "source: \"spec.pdf\"\n"
        "pages: 2\n"
        "---\n\n"
        "Page one first\n\n"
        "Page one second\n\n"
        "---\n\n"
        "Page two body\n";
    EXPECT_EQ(pdf2md::write_markdown(doc, opts), expected);
}

StyledRun make_link_run(const char32_t* text, std::string url, bool bold = false) {
    StyledRun r;
    r.text = text;
    r.bold = bold;
    r.link_url = std::move(url);
    return r;
}

StyledRun make_strike_run(const char32_t* text, bool bold = false, bool italic = false) {
    StyledRun r;
    r.text = text;
    r.bold = bold;
    r.italic = italic;
    r.strikethrough = true;
    return r;
}

TEST(MarkdownWriter, FormatsHyperlinksLinkEscapingAndBareProseUrls) {
    struct LinkInlineCase {
        const char* name;
        std::vector<StyledRun> runs;
        bool enable_links;
        const char* expected_md;
    };

    const std::vector<LinkInlineCase> cases = {
        {"basic_link_hugs_text_and_pushes_spaces_outside",
         {StyledRun{U"See"}, make_link_run(U" Specification ", "https://example.org/spec"),
          StyledRun{U"now"}},
         true,
         "See [Specification](https://example.org/spec) now\n"},
        {"closing_bracket_escaped_inside_link_label",
         {make_link_run(U"RFC [2119]", "https://example.org/rfc2119")},
         true,
         "[RFC \\[2119\\]](https://example.org/rfc2119)\n"},
        {"destination_with_space_wrapped_in_angle_brackets",
         {make_link_run(U"Manual", "https://example.org/user guide.pdf")},
         true,
         "[Manual](<https://example.org/user guide.pdf>)\n"},
        {"destination_with_unbalanced_paren_wrapped_in_angle_brackets",
         {make_link_run(U"C++ Reference", "https://en.wikipedia.org/wiki/C++)")},
         true,
         "[C++ Reference](<https://en.wikipedia.org/wiki/C++)>)\n"},
        {"links_disabled_option_suppresses_link_syntax",
         {StyledRun{U"Read "}, make_link_run(U"Specification", "https://example.org/spec")},
         false,
         "Read Specification\n"},
        {"url_shaped_link_label_escapes_tildes_and_underscores_inside_brackets",
         {make_link_run(U"https://example.org/~alice/my_spec.pdf",
                        "https://example.org/~alice/my_spec.pdf")},
         true,
         "[https://example.org/\\~alice/my\\_spec.pdf](https://example.org/~alice/my_spec.pdf)\n"},
        {"bare_prose_url_preserves_underscores_and_tildes_while_surrounding_text_escapes",
         {StyledRun{U"visit https://example.org/~alice/my_spec_v2.pdf for var_name ~approx~"}},
         true,
         "visit https://example.org/~alice/my_spec_v2.pdf for var\\_name \\~approx\\~\n"},
    };

    constexpr std::size_t kExpectedLinkCaseCount = 7;
    ASSERT_EQ(cases.size(), kExpectedLinkCaseCount);
    for (const auto& tc : cases) {
        SCOPED_TRACE(tc.name);
        Document doc;
        doc.blocks.push_back(make_paragraph(tc.runs));
        WriteOptions opts;
        opts.links = tc.enable_links;
        EXPECT_EQ(pdf2md::write_markdown(doc, opts), tc.expected_md);
    }

    // Wrapped link runs across lines merge into one hyperlink when sharing the same URL,
    // while adjacent links with distinct URLs remain separate.
    Document wrapped_doc;
    wrapped_doc.blocks.push_back(make_wrapped_paragraph({
        {make_link_run(U"Developer", "https://example.org/dev-guide")},
        {make_link_run(U"Guide", "https://example.org/dev-guide"), StyledRun{U" and "},
         make_link_run(U"API", "https://example.org/api")},
    }));
    EXPECT_EQ(pdf2md::write_markdown(wrapped_doc, WriteOptions{}),
              "[Developer Guide](https://example.org/dev-guide) and "
              "[API](https://example.org/api)\n");

    // Pipe character inside a table-cell link URL is percent-encoded as %7C.
    Block table;
    table.kind = BlockKind::Table;
    table.table_cells = {
        {TableCell{StyledRun{U"Resource"}}},
        {TableCell{make_link_run(U"Spec", "https://example.org/a|b")}},
    };
    Document table_doc;
    table_doc.blocks.push_back(std::move(table));
    EXPECT_EQ(pdf2md::write_markdown(table_doc, WriteOptions{}),
              "| Resource |\n"
              "| --- |\n"
              "| [Spec](https://example.org/a%7Cb) |\n");
}

TEST(MarkdownWriter, FormatsInlineStrikethroughAndCombinedEmphasis) {
    const std::vector<StyledRunCase> cases = {
        {"plain_strikethrough",
         {make_strike_run(U"deprecated")},
         "~~deprecated~~\n"},
        {"combined_bold_italic_strikethrough",
         {make_strike_run(U"text", true, true)},
         "***~~text~~***\n"},
        {"spaces_kept_outside_strikethrough_delimiters",
         {StyledRun{U"keep"}, make_strike_run(U" old draft "), StyledRun{U"active"}},
         "keep ~~old draft~~ active\n"},
    };

    constexpr std::size_t kExpectedStrikeCaseCount = 3;
    ASSERT_EQ(cases.size(), kExpectedStrikeCaseCount);
    for (const auto& tc : cases) {
        SCOPED_TRACE(tc.name);
        Document doc;
        doc.blocks.push_back(make_paragraph(tc.runs));
        EXPECT_EQ(pdf2md::write_markdown(doc, WriteOptions{}), tc.expected_md);
    }

    // Wrapped strikethrough runs join seamlessly across line breaks.
    Document wrapped_strike;
    wrapped_strike.blocks.push_back(make_wrapped_paragraph({
        {make_strike_run(U"removed")},
        {make_strike_run(U"clause")},
    }));
    EXPECT_EQ(pdf2md::write_markdown(wrapped_strike, WriteOptions{}), "~~removed clause~~\n");
}

TEST(MarkdownWriter, RendersHorizontalRulesImageAltTextAndTableColumnAlignments) {
    Document doc;
    doc.blocks.push_back(make_paragraph({StyledRun{U"Before divider"}}));

    Block hr;
    hr.kind = BlockKind::HorizontalRule;
    doc.blocks.push_back(std::move(hr));

    Block img_with_alt;
    img_with_alt.kind = BlockKind::Image;
    img_with_alt.image_path = "img.png";
    img_with_alt.image_alt = "Figure 1: Architecture [v2]";
    doc.blocks.push_back(std::move(img_with_alt));

    Block img_empty_alt;
    img_empty_alt.kind = BlockKind::Image;
    img_empty_alt.image_path = "img.png";
    doc.blocks.push_back(std::move(img_empty_alt));

    Block aligned_table;
    aligned_table.kind = BlockKind::Table;
    aligned_table.table_align = {
        TableAlign::None,
        TableAlign::Left,
        TableAlign::Center,
        TableAlign::Right,
    };
    aligned_table.table_cells = {
        {TableCell{StyledRun{U"Default"}}, TableCell{StyledRun{U"Left"}},
         TableCell{StyledRun{U"Center"}}, TableCell{StyledRun{U"Right"}}},
        {TableCell{StyledRun{U"a"}}, TableCell{StyledRun{U"b"}},
         TableCell{StyledRun{U"c"}}, TableCell{StyledRun{U"99"}}},
    };
    doc.blocks.push_back(std::move(aligned_table));

    const std::string expected =
        "Before divider\n\n"
        "---\n\n"
        "![Figure 1: Architecture \\[v2\\]](img.png)\n\n"
        "![](img.png)\n\n"
        "| Default | Left | Center | Right |\n"
        "| --- | :--- | :---: | ---: |\n"
        "| a | b | c | 99 |\n";
    EXPECT_EQ(pdf2md::write_markdown(doc, WriteOptions{}), expected);

    // Standalone HorizontalRule produces exact "---\n".
    Document single_hr;
    Block only_hr;
    only_hr.kind = BlockKind::HorizontalRule;
    single_hr.blocks.push_back(std::move(only_hr));
    EXPECT_EQ(pdf2md::write_markdown(single_hr, WriteOptions{}), "---\n");
}

TEST(MarkdownWriter, EmitsTableOfContentsAndRichEscapedYamlFrontMatter) {
    auto make_heading = [](int level, const char32_t* title) {
        Block h;
        h.kind = BlockKind::Heading;
        h.heading_level = level;
        h.size = kHeadingFontSize;
        h.lines.push_back(make_line({StyledRun{title}}));
        return h;
    };

    Document doc;
    doc.meta.title = "Spec\x07Title";
    doc.meta.author = "Ada Lovelace";
    doc.meta.page_count = 3;
    doc.meta.subject = "Architecture\x01Overview";
    doc.meta.keywords = "pdf, markdown";
    doc.meta.creation_date = "2026-01-15T08:30:00Z";
    doc.meta.mod_date = "2026-02-20T14:00:00Z";

    doc.blocks.push_back(make_heading(2, U"Overview"));
    doc.blocks.push_back(make_paragraph({StyledRun{U"Intro text."}}));
    doc.blocks.push_back(make_heading(3, U"Deep Dive"));
    doc.blocks.push_back(make_heading(2, U"Summary"));
    doc.blocks.push_back(make_heading(3, U"Deep Dive"));

    WriteOptions opts;
    opts.front_matter = true;
    opts.toc = true;
    opts.source_name = "manual.pdf";

    const std::string expected =
        "---\n"
        "title: \"Spec\\u0007Title\"\n"
        "author: \"Ada Lovelace\"\n"
        "subject: \"Architecture\\u0001Overview\"\n"
        "keywords: [\"pdf\", \"markdown\"]\n"
        "date: \"2026-01-15T08:30:00Z\"\n"
        "modified: \"2026-02-20T14:00:00Z\"\n"
        "source: \"manual.pdf\"\n"
        "pages: 3\n"
        "---\n\n"
        "- [Overview](#overview)\n"
        "  - [Deep Dive](#deep-dive)\n"
        "- [Summary](#summary)\n"
        "  - [Deep Dive](#deep-dive-1)\n\n"
        "## Overview\n\n"
        "Intro text.\n\n"
        "### Deep Dive\n\n"
        "## Summary\n\n"
        "### Deep Dive\n";
    EXPECT_EQ(pdf2md::write_markdown(doc, opts), expected);

    // Identical creation_date and mod_date omits redundant modified field, and heading-free
    // document with toc = true emits no empty TOC list.
    Document no_heading_doc;
    no_heading_doc.meta.creation_date = "2026-01-15";
    no_heading_doc.meta.mod_date = "2026-01-15";
    no_heading_doc.meta.page_count = 1;
    no_heading_doc.blocks.push_back(make_paragraph({StyledRun{U"Body only."}}));

    WriteOptions same_date_opts;
    same_date_opts.front_matter = true;
    same_date_opts.toc = true;
    EXPECT_EQ(pdf2md::write_markdown(no_heading_doc, same_date_opts),
              "---\n"
              "date: \"2026-01-15\"\n"
              "pages: 1\n"
              "---\n\n"
              "Body only.\n");
}

static_assert(std::is_copy_constructible_v<pdf2md::MarkdownWriter>);
static_assert(std::is_copy_assignable_v<pdf2md::MarkdownWriter>);
static_assert(std::is_nothrow_move_constructible_v<pdf2md::MarkdownWriter> &&
              std::is_nothrow_move_assignable_v<pdf2md::MarkdownWriter>);

TEST(MarkdownWriter, PimplInstanceSupportsCopyMoveAndStatelessMultiDocumentReuse) {
    constexpr int kHeadingH2 = 2;
    constexpr int kPageZero = 0;
    constexpr int kPageOne = 1;
    constexpr int kTwoPages = 2;
    constexpr int kSinglePage = 1;
    constexpr int kTopListIndent = 0;

    auto make_heading_on_page = [](int level, const char32_t* title, int page) {
        Block h;
        h.kind = BlockKind::Heading;
        h.page = page;
        h.heading_level = level;
        h.size = kHeadingFontSize;
        h.lines.push_back(make_line({StyledRun{title}}));
        return h;
    };

    auto make_list_item_on_page = [](const char32_t* marker, bool ordered, const char32_t* text,
                                     int page) {
        Block item;
        item.kind = BlockKind::ListItem;
        item.page = page;
        item.size = kDefaultFontSize;
        item.list_marker = marker;
        item.list_ordered = ordered;
        item.list_indent = kTopListIndent;
        item.lines.push_back(make_line({StyledRun{text}}));
        return item;
    };

    // Document 1 exercises duplicate TOC slugs, hyperlinks, page breaks (page 0 -> 1),
    // unordered list state, and a numbered footnote definition.
    Document doc1;
    doc1.meta.title = "First Spec";
    doc1.meta.author = "Ada Lovelace";
    doc1.meta.page_count = kTwoPages;

    StyledRun fn_ref;
    fn_ref.text = U"[^1]";
    fn_ref.raw = true;

    doc1.blocks.push_back(make_heading_on_page(kHeadingH2, U"Architecture", kPageZero));
    doc1.blocks.push_back(
        make_paragraph({StyledRun{U"Claim "},
                        make_link_run(U"Guide", "https://example.org/one"), fn_ref},
                       kPageZero));
    doc1.blocks.push_back(make_heading_on_page(kHeadingH2, U"Architecture", kPageOne));
    doc1.blocks.push_back(make_list_item_on_page(U"-", false, U"First doc bullet", kPageOne));

    Block numbered_fn;
    numbered_fn.kind = BlockKind::Paragraph;
    numbered_fn.page = kPageOne;
    numbered_fn.size = kFootnoteFontSize;
    numbered_fn.is_footnote = true;
    numbered_fn.footnote_numbered = true;
    numbered_fn.footnote_label = U"1";
    numbered_fn.lines.push_back(make_line({StyledRun{U"First doc note."}}));
    doc1.blocks.push_back(std::move(numbered_fn));

    // Document 2 reuses the same heading title on page 0, starts with an ordered list,
    // and carries no author or footnotes — catching any slug, page-break, list, or
    // footnote bleed from Document 1.
    Document doc2;
    doc2.meta.title = "Second Spec";
    doc2.meta.page_count = kSinglePage;
    doc2.blocks.push_back(make_heading_on_page(kHeadingH2, U"Architecture", kPageZero));
    doc2.blocks.push_back(make_list_item_on_page(U"1.", true, U"Clean start", kPageZero));
    doc2.blocks.push_back(make_paragraph(
        {StyledRun{U"See "}, make_link_run(U"Portal", "https://example.org/two")}, kPageZero));

    WriteOptions rich_opts;
    rich_opts.front_matter = true;
    rich_opts.page_breaks = true;
    rich_opts.toc = true;
    rich_opts.links = true;
    rich_opts.source_name = "batch.pdf";

    const std::string expected_doc1 =
        "---\n"
        "title: \"First Spec\"\n"
        "author: \"Ada Lovelace\"\n"
        "source: \"batch.pdf\"\n"
        "pages: 2\n"
        "---\n\n"
        "- [Architecture](#architecture)\n"
        "- [Architecture](#architecture-1)\n\n"
        "## Architecture\n\n"
        "Claim [Guide](https://example.org/one)[^1]\n\n"
        "---\n\n"
        "## Architecture\n\n"
        "- First doc bullet\n\n"
        "[^1]: First doc note.\n";

    const std::string expected_doc2 =
        "---\n"
        "title: \"Second Spec\"\n"
        "source: \"batch.pdf\"\n"
        "pages: 1\n"
        "---\n\n"
        "- [Architecture](#architecture)\n\n"
        "## Architecture\n\n"
        "1. Clean start\n\n"
        "See [Portal](https://example.org/two)\n";

    const pdf2md::MarkdownWriter rich_writer(rich_opts);
    EXPECT_EQ(rich_writer.write(doc1), expected_doc1);
    EXPECT_EQ(rich_writer.write(doc1), pdf2md::write_markdown(doc1, rich_opts));
    EXPECT_EQ(rich_writer.write(doc2), expected_doc2);
    EXPECT_EQ(rich_writer.write(doc2), pdf2md::write_markdown(doc2, rich_opts));
    EXPECT_EQ(rich_writer.write(doc1), expected_doc1);

    WriteOptions minimal_opts;
    minimal_opts.links = false;
    const std::string expected_plain_doc2 =
        "## Architecture\n\n"
        "1. Clean start\n\n"
        "See Portal\n";

    pdf2md::MarkdownWriter mutable_writer(minimal_opts);
    EXPECT_EQ(mutable_writer.write(doc2), expected_plain_doc2);
    EXPECT_EQ(mutable_writer.write(doc2), pdf2md::write_markdown(doc2, minimal_opts));

    // Copy construction, copy assignment, and self-copy assignment safety.
    const pdf2md::MarkdownWriter copied_writer(rich_writer);
    EXPECT_EQ(copied_writer.write(doc2), expected_doc2);

    mutable_writer = copied_writer;
    const pdf2md::MarkdownWriter& self_alias = mutable_writer;
    mutable_writer = self_alias;
    EXPECT_EQ(mutable_writer.write(doc1), expected_doc1);
    EXPECT_EQ(mutable_writer.write(doc2), expected_doc2);

    // Move construction, move assignment, and reassignment of a moved-from instance.
    pdf2md::MarkdownWriter move_constructed(std::move(mutable_writer));
    EXPECT_EQ(move_constructed.write(doc1), expected_doc1);

    pdf2md::MarkdownWriter move_assigned(minimal_opts);
    move_assigned = std::move(move_constructed);
    EXPECT_EQ(move_assigned.write(doc2), expected_doc2);

    mutable_writer = pdf2md::MarkdownWriter(minimal_opts);
    EXPECT_EQ(mutable_writer.write(doc2), expected_plain_doc2);
}

}  // namespace
