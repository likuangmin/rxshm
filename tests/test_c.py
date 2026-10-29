"""The C reader and writer, and both with Python's"""

import os
import subprocess
import threading
import time
import unittest

from tests.helpers import TAG, check, has_compilers, pattern, program, unique, wait_open
import rxshm
from rxshm import PAGE, ShmWriter, segment_path

# Python's constants for the fields layout.c prints
LAYOUT = {
    "ring_bytes": rxshm.RING_BYTES, "sample_bytes": rxshm.SAMPLE_BYTES, "state": rxshm.STATE,
    "write_count": rxshm.WRITE_COUNT, "heartbeat_ns": rxshm.HEARTBEAT_NS,
    "start_ns": rxshm.START_NS, "lost_samples": rxshm.LOST_SAMPLES,
    "session": rxshm.SESSION, "writer_pid": rxshm.WRITER_PID, "flags": rxshm.FLAGS,
    "slot_offset": rxshm.SLOT_OFFSET, "slot_count": rxshm.SLOT_COUNT,
    "slot_bytes": rxshm.SLOT_BYTES, "lockstep_timeout_ns": rxshm.LOCKSTEP_TIMEOUT_NS,
    "write_end": rxshm.WRITE_END, "tag": rxshm.TAG, "data_path": rxshm.DATA_PATH,
    "pid": rxshm.SLOT_PID, "pos": rxshm.SLOT_POS, "slot_size": rxshm.SLOT_SIZE,
}


def run(*args, **options) -> subprocess.CompletedProcess:
    """A program run to its end, its output captured"""
    return subprocess.run(args, capture_output=True, check=False, timeout=60, **options)


def drain(stream, out: list):
    """Thread: a stream read to its end into out"""
    threading.Thread(target=lambda: out.append(stream.read()), daemon=True).start()


