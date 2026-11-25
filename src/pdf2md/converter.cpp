#include "pdf2md/converter.h"

#include "pdf2md/layout_analyzer.h"
#include "pdf2md/markdown_writer.h"
#include "pdf2md/pdf_extractor.h"

#include <memory>
#include <utility>

namespace pdf2md {

struct Converter::Impl {
    PdfExtractor extractor;
    LayoutAnalyzer analyzer;
    MarkdownWriter writer;

    explicit Impl(ConvertOptions opts)
        : extractor(std::move(opts.extract)),
          analyzer(opts.analyze),
          writer(std::move(opts.write)) {}

    [[nodiscard]] ConvertResult convert(const std::filesystem::path& input) const {
        const Extraction ex = extractor.extract(input);
        const Document doc = analyzer.analyze(ex.meta, ex.pages);
        std::string markdown = writer.write(doc);
        return ConvertResult{std::move(markdown), ex.pages.size(), doc.blocks.size()};
    }
};

Converter::Converter(ConvertOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

Converter::Converter(const Converter& other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

Converter& Converter::operator=(const Converter& other) {
    if (this == &other) {
        return *this;
    }
    if (!other.impl_) {
        impl_.reset();
        return *this;
    }
    auto copy = std::make_unique<Impl>(*other.impl_);
    impl_ = std::move(copy);
    return *this;
}

Converter::Converter(Converter&& other) noexcept = default;

Converter& Converter::operator=(Converter&& other) noexcept = default;

Converter::~Converter() = default;

ConvertResult Converter::convert(const std::filesystem::path& input) const {
    if (!impl_) {
        return Converter{}.convert(input);
    }
    return impl_->convert(input);
}

}  // namespace pdf2md
