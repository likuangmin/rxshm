/*
 * The header's and a slot's field offsets as the C structs lay them out,
 * one "name offset" a line: tests/test_c.py checks them against the Python
 * constants.
 */

#include <stdio.h>

#include <rxshm/sdr_shm.h>

#define FIELD(type, name) printf("%s %u\n", #name, (unsigned)offsetof(type, name))

int main(void)
{
    FIELD(sdr_shm_header_t, ring_bytes);
    FIELD(sdr_shm_header_t, sample_bytes);
    FIELD(sdr_shm_header_t, state);
    FIELD(sdr_shm_header_t, write_count);
    FIELD(sdr_shm_header_t, heartbeat_ns);
    FIELD(sdr_shm_header_t, start_ns);
    FIELD(sdr_shm_header_t, lost_samples);
    FIELD(sdr_shm_header_t, session);
    FIELD(sdr_shm_header_t, writer_pid);
    FIELD(sdr_shm_header_t, flags);
    FIELD(sdr_shm_header_t, slot_offset);
    FIELD(sdr_shm_header_t, slot_count);
    FIELD(sdr_shm_header_t, slot_bytes);
    FIELD(sdr_shm_header_t, lockstep_timeout_ns);
    FIELD(sdr_shm_header_t, write_end);
    FIELD(sdr_shm_header_t, tag);
    FIELD(sdr_shm_header_t, data_path);
    FIELD(sdr_shm_slot_t, state);
    FIELD(sdr_shm_slot_t, pid);
    FIELD(sdr_shm_slot_t, pos);
    FIELD(sdr_shm_slot_t, heartbeat_ns);
    FIELD(sdr_shm_slot_t, session);
    printf("slot_size %u\n", (unsigned)sizeof(sdr_shm_slot_t));
    return 0;
}
