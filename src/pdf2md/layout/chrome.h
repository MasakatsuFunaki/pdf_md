#pragma once

#include "pdf2md/document_model.h"

#include <vector>

namespace pdf2md::layout {

struct PageLines {
    int index = 0;
    double width = 0.0;
    double height = 0.0;
    std::vector<std::vector<Line>> regions;
};

double compute_body_size(const std::vector<PageData>& pages);
void strip_running_chrome(std::vector<PageLines>& pages);
void strip_headers_footers(std::vector<Block>& blocks, const std::vector<PageData>& pages);

}  // namespace pdf2md::layout
