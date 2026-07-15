// End-to-end tests: libharu-generated PDFs through extract -> analyze -> write.

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "fixture_pdfs.h"
#include "layout_analyzer.h"
#include "markdown_writer.h"
#include "pdf_extractor.h"

namespace fs = std::filesystem;
using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::StartsWith;

#ifndef PDF2MD_TEST_OUTPUT_DIR
#define PDF2MD_TEST_OUTPUT_DIR ""
#endif
#ifndef PDF2MD_CLI_EXE
#define PDF2MD_CLI_EXE ""
#endif

namespace {

// Runs the built pdf2md executable with `args` appended after the exe path.
// Mirrors examples_export.cpp's quoting so paths with spaces survive cmd /c.
int run_cli(const std::string& args) {
    fs::path exe(PDF2MD_CLI_EXE);
    exe.make_preferred();
    std::string cmd = "\"" + exe.string() + "\" " + args;
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";
#endif
    return std::system(cmd.c_str());
}

std::string read_file(const fs::path& p) {
    std::ifstream is(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
}

// Fixture PDFs and every converted .md land here (build/test_output) so a
// test run leaves inspectable artifacts.
const fs::path& output_dir() {
    static const fs::path dir = [] {
        fs::path d(PDF2MD_TEST_OUTPUT_DIR);
        if (d.empty()) d = fs::path(::testing::TempDir()) / "pdf2md_test_output";
        fs::create_directories(d);
        return d;
    }();
    return dir;
}

const fs::path& fixture_dir() {
    static const fs::path dir = pdf2md::testfx::generate_all(output_dir() / "fixtures");
    return dir;
}

// `variant` distinguishes the .md dumped by option-variant conversions from
// the default one (e.g. basic.keep-headers.md next to basic.md).
std::string convert(const std::string& name,
                    pdf2md::ExtractOptions ex_opts = {},
                    pdf2md::AnalyzeParams params = {},
                    pdf2md::WriteOptions w_opts = {},
                    const std::string& variant = "") {
    pdf2md::Extraction ex = pdf2md::extract_pdf(fixture_dir() / name, ex_opts);
    pdf2md::Document doc = pdf2md::analyze_layout(ex.meta, ex.pages, params);
    std::string md = pdf2md::write_markdown(doc, w_opts);

    std::string out_name = fs::path(name).stem().string() +
                           (variant.empty() ? "" : "." + variant) + ".md";
    std::ofstream(fixture_dir() / out_name, std::ios::binary) << md;
    return md;
}

TEST(BasicFixture, HeadingTiersFromFontSizes) {
    std::string md = convert("basic.pdf");
    EXPECT_THAT(md, HasSubstr("# Sample Document Title\n"));
    EXPECT_THAT(md, HasSubstr("## First Section\n"));
    EXPECT_THAT(md, HasSubstr("## Lists\n"));
    EXPECT_THAT(md, HasSubstr("## Code Example\n"));
}

TEST(BasicFixture, WrappedLinesJoinIntoOneParagraph) {
    std::string md = convert("basic.pdf");
    EXPECT_THAT(md, HasSubstr("This is the first paragraph of the sample document. "
                              "It spans multiple lines and should be joined into a single "
                              "markdown paragraph without unwanted line breaks."));
}

TEST(BasicFixture, InlineBoldAndItalic) {
    std::string md = convert("basic.pdf");
    EXPECT_THAT(md, HasSubstr("**bold text**"));
    EXPECT_THAT(md, HasSubstr("*italic text*"));
    EXPECT_THAT(md, HasSubstr("detect **bold text** and also *italic text* inside"));
}

TEST(BasicFixture, BulletListWithNesting) {
    std::string md = convert("basic.pdf");
    EXPECT_THAT(md, HasSubstr("- First bullet item\n"));
    EXPECT_THAT(md, HasSubstr("- Second bullet item\n"));
    EXPECT_THAT(md, HasSubstr("    - Nested bullet item\n"));
    EXPECT_THAT(md, HasSubstr("- Third bullet item\n"));
}

TEST(BasicFixture, OrderedListKeepsNumbers) {
    std::string md = convert("basic.pdf");
    EXPECT_THAT(md, HasSubstr("1. Alpha step\n"));
    EXPECT_THAT(md, HasSubstr("2. Beta step\n"));
    EXPECT_THAT(md, HasSubstr("3. Gamma step\n"));
}

TEST(BasicFixture, MonospaceBecomesFencedCode) {
    std::string md = convert("basic.pdf");
    EXPECT_THAT(md, HasSubstr("```\nint main() {\n    return 42;\n}\n```"));
}

TEST(BasicFixture, DehyphenationRejoinsWords) {
    std::string md = convert("basic.pdf");
    EXPECT_THAT(md, HasSubstr("exercises the hyphenation repair logic"));
    EXPECT_THAT(md, HasSubstr("the same paragraph in the converter."));
}

TEST(BasicFixture, ParagraphMergesAcrossPages) {
    std::string md = convert("basic.pdf");
    EXPECT_THAT(md, HasSubstr("split across the page boundary, which is exactly what "
                              "this final sentence verifies."));
}

TEST(BasicFixture, RepeatedHeadersAndFootersStripped) {
    std::string md = convert("basic.pdf");
    EXPECT_THAT(md, Not(HasSubstr("PDF2MD Fixture Suite")));
    EXPECT_THAT(md, Not(HasSubstr("Page 2")));
}

TEST(BasicFixture, KeepHeadersOptionPreservesThem) {
    pdf2md::AnalyzeParams params;
    params.strip_headers_footers = false;
    std::string md = convert("basic.pdf", {}, params, {}, "keep-headers");
    EXPECT_THAT(md, HasSubstr("PDF2MD Fixture Suite"));
    EXPECT_THAT(md, HasSubstr("Page 2"));
}

TEST(BasicFixture, NoHeadingsOptionDisablesInference) {
    pdf2md::AnalyzeParams params;
    params.detect_headings = false;
    std::string md = convert("basic.pdf", {}, params, {}, "no-headings");
    EXPECT_THAT(md, Not(HasSubstr("# Sample Document Title")));
    EXPECT_THAT(md, HasSubstr("Sample Document Title"));
}

TEST(BasicFixture, FrontMatterFromMetadata) {
    pdf2md::WriteOptions w;
    w.front_matter = true;
    w.source_name = "basic.pdf";
    std::string md = convert("basic.pdf", {}, {}, w, "front-matter");
    EXPECT_THAT(md, StartsWith("---\ntitle: \"Basic Fixture Document\"\n"
                               "author: \"pdf2md tests\"\n"
                               "source: \"basic.pdf\"\npages: 3\n---"));
}

TEST(BasicFixture, PageBreaksOptionEmitsRules) {
    pdf2md::WriteOptions w;
    w.page_breaks = true;
    std::string md = convert("basic.pdf", {}, {}, w, "page-breaks");
    EXPECT_THAT(md, HasSubstr("\n\n---\n\n"));
}

TEST(BasicFixture, PageFilterLimitsExtraction) {
    pdf2md::ExtractOptions ex;
    ex.pages = {0};
    std::string md = convert("basic.pdf", ex, {}, {}, "page1-only");
    EXPECT_THAT(md, HasSubstr("Sample Document Title"));
    EXPECT_THAT(md, Not(HasSubstr("Hyphenation")));
}

TEST(BasicFixture, MetadataIsExtracted) {
    pdf2md::Extraction ex = pdf2md::extract_pdf(fixture_dir() / "basic.pdf", {});
    EXPECT_EQ(ex.meta.title, "Basic Fixture Document");
    EXPECT_EQ(ex.meta.author, "pdf2md tests");
    EXPECT_EQ(ex.meta.page_count, 3);
    ASSERT_EQ(ex.pages.size(), 3u);
}

TEST(TwoColumnFixture, LeftColumnReadsBeforeRight) {
    std::string md = convert("twocol.pdf");
    size_t left_end = md.find("any right column text appears.");
    size_t right_start = md.find("Right column sentence one");
    ASSERT_NE(left_end, std::string::npos);
    ASSERT_NE(right_start, std::string::npos);
    EXPECT_LT(left_end, right_start);
}

TEST(TwoColumnFixture, ColumnsJoinAsSingleParagraphs) {
    std::string md = convert("twocol.pdf");
    EXPECT_THAT(md, HasSubstr("Left column sentence one flows across several short lines "
                              "and must be read completely before any right column text "
                              "appears."));
    EXPECT_THAT(md, HasSubstr("Right column sentence one also flows across several lines "
                              "and must appear after the whole of the left column in the "
                              "output."));
}

TEST(TableFixture, AlignedColumnsBecomeMarkdownTable) {
    std::string md = convert("table.pdf");
    EXPECT_THAT(md, HasSubstr("| Name | Quantity | Price |\n"
                              "| --- | --- | --- |\n"
                              "| Apples | 12 | 1.50 |\n"
                              "| Bananas | 7 | 0.75 |\n"
                              "| Cherries | 31 | 4.20 |"));
}

TEST(TableFixture, TrailingParagraphNotAbsorbed) {
    std::string md = convert("table.pdf");
    EXPECT_THAT(md, HasSubstr("A regular paragraph after the table should not be absorbed "
                              "into it."));
    EXPECT_THAT(md, Not(HasSubstr("| A regular paragraph")));
}

TEST(TableFixture, NoTablesOptionDisablesDetection) {
    pdf2md::AnalyzeParams params;
    params.detect_tables = false;
    std::string md = convert("table.pdf", {}, params, {}, "no-tables");
    EXPECT_THAT(md, Not(HasSubstr("| Name |")));
    EXPECT_THAT(md, HasSubstr("Name"));
}

TEST(ImageFixture, ImagesExtractedAsPngAndReferenced) {
    fs::path img_dir = fixture_dir() / "extracted_images";
    pdf2md::ExtractOptions ex;
    ex.extract_images = true;
    ex.image_dir = img_dir;
    ex.image_ref_prefix = "extracted_images/";
    std::string md = convert("image.pdf", ex, {}, {}, "with-images");

    EXPECT_THAT(md, HasSubstr("![](extracted_images/image_1.png)"));
    // The image sits between the two paragraphs in reading order.
    size_t above = md.find("above the picture");
    size_t ref = md.find("![](");
    size_t below = md.find("below the picture");
    ASSERT_NE(above, std::string::npos);
    ASSERT_NE(ref, std::string::npos);
    ASSERT_NE(below, std::string::npos);
    EXPECT_LT(above, ref);
    EXPECT_LT(ref, below);

    fs::path png = img_dir / "image_1.png";
    ASSERT_TRUE(fs::exists(png));
    std::ifstream is(png, std::ios::binary);
    unsigned char magic[8] = {};
    is.read(reinterpret_cast<char*>(magic), 8);
    const unsigned char expect[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    EXPECT_EQ(std::memcmp(magic, expect, 8), 0);
}

// The CLI extracts images by default (no --images required): a document's
// figures are written to a directory derived from the output file's name and
// referenced as embeds, so a plain `pdf2md paper.pdf` keeps its figures.
// `--no-images` turns the whole step off and creates no directory.
TEST(ImageFixture, CliExtractsImagesByDefault) {
    const fs::path exe(PDF2MD_CLI_EXE);
    ASSERT_TRUE(fs::exists(exe)) << "pdf2md executable not built: " << exe;
    const fs::path pdf = fixture_dir() / "image.pdf";
    ASSERT_TRUE(fs::exists(pdf));

    // Default run: no image flag at all. The derived directory is <stem>_images.
    const fs::path md = output_dir() / "cli_default_image.md";
    const fs::path derived = output_dir() / "cli_default_image_images";
    fs::remove_all(derived);
    fs::remove(md);
    ASSERT_EQ(run_cli("\"" + pdf.string() + "\" -o \"" + md.string() + "\" --quiet"), 0);
    const std::string body = read_file(md);
    EXPECT_THAT(body, HasSubstr("![](cli_default_image_images/image_1.png)"));
    EXPECT_TRUE(fs::exists(derived / "image_1.png"));

    // --no-images suppresses both the embed and the directory.
    const fs::path md2 = output_dir() / "cli_noimg_image.md";
    const fs::path derived2 = output_dir() / "cli_noimg_image_images";
    fs::remove_all(derived2);
    fs::remove(md2);
    ASSERT_EQ(
        run_cli("\"" + pdf.string() + "\" -o \"" + md2.string() + "\" --no-images --quiet"), 0);
    const std::string body2 = read_file(md2);
    EXPECT_THAT(body2, Not(HasSubstr("![](")));
    EXPECT_FALSE(fs::exists(derived2));
}

// Writes a minimal PDF whose page carries a scaled form XObject filled with many
// nested path rectangles plus interior label text (a synthetic vector diagram),
// and a plain caption below the form. libharu cannot emit form XObjects, so the
// bytes are assembled by hand; this is the structure detect_figure_regions keys
// on (dense nested paths + a sub-unity placement scale). If `diagram` is false the
// form is placed at unit scale (an in-page form, e.g. a table) that must NOT be
// treated as a diagram. If `overhang` is set, a full-width prose sentence and a
// short stray label are drawn ON THE PAGE inside the placed form's bounding box,
// modelling a caption/body line the diagram box overhangs: the prose must survive,
// the stray label must be dropped along with the diagram's own interior labels.
// If `label_row` is set, a row of two dense word clusters separated by a wide
// column gutter is drawn inside the box (the signature of a UML/architecture
// diagram's box-label row: wide and densely packed enough to clear the prose
// width/fill bars, yet split by a gutter no wrapped sentence ever contains). It
// must be dropped, not mistaken for an overhung prose line.
void write_form_xobject_pdf(const fs::path& out, bool diagram, bool overhang = false,
                            bool label_row = false) {
    std::string form;  // form XObject content stream, local space [0 0 600 600]
    form += "0 0 0 rg\n";
    for (int i = 0; i < 16; ++i) {  // >= 12 nested paths -> dense vector cluster
        int x = 20 + (i % 4) * 140, y = 40 + (i / 4) * 140;
        form += std::to_string(x) + " " + std::to_string(y) + " 90 90 re f\n";
    }
    form += "BT /F1 40 Tf 120 300 Td (SECRETLABELWORD) Tj ET\n";

    // Placement: scale then translate the 600x600 canvas into the page's middle.
    // The diagram then occupies page x[150,450] y[400,700].
    std::string scale = diagram ? "0.5 0 0 0.5" : "1 0 0 1";
    std::string page;
    page += "BT /F1 12 Tf 150 360 Td (DIAGRAMCAPTIONWORD) Tj ET\n";
    page += "q " + scale + " 150 400 cm /Fm1 Do Q\n";
    if (overhang) {
        // Dense, wide sentence inside the box (prose) and a short isolated label.
        page += "BT /F1 10 Tf 155 660 Td "
                "(This overhanging body sentence must remain visible in output) Tj ET\n";
        page += "BT /F1 10 Tf 250 520 Td (STRAYLABEL) Tj ET\n";
    }
    if (label_row) {
        // Two dense clusters on one baseline, kept inside the diagram box
        // (page x[150,450]) and separated by a wide gutter. Each word packs its
        // span (no interior spaces), so the row clears the width/fill prose bars;
        // only the gutter between them betrays it as a label row, not a sentence.
        page += "BT /F1 10 Tf 160 630 Td (LEFTLABELCLUSTERWORD) Tj ET\n";
        page += "BT /F1 10 Tf 330 630 Td (RIGHTLABELCLUSTERWD) Tj ET\n";
    }

    auto obj_stream = [](int n, const std::string& dict, const std::string& s) {
        return std::to_string(n) + " 0 obj<<" + dict + "/Length " +
               std::to_string(s.size()) + ">>stream\n" + s + "\nendstream endobj\n";
    };
    std::vector<std::string> objs = {
        "1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n",
        "2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n",
        "3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Contents 4 0 R"
        "/Resources<</Font<</F1 6 0 R>>/XObject<</Fm1 5 0 R>>>>>>endobj\n",
        obj_stream(4, "", page),
        obj_stream(5, "/Type/XObject/Subtype/Form/BBox[0 0 600 600]"
                      "/Resources<</Font<</F1 6 0 R>>>>", form),
        "6 0 obj<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>endobj\n",
    };
    std::string pdf = "%PDF-1.7\n";
    std::vector<size_t> off(objs.size() + 1, 0);
    for (size_t i = 0; i < objs.size(); ++i) { off[i + 1] = pdf.size(); pdf += objs[i]; }
    size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
    for (size_t i = 1; i <= objs.size(); ++i) {
        char e[24];
        std::snprintf(e, sizeof(e), "%010zu 00000 n \n", off[i]);
        pdf += e;
    }
    pdf += "trailer<</Size " + std::to_string(objs.size() + 1) +
           "/Root 1 0 R>>\nstartxref\n" + std::to_string(xref) + "\n%%EOF";
    std::ofstream(out, std::ios::binary) << pdf;
}

TEST(VectorDiagram, InteriorLabelsSuppressedAndFigureRasterized) {
    fs::path pdf = output_dir() / "vecdiagram.pdf";
    write_form_xobject_pdf(pdf, /*diagram=*/true);

    pdf2md::ExtractOptions ex_opts;
    ex_opts.extract_images = true;
    ex_opts.image_dir = output_dir() / "vecdiagram_img";
    pdf2md::Extraction ex = pdf2md::extract_pdf(pdf, ex_opts);
    pdf2md::Document doc = pdf2md::analyze_layout(ex.meta, ex.pages, {});
    std::string md = pdf2md::write_markdown(doc, {});

    // The scaled diagram form is rasterized; its interior label never reaches the
    // text flow, while the caption drawn below the form survives as prose.
    EXPECT_THAT(md, HasSubstr("![]("));
    EXPECT_THAT(md, Not(HasSubstr("SECRETLABELWORD")));
    EXPECT_THAT(md, HasSubstr("DIAGRAMCAPTIONWORD"));
}

TEST(VectorDiagram, UnscaledInPageFormKeepsItsText) {
    fs::path pdf = output_dir() / "inpageform.pdf";
    write_form_xobject_pdf(pdf, /*diagram=*/false);

    pdf2md::Extraction ex = pdf2md::extract_pdf(pdf, {});
    pdf2md::Document doc = pdf2md::analyze_layout(ex.meta, ex.pages, {});
    std::string md = pdf2md::write_markdown(doc, {});

    // A form drawn at page scale (in-page content, not an imported drawing) is not
    // a diagram: its text is preserved rather than suppressed as figure labelling.
    EXPECT_THAT(md, HasSubstr("SECRETLABELWORD"));
}

TEST(VectorDiagram, OverhangingProseSurvivesWhileLabelsDropped) {
    fs::path pdf = output_dir() / "vecoverhang.pdf";
    write_form_xobject_pdf(pdf, /*diagram=*/true, /*overhang=*/true);

    pdf2md::ExtractOptions ex_opts;
    ex_opts.extract_images = true;
    ex_opts.image_dir = output_dir() / "vecoverhang_img";
    pdf2md::Extraction ex = pdf2md::extract_pdf(pdf, ex_opts);
    pdf2md::Document doc = pdf2md::analyze_layout(ex.meta, ex.pages, {});
    std::string md = pdf2md::write_markdown(doc, {});

    // A prose line the diagram's bounding box overhangs is running text, not one of
    // the diagram's interior labels: it survives even though it lies inside the box.
    // The diagram's own short labels (its baked-in one and a stray page label inside
    // the box) are still dropped, and the figure is rasterized.
    EXPECT_THAT(md, HasSubstr("![]("));
    EXPECT_THAT(md, HasSubstr("overhanging body sentence must remain visible"));
    EXPECT_THAT(md, Not(HasSubstr("SECRETLABELWORD")));
    EXPECT_THAT(md, Not(HasSubstr("STRAYLABEL")));
}

TEST(VectorDiagram, MultiColumnLabelRowDroppedNotMistakenForProse) {
    fs::path pdf = output_dir() / "veclabelrow.pdf";
    write_form_xobject_pdf(pdf, /*diagram=*/true, /*overhang=*/true, /*label_row=*/true);

    pdf2md::ExtractOptions ex_opts;
    ex_opts.extract_images = true;
    ex_opts.image_dir = output_dir() / "veclabelrow_img";
    pdf2md::Extraction ex = pdf2md::extract_pdf(pdf, ex_opts);
    pdf2md::Document doc = pdf2md::analyze_layout(ex.meta, ex.pages, {});
    std::string md = pdf2md::write_markdown(doc, {});

    // A wide, densely packed row of box labels split by a column gutter is diagram
    // interior text, not an overhung sentence: both clusters are dropped even though
    // the row clears the prose width/fill bars. The genuine overhung prose sentence
    // on the same page still survives, so the gutter gate did not over-suppress.
    EXPECT_THAT(md, HasSubstr("![]("));
    EXPECT_THAT(md, Not(HasSubstr("LEFTLABELCLUSTERWORD")));
    EXPECT_THAT(md, Not(HasSubstr("RIGHTLABELCLUSTERWD")));
    EXPECT_THAT(md, HasSubstr("overhanging body sentence must remain visible"));
}

// Writes a PDF shaped like a scanned book with an OCR text layer -- the
// structure scanning software typically produces for print books. Every page
// carries (a) a full-page raster image (the page scan) and (b) an invisible
// (render mode 3) text layer whose nominal font size is 1pt blown up by the
// text matrix -- the standard output of OCR software, and the pattern that
// used to defeat every size-derived layout heuristic. Every page also carries
// a small visible watermark stamped at the same position ("draftmark", like a
// real DRM stamp), and page 2 additionally holds a small genuine figure
// image that must survive extraction while the page scans must not.
void write_scanned_ocr_pdf(const fs::path& out) {
    auto ocr_line = [](double size, double x, double y, const std::string& text) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.1f 0 0 %.1f %.1f %.1f Tm ", size, size, x, y);
        return std::string(buf) + "(" + text + ") Tj\n";
    };
    auto page_content = [&](const std::string& body_lines, bool with_figure) {
        std::string s;
        s += "q 612 0 0 792 0 0 cm /Im1 Do Q\n";  // full-page scan backdrop
        if (with_figure) s += "q 96 0 0 64 400 150 cm /Im2 Do Q\n";  // real figure
        s += "BT 3 Tr /F1 1 Tf\n" + body_lines + "ET\n";
        // The watermark: ordinary metrics, stamped at the same spot on every
        // page, mid-height in the outer right margin (like a real DRM stamp).
        s += "BT /F1 9 Tf 1 0 0 1 560 400 Tm (draftmark) Tj ET\n";
        return s;
    };

    std::string p1 = page_content(
        ocr_line(11, 72, 640, "The scanned optical layer must survive with every word intact") +
            ocr_line(11, 72, 626, "and join into one readable paragraph of body prose.") +
            // A space glyph whose coordinates land INSIDE the preceding word
            // (x=112 falls between the 'e' and 'd' of "misplaced" at 11pt
            // Helvetica metrics) while its stream position is correct -- the
            // OCR space-re-anchoring case. Without the repair the x-sort drags
            // it mid-word ("misplace d...").
            ocr_line(11, 72, 612, "misplaced") + ocr_line(11, 112, 612, " ") +
            ocr_line(11, 124, 612, "spaces") +
            // Two words separated by a 2.0pt gap with NO space glyph at all
            // (word ends at x=98.9): far under the normal 0.18em bimodal clamp
            // floor, split only by the OCR-relaxed word-gap gates.
            ocr_line(11, 72, 598, "alpha") + ocr_line(11, 100.9, 598, "beta"),
        false);
    std::string p2 = page_content(
        // The two lines leave a >40pt text-free band between them over blank
        // (near-white) scan paper: band mining must NOT emit an empty crop.
        ocr_line(11, 72, 640, "Second page copy keeps its own sentences readable too.") +
            ocr_line(11, 72, 560, "A caption may accompany the small genuine figure below."),
        true);
    std::string p3 = page_content(
        ocr_line(11, 72, 640, "Third page text exists so repetition thresholds are met."),
        false);
    // A cover-like page: the full-page bitmap with almost no recognized text on
    // top. Without an OCR layer the bitmap IS the content and must survive.
    std::string p4 = page_content(ocr_line(11, 72, 640, "Cover"), false);
    // A page whose scan bitmap (Im3) carries a dark "diagram" block in the
    // text-free middle band: the suppressed scan's band must come back as a
    // cropped figure embed.
    std::string p5;
    p5 += "q 612 0 0 792 0 0 cm /Im3 Do Q\n";
    p5 += "BT 3 Tr /F1 1 Tf\n" +
          ocr_line(11, 72, 700, "Fifth page prose sits above the scanned diagram region.") +
          ocr_line(11, 72, 686, "More prose follows directly beneath the first sentence.") +
          ocr_line(11, 72, 150, "Closing sentence beneath the diagram wraps the page.") + "ET\n";
    p5 += "BT /F1 9 Tf 1 0 0 1 560 400 Tm (draftmark) Tj ET\n";

    auto obj_stream = [](int n, const std::string& dict, const std::string& s) {
        return std::to_string(n) + " 0 obj<<" + dict + "/Length " +
               std::to_string(s.size()) + ">>stream\n" + s + "\nendstream endobj\n";
    };
    const std::string scan_pixels(16, '\xDD');   // 4x4 near-white gray page scan
    std::string figure_pixels;                    // 4x4 RGB checkerboard
    for (int i = 0; i < 16; ++i) {
        bool on = (i / 4 + i % 4) % 2;
        figure_pixels += static_cast<char>(on ? 0xE0 : 0x20);
        figure_pixels += static_cast<char>(on ? 0x20 : 0xE0);
        figure_pixels += '\x80';
    }
    // 4x4 gray scan whose middle rows are dark: stretched to the page it puts a
    // "diagram" block across y~[198,594], inside page 5's text-free band.
    std::string diagram_scan_pixels(16, '\xDD');
    for (int i = 4; i < 12; ++i) diagram_scan_pixels[i] = '\x30';
    const char* page_dict =
        "/Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Resources<</Font<</F1 10 0 R>>"
        "/XObject<</Im1 11 0 R/Im2 12 0 R/Im3 16 0 R>>>>";
    std::vector<std::string> objs = {
        "1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n",
        "2 0 obj<</Type/Pages/Kids[3 0 R 4 0 R 5 0 R 6 0 R 14 0 R]/Count 5>>endobj\n",
        "3 0 obj<<" + std::string(page_dict) + "/Contents 7 0 R>>endobj\n",
        "4 0 obj<<" + std::string(page_dict) + "/Contents 8 0 R>>endobj\n",
        "5 0 obj<<" + std::string(page_dict) + "/Contents 9 0 R>>endobj\n",
        "6 0 obj<<" + std::string(page_dict) + "/Contents 13 0 R>>endobj\n",
        obj_stream(7, "", p1),
        obj_stream(8, "", p2),
        obj_stream(9, "", p3),
        "10 0 obj<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>endobj\n",
        obj_stream(11,
                   "/Type/XObject/Subtype/Image/Width 4/Height 4/ColorSpace/DeviceGray"
                   "/BitsPerComponent 8",
                   scan_pixels),
        obj_stream(12,
                   "/Type/XObject/Subtype/Image/Width 4/Height 4/ColorSpace/DeviceRGB"
                   "/BitsPerComponent 8",
                   figure_pixels),
        obj_stream(13, "", p4),
        "14 0 obj<<" + std::string(page_dict) + "/Contents 15 0 R>>endobj\n",
        obj_stream(15, "", p5),
        obj_stream(16,
                   "/Type/XObject/Subtype/Image/Width 4/Height 4/ColorSpace/DeviceGray"
                   "/BitsPerComponent 8",
                   diagram_scan_pixels),
    };
    std::string pdf = "%PDF-1.7\n";
    std::vector<size_t> off(objs.size() + 1, 0);
    for (size_t i = 0; i < objs.size(); ++i) { off[i + 1] = pdf.size(); pdf += objs[i]; }
    size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
    for (size_t i = 1; i <= objs.size(); ++i) {
        char e[24];
        std::snprintf(e, sizeof(e), "%010zu 00000 n \n", off[i]);
        pdf += e;
    }
    pdf += "trailer<</Size " + std::to_string(objs.size() + 1) +
           "/Root 1 0 R>>\nstartxref\n" + std::to_string(xref) + "\n%%EOF";
    std::ofstream(out, std::ios::binary) << pdf;
}

std::string convert_scanned_ocr(bool images, bool strip_chrome = true,
                                const char* stem = "scanned_ocr") {
    fs::path pdf = output_dir() / (std::string(stem) + ".pdf");
    write_scanned_ocr_pdf(pdf);
    pdf2md::ExtractOptions ex_opts;
    if (images) {
        ex_opts.extract_images = true;
        ex_opts.image_dir = output_dir() / (std::string(stem) + "_img");
    }
    pdf2md::Extraction ex = pdf2md::extract_pdf(pdf, ex_opts);
    pdf2md::AnalyzeParams params;
    params.strip_headers_footers = strip_chrome;
    pdf2md::Document doc = pdf2md::analyze_layout(ex.meta, ex.pages, params);
    return pdf2md::write_markdown(doc, {});
}

// The OCR layer's nominal 1pt font size must not shred words with false
// word-break spaces, and consecutive OCR lines must still join into one
// paragraph -- both need the repaired (drawn) glyph size.
TEST(ScannedOcr, WordsAndParagraphsSurviveOcrLayer) {
    std::string md = convert_scanned_ocr(/*images=*/false);
    EXPECT_THAT(md, HasSubstr("The scanned optical layer must survive with every word "
                              "intact and join into one readable paragraph of body prose."));
    EXPECT_THAT(md, HasSubstr("Second page copy keeps its own sentences readable too."));
}

// An OCR space glyph whose coordinates overlap the previous word is re-anchored
// between its stream neighbours instead of being x-sorted into the word.
TEST(ScannedOcr, MisplacedSpaceGlyphReanchored) {
    std::string md = convert_scanned_ocr(/*images=*/false);
    EXPECT_THAT(md, HasSubstr("misplaced spaces"));
    EXPECT_THAT(md, Not(HasSubstr("misplace d")));
}

// A word boundary the OCR layer left with only a ~0.18em gap (no space glyph)
// is recovered by the OCR-relaxed bimodal word-gap gates.
TEST(ScannedOcr, TightWordGapWithoutSpaceGlyphSplits) {
    std::string md = convert_scanned_ocr(/*images=*/false);
    EXPECT_THAT(md, HasSubstr("alpha beta"));
    EXPECT_THAT(md, Not(HasSubstr("alphabeta")));
}

// The full-page scan bitmaps are page backgrounds under the OCR text layer:
// they must not be embedded. Exactly three images survive: the small genuine
// figure on page 2, the cover-like page 4 (a full-page bitmap with almost no
// recognized text -- the bitmap IS that page's content), and the diagram block
// mined from page 5's suppressed scan. Page 2's equally tall but blank
// text-free band must NOT produce a fourth (empty) crop.
TEST(ScannedOcr, PageScansSuppressedFigureCoverAndDiagramKept) {
    std::string md = convert_scanned_ocr(/*images=*/true);
    size_t embeds = 0;
    for (size_t p = md.find("![]("); p != std::string::npos; p = md.find("![](", p + 1))
        ++embeds;
    EXPECT_EQ(embeds, 3u);
    // The diagram crop sits between page 5's body prose and its closing line.
    size_t above = md.find("More prose follows directly beneath the first sentence.");
    size_t below = md.find("Closing sentence beneath the diagram wraps the page.");
    size_t last_embed = md.rfind("![](");
    ASSERT_NE(above, std::string::npos);
    ASSERT_NE(below, std::string::npos);
    ASSERT_NE(last_embed, std::string::npos);
    EXPECT_LT(above, last_embed);
    EXPECT_LT(last_embed, below);
}

// The repeated same-position stamp is running chrome (a watermark), stripped by
// default and kept when header/footer stripping is disabled.
TEST(ScannedOcr, WatermarkStrippedByDefaultKeptOnRequest) {
    EXPECT_THAT(convert_scanned_ocr(false), Not(HasSubstr("draftmark")));
    EXPECT_THAT(convert_scanned_ocr(false, /*strip_chrome=*/false, "scanned_ocr_keep"),
                HasSubstr("draftmark"));
}

// Position-anchored running-head stripping: a chapter-scoped head repeating at
// one top-margin spot on 4 of 9 pages (under the document-wide repeat bar) is
// chrome; a borderless table's header row repeating at a fixed top-margin
// position is NOT -- its data row sits right beneath it.
TEST(RunningHeads, ChapterHeadStrippedTableHeaderKept) {
    std::string md = convert("runheads.pdf");
    EXPECT_THAT(md, Not(HasSubstr("GAMMA CHAPTER 7")));
    EXPECT_THAT(md, HasSubstr("Country"));
    EXPECT_THAT(md, HasSubstr("Brasilia"));
    // Every page's unique body text survives.
    EXPECT_THAT(md, HasSubstr("Opening paragraph about alpha topics"));
    EXPECT_THAT(md, HasSubstr("Ninth page closes the fixture with plain iota prose."));
}

TEST(ErrorHandling, MissingFileThrowsPdfError) {
    EXPECT_THROW(pdf2md::extract_pdf(fixture_dir() / "nope.pdf", {}), pdf2md::PdfError);
}

TEST(ErrorHandling, GarbageFileThrowsPdfError) {
    fs::path bad = fixture_dir() / "garbage.pdf";
    {
        std::ofstream os(bad, std::ios::binary);
        os << "this is not a pdf at all";
    }
    EXPECT_THROW(pdf2md::extract_pdf(bad, {}), pdf2md::PdfError);
}

}  // namespace
