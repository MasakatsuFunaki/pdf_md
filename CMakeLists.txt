cmake_minimum_required(VERSION 3.23)
project(pdf2md VERSION 0.2.0 LANGUAGES CXX)

option(PDF2MD_BUILD_TESTS "Build the pdf2md test suite" ON)

find_package(pdfium REQUIRED CONFIG)
find_package(CLI11 REQUIRED CONFIG)
find_package(stb REQUIRED CONFIG)

add_library(pdf2md_core STATIC
    src/pdf2md/utf8.cpp
    src/pdf2md/options.cpp
    src/pdf2md/cli_config.cpp
    src/pdf2md/converter.cpp
    src/pdf2md/model/document_utils.cpp
    src/pdf2md/pdf/image_extractor.cpp
    src/pdf2md/pdf/ocr_repair.cpp
    src/pdf2md/pdf/figures.cpp
    src/pdf2md/pdf/extractor.cpp
    src/pdf2md/markdown/inline_normalizer.cpp
    src/pdf2md/markdown/writer.cpp
    src/pdf2md/layout/line_cluster.cpp
    src/pdf2md/layout/headings.cpp
    src/pdf2md/layout/math.cpp
    src/pdf2md/layout/tables_text.cpp
    src/pdf2md/layout/tables_ruled.cpp
    src/pdf2md/layout/chrome.cpp
    src/pdf2md/layout/regions.cpp
    src/pdf2md/layout/blocks.cpp
    src/pdf2md/layout/passes.cpp
    src/pdf2md/layout/analyzer.cpp
)
target_compile_features(pdf2md_core PUBLIC cxx_std_20)
target_include_directories(pdf2md_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include/pdf2md
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(pdf2md_core PRIVATE PDF2MD_VERSION="${PROJECT_VERSION}")
target_link_libraries(pdf2md_core PRIVATE pdfium::pdfium stb::stb)

add_executable(pdf2md src/main.cpp)
target_compile_definitions(pdf2md PRIVATE PDF2MD_VERSION="${PROJECT_VERSION}")
target_link_libraries(pdf2md PRIVATE pdf2md_core CLI11::CLI11)

if(MSVC)
    foreach(tgt pdf2md_core pdf2md)
        target_compile_options(${tgt} PRIVATE /utf-8 /W4)
        # pdfium's fpdfview.h includes windows.h; keep its min/max macros out.
        target_compile_definitions(${tgt} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endforeach()
endif()

if(PDF2MD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
