#pragma once

#include <filesystem>

#include <fpdfview.h>

namespace pdf2md {

// Writes a PDFium bitmap as a PNG file. Returns false on failure.
bool write_bitmap_png(FPDF_BITMAP bitmap, const std::filesystem::path& out_path);

// FNV-1a hash of the bitmap pixel data, used to deduplicate repeated images.
uint64_t hash_bitmap(FPDF_BITMAP bitmap);

// Fraction of pixels darker than a near-white threshold -- how much "ink" the
// bitmap carries. Used to tell a scanned diagram crop from blank paper.
double bitmap_ink_fraction(FPDF_BITMAP bitmap);

// Writes a scanned-band crop: first trims away any text-line-height run of ink
// hugging the top or bottom edge that a clear white gap separates from the rest
// (the neighbouring text line bleeding into the band -- the OCR geometry the
// band was cut by sits a few points off the print), then crops to the ink
// bounding box plus `pad` pixels. Returns false (writing nothing) when what
// remains is blank or under `min_ink_fraction` -- blank paper is not a figure.
bool write_scan_band_png(FPDF_BITMAP bitmap, const std::filesystem::path& out_path,
                         int pad, double min_ink_fraction);

}  // namespace pdf2md