@unittest.skipUnless(has_compilers(), "needs a C compiler")
@unittest.skipUnless(os.path.isdir("/dev/shm"), "needs /dev/shm")
class TestC(unittest.TestCase):
    """C programs against each other and Python"""

    def setUp(self):
        self.name = unique("c")
        self.addCleanup(lambda: os.path.exists(segment_path(self.name)) and
                        os.unlink(segment_path(self.name)))

    def test_layout(self):
        """The C structs' offsets are Python's"""
        lines = run(program("layout"), text=True).stdout.split()
        offsets = dict(zip(lines[::2], map(int, lines[1::2])))
        # The slot's state and heartbeat_ns share names with the header's
        self.assertEqual(offsets.pop("heartbeat_ns"), rxshm.SLOT_HEARTBEAT)
        self.assertEqual(offsets.pop("state"), rxshm.SLOT_STATE)
        self.assertEqual(offsets.pop("session"), rxshm.SLOT_SESSION)
        expected = {k: v for k, v in LAYOUT.items() if k in offsets}
        self.assertEqual(offsets, expected)

    def test_writer_api(self):
        """tests/c/writer_test.c passes"""
        result = run(program("writer_test"), self.name, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_cxx(self):
        """The header in C++20 with -Wold-style-cast -Wshadow: builds and works"""
        result = run(program("cxx_test"), self.name, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_c_reader(self):
        """sdr_shm_cat reads a Python writer's stream from its start to its end"""
        # 16 MB: a quarter of the stream, slack for a busy computer
        writer = ShmWriter(self.name, ring_bytes=4096 * PAGE, sample_bytes=8, tag_text=TAG)
        total = 64 * 2**20
        with subprocess.Popen([program("sdr_shm_cat"), self.name, "--oldest"],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE) as cat:
            out = []
            drain(cat.stdout, out)
            time.sleep(0.2)
            for offset in range(0, total, 2**16):
                writer.write(pattern(offset, 2**16))
                time.sleep(0.001)
            writer.close()
            cat.wait(10)
            self.assertEqual(cat.stderr.read(), b"sdr_shm_cat: from sample 0\n")
        self.assertEqual(out[0], pattern(0, total))

    def test_c_reader_being_overwritten(self):
        """sdr_shm_cat drops the bytes being overwritten (write_end)"""
        writer = ShmWriter(self.name, ring_bytes=4 * PAGE, sample_bytes=8, tag_text=TAG)
        writer.write(pattern(0, 4 * PAGE))
        writer.end.value = 5 * PAGE
        writer.ring[:16] = bytes(16)
        with subprocess.Popen([program("sdr_shm_cat"), self.name, "--oldest"],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE) as cat:
            out = []
            drain(cat.stdout, out)
            self.assertEqual(cat.stderr.readline(), b"sdr_shm_cat: 4096 bytes lost at 0\n")
            writer.close()
            cat.wait(10)
        self.assertEqual(out[0], pattern(PAGE, 3 * PAGE))

    def put(self, *options) -> subprocess.Popen:
        """sdr_shm_put of 8-byte samples into the test's segment, its stdin a pipe"""
        # pylint: disable-next=consider-using-with
        return subprocess.Popen([program("sdr_shm_put"), self.name, "--sample-bytes", "8",
                                 "--ring", str(4 * PAGE), *options],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE)

    def test_c_writer_lockstep(self):
        """sdr_shm_put --lockstep to a registered Python reader: every byte, none lost"""
        total = 4 * 2**20
        with self.put("--lockstep", "--readers", "1") as put:
            reader = wait_open(self.name, oldest=True, register=True)
            feed = threading.Thread(target=lambda: (put.stdin.write(pattern(0, total)),
                                                    put.stdin.close()))
            feed.start()
            chunks, skipped = [], 0
            while not reader.ended:
                chunk = reader.wait(1.0, max_bytes=PAGE)
                chunks.append(chunk.data)
                skipped += chunk.skipped
            feed.join()
            put.wait(10)
            reader.close()
            self.assertIn(b"READERS 1 of 1\nDONE bytes=4194304 evicted=0\n", put.stderr.read())
        self.assertEqual(skipped, 0)
        self.assertEqual(b"".join(chunks), pattern(0, total))

    def test_c_writer_lossy(self):
        """sdr_shm_put without lockstep: what a Python reader keeps is right"""
        total = 16 * 2**20
        with self.put() as put:
            reader = wait_open(self.name, oldest=True)
            self.assertEqual(reader.sample_bytes, 8)
            feed = threading.Thread(target=lambda: (put.stdin.write(pattern(0, total)),
                                                    put.stdin.close()))
            feed.start()
            end = 0
            while not reader.ended:
                chunk = reader.wait(1.0)
                check(self, chunk)
                end = max(end, chunk.offset + len(chunk.data))
            feed.join()
            put.wait(10)
            reader.close()
        self.assertEqual(end, total)

    def test_c_to_c(self):
        """sdr_shm_put --lockstep into sdr_shm_cat --lockstep: byte for byte"""
        total = 8 * 2**20
        with self.put("--lockstep", "--readers", "1") as put:
            while not os.path.exists(segment_path(self.name)):
                time.sleep(0.01)
            with subprocess.Popen([program("sdr_shm_cat"), self.name, "--oldest", "--lockstep"],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE) as cat:
                out = []
                drain(cat.stdout, out)
                put.stdin.write(pattern(0, total))
                put.stdin.close()
                put.wait(30)
                cat.wait(30)
                self.assertEqual(cat.stderr.read(), b"sdr_shm_cat: from sample 0\n")
        self.assertEqual(out[0], pattern(0, total))

    def test_names(self):
        """
        The C writer refuses a running Python writer's name, takes it with
        --replace, and the Python writer closing leaves its segment be
        """
        writer = ShmWriter(self.name, ring_bytes=PAGE, sample_bytes=8, tag_text=TAG)
        result = run(program("sdr_shm_put"), self.name, "--sample-bytes", "8", input=b"")
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"File exists", result.stderr)
        with self.put("--replace") as put:
            reader = wait_open(self.name)
            while reader.ring_bytes != 4 * PAGE:
                reader.close()
                reader = wait_open(self.name)
            writer.close()
            self.assertTrue(os.path.exists(segment_path(self.name)))
            put.stdin.write(pattern(0, 800))
            put.stdin.close()
            put.wait(10)
            check(self, reader.wait(1.0))
            reader.close()
        self.assertFalse(os.path.exists(segment_path(self.name)))


if __name__ == "__main__":
    unittest.main()
