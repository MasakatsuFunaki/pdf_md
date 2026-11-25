// Black-box system tests exercising the built pdf2md CLI executable across OS
// process boundaries, filesystem paths, flags, and exit-code contracts.

#include "fixture_pdfs.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <hpdf.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
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

#ifndef PDF2MD_CLI_EXE
#define PDF2MD_CLI_EXE ""
#endif

namespace {

namespace fs = std::filesystem;
using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::StartsWith;

constexpr int kExitOk = 0;
constexpr int kExitBadArgs = 1;
constexpr int kExitPdfError = 2;
constexpr int kExitIoError = 3;

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

uint64_t current_process_id() noexcept {
#ifdef _WIN32
    return static_cast<uint64_t>(GetCurrentProcessId());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

class TempSandbox {
public:
    explicit TempSandbox(std::string_view label) {
        static std::atomic<uint64_t> counter{0};
        const uint64_t seq = counter.fetch_add(1, std::memory_order_relaxed);
        const auto ticks = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const std::string folder = "pdf2md_cli_st_" + std::string(label) + "_" +
                                   std::to_string(current_process_id()) + "_" +
                                   std::to_string(seq) + "_" + std::to_string(ticks);
        dir_ = fs::temp_directory_path() / folder;
        fs::create_directories(dir_);
    }

    ~TempSandbox() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    TempSandbox(const TempSandbox&) = delete;
    TempSandbox& operator=(const TempSandbox&) = delete;

    const fs::path& dir() const noexcept { return dir_; }

private:
    fs::path dir_;
};

#ifdef _WIN32
std::wstring utf8_to_wide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        throw std::runtime_error("MultiByteToWideChar failed on CLI argument");
    }
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                        static_cast<int>(utf8.size()), wide.data(), needed);
    return wide;
}

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

struct CliArg {
#ifdef _WIN32
    std::wstring value;
    CliArg(const char* s) : value(utf8_to_wide(s)) {}
    CliArg(std::string_view s) : value(utf8_to_wide(s)) {}
    CliArg(const std::string& s) : value(utf8_to_wide(s)) {}
    CliArg(const fs::path& p) : value(p.wstring()) {}
#else
    std::string value;
    CliArg(const char* s) : value(s) {}
    CliArg(std::string_view s) : value(s) {}
    CliArg(const std::string& s) : value(s) {}
    CliArg(const fs::path& p) : value(p.string()) {}
#endif
};

struct CliResult {
    int exit_code = -1;
    std::string stdout_bytes;
    std::string stderr_bytes;
};

fs::path require_cli_exe() {
    const fs::path cli(PDF2MD_CLI_EXE);
    if (cli.empty() || !fs::exists(cli)) {
        throw std::runtime_error("PDF2MD_CLI_EXE missing or not built");
    }
    return cli;
}

// Spawns PDF2MD_CLI_EXE directly via OS process primitives (no shell), capturing
// stdout, stderr, and the exact process exit code deterministically.
CliResult run_cli(const std::vector<CliArg>& args) {
    const fs::path exe = require_cli_exe();
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

    std::wstring cmdline = quote_windows_arg(exe.wstring());
    for (const CliArg& arg : args) {
        cmdline.push_back(L' ');
        cmdline += quote_windows_arg(arg.value);
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
        throw std::runtime_error("CreateProcessW failed for pdf2md executable");
    }
    ScopedHandle proc(pi.hProcess);
    ScopedHandle primary_thread(pi.hThread);

    // Close child-side handles in parent so ReadFile sees EOF on child termination.
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

        std::string exe_str = exe.string();
        std::vector<std::string> owned_args;
        owned_args.reserve(args.size() + 1);
        owned_args.push_back(exe_str);
        for (const CliArg& a : args) {
            owned_args.push_back(a.value);
        }
        std::vector<char*> argv;
        argv.reserve(owned_args.size() + 1);
        for (std::string& s : owned_args) {
            argv.push_back(s.data());
        }
        argv.push_back(nullptr);
        execv(exe_str.c_str(), argv.data());
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

std::vector<uint8_t> read_bytes(const fs::path& p) {
    std::ifstream is(p, std::ios::binary);
    if (!is) {
        throw std::runtime_error("failed to open file for reading");
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(is),
                                std::istreambuf_iterator<char>());
}

std::string read_text(const fs::path& p) {
    std::ifstream is(p, std::ios::binary);
    if (!is) {
        throw std::runtime_error("failed to open markdown file for reading");
    }
    return std::string(std::istreambuf_iterator<char>(is), std::istreambuf_iterator<char>());
}

uint32_t read_be_u32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24U) | (static_cast<uint32_t>(p[1]) << 16U) |
           (static_cast<uint32_t>(p[2]) << 8U) | static_cast<uint32_t>(p[3]);
}

struct PngHeaderInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t bit_depth = 0;
    uint8_t color_type = 0;
};

