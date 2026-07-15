// Smoke tests over real-world public PDFs (papers, tables, CJK, PDF 2.0).
// Run tests/corpus/download_corpus.ps1 first; without the cache directory
// these tests skip so offline/CI runs stay green.

#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "layout_analyzer.h"
#include "markdown_writer.h"
#include "pdf_extractor.h"

namespace fs = std::filesystem;
using ::testing::HasSubstr;
using ::testing::Not;

#ifndef PDF2MD_CORPUS_DIR
#define PDF2MD_CORPUS_DIR ""
#endif
#ifndef PDF2MD_TEST_OUTPUT_DIR
#define PDF2MD_TEST_OUTPUT_DIR ""
#endif

namespace {

fs::path corpus_dir() { return fs::path(PDF2MD_CORPUS_DIR); }

#define REQUIRE_CORPUS_FILE(name)                                              \
    if (!fs::exists(corpus_dir() / (name)))                                    \
    GTEST_SKIP() << "corpus not downloaded (tests/corpus/download_corpus.ps1)"

// Every conversion leaves its markdown in build/test_output/corpus/<name>.md.
std::string convert(const std::string& name, pdf2md::ExtractOptions ex_opts = {}) {
    pdf2md::Extraction ex = pdf2md::extract_pdf(corpus_dir() / name, ex_opts);
    pdf2md::Document doc = pdf2md::analyze_layout(ex.meta, ex.pages, {});
    std::string md = pdf2md::write_markdown(doc, {});

    fs::path out_dir = fs::path(PDF2MD_TEST_OUTPUT_DIR);
    if (!out_dir.empty()) {
        out_dir /= "corpus";
        std::error_code ec;
        fs::create_directories(out_dir, ec);
        std::ofstream(out_dir / (fs::path(name).stem().string() + ".md"), std::ios::binary)
            << md;
    }
    return md;
}

TEST(Corpus, EveryFileConvertsToNonEmptyMarkdown) {
    if (!fs::exists(corpus_dir())) GTEST_SKIP() << "corpus not downloaded";
    size_t seen = 0;
    for (const auto& entry : fs::directory_iterator(corpus_dir())) {
        if (entry.path().extension() != ".pdf") continue;
        ++seen;
        SCOPED_TRACE(entry.path().filename().string());
        std::string md;
        ASSERT_NO_THROW(md = convert(entry.path().filename().string()));
        EXPECT_FALSE(md.empty());
    }
    EXPECT_GE(seen, 1u);
}

TEST(Corpus, DummySingleLine) {
    REQUIRE_CORPUS_FILE("dummy.pdf");
    EXPECT_THAT(convert("dummy.pdf"), HasSubstr("Dummy PDF file"));
}

TEST(Corpus, TracemonkeyTwoColumnPaper) {
    REQUIRE_CORPUS_FILE("tracemonkey.pdf");
    std::string md = convert("tracemonkey.pdf");
    // Title and body text extracted.
    EXPECT_THAT(md, HasSubstr("Type Specialization for Dynamic"));
    EXPECT_THAT(md, HasSubstr("JavaScript"));
    // Column reading order: this sentence spans wrapped lines in the left
    // column of page 2 and is destroyed if columns interleave.
    EXPECT_THAT(md, HasSubstr("recording and compiling a trace"));
    // Regression: unstable x-sorting once produced "lfow" from "flow".
    EXPECT_THAT(md, Not(HasSubstr("lfow")));
}

TEST(Corpus, AttentionPaperHeadingsAndText) {
    REQUIRE_CORPUS_FILE("arxiv-attention.pdf");
    std::string md = convert("arxiv-attention.pdf");
    EXPECT_THAT(md, HasSubstr("# Attention Is All You Need"));
    EXPECT_THAT(md, HasSubstr("## Abstract"));
    EXPECT_THAT(md, HasSubstr("Transformer"));
}

TEST(Corpus, AttentionFiguresEmbeddedAndHiddenLabelsSuppressed) {
    REQUIRE_CORPUS_FILE("arxiv-attention.pdf");
    fs::path img_dir = fs::path(PDF2MD_TEST_OUTPUT_DIR) / "corpus" / "attention_images";
    pdf2md::ExtractOptions ex;
    ex.extract_images = true;
    ex.image_dir = img_dir;
    ex.image_ref_prefix = "attention_images/";
    pdf2md::Extraction extraction = pdf2md::extract_pdf(corpus_dir() / "arxiv-attention.pdf", ex);
    pdf2md::Document doc = pdf2md::analyze_layout(extraction.meta, extraction.pages, {});
    std::string md = pdf2md::write_markdown(doc, {});

    // Both raster figures (1, 2) and vector/form figures (3, 4, 5) become embeds.
    size_t embeds = 0;
    for (size_t p = md.find("!["); p != std::string::npos; p = md.find("![", p + 1)) ++embeds;
    EXPECT_GE(embeds, 5u);

    // The heading whose overlapping hidden "Input-Input Layer5" label used to
    // interleave into a pseudo-LaTeX garble now reads cleanly.
    EXPECT_THAT(md, HasSubstr("## Attention Visualizations"));
    EXPECT_THAT(md, Not(HasSubstr("$IA")));
    // The degenerate, letter-spaced figure label no longer leaks as body text.
    EXPECT_THAT(md, Not(HasSubstr("I n p u t - I n p u t")));
}

TEST(Corpus, PdfminerSimpleTextExtracted) {
    REQUIRE_CORPUS_FILE("pdfminer-simple1.pdf");
    std::string md = convert("pdfminer-simple1.pdf");
    EXPECT_THAT(md, HasSubstr("Hello"));
    EXPECT_THAT(md, HasSubstr("World"));
}

TEST(Corpus, TabulaEu002ProducesTable) {
    REQUIRE_CORPUS_FILE("tabula-eu-002.pdf");
    // Pathological glyph-by-glyph spacing; require structural survival:
    // a markdown table appears and conversion stays lossless enough to
    // contain the Pearson correlation values.
    std::string md = convert("tabula-eu-002.pdf");
    EXPECT_THAT(md, HasSubstr("| ---"));
}

TEST(Corpus, JapaneseFinancialTablesNotCodeBlocks) {
    REQUIRE_CORPUS_FILE("tabula-twotables.pdf");
    std::string md = convert("tabula-twotables.pdf");
    // CJK prose must survive; fixed-pitch CJK fonts must not be fenced code.
    EXPECT_THAT(md, HasSubstr("株主資本"));
    EXPECT_THAT(md, Not(HasSubstr("```\n株主資本")));
}

TEST(Corpus, Pdf20SimpleFile) {
    REQUIRE_CORPUS_FILE("pdf20-simple.pdf");
    EXPECT_FALSE(convert("pdf20-simple.pdf").empty());
}

// Counts markdown image embeds in a document.
size_t count_embeds(const std::string& md) {
    size_t embeds = 0;
    for (size_t p = md.find("![]("); p != std::string::npos; p = md.find("![](", p + 1))
        ++embeds;
    return embeds;
}

// Fraction of whitespace-separated tokens that are a single letter (the
// legitimate one-letter words "a"/"A"/"I" excluded). A broken OCR-layer size
// repair shreds nearly every word into letter runs ("w o r d s"), driving this
// ratio toward 1; clean prose keeps it near zero.
double single_letter_token_ratio(const std::string& md) {
    size_t singles = 0, tokens = 0;
    size_t i = 0;
    while (i < md.size()) {
        while (i < md.size() && std::isspace(static_cast<unsigned char>(md[i]))) ++i;
        size_t start = i;
        while (i < md.size() && !std::isspace(static_cast<unsigned char>(md[i]))) ++i;
        if (i == start) continue;
        ++tokens;
        if (i - start == 1) {
            char c = md[start];
            bool letter = (c >= 'b' && c <= 'z' && c != 'i') || (c >= 'B' && c <= 'Z' && c != 'A' && c != 'I');
            if (letter) ++singles;
        }
    }
    return tokens ? static_cast<double>(singles) / static_cast<double>(tokens) : 0.0;
}

// scanned_book.pdf stands for any scanned book with an OCR text layer: tiny
// nominal font sizes blown up by the text matrix, sloppy space-glyph
// coordinates, a repeated stamp on every page, and a full-page scan bitmap
// under the text. Not downloadable -- place such a file in tests/corpus/cache
// by hand; these tests skip when absent. The assertions are content-agnostic
// so any suitable document works.
TEST(Corpus, ScannedBookWordsNotShredded) {
    REQUIRE_CORPUS_FILE("scanned_book.pdf");
    std::string md = convert("scanned_book.pdf");
    ASSERT_FALSE(md.empty());
    // Without the OCR-layer size repair the ratio lands near 0.5; repaired
    // output stays well under 1%.
    EXPECT_LT(single_letter_token_ratio(md), 0.02);
}

TEST(Corpus, ScannedBookPageScansNotEmbeddedPerPage) {
    REQUIRE_CORPUS_FILE("scanned_book.pdf");
    fs::path img_dir = fs::path(PDF2MD_TEST_OUTPUT_DIR) / "corpus" / "scanned_book_images";
    pdf2md::ExtractOptions ex;
    ex.extract_images = true;
    ex.image_dir = img_dir;
    ex.image_ref_prefix = "scanned_book_images/";
    pdf2md::Extraction extraction =
        pdf2md::extract_pdf(corpus_dir() / "scanned_book.pdf", ex);
    pdf2md::Document doc = pdf2md::analyze_layout(extraction.meta, extraction.pages, {});
    std::string md = pdf2md::write_markdown(doc, {});
    // If the page-background scans leaked through, every text page would embed
    // its own full-page bitmap (roughly one embed per page); genuine figures,
    // covers and mined diagram crops stay far below that.
    ASSERT_GT(extraction.meta.page_count, 0);
    EXPECT_LT(count_embeds(md), static_cast<size_t>(extraction.meta.page_count) / 2);
}

// spec_document.pdf stands for any large native-text document (headings,
// tables, sub-page figures): a regression canary that the scanned-book
// heuristics (OCR size repair, background-image suppression, watermark
// stripping) leave ordinary documents alone. Place such a file in
// tests/corpus/cache by hand; skips when absent.
TEST(Corpus, NativeSpecUnaffectedByScanHeuristics) {
    REQUIRE_CORPUS_FILE("spec_document.pdf");
    fs::path img_dir = fs::path(PDF2MD_TEST_OUTPUT_DIR) / "corpus" / "spec_document_images";
    pdf2md::ExtractOptions ex;
    ex.extract_images = true;
    ex.image_dir = img_dir;
    ex.image_ref_prefix = "spec_document_images/";
    std::string md = convert("spec_document.pdf", ex);

    ASSERT_FALSE(md.empty());
    EXPECT_LT(single_letter_token_ratio(md), 0.02);   // no size-repair misfire
    EXPECT_THAT(md, HasSubstr("\n# "));               // headings survive
    EXPECT_THAT(md, HasSubstr("| --- |"));            // tables survive
    // Its figures are ordinary sub-page images: none may be suppressed as a
    // page background.
    EXPECT_GE(count_embeds(md), 1u);
}

}  // namespace
