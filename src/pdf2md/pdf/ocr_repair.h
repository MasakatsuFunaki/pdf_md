#pragma once

#include "pdf2md/document_model.h"

#include <vector>

namespace pdf2md::pdf {

// Detects scanned-page OCR text layers (scaled degenerate font sizes) and
// repairs glyph sizes and stream-order word-break space positions in place.
void repair_ocr_text_layer(PageData& page_data);

// Repairs pages where nominal font sizes disagree with drawn loose_height
// by rescaling inflated glyphs against the document's median healthy ratio.
void repair_inflated_font_sizes(std::vector<PageData>& pages);

}  // namespace pdf2md::pdf
