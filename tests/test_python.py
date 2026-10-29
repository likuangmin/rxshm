"""The Python writer and readers"""

import multiprocessing
import os
import unittest

from tests.helpers import TAG, check, pattern, unique, wait_open
from rxshm import (ENDED, FAILED, PAGE, ShmError, ShmReader, ShmWriter, ring_size,
                   segment_path)

RING = 4 * PAGE


def write_fast(name: str, total: int, block: int):
    """Writer process: total bytes of pattern as fast as it can"""
    writer = ShmWriter(name, ring_bytes=64 * PAGE, sample_bytes=8, tag_text=TAG)
    for offset in range(0, total, block):
        writer.write(pattern(offset, block))
    writer.close()


@unittest.skipUnless(os.path.isdir("/dev/shm"), "needs /dev/shm")
class TestShm(unittest.TestCase):
    """Writer and readers in one process"""

    def setUp(self):
        self.name = unique("py")
        self.writer = ShmWriter(self.name, ring_bytes=RING, sample_bytes=8, tag_text=TAG,
                                data_path="/data/run.bin", start_ns=1_800_000_000 * 10**9)
        self.addCleanup(self.cleanup)

    def cleanup(self):
        """End the writer unless the test did"""
        if os.path.exists(segment_path(self.name)):
            self.writer.close()

    def test_header(self):
        """The fields a reader needs: tag, ring, sample size, file, start"""
        reader = ShmReader(self.name)
        self.assertEqual((reader.ring_bytes, reader.sample_bytes), (RING, 8))
        self.assertEqual(reader.tag_text, TAG)
        self.assertEqual(reader.data_path, "/data/run.bin")
        self.assertEqual(reader.start_ns, 1_800_000_000 * 10**9)
        self.assertEqual(reader.writer_pid, os.getpid())
        self.assertFalse(reader.lockstep)
        reader.close()

    def test_round_trip(self):
        """Read as written, across the ring's end, from the oldest bytes"""
        reader = ShmReader(self.name, oldest=True)
        offset = 0
        for size in (1000 * 8, 1500 * 8, 900 * 8, 2048 * 8):
            self.writer.write(pattern(offset, size))
            offset += size
            chunk = reader.read()
            self.assertEqual((chunk.skipped, chunk.offset + len(chunk.data)), (0, offset))
            check(self, chunk)
        reader.close()

    def test_newest(self):
        """A reader starting late starts at the newest byte: its sample index"""
        self.writer.write(pattern(0, 800))
        reader = ShmReader(self.name)
        self.assertEqual(reader.read().data, b"")
        self.writer.write(pattern(800, 80))
        chunk = reader.read()
        self.assertEqual((chunk.offset, len(chunk.data)), (800, 80))
        check(self, chunk)
        reader.close()

    def test_fell_behind(self):
        """Overwritten bytes are skipped and counted; the rest is right"""
        reader = ShmReader(self.name, oldest=True)
        self.writer.write(pattern(0, RING + 4000))
        chunk = reader.read()
        self.assertEqual((chunk.offset, chunk.skipped, len(chunk.data)), (4000, 4000, RING))
        check(self, chunk)
        reader.close()

    def test_being_overwritten(self):
        """
        Bytes the writer is overwriting (up to write_end) are dropped, even
        before write_count says so
        """
        reader = ShmReader(self.name, oldest=True)
        self.writer.write(pattern(0, RING))
        # The writer midway through its next write of a page: write_end
        # stored, the first page's bytes partly overwritten
        self.writer.end.value = RING + PAGE
        self.writer.ring[:16] = bytes(16)
        chunk = reader.read()
        self.assertEqual((chunk.offset, chunk.skipped), (PAGE, PAGE))
        check(self, chunk)
        reader.close()

    def test_bigger_than_ring(self):
        """A write bigger than the ring keeps its end"""
        reader = ShmReader(self.name, oldest=True)
        self.writer.write(pattern(0, 3 * RING))
        chunk = reader.read()
        self.assertEqual((chunk.offset, chunk.skipped), (2 * RING, 2 * RING))
        check(self, chunk)
        reader.close()

    def test_max_bytes(self):
        """A read takes at most max_bytes, whole samples"""
        reader = ShmReader(self.name, oldest=True)
        self.writer.write(pattern(0, 800))
        chunk = reader.read(max_bytes=100)
        self.assertEqual(len(chunk.data), 96)
        self.assertEqual(reader.read().offset, 96)
        reader.close()

    def test_whole_samples(self):
        """Writes of part of a sample are refused"""
        with self.assertRaises(ShmError):
            self.writer.write(b"\0" * 12)

    def test_ended(self):
        """A reader drains what was written, then sees the end; the name is gone"""
        reader = ShmReader(self.name, oldest=True)
        self.writer.write(pattern(0, 160))
        self.writer.close()
        self.assertFalse(os.path.exists(segment_path(self.name)))
        self.assertFalse(reader.ended)
        check(self, reader.wait(1.0))
        self.assertTrue(reader.ended)
        self.assertEqual(reader.wait(1.0).data, b"")
        reader.close()

    def test_failed(self):
        """A capture that failed: state FAILED"""
        reader = ShmReader(self.name)
        self.writer.close(FAILED)
        self.assertEqual(reader.state, FAILED)
        self.assertTrue(reader.ended)
        reader.close()

    def test_stale(self):
        """A segment of a writer still running isn't taken; one left behind is"""
        with self.assertRaisesRegex(ShmError, "being written"):
            ShmWriter(self.name, ring_bytes=RING, sample_bytes=8, tag_text=TAG)
        self.writer.close(ENDED)
        # Left behind: the file exists, its writer ended
        with open(segment_path(self.name), "wb") as f:
            f.write(b"stale")
        self.writer = ShmWriter(self.name, ring_bytes=RING, sample_bytes=8, tag_text=TAG)
        reader = ShmReader(self.name)
        self.assertEqual(reader.ring_bytes, RING)
        reader.close()

    def test_replace(self):
        """
        replace takes a running writer's name; the replaced writer closing
        leaves the new segment be
        """
        old = self.writer
        self.writer = ShmWriter(self.name, ring_bytes=2 * RING, sample_bytes=8, tag_text=TAG,
                                replace=True)
        old.close()
        reader = ShmReader(self.name)
        self.assertEqual(reader.ring_bytes, 2 * RING)
        reader.close()


