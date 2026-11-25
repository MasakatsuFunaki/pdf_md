#pragma once

#include "pdf2md/document_model.h"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace pdf2md::layout {

struct ExtraTableGrid {
    std::size_t first_line = 0;
    std::size_t last_line = 0;
    std::vector<std::vector<TableCell>> cells;
    std::vector<TableAlign> align;
};

struct TableCandidate {
    std::size_t first_line = 0;
    std::size_t last_line = 0;  // inclusive range into region lines (across head + extra)
    std::vector<std::vector<TableCell>> cells;
    std::vector<TableAlign> align;
    // A class-style box holds a 2-column key/value head above a nested grid with
    // more columns (Attribute/Type/Mult./Kind/Note). The nested grid is split off
    // here and emitted as its own GFM table so its columns survive.
    std::vector<ExtraTableGrid> extra;
};

TableCell render_cell_runs(const Line& line, std::size_t lo, std::size_t hi);

std::optional<TableCandidate> try_table(const std::vector<Line>& lines, std::size_t start,
                                        std::size_t region_lines, bool render, double page_width);

// Fast geometry-only probe (cluster_lines + plan_line, no LaTeX render_math)
// used by XY-cut region splitting to avoid slicing aligned tables vertically.
bool region_looks_tabular(std::span<const CharInfo* const> chars);

}  // namespace pdf2md::layout
