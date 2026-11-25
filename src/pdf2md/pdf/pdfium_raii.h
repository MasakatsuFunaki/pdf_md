#pragma once

#include "pdf2md/image_extractor.h"

#include <fpdf_annot.h>
#include <fpdf_structtree.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <type_traits>

namespace pdf2md::pdf {

// Process-wide PDFium lifecycle and serialization guard (Meyers Singleton).
// C++11 static initialization guarantees one-time thread-safe FPDF_InitLibrary,
// and mutex() serializes calls into PDFium's non-thread-safe global C state
// (including FPDF_GetLastError).
class PdfiumInit {
public:
    static PdfiumInit& instance() noexcept {
        static PdfiumInit init;
        return init;
    }

    PdfiumInit(const PdfiumInit&) = delete;
    PdfiumInit& operator=(const PdfiumInit&) = delete;
    PdfiumInit(PdfiumInit&&) = delete;
    PdfiumInit& operator=(PdfiumInit&&) = delete;

    [[nodiscard]] std::mutex& mutex() noexcept { return mutex_; }

private:
    PdfiumInit() noexcept { FPDF_InitLibrary(); }
    ~PdfiumInit() { FPDF_DestroyLibrary(); }

    std::mutex mutex_;
};

// RAII owners for PDFium C handles. Destruction runs in reverse construction
// order, releasing child handles before pages, and pages before the document.
struct FpdfDocumentDeleter {
    void operator()(FPDF_DOCUMENT h) const noexcept { FPDF_CloseDocument(h); }
};
struct FpdfPageDeleter {
    void operator()(FPDF_PAGE h) const noexcept { FPDF_ClosePage(h); }
};
struct FpdfTextPageDeleter {
    void operator()(FPDF_TEXTPAGE h) const noexcept { FPDFText_ClosePage(h); }
};
struct FpdfBitmapDeleter {
    void operator()(FPDF_BITMAP h) const noexcept { FPDFBitmap_Destroy(h); }
};
struct FpdfPageLinkDeleter {
    void operator()(FPDF_PAGELINK h) const noexcept { FPDFLink_CloseWebLinks(h); }
};
struct FpdfAnnotationDeleter {
    void operator()(FPDF_ANNOTATION h) const noexcept { FPDFPage_CloseAnnot(h); }
};
struct FpdfStructTreeDeleter {
    void operator()(FPDF_STRUCTTREE h) const noexcept { FPDF_StructTree_Close(h); }
};

using DocumentHandle = std::unique_ptr<std::remove_pointer_t<FPDF_DOCUMENT>, FpdfDocumentDeleter>;
using PageHandle = std::unique_ptr<std::remove_pointer_t<FPDF_PAGE>, FpdfPageDeleter>;
using TextPageHandle = std::unique_ptr<std::remove_pointer_t<FPDF_TEXTPAGE>, FpdfTextPageDeleter>;
using BitmapHandle = std::unique_ptr<std::remove_pointer_t<FPDF_BITMAP>, FpdfBitmapDeleter>;
using PageLinkHandle = std::unique_ptr<std::remove_pointer_t<FPDF_PAGELINK>, FpdfPageLinkDeleter>;
using AnnotationHandle =
    std::unique_ptr<std::remove_pointer_t<FPDF_ANNOTATION>, FpdfAnnotationDeleter>;
using StructTreeHandle =
    std::unique_ptr<std::remove_pointer_t<FPDF_STRUCTTREE>, FpdfStructTreeDeleter>;

inline BitmapView make_bitmap_view(FPDF_BITMAP bitmap) noexcept {
    if (bitmap == nullptr) {
        return {};
    }
    const auto* buf = static_cast<const std::uint8_t*>(FPDFBitmap_GetBuffer(bitmap));
    const int width = FPDFBitmap_GetWidth(bitmap);
    const int height = FPDFBitmap_GetHeight(bitmap);
    const int stride = FPDFBitmap_GetStride(bitmap);
    if (buf == nullptr || width <= 0 || height <= 0 || stride <= 0) {
        return {};
    }
    PixelFormat fmt = PixelFormat::BGRA;
    switch (FPDFBitmap_GetFormat(bitmap)) {
        case FPDFBitmap_Gray:
            fmt = PixelFormat::Gray;
            break;
        case FPDFBitmap_BGR:
            fmt = PixelFormat::BGR;
            break;
        case FPDFBitmap_BGRx:
            fmt = PixelFormat::BGRx;
            break;
        case FPDFBitmap_BGRA:
            fmt = PixelFormat::BGRA;
            break;
        default:
            return {};
    }
    return BitmapView{buf, width, height, stride, fmt};
}

inline RasterImage copy_bitmap_raster(FPDF_BITMAP bitmap) {
    const BitmapView src = make_bitmap_view(bitmap);
    if (src.data == nullptr) {
        return {};
    }
    const int bpp = bytes_per_pixel(src.format);
    if (bpp <= 0 ||
        static_cast<std::int64_t>(src.stride) < static_cast<std::int64_t>(src.width) * bpp) {
        return {};
    }
    RasterImage dst(src.width, src.height, src.format);
    if (dst.empty()) {
        return {};
    }
    const auto row_bytes = static_cast<std::size_t>(src.width) * static_cast<std::size_t>(bpp);
    for (int y = 0; y < src.height; ++y) {
        const std::uint8_t* src_row =
            src.data + static_cast<std::size_t>(y) * static_cast<std::size_t>(src.stride);
        std::uint8_t* dst_row =
            dst.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(dst.stride);
        std::copy_n(src_row, row_bytes, dst_row);
    }
    return dst;
}

}  // namespace pdf2md::pdf
