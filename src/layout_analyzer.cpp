#include "layout_analyzer.h"

#include "layout_internal.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

namespace pdf2md {

namespace {

// Minimum non-space glyphs for a line to count as a full body line when
// estimating the page text column's left margin.
constexpr size_t kBodyColumnMinGlyphs = 15;

}  // namespace

Document analyze_layout(const DocMeta& meta, const std::vector<PageData>& pages,
                        const AnalyzeParams& params) {
    Document doc;
    doc.meta = meta;
    doc.body_size = detail::compute_body_size(pages);

    std::vector<detail::PageLines> page_lines;
    std::vector<std::vector<detail::LatticeGroup>> lattice_groups(pages.size());
    page_lines.reserve(pages.size());
    for (size_t pi = 0; pi < pages.size(); ++pi) {
        const PageData& page = pages[pi];

        // Bordered-grid tables recovered from the ruling lattice are extracted
        // first; their characters are withheld from the text-geometry pipeline so
        // a heavily wrapped grid is not shredded by the column split.
        std::vector<char> consumed;
        if (params.detect_tables) {
            lattice_groups[pi] = detail::detect_ruled_tables(page, doc.body_size, consumed);
        }

        std::vector<const CharInfo*> chars;
        chars.reserve(page.chars.size());
        for (size_t k = 0; k < page.chars.size(); ++k) {
            if (consumed.empty() || !consumed[k]) {
                chars.push_back(&page.chars[k]);
            }
        }

        std::vector<std::vector<const CharInfo*>> regions;
        detail::split_region(std::move(chars), 0, regions);

        detail::PageLines pl;
        pl.index = page.index;
        pl.width = page.width;
        pl.height = page.height;
        pl.regions.reserve(regions.size());
        for (const auto& region : regions) {
            pl.regions.push_back(detail::build_lines(region));
        }
        page_lines.push_back(std::move(pl));
    }

    // Remove running headers/footers at the line level before blocks form, so a
    // footer can never fuse with an adjacent caption or reference.
    if (params.strip_headers_footers) {
        detail::strip_running_chrome(page_lines);
    }

    for (size_t pi = 0; pi < page_lines.size(); ++pi) {
        detail::PageLines& pl = page_lines[pi];
        // The page text column's left edge: the leftmost start of the long
        // (body) lines. Display equations sit well to the right of it.
        double col_left = detail::kCoordSentinel;
        double any_left = detail::kCoordSentinel;
        for (const auto& lines : pl.regions) {
            for (const Line& ln : lines) {
                any_left = std::min(any_left, ln.x0);
                if (detail::count_glyphs(ln) >= kBodyColumnMinGlyphs) {
                    col_left = std::min(col_left, ln.x0);
                }
            }
        }
        if (col_left > detail::kCoordSentinelThreshold) {
            col_left = any_left > detail::kCoordSentinelThreshold ? 0.0 : any_left;
        }

        std::vector<Block> page_blocks;
        page_blocks.reserve(pl.regions.size());
        for (auto& lines : pl.regions) {
            detail::region_to_blocks(std::move(lines), pl.index, params, col_left, doc.body_size,
                                     pl.width, page_blocks);
        }

        // Splice ruled-grid tables into reading order by their top edge: each
        // group inserts ahead of the first text block that starts below it.
        for (auto& group : lattice_groups[pi]) {
            auto pos = page_blocks.end();
            for (auto it = page_blocks.begin(); it != page_blocks.end(); ++it) {
                if (it->y_top < group.y_top) {
                    pos = it;
                    break;
                }
            }
            page_blocks.insert(pos, std::make_move_iterator(group.blocks.begin()),
                               std::make_move_iterator(group.blocks.end()));
        }

        doc.blocks.reserve(doc.blocks.size() + page_blocks.size());
        for (Block& b : page_blocks) {
            doc.blocks.push_back(std::move(b));
        }
    }

    // Document-level cleanup: a fixed Strategy pipeline over doc.blocks, gated
    // and ordered by build_pass_pipeline.
    const detail::PassContext ctx{pages, params};
    for (const auto& pass : detail::build_pass_pipeline()) {
        pass->run(doc, ctx);
    }
    return doc;
}

}  // namespace pdf2md
