# pdf2md

A PDF → Markdown converter written in C++20. It extracts text with per-character
geometry, font metadata, hyperlinks, outlines, and figures via **PDFium**,
reconstructs the document structure with heuristic layout analysis, and emits
clean GitHub-flavored Markdown (GFM).

## Features

- **Headings & Table of Contents** — headings (`#`–`######`) inferred from
  font-size tiers, bold body-size section lines, numbered headings, and PDF
  outline bookmarks (`--no-headings` to disable), plus an optional nested GFM
  table of contents (`--toc`)
- **Paragraphs** — rebuilt from wrapped lines with compound-aware dehyphenation
  (`hyphen-`/`ation` → `hyphenation`, including PDFium soft hyphens), URL wrap
  joining, and cross-page paragraph continuation merging
- **Inline styles & hyperlinks** — bold, italic, strikethrough (`~~…~~` from
  strikeout annotations and midline vector rules), inline code spans, and
  `[text](url)` hyperlinks from explicit PDF URI annotations and implicit
  WebLinks (`--no-links` to disable)
- **Lists, task lists & dividers** — bullet, numbered, and GFM task lists
  (`- [ ]` / `- [x]`) with indentation-based nesting, plus horizontal thematic
  rules (`---`)
- **Fenced code blocks** — monospace listings with indentation reconstructed from
  glyph positions
- **Tables** — recovered from both aligned-column layouts and ruled-lattice
  grids, with GFM column alignment (`:---`, `:---:`, `---:`) and multi-page
  table continuation stitching (`--no-tables` to disable)
- **Inline & display math** — superscripts/subscripts, Greek & operator symbols,
  rule-anchored stacked fractions (`\frac{num}{den}`), and vinculum / Nth-root
  radicals (`\sqrt[n]{…}`) reconstructed as LaTeX in `$…$` and `$$…$$`
- **Multi-column reading order** — recursive XY-cut segmentation with full-width
  / two-column transition splits
- **Running chrome & watermark removal** — repeated headers, footers, page
  numbers, and background stamps detected across pages and stripped by default
- **Image & vector figure extraction** — extracted as PNG **by default** into
  `<output_stem>_images/` when writing to a file (or into `DIR` via
  `--images DIR`, disabled via `--no-images`), covering embedded raster images,
  vector diagram rasterization, scanned figure-band mining, content-hash
  deduplication, and `![alt](path)` alt text from tagged-PDF `/Alt` metadata or
  adjacent figure captions
- **Rich YAML 1.2 front matter & encrypted PDFs** — `--front-matter` emits
  `title`, `author`, `subject`, `keywords`, ISO-8601 `date`/`modified`, `source`,
  and `pages`; `--password` unlocks encrypted documents
- **Unicode & OCR text-layer repair** — full UTF-8/UTF-16 surrogate pipeline,
  ligature & TeX accent normalization, non-ASCII paths on Windows, and geometry
  repair for scanned PDFs carrying an existing OCR text layer (optical character
  recognition on image-only scans without a text layer is out of scope)

## Building

Requires CMake ≥ 3.23, Conan 2, and a C++20 compiler (tested with MSVC 2022 and
GCC 11). The `build.py` helper runs the whole flow — Conan install, CMake
configure, and build — picking the right preset for your OS:

```
python build.py            # configure + build (Release)
python build.py --test     # build, then run the ctest suite
```

Run `python build.py -h` for options (`--profile`, `--preset`, `--clean`, `-j`,
…). The binary lands in `build/Release/pdf2md` (`.exe` on Windows). Dependencies
(pdfium, CLI11, stb, gtest, libharu) come from ConanCenter; prebuilt binaries
exist for common platforms.

## Usage

```
pdf2md input.pdf                     # writes input.md + input_images/ next to the PDF
pdf2md input.pdf -o out.md           # explicit output ('-' for stdout, no default image dir)
pdf2md input.pdf --no-images         # skip image extraction and image references
pdf2md input.pdf --images assets     # extract PNG images into assets/ instead of <stem>_images/
pdf2md input.pdf --pages 1-3,7       # convert selected pages (1-based)
pdf2md input.pdf --front-matter      # YAML front matter from PDF metadata
pdf2md input.pdf --toc               # nested GFM table of contents from headings
pdf2md input.pdf --page-breaks       # '---' between pages
```

