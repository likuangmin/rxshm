/*
 * rxshm: SDR samples in shared memory, version 1 of the layout (SPEC.md).
 * A writer (a capture, or a replay of a recording) publishes a stream of
 * IF samples in a ring; readers in other processes read it live. Header
 * only; Linux; needs POSIX.1-2008 (_POSIX_C_SOURCE=200809L, which the CMake
 * target rxshm::rxshm defines: strict -std=c11 lacks clock_gettime).
 *
 * Reader:
 *
 *   sdr_shm_t shm;
 *   if (sdr_shm_open(&shm, "rxbenchmark-bladeRF-0", 0) != 0) ...;
 *   for (;;) {
 *       uint64_t offset, skipped;
 *       size_t n = sdr_shm_read(&shm, buf, sizeof buf, &offset, &skipped);
 *       if (n == 0) {
 *           if (sdr_shm_ended(&shm)) break;
 *           sdr_shm_sleep_ns(1000000);
 *           continue;
 *       }
 *       // n bytes of whole samples; offset / shm.sample_bytes: the first's index
 *   }
 *   sdr_shm_close(&shm);
 *
 * A lockstep writer (a replay) waits for the readers registered with it:
 * call sdr_shm_register(&shm) after sdr_shm_open. A registered reader that
 * stops reading is evicted (sdr_shm_evicted) and goes on lossy.
 *
 * Writer:
 *
 *   sdr_shm_writer_t w;
 *   if (sdr_shm_writer_create(&w, "myreplay-0", ring_bytes, 4, tag, "", 0,
 *                             SDR_SHM_FLAG_LOCKSTEP, 0) != 0) ...;
 *   sdr_shm_writer_wait_readers(&w, 1, 5000000000u);
 *   while (...) sdr_shm_writer_write(&w, data, n, SDR_SHM_FOREVER);
 *   sdr_shm_writer_close(&w, SDR_SHM_ENDED);
 *
 * or sdr_shm_writer_reserve / sdr_shm_writer_commit to write into the ring
 * directly.
 */

#ifndef RXSHM_SDR_SHM_H
#define RXSHM_SDR_SHM_H

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Linux open file description locks, without _GNU_SOURCE */
#ifndef F_OFD_GETLK
#define F_OFD_GETLK 36
#endif
#ifndef F_OFD_SETLK
#define F_OFD_SETLK 37
#endif

#define SDR_SHM_MAGIC "RXBSHM1"
#define SDR_SHM_VERSION 1
#define SDR_SHM_HEADER_BYTES 4096
#define SDR_SHM_WRITING 1
#define SDR_SHM_ENDED 2
#define SDR_SHM_FAILED 3
#define SDR_SHM_FLAG_LOCKSTEP 1u
#define SDR_SHM_SLOT_FREE 0
#define SDR_SHM_SLOT_ACTIVE 2
#define SDR_SHM_SLOT_EVICTED 3
/* Where a lockstep writer puts the reader slots, how many, their size */
#define SDR_SHM_SLOT_OFFSET 2048
#define SDR_SHM_SLOTS 32
#define SDR_SHM_SLOT_BYTES 64
/* sdr_shm_writer_create flag: unlink an existing segment of the name even
   if its writer is running (by default only a stale one is) */
#define SDR_SHM_REPLACE 0x80000000u
/* A registered reader's heartbeat timeout by default (ns), and how long a
   lockstep writer sleeps between looks at its readers */
#define SDR_SHM_READER_TIMEOUT_NS 2000000000u
#define SDR_SHM_POLL_NS 200000
/* Waits without a timeout */
#define SDR_SHM_FOREVER UINT64_MAX

