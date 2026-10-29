"""
rxshm: SDR samples in shared memory. A writer (a capture, or a replay of a
recording) publishes a stream of IF samples in a ring in shared memory,
which readers in other processes read live. Layout and protocol: SPEC.md;
c/include/rxshm/sdr_shm.h is the same in C. Standard library only; Linux.
"""

from dataclasses import dataclass
import ctypes
import fcntl
import math
import mmap
import os
import struct
import time

__version__ = "1.0.0"

SHM_DIR = "/dev/shm"
MAGIC = b"RXBSHM1\0"
VERSION = 1
HEADER_BYTES = 4096
PAGE = 4096
# Field offsets, see SPEC.md
RING_BYTES, SAMPLE_BYTES, STATE = 16, 24, 28
WRITE_COUNT, HEARTBEAT_NS, START_NS, LOST_SAMPLES = 32, 40, 48, 56
SESSION, WRITER_PID, FLAGS = 64, 72, 76
SLOT_OFFSET, SLOT_COUNT, SLOT_BYTES, LOCKSTEP_TIMEOUT_NS = 80, 84, 88, 96
WRITE_END = 104
TAG, TAG_SIZE = 128, 1024
DATA_PATH, DATA_PATH_SIZE = 1152, 512
# state
WRITING, ENDED, FAILED = 1, 2, 3
# flags: the writer waits for registered readers (lockstep)
FLAG_LOCKSTEP = 1
# Lockstep readers' slots: where, how many, their size; and their fields
SLOTS_AT, SLOTS, SLOT_SIZE = 2048, 32, 64
SLOT_STATE, SLOT_PID, SLOT_POS, SLOT_HEARTBEAT, SLOT_SESSION = 0, 4, 8, 16, 24
# A slot's state
SLOT_FREE, SLOT_ACTIVE, SLOT_EVICTED = 0, 2, 3
# Seconds a writer waits for a registered reader's heartbeat by default, and
# between looks while it waits
READER_TIMEOUT = 2.0
LOCKSTEP_POLL = 0.0002
# Linux open file description locks (fcntl(2)): a slot is its reader's
# while it holds one on the slot's bytes; released when it dies
F_OFD_GETLK = getattr(fcntl, "F_OFD_GETLK", 36)
F_OFD_SETLK = getattr(fcntl, "F_OFD_SETLK", 37)


class ShmError(OSError):
    """A segment that can't be created, opened or read"""


class ShmTimeout(ShmError, TimeoutError):
    """A lockstep writer's wait for its readers timed out"""


def segment_path(name: str) -> str:
    """Where a segment is"""
    return os.path.join(SHM_DIR, name)


