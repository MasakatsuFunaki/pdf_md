#pragma once

#include "pdf2md/options.h"

#include <filesystem>
#include <set>
#include <string>
#include <string_view>

namespace pdf2md {

enum class ExitCode : int {
    Ok = 0,
    BadArgs = 1,
    PdfError = 2,
    IoError = 3,
};

struct CliRawArgs {
    std::string input;
    std::string output;
    std::string pages_spec;
    std::string password;
    std::string images_dir;
    bool no_images = false;
    bool page_breaks = false;
    bool front_matter = false;
    bool keep_headers = false;
    bool no_headings = false;
    bool no_tables = false;
    bool keep_rotated = false;
    bool no_links = false;
    bool toc = false;
    bool quiet = false;
};

struct ResolvedCliConfig {
    ExitCode status = ExitCode::Ok;
    std::string error;  // human-readable reason when status != ExitCode::Ok
    std::filesystem::path input_path;
    std::filesystem::path output_path;
    bool to_stdout = false;
    bool quiet = false;
    ConvertOptions options;
};

// Parses "1-3,7,12-14" (1-based) into a set of 0-based page indices.
bool parse_pages(std::string_view spec, std::set<int>& out);

// Validates raw CLI arguments against filesystem preconditions and resolves
// output paths, image directory/reference prefix, and pipeline options.
ResolvedCliConfig resolve_cli_config(const CliRawArgs& raw);

}  // namespace pdf2md
