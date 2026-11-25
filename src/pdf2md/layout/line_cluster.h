#pragma once

#include "pdf2md/document_model.h"

#include <span>
#include <vector>

namespace pdf2md::layout {

double column_gap_threshold(double size) noexcept;
double compute_word_gap(const Line& line);
void finalize_line(Line& line);

// Pure baseline clustering and x-sorting of region characters into finalized
// Lines (without inline run or LaTeX math rendering).
std::vector<Line> cluster_lines(std::span<const CharInfo* const> chars);

}  // namespace pdf2md::layout
