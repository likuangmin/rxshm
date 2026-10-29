# rxshm: SDR samples in shared memory

A writer — an SDR's capture process, or a replay of a recording — publishes
the stream of IF samples it captures in a ring in shared memory, and any
number of readers in other processes read it live. A live capture never
waits for a reader: a reader that falls behind loses samples, and is told
how many. A replay may instead wait for the readers registered with it
(lockstep), so that each run gets the same samples.

Layout version 1. Implementations: [c/include/rxshm/sdr_shm.h](c/include/rxshm/sdr_shm.h)
(C, header only: reader and writer) and [python/rxshm](python/rxshm/__init__.py)
(Python, standard library only: reader and writer).

## Segments

A segment is a POSIX shared memory object, `/dev/shm/<name>` on Linux,
carrying one stream: e.g. the bytes written to one IF data file. An SDR
recording two channels into two files publishes two segments. Names are
the writers' to choose. GnssBenchmark names its segments
`rxbenchmark-<receiver>-<n>`: the receiver's name (characters other than
letters, digits, `_`, `.` and `-` as `_`) and the data file's index (0
first).

The writer creates the segment when the capture starts and removes the name
when it ends. Readers that have it mapped keep it until they unmap it, and
see that it ended (`state`). A new capture under the same name creates a new
segment; readers of the old one see it ended, and open the name again to
follow the new one.

Creating: a segment of the name whose `state` is 1 (writing) and whose
`writer_pid` is a running process belongs to a running writer, and is not
replaced (two captures of the same SDR, or two pipelines of the same name,
are an error), unless the writer asks to replace it (`SDR_SHM_REPLACE`,
`replace=True`). Anything else under the name is stale and is removed.
Closing: the writer removes the name only if it is still its own segment
(the same `st_dev` and `st_ino` as the file it created), not a newer
writer's that replaced it.

## Layout

All integers are little-endian. The header is 4096 bytes, followed by the
ring of `ring_bytes` bytes.

| Offset | Type | Field | Meaning |
| --- | --- | --- | --- |
| 0 | char[8] | `magic` | `RXBSHM1\0` |
| 8 | u32 | `version` | 1 |
| 12 | u32 | `header_bytes` | 4096: where the ring starts |
| 16 | u64 | `ring_bytes` | Size of the ring, a multiple of `sample_bytes` and of 4096 |
| 24 | u32 | `sample_bytes` | Bytes of one sample instant of the stream (all RF channels in it): e.g. 4 for CS16, 2 for CS8 and RAW16 |
| 28 | u32 | `state` | 1 writing, 2 ended (the capture stopped), 3 failed (the capture stopped on an error) |
| 32 | u64 | `write_count` | Bytes written since the start: atomic, see Reading |
| 40 | u64 | `heartbeat_ns` | Unix time (ns) of the last write |
| 48 | i64 | `start_ns` | Unix time (ns, UTC) of the stream's first sample by the capture's clock, 0 if unknown |
| 56 | u64 | `lost_samples` | Samples the SDR lost, written as zeros |
| 64 | u64 | `session` | Random, new for each capture |
| 72 | u32 | `writer_pid` | Process ID of the writer |
| 76 | u32 | `flags` | Bit 0 `LOCKSTEP`: the writer waits for registered readers (see Lockstep). Readers ignore other bits |
| 80 | u32 | `slot_offset` | Lockstep: where the reader slots are, 2048; 0 otherwise |
| 84 | u32 | `slot_count` | Lockstep: 32 |
| 88 | u32 | `slot_bytes` | Lockstep: 64 |
| 92 | u32 | `reserved2` | 0 |
| 96 | u64 | `lockstep_timeout_ns` | Lockstep: a registered reader whose heartbeat is older than this while the writer waits for it is evicted |
| 104 | u64 | `write_end` | Stream byte offset up to which the writer may be writing (≥ `write_count`), stored before it writes bytes into the ring; 0 if the writer doesn't (then it is `write_count`) |
| 112 | | | 0 |
| 128 | char[1024] | `tag` | A Pocket SDR tag (the text of a `<data>.tag` file) describing the samples: `FMT`, `F_S`, `F_LO`, `IQ`, `BITS`, `TIME`, NUL-padded |
| 1152 | char[512] | `data_path` | The IF data file the stream mirrors, "" if it isn't written to disk |
| 2048 | slot[32] | slots | Lockstep: the registered readers' slots, 64 bytes each (see Lockstep) |
| 4096 | bytes | ring | Byte `k` of the stream is at ring offset `k % ring_bytes` |

The stream's samples are packed as the tag describes. Sample `i` starts at
stream byte `i * sample_bytes`; its time is `start_ns + i / F_S` (F_S in
samples per second), `write_count` being a multiple of `sample_bytes`
whenever it is read.

## Writing

To append `n` bytes (whole samples, at most `ring_bytes`), the writer:

1. Stores `write_end` = `write_count + n`, then a release fence (stores
   before it stay before the stores after it).
