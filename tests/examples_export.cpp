// Regenerates the examples/ gallery by running the built pdf2md executable over
// every fixture and corpus PDF, verifying clean diagnostics, concrete content
// landmarks, and valid PNG outputs for image-bearing documents.

#include "fixture_pdfs.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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
using ::testing::HasSubstr;
using ::testing::StartsWith;

constexpr size_t kPipeChunkBytes = 4096;
constexpr size_t kPngSignatureBytes = 8;
constexpr size_t kIhdrChunkOffset = 8;
constexpr uint32_t kIhdrDataLength = 13;
constexpr size_t kMinValidPngBytes = 45;
constexpr size_t kIendTrailerBytes = 12;

constexpr std::array<uint8_t, kPngSignatureBytes> kPngSignature = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
constexpr std::array<uint8_t, 4> kIhdrTag = {'I', 'H', 'D', 'R'};
constexpr std::array<uint8_t, kIendTrailerBytes> kIendTrailer = {
    0x00, 0x00, 0x00, 0x00, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82};

#ifdef _WIN32
std::wstring quote_windows_arg(std::wstring_view arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(arg);
    }
    std::wstring out;
    out.push_back(L'"');
    for (size_t i = 0; i < arg.size();) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++backslashes;
            ++i;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            ++i;
            continue;
        }
        out.append(backslashes, L'\\');
        out.push_back(arg[i]);
        ++i;
    }
    out.push_back(L'"');
    return out;
}

