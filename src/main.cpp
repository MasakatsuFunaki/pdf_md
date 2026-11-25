#include "pdf2md/cli_config.h"
#include "pdf2md/converter.h"
#include "pdf2md/utf8.h"

#include <CLI/CLI.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#ifndef PDF2MD_VERSION
#define PDF2MD_VERSION "0.2.0"
#endif

namespace fs = std::filesystem;

namespace {

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
        std::cerr << "pdf2md: output path is a directory: " << pdf2md::path_to_utf8(output_path)
                  << "\n";
        return false;
    }

    std::ofstream os(output_path, std::ios::binary);
    if (!os) {
        std::cerr << "pdf2md: cannot open output file for writing: "
                  << pdf2md::path_to_utf8(output_path) << "\n";
        return false;
    }
    os.write(markdown.data(), static_cast<std::streamsize>(markdown.size()));
    os.close();  // flush and surface any final write failure
    if (!os.good()) {
        fs::remove(output_path, ec);
        std::cerr << "pdf2md: failed writing output file: " << pdf2md::path_to_utf8(output_path)
                  << "\n";
        return false;
    }
    return true;
}

template <typename CharT>
int run_app(int argc, const CharT* const* argv) {
    CLI::App app{"pdf2md - convert PDF documents to Markdown"};
    app.set_version_flag("--version", std::string("pdf2md ") + PDF2MD_VERSION);

    pdf2md::CliRawArgs raw;
    app.add_option("input", raw.input, "Input PDF file")->required();
    app.add_option("-o,--output", raw.output,
                   "Output markdown file (default: input name with .md; '-' for stdout)");
    app.add_option("--pages", raw.pages_spec, "Pages to convert, 1-based (e.g. 1-3,7)");
    app.add_option("--password", raw.password, "Password for encrypted PDFs");
    auto* images_opt =
        app.add_option("--images", raw.images_dir,
                       "Directory for extracted PNG images (default: <output_stem>_images)");
    auto* no_images_flag =
        app.add_flag("--no-images", raw.no_images, "Do not extract or reference embedded images");
    images_opt->excludes(no_images_flag);
    app.add_flag("--page-breaks", raw.page_breaks, "Emit a horizontal rule between pages");
    app.add_flag("--front-matter", raw.front_matter,
                 "Emit YAML front matter (title, author, subject, keywords, dates, source, pages)");
    app.add_flag("--keep-headers", raw.keep_headers, "Keep repeated page headers/footers");
    app.add_flag("--no-headings", raw.no_headings,
                 "Do not infer headings from font sizes or outlines");
    app.add_flag("--no-tables", raw.no_tables,
                 "Do not infer tables from aligned columns or ruled grids");
    app.add_flag("--keep-rotated", raw.keep_rotated, "Keep rotated text (watermarks etc.)");
    app.add_flag("--no-links", raw.no_links, "Do not extract or emit hyperlinks");
    app.add_flag("--toc", raw.toc, "Emit a Markdown table of contents from headings");
    app.add_flag("-q,--quiet", raw.quiet, "Suppress the summary message");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        const int rc = app.exit(e);
        return rc == 0 ? static_cast<int>(pdf2md::ExitCode::Ok)
                       : static_cast<int>(pdf2md::ExitCode::BadArgs);
    }

    const pdf2md::ResolvedCliConfig cfg = pdf2md::resolve_cli_config(raw);
    if (cfg.status != pdf2md::ExitCode::Ok) {
        std::cerr << "pdf2md: " << cfg.error << "\n";
        return static_cast<int>(cfg.status);
    }

    pdf2md::ConvertResult result;
    try {
        const pdf2md::Converter converter{cfg.options};
        result = converter.convert(cfg.input_path);
    } catch (const pdf2md::PdfError& e) {
        std::cerr << "pdf2md: " << e.what() << "\n";
        return static_cast<int>(pdf2md::ExitCode::PdfError);
    }

    if (!write_output(cfg.to_stdout, cfg.output_path, result.markdown)) {
        return static_cast<int>(pdf2md::ExitCode::IoError);
    }

    if (!cfg.quiet) {
        std::cerr << "pdf2md: converted " << result.page_count << " page(s), "
                  << result.block_count << " block(s)";
        if (!cfg.to_stdout) {
            std::cerr << " -> " << pdf2md::path_to_utf8(cfg.output_path);
        }
        std::cerr << "\n";
    }
    return static_cast<int>(pdf2md::ExitCode::Ok);
}

}  // namespace

#ifdef _WIN32
// wmain + CLI11's wide parse keep non-ASCII paths intact on Windows.
int wmain(int argc, wchar_t** argv) { return run_app(argc, argv); }
#else
int main(int argc, char** argv) { return run_app(argc, argv); }
#endif
