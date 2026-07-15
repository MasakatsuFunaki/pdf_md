#include "converter.h"

#include <utility>

namespace pdf2md {

Converter::Converter(ConvertOptions options) : options_(std::move(options)) {}

// Runs the three subsystems back to back: extract -> analyze -> write.
ConvertResult Converter::convert(const std::filesystem::path& input) const {
    Extraction extraction = extract_pdf(input, options_.extract);
    Document doc = analyze_layout(extraction.meta, extraction.pages, options_.analyze);
    std::string markdown = write_markdown(doc, options_.write);
    return ConvertResult{std::move(markdown), extraction.pages.size(), doc.blocks.size()};
}

}  // namespace pdf2md
