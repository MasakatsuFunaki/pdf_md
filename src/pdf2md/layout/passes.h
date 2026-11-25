#pragma once

#include "pdf2md/document_model.h"
#include "pdf2md/options.h"

#include <memory>
#include <vector>

namespace pdf2md::layout {

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

    void run(Document& doc, const PassContext& ctx) const;

protected:
    DocumentPass() noexcept = default;

    virtual bool enabled(const AnalyzeParams& params) const noexcept;
    virtual void apply(Document& doc, const PassContext& ctx) const = 0;
};

std::vector<std::unique_ptr<DocumentPass>> build_pass_pipeline();

}  // namespace pdf2md::layout