class ScopedHandle {
public:
    ScopedHandle() noexcept = default;
    explicit ScopedHandle(HANDLE h) noexcept : handle_(h) {}
    ~ScopedHandle() { reset(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE get() const noexcept { return handle_; }
    bool valid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

    void reset(HANDLE next = nullptr) noexcept {
        if (valid()) {
            CloseHandle(handle_);
        }
        handle_ = next;
    }

private:
    HANDLE handle_ = nullptr;
};

std::string read_pipe_all(HANDLE pipe) {
    std::string out;
    std::array<char, kPipeChunkBytes> buf{};
    DWORD bytes_read = 0;
    while (ReadFile(pipe, buf.data(), static_cast<DWORD>(buf.size()), &bytes_read, nullptr) &&
           bytes_read > 0) {
        out.append(buf.data(), static_cast<size_t>(bytes_read));
    }
    return out;
}
#else
std::string read_fd_all(int fd) {
    std::string out;
    std::array<char, kPipeChunkBytes> buf{};
    for (;;) {
        const ssize_t n = read(fd, buf.data(), buf.size());
        if (n > 0) {
            out.append(buf.data(), static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    return out;
}
#endif

struct CliResult {
    int exit_code = -1;
    std::string stdout_bytes;
    std::string stderr_bytes;
};

CliResult run_pdf2md(const fs::path& exe, const fs::path& pdf, const fs::path& md,
                     const fs::path& image_dir) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE out_r_raw = nullptr;
    HANDLE out_w_raw = nullptr;
    if (!CreatePipe(&out_r_raw, &out_w_raw, &sa, 0)) {
        throw std::runtime_error("CreatePipe(stdout) failed");
    }
    ScopedHandle out_r(out_r_raw);
    ScopedHandle out_w(out_w_raw);
    SetHandleInformation(out_r.get(), HANDLE_FLAG_INHERIT, 0);

    HANDLE err_r_raw = nullptr;
    HANDLE err_w_raw = nullptr;
    if (!CreatePipe(&err_r_raw, &err_w_raw, &sa, 0)) {
        throw std::runtime_error("CreatePipe(stderr) failed");
    }
    ScopedHandle err_r(err_r_raw);
    ScopedHandle err_w(err_w_raw);
    SetHandleInformation(err_r.get(), HANDLE_FLAG_INHERIT, 0);

    ScopedHandle nul_in(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!nul_in.valid()) {
        throw std::runtime_error("CreateFileW(NUL) failed");
    }

    std::wstring cmdline = quote_windows_arg(exe.wstring()) + L" " +
                           quote_windows_arg(pdf.wstring()) + L" -o " +
                           quote_windows_arg(md.wstring());
    if (!image_dir.empty()) {
        cmdline += L" --images " + quote_windows_arg(image_dir.wstring());
    } else {
        cmdline += L" --no-images";
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul_in.get();
    si.hStdOutput = out_w.get();
    si.hStdError = err_w.get();

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        throw std::runtime_error("CreateProcessW failed for pdf2md");
    }
    ScopedHandle proc(pi.hProcess);
    ScopedHandle primary_thread(pi.hThread);

    out_w.reset();
    err_w.reset();
    nul_in.reset();

    std::string stderr_data;
    std::thread err_reader([&stderr_data, h = err_r.get()] { stderr_data = read_pipe_all(h); });
    std::string stdout_data = read_pipe_all(out_r.get());
    err_reader.join();

    if (WaitForSingleObject(proc.get(), INFINITE) != WAIT_OBJECT_0) {
        throw std::runtime_error("WaitForSingleObject failed");
    }
    DWORD raw_code = 0;
    if (!GetExitCodeProcess(proc.get(), &raw_code)) {
        throw std::runtime_error("GetExitCodeProcess failed");
    }
    return CliResult{static_cast<int>(raw_code), std::move(stdout_data), std::move(stderr_data)};
#else
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        if (out_pipe[0] >= 0) {
            close(out_pipe[0]);
            close(out_pipe[1]);
        }
        throw std::runtime_error("pipe() failed");
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        throw std::runtime_error("fork() failed");
    }
    if (pid == 0) {
        const int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);

        std::vector<std::string> owned = {exe.string(), pdf.string(), "-o", md.string()};
        if (!image_dir.empty()) {
            owned.emplace_back("--images");
            owned.push_back(image_dir.string());
        } else {
            owned.emplace_back("--no-images");
        }
        std::vector<char*> argv;
        argv.reserve(owned.size() + 1);
        for (std::string& s : owned) {
            argv.push_back(s.data());
        }
        argv.push_back(nullptr);
        execv(owned[0].c_str(), argv.data());
        _exit(127);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);

    std::string stderr_data;
    std::thread err_reader([&stderr_data, fd = err_pipe[0]] { stderr_data = read_fd_all(fd); });
    std::string stdout_data = read_fd_all(out_pipe[0]);
    err_reader.join();
    close(out_pipe[0]);
    close(err_pipe[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            throw std::runtime_error("waitpid() failed");
        }
    }
    const int code = WIFEXITED(status) ? WEXITSTATUS(status)
                                       : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
    return CliResult{code, std::move(stdout_data), std::move(stderr_data)};
#endif
}

void clean_examples(const fs::path& dir) {
    std::error_code ec;
    std::vector<fs::path> stale;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) {
            continue;
        }
        const fs::path ext = e.path().extension();
        if ((ext == ".pdf" || ext == ".md") && e.path().filename() != "README.md") {
            stale.push_back(e.path());
        }
    }
    for (const auto& p : stale) {
        fs::remove(p, ec);
    }
    fs::remove_all(dir / "images", ec);
}

std::string read_text(const fs::path& p) {
    std::ifstream is(p, std::ios::binary);
    if (!is) {
        throw std::runtime_error("failed to read generated file: " + p.string());
    }
    return std::string(std::istreambuf_iterator<char>(is), std::istreambuf_iterator<char>());
}

std::vector<uint8_t> read_bytes(const fs::path& p) {
    std::ifstream is(p, std::ios::binary);
    if (!is) {
        throw std::runtime_error("failed to read binary file: " + p.string());
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(is),
                                std::istreambuf_iterator<char>());
}

uint32_t read_be_u32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24U) | (static_cast<uint32_t>(p[1]) << 16U) |
           (static_cast<uint32_t>(p[2]) << 8U) | static_cast<uint32_t>(p[3]);
}

