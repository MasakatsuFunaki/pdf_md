#pragma once

#include <filesystem>

// libharu-based generators for the PDFs used by the test suite.
namespace pdf2md::testfx {

// Three pages: title, headings, styled runs, lists, code block, hyphenated
// lines, a cross-page paragraph, and repeated headers/footers on every page.
void make_basic(const std::filesystem::path& out);

// One page with a full-width title above two columns of prose.
void make_twocol(const std::filesystem::path& out);

// One page with a title, a 3-column table and a trailing paragraph.
void make_table(const std::filesystem::path& out);

// One page with a paragraph and an embedded raw RGB image.
void make_image(const std::filesystem::path& out);

// Nine pages exercising position-anchored running-head stripping: pages 1-4
// carry a chapter-scoped running head at a fixed top-margin position (too few
// pages for the document-wide repeat threshold), pages 5-8 a borderless table
// whose header row repeats at a fixed top-margin position with data rows right
// beneath it (must survive), page 9 is plain. Every page has unique body text.
void make_runheads(const std::filesystem::path& out);

// Generates all fixtures into a directory (created if needed) and returns it.
std::filesystem::path generate_all(const std::filesystem::path& dir);

}  // namespace pdf2md::testfx
