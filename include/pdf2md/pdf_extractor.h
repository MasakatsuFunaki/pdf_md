#pragma once

#include "pdf2md/document_model.h"
#include "pdf2md/options.h"

#include <filesystem>
#include <memory>

namespace pdf2md {

// Stage 1: loads a PDF document and extracts per-page characters, hyperlinks,
// outlines, vector rules, and figures. Insulated via PIMPL; concurrent const
// extract() calls are thread-safe. Throws PdfError on load/parse failure.
class PdfExtractor {
public:
    explicit PdfExtractor(ExtractOptions options = {});
    PdfExtractor(const PdfExtractor& other);
    PdfExtractor& operator=(const PdfExtractor& other);
    PdfExtractor(PdfExtractor&& other) noexcept;
    PdfExtractor& operator=(PdfExtractor&& other) noexcept;
    ~PdfExtractor();

    [[nodiscard]] Extraction extract(const std::filesystem::path& file) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Loads the PDF and extracts characters (and optionally images) per page.
// Throws PdfError on failure.
Extraction extract_pdf(const std::filesystem::path& file, const ExtractOptions& options = {});

}  // namespace pdf2md