void assert_valid_png(const fs::path& png_path) {
    ASSERT_TRUE(fs::exists(png_path)) << "Missing PNG file: " << png_path;
    const std::vector<uint8_t> bytes = read_bytes(png_path);
    ASSERT_GE(bytes.size(), kMinValidPngBytes) << "PNG truncated: " << png_path;
    ASSERT_EQ(std::memcmp(bytes.data(), kPngSignature.data(), kPngSignature.size()), 0)
        << "Invalid PNG magic header in " << png_path;
    ASSERT_EQ(read_be_u32(bytes.data() + kIhdrChunkOffset), kIhdrDataLength)
        << "Unexpected IHDR length in " << png_path;
    ASSERT_EQ(std::memcmp(bytes.data() + kIhdrChunkOffset + 4, kIhdrTag.data(), kIhdrTag.size()), 0)
        << "Missing IHDR tag in " << png_path;
    EXPECT_GT(read_be_u32(bytes.data() + kIhdrChunkOffset + 8), 0U)
        << "Zero PNG width in " << png_path;
    EXPECT_GT(read_be_u32(bytes.data() + kIhdrChunkOffset + 12), 0U)
        << "Zero PNG height in " << png_path;
    const uint8_t* tail = bytes.data() + (bytes.size() - kIendTrailerBytes);
    EXPECT_EQ(std::memcmp(tail, kIendTrailer.data(), kIendTrailer.size()), 0)
        << "Missing canonical IEND trailer in " << png_path;
}

void assert_clean_summary_stderr(std::string_view stderr_bytes, std::string_view name) {
    ASSERT_FALSE(stderr_bytes.empty()) << "Missing summary milestone on stderr for " << name;
    std::string_view trimmed = stderr_bytes;
    if (!trimmed.empty() && trimmed.back() == '\n') {
        trimmed.remove_suffix(1);
    }
    if (!trimmed.empty() && trimmed.back() == '\r') {
        trimmed.remove_suffix(1);
    }
    EXPECT_EQ(trimmed.find('\n'), std::string_view::npos)
        << "Unexpected extra diagnostic lines on stderr for " << name << ": " << stderr_bytes;
    EXPECT_EQ(trimmed.find('\r'), std::string_view::npos)
        << "Unexpected carriage return inside stderr for " << name << ": " << stderr_bytes;
    EXPECT_THAT(std::string(trimmed), StartsWith("pdf2md: converted "));
    EXPECT_THAT(std::string(trimmed), HasSubstr(" page(s), "));
    EXPECT_THAT(std::string(trimmed), HasSubstr(" block(s) -> "));
}

std::vector<fs::path> collect_png_files(const fs::path& dir) {
    std::vector<fs::path> pngs;
    if (!fs::exists(dir)) {
        return pngs;
    }
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".png") {
            pngs.push_back(entry.path());
        }
    }
    std::sort(pngs.begin(), pngs.end());
    return pngs;
}

std::vector<std::string> extract_markdown_image_links(std::string_view md) {
    std::vector<std::string> links;
    constexpr std::string_view kOpen = "![](";
    for (size_t pos = md.find(kOpen); pos != std::string_view::npos; pos = md.find(kOpen, pos)) {
        pos += kOpen.size();
        const size_t close = md.find(')', pos);
        if (close == std::string_view::npos) {
            break;
        }
        links.emplace_back(md.substr(pos, close - pos));
        pos = close + 1;
    }
    return links;
}

