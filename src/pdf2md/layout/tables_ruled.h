#pragma once

#include "pdf2md/document_model.h"

#include <vector>

namespace pdf2md::layout {

struct LatticeGroup {
    double y_top = 0.0;
    std::vector<Block> blocks;  // caption paragraph(s), if any, then the table
};

// Recovers bordered grid tables from stroked/filled RuleSegments without linking
// layout_blocks. Marks consumed page characters in `consumed_chars`, and when
// `consumed_rules` is non-null marks rules belonging to accepted table boxes.
std::vector<LatticeGroup> detect_ruled_tables(const PageData& page, double body_size,
                                              std::vector<char>& consumed_chars,
                                              std::vector<char>* consumed_rules = nullptr);

}  // namespace pdf2md::layout
