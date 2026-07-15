# Downloads well-known public test PDFs used by the corpus smoke tests.
# Usage: pwsh tests/corpus/download_corpus.ps1
# Files land in tests/corpus/cache (skipped by the test suite when absent).

$ErrorActionPreference = "Stop"
$cache = Join-Path $PSScriptRoot "cache"
New-Item -ItemType Directory -Force $cache | Out-Null

# name -> url. Sources are pinned to stable hosts/paths.
$files = [ordered]@{
    # Trivial single-line PDF (W3C accessibility test resource).
    "dummy.pdf" = "https://www.w3.org/WAI/ER/tests/xhtml/testfiles/resources/pdf/dummy.pdf"
    # The classic pdf.js demo: two-column ACM paper with figures and formulas.
    "tracemonkey.pdf" = "https://raw.githubusercontent.com/mozilla/pdf.js/master/web/compressed.tracemonkey-pldi-09.pdf"
    # pdfminer.six samples: individually positioned glyphs, CJK text.
    "pdfminer-simple1.pdf" = "https://raw.githubusercontent.com/pdfminer/pdfminer.six/master/samples/simple1.pdf"
    "pdfminer-simple3.pdf" = "https://raw.githubusercontent.com/pdfminer/pdfminer.six/master/samples/simple3.pdf"
    # tabula-java test resources: real-world tables.
    "tabula-eu-002.pdf" = "https://raw.githubusercontent.com/tabulapdf/tabula-java/master/src/test/resources/technology/tabula/eu-002.pdf"
    "tabula-twotables.pdf" = "https://raw.githubusercontent.com/tabulapdf/tabula-java/master/src/test/resources/technology/tabula/twotables.pdf"
    # PDF Association: a minimal, spec-clean PDF 2.0 file.
    "pdf20-simple.pdf" = "https://raw.githubusercontent.com/pdf-association/pdf20examples/master/Simple%20PDF%202.0%20file.pdf"
    # arXiv: 'Attention Is All You Need' - formulas, tables, references.
    "arxiv-attention.pdf" = "https://arxiv.org/pdf/1706.03762"
}

foreach ($name in $files.Keys) {
    $dest = Join-Path $cache $name
    if (Test-Path $dest) {
        Write-Host "cached  $name"
        continue
    }
    Write-Host "fetch   $name"
    Invoke-WebRequest -Uri $files[$name] -OutFile $dest -UserAgent "pdf2md-tests"
}
Write-Host "corpus ready: $cache"