/* The header, as laid out in the segment */
typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t header_bytes;
    uint64_t ring_bytes;
    uint32_t sample_bytes;
    uint32_t state;
    uint64_t write_count;
    uint64_t heartbeat_ns;
    int64_t start_ns;
    uint64_t lost_samples;
    uint64_t session;
    uint32_t writer_pid;
    uint32_t flags;            /* SDR_SHM_FLAG_LOCKSTEP; ignore other bits */
    uint32_t slot_offset;      /* lockstep: where the reader slots are (2048) */
    uint32_t slot_count;       /* 32 */
    uint32_t slot_bytes;       /* 64 */
    uint32_t reserved2;
    uint64_t lockstep_timeout_ns;
    uint64_t write_end;   /* the writer may be writing bytes up to here; 0: unknown */
    char unused[16];
    char tag[1024];       /* the samples' Pocket SDR tag text */
    char data_path[512];  /* the IF data file it mirrors */
} sdr_shm_header_t;

/* A lockstep reader's slot */
typedef struct {
    uint32_t state;  /* SDR_SHM_SLOT_FREE, _ACTIVE, _EVICTED */
    uint32_t pid;
    uint64_t pos;    /* stream byte offset read up to (release store) */
    uint64_t heartbeat_ns;
    uint64_t session;
    char pad[32];
} sdr_shm_slot_t;

#ifdef __cplusplus
#define SDR_SHM_ASSERT(c, m) static_assert(c, m)
#else
#define SDR_SHM_ASSERT(c, m) _Static_assert(c, m)
#endif
SDR_SHM_ASSERT(offsetof(sdr_shm_header_t, ring_bytes) == 16, "layout");
SDR_SHM_ASSERT(offsetof(sdr_shm_header_t, write_count) == 32, "layout");
SDR_SHM_ASSERT(offsetof(sdr_shm_header_t, writer_pid) == 72, "layout");
SDR_SHM_ASSERT(offsetof(sdr_shm_header_t, flags) == 76, "layout");
SDR_SHM_ASSERT(offsetof(sdr_shm_header_t, lockstep_timeout_ns) == 96, "layout");
SDR_SHM_ASSERT(offsetof(sdr_shm_header_t, write_end) == 104, "layout");
SDR_SHM_ASSERT(offsetof(sdr_shm_header_t, tag) == 128, "layout");
SDR_SHM_ASSERT(offsetof(sdr_shm_header_t, data_path) == 1152, "layout");
SDR_SHM_ASSERT(sizeof(sdr_shm_header_t) <= SDR_SHM_SLOT_OFFSET, "layout");
SDR_SHM_ASSERT(sizeof(sdr_shm_slot_t) == SDR_SHM_SLOT_BYTES, "layout");
SDR_SHM_ASSERT(offsetof(sdr_shm_slot_t, pos) == 8, "layout");
SDR_SHM_ASSERT(offsetof(sdr_shm_slot_t, heartbeat_ns) == 16, "layout");
SDR_SHM_ASSERT(offsetof(sdr_shm_slot_t, session) == 24, "layout");

typedef struct {
    volatile sdr_shm_header_t *header;
    const uint8_t *ring;
    size_t map_bytes;
    uint64_t ring_bytes;
    uint32_t sample_bytes;
    uint64_t pos;  /* stream byte offset of the next byte to read */
    char path[256];
    /* Registered (lockstep): the segment open read-write, its header
       mapped writable, the slot locked; evicted: the writer stopped
       waiting for this reader */
    int fd;
    void *rw_header;
    volatile sdr_shm_slot_t *slot;
    int slot_index;
    int evicted;
} sdr_shm_t;

static inline uint64_t sdr_shm_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static inline uint64_t sdr_shm_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static inline void sdr_shm_sleep_ns(uint64_t ns)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000u);
    ts.tv_nsec = (long)(ns % 1000000000u);
    nanosleep(&ts, NULL);
}

/* /dev/shm/<name> into path */
static inline void sdr_shm_path(char *path, size_t size, const char *name)
{
    path[0] = '\0';
    strncat(path, "/dev/shm/", size - 1);
    strncat(path, name, size - strlen(path) - 1);
}

