#pragma once

#include "pdf2md/document_model.h"
#include "pdf2md/options.h"

#include <memory>
#include <string>

namespace pdf2md {

// Stage 3: renders a reconstructed Document AST to GitHub-flavored Markdown.
// Insulated via PIMPL; stateless per document so concurrent or repeated const
// write() calls on the same instance are thread-safe and bleed-free.
class MarkdownWriter {
public:
    explicit MarkdownWriter(WriteOptions options = {});
    MarkdownWriter(const MarkdownWriter& other);
    MarkdownWriter& operator=(const MarkdownWriter& other);
    MarkdownWriter(MarkdownWriter&& other) noexcept;
    MarkdownWriter& operator=(MarkdownWriter&& other) noexcept;
    ~MarkdownWriter();

    [[nodiscard]] std::string write(const Document& doc) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::string write_markdown(const Document& doc, const WriteOptions& options = {});

}  // namespace pdf2md
