#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "layout_analyzer.h"
#include "markdown_writer.h"
#include "pdf_extractor.h"

namespace pdf2md {

// The options for each stage of the pipeline, bundled into one struct.
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

// Facade: a single entry point over the three conversion subsystems
// (pdf_extractor -> layout_analyzer -> markdown_writer). It bundles their
// options and runs the extract -> analyze -> write pipeline in one call, so a
// caller need not wire the stages together. The subsystems stay independently
// usable and are still driven directly by the tests. convert() throws PdfError
// on load/extraction failure, just as extract_pdf does.
class Converter {
public:
    explicit Converter(ConvertOptions options);
    ConvertResult convert(const std::filesystem::path& input) const;

private:
    ConvertOptions options_;
};

}  // namespace pdf2md