static inline uint64_t sdr_shm_write_count(const sdr_shm_t *shm)
{
    return __atomic_load_n(&shm->header->write_count, __ATOMIC_ACQUIRE);
}

static inline uint32_t sdr_shm_state(const sdr_shm_t *shm)
{
    return __atomic_load_n(&shm->header->state, __ATOMIC_ACQUIRE);
}

static inline uint64_t sdr_shm_ceil_sample(const sdr_shm_t *shm, uint64_t offset)
{
    return (offset + shm->sample_bytes - 1) / shm->sample_bytes * shm->sample_bytes;
}

/*
 * Open a segment by name (e.g. "rxbenchmark-bladeRF-0"), starting at the
 * newest bytes, or the oldest still in the ring if oldest is non-zero.
 * Returns 0, or -1 (errno set; EPROTO: not version 1 of this interface).
 */
static inline int sdr_shm_open(sdr_shm_t *shm, const char *name, int oldest)
{
    struct stat st;
    void *map;
    int fd;

    memset(shm, 0, sizeof(*shm));
    shm->fd = -1;
    shm->slot_index = -1;
    sdr_shm_path(shm->path, sizeof(shm->path), name);
    if ((fd = open(shm->path, O_RDONLY)) < 0) return -1;
    if (fstat(fd, &st) < 0 || (size_t)st.st_size < SDR_SHM_HEADER_BYTES) {
        close(fd);
        errno = EPROTO;
        return -1;
    }
    map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return -1;
    shm->header = (volatile sdr_shm_header_t *)map;
    shm->map_bytes = (size_t)st.st_size;
    if (memcmp((const void *)shm->header->magic, SDR_SHM_MAGIC, 8) != 0 ||
        shm->header->version != SDR_SHM_VERSION) {
        munmap(map, shm->map_bytes);
        shm->header = NULL;
        errno = EPROTO;
        return -1;
    }
    shm->ring = (const uint8_t *)map + shm->header->header_bytes;
    shm->ring_bytes = shm->header->ring_bytes;
    shm->sample_bytes = shm->header->sample_bytes;
    uint64_t count = sdr_shm_write_count(shm);
    uint64_t start = count;
    if (oldest) start = count > shm->ring_bytes ? count - shm->ring_bytes : 0;
    shm->pos = sdr_shm_ceil_sample(shm, start);
    return 0;
}

/* fcntl(2) OFD lock (F_WRLCK) or unlock (F_UNLCK) of a slot's bytes */
static inline int sdr_shm_slot_lock(const sdr_shm_t *shm, int index, short type)
{
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    fl.l_start = (off_t)shm->header->slot_offset + (off_t)index * shm->header->slot_bytes;
    fl.l_len = (off_t)shm->header->slot_bytes;
    return fcntl(shm->fd, F_OFD_SETLK, &fl);
}

/* Free the slot: no longer waited for */
static inline void sdr_shm_release(sdr_shm_t *shm)
{
    if (!shm->slot) return;
    if (__atomic_load_n(&shm->slot->state, __ATOMIC_ACQUIRE) == SDR_SHM_SLOT_ACTIVE)
        __atomic_store_n(&shm->slot->state, SDR_SHM_SLOT_FREE, __ATOMIC_RELEASE);
    sdr_shm_slot_lock(shm, shm->slot_index, F_UNLCK);
    shm->slot = NULL;
    shm->slot_index = -1;
}

/*
 * Register with a lockstep writer, which then waits for this reader to read
 * rather than overwrite (it is evicted if it stops reading). Returns 0, or
 * -1 (errno set; ENOTSUP: the writer isn't in lockstep; EBUSY: no free
 * slot; or why the segment can't be opened read-write).
 */
