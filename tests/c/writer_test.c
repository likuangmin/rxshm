/*
 * The C writer's API: names taken only from stale writers unless replacing,
 * close removing only its own segment, spans where the ring wraps, sizes
 * refused, and lockstep timeouts. Run by tests/test_c.py with a segment
 * name prefix; prints what failed, exits 0 if nothing did.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rxshm/sdr_shm.h>

static int failures;

#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                \
        }                                                              \
    } while (0)

static int exists(const char *name)
{
    char path[256];
    struct stat st;
    sdr_shm_path(path, sizeof(path), name);
    return stat(path, &st) == 0;
}

static void test_names(const char *name)
{
    sdr_shm_writer_t a, b, c;
    sdr_shm_t r;
    CHECK(sdr_shm_writer_create(&a, name, 4096, 8, "FMT  = CS16\n", "/x.bin", 5, 0, 0) == 0);
    /* A running writer's name: refused, unless replacing */
    CHECK(sdr_shm_writer_create(&c, name, 4096, 8, "", NULL, 0, 0, 0) == -1 && errno == EEXIST);
    CHECK(sdr_shm_writer_create(&b, name, 8192, 8, "", NULL, 0, SDR_SHM_REPLACE, 0) == 0);
    /* A closing doesn't remove B's segment */
    sdr_shm_writer_close(&a, SDR_SHM_ENDED);
    CHECK(exists(name));
    CHECK(sdr_shm_open(&r, name, 0) == 0 && r.ring_bytes == 8192);
    sdr_shm_close(&r);
    sdr_shm_writer_close(&b, SDR_SHM_ENDED);
    CHECK(!exists(name));
    /* A stale segment (state still writing, its writer gone) is replaced */
    CHECK(sdr_shm_writer_create(&a, name, 4096, 8, "", NULL, 0, 0, 0) == 0);
    a.header->writer_pid = 0x7ffffffe;
    CHECK(sdr_shm_writer_create(&b, name, 4096, 8, "", NULL, 0, 0, 0) == 0);
    sdr_shm_writer_close(&a, SDR_SHM_ENDED);
    sdr_shm_writer_close(&b, SDR_SHM_ENDED);
    CHECK(sdr_shm_writer_create(&a, name, 4000, 8, "", NULL, 0, 0, 0) == -1 && errno == EINVAL);
    CHECK(sdr_shm_writer_create(&a, name, 4096, 3, "", NULL, 0, 0, 0) == -1 && errno == EINVAL);
}

static void test_spans(const char *name)
{
    sdr_shm_writer_t w;
    sdr_shm_t r;
    uint8_t *s1, *s2, buf[8192];
    size_t n1, n2, n;
    uint64_t offset, skipped;
    CHECK(sdr_shm_writer_create(&w, name, 4096, 8, "", NULL, 0, 0, 0) == 0);
    CHECK(sdr_shm_open(&r, name, 1) == 0);
    CHECK(sdr_shm_writer_reserve(&w, 12, 0, &s1, &n1, &s2, &n2) == -1 && errno == EINVAL);
    CHECK(sdr_shm_writer_reserve(&w, 8192, 0, &s1, &n1, &s2, &n2) == -1 && errno == EINVAL);
    CHECK(sdr_shm_writer_reserve(&w, 3000, 0, &s1, &n1, &s2, &n2) == 0);
    CHECK(n1 == 3000 && s2 == NULL && n2 == 0);
    memset(s1, 1, n1);
    CHECK(sdr_shm_writer_commit(&w, 3008) == -1 && errno == EINVAL);
    CHECK(sdr_shm_writer_commit(&w, 3000) == 0);
    n = sdr_shm_read(&r, buf, sizeof(buf), &offset, &skipped);
    CHECK(n == 3000 && offset == 0 && skipped == 0 && buf[2999] == 1);
    /* Wraps: two spans */
    CHECK(sdr_shm_writer_reserve(&w, 2000, 0, &s1, &n1, &s2, &n2) == 0);
    CHECK(n1 == 1096 && s2 == w.ring && n2 == 904);
    CHECK(w.header->write_end == 5000 && w.header->write_count == 3000);
    memset(s1, 2, n1);
    memset(s2, 3, n2);
    CHECK(sdr_shm_writer_commit(&w, 2000) == 0);
    n = sdr_shm_read(&r, buf, sizeof(buf), &offset, &skipped);
    CHECK(n == 2000 && offset == 3000 && buf[0] == 2 && buf[1999] == 3);
    sdr_shm_writer_add_lost(&w, 7);
    CHECK(r.header->lost_samples == 7);
    sdr_shm_writer_close(&w, SDR_SHM_FAILED);
    CHECK(sdr_shm_state(&r) == SDR_SHM_FAILED && sdr_shm_ended(&r));
    sdr_shm_close(&r);
}

static void test_lockstep(const char *name)
{
    static uint8_t data[8192];
    sdr_shm_writer_t w;
    sdr_shm_t r;
    uint8_t buf[4096];
    uint64_t offset, skipped;
    CHECK(sdr_shm_writer_create(&w, name, 4096, 8, "", NULL, 0, SDR_SHM_FLAG_LOCKSTEP,
                                60000000000u) == 0);
    CHECK(sdr_shm_writer_wait_readers(&w, 1, 1000000) == 0);
    CHECK(sdr_shm_open(&r, name, 1) == 0 && sdr_shm_register(&r) == 0);
    CHECK(sdr_shm_writer_readers(&w) == 1);
    CHECK(sdr_shm_writer_write(&w, data, 4096, 0) == 0);
    /* The reader hasn't read: no room, nothing reserved or written */
    uint64_t began = sdr_shm_monotonic_ns();
    CHECK(sdr_shm_writer_write(&w, data, 8, 50000000) == -1 && errno == ETIMEDOUT);
    CHECK(sdr_shm_monotonic_ns() - began >= 50000000);
    CHECK(w.written == 4096 && w.header->write_count == 4096);
    CHECK(sdr_shm_read(&r, buf, 1024, &offset, &skipped) == 1024 && skipped == 0);
    CHECK(sdr_shm_writer_write(&w, data, 1024, 0) == 0);
    CHECK(sdr_shm_writer_write(&w, data, 8, 0) == -1 && errno == ETIMEDOUT);
    /* Closed: no longer waited for */
    sdr_shm_close(&r);
    CHECK(sdr_shm_writer_readers(&w) == 0);
    CHECK(sdr_shm_writer_write(&w, data, sizeof(data), 0) == 0 && w.evicted == 0);
    sdr_shm_writer_close(&w, SDR_SHM_ENDED);
    /* Not lockstep: registering refused */
    CHECK(sdr_shm_writer_create(&w, name, 4096, 8, "", NULL, 0, 0, 0) == 0);
    CHECK(sdr_shm_open(&r, name, 0) == 0);
    CHECK(sdr_shm_register(&r) == -1 && errno == ENOTSUP);
    CHECK(sdr_shm_writer_readers(&w) == 0);
    sdr_shm_close(&r);
    sdr_shm_writer_close(&w, SDR_SHM_ENDED);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: writer_test <segment name>\n");
        return 2;
    }
    test_names(argv[1]);
    test_spans(argv[1]);
    test_lockstep(argv[1]);
    return failures ? 1 : 0;
}
