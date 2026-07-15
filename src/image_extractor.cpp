#include "image_extractor.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

#include <cstdio>
#include <fstream>

namespace pdf2md {

namespace {

// Converts the PDFium bitmap into tightly packed RGB or RGBA rows.
// Returns channel count, or 0 if the format is unsupported.
int to_packed_rgb(FPDF_BITMAP bitmap, std::vector<unsigned char>& out) {
    const int w = FPDFBitmap_GetWidth(bitmap);
    const int h = FPDFBitmap_GetHeight(bitmap);
    const int stride = FPDFBitmap_GetStride(bitmap);
    const int format = FPDFBitmap_GetFormat(bitmap);
    const auto* buf = static_cast<const unsigned char*>(FPDFBitmap_GetBuffer(bitmap));
    if (!buf || w <= 0 || h <= 0) return 0;

    switch (format) {
        case FPDFBitmap_Gray: {
            out.resize(static_cast<size_t>(w) * h);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    out[static_cast<size_t>(y) * w + x] = buf[static_cast<size_t>(y) * stride + x];
            return 1;
        }
        case FPDFBitmap_BGR: {
            out.resize(static_cast<size_t>(w) * h * 3);
            for (int y = 0; y < h; ++y) {
                const unsigned char* row = buf + static_cast<size_t>(y) * stride;
                unsigned char* dst = out.data() + static_cast<size_t>(y) * w * 3;
                for (int x = 0; x < w; ++x) {
                    dst[3 * x + 0] = row[3 * x + 2];
                    dst[3 * x + 1] = row[3 * x + 1];
                    dst[3 * x + 2] = row[3 * x + 0];
                }
            }
            return 3;
        }
        case FPDFBitmap_BGRx: {
            out.resize(static_cast<size_t>(w) * h * 3);
            for (int y = 0; y < h; ++y) {
                const unsigned char* row = buf + static_cast<size_t>(y) * stride;
                unsigned char* dst = out.data() + static_cast<size_t>(y) * w * 3;
                for (int x = 0; x < w; ++x) {
                    dst[3 * x + 0] = row[4 * x + 2];
                    dst[3 * x + 1] = row[4 * x + 1];
                    dst[3 * x + 2] = row[4 * x + 0];
                }
            }
            return 3;
        }
        case FPDFBitmap_BGRA: {
            out.resize(static_cast<size_t>(w) * h * 4);
            for (int y = 0; y < h; ++y) {
                const unsigned char* row = buf + static_cast<size_t>(y) * stride;
                unsigned char* dst = out.data() + static_cast<size_t>(y) * w * 4;
                for (int x = 0; x < w; ++x) {
                    dst[4 * x + 0] = row[4 * x + 2];
                    dst[4 * x + 1] = row[4 * x + 1];
                    dst[4 * x + 2] = row[4 * x + 0];
                    dst[4 * x + 3] = row[4 * x + 3];
                }
            }
            return 4;
        }
        default:
            return 0;
    }
}

void stbi_write_to_ofstream(void* context, void* data, int size) {
    auto* os = static_cast<std::ofstream*>(context);
    os->write(static_cast<const char*>(data), size);
}

}  // namespace

namespace {

bool write_packed_png(const std::filesystem::path& out_path, int w, int h, int channels,
                      const unsigned char* data, int stride) {
    // Create the target directory on demand, so a conversion that turns out to
    // hold no images leaves no empty output directory behind.
    if (out_path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(out_path.parent_path(), ec);
    }

    std::ofstream os(out_path, std::ios::binary);
    if (!os) return false;
    int ok = stbi_write_png_to_func(stbi_write_to_ofstream, &os, w, h, channels, data, stride);
    os.close();  // flush now so a failure on the final buffer surfaces in good()
    return ok != 0 && os.good();
}

}  // namespace

bool write_bitmap_png(FPDF_BITMAP bitmap, const std::filesystem::path& out_path) {
    std::vector<unsigned char> pixels;
    const int channels = to_packed_rgb(bitmap, pixels);
    if (channels == 0) return false;

    const int w = FPDFBitmap_GetWidth(bitmap);
    const int h = FPDFBitmap_GetHeight(bitmap);
    return write_packed_png(out_path, w, h, channels, pixels.data(), w * channels);
}

bool write_scan_band_png(FPDF_BITMAP bitmap, const std::filesystem::path& out_path,
                         int pad, double min_ink_fraction) {
    std::vector<unsigned char> pixels;
    const int channels = to_packed_rgb(bitmap, pixels);
    if (channels == 0) return false;
    const int w = FPDFBitmap_GetWidth(bitmap);
    const int h = FPDFBitmap_GetHeight(bitmap);
    if (w <= 0 || h <= 0) return false;

    // Per-row ink counts at the render's 2x scale (so a text line is ~26px).
    std::vector<int> row_dark(static_cast<size_t>(h), 0);
    for (int y = 0; y < h; ++y) {
        const unsigned char* row = pixels.data() + static_cast<size_t>(y) * w * channels;
        for (int x = 0; x < w; ++x) {
            const unsigned char* px = row + x * channels;
            int lum = channels == 1 ? px[0] : (px[0] + px[1] + px[2]) / 3;
            if (lum < 200) ++row_dark[y];
        }
    }
    auto inky = [&](int y) { return row_dark[y] > 2; };

    // Trim a text-height inky run hugging an edge when a clear white gap
    // separates it from the rest -- a bled neighbour text line. Anything
    // taller, or fused with the body, is figure content and stays.
    constexpr int kEdge = 34, kMaxTextRun = 28, kMinWhiteGap = 12;
    int top = 0, bottom = h - 1;
    auto trim = [&](int from, int dir) {
        int r = from;
        while (r != from + dir * kEdge && r >= 0 && r < h && !inky(r)) r += dir;
        if (r < 0 || r >= h || !inky(r) || std::abs(r - from) >= kEdge) return from;
        int run_end = r;
        while (run_end + dir >= 0 && run_end + dir < h && inky(run_end + dir)) run_end += dir;
        if (std::abs(run_end - r) + 1 > kMaxTextRun) return from;
        for (int g = 1; g <= kMinWhiteGap; ++g) {
            int y = run_end + dir * g;
            if (y < 0 || y >= h || inky(y)) return from;
        }
        return run_end + dir * (kMinWhiteGap / 2);
    };
    top = trim(0, +1);
    bottom = trim(h - 1, -1);
    if (bottom <= top) return false;

    long dark = 0;
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = top; y <= bottom; ++y) {
        const unsigned char* row = pixels.data() + static_cast<size_t>(y) * w * channels;
        for (int x = 0; x < w; ++x) {
            const unsigned char* px = row + x * channels;
            int lum = channels == 1 ? px[0] : (px[0] + px[1] + px[2]) / 3;
            if (lum >= 200) continue;
            ++dark;
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
        }
    }
    if (x1 < 0) return false;  // blank
    if (static_cast<double>(dark) < min_ink_fraction * w * h) return false;

