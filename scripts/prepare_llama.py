"""Build the pinned upstream llama.cpp on the Mac development machine."""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / ".work"
REV = "8d60873dfb8619f2b5ed31adfbab07cccb40db5c"


def run(*args, cwd=ROOT):
    subprocess.run(args, cwd=cwd, check=True)


def prepare():
    source = WORK / ("llama-" + REV)
    build = WORK / ("llama-build-" + REV)
    source.mkdir(parents=True, exist_ok=True)
    if not (source / ".git").is_dir():
        run("git", "init", "-q", cwd=source)
    head = subprocess.run(["git", "rev-parse", "--verify", "HEAD"], cwd=source, capture_output=True, text=True)
    if head.returncode:
        run("git", "fetch", "--depth=1", "https://github.com/ggml-org/llama.cpp.git", REV, cwd=source)
        run("git", "checkout", "--detach", REV, cwd=source)
    elif head.stdout.strip() != REV:
        raise RuntimeError(f"Unexpected upstream revision in {source}")
    run("git", "diff", "--quiet", "HEAD", "--", cwd=source)
    flags = ["-D" + key + "=OFF" for key in (
        "BUILD_SHARED_LIBS", "GGML_BACKEND_DL", "GGML_NATIVE", "GGML_OPENMP", "GGML_BLAS",
        "GGML_CCACHE", "LLAMA_BUILD_COMMON", "LLAMA_BUILD_TESTS", "LLAMA_BUILD_TOOLS",
        "LLAMA_BUILD_EXAMPLES", "LLAMA_BUILD_SERVER", "LLAMA_BUILD_APP", "LLAMA_BUILD_MTMD")]
    run("cmake", "-S", source, "-B", build, "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_INSTALL_PREFIX=" + str(WORK / "llama-install"), "-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0",
        "-DGGML_METAL=ON", "-DGGML_METAL_EMBED_LIBRARY=ON", "-DGGML_ACCELERATE=ON", *flags)
    run("cmake", "--build", build, "--parallel", str(min(os.cpu_count() or 2, 8)))
    run("cmake", "--install", build)


if __name__ == "__main__":
    prepare()
