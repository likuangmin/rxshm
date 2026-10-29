// The header in a C++20 program built with RealSuite's warnings
// (-Wold-style-cast -Wshadow -Werror): quiet through the CMake target's
// SYSTEM include directory. Writes and reads back one sample.

#include <cstdio>
#include <string>

#include <rxshm/sdr_shm.h>

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    const std::string name = argv[1];
    sdr_shm_writer_t w;
    sdr_shm_t r;
    const uint64_t sample = 0x0123456789abcdefu;
    uint64_t got = 0, offset = 0, skipped = 0;
    if (sdr_shm_writer_create(&w, name.c_str(), 4096, 8, "", nullptr, 0, 0, 0) != 0) return 1;
    if (sdr_shm_open(&r, name.c_str(), 1) != 0) return 1;
    sdr_shm_writer_write(&w, &sample, sizeof(sample), SDR_SHM_FOREVER);
    const size_t n = sdr_shm_read(&r, reinterpret_cast<uint8_t *>(&got), sizeof(got), &offset,
                                  &skipped);
    sdr_shm_close(&r);
    sdr_shm_writer_close(&w, SDR_SHM_ENDED);
    std::printf("%zu %llx\n", n, static_cast<unsigned long long>(got));
    return n == sizeof(got) && got == sample ? 0 : 1;
}
