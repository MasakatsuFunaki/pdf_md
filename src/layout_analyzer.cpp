#include "layout_analyzer.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

#include "layout_internal.h"

namespace pdf2md {

using namespace detail;

Document analyze_layout(const DocMeta& meta, const std::vector<PageData>& pages,
                        const AnalyzeParams& params) {
    Document doc;
    doc.meta = meta;
    doc.body_size = compute_body_size(pages);

    std::vector<PageLines> page_lines;
    std::vector<std::vector<LatticeGroup>> lattice_groups(pages.size());
    page_lines.reserve(pages.size());
    for (size_t pi = 0; pi < pages.size(); ++pi) {
        const PageData& page = pages[pi];

        // Bordered-grid tables recovered from the ruling lattice are extracted
        // first; their characters are withheld from the text-geometry pipeline so
        // a heavily wrapped grid is not shredded by the column split.
        std::vector<char> consumed;
        if (params.detect_tables)
            lattice_groups[pi] = detect_ruled_tables(page, doc.body_size, consumed);

        std::vector<const CharInfo*> chars;
        chars.reserve(page.chars.size());
        for (size_t k = 0; k < page.chars.size(); ++k)
            if (consumed.empty() || !consumed[k]) chars.push_back(&page.chars[k]);

        std::vector<std::vector<const CharInfo*>> regions;
        split_region(std::move(chars), 0, regions);

        PageLines pl;
        pl.index = page.index;
        pl.width = page.width;
        pl.height = page.height;
        pl.regions.reserve(regions.size());
        for (auto& region : regions) pl.regions.push_back(build_lines(region));
        page_lines.push_back(std::move(pl));
    }

    // Remove running headers/footers at the line level before blocks form, so a
    // footer can never fuse with an adjacent caption or reference.
    if (params.strip_headers_footers) strip_running_chrome(page_lines);

    for (size_t pi = 0; pi < page_lines.size(); ++pi) {
        PageLines& pl = page_lines[pi];
        // The page text column's left edge: the leftmost start of the long
        // (body) lines. Display equations sit well to the right of it.
        double col_left = 1e30, any_left = 1e30;
        for (const auto& lines : pl.regions) {
            for (const Line& ln : lines) {
                any_left = std::min(any_left, ln.x0);
                if (count_glyphs(ln) >= 15) col_left = std::min(col_left, ln.x0);
            }
        }
        if (col_left > 1e29) col_left = any_left > 1e29 ? 0.0 : any_left;

        std::vector<Block> page_blocks;
        for (auto& lines : pl.regions)
            region_to_blocks(std::move(lines), pl.index, params, col_left, doc.body_size,
                             pl.width, page_blocks);

        // Splice ruled-grid tables into reading order by their top edge: each
        // group inserts ahead of the first text block that starts below it.
        for (auto& group : lattice_groups[pi]) {
            auto pos = page_blocks.end();
            for (auto it = page_blocks.begin(); it != page_blocks.end(); ++it)
                if (it->y_top < group.y_top) { pos = it; break; }
            page_blocks.insert(pos, std::make_move_iterator(group.blocks.begin()),
                               std::make_move_iterator(group.blocks.end()));
        }

        for (Block& b : page_blocks) doc.blocks.push_back(std::move(b));
    }

    // Document-level cleanup: a fixed Strategy pipeline over doc.blocks, gated
    // and ordered by build_pass_pipeline (see the pass-pipeline section above).
    const PassContext ctx{pages, params};
    for (const auto& pass : build_pass_pipeline()) pass->run(doc, ctx);
    return doc;
}

}  // namespace pdf2md
