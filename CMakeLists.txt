cmake_minimum_required(VERSION 3.23)
project(pdf2md VERSION 0.1.0 LANGUAGES CXX)

option(PDF2MD_BUILD_TESTS "Build the pdf2md test suite" ON)

find_package(pdfium REQUIRED CONFIG)
find_package(CLI11 REQUIRED CONFIG)
find_package(stb REQUIRED CONFIG)

add_library(pdf2md_core STATIC
    src/converter.cpp
    src/pdf_extractor.cpp
    src/layout_analyzer.cpp
    src/layout_lines.cpp
    src/layout_math.cpp
    src/layout_tables_text.cpp
    src/layout_tables_ruled.cpp
    src/layout_blocks.cpp
    src/layout_passes.cpp
    src/markdown_writer.cpp
    src/image_extractor.cpp
)
target_compile_features(pdf2md_core PUBLIC cxx_std_20)
target_include_directories(pdf2md_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pdf2md_core PRIVATE pdfium::pdfium stb::stb)

add_executable(pdf2md src/main.cpp)
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
