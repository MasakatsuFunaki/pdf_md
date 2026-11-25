#pragma once

#include "pdf2md/document_model.h"

#include <span>
#include <vector>

namespace pdf2md::layout {

void classify_headings(std::vector<Block>& blocks, double body_size,
                       std::span<const OutlineEntry> outlines = {});

}  // namespace pdf2md::layout