static inline int sdr_shm_register(sdr_shm_t *shm)
{
    const sdr_shm_header_t *h = (const sdr_shm_header_t *)shm->header;
    if (!(h->flags & SDR_SHM_FLAG_LOCKSTEP) || h->slot_offset == 0) {
        errno = ENOTSUP;
        return -1;
    }
    if (shm->fd < 0) {
        if ((shm->fd = open(shm->path, O_RDWR)) < 0) return -1;
        shm->rw_header = mmap(NULL, SDR_SHM_HEADER_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED,
                              shm->fd, 0);
        if (shm->rw_header == MAP_FAILED) {
            int error = errno;
            close(shm->fd);
            shm->fd = -1;
            shm->rw_header = NULL;
            errno = error;
            return -1;
        }
    }
    for (uint32_t i = 0; i < h->slot_count; i++) {
        if (sdr_shm_slot_lock(shm, (int)i, F_WRLCK) != 0) continue;
        volatile sdr_shm_slot_t *slot = (volatile sdr_shm_slot_t *)
            ((uint8_t *)shm->rw_header + h->slot_offset + i * h->slot_bytes);
        slot->pid = (uint32_t)getpid();
        slot->session = h->session;
        __atomic_store_n(&slot->pos, shm->pos, __ATOMIC_RELEASE);
        __atomic_store_n(&slot->heartbeat_ns, sdr_shm_now_ns(), __ATOMIC_RELEASE);
        __atomic_store_n(&slot->state, SDR_SHM_SLOT_ACTIVE, __ATOMIC_RELEASE);
        shm->slot = slot;
        shm->slot_index = (int)i;
        shm->evicted = 0;
        return 0;
    }
    errno = EBUSY;
    return -1;
}

/* Registered, and the writer stopped waiting for this reader */
static inline int sdr_shm_evicted(const sdr_shm_t *shm)
{
    return shm->evicted;
}

/*
 * Copy up to size bytes (whole samples) written since the last read into
 * buf. *offset: the stream byte offset of buf[0]; *skipped: bytes lost
 * before it, overwritten before they were read. Returns the bytes copied,
 * 0 if none yet.
 */
static inline size_t sdr_shm_read(sdr_shm_t *shm, uint8_t *buf, size_t size,
                                  uint64_t *offset, uint64_t *skipped)
{
    uint64_t count = sdr_shm_write_count(shm), end, start, first, written;
    size_t n;

    *skipped = 0;
    if (count - shm->pos > shm->ring_bytes) {
        uint64_t next = sdr_shm_ceil_sample(shm, count - shm->ring_bytes);
        *skipped = next - shm->pos;
        shm->pos = next;
    }
    end = count - shm->pos > size ? shm->pos + size : count;
    end -= (end - shm->pos) % shm->sample_bytes;
    n = (size_t)(end - shm->pos);
    start = shm->pos % shm->ring_bytes;
    first = shm->ring_bytes - start < n ? shm->ring_bytes - start : n;
    memcpy(buf, shm->ring + start, (size_t)first);
    memcpy(buf + first, shm->ring, n - (size_t)first);
    /* Overwritten while copying, or being overwritten (up to write_end):
       drop them */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    written = sdr_shm_write_count(shm);
    end = __atomic_load_n(&shm->header->write_end, __ATOMIC_RELAXED);
    if (end > written) written = end;
    if (written > shm->ring_bytes && written - shm->ring_bytes > shm->pos) {
        uint64_t cut = sdr_shm_ceil_sample(shm, written - shm->ring_bytes - shm->pos);
        if (cut > n) cut = n;
        memmove(buf, buf + cut, n - (size_t)cut);
        n -= (size_t)cut;
        *skipped += cut;
        shm->pos += cut;
    }
    *offset = shm->pos;
    shm->pos += n;
    if (shm->slot) {
        /* After the copy: the writer may overwrite what was read */
        __atomic_store_n(&shm->slot->pos, shm->pos, __ATOMIC_RELEASE);
        __atomic_store_n(&shm->slot->heartbeat_ns, sdr_shm_now_ns(), __ATOMIC_RELEASE);
        if (__atomic_load_n(&shm->slot->state, __ATOMIC_ACQUIRE) == SDR_SHM_SLOT_EVICTED) {
            shm->evicted = 1;
            sdr_shm_release(shm);
        }
    }
    return n;
}