2. Copies the bytes into the ring.
3. Stores the new `write_count` with release ordering (one aligned 64-bit
   store), then `heartbeat_ns`.

It never waits for readers (but see Lockstep): bytes older than
`write_count + n - ring_bytes` are overwritten. A write longer than the
ring goes in pieces of at most `ring_bytes`, or keeps only its last
`ring_bytes`. At the end it sets `state` to 2 (or 3) and removes the name.

## Reading

A reader keeps its own position `pos` (a stream byte offset, a multiple of
`sample_bytes`):

1. Load `write_count` (acquire): `w`.
2. If `w - pos > ring_bytes`, the bytes from `pos` were overwritten: skip
   to `w - ring_bytes` (rounded up to a sample), counting the bytes skipped.
3. Copy the bytes from `pos` up to `w` out of the ring (two pieces when
   they wrap).
4. An acquire fence (loads before it stay before the loads after it), then
   load `write_count` and `write_end`: `e`, the larger of the two. Bytes
   before `e - ring_bytes` were overwritten while copying, or are being
   overwritten: drop them from the front of the copy (rounded up to a
   sample), counting them as skipped.
5. `pos` moves past what was kept.

A reader may start at the newest byte (`pos = write_count`, rounded up to
a sample) or the oldest still in the ring (`max(0, write_count -
ring_bytes)`). Its first sample's index is `pos / sample_bytes`, which ties
its own sample count to the stream's time.

The stream has ended when `state` is not 1 and `pos == write_count`. A
`heartbeat_ns` that stops moving while `state` is 1 means the writer died.

Readers poll: there is no wake-up. A ring of about 2 s and a poll every
millisecond or so leaves plenty of slack.

`write_end` (step 1 and 4) was added in rxshm 1.0.0, within version 1: a
reader that ignores it, or a writer that leaves it 0, works as before,
except that a reader nearly a ring behind may keep bytes the writer was
overwriting while it copied them, which `write_end` makes it drop.

## Lockstep

A writer replaying a recording may wait for its readers instead of
overwriting what they haven't read, so that a run is reproducible: it sets
`flags` bit 0 (`LOCKSTEP`) and the slot fields. A live SDR's capture never
does. Lockstep is additive: readers that don't register are never waited
for, and lose bytes as above.

A slot (64 bytes at `slot_offset + i * slot_bytes`):

| Offset | Type | Field | Meaning |
| --- | --- | --- | --- |
| 0 | u32 | `state` | 0 free, 2 active, 3 evicted |
| 4 | u32 | `pid` | The reader's process ID (information) |
| 8 | u64 | `pos` | Stream byte offset the reader has read up to, a multiple of `sample_bytes` like `write_count`; stored (release) after each copy |
| 16 | u64 | `heartbeat_ns` | Unix time (ns) of the reader's last read |
| 24 | u64 | `session` | The header's `session` when it registered |
| 32 | bytes | | 0 |

Registering: the reader opens the segment read-write (the writer's user)
and takes a Linux open file description lock (`fcntl` `F_OFD_SETLK`,
`F_WRLCK`) on the first slot's 64 bytes it can, then sets `pid`,
`session`, `pos` (its position) and `heartbeat_ns`, then `state` = 2. It
holds the lock while registered; the kernel releases it if the reader dies.
After each read it stores `pos`, then `heartbeat_ns`; if it then finds
`state` = 3 it was evicted: it frees the slot (unlocks) and reads on as an
unregistered reader. Closing: `state` = 0, then unlock.

Writing: before writing `n` bytes, the writer waits while an active slot of
its session has `write_count + n - pos > ring_bytes`, looking again every
fraction of a millisecond. Such a slot is evicted (`state` = 3) if no lock
is held on it any more (`F_OFD_GETLK`: its reader died) or its
`heartbeat_ns` is older than `lockstep_timeout_ns`. Then it writes as
above. The writer may give up waiting after a timeout of its own, having
written nothing of those `n` bytes (`ETIMEDOUT`, `ShmTimeout`).

A replay may also wait for a number of readers to register before its
first byte, so that they get the stream from its start. When that wait
times out, the writer starts anyway, waiting for the readers registered by
then (none: it writes as a writer without lockstep would); readers that
register later are waited for from then on. (`wait_for_readers` and
`sdr_shm_writer_wait_readers` return how many registered; a writer may
also choose to fail instead.)

Lockstep readers store `pos` with release ordering after copying, and the
writer loads it (acquire) before overwriting, so it never overwrites bytes
a registered reader is still copying.

## Platform

Linux only (OFD locks, `/dev/shm`), little-endian. Aligned 64-bit loads
and stores are atomic on x86-64 and AArch64; the C reader and writer order
them with `__atomic` builtins and fences. Python has neither atomics nor
fences: its writer and readers rely on x86-64 keeping stores in order and
loads in order (and loads before later stores), and load 64-bit fields
until two loads agree. On AArch64 (e.g. a Raspberry Pi 5) use the C
writer; a Python reader there may, rarely, keep a byte it shouldn't.
