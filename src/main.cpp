#include "converter.h"
#include "layout_analyzer.h"
#include "markdown_writer.h"
#include "pdf_extractor.h"

#include <CLI/CLI.hpp>

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr int kExitOk = 0;
constexpr int kExitBadArgs = 1;
constexpr int kExitPdfError = 2;
constexpr int kExitIoError = 3;

constexpr long long kMaxPageSpan = 100000LL;
constexpr long long kMaxPageNumber = 10000000LL;

fs::path path_from_utf8(std::string_view utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

// path::string() can throw for non-ANSI-representable paths on Windows;
// always display paths as UTF-8 instead.
std::string path_to_utf8(const fs::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// UTF-8 with forward slashes, for paths embedded in the markdown output.
std::string path_to_utf8_generic(const fs::path& p) {
    const std::u8string u8 = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

bool parse_positive_page(std::string_view text, long long& value) noexcept {
    if (text.empty()) {
        return false;
    }
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    long long parsed = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return false;
    }
    if (parsed < 1LL || parsed > kMaxPageNumber) {
        return false;
    }
    value = parsed;
    return true;
}

// Parses "1-3,7,12-14" (1-based) into a set of 0-based page indices.
bool parse_pages(std::string_view spec, std::set<int>& out) {
    if (spec.empty()) {
        return false;
    }
    std::size_t pos = 0U;
    while (pos <= spec.size()) {
        const std::size_t comma = spec.find(',', pos);
        const std::string_view token =
            comma == std::string_view::npos ? spec.substr(pos) : spec.substr(pos, comma - pos);
        if (token.empty()) {
            return false;
        }

        const std::size_t dash = token.find('-');
        if (dash == std::string_view::npos) {
            long long p = 0;
            if (!parse_positive_page(token, p)) {
                return false;
            }
            out.insert(static_cast<int>(p - 1LL));
        } else {
            long long a = 0;
            long long b = 0;
            if (!parse_positive_page(token.substr(0U, dash), a) ||
                !parse_positive_page(token.substr(dash + 1U), b)) {
                return false;
            }
            if (b < a || b - a >= kMaxPageSpan) {
                return false;
            }
            for (long long p = a; p <= b; ++p) {
                out.insert(static_cast<int>(p - 1LL));
            }
        }

        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1U;
    }
    return !out.empty();
}

bool write_output(bool to_stdout, const fs::path& output_path, std::string_view markdown) {
    if (to_stdout) {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        std::cout.write(markdown.data(), static_cast<std::streamsize>(markdown.size()));
        std::cout.flush();
        if (!std::cout.good()) {
            std::cerr << "pdf2md: failed writing to stdout\n";
            return false;
        }
        return true;
    }

    std::error_code ec;
    if (fs::is_directory(output_path, ec)) {
        std::cerr << "pdf2md: output path is a directory: " << path_to_utf8(output_path) << "\n";
        return false;
    }

    std::ofstream os(output_path, std::ios::binary);
    if (!os) {
        std::cerr << "pdf2md: cannot open output file for writing: " << path_to_utf8(output_path)
                  << "\n";
        return false;
    }
    os.write(markdown.data(), static_cast<std::streamsize>(markdown.size()));
    os.close();  // flush and surface any final write failure
    if (!os.good()) {
        fs::remove(output_path, ec);
        std::cerr << "pdf2md: failed writing output file: " << path_to_utf8(output_path) << "\n";
        return false;
    }
    return true;
}

template <typename CharT>
int run_app(int argc, const CharT* const* argv) {
    CLI::App app{"pdf2md - convert PDF documents to Markdown"};
    app.set_version_flag("--version", "pdf2md 0.1.0");

    std::string input;
    std::string output;
    std::string pages_spec;
    std::string password;
    std::string images_dir;
    bool page_breaks = false;
    bool front_matter = false;
    bool keep_headers = false;
    bool no_headings = false;
    bool no_tables = false;
    bool keep_rotated = false;
    bool quiet = false;
    bool no_images = false;

    app.add_option("input", input, "Input PDF file")->required();
    app.add_option("-o,--output", output,
                   "Output markdown file (default: input name with .md; '-' for stdout)");
    app.add_option("--pages", pages_spec, "Pages to convert, 1-based (e.g. 1-3,7)");
    app.add_option("--password", password, "Password for encrypted PDFs");
    app.add_option("--images", images_dir, "Extract images as PNG into this directory");
    app.add_flag("--no-images", no_images, "Do not extract or reference embedded images");
    app.add_flag("--page-breaks", page_breaks, "Emit a horizontal rule between pages");
    app.add_flag("--front-matter", front_matter, "Emit YAML front matter (title, source, pages)");
    app.add_flag("--keep-headers", keep_headers, "Keep repeated page headers/footers");
    app.add_flag("--no-headings", no_headings, "Do not infer headings from font sizes");
    app.add_flag("--no-tables", no_tables, "Do not infer tables from aligned columns");
    app.add_flag("--keep-rotated", keep_rotated, "Keep rotated text (watermarks etc.)");
    app.add_flag("-q,--quiet", quiet, "Suppress the summary message");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        const int rc = app.exit(e);
        return rc == 0 ? kExitOk : kExitBadArgs;
    }

    pdf2md::ExtractOptions ex_opts;
    ex_opts.password = std::move(password);
    ex_opts.keep_rotated = keep_rotated;
    if (!pages_spec.empty() && !parse_pages(pages_spec, ex_opts.pages)) {
        std::cerr << "pdf2md: invalid --pages value: " << pages_spec << "\n";
        return kExitBadArgs;
    }

    const fs::path input_path = path_from_utf8(input);
    std::error_code ec;
    if (!fs::exists(input_path, ec) || fs::is_directory(input_path, ec)) {
        std::cerr << "pdf2md: input file does not exist: " << input << "\n";
        return kExitPdfError;
    }

    fs::path output_path;
    const bool to_stdout = output == "-";
    if (!to_stdout) {
        output_path = output.empty() ? fs::path(input_path).replace_extension(".md")
                                     : path_from_utf8(output);
        if (fs::is_directory(output_path, ec)) {
            std::cerr << "pdf2md: output path is a directory: " << path_to_utf8(output_path)
                      << "\n";
            return kExitIoError;
        }
    }

    // Images are extracted by default unless --no-images is given or output is
    // streaming to stdout without an explicit --images directory.
    fs::path img_dir;
    if (!no_images) {
        if (!images_dir.empty()) {
            img_dir = path_from_utf8(images_dir);
        } else if (!to_stdout) {
            img_dir = output_path;
            img_dir.replace_extension();
            img_dir += "_images";
        }
    }
    if (!img_dir.empty()) {
        if (fs::exists(img_dir, ec) && !fs::is_directory(img_dir, ec)) {
            std::cerr << "pdf2md: image path is not a directory: " << path_to_utf8(img_dir) << "\n";
            return kExitIoError;
        }
        ex_opts.extract_images = true;
        ex_opts.image_dir = std::move(img_dir);
        std::string prefix;
        if (!to_stdout) {
            fs::path base = output_path.parent_path();
            if (base.empty()) {
                base = fs::current_path(ec);
            }
            const fs::path rel = fs::relative(ex_opts.image_dir, base, ec);
            prefix = (ec || rel.empty()) ? path_to_utf8_generic(ex_opts.image_dir)
                                         : path_to_utf8_generic(rel);
        } else {
            prefix = path_to_utf8_generic(ex_opts.image_dir);
        }
        if (!prefix.empty() && prefix.back() != '/') {
            prefix += '/';
        }
        ex_opts.image_ref_prefix = std::move(prefix);
    }

    pdf2md::ConvertOptions opts;
    opts.extract = std::move(ex_opts);
    opts.analyze.detect_headings = !no_headings;
    opts.analyze.detect_tables = !no_tables;
    opts.analyze.strip_headers_footers = !keep_headers;
    opts.write.page_breaks = page_breaks;
    opts.write.front_matter = front_matter;
    opts.write.source_name = path_to_utf8(input_path.filename());

    pdf2md::ConvertResult result;
    try {
        const pdf2md::Converter converter{std::move(opts)};
        result = converter.convert(input_path);
    } catch (const pdf2md::PdfError& e) {
        std::cerr << "pdf2md: " << e.what() << "\n";
        return kExitPdfError;
    }

    if (!write_output(to_stdout, output_path, result.markdown)) {
        return kExitIoError;
    }

    if (!quiet) {
        std::cerr << "pdf2md: converted " << result.page_count << " page(s), "
                  << result.block_count << " block(s)";
        if (!to_stdout) {
            std::cerr << " -> " << path_to_utf8(output_path);
        }
        std::cerr << "\n";
    }
    return kExitOk;
}

}  // namespace

#ifdef _WIN32
// wmain + CLI11's wide parse keep non-ASCII paths intact on Windows.
int wmain(int argc, wchar_t** argv) { return run_app(argc, argv); }
#else
int main(int argc, char** argv) { return run_app(argc, argv); }
#endif