void assert_valid_png(const fs::path& png_path, PngHeaderInfo* out_info = nullptr) {
    ASSERT_TRUE(fs::exists(png_path)) << "Missing PNG file: " << png_path;
    const std::vector<uint8_t> bytes = read_bytes(png_path);
    ASSERT_GE(bytes.size(), kMinValidPngBytes) << "PNG truncated: " << png_path;
    ASSERT_EQ(std::memcmp(bytes.data(), kPngSignature.data(), kPngSignature.size()), 0)
        << "Invalid PNG signature in " << png_path;

    const uint32_t ihdr_len = read_be_u32(bytes.data() + kIhdrChunkOffset);
    ASSERT_EQ(ihdr_len, kIhdrDataLength) << "Unexpected IHDR length in " << png_path;
    ASSERT_EQ(std::memcmp(bytes.data() + kIhdrChunkOffset + 4, kIhdrTag.data(), kIhdrTag.size()), 0)
        << "First PNG chunk is not IHDR in " << png_path;

    const uint32_t width = read_be_u32(bytes.data() + kIhdrChunkOffset + 8);
    const uint32_t height = read_be_u32(bytes.data() + kIhdrChunkOffset + 12);
    const uint8_t bit_depth = bytes[kIhdrChunkOffset + 16];
    const uint8_t color_type = bytes[kIhdrChunkOffset + 17];

    EXPECT_GT(width, 0U) << "Zero PNG width in " << png_path;
    EXPECT_GT(height, 0U) << "Zero PNG height in " << png_path;
    EXPECT_EQ(bit_depth, 8U) << "Unexpected PNG bit depth in " << png_path;
    EXPECT_TRUE(color_type == 0U || color_type == 2U || color_type == 3U || color_type == 4U ||
                color_type == 6U)
        << "Invalid PNG color type in " << png_path;

    const uint8_t* tail = bytes.data() + (bytes.size() - kIendTrailerBytes);
    EXPECT_EQ(std::memcmp(tail, kIendTrailer.data(), kIendTrailer.size()), 0)
        << "Missing canonical IEND trailer in " << png_path;

    if (out_info != nullptr) {
        *out_info = PngHeaderInfo{width, height, bit_depth, color_type};
    }
}

// Verifies default non-quiet stderr holds strictly the single conversion summary
// milestone line (`pdf2md: converted <N> page(s), <M> block(s)...`) with zero
// unexpected warning, error, or debug lines.
void assert_summary_milestone_stderr(std::string_view stderr_bytes, int expected_pages = -1) {
    ASSERT_FALSE(stderr_bytes.empty()) << "Expected conversion summary line on stderr";
    std::string_view trimmed = stderr_bytes;
    if (!trimmed.empty() && trimmed.back() == '\n') {
        trimmed.remove_suffix(1);
    }
    if (!trimmed.empty() && trimmed.back() == '\r') {
        trimmed.remove_suffix(1);
    }
    EXPECT_EQ(trimmed.find('\n'), std::string_view::npos)
        << "Unexpected extra lines on stderr: " << stderr_bytes;
    EXPECT_EQ(trimmed.find('\r'), std::string_view::npos)
        << "Unexpected carriage return inside stderr: " << stderr_bytes;

    constexpr std::string_view kPrefix = "pdf2md: converted ";
    ASSERT_THAT(std::string(trimmed), StartsWith(std::string(kPrefix)));
    EXPECT_THAT(std::string(trimmed), HasSubstr(" page(s), "));
    EXPECT_THAT(std::string(trimmed), HasSubstr(" block(s)"));

    if (expected_pages >= 0) {
        const std::string expected_prefix =
            std::string(kPrefix) + std::to_string(expected_pages) + " page(s), ";
        EXPECT_THAT(std::string(trimmed), StartsWith(expected_prefix));
    }
}

