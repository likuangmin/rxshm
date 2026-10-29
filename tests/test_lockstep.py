"""Lockstep: a writer waiting for registered readers"""

import multiprocessing
import os
import signal
import subprocess
import threading
import time
import unittest

from tests.helpers import TAG, has_compilers, pattern, program, unique
from rxshm import PAGE, SLOTS, ShmError, ShmReader, ShmTimeout, ShmWriter

RING = 16 * PAGE


def register_and_hang(name: str, ready):
    """Child process: register, then never read"""
    reader = ShmReader(name, oldest=True, register=True)
    ready.set()
    time.sleep(60)
    reader.close()


@unittest.skipUnless(os.path.isdir("/dev/shm"), "needs /dev/shm")
class TestLockstep(unittest.TestCase):
    """Registered readers lose nothing; dead or hung ones are evicted"""

    def setUp(self):
        self.name = unique("lock")

    def writer(self, **options) -> ShmWriter:
        """A lockstep writer, closed at the end"""
        writer = ShmWriter(self.name, ring_bytes=RING, sample_bytes=8, tag_text=TAG,
                           lockstep=True, **options)
        self.addCleanup(lambda: os.path.exists(writer.path) and writer.close())
        return writer

    def test_slow_reader(self):
        """A reader slower than the writer: the writer waits, nothing skipped"""
        writer = self.writer()
        reader = ShmReader(self.name, oldest=True, register=True)
        self.assertTrue(reader.registered)
        total = 40 * RING
        got, skipped = [], []

        def read():
            while not reader.ended:
                chunk = reader.wait(1.0, max_bytes=PAGE)
                got.append(chunk.data)
                skipped.append(chunk.skipped)
                time.sleep(0.0005)

        thread = threading.Thread(target=read)
        thread.start()
        for offset in range(0, total, 4 * PAGE):
            writer.write(pattern(offset, 4 * PAGE))
        writer.close()
        thread.join()
        reader.close()
        self.assertEqual(sum(skipped), 0)
        self.assertEqual(b"".join(got), pattern(0, total))

    def test_unregistered_reader(self):
        """A reader that doesn't register isn't waited for: it loses bytes, as with v1"""
        writer = self.writer()
        reader = ShmReader(self.name, oldest=True)
        self.assertTrue(reader.lockstep)
        writer.write(pattern(0, 3 * RING))
        chunk = reader.read()
        self.assertEqual(chunk.skipped, 2 * RING)
        reader.close()

    def test_dead_reader(self):
        """A registered reader killed: evicted at once (its lock went), the writer goes on"""
        writer = self.writer(reader_timeout=60)
        ready = multiprocessing.get_context("fork").Event()
        child = multiprocessing.get_context("fork").Process(
            target=register_and_hang, args=(self.name, ready))
        child.start()
        self.assertTrue(ready.wait(5))
        writer.write(pattern(0, RING))
        blocked = threading.Thread(target=writer.write, args=(pattern(RING, RING),))
        blocked.start()
        blocked.join(0.3)
        self.assertTrue(blocked.is_alive(), "the writer didn't wait for the reader")
        os.kill(child.pid, signal.SIGKILL)
        child.join()
        blocked.join(5)
        self.assertFalse(blocked.is_alive())
        self.assertEqual(writer.evicted, 1)

    def test_hung_reader(self):
        """A registered reader that stops reading: evicted after the timeout, then lossy"""
        writer = self.writer(reader_timeout=0.3)
        reader = ShmReader(self.name, oldest=True, register=True)
        began = time.monotonic()
        writer.write(pattern(0, 3 * RING))
        self.assertGreater(time.monotonic() - began, 0.25)
        self.assertEqual(writer.evicted, 1)
        chunk = reader.read()
        self.assertTrue(reader.evicted)
        self.assertFalse(reader.registered)
        self.assertEqual(chunk.skipped, 2 * RING)
        reader.close()

    def test_timeout(self):
        """A write a reader has no room for times out, writing nothing"""
        writer = self.writer(reader_timeout=60)
        reader = ShmReader(self.name, oldest=True, register=True)
        writer.write(pattern(0, RING), timeout=0)
        began = time.monotonic()
        with self.assertRaises(ShmTimeout):
            writer.write(pattern(RING, 8), timeout=0.05)
        self.assertGreaterEqual(time.monotonic() - began, 0.05)
        self.assertEqual((writer.written, writer.write_count, writer.evicted), (RING, RING, 0))
        self.assertEqual(len(reader.read(max_bytes=8).data), 8)
        writer.write(pattern(RING, 8), timeout=0)
        reader.close()

    def test_wait_for_readers(self):
        """A replay waits for its readers before its first byte"""
        writer = self.writer()
        self.assertEqual(writer.wait_for_readers(1, 0.05), 0)
        threading.Timer(0.1, lambda: readers.append(
            ShmReader(self.name, register=True))).start()
        readers = []
        self.assertEqual(writer.wait_for_readers(1, 5), 1)
        time.sleep(0.05)
        readers[0].close()
        self.assertEqual(writer.wait_for_readers(1, 0.05), 0)

    def test_registration(self):
        """Not a lockstep writer, or all slots taken: refused; a closed reader's slot is free"""
        plain = ShmWriter(unique("plain"), ring_bytes=RING, sample_bytes=8,
                          tag_text=TAG)
        with self.assertRaisesRegex(ShmError, "isn't in lockstep"):
            ShmReader(os.path.basename(plain.path), register=True)
        plain.close()
        self.writer()
        readers = [ShmReader(self.name, register=True) for _ in range(SLOTS)]
        with self.assertRaisesRegex(ShmError, "slots taken"):
            ShmReader(self.name, register=True)
        readers.pop().close()
        readers.append(ShmReader(self.name, register=True))
        for reader in readers:
            reader.close()

    @unittest.skipUnless(has_compilers(), "needs a C compiler")
    def test_c_reader(self):
        """sdr_shm_cat --lockstep, slowed by its pipe: every byte, none lost"""
        exe = program("sdr_shm_cat")
        writer = self.writer()
        total = 16 * 2**20
        with subprocess.Popen([exe, self.name, "--oldest", "--lockstep"],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE) as cat:
            self.assertEqual(writer.wait_for_readers(1, 5), 1)
            out = []

            def drain():
                chunks = []
                while data := cat.stdout.read(PAGE):
                    chunks.append(data)
                    time.sleep(0.0002)
                out.append(b"".join(chunks))

            thread = threading.Thread(target=drain)
            thread.start()
            for offset in range(0, total, 2**16):
                writer.write(pattern(offset, 2**16))
            writer.close()
            cat.wait(30)
            thread.join()
            self.assertEqual(cat.stderr.read(), b"sdr_shm_cat: from sample 0\n")
        self.assertEqual(out[0], pattern(0, total))


if __name__ == "__main__":
    unittest.main()
