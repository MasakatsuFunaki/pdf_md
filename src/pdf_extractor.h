#pragma once

#include <filesystem>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "document_model.h"

namespace pdf2md {

struct ExtractOptions {
    std::string password;
    std::set<int> pages;                 // 0-based; empty = all pages
    bool keep_rotated = false;           // keep text runs at non-horizontal angles
    bool extract_images = false;
    std::filesystem::path image_dir;     // where PNG files are written
    std::string image_ref_prefix;        // path prefix used inside the markdown
};

struct Extraction {
    DocMeta meta;
    std::vector<PageData> pages;
};

class PdfError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Loads the PDF and extracts characters (and optionally images) per page.
// Throws PdfError on failure.
Extraction extract_pdf(const std::filesystem::path& file, const ExtractOptions& options);

}  // namespace pdf2md