std::vector<fs::path> list_regular_files_sorted(const fs::path& dir) {
    std::vector<fs::path> files;
    if (!fs::exists(dir)) {
        return files;
    }
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

void hpdf_throw_on_error(HPDF_STATUS error_no, HPDF_STATUS detail_no, void*) {
    char buf[96]{};
    std::snprintf(buf, sizeof(buf), "libharu error 0x%04X detail %u",
                  static_cast<unsigned>(error_no), static_cast<unsigned>(detail_no));
    throw std::runtime_error(buf);
}

class ScopedHpdfDoc {
public:
    ScopedHpdfDoc() : doc_(HPDF_New(hpdf_throw_on_error, nullptr)) {
        if (doc_ == nullptr) {
            throw std::runtime_error("HPDF_New failed");
        }
    }
    ~ScopedHpdfDoc() { HPDF_Free(doc_); }
    ScopedHpdfDoc(const ScopedHpdfDoc&) = delete;
    ScopedHpdfDoc& operator=(const ScopedHpdfDoc&) = delete;

    HPDF_Doc get() const noexcept { return doc_; }

private:
    HPDF_Doc doc_ = nullptr;
};

void make_rotated_watermark_pdf(const fs::path& out) {
    ScopedHpdfDoc holder;
    HPDF_Doc pdf = holder.get();
    HPDF_Font regular = HPDF_GetFont(pdf, "Helvetica", "WinAnsiEncoding");
    HPDF_Font mono = HPDF_GetFont(pdf, "Courier", "WinAnsiEncoding");
    HPDF_Page page = HPDF_AddPage(pdf);
    HPDF_Page_SetSize(page, HPDF_PAGE_SIZE_LETTER, HPDF_PAGE_PORTRAIT);

    HPDF_Page_BeginText(page);
    HPDF_Page_SetFontAndSize(page, regular, 12.0f);
    HPDF_Page_TextOut(page, 72.0f, 700.0f,
                      "Standard horizontal body paragraph stays readable.");
    HPDF_Page_EndText(page);

    // Place 45-degree rotated monospace glyphs along a horizontal baseline so
    // rotated-glyph filtering skips them by default while `--keep-rotated` emits
    // the exact stamp word without proportional-width gaps.
    constexpr const char* kStamp = "CONFIDENTIALSTAMP";
    constexpr float kCos45 = 0.70710678f;
    constexpr float kSin45 = 0.70710678f;
    HPDF_Page_BeginText(page);
    HPDF_Page_SetFontAndSize(page, mono, 14.0f);
    for (size_t i = 0; kStamp[i] != '\0'; ++i) {
        const char glyph[2] = {kStamp[i], '\0'};
        const float gx = 72.0f + static_cast<float>(i) * 8.4f;
        HPDF_Page_SetTextMatrix(page, kCos45, kSin45, -kSin45, kCos45, gx, 360.0f);
        HPDF_Page_ShowText(page, glyph);
    }
    HPDF_Page_EndText(page);

    HPDF_SaveToFile(pdf, out.string().c_str());
}

void make_encrypted_pdf(const fs::path& out, const char* owner_pw, const char* user_pw) {
    ScopedHpdfDoc holder;
    HPDF_Doc pdf = holder.get();
    HPDF_Font regular = HPDF_GetFont(pdf, "Helvetica", "WinAnsiEncoding");
    HPDF_Page page = HPDF_AddPage(pdf);
    HPDF_Page_SetSize(page, HPDF_PAGE_SIZE_LETTER, HPDF_PAGE_PORTRAIT);

    HPDF_Page_BeginText(page);
    HPDF_Page_SetFontAndSize(page, regular, 12.0f);
    HPDF_Page_TextOut(page, 72.0f, 700.0f, "Unlocked confidential body paragraph.");
    HPDF_Page_EndText(page);

    HPDF_SetPassword(pdf, owner_pw, user_pw);
    HPDF_SetEncryptionMode(pdf, HPDF_ENCRYPT_R3, 16);
    HPDF_SaveToFile(pdf, out.string().c_str());
}

TEST(Cli, OutputDestinationsAndSummaryVerbosity) {
    const fs::path cli_exe(PDF2MD_CLI_EXE);
    ASSERT_FALSE(cli_exe.empty()) << "PDF2MD_CLI_EXE macro is empty";
    ASSERT_TRUE(fs::exists(cli_exe)) << "Built CLI binary missing: " << cli_exe;

    TempSandbox sandbox("destinations");
    const fs::path sibling_dir = sandbox.dir() / "sibling_case";
    fs::create_directories(sibling_dir);
    const fs::path input_pdf = sibling_dir / "basic.pdf";
    pdf2md::testfx::make_basic(input_pdf);
    const std::vector<uint8_t> pdf_before = read_bytes(input_pdf);

    // Default invocation: writes <stem>.md beside input PDF and logs summary to stderr.
    const CliResult res_default = run_cli({input_pdf});
    ASSERT_EQ(res_default.exit_code, kExitOk) << res_default.stderr_bytes;
    EXPECT_TRUE(res_default.stdout_bytes.empty());
    assert_summary_milestone_stderr(res_default.stderr_bytes, 3);

    const fs::path sibling_md_path = sibling_dir / "basic.md";
    ASSERT_TRUE(fs::exists(sibling_md_path));
    const std::string sibling_md = read_text(sibling_md_path);

    EXPECT_THAT(sibling_md, HasSubstr("# Sample Document Title\n"));
    EXPECT_THAT(sibling_md, HasSubstr("## First Section\n"));
    EXPECT_THAT(sibling_md, HasSubstr("This is the first paragraph of the sample document. "
                                      "It spans multiple lines and should be joined into a "
                                      "single markdown paragraph without unwanted line breaks."));
    EXPECT_THAT(sibling_md, HasSubstr("detect **bold text** and also *italic text* inside"));
    EXPECT_THAT(sibling_md, HasSubstr("- First bullet item\n"));
    EXPECT_THAT(sibling_md, HasSubstr("    - Nested bullet item\n"));
    EXPECT_THAT(sibling_md, HasSubstr("1. Alpha step\n"));
    EXPECT_THAT(sibling_md, HasSubstr("```\nint main() {\n    return 42;\n}\n```"));
    EXPECT_THAT(sibling_md, HasSubstr("exercises the hyphenation repair logic"));
    EXPECT_THAT(sibling_md, HasSubstr("split across the page boundary, which is exactly what "
                                      "this final sentence verifies."));
    EXPECT_THAT(sibling_md, Not(HasSubstr("PDF2MD Fixture Suite")));
    EXPECT_THAT(sibling_md, Not(HasSubstr("Page 2")));

    // Explicit `-o <file>` + `-q` short flag: byte-for-byte identical .md, empty stderr.
    const fs::path explicit_short_q = sandbox.dir() / "explicit_short_q.md";
    const CliResult res_short_q = run_cli({input_pdf, "-o", explicit_short_q, "-q"});
    ASSERT_EQ(res_short_q.exit_code, kExitOk) << res_short_q.stderr_bytes;
    EXPECT_TRUE(res_short_q.stdout_bytes.empty());
    EXPECT_TRUE(res_short_q.stderr_bytes.empty());
    EXPECT_EQ(read_text(explicit_short_q), sibling_md);

    // Explicit `--output <file>` + `--quiet` long flag: identical .md, empty stderr.
    const fs::path explicit_long_q = sandbox.dir() / "explicit_long_q.md";
    const CliResult res_long_q = run_cli({input_pdf, "--output", explicit_long_q, "--quiet"});
    ASSERT_EQ(res_long_q.exit_code, kExitOk) << res_long_q.stderr_bytes;
    EXPECT_TRUE(res_long_q.stdout_bytes.empty());
    EXPECT_TRUE(res_long_q.stderr_bytes.empty());
    EXPECT_EQ(read_text(explicit_long_q), sibling_md);

    // `-o -` writes markdown to stdout with byte-for-byte parity and creates no '-' file.
    const CliResult res_stdout = run_cli({input_pdf, "-o", "-", "--quiet"});
    ASSERT_EQ(res_stdout.exit_code, kExitOk) << res_stdout.stderr_bytes;
    EXPECT_TRUE(res_stdout.stderr_bytes.empty());
    EXPECT_EQ(res_stdout.stdout_bytes, sibling_md);
    EXPECT_FALSE(fs::exists(sandbox.dir() / "-"));
    EXPECT_FALSE(fs::exists(sibling_dir / "-"));

    EXPECT_EQ(read_bytes(input_pdf), pdf_before);
}

TEST(Cli, MultiFeatureOptionsAndImageExtraction) {
    const fs::path cli_exe(PDF2MD_CLI_EXE);
    ASSERT_FALSE(cli_exe.empty());
    ASSERT_TRUE(fs::exists(cli_exe)) << "Built CLI binary missing: " << cli_exe;

    TempSandbox sandbox("multifeature");
    const fs::path basic_pdf = sandbox.dir() / "basic.pdf";
    const fs::path image_pdf = sandbox.dir() / "image.pdf";
    pdf2md::testfx::make_basic(basic_pdf);
    pdf2md::testfx::make_image(image_pdf);
    const std::vector<uint8_t> basic_before = read_bytes(basic_pdf);
    const std::vector<uint8_t> image_before = read_bytes(image_pdf);

    // Multi-page subsetting + YAML front matter + explicit horizontal-rule page breaks.
    const fs::path paged_md = sandbox.dir() / "basic_pages_1_2.md";
    const CliResult res_paged = run_cli(
        {basic_pdf, "-o", paged_md, "--front-matter", "--page-breaks", "--pages", "1-2"});
    ASSERT_EQ(res_paged.exit_code, kExitOk) << res_paged.stderr_bytes;
    EXPECT_TRUE(res_paged.stdout_bytes.empty());
    assert_summary_milestone_stderr(res_paged.stderr_bytes, 2);

    const std::string paged_body = read_text(paged_md);
    EXPECT_THAT(paged_body, StartsWith("---\ntitle: \"Basic Fixture Document\"\n"
                                       "author: \"pdf2md tests\"\n"
                                       "source: \"basic.pdf\"\npages: 3\n---\n\n"));
    const size_t p1_pos = paged_body.find("# Sample Document Title");
    const size_t rule_pos = paged_body.find("\n\n---\n\n");
    const size_t p2_pos = paged_body.find("## Hyphenation");
    ASSERT_NE(p1_pos, std::string::npos);
    ASSERT_NE(rule_pos, std::string::npos);
    ASSERT_NE(p2_pos, std::string::npos);
    EXPECT_LT(p1_pos, rule_pos);
    EXPECT_LT(rule_pos, p2_pos);
    EXPECT_THAT(paged_body, Not(HasSubstr("Final page content for the header stripping")));

    // Explicit `--images <dir>` + `--front-matter`: PNG written with valid IHDR & linked in order.
    const fs::path custom_md = sandbox.dir() / "with_custom_images.md";
    const fs::path custom_img_dir = sandbox.dir() / "custom_assets";
    const CliResult res_img =
        run_cli({image_pdf, "-o", custom_md, "--images", custom_img_dir, "--front-matter"});
    ASSERT_EQ(res_img.exit_code, kExitOk) << res_img.stderr_bytes;
    EXPECT_TRUE(res_img.stdout_bytes.empty());
    assert_summary_milestone_stderr(res_img.stderr_bytes, 1);

    const std::string img_body = read_text(custom_md);
    EXPECT_THAT(img_body, StartsWith("---\ntitle: \"Image Fixture\"\n"
                                     "source: \"image.pdf\"\npages: 1\n---\n\n"));
    EXPECT_THAT(img_body, HasSubstr("![](custom_assets/image_1.png)"));
    const size_t above = img_body.find("A paragraph above the picture.");
    const size_t embed = img_body.find("![](custom_assets/image_1.png)");
    const size_t below = img_body.find("A paragraph below the picture.");
    ASSERT_NE(above, std::string::npos);
    ASSERT_NE(embed, std::string::npos);
    ASSERT_NE(below, std::string::npos);
    EXPECT_LT(above, embed);
    EXPECT_LT(embed, below);

    const std::vector<fs::path> custom_pngs = list_regular_files_sorted(custom_img_dir);
    ASSERT_EQ(custom_pngs.size(), 1U);
    EXPECT_EQ(custom_pngs[0].filename(), fs::path("image_1.png"));
    PngHeaderInfo png_info{};
    assert_valid_png(custom_pngs[0], &png_info);
    EXPECT_GE(png_info.width, 8U);
    EXPECT_GE(png_info.height, 8U);

    // Default CLI image behavior (no `--images` flag): extracts into `<stem>_images/`.
    const fs::path default_img_md = sandbox.dir() / "default_img.md";
    const fs::path derived_img_dir = sandbox.dir() / "default_img_images";
    const CliResult res_default_img = run_cli({image_pdf, "-o", default_img_md});
    ASSERT_EQ(res_default_img.exit_code, kExitOk) << res_default_img.stderr_bytes;
    assert_summary_milestone_stderr(res_default_img.stderr_bytes, 1);
    EXPECT_THAT(read_text(default_img_md), HasSubstr("![](default_img_images/image_1.png)"));
    assert_valid_png(derived_img_dir / "image_1.png");

    // `--no-images`: suppresses image extraction and omits directory creation.
    const fs::path no_img_md = sandbox.dir() / "suppressed_img.md";
    const fs::path suppressed_img_dir = sandbox.dir() / "suppressed_img_images";
    const CliResult res_no_img = run_cli({image_pdf, "-o", no_img_md, "--no-images"});
    ASSERT_EQ(res_no_img.exit_code, kExitOk) << res_no_img.stderr_bytes;
    assert_summary_milestone_stderr(res_no_img.stderr_bytes, 1);
    const std::string no_img_body = read_text(no_img_md);
    EXPECT_THAT(no_img_body, HasSubstr("A paragraph above the picture."));
    EXPECT_THAT(no_img_body, HasSubstr("A paragraph below the picture."));
    EXPECT_THAT(no_img_body, Not(HasSubstr("![](")));
    EXPECT_FALSE(fs::exists(suppressed_img_dir));

    EXPECT_EQ(read_bytes(basic_pdf), basic_before);
    EXPECT_EQ(read_bytes(image_pdf), image_before);
}

TEST(Cli, LayoutFlagsAlterOutputPerContract) {
    const fs::path cli_exe(PDF2MD_CLI_EXE);
    ASSERT_FALSE(cli_exe.empty());
    ASSERT_TRUE(fs::exists(cli_exe)) << "Built CLI binary missing: " << cli_exe;

    TempSandbox sandbox("layout_flags");
    const fs::path basic_pdf = sandbox.dir() / "basic.pdf";
    const fs::path table_pdf = sandbox.dir() / "table.pdf";
    const fs::path rotated_pdf = sandbox.dir() / "rotated.pdf";
    pdf2md::testfx::make_basic(basic_pdf);
    pdf2md::testfx::make_table(table_pdf);
    make_rotated_watermark_pdf(rotated_pdf);

    const std::vector<uint8_t> basic_before = read_bytes(basic_pdf);
    const std::vector<uint8_t> table_before = read_bytes(table_pdf);
    const std::vector<uint8_t> rotated_before = read_bytes(rotated_pdf);

    // Baseline basic conversion vs `--keep-headers` and `--no-headings`.
    const fs::path basic_default_md = sandbox.dir() / "basic_default.md";
    const fs::path basic_headers_md = sandbox.dir() / "basic_keep_headers.md";
    const fs::path basic_nohead_md = sandbox.dir() / "basic_no_headings.md";

    const CliResult r_basic_def = run_cli({basic_pdf, "-o", basic_default_md});
    const CliResult r_basic_hdr = run_cli({basic_pdf, "-o", basic_headers_md, "--keep-headers"});
    const CliResult r_basic_noh = run_cli({basic_pdf, "-o", basic_nohead_md, "--no-headings"});
    ASSERT_EQ(r_basic_def.exit_code, kExitOk);
    ASSERT_EQ(r_basic_hdr.exit_code, kExitOk);
    ASSERT_EQ(r_basic_noh.exit_code, kExitOk);
    assert_summary_milestone_stderr(r_basic_def.stderr_bytes, 3);
    assert_summary_milestone_stderr(r_basic_hdr.stderr_bytes, 3);
    assert_summary_milestone_stderr(r_basic_noh.stderr_bytes, 3);

    const std::string md_basic_def = read_text(basic_default_md);
    const std::string md_basic_hdr = read_text(basic_headers_md);
    const std::string md_basic_noh = read_text(basic_nohead_md);

    EXPECT_THAT(md_basic_def, Not(HasSubstr("PDF2MD Fixture Suite")));
    EXPECT_THAT(md_basic_def, Not(HasSubstr("Page 2")));
    EXPECT_THAT(md_basic_hdr, HasSubstr("PDF2MD Fixture Suite"));
    EXPECT_THAT(md_basic_hdr, HasSubstr("Page 2"));
    EXPECT_THAT(md_basic_hdr, HasSubstr("# Sample Document Title"));

    EXPECT_THAT(md_basic_def, HasSubstr("# Sample Document Title\n"));
    EXPECT_THAT(md_basic_def, HasSubstr("## First Section\n"));
    EXPECT_THAT(md_basic_noh, Not(HasSubstr("# Sample Document Title")));
    EXPECT_THAT(md_basic_noh, Not(HasSubstr("## First Section")));
    EXPECT_THAT(md_basic_noh, HasSubstr("Sample Document Title"));
    EXPECT_THAT(md_basic_noh, HasSubstr("First Section"));

    // Baseline table conversion vs `--no-tables`.
    const fs::path table_default_md = sandbox.dir() / "table_default.md";
    const fs::path table_disabled_md = sandbox.dir() / "table_disabled.md";
    const CliResult r_tbl_def = run_cli({table_pdf, "-o", table_default_md});
    const CliResult r_tbl_off = run_cli({table_pdf, "-o", table_disabled_md, "--no-tables"});
    ASSERT_EQ(r_tbl_def.exit_code, kExitOk);
    ASSERT_EQ(r_tbl_off.exit_code, kExitOk);
    assert_summary_milestone_stderr(r_tbl_def.stderr_bytes, 1);
    assert_summary_milestone_stderr(r_tbl_off.stderr_bytes, 1);

    const std::string md_tbl_def = read_text(table_default_md);
    const std::string md_tbl_off = read_text(table_disabled_md);
    EXPECT_THAT(md_tbl_def, HasSubstr("| Name | Quantity | Price |\n"
                                      "| --- | --- | --- |\n"
                                      "| Apples | 12 | 1.50 |\n"
                                      "| Bananas | 7 | 0.75 |\n"
                                      "| Cherries | 31 | 4.20 |"));
    EXPECT_THAT(md_tbl_off, Not(HasSubstr("| --- |")));
    EXPECT_THAT(md_tbl_off, Not(HasSubstr("| Name |")));
    EXPECT_THAT(md_tbl_off, HasSubstr("Apples"));
    EXPECT_THAT(md_tbl_off, HasSubstr("Cherries"));
    EXPECT_THAT(md_tbl_off, HasSubstr("A regular paragraph after the table"));

    // Baseline rotated-text suppression vs `--keep-rotated`.
    const fs::path rot_default_md = sandbox.dir() / "rotated_default.md";
    const fs::path rot_kept_md = sandbox.dir() / "rotated_kept.md";
    const CliResult r_rot_def = run_cli({rotated_pdf, "-o", rot_default_md});
    const CliResult r_rot_kept = run_cli({rotated_pdf, "-o", rot_kept_md, "--keep-rotated"});
    ASSERT_EQ(r_rot_def.exit_code, kExitOk);
    ASSERT_EQ(r_rot_kept.exit_code, kExitOk);
    assert_summary_milestone_stderr(r_rot_def.stderr_bytes, 1);
    assert_summary_milestone_stderr(r_rot_kept.stderr_bytes, 1);

    const std::string md_rot_def = read_text(rot_default_md);
    const std::string md_rot_kept = read_text(rot_kept_md);
    EXPECT_THAT(md_rot_def, HasSubstr("Standard horizontal body paragraph stays readable."));
    EXPECT_THAT(md_rot_def, Not(HasSubstr("CONFIDENTIALSTAMP")));
    EXPECT_THAT(md_rot_kept, HasSubstr("Standard horizontal body paragraph stays readable."));
    EXPECT_THAT(md_rot_kept, HasSubstr("CONFIDENTIALSTAMP"));

    EXPECT_EQ(read_bytes(basic_pdf), basic_before);
    EXPECT_EQ(read_bytes(table_pdf), table_before);
    EXPECT_EQ(read_bytes(rotated_pdf), rotated_before);
}

TEST(Cli, UnicodeAndSpacedFilesystemPaths) {
    const fs::path cli_exe(PDF2MD_CLI_EXE);
    ASSERT_FALSE(cli_exe.empty());
    ASSERT_TRUE(fs::exists(cli_exe)) << "Built CLI binary missing: " << cli_exe;

    TempSandbox sandbox("unicode_paths");

    // Generate to an ASCII seed first because libharu's C89 fopen uses narrow paths on Windows,
    // then copy into non-ASCII + space-bearing directories to test pdf2md's Unicode argv & I/O.
    const fs::path seed_pdf = sandbox.dir() / "seed_image.pdf";
    pdf2md::testfx::make_image(seed_pdf);

    const fs::path in_dir = sandbox.dir() / fs::path(u8"вход café 資料 2026");
    const fs::path out_dir = sandbox.dir() / fs::path(u8"выход résumé 出力 dir");
    const fs::path img_dir = out_dir / fs::path(u8"картинки 画像 assets");
    fs::create_directories(in_dir);
    fs::create_directories(out_dir);

    const fs::path unicode_pdf = in_dir / fs::path(u8"документ テスト sample.pdf");
    const fs::path unicode_md = out_dir / fs::path(u8"отчёт 結果 report.md");
    fs::copy_file(seed_pdf, unicode_pdf, fs::copy_options::overwrite_existing);
    const std::vector<uint8_t> pdf_before = read_bytes(unicode_pdf);

    const CliResult res = run_cli({unicode_pdf, "-o", unicode_md, "--images", img_dir});
    ASSERT_EQ(res.exit_code, kExitOk) << res.stderr_bytes;
    EXPECT_TRUE(res.stdout_bytes.empty());
    assert_summary_milestone_stderr(res.stderr_bytes, 1);

    ASSERT_TRUE(fs::exists(unicode_md));
    const std::string md = read_text(unicode_md);
    EXPECT_THAT(md, HasSubstr("A paragraph above the picture."));
    EXPECT_THAT(md, HasSubstr("A paragraph below the picture."));
    EXPECT_THAT(
        md,
        HasSubstr("![](<\xD0\xBA\xD0\xB0\xD1\x80\xD1\x82\xD0\xB8\xD0\xBD\xD0\xBA\xD0\xB8 "
                  "\xE7\x94\xBB\xE5\x83\x8F assets/image_1.png>)"));

    const std::vector<fs::path> pngs = list_regular_files_sorted(img_dir);
    ASSERT_EQ(pngs.size(), 1U);
    assert_valid_png(pngs[0]);
    EXPECT_EQ(read_bytes(unicode_pdf), pdf_before);
}

TEST(Cli, ErrorExitCodeContractsAndZeroPartialOutput) {
    const fs::path cli_exe(PDF2MD_CLI_EXE);
    ASSERT_FALSE(cli_exe.empty());
    ASSERT_TRUE(fs::exists(cli_exe)) << "Built CLI binary missing: " << cli_exe;

    TempSandbox sandbox("error_contracts");
    const fs::path valid_pdf = sandbox.dir() / "basic.pdf";
    const fs::path corrupt_pdf = sandbox.dir() / "corrupt.pdf";
    const fs::path encrypted_pdf = sandbox.dir() / "encrypted.pdf";
    const fs::path existing_dir = sandbox.dir() / "existing_output_dir";

    pdf2md::testfx::make_basic(valid_pdf);
    {
        std::ofstream bad(corrupt_pdf, std::ios::binary);
        bad << "not a valid PDF header or xref payload";
    }
    make_encrypted_pdf(encrypted_pdf, "ownerSecret99", "readerSecret42");
    fs::create_directories(existing_dir);

    const std::vector<uint8_t> valid_before = read_bytes(valid_pdf);
    const std::vector<uint8_t> encrypted_before = read_bytes(encrypted_pdf);

    // Positive controls: `--help` and `--version` exit 0 cleanly, and the encrypted PDF
    // opens when supplied the matching password.
    const CliResult res_help = run_cli({"--help"});
    EXPECT_EQ(res_help.exit_code, kExitOk);
    EXPECT_THAT(res_help.stdout_bytes, HasSubstr("--pages"));
    EXPECT_TRUE(res_help.stderr_bytes.empty());

    const CliResult res_version = run_cli({"--version"});
    EXPECT_EQ(res_version.exit_code, kExitOk);
    EXPECT_FALSE(res_version.stdout_bytes.empty());
    EXPECT_TRUE(res_version.stderr_bytes.empty());

    const fs::path unlocked_md = sandbox.dir() / "unlocked.md";
    const CliResult res_unlocked =
        run_cli({encrypted_pdf, "--password", "readerSecret42", "-o", unlocked_md});
    ASSERT_EQ(res_unlocked.exit_code, kExitOk) << res_unlocked.stderr_bytes;
    assert_summary_milestone_stderr(res_unlocked.stderr_bytes, 1);
    EXPECT_THAT(read_text(unlocked_md), HasSubstr("Unlocked confidential body paragraph."));

    struct ErrorCase {
        const char* name;
        std::vector<CliArg> args;
        int expected_exit;
        fs::path forbidden_output;
    };

    const fs::path missing_pdf = sandbox.dir() / "does_not_exist.pdf";
    const fs::path corrupt_img_dir = sandbox.dir() / "corrupt_imgs";
    const std::vector<ErrorCase> cases = {
        {"missing_required_input", {}, kExitBadArgs, fs::path{}},
        {"unknown_flag",
         {valid_pdf, "--nonexistent-cli-flag", "-o", sandbox.dir() / "err_unknown.md"},
         kExitBadArgs,
         sandbox.dir() / "err_unknown.md"},
        {"pages_zero",
         {valid_pdf, "--pages", "0", "-o", sandbox.dir() / "err_pages0.md"},
         kExitBadArgs,
         sandbox.dir() / "err_pages0.md"},
        {"pages_non_numeric",
         {valid_pdf, "--pages", "abc", "-o", sandbox.dir() / "err_pages_nan.md"},
         kExitBadArgs,
         sandbox.dir() / "err_pages_nan.md"},
        {"missing_input_pdf",
         {missing_pdf, "-o", sandbox.dir() / "err_missing.md"},
         kExitPdfError,
         sandbox.dir() / "err_missing.md"},
        {"corrupt_input_pdf",
         {corrupt_pdf, "-o", sandbox.dir() / "err_corrupt.md", "--images", corrupt_img_dir},
         kExitPdfError,
         sandbox.dir() / "err_corrupt.md"},
        {"wrong_password",
         {encrypted_pdf, "--password", "wrongPassword", "-o", sandbox.dir() / "err_pw.md"},
         kExitPdfError,
         sandbox.dir() / "err_pw.md"},
        {"output_path_is_directory",
         {valid_pdf, "-o", existing_dir},
         kExitIoError,
         fs::path{}},
    };

    ASSERT_EQ(cases.size(), 8U);
    for (const ErrorCase& tc : cases) {
        SCOPED_TRACE(tc.name);
        const CliResult res = run_cli(tc.args);
        EXPECT_EQ(res.exit_code, tc.expected_exit)
            << "stdout: " << res.stdout_bytes << "\nstderr: " << res.stderr_bytes;
        EXPECT_TRUE(res.stdout_bytes.empty());
        EXPECT_FALSE(res.stderr_bytes.empty()) << "Expected diagnostic message on stderr";
        if (!tc.forbidden_output.empty()) {
            EXPECT_FALSE(fs::exists(tc.forbidden_output))
                << "Partial output file left behind: " << tc.forbidden_output;
        }
    }

    EXPECT_TRUE(list_regular_files_sorted(corrupt_img_dir).empty());
    EXPECT_TRUE(fs::is_directory(existing_dir));
    EXPECT_TRUE(list_regular_files_sorted(existing_dir).empty());
    EXPECT_EQ(read_bytes(valid_pdf), valid_before);
    EXPECT_EQ(read_bytes(encrypted_pdf), encrypted_before);
}

TEST(Cli, IdempotentRerunAndOverwriteSafety) {
    const fs::path cli_exe(PDF2MD_CLI_EXE);
    ASSERT_FALSE(cli_exe.empty());
    ASSERT_TRUE(fs::exists(cli_exe)) << "Built CLI binary missing: " << cli_exe;

    TempSandbox sandbox("idempotence");
    const fs::path input_pdf = sandbox.dir() / "image.pdf";
    pdf2md::testfx::make_image(input_pdf);
    const std::vector<uint8_t> pdf_before = read_bytes(input_pdf);

    const fs::path out_md = sandbox.dir() / "idempotent.md";
    const fs::path img_dir = sandbox.dir() / "idempotent_assets";

    // Pre-populate stale content in target .md to verify clean truncation/overwrite, not append.
    {
        std::ofstream stale(out_md, std::ios::binary);
        stale << std::string(4096, 'X');
    }

    const CliResult run1 =
        run_cli({input_pdf, "-o", out_md, "--images", img_dir, "--front-matter", "--page-breaks"});
    ASSERT_EQ(run1.exit_code, kExitOk) << run1.stderr_bytes;
    assert_summary_milestone_stderr(run1.stderr_bytes, 1);

    const std::string md_first = read_text(out_md);
    EXPECT_THAT(md_first, Not(HasSubstr("XXXX")));
    EXPECT_THAT(md_first, StartsWith("---\ntitle: \"Image Fixture\"\n"));
    EXPECT_THAT(md_first, HasSubstr("![](idempotent_assets/image_1.png)"));

    const std::vector<fs::path> pngs_first = list_regular_files_sorted(img_dir);
    ASSERT_EQ(pngs_first.size(), 1U);
    assert_valid_png(pngs_first[0]);
    const std::vector<uint8_t> png_bytes_first = read_bytes(pngs_first[0]);

    const CliResult run2 =
        run_cli({input_pdf, "-o", out_md, "--images", img_dir, "--front-matter", "--page-breaks"});
    ASSERT_EQ(run2.exit_code, kExitOk) << run2.stderr_bytes;
    assert_summary_milestone_stderr(run2.stderr_bytes, 1);

    EXPECT_EQ(read_text(out_md), md_first);
    const std::vector<fs::path> pngs_second = list_regular_files_sorted(img_dir);
    ASSERT_EQ(pngs_second, pngs_first);
    assert_valid_png(pngs_second[0]);
    EXPECT_EQ(read_bytes(pngs_second[0]), png_bytes_first);
    EXPECT_EQ(read_bytes(input_pdf), pdf_before);
}

}  // namespace
