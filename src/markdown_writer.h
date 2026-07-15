#pragma once

#include <string>

#include "layout_analyzer.h"

namespace pdf2md {

struct WriteOptions {
    bool page_breaks = false;    // emit "---" between pages
    bool front_matter = false;   // emit a YAML front-matter block
    std::string source_name;     // original file name, used in front matter
};

std::string write_markdown(const Document& doc, const WriteOptions& options);

}  // namespace pdf2md