void assert_document_landmarks(std::string_view name, const std::string& md,
                               const fs::path& gallery_dir) {
    if (name == "basic") {
        EXPECT_THAT(md, HasSubstr("# Sample Document Title\n"));
        EXPECT_THAT(md, HasSubstr("## First Section\n"));
        EXPECT_THAT(md, HasSubstr("**bold text**"));
        EXPECT_THAT(md, HasSubstr("*italic text*"));
        EXPECT_THAT(md, HasSubstr("- First bullet item\n"));
        EXPECT_THAT(md, HasSubstr("int main() {"));
        EXPECT_THAT(md, HasSubstr("hyphenation repair logic"));
        return;
    }
    if (name == "twocol") {
        EXPECT_THAT(md, HasSubstr("# Two Column Layout Test"));
        const size_t left_end = md.find("any right column text appears.");
        const size_t right_start = md.find("Right column sentence one");
        ASSERT_NE(left_end, std::string::npos);
        ASSERT_NE(right_start, std::string::npos);
        EXPECT_LT(left_end, right_start);
        return;
    }
    if (name == "table") {
        EXPECT_THAT(md, HasSubstr("# Inventory Report"));
        EXPECT_THAT(md, HasSubstr("| Name | Quantity | Price |\n"
                                  "| --- | --- | --- |\n"
                                  "| Apples | 12 | 1.50 |"));
        EXPECT_THAT(md, HasSubstr("A regular paragraph after the table"));
        return;
    }
    if (name == "image") {
        EXPECT_THAT(md, HasSubstr("A paragraph above the picture."));
        EXPECT_THAT(md, HasSubstr("![](images/image/image_1.png)"));
        EXPECT_THAT(md, HasSubstr("A paragraph below the picture."));
        const std::vector<fs::path> pngs = collect_png_files(gallery_dir / "images" / "image");
        ASSERT_EQ(pngs.size(), 1U);
        assert_valid_png(pngs[0]);
        return;
    }
    if (name == "dummy") {
        EXPECT_THAT(md, HasSubstr("Dummy PDF file"));
        return;
    }
    if (name == "pdf20-simple") {
        EXPECT_THAT(md, HasSubstr("Hello World"));
        return;
    }
    if (name == "pdfminer-simple1") {
        EXPECT_THAT(md, HasSubstr("Hello"));
        EXPECT_THAT(md, HasSubstr("World"));
        return;
    }
    if (name == "pdfminer-simple3") {
        EXPECT_THAT(md, HasSubstr("HelloHello"));
        EXPECT_THAT(md, HasSubstr("あ い う え お"));
        EXPECT_THAT(md, HasSubstr("World"));
        return;
    }
    if (name == "tabula-eu-002") {
        EXPECT_THAT(md, HasSubstr("| --- |"));
        EXPECT_THAT(md, HasSubstr("0,2885"));
        EXPECT_THAT(md, HasSubstr("Foreign language competence"));
        return;
    }
    if (name == "tabula-twotables") {
        EXPECT_THAT(md, HasSubstr("株主資本"));
        EXPECT_THAT(md, HasSubstr("12,017"));
        return;
    }
    if (name == "tracemonkey") {
        EXPECT_THAT(md, HasSubstr("# Trace-based Just-in-Time Type Specialization for Dynamic"));
        EXPECT_THAT(md, HasSubstr("## Abstract"));
        EXPECT_THAT(md, HasSubstr("recording and compiling a trace"));
        EXPECT_THAT(md, HasSubstr("![](images/tracemonkey/image_1.png)"));
        const std::vector<fs::path> pngs =
            collect_png_files(gallery_dir / "images" / "tracemonkey");
        ASSERT_GE(pngs.size(), 1U);
        for (const fs::path& png : pngs) {
            assert_valid_png(png);
        }
        return;
    }
    if (name == "arxiv-attention") {
        EXPECT_THAT(md, HasSubstr("# Attention Is All You Need"));
        EXPECT_THAT(md, HasSubstr("## Abstract"));
        EXPECT_THAT(md, HasSubstr("## Attention Visualizations"));
        EXPECT_THAT(md, HasSubstr("![](images/arxiv-attention/image_1.png)"));
        const std::vector<fs::path> pngs =
            collect_png_files(gallery_dir / "images" / "arxiv-attention");
        ASSERT_GE(pngs.size(), 5U);
        for (const fs::path& png : pngs) {
            assert_valid_png(png);
        }
        return;
    }
    if (name == "spec_document") {
        EXPECT_THAT(md, HasSubstr("\n# "));
        EXPECT_THAT(md, HasSubstr("| --- |"));
        const std::vector<fs::path> pngs =
            collect_png_files(gallery_dir / "images" / "spec_document");
        ASSERT_GE(pngs.size(), 1U);
        for (const fs::path& png : pngs) {
            assert_valid_png(png);
        }
        return;
    }

    // Any extra manually-placed corpus PDF must still yield substantive prose/markdown.
    const bool has_non_ws = std::any_of(md.begin(), md.end(), [](unsigned char c) {
        return c > ' ';
    });
    EXPECT_TRUE(has_non_ws) << "Generated markdown contains only whitespace for " << name;
}

