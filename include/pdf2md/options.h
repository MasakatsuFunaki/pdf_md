#pragma once

#include <cstddef>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>

namespace pdf2md {

struct ExtractOptions {
    std::string password;
    std::set<int> pages;                 // 0-based; empty = all pages
    bool keep_rotated = false;           // keep text runs at non-horizontal angles
    bool extract_images = false;
    std::filesystem::path image_dir;     // where PNG files are written
    std::string image_ref_prefix;        // path prefix used inside the markdown
    bool extract_links = true;
};

struct AnalyzeParams {
    bool detect_headings = true;
    bool detect_tables = true;
    bool strip_headers_footers = true;
};

struct WriteOptions {
    bool page_breaks = false;    // emit "---" between pages
    bool front_matter = false;   // emit a YAML front-matter block
    std::string source_name;     // original file name, used in front matter
    bool links = true;           // emit [text](url) hyperlinks
    bool toc = false;            // emit a Markdown table of contents
};

// Options for each stage of the conversion pipeline, bundled into one aggregate.
struct ConvertOptions {
    ExtractOptions extract;
    AnalyzeParams analyze;
    WriteOptions write;
};

struct ConvertResult {
    std::string markdown;
    std::size_t page_count = 0;   // pages actually extracted
    std::size_t block_count = 0;  // document blocks produced
};

class PdfError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
    PdfError(const PdfError&) = default;
    PdfError& operator=(const PdfError&) = default;
    PdfError(PdfError&&) noexcept = default;
    PdfError& operator=(PdfError&&) noexcept = default;
    ~PdfError() noexcept override;
};

}  // namespace pdf2md
