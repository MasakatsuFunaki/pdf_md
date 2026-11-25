#pragma once

#include "pdf2md/document_model.h"

#include <cstddef>
#include <span>

namespace pdf2md::model {

// Private domain-model geometry and classification helpers shared across src/.
void compute_lines_geometry(std::span<const Line> lines, Block& b);
void finalize_block_geometry(Block& b);
std::size_t count_glyphs(const Line& line) noexcept;
bool raised_origin_glyph(const CharInfo& c) noexcept;
bool is_citation_lead(const Line& ln) noexcept;

constexpr bool is_prose_paragraph(const Block& b) noexcept {
    return b.kind == BlockKind::Paragraph && !b.is_footnote && !b.blockquote;
}

}  // namespace pdf2md::model
