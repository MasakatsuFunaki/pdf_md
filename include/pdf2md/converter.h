#pragma once

#include "pdf2md/options.h"

#include <filesystem>
#include <memory>

namespace pdf2md {

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
