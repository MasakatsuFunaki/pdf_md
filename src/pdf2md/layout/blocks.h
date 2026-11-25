#pragma once

#include "pdf2md/document_model.h"
#include "pdf2md/options.h"

#include <span>
#include <vector>

namespace pdf2md::layout {

void region_to_blocks(std::vector<Line> lines, int page, const AnalyzeParams& params,
                      double col_left, double body_size, double page_width,
                      std::vector<Block>& out,
                      std::span<const RuleSegment> page_rules = {},
                      std::vector<char>* consumed_rules = nullptr);

}  // namespace pdf2md::layout
