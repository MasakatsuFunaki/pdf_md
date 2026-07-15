#pragma once

// Internal seam of the layout analyzer -- declarations shared between the
// layout_*.cpp modules; not part of the public API.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "document_model.h"
#include "layout_analyzer.h"

namespace pdf2md {
namespace detail {

// ---------------------------------------------------------------- utilities
double median(std::vector<double> v);
bool is_bullet_cp(char32_t cp);
bool is_dashy_cp(char32_t cp);
bool raised_origin_glyph(const CharInfo& c);

// ---------------------------------------------------------------- XY cut
void split_region(std::vector<const CharInfo*> chars, int depth,
                  std::vector<std::vector<const CharInfo*>>& out);

// ---------------------------------------------------------------- lines
double column_gap_threshold(double size);
void finalize_line(Line& line);
void build_runs(Line& line);
std::vector<Line> build_lines(const std::vector<const CharInfo*>& chars);

// ---------------------------------------------------------------- inline math
// One non-space glyph, tagged with whether an inter-word space precedes it.
struct Emit {
    size_t idx;
    bool space_before;
};

bool is_latin_letter_cp(char32_t cp);
bool is_letter_cp(char32_t cp);
int script_role(const Line& line, const CharInfo& c);
std::vector<Emit> plan_line(Line& line);
void render_plain(Line& line, const std::vector<Emit>& emits);
std::u32string math_to_latex(std::vector<const CharInfo*> gl, double axis, bool top_level,
                             int depth = 0);
void render_math(Line& line, const std::vector<Emit>& emits);
void normalize_inline_punct(Line& line);
bool line_needs_math(const Line& line);
void rewrite_footnote_refs(Line& line);
void repair_token_spaces(const Line& line, std::vector<Emit>& emits);

// ---------------------------------------------------------------- tables
struct TableCandidate {
    size_t first_line = 0, last_line = 0;  // inclusive range into region lines
    std::vector<std::vector<TableCell>> cells;
    // A class-style box holds a 2-column key/value head above a nested grid with
    // more columns (Attribute/Type/Mult./Kind/Note). The nested grid is split off
    // here and emitted as its own GFM table so its columns survive.
    std::vector<std::vector<std::vector<TableCell>>> extra;
};

TableCell render_cell_runs(const Line& line, size_t lo, size_t hi);
std::optional<TableCandidate> try_table(const std::vector<Line>& lines, size_t start,
                                        size_t region_lines, bool render, double page_width);
bool region_looks_tabular(const std::vector<const CharInfo*>& chars);

// ---------------------------------------------------------------- blocks
void finalize_block_geometry(Block& b);
size_t count_glyphs(const Line& line);
bool is_citation_lead(const Line& ln);
void region_to_blocks(std::vector<Line> lines, int page, const AnalyzeParams& params,
                      double col_left, double body_size, double page_width,
                      std::vector<Block>& out);

// ---------------------------------------------------------------- doc level
double compute_body_size(const std::vector<PageData>& pages);

// ------------------------------------------------- running chrome (line level)
struct PageLines {
    int index = 0;
    double width = 0, height = 0;
    std::vector<std::vector<Line>> regions;
};

void strip_running_chrome(std::vector<PageLines>& pages);

// ---------------------------------------------------------------- ruled grids
struct LatticeGroup {
    double y_top = 0;
    std::vector<Block> blocks;  // caption paragraph(s), if any, then the table
};

std::vector<LatticeGroup> detect_ruled_tables(const PageData& page, double body_size,
                                              std::vector<char>& consumed);

// ------------------------------------------------------------ pass pipeline
// The shared inputs a pass needs beyond the Document itself.
struct PassContext {
    const std::vector<PageData>& pages;
    const AnalyzeParams& params;
};

class DocumentPass {
public:
    virtual ~DocumentPass() = default;
    // Template Method: the gate is uniform, the algorithm virtual.
    void run(Document& doc, const PassContext& ctx) const {
        if (enabled(ctx.params)) apply(doc, ctx);
    }

protected:
    virtual bool enabled(const AnalyzeParams&) const { return true; }
    virtual void apply(Document& doc, const PassContext& ctx) const = 0;
};

std::vector<std::unique_ptr<DocumentPass>> build_pass_pipeline();

}  // namespace detail
}  // namespace pdf2md
