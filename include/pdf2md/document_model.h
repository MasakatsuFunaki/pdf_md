#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pdf2md {

// One extracted character in PDF page space (origin bottom-left, y grows up).
struct CharInfo {
    char32_t code = 0;
    double x = 0, y = 0;                              // baseline origin
    double left = 0, right = 0, bottom = 0, top = 0;  // glyph bounding box
    double size = 0;                                  // font size in points
    // Em-box height as drawn, i.e. the em square pushed through the full text
    // matrix. Unlike `size` this survives a scaling `cm`, so the two disagreeing
    // is how a page with an inflated nominal size is recognised. Carried on the
    // char rather than in a parallel array because glyphs are erased (figure
    // labels) between extraction and the document-wide size repair.
    double loose_height = 0;
    bool bold = false;
    bool italic = false;
    bool mono = false;
    bool space = false;
    // Char came from a scanned page's OCR text layer (sizes/positions repaired
    // from drawn geometry); line building applies OCR-tolerant gap heuristics.
    bool ocr = false;
    bool strikethrough = false;
    std::uint16_t link_id = 0;  // 0 = none; 1..N = 1-based index into PageData::links
};

struct ImageRef {
    std::string path;      // path as referenced from the markdown file
    double x = 0;          // left edge on page
    double y_top = 0;      // top edge on page (PDF space)
    double y_bottom = 0;   // bottom edge on page (PDF space)
    double x_right = 0;    // right edge on page
    std::string alt;       // tagged-PDF /Alt text, if present
};

struct PageLink {
    double left = 0;
    double bottom = 0;
    double right = 0;
    double top = 0;
    std::string uri;
};

// A thin axis-aligned line from a stroked or thin-filled vector path -- a table
// border rule, fraction bar, vinculum, or divider. Page space (origin bottom-left,
// y grows up). A horizontal rule has y0 == y1 (its constant y); a vertical rule
// has x0 == x1 (its constant x).
struct RuleSegment {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool horizontal = false;
    double thickness = 0.0;
};

struct PageData {
    int index = 0;         // 0-based page number
    double width = 0;
    double height = 0;
    std::vector<CharInfo> chars;
    std::vector<ImageRef> images;
    std::vector<RuleSegment> rules;
    std::vector<PageLink> links;
};

struct OutlineEntry {
    std::string title;
    int level = 1;
    int page = -1;
    double y = 0.0;
    bool has_y = false;
};

struct DocMeta {
    std::string title;
    std::string author;
    int page_count = 0;
    std::string subject;
    std::string keywords;
    std::string creator;
    std::string producer;
    std::string creation_date;
    std::string mod_date;
    std::vector<OutlineEntry> outlines;
};

struct Extraction {
    DocMeta meta;
    std::vector<PageData> pages;
};

struct StyledRun {
    std::u32string text;
    bool bold = false;
    bool italic = false;
    bool mono = false;
    bool math = false;  // text is LaTeX; the writer wraps it in `$...$`
    bool raw = false;   // text is literal markdown emitted verbatim (e.g. "[^5]")
    bool strikethrough = false;
    std::uint16_t link_id = 0;
    std::string link_url;
};

// A visual line: characters sharing a baseline inside one layout region.
struct Line {
    std::vector<CharInfo> chars;   // x-sorted
    std::vector<StyledRun> runs;   // built from chars, spaces collapsed
    std::u32string plain;          // concatenation of run texts
    // Maximal char index ranges separated by column-sized horizontal gaps.
    std::vector<std::pair<std::size_t, std::size_t>> segments;  // [first, last] inclusive
    double x0 = 0, x1 = 0, baseline = 0, size = 0;
    double size_max = 0;  // body-size reference for sub/superscript detection
    double word_gap = 0;  // adaptive inter-word gap threshold for this line
    bool mono_majority = false;
    bool ocr = false;      // majority of glyphs from a repaired OCR text layer
    std::vector<RuleSegment> rules;
};

enum class BlockKind {
    Paragraph,
    Heading,
    ListItem,
    Code,
    Table,
    Image,
    MathDisplay,
    HorizontalRule,
};

enum class TableAlign { None, Left, Center, Right };

enum class TaskState { None, Unchecked, Checked };

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
    std::u32string list_marker;     // normalized marker, e.g. U"-" or U"3."
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
    std::vector<TableAlign> table_align;
    std::string image_alt;
    TaskState task_state = TaskState::None;
};

struct Document {
    DocMeta meta;
    std::vector<Block> blocks;     // in reading order
    double body_size = 11.0;
};

}  // namespace pdf2md
