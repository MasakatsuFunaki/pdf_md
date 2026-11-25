#pragma once

// Internal seam of the layout analyzer -- declarations shared between the
// layout_*.cpp modules; not part of the public API.

#include "document_model.h"
#include "layout_analyzer.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pdf2md {
namespace detail {

// Shared baseline defaults and coordinate sentinels across layout modules.
inline constexpr double kDefaultBodySizePt = 11.0;
inline constexpr double kMinFontSizePt = 1.0;
inline constexpr double kCoordSentinel = 1e30;
inline constexpr double kCoordSentinelThreshold = 1e29;
inline constexpr double kDefaultWordGapScale = 0.28;

// Shared utilities (layout_lines.cpp)
double median(std::vector<double> v);
bool is_bullet_cp(char32_t cp) noexcept;
bool is_dashy_cp(char32_t cp) noexcept;
bool raised_origin_glyph(const CharInfo& c) noexcept;

// Recursive XY-cut segmentation (layout_lines.cpp)
void split_region(std::vector<const CharInfo*> chars, int depth,
                  std::vector<std::vector<const CharInfo*>>& out);

// Baseline clustering and run assembly (layout_lines.cpp)
double column_gap_threshold(double size) noexcept;
void finalize_line(Line& line);
void build_runs(Line& line);
std::vector<Line> build_lines(std::span<const CharInfo* const> chars);

// Inline and 2-D math reconstruction (layout_math.cpp)
struct Emit {
    size_t idx = 0;
    bool space_before = false;
};

bool is_latin_letter_cp(char32_t cp) noexcept;
bool is_letter_cp(char32_t cp) noexcept;
int script_role(const Line& line, const CharInfo& c) noexcept;
std::vector<Emit> plan_line(Line& line);
void render_plain(Line& line, std::span<const Emit> emits);
std::u32string math_to_latex(std::vector<const CharInfo*> gl, double axis, bool top_level,
                             int depth = 0);
void render_math(Line& line, std::span<const Emit> emits);
void normalize_inline_punct(Line& line);
bool line_needs_math(const Line& line) noexcept;
void rewrite_footnote_refs(Line& line);
void repair_token_spaces(const Line& line, std::vector<Emit>& emits);

// Text-geometry table detection (layout_tables_text.cpp)
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
bool region_looks_tabular(std::span<const CharInfo* const> chars);

// Block assembly and display equations (layout_blocks.cpp)
void compute_lines_geometry(std::span<const Line> lines, Block& b);
void finalize_block_geometry(Block& b);
size_t count_glyphs(const Line& line) noexcept;
bool is_citation_lead(const Line& ln) noexcept;
void region_to_blocks(std::vector<Line> lines, int page, const AnalyzeParams& params,
                      double col_left, double body_size, double page_width,
                      std::vector<Block>& out);

// Document-level body font estimation and line-level chrome stripping (layout_passes.cpp)
double compute_body_size(const std::vector<PageData>& pages);

struct PageLines {
    int index = 0;
    double width = 0, height = 0;
    std::vector<std::vector<Line>> regions;
};

void strip_running_chrome(std::vector<PageLines>& pages);

// Ruled-lattice bordered grid recovery (layout_tables_ruled.cpp)
struct LatticeGroup {
    double y_top = 0;
    std::vector<Block> blocks;  // caption paragraph(s), if any, then the table
};

std::vector<LatticeGroup> detect_ruled_tables(const PageData& page, double body_size,
                                              std::vector<char>& consumed);

// Document-cleanup Strategy / NVI Template-Method pipeline (layout_passes.cpp)
struct PassContext {
    const std::vector<PageData>& pages;
    const AnalyzeParams& params;
};

// Polymorphic Strategy base class with NVI Template Method `run()`.
// Non-copyable and non-movable to prevent object slicing; owned via unique_ptr.
class DocumentPass {
public:
    DocumentPass(const DocumentPass&) = delete;
    DocumentPass& operator=(const DocumentPass&) = delete;
    DocumentPass(DocumentPass&&) = delete;
    DocumentPass& operator=(DocumentPass&&) = delete;
    virtual ~DocumentPass();

    void run(Document& doc, const PassContext& ctx) const {
        if (enabled(ctx.params)) {
            apply(doc, ctx);
        }
    }

protected:
    DocumentPass() noexcept = default;

    virtual bool enabled(const AnalyzeParams& params) const noexcept {
        (void)params;
        return true;
    }
    virtual void apply(Document& doc, const PassContext& ctx) const = 0;
};

std::vector<std::unique_ptr<DocumentPass>> build_pass_pipeline();

}  // namespace detail
}  // namespace pdf2md
