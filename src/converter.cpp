#include "converter.h"

#include <cassert>
#include <memory>
#include <utility>

namespace pdf2md {

struct Converter::Impl {
    ConvertOptions options;

    explicit Impl(ConvertOptions opts) : options(std::move(opts)) {}

    [[nodiscard]] ConvertResult convert(const std::filesystem::path& input) const {
        Extraction extraction = extract_pdf(input, options.extract);
        const Document doc = analyze_layout(extraction.meta, extraction.pages, options.analyze);
        std::string markdown = write_markdown(doc, options.write);
        return ConvertResult{std::move(markdown), extraction.pages.size(), doc.blocks.size()};
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
    assert(impl_ != nullptr);
    return impl_->convert(input);
}

}  // namespace pdf2md
