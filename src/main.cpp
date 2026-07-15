#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

#include <CLI/CLI.hpp>

#include "converter.h"
#include "layout_analyzer.h"
#include "markdown_writer.h"
#include "pdf_extractor.h"

namespace fs = std::filesystem;

namespace {

fs::path path_from_utf8(const std::string& utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

// path::string() can throw for non-ANSI-representable paths on Windows;
// always display paths as UTF-8 instead.
std::string path_to_utf8(const fs::path& p) {
    std::u8string u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

// UTF-8 with forward slashes, for paths embedded in the markdown output.
std::string path_to_utf8_generic(const fs::path& p) {
    std::u8string u8 = p.generic_u8string();
    return std::string(u8.begin(), u8.end());
}

// Parses "1-3,7,12-14" (1-based) into a set of 0-based page indices.
bool parse_pages(const std::string& spec, std::set<int>& out) {
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t comma = spec.find(',', pos);
        std::string token = spec.substr(pos, comma == std::string::npos ? comma : comma - pos);
        pos = comma == std::string::npos ? spec.size() : comma + 1;
        if (token.empty()) continue;

        size_t dash = token.find('-');
        try {
            if (dash == std::string::npos) {
                int p = std::stoi(token);
                if (p < 1) return false;
                out.insert(p - 1);
            } else {
                long long a = std::stoll(token.substr(0, dash));
                long long b = std::stoll(token.substr(dash + 1));
                // Bound the expansion; no real document needs more.
                if (a < 1 || b < a || b - a >= 100000 || b > 10000000) return false;
                for (long long p = a; p <= b; ++p) out.insert(static_cast<int>(p - 1));
            }
        } catch (const std::exception&) {
            return false;
        }
    }
    return !out.empty();
}

}  // namespace

template <typename CharT>
int run_app(int argc, const CharT* const* argv) {
    CLI::App app{"pdf2md - convert PDF documents to Markdown"};
    app.set_version_flag("--version", "pdf2md 0.1.0");

    std::string input, output, pages_spec, password, images_dir;
    bool page_breaks = false, front_matter = false, keep_headers = false;
    bool no_headings = false, no_tables = false, keep_rotated = false, quiet = false;
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
        return app.exit(e);
    }

    fs::path input_path = path_from_utf8(input);
    if (!fs::exists(input_path)) {
        std::cerr << "pdf2md: input file does not exist: " << input << "\n";
        return 2;
    }

    fs::path output_path;
    bool to_stdout = output == "-";
    if (!to_stdout) {
        output_path = output.empty() ? fs::path(input_path).replace_extension(".md")
                                     : path_from_utf8(output);
    }

    pdf2md::ExtractOptions ex_opts;
    ex_opts.password = password;
    ex_opts.keep_rotated = keep_rotated;
    if (!pages_spec.empty() && !parse_pages(pages_spec, ex_opts.pages)) {
        std::cerr << "pdf2md: invalid --pages value: " << pages_spec << "\n";
        return 1;
    }
    // Images are extracted by default so a document's figures survive into the
    // markdown as ![](...) embeds. An explicit --images picks the target
    // directory; otherwise one is derived from the output file's name (e.g.
    // "paper.md" -> "paper_images/"). Streaming to stdout has no such anchor to
    // hang a relative path off, so it needs an explicit --images to enable them;
    // --no-images turns the whole step off.
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
        ex_opts.extract_images = true;
        ex_opts.image_dir = img_dir;
        // Reference images relative to the markdown file when possible.
        std::string prefix;
        if (!to_stdout) {
            std::error_code ec;
            fs::path base = output_path.parent_path();
            if (base.empty()) base = fs::current_path(ec);
            fs::path rel = fs::relative(ex_opts.image_dir, base, ec);
            prefix = (ec || rel.empty()) ? path_to_utf8_generic(ex_opts.image_dir)
                                         : path_to_utf8_generic(rel);
        } else {
            prefix = path_to_utf8_generic(ex_opts.image_dir);
        }
        if (!prefix.empty() && prefix.back() != '/') prefix += '/';
        ex_opts.image_ref_prefix = prefix;
    }

    pdf2md::ConvertOptions opts;
    opts.extract = ex_opts;
    opts.analyze.detect_headings = !no_headings;
    opts.analyze.detect_tables = !no_tables;
    opts.analyze.strip_headers_footers = !keep_headers;
    opts.write.page_breaks = page_breaks;
    opts.write.front_matter = front_matter;
    opts.write.source_name = path_to_utf8(input_path.filename());

    pdf2md::ConvertResult result;
    try {
        result = pdf2md::Converter{opts}.convert(input_path);
    } catch (const pdf2md::PdfError& e) {
        std::cerr << "pdf2md: " << e.what() << "\n";
        return 2;
    }
    std::string markdown = std::move(result.markdown);

    if (to_stdout) {
        std::cout << markdown;
    } else {
        std::ofstream os(output_path, std::ios::binary);
        if (!os) {
            std::cerr << "pdf2md: cannot open output file for writing\n";
            return 3;
        }
        os << markdown;
        os.close();  // flush and surface any final write failure
        if (!os.good()) {
            std::cerr << "pdf2md: failed writing output file\n";
            return 3;
        }
    }

    if (!quiet) {
        std::cerr << "pdf2md: converted " << result.page_count << " page(s), "
                  << result.block_count << " block(s)";
        if (!to_stdout) std::cerr << " -> " << path_to_utf8(output_path);
        std::cerr << "\n";
    }
    return 0;
}

#ifdef _WIN32
// wmain + CLI11's wide parse keep non-ASCII paths intact on Windows.
int wmain(int argc, wchar_t** argv) { return run_app(argc, argv); }
#else
int main(int argc, char** argv) { return run_app(argc, argv); }
#endif