/* The capture stopped and everything it wrote has been read */
static inline int sdr_shm_ended(const sdr_shm_t *shm)
{
    return sdr_shm_state(shm) != SDR_SHM_WRITING && shm->pos >= sdr_shm_write_count(shm);
}

static inline void sdr_shm_close(sdr_shm_t *shm)
{
    if (shm->header) sdr_shm_release(shm);
    if (shm->rw_header) munmap(shm->rw_header, SDR_SHM_HEADER_BYTES);
    shm->rw_header = NULL;
    if (shm->fd >= 0) close(shm->fd);
    shm->fd = -1;
    if (shm->header) munmap((void *)shm->header, shm->map_bytes);
    shm->header = NULL;
}

/* ------------------------------------------------------------------ */
/* Writer */

typedef struct {
    volatile sdr_shm_header_t *header;
    uint8_t *ring;
    size_t map_bytes;
    uint64_t ring_bytes;
    uint32_t sample_bytes;
    uint32_t flags;
    uint64_t session;
    uint64_t written;   /* stream bytes committed */
    uint64_t reserved;  /* bytes reserved, not committed yet */
    uint64_t timeout_ns;
    unsigned evicted;   /* lockstep readers evicted so far */
    int fd;
    dev_t dev;          /* the segment's file: close unlinks only it */
    ino_t ino;
    char path[256];
} sdr_shm_writer_t;

/*
 * Unlink path, unless (without SDR_SHM_REPLACE) it is a segment its writer
 * is still writing: then -1, errno EEXIST
 */
static inline int sdr_shm_writer_unlink(const char *path, unsigned flags)
{
    if (!(flags & SDR_SHM_REPLACE)) {
        uint8_t h[80];
        int fd = open(path, O_RDONLY);
        if (fd < 0) return errno == ENOENT ? 0 : -1;
        ssize_t got = pread(fd, h, sizeof(h), 0);
        close(fd);
        if (got == (ssize_t)sizeof(h) && memcmp(h, SDR_SHM_MAGIC, 8) == 0) {
            uint32_t state, pid;
            memcpy(&state, h + offsetof(sdr_shm_header_t, state), 4);
            memcpy(&pid, h + offsetof(sdr_shm_header_t, writer_pid), 4);
            if (state == SDR_SHM_WRITING && pid != 0 &&
                (kill((pid_t)pid, 0) == 0 || errno == EPERM)) {
                errno = EEXIST;
                return -1;
            }
        }
    }
    if (unlink(path) != 0 && errno != ENOENT) return -1;
    return 0;
}

static inline uint64_t sdr_shm_random64(void)
{
    uint64_t value = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, &value, sizeof(value)) != (ssize_t)sizeof(value)) value = 0;
        close(fd);
    }
    if (value == 0) value = sdr_shm_now_ns() ^ ((uint64_t)getpid() << 32);
    return value;
}

/*
 * Create segment name with a ring of ring_bytes (whole pages of 4096 and
 * whole samples of sample_bytes). tag: the samples' Pocket SDR tag text
 * (FMT, F_S, F_LO, IQ, BITS, TIME), under 1024 bytes; data_path: the file
 * it mirrors, "" or NULL if none; start_ns: Unix time (ns) of the first
 * sample, 0 if unknown. flags: SDR_SHM_FLAG_LOCKSTEP to wait for
 * registered readers (replays; never a live capture), SDR_SHM_REPLACE to
 * take the name from a running writer. timeout_ns: lockstep, how long a
 * reader in the way may go without a heartbeat before it is evicted (0:
 * SDR_SHM_READER_TIMEOUT_NS). A stale segment of the name is replaced; a
 * running writer's is not (EEXIST). Returns 0, or -1 (errno set; EINVAL:
 * bad sizes).
 */
