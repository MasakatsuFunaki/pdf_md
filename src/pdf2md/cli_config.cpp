#include "pdf2md/cli_config.h"

#include "pdf2md/utf8.h"

#include <charconv>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace pdf2md {

namespace {

namespace fs = std::filesystem;

constexpr long long kMaxPageSpan = 100000LL;
constexpr long long kMaxPageNumber = 10000000LL;

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

}  // namespace

bool parse_pages(std::string_view spec, std::set<int>& out) {
    if (spec.empty()) {
        return false;
    }
    std::set<int> parsed;
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
            parsed.insert(static_cast<int>(p - 1LL));
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
                parsed.insert(static_cast<int>(p - 1LL));
            }
        }

        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1U;
    }
    if (parsed.empty()) {
        return false;
    }
    out = std::move(parsed);
    return true;
}

ResolvedCliConfig resolve_cli_config(const CliRawArgs& raw) {
    ResolvedCliConfig cfg;
    cfg.quiet = raw.quiet;

    if (raw.no_images && !raw.images_dir.empty()) {
        cfg.status = ExitCode::BadArgs;
        cfg.error = "--images and --no-images cannot be used together";
        return cfg;
    }

    ExtractOptions ex_opts;
    ex_opts.password = raw.password;
    ex_opts.keep_rotated = raw.keep_rotated;
    ex_opts.extract_links = !raw.no_links;
    if (!raw.pages_spec.empty() && !parse_pages(raw.pages_spec, ex_opts.pages)) {
        cfg.status = ExitCode::BadArgs;
        cfg.error = "invalid --pages value: " + raw.pages_spec;
        return cfg;
    }

    cfg.input_path = path_from_utf8(raw.input);
    std::error_code ec;
    if (!fs::exists(cfg.input_path, ec) || fs::is_directory(cfg.input_path, ec)) {
        cfg.status = ExitCode::PdfError;
        cfg.error = "input file does not exist: " + raw.input;
        return cfg;
    }

    cfg.to_stdout = (raw.output == "-");
    if (!cfg.to_stdout) {
        cfg.output_path = raw.output.empty() ? fs::path(cfg.input_path).replace_extension(".md")
                                             : path_from_utf8(raw.output);
        if (fs::is_directory(cfg.output_path, ec)) {
            cfg.status = ExitCode::IoError;
            cfg.error = "output path is a directory: " + path_to_utf8(cfg.output_path);
            return cfg;
        }
    }

    // Images are extracted by default unless --no-images is given or output is
    // streaming to stdout without an explicit --images directory.
    fs::path img_dir;
    if (!raw.no_images) {
        if (!raw.images_dir.empty()) {
            img_dir = path_from_utf8(raw.images_dir);
        } else if (!cfg.to_stdout) {
            img_dir = cfg.output_path;
            img_dir.replace_extension();
            img_dir += "_images";
        }
    }
    if (!img_dir.empty()) {
        if (fs::exists(img_dir, ec) && !fs::is_directory(img_dir, ec)) {
            cfg.status = ExitCode::IoError;
            cfg.error = "image path is not a directory: " + path_to_utf8(img_dir);
            return cfg;
        }
        ex_opts.extract_images = true;
        ex_opts.image_dir = std::move(img_dir);
        std::string prefix;
        if (!cfg.to_stdout) {
            fs::path base = cfg.output_path.parent_path();
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

    cfg.options.extract = std::move(ex_opts);
    cfg.options.analyze.detect_headings = !raw.no_headings;
    cfg.options.analyze.detect_tables = !raw.no_tables;
    cfg.options.analyze.strip_headers_footers = !raw.keep_headers;
    cfg.options.write.page_breaks = raw.page_breaks;
    cfg.options.write.front_matter = raw.front_matter;
    cfg.options.write.source_name = path_to_utf8(cfg.input_path.filename());
    cfg.options.write.links = !raw.no_links;
    cfg.options.write.toc = raw.toc;
    return cfg;
}

}  // namespace pdf2md
