// Component tests for the `pdf2md::Converter` public facade over synthetic PDFs.

#include "converter.h"

#include "fixture_pdfs.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace testfx = pdf2md::testfx;
using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::StartsWith;

// Validates every `![alt](rel_path)` embed in `markdown` by resolving `rel_path`
// under `sandbox_root`, verifying PNG wire structure + chunk CRC-32 checksums,
// and decoding the PNG image stream through libharu's real libpng+zlib consumer.
void verify_all_embedded_pngs(std::string_view markdown, const fs::path& sandbox_root,
                              std::size_t expected_embed_count) {
    const std::vector<std::string> links = testfx::extract_markdown_image_paths(markdown);
    ASSERT_EQ(links.size(), expected_embed_count);
    for (std::size_t i = 0; i < links.size(); ++i) {
        SCOPED_TRACE("Embed index " + std::to_string(i) + ": " + links[i]);
        const fs::path png_path = sandbox_root / fs::path(links[i]);
        ASSERT_TRUE(fs::exists(png_path)) << "Missing referenced PNG: " << png_path.string();

        const testfx::PngWireReport wire = testfx::inspect_png_wire(png_path);
        ASSERT_TRUE(wire.valid) << wire.error;
        EXPECT_GT(wire.width, 0U);
        EXPECT_GT(wire.height, 0U);
        EXPECT_EQ(wire.bit_depth, 8U);
        EXPECT_GT(wire.idat_bytes, 0U);

        const testfx::PngConsumerReport consumer = testfx::consume_png_with_libharu(png_path);
        ASSERT_TRUE(consumer.valid) << consumer.error;
        EXPECT_EQ(consumer.width, wire.width);
        EXPECT_EQ(consumer.height, wire.height);
        EXPECT_EQ(consumer.bits_per_component, static_cast<std::uint32_t>(wire.bit_depth));
    }
}

