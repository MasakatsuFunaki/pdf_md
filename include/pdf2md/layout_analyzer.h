#pragma once

#include "pdf2md/document_model.h"
#include "pdf2md/options.h"

#include <memory>
#include <vector>

namespace pdf2md {

// Stage 2: reconstructs reading-order blocks, headings, lists, tables, math,
// and image placements from extracted page data. Insulated via PIMPL with a
// pre-built DocumentPass pipeline reused across const analyze() calls;
// concurrent const analyze() calls on independent inputs are thread-safe.
class LayoutAnalyzer {
public:
    explicit LayoutAnalyzer(AnalyzeParams params = {});
    LayoutAnalyzer(const LayoutAnalyzer& other);
    LayoutAnalyzer& operator=(const LayoutAnalyzer& other);
    LayoutAnalyzer(LayoutAnalyzer&& other) noexcept;
    LayoutAnalyzer& operator=(LayoutAnalyzer&& other) noexcept;
    ~LayoutAnalyzer();

    [[nodiscard]] Document analyze(const DocMeta& meta,
                                   const std::vector<PageData>& pages) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Thread-safe: pure function with zero shared mutable state; safe to invoke
// concurrently across threads on independent inputs.
Document analyze_layout(const DocMeta& meta, const std::vector<PageData>& pages,
                        const AnalyzeParams& params = {});

}  // namespace pdf2md
