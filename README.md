# rxshm

SDR samples in shared memory. A capture process (or a replay of a
recording) publishes the IF samples it reads from an SDR in a ring in
shared memory, and any number of programs read them live as separate
processes: a tracker, a recorder, your own processing. A live capture never
waits for its readers; a replay may (lockstep), so that each run is fed the
same samples.

* [SPEC.md](SPEC.md): the layout and protocol (version 1), all you need to
  write another implementation
* [c/include/rxshm/sdr_shm.h](c/include/rxshm/sdr_shm.h): reader and writer
  in C, header only, no dependencies (C11 or C++, POSIX.1-2008)
* [python/rxshm](python/rxshm/__init__.py): reader and writer in Python,
  standard library only (3.10 or later)
* [c/tools](c/tools): `sdr_shm_cat` writes a stream to stdout (e.g. into a
  program reading IF data from a pipe), `sdr_shm_put` publishes stdin

Linux only. Used by [GnssBenchmark](https://github.com/likuangmin/GnssBenchmark)'s
SDR recorders and processors, and by RealSuite.

## C and C++

Header only. With CMake, the target `rxshm::rxshm` adds the include
directory (as SYSTEM, so C++ warnings such as `-Wold-style-cast` stay
quiet about it) and `_POSIX_C_SOURCE=200809L`:

```cmake
include(FetchContent)
FetchContent_Declare(rxshm GIT_REPOSITORY https://github.com/likuangmin/rxshm GIT_TAG v1.0.0)
FetchContent_MakeAvailable(rxshm)
target_link_libraries(myprogram PRIVATE rxshm::rxshm)
```

or `find_package(rxshm 1.0 REQUIRED)` once installed (`cmake -S . -B build
&& cmake --install build`). Without CMake: `-I c/include`, and
`-D_POSIX_C_SOURCE=200809L` with a strict `-std=c11`.

Reading:

```c
#include <rxshm/sdr_shm.h>

sdr_shm_t shm;
if (sdr_shm_open(&shm, "rxbenchmark-bladeRF-0", 0) != 0) ...;
for (;;) {
    uint64_t offset, skipped;
    size_t n = sdr_shm_read(&shm, buf, sizeof buf, &offset, &skipped);
    if (n == 0) {
        if (sdr_shm_ended(&shm)) break;
        sdr_shm_sleep_ns(1000000);
        continue;
    }
    /* n bytes of whole samples; offset / shm.sample_bytes: the first's index */
}
sdr_shm_close(&shm);
```

`sdr_shm_register(&shm)` after opening has a lockstep writer wait for this
reader. Writing:

```c
sdr_shm_writer_t w;
if (sdr_shm_writer_create(&w, "myreplay-0", ring_bytes, sample_bytes, tag_text, "", 0,
                          SDR_SHM_FLAG_LOCKSTEP, 0) != 0) ...;
sdr_shm_writer_wait_readers(&w, 1, 5000000000u);
sdr_shm_writer_write(&w, data, n, SDR_SHM_FOREVER);   /* or reserve, fill, commit */
sdr_shm_writer_close(&w, SDR_SHM_ENDED);
```

## Python

```
pip install "rxshm @ git+https://github.com/likuangmin/rxshm@v1.0.0#subdirectory=python"
```

```python
from rxshm import ShmReader, ShmWriter, ring_size

writer = ShmWriter("myreplay-0", ring_bytes=ring_size(2.0, 4.0, 4), sample_bytes=4,
                   tag_text=open("rec.bin.tag").read())
writer.write(samples)
writer.close()

reader = ShmReader("myreplay-0", oldest=True)       # register=True: lockstep
while not reader.ended:
    chunk = reader.wait(1.0)   # chunk.data, chunk.offset, chunk.skipped
reader.close()
```

## Tests

```
cmake -S . -B build -DRXSHM_WERROR=ON && cmake --build build && ctest --test-dir build
```

or `python3 -m unittest` alone, which builds the C programs with `cc` and
`c++`. Python and C writers against Python and C readers, lossy and in
lockstep; dead and hung readers; the C structs' layout against Python's.

## Versions

The layout's `version` stays 1 while changes are additive (new flags,
fields in unused space): readers ignore what they don't know. The
repository's tags (v1.0.0, ...) version the implementations.