static inline int sdr_shm_writer_create(sdr_shm_writer_t *w, const char *name,
                                        uint64_t ring_bytes, uint32_t sample_bytes,
                                        const char *tag, const char *data_path,
                                        int64_t start_ns, unsigned flags, uint64_t timeout_ns)
{
    sdr_shm_header_t h;
    struct stat st;
    void *map;
    int error;

    memset(w, 0, sizeof(*w));
    w->fd = -1;
    if (!data_path) data_path = "";
    if (!tag) tag = "";
    if (sample_bytes == 0 || ring_bytes == 0 || ring_bytes % sample_bytes ||
        ring_bytes % 4096 || strlen(tag) >= sizeof(h.tag) ||
        strlen(data_path) >= sizeof(h.data_path) || strlen(name) + 10 > sizeof(w->path)) {
        errno = EINVAL;
        return -1;
    }
    sdr_shm_path(w->path, sizeof(w->path), name);
    if (sdr_shm_writer_unlink(w->path, flags) != 0) return -1;
    if ((w->fd = open(w->path, O_RDWR | O_CREAT | O_EXCL, 0644)) < 0) return -1;
    w->ring_bytes = ring_bytes;
    w->sample_bytes = sample_bytes;
    w->flags = flags & SDR_SHM_FLAG_LOCKSTEP;
    w->session = sdr_shm_random64();
    w->timeout_ns = timeout_ns ? timeout_ns : SDR_SHM_READER_TIMEOUT_NS;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, SDR_SHM_MAGIC, 8);
    h.version = SDR_SHM_VERSION;
    h.header_bytes = SDR_SHM_HEADER_BYTES;
    h.ring_bytes = ring_bytes;
    h.sample_bytes = sample_bytes;
    h.state = SDR_SHM_WRITING;
    h.heartbeat_ns = sdr_shm_now_ns();
    h.start_ns = start_ns;
    h.session = w->session;
    h.writer_pid = (uint32_t)getpid();
    h.flags = w->flags;
    if (w->flags & SDR_SHM_FLAG_LOCKSTEP) {
        h.slot_offset = SDR_SHM_SLOT_OFFSET;
        h.slot_count = SDR_SHM_SLOTS;
        h.slot_bytes = SDR_SHM_SLOT_BYTES;
        h.lockstep_timeout_ns = w->timeout_ns;
    }
    memcpy(h.tag, tag, strlen(tag));
    memcpy(h.data_path, data_path, strlen(data_path));
    w->map_bytes = (size_t)(SDR_SHM_HEADER_BYTES + ring_bytes);
    if (ftruncate(w->fd, (off_t)w->map_bytes) != 0 ||
        pwrite(w->fd, &h, sizeof(h), 0) != (ssize_t)sizeof(h) || fstat(w->fd, &st) != 0)
        goto fail;
    w->dev = st.st_dev;
    w->ino = st.st_ino;
    map = mmap(NULL, w->map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, w->fd, 0);
    if (map == MAP_FAILED) goto fail;
    w->header = (volatile sdr_shm_header_t *)map;
    w->ring = (uint8_t *)map + SDR_SHM_HEADER_BYTES;
    return 0;
fail:
    error = errno;
    unlink(w->path);
    close(w->fd);
    w->fd = -1;
    errno = error;
    return -1;
}

static inline volatile sdr_shm_slot_t *sdr_shm_writer_slot(const sdr_shm_writer_t *w, int i)
{
    return (volatile sdr_shm_slot_t *)((volatile uint8_t *)w->header + SDR_SHM_SLOT_OFFSET +
                                       i * SDR_SHM_SLOT_BYTES);
}

/* Whether slot i is a reader's of this writer's session */
static inline int sdr_shm_writer_active(const sdr_shm_writer_t *w, int i)
{
    volatile sdr_shm_slot_t *slot = sdr_shm_writer_slot(w, i);
    return __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) == SDR_SHM_SLOT_ACTIVE &&
           slot->session == w->session;
}

