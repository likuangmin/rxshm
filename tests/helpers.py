"""What the tests share: the byte pattern of their streams, segment names, and the C programs"""

import array
import atexit
import os
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "python"))

# pylint: disable-next=wrong-import-position
from rxshm import ShmError, ShmReader  # noqa: E402

TAG = "PROG = test\nTIME = 2026/10/27 17:00:00.000\nFMT  = CS16\nF_S  = 4\nF_LO = 1575.42\n" \
      "IQ   = 2\nBITS = 16\n"
# Where the C programs are: built by CMake (RXSHM_BIN, as ctest sets it),
# else by the tests with cc and c++
SOURCES = {
    "sdr_shm_cat": "c/tools/sdr_shm_cat.c",
    "sdr_shm_put": "c/tools/sdr_shm_put.c",
    "writer_test": "tests/c/writer_test.c",
    "layout": "tests/c/layout.c",
    "cxx_test": "tests/c/cxx.cpp",
}
built = {}


def pattern(offset: int, size: int) -> bytes:
    """Stream bytes from offset: each 8-byte word is its own stream offset (little-endian)"""
    return array.array("Q", range(offset // 8, (offset + size) // 8)).tobytes()


def check(test, chunk):
    """A chunk holds the stream's bytes at its offset"""
    test.assertEqual(chunk.data, pattern(chunk.offset, len(chunk.data)))


def unique(what: str) -> str:
    """A segment name of this test process's"""
    return f"rxshm-test-{what}-{os.getpid()}"


def wait_open(name: str, **options) -> ShmReader:
    """A reader of a segment its writer is creating"""
    deadline = time.monotonic() + 5
    while True:
        try:
            return ShmReader(name, **options)
        except ShmError:
            if time.monotonic() > deadline:
                raise
            time.sleep(0.01)


def has_compilers() -> bool:
    """Whether the C programs are there or can be built"""
    return bool(os.environ.get("RXSHM_BIN")) or bool(shutil.which("cc") and shutil.which("c++"))


def program(name: str) -> str:
    """A C program's executable, built the first time it is asked for"""
    if os.environ.get("RXSHM_BIN"):
        return os.path.join(os.environ["RXSHM_BIN"], name)
    if name not in built:
        if "folder" not in built:
            built["folder"] = tempfile.mkdtemp(prefix="rxshm-tests-")
            atexit.register(shutil.rmtree, built["folder"], True)
        source = SOURCES[name]
        exe = os.path.join(built["folder"], name)
        if source.endswith(".cpp"):
            flags = ["c++", "-std=c++20", "-Wold-style-cast", "-Wshadow", "-isystem"]
        else:
            flags = ["cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-I"]
        subprocess.run(flags[:-1] + ["-O2", "-Wall", "-Wextra", "-Werror", flags[-1],
                                     os.path.join(ROOT, "c", "include"), "-o", exe,
                                     os.path.join(ROOT, source)], check=True)
        built[name] = exe
    return built[name]