struct Input {
    fs::path source;
    std::string name;
    bool images;
};

}  // namespace

TEST(Examples, GenerateGalleryWithCli) {
    const fs::path dir(PDF2MD_EXAMPLES_DIR);
    const fs::path cli(PDF2MD_CLI_EXE);
    ASSERT_FALSE(dir.empty()) << "PDF2MD_EXAMPLES_DIR not defined";
    ASSERT_FALSE(cli.empty()) << "PDF2MD_CLI_EXE not defined";
    ASSERT_TRUE(fs::exists(cli)) << "pdf2md executable not built: " << cli;

    fs::create_directories(dir);
    clean_examples(dir);

    std::vector<Input> inputs;

    struct Fixture {
        const char* name;
        void (*make)(const fs::path&);
        bool images;
    };
    constexpr std::array<Fixture, 4> kFixtures = {{
        {"basic", &pdf2md::testfx::make_basic, false},
        {"twocol", &pdf2md::testfx::make_twocol, false},
        {"table", &pdf2md::testfx::make_table, false},
        {"image", &pdf2md::testfx::make_image, true},
    }};
    for (const Fixture& f : kFixtures) {
        const fs::path pdf = dir / (std::string(f.name) + ".pdf");
        f.make(pdf);
        ASSERT_TRUE(fs::exists(pdf)) << "Fixture generator did not create " << pdf;
        ASSERT_GT(fs::file_size(pdf), 0U) << "Fixture generator wrote empty PDF: " << pdf;
        inputs.push_back({pdf, f.name, f.images});
    }

    std::error_code ec;
    const fs::path corpus(PDF2MD_CORPUS_DIR);
    if (!corpus.empty() && fs::exists(corpus, ec)) {
        std::vector<fs::path> corpus_pdfs;
        for (const auto& e : fs::directory_iterator(corpus)) {
            if (e.is_regular_file() && e.path().extension() == ".pdf") {
                corpus_pdfs.push_back(e.path());
            }
        }
        std::sort(corpus_pdfs.begin(), corpus_pdfs.end());
        for (const fs::path& pdf_path : corpus_pdfs) {
            fs::copy_file(pdf_path, dir / pdf_path.filename(),
                          fs::copy_options::overwrite_existing, ec);
            EXPECT_FALSE(ec) << "Failed copying corpus PDF into examples/: " << pdf_path << " ("
                             << ec.message() << ")";
            inputs.push_back({pdf_path, pdf_path.stem().string(), true});
        }
    }

    ASSERT_GE(inputs.size(), kFixtures.size());
    for (const Input& in : inputs) {
        SCOPED_TRACE(in.name);
        const fs::path md_path = dir / (in.name + ".md");
        const fs::path img_dir = in.images ? (dir / "images" / in.name) : fs::path{};
        const CliResult res = run_pdf2md(cli, in.source, md_path, img_dir);

        ASSERT_EQ(res.exit_code, 0) << "pdf2md failed on " << in.name
                                    << "\nstderr: " << res.stderr_bytes;
        EXPECT_TRUE(res.stdout_bytes.empty()) << "Unexpected stdout on " << in.name;
        assert_clean_summary_stderr(res.stderr_bytes, in.name);

        ASSERT_TRUE(fs::exists(md_path)) << "Missing markdown output for " << in.name;
        const std::string md = read_text(md_path);
        assert_document_landmarks(in.name, md, dir);

        // Every image link emitted in the markdown must resolve to a valid PNG on disk.
        const std::vector<std::string> links = extract_markdown_image_links(md);
        for (const std::string& rel_link : links) {
            SCOPED_TRACE(rel_link);
            assert_valid_png(dir / fs::path(rel_link));
        }
    }
}
