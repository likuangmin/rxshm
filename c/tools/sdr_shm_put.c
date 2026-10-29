/*
 * Publish stdin as an SDR shared memory stream until its end: an example of
 * the writer in rxshm/sdr_shm.h, and a way to publish a recording or
 * another program's output. Its samples are sample_bytes each; --tag gives
 * the tag text describing them (a Pocket SDR <file>.tag). With --lockstep
 * it waits for the readers registered with it rather than overwrite what
 * they haven't read, first up to --wait seconds for --readers of them.
 * --replace takes the name even from a running writer.
 * Prints the segment's name on stderr when created, and what it wrote at
 * the end:
 *
 *   gcc -O2 -Ic/include -o sdr_shm_put c/tools/sdr_shm_put.c
 *   sdr_shm_put myreplay-0 --sample-bytes 2 --tag rec.bin.tag < rec.bin
 *
 *   SHM myreplay-0
 *   DONE bytes=1048576 evicted=0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rxshm/sdr_shm.h>

static volatile sig_atomic_t stopped;

static void stop(int number)
{
    (void)number;
    stopped = 1;
}

static int usage(void)
{
    fprintf(stderr, "usage: sdr_shm_put <segment name> --sample-bytes N [--ring BYTES] "
                    "[--tag FILE] [--lockstep] [--readers N] [--wait SECONDS] [--replace]\n");
    return 2;
}

int main(int argc, char **argv)
{
    static uint8_t buf[1 << 20];
    static char tag[1024];
    unsigned long long ring = 1 << 22;
    unsigned sample_bytes = 0, flags = 0;
    int readers = 0;
    double wait = 5.0;
    size_t held = 0;
    sdr_shm_writer_t w;
    struct sigaction action;

    if (argc < 2 || argv[1][0] == '-') return usage();
    for (int i = 2; i < argc; i++) {
        const char *value = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(argv[i], "--lockstep")) {
            flags |= SDR_SHM_FLAG_LOCKSTEP;
            continue;
        }
        if (!strcmp(argv[i], "--replace")) {
            flags |= SDR_SHM_REPLACE;
            continue;
        }
        if (!value) return usage();
        if (!strcmp(argv[i], "--sample-bytes")) {
            sample_bytes = (unsigned)strtoul(value, NULL, 0);
        } else if (!strcmp(argv[i], "--ring")) {
            ring = strtoull(value, NULL, 0);
        } else if (!strcmp(argv[i], "--readers")) {
            readers = atoi(value);
        } else if (!strcmp(argv[i], "--wait")) {
            wait = atof(value);
        } else if (!strcmp(argv[i], "--tag")) {
            FILE *f = fopen(value, "r");
            if (!f) {
                perror(value);
                return 1;
            }
            tag[fread(tag, 1, sizeof(tag) - 1, f)] = '\0';
            fclose(f);
        } else {
            return usage();
        }
        i++;
    }
    if (sample_bytes == 0) return usage();
    if (sdr_shm_writer_create(&w, argv[1], ring, sample_bytes, tag, "", 0, flags, 0) != 0) {
        perror(argv[1]);
        return 1;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    fprintf(stderr, "SHM %s\n", argv[1]);
    if (readers > 0) {
        int count = sdr_shm_writer_wait_readers(&w, readers, (uint64_t)(wait * 1e9));
        fprintf(stderr, "READERS %d of %d\n", count, readers);
    }
    while (!stopped) {
        ssize_t got = read(STDIN_FILENO, buf + held, sizeof(buf) - held);
        if (got <= 0) {
            if (got < 0 && errno == EINTR) continue;
            break;
        }
        held += (size_t)got;
        size_t whole = held - held % sample_bytes;
        if (sdr_shm_writer_write(&w, buf, whole, SDR_SHM_FOREVER) != 0) {
            perror("sdr_shm_put: write");
            sdr_shm_writer_close(&w, SDR_SHM_FAILED);
            return 1;
        }
        memmove(buf, buf + whole, held - whole);
        held -= whole;
    }
    sdr_shm_writer_close(&w, SDR_SHM_ENDED);
    fprintf(stderr, "DONE bytes=%llu evicted=%u\n", (unsigned long long)w.written, w.evicted);
    return 0;
}
