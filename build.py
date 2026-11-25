import argparse
from datetime import datetime
import os
import platform
import shutil
import subprocess

PROJECT_ROOT = os.path.dirname(os.path.realpath(__file__))

IS_WINDOWS = platform.system() == "Windows"
IS_AARCH64 = platform.machine().lower() in ("aarch64", "arm64")
DEFAULT_PRESET = "conan-windows-release" if IS_WINDOWS else "conan-linux-release"

# Conan writes CMakeUserPresets.json at the repo root and *accumulates* one
# include per build layout it has ever generated -- e.g. a Windows
# 'build/generators' and a Linux 'build/Release/generators'. Every layout's
# generated file defines a preset named 'conan-release', so once two are present
# CMake aborts with "Duplicate preset: conan-release" before it even reaches the
# preset we ask for. We delete it before each run so Conan regenerates it
# referencing only the current host's layout.
CONAN_USER_PRESETS = os.path.join(PROJECT_ROOT, "CMakeUserPresets.json")
BUILD_DIR = os.path.join(PROJECT_ROOT, "build")


def run_cmd(cmd):
    print(f"Running: {' '.join(cmd)}")
    subprocess.run(cmd, check=True, cwd=PROJECT_ROOT)


def rm(path):
    """Remove a file or directory tree if present (best effort)."""
    if os.path.isdir(path):
        print(f"Removing {path}")
        shutil.rmtree(path, ignore_errors=True)
    elif os.path.exists(path):
        print(f"Removing {path}")
        os.remove(path)


def generate_demo():
    """Run the freshly built pdf2md over the project's PDFs into a
    build/demo_last_run_release_<YYYY-MM-DD_HH-MM-SS> folder."""
    exe = os.path.join(BUILD_DIR, "Release", "pdf2md.exe" if IS_WINDOWS else "pdf2md")
    if not os.path.isfile(exe):
        print(f"pdf2md binary not found at {exe}; skipping demo generation")
        return

    demo_prefix = "demo_last_run_release_"
    for parent in (PROJECT_ROOT, BUILD_DIR):
        if not os.path.isdir(parent):
            continue
        for entry in sorted(os.listdir(parent)):
            if entry.startswith(demo_prefix):
                rm(os.path.join(parent, entry))

    timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    demo_dir = os.path.join(BUILD_DIR, f"{demo_prefix}{timestamp}")
    os.makedirs(demo_dir, exist_ok=True)

    cmake_build_dir = BUILD_DIR if IS_WINDOWS else os.path.join(BUILD_DIR, "Release")
    pdf_dirs = [
        os.path.join(PROJECT_ROOT, "tests", "corpus", "cache"),
        os.path.join(BUILD_DIR, "examples"),
        os.path.join(cmake_build_dir, "examples"),
        os.path.join(cmake_build_dir, "test_output"),
        os.path.join(cmake_build_dir, "test_output", "fixtures"),
    ]
    pdfs = []
    seen_names = set()
    for d in pdf_dirs:
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            key = f.lower()
            if not key.endswith(".pdf") or key in seen_names:
                continue
            full_path = os.path.join(d, f)
            if os.path.isfile(full_path):
                seen_names.add(key)
                pdfs.append(full_path)
    pdfs.sort(key=lambda p: os.path.basename(p).lower())
    if not pdfs:
        print("No PDFs found (corpus not fetched / tests not run); demo folder left empty")
        return

    failed = []
    for pdf in pdfs:
        name = os.path.splitext(os.path.basename(pdf))[0]
        out = os.path.join(demo_dir, name + ".md")
        if subprocess.run([exe, pdf, "-o", out]).returncode != 0:
            failed.append(os.path.basename(pdf))
    print(f"Demo: converted {len(pdfs) - len(failed)}/{len(pdfs)} PDFs -> {demo_dir}")
    if failed:
        print(f"Demo: failed to convert: {', '.join(failed)}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", default="default", help="Conan profile to use")
    parser.add_argument("--preset", default=DEFAULT_PRESET,
                        help="CMake preset to build (default: chosen by host OS)")
    parser.add_argument("--jobs", "-j", type=int, default=os.cpu_count(),
                        help="Parallel compile jobs (default: all CPU cores)")
    parser.add_argument("--clean", action="store_true",
                        help="Wipe the build/ tree (and CMakeUserPresets.json) for a "
                             "from-scratch build")
    parser.add_argument("--test", action="store_true", help="Run the ctest suite after building")
    parser.add_argument("--with-corpus", action="store_true",
                        help="Download the real-world corpus PDFs before building "
                             "(tests/corpus/download_corpus.ps1) so Corpus.* tests do not skip")
    args = parser.parse_args()

    # A full clean wipes the build tree; otherwise just drop the accumulated
    # presets file so Conan rewrites it for this host only. Both prevent the
    # cross-layout "Duplicate preset: conan-release" configure error.
    if args.clean:
        print("\n--- Cleaning build tree ---")
        rm(BUILD_DIR)
    rm(CONAN_USER_PRESETS)

    if args.with_corpus:
        print("\n--- Downloading corpus PDFs ---")
        run_cmd(["pwsh", os.path.join("tests", "corpus", "download_corpus.ps1")])

    # On aarch64 (e.g. Raspberry Pi 5 with a 16 KB-page kernel) ConanCenter has no
    # prebuilt pdfium binary, so pdfium is built from source here anyway. Its bundled
    # PartitionAlloc assumes 4 KB pages and SIGTRAPs at init on a 16 KB-page kernel, so
    # we export a locally-patched pdfium recipe (conan/pdfium) that adds the aarch64
    # 16 KB-page config. This is local-only; the newest recipe revision wins, so the
    # following `conan install` resolves and builds our patched pdfium from source.
    # x86_64 / Windows are untouched: they keep resolving ConanCenter's official recipe
    # and its prebuilt binary (no source build).
    if IS_AARCH64:
        print("\n--- Exporting patched pdfium recipe (aarch64 16 KB pages) ---")
        run_cmd(["conan", "export", os.path.join("conan", "pdfium"), "--version", "95.0.4629"])

    print("\n--- Running Conan ---")
    # On Linux, deps that wrap OS libraries declare Conan `*/system` packages.
    # Conan's default package_manager mode is 'check', which aborts on the first
    # missing -dev lib; 'install' + sudo lets Conan apt-install them itself.
    conan_cmd = ["conan", "install", ".", "--build=missing", f"-pr={args.profile}"]
    if not IS_WINDOWS:
        conan_cmd += [
            "-c", "tools.system.package_manager:mode=install",
            "-c", "tools.system.package_manager:sudo=True",
        ]
    run_cmd(conan_cmd)

    print("\n--- Configuring CMake ---")
    run_cmd(["cmake", "--preset", args.preset])

    print(f"\n--- Building (parallel jobs: {args.jobs}) ---")
    run_cmd(["cmake", "--build", "--preset", args.preset, "--parallel", str(args.jobs)])

    if args.test:
        print("\n--- Running Tests ---")
        run_cmd(["ctest", "--preset", args.preset])

    print("\n--- Generating demo output ---")
    generate_demo()


if __name__ == "__main__":
    main()
