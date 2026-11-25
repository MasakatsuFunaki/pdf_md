#pragma once

#include "pdf2md/document_model.h"

#include <vector>

namespace pdf2md::layout {

// Recursive XY-cut segmentation: horizontal cuts first, then column cuts
// gated by region_looks_tabular.
void split_region(std::vector<const CharInfo*> chars, int depth,
                  std::vector<std::vector<const CharInfo*>>& out);

}  // namespace pdf2md::layout