/* Lockstep readers registered with this writer (0 if not lockstep) */
static inline int sdr_shm_writer_readers(const sdr_shm_writer_t *w)
{
    int count = 0;
    if (!(w->flags & SDR_SHM_FLAG_LOCKSTEP)) return 0;
    for (int i = 0; i < SDR_SHM_SLOTS; i++) count += sdr_shm_writer_active(w, i);
    return count;
}

/* Whether a reader still holds slot i's lock (alive) */
static inline int sdr_shm_writer_held(const sdr_shm_writer_t *w, int i)
{
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = (off_t)SDR_SHM_SLOT_OFFSET + (off_t)i * SDR_SHM_SLOT_BYTES;
    fl.l_len = SDR_SHM_SLOT_BYTES;
    if (fcntl(w->fd, F_OFD_GETLK, &fl) != 0) return 1;
    return fl.l_type != F_UNLCK;
}

/*
 * Lockstep: wait until every registered reader has read far enough for n
 * more bytes, evicting one in the way that died (its lock went) or whose
 * heartbeat is older than the timeout. Returns 0, or -1 (ETIMEDOUT) after
 * timeout_ns (0: look once; SDR_SHM_FOREVER: no timeout)
 */
static inline int sdr_shm_writer_wait_room(sdr_shm_writer_t *w, uint64_t n, uint64_t timeout_ns)
{
    uint64_t began = sdr_shm_monotonic_ns();
    if (!(w->flags & SDR_SHM_FLAG_LOCKSTEP)) return 0;
    for (;;) {
        int blocked = 0;
        for (int i = 0; i < SDR_SHM_SLOTS; i++) {
            volatile sdr_shm_slot_t *slot = sdr_shm_writer_slot(w, i);
            if (!sdr_shm_writer_active(w, i)) continue;
            uint64_t pos = __atomic_load_n(&slot->pos, __ATOMIC_ACQUIRE);
            if (w->written + n - pos <= w->ring_bytes) continue;
            uint64_t beat = __atomic_load_n(&slot->heartbeat_ns, __ATOMIC_ACQUIRE);
            if (!sdr_shm_writer_held(w, i) || sdr_shm_now_ns() - beat > w->timeout_ns) {
                __atomic_store_n(&slot->state, SDR_SHM_SLOT_EVICTED, __ATOMIC_RELEASE);
                w->evicted++;
            } else {
                blocked = 1;
            }
        }
        if (!blocked) return 0;
        if (timeout_ns != SDR_SHM_FOREVER && sdr_shm_monotonic_ns() - began >= timeout_ns) {
            errno = ETIMEDOUT;
            return -1;
        }
        sdr_shm_sleep_ns(SDR_SHM_POLL_NS);
    }
}

/*
 * Reserve n bytes (whole samples, at most ring_bytes) of the ring to write
 * the next bytes of the stream into: *span1 (*len1 bytes) then *span2
 * (*len2 bytes, NULL if 0) where it wraps. Lockstep: waits for readers as
 * sdr_shm_writer_wait_room. Then sdr_shm_writer_commit publishes them.
 * Returns 0, or -1 (errno EINVAL, or ETIMEDOUT: nothing reserved)
 */
