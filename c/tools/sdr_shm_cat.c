/*
 * Write an SDR shared memory stream to stdout until the capture ends: an
 * example of the reader in rxshm/sdr_shm.h, and a way to pipe a live stream into a
 * program that reads IF data from a file or a pipe (e.g. pocket_trk).
 * Lost bytes (the reader fell behind) are reported on stderr. With
 * --lockstep it registers with a lockstep writer (a replay), which then
 * waits for it rather than lose bytes.
 *
 *   gcc -O2 -Ic/include -o sdr_shm_cat c/tools/sdr_shm_cat.c
 *   sdr_shm_cat rxbenchmark-bladeRF-0 [--oldest] [--lockstep] > if.bin
 */

#include <stdio.h>
#include <string.h>

#include <rxshm/sdr_shm.h>

int main(int argc, char **argv)
{
    static uint8_t buf[1 << 20];
    sdr_shm_t shm;
    uint64_t offset, skipped, first = 0;
    int started = 0, oldest = 0, lockstep = 0, evicted = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: sdr_shm_cat <segment name> [--oldest] [--lockstep]\n");
        return 2;
    }
    for (int i = 2; i < argc; i++) {
        oldest |= !strcmp(argv[i], "--oldest");
        lockstep |= !strcmp(argv[i], "--lockstep");
    }
    if (sdr_shm_open(&shm, argv[1], oldest) != 0) {
        perror(argv[1]);
        return 1;
    }
    if (lockstep && sdr_shm_register(&shm) != 0) {
        perror("sdr_shm_cat: register");
        sdr_shm_close(&shm);
        return 1;
    }
    for (;;) {
        size_t n = sdr_shm_read(&shm, buf, sizeof(buf), &offset, &skipped);
        if (sdr_shm_evicted(&shm) && !evicted) {
            evicted = 1;
            fprintf(stderr, "sdr_shm_cat: evicted by the writer, going on lossy\n");
        }
        if (skipped) {
            fprintf(stderr, "sdr_shm_cat: %llu bytes lost at %llu\n",
                    (unsigned long long)skipped, (unsigned long long)(offset - skipped));
        }
        if (n == 0) {
            if (sdr_shm_ended(&shm)) break;
            sdr_shm_sleep_ns(1000000);
            continue;
        }
        if (!started) {
            first = offset;
            started = 1;
            fprintf(stderr, "sdr_shm_cat: from sample %llu\n",
                    (unsigned long long)(first / shm.sample_bytes));
        }
        if (fwrite(buf, 1, n, stdout) != n) break;
    }
    sdr_shm_close(&shm);
    return 0;
}
