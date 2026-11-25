#include "cli_config.h"

#include "utf8.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<std::uint64_t> counter{0};
        std::random_device rd;
        const auto ticks = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const std::uint64_t id = counter.fetch_add(1, std::memory_order_relaxed);
        const std::string dir_name = "pdf2md_cli_ut_" + std::to_string(ticks) + "_" +
                                     std::to_string(rd()) + "_" + std::to_string(id);
        path_ = fs::temp_directory_path() / dir_name;
        fs::create_directories(path_);
    }

    ~ScopedTempDir() noexcept {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

void write_dummy_file(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << "%PDF-1.4\n";
}

struct InvalidPageSpecCase {
    const char* name;
    std::string_view spec;
};

constexpr InvalidPageSpecCase kInvalidPageSpecs[] = {
    {"empty_spec", ""},
    {"zero_page_below_one_based_min", "0"},
    {"range_starting_at_zero", "0-3"},
    {"negative_page_number", "-1"},
    {"reversed_range_bounds", "5-2"},
    {"span_exceeding_max_limit_seam", "1-100001"},
    {"page_exceeding_max_page_seam", "10000001"},
    {"uint64_overflow_digits", "99999999999999999999"},
    {"trailing_comma", "1,"},
    {"leading_comma", ",1"},
    {"double_comma_empty_segment", "1,,3"},
    {"dangling_range_hyphen", "1-"},
    {"triple_endpoint_range", "1-2-3"},
    {"alphabetic_token", "abc"},
    {"trailing_alpha_in_number", "1a"},
    {"embedded_whitespace_around_hyphen", "1 - 2"},
};

constexpr std::size_t kExpectedInvalidPageSpecCount = 16;
static_assert(std::size(kInvalidPageSpecs) == kExpectedInvalidPageSpecCount);

struct ValidPageSpecCase {
    const char* name;
    std::string_view spec;
    std::vector<int> expected_zero_based;
};

TEST(CliConfig, ParsesOneBasedPageSpecsAndRejectsBoundaryViolations) {
    ASSERT_EQ(std::size(kInvalidPageSpecs), kExpectedInvalidPageSpecCount);
    for (const auto& tc : kInvalidPageSpecs) {
        SCOPED_TRACE(tc.name);
        std::set<int> out;
        EXPECT_FALSE(pdf2md::parse_pages(tc.spec, out));
    }

    const std::vector<ValidPageSpecCase> valid_cases = {
        {"min_single_page_one", "1", {0}},
        {"degenerate_single_page_range", "4-4", {3}},
        {"mixed_range_and_singleton", "1-3,7", {0, 1, 2, 6}},
        {"overlapping_and_unsorted_segments_deduplicated", "7,2-4,1-2", {0, 1, 2, 3, 6}},
        {"max_page_number_seam", "10000000", {9999999}},
    };
    constexpr std::size_t kExpectedValidCount = 5;
    ASSERT_EQ(valid_cases.size(), kExpectedValidCount);
    for (const auto& tc : valid_cases) {
        SCOPED_TRACE(tc.name);
        std::set<int> out;
        ASSERT_TRUE(pdf2md::parse_pages(tc.spec, out));
        const std::set<int> expected(tc.expected_zero_based.begin(), tc.expected_zero_based.end());
        EXPECT_EQ(out, expected);
    }

    // Exact maximum span seam: "1-100000" succeeds -> "1-100001" fails -> "2" succeeds cleanly.
    constexpr std::size_t kMaxSpanCount = 100000U;
    constexpr int kMaxSpanLastZeroBased = 99999;
    std::set<int> span_out;
    ASSERT_TRUE(pdf2md::parse_pages("1-100000", span_out));
    ASSERT_EQ(span_out.size(), kMaxSpanCount);
    EXPECT_EQ(*span_out.begin(), 0);
    EXPECT_EQ(*span_out.rbegin(), kMaxSpanLastZeroBased);

    EXPECT_FALSE(pdf2md::parse_pages("1-100001", span_out));

    std::set<int> recovery_out;
    ASSERT_TRUE(pdf2md::parse_pages("2", recovery_out));
    EXPECT_EQ(recovery_out, std::set<int>{1});
}

TEST(CliConfig, ResolvesDefaultPathsStdoutAndImageExtractionModes) {
    ScopedTempDir tmp;
    const fs::path input_pdf = tmp.path() / "docs" / "paper.pdf";
    write_dummy_file(input_pdf);

    // 1. Default output: sibling .md + sibling <stem>_images/ directory and relative prefix.
    {
        pdf2md::CliRawArgs raw;
        raw.input = pdf2md::path_to_utf8(input_pdf);
        const pdf2md::ResolvedCliConfig cfg = pdf2md::resolve_cli_config(raw);

        EXPECT_EQ(cfg.status, pdf2md::ExitCode::Ok);
        EXPECT_EQ(cfg.error, "");
        EXPECT_FALSE(cfg.to_stdout);
        EXPECT_FALSE(cfg.quiet);
        EXPECT_EQ(cfg.input_path, input_pdf);
        EXPECT_EQ(cfg.output_path, tmp.path() / "docs" / "paper.md");
        EXPECT_TRUE(cfg.options.extract.extract_images);
        EXPECT_EQ(cfg.options.extract.image_dir, tmp.path() / "docs" / "paper_images");
        EXPECT_EQ(cfg.options.extract.image_ref_prefix, "paper_images/");
        EXPECT_EQ(cfg.options.write.source_name, "paper.pdf");
    }

    // 2. Explicit -o - (stdout) disables default image extraction unless --images is provided.
    {
        pdf2md::CliRawArgs raw;
        raw.input = pdf2md::path_to_utf8(input_pdf);
        raw.output = "-";
        const pdf2md::ResolvedCliConfig stdout_default = pdf2md::resolve_cli_config(raw);

        EXPECT_EQ(stdout_default.status, pdf2md::ExitCode::Ok);
        EXPECT_EQ(stdout_default.error, "");
        EXPECT_TRUE(stdout_default.to_stdout);
        EXPECT_EQ(stdout_default.output_path, fs::path{});
        EXPECT_FALSE(stdout_default.options.extract.extract_images);

        const fs::path explicit_imgs = tmp.path() / "custom_assets";
        raw.images_dir = pdf2md::path_to_utf8(explicit_imgs);
        const pdf2md::ResolvedCliConfig stdout_with_imgs = pdf2md::resolve_cli_config(raw);

        EXPECT_EQ(stdout_with_imgs.status, pdf2md::ExitCode::Ok);
        EXPECT_TRUE(stdout_with_imgs.to_stdout);
        EXPECT_TRUE(stdout_with_imgs.options.extract.extract_images);
        EXPECT_EQ(stdout_with_imgs.options.extract.image_dir, explicit_imgs);
    }

    // 3. --no-images disables image extraction even when writing to a file.
    {
        pdf2md::CliRawArgs raw;
        raw.input = pdf2md::path_to_utf8(input_pdf);
        raw.no_images = true;
        const pdf2md::ResolvedCliConfig cfg = pdf2md::resolve_cli_config(raw);

        EXPECT_EQ(cfg.status, pdf2md::ExitCode::Ok);
        EXPECT_FALSE(cfg.options.extract.extract_images);
        EXPECT_EQ(cfg.options.extract.image_dir, fs::path{});
        EXPECT_EQ(cfg.options.extract.image_ref_prefix, "");
    }

    // 4. Non-ASCII Unicode PDF stem resolves sibling .md and <stem>_images cleanly.
    {
        const fs::path unicode_pdf =
            tmp.path() / pdf2md::path_from_utf8("\xE8\xAB\x96\xE6\x96\x87_\xF0\x9F\x93\x84.pdf");
        write_dummy_file(unicode_pdf);

        pdf2md::CliRawArgs raw;
        raw.input = pdf2md::path_to_utf8(unicode_pdf);
        const pdf2md::ResolvedCliConfig cfg = pdf2md::resolve_cli_config(raw);

        EXPECT_EQ(cfg.status, pdf2md::ExitCode::Ok);
        EXPECT_EQ(cfg.output_path,
                  tmp.path() /
                      pdf2md::path_from_utf8("\xE8\xAB\x96\xE6\x96\x87_\xF0\x9F\x93\x84.md"));
        EXPECT_EQ(cfg.options.extract.image_ref_prefix,
                  "\xE8\xAB\x96\xE6\x96\x87_\xF0\x9F\x93\x84_images/");
    }
}

TEST(CliConfig, MapsPipelineFeatureFlagsToConvertOptions) {
    ScopedTempDir tmp;
    const fs::path input_pdf = tmp.path() / "spec.pdf";
    write_dummy_file(input_pdf);

    // Baseline defaults: links enabled in both extractor and writer, TOC disabled.
    {
        pdf2md::CliRawArgs defaults;
        defaults.input = pdf2md::path_to_utf8(input_pdf);
        const pdf2md::ResolvedCliConfig cfg = pdf2md::resolve_cli_config(defaults);

        EXPECT_EQ(cfg.status, pdf2md::ExitCode::Ok);
        EXPECT_TRUE(cfg.options.extract.extract_links);
        EXPECT_FALSE(cfg.options.extract.keep_rotated);
        EXPECT_TRUE(cfg.options.analyze.detect_headings);
        EXPECT_TRUE(cfg.options.analyze.detect_tables);
        EXPECT_TRUE(cfg.options.analyze.strip_headers_footers);
        EXPECT_TRUE(cfg.options.write.links);
        EXPECT_FALSE(cfg.options.write.toc);
        EXPECT_FALSE(cfg.options.write.front_matter);
        EXPECT_FALSE(cfg.options.write.page_breaks);
    }

    // All feature toggles flipped simultaneously.
    {
        pdf2md::CliRawArgs raw;
        raw.input = pdf2md::path_to_utf8(input_pdf);
        raw.pages_spec = "2-3,5";
        raw.password = "secret42";
        raw.no_images = true;
        raw.page_breaks = true;
        raw.front_matter = true;
        raw.keep_headers = true;
        raw.no_headings = true;
        raw.no_tables = true;
        raw.keep_rotated = true;
        raw.no_links = true;
        raw.toc = true;
        raw.quiet = true;

        const pdf2md::ResolvedCliConfig cfg = pdf2md::resolve_cli_config(raw);
        EXPECT_EQ(cfg.status, pdf2md::ExitCode::Ok);
        EXPECT_TRUE(cfg.quiet);
        EXPECT_EQ(cfg.options.extract.password, "secret42");
        const std::set<int> expected_pages = {1, 2, 4};
        EXPECT_EQ(cfg.options.extract.pages, expected_pages);
        EXPECT_TRUE(cfg.options.extract.keep_rotated);
        EXPECT_FALSE(cfg.options.extract.extract_links);
        EXPECT_FALSE(cfg.options.analyze.detect_headings);
        EXPECT_FALSE(cfg.options.analyze.detect_tables);
        EXPECT_FALSE(cfg.options.analyze.strip_headers_footers);
        EXPECT_TRUE(cfg.options.write.page_breaks);
        EXPECT_TRUE(cfg.options.write.front_matter);
        EXPECT_FALSE(cfg.options.write.links);
        EXPECT_TRUE(cfg.options.write.toc);
    }
}

TEST(CliConfig, ClassifiesFilesystemAndArgumentPreconditionErrors) {
    ScopedTempDir tmp;
    const fs::path valid_pdf = tmp.path() / "valid.pdf";
    write_dummy_file(valid_pdf);

    const fs::path existing_dir = tmp.path() / "existing_dir";
    fs::create_directories(existing_dir);

    const fs::path existing_file = tmp.path() / "occupied_file.bin";
    write_dummy_file(existing_file);

    const fs::path default_md_dir_pdf = tmp.path() / "dir_collision.pdf";
    write_dummy_file(default_md_dir_pdf);
    fs::create_directories(tmp.path() / "dir_collision.md");

    const fs::path default_img_file_pdf = tmp.path() / "img_collision.pdf";
    write_dummy_file(default_img_file_pdf);
    write_dummy_file(tmp.path() / "img_collision_images");

    struct ErrorCase {
        const char* name;
        pdf2md::CliRawArgs raw;
        pdf2md::ExitCode expected_status;
    };

    pdf2md::CliRawArgs bad_pages;
    bad_pages.input = pdf2md::path_to_utf8(valid_pdf);
    bad_pages.pages_spec = "5-2";

    pdf2md::CliRawArgs missing_input;
    missing_input.input = pdf2md::path_to_utf8(tmp.path() / "nonexistent.pdf");

    pdf2md::CliRawArgs dir_input;
    dir_input.input = pdf2md::path_to_utf8(existing_dir);

    pdf2md::CliRawArgs output_is_dir;
    output_is_dir.input = pdf2md::path_to_utf8(valid_pdf);
    output_is_dir.output = pdf2md::path_to_utf8(existing_dir);

    pdf2md::CliRawArgs default_output_is_dir;
    default_output_is_dir.input = pdf2md::path_to_utf8(default_md_dir_pdf);

    pdf2md::CliRawArgs img_dir_is_file;
    img_dir_is_file.input = pdf2md::path_to_utf8(valid_pdf);
    img_dir_is_file.images_dir = pdf2md::path_to_utf8(existing_file);

    pdf2md::CliRawArgs default_img_dir_is_file;
    default_img_dir_is_file.input = pdf2md::path_to_utf8(default_img_file_pdf);

    const std::vector<ErrorCase> cases = {
        {"invalid_reversed_page_range", bad_pages, pdf2md::ExitCode::BadArgs},
        {"missing_input_pdf_path", missing_input, pdf2md::ExitCode::PdfError},
        {"input_path_is_directory", dir_input, pdf2md::ExitCode::PdfError},
        {"explicit_output_path_is_directory", output_is_dir, pdf2md::ExitCode::IoError},
        {"default_sibling_output_path_is_directory", default_output_is_dir,
         pdf2md::ExitCode::IoError},
        {"explicit_images_dir_is_regular_file", img_dir_is_file, pdf2md::ExitCode::IoError},
        {"default_images_dir_is_regular_file", default_img_dir_is_file, pdf2md::ExitCode::IoError},
    };

    constexpr std::size_t kExpectedErrorCaseCount = 7;
    ASSERT_EQ(cases.size(), kExpectedErrorCaseCount);
    for (const auto& tc : cases) {
        SCOPED_TRACE(tc.name);
        const pdf2md::ResolvedCliConfig cfg = pdf2md::resolve_cli_config(tc.raw);
        EXPECT_EQ(cfg.status, tc.expected_status);
    }
}

TEST(CliConfig, RejectsConflictingImagesAndNoImagesFlags) {
    ScopedTempDir tmp;
    const fs::path valid_pdf = tmp.path() / "valid.pdf";
    write_dummy_file(valid_pdf);

    pdf2md::CliRawArgs both_img_flags;
    both_img_flags.input = pdf2md::path_to_utf8(valid_pdf);
    both_img_flags.images_dir = pdf2md::path_to_utf8(tmp.path() / "imgs");
    both_img_flags.no_images = true;

    const pdf2md::ResolvedCliConfig cfg = pdf2md::resolve_cli_config(both_img_flags);
    EXPECT_EQ(cfg.status, pdf2md::ExitCode::BadArgs);
}

}  // namespace
