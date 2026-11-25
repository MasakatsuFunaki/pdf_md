// Component tests driving `pdf2md::Converter` over real-world public corpus PDFs.
// Run `python build.py --with-corpus --test` (or `pwsh tests/corpus/download_corpus.ps1`)
// to populate the cache directory; missing optional cache files skip with exact path
// diagnostics so offline runs stay transparent.

#include "converter.h"

#include "fixture_pdfs.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#ifndef PDF2MD_CORPUS_DIR
#define PDF2MD_CORPUS_DIR ""
#endif

namespace {

namespace fs = std::filesystem;
namespace testfx = pdf2md::testfx;
using ::testing::HasSubstr;
using ::testing::Not;

constexpr double kMaxSingleLetterTokenRatio = 0.02;
constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct FileFingerprint {
    std::size_t size = 0;
    std::uint64_t fnv1a = kFnvOffsetBasis;

    bool operator==(const FileFingerprint& other) const noexcept {
        return size == other.size && fnv1a == other.fnv1a;
    }
};

fs::path corpus_file(std::string_view filename) {
    return fs::path(PDF2MD_CORPUS_DIR) / fs::path(filename);
}

FileFingerprint fingerprint_file(const fs::path& path) {
    const std::vector<std::uint8_t> bytes = testfx::read_binary_bytes(path);
    FileFingerprint fp;
    fp.size = bytes.size();
    for (std::uint8_t b : bytes) {
        fp.fnv1a ^= static_cast<std::uint64_t>(b);
        fp.fnv1a *= kFnvPrime;
    }
    return fp;
}

void verify_corpus_png_embeds(std::string_view markdown, const fs::path& sandbox_root,
                              std::size_t min_expected_embeds) {
    const std::vector<std::string> links = testfx::extract_markdown_image_paths(markdown);
    ASSERT_GE(links.size(), min_expected_embeds);
    for (std::size_t i = 0; i < links.size(); ++i) {
        SCOPED_TRACE("Corpus PNG embed " + std::to_string(i) + ": " + links[i]);
        const fs::path png_path = sandbox_root / fs::path(links[i]);
        ASSERT_TRUE(fs::exists(png_path)) << "Missing corpus PNG: " << png_path.string();

        const testfx::PngWireReport wire = testfx::inspect_png_wire(png_path);
        ASSERT_TRUE(wire.valid) << wire.error;
        EXPECT_GT(wire.width, 0U);
        EXPECT_GT(wire.height, 0U);
        EXPECT_EQ(wire.bit_depth, 8U);

        const testfx::PngConsumerReport consumer = testfx::consume_png_with_libharu(png_path);
        ASSERT_TRUE(consumer.valid) << consumer.error;
        EXPECT_EQ(consumer.width, wire.width);
        EXPECT_EQ(consumer.height, wire.height);
    }
}

// Fraction of whitespace-separated tokens that are an unexpected single ASCII
// letter (excluding legitimate words "a", "A", "I", and roman/index "i").
double single_letter_token_ratio(std::string_view md) noexcept {
    std::size_t singles = 0;
    std::size_t tokens = 0;
    std::size_t i = 0;
    while (i < md.size()) {
        while (i < md.size() && std::isspace(static_cast<unsigned char>(md[i])) != 0) {
            ++i;
        }
        const std::size_t start = i;
        while (i < md.size() && std::isspace(static_cast<unsigned char>(md[i])) == 0) {
            ++i;
        }
        if (i == start) {
            continue;
        }
        ++tokens;
        if (i - start == 1U) {
            const char c = md[start];
            const bool unexpected_letter =
                (c >= 'b' && c <= 'z' && c != 'i') ||
                (c >= 'B' && c <= 'Z' && c != 'A' && c != 'I');
            if (unexpected_letter) {
                ++singles;
            }
        }
    }
    return tokens != 0U ? static_cast<double>(singles) / static_cast<double>(tokens) : 0.0;
}

TEST(Corpus, DummySingleLineHeadingAndCounts) {
    const fs::path pdf = corpus_file("dummy.pdf");
    if (!fs::exists(pdf)) {
        GTEST_SKIP() << "Missing corpus cache file: " << pdf.string();
    }
    const std::vector<std::uint8_t> before = testfx::read_binary_bytes(pdf);

    const pdf2md::Converter converter(pdf2md::ConvertOptions{});
    const pdf2md::ConvertResult res = converter.convert(pdf);
    EXPECT_EQ(res.page_count, 1U);
    EXPECT_EQ(res.block_count, 1U);
    EXPECT_EQ(res.markdown, "## Dummy PDF file\n");
    EXPECT_EQ(testfx::read_binary_bytes(pdf), before);
}

TEST(Corpus, Pdf20SimpleFileExtractsGreetingAndSingleBlock) {
    const fs::path pdf = corpus_file("pdf20-simple.pdf");
    if (!fs::exists(pdf)) {
        GTEST_SKIP() << "Missing corpus cache file: " << pdf.string();
    }
    const std::vector<std::uint8_t> before = testfx::read_binary_bytes(pdf);

    const pdf2md::Converter converter(pdf2md::ConvertOptions{});
    const pdf2md::ConvertResult res = converter.convert(pdf);
    EXPECT_EQ(res.page_count, 1U);
    EXPECT_EQ(res.block_count, 1U);
    EXPECT_EQ(res.markdown, "Hello World\n");
    EXPECT_EQ(testfx::read_binary_bytes(pdf), before);
}

TEST(Corpus, PdfminerPositionedGlyphsAndHiraganaSamples) {
    const fs::path simple1 = corpus_file("pdfminer-simple1.pdf");
    if (!fs::exists(simple1)) {
        GTEST_SKIP() << "Missing corpus cache file: " << simple1.string();
    }
    const fs::path simple3 = corpus_file("pdfminer-simple3.pdf");
    if (!fs::exists(simple3)) {
        GTEST_SKIP() << "Missing corpus cache file: " << simple3.string();
    }
    const std::vector<std::uint8_t> before1 = testfx::read_binary_bytes(simple1);
    const std::vector<std::uint8_t> before3 = testfx::read_binary_bytes(simple3);

    const pdf2md::Converter converter(pdf2md::ConvertOptions{});

    const pdf2md::ConvertResult res1 = converter.convert(simple1);
    EXPECT_EQ(res1.page_count, 1U);
    EXPECT_EQ(res1.block_count, 8U);
    EXPECT_THAT(res1.markdown, HasSubstr("Hello"));
    EXPECT_THAT(res1.markdown, HasSubstr("World"));
    EXPECT_THAT(res1.markdown, HasSubstr("H e l l o"));
    EXPECT_THAT(res1.markdown, HasSubstr("W o r l d"));

    const pdf2md::ConvertResult res3 = converter.convert(simple3);
    EXPECT_EQ(res3.page_count, 1U);
    EXPECT_EQ(res3.block_count, 2U);
    EXPECT_THAT(res3.markdown, HasSubstr("HelloHello"));
    EXPECT_THAT(res3.markdown, HasSubstr("\u3042 \u3044 \u3046 \u3048"));
    EXPECT_THAT(res3.markdown, HasSubstr("World World"));

    EXPECT_EQ(testfx::read_binary_bytes(simple1), before1);
    EXPECT_EQ(testfx::read_binary_bytes(simple3), before3);
}

TEST(Corpus, TabulaEu002ProducesTablesWithPearsonCorrelationValues) {
    const fs::path pdf = corpus_file("tabula-eu-002.pdf");
    if (!fs::exists(pdf)) {
        GTEST_SKIP() << "Missing corpus cache file: " << pdf.string();
    }
    const std::vector<std::uint8_t> before = testfx::read_binary_bytes(pdf);

    const pdf2md::Converter converter(pdf2md::ConvertOptions{});
    const pdf2md::ConvertResult res = converter.convert(pdf);
    EXPECT_EQ(res.page_count, 2U);
    EXPECT_EQ(res.block_count, 23U);
    EXPECT_THAT(res.markdown, HasSubstr("Pearsons correlation coefficient"));
    EXPECT_THAT(res.markdown,
                HasSubstr("| Knowledge and awareness of different cultures | 0,2885 | 0,3974 | 0,3904 |"));
    EXPECT_THAT(res.markdown,
                HasSubstr("| Foreign language competence | 0,3057 | 0,4184 | 0,3899 |"));
    EXPECT_THAT(res.markdown,
                HasSubstr("| Self competence | 0,3791 | 0,3320 | 0,4617 |"));
    EXPECT_THAT(res.markdown,
                HasSubstr("| Knowledge and awareness of different cultures | -0,1651 | -0,2742 | -0,1618 |"));
    EXPECT_THAT(res.markdown,
                HasSubstr("| School climate | -0,2976 | -0,1810 | -0,1012 |"));
    EXPECT_THAT(res.markdown,
                HasSubstr("Overall satisfaction with the outcomes and impacts of the Comenius project"));

    EXPECT_EQ(testfx::read_binary_bytes(pdf), before);
}

TEST(Corpus, JapaneseFinancialTablesPreserveCjkProseAndGridFigures) {
    const fs::path pdf = corpus_file("tabula-twotables.pdf");
    if (!fs::exists(pdf)) {
        GTEST_SKIP() << "Missing corpus cache file: " << pdf.string();
    }
    const std::vector<std::uint8_t> before = testfx::read_binary_bytes(pdf);

    const pdf2md::Converter converter(pdf2md::ConvertOptions{});
    const pdf2md::ConvertResult res = converter.convert(pdf);
    EXPECT_EQ(res.page_count, 1U);
    EXPECT_EQ(res.block_count, 34U);
    EXPECT_THAT(res.markdown, HasSubstr("\u9023\u7D50\u682A\u4E3B\u8CC7\u672C\u7B49\u5909\u52D5\u8A08\u7B97\u66F8"));
    EXPECT_THAT(res.markdown, HasSubstr("\u682A\u4E3B\u8CC7\u672C"));
    EXPECT_THAT(res.markdown, Not(HasSubstr("```\n\u682A\u4E3B\u8CC7\u672C")));
    EXPECT_THAT(res.markdown, HasSubstr("| \u5F53\u671F\u5909\u52D5\u984D\u5408\u8A08 | 556 | 80 | 5 |"));
    EXPECT_THAT(res.markdown, HasSubstr("18,683"));

    EXPECT_EQ(testfx::read_binary_bytes(pdf), before);
}

TEST(Corpus, TracemonkeyTwoColumnPaperPreservesColumnOrderAndTitle) {
    const fs::path pdf = corpus_file("tracemonkey.pdf");
    if (!fs::exists(pdf)) {
        GTEST_SKIP() << "Missing corpus cache file: " << pdf.string();
    }
    const FileFingerprint before = fingerprint_file(pdf);

    const pdf2md::Converter converter(pdf2md::ConvertOptions{});
    const pdf2md::ConvertResult res = converter.convert(pdf);
    EXPECT_EQ(res.page_count, 14U);
    EXPECT_GE(res.block_count, 250U);
    EXPECT_THAT(res.markdown,
                HasSubstr("# Trace-based Just-in-Time Type Specialization for Dynamic Languages"));
    EXPECT_THAT(res.markdown, HasSubstr("## Abstract"));
    EXPECT_THAT(res.markdown, HasSubstr("SpiderMonkey"));
    EXPECT_THAT(res.markdown, HasSubstr("TraceMonkey"));
    EXPECT_THAT(res.markdown, HasSubstr("recording and compiling a trace"));
    EXPECT_THAT(res.markdown, Not(HasSubstr("lfow")));

    EXPECT_EQ(fingerprint_file(pdf), before);
}

TEST(Corpus, AttentionPaperHeadingsMathAndTables) {
    const fs::path pdf = corpus_file("arxiv-attention.pdf");
    if (!fs::exists(pdf)) {
        GTEST_SKIP() << "Missing corpus cache file: " << pdf.string();
    }
    const FileFingerprint before = fingerprint_file(pdf);

    const pdf2md::Converter converter(pdf2md::ConvertOptions{});
    const pdf2md::ConvertResult res = converter.convert(pdf);
    EXPECT_EQ(res.page_count, 15U);
    EXPECT_GE(res.block_count, 150U);
    EXPECT_THAT(res.markdown, HasSubstr("# Attention Is All You Need"));
    EXPECT_THAT(res.markdown, HasSubstr("## Abstract"));
    EXPECT_THAT(res.markdown, HasSubstr("## 1 Introduction"));
    EXPECT_THAT(res.markdown, HasSubstr("## 3 Model Architecture"));
    EXPECT_THAT(res.markdown, HasSubstr("Transformer"));
    EXPECT_THAT(res.markdown, HasSubstr("28.4 BLEU"));
    EXPECT_THAT(res.markdown, HasSubstr("\\sqrt{d_k}"));

    EXPECT_EQ(fingerprint_file(pdf), before);
}

TEST(Corpus, AttentionFiguresEmbeddedAsValidPngsAndHiddenLabelsSuppressed) {
    const fs::path pdf = corpus_file("arxiv-attention.pdf");
    if (!fs::exists(pdf)) {
        GTEST_SKIP() << "Missing corpus cache file: " << pdf.string();
    }
    const FileFingerprint before = fingerprint_file(pdf);
    const testfx::ScopedSandbox sandbox("corpus_attention_imgs");

    pdf2md::ConvertOptions opts;
    opts.extract.extract_images = true;
    opts.extract.image_dir = sandbox.file("attention_images");
    opts.extract.image_ref_prefix = "attention_images/";
    const pdf2md::Converter converter(opts);
    const pdf2md::ConvertResult res = converter.convert(pdf);

    EXPECT_EQ(res.page_count, 15U);
    EXPECT_GE(res.block_count, 160U);
    verify_corpus_png_embeds(res.markdown, sandbox.path(), 5U);
    EXPECT_THAT(res.markdown, HasSubstr("## Attention Visualizations"));
    EXPECT_THAT(res.markdown, Not(HasSubstr("$IA")));
    EXPECT_THAT(res.markdown, Not(HasSubstr("I n p u t - I n p u t")));

    EXPECT_EQ(fingerprint_file(pdf), before);
}

TEST(Corpus, SequentialBatchAcrossDownloadedCorpusIsIdempotentAndSideEffectFree) {
    struct CorpusCase {
        const char* filename;
        std::size_t expected_pages;
        std::size_t expected_blocks;
        const char* expected_substr;
    };
    constexpr std::array<CorpusCase, 6> kBatch = {{
        {"dummy.pdf", 1U, 1U, "Dummy PDF file"},
        {"pdf20-simple.pdf", 1U, 1U, "Hello World"},
        {"pdfminer-simple1.pdf", 1U, 8U, "H e l l o"},
        {"pdfminer-simple3.pdf", 1U, 2U, "HelloHello"},
        {"tabula-eu-002.pdf", 2U, 23U, "0,2885"},
        {"tabula-twotables.pdf", 1U, 34U, "18,683"},
    }};

    for (const CorpusCase& item : kBatch) {
        const fs::path pdf = corpus_file(item.filename);
        if (!fs::exists(pdf)) {
            GTEST_SKIP() << "Missing corpus cache file: " << pdf.string();
        }
    }

    const pdf2md::Converter converter(pdf2md::ConvertOptions{});
    std::vector<pdf2md::ConvertResult> first_pass;
    first_pass.reserve(kBatch.size());

    for (const CorpusCase& item : kBatch) {
        SCOPED_TRACE(item.filename);
        const fs::path pdf = corpus_file(item.filename);
        const FileFingerprint before = fingerprint_file(pdf);
        pdf2md::ConvertResult res = converter.convert(pdf);
        EXPECT_EQ(res.page_count, item.expected_pages);
        EXPECT_EQ(res.block_count, item.expected_blocks);
        EXPECT_THAT(res.markdown, HasSubstr(item.expected_substr));
        EXPECT_EQ(fingerprint_file(pdf), before);
        first_pass.push_back(std::move(res));
    }

    // Replaying every item in reverse order on the same Converter proves zero cross-file bleed.
    ASSERT_EQ(first_pass.size(), kBatch.size());
    for (std::size_t rev = kBatch.size(); rev > 0U; --rev) {
        const std::size_t idx = rev - 1U;
        SCOPED_TRACE(kBatch[idx].filename);
        const pdf2md::ConvertResult replay = converter.convert(corpus_file(kBatch[idx].filename));
        EXPECT_EQ(replay.page_count, first_pass[idx].page_count);
        EXPECT_EQ(replay.block_count, first_pass[idx].block_count);
        EXPECT_EQ(replay.markdown, first_pass[idx].markdown);
    }
}

TEST(Corpus, ScannedBookWordsNotShredded) {
    const fs::path pdf = corpus_file("scanned_book.pdf");
    if (!fs::exists(pdf)) {
        GTEST_SKIP() << "Missing optional corpus cache file: " << pdf.string();
    }
    const FileFingerprint before = fingerprint_file(pdf);

    const pdf2md::Converter converter(pdf2md::ConvertOptions{});
    const pdf2md::ConvertResult res = converter.convert(pdf);
    EXPECT_GT(res.page_count, 0U);
    EXPECT_GT(res.block_count, 0U);
    ASSERT_FALSE(res.markdown.empty());
    EXPECT_LT(single_letter_token_ratio(res.markdown), kMaxSingleLetterTokenRatio);

    EXPECT_EQ(fingerprint_file(pdf), before);
}

TEST(Corpus, ScannedBookPageScansNotEmbeddedPerPage) {
    const fs::path pdf = corpus_file("scanned_book.pdf");
    if (!fs::exists(pdf)) {
        GTEST_SKIP() << "Missing optional corpus cache file: " << pdf.string();
    }
    const FileFingerprint before = fingerprint_file(pdf);
    const testfx::ScopedSandbox sandbox("corpus_scanned_book");

    pdf2md::ConvertOptions opts;
    opts.extract.extract_images = true;
    opts.extract.image_dir = sandbox.file("scanned_book_images");
    opts.extract.image_ref_prefix = "scanned_book_images/";
    const pdf2md::Converter converter(opts);
    const pdf2md::ConvertResult res = converter.convert(pdf);

    ASSERT_GT(res.page_count, 0U);
    EXPECT_GT(res.block_count, 0U);
    const std::vector<std::string> embeds = testfx::extract_markdown_image_paths(res.markdown);
    EXPECT_LT(embeds.size(), res.page_count / 2U);
    verify_corpus_png_embeds(res.markdown, sandbox.path(), embeds.size());

    EXPECT_EQ(fingerprint_file(pdf), before);
}

TEST(Corpus, NativeSpecUnaffectedByScanHeuristics) {
    const fs::path pdf = corpus_file("spec_document.pdf");
    if (!fs::exists(pdf)) {
        GTEST_SKIP() << "Missing optional corpus cache file: " << pdf.string();
    }
    const FileFingerprint before = fingerprint_file(pdf);
    const testfx::ScopedSandbox sandbox("corpus_spec_doc");

    pdf2md::ConvertOptions opts;
    opts.extract.extract_images = true;
    opts.extract.image_dir = sandbox.file("spec_document_images");
    opts.extract.image_ref_prefix = "spec_document_images/";
    const pdf2md::Converter converter(opts);
    const pdf2md::ConvertResult res = converter.convert(pdf);

    EXPECT_GT(res.page_count, 0U);
    EXPECT_GT(res.block_count, 0U);
    ASSERT_FALSE(res.markdown.empty());
    EXPECT_LT(single_letter_token_ratio(res.markdown), kMaxSingleLetterTokenRatio);
    EXPECT_THAT(res.markdown, HasSubstr("\n# "));
    EXPECT_THAT(res.markdown, HasSubstr("| --- |"));
    verify_corpus_png_embeds(res.markdown, sandbox.path(), 1U);

    EXPECT_EQ(fingerprint_file(pdf), before);
}

}  // namespace
