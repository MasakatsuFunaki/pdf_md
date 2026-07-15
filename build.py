
import os
import platform
import shutil
import subprocess
import argparse

PROJECT_ROOT = os.path.dirname(os.path.realpath(__file__))

IS_WINDOWS = platform.system() == "Windows"
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
    demo_last_run_release_<git-hash> folder at the repo root."""
    exe = os.path.join(BUILD_DIR, "Release", "pdf2md.exe" if IS_WINDOWS else "pdf2md")
    if not os.path.isfile(exe):
        print(f"pdf2md binary not found at {exe}; skipping demo generation")
        return

    git_hash = subprocess.run(
        ["git", "rev-parse", "--short", "HEAD"],
        check=True, capture_output=True, text=True, cwd=PROJECT_ROOT,
    ).stdout.strip()
    demo_dir = os.path.join(PROJECT_ROOT, f"demo_last_run_release_{git_hash}")
    rm(demo_dir)
    os.makedirs(demo_dir)

    pdf_dirs = [
        os.path.join(PROJECT_ROOT, "tests", "corpus", "cache"),
        os.path.join(BUILD_DIR, "test_output"),
        os.path.join(BUILD_DIR, "test_output", "fixtures"),
    ]
    pdfs = [os.path.join(d, f)
            for d in pdf_dirs if os.path.isdir(d)
            for f in sorted(os.listdir(d)) if f.lower().endswith(".pdf")]
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
