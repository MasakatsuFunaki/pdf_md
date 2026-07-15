// Regenerates the examples/ gallery by running the *actual* pdf2md executable
// over every fixture and corpus PDF. This exercises the whole shipped tool end
// to end (CLI parsing -> extract -> analyze -> write markdown) and leaves a
// browsable set of real conversions in examples/. It deliberately does not
// reuse artifacts produced by the other tests -- each .md here is what a user
// running `pdf2md <file>` would get.

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "fixture_pdfs.h"  // libharu generators for the synthetic input PDFs

#ifndef PDF2MD_EXAMPLES_DIR
#define PDF2MD_EXAMPLES_DIR ""
#endif
#ifndef PDF2MD_CORPUS_DIR
#define PDF2MD_CORPUS_DIR ""
#endif
#ifndef PDF2MD_CLI_EXE
#define PDF2MD_CLI_EXE ""
#endif

namespace {

namespace fs = std::filesystem;

std::string quote(const std::string& s) { return "\"" + s + "\""; }

// Invoke the built tool: pdf2md <pdf> -o <md> [--images <dir>] --quiet.
// Returns the process exit code (0 == success).
int run_pdf2md(const fs::path& pdf, const fs::path& md, const fs::path& image_dir) {
    fs::path exe(PDF2MD_CLI_EXE);
    exe.make_preferred();
    std::string cmd = quote(exe.string()) + " " + quote(pdf.string()) + " -o " +
                      quote(md.string()) + " --quiet";
    if (!image_dir.empty()) cmd += " --images " + quote(image_dir.string());
#ifdef _WIN32
    // std::system runs `cmd /c <cmd>`, which strips one layer of outer quotes;
    // wrap the whole command again so quoted paths survive.
    cmd = "\"" + cmd + "\"";
#endif
    return std::system(cmd.c_str());
}

// Drop previous inputs/conversions (keep the checked-in README) so the gallery
// is exactly the current set. Collect first, then remove: deleting entries while
// iterating the directory is undefined behaviour. Removal is best-effort -- a
// file locked by a viewer simply stays, and gets overwritten in place below.
void clean_examples(const fs::path& dir) {
    std::error_code ec;
    std::vector<fs::path> stale;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        const fs::path ext = e.path().extension();
        if ((ext == ".pdf" || ext == ".md") && e.path().filename() != "README.md")
            stale.push_back(e.path());
    }
    for (const auto& p : stale) fs::remove(p, ec);
    fs::remove_all(dir / "images", ec);  // extracted PNGs from a prior run
}

// One PDF to convert: `source` is the file we read (always available), `name`
// the output stem in examples/, `images` whether to run --images.
struct Input {
    fs::path source;
    std::string name;
    bool images;
};

}  // namespace

// Not a pure unit test: a one-shot generator that also smoke-tests the CLI on
// every sample PDF. Runs once per suite invocation (gtest_discover_tests gives
// it its own process), so the tool is spawned a dozen times, not once per test.
TEST(Examples, GenerateGalleryWithCli) {
    const fs::path dir(PDF2MD_EXAMPLES_DIR);
    const std::string cli = PDF2MD_CLI_EXE;
    ASSERT_FALSE(dir.empty()) << "PDF2MD_EXAMPLES_DIR not defined";
    ASSERT_FALSE(cli.empty()) << "PDF2MD_CLI_EXE not defined";
    ASSERT_TRUE(fs::exists(cli)) << "pdf2md executable not built: " << cli;

    std::error_code ec;
    fs::create_directories(dir, ec);
    clean_examples(dir);

    std::vector<Input> inputs;

    // Fixtures: synthetic PDFs generated straight into examples/ (best-effort --
    // if one is open in a viewer we keep the existing copy) and read from there.
    struct Fixture {
        const char* name;
        void (*make)(const fs::path&);
        bool images;
    };
    const Fixture fixtures[] = {
        {"basic", &pdf2md::testfx::make_basic, false},
        {"twocol", &pdf2md::testfx::make_twocol, false},
        {"table", &pdf2md::testfx::make_table, false},
        {"image", &pdf2md::testfx::make_image, true},  // has an embedded picture
    };
    for (const Fixture& f : fixtures) {
        const fs::path pdf = dir / (std::string(f.name) + ".pdf");
        try {
            f.make(pdf);
        } catch (...) {
            // Locked by a viewer? Fall back to converting whatever PDF is there.
        }
        inputs.push_back({pdf, f.name, f.images});
    }

    // Corpus: read every real-world PDF straight from the download cache (always
    // readable) and also mirror it into examples/ so the pair is browsable. The
    // copy is best-effort -- a viewer holding the example copy open must not stop
    // the .md from being generated from the cache original.
    const fs::path corpus(PDF2MD_CORPUS_DIR);
    if (!corpus.empty() && fs::exists(corpus, ec)) {
        for (const auto& e : fs::directory_iterator(corpus, ec)) {
            if (!e.is_regular_file() || e.path().extension() != ".pdf") continue;
            fs::copy_file(e.path(), dir / e.path().filename(),
                          fs::copy_options::overwrite_existing, ec);
            // Extract images for corpus PDFs too, so vector/raster figures land
            // in examples/images/ and the gallery embeds them.
            inputs.push_back({e.path(), e.path().stem().string(), true});
        }
    }

    // Run the real tool on each input; every conversion must succeed and leave a
    // non-empty .md in examples/.
    for (const Input& in : inputs) {
        const fs::path md = dir / (in.name + ".md");
        // Each document gets its own subdirectory under examples/images/: the CLI
        // numbers PNGs from image_1 per run, so a shared directory would let one
        // document's figures clobber another's.
        const int rc =
            run_pdf2md(in.source, md, in.images ? dir / "images" / in.name : fs::path{});
        EXPECT_EQ(rc, 0) << "pdf2md exited nonzero on " << in.name;
        EXPECT_TRUE(fs::exists(md) && fs::file_size(md, ec) > 0)
            << "no markdown produced for " << in.name;
    }
}