| Flag | Effect |
| --- | --- |
| `-o, --output FILE` | output Markdown path (default: `<input_stem>.md`; `-` for stdout) |
| `--pages SPEC` | 1-based page subset, e.g. `1-3,7` |
| `--password PW` | open an encrypted PDF |
| `--images DIR` | directory for extracted PNG images (default: `<output_stem>_images/`; mutually exclusive with `--no-images`) |
| `--no-images` | do not extract or reference embedded images (mutually exclusive with `--images`) |
| `--no-links` | do not extract or emit hyperlinks |
| `--toc` | emit a Markdown table of contents from headings |
| `--front-matter` | emit YAML front matter (`title`, `author`, `subject`, `keywords`, `date`, `modified`, `source`, `pages`) |
| `--page-breaks` | emit a horizontal rule (`---`) between pages |
| `--keep-headers` | keep repeated page headers, footers, and watermarks |
| `--no-headings` | do not infer headings from font sizes or outlines |
| `--no-tables` | do not infer tables from aligned columns or ruled grids |
| `--keep-rotated` | keep rotated text (watermarks are skipped by default) |
| `-q, --quiet` | suppress the conversion summary line on stderr |
| `--version` | print `pdf2md <version>` and exit |
| `-h, --help` | print CLI help and exit |

Exit codes: `0` success, `1` bad arguments, `2` PDF load/parse error, `3` output I/O error.

## Tests

Google Test suite in `tests/`; synthetic test fixtures are PDFs generated at
test run time by libharu (`tests/fixture_pdfs.cpp`), so no binary fixtures are
checked in.

```
python build.py --test
```

Each run leaves its inputs and results in `build/test_output/` for inspection:
`fixtures/` holds the generated PDFs plus the converted `.md` for each, and
`corpus/` holds one `.md` per downloaded real-world PDF.

The `Examples.GenerateGalleryWithCli` test (`tests/examples_export.cpp`) builds
a browsable gallery under `build/examples/`: it runs the built `pdf2md`
executable over every fixture PDF and every downloaded corpus PDF, leaving each
`<name>.pdf` beside the `<name>.md` and extracted images the tool produces.

### Real-world corpus tests

A second layer of component tests runs against well-known public PDFs (the
pdf.js `tracemonkey` two-column paper, the *Attention Is All You Need* arXiv
paper, `pdfminer` samples, `tabula` table PDFs including CJK financial tables,
and a PDF 2.0 sample). Pass `--with-corpus` to fetch them first; the `Corpus.*`
tests then run instead of skipping:

```
python build.py --with-corpus --test
```

## Debugging

Set any of the following environment variables to dump diagnostic streams to
`stderr`:

| Variable | Effect |
| --- | --- |
| `PDF2MD_DEBUG_RAW=1` | dump PDFium's raw character stream (code point + position) |
| `PDF2MD_DEBUG_RULES=1` | dump detected vector rule segments (table borders, bars, dividers) |
| `PDF2MD_DEBUG_BG=1` | dump per-image page-background coverage and overlying glyph counts |
| `PDF2MD_DEBUG_WM=1` | dump repeated-text signatures classified as watermarks/stamps |

## Architecture

The library (`pdf2md_core`) mirrors the C++ namespace hierarchy 1-to-1 in its
directory layout as a strict 9-level acyclic component DAG, separating insulated
public headers in `include/pdf2md/` (`namespace pdf2md`) from private subsystems
in `src/pdf2md/<pkg>/` (`namespace pdf2md::<pkg>`). The PIMPL-insulated stage
engines `PdfExtractor`, `LayoutAnalyzer` (which caches its `DocumentPass`
pipeline), and `MarkdownWriter` compose directly inside the PIMPL `Converter`
facade, while `extract_pdf`, `analyze_layout`, and `write_markdown` remain
convenience stage seams:

```
include/pdf2md/                 Public headers (namespace pdf2md; zero PDFium / CLI11 / stb leaks)
  utf8.h                          Level 1: UTF-8/16LE codec declarations, constexpr predicates, UTF-8 path helpers
  options.h                       Level 1: ExtractOptions, AnalyzeParams, WriteOptions,
                                           ConvertOptions, ConvertResult, PdfError
  image_extractor.h               Level 1: PDFium-free BitmapView / RasterImage PNG encoding & hashing
  document_model.h                Level 2: Pure domain AST structs & enums (CharInfo, PageData, Block, Document)
  cli_config.h                    Level 2: ExitCode, parse_pages, resolve_cli_config
  pdf_extractor.h                 Level 4: PIMPL PdfExtractor + extract_pdf stage seam
  markdown_writer.h               Level 4: PIMPL MarkdownWriter + write_markdown stage seam
  layout_analyzer.h               Level 7: PIMPL LayoutAnalyzer + analyze_layout stage seam
  converter.h                     Level 8: PIMPL Converter facade (includes only pdf2md/options.h)
src/
  main.cpp                        Level 9: CLI11 shell (wmain on Windows) -> cli_config -> Converter
  pdf2md/                         Core public-namespace implementations (namespace pdf2md)
    utf8.cpp, options.cpp, cli_config.cpp
    converter.cpp                 Level 8: Composes PdfExtractor -> LayoutAnalyzer -> MarkdownWriter
    model/                        Domain geometry & helpers (namespace pdf2md::model)
      geometry.h                  Level 1: Rect, Interval, find_gap_cuts, median, has_env_flag
      document_utils.*            Level 2: Block/line geometry & classification helpers
    pdf/                          PDFium adapter & raster pipeline (namespace pdf2md::pdf)
      image_extractor.cpp         Level 1: stb_image_write PNG encoder & FNV-1a bitmap hashing
      pdfium_raii.h               Level 2: PdfiumInit Meyers singleton, RAII handles, bitmap adapters
      ocr_repair.*                Level 3: OCR text-layer & inflated font-size geometry repair
      figures.*                   Level 3: raster images, vector diagrams, scan-band crops (I/O outside lock)
      extractor.cpp               Level 4: PdfExtractor PIMPL adapter over PDFium's C API
    markdown/                     Markdown emitter & inline normalization (namespace pdf2md::markdown)
      inline_normalizer.*         Level 3: document vocab, dehyphenation, URL wraps, merge_block_runs
      writer.cpp                  Level 4: MarkdownWriter PIMPL -> MarkdownRenderer + MarkdownAssembler
    layout/                       Heuristic layout analysis & pass pipeline (namespace pdf2md::layout)
      line_cluster.*              Level 3: baseline clustering, word/column gap thresholds
      headings.*                  Level 3: font-tier, section-number & outline heading classification
      math.*                      Level 4: line planning, LaTeX math (\frac, \sqrt, scripts), build_lines
      tables_text.*               Level 5: aligned-column table detection & alignment inference
      tables_ruled.*              Level 5: ruled-lattice grid table recovery & alignment inference
      chrome.*                    Level 5: body-size estimation & running header/footer/watermark stripping
      regions.*                   Level 6: recursive XY-cut & 1-col/2-col transition split_region
      blocks.*                    Level 6: lists, task lists, equations, dividers, region_to_blocks
      passes.*                    Level 6: DocumentPass Strategy/Template Method pipeline
      analyzer.cpp                Level 7: LayoutAnalyzer PIMPL + cached DocumentPass pipeline runner
tests/                          Google Test UT / CT / ST suites + libharu fixture generators
```

Design patterns are applied only where they carry weight: Facade + PIMPL
(`Converter`, composing PIMPL-insulated `PdfExtractor`, `LayoutAnalyzer`, and
`MarkdownWriter`), Adapter + RAII (the PDFium boundary confined inside `src/`),
a Meyers Singleton for library init (deliberately the only singleton, with PNG
compression and disk writes kept outside its lock), Strategy / Template Method /
Factory Method (the `DocumentPass` pipeline), and Builder (`MarkdownAssembler`).
Deliberately rejected as poor fits here: Visitor or a polymorphic `Block`
hierarchy (`Block` is a plain value struct that passes re-tag in place), State
for the block classifier, and Command / Observer / Interpreter / Flyweight.

## License

pdf2md is dual-licensed — pick whichever fits your project:

- **[AGPL-3.0](LICENSE)** — free to use, modify and distribute, including
  commercially. In exchange, anything you build on pdf2md must also be released
  under the AGPL with full source, and that obligation extends to software you
  offer over a network (SaaS), not just software you ship.
- **[Commercial license](COMMERCIAL-LICENSE.md)** — for closed-source products,
  hosted services, or organisations that cannot take on AGPL obligations. No
  copyleft requirement. Contact <pularaulviclean@gmail.com>.

Copyright © 2026 Masakatsu Funaki. Third-party dependencies keep their own
permissive licenses; see [COMMERCIAL-LICENSE.md](COMMERCIAL-LICENSE.md) for the
breakdown.