class TestSizes(unittest.TestCase):
    """Ring sizes"""

    def test_ring_size(self):
        """Whole pages and samples, about the time asked for"""
        self.assertEqual(ring_size(2.0, 4.0, 4), 32_002_048)
        self.assertEqual(ring_size(2.0, 61.44, 4) % PAGE, 0)
        self.assertEqual(ring_size(0.0, 4.0, 4), PAGE)
        self.assertEqual(ring_size(1.0, 1.0, 12) % 12, 0)

    def test_bad_ring(self):
        """A ring of part of a page or sample is refused"""
        with self.assertRaises(ShmError):
            ShmWriter(unique("bad"), ring_bytes=4000, sample_bytes=8, tag_text="")
        with self.assertRaises(ShmError):
            ShmWriter(unique("bad"), ring_bytes=PAGE, sample_bytes=3, tag_text="")


@unittest.skipUnless(os.path.isdir("/dev/shm"), "needs /dev/shm")
class TestConcurrent(unittest.TestCase):
    """A writer process at full speed: readers never see torn or misplaced bytes"""

    def test_python_reader(self):
        """Every byte read is the stream's byte at its offset"""
        name = unique("conc")
        total, block = 400 * 2**20, 64 * 2**10
        proc = multiprocessing.get_context("fork").Process(
            target=write_fast, args=(name, total, block))
        proc.start()
        reader = wait_open(name, oldest=True)
        start, end, skipped = reader.pos, None, 0
        while not reader.ended:
            chunk = reader.wait(1.0, max_bytes=2**20)
            skipped += chunk.skipped
            check(self, chunk)
            end = chunk.offset + len(chunk.data)
        proc.join()
        reader.close()
        self.assertEqual(end, total)
        self.assertGreater(total - start - skipped, 0)


if __name__ == "__main__":
    unittest.main()
