find_package(GTest REQUIRED CONFIG)
find_package(libharu REQUIRED CONFIG)

add_executable(pdf2md_tests
    fixture_pdfs.cpp
    conversion_test.cpp
    corpus_test.cpp
    layout_analyzer_test.cpp
    markdown_writer_test.cpp
    utf8_test.cpp
    examples_export.cpp
)
target_compile_features(pdf2md_tests PRIVATE cxx_std_20)
target_link_libraries(pdf2md_tests PRIVATE pdf2md_core libharu::libharu GTest::gmock_main)
# Real-world PDFs fetched by tests/corpus/download_corpus.ps1; the corpus
# tests skip when this directory does not exist.
target_compile_definitions(pdf2md_tests
    PRIVATE PDF2MD_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus/cache"
    # Test runs leave their fixture PDFs and converted .md files here for
    # inspection.
    PRIVATE PDF2MD_TEST_OUTPUT_DIR="${CMAKE_BINARY_DIR}/test_output"
    # examples_export.cpp runs the built CLI over every fixture + corpus PDF and
    # writes the pdf/md pairs here (in the build tree) as a browsable gallery.
    PRIVATE PDF2MD_EXAMPLES_DIR="${CMAKE_BINARY_DIR}/examples"
    PRIVATE PDF2MD_CLI_EXE="$<TARGET_FILE:pdf2md>")
# The examples generator shells out to the real tool, so make sure it is built.
add_dependencies(pdf2md_tests pdf2md)
if(MSVC)
    target_compile_options(pdf2md_tests PRIVATE /utf-8 /W4)
    target_compile_definitions(pdf2md_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

include(GoogleTest)
gtest_discover_tests(pdf2md_tests DISCOVERY_TIMEOUT 60)