static inline int sdr_shm_writer_reserve(sdr_shm_writer_t *w, size_t n, uint64_t timeout_ns,
                                         uint8_t **span1, size_t *len1,
                                         uint8_t **span2, size_t *len2)
{
    if (n % w->sample_bytes || n > w->ring_bytes) {
        errno = EINVAL;
        return -1;
    }
    if (sdr_shm_writer_wait_room(w, n, timeout_ns) != 0) return -1;
    uint64_t start = w->written % w->ring_bytes;
    size_t first = w->ring_bytes - start < n ? (size_t)(w->ring_bytes - start) : n;
    *span1 = w->ring + start;
    *len1 = first;
    *span2 = n > first ? w->ring : NULL;
    *len2 = n - first;
    w->reserved = n;
    /* Readers drop bytes up to write_end - ring_bytes they copied: stored
       before this writer's bytes go in */
    __atomic_store_n(&w->header->write_end, w->written + n, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return 0;
}

/*
 * Publish n bytes (whole samples, at most those reserved) written into the
 * reserved spans. Returns 0, or -1 (EINVAL)
 */
static inline int sdr_shm_writer_commit(sdr_shm_writer_t *w, size_t n)
{
    if (n % w->sample_bytes || n > w->reserved) {
        errno = EINVAL;
        return -1;
    }
    w->written += n;
    w->reserved = 0;
    __atomic_store_n(&w->header->write_count, w->written, __ATOMIC_RELEASE);
    __atomic_store_n(&w->header->heartbeat_ns, sdr_shm_now_ns(), __ATOMIC_RELEASE);
    return 0;
}

/*
 * Append n bytes (whole samples): reserve, copy, commit, in pieces of at
 * most ring_bytes. timeout_ns applies to each piece's lockstep wait.
 * Returns 0, or -1 (errno EINVAL, or ETIMEDOUT: a write of at most
 * ring_bytes wrote nothing; a longer one may have written whole pieces,
 * w->written says)
 */
static inline int sdr_shm_writer_write(sdr_shm_writer_t *w, const void *data, size_t n,
                                       uint64_t timeout_ns)
{
    const uint8_t *bytes = (const uint8_t *)data;
    if (n % w->sample_bytes) {
        errno = EINVAL;
        return -1;
    }
    while (n > 0) {
        size_t piece = n < w->ring_bytes ? n : (size_t)w->ring_bytes, len1, len2;
        uint8_t *span1, *span2;
        if (sdr_shm_writer_reserve(w, piece, timeout_ns, &span1, &len1, &span2, &len2) != 0)
            return -1;
        memcpy(span1, bytes, len1);
        if (len2) memcpy(span2, bytes + len1, len2);
        sdr_shm_writer_commit(w, piece);
        bytes += piece;
        n -= piece;
    }
    return 0;
}

/*
 * Up to timeout_ns for count lockstep readers to register (before the
 * first byte, so that they read the stream from its start); returns how
 * many did
 */
static inline int sdr_shm_writer_wait_readers(const sdr_shm_writer_t *w, int count,
                                              uint64_t timeout_ns)
{
    uint64_t began = sdr_shm_monotonic_ns();
    int readers;
    while ((readers = sdr_shm_writer_readers(w)) < count &&
           sdr_shm_monotonic_ns() - began < timeout_ns)
        sdr_shm_sleep_ns(SDR_SHM_POLL_NS);
    return readers;
}

/* Samples the SDR lost (written as zeros too) */
static inline void sdr_shm_writer_add_lost(sdr_shm_writer_t *w, uint64_t samples)
{
    __atomic_fetch_add(&w->header->lost_samples, samples, __ATOMIC_RELAXED);
}

/*
 * End the stream with state SDR_SHM_ENDED (or SDR_SHM_FAILED): readers
 * drain it and stop. The name is removed if it is still this segment's
 * (not a newer writer's that replaced it)
 */
static inline void sdr_shm_writer_close(sdr_shm_writer_t *w, uint32_t state)
{
    struct stat st;
    if (w->header) {
        __atomic_store_n(&w->header->state, state, __ATOMIC_RELEASE);
        munmap((void *)w->header, w->map_bytes);
        w->header = NULL;
        if (stat(w->path, &st) == 0 && st.st_dev == w->dev && st.st_ino == w->ino)
            unlink(w->path);
    }
    if (w->fd >= 0) close(w->fd);
    w->fd = -1;
}

#endif /* RXSHM_SDR_SHM_H */
