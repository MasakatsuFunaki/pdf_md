#pragma once

#include <string>
#include <vector>

namespace pdf2md {

// One extracted character in PDF page space (origin bottom-left, y grows up).
struct CharInfo {
    char32_t code = 0;
    double x = 0, y = 0;                              // baseline origin
    double left = 0, right = 0, bottom = 0, top = 0;  // glyph bounding box
    double size = 0;                                  // font size in points
    bool bold = false;
    bool italic = false;
    bool mono = false;
    bool space = false;
    // Char came from a scanned page's OCR text layer (sizes/positions repaired
    // from drawn geometry); line building applies OCR-tolerant gap heuristics.
    bool ocr = false;
};

struct ImageRef {
    std::string path;      // path as referenced from the markdown file
    double x = 0;          // left edge on page
    double y_top = 0;      // top edge on page (PDF space)
    double y_bottom = 0;   // bottom edge on page (PDF space)
};

// A thin axis-aligned line from a stroked or thin-filled vector path -- a table
// border rule. Page space (origin bottom-left, y grows up). A horizontal rule
// has y0 == y1 (its constant y); a vertical rule has x0 == x1 (its constant x).
struct RuleSegment {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool horizontal = false;
};

struct PageData {
    int index = 0;         // 0-based page number
    double width = 0;
    double height = 0;
    std::vector<CharInfo> chars;
    std::vector<ImageRef> images;
    std::vector<RuleSegment> rules;
};

struct DocMeta {
    std::string title;
    std::string author;
    int page_count = 0;
};

}  // namespace pdf2md
