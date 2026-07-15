#pragma once

#include <string>
#include <vector>

#include "document_model.h"

namespace pdf2md {

struct StyledRun {
    std::u32string text;
    bool bold = false;
    bool italic = false;
    bool mono = false;
    bool math = false;  // text is LaTeX; the writer wraps it in `$...$`
    bool raw = false;   // text is literal markdown emitted verbatim (e.g. "[^5]")
};

// A visual line: characters sharing a baseline inside one layout region.
struct Line {
    std::vector<CharInfo> chars;   // x-sorted
    std::vector<StyledRun> runs;   // built from chars, spaces collapsed
    std::u32string plain;          // concatenation of run texts
    // Maximal char index ranges separated by column-sized horizontal gaps.
    std::vector<std::pair<size_t, size_t>> segments;  // [first, last] inclusive
    double x0 = 0, x1 = 0, baseline = 0, size = 0;
    double size_max = 0;  // body-size reference for sub/superscript detection
    double word_gap = 0;  // adaptive inter-word gap threshold for this line
    bool mono_majority = false;
    bool ocr = false;      // majority of glyphs from a repaired OCR text layer
};

enum class BlockKind { Paragraph, Heading, ListItem, Code, Table, Image, MathDisplay };

// A rendered table cell: math-aware styled runs, emitted by the markdown
// writer (so superscripts/subscripts and bold highlights survive into cells).
using TableCell = std::vector<StyledRun>;

struct Block {
    BlockKind kind = BlockKind::Paragraph;
    int page = 0;
    double x0 = 0, x1 = 0, y_top = 0, y_bottom = 0;
    double size = 0;               // median font size of the block
    bool bold_majority = false;
    int heading_level = 0;
    int list_indent = 0;           // nesting depth, 0-based
    bool list_ordered = false;
    bool blockquote = false;       // paragraph rendered as a `>` blockquote
    // Footnote definition: rendered "[^label]: text" (numbered) or with the
    // symbol as a literal prefix (e.g. "†text"); numbered notes float to the end.
    bool is_footnote = false;
    bool footnote_numbered = false;
    std::u32string footnote_label;  // "5" or U"†"
    std::u32string list_marker;    // normalized marker, e.g. U"-" or U"3."
    std::vector<Line> lines;
    std::vector<std::vector<TableCell>> table_cells;  // rows x columns
    // For a ruled-lattice Table: the x-positions of its column boundaries (the
    // vertical rules). Empty for text-geometry tables. Used to recognize a table
    // continued on the next page (same column signature) and stitch it back.
    std::vector<double> col_edges;
    std::string image_path;
    // For MathDisplay: one reconstructed LaTeX body per visual equation line
    // (aligned when there is more than one) and the equation number, if any.
    std::vector<std::u32string> math_lines;
    std::u32string math_tag;       // e.g. U"1" for \tag{1}; empty when absent
};

struct AnalyzeParams {
    bool detect_headings = true;
    bool detect_tables = true;
    bool strip_headers_footers = true;
};

struct Document {
    DocMeta meta;
    std::vector<Block> blocks;     // in reading order
    double body_size = 11.0;
};

Document analyze_layout(const DocMeta& meta, const std::vector<PageData>& pages,
                        const AnalyzeParams& params);

}  // namespace pdf2md
