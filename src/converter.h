#pragma once

#include "layout_analyzer.h"
#include "markdown_writer.h"
#include "pdf_extractor.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

namespace pdf2md {

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

// Facade: unified entry point over the three conversion subsystems
// (pdf_extractor -> layout_analyzer -> markdown_writer). Insulated via PIMPL;
// concurrent const convert() calls are thread-safe. Throws PdfError on PDF
// load/extraction failure.
class Converter {
public:
    explicit Converter(ConvertOptions options = {});
    Converter(const Converter& other);
    Converter& operator=(const Converter& other);
    Converter(Converter&& other) noexcept;
    Converter& operator=(Converter&& other) noexcept;
    ~Converter();

    ConvertResult convert(const std::filesystem::path& input) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pdf2md