    x0 = std::max(0, x0 - pad);
    y0 = std::max(top, y0 - pad);
    x1 = std::min(w - 1, x1 + pad);
    y1 = std::min(bottom, y1 + pad);
    const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    const unsigned char* origin =
        pixels.data() + (static_cast<size_t>(y0) * w + x0) * channels;
    return write_packed_png(out_path, cw, ch, channels, origin, w * channels);
}

double bitmap_ink_fraction(FPDF_BITMAP bitmap) {
    std::vector<unsigned char> pixels;
    const int channels = to_packed_rgb(bitmap, pixels);
    if (channels == 0) return 0.0;
    const size_t count = pixels.size() / channels;
    if (count == 0) return 0.0;
    size_t dark = 0;
    for (size_t i = 0; i < count; ++i) {
        const unsigned char* px = pixels.data() + i * channels;
        int lum = channels == 1 ? px[0] : (px[0] + px[1] + px[2]) / 3;
        if (lum < 200) ++dark;
    }
    return static_cast<double>(dark) / static_cast<double>(count);
}

uint64_t hash_bitmap(FPDF_BITMAP bitmap) {
    const int h = FPDFBitmap_GetHeight(bitmap);
    const int stride = FPDFBitmap_GetStride(bitmap);
    const auto* buf = static_cast<const unsigned char*>(FPDFBitmap_GetBuffer(bitmap));
    uint64_t hash = 1469598103934665603ULL;
    if (!buf) return hash;
    const size_t total = static_cast<size_t>(h) * stride;
    for (size_t i = 0; i < total; ++i) {
        hash ^= buf[i];
        hash *= 1099511628211ULL;
    }
    hash ^= static_cast<uint64_t>(FPDFBitmap_GetWidth(bitmap)) << 32 | static_cast<uint32_t>(h);
    return hash;
}

}  // namespace pdf2md
