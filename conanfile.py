from conan import ConanFile
from conan.tools.cmake import cmake_layout


class Pdf2MdRecipe(ConanFile):
    name = "pdf2md"
    version = "0.1.0"
    license = "MIT"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeToolchain", "CMakeDeps"

    default_options = {
        "pdfium/*:shared": False,
    }

    def requirements(self):
        self.requires("pdfium/95.0.4629")
        self.requires("cli11/2.6.2")
        self.requires("stb/cci.20240531")
        self.test_requires("gtest/1.15.0")
        self.test_requires("libharu/2.4.6")  # generates test fixture PDFs

    def layout(self):
        cmake_layout(self)