def ring_size(seconds: float, rate_msps: float, sample_bytes: int) -> int:
    """Ring bytes holding about seconds of samples: whole pages and samples"""
    unit = math.lcm(PAGE, sample_bytes)
    wanted = seconds * rate_msps * 1e6 * sample_bytes
    return max(unit, -(-int(wanted) // unit) * unit)


def slot_lock(fd: int, index: int, kind: int = fcntl.F_WRLCK, cmd: int = F_OFD_SETLK) -> int:
    """
    fcntl(2) lock of slot index's bytes (kind F_WRLCK or F_UNLCK) with an
    OFD command: F_OFD_SETLK raises BlockingIOError if another holds it;
    F_OFD_GETLK returns the type of a lock in the way, F_UNLCK if none
    """
    # struct flock on Linux x86-64 and AArch64: l_type, l_whence, l_start,
    # l_len, l_pid (0 for OFD locks)
    flock = struct.pack("=hh4xqqi4x", kind, os.SEEK_SET, SLOTS_AT + index * SLOT_SIZE,
                        SLOT_SIZE, 0)
    return struct.unpack_from("=h", fcntl.fcntl(fd, cmd, flock))[0]


class Segment:
    """A mapped segment: its header fields"""

    def __init__(self, fd: int, size: int, writable: bool, keep_fd: bool = False):
        access = mmap.ACCESS_WRITE if writable else mmap.ACCESS_READ
        self.map = mmap.mmap(fd, size, access=access)
        # Kept for the lockstep slots' locks
        self.fd = fd if keep_fd else None
        if not keep_fd:
            os.close(fd)
        self.ring_bytes = struct.unpack_from("<Q", self.map, RING_BYTES)[0]
        self.sample_bytes = struct.unpack_from("<I", self.map, SAMPLE_BYTES)[0]
        self.ring = memoryview(self.map)[HEADER_BYTES : HEADER_BYTES + self.ring_bytes]

    def u64(self, offset: int) -> int:
        """
        A 64-bit field: loaded until two loads agree, as Python can't make
        one aligned load of a read-only map (a torn load can't repeat itself)
        """
        value = struct.unpack_from("<Q", self.map, offset)[0]
        while True:
            again = struct.unpack_from("<Q", self.map, offset)[0]
            if again == value:
                return value
            value = again

    @property
    def write_count(self) -> int:
        """Bytes written since the start"""
        return self.u64(WRITE_COUNT)

    @property
    def write_end(self) -> int:
        """Where the writer may be writing up to, at least write_count"""
        return max(self.u64(WRITE_END), self.write_count)

    @property
    def state(self) -> int:
        """WRITING, ENDED or FAILED"""
        return struct.unpack_from("<I", self.map, STATE)[0]

    @property
    def heartbeat_ns(self) -> int:
        """Unix time (ns) of the last write"""
        return self.u64(HEARTBEAT_NS)

    @property
    def start_ns(self) -> int:
        """Unix time (ns) of the first sample, 0 if unknown"""
        return struct.unpack_from("<q", self.map, START_NS)[0]

    @property
    def lost_samples(self) -> int:
        """Samples the SDR lost, written as zeros"""
        return self.u64(LOST_SAMPLES)

    @property
    def session(self) -> int:
        """Random, new for each capture"""
        return self.u64(SESSION)

    @property
    def writer_pid(self) -> int:
        """Process ID of the capture"""
        return struct.unpack_from("<I", self.map, WRITER_PID)[0]

    @property
    def flags(self) -> int:
        """FLAG_LOCKSTEP if the writer waits for registered readers"""
        return struct.unpack_from("<I", self.map, FLAGS)[0]

    @property
    def lockstep(self) -> bool:
        """Whether the writer waits for registered readers"""
        return bool(self.flags & FLAG_LOCKSTEP)

    @property
    def tag_text(self) -> str:
        """The samples' tag text (a Pocket SDR tag file's)"""
        return bytes(self.map[TAG : TAG + TAG_SIZE]).split(b"\0")[0].decode("ascii", "replace")

    @property
    def data_path(self) -> str:
        """The IF data file it mirrors, "" if none"""
        raw = bytes(self.map[DATA_PATH : DATA_PATH + DATA_PATH_SIZE]).split(b"\0")[0]
        return raw.decode("utf-8", "replace")

    def close(self):
        """Unmap it"""
        self.ring.release()
        self.map.close()
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None


class Slot:  # pylint: disable=too-few-public-methods
    """A lockstep reader's slot in a writable map of the header"""

    def __init__(self, header: mmap.mmap, index: int):
        base = SLOTS_AT + index * SLOT_SIZE
        self.index = index
        # Single aligned loads and stores (ctypes)
        self.state = ctypes.c_uint32.from_buffer(header, base + SLOT_STATE)
        self.pid = ctypes.c_uint32.from_buffer(header, base + SLOT_PID)
        self.pos = ctypes.c_uint64.from_buffer(header, base + SLOT_POS)
        self.heartbeat = ctypes.c_uint64.from_buffer(header, base + SLOT_HEARTBEAT)
        self.session = ctypes.c_uint64.from_buffer(header, base + SLOT_SESSION)


class ShmWriter(Segment):  # pylint: disable=too-many-instance-attributes
    """
    The capture's side: creates the segment, appends the stream's bytes to
    its ring, and removes the name at the end. It never waits for readers,
    unless in lockstep
    """

    # pylint: disable-next=too-many-arguments,too-many-locals
    def __init__(self, name: str, *, ring_bytes: int, sample_bytes: int, tag_text: str,
                 data_path: str = "", start_ns: int = 0, lockstep: bool = False,
                 reader_timeout: float = READER_TIMEOUT, replace: bool = False):
        """
        :param tag_text: The samples' tag (a Pocket SDR tag file's text)
        :param lockstep: Wait for registered readers rather than overwrite
            what they haven't read (replays; never a live SDR's capture)
        :param reader_timeout: lockstep: seconds without a registered
            reader's heartbeat, while waiting for it, before it is evicted
        :param replace: Take the name even from a running writer; by
            default only a stale segment is replaced
        """
        if ring_bytes % sample_bytes or ring_bytes % PAGE:
            raise ShmError(f"ring of {ring_bytes} bytes: not whole pages and samples")
        tag = tag_text.encode("ascii")
        path = data_path.encode("utf-8")
        if len(tag) >= TAG_SIZE or len(path) >= DATA_PATH_SIZE:
            raise ShmError("tag or data path too long for the header")
        self.path = segment_path(name)
        if replace:
            try:
                os.unlink(self.path)
            except FileNotFoundError:
                pass
        else:
            self.remove_stale(self.path)
        try:
            fd = os.open(self.path, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o644)
            os.ftruncate(fd, HEADER_BYTES + ring_bytes)
        except OSError as e:
            raise ShmError(f"can't create the shared memory {self.path}: {e}") from e
        stat = os.fstat(fd)
        # Close removes the name only if it is still this file
        self.file_id = (stat.st_dev, stat.st_ino)
        header = bytearray(HEADER_BYTES)
        struct.pack_into("<8sIIQII", header, 0, MAGIC, VERSION, HEADER_BYTES, ring_bytes,
                         sample_bytes, WRITING)
        self.session_id = int.from_bytes(os.urandom(8), "little")
        struct.pack_into("<QQqQQII", header, WRITE_COUNT, 0, time.time_ns(), start_ns, 0,
                         self.session_id, os.getpid(), FLAG_LOCKSTEP if lockstep else 0)
        if lockstep:
            struct.pack_into("<IIIIQ", header, SLOT_OFFSET, SLOTS_AT, SLOTS, SLOT_SIZE, 0,
                             round(reader_timeout * 1e9))
        header[TAG : TAG + len(tag)] = tag
        header[DATA_PATH : DATA_PATH + len(path)] = path
        os.pwrite(fd, header, 0)
        super().__init__(fd, HEADER_BYTES + ring_bytes, writable=True, keep_fd=lockstep)
        self.count = ctypes.c_uint64.from_buffer(self.map, WRITE_COUNT)
        self.end = ctypes.c_uint64.from_buffer(self.map, WRITE_END)
        self.heartbeat = ctypes.c_uint64.from_buffer(self.map, HEARTBEAT_NS)
        self.lost = ctypes.c_uint64.from_buffer(self.map, LOST_SAMPLES)
        self.written = 0
        self.slots = [Slot(self.map, i) for i in range(SLOTS)] if lockstep else []
        self.timeout_ns = round(reader_timeout * 1e9)
        # Readers evicted so far
        self.evicted = 0

    @staticmethod
    def remove_stale(path: str):
        """Remove a segment left by a writer that isn't running any more"""
        try:
            with open(path, "rb") as f:
                header = f.read(WRITER_PID + 4)
        except FileNotFoundError:
            return
        except OSError as e:
            raise ShmError(f"can't read the shared memory {path}: {e}") from e
        if len(header) == WRITER_PID + 4 and header[:8] == MAGIC:
            state = struct.unpack_from("<I", header, STATE)[0]
            pid = struct.unpack_from("<I", header, WRITER_PID)[0]
            if state == WRITING and pid and pid_alive(pid):
                raise ShmError(f"{path} is being written by process {pid}")
        os.unlink(path)

    def write(self, data, timeout: float = None):
        """
        Append bytes of whole samples. Lockstep: once each registered reader
        has room for them, in pieces of at most the ring; raises ShmTimeout
        if a piece's wait takes longer than timeout seconds (None: no
        limit), a write of at most the ring having written nothing (a
        longer one may have written whole pieces: see written). Otherwise
        at once, more than the ring keeping their end
        """
        view = memoryview(data).cast("B")
        size = len(view)
        if size % self.sample_bytes:
            raise ShmError(f"{size} bytes: not whole samples of {self.sample_bytes}")
        if self.slots:
            for start in range(0, size, self.ring_bytes):
                piece = view[start : start + self.ring_bytes]
                self.wait_for_room(len(piece), timeout)
                self.append(piece)
            return
        self.append(view)

    def append(self, view: memoryview):
        """Append bytes of whole samples, overwriting the oldest"""
        size = len(view)
        if size > self.ring_bytes:
            view, skipped = view[size - self.ring_bytes :], size - self.ring_bytes
        else:
            skipped = 0
        # Readers drop what they copied before write_end - ring_bytes: stored
        # before the bytes go in. One aligned store each (ctypes); x86-64
        # keeps stores in order, Python having no fences (see SPEC.md)
        self.end.value = self.written + size
        start = (self.written + skipped) % self.ring_bytes
        first = min(len(view), self.ring_bytes - start)
        self.ring[start : start + first] = view[:first]
        if first < len(view):
            self.ring[: len(view) - first] = view[first:]
        self.written += size
        self.count.value = self.written
        self.heartbeat.value = time.time_ns()

    def active_slots(self) -> list:
        """The slots of readers registered with this writer"""
        return [slot for slot in self.slots
                if slot.state.value == SLOT_ACTIVE and slot.session.value == self.session_id]

    def evict(self, slot: Slot):
        """Stop waiting for a slot's reader"""
        slot.state.value = SLOT_EVICTED
        self.evicted += 1

    def wait_for_room(self, size: int, timeout: float = None):
        """
        Until every registered reader has read far enough for size more
        bytes: a reader that died (its slot's lock went) or whose heartbeat
        stopped for the reader timeout while in the way is evicted. Raises
        ShmTimeout after timeout seconds (0: look once; None: no limit)
        """
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            blocked = False
            for slot in self.active_slots():
                if self.written + size - slot.pos.value <= self.ring_bytes:
                    continue
                gone = slot_lock(self.fd, slot.index, cmd=F_OFD_GETLK) == fcntl.F_UNLCK
                if gone or time.time_ns() - slot.heartbeat.value > self.timeout_ns:
                    self.evict(slot)
                else:
                    blocked = True
            if not blocked:
                return
            if deadline is not None and time.monotonic() >= deadline:
                raise ShmTimeout(f"{self.path}: readers didn't make room within {timeout:g} s")
            time.sleep(LOCKSTEP_POLL)

    def wait_for_readers(self, count: int, timeout: float) -> int:
        """Up to timeout seconds for count readers to register; how many did"""
        deadline = time.monotonic() + timeout
        while len(self.active_slots()) < count and time.monotonic() < deadline:
            time.sleep(LOCKSTEP_POLL)
        return len(self.active_slots())

    def add_lost(self, samples: int):
        """Samples the SDR lost (written as zeros too)"""
        self.lost.value += samples

    def close(self, state: int = ENDED):
        """
        End the stream: readers drain it and stop. The name is removed if it
        is still this segment's (not a newer writer's that replaced it)
        """
        struct.pack_into("<I", self.map, STATE, state)
        del self.count, self.end, self.heartbeat, self.lost
        self.slots = []
        super().close()
        try:
            stat = os.stat(self.path)
            if (stat.st_dev, stat.st_ino) == self.file_id:
                os.unlink(self.path)
        except FileNotFoundError:
            pass


@dataclass
class Chunk:
    """
    Bytes read from a stream
    data: The bytes, whole samples
    offset: Stream byte offset of data[0] (offset // sample_bytes: its sample index)
    skipped: Bytes lost before them, overwritten before they were read
    """

    data: bytes
    offset: int
    skipped: int


class ShmReader(Segment):  # pylint: disable=too-many-instance-attributes
    """A processing program's side: reads the stream from its own position"""

    def __init__(self, name: str, *, oldest: bool = False, register: bool = False):
        """
        :param oldest: Start at the oldest bytes still in the ring, rather
            than the newest
        :param register: Have a lockstep writer wait for this reader (see
            register); raises ShmError if it can't
        """
        path = segment_path(name)
        try:
            fd = os.open(path, os.O_RDONLY)
            size = os.fstat(fd).st_size
        except OSError as e:
            raise ShmError(f"can't open the shared memory {path}: {e}") from e
        if size < HEADER_BYTES:
            os.close(fd)
            raise ShmError(f"{path} is too small for a header")
        super().__init__(fd, size, writable=False)
        magic, version = struct.unpack_from("<8sI", self.map, 0)
        if magic != MAGIC or version != VERSION:
            self.close()
            raise ShmError(f"{path} isn't version {VERSION} of the SDR shared memory")
        count = self.write_count
        start = max(0, count - self.ring_bytes) if oldest else count
        self.pos = self.ceil_sample(start)
        self.path = path
        self.slot = None
        self.header = None
        self.evicted = False
        if register:
            try:
                self.register()
            except ShmError:
                self.close()
                raise

    def register(self):
        """
        Take a free slot of a lockstep writer, which then waits for this
        reader to read rather than overwrite (evicted if it stops reading)
        """
        if not self.lockstep:
            raise ShmError(f"{self.path}: its writer isn't in lockstep")
        try:
            self.fd = os.open(self.path, os.O_RDWR)
        except OSError as e:
            raise ShmError(f"can't register with {self.path}: {e}") from e
        self.header = mmap.mmap(self.fd, HEADER_BYTES, access=mmap.ACCESS_WRITE)
        for index in range(SLOTS):
            try:
                slot_lock(self.fd, index)
            except BlockingIOError:
                continue
            slot = Slot(self.header, index)
            slot.pid.value = os.getpid()
            slot.session.value = self.session
            slot.pos.value = self.pos
            slot.heartbeat.value = time.time_ns()
            slot.state.value = SLOT_ACTIVE
            self.slot = slot
            return
        raise ShmError(f"{self.path}: all {SLOTS} reader slots taken")

    @property
    def registered(self) -> bool:
        """Whether the writer waits for this reader"""
        return self.slot is not None

    def release(self):
        """Free the slot: no longer waited for"""
        if self.slot is not None:
            if self.slot.state.value == SLOT_ACTIVE:
                self.slot.state.value = SLOT_FREE
            slot_lock(self.fd, self.slot.index, fcntl.F_UNLCK)
            self.slot = None

    def ceil_sample(self, offset: int) -> int:
        """offset rounded up to a whole sample"""
        return -(-offset // self.sample_bytes) * self.sample_bytes

    def read(self, max_bytes: int = None) -> Chunk:
        """What was written since the last read (whole samples), maybe nothing"""
        count = self.write_count
        skipped = 0
        if count - self.pos > self.ring_bytes:
            new = self.ceil_sample(count - self.ring_bytes)
            skipped, self.pos = new - self.pos, new
        end = count if max_bytes is None else min(count, self.pos + max_bytes)
        end -= (end - self.pos) % self.sample_bytes
        start = self.pos % self.ring_bytes
        size = end - self.pos
        first = min(size, self.ring_bytes - start)
        data = bytes(self.ring[start : start + first]) + bytes(self.ring[: size - first])
        # Overwritten while copying, or being overwritten: drop them
        overwritten = self.write_end - self.ring_bytes - self.pos
        if overwritten > 0:
            cut = min(self.ceil_sample(overwritten), size)
            data, skipped, self.pos = data[cut:], skipped + cut, self.pos + cut
        chunk = Chunk(data, self.pos, skipped)
        self.pos += len(data)
        if self.slot is not None:
            # After the copy: x86-64 keeps loads before later stores
            self.slot.pos.value = self.pos
            self.slot.heartbeat.value = time.time_ns()
            if self.slot.state.value == SLOT_EVICTED:
                self.evicted = True
                self.release()
        return chunk

    def wait(self, timeout: float, max_bytes: int = None, poll: float = 0.001) -> Chunk:
        """read() once it has bytes, the stream ends or timeout seconds pass"""
        deadline = time.monotonic() + timeout
        while True:
            chunk = self.read(max_bytes)
            if chunk.data or chunk.skipped or self.ended or time.monotonic() >= deadline:
                return chunk
            time.sleep(poll)

    @property
    def ended(self) -> bool:
        """The capture stopped and everything it wrote has been read"""
        return self.state != WRITING and self.pos >= self.write_count

    def close(self):
        """Free the slot, unmap"""
        if self.header is not None:
            self.release()
            self.header.close()
            self.header = None
        super().close()


def pid_alive(pid: int) -> bool:
    """Whether a process exists"""
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True