std::size_t count_occurrences(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty()) {
        return 0U;
    }
    std::size_t count = 0U;
    for (std::size_t pos = haystack.find(needle); pos != std::string_view::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

std::vector<std::string> list_regular_filenames_sorted(const fs::path& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        return names;
    }
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file()) {
            names.push_back(entry.path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

TEST(BasicFixture, DefaultPipelineReconstructsStructureIdempotentlyWithoutMutatingInput) {
    const testfx::ScopedSandbox sandbox("basic_default");
    const fs::path pdf = sandbox.file("basic.pdf");
    testfx::make_basic(pdf);
    const std::vector<std::uint8_t> bytes_before = testfx::read_binary_bytes(pdf);

    const pdf2md::Converter converter(pdf2md::ConvertOptions{});
    const pdf2md::ConvertResult first = converter.convert(pdf);

    EXPECT_EQ(first.page_count, 3U);
    EXPECT_EQ(first.block_count, 19U);

    // Font-size heading hierarchy across pages 1-2.
    EXPECT_THAT(first.markdown, HasSubstr("# Sample Document Title\n"));
    EXPECT_THAT(first.markdown, HasSubstr("## First Section\n"));
    EXPECT_THAT(first.markdown, HasSubstr("## Lists\n"));
    EXPECT_THAT(first.markdown, HasSubstr("## Code Example\n"));
    EXPECT_THAT(first.markdown, HasSubstr("## Hyphenation\n"));
    EXPECT_THAT(first.markdown, HasSubstr("## Bold Pseudo Heading Section\n"));

    // Wrapped lines join into a single paragraph; inline bold/italic runs survive.
    EXPECT_THAT(first.markdown,
                HasSubstr("This is the first paragraph of the sample document. "
                          "It spans multiple lines and should be joined into a single "
                          "markdown paragraph without unwanted line breaks."));
    EXPECT_THAT(first.markdown,
                HasSubstr("The converter must detect **bold text** and also *italic text* "
                          "inside a sentence."));

    // Nested unordered list, ordered list, and fenced monospace code block.
    EXPECT_THAT(first.markdown,
                HasSubstr("- First bullet item\n"
                          "- Second bullet item\n"
                          "    - Nested bullet item\n"
                          "- Third bullet item\n"));
    EXPECT_THAT(first.markdown,
                HasSubstr("1. Alpha step\n"
                          "2. Beta step\n"
                          "3. Gamma step\n"));
    EXPECT_THAT(first.markdown, HasSubstr("```\nint main() {\n    return 42;\n}\n```"));

    // Dehyphenation and cross-page paragraph continuation (page 2 -> page 3).
    EXPECT_THAT(first.markdown,
                HasSubstr("This paragraph exercises the hyphenation repair logic across "
                          "wrapped lines of the same paragraph in the converter."));
    EXPECT_THAT(first.markdown,
                HasSubstr("A paragraph that continues onto the next page must be merged into "
                          "a single paragraph even though it is split across the page "
                          "boundary, which is exactly what this final sentence verifies."));

    // Repeated running header and page numbers are stripped by default.
    EXPECT_THAT(first.markdown, Not(HasSubstr("PDF2MD Fixture Suite")));
    EXPECT_THAT(first.markdown, Not(HasSubstr("Page 1")));
    EXPECT_THAT(first.markdown, Not(HasSubstr("Page 2")));
    EXPECT_THAT(first.markdown, Not(HasSubstr("Page 3")));

    // Idempotence: second invocation on the same Converter produces identical output.
    const pdf2md::ConvertResult second = converter.convert(pdf);
    EXPECT_EQ(second.page_count, first.page_count);
    EXPECT_EQ(second.block_count, first.block_count);
    EXPECT_EQ(second.markdown, first.markdown);

    // Move semantics on Converter facade preserve configured behavior.
    pdf2md::Converter movable{pdf2md::ConvertOptions{}};
    pdf2md::Converter moved(std::move(movable));
    const pdf2md::ConvertResult after_move = moved.convert(pdf);
    EXPECT_EQ(after_move.page_count, first.page_count);
    EXPECT_EQ(after_move.block_count, first.block_count);
    EXPECT_EQ(after_move.markdown, first.markdown);

    // Side-effect hygiene: input PDF bytes are untouched.
    EXPECT_EQ(testfx::read_binary_bytes(pdf), bytes_before);
}

TEST(BasicFixture, InterleavedOptionVariantsControlStagesAcrossDocuments) {
    const testfx::ScopedSandbox sandbox("basic_options");
    const fs::path basic_pdf = sandbox.file("basic.pdf");
    const fs::path twocol_pdf = sandbox.file("twocol.pdf");
    testfx::make_basic(basic_pdf);
    testfx::make_twocol(twocol_pdf);

    // 1. Keeping repeated headers/footers preserves running chrome on all 3 pages.
    pdf2md::ConvertOptions keep_headers_opts;
    keep_headers_opts.analyze.strip_headers_footers = false;
    const pdf2md::Converter keep_headers_conv(keep_headers_opts);
    const pdf2md::ConvertResult kept = keep_headers_conv.convert(basic_pdf);
    EXPECT_EQ(kept.page_count, 3U);
    EXPECT_EQ(kept.block_count, 26U);
    EXPECT_THAT(kept.markdown, HasSubstr("PDF2MD Fixture Suite"));
    EXPECT_THAT(kept.markdown, HasSubstr("Page 2"));

    // 2. Disabling heading inference suppresses `#` markers while preserving text.
    pdf2md::ConvertOptions no_headings_opts;
    no_headings_opts.analyze.detect_headings = false;
    const pdf2md::Converter no_headings_conv(no_headings_opts);
    const pdf2md::ConvertResult flat = no_headings_conv.convert(basic_pdf);
    EXPECT_EQ(flat.page_count, 3U);
    EXPECT_EQ(flat.block_count, 19U);
    EXPECT_THAT(flat.markdown, Not(HasSubstr("# Sample Document Title")));
    EXPECT_THAT(flat.markdown, HasSubstr("Sample Document Title"));

    // 3. Front matter + page breaks across two sequential documents on one Converter
    //    updates document-specific metadata (`title`, `author`, `pages`) without bleed.
    pdf2md::ConvertOptions fm_opts;
    fm_opts.write.front_matter = true;
    fm_opts.write.page_breaks = true;
    fm_opts.write.source_name = "input.pdf";
    const pdf2md::Converter fm_conv(fm_opts);

    const pdf2md::ConvertResult fm_basic = fm_conv.convert(basic_pdf);
    EXPECT_EQ(fm_basic.page_count, 3U);
    EXPECT_EQ(fm_basic.block_count, 19U);
    EXPECT_THAT(fm_basic.markdown,
                StartsWith("---\n"
                           "title: \"Basic Fixture Document\"\n"
                           "author: \"pdf2md tests\"\n"
                           "source: \"input.pdf\"\n"
                           "pages: 3\n"
                           "---\n\n"));
    EXPECT_THAT(fm_basic.markdown, HasSubstr("\n\n---\n\n"));

    const pdf2md::ConvertResult fm_twocol = fm_conv.convert(twocol_pdf);
    EXPECT_EQ(fm_twocol.page_count, 1U);
    EXPECT_EQ(fm_twocol.block_count, 3U);
    EXPECT_THAT(fm_twocol.markdown,
                StartsWith("---\n"
                           "title: \"Two Column Fixture\"\n"
                           "source: \"input.pdf\"\n"
                           "pages: 1\n"
                           "---\n\n"));
    EXPECT_THAT(fm_twocol.markdown, Not(HasSubstr("Basic Fixture Document")));
    EXPECT_THAT(fm_twocol.markdown, Not(HasSubstr("pdf2md tests")));

    // 4. 0-based page subsetting extracts only requested in-range pages.
    pdf2md::ConvertOptions page_filter_opts;
    page_filter_opts.extract.pages = {0, 99};
    const pdf2md::Converter page_filter_conv(page_filter_opts);
    const pdf2md::ConvertResult page_one = page_filter_conv.convert(basic_pdf);
    EXPECT_EQ(page_one.page_count, 1U);
    EXPECT_EQ(page_one.block_count, 16U);
    EXPECT_THAT(page_one.markdown, HasSubstr("Sample Document Title"));
    EXPECT_THAT(page_one.markdown, Not(HasSubstr("Hyphenation")));
    EXPECT_THAT(page_one.markdown, Not(HasSubstr("Final page content")));
}

TEST(BasicFixture, SequentialMultiDocumentBatchIsolatesStateAndCleansUpSandbox) {
    fs::path inner_dir_snapshot;
    {
        const testfx::ScopedSandbox sandbox("sequential_batch");
        inner_dir_snapshot = sandbox.path();
        ASSERT_TRUE(fs::exists(inner_dir_snapshot));

        const fs::path image_pdf = sandbox.file("image.pdf");
        const fs::path basic_pdf = sandbox.file("basic.pdf");
        const fs::path table_pdf = sandbox.file("table.pdf");
        testfx::make_image(image_pdf);
        testfx::make_basic(basic_pdf);
        testfx::make_table(table_pdf);

        const std::vector<std::uint8_t> img_bytes = testfx::read_binary_bytes(image_pdf);
        const std::vector<std::uint8_t> basic_bytes = testfx::read_binary_bytes(basic_pdf);
        const std::vector<std::uint8_t> table_bytes = testfx::read_binary_bytes(table_pdf);

        pdf2md::ConvertOptions opts;
        opts.extract.extract_images = true;
        opts.extract.image_dir = sandbox.file("batch_images");
        opts.extract.image_ref_prefix = "batch_images/";
        const pdf2md::Converter converter(opts);

        // Call #1: image-bearing document produces image_1.png.
        const pdf2md::ConvertResult r1 = converter.convert(image_pdf);
        EXPECT_EQ(r1.page_count, 1U);
        EXPECT_EQ(r1.block_count, 3U);
        verify_all_embedded_pngs(r1.markdown, sandbox.path(), 1U);

        // Call #2: 3-page prose/code/list document on same Converter has zero image bleed.
        const pdf2md::ConvertResult r2 = converter.convert(basic_pdf);
        EXPECT_EQ(r2.page_count, 3U);
        EXPECT_EQ(r2.block_count, 19U);
        EXPECT_TRUE(testfx::extract_markdown_image_paths(r2.markdown).empty());
        EXPECT_THAT(r2.markdown, HasSubstr("# Sample Document Title\n"));

        // Call #3: 1-page table document on same Converter renders table cleanly.
        const pdf2md::ConvertResult r3 = converter.convert(table_pdf);
        EXPECT_EQ(r3.page_count, 1U);
        EXPECT_EQ(r3.block_count, 3U);
        EXPECT_TRUE(testfx::extract_markdown_image_paths(r3.markdown).empty());
        EXPECT_THAT(r3.markdown, HasSubstr("| Apples | 12 | 1.50 |"));

        // Call #4: re-running Call #1 resets per-run image numbering and matches r1.
        const pdf2md::ConvertResult r4 = converter.convert(image_pdf);
        EXPECT_EQ(r4.page_count, r1.page_count);
        EXPECT_EQ(r4.block_count, r1.block_count);
        EXPECT_EQ(r4.markdown, r1.markdown);
        verify_all_embedded_pngs(r4.markdown, sandbox.path(), 1U);

        EXPECT_EQ(testfx::read_binary_bytes(image_pdf), img_bytes);
        EXPECT_EQ(testfx::read_binary_bytes(basic_pdf), basic_bytes);
        EXPECT_EQ(testfx::read_binary_bytes(table_pdf), table_bytes);
    }
    EXPECT_FALSE(fs::exists(inner_dir_snapshot));
}

TEST(BasicFixture, UnicodeLigaturesSurrogatesRotatedTextAndNonAsciiPathSurvive) {
    const testfx::ScopedSandbox sandbox("unicode_seam");
    const fs::path unicode_dir = sandbox.path() / fs::path(u8"dir_caf\u00E9_\u65E5\u672C");
    const fs::path pdf = unicode_dir / fs::path(u8"seam_\u65E5\u672C\u8A9E.pdf");
    testfx::make_unicode_and_rotated(pdf);
    const std::vector<std::uint8_t> bytes_before = testfx::read_binary_bytes(pdf);

    // Default (`keep_rotated = false`): Unicode, ligature expansion, CJK, and
    // astral UTF-16 surrogate pair survive while rotated watermark is skipped.
    const pdf2md::Converter default_conv(pdf2md::ConvertOptions{});
    const pdf2md::ConvertResult res = default_conv.convert(pdf);
    EXPECT_EQ(res.page_count, 1U);
    EXPECT_EQ(res.block_count, 2U);
    EXPECT_THAT(res.markdown, HasSubstr("Unicode Seam Verification"));
    EXPECT_THAT(res.markdown, HasSubstr("Caf\u00E9"));
    EXPECT_THAT(res.markdown, HasSubstr("Stra\u00DFe"));
    EXPECT_THAT(res.markdown, HasSubstr("profile"));
    EXPECT_THAT(res.markdown, HasSubstr("workflow"));
    EXPECT_THAT(res.markdown, HasSubstr("\u00A9\u20AC"));
    EXPECT_THAT(res.markdown, HasSubstr("\u65E5\u672C\u8A9E"));
    EXPECT_THAT(res.markdown, HasSubstr("\U0001D11E"));
    EXPECT_THAT(res.markdown, Not(HasSubstr("ZX9")));

    // `keep_rotated = true`: rotated diagonal stamp is retained as concrete text.
    pdf2md::ConvertOptions rotated_opts;
    rotated_opts.extract.keep_rotated = true;
    const pdf2md::Converter rotated_conv(rotated_opts);
    const pdf2md::ConvertResult with_rot = rotated_conv.convert(pdf);
    EXPECT_EQ(with_rot.page_count, 1U);
    EXPECT_EQ(with_rot.block_count, 3U);
    EXPECT_THAT(with_rot.markdown, HasSubstr("Caf\u00E9"));
    EXPECT_THAT(with_rot.markdown, HasSubstr("\u65E5\u672C\u8A9E"));
    EXPECT_THAT(with_rot.markdown, HasSubstr("ZX9"));

    EXPECT_EQ(testfx::read_binary_bytes(pdf), bytes_before);
}

TEST(TwoColumnFixture, ReadingOrderPreservesTitleThenLeftColumnBeforeRightColumn) {
    const testfx::ScopedSandbox sandbox("twocol");
    const fs::path twocol_pdf = sandbox.file("twocol.pdf");
    const fs::path table_pdf = sandbox.file("table.pdf");
    testfx::make_twocol(twocol_pdf);
    testfx::make_table(table_pdf);
    const std::vector<std::uint8_t> bytes_before = testfx::read_binary_bytes(twocol_pdf);

    const pdf2md::Converter converter(pdf2md::ConvertOptions{});
    const pdf2md::ConvertResult res = converter.convert(twocol_pdf);
    EXPECT_EQ(res.page_count, 1U);
    EXPECT_EQ(res.block_count, 3U);
    EXPECT_THAT(res.markdown, HasSubstr("# Two Column Layout Test"));

    const std::string expected_left =
        "Left column sentence one flows across several short lines and must be read "
        "completely before any right column text appears.";
    const std::string expected_right =
        "Right column sentence one also flows across several lines and must appear "
        "after the whole of the left column in the output.";
    EXPECT_THAT(res.markdown, HasSubstr(expected_left));
    EXPECT_THAT(res.markdown, HasSubstr(expected_right));

    const std::size_t title_pos = res.markdown.find("Two Column Layout Test");
    const std::size_t left_pos = res.markdown.find(expected_left);
    const std::size_t right_pos = res.markdown.find(expected_right);
    ASSERT_NE(title_pos, std::string::npos);
    ASSERT_NE(left_pos, std::string::npos);
    ASSERT_NE(right_pos, std::string::npos);
    EXPECT_LT(title_pos, left_pos);
    EXPECT_LT(left_pos, right_pos);

    // Sequential conversion after another fixture yields identical result.
    const pdf2md::ConvertResult other = converter.convert(table_pdf);
    EXPECT_EQ(other.page_count, 1U);
    EXPECT_EQ(other.block_count, 3U);
    const pdf2md::ConvertResult replay = converter.convert(twocol_pdf);
    EXPECT_EQ(replay.markdown, res.markdown);
    EXPECT_EQ(replay.page_count, res.page_count);
    EXPECT_EQ(replay.block_count, res.block_count);
    EXPECT_EQ(testfx::read_binary_bytes(twocol_pdf), bytes_before);
}

TEST(TableFixture, AlignedTextColumnsBecomeGfmTableAndRespectNoTablesToggle) {
    const testfx::ScopedSandbox sandbox("table_text");
    const fs::path pdf = sandbox.file("table.pdf");
    testfx::make_table(pdf);
    const std::vector<std::uint8_t> bytes_before = testfx::read_binary_bytes(pdf);

    const pdf2md::Converter default_conv(pdf2md::ConvertOptions{});
    const pdf2md::ConvertResult res = default_conv.convert(pdf);
    EXPECT_EQ(res.page_count, 1U);
    EXPECT_EQ(res.block_count, 3U);
    EXPECT_THAT(res.markdown, HasSubstr("# Inventory Report\n"));
    EXPECT_THAT(res.markdown,
                HasSubstr("| Name | Quantity | Price |\n"
                          "| --- | --- | --- |\n"
                          "| Apples | 12 | 1.50 |\n"
                          "| Bananas | 7 | 0.75 |\n"
                          "| Cherries | 31 | 4.20 |"));
    EXPECT_THAT(res.markdown,
                HasSubstr("A regular paragraph after the table should not be absorbed into it."));
    EXPECT_THAT(res.markdown, Not(HasSubstr("| A regular paragraph")));

    pdf2md::ConvertOptions no_tables_opts;
    no_tables_opts.analyze.detect_tables = false;
    const pdf2md::Converter no_tables_conv(no_tables_opts);
    const pdf2md::ConvertResult plain = no_tables_conv.convert(pdf);
    EXPECT_EQ(plain.page_count, 1U);
    EXPECT_EQ(plain.block_count, 3U);
    EXPECT_THAT(plain.markdown, Not(HasSubstr("| Name |")));
    EXPECT_THAT(plain.markdown, Not(HasSubstr("| --- |")));
    EXPECT_THAT(plain.markdown, HasSubstr("Name"));
    EXPECT_THAT(plain.markdown, HasSubstr("Cherries"));

    EXPECT_EQ(testfx::read_binary_bytes(pdf), bytes_before);
}

TEST(TableFixture, MultiPageRuledLatticeTableStitchesContinuationAcrossPages) {
    const testfx::ScopedSandbox sandbox("table_ruled_multipage");
    const fs::path pdf = sandbox.file("ruled_multipage.pdf");
    testfx::make_multipage_ruled_table(pdf);
    const std::vector<std::uint8_t> bytes_before = testfx::read_binary_bytes(pdf);

    const pdf2md::Converter converter(pdf2md::ConvertOptions{});
    const pdf2md::ConvertResult res = converter.convert(pdf);
    EXPECT_EQ(res.page_count, 2U);
    EXPECT_EQ(res.block_count, 4U);
    EXPECT_EQ(count_occurrences(res.markdown, "| --- | --- | --- |"), 1U);
    EXPECT_THAT(res.markdown, HasSubstr("Quarterly Revenue Ledger"));
    EXPECT_THAT(res.markdown,
                HasSubstr("| Region | Quarter | Revenue |\n"
                          "| --- | --- | --- |\n"
                          "| North | Q1 | 120.5 |\n"
                          "| South | Q2 | 98.0 |\n"
                          "| East | Q3 | 145.2 |\n"
                          "| West | Q4 | 163.8 |"));
    EXPECT_THAT(res.markdown,
                HasSubstr("Ledger summary concludes after all four regional rows."));

    const std::size_t table_pos = res.markdown.find("| Region | Quarter | Revenue |");
    const std::size_t footer_pos = res.markdown.find("Ledger summary concludes");
    ASSERT_NE(table_pos, std::string::npos);
    ASSERT_NE(footer_pos, std::string::npos);
    EXPECT_LT(table_pos, footer_pos);

    EXPECT_EQ(testfx::read_binary_bytes(pdf), bytes_before);
}

TEST(ImageFixture, ExtractedPngPassesWireCheckLibharuDecodeAndPdfiumRoundTrip) {
    const testfx::ScopedSandbox sandbox("image_roundtrip");
    const fs::path pdf = sandbox.file("image.pdf");
    testfx::make_image(pdf);
    const std::vector<std::uint8_t> bytes_before = testfx::read_binary_bytes(pdf);

    // Default (`extract_images = false`) omits embeds and writes no image folder.
    const pdf2md::Converter no_img_conv(pdf2md::ConvertOptions{});
    const pdf2md::ConvertResult text_only = no_img_conv.convert(pdf);
    EXPECT_EQ(text_only.page_count, 1U);
    EXPECT_EQ(text_only.block_count, 2U);
    EXPECT_TRUE(testfx::extract_markdown_image_paths(text_only.markdown).empty());
    EXPECT_FALSE(fs::exists(sandbox.file("extracted_images")));

    // Producer (`Converter` with `extract_images = true`) emits PNG on disk.
    pdf2md::ConvertOptions opts;
    opts.extract.extract_images = true;
    opts.extract.image_dir = sandbox.file("extracted_images");
    opts.extract.image_ref_prefix = "extracted_images/";
    const pdf2md::Converter img_conv(opts);
    const pdf2md::ConvertResult res = img_conv.convert(pdf);

    EXPECT_EQ(res.page_count, 1U);
    EXPECT_EQ(res.block_count, 3U);
    EXPECT_THAT(res.markdown, HasSubstr("![](extracted_images/image_1.png)"));

    const std::size_t above = res.markdown.find("A paragraph above the picture.");
    const std::size_t embed = res.markdown.find("![](extracted_images/image_1.png)");
    const std::size_t below = res.markdown.find("A paragraph below the picture.");
    ASSERT_NE(above, std::string::npos);
    ASSERT_NE(embed, std::string::npos);
    ASSERT_NE(below, std::string::npos);
    EXPECT_LT(above, embed);
    EXPECT_LT(embed, below);

    verify_all_embedded_pngs(res.markdown, sandbox.path(), 1U);

    const fs::path emitted_png = sandbox.file("extracted_images/image_1.png");
    const testfx::PngWireReport wire = testfx::inspect_png_wire(emitted_png);
    ASSERT_TRUE(wire.valid) << wire.error;
    EXPECT_EQ(wire.width, 96U);
    EXPECT_EQ(wire.height, 96U);

    // Real Consumer #1 (libharu / libpng+zlib) decodes the PNG and embeds it into
    // a new PDF; Real Consumer #2 (`pdf2md::Converter` / PDFium) extracts it back.
    const fs::path roundtrip_pdf = sandbox.file("roundtrip.pdf");
    const testfx::PngConsumerReport consumer =
        testfx::consume_png_with_libharu(emitted_png, roundtrip_pdf);
    ASSERT_TRUE(consumer.valid) << consumer.error;
    EXPECT_EQ(consumer.width, 96U);
    EXPECT_EQ(consumer.height, 96U);
    EXPECT_EQ(consumer.bits_per_component, 8U);

    pdf2md::ConvertOptions rt_opts;
    rt_opts.extract.extract_images = true;
    rt_opts.extract.image_dir = sandbox.file("roundtrip_images");
    rt_opts.extract.image_ref_prefix = "roundtrip_images/";
    const pdf2md::Converter rt_conv(rt_opts);
    const pdf2md::ConvertResult rt_res = rt_conv.convert(roundtrip_pdf);
    EXPECT_EQ(rt_res.page_count, 1U);
    EXPECT_EQ(rt_res.block_count, 2U);
    EXPECT_THAT(rt_res.markdown, HasSubstr("Roundtrip consumer caption above image."));
    verify_all_embedded_pngs(rt_res.markdown, sandbox.path(), 1U);

    EXPECT_EQ(testfx::read_binary_bytes(pdf), bytes_before);
}

TEST(ImageFixture, DuplicateEmbeddedBitmapsDeduplicateByContentHashAcrossPages) {
    const testfx::ScopedSandbox sandbox("image_dedup");
    const fs::path pdf = sandbox.file("dedup.pdf");
    testfx::make_multi_image_dedup(pdf);

    // Space-containing prefix exercises GFM `<...>` destination wrapping & unwrapping.
    pdf2md::ConvertOptions opts;
    opts.extract.extract_images = true;
    opts.extract.image_dir = sandbox.file("dedup imgs");
    opts.extract.image_ref_prefix = "dedup imgs/";
    const pdf2md::Converter converter(opts);
    const pdf2md::ConvertResult res = converter.convert(pdf);

    EXPECT_EQ(res.page_count, 2U);
    EXPECT_EQ(res.block_count, 5U);
    EXPECT_THAT(res.markdown, HasSubstr("![](<dedup imgs/image_1.png>)"));
    EXPECT_THAT(res.markdown, HasSubstr("![](<dedup imgs/image_2.png>)"));
    verify_all_embedded_pngs(res.markdown, sandbox.path(), 3U);

    const std::vector<std::string> links = testfx::extract_markdown_image_paths(res.markdown);
    ASSERT_EQ(links.size(), 3U);
    EXPECT_EQ(links[0], "dedup imgs/image_1.png");
    EXPECT_EQ(links[1], "dedup imgs/image_2.png");
    EXPECT_EQ(links[2], "dedup imgs/image_1.png");

    const std::vector<std::string> files =
        list_regular_filenames_sorted(sandbox.file("dedup imgs"));
    ASSERT_EQ(files.size(), 2U);
    EXPECT_EQ(files[0], "image_1.png");
    EXPECT_EQ(files[1], "image_2.png");
}

TEST(VectorDiagram, InteriorLabelsSuppressedAndFigureRasterizedToValidPng) {
    const testfx::ScopedSandbox sandbox("vec_diagram");
    const fs::path pdf = sandbox.file("vecdiagram.pdf");
    testfx::make_vector_diagram(pdf, /*diagram=*/true);
    const std::vector<std::uint8_t> bytes_before = testfx::read_binary_bytes(pdf);

    pdf2md::ConvertOptions opts;
    opts.extract.extract_images = true;
    opts.extract.image_dir = sandbox.file("vecdiagram_img");
    opts.extract.image_ref_prefix = "vecdiagram_img/";
    const pdf2md::Converter converter(opts);
    const pdf2md::ConvertResult res = converter.convert(pdf);

    EXPECT_EQ(res.page_count, 1U);
    EXPECT_EQ(res.block_count, 2U);
    EXPECT_THAT(res.markdown, Not(HasSubstr("SECRETLABELWORD")));
    EXPECT_THAT(res.markdown, HasSubstr("DIAGRAMCAPTIONWORD"));
    verify_all_embedded_pngs(res.markdown, sandbox.path(), 1U);
    EXPECT_EQ(testfx::read_binary_bytes(pdf), bytes_before);
}

TEST(VectorDiagram, UnscaledInPageFormKeepsItsText) {
    const testfx::ScopedSandbox sandbox("vec_inpage");
    const fs::path pdf = sandbox.file("inpageform.pdf");
    testfx::make_vector_diagram(pdf, /*diagram=*/false);
    const std::vector<std::uint8_t> bytes_before = testfx::read_binary_bytes(pdf);

    const pdf2md::Converter converter(pdf2md::ConvertOptions{});
    const pdf2md::ConvertResult res = converter.convert(pdf);

    EXPECT_EQ(res.page_count, 1U);
    EXPECT_EQ(res.block_count, 2U);
    EXPECT_THAT(res.markdown, HasSubstr("SECRETLABELWORD"));
    EXPECT_THAT(res.markdown, HasSubstr("DIAGRAMCAPTIONWORD"));
    EXPECT_EQ(testfx::read_binary_bytes(pdf), bytes_before);
}

TEST(VectorDiagram, OverhangingProseSurvivesWhileDiagramLabelsAreDropped) {
    const testfx::ScopedSandbox sandbox("vec_overhang");
    const fs::path pdf = sandbox.file("vecoverhang.pdf");
    testfx::make_vector_diagram(pdf, /*diagram=*/true, /*overhang=*/true);

    pdf2md::ConvertOptions opts;
    opts.extract.extract_images = true;
    opts.extract.image_dir = sandbox.file("vecoverhang_img");
    opts.extract.image_ref_prefix = "vecoverhang_img/";
    const pdf2md::Converter converter(opts);
    const pdf2md::ConvertResult res = converter.convert(pdf);

    EXPECT_EQ(res.page_count, 1U);
    EXPECT_EQ(res.block_count, 3U);
    EXPECT_THAT(res.markdown, HasSubstr("overhanging body sentence must remain visible"));
    EXPECT_THAT(res.markdown, HasSubstr("DIAGRAMCAPTIONWORD"));
    EXPECT_THAT(res.markdown, Not(HasSubstr("SECRETLABELWORD")));
    EXPECT_THAT(res.markdown, Not(HasSubstr("STRAYLABEL")));
    verify_all_embedded_pngs(res.markdown, sandbox.path(), 1U);
}

TEST(VectorDiagram, MultiColumnLabelRowDroppedNotMistakenForOverhungProse) {
    const testfx::ScopedSandbox sandbox("vec_labelrow");
    const fs::path pdf = sandbox.file("veclabelrow.pdf");
    testfx::make_vector_diagram(pdf, /*diagram=*/true, /*overhang=*/true, /*label_row=*/true);

    pdf2md::ConvertOptions opts;
    opts.extract.extract_images = true;
    opts.extract.image_dir = sandbox.file("veclabelrow_img");
    opts.extract.image_ref_prefix = "veclabelrow_img/";
    const pdf2md::Converter converter(opts);
    const pdf2md::ConvertResult res = converter.convert(pdf);

    EXPECT_EQ(res.page_count, 1U);
    EXPECT_EQ(res.block_count, 4U);
    EXPECT_THAT(res.markdown, Not(HasSubstr("LEFTLABELCLUSTERWORD")));
    EXPECT_THAT(res.markdown, Not(HasSubstr("RIGHTLABELCLUSTERWD")));
    EXPECT_THAT(res.markdown, HasSubstr("overhanging body sentence must remain visible"));
    verify_all_embedded_pngs(res.markdown, sandbox.path(), 1U);
}

TEST(ScannedOcr, OcrTextLayerRepairsGlyphSizesMisplacedSpacesAndTightWordGaps) {
    const testfx::ScopedSandbox sandbox("scanned_text");
    const fs::path pdf = sandbox.file("scanned_ocr.pdf");
    testfx::make_scanned_ocr(pdf);
    const std::vector<std::uint8_t> bytes_before = testfx::read_binary_bytes(pdf);

    const pdf2md::Converter converter(pdf2md::ConvertOptions{});
    const pdf2md::ConvertResult res = converter.convert(pdf);

    EXPECT_EQ(res.page_count, 5U);
    EXPECT_EQ(res.block_count, 8U);
    EXPECT_THAT(res.markdown,
                HasSubstr("The scanned optical layer must survive with every word intact and "
                          "join into one readable paragraph of body prose."));
    EXPECT_THAT(res.markdown,
                HasSubstr("Second page copy keeps its own sentences readable too."));
    EXPECT_THAT(res.markdown, HasSubstr("misplaced spaces"));
    EXPECT_THAT(res.markdown, Not(HasSubstr("misplace d")));
    EXPECT_THAT(res.markdown, HasSubstr("alpha beta"));
    EXPECT_THAT(res.markdown, Not(HasSubstr("alphabeta")));

    const pdf2md::ConvertResult replay = converter.convert(pdf);
    EXPECT_EQ(replay.markdown, res.markdown);
    EXPECT_EQ(replay.page_count, res.page_count);
    EXPECT_EQ(replay.block_count, res.block_count);
    EXPECT_EQ(testfx::read_binary_bytes(pdf), bytes_before);
}

TEST(ScannedOcr, PageScansSuppressedWhileFigureCoverAndMinedDiagramBecomeValidPngs) {
    const testfx::ScopedSandbox sandbox("scanned_images");
    const fs::path pdf = sandbox.file("scanned_ocr.pdf");
    testfx::make_scanned_ocr(pdf);

    pdf2md::ConvertOptions opts;
    opts.extract.extract_images = true;
    opts.extract.image_dir = sandbox.file("scanned_imgs");
    opts.extract.image_ref_prefix = "scanned_imgs/";
    const pdf2md::Converter converter(opts);
    const pdf2md::ConvertResult res = converter.convert(pdf);

    EXPECT_EQ(res.page_count, 5U);
    EXPECT_EQ(res.block_count, 11U);
    verify_all_embedded_pngs(res.markdown, sandbox.path(), 3U);

    const std::size_t above =
        res.markdown.find("More prose follows directly beneath the first sentence.");
    const std::size_t below =
        res.markdown.find("Closing sentence beneath the diagram wraps the page.");
    const std::size_t last_embed = res.markdown.rfind("![](scanned_imgs/");
    ASSERT_NE(above, std::string::npos);
    ASSERT_NE(below, std::string::npos);
    ASSERT_NE(last_embed, std::string::npos);
    EXPECT_LT(above, last_embed);
    EXPECT_LT(last_embed, below);
}

TEST(ScannedOcr, RepeatedWatermarkStrippedByDefaultAndPreservedWhenConfigured) {
    const testfx::ScopedSandbox sandbox("scanned_wm");
    const fs::path pdf = sandbox.file("scanned_ocr.pdf");
    testfx::make_scanned_ocr(pdf);

    const pdf2md::Converter stripped_conv(pdf2md::ConvertOptions{});
    const pdf2md::ConvertResult stripped = stripped_conv.convert(pdf);
    EXPECT_EQ(stripped.page_count, 5U);
    EXPECT_EQ(stripped.block_count, 8U);
    EXPECT_THAT(stripped.markdown, Not(HasSubstr("draftmark")));

    pdf2md::ConvertOptions keep_opts;
    keep_opts.analyze.strip_headers_footers = false;
    const pdf2md::Converter keep_conv(keep_opts);
    const pdf2md::ConvertResult kept = keep_conv.convert(pdf);
    EXPECT_EQ(kept.page_count, 5U);
    EXPECT_EQ(kept.block_count, 13U);
    EXPECT_THAT(kept.markdown, HasSubstr("draftmark"));
}

TEST(RunningHeads, PositionAnchoredChapterHeadStrippedWhileRepeatedTableHeaderSurvives) {
    const testfx::ScopedSandbox sandbox("runheads");
    const fs::path pdf = sandbox.file("runheads.pdf");
    testfx::make_runheads(pdf);
    const std::vector<std::uint8_t> bytes_before = testfx::read_binary_bytes(pdf);

    const pdf2md::Converter default_conv(pdf2md::ConvertOptions{});
    const pdf2md::ConvertResult res = default_conv.convert(pdf);
    EXPECT_EQ(res.page_count, 9U);
    EXPECT_EQ(res.block_count, 13U);

    EXPECT_THAT(res.markdown, Not(HasSubstr("GAMMA CHAPTER 7")));
    EXPECT_THAT(res.markdown, HasSubstr("Country"));
    EXPECT_THAT(res.markdown, HasSubstr("Capital"));
    EXPECT_THAT(res.markdown, HasSubstr("Population"));
    EXPECT_THAT(res.markdown, HasSubstr("Paris"));
    EXPECT_THAT(res.markdown, HasSubstr("Tokyo"));
    EXPECT_THAT(res.markdown, HasSubstr("Brasilia"));
    EXPECT_THAT(res.markdown, HasSubstr("Nairobi"));
    EXPECT_THAT(res.markdown, HasSubstr("Opening paragraph about alpha topics"));
    EXPECT_THAT(res.markdown, HasSubstr("Ninth page closes the fixture with plain iota prose."));

    pdf2md::ConvertOptions keep_opts;
    keep_opts.analyze.strip_headers_footers = false;
    const pdf2md::Converter keep_conv(keep_opts);
    const pdf2md::ConvertResult kept = keep_conv.convert(pdf);
    EXPECT_EQ(kept.page_count, 9U);
    EXPECT_EQ(kept.block_count, 17U);
    EXPECT_THAT(kept.markdown, HasSubstr("GAMMA CHAPTER 7"));

    EXPECT_EQ(testfx::read_binary_bytes(pdf), bytes_before);
}

TEST(ErrorHandling, MalformedMissingAndEncryptedInputsThrowPdfErrorCleanly) {
    const testfx::ScopedSandbox sandbox("error_inputs");

    const fs::path missing_pdf = sandbox.file("does_not_exist.pdf");
    const fs::path empty_pdf = sandbox.file("empty.pdf");
    const fs::path garbage_pdf = sandbox.file("garbage.pdf");
    const fs::path truncated_pdf = sandbox.file("truncated.pdf");
    const fs::path encrypted_pdf = sandbox.file("encrypted.pdf");

    testfx::write_binary_bytes(empty_pdf, {});
    const std::vector<std::uint8_t> garbage_bytes = {
        'n', 'o', 't', ' ', 'a', ' ', 'v', 'a', 'l', 'i', 'd', ' ', 'p', 'd', 'f'};
    testfx::write_binary_bytes(garbage_pdf, garbage_bytes);
    const std::vector<std::uint8_t> truncated_bytes = {
        '%', 'P', 'D', 'F', '-', '1', '.', '7', '\n', '1', ' ', '0', ' ', 'o', 'b', 'j'};
    testfx::write_binary_bytes(truncated_pdf, truncated_bytes);
    testfx::make_encrypted(encrypted_pdf, "owner-secret", "reader-secret");
    const std::vector<std::uint8_t> encrypted_bytes = testfx::read_binary_bytes(encrypted_pdf);

    pdf2md::ConvertOptions fault_opts;
    fault_opts.extract.extract_images = true;
    fault_opts.extract.image_dir = sandbox.file("should_stay_empty");
    fault_opts.extract.image_ref_prefix = "should_stay_empty/";
    const pdf2md::Converter converter(fault_opts);

    const std::vector<fs::path> bad_inputs = {
        missing_pdf, empty_pdf, garbage_pdf, truncated_pdf, encrypted_pdf};
    for (const fs::path& bad : bad_inputs) {
        SCOPED_TRACE(bad.filename().string());
        EXPECT_THROW((void)converter.convert(bad), pdf2md::PdfError);
        EXPECT_TRUE(list_regular_filenames_sorted(fault_opts.extract.image_dir).empty());
    }

    // Wrong password also throws PdfError without emitting partial files.
    pdf2md::ConvertOptions wrong_pw_opts = fault_opts;
    wrong_pw_opts.extract.password = "wrong-password";
    const pdf2md::Converter wrong_pw_conv(wrong_pw_opts);
    EXPECT_THROW((void)wrong_pw_conv.convert(encrypted_pdf), pdf2md::PdfError);
    EXPECT_TRUE(list_regular_filenames_sorted(fault_opts.extract.image_dir).empty());

    // Correct user password succeeds and decrypts full content.
    pdf2md::ConvertOptions ok_pw_opts;
    ok_pw_opts.extract.password = "reader-secret";
    const pdf2md::Converter ok_pw_conv(ok_pw_opts);
    const pdf2md::ConvertResult decrypted = ok_pw_conv.convert(encrypted_pdf);
    EXPECT_EQ(decrypted.page_count, 1U);
    EXPECT_EQ(decrypted.block_count, 2U);
    EXPECT_THAT(decrypted.markdown, HasSubstr("# Confidential Briefing"));
    EXPECT_THAT(decrypted.markdown,
                HasSubstr("Authorized eyes only payload verified through password decryption."));

    // All malformed/encrypted input files remain byte-for-byte untouched.
    EXPECT_TRUE(testfx::read_binary_bytes(empty_pdf).empty());
    EXPECT_EQ(testfx::read_binary_bytes(garbage_pdf), garbage_bytes);
    EXPECT_EQ(testfx::read_binary_bytes(truncated_pdf), truncated_bytes);
    EXPECT_EQ(testfx::read_binary_bytes(encrypted_pdf), encrypted_bytes);
}

TEST(ErrorHandling, MidBatchFaultIsolatesPriorArtifactsWritesZeroPartialPngsAndRecovers) {
    const testfx::ScopedSandbox sandbox("mid_batch_fault");
    const fs::path valid_image_pdf = sandbox.file("valid_1_image.pdf");
    const fs::path corrupt_pdf = sandbox.file("corrupt_mid_batch.pdf");
    const fs::path missing_pdf = sandbox.file("missing_mid_batch.pdf");
    const fs::path valid_basic_pdf = sandbox.file("valid_2_basic.pdf");
    const fs::path valid_table_pdf = sandbox.file("valid_3_table.pdf");

    testfx::make_image(valid_image_pdf);
    testfx::write_binary_bytes(corrupt_pdf, {'%', 'P', 'D', 'F', '-', 'b', 'r', 'o', 'k', 'e', 'n'});
    testfx::make_basic(valid_basic_pdf);
    testfx::make_table(valid_table_pdf);

    const fs::path img_dir = sandbox.file("batch_out_images");
    pdf2md::ConvertOptions opts;
    opts.extract.extract_images = true;
    opts.extract.image_dir = img_dir;
    opts.extract.image_ref_prefix = "batch_out_images/";
    const pdf2md::Converter converter(opts);

    // Item #1 succeeds and writes valid `image_1.png`.
    const pdf2md::ConvertResult res1 = converter.convert(valid_image_pdf);
    EXPECT_EQ(res1.page_count, 1U);
    EXPECT_EQ(res1.block_count, 3U);
    verify_all_embedded_pngs(res1.markdown, sandbox.path(), 1U);
    const fs::path first_png = img_dir / "image_1.png";
    const std::vector<std::uint8_t> first_png_bytes = testfx::read_binary_bytes(first_png);

    // Item #2a (corrupt PDF) and Item #2b (missing PDF) throw `PdfError`, write
    // zero new/partial files, and do not corrupt Item #1's already-committed PNG.
    EXPECT_THROW((void)converter.convert(corrupt_pdf), pdf2md::PdfError);
    EXPECT_THROW((void)converter.convert(missing_pdf), pdf2md::PdfError);

    const std::vector<std::string> after_fault_files = list_regular_filenames_sorted(img_dir);
    ASSERT_EQ(after_fault_files.size(), 1U);
    EXPECT_EQ(after_fault_files[0], "image_1.png");
    EXPECT_EQ(testfx::read_binary_bytes(first_png), first_png_bytes);

    // Items #3 and #4 on the SAME `Converter` instance succeed cleanly after faults.
    const pdf2md::ConvertResult res3 = converter.convert(valid_basic_pdf);
    EXPECT_EQ(res3.page_count, 3U);
    EXPECT_EQ(res3.block_count, 19U);
    EXPECT_THAT(res3.markdown, HasSubstr("# Sample Document Title\n"));
    EXPECT_THAT(res3.markdown, HasSubstr("```\nint main() {\n    return 42;\n}\n```"));

    const pdf2md::ConvertResult res4 = converter.convert(valid_table_pdf);
    EXPECT_EQ(res4.page_count, 1U);
    EXPECT_EQ(res4.block_count, 3U);
    EXPECT_THAT(res4.markdown, HasSubstr("| Cherries | 31 | 4.20 |"));
}

}  // namespace
